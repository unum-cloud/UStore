/**
 * @file graph_stream.hpp
 * @author Ashot Vardanian
 * @date 30 Jun 2022
 * @addtogroup Cpp
 *
 * @brief C++ bindings for "ustore/graph.h".
 */

#pragma once
#include "ustore/graph.h"
#include "ustore/cpp/ranges.hpp"      // `edges_span_t`
#include "ustore/cpp/blobs_range.hpp" // `keys_stream_t`

namespace unum::ustore {

/**
 * @brief A stream of all @c edge_t's in a graph.
 * No particular order is guaranteed.
 */
class graph_stream_t {

    ustore_database_t db_ {nullptr};
    ustore_collection_t collection_ {ustore_collection_main_k};
    ustore_transaction_t transaction_ {nullptr};
    ustore_snapshot_t snapshot_ {};
    ustore_vertex_role_t role_ = ustore_vertex_role_any_k;

    edges_span_t fetched_edges_ {};
    std::size_t fetched_offset_ {0};

    arena_t arena_;
    keys_stream_t vertex_stream_;

    status_t prefetch_gather() noexcept {

        auto vertices = vertex_stream_.keys_batch().strided();

        status_t status;
        ustore_vertex_degree_t* degrees_per_vertex = nullptr;
        ustore_key_t* edges_per_vertex = nullptr;

        ustore_graph_find_edges_t graph_find_edges {};
        graph_find_edges.db = db_;
        graph_find_edges.error = status.member_ptr();
        graph_find_edges.transaction = transaction_;
        graph_find_edges.snapshot = snapshot_;
        graph_find_edges.arena = arena_.member_ptr();
        graph_find_edges.tasks_count = vertices.count();
        graph_find_edges.collections = &collection_;
        graph_find_edges.vertices = vertices.begin().get();
        graph_find_edges.vertices_stride = vertices.stride();
        graph_find_edges.roles = &role_;
        graph_find_edges.degrees_per_vertex = &degrees_per_vertex;
        graph_find_edges.edges_per_vertex = &edges_per_vertex;

        ustore_graph_find_edges(&graph_find_edges);

        if (!status)
            return status;

        auto edges_begin = reinterpret_cast<edge_t*>(edges_per_vertex);
        auto edges_count = transform_reduce_n(degrees_per_vertex, vertices.size(), 0ul, [](ustore_vertex_degree_t deg) {
            return deg == ustore_vertex_degree_missing_k ? 0 : deg;
        });
        fetched_offset_ = 0;
        fetched_edges_ = {edges_begin, edges_begin + edges_count};
        return {};
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = ustore_key_t;
    using pointer = ustore_key_t*;
    using reference = ustore_key_t&;

    static constexpr std::size_t default_read_ahead_k = 256;

    graph_stream_t(ustore_database_t db,
                   ustore_collection_t collection = ustore_collection_main_k,
                   ustore_transaction_t txn = nullptr,
                   ustore_snapshot_t snap = 0,
                   std::size_t read_ahead_vertices = keys_stream_t::default_read_ahead_k,
                   ustore_vertex_role_t role = ustore_vertex_role_any_k) noexcept
        : db_(db), collection_(collection), transaction_(txn), snapshot_(snap), role_(role), arena_(db),
          vertex_stream_(db, collection, read_ahead_vertices, txn) {}

    graph_stream_t(graph_stream_t&&) = default;
    graph_stream_t& operator=(graph_stream_t&&) = default;

    graph_stream_t(graph_stream_t const&) = delete;
    graph_stream_t& operator=(graph_stream_t const&) = delete;

    status_t seek(ustore_key_t vertex_id) noexcept {
        auto status = vertex_stream_.seek(vertex_id);
        if (!status)
            return status;
        return prefetch_gather();
    }

    status_t advance() noexcept {

        if (fetched_offset_ >= fetched_edges_.size() - 1) {
            auto status = vertex_stream_.seek_to_next_batch();
            if (!status)
                return status;
            return prefetch_gather();
        }

        ++fetched_offset_;
        return {};
    }

    /**
     * ! Unlike the `advance()`, canonically returns a self-reference,
     * ! meaning that the error must be propagated in a different way.
     * ! So we promote this iterator to `end()`, once an error occurs.
     */
    inline graph_stream_t& operator++() noexcept {
        status_t status = advance();
        if (status)
            return *this;

        fetched_edges_ = {};
        fetched_offset_ = 0;
        return *this;
    }

    edge_t edge() const noexcept { return fetched_edges_[fetched_offset_]; }
    edge_t operator*() const noexcept { return edge(); }
    status_t seek_to_first() noexcept { return seek(std::numeric_limits<ustore_key_t>::min()); }
    status_t seek_to_next_batch() noexcept {
        auto status = vertex_stream_.seek_to_next_batch();
        if (!status)
            return status;
        return prefetch_gather();
    }

    /**
     * @brief Exposes all the fetched edges at once, including the passed ones.
     * Should be used with `seek_to_next_batch`. Next `advance` will do the same.
     */
    edges_span_t edges_batch() noexcept {
        fetched_offset_ = fetched_edges_.size();
        return fetched_edges_;
    }

    bool is_end() const noexcept { return vertex_stream_.is_end() && fetched_offset_ >= fetched_edges_.size(); }

    bool operator==(graph_stream_t const& other) const noexcept {
        return vertex_stream_ == other.vertex_stream_ && fetched_offset_ == other.fetched_offset_;
    }

    bool operator!=(graph_stream_t const& other) const noexcept {
        return vertex_stream_ != other.vertex_stream_ || fetched_offset_ != other.fetched_offset_;
    }
};

} // namespace unum::ustore
