set(SOURCES
  edge_edge.cpp
  edge_vertex.cpp
  face_vertex.cpp
  tangential_collision.cpp
  tangential_collisions.cpp
  vertex_vertex.cpp
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${SOURCES})
target_sources(ipctk PRIVATE ${SOURCES})