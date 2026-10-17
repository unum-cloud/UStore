/**
 *  @file src/nvme/graph_store.hpp
 *  @author Ash Vardanian
 *  @date December 1, 2022
 *  @brief The graph calls of the C ABI served over one graph store of `include/ustore/graph/nvme_graph.hpp`.
 *
 *  The store keeps one graph in two databases of its own, vertices and edge blocks, whose transactions are separate
 *  from those of the engine's collections. So the engine serves its graph collections through the vertex values of
 *  `src/graph.cpp`, which its transactions span with every other modality, and these serves stay beside it until the
 *  adjacency fold moves the edge blocks into the engine's own collections. Each serve reads and writes through one
 *  access, the store's or a graph transaction's, on one channel the calling thread holds, and refuses a batch naming
 *  any collection but the main one, as the store holds a single graph.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <span> // `std::span`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/graph.h"    // `ustore_graph_read_edges_t`, `ustore_vertex_role_t`
#include "ustore/ranges.hpp" // `strided`, `strided_iterator`

#include "ustore/graph/nvme_graph.hpp"        // `graph_change_t`, `labeled_edge_t`, `edges_set_t`, `vertex_t`
#include "ustore/utilities/nvme_channels.hpp" // `io_channel_t`
#include "ustore/utilities/ram_buffers.hpp"   // `arena_allocate`, `arena_allocator`, `growth_status`

namespace unum::ustore::nvme {

using graph::any_role_k;
using graph::dedge_source_k;
using graph::dedge_target_k;
using graph::edges_set_t;
using graph::graph_change_t;
using graph::id_entity_t;
using graph::labeled_edge_t;
using graph::role_t;
using graph::vertex_t;
using utilities::arena_allocate;
using utilities::arena_allocator;
using utilities::growth_status;
using utilities::io_channel_t;

/** The role a vertex plays in an edge, as the graph store names it. */
constexpr role_t graph_role_of(ustore_vertex_role_t role) noexcept {
    switch (role) {
    case ustore_vertex_role_source_k: return dedge_source_k;
    case ustore_vertex_role_target_k: return dedge_target_k;
    default: return any_role_k;
    }
}

/** Refuses @p count tasks naming any collection but the main one, which is the one graph a store holds. */
inline status_t refuse_other_graphs(ustore_collections_view_t collections, std::size_t count) noexcept {
    strided_iterator<ustore_collection_id_t const> const named = strided(collections);
    for (std::size_t index = 0; named && index != count; ++index)
        if (named[index] != ustore_collection_main_k)
            return {status_code_t::invalid_argument_k, "A graph store holds the main collection's graph alone"};
    return {};
}

/** The edges a write or a removal of @p count names, directed from their sources, laid out in @p scratch. */
template <typename arguments_type_>
expected<std::span<labeled_edge_t>> graph_edges_of(arguments_type_ const& arguments, ustore_arena_t scratch) noexcept {
    auto [edges, allocate_status] = arena_allocate<labeled_edge_t>(scratch, arguments.count);
    if (allocate_status.failed()) return allocate_status;
    strided_iterator<ustore_key_t const> const sources = strided(arguments.sources);
    strided_iterator<ustore_key_t const> const targets = strided(arguments.targets);
    strided_iterator<ustore_key_t const> const identifiers = strided(arguments.edges);
    for (std::size_t index = 0; index != edges.size(); ++index) {
        labeled_edge_t edge;
        edge.id = identifiers ? identifiers[index] : ustore_edge_id_default_k;
        edge.first = sources[index];
        edge.second = targets[index];
        edges[index] = edge;
    }
    return edges;
}

/** Implements @c ustore_graph_write_vertices over @p access, keeping whatever edges a vertex already has. */
template <typename access_type_>
status_t serve_graph(access_type_ access, io_channel_t& channel, ustore_graph_write_vertices_t const& arguments,
                     ustore_arena_t scratch) noexcept {
    if (!arguments.count) return {};
    if (!arguments.vertices.data) return {status_code_t::invalid_argument_k, "No vertices were provided"};
    if (status_t const refused = refuse_other_graphs(arguments.collections, arguments.count); refused.failed())
        return refused;
    auto [wanted, allocate_status] = arena_allocate<vertex_t>(scratch, arguments.count);
    if (allocate_status.failed()) return allocate_status;
    strided_iterator<ustore_key_t const> const vertices = strided(arguments.vertices);
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        wanted[index] = vertex_t {};
        wanted[index].id = vertices[index];
    }
    std::size_t created = 0;
    return access.upsert_vertices(channel, std::span<vertex_t const> {wanted}, created);
}

/** Implements @c ustore_graph_remove_vertices over @p access, with every edge the vertices play any role in. */
template <typename access_type_>
status_t serve_graph(access_type_ access, io_channel_t& channel, ustore_graph_remove_vertices_t const& arguments,
                     ustore_arena_t scratch) noexcept {
    if (!arguments.count) return {};
    if (!arguments.vertices.data) return {status_code_t::invalid_argument_k, "No vertices were provided"};
    if (status_t const refused = refuse_other_graphs(arguments.collections, arguments.count); refused.failed())
        return refused;
    auto [doomed, allocate_status] = arena_allocate<id_entity_t>(scratch, arguments.count);
    if (allocate_status.failed()) return allocate_status;
    strided_iterator<ustore_key_t const> const vertices = strided(arguments.vertices);
    for (std::size_t index = 0; index != doomed.size(); ++index) doomed[index] = vertices[index];
    graph_change_t changed;
    return access.remove_vertices(channel, std::span<id_entity_t const> {doomed}, changed);
}

/** Implements @c ustore_graph_write_edges over @p access, filing every edge under both of its ends. */
template <typename access_type_>
status_t serve_graph(access_type_ access, io_channel_t& channel, ustore_graph_write_edges_t const& arguments,
                     ustore_arena_t scratch) noexcept {
    if (!arguments.count) return {};
    if (!arguments.sources.data || !arguments.targets.data)
        return {status_code_t::invalid_argument_k, "Need the sources and the targets of the edges"};
    if (status_t const refused = refuse_other_graphs(arguments.collections, arguments.count); refused.failed())
        return refused;
    auto [edges, edges_status] = graph_edges_of(arguments, scratch);
    if (edges_status.failed()) return edges_status;
    graph_change_t changed;
    return access.upsert_edges(channel, std::span<labeled_edge_t const> {edges}, changed);
}

/** Implements @c ustore_graph_remove_edges over @p access, keeping the vertices. */
template <typename access_type_>
status_t serve_graph(access_type_ access, io_channel_t& channel, ustore_graph_remove_edges_t const& arguments,
                     ustore_arena_t scratch) noexcept {
    if (!arguments.count) return {};
    if (!arguments.sources.data || !arguments.targets.data)
        return {status_code_t::invalid_argument_k, "Need the sources and the targets of the edges"};
    if (status_t const refused = refuse_other_graphs(arguments.collections, arguments.count); refused.failed())
        return refused;
    auto [edges, edges_status] = graph_edges_of(arguments, scratch);
    if (edges_status.failed()) return edges_status;
    graph_change_t changed;
    return access.remove_edges(channel, std::span<labeled_edge_t const> {edges}, changed);
}

/** Implements @c ustore_graph_read_edges over @p access: the degree of every vertex, and its edges as a tape of
 *  source, target and edge keys, laid out once their total is known. */
template <typename access_type_>
status_t serve_graph(access_type_ access, io_channel_t& channel, ustore_graph_read_edges_t const& arguments,
                     ustore_arena_t scratch) noexcept {
    if (!arguments.count) return {};
    if (!arguments.vertices.data || !arguments.degrees_per_vertex)
        return {status_code_t::invalid_argument_k, "Need the vertices and an output for their degrees"};
    if (arguments.edges_per_vertex && !arguments.edges_offsets)
        return {status_code_t::invalid_argument_k, "Need an output for the offsets of the edges"};
    if (status_t const refused = refuse_other_graphs(arguments.collections, arguments.count); refused.failed())
        return refused;
    auto [degrees, degrees_status] = arena_allocate<ustore_vertex_degree_t>(arguments.arena, arguments.count);
    if (degrees_status.failed()) return degrees_status;
    auto [offsets, offsets_status] = arena_allocate<ustore_length_t>(arguments.arena,
                                                                     arguments.edges_offsets ? arguments.count + 1 : 0);
    if (offsets_status.failed()) return offsets_status;

    strided_iterator<ustore_key_t const> const vertices = strided(arguments.vertices);
    strided_iterator<ustore_vertex_role_t const> const roles = strided(arguments.roles);
    st::basic_vector<labeled_edge_t, arena_allocator<labeled_edge_t>> gathered {{scratch}};
    for (std::size_t index = 0; index != arguments.count; ++index) {
        if (!offsets.empty()) offsets[index] = static_cast<ustore_length_t>(gathered.size());
        id_entity_t const vertex = vertices[index];
        vertex_t found;
        status_t const known = access.find_vertex(channel, vertex, found);
        if (known == status_code_t::not_found_k) {
            degrees[index] = ustore_vertex_degree_missing_k;
            continue;
        }
        if (known.failed()) return known;
        edges_set_t edges;
        role_t const role = graph_role_of(roles ? roles[index] : ustore_vertex_role_any_k);
        if (status_t const walked = access.edges_containing(channel, vertex, role, edges); walked.failed())
            return walked;
        degrees[index] = static_cast<ustore_vertex_degree_t>(edges.size());
        for (labeled_edge_t const& edge : edges)
            if (status_t const kept = growth_status(gathered.push_back(labeled_edge_t {edge})); kept.failed())
                return kept;
    }
    if (!offsets.empty()) offsets[arguments.count] = static_cast<ustore_length_t>(gathered.size());

    auto [tape, tape_status] = arena_allocate<ustore_key_t>(arguments.arena,
                                                            arguments.edges_per_vertex ? 3 * gathered.size() : 0);
    if (tape_status.failed()) return tape_status;
    for (std::size_t index = 0; index != gathered.size() && !tape.empty(); ++index) {
        tape[3 * index + 0] = gathered[index].first;
        tape[3 * index + 1] = gathered[index].second;
        tape[3 * index + 2] = gathered[index].id;
    }
    *arguments.degrees_per_vertex = degrees.data();
    if (arguments.edges_offsets) *arguments.edges_offsets = offsets.data();
    if (arguments.edges_per_vertex) *arguments.edges_per_vertex = tape.data();
    return {};
}

} // namespace unum::ustore::nvme
