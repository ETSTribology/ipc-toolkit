// Modified version of SpatialHash.hpp from IPC codebase.
// Originally created by Minchen Li.
#include <ipc/spatial_hash/spatial_hash.hpp>

#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>

#include <ipc/ccd/broadphase.hpp>
#include <ipc/spatial_hash/hash_grid.hpp>

// Uncomment this to construct spatial hash in parallel.
// #define IPC_TOOLKIT_PARALLEL_SH_CONSTRUCT

namespace ipc {

double suggestGoodVoxelSize(
    const Eigen::MatrixXd& V,
    const Eigen::MatrixXi& E,
    const Eigen::MatrixXi& F,
    double inflation_radius = 0)
{
    double edge_len = average_edge_length(V, V, E);
    return 2 * edge_len + inflation_radius;
}

double suggestGoodVoxelSize(
    const Eigen::MatrixXd& V0,
    const Eigen::MatrixXd& V1,
    const Eigen::MatrixXi& E,
    const Eigen::MatrixXi& F,
    double inflation_radius = 0)
{
    double edge_len = average_edge_length(V0, V1, E);
    double disp_len = average_displacement_length(V1 - V0);
    return 2 * std::max(edge_len, disp_len) + inflation_radius;
}

void SpatialHash::build(
    const Eigen::MatrixXd& V,
    const Eigen::MatrixXi& E,
    const Eigen::MatrixXi& F,
    double voxelSize)
{
    clear();
    dim = V.cols();

    if (voxelSize <= 0) {
        voxelSize = suggestGoodVoxelSize(V, E, F);
    }

    leftBottomCorner = V.colwise().minCoeff();
    rightTopCorner = V.colwise().maxCoeff();
    one_div_voxelSize = 1.0 / voxelSize;
    Eigen::ArrayMax3d range = rightTopCorner - leftBottomCorner;
    voxelCount = (range * one_div_voxelSize).ceil().template cast<int>();
    if (voxelCount.minCoeff() <= 0) {
        // cast overflow due to huge search direction
        one_div_voxelSize = 1.0 / (range.maxCoeff() * 1.01);
        voxelCount.setOnes();
    }
    voxelCount0x1 = voxelCount[0] * voxelCount[1];

    edgeStartInd = V.rows();
    triStartInd = edgeStartInd + E.rows();

    // precompute svVAI
    std::vector<Eigen::ArrayMax3i> vertexVoxelAxisIndex(V.rows());
    tbb::parallel_for(size_t(0), size_t(V.rows()), [&](size_t vi) {
        locateVoxelAxisIndex(V.row(vi), vertexVoxelAxisIndex[vi]);
    });

    voxel.clear();

#ifdef IPC_TOOLKIT_PARALLEL_SH_CONSTRUCT
    std::vector<std::pair<int, int>> voxel_tmp;

    for (int vi = 0; vi < V.rows(); vi++) {
        voxel_tmp.emplace_back(locateVoxelIndex(V.row(vi)), vi);
    }
#else
    for (int vi = 0; vi < V.rows(); vi++) {
        voxel[locateVoxelIndex(V.row(vi))].emplace_back(vi);
    }
#endif

    std::vector<std::vector<int>> voxelLoc_e(E.rows());
    tbb::parallel_for(size_t(0), size_t(E.rows()), [&](size_t ei) {
        const Eigen::ArrayMax3i& voxelAxisIndex0 =
            vertexVoxelAxisIndex[E(ei, 0)];
        const Eigen::ArrayMax3i& voxelAxisIndex1 =
            vertexVoxelAxisIndex[E(ei, 1)];

        Eigen::ArrayMax3i mins = voxelAxisIndex0.min(voxelAxisIndex1);
        Eigen::ArrayMax3i maxs = voxelAxisIndex0.max(voxelAxisIndex1);

        for (int iz = mins[2]; iz <= maxs[2]; iz++) {
            int zOffset = iz * voxelCount0x1;
            for (int iy = mins[1]; iy <= maxs[1]; iy++) {
                int yzOffset = iy * voxelCount[0] + zOffset;
                for (int ix = mins[0]; ix <= maxs[0]; ix++) {
                    voxelLoc_e[ei].emplace_back(ix + yzOffset);
                }
            }
        }
    });

    std::vector<std::vector<int>> voxelLoc_f(F.rows());
    tbb::parallel_for(size_t(0), size_t(F.rows()), [&](size_t fi) {
        const Eigen::ArrayMax3i& voxelAxisIndex0 =
            vertexVoxelAxisIndex[F(fi, 0)];
        const Eigen::ArrayMax3i& voxelAxisIndex1 =
            vertexVoxelAxisIndex[F(fi, 1)];
        const Eigen::ArrayMax3i& voxelAxisIndex2 =
            vertexVoxelAxisIndex[F(fi, 1)];

        Eigen::ArrayMax3i mins =
            voxelAxisIndex0.min(voxelAxisIndex1).min(voxelAxisIndex2);
        Eigen::ArrayMax3i maxs =
            voxelAxisIndex0.max(voxelAxisIndex1).max(voxelAxisIndex2);

        for (int iz = mins[2]; iz <= maxs[2]; iz++) {
            int zOffset = iz * voxelCount0x1;
            for (int iy = mins[1]; iy <= maxs[1]; iy++) {
                int yzOffset = iy * voxelCount[0] + zOffset;
                for (int ix = mins[0]; ix <= maxs[0]; ix++) {
                    voxelLoc_f[fi].emplace_back(ix + yzOffset);
                }
            }
        }
    });

#ifdef IPC_TOOLKIT_PARALLEL_SH_CONSTRUCT
    for (int ei = 0; ei < voxelLoc_e.size(); ei++) {
        for (const auto& voxelI : voxelLoc_e[ei]) {
            voxel_tmp.emplace_back(voxelI, ei + edgeStartInd);
        }
    }

    for (int fi = 0; fi < voxelLoc_f.size(); fi++) {
        for (const auto& voxelI : voxelLoc_f[fi]) {
            voxel_tmp.emplace_back(voxelI, fi + triStartInd);
        }
    }

    // Sort the voxels based on the voxel indices
    tbb::parallel_sort(
        voxel_tmp.begin(), voxel_tmp.end(),
        [](const std::pair<int, int>& f, const std::pair<int, int>& s) {
            return f.first < s.first;
        });

    std::vector<std::pair<int, std::vector<int>>> voxel_tmp_merged;
    voxel_tmp_merged.reserve(voxel_tmp.size());
    int current_voxel = -1;
    for (const auto& v : voxel_tmp) {
        if (current_voxel != v.first) {
            assert(current_voxel < v.first);
            voxel_tmp_merged.emplace_back();
            voxel_tmp_merged.back().first = v.first;
            current_voxel = v.first;
        }

        voxel_tmp_merged.back().second.push_back(v.second);
    }
    assert(voxel_tmp_merged.size() <= voxel_tmp.size());

    voxel.insert(voxel_tmp_merged.begin(), voxel_tmp_merged.end());
#else
    for (int ei = 0; ei < voxelLoc_e.size(); ei++) {
        for (const auto& voxelI : voxelLoc_e[ei]) {
            voxel[voxelI].emplace_back(ei + edgeStartInd);
        }
    }
    for (int fi = 0; fi < voxelLoc_f.size(); fi++) {
        for (const auto& voxelI : voxelLoc_f[fi]) {
            voxel[voxelI].emplace_back(fi + triStartInd);
        }
    }
#endif
}

void SpatialHash::queryPointForTriangles(
    const Eigen::VectorX3d& p,
    std::unordered_set<int>& triInds,
    double radius) const
{
    Eigen::ArrayMax3i mins, maxs;
    locateVoxelAxisIndex(p.array() - radius, mins);
    locateVoxelAxisIndex(p.array() + radius, maxs);
    mins = mins.max(Eigen::ArrayMax3i::Zero(dim));
    maxs = maxs.min(voxelCount - 1);

    triInds.clear();
    for (int iz = mins[2]; iz <= maxs[2]; iz++) {
        int zOffset = iz * voxelCount0x1;
        for (int iy = mins[1]; iy <= maxs[1]; iy++) {
            int yzOffset = iy * voxelCount[0] + zOffset;
            for (int ix = mins[0]; ix <= maxs[0]; ix++) {
                const auto voxelI = voxel.find(ix + yzOffset);
                if (voxelI != voxel.end()) {
                    for (const auto& indI : voxelI->second) {
                        if (indI >= triStartInd) {
                            triInds.insert(indI - triStartInd);
                        }
                    }
                }
            }
        }
    }
}

void SpatialHash::queryPointForTriangles(
    const Eigen::VectorX3d& p_t0,
    const Eigen::VectorX3d& p_t1,
    std::unordered_set<int>& triInds,
    double radius) const
{
    Eigen::ArrayMax3i mins, maxs;
    locateVoxelAxisIndex(p_t0.array().min((p_t1).array()) - radius, mins);
    locateVoxelAxisIndex(p_t0.array().max((p_t1).array()) + radius, maxs);
    mins = mins.max(Eigen::ArrayMax3i::Zero(dim));
    maxs = maxs.min(voxelCount - 1);

    triInds.clear();
    for (int iz = mins[2]; iz <= maxs[2]; iz++) {
        int zOffset = iz * voxelCount0x1;
        for (int iy = mins[1]; iy <= maxs[1]; iy++) {
            int yzOffset = iy * voxelCount[0] + zOffset;
            for (int ix = mins[0]; ix <= maxs[0]; ix++) {
                const auto voxelI = voxel.find(ix + yzOffset);
                if (voxelI != voxel.end()) {
                    for (const auto& indI : voxelI->second) {
                        if (indI >= triStartInd) {
                            triInds.insert(indI - triStartInd);
                        }
                    }
                }
            }
        }
    }
}

void SpatialHash::queryPointForPrimitives(
    const Eigen::VectorX3d& p_t0,
    const Eigen::VectorX3d& p_t1,
    std::unordered_set<int>& vertInds,
    std::unordered_set<int>& edgeInds,
    std::unordered_set<int>& triInds) const
{
    Eigen::ArrayMax3i mins, maxs;
    locateVoxelAxisIndex(p_t0.array().min((p_t1).array()), mins);
    locateVoxelAxisIndex(p_t0.array().max((p_t1).array()), maxs);
    mins = mins.max(Eigen::ArrayMax3i::Zero(dim));
    maxs = maxs.min(voxelCount - 1);

    vertInds.clear();
    edgeInds.clear();
    triInds.clear();
    for (int iz = mins[2]; iz <= maxs[2]; iz++) {
        int zOffset = iz * voxelCount0x1;
        for (int iy = mins[1]; iy <= maxs[1]; iy++) {
            int yzOffset = iy * voxelCount[0] + zOffset;
            for (int ix = mins[0]; ix <= maxs[0]; ix++) {
                const auto voxelI = voxel.find(ix + yzOffset);
                if (voxelI != voxel.end()) {
                    for (const auto& indI : voxelI->second) {
                        if (indI < edgeStartInd) {
                            vertInds.insert(indI);
                        } else if (indI < triStartInd) {
                            edgeInds.insert(indI - edgeStartInd);
                        } else {
                            triInds.insert(indI - triStartInd);
                        }
                    }
                }
            }
        }
    }
}

void SpatialHash::queryEdgeForPE(
    const Eigen::VectorX3d& e0,
    const Eigen::VectorX3d& e1,
    std::vector<int>& vertInds,
    std::vector<int>& edgeInds) const
{
    Eigen::VectorX3d leftBottom = e0.array().min(e1.array());
    Eigen::VectorX3d rightTop = e0.array().max(e1.array());
    Eigen::ArrayMax3i mins, maxs;
    locateVoxelAxisIndex(leftBottom, mins);
    locateVoxelAxisIndex(rightTop, maxs);
    mins = mins.max(Eigen::ArrayMax3i::Zero(dim));
    maxs = maxs.min(voxelCount - 1);

    vertInds.clear();
    edgeInds.clear();
    for (int iz = mins[2]; iz <= maxs[2]; iz++) {
        int zOffset = iz * voxelCount0x1;
        for (int iy = mins[1]; iy <= maxs[1]; iy++) {
            int yzOffset = iy * voxelCount[0] + zOffset;
            for (int ix = mins[0]; ix <= maxs[0]; ix++) {
                const auto voxelI = voxel.find(ix + yzOffset);
                if (voxelI != voxel.end()) {
                    for (const auto& indI : voxelI->second) {
                        if (indI < edgeStartInd) {
                            vertInds.emplace_back(indI);
                        } else if (indI < triStartInd) {
                            edgeInds.emplace_back(indI - edgeStartInd);
                        }
                    }
                }
            }
        }
    }
    std::sort(edgeInds.begin(), edgeInds.end());
    edgeInds.erase(
        std::unique(edgeInds.begin(), edgeInds.end()), edgeInds.end());
    std::sort(vertInds.begin(), vertInds.end());
    vertInds.erase(
        std::unique(vertInds.begin(), vertInds.end()), vertInds.end());
}

void SpatialHash::queryEdgeForEdges(
    const Eigen::VectorX3d& e0,
    const Eigen::VectorX3d& e1,
    std::vector<int>& edgeInds,
    double radius,
    int eai) const
{
    Eigen::VectorX3d leftBottom = e0.array().min(e1.array()) - radius;
    Eigen::VectorX3d rightTop = e0.array().max(e1.array()) + radius;
    Eigen::ArrayMax3i mins, maxs;
    locateVoxelAxisIndex(leftBottom, mins);
    locateVoxelAxisIndex(rightTop, maxs);
    mins = mins.max(Eigen::ArrayMax3i::Zero(dim));
    maxs = maxs.min(voxelCount - 1);

    edgeInds.clear();
    for (int iz = mins[2]; iz <= maxs[2]; iz++) {
        int zOffset = iz * voxelCount0x1;
        for (int iy = mins[1]; iy <= maxs[1]; iy++) {
            int yzOffset = iy * voxelCount[0] + zOffset;
            for (int ix = mins[0]; ix <= maxs[0]; ix++) {

                const auto voxelI = voxel.find(ix + yzOffset);
                if (voxelI != voxel.end()) {
                    for (const auto& indI : voxelI->second) {
                        if (indI >= edgeStartInd && indI < triStartInd
                            && indI - edgeStartInd > eai) {
                            edgeInds.emplace_back(indI - edgeStartInd);
                        }
                    }
                }
            }
        }
    }

    std::sort(edgeInds.begin(), edgeInds.end());
    edgeInds.erase(
        std::unique(edgeInds.begin(), edgeInds.end()), edgeInds.end());
}

void SpatialHash::queryEdgeForEdgesWithBBoxCheck(
    const Eigen::MatrixXd& V,
    const Eigen::MatrixXi& E,
    const Eigen::MatrixXi& F,
    const Eigen::VectorX3d& ea0,
    const Eigen::VectorX3d& ea1,
    std::vector<int>& edgeInds,
    double radius,
    int eai) const
{
    Eigen::VectorX3d leftBottom = ea0.array().min(ea1.array()) - radius;
    Eigen::VectorX3d rightTop = ea0.array().max(ea1.array()) + radius;
    Eigen::ArrayMax3i mins, maxs;
    locateVoxelAxisIndex(leftBottom, mins);
    locateVoxelAxisIndex(rightTop, maxs);
    mins = mins.max(Eigen::ArrayMax3i::Zero(dim));
    maxs = maxs.min(voxelCount - 1);

    edgeInds.clear();
    for (int iz = mins[2]; iz <= maxs[2]; iz++) {
        int zOffset = iz * voxelCount0x1;
        for (int iy = mins[1]; iy <= maxs[1]; iy++) {
            int yzOffset = iy * voxelCount[0] + zOffset;
            for (int ix = mins[0]; ix <= maxs[0]; ix++) {

                const auto voxelI = voxel.find(ix + yzOffset);
                if (voxelI != voxel.end()) {
                    for (const auto& indI : voxelI->second) {
                        if (indI >= edgeStartInd && indI < triStartInd
                            && indI - edgeStartInd > eai) {
                            int ebi = indI - edgeStartInd;
                            const Eigen::VectorX3d& eb0 = V.row(E(ebi, 0));
                            const Eigen::VectorX3d& eb1 = V.row(E(ebi, 1));
                            Eigen::ArrayMax3d bboxEBTopRight =
                                eb0.array().max(eb1.array());
                            Eigen::ArrayMax3d bboxEBBottomLeft =
                                eb0.array().min(eb1.array());
                            if (!((bboxEBBottomLeft - rightTop.array() > 0.0)
                                      .any()
                                  || (leftBottom.array() - bboxEBTopRight > 0.0)
                                         .any())) {
                                edgeInds.emplace_back(indI - edgeStartInd);
                            }
                        }
                    }
                }
            }
        }
    }
    std::sort(edgeInds.begin(), edgeInds.end());
    edgeInds.erase(
        std::unique(edgeInds.begin(), edgeInds.end()), edgeInds.end());
}

void SpatialHash::queryEdgeForEdges(
    const Eigen::VectorX3d& ea0_t0,
    const Eigen::VectorX3d& ea1_t0,
    const Eigen::VectorX3d& ea0_t1,
    const Eigen::VectorX3d& ea1_t1,
    std::vector<int>& edgeInds,
    double radius,
    int eai) const
{
    Eigen::VectorX3d leftBottom = ea0_t0.array()
                                      .min(ea1_t0.array())
                                      .min(ea0_t1.array())
                                      .min(ea1_t1.array());
    Eigen::VectorX3d rightTop = ea0_t0.array()
                                    .max(ea1_t0.array())
                                    .max(ea0_t1.array())
                                    .max(ea1_t1.array());
    Eigen::ArrayMax3i mins, maxs;
    locateVoxelAxisIndex(leftBottom.array() - radius, mins);
    locateVoxelAxisIndex(rightTop.array() + radius, maxs);
    mins = mins.max(Eigen::ArrayMax3i::Zero(dim));
    maxs = maxs.min(voxelCount - 1);

    edgeInds.clear();
    for (int iz = mins[2]; iz <= maxs[2]; iz++) {
        int zOffset = iz * voxelCount0x1;
        for (int iy = mins[1]; iy <= maxs[1]; iy++) {
            int yzOffset = iy * voxelCount[0] + zOffset;
            for (int ix = mins[0]; ix <= maxs[0]; ix++) {
                const auto voxelI = voxel.find(ix + yzOffset);
                if (voxelI != voxel.end()) {
                    for (const auto& indI : voxelI->second) {
                        if (indI >= edgeStartInd && indI < triStartInd
                            && indI - edgeStartInd > eai) {
                            edgeInds.emplace_back(indI - edgeStartInd);
                        }
                    }
                }
            }
        }
    }
    std::sort(edgeInds.begin(), edgeInds.end());
    edgeInds.erase(
        std::unique(edgeInds.begin(), edgeInds.end()), edgeInds.end());
}

void SpatialHash::queryTriangleForPoints(
    const Eigen::VectorX3d& t0,
    const Eigen::VectorX3d& t1,
    const Eigen::VectorX3d& t2,
    std::unordered_set<int>& pointInds,
    double radius) const
{
    Eigen::VectorX3d leftBottom = t0.array().min(t1.array()).min(t2.array());
    Eigen::VectorX3d rightTop = t0.array().max(t1.array()).max(t2.array());
    Eigen::ArrayMax3i mins, maxs;
    locateVoxelAxisIndex(leftBottom.array() - radius, mins);
    locateVoxelAxisIndex(rightTop.array() + radius, maxs);
    mins = mins.max(Eigen::ArrayMax3i::Zero(dim));
    maxs = maxs.min(voxelCount - 1);

    pointInds.clear();
    for (int iz = mins[2]; iz <= maxs[2]; iz++) {
        int zOffset = iz * voxelCount0x1;
        for (int iy = mins[1]; iy <= maxs[1]; iy++) {
            int yzOffset = iy * voxelCount[0] + zOffset;
            for (int ix = mins[0]; ix <= maxs[0]; ix++) {
                const auto voxelI = voxel.find(ix + yzOffset);
                if (voxelI != voxel.end()) {
                    for (const auto& indI : voxelI->second) {
                        if (indI < edgeStartInd) {
                            pointInds.insert(indI);
                        }
                    }
                }
            }
        }
    }
}

void SpatialHash::queryTriangleForPoints(
    const Eigen::VectorX3d& t0_t0,
    const Eigen::VectorX3d& t1_t0,
    const Eigen::VectorX3d& t2_t0,
    const Eigen::VectorX3d& t0_t1,
    const Eigen::VectorX3d& t1_t1,
    const Eigen::VectorX3d& t2_t1,
    std::unordered_set<int>& pointInds) const
{
    Eigen::VectorX3d leftBottom = t0_t0.array()
                                      .min(t1_t0.array())
                                      .min(t2_t0.array())
                                      .min(t0_t1.array())
                                      .min(t1_t1.array())
                                      .min(t2_t1.array());
    Eigen::VectorX3d rightTop = t0_t0.array()
                                    .max(t1_t0.array())
                                    .max(t2_t0.array())
                                    .max(t0_t1.array())
                                    .max(t1_t1.array())
                                    .max(t2_t1.array());
    Eigen::ArrayMax3i mins, maxs;
    locateVoxelAxisIndex(leftBottom.array(), mins);
    locateVoxelAxisIndex(rightTop.array(), maxs);
    mins = mins.max(Eigen::ArrayMax3i::Zero(dim));
    maxs = maxs.min(voxelCount - 1);

    pointInds.clear();
    for (int iz = mins[2]; iz <= maxs[2]; iz++) {
        int zOffset = iz * voxelCount0x1;
        for (int iy = mins[1]; iy <= maxs[1]; iy++) {
            int yzOffset = iy * voxelCount[0] + zOffset;
            for (int ix = mins[0]; ix <= maxs[0]; ix++) {
                const auto voxelI = voxel.find(ix + yzOffset);
                if (voxelI != voxel.end()) {
                    for (const auto& indI : voxelI->second) {
                        if (indI < edgeStartInd) {
                            pointInds.insert(indI);
                        }
                    }
                }
            }
        }
    }
}

void SpatialHash::queryTriangleForEdges(
    const Eigen::VectorX3d& t0,
    const Eigen::VectorX3d& t1,
    const Eigen::VectorX3d& t2,
    std::unordered_set<int>& edgeInds,
    double radius) const
{
    Eigen::VectorX3d leftBottom = t0.array().min(t1.array()).min(t2.array());
    Eigen::VectorX3d rightTop = t0.array().max(t1.array()).max(t2.array());
    Eigen::ArrayMax3i mins, maxs;
    locateVoxelAxisIndex(leftBottom.array() - radius, mins);
    locateVoxelAxisIndex(rightTop.array() + radius, maxs);
    mins = mins.max(Eigen::ArrayMax3i::Zero(dim));
    maxs = maxs.min(voxelCount - 1);

    edgeInds.clear();
    for (int iz = mins[2]; iz <= maxs[2]; iz++) {
        int zOffset = iz * voxelCount0x1;
        for (int iy = mins[1]; iy <= maxs[1]; iy++) {
            int yzOffset = iy * voxelCount[0] + zOffset;
            for (int ix = mins[0]; ix <= maxs[0]; ix++) {
                const auto voxelI = voxel.find(ix + yzOffset);
                if (voxelI != voxel.end()) {
                    for (const auto& indI : voxelI->second) {
                        if (indI >= edgeStartInd && indI < triStartInd) {
                            edgeInds.insert(indI - edgeStartInd);
                        }
                    }
                }
            }
        }
    }
}

void SpatialHash::queryEdgeForTriangles(
    const Eigen::VectorX3d& e0,
    const Eigen::VectorX3d& e1,
    std::unordered_set<int>& triInds,
    double radius) const
{
    Eigen::VectorX3d leftBottom = e0.array().min(e1.array());
    Eigen::VectorX3d rightTop = e0.array().max(e1.array());
    Eigen::ArrayMax3i mins, maxs;
    locateVoxelAxisIndex(leftBottom.array() - radius, mins);
    locateVoxelAxisIndex(rightTop.array() + radius, maxs);
    mins = mins.max(Eigen::ArrayMax3i::Zero(dim));
    maxs = maxs.min(voxelCount - 1);

    triInds.clear();
    for (int iz = mins[2]; iz <= maxs[2]; iz++) {
        int zOffset = iz * voxelCount0x1;
        for (int iy = mins[1]; iy <= maxs[1]; iy++) {
            int yzOffset = iy * voxelCount[0] + zOffset;
            for (int ix = mins[0]; ix <= maxs[0]; ix++) {
                const auto voxelI = voxel.find(ix + yzOffset);
                if (voxelI != voxel.end()) {
                    for (const auto& indI : voxelI->second) {
                        if (indI >= triStartInd) {
                            triInds.insert(indI - triStartInd);
                        }
                    }
                }
            }
        }
    }
}

void SpatialHash::build(
    const Eigen::MatrixXd& V0,
    const Eigen::MatrixXd& V1,
    const Eigen::MatrixXi& E,
    const Eigen::MatrixXi& F,
    double voxelSize)
{
    assert(V0.rows() == V1.rows() && V0.cols() == V1.cols());
    clear();
    dim = V0.cols();

    if (voxelSize <= 0) {
        voxelSize = suggestGoodVoxelSize(V0, V1, E, F);
    }

    leftBottomCorner =
        V0.colwise().minCoeff().array().min(V1.colwise().minCoeff().array());
    rightTopCorner =
        V0.colwise().maxCoeff().array().max(V1.colwise().maxCoeff().array());
    one_div_voxelSize = 1.0 / voxelSize;
    Eigen::ArrayMax3d range = rightTopCorner - leftBottomCorner;
    voxelCount = (range * one_div_voxelSize).ceil().template cast<int>();
    if (voxelCount.minCoeff() <= 0) {
        // cast overflow due to huge search direction
        one_div_voxelSize = 1.0 / (range.maxCoeff() * 1.01);
        voxelCount.setOnes();
    }
    voxelCount0x1 = voxelCount[0] * voxelCount[1];

    edgeStartInd = V0.rows();
    triStartInd = edgeStartInd + E.rows();

    // precompute vVAI
    std::vector<Eigen::ArrayMax3i> vertexMinVAI(V0.rows());
    std::vector<Eigen::ArrayMax3i> vertexMaxVAI(V0.rows());
    tbb::parallel_for(size_t(0), size_t(V0.rows()), [&](size_t vi) {
        Eigen::ArrayMax3i v0VAI, v1VAI;
        locateVoxelAxisIndex(V0.row(vi), v0VAI);
        locateVoxelAxisIndex(V1.row(vi), v1VAI);
        vertexMinVAI[vi] = v0VAI.min(v1VAI);
        vertexMaxVAI[vi] = v0VAI.max(v1VAI);
    });

    voxel.clear(); // TODO: parallel insert
    pointAndEdgeOccupancy.clear();
    pointAndEdgeOccupancy.resize(triStartInd);

    tbb::parallel_for(size_t(0), size_t(V0.rows()), [&](size_t vi) {
        const Eigen::ArrayMax3i& mins = vertexMinVAI[vi];
        const Eigen::ArrayMax3i& maxs = vertexMaxVAI[vi];
        pointAndEdgeOccupancy[vi].reserve((maxs - mins + 1).prod());
        for (int iz = mins[2]; iz <= maxs[2]; iz++) {
            int zOffset = iz * voxelCount0x1;
            for (int iy = mins[1]; iy <= maxs[1]; iy++) {
                int yzOffset = iy * voxelCount[0] + zOffset;
                for (int ix = mins[0]; ix <= maxs[0]; ix++) {
                    pointAndEdgeOccupancy[vi].emplace_back(ix + yzOffset);
                }
            }
        }
    });

    tbb::parallel_for(size_t(0), size_t(E.rows()), [&](size_t ei) {
        int eiInd = ei + edgeStartInd;

        Eigen::ArrayMax3i mins =
            vertexMinVAI[E(ei, 0)].min(vertexMinVAI[E(ei, 1)]);
        Eigen::ArrayMax3i maxs =
            vertexMaxVAI[E(ei, 0)].max(vertexMaxVAI[E(ei, 1)]);

        pointAndEdgeOccupancy[eiInd].reserve((maxs - mins + 1).prod());
        for (int iz = mins[2]; iz <= maxs[2]; iz++) {
            int zOffset = iz * voxelCount0x1;
            for (int iy = mins[1]; iy <= maxs[1]; iy++) {
                int yzOffset = iy * voxelCount[0] + zOffset;
                for (int ix = mins[0]; ix <= maxs[0]; ix++) {
                    pointAndEdgeOccupancy[eiInd].emplace_back(ix + yzOffset);
                }
            }
        }
    });

    std::vector<std::vector<int>> voxelLoc_f(F.rows());
    tbb::parallel_for(size_t(0), size_t(F.rows()), [&](size_t fi) {
        Eigen::ArrayMax3i mins = vertexMinVAI[F(fi, 0)]
                                     .min(vertexMinVAI[F(fi, 1)])
                                     .min(vertexMinVAI[F(fi, 2)]);
        Eigen::ArrayMax3i maxs = vertexMaxVAI[F(fi, 0)]
                                     .max(vertexMaxVAI[F(fi, 1)])
                                     .max(vertexMaxVAI[F(fi, 2)]);

        for (int iz = mins[2]; iz <= maxs[2]; iz++) {
            int zOffset = iz * voxelCount0x1;
            for (int iy = mins[1]; iy <= maxs[1]; iy++) {
                int yzOffset = iy * voxelCount[0] + zOffset;
                for (int ix = mins[0]; ix <= maxs[0]; ix++) {
                    voxelLoc_f[fi].emplace_back(ix + yzOffset);
                }
            }
        }
    });

    for (int i = 0; i < pointAndEdgeOccupancy.size(); i++) {
        for (const auto& voxelI : pointAndEdgeOccupancy[i]) {
            voxel[voxelI].emplace_back(i);
        }
    }
    for (int fi = 0; fi < voxelLoc_f.size(); fi++) {
        for (const auto& voxelI : voxelLoc_f[fi]) {
            voxel[voxelI].emplace_back(fi + triStartInd);
        }
    }
}

void SpatialHash::queryPointForPrimitives(
    int vi,
    std::unordered_set<int>& vertInds,
    std::unordered_set<int>& edgeInds,
    std::unordered_set<int>& triInds) const
{
    vertInds.clear();
    edgeInds.clear();
    triInds.clear();
    for (const auto& voxelInd : pointAndEdgeOccupancy[vi]) {
        const auto& voxelI = voxel.find(voxelInd);
        assert(voxelI != voxel.end());
        for (const auto& indI : voxelI->second) {
            if (indI < edgeStartInd) {
                vertInds.insert(indI);
            } else if (indI < triStartInd) {
                edgeInds.insert(indI - edgeStartInd);
            } else {
                triInds.insert(indI - triStartInd);
            }
        }
    }
}

void SpatialHash::queryPointForEdges(
    int vi, std::unordered_set<int>& edgeInds) const
{
    edgeInds.clear();
    for (const auto& voxelInd : pointAndEdgeOccupancy[vi]) {
        const auto& voxelI = voxel.find(voxelInd);
        assert(voxelI != voxel.end());
        for (const auto& indI : voxelI->second) {
            if (indI >= edgeStartInd && indI < triStartInd) {
                edgeInds.insert(indI - edgeStartInd);
            }
        }
    }
}

void SpatialHash::queryPointForTriangles(
    int vi, std::unordered_set<int>& triInds) const
{
    triInds.clear();
    for (const auto& voxelInd : pointAndEdgeOccupancy[vi]) {
        const auto& voxelI = voxel.find(voxelInd);
        assert(voxelI != voxel.end());
        for (const auto& indI : voxelI->second) {
            if (indI >= triStartInd) {
                triInds.insert(indI - triStartInd);
            }
        }
    }
}

// will only put edges with larger than eai index into edgeInds
void SpatialHash::queryEdgeForEdges(
    int eai, std::unordered_set<int>& edgeInds) const
{
    edgeInds.clear();
    for (const auto& voxelInd : pointAndEdgeOccupancy[eai + edgeStartInd]) {
        const auto& voxelI = voxel.find(voxelInd);
        assert(voxelI != voxel.end());
        for (const auto& indI : voxelI->second) {
            if (indI >= edgeStartInd && indI < triStartInd
                && indI - edgeStartInd > eai) {
                edgeInds.insert(indI - edgeStartInd);
            }
        }
    }
}

void SpatialHash::queryEdgeForEdgesWithBBoxCheck(
    const Eigen::MatrixXd& V0,
    const Eigen::MatrixXd& V1,
    const Eigen::MatrixXi& E,
    const Eigen::MatrixXi& F,
    int eai,
    std::unordered_set<int>& edgeInds) const
{
    const Eigen::VectorX3d& ea0_t0 = V0.row(E(eai, 0));
    const Eigen::VectorX3d& ea1_t0 = V0.row(E(eai, 1));
    const Eigen::VectorX3d& ea0_t1 = V1.row(E(eai, 0));
    const Eigen::VectorX3d& ea1_t1 = V1.row(E(eai, 1));

    Eigen::ArrayMax3d bboxEATopRight = ea0_t0.array()
                                           .max(ea1_t0.array())
                                           .max(ea0_t1.array())
                                           .max(ea1_t1.array());
    Eigen::ArrayMax3d bboxEABottomLeft = ea0_t0.array()
                                             .min(ea1_t0.array())
                                             .min(ea0_t1.array())
                                             .min(ea1_t1.array());

    edgeInds.clear();
    for (const auto& voxelInd : pointAndEdgeOccupancy[eai + edgeStartInd]) {
        const auto& voxelI = voxel.find(voxelInd);
        assert(voxelI != voxel.end());
        for (const auto& indI : voxelI->second) {
            if (indI >= edgeStartInd && indI < triStartInd
                && indI - edgeStartInd > eai) {
                int ebi = indI - edgeStartInd;
                const Eigen::VectorX3d& eb0_t0 = V0.row(E(ebi, 0));
                const Eigen::VectorX3d& eb1_t0 = V0.row(E(ebi, 1));
                const Eigen::VectorX3d& eb0_t1 = V1.row(E(ebi, 0));
                const Eigen::VectorX3d& eb1_t1 = V1.row(E(ebi, 1));

                Eigen::ArrayMax3d bboxEBTopRight = eb0_t0.array()
                                                       .max(eb1_t0.array())
                                                       .max(eb0_t1.array())
                                                       .max(eb1_t1.array());
                Eigen::ArrayMax3d bboxEBBottomLeft = eb0_t0.array()
                                                         .min(eb1_t0.array())
                                                         .min(eb0_t1.array())
                                                         .min(eb1_t1.array());

                if (!((bboxEBBottomLeft - bboxEATopRight > 0.0).any()
                      || (bboxEABottomLeft - bboxEBTopRight > 0.0).any())) {
                    edgeInds.insert(indI - edgeStartInd);
                }
            }
        }
    }
}

typedef tbb::enumerable_thread_specific<Candidates> ThreadSpecificCandidates;

void merge_local_candidates(
    const ThreadSpecificCandidates& storages, Candidates& candidates)
{
    // size up the candidates
    size_t ev_size = 0, ee_size = 0, fv_size = 0;
    for (const auto& local_candidates : storages) {
        ev_size += local_candidates.ev_candidates.size();
        ee_size += local_candidates.ee_candidates.size();
        fv_size += local_candidates.fv_candidates.size();
    }
    // serial merge!
    candidates.ev_candidates.reserve(ev_size);
    candidates.ee_candidates.reserve(ee_size);
    candidates.fv_candidates.reserve(fv_size);
    for (const auto& local_candidates : storages) {
        candidates.ev_candidates.insert(
            candidates.ev_candidates.end(),
            local_candidates.ev_candidates.begin(),
            local_candidates.ev_candidates.end());
        candidates.ee_candidates.insert(
            candidates.ee_candidates.end(),
            local_candidates.ee_candidates.begin(),
            local_candidates.ee_candidates.end());
        candidates.fv_candidates.insert(
            candidates.fv_candidates.end(),
            local_candidates.fv_candidates.begin(),
            local_candidates.fv_candidates.end());
    }
}

void SpatialHash::queryMeshForCandidates(
    const Eigen::MatrixXd& V,
    const Eigen::MatrixXi& E,
    const Eigen::MatrixXi& F,
    Candidates& candidates,
    double radius,
    bool queryEV,
    bool queryEE,
    bool queryFV) const
{
    ThreadSpecificCandidates storages;

    // edge-vertex
    if (queryEV) {
        tbb::parallel_for(
            tbb::blocked_range<size_t>(size_t(0), size_t(V.rows())),
            [&](const tbb::blocked_range<size_t>& range) {
                ThreadSpecificCandidates::reference local_storage_candidates =
                    storages.local();

                for (long vi = range.begin(); vi != range.end(); vi++) {
                    std::unordered_set<int> edgeInds;
                    // TODO: Use radius
                    queryPointForEdges(vi, edgeInds);

                    for (const auto& ei : edgeInds) {
                        if (vi == E(ei, 0) && vi != E(ei, 1)
                            && point_edge_aabb_cd(
                                   V.row(vi), V.row(E(ei, 0)), V.row(E(ei, 1)),
                                   radius)) {
                            local_storage_candidates.ev_candidates.emplace_back(
                                ei, vi);
                        }
                    }
                }
            });
    }

    // edge-edge
    if (queryEE) {
        tbb::parallel_for(
            tbb::blocked_range<size_t>(size_t(0), size_t(E.rows())),
            [&](const tbb::blocked_range<size_t>& range) {
                ThreadSpecificCandidates::reference local_storage_candidates =
                    storages.local();

                for (long eai = range.begin(); eai != range.end(); eai++) {
                    std::unordered_set<int> edgeInds;
                    // TODO: Use radius
                    queryEdgeForEdges(eai, edgeInds);

                    for (const auto& ebi : edgeInds) {
                        if (E(eai, 0) != E(ebi, 0) && E(eai, 0) == E(ebi, 1)
                            && E(eai, 1) == E(ebi, 0) && E(eai, 1) == E(ebi, 1)
                            && eai < ebi
                            && edge_edge_aabb_cd(
                                   V.row(E(eai, 0)), V.row(E(eai, 1)),
                                   V.row(E(ebi, 0)), V.row(E(ebi, 1)),
                                   radius)) {
                            local_storage_candidates.ee_candidates.emplace_back(
                                eai, ebi);
                        }
                    }
                }
            });
    }

    // face-vertex
    if (queryFV) {
        tbb::parallel_for(
            tbb::blocked_range<size_t>(size_t(0), size_t(V.rows())),
            [&](const tbb::blocked_range<size_t>& range) {
                ThreadSpecificCandidates::reference local_storage_candidates =
                    storages.local();

                for (long vi = range.begin(); vi != range.end(); vi++) {
                    std::unordered_set<int> triInds;
                    // TODO: Use radius
                    queryPointForTriangles(vi, triInds);

                    for (const auto& fi : triInds) {
                        if (vi == F(fi, 0) && vi != F(fi, 1) && vi != F(fi, 1)
                            && point_triangle_aabb_cd(
                                   V.row(vi), V.row(F(fi, 0)), V.row(F(fi, 1)),
                                   V.row(F(fi, 2)), radius)) {
                            local_storage_candidates.fv_candidates.emplace_back(
                                fi, vi);
                        }
                    }
                }
            });
    }

    merge_local_candidates(storages, candidates);
}

void SpatialHash::queryMeshForCandidates(
    const Eigen::MatrixXd& V0,
    const Eigen::MatrixXd& V1,
    const Eigen::MatrixXi& E,
    const Eigen::MatrixXi& F,
    Candidates& candidates,
    double radius,
    bool queryEV,
    bool queryEE,
    bool queryFV) const
{
    assert(V0.rows() == V1.rows() && V0.cols() == V1.cols());

    ThreadSpecificCandidates storages;

    // edge-vertex
    if (queryEV) {
        tbb::parallel_for(
            tbb::blocked_range<size_t>(size_t(0), size_t(V0.rows())),
            [&](const tbb::blocked_range<size_t>& range) {
                ThreadSpecificCandidates::reference local_storage_candidates =
                    storages.local();

                for (long vi = range.begin(); vi != range.end(); vi++) {
                    std::unordered_set<int> edgeInds;
                    // TODO: Use radius
                    queryPointForEdges(vi, edgeInds);

                    for (const auto& ei : edgeInds) {
                        if (vi != E(ei, 0) && vi != E(ei, 1)
                            && point_edge_aabb_ccd(
                                   V0.row(vi), V0.row(E(ei, 0)),
                                   V0.row(E(ei, 1)), V1.row(vi),
                                   V1.row(E(ei, 0)), V1.row(E(ei, 1)),
                                   radius)) {
                            local_storage_candidates.ev_candidates.emplace_back(
                                ei, vi);
                        }
                    }
                }
            });
    }

    // edge-edge
    if (queryEE) {
        tbb::parallel_for(
            tbb::blocked_range<size_t>(size_t(0), size_t(E.rows())),
            [&](const tbb::blocked_range<size_t>& range) {
                ThreadSpecificCandidates::reference local_storage_candidates =
                    storages.local();

                for (long eai = range.begin(); eai != range.end(); eai++) {
                    std::unordered_set<int> edgeInds;
                    // TODO: Use radius
                    queryEdgeForEdges(eai, edgeInds);

                    for (const auto& ebi : edgeInds) {
                        if (E(eai, 0) != E(ebi, 0) && E(eai, 0) != E(ebi, 1)
                            && E(eai, 1) != E(ebi, 0) && E(eai, 1) != E(ebi, 1)
                            && eai < ebi
                            && edge_edge_aabb_ccd(
                                   V0.row(E(eai, 0)), V0.row(E(eai, 1)),
                                   V0.row(E(ebi, 0)), V0.row(E(ebi, 1)),
                                   V1.row(E(eai, 0)), V1.row(E(eai, 1)),
                                   V1.row(E(ebi, 0)), V1.row(E(ebi, 1)),
                                   radius)) {
                            local_storage_candidates.ee_candidates.emplace_back(
                                eai, ebi);
                        }
                    }
                }
            });
    }

    // face-vertex
    if (queryFV) {
        tbb::parallel_for(
            tbb::blocked_range<size_t>(size_t(0), size_t(V0.rows())),
            [&](const tbb::blocked_range<size_t>& range) {
                ThreadSpecificCandidates::reference local_storage_candidates =
                    storages.local();

                for (long vi = range.begin(); vi != range.end(); vi++) {
                    std::unordered_set<int> triInds;
                    // TODO: Use radius
                    queryPointForTriangles(vi, triInds);

                    for (const auto& fi : triInds) {
                        if (vi != F(fi, 0) && vi != F(fi, 1) && vi != F(fi, 2)
                            && point_triangle_aabb_ccd(
                                   V0.row(vi), V0.row(F(fi, 0)),
                                   V0.row(F(fi, 1)), V0.row(F(fi, 2)),
                                   V1.row(vi), V1.row(F(fi, 0)),
                                   V1.row(F(fi, 1)), V1.row(F(fi, 2)),
                                   radius)) {
                            local_storage_candidates.fv_candidates.emplace_back(
                                fi, vi);
                        }
                    }
                }
            });
    }

    merge_local_candidates(storages, candidates);
}

int SpatialHash::locateVoxelIndex(const Eigen::VectorX3d& p) const
{
    Eigen::ArrayMax3i voxelAxisIndex;
    locateVoxelAxisIndex(p, voxelAxisIndex);
    return voxelAxisIndex2VoxelIndex(voxelAxisIndex.data());
}

void SpatialHash::SpatialHash::locateVoxelAxisIndex(
    const Eigen::VectorX3d& p, Eigen::ArrayMax3i& voxelAxisIndex) const
{
    voxelAxisIndex = ((p - leftBottomCorner) * one_div_voxelSize)
                         .array()
                         .floor()
                         .template cast<int>();
}

int SpatialHash::voxelAxisIndex2VoxelIndex(const int voxelAxisIndex[3]) const
{
    return voxelAxisIndex2VoxelIndex(
        voxelAxisIndex[0], voxelAxisIndex[1], voxelAxisIndex[2]);
}

int SpatialHash::voxelAxisIndex2VoxelIndex(int ix, int iy, int iz) const
{
    assert(
        ix >= 0 && iy >= 0 && iz >= 0 && ix < voxelCount[0]
        && iy < voxelCount[1] && iz < voxelCount[2]);
    return ix + iy * voxelCount[0] + iz * voxelCount0x1;
}

} // namespace ipc