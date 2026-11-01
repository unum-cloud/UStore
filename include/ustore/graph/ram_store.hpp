/**
 *  @file include/ustore/graph/ram_graph.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The graph algorithms over a store of half-edges: walking a vertex's edges, exporting degrees and edges,
 *      and staging edge and vertex inserts and removals into a transaction.
 *
 *  Every template here takes the view or the transaction part of a @c half_edges_store, and the batch of tasks or
 *  edges in whatever form the caller keeps them, as long as indexing it answers a task with @c vertex_id and @c role,
 *  or an @c edge_t.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`
#include <cstring> // `std::memcpy`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/blobs/ram_store.hpp"       // `store_status_t`, `walk_control_t`, `translate`
#include "ustore/graph/half_edges.hpp"      // `half_edge_t`, `half_edge_direction_t`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocator`
#include "ustore/utilities/types.hpp"       // `edge_t`, `status_t`

namespace unum::ustore::graph {

using blobs::store_status_t;
using blobs::translate;
using blobs::walk_control_t;
using utilities::arena_allocator;
using utilities::edge_t;
using utilities::status_t;

/** Walks the entries of @p vertex in @p direction through @p view, handing @p visit every one. */
template <typename view_type_, typename visit_type_>
store_status_t walk_direction(view_type_ const& view, ustore_key_t const& vertex, half_edge_direction_t direction,
                              visit_type_&& visit) noexcept {
    auto const next = static_cast<half_edge_direction_t>(static_cast<std::uint8_t>(direction) + 1);
    return view.range(half_edge_t::first_of(vertex, direction), half_edge_t::first_of(vertex, next), visit);
}

/** Exports the degree of tasks [ @p first, @p end ) through @p view of a graph into @p degrees, with their edges as
 *  source, target and edge triples when @p edges is given, watching what it walked when the view watches. */
template <typename view_type_, typename tasks_type_, typename degrees_type_>
store_status_t export_edges(view_type_ const& view, tasks_type_ const& tasks, std::size_t first, std::size_t end,
                            degrees_type_& degrees,
                            st::basic_vector<ustore_key_t, arena_allocator<ustore_key_t>>* edges,
                            st::basic_vector<half_edge_t, arena_allocator<half_edge_t>>& walked,
                            status_t& outcome) noexcept {
    std::size_t const walked_before = walked.size();
    for (std::size_t task = first; task != end && outcome.succeeded(); ++task) {
        auto const asked = tasks[task];
        bool present = false;
        store_status_t const looked_up = view.find(
            half_edge_t::vertex_entry(asked.vertex_id), [&](half_edge_t const&) noexcept { present = true; },
            st::no_op_t {});
        if (st::failed(looked_up)) return looked_up;
        if (!present) {
            degrees[task] = ustore_vertex_degree_missing_k;
            continue;
        }
        ustore_vertex_degree_t degree = 0;
        auto const visit = [&](half_edge_t const& entry) noexcept -> walk_control_t {
            ++degree;
            if (view.watches()) outcome = translate(walked.emplace_back(entry));
            if (edges && outcome.succeeded()) {
                bool const outgoing = entry.direction == half_edge_direction_t::outgoing_k;
                ustore_key_t const triple[3] {outgoing ? entry.vertex : entry.neighbor,
                                              outgoing ? entry.neighbor : entry.vertex, entry.edge};
                std::size_t const exported = edges->size();
                outcome = translate(edges->resize(exported + 3));
                if (outcome.succeeded()) std::memcpy(edges->data() + exported, triple, sizeof(triple));
            }
            return outcome.failed() ? walk_control_t::halt_k : walk_control_t::resume_k;
        };
        if (asked.role & ustore_vertex_role_source_k) {
            store_status_t const outgoing = walk_direction(view, asked.vertex_id, half_edge_direction_t::outgoing_k,
                                                           visit);
            if (st::failed(outgoing)) return outgoing;
        }
        if (asked.role & ustore_vertex_role_target_k) {
            store_status_t const incoming = walk_direction(view, asked.vertex_id, half_edge_direction_t::incoming_k,
                                                           visit);
            if (st::failed(incoming)) return incoming;
        }
        degrees[task] = degree;
    }
    for (std::size_t index = walked_before; index != walked.size() && outcome.succeeded(); ++index) {
        store_status_t const watched = view.watch(walked[index]);
        if (st::failed(watched)) return watched;
    }
    return store_status_t::success_k;
}

/** Stages @p vertex into @p part of a graph unless it already is one, which never writes over a present vertex, so
 *  edges added to one vertex by two transactions don't conflict over it. */
template <typename transaction_type_>
store_status_t stage_vertex(transaction_type_& part, ustore_key_t const& vertex) noexcept {
    return part.insert_if_missing(half_edge_t::vertex_entry(vertex));
}

/** Stages edges [ @p first, @p end ) into @p part of a graph, under both of their ends, and each end as a vertex. */
template <typename transaction_type_, typename edges_type_>
store_status_t stage_edges(transaction_type_& part, edges_type_ const& edges, std::size_t first,
                           std::size_t end) noexcept {
    for (std::size_t index = first; index != end; ++index) {
        edge_t const edge = edges[index];
        store_status_t status = stage_vertex(part, edge.source);
        if (st::succeeded(status)) status = stage_vertex(part, edge.target);
        if (st::succeeded(status))
            status = part.upsert(half_edge_t {edge.source, half_edge_direction_t::outgoing_k, edge.target, edge.id});
        if (st::succeeded(status))
            status = part.upsert(half_edge_t {edge.target, half_edge_direction_t::incoming_k, edge.source, edge.id});
        if (st::failed(status)) return status;
    }
    return store_status_t::success_k;
}

/** Stages erasing @p entry from @p part, which may be gone already. */
template <typename transaction_type_>
store_status_t stage_absent(transaction_type_& part, half_edge_t const& entry) noexcept {
    store_status_t const erased = part.erase(entry);
    return erased == store_status_t::key_not_found_k ? store_status_t::success_k : erased;
}

/** Stages erasing edges [ @p first, @p end ) from @p part of a graph, under both of their ends, keeping the
 *  vertices. */
template <typename transaction_type_, typename edges_type_>
store_status_t stage_edges_removal(transaction_type_& part, edges_type_ const& edges, std::size_t first,
                                   std::size_t end) noexcept {
    for (std::size_t index = first; index != end; ++index) {
        edge_t const edge = edges[index];
        store_status_t status = stage_absent(
            part, half_edge_t {edge.source, half_edge_direction_t::outgoing_k, edge.target, edge.id});
        if (st::succeeded(status))
            status = stage_absent(part,
                                  half_edge_t {edge.target, half_edge_direction_t::incoming_k, edge.source, edge.id});
        if (st::failed(status)) return status;
    }
    return store_status_t::success_k;
}

/** Stages removing @p vertex from @p part of a graph: the edges its neighbors file of it in @p role, then every entry
 *  of its own. */
template <typename transaction_type_>
store_status_t stage_vertex_removal(transaction_type_& part, ustore_key_t const& vertex, ustore_vertex_role_t role,
                                    st::basic_vector<half_edge_t, arena_allocator<half_edge_t>>& walked) noexcept {
    std::size_t const first = walked.size();
    store_status_t collected = store_status_t::success_k;
    auto const collect = [&](half_edge_t const& entry) noexcept -> walk_control_t {
        if (st::succeeded(collected)) collected = walked.emplace_back(entry);
        return st::failed(collected) ? walk_control_t::halt_k : walk_control_t::resume_k;
    };
    if (role & ustore_vertex_role_source_k) {
        store_status_t const outgoing = walk_direction(part, vertex, half_edge_direction_t::outgoing_k, collect);
        if (st::failed(outgoing)) return outgoing;
    }
    if (role & ustore_vertex_role_target_k) {
        store_status_t const incoming = walk_direction(part, vertex, half_edge_direction_t::incoming_k, collect);
        if (st::failed(incoming)) return incoming;
    }
    if (st::failed(collected)) return collected;
    for (std::size_t index = first; index != walked.size(); ++index) {
        half_edge_t const seen = walked[index];
        half_edge_direction_t const mirrored = seen.direction == half_edge_direction_t::outgoing_k
                                                   ? half_edge_direction_t::incoming_k
                                                   : half_edge_direction_t::outgoing_k;
        store_status_t const erased = stage_absent(part, half_edge_t {seen.neighbor, mirrored, vertex, seen.edge});
        if (st::failed(erased)) return erased;
    }
    return part.erase_range(half_edge_t::vertex_entry(vertex),
                            half_edge_t::first_of(vertex, half_edge_direction_t::past_k), st::no_op_t {});
}

} // namespace unum::ustore::graph
