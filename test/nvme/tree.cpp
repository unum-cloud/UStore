/**
 *  @file test/nvme/tree.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The LSM level and the tree over the ring: what a flush publishes, what a merge keeps, and how deep it reads.
 *
 *  The results this file exists for are @c cancelled_leaves_nothing and @c holds_its_depth. A
 *  cancelled compaction must leave the level set it was replacing live, every block of both
 *  channels back in its pool and not one byte of the volume reserved, since a merge that leaks
 *  either is worse than one that never ran. A batched read must sit at the queue pair's depth
 *  through the middle of a run rather than at the size of whatever batch a caller handed it.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcmp`

#include <array>   // `std::array`
#include <memory>  // `std::addressof`, `std::unique_ptr`
#include <span>    // `std::span`
#include <thread>  // `std::thread`
#include <utility> // `std::move`

#include <forkunion/capabilities.hpp>     // `fu::preferred_yield_t`
#include <smashtable/basic_vector.hpp>    // `st::vector`
#include <smashtable/reference_store.hpp> // `st::reference_store`

#include "ustore/blobs/nvme_key_slot.hpp"          // `key_slot`
#include "ustore/blobs/nvme_tree.hpp"              // `lsm_level`, `lsm_level_order_t`, `lsm_level_depth_t`
#include "ustore/blobs/vram_nvidia_compaction.cuh" // `cuda_compactor_t`, `strings_merge_verdict`
#include "ustore/utilities/ram_device.hpp"         // `executor_t`, `pool_view_t`
#include "ustore/utilities/vram_device.hpp"        // `gpu_drivers_t`, `kernel_tables`, `vram_nvidia_bundle`

#include "nvme/allocation_watch.hpp" // `allocation_watch_t`, `allocation_watch_g`
#include "nvme/fixture.hpp"          // `run_on_transport`, `expectation_t`, `painted`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Shape

/** The numbers every case mounts with, which are the defaults: channels enough for a compaction to hold two at once. */
constexpr fixture_config_t lsm_config_k {};

/** The slot every case sizes its levels and runs by, kept small so a few flushes
 *  fill level zero. */
constexpr std::uint64_t slot_bytes_k = 64ull * 1024ull;

/** Run records every case's pool holds, far more than any case keeps at once. */
constexpr std::size_t run_records_k = 4096;

/** The largest value the collection under test admits. */
constexpr stored_value_size_t value_max_size_k = 128ull * 1024ull;

/** The keys and the key slots every case of this suite drives, the engine's own, so its device
 *  merges run on the kernels the library's bundle carries. */
using stored_key_t = st::key128_t;
using variable_slot_t = key_slot<stored_key_t, value_layout_t::variable_width_k>;

/** The key of integer @p index, as the engine reads an identity below 2^64. */
constexpr stored_key_t key_of(std::uint64_t index) noexcept { return {0, index}; }

/** The runs, the levels and the trees every case drives over the volume of @p fixture_type_. */
template <typename fixture_type_>
struct lsm_types {
    using fixture_t = fixture_type_;
    using guard_t = typename fixture_t::guard_t;
    using volume_t = typename fixture_t::volume_t;
    using sorted_run_t = lsm_sorted_run<variable_slot_t, volume_t, waiter_t>;
    using level_t = lsm_level<variable_slot_t, volume_t, atomic_reference, waiter_t>;
    using run_pool_t = typename level_t::run_pool_t;
    using run_handle_t = typename level_t::run_handle_t;
    using merger_t = lsm_level_merger<variable_slot_t, volume_t, atomic_reference, waiter_t>;
    using tree_t = lsm_tree<variable_slot_t, volume_t, atomic_reference, waiter_t>;
};

/** The tree shape every case mounts, whose first level fills after a handful of flushes. */
[[nodiscard]] lsm_tree_config_t test_tree_config() noexcept {
    lsm_tree_config_t config;
    config.lsm_level_0.sorted_run.value_max_size = value_max_size_k;
    config.level_enlarge_factor = 4;
    return config;
}

#pragma endregion Shape

#pragma region Model

/** One stamp per key slot of a flush of @p count distinct keys, all newer than every stamp handed out before. */
inline st::vector<std::uint64_t> fresh_stamps(std::size_t count) {
    static std::uint64_t drawn = 0;
    st::vector<std::uint64_t> stamps;
    [[maybe_unused]] st::status_t const sized = stamps.resize(count);
    for (std::size_t index = 0; index != stamps.size(); ++index) stamps[index] = ++drawn;
    return stamps;
}

/** The key slots and the bytes behind them that one flush writes. */
struct batch_t {

    /** Key slots in ascending order of their keys. */
    st::vector<variable_slot_t> key_slots;

    /** The values back to back, in the order of their key slots. */
    st::vector<byte_t> values;

    /** Where each value starts inside @c values and one past the last. */
    st::vector<stored_value_offset_t> offsets;

    /** The @c delta_kind_t of every key slot. */
    st::vector<std::uint8_t> kinds;
};

/** Lays @p wanted out as one run's key slots, kinds and values, which must already be in ascending key order. */
[[nodiscard]] std::size_t build_batch(std::span<expectation_t const> wanted, batch_t& into) {
    std::size_t failures = 0;
    failures += expect(into.key_slots.resize(wanted.size()) == st::status_t::success_k);
    failures += expect(into.kinds.resize(wanted.size()) == st::status_t::success_k);
    failures += expect(into.offsets.resize(wanted.size() + 1) == st::status_t::success_k);
    if (failures != 0) return failures;

    std::uint64_t total = 0;
    for (std::size_t index = 0; index != wanted.size(); ++index)
        if (wanted[index].state == lookup_result_t::written_k) total += wanted[index].bytes;
    failures += expect(into.values.resize(static_cast<std::size_t>(total)) == st::status_t::success_k);
    if (failures != 0) return failures;

    std::uint64_t written = 0;
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        expectation_t const& entry = wanted[index];
        bool const removed = entry.state == lookup_result_t::removed_k;
        variable_slot_t slot;
        slot.stored_key = key_of(entry.stored_key);
        into.offsets[index] = static_cast<stored_value_offset_t>(written);
        into.kinds[index] = static_cast<std::uint8_t>(removed ? delta_kind_t::remove_k : delta_kind_t::write_k);
        if (!removed) {
            for (std::uint64_t byte = 0; byte != entry.bytes; ++byte)
                into.values[static_cast<std::size_t>(written + byte)] = painted(entry.seed, byte);
            written += entry.bytes;
        }
        into.key_slots[index] = slot;
    }
    into.offsets[wanted.size()] = static_cast<stored_value_offset_t>(written);
    return failures;
}

/** Flushes @p wanted onto level zero of @p tree as one run. */
template <typename tree_type_>
[[nodiscard]] std::size_t flush_batch(tree_type_& tree, io_channel_t& channel, std::span<expectation_t const> wanted) {
    std::size_t failures = 0;
    batch_t batch;
    failures += build_batch(wanted, batch);
    if (failures != 0) return failures;
    failures += expect(tree.flush(channel, std::span<variable_slot_t const> {batch.key_slots},
                                  std::span<std::uint64_t const> {fresh_stamps(batch.key_slots.size())},
                                  std::span<std::uint8_t const> {batch.kinds}, 0,
                                  std::span<byte_t const> {batch.values},
                                  std::span<stored_value_offset_t const> {batch.offsets}, value_max_size_k)
                           .succeeded());
    return failures;
}

/** Reads every key of @p wanted out of @p tree and counts what it does not answer as the model says. */
template <typename tree_type_>
[[nodiscard]] std::size_t check_model(tree_type_ const& tree, io_channel_t& channel,
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
        keys[index] = key_of(wanted[index].stored_key);
        results[index] = lookup_result_t::missing_k;
        seen[index] = 0;
    }

    std::size_t mismatches = 0;
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        seen[request] += bytes.size();
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(wanted[request].seed, offset + index)) ++mismatches;
    };

    typename tree_type_::level_set_t const& taken = tree.levels();
    failures += expect(tree.read_values(channel, taken, std::span<stored_key_t const> {keys},
                                        std::span<lookup_result_t> {results}, sink)
                           .succeeded());
    failures += expect_eq(mismatches, std::size_t {0});
    for (std::size_t index = 0; index != count; ++index) {
        switch (wanted[index].state) {
        case lookup_result_t::missing_k:
        case lookup_result_t::removed_k: failures += expect(results[index] == wanted[index].state); break;
        case lookup_result_t::written_k:
            failures += expect(results[index] == lookup_result_t::written_k);
            failures += expect_eq(seen[index], wanted[index].bytes);
            break;
        // A generated key never asks for a short read or a change, so these states mean the generator broke.
        case lookup_result_t::changed_k:
        case lookup_result_t::incomplete_k: failures += expect(false); break;
        }
    }
    return failures;
}

/** Reads every value of every run of @p level back to back, which is what two compactors must agree on. */
template <typename level_type_>
[[nodiscard]] std::size_t collect_level_values(level_type_ const& level, io_channel_t& channel,
                                               st::vector<byte_t>& into) {
    std::size_t failures = 0;
    for (std::size_t run = 0; run != level.size(); ++run) {
        auto const& source = level.run(run);
        std::size_t const count = static_cast<std::size_t>(source.population_count());
        st::vector<std::uint64_t> positions;
        st::vector<std::size_t> starts;
        failures += expect(positions.resize(count) == st::status_t::success_k);
        failures += expect(starts.resize(count) == st::status_t::success_k);
        if (failures != 0) return failures;
        std::size_t const base = into.size();
        std::size_t total = 0;
        for (std::size_t at = 0; at != count; ++at) {
            positions[at] = at;
            starts[at] = base + total;
            total += source.value_window(at).bytes;
        }
        failures += expect(into.resize(base + total) == st::status_t::success_k);
        if (failures != 0) return failures;

        // Completions arrive in the transport's order, so every piece lands by its request and offset.
        auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            std::memcpy(into.data() + starts[request] + offset, bytes.data(), bytes.size());
        };
        failures += expect(source.read_values(channel, std::span<std::uint64_t const> {positions}, sink).succeeded());
    }
    return failures;
}

/** Writes @p wanted as one run of @p volume, takes it into @p runs and names it in @p level, which
 *  is how a case builds a level. */
template <typename level_type_, typename volume_type_>
[[nodiscard]] std::size_t adopt_batch(level_type_& level, typename level_type_::run_pool_t& runs, volume_type_& volume,
                                      io_channel_t& channel, std::span<expectation_t const> wanted) {
    using sorted_run_t = typename level_type_::sorted_run_t;
    std::size_t failures = 0;
    batch_t batch;
    failures += build_batch(wanted, batch);
    if (failures != 0) return failures;
    expected<sorted_run_t> written = sorted_run_t::write(
        volume, channel, uuid_t {}, uuid_generator(), std::span<variable_slot_t const> {batch.key_slots},
        std::span<std::uint64_t const> {fresh_stamps(batch.key_slots.size())},
        std::span<std::uint8_t const> {batch.kinds}, 0, std::span<byte_t const> {batch.values},
        std::span<stored_value_offset_t const> {batch.offsets}, value_max_size_k);
    failures += expect(written.has_value());
    if (!written) return failures;
    auto [handle, take_status] = runs.take(std::move(*written));
    failures += expect(take_status.succeeded());
    if (take_status.failed()) return failures;
    failures += expect(level.insert_run(handle).succeeded());
    return failures;
}

/** The runs @p level names, in its order, as a merge request takes them. */
template <typename level_type_>
[[nodiscard]] st::vector<typename level_type_::sorted_run_t const*> runs_of(level_type_ const& level) {
    st::vector<typename level_type_::sorted_run_t const*> runs;
    for (std::size_t index = 0; index != level.size(); ++index) [[maybe_unused]]
        st::status_t const placed = runs.push_back(&level.run(index));
    return runs;
}

/** A sorted level numbered @p number naming the runs a merge wrote, which is how a case
 *  reads them back. */
template <typename level_type_>
[[nodiscard]] level_type_ level_of(typename level_type_::run_pool_t& runs, std::uint32_t number,
                                   std::span<typename level_type_::run_handle_t const> merged) {
    level_type_ level = level_type_::of(runs, number, lsm_level_order_t::sorted_k);
    for (typename level_type_::run_handle_t const handle : merged) [[maybe_unused]]
        status_t const placed = level.insert_run(handle);
    return level;
}

/** Runs every slice of a merge in turn on the calling thread over one pair of channels, so a case
 *  sees what slicing changes apart from how the slices are scheduled. */
struct sequential_fan_out_t {

    /** Slices one merge is cut into. */
    std::size_t slices_count = 1;

    /** The channel every slice gathers its values on. */
    io_channel_t* reading = nullptr;

    /** The channel every slice writes its runs on. */
    io_channel_t* writing = nullptr;

    [[nodiscard]] std::size_t slices() const noexcept { return slices_count; }

    status_t run(compaction_fan_out_t::slice_call_t const& slice) const noexcept {
        for (std::size_t index = 0; index != slices_count; ++index)
            if (status_t const ran = slice(index, *reading, *writing); ran.failed()) return ran;
        return {};
    }
};

/** Runs every slice of a merge on its own thread of a ForkUnion pool, each reading on that thread's
 *  executor channel and writing on a shared one, as a blocking compact on an executor fans out. */
template <typename channel_pool_type_>
struct pooled_fan_out {

    /** The pool whose threads run the slices, one slice per thread. */
    pool_view_t pool;

    /** The channels every slice takes its pair from. */
    channel_pool_type_* channels = nullptr;

    [[nodiscard]] std::size_t slices() const noexcept { return pool.threads_count(); }

    status_t run(compaction_fan_out_t::slice_call_t const& slice) const noexcept {
        st::vector<status_t> statuses;
        if (statuses.resize(slices()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for the slices' statuses"};
        auto run_slice = [&](std::size_t task, std::size_t thread) noexcept {
            auto [reading, reading_status] = channels->acquire(thread);
            if (reading_status.failed()) {
                statuses[task] = reading_status;
                return;
            }
            auto [writing, writing_status] = channels->acquire();
            if (writing_status.failed()) {
                statuses[task] = writing_status;
                return;
            }
            statuses[task] = slice(task, reading.channel(), writing.channel());
        };
        if (st::failed(pool.for_n_dynamic(statuses.size(), run_slice)))
            return {status_code_t::contended_k, "The pool refused the slices"};
        for (status_t const& status : statuses)
            if (status.failed()) return status;
        return {};
    }
};

/** How many versions two levels disagree on by key, stamp and kind, read in key order across their
 *  runs, however differently they were cut. */
template <typename level_type_>
[[nodiscard]] std::size_t differing_versions(level_type_ const& left, level_type_ const& right) {
    using sorted_run_t = typename level_type_::sorted_run_t;
    std::size_t differing = left.population_count() != right.population_count();
    std::size_t left_run = 0, right_run = 0;
    std::uint64_t left_position = 0, right_position = 0;
    while (left_run != left.size() && right_run != right.size()) {
        sorted_run_t const& left_source = left.run(left_run);
        sorted_run_t const& right_source = right.run(right_run);
        if (left_position == left_source.population_count()) {
            ++left_run, left_position = 0;
            continue;
        }
        if (right_position == right_source.population_count()) {
            ++right_run, right_position = 0;
            continue;
        }
        differing += left_source.key_slot(left_position).stored_key !=
                         right_source.key_slot(right_position).stored_key ||
                     left_source.stamp(left_position) != right_source.stamp(right_position) ||
                     left_source.kind(left_position) != right_source.kind(right_position);
        ++left_position, ++right_position;
    }
    return differing;
}

#pragma endregion Model

#pragma region Flush Cases

/** One flush of memory runs becomes one level-zero run whose keys a point lookup finds. */
template <typename mount_type_>
std::size_t flush_lands_on_level_zero(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 600;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<run_pool_t> runs = run_pool_t::make(run_records_k, (*fixture)->resources->memory_constraint);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config(), *runs, slot_bytes_k);
    failures += expect(tree.has_value());
    if (!tree) return failures;

    st::vector<expectation_t> model;
    failures += expect(model.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index) * 2 + 1;
        entry.state = index % 7 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
        entry.bytes = entry.state == lookup_result_t::written_k ? 1 + index % 1021 : 0;
        entry.seed = index;
        model[index] = entry;
    }

    failures += flush_batch(*tree, guard->channel(), std::span<expectation_t const> {model});
    if (failures != 0) return failures;

    {
        typename tree_t::level_set_t const& taken = tree->levels();
        failures += expect_eq(taken.size(), std::size_t {1});
        failures += expect_eq(taken.level(0).size(), std::size_t {1});
        failures += expect(taken.level(0).order() == lsm_level_order_t::overlapping_k);
        failures += expect_eq(taken.population_count(), static_cast<std::uint64_t>(count_k));
    }
    failures += check_model(*tree, guard->channel(), std::span<expectation_t const> {model});

    // Every even key is a miss, since the model only ever wrote odd ones.
    st::vector<expectation_t> absent;
    failures += expect(absent.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index) * 2 + 2;
        entry.state = lookup_result_t::missing_k;
        absent[index] = entry;
    }
    failures += check_model(*tree, guard->channel(), std::span<expectation_t const> {absent});
    return failures;
}

/** A run written at stamp zero and restored with the stamp an ingest assigned it is seen by a reader at that stamp and
 *  skipped by one below it, as a commit at that stamp would be. */
template <typename mount_type_>
std::size_t restore_assigns_ingested_stamps(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using sorted_run_t = typename types_t::sorted_run_t;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 64;
    constexpr std::uint64_t assigned_k = 7;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;
    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<run_pool_t> runs = run_pool_t::make(run_records_k, (*fixture)->resources->memory_constraint);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config(), *runs, slot_bytes_k);
    failures += expect(tree.has_value());
    if (!tree) return failures;

    st::vector<expectation_t> model;
    st::vector<std::uint64_t> zeros;
    failures += expect(model.resize(count_k) == st::status_t::success_k);
    failures += expect(zeros.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index) * 2 + 1;
        entry.state = lookup_result_t::written_k;
        entry.bytes = 1 + index;
        entry.seed = index;
        model[index] = entry;
        zeros[index] = 0;
    }
    batch_t batch;
    failures += build_batch(std::span<expectation_t const> {model}, batch);
    if (failures != 0) return failures;
    uuid_t const content_id = uuid_generator();
    expected<sorted_run_t> const written = sorted_run_t::write(
        *volume, guard->channel(), uuid_t {}, content_id, std::span<variable_slot_t const> {batch.key_slots},
        std::span<std::uint64_t const> {zeros}, std::span<std::uint8_t const> {batch.kinds}, 0,
        std::span<byte_t const> {batch.values}, std::span<stored_value_offset_t const> {batch.offsets},
        value_max_size_k);
    failures += expect(written.has_value());
    if (!written) return failures;
    failures += expect_eq(written->assigned_stamp(), std::uint64_t {0});

    lsm_tree_run_record_t record;
    record.content_id = content_id;
    record.extent = written->extent();
    record.stamp = assigned_k;
    failures += expect(
        tree->restore(guard->channel(), std::span<lsm_tree_run_record_t const> {&record, 1}).succeeded());
    typename tree_t::level_set_t const& taken = tree->levels();
    failures += expect_eq(taken.level(0).run(0).assigned_stamp(), assigned_k);
    std::size_t misread = 0;
    for (expectation_t const& entry : model) {
        lsm_tree_position_t position;
        misread += tree->find(taken, key_of(entry.stored_key), position, assigned_k - 1) != lookup_result_t::missing_k;
        misread += tree->find(taken, key_of(entry.stored_key), position, assigned_k) != lookup_result_t::written_k;
    }
    failures += expect_eq(misread, std::size_t {0});
    return failures;
}

/** Several level-zero runs overlap, and the youngest version of every key is the one a read answers. */
template <typename mount_type_>
std::size_t overlapping_runs_take_the_youngest(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 400;
    constexpr std::size_t flushes_k = 5;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<run_pool_t> runs = run_pool_t::make(run_records_k, (*fixture)->resources->memory_constraint);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config(), *runs, slot_bytes_k);
    failures += expect(tree.has_value());
    if (!tree) return failures;

    st::vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index);
        entry.state = lookup_result_t::missing_k;
        model[index] = entry;
    }

    // Every flush rewrites the keys of one residue class, so the runs overlap and the last one wins.
    for (std::size_t flush = 0; flush != flushes_k; ++flush) {
        st::vector<expectation_t> batch;
        for (std::size_t index = 0; index != keys_k; ++index) {
            if ((index + flush) % 3 != 0) continue;
            expectation_t entry;
            entry.stored_key = static_cast<std::uint64_t>(index);
            entry.state = flush % 4 == 3 ? lookup_result_t::removed_k : lookup_result_t::written_k;
            entry.bytes = entry.state == lookup_result_t::written_k ? 1 + (index * 13 + flush) % 997 : 0;
            entry.seed = flush * keys_k + index;
            failures += expect(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
            model[index] = entry;
        }
        failures += flush_batch(*tree, guard->channel(), std::span<expectation_t const> {batch});
        if (failures != 0) return failures;
    }

    {
        typename tree_t::level_set_t const& taken = tree->levels();
        failures += expect_eq(taken.level(0).size(), flushes_k);
    }
    failures += check_model(*tree, guard->channel(), std::span<expectation_t const> {model});

    // A merged walk of level zero reports every key once, in ascending order, at its youngest run.
    typename tree_t::level_set_t const& taken = tree->levels();
    std::size_t visited = 0;
    std::size_t disordered = 0;
    stored_key_t previous {};
    failures += expect(taken.level(0)
                           .for_each_key_slot([&](lsm_level_position_t, variable_slot_t const& slot) noexcept {
                               if (visited != 0 && !(previous < slot.stored_key)) ++disordered;
                               previous = slot.stored_key;
                               ++visited;
                           })
                           .succeeded());
    failures += expect_eq(disordered, std::size_t {0});
    std::size_t live_keys = 0;
    for (std::size_t index = 0; index != keys_k; ++index)
        if (model[index].state != lookup_result_t::missing_k) ++live_keys;
    failures += expect_eq(visited, live_keys);
    return failures;
}

#pragma endregion Flush Cases

#pragma region Compaction Cases

/** A compaction merges level zero into level one, keeps the youngest value and forgets the remove deltas. */
template <typename mount_type_>
std::size_t compaction_drops_remove_deltas_at_the_bottom(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 900;
    constexpr std::size_t flushes_k = 4;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    expected<guard_t> reading = (*fixture)->channels.acquire();
    expected<guard_t> writing = (*fixture)->channels.acquire();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<run_pool_t> runs = run_pool_t::make(run_records_k, (*fixture)->resources->memory_constraint);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config(), *runs, slot_bytes_k);
    failures += expect(tree.has_value());
    if (!tree) return failures;

    st::vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index);
        entry.state = lookup_result_t::missing_k;
        model[index] = entry;
    }

    for (std::size_t flush = 0; flush != flushes_k; ++flush) {
        st::vector<expectation_t> batch;
        for (std::size_t index = 0; index != keys_k; ++index) {
            if ((index + flush) % 2 != 0) continue;
            expectation_t entry;
            entry.stored_key = static_cast<std::uint64_t>(index);
            entry.state = (index % 5 == 0 && flush + 1 == flushes_k) ? lookup_result_t::removed_k
                                                                     : lookup_result_t::written_k;
            entry.bytes = entry.state == lookup_result_t::written_k ? 1 + (index * 29 + flush) % 2039 : 0;
            entry.seed = flush * keys_k + index + 1;
            failures += expect(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
            model[index] = entry;
        }
        failures += flush_batch(*tree, guard->channel(), std::span<expectation_t const> {batch});
        if (failures != 0) return failures;
    }

    failures += expect(tree->needs_compaction(tree->levels()));
    std::array<typename tree_t::merge_buffers_t, 1> buffers {};
    expected<compaction_outcome_t> const outcome = tree->compact(
        reading->channel(), writing->channel(), std::span<typename tree_t::merge_buffers_t> {buffers},
        compaction_placement_t::host_k, {}, nullptr, last_write_fold_t {}, no_device_fold_t {});
    failures += expect(outcome.has_value());
    if (!outcome) return failures;
    failures += expect(*outcome == compaction_outcome_t::published_k);

    // The bottom level forgets a remove delta for good, so a removed key becomes a key nothing carries.
    for (std::size_t index = 0; index != keys_k; ++index)
        if (model[index].state == lookup_result_t::removed_k) model[index].state = lookup_result_t::missing_k;

    {
        typename tree_t::level_set_t const& taken = tree->levels();
        failures += expect_eq(taken.size(), std::size_t {2});
        failures += expect(taken.level(0).empty());
        failures += expect(!taken.level(1).empty());
        failures += expect(taken.level(1).order() == lsm_level_order_t::sorted_k);
        std::size_t live_keys = 0;
        for (std::size_t index = 0; index != keys_k; ++index)
            if (model[index].state != lookup_result_t::missing_k) ++live_keys;
        failures += expect_eq(taken.level(1).population_count(), static_cast<std::uint64_t>(live_keys));
    }
    failures += check_model(*tree, guard->channel(), std::span<expectation_t const> {model});
    return failures;
}

/** The same two levels merged into an intermediate level keep every remove delta the bottom would
 *  drop, and a merge cut into four slices, in turn or on four threads at once, keeps exactly what a
 *  merge in one does. */
template <typename mount_type_>
std::size_t intermediate_merge_keeps_remove_deltas(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using level_t = typename types_t::level_t;
    using run_pool_t = typename types_t::run_pool_t;
    using run_handle_t = typename types_t::run_handle_t;
    using merger_t = typename types_t::merger_t;
    using sorted_run_t = typename types_t::sorted_run_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 240;
    constexpr std::size_t threads_k = 4;
    // Every thread of the pool reads on a channel of its own and writes on a shared one beside the
    // case's own three
    fixture_config_t threaded = mount.config;
    threaded.executor_threads = threads_k;
    threaded.shared_io_channels += threads_k;
    expected<std::unique_ptr<fixture_t>> fixture = mount(threaded);
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    expected<guard_t> reading = (*fixture)->channels.acquire();
    expected<guard_t> writing = (*fixture)->channels.acquire();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    st::vector<expectation_t> older;
    st::vector<expectation_t> newer;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index);
        entry.state = lookup_result_t::written_k;
        entry.bytes = 1 + index % 311;
        entry.seed = index + 1;
        failures += expect(older.push_back(expectation_t {entry}) == st::status_t::success_k);
        if (index % 3 != 0) continue;
        entry.state = lookup_result_t::removed_k;
        entry.bytes = 0;
        failures += expect(newer.push_back(expectation_t {entry}) == st::status_t::success_k);
    }
    if (failures != 0) return failures;

    std::size_t removed_keys = 0;
    for (std::size_t index = 0; index != keys_k; ++index)
        if (index % 3 == 0) ++removed_keys;

    expected<run_pool_t> runs = run_pool_t::make(run_records_k, (*fixture)->resources->memory_constraint);
    failures += expect(runs.has_value());
    if (!runs) return failures;

    // One level of two overlapping runs, merged twice with nothing changed but the depth.
    level_t upper = level_t::of(*runs, 0, lsm_level_order_t::overlapping_k);
    failures += adopt_batch(upper, *runs, *volume, guard->channel(), std::span<expectation_t const> {older});
    failures += adopt_batch(upper, *runs, *volume, guard->channel(), std::span<expectation_t const> {newer});
    if (failures != 0) return failures;
    st::vector<sorted_run_t const*> const sources = runs_of(upper);

    std::array<typename merger_t::merge_buffers_t, 4> buffers;
    typename merger_t::merge_request_t request;
    request.upper = sources;
    request.upper_order = lsm_level_order_t::overlapping_k;
    request.value_max_size = value_max_size_k;
    request.placement = compaction_placement_t::host_k;
    request.buffers = std::span<typename merger_t::merge_buffers_t> {buffers}.first(1);

    request.depth = lsm_level_depth_t::intermediate_k;
    expected<st::vector<run_handle_t>> kept_runs = merger_t::merge(
        *volume, *runs, reading->channel(), writing->channel(), request, last_write_fold_t {}, no_device_fold_t {});
    failures += expect_status(kept_runs, status_code_t::success_k);
    if (!kept_runs) return failures;
    level_t const kept = level_of<level_t>(*runs, 1, *kept_runs);
    failures += expect_eq(kept.population_count(), static_cast<std::uint64_t>(keys_k));

    // The same merge cut into four slices writes the very same versions and values, only cut
    // into more runs.
    sequential_fan_out_t sequential {4, &reading->channel(), &writing->channel()};
    request.buffers = buffers;
    request.fan_out = compaction_fan_out_t {sequential};
    expected<st::vector<run_handle_t>> sliced_runs = merger_t::merge(
        *volume, *runs, reading->channel(), writing->channel(), request, last_write_fold_t {}, no_device_fold_t {});
    request.buffers = std::span<typename merger_t::merge_buffers_t> {buffers}.first(1);
    request.fan_out = compaction_fan_out_t {};
    failures += expect_status(sliced_runs, status_code_t::success_k);
    if (!sliced_runs) return failures;
    level_t const sliced = level_of<level_t>(*runs, 1, *sliced_runs);
    failures += expect(sliced.size() >= 4);
    failures += expect_eq(differing_versions(kept, sliced), std::size_t {0});
    st::vector<byte_t> kept_values;
    st::vector<byte_t> sliced_values;
    failures += collect_level_values(kept, guard->channel(), kept_values);
    failures += collect_level_values(sliced, guard->channel(), sliced_values);
    failures += expect_eq(sliced_values.size(), kept_values.size());
    if (sliced_values.size() == kept_values.size())
        failures += expect_eq(std::memcmp(kept_values.data(), sliced_values.data(), kept_values.size()), 0);

    // And again with every slice on its own thread of a pool, side by side.
    st::expected<executor_t> pool = executor_t::make_unpinned(threads_k, caller_exclusivity_t::caller_inclusive_k,
                                                              fu_capabilities_all_k);
    failures += expect(pool.has_value());
    if (!pool) return failures;
    pooled_fan_out<typename fixture_t::channel_pool_t> pooled {pool->view(), &(*fixture)->channels};
    failures += expect_eq(pooled.slices(), threads_k);
    request.buffers = buffers;
    request.fan_out = compaction_fan_out_t {pooled};
    expected<st::vector<run_handle_t>> threaded_runs = merger_t::merge(
        *volume, *runs, reading->channel(), writing->channel(), request, last_write_fold_t {}, no_device_fold_t {});
    request.buffers = std::span<typename merger_t::merge_buffers_t> {buffers}.first(1);
    request.fan_out = compaction_fan_out_t {};
    failures += expect_status(threaded_runs, status_code_t::success_k);
    if (!threaded_runs) return failures;
    level_t const threaded_level = level_of<level_t>(*runs, 1, *threaded_runs);
    failures += expect_eq(differing_versions(kept, threaded_level), std::size_t {0});
    st::vector<byte_t> threaded_values;
    failures += collect_level_values(threaded_level, guard->channel(), threaded_values);
    failures += expect_eq(threaded_values.size(), kept_values.size());
    if (threaded_values.size() == kept_values.size())
        failures += expect_eq(std::memcmp(kept_values.data(), threaded_values.data(), kept_values.size()), 0);

    request.depth = lsm_level_depth_t::bottom_k;
    expected<st::vector<run_handle_t>> dropped_runs = merger_t::merge(
        *volume, *runs, reading->channel(), writing->channel(), request, last_write_fold_t {}, no_device_fold_t {});
    failures += expect(dropped_runs.has_value());
    if (!dropped_runs) return failures;
    level_t const dropped = level_of<level_t>(*runs, 1, *dropped_runs);
    failures += expect_eq(dropped.population_count(), static_cast<std::uint64_t>(keys_k - removed_keys));

    // The intermediate level answers a removed key as a key it carries, and the bottom one as a miss.
    for (std::size_t index = 0; index < keys_k; index += 3) {
        lsm_level_position_t position;
        failures += expect(kept.find(key_of(index), position) == lookup_result_t::removed_k);
        failures += expect(dropped.find(key_of(index), position) == lookup_result_t::missing_k);
    }
    for (std::size_t index = 1; index < keys_k; index += 3) {
        lsm_level_position_t position;
        failures += expect(kept.find(key_of(index), position) == lookup_result_t::written_k);
        failures += expect(dropped.find(key_of(index), position) == lookup_result_t::written_k);
    }

    runs->discard(*volume, *kept_runs);
    runs->discard(*volume, *sliced_runs);
    runs->discard(*volume, *threaded_runs);
    runs->discard(*volume, *dropped_runs);
    runs->discard(*volume, upper.runs());
    failures += expect_eq(runs->size(), std::size_t {0});
    return failures;
}

/** A cancelled compaction leaves the level set live, both pools full and not one byte of the volume out. */
template <typename mount_type_>
std::size_t cancelled_compaction_leaves_nothing(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 700;
    constexpr std::size_t flushes_k = 4;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    expected<guard_t> reading = (*fixture)->channels.acquire();
    expected<guard_t> writing = (*fixture)->channels.acquire();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<run_pool_t> runs = run_pool_t::make(run_records_k, (*fixture)->resources->memory_constraint);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config(), *runs, slot_bytes_k);
    failures += expect(tree.has_value());
    if (!tree) return failures;

    st::vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index);
        entry.state = lookup_result_t::written_k;
        entry.bytes = 1 + index % 1531;
        entry.seed = index + 3;
        model[index] = entry;
    }
    for (std::size_t flush = 0; flush != flushes_k; ++flush) {
        st::vector<expectation_t> batch;
        for (std::size_t index = flush; index < keys_k; index += flushes_k)
            failures += expect(batch.push_back(expectation_t {model[index]}) == st::status_t::success_k);
        failures += flush_batch(*tree, guard->channel(), std::span<expectation_t const> {batch});
        if (failures != 0) return failures;
    }
    failures += expect(tree->needs_compaction(tree->levels()));

    std::size_t const records_before = runs->size();
    std::uint32_t const requests_before = reading->channel().requests().available_buffers();
    std::uint32_t const streams_before = writing->channel().streams().available_buffers();
    std::uint64_t population_before = 0;
    std::size_t runs_before = 0;
    {
        typename tree_t::level_set_t const& taken = tree->levels();
        population_before = taken.population_count();
        runs_before = taken.level(0).size();
    }

    compaction_cancellation<atomic_reference> cancellation;
    cancellation.raise();
    std::array<typename tree_t::merge_buffers_t, 1> buffers {};
    expected<compaction_outcome_t> const outcome = tree->compact(
        reading->channel(), writing->channel(), std::span<typename tree_t::merge_buffers_t> {buffers},
        compaction_placement_t::host_k, {}, &cancellation, last_write_fold_t {}, no_device_fold_t {});
    failures += expect(!outcome.has_value());
    failures += expect_status(outcome, status_code_t::uninitialized_k);

    // Nothing the merge took is still out: no queue pair, no block and no run record.
    failures += expect_eq(reading->channel().requests().available_buffers(), requests_before);
    failures += expect_eq(writing->channel().streams().available_buffers(), streams_before);
    failures += expect_eq(writing->channel().requests().available_buffers(), lsm_config_k.request_buffers);
    failures += expect_eq(runs->size(), records_before);
    failures += expect_eq((*fixture)->disk.outstanding(reading->channel().queue_pair()), std::size_t {0});
    failures += expect_eq((*fixture)->disk.outstanding(writing->channel().queue_pair()), std::size_t {0});

    {
        typename tree_t::level_set_t const& taken = tree->levels();
        failures += expect_eq(taken.size(), std::size_t {1});
        failures += expect_eq(taken.level(0).size(), runs_before);
        failures += expect_eq(taken.population_count(), population_before);
    }
    failures += check_model(*tree, guard->channel(), std::span<expectation_t const> {model});

    // The same tree still compacts once nobody is cancelling it.
    expected<compaction_outcome_t> const again = tree->compact(
        reading->channel(), writing->channel(), std::span<typename tree_t::merge_buffers_t> {buffers},
        compaction_placement_t::host_k, {}, nullptr, last_write_fold_t {}, no_device_fold_t {});
    failures += expect(again.has_value());
    if (again) failures += expect(*again == compaction_outcome_t::published_k);
    failures += check_model(*tree, guard->channel(), std::span<expectation_t const> {model});
    return failures;
}

/** A compaction raced by a cancellation either publishes whole or changes nothing, and never leaks either way. */
template <typename mount_type_>
std::size_t raced_cancellation_is_all_or_nothing(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 800;
    constexpr std::size_t flushes_k = 4;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    expected<guard_t> reading = (*fixture)->channels.acquire();
    expected<guard_t> writing = (*fixture)->channels.acquire();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<run_pool_t> runs = run_pool_t::make(run_records_k, (*fixture)->resources->memory_constraint);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config(), *runs, slot_bytes_k);
    failures += expect(tree.has_value());
    if (!tree) return failures;

    st::vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index);
        entry.state = lookup_result_t::written_k;
        entry.bytes = 1 + index % 907;
        entry.seed = index + 11;
        model[index] = entry;
    }
    for (std::size_t flush = 0; flush != flushes_k; ++flush) {
        st::vector<expectation_t> batch;
        for (std::size_t index = flush; index < keys_k; index += flushes_k)
            failures += expect(batch.push_back(expectation_t {model[index]}) == st::status_t::success_k);
        failures += flush_batch(*tree, guard->channel(), std::span<expectation_t const> {batch});
        if (failures != 0) return failures;
    }

    std::size_t const records_before = runs->size();
    std::uint64_t population_before = 0;
    {
        typename tree_t::level_set_t const& taken = tree->levels();
        population_before = taken.population_count();
    }

    compaction_cancellation<atomic_reference> cancellation;
    std::thread raiser {[&cancellation]() noexcept {
        for (std::size_t spins = 0; spins != 64; ++spins) fu::preferred_yield_t {}(&spins, spins, std::size_t {0});
        cancellation.raise();
    }};
    std::array<typename tree_t::merge_buffers_t, 1> buffers {};
    expected<compaction_outcome_t> const outcome = tree->compact(
        reading->channel(), writing->channel(), std::span<typename tree_t::merge_buffers_t> {buffers},
        compaction_placement_t::host_k, {}, &cancellation, last_write_fold_t {}, no_device_fold_t {});
    raiser.join();

    failures += expect_eq(writing->channel().streams().available_buffers(), lsm_config_k.stream_buffers);
    failures += expect_eq(reading->channel().streams().available_buffers(), lsm_config_k.stream_buffers);
    failures += expect_eq(reading->channel().requests().available_buffers(), lsm_config_k.request_buffers);
    failures += expect_eq((*fixture)->disk.outstanding(writing->channel().queue_pair()), std::size_t {0});

    typename tree_t::level_set_t const& taken = tree->levels();
    failures += expect_eq(taken.population_count(), population_before);
    if (!outcome) {
        failures += expect_status(outcome, status_code_t::uninitialized_k);
        failures += expect_eq(runs->size(), records_before);
        failures += expect_eq(taken.size(), std::size_t {1});
    }
    else {
        failures += expect(*outcome == compaction_outcome_t::published_k);
        failures += expect_eq(taken.size(), std::size_t {2});
        failures += expect(taken.level(0).empty());
    }
    failures += check_model(*tree, guard->channel(), std::span<expectation_t const> {model});
    return failures;
}

/** Checks every key below @p keys_count of @p tree against @p reference: missing and written ones
 *  by a batched read, removed ones by a lookup that finds the remove delta or, once a merge at the
 *  bottom dropped it, nothing. */
template <typename tree_type_, typename reference_type_>
[[nodiscard]] std::size_t check_reference(tree_type_ const& tree, io_channel_t& channel,
                                          reference_type_ const& reference, std::uint64_t keys_count) {
    std::size_t mismatched = 0;
    st::vector<expectation_t> answered;
    typename tree_type_::level_set_t const& taken = tree.levels();
    for (std::uint64_t key = 0; key != keys_count; ++key) {
        expectation_t wanted;
        wanted.stored_key = key;
        [[maybe_unused]] st::status_t const found = reference.find(
            key, [&](st::mapping<std::uint64_t, expectation_t> const& stored) noexcept { wanted = stored.mapped; });
        if (wanted.state != lookup_result_t::removed_k) {
            mismatched += answered.push_back(expectation_t {wanted}) != st::status_t::success_k;
            continue;
        }
        lsm_tree_position_t position;
        lookup_result_t const result = tree.find(taken, key_of(key), position);
        mismatched += result != lookup_result_t::removed_k && result != lookup_result_t::missing_k;
    }
    return expect_eq(mismatched, std::size_t {0}) +
           check_model(tree, channel, std::span<expectation_t const> {answered});
}

/** A run alone on level zero past one slot, which overlaps nothing below, moves down by
 *  its handle untouched. */
template <typename mount_type_>
std::size_t trivial_move_writes_nothing(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    using sorted_run_t = typename types_t::sorted_run_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 200;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    expected<guard_t> reading = (*fixture)->channels.acquire();
    expected<guard_t> writing = (*fixture)->channels.acquire();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<run_pool_t> runs = run_pool_t::make(run_records_k, (*fixture)->resources->memory_constraint);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config(), *runs, slot_bytes_k);
    failures += expect(tree.has_value());
    if (!tree) return failures;

    st::vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index);
        entry.state = index % 9 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
        entry.bytes = entry.state == lookup_result_t::written_k ? 400 + index % 401 : 0;
        entry.seed = index + 17;
        model[index] = entry;
    }
    failures += flush_batch(*tree, guard->channel(), std::span<expectation_t const> {model});
    if (failures != 0) return failures;

    sorted_run_t const* const flushed = &tree->levels().level(0).run(0);
    uuid_t const content_id = flushed->content_id();
    failures += expect(flushed->size_bytes() >= slot_bytes_k);
    failures += expect(tree->needs_compaction(tree->levels()));
    std::array<typename tree_t::merge_buffers_t, 1> buffers {};
    expected<compaction_outcome_t> const outcome = tree->compact(
        reading->channel(), writing->channel(), std::span<typename tree_t::merge_buffers_t> {buffers},
        compaction_placement_t::host_k, {}, nullptr, last_write_fold_t {}, no_device_fold_t {});
    failures += expect(outcome.has_value());
    if (!outcome) return failures;
    failures += expect(*outcome == compaction_outcome_t::published_k);

    // The run below is the very record level zero named, so no byte was read or written and no
    // run was minted.
    typename tree_t::level_set_t const& taken = tree->levels();
    failures += expect_eq(taken.size(), std::size_t {2});
    failures += expect(taken.level(0).empty());
    failures += expect_eq(taken.level(1).size(), std::size_t {1});
    if (taken.level(1).size() == 1) {
        failures += expect(&taken.level(1).run(0) == flushed);
        failures += expect(taken.level(1).run(0).content_id() == content_id);
    }
    failures += expect_eq(runs->size(), std::size_t {1});
    failures += check_model(*tree, guard->channel(), std::span<expectation_t const> {model});
    return failures;
}

/** A run handle kept past its record's vacate resolves to nothing, even once another run
 *  takes the record. */
template <typename mount_type_>
std::size_t stale_run_handle_resolves_to_nothing(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using run_pool_t = typename types_t::run_pool_t;
    using sorted_run_t = typename types_t::sorted_run_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;
    expected<run_pool_t> runs = run_pool_t::make(1, (*fixture)->resources->memory_constraint);
    failures += expect(runs.has_value());
    if (!runs) return failures;

    auto [first, take_status] = runs->take(sorted_run_t {});
    failures += expect(take_status.succeeded());
    failures += expect(runs->at(first) != nullptr);
    // The one record is taken, so another take is refused rather than handed a live run.
    failures += expect_status(runs->take(sorted_run_t {}), status_code_t::capacity_exhausted_k);
    runs->vacate(first);
    failures += expect(runs->at(first) == nullptr);
    failures += expect_eq(runs->size(), std::size_t {0});

    auto [second, retake_status] = runs->take(sorted_run_t {});
    failures += expect(retake_status.succeeded());
    failures += expect_eq(second.index, first.index);
    failures += expect(runs->at(second) != nullptr);
    failures += expect(runs->at(first) == nullptr);
    runs->vacate(second);
    // Vacating a stale handle again changes nothing.
    runs->vacate(first);
    failures += expect_eq(runs->size(), std::size_t {0});
    return failures;
}

/** Many small flushes, each followed by the compactions a commit runs, answer every key as a
 *  reference store does, while no step writes more than F + 3 slots and a step writing several runs
 *  fills each halfway. */
template <typename mount_type_>
std::size_t compaction_history_matches_reference(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    using sorted_run_t = typename types_t::sorted_run_t;
    using reference_t = st::reference_store<st::mapping<std::uint64_t, expectation_t>>;
    std::size_t failures = 0;
    constexpr std::uint64_t slot_bytes = 256ull * 1024ull;
    constexpr std::size_t keys_k = 12000;
    constexpr std::size_t flushes_k = 200;
    constexpr std::size_t flushed_keys_k = 160;
    constexpr std::uint64_t value_bytes_max_k = 700;
    constexpr std::size_t steps_per_commit_max_k = 64;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    expected<guard_t> reading = (*fixture)->channels.acquire();
    expected<guard_t> writing = (*fixture)->channels.acquire();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<run_pool_t> runs = run_pool_t::make(run_records_k, (*fixture)->resources->memory_constraint);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config(), *runs, slot_bytes);
    failures += expect(tree.has_value());
    if (!tree) return failures;
    st::expected<reference_t> reference = reference_t::make();
    failures += expect(reference.has_value());
    if (!reference) return failures;

    // A step reads at most a slot of level zero or one run, and what it overlaps below, F + 2
    // slots at most.
    std::uint64_t const headroom_bytes = (test_tree_config().level_enlarge_factor + 3) * slot_bytes;
    // The merger cuts at the slot less its owner block, leaving the trailer a stamp and a kind per
    // key slot and a page, and lands within a row of cells, a value and the alignment of four
    // sections of its aim.
    std::uint64_t const run_bytes_limit = (slot_bytes - 2 * portable_alignment_k) /
                                          (sizeof(variable_slot_t) + sizeof(std::uint64_t) + 1) *
                                          sizeof(variable_slot_t);
    std::uint64_t const cut_slack_bytes = 4 * portable_alignment_k + 16 * sizeof(variable_slot_t) + value_bytes_max_k;

    random_t random {0x7EEC0FFEEull};
    std::size_t steps = 0;
    std::size_t multi_run_steps = 0;
    std::size_t overspent = 0;
    std::size_t mislengthed = 0;
    std::size_t underfilled = 0;
    st::vector<uuid_t> before;
    for (std::size_t flush = 0; flush != flushes_k && failures == 0; ++flush) {
        st::vector<expectation_t> batch;
        for (std::uint64_t key = 0; key != keys_k; ++key) {
            if (random.below(keys_k) >= flushed_keys_k) continue;
            expectation_t entry;
            entry.stored_key = key;
            entry.state = random.below(8) == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
            entry.bytes = entry.state == lookup_result_t::written_k ? 1 + random.below(value_bytes_max_k) : 0;
            entry.seed = random.next();
            failures += expect(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
            failures += expect(reference->upsert(st::mapping<std::uint64_t, expectation_t> {key, entry}) ==
                               st::status_t::success_k);
        }
        if (batch.empty()) continue;
        failures += flush_batch(*tree, guard->channel(), std::span<expectation_t const> {batch});

        for (std::size_t step = 0; step != steps_per_commit_max_k && tree->needs_compaction(tree->levels()); ++step) {
            before.clear();
            typename tree_t::level_set_t const& source = tree->levels();
            for (std::size_t number = 0; number != source.size(); ++number)
                for (std::size_t index = 0; index != source.level(number).size(); ++index)
                    failures += expect(before.push_back(source.level(number).run(index).content_id()) ==
                                       st::status_t::success_k);
            std::array<typename tree_t::merge_buffers_t, 1> buffers {};
            expected<compaction_outcome_t> const outcome = tree->compact(
                reading->channel(), writing->channel(), std::span<typename tree_t::merge_buffers_t> {buffers},
                compaction_placement_t::host_k, {}, nullptr, last_write_fold_t {}, no_device_fold_t {});
            failures += expect(outcome.has_value() && *outcome == compaction_outcome_t::published_k);
            if (!outcome) break;
            ++steps;

            st::vector<sorted_run_t const*> written;
            typename tree_t::level_set_t const& merged = tree->levels();
            for (std::size_t number = 0; number != merged.size(); ++number)
                for (std::size_t index = 0; index != merged.level(number).size(); ++index) {
                    sorted_run_t const& run = merged.level(number).run(index);
                    bool known = false;
                    for (uuid_t const& id : before) known |= id == run.content_id();
                    if (!known) failures += expect(written.push_back(&run) == st::status_t::success_k);
                }
            std::uint64_t written_bytes = 0;
            for (sorted_run_t const* run : written) {
                written_bytes += run->size_bytes();
                mislengthed += run->size_bytes() != sorted_run_layout<variable_slot_t>::blob_bytes(run->header());
            }
            overspent += written_bytes > headroom_bytes;
            if (written.size() < 2) continue;
            ++multi_run_steps;
            for (sorted_run_t const* run : written)
                underfilled += 2 * (run->size_bytes() + cut_slack_bytes) < run_bytes_limit;
        }
        failures += expect(!tree->needs_compaction(tree->levels()));
        if (flush % 25 == 24) failures += check_reference(*tree, guard->channel(), *reference, keys_k);
    }
    failures += check_reference(*tree, guard->channel(), *reference, keys_k);
    failures += expect(tree->levels().size() >= 3);
    failures += expect(steps != 0 && multi_run_steps != 0);
    failures += expect_eq(overspent, std::size_t {0});
    failures += expect_eq(mislengthed, std::size_t {0});
    failures += expect_eq(underfilled, std::size_t {0});
    // Every record the pool holds is a run the current set names, so every consumed run
    // was given back.
    std::size_t named = 0;
    for (std::size_t number = 0; number != tree->levels().size(); ++number)
        named += tree->levels().level(number).size();
    failures += expect_eq(runs->size(), named);
    return failures;
}

#pragma endregion Compaction Cases

#pragma region Compactor Choice Cases

/** The threshold picks the device compactor only above it, and only where a device is present. */
std::size_t compactor_choice_follows_the_threshold() {
    std::size_t failures = 0;
    constexpr std::uint64_t threshold_k = storage_limits_t::cuda_compaction_threshold_k;

    failures += expect(choose_compactor(compaction_placement_t::automatic_k, 1, threshold_k) ==
                       compactor_choice_t::host_k);
    failures += expect(choose_compactor(compaction_placement_t::automatic_k, 1, threshold_k + 1) ==
                       compactor_choice_t::device_k);
    failures += expect(choose_compactor(compaction_placement_t::automatic_k, 0, threshold_k + 1) ==
                       compactor_choice_t::host_k);
    failures += expect(choose_compactor(compaction_placement_t::automatic_k, 4, threshold_k * 8) ==
                       compactor_choice_t::device_k);

    // A fixed placement answers itself, whatever the machine carries and however large the merge is.
    failures += expect(choose_compactor(compaction_placement_t::host_k, 8, threshold_k * 8) ==
                       compactor_choice_t::host_k);
    failures += expect(choose_compactor(compaction_placement_t::device_k, 0, 1) == compactor_choice_t::device_k);
    return failures;
}

/** A merge on the host and the same merge on a device answer the same runs, key slot for key slot. */
template <typename mount_type_>
case_result_t both_compactors_agree(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using level_t = typename types_t::level_t;
    using run_pool_t = typename types_t::run_pool_t;
    using run_handle_t = typename types_t::run_handle_t;
    using merger_t = typename types_t::merger_t;
    using sorted_run_t = typename types_t::sorted_run_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 512;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    expected<guard_t> reading = (*fixture)->channels.acquire();
    expected<guard_t> writing = (*fixture)->channels.acquire();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    st::vector<expectation_t> older;
    st::vector<expectation_t> newer;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index);
        entry.state = index % 11 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
        entry.bytes = entry.state == lookup_result_t::written_k ? 1 + index % 613 : 0;
        entry.seed = index + 5;
        failures += expect(older.push_back(expectation_t {entry}) == st::status_t::success_k);
        if (index % 2 != 0) continue;
        entry.bytes = entry.state == lookup_result_t::written_k ? 1 + index % 271 : 0;
        entry.seed = index + 4096;
        failures += expect(newer.push_back(expectation_t {entry}) == st::status_t::success_k);
    }
    if (failures != 0) return failures;

    expected<run_pool_t> runs = run_pool_t::make(run_records_k, (*fixture)->resources->memory_constraint);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    level_t upper = level_t::of(*runs, 0, lsm_level_order_t::overlapping_k);
    failures += adopt_batch(upper, *runs, *volume, guard->channel(), std::span<expectation_t const> {older});
    failures += adopt_batch(upper, *runs, *volume, guard->channel(), std::span<expectation_t const> {newer});
    if (failures != 0) return failures;
    st::vector<sorted_run_t const*> const sources = runs_of(upper);

    std::array<typename merger_t::merge_buffers_t, 1> buffers;
    typename merger_t::merge_request_t request;
    request.upper = sources;
    request.upper_order = lsm_level_order_t::overlapping_k;
    request.depth = lsm_level_depth_t::intermediate_k;
    request.value_max_size = value_max_size_k;
    request.buffers = buffers;

    request.placement = compaction_placement_t::host_k;
    expected<st::vector<run_handle_t>> host_runs = merger_t::merge(
        *volume, *runs, reading->channel(), writing->channel(), request, last_write_fold_t {}, no_device_fold_t {});
    failures += expect(host_runs.has_value());
    if (!host_runs) return failures;
    level_t const host = level_of<level_t>(*runs, 1, *host_runs);

    // The device side runs on a compactor made once, as an engine makes its own at open, and a build without one
    // refuses.
    request.placement = compaction_placement_t::device_k;
    gpu_drivers_t drivers;
    kernel_tables<cuda_module_t> kernels;
    expected<cuda_executor_t> executor = status_t {status_code_t::device_k, "No CUDA device zero"};
    if (drivers.retain_context(0) &&
        kernels.load(drivers.cuda, drivers.cuda_contexts[0], 0, vram_nvidia_bundle(), vram_kernel_names_k).succeeded())
        executor = cuda_executor_t::spawn(drivers.cuda, drivers.cuda_contexts[0], 0, gpu_class_t::unknown_k, nullptr, 0,
                                          ustore_gds_automatic_k, kernels.table_of(vram_nvidia_bundle(), 0));
    expected<cuda_compactor_t> compactor = executor ? cuda_compactor_t::make(*executor)
                                                    : expected<cuda_compactor_t> {executor.status()};
    if (compactor) request.compactor = std::addressof(*compactor);
    expected<st::vector<run_handle_t>> device_runs = merger_t::merge(
        *volume, *runs, reading->channel(), writing->channel(), request, last_write_fold_t {}, no_device_fold_t {});
    if (!device_runs) {
        bool const no_compactor = device_runs == status_code_t::unsupported_k || device_runs == status_code_t::device_k;
        if (!no_compactor) failures += expect_status(device_runs, status_code_t::unsupported_k);
        runs->discard(*volume, *host_runs);
        runs->discard(*volume, upper.runs());
        return no_compactor ? case_result_t::skipped("no device compactor", failures) : case_result_t {failures};
    }
    level_t const device = level_of<level_t>(*runs, 1, *device_runs);

    failures += expect_eq(device.size(), host.size());
    failures += expect_eq(device.population_count(), host.population_count());
    std::size_t differing = 0;
    for (std::size_t run = 0; run != host.size() && run != device.size(); ++run) {
        sorted_run_t const& left = host.run(run);
        sorted_run_t const& right = device.run(run);
        if (left.population_count() != right.population_count()) {
            ++differing;
            continue;
        }
        for (std::uint64_t at = 0; at != left.population_count(); ++at)
            differing += left.key_slot(at).stored_key != right.key_slot(at).stored_key ||
                         left.kind(at) != right.kind(at);
        // Equal populations carry equally many value offsets, which must match one for one.
        differing += std::memcmp(left.value_offsets().data(), right.value_offsets().data(),
                                 left.value_offsets().size_bytes()) != 0;
    }
    failures += expect_eq(differing, std::size_t {0});

    // Identical cells are not identical runs until the bytes they locate come back the same too.
    st::vector<byte_t> host_values;
    st::vector<byte_t> device_values;
    failures += collect_level_values(host, guard->channel(), host_values);
    failures += collect_level_values(device, guard->channel(), device_values);
    failures += expect_eq(device_values.size(), host_values.size());
    if (device_values.size() == host_values.size())
        failures += expect_eq(std::memcmp(host_values.data(), device_values.data(), host_values.size()), 0);
    runs->discard(*volume, *host_runs);
    runs->discard(*volume, *device_runs);
    runs->discard(*volume, upper.runs());
    return failures;
}

#pragma endregion Compactor Choice Cases

#pragma region Allocation Cases

/**
 *  @brief A merge reads every source index in place, so it never asks the heap for a copy of
 *      one, and a merge again through the same buffers takes none of its room anew and writes
 *      the same values.
 *
 *  The runs overlap and the survivors outnumber every source run, so no buffer of the merge or of
 *  the new run is the size of a source index. The output's cells take room for every merged key
 *  slot, the size of all indexes together, which only the first merge asks for. The second merge
 *  finds every streaming block of its reading channel taken, so its windows fall back to the heap.
 */
template <typename mount_type_>
std::size_t merge_charges_no_index_copy(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using level_t = typename types_t::level_t;
    using run_pool_t = typename types_t::run_pool_t;
    using run_handle_t = typename types_t::run_handle_t;
    using merger_t = typename types_t::merger_t;
    using sorted_run_t = typename types_t::sorted_run_t;
    std::size_t failures = 0;
    constexpr std::size_t older_keys_k = 1000;
    constexpr std::size_t newer_keys_k = 300;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    expected<guard_t> reading = (*fixture)->channels.acquire();
    expected<guard_t> writing = (*fixture)->channels.acquire();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    // Half of the newer keys shadow older ones and half sit past them, so the survivors number 1150.
    st::vector<expectation_t> older;
    st::vector<expectation_t> newer;
    for (std::size_t index = 0; index != older_keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index);
        entry.state = index % 11 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
        entry.bytes = entry.state == lookup_result_t::written_k ? 1 + index % 613 : 0;
        entry.seed = index + 5;
        failures += expect(older.push_back(expectation_t {entry}) == st::status_t::success_k);
    }
    for (std::size_t index = 0; index != newer_keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index < newer_keys_k / 2 ? index * 2 : older_keys_k + index);
        entry.state = lookup_result_t::written_k;
        entry.bytes = 1 + index % 271;
        entry.seed = index + 4096;
        failures += expect(newer.push_back(expectation_t {entry}) == st::status_t::success_k);
    }
    if (failures != 0) return failures;

    expected<run_pool_t> runs = run_pool_t::make(run_records_k, (*fixture)->resources->memory_constraint);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    level_t upper = level_t::of(*runs, 0, lsm_level_order_t::overlapping_k);
    failures += adopt_batch(upper, *runs, *volume, guard->channel(), std::span<expectation_t const> {older});
    failures += adopt_batch(upper, *runs, *volume, guard->channel(), std::span<expectation_t const> {newer});
    if (failures != 0) return failures;
    st::vector<sorted_run_t const*> const sources = runs_of(upper);

    // The sizes a copy of one source index would be requested at.
    allocation_watch_t& watch = allocation_watch_g;
    allocation_watch_t indexes_watch;
    std::size_t all_indexes = 0;
    for (std::size_t run = 0; run != upper.size(); ++run) {
        std::size_t const index_bytes = upper.run(run).population_count() * sizeof(variable_slot_t);
        indexes_watch.sizes[indexes_watch.count++] = index_bytes;
        all_indexes += index_bytes;
    }

    std::array<typename merger_t::merge_buffers_t, 1> buffers;
    typename merger_t::merge_request_t request;
    request.upper = sources;
    request.upper_order = lsm_level_order_t::overlapping_k;
    request.depth = lsm_level_depth_t::intermediate_k;
    request.value_max_size = value_max_size_k;
    request.placement = compaction_placement_t::host_k;
    request.buffers = buffers;
    watch = indexes_watch;
    expected<st::vector<run_handle_t>> merged_runs = merger_t::merge(
        *volume, *runs, reading->channel(), writing->channel(), request, last_write_fold_t {}, no_device_fold_t {});
    allocation_watch_t const seen = watch;
    st::vector<pooled_buffer_t> taken_streams;
    for (std::uint32_t index = 0; index != lsm_config_k.stream_buffers; ++index) {
        auto [held, held_status] = reading->channel().streams().acquire();
        failures += expect_status(held_status, status_code_t::success_k);
        failures += expect(taken_streams.push_back(std::move(held)) == st::status_t::success_k);
    }
    watch = indexes_watch;
    watch.sizes[watch.count++] = all_indexes;
    expected<st::vector<run_handle_t>> merged_again = merger_t::merge(
        *volume, *runs, reading->channel(), writing->channel(), request, last_write_fold_t {}, no_device_fold_t {});
    allocation_watch_t const seen_again = watch;
    watch = allocation_watch_t {};
    taken_streams.clear();
    failures += expect_status(merged_runs, status_code_t::success_k);
    failures += expect_status(merged_again, status_code_t::success_k);
    if (!merged_runs || !merged_again) return failures;

    level_t const merged = level_of<level_t>(*runs, 1, *merged_runs);
    level_t const merged_warm = level_of<level_t>(*runs, 1, *merged_again);
    failures += expect_eq(merged.population_count(), std::uint64_t {older_keys_k + newer_keys_k / 2});
    failures += expect_eq(merged_warm.population_count(), merged.population_count());
    st::vector<byte_t> values;
    st::vector<byte_t> values_again;
    failures += collect_level_values(merged, guard->channel(), values);
    failures += collect_level_values(merged_warm, guard->channel(), values_again);
    failures += expect_eq(values_again.size(), values.size());
    if (values_again.size() == values.size())
        failures += expect_eq(std::memcmp(values.data(), values_again.data(), values.size()), 0);
    for (std::size_t index = 0; index != seen.count; ++index) failures += expect_eq(seen.hits[index], std::size_t {0});
    for (std::size_t index = 0; index != seen_again.count; ++index)
        failures += expect_eq(seen_again.hits[index], std::size_t {0});
    runs->discard(*volume, *merged_runs);
    runs->discard(*volume, *merged_again);
    runs->discard(*volume, upper.runs());
    return failures;
}

#pragma endregion Allocation Cases

#pragma region Depth Cases

/**
 *  @brief A batched read of many keys across several runs sits at the queue pair's depth through the middle.
 *
 *  The keys are spread over four level-zero runs, so what is held to the depth is a read that crosses
 *  runs rather than one run's own values read back to back.
 */
template <typename mount_type_>
std::size_t batched_read_holds_its_depth(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 1024;
    constexpr std::size_t flushes_k = 4;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<run_pool_t> runs = run_pool_t::make(run_records_k, (*fixture)->resources->memory_constraint);
    failures += expect(runs.has_value());
    if (!runs) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config(), *runs, slot_bytes_k);
    failures += expect(tree.has_value());
    if (!tree) return failures;

    st::vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index);
        entry.state = lookup_result_t::written_k;
        entry.bytes = 1 + index % 4093;
        entry.seed = index + 7;
        model[index] = entry;
    }
    for (std::size_t flush = 0; flush != flushes_k; ++flush) {
        st::vector<expectation_t> batch;
        for (std::size_t index = flush; index < keys_k; index += flushes_k)
            failures += expect(batch.push_back(expectation_t {model[index]}) == st::status_t::success_k);
        failures += flush_batch(*tree, guard->channel(), std::span<expectation_t const> {batch});
        if (failures != 0) return failures;
    }

    st::vector<stored_key_t> keys;
    st::vector<lookup_result_t> results;
    failures += expect(keys.resize(keys_k) == st::status_t::success_k);
    failures += expect(results.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) keys[index] = key_of(model[index].stored_key);

    // The blocks a batched read holds are counted in the pool, since the ring performs a channel's
    // requests inside the poll that hands them to the sink.
    std::uint32_t idlest = lsm_config_k.request_buffers;
    std::size_t mismatches = 0;
    auto counting = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        std::uint32_t const free_blocks = guard->channel().requests().available_buffers();
        if (free_blocks < idlest) idlest = free_blocks;
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(model[request].seed, offset + index)) ++mismatches;
    };

    typename tree_t::level_set_t const& taken = tree->levels();
    failures += expect_eq(taken.level(0).size(), flushes_k);
    failures += expect(tree->read_values(guard->channel(), taken, std::span<stored_key_t const> {keys},
                                         std::span<lookup_result_t> {results}, counting)
                           .succeeded());
    failures += expect_eq(mismatches, std::size_t {0});
    failures += expect_eq(lsm_config_k.request_buffers - idlest, lsm_config_k.queue_depth);
    for (std::size_t index = 0; index != keys_k; ++index)
        failures += expect(results[index] == lookup_result_t::written_k);
    return failures;
}

#pragma endregion Depth Cases

#pragma region Strings Merges

/** Run types over strings, whose keys the heads hold first and a tape holds whole. */
template <typename fixture_type_>
struct strings_types {
    using volume_t = typename fixture_type_::volume_t;
    using key_slot_t = key_slot<st::key128_t, value_layout_t::uuid_k, key_tails_t::carried_k>;
    using sorted_run_t = lsm_sorted_run<key_slot_t, volume_t, waiter_t>;
    using merger_t = lsm_level_merger<key_slot_t, volume_t, atomic_reference, waiter_t>;
    using run_pool_t = typename merger_t::run_pool_t;
    using run_handle_t = typename merger_t::run_handle_t;
};

/** Sections and items of the URL corpus, whose URLs share 36 bytes and tie on their
 *  heads three deep. */
constexpr std::size_t url_sections_k = 40;
constexpr std::size_t url_items_k = 150;
constexpr std::size_t urls_k = url_sections_k * url_items_k;

/** The URL at @p index of the corpus, which ascends with its index, written into @p buffer. */
[[nodiscard]] std::string_view url_of(std::size_t index, std::span<char, 64> buffer) noexcept {
    auto const written = fmt::format_to_n(buffer.data(), buffer.size(),
                                          "https://example.org/catalog/section-{:03}/item-{:05}", index / url_items_k,
                                          index % url_items_k);
    return std::string_view {buffer.data(), static_cast<std::size_t>(written.out - buffer.data())};
}

/** One version a source run carries for the URL at @c index. */
struct url_version_t {
    std::size_t index = 0;
    std::uint64_t stamp = 0;
    delta_kind_t kind = delta_kind_t::write_k;
};

/** Writes @p versions, ascending by URL, as one run of strings taken into @p runs, its values
 *  painted by stamp. */
template <typename types_type_>
[[nodiscard]] std::size_t write_url_run(typename types_type_::run_pool_t& runs, typename types_type_::volume_t& volume,
                                        io_channel_t& channel, std::span<url_version_t const> versions,
                                        st::vector<typename types_type_::run_handle_t>& into) {
    using key_slot_t = typename types_type_::key_slot_t;
    using sorted_run_t = typename types_type_::sorted_run_t;
    std::size_t failures = 0;
    std::size_t const count = versions.size();
    st::vector<key_slot_t> key_slots;
    st::vector<std::uint64_t> stamps;
    st::vector<std::uint8_t> kinds;
    st::vector<std::uint32_t> tail_offsets;
    st::vector<byte_t> tails;
    failures += expect(key_slots.resize(count) == st::status_t::success_k);
    failures += expect(stamps.resize(count) == st::status_t::success_k);
    failures += expect(kinds.resize(count) == st::status_t::success_k);
    failures += expect(tail_offsets.resize(count + 1) == st::status_t::success_k);
    if (failures != 0) return failures;
    std::uint32_t tails_bytes = 0;
    for (std::size_t position = 0; position != count; ++position) {
        char buffer[64];
        std::string_view const text = url_of(versions[position].index, buffer);
        key_slot_t slot;
        slot.stored_key = strings::head_of(text);
        for (std::size_t byte = 0; byte != key_slot_t::size; ++byte)
            slot.data[byte] = painted(versions[position].stamp, byte);
        key_slots[position] = slot;
        stamps[position] = versions[position].stamp;
        kinds[position] = static_cast<std::uint8_t>(versions[position].kind);
        tail_offsets[position] = tails_bytes;
        std::string_view const tail = strings::tail_of(text);
        failures += expect(tails.resize(tails_bytes + tail.size()) == st::status_t::success_k);
        if (failures != 0) return failures;
        std::memcpy(tails.data() + tails_bytes, tail.data(), tail.size());
        tails_bytes += static_cast<std::uint32_t>(tail.size());
    }
    tail_offsets[count] = tails_bytes;
    expected<sorted_run_t> run = sorted_run_t::write(
        volume, channel, uuid_t {}, uuid_generator(), std::span<key_slot_t const> {key_slots},
        std::span<std::uint64_t const> {stamps}, std::span<std::uint8_t const> {kinds}, 0, {}, {}, key_slot_t::size,
        key_tails_column_t {std::span<std::uint32_t const> {tail_offsets}, std::span<byte_t const> {tails}});
    failures += expect(run.has_value());
    if (!run) return failures;
    auto [taken, take_status] = runs.take(std::move(*run));
    failures += expect(take_status.succeeded());
    if (take_status.failed()) return failures;
    failures += expect(into.push_back(std::move(taken)) == st::status_t::success_k);
    return failures;
}

/** Compares the runs @p merged, in order, with @p wanted, ascending by URL and newest first within
 *  one, and checks every run stays under @p run_bytes_limit and orders wholly before the next. */
template <typename types_type_>
[[nodiscard]] std::size_t check_url_runs(typename types_type_::run_pool_t const& runs,
                                         std::span<typename types_type_::run_handle_t const> merged,
                                         std::span<url_version_t const> wanted, std::uint64_t run_bytes_limit) {
    using sorted_run_t = typename types_type_::sorted_run_t;
    std::size_t failures = 0;
    std::size_t at = 0;
    std::size_t mismatches = 0;
    sorted_run_t const* previous = nullptr;
    for (auto const& handle : merged) {
        sorted_run_t const& run = *runs.at(handle);
        failures += expect(run.size_bytes() <= run_bytes_limit);
        failures += expect_eq(run.size_bytes(), sorted_run_t::strings_layout_t::blob_bytes(run.header()));
        if (previous != nullptr) failures += expect(previous->precedes(run));
        previous = &run;
        for (std::uint64_t position = 0; position != run.population_count(); ++position, ++at) {
            if (at == wanted.size()) return failures + expect(false);
            char buffer[64];
            std::string_view const text = url_of(wanted[at].index, buffer);
            std::span<byte_t const> const value = run.value(position);
            mismatches += run.key(position) != text || run.stamp(position) != wanted[at].stamp ||
                          run.kind(position) != wanted[at].kind || value[3] != painted(wanted[at].stamp, 3);
        }
    }
    failures += expect_eq(at, wanted.size());
    failures += expect_eq(mismatches, std::size_t {0});
    return failures;
}

/** Ranks every version of @p sources by @c strings_merge_verdict, compiled for the host, as the
 *  device kernel ranks them, and compares the survivors in merged order with @p wanted, which the
 *  host merge wrote. */
template <typename sorted_run_type_>
[[nodiscard]] std::size_t check_device_ranking(std::span<sorted_run_type_ const* const> sources,
                                               std::uint32_t upper_runs, compaction_kind_t kind,
                                               compaction_stamps_t const& stamps,
                                               std::span<url_version_t const> wanted) {
    std::size_t failures = 0;
    std::size_t records = 0;
    for (sorted_run_type_ const* source : sources) records += source->population_count();
    st::vector<st::key128_t> heads;
    st::vector<st::vector<std::uint64_t>> saved;
    st::vector<strings_merge_run> runs;
    failures += expect(heads.resize(records) == st::status_t::success_k);
    failures += expect(saved.resize(2 * sources.size()) == st::status_t::success_k);
    failures += expect(runs.resize(sources.size()) == st::status_t::success_k);
    if (failures != 0) return failures;
    std::size_t first = 0;
    for (std::size_t index = 0; index != sources.size(); ++index) {
        sorted_run_type_ const& source = *sources[index];
        std::size_t const count = source.population_count();
        for (std::size_t position = 0; position != count; ++position) heads[first + position] = source.head(position);
        st::vector<std::uint64_t>& stamps_words = saved[2 * index];
        st::vector<std::uint64_t>& kinds_words = saved[2 * index + 1];
        failures += expect(stamps_words.resize(source.stamps().saved_bytes() / sizeof(std::uint64_t)) ==
                           st::status_t::success_k);
        failures += expect(kinds_words.resize(source.kinds().saved_bytes() / sizeof(std::uint64_t)) ==
                           st::status_t::success_k);
        if (failures != 0) return failures;
        failures += expect(source.stamps().save(std::as_writable_bytes(std::span<std::uint64_t> {stamps_words})) ==
                           st::status_t::success_k);
        failures += expect(source.kinds().save(std::as_writable_bytes(std::span<std::uint64_t> {kinds_words})) ==
                           st::status_t::success_k);
        runs[index] = strings_merge_run {heads.data() + first,
                                         source.key_offsets().data(),
                                         source.key_tape().data(),
                                         stamps_words.data(),
                                         kinds_words.data(),
                                         first,
                                         count};
        first += count;
    }

    st::vector<strings_version_t> merged;
    st::vector<std::uint8_t> flags;
    failures += expect(merged.resize(records) == st::status_t::success_k);
    failures += expect(flags.resize(records, std::uint8_t {2}) == st::status_t::success_k);
    if (failures != 0) return failures;
    std::size_t collisions = 0;
    for (std::uint32_t run = 0; run != runs.size(); ++run)
        for (std::uint64_t position = 0; position != runs[run].count; ++position) {
            strings_merge_verdict_t const verdict = strings_merge_verdict(
                runs.data(), static_cast<std::uint32_t>(runs.size()), upper_runs, run, position, kind, stamps);
            if (verdict.rank >= records || flags[verdict.rank] != 2) {
                ++collisions;
                continue;
            }
            merged[verdict.rank] = strings_version_t {run, static_cast<std::uint32_t>(position)};
            flags[verdict.rank] = verdict.survives ? 1 : 0;
        }
    failures += expect_eq(collisions, std::size_t {0});

    std::size_t at = 0;
    std::size_t mismatches = 0;
    for (std::size_t rank = 0; rank != records; ++rank) {
        if (flags[rank] != 1) continue;
        if (at == wanted.size()) return failures + expect(false);
        sorted_run_type_ const& source = *sources[merged[rank].source];
        char buffer[64];
        mismatches += source.key(merged[rank].position) != url_of(wanted[at].index, buffer) ||
                      source.stamp(merged[rank].position) != wanted[at].stamp ||
                      source.kind(merged[rank].position) != wanted[at].kind;
        ++at;
    }
    failures += expect_eq(at, wanted.size());
    failures += expect_eq(mismatches, std::size_t {0});
    return failures;
}

/**
 *  @brief Strings merges over URLs with long shared prefixes keep the versions a reference keeps,
 *      in one merge or two, cut into runs under their limit that order wholly one after another.
 *
 *  Three overlapping upper runs, the newest removing some URLs, fold into two lower runs. With
 *  a reader pinned between the lower and the upper stamps, a URL both carry keeps two
 *  versions, newest first.
 */
template <typename mount_type_>
std::size_t strings_merges_match_reference(mount_type_ const& mount) {
    using types_t = strings_types<typename mount_type_::fixture_t>;
    using fixture_t = typename mount_type_::fixture_t;
    using guard_t = typename fixture_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using merger_t = typename types_t::merger_t;
    using run_pool_t = typename types_t::run_pool_t;
    using run_handle_t = typename types_t::run_handle_t;
    using sorted_run_t = typename types_t::sorted_run_t;
    constexpr std::size_t upper_runs_k = 3;
    constexpr std::uint64_t lower_stamps_k = 100000;
    constexpr std::uint64_t run_bytes_limit_k = 64ull * 1024ull;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;
    expected<guard_t> guard = (*fixture)->channels.acquire();
    expected<guard_t> reading = (*fixture)->channels.acquire();
    expected<guard_t> writing = (*fixture)->channels.acquire();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<run_pool_t> runs = run_pool_t::make(run_records_k, (*fixture)->resources->memory_constraint);
    failures += expect(runs.has_value());
    if (!runs) return failures;

    // Lower runs hold the even URLs in two halves; upper run u holds every (u + 3)-th URL, stamped
    // newer per run.
    auto upper_stamp = [](std::size_t run, std::size_t index) noexcept { return lower_stamps_k * (run + 2) + index; };
    st::vector<run_handle_t> lower;
    st::vector<run_handle_t> upper;
    for (std::size_t half = 0; half != 2; ++half) {
        st::vector<url_version_t> versions;
        for (std::size_t index = half * urls_k / 2; index != (half + 1) * urls_k / 2; index += 2)
            failures += expect(versions.push_back(url_version_t {index, 1 + index}) == st::status_t::success_k);
        failures += write_url_run<types_t>(*runs, *volume, guard->channel(), versions, lower);
    }
    for (std::size_t run = 0; run != upper_runs_k; ++run) {
        st::vector<url_version_t> versions;
        for (std::size_t index = 0; index < urls_k; index += run + 3) {
            delta_kind_t const kind = run + 1 == upper_runs_k && index % 7 == 0 ? delta_kind_t::remove_k
                                                                                : delta_kind_t::write_k;
            failures += expect(versions.push_back(url_version_t {index, upper_stamp(run, index), kind}) ==
                               st::status_t::success_k);
        }
        failures += write_url_run<types_t>(*runs, *volume, guard->channel(), versions, upper);
    }
    if (failures != 0) return failures;

    // The reference for a merge shaped as @p merged: every URL's newest version, and under a pin,
    // its newest lower one too where an upper exists.
    auto reference = [&](typename merger_t::merge_request_t const& merged, st::vector<url_version_t>& wanted) {
        bool const bottom = merged.depth == lsm_level_depth_t::bottom_k;
        bool const pinned = merged.stamps.pinned_count != 0;
        for (std::size_t index = 0; index != urls_k; ++index) {
            bool upper_found = false;
            url_version_t newest;
            for (std::size_t run = upper_runs_k; run-- != 0 && !upper_found;)
                if (index % (run + 3) == 0) {
                    bool const removes = run + 1 == upper_runs_k && index % 7 == 0;
                    newest = url_version_t {index, upper_stamp(run, index),
                                            removes ? delta_kind_t::remove_k : delta_kind_t::write_k};
                    upper_found = true;
                }
            bool const lower_found = index % 2 == 0;
            if (upper_found && !(bottom && newest.kind == delta_kind_t::remove_k)) [[maybe_unused]]
                st::status_t const kept = wanted.push_back(url_version_t {newest});
            if (lower_found && (!upper_found || pinned)) [[maybe_unused]]
                st::status_t const kept = wanted.push_back(url_version_t {index, 1 + index});
        }
    };
    st::vector<sorted_run_t const*> upper_sources;
    st::vector<sorted_run_t const*> lower_sources;
    for (run_handle_t const& handle : upper)
        failures += expect(upper_sources.push_back(runs->at(handle)) == st::status_t::success_k);
    for (run_handle_t const& handle : lower)
        failures += expect(lower_sources.push_back(runs->at(handle)) == st::status_t::success_k);
    st::vector<sorted_run_t const*> all_sources;
    for (sorted_run_t const* source : upper_sources)
        failures += expect(all_sources.push_back(static_cast<sorted_run_t const*>(source)) == st::status_t::success_k);
    for (sorted_run_t const* source : lower_sources)
        failures += expect(all_sources.push_back(static_cast<sorted_run_t const*>(source)) == st::status_t::success_k);

    typename merger_t::merge_request_t request;
    request.upper = upper_sources;
    request.upper_order = lsm_level_order_t::overlapping_k;
    request.lower = lower_sources;
    request.value_max_size = types_t::key_slot_t::size;
    request.run_bytes_limit = run_bytes_limit_k;
    request.placement = compaction_placement_t::host_k;

    // One merge at the bottom drops the removals and every shadowed lower version.
    request.depth = lsm_level_depth_t::bottom_k;
    expected<st::vector<run_handle_t>> once = merger_t::merge(*volume, *runs, reading->channel(), writing->channel(),
                                                              request, last_write_fold_t {}, no_device_fold_t {});
    failures += expect_status(once, status_code_t::success_k);
    if (!once) return failures;
    failures += expect(once->size() > 1);
    st::vector<url_version_t> wanted;
    reference(request, wanted);
    failures += check_url_runs<types_t>(*runs, *once, wanted, run_bytes_limit_k);
    failures += check_device_ranking<sorted_run_t>(
        std::span<sorted_run_t const* const> {all_sources}, std::uint32_t {upper_runs_k},
        compaction_kind_t::overlapping_into_bottom_k, request.stamps, wanted);

    // The upper runs merged first and then into the lower ones end in the very same versions.
    request.lower = {};
    request.depth = lsm_level_depth_t::intermediate_k;
    expected<st::vector<run_handle_t>> folded = merger_t::merge(*volume, *runs, reading->channel(), writing->channel(),
                                                                request, last_write_fold_t {}, no_device_fold_t {});
    failures += expect_status(folded, status_code_t::success_k);
    if (!folded) return failures;
    st::vector<sorted_run_t const*> folded_sources;
    for (run_handle_t const& handle : *folded)
        failures += expect(folded_sources.push_back(runs->at(handle)) == st::status_t::success_k);
    request.upper = folded_sources;
    request.upper_order = lsm_level_order_t::sorted_k;
    request.lower = lower_sources;
    request.depth = lsm_level_depth_t::bottom_k;
    expected<st::vector<run_handle_t>> twice = merger_t::merge(*volume, *runs, reading->channel(), writing->channel(),
                                                               request, last_write_fold_t {}, no_device_fold_t {});
    failures += expect_status(twice, status_code_t::success_k);
    if (!twice) return failures;
    failures += check_url_runs<types_t>(*runs, *twice, wanted, run_bytes_limit_k);

    // A reader pinned between the lower and the upper stamps keeps both versions of a
    // URL, newest first.
    std::uint64_t const pins[] {lower_stamps_k};
    request.upper = upper_sources;
    request.upper_order = lsm_level_order_t::overlapping_k;
    request.depth = lsm_level_depth_t::intermediate_k;
    request.stamps.pinned = pins;
    request.stamps.pinned_count = 1;
    expected<st::vector<run_handle_t>> pinned = merger_t::merge(*volume, *runs, reading->channel(), writing->channel(),
                                                                request, last_write_fold_t {}, no_device_fold_t {});
    failures += expect_status(pinned, status_code_t::success_k);
    if (!pinned) return failures;
    st::vector<url_version_t> wanted_pinned;
    reference(request, wanted_pinned);
    failures += check_url_runs<types_t>(*runs, *pinned, wanted_pinned, run_bytes_limit_k);
    failures += check_device_ranking<sorted_run_t>(
        std::span<sorted_run_t const* const> {all_sources}, std::uint32_t {upper_runs_k},
        compaction_kind_t::overlapping_into_intermediate_k, request.stamps, wanted_pinned);

    runs->discard(*volume, *once);
    runs->discard(*volume, *folded);
    runs->discard(*volume, *twice);
    runs->discard(*volume, *pinned);
    runs->discard(*volume, upper);
    runs->discard(*volume, lower);
    failures += expect_eq(runs->size(), std::size_t {0});
    return failures;
}

#pragma endregion Strings Merges

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

    report(totals, "lsm.compactor_choice", compactor_choice_follows_the_threshold);
    auto const cases = [&](auto const& mount) {
        report(totals, mount, "flush_lands_on_level_zero", flush_lands_on_level_zero);
        report(totals, mount, "restore_assigns_ingested_stamps", restore_assigns_ingested_stamps);
        report(totals, mount, "overlapping_takes_youngest", overlapping_runs_take_the_youngest);
        report(totals, mount, "drops_remove_deltas_at_bottom", compaction_drops_remove_deltas_at_the_bottom);
        report(totals, mount, "intermediate_keeps_remove_deltas", intermediate_merge_keeps_remove_deltas);
        report(totals, mount, "cancelled_leaves_nothing", cancelled_compaction_leaves_nothing);
        report(totals, mount, "raced_cancellation", raced_cancellation_is_all_or_nothing);
        report(totals, mount, "trivial_move_writes_nothing", trivial_move_writes_nothing);
        report(totals, mount, "stale_run_handle", stale_run_handle_resolves_to_nothing);
        report(totals, mount, "history_matches_reference", compaction_history_matches_reference);
        report(totals, mount, "compactors_agree", both_compactors_agree);
        report(totals, mount, "merge_charges_no_index_copy", merge_charges_no_index_copy);
        report(totals, mount, "holds_its_depth", batched_read_holds_its_depth);
        report(totals, mount, "strings_merges_match_reference", strings_merges_match_reference);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, lsm_config_k, 1, "lsm", cases);

    return suite_exit_code(totals);
}
