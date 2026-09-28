/**
 *  @file include/ustore/graph_collection.hpp
 *  @author Ash Vardanian
 *  @date June 30, 2022
 *  @brief A handle to one graph collection with typed vertex and edge operations.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <algorithm> // `std::ranges::equal_range`
#include <span>      // `std::span`
#include <utility>   // `std::forward`, `std::move`, `std::swap`

#include <ustore/blobs_range.hpp> // `keys_range_t`
#include <ustore/blobs_ref.hpp>   // `blobs_ref`
#include <ustore/graph.h>
#include <ustore/graph_stream.hpp> // `graph_stream_t`
#include <ustore/ranges.hpp>       // `range`, `strided`, `sort_and_deduplicate`, `transform_reduce_n`
#include <ustore/ranges_args.hpp>  // `edges_span_t`, `edges_view_t`, `places_arg_t`
#include <ustore/status.hpp>       // `status_t`, `expected`
#include <ustore/types.hpp>        // `any_arena_t`, `collection_key_field_t`, `edge_t`

namespace unum::ustore {

/** A handle to one graph collection, stored as an inverted index from every vertex to its edges.
 *  It reads into a local arena and so serves one thread, though every thread may hold its own
 *  handle to the same collection, inside or outside a transaction. */
class graph_collection_t {
    ustore_database_t database_ = nullptr;
    ustore_collection_id_t collection_ = ustore_collection_main_k;
    ustore_transaction_t transaction_ = nullptr;
    ustore_snapshot_t snapshot_ = {};
    any_arena_t arena_;

    status_t drop(ustore_drop_mode_t mode) noexcept {
        ustore_error_t error {};
        ustore_collection_drop_t collection_drop {};
        collection_drop.db = database_;
        collection_drop.error = &error;
        collection_drop.collection = collection_;
        collection_drop.mode = mode;
        ustore_collection_drop(&collection_drop);
        return status_t {error};
    }

  public:
    using adjacency_range_t = range<graph_stream_t>;

    graph_collection_t() noexcept : arena_(nullptr) {}
    graph_collection_t(ustore_database_t database, ustore_collection_id_t collection = ustore_collection_main_k,
                       ustore_transaction_t transaction = nullptr, ustore_snapshot_t snapshot = {},
                       ustore_arena_t* arena = nullptr) noexcept
        : database_(database), collection_(collection), transaction_(transaction), snapshot_(snapshot),
          arena_(database_, arena) {}

    graph_collection_t(graph_collection_t&&) = default;
    graph_collection_t& operator=(graph_collection_t&&) = default;
    graph_collection_t(graph_collection_t const&) = delete;
    graph_collection_t& operator=(graph_collection_t const&) = delete;

    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }

    status_t upsert_vertices(strided_range<ustore_key_t const> vertices) noexcept {
        ustore_error_t error {};
        ustore_graph_upsert_vertices_t upsert {};
        upsert.db = database_;
        upsert.error = &error;
        upsert.transaction = transaction_;
        upsert.arena = arena_;
        upsert.tasks_count = vertices.size();
        upsert.collections = &collection_;
        upsert.vertices = vertices.data();
        upsert.vertices_stride = vertices.stride();

        ustore_graph_upsert_vertices(&upsert);
        return status_t {error};
    }

    template <typename keys_arg_>
    status_t upsert_vertices(keys_arg_&& vertices) noexcept {
        return upsert_vertices(strided(vertices).immutable());
    }

    status_t upsert_vertex(ustore_key_t const vertex) noexcept { return upsert_vertices({{&vertex}, 1}); }

    status_t upsert_edges(edges_view_t const& edges) noexcept {
        ustore_error_t error {};
        ustore_graph_upsert_edges_t graph_upsert_edges {};
        graph_upsert_edges.db = database_;
        graph_upsert_edges.error = &error;
        graph_upsert_edges.transaction = transaction_;
        graph_upsert_edges.arena = arena_;
        graph_upsert_edges.tasks_count = edges.size();
        graph_upsert_edges.collections = &collection_;
        graph_upsert_edges.edges_ids = edges.edge_ids.begin().get();
        graph_upsert_edges.edges_stride = edges.edge_ids.stride();
        graph_upsert_edges.sources_ids = edges.source_ids.begin().get();
        graph_upsert_edges.sources_stride = edges.source_ids.stride();
        graph_upsert_edges.targets_ids = edges.target_ids.begin().get();
        graph_upsert_edges.targets_stride = edges.target_ids.stride();

        ustore_graph_upsert_edges(&graph_upsert_edges);
        return status_t {error};
    }

    status_t upsert_edge(edge_t const& edge) noexcept { return upsert_edges(edges_view_t {&edge, &edge + 1}); }

    status_t remove_vertices( //
        strided_range<ustore_key_t const> vertices, strided_range<ustore_vertex_role_t const> roles = {},
        flush_t flush = flush_t::dont_flush_k) noexcept {

        ustore_error_t error {};
        ustore_graph_remove_vertices_t graph_remove_vertices {};
        graph_remove_vertices.db = database_;
        graph_remove_vertices.error = &error;
        graph_remove_vertices.transaction = transaction_;
        graph_remove_vertices.arena = arena_;
        graph_remove_vertices.options = to_options(flush);
        graph_remove_vertices.tasks_count = vertices.count();
        graph_remove_vertices.collections = &collection_;
        graph_remove_vertices.vertices = vertices.begin().get();
        graph_remove_vertices.vertices_stride = vertices.stride();
        graph_remove_vertices.roles = roles.begin().get();
        graph_remove_vertices.roles_stride = roles.stride();

        ustore_graph_remove_vertices(&graph_remove_vertices);
        return status_t {error};
    }

    template <typename keys_arg_>
    status_t remove_vertices(keys_arg_&& vertices) noexcept {
        return remove_vertices(strided(vertices).immutable());
    }

    status_t remove_vertex( //
        ustore_key_t const vertex, ustore_vertex_role_t const role = ustore_vertex_role_any_k,
        flush_t flush = flush_t::dont_flush_k) noexcept {
        return remove_vertices({{&vertex}, 1}, {{&role}, 1}, flush);
    }

    status_t remove_edges(edges_view_t const& edges) noexcept {
        ustore_error_t error {};
        ustore_graph_remove_edges_t graph_remove_edges {};
        graph_remove_edges.db = database_;
        graph_remove_edges.error = &error;
        graph_remove_edges.transaction = transaction_;
        graph_remove_edges.arena = arena_;
        graph_remove_edges.tasks_count = edges.size();
        graph_remove_edges.collections = &collection_;
        graph_remove_edges.edges_ids = edges.edge_ids.begin().get();
        graph_remove_edges.edges_stride = edges.edge_ids.stride();
        graph_remove_edges.sources_ids = edges.source_ids.begin().get();
        graph_remove_edges.sources_stride = edges.source_ids.stride();
        graph_remove_edges.targets_ids = edges.target_ids.begin().get();
        graph_remove_edges.targets_stride = edges.target_ids.stride();

        ustore_graph_remove_edges(&graph_remove_edges);
        return status_t {error};
    }

    status_t remove_edge(edge_t const& edge) noexcept { return remove_edges(edges_view_t {&edge, &edge + 1}); }

    /** Removes every edge but keeps the vertices. */
    status_t remove_edges() noexcept { return drop(ustore_drop_vals_k); }

    status_t clear() noexcept { return drop(ustore_drop_keys_vals_k); }
    status_t remove() noexcept { return drop(ustore_drop_keys_vals_handle_k); }

    expected<ustore_vertex_degree_t> degree( //
        ustore_key_t vertex, ustore_vertex_role_t role = ustore_vertex_role_any_k,
        watch_t watch = watch_t::watch_k) noexcept {

        auto [found, degrees_status] = degrees({{&vertex}, 1}, {{&role}, 1}, watch);
        if (degrees_status.failed()) return degrees_status;
        return ustore_vertex_degree_t {found[0]};
    }

    expected<std::span<ustore_vertex_degree_t>> degrees( //
        strided_range<ustore_key_t const> vertices, strided_range<ustore_vertex_role_t const> roles = {},
        watch_t watch = watch_t::watch_k) noexcept {

        ustore_error_t error {};
        ustore_vertex_degree_t* degrees_per_vertex = nullptr;

        ustore_graph_find_edges_t graph_find_edges {};
        graph_find_edges.db = database_;
        graph_find_edges.error = &error;
        graph_find_edges.transaction = transaction_;
        graph_find_edges.snapshot = snapshot_;
        graph_find_edges.arena = arena_;
        graph_find_edges.options = to_options(watch);
        graph_find_edges.tasks_count = vertices.count();
        graph_find_edges.collections = &collection_;
        graph_find_edges.vertices = vertices.begin().get();
        graph_find_edges.vertices_stride = vertices.stride();
        graph_find_edges.roles = roles.begin().get();
        graph_find_edges.roles_stride = roles.stride();
        graph_find_edges.degrees_per_vertex = &degrees_per_vertex;

        ustore_graph_find_edges(&graph_find_edges);
        if (error.status) return status_t {error};
        return std::span<ustore_vertex_degree_t> {degrees_per_vertex, degrees_per_vertex + vertices.size()};
    }

    expected<bool> contains(ustore_key_t vertex, watch_t watch = watch_t::watch_k) noexcept {
        return blobs_ref<collection_key_field_t>(database_, transaction_, snapshot_,
                                                 collection_key_field_t {collection_, vertex}, arena_)
            .present(watch);
    }

    /** Checks which vertices are present, including those without any edges. */
    expected<bits_span_t> contains( //
        strided_range<ustore_key_t const> const& vertices, watch_t watch = watch_t::watch_k) noexcept {
        places_arg_t arg;
        arg.collections_begin = {&collection_, 0};
        arg.keys_begin = vertices.begin();
        arg.count = vertices.count();
        return blobs_ref<places_arg_t>(database_, transaction_, snapshot_, std::move(arg), arena_).present(watch);
    }

    expected<keys_stream_t> vertex_stream(
        std::size_t vertices_read_ahead = keys_stream_t::default_read_ahead_k) const noexcept {
        blobs_range_t members(database_, transaction_, snapshot_, collection_);
        return members.keys_begin(vertices_read_ahead);
    }

    /** Counts the vertices by streaming through all of them. */
    expected<std::size_t> number_of_vertices() noexcept {
        return keys_range_t {blobs_range_t {database_, transaction_, snapshot_, collection_}}.size();
    }

    /** Counts the edges by streaming through the outgoing edges of every vertex. */
    expected<std::size_t> number_of_edges() noexcept {
        graph_stream_t stream {
            database_,
            collection_,
            transaction_,
            snapshot_,
            keys_stream_t::default_read_ahead_k,
            ustore_vertex_source_k,
        };
        if (status_t const sought = stream.seek_to_first(); sought.failed()) return sought;

        std::size_t count = 0;
        for (; !stream.is_end(); ++stream) ++count;
        return count;
    }

    expected<adjacency_range_t> edges(
        ustore_vertex_role_t role = ustore_vertex_role_any_k,
        std::size_t vertices_read_ahead = keys_stream_t::default_read_ahead_k) const noexcept {

        graph_stream_t begin {database_, collection_, transaction_, snapshot_, vertices_read_ahead, role};
        graph_stream_t end {database_, collection_, transaction_, snapshot_, vertices_read_ahead, role};
        if (status_t const sought = begin.seek_to_first(); sought.failed()) return sought;
        if (status_t const sought = end.seek(ustore_key_unknown_k); sought.failed()) return sought;
        return adjacency_range_t {std::move(begin), std::move(end)};
    }

    expected<edges_span_t> edges_containing( //
        ustore_key_t vertex, ustore_vertex_role_t role = ustore_vertex_role_any_k,
        watch_t watch = watch_t::watch_k) noexcept {

        ustore_error_t error {};
        ustore_vertex_degree_t* degrees_per_vertex {};
        ustore_key_t* edges_per_vertex {};

        ustore_graph_find_edges_t graph_find_edges {};
        graph_find_edges.db = database_;
        graph_find_edges.error = &error;
        graph_find_edges.transaction = transaction_;
        graph_find_edges.snapshot = snapshot_;
        graph_find_edges.arena = arena_;
        graph_find_edges.options = to_options(watch);
        graph_find_edges.tasks_count = 1;
        graph_find_edges.collections = &collection_;
        graph_find_edges.vertices = &vertex;
        graph_find_edges.roles = &role;
        graph_find_edges.degrees_per_vertex = &degrees_per_vertex;
        graph_find_edges.edges_per_vertex = &edges_per_vertex;

        ustore_graph_find_edges(&graph_find_edges);
        if (error.status) return status_t {error};

        ustore_vertex_degree_t const edges_count = degrees_per_vertex[0];
        if (edges_count == ustore_vertex_degree_missing_k) return edges_span_t {};

        auto const edges_begin = reinterpret_cast<edge_t*>(edges_per_vertex);
        return edges_span_t {edges_begin, edges_begin + edges_count};
    }

    expected<edges_span_t> edges_between(ustore_key_t source, ustore_key_t target,
                                         watch_t watch = watch_t::watch_k) noexcept {
        auto maybe_all = edges_containing(source, ustore_vertex_source_k, watch);
        if (!maybe_all) return maybe_all;

        edges_span_t all = *maybe_all;
        auto const matches = std::ranges::equal_range(all.target_ids.begin(), all.target_ids.end(), target);
        std::size_t const offset = static_cast<std::size_t>(matches.begin() - all.target_ids.begin());
        std::size_t const count = static_cast<std::size_t>(matches.end() - matches.begin());
        all.source_ids = all.source_ids.subspan(offset, count);
        all.target_ids = all.target_ids.subspan(offset, count);
        all.edge_ids = all.edge_ids.subspan(offset, count);
        return all;
    }

    /** Finds every edge that has any of @p vertices in an allowed role,
     *  where undirected graphs may repeat an edge reversed. */
    expected<edges_span_t> edges_containing( //
        strided_range<ustore_key_t const> vertices, strided_range<ustore_vertex_role_t const> roles = {},
        watch_t watch = watch_t::watch_k) noexcept {

        ustore_error_t error {};
        ustore_vertex_degree_t* degrees_per_vertex = nullptr;
        ustore_key_t* edges_per_vertex = nullptr;

        ustore_graph_find_edges_t graph_find_edges {};
        graph_find_edges.db = database_;
        graph_find_edges.error = &error;
        graph_find_edges.transaction = transaction_;
        graph_find_edges.snapshot = snapshot_;
        graph_find_edges.arena = arena_;
        graph_find_edges.options = to_options(watch);
        graph_find_edges.tasks_count = vertices.count();
        graph_find_edges.collections = &collection_;
        graph_find_edges.vertices = vertices.begin().get();
        graph_find_edges.vertices_stride = vertices.stride();
        graph_find_edges.roles = roles.begin().get();
        graph_find_edges.roles_stride = roles.stride();
        graph_find_edges.degrees_per_vertex = &degrees_per_vertex;
        graph_find_edges.edges_per_vertex = &edges_per_vertex;

        ustore_graph_find_edges(&graph_find_edges);
        if (error.status) return status_t {error};

        auto const edges_begin = reinterpret_cast<edge_t*>(edges_per_vertex);
        std::size_t const edges_count = transform_reduce_n(
            degrees_per_vertex, vertices.size(), std::size_t {0},
            [](ustore_vertex_degree_t degree) { return degree == ustore_vertex_degree_missing_k ? 0 : degree; });
        return edges_span_t {edges_begin, edges_begin + edges_count};
    }

    expected<strided_range<ustore_key_t>> successors(ustore_key_t vertex) noexcept {
        auto [edges, edges_containing_status] = edges_containing(vertex, ustore_vertex_source_k);
        if (edges_containing_status.failed()) return edges_containing_status;
        return strided_range<ustore_key_t> {edges.target_ids};
    }

    expected<strided_range<ustore_key_t>> predecessors(ustore_key_t vertex) noexcept {
        auto [edges, edges_containing_status] = edges_containing(vertex, ustore_vertex_target_k);
        if (edges_containing_status.failed()) return edges_containing_status;
        return strided_range<ustore_key_t> {edges.source_ids};
    }

    /** Lists the distinct neighbors of @p vertex, reordering the fetched edges in place. */
    expected<strided_range<ustore_key_t>> neighbors(ustore_key_t vertex,
                                                    ustore_vertex_role_t role = ustore_vertex_role_any_k) noexcept {
        auto [edges, edges_containing_status] = edges_containing(vertex, role);
        if (edges_containing_status.failed()) return edges_containing_status;

        // Incoming and outgoing edges hold the neighbor in different columns, so every edge is flipped to target it
        for (std::size_t index = 0; index != edges.size(); ++index) {
            ustore_key_t& source = edges.source_ids[index];
            ustore_key_t& target = edges.target_ids[index];
            if (target == vertex) std::swap(source, target);
        }

        strided_range<ustore_key_t> const targets = edges.target_ids;
        std::size_t const count = sort_and_deduplicate(targets.begin(), targets.end());
        return strided_range<ustore_key_t> {targets.begin(), count};
    }
};

} // namespace unum::ustore
