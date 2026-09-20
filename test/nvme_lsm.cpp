/**
 *  @file test/nvme_lsm.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The LSM level and the tree over the ring: what a flush publishes, what a merge keeps, and how deep it reads.
 *
 *  The verdicts this file exists for are @c cancelled_leaves_nothing and @c holds_its_depth. A cancelled
 *  compaction must leave the level set it was replacing live, every block of both lanes back in its pool
 *  and not one byte of the volume reserved, since a merge that leaks either is worse than one that never
 *  ran. A batched read must sit at the lease's depth through the middle of a run rather than at the size
 *  of whatever batch a caller handed it.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstdlib> // `std::malloc`, `std::aligned_alloc`, `std::free`
#include <cstring> // `std::memcmp`

#include <array>   // `std::array`
#include <memory>  // `std::unique_ptr`
#include <new>     // `std::bad_alloc`, `std::align_val_t`, `std::nothrow_t`
#include <span>    // `std::span`
#include <thread>  // `std::thread`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "nvme/key_slot.hpp"         // `key_slot`
#include "nvme/lsm_level.hpp"        // `lsm_level`, `lsm_level_order_t`, `lsm_level_depth_t`
#include "nvme/lsm_level_merger.hpp" // `lsm_level_merger`, `choose_compactor`, `compaction_cancellation_t`
#include "nvme/lsm_tree.hpp"         // `lsm_tree`, `compaction_outcome_t`

#include "nvme_fixture.hpp" // `basic_fixture`, `on_transport`, `expectation_t`, `painted`

#pragma region Allocation Watch

/** The allocation sizes one case watches for, and how often a request of each size arrived. */
struct allocation_watch_t {

    /** Most sizes one case watches at once. */
    static constexpr std::size_t capacity_k = 4;

    /** The byte sizes watched, which a request matches exactly or not at all. */
    std::size_t sizes[capacity_k] = {};

    /** Requests seen of each watched size. */
    std::size_t hits[capacity_k] = {};

    /** Sizes watched right now, zero between cases. */
    std::size_t count = 0;
};

/** The one watch the replaced allocation functions write to, since a global @c operator @c new has no other context. */
allocation_watch_t allocation_watch_g;

/** Counts @p bytes against the watched sizes, which every replaced allocation function passes through. */
void* watched_allocate(std::size_t bytes, std::size_t alignment) noexcept {
    for (std::size_t index = 0; index != allocation_watch_g.count; ++index)
        if (allocation_watch_g.sizes[index] == bytes) ++allocation_watch_g.hits[index];
    return alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__
               ? std::aligned_alloc(alignment, (bytes + alignment - 1) / alignment * alignment)
               : std::malloc(bytes);
}

void* operator new(std::size_t bytes) {
    void* const pointer = watched_allocate(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    if (pointer == nullptr) throw std::bad_alloc {};
    return pointer;
}
void* operator new[](std::size_t bytes) {
    void* const pointer = watched_allocate(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    if (pointer == nullptr) throw std::bad_alloc {};
    return pointer;
}
void* operator new(std::size_t bytes, std::nothrow_t const&) noexcept {
    return watched_allocate(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void* operator new[](std::size_t bytes, std::nothrow_t const&) noexcept {
    return watched_allocate(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    void* const pointer = watched_allocate(bytes, static_cast<std::size_t>(alignment));
    if (pointer == nullptr) throw std::bad_alloc {};
    return pointer;
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    void* const pointer = watched_allocate(bytes, static_cast<std::size_t>(alignment));
    if (pointer == nullptr) throw std::bad_alloc {};
    return pointer;
}
void* operator new(std::size_t bytes, std::align_val_t alignment, std::nothrow_t const&) noexcept {
    return watched_allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, std::nothrow_t const&) noexcept {
    return watched_allocate(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::nothrow_t const&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::nothrow_t const&) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t, std::nothrow_t const&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t, std::nothrow_t const&) noexcept { std::free(pointer); }

#pragma endregion Allocation Watch

namespace unum::ustore {

using namespace unum::ustore::nvme;

#pragma region Shape

/** The numbers every case mounts with, which are the defaults: lanes enough for a compaction to hold two at once. */
constexpr fixture_shape_t lsm_shape_k {};

/** The bytes level zero admits before a compaction is due, kept small so a few flushes reach it. */
constexpr std::uint64_t level_zero_bytes_k = 256ull * 1024ull;

/** The largest value the collection under test admits. */
constexpr stored_value_size_t value_max_size_k = 128ull * 1024ull;

/** The keys and the key slots every case of this suite drives. */
using stored_key_t = std::uint64_t;
using variable_slot_t = key_slot<stored_key_t, value_layout_t::variable_width_k>;
using layout_t = sorted_run_layout<variable_slot_t>;

/** The runs, the levels and the trees every case drives over the volume of @p fixture_type_. */
template <typename fixture_type_>
struct lsm_types {
    using fixture_t = fixture_type_;
    using guard_t = typename fixture_t::guard_t;
    using volume_t = typename fixture_t::volume_t;
    using sorted_run_t = lsm_sorted_run<variable_slot_t, volume_t>;
    using level_t = lsm_level<variable_slot_t, volume_t>;
    using merger_t = lsm_level_merger<variable_slot_t, volume_t>;
    using tree_t = lsm_tree<variable_slot_t, volume_t>;
};

/** The tree shape every case mounts, whose first level fills after a handful of flushes. */
[[nodiscard]] lsm_tree_config_t test_tree_config() noexcept {
    lsm_tree_config_t config;
    config.lsm_level_0.max_bytes = level_zero_bytes_k;
    config.lsm_level_0.sorted_run.value_max_size = value_max_size_k;
    config.level_enlarge_factor = 4;
    return config;
}

#pragma endregion Shape

#pragma region Model

/** The key slots and the bytes behind them that one flush writes. */
struct batch_t {

    /** Key slots in ascending order of their keys. */
    st::basic_vector<variable_slot_t> key_slots;

    /** The values back to back, in the order their key slots locate them. */
    st::basic_vector<byte_t> values;
};

/** Lays @p wanted out as one run's key slots and values, which must already be in ascending key order. */
[[nodiscard]] std::size_t build_batch(std::span<expectation_t const> wanted, batch_t& into) {
    std::size_t failures = 0;
    failures += expect(into.key_slots.resize(wanted.size()) == st::status_t::success_k);
    if (failures != 0) return failures;

    std::uint64_t total = 0;
    for (std::size_t index = 0; index != wanted.size(); ++index)
        if (wanted[index].state == select_result_t::success_k) total += wanted[index].bytes;
    failures += expect(into.values.resize(static_cast<std::size_t>(total)) == st::status_t::success_k);
    if (failures != 0) return failures;

    std::uint64_t location = layout_t::values_offset(wanted.size());
    std::uint64_t written = 0;
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        expectation_t const& entry = wanted[index];
        variable_slot_t slot;
        slot.stored_key = entry.stored_key;
        if (entry.state == select_result_t::erased_k) slot.make_tombstone();
        else if (entry.bytes == 0) {
            slot.size = 0;
            slot.location = empty_stored_value_offset_k;
        }
        else {
            slot.size = static_cast<stored_value_size_t>(entry.bytes);
            slot.location = static_cast<stored_value_offset_t>(location);
            for (std::uint64_t byte = 0; byte != entry.bytes; ++byte)
                into.values[static_cast<std::size_t>(written + byte)] = painted(entry.seed, byte);
            location += entry.bytes;
            written += entry.bytes;
        }
        into.key_slots[index] = slot;
    }
    return failures;
}

/** Flushes @p wanted onto level zero of @p tree as one run. */
template <typename tree_type_>
[[nodiscard]] std::size_t flush_batch(tree_type_& tree, io_lane_t& lane, std::span<expectation_t const> wanted) {
    std::size_t failures = 0;
    batch_t batch;
    failures += build_batch(wanted, batch);
    if (failures != 0) return failures;
    failures += expect(tree.flush(lane, std::span<variable_slot_t const> {batch.key_slots},
                                  std::span<byte_t const> {batch.values}, value_max_size_k)
                           .succeeded());
    return failures;
}

/** Reads every key of @p wanted out of @p tree and counts what it does not answer as the model says. */
template <typename tree_type_>
[[nodiscard]] std::size_t check_model(tree_type_ const& tree, io_lane_t& lane, std::span<expectation_t const> wanted) {
    std::size_t failures = 0;
    std::size_t const count = wanted.size();
    st::basic_vector<stored_key_t> keys;
    st::basic_vector<find_result_t> results;
    st::basic_vector<std::uint64_t> seen;
    failures += expect(keys.resize(count) == st::status_t::success_k);
    failures += expect(results.resize(count) == st::status_t::success_k);
    failures += expect(seen.resize(count) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count; ++index) {
        keys[index] = wanted[index].stored_key;
        results[index] = find_result_t::nothing_k;
        seen[index] = 0;
    }

    std::size_t mismatches = 0;
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        seen[request] += bytes.size();
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(wanted[request].seed, offset + index)) ++mismatches;
    };

    typename tree_type_::snapshot_t const taken = tree.snapshot();
    failures += expect(
        tree.read_values(lane, taken, std::span<stored_key_t const> {keys}, std::span<find_result_t> {results}, sink)
            .succeeded());
    failures += expect_eq(mismatches, std::size_t {0});
    for (std::size_t index = 0; index != count; ++index) {
        switch (wanted[index].state) {
        case select_result_t::nothing_k: failures += expect(results[index] == find_result_t::nothing_k); break;
        case select_result_t::erased_k: failures += expect(results[index] == find_result_t::stored_key_k); break;
        case select_result_t::success_k:
            failures += expect(results[index] == find_result_t::success_k);
            failures += expect_eq(seen[index], wanted[index].bytes);
            break;
        }
    }
    return failures;
}

/** Reads every value of every run of @p level back to back, which is what two compactors must agree on. */
template <typename level_type_>
[[nodiscard]] std::size_t collect_level_values(level_type_ const& level, io_lane_t& lane,
                                               st::basic_vector<byte_t>& into) {
    std::size_t failures = 0;
    for (std::size_t run = 0; run != level.size(); ++run) {
        auto const& source = level.run(run);
        std::size_t const count = static_cast<std::size_t>(source.population_count());
        st::basic_vector<std::uint64_t> positions;
        st::basic_vector<std::size_t> starts;
        failures += expect(positions.resize(count) == st::status_t::success_k);
        failures += expect(starts.resize(count) == st::status_t::success_k);
        if (failures != 0) return failures;
        std::size_t const base = into.size();
        std::size_t total = 0;
        for (std::size_t at = 0; at != count; ++at) {
            positions[at] = at;
            starts[at] = base + total;
            auto const& slot = source.key_slot(at);
            total += slot.is_tombstone() ? 0 : slot.size;
        }
        failures += expect(into.resize(base + total) == st::status_t::success_k);
        if (failures != 0) return failures;

        // Completions arrive in the transport's order, so every piece lands by its request and offset.
        auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            std::memcpy(into.data() + starts[request] + offset, bytes.data(), bytes.size());
        };
        failures += expect(source.read_values(lane, std::span<std::uint64_t const> {positions}, sink).succeeded());
    }
    return failures;
}

/** Writes @p wanted as one run of @p volume and takes it into @p level, which is how a case builds a level. */
template <typename level_type_, typename volume_type_>
[[nodiscard]] std::size_t adopt_batch(level_type_& level, volume_type_& volume, io_lane_t& lane,
                                      std::span<expectation_t const> wanted) {
    using sorted_run_t = lsm_sorted_run<variable_slot_t, volume_type_>;
    std::size_t failures = 0;
    batch_t batch;
    failures += build_batch(wanted, batch);
    if (failures != 0) return failures;
    expected<sorted_run_t> written = sorted_run_t::write(volume, lane, uuid_generator(),
                                                         std::span<variable_slot_t const> {batch.key_slots},
                                                         std::span<byte_t const> {batch.values}, value_max_size_k);
    failures += expect(written.has_value());
    if (!written) return failures;
    failures += expect(level.adopt(std::move(*written)).succeeded());
    return failures;
}

#pragma endregion Model

#pragma region Flush Cases

/** A flushed write buffer becomes one level-zero run whose keys a point lookup finds. */
template <typename mount_type_>
std::size_t flush_lands_on_level_zero(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using tree_t = typename types_t::tree_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 600;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->lane());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config());
    failures += expect(tree.has_value());
    if (!tree) return failures;

    st::basic_vector<expectation_t> model;
    failures += expect(model.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 2 + 1;
        entry.state = index % 7 == 0 ? select_result_t::erased_k : select_result_t::success_k;
        entry.bytes = entry.state == select_result_t::success_k ? 1 + index % 1021 : 0;
        entry.seed = index;
        model[index] = entry;
    }

    failures += flush_batch(*tree, guard->lane(), std::span<expectation_t const> {model});
    if (failures != 0) return failures;

    {
        typename tree_t::snapshot_t const taken = tree->snapshot();
        failures += expect_eq(taken.size(), std::size_t {1});
        failures += expect_eq(taken.level(0).size(), std::size_t {1});
        failures += expect(taken.level(0).order() == lsm_level_order_t::overlapping_k);
        failures += expect_eq(taken.population_count(), static_cast<std::uint64_t>(count_k));
    }
    failures += check_model(*tree, guard->lane(), std::span<expectation_t const> {model});

    // Every even key is a miss, since the model only ever wrote odd ones.
    st::basic_vector<expectation_t> absent;
    failures += expect(absent.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 2 + 2;
        entry.state = select_result_t::nothing_k;
        absent[index] = entry;
    }
    failures += check_model(*tree, guard->lane(), std::span<expectation_t const> {absent});
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
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 400;
    constexpr std::size_t flushes_k = 5;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->lane());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config());
    failures += expect(tree.has_value());
    if (!tree) return failures;

    st::basic_vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index);
        entry.state = select_result_t::nothing_k;
        model[index] = entry;
    }

    // Every flush rewrites the keys of one residue class, so the runs overlap and the last one wins.
    for (std::size_t flush = 0; flush != flushes_k; ++flush) {
        st::basic_vector<expectation_t> batch;
        for (std::size_t index = 0; index != keys_k; ++index) {
            if ((index + flush) % 3 != 0) continue;
            expectation_t entry;
            entry.stored_key = static_cast<stored_key_t>(index);
            entry.state = flush % 4 == 3 ? select_result_t::erased_k : select_result_t::success_k;
            entry.bytes = entry.state == select_result_t::success_k ? 1 + (index * 13 + flush) % 997 : 0;
            entry.seed = flush * keys_k + index;
            failures += expect(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
            model[index] = entry;
        }
        failures += flush_batch(*tree, guard->lane(), std::span<expectation_t const> {batch});
        if (failures != 0) return failures;
    }

    {
        typename tree_t::snapshot_t const taken = tree->snapshot();
        failures += expect_eq(taken.level(0).size(), flushes_k);
    }
    failures += check_model(*tree, guard->lane(), std::span<expectation_t const> {model});

    // A merged walk of level zero reports every key once, in ascending order, at its youngest run.
    typename tree_t::snapshot_t const taken = tree->snapshot();
    std::size_t visited = 0;
    std::size_t disordered = 0;
    stored_key_t previous = 0;
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
        if (model[index].state != select_result_t::nothing_k) ++live_keys;
    failures += expect_eq(visited, live_keys);
    return failures;
}

#pragma endregion Flush Cases

#pragma region Compaction Cases

/** A compaction merges level zero into level one, keeps the youngest value and forgets the tombstones. */
template <typename mount_type_>
std::size_t compaction_drops_tombstones_at_the_bottom(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using tree_t = typename types_t::tree_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 900;
    constexpr std::size_t flushes_k = 4;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    expected<guard_t> reading = (*fixture)->lanes.checkout();
    expected<guard_t> writing = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->lane());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config());
    failures += expect(tree.has_value());
    if (!tree) return failures;

    st::basic_vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index);
        entry.state = select_result_t::nothing_k;
        model[index] = entry;
    }

    for (std::size_t flush = 0; flush != flushes_k; ++flush) {
        st::basic_vector<expectation_t> batch;
        for (std::size_t index = 0; index != keys_k; ++index) {
            if ((index + flush) % 2 != 0) continue;
            expectation_t entry;
            entry.stored_key = static_cast<stored_key_t>(index);
            entry.state = (index % 5 == 0 && flush + 1 == flushes_k) ? select_result_t::erased_k
                                                                     : select_result_t::success_k;
            entry.bytes = entry.state == select_result_t::success_k ? 1 + (index * 29 + flush) % 2039 : 0;
            entry.seed = flush * keys_k + index + 1;
            failures += expect(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
            model[index] = entry;
        }
        failures += flush_batch(*tree, guard->lane(), std::span<expectation_t const> {batch});
        if (failures != 0) return failures;
    }

    failures += expect(tree->needs_compaction());
    expected<compaction_outcome_t> const outcome = tree->compact(reading->lane(), writing->lane(),
                                                                 compaction_placement_t::host_k, nullptr);
    failures += expect(outcome.has_value());
    if (!outcome) return failures;
    failures += expect(*outcome == compaction_outcome_t::published_k);

    // The bottom level forgets a tombstone for good, so an erased key becomes a key nothing carries.
    for (std::size_t index = 0; index != keys_k; ++index)
        if (model[index].state == select_result_t::erased_k) model[index].state = select_result_t::nothing_k;

    {
        typename tree_t::snapshot_t const taken = tree->snapshot();
        failures += expect_eq(taken.size(), std::size_t {2});
        failures += expect(taken.level(0).empty());
        failures += expect(!taken.level(1).empty());
        failures += expect(taken.level(1).order() == lsm_level_order_t::sorted_k);
        std::size_t live_keys = 0;
        for (std::size_t index = 0; index != keys_k; ++index)
            if (model[index].state != select_result_t::nothing_k) ++live_keys;
        failures += expect_eq(taken.level(1).population_count(), static_cast<std::uint64_t>(live_keys));
    }
    failures += check_model(*tree, guard->lane(), std::span<expectation_t const> {model});
    return failures;
}

/** The same two levels merged into an intermediate level keep every tombstone the bottom would drop. */
template <typename mount_type_>
std::size_t intermediate_merge_keeps_tombstones(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using level_t = typename types_t::level_t;
    using merger_t = typename types_t::merger_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 240;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    expected<guard_t> reading = (*fixture)->lanes.checkout();
    expected<guard_t> writing = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->lane());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    st::basic_vector<expectation_t> older;
    st::basic_vector<expectation_t> newer;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index);
        entry.state = select_result_t::success_k;
        entry.bytes = 1 + index % 311;
        entry.seed = index + 1;
        failures += expect(older.push_back(expectation_t {entry}) == st::status_t::success_k);
        if (index % 3 != 0) continue;
        entry.state = select_result_t::erased_k;
        entry.bytes = 0;
        failures += expect(newer.push_back(expectation_t {entry}) == st::status_t::success_k);
    }
    if (failures != 0) return failures;

    std::size_t erased_keys = 0;
    for (std::size_t index = 0; index != keys_k; ++index)
        if (index % 3 == 0) ++erased_keys;

    // One level of two overlapping runs, merged twice with nothing changed but the depth.
    level_t upper = level_t::of(0, lsm_level_order_t::overlapping_k);
    failures += adopt_batch(upper, *volume, guard->lane(), std::span<expectation_t const> {older});
    failures += adopt_batch(upper, *volume, guard->lane(), std::span<expectation_t const> {newer});
    if (failures != 0) return failures;

    typename merger_t::merge_request_t request;
    request.upper = &upper;
    request.lower = nullptr;
    request.number = 1;
    request.value_max_size = value_max_size_k;
    request.placement = compaction_placement_t::host_k;

    request.depth = lsm_level_depth_t::intermediate_k;
    expected<level_t> kept = merger_t::merge(*volume, reading->lane(), writing->lane(), request);
    failures += expect_status(kept, status_code_t::success_k);
    if (!kept) return failures;
    failures += expect_eq(kept->population_count(), static_cast<std::uint64_t>(keys_k));

    request.depth = lsm_level_depth_t::bottom_k;
    expected<level_t> dropped = merger_t::merge(*volume, reading->lane(), writing->lane(), request);
    failures += expect(dropped.has_value());
    if (!dropped) return failures;
    failures += expect_eq(dropped->population_count(), static_cast<std::uint64_t>(keys_k - erased_keys));

    // The intermediate level answers an erased key as a key it carries, and the bottom one as a miss.
    for (std::size_t index = 0; index < keys_k; index += 3) {
        lsm_level_position_t position;
        failures += expect(kept->find(static_cast<stored_key_t>(index), position) == find_result_t::stored_key_k);
        failures += expect(dropped->find(static_cast<stored_key_t>(index), position) == find_result_t::nothing_k);
    }
    for (std::size_t index = 1; index < keys_k; index += 3) {
        lsm_level_position_t position;
        failures += expect(kept->find(static_cast<stored_key_t>(index), position) == find_result_t::success_k);
        failures += expect(dropped->find(static_cast<stored_key_t>(index), position) == find_result_t::success_k);
    }

    failures += expect(kept->drop(*volume, writing->lane()).succeeded());
    failures += expect(dropped->drop(*volume, writing->lane()).succeeded());
    failures += expect(upper.drop(*volume, writing->lane()).succeeded());
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
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 700;
    constexpr std::size_t flushes_k = 4;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    expected<guard_t> reading = (*fixture)->lanes.checkout();
    expected<guard_t> writing = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->lane());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config());
    failures += expect(tree.has_value());
    if (!tree) return failures;

    st::basic_vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index);
        entry.state = select_result_t::success_k;
        entry.bytes = 1 + index % 1531;
        entry.seed = index + 3;
        model[index] = entry;
    }
    for (std::size_t flush = 0; flush != flushes_k; ++flush) {
        st::basic_vector<expectation_t> batch;
        for (std::size_t index = flush; index < keys_k; index += flushes_k)
            failures += expect(batch.push_back(expectation_t {model[index]}) == st::status_t::success_k);
        failures += flush_batch(*tree, guard->lane(), std::span<expectation_t const> {batch});
        if (failures != 0) return failures;
    }
    failures += expect(tree->needs_compaction());

    std::uint64_t const free_before = volume->free_bytes();
    std::size_t const reserved_before = volume->reservations();
    std::size_t const blobs_before = volume->size();
    std::uint32_t const requests_before = reading->lane().requests().available();
    std::uint32_t const streams_before = writing->lane().streams().available();
    std::uint64_t population_before = 0;
    std::size_t runs_before = 0;
    {
        typename tree_t::snapshot_t const taken = tree->snapshot();
        population_before = taken.population_count();
        runs_before = taken.level(0).size();
    }

    compaction_cancellation_t cancellation;
    cancellation.raise();
    expected<compaction_outcome_t> const outcome = tree->compact(reading->lane(), writing->lane(),
                                                                 compaction_placement_t::host_k, &cancellation);
    failures += expect(!outcome.has_value());
    failures += expect_status(outcome, status_code_t::uninitialized_k);

    // Nothing the merge took is still out: no lease, no block and no reservation.
    failures += expect_eq(reading->lane().requests().available(), requests_before);
    failures += expect_eq(writing->lane().streams().available(), streams_before);
    failures += expect_eq(writing->lane().requests().available(), lsm_shape_k.request_blocks);
    failures += expect_eq(volume->free_bytes(), free_before);
    failures += expect_eq(volume->reservations(), reserved_before);
    failures += expect_eq(volume->size(), blobs_before);
    failures += expect_eq((*fixture)->disk.in_flight(reading->lane().owner()), std::size_t {0});
    failures += expect_eq((*fixture)->disk.in_flight(writing->lane().owner()), std::size_t {0});

    {
        typename tree_t::snapshot_t const taken = tree->snapshot();
        failures += expect_eq(taken.size(), std::size_t {1});
        failures += expect_eq(taken.level(0).size(), runs_before);
        failures += expect_eq(taken.population_count(), population_before);
    }
    failures += check_model(*tree, guard->lane(), std::span<expectation_t const> {model});

    // The same tree still compacts once nobody is cancelling it.
    expected<compaction_outcome_t> const again = tree->compact(reading->lane(), writing->lane(),
                                                               compaction_placement_t::host_k, nullptr);
    failures += expect(again.has_value());
    if (again) failures += expect(*again == compaction_outcome_t::published_k);
    failures += check_model(*tree, guard->lane(), std::span<expectation_t const> {model});
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
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 800;
    constexpr std::size_t flushes_k = 4;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    expected<guard_t> reading = (*fixture)->lanes.checkout();
    expected<guard_t> writing = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->lane());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config());
    failures += expect(tree.has_value());
    if (!tree) return failures;

    st::basic_vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index);
        entry.state = select_result_t::success_k;
        entry.bytes = 1 + index % 907;
        entry.seed = index + 11;
        model[index] = entry;
    }
    for (std::size_t flush = 0; flush != flushes_k; ++flush) {
        st::basic_vector<expectation_t> batch;
        for (std::size_t index = flush; index < keys_k; index += flushes_k)
            failures += expect(batch.push_back(expectation_t {model[index]}) == st::status_t::success_k);
        failures += flush_batch(*tree, guard->lane(), std::span<expectation_t const> {batch});
        if (failures != 0) return failures;
    }

    std::uint64_t const free_before = volume->free_bytes();
    std::size_t const blobs_before = volume->size();
    std::uint64_t population_before = 0;
    {
        typename tree_t::snapshot_t const taken = tree->snapshot();
        population_before = taken.population_count();
    }

    compaction_cancellation_t cancellation;
    std::thread raiser {[&cancellation]() noexcept {
        for (std::size_t spins = 0; spins != 64; ++spins) std::this_thread::yield();
        cancellation.raise();
    }};
    expected<compaction_outcome_t> const outcome = tree->compact(reading->lane(), writing->lane(),
                                                                 compaction_placement_t::host_k, &cancellation);
    raiser.join();

    failures += expect_eq(writing->lane().streams().available(), lsm_shape_k.stream_blocks);
    failures += expect_eq(reading->lane().requests().available(), lsm_shape_k.request_blocks);
    failures += expect_eq(volume->reservations(), std::size_t {0});
    failures += expect_eq((*fixture)->disk.in_flight(writing->lane().owner()), std::size_t {0});

    typename tree_t::snapshot_t const taken = tree->snapshot();
    failures += expect_eq(taken.population_count(), population_before);
    if (!outcome) {
        failures += expect_status(outcome, status_code_t::uninitialized_k);
        failures += expect_eq(volume->free_bytes(), free_before);
        failures += expect_eq(volume->size(), blobs_before);
        failures += expect_eq(taken.size(), std::size_t {1});
    }
    else {
        failures += expect(*outcome == compaction_outcome_t::published_k);
        failures += expect_eq(taken.size(), std::size_t {2});
        failures += expect(taken.level(0).empty());
    }
    failures += check_model(*tree, guard->lane(), std::span<expectation_t const> {model});
    return failures;
}

#pragma endregion Compaction Cases

#pragma region Compactor Choice Cases

/** The threshold picks the device compactor only above it, and only where a device is present. */
std::size_t compactor_choice_follows_the_threshold() {
    std::size_t failures = 0;
    constexpr std::uint64_t threshold_k = storage_limits_t::gpu_compaction_threshold_k;

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
verdict_t both_compactors_agree(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using level_t = typename types_t::level_t;
    using merger_t = typename types_t::merger_t;
    using sorted_run_t = typename types_t::sorted_run_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 512;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    expected<guard_t> reading = (*fixture)->lanes.checkout();
    expected<guard_t> writing = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->lane());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    st::basic_vector<expectation_t> older;
    st::basic_vector<expectation_t> newer;
    for (std::size_t index = 0; index != keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index);
        entry.state = index % 11 == 0 ? select_result_t::erased_k : select_result_t::success_k;
        entry.bytes = entry.state == select_result_t::success_k ? 1 + index % 613 : 0;
        entry.seed = index + 5;
        failures += expect(older.push_back(expectation_t {entry}) == st::status_t::success_k);
        if (index % 2 != 0) continue;
        entry.bytes = entry.state == select_result_t::success_k ? 1 + index % 271 : 0;
        entry.seed = index + 4096;
        failures += expect(newer.push_back(expectation_t {entry}) == st::status_t::success_k);
    }
    if (failures != 0) return failures;

    level_t upper = level_t::of(0, lsm_level_order_t::overlapping_k);
    failures += adopt_batch(upper, *volume, guard->lane(), std::span<expectation_t const> {older});
    failures += adopt_batch(upper, *volume, guard->lane(), std::span<expectation_t const> {newer});
    if (failures != 0) return failures;

    typename merger_t::merge_request_t request;
    request.upper = &upper;
    request.number = 1;
    request.depth = lsm_level_depth_t::intermediate_k;
    request.value_max_size = value_max_size_k;

    request.placement = compaction_placement_t::host_k;
    expected<level_t> host = merger_t::merge(*volume, reading->lane(), writing->lane(), request);
    failures += expect(host.has_value());
    if (!host) return failures;

    request.placement = compaction_placement_t::device_k;
    expected<level_t> device = merger_t::merge(*volume, reading->lane(), writing->lane(), request);
    if (!device) {
        bool const no_compactor = device.status() == status_code_t::unsupported_k ||
                                  device.status() == status_code_t::device_k;
        if (!no_compactor) failures += expect_status(device, status_code_t::unsupported_k);
        failures += expect(host->drop(*volume, writing->lane()).succeeded());
        failures += expect(upper.drop(*volume, writing->lane()).succeeded());
        return no_compactor ? verdict_t::skipped("no device compactor", failures) : verdict_t {failures};
    }

    failures += expect_eq(device->size(), host->size());
    failures += expect_eq(device->population_count(), host->population_count());
    std::size_t differing = 0;
    for (std::size_t run = 0; run != host->size() && run != device->size(); ++run) {
        sorted_run_t const& left = host->run(run);
        sorted_run_t const& right = device->run(run);
        if (left.population_count() != right.population_count()) {
            ++differing;
            continue;
        }
        for (std::uint64_t at = 0; at != left.population_count(); ++at) {
            variable_slot_t const& one = left.key_slot(at);
            variable_slot_t const& two = right.key_slot(at);
            if (one.stored_key != two.stored_key || one.size != two.size || one.location != two.location) ++differing;
        }
    }
    failures += expect_eq(differing, std::size_t {0});

    // Identical cells are not identical runs until the bytes they locate come back the same too.
    st::basic_vector<byte_t> host_values;
    st::basic_vector<byte_t> device_values;
    failures += collect_level_values(*host, guard->lane(), host_values);
    failures += collect_level_values(*device, guard->lane(), device_values);
    failures += expect_eq(device_values.size(), host_values.size());
    if (device_values.size() == host_values.size())
        failures += expect_eq(std::memcmp(host_values.data(), device_values.data(), host_values.size()), 0);
    failures += expect(host->drop(*volume, writing->lane()).succeeded());
    failures += expect(device->drop(*volume, writing->lane()).succeeded());
    failures += expect(upper.drop(*volume, writing->lane()).succeeded());
    return failures;
}

#pragma endregion Compactor Choice Cases

#pragma region Allocation Cases

/**
 *  @brief A merge reads every source index in place, so it never asks the heap for a copy of one.
 *
 *  The runs overlap and the survivors outnumber every source run, so neither the compactor's records
 *  nor the new run's own index at attach is the size of a source index or of all of them together.
 */
template <typename mount_type_>
std::size_t merge_charges_no_index_copy(mount_type_ const& mount) {
    using types_t = lsm_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using level_t = typename types_t::level_t;
    using merger_t = typename types_t::merger_t;
    std::size_t failures = 0;
    constexpr std::size_t older_keys_k = 1000;
    constexpr std::size_t newer_keys_k = 300;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    expected<guard_t> reading = (*fixture)->lanes.checkout();
    expected<guard_t> writing = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value() && reading.has_value() && writing.has_value());
    if (!guard || !reading || !writing) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->lane());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    // Half of the newer keys shadow older ones and half sit past them, so the survivors number 1150.
    st::basic_vector<expectation_t> older;
    st::basic_vector<expectation_t> newer;
    for (std::size_t index = 0; index != older_keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index);
        entry.state = index % 11 == 0 ? select_result_t::erased_k : select_result_t::success_k;
        entry.bytes = entry.state == select_result_t::success_k ? 1 + index % 613 : 0;
        entry.seed = index + 5;
        failures += expect(older.push_back(expectation_t {entry}) == st::status_t::success_k);
    }
    for (std::size_t index = 0; index != newer_keys_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index < newer_keys_k / 2 ? index * 2 : older_keys_k + index);
        entry.state = select_result_t::success_k;
        entry.bytes = 1 + index % 271;
        entry.seed = index + 4096;
        failures += expect(newer.push_back(expectation_t {entry}) == st::status_t::success_k);
    }
    if (failures != 0) return failures;

    level_t upper = level_t::of(0, lsm_level_order_t::overlapping_k);
    failures += adopt_batch(upper, *volume, guard->lane(), std::span<expectation_t const> {older});
    failures += adopt_batch(upper, *volume, guard->lane(), std::span<expectation_t const> {newer});
    if (failures != 0) return failures;

    // The sizes a copy of one source index, or of both back to back, would be requested at.
    allocation_watch_t& watch = allocation_watch_g;
    watch = allocation_watch_t {};
    std::size_t all_indexes = 0;
    for (std::size_t run = 0; run != upper.size(); ++run) {
        std::size_t const index_bytes = upper.run(run).cells().size_bytes();
        watch.sizes[watch.count++] = index_bytes;
        all_indexes += index_bytes;
    }
    watch.sizes[watch.count++] = all_indexes;

    typename merger_t::merge_request_t request;
    request.upper = &upper;
    request.number = 1;
    request.depth = lsm_level_depth_t::intermediate_k;
    request.value_max_size = value_max_size_k;
    request.placement = compaction_placement_t::host_k;
    expected<level_t> merged = merger_t::merge(*volume, reading->lane(), writing->lane(), request);
    allocation_watch_t const seen = watch;
    watch = allocation_watch_t {};
    failures += expect_status(merged, status_code_t::success_k);
    if (!merged) return failures;

    failures += expect_eq(merged->population_count(), std::uint64_t {older_keys_k + newer_keys_k / 2});
    for (std::size_t index = 0; index != seen.count; ++index) failures += expect_eq(seen.hits[index], std::size_t {0});
    failures += expect(merged->drop(*volume, writing->lane()).succeeded());
    failures += expect(upper.drop(*volume, writing->lane()).succeeded());
    return failures;
}

#pragma endregion Allocation Cases

#pragma region Depth Cases

/**
 *  @brief A batched read of many keys across several runs sits at the lease's depth through the middle.
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
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 1024;
    constexpr std::size_t flushes_k = 4;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->lane());
    failures += expect(volume.has_value());
    if (!volume) return failures;
    expected<tree_t> tree = tree_t::mount(*volume, test_tree_config());
    failures += expect(tree.has_value());
    if (!tree) return failures;

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
        failures += flush_batch(*tree, guard->lane(), std::span<expectation_t const> {batch});
        if (failures != 0) return failures;
    }

    st::basic_vector<stored_key_t> keys;
    st::basic_vector<find_result_t> results;
    failures += expect(keys.resize(keys_k) == st::status_t::success_k);
    failures += expect(results.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) keys[index] = model[index].stored_key;

    // The blocks a batched read holds are counted in the pool, since the ring performs a lane's
    // requests inside the reap that hands them to the sink.
    std::uint32_t idlest = lsm_shape_k.request_blocks;
    std::size_t mismatches = 0;
    auto counting = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        std::uint32_t const free_blocks = guard->lane().requests().available();
        if (free_blocks < idlest) idlest = free_blocks;
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(model[request].seed, offset + index)) ++mismatches;
    };

    typename tree_t::snapshot_t const taken = tree->snapshot();
    failures += expect_eq(taken.level(0).size(), flushes_k);
    failures += expect(tree->read_values(guard->lane(), taken, std::span<stored_key_t const> {keys},
                                         std::span<find_result_t> {results}, counting)
                           .succeeded());
    failures += expect_eq(mismatches, std::size_t {0});
    failures += expect_eq(lsm_shape_k.request_blocks - idlest, lsm_shape_k.lease_depth);
    for (std::size_t index = 0; index != keys_k; ++index)
        failures += expect(results[index] == find_result_t::success_k);
    return failures;
}

#pragma endregion Depth Cases

} // namespace unum::ustore

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;

    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    ledger_t ledger = options.ledger();

    st::expected<test_directory_t> const directory = test_directory_t::make(options.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    medium_t const medium = medium_t::of(options, *directory);

    report(ledger, "lsm.compactor_choice", compactor_choice_follows_the_threshold());
    auto const cases = [&](auto const& mount) {
        report(ledger, mount.label("flush_lands_on_level_zero").c_str(), flush_lands_on_level_zero(mount));
        report(ledger, mount.label("overlapping_takes_youngest").c_str(), overlapping_runs_take_the_youngest(mount));
        report(ledger, mount.label("drops_tombstones_at_bottom").c_str(),
               compaction_drops_tombstones_at_the_bottom(mount));
        report(ledger, mount.label("intermediate_keeps_tombstones").c_str(),
               intermediate_merge_keeps_tombstones(mount));
        report(ledger, mount.label("cancelled_leaves_nothing").c_str(), cancelled_compaction_leaves_nothing(mount));
        report(ledger, mount.label("raced_cancellation").c_str(), raced_cancellation_is_all_or_nothing(mount));
        report(ledger, mount.label("compactors_agree").c_str(), both_compactors_agree(mount));
        report(ledger, mount.label("merge_charges_no_index_copy").c_str(), merge_charges_no_index_copy(mount));
        report(ledger, mount.label("holds_its_depth").c_str(), batched_read_holds_its_depth(mount));
    };
    for (transport_t const transport : every_transport_k)
        on_transport(ledger, transport, medium, lsm_shape_k, 1, "lsm", cases);

    return conclude(ledger);
}
