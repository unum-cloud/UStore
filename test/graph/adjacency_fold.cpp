/**
 *  @file test/graph/adjacency_fold.cpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief Unit tests of `ustore/graph/adjacency_fold.hpp` against a reference model, a sorted map of half-edges: the
 *      record and block codecs, composition against sequential application, random insert and removal streams with
 *      rebalances at random points, block size invariants, and the reads that fetch blocks lazily.
 *
 *  The model shares nothing with the templates beyond the key and role types. The store under test keeps, per vertex,
 *  a registry, its blocks by first neighbor, and the chains of pending deltas, as the LSM will once the fold is wired
 *  in. The suite prints one line per case and a total, and exits non-zero on a failure.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::int32_t`
#include <cstring> // `std::memcmp`

#include <algorithm>       // `std::min`
#include <map>             // `std::map`
#include <memory>          // `std::unique_ptr`, `std::align`
#include <new>             // `std::nothrow`
#include <random>          // `std::mt19937_64`
#include <set>             // `std::set`
#include <source_location> // `std::source_location`
#include <span>            // `std::span`
#include <tuple>           // `std::tuple`
#include <vector>          // `std::vector`

#include <fmt/format.h> // `fmt::println`

#include "ustore/graph/adjacency_fold.hpp" // `adjacency_fold`, `rebalance`, `vertex_adjacency`

namespace unum::ustore::test {

using namespace unum::ustore::graph;

#pragma region Harness

/** Prints a failed check with its location and counts it. */
static std::size_t check(bool condition, std::source_location location = std::source_location::current()) {
    if (condition) return 0;
    fmt::println("    check failed at {}:{}", location.file_name(), location.line());
    return 1;
}

/** Prints both codes when they differ and counts the failure. */
static std::size_t check_code(status_code_t actual, status_code_t wanted,
                              std::source_location location = std::source_location::current()) {
    if (actual == wanted) return 0;
    fmt::println("    check failed at {}:{}: status {}, wanted {}", location.file_name(), location.line(), int(actual),
                 int(wanted));
    return 1;
}

/** A bump arena handing out blocks that live until it is destroyed, as engines' arenas do. */
struct test_arena_t {
    std::vector<std::unique_ptr<std::byte[]>> blocks;
};

template <typename value_type_>
struct test_allocator {
    using value_type = value_type_;
    using propagate_on_container_move_assignment = std::true_type;

    test_arena_t* arena = nullptr;

    test_allocator(test_arena_t* owner = nullptr) noexcept : arena(owner) {}
    template <typename other_type_>
    test_allocator(test_allocator<other_type_> const& other) noexcept : arena(other.arena) {}

    value_type_* allocate(std::size_t count) noexcept {
        std::size_t const bytes = count * sizeof(value_type_) + alignof(std::max_align_t);
        auto block = std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[bytes]);
        if (!block) return nullptr;
        void* pointer = block.get();
        std::size_t space = bytes;
        pointer = std::align(alignof(value_type_), count * sizeof(value_type_), pointer, space);
        arena->blocks.push_back(std::move(block));
        return static_cast<value_type_*>(pointer);
    }
    void deallocate(value_type_*, std::size_t) noexcept {}

    template <typename other_type_>
    bool operator==(test_allocator<other_type_> const& other) const noexcept {
        return arena == other.arena;
    }
};

using bytes_allocator_t = test_allocator<byte_t>;
using bytes_t = std::vector<byte_t>;

template <typename body_type_>
void run(char const* name, body_type_&& body, std::size_t& failed) {
    std::size_t const failures = body();
    fmt::println("{:<40} {}", name, failures ? "FAILED" : "passed");
    failed += failures != 0;
}

static bytes_t to_bytes(std::span<byte_t const> span) { return bytes_t(span.begin(), span.end()); }

#pragma endregion Harness

#pragma region Reference Model

/** What names a half-edge in the model: its neighbor, edge and role. */
struct model_key_t {
    ustore_key_t neighbor {};
    ustore_key_t edge {};
    std::uint8_t role = 0;

    friend bool operator<(model_key_t const& first, model_key_t const& second) noexcept {
        if (int order = std::memcmp(&first.neighbor, &second.neighbor, 16); order) return order < 0;
        if (int order = std::memcmp(&first.edge, &second.edge, 16); order) return order < 0;
        return first.role < second.role;
    }
};

/** What a half-edge carries besides its name. */
struct model_payload_t {
    float weight = 0;
    std::int32_t label = 0;
};

using model_adjacency_t = std::map<model_key_t, model_payload_t>;

/** Applies one record to @p adjacency, the way the fold promises to. */
static void model_apply(model_adjacency_t& adjacency, half_edge_delta_t const& delta) {
    switch (delta.operation) {
    case half_edge_operation_t::insert_k:
        adjacency[model_key_t {delta.neighbor, delta.edge, delta.role.raw}] = {delta.weight, delta.label.raw};
        break;
    case half_edge_operation_t::remove_k:
        adjacency.erase(model_key_t {delta.neighbor, delta.edge, delta.role.raw});
        break;
    case half_edge_operation_t::remove_vertex_k: adjacency.clear(); break;
    }
}

/** The half-edges a block holds, as the model keeps them. */
static model_adjacency_t model_of_block(std::span<byte_t const> bytes) {
    model_adjacency_t adjacency;
    auto view = edge_block_view::make(bytes);
    if (!view) return adjacency;
    for (std::size_t index = 0; index != view->size(); ++index) {
        labeled_half_edge_t const half_edge = (*view)[index];
        adjacency[model_key_t {half_edge.neighbor_id, half_edge.edge_id, half_edge.role.raw}] = {
            half_edge.edge_weight, half_edge.edge_label.raw};
    }
    return adjacency;
}

static bool same_adjacency(model_adjacency_t const& first, model_adjacency_t const& second) {
    if (first.size() != second.size()) return false;
    for (auto a = first.begin(), b = second.begin(); a != first.end(); ++a, ++b)
        if (a->first < b->first || b->first < a->first || a->second.weight != b->second.weight ||
            a->second.label != b->second.label)
            return false;
    return true;
}

/** The serialized block of @p adjacency, through the header's own writer. */
static bytes_t block_of_model(model_adjacency_t const& adjacency) {
    if (adjacency.empty()) return {};
    bytes_t bytes(encoded_block_bytes(adjacency.size()));
    edge_block_writer writer(bytes.data(), adjacency.size());
    std::size_t index = 0;
    for (auto const& [key, payload] : adjacency) {
        labeled_half_edge_t half_edge;
        half_edge.neighbor_id = key.neighbor, half_edge.edge_id = key.edge, half_edge.role.raw = key.role;
        half_edge.edge_weight = payload.weight, half_edge.edge_label.raw = payload.label;
        writer.set(index++, half_edge);
    }
    return bytes;
}

/** Random keys from small pools, sharing their first or last eight bytes, so 128-bit orders get exercised. */
struct key_pools_t {
    std::vector<ustore_key_t> vertices, neighbors, edges;

    explicit key_pools_t(std::mt19937_64& generator, std::size_t vertex_count = 6, std::size_t neighbor_count = 220) {
        auto random_key = [&](bool share) {
            ustore_key_t key {};
            for (std::size_t index = 0; index != 16; ++index) key.uuid.bytes[index] = std::uint8_t(generator());
            if (share)
                for (std::size_t index = 0; index != 8; ++index) key.uuid.bytes[index] = std::uint8_t(index);
            return key;
        };
        for (std::size_t index = 0; index != vertex_count; ++index) vertices.push_back(random_key(false));
        // Vertices are neighbors too, so loops and shared neighbors show up
        for (ustore_key_t const& vertex : vertices) neighbors.push_back(vertex);
        while (neighbors.size() != neighbor_count) neighbors.push_back(random_key(generator() % 2));
        for (std::size_t index = 0; index != 4; ++index) edges.push_back(random_key(index % 2));
    }
};

static role_t random_role(std::mt19937_64& generator) {
    role_t const roles[3] = {edge_member_k, dedge_source_k, dedge_target_k};
    return roles[generator() % 3];
}

/** A random record for @p vertex, removals aimed at what @p model holds three times in four. */
static half_edge_delta_t random_delta(std::mt19937_64& generator, key_pools_t const& pools, ustore_key_t vertex,
                                      model_adjacency_t const& model, std::size_t removal_percent,
                                      std::size_t vertex_removal_permille = 0) {
    if (generator() % 1000 < vertex_removal_permille) return half_edge_delta_t::remove_vertex(vertex);
    if (generator() % 100 < removal_percent) {
        if (!model.empty() && generator() % 4) {
            auto it = model.begin();
            std::advance(it, generator() % model.size());
            role_t role;
            role.raw = it->first.role;
            return half_edge_delta_t::remove(vertex, it->first.neighbor, it->first.edge, role);
        }
        return half_edge_delta_t::remove(vertex, pools.neighbors[generator() % pools.neighbors.size()],
                                         pools.edges[generator() % pools.edges.size()], random_role(generator));
    }
    labeled_half_edge_t half_edge;
    half_edge.neighbor_id = pools.neighbors[generator() % pools.neighbors.size()];
    half_edge.edge_id = pools.edges[generator() % pools.edges.size()];
    half_edge.role = random_role(generator);
    half_edge.edge_weight = float(generator() % 1000) / 8;
    half_edge.edge_label.raw = std::int32_t(generator() % 5) - 1;
    return half_edge_delta_t::insert(vertex, half_edge);
}

static bytes_t encode_chain(std::vector<half_edge_delta_t> const& deltas) {
    bytes_t bytes(deltas.size() * half_edge_delta_bytes_k);
    for (std::size_t index = 0; index != deltas.size(); ++index)
        save_delta(deltas[index], bytes.data() + index * half_edge_delta_bytes_k);
    return bytes;
}

static std::vector<delta_view_t> views_of(std::vector<bytes_t> const& chains) {
    std::vector<delta_view_t> views;
    for (bytes_t const& chain : chains) views.push_back(delta_view_t {delta_kind_t::change_k, chain});
    return views;
}

#pragma endregion Reference Model

#pragma region Store Under Test

/** One vertex of the store: its registry, blocks by key, and the chains of pending deltas, oldest first. */
struct stored_vertex_t {
    vertex_registry_t registry;
    std::map<id_chunk_t, bytes_t> blocks;
    std::vector<bytes_t> pending;
    std::size_t fetches = 0;

    explicit stored_vertex_t(ustore_key_t vertex) { registry.vertex_info.id = vertex; }

    /** Answers a block by key, counting the reads. */
    auto fetcher() {
        return [this](id_chunk_t const& key) noexcept -> expected<std::span<byte_t const>> {
            ++fetches;
            auto it = blocks.find(key);
            if (it == blocks.end()) return status_t {status_code_t::not_found_k, "No such block"};
            return std::span<byte_t const>(it->second);
        };
    }
};

using fetcher_t = decltype(std::declval<stored_vertex_t&>().fetcher());

/** Composes the pending chains of @p vertex for a read, into @p arena. */
static expected<std::span<byte_t const>> pending_chain(stored_vertex_t const& vertex, test_arena_t& arena) {
    if (vertex.pending.empty()) return std::span<byte_t const> {};
    bytes_allocator_t allocator(&arena);
    std::vector<delta_view_t> const views = views_of(vertex.pending);
    return adjacency_fold {}.compose(views, allocator);
}

/** Folds the pending chains of @p vertex into its blocks, as compaction will. */
static std::size_t rebalance_vertex(stored_vertex_t& vertex) {
    std::size_t failures = 0;
    test_arena_t arena;
    bytes_allocator_t allocator(&arena);
    std::vector<delta_view_t> const views = views_of(vertex.pending);
    auto result = rebalance(vertex.registry, vertex.fetcher(), views, allocator);
    failures += check_code(result.code(), status_code_t::success_k);
    if (!result) return failures;
    for (id_chunk_t const& key : result->removed) failures += check(vertex.blocks.erase(key) == 1);
    for (written_block_t const& block : result->written) vertex.blocks[block.key] = to_bytes(block.bytes);
    vertex.pending.clear();
    return failures;
}

/** Every block is sorted, keyed by its first neighbor, listed exactly as the registry says, and within 16..512
 *  unless it is the only block of its vertex, with no neighbor split across blocks. */
static std::size_t check_blocks(stored_vertex_t const& vertex) {
    std::size_t failures = 0;
    vertex_registry_t const& registry = vertex.registry;
    failures += check(registry.size() == vertex.blocks.size());
    std::size_t index = 0;
    ustore_key_t previous_last {};
    for (auto const& [key, bytes] : vertex.blocks) {
        if (index >= registry.size()) break;
        failures += check(key.entity == registry.vertex_info.id);
        failures += check(key.chunk == registry.first_neighbors[index]);
        auto view = edge_block_view::make(bytes);
        failures += check(view.has_value());
        if (!view) return failures;
        std::size_t const count = view->size();
        failures += check(count != 0 && view->sorted());
        if (!count) return failures;
        failures += check(view->neighbor(0) == key.chunk);
        failures += check(view->neighbor(count - 1) == registry.last_neighbors[index]);
        if (index) failures += check(previous_last < key.chunk);
        previous_last = view->neighbor(count - 1);
        edge_block_size_t const wanted = count < edge_block_t::min_count_k   ? edge_block_size_t::too_small_k
                                         : count > edge_block_t::max_count_k ? edge_block_size_t::too_big_k
                                                                             : edge_block_size_t::normal_k;
        failures += check(registry.sizes[index] == wanted);
        if (registry.size() > 1) failures += check(count >= edge_block_t::min_count_k);
        failures += check(count <= edge_block_t::max_count_k);

        // The block is byte-compatible with `edge_block_t`
        edge_block_t decoded;
        failures += check(decoded.load(bytes).succeeded() && decoded.size() == count);
        ++index;
    }
    return failures;
}

/** Every read of @p vertex matches @p model: the walk, degrees by role, edges to neighbors, and shared neighbors
 *  with @p other. */
static std::size_t check_reads(stored_vertex_t& vertex, model_adjacency_t const& model, stored_vertex_t& other,
                               model_adjacency_t const& other_model, key_pools_t const& pools,
                               std::mt19937_64& generator) {
    std::size_t failures = 0;
    test_arena_t arena;
    auto pending = pending_chain(vertex, arena);
    failures += check_code(pending.code(), status_code_t::success_k);
    if (!pending) return failures;
    auto adjacency = vertex_adjacency<fetcher_t>::make(vertex.registry, vertex.fetcher(), *pending);
    failures += check_code(adjacency.code(), status_code_t::success_k);
    if (!adjacency) return failures;

    model_adjacency_t walked;
    std::size_t order_failures = 0;
    model_key_t last {};
    bool first = true;
    status_t const walk = adjacency->for_each([&](labeled_half_edge_t const& half_edge) noexcept {
        model_key_t const key {half_edge.neighbor_id, half_edge.edge_id, half_edge.role.raw};
        if (!first && !(last < key)) ++order_failures;
        first = false, last = key;
        walked[key] = {half_edge.edge_weight, half_edge.edge_label.raw};
        return status_t {};
    });
    failures += check(walk.succeeded() && !order_failures);
    failures += check(same_adjacency(walked, model));

    for (role_t const roles : {any_role_k, edge_member_k, dedge_source_k, dedge_source_k | dedge_target_k}) {
        std::size_t wanted = 0;
        for (auto const& [key, payload] : model) wanted += (role_t {key.role} & roles) ? 1 : 0;
        auto degree = adjacency->degree(roles);
        failures += check(degree.has_value() && *degree == wanted);
    }

    for (std::size_t probe = 0; probe != 6; ++probe) {
        ustore_key_t neighbor = pools.neighbors[generator() % pools.neighbors.size()];
        if (probe % 2 && !model.empty()) {
            auto it = model.begin();
            std::advance(it, generator() % model.size());
            neighbor = it->first.neighbor;
        }
        model_adjacency_t wanted, found;
        for (auto const& [key, payload] : model)
            if (key.neighbor == neighbor) wanted[key] = payload;
        status_t const to = adjacency->edges_to(neighbor, [&](labeled_half_edge_t const& half_edge) noexcept {
            found[model_key_t {half_edge.neighbor_id, half_edge.edge_id, half_edge.role.raw}] = {
                half_edge.edge_weight, half_edge.edge_label.raw};
            return status_t {};
        });
        failures += check(to.succeeded() && same_adjacency(found, wanted));

        // Edges lose which end of a directed loop was read, so they compare by identifier, payload and direction
        std::multiset<std::tuple<std::uint64_t, std::uint64_t, float, std::int32_t, bool>> wanted_edges, found_edges;
        auto edge_row = [](ustore_key_t const& id, float weight, std::int32_t label, bool directed) {
            std::uint64_t halves[2];
            std::memcpy(halves, &id, 16);
            return std::tuple {halves[0], halves[1], weight, label, directed};
        };
        for (auto const& [key, payload] : wanted)
            wanted_edges.insert(edge_row(key.edge, payload.weight, payload.label, key.role != edge_member_k.raw));
        std::size_t endpoint_failures = 0;
        status_t const between = edges_between(*adjacency, neighbor, [&](labeled_edge_t const& edge) noexcept {
            if (!edge.contains(vertex.registry.vertex_info.id) || !edge.contains(neighbor)) ++endpoint_failures;
            found_edges.insert(
                edge_row(edge.id, edge.weight, edge.label.raw, edge.direction == direction_t::directed_k));
            return status_t {};
        });
        failures += check(between.succeeded() && !endpoint_failures && found_edges == wanted_edges);
    }

    test_arena_t other_arena;
    auto other_pending = pending_chain(other, other_arena);
    failures += check(other_pending.has_value());
    if (!other_pending) return failures;
    auto other_adjacency = vertex_adjacency<fetcher_t>::make(other.registry, other.fetcher(), *other_pending);
    failures += check(other_adjacency.has_value());
    if (!other_adjacency) return failures;
    std::set<model_key_t> first_neighbors, second_neighbors;
    for (auto const& [key, payload] : model) first_neighbors.insert(model_key_t {key.neighbor, {}, 0});
    for (auto const& [key, payload] : other_model) second_neighbors.insert(model_key_t {key.neighbor, {}, 0});
    std::vector<ustore_key_t> wanted;
    for (model_key_t const& key : first_neighbors)
        if (second_neighbors.count(key)) wanted.push_back(key.neighbor);
    std::vector<ustore_key_t> shared;
    status_t const common = common_neighbors(*adjacency, *other_adjacency, [&](ustore_key_t const& neighbor) noexcept {
        shared.push_back(neighbor);
        return status_t {};
    });
    failures += check(common.succeeded() && shared.size() == wanted.size());
    for (std::size_t index = 0; index != std::min(shared.size(), wanted.size()); ++index)
        failures += check(shared[index] == wanted[index]);
    return failures;
}

#pragma endregion Store Under Test

#pragma region Cases

/** Records and blocks round-trip, malformed ones are refused, and edges file the half-edges their roles say. */
std::size_t codecs() {
    std::size_t failures = 0;
    std::mt19937_64 generator(3);
    key_pools_t const pools(generator);
    for (std::size_t round = 0; round != 200; ++round) {
        half_edge_delta_t const delta = random_delta(generator, pools, pools.vertices[0], {}, 30, 50);
        byte_t record[half_edge_delta_bytes_k];
        save_delta(delta, record);
        failures += check(check_delta(record).succeeded());
        half_edge_delta_t const loaded = load_delta(record);
        failures += check(loaded.vertex == delta.vertex && loaded.neighbor == delta.neighbor &&
                          loaded.edge == delta.edge && loaded.weight == delta.weight && loaded.label == delta.label &&
                          loaded.role == delta.role && loaded.operation == delta.operation);
        id_chunk_t const key = delta.key();
        failures += check(key.entity == delta.vertex);
        failures += check(
            key.chunk == (delta.operation == half_edge_operation_t::remove_vertex_k ? smallest_key_k : delta.neighbor));
    }

    byte_t record[half_edge_delta_bytes_k];
    save_delta(half_edge_delta_t::remove(pools.vertices[0], pools.neighbors[9], pools.edges[0], edge_member_k), record);
    record[delta_operation_offset_k] = byte_t {3};
    failures += check_code(check_delta(record).code, status_code_t::malformed_k);
    record[delta_operation_offset_k] = byte_t {1}, record[60] = byte_t {1};
    failures += check_code(check_delta(record).code, status_code_t::malformed_k);
    save_delta(half_edge_delta_t::remove(pools.vertices[0], smallest_key_k, pools.edges[0], edge_member_k), record);
    failures += check_code(check_delta(record).code, status_code_t::malformed_k);
    save_delta(half_edge_delta_t::remove_vertex(pools.vertices[0]), record);
    failures += check(check_delta(record).succeeded());
    failures += check_code(delta_chain_view::make(std::span<byte_t const>(record, 63)).code(),
                           status_code_t::malformed_k);

    // Blocks the header writes load into `edge_block_t` and save back to the same bytes
    for (std::size_t round = 0; round != 50; ++round) {
        model_adjacency_t model;
        std::size_t const wanted = generator() % 600;
        while (model.size() < wanted) model_apply(model, random_delta(generator, pools, pools.vertices[1], model, 0));
        bytes_t const bytes = block_of_model(model);
        if (model.empty()) continue;
        edge_block_t block;
        failures += check(block.load(bytes).succeeded() && block.size() == model.size());
        bytes_t saved(block.storage_requirements());
        failures += check(saved.size() == bytes.size() && block.save(saved).succeeded());
        failures += check(saved == bytes);
        failures += check(same_adjacency(model_of_block(bytes), model));
    }
    model_adjacency_t pair;
    model_apply(pair, random_delta(generator, pools, pools.vertices[1], pair, 0));
    model_apply(pair, random_delta(generator, pools, pools.vertices[1], pair, 0));
    bytes_t block = block_of_model(pair);
    failures += check_code(edge_block_view::make(std::span<byte_t const>(block).first(block.size() - 1)).code(),
                           status_code_t::malformed_k);
    block[2] = byte_t {7};
    failures += check_code(edge_block_view::make(block).code(), status_code_t::malformed_k);

    // A block out of order is refused by the fold
    if (pair.size() == 2) {
        bytes_t swapped = block_of_model(pair);
        auto view = *edge_block_view::make(swapped);
        labeled_half_edge_t const first = view[0], second = view[1];
        edge_block_writer writer(swapped.data(), 2);
        writer.set(0, second), writer.set(1, first);
        test_arena_t arena;
        bytes_allocator_t allocator(&arena);
        failures += check_code(adjacency_fold {}.apply(swapped, {}, allocator).code(), status_code_t::malformed_k);
    }

    // Edges file one half-edge per end, a directed loop two, and an undirected loop one
    labeled_edge_t edge;
    edge.id = pools.edges[1], edge.first = pools.vertices[0], edge.second = pools.vertices[1];
    half_edge_delta_t deltas[2];
    failures += check(deltas_of_edge(edge, half_edge_operation_t::insert_k, deltas) == 2);
    failures += check(deltas[0].vertex == edge.first && deltas[0].neighbor == edge.second &&
                      deltas[0].role == dedge_source_k);
    failures += check(deltas[1].vertex == edge.second && deltas[1].neighbor == edge.first &&
                      deltas[1].role == dedge_target_k);
    edge.second = edge.first;
    failures += check(deltas_of_edge(edge, half_edge_operation_t::remove_k, deltas) == 2);
    failures += check(deltas[0].role == dedge_source_k && deltas[1].role == dedge_target_k &&
                      deltas[1].operation == half_edge_operation_t::remove_k);
    edge.direction = direction_t::undirected_k;
    failures += check(deltas_of_edge(edge, half_edge_operation_t::insert_k, deltas) == 1);
    failures += check(deltas[0].role == edge_member_k);
    return failures;
}

/** Applying a chain matches the model, and so does applying its composition, whole or in parts, which is canonical. */
std::size_t compose_matches_sequential() {
    std::size_t failures = 0;
    std::mt19937_64 generator(5);
    // Enough half-edges for bases past one block, few enough that chains hit what the base holds
    key_pools_t const pools(generator, 3, 90);
    adjacency_fold const fold;
    for (std::size_t round = 0; round != 1500; ++round) {
        ustore_key_t const vertex = pools.vertices[0];
        model_adjacency_t base_model;
        std::size_t const base_size = round % 5 ? generator() % 80 : generator() % 700;
        while (base_model.size() < base_size)
            model_apply(base_model, random_delta(generator, pools, vertex, base_model, 0));
        bytes_t const base = block_of_model(base_model);

        model_adjacency_t model = base_model;
        std::vector<bytes_t> chains;
        std::size_t const chain_count = 1 + generator() % 6;
        for (std::size_t chain = 0; chain != chain_count; ++chain) {
            std::vector<half_edge_delta_t> records;
            std::size_t const record_count = generator() % 20;
            for (std::size_t record = 0; record != record_count; ++record) {
                half_edge_delta_t const delta = random_delta(generator, pools, vertex, model, 45, 30);
                model_apply(model, delta);
                records.push_back(delta);
            }
            chains.push_back(encode_chain(records));
        }
        std::vector<delta_view_t> const views = views_of(chains);
        test_arena_t arena;
        bytes_allocator_t allocator(&arena);

        auto applied = fold.apply(base, views, allocator);
        if (model.empty()) failures += check_code(applied.code(), status_code_t::not_found_k);
        else failures += check(applied.has_value() && same_adjacency(model_of_block(*applied), model));

        auto composed = fold.compose(views, allocator);
        failures += check_code(composed.code(), status_code_t::success_k);
        if (!composed) continue;
        bytes_t const whole = to_bytes(*composed);
        delta_view_t const composed_view {delta_kind_t::change_k, whole};
        auto from_composed = fold.apply(base, std::span<delta_view_t const>(&composed_view, 1), allocator);
        if (model.empty()) failures += check_code(from_composed.code(), status_code_t::not_found_k);
        else failures += check(from_composed.has_value() && same_adjacency(model_of_block(*from_composed), model));

        // Each record of a composed chain names a different half-edge, and a removal of the vertex leads it
        auto chain = delta_chain_view::make(whole);
        failures += check(chain.has_value());
        bool closes = false;
        for (std::size_t chain_index = 0; chain_index != chains.size(); ++chain_index)
            closes = closes || fold.closes(views[chain_index]);
        failures += check(fold.closes(composed_view) == closes);
        for (std::size_t index = 1; chain && index < chain->size(); ++index)
            failures += check((*chain)[index].operation != half_edge_operation_t::remove_vertex_k);
        if (closes)
            for (std::size_t index = 1; chain && index < chain->size(); ++index)
                failures += check((*chain)[index].operation == half_edge_operation_t::insert_k);

        // Composing in two parts and then together yields the same chain, byte for byte
        std::size_t const split = generator() % views.size();
        if (split) {
            auto head = fold.compose(std::span<delta_view_t const>(views).first(split), allocator);
            auto tail = fold.compose(std::span<delta_view_t const>(views).subspan(split), allocator);
            failures += check(head.has_value() && tail.has_value());
            if (head && tail) {
                delta_view_t const parts[2] = {{delta_kind_t::change_k, *head}, {delta_kind_t::change_k, *tail}};
                auto rejoined = fold.compose(parts, allocator);
                failures += check(rejoined.has_value() && to_bytes(*rejoined) == whole);
                auto from_parts = fold.apply(base, parts, allocator);
                if (model.empty()) failures += check_code(from_parts.code(), status_code_t::not_found_k);
                else failures += check(from_parts.has_value() && same_adjacency(model_of_block(*from_parts), model));
            }
        }

        // A block write in the middle restarts from that block, which compose leaves to apply
        if (round % 7 == 0) {
            model_adjacency_t restart;
            std::size_t const restart_size = 1 + generator() % 30;
            while (restart.size() < restart_size)
                model_apply(restart, random_delta(generator, pools, vertex, restart, 0));
            bytes_t const restart_block = block_of_model(restart);
            std::vector<delta_view_t> mixed = views;
            std::size_t const at = generator() % (mixed.size() + 1);
            mixed.insert(mixed.begin() + std::ptrdiff_t(at), delta_view_t {delta_kind_t::write_k, restart_block});
            for (std::size_t index = at + 1; index != mixed.size(); ++index) {
                auto records = *delta_chain_view::make(mixed[index].bytes);
                for (std::size_t record = 0; record != records.size(); ++record) model_apply(restart, records[record]);
            }
            auto restarted = fold.apply(base, mixed, allocator);
            if (restart.empty()) failures += check_code(restarted.code(), status_code_t::not_found_k);
            else failures += check(restarted.has_value() && same_adjacency(model_of_block(*restarted), restart));
            failures += check_code(fold.compose(mixed, allocator).code(), status_code_t::invalid_argument_k);
            failures += check(fold.closes(mixed[at]));
        }
    }
    return failures;
}

/** An insert and a removal of one half-edge cancel after a removal of the vertex, and collapse to the removal
 *  otherwise; records of two vertices are refused by a block. */
std::size_t compose_shapes() {
    std::size_t failures = 0;
    std::mt19937_64 generator(9);
    key_pools_t const pools(generator, 2, 20);
    adjacency_fold const fold;
    test_arena_t arena;
    bytes_allocator_t allocator(&arena);
    ustore_key_t const vertex = pools.vertices[0];
    labeled_half_edge_t half_edge;
    half_edge.neighbor_id = pools.neighbors[5], half_edge.edge_id = pools.edges[2], half_edge.role = edge_member_k;
    half_edge_delta_t const insert = half_edge_delta_t::insert(vertex, half_edge);
    half_edge_delta_t const remove = half_edge_delta_t::remove(vertex, half_edge.neighbor_id, half_edge.edge_id,
                                                               half_edge.role);

    bytes_t const open_chain = encode_chain({insert, remove});
    delta_view_t const open_view {delta_kind_t::change_k, open_chain};
    auto open = fold.compose(std::span<delta_view_t const>(&open_view, 1), allocator);
    failures += check(open.has_value() && open->size() == half_edge_delta_bytes_k);
    if (open && open->size() == half_edge_delta_bytes_k)
        failures += check(load_delta(open->data()).operation == half_edge_operation_t::remove_k);
    failures += check(!fold.closes(open_view));

    bytes_t const closed_chain = encode_chain({insert, half_edge_delta_t::remove_vertex(vertex), insert, remove});
    delta_view_t const closed_view {delta_kind_t::change_k, closed_chain};
    auto closed = fold.compose(std::span<delta_view_t const>(&closed_view, 1), allocator);
    failures += check(closed.has_value() && closed->size() == half_edge_delta_bytes_k);
    if (closed && closed->size() == half_edge_delta_bytes_k)
        failures += check(load_delta(closed->data()).operation == half_edge_operation_t::remove_vertex_k);
    failures += check(fold.closes(closed_view));

    bytes_t const empty_chain;
    delta_view_t const empty_view {delta_kind_t::change_k, empty_chain};
    auto empty = fold.compose(std::span<delta_view_t const>(&empty_view, 1), allocator);
    failures += check(empty.has_value() && empty->empty());
    failures += check_code(fold.compose({}, allocator).code(), status_code_t::invalid_argument_k);

    half_edge_delta_t stranger = insert;
    stranger.vertex = pools.vertices[1];
    bytes_t const mixed_chain = encode_chain({insert, stranger});
    delta_view_t const mixed_view {delta_kind_t::change_k, mixed_chain};
    failures += check_code(fold.apply({}, std::span<delta_view_t const>(&mixed_view, 1), allocator).code(),
                           status_code_t::invalid_argument_k);
    // Composing many vertices is fine, sorted by vertex
    auto both = fold.compose(std::span<delta_view_t const>(&mixed_view, 1), allocator);
    failures += check(both.has_value() && both->size() == 2 * half_edge_delta_bytes_k);
    return failures;
}

/** Random streams of inserts and removals over a few vertices, read after every step and folded at random points. */
std::size_t random_streams() {
    std::size_t failures = 0;
    for (std::uint64_t seed = 0; seed != 6 && !failures; ++seed) {
        std::mt19937_64 generator(100 + seed);
        key_pools_t const pools(generator, 5, 700);
        std::vector<stored_vertex_t> store;
        std::vector<model_adjacency_t> models(pools.vertices.size());
        for (ustore_key_t const& vertex : pools.vertices) store.emplace_back(vertex);
        std::size_t const steps = 3000;
        std::size_t rebalances = 0, splits = 0, merges = 0, most_blocks = 0, vertex_removals = 0;
        for (std::size_t step = 0; step != steps && !failures; ++step) {
            // Phases grow the hub past many blocks, shrink it so blocks merge back, then mix both
            std::size_t const phase = step / 1000;
            std::size_t const removal_percent = phase == 0 ? 10 : phase == 1 ? 80 : 40;
            std::size_t const target = generator() % 2 ? 0 : generator() % store.size();
            stored_vertex_t& vertex = store[target];
            model_adjacency_t& model = models[target];

            std::vector<half_edge_delta_t> records;
            std::size_t const record_count = generator() % 10 == 0 ? 20 + generator() % 120 : 1 + generator() % 4;
            // The hub loses everything once, late, so a removal of a vertex closes a run of many blocks
            std::size_t const vertex_removal_permille = target ? 20 : step == 2600 ? 1000 : 0;
            for (std::size_t record = 0; record != record_count; ++record) {
                half_edge_delta_t const delta = random_delta(generator, pools, vertex.registry.vertex_info.id, model,
                                                             removal_percent, record ? 0 : vertex_removal_permille);
                vertex_removals += delta.operation == half_edge_operation_t::remove_vertex_k;
                model_apply(model, delta);
                records.push_back(delta);
            }
            vertex.pending.push_back(encode_chain(records));

            if (generator() % 20 == 0 && vertex.pending.size() > 1) {
                test_arena_t arena;
                auto composed = pending_chain(vertex, arena);
                failures += check(composed.has_value());
                if (composed) vertex.pending = {to_bytes(*composed)};
            }
            if (generator() % 12 == 0) {
                std::size_t const blocks_before = vertex.blocks.size();
                failures += rebalance_vertex(vertex);
                failures += check_blocks(vertex);
                ++rebalances;
                splits += vertex.blocks.size() > blocks_before;
                merges += vertex.blocks.size() < blocks_before;
                most_blocks = std::max(most_blocks, vertex.blocks.size());
            }
            std::size_t const other = generator() % store.size();
            failures += check_reads(vertex, model, store[other], models[other], pools, generator);
        }
        for (std::size_t index = 0; index != store.size(); ++index) {
            failures += rebalance_vertex(store[index]);
            failures += check_blocks(store[index]);
            failures += check_reads(store[index], models[index], store[(index + 1) % store.size()],
                                    models[(index + 1) % store.size()], pools, generator);
        }
        failures += check(rebalances > 100 && splits > 5 && merges > 5 && most_blocks >= 8 && vertex_removals > 2);
        if (failures)
            fmt::println("    seed {}: {} folds, {} grew, {} shrank, {} blocks at most, {} vertex removals", seed,
                         rebalances, splits, merges, most_blocks, vertex_removals);
    }
    return failures;
}

/** Reads and rebalances fetch only the blocks they need: one for a pair, the touched ones for a fold. */
std::size_t lazy_fetches() {
    std::size_t failures = 0;
    std::mt19937_64 generator(21);
    key_pools_t const pools(generator, 2, 2000);
    stored_vertex_t hub(pools.vertices[0]), leaf(pools.vertices[1]);
    model_adjacency_t hub_model, leaf_model;
    std::vector<half_edge_delta_t> records;
    for (std::size_t index = 2; index != pools.neighbors.size(); ++index) {
        labeled_half_edge_t half_edge;
        half_edge.neighbor_id = pools.neighbors[index], half_edge.edge_id = pools.edges[0];
        half_edge.role = edge_member_k;
        records.push_back(half_edge_delta_t::insert(hub.registry.vertex_info.id, half_edge));
        model_apply(hub_model, records.back());
    }
    hub.pending.push_back(encode_chain(records));
    failures += rebalance_vertex(hub);
    failures += check_blocks(hub);
    std::size_t const blocks = hub.blocks.size();
    failures += check(blocks >= 10);

    // One insert into the middle reads its block alone, or one more if it must merge
    auto middle = hub.blocks.begin();
    std::advance(middle, blocks / 2);
    labeled_half_edge_t half_edge;
    half_edge.neighbor_id = middle->first.chunk, half_edge.edge_id = pools.edges[1], half_edge.role = edge_member_k;
    hub.pending.push_back(encode_chain({half_edge_delta_t::insert(hub.registry.vertex_info.id, half_edge)}));
    model_apply(hub_model, half_edge_delta_t::insert(hub.registry.vertex_info.id, half_edge));
    hub.fetches = 0;
    failures += rebalance_vertex(hub);
    failures += check(hub.fetches == 1);
    failures += check(hub.blocks.size() == blocks);
    failures += check_blocks(hub);

    // A pair reads one block, a degree without deltas reads every block's length
    test_arena_t arena;
    auto adjacency = vertex_adjacency<fetcher_t>::make(hub.registry, hub.fetcher(), {});
    failures += check(adjacency.has_value());
    if (!adjacency) return failures;
    hub.fetches = 0;
    std::size_t found = 0;
    status_t const status = adjacency->edges_to(half_edge.neighbor_id, [&](labeled_half_edge_t const&) noexcept {
        ++found;
        return status_t {};
    });
    failures += check(status.succeeded() && found == 2 && hub.fetches == 1);
    hub.fetches = 0;
    auto degree = adjacency->degree();
    failures += check(degree.has_value() && *degree == hub_model.size() && hub.fetches == blocks);

    // Shared neighbors with a vertex of three neighbors leap over most of the hub's blocks
    std::vector<half_edge_delta_t> leaf_records;
    for (std::size_t index : {100u, 1000u, 1900u}) {
        labeled_half_edge_t leaf_edge;
        leaf_edge.neighbor_id = pools.neighbors[index], leaf_edge.edge_id = pools.edges[0];
        leaf_edge.role = edge_member_k;
        leaf_records.push_back(half_edge_delta_t::insert(leaf.registry.vertex_info.id, leaf_edge));
        model_apply(leaf_model, leaf_records.back());
    }
    leaf.pending.push_back(encode_chain(leaf_records));
    failures += rebalance_vertex(leaf);
    auto leaf_adjacency = vertex_adjacency<fetcher_t>::make(leaf.registry, leaf.fetcher(), {});
    failures += check(leaf_adjacency.has_value());
    if (!leaf_adjacency) return failures;
    hub.fetches = 0;
    std::size_t shared = 0;
    status_t const common = common_neighbors(*adjacency, *leaf_adjacency, [&](ustore_key_t const&) noexcept {
        ++shared;
        return status_t {};
    });
    failures += check(common.succeeded() && shared == 3 && hub.fetches <= 5);
    return failures;
}

#pragma endregion Cases

} // namespace unum::ustore::test

int main() {
    using namespace unum::ustore::test;
    std::size_t failed = 0, total = 0;
    auto counted = [&](char const* name, auto body) { run(name, body, failed), ++total; };
    counted("adjacency.codecs", codecs);
    counted("adjacency.compose_matches_sequential", compose_matches_sequential);
    counted("adjacency.compose_shapes", compose_shapes);
    counted("adjacency.random_streams", random_streams);
    counted("adjacency.lazy_fetches", lazy_fetches);
    fmt::println("total {} passed, {} failed", total - failed, failed);
    return failed ? 1 : 0;
}
