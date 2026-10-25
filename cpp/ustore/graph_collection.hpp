/**
 *  @file cpp/ustore/graph_collection.hpp
 *  @author Ash Vardanian
 *  @date June 30, 2022
 *  @brief Handles to one graph collection with typed vertex and edge operations, and their deferred twins, whose verbs
 *      submit to an executor.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <algorithm>   // `std::ranges::equal_range`
#include <span>        // `std::span`
#include <type_traits> // `std::is_same_v`
#include <utility>     // `std::forward`, `std::move`, `std::swap`

#include "ustore/blobs_range.hpp" // `blobs_range_t`, `keys_range_t`, `keys_stream_t`
#include "ustore/graph.h"
#include "ustore/graph_stream.hpp" // `graph_stream_t`
#include "ustore/ranges.hpp"       // `range`, `strided`, `exported_edges`, `bits_view_t`, `owned_array_concept`
#include "ustore/status.hpp"       // `status_t`, `expected`, `deferred`
#include "ustore/types.hpp"        // `arena_t`, `in_one_off_arena`, `receiver_t`, `collection_key_field_t`, `edge_t`

namespace unum::ustore {

class deferred_graph_read_collection_t;
class deferred_graph_collection_t;

#pragma region Collections

/** A handle to one graph collection that only reads, stored as an inverted index from every vertex to its edges,
 *  cheap to construct and to copy. It holds no state of its own, so any number of threads share it, at the head,
 *  inside a transaction or at a snapshot. */
class graph_read_collection_t {
  protected:
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};

    /** Reads the degrees of @p vertices into @p arena, with the offsets and edges unless @p edges is null. */
    status_t read_edges_into_(strided_span<ustore_key_t const> vertices, strided_span<ustore_vertex_role_t const> roles,
                              arena_t& arena, ustore_watch_t watch, ustore_vertex_degree_t const** degrees,
                              ustore_length_t const** offsets, ustore_key_t const** edges) noexcept {
        ustore_outcome_t outcome {};
        ustore_graph_read_edges_t graph_read_edges {};
        graph_read_edges.database = receiver_.database;
        graph_read_edges.executor = receiver_.executor;
        graph_read_edges.outcome = &outcome;
        graph_read_edges.view = receiver_.view(watch);
        graph_read_edges.count = vertices.count();
        graph_read_edges.collections = {&collection_, 0};
        graph_read_edges.vertices = {vertices.data(), vertices.stride()};
        graph_read_edges.roles = {roles.data(), roles.stride()};
        graph_read_edges.degrees_per_vertex = degrees;
        graph_read_edges.edges_offsets = offsets;
        graph_read_edges.edges_per_vertex = edges;
        graph_read_edges.arena = arena;

        ustore_graph_read_edges(&graph_read_edges);
        return status_t {outcome};
    }

  public:
    using adjacency_range_t = range<graph_stream_t>;

    /** The handle a snapshot hands out for this kind of collection, which only reads. */
    using read_only_t = graph_read_collection_t;

    /** The twin a deferred receiver hands out, whose verbs answer deferred calls. */
    using deferred_t = deferred_graph_read_collection_t;

    /** What the collections this kind of handle addresses hold, which a creation records and a lookup checks. */
    static constexpr ustore_modality_t modality_k = ustore_modality_graph_k;

    graph_read_collection_t() noexcept = default;
    graph_read_collection_t(receiver_t receiver, ustore_collection_id_t collection) noexcept
        : receiver_(receiver), collection_(collection) {}

    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }

    /** Reads the degree of @p vertex in @p role through a one-off arena. */
    expected<ustore_vertex_degree_t> degree( //
        ustore_key_t vertex, ustore_vertex_role_t role = ustore_vertex_role_any_k,
        ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept -> expected<ustore_vertex_degree_t> {
            auto [found, degrees_status] = degrees({{&vertex}, 1}, arena, {{&role}, 1}, watch);
            if (degrees_status.failed()) return degrees_status;
            return found[0];
        });
    }

    /** Reads the degree of every vertex into @p arena. */
    expected<std::span<ustore_vertex_degree_t const>> degrees( //
        strided_span<ustore_key_t const> vertices, arena_t& arena, strided_span<ustore_vertex_role_t const> roles = {},
        ustore_watch_t watch = ustore_watch_keys_k) noexcept {

        ustore_vertex_degree_t const* found = nullptr;
        if (status_t const status = read_edges_into_(vertices, roles, arena, watch, &found, nullptr, nullptr);
            status.failed())
            return status;
        return std::span<ustore_vertex_degree_t const> {found, found ? vertices.size() : 0};
    }

    /** Checks whether @p vertex is present through a one-off arena, including a vertex without edges, as a vertex whose
     *  degree is known. */
    expected<bool> present(ustore_key_t vertex, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        auto [found, degree_status] = degree(vertex, ustore_vertex_role_any_k, watch);
        if (degree_status.failed()) return degree_status;
        return found != ustore_vertex_degree_missing_k;
    }

    /** Marks in @p presences, one bit per vertex, which of @p vertices are present, including those without any edges,
     *  reading their degrees into @p arena. */
    expected<bits_view_t> present( //
        strided_span<ustore_key_t const> const& vertices, std::span<ustore_octet_t> presences, arena_t& arena,
        ustore_watch_t watch = ustore_watch_keys_k) noexcept {
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
        std::size_t vertices_read_ahead = keys_stream_t::default_read_ahead_k) const noexcept {
        blobs_range_t members(receiver_, collection_, smallest_key_k, largest_key_k, modality_k);
        return members.keys_begin(vertices_read_ahead);
    }

    /** Counts the vertices by streaming through all of them. */
    expected<std::size_t> size() noexcept {
        return keys_range_t {blobs_range_t {receiver_, collection_, smallest_key_k, largest_key_k, modality_k}}.size();
    }

    /** Counts the edges by streaming through the outgoing edges of every vertex. */
    expected<std::size_t> number_of_edges() noexcept {
        auto [stream, stream_status] = graph_stream_t::make(receiver_, collection_, ustore_vertex_role_source_k,
                                                            smallest_key_k);
        if (stream_status.failed()) return stream_status;

        std::size_t count = 0;
        for (; !stream.is_end(); ++stream) ++count;
        return count;
    }

    expected<adjacency_range_t> edges(
        ustore_vertex_role_t role = ustore_vertex_role_any_k,
        std::size_t vertices_read_ahead = keys_stream_t::default_read_ahead_k) const noexcept {

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
        ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return read_edges({{&vertex}, 1}, arena, {{&role}, 1}, watch);
    }

    /** Reads the edges from @p source to @p target into @p arena. */
    expected<edges_view_t> edges_between(ustore_key_t source, ustore_key_t target, arena_t& arena,
                                         ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        auto [all, read_edges_status] = read_edges(source, arena, ustore_vertex_role_source_k, watch);
        if (read_edges_status.failed()) return read_edges_status;

        auto const matches = std::ranges::equal_range(all.targets.begin(), all.targets.end(), target);
        std::size_t const offset = static_cast<std::size_t>(matches.begin() - all.targets.begin());
        std::size_t const count = static_cast<std::size_t>(matches.end() - matches.begin());
        return edges_view_t {all.sources.subspan(offset, count), all.targets.subspan(offset, count),
                             all.ids.subspan(offset, count)};
    }

    /** Reads every edge that has any of @p vertices in an allowed role into @p arena, split by vertex through the
     *  offsets of the answer, where undirected graphs may repeat an edge reversed. */
    expected<edges_view_t> read_edges( //
        strided_span<ustore_key_t const> vertices, arena_t& arena, strided_span<ustore_vertex_role_t const> roles = {},
        ustore_watch_t watch = ustore_watch_keys_k) noexcept {

        ustore_vertex_degree_t const* degrees = nullptr;
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* edges = nullptr;
        if (status_t const status = read_edges_into_(vertices, roles, arena, watch, &degrees, &offsets, &edges);
            status.failed())
            return status;
        return exported_edges(edges, offsets, vertices.size());
    }

    /** Reads the targets of the edges leaving @p vertex into @p arena. */
    expected<strided_span<ustore_key_t const>> successors(ustore_key_t vertex, arena_t& arena) noexcept {
        auto [edges, read_edges_status] = read_edges(vertex, arena, ustore_vertex_role_source_k);
        if (read_edges_status.failed()) return read_edges_status;
        return edges.targets;
    }

    /** Reads the sources of the edges entering @p vertex into @p arena. */
    expected<strided_span<ustore_key_t const>> predecessors(ustore_key_t vertex, arena_t& arena) noexcept {
        auto [edges, read_edges_status] = read_edges(vertex, arena, ustore_vertex_role_target_k);
        if (read_edges_status.failed()) return read_edges_status;
        return edges.sources;
    }

    /** Lists the distinct neighbors of @p vertex in @p arena, reordering the edges it fetched there in place. */
    expected<strided_span<ustore_key_t const>> neighbors(
        ustore_key_t vertex, arena_t& arena, ustore_vertex_role_t role = ustore_vertex_role_any_k) noexcept {
        auto [fetched, read_edges_status] = read_edges(vertex, arena, role);
        if (read_edges_status.failed()) return read_edges_status;

        // The arena belongs to the caller, so the edges it holds are reordered where they landed
        edge_t* const begin = const_cast<edge_t*>(reinterpret_cast<edge_t const*>(fetched.sources.data()));
        edges_span<ustore_key_t> const edges {begin, begin + fetched.size()};

        // Incoming and outgoing edges hold the neighbor in different columns, so every edge is flipped to target it
        for (std::size_t index = 0; index != edges.size(); ++index) {
            ustore_key_t& source = edges.sources[index];
            ustore_key_t& target = edges.targets[index];
            if (target == vertex) std::swap(source, target);
        }

        strided_span<ustore_key_t> const targets = edges.targets;
        std::size_t const count = sort_and_deduplicate(targets.begin(), targets.end());
        return strided_span<ustore_key_t const> {{targets.data(), targets.stride()}, count};
    }
};

/** A handle to one graph collection that reads and writes the head or the view of a transaction. */
class graph_collection_t : public graph_read_collection_t {

    status_t drop_with_(ustore_drop_mode_t mode) noexcept {
        ustore_outcome_t outcome {};
        ustore_collection_drop_t collection_drop {};
        collection_drop.database = receiver_.database;
        collection_drop.executor = receiver_.executor;
        collection_drop.outcome = &outcome;
        collection_drop.collection = collection_;
        collection_drop.mode = mode;
        ustore_collection_drop(&collection_drop);
        return status_t {outcome};
    }

  public:
    using graph_read_collection_t::graph_read_collection_t;

    /** The twin a deferred receiver hands out, whose verbs answer deferred calls. */
    using deferred_t = deferred_graph_collection_t;

    status_t write_vertices(strided_span<ustore_key_t const> vertices) noexcept {
        ustore_outcome_t outcome {};
        ustore_graph_write_vertices_t write {};
        write.database = receiver_.database;
        write.executor = receiver_.executor;
        write.outcome = &outcome;
        write.transaction = receiver_.transaction;
        write.count = vertices.size();
        write.collections = {&collection_, 0};
        write.vertices = {vertices.data(), vertices.stride()};

        ustore_graph_write_vertices(&write);
        return status_t {outcome};
    }

    template <typename keys_argument_type_>
    status_t write_vertices(keys_argument_type_&& vertices) noexcept {
        return write_vertices(strided(vertices).immutable());
    }

    status_t write_vertex(ustore_key_t const vertex) noexcept { return write_vertices({{&vertex}, 1}); }

    status_t write_edges(edges_view_t const& edges) noexcept {
        ustore_outcome_t outcome {};
        ustore_graph_write_edges_t graph_write_edges {};
        graph_write_edges.database = receiver_.database;
        graph_write_edges.executor = receiver_.executor;
        graph_write_edges.outcome = &outcome;
        graph_write_edges.transaction = receiver_.transaction;
        graph_write_edges.count = edges.size();
        graph_write_edges.collections = {&collection_, 0};
        graph_write_edges.edges = {edges.ids.data(), edges.ids.stride()};
        graph_write_edges.sources = {edges.sources.data(), edges.sources.stride()};
        graph_write_edges.targets = {edges.targets.data(), edges.targets.stride()};

        ustore_graph_write_edges(&graph_write_edges);
        return status_t {outcome};
    }

    status_t write_edge(edge_t const& edge) noexcept { return write_edges(edges_view_t {&edge, &edge + 1}); }

    status_t remove_vertices( //
        strided_span<ustore_key_t const> vertices, strided_span<ustore_vertex_role_t const> roles = {},
        ustore_durability_t durability = ustore_durability_buffered_k) noexcept {

        ustore_outcome_t outcome {};
        ustore_graph_remove_vertices_t graph_remove_vertices {};
        graph_remove_vertices.database = receiver_.database;
        graph_remove_vertices.executor = receiver_.executor;
        graph_remove_vertices.outcome = &outcome;
        graph_remove_vertices.transaction = receiver_.transaction;
        graph_remove_vertices.count = vertices.count();
        graph_remove_vertices.collections = {&collection_, 0};
        graph_remove_vertices.vertices = {vertices.data(), vertices.stride()};
        graph_remove_vertices.roles = {roles.data(), roles.stride()};
        graph_remove_vertices.durability = durability;

        ustore_graph_remove_vertices(&graph_remove_vertices);
        return status_t {outcome};
    }

    template <typename keys_argument_type_>
    status_t remove_vertices(keys_argument_type_&& vertices) noexcept {
        return remove_vertices(strided(vertices).immutable());
    }

    status_t remove_vertex( //
        ustore_key_t const vertex, ustore_vertex_role_t const role = ustore_vertex_role_any_k,
        ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return remove_vertices({{&vertex}, 1}, {{&role}, 1}, durability);
    }

    status_t remove_edges(edges_view_t const& edges) noexcept {
        ustore_outcome_t outcome {};
        ustore_graph_remove_edges_t graph_remove_edges {};
        graph_remove_edges.database = receiver_.database;
        graph_remove_edges.executor = receiver_.executor;
        graph_remove_edges.outcome = &outcome;
        graph_remove_edges.transaction = receiver_.transaction;
        graph_remove_edges.count = edges.size();
        graph_remove_edges.collections = {&collection_, 0};
        graph_remove_edges.edges = {edges.ids.data(), edges.ids.stride()};
        graph_remove_edges.sources = {edges.sources.data(), edges.sources.stride()};
        graph_remove_edges.targets = {edges.targets.data(), edges.targets.stride()};

        ustore_graph_remove_edges(&graph_remove_edges);
        return status_t {outcome};
    }

    status_t remove_edge(edge_t const& edge) noexcept { return remove_edges(edges_view_t {&edge, &edge + 1}); }

    /** Removes every edge but keeps the vertices. */
    status_t clear_values() noexcept { return drop_with_(ustore_drop_values_k); }

    status_t clear() noexcept { return drop_with_(ustore_drop_pairs_k); }
    status_t drop() noexcept { return drop_with_(ustore_drop_collection_k); }
};

#pragma endregion Collections

#pragma region Deferred Verbs

/** What a deferred @c ustore_graph_read_edges keeps beside its vertices: the collection and role the arguments point
 *  to, and the outputs the call writes into the arena it borrows, the edges only when asked for. */
template <typename answer_type_>
struct graph_read {
    using arguments_t = ustore_graph_read_edges_t;
    using answer_t = expected<answer_type_>;

    /** Collection every vertex lives in. */
    ustore_collection_id_t collection {};

    /** Role every vertex plays in the wanted edges. */
    ustore_vertex_role_t role {};

    /** Number of vertices, the length of the degrees. */
    ustore_size_t count = 0;

    /** Outputs for the degree of every vertex, the offsets of their edges and the tape of edges, three keys per edge,
     *  inside the arena. */
    ustore_vertex_degree_t const* degrees = nullptr;
    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* edges = nullptr;

    static void submit(arguments_t& arguments) noexcept { ustore_graph_read_edges(&arguments); }

    answer_t finish() && noexcept {
        if constexpr (std::is_same_v<answer_type_, edges_view_t>) return exported_edges(edges, offsets, count);
        else return std::span<ustore_vertex_degree_t const> {degrees, degrees ? count : 0};
    }
};

/** A deferred read of every edge of a batch of vertices, answering the edges in the order of the vertices, split by
 *  vertex through the offsets of the answer, where undirected graphs may repeat an edge reversed. */
using graph_read_edges_t = graph_read<edges_view_t>;

/** A deferred read of the degree of every vertex of a batch. */
using graph_degrees_t = graph_read<std::span<ustore_vertex_degree_t const>>;

/** What a deferred @c ustore_graph_write_edges keeps beside its edges: the collection. */
struct graph_write_edges_t {
    using arguments_t = ustore_graph_write_edges_t;
    using answer_t = status_t;

    /** Collection every edge lives in. */
    ustore_collection_id_t collection {};

    static void submit(arguments_t& arguments) noexcept { ustore_graph_write_edges(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

/** What a deferred @c ustore_graph_remove_edges keeps beside its edges: the collection. */
struct graph_remove_edges_t {
    using arguments_t = ustore_graph_remove_edges_t;
    using answer_t = status_t;

    /** Collection every edge lives in. */
    ustore_collection_id_t collection {};

    static void submit(arguments_t& arguments) noexcept { ustore_graph_remove_edges(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

/** What a deferred @c ustore_graph_write_vertices keeps beside its vertices: the collection. */
struct graph_write_vertices_t {
    using arguments_t = ustore_graph_write_vertices_t;
    using answer_t = status_t;

    /** Collection every vertex lives in. */
    ustore_collection_id_t collection {};

    static void submit(arguments_t& arguments) noexcept { ustore_graph_write_vertices(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

/** What a deferred @c ustore_graph_remove_vertices keeps beside its vertices: the collection and the role. */
struct graph_remove_vertices_t {
    using arguments_t = ustore_graph_remove_vertices_t;
    using answer_t = status_t;

    /** Collection every vertex lives in. */
    ustore_collection_id_t collection {};

    /** Role every vertex plays, which only joining graphs need. */
    ustore_vertex_role_t role {};

    static void submit(arguments_t& arguments) noexcept { ustore_graph_remove_vertices(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

#pragma endregion Deferred Verbs

#pragma region Deferred Collections

/** A graph collection that only reads, whose verbs submit to an executor and answer deferred calls, as a snapshot
 *  offers it. Every plain input a verb reads moves into the call, so nothing it borrowed can go before the call is
 *  final, while an arena is borrowed, since freeing it waits for the call. */
class deferred_graph_read_collection_t {
  protected:
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};

    /** Submits a read of the edges of @p vertices in @p role into @p arena, exporting the edges themselves only when
     *  @p answer_type_ asks for them. */
    template <typename answer_type_, typename vertices_type_>
    deferred<graph_read<answer_type_>> read_edges_of_(vertices_type_ vertices, arena_t& arena,
                                                      ustore_vertex_role_t role, ustore_watch_t watch) const noexcept {
        using verb_t = graph_read<answer_type_>;
        return deferred<verb_t>::submit(
            [&](verb_t& verb, ustore_graph_read_edges_t& arguments, vertices_type_ const& owned) noexcept {
                verb.collection = collection_;
                verb.role = role;
                verb.count = owned.size();
                arguments.database = receiver_.database;
                arguments.executor = receiver_.executor;
                arguments.view = receiver_.view(watch);
                arguments.count = owned.size();
                arguments.collections = {&verb.collection, 0};
                arguments.vertices = {owned.data(), sizeof(ustore_key_t)};
                arguments.roles = {&verb.role, 0};
                arguments.degrees_per_vertex = &verb.degrees;
                if constexpr (std::is_same_v<answer_type_, edges_view_t>) {
                    arguments.edges_offsets = &verb.offsets;
                    arguments.edges_per_vertex = &verb.edges;
                }
                arguments.arena = arena;
                return status_t {};
            },
            std::move(vertices));
    }

  public:
    deferred_graph_read_collection_t() noexcept = default;
    explicit deferred_graph_read_collection_t(receiver_t receiver, ustore_collection_id_t collection) noexcept
        : receiver_(receiver), collection_(collection) {}

    operator ustore_collection_id_t() const noexcept { return collection_; }

    /**
     *  @brief Reads every edge that has any of @p vertices in @p role into @p arena.
     *  @param[in] vertices Vertices whose edges are read, owned by the call until it is final.
     *  @param[in] arena Memory the edges land in, borrowed until the call is final, which the answer views.
     *  @param[in] role Role every vertex plays in the wanted edges.
     *  @param[in] watch Whether a read inside a transaction records the vertices for conflicts.
     */
    template <owned_array_concept<ustore_key_t> vertices_type_>
    deferred<graph_read_edges_t> read_edges(vertices_type_ vertices, arena_t& arena,
                                            ustore_vertex_role_t role = ustore_vertex_role_any_k,
                                            ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return read_edges_of_<edges_view_t>(std::move(vertices), arena, role, watch);
    }

    /**
     *  @brief Reads the degree of every vertex of @p vertices in @p role into @p arena.
     *  @param[in] vertices Vertices whose edges are counted, owned by the call until it is final.
     *  @param[in] arena Memory the degrees land in, borrowed until the call is final, which the answer views.
     *  @param[in] role Role every vertex plays in the counted edges.
     *  @param[in] watch Whether a read inside a transaction records the vertices for conflicts.
     */
    template <owned_array_concept<ustore_key_t> vertices_type_>
    deferred<graph_degrees_t> degrees(vertices_type_ vertices, arena_t& arena,
                                      ustore_vertex_role_t role = ustore_vertex_role_any_k,
                                      ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return read_edges_of_<std::span<ustore_vertex_degree_t const>>(std::move(vertices), arena, role, watch);
    }
};

/** A graph collection whose verbs submit to an executor and answer deferred calls, reading and writing the head or a
 *  transaction. */
class deferred_graph_collection_t : public deferred_graph_read_collection_t {

    /** Fills what a write and a removal of @p edges share. */
    template <typename verb_type_, typename edges_type_>
    void edit_edges_(verb_type_& verb, typename verb_type_::arguments_t& arguments, edges_type_ const& edges,
                     ustore_durability_t durability) const noexcept {
        edges_view_t const view {edges.data(), edges.data() + edges.size()};
        verb.collection = collection_;
        arguments.database = receiver_.database;
        arguments.executor = receiver_.executor;
        arguments.transaction = receiver_.transaction;
        arguments.count = view.size();
        arguments.collections = {&verb.collection, 0};
        arguments.edges = {view.ids.data(), view.ids.stride()};
        arguments.sources = {view.sources.data(), view.sources.stride()};
        arguments.targets = {view.targets.data(), view.targets.stride()};
        arguments.durability = durability;
    }

  public:
    using deferred_graph_read_collection_t::deferred_graph_read_collection_t;

    /**
     *  @brief Writes @p edges between existing or new vertices.
     *  @param[in] edges Edges to write, owned by the call until it is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    template <owned_array_concept<edge_t> edges_type_>
    deferred<graph_write_edges_t> write_edges(
        edges_type_ edges, ustore_durability_t durability = ustore_durability_buffered_k) const noexcept {
        return deferred<graph_write_edges_t>::submit(
            [&](graph_write_edges_t& verb, ustore_graph_write_edges_t& arguments, edges_type_ const& owned) noexcept {
                edit_edges_(verb, arguments, owned, durability);
                return status_t {};
            },
            std::move(edges));
    }

    /**
     *  @brief Removes @p edges, keeping their vertices.
     *  @param[in] edges Edges to remove, owned by the call until it is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    template <owned_array_concept<edge_t> edges_type_>
    deferred<graph_remove_edges_t> remove_edges(
        edges_type_ edges, ustore_durability_t durability = ustore_durability_buffered_k) const noexcept {
        return deferred<graph_remove_edges_t>::submit(
            [&](graph_remove_edges_t& verb, ustore_graph_remove_edges_t& arguments, edges_type_ const& owned) noexcept {
                edit_edges_(verb, arguments, owned, durability);
                return status_t {};
            },
            std::move(edges));
    }

    /**
     *  @brief Writes @p vertices without edges, keeping those already present.
     *  @param[in] vertices Vertices to write, owned by the call until it is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    template <owned_array_concept<ustore_key_t> vertices_type_>
    deferred<graph_write_vertices_t> write_vertices(
        vertices_type_ vertices, ustore_durability_t durability = ustore_durability_buffered_k) const noexcept {
        return deferred<graph_write_vertices_t>::submit(
            [&](graph_write_vertices_t& verb, ustore_graph_write_vertices_t& arguments,
                vertices_type_ const& owned) noexcept {
                verb.collection = collection_;
                arguments.database = receiver_.database;
                arguments.executor = receiver_.executor;
                arguments.transaction = receiver_.transaction;
                arguments.count = owned.size();
                arguments.collections = {&verb.collection, 0};
                arguments.vertices = {owned.data(), sizeof(ustore_key_t)};
                arguments.durability = durability;
                return status_t {};
            },
            std::move(vertices));
    }

    /**
     *  @brief Removes @p vertices together with every edge they belong to.
     *  @param[in] vertices Vertices to remove, owned by the call until it is final.
     *  @param[in] role Role every vertex plays, which only joining graphs need.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    template <owned_array_concept<ustore_key_t> vertices_type_>
    deferred<graph_remove_vertices_t> remove_vertices(
        vertices_type_ vertices, ustore_vertex_role_t role = ustore_vertex_role_any_k,
        ustore_durability_t durability = ustore_durability_buffered_k) const noexcept {
        return deferred<graph_remove_vertices_t>::submit(
            [&](graph_remove_vertices_t& verb, ustore_graph_remove_vertices_t& arguments,
                vertices_type_ const& owned) noexcept {
                verb.collection = collection_;
                verb.role = role;
                arguments.database = receiver_.database;
                arguments.executor = receiver_.executor;
                arguments.transaction = receiver_.transaction;
                arguments.count = owned.size();
                arguments.collections = {&verb.collection, 0};
                arguments.vertices = {owned.data(), sizeof(ustore_key_t)};
                arguments.roles = {&verb.role, 0};
                arguments.durability = durability;
                return status_t {};
            },
            std::move(vertices));
    }
};

#pragma endregion Deferred Collections

} // namespace unum::ustore
