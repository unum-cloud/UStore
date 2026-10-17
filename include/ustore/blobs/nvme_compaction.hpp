/**
 *  @file include/ustore/blobs/nvme_compaction.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Compaction of LSM levels into new sorted runs on the host, and the device compactor every compiler sees.
 *
 *  A compaction reads the sorted run indexes of an upper LSM level and of the lower sorted runs it overlaps, keeps the
 *  newest key slot of every stored key, drops tombstones when it writes the bottom LSM level, and lays the survivors
 *  into sorted runs that fit their bounds, planning where every stored value moves. Both compactors share the rules
 *  below. A build with @c USTORE_WITH_CUDA declares the device compactor for every compiler, its kernels in
 *  @c nvme_compaction.cuh compile under nvcc alone, and @c compaction.cu instantiates its compaction for the
 *  engine's key slots, so a host TU calls it.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::int64_t`, `std::uint32_t`, `std::uint64_t`

#include <concepts>    // `std::convertible_to`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::allocator_traits`
#include <span>        // `std::span`
#include <type_traits> // `std::is_trivially_copyable_v`
#include <utility>     // `std::move`, `std::swap`

#include <smashtable/basic_vector.hpp>           // `st::basic_vector`
#include <smashtable/immutable_packed_array.hpp> // `st::immutable_packed_array`
#include <smashtable/shared.hpp>                 // `st::status_t`, `st::default_allocator`

#include "ustore/blobs/nvme_key_slot.hpp"          // `limit`, `stored_key_concept`, `sorted_run_index_layout_t`
#include "ustore/utilities/types.hpp"              // `round_up_to_multiple`
#include "ustore/utilities/vram_nvidia_device.hpp" // `cuda_stream_t`

namespace unum::ustore::blobs {

#pragma region Vocabulary

/** A key slot shape compaction merges: trivially copyable, keyed by a stored key, sized, and able to say whether it is
 *  a tombstone. */
template <typename key_slot_type_>
concept compactable_key_slot_concept = std::is_trivially_copyable_v<key_slot_type_> &&
                                       stored_key_concept<typename key_slot_type_::stored_key_t> &&
                                       requires(key_slot_type_ const& key_slot) {
                                           {
                                               key_slot.stored_key
                                           } -> std::convertible_to<typename key_slot_type_::stored_key_t>;
                                           { key_slot.size } -> std::convertible_to<std::uint64_t>;
                                           { key_slot.is_tombstone() } -> std::convertible_to<bool>;
                                       };

/** A key slot shape whose stored values live in its sorted run's stored values section, found by location. */
template <typename key_slot_type_>
concept locates_stored_values_concept = requires(key_slot_type_ const& key_slot) {
    { key_slot.location } -> std::convertible_to<std::uint64_t>;
    { key_slot.is_stored_value_empty() } -> std::convertible_to<bool>;
};

/** Which LSM levels a compaction merges, fixing whether the upper LSM level overlaps and whether tombstones drop. */
enum class compaction_kind_t : std::uint8_t {

    /** Overlapping L0 sorted runs into an LSM level with LSM levels below it, keeping tombstones. */
    overlapping_into_intermediate_k,

    /** Overlapping L0 sorted runs into the bottom LSM level, dropping tombstones. */
    overlapping_into_bottom_k,

    /** A sorted LSM level into the LSM level below it, which has LSM levels below it too, keeping tombstones. */
    sorted_into_intermediate_k,

    /** A sorted LSM level into the bottom LSM level, or into a new bottom LSM level, dropping tombstones. */
    sorted_into_bottom_k,
};

/** The stamps of one sorted run's key slots, in sorted order, as the run keeps them packed beside its index. */
using stamps_column_t = st::immutable_packed_array<std::uint64_t>;

/** The key slots of one sorted run as its sorted run index's cells hold them, and the stamp of every one. */
template <typename key_slot_type_>
struct sorted_run_index_view {

    /** The sorted run index's cells, whole rows including padding. */
    key_slot_type_ const* cells = nullptr;

    /** Key slots in the sorted run index, padding excluded. */
    std::uint64_t key_slots_count = 0;

    /** The stamp of every key slot in sorted order, or null for a run whose every stamp reads as zero. */
    stamps_column_t const* stamps = nullptr;
};

/**
 *  @brief The stamps a merge keeps versions for beyond the newest of every stored key, and the range it keeps.
 *
 *  With no reader pinned and both bounds at their widest, a merge keeps exactly the newest version of every key,
 *  which is the rule a store without stamps would follow.
 */
struct compaction_stamps_t {

    /** Every stamp a live reader reads at, ascending and distinct: the newest version at or below each survives. */
    std::uint64_t const* pinned = nullptr;

    /** How many stamps @c pinned holds. */
    std::uint64_t pinned_count = 0;

    /** The newest stamp a catalog can promise durable: every version above it survives, as a mount may drop it. */
    std::uint64_t horizon = std::numeric_limits<std::uint64_t>::max();

    /** The newest stamp a merge keeps at all, which a mount sets to drop what no durable catalog covers. */
    std::uint64_t ceiling = std::numeric_limits<std::uint64_t>::max();

    /** The stamp every surviving stamp is raised to at least, which no reader and no mount can tell apart. */
    std::uint64_t floor = 0;
};

/** Bounds every sorted run a compaction writes must fit, derived from the sorted run layout and the storage limits. */
struct sorted_run_bounds_t {

    /** Most key slots in one sorted run. */
    std::uint64_t key_slots_capacity = 0;

    /** Largest sorted run file in bytes, which a sorted run of one key slot may still exceed. */
    std::uint64_t file_bytes_limit = 0;

    /** Bytes of the serialized header, aligned already. */
    std::uint64_t header_bytes = 0;

    /** Alignment of the key slots and stored values sections. */
    std::uint64_t alignment_bytes = 0;

    /** Cells in one sorted run index row, the same for the input and the output sorted runs. */
    std::uint64_t branching = 0;
};

/** One sorted run a compaction produced. */
struct compacted_sorted_run_t {

    /** Key slots in the sorted run, padding cells excluded. */
    std::uint64_t key_slots_count = 0;

    /** Bytes of the stored values its key slots locate, before alignment. */
    std::uint64_t stored_values_bytes = 0;
};

/** Where one key slot's stored value comes from; a size of zero leaves nothing to copy. */
struct stored_value_migration_t {

    /** Bytes of the stored value. */
    std::uint32_t size = 0;

    /** Location of the stored value in its old sorted run. */
    std::uint32_t old_location = 0;

    /** Index of the old sorted run, counting the upper LSM level first and the lower LSM level after it. */
    std::uint32_t old_sorted_run = 0;
};

/** A SmashTable vector of @p element_type_ on the allocator @p allocator_type_ rebinds to it. */
template <typename element_type_, typename allocator_type_>
using rebound_vector =
    st::basic_vector<element_type_,
                     typename std::allocator_traits<allocator_type_>::template rebind_alloc<element_type_>>;

/** What one compaction writes, kept between compactions so their buffers are reused. */
template <typename key_slot_type_, typename allocator_type_ = st::default_allocator<key_slot_type_>>
struct compaction_output {

    /** Sorted run indexes of the new sorted runs one after another, padding included. */
    rebound_vector<key_slot_type_, allocator_type_> cells;

    /** The new sorted runs in ascending order of stored keys. */
    rebound_vector<compacted_sorted_run_t, allocator_type_> sorted_runs;

    /** One migration per key slot in sorted order across the sorted runs, empty for stored values held inline. */
    rebound_vector<stored_value_migration_t, allocator_type_> migrations;

    /** One stamp per key slot in sorted order across the sorted runs, each at or above the merge's floor. */
    rebound_vector<std::uint64_t, allocator_type_> stamps;

    compaction_output() noexcept = default;

    /** Allocates every buffer through @p allocator. */
    explicit compaction_output(allocator_type_ const& allocator) noexcept
        : cells(typename std::allocator_traits<allocator_type_>::template rebind_alloc<key_slot_type_>(allocator)),
          sorted_runs(typename std::allocator_traits<allocator_type_>::template rebind_alloc<compacted_sorted_run_t>(
              allocator)),
          migrations(typename std::allocator_traits<allocator_type_>::template rebind_alloc<stored_value_migration_t>(
              allocator)),
          stamps(typename std::allocator_traits<allocator_type_>::template rebind_alloc<std::uint64_t>(allocator)) {}
};

/** A key slot with the index of the sorted run it came from and its stamp, the unit every merge moves. */
template <typename key_slot_type_>
struct located_key_slot {

    /** The key slot as its sorted run held it. */
    key_slot_type_ key_slot;

    /** Index of its sorted run, counting the upper LSM level first and the lower LSM level after it. */
    std::uint32_t sorted_run = 0;

    /** The stamp of the commit that wrote it. */
    std::uint64_t stamp = 0;
};

#pragma endregion Vocabulary

#pragma region Rules

/** Orders located key slots by stored key, and equal stored keys newest first, so the one that survives leads. */
struct survivor_first_t {

    /** Sorted runs of the upper LSM level: a newer upper sorted run shadows older ones, and any upper sorted run every
     *  lower one. */
    std::uint32_t upper_sorted_runs_count = 0;

    /** Whether @p first orders before @p second. */
    template <typename key_slot_type_>
    constexpr bool operator()(located_key_slot<key_slot_type_> const& first,
                              located_key_slot<key_slot_type_> const& second) const noexcept {
        if (first.key_slot.stored_key < second.key_slot.stored_key) return true;
        if (second.key_slot.stored_key < first.key_slot.stored_key) return false;
        // Versions of one key inside one run keep that run's newest-first order, whatever the sort's stability.
        if (first.sorted_run == second.sorted_run) return first.stamp > second.stamp;
        bool const both_upper = (first.sorted_run < upper_sorted_runs_count) &
                                (second.sorted_run < upper_sorted_runs_count);
        return both_upper ? first.sorted_run > second.sorted_run : first.sorted_run < second.sorted_run;
    }
};

/** How many of the readers @p stamps pins read at or above @p stamp, which names the interval between two
 *  pinned stamps that @p stamp falls into. */
constexpr std::uint64_t reader_interval(compaction_stamps_t const& stamps, std::uint64_t stamp) noexcept {
    std::uint64_t low = 0;
    std::uint64_t high = stamps.pinned_count;
    while (low < high) {
        std::uint64_t const middle = low + (high - low) / 2;
        if (stamps.pinned[middle] < stamp) low = middle + 1;
        else high = middle;
    }
    return stamps.pinned_count - low;
}

/**
 *  @brief Whether a version stamped @p current, following one stamped @p previous of the same key in newest-first
 *      order, is still read by someone.
 *
 *  A version is read by the readers pinned at or above it and below the version before it, so it survives when
 *  the two fall in different reader intervals. Above the durable horizon every version survives, since a mount may
 *  drop any version above whatever horizon the catalog it finds promises, and needs the one beneath.
 */
constexpr bool keeps_older_version(compaction_stamps_t const& stamps, std::uint64_t previous,
                                   std::uint64_t current) noexcept {
    if (previous > stamps.ceiling || previous > stamps.horizon || current > stamps.horizon) return true;
    return reader_interval(stamps, current) != reader_interval(stamps, previous);
}

/**
 *  @brief Whether @p current survives a merge: some reader still reads it, and it is no tombstone where nothing
 *      older survives below it to hide.
 *
 *  With no reader pinned and both bounds at their widest, this is the newest version of every key, tombstones
 *  dropping where the merge writes the bottom level.
 */
template <typename key_slot_type_>
constexpr bool survives(located_key_slot<key_slot_type_> const* previous,
                        located_key_slot<key_slot_type_> const& current, compaction_kind_t kind,
                        compaction_stamps_t const& stamps) noexcept {
    assert((previous == nullptr || !(current.key_slot.stored_key < previous->key_slot.stored_key)) &&
           "survivors are flagged over a merge sorted by stored key");
    if (current.stamp > stamps.ceiling) return false;
    if (previous != nullptr && previous->key_slot.stored_key == current.key_slot.stored_key &&
        !keeps_older_version(stamps, previous->stamp, current.stamp))
        return false;
    bool const drops_tombstones = (kind == compaction_kind_t::overlapping_into_bottom_k) |
                                  (kind == compaction_kind_t::sorted_into_bottom_k);
    // Only in the oldest interval does every older version of the key fall in the same one and drop with it.
    bool const oldest = (current.stamp <= stamps.horizon) &
                        (reader_interval(stamps, current.stamp) == stamps.pinned_count);
    return !(drops_tombstones && current.key_slot.is_tombstone() && oldest);
}

/** Bytes of the stored value @p key_slot locates in a stored values section, zero for empty, tombstoned and inline
 *  stored values. */
template <typename key_slot_type_>
constexpr std::uint64_t stored_value_bytes(key_slot_type_ const& key_slot) noexcept {
    if constexpr (locates_stored_values_concept<key_slot_type_>)
        return key_slot.is_stored_value_empty() ? 0 : key_slot.size;
    else return 0;
}

/** Offset of the stored values section in a sorted run of @p key_slots_count key slots. */
template <typename key_slot_type_>
constexpr std::uint64_t stored_values_offset(sorted_run_bounds_t const& bounds,
                                             std::uint64_t key_slots_count) noexcept {
    std::uint64_t const cells_bytes = round_up_to_multiple(key_slots_count, bounds.branching) * sizeof(key_slot_type_);
    return bounds.header_bytes + round_up_to_multiple(cells_bytes, bounds.alignment_bytes);
}

/** Bytes of a sorted run file of @p key_slots_count key slots locating @p stored_values_bytes bytes of stored
 *  values. */
template <typename key_slot_type_>
constexpr std::uint64_t sorted_run_file_bytes(sorted_run_bounds_t const& bounds, std::uint64_t key_slots_count,
                                              std::uint64_t stored_values_bytes) noexcept {
    std::uint64_t const key_slots_end = stored_values_offset<key_slot_type_>(bounds, key_slots_count);
    if constexpr (locates_stored_values_concept<key_slot_type_>)
        return key_slots_end + round_up_to_multiple(stored_values_bytes, bounds.alignment_bytes);
    else return key_slots_end;
}

/** Whether @p bounds admit sorted runs: a key slot per sorted run, rows of two cells or more, and an alignment. */
constexpr bool admits_sorted_runs(sorted_run_bounds_t const& bounds) noexcept {
    return (bounds.key_slots_capacity != 0) & (bounds.branching >= 2) & (bounds.alignment_bytes != 0);
}

/** The key slot padding cells hold: the greatest stored key, with everything else as a key slot starts. */
template <typename key_slot_type_>
constexpr key_slot_type_ padding_key_slot() noexcept {
    key_slot_type_ padding {};
    padding.stored_key = limit<typename key_slot_type_::stored_key_t>::max();
    return padding;
}

/** Copies @p cell of a sorted run and its stamp out of @p stamps, null reading as zero, into its sorted position
 *  among @p records, skipping padding. */
template <typename key_slot_type_>
constexpr void extract_cell(key_slot_type_ const* cells, sorted_run_index_layout_t const& layout, std::uint64_t cell,
                            std::uint32_t sorted_run, std::uint64_t const* stamps,
                            located_key_slot<key_slot_type_>* records) noexcept {
    std::uint64_t const sorted = layout.cell_to_sorted(cell);
    if (sorted < layout.key_slots_count)
        records[sorted] = located_key_slot<key_slot_type_> {cells[cell], sorted_run,
                                                            stamps != nullptr ? stamps[sorted] : std::uint64_t {0}};
}

/**
 *  @brief Writes a surviving key slot into its cell of a new sorted run, and plans where its stored value comes from.
 *
 *  @param[in] source The survivor with the index of its old sorted run.
 *  @param[in] layout Layout of the new sorted run's sorted run index.
 *  @param[in] position Position of the survivor in the new sorted run's sorted order.
 *  @param[in] section_offset Offset of the new sorted run's stored values section.
 *  @param[in] preceding_bytes Stored value bytes the survivors before it in the new sorted run locate.
 *  @param[in] floor The stamp the survivor's stamp is raised to at least.
 *  @param[out] cells The new sorted run's cells.
 *  @param[out] migration The survivor's migration, written only for key slots that locate stored values.
 *  @param[out] stamp The survivor's stamp, raised to @p floor.
 */
template <typename key_slot_type_>
constexpr void place_key_slot(located_key_slot<key_slot_type_> const& source, sorted_run_index_layout_t const& layout,
                              std::uint64_t position, std::uint64_t section_offset, std::uint64_t preceding_bytes,
                              std::uint64_t floor, key_slot_type_* cells, stored_value_migration_t* migration,
                              std::uint64_t* stamp) noexcept {
    *stamp = source.stamp > floor ? source.stamp : floor;
    key_slot_type_ key_slot = source.key_slot;
    if constexpr (locates_stored_values_concept<key_slot_type_>) {
        migration->size = static_cast<std::uint32_t>(stored_value_bytes(key_slot));
        migration->old_location = static_cast<std::uint32_t>(key_slot.location);
        migration->old_sorted_run = source.sorted_run;
        if (!key_slot.is_stored_value_empty())
            key_slot.location = static_cast<decltype(key_slot.location)>(section_offset + preceding_bytes);
    }
    cells[layout.sorted_to_cell(position)] = key_slot;
}

/**
 *  @brief Cuts @p key_slots_count sorted survivors into sorted runs, filling each as far as @p bounds allow.
 *
 *  A sorted run takes the most key slots that fit its capacity and file limit, and always at least one. The
 *  versions of one key never straddle two runs, since a sorted level answers a key from the one run whose bounds
 *  hold it: a cut moves back to where the key begins, or past where it ends when it began the run.
 *
 *  @param[in] read_prefix Called as @c read_prefix(index, bytes) to write the stored value bytes of the first @c index
 *      survivors.
 *  @param[in] shares_key Called as @c shares_key(index, shared) to say whether survivor @c index carries the key
 *      survivor @c index - 1 does.
 *  @param[out] sorted runs The planned sorted runs, replacing what it held.
 */
template <typename key_slot_type_, typename prefix_reader_type_, typename key_sharing_type_, typename sorted_runs_type_>
st::status_t plan_sorted_runs(std::uint64_t key_slots_count, sorted_run_bounds_t const& bounds,
                              prefix_reader_type_&& read_prefix, key_sharing_type_&& shares_key,
                              sorted_runs_type_& sorted_runs) noexcept {
    using st::status_t;
    sorted_runs.clear();

    std::uint64_t first = 0;
    std::uint64_t first_bytes = 0;
    while (first != key_slots_count) {
        std::uint64_t const remaining = key_slots_count - first;
        std::uint64_t taken = remaining < bounds.key_slots_capacity ? remaining : bounds.key_slots_capacity;
        std::uint64_t taken_bytes = 0;
        status_t status = read_prefix(first + taken, taken_bytes);
        if (status != status_t::success_k) return status;

        if (sorted_run_file_bytes<key_slot_type_>(bounds, taken, taken_bytes - first_bytes) > bounds.file_bytes_limit) {
            std::uint64_t fitting = 1;
            std::uint64_t low = 2;
            std::uint64_t high = taken - 1;
            while (low <= high) {
                std::uint64_t const middle = low + (high - low) / 2;
                std::uint64_t middle_bytes = 0;
                status = read_prefix(first + middle, middle_bytes);
                if (status != status_t::success_k) return status;
                if (sorted_run_file_bytes<key_slot_type_>(bounds, middle, middle_bytes - first_bytes) <=
                    bounds.file_bytes_limit) {
                    fitting = middle;
                    low = middle + 1;
                }
                else high = middle - 1;
            }
            taken = fitting;
            status = read_prefix(first + taken, taken_bytes);
            if (status != status_t::success_k) return status;
        }

        bool shared = false;
        if (first + taken != key_slots_count) {
            status = shares_key(first + taken, shared);
            if (status != status_t::success_k) return status;
        }
        if (shared) {
            std::uint64_t cut = first + taken;
            while (shared && cut != first + 1) {
                status = shares_key(--cut, shared);
                if (status != status_t::success_k) return status;
            }
            // Every survivor of the run so far carries one key, so the run takes that key whole however long it is.
            if (shared)
                for (cut = first + taken + 1; cut != key_slots_count; ++cut) {
                    status = shares_key(cut, shared);
                    if (status != status_t::success_k) return status;
                    if (!shared) break;
                }
            taken = cut - first;
            status = read_prefix(first + taken, taken_bytes);
            if (status != status_t::success_k) return status;
        }

        status = sorted_runs.push_back(compacted_sorted_run_t {taken, taken_bytes - first_bytes});
        if (status != status_t::success_k) return status;
        first += taken;
        first_bytes = taken_bytes;
    }
    return status_t::success_k;
}

#pragma endregion Rules

#pragma region Host

/** Merges sorted ranges @c [begin,middle) and @c [middle,end) of @p source into the same range of @p target. */
template <typename record_type_, typename order_type_>
constexpr void merge_ranges(record_type_ const* source, std::uint64_t begin, std::uint64_t middle, std::uint64_t end,
                            record_type_* target, order_type_ const& order) noexcept {
    std::uint64_t left = begin;
    std::uint64_t right = middle;
    std::uint64_t output = begin;
    while ((left != middle) & (right != end))
        target[output++] = order(source[right], source[left]) ? source[right++] : source[left++];
    while (left != middle) target[output++] = source[left++];
    while (right != end) target[output++] = source[right++];
}

/** Compacts LSM levels on the calling thread. */
struct cpu_compactor_t {

    /**
     *  @brief Merges @p upper into @p lower as @p kind directs, writing the new sorted runs into @p output.
     *
     *  @param[in] kind Whether the upper LSM level overlaps, and whether tombstones drop.
     *  @param[in] upper The upper LSM level's sorted runs, oldest first when they overlap and ascending otherwise.
     *  @param[in] lower The lower LSM level's sorted runs the upper LSM level overlaps, ascending.
     *  @param[in] bounds Bounds every new sorted run fits.
     *  @param[in] stamps Which older versions survive, which versions drop, and the floor every stamp is raised to.
     *  @param[out] output The new sorted runs, their cells, their stamps and their stored values' migrations.
     *  @retval invalid_argument_k The bounds admit no sorted run, or the LSM levels hold more than 2^32 sorted runs.
     *  @retval out_of_memory_heap_k A buffer could not grow.
     */
    template <typename key_slot_type_, typename allocator_type_>
        requires compactable_key_slot_concept<key_slot_type_>
    st::status_t compact(compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_type_> const> upper,
                         std::span<sorted_run_index_view<key_slot_type_> const> lower,
                         sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps,
                         compaction_output<key_slot_type_, allocator_type_>& output) const noexcept {
        using st::basic_vector;
        using st::status_t;
        using record_t = located_key_slot<key_slot_type_>;

        if (!admits_sorted_runs(bounds)) return status_t::invalid_argument_k;
        std::uint64_t const sorted_runs_count = upper.size() + lower.size();
        if (sorted_runs_count > std::numeric_limits<std::uint32_t>::max()) return status_t::invalid_argument_k;

        std::uint64_t upper_key_slots = 0;
        for (std::size_t index = 0; index != upper.size(); ++index) upper_key_slots += upper[index].key_slots_count;
        std::uint64_t merged_count = upper_key_slots;
        for (std::size_t index = 0; index != lower.size(); ++index) merged_count += lower[index].key_slots_count;

        basic_vector<record_t> records;
        basic_vector<record_t> scratch;
        status_t status = records.resize(merged_count);
        if (status != status_t::success_k) return status;
        status = scratch.resize(merged_count);
        if (status != status_t::success_k) return status;

        // Every sorted run's cells land at their sorted positions, in one run per sorted run
        basic_vector<std::uint64_t> run_starts;
        status = run_starts.resize(upper.size() + 1);
        if (status != status_t::success_k) return status;
        std::uint64_t run_offset = 0;
        for (std::uint64_t sorted_run = 0; sorted_run != sorted_runs_count; ++sorted_run) {
            sorted_run_index_view<key_slot_type_> const& view = sorted_run < upper.size()
                                                                    ? upper[sorted_run]
                                                                    : lower[sorted_run - upper.size()];
            if (sorted_run <= upper.size()) run_starts[sorted_run] = run_offset;
            sorted_run_index_layout_t const layout(view.key_slots_count, bounds.branching);
            for (std::uint64_t cell = 0; cell != layout.cells_count(); ++cell)
                extract_cell(view.cells, layout, cell, static_cast<std::uint32_t>(sorted_run), nullptr,
                             records.data() + run_offset);
            if (view.stamps != nullptr)
                for (std::uint64_t sorted = 0; sorted != view.key_slots_count; ++sorted)
                    records[run_offset + sorted].stamp = (*view.stamps)[static_cast<std::size_t>(sorted)];
            run_offset += view.key_slots_count;
        }
        run_starts[upper.size()] = upper_key_slots;

        // Overlapping runs merge pairwise, so the newest key slot of every stored key leads its run
        survivor_first_t const order {static_cast<std::uint32_t>(upper.size())};
        bool const overlapping = (kind == compaction_kind_t::overlapping_into_intermediate_k) |
                                 (kind == compaction_kind_t::overlapping_into_bottom_k);
        if (overlapping) {
            record_t* source = records.data();
            record_t* target = scratch.data();
            std::uint64_t runs = upper.size();
            while (runs > 1) {
                std::uint64_t merged_runs = 0;
                for (std::uint64_t run = 0; run < runs; run += 2) {
                    std::uint64_t const middle = run_starts[run + 1 < runs ? run + 1 : runs];
                    std::uint64_t const end = run_starts[run + 2 < runs ? run + 2 : runs];
                    merge_ranges(source, run_starts[run], middle, end, target, order);
                    run_starts[merged_runs++] = run_starts[run];
                }
                run_starts[merged_runs] = upper_key_slots;
                runs = merged_runs;
                std::swap(source, target);
            }
            if (source != records.data())
                for (std::uint64_t index = 0; index != upper_key_slots; ++index) records[index] = scratch[index];
        }

        // One merge path joins the upper and lower runs, and the survivors move back
        merge_ranges(records.data(), 0, upper_key_slots, merged_count, scratch.data(), order);
        std::uint64_t survivors = 0;
        for (std::uint64_t index = 0; index != merged_count; ++index) {
            record_t const* previous = index != 0 ? scratch.data() + index - 1 : nullptr;
            if (survives(previous, scratch[index], kind, stamps)) records[survivors++] = scratch[index];
        }

        basic_vector<std::uint64_t> prefix;
        if constexpr (locates_stored_values_concept<key_slot_type_>) {
            status = prefix.resize(survivors + 1);
            if (status != status_t::success_k) return status;
            prefix[0] = 0;
            for (std::uint64_t index = 0; index != survivors; ++index)
                prefix[index + 1] = prefix[index] + stored_value_bytes(records[index].key_slot);
        }
        auto const read_prefix = [&prefix](std::uint64_t index, std::uint64_t& bytes) noexcept {
            bytes = prefix.size() != 0 ? prefix[index] : 0;
            return status_t::success_k;
        };
        auto const shares_key = [&records](std::uint64_t index, bool& shared) noexcept {
            shared = records[index - 1].key_slot.stored_key == records[index].key_slot.stored_key;
            return status_t::success_k;
        };
        status = plan_sorted_runs<key_slot_type_>(survivors, bounds, read_prefix, shares_key, output.sorted_runs);
        if (status != status_t::success_k) return status;

        std::uint64_t cells_count = 0;
        for (std::size_t index = 0; index != output.sorted_runs.size(); ++index)
            cells_count += round_up_to_multiple(output.sorted_runs[index].key_slots_count, bounds.branching);
        status = output.cells.resize(cells_count);
        if (status != status_t::success_k) return status;
        key_slot_type_ const padding = padding_key_slot<key_slot_type_>();
        for (std::uint64_t cell = 0; cell != cells_count; ++cell) output.cells[cell] = padding;

        status = output.migrations.resize(locates_stored_values_concept<key_slot_type_> ? survivors : 0);
        if (status != status_t::success_k) return status;
        status = output.stamps.resize(survivors);
        if (status != status_t::success_k) return status;

        std::uint64_t first = 0;
        std::uint64_t first_cell = 0;
        for (std::size_t index = 0; index != output.sorted_runs.size(); ++index) {
            std::uint64_t const count = output.sorted_runs[index].key_slots_count;
            sorted_run_index_layout_t const layout(count, bounds.branching);
            std::uint64_t const section_offset = stored_values_offset<key_slot_type_>(bounds, count);
            for (std::uint64_t position = 0; position != count; ++position) {
                std::uint64_t const survivor = first + position;
                std::uint64_t const preceding = prefix.size() != 0 ? prefix[survivor] - prefix[first] : 0;
                stored_value_migration_t* const migration = output.migrations.size() != 0
                                                                ? output.migrations.data() + survivor
                                                                : nullptr;
                place_key_slot(records[survivor], layout, position, section_offset, preceding, stamps.floor,
                               output.cells.data() + first_cell, migration, output.stamps.data() + survivor);
            }
            first += count;
            first_cell += layout.cells_count();
        }
        return status_t::success_k;
    }
};

#pragma endregion Host

#if USTORE_WITH_CUDA

#pragma region Device

using utilities::cuda_count_devices;
using utilities::cuda_stream_t;

/** Compacts LSM levels on one CUDA device, queueing every transfer and kernel on a stream of its own. */
class gpu_compactor_t {
  public:

    /** Binds a compactor to device @p ordinal with a new stream, failing when no such device exists. */
    static st::expected<gpu_compactor_t> make(int ordinal) noexcept;

    /** Does what @c cpu_compactor_t::compact does with identical output, and reports CUDA failures as statuses. */
    template <typename key_slot_type_, typename allocator_type_>
        requires compactable_key_slot_concept<key_slot_type_>
    st::status_t compact(compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_type_> const> upper,
                         std::span<sorted_run_index_view<key_slot_type_> const> lower,
                         sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps,
                         compaction_output<key_slot_type_, allocator_type_>& output) noexcept;

    /** The ordinal of the device the compactor runs on. */
    int device() const noexcept { return stream_.device(); }

  private:
    explicit gpu_compactor_t(cuda_stream_t&& stream) noexcept : stream_(std::move(stream)) {}

    /** The stream every transfer and kernel of the compactor queues on. */
    cuda_stream_t stream_;
};

#pragma endregion Device

#endif

} // namespace unum::ustore::blobs
