/**
 *  @file test/nvme/collection.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The store of one collection over the ring: what the memory runs answer, what the tree answers, and how deep
 *      it reads.
 *
 *  The results this file exists for are @c holds_its_depth and @c test_snapshot_holds_its_moment. A
 *  batched select must sit at the queue pair's depth through the middle of a run rather than at the
 *  size of whatever batch a caller handed it, and a pinned stamp must keep answering the versions
 *  it pinned while another thread commits and flushes over them.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcmp`

#include <array>    // `std::array`
#include <map>      // `std::map`
#include <memory>   // `std::unique_ptr`
#include <new>      // `std::nothrow`
#include <optional> // `std::optional`
#include <span>     // `std::span`
#include <thread>   // `std::thread`
#include <utility>  // `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_store.hpp"    // `nvme_store`, `nvme_store_cursor`
#include "ustore/blobs/nvme_key_slot.hpp" // `key_slot`

#include "nvme/fixture.hpp" // `run_on_transport`, `expectation_t`, `painted`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;

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
    using volume_t = typename fixture_t::volume_t;
    using variable_store_t =
        nvme_store<variable_slot_t, volume_t, atomic_reference, waiter_t, util::last_write_fold_t, no_device_fold_t>;
    using variable_cursor_t = nvme_store_cursor<variable_slot_t, volume_t, atomic_reference, waiter_t,
                                                util::last_write_fold_t, no_device_fold_t>;

    /** A fixture, one checked-out channel and the volume mounted over it, which every case starts from. */
    struct stack_t {

        /** The disk, the arena and the channels. */
        std::unique_ptr<fixture_t> fixture;

        /** The channel the calling thread submits and polls on. */
        util::io_channel_t* guard = nullptr;

        /** The volume every run lands on, pinned so a collection may point at it. */
        std::unique_ptr<volume_t> volume;

        /** The stamps every collection of the stack holds, outliving the collections. */
        commit_ledger_t ledger;

        /** The channel of this stack, which every call that moves bytes takes. */
        [[nodiscard]] util::io_channel_t& channel() noexcept { return *guard; }
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
util::expected<typename store_type_::run_pool_t> make_run_pool(stack_type_& stack) noexcept {
    return store_type_::run_pool_t::make(run_records_k, stack.fixture->resources->memory_constraint);
}

/** Mounts a fixture of @p reshaped through @p mount, takes one channel and formats one
 *  volume over it. */
template <typename mount_type_>
util::expected<std::unique_ptr<typename collection_types<typename mount_type_::fixture_t>::stack_t>> make_stack(
    mount_type_ const& mount, fixture_config_t const& reshaped) noexcept {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using volume_t = typename types_t::volume_t;
    auto stack = std::unique_ptr<stack_t> {new (std::nothrow) stack_t {}};
    if (!stack) return util::status_t {util::status_code_t::out_of_memory_k, "The stack did not fit"};

    auto [fixture, mount_status] = mount(reshaped);
    if (mount_status.failed()) return mount_status;
    stack->fixture = std::move(fixture);

    auto [guard, acquire_status] = stack->fixture->channel(0);
    if (acquire_status.failed()) return acquire_status;
    stack->guard = std::move(guard);

    auto [volume, mount_volume_status] = stack->fixture->mount_volume(*stack->guard);
    if (mount_volume_status.failed()) return mount_volume_status;
    stack->volume = std::unique_ptr<volume_t> {new (std::nothrow) volume_t {std::move(volume)}};
    if (!stack->volume) return util::status_t {util::status_code_t::out_of_memory_k, "The volume did not fit"};
    return stack;
}

/** Mounts a fixture of the suite's config through @p mount, as @c make_stack of a
 *  reshaped one does. */
template <typename mount_type_>
util::expected<std::unique_ptr<typename collection_types<typename mount_type_::fixture_t>::stack_t>> make_stack(
    mount_type_ const& mount) noexcept {
    return make_stack(mount, mount.config);
}

#pragma endregion Fixtures

#pragma region Model

/** Writes every entry of @p wanted into @p store, one commit per key. */
template <typename collection_type_>
void write_model(collection_type_& store, util::io_channel_t& channel, std::span<expectation_t const> wanted) {
    st::vector<util::byte_t> value;
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        expectation_t const& entry = wanted[index];
        if (entry.state == lookup_result_t::removed_k) {
            expect_true(store.remove(channel, entry.stored_key).succeeded());
            continue;
        }
        expect_true(value.resize(static_cast<std::size_t>(entry.bytes)) == st::status_t::success_k);
        paint_into(entry, std::span<util::byte_t> {value});
        expect_true(store.insert(channel, entry.stored_key, std::span<util::byte_t const> {value}).succeeded());
    }
}

/** Reads every key of @p wanted through one batched select and counts what the model disagrees with. */
template <typename readable_type_>
void check_model(readable_type_ const& source, util::io_channel_t& channel, std::span<expectation_t const> wanted) {
    std::size_t const count = wanted.size();
    st::vector<stored_key_t> keys;
    st::vector<lookup_result_t> results;
    st::vector<std::uint64_t> seen;
    expect_true(keys.resize(count) == st::status_t::success_k);
    expect_true(results.resize(count) == st::status_t::success_k);
    if (!expect_true(seen.resize(count) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != count; ++index) {
        keys[index] = wanted[index].stored_key;
        results[index] = lookup_result_t::missing_k;
        seen[index] = 0;
    }

    std::size_t mismatches = 0;
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<util::byte_t const> bytes) noexcept {
        seen[request] += bytes.size();
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(wanted[request].seed, offset + index)) ++mismatches;
    };
    expect_true(source.select(channel, std::span<stored_key_t const> {keys}, std::span<lookup_result_t> {results}, sink)
                    .succeeded());
    expect_equal(mismatches, std::size_t {0});

    for (std::size_t index = 0; index != count; ++index) {
        expect_true(results[index] == wanted[index].state);
        if (wanted[index].state == lookup_result_t::written_k) expect_equal(seen[index], wanted[index].bytes);
    }
}

/** Reads every key of @p wanted one at a time, which is the path that fills the value cache. */
template <typename readable_type_>
void check_model_one_by_one(readable_type_& source, util::io_channel_t& channel,
                            std::span<expectation_t const> wanted) {
    st::vector<util::byte_t> into;
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        expectation_t const& entry = wanted[index];
        stored_value_size_t size = 0;
        if (entry.state != lookup_result_t::written_k) {
            expect_true(source.select(channel, entry.stored_key, std::span<util::byte_t> {}, size).failed());
            continue;
        }
        expect_true(into.resize(static_cast<std::size_t>(entry.bytes)) == st::status_t::success_k);
        expect_true(source.select(channel, entry.stored_key, std::span<util::byte_t> {into}, size).succeeded());
        expect_equal(static_cast<std::uint64_t>(size), entry.bytes);
        for (std::uint64_t position = 0; position != entry.bytes; ++position)
            if (into[static_cast<std::size_t>(position)] != painted(entry.seed, position)) ++mismatches;
    }
    expect_equal(mismatches, std::size_t {0});
}

#pragma endregion Model

#pragma region Round Trip Cases

/** Values of many widths survive the memory runs, the flush and the read back, byte for byte. */
template <typename mount_type_>
void test_mixed_widths_round_trip(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t count_k = 700;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    collection_config_t const config = test_config(0, value_max_size_k);
    auto [runs, runs_status] = make_run_pool<variable_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = variable_store_t::mount(
        *stack->volume, config, stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(count_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 2 + 1;
        entry.state = index % 11 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
        entry.bytes = entry.state == lookup_result_t::written_k ? 1 + index % 4093 : 0;
        entry.seed = index;
        model[index] = entry;
    }

    write_model(store, stack->channel(), std::span<expectation_t const> {model});

    // Nothing is flushed yet, so the memory runs are what answers.
    expect_equal(store.memory_deltas_count(), count_k);
    check_model(store, stack->channel(), std::span<expectation_t const> {model});

    expect_true(store.flush(stack->channel()).succeeded());
    expect_equal(store.memory_deltas_count(), std::size_t {0});
    check_model(store, stack->channel(), std::span<expectation_t const> {model});
    check_model_one_by_one(store, stack->channel(), std::span<expectation_t const> {model});

    // Reading again now goes through the value cache the point reads just filled.
    check_model_one_by_one(store, stack->channel(), std::span<expectation_t const> {model});
}

/** Writes and reads one collection of @p key_slot_type_, which is how every layout is covered. */
template <typename key_slot_type_, typename mount_type_>
void layout_round_trip(mount_type_ const& mount, stored_value_size_t value_size, stored_value_size_t value_max_size) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using nvme_store_t = nvme_store<key_slot_type_, typename types_t::volume_t, atomic_reference, waiter_t,
                                    util::last_write_fold_t, no_device_fold_t>;
    constexpr std::size_t count_k = 300;

    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    collection_config_t const config = test_config(value_size, value_max_size);
    auto [runs, runs_status] = make_run_pool<nvme_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = nvme_store_t::mount(*stack->volume, config,
                                                     stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;

    stored_value_size_t const widest = store.value_max_size();
    st::vector<expectation_t> model;
    if (!expect_true(model.resize(count_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 3;
        entry.state = index % 13 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
        entry.bytes = entry.state != lookup_result_t::written_k ? 0 : value_size != 0 ? value_size : 1 + index % widest;
        entry.seed = index + widest;
        model[index] = entry;
    }

    write_model(store, stack->channel(), std::span<expectation_t const> {model});
    check_model(store, stack->channel(), std::span<expectation_t const> {model});
    expect_true(store.flush(stack->channel()).succeeded());
    check_model(store, stack->channel(), std::span<expectation_t const> {model});
    check_model_one_by_one(store, stack->channel(), std::span<expectation_t const> {model});
}

/** Every value layout the collection serves round trips through the memory runs and the tree. */
template <typename mount_type_>
void test_every_layout_round_trips(mount_type_ const& mount) {
    layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_12_bytes_k>>(mount, 0, 0);
    layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_44_bytes_k>>(mount, 0, 0);
    layout_round_trip<key_slot<stored_key_t, value_layout_t::fixed_width_k>>(mount, 512, 0);
    layout_round_trip<key_slot<stored_key_t, value_layout_t::variable_width_k>>(mount, 0, 4096);
}

#pragma endregion Round Trip Cases

#pragma region Presence Cases

/** A key nobody wrote is missing, and a key a remove delta covers is removed in both places it can live. */
template <typename mount_type_>
void test_misses_and_remove_deltas(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    collection_config_t const config = test_config(0, value_max_size_k);
    auto [runs, runs_status] = make_run_pool<variable_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = variable_store_t::mount(
        *stack->volume, config, stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;

    st::vector<util::byte_t> value;
    expect_true(value.resize(64) == st::status_t::success_k);
    for (std::size_t index = 0; index != value.size(); ++index) value[index] = painted(5, index);

    expect_true(store.insert(stack->channel(), stored_key_t {10}, std::span<util::byte_t const> {value}).succeeded());
    expect_true(store.insert(stack->channel(), stored_key_t {20}, std::span<util::byte_t const> {value}).succeeded());
    expect_true(store.flush(stack->channel()).succeeded());
    expect_true(store.remove(stack->channel(), stored_key_t {20}).succeeded());

    stored_value_size_t size = 0;
    expect_true(store.find(stored_key_t {10}, size) == lookup_result_t::written_k);
    expect_equal(static_cast<std::size_t>(size), value.size());

    // The removal sits in a memory run, which answers before the run that still holds the value.
    expect_true(store.find(stored_key_t {20}, size) == lookup_result_t::removed_k);
    expect_true(store.find(stored_key_t {30}, size) == lookup_result_t::missing_k);

    st::vector<util::byte_t> into;
    expect_true(into.resize(value.size()) == st::status_t::success_k);
    expect_status(store.select(stack->channel(), stored_key_t {20}, std::span<util::byte_t> {into}, size),
                  util::status_code_t::not_found_k);
    expect_status(store.select(stack->channel(), stored_key_t {30}, std::span<util::byte_t> {into}, size),
                  util::status_code_t::not_found_k);

    // Once the remove delta reaches the tree it must keep shadowing the older run.
    expect_true(store.flush(stack->channel()).succeeded());
    expect_true(store.find(stored_key_t {20}, size) == lookup_result_t::removed_k);
    expect_true(store.find(stored_key_t {30}, size) == lookup_result_t::missing_k);
    expect_status(store.select(stack->channel(), stored_key_t {20}, std::span<util::byte_t> {into}, size),
                  util::status_code_t::not_found_k);

    std::array<stored_key_t, 3> const keys {stored_key_t {10}, stored_key_t {20}, stored_key_t {30}};
    std::array<lookup_result_t, 3> answers {lookup_result_t::missing_k, lookup_result_t::missing_k,
                                            lookup_result_t::missing_k};
    auto sink = [](std::size_t, std::uint64_t, std::span<util::byte_t const>) noexcept {};
    expect_true(
        store.select(stack->channel(), std::span<stored_key_t const> {keys}, std::span<lookup_result_t> {answers}, sink)
            .succeeded());
    expect_true(answers[0] == lookup_result_t::written_k);
    expect_true(answers[1] == lookup_result_t::removed_k);
    expect_true(answers[2] == lookup_result_t::missing_k);
}

/** A memory run answers a value the tree has never seen, and the tree answers it after a flush. */
template <typename mount_type_>
void test_memory_runs_answer_before_the_tree(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    collection_config_t const config = test_config(0, value_max_size_k);
    auto [runs, runs_status] = make_run_pool<variable_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = variable_store_t::mount(
        *stack->volume, config, stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;

    st::vector<util::byte_t> older;
    st::vector<util::byte_t> newer;
    expect_true(older.resize(300) == st::status_t::success_k);
    expect_true(newer.resize(700) == st::status_t::success_k);
    for (std::size_t index = 0; index != older.size(); ++index) older[index] = painted(1, index);
    for (std::size_t index = 0; index != newer.size(); ++index) newer[index] = painted(2, index);

    expect_true(store.insert(stack->channel(), stored_key_t {77}, std::span<util::byte_t const> {older}).succeeded());
    expect_true(store.flush(stack->channel()).succeeded());
    expect_equal(store.tree().levels().population_count(), std::uint64_t {1});

    // Reading the run once is what puts the older value in the cache a later flush has to drop.
    st::vector<util::byte_t> cached;
    expect_true(cached.resize(older.size()) == st::status_t::success_k);
    stored_value_size_t cached_size = 0;
    expect_true(
        store.select(stack->channel(), stored_key_t {77}, std::span<util::byte_t> {cached}, cached_size).succeeded());
    expect_equal(static_cast<std::size_t>(cached_size), older.size());
    expect_equal(std::memcmp(cached.data(), older.data(), older.size()), 0);

    // Committed over the run, the memory run must be the one that answers.
    expect_true(store.insert(stack->channel(), stored_key_t {77}, std::span<util::byte_t const> {newer}).succeeded());
    st::vector<util::byte_t> into;
    expect_true(into.resize(newer.size()) == st::status_t::success_k);
    stored_value_size_t size = 0;
    expect_true(store.select(stack->channel(), stored_key_t {77}, std::span<util::byte_t> {into}, size).succeeded());
    expect_equal(static_cast<std::size_t>(size), newer.size());
    expect_equal(std::memcmp(into.data(), newer.data(), newer.size()), 0);

    expect_true(store.flush(stack->channel()).succeeded());
    expect_equal(store.memory_deltas_count(), std::size_t {0});
    size = 0;
    for (std::size_t index = 0; index != into.size(); ++index) into[index] = util::byte_t {0};
    expect_true(store.select(stack->channel(), stored_key_t {77}, std::span<util::byte_t> {into}, size).succeeded());
    expect_equal(static_cast<std::size_t>(size), newer.size());
    expect_equal(std::memcmp(into.data(), newer.data(), newer.size()), 0);
}

/** A burst of one-delta commits many times the memory-run records the collection holds
 *  stalls on the records, merges by tier and lands every commit, keeping no more runs than a
 *  merged view holds. */
template <typename mount_type_>
void test_burst_past_records_lands_every_commit(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t count_k = 2000;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    // Bounds of deltas and bytes no burst below reaches, so only the records stall it: (4 − 1)(7 +
    // 1) runs at most.
    collection_config_t const config = test_config(0, value_max_size_k);
    std::size_t const merged_runs_max = (config.lsm_tree.level_enlarge_factor - 1) * (7 + 1);
    auto [runs, runs_status] = make_run_pool<variable_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = variable_store_t::mount(
        *stack->volume, config, stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(count_k) == st::status_t::success_k)) return;
    std::size_t widest_shape = 0;
    st::vector<util::byte_t> value;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 3 + 1;
        entry.state = lookup_result_t::written_k;
        entry.bytes = 1 + index % 16;
        entry.seed = index + 11;
        model[index] = entry;
        expect_true(value.resize(static_cast<std::size_t>(entry.bytes)) == st::status_t::success_k);
        paint_into(entry, std::span<util::byte_t> {value});
        expect_true(
            store.insert(stack->channel(), entry.stored_key, std::span<util::byte_t const> {value}).succeeded());
        widest_shape = fu::max_of(widest_shape, store.memory_shape().runs);
    }
    expect_true(widest_shape <= merged_runs_max);
    expect_equal(store.memory_deltas_count(), count_k);
    check_model(store, stack->channel(), std::span<expectation_t const> {model});
}

/** Memory runs too small for the batch flush themselves, and every value survives the flushes it forced. */
template <typename mount_type_>
void test_full_memory_runs_flush_themselves(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t count_k = 500;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    collection_config_t config = test_config(0, value_max_size_k);
    config.memory_runs_max_bytes = 32ull * 1024ull;
    config.memory_runs_max_deltas = 64;
    auto [runs, runs_status] = make_run_pool<variable_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = variable_store_t::mount(
        *stack->volume, config, stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(count_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 5 + 2;
        entry.state = lookup_result_t::written_k;
        entry.bytes = 120 + index % 200;
        entry.seed = index * 3 + 1;
        model[index] = entry;
    }

    write_model(store, stack->channel(), std::span<expectation_t const> {model});
    expect_true(store.memory_deltas_count() <= config.memory_runs_max_deltas);
    expect_true(store.tree().levels().level(0).size() > std::size_t {1});
    check_model(store, stack->channel(), std::span<expectation_t const> {model});

    // A batch larger than the bounds of the memory runs lands whole, past the bounds, and the next write flushes it.
    st::vector<util::byte_t> wide;
    expect_true(wide.resize(64ull * 1024ull) == st::status_t::success_k);
    for (std::size_t index = 0; index != wide.size(); ++index) wide[index] = painted(7, index);
    st::vector<stored_key_t> keys;
    st::vector<stored_value_size_t> sizes;
    expect_true(keys.resize(2) == st::status_t::success_k);
    expect_true(sizes.resize(2) == st::status_t::success_k);
    keys[0] = 900001;
    keys[1] = 900002;
    sizes[0] = static_cast<stored_value_size_t>(wide.size() / 2);
    sizes[1] = static_cast<stored_value_size_t>(wide.size() / 2);
    expect_true(store
                    .insert(stack->channel(), std::span<stored_key_t const> {keys},
                            std::span<util::byte_t const> {wide}, std::span<stored_value_size_t const> {sizes})
                    .succeeded());
    expect_equal(store.memory_deltas_count(), std::size_t {2});
    st::vector<util::byte_t> short_value;
    expect_true(short_value.resize(8) == st::status_t::success_k);
    expect_true(
        store.insert(stack->channel(), stored_key_t {900003}, std::span<util::byte_t const> {short_value}).succeeded());
    expect_equal(store.memory_deltas_count(), std::size_t {1});
    stored_value_size_t size = 0;
    expect_true(store.find(stored_key_t {900002}, size) == lookup_result_t::written_k);
    expect_equal(static_cast<std::size_t>(size), wide.size() / 2);
}

/** Memory runs past one slot flush as several runs of level zero, and a range removal across
 *  their cut still hides what an older run holds. */
template <typename mount_type_>
void test_flush_past_one_slot_cuts_runs(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t older_k = 64;
    constexpr std::size_t batches_k = 17;
    constexpr std::size_t batch_keys_k = 1024;
    constexpr stored_key_t start_k = 16385;
    constexpr stored_key_t end_k = 34001;
    // The memory runs hold a little over one slot, and the memory disk the runs flushed from them.
    fixture_config_t shape = mount.config;
    shape.memory_limit = 3ull * 1024ull * 1024ull * 1024ull;
    auto [stack, make_status] = make_stack(mount, shape);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    collection_config_t config = test_config(0, value_max_size_k);
    config.memory_runs_max_bytes = 2ull * 1024ull * 1024ull * 1024ull;
    config.memory_runs_max_deltas = 2 * (older_k + batches_k * batch_keys_k);
    auto [runs, runs_status] = make_run_pool<variable_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = variable_store_t::mount(
        *stack->volume, config, stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(older_k + batches_k * batch_keys_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != older_k; ++index)
        model[index] = expectation_t {1 + index * 544, lookup_result_t::written_k, 100 + index, index * 7 + 3};
    std::span<expectation_t const> const older {model.data(), older_k};
    write_model(store, stack->channel(), older);
    expect_true(store.flush(stack->channel()).succeeded());
    expect_true(store.remove_range(stack->channel(), start_k, end_k).succeeded());
    for (std::size_t index = 0; index != older_k; ++index)
        if (model[index].stored_key >= start_k && model[index].stored_key < end_k)
            model[index] = expectation_t {model[index].stored_key, lookup_result_t::removed_k};

    st::vector<util::byte_t> batch;
    st::vector<stored_key_t> keys;
    st::vector<stored_value_size_t> sizes;
    expect_true(batch.resize(batch_keys_k * value_max_size_k) == st::status_t::success_k);
    expect_true(keys.resize(batch_keys_k) == st::status_t::success_k);
    if (!expect_true(sizes.resize(batch_keys_k) == st::status_t::success_k)) return;
    for (std::size_t batch_index = 0; batch_index != batches_k; ++batch_index) {
        for (std::size_t index = 0; index != batch_keys_k; ++index) {
            std::size_t const written = batch_index * batch_keys_k + index;
            expectation_t& entry = model[older_k + written];
            entry = expectation_t {written * 2, lookup_result_t::written_k, value_max_size_k, written * 5 + 11};
            keys[index] = entry.stored_key;
            sizes[index] = value_max_size_k;
            paint_into(entry, std::span<util::byte_t> {batch}.subspan(index * value_max_size_k, value_max_size_k));
        }
        expect_true(store
                        .insert(stack->channel(), std::span<stored_key_t const> {keys},
                                std::span<util::byte_t const> {batch}, std::span<stored_value_size_t const> {sizes})
                        .succeeded());
    }
    expect_equal(store.tree().levels().level(0).size(), std::size_t {1});
    expect_status(store.flush(stack->channel()), util::status_code_t::success_k);
    expect_true(store.tree().levels().level(0).size() > std::size_t {2});
    check_model(store, stack->channel(), std::span<expectation_t const> {model});
}

#pragma endregion Presence Cases

#pragma region Buffer Cases

/** A caller's span is taken at the value's own size and at any address, aligned or not. */
template <typename mount_type_>
void test_unaligned_exact_span_is_taken(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    collection_config_t const config = test_config(0, value_max_size_k);
    auto [runs, runs_status] = make_run_pool<variable_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = variable_store_t::mount(
        *stack->volume, config, stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;

    constexpr std::size_t bytes_k = 5001;
    st::vector<util::byte_t> value;
    expect_true(value.resize(bytes_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != bytes_k; ++index) value[index] = painted(9, index);
    expect_true(store.insert(stack->channel(), stored_key_t {3}, std::span<util::byte_t const> {value}).succeeded());
    expect_true(store.flush(stack->channel()).succeeded());

    // One byte of slack in front makes the span odd, and it is exactly as long as the value.
    st::vector<util::byte_t> room;
    expect_true(room.resize(bytes_k + 1) == st::status_t::success_k);
    for (std::size_t index = 0; index != room.size(); ++index) room[index] = util::byte_t {0};
    std::span<util::byte_t> const unaligned {room.data() + 1, bytes_k};
    expect_true(reinterpret_cast<std::uintptr_t>(unaligned.data()) % util::buffer_alignment_k != 0);

    stored_value_size_t size = 0;
    expect_true(store.select(stack->channel(), stored_key_t {3}, unaligned, size).succeeded());
    expect_equal(static_cast<std::size_t>(size), bytes_k);
    expect_equal(std::memcmp(unaligned.data(), value.data(), bytes_k), 0);
    expect_true(room[0] == util::byte_t {0});

    // A span one byte shorter than the value is refused rather than half filled.
    expect_true(store.select(stack->channel(), stored_key_t {3}, unaligned.first(bytes_k - 1), size) ==
                util::status_code_t::invalid_argument_k);
}

/** The widest value a layout admits and the empty value both survive a round trip. */
template <typename mount_type_>
void test_widest_and_empty_values(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    collection_config_t const config = test_config(0, value_max_size_k);
    auto [runs, runs_status] = make_run_pool<variable_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = variable_store_t::mount(
        *stack->volume, config, stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;
    expect_equal(store.value_max_size(), value_max_size_k);

    st::vector<util::byte_t> widest;
    expect_true(widest.resize(value_max_size_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != widest.size(); ++index) widest[index] = painted(4, index);
    expect_true(store.insert(stack->channel(), stored_key_t {1}, std::span<util::byte_t const> {widest}).succeeded());
    expect_true(store.insert(stack->channel(), stored_key_t {2}, std::span<util::byte_t const> {}).succeeded());

    // Seven bytes ahead of it leave the next flushed value on no page boundary, which is what would make an
    // empty value read by its offset rather than by its size cost a read.
    expect_true(
        store.insert(stack->channel(), stored_key_t {6}, std::span<util::byte_t const> {widest.data(), 7}).succeeded());
    expect_true(store.insert(stack->channel(), stored_key_t {5}, std::span<util::byte_t const> {}).succeeded());

    // One byte past the widest value is refused, whatever the memory runs still have room for.
    st::vector<util::byte_t> wider;
    expect_true(wider.resize(value_max_size_k + 1) == st::status_t::success_k);
    expect_true(store.insert(stack->channel(), stored_key_t {4}, std::span<util::byte_t const> {wider}) ==
                util::status_code_t::malformed_k);

    st::vector<util::byte_t> into;
    expect_true(into.resize(value_max_size_k) == st::status_t::success_k);
    for (std::size_t round = 0; round != 2; ++round) {
        stored_value_size_t size = 0;
        expect_true(store.select(stack->channel(), stored_key_t {1}, std::span<util::byte_t> {into}, size).succeeded());
        expect_equal(static_cast<std::size_t>(size), widest.size());
        expect_equal(std::memcmp(into.data(), widest.data(), widest.size()), 0);

        size = value_max_size_k;
        expect_true(store.select(stack->channel(), stored_key_t {2}, std::span<util::byte_t> {}, size).succeeded());
        expect_equal(static_cast<std::size_t>(size), std::size_t {0});
        expect_true(store.find(stored_key_t {2}, size) == lookup_result_t::written_k);
        expect_equal(static_cast<std::size_t>(size), std::size_t {0});
        if (round == 0) expect_true(store.flush(stack->channel()).succeeded());
    }

    // Key five was never read one at a time, so nothing of it sits in the value cache and the batch
    // reaches the run. An empty value is a write of no bytes, so reading it must take no block at all
    // rather than a page nobody wanted.
    std::array<stored_key_t, 1> const untouched {stored_key_t {5}};
    std::array<lookup_result_t, 1> answers {lookup_result_t::missing_k};
    std::uint32_t idlest = collection_config_k.request_buffers;
    std::size_t held = 0;
    auto watching = [&](std::size_t, std::uint64_t, std::span<util::byte_t const> bytes) noexcept {
        std::uint32_t const free_blocks = stack->channel().requests().available_buffers();
        if (free_blocks < idlest) idlest = free_blocks;
        held += bytes.size();
    };
    expect_true(store
                    .select(stack->channel(), std::span<stored_key_t const> {untouched},
                            std::span<lookup_result_t> {answers}, watching)
                    .succeeded());
    expect_true(answers[0] == lookup_result_t::written_k);
    expect_equal(held, std::size_t {0});
    expect_equal(idlest, collection_config_k.request_buffers);
}

/** A batch naming a key several times keeps its last delta: the last write whatever its
 *  width, an empty value before or after a wider one included, and a removal batch naming a
 *  key twice removes it. */
template <typename mount_type_>
void test_repeated_keys_keep_the_last(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    collection_config_t const config = test_config(0, value_max_size_k);
    auto [runs, runs_status] = make_run_pool<variable_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = variable_store_t::mount(
        *stack->volume, config, stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;

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
    expect_true(keys.resize(count) == st::status_t::success_k);
    if (!expect_true(sizes.resize(count) == st::status_t::success_k)) return;
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
    st::vector<util::byte_t> values;
    expect_true(values.resize(total) == st::status_t::success_k);
    for (std::size_t index = 0; index != values.size(); ++index) values[index] = painted(13, index);
    expect_true(store
                    .insert(stack->channel(), std::span<stored_key_t const> {keys},
                            std::span<util::byte_t const> {values}, std::span<stored_value_size_t const> {sizes})
                    .succeeded());

    auto holds = [&](stored_key_t stored_key, std::span<util::byte_t const> wanted) noexcept {
        stored_value_size_t size = 0;
        expect_true(store.find(stored_key, size) == lookup_result_t::written_k);
        expect_equal(static_cast<std::size_t>(size), wanted.size());
        std::array<util::byte_t, 8> into {};
        expect_true(store.select(stack->channel(), stored_key, std::span<util::byte_t> {into}, size).succeeded());
        expect_equal(std::memcmp(into.data(), wanted.data(), wanted.size()), 0);
    };
    auto last_of = [&](stored_key_t stored_key) noexcept {
        return std::span<util::byte_t const> {values}.subspan(last_offsets[stored_key], last_sizes[stored_key]);
    };
    for (stored_key_t stored_key = 1; stored_key != last_offsets.size(); ++stored_key)
        holds(stored_key, last_of(stored_key));

    std::array<stored_key_t, 4> const removed {4, 77777, 4, 1};
    expect_true(store.remove(stack->channel(), std::span<stored_key_t const> {removed}).succeeded());
    std::array<stored_key_t, 2> const rewritten {4, 4};
    std::array<stored_value_size_t, 2> const rewritten_sizes {2, 3};
    std::array<util::byte_t, 5> rewritten_values {};
    for (std::size_t index = 0; index != rewritten_values.size(); ++index) rewritten_values[index] = painted(14, index);
    expect_true(store
                    .insert(stack->channel(), std::span<stored_key_t const> {rewritten},
                            std::span<util::byte_t const> {rewritten_values},
                            std::span<stored_value_size_t const> {rewritten_sizes})
                    .succeeded());
    for (std::size_t round = 0; round != 2; ++round) {
        stored_value_size_t size = 0;
        expect_true(store.find(stored_key_t {1}, size) == lookup_result_t::removed_k);
        holds(2, last_of(2));
        holds(3, last_of(3));
        holds(4, std::span<util::byte_t const> {rewritten_values}.subspan(2, 3));
        if (round == 0) expect_true(store.flush(stack->channel()).succeeded());
    }
}

/** The value cache answers a version only with that version's bytes, never holds more than it was made with however
 *  many versions displace one another, and gives everything back when it dies. */
void test_value_cache_holds_what_it_took() {
    using cache_t = value_cache<stored_key_t, atomic_reference>;
    constexpr stored_value_size_t width_k = 300;
    constexpr std::size_t cache_bytes_k = 1024ull * 1024ull;
    resource_constraint_t budget {std::size_t {8} * 1024 * 1024};

    // A budget that affords entries must hold entries, and a slot is found by masking, so a power of two of them.
    std::optional<cache_t> cache {cache_t::of(cache_bytes_k, width_k, budget)};
    expect_true(cache->capacity() != 0);
    expect_true((cache->capacity() & (cache->capacity() - 1)) == 0);
    if (cache->capacity() == 0) return;
    std::size_t const slots_bytes = budget.used();

    st::vector<util::byte_t> value;
    st::vector<util::byte_t> copied;
    expect_true(value.resize(width_k) == st::status_t::success_k);
    expect_true(copied.resize(width_k) == st::status_t::success_k);
    auto fill = [&](std::size_t index, transaction_stamp_t stamp) noexcept {
        for (std::size_t position = 0; position != width_k; ++position)
            value[position] = painted(index + stamp, position);
        cache->add(static_cast<stored_key_t>(index), stamp, std::span<util::byte_t const> {value});
    };
    std::size_t mismatches = 0;
    auto check = [&](std::size_t index, transaction_stamp_t stamp) noexcept {
        stored_value_size_t size = 0;
        if (!cache->copy(static_cast<stored_key_t>(index), stamp, std::span<util::byte_t> {copied}, size)) return false;
        if (size != width_k) ++mismatches;
        for (std::size_t position = 0; position != size; ++position)
            if (copied[position] != painted(index + stamp, position)) ++mismatches;
        return true;
    };

    // Two versions of one key are two entries, and a stamp no version holds finds nothing.
    fill(7, 3);
    fill(7, 5);
    expect_true(check(7, 5));
    expect_true(!check(7, 4));

    // Four times as many versions as slots overwrite one another in place, within the bytes the cache was made with,
    // and whatever is still found is its own version.
    std::size_t const held = cache->capacity();
    std::size_t found = 0;
    for (std::size_t index = 0; index != 4 * held; ++index) fill(index, 1);
    for (std::size_t index = 0; index != 4 * held; ++index) found += check(index, 1) ? 1 : 0;
    expect_true(found != 0);
    expect_equal(mismatches, std::size_t {0});
    expect_true(budget.used() <= cache_bytes_k);

    // With the cache gone, the budget holds nothing.
    cache.reset();
    expect_true(slots_bytes != 0);
    expect_equal(budget.used(), std::size_t {0});

    // A budget too small for one entry, and a layout of no width, both hold nothing on purpose.
    expect_equal(cache_t::of(0, width_k, budget).capacity(), std::size_t {0});
    expect_equal(cache_t::of(1024ull * 1024ull, 0, budget).capacity(), std::size_t {0});
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
void test_batched_select_holds_its_depth(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t keys_k = 1024;
    constexpr std::size_t flushes_k = 4;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    collection_config_t const config = test_config(0, value_max_size_k);
    auto [runs, runs_status] = make_run_pool<variable_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = variable_store_t::mount(
        *stack->volume, config, stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(keys_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index);
        entry.state = lookup_result_t::written_k;
        // Values of about 2 KiB lie back to back, so even merged reads outnumber the queue's depth
        entry.bytes = 1 + index * 61 % 4093;
        entry.seed = index + 7;
        model[index] = entry;
    }
    for (std::size_t flush = 0; flush != flushes_k; ++flush) {
        st::vector<expectation_t> batch;
        for (std::size_t index = flush; index < keys_k; index += flushes_k)
            expect_true(batch.push_back(expectation_t {model[index]}) == st::status_t::success_k);
        write_model(store, stack->channel(), std::span<expectation_t const> {batch});
        if (!expect_true(store.flush(stack->channel()).succeeded())) return;
    }
    expect_equal(store.tree().levels().level(0).size(), flushes_k);

    st::vector<stored_key_t> keys;
    st::vector<lookup_result_t> results;
    expect_true(keys.resize(keys_k) == st::status_t::success_k);
    if (!expect_true(results.resize(keys_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != keys_k; ++index) keys[index] = model[index].stored_key;

    // The blocks a batched read holds are counted in the pool, since the ring performs a channel's
    // requests inside the poll that hands them to the sink.
    std::uint32_t idlest = collection_config_k.request_buffers;
    std::size_t mismatches = 0;
    auto counting = [&](std::size_t request, std::uint64_t offset, std::span<util::byte_t const> bytes) noexcept {
        std::uint32_t const free_blocks = stack->channel().requests().available_buffers();
        if (free_blocks < idlest) idlest = free_blocks;
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(model[request].seed, offset + index)) ++mismatches;
    };
    expect_true(store
                    .select(stack->channel(), std::span<stored_key_t const> {keys},
                            std::span<lookup_result_t> {results}, counting)
                    .succeeded());
    expect_equal(mismatches, std::size_t {0});
    expect_equal(collection_config_k.request_buffers - idlest, collection_config_k.queue_depth);
    for (std::size_t index = 0; index != keys_k; ++index) expect_true(results[index] == lookup_result_t::written_k);
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
void test_snapshot_holds_its_moment(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t bytes_k = 900;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    collection_config_t const config = test_config(0, value_max_size_k);
    auto [runs, runs_status] = make_run_pool<variable_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = variable_store_t::mount(
        *stack->volume, config, stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;

    st::vector<util::byte_t> older;
    st::vector<util::byte_t> newer;
    expect_true(older.resize(bytes_k) == st::status_t::success_k);
    expect_true(newer.resize(bytes_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != bytes_k; ++index) older[index] = painted(11, index);
    for (std::size_t index = 0; index != bytes_k; ++index) newer[index] = painted(12, index);

    expect_true(store.insert(stack->channel(), stored_key_t {42}, std::span<util::byte_t const> {older}).succeeded());
    stamp_pin_t pin;
    transaction_stamp_t const pinned = stack->ledger.pin_reader(pin);

    std::thread writer {[&]() noexcept {
        auto [guard, acquire_status] = stack->fixture->channel(1);
        if (!expect_true(acquire_status.succeeded())) return;
        expect_true(store.insert(*guard, stored_key_t {42}, std::span<util::byte_t const> {newer}).succeeded());
        expect_true(store.flush(*guard).succeeded());
    }};

    for (std::size_t round = 0; round != 2; ++round) {
        st::vector<util::byte_t> into;
        expect_true(into.resize(bytes_k) == st::status_t::success_k);
        stored_value_size_t size = 0;
        expect_true(store.select(stack->channel(), stored_key_t {42}, std::span<util::byte_t> {into}, size, pinned)
                        .succeeded());
        expect_equal(static_cast<std::size_t>(size), bytes_k);
        expect_equal(std::memcmp(into.data(), older.data(), bytes_k), 0);
        // The second read follows the flush, which kept the pinned version in the run it wrote.
        if (round == 0) writer.join();
    }
    pin.release();

    st::vector<util::byte_t> into;
    expect_true(into.resize(bytes_k) == st::status_t::success_k);
    stored_value_size_t size = 0;
    expect_true(store.select(stack->channel(), stored_key_t {42}, std::span<util::byte_t> {into}, size).succeeded());
    expect_equal(static_cast<std::size_t>(size), bytes_k);
    expect_equal(std::memcmp(into.data(), newer.data(), bytes_k), 0);
}

/** Reads each key of @p wanted one at a time at @p stamp, counting where the model disagrees. */
template <typename store_type_>
void check_model_as_of(store_type_& store, util::io_channel_t& channel, std::span<expectation_t const> wanted,
                       transaction_stamp_t stamp) {
    st::vector<util::byte_t> into;
    for (expectation_t const& entry : wanted) {
        stored_value_size_t size = 0;
        expect_true(into.resize(static_cast<std::size_t>(entry.bytes)) == st::status_t::success_k);
        util::status_t const read = store.select(channel, entry.stored_key, std::span<util::byte_t> {into}, size,
                                                 stamp);
        if (entry.state != lookup_result_t::written_k) {
            expect_true(read == util::status_code_t::not_found_k);
            continue;
        }
        expect_true(read.succeeded());
        expect_equal(static_cast<std::uint64_t>(size), entry.bytes);
        for (std::uint64_t position = 0; position != size; ++position)
            expect_true(into[static_cast<std::size_t>(position)] == painted(entry.seed, position));
    }
}

/** Walks @p store at its head ascending from @p lowest and descending from @p highest, once reading
 *  a page at a time and once through one reading, and checks the keys and sizes each walk hands
 *  out, in its order, against what @p wanted, indexed by key, says is written. */
template <typename cursor_type_, typename store_type_>
void check_scan(store_type_ const& store, std::span<expectation_t const> wanted,
                stored_key_t lowest = limit<stored_key_t>::min_k, stored_key_t highest = limit<stored_key_t>::max_k) {
    for (scan_direction_t const direction : {scan_direction_t::ascending_k, scan_direction_t::descending_k}) {
        bool const descending = direction == scan_direction_t::descending_k;
        stored_key_t const from = descending ? highest : lowest;
        std::size_t present = 0;
        for (expectation_t const& entry : wanted)
            present += entry.state == lookup_result_t::written_k &&
                       (descending ? !(from < entry.stored_key) : !(entry.stored_key < from));
        auto const check = [&](cursor_type_& scan, util::status_t scan_status) {
            expect_true(scan_status.succeeded());
            if (scan_status.failed()) return;
            std::size_t listed = 0;
            stored_key_t previous = 0;
            for (; !scan.reached_end(); expect_true(scan.advance().succeeded())) {
                if (scan.state() != lookup_result_t::written_k) continue;
                stored_key_t const key = scan.stored_key();
                if (listed != 0) expect_true(descending ? key < previous : previous < key);
                previous = key;
                ++listed;
                bool const written = key < wanted.size() &&
                                     wanted[static_cast<std::size_t>(key)].state == lookup_result_t::written_k;
                expect_true(written);
                if (written)
                    expect_equal(static_cast<std::uint64_t>(scan.size()), wanted[static_cast<std::size_t>(key)].bytes);
            }
            expect_equal(listed, present);
        };
        {
            util::local_arena_t walk;
            auto [scan, scan_status] = cursor_type_::over(store, walk.allocator().arena, from, head_stamp_k, 0,
                                                          direction);
            check(scan, scan_status);
        }
        util::local_arena_t walk;
        typename store_type_::reader_t const inside = store.enter();
        auto [scan, scan_status] = cursor_type_::over(store, store.reading(head_stamp_k), walk.allocator().arena, from,
                                                      direction);
        check(scan, scan_status);
    }
}

/** A range removal hides every older version of its keys, in memory runs, after the flush that
 *  writes it out and after merges hold it down, while a snapshot pinned before it keeps reading
 *  them and a write after it survives. */
template <typename mount_type_>
void test_range_removal_hides_older_runs(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using variable_cursor_t = typename types_t::variable_cursor_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t keys_k = 300;
    constexpr stored_key_t start_k = 50;
    constexpr stored_key_t end_k = 250;
    constexpr stored_key_t rewritten_k = 100;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    // Two runs fill level zero, so every second flush leaves a merge due.
    collection_config_t config = test_config(0, value_max_size_k);
    config.lsm_tree.level_enlarge_factor = 2;
    auto [runs, runs_status] = make_run_pool<variable_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = variable_store_t::mount(
        *stack->volume, config, stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    auto [writing, acquire_writing_status] = stack->fixture->channel(1);
    expect_true(acquire_writing_status.succeeded());
    if (store_status.failed() || acquire_writing_status.failed()) return;

    st::vector<expectation_t> before;
    if (!expect_true(before.resize(keys_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != keys_k; ++index)
        before[index] = expectation_t {static_cast<stored_key_t>(index), lookup_result_t::written_k, 9 + index % 31,
                                       index * 13 + 1};
    // The lower keys sink a level below the upper ones, so the tombstone hides runs of two levels.
    std::span<expectation_t const> const all {before};
    write_model(store, stack->channel(), all.first(100));
    expect_true(store.flush(stack->channel()).succeeded());
    write_model(store, stack->channel(), all.subspan(100, 100));
    expect_true(store.flush(stack->channel()).succeeded());
    std::array<typename variable_store_t::merge_buffers_t, 1> buffers {};
    auto const [sunk, sunk_status] = store.compact(stack->channel(), *writing, buffers, {});
    expect_true(sunk_status.succeeded() && sunk == compaction_outcome_t::published_k);
    write_model(store, stack->channel(), all.subspan(200));
    expect_true(store.flush(stack->channel()).succeeded());

    stamp_pin_t pin;
    transaction_stamp_t const pinned = stack->ledger.pin_reader(pin);
    expect_true(store.remove_range(stack->channel(), start_k, end_k).succeeded());
    st::vector<expectation_t> after;
    if (!expect_true(after.resize(keys_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != keys_k; ++index) {
        after[index] = before[index];
        if (index >= start_k && index < end_k) after[index] = expectation_t {before[index].stored_key};
        if (after[index].state == lookup_result_t::missing_k) after[index].state = lookup_result_t::removed_k;
    }
    expectation_t const rewritten {rewritten_k, lookup_result_t::written_k, 17, 77};
    write_model(store, stack->channel(), std::span<expectation_t const> {&rewritten, 1});
    after[rewritten_k] = rewritten;

    auto const check = [&]() {
        check_model(store, stack->channel(), std::span<expectation_t const> {after});
        check_model_as_of(store, stack->channel(), std::span<expectation_t const> {after}, head_stamp_k);
        check_model_as_of(store, stack->channel(), std::span<expectation_t const> {before}, pinned);
        check_scan<variable_cursor_t>(store, std::span<expectation_t const> {after});
    };
    check();
    expect_true(store.flush(stack->channel()).succeeded());
    check();
    for (std::size_t pass = 0; pass != 4 && store.tree().levels().level(0).size() != 0; ++pass) {
        auto const [merged, merged_status] = store.compact(stack->channel(), *writing, buffers, {});
        expect_true(merged_status.succeeded() && merged == compaction_outcome_t::published_k);
        check();
    }

    // With the pin gone, a merge into the bottom drops the tombstone with every version it hid.
    pin.release();
    for (std::size_t flush = 0; flush != 2; ++flush) {
        write_model(store, stack->channel(), std::span<expectation_t const> {&rewritten, 1});
        expect_true(store.flush(stack->channel()).succeeded());
    }
    auto const [dropped, dropped_status] = store.compact(stack->channel(), *writing, buffers, {});
    expect_true(dropped_status.succeeded() && dropped == compaction_outcome_t::published_k);
    std::uint64_t tombstones = 0;
    for (std::size_t number = 0; number != store.tree().levels().size(); ++number)
        tombstones += store.tree().levels().level(number).range_tombstones_count();
    expect_equal(tombstones, std::uint64_t {0});
    for (expectation_t& entry : after)
        if (entry.state == lookup_result_t::removed_k) entry.state = lookup_result_t::missing_k;
    check_model(store, stack->channel(), std::span<expectation_t const> {after});
    check_scan<variable_cursor_t>(store, std::span<expectation_t const> {after});
}

#pragma endregion Snapshot Cases

#pragma region Scan Cases

/** A scan visits every key once, in ascending order, across the memory runs and two levels. */
template <typename mount_type_>
void test_scan_visits_every_key_in_order(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using variable_cursor_t = typename types_t::variable_cursor_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t keys_k = 1000;
    constexpr std::size_t rounds_k = 4;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    collection_config_t const config = test_config(0, value_max_size_k);
    auto [runs, runs_status] = make_run_pool<variable_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = variable_store_t::mount(
        *stack->volume, config, stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(keys_k) == st::status_t::success_k)) return;
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
            if (!expect_true(batch.push_back(expectation_t {entry}) == st::status_t::success_k)) return;
        }
        write_model(store, stack->channel(), std::span<expectation_t const> {batch});
        if (!expect_true(store.flush(stack->channel()).succeeded())) return;
        apply(std::span<expectation_t const> {batch});
    }

    // A merge needs a channel of its own to write on, since both pipelines poll into the same room.
    auto [writing, acquire_writing_status] = stack->fixture->channel(1);
    expect_true(acquire_writing_status.succeeded());
    if (acquire_writing_status.failed()) return;
    std::array<typename variable_store_t::merge_buffers_t, 1> buffers {};
    auto const [merged, compact_status] = store.compact(stack->channel(), *writing, buffers, {});
    expect_true(compact_status.succeeded());
    if (compact_status.failed()) return;
    expect_true(merged == compaction_outcome_t::published_k);
    expect_equal(store.tree().levels().size(), std::size_t {2});

    // Two more flushes put runs back on level zero, and they hold the same keys, so the scan has to
    // pick the younger of two runs of one level as well as the shallower of two levels.
    for (std::size_t again = 0; again != 2; ++again) {
        st::vector<expectation_t> batch;
        for (std::size_t index = 0; index < keys_k; index += 10) {
            expectation_t entry;
            entry.stored_key = static_cast<stored_key_t>(index);
            entry.state = lookup_result_t::written_k;
            entry.bytes = 7 + (index + again * 11) % 53;
            entry.seed = index * 17 + 5 + again;
            expect_true(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
        }
        write_model(store, stack->channel(), std::span<expectation_t const> {batch});
        expect_true(store.flush(stack->channel()).succeeded());
        apply(std::span<expectation_t const> {batch});
    }
    expect_equal(store.tree().levels().level(0).size(), std::size_t {2});

    // What stays in memory runs is what the scan must merge in front of every run.
    {
        st::vector<expectation_t> batch;
        for (std::size_t index = 0; index < keys_k; index += 25) {
            expectation_t entry;
            entry.stored_key = static_cast<stored_key_t>(index);
            entry.state = index % 50 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
            entry.bytes = entry.state == lookup_result_t::written_k ? 13 + index % 29 : 0;
            entry.seed = index * 7 + 3;
            expect_true(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
        }
        write_model(store, stack->channel(), std::span<expectation_t const> {batch});
        apply(std::span<expectation_t const> {batch});
    }
    expect_true(store.memory_deltas_count() != 0);

    util::local_arena_t walk;
    auto [scan, scan_status] = variable_cursor_t::over(store, walk.allocator().arena);
    expect_true(scan_status.succeeded());
    if (scan_status.failed()) return;

    // The walk is bounded so a merge that forgets to advance a cursor is a result and not a hang.
    constexpr std::size_t visits_limit_k = keys_k + 64;
    std::size_t visited = 0;
    std::size_t out_of_order = 0;
    std::size_t wrong_state = 0;
    std::size_t wrong_size = 0;
    stored_key_t previous = 0;
    while (!scan.reached_end() && visited != visits_limit_k) {
        if (visited != 0 && !(previous < scan.stored_key())) ++out_of_order;
        previous = scan.stored_key();
        expectation_t const& wanted = model[static_cast<std::size_t>(previous)];
        if (scan.state() != wanted.state) ++wrong_state;
        if (static_cast<std::uint64_t>(scan.size()) != wanted.bytes) ++wrong_size;
        // The scan holds no epoch between pages, so its own thread commits while it walks, past its pinned stamp.
        if (visited == 0)
            expect_true(store.remove(stack->channel(), static_cast<stored_key_t>(keys_k - 1)).succeeded());
        ++visited;
        expect_true(scan.advance().succeeded());
    }
    expect_equal(visited, keys_k);
    expect_equal(out_of_order, std::size_t {0});
    expect_equal(wrong_state, std::size_t {0});
    expect_equal(wrong_size, std::size_t {0});
    model[keys_k - 1].state = lookup_result_t::removed_k;
    model[keys_k - 1].bytes = 0;

    // The newest version of every key must also be the one a read of it brings back.
    check_model(store, stack->channel(), std::span<expectation_t const> {model});
}

/** A scan walks the disjoint runs of a sorted level one after another, under the overlapping runs
 *  of level zero and the memory runs, while removals and range removals straddle their cuts. */
template <typename mount_type_>
void test_scan_crosses_the_runs_of_a_sorted_level(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using variable_store_t = typename types_t::variable_store_t;
    using variable_cursor_t = typename types_t::variable_cursor_t;
    using stack_t = typename types_t::stack_t;
    using keyed_delta_t = typename variable_store_t::keyed_delta_t;
    constexpr std::size_t ingests_k = 3;
    constexpr std::size_t ingest_keys_k = 1000;
    constexpr std::size_t keys_k = ingests_k * ingest_keys_k + 2;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    auto [runs, runs_status] = make_run_pool<variable_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = variable_store_t::mount(*stack->volume, test_config(0, value_max_size_k),
                                                         stack->fixture->resources->memory_constraint, stack->ledger,
                                                         runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(keys_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != keys_k; ++index) model[index].stored_key = static_cast<stored_key_t>(index);
    auto const write = [&](std::initializer_list<expectation_t> batch) {
        write_model(store, stack->channel(), std::span<expectation_t const> {batch.begin(), batch.size()});
        for (expectation_t const& entry : batch) model[static_cast<std::size_t>(entry.stored_key)] = entry;
    };
    auto const remove_range = [&](stored_key_t start, stored_key_t end) {
        expect_true(store.remove_range(stack->channel(), start, end).succeeded());
        // A range removal leaves a key never written missing, as a read of it answers.
        for (stored_key_t key = start; key != end; ++key)
            if (model[static_cast<std::size_t>(key)].state != lookup_result_t::missing_k)
                model[static_cast<std::size_t>(key)] = expectation_t {key, lookup_result_t::removed_k};
    };

    // Each ingest of even keys lands past the one before, which no level holds, so the ingests
    // sit side by side on level one.
    st::vector<util::byte_t> values;
    st::vector<keyed_delta_t> rows;
    for (std::size_t ingest = 0; ingest != ingests_k; ++ingest) {
        std::size_t bytes = 0;
        for (std::size_t key = ingest * ingest_keys_k; key != (ingest + 1) * ingest_keys_k; key += 2) {
            model[key] = expectation_t {key, lookup_result_t::written_k, 9 + key % 31, key * 13 + 1};
            bytes += static_cast<std::size_t>(model[key].bytes);
        }
        expect_true(values.resize(bytes) == st::status_t::success_k);
        if (!expect_true(rows.resize(ingest_keys_k / 2) == st::status_t::success_k)) return;
        std::size_t offset = 0;
        for (std::size_t index = 0; index != rows.size(); ++index) {
            expectation_t const& entry = model[ingest * ingest_keys_k + index * 2];
            std::span<util::byte_t> const value = std::span<util::byte_t> {values}.subspan(offset, entry.bytes);
            paint_into(entry, value);
            rows[index] = keyed_delta_t {entry.stored_key, util::delta_view_t {util::delta_kind_t::write_k, value}};
            offset += value.size();
        }
        typename variable_store_t::ingest_runs_t ingested = store.begin_ingest();
        expect_true(store.write_ingest(stack->channel(), std::span<keyed_delta_t const> {rows}, ingested).succeeded());
        expect_true(store.publish_ingest(stack->channel(), ingested).has_value());
    }
    expect_equal(store.tree().levels().size(), std::size_t {2});
    expect_equal(store.tree().levels().level(1).size(), ingests_k);

    // Two flushes over both cuts: the last key of one run and the first of the next go, odd keys
    // between the runs arrive, and a range removal hides a cut but for one key written later.
    write({{998, lookup_result_t::removed_k},
           {1000, lookup_result_t::removed_k},
           {999, lookup_result_t::written_k, 21, 5},
           {1001, lookup_result_t::written_k, 22, 6},
           {1999, lookup_result_t::written_k, 23, 7}});
    expect_true(store.flush(stack->channel()).succeeded());
    remove_range(1990, 2010);
    write({{2004, lookup_result_t::written_k, 24, 8}, {1000, lookup_result_t::written_k, 25, 9}});
    expect_true(store.flush(stack->channel()).succeeded());
    expect_equal(store.tree().levels().level(0).size(), std::size_t {2});

    // The memory runs remove across the first cut again, rewrite a key inside it, and write one
    // key past every run.
    remove_range(996, 1004);
    write({{1002, lookup_result_t::written_k, 26, 10},
           {2998, lookup_result_t::removed_k},
           {3001, lookup_result_t::written_k, 27, 11}});
    expect_true(store.memory_deltas_count() != 0);

    std::span<expectation_t const> const wanted {model};
    check_scan<variable_cursor_t>(store, wanted);
    check_scan<variable_cursor_t>(store, wanted, 1000, 999);
    check_scan<variable_cursor_t>(store, wanted, 2000, 1998);
    check_model(store, stack->channel(), wanted);
}

#pragma endregion Scan Cases

#pragma region Fold Cases

/** A counter of 8 bytes, which a write sets, a removal clears, and a change adds its own 8 bytes to. */
struct counter_fold {
    static std::uint64_t count_of(std::span<util::byte_t const> bytes) noexcept {
        std::uint64_t count = 0;
        if (!bytes.empty()) std::memcpy(&count, bytes.data(), fu::min_of(bytes.size(), sizeof(count)));
        return count;
    }
    static util::expected<std::span<util::byte_t const>> counted(std::uint64_t count,
                                                                 util::arena_allocator<util::byte_t>& arena) noexcept {
        util::byte_t* const bytes = arena.allocate(sizeof(count));
        if (!bytes) return util::status_t {util::status_code_t::out_of_memory_k, "No room for a count"};
        std::memcpy(bytes, &count, sizeof(count));
        return std::span<util::byte_t const> {bytes, sizeof(count)};
    }
    util::expected<std::span<util::byte_t const>> apply(std::span<util::byte_t const> base,
                                                        std::span<util::delta_view_t const> deltas,
                                                        util::arena_allocator<util::byte_t>& arena) const noexcept {
        std::uint64_t count = count_of(base);
        for (util::delta_view_t const& delta : deltas) count += count_of(delta.bytes);
        return counted(count, arena);
    }
    util::expected<std::span<util::byte_t const>> compose(std::span<util::delta_view_t const> deltas,
                                                          util::arena_allocator<util::byte_t>& arena) const noexcept {
        return apply({}, deltas, arena);
    }
    bool closes(util::delta_view_t const& delta) const noexcept { return delta.kind != util::delta_kind_t::change_k; }
};

/** Whether a round of reads also checks what @c find answers, which a flushed level's value needs a
 *  read for. */
enum class find_answers_t : bool { unchecked_k, checked_k };

/** Counters folded from change chains read as a map says, while the chains sit in memory runs, once a merge folds
 *  them, once two flushes leave them in level zero, and once a compaction applies them at the bottom level. */
template <typename mount_type_>
void test_counter_fold_reads_alike_everywhere(mount_type_ const& mount) {
    using types_t = collection_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using counter_store_t = nvme_store<variable_slot_t, typename types_t::volume_t, atomic_reference, waiter_t,
                                       counter_fold, no_device_fold_t>;
    constexpr stored_key_t keys_k = 12;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;

    // A level zero due at two runs, which the two flushes below fill.
    collection_config_t config = test_config(0, value_max_size_k);
    config.memory_runs_max_bytes = 64;
    config.lsm_tree.level_enlarge_factor = 2;
    auto [runs, runs_status] = make_run_pool<counter_store_t>(*stack);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [store, store_status] = counter_store_t::mount(
        *stack->volume, config, stack->fixture->resources->memory_constraint, stack->ledger, runs);
    expect_true(store_status.succeeded());
    if (store_status.failed()) return;

    // Key zero only ever changes, so its chain has no base until the bottom level folds it onto nothing.
    std::map<stored_key_t, std::optional<std::uint64_t>> model;
    auto commit_round = [&](std::uint64_t round) {
        std::array<std::uint64_t, keys_k> words {};
        std::array<keyed_delta<stored_key_t>, keys_k> deltas {};
        for (stored_key_t key = 0; key != keys_k; ++key) {
            std::uint64_t const operation = key == 0 ? 2 : (key * 7 + round) % 5;
            std::optional<std::uint64_t>& counted = model[key];
            util::delta_view_t delta {util::delta_kind_t::change_k, {}};
            if (operation == 0) {
                delta.kind = util::delta_kind_t::write_k;
                words[key] = round * 1000 + key;
                counted = words[key];
            }
            else if (operation == 1) {
                delta.kind = util::delta_kind_t::remove_k;
                counted.reset();
            }
            else {
                words[key] = key + round + 1;
                counted = counted.value_or(0) + words[key];
            }
            if (delta.kind != util::delta_kind_t::remove_k)
                delta.bytes = std::as_bytes(std::span<std::uint64_t const, 1> {&words[key], 1});
            deltas[key] = keyed_delta<stored_key_t> {key, delta};
        }
        auto [sealed, seal_status] = store.seal(std::span<keyed_delta<stored_key_t>> {deltas});
        memory_run_claim_t claimed;
        if (seal_status.failed() || store.try_claim(claimed).failed()) return expect_true(false);
        return expect_true(store.commit(sealed, std::move(claimed)).succeeded());
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
            util::status_t const selected = store.select(
                stack->channel(), key, std::as_writable_bytes(std::span<std::uint64_t, 1> {&count, 1}), size);
            wrong += wanted ? selected.failed() || size != sizeof(count) || count != *wanted
                            : !(selected == util::status_code_t::not_found_k);
            lookup_result_t const found = store.find(key, size);
            if (finds == find_answers_t::checked_k)
                wrong += wanted ? found != lookup_result_t::written_k || size != sizeof(count)
                                : found == lookup_result_t::written_k;
        }
        auto sink = [&](std::size_t request, std::uint64_t, std::span<util::byte_t const> bytes) noexcept {
            batched[request] = counter_fold::count_of(bytes);
        };
        expect_true(store
                        .select(stack->channel(), std::span<stored_key_t const> {keys},
                                std::span<lookup_result_t> {answers}, sink)
                        .succeeded());
        for (stored_key_t key = 0; key != keys_k + 1; ++key) {
            std::optional<std::uint64_t> const wanted = model.count(key) ? model[key] : std::nullopt;
            wrong += wanted ? answers[key] != lookup_result_t::written_k || batched[key] != *wanted
                            : answers[key] == lookup_result_t::written_k;
        }
        return expect_equal(wrong, std::size_t {0});
    };

    for (std::uint64_t round = 0; round != 8; ++round) commit_round(round);
    check(find_answers_t::checked_k);
    expect_true(store.merge_memory_runs().succeeded());
    expect_true(store.memory_shape().runs < 8);
    check(find_answers_t::checked_k);

    for (std::uint64_t round = 8; round != 12; ++round) commit_round(round);
    expect_true(store.flush(stack->channel()).succeeded());
    for (std::uint64_t round = 12; round != 14; ++round) commit_round(round);
    expect_true(store.flush(stack->channel()).succeeded());
    commit_round(14);
    // Key zero's chain reaches level zero as changes in both runs, so its reads fold through their values.
    auto const& flushed = store.tree().levels().level(0);
    for (std::size_t index = 0; index != flushed.size(); ++index)
        expect_true(holds_changes(&flushed.run(index).kinds()));
    expect_equal(flushed.size(), std::size_t {2});
    check(find_answers_t::unchecked_k);

    auto [writing, acquire_writing_status] = stack->fixture->channel(1);
    expect_true(acquire_writing_status.succeeded());
    if (acquire_writing_status.failed()) return;
    std::array<typename counter_store_t::merge_buffers_t, 1> buffers {};
    auto const [compacted, compact_status] = store.compact(stack->channel(), *writing, buffers, {});
    expect_true(compact_status.succeeded() && compacted == compaction_outcome_t::published_k);
    auto const& levels = store.tree().levels();
    expect_true(levels.size() == 2 && levels.level(0).empty());
    for (std::size_t index = 0; levels.size() == 2 && index != levels.level(1).size(); ++index)
        expect_true(!holds_changes(&levels.level(1).run(index).kinds()));
    check(find_answers_t::unchecked_k);
}

#pragma endregion Fold Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;

    start_test_run(argc, argv);

    auto [directory, directory_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(directory_status)) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }

    run_test_case("test_value_cache_holds_what_it_took", test_value_cache_holds_what_it_took);
    auto const cases = [&](auto const& mount) {
        report_on_mount(mount, "test_mixed_widths_round_trip", test_mixed_widths_round_trip);
        report_on_mount(mount, "test_every_layout_round_trips", test_every_layout_round_trips);
        report_on_mount(mount, "test_misses_and_remove_deltas", test_misses_and_remove_deltas);
        report_on_mount(mount, "test_memory_runs_answer_before_the_tree", test_memory_runs_answer_before_the_tree);
        report_on_mount(mount, "test_full_memory_runs_flush_themselves", test_full_memory_runs_flush_themselves);
        report_on_mount(mount, "test_flush_past_one_slot_cuts_runs", test_flush_past_one_slot_cuts_runs);
        report_on_mount(mount, "test_burst_past_records_lands_every_commit",
                        test_burst_past_records_lands_every_commit);
        report_on_mount(mount, "test_unaligned_exact_span_is_taken", test_unaligned_exact_span_is_taken);
        report_on_mount(mount, "test_widest_and_empty_values", test_widest_and_empty_values);
        report_on_mount(mount, "test_repeated_keys_keep_the_last", test_repeated_keys_keep_the_last);
        report_on_mount(mount, "test_batched_select_holds_its_depth", test_batched_select_holds_its_depth);
        report_on_mount(mount, "test_snapshot_holds_its_moment", test_snapshot_holds_its_moment);
        report_on_mount(mount, "test_range_removal_hides_older_runs", test_range_removal_hides_older_runs);
        report_on_mount(mount, "test_scan_visits_every_key_in_order", test_scan_visits_every_key_in_order);
        report_on_mount(mount, "test_scan_crosses_the_runs_of_a_sorted_level",
                        test_scan_crosses_the_runs_of_a_sorted_level);
        report_on_mount(mount, "test_counter_fold_reads_alike_everywhere", test_counter_fold_reads_alike_everywhere);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(transport, directory.path(), collection_config_k, 1, "collection", cases);

    return finish_test_run();
}
