/**
 *  @file src/ram/graph.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The graph calls of the C ABI over the half-edge stores of `src/ram/database.hpp`, for the in-memory engine's
 *      tables.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <type_traits> // `std::is_same_v`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/graph.h"
#include "ustore/ranges.hpp" // `edges_view_t`, `strided`

#include "ustore/graph/half_edges.hpp"      // `half_edge_t`
#include "ustore/graph/ram_graph.hpp"       // `export_edges`, `stage_edges`, `stage_vertex_removal`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_copy`, `arena_allocator`

#include "database.hpp"          // `database_state`, `with_source`, `stage_runs_of`, `serve_write`, `scan_keys`
#include "utilities/batches.hpp" // `edges_batch_t`, `edges_of`

namespace unum::ustore::ram {

#pragma region Graph Calls

/** Implements @c ustore_graph_read_edges as walks of each vertex's half-edges in the roles it asks for. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_graph_read_edges_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.vertices.data || !arguments.degrees_per_vertex)
        return {status_code_t::invalid_argument_k, "Need the vertices and an output for their degrees"};
    if (arguments.edges_per_vertex && !arguments.edges_offsets)
        return {status_code_t::invalid_argument_k, "Need an output for the offsets of the edges"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    edges_batch_t const tasks {collections, strided(arguments.vertices), strided(arguments.roles), arguments.count};
    auto [degrees, degrees_status] = arena_allocate<ustore_vertex_degree_t>(arguments.arena, arguments.count);
    if (degrees_status.failed()) return degrees_status;
    st::basic_vector<ustore_key_t, arena_allocator<ustore_key_t>> edges {{view.scratch}};
    st::basic_vector<half_edge_t, arena_allocator<half_edge_t>> walked {{view.scratch}};

    status_t outcome;
    store_status_t const exported = with_source(state, view, [&](auto& source) noexcept {
        return for_each_run(source, collections, arguments.count, outcome,
                            [&](auto& entries, std::size_t first, std::size_t end) noexcept -> store_status_t {
                                using entry_t = entry_of<decltype(entries)>;
                                if constexpr (!std::is_same_v<entry_t, half_edge_t>) return refused_modality(outcome);
                                else {
                                    auto store = source.view(entries);
                                    if (!store) return store.status();
                                    return export_edges(*store, tasks, first, end, degrees,
                                                        arguments.edges_per_vertex ? &edges : nullptr, walked, outcome);
                                }
                            });
    });
    if (status_t const status = reported(exported, outcome); status.failed()) return status;
    auto [copied_edges, edges_status] = arena_copy<ustore_key_t>(arguments.arena, edges);
    if (edges_status.failed()) return edges_status;

    // A missing vertex spans no edges
    auto [offsets, offsets_status] = arena_allocate<ustore_length_t>(arguments.arena,
                                                                     arguments.edges_offsets ? arguments.count + 1 : 0);
    if (offsets_status.failed()) return offsets_status;
    if (!offsets.empty()) offsets[0] = 0;
    for (std::size_t index = 0; index + 1 < offsets.size(); ++index)
        offsets[index + 1] = offsets[index] + (degrees[index] == ustore_vertex_degree_missing_k ? 0 : degrees[index]);

    *arguments.degrees_per_vertex = degrees.data();
    if (arguments.edges_offsets) *arguments.edges_offsets = offsets.data();
    if (arguments.edges_per_vertex) *arguments.edges_per_vertex = copied_edges.data();
    return {};
}

/** Implements @c ustore_graph_write_edges, filing every edge under both of its ends. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_graph_write_edges_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.sources.data || !arguments.targets.data)
        return {status_code_t::invalid_argument_k, "Need the sources and the targets of the edges"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    edges_view_t const edges = edges_of(arguments);
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_runs_of<half_edge_t>(source, collections, arguments.count, outcome,
                                          [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                              return stage_edges(part, edges, first, end);
                                          });
    });
}

/** Implements @c ustore_graph_remove_edges, erasing every edge under both of its ends and keeping the vertices. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_graph_remove_edges_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.sources.data || !arguments.targets.data)
        return {status_code_t::invalid_argument_k, "Need the sources and the targets of the edges"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    edges_view_t const edges = edges_of(arguments);
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_runs_of<half_edge_t>(source, collections, arguments.count, outcome,
                                          [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                              return stage_edges_removal(part, edges, first, end);
                                          });
    });
}

/** Implements @c ustore_graph_write_vertices, adding every vertex that is not one already. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_graph_write_vertices_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.vertices.data) return {status_code_t::invalid_argument_k, "No vertices were provided"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    strided_iterator<ustore_key_t const> const vertices = strided(arguments.vertices);
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_runs_of<half_edge_t>(source, collections, arguments.count, outcome,
                                          [&](auto& part, std::size_t first, std::size_t end) noexcept {
                                              for (std::size_t index = first; index != end; ++index) {
                                                  store_status_t const staged = stage_vertex(part, vertices[index]);
                                                  if (st::failed(staged)) return staged;
                                              }
                                              return store_status_t::success_k;
                                          });
    });
}

/** Implements @c ustore_graph_remove_vertices, erasing the edges every neighbor files of each vertex in its role,
 *  then every entry of the vertex. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_graph_remove_vertices_t const& arguments,
               view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.vertices.data) return {status_code_t::invalid_argument_k, "No vertices were provided"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    strided_iterator<ustore_key_t const> const vertices = strided(arguments.vertices);
    strided_iterator<ustore_vertex_role_t const> const roles = strided(arguments.roles);
    st::basic_vector<half_edge_t, arena_allocator<half_edge_t>> walked {{view.scratch}};
    return serve_write(state, arguments.durability, view, [&](auto& source, status_t& outcome) noexcept {
        return stage_runs_of<half_edge_t>(
            source, collections, arguments.count, outcome,
            [&](auto& part, std::size_t first, std::size_t end) noexcept {
                for (std::size_t index = first; index != end; ++index) {
                    ustore_vertex_role_t const role = roles ? roles[index] : ustore_vertex_role_any_k;
                    store_status_t const removed = stage_vertex_removal(part, vertices[index], role, walked);
                    if (st::failed(removed)) return removed;
                }
                return store_status_t::success_k;
            });
    });
}

/** Implements @c ustore_graph_scan_vertices as ordered walks of each graph's vertex entries. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_graph_scan_vertices_t const& arguments,
               view_t const& view) noexcept {
    return scan_keys<half_edge_t>(state, arguments, view);
}

#pragma endregion Graph Calls

#pragma region Table

template <cpu_class_t cpu_class_, isolation_t isolation_>
graph_verbs_t const ram_variant<cpu_class_, isolation_>::graph_verbs_k {
    .read_edges = &serve_database<cpu_class_, isolation_, ustore_graph_read_edges_t>,
    .write_edges = &serve_database<cpu_class_, isolation_, ustore_graph_write_edges_t>,
    .remove_edges = &serve_database<cpu_class_, isolation_, ustore_graph_remove_edges_t>,
    .write_vertices = &serve_database<cpu_class_, isolation_, ustore_graph_write_vertices_t>,
    .remove_vertices = &serve_database<cpu_class_, isolation_, ustore_graph_remove_vertices_t>,
    .scan_vertices = &serve_database<cpu_class_, isolation_, ustore_graph_scan_vertices_t>,
};

#pragma endregion Table

} // namespace unum::ustore::ram
