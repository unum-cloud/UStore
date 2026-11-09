/**
 *  @file test/nvme/collection.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The store of one collection over the ring: what the memory runs answer, what the tree answers, and how deep
 *      it reads.
 *
 *  The results this file exists for are @c holds_its_depth and @c snapshot_holds_its_moment. A batched
 *  select must sit at the queue pair's depth through the middle of a run rather than at the size of whatever
 *  batch a caller handed it, and a pinned stamp must keep answering the versions it pinned while another
 *  thread commits and flushes over them.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcmp`

#include <algorithm> // `std::min`
#include <array>     // `std::array`
#include <map>       // `std::map`
#include <memory>    // `std::unique_ptr`
#include <new>       // `std::nothrow`
#include <optional>  // `std::optional`
#include <span>      // `std::span`
#include <thread>    // `std::thread`
#include <utility>   // `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_store.hpp"    // `nvme_store`, `nvme_store_cursor`
#include "ustore/blobs/nvme_key_slot.hpp" // `key_slot`

#include "nvme/fixture.hpp" // `run_on_transport`, `expectation_t`, `painted`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

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

/** The stores and the cursors every case drives over the volume of @p fixture_type_. */
template <typename fixture_type_>
struct collection_types {
    using fixture_t = fixture_type_;
    using guard_t = typename fixture_t::guard_t;
    using volume_t = typename fixture_t::volume_t;
    using variable_store_t =
        nvme_store<variable_slot_t, volume_t, atomic_reference, waiter_t, last_write_fold_t, no_device_fold_t>;
    using variable_cursor_t =
        nvme_store_cursor<variable_slot_t, volume_t, atomic_reference, waiter_t, last_write_fold_t, no_device_fold_t>;

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
 */
[[nodiscard]] collection_config_t test_config(stored_value_size_t value_size,
                                              stored_value_size_t value_max_size) noexcept {
    collection_config_t config;
    config.concurrency_limit = 1;
    config.cache_limit = 1024ull * 1024ull;
    config.memory_runs_max_bytes = 8ull * 1024ull * 1024ull;
    config.memory_runs_max_deltas = 8192;
    config.lsm_tree.level_enlarge_factor = 4;
    config.lsm_tree.lsm_level_0.sorted_run.value_size = value_size;
    config.lsm_tree.lsm_level_0.sorted_run.value_max_size = value_max_size;
    return config;
}

#pragma endregion Shape

#pragma region Fixtures

/** Run records a case's pool holds, far more than any case keeps at once. */
constexpr std::size_t run_records_k = 4096;

/** A run pool for stores of @p store_type_ charged to @p stack's budget, which a case declares
 *  before its store. */
template <typename store_type_, typename stack_type_>
expected<typename store_type_::run_pool_t> make_run_pool(stack_type_& stack) noexcept {
    return store_type_::run_pool_t::make(run_records_k, stack.fixture->resources->memory_constraint);
}

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

/** Writes every entry of @p wanted into @p store, one commit per key. */
template <typename collection_type_>
[[nodiscard]] std::size_t write_model(collection_type_& store, io_channel_t& channel,
                                      std::span<expectation_t const> wanted) {
    std::size_t failures = 0;
    st::vector<byte_t> value;
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        expectation_t const& entry = wanted[index];
        if (entry.state == lookup_result_t::removed_k) {
            failures += expect(store.remove(channel, entry.stored_key).succeeded());
            continue;
        }
        failures += expect(value.resize(static_cast<std::size_t>(entry.bytes)) == st::status_t::success_k);
        paint_into(entry, std::span<byte_t> {value});
        failures += expect(store.insert(channel, entry.stored_key, std::span<byte_t const> {value}).succeeded());
    }
    return failures;
}

/** Reads every key of @p wanted through one batched select and counts what the model disagrees with. */
template <typename readable_type_>
[[nodiscard]] std::size_t check_model(readable_type_ const& source, io_channel_t& channel,
                                      std::span<expectation_t const> wanted) {
    std::size_t failures = 0;
    std::size_t const count = wanted.size();
    st::vector<stored_key_t> keys;
    st::vector<lookup_result_t> results;
    st::vector<std::uint64_t> seen;
    failures += expect(keys.resize(count) == st::status_t::success_k);
    failures += expect(results.resize(count) == st::status_t::success_k);
    failures += expect(seen.resize(count) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count; ++index) {
        keys[index] = wanted[index].stored_key;
        results[index] = lookup_result_t::missing_k;
        seen[index] = 0;
    }

    std::size_t mismatches = 0;
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        seen[request] += bytes.size();
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(wanted[request].seed, offset + index)) ++mismatches;
    };
    failures += expect(
        source.select(channel, std::span<stored_key_t const> {keys}, std::span<lookup_result_t> {results}, sink)
            .succeeded());
    failures += expect_eq(mismatches, std::size_t {0});

    for (std::size_t index = 0; index != count; ++index) {
        failures += expect(results[index] == wanted[index].state);
        if (wanted[index].state == lookup_result_t::written_k) failures += expect_eq(seen[index], wanted[index].bytes);
    }
    return failures;
}

/** Reads every key of @p wanted one at a time, which is the path that fills the value cache. */
template <typename readable_type_>
[[nodiscard]] std::size_t check_model_one_by_one(readable_type_& source, io_channel_t& channel,
                                                 std::span<expectation_t const> wanted) {
    std::size_t failures = 0;
    st::vector<byte_t> into;
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        expectation_t const& entry = wanted[index];
        stored_value_size_t size = 0;
        if (entry.state != lookup_result_t::written_k) {
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

/** Values of many widths survive the memory runs, the flush and the read back, byte for byte. */
template <typename mount_type_>
std::size_t mixed_widths_round_trip(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 700;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_store_t::run_pool_t> runs = make_run_pool<variable_store_t>(**stack);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<variable_store_t> store = variable_store_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger, *runs);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::vector<expectation_t> model;
    failures += expect(model.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 2 + 1;
        entry.state = index % 11 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
        entry.bytes = entry.state == lookup_result_t::written_k ? 1 + index % 4093 : 0;
        entry.seed = index;
        model[index] = entry;
    }

    failures += write_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});

    // Nothing is flushed yet, so the memory runs are what answers.
    failures += expect_eq(store->memory_deltas_count(), count_k);
    failures += check_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});

    failures += expect(store->flush((*stack)->channel()).succeeded());
    failures += expect_eq(store->memory_deltas_count(), std::size_t {0});
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
    using nvme_store_t = nvme_store<key_slot_type_, typename types_t::volume_t, atomic_reference, waiter_t,
                                    last_write_fold_t, no_device_fold_t>;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 300;

    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(value_size, value_max_size);
    expected<typename nvme_store_t::run_pool_t> runs = make_run_pool<nvme_store_t>(**stack);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<nvme_store_t> store = nvme_store_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger, *runs);
    failures += expect(store.has_value());
    if (!store) return failures;

    stored_value_size_t const widest = store->value_max_size();
    st::vector<expectation_t> model;
    failures += expect(model.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 3;
        entry.state = index % 13 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
        entry.bytes = entry.state != lookup_result_t::written_k ? 0 : value_size != 0 ? value_size : 1 + index % widest;
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

/** Every value layout the collection serves round trips through the memory runs and the tree. */
template <typename mount_type_>
std::size_t every_layout_round_trips(mount_type_ const& mount) {
    std::size_t failures = 0;
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_12_bytes_k>>(mount, 0, 0);
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_44_bytes_k>>(mount, 0, 0);
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::fixed_width_k>>(mount, 512, 0);
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::variable_width_k>>(mount, 0, 4096);
    return failures;
}

#pragma endregion Round Trip Cases

#pragma region Presence Cases

/** A key nobody wrote is missing, and a key a remove delta covers is removed in both places it can live. */
template <typename mount_type_>
std::size_t misses_and_remove_deltas(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_store_t::run_pool_t> runs = make_run_pool<variable_store_t>(**stack);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<variable_store_t> store = variable_store_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger, *runs);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::vector<byte_t> value;
    failures += expect(value.resize(64) == st::status_t::success_k);
    for (std::size_t index = 0; index != value.size(); ++index) value[index] = painted(5, index);

    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {10}, std::span<byte_t const> {value}).succeeded());
    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {20}, std::span<byte_t const> {value}).succeeded());
    failures += expect(store->flush((*stack)->channel()).succeeded());
    failures += expect(store->remove((*stack)->channel(), stored_key_t {20}).succeeded());

    stored_value_size_t size = 0;
    failures += expect(store->find(stored_key_t {10}, size) == lookup_result_t::written_k);
    failures += expect_eq(static_cast<std::size_t>(size), value.size());

    // The removal sits in a memory run, which answers before the run that still carries the value.
    failures += expect(store->find(stored_key_t {20}, size) == lookup_result_t::removed_k);
    failures += expect(store->find(stored_key_t {30}, size) == lookup_result_t::missing_k);

    st::vector<byte_t> into;
    failures += expect(into.resize(value.size()) == st::status_t::success_k);
    failures += expect_status(store->select((*stack)->channel(), stored_key_t {20}, std::span<byte_t> {into}, size),
                              status_code_t::not_found_k);
    failures += expect_status(store->select((*stack)->channel(), stored_key_t {30}, std::span<byte_t> {into}, size),
                              status_code_t::not_found_k);

    // Once the remove delta reaches the tree it must keep shadowing the older run.
    failures += expect(store->flush((*stack)->channel()).succeeded());
    failures += expect(store->find(stored_key_t {20}, size) == lookup_result_t::removed_k);
    failures += expect(store->find(stored_key_t {30}, size) == lookup_result_t::missing_k);
    failures += expect_status(store->select((*stack)->channel(), stored_key_t {20}, std::span<byte_t> {into}, size),
                              status_code_t::not_found_k);

    std::array<stored_key_t, 3> const keys {stored_key_t {10}, stored_key_t {20}, stored_key_t {30}};
    std::array<lookup_result_t, 3> answers {lookup_result_t::missing_k, lookup_result_t::missing_k,
                                            lookup_result_t::missing_k};
    auto sink = [](std::size_t, std::uint64_t, std::span<byte_t const>) noexcept {};
    failures += expect(store
                           ->select((*stack)->channel(), std::span<stored_key_t const> {keys},
                                    std::span<lookup_result_t> {answers}, sink)
                           .succeeded());
    failures += expect(answers[0] == lookup_result_t::written_k);
    failures += expect(answers[1] == lookup_result_t::removed_k);
    failures += expect(answers[2] == lookup_result_t::missing_k);
    return failures;
}

/** A memory run answers a value the tree has never seen, and the tree answers it after a flush. */
template <typename mount_type_>
std::size_t memory_runs_answer_before_the_tree(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_store_t::run_pool_t> runs = make_run_pool<variable_store_t>(**stack);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<variable_store_t> store = variable_store_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger, *runs);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::vector<byte_t> older;
    st::vector<byte_t> newer;
    failures += expect(older.resize(300) == st::status_t::success_k);
    failures += expect(newer.resize(700) == st::status_t::success_k);
    for (std::size_t index = 0; index != older.size(); ++index) older[index] = painted(1, index);
    for (std::size_t index = 0; index != newer.size(); ++index) newer[index] = painted(2, index);

    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {77}, std::span<byte_t const> {older}).succeeded());
    failures += expect(store->flush((*stack)->channel()).succeeded());
    failures += expect_eq(store->tree().levels().population_count(), std::uint64_t {1});

    // Reading the run once is what puts the older value in the cache a later flush has to drop.
    st::vector<byte_t> cached;
    failures += expect(cached.resize(older.size()) == st::status_t::success_k);
    stored_value_size_t cached_size = 0;
    failures += expect(
        store->select((*stack)->channel(), stored_key_t {77}, std::span<byte_t> {cached}, cached_size).succeeded());
    failures += expect_eq(static_cast<std::size_t>(cached_size), older.size());
    failures += expect_eq(std::memcmp(cached.data(), older.data(), older.size()), 0);

    // Committed over the run, the memory run must be the one that answers.
    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {77}, std::span<byte_t const> {newer}).succeeded());
    st::vector<byte_t> into;
    failures += expect(into.resize(newer.size()) == st::status_t::success_k);
    stored_value_size_t size = 0;
    failures += expect(
        store->select((*stack)->channel(), stored_key_t {77}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(static_cast<std::size_t>(size), newer.size());
    failures += expect_eq(std::memcmp(into.data(), newer.data(), newer.size()), 0);

    failures += expect(store->flush((*stack)->channel()).succeeded());
    failures += expect_eq(store->memory_deltas_count(), std::size_t {0});
    size = 0;
    for (std::size_t index = 0; index != into.size(); ++index) into[index] = byte_t {0};
    failures += expect(
        store->select((*stack)->channel(), stored_key_t {77}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(static_cast<std::size_t>(size), newer.size());
    failures += expect_eq(std::memcmp(into.data(), newer.data(), newer.size()), 0);
    return failures;
}

/** A burst of one-delta commits many times the memory-run records the collection holds
 *  stalls on the records, merges by tier and lands every commit, keeping no more runs than a
 *  merged view holds. */
template <typename mount_type_>
std::size_t burst_past_records_lands_every_commit(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 2000;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    // Bounds of deltas and bytes no burst below reaches, so only the records stall it: (4 − 1)(7 +
    // 1) runs at most.
    collection_config_t const config = test_config(0, value_max_size_k);
    std::size_t const merged_runs_max = (config.lsm_tree.level_enlarge_factor - 1) * (7 + 1);
    expected<typename variable_store_t::run_pool_t> runs = make_run_pool<variable_store_t>(**stack);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<variable_store_t> store = variable_store_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger, *runs);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::vector<expectation_t> model;
    failures += expect(model.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    std::size_t widest_shape = 0;
    st::vector<byte_t> value;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 3 + 1;
        entry.state = lookup_result_t::written_k;
        entry.bytes = 1 + index % 16;
        entry.seed = index + 11;
        model[index] = entry;
        failures += expect(value.resize(static_cast<std::size_t>(entry.bytes)) == st::status_t::success_k);
        paint_into(entry, std::span<byte_t> {value});
        failures += expect(
            store->insert((*stack)->channel(), entry.stored_key, std::span<byte_t const> {value}).succeeded());
        widest_shape = std::max(widest_shape, store->memory_shape().runs);
    }
    failures += expect(widest_shape <= merged_runs_max);
    failures += expect_eq(store->memory_deltas_count(), count_k);
    failures += check_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});
    return failures;
}

/** Memory runs too small for the batch flush themselves, and every value survives the flushes it forced. */
template <typename mount_type_>
std::size_t full_memory_runs_flush_themselves(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 500;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t config = test_config(0, value_max_size_k);
    config.memory_runs_max_bytes = 32ull * 1024ull;
    config.memory_runs_max_deltas = 64;
    expected<typename variable_store_t::run_pool_t> runs = make_run_pool<variable_store_t>(**stack);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<variable_store_t> store = variable_store_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger, *runs);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::vector<expectation_t> model;
    failures += expect(model.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 5 + 2;
        entry.state = lookup_result_t::written_k;
        entry.bytes = 120 + index % 200;
        entry.seed = index * 3 + 1;
        model[index] = entry;
    }

    failures += write_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});
    failures += expect(store->memory_deltas_count() <= config.memory_runs_max_deltas);
    failures += expect(store->tree().levels().level(0).size() > std::size_t {1});
    failures += check_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});

    // A batch larger than the bounds of the memory runs lands whole, past the bounds, and the next write flushes it.
    st::vector<byte_t> wide;
    failures += expect(wide.resize(64ull * 1024ull) == st::status_t::success_k);
    for (std::size_t index = 0; index != wide.size(); ++index) wide[index] = painted(7, index);
    st::vector<stored_key_t> keys;
    st::vector<stored_value_size_t> sizes;
    failures += expect(keys.resize(2) == st::status_t::success_k);
    failures += expect(sizes.resize(2) == st::status_t::success_k);
    keys[0] = 900001;
    keys[1] = 900002;
    sizes[0] = static_cast<stored_value_size_t>(wide.size() / 2);
    sizes[1] = static_cast<stored_value_size_t>(wide.size() / 2);
    failures += expect(store
                           ->insert((*stack)->channel(), std::span<stored_key_t const> {keys},
                                    std::span<byte_t const> {wide}, std::span<stored_value_size_t const> {sizes})
                           .succeeded());
    failures += expect_eq(store->memory_deltas_count(), std::size_t {2});
    st::vector<byte_t> short_value;
    failures += expect(short_value.resize(8) == st::status_t::success_k);
    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {900003}, std::span<byte_t const> {short_value}).succeeded());
    failures += expect_eq(store->memory_deltas_count(), std::size_t {1});
    stored_value_size_t size = 0;
    failures += expect(store->find(stored_key_t {900002}, size) == lookup_result_t::written_k);
    failures += expect_eq(static_cast<std::size_t>(size), wide.size() / 2);
    return failures;
}

#pragma endregion Presence Cases

#pragma region Buffer Cases

/** A caller's span is taken at the value's own size and at any address, aligned or not. */
template <typename mount_type_>
std::size_t unaligned_exact_span_is_taken(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_store_t::run_pool_t> runs = make_run_pool<variable_store_t>(**stack);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<variable_store_t> store = variable_store_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger, *runs);
    failures += expect(store.has_value());
    if (!store) return failures;

    constexpr std::size_t bytes_k = 5001;
    st::vector<byte_t> value;
    failures += expect(value.resize(bytes_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != bytes_k; ++index) value[index] = painted(9, index);
    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {3}, std::span<byte_t const> {value}).succeeded());
    failures += expect(store->flush((*stack)->channel()).succeeded());

    // One byte of slack in front makes the span odd, and it is exactly as long as the value.
    st::vector<byte_t> room;
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
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_store_t::run_pool_t> runs = make_run_pool<variable_store_t>(**stack);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<variable_store_t> store = variable_store_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger, *runs);
    failures += expect(store.has_value());
    if (!store) return failures;
    failures += expect_eq(store->value_max_size(), value_max_size_k);

    st::vector<byte_t> widest;
    failures += expect(widest.resize(value_max_size_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != widest.size(); ++index) widest[index] = painted(4, index);
    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {1}, std::span<byte_t const> {widest}).succeeded());
    failures += expect(store->insert((*stack)->channel(), stored_key_t {2}, std::span<byte_t const> {}).succeeded());

    // Seven bytes ahead of it leave the next flushed value on no page boundary, which is what would make an
    // empty value read by its offset rather than by its size cost a read.
    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {6}, std::span<byte_t const> {widest.data(), 7}).succeeded());
    failures += expect(store->insert((*stack)->channel(), stored_key_t {5}, std::span<byte_t const> {}).succeeded());

    // One byte past the widest value is refused, whatever the memory runs still have room for.
    st::vector<byte_t> wider;
    failures += expect(wider.resize(value_max_size_k + 1) == st::status_t::success_k);
    failures += expect(store->insert((*stack)->channel(), stored_key_t {4}, std::span<byte_t const> {wider}) ==
                       status_code_t::malformed_k);

    st::vector<byte_t> into;
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
        failures += expect(store->find(stored_key_t {2}, size) == lookup_result_t::written_k);
        failures += expect_eq(static_cast<std::size_t>(size), std::size_t {0});
        if (round == 0) failures += expect(store->flush((*stack)->channel()).succeeded());
    }

    // Key five was never read one at a time, so nothing of it sits in the value cache and the batch
    // reaches the run. An empty value is a write of no bytes, so reading it must take no block at all
    // rather than a page nobody wanted.
    std::array<stored_key_t, 1> const untouched {stored_key_t {5}};
    std::array<lookup_result_t, 1> answers {lookup_result_t::missing_k};
    std::uint32_t idlest = collection_config_k.request_buffers;
    std::size_t carried = 0;
    auto watching = [&](std::size_t, std::uint64_t, std::span<byte_t const> bytes) noexcept {
        std::uint32_t const free_blocks = (*stack)->channel().requests().available_buffers();
        if (free_blocks < idlest) idlest = free_blocks;
        carried += bytes.size();
    };
    failures += expect(store
                           ->select((*stack)->channel(), std::span<stored_key_t const> {untouched},
                                    std::span<lookup_result_t> {answers}, watching)
                           .succeeded());
    failures += expect(answers[0] == lookup_result_t::written_k);
    failures += expect_eq(carried, std::size_t {0});
    failures += expect_eq(idlest, collection_config_k.request_buffers);
    return failures;
}

/** A batch naming a key several times keeps its last delta: the last write whatever its
 *  width, an empty value before or after a wider one included, and a removal batch naming a
 *  key twice removes it. */
template <typename mount_type_>
std::size_t repeated_keys_keep_the_last(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_store_t::run_pool_t> runs = make_run_pool<variable_store_t>(**stack);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<variable_store_t> store = variable_store_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger, *runs);
    failures += expect(store.has_value());
    if (!store) return failures;

    // The repeats of keys one to four sit on both sides of more distinct keys than an
    // insertion sort takes.
    constexpr std::size_t distinct_k = 100;
    std::array<stored_key_t, 5> const leading_keys {1, 2, 2, 3, 4};
    std::array<stored_value_size_t, 5> const leading_sizes {3, 0, 5, 0, 2};
    std::array<stored_key_t, 4> const trailing_keys {3, 4, 1, 4};
    std::array<stored_value_size_t, 4> const trailing_sizes {0, 4, 0, 1};
    st::vector<stored_key_t> keys;
    st::vector<stored_value_size_t> sizes;
    std::size_t const count = leading_keys.size() + distinct_k + trailing_keys.size();
    failures += expect(keys.resize(count) == st::status_t::success_k);
    failures += expect(sizes.resize(count) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count; ++index) {
        std::size_t const trailing = index - (count - trailing_keys.size());
        bool const leads = index < leading_keys.size(), trails = index >= count - trailing_keys.size();
        keys[index] = leads ? leading_keys[index] : trails ? trailing_keys[trailing] : 1000 + index;
        sizes[index] = leads ? leading_sizes[index] : trails ? trailing_sizes[trailing] : 1;
    }

    // Where the last value of each of keys one to four starts, and how wide it is.
    std::array<std::size_t, 5> last_offsets {}, last_sizes {};
    std::size_t total = 0;
    for (std::size_t index = 0; index != count; ++index) {
        if (keys[index] < last_offsets.size()) {
            last_offsets[keys[index]] = total;
            last_sizes[keys[index]] = sizes[index];
        }
        total += sizes[index];
    }
    st::vector<byte_t> values;
    failures += expect(values.resize(total) == st::status_t::success_k);
    for (std::size_t index = 0; index != values.size(); ++index) values[index] = painted(13, index);
    failures += expect(store
                           ->insert((*stack)->channel(), std::span<stored_key_t const> {keys},
                                    std::span<byte_t const> {values}, std::span<stored_value_size_t const> {sizes})
                           .succeeded());

    auto holds = [&](stored_key_t stored_key, std::span<byte_t const> wanted) noexcept {
        std::size_t missed = 0;
        stored_value_size_t size = 0;
        missed += expect(store->find(stored_key, size) == lookup_result_t::written_k);
        missed += expect_eq(static_cast<std::size_t>(size), wanted.size());
        std::array<byte_t, 8> into {};
        missed += expect(store->select((*stack)->channel(), stored_key, std::span<byte_t> {into}, size).succeeded());
        missed += expect_eq(std::memcmp(into.data(), wanted.data(), wanted.size()), 0);
        return missed;
    };
    auto last_of = [&](stored_key_t stored_key) noexcept {
        return std::span<byte_t const> {values}.subspan(last_offsets[stored_key], last_sizes[stored_key]);
    };
    for (stored_key_t stored_key = 1; stored_key != last_offsets.size(); ++stored_key)
        failures += holds(stored_key, last_of(stored_key));

    std::array<stored_key_t, 4> const removed {4, 77777, 4, 1};
    failures += expect(store->remove((*stack)->channel(), std::span<stored_key_t const> {removed}).succeeded());
    std::array<stored_key_t, 2> const rewritten {4, 4};
    std::array<stored_value_size_t, 2> const rewritten_sizes {2, 3};
    std::array<byte_t, 5> rewritten_values {};
    for (std::size_t index = 0; index != rewritten_values.size(); ++index) rewritten_values[index] = painted(14, index);
    failures += expect(store
                           ->insert((*stack)->channel(), std::span<stored_key_t const> {rewritten},
                                    std::span<byte_t const> {rewritten_values},
                                    std::span<stored_value_size_t const> {rewritten_sizes})
                           .succeeded());
    for (std::size_t round = 0; round != 2; ++round) {
        stored_value_size_t size = 0;
        failures += expect(store->find(stored_key_t {1}, size) == lookup_result_t::removed_k);
        failures += holds(2, last_of(2));
        failures += holds(3, last_of(3));
        failures += holds(4, std::span<byte_t const> {rewritten_values}.subspan(2, 3));
        if (round == 0) failures += expect(store->flush((*stack)->channel()).succeeded());
    }
    return failures;
}

/** The value cache answers a version only with that version's bytes, never holds more than it was made with however
 *  many versions displace one another, and gives everything back when it dies. */
std::size_t value_cache_holds_what_it_took() {
    std::size_t failures = 0;
    using cache_t = value_cache<stored_key_t, atomic_reference>;
    constexpr stored_value_size_t width_k = 300;
    constexpr std::size_t cache_bytes_k = 1024ull * 1024ull;
    resource_constraint_t budget {std::size_t {8} * 1024 * 1024};

    // A budget that affords entries must hold entries, and a slot is found by masking, so a power of two of them.
    std::optional<cache_t> cache {cache_t::of(cache_bytes_k, width_k, budget)};
    failures += expect(cache->capacity() != 0);
    failures += expect((cache->capacity() & (cache->capacity() - 1)) == 0);
    if (cache->capacity() == 0) return failures;
    std::size_t const slots_bytes = budget.used();

    st::vector<byte_t> value;
    st::vector<byte_t> copied;
    failures += expect(value.resize(width_k) == st::status_t::success_k);
    failures += expect(copied.resize(width_k) == st::status_t::success_k);
    auto fill = [&](std::size_t index, transaction_stamp_t stamp) noexcept {
        for (std::size_t position = 0; position != width_k; ++position)
            value[position] = painted(index + stamp, position);
        cache->add(static_cast<stored_key_t>(index), stamp, std::span<byte_t const> {value});
    };
    std::size_t mismatches = 0;
    auto check = [&](std::size_t index, transaction_stamp_t stamp) noexcept {
        stored_value_size_t size = 0;
        if (!cache->copy(static_cast<stored_key_t>(index), stamp, std::span<byte_t> {copied}, size)) return false;
        if (size != width_k) ++mismatches;
        for (std::size_t position = 0; position != size; ++position)
            if (copied[position] != painted(index + stamp, position)) ++mismatches;
        return true;
    };

    // Two versions of one key are two entries, and a stamp no version carries finds nothing.
    fill(7, 3);
    fill(7, 5);
    failures += expect(check(7, 5));
    failures += expect(!check(7, 4));

    // Four times as many versions as slots overwrite one another in place, within the bytes the cache was made with,
    // and whatever is still found is its own version.
    std::size_t const held = cache->capacity();
    std::size_t found = 0;
    for (std::size_t index = 0; index != 4 * held; ++index) fill(index, 1);
    for (std::size_t index = 0; index != 4 * held; ++index) found += check(index, 1) ? 1 : 0;
    failures += expect(found != 0);
    failures += expect_eq(mismatches, std::size_t {0});
    failures += expect(budget.used() <= cache_bytes_k);

    // With the cache gone, the budget holds nothing.
    cache.reset();
    failures += expect(slots_bytes != 0);
    failures += expect_eq(budget.used(), std::size_t {0});

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
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 1024;
    constexpr std::size_t flushes_k = 4;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_store_t::run_pool_t> runs = make_run_pool<variable_store_t>(**stack);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<variable_store_t> store = variable_store_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger, *runs);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index);
        entry.state = lookup_result_t::written_k;
        entry.bytes = 1 + index % 4093;
        entry.seed = index + 7;
        model[index] = entry;
    }
    for (std::size_t flush = 0; flush != flushes_k; ++flush) {
        st::vector<expectation_t> batch;
        for (std::size_t index = flush; index < keys_k; index += flushes_k)
            failures += expect(batch.push_back(expectation_t {model[index]}) == st::status_t::success_k);
        failures += write_model(*store, (*stack)->channel(), std::span<expectation_t const> {batch});
        failures += expect(store->flush((*stack)->channel()).succeeded());
        if (failures != 0) return failures;
    }
    failures += expect_eq(store->tree().levels().level(0).size(), flushes_k);

    st::vector<stored_key_t> keys;
    st::vector<lookup_result_t> results;
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
                                    std::span<lookup_result_t> {results}, counting)
                           .succeeded());
    failures += expect_eq(mismatches, std::size_t {0});
    failures += expect_eq(collection_config_k.request_buffers - idlest, collection_config_k.queue_depth);
    for (std::size_t index = 0; index != keys_k; ++index)
        failures += expect(results[index] == lookup_result_t::written_k);
    return failures;
}

#pragma endregion Depth Cases

#pragma region Snapshot Cases

/**
 *  @brief A pinned stamp keeps answering the version it pinned while another thread commits and flushes over it.
 *
 *  The writer neither waits for the pin nor drops the older version, since the flush keeps the version a pinned
 *  reader reads, so the read in the middle sees the older bytes whether it lands before the flush or after.
 */
template <typename mount_type_>
std::size_t snapshot_holds_its_moment(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using guard_t = typename types_t::guard_t;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t bytes_k = 900;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_store_t::run_pool_t> runs = make_run_pool<variable_store_t>(**stack);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<variable_store_t> store = variable_store_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger, *runs);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::vector<byte_t> older;
    st::vector<byte_t> newer;
    failures += expect(older.resize(bytes_k) == st::status_t::success_k);
    failures += expect(newer.resize(bytes_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != bytes_k; ++index) older[index] = painted(11, index);
    for (std::size_t index = 0; index != bytes_k; ++index) newer[index] = painted(12, index);

    failures += expect(
        store->insert((*stack)->channel(), stored_key_t {42}, std::span<byte_t const> {older}).succeeded());
    stamp_pin_t pin;
    transaction_stamp_t const pinned = (*stack)->ledger.pin_reader(pin);

    std::size_t writer_failures = 0;
    std::thread writer {[&]() noexcept {
        expected<guard_t> guard = (*stack)->fixture->channels.acquire();
        writer_failures += expect(guard.has_value());
        if (!guard) return;
        writer_failures += expect(
            store->insert(guard->channel(), stored_key_t {42}, std::span<byte_t const> {newer}).succeeded());
        writer_failures += expect(store->flush(guard->channel()).succeeded());
    }};

    for (std::size_t round = 0; round != 2; ++round) {
        st::vector<byte_t> into;
        failures += expect(into.resize(bytes_k) == st::status_t::success_k);
        stored_value_size_t size = 0;
        failures += expect(
            store->select((*stack)->channel(), stored_key_t {42}, std::span<byte_t> {into}, size, pinned).succeeded());
        failures += expect_eq(static_cast<std::size_t>(size), bytes_k);
        failures += expect_eq(std::memcmp(into.data(), older.data(), bytes_k), 0);
        // The second read follows the flush, which kept the pinned version in the run it wrote.
        if (round == 0) writer.join();
    }
    pin.release();
    failures += writer_failures;

    st::vector<byte_t> into;
    failures += expect(into.resize(bytes_k) == st::status_t::success_k);
    stored_value_size_t size = 0;
    failures += expect(
        store->select((*stack)->channel(), stored_key_t {42}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(static_cast<std::size_t>(size), bytes_k);
    failures += expect_eq(std::memcmp(into.data(), newer.data(), bytes_k), 0);
    return failures;
}

/** Reads each key of @p wanted one at a time at @p stamp, counting where the model disagrees. */
template <typename store_type_>
[[nodiscard]] std::size_t check_model_as_of(store_type_& store, io_channel_t& channel,
                                            std::span<expectation_t const> wanted, transaction_stamp_t stamp) {
    std::size_t failures = 0;
    st::vector<byte_t> into;
    for (expectation_t const& entry : wanted) {
        stored_value_size_t size = 0;
        failures += expect(into.resize(static_cast<std::size_t>(entry.bytes)) == st::status_t::success_k);
        status_t const read = store.select(channel, entry.stored_key, std::span<byte_t> {into}, size, stamp);
        if (entry.state != lookup_result_t::written_k) {
            failures += expect(read == status_code_t::not_found_k);
            continue;
        }
        failures += expect(read.succeeded());
        failures += expect_eq(static_cast<std::uint64_t>(size), entry.bytes);
        for (std::uint64_t position = 0; position != size; ++position)
            failures += expect(into[static_cast<std::size_t>(position)] == painted(entry.seed, position));
    }
    return failures;
}

/** Walks @p store at its head both ways and counts the keys each walk hands out, in its order,
 *  against what @p wanted says is written. */
template <typename cursor_type_, typename store_type_>
[[nodiscard]] std::size_t check_scan(store_type_ const& store, std::span<expectation_t const> wanted) {
    std::size_t failures = 0;
    std::size_t present = 0;
    for (expectation_t const& entry : wanted) present += entry.state == lookup_result_t::written_k;
    for (scan_direction_t const direction : {scan_direction_t::ascending_k, scan_direction_t::descending_k}) {
        bool const descending = direction == scan_direction_t::descending_k;
        utilities::local_arena_t walk;
        expected<cursor_type_> scan = cursor_type_::over(
            store, walk.allocator().arena, descending ? limit<stored_key_t>::max() : limit<stored_key_t>::min(),
            head_stamp_k, outside_executor_k, direction);
        failures += expect(scan.has_value());
        if (!scan) return failures;
        std::size_t listed = 0;
        stored_key_t previous = 0;
        for (; !scan->reached_end(); failures += expect(scan->advance().succeeded())) {
            if (scan->state() != lookup_result_t::written_k) continue;
            stored_key_t const key = scan->stored_key();
            if (listed != 0) failures += expect(descending ? key < previous : previous < key);
            previous = key;
            ++listed;
            failures += expect(key < wanted.size() &&
                               wanted[static_cast<std::size_t>(key)].state == lookup_result_t::written_k);
        }
        failures += expect_eq(listed, present);
    }
    return failures;
}

/** A range removal hides every older version of its keys, in memory runs, after the flush that
 *  writes it out and after merges carry it down, while a snapshot pinned before it keeps reading
 *  them and a write after it survives. */
template <typename mount_type_>
std::size_t range_removal_hides_older_runs(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using guard_t = typename types_t::guard_t;
    using variable_store_t = typename types_t::variable_store_t;
    using variable_cursor_t = typename types_t::variable_cursor_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 300;
    constexpr stored_key_t start_k = 50;
    constexpr stored_key_t end_k = 250;
    constexpr stored_key_t rewritten_k = 100;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    // Two runs fill level zero, so every second flush leaves a merge due.
    collection_config_t config = test_config(0, value_max_size_k);
    config.lsm_tree.level_enlarge_factor = 2;
    expected<typename variable_store_t::run_pool_t> runs = make_run_pool<variable_store_t>(**stack);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<variable_store_t> store = variable_store_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger, *runs);
    failures += expect(store.has_value());
    expected<guard_t> writing = (*stack)->fixture->channels.acquire();
    failures += expect(writing.has_value());
    if (!store || !writing) return failures;

    st::vector<expectation_t> before;
    failures += expect(before.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index)
        before[index] = expectation_t {static_cast<stored_key_t>(index), lookup_result_t::written_k, 9 + index % 31,
                                       index * 13 + 1};
    // The lower keys sink a level below the upper ones, so the tombstone hides runs of two levels.
    std::span<expectation_t const> const all {before};
    failures += write_model(*store, (*stack)->channel(), all.first(100));
    failures += expect(store->flush((*stack)->channel()).succeeded());
    failures += write_model(*store, (*stack)->channel(), all.subspan(100, 100));
    failures += expect(store->flush((*stack)->channel()).succeeded());
    std::array<typename variable_store_t::merge_buffers_t, 1> buffers {};
    expected<compaction_outcome_t> const sunk = store->compact((*stack)->channel(), writing->channel(), buffers, {});
    failures += expect(sunk.has_value() && *sunk == compaction_outcome_t::published_k);
    failures += write_model(*store, (*stack)->channel(), all.subspan(200));
    failures += expect(store->flush((*stack)->channel()).succeeded());

    stamp_pin_t pin;
    transaction_stamp_t const pinned = (*stack)->ledger.pin_reader(pin);
    failures += expect(store->remove_range((*stack)->channel(), start_k, end_k).succeeded());
    st::vector<expectation_t> after;
    failures += expect(after.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) {
        after[index] = before[index];
        if (index >= start_k && index < end_k) after[index] = expectation_t {before[index].stored_key};
        if (after[index].state == lookup_result_t::missing_k) after[index].state = lookup_result_t::removed_k;
    }
    expectation_t const rewritten {rewritten_k, lookup_result_t::written_k, 17, 77};
    failures += write_model(*store, (*stack)->channel(), std::span<expectation_t const> {&rewritten, 1});
    after[rewritten_k] = rewritten;

    auto const check = [&]() {
        std::size_t checked = 0;
        checked += check_model(*store, (*stack)->channel(), std::span<expectation_t const> {after});
        checked += check_model_as_of(*store, (*stack)->channel(), std::span<expectation_t const> {after}, head_stamp_k);
        checked += check_model_as_of(*store, (*stack)->channel(), std::span<expectation_t const> {before}, pinned);
        checked += check_scan<variable_cursor_t>(*store, std::span<expectation_t const> {after});
        return checked;
    };
    failures += check();
    failures += expect(store->flush((*stack)->channel()).succeeded());
    failures += check();
    for (std::size_t pass = 0; pass != 4 && store->tree().levels().level(0).size() != 0; ++pass) {
        expected<compaction_outcome_t> const merged = store->compact((*stack)->channel(), writing->channel(), buffers,
                                                                     {});
        failures += expect(merged.has_value() && *merged == compaction_outcome_t::published_k);
        failures += check();
    }

    // With the pin gone, a merge into the bottom drops the tombstone with every version it hid.
    pin.release();
    for (std::size_t flush = 0; flush != 2; ++flush) {
        failures += write_model(*store, (*stack)->channel(), std::span<expectation_t const> {&rewritten, 1});
        failures += expect(store->flush((*stack)->channel()).succeeded());
    }
    expected<compaction_outcome_t> const dropped = store->compact((*stack)->channel(), writing->channel(), buffers, {});
    failures += expect(dropped.has_value() && *dropped == compaction_outcome_t::published_k);
    std::uint64_t tombstones = 0;
    for (std::size_t number = 0; number != store->tree().levels().size(); ++number)
        tombstones += store->tree().levels().level(number).range_tombstones_count();
    failures += expect_eq(tombstones, std::uint64_t {0});
    for (expectation_t& entry : after)
        if (entry.state == lookup_result_t::removed_k) entry.state = lookup_result_t::missing_k;
    failures += check_model(*store, (*stack)->channel(), std::span<expectation_t const> {after});
    failures += check_scan<variable_cursor_t>(*store, std::span<expectation_t const> {after});
    return failures;
}

#pragma endregion Snapshot Cases

#pragma region Scan Cases

/** A scan visits every key once, in ascending order, across the memory runs and two levels. */
template <typename mount_type_>
std::size_t scan_visits_every_key_in_order(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using guard_t = typename types_t::guard_t;
    using variable_store_t = typename types_t::variable_store_t;
    using variable_cursor_t = typename types_t::variable_cursor_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 1000;
    constexpr std::size_t rounds_k = 4;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_store_t::run_pool_t> runs = make_run_pool<variable_store_t>(**stack);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<variable_store_t> store = variable_store_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger, *runs);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) model[index].stored_key = static_cast<stored_key_t>(index);

    auto apply = [&](std::span<expectation_t const> batch) noexcept {
        for (std::size_t index = 0; index != batch.size(); ++index)
            model[static_cast<std::size_t>(batch[index].stored_key)] = batch[index];
    };

    for (std::size_t round = 0; round != rounds_k; ++round) {
        st::vector<expectation_t> batch;
        for (std::size_t index = round; index < keys_k; index += rounds_k) {
            expectation_t entry;
            entry.stored_key = static_cast<stored_key_t>(index);
            entry.state = lookup_result_t::written_k;
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
    std::array<typename variable_store_t::merge_buffers_t, 1> buffers {};
    expected<compaction_outcome_t> const merged = store->compact((*stack)->channel(), writing->channel(), buffers, {});
    failures += expect(merged.has_value());
    if (!merged) return failures;
    failures += expect(*merged == compaction_outcome_t::published_k);
    failures += expect_eq(store->tree().levels().size(), std::size_t {2});

    // Two more flushes put runs back on level zero, and they carry the same keys, so the scan has to
    // pick the younger of two runs of one level as well as the shallower of two levels.
    for (std::size_t again = 0; again != 2; ++again) {
        st::vector<expectation_t> batch;
        for (std::size_t index = 0; index < keys_k; index += 10) {
            expectation_t entry;
            entry.stored_key = static_cast<stored_key_t>(index);
            entry.state = lookup_result_t::written_k;
            entry.bytes = 7 + (index + again * 11) % 53;
            entry.seed = index * 17 + 5 + again;
            failures += expect(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
        }
        failures += write_model(*store, (*stack)->channel(), std::span<expectation_t const> {batch});
        failures += expect(store->flush((*stack)->channel()).succeeded());
        apply(std::span<expectation_t const> {batch});
    }
    failures += expect_eq(store->tree().levels().level(0).size(), std::size_t {2});

    // What stays in memory runs is what the scan must merge in front of every run.
    {
        st::vector<expectation_t> batch;
        for (std::size_t index = 0; index < keys_k; index += 25) {
            expectation_t entry;
            entry.stored_key = static_cast<stored_key_t>(index);
            entry.state = index % 50 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
            entry.bytes = entry.state == lookup_result_t::written_k ? 13 + index % 29 : 0;
            entry.seed = index * 7 + 3;
            failures += expect(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
        }
        failures += write_model(*store, (*stack)->channel(), std::span<expectation_t const> {batch});
        apply(std::span<expectation_t const> {batch});
    }
    failures += expect(store->memory_deltas_count() != 0);

    utilities::local_arena_t walk;
    expected<variable_cursor_t> scan = variable_cursor_t::over(*store, walk.allocator().arena);
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
        // The scan holds no epoch between pages, so its own thread commits while it walks, past its pinned stamp.
        if (visited == 0)
            failures += expect(store->remove((*stack)->channel(), static_cast<stored_key_t>(keys_k - 1)).succeeded());
        ++visited;
        failures += expect(scan->advance().succeeded());
    }
    failures += expect_eq(visited, keys_k);
    failures += expect_eq(out_of_order, std::size_t {0});
    failures += expect_eq(wrong_state, std::size_t {0});
    failures += expect_eq(wrong_size, std::size_t {0});
    model[keys_k - 1].state = lookup_result_t::removed_k;
    model[keys_k - 1].bytes = 0;

    // The newest version of every key must also be the one a read of it brings back.
    failures += check_model(*store, (*stack)->channel(), std::span<expectation_t const> {model});
    return failures;
}

#pragma endregion Scan Cases

#pragma region Fold Cases

/** A counter of 8 bytes, which a write sets, a removal clears, and a change adds its own 8 bytes to. */
struct counter_fold {
    static std::uint64_t count_of(std::span<byte_t const> bytes) noexcept {
        std::uint64_t count = 0;
        if (!bytes.empty()) std::memcpy(&count, bytes.data(), std::min(bytes.size(), sizeof(count)));
        return count;
    }
    static expected<std::span<byte_t const>> counted(std::uint64_t count, arena_allocator<byte_t>& arena) noexcept {
        byte_t* const bytes = arena.allocate(sizeof(count));
        if (!bytes) return status_t {status_code_t::out_of_memory_k, "No room for a count"};
        std::memcpy(bytes, &count, sizeof(count));
        return std::span<byte_t const> {bytes, sizeof(count)};
    }
    expected<std::span<byte_t const>> apply(std::span<byte_t const> base, std::span<delta_view_t const> deltas,
                                            arena_allocator<byte_t>& arena) const noexcept {
        std::uint64_t count = count_of(base);
        for (delta_view_t const& delta : deltas) count += count_of(delta.bytes);
        return counted(count, arena);
    }
    expected<std::span<byte_t const>> compose(std::span<delta_view_t const> deltas,
                                              arena_allocator<byte_t>& arena) const noexcept {
        return apply({}, deltas, arena);
    }
    bool closes(delta_view_t const& delta) const noexcept { return delta.kind != delta_kind_t::change_k; }
};

/** Whether a round of reads also checks what @c find answers, which a flushed level's value needs a
 *  read for. */
enum class find_answers_t : bool { unchecked_k, checked_k };

/** Counters folded from change chains read as a map says, while the chains sit in memory runs, once a merge folds
 *  them, once two flushes leave them in level zero, and once a compaction applies them at the bottom level. */
template <typename mount_type_>
std::size_t counter_fold_reads_alike_everywhere(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using guard_t = typename types_t::guard_t;
    using stack_t = typename types_t::stack_t;
    using counter_store_t = nvme_store<variable_slot_t, typename types_t::volume_t, atomic_reference, waiter_t,
                                       counter_fold, no_device_fold_t>;
    std::size_t failures = 0;
    constexpr stored_key_t keys_k = 12;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    // A level zero due at two runs, which the two flushes below fill.
    collection_config_t config = test_config(0, value_max_size_k);
    config.memory_runs_max_bytes = 64;
    config.lsm_tree.level_enlarge_factor = 2;
    expected<typename counter_store_t::run_pool_t> runs = make_run_pool<counter_store_t>(**stack);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<counter_store_t> store = counter_store_t::mount(
        *(*stack)->volume, config, (*stack)->fixture->resources->memory_constraint, (*stack)->ledger, *runs);
    failures += expect(store.has_value());
    if (!store) return failures;

    // Key zero only ever changes, so its chain has no base until the bottom level folds it onto nothing.
    std::map<stored_key_t, std::optional<std::uint64_t>> model;
    auto commit_round = [&](std::uint64_t round) {
        std::array<std::uint64_t, keys_k> words {};
        std::array<keyed_delta<stored_key_t>, keys_k> deltas {};
        for (stored_key_t key = 0; key != keys_k; ++key) {
            std::uint64_t const operation = key == 0 ? 2 : (key * 7 + round) % 5;
            std::optional<std::uint64_t>& counted = model[key];
            delta_view_t delta {delta_kind_t::change_k, {}};
            if (operation == 0) {
                delta.kind = delta_kind_t::write_k;
                words[key] = round * 1000 + key;
                counted = words[key];
            }
            else if (operation == 1) {
                delta.kind = delta_kind_t::remove_k;
                counted.reset();
            }
            else {
                words[key] = key + round + 1;
                counted = counted.value_or(0) + words[key];
            }
            if (delta.kind != delta_kind_t::remove_k)
                delta.bytes = std::as_bytes(std::span<std::uint64_t const, 1> {&words[key], 1});
            deltas[key] = keyed_delta<stored_key_t> {key, delta};
        }
        auto [sealed, seal_status] = store->seal(std::span<keyed_delta<stored_key_t>> {deltas});
        memory_run_claim_t claimed;
        if (seal_status.failed() || store->try_claim(claimed).failed()) return expect(false);
        return expect(store->commit(sealed, std::move(claimed)).succeeded());
    };

    // Point reads, the size a find answers where no level's value needs a read, and one batch, all against the map.
    auto check = [&](find_answers_t finds) {
        std::size_t wrong = 0;
        std::array<stored_key_t, keys_k + 1> keys {};
        std::array<lookup_result_t, keys_k + 1> answers {};
        std::array<std::uint64_t, keys_k + 1> batched {};
        for (stored_key_t key = 0; key != keys_k + 1; ++key) {
            keys[key] = key;
            std::optional<std::uint64_t> const wanted = model.count(key) ? model[key] : std::nullopt;
            std::uint64_t count = 0;
            stored_value_size_t size = 0;
            status_t const selected = store->select(
                (*stack)->channel(), key, std::as_writable_bytes(std::span<std::uint64_t, 1> {&count, 1}), size);
            wrong += wanted ? selected.failed() || size != sizeof(count) || count != *wanted
                            : !(selected == status_code_t::not_found_k);
            lookup_result_t const found = store->find(key, size);
            if (finds == find_answers_t::checked_k)
                wrong += wanted ? found != lookup_result_t::written_k || size != sizeof(count)
                                : found == lookup_result_t::written_k;
        }
        auto sink = [&](std::size_t request, std::uint64_t, std::span<byte_t const> bytes) noexcept {
            batched[request] = counter_fold::count_of(bytes);
        };
        failures += expect(store
                               ->select((*stack)->channel(), std::span<stored_key_t const> {keys},
                                        std::span<lookup_result_t> {answers}, sink)
                               .succeeded());
        for (stored_key_t key = 0; key != keys_k + 1; ++key) {
            std::optional<std::uint64_t> const wanted = model.count(key) ? model[key] : std::nullopt;
            wrong += wanted ? answers[key] != lookup_result_t::written_k || batched[key] != *wanted
                            : answers[key] == lookup_result_t::written_k;
        }
        return expect_eq(wrong, std::size_t {0});
    };

    for (std::uint64_t round = 0; round != 8; ++round) failures += commit_round(round);
    failures += check(find_answers_t::checked_k);
    failures += expect(store->merge_memory_runs().succeeded());
    failures += expect(store->memory_shape().runs < 8);
    failures += check(find_answers_t::checked_k);

    for (std::uint64_t round = 8; round != 12; ++round) failures += commit_round(round);
    failures += expect(store->flush((*stack)->channel()).succeeded());
    for (std::uint64_t round = 12; round != 14; ++round) failures += commit_round(round);
    failures += expect(store->flush((*stack)->channel()).succeeded());
    failures += commit_round(14);
    // Key zero's chain reaches level zero as changes in both runs, so its reads fold through their values.
    auto const& flushed = store->tree().levels().level(0);
    for (std::size_t index = 0; index != flushed.size(); ++index)
        failures += expect(holds_changes(&flushed.run(index).kinds()));
    failures += expect_eq(flushed.size(), std::size_t {2});
    failures += check(find_answers_t::unchecked_k);

    expected<guard_t> writing = (*stack)->fixture->channels.acquire();
    failures += expect(writing.has_value());
    if (!writing) return failures;
    std::array<typename counter_store_t::merge_buffers_t, 1> buffers {};
    expected<compaction_outcome_t> const compacted = store->compact((*stack)->channel(), writing->channel(), buffers,
                                                                    {});
    failures += expect(compacted.has_value() && *compacted == compaction_outcome_t::published_k);
    auto const& levels = store->tree().levels();
    failures += expect(levels.size() == 2 && levels.level(0).empty());
    for (std::size_t index = 0; levels.size() == 2 && index != levels.level(1).size(); ++index)
        failures += expect(!holds_changes(&levels.level(1).run(index).kinds()));
    failures += check(find_answers_t::unchecked_k);
    return failures;
}

#pragma endregion Fold Cases

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
        report(totals, mount, "misses_and_remove_deltas", misses_and_remove_deltas);
        report(totals, mount, "buffer_before_tree", memory_runs_answer_before_the_tree);
        report(totals, mount, "full_buffer_flushes", full_memory_runs_flush_themselves);
        report(totals, mount, "burst_past_records", burst_past_records_lands_every_commit);
        report(totals, mount, "unaligned_exact_span", unaligned_exact_span_is_taken);
        report(totals, mount, "widest_and_empty", widest_and_empty_values);
        report(totals, mount, "repeated_keys_keep_the_last", repeated_keys_keep_the_last);
        report(totals, mount, "holds_its_depth", batched_select_holds_its_depth);
        report(totals, mount, "snapshot_holds_its_moment", snapshot_holds_its_moment);
        report(totals, mount, "range_removal_hides_older_runs", range_removal_hides_older_runs);
        report(totals, mount, "scan_in_order", scan_visits_every_key_in_order);
        report(totals, mount, "counter_fold", counter_fold_reads_alike_everywhere);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, collection_config_k, 1, "collection", cases);

    return suite_exit_code(totals);
}
