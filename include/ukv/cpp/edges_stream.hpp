/**
 * @file edges_stream.hpp
 * @author Ashot Vardanian
 * @date 30 Jun 2022
 * @brief C++ bindings for @see "ukv/graph.h".
 */

#pragma once
#include "ukv/graph.h"
#include "ukv/cpp/ranges.hpp"      // `edges_span_t`
#include "ukv/cpp/keys_stream.hpp" // `keys_stream_t`

namespace unum::ukv {

/**
 * @brief A stream of all @c `edge_t`s in a graph.
 * No particular order is guaranteed.
 */
class edges_stream_t {

    ukv_t db_ = nullptr;
    ukv_collection_t col_ = ukv_default_collection_k;
    ukv_txn_t txn_ = nullptr;

    edges_span_t fetched_edges_ = {};
    std::size_t fetched_offset_ = 0;

    arena_t arena_;
    keys_stream_t vertex_stream_;

    status_t prefetch_gather() noexcept {

        auto vertices = vertex_stream_.keys_batch().strided();

        status_t status;
        ukv_vertex_degree_t* degrees_per_vertex = nullptr;
        ukv_key_t* neighborships_per_vertex = nullptr;
        ukv_vertex_role_t role = ukv_vertex_role_any_k;
        ukv_graph_find_edges(db_,
                             txn_,
                             vertices.count(),
                             &col_,
                             0,
                             vertices.begin().get(),
                             vertices.stride(),
                             &role,
                             0,
                             ukv_options_default_k,
                             &degrees_per_vertex,
                             &neighborships_per_vertex,
                             arena_.member_ptr(),
                             status.member_ptr());
        if (!status)
            return status;

        auto edges_begin = reinterpret_cast<edge_t*>(neighborships_per_vertex);
        auto edges_count = transform_reduce_n(degrees_per_vertex, vertices.size(), 0ul, [](ukv_vertex_degree_t deg) {
            return deg == ukv_vertex_degree_missing_k ? 0 : deg;
        });
        fetched_offset_ = 0;
        fetched_edges_ = {edges_begin, edges_begin + edges_count};
        return {};
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = ukv_key_t;
    using pointer = ukv_key_t*;
    using reference = ukv_key_t&;

    static constexpr std::size_t default_read_ahead_k = 256;

    edges_stream_t(ukv_t db,
                   ukv_collection_t col = ukv_default_collection_k,
                   std::size_t read_ahead_vertices = keys_stream_t::default_read_ahead_k,
                   ukv_txn_t txn = nullptr)
        : db_(db), col_(col), txn_(txn), arena_(db), vertex_stream_(db, col, read_ahead_vertices, txn) {}

    edges_stream_t(edges_stream_t&&) = default;
    edges_stream_t& operator=(edges_stream_t&&) = default;

    edges_stream_t(edges_stream_t const&) = delete;
    edges_stream_t& operator=(edges_stream_t const&) = delete;

    status_t seek(ukv_key_t vertex_id) noexcept {
        auto status = vertex_stream_.seek(vertex_id);
        if (!status)
            return status;
        return prefetch_gather();
    }

    status_t advance() noexcept {

        if (fetched_offset_ >= fetched_edges_.size()) {
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
    inline edges_stream_t& operator++() noexcept {
        status_t status = advance();
        if (status)
            return *this;

        fetched_edges_ = {};
        fetched_offset_ = 0;
        return *this;
    }

    edge_t edge() const noexcept { return fetched_edges_[fetched_offset_]; }
    edge_t operator*() const noexcept { return edge(); }
    status_t seek_to_first() noexcept { return seek(std::numeric_limits<ukv_key_t>::min()); }
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

    bool operator==(edges_stream_t const& other) const noexcept {
        return vertex_stream_ == other.vertex_stream_ && fetched_offset_ == other.fetched_offset_;
    }

    bool operator!=(edges_stream_t const& other) const noexcept {
        return vertex_stream_ != other.vertex_stream_ || fetched_offset_ != other.fetched_offset_;
    }
};

} // namespace unum::ukv
