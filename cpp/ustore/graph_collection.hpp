/**
 *  @file cpp/ustore/graph_collection.hpp
 *  @author Ash Vardanian
 *  @date June 30, 2022
 *  @brief Handles to one graph collection with typed vertex and edge operations, whose verbs block
 *      or answer deferred calls as their receiver chose.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <algorithm> // `std::ranges::equal_range`
#include <span>      // `std::span`
#include <utility>   // `std::move`, `std::swap`

#include "ustore/blobs_range.hpp" // `blobs_range_t`, `keys_range_t`, `keys_stream_t`, `sample_keys_`, `measure_keys_`
#include "ustore/graph.h"
#include "ustore/graph_stream.hpp" // `graph_stream_t`, `graph_read_edges_`, `degrees_view_t`
#include "ustore/ranges.hpp"       // `range`, `edges_view_t`, `bits_view_t`, `sort_and_deduplicate`
#include "ustore/status.hpp"       // `status_t`, `expected`, `answer`, `called_`, `defers`
#include "ustore/types.hpp"        // `arena_t`, `in_one_off_arena`, `receiver_t`, `edge_t`, `collection_access_t`

namespace unum::ustore {

#pragma region Verbs

/** Writes @p edges into @p collection, between existing or new vertices. */
template <ustore_completion_t completion_>
answer<completion_, void> graph_write_edges_(receiver_t const& receiver, ustore_collection_id_t const& collection,
                                             edges_view_t const& edges, ustore_durability_t durability) noexcept {
    return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_graph_write_edges_t write {};
        write.database = receiver.database;
        write.executor = receiver.executor;
        write.outcome = outcome;
        write.completion = completion_;
        write.transaction = receiver.transaction;
        write.count = edges.size();
        write.collections = {&collection, 0};
        write.edges = {edges.ids.data(), edges.ids.stride()};
        write.sources = {edges.sources.data(), edges.sources.stride()};
        write.targets = {edges.targets.data(), edges.targets.stride()};
        write.durability = durability;
        ustore_graph_write_edges(&write);
    });
}

/** Removes @p edges from @p collection, keeping their vertices. */
template <ustore_completion_t completion_>
answer<completion_, void> graph_remove_edges_(receiver_t const& receiver, ustore_collection_id_t const& collection,
                                              edges_view_t const& edges, ustore_durability_t durability) noexcept {
    return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_graph_remove_edges_t remove {};
        remove.database = receiver.database;
        remove.executor = receiver.executor;
        remove.outcome = outcome;
        remove.completion = completion_;
        remove.transaction = receiver.transaction;
        remove.count = edges.size();
        remove.collections = {&collection, 0};
        remove.edges = {edges.ids.data(), edges.ids.stride()};
        remove.sources = {edges.sources.data(), edges.sources.stride()};
        remove.targets = {edges.targets.data(), edges.targets.stride()};
        remove.durability = durability;
        ustore_graph_remove_edges(&remove);
    });
}

/** Writes @p vertices into @p collection without edges, keeping those already present. */
template <ustore_completion_t completion_>
answer<completion_, void> graph_write_vertices_(receiver_t const& receiver, ustore_collection_id_t const& collection,
                                                strided_span<ustore_key_t const> vertices,
                                                ustore_durability_t durability) noexcept {
    return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_graph_write_vertices_t write {};
        write.database = receiver.database;
        write.executor = receiver.executor;
        write.outcome = outcome;
        write.completion = completion_;
        write.transaction = receiver.transaction;
        write.count = vertices.size();
        write.collections = {&collection, 0};
        write.vertices = {vertices.data(), vertices.stride()};
        write.durability = durability;
        ustore_graph_write_vertices(&write);
    });
}

/** Removes @p vertices from @p collection together with every edge they play @p roles in. */
template <ustore_completion_t completion_>
answer<completion_, void> graph_remove_vertices_(receiver_t const& receiver, ustore_collection_id_t const& collection,
                                                 strided_span<ustore_key_t const> vertices,
                                                 strided_span<ustore_vertex_role_t const> roles,
                                                 ustore_durability_t durability) noexcept {
    return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_graph_remove_vertices_t remove {};
        remove.database = receiver.database;
        remove.executor = receiver.executor;
        remove.outcome = outcome;
        remove.completion = completion_;
        remove.transaction = receiver.transaction;
        remove.count = vertices.count();
        remove.collections = {&collection, 0};
        remove.vertices = {vertices.data(), vertices.stride()};
        remove.roles = {roles.data(), roles.stride()};
        remove.durability = durability;
        ustore_graph_remove_vertices(&remove);
    });
}

#pragma endregion Verbs

/**
 *  @brief A handle to one graph collection, stored as an inverted index from every vertex to its
 *      edges, cheap to construct and to copy, whose verbs answer as @p completion_ says.
 *
 *  A blocking handle answers an @c outcome, while a deferred one answers a @c deferred call,
 *  borrowing the handle, its inputs and its arena until the call is final. A handle with
 *  @c collection_access_t::read_only_k only reads, as a snapshot hands out. It holds no state of
 *  its own, so any number of threads share it, at the head, inside a transaction or at a snapshot.
 */
template <ustore_completion_t completion_, collection_access_t access_>
class graph_collection_on {
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};

    static constexpr bool blocks_k = !defers<completion_>;
    static constexpr bool writes_k = access_ == collection_access_t::read_write_k;

  public:
    using adjacency_range_t = range<graph_stream_t>;

    /** The handle a snapshot hands out for this kind of collection, which only reads. */
    using read_only_t = graph_collection_on<completion_, collection_access_t::read_only_k>;

    /** The same kind of handle, completing its verbs as @p other_completion_ says. */
    template <ustore_completion_t other_completion_>
    using completed_as = graph_collection_on<other_completion_, access_>;

    /** What the collections this kind of handle addresses hold, which a creation records and a
     *  lookup checks. */
    static constexpr ustore_modality_t modality_k = ustore_modality_graph_k;

    graph_collection_on() noexcept = default;
    graph_collection_on(receiver_t receiver, ustore_collection_id_t collection) noexcept
        : receiver_(receiver), collection_(collection) {}

    operator ustore_collection_id_t() const noexcept { return collection_; }
    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }
    receiver_t receiver() const noexcept { return receiver_; }

    /** The same collection, whose verbs block on @p executor, or on the calling
     *  thread without one. */
    completed_as<ustore_completion_blocking_k> on(ustore_executor_t executor) const noexcept {
        return {receiver_.on(executor), collection_};
    }

    /** The same collection, whose verbs submit to @p executor and answer deferred calls. */
    completed_as<ustore_completion_deferred_k> deferred(ustore_executor_t executor) const noexcept {
        return {receiver_.on(executor), collection_};
    }

#pragma region Verbs

    /**
     *  @brief Reads the degree of every vertex of @p vertices into @p arena.
     *  @param[in] vertices Vertices whose edges are counted, borrowed until the call is final.
     *  @param[in] arena Memory the degrees land in, borrowed until the call is final, which
     *      the answer views.
     *  @param[in] roles Role every vertex plays in the counted edges, empty for any, borrowed until
     *      the call is final.
     *  @param[in] watch Whether a read inside a transaction records the vertices for conflicts.
     */
    answer<completion_, degrees_view_t> degrees(strided_span<ustore_key_t const> vertices, arena_t& arena,
                                                strided_span<ustore_vertex_role_t const> roles = {},
                                                ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return graph_read_edges_<completion_, degrees_view_t>(receiver_, collection_, vertices, roles, arena, watch);
    }

    /**
     *  @brief Reads every edge that has any of @p vertices in an allowed role into @p arena,
     *      split by vertex through the offsets of the answer, where undirected graphs may repeat
     *      an edge reversed.
     *
     *  @param[in] vertices Vertices whose edges are read, borrowed until the call is final.
     *  @param[in] arena Memory the edges land in, borrowed until the call is final, which
     *      the answer views.
     *  @param[in] roles Role every vertex plays in the wanted edges, empty for any, borrowed until
     *      the call is final.
     *  @param[in] watch Whether a read inside a transaction records the vertices for conflicts.
     */
    answer<completion_, edges_view_t> read_edges(strided_span<ustore_key_t const> vertices, arena_t& arena,
                                                 strided_span<ustore_vertex_role_t const> roles = {},
                                                 ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return graph_read_edges_<completion_, edges_view_t>(receiver_, collection_, vertices, roles, arena, watch);
    }

    /** Writes @p edges, borrowed until the call is final, between existing or new vertices. */
    answer<completion_, void> write_edges(edges_view_t const& edges,
                                          ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        return graph_write_edges_<completion_>(receiver_, collection_, edges, durability);
    }

    /** Removes @p edges, borrowed until the call is final, keeping their vertices. */
    answer<completion_, void> remove_edges(edges_view_t const& edges,
                                           ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        return graph_remove_edges_<completion_>(receiver_, collection_, edges, durability);
    }

    /** Writes @p vertices, borrowed until the call is final, without edges, keeping those already
     *  present. */
    answer<completion_, void> write_vertices(
        strided_span<ustore_key_t const> vertices,
        ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        return graph_write_vertices_<completion_>(receiver_, collection_, vertices, durability);
    }

    /**
     *  @brief Removes @p vertices together with every edge they belong to.
     *  @param[in] vertices Vertices to remove, borrowed until the call is final.
     *  @param[in] roles Role every vertex plays, which only joining graphs need, borrowed until the
     *      call is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    answer<completion_, void> remove_vertices(
        strided_span<ustore_key_t const> vertices, strided_span<ustore_vertex_role_t const> roles = {},
        ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        return graph_remove_vertices_<completion_>(receiver_, collection_, vertices, roles, durability);
    }

    /**
     *  @brief Draws up to @p wanted vertices at random into @p arena.
     *  @param[in] arena Memory the vertices land in, which the answer views, borrowed until the
     *      call is final.
     *  @param[in] watch Whether a read inside a transaction records the vertices for conflicts.
     */
    answer<completion_, std::span<ustore_key_t const>> sample_vertices(
        std::size_t wanted, arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return sample_keys_<completion_, ustore_graph_sample_vertices_t, &ustore_graph_sample_vertices>(
            receiver_, collection_, wanted, arena, watch);
    }

    /**
     *  @brief Estimates the vertices and the space from @p start_key, inclusive, to @p end_key,
     *      exclusive, into @p arena.
     *  @param[in] start_key First vertex of the range, borrowed until the call is final.
     *  @param[in] end_key Vertex the range stops before, borrowed until the call is final.
     *  @param[in] arena Memory the bounds land in, borrowed until the call is final.
     *  @param[in] watch Whether a read inside a transaction records the range for conflicts.
     */
    answer<completion_, size_estimates_t> measure_vertices(ustore_key_t const& start_key, ustore_key_t const& end_key,
                                                           arena_t& arena,
                                                           ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return measure_keys_<completion_, ustore_graph_measure_vertices_t, &ustore_graph_measure_vertices>(
            receiver_, collection_, start_key, end_key, arena, watch);
    }

    /** Removes every edge but keeps the vertices. */
    answer<completion_, void> clear_values() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_values_k);
    }

    /** Removes every vertex and edge, keeping the collection. */
    answer<completion_, void> clear() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_pairs_k);
    }

    /** Removes the collection with its name and its whole graph. */
    answer<completion_, void> drop() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_collection_k);
    }

#pragma endregion Verbs

#pragma region Shorthand

    /** Reads the degree of @p vertex in @p role through a one-off arena. */
    expected<ustore_vertex_degree_t> degree( //
        ustore_key_t vertex, ustore_vertex_role_t role = ustore_vertex_role_any_k,
        ustore_watch_t watch = ustore_watch_keys_k) const noexcept
        requires blocks_k
    {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept -> expected<ustore_vertex_degree_t> {
            auto [found, degrees_status] = degrees({{&vertex}, 1}, arena, {{&role}, 1}, watch);
            if (degrees_status.failed()) return degrees_status;
            return found[0];
        });
    }

    /** Checks whether @p vertex is present through a one-off arena, including a vertex without
     *  edges, as a vertex whose degree is known. */
    expected<bool> present(ustore_key_t vertex, ustore_watch_t watch = ustore_watch_keys_k) const noexcept
        requires blocks_k
    {
        auto [found, degree_status] = degree(vertex, ustore_vertex_role_any_k, watch);
        if (degree_status.failed()) return degree_status;
        return found != ustore_vertex_degree_missing_k;
    }

    /**
     *  @brief Marks in @p presences, one bit per vertex, which of @p vertices are present,
     *      including those without any edges, reading their degrees into @p arena.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The @p presences hold fewer bits than @p vertices.
     */
    expected<bits_view_t> present( //
        strided_span<ustore_key_t const> const& vertices, std::span<ustore_octet_t> presences, arena_t& arena,
        ustore_watch_t watch = ustore_watch_keys_k) const noexcept
        requires blocks_k
    {
        if (presences.size() * bits_in_byte_k < vertices.size())
            return status_t {status_code_t::invalid_argument_k, "Need a bit of presence for every vertex"};
        auto [found, degrees_status] = degrees(vertices, arena, {}, watch);
        if (degrees_status.failed()) return degrees_status;
        for (ustore_octet_t& octet : presences) octet = 0;
        for (std::size_t index = 0; index != found.size(); ++index)
            if (found[index] != ustore_vertex_degree_missing_k)
                presences[index / bits_in_byte_k] |= static_cast<ustore_octet_t>(1u << (index % bits_in_byte_k));
        return bits_view_t {presences.data()};
    }

    expected<keys_stream_t> vertex_stream(
        std::size_t vertices_read_ahead = keys_stream_t::default_read_ahead_k) const noexcept
        requires blocks_k
    {
        blobs_range_t members(receiver_, collection_, smallest_key_k, largest_key_k, modality_k);
        return members.keys_begin(vertices_read_ahead);
    }

    /** Counts the vertices by streaming through all of them. */
    expected<std::size_t> size() const noexcept
        requires blocks_k
    {
        return keys_range_t {blobs_range_t {receiver_, collection_, smallest_key_k, largest_key_k, modality_k}}.size();
    }

    /** Counts the edges by streaming through the outgoing edges of every vertex. */
    expected<std::size_t> number_of_edges() const noexcept
        requires blocks_k
    {
        auto [stream, stream_status] = graph_stream_t::make(receiver_, collection_, ustore_vertex_role_source_k,
                                                            smallest_key_k);
        if (stream_status.failed()) return stream_status;

        std::size_t count = 0;
        for (; !stream.is_end(); ++stream) ++count;
        return count;
    }

    expected<adjacency_range_t> edges(
        ustore_vertex_role_t role = ustore_vertex_role_any_k,
        std::size_t vertices_read_ahead = keys_stream_t::default_read_ahead_k) const noexcept
        requires blocks_k
    {
        auto [begin, begin_status] = graph_stream_t::make(receiver_, collection_, role, smallest_key_k,
                                                          vertices_read_ahead);
        if (begin_status.failed()) return begin_status;
        auto [end, end_status] = graph_stream_t::make(receiver_, collection_, role, ustore_key_unknown_k,
                                                      vertices_read_ahead);
        if (end_status.failed()) return end_status;
        return adjacency_range_t {std::move(begin), std::move(end)};
    }

    /** Reads every edge that has @p vertex in @p role into @p arena. */
    expected<edges_view_t> read_edges( //
        ustore_key_t vertex, arena_t& arena, ustore_vertex_role_t role = ustore_vertex_role_any_k,
        ustore_watch_t watch = ustore_watch_keys_k) const noexcept
        requires blocks_k
    {
        return read_edges({{&vertex}, 1}, arena, {{&role}, 1}, watch);
    }

    /** Reads the edges from @p source to @p target into @p arena. */
    expected<edges_view_t> edges_between(ustore_key_t source, ustore_key_t target, arena_t& arena,
                                         ustore_watch_t watch = ustore_watch_keys_k) const noexcept
        requires blocks_k
    {
        auto [outgoing, read_edges_status] = read_edges(source, arena, ustore_vertex_role_source_k, watch);
        if (read_edges_status.failed()) return read_edges_status;

        auto const matches = std::ranges::equal_range(outgoing.targets.begin(), outgoing.targets.end(), target);
        std::size_t const offset = static_cast<std::size_t>(matches.begin() - outgoing.targets.begin());
        std::size_t const count = static_cast<std::size_t>(matches.end() - matches.begin());
        return edges_view_t {outgoing.sources.subspan(offset, count), outgoing.targets.subspan(offset, count),
                             outgoing.ids.subspan(offset, count)};
    }

    /** Reads the targets of the edges leaving @p vertex into @p arena. */
    expected<strided_span<ustore_key_t const>> successors(ustore_key_t vertex, arena_t& arena) const noexcept
        requires blocks_k
    {
        auto [edges, read_edges_status] = read_edges(vertex, arena, ustore_vertex_role_source_k);
        if (read_edges_status.failed()) return read_edges_status;
        return edges.targets;
    }

    /** Reads the sources of the edges entering @p vertex into @p arena. */
    expected<strided_span<ustore_key_t const>> predecessors(ustore_key_t vertex, arena_t& arena) const noexcept
        requires blocks_k
    {
        auto [edges, read_edges_status] = read_edges(vertex, arena, ustore_vertex_role_target_k);
        if (read_edges_status.failed()) return read_edges_status;
        return edges.sources;
    }

    /** Lists the distinct neighbors of @p vertex in @p arena, reordering the edges it fetched there
     *  in place. */
    expected<strided_span<ustore_key_t const>> neighbors(
        ustore_key_t vertex, arena_t& arena, ustore_vertex_role_t role = ustore_vertex_role_any_k) const noexcept
        requires blocks_k
    {
        auto [fetched, read_edges_status] = read_edges(vertex, arena, role);
        if (read_edges_status.failed()) return read_edges_status;

        // The arena belongs to the caller, so the edges it holds are reordered where they landed
        edge_t* const begin = const_cast<edge_t*>(reinterpret_cast<edge_t const*>(fetched.sources.data()));
        edges_span<ustore_key_t> const edges {begin, begin + fetched.size()};

        // Incoming and outgoing edges hold the neighbor in different columns, so each is flipped
        for (std::size_t index = 0; index != edges.size(); ++index) {
            ustore_key_t& source = edges.sources[index];
            ustore_key_t& target = edges.targets[index];
            if (target == vertex) std::swap(source, target);
        }

        strided_span<ustore_key_t> const targets = edges.targets;
        std::size_t const count = sort_and_deduplicate(targets.begin(), targets.end());
        return strided_span<ustore_key_t const> {{targets.data(), targets.stride()}, count};
    }

    status_t write_vertex(ustore_key_t const vertex) const noexcept
        requires(blocks_k && writes_k)
    {
        return write_vertices({{&vertex}, 1});
    }

    status_t write_edge(edge_t const& edge) const noexcept
        requires(blocks_k && writes_k)
    {
        return write_edges(edges_view_t {&edge, &edge + 1});
    }

    status_t remove_vertex( //
        ustore_key_t const vertex, ustore_vertex_role_t const role = ustore_vertex_role_any_k,
        ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires(blocks_k && writes_k)
    {
        return remove_vertices({{&vertex}, 1}, {{&role}, 1}, durability);
    }

    status_t remove_edge(edge_t const& edge) const noexcept
        requires(blocks_k && writes_k)
    {
        return remove_edges(edges_view_t {&edge, &edge + 1});
    }

#pragma endregion Shorthand
};

/** A graph collection whose verbs block, reading and writing the head or a transaction. */
using graph_collection_t = graph_collection_on<ustore_completion_blocking_k, collection_access_t::read_write_k>;

/** A graph collection whose verbs block and only read, as a snapshot hands out. */
using graph_read_collection_t = graph_collection_on<ustore_completion_blocking_k, collection_access_t::read_only_k>;

} // namespace unum::ustore
