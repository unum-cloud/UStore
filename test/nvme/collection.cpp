/**
 *  @file test/nvme/collection.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The collection over the ring: what the write buffer answers, what the tree answers, and how deep it reads.
 *
 *  The results this file exists for are @c holds_its_depth and @c snapshot_holds_its_moment. A batched
 *  select must sit at the queue pair's depth through the middle of a run rather than at the size of whatever
 *  batch a caller handed it, and a snapshot must keep answering the level set it was taken from while
 *  another thread publishes over it.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcmp`

#include <array>   // `std::array`
#include <memory>  // `std::unique_ptr`
#include <new>     // `std::nothrow`
#include <span>    // `std::span`
#include <thread>  // `std::thread`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/blobs/nvme_collection.hpp" // `collection`
#include "ustore/blobs/nvme_key_slot.hpp"   // `key_slot`

#include "nvme/fixture.hpp" // `basic_fixture`, `run_on_transport`, `expectation_t`, `painted`

namespace unum::ustore::test {

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Shape

/** The numbers every case mounts with, which are the defaults: channels enough for a compaction to hold two at once. */
constexpr fixture_config_t collection_config_k {};

/** The largest value the variable-width collections under test admit. */
constexpr stored_value_size_t value_max_size_k = 64ull * 1024ull;

/** The keys and the key slots every case of this suite drives. */
using stored_key_t = std::uint64_t;
using variable_slot_t = key_slot<stored_key_t, value_layout_t::variable_width_k>;

/** The collections and the scanners every case drives over the volume of @p fixture_type_. */
template <typename fixture_type_>
struct collection_types {
    using fixture_t = fixture_type_;
    using guard_t = typename fixture_t::guard_t;
    using volume_t = typename fixture_t::volume_t;
    using variable_collection_t = collection<variable_slot_t, volume_t>;
    using variable_scanner_t = collection_scanner<variable_slot_t, volume_t>;

    /** A fixture, one checked-out channel and the volume mounted over it, which every case starts from. */
    struct stack_t {

        /** The disk, the arena and the channels. */
        std::unique_ptr<fixture_t> fixture;

        /** The channel the calling thread submits and polls on. */
        guard_t guard;

        /** The volume every run lands on, pinned so a collection may point at it. */
        std::unique_ptr<volume_t> volume;

        /** The stamps every collection of the stack carries, outliving the collections. */
        commit_ledger_t ledger;

        /** The channel of this stack, which every call that moves bytes takes. */
        [[nodiscard]] io_channel_t& channel() noexcept { return guard.channel(); }
    };
};

/**
 *  @brief The collection shape a case mounts, whose first level fills after a handful of flushes.
 *  @param[in] value_size Bytes every value takes, which is zero for every layout but the fixed one.
 *  @param[in] value_max_size Bytes the largest value takes, which is zero for a fixed-width layout.
 *  @param[in] level_zero_bytes Bytes level zero admits before a compaction is due.
 */
[[nodiscard]] collection_config_t test_config(stored_value_size_t value_size, stored_value_size_t value_max_size,
                                              std::uint64_t level_zero_bytes) noexcept {
    collection_config_t config;
    config.concurrency_limit = 1;
    config.cache_limit = 1024ull * 1024ull;
    config.write_buffer_max_bytes = 8ull * 1024ull * 1024ull;
    config.write_buffer_max_elements = 8192;
    config.lsm_tree.level_enlarge_factor = 4;
    config.lsm_tree.lsm_level_0.max_bytes = level_zero_bytes;
    config.lsm_tree.lsm_level_0.sorted_run.value_size = value_size;
    config.lsm_tree.lsm_level_0.sorted_run.value_max_size = value_max_size;
    return config;
}

#pragma endregion Shape

#pragma region Fixtures

/** Mounts a fixture through @p mount, takes one channel and formats one volume over it. */
template <typename mount_type_>
expected<std::unique_ptr<typename collection_types<typename mount_type_::fixture_t>::stack_t>> make_stack(
    mount_type_ const& mount) noexcept {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using volume_t = typename types_t::volume_t;
    auto stack = std::unique_ptr<stack_t> {new (std::nothrow) stack_t {}};
    if (!stack) return status_t {status_code_t::out_of_memory_k, "The stack did not fit"};

    auto [fixture, mount_status] = mount();
    if (mount_status.failed()) return mount_status;
    stack->fixture = std::move(fixture);

    auto [guard, acquire_status] = stack->fixture->channels.acquire();
    if (acquire_status.failed()) return acquire_status;
    stack->guard = std::move(guard);

    expected<volume_t> volume = stack->fixture->mount_volume(stack->guard.channel());
    if (!volume) return volume.status();
    stack->volume = std::unique_ptr<volume_t> {new (std::nothrow) volume_t {std::move(*volume)}};
    if (!stack->volume) return status_t {status_code_t::out_of_memory_k, "The volume did not fit"};
    return stack;
}

#pragma endregion Fixtures

#pragma region Model

/** Writes every entry of @p wanted through the write buffer of @p store, one call per key. */
template <typename collection_type_>
[[nodiscard]] std::size_t write_model(collection_type_& store, io_channel_t& channel,
                                      std::span<expectation_t const> wanted) {
    std::size_t failures = 0;
    st::basic_vector<byte_t> value;
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        expectation_t const& entry = wanted[index];
        if (entry.state == select_result_t::erased_k) {
            failures += expect(store.erase(channel, entry.stored_key, store.newest_stamp() + 1).succeeded());
            continue;
        }
        failures += expect(value.resize(static_cast<std::size_t>(entry.bytes)) == st::status_t::success_k);
        paint_into(entry, std::span<byte_t> {value});
        failures += expect(
            store.insert(channel, entry.stored_key, std::span<byte_t const> {value}, store.newest_stamp() + 1)
                .succeeded());
    }
    return failures;
}

/** Reads every key of @p wanted through one batched select and counts what the model disagrees with. */
template <typename readable_type_>
[[nodiscard]] std::size_t check_model(readable_type_ const& source, io_channel_t& channel,
                                      std::span<expectation_t const> wanted) {
    std::size_t failures = 0;
    std::size_t const count = wanted.size();
    st::basic_vector<stored_key_t> keys;
    st::basic_vector<select_result_t> results;
    st::basic_vector<std::uint64_t> seen;
    failures += expect(keys.resize(count) == st::status_t::success_k);
    failures += expect(results.resize(count) == st::status_t::success_k);
    failures += expect(seen.resize(count) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count; ++index) {
        keys[index] = wanted[index].stored_key;
        results[index] = select_result_t::nothing_k;
        seen[index] = 0;
    }

    std::size_t mismatches = 0;
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        seen[request] += bytes.size();
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(wanted[request].seed, offset + index)) ++mismatches;
    };
    failures += expect(
        source.select(channel, std::span<stored_key_t const> {keys}, std::span<select_result_t> {results}, sink)
            .succeeded());
    failures += expect_eq(mismatches, std::size_t {0});

    for (std::size_t index = 0; index != count; ++index) {
        failures += expect(results[index] == wanted[index].state);
        if (wanted[index].state == select_result_t::success_k) failures += expect_eq(seen[index], wanted[index].bytes);
    }
    return failures;
}

/** Reads every key of @p wanted one at a time, which is the path that fills the value cache. */
template <typename readable_type_>
[[nodiscard]] std::size_t check_model_one_by_one(readable_type_ const& source, io_channel_t& channel,
                                                 std::span<expectation_t const> wanted) {
    std::size_t failures = 0;
    st::basic_vector<byte_t> into;
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        expectation_t const& entry = wanted[index];
        stored_value_size_t size = 0;
        if (entry.state != select_result_t::success_k) {
            failures += expect(source.select(channel, entry.stored_key, std::span<byte_t> {}, size).failed());
            continue;
        }
        failures += expect(into.resize(static_cast<std::size_t>(entry.bytes)) == st::status_t::success_k);
        failures += expect(source.select(channel, entry.stored_key, std::span<byte_t> {into}, size).succeeded());
        failures += expect_eq(static_cast<std::uint64_t>(size), entry.bytes);
        for (std::uint64_t position = 0; position != entry.bytes; ++position)
            if (into[static_cast<std::size_t>(position)] != painted(entry.seed, position)) ++mismatches;
    }
    failures += expect_eq(mismatches, std::size_t {0});
    return failures;
}

#pragma endregion Model

#pragma region Round Trip Cases

/** Values of many widths survive the write buffer, the flush and the read back, byte for byte. */
template <typename mount_type_>
std::size_t mixed_widths_round_trip(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_collection_t = typename types_t::variable_collection_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 700;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k, 16ull * 1024ull * 1024ull);
    expected<variable_collection_t> store = variable_collection_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::basic_vector<expectation_t> model;
    failures += expect(model.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 2 + 1;
        entry.state = index % 11 == 0 ? select_result_t::erased_k : select_result_t::success_k;
        entry.bytes = entry.state == select_result_t::success_k ? 1 + index % 4093 : 0;
        entry.seed = index;
        model[index] = entry;
    }

    failures += write_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});

    // Everything is still staged, so the write buffer is what answers.
    failures += expect_eq(store->staged_count(), count_k);
    failures += check_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});

    failures += expect(store->flush((*stack)->channel()).succeeded());
    failures += expect_eq(store->staged_count(), std::size_t {0});
    failures += check_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});
    failures += check_model_one_by_one(*store, (*stack)->channel(), std::span<expectation_t const> {model});

    // Reading again now goes through the value cache the point reads just filled.
    failures += check_model_one_by_one(*store, (*stack)->channel(), std::span<expectation_t const> {model});
    return failures;
}

/** Writes and reads one collection of @p key_slot_type_, which is how every layout is covered. */
template <typename key_slot_type_, typename mount_type_>
[[nodiscard]] std::size_t layout_round_trip(mount_type_ const& mount, stored_value_size_t value_size,
                                            stored_value_size_t value_max_size) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using collection_t = collection<key_slot_type_, typename types_t::volume_t>;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 300;

    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(value_size, value_max_size, 16ull * 1024ull * 1024ull);
    expected<collection_t> store = collection_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger);
    failures += expect(store.has_value());
    if (!store) return failures;

    stored_value_size_t const widest = store->value_max_size();
    st::basic_vector<expectation_t> model;
    failures += expect(model.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 3;
        entry.state = index % 13 == 0 ? select_result_t::erased_k : select_result_t::success_k;
        entry.bytes = entry.state != select_result_t::success_k ? 0 : value_size != 0 ? value_size : 1 + index % widest;
        entry.seed = index + widest;
        model[index] = entry;
    }

    failures += write_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});
    failures += check_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});
    failures += expect(store->flush((*stack)->channel()).succeeded());
    failures += check_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});
    failures += check_model_one_by_one(*store, (*stack)->channel(), std::span<expectation_t const> {model});
    return failures;
}

/** Every value layout the collection serves round trips through the write buffer and the tree. */
template <typename mount_type_>
std::size_t every_layout_round_trips(mount_type_ const& mount) {
    std::size_t failures = 0;
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_1_byte_k>>(mount, 0, 0);
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_4_bytes_k>>(mount, 0, 0);
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_8_bytes_k>>(mount, 0, 0);
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_16_bytes_k>>(mount, 0, 0);
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_32_bytes_k>>(mount, 0, 0);
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_64_bytes_k>>(mount, 0, 0);
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::fixed_width_k>>(mount, 512, 0);
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::variable_width_k>>(mount, 0, 4096);
    return failures;
}

#pragma endregion Round Trip Cases

#pragma region Presence Cases

/** A key nobody wrote is missing, and a key a tombstone covers is erased in both places it can live. */
template <typename mount_type_>
std::size_t misses_and_tombstones(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_collection_t = typename types_t::variable_collection_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k, 16ull * 1024ull * 1024ull);
    expected<variable_collection_t> store = variable_collection_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::basic_vector<byte_t> value;
    failures += expect(value.resize(64) == st::status_t::success_k);
    for (std::size_t index = 0; index != value.size(); ++index) value[index] = painted(5, index);

    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {10}, std::span<byte_t const> {value}, 1).succeeded());
    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {20}, std::span<byte_t const> {value}, 2).succeeded());
    failures += expect(store->flush((*stack)->channel()).succeeded());
    failures += expect(store->erase((*stack)->channel(), stored_key_t {20}, 3).succeeded());

    stored_value_size_t size = 0;
    failures += expect(store->find(stored_key_t {10}, size) == select_result_t::success_k);
    failures += expect_eq(static_cast<std::size_t>(size), value.size());

    // The tombstone is staged, so the write buffer answers before the run that still carries the value.
    failures += expect(store->find(stored_key_t {20}, size) == select_result_t::erased_k);
    failures += expect(store->find(stored_key_t {30}, size) == select_result_t::nothing_k);

    st::basic_vector<byte_t> into;
    failures += expect(into.resize(value.size()) == st::status_t::success_k);
    failures += expect_status(store->select((*stack)->channel(), stored_key_t {20}, std::span<byte_t> {into}, size),
                              status_code_t::not_found_k);
    failures += expect_status(store->select((*stack)->channel(), stored_key_t {30}, std::span<byte_t> {into}, size),
                              status_code_t::not_found_k);

    // Once the tombstone reaches the tree it must keep shadowing the older run.
    failures += expect(store->flush((*stack)->channel()).succeeded());
    failures += expect(store->find(stored_key_t {20}, size) == select_result_t::erased_k);
    failures += expect(store->find(stored_key_t {30}, size) == select_result_t::nothing_k);
    failures += expect_status(store->select((*stack)->channel(), stored_key_t {20}, std::span<byte_t> {into}, size),
                              status_code_t::not_found_k);

    std::array<stored_key_t, 3> const keys {stored_key_t {10}, stored_key_t {20}, stored_key_t {30}};
    std::array<select_result_t, 3> answers {select_result_t::nothing_k, select_result_t::nothing_k,
                                            select_result_t::nothing_k};
    auto sink = [](std::size_t, std::uint64_t, std::span<byte_t const>) noexcept {};
    failures += expect(store
                           ->select((*stack)->channel(), std::span<stored_key_t const> {keys},
                                    std::span<select_result_t> {answers}, sink)
                           .succeeded());
    failures += expect(answers[0] == select_result_t::success_k);
    failures += expect(answers[1] == select_result_t::erased_k);
    failures += expect(answers[2] == select_result_t::nothing_k);
    return failures;
}

/** The write buffer answers a value the tree has never seen, and the tree answers it after a flush. */
template <typename mount_type_>
std::size_t write_buffer_answers_before_the_tree(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_collection_t = typename types_t::variable_collection_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k, 16ull * 1024ull * 1024ull);
    expected<variable_collection_t> store = variable_collection_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::basic_vector<byte_t> older;
    st::basic_vector<byte_t> newer;
    failures += expect(older.resize(300) == st::status_t::success_k);
    failures += expect(newer.resize(700) == st::status_t::success_k);
    for (std::size_t index = 0; index != older.size(); ++index) older[index] = painted(1, index);
    for (std::size_t index = 0; index != newer.size(); ++index) newer[index] = painted(2, index);

    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {77}, std::span<byte_t const> {older}, 1).succeeded());
    failures += expect(store->flush((*stack)->channel()).succeeded());
    failures += expect_eq(store->tree().snapshot().population_count(), std::uint64_t {1});

    // Reading the run once is what puts the older value in the cache a later flush has to drop.
    st::basic_vector<byte_t> cached;
    failures += expect(cached.resize(older.size()) == st::status_t::success_k);
    stored_value_size_t cached_size = 0;
    failures += expect(
        store->select((*stack)->channel(), stored_key_t {77}, std::span<byte_t> {cached}, cached_size).succeeded());
    failures += expect_eq(static_cast<std::size_t>(cached_size), older.size());
    failures += expect_eq(std::memcmp(cached.data(), older.data(), older.size()), 0);

    // Staged over the run, the write buffer must be the one that answers.
    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {77}, std::span<byte_t const> {newer}, 2).succeeded());
    st::basic_vector<byte_t> into;
    failures += expect(into.resize(newer.size()) == st::status_t::success_k);
    stored_value_size_t size = 0;
    failures += expect(
        store->select((*stack)->channel(), stored_key_t {77}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(static_cast<std::size_t>(size), newer.size());
    failures += expect_eq(std::memcmp(into.data(), newer.data(), newer.size()), 0);

    failures += expect(store->flush((*stack)->channel()).succeeded());
    failures += expect_eq(store->staged_count(), std::size_t {0});
    size = 0;
    for (std::size_t index = 0; index != into.size(); ++index) into[index] = byte_t {0};
    failures += expect(
        store->select((*stack)->channel(), stored_key_t {77}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(static_cast<std::size_t>(size), newer.size());
    failures += expect_eq(std::memcmp(into.data(), newer.data(), newer.size()), 0);
    return failures;
}

/** A write buffer too small for the batch flushes itself, and every value survives the flushes it forced. */
template <typename mount_type_>
std::size_t full_write_buffer_flushes_itself(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_collection_t = typename types_t::variable_collection_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 500;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t config = test_config(0, value_max_size_k, 64ull * 1024ull * 1024ull);
    config.write_buffer_max_bytes = 32ull * 1024ull;
    config.write_buffer_max_elements = 64;
    expected<variable_collection_t> store = variable_collection_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::basic_vector<expectation_t> model;
    failures += expect(model.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 5 + 2;
        entry.state = select_result_t::success_k;
        entry.bytes = 120 + index % 200;
        entry.seed = index * 3 + 1;
        model[index] = entry;
    }

    failures += write_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});
    failures += expect(store->staged_count() <= config.write_buffer_max_elements);
    failures += expect(store->tree().snapshot().level(0).size() > std::size_t {1});
    failures += check_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});

    // A batch larger than the whole write buffer lands whole, past the bounds, and the next write flushes it.
    st::basic_vector<byte_t> wide;
    failures += expect(wide.resize(64ull * 1024ull) == st::status_t::success_k);
    for (std::size_t index = 0; index != wide.size(); ++index) wide[index] = painted(7, index);
    st::basic_vector<stored_key_t> keys;
    st::basic_vector<stored_value_size_t> sizes;
    failures += expect(keys.resize(2) == st::status_t::success_k);
    failures += expect(sizes.resize(2) == st::status_t::success_k);
    keys[0] = 900001;
    keys[1] = 900002;
    sizes[0] = static_cast<stored_value_size_t>(wide.size() / 2);
    sizes[1] = static_cast<stored_value_size_t>(wide.size() / 2);
    failures += expect(store
                           ->insert((*stack)->channel(), std::span<stored_key_t const> {keys},
                                    std::span<byte_t const> {wide}, std::span<stored_value_size_t const> {sizes},
                                    store->newest_stamp() + 1)
                           .succeeded());
    failures += expect_eq(store->staged_count(), std::size_t {2});
    st::basic_vector<byte_t> one;
    failures += expect(one.resize(8) == st::status_t::success_k);
    failures += expect(store
                           ->insert((*stack)->channel(), stored_key_t {900003}, std::span<byte_t const> {one},
                                    store->newest_stamp() + 1)
                           .succeeded());
    failures += expect_eq(store->staged_count(), std::size_t {1});
    stored_value_size_t size = 0;
    failures += expect(store->find(stored_key_t {900002}, size) == select_result_t::success_k);
    failures += expect_eq(static_cast<std::size_t>(size), wide.size() / 2);
    return failures;
}

#pragma endregion Presence Cases

#pragma region Buffer Cases

/** A caller's span is taken at the value's own size and at any address, aligned or not. */
template <typename mount_type_>
std::size_t unaligned_exact_span_is_taken(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_collection_t = typename types_t::variable_collection_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k, 16ull * 1024ull * 1024ull);
    expected<variable_collection_t> store = variable_collection_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger);
    failures += expect(store.has_value());
    if (!store) return failures;

    constexpr std::size_t bytes_k = 5001;
    st::basic_vector<byte_t> value;
    failures += expect(value.resize(bytes_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != bytes_k; ++index) value[index] = painted(9, index);
    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {3}, std::span<byte_t const> {value}, 1).succeeded());
    failures += expect(store->flush((*stack)->channel()).succeeded());

    // One byte of slack in front makes the span odd, and it is exactly as long as the value.
    st::basic_vector<byte_t> room;
    failures += expect(room.resize(bytes_k + 1) == st::status_t::success_k);
    for (std::size_t index = 0; index != room.size(); ++index) room[index] = byte_t {0};
    std::span<byte_t> const unaligned {room.data() + 1, bytes_k};
    failures += expect(reinterpret_cast<std::uintptr_t>(unaligned.data()) % buffer_alignment_k != 0);

    stored_value_size_t size = 0;
    failures += expect(store->select((*stack)->channel(), stored_key_t {3}, unaligned, size).succeeded());
    failures += expect_eq(static_cast<std::size_t>(size), bytes_k);
    failures += expect_eq(std::memcmp(unaligned.data(), value.data(), bytes_k), 0);
    failures += expect(room[0] == byte_t {0});

    // A span one byte shorter than the value is refused rather than half filled.
    failures += expect(store->select((*stack)->channel(), stored_key_t {3}, unaligned.first(bytes_k - 1), size) ==
                       status_code_t::invalid_argument_k);
    return failures;
}

/** The widest value a layout admits and the empty value both survive a round trip. */
template <typename mount_type_>
std::size_t widest_and_empty_values(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_collection_t = typename types_t::variable_collection_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k, 16ull * 1024ull * 1024ull);
    expected<variable_collection_t> store = variable_collection_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger);
    failures += expect(store.has_value());
    if (!store) return failures;
    failures += expect_eq(store->value_max_size(), value_max_size_k);

    st::basic_vector<byte_t> widest;
    failures += expect(widest.resize(value_max_size_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != widest.size(); ++index) widest[index] = painted(4, index);
    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {1}, std::span<byte_t const> {widest}, 1).succeeded());
    failures += expect(store->insert((*stack)->channel(), stored_key_t {2}, std::span<byte_t const> {}, 2).succeeded());

    // Seven bytes ahead of it leave the next staged value on no page boundary, which is what makes an
    // empty value located by an offset rather than by its sentinel cost a read.
    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {6}, std::span<byte_t const> {widest.data(), 7}, 3)
            .succeeded());
    failures += expect(store->insert((*stack)->channel(), stored_key_t {5}, std::span<byte_t const> {}, 4).succeeded());

    // One byte past the widest value is refused, whatever the write buffer still has room for.
    st::basic_vector<byte_t> wider;
    failures += expect(wider.resize(value_max_size_k + 1) == st::status_t::success_k);
    failures += expect(store->insert((*stack)->channel(), stored_key_t {4}, std::span<byte_t const> {wider}, 5) ==
                       status_code_t::malformed_k);

    st::basic_vector<byte_t> into;
    failures += expect(into.resize(value_max_size_k) == st::status_t::success_k);
    for (std::size_t round = 0; round != 2; ++round) {
        stored_value_size_t size = 0;
        failures += expect(
            store->select((*stack)->channel(), stored_key_t {1}, std::span<byte_t> {into}, size).succeeded());
        failures += expect_eq(static_cast<std::size_t>(size), widest.size());
        failures += expect_eq(std::memcmp(into.data(), widest.data(), widest.size()), 0);

        size = value_max_size_k;
        failures += expect(
            store->select((*stack)->channel(), stored_key_t {2}, std::span<byte_t> {}, size).succeeded());
        failures += expect_eq(static_cast<std::size_t>(size), std::size_t {0});
        failures += expect(store->find(stored_key_t {2}, size) == select_result_t::success_k);
        failures += expect_eq(static_cast<std::size_t>(size), std::size_t {0});
        if (round == 0) failures += expect(store->flush((*stack)->channel()).succeeded());
    }

    // Key five was never read one at a time, so nothing of it sits in the value cache and the batch
    // reaches the run. An empty value is located by a sentinel rather than an offset, so reading it
    // must take no block at all rather than a page nobody wanted.
    std::array<stored_key_t, 1> const untouched {stored_key_t {5}};
    std::array<select_result_t, 1> answers {select_result_t::nothing_k};
    std::uint32_t idlest = collection_config_k.request_buffers;
    std::size_t carried = 0;
    auto watching = [&](std::size_t, std::uint64_t, std::span<byte_t const> bytes) noexcept {
        std::uint32_t const free_blocks = (*stack)->channel().requests().available_buffers();
        if (free_blocks < idlest) idlest = free_blocks;
        carried += bytes.size();
    };
    failures += expect(store
                           ->select((*stack)->channel(), std::span<stored_key_t const> {untouched},
                                    std::span<select_result_t> {answers}, watching)
                           .succeeded());
    failures += expect(answers[0] == select_result_t::success_k);
    failures += expect_eq(carried, std::size_t {0});
    failures += expect_eq(idlest, collection_config_k.request_buffers);
    return failures;
}

/** The value cache holds what it was given and evicts the oldest, rather than quietly holding nothing. */
std::size_t value_cache_holds_what_it_took() {
    std::size_t failures = 0;
    using cache_t = value_cache<stored_key_t, value_layout_t::variable_width_k>;
    constexpr stored_value_size_t width_k = 300;
    resource_constraint_t budget {std::size_t {8} * 1024 * 1024};

    // A budget that affords entries must hold entries, and the eviction ring only takes a power of two.
    cache_t cache = cache_t::of(1024ull * 1024ull, width_k, budget);
    failures += expect(cache.capacity() != 0);
    failures += expect((cache.capacity() & (cache.capacity() - 1)) == 0);
    if (cache.capacity() == 0) return failures;

    st::basic_vector<byte_t> value;
    failures += expect(value.resize(width_k) == st::status_t::success_k);
    std::size_t const held = cache.capacity();
    for (std::size_t index = 0; index != held + 1; ++index) {
        for (std::size_t position = 0; position != width_k; ++position) value[position] = painted(index, position);
        cache.add(static_cast<stored_key_t>(index), std::span<byte_t const> {value});
    }
    failures += expect_eq(cache.size(), held);

    std::size_t mismatches = 0;
    auto check = [&](std::size_t index) noexcept {
        return cache.visit(static_cast<stored_key_t>(index), [&](std::span<byte_t const> bytes) noexcept {
            if (bytes.size() != width_k) ++mismatches;
            for (std::size_t position = 0; position != bytes.size(); ++position)
                if (bytes[position] != painted(index, position)) ++mismatches;
        });
    };
    failures += expect(!check(0));
    for (std::size_t index = 1; index != held + 1; ++index) failures += expect(check(index));
    failures += expect_eq(mismatches, std::size_t {0});

    // Writing a key the cache already holds replaces its bytes instead of taking another slot.
    for (std::size_t position = 0; position != width_k; ++position) value[position] = painted(9999, position);
    cache.add(static_cast<stored_key_t>(held), std::span<byte_t const> {value});
    failures += expect_eq(cache.size(), held);
    std::size_t replaced = 0;
    failures += expect(cache.visit(static_cast<stored_key_t>(held), [&](std::span<byte_t const> bytes) noexcept {
        for (std::size_t position = 0; position != bytes.size(); ++position)
            if (bytes[position] == painted(9999, position)) ++replaced;
    }));
    failures += expect_eq(replaced, std::size_t {width_k});

    cache.clear();
    failures += expect_eq(cache.size(), std::size_t {0});

    // A budget too small for one entry, and a layout of no width, both hold nothing on purpose.
    failures += expect_eq(cache_t::of(0, width_k, budget).capacity(), std::size_t {0});
    failures += expect_eq(cache_t::of(1024ull * 1024ull, 0, budget).capacity(), std::size_t {0});
    return failures;
}

#pragma endregion Buffer Cases

#pragma region Depth Cases

/**
 *  @brief A batched select of many keys across several runs sits at the queue pair's depth through the middle.
 *
 *  The keys are spread over four level-zero runs and never read one at a time, so the value cache is
 *  empty and every key of the batch reaches the tree.
 */
template <typename mount_type_>
std::size_t batched_select_holds_its_depth(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_collection_t = typename types_t::variable_collection_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 1024;
    constexpr std::size_t flushes_k = 4;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k, 64ull * 1024ull * 1024ull);
    expected<variable_collection_t> store = variable_collection_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::basic_vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index);
        entry.state = select_result_t::success_k;
        entry.bytes = 1 + index % 4093;
        entry.seed = index + 7;
        model[index] = entry;
    }
    for (std::size_t flush = 0; flush != flushes_k; ++flush) {
        st::basic_vector<expectation_t> batch;
        for (std::size_t index = flush; index < keys_k; index += flushes_k)
            failures += expect(batch.push_back(expectation_t {model[index]}) == st::status_t::success_k);
        failures += write_model(*store, (*stack)->channel(), std::span<expectation_t const> {batch});
        failures += expect(store->flush((*stack)->channel()).succeeded());
        if (failures != 0) return failures;
    }
    failures += expect_eq(store->tree().snapshot().level(0).size(), flushes_k);

    st::basic_vector<stored_key_t> keys;
    st::basic_vector<select_result_t> results;
    failures += expect(keys.resize(keys_k) == st::status_t::success_k);
    failures += expect(results.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) keys[index] = model[index].stored_key;

    // The blocks a batched read holds are counted in the pool, since the ring performs a channel's
    // requests inside the poll that hands them to the sink.
    std::uint32_t idlest = collection_config_k.request_buffers;
    std::size_t mismatches = 0;
    auto counting = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        std::uint32_t const free_blocks = (*stack)->channel().requests().available_buffers();
        if (free_blocks < idlest) idlest = free_blocks;
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(model[request].seed, offset + index)) ++mismatches;
    };
    failures += expect(store
                           ->select((*stack)->channel(), std::span<stored_key_t const> {keys},
                                    std::span<select_result_t> {results}, counting)
                           .succeeded());
    failures += expect_eq(mismatches, std::size_t {0});
    failures += expect_eq(collection_config_k.request_buffers - idlest, collection_config_k.queue_depth);
    for (std::size_t index = 0; index != keys_k; ++index)
        failures += expect(results[index] == select_result_t::success_k);
    return failures;
}

#pragma endregion Depth Cases

#pragma region Snapshot Cases

/**
 *  @brief A snapshot keeps answering the level set it was taken from while another thread publishes over it.
 *
 *  The writer blocks in its own publication until the snapshot is let go, which is what makes the read
 *  in the middle meaningful: the level set the snapshot holds cannot have been retired under it.
 */
template <typename mount_type_>
std::size_t snapshot_holds_its_moment(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using guard_t = typename types_t::guard_t;
    using variable_collection_t = typename types_t::variable_collection_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t bytes_k = 900;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k, 64ull * 1024ull * 1024ull);
    expected<variable_collection_t> store = variable_collection_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::basic_vector<byte_t> older;
    st::basic_vector<byte_t> newer;
    failures += expect(older.resize(bytes_k) == st::status_t::success_k);
    failures += expect(newer.resize(bytes_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != bytes_k; ++index) older[index] = painted(11, index);
    for (std::size_t index = 0; index != bytes_k; ++index) newer[index] = painted(12, index);

    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {42}, std::span<byte_t const> {older}, 1).succeeded());
    expected<typename variable_collection_t::snapshot_t> taken = store->take_snapshot((*stack)->channel());
    failures += expect(taken.has_value());
    if (!taken) return failures;

    std::size_t writer_failures = 0;
    std::thread writer {[&]() noexcept {
        expected<guard_t> guard = (*stack)->fixture->channels.acquire();
        writer_failures += expect(guard.has_value());
        if (!guard) return;
        writer_failures += expect(
            store->insert(guard->channel(), stored_key_t {42}, std::span<byte_t const> {newer}, 2).succeeded());
        writer_failures += expect(store->flush(guard->channel()).succeeded());
    }};

    {
        st::basic_vector<byte_t> into;
        failures += expect(into.resize(bytes_k) == st::status_t::success_k);
        stored_value_size_t size = 0;
        failures += expect(
            taken->select((*stack)->channel(), stored_key_t {42}, std::span<byte_t> {into}, size).succeeded());
        failures += expect_eq(static_cast<std::size_t>(size), bytes_k);
        failures += expect_eq(std::memcmp(into.data(), older.data(), bytes_k), 0);
        failures += expect_eq(taken->population_count(), std::uint64_t {1});
    }

    // Letting the hold go is what lets the writer's publication retire the set it replaced.
    taken = expected<typename variable_collection_t::snapshot_t> {status_t {status_code_t::uninitialized_k, "Let go"}};
    writer.join();
    failures += writer_failures;

    st::basic_vector<byte_t> into;
    failures += expect(into.resize(bytes_k) == st::status_t::success_k);
    stored_value_size_t size = 0;
    failures += expect(
        store->select((*stack)->channel(), stored_key_t {42}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(static_cast<std::size_t>(size), bytes_k);
    failures += expect_eq(std::memcmp(into.data(), newer.data(), bytes_k), 0);
    return failures;
}

#pragma endregion Snapshot Cases

#pragma region Scan Cases

/** A scan visits every key once, in ascending order, across the write buffer and two levels. */
template <typename mount_type_>
std::size_t scan_visits_every_key_in_order(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using guard_t = typename types_t::guard_t;
    using variable_collection_t = typename types_t::variable_collection_t;
    using variable_scanner_t = typename types_t::variable_scanner_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 1000;
    constexpr std::size_t rounds_k = 4;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k, 64ull * 1024ull);
    expected<variable_collection_t> store = variable_collection_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::basic_vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) model[index].stored_key = static_cast<stored_key_t>(index);

    auto apply = [&](std::span<expectation_t const> batch) noexcept {
        for (std::size_t index = 0; index != batch.size(); ++index)
            model[static_cast<std::size_t>(batch[index].stored_key)] = batch[index];
    };

    for (std::size_t round = 0; round != rounds_k; ++round) {
        st::basic_vector<expectation_t> batch;
        for (std::size_t index = round; index < keys_k; index += rounds_k) {
            expectation_t entry;
            entry.stored_key = static_cast<stored_key_t>(index);
            entry.state = select_result_t::success_k;
            entry.bytes = 40 + index % 97;
            entry.seed = index * 31 + round;
            failures += expect(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
        }
        failures += write_model(*store, (*stack)->channel(), std::span<expectation_t const> {batch});
        failures += expect(store->flush((*stack)->channel()).succeeded());
        apply(std::span<expectation_t const> {batch});
        if (failures != 0) return failures;
    }

    // A merge needs a channel of its own to write on, since both pipelines poll into the same room.
    expected<guard_t> writing = (*stack)->fixture->channels.acquire();
    failures += expect(writing.has_value());
    if (!writing) return failures;
    expected<compaction_outcome_t> const merged = store->compact((*stack)->channel(), writing->channel());
    failures += expect(merged.has_value());
    if (!merged) return failures;
    failures += expect(*merged == compaction_outcome_t::published_k);
    failures += expect_eq(store->tree().snapshot().size(), std::size_t {2});

    // Two more flushes put runs back on level zero, and they carry the same keys, so the scan has to
    // pick the younger of two runs of one level as well as the shallower of two levels.
    for (std::size_t again = 0; again != 2; ++again) {
        st::basic_vector<expectation_t> batch;
        for (std::size_t index = 0; index < keys_k; index += 10) {
            expectation_t entry;
            entry.stored_key = static_cast<stored_key_t>(index);
            entry.state = select_result_t::success_k;
            entry.bytes = 7 + (index + again * 11) % 53;
            entry.seed = index * 17 + 5 + again;
            failures += expect(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
        }
        failures += write_model(*store, (*stack)->channel(), std::span<expectation_t const> {batch});
        failures += expect(store->flush((*stack)->channel()).succeeded());
        apply(std::span<expectation_t const> {batch});
    }
    failures += expect_eq(store->tree().snapshot().level(0).size(), std::size_t {2});

    // What stays staged is what the scan must merge in front of every run.
    {
        st::basic_vector<expectation_t> batch;
        for (std::size_t index = 0; index < keys_k; index += 25) {
            expectation_t entry;
            entry.stored_key = static_cast<stored_key_t>(index);
            entry.state = index % 50 == 0 ? select_result_t::erased_k : select_result_t::success_k;
            entry.bytes = entry.state == select_result_t::success_k ? 13 + index % 29 : 0;
            entry.seed = index * 7 + 3;
            failures += expect(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
        }
        failures += write_model(*store, (*stack)->channel(), std::span<expectation_t const> {batch});
        apply(std::span<expectation_t const> {batch});
    }
    failures += expect(store->staged_count() != 0);

    expected<variable_scanner_t> scan = variable_scanner_t::over(*store);
    failures += expect(scan.has_value());
    if (!scan) return failures;

    // The walk is bounded so a merge that forgets to advance a cursor is a result and not a hang.
    constexpr std::size_t visits_limit_k = keys_k + 64;
    std::size_t visited = 0;
    std::size_t out_of_order = 0;
    std::size_t wrong_state = 0;
    std::size_t wrong_size = 0;
    stored_key_t previous = 0;
    while (!scan->reached_end() && visited != visits_limit_k) {
        if (visited != 0 && !(previous < scan->stored_key())) ++out_of_order;
        previous = scan->stored_key();
        expectation_t const& wanted = model[static_cast<std::size_t>(previous)];
        if (scan->state() != wanted.state) ++wrong_state;
        if (static_cast<std::uint64_t>(scan->size()) != wanted.bytes) ++wrong_size;
        ++visited;
        scan->advance();
    }
    failures += expect_eq(visited, keys_k);
    failures += expect_eq(out_of_order, std::size_t {0});
    failures += expect_eq(wrong_state, std::size_t {0});
    failures += expect_eq(wrong_size, std::size_t {0});

    // The newest version of every key must also be the one a read of it brings back.
    failures += check_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});
    return failures;
}

#pragma endregion Scan Cases

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

    report(totals, "collection.value_cache", value_cache_holds_what_it_took);
    auto const cases = [&](auto const& mount) {
        report(totals, mount, "mixed_widths_round_trip", mixed_widths_round_trip);
        report(totals, mount, "every_layout", every_layout_round_trips);
        report(totals, mount, "misses_and_tombstones", misses_and_tombstones);
        report(totals, mount, "buffer_before_tree", write_buffer_answers_before_the_tree);
        report(totals, mount, "full_buffer_flushes", full_write_buffer_flushes_itself);
        report(totals, mount, "unaligned_exact_span", unaligned_exact_span_is_taken);
        report(totals, mount, "widest_and_empty", widest_and_empty_values);
        report(totals, mount, "holds_its_depth", batched_select_holds_its_depth);
        report(totals, mount, "snapshot_holds_its_moment", snapshot_holds_its_moment);
        report(totals, mount, "scan_in_order", scan_visits_every_key_in_order);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, collection_config_k, 1, "collection", cases);

    return suite_exit_code(totals);
}
