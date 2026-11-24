/**
 *  @file test/graph/adjacency_fold.cpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief Unit tests of `ustore/graph/adjacency_fold.hpp` against a reference model, a sorted map of half-edges: the
 *      record codec, application and composition against the model, associativity, closing records, random insert
 *      and removal streams folded at random points, and one vertex of 200,000 half-edges.
 *
 *  The model shares nothing with the fold beyond the key and role types. A vertex value the fold
 *  writes must equal, byte for byte, the inserts of the model's half-edges in order, which pins the
 *  layout as well as the contents. The store under test keeps, per vertex, a value and the chains
 *  of pending changes, as the LSM does once the fold is wired in.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`, `std::int32_t`
#include <cstring> // `std::memcmp`, `std::memcpy`

#include <map>    // `std::map`
#include <memory> // `std::unique_ptr`, `std::align`
#include <new>    // `std::nothrow`
#include <set>    // `std::set`, `std::multiset`
#include <span>   // `std::span`
#include <tuple>  // `std::tuple`
#include <vector> // `std::vector`

#include "ustore/graph/adjacency_fold.hpp" // `adjacency_fold_t`, `vertex_adjacency_t`, `half_edge_delta_t`

#include "harness.hpp" // `expect_true`, `expect_status`, `run_test_case`, `split_mix_generator_t`

namespace unum::ustore::test {

using namespace unum::ustore::graph;

#pragma region Arenas

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

static bytes_t to_bytes(std::span<byte_t const> span) { return bytes_t(span.begin(), span.end()); }

#pragma endregion Arenas

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

/** What a half-edge holds besides its name. */
struct model_payload_t {
    float weight = 0;
    std::int32_t label = 0;
};

using model_adjacency_t = std::map<model_key_t, model_payload_t>;

/** Applies one record to @p adjacency, the way the fold promises to. */
static void model_apply(model_adjacency_t& adjacency, half_edge_delta_t const& delta) {
    switch (delta.operation) {
    case half_edge_operation_t::insert_k:
        adjacency[model_key_t {delta.neighbor, delta.edge, delta.role.bits}] = {delta.weight, delta.label.value};
        break;
    case half_edge_operation_t::remove_k:
        adjacency.erase(model_key_t {delta.neighbor, delta.edge, delta.role.bits});
        break;
    case half_edge_operation_t::remove_vertex_k: adjacency.clear(); break;
    }
}

/** The vertex value of @p adjacency: the inserts of its half-edges in the model's order, written field by field. */
static bytes_t value_of_model(model_adjacency_t const& adjacency) {
    bytes_t bytes(adjacency.size() * half_edge_delta_bytes_k);
    std::size_t offset = 0;
    for (auto const& [key, payload] : adjacency) {
        std::uint8_t const operation = 0;
        std::memcpy(bytes.data() + offset, &key.neighbor, 16);
        std::memcpy(bytes.data() + offset + 16, &key.edge, 16);
        std::memcpy(bytes.data() + offset + 32, &payload.weight, 4);
        std::memcpy(bytes.data() + offset + 36, &payload.label, 4);
        std::memcpy(bytes.data() + offset + 40, &key.role, 1);
        std::memcpy(bytes.data() + offset + 41, &operation, 1);
        offset += half_edge_delta_bytes_k;
    }
    return bytes;
}

/** Whether a random key's first eight bytes are random too, or shared with every other key spelled
 *  so. */
enum class key_prefix_t : bool { random_k, shared_k };

/** Random keys from small pools, sharing their first or last eight bytes, so 128-bit orders get exercised. */
struct key_pools_t {
    std::vector<ustore_key_t> vertices, neighbors, edges;

    explicit key_pools_t(split_mix_generator_t& generator, std::size_t vertex_count = 6,
                         std::size_t neighbor_count = 220) {
        auto random_key = [&](key_prefix_t prefix) {
            ustore_key_t key {};
            for (std::size_t index = 0; index != 16; ++index) key.uuid.bytes[index] = std::uint8_t(generator());
            if (prefix == key_prefix_t::shared_k)
                for (std::size_t index = 0; index != 8; ++index) key.uuid.bytes[index] = std::uint8_t(index);
            return key;
        };
        for (std::size_t index = 0; index != vertex_count; ++index)
            vertices.push_back(random_key(key_prefix_t::random_k));
        // Vertices are neighbors too, so loops and shared neighbors show up
        for (ustore_key_t const& vertex : vertices) neighbors.push_back(vertex);
        while (neighbors.size() != neighbor_count)
            neighbors.push_back(random_key(generator.below(2) ? key_prefix_t::shared_k : key_prefix_t::random_k));
        for (std::size_t index = 0; index != 4; ++index)
            edges.push_back(random_key(index % 2 ? key_prefix_t::shared_k : key_prefix_t::random_k));
    }
};

static role_t random_role(split_mix_generator_t& generator) {
    role_t const roles[3] = {edge_member_k, dedge_source_k, dedge_target_k};
    return roles[generator.below(3)];
}

/** A random record, removals aimed at what @p model holds three times in four. */
static half_edge_delta_t random_delta(split_mix_generator_t& generator, key_pools_t const& pools,
                                      model_adjacency_t const& model, std::size_t removal_percent,
                                      std::size_t vertex_removal_permille = 0) {
    if (generator.below(1000) < vertex_removal_permille) return half_edge_delta_t::remove_vertex();
    if (generator.below(100) < removal_percent) {
        if (!model.empty() && generator.below(4)) {
            auto picked = model.begin();
            std::advance(picked, generator.below(model.size()));
            return half_edge_delta_t::remove(picked->first.neighbor, picked->first.edge, role_t {picked->first.role});
        }
        return half_edge_delta_t::remove(pools.neighbors[generator.below(pools.neighbors.size())],
                                         pools.edges[generator.below(pools.edges.size())], random_role(generator));
    }
    labeled_half_edge_t half_edge;
    half_edge.neighbor_id = pools.neighbors[generator.below(pools.neighbors.size())];
    half_edge.edge_id = pools.edges[generator.below(pools.edges.size())];
    half_edge.role = random_role(generator);
    half_edge.edge_weight = float(generator.below(1000)) / 8;
    half_edge.edge_label.value = std::int32_t(generator.below(5)) - 1;
    return half_edge_delta_t::insert(half_edge);
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

/** The composition of @p views as bytes, or nothing on a refusal. */
static bytes_t composed_bytes(std::span<delta_view_t const> views, test_arena_t& arena) {
    bytes_allocator_t allocator(&arena);
    auto composed = adjacency_fold_t {}.compose(views, allocator);
    return composed ? to_bytes(*composed) : bytes_t {};
}

#pragma endregion Reference Model

#pragma region Store Under Test

/** One vertex of the store: its value and the chains of pending changes, oldest first. */
struct stored_vertex_t {
    ustore_key_t vertex {};
    bytes_t value;
    std::vector<bytes_t> pending;
};

/** Composes the pending chains of @p vertex for a read, into @p arena. */
static expected<std::span<byte_t const>> pending_chain(stored_vertex_t const& vertex, test_arena_t& arena) {
    if (vertex.pending.empty()) return std::span<byte_t const> {};
    bytes_allocator_t allocator(&arena);
    std::vector<delta_view_t> const views = views_of(vertex.pending);
    return adjacency_fold_t {}.compose(views, allocator);
}

/** Folds the pending chains of @p vertex into its value, as compaction does, which must equal @p
 *  model's value; answers whether it did. */
static bool fold_vertex(stored_vertex_t& vertex, model_adjacency_t const& model) {
    test_arena_t arena;
    bytes_allocator_t allocator(&arena);
    std::vector<delta_view_t> const views = views_of(vertex.pending);
    auto result = adjacency_fold_t {}.apply(vertex.value, views, allocator);
    if (!expect_status(result.code(), status_code_t::success_k)) return false;
    vertex.value = to_bytes(*result);
    vertex.pending.clear();
    return expect_true(vertex.value == value_of_model(model));
}

/** Every read of @p vertex matches @p model: the walk, degrees by role, edges to neighbors, and shared neighbors
 *  with @p other; answers whether every one did. */
static bool check_reads(stored_vertex_t const& vertex, model_adjacency_t const& model, stored_vertex_t const& other,
                        model_adjacency_t const& other_model, key_pools_t const& pools,
                        split_mix_generator_t& generator) {
    bool held = true;
    test_arena_t arena;
    auto pending = pending_chain(vertex, arena);
    if (!expect_status(pending.code(), status_code_t::success_k)) return false;
    auto adjacency = vertex_adjacency_t::make(vertex.vertex, vertex.value, *pending);
    if (!expect_status(adjacency.code(), status_code_t::success_k)) return false;

    // The walk yields the records the fold would write, so they concatenate into the model's value
    bytes_t walked;
    for (half_edge_cursor_t cursor = adjacency->half_edges(); cursor.valid(); cursor.next())
        walked.insert(walked.end(), cursor.record(), cursor.record() + half_edge_delta_bytes_k);
    held = expect_true(walked == value_of_model(model)) && held;
    std::size_t visited = 0;
    status_t const visit = adjacency->for_each([&](labeled_half_edge_t const&) noexcept {
        ++visited;
        return status_t {};
    });
    held = expect_true(visit.succeeded() && visited == model.size()) && held;

    for (role_t const roles : {any_role_k, edge_member_k, dedge_source_k, dedge_source_k | dedge_target_k}) {
        std::size_t wanted = 0;
        for (auto const& [key, payload] : model) wanted += (role_t {key.role} & roles) ? 1 : 0;
        held = expect_true(adjacency->degree(roles) == wanted) && held;
    }

    for (std::size_t probe = 0; probe != 6; ++probe) {
        ustore_key_t neighbor = pools.neighbors[generator.below(pools.neighbors.size())];
        if (probe % 2 && !model.empty()) {
            auto picked = model.begin();
            std::advance(picked, generator.below(model.size()));
            neighbor = picked->first.neighbor;
        }
        model_adjacency_t wanted, found;
        for (auto const& [key, payload] : model)
            if (key.neighbor == neighbor) wanted[key] = payload;
        status_t const to = adjacency->edges_to(neighbor, [&](labeled_half_edge_t const& half_edge) noexcept {
            found[model_key_t {half_edge.neighbor_id, half_edge.edge_id, half_edge.role.bits}] = {
                half_edge.edge_weight, half_edge.edge_label.value};
            return status_t {};
        });
        held = expect_true(to.succeeded() && value_of_model(found) == value_of_model(wanted)) && held;

        // Edges lose which end of a directed loop was read, so they compare by identifier, payload and direction
        std::multiset<std::tuple<std::uint64_t, std::uint64_t, float, std::int32_t, direction_t>> wanted_edges,
            found_edges;
        auto edge_row = [](ustore_key_t const& id, float weight, std::int32_t label, direction_t direction) {
            std::uint64_t halves[2];
            std::memcpy(halves, &id, 16);
            return std::tuple {halves[0], halves[1], weight, label, direction};
        };
        for (auto const& [key, payload] : wanted)
            wanted_edges.insert(
                edge_row(key.edge, payload.weight, payload.label,
                         key.role != edge_member_k.bits ? direction_t::directed_k : direction_t::undirected_k));
        std::size_t stray_endpoints = 0;
        status_t const between = edges_between(*adjacency, neighbor, [&](labeled_edge_t const& edge) noexcept {
            if (!edge.contains(vertex.vertex) || !edge.contains(neighbor)) ++stray_endpoints;
            found_edges.insert(edge_row(edge.id, edge.weight, edge.label.value, edge.direction));
            return status_t {};
        });
        held = expect_true(between.succeeded() && stray_endpoints == 0 && found_edges == wanted_edges) && held;
    }

    test_arena_t other_arena;
    auto other_pending = pending_chain(other, other_arena);
    if (!expect_true(other_pending.has_value())) return false;
    auto other_adjacency = vertex_adjacency_t::make(other.vertex, other.value, *other_pending);
    if (!expect_true(other_adjacency.has_value())) return false;
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
    return expect_true(common.succeeded() && shared == wanted) && held;
}

#pragma endregion Store Under Test

#pragma region Cases

/** Records round-trip, malformed records and values are refused, and edges file the half-edges their roles say. */
void test_codecs() {
    split_mix_generator_t generator {case_stream_key()};
    key_pools_t const pools(generator);
    for (std::size_t round = 0; round != 200; ++round) {
        half_edge_delta_t const delta = random_delta(generator, pools, {}, 30, 50);
        byte_t record[half_edge_delta_bytes_k];
        save_delta(delta, record);
        expect_true(check_delta(record).succeeded());
        half_edge_delta_t const loaded = load_delta(record);
        expect_true(loaded.neighbor == delta.neighbor && loaded.edge == delta.edge && loaded.weight == delta.weight &&
                    loaded.label == delta.label && loaded.role == delta.role && loaded.operation == delta.operation);
    }

    byte_t record[half_edge_delta_bytes_k];
    save_delta(half_edge_delta_t::remove(pools.neighbors[9], pools.edges[0], edge_member_k), record);
    record[delta_operation_offset_k] = byte_t {3};
    expect_status(check_delta(record).code, status_code_t::malformed_k);
    record[delta_operation_offset_k] = byte_t {1}, record[half_edge_delta_bytes_k - 1] = byte_t {1};
    expect_status(check_delta(record).code, status_code_t::malformed_k);
    // The nil key is a vertex of the C ABI like any other, and the operation tells a removal of the vertex apart
    save_delta(half_edge_delta_t::remove(smallest_key_k, pools.edges[0], edge_member_k), record);
    expect_true(check_delta(record).succeeded());
    save_delta(half_edge_delta_t::remove_vertex(), record);
    expect_true(check_delta(record).succeeded());
    expect_status(half_edge_deltas_view_t::make(std::span<byte_t const>(record, half_edge_delta_bytes_k - 1)).code(),
                  status_code_t::malformed_k);

    // A value out of order, with a repeated half-edge, or holding a removal is refused by the fold
    model_adjacency_t pair;
    while (pair.size() != 2) model_apply(pair, random_delta(generator, pools, pair, 0));
    bytes_t const value = value_of_model(pair);
    test_arena_t arena;
    bytes_allocator_t allocator(&arena);
    adjacency_fold_t const fold;
    expect_true(fold.apply(value, {}, allocator).has_value());
    bytes_t swapped(value.begin() + half_edge_delta_bytes_k, value.end());
    swapped.insert(swapped.end(), value.begin(), value.begin() + half_edge_delta_bytes_k);
    expect_status(fold.apply(swapped, {}, allocator).code(), status_code_t::malformed_k);
    bytes_t repeated(value.begin(), value.begin() + half_edge_delta_bytes_k);
    repeated.insert(repeated.end(), repeated.begin(), repeated.end());
    expect_status(fold.apply(repeated, {}, allocator).code(), status_code_t::malformed_k);
    bytes_t holding_removal = value;
    holding_removal[delta_operation_offset_k] = byte_t {1};
    expect_status(fold.apply(holding_removal, {}, allocator).code(), status_code_t::malformed_k);
    expect_status(vertex_adjacency_t::make(pools.vertices[0], value, swapped).code(), status_code_t::malformed_k);

    // Edges file one half-edge per end, a directed loop two, and an undirected loop one
    labeled_edge_t edge;
    edge.id = pools.edges[1], edge.first = pools.vertices[0], edge.second = pools.vertices[1];
    id_entity_t vertices[2];
    half_edge_delta_t deltas[2];
    expect_true(deltas_of_edge(edge, half_edge_operation_t::insert_k, vertices, deltas) == 2);
    expect_true(vertices[0] == edge.first && deltas[0].neighbor == edge.second && deltas[0].role == dedge_source_k);
    expect_true(vertices[1] == edge.second && deltas[1].neighbor == edge.first && deltas[1].role == dedge_target_k);
    edge.second = edge.first;
    expect_true(deltas_of_edge(edge, half_edge_operation_t::remove_k, vertices, deltas) == 2);
    expect_true(deltas[0].role == dedge_source_k && deltas[1].role == dedge_target_k &&
                deltas[1].operation == half_edge_operation_t::remove_k);
    edge.direction = direction_t::undirected_k;
    expect_true(deltas_of_edge(edge, half_edge_operation_t::insert_k, vertices, deltas) == 1);
    expect_true(deltas[0].role == edge_member_k);
}

/** Applying a chain matches the model, as does applying its composition, a composition of three parts in either
 *  grouping, and the chain from its newest closing record onto nothing. */
void test_compose_matches_sequential() {
    split_mix_generator_t generator {case_stream_key()};
    // Few enough half-edges that chains hit what the base holds
    key_pools_t const pools(generator, 3, 90);
    adjacency_fold_t const fold;
    for (std::size_t round = 0; round != 1500; ++round) {
        model_adjacency_t base_model;
        std::size_t const base_size = round % 5 ? generator.below(80) : generator.below(700);
        while (base_model.size() < base_size) model_apply(base_model, random_delta(generator, pools, base_model, 0));
        bytes_t const base = value_of_model(base_model);

        model_adjacency_t model = base_model;
        std::vector<bytes_t> chains;
        std::size_t const chain_count = 1 + generator.below(6);
        for (std::size_t chain = 0; chain != chain_count; ++chain) {
            std::vector<half_edge_delta_t> records;
            std::size_t const record_count = generator.below(20);
            for (std::size_t record = 0; record != record_count; ++record) {
                half_edge_delta_t const delta = random_delta(generator, pools, model, 45, 30);
                model_apply(model, delta);
                records.push_back(delta);
            }
            chains.push_back(encode_chain(records));
        }
        std::vector<delta_view_t> const views = views_of(chains);
        bytes_t const wanted = value_of_model(model);
        test_arena_t arena;
        bytes_allocator_t allocator(&arena);

        auto applied = fold.apply(base, views, allocator);
        expect_true(applied.has_value() && to_bytes(*applied) == wanted);

        auto composed = fold.compose(views, allocator);
        expect_status(composed.code(), status_code_t::success_k);
        if (!composed) continue;
        bytes_t const whole = to_bytes(*composed);
        delta_view_t const composed_view {delta_kind_t::change_k, whole};
        auto from_composed = fold.apply(base, std::span<delta_view_t const>(&composed_view, 1), allocator);
        expect_true(from_composed.has_value() && to_bytes(*from_composed) == wanted);

        // A composed chain names each half-edge once, in order, after at most one leading removal of the vertex,
        // which it holds whenever a record it composed removed the vertex
        auto chain = half_edge_deltas_view_t::make(whole);
        expect_true(chain.has_value() && check_composed(*chain).succeeded());
        bool removes_vertex = false;
        for (bytes_t const& bytes : chains)
            for (std::size_t offset = 0; offset != bytes.size(); offset += half_edge_delta_bytes_k)
                removes_vertex = removes_vertex ||
                                 load_delta(bytes.data() + offset).operation == half_edge_operation_t::remove_vertex_k;
        expect_true(fold.closes(composed_view) == removes_vertex);
        if (removes_vertex)
            for (std::size_t index = 1; chain && index < chain->size(); ++index)
                expect_true((*chain)[index].operation == half_edge_operation_t::insert_k);

        // A read stops at the newest closing change and folds it onto nothing
        std::size_t closing = views.size();
        while (closing && !fold.closes(views[closing - 1])) --closing;
        if (closing) {
            auto from_closing = fold.apply({}, std::span<delta_view_t const>(views).subspan(closing - 1), allocator);
            expect_true(from_closing.has_value() && to_bytes(*from_closing) == wanted);
        }

        // Three parts compose alike in either grouping and as a whole, byte for byte
        std::size_t const first_cut = generator.below(views.size() + 1);
        std::size_t const second_cut = first_cut + generator.below(views.size() - first_cut + 1);
        std::span<delta_view_t const> const deltas(views);
        std::span<delta_view_t const> const parts[3] = {
            deltas.first(first_cut), deltas.subspan(first_cut, second_cut - first_cut), deltas.subspan(second_cut)};
        auto composed_of = [&](std::span<delta_view_t const> part) {
            return part.empty() ? bytes_t {} : composed_bytes(part, arena);
        };
        bytes_t const left = composed_of(parts[0]), middle = composed_of(parts[1]), right = composed_of(parts[2]);
        delta_view_t const left_middle[2] = {{delta_kind_t::change_k, left}, {delta_kind_t::change_k, middle}};
        delta_view_t const middle_right[2] = {{delta_kind_t::change_k, middle}, {delta_kind_t::change_k, right}};
        bytes_t const left_grouped = composed_bytes(left_middle, arena);
        bytes_t const right_grouped = composed_bytes(middle_right, arena);
        delta_view_t const left_first[2] = {{delta_kind_t::change_k, left_grouped}, {delta_kind_t::change_k, right}};
        delta_view_t const right_first[2] = {{delta_kind_t::change_k, left}, {delta_kind_t::change_k, right_grouped}};
        expect_true(composed_bytes(left_first, arena) == whole);
        expect_true(composed_bytes(right_first, arena) == whole);
        auto from_parts = fold.apply(base, right_first, allocator);
        expect_true(from_parts.has_value() && to_bytes(*from_parts) == wanted);

        // A value write in the middle restarts from that value, which compose leaves to apply
        if (round % 7 == 0) {
            model_adjacency_t restart;
            std::size_t const restart_size = generator.below(30);
            while (restart.size() < restart_size) model_apply(restart, random_delta(generator, pools, restart, 0));
            bytes_t const restart_value = value_of_model(restart);
            std::vector<delta_view_t> mixed = views;
            std::size_t const at = generator.below(mixed.size() + 1);
            mixed.insert(mixed.begin() + std::ptrdiff_t(at), delta_view_t {delta_kind_t::write_k, restart_value});
            for (std::size_t index = at + 1; index != mixed.size(); ++index) {
                auto records = *half_edge_deltas_view_t::make(mixed[index].bytes);
                for (std::size_t record = 0; record != records.size(); ++record) model_apply(restart, records[record]);
            }
            auto restarted = fold.apply(base, mixed, allocator);
            expect_true(restarted.has_value() && to_bytes(*restarted) == value_of_model(restart));
            expect_status(fold.compose(mixed, allocator).code(), status_code_t::invalid_argument_k);
            expect_true(fold.closes(mixed[at]));
        }
    }
}

/** An insert and a removal of one half-edge cancel after a removal of the vertex and collapse to the removal
 *  otherwise, a removal of the vertex leaves a value without half-edges, and only a leading one closes a chain. */
void test_compose_shapes() {
    split_mix_generator_t generator {case_stream_key()};
    key_pools_t const pools(generator, 2, 20);
    adjacency_fold_t const fold;
    test_arena_t arena;
    bytes_allocator_t allocator(&arena);
    labeled_half_edge_t half_edge;
    half_edge.neighbor_id = pools.neighbors[5], half_edge.edge_id = pools.edges[2], half_edge.role = edge_member_k;
    half_edge_delta_t const insert = half_edge_delta_t::insert(half_edge);
    half_edge_delta_t const remove = half_edge_delta_t::remove(half_edge.neighbor_id, half_edge.edge_id,
                                                               half_edge.role);
    half_edge_delta_t const remove_vertex = half_edge_delta_t::remove_vertex();

    bytes_t const open_chain = encode_chain({insert, remove});
    delta_view_t const open_view {delta_kind_t::change_k, open_chain};
    auto open = fold.compose(std::span<delta_view_t const>(&open_view, 1), allocator);
    expect_true(open.has_value() && to_bytes(*open) == encode_chain({remove}));
    expect_true(!fold.closes(open_view));

    bytes_t const closed_chain = encode_chain({insert, remove_vertex, insert, remove});
    delta_view_t const closed_view {delta_kind_t::change_k, closed_chain};
    auto closed = fold.compose(std::span<delta_view_t const>(&closed_view, 1), allocator);
    expect_true(closed.has_value() && to_bytes(*closed) == encode_chain({remove_vertex}));
    // The removal of the vertex is not at the head, so the chain does not close, though its composition does
    expect_true(!fold.closes(closed_view));
    delta_view_t const composed_view {delta_kind_t::change_k, *closed};
    expect_true(fold.closes(composed_view));

    // Only a removal of the vertex is left, which reads as a vertex without half-edges
    model_adjacency_t base_model;
    while (base_model.size() != 10) model_apply(base_model, random_delta(generator, pools, base_model, 0));
    bytes_t const base = value_of_model(base_model);
    auto cleared = fold.apply(base, std::span<delta_view_t const>(&composed_view, 1), allocator);
    expect_true(cleared.has_value() && cleared->empty());
    auto reread = vertex_adjacency_t::make(pools.vertices[0], base, *closed);
    expect_true(reread.has_value() && reread->degree() == 0 && !reread->half_edges().valid());

    bytes_t const empty_chain;
    delta_view_t const empty_view {delta_kind_t::change_k, empty_chain};
    auto empty = fold.compose(std::span<delta_view_t const>(&empty_view, 1), allocator);
    expect_true(empty.has_value() && empty->empty());
    expect_true(!fold.closes(empty_view));
    expect_status(fold.compose({}, allocator).code(), status_code_t::invalid_argument_k);
    expect_true(fold.closes({delta_kind_t::write_k, {}}) && fold.closes({delta_kind_t::remove_k, {}}));

    // A removal of the vertex value under a change folds the change onto nothing
    bytes_t const lone_insert = encode_chain({insert});
    delta_view_t const removed_then_inserted[2] = {{delta_kind_t::remove_k, {}}, {delta_kind_t::change_k, lone_insert}};
    auto revived = fold.apply(base, removed_then_inserted, allocator);
    expect_true(revived.has_value() && to_bytes(*revived) == lone_insert);
}

/** Random streams of inserts and removals over a few vertices, read after every step and folded at random points. */
void test_random_streams() {
    for (std::uint64_t seed = 0; seed != 6; ++seed) {
        split_mix_generator_t generator {case_stream_key(seed)};
        key_pools_t const pools(generator, 5, 700);
        std::vector<stored_vertex_t> store;
        std::vector<model_adjacency_t> models(pools.vertices.size());
        for (ustore_key_t const& vertex : pools.vertices) store.push_back(stored_vertex_t {vertex, {}, {}});
        std::size_t const steps = 3000;
        std::size_t folds = 0, largest = 0, vertex_removals = 0;
        for (std::size_t step = 0; step != steps; ++step) {
            // Phases grow the hub, shrink it, then mix both
            std::size_t const phase = step / 1000;
            std::size_t const removal_percent = phase == 0 ? 10 : phase == 1 ? 80 : 40;
            std::size_t const target = generator.below(2) ? 0 : generator.below(store.size());
            stored_vertex_t& vertex = store[target];
            model_adjacency_t& model = models[target];

            std::vector<half_edge_delta_t> records;
            std::size_t const record_count = generator.below(10) == 0 ? 20 + generator.below(120)
                                                                      : 1 + generator.below(4);
            // The hub loses everything once, late, so a removal of the vertex closes a large value
            std::size_t const vertex_removal_permille = target ? 20 : step == 2600 ? 1000 : 0;
            for (std::size_t record = 0; record != record_count; ++record) {
                half_edge_delta_t const delta = random_delta(generator, pools, model, removal_percent,
                                                             record ? 0 : vertex_removal_permille);
                vertex_removals += delta.operation == half_edge_operation_t::remove_vertex_k;
                model_apply(model, delta);
                records.push_back(delta);
            }
            vertex.pending.push_back(encode_chain(records));

            if (generator.below(20) == 0 && vertex.pending.size() > 1) {
                test_arena_t arena;
                auto composed = pending_chain(vertex, arena);
                if (!expect_true(composed.has_value())) return;
                vertex.pending = {to_bytes(*composed)};
            }
            if (generator.below(12) == 0) {
                if (!fold_vertex(vertex, model)) return;
                ++folds;
                largest = fu::max_of(largest, model.size());
            }
            std::size_t const other = generator.below(store.size());
            if (!check_reads(vertex, model, store[other], models[other], pools, generator)) return;
        }
        for (std::size_t index = 0; index != store.size(); ++index) {
            fold_vertex(store[index], models[index]);
            check_reads(store[index], models[index], store[(index + 1) % store.size()],
                        models[(index + 1) % store.size()], pools, generator);
        }
        expect_true(folds > 100 && largest > 1000 && vertex_removals > 2,
                    "seed {}: {} folds, {} half-edges at most, {} vertex removals", seed, folds, largest,
                    vertex_removals);
    }
}

/** One vertex of 200,000 half-edges, past the 4 MiB cap of written values, built, changed and read by the fold. */
void test_hub_vertex() {
    split_mix_generator_t generator {case_stream_key()};
    std::size_t const half_edges = 200'000;
    key_pools_t const pools(generator, 2, 2);
    std::vector<half_edge_delta_t> records;
    model_adjacency_t model;
    for (std::size_t index = 0; index != half_edges; ++index) {
        labeled_half_edge_t half_edge;
        for (std::size_t byte = 0; byte != 16; ++byte)
            half_edge.neighbor_id.uuid.bytes[byte] = std::uint8_t(generator());
        half_edge.edge_id = pools.edges[index % pools.edges.size()];
        half_edge.role = random_role(generator);
        half_edge.edge_weight = float(index);
        records.push_back(half_edge_delta_t::insert(half_edge));
        model_apply(model, records.back());
    }
    expect_true(model.size() == half_edges);
    expect_true(half_edges * half_edge_delta_bytes_k > 4u << 20);

    // Unsorted inserts in four changes onto no value
    std::vector<bytes_t> chains;
    for (std::size_t part = 0; part != 4; ++part)
        chains.push_back(encode_chain(
            std::vector<half_edge_delta_t>(records.begin() + std::ptrdiff_t(part * half_edges / 4),
                                           records.begin() + std::ptrdiff_t((part + 1) * half_edges / 4))));
    test_arena_t arena;
    bytes_allocator_t allocator(&arena);
    adjacency_fold_t const fold;
    std::vector<delta_view_t> const views = views_of(chains);
    auto built = fold.apply({}, views, allocator);
    expect_true(built.has_value() && to_bytes(*built) == value_of_model(model));
    if (!built) return;
    stored_vertex_t hub {pools.vertices[0], to_bytes(*built), {}};

    // A few removals and overwrites stay pending, then fold
    std::vector<half_edge_delta_t> changes;
    for (std::size_t index = 0; index != 100; ++index) {
        half_edge_delta_t const& chosen = records[generator.below(records.size())];
        half_edge_delta_t change = chosen;
        if (index % 2) change = half_edge_delta_t::remove(chosen.neighbor, chosen.edge, chosen.role);
        else change.weight = -1;
        model_apply(model, change);
        changes.push_back(change);
    }
    hub.pending.push_back(encode_chain(changes));

    stored_vertex_t leaf {pools.vertices[1], {}, {}};
    model_adjacency_t leaf_model;
    std::vector<half_edge_delta_t> leaf_records;
    for (std::size_t index : {7u, 70'000u, 199'999u}) {
        labeled_half_edge_t half_edge = records[index].half_edge();
        half_edge.edge_id = pools.edges[0];
        leaf_records.push_back(half_edge_delta_t::insert(half_edge));
        model_apply(leaf_model, leaf_records.back());
    }
    leaf.pending.push_back(encode_chain(leaf_records));
    check_reads(hub, model, leaf, leaf_model, pools, generator);
    fold_vertex(hub, model);
    check_reads(hub, model, leaf, leaf_model, pools, generator);
}

#pragma endregion Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore::test;
    start_test_run(argc, argv);
    run_test_case("test_codecs", test_codecs);
    run_test_case("test_compose_matches_sequential", test_compose_matches_sequential);
    run_test_case("test_compose_shapes", test_compose_shapes);
    run_test_case("test_random_streams", test_random_streams);
    run_test_case("test_hub_vertex", test_hub_vertex);
    return finish_test_run();
}
