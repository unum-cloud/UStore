/**
 *  @file cpp/ustore/graph.hpp
 *  @author Ash Vardanian
 *  @date June 30, 2022
 *  @brief Handles to one graph collection with typed vertex and edge operations, the views of the
 *      neighbors and presences of vertices, and a paginated stream over every edge.
 */
#pragma once

#include <cstddef> // `std::size_t`, `std::ptrdiff_t`

#include <algorithm> // `std::ranges::equal_range`
#include <iterator>  // `std::forward_iterator_tag`
#include <ranges>    // `std::ranges::subrange`
#include <span>      // `std::span`
#include <utility>   // `std::move`

#include "ustore/blobs.hpp" // `blobs_range_t`, `keys_stream_t`, `keys_cursor`, `sample_keys_`
#include "ustore/types.hpp" // `receiver_t`, `answered_`, `edges_view_t`, `exported_edges`, `indexed_iterator`

namespace unum::ustore {

#pragma region Verbs

/** Degrees a read writes into its arena, one per vertex. */
using degrees_view_t = std::span<ustore_vertex_degree_t const>;

/** Where a read of edges writes the degree of every vertex. */
template <>
struct outputs<degrees_view_t> {

    /** Vertices read, the length of the output. */
    ustore_size_t count = 0;

    /** Output for one degree per vertex, inside the arena. */
    ustore_vertex_degree_t const* degrees = nullptr;

    degrees_view_t answer() const noexcept { return {degrees, degrees ? count : 0}; }
};

/** Where a read of edges writes the edges of every vertex, split by their offsets, beside the
 *  degrees. */
template <>
struct outputs<edges_view_t> {

    /** Vertices read, one offset each plus one closing the last. */
    ustore_size_t count = 0;

    /** Output for one degree per vertex, inside the arena. */
    ustore_vertex_degree_t const* degrees = nullptr;

    /** Output for where the edges of every vertex start, inside the arena. */
    ustore_length_t const* offsets = nullptr;

    /** Output for the tape of edges, three keys per edge, inside the arena. */
    ustore_key_t const* edges = nullptr;

    edges_view_t answer() const noexcept { return exported_edges(edges, offsets, count); }
};

/** Where a read of edges asks for offsets and edges: nowhere, unless it answers them. */
template <typename answer_type_>
ustore_length_t const** edges_offsets_output_(outputs<answer_type_>&) noexcept {
    return nullptr;
}

inline ustore_length_t const** edges_offsets_output_(outputs<edges_view_t>& written) noexcept {
    return &written.offsets;
}

template <typename answer_type_>
ustore_key_t const** edges_output_(outputs<answer_type_>&) noexcept {
    return nullptr;
}

inline ustore_key_t const** edges_output_(outputs<edges_view_t>& written) noexcept { return &written.edges; }

/**
 *  @brief Reads the degrees of @p vertices in @p roles into @p arena, and their edges too when
 *      @p answer_type_ asks.
 *  @param[in] collection Collection every vertex lives in, borrowed until the call is final.
 *  @param[in] vertices Vertices to read, borrowed until the call is final.
 *  @param[in] roles Role every vertex plays in the wanted edges, empty for any, borrowed until the
 *      call is final.
 *  @param[in] arena Memory the answer lands in, borrowed until the call is final, which
 *      the answer views.
 *  @param[in] watch Whether a read inside a transaction records the vertices for conflicts.
 */
template <ustore_completion_t completion_, typename answer_type_>
deferrable<answer_type_, completion_> graph_read_edges_(receiver_t const& receiver,
                                                        ustore_collection_id_t const& collection,
                                                        strided_span<ustore_key_t const> vertices,
                                                        strided_span<ustore_vertex_role_t const> roles, arena_t& arena,
                                                        ustore_watch_t watch) noexcept {
    return answered_<deferrable<answer_type_, completion_>>(
        [&](ustore_outcome_t* outcome, outputs<answer_type_>* written) noexcept {
            written->count = vertices.count();
            ustore_graph_read_edges_t arguments {.database = receiver.database,
                                                 .executor = receiver.executor,
                                                 .outcome = outcome,
                                                 .completion = completion_,
                                                 .view = receiver.view(watch),
                                                 .count = vertices.count(),
                                                 .collections = {&collection, 0},
                                                 .vertices = {vertices.data(), vertices.stride()},
                                                 .roles = {roles.data(), roles.stride()},
                                                 .degrees_per_vertex = &written->degrees,
                                                 .edges_offsets = edges_offsets_output_(*written),
                                                 .edges_per_vertex = edges_output_(*written),
                                                 .arena = arena.handle()};
            ustore_graph_read_edges(&arguments);
        });
}

/** Writes @p edges into @p collection, between existing or new vertices. */
template <ustore_completion_t completion_>
deferrable<void, completion_> graph_write_edges_(receiver_t const& receiver, ustore_collection_id_t const& collection,
                                                 edges_view_t const& edges, ustore_durability_t durability) noexcept {
    return answered_<deferrable<void, completion_>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_graph_write_edges_t arguments {.database = receiver.database,
                                              .executor = receiver.executor,
                                              .outcome = outcome,
                                              .completion = completion_,
                                              .transaction = receiver.transaction,
                                              .count = edges.size(),
                                              .collections = {&collection, 0},
                                              .edges = {edges.ids.data(), edges.ids.stride()},
                                              .sources = {edges.sources.data(), edges.sources.stride()},
                                              .targets = {edges.targets.data(), edges.targets.stride()},
                                              .durability = durability};
        ustore_graph_write_edges(&arguments);
    });
}

/** Removes @p edges from @p collection, keeping their vertices. */
template <ustore_completion_t completion_>
deferrable<void, completion_> graph_remove_edges_(receiver_t const& receiver, ustore_collection_id_t const& collection,
                                                  edges_view_t const& edges, ustore_durability_t durability) noexcept {
    return answered_<deferrable<void, completion_>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_graph_remove_edges_t arguments {.database = receiver.database,
                                               .executor = receiver.executor,
                                               .outcome = outcome,
                                               .completion = completion_,
                                               .transaction = receiver.transaction,
                                               .count = edges.size(),
                                               .collections = {&collection, 0},
                                               .edges = {edges.ids.data(), edges.ids.stride()},
                                               .sources = {edges.sources.data(), edges.sources.stride()},
                                               .targets = {edges.targets.data(), edges.targets.stride()},
                                               .durability = durability};
        ustore_graph_remove_edges(&arguments);
    });
}

/** Writes @p vertices into @p collection without edges, keeping those already present. */
template <ustore_completion_t completion_>
deferrable<void, completion_> graph_write_vertices_(receiver_t const& receiver,
                                                    ustore_collection_id_t const& collection,
                                                    strided_span<ustore_key_t const> vertices,
                                                    ustore_durability_t durability) noexcept {
    return answered_<deferrable<void, completion_>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_graph_write_vertices_t arguments {.database = receiver.database,
                                                 .executor = receiver.executor,
                                                 .outcome = outcome,
                                                 .completion = completion_,
                                                 .transaction = receiver.transaction,
                                                 .count = vertices.size(),
                                                 .collections = {&collection, 0},
                                                 .vertices = {vertices.data(), vertices.stride()},
                                                 .durability = durability};
        ustore_graph_write_vertices(&arguments);
    });
}

/** Removes @p vertices from @p collection together with every edge they play @p roles in. */
template <ustore_completion_t completion_>
deferrable<void, completion_> graph_remove_vertices_(receiver_t const& receiver,
                                                     ustore_collection_id_t const& collection,
                                                     strided_span<ustore_key_t const> vertices,
                                                     strided_span<ustore_vertex_role_t const> roles,
                                                     ustore_durability_t durability) noexcept {
    return answered_<deferrable<void, completion_>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_graph_remove_vertices_t arguments {.database = receiver.database,
                                                  .executor = receiver.executor,
                                                  .outcome = outcome,
                                                  .completion = completion_,
                                                  .transaction = receiver.transaction,
                                                  .count = vertices.count(),
                                                  .collections = {&collection, 0},
                                                  .vertices = {vertices.data(), vertices.stride()},
                                                  .roles = {roles.data(), roles.stride()},
                                                  .durability = durability};
        ustore_graph_remove_vertices(&arguments);
    });
}

#pragma endregion Verbs

#pragma region Views

/** The other end of every edge a read exported for one vertex, as stored: an edge from the vertex
 *  to itself names the vertex, and a neighbor joined by two edges appears twice. */
class neighbors_view_t {
    edges_view_t edges_ {};
    ustore_key_t vertex_ {};

  public:
    using value_type = ustore_key_t;

    neighbors_view_t() noexcept = default;
    neighbors_view_t(edges_view_t const& edges, ustore_key_t vertex) noexcept : edges_(edges), vertex_(vertex) {}

    std::size_t size() const noexcept { return edges_.size(); }
    bool empty() const noexcept { return !edges_.size(); }

    ustore_key_t operator[](std::size_t index) const noexcept {
        return edges_.targets[index] == vertex_ ? edges_.sources[index] : edges_.targets[index];
    }

    indexed_iterator<neighbors_view_t> begin() const noexcept { return {*this, 0}; }
    indexed_iterator<neighbors_view_t> end() const noexcept { return {*this, size()}; }

    /** The edges the neighbors were read off. */
    edges_view_t const& edges() const noexcept { return edges_; }
};

/** Whether every vertex a read of degrees asked about is present, edges or not. */
class presences_view_t {
    degrees_view_t degrees_ {};

  public:
    using value_type = bool;

    presences_view_t() noexcept = default;
    explicit presences_view_t(degrees_view_t degrees) noexcept : degrees_(degrees) {}

    std::size_t size() const noexcept { return degrees_.size(); }
    bool empty() const noexcept { return degrees_.empty(); }
    bool operator[](std::size_t index) const noexcept { return degrees_[index] != ustore_vertex_degree_missing_k; }

    indexed_iterator<presences_view_t> begin() const noexcept { return {*this, 0}; }
    indexed_iterator<presences_view_t> end() const noexcept { return {*this, size()}; }
};

#pragma endregion Views

#pragma region Streams

/** A walk over the vertices of one graph collection, which pulls keys alone. */
using graph_vertices_cursor_t = keys_cursor<ustore_graph_vertices_cursor_t, ustore_graph_vertices_cursor_init_t,
                                            ustore_graph_vertices_cursor_next_t, &ustore_graph_vertices_cursor_init,
                                            &ustore_graph_vertices_cursor_next, &ustore_graph_vertices_cursor_free>;

/** A stream of every edge in a graph, pulled one page of vertices at a time from the vertex scans
 *  of its stream and in no particular order, the vertices into the arena of that stream and their
 *  edges into an arena of its own, as a read never takes its inputs from the arena it rewinds. It
 *  owns both for its lifetime as shorthand for a range-based loop, with no place for an arena. A
 *  default-constructed stream is at the end. */
class graph_stream_t {

    ustore_vertex_role_t role_ = ustore_vertex_role_any_k;

    edges_view_t fetched_edges_ {};
    std::size_t fetched_offset_ {0};

    arena_t arena_;
    keys_stream_t vertex_stream_;

    graph_stream_t(keys_stream_t vertex_stream, arena_t arena, ustore_vertex_role_t role) noexcept
        : role_(role), arena_(std::move(arena)), vertex_stream_(std::move(vertex_stream)) {}

    /** Reads the edges of the page of vertices in hand, then of the pages after it until one has
     *  edges or the vertices run out. */
    status_t prefetch_gather() noexcept {
        for (;;) {
            strided_span<ustore_key_t const> const vertices = strided(vertex_stream_.keys_batch());
            strided_span<ustore_vertex_role_t const> const roles {{&role_, 0}, vertices.size()};
            auto [edges, read_status] = graph_read_edges_<ustore_completion_blocking_k, edges_view_t>(
                vertex_stream_.receiver_, vertex_stream_.collection_, vertices, roles, arena_, ustore_watch_keys_k);
            if (read_status.failed()) return read_status;
            fetched_offset_ = 0;
            fetched_edges_ = edges;
            if (fetched_edges_.size() || vertex_stream_.is_end()) return {};
            if (status_t const pulled = vertex_stream_.seek_to_next_batch(); pulled.failed()) return pulled;
        }
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = edge_t;

    static constexpr std::size_t default_read_ahead_k = 256;

    graph_stream_t() noexcept = default;
    graph_stream_t(graph_stream_t&&) noexcept = default;
    graph_stream_t& operator=(graph_stream_t&&) noexcept = default;
    graph_stream_t(graph_stream_t const&) = delete;
    graph_stream_t& operator=(graph_stream_t const&) = delete;

    /** Opens a stream over the edges of @p collection in @p role, from the vertex @p start onwards,
     *  fetching the edges of @p read_ahead vertices a page. */
    static expected<graph_stream_t> make(receiver_t receiver, ustore_collection_id_t collection,
                                         ustore_vertex_role_t role, ustore_key_t start,
                                         std::size_t read_ahead = keys_stream_t::default_read_ahead_k) noexcept {
        auto [vertex_stream, vertex_stream_status] = keys_stream_t::make(receiver, collection, start, read_ahead,
                                                                         ustore_modality_graph_k);
        if (vertex_stream_status.failed()) return {{}, vertex_stream_status};
        auto [arena, arena_status] = arena_t::make({.executor = receiver.executor});
        if (arena_status.failed()) return {{}, arena_status};
        graph_stream_t stream {std::move(vertex_stream), std::move(arena), role};
        if (status_t const gathered = stream.prefetch_gather(); gathered.failed()) return {{}, gathered};
        return {std::move(stream)};
    }

    status_t seek(ustore_key_t vertex_id) noexcept {
        if (status_t const sought = vertex_stream_.seek(vertex_id); sought.failed()) return sought;
        return prefetch_gather();
    }

    status_t advance() noexcept {
        if (fetched_offset_ + 1 < fetched_edges_.size()) ++fetched_offset_;
        else if (vertex_stream_.is_end()) fetched_offset_ = fetched_edges_.size();
        else return seek_to_next_batch();
        return {};
    }

    /** Advances like @c advance, but a failed fetch ends the stream since a reference carries no
     *  status. */
    graph_stream_t& operator++() noexcept {
        if (!advance().failed()) return *this;
        fetched_edges_ = {};
        fetched_offset_ = 0;
        return *this;
    }

    edge_t edge() const noexcept { return fetched_edges_[fetched_offset_]; }
    edge_t operator*() const noexcept { return edge(); }
    status_t seek_to_first() noexcept { return seek(smallest_key_k); }
    status_t seek_to_next_batch() noexcept {
        if (status_t const sought = vertex_stream_.seek_to_next_batch(); sought.failed()) return sought;
        return prefetch_gather();
    }

    /** Exposes the whole fetched page, passed edges included, for use
     *  with @c seek_to_next_batch. */
    edges_view_t edges_batch() noexcept {
        fetched_offset_ = fetched_edges_.size();
        return fetched_edges_;
    }

    bool is_end() const noexcept { return vertex_stream_.is_end() && fetched_offset_ >= fetched_edges_.size(); }

    bool operator==(graph_stream_t const& other) const noexcept {
        return vertex_stream_ == other.vertex_stream_ && fetched_offset_ == other.fetched_offset_;
    }
};

#pragma endregion Streams

#pragma region Collections

/**
 *  @brief A handle to one graph collection, stored as an inverted index from every vertex to its
 *      edges, cheap to construct and to copy, whose verbs answer as @p completion_ says.
 *
 *  A blocking handle answers a @c status_t or an @c expected, while a deferred one answers a
 *  @c deferred call, borrowing the handle, its inputs and its arena until the call is final. A
 *  handle with @c collection_access_t::read_only_k only reads, as a snapshot hands out. It holds no
 *  state, so any number of threads share it, at the head, inside a transaction or at a snapshot.
 */
template <ustore_completion_t completion_, collection_access_t access_>
class graph_collection_on {
    template <typename value_type_ = void>
    using deferrable = ustore::deferrable<value_type_, completion_>;
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};
    static constexpr bool writes_k = access_ == collection_access_t::read_write_k;

  public:
    using adjacency_range_t = stream_range<graph_stream_t>;

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

    ustore_collection_id_t handle() const noexcept { return collection_; }
    receiver_t receiver() const noexcept { return receiver_; }

    /** The same collection, whose verbs block on @p executor, or on the calling
     *  thread without one. */
    completed_as<ustore_completion_blocking_k> on(executor_t const& executor) const noexcept {
        return {receiver_.on(executor.handle()), collection_};
    }

    /** The same collection, whose verbs submit to @p executor and answer deferred calls. */
    completed_as<ustore_completion_deferred_k> deferred(executor_t const& executor) const noexcept {
        return {receiver_.on(executor.handle()), collection_};
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
    deferrable<degrees_view_t> degrees(strided_span<ustore_key_t const> vertices, arena_t& arena,
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
    deferrable<edges_view_t> read_edges(strided_span<ustore_key_t const> vertices, arena_t& arena,
                                        strided_span<ustore_vertex_role_t const> roles = {},
                                        ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return graph_read_edges_<completion_, edges_view_t>(receiver_, collection_, vertices, roles, arena, watch);
    }

    /** Writes @p edges, borrowed until the call is final, between existing or new vertices. */
    deferrable<> write_edges(edges_view_t const& edges,
                             ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        return graph_write_edges_<completion_>(receiver_, collection_, edges, durability);
    }

    /** Removes @p edges, borrowed until the call is final, keeping their vertices. */
    deferrable<> remove_edges(edges_view_t const& edges,
                              ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        return graph_remove_edges_<completion_>(receiver_, collection_, edges, durability);
    }

    /** Writes @p vertices, borrowed until the call is final, without edges, keeping those already
     *  present. */
    deferrable<> write_vertices(strided_span<ustore_key_t const> vertices,
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
    deferrable<> remove_vertices(strided_span<ustore_key_t const> vertices,
                                 strided_span<ustore_vertex_role_t const> roles = {},
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
    deferrable<std::span<ustore_key_t const>> sample_vertices(
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
    deferrable<size_estimates_t> measure_vertices(ustore_key_t const& start_key, ustore_key_t const& end_key,
                                                  arena_t& arena,
                                                  ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return measure_keys_<completion_, ustore_graph_measure_vertices_t, &ustore_graph_measure_vertices>(
            receiver_, collection_, start_key, end_key, arena, watch);
    }

    /** Removes every edge but keeps the vertices. */
    deferrable<> clear_values() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_values_k);
    }

    /** Removes every vertex and edge, keeping the collection. */
    deferrable<> clear() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_pairs_k);
    }

    /** Removes the collection with its name and its whole graph. */
    deferrable<> drop() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_collection_k);
    }

#pragma endregion Verbs

#pragma region Shorthand

    /** Reads the degree of @p vertex in @p role through a one-off arena, which a missing vertex
     *  reads as @c ustore_vertex_degree_missing_k. */
    expected<ustore_vertex_degree_t> degree( //
        ustore_key_t vertex, ustore_vertex_role_t role = ustore_vertex_role_any_k,
        ustore_watch_t watch = ustore_watch_keys_k) const noexcept
        requires blocks<completion_>
    {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept -> expected<ustore_vertex_degree_t> {
            auto [found, degrees_status] = degrees({{&vertex}, 1}, arena, {{&role}, 1}, watch);
            if (degrees_status.failed()) return {{}, degrees_status};
            return {found[0]};
        });
    }

    /** Reads which of @p vertices are present into @p arena, including those without any edges. */
    expected<presences_view_t> present( //
        strided_span<ustore_key_t const> const& vertices, arena_t& arena,
        ustore_watch_t watch = ustore_watch_keys_k) const noexcept
        requires blocks<completion_>
    {
        auto [found, degrees_status] = degrees(vertices, arena, {}, watch);
        if (degrees_status.failed()) return {{}, degrees_status};
        return {presences_view_t {found}};
    }

    expected<keys_stream_t> vertex_stream(
        std::size_t vertices_read_ahead = keys_stream_t::default_read_ahead_k) const noexcept
        requires blocks<completion_>
    {
        return keys_stream_t::make(receiver_, collection_, smallest_key_k, vertices_read_ahead, modality_k);
    }

    /** The bounds on the vertices the engine estimates over the whole key range, through a one-off
     *  arena. */
    expected<size_range_t> size_range() const noexcept
        requires blocks<completion_>
    {
        return measured_size_range_<ustore_graph_measure_vertices_t, &ustore_graph_measure_vertices>(receiver_,
                                                                                                     collection_);
    }

    /** Counts the vertices exactly by streaming through all of them. */
    expected<std::size_t> size() const noexcept
        requires blocks<completion_>
    {
        return blobs_range_t {receiver_, collection_, smallest_key_k, largest_key_k, modality_k}.keys().size();
    }

    /** Counts the edges by streaming through the outgoing edges of every vertex. */
    expected<std::size_t> number_of_edges() const noexcept
        requires blocks<completion_>
    {
        auto [stream, stream_status] = graph_stream_t::make(receiver_, collection_, ustore_vertex_role_source_k,
                                                            smallest_key_k);
        if (stream_status.failed()) return {{}, stream_status};

        std::size_t count = 0;
        for (; !stream.is_end(); ++stream) ++count;
        return {count};
    }

    /** Every edge that has a vertex in @p role, walked once by a range-based loop. */
    adjacency_range_t edges(ustore_vertex_role_t role = ustore_vertex_role_any_k,
                            std::size_t vertices_read_ahead = keys_stream_t::default_read_ahead_k) const noexcept
        requires blocks<completion_>
    {
        return adjacency_range_t {
            graph_stream_t::make(receiver_, collection_, role, smallest_key_k, vertices_read_ahead)};
    }

    /** Reads every edge that has @p vertex in @p role into @p arena. */
    expected<edges_view_t> read_edges( //
        ustore_key_t vertex, arena_t& arena, ustore_vertex_role_t role = ustore_vertex_role_any_k,
        ustore_watch_t watch = ustore_watch_keys_k) const noexcept
        requires blocks<completion_>
    {
        return read_edges({{&vertex}, 1}, arena, {{&role}, 1}, watch);
    }

    /** Reads the edges from @p source to @p target into @p arena. */
    expected<edges_view_t> edges_between(ustore_key_t source, ustore_key_t target, arena_t& arena,
                                         ustore_watch_t watch = ustore_watch_keys_k) const noexcept
        requires blocks<completion_>
    {
        auto [outgoing, read_edges_status] = read_edges(source, arena, ustore_vertex_role_source_k, watch);
        if (read_edges_status.failed()) return {{}, read_edges_status};

        std::ranges::subrange<strided_iterator<ustore_key_t const>> const matches = std::ranges::equal_range(
            outgoing.targets.begin(), outgoing.targets.end(), target);
        std::size_t const offset = static_cast<std::size_t>(matches.begin() - outgoing.targets.begin());
        std::size_t const count = static_cast<std::size_t>(matches.end() - matches.begin());
        return {edges_view_t {outgoing.sources.subspan(offset, count), outgoing.targets.subspan(offset, count),
                              outgoing.ids.subspan(offset, count)}};
    }

    /** The other end of every edge that has @p vertex in @p role, read into @p arena, as stored. */
    expected<neighbors_view_t> neighbors(ustore_key_t vertex, arena_t& arena,
                                         ustore_vertex_role_t role = ustore_vertex_role_any_k) const noexcept
        requires blocks<completion_>
    {
        auto [edges, read_edges_status] = read_edges(vertex, arena, role);
        if (read_edges_status.failed()) return {{}, read_edges_status};
        return {neighbors_view_t {edges, vertex}};
    }

    status_t write_vertex(ustore_key_t const vertex) const noexcept
        requires(blocks<completion_> && writes_k)
    {
        return write_vertices({{&vertex}, 1});
    }

    status_t write_edge(edge_t const& edge) const noexcept
        requires(blocks<completion_> && writes_k)
    {
        return write_edges(edges_view_t {&edge, &edge + 1});
    }

    status_t remove_vertex( //
        ustore_key_t const vertex, ustore_vertex_role_t const role = ustore_vertex_role_any_k,
        ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires(blocks<completion_> && writes_k)
    {
        return remove_vertices({{&vertex}, 1}, {{&role}, 1}, durability);
    }

    status_t remove_edge(edge_t const& edge) const noexcept
        requires(blocks<completion_> && writes_k)
    {
        return remove_edges(edges_view_t {&edge, &edge + 1});
    }

#pragma endregion Shorthand
};

/** A graph collection whose verbs block, reading and writing the head or a transaction. */
using graph_collection_t = graph_collection_on<ustore_completion_blocking_k, collection_access_t::read_write_k>;

/** A graph collection whose verbs block and only read, as a snapshot hands out. */
using graph_read_collection_t = graph_collection_on<ustore_completion_blocking_k, collection_access_t::read_only_k>;

#pragma endregion Collections

} // namespace unum::ustore
