/**
 *  @file include/ustore/blobs/nvme_compaction.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Compaction of LSM levels into new sorted runs on the host, and the device compactor every compiler sees.
 *
 *  A compaction reads the sorted run indexes of an upper LSM level and of the lower sorted runs it overlaps, keeps the
 *  newest key slot of every stored key, drops removals when it writes the bottom LSM level, and lays the survivors
 *  into sorted runs that fit their bounds, planning the coalesced ranges every stored value moves in and the value
 *  offsets of every new run. Both compactors share the rules below. The device one composes chains of changes only
 *  through a device fold the caller passes, and folds on the host every chain reaching a write or a base, so without a
 *  device fold a merge of runs holding a change runs on the host. A build with @c USTORE_WITH_CUDA declares the device
 *  compactor for every compiler, its kernels in @c vram_nvidia_compaction.cuh compile under nvcc alone, and
 *  @c compaction.cu instantiates its compaction for the engine's key slots, so a host TU calls it.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::int64_t`, `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <array>       // `std::array`
#include <concepts>    // `std::convertible_to`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::allocator_traits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_trivially_copyable_v`, `std::conditional_t`, `std::type_identity_t`
#include <utility>     // `std::index_sequence`, `std::move`, `std::swap`
#include <variant>     // `std::variant`

#include <smashtable/basic_vector.hpp>           // `st::basic_vector`
#include <smashtable/immutable_packed_array.hpp> // `st::immutable_packed_array`
#include <smashtable/shared.hpp>                 // `st::status_t`, `st::default_allocator`

#include "ustore/blobs/nvme_key_slot.hpp"          // `limit`, `stored_key_concept`, `sorted_run_index_layout_t`
#include "ustore/utilities/folds.hpp"              // `fold_concept`, `fold_delta_pair`, `delta_kind_t`
#include "ustore/utilities/ram_buffers.hpp"        // `local_arena_t`, `arena_allocator`
#include "ustore/utilities/types.hpp"              // `round_up_to_multiple`
#include "ustore/utilities/vram_nvidia_device.hpp" // `cuda_stream_t`

namespace unum::ustore::blobs {

using utilities::delta_view_t;

#pragma region Vocabulary

/** A key slot shape compaction merges: trivially copyable and keyed by a stored key, with the kind of each delta in
 *  its run's kinds column. */
template <typename key_slot_type_>
concept compactable_key_slot_concept = std::is_trivially_copyable_v<key_slot_type_> &&
                                       stored_key_concept<typename key_slot_type_::stored_key_t> &&
                                       requires(key_slot_type_ const& key_slot) {
                                           {
                                               key_slot.stored_key
                                           } -> std::convertible_to<typename key_slot_type_::stored_key_t>;
                                       };

/** A key slot shape recording the size of its inline value, which a shape holding one width for every value lacks. */
template <typename key_slot_type_>
concept sizes_stored_values_concept = requires(key_slot_type_& key_slot) {
    key_slot.size = static_cast<stored_value_size_t>(0);
};

/** A key slot shape whose stored values live in its sorted run's stored values section, found by sorted position. */
template <typename key_slot_type_>
concept locates_stored_values_concept = requires {
    requires is_fixed_or_variable_width(key_slot_type_::value_layout_k);
};

/** A key slot shape whose stored key is the head of a longer key, as a string's 16-byte head is, so its runs carry the
 *  tails the heads cut short, and two key slots carry one key only where their tails match too. Only the host
 *  compactor merges these. */
template <typename key_slot_type_>
concept tailed_key_slot_concept = compactable_key_slot_concept<key_slot_type_> &&
                                  requires { requires key_slot_type_::carries_tails_k; };

/** The tails of the keys a run's heads cut short, in sorted order: key slot @c i's tail is @c bytes from
 *  @c offsets[i] to @c offsets[i + 1], and a run of whole keys carries no offsets at all. */
struct key_tails_column_t {

    /** One offset per key slot into @c bytes and one past the last, or none for a run of whole keys. */
    std::span<std::uint32_t const> offsets;

    /** The tails back to back. */
    std::span<byte_t const> bytes;

    /** The tail of the key at sorted @p position, empty for a run of whole keys. */
    [[nodiscard]] std::string_view at(std::uint64_t position) const noexcept {
        if (offsets.empty()) return {};
        std::size_t const begin = offsets[static_cast<std::size_t>(position)];
        std::size_t const end = offsets[static_cast<std::size_t>(position) + 1];
        return std::string_view {reinterpret_cast<char const*>(bytes.data()) + begin, end - begin};
    }
};

/** Which LSM levels a compaction merges, fixing whether the upper LSM level overlaps and whether remove deltas drop. */
enum class compaction_kind_t : std::uint8_t {

    /** Overlapping L0 sorted runs into an LSM level with LSM levels below it, keeping remove deltas. */
    overlapping_into_intermediate_k,

    /** Overlapping L0 sorted runs into the bottom LSM level, dropping remove deltas. */
    overlapping_into_bottom_k,

    /** A sorted LSM level into the LSM level below it, which has LSM levels below it too, keeping remove deltas. */
    sorted_into_intermediate_k,

    /** A sorted LSM level into the bottom LSM level, or into a new bottom LSM level, dropping remove deltas. */
    sorted_into_bottom_k,
};

/** The stamps of one sorted run's key slots, in sorted order, as the run keeps them packed beside its index. */
using stamps_column_t = st::immutable_packed_array<std::uint64_t>;

/** The @c delta_kind_t of one sorted run's key slots, in sorted order, packed beside its stamps. */
using kinds_column_t = st::immutable_packed_array<std::uint8_t>;

/** The key slots of one sorted run as its sorted run index's cells hold them, and the stamp and kind of every one. */
template <typename key_slot_type_>
struct sorted_run_index_view {

    /** The sorted run index's cells, whole rows including padding. */
    key_slot_type_ const* cells = nullptr;

    /** Key slots in the sorted run index, padding excluded. */
    std::uint64_t key_slots_count = 0;

    /** The stamp of every key slot in sorted order, or null for a run whose every stamp reads as zero. */
    stamps_column_t const* stamps = nullptr;

    /** The kind of every key slot in sorted order, or null for a run whose every delta is a write. */
    kinds_column_t const* kinds = nullptr;

    /** The tails of the keys the key slots' heads cut short, none for a run of whole keys. */
    key_tails_column_t tails;

    /** Where each key slot's value starts inside the run's values section and one past the last, in sorted order,
     *  for a variable-width run alone. */
    std::span<stored_value_offset_t const> value_offsets;
};

/** Whether @p kinds may hold a change, which only a fold's merge reads; a column packs from its least kind to its
 *  greatest, so the widest kind it can hold is exactly that. */
inline bool holds_changes(kinds_column_t const* kinds) noexcept {
    if (kinds == nullptr || kinds->empty()) return false;
    std::uint64_t const widest = std::uint64_t {kinds->base()} + ((std::uint64_t {1} << kinds->bits()) - 1);
    return widest >= static_cast<std::uint64_t>(delta_kind_t::change_k);
}

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

    /** The newest stamp a schema can promise durable: every version above it survives, as a mount may drop it. */
    std::uint64_t horizon = std::numeric_limits<std::uint64_t>::max();

    /** The newest stamp a merge keeps at all, which a mount sets to drop what no durable schema covers. */
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

    /** Bytes every value of a fixed-width layout takes in its runs, removals included, and zero for any other. */
    std::uint64_t value_stride = 0;
};

/** One sorted run a compaction produced. */
struct compacted_sorted_run_t {

    /** Key slots in the sorted run, padding cells excluded. */
    std::uint64_t key_slots_count = 0;

    /** Bytes of the stored values its key slots locate, before alignment. */
    std::uint64_t stored_values_bytes = 0;

    /** Value copy ranges filling its stored values section, which follow those of the sorted runs before it. */
    std::uint64_t value_copy_ranges_count = 0;
};

/** One range of stored value bytes a new sorted run copies, coalesced from the values of neighbouring survivors that
 *  sit back to back in one source. */
struct value_copy_range_t {

    /** Index of the source run, counting the upper LSM level first and the lower LSM level after it, where the index
     *  past every source run names the compaction's folded bytes. */
    std::uint32_t source_run = 0;

    /** Where the range starts inside the source run's values section, or inside the folded bytes. */
    std::uint32_t source_begin = 0;

    /** Bytes the range copies, never zero. */
    std::uint32_t bytes = 0;

    /** Where the range lands inside the new sorted run's values section. */
    std::uint32_t target_begin = 0;
};

static_assert(sizeof(value_copy_range_t) == 16, "A value copy range is four words");

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

    /** The value copy ranges of every sorted run in turn, empty for stored values held inline. */
    rebound_vector<value_copy_range_t, allocator_type_> value_copy_ranges;

    /** One offset per key slot into the stored values across the sorted runs and one past the last, for a
     *  variable-width layout alone, which every new run rebases from its first key slot. */
    rebound_vector<std::uint64_t, allocator_type_> value_offsets;

    /** One stamp per key slot in sorted order across the sorted runs, each at or above the merge's floor. */
    rebound_vector<std::uint64_t, allocator_type_> stamps;

    /** One @c delta_kind_t per key slot in sorted order across the sorted runs. */
    rebound_vector<std::uint8_t, allocator_type_> kinds;

    /** The values a fold wrote, back to back in survivor order, which copy ranges naming the index past every source
     *  run read. */
    rebound_vector<byte_t, allocator_type_> folded;

    /** One offset per key slot into @c tails across the sorted runs and one past the last, none for whole keys. */
    rebound_vector<std::uint64_t, allocator_type_> tail_offsets;

    /** The tails of the key slots back to back in sorted order across the sorted runs. */
    rebound_vector<byte_t, allocator_type_> tails;

    compaction_output() noexcept = default;

    /** Allocates every buffer through @p allocator. */
    explicit compaction_output(allocator_type_ const& allocator) noexcept
        : cells(typename std::allocator_traits<allocator_type_>::template rebind_alloc<key_slot_type_>(allocator)),
          sorted_runs(typename std::allocator_traits<allocator_type_>::template rebind_alloc<compacted_sorted_run_t>(
              allocator)),
          value_copy_ranges(
              typename std::allocator_traits<allocator_type_>::template rebind_alloc<value_copy_range_t>(allocator)),
          value_offsets(
              typename std::allocator_traits<allocator_type_>::template rebind_alloc<std::uint64_t>(allocator)),
          stamps(typename std::allocator_traits<allocator_type_>::template rebind_alloc<std::uint64_t>(allocator)),
          kinds(typename std::allocator_traits<allocator_type_>::template rebind_alloc<std::uint8_t>(allocator)),
          folded(typename std::allocator_traits<allocator_type_>::template rebind_alloc<byte_t>(allocator)),
          tail_offsets(
              typename std::allocator_traits<allocator_type_>::template rebind_alloc<std::uint64_t>(allocator)),
          tails(typename std::allocator_traits<allocator_type_>::template rebind_alloc<byte_t>(allocator)) {}
};

/** A key slot with the index of the sorted run it came from, its stamp and its kind, the unit every merge moves. */
template <typename key_slot_type_>
struct located_key_slot {

    /** The key slot as its sorted run held it. */
    key_slot_type_ key_slot;

    /** Index of its sorted run, counting the upper LSM level first and the lower LSM level after it. */
    std::uint32_t sorted_run = 0;

    /** The kind of its delta, in the bytes the stamp's alignment leaves. */
    delta_kind_t kind = delta_kind_t::write_k;

    /** The stamp of the commit that wrote it. */
    std::uint64_t stamp = 0;

    /** Its sorted position in its sorted run, by which its value is read. */
    std::uint32_t position = 0;

    /** Bytes its value takes in its sorted run's values section, or in the folded bytes, zero for values inline. */
    std::uint32_t size = 0;
};

static_assert(sizeof(located_key_slot<key_slot<st::key128_t, value_layout_t::variable_width_k>>) == 40,
              "A compaction record of a 16-byte cell is 40 bytes");

/** A located key slot of a tailed shape with the tail of its key, viewing its sorted run's tails, the unit a merge of
 *  such runs moves. */
template <typename key_slot_type_>
struct located_tailed_key_slot {

    /** The key slot with its run, kind and stamp. */
    located_key_slot<key_slot_type_> located;

    /** The bytes of its key past what the head holds. */
    std::string_view tail;
};

/** The located key slot @p record carries, which is itself for a whole key. */
template <typename key_slot_type_>
constexpr located_key_slot<key_slot_type_>& located_of(located_key_slot<key_slot_type_>& record) noexcept {
    return record;
}

/** The located key slot @p record carries beside its tail. */
template <typename key_slot_type_>
constexpr located_key_slot<key_slot_type_>& located_of(located_tailed_key_slot<key_slot_type_>& record) noexcept {
    return record.located;
}

template <typename key_slot_type_>
constexpr located_key_slot<key_slot_type_> const& located_of(located_key_slot<key_slot_type_> const& record) noexcept {
    return record;
}

template <typename key_slot_type_>
constexpr located_key_slot<key_slot_type_> const& located_of(
    located_tailed_key_slot<key_slot_type_> const& record) noexcept {
    return record.located;
}

/** Whether @p first and @p second carry one key, which their stored keys decide. */
template <typename key_slot_type_>
constexpr bool same_key(located_key_slot<key_slot_type_> const& first,
                        located_key_slot<key_slot_type_> const& second) noexcept {
    return first.key_slot.stored_key == second.key_slot.stored_key;
}

/** Whether @p first and @p second carry one key, which their heads and then their tails decide. */
template <typename key_slot_type_>
constexpr bool same_key(located_tailed_key_slot<key_slot_type_> const& first,
                        located_tailed_key_slot<key_slot_type_> const& second) noexcept {
    return first.located.key_slot.stored_key == second.located.key_slot.stored_key && first.tail == second.tail;
}

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

    /** Whether @p first orders before @p second, comparing tails where the heads tie. */
    template <typename key_slot_type_>
    constexpr bool operator()(located_tailed_key_slot<key_slot_type_> const& first,
                              located_tailed_key_slot<key_slot_type_> const& second) const noexcept {
        if (first.located.key_slot.stored_key == second.located.key_slot.stored_key && first.tail != second.tail)
            return first.tail < second.tail;
        return (*this)(first.located, second.located);
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
 *  drop any version above whatever horizon the schema it finds promises, and needs the one beneath.
 */
constexpr bool keeps_older_version(compaction_stamps_t const& stamps, std::uint64_t previous,
                                   std::uint64_t current) noexcept {
    if (previous > stamps.ceiling || previous > stamps.horizon || current > stamps.horizon) return true;
    return reader_interval(stamps, current) != reader_interval(stamps, previous);
}

/** Whether a merge of @p kind writes the bottom level, where nothing older is left for a removal to hide. */
constexpr bool writes_bottom(compaction_kind_t kind) noexcept {
    return (kind == compaction_kind_t::overlapping_into_bottom_k) | (kind == compaction_kind_t::sorted_into_bottom_k);
}

/**
 *  @brief Whether @p current survives a merge: some reader still reads it, and it is no removal where nothing
 *      older survives below it to hide.
 *
 *  With no reader pinned and both bounds at their widest, this is the newest version of every key, removals
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
    // Only in the oldest interval does every older version of the key fall in the same one and drop with it.
    bool const oldest = (current.stamp <= stamps.horizon) &
                        (reader_interval(stamps, current.stamp) == stamps.pinned_count);
    return !(writes_bottom(kind) && current.kind == delta_kind_t::remove_k && oldest);
}

/** As @c survives for whole keys, where a record before @p current that shares its head and not its tail carries
 *  another key, so @p current is that key's newest version. */
template <typename key_slot_type_>
constexpr bool survives(located_tailed_key_slot<key_slot_type_> const* previous,
                        located_tailed_key_slot<key_slot_type_> const& current, compaction_kind_t kind,
                        compaction_stamps_t const& stamps) noexcept {
    bool const shares_key = previous != nullptr && same_key(*previous, current);
    return survives(shares_key ? &previous->located : nullptr, current.located, kind, stamps);
}

/** Bytes the value at sorted @p position takes in its run's values section: through @p value_offsets where the run
 *  carries them, and @p value_stride otherwise, which is zero for values held inline. */
constexpr std::uint32_t stored_value_bytes(stored_value_offset_t const* value_offsets, std::uint64_t value_stride,
                                           std::uint64_t position) noexcept {
    return value_offsets != nullptr ? value_offsets[position + 1] - value_offsets[position]
                                    : static_cast<std::uint32_t>(value_stride);
}

/** Offset of the stored values section in a sorted run of @p key_slots_count key slots, past its value offsets. */
template <typename key_slot_type_>
constexpr std::uint64_t stored_values_offset(sorted_run_bounds_t const& bounds,
                                             std::uint64_t key_slots_count) noexcept {
    std::uint64_t const cells_bytes = round_up_to_multiple(key_slots_count, bounds.branching) * sizeof(key_slot_type_);
    std::uint64_t offsets_bytes = 0;
    if constexpr (locates_stored_values_concept<key_slot_type_>)
        if (is_variable_width(key_slot_type_::value_layout_k))
            offsets_bytes = round_up_to_multiple((key_slots_count + 1) * sizeof(stored_value_offset_t),
                                                 bounds.alignment_bytes);
    return bounds.header_bytes + round_up_to_multiple(cells_bytes, bounds.alignment_bytes) + offsets_bytes;
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

/** Copies @p cell of a sorted run, its stamp out of @p stamps, null reading as zero, its kind out of @p kinds, null
 *  reading as a write, and its value's bytes as @c stored_value_bytes reads them, into its sorted position among
 *  @p records, skipping padding. */
template <typename key_slot_type_>
constexpr void extract_cell(key_slot_type_ const* cells, sorted_run_index_layout_t const& layout, std::uint64_t cell,
                            std::uint32_t sorted_run, std::uint64_t const* stamps, std::uint8_t const* kinds,
                            stored_value_offset_t const* value_offsets, std::uint64_t value_stride,
                            located_key_slot<key_slot_type_>* records) noexcept {
    std::uint64_t const sorted = layout.cell_to_sorted(cell);
    if (sorted < layout.key_slots_count)
        records[sorted] = located_key_slot<key_slot_type_> {
            cells[cell],
            sorted_run,
            kinds != nullptr ? static_cast<delta_kind_t>(kinds[sorted]) : delta_kind_t::write_k,
            stamps != nullptr ? stamps[sorted] : std::uint64_t {0},
            static_cast<std::uint32_t>(sorted),
            stored_value_bytes(value_offsets, value_stride, sorted)};
}

/**
 *  @brief Writes a surviving key slot into its cell of a new sorted run, with its stamp and its kind.
 *
 *  @param[in] source The survivor with the index of its old sorted run.
 *  @param[in] layout Layout of the new sorted run's sorted run index.
 *  @param[in] position Position of the survivor in the new sorted run's sorted order.
 *  @param[in] floor The stamp the survivor's stamp is raised to at least.
 *  @param[out] cells The new sorted run's cells.
 *  @param[out] stamp The survivor's stamp, raised to @p floor.
 *  @param[out] kind The survivor's kind.
 */
template <typename key_slot_type_>
constexpr void place_key_slot(located_key_slot<key_slot_type_> const& source, sorted_run_index_layout_t const& layout,
                              std::uint64_t position, std::uint64_t floor, key_slot_type_* cells, std::uint64_t* stamp,
                              std::uint8_t* kind) noexcept {
    *stamp = source.stamp > floor ? source.stamp : floor;
    *kind = static_cast<std::uint8_t>(source.kind);
    cells[layout.sorted_to_cell(position)] = source.key_slot;
}

/**
 *  @brief Lays the value @p bytes a fold wrote for @p record into @p folded, the bytes of a merge's folded values, or
 *      into its inline key slot, so the copy plan and the new run read it where @p record now names it.
 *  @param[in] value_stride Bytes every value of a fixed-width layout takes, which a folded removal fills with zeros.
 *  @param[in] folded_run The index past every source run, which names the folded bytes.
 */
template <typename key_slot_type_, typename folded_type_>
st::status_t place_folded_value(std::span<byte_t const> bytes, std::uint64_t value_stride, std::uint32_t folded_run,
                                located_key_slot<key_slot_type_>& record, folded_type_& folded) noexcept {
    using st::status_t;
    key_slot_type_& key_slot = record.key_slot;
    if constexpr (locates_stored_values_concept<key_slot_type_>) {
        std::size_t size = bytes.size();
        if constexpr (is_fixed(key_slot_type_::value_layout_k)) {
            if (!bytes.empty() && bytes.size() != value_stride) return status_t::invalid_argument_k;
            size = static_cast<std::size_t>(value_stride);
        }
        std::size_t const location = folded.size();
        if (location + size > std::numeric_limits<std::uint32_t>::max()) return status_t::capacity_exhausted_k;
        if (status_t const grown = folded.resize(location + size); grown != status_t::success_k) return grown;
        if (!bytes.empty()) std::memcpy(folded.data() + location, bytes.data(), bytes.size());
        if (bytes.size() != size) std::memset(folded.data() + location + bytes.size(), 0, size - bytes.size());
        record.sorted_run = folded_run;
        record.position = 0;
        record.size = static_cast<std::uint32_t>(size);
    }
    else {
        if (bytes.size() > sizeof(key_slot.data)) return status_t::capacity_exhausted_k;
        if (!bytes.empty()) std::memcpy(key_slot.data, bytes.data(), bytes.size());
        if constexpr (sizes_stored_values_concept<key_slot_type_>)
            key_slot.size = static_cast<stored_value_size_t>(bytes.size());
        else if (bytes.size() != key_slot_type_::size) return status_t::invalid_argument_k;
    }
    return status_t::success_k;
}

/**
 *  @brief Coalesces where the values of @p survivors come from into the copy ranges of each of @p sorted_runs, and
 *      counts each run's ranges into it.
 *  @param[in] survivors The survivors in sorted order across the new runs; one naming the run past every source has
 *      its value next in the folded bytes, which hold the folded values in survivor order.
 *  @param[in] upper The upper LSM level's views, whose value offsets locate the values of a variable width.
 *  @param[in] lower The lower LSM level's views.
 *  @param[in] value_stride Bytes every value of a fixed-width layout takes, which locates its values by position.
 *  @param[out] ranges The copy ranges of every run in turn, replacing what it held.
 */
template <typename key_slot_type_, typename record_type_, typename sorted_runs_type_, typename ranges_type_>
st::status_t plan_value_copies(record_type_ const* survivors,
                               std::span<sorted_run_index_view<key_slot_type_> const> upper,
                               std::span<sorted_run_index_view<key_slot_type_> const> lower, std::uint64_t value_stride,
                               sorted_runs_type_& sorted_runs, ranges_type_& ranges) noexcept {
    using st::status_t;
    ranges.clear();
    std::uint64_t const sources_count = upper.size() + lower.size();
    std::uint64_t folded_begin = 0;
    std::uint64_t first = 0;
    for (std::size_t run = 0; run != sorted_runs.size(); ++run) {
        std::size_t const first_range = ranges.size();
        std::uint64_t const end = first + sorted_runs[run].key_slots_count;
        std::uint64_t target = 0;
        for (std::uint64_t index = first; index != end; ++index) {
            located_key_slot<key_slot_type_> const& record = located_of(survivors[index]);
            if (record.size == 0) continue;
            std::uint64_t source_begin = folded_begin;
            if (record.sorted_run == sources_count) folded_begin += record.size;
            else {
                sorted_run_index_view<key_slot_type_> const& view = record.sorted_run < upper.size()
                                                                        ? upper[record.sorted_run]
                                                                        : lower[record.sorted_run - upper.size()];
                source_begin = !view.value_offsets.empty() ? view.value_offsets[record.position]
                                                           : record.position * value_stride;
            }
            if (ranges.size() != first_range) {
                value_copy_range_t& last = ranges[ranges.size() - 1];
                std::uint64_t const extended = std::uint64_t {last.bytes} + record.size;
                if (last.source_run == record.sorted_run &&
                    last.source_begin + std::uint64_t {last.bytes} == source_begin &&
                    extended <= std::numeric_limits<std::uint32_t>::max()) {
                    last.bytes = static_cast<std::uint32_t>(extended);
                    target += record.size;
                    continue;
                }
            }
            if (source_begin > std::numeric_limits<std::uint32_t>::max() ||
                target > std::numeric_limits<std::uint32_t>::max())
                return status_t::capacity_exhausted_k;
            status_t const pushed = ranges.push_back(
                value_copy_range_t {record.sorted_run, static_cast<std::uint32_t>(source_begin), record.size,
                                    static_cast<std::uint32_t>(target)});
            if (pushed != status_t::success_k) return pushed;
            target += record.size;
        }
        sorted_runs[run].value_copy_ranges_count = ranges.size() - first_range;
        first = end;
    }
    return status_t::success_k;
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

/** Whether every view of @p views can tell its values' bytes as @p bounds lay them: through value offsets for each run
 *  holding key slots of a variable width, and by a stride the bounds name for a fixed width. */
template <typename key_slot_type_>
constexpr bool views_locate_values(std::span<sorted_run_index_view<key_slot_type_> const> views,
                                   sorted_run_bounds_t const& bounds) noexcept {
    if constexpr (!locates_stored_values_concept<key_slot_type_>) return bounds.value_stride == 0;
    else if constexpr (is_fixed(key_slot_type_::value_layout_k)) return bounds.value_stride != 0;
    else {
        if (bounds.value_stride != 0) return false;
        for (sorted_run_index_view<key_slot_type_> const& view : views)
            if (view.key_slots_count != 0 && view.value_offsets.size() != view.key_slots_count + 1) return false;
        return true;
    }
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

/** The compactor's status for a fold that refused a chain, which only running out of memory is not a refusal. */
inline st::status_t fold_refusal(utilities::status_t const& refused) noexcept {
    return refused == utilities::status_code_t::out_of_memory_k ? st::status_t::out_of_memory_heap_k
                                                                : st::status_t::invalid_argument_k;
}

/** The versions a merge folds into a surviving change, and whether its chain applies onto nothing. */
struct folded_versions_t {

    /** The first merged record past the versions that fold into the change. */
    std::uint64_t end = 0;

    /** Whether the change is the oldest survivor of its key where the merge writes the bottom level. */
    bool applies = false;
};

/**
 *  @brief The versions that fold into the surviving change at @p head of @p merged: the dropped versions of its key
 *      behind it, up to the first a reader still tells apart from the version before it.
 *  @param[in] merged The merged records, in key order with the versions of a key newest first.
 *  @param[in] kept Whether each merged record survives.
 */
template <typename record_type_>
constexpr folded_versions_t folded_versions(compaction_kind_t kind, compaction_stamps_t const& stamps,
                                            record_type_ const* merged, std::uint8_t const* kept,
                                            std::uint64_t merged_count, std::uint64_t head) noexcept {
    std::uint64_t end = head + 1;
    // A removal dropped at the bottom hides every older version from the change above it.
    while (end != merged_count && !kept[end] && same_key(merged[end], merged[head]) &&
           !keeps_older_version(stamps, located_of(merged[end - 1]).stamp, located_of(merged[end]).stamp))
        ++end;
    std::uint64_t next = end;
    while (next != merged_count && !kept[next]) ++next;
    bool const applies = writes_bottom(kind) && (next == merged_count || !same_key(merged[next], merged[head]));
    return folded_versions_t {end, applies};
}

/**
 *  @brief Folds the versions @p versions names into the surviving change at @p head of @p merged, newest first, until
 *      one closes it, and at the bottom applies a change left open onto nothing, as every compactor folds on the host.
 *  @param[in] value_of Called as @c value_of(record, bytes) to view the value of a merged record, answering an
 *      @c st::status_t, with any bytes it reads allocated from @p arena.
 *  @param[out] folded What the change folds to, a write, a removal or a change, its bytes in @p arena or a record.
 */
template <typename record_type_, utilities::fold_concept fold_type_, typename value_of_type_>
st::status_t fold_versions(fold_type_ const& fold, record_type_ const* merged, std::uint64_t head,
                           folded_versions_t const& versions, value_of_type_&& value_of,
                           utilities::arena_allocator<byte_t>& arena, delta_view_t& folded) noexcept {
    folded = delta_view_t {delta_kind_t::change_k, {}};
    if (st::status_t const read = value_of(merged[head], folded.bytes); read != st::status_t::success_k) return read;
    for (std::uint64_t older = head + 1; older != versions.end && folded.kind == delta_kind_t::change_k; ++older) {
        delta_view_t version {located_of(merged[older]).kind, {}};
        if (st::status_t const read = value_of(merged[older], version.bytes); read != st::status_t::success_k)
            return read;
        auto [pair, fold_status] = utilities::fold_delta_pair(fold, folded, version, arena);
        if (fold_status.failed()) return fold_refusal(fold_status);
        folded = pair;
    }
    if (versions.applies && folded.kind == delta_kind_t::change_k) {
        auto [applied, apply_status] = fold.apply({}, std::span<delta_view_t const> {&folded, 1}, arena);
        if (apply_status.failed()) return fold_refusal(apply_status);
        folded = delta_view_t {delta_kind_t::write_k, applied};
    }
    return st::status_t::success_k;
}

/** Compacts LSM levels on the calling thread. */
struct host_compactor_t {

    /**
     *  @brief Merges @p upper into @p lower as @p kind directs, writing the new sorted runs into @p output.
     *
     *  A version no reader tells apart from a change kept above it folds into that change, which keeps its stamp, and
     *  at the bottom level the oldest change of a key folds onto nothing, so a fold's chains shrink with every merge.
     *  A collection of blobs never writes a change, so its merge folds nothing and reads no value. Key slots of a
     *  tailed shape move with their tails, which order and tell apart the keys of one head, and a new sorted run never
     *  splits the key slots of one head, so a sorted level still finds a head in one run.
     *
     *  @param[in] kind Whether the upper LSM level overlaps, and whether removals drop.
     *  @param[in] upper The upper LSM level's sorted runs, oldest first when they overlap and ascending otherwise.
     *  @param[in] lower The lower LSM level's sorted runs the upper LSM level overlaps, ascending.
     *  @param[in] bounds Bounds every new sorted run fits.
     *  @param[in] stamps Which older versions survive, which versions drop, and the floor every stamp is raised to.
     *  @param[in] fold How the collection's changes compose and apply.
     *  @param[in] read_value Called as @c read_value(record, arena, bytes) to view the bytes of a record a fold reads,
     *      answering an @c st::status_t, with any bytes it reads allocated from @c arena.
     *  @param[out] output The new sorted runs, their cells, stamps and kinds, their value offsets and value copy
     *      ranges, and the values a fold wrote.
     *  @retval invalid_argument_k The bounds admit no sorted run, the LSM levels hold more than 2^32 sorted runs, a
     *      variable-width view lacks its value offsets, or the fold refused a chain.
     *  @retval capacity_exhausted_k A folded value outgrows its inline key slot or the 32-bit locations.
     *  @retval out_of_memory_heap_k A buffer could not grow.
     */
    template <typename key_slot_type_, utilities::fold_concept fold_type_, typename value_reader_type_,
              typename allocator_type_>
        requires compactable_key_slot_concept<key_slot_type_>
    st::status_t compact(compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_type_> const> upper,
                         std::span<sorted_run_index_view<key_slot_type_> const> lower,
                         sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps, fold_type_ const& fold,
                         value_reader_type_&& read_value,
                         compaction_output<key_slot_type_, allocator_type_>& output) const noexcept {
        using st::basic_vector;
        using st::status_t;
        constexpr bool tailed_k = tailed_key_slot_concept<key_slot_type_>;
        using record_t =
            std::conditional_t<tailed_k, located_tailed_key_slot<key_slot_type_>, located_key_slot<key_slot_type_>>;

        if (!admits_sorted_runs(bounds)) return status_t::invalid_argument_k;
        std::uint64_t const sorted_runs_count = upper.size() + lower.size();
        if (sorted_runs_count > std::numeric_limits<std::uint32_t>::max()) return status_t::invalid_argument_k;
        if (!views_locate_values(upper, bounds) || !views_locate_values(lower, bounds))
            return status_t::invalid_argument_k;

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
            stored_value_offset_t const* const offsets = !view.value_offsets.empty() ? view.value_offsets.data()
                                                                                     : nullptr;
            for (std::uint64_t cell = 0; cell != layout.cells_count(); ++cell) {
                std::uint64_t const sorted = layout.cell_to_sorted(cell);
                if (sorted < layout.key_slots_count)
                    located_of(records[run_offset + sorted]) = located_key_slot<key_slot_type_> {
                        view.cells[cell],
                        static_cast<std::uint32_t>(sorted_run),
                        delta_kind_t::write_k,
                        0,
                        static_cast<std::uint32_t>(sorted),
                        stored_value_bytes(offsets, bounds.value_stride, sorted)};
            }
            if (view.stamps != nullptr)
                for (std::uint64_t sorted = 0; sorted != view.key_slots_count; ++sorted)
                    located_of(records[run_offset + sorted]).stamp = (*view.stamps)[static_cast<std::size_t>(sorted)];
            if (view.kinds != nullptr)
                for (std::uint64_t sorted = 0; sorted != view.key_slots_count; ++sorted)
                    located_of(records[run_offset + sorted]).kind = static_cast<delta_kind_t>(
                        (*view.kinds)[static_cast<std::size_t>(sorted)]);
            if constexpr (tailed_k)
                for (std::uint64_t sorted = 0; sorted != view.key_slots_count; ++sorted)
                    records[run_offset + sorted].tail = view.tails.at(sorted);
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

        // Survival settles before any fold, so a fold sees every version dropped behind its change
        basic_vector<std::uint8_t> kept;
        status = kept.resize(merged_count);
        if (status != status_t::success_k) return status;
        for (std::uint64_t index = 0; index != merged_count; ++index)
            kept[index] = survives(index != 0 ? scratch.data() + index - 1 : nullptr, scratch[index], kind, stamps);

        // A folded survivor's value, which replaces its own once every survivor is placed
        struct folded_t {
            std::uint64_t survivor = 0;
            std::span<byte_t const> bytes;
        };
        basic_vector<folded_t> folds;
        utilities::local_arena_t arena;
        utilities::arena_allocator<byte_t> arena_bytes = arena.allocator();
        auto const value_of = [&](record_t const& record, std::span<byte_t const>& bytes) noexcept {
            bytes = {};
            if (located_of(record).kind == delta_kind_t::remove_k) return status_t::success_k;
            return read_value(located_of(record), arena_bytes, bytes);
        };
        std::uint64_t survivors = 0;
        for (std::uint64_t index = 0; index != merged_count; ++index) {
            if (!kept[index]) continue;
            record_t& survivor = records[survivors++];
            survivor = scratch[index];
            if (located_of(survivor).kind != delta_kind_t::change_k) continue;
            folded_versions_t const versions = folded_versions(kind, stamps, scratch.data(), kept.data(), merged_count,
                                                               index);
            if (versions.end == index + 1 && !versions.applies) continue;
            delta_view_t folded;
            status = fold_versions(fold, scratch.data(), index, versions, value_of, arena_bytes, folded);
            if (status != status_t::success_k) return status;
            located_of(survivor).kind = folded.kind;
            if (folds.push_back(folded_t {survivors - 1, folded.bytes}) != status_t::success_k)
                return status_t::out_of_memory_heap_k;
        }

        // A folded value replaces its survivor's own: inline in the key slot, or in the compaction's folded bytes
        output.folded.clear();
        for (folded_t const& folded : folds) {
            status = place_folded_value(folded.bytes, bounds.value_stride,
                                        static_cast<std::uint32_t>(sorted_runs_count),
                                        located_of(records[folded.survivor]), output.folded);
            if (status != status_t::success_k) return status;
        }

        basic_vector<std::uint64_t> prefix;
        if constexpr (locates_stored_values_concept<key_slot_type_>) {
            status = prefix.resize(survivors + 1);
            if (status != status_t::success_k) return status;
            prefix[0] = 0;
            for (std::uint64_t index = 0; index != survivors; ++index)
                prefix[index + 1] = prefix[index] + located_of(records[index]).size;
        }
        // Tails take room in a run file beside the values, so the plan bounds both together.
        basic_vector<std::uint64_t> planned;
        if constexpr (tailed_k) {
            status = planned.resize(survivors + 1);
            if (status != status_t::success_k) return status;
            planned[0] = 0;
            for (std::uint64_t index = 0; index != survivors; ++index)
                planned[index + 1] = planned[index] + (prefix.size() != 0 ? prefix[index + 1] - prefix[index] : 0) +
                                     records[index].tail.size();
        }
        basic_vector<std::uint64_t> const& bounded = tailed_k ? planned : prefix;
        auto const read_prefix = [&bounded](std::uint64_t index, std::uint64_t& bytes) noexcept {
            bytes = bounded.size() != 0 ? bounded[index] : 0;
            return status_t::success_k;
        };
        // Runs split between heads alone, so the key slots of one head stay in one run however their tails differ.
        auto const shares_key = [&records](std::uint64_t index, bool& shared) noexcept {
            shared = located_of(records[index - 1]).key_slot.stored_key ==
                     located_of(records[index]).key_slot.stored_key;
            return status_t::success_k;
        };
        status = plan_sorted_runs<key_slot_type_>(survivors, bounds, read_prefix, shares_key, output.sorted_runs);
        if (status != status_t::success_k) return status;
        if constexpr (tailed_k) {
            std::uint64_t planned_first = 0;
            for (std::size_t index = 0; index != output.sorted_runs.size(); ++index) {
                std::uint64_t const end = planned_first + output.sorted_runs[index].key_slots_count;
                output.sorted_runs[index].stored_values_bytes = prefix.size() != 0 ? prefix[end] - prefix[planned_first]
                                                                                   : 0;
                planned_first = end;
            }
        }

        std::uint64_t cells_count = 0;
        for (std::size_t index = 0; index != output.sorted_runs.size(); ++index)
            cells_count += round_up_to_multiple(output.sorted_runs[index].key_slots_count, bounds.branching);
        status = output.cells.resize(cells_count);
        if (status != status_t::success_k) return status;
        key_slot_type_ const padding = padding_key_slot<key_slot_type_>();
        for (std::uint64_t cell = 0; cell != cells_count; ++cell) output.cells[cell] = padding;

        status = output.stamps.resize(survivors);
        if (status != status_t::success_k) return status;
        status = output.kinds.resize(survivors);
        if (status != status_t::success_k) return status;

        std::uint64_t first = 0;
        std::uint64_t first_cell = 0;
        for (std::size_t index = 0; index != output.sorted_runs.size(); ++index) {
            std::uint64_t const count = output.sorted_runs[index].key_slots_count;
            sorted_run_index_layout_t const layout(count, bounds.branching);
            for (std::uint64_t position = 0; position != count; ++position) {
                std::uint64_t const survivor = first + position;
                place_key_slot(located_of(records[survivor]), layout, position, stamps.floor,
                               output.cells.data() + first_cell, output.stamps.data() + survivor,
                               output.kinds.data() + survivor);
            }
            first += count;
            first_cell += layout.cells_count();
        }

        status = plan_value_copies<key_slot_type_>(records.data(), upper, lower, bounds.value_stride,
                                                   output.sorted_runs, output.value_copy_ranges);
        if (status != status_t::success_k) return status;
        output.value_offsets.clear();
        if constexpr (locates_stored_values_concept<key_slot_type_>)
            if (is_variable_width(key_slot_type_::value_layout_k)) {
                status = output.value_offsets.resize(prefix.size());
                if (status != status_t::success_k) return status;
                for (std::size_t index = 0; index != prefix.size(); ++index)
                    output.value_offsets[index] = prefix[index];
            }

        output.tail_offsets.clear();
        output.tails.clear();
        if constexpr (tailed_k) {
            std::uint64_t tails_bytes = 0;
            for (std::uint64_t index = 0; index != survivors; ++index) tails_bytes += records[index].tail.size();
            status = output.tail_offsets.resize(survivors + 1);
            if (status != status_t::success_k) return status;
            status = output.tails.resize(tails_bytes);
            if (status != status_t::success_k) return status;
            std::uint64_t written = 0;
            for (std::uint64_t index = 0; index != survivors; ++index) {
                std::string_view const tail = records[index].tail;
                output.tail_offsets[index] = written;
                if (!tail.empty()) std::memcpy(output.tails.data() + written, tail.data(), tail.size());
                written += tail.size();
            }
            output.tail_offsets[survivors] = written;
        }
        return status_t::success_k;
    }
};

#pragma endregion Host

#pragma region Device

/** The device fold of a collection no device composes the changes of, so a merge of runs holding a change runs on the
 *  host. */
struct no_device_fold_t {};

/** The device compactor a database's merges may run on, defined for builds with @c USTORE_WITH_CUDA. */
class cuda_compaction_lane_t;

#if USTORE_WITH_CUDA

using utilities::cuda_stream_t;

/** A borrowed reader of the values a fold reads, called as @c host_compactor_t calls its own, so one device compaction
 *  compiled per key slot serves a reader of any type. */
template <typename key_slot_type_>
class located_value_reader {
  public:

    /** A reader refusing every read, for a merge that folds nothing. */
    located_value_reader() noexcept = default;

    /** Borrows @p reader, which must outlive every read. */
    template <typename reader_type_>
    located_value_reader(reader_type_ const& reader) noexcept
        : context_(&reader), read_(&read_through_<reader_type_>) {}

    /** Views the bytes of @p record in @p bytes, allocating any it reads from @p arena. */
    st::status_t operator()(located_key_slot<key_slot_type_> const& record, utilities::arena_allocator<byte_t>& arena,
                            std::span<byte_t const>& bytes) const noexcept {
        return read_(context_, record, arena, bytes);
    }

  private:
    using read_t = st::status_t (*)(void const*, located_key_slot<key_slot_type_> const&,
                                    utilities::arena_allocator<byte_t>&, std::span<byte_t const>&) noexcept;

    template <typename reader_type_>
    static st::status_t read_through_(void const* context, located_key_slot<key_slot_type_> const& record,
                                      utilities::arena_allocator<byte_t>& arena,
                                      std::span<byte_t const>& bytes) noexcept {
        return (*static_cast<reader_type_ const*>(context))(record, arena, bytes);
    }

    static st::status_t refuse_(void const*, located_key_slot<key_slot_type_> const&,
                                utilities::arena_allocator<byte_t>&, std::span<byte_t const>&) noexcept {
        return st::status_t::invalid_argument_k;
    }

    /** The borrowed reader. */
    void const* context_ = nullptr;

    /** Calls the borrowed reader as its own type. */
    read_t read_ = &refuse_;
};

/** The one device compactor a database's merges run on, made once at open by whoever opens it: one stream, and device
 *  scratch every compaction reuses, grown to the largest merge it served, so a warm lane allocates nothing. */
class cuda_compaction_lane_t {
  public:

    /** Binds a lane to device @p ordinal with a stream of its own, its scratch never growing past @p scratch_limit
     *  bytes, zero for no limit, failing when no such device exists. */
    static st::expected<cuda_compaction_lane_t> make(int ordinal, std::size_t scratch_limit) noexcept;

    /**
     *  @brief Does what @c host_compactor_t::compact does with identical output, composing the chains of changes alone
     *      through @p device_fold and applying on the host, through @p fold, every chain that reaches a write, a
     *      removal or the bottom level's empty base.
     *
     *  With @c no_device_fold_t it refuses runs holding a change with @c invalid_argument_k, and with no change to fold
     *  the folds and @p read_value go unused. CUDA failures come back as statuses.
     */
    template <typename key_slot_type_, utilities::fold_concept fold_type_, typename device_fold_type_,
              typename allocator_type_>
        requires compactable_key_slot_concept<key_slot_type_>
    st::status_t compact(compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_type_> const> upper,
                         std::span<sorted_run_index_view<key_slot_type_> const> lower,
                         sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps, fold_type_ const& fold,
                         device_fold_type_ const& device_fold,
                         std::type_identity_t<located_value_reader<key_slot_type_>> read_value,
                         compaction_output<key_slot_type_, allocator_type_>& output) noexcept;

    /** The ordinal of the device the lane runs on. */
    int device() const noexcept { return stream_.device(); }

    /** Device allocations the lane made, which stop once its scratch holds the largest merge it serves. */
    std::uint64_t device_allocations() const noexcept { return device_allocations_; }

  private:

    /** One role of the lane's device scratch, which each compaction views as the elements it needs there. */
    enum class scratch_role_t : std::uint8_t {
        records_k,
        merged_k,
        run_cells_k,
        run_stamps_k,
        run_kinds_k,
        run_offsets_k,
        pinned_k,
        temporary_k,
        flags_k,
        selected_k,
        prefix_k,
        placements_k,
        cells_k,
        stamps_k,
        kinds_k,
        fold_indices_k,
        fold_records_k,
        count_k,
    };

    /** The device bytes of one role. */
    using scratch_t = utilities::cuda_device_buffer<std::byte>;

    /** Roles the scratch holds. */
    static constexpr std::size_t scratch_roles_k = static_cast<std::size_t>(scratch_role_t::count_k);

    cuda_compaction_lane_t(cuda_stream_t&& stream, std::size_t scratch_limit) noexcept
        : stream_(std::move(stream)), scratch_limit_(scratch_limit),
          scratch_(scratch_of_(stream_.device(), std::make_index_sequence<scratch_roles_k> {})) {}

    template <std::size_t... roles_>
    static std::array<scratch_t, scratch_roles_k> scratch_of_(int device, std::index_sequence<roles_...>) noexcept {
        return {((void)roles_, scratch_t {utilities::cuda_device_allocator<std::byte>(device)})...};
    }

    /**
     *  @brief Points @p into at room for @p count elements in the scratch of @p role, growing it when it holds fewer
     *      bytes, after the kernels queued so far finished with what it held.
     *  @retval out_of_memory_heap_k The growth would take the scratch past its limit, so the merge runs on the host.
     *  @note Device memory carries no type of its own, so a role's bytes are viewed as whatever elements a compaction
     *      places there.
     */
    template <typename element_type_>
    st::status_t room_(scratch_role_t role, std::size_t count, element_type_*& into) noexcept {
        scratch_t& scratch = scratch_[static_cast<std::size_t>(role)];
        std::size_t const bytes = count * sizeof(element_type_);
        if (bytes > scratch.size()) {
            std::size_t const others = scratch_bytes_ - scratch.size();
            if (scratch_limit_ != 0 && others + bytes > scratch_limit_) return st::status_t::out_of_memory_heap_k;
            if (st::status_t const drained = stream_.synchronize(); drained != st::status_t::success_k) return drained;
            st::status_t const grown = scratch.reserve(bytes);
            scratch_bytes_ = others + scratch.size();
            if (grown != st::status_t::success_k) return grown;
            ++device_allocations_;
        }
        into = static_cast<element_type_*>(static_cast<void*>(scratch.data()));
        return st::status_t::success_k;
    }

    /** The stream every transfer and kernel of the lane queues on. */
    cuda_stream_t stream_;

    /** Bytes the scratch may hold across every role, zero for no limit. */
    std::size_t scratch_limit_ = 0;

    /** Bytes the scratch holds across every role. */
    std::size_t scratch_bytes_ = 0;

    /** The device scratch every compaction reuses, one buffer per role. */
    std::array<scratch_t, scratch_roles_k> scratch_;

    /** Device allocations the scratch made since the lane was made. */
    std::uint64_t device_allocations_ = 0;
};

#endif

/** The compactor a database's merges run on, picked once at mount by whoever opens it: the calling thread, or a warm
 *  device lane that opener owns. */
using compactor_ref_t = std::variant<host_compactor_t, cuda_compaction_lane_t*>;

#pragma endregion Device

} // namespace unum::ustore::blobs
