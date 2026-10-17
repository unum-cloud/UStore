/**
 *  @file test/nvme/graph.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief A graph over two single-collection databases: what a walk sees, and what a commit shows.
 *
 *  The results this file exists for are @c commit_shows_both_halves_or_neither and
 *  @c blocks_split_and_merge. A reader walking a vertex while a transaction lands must see all of that
 *  transaction's edges or none of them, and an adjacency that outgrows one block must break into several
 *  the registry still routes every neighbor through.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`

#include <array>       // `std::array`
#include <atomic>      // `std::memory_order`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <thread>      // `std::thread`
#include <utility>     // `std::move`

#include <forkunion/atomics.hpp>       // `fu::preferred_atomic_ref`
#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/graph/nvme_graph.hpp" // `basic_graph_transaction`

#include "nvme/fixture.hpp" // `basic_fixture`, `run_on_transport`

namespace unum::ustore::test {

using namespace unum::ustore::blobs;
using namespace unum::ustore::graph;
using namespace unum::ustore::utilities;

#pragma region Shape

/** The numbers every case mounts with: channels for the graph, the case and every worker thread, two volumes each. */
constexpr fixture_config_t graph_config_k {
    .volume_bytes = 64ull * 1024ull * 1024ull,
    .shared_io_channels = 16,
    .memory_limit = std::size_t {512} * 1024 * 1024,
};

/** Threads the concurrency cases drive the graph on. */
constexpr std::size_t worker_threads_k = 4;

/** The graph and its transactions every case drives over the two volumes of @p fixture_type_. */
template <typename fixture_type_>
struct graph_types {
    using fixture_t = fixture_type_;
    using guard_t = typename fixture_t::guard_t;
    using volume_t = typename fixture_t::volume_t;
    using graph_store_t = basic_graph_store<volume_t>;
    using graph_transaction_t = basic_graph_transaction<graph_store_t>;

    /** A fixture, the channel the calling thread reads on, and the two volumes a graph lives on. */
    struct stack_t {

        /** The graph the two volumes carry. */
        using store_t = graph_store_t;

        /** The disk, the arena and the channels. */
        std::unique_ptr<fixture_t> fixture;

        /** The channel the calling thread reads on. */
        guard_t reading;

        /** The volume every registry lands on, pinned so a database may point at it. */
        std::unique_ptr<volume_t> vertices_volume;

        /** The volume every edge block lands on, pinned so a database may point at it. */
        std::unique_ptr<volume_t> half_edges_volume;

        /** The channel of the calling thread, which every read of this stack takes. */
        [[nodiscard]] io_channel_t& channel() noexcept { return reading.channel(); }
    };
};

/**
 *  @brief The bounds a case opens its graph with.
 *  @param[in] cache_limit Bytes each collection's value cache holds, which is zero to force every read.
 */
[[nodiscard]] graph_store_config_t test_config(std::size_t cache_limit) noexcept {
    graph_store_config_t config;
    config.transaction_max_elements = 4096;
    config.transaction_max_bytes = 4ull * 1024ull * 1024ull;
    config.concurrency_limit = 1;
    config.cache_limit = cache_limit;
    config.write_buffer_max_elements = 4096;
    config.write_buffer_max_bytes = 4ull * 1024ull * 1024ull;
    config.first_level_max_bytes = 16ull * 1024ull * 1024ull;
    config.level_enlarge_factor = 4;
    config.value_max_size = 256ull * 1024ull;
    return config;
}

/** The key spelling @p number, which orders among the others as the number does. */
[[nodiscard]] constexpr id_entity_t key_of(std::uint64_t number) noexcept { return key_from_integer(number); }

/** The edge of @p edge_id between @p first_id and @p second_id, which every case builds its graph from. */
[[nodiscard]] labeled_edge_t made_edge(id_entity_t edge_id, id_entity_t first_id, id_entity_t second_id, float weight,
                                       std::int32_t label, direction_t direction) noexcept {
    labeled_edge_t edge;
    edge.id = edge_id;
    edge.first = first_id;
    edge.second = second_id;
    edge.weight = weight;
    edge.label.raw = label;
    edge.direction = direction;
    return edge;
}

/** The directed edge of @p edge_id from @p first_id to @p second_id, weighted and labeled by its own number. */
[[nodiscard]] labeled_edge_t plain_edge(id_entity_t edge_id, id_entity_t first_id, id_entity_t second_id) noexcept {
    return made_edge(edge_id, first_id, second_id, 1.0f, 1, direction_t::directed_k);
}

#pragma endregion Shape

#pragma region Fixtures

/** Mounts a fixture through @p mount, takes the channel a case reads on and formats the two volumes a graph needs. */
template <typename mount_type_>
expected<std::unique_ptr<typename graph_types<typename mount_type_::fixture_t>::stack_t>> make_stack(
    mount_type_ const& mount) noexcept {
    using types_t = graph_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using volume_t = typename types_t::volume_t;
    auto stack = std::unique_ptr<stack_t> {new (std::nothrow) stack_t {}};
    if (!stack) return status_t {status_code_t::out_of_memory_k, "The stack did not fit"};

    expected<std::unique_ptr<typename types_t::fixture_t>> fixture = mount();
    if (!fixture) return fixture.status();
    stack->fixture = std::move(*fixture);

    expected<typename types_t::guard_t> reading = stack->fixture->channels.acquire();
    if (!reading) return reading.status();
    stack->reading = std::move(*reading);

    expected<volume_t> vertices = stack->fixture->mount_volume(stack->reading.channel(), 0);
    if (!vertices) return vertices.status();
    stack->vertices_volume = std::unique_ptr<volume_t> {new (std::nothrow) volume_t {std::move(*vertices)}};
    if (!stack->vertices_volume) return status_t {status_code_t::out_of_memory_k, "The volume did not fit"};

    expected<volume_t> half_edges = stack->fixture->mount_volume(stack->reading.channel(), 1);
    if (!half_edges) return half_edges.status();
    stack->half_edges_volume = std::unique_ptr<volume_t> {new (std::nothrow) volume_t {std::move(*half_edges)}};
    if (!stack->half_edges_volume) return status_t {status_code_t::out_of_memory_k, "The volume did not fit"};
    return stack;
}

/** Opens a graph over the two volumes and the channels of @p stack. */
template <typename stack_type_>
expected<typename stack_type_::store_t::graph_store_unique_ptr_t> open_over(
    stack_type_& stack, graph_store_config_t const& config) noexcept {
    return stack_type_::store_t::open(*stack.vertices_volume, *stack.half_edges_volume, stack.fixture->channels, config,
                                      stack.fixture->resources);
}

#pragma endregion Fixtures

#pragma region Inspection

/** Reads the registry of @p vertex_id straight out of the vertices database. */
template <typename graph_store_type_>
[[nodiscard]] std::size_t read_registry(graph_store_type_& store, io_channel_t& channel, id_entity_t vertex_id,
                                        vertex_registry_t& into) {
    std::size_t failures = 0;
    vertex_batch_t vertices;
    failures += expect(vertices.request(vertex_id).succeeded());
    failures += expect(vertices.resolve(store.vertices(), channel).succeeded());
    vertex_entry_t const* const entry = vertices.find(vertex_id);
    failures += expect(entry != nullptr && is_live(entry->state));
    if (failures != 0) return failures;
    into.clear();
    into.vertex_info = entry->record.vertex_info;
    for (std::size_t index = 0; index != entry->record.size(); ++index)
        failures += expect(into.upsert(entry->record[index]).succeeded());
    return failures;
}

/** Reads every edge block of @p vertex_id and lists the neighbors it carries, block by block. */
template <typename graph_store_type_>
[[nodiscard]] std::size_t read_neighbors_in_order(graph_store_type_& store, io_channel_t& channel,
                                                  id_entity_t vertex_id, st::basic_vector<id_entity_t>& into) {
    std::size_t failures = 0;
    vertex_registry_t registry;
    failures += read_registry(store, channel, vertex_id, registry);
    if (failures != 0) return failures;

    edge_block_batch_t blocks;
    for (id_entity_t const first_neighbor : registry.first_neighbors)
        failures += expect(blocks.request(id_chunk_t {vertex_id, first_neighbor}).succeeded());
    failures += expect(blocks.resolve(store.half_edges(), channel).succeeded());
    if (failures != 0) return failures;

    into.clear();
    for (std::size_t index = 0; index != blocks.size(); ++index) {
        edge_block_entry_t const& entry = blocks[index];
        failures += expect(is_live(entry.state));
        if (!is_live(entry.state)) continue;
        // Every block is keyed by its own first neighbor, and every block orders after the one before it.
        failures += expect_eq(entry.record.neighbor_ids.front(), entry.stored_key.chunk);
        failures += expect(entry.record.correct_order());
        failures += expect(entry.record.size() <= edge_block_t::max_count_k);
        for (id_entity_t const neighbor : entry.record.neighbor_ids)
            failures += expect(into.push_back(id_entity_t {neighbor}) == st::status_t::success_k);
    }
    return failures;
}

/** What a walk of one vertex saw: the neighbors its blocks carry, and the blocks nothing answered. */
struct walk_t {

    /** Half-edges the blocks the registry names carry together. */
    std::size_t neighbors = 0;

    /** Blocks the registry names that the half-edges database does not hold. */
    std::size_t missing_blocks = 0;
};

/** Reads the registry of @p vertex_id and then every edge block it names, exactly as a graph call does. */
template <typename graph_store_type_>
status_t walk_vertex(graph_store_type_& store, io_channel_t& channel, id_entity_t vertex_id, walk_t& into) {
    into = walk_t {};
    vertex_batch_t vertices;
    if (status_t const named = vertices.request(vertex_id); named.failed()) return named;
    if (status_t const read = vertices.resolve(store.vertices(), channel); read.failed()) return read;
    vertex_entry_t const* const entry = vertices.find(vertex_id);
    if (!entry || !is_live(entry->state)) return {};

    edge_block_batch_t blocks;
    for (id_entity_t const first_neighbor : entry->record.first_neighbors)
        if (status_t const named = blocks.request(id_chunk_t {vertex_id, first_neighbor}); named.failed()) return named;
    if (status_t const read = blocks.resolve(store.half_edges(), channel); read.failed()) return read;
    for (std::size_t index = 0; index != blocks.size(); ++index) {
        if (!is_live(blocks[index].state)) ++into.missing_blocks;
        else into.neighbors += blocks[index].record.size();
    }
    return {};
}

/** Whether @p first and @p second hold the same edges in the same order. */
[[nodiscard]] std::size_t expect_same_edges(edges_set_t const& first, edges_set_t const& second) {
    std::size_t failures = expect_eq(first.size(), second.size());
    if (failures != 0) return failures;
    auto left = first.begin();
    auto right = second.begin();
    for (; left != first.end(); ++left, ++right) failures += expect(labeled_edge_t::equals(*left, *right));
    return failures;
}

#pragma endregion Inspection

#pragma region Round Trip Cases

/** Vertices, directed edges and undirected ones come back with their weights, labels and roles. */
template <typename mount_type_>
std::size_t vertices_and_edges_round_trip(mount_type_ const& mount) {
    using types_t = graph_types<typename mount_type_::fixture_t>;
    using graph_store_t = typename types_t::graph_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    expected<typename graph_store_t::graph_store_unique_ptr_t> store = open_over(**stack,
                                                                                 test_config(1024ull * 1024ull));
    failures += expect(store.has_value());
    if (!store) return failures;
    io_channel_t& channel = (*stack)->channel();

    std::array<vertex_t, 5> wanted {};
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        wanted[index].id = key_of(index + 1);
        wanted[index].weight = 0.5f * static_cast<float>(index + 1);
        wanted[index].label.raw = static_cast<std::int32_t>(index + 10);
    }
    std::size_t created = 0;
    failures += expect(
        (*store)->access().upsert_vertices(channel, std::span<vertex_t const> {wanted}, created).succeeded());
    failures += expect_eq(created, wanted.size());

    for (vertex_t const& vertex : wanted) {
        vertex_t seen;
        failures += expect((*store)->access().find_vertex(channel, vertex.id, seen).succeeded());
        failures += expect_eq(seen.id, vertex.id);
        failures += expect_near(seen.weight, vertex.weight);
        failures += expect_eq(seen.label.raw, vertex.label.raw);
    }
    vertex_t missing;
    failures += expect_status((*store)->access().find_vertex(channel, key_of(99), missing), status_code_t::not_found_k);

    std::array<labeled_edge_t, 3> edges {
        made_edge(key_of(100), key_of(1), key_of(2), 2.5f, 7, direction_t::directed_k),
        made_edge(key_of(101), key_of(3), key_of(4), 1.5f, 8, direction_t::undirected_k),
        made_edge(key_of(102), key_of(2), key_of(3), 3.5f, 9, direction_t::directed_k),
    };
    graph_change_t changed;
    failures += expect(
        (*store)->access().upsert_edges(channel, std::span<labeled_edge_t const> {edges}, changed).succeeded());
    failures += expect_eq(changed.edges, std::size_t {3});
    failures += expect_eq(changed.vertices, std::size_t {0});

    // Writing the very same edges again changes their weights and labels but adds nothing.
    failures += expect(
        (*store)->access().upsert_edges(channel, std::span<labeled_edge_t const> {edges}, changed).succeeded());
    failures += expect_eq(changed.edges, std::size_t {0});

    std::size_t degree = 0;
    failures += expect((*store)->access().degree(channel, key_of(1), any_role_k, degree).succeeded());
    failures += expect_eq(degree, std::size_t {1});
    failures += expect((*store)->access().degree(channel, key_of(2), any_role_k, degree).succeeded());
    failures += expect_eq(degree, std::size_t {2});
    failures += expect((*store)->access().degree(channel, key_of(5), any_role_k, degree).succeeded());
    failures += expect_eq(degree, std::size_t {0});

    // The source of a directed edge sees it as a source, and the medium sees the very same edge as a medium.
    edges_set_t outgoing;
    failures += expect((*store)->access().edges_containing(channel, key_of(1), dedge_source_k, outgoing).succeeded());
    failures += expect_eq(outgoing.size(), std::size_t {1});
    if (outgoing.size() == 1) {
        labeled_edge_t const& seen = *outgoing.begin();
        failures += expect_eq(seen.id, key_of(100));
        failures += expect_eq(seen.first, key_of(1));
        failures += expect_eq(seen.second, key_of(2));
        failures += expect_near(seen.weight, 2.5f);
        failures += expect_eq(seen.label.raw, 7);
        failures += expect(seen.direction == direction_t::directed_k);
    }
    edges_set_t incoming;
    failures += expect((*store)->access().edges_containing(channel, key_of(2), dedge_target_k, incoming).succeeded());
    failures += expect_eq(incoming.size(), std::size_t {1});
    if (incoming.size() == 1) failures += expect_eq(incoming.begin()->id, key_of(100));
    edges_set_t wrong_role;
    failures += expect((*store)->access().edges_containing(channel, key_of(1), dedge_target_k, wrong_role).succeeded());
    failures += expect_eq(wrong_role.size(), std::size_t {0});

    // An undirected edge makes both of its ends plain members, whichever end is asked.
    for (id_entity_t const member : {key_of(3), key_of(4)}) {
        edges_set_t undirected;
        failures += expect((*store)->access().edges_containing(channel, member, edge_member_k, undirected).succeeded());
        failures += expect_eq(undirected.size(), std::size_t {1});
        if (undirected.size() != 1) continue;
        labeled_edge_t const& seen = *undirected.begin();
        failures += expect_eq(seen.id, key_of(101));
        failures += expect(seen.direction == direction_t::undirected_k);
        failures += expect_eq(seen.first, key_of(3));
        failures += expect_eq(seen.second, key_of(4));
        failures += expect_near(seen.weight, 1.5f);
    }

    vertex_ids_set_t neighbors;
    std::array<id_entity_t, 1> const second {key_of(2)};
    failures += expect(
        (*store)->access().neighbors(channel, std::span<id_entity_t const> {second}, neighbors).succeeded());
    failures += expect_eq(neighbors.size(), std::size_t {2});
    failures += expect(neighbors.find(key_of(1)) != nullptr);
    failures += expect(neighbors.find(key_of(3)) != nullptr);

    // Either end of a pair answers the lookup, and both answer the same edge.
    edges_set_t forward;
    edges_set_t backward;
    failures += expect(
        (*store)->access().edges_between(channel, key_of(1), any_role_k, key_of(2), any_role_k, forward).succeeded());
    failures += expect(
        (*store)->access().edges_between(channel, key_of(2), any_role_k, key_of(1), any_role_k, backward).succeeded());
    failures += expect_same_edges(forward, backward);
    failures += expect_eq(forward.size(), std::size_t {1});

    // Removing an edge takes it out from under both of its ends, and leaves the vertices behind.
    graph_change_t removed;
    failures += expect(
        (*store)->access().remove_edges(channel, std::span<labeled_edge_t const> {&edges[0], 1}, removed).succeeded());
    failures += expect_eq(removed.edges, std::size_t {1});
    failures += expect((*store)->access().degree(channel, key_of(1), any_role_k, degree).succeeded());
    failures += expect_eq(degree, std::size_t {0});
    failures += expect((*store)->access().degree(channel, key_of(2), any_role_k, degree).succeeded());
    failures += expect_eq(degree, std::size_t {1});
    vertex_t still_there;
    failures += expect((*store)->access().find_vertex(channel, key_of(1), still_there).succeeded());
    return failures;
}

/** Removing a vertex takes every edge that touched it, from both sides. */
template <typename mount_type_>
std::size_t removing_a_vertex_takes_its_edges(mount_type_ const& mount) {
    using types_t = graph_types<typename mount_type_::fixture_t>;
    using graph_store_t = typename types_t::graph_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t spokes_k = 40;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    expected<typename graph_store_t::graph_store_unique_ptr_t> store = open_over(**stack,
                                                                                 test_config(1024ull * 1024ull));
    failures += expect(store.has_value());
    if (!store) return failures;
    io_channel_t& channel = (*stack)->channel();

    st::basic_vector<labeled_edge_t> edges;
    failures += expect(edges.resize(spokes_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != spokes_k; ++index)
        edges[index] = plain_edge(key_of(1000 + index), key_of(1), key_of(index + 2));

    graph_change_t changed;
    failures += expect(
        (*store)->access().upsert_edges(channel, std::span<labeled_edge_t const> {edges}, changed).succeeded());
    failures += expect_eq(changed.edges, spokes_k);
    failures += expect_eq(changed.vertices, spokes_k + 1);

    std::array<id_entity_t, 1> const hub {key_of(1)};
    graph_change_t removed;
    failures += expect(
        (*store)->access().remove_vertices(channel, std::span<id_entity_t const> {hub}, removed).succeeded());
    failures += expect_eq(removed.vertices, std::size_t {1});
    failures += expect_eq(removed.edges, spokes_k);

    vertex_t gone;
    failures += expect_status((*store)->access().find_vertex(channel, key_of(1), gone), status_code_t::not_found_k);
    for (std::size_t index = 0; index != spokes_k; ++index) {
        std::size_t degree = 1;
        failures += expect((*store)->access().degree(channel, key_of(index + 2), any_role_k, degree).succeeded());
        failures += expect_eq(degree, std::size_t {0});
    }
    return failures;
}

#pragma endregion Round Trip Cases

#pragma region Block Cases

/** An adjacency that outgrows one block breaks into several, and a removal folds them back together. */
template <typename mount_type_>
std::size_t blocks_split_and_merge(mount_type_ const& mount) {
    using types_t = graph_types<typename mount_type_::fixture_t>;
    using graph_store_t = typename types_t::graph_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t spokes_k = 600;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    expected<typename graph_store_t::graph_store_unique_ptr_t> store = open_over(**stack,
                                                                                 test_config(1024ull * 1024ull));
    failures += expect(store.has_value());
    if (!store) return failures;
    io_channel_t& channel = (*stack)->channel();

    st::basic_vector<labeled_edge_t> edges;
    failures += expect(edges.resize(spokes_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != spokes_k; ++index)
        edges[index] = plain_edge(key_of(1000 + index), key_of(1), key_of(index + 2));

    graph_change_t changed;
    failures += expect(
        (*store)->access().upsert_edges(channel, std::span<labeled_edge_t const> {edges}, changed).succeeded());
    failures += expect_eq(changed.edges, spokes_k);

    vertex_registry_t registry;
    failures += read_registry(**store, channel, key_of(1), registry);
    failures += expect(registry.size() > std::size_t {1});
    // A split aims at the optimal count, so the blocks stay packed rather than one per neighbor.
    failures += expect(registry.size() <= 2 * (spokes_k / edge_block_t::optimal_count_k) + 1);

    // Every neighbor is still there, once, and the blocks hand them over in neighbor order.
    st::basic_vector<id_entity_t> walked;
    failures += read_neighbors_in_order(**store, channel, key_of(1), walked);
    failures += expect_eq(walked.size(), spokes_k);
    for (std::size_t index = 0; index + 1 < walked.size(); ++index)
        failures += expect(walked[index] < walked[index + 1]);
    if (walked.size() == spokes_k) {
        failures += expect_eq(walked.front(), key_of(2));
        failures += expect_eq(walked.back(), key_of(spokes_k + 1));
    }

    std::size_t degree = 0;
    failures += expect((*store)->access().degree(channel, key_of(1), any_role_k, degree).succeeded());
    failures += expect_eq(degree, spokes_k);
    std::size_t const blocks_before = registry.size();

    // Keeping a couple of neighbors out of every block leaves each of them under its bound.
    std::array<id_entity_t, 10> const kept {key_of(2),   key_of(3),   key_of(130), key_of(131), key_of(260),
                                            key_of(261), key_of(400), key_of(401), key_of(550), key_of(551)};
    st::basic_vector<labeled_edge_t> doomed;
    for (std::size_t index = 0; index != spokes_k; ++index) {
        id_entity_t const neighbor = key_of(index + 2);
        bool spared = false;
        for (id_entity_t const keep : kept) spared |= keep == neighbor;
        if (spared) continue;
        failures += expect(doomed.push_back(labeled_edge_t {edges[index]}) == st::status_t::success_k);
    }
    graph_change_t removed;
    failures += expect(
        (*store)->access().remove_edges(channel, std::span<labeled_edge_t const> {doomed}, removed).succeeded());
    failures += expect_eq(removed.edges, spokes_k - kept.size());

    // What is left fits in one block, so every under-full block took the one after it.
    failures += read_registry(**store, channel, key_of(1), registry);
    failures += expect(registry.size() < blocks_before);
    failures += expect_eq(registry.size(), std::size_t {1});
    failures += expect((*store)->access().degree(channel, key_of(1), any_role_k, degree).succeeded());
    failures += expect_eq(degree, kept.size());

    failures += read_neighbors_in_order(**store, channel, key_of(1), walked);
    failures += expect_eq(walked.size(), kept.size());
    for (std::size_t index = 0; index != walked.size() && index != kept.size(); ++index)
        failures += expect_eq(walked[index], kept[index]);
    return failures;
}

#pragma endregion Block Cases

#pragma region Batch Cases

/** Looking many vertex pairs up together answers exactly what looking each one up alone answers. */
template <typename mount_type_>
std::size_t batched_pairs_agree_with_single_ones(mount_type_ const& mount) {
    using types_t = graph_types<typename mount_type_::fixture_t>;
    using graph_store_t = typename types_t::graph_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t vertices_k = 200;
    constexpr std::size_t edges_k = 600;
    constexpr std::size_t pairs_k = 300;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    expected<typename graph_store_t::graph_store_unique_ptr_t> store = open_over(**stack,
                                                                                 test_config(1024ull * 1024ull));
    failures += expect(store.has_value());
    if (!store) return failures;
    io_channel_t& channel = (*stack)->channel();

    st::basic_vector<labeled_edge_t> edges;
    failures += expect(edges.resize(edges_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != edges_k; ++index) {
        id_entity_t const first = key_of(index % vertices_k + 1);
        id_entity_t const second = key_of((index * 7 + 3) % vertices_k + 1);
        edges[index] = made_edge(key_of(index + 1), first, second, 1.0f + 0.25f * (index % 4),
                                 static_cast<std::int32_t>(index % 3) + 1,
                                 index % 2 == 0 ? direction_t::directed_k : direction_t::undirected_k);
    }
    graph_change_t changed;
    failures += expect(
        (*store)->access().upsert_edges(channel, std::span<labeled_edge_t const> {edges}, changed).succeeded());
    failures += expect((*store)->flush().succeeded());

    st::basic_vector<id_entity_t> firsts;
    st::basic_vector<id_entity_t> seconds;
    failures += expect(firsts.resize(pairs_k) == st::status_t::success_k);
    failures += expect(seconds.resize(pairs_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t pair = 0; pair != pairs_k; ++pair) {
        firsts[pair] = key_of(pair % vertices_k + 1);
        seconds[pair] = key_of((pair * 7 + 3) % vertices_k + 1);
    }

    edges_set_t batched;
    failures += expect((*store)
                           ->access()
                           .edges_between(channel, std::span<id_entity_t const> {firsts},
                                          std::span<id_entity_t const> {seconds}, batched)
                           .succeeded());

    edges_set_t alone;
    for (std::size_t pair = 0; pair != pairs_k; ++pair)
        failures += expect((*store)
                               ->access()
                               .edges_between(channel, firsts[pair], any_role_k, seconds[pair], any_role_k, alone)
                               .succeeded());
    failures += expect(batched.size() > std::size_t {0});
    failures += expect_same_edges(batched, alone);
    return failures;
}

/** A batched read of the edge blocks keeps as many requests in flight as the queue pair has credit for. */
template <typename mount_type_>
std::size_t batched_read_fills_the_queue_pair(mount_type_ const& mount) {
    using types_t = graph_types<typename mount_type_::fixture_t>;
    using graph_store_t = typename types_t::graph_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t spokes_k = 400;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    // No value cache, so every block of the batch is a request the medium has to answer.
    expected<typename graph_store_t::graph_store_unique_ptr_t> store = open_over(**stack, test_config(0));
    failures += expect(store.has_value());
    if (!store) return failures;
    io_channel_t& channel = (*stack)->channel();

    st::basic_vector<labeled_edge_t> edges;
    failures += expect(edges.resize(spokes_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != spokes_k; ++index)
        edges[index] = plain_edge(key_of(1000 + index), key_of(index * 2 + 2), key_of(index * 2 + 3));

    graph_change_t changed;
    failures += expect(
        (*store)->access().upsert_edges(channel, std::span<labeled_edge_t const> {edges}, changed).succeeded());
    failures += expect((*store)->flush().succeeded());

    st::basic_vector<id_chunk_t> keys;
    for (std::size_t index = 0; index != spokes_k; ++index) {
        id_entity_t const vertex_id = key_of(index * 2 + 2);
        vertex_registry_t registry;
        failures += read_registry(**store, channel, vertex_id, registry);
        for (id_entity_t const first_neighbor : registry.first_neighbors)
            failures += expect(keys.push_back(id_chunk_t {vertex_id, first_neighbor}) == st::status_t::success_k);
    }
    failures += expect_eq(keys.size(), spokes_k);
    if (failures != 0) return failures;

    st::basic_vector<select_result_t> answers;
    failures += expect(answers.resize(keys.size()) == st::status_t::success_k);
    if (failures != 0) return failures;

    std::uint32_t idlest = graph_config_k.request_buffers;
    std::size_t arrivals = 0;
    auto counting = [&](std::size_t, std::uint64_t, std::span<byte_t const>) noexcept {
        ++arrivals;
        std::uint32_t const free_blocks = channel.requests().available_buffers();
        if (free_blocks < idlest) idlest = free_blocks;
    };
    failures += expect(
        (*store)
            ->half_edges()
            .select(channel, std::span<id_chunk_t const> {keys}, std::span<select_result_t> {answers}, counting)
            .succeeded());
    failures += expect_eq(arrivals, keys.size());
    for (select_result_t const answer : answers) failures += expect(answer == select_result_t::success_k);
    failures += expect_eq(graph_config_k.request_buffers - idlest, graph_config_k.queue_depth);
    return failures;
}

#pragma endregion Batch Cases

#pragma region Durability Cases

/** A graph closed and opened again over the same volumes holds every vertex and every edge it held. */
template <typename mount_type_>
std::size_t graph_survives_a_reopen(mount_type_ const& mount) {
    using types_t = graph_types<typename mount_type_::fixture_t>;
    using graph_store_t = typename types_t::graph_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t vertices_k = 120;
    constexpr std::size_t edges_k = 400;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    graph_store_config_t const config = test_config(1024ull * 1024ull);

    st::basic_vector<labeled_edge_t> edges;
    failures += expect(edges.resize(edges_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != edges_k; ++index)
        edges[index] = made_edge(key_of(index + 1), key_of(index % vertices_k + 1),
                                 key_of((index * 11 + 5) % vertices_k + 1), 2.0f,
                                 static_cast<std::int32_t>(index % 3) + 1, direction_t::directed_k);

    edges_set_t before;
    {
        expected<typename graph_store_t::graph_store_unique_ptr_t> store = open_over(**stack, config);
        failures += expect(store.has_value());
        if (!store) return failures;
        io_channel_t& channel = (*stack)->channel();

        graph_change_t changed;
        failures += expect(
            (*store)->access().upsert_edges(channel, std::span<labeled_edge_t const> {edges}, changed).succeeded());
        // Two flushed rounds and a merge give the trees depth, so a reopen that lost one would lose keys.
        failures += expect((*store)->flush().succeeded());
        failures += expect(
            (*store)
                ->access()
                .upsert_edges(channel, std::span<labeled_edge_t const> {edges.data(), edges.size() / 2}, changed)
                .succeeded());
        failures += expect((*store)->flush().succeeded());

        for (std::size_t index = 0; index != vertices_k; ++index)
            failures += expect(
                (*store)->access().edges_containing(channel, key_of(index + 1), any_role_k, before).succeeded());
        failures += expect(before.size() > std::size_t {0});
        if (failures != 0) return failures;
    }

    expected<typename graph_store_t::graph_store_unique_ptr_t> store = open_over(**stack, config);
    failures += expect(store.has_value());
    if (!store) return failures;
    io_channel_t& channel = (*stack)->channel();

    edges_set_t after;
    for (std::size_t index = 0; index != vertices_k; ++index)
        failures += expect(
            (*store)->access().edges_containing(channel, key_of(index + 1), any_role_k, after).succeeded());
    failures += expect_same_edges(before, after);
    return failures;
}

#pragma endregion Durability Cases

#pragma region Transaction Cases

/** A transaction spanning both databases is refused when another one wrote the vertex it read. */
template <typename mount_type_>
std::size_t conflicting_transactions_are_refused(mount_type_ const& mount) {
    using types_t = graph_types<typename mount_type_::fixture_t>;
    using graph_store_t = typename types_t::graph_store_t;
    using graph_transaction_t = typename types_t::graph_transaction_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    expected<typename graph_store_t::graph_store_unique_ptr_t> store = open_over(**stack,
                                                                                 test_config(1024ull * 1024ull));
    failures += expect(store.has_value());
    if (!store) return failures;
    io_channel_t& channel = (*stack)->channel();

    std::array<vertex_t, 1> const hub {vertex_t {key_of(1), 1.0f, label_t {1}}};
    std::size_t created = 0;
    failures += expect(
        (*store)->access().upsert_vertices(channel, std::span<vertex_t const> {hub}, created).succeeded());

    expected<typename graph_transaction_t::graph_transaction_unique_ptr_t> first = graph_transaction_t::begin(**store,
                                                                                                              1);
    expected<typename graph_transaction_t::graph_transaction_unique_ptr_t> second = graph_transaction_t::begin(**store,
                                                                                                               2);
    failures += expect(first.has_value());
    failures += expect(second.has_value());
    if (!first || !second) return failures;

    labeled_edge_t const one = plain_edge(key_of(10), key_of(1), key_of(2));
    labeled_edge_t const other = plain_edge(key_of(11), key_of(1), key_of(3));
    graph_change_t changed;
    failures += expect(
        (*first)->access().upsert_edges(channel, std::span<labeled_edge_t const> {&one, 1}, changed).succeeded());
    failures += expect(
        (*second)->access().upsert_edges(channel, std::span<labeled_edge_t const> {&other, 1}, changed).succeeded());

    failures += expect((*first)->commit().succeeded());
    failures += expect_status((*second)->commit(), status_code_t::write_conflict_k);
    (*second)->rollback();

    std::size_t degree = 0;
    failures += expect((*store)->access().degree(channel, key_of(1), any_role_k, degree).succeeded());
    failures += expect_eq(degree, std::size_t {1});

    // The refused transaction starts again from the rollback, so doing the work once more lands it.
    failures += expect(
        (*second)->access().upsert_edges(channel, std::span<labeled_edge_t const> {&other, 1}, changed).succeeded());
    failures += expect((*second)->commit().succeeded());
    failures += expect((*store)->access().degree(channel, key_of(1), any_role_k, degree).succeeded());
    failures += expect_eq(degree, std::size_t {2});
    return failures;
}

/**
 *  @brief A walk of a vertex sees every edge one transaction landed on it, or none of them.
 *
 *  A walk reads the registry before the blocks it names, so it waits behind whichever half of a commit
 *  has not landed; a count between zero and the whole batch would mean it read across the two databases.
 */
template <typename mount_type_>
std::size_t commit_shows_both_halves_or_neither(mount_type_ const& mount) {
    using types_t = graph_types<typename mount_type_::fixture_t>;
    using guard_t = typename types_t::guard_t;
    using graph_store_t = typename types_t::graph_store_t;
    using graph_transaction_t = typename types_t::graph_transaction_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t hubs_k = 200;
    constexpr std::size_t spokes_k = 12;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    expected<typename graph_store_t::graph_store_unique_ptr_t> store = open_over(**stack,
                                                                                 test_config(1024ull * 1024ull));
    failures += expect(store.has_value());
    if (!store) return failures;
    io_channel_t& channel = (*stack)->channel();

    st::basic_vector<vertex_t> hubs;
    failures += expect(hubs.resize(hubs_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != hubs_k; ++index) hubs[index].id = key_of(index + 1);
    std::size_t created = 0;
    failures += expect(
        (*store)->access().upsert_vertices(channel, std::span<vertex_t const> {hubs}, created).succeeded());

    // The writer says which hub it is landing, so the reader watches that one rather than sweeping them all.
    std::int32_t landing = -1;
    std::int32_t stop = 0;
    std::size_t torn = 0;
    std::size_t watched = 0;
    std::size_t reader_failures = 0;
    std::thread reader {[&]() noexcept {
        expected<guard_t> guard = (*stack)->fixture->channels.acquire();
        reader_failures += expect(guard.has_value());
        if (!guard) return;
        fu::preferred_atomic_ref<std::int32_t> const flag {stop};
        fu::preferred_atomic_ref<std::int32_t> const hub {landing};
        while (flag.load(std::memory_order_acquire) == 0) {
            std::int32_t const watching = hub.load(std::memory_order_acquire);
            if (watching < 0) continue;
            walk_t walked;
            if (walk_vertex(**store, guard->channel(), key_of(watching + 1), walked).failed()) {
                ++reader_failures;
                return;
            }
            ++watched;
            // A registry naming a block the half-edges do not carry is half of a commit.
            if (walked.missing_blocks != 0) ++torn;
            if (walked.neighbors != 0 && walked.neighbors != spokes_k) ++torn;
        }
    }};

    expected<typename graph_transaction_t::graph_transaction_unique_ptr_t> changing = graph_transaction_t::begin(
        **store, 7);
    failures += expect(changing.has_value());
    if (changing)
        for (std::size_t hub = 0; hub != hubs_k; ++hub) {
            st::basic_vector<labeled_edge_t> edges;
            failures += expect(edges.resize(spokes_k) == st::status_t::success_k);
            for (std::size_t spoke = 0; spoke != spokes_k; ++spoke)
                edges[spoke] = plain_edge(key_of(hub * spokes_k + spoke + 1000), key_of(hub + 1),
                                          key_of(hubs_k + hub * spokes_k + spoke + 1));
            graph_change_t changed;
            failures += expect((*changing)
                                   ->access()
                                   .upsert_edges(channel, std::span<labeled_edge_t const> {edges}, changed)
                                   .succeeded());
            fu::preferred_atomic_ref<std::int32_t> {landing}.store(static_cast<std::int32_t>(hub),
                                                                   std::memory_order_release);
            failures += expect((*changing)->commit().succeeded());
        }

    fu::preferred_atomic_ref<std::int32_t> {stop}.store(1, std::memory_order_release);
    reader.join();
    failures += reader_failures;
    failures += expect(watched > std::size_t {0});
    failures += expect_eq(torn, std::size_t {0});

    for (std::size_t hub = 0; hub != hubs_k; ++hub) {
        std::size_t degree = 0;
        failures += expect((*store)->access().degree(channel, key_of(hub + 1), any_role_k, degree).succeeded());
        failures += expect_eq(degree, spokes_k);
    }
    return failures;
}

#pragma endregion Transaction Cases

#pragma region Concurrency Cases

/** Threads writing their own vertices and threads walking them leave every adjacency whole. */
template <typename mount_type_>
std::size_t threads_write_and_walk_together(mount_type_ const& mount) {
    using types_t = graph_types<typename mount_type_::fixture_t>;
    using guard_t = typename types_t::guard_t;
    using graph_store_t = typename types_t::graph_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t rounds_k = 12;
    constexpr std::size_t spokes_k = 8;
    constexpr std::size_t stride_k = 1000;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    expected<typename graph_store_t::graph_store_unique_ptr_t> store = open_over(**stack,
                                                                                 test_config(1024ull * 1024ull));
    failures += expect(store.has_value());
    if (!store) return failures;

    std::array<std::size_t, worker_threads_k> worker_failures {};
    std::array<std::thread, worker_threads_k> workers;
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker)
        workers[worker] = std::thread {[&, worker]() noexcept {
            expected<guard_t> guard = (*stack)->fixture->channels.acquire();
            worker_failures[worker] += expect(guard.has_value());
            if (!guard) return;

            // Each writer owns a range of its own, since two writers sharing a vertex need a transaction.
            std::uint64_t const base_number = worker * stride_k + 1;
            id_entity_t const base = key_of(base_number);
            for (std::size_t round = 0; round != rounds_k; ++round) {
                std::array<labeled_edge_t, spokes_k> edges {};
                for (std::size_t spoke = 0; spoke != spokes_k; ++spoke)
                    edges[spoke] = plain_edge(key_of(worker * stride_k + round * spokes_k + spoke + 1), base,
                                              key_of(base_number + round * spokes_k + spoke + 1));
                graph_change_t changed;
                worker_failures[worker] += expect(
                    (*store)
                        ->access()
                        .upsert_edges(guard->channel(), std::span<labeled_edge_t const> {edges}, changed)
                        .succeeded());
                worker_failures[worker] += expect_eq(changed.edges, spokes_k);

                std::size_t degree = 0;
                worker_failures[worker] += expect(
                    (*store)->access().degree(guard->channel(), base, any_role_k, degree).succeeded());
                worker_failures[worker] += expect_eq(degree, (round + 1) * spokes_k);
            }
        }};
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker) workers[worker].join();
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker) failures += worker_failures[worker];

    io_channel_t& channel = (*stack)->channel();
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker) {
        id_entity_t const base = key_of(worker * stride_k + 1);
        std::size_t degree = 0;
        failures += expect((*store)->access().degree(channel, base, any_role_k, degree).succeeded());
        failures += expect_eq(degree, rounds_k * spokes_k);
        st::basic_vector<id_entity_t> walked;
        failures += read_neighbors_in_order(**store, channel, base, walked);
        failures += expect_eq(walked.size(), rounds_k * spokes_k);
    }
    return failures;
}

#pragma endregion Concurrency Cases

} // namespace unum::ustore::test

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;

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
    test_environment_t const environment = test_environment_t::of(options, *directory);

    auto const cases = [&](auto const& mount) {
        report(totals, mount, "round_trip", vertices_and_edges_round_trip);
        report(totals, mount, "remove_vertex", removing_a_vertex_takes_its_edges);
        report(totals, mount, "blocks_split_and_merge", blocks_split_and_merge);
        report(totals, mount, "batched_pairs_agree", batched_pairs_agree_with_single_ones);
        report(totals, mount, "batched_read_depth", batched_read_fills_the_queue_pair);
        report(totals, mount, "survives_reopen", graph_survives_a_reopen);
        report(totals, mount, "conflicts_refused", conflicting_transactions_are_refused);
        report(totals, mount, "commit_all_or_nothing", commit_shows_both_halves_or_neither);
        report(totals, mount, "threads_write_and_walk", threads_write_and_walk_together);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, graph_config_k, 2, "graph", cases);

    return suite_exit_code(totals);
}
