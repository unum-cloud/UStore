/**
 *  @file include/ustore/graph_stream.hpp
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
#include "ustore/ranges.hpp"      // `strided`, `transform_reduce_n`
#include "ustore/ranges_args.hpp" // `edges_span_t`
#include "ustore/status.hpp"      // `status_t`, `expected`
#include "ustore/types.hpp"       // `arena_t`, `edge_t`

namespace unum::ustore {

/** A stream of every edge in a graph, fetched one page of vertices at a time and in no particular order.
 *  A stream made at the end holds no page, and a default-constructed one is at the end. */
class graph_stream_t {

    ustore_vertex_role_t role_ = ustore_vertex_role_any_k;

    edges_span_t fetched_edges_ {};
    std::size_t fetched_offset_ {0};

    arena_t arena_;
    keys_stream_t vertex_stream_;
    st::basic_vector<ustore_vertex_degree_t> degrees_ {};

    graph_stream_t(keys_stream_t vertex_stream, ustore_vertex_role_t role) noexcept
        : role_(role), vertex_stream_(std::move(vertex_stream)) {}

    status_t prefetch_gather() noexcept {

        strided_range<ustore_key_t const> const vertices = strided(vertex_stream_.keys_batch());
        ustore_key_t* edges_per_vertex = nullptr;
        ustore_outcome_t outcome {};
        ustore_graph_find_edges_t graph_find_edges {};
        graph_find_edges.database = vertex_stream_.database_;
        graph_find_edges.outcome = &outcome;
        graph_find_edges.view = vertex_stream_.view_;
        graph_find_edges.count = vertices.count();
        graph_find_edges.collections = &vertex_stream_.collection_;
        graph_find_edges.vertices = vertices.begin().get();
        graph_find_edges.vertices_stride = vertices.stride();
        graph_find_edges.roles = &role_;
        graph_find_edges.degrees_per_vertex = degrees_.data();
        graph_find_edges.degrees_per_vertex_stride = sizeof(ustore_vertex_degree_t);
        graph_find_edges.edges_per_vertex = &edges_per_vertex;
        graph_find_edges.arena = arena_.member_ptr();

        ustore_graph_find_edges(&graph_find_edges);
        if (outcome.status != ustore_status_success_k) return status_t {outcome};

        edge_t* const edges_begin = reinterpret_cast<edge_t*>(edges_per_vertex);
        std::size_t const edges_count = transform_reduce_n(
            degrees_.data(), vertices.size(), std::size_t {0}, [](ustore_vertex_degree_t degree) noexcept {
                return degree == ustore_vertex_degree_missing_k ? 0 : degree;
            });
        fetched_offset_ = 0;
        fetched_edges_ = {edges_begin, edges_begin + edges_count};
        return {};
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
    static expected<graph_stream_t> make(ustore_database_t database, ustore_view_t view,
                                         ustore_collection_id_t collection, ustore_vertex_role_t role,
                                         ustore_key_t start,
                                         std::size_t read_ahead = keys_stream_t::default_read_ahead_k) noexcept {
        auto [vertex_stream, vertex_stream_status] = keys_stream_t::make(database, view, collection, start, read_ahead);
        if (vertex_stream_status.failed()) return vertex_stream_status;
        graph_stream_t stream {std::move(vertex_stream), role};
        if (st::failed(stream.degrees_.resize(stream.vertex_stream_.page_.size())))
            return status_t {status_code_t::out_of_memory_k, "Failed to allocate a page of degrees"};
        if (status_t const gathered = stream.prefetch_gather(); gathered.failed()) return gathered;
        return stream;
    }

    status_t seek(ustore_key_t vertex_id) noexcept {
        if (status_t const sought = vertex_stream_.seek(vertex_id); sought.failed()) return sought;
        return prefetch_gather();
    }

    status_t advance() noexcept {
        if (fetched_offset_ >= fetched_edges_.size() - 1) return seek_to_next_batch();
        ++fetched_offset_;
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
    edges_span_t edges_batch() noexcept {
        fetched_offset_ = fetched_edges_.size();
        return fetched_edges_;
    }

    bool is_end() const noexcept { return vertex_stream_.is_end() && fetched_offset_ >= fetched_edges_.size(); }

    bool operator==(graph_stream_t const& other) const noexcept {
        return vertex_stream_ == other.vertex_stream_ && fetched_offset_ == other.fetched_offset_;
    }
};

} // namespace unum::ustore
