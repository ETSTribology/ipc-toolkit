#include <common.hpp>

#include <ipc/broad_phase/aabb.hpp>

namespace py = pybind11;
using namespace ipc;

void define_aabb(py::module_& m)
{
    py::class_<AABB>(m, "AABB")
        .def(py::init())
        .def(
            py::init<
                Eigen::ConstRef<ArrayMax3d>, Eigen::ConstRef<ArrayMax3d>>(),
            py::arg("min"), py::arg("max"))
        .def(
            py::init<const AABB&, const AABB&>(), py::arg("aabb1"),
            py::arg("aabb2"))
        .def(
            py::init<const AABB&, const AABB&, const AABB&>(), py::arg("aabb1"),
            py::arg("aabb2"), py::arg("aabb3"))
        .def_static(
            "from_point",
            py::overload_cast<Eigen::ConstRef<VectorMax3d>, const double>(
                &AABB::from_point),
            R"ipc_Qu8mg5v7(
            Construct an AABB for a static point.

            Parameters:
                p: The point's position.
                inflation_radius: Radius of a sphere around the point which the AABB encloses.

            Returns:
                The constructed AABB.
            )ipc_Qu8mg5v7",
            py::arg("p"), py::arg("inflation_radius") = 0)
        .def_static(
            "from_point",
            py::overload_cast<
                Eigen::ConstRef<VectorMax3d>, Eigen::ConstRef<VectorMax3d>,
                const double>(&AABB::from_point),
            R"ipc_Qu8mg5v7(
            Construct an AABB for a moving point (i.e. temporal edge).

            Parameters:
                p_t0: The point's position at time t=0.
                p_t1: The point's position at time t=1.
                inflation_radius: Radius of a capsule around the temporal edge which the AABB encloses.

            Returns:
                The constructed AABB.
            )ipc_Qu8mg5v7",
            py::arg("p_t0"), py::arg("p_t1"), py::arg("inflation_radius") = 0)
        .def(
            "intersects", &AABB::intersects,
            R"ipc_Qu8mg5v7(
            Check if another AABB intersects with this one.

            Parameters:
                other: The other AABB.

            Returns:
                If the two AABBs intersect.
            )ipc_Qu8mg5v7",
            py::arg("other"))
        .def_static(
            "conservative_inflation",
            [](ArrayMax3d min, ArrayMax3d max, const double inflation_radius) {
                AABB::conservative_inflation(min, max, inflation_radius);
                return std::make_tuple(min, max);
            },
            "Compute a conservative inflation of the AABB.", py::arg("min"),
            py::arg("max"), py::arg("inflation_radius"))
        .def_readwrite("min", &AABB::min, "Minimum corner of the AABB.")
        .def_readwrite("max", &AABB::max, "Maximum corner of the AABB.")
        .def_readwrite(
            "vertex_ids", &AABB::vertex_ids,
            "Vertex IDs attached to the AABB.");

    m.def(
        "build_vertex_boxes",
        [](Eigen::ConstRef<Eigen::MatrixXd> vertices,
           const double inflation_radius = 0) {
            std::vector<AABB> vertex_boxes;
            build_vertex_boxes(vertices, vertex_boxes, inflation_radius);
            return vertex_boxes;
        },
        R"ipc_Qu8mg5v7(
        Build one AABB per vertex position (row of V).

        Parameters:
            vertices: Vertex positions (rowwise).
            inflation_radius: Radius of a sphere around the points which the AABBs enclose.

        Returns:
            Vertex AABBs.
        )ipc_Qu8mg5v7",
        py::arg("vertices"), py::arg("inflation_radius") = 0);

    m.def(
        "build_vertex_boxes",
        [](Eigen::ConstRef<Eigen::MatrixXd> vertices_t0,
           Eigen::ConstRef<Eigen::MatrixXd> vertices_t1,
           const double inflation_radius = 0) {
            std::vector<AABB> vertex_boxes;
            build_vertex_boxes(
                vertices_t0, vertices_t1, vertex_boxes, inflation_radius);
            return vertex_boxes;
        },
        R"ipc_Qu8mg5v7(
        Build one AABB per vertex position moving linearly from t=0 to t=1.

        Parameters:
            vertices_t0: Vertex positions at t=0 (rowwise).
            vertices_t1: Vertex positions at t=1 (rowwise).
            inflation_radius: Radius of a capsule around the temporal edges which the AABBs enclose.

        Returns:
            Vertex AABBs.
        )ipc_Qu8mg5v7",
        py::arg("vertices_t0"), py::arg("vertices_t1"),
        py::arg("inflation_radius") = 0);

    m.def(
        "build_edge_boxes",
        [](const std::vector<AABB>& vertex_boxes,
           Eigen::ConstRef<Eigen::MatrixXi> edges) {
            std::vector<AABB> edge_boxes;
            build_edge_boxes(vertex_boxes, edges, edge_boxes);
            return edge_boxes;
        },
        R"ipc_Qu8mg5v7(
        Build one AABB per edge.

        Parameters:
            vertex_boxes: Vertex AABBs.
            edges: Edges (rowwise).

        Returns:
            edge_boxes: Edge AABBs.
        )ipc_Qu8mg5v7",
        py::arg("vertex_boxes"), py::arg("edges"));

    m.def(
        "build_face_boxes",
        [](const std::vector<AABB>& vertex_boxes,
           Eigen::ConstRef<Eigen::MatrixXi> faces) {
            std::vector<AABB> face_boxes;
            build_face_boxes(vertex_boxes, faces, face_boxes);
            return face_boxes;
        },
        R"ipc_Qu8mg5v7(
        Build one AABB per face.

        Parameters:
            vertex_boxes: Vertex AABBs.
            faces: Faces (rowwise).

        Returns:
            face_boxes: Face AABBs.
        )ipc_Qu8mg5v7",
        py::arg("vertex_boxes"), py::arg("faces"));
}
