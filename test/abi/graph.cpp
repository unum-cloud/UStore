/**
 *  @file test/abi/graph.cpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief Unit tests of the graph modality through the C++ wrappers.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstdio>  // `stderr`

#include <array>   // `std::array`
#include <utility> // `std::move`

#include <fmt/format.h>

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/ustore.hpp"

#include "abi/fixture.hpp" // `edge_of`, `open_compared`, `expect_same_scan`
#include "harness.hpp"     // `finish_test_run`, `expect_true`, `expect_equal`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::test::abi;
using namespace unum;

#pragma region Graph Modality

/** Vertices every graph case builds. */
constexpr std::size_t graph_vertices_count_k = 1000;

/** Stride between the vertices each vertex connects to, which leaves every degree at nine. */
constexpr std::size_t graph_connect_stride_k = 100;

/** Connects every vertex to each @c graph_connect_stride_k-th vertex after it, and answers whether
 *  every edge fit. */
[[nodiscard]] bool fill_edges(st::vector<edge_t>& edges) noexcept {
    std::uint64_t edge_id = 0;
    for (std::size_t vertex_id = 0; vertex_id != graph_vertices_count_k; ++vertex_id)
        for (std::size_t connect_with = vertex_id + graph_connect_stride_k; connect_with < graph_vertices_count_k;
             connect_with += graph_connect_stride_k) {
            ++edge_id;
            edge_t edge = edge_of(vertex_id, connect_with, edge_id);
            if (st::failed(edges.push_back(std::move(edge)))) return false;
        }
    return true;
}

/** Whether @p graph answers a degree for @p vertex, which a missing vertex never has. */
template <typename graph_type_>
bool has_vertex(graph_type_ const& graph, ustore_key_t vertex) noexcept {
    auto [degree, degree_status] = graph.degree(vertex);
    return !degree_status.failed() && degree != ustore_vertex_degree_missing_k;
}

/** Views @p edges as the columns the graph calls take. */
edges_view_t edges_of(st::vector<edge_t> const& edges) noexcept { return edges_view_t(edges.begin(), edges.end()); }

/** Writes disconnected vertices. */
void test_database_graph_write_vertices(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    expect_true(!graph_status.failed());
    std::array<ustore_key_t, 4> const vertices {key_from_integer(1), key_from_integer(4), key_from_integer(5),
                                                key_from_integer(2)};
    expect_true(!graph.write_vertices(strided(vertices)).failed());

    expect_true(has_vertex(graph, key_from_integer(1)));
    expect_true(has_vertex(graph, key_from_integer(2)));
    expect_true(has_vertex(graph, key_from_integer(4)));
    expect_true(has_vertex(graph, key_from_integer(5)));
}

/** Writes an edge together with its vertices. */
void test_database_graph_write_edge(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    expect_true(!graph_status.failed());
    edge_t const edge = edge_of(1, 2, 9);
    expect_true(!graph.write_edge(edge).failed());
    expect_true(has_vertex(graph, key_from_integer(1)));
    expect_true(has_vertex(graph, key_from_integer(2)));
    expect_true(answers(graph.degree(key_from_integer(3)), ustore_vertex_degree_missing_k));

    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return;
    auto const [neighbors, neighbors_status] = graph.neighbors(key_from_integer(1), arena);
    expect_true(!neighbors_status.failed());
    expect_equal(neighbors.size(), 1u);
    expect_equal(neighbors[0], key_from_integer(2));
}

/** Checks a triangle of three vertices and three directed edges, before and after removing its
 *  parts, reading every edge into @p arena. */
void check_triangle(graph_collection_t& graph, std::array<edge_t, 3>& triangle, arena_t& arena) {
    expect_true(has_vertex(graph, key_from_integer(1)));
    expect_true(has_vertex(graph, key_from_integer(2)));
    expect_true(answers(graph.degree(key_from_integer(9)), ustore_vertex_degree_missing_k));
    expect_true(answers(graph.degree(key_from_integer(10)), ustore_vertex_degree_missing_k));
    expect_true(answers(graph.degree(key_from_integer(1000)), ustore_vertex_degree_missing_k));

    expect_true(answers(graph.degree(key_from_integer(1)), 2u));
    expect_true(answers(graph.degree(key_from_integer(2)), 2u));
    expect_true(answers(graph.degree(key_from_integer(3)), 2u));
    expect_true(answers(graph.degree(key_from_integer(1), ustore_vertex_role_source_k), 1u));
    expect_true(answers(graph.degree(key_from_integer(2), ustore_vertex_role_source_k), 1u));
    expect_true(answers(graph.degree(key_from_integer(3), ustore_vertex_role_source_k), 1u));

    expect_true(!graph.read_edges(key_from_integer(1), arena).status.failed());
    expect_true(sized(graph.read_edges(key_from_integer(1), arena), 2));
    expect_true(sized(graph.read_edges(key_from_integer(1), arena, ustore_vertex_role_source_k), 1));
    expect_true(sized(graph.read_edges(key_from_integer(1), arena, ustore_vertex_role_target_k), 1));

    expect_true(sized(graph.read_edges(key_from_integer(3), arena, ustore_vertex_role_target_k), 1));
    expect_true(sized(graph.read_edges(key_from_integer(2), arena, ustore_vertex_role_source_k), 1));
    auto [incoming, incoming_status] = graph.read_edges(key_from_integer(3), arena, ustore_vertex_role_target_k);
    if (expect_true(!incoming_status.failed() && incoming.size() == 1)) {
        expect_equal(incoming[0].source, key_from_integer(2));
        expect_equal(incoming[0].target, key_from_integer(3));
        expect_equal(incoming[0].id, key_from_integer(10));
    }
    expect_true(sized(graph.edges_between(key_from_integer(3), key_from_integer(1), arena), 1));
    expect_true(sized(graph.edges_between(key_from_integer(1), key_from_integer(3), arena), 0));

    auto all_edges = graph.edges();
    for ([[maybe_unused]] auto const& edge : all_edges) {}
    expect_true(!all_edges.status().failed());
    {
        auto present_edges = graph.edges(ustore_vertex_role_source_k);
        std::size_t count = 0;
        for (auto const& edge : present_edges) {
            if (count < triangle.size()) expect_true(edge == triangle[count]);
            ++count;
        }
        expect_true(!present_edges.status().failed());
        expect_equal(count, triangle.size());
    }

    // Removing an edge keeps its vertices
    expect_true(!graph
                     .remove_edges(edges_view_t {
                         {{&triangle[0].source}, 1},
                         {{&triangle[0].target}, 1},
                         {{&triangle[0].id}, 1},
                     })
                     .failed());
    expect_true(has_vertex(graph, key_from_integer(1)));
    expect_true(has_vertex(graph, key_from_integer(2)));
    expect_true(sized(graph.edges_between(key_from_integer(1), key_from_integer(2), arena), 0));

    expect_true(!graph
                     .write_edges(edges_view_t {
                         {{&triangle[0].source}, 1},
                         {{&triangle[0].target}, 1},
                         {{&triangle[0].id}, 1},
                     })
                     .failed());
    expect_true(sized(graph.edges_between(key_from_integer(1), key_from_integer(2), arena), 1));

    ustore_key_t const vertex_to_remove = key_from_integer(2);
    expect_true(!graph.remove_vertex(vertex_to_remove).failed());
    expect_true(answers(graph.degree(vertex_to_remove), ustore_vertex_degree_missing_k));
    expect_true(sized(graph.read_edges(vertex_to_remove, arena), 0));
    expect_true(sized(graph.edges_between(key_from_integer(1), vertex_to_remove, arena), 0));
    expect_true(sized(graph.edges_between(vertex_to_remove, key_from_integer(1), arena), 0));

    expect_true(!graph.write_edges(edges(triangle)).failed());
    expect_true(has_vertex(graph, vertex_to_remove));
    expect_true(sized(graph.read_edges(vertex_to_remove, arena), 2));
    expect_true(sized(graph.edges_between(key_from_integer(1), vertex_to_remove, arena), 1));
    expect_true(sized(graph.edges_between(vertex_to_remove, key_from_integer(1), arena), 0));
}

/** Builds a triangle one edge at a time. */
void test_database_graph_triangle(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    expect_true(!graph_status.failed());
    std::array<edge_t, 3> triangle {edge_of(1, 2, 9), edge_of(2, 3, 10), edge_of(3, 1, 11)};
    expect_true(!graph.write_edge(triangle[0]).failed());
    expect_true(!graph.write_edge(triangle[1]).failed());
    expect_true(!graph.write_edge(triangle[2]).failed());

    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return;
    auto const [neighbors, neighbors_status] = graph.neighbors(key_from_integer(1), arena);
    expect_true(!neighbors_status.failed());
    expect_equal(neighbors.size(), 2u);
    expect_equal(neighbors[0], key_from_integer(2));
    expect_equal(neighbors[1], key_from_integer(3));

    check_triangle(graph, triangle, arena);
}

/** Builds a triangle in one batch, catching inconsistent concurrent updates behind
 *  one graph call. */
void test_database_graph_triangle_batch(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    expect_true(!graph_status.failed());
    std::array<edge_t, 3> triangle {edge_of(1, 2, 9), edge_of(2, 3, 10), edge_of(3, 1, 11)};
    expect_true(!graph.write_edges(edges(triangle)).failed());

    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return;
    check_triangle(graph, triangle, arena);
}

/** A transaction reading the middle of a wedge A-B-C must fail to commit once A-B
 *  changes outside it. */
case_result_t test_database_graph_transaction_watch(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    if (!probed(database.supports_transactions())) return case_result_t::skipped(needs_transactions_k);
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    expect_true(!graph_status.failed());
    edge_t const edge_ab = edge_of('A', 'B', 19);
    edge_t const edge_bc = edge_of('B', 'C', 31);
    expect_true(!graph.write_edge(edge_ab).failed());
    expect_true(!graph.write_edge(edge_bc).failed());

    auto [transaction, transaction_status] = database.transaction();
    if (!expect_true(!transaction_status.failed())) return {};
    auto [transaction_graph, transaction_graph_status] = transaction.find<graph_collection_t>("graph");
    expect_true(!transaction_graph_status.failed());
    expect_true(answers(transaction_graph.degree(key_from_integer('B')), 2u));
    expect_true(!transaction_graph.remove_edge(edge_bc).failed());
    expect_true(!graph.remove_edge(edge_ab).failed());

    expect_true(transaction.commit().status.failed());
    return {};
}

/** Builds a larger graph and checks every vertex degree. */
void test_database_graph_random_fill(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    expect_true(!graph_status.failed());
    st::vector<edge_t> generated;
    expect_true(fill_edges(generated));
    expect_true(!graph.write_edges(edges_of(generated)).failed());

    for (std::size_t index = 0; index != graph_vertices_count_k; ++index) {
        ustore_key_t const vertex_id = key_from_integer(index);
        expect_true(answers(graph.degree(vertex_id), 9u));
    }
}

/** Two transactions adding edges around a shared vertex: the later commit depends on stale
 *  state and fails. */
case_result_t test_database_graph_conflicting_transactions(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    if (!probed(database.supports_transactions())) return case_result_t::skipped(needs_transactions_k);
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    expect_true(!database.create<graph_collection_t>("graph").status.failed());
    auto [first_transaction, first_transaction_status] = database.transaction();
    if (!expect_true(!first_transaction_status.failed())) return {};
    auto [first_graph, first_graph_status] = first_transaction.find<graph_collection_t>("graph");
    auto [second_transaction, second_transaction_status] = database.transaction();
    if (!expect_true(!second_transaction_status.failed())) return {};
    auto [second_graph, second_graph_status] = second_transaction.find<graph_collection_t>("graph");
    expect_true(!first_graph_status.failed() && !second_graph_status.failed());

    edge_t const first_edge = edge_of(4, 5, 15);
    edge_t const second_edge = edge_of(5, 6, 16);

    expect_true(!first_graph.write_edge(first_edge).failed());
    expect_true(!second_graph.write_edge(second_edge).failed());
    expect_true(!first_transaction.commit().status.failed());
    expect_true(second_transaction.commit().status.failed());
    return {};
}

/** Layers a star, a pentagon and five self-loops over the same five vertices. */
void test_database_graph_layering_shapes(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    expect_true(!graph_status.failed());

    std::array<ustore_key_t, 5> const vertices {key_from_integer(1), key_from_integer(2), key_from_integer(3),
                                                key_from_integer(4), key_from_integer(5)};
    auto check_vertices = [&](ustore_vertex_degree_t degree) {
        for (ustore_key_t const vertex_id : vertices) { expect_true(answers(graph.degree(vertex_id), degree)); }
    };

    check_vertices(ustore_vertex_degree_missing_k);

    std::array<edge_t, 5> star {edge_of(1, 3, 1), edge_of(1, 4, 2), edge_of(2, 4, 3), edge_of(2, 5, 4),
                                edge_of(3, 5, 5)};
    std::array<edge_t, 5> pentagon {edge_of(1, 2, 6), edge_of(2, 3, 7), edge_of(3, 4, 8), edge_of(4, 5, 9),
                                    edge_of(5, 1, 10)};
    std::array<edge_t, 5> self_loops {edge_of(1, 1, 11), edge_of(2, 2, 12), edge_of(3, 3, 13), edge_of(4, 4, 14),
                                      edge_of(5, 5, 15)};

    expect_true(!graph.write_edges(edges(star)).failed());
    check_vertices(2);
    expect_true(!graph.write_edges(edges(pentagon)).failed());
    check_vertices(4);
    expect_true(!graph.remove_edges(edges(star)).failed());
    check_vertices(2);
    expect_true(!graph.write_edges(edges(star)).failed());
    check_vertices(4);
    expect_true(!graph.remove_edges(edges(pentagon)).failed());
    check_vertices(2);
    expect_true(!graph.write_edges(edges(pentagon)).failed());
    check_vertices(4);
    expect_true(!graph.write_edges(edges(self_loops)).failed());
    check_vertices(6);
    expect_true(!graph.remove_edges(edges(star)).failed());
    expect_true(!graph.remove_edges(edges(pentagon)).failed());
    check_vertices(2);
    expect_true(!graph.remove_edges(edges(self_loops)).failed());
    check_vertices(0);
    expect_true(!graph.clear().failed());
    check_vertices(ustore_vertex_degree_missing_k);
}

/** Removes vertices one by one and in a batch, each removal updating every neighbor. */
void test_database_graph_remove_vertices(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    expect_true(!graph_status.failed());
    st::vector<edge_t> generated;
    expect_true(fill_edges(generated));
    expect_true(!graph.write_edges(edges_of(generated)).failed());

    for (std::size_t index = 0; index != graph_vertices_count_k; ++index) {
        ustore_key_t const vertex_id = key_from_integer(index);
        expect_true(has_vertex(graph, vertex_id));
        expect_true(!graph.remove_vertex(vertex_id).failed());
        expect_true(answers(graph.degree(vertex_id), ustore_vertex_degree_missing_k));
    }

    expect_true(!graph.write_edges(edges_of(generated)).failed());
    std::array<ustore_key_t, graph_vertices_count_k> vertices;
    for (std::size_t index = 0; index != vertices.size(); ++index) vertices[index] = key_from_integer(index);
    expect_true(!graph.remove_vertices(strided(vertices).immutable()).failed());
    for (std::size_t index = 0; index != graph_vertices_count_k; ++index) {
        ustore_key_t const vertex_id = key_from_integer(index);
        expect_true(answers(graph.degree(vertex_id), ustore_vertex_degree_missing_k));
    }
}

/** Removing a known list of edges leaves the vertices in the graph, disconnected. */
void test_database_graph_remove_edges_keep_vertices(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    expect_true(!graph_status.failed());
    st::vector<edge_t> generated;
    expect_true(fill_edges(generated));
    expect_true(!graph.write_edges(edges_of(generated)).failed());
    expect_true(!graph.remove_edges(edges_of(generated)).failed());

    for (std::size_t index = 0; index != graph_vertices_count_k; ++index) {
        ustore_key_t const vertex_id = key_from_integer(index);
        expect_true(has_vertex(graph, vertex_id));
    }
}

/** Counts the edges of each vertex, then removes exactly those edges. */
void test_database_graph_get_vertex_edges(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    expect_true(!graph_status.failed());
    st::vector<edge_t> generated;
    expect_true(fill_edges(generated));
    expect_true(!graph.write_edges(edges_of(generated)).failed());

    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return;
    st::vector<edge_t> received;
    for (std::size_t index = 0; index != graph_vertices_count_k; ++index) {
        ustore_key_t const vertex_id = key_from_integer(index);
        auto [vertex_edges, vertex_edges_status] = graph.read_edges(vertex_id, arena);
        if (!expect_true(!vertex_edges_status.failed())) return;
        expect_equal(vertex_edges.size(), 9u);
        for (std::size_t index = 0; index != vertex_edges.size(); ++index) {
            edge_t edge = vertex_edges[index];
            expect_true(st::succeeded(received.push_back(std::move(edge))));
        }
    }
    expect_true(!graph.remove_edges(edges_of(received)).failed());

    for (std::size_t index = 0; index != graph_vertices_count_k; ++index) {
        ustore_key_t const vertex_id = key_from_integer(index);
        expect_true(has_vertex(graph, vertex_id));
        expect_true(sized(graph.read_edges(vertex_id, arena), 0));
    }
}

/** Reads the degrees of many vertices in one call. */
void test_database_graph_degrees(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    expect_true(!graph_status.failed());
    std::array<ustore_key_t, graph_vertices_count_k> vertices;
    for (std::size_t index = 0; index != vertices.size(); ++index) vertices[index] = key_from_integer(index);

    st::vector<edge_t> generated;
    expect_true(fill_edges(generated));
    expect_true(!graph.write_edges(edges_of(generated)).failed());

    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return;
    auto [degrees, degrees_status] = graph.degrees(strided(vertices).immutable(), arena);
    if (!expect_true(!degrees_status.failed())) return;
    expect_equal(degrees.size(), graph_vertices_count_k);
}

/** Neighbors of a vertex include itself through a self-loop and both directions of its edges. */
void test_database_graph_neighbors(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    expect_true(!graph_status.failed());
    expect_true(!graph.write_edge(edge_of(1, 1, 17)).failed());
    expect_true(!graph.write_edge(edge_of(1, 2, 15)).failed());
    expect_true(!graph.write_edge(edge_of(2, 3, 16)).failed());

    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return;
    auto const [first_neighbors, first_status] = graph.neighbors(key_from_integer(1), arena);
    expect_true(!first_status.failed());
    expect_equal(first_neighbors.size(), 2u);
    expect_equal(first_neighbors[0], key_from_integer(1));
    expect_equal(first_neighbors[1], key_from_integer(2));

    auto const [second_neighbors, second_status] = graph.neighbors(key_from_integer(2), arena);
    expect_true(!second_status.failed());
    expect_equal(second_neighbors.size(), 2u);
    expect_equal(second_neighbors[0], key_from_integer(1));
    expect_equal(second_neighbors[1], key_from_integer(3));
}

#pragma endregion Graph Modality

#pragma region Bounded Ranges

/** Writes integer vertices [0, 20) into both graphs of @p compared, each linked to the next. */
void write_range_vertices(compared_collections_t& compared) {
    std::array<ustore_key_t, 20> const vertices = keys_of_integers<20>(0);
    std::array<ustore_key_t, 19> const targets = keys_of_integers<19>(1);
    std::array<ustore_key_t, 19> const edges = keys_of_integers<19>(100);
    for (std::size_t side = 0; side != 2; ++side) {
        ustore_outcome_t outcome {};
        ustore_graph_write_vertices_t write {};
        write.database = compared.databases[side].handle();
        write.outcome = &outcome;
        write.count = vertices.size();
        write.collections = {&compared.collections[side], 0};
        write.vertices = {vertices.data(), sizeof(ustore_key_t)};
        ustore_graph_write_vertices(&write);
        expect_success(outcome);
        ustore_graph_write_edges_t connect {};
        connect.database = compared.databases[side].handle();
        connect.outcome = &outcome;
        connect.count = edges.size();
        connect.collections = {&compared.collections[side], 0};
        connect.edges = {edges.data(), sizeof(ustore_key_t)};
        connect.sources = {vertices.data(), sizeof(ustore_key_t)};
        connect.targets = {targets.data(), sizeof(ustore_key_t)};
        ustore_graph_write_edges(&connect);
        expect_success(outcome);
    }
}

/** A vertices scan stops before its end key. */
void test_graph_range_end_bound(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_graph_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_vertices(compared);
    ustore_key_t const start = key_from_integer(5), end = key_from_integer(9);
    expect_same_scan(ustore_graph_scan_vertices, compared, {&start, &end, ustore_scan_ascending_k, 10},
                     keys_of_integers<4>(5));
}

/** A descending vertices scan lists its range largest first, past every half-edge between them. */
void test_graph_range_descending(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_graph_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_vertices(compared);
    ustore_key_t const start = key_from_integer(14), end = key_from_integer(4);
    expect_same_scan(ustore_graph_scan_vertices, compared, {&start, &end, ustore_scan_descending_k, 3},
                     keys_of_integers<3>(12, ustore_scan_descending_k));
}

/** A range removal of vertices removes every vertex of the range with its edges, so its neighbors
 *  outside keep only their other edges. */
void test_graph_range_remove(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_graph_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_vertices(compared);
    ustore_key_t const start = key_from_integer(3), end = key_from_integer(17);
    std::array<ustore_key_t, 2> const neighbors {key_from_integer(2), key_from_integer(17)};
    for (std::size_t side = 0; side != 2; ++side) {
        ustore_outcome_t outcome {};
        ustore_graph_remove_vertices_t remove {};
        remove.database = compared.databases[side].handle();
        remove.outcome = &outcome;
        remove.count = 1;
        remove.collections = {&compared.collections[side], 0};
        remove.vertices = {&start, 0};
        remove.end_vertices = {&end, 0};
        ustore_graph_remove_vertices(&remove);
        expect_success(outcome);

        ustore_vertex_degree_t const* degrees = nullptr;
        ustore_graph_read_edges_t read {};
        read.database = compared.databases[side].handle();
        read.outcome = &outcome;
        read.count = neighbors.size();
        read.collections = {&compared.collections[side], 0};
        read.vertices = {neighbors.data(), sizeof(ustore_key_t)};
        read.degrees_per_vertex = &degrees;
        read.arena = compared.arenas[side].handle();
        ustore_graph_read_edges(&read);
        expect_success(outcome);
        if (outcome.status == ustore_status_success_k) expect_true(degrees && degrees[0] == 1 && degrees[1] == 1);
    }
    std::array<ustore_key_t, 6> const remaining {key_from_integer(0),  key_from_integer(1),  key_from_integer(2),
                                                 key_from_integer(17), key_from_integer(18), key_from_integer(19)};
    expect_same_scan(ustore_graph_scan_vertices, compared, {nullptr, nullptr, ustore_scan_ascending_k, 20}, remaining);
}

/** A vertices measure counts the vertices of its range, end excluded, whatever their edges. */
void test_graph_range_measure(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_graph_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_vertices(compared);
    ustore_key_t const lower = key_from_integer(5), upper = key_from_integer(9);
    expect_measure_holds(ustore_graph_measure_vertices, compared, {&lower, &upper, ustore_scan_ascending_k}, 4);
    expect_measure_holds(ustore_graph_measure_vertices, compared, {&upper, nullptr, ustore_scan_descending_k}, 10);
}

/** A vertices sample draws distinct written vertices, as many as asked or all when fewer exist. */
void test_graph_sample_vertices(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_graph_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_vertices(compared);
    std::array<ustore_key_t, 20> const written = keys_of_integers<20>(0);
    expect_samples_of(ustore_graph_sample_vertices, compared, written, 5, 5);
    expect_samples_of(ustore_graph_sample_vertices, compared, written, 30, 20);
}

#pragma endregion Bounded Ranges

int main(int argc, char** argv) {
    start_test_run(argc, argv);

    auto [directory, directory_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(directory_status)) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    config_t const config {test_run.settings.engine, directory.path()};

    run_test_case(engine_label_t {"test_database_graph_write_vertices"}.c_str(), test_database_graph_write_vertices,
                  config);
    run_test_case(engine_label_t {"test_database_graph_write_edge"}.c_str(), test_database_graph_write_edge, config);
    run_test_case(engine_label_t {"test_database_graph_triangle"}.c_str(), test_database_graph_triangle, config);
    run_test_case(engine_label_t {"test_database_graph_triangle_batch"}.c_str(), test_database_graph_triangle_batch,
                  config);
    run_test_case(engine_label_t {"test_database_graph_transaction_watch"}.c_str(),
                  test_database_graph_transaction_watch, config);
    run_test_case(engine_label_t {"test_database_graph_random_fill"}.c_str(), test_database_graph_random_fill, config);
    run_test_case(engine_label_t {"test_database_graph_conflicting_transactions"}.c_str(),
                  test_database_graph_conflicting_transactions, config);
    run_test_case(engine_label_t {"test_database_graph_layering_shapes"}.c_str(), test_database_graph_layering_shapes,
                  config);
    run_test_case(engine_label_t {"test_database_graph_remove_vertices"}.c_str(), test_database_graph_remove_vertices,
                  config);
    run_test_case(engine_label_t {"test_database_graph_remove_edges_keep_vertices"}.c_str(),
                  test_database_graph_remove_edges_keep_vertices, config);
    run_test_case(engine_label_t {"test_database_graph_get_vertex_edges"}.c_str(), test_database_graph_get_vertex_edges,
                  config);
    run_test_case(engine_label_t {"test_database_graph_degrees"}.c_str(), test_database_graph_degrees, config);
    run_test_case(engine_label_t {"test_database_graph_neighbors"}.c_str(), test_database_graph_neighbors, config);
    run_test_case(engine_label_t {"test_graph_range_end_bound"}.c_str(), test_graph_range_end_bound, config);
    run_test_case(engine_label_t {"test_graph_range_descending"}.c_str(), test_graph_range_descending, config);
    run_test_case(engine_label_t {"test_graph_range_remove"}.c_str(), test_graph_range_remove, config);
    run_test_case(engine_label_t {"test_graph_range_measure"}.c_str(), test_graph_range_measure, config);
    run_test_case(engine_label_t {"test_graph_sample_vertices"}.c_str(), test_graph_sample_vertices, config);
    return finish_test_run();
}
