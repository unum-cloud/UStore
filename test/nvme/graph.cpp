/**
 *  @file test/nvme/graph.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The graph store of the NVMe engine through the C ABI: one vertex past the old value bound, transactions
 *      adding edges to one vertex side by side, vertex removal, and reads through merges, flushes, compactions and a
 *      reopen, checked against a model.
 *
 *  The results this file exists for are @c concurrent_adds_commit and @c fold_survives_maintenance. Transactions
 *  adding edges to one vertex append change deltas rather than rewrite it, so every one of them commits, and the
 *  deltas fold into the vertex wherever they lie, in memory, in a flushed run or at the bottom of the tree.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::strlen`

#include <algorithm> // `std::sort`
#include <array>     // `std::array`
#include <set>       // `std::set`
#include <thread>    // `std::thread`
#include <tuple>     // `std::tuple`
#include <vector>    // `std::vector`

#include <fmt/format.h>

#include <forkunion/atomics.hpp> // `fu::standard_atomic_ref`

#include "ustore/ustore.hpp"

#include "harness.hpp" // `expect`, `expect_eq`, `report`, `suite_exit_code`, `directory_t`

using namespace unum::ustore;
using namespace unum::ustore::test;

#pragma region Shape

/** Edges one call of the hub case writes, so a batch stays one commit of bounded size. */
constexpr std::size_t hub_batch_edges_k = 20'000;

/** Edges the hub case leaves on one vertex: 9.6 MB of 48-byte half-edges, past the old 1 MiB value bound. */
constexpr std::size_t hub_edges_k = 200'000;

/** Threads the concurrency case adds edges to one vertex from. */
constexpr std::size_t adding_threads_k = 4;

/** Transactions every thread of the concurrency case commits. */
constexpr std::size_t transactions_per_thread_k = 64;

/** Vertices the maintenance case draws its edges between. */
constexpr std::uint64_t model_vertices_k = 48;

/** Edge identifiers the maintenance case draws from, so two vertices share parallel edges. */
constexpr std::uint64_t model_edge_ids_k = 4;

/** Calls the maintenance case makes, each a small commit of its own. */
constexpr std::size_t model_calls_k = 3000;

/** Bytes of memory runs every collection of the maintenance case holds before it flushes, small enough to flush many
 *  times. */
constexpr std::size_t small_memory_runs_bytes_k = 64 * 1024;

/** Parallel edges one round of the bottom case writes and then removes, a chain of 12 KiB per end. */
constexpr std::size_t bottom_round_edges_k = 256;

/** Rounds the bottom case runs, whose removals take 768 KiB of half-edge records unless the bottom applies them. */
constexpr std::size_t bottom_rounds_k = 32;

#pragma endregion Shape

#pragma region Edge Fixtures

/** Opens the NVMe engine over a manifest in @p directory, every collection flushing past
 *  @p memory_runs_bytes when not zero, every level @p enlarge_factor times the one above, and every
 *  value @p value_size bytes when not zero. */
expected<database_t> open_nvme(char const* directory, std::size_t memory_runs_bytes = 0, std::size_t enlarge_factor = 0,
                               std::size_t value_size = 0) noexcept {
    ustore_database_t database = nullptr;
    ustore_outcome_t outcome {};
    ustore_database_open_nvme_t open {};
    open.database = &database;
    open.outcome = &outcome;
    path_t const manifest {directory, "graph.ustore.json"};
    open.directory = {reinterpret_cast<ustore_byte_t const*>(manifest.c_str()), std::strlen(manifest.c_str())};
    if (memory_runs_bytes) {
        open.collection_memory_runs_max_bytes = memory_runs_bytes;
        open.collection_value_max_size = memory_runs_bytes / 4;
        open.transaction_max_bytes = memory_runs_bytes / 2;
    }
    open.collection_level_enlarge_factor = enlarge_factor;
    open.collection_value_size = value_size;
    ustore_database_open_nvme(&open);
    if (status_t const status {outcome}; status.failed()) return status;
    return database_t {database};
}

/** The edge from @p source to @p target named @p id, each spelled as the key of that integer. */
constexpr edge_t edge_of(std::uint64_t source, std::uint64_t target, std::uint64_t id) noexcept {
    return {key_from_integer(source), key_from_integer(target), key_from_integer(id)};
}

/** Views @p edges as the columns the graph calls take. */
edges_view_t edges_of(std::vector<edge_t> const& edges) noexcept {
    return edges_view_t(edges.data(), edges.data() + edges.size());
}

/** Answers whether @p graph reports @p wanted as the degree of @p vertex in @p role. */
std::size_t expect_degree(graph_collection_t& graph, std::uint64_t vertex, ustore_vertex_degree_t wanted,
                          ustore_vertex_role_t role = ustore_vertex_role_any_k) {
    auto [degree, degree_status] = graph.degree(key_from_integer(vertex), role);
    std::size_t failures = expect(degree_status.succeeded());
    if (degree_status.succeeded()) failures += expect_eq(degree, wanted);
    return failures;
}

#pragma endregion Edge Fixtures

#pragma region Model

/**
 *  @brief What a graph holds after a run of calls, kept the plain way: its vertices and its edges.
 *
 *  An edge write creates both ends, a removal of edges keeps the vertices and touches only the ends present, and a
 *  vertex removal takes every edge touching it.
 */
struct graph_model_t {
    using triple_t = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;

    std::set<std::uint64_t> vertices;
    std::set<triple_t> edges;

    void write_edge(std::uint64_t source, std::uint64_t target, std::uint64_t id) {
        vertices.insert(source);
        vertices.insert(target);
        edges.insert({source, target, id});
    }

    void remove_edge(std::uint64_t source, std::uint64_t target, std::uint64_t id) {
        edges.erase({source, target, id});
    }

    void remove_vertex(std::uint64_t vertex) {
        vertices.erase(vertex);
        for (auto iterator = edges.begin(); iterator != edges.end();)
            if (std::get<0>(*iterator) == vertex || std::get<1>(*iterator) == vertex) iterator = edges.erase(iterator);
            else ++iterator;
    }

    /** The degree of @p vertex in both roles, a loop counting once per role, or the missing degree. */
    ustore_vertex_degree_t degree(std::uint64_t vertex) const {
        if (!vertices.contains(vertex)) return ustore_vertex_degree_missing_k;
        ustore_vertex_degree_t degree = 0;
        for (triple_t const& edge : edges)
            degree += (std::get<0>(edge) == vertex ? 1u : 0u) + (std::get<1>(edge) == vertex ? 1u : 0u);
        return degree;
    }

    /** The edges leaving @p vertex in the order a read answers them: by target, then by identifier. */
    std::vector<edge_t> outgoing(std::uint64_t vertex) const {
        std::vector<edge_t> found;
        for (triple_t const& edge : edges)
            if (std::get<0>(edge) == vertex)
                found.push_back(edge_of(std::get<0>(edge), std::get<1>(edge), std::get<2>(edge)));
        std::sort(found.begin(), found.end(), [](edge_t const& first, edge_t const& second) {
            if (first.target != second.target) return first.target < second.target;
            return first.id < second.id;
        });
        return found;
    }
};

/** Compares every vertex of the model's range with @p graph: presence, degree in one batched read, and the edges
 *  leaving it. */
std::size_t expect_model(graph_collection_t& graph, graph_model_t const& model, arena_t& arena) {
    std::size_t failures = 0;
    std::array<ustore_key_t, model_vertices_k> keys;
    for (std::uint64_t vertex = 0; vertex != model_vertices_k; ++vertex) keys[vertex] = key_from_integer(vertex);
    auto [degrees, degrees_status] = graph.degrees(strided(keys).immutable(), arena);
    failures += expect(degrees_status.succeeded());
    if (degrees_status.failed()) return failures;
    for (std::uint64_t vertex = 0; vertex != model_vertices_k; ++vertex) {
        failures += expect_eq(degrees[vertex], model.degree(vertex));
        failures += expect_eq(*graph.present(keys[vertex]), model.vertices.contains(vertex));
    }
    for (std::uint64_t vertex = 0; vertex != model_vertices_k; ++vertex) {
        auto [found, found_status] = graph.read_edges(keys[vertex], arena, ustore_vertex_role_source_k);
        failures += expect(found_status.succeeded());
        if (found_status.failed()) continue;
        std::vector<edge_t> const wanted = model.outgoing(vertex);
        failures += expect_eq(found.size(), wanted.size());
        for (std::size_t index = 0; index != std::min(found.size(), wanted.size()); ++index)
            failures += expect(found[index] == wanted[index]);
    }
    return failures;
}

#pragma endregion Model

#pragma region Cases

/** One vertex with 200,000 half-edges, read whole in memory, from flushed runs, after compaction and after a reopen. */
case_result_t hub_past_value_bound(char const* directory) {
    if (!directory) return case_result_t::skipped(needs_directory_k);
    std::size_t failures = 0;
    path_t const home {directory, "hub"};
    if (!recreate_directory(home.c_str())) return failures + 1;

    auto const check = [&](graph_collection_t& graph) {
        std::size_t found_failures = expect_degree(graph, 0, hub_edges_k);
        found_failures += expect_degree(graph, 0, hub_edges_k, ustore_vertex_role_source_k);
        found_failures += expect_degree(graph, 0, 0, ustore_vertex_role_target_k);
        found_failures += expect_degree(graph, 1, 1);
        found_failures += expect_degree(graph, hub_edges_k, 1, ustore_vertex_role_target_k);
        auto [arena, arena_status] = arena_t::make();
        found_failures += expect(arena_status.succeeded());
        if (arena_status.failed()) return found_failures;
        auto [found, found_status] = graph.read_edges(key_from_integer(0), arena, ustore_vertex_role_source_k);
        found_failures += expect(found_status.succeeded());
        if (found_status.failed()) return found_failures;
        found_failures += expect_eq(found.size(), hub_edges_k);
        // Every half-edge of the hub is checked, so a single one lost or misordered fails the case once
        bool ordered = found.size() == hub_edges_k;
        for (std::size_t index = 0; ordered && index != hub_edges_k; ++index)
            ordered = found[index] == edge_of(0, index + 1, index + 1);
        found_failures += expect(ordered);
        return found_failures;
    };

    {
        auto [database, open_status] = open_nvme(home.c_str());
        failures += expect(open_status.succeeded());
        if (failures) return failures;
        auto [graph, graph_status] = database.create<graph_collection_t>("hub");
        failures += expect(graph_status.succeeded());
        if (failures) return failures;
        std::vector<edge_t> batch;
        for (std::size_t first = 0; first != hub_edges_k; first += hub_batch_edges_k) {
            batch.clear();
            for (std::size_t index = first; index != first + hub_batch_edges_k; ++index)
                batch.push_back(edge_of(0, index + 1, index + 1));
            status_t const written = graph.write_edges(edges_of(batch));
            if (written.failed()) fmt::println(stderr, "  hub write: {}", written.message ? written.message : "");
            failures += expect(written.succeeded());
        }
        failures += check(graph);
        failures += expect(database.compact().succeeded());
        failures += check(graph);
    }
    auto [database, reopen_status] = open_nvme(home.c_str());
    failures += expect(reopen_status.succeeded());
    if (failures) return failures;
    auto [graph, graph_status] = database.find<graph_collection_t>("hub");
    failures += expect(graph_status.succeeded());
    if (failures) return failures;
    failures += check(graph);
    return failures;
}

/** Transactions adding edges to one vertex commit side by side, interleaved on one thread and racing on several. */
case_result_t concurrent_adds_commit(char const* directory) {
    if (!directory) return case_result_t::skipped(needs_directory_k);
    std::size_t failures = 0;
    path_t const home {directory, "adds"};
    if (!recreate_directory(home.c_str())) return failures + 1;
    auto [database, open_status] = open_nvme(home.c_str());
    failures += expect(open_status.succeeded());
    if (failures) return failures;
    auto [graph, graph_status] = database.create<graph_collection_t>("adds");
    failures += expect(graph_status.succeeded());
    if (failures) return failures;
    std::array<ustore_key_t, 1> const hub {key_from_integer(0)};
    failures += expect(graph.write_vertices(strided(hub)).succeeded());

    transaction_t first = *database.transaction();
    transaction_t second = *database.transaction();
    auto [first_graph, first_status] = first.find<graph_collection_t>("adds");
    auto [second_graph, second_status] = second.find<graph_collection_t>("adds");
    failures += expect(first_status.succeeded() && second_status.succeeded());
    if (failures) return failures;
    failures += expect(first_graph.write_edge(edge_of(0, 1, 1)).succeeded());
    failures += expect(second_graph.write_edge(edge_of(0, 2, 2)).succeeded());
    failures += expect(first.commit().has_value());
    failures += expect(second.commit().has_value());
    failures += expect_degree(graph, 0, 2);

    // A transaction reads its own changes merged into the committed adjacency
    transaction_t reading = *database.transaction();
    auto [reading_graph, reading_status] = reading.find<graph_collection_t>("adds");
    failures += expect(reading_status.succeeded());
    if (failures) return failures;
    failures += expect(reading_graph.write_edge(edge_of(3, 0, 3)).succeeded());
    failures += expect_eq(*reading_graph.degree(key_from_integer(0)), 3u);
    failures += expect_eq(*reading_graph.degree(key_from_integer(0), ustore_vertex_role_target_k), 1u);
    failures += expect(reading.commit().has_value());
    failures += expect_degree(graph, 0, 3);

    std::size_t refused = 0;
    std::vector<std::thread> threads;
    for (std::size_t thread = 0; thread != adding_threads_k; ++thread)
        threads.emplace_back([&, thread] {
            for (std::size_t round = 0; round != transactions_per_thread_k; ++round) {
                auto [transaction, begin_status] = database.transaction();
                if (begin_status.failed()) {
                    fu::standard_atomic_ref<std::size_t> {refused}.add(1, std::memory_order_relaxed);
                    continue;
                }
                auto [adding, adding_status] = transaction.find<graph_collection_t>("adds");
                std::uint64_t const leaf = 1000 + thread * transactions_per_thread_k + round;
                bool const committed = adding_status.succeeded() &&
                                       adding.write_edge(edge_of(0, leaf, leaf)).succeeded() &&
                                       transaction.commit().has_value();
                if (!committed) fu::standard_atomic_ref<std::size_t> {refused}.add(1, std::memory_order_relaxed);
            }
        });
    for (std::thread& thread : threads) thread.join();
    failures += expect_eq(fu::standard_atomic_ref<std::size_t> {refused}.load(std::memory_order_acquire), 0u);
    failures += expect_degree(graph, 0, 3 + adding_threads_k * transactions_per_thread_k);
    return failures;
}

/** A removed vertex goes with every half-edge its neighbors filed of it, and a transaction that read a vertex it
 *  removes conflicts with an edge added to it meanwhile. */
case_result_t vertex_removal(char const* directory) {
    if (!directory) return case_result_t::skipped(needs_directory_k);
    std::size_t failures = 0;
    path_t const home {directory, "removal"};
    if (!recreate_directory(home.c_str())) return failures + 1;
    auto [database, open_status] = open_nvme(home.c_str());
    failures += expect(open_status.succeeded());
    if (failures) return failures;
    auto [graph, graph_status] = database.create<graph_collection_t>("removal");
    failures += expect(graph_status.succeeded());
    if (failures) return failures;
    // A vertex changes through the graph calls alone, as on the in-memory engine, never through a blobs write
    blobs_collection_t vertices {database.receiver(), *graph.member_ptr()};
    failures += expect(vertices[key_from_integer(0)].assign("vertex") == status_code_t::invalid_argument_k);
    failures += expect(vertices[key_from_integer(0)].remove() == status_code_t::invalid_argument_k);

    std::vector<edge_t> const star {edge_of(0, 1, 1), edge_of(0, 2, 2), edge_of(3, 0, 3), edge_of(0, 0, 4),
                                    edge_of(1, 2, 5)};
    failures += expect(graph.write_edges(edges_of(star)).succeeded());
    failures += expect_degree(graph, 0, 5);
    failures += expect(graph.remove_vertex(key_from_integer(0)).succeeded());
    failures += expect(!*graph.present(key_from_integer(0)));
    failures += expect_degree(graph, 0, ustore_vertex_degree_missing_k);
    failures += expect_degree(graph, 1, 1);
    failures += expect_degree(graph, 2, 1);
    failures += expect_degree(graph, 3, 0);
    failures += expect(*graph.present(key_from_integer(3)));

    // The removal's own transaction reads the vertex, which an edge landing meanwhile changes
    failures += expect(graph.write_edge(edge_of(1, 3, 6)).succeeded());
    transaction_t removing = *database.transaction();
    auto [removing_graph, removing_status] = removing.find<graph_collection_t>("removal");
    failures += expect(removing_status.succeeded());
    if (failures) return failures;
    failures += expect(removing_graph.remove_vertex(key_from_integer(3)).succeeded());
    failures += expect(graph.write_edge(edge_of(3, 2, 7)).succeeded());
    failures += expect(removing.commit().failed());
    failures += expect_degree(graph, 3, 2);
    failures += expect_degree(graph, 2, 2);
    return failures;
}

/** A database whose values share one width has no store for a graph, whose vertices vary in width, so creating one is
 *  refused rather than landing in a blobs store, while a blobs collection of that width is created and kept. */
case_result_t fixed_width_refuses_graph(char const* directory) {
    if (!directory) return case_result_t::skipped(needs_directory_k);
    std::size_t failures = 0;
    path_t const home {directory, "fixed"};
    if (!recreate_directory(home.c_str())) return failures + 1;
    auto [database, open_status] = open_nvme(home.c_str(), 0, 0, 8);
    failures += expect(open_status.succeeded());
    if (failures) return failures;
    failures += expect(database.create<graph_collection_t>("graph").status() == status_code_t::unsupported_k);
    auto [blobs, blobs_status] = database.create<blobs_collection_t>("blobs");
    failures += expect(blobs_status.succeeded());
    if (failures) return failures;
    ustore_key_t const key = key_from_integer(1);
    failures += expect(blobs[key].assign("12345678").succeeded());
    failures += expect(blobs[key].value() == "12345678");
    return failures;
}

/** Edges written and removed round after round leave no deltas behind once compactions reach the bottom level,
 *  where every chain applies onto nothing and its removals drop, rather than lying there as change deltas that
 *  every read folds again. */
case_result_t bottom_applies_chains(char const* directory) {
    if (!directory) return case_result_t::skipped(needs_directory_k);
    std::size_t failures = 0;
    path_t const home {directory, "bottom"};
    if (!recreate_directory(home.c_str())) return failures + 1;
    auto [database, open_status] = open_nvme(home.c_str(), small_memory_runs_bytes_k, 2);
    failures += expect(open_status.succeeded());
    if (failures) return failures;
    auto [graph, graph_status] = database.create<graph_collection_t>("bottom");
    failures += expect(graph_status.succeeded());
    if (failures) return failures;

    std::vector<edge_t> batch;
    for (std::size_t round = 0; round != bottom_rounds_k; ++round) {
        batch.clear();
        for (std::size_t index = 0; index != bottom_round_edges_k; ++index)
            batch.push_back(edge_of(0, 1, round * bottom_round_edges_k + index));
        failures += expect(graph.write_edges(edges_of(batch)).succeeded());
        failures += expect(graph.remove_edges(edges_of(batch)).succeeded());
    }
    failures += expect(database.compact().succeeded());
    failures += expect_degree(graph, 0, 0);
    failures += expect_degree(graph, 1, 0);

    // Unapplied, the newest record of every half-edge is its removal, two of 48 bytes per edge at the bottom
    auto [description, describe_status] = database.describe();
    failures += expect(describe_status.succeeded());
    std::size_t const removal_bytes = 2 * bottom_rounds_k * bottom_round_edges_k * 48;
    if (describe_status.succeeded()) failures += expect(description.disk_bytes < removal_bytes / 2);
    return failures;
}

/** Many small commits, removals among them, read against a model as memory runs merge, after a compaction folds them
 *  down to the bottom level, and after a reopen. */
case_result_t fold_survives_maintenance(char const* directory, random_t random) {
    if (!directory) return case_result_t::skipped(needs_directory_k);
    std::size_t failures = 0;
    path_t const home {directory, "maintenance"};
    if (!recreate_directory(home.c_str())) return failures + 1;
    graph_model_t model;
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures) return failures;

    {
        auto [database, open_status] = open_nvme(home.c_str(), small_memory_runs_bytes_k, 2);
        failures += expect(open_status.succeeded());
        if (failures) return failures;
        auto [graph, graph_status] = database.create<graph_collection_t>("maintenance");
        failures += expect(graph_status.succeeded());
        if (failures) return failures;

        std::vector<edge_t> batch;
        std::vector<graph_model_t::triple_t> drawn;
        for (std::size_t call = 0; call != model_calls_k; ++call) {
            std::uint64_t const draw = random.below(100);
            batch.clear();
            drawn.clear();
            std::size_t const count = 1 + random.below(4);
            for (std::size_t index = 0; index != count; ++index) {
                drawn.push_back(
                    {random.below(model_vertices_k), random.below(model_vertices_k), random.below(model_edge_ids_k)});
                auto const [source, target, id] = drawn.back();
                batch.push_back(edge_of(source, target, id));
            }
            if (draw < 70) {
                failures += expect(graph.write_edges(edges_of(batch)).succeeded());
                for (auto const& [source, target, id] : drawn) model.write_edge(source, target, id);
            }
            else if (draw < 90) {
                failures += expect(graph.remove_edges(edges_of(batch)).succeeded());
                for (auto const& [source, target, id] : drawn) model.remove_edge(source, target, id);
            }
            else {
                std::uint64_t const vertex = random.below(model_vertices_k);
                failures += expect(graph.remove_vertex(key_from_integer(vertex)).succeeded());
                model.remove_vertex(vertex);
            }
            if (call == model_calls_k / 2) failures += expect_model(graph, model, arena);
        }
        failures += expect_model(graph, model, arena);
        failures += expect(database.compact().succeeded());
        failures += expect_model(graph, model, arena);
    }
    auto [database, reopen_status] = open_nvme(home.c_str(), small_memory_runs_bytes_k, 2);
    failures += expect(reopen_status.succeeded());
    if (failures) return failures;
    auto [graph, graph_status] = database.find<graph_collection_t>("maintenance");
    failures += expect(graph_status.succeeded());
    if (failures) return failures;
    failures += expect_model(graph, model, arena);
    return failures;
}

#pragma endregion Cases

int main(int argc, char const* argv[]) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    st::expected<directory_t> const directory = directory_t::make(options.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    char const* const home = directory->path();
    report(totals, "graph.hub_past_value_bound", hub_past_value_bound, home);
    report(totals, "graph.concurrent_adds_commit", concurrent_adds_commit, home);
    report(totals, "graph.vertex_removal", vertex_removal, home);
    report(totals, "graph.fixed_width_refuses_graph", fixed_width_refuses_graph, home);
    report(totals, "graph.bottom_applies_chains", bottom_applies_chains, home);
    report(totals, "graph.fold_survives_maintenance", fold_survives_maintenance, home, options.random());
    return suite_exit_code(totals);
}
