/**
 *  @file cpp/ustore/graph_stream.hpp
 *  @author Ash Vardanian
 *  @date June 30, 2022
 *  @brief A paginated stream over every edge of a graph collection.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::ptrdiff_t`

#include <iterator> // `std::forward_iterator_tag`
#include <utility>  // `std::move`

#include "ustore/blobs_range.hpp" // `keys_stream_t`
#include "ustore/graph.h"
#include "ustore/ranges.hpp" // `strided`, `edges_view_t`, `exported_edges`
#include "ustore/status.hpp" // `status_t`, `expected`
#include "ustore/types.hpp"  // `arena_t`, `edge_t`, `receiver_t`

namespace unum::ustore {

/** A stream of every edge in a graph, pulled one page of vertices at a time from the vertex scans of its stream and
 *  in no particular order, the vertices into the arena of that stream and their edges into an arena of its own, as a
 *  read never takes its inputs from the arena it rewinds. It owns both for its lifetime as shorthand for a range-based
 *  loop, with no place for an arena. A default-constructed stream is at the end. */
class graph_stream_t {

    ustore_vertex_role_t role_ = ustore_vertex_role_any_k;

    edges_view_t fetched_edges_ {};
    std::size_t fetched_offset_ {0};

    arena_t arena_;
    keys_stream_t vertex_stream_;

    graph_stream_t(keys_stream_t vertex_stream, arena_t arena, ustore_vertex_role_t role) noexcept
        : role_(role), arena_(std::move(arena)), vertex_stream_(std::move(vertex_stream)) {}

    /** Reads the edges of the page of vertices in hand, then of the pages after it until one has edges or the vertices
     *  run out. */
    status_t prefetch_gather() noexcept {
        for (;;) {
            strided_span<ustore_key_t const> const vertices = strided(vertex_stream_.keys_batch());
            ustore_vertex_degree_t const* degrees = nullptr;
            ustore_length_t const* edges_offsets = nullptr;
            ustore_key_t const* edges_per_vertex = nullptr;
            ustore_outcome_t outcome {};
            ustore_graph_read_edges_t graph_read_edges {};
            graph_read_edges.database = vertex_stream_.receiver_.database;
            graph_read_edges.executor = vertex_stream_.receiver_.executor;
            graph_read_edges.outcome = &outcome;
            graph_read_edges.view = vertex_stream_.receiver_.view(ustore_watch_keys_k);
            graph_read_edges.count = vertices.count();
            graph_read_edges.collections = {&vertex_stream_.collection_, 0};
            graph_read_edges.vertices = {vertices.data(), vertices.stride()};
            graph_read_edges.roles = {&role_, 0};
            graph_read_edges.degrees_per_vertex = &degrees;
            graph_read_edges.edges_offsets = &edges_offsets;
            graph_read_edges.edges_per_vertex = &edges_per_vertex;
            graph_read_edges.arena = arena_;

            ustore_graph_read_edges(&graph_read_edges);
            if (status_t const status {outcome}; status.failed()) return status;
            fetched_offset_ = 0;
            fetched_edges_ = exported_edges(edges_per_vertex, edges_offsets, vertices.size());
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

    /** Opens a stream over the edges of @p collection in @p role, from the vertex @p start onwards, fetching the edges
     *  of @p read_ahead vertices a page. */
    static expected<graph_stream_t> make(receiver_t receiver, ustore_collection_id_t collection,
                                         ustore_vertex_role_t role, ustore_key_t start,
                                         std::size_t read_ahead = keys_stream_t::default_read_ahead_k) noexcept {
        auto [vertex_stream, vertex_stream_status] = keys_stream_t::make(receiver, collection, start, read_ahead,
                                                                         ustore_modality_graph_k);
        if (vertex_stream_status.failed()) return vertex_stream_status;
        auto [arena, arena_status] = arena_t::make(receiver.executor);
        if (arena_status.failed()) return arena_status;
        graph_stream_t stream {std::move(vertex_stream), std::move(arena), role};
        if (status_t const gathered = stream.prefetch_gather(); gathered.failed()) return gathered;
        return stream;
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

    /** Advances like @c advance, but a failed fetch ends the stream since a reference carries no status. */
    graph_stream_t& operator++() noexcept {
        if (advance().succeeded()) return *this;
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

    /** Exposes the whole fetched page, passed edges included, for use with @c seek_to_next_batch. */
    edges_view_t edges_batch() noexcept {
        fetched_offset_ = fetched_edges_.size();
        return fetched_edges_;
    }

    bool is_end() const noexcept { return vertex_stream_.is_end() && fetched_offset_ >= fetched_edges_.size(); }

    bool operator==(graph_stream_t const& other) const noexcept {
        return vertex_stream_ == other.vertex_stream_ && fetched_offset_ == other.fetched_offset_;
    }
};

} // namespace unum::ustore
