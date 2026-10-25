/**
 *  @file src/nvme/graph_calls.hpp
 *  @author Ash Vardanian
 *  @date December 1, 2022
 *  @brief The graph calls of the C ABI over the graph stores of `include/ustore/graph/nvme_store.hpp`, for the NVMe
 *      engine's table.
 *
 *  `src/nvme/variants.hpp` includes this file right before its table, as these calls build on its accesses, its
 *  writes and its catalog lookups. Every vertex is one key of its graph collection, holding the half-edges of
 *  `include/ustore/graph/adjacency_fold.hpp`, and every edge is a half-edge filed under each of its ends.
 *
 *  A write appends one change delta per touched vertex and reads nothing, so two transactions adding edges to one
 *  vertex both commit. What a write does depend on is watched, as a read is: a vertex an edge or a vertex write
 *  creates, which two transactions creating it conflict over, as the in-memory engine's do; a vertex a removal of edges
 *  changes, which must still be there; and the adjacency a vertex removal reads to find the other ends of its edges.
 *  A read batches the vertices of every run of tasks on one collection into one select, and merges the reading
 *  transaction's own changes into the committed values in place.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::max`, `std::sort`
#include <span>      // `std::span`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/graph.h"    // `ustore_graph_read_edges_t`, `ustore_vertex_role_t`
#include "ustore/ranges.hpp" // `strided`, `strided_iterator`

#include "ustore/graph/adjacency_fold.hpp"  // `adjacency_fold_t`, `vertex_adjacency_t`, `deltas_of_edge`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_allocator`, `growth_status`

namespace unum::ustore::nvme {

using graph::adjacency_fold_t;
using graph::any_role_k;
using graph::dedge_source_k;
using graph::dedge_target_k;
using graph::deltas_of_edge;
using graph::encode_deltas;
using graph::half_edge_cursor_t;
using graph::half_edge_delta_t;
using graph::half_edge_operation_t;
using graph::labeled_edge_t;
using graph::role_t;
using graph::vertex_adjacency_t;

/** The fold every change delta of a graph collection is written for. */
inline constexpr adjacency_fold_t graph_fold_k {};

/** The half-edge roles a vertex plays as @p role names it, every role for any. */
constexpr role_t half_edge_roles_of(ustore_vertex_role_t role) noexcept {
    switch (role) {
    case ustore_vertex_role_source_k: return dedge_source_k;
    case ustore_vertex_role_target_k: return dedge_target_k;
    case ustore_vertex_role_any_k: return any_role_k;
    default: return role_t {};
    }
}

/** The role the other end of a half-edge in @p role files it under. */
constexpr role_t mirrored_role(role_t role) noexcept {
    if (role == dedge_source_k) return dedge_target_k;
    if (role == dedge_target_k) return dedge_source_k;
    return role;
}

/** Admits a graph collection, which the graph calls alone address. */
inline status_t holds_graph(ustore_modality_t modality) noexcept {
    if (modality != ustore_modality_graph_k)
        return {status_code_t::invalid_argument_k, "The collection holds no graph"};
    return {};
}

/** Refuses a graph write of @p durability, or one watching as @p watch, that the transaction or its absence rules
 *  out, as every write of the engine does. */
inline status_t validate_graph_write(ustore_transaction_t transaction, ustore_durability_t durability,
                                     ustore_watch_t watch) noexcept {
    if (durability != ustore_durability_buffered_k && durability != ustore_durability_flushed_k)
        return {status_code_t::invalid_argument_k, "Unknown durability"};
    if (watch != ustore_watch_keys_k && watch != ustore_watch_none_k)
        return {status_code_t::invalid_argument_k, "Unknown watch"};
    if (transaction && durability == ustore_durability_flushed_k)
        return {status_code_t::invalid_argument_k, "A transaction flushes at commit, not at a write"};
    return {};
}

/** The schedule a graph write watches what it depends on with: a transaction's own, and conflicts retried at the
 *  head. */
inline watch_schedule_t graph_write_schedule(view_t const& view) noexcept {
    return view.transaction ? schedule_of(view.watch) : watch_schedule_t::on_commit_k;
}

/** Where the tasks on the collection of task @p first end: the next task naming another one. */
inline std::size_t run_end(strided_iterator<ustore_collection_id_t const> collections, std::size_t first,
                           std::size_t count) noexcept {
    if (!collections) return count;
    std::size_t end = first + 1;
    while (end != count && collections[end] == collections[first]) ++end;
    return end;
}

#pragma region Vertex Reads

/** One vertex as a call sees it: whether it exists, its folded value, and the composed chain the reading transaction
 *  changed it by. */
struct vertex_read_t {
    bool present = false;
    std::span<byte_t const> base;
    std::span<byte_t const> pending;
};

/** Whether @p key of @p collection held a vertex at the stamp @p access reads committed values at. */
template <cpu_class_t cpu_class_>
bool committed_vertex(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t key) noexcept {
    return access.state.store->contains(collection, key, read_stamp_of(access), access.thread_index);
}

/** Whether @p key of @p collection holds a vertex as @p access sees it, its own writes included. */
template <cpu_class_t cpu_class_>
bool visible_vertex(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t key) noexcept {
    delta_view_t own;
    if (access.transaction && access.transaction->uncommitted_delta(collection, key, own))
        return own.kind != delta_kind_t::remove_k;
    return committed_vertex(access, collection, key);
}

/**
 *  @brief Reads the vertices @p keys of @p collection as @p access sees them into @p reads, in one batched select,
 *      watching them through a watching transaction.
 *
 *  A transaction's own write replaces the committed value, its removal hides it, and its change rides beside the
 *  committed value for the read to merge, viewing the transaction's own buffer.
 */
template <cpu_class_t cpu_class_>
status_t read_vertices(call_access<cpu_class_> const& access, collection_id_t collection,
                       std::span<stored_key_t const> keys, std::span<vertex_read_t> reads) noexcept {
    using value_t = st::basic_vector<byte_t, arena_allocator<byte_t>>;
    auto [results, results_status] = arena_allocate<lookup_result_t>(access.scratch, keys.size());
    if (results_status.failed()) return results_status;
    st::basic_vector<value_t, arena_allocator<value_t>> values {{access.scratch}};
    if (status_t const room = growth_status(values.reserve(keys.size())); room.failed()) return room;
    for (std::size_t index = 0; index != keys.size(); ++index) {
        results[index] = lookup_result_t::missing_k;
        [[maybe_unused]] st::status_t const made = values.push_back(st::assume_reserved,
                                                                    value_t {arena_allocator<byte_t> {access.scratch}});
    }

    // A value read from a run arrives in pieces at their offsets, and a folded one whole
    status_t grown;
    auto const sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        value_t& value = values[request];
        std::size_t const end = static_cast<std::size_t>(offset) + bytes.size();
        if (value.size() < end && grown.succeeded()) grown = growth_status(value.resize(end));
        if (grown.succeeded() && !bytes.empty()) std::memcpy(value.data() + offset, bytes.data(), bytes.size());
    };
    if (status_t const read = access.state.store->select(collection, keys, results, sink, read_stamp_of(access),
                                                         access.thread_index);
        read.failed())
        return read;
    if (grown.failed()) return grown;

    for (std::size_t index = 0; index != keys.size(); ++index) {
        if (results[index] == lookup_result_t::incomplete_k)
            return {status_code_t::io_k, "The medium moved fewer bytes than a vertex claims"};
        vertex_read_t& read = reads[index];
        read = vertex_read_t {};
        if (results[index] == lookup_result_t::written_k)
            read = vertex_read_t {true, std::span<byte_t const> {values[index].data(), values[index].size()}, {}};
        delta_view_t own;
        if (!access.transaction || !access.transaction->uncommitted_delta(collection, keys[index], own)) continue;
        if (own.kind == delta_kind_t::remove_k) read = vertex_read_t {};
        else if (own.kind == delta_kind_t::write_k) read = vertex_read_t {true, own.bytes, {}};
        else {
            read.present = true;
            read.pending = own.bytes;
        }
    }
    return watch_keys(access, collection, keys);
}

/** Reads the vertices of every run of tasks on one collection, @p keys naming them in task order, into @p reads. */
template <cpu_class_t cpu_class_>
status_t read_task_vertices(call_access<cpu_class_> const& access,
                            strided_iterator<ustore_collection_id_t const> collections,
                            std::span<stored_key_t const> keys, std::span<vertex_read_t> reads) noexcept {
    for (std::size_t first = 0; first != keys.size();) {
        std::size_t const end = run_end(collections, first, keys.size());
        if (status_t const read = read_vertices(access, task_collection(collections, first),
                                                keys.subspan(first, end - first), reads.subspan(first, end - first));
            read.failed())
            return read;
        first = end;
    }
    return {};
}

#pragma endregion Vertex Reads

#pragma region Vertex Writes

/** One half-edge delta a write appends to the vertex it names, with its place in the batch, which orders the deltas of
 *  one half-edge. */
struct vertex_delta_t {
    ustore_collection_id_t collection;
    ustore_key_t vertex;
    half_edge_delta_t delta;
    std::size_t sequence = 0;
};

/** The records of one vertex, serialized back to back in batch order, which one change carries. */
struct vertex_chain_t {
    ustore_collection_id_t collection;
    ustore_key_t vertex;
    std::span<byte_t const> records;
};

/** Sorts @p deltas by collection, vertex and batch position, and serializes the records of every vertex into one
 *  chain of @p chains, all in @p scratch. */
template <typename chains_type_>
status_t chain_by_vertex(std::span<vertex_delta_t> deltas, ustore_arena_t scratch, chains_type_& chains) noexcept {
    std::sort(deltas.begin(), deltas.end(), [](vertex_delta_t const& first, vertex_delta_t const& second) noexcept {
        if (first.collection != second.collection) return first.collection < second.collection;
        if (first.vertex != second.vertex) return first.vertex < second.vertex;
        return first.sequence < second.sequence;
    });
    arena_allocator<byte_t> arena {scratch};
    for (std::size_t first = 0; first != deltas.size();) {
        std::size_t end = first + 1;
        while (end != deltas.size() && deltas[end].collection == deltas[first].collection &&
               deltas[end].vertex == deltas[first].vertex)
            ++end;
        auto [records, records_status] = arena_allocate<half_edge_delta_t>(scratch, end - first);
        if (records_status.failed()) return records_status;
        for (std::size_t index = first; index != end; ++index) records[index - first] = deltas[index].delta;
        auto [chain, chain_status] = encode_deltas(std::span<half_edge_delta_t const> {records}, arena);
        if (chain_status.failed()) return chain_status;
        if (status_t const kept = growth_status(
                chains.push_back(vertex_chain_t {deltas[first].collection, deltas[first].vertex, chain}));
            kept.failed())
            return kept;
        first = end;
    }
    return {};
}

/** Appends @p chain to its vertex through the transaction of @p access, folded after the vertex's own writes. */
template <cpu_class_t cpu_class_>
status_t append_chain(call_access<cpu_class_> const& access, vertex_chain_t const& chain) noexcept {
    return access.transaction->change(chain.collection, stored_of(chain.vertex.uuid), chain.records, graph_fold_k);
}

/** Watches @p key of @p collection through the transaction of @p access, when it watches at all. */
template <cpu_class_t cpu_class_>
status_t watch_vertex(call_access<cpu_class_> const& access, collection_id_t collection, stored_key_t key) noexcept {
    return watch_keys(access, collection, std::span<stored_key_t const> {&key, 1});
}

/** Lays the records of the edges of @p arguments out in @p scratch, two per edge but for an undirected loop, as
 *  @p operation inserts or removes them, directed from their sources. */
template <typename arguments_type_>
expected<std::span<vertex_delta_t>> edge_deltas(arguments_type_ const& arguments, half_edge_operation_t operation,
                                                ustore_arena_t scratch) noexcept {
    auto [deltas, allocate_status] = arena_allocate<vertex_delta_t>(scratch, 2 * arguments.count);
    if (allocate_status.failed()) return allocate_status;
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    strided_iterator<ustore_key_t const> const sources = strided(arguments.sources);
    strided_iterator<ustore_key_t const> const targets = strided(arguments.targets);
    strided_iterator<ustore_key_t const> const identifiers = strided(arguments.edges);
    std::size_t count = 0;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        labeled_edge_t edge;
        edge.id = identifiers ? identifiers[index] : ustore_edge_id_default_k;
        edge.first = sources[index];
        edge.second = targets[index];
        ustore_key_t ends[2];
        half_edge_delta_t records[2];
        std::size_t const filed = deltas_of_edge(edge, operation, ends, records);
        for (std::size_t end = 0; end != filed; ++end, ++count)
            deltas[count] = vertex_delta_t {task_collection(collections, index), ends[end], records[end], count};
    }
    return deltas.first(count);
}

#pragma endregion Vertex Writes

#pragma region Graph Calls

/** Implements @c ustore_graph_read_edges: the degree of every vertex in its roles, and its edges as a tape of source,
 *  target and edge keys, outgoing by target before incoming by source, laid out once their total is known. */
template <cpu_class_t cpu_class_>
status_t graph_read_edges(ustore_graph_read_edges_t const& arguments, view_t const& view) noexcept {
    std::size_t const count = arguments.count;
    if (!count) return {};
    if (!arguments.vertices.data || !arguments.degrees_per_vertex)
        return {status_code_t::invalid_argument_k, "Need the vertices and an output for their degrees"};
    if (arguments.edges_per_vertex && !arguments.edges_offsets)
        return {status_code_t::invalid_argument_k, "Need an output for the offsets of the edges"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, count, holds_graph); resolved.failed())
        return resolved;
    call_access<cpu_class_> const access = access_of(state, view);

    strided_iterator<ustore_key_t const> const vertices = strided(arguments.vertices);
    strided_iterator<ustore_vertex_role_t const> const roles = strided(arguments.roles);
    auto [keys, keys_status] = arena_allocate<stored_key_t>(view.scratch, count);
    if (keys_status.failed()) return keys_status;
    auto [reads, reads_status] = arena_allocate<vertex_read_t>(view.scratch, count);
    if (reads_status.failed()) return reads_status;
    for (std::size_t index = 0; index != count; ++index) keys[index] = stored_of(vertices[index].uuid);
    if (status_t const read = read_task_vertices(access, collections, keys, reads); read.failed()) return read;

    auto [degrees, degrees_status] = arena_allocate<ustore_vertex_degree_t>(arguments.arena, count);
    if (degrees_status.failed()) return degrees_status;
    auto [offsets, offsets_status] = arena_allocate<ustore_length_t>(arguments.arena,
                                                                     arguments.edges_offsets ? count + 1 : 0);
    if (offsets_status.failed()) return offsets_status;
    std::size_t exported = 0;
    for (std::size_t index = 0; index != count; ++index) {
        if (!offsets.empty()) offsets[index] = static_cast<ustore_length_t>(exported);
        if (!reads[index].present) {
            degrees[index] = ustore_vertex_degree_missing_k;
            continue;
        }
        expected<vertex_adjacency_t> const adjacency = vertex_adjacency_t::make(vertices[index], reads[index].base,
                                                                                reads[index].pending);
        if (!adjacency) return adjacency.status();
        std::size_t const degree = adjacency->degree(
            half_edge_roles_of(roles ? roles[index] : ustore_vertex_role_any_k));
        degrees[index] = static_cast<ustore_vertex_degree_t>(degree);
        exported += degree;
    }
    if (!offsets.empty()) offsets[count] = static_cast<ustore_length_t>(exported);
    *arguments.degrees_per_vertex = degrees.data();
    if (arguments.edges_offsets) *arguments.edges_offsets = offsets.data();
    if (!arguments.edges_per_vertex) return {};

    auto [tape, tape_status] = arena_allocate<ustore_key_t>(arguments.arena, 3 * exported);
    if (tape_status.failed()) return tape_status;
    ustore_key_t* written = tape.data();
    for (std::size_t index = 0; index != count; ++index) {
        if (!reads[index].present) continue;
        ustore_key_t const vertex = vertices[index];
        role_t const wanted = half_edge_roles_of(roles ? roles[index] : ustore_vertex_role_any_k);
        auto const adjacency = *vertex_adjacency_t::make(vertex, reads[index].base, reads[index].pending);
        for (role_t const role : {dedge_source_k, dedge_target_k}) {
            if (!(role & wanted)) continue;
            for (half_edge_cursor_t cursor = adjacency.half_edges(); cursor.valid(); cursor.next()) {
                graph::labeled_half_edge_t const half_edge = cursor.current();
                if (!(half_edge.role & role)) continue;
                bool const outgoing = role == dedge_source_k;
                *written++ = outgoing ? vertex : half_edge.neighbor_id;
                *written++ = outgoing ? half_edge.neighbor_id : vertex;
                *written++ = half_edge.edge_id;
            }
        }
    }
    *arguments.edges_per_vertex = tape.data();
    return {};
}

/** Implements @c ustore_graph_write_edges, appending every edge to both of its ends, which creates the ends a graph
 *  lacked, and watching the ends it creates. */
template <cpu_class_t cpu_class_>
status_t graph_write_edges(ustore_graph_write_edges_t const& arguments, view_t const& view) noexcept {
    if (status_t const valid = validate_graph_write(view.transaction, arguments.durability, view.watch);
        valid.failed() || !arguments.count)
        return valid;
    if (!arguments.sources.data || !arguments.targets.data)
        return {status_code_t::invalid_argument_k, "Need the sources and the targets of the edges"};
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, strided(arguments.collections), arguments.count,
                                               holds_graph);
        resolved.failed())
        return resolved;
    auto [deltas, deltas_status] = edge_deltas(arguments, half_edge_operation_t::insert_k, view.scratch);
    if (deltas_status.failed()) return deltas_status;
    st::basic_vector<vertex_chain_t, arena_allocator<vertex_chain_t>> chains {{view.scratch}};
    if (status_t const chained = chain_by_vertex(deltas, view.scratch, chains); chained.failed()) return chained;

    std::size_t const bytes = deltas.size() * graph::half_edge_delta_bytes_k;
    return serve_write(
        state, view, arguments.durability, chains.size(), bytes, graph_write_schedule(view),
        [&](call_access<cpu_class_> const& access) noexcept -> status_t {
            for (vertex_chain_t const& chain : chains) {
                collection_id_t const collection = chain.collection;
                stored_key_t const key = stored_of(chain.vertex.uuid);
                if (access.schedule != watch_schedule_t::never_k && !committed_vertex(access, collection, key))
                    if (status_t const watched = watch_vertex(access, collection, key); watched.failed())
                        return watched;
                if (status_t const appended = append_chain(access, chain); appended.failed()) return appended;
            }
            return {};
        });
}

/** Implements @c ustore_graph_remove_edges, appending the removal of every edge to both of its ends, which keeps the
 *  vertices and skips the ends a graph lacks, watching the ends it changes. */
template <cpu_class_t cpu_class_>
status_t graph_remove_edges(ustore_graph_remove_edges_t const& arguments, view_t const& view) noexcept {
    if (status_t const valid = validate_graph_write(view.transaction, arguments.durability, view.watch);
        valid.failed() || !arguments.count)
        return valid;
    if (!arguments.sources.data || !arguments.targets.data)
        return {status_code_t::invalid_argument_k, "Need the sources and the targets of the edges"};
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, strided(arguments.collections), arguments.count,
                                               holds_graph);
        resolved.failed())
        return resolved;
    auto [deltas, deltas_status] = edge_deltas(arguments, half_edge_operation_t::remove_k, view.scratch);
    if (deltas_status.failed()) return deltas_status;
    st::basic_vector<vertex_chain_t, arena_allocator<vertex_chain_t>> chains {{view.scratch}};
    if (status_t const chained = chain_by_vertex(deltas, view.scratch, chains); chained.failed()) return chained;

    std::size_t const bytes = deltas.size() * graph::half_edge_delta_bytes_k;
    return serve_write(state, view, arguments.durability, chains.size(), bytes, graph_write_schedule(view),
                       [&](call_access<cpu_class_> const& access) noexcept -> status_t {
                           for (vertex_chain_t const& chain : chains) {
                               collection_id_t const collection = chain.collection;
                               stored_key_t const key = stored_of(chain.vertex.uuid);
                               if (!visible_vertex(access, collection, key)) continue;
                               if (status_t const watched = watch_vertex(access, collection, key); watched.failed())
                                   return watched;
                               if (status_t const appended = append_chain(access, chain); appended.failed())
                                   return appended;
                           }
                           return {};
                       });
}

/** Implements @c ustore_graph_write_vertices, appending an empty change to every vertex the graph lacks, which keeps
 *  the edges of those it holds, and watching the vertices it creates. */
template <cpu_class_t cpu_class_>
status_t graph_write_vertices(ustore_graph_write_vertices_t const& arguments, view_t const& view) noexcept {
    if (status_t const valid = validate_graph_write(view.transaction, arguments.durability, view.watch);
        valid.failed() || !arguments.count)
        return valid;
    if (!arguments.vertices.data) return {status_code_t::invalid_argument_k, "No vertices were provided"};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, arguments.count, holds_graph);
        resolved.failed())
        return resolved;
    strided_iterator<ustore_key_t const> const vertices = strided(arguments.vertices);
    return serve_write(
        state, view, arguments.durability, arguments.count, 1, graph_write_schedule(view),
        [&](call_access<cpu_class_> const& access) noexcept -> status_t {
            for (std::size_t index = 0; index != arguments.count; ++index) {
                collection_id_t const collection = task_collection(collections, index);
                stored_key_t const key = stored_of(vertices[index].uuid);
                if (visible_vertex(access, collection, key)) continue;
                if (!committed_vertex(access, collection, key))
                    if (status_t const watched = watch_vertex(access, collection, key); watched.failed())
                        return watched;
                if (status_t const created = access.transaction->change(collection, key, {}, graph_fold_k);
                    created.failed())
                    return created;
            }
            return {};
        });
}

/** Implements @c ustore_graph_remove_vertices: the removal of every half-edge the other ends of its edges in its role
 *  file of it, then of the vertex itself, watching the adjacency it read and the ends it changes. */
template <cpu_class_t cpu_class_>
status_t graph_remove_vertices(ustore_graph_remove_vertices_t const& arguments, view_t const& view) noexcept {
    if (status_t const valid = validate_graph_write(view.transaction, arguments.durability, view.watch);
        valid.failed() || !arguments.count)
        return valid;
    if (!arguments.vertices.data) return {status_code_t::invalid_argument_k, "No vertices were provided"};
    std::size_t const count = arguments.count;
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, collections, count, holds_graph); resolved.failed())
        return resolved;
    strided_iterator<ustore_key_t const> const vertices = strided(arguments.vertices);
    strided_iterator<ustore_vertex_role_t const> const roles = strided(arguments.roles);
    auto [keys, keys_status] = arena_allocate<stored_key_t>(view.scratch, count);
    if (keys_status.failed()) return keys_status;
    auto [reads, reads_status] = arena_allocate<vertex_read_t>(view.scratch, count);
    if (reads_status.failed()) return reads_status;
    for (std::size_t index = 0; index != count; ++index) keys[index] = stored_of(vertices[index].uuid);

    // Every attempt reads the adjacency again through its own transaction, so a retry finds the edges as they now are
    return serve_write(
        state, view, arguments.durability, count, count * graph::half_edge_delta_bytes_k, graph_write_schedule(view),
        [&](call_access<cpu_class_> const& access) noexcept -> status_t {
            if (status_t const read = read_task_vertices(access, collections, keys, reads); read.failed()) return read;
            st::basic_vector<vertex_delta_t, arena_allocator<vertex_delta_t>> deltas {{access.scratch}};
            for (std::size_t index = 0; index != count; ++index) {
                if (!reads[index].present) continue;
                ustore_key_t const vertex = vertices[index];
                role_t const wanted = half_edge_roles_of(roles ? roles[index] : ustore_vertex_role_any_k);
                expected<vertex_adjacency_t> const adjacency = vertex_adjacency_t::make(vertex, reads[index].base,
                                                                                        reads[index].pending);
                if (!adjacency) return adjacency.status();
                for (half_edge_cursor_t cursor = adjacency->half_edges(); cursor.valid(); cursor.next()) {
                    graph::labeled_half_edge_t const half_edge = cursor.current();
                    if (!(half_edge.role & wanted) || half_edge.neighbor_id == vertex) continue;
                    half_edge_delta_t const removal = half_edge_delta_t::remove(vertex, half_edge.edge_id,
                                                                                mirrored_role(half_edge.role));
                    vertex_delta_t const filed {task_collection(collections, index), half_edge.neighbor_id, removal,
                                                deltas.size()};
                    if (status_t const kept = growth_status(deltas.push_back(vertex_delta_t {filed})); kept.failed())
                        return kept;
                }
            }
            st::basic_vector<vertex_chain_t, arena_allocator<vertex_chain_t>> chains {{access.scratch}};
            if (status_t const chained = chain_by_vertex(std::span<vertex_delta_t> {deltas}, access.scratch, chains);
                chained.failed())
                return chained;
            for (vertex_chain_t const& chain : chains) {
                collection_id_t const collection = chain.collection;
                stored_key_t const key = stored_of(chain.vertex.uuid);
                if (!visible_vertex(access, collection, key)) continue;
                if (status_t const watched = watch_vertex(access, collection, key); watched.failed()) return watched;
                if (status_t const appended = append_chain(access, chain); appended.failed()) return appended;
            }
            // The removals go last, so one outranks a change this batch appended to a vertex it also removes
            for (std::size_t index = 0; index != count; ++index) {
                if (!reads[index].present) continue;
                if (status_t const removed = write_value(access, task_collection(collections, index), keys[index],
                                                         value_view_t {});
                    removed.failed())
                    return removed;
            }
            return {};
        });
}

#pragma endregion Graph Calls

} // namespace unum::ustore::nvme
