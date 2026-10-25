/**
 *  @file src/graph.cpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Implements `c/ustore/graph.h`: routes every graph call to the graph verbs of the engine serving its
 *      database, and spells the vertex roles.
 */

#include "ustore/graph.h" // `ustore_vertex_role_t`

#include "engine.hpp" // `engine_t`, `graph_verbs_t`
#include "lib.hpp"    // `route`, `enumerator_spelling`, `name_in`, `value_in`

namespace unum::ustore {

#pragma region Names

/** The canonical spelling of every vertex role. */
constexpr enumerator_spelling<ustore_vertex_role_t> vertex_role_names_k[] {
    {ustore_vertex_role_unknown_k, "unknown"},
    {ustore_vertex_role_source_k, "source"},
    {ustore_vertex_role_target_k, "target"},
    {ustore_vertex_role_any_k, "any"},
};

#pragma endregion Names

} // namespace unum::ustore

void ustore_graph_read_edges(ustore_graph_read_edges_t* arguments) {
    route<&engine_t::graph, &graph_verbs_t::read_edges>(arguments);
}
void ustore_graph_write_edges(ustore_graph_write_edges_t* arguments) {
    route<&engine_t::graph, &graph_verbs_t::write_edges>(arguments);
}
void ustore_graph_remove_edges(ustore_graph_remove_edges_t* arguments) {
    route<&engine_t::graph, &graph_verbs_t::remove_edges>(arguments);
}
void ustore_graph_write_vertices(ustore_graph_write_vertices_t* arguments) {
    route<&engine_t::graph, &graph_verbs_t::write_vertices>(arguments);
}
void ustore_graph_remove_vertices(ustore_graph_remove_vertices_t* arguments) {
    route<&engine_t::graph, &graph_verbs_t::remove_vertices>(arguments);
}
void ustore_graph_scan_vertices(ustore_graph_scan_vertices_t* arguments) {
    route<&engine_t::graph, &graph_verbs_t::scan_vertices>(arguments);
}

char const* ustore_vertex_role_name(ustore_vertex_role_t vertex_role) {
    return name_in(vertex_role_names_k, vertex_role);
}

ustore_vertex_role_t ustore_vertex_role_from_name(ustore_bytes_view_t name) {
    return value_in(vertex_role_names_k, name);
}
