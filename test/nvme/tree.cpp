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
#include "ustore/utilities/ram_device.hpp"         // `spawn_pool`, `harvest_topology`
#include "ustore/utilities/vram_device.hpp"        // `gpu_drivers_t`

#include "nvme/allocation_watch.hpp" // `allocation_watch_t`, `allocation_watch_g`
#include "nvme/fixture.hpp"          // `run_on_transport`, `expectation_t`, `painted`
#include "vram/classes.hpp"          // `kernel_tables`, `pick_gpu_class`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;

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
 *  merges run on the kernels the library's bundle holds. */
using stored_key_t = st::key128_t;
using variable_slot_t = key_slot<stored_key_t, value_layout_t::variable_width_k>;

/** The key of integer @p index, as the engine reads an identity below 2^64. */
constexpr stored_key_t key_of(std::uint64_t index) noexcept { return {0, index}; }

/** The runs, the levels and the trees every case drives over the volume of @p fixture_type_. */
template <typename fixture_type_>
struct lsm_types {
    using fixture_t = fixture_type_;
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
    st::vector<util::byte_t> values;

    /** Where each value starts inside @c values and one past the last. */
    st::vector<stored_value_offset_t> offsets;

    /** The @c delta_kind_t of every key slot. */
    st::vector<std::uint8_t> kinds;
};

/** Lays @p wanted out as one run's key slots, kinds and values, which must already be in ascending
 *  key order, answering whether every buffer fit. */
bool build_batch(std::span<expectation_t const> wanted, batch_t& into) {
    if (!expect_true(into.key_slots.resize(wanted.size()) == st::status_t::success_k) ||
        !expect_true(into.kinds.resize(wanted.size()) == st::status_t::success_k) ||
        !expect_true(into.offsets.resize(wanted.size() + 1) == st::status_t::success_k))
        return false;

    std::uint64_t total = 0;
    for (std::size_t index = 0; index != wanted.size(); ++index)
        if (wanted[index].state == lookup_result_t::written_k) total += wanted[index].bytes;
    if (!expect_true(into.values.resize(static_cast<std::size_t>(total)) == st::status_t::success_k)) return false;

    std::uint64_t written = 0;
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        expectation_t const& entry = wanted[index];
        bool const removed = entry.state == lookup_result_t::removed_k;
        variable_slot_t slot;
        slot.stored_key = key_of(entry.stored_key);
        into.offsets[index] = static_cast<stored_value_offset_t>(written);
        into.kinds[index] = static_cast<std::uint8_t>(removed ? util::delta_kind_t::remove_k
                                                              : util::delta_kind_t::write_k);
        if (!removed) {
            for (std::uint64_t byte = 0; byte != entry.bytes; ++byte)
                into.values[static_cast<std::size_t>(written + byte)] = painted(entry.seed, byte);
            written += entry.bytes;
        }
        into.key_slots[index] = slot;
    }
    into.offsets[wanted.size()] = static_cast<stored_value_offset_t>(written);
    return true;
}

/** Flushes @p wanted onto level zero of @p tree as one run, answering whether it landed. */
template <typename tree_type_>
bool flush_batch(tree_type_& tree, util::io_channel_t& channel, std::span<expectation_t const> wanted) {
    batch_t batch;
    if (!build_batch(wanted, batch)) return false;
    return expect_true(tree.flush(channel, std::span<variable_slot_t const> {batch.key_slots},
                                  std::span<std::uint64_t const> {fresh_stamps(batch.key_slots.size())},
                                  std::span<std::uint8_t const> {batch.kinds}, 0,
                                  std::span<util::byte_t const> {batch.values},
                                  std::span<stored_value_offset_t const> {batch.offsets}, value_max_size_k)
                           .succeeded());
}

/** Reads every key of @p wanted out of @p tree and counts what it does not answer as the model says. */
template <typename tree_type_>
void check_model(tree_type_ const& tree, util::io_channel_t& channel, std::span<expectation_t const> wanted) {
    std::size_t const count = wanted.size();
    st::vector<stored_key_t> keys;
    st::vector<lookup_result_t> results;
    st::vector<std::uint64_t> seen;
    expect_true(keys.resize(count) == st::status_t::success_k);
    expect_true(results.resize(count) == st::status_t::success_k);
    if (!expect_true(seen.resize(count) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != count; ++index) {
        keys[index] = key_of(wanted[index].stored_key);
        results[index] = lookup_result_t::missing_k;
        seen[index] = 0;
    }

    // Every key is looked up once, and the keys found are read where the lookups found them
    typename tree_type_::level_set_t const& taken = tree.levels();
    st::vector<lsm_tree_position_t> positions;
    st::vector<std::size_t> located;
    for (std::size_t index = 0; index != count; ++index) {
        lsm_tree_position_t position;
        results[index] = tree.find(taken, keys[index], position);
        if (results[index] != lookup_result_t::written_k) continue;
        expect_true(positions.push_back(lsm_tree_position_t {position}) == st::status_t::success_k);
        expect_true(located.push_back(std::size_t {index}) == st::status_t::success_k);
    }
    std::size_t mismatches = 0;
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<util::byte_t const> bytes) noexcept {
        std::size_t const index = located[request];
        seen[index] += bytes.size();
        for (std::size_t at = 0; at != bytes.size(); ++at)
            if (bytes[at] != painted(wanted[index].seed, offset + at)) ++mismatches;
    };
    ustore_arena_handle_t scratch {};
    expect_true(tree.read_located(channel, taken, std::span<lsm_tree_position_t const> {positions}, &scratch, sink)
                    .has_value());
    util::arena_free_blocks(scratch);
    expect_equal(mismatches, std::size_t {0});
    for (std::size_t index = 0; index != count; ++index) {
        switch (wanted[index].state) {
        case lookup_result_t::missing_k:
        case lookup_result_t::removed_k: expect_true(results[index] == wanted[index].state); break;
        case lookup_result_t::written_k:
            expect_true(results[index] == lookup_result_t::written_k);
            expect_equal(seen[index], wanted[index].bytes);
            break;
        // A generated key never asks for a short read or a change, so these states mean the generator broke.
        case lookup_result_t::changed_k:
        case lookup_result_t::incomplete_k: expect_true(false); break;
        }
    }
}

/** Reads every value of every run of @p level back to back, which is what two compactors must agree on. */
template <typename level_type_>
void collect_level_values(level_type_ const& level, util::io_channel_t& channel, st::vector<util::byte_t>& into) {
    for (std::size_t run = 0; run != level.size(); ++run) {
        auto const& source = level.run(run);
        std::size_t const count = static_cast<std::size_t>(source.population_count());
        st::vector<std::uint64_t> positions;
        st::vector<std::size_t> starts;
        expect_true(positions.resize(count) == st::status_t::success_k);
        if (!expect_true(starts.resize(count) == st::status_t::success_k)) return;
        std::size_t const base = into.size();
        std::size_t total = 0;
        for (std::size_t at = 0; at != count; ++at) {
            positions[at] = at;
            starts[at] = base + total;
            total += source.value_window(at).bytes;
        }
        if (!expect_true(into.resize(base + total) == st::status_t::success_k)) return;

        // Completions arrive in the transport's order, so every piece lands by its request and offset.
        auto sink = [&](std::size_t request, std::uint64_t offset, std::span<util::byte_t const> bytes) noexcept {
            std::memcpy(into.data() + starts[request] + offset, bytes.data(), bytes.size());
        };
        expect_true(source.read_values(channel, std::span<std::uint64_t const> {positions}, sink).succeeded());
    }
}

/** Writes @p wanted as one run of @p volume, takes it into @p runs and names it in @p level, which
 *  is how a case builds a level, answering whether the level names it. */
template <typename level_type_, typename volume_type_>
bool adopt_batch(level_type_& level, typename level_type_::run_pool_t& runs, volume_type_& volume,
                 util::io_channel_t& channel, std::span<expectation_t const> wanted) {
    using sorted_run_t = typename level_type_::sorted_run_t;
    batch_t batch;
    if (!build_batch(wanted, batch)) return false;
    auto [written, written_status] = sorted_run_t::write(
        volume, channel, util::uuid128_t {}, util::uuid_generator(), std::span<variable_slot_t const> {batch.key_slots},
        std::span<std::uint64_t const> {fresh_stamps(batch.key_slots.size())},
        std::span<std::uint8_t const> {batch.kinds}, 0, std::span<util::byte_t const> {batch.values},
        std::span<stored_value_offset_t const> {batch.offsets}, value_max_size_k);
    if (!expect_true(written_status.succeeded())) return false;
    auto [handle, take_status] = runs.take(std::move(written));
    if (!expect_true(take_status.succeeded())) return false;
    return expect_true(level.insert_run(handle).succeeded());
}

/** A sorted level numbered @p number naming the runs a merge wrote, which is how a case
 *  reads them back. */
template <typename level_type_>
[[nodiscard]] level_type_ level_of(typename level_type_::run_pool_t& runs, std::uint32_t number,
                                   std::span<typename level_type_::run_handle_t const> merged) {
    level_type_ level = level_type_::of(runs, number, lsm_level_order_t::sorted_k);
    for (typename level_type_::run_handle_t const handle : merged) [[maybe_unused]]
        util::status_t const placed = level.insert_run(handle);
    return level;
}

/** Runs every slice of a merge in turn on the calling thread over one pair of channels, so a case
 *  sees what slicing changes apart from how the slices are scheduled. */
struct sequential_fan_out_t {

    /** Slices one merge is cut into. */
    std::size_t slices_count = 1;

    /** The channel every slice gathers its values on. */
    util::io_channel_t* reading = nullptr;

    /** The channel every slice writes its runs on. */
    util::io_channel_t* writing = nullptr;

    [[nodiscard]] std::size_t slices() const noexcept { return slices_count; }

    util::status_t run(compaction_fan_out_t::slice_call_t const& slice) const noexcept {
        for (std::size_t index = 0; index != slices_count; ++index)
            if (util::status_t const ran = slice(index, *reading, *writing); ran.failed()) return ran;
        return {};
    }
};

/** Runs every slice of a merge on its own thread of a ForkUnion pool, each on both channels of its
 *  own thread, as a blocking compact on an executor fans out. */
template <typename disk_type_>
struct pooled_fan_out {

    /** The pool whose threads run the slices, one slice per thread. */
    fu::pool_t& pool;

    /** The queue set every slice registers the disk's media on. */
    util::nvme_queues_t* queues = nullptr;

    /** The disk the slices read and write. */
    disk_type_* disk = nullptr;

    /** The thread slice 0 runs on, past the threads whose channels the case holds itself. */
    std::size_t first_thread = 0;

    [[nodiscard]] std::size_t slices() const noexcept { return pool.threads_count(); }

    util::status_t run(compaction_fan_out_t::slice_call_t const& slice) const noexcept {
        st::vector<util::status_t> statuses;
        if (statuses.resize(slices()) != st::status_t::success_k)
            return {util::status_code_t::out_of_memory_k, "No room for the slices' statuses"};
        auto run_slice = [&](std::size_t task, std::size_t thread) noexcept {
            auto [channels, media_status] = queues->both_channels_with_disk_media(first_thread + thread, *disk);
            if (media_status.failed()) {
                statuses[task] = media_status;
                return;
            }
            statuses[task] = slice(task, *channels.reading, *channels.writing);
        };
        if (fu::failed(pool.for_n_dynamic(statuses.size(), run_slice)))
            return {util::status_code_t::contended_k, "The pool refused the slices"};
        for (util::status_t const& status : statuses)
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
void test_flush_lands_on_level_zero(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    constexpr std::size_t count_k = 600;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    auto [volume, mount_volume_status] = fixture->mount_volume(*guard);
    expect_true(mount_volume_status.succeeded());
    if (mount_volume_status.failed()) return;
    auto [runs, runs_status] = run_pool_t::make(run_records_k, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [tree, tree_status] = tree_t::mount(volume, test_tree_config(), runs, slot_bytes_k);
    expect_true(tree_status.succeeded());
    if (tree_status.failed()) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(count_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index) * 2 + 1;
        entry.state = index % 7 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
        entry.bytes = entry.state == lookup_result_t::written_k ? 1 + index % 1021 : 0;
        entry.seed = index;
        model[index] = entry;
    }

    if (!flush_batch(tree, *guard, std::span<expectation_t const> {model})) return;

    {
        typename tree_t::level_set_t const& taken = tree.levels();
        expect_equal(taken.size(), std::size_t {1});
        expect_equal(taken.level(0).size(), std::size_t {1});
        expect_true(taken.level(0).order() == lsm_level_order_t::overlapping_k);
        expect_equal(taken.population_count(), static_cast<std::uint64_t>(count_k));
    }
    check_model(tree, *guard, std::span<expectation_t const> {model});

    // Every even key is a miss, since the model only ever wrote odd ones.
    st::vector<expectation_t> absent;
    if (!expect_true(absent.resize(count_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index) * 2 + 2;
        entry.state = lookup_result_t::missing_k;
        absent[index] = entry;
    }
    check_model(tree, *guard, std::span<expectation_t const> {absent});
}

/** A run written at stamp zero and restored with the stamp an ingest assigned it is seen by a reader at that stamp and
 *  skipped by one below it, as a commit at that stamp would be. */
template <typename mount_type_>
void test_restore_assigns_ingested_stamps(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using sorted_run_t = typename types_t::sorted_run_t;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    constexpr std::size_t count_k = 64;
    constexpr std::uint64_t assigned_k = 7;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;
    auto [guard, acquire_status] = fixture->channel(0);
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    auto [volume, mount_volume_status] = fixture->mount_volume(*guard);
    expect_true(mount_volume_status.succeeded());
    if (mount_volume_status.failed()) return;
    auto [runs, runs_status] = run_pool_t::make(run_records_k, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [tree, tree_status] = tree_t::mount(volume, test_tree_config(), runs, slot_bytes_k);
    expect_true(tree_status.succeeded());
    if (tree_status.failed()) return;

    st::vector<expectation_t> model;
    st::vector<std::uint64_t> zeros;
    expect_true(model.resize(count_k) == st::status_t::success_k);
    if (!expect_true(zeros.resize(count_k) == st::status_t::success_k)) return;
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
    if (!build_batch(std::span<expectation_t const> {model}, batch)) return;
    util::uuid128_t const content_id = util::uuid_generator();
    auto [written, written_status] = sorted_run_t::write(
        volume, *guard, util::uuid128_t {}, content_id, std::span<variable_slot_t const> {batch.key_slots},
        std::span<std::uint64_t const> {zeros}, std::span<std::uint8_t const> {batch.kinds}, 0,
        std::span<util::byte_t const> {batch.values}, std::span<stored_value_offset_t const> {batch.offsets},
        value_max_size_k);
    expect_true(written_status.succeeded());
    if (written_status.failed()) return;
    expect_equal(written.assigned_stamp(), std::uint64_t {0});

    lsm_tree_run_record_t record;
    record.content_id = content_id;
    record.extent = written.extent();
    record.stamp = assigned_k;
    expect_true(tree.restore(*guard, std::span<lsm_tree_run_record_t const> {&record, 1}).succeeded());
    typename tree_t::level_set_t const& taken = tree.levels();
    expect_equal(taken.level(0).run(0).assigned_stamp(), assigned_k);
    std::size_t misread = 0;
    for (expectation_t const& entry : model) {
        lsm_tree_position_t position;
        misread += tree.find(taken, key_of(entry.stored_key), position, assigned_k - 1) != lookup_result_t::missing_k;
        misread += tree.find(taken, key_of(entry.stored_key), position, assigned_k) != lookup_result_t::written_k;
    }
    expect_equal(misread, std::size_t {0});
}

/** Several level-zero runs overlap, and the youngest version of every key is the one a read answers. */
template <typename mount_type_>
void test_overlapping_runs_take_the_youngest(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    constexpr std::size_t keys_k = 400;
    constexpr std::size_t flushes_k = 5;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    auto [volume, mount_volume_status] = fixture->mount_volume(*guard);
    expect_true(mount_volume_status.succeeded());
    if (mount_volume_status.failed()) return;
    auto [runs, runs_status] = run_pool_t::make(run_records_k, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [tree, tree_status] = tree_t::mount(volume, test_tree_config(), runs, slot_bytes_k);
    expect_true(tree_status.succeeded());
    if (tree_status.failed()) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(keys_k) == st::status_t::success_k)) return;
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
            expect_true(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
            model[index] = entry;
        }
        if (!flush_batch(tree, *guard, std::span<expectation_t const> {batch})) return;
    }

    {
        typename tree_t::level_set_t const& taken = tree.levels();
        expect_equal(taken.level(0).size(), flushes_k);
    }
    check_model(tree, *guard, std::span<expectation_t const> {model});

    // A merged walk of level zero reports every key once, in ascending order, at its youngest run.
    typename tree_t::level_set_t const& taken = tree.levels();
    std::size_t visited = 0;
    std::size_t disordered = 0;
    stored_key_t previous {};
    expect_true(taken.level(0)
                    .for_each_key_slot([&](lsm_level_position_t, variable_slot_t const& slot) noexcept {
                        if (visited != 0 && !(previous < slot.stored_key)) ++disordered;
                        previous = slot.stored_key;
                        ++visited;
                    })
                    .succeeded());
    expect_equal(disordered, std::size_t {0});
    std::size_t live_keys = 0;
    for (std::size_t index = 0; index != keys_k; ++index)
        if (model[index].state != lookup_result_t::missing_k) ++live_keys;
    expect_equal(visited, live_keys);
}

#pragma endregion Flush Cases

#pragma region Compaction Cases

/** A compaction merges level zero into level one, keeps the youngest value and forgets the remove deltas. */
template <typename mount_type_>
void test_compaction_drops_remove_deltas_at_the_bottom(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    constexpr std::size_t keys_k = 900;
    constexpr std::size_t flushes_k = 4;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    auto [reading, acquire_reading_status] = fixture->channel(1);
    auto [writing, acquire_writing_status] = fixture->channel(2);
    expect_true(acquire_status.succeeded() && acquire_reading_status.succeeded() && acquire_writing_status.succeeded());
    if (acquire_status.failed() || acquire_reading_status.failed() || acquire_writing_status.failed()) return;
    auto [volume, mount_volume_status] = fixture->mount_volume(*guard);
    expect_true(mount_volume_status.succeeded());
    if (mount_volume_status.failed()) return;
    auto [runs, runs_status] = run_pool_t::make(run_records_k, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [tree, tree_status] = tree_t::mount(volume, test_tree_config(), runs, slot_bytes_k);
    expect_true(tree_status.succeeded());
    if (tree_status.failed()) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(keys_k) == st::status_t::success_k)) return;
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
            expect_true(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
            model[index] = entry;
        }
        if (!flush_batch(tree, *guard, std::span<expectation_t const> {batch})) return;
    }

    expect_true(tree.needs_compaction(tree.levels()));
    std::array<typename tree_t::merge_buffers_t, 1> buffers {};
    auto const [outcome, outcome_status] = tree.compact(
        *reading, *writing, std::span<typename tree_t::merge_buffers_t> {buffers}, compaction_placement_t::host_k, {},
        nullptr, util::last_write_fold_t {}, no_device_fold_t {});
    expect_true(outcome_status.succeeded());
    if (outcome_status.failed()) return;
    expect_true(outcome == compaction_outcome_t::published_k);

    // The bottom level forgets a remove delta for good, so a removed key becomes a
    // key nothing holds.
    for (std::size_t index = 0; index != keys_k; ++index)
        if (model[index].state == lookup_result_t::removed_k) model[index].state = lookup_result_t::missing_k;

    {
        typename tree_t::level_set_t const& taken = tree.levels();
        expect_equal(taken.size(), std::size_t {2});
        expect_true(taken.level(0).empty());
        expect_true(!taken.level(1).empty());
        expect_true(taken.level(1).order() == lsm_level_order_t::sorted_k);
        std::size_t live_keys = 0;
        for (std::size_t index = 0; index != keys_k; ++index)
            if (model[index].state != lookup_result_t::missing_k) ++live_keys;
        expect_equal(taken.level(1).population_count(), static_cast<std::uint64_t>(live_keys));
    }
    check_model(tree, *guard, std::span<expectation_t const> {model});
}

/** The same two levels merged into an intermediate level keep every remove delta the bottom would
 *  drop, and a merge cut into four slices, in turn or on four threads at once, keeps exactly what a
 *  merge in one does. */
template <typename mount_type_>
void test_intermediate_merge_keeps_remove_deltas(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using level_t = typename types_t::level_t;
    using run_pool_t = typename types_t::run_pool_t;
    using run_handle_t = typename types_t::run_handle_t;
    using merger_t = typename types_t::merger_t;
    constexpr std::size_t keys_k = 240;
    constexpr std::size_t threads_k = 4;
    // Every thread of the pool reads and writes on its own thread's channels, past the two threads
    // the case's own three take
    fixture_config_t threaded = mount.config;
    threaded.queues_threads = 2 + threads_k;
    auto [fixture, mount_status] = mount(threaded);
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    auto [reading, acquire_reading_status] = fixture->channel(1);
    auto [writing, acquire_writing_status] = fixture->channel(2);
    expect_true(acquire_status.succeeded() && acquire_reading_status.succeeded() && acquire_writing_status.succeeded());
    if (acquire_status.failed() || acquire_reading_status.failed() || acquire_writing_status.failed()) return;
    auto [volume, mount_volume_status] = fixture->mount_volume(*guard);
    expect_true(mount_volume_status.succeeded());
    if (mount_volume_status.failed()) return;

    st::vector<expectation_t> older;
    st::vector<expectation_t> newer;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index);
        entry.state = lookup_result_t::written_k;
        entry.bytes = 1 + index % 311;
        entry.seed = index + 1;
        if (!expect_true(older.push_back(expectation_t {entry}) == st::status_t::success_k)) return;
        if (index % 3 != 0) continue;
        entry.state = lookup_result_t::removed_k;
        entry.bytes = 0;
        if (!expect_true(newer.push_back(expectation_t {entry}) == st::status_t::success_k)) return;
    }

    std::size_t removed_keys = 0;
    for (std::size_t index = 0; index != keys_k; ++index)
        if (index % 3 == 0) ++removed_keys;

    auto [runs, runs_status] = run_pool_t::make(run_records_k, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;

    // One level of two overlapping runs, merged twice with nothing changed but the depth.
    level_t upper = level_t::of(runs, 0, lsm_level_order_t::overlapping_k);
    if (!adopt_batch(upper, runs, volume, *guard, std::span<expectation_t const> {older}) ||
        !adopt_batch(upper, runs, volume, *guard, std::span<expectation_t const> {newer}))
        return;
    std::span<run_handle_t const> const sources = upper.runs();

    std::array<typename merger_t::merge_buffers_t, 4> buffers;
    typename merger_t::merge_request_t request;
    request.upper = sources;
    request.upper_order = lsm_level_order_t::overlapping_k;
    request.value_max_size = value_max_size_k;
    request.placement = compaction_placement_t::host_k;
    request.buffers = std::span<typename merger_t::merge_buffers_t> {buffers}.first(1);

    request.depth = lsm_level_depth_t::intermediate_k;
    auto [kept_runs, kept_runs_status] = merger_t::merge(volume, runs, *reading, *writing, request,
                                                         util::last_write_fold_t {}, no_device_fold_t {});
    expect_status(kept_runs_status, util::status_code_t::success_k);
    if (kept_runs_status.failed()) return;
    level_t const kept = level_of<level_t>(runs, 1, kept_runs);
    expect_equal(kept.population_count(), static_cast<std::uint64_t>(keys_k));

    // The same merge cut into four slices writes the very same versions and values, only cut
    // into more runs.
    sequential_fan_out_t sequential {4, reading, writing};
    request.buffers = buffers;
    request.fan_out = compaction_fan_out_t {sequential};
    auto [sliced_runs, sliced_runs_status] = merger_t::merge(volume, runs, *reading, *writing, request,
                                                             util::last_write_fold_t {}, no_device_fold_t {});
    request.buffers = std::span<typename merger_t::merge_buffers_t> {buffers}.first(1);
    request.fan_out = compaction_fan_out_t {};
    expect_status(sliced_runs_status, util::status_code_t::success_k);
    if (sliced_runs_status.failed()) return;
    level_t const sliced = level_of<level_t>(runs, 1, sliced_runs);
    expect_true(sliced.size() >= 4);
    expect_equal(differing_versions(kept, sliced), std::size_t {0});
    st::vector<util::byte_t> kept_values;
    st::vector<util::byte_t> sliced_values;
    collect_level_values(kept, *guard, kept_values);
    collect_level_values(sliced, *guard, sliced_values);
    expect_equal(sliced_values.size(), kept_values.size());
    if (sliced_values.size() == kept_values.size())
        expect_equal(std::memcmp(kept_values.data(), sliced_values.data(), kept_values.size()), 0);

    // And again with every slice on its own thread of a pool, side by side.
    // Without memory placement ForkUnion spawns a flat pool of plain threads, which pins nothing
    auto [topology, topology_status] = util::harvest_topology();
    auto [pool, make_pool_status] = util::spawn_pool(
        topology, threads_k, fu::caller_inclusive_k,
        static_cast<fu_capabilities_t>(fu_capabilities_all_k & ~fu_capability_place_memory_on_domain_k));
    expect_true(st::succeeded(make_pool_status));
    if (st::failed(make_pool_status)) return;
    pooled_fan_out<typename fixture_t::disk_t> pooled {pool, &fixture->queues, &fixture->disk, 2};
    expect_equal(pooled.slices(), threads_k);
    request.buffers = buffers;
    request.fan_out = compaction_fan_out_t {pooled};
    auto [threaded_runs, threaded_runs_status] = merger_t::merge(volume, runs, *reading, *writing, request,
                                                                 util::last_write_fold_t {}, no_device_fold_t {});
    request.buffers = std::span<typename merger_t::merge_buffers_t> {buffers}.first(1);
    request.fan_out = compaction_fan_out_t {};
    expect_status(threaded_runs_status, util::status_code_t::success_k);
    if (threaded_runs_status.failed()) return;
    level_t const threaded_level = level_of<level_t>(runs, 1, threaded_runs);
    expect_equal(differing_versions(kept, threaded_level), std::size_t {0});
    st::vector<util::byte_t> threaded_values;
    collect_level_values(threaded_level, *guard, threaded_values);
    expect_equal(threaded_values.size(), kept_values.size());
    if (threaded_values.size() == kept_values.size())
        expect_equal(std::memcmp(kept_values.data(), threaded_values.data(), kept_values.size()), 0);

    request.depth = lsm_level_depth_t::bottom_k;
    auto [dropped_runs, dropped_runs_status] = merger_t::merge(volume, runs, *reading, *writing, request,
                                                               util::last_write_fold_t {}, no_device_fold_t {});
    expect_true(dropped_runs_status.succeeded());
    if (dropped_runs_status.failed()) return;
    level_t const dropped = level_of<level_t>(runs, 1, dropped_runs);
    expect_equal(dropped.population_count(), static_cast<std::uint64_t>(keys_k - removed_keys));

    // The intermediate level answers a removed key as a key it holds, and the bottom one as a miss.
    for (std::size_t index = 0; index < keys_k; index += 3) {
        lsm_level_position_t position;
        expect_true(kept.find(key_of(index), position) == lookup_result_t::removed_k);
        expect_true(dropped.find(key_of(index), position) == lookup_result_t::missing_k);
    }
    for (std::size_t index = 1; index < keys_k; index += 3) {
        lsm_level_position_t position;
        expect_true(kept.find(key_of(index), position) == lookup_result_t::written_k);
        expect_true(dropped.find(key_of(index), position) == lookup_result_t::written_k);
    }

    runs.discard(volume, kept_runs);
    runs.discard(volume, sliced_runs);
    runs.discard(volume, threaded_runs);
    runs.discard(volume, dropped_runs);
    runs.discard(volume, upper.runs());
    expect_equal(runs.size(), std::size_t {0});
}

/** A cancelled compaction leaves the level set live, both pools full and not one byte of the volume out. */
template <typename mount_type_>
void test_cancelled_compaction_leaves_nothing(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    constexpr std::size_t keys_k = 700;
    constexpr std::size_t flushes_k = 4;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    auto [reading, acquire_reading_status] = fixture->channel(1);
    auto [writing, acquire_writing_status] = fixture->channel(2);
    expect_true(acquire_status.succeeded() && acquire_reading_status.succeeded() && acquire_writing_status.succeeded());
    if (acquire_status.failed() || acquire_reading_status.failed() || acquire_writing_status.failed()) return;
    auto [volume, mount_volume_status] = fixture->mount_volume(*guard);
    expect_true(mount_volume_status.succeeded());
    if (mount_volume_status.failed()) return;
    auto [runs, runs_status] = run_pool_t::make(run_records_k, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [tree, tree_status] = tree_t::mount(volume, test_tree_config(), runs, slot_bytes_k);
    expect_true(tree_status.succeeded());
    if (tree_status.failed()) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(keys_k) == st::status_t::success_k)) return;
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
            expect_true(batch.push_back(expectation_t {model[index]}) == st::status_t::success_k);
        if (!flush_batch(tree, *guard, std::span<expectation_t const> {batch})) return;
    }
    expect_true(tree.needs_compaction(tree.levels()));

    std::size_t const records_before = runs.size();
    std::uint32_t const requests_before = reading->requests().available_buffers();
    std::uint32_t const streams_before = writing->streams().available_buffers();
    std::uint64_t population_before = 0;
    std::size_t runs_before = 0;
    {
        typename tree_t::level_set_t const& taken = tree.levels();
        population_before = taken.population_count();
        runs_before = taken.level(0).size();
    }

    compaction_cancellation<atomic_reference> cancellation;
    cancellation.raise();
    std::array<typename tree_t::merge_buffers_t, 1> buffers {};
    auto const [outcome, outcome_status] = tree.compact(
        *reading, *writing, std::span<typename tree_t::merge_buffers_t> {buffers}, compaction_placement_t::host_k, {},
        &cancellation, util::last_write_fold_t {}, no_device_fold_t {});
    expect_true(outcome_status.failed());
    expect_status(outcome_status, util::status_code_t::uninitialized_k);

    // Nothing the merge took is still out: no queue pair, no block and no run record.
    expect_equal(reading->requests().available_buffers(), requests_before);
    expect_equal(writing->streams().available_buffers(), streams_before);
    expect_equal(writing->requests().available_buffers(), lsm_config_k.request_buffers);
    expect_equal(runs.size(), records_before);
    expect_equal(fixture->disk.outstanding(reading->queue_pair()), std::size_t {0});
    expect_equal(fixture->disk.outstanding(writing->queue_pair()), std::size_t {0});

    {
        typename tree_t::level_set_t const& taken = tree.levels();
        expect_equal(taken.size(), std::size_t {1});
        expect_equal(taken.level(0).size(), runs_before);
        expect_equal(taken.population_count(), population_before);
    }
    check_model(tree, *guard, std::span<expectation_t const> {model});

    // The same tree still compacts once nobody is cancelling it.
    auto const [again, again_status] = tree.compact(
        *reading, *writing, std::span<typename tree_t::merge_buffers_t> {buffers}, compaction_placement_t::host_k, {},
        nullptr, util::last_write_fold_t {}, no_device_fold_t {});
    expect_true(again_status.succeeded());
    if (again_status.succeeded()) expect_true(again == compaction_outcome_t::published_k);
    check_model(tree, *guard, std::span<expectation_t const> {model});
}

/** A compaction raced by a cancellation either publishes whole or changes nothing, and never leaks either way. */
template <typename mount_type_>
void test_raced_cancellation_is_all_or_nothing(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    constexpr std::size_t keys_k = 800;
    constexpr std::size_t flushes_k = 4;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    auto [reading, acquire_reading_status] = fixture->channel(1);
    auto [writing, acquire_writing_status] = fixture->channel(2);
    expect_true(acquire_status.succeeded() && acquire_reading_status.succeeded() && acquire_writing_status.succeeded());
    if (acquire_status.failed() || acquire_reading_status.failed() || acquire_writing_status.failed()) return;
    auto [volume, mount_volume_status] = fixture->mount_volume(*guard);
    expect_true(mount_volume_status.succeeded());
    if (mount_volume_status.failed()) return;
    auto [runs, runs_status] = run_pool_t::make(run_records_k, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [tree, tree_status] = tree_t::mount(volume, test_tree_config(), runs, slot_bytes_k);
    expect_true(tree_status.succeeded());
    if (tree_status.failed()) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(keys_k) == st::status_t::success_k)) return;
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
            expect_true(batch.push_back(expectation_t {model[index]}) == st::status_t::success_k);
        if (!flush_batch(tree, *guard, std::span<expectation_t const> {batch})) return;
    }

    std::size_t const records_before = runs.size();
    std::uint64_t population_before = 0;
    {
        typename tree_t::level_set_t const& taken = tree.levels();
        population_before = taken.population_count();
    }

    compaction_cancellation<atomic_reference> cancellation;
    std::thread raiser {[&cancellation]() noexcept {
        for (std::size_t spins = 0; spins != 64; ++spins) fu::preferred_yield_t {}(&spins, spins, std::size_t {0});
        cancellation.raise();
    }};
    std::array<typename tree_t::merge_buffers_t, 1> buffers {};
    auto const [outcome, outcome_status] = tree.compact(
        *reading, *writing, std::span<typename tree_t::merge_buffers_t> {buffers}, compaction_placement_t::host_k, {},
        &cancellation, util::last_write_fold_t {}, no_device_fold_t {});
    raiser.join();

    expect_equal(writing->streams().available_buffers(), lsm_config_k.stream_buffers);
    expect_equal(reading->streams().available_buffers(), lsm_config_k.stream_buffers);
    expect_equal(reading->requests().available_buffers(), lsm_config_k.request_buffers);
    expect_equal(fixture->disk.outstanding(writing->queue_pair()), std::size_t {0});

    typename tree_t::level_set_t const& taken = tree.levels();
    expect_equal(taken.population_count(), population_before);
    if (outcome_status.failed()) {
        expect_status(outcome_status, util::status_code_t::uninitialized_k);
        expect_equal(runs.size(), records_before);
        expect_equal(taken.size(), std::size_t {1});
    }
    else {
        expect_true(outcome == compaction_outcome_t::published_k);
        expect_equal(taken.size(), std::size_t {2});
        expect_true(taken.level(0).empty());
    }
    check_model(tree, *guard, std::span<expectation_t const> {model});
}

/** Checks every key below @p keys_count of @p tree against @p reference: missing and written ones
 *  by a batched read, removed ones by a lookup that finds the remove delta or, once a merge at the
 *  bottom dropped it, nothing. */
template <typename tree_type_, typename reference_type_>
void check_reference(tree_type_ const& tree, util::io_channel_t& channel, reference_type_ const& reference,
                     std::uint64_t keys_count) {
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
    expect_equal(mismatched, std::size_t {0});
    check_model(tree, channel, std::span<expectation_t const> {answered});
}

/** A run alone on level zero past one slot, which overlaps nothing below, moves down by
 *  its handle untouched. */
template <typename mount_type_>
void test_trivial_move_writes_nothing(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    using sorted_run_t = typename types_t::sorted_run_t;
    constexpr std::size_t keys_k = 200;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    auto [reading, acquire_reading_status] = fixture->channel(1);
    auto [writing, acquire_writing_status] = fixture->channel(2);
    expect_true(acquire_status.succeeded() && acquire_reading_status.succeeded() && acquire_writing_status.succeeded());
    if (acquire_status.failed() || acquire_reading_status.failed() || acquire_writing_status.failed()) return;
    auto [volume, mount_volume_status] = fixture->mount_volume(*guard);
    expect_true(mount_volume_status.succeeded());
    if (mount_volume_status.failed()) return;
    auto [runs, runs_status] = run_pool_t::make(run_records_k, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [tree, tree_status] = tree_t::mount(volume, test_tree_config(), runs, slot_bytes_k);
    expect_true(tree_status.succeeded());
    if (tree_status.failed()) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(keys_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index);
        entry.state = index % 9 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
        entry.bytes = entry.state == lookup_result_t::written_k ? 400 + index % 401 : 0;
        entry.seed = index + 17;
        model[index] = entry;
    }
    if (!flush_batch(tree, *guard, std::span<expectation_t const> {model})) return;

    sorted_run_t const* const flushed = &tree.levels().level(0).run(0);
    util::uuid128_t const content_id = flushed->content_id();
    expect_true(flushed->size_bytes() >= slot_bytes_k);
    expect_true(tree.needs_compaction(tree.levels()));
    std::array<typename tree_t::merge_buffers_t, 1> buffers {};
    auto const [outcome, outcome_status] = tree.compact(
        *reading, *writing, std::span<typename tree_t::merge_buffers_t> {buffers}, compaction_placement_t::host_k, {},
        nullptr, util::last_write_fold_t {}, no_device_fold_t {});
    expect_true(outcome_status.succeeded());
    if (outcome_status.failed()) return;
    expect_true(outcome == compaction_outcome_t::published_k);

    // The run below is the very record level zero named, so no byte was read or written and no
    // run was minted.
    typename tree_t::level_set_t const& taken = tree.levels();
    expect_equal(taken.size(), std::size_t {2});
    expect_true(taken.level(0).empty());
    expect_equal(taken.level(1).size(), std::size_t {1});
    if (taken.level(1).size() == 1) {
        expect_true(&taken.level(1).run(0) == flushed);
        expect_true(taken.level(1).run(0).content_id() == content_id);
    }
    expect_equal(runs.size(), std::size_t {1});
    check_model(tree, *guard, std::span<expectation_t const> {model});
}

/** A run handle kept past its record's vacate resolves to nothing, even once another run
 *  takes the record. */
template <typename mount_type_>
void test_stale_run_handle_resolves_to_nothing(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using run_pool_t = typename types_t::run_pool_t;
    using sorted_run_t = typename types_t::sorted_run_t;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;
    auto [runs, runs_status] = run_pool_t::make(1, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;

    auto [first, take_status] = runs.take(sorted_run_t {});
    expect_true(take_status.succeeded());
    expect_true(runs.at(first) != nullptr);
    // The one record is taken, so another take is refused rather than handed a live run.
    expect_status(runs.take(sorted_run_t {}), util::status_code_t::capacity_exhausted_k);
    runs.vacate(first);
    expect_true(runs.at(first) == nullptr);
    expect_equal(runs.size(), std::size_t {0});

    auto [second, retake_status] = runs.take(sorted_run_t {});
    expect_true(retake_status.succeeded());
    expect_equal(second.index, first.index);
    expect_true(runs.at(second) != nullptr);
    expect_true(runs.at(first) == nullptr);
    runs.vacate(second);
    // Vacating a stale handle again changes nothing.
    runs.vacate(first);
    expect_equal(runs.size(), std::size_t {0});
}

/** Many small flushes, each followed by the compactions a commit runs, answer every key as a
 *  reference store does, while no step writes more than F + 3 slots and a step writing several runs
 *  fills each halfway. */
template <typename mount_type_>
void test_compaction_history_matches_reference(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    using sorted_run_t = typename types_t::sorted_run_t;
    using reference_t = st::reference_store<st::mapping<std::uint64_t, expectation_t>>;
    constexpr std::uint64_t slot_bytes = 256ull * 1024ull;
    constexpr std::size_t keys_k = 12000;
    constexpr std::size_t flushes_k = 200;
    constexpr std::size_t flushed_keys_k = 160;
    constexpr std::uint64_t value_bytes_max_k = 700;
    constexpr std::size_t steps_per_commit_max_k = 64;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    auto [reading, acquire_reading_status] = fixture->channel(1);
    auto [writing, acquire_writing_status] = fixture->channel(2);
    expect_true(acquire_status.succeeded() && acquire_reading_status.succeeded() && acquire_writing_status.succeeded());
    if (acquire_status.failed() || acquire_reading_status.failed() || acquire_writing_status.failed()) return;
    auto [volume, mount_volume_status] = fixture->mount_volume(*guard);
    expect_true(mount_volume_status.succeeded());
    if (mount_volume_status.failed()) return;
    auto [runs, runs_status] = run_pool_t::make(run_records_k, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [tree, tree_status] = tree_t::mount(volume, test_tree_config(), runs, slot_bytes);
    expect_true(tree_status.succeeded());
    if (tree_status.failed()) return;
    auto [reference, make_reference_status] = reference_t::make();
    expect_true(st::succeeded(make_reference_status));
    if (st::failed(make_reference_status)) return;

    // A step reads at most a slot of level zero or one run, and what it overlaps below, F + 2
    // slots at most.
    std::uint64_t const headroom_bytes = (test_tree_config().level_enlarge_factor + 3) * slot_bytes;
    // The merger cuts at the slot less its owner block, leaving the trailer a stamp and a kind per
    // key slot and a page, and lands within a row of cells, a value and the alignment of four
    // sections of its aim.
    std::uint64_t const run_bytes_limit = (slot_bytes - 2 * util::portable_alignment_k) /
                                          (sizeof(variable_slot_t) + sizeof(std::uint64_t) + 1) *
                                          sizeof(variable_slot_t);
    std::uint64_t const cut_slack_bytes = 4 * util::portable_alignment_k + 16 * sizeof(variable_slot_t) +
                                          value_bytes_max_k;

    split_mix_generator_t random {case_stream_key()};
    std::size_t steps = 0;
    std::size_t multi_run_steps = 0;
    std::size_t overspent = 0;
    std::size_t mislengthed = 0;
    std::size_t underfilled = 0;
    st::vector<util::uuid128_t> before;
    for (std::size_t flush = 0; flush != flushes_k; ++flush) {
        st::vector<expectation_t> batch;
        for (std::uint64_t key = 0; key != keys_k; ++key) {
            if (random.below(keys_k) >= flushed_keys_k) continue;
            expectation_t entry;
            entry.stored_key = key;
            entry.state = random.below(8) == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
            entry.bytes = entry.state == lookup_result_t::written_k ? 1 + random.below(value_bytes_max_k) : 0;
            entry.seed = random();
            if (!expect_true(batch.push_back(expectation_t {entry}) == st::status_t::success_k) ||
                !expect_true(reference.upsert(st::mapping<std::uint64_t, expectation_t> {key, entry}) ==
                             st::status_t::success_k))
                return;
        }
        if (batch.empty()) continue;
        if (!flush_batch(tree, *guard, std::span<expectation_t const> {batch})) break;

        for (std::size_t step = 0; step != steps_per_commit_max_k && tree.needs_compaction(tree.levels()); ++step) {
            before.clear();
            typename tree_t::level_set_t const& source = tree.levels();
            for (std::size_t number = 0; number != source.size(); ++number)
                for (std::size_t index = 0; index != source.level(number).size(); ++index)
                    expect_true(before.push_back(source.level(number).run(index).content_id()) ==
                                st::status_t::success_k);
            std::array<typename tree_t::merge_buffers_t, 1> buffers {};
            auto const [outcome, outcome_status] = tree.compact(
                *reading, *writing, std::span<typename tree_t::merge_buffers_t> {buffers},
                compaction_placement_t::host_k, {}, nullptr, util::last_write_fold_t {}, no_device_fold_t {});
            expect_true(outcome_status.succeeded() && outcome == compaction_outcome_t::published_k);
            if (outcome_status.failed()) break;
            ++steps;

            st::vector<sorted_run_t const*> written;
            typename tree_t::level_set_t const& merged = tree.levels();
            for (std::size_t number = 0; number != merged.size(); ++number)
                for (std::size_t index = 0; index != merged.level(number).size(); ++index) {
                    sorted_run_t const& run = merged.level(number).run(index);
                    bool known = false;
                    for (util::uuid128_t const& id : before) known |= id == run.content_id();
                    if (!known) expect_true(written.push_back(&run) == st::status_t::success_k);
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
        if (!expect_true(!tree.needs_compaction(tree.levels()))) break;
        if (flush % 25 == 24) check_reference(tree, *guard, reference, keys_k);
    }
    check_reference(tree, *guard, reference, keys_k);
    expect_true(tree.levels().size() >= 3);
    expect_true(steps != 0 && multi_run_steps != 0);
    expect_equal(overspent, std::size_t {0});
    expect_equal(mislengthed, std::size_t {0});
    expect_equal(underfilled, std::size_t {0});
    // Every record the pool holds is a run the current set names, so every consumed run
    // was given back.
    std::size_t named = 0;
    for (std::size_t number = 0; number != tree.levels().size(); ++number) named += tree.levels().level(number).size();
    expect_equal(runs.size(), named);
}

#pragma endregion Compaction Cases

#pragma region Compactor Choice Cases

/** The derived break-even picks the device compactor only past it, and only where a device is
 *  present: it never falls as the host merges on more slices, and stays finite on one thread. */
void test_compactor_choice_follows_the_break_even() {
    constexpr std::size_t slot_bytes_k = sizeof(st::key128_t);
    constexpr std::uint64_t one_slice_k = device_compaction_min_key_slots(slot_bytes_k, 4, 1);
    static_assert(one_slice_k != (std::numeric_limits<std::uint64_t>::max)());
    for (std::size_t runs : {2u, 4u, 16u}) {
        std::uint64_t fewer = device_compaction_min_key_slots(slot_bytes_k, runs, 1);
        for (std::size_t slices = 2; slices <= 64; slices *= 2) {
            std::uint64_t const more = device_compaction_min_key_slots(slot_bytes_k, runs, slices);
            expect_true(more >= fewer, "{} runs on {} slices", runs, slices);
            fewer = more;
        }
    }

    cuda_compactor_t* const absent = nullptr;
    compactor_ref_t const device {absent}, host {host_compactor_t {}};
    auto const automatic = compaction_placement_t::automatic_k;
    expect_true(choose_compactor(automatic, device, one_slice_k, one_slice_k) == compactor_choice_t::host_k);
    expect_true(choose_compactor(automatic, device, one_slice_k + 1, one_slice_k) == compactor_choice_t::device_k);
    expect_true(choose_compactor(automatic, host, one_slice_k + 1, one_slice_k) == compactor_choice_t::host_k);

    // A fixed placement answers itself, whatever the database holds and however large the merge is.
    expect_true(choose_compactor(compaction_placement_t::host_k, device, one_slice_k * 8, one_slice_k) ==
                compactor_choice_t::host_k);
    expect_true(choose_compactor(compaction_placement_t::device_k, host, 1, one_slice_k) ==
                compactor_choice_t::device_k);
}

/** The compactor of NVIDIA device zero, made once as an engine makes its own at open, or none where
 *  the build or the machine carries no device. */
struct device_zero_compactor_t {
    util::gpu_drivers_t drivers;
    kernel_tables<util::cuda_module_t> kernels;
    util::cuda_executor_t executor;
    cuda_compactor_t compactor;
    util::status_t make_status {util::status_code_t::device_k, "No CUDA device zero"};

    device_zero_compactor_t() noexcept {
        util::gpu_device_t device_zero;
        gpu_class_t const gpu_class = util::list_nvidia_gpu_devices(drivers.cuda, {&device_zero, 1})
                                          ? pick_gpu_class(drivers, device_zero, gpu_class_t::unknown_k)
                                          : gpu_class_t::unknown_k;
        if (!drivers.retain_context(0) ||
            !kernels.load(drivers.cuda, drivers.cuda_contexts[0], 0, gpu_class, util::vram_kernel_names_k).succeeded())
            return;
        auto [spawned, spawn_status] = util::cuda_executor_t::spawn(drivers.cuda, drivers.cuda_contexts[0], device_zero,
                                                                    0, ustore_gds_automatic_k,
                                                                    kernels.table_of(0, gpu_class));
        if (spawn_status.failed()) {
            make_status = spawn_status;
            return;
        }
        executor = std::move(spawned);
        auto [made, made_status] = cuda_compactor_t::make(executor);
        compactor = std::move(made);
        make_status = made_status;
    }
};

/** A merge on the host and the same merge on a device answer the same runs, key slot for key slot. */
template <typename mount_type_>
case_result_t test_both_compactors_agree(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using level_t = typename types_t::level_t;
    using run_pool_t = typename types_t::run_pool_t;
    using run_handle_t = typename types_t::run_handle_t;
    using merger_t = typename types_t::merger_t;
    using sorted_run_t = typename types_t::sorted_run_t;
    constexpr std::size_t keys_k = 512;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return {};

    auto [guard, acquire_status] = fixture->channel(0);
    auto [reading, acquire_reading_status] = fixture->channel(1);
    auto [writing, acquire_writing_status] = fixture->channel(2);
    expect_true(acquire_status.succeeded() && acquire_reading_status.succeeded() && acquire_writing_status.succeeded());
    if (acquire_status.failed() || acquire_reading_status.failed() || acquire_writing_status.failed()) return {};
    auto [volume, mount_volume_status] = fixture->mount_volume(*guard);
    expect_true(mount_volume_status.succeeded());
    if (mount_volume_status.failed()) return {};

    st::vector<expectation_t> older;
    st::vector<expectation_t> newer;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index);
        entry.state = index % 11 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
        entry.bytes = entry.state == lookup_result_t::written_k ? 1 + index % 613 : 0;
        entry.seed = index + 5;
        if (!expect_true(older.push_back(expectation_t {entry}) == st::status_t::success_k)) return {};
        if (index % 2 != 0) continue;
        entry.bytes = entry.state == lookup_result_t::written_k ? 1 + index % 271 : 0;
        entry.seed = index + 4096;
        if (!expect_true(newer.push_back(expectation_t {entry}) == st::status_t::success_k)) return {};
    }

    auto [runs, runs_status] = run_pool_t::make(run_records_k, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return {};
    level_t upper = level_t::of(runs, 0, lsm_level_order_t::overlapping_k);
    if (!adopt_batch(upper, runs, volume, *guard, std::span<expectation_t const> {older}) ||
        !adopt_batch(upper, runs, volume, *guard, std::span<expectation_t const> {newer}))
        return {};
    std::span<run_handle_t const> const sources = upper.runs();

    std::array<typename merger_t::merge_buffers_t, 1> buffers;
    typename merger_t::merge_request_t request;
    request.upper = sources;
    request.upper_order = lsm_level_order_t::overlapping_k;
    request.depth = lsm_level_depth_t::intermediate_k;
    request.value_max_size = value_max_size_k;
    request.buffers = buffers;

    request.placement = compaction_placement_t::host_k;
    auto [host_runs, host_runs_status] = merger_t::merge(volume, runs, *reading, *writing, request,
                                                         util::last_write_fold_t {}, no_device_fold_t {});
    expect_true(host_runs_status.succeeded());
    if (host_runs_status.failed()) return {};
    level_t const host = level_of<level_t>(runs, 1, host_runs);

    // The device side runs on a compactor made once, and a build without one refuses.
    request.placement = compaction_placement_t::device_k;
    device_zero_compactor_t device_zero;
    if (device_zero.make_status.succeeded()) request.compactor = std::addressof(device_zero.compactor);
    auto [device_runs, device_runs_status] = merger_t::merge(volume, runs, *reading, *writing, request,
                                                             util::last_write_fold_t {}, no_device_fold_t {});
    if (device_runs_status.failed()) {
        bool const no_compactor = device_runs_status == util::status_code_t::unsupported_k ||
                                  device_runs_status == util::status_code_t::device_k;
        if (!no_compactor) expect_status(device_runs_status, util::status_code_t::unsupported_k);
        runs.discard(volume, host_runs);
        runs.discard(volume, upper.runs());
        if (no_compactor) return case_result_t::skipped("no device compactor");
        return {};
    }
    level_t const device = level_of<level_t>(runs, 1, device_runs);

    expect_equal(device.size(), host.size());
    expect_equal(device.population_count(), host.population_count());
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
        // Equal populations hold equally many value offsets, which must match one for one.
        differing += std::memcmp(left.value_offsets().data(), right.value_offsets().data(),
                                 left.value_offsets().size_bytes()) != 0;
    }
    expect_equal(differing, std::size_t {0});

    // Identical cells are not identical runs until the bytes they locate come back the same too.
    st::vector<util::byte_t> host_values;
    st::vector<util::byte_t> device_values;
    collect_level_values(host, *guard, host_values);
    collect_level_values(device, *guard, device_values);
    expect_equal(device_values.size(), host_values.size());
    if (device_values.size() == host_values.size())
        expect_equal(std::memcmp(host_values.data(), device_values.data(), host_values.size()), 0);
    runs.discard(volume, host_runs);
    runs.discard(volume, device_runs);
    runs.discard(volume, upper.runs());
    return {};
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
void test_merge_charges_no_index_copy(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using level_t = typename types_t::level_t;
    using run_pool_t = typename types_t::run_pool_t;
    using run_handle_t = typename types_t::run_handle_t;
    using merger_t = typename types_t::merger_t;
    constexpr std::size_t older_keys_k = 1000;
    constexpr std::size_t newer_keys_k = 300;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    auto [reading, acquire_reading_status] = fixture->channel(1);
    auto [writing, acquire_writing_status] = fixture->channel(2);
    expect_true(acquire_status.succeeded() && acquire_reading_status.succeeded() && acquire_writing_status.succeeded());
    if (acquire_status.failed() || acquire_reading_status.failed() || acquire_writing_status.failed()) return;
    auto [volume, mount_volume_status] = fixture->mount_volume(*guard);
    expect_true(mount_volume_status.succeeded());
    if (mount_volume_status.failed()) return;

    // Half of the newer keys shadow older ones and half sit past them, so the survivors number 1150.
    st::vector<expectation_t> older;
    st::vector<expectation_t> newer;
    for (std::size_t index = 0; index != older_keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index);
        entry.state = index % 11 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
        entry.bytes = entry.state == lookup_result_t::written_k ? 1 + index % 613 : 0;
        entry.seed = index + 5;
        if (!expect_true(older.push_back(expectation_t {entry}) == st::status_t::success_k)) return;
    }
    for (std::size_t index = 0; index != newer_keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index < newer_keys_k / 2 ? index * 2 : older_keys_k + index);
        entry.state = lookup_result_t::written_k;
        entry.bytes = 1 + index % 271;
        entry.seed = index + 4096;
        if (!expect_true(newer.push_back(expectation_t {entry}) == st::status_t::success_k)) return;
    }

    auto [runs, runs_status] = run_pool_t::make(run_records_k, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    level_t upper = level_t::of(runs, 0, lsm_level_order_t::overlapping_k);
    if (!adopt_batch(upper, runs, volume, *guard, std::span<expectation_t const> {older}) ||
        !adopt_batch(upper, runs, volume, *guard, std::span<expectation_t const> {newer}))
        return;
    std::span<run_handle_t const> const sources = upper.runs();

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
    auto [merged_runs, merged_runs_status] = merger_t::merge(volume, runs, *reading, *writing, request,
                                                             util::last_write_fold_t {}, no_device_fold_t {});
    allocation_watch_t const seen = watch;
    st::vector<util::pooled_buffer_t> taken_streams;
    for (std::uint32_t index = 0; index != lsm_config_k.stream_buffers; ++index) {
        auto [held, held_status] = reading->streams().acquire();
        expect_status(held_status, util::status_code_t::success_k);
        expect_true(taken_streams.push_back(std::move(held)) == st::status_t::success_k);
    }
    watch = indexes_watch;
    watch.sizes[watch.count++] = all_indexes;
    auto [merged_again, merged_again_status] = merger_t::merge(volume, runs, *reading, *writing, request,
                                                               util::last_write_fold_t {}, no_device_fold_t {});
    allocation_watch_t const seen_again = watch;
    watch = allocation_watch_t {};
    taken_streams.clear();
    expect_status(merged_runs_status, util::status_code_t::success_k);
    expect_status(merged_again_status, util::status_code_t::success_k);
    if (merged_runs_status.failed() || merged_again_status.failed()) return;

    level_t const merged = level_of<level_t>(runs, 1, merged_runs);
    level_t const merged_warm = level_of<level_t>(runs, 1, merged_again);
    expect_equal(merged.population_count(), std::uint64_t {older_keys_k + newer_keys_k / 2});
    expect_equal(merged_warm.population_count(), merged.population_count());
    st::vector<util::byte_t> values;
    st::vector<util::byte_t> values_again;
    collect_level_values(merged, *guard, values);
    collect_level_values(merged_warm, *guard, values_again);
    expect_equal(values_again.size(), values.size());
    if (values_again.size() == values.size())
        expect_equal(std::memcmp(values.data(), values_again.data(), values.size()), 0);
    for (std::size_t index = 0; index != seen.count; ++index) expect_equal(seen.hits[index], std::size_t {0});
    for (std::size_t index = 0; index != seen_again.count; ++index)
        expect_equal(seen_again.hits[index], std::size_t {0});
    runs.discard(volume, merged_runs);
    runs.discard(volume, merged_again);
    runs.discard(volume, upper.runs());
}

/** A merge reading and writing on the two channels of one thread, which share one request and one
 *  stream pool, completes five sources on one block per pool and refuses at open without one. */
template <typename mount_type_>
void test_merge_runs_on_one_block_per_pool(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using level_t = typename types_t::level_t;
    using run_pool_t = typename types_t::run_pool_t;
    using merger_t = typename types_t::merger_t;
    constexpr std::size_t sources_k = 5;
    constexpr std::size_t keys_per_source_k = 4000;
    fixture_config_t minimum = mount.config;
    minimum.request_buffer_bytes = util::blocking_queues_config_k.request_buffer_bytes;
    minimum.request_buffers = 1;
    minimum.stream_buffer_bytes = util::blocking_queues_config_k.stream_buffer_bytes;
    minimum.stream_buffers = 1;
    minimum.queue_depth = static_cast<std::uint32_t>(util::blocking_queues_config_k.depth);
    auto [fixture, mount_status] = mount(minimum);
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    // The merge reads and writes on thread zero's two channels, as an engine's merge slice does
    auto [reading, acquire_reading_status] = fixture->channel(0);
    auto [writing, acquire_writing_status] = fixture->channel(1);
    auto [guard, acquire_status] = fixture->channel(2);
    expect_true(acquire_status.succeeded() && acquire_reading_status.succeeded() && acquire_writing_status.succeeded());
    if (acquire_status.failed() || acquire_reading_status.failed() || acquire_writing_status.failed()) return;
    auto [volume, mount_volume_status] = fixture->mount_volume(*guard);
    expect_true(mount_volume_status.succeeded());
    if (mount_volume_status.failed()) return;

    auto [runs, runs_status] = run_pool_t::make(run_records_k, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    // Every source interleaves its keys with the others', so the merge overlaps them all
    level_t upper = level_t::of(runs, 0, lsm_level_order_t::overlapping_k);
    for (std::size_t source = 0; source != sources_k; ++source) {
        st::vector<expectation_t> batch;
        for (std::size_t index = 0; index != keys_per_source_k; ++index) {
            expectation_t entry;
            entry.stored_key = static_cast<std::uint64_t>(index * sources_k + source);
            entry.state = lookup_result_t::written_k;
            entry.bytes = 1 + (index + source) % 61;
            entry.seed = index + source * 7919;
            if (!expect_true(batch.push_back(expectation_t {entry}) == st::status_t::success_k)) return;
        }
        if (!adopt_batch(upper, runs, volume, *guard, std::span<expectation_t const> {batch})) return;
    }

    std::array<typename merger_t::merge_buffers_t, 1> buffers;
    typename merger_t::merge_request_t request;
    request.upper = upper.runs();
    request.upper_order = lsm_level_order_t::overlapping_k;
    request.depth = lsm_level_depth_t::intermediate_k;
    request.value_max_size = value_max_size_k;
    request.placement = compaction_placement_t::host_k;
    request.buffers = buffers;
    {
        auto [held, held_status] = writing->streams().acquire();
        expect_true(held_status.succeeded());
        auto [refused_runs, refused_status] = merger_t::merge(volume, runs, *reading, *writing, request,
                                                              util::last_write_fold_t {}, no_device_fold_t {});
        expect_status(refused_status, util::status_code_t::capacity_exhausted_k);
    }
    auto [merged_runs, merged_runs_status] = merger_t::merge(volume, runs, *reading, *writing, request,
                                                             util::last_write_fold_t {}, no_device_fold_t {});
    if (!expect_status(merged_runs_status, util::status_code_t::success_k)) return;
    level_t const merged = level_of<level_t>(runs, 1, merged_runs);
    expect_equal(merged.population_count(), std::uint64_t {sources_k * keys_per_source_k});
    runs.discard(volume, merged_runs);
    runs.discard(volume, upper.runs());
}

#pragma endregion Allocation Cases

#pragma region Depth Cases

/**
 *  @brief A batched read of many keys across several runs sits at the queue pair's depth through
 *      the middle, and merges the values that lie back to back into reads of whole blocks.
 *
 *  The keys are spread over four level-zero runs, so what is held to the depth is a read that crosses
 *  runs rather than one run's own values read back to back.
 */
template <typename mount_type_>
void test_batched_read_holds_its_depth(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using tree_t = typename types_t::tree_t;
    using run_pool_t = typename types_t::run_pool_t;
    constexpr std::size_t keys_k = 1024;
    constexpr std::size_t flushes_k = 4;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    auto [volume, mount_volume_status] = fixture->mount_volume(*guard);
    expect_true(mount_volume_status.succeeded());
    if (mount_volume_status.failed()) return;
    auto [runs, runs_status] = run_pool_t::make(run_records_k, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;
    auto [tree, tree_status] = tree_t::mount(volume, test_tree_config(), runs, slot_bytes_k);
    expect_true(tree_status.succeeded());
    if (tree_status.failed()) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(keys_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<std::uint64_t>(index);
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
        if (!flush_batch(tree, *guard, std::span<expectation_t const> {batch})) return;
    }

    st::vector<stored_key_t> keys;
    if (!expect_true(keys.resize(keys_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != keys_k; ++index) keys[index] = key_of(model[index].stored_key);

    // The blocks a batched read holds are counted in the pool, since the ring performs a channel's
    // requests inside the poll that hands them to the sink.
    std::uint32_t idlest = lsm_config_k.request_buffers;
    std::size_t mismatches = 0;
    auto counting = [&](std::size_t request, std::uint64_t offset, std::span<util::byte_t const> bytes) noexcept {
        std::uint32_t const free_blocks = guard->requests().available_buffers();
        if (free_blocks < idlest) idlest = free_blocks;
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(model[request].seed, offset + index)) ++mismatches;
    };

    typename tree_t::level_set_t const& taken = tree.levels();
    expect_equal(taken.level(0).size(), flushes_k);
    st::vector<lsm_tree_position_t> positions;
    if (!expect_true(positions.resize(keys_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != keys_k; ++index)
        expect_true(tree.find(taken, keys[index], positions[index]) == lookup_result_t::written_k);
    ustore_arena_handle_t scratch {};
    auto const [reads, read_status] = tree.read_located(*guard, taken, std::span<lsm_tree_position_t const> {positions},
                                                        &scratch, counting);
    util::arena_free_blocks(scratch);
    if (!expect_true(read_status.succeeded())) return;
    expect_equal(mismatches, std::size_t {0});
    expect_equal(lsm_config_k.request_buffers - idlest, lsm_config_k.queue_depth);
    // Back-to-back values share blocks of 64 KiB, fewer than one read per eight values
    expect_true(reads * 8 < keys_k, "{} reads for {} values", reads, keys_k);
}

#pragma endregion Depth Cases

#pragma region Strings Merges

/** Run types over strings, whose keys the heads hold first and a tape holds whole. */
template <typename fixture_type_>
struct strings_types {
    using volume_t = typename fixture_type_::volume_t;
    using key_slot_t = key_slot<st::key128_t, value_layout_t::uuid_k, key_tails_t::present_k>;
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

/** One version a source run holds for the URL at @c index. */
struct url_version_t {
    std::size_t index = 0;
    std::uint64_t stamp = 0;
    util::delta_kind_t kind = util::delta_kind_t::write_k;
};

/** Writes @p versions, ascending by URL, as one run of strings taken into @p runs, its values
 *  painted by stamp and @p tombstones beside them, answering whether @p into names it. */
template <typename types_type_>
bool write_url_run(typename types_type_::run_pool_t& runs, typename types_type_::volume_t& volume,
                   util::io_channel_t& channel, std::span<url_version_t const> versions,
                   st::vector<typename types_type_::run_handle_t>& into,
                   typename types_type_::sorted_run_t::range_tombstones_t const& tombstones = {}) {
    using key_slot_t = typename types_type_::key_slot_t;
    using sorted_run_t = typename types_type_::sorted_run_t;
    std::size_t const count = versions.size();
    st::vector<key_slot_t> key_slots;
    st::vector<std::uint64_t> stamps;
    st::vector<std::uint8_t> kinds;
    st::vector<std::uint32_t> tail_offsets;
    st::vector<util::byte_t> tails;
    if (!expect_true(key_slots.resize(count) == st::status_t::success_k) ||
        !expect_true(stamps.resize(count) == st::status_t::success_k) ||
        !expect_true(kinds.resize(count) == st::status_t::success_k) ||
        !expect_true(tail_offsets.resize(count + 1) == st::status_t::success_k))
        return false;
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
        if (!expect_true(tails.resize(tails_bytes + tail.size()) == st::status_t::success_k)) return false;
        std::memcpy(tails.data() + tails_bytes, tail.data(), tail.size());
        tails_bytes += static_cast<std::uint32_t>(tail.size());
    }
    tail_offsets[count] = tails_bytes;
    auto [run, run_status] = sorted_run_t::write(
        volume, channel, util::uuid128_t {}, util::uuid_generator(), std::span<key_slot_t const> {key_slots},
        std::span<std::uint64_t const> {stamps}, std::span<std::uint8_t const> {kinds}, 0, {}, {}, key_slot_t::size,
        key_tails_column_t {std::span<std::uint32_t const> {tail_offsets}, std::span<util::byte_t const> {tails}},
        tombstones);
    if (!expect_true(run_status.succeeded())) return false;
    auto [taken, take_status] = runs.take(std::move(run));
    if (!expect_true(take_status.succeeded())) return false;
    return expect_true(into.push_back(std::move(taken)) == st::status_t::success_k);
}

/** Compares the runs @p merged, in order, with @p wanted, ascending by URL and newest first within
 *  one, and checks every run stays under @p run_bytes_limit and orders wholly before the next. */
template <typename types_type_>
void check_url_runs(typename types_type_::run_pool_t const& runs,
                    std::span<typename types_type_::run_handle_t const> merged, std::span<url_version_t const> wanted,
                    std::uint64_t run_bytes_limit) {
    using sorted_run_t = typename types_type_::sorted_run_t;
    std::size_t at = 0;
    std::size_t mismatches = 0;
    sorted_run_t const* previous = nullptr;
    for (auto const& handle : merged) {
        sorted_run_t const& run = *runs.at(handle);
        expect_true(run.size_bytes() <= run_bytes_limit);
        expect_equal(run.size_bytes(), sorted_run_t::strings_layout_t::blob_bytes(run.header()));
        if (previous != nullptr) expect_true(previous->precedes(run));
        previous = &run;
        for (std::uint64_t position = 0; position != run.population_count(); ++position, ++at) {
            if (!expect_true(at != wanted.size())) return;
            char buffer[64];
            std::string_view const text = url_of(wanted[at].index, buffer);
            std::span<util::byte_t const> const value = run.value(position);
            mismatches += run.key(position) != text || run.stamp(position) != wanted[at].stamp ||
                          run.kind(position) != wanted[at].kind || value[3] != painted(wanted[at].stamp, 3);
        }
    }
    expect_equal(at, wanted.size());
    expect_equal(mismatches, std::size_t {0});
}

/** Ranks every version of @p sources by @c strings_merge_verdict, compiled for the host, as the
 *  device kernel ranks them under the sources' range tombstones, and compares the survivors in
 *  merged order with @p wanted, which the host merge wrote. */
template <typename sorted_run_type_>
void check_device_ranking(std::span<sorted_run_type_ const* const> sources, std::uint32_t upper_runs,
                          compaction_kind_t kind, compaction_stamps_t const& stamps,
                          std::span<url_version_t const> wanted) {
    std::size_t records = 0;
    for (sorted_run_type_ const* source : sources) records += source->population_count();
    st::vector<st::key128_t> heads;
    st::vector<st::vector<std::uint64_t>> saved;
    st::vector<st::vector<std::uint32_t>> range_offsets;
    st::vector<st::vector<util::byte_t>> range_tapes;
    st::vector<strings_merge_run> runs;
    expect_true(heads.resize(records) == st::status_t::success_k);
    expect_true(saved.resize(2 * sources.size()) == st::status_t::success_k);
    expect_true(range_offsets.resize(sources.size()) == st::status_t::success_k);
    expect_true(range_tapes.resize(sources.size()) == st::status_t::success_k);
    if (!expect_true(runs.resize(sources.size()) == st::status_t::success_k)) return;
    std::size_t first = 0;
    for (std::size_t index = 0; index != sources.size(); ++index) {
        sorted_run_type_ const& source = *sources[index];
        std::size_t const count = source.population_count();
        for (std::size_t position = 0; position != count; ++position) heads[first + position] = source.head(position);
        st::vector<std::uint64_t>& stamps_words = saved[2 * index];
        st::vector<std::uint64_t>& kinds_words = saved[2 * index + 1];
        expect_true(stamps_words.resize(source.stamps().saved_bytes() / sizeof(std::uint64_t)) ==
                    st::status_t::success_k);
        if (!expect_true(kinds_words.resize(source.kinds().saved_bytes() / sizeof(std::uint64_t)) ==
                         st::status_t::success_k))
            return;
        expect_true(source.stamps().save(std::as_writable_bytes(std::span<std::uint64_t> {stamps_words})) ==
                    st::status_t::success_k);
        expect_true(source.kinds().save(std::as_writable_bytes(std::span<std::uint64_t> {kinds_words})) ==
                    st::status_t::success_k);
        runs[index] = strings_merge_run {heads.data() + first,
                                         source.key_offsets().data(),
                                         source.key_tape().data(),
                                         stamps_words.data(),
                                         kinds_words.data(),
                                         first,
                                         count};
        first += count;

        // The bounds of the run's range tombstones leave as a tape, as the compactor uploads them
        typename sorted_run_type_::range_tombstones_t const& tombstones = source.range_tombstones();
        st::vector<std::uint32_t>& offsets = range_offsets[index];
        st::vector<util::byte_t>& tape = range_tapes[index];
        for (std::size_t range = 0; range != tombstones.size(); ++range)
            for (std::string_view const bound : {tombstones.range(range).start, tombstones.range(range).end}) {
                expect_true(offsets.push_back(static_cast<std::uint32_t>(tape.size())) == st::status_t::success_k);
                for (char const byte : bound)
                    expect_true(tape.push_back(static_cast<util::byte_t>(byte)) == st::status_t::success_k);
            }
        if (!expect_true(offsets.push_back(static_cast<std::uint32_t>(tape.size())) == st::status_t::success_k)) return;
        runs[index].range_stamps = tombstones.stamps().data(), runs[index].range_offsets = offsets.data();
        runs[index].range_tape = tape.data(), runs[index].ranges_count = tombstones.size();
    }

    st::vector<strings_version_t> merged;
    st::vector<std::uint8_t> flags;
    expect_true(merged.resize(records) == st::status_t::success_k);
    if (!expect_true(flags.resize(records, std::uint8_t {2}) == st::status_t::success_k)) return;
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
    expect_equal(collisions, std::size_t {0});

    std::size_t at = 0;
    std::size_t mismatches = 0;
    for (std::size_t rank = 0; rank != records; ++rank) {
        if (flags[rank] != 1) continue;
        if (!expect_true(at != wanted.size())) return;
        sorted_run_type_ const& source = *sources[merged[rank].source];
        char buffer[64];
        mismatches += source.key(merged[rank].position) != url_of(wanted[at].index, buffer) ||
                      source.stamp(merged[rank].position) != wanted[at].stamp ||
                      source.kind(merged[rank].position) != wanted[at].kind;
        ++at;
    }
    expect_equal(at, wanted.size());
    expect_equal(mismatches, std::size_t {0});
}

/**
 *  @brief Strings merges over URLs with long shared prefixes keep the versions a reference keeps,
 *      in one merge or two, cut into runs under their limit that order wholly one after another.
 *
 *  Three overlapping upper runs, the newest removing some URLs, fold into two lower runs.
 *  With a reader pinned between the lower and the upper stamps, a URL both hold keeps two
 *  versions, newest first.
 */
template <typename mount_type_>
void test_strings_merges_match_reference(mount_type_ const& mount) {
    using types_t = strings_types<typename mount_type_::fixture_t>;
    using merger_t = typename types_t::merger_t;
    using run_pool_t = typename types_t::run_pool_t;
    using run_handle_t = typename types_t::run_handle_t;
    using sorted_run_t = typename types_t::sorted_run_t;
    constexpr std::size_t upper_runs_k = 3;
    constexpr std::uint64_t lower_stamps_k = 100000;
    constexpr std::uint64_t run_bytes_limit_k = 64ull * 1024ull;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;
    auto [guard, acquire_status] = fixture->channel(0);
    auto [reading, acquire_reading_status] = fixture->channel(1);
    auto [writing, acquire_writing_status] = fixture->channel(2);
    expect_true(acquire_status.succeeded() && acquire_reading_status.succeeded() && acquire_writing_status.succeeded());
    if (acquire_status.failed() || acquire_reading_status.failed() || acquire_writing_status.failed()) return;
    auto [volume, mount_volume_status] = fixture->mount_volume(*guard);
    expect_true(mount_volume_status.succeeded());
    if (mount_volume_status.failed()) return;
    auto [runs, runs_status] = run_pool_t::make(run_records_k, fixture->resources->memory_constraint);
    expect_true(runs_status.succeeded());
    if (runs_status.failed()) return;

    // Lower runs hold the even URLs in two halves; upper run u holds every (u + 3)-th URL, stamped
    // newer per run.
    auto upper_stamp = [](std::size_t run, std::size_t index) noexcept { return lower_stamps_k * (run + 2) + index; };
    st::vector<run_handle_t> lower;
    st::vector<run_handle_t> upper;
    for (std::size_t half = 0; half != 2; ++half) {
        st::vector<url_version_t> versions;
        for (std::size_t index = half * urls_k / 2; index != (half + 1) * urls_k / 2; index += 2)
            if (!expect_true(versions.push_back(url_version_t {index, 1 + index}) == st::status_t::success_k)) return;
        if (!write_url_run<types_t>(runs, volume, *guard, versions, lower)) return;
    }
    for (std::size_t run = 0; run != upper_runs_k; ++run) {
        st::vector<url_version_t> versions;
        for (std::size_t index = 0; index < urls_k; index += run + 3) {
            util::delta_kind_t const kind = run + 1 == upper_runs_k && index % 7 == 0 ? util::delta_kind_t::remove_k
                                                                                      : util::delta_kind_t::write_k;
            if (!expect_true(versions.push_back(url_version_t {index, upper_stamp(run, index), kind}) ==
                             st::status_t::success_k))
                return;
        }
        if (!write_url_run<types_t>(runs, volume, *guard, versions, upper)) return;
    }

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
                                            removes ? util::delta_kind_t::remove_k : util::delta_kind_t::write_k};
                    upper_found = true;
                }
            bool const lower_found = index % 2 == 0;
            if (upper_found && !(bottom && newest.kind == util::delta_kind_t::remove_k)) [[maybe_unused]]
                st::status_t const kept = wanted.push_back(url_version_t {newest});
            if (lower_found && (!upper_found || pinned)) [[maybe_unused]]
                st::status_t const kept = wanted.push_back(url_version_t {index, 1 + index});
        }
    };
    st::vector<sorted_run_t const*> upper_sources;
    st::vector<sorted_run_t const*> lower_sources;
    for (run_handle_t const& handle : upper)
        expect_true(upper_sources.push_back(runs.at(handle)) == st::status_t::success_k);
    for (run_handle_t const& handle : lower)
        expect_true(lower_sources.push_back(runs.at(handle)) == st::status_t::success_k);
    st::vector<sorted_run_t const*> all_sources;
    for (sorted_run_t const* source : upper_sources)
        expect_true(all_sources.push_back(static_cast<sorted_run_t const*>(source)) == st::status_t::success_k);
    for (sorted_run_t const* source : lower_sources)
        expect_true(all_sources.push_back(static_cast<sorted_run_t const*>(source)) == st::status_t::success_k);

    typename merger_t::merge_request_t request;
    request.upper = upper;
    request.upper_order = lsm_level_order_t::overlapping_k;
    request.lower = lower;
    request.value_max_size = types_t::key_slot_t::size;
    request.run_bytes_limit = run_bytes_limit_k;
    request.placement = compaction_placement_t::host_k;

    // One merge at the bottom drops the removals and every shadowed lower version.
    request.depth = lsm_level_depth_t::bottom_k;
    auto [once, once_status] = merger_t::merge(volume, runs, *reading, *writing, request, util::last_write_fold_t {},
                                               no_device_fold_t {});
    expect_status(once_status, util::status_code_t::success_k);
    if (once_status.failed()) return;
    expect_true(once.size() > 1);
    st::vector<url_version_t> wanted;
    reference(request, wanted);
    check_url_runs<types_t>(runs, once, wanted, run_bytes_limit_k);
    check_device_ranking<sorted_run_t>(std::span<sorted_run_t const* const> {all_sources}, std::uint32_t {upper_runs_k},
                                       compaction_kind_t::overlapping_into_bottom_k, request.stamps, wanted);

    // The upper runs merged first and then into the lower ones end in the very same versions.
    request.lower = {};
    request.depth = lsm_level_depth_t::intermediate_k;
    auto [folded, folded_status] = merger_t::merge(volume, runs, *reading, *writing, request,
                                                   util::last_write_fold_t {}, no_device_fold_t {});
    expect_status(folded_status, util::status_code_t::success_k);
    if (folded_status.failed()) return;
    request.upper = folded;
    request.upper_order = lsm_level_order_t::sorted_k;
    request.lower = lower;
    request.depth = lsm_level_depth_t::bottom_k;
    auto [twice, twice_status] = merger_t::merge(volume, runs, *reading, *writing, request, util::last_write_fold_t {},
                                                 no_device_fold_t {});
    expect_status(twice_status, util::status_code_t::success_k);
    if (twice_status.failed()) return;
    check_url_runs<types_t>(runs, twice, wanted, run_bytes_limit_k);

    // A reader pinned between the lower and the upper stamps keeps both versions of a
    // URL, newest first.
    std::uint64_t const pins[] {lower_stamps_k};
    request.upper = upper;
    request.upper_order = lsm_level_order_t::overlapping_k;
    request.depth = lsm_level_depth_t::intermediate_k;
    request.stamps.pinned = pins;
    request.stamps.pinned_count = 1;
    auto [pinned, pinned_status] = merger_t::merge(volume, runs, *reading, *writing, request,
                                                   util::last_write_fold_t {}, no_device_fold_t {});
    expect_status(pinned_status, util::status_code_t::success_k);
    if (pinned_status.failed()) return;
    st::vector<url_version_t> wanted_pinned;
    reference(request, wanted_pinned);
    check_url_runs<types_t>(runs, pinned, wanted_pinned, run_bytes_limit_k);
    check_device_ranking<sorted_run_t>(std::span<sorted_run_t const* const> {all_sources}, std::uint32_t {upper_runs_k},
                                       compaction_kind_t::overlapping_into_intermediate_k, request.stamps,
                                       wanted_pinned);

    // Range tombstones alone hide every older version from the readers above them
    using range_tombstones_t = typename sorted_run_t::range_tombstones_t;
    constexpr std::size_t hidden_first_k = 1000, hidden_end_k = 1300, hidden_tail_k = 5000;
    char first_buffer[64], end_buffer[64], tail_buffer[64];
    typename range_tombstones_t::range_t const ranges[] {
        {url_of(hidden_first_k, first_buffer), url_of(hidden_end_k, end_buffer)},
        {url_of(hidden_tail_k, tail_buffer), {}},
    };
    std::uint64_t const range_stamps[] {upper_stamp(upper_runs_k, 0), upper_stamp(upper_runs_k, 1)};
    auto [tombstones, tombstones_status] = range_tombstones_t::make(ranges, range_stamps);
    if (!expect_true(tombstones_status.succeeded())) return;
    if (!write_url_run<types_t>(runs, volume, *guard, {}, upper, tombstones)) return;
    st::vector<sorted_run_t const*> hidden_sources;
    for (run_handle_t const& handle : upper)
        expect_true(hidden_sources.push_back(runs.at(handle)) == st::status_t::success_k);
    for (sorted_run_t const* source : lower_sources)
        expect_true(hidden_sources.push_back(static_cast<sorted_run_t const*>(source)) == st::status_t::success_k);
    device_zero_compactor_t device_zero;
    request.upper = upper;
    request.upper_order = lsm_level_order_t::overlapping_k;
    auto const check_hidden = [&](lsm_level_depth_t depth, compaction_kind_t kind, std::uint64_t oldest_hidden,
                                  std::span<url_version_t const> unhidden) {
        st::vector<url_version_t> wanted_hidden;
        for (url_version_t const& version : unhidden) {
            bool const covered = (version.index >= hidden_first_k && version.index < hidden_end_k) ||
                                 version.index >= hidden_tail_k;
            if (covered && version.stamp >= oldest_hidden) continue;
            expect_true(wanted_hidden.push_back(url_version_t {version}) == st::status_t::success_k);
        }
        expect_true(wanted_hidden.size() < unhidden.size());
        request.depth = depth;
        request.placement = compaction_placement_t::host_k;
        request.compactor = {};
        auto [on_host, on_host_status] = merger_t::merge(volume, runs, *reading, *writing, request,
                                                         util::last_write_fold_t {}, no_device_fold_t {});
        expect_status(on_host_status, util::status_code_t::success_k);
        if (on_host_status.failed()) return;
        check_url_runs<types_t>(runs, on_host, wanted_hidden, run_bytes_limit_k);
        check_device_ranking<sorted_run_t>(std::span<sorted_run_t const* const> {hidden_sources},
                                           std::uint32_t {upper_runs_k + 1}, kind, request.stamps, wanted_hidden);
        runs.discard(volume, on_host);
        if (device_zero.make_status.failed()) return;
        request.placement = compaction_placement_t::device_k;
        request.compactor = std::addressof(device_zero.compactor);
        auto [on_device, on_device_status] = merger_t::merge(volume, runs, *reading, *writing, request,
                                                             util::last_write_fold_t {}, no_device_fold_t {});
        expect_status(on_device_status, util::status_code_t::success_k);
        if (on_device_status.failed()) return;
        check_url_runs<types_t>(runs, on_device, wanted_hidden, run_bytes_limit_k);
        runs.discard(volume, on_device);
    };
    // A reader pinned below the upper stamps still reads the lower versions, hidden by no tombstone
    check_hidden(lsm_level_depth_t::intermediate_k, compaction_kind_t::overlapping_into_intermediate_k,
                 lower_stamps_k + 1, wanted_pinned);
    request.stamps = {};
    check_hidden(lsm_level_depth_t::bottom_k, compaction_kind_t::overlapping_into_bottom_k, 0, wanted);

    runs.discard(volume, once);
    runs.discard(volume, folded);
    runs.discard(volume, twice);
    runs.discard(volume, pinned);
    runs.discard(volume, upper);
    runs.discard(volume, lower);
    expect_equal(runs.size(), std::size_t {0});
}

#pragma endregion Strings Merges

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

    run_test_case("test_compactor_choice_follows_the_break_even", test_compactor_choice_follows_the_break_even);
    auto const cases = [&](auto const& mount) {
        report_on_mount(mount, "test_flush_lands_on_level_zero", test_flush_lands_on_level_zero);
        report_on_mount(mount, "test_restore_assigns_ingested_stamps", test_restore_assigns_ingested_stamps);
        report_on_mount(mount, "test_overlapping_runs_take_the_youngest", test_overlapping_runs_take_the_youngest);
        report_on_mount(mount, "test_compaction_drops_remove_deltas_at_the_bottom",
                        test_compaction_drops_remove_deltas_at_the_bottom);
        report_on_mount(mount, "test_intermediate_merge_keeps_remove_deltas",
                        test_intermediate_merge_keeps_remove_deltas);
        report_on_mount(mount, "test_cancelled_compaction_leaves_nothing", test_cancelled_compaction_leaves_nothing);
        report_on_mount(mount, "test_raced_cancellation_is_all_or_nothing", test_raced_cancellation_is_all_or_nothing);
        report_on_mount(mount, "test_trivial_move_writes_nothing", test_trivial_move_writes_nothing);
        report_on_mount(mount, "test_stale_run_handle_resolves_to_nothing", test_stale_run_handle_resolves_to_nothing);
        report_on_mount(mount, "test_compaction_history_matches_reference", test_compaction_history_matches_reference);
        report_on_mount(mount, "test_both_compactors_agree", test_both_compactors_agree);
        report_on_mount(mount, "test_merge_charges_no_index_copy", test_merge_charges_no_index_copy);
        report_on_mount(mount, "test_merge_runs_on_one_block_per_pool", test_merge_runs_on_one_block_per_pool);
        report_on_mount(mount, "test_batched_read_holds_its_depth", test_batched_read_holds_its_depth);
        report_on_mount(mount, "test_strings_merges_match_reference", test_strings_merges_match_reference);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(transport, directory.path(), lsm_config_k, 1, "tree", cases);

    return finish_test_run();
}
