/**
 *  @file include/ustore/graph_collection.hpp
 *  @author Ash Vardanian
 *  @date June 30, 2022
 *  @brief A handle to one graph collection with typed vertex and edge operations.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`

#include <algorithm> // `std::ranges::equal_range`
#include <span>      // `std::span`
#include <utility>   // `std::forward`, `std::move`, `std::swap`

#include "ustore/blobs_range.hpp" // `keys_range_t`
#include "ustore/blobs_ref.hpp"   // `blobs_ref`
#include "ustore/graph.h"
#include "ustore/graph_stream.hpp" // `graph_stream_t`
#include "ustore/ranges.hpp"       // `range`, `strided`, `transform_reduce_n`, `owning_view`
#include "ustore/ranges_args.hpp"  // `edges_span_t`, `edges_view_t`, `places_arg_t`
#include "ustore/status.hpp"       // `status_t`, `expected`
#include "ustore/types.hpp"        // `any_arena_t`, `collection_key_field_t`, `edge_t`

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

    status_t drop_with(ustore_drop_mode_t mode) noexcept {
        ustore_outcome_t outcome {};
        ustore_collection_drop_t collection_drop {};
        collection_drop.database = database_;
        collection_drop.outcome = &outcome;
        collection_drop.collection = collection_;
        collection_drop.mode = mode;
        ustore_collection_drop(&collection_drop);
        return status_t {outcome};
    }

    /** The view reads through this handle see, watching the vertices they read inside a transaction. */
    ustore_view_t view(ustore_watch_t watch) const noexcept { return view_through(transaction_, snapshot_, watch); }

    /** Finds the degrees of @p vertices into @p degrees, and their edges into the arena unless @p edges is null. */
    status_t find_edges(strided_range<ustore_key_t const> vertices, strided_range<ustore_vertex_role_t const> roles,
                        ustore_watch_t watch, ustore_vertex_degree_t* degrees, ustore_key_t** edges) noexcept {
        ustore_outcome_t outcome {};
        ustore_graph_find_edges_t graph_find_edges {};
        graph_find_edges.database = database_;
        graph_find_edges.outcome = &outcome;
        graph_find_edges.view = view(watch);
        graph_find_edges.count = vertices.count();
        graph_find_edges.collections = &collection_;
        graph_find_edges.vertices = vertices.begin().get();
        graph_find_edges.vertices_stride = vertices.stride();
        graph_find_edges.roles = roles.begin().get();
        graph_find_edges.roles_stride = roles.stride();
        graph_find_edges.degrees_per_vertex = degrees;
        graph_find_edges.degrees_per_vertex_stride = sizeof(ustore_vertex_degree_t);
        graph_find_edges.edges_per_vertex = edges;
        graph_find_edges.arena = arena_;

        ustore_graph_find_edges(&graph_find_edges);
        return status_t {outcome};
    }

  public:
    using adjacency_range_t = range<graph_stream_t>;

    graph_collection_t() noexcept = default;
    graph_collection_t(ustore_database_t database, ustore_collection_id_t collection = ustore_collection_main_k,
                       ustore_transaction_t transaction = nullptr, ustore_snapshot_t snapshot = {},
                       ustore_arena_t* arena = nullptr) noexcept
        : database_(database), collection_(collection), transaction_(transaction), snapshot_(snapshot), arena_(arena) {}

    graph_collection_t(graph_collection_t&&) = default;
    graph_collection_t& operator=(graph_collection_t&&) = default;
    graph_collection_t(graph_collection_t const&) = delete;
    graph_collection_t& operator=(graph_collection_t const&) = delete;

    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }

    status_t upsert_vertices(strided_range<ustore_key_t const> vertices) noexcept {
        ustore_outcome_t outcome {};
        ustore_graph_upsert_vertices_t upsert {};
        upsert.database = database_;
        upsert.outcome = &outcome;
        upsert.transaction = transaction_;
        upsert.count = vertices.size();
        upsert.collections = &collection_;
        upsert.vertices = vertices.data();
        upsert.vertices_stride = vertices.stride();

        ustore_graph_upsert_vertices(&upsert);
        return status_t {outcome};
    }

    template <typename keys_arg_>
    status_t upsert_vertices(keys_arg_&& vertices) noexcept {
        return upsert_vertices(strided(vertices).immutable());
    }

    status_t upsert_vertex(ustore_key_t const vertex) noexcept { return upsert_vertices({{&vertex}, 1}); }

    status_t upsert_edges(edges_view_t const& edges) noexcept {
        ustore_outcome_t outcome {};
        ustore_graph_upsert_edges_t graph_upsert_edges {};
        graph_upsert_edges.database = database_;
        graph_upsert_edges.outcome = &outcome;
        graph_upsert_edges.transaction = transaction_;
        graph_upsert_edges.count = edges.size();
        graph_upsert_edges.collections = &collection_;
        graph_upsert_edges.edges = edges.edge_ids.begin().get();
        graph_upsert_edges.edges_stride = edges.edge_ids.stride();
        graph_upsert_edges.sources = edges.source_ids.begin().get();
        graph_upsert_edges.sources_stride = edges.source_ids.stride();
        graph_upsert_edges.targets = edges.target_ids.begin().get();
        graph_upsert_edges.targets_stride = edges.target_ids.stride();

        ustore_graph_upsert_edges(&graph_upsert_edges);
        return status_t {outcome};
    }

    status_t upsert_edge(edge_t const& edge) noexcept { return upsert_edges(edges_view_t {&edge, &edge + 1}); }

    status_t remove_vertices( //
        strided_range<ustore_key_t const> vertices, strided_range<ustore_vertex_role_t const> roles = {},
        ustore_durability_t durability = ustore_durability_buffered_k) noexcept {

        ustore_outcome_t outcome {};
        ustore_graph_remove_vertices_t graph_remove_vertices {};
        graph_remove_vertices.database = database_;
        graph_remove_vertices.outcome = &outcome;
        graph_remove_vertices.transaction = transaction_;
        graph_remove_vertices.count = vertices.count();
        graph_remove_vertices.collections = &collection_;
        graph_remove_vertices.vertices = vertices.begin().get();
        graph_remove_vertices.vertices_stride = vertices.stride();
        graph_remove_vertices.roles = roles.begin().get();
        graph_remove_vertices.roles_stride = roles.stride();
        graph_remove_vertices.durability = durability;

        ustore_graph_remove_vertices(&graph_remove_vertices);
        return status_t {outcome};
    }

    template <typename keys_arg_>
    status_t remove_vertices(keys_arg_&& vertices) noexcept {
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
        graph_remove_edges.database = database_;
        graph_remove_edges.outcome = &outcome;
        graph_remove_edges.transaction = transaction_;
        graph_remove_edges.count = edges.size();
        graph_remove_edges.collections = &collection_;
        graph_remove_edges.edges = edges.edge_ids.begin().get();
        graph_remove_edges.edges_stride = edges.edge_ids.stride();
        graph_remove_edges.sources = edges.source_ids.begin().get();
        graph_remove_edges.sources_stride = edges.source_ids.stride();
        graph_remove_edges.targets = edges.target_ids.begin().get();
        graph_remove_edges.targets_stride = edges.target_ids.stride();

        ustore_graph_remove_edges(&graph_remove_edges);
        return status_t {outcome};
    }

    status_t remove_edge(edge_t const& edge) noexcept { return remove_edges(edges_view_t {&edge, &edge + 1}); }

    /** Removes every edge but keeps the vertices. */
    status_t remove_edges() noexcept { return drop_with(ustore_drop_vals_k); }

    status_t clear() noexcept { return drop_with(ustore_drop_keys_vals_k); }
    status_t drop() noexcept { return drop_with(ustore_drop_keys_vals_handle_k); }

    expected<ustore_vertex_degree_t> degree( //
        ustore_key_t vertex, ustore_vertex_role_t role = ustore_vertex_role_any_k,
        ustore_watch_t watch = ustore_watch_keys_k) noexcept {

        ustore_vertex_degree_t found = 0;
        if (status_t const status = find_edges({{&vertex}, 1}, {{&role}, 1}, watch, &found, nullptr); status.failed())
            return status;
        return found;
    }

    /** Reads the degree of every vertex into a span the result owns. */
    expected<owning_view<std::span<ustore_vertex_degree_t>>> degrees( //
        strided_range<ustore_key_t const> vertices, strided_range<ustore_vertex_role_t const> roles = {},
        ustore_watch_t watch = ustore_watch_keys_k) noexcept {

        st::basic_vector<std::byte> buffer;
        if (st::failed(buffer.resize(vertices.size() * sizeof(ustore_vertex_degree_t))))
            return status_t {status_code_t::out_of_memory_k, "Failed to allocate the degrees of vertices"};
        std::span<ustore_vertex_degree_t> const found {reinterpret_cast<ustore_vertex_degree_t*>(buffer.data()),
                                                       vertices.size()};
        if (status_t const status = find_edges(vertices, roles, watch, found.data(), nullptr); status.failed())
            return status;
        return owning_view<std::span<ustore_vertex_degree_t>> {found, std::move(buffer)};
    }

    expected<bool> contains(ustore_key_t vertex, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return blobs_ref<collection_key_field_t>(database_, transaction_, snapshot_,
                                                 collection_key_field_t {collection_, vertex}, arena_)
            .present(watch);
    }

    /** Checks which vertices are present, including those without any edges. */
    expected<owning_view<bits_span_t>> contains( //
        strided_range<ustore_key_t const> const& vertices, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        places_arg_t arg;
        arg.collections_begin = {&collection_, 0};
        arg.keys_begin = vertices.begin();
        arg.count = vertices.count();
        return blobs_ref<places_arg_t>(database_, transaction_, snapshot_, std::move(arg), arena_).present(watch);
    }

    expected<keys_stream_t> vertex_stream(
        std::size_t vertices_read_ahead = keys_stream_t::default_read_ahead_k) const noexcept {
        blobs_range_t members(database_, view(ustore_watch_keys_k), collection_);
        return members.keys_begin(vertices_read_ahead);
    }

    /** Counts the vertices by streaming through all of them. */
    expected<std::size_t> number_of_vertices() noexcept {
        return keys_range_t {blobs_range_t {database_, view(ustore_watch_keys_k), collection_}}.size();
    }

    /** Counts the edges by streaming through the outgoing edges of every vertex. */
    expected<std::size_t> number_of_edges() noexcept {
        auto [stream, stream_status] = graph_stream_t::make(database_, view(ustore_watch_keys_k), collection_,
                                                            ustore_vertex_source_k, smallest_key_k);
        if (stream_status.failed()) return stream_status;

        std::size_t count = 0;
        for (; !stream.is_end(); ++stream) ++count;
        return count;
    }

    expected<adjacency_range_t> edges(
        ustore_vertex_role_t role = ustore_vertex_role_any_k,
        std::size_t vertices_read_ahead = keys_stream_t::default_read_ahead_k) const noexcept {

        ustore_view_t const head = view(ustore_watch_keys_k);
        auto [begin, begin_status] = graph_stream_t::make(database_, head, collection_, role, smallest_key_k,
                                                          vertices_read_ahead);
        if (begin_status.failed()) return begin_status;
        auto [end, end_status] = graph_stream_t::make(database_, head, collection_, role, ustore_key_unknown_k,
                                                      vertices_read_ahead);
        if (end_status.failed()) return end_status;
        return adjacency_range_t {std::move(begin), std::move(end)};
    }

    expected<edges_span_t> edges_containing( //
        ustore_key_t vertex, ustore_vertex_role_t role = ustore_vertex_role_any_k,
        ustore_watch_t watch = ustore_watch_keys_k) noexcept {

        ustore_vertex_degree_t edges_count = 0;
        ustore_key_t* edges_per_vertex = nullptr;
        if (status_t const status = find_edges({{&vertex}, 1}, {{&role}, 1}, watch, &edges_count, &edges_per_vertex);
            status.failed())
            return status;
        if (edges_count == ustore_vertex_degree_missing_k) return edges_span_t {};

        edge_t* const edges_begin = reinterpret_cast<edge_t*>(edges_per_vertex);
        return edges_span_t {edges_begin, edges_begin + edges_count};
    }

    expected<edges_span_t> edges_between(ustore_key_t source, ustore_key_t target,
                                         ustore_watch_t watch = ustore_watch_keys_k) noexcept {
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
        ustore_watch_t watch = ustore_watch_keys_k) noexcept {

        std::size_t const count = vertices.size();
        st::basic_vector<ustore_vertex_degree_t> found_degrees;
        if (st::failed(found_degrees.resize(count)))
            return status_t {status_code_t::out_of_memory_k, "Failed to allocate the degrees of vertices"};
        ustore_key_t* edges_per_vertex = nullptr;
        if (status_t const status = find_edges(vertices, roles, watch, found_degrees.data(), &edges_per_vertex);
            status.failed())
            return status;

        edge_t* const edges_begin = reinterpret_cast<edge_t*>(edges_per_vertex);
        std::size_t const edges_count = transform_reduce_n(
            found_degrees.data(), count, std::size_t {0}, [](ustore_vertex_degree_t degree) noexcept {
                return degree == ustore_vertex_degree_missing_k ? 0 : degree;
            });
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
