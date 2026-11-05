/**
 *  @file include/ustore/blobs/nvme_compaction.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Compaction of LSM levels into new sorted runs on the host, and the device compactor every compiler sees.
 *
 *  A compaction reads the sorted run indexes of an upper LSM level and of the lower sorted runs
 *  it overlaps, keeps the newest key slot of every stored key, drops removals when it writes the
 *  bottom LSM level, and lays the survivors into sorted runs that fit their bounds, planning the
 *  coalesced ranges every stored value moves in and the value offsets of every new run. Both
 *  compactors share the rules below. The device one composes chains of changes only through a
 *  device fold the caller passes, and folds on the host every chain reaching a write or a base,
 *  so without a device fold a merge of runs holding a change runs on the host. Every compiler
 *  sees the device compactor, which carves, copies and launches through its executor: its kernels
 *  in @c vram_nvidia_compaction.cuh compile into the library's bundles, and @c compaction.cpp
 *  instantiates its compaction for the engine's key slots, so a host TU calls it, and a device
 *  whose bundle carries no kernels makes no device compactor.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::min`
#include <array>       // `std::array`
#include <concepts>    // `std::convertible_to`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::allocator_traits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_trivially_copyable_v`, `std::conditional_t`, `std::is_void_v`
#include <utility>     // `std::exchange`, `std::move`, `std::swap`
#include <variant>     // `std::variant`

#include <smashtable/basic_vector.hpp>           // `st::vector`
#include <smashtable/immutable_packed_array.hpp> // `st::immutable_packed_array`
#include <smashtable/immutable_splus_tree.hpp>   // `st::immutable_splus_tree_view`
#include <smashtable/shared.hpp>                 // `st::status_t`, `st::default_allocator`

#include "ustore/blobs/nvme_key_slot.hpp"      // `stored_key_concept`
#include "ustore/utilities/blocking_files.hpp" // `path_max_bytes_k`
#include "ustore/utilities/folds.hpp"          // `fold_concept`, `fold_delta_pair`, `delta_kind_t`
#include "ustore/utilities/nvme_ring.hpp"      // `extent_t`, `ring_concept`, `queue_pair_t`, `read_request`
#include "ustore/utilities/ram_buffers.hpp"    // `local_arena_t`, `arena_allocator`
#include "ustore/utilities/types.hpp"          // `round_up_to_multiple`, `expected`, `uuid_t`

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

/** Where a sorted run's sections sit on its volume, all empty for a run held in memory alone, and the stamp an ingest
 *  gave it, which only a device compactor reads, from the trailer, beside the run's index view. */
struct sorted_run_extents_t {

    /** The volume the run lives on. */
    std::uint32_t volume = 0;

    /** The index section: every cell, empty for a run whose cells are its keys alone. */
    utilities::extent_t cells;

    /** The value offsets section, empty but for a variable width. */
    utilities::extent_t value_offsets;

    /** The trailer: the saved stamps column, then the saved kinds column @c kinds_begin bytes in. */
    utilities::extent_t trailer;

    /** Where the saved kinds start inside the trailer. */
    std::uint64_t kinds_begin = 0;

    /** The stamp an ingest gave every version of a run its blob stamps zero, or zero for a run carrying its own, which
     *  the device raises every stamp it unpacks from the trailer to. */
    std::uint64_t assigned_stamp = 0;

    /** The section of the saved key tree, which a run of keys alone is read through. */
    utilities::extent_t keys;

    /** Where the tree's leaf rows start inside @c keys. */
    std::uint64_t leaves_begin = 0;

    /** The run's ID, which names its file on a directory volume. */
    utilities::uuid_t run {};
};

/** The keys from @c start up to @c end, exclusive, which one range removal hides. */
template <typename stored_key_type_>
struct range_tombstone {

    /** The first key hidden. */
    stored_key_type_ start {};

    /** The key past the last one hidden. */
    stored_key_type_ end {};

    /** Whether @p stored_key falls between the bounds. */
    [[nodiscard]] constexpr bool covers(stored_key_type_ const& stored_key) const noexcept {
        return !(stored_key < start) && stored_key < end;
    }
};

/**
 *  @brief The range tombstones of one run, ascending by start: each one's bounds and the stamp of
 *      the commit that removed them, two columns beside the run's deltas.
 *
 *  A run holds few of them, so a lookup scans every tombstone starting at or below its key. Every
 *  stamp is raw, never raised to a floor, as a reader compares it to the stamp it reads at.
 */
template <typename stored_key_type_, typename allocator_type_ = st::default_allocator<byte_t>>
class range_tombstones {
  public:
    using stored_key_t = stored_key_type_;
    using allocator_t = allocator_type_;
    using range_t = range_tombstone<stored_key_t>;

    range_tombstones() noexcept = default;
    range_tombstones(range_tombstones&&) noexcept = default;
    range_tombstones& operator=(range_tombstones&&) noexcept = default;
    range_tombstones(range_tombstones const&) = delete;
    range_tombstones& operator=(range_tombstones const&) = delete;

    /**
     *  @brief Copies @p ranges sorted by start, each stamped by its entry of @p stamps.
     *
     *  @par Statuses:
     *  - @c invalid_argument_k Spans of unequal lengths, or a range ending at or before its start.
     *  - @c out_of_memory_k A column could not be allocated.
     */
    static expected<range_tombstones> make(std::span<range_t const> ranges, std::span<std::uint64_t const> stamps,
                                           allocator_t allocator = {}) noexcept {
        if (ranges.size() != stamps.size())
            return status_t {status_code_t::invalid_argument_k, "One stamp per range tombstone"};
        range_tombstones made {allocator};
        if (made.ranges_.resize(ranges.size()) != st::status_t::success_k ||
            made.stamps_.resize(ranges.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for range tombstones"};
        for (std::size_t index = 0; index != ranges.size(); ++index) {
            if (!(ranges[index].start < ranges[index].end))
                return status_t {status_code_t::invalid_argument_k, "A range tombstone ends at or before its start"};
            // Few tombstones arrive at once, so an insertion keeps both columns in step.
            std::size_t at = index;
            for (; at != 0 && ranges[index].start < made.ranges_[at - 1].start; --at) {
                made.ranges_[at] = made.ranges_[at - 1];
                made.stamps_[at] = made.stamps_[at - 1];
            }
            made.ranges_[at] = ranges[index];
            made.stamps_[at] = stamps[index];
        }
        return made;
    }

    /** Tombstones held. */
    [[nodiscard]] std::size_t size() const noexcept { return ranges_.size(); }

    /** Whether no tombstone is held. */
    [[nodiscard]] bool empty() const noexcept { return ranges_.empty(); }

    /** Every tombstone's bounds, ascending by start. */
    [[nodiscard]] std::span<range_t const> ranges() const noexcept { return ranges_; }

    /** The stamp of every tombstone, in the order of @c ranges. */
    [[nodiscard]] std::span<std::uint64_t const> stamps() const noexcept { return stamps_; }

    /** The bounds of tombstone @p index. */
    [[nodiscard]] range_t const& range(std::size_t index) const noexcept { return ranges_[index]; }

    /** The stamp of tombstone @p index. */
    [[nodiscard]] std::uint64_t stamp(std::size_t index) const noexcept { return stamps_[index]; }

    /** Bytes both columns take. */
    [[nodiscard]] std::uint64_t bytes() const noexcept {
        return std::uint64_t {ranges_.size()} * (sizeof(range_t) + sizeof(std::uint64_t));
    }

    /** Stamps every tombstone with the commit's @p stamp, into columns already allocated. */
    void assign_stamp(std::uint64_t stamp) noexcept {
        for (std::uint64_t& held : stamps_) held = stamp;
    }

    /** The newest stamp a tombstone carries, or zero for none. */
    [[nodiscard]] std::uint64_t newest_stamp() const noexcept {
        std::uint64_t newest = 0;
        for (std::uint64_t const stamp : stamps_) newest = stamp > newest ? stamp : newest;
        return newest;
    }

    /** The least stamp above @p stamp of a tombstone covering @p stored_key, or zero for
     *  none, which decides whether any reader still tells a version stamped @p stamp apart
     *  from its removal. */
    [[nodiscard]] std::uint64_t covering_stamp_above(stored_key_t const& stored_key,
                                                     std::uint64_t stamp) const noexcept {
        std::uint64_t covering = 0;
        for (std::size_t index = 0; index != ranges_.size() && !(stored_key < ranges_[index].start); ++index)
            if (stored_key < ranges_[index].end && stamps_[index] > stamp &&
                (covering == 0 || stamps_[index] < covering))
                covering = stamps_[index];
        return covering;
    }

    /** The newest stamp up to @p at of a tombstone covering @p stored_key, or zero for none. */
    [[nodiscard]] std::uint64_t covering_stamp(stored_key_t const& stored_key, std::uint64_t at) const noexcept {
        std::uint64_t covering = 0;
        for (std::size_t index = 0; index != ranges_.size() && !(stored_key < ranges_[index].start); ++index)
            if (stored_key < ranges_[index].end && stamps_[index] <= at && stamps_[index] > covering)
                covering = stamps_[index];
        return covering;
    }

    /** Whether a tombstone newer than @p from hides a key from @p lower to @p upper inclusive. */
    [[nodiscard]] bool overlaps_since(stored_key_t const& lower, stored_key_t const& upper,
                                      std::uint64_t from) const noexcept {
        for (std::size_t index = 0; index != ranges_.size() && !(upper < ranges_[index].start); ++index)
            if (lower < ranges_[index].end && stamps_[index] > from) return true;
        return false;
    }

    /** The least key a tombstone hides, or the greatest key of the type for none. */
    [[nodiscard]] stored_key_t lower_stored_key() const noexcept {
        return ranges_.empty() ? limit<stored_key_t>::max() : ranges_[0].start;
    }

    /** The greatest key a tombstone hides, or the least key of the type for none. */
    [[nodiscard]] stored_key_t upper_stored_key() const noexcept {
        stored_key_t upper = limit<stored_key_t>::min();
        for (range_t const& range : ranges_)
            if (upper < stored_key_before(range.end)) upper = stored_key_before(range.end);
        return upper;
    }

  private:
    using ranges_column_t =
        st::vector<range_t, typename std::allocator_traits<allocator_t>::template rebind_alloc<range_t>>;
    using stamps_column_t =
        st::vector<std::uint64_t, typename std::allocator_traits<allocator_t>::template rebind_alloc<std::uint64_t>>;

    explicit range_tombstones(allocator_t const& allocator) noexcept
        : ranges_(typename ranges_column_t::allocator_t(allocator)),
          stamps_(typename stamps_column_t::allocator_t(allocator)) {}

    /** The bounds, ascending by start. */
    ranges_column_t ranges_;

    /** The stamp of every tombstone, in the order of @c ranges_. */
    stamps_column_t stamps_;
};

/** The strings from @c start up to @c end, exclusive, which one range removal hides, an empty
 *  @c end reaching past the last string; both view the bytes of whoever holds the tombstone. */
template <>
struct range_tombstone<std::string_view> {

    /** The first string hidden. */
    std::string_view start;

    /** The string past the last one hidden, or empty for none. */
    std::string_view end;

    /** Whether @p text falls between the bounds. */
    [[nodiscard]] constexpr bool covers(std::string_view text) const noexcept {
        return !(text < start) && (end.empty() || text < end);
    }

    /** Whether the bounds hide a string from @p lower to @p upper inclusive. */
    [[nodiscard]] constexpr bool overlaps(std::string_view lower, std::string_view upper) const noexcept {
        return !(upper < start) && (end.empty() || lower < end);
    }
};

/**
 *  @brief The range tombstones of one run of strings, ascending by start: each one's bounds, copied
 *      onto a tape the tombstones own, and the raw stamp of the commit that removed them.
 *
 *  A run holds few of them, so a lookup scans every tombstone starting at or below its string.
 */
template <typename allocator_type_>
class range_tombstones<std::string_view, allocator_type_> {
  public:
    using stored_key_t = std::string_view;
    using allocator_t = allocator_type_;
    using range_t = range_tombstone<std::string_view>;

    range_tombstones() noexcept = default;
    range_tombstones(range_tombstones&&) noexcept = default;
    range_tombstones& operator=(range_tombstones&&) noexcept = default;
    range_tombstones(range_tombstones const&) = delete;
    range_tombstones& operator=(range_tombstones const&) = delete;

    /**
     *  @brief Copies @p ranges and their bytes, the one at each index stamped by @p stamps at the
     *      same index, sorted by start.
     *
     *  @par Statuses:
     *  - @c invalid_argument_k Spans of unequal lengths, or a range ending at or before its start.
     *  - @c out_of_memory_k A column could not be allocated.
     */
    static expected<range_tombstones> make(std::span<range_t const> ranges, std::span<std::uint64_t const> stamps,
                                           allocator_t allocator = {}) noexcept {
        if (ranges.size() != stamps.size())
            return status_t {status_code_t::invalid_argument_k, "One stamp per range tombstone"};
        std::size_t tape_bytes = 0;
        for (range_t const& range : ranges) {
            if (!range.end.empty() && !(range.start < range.end))
                return status_t {status_code_t::invalid_argument_k, "A range tombstone ends at or before its start"};
            tape_bytes += range.start.size() + range.end.size();
        }
        range_tombstones made {allocator};
        if (made.ranges_.resize(ranges.size()) != st::status_t::success_k ||
            made.stamps_.resize(ranges.size()) != st::status_t::success_k ||
            made.tape_.resize(tape_bytes) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for range tombstones"};
        std::size_t written = 0;
        auto const copied = [&](std::string_view text) noexcept {
            if (!text.empty()) std::memcpy(made.tape_.data() + written, text.data(), text.size());
            std::string_view const kept {made.tape_.data() + written, text.size()};
            written += text.size();
            return kept;
        };
        for (std::size_t index = 0; index != ranges.size(); ++index) {
            range_t const range {copied(ranges[index].start), copied(ranges[index].end)};
            std::size_t at = index;
            for (; at != 0 && range.start < made.ranges_[at - 1].start; --at) {
                made.ranges_[at] = made.ranges_[at - 1];
                made.stamps_[at] = made.stamps_[at - 1];
            }
            made.ranges_[at] = range;
            made.stamps_[at] = stamps[index];
        }
        return made;
    }

    /** Tombstones held. */
    [[nodiscard]] std::size_t size() const noexcept { return ranges_.size(); }

    /** Whether no tombstone is held. */
    [[nodiscard]] bool empty() const noexcept { return ranges_.empty(); }

    /** Every tombstone's bounds, ascending by start, viewing the tape. */
    [[nodiscard]] std::span<range_t const> ranges() const noexcept { return ranges_; }

    /** The stamp of every tombstone, in the order of @c ranges. */
    [[nodiscard]] std::span<std::uint64_t const> stamps() const noexcept { return stamps_; }

    /** The bounds of tombstone @p index. */
    [[nodiscard]] range_t const& range(std::size_t index) const noexcept { return ranges_[index]; }

    /** The stamp of tombstone @p index. */
    [[nodiscard]] std::uint64_t stamp(std::size_t index) const noexcept { return stamps_[index]; }

    /** Bytes the bounds and the stamps take, the tape's included. */
    [[nodiscard]] std::uint64_t bytes() const noexcept {
        return std::uint64_t {ranges_.size()} * (sizeof(range_t) + sizeof(std::uint64_t)) + tape_.size();
    }

    /** Stamps every tombstone with @p stamp, the commit's, into columns already allocated. */
    void assign_stamp(std::uint64_t stamp) noexcept {
        for (std::uint64_t& held : stamps_) held = stamp;
    }

    /** The newest stamp a tombstone carries, or zero for none. */
    [[nodiscard]] std::uint64_t newest_stamp() const noexcept {
        std::uint64_t newest = 0;
        for (std::uint64_t const stamp : stamps_) newest = stamp > newest ? stamp : newest;
        return newest;
    }

    /** The least stamp above @p stamp of a tombstone covering @p text, or zero for none. */
    [[nodiscard]] std::uint64_t covering_stamp_above(std::string_view text, std::uint64_t stamp) const noexcept {
        std::uint64_t covering = 0;
        for (std::size_t index = 0; index != ranges_.size() && !(text < ranges_[index].start); ++index)
            if (ranges_[index].covers(text) && stamps_[index] > stamp && (covering == 0 || stamps_[index] < covering))
                covering = stamps_[index];
        return covering;
    }

    /** The newest stamp at or below @p at of a tombstone covering @p text, or zero for none. */
    [[nodiscard]] std::uint64_t covering_stamp(std::string_view text, std::uint64_t at) const noexcept {
        std::uint64_t covering = 0;
        for (std::size_t index = 0; index != ranges_.size() && !(text < ranges_[index].start); ++index)
            if (ranges_[index].covers(text) && stamps_[index] <= at && stamps_[index] > covering)
                covering = stamps_[index];
        return covering;
    }

    /** Whether a tombstone newer than @p from hides a string in @p lower to @p upper inclusive. */
    [[nodiscard]] bool overlaps_since(std::string_view lower, std::string_view upper,
                                      std::uint64_t from) const noexcept {
        for (std::size_t index = 0; index != ranges_.size(); ++index)
            if (ranges_[index].overlaps(lower, upper) && stamps_[index] > from) return true;
        return false;
    }

  private:
    template <typename value_type_>
    using column =
        st::vector<value_type_, typename std::allocator_traits<allocator_t>::template rebind_alloc<value_type_>>;

    explicit range_tombstones(allocator_t const& allocator) noexcept
        : ranges_(typename column<range_t>::allocator_t(allocator)),
          stamps_(typename column<std::uint64_t>::allocator_t(allocator)),
          tape_(typename column<char>::allocator_t(allocator)) {}

    /** The bounds, ascending by start, viewing @c tape_. */
    column<range_t> ranges_;

    /** The stamp of every tombstone, in the order of @c ranges_. */
    column<std::uint64_t> stamps_;

    /** The bytes of every bound, which a move hands over unmoved. */
    column<char> tape_;
};

/** The key slots of one sorted run in sorted order, and the stamp and kind of every one. */
template <typename key_slot_type_>
struct sorted_run_index_view {

    /** The run's cells in sorted order, or null for a run of keys alone, read through @c keys. */
    key_slot_type_ const* cells = nullptr;

    /** Key slots the run holds. */
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

    /** The saved tree of the run's keys, which a view without @c cells reads key slots through. */
    st::immutable_splus_tree_view<typename key_slot_type_::stored_key_t> const* keys = nullptr;

    /** The run's range tombstones, or null for none, which only the host compactor reads. */
    blobs::range_tombstones<typename key_slot_type_::stored_key_t> const* range_tombstones = nullptr;
};

/** One version a merge of strings carries: its source run and its sorted position there. */
struct strings_version_t {

    /** Its source: the upper runs in their order, then the lower runs. */
    std::uint32_t source = 0;

    /** Its sorted position in that source. */
    std::uint32_t position = 0;
};

/** One run of strings as a merge reads it in place: the saved tree of its heads, its keys whole,
 *  and the stamp and kind of every one. */
struct strings_run_view {

    /** The saved S+ tree of the run's heads. */
    st::immutable_splus_tree_view<st::key128_t> const* heads = nullptr;

    /** One offset per key into @c keys and one past the last. */
    std::span<std::uint32_t const> key_offsets;

    /** Every key whole, back to back in sorted order. */
    std::span<byte_t const> keys;

    /** The stamp of every key in sorted order. */
    stamps_column_t const* stamps = nullptr;

    /** The kind of every key in sorted order. */
    kinds_column_t const* kinds = nullptr;
};

/** The key slot at sorted @p position of the run @p view describes, from its cells or tree. */
template <typename key_slot_type_>
key_slot_type_ key_slot_of(sorted_run_index_view<key_slot_type_> const& view, std::uint64_t position) noexcept {
    if (view.cells != nullptr) return view.cells[position];
    key_slot_type_ slot {};
    slot.stored_key = view.keys->select(static_cast<std::size_t>(position));
    return slot;
}

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

    /** Largest sorted run file in bytes, which a sorted run of one key slot may still exceed. */
    std::uint64_t file_bytes_limit = 0;

    /** Bytes of the serialized header, aligned already. */
    std::uint64_t header_bytes = 0;

    /** Alignment of the key slots and stored values sections. */
    std::uint64_t alignment_bytes = 0;

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
    st::vector<element_type_, typename std::allocator_traits<allocator_type_>::template rebind_alloc<element_type_>>;

/** What one compaction writes, kept between compactions so their buffers are reused. */
template <typename key_slot_type_, typename allocator_type_ = st::default_allocator<key_slot_type_>>
struct compaction_output {

    /** The cells of the new sorted runs one after another, each run's in sorted order. */
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
 *  A version is read by the readers pinned at or above it and below the version before it, so it
 *  survives when the two fall in different reader intervals. Above the durable horizon every
 *  version survives, since a mount may drop any version above whatever horizon the schema it finds
 *  promises, and needs the one beneath.
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

/** Whether a source of @p upper or @p lower has a range tombstone, keeping a merge on the host. */
template <typename key_slot_type_>
bool carries_range_tombstones(std::span<sorted_run_index_view<key_slot_type_> const> upper,
                              std::span<sorted_run_index_view<key_slot_type_> const> lower) noexcept {
    for (sorted_run_index_view<key_slot_type_> const& view : upper)
        if (view.range_tombstones != nullptr && !view.range_tombstones->empty()) return true;
    for (sorted_run_index_view<key_slot_type_> const& view : lower)
        if (view.range_tombstones != nullptr && !view.range_tombstones->empty()) return true;
    return false;
}

/**
 *  @brief Whether a range tombstone of the sources @p upper and @p lower hides @p record from every
 *      reader of @p stamps, so a merge drops it.
 *
 *  A tombstone of a newer source, or of the record's own source stamped above it, hides the
 *  record. A merge raises a version's stamp to its floor while a tombstone keeps its own, so a
 *  record of an older source stamped at or above the tombstone was raised past it, where no reader
 *  tells the two apart; any other the tombstone hides from every reader once none reads between
 *  them. A tombstone above the horizon, which a mount may drop, or above the ceiling, which this
 *  merge drops, hides nothing.
 */
template <typename key_slot_type_>
bool hidden_by_range(std::span<sorted_run_index_view<key_slot_type_> const> upper,
                     std::span<sorted_run_index_view<key_slot_type_> const> lower,
                     located_key_slot<key_slot_type_> const& record, compaction_stamps_t const& stamps) noexcept {
    std::size_t const upper_count = upper.size();
    for (std::size_t source = 0; source != upper.size() + lower.size(); ++source) {
        auto const* const tombstones = source < upper_count ? upper[source].range_tombstones
                                                            : lower[source - upper_count].range_tombstones;
        if (tombstones == nullptr || tombstones->empty()) continue;
        bool const own = source == record.sorted_run;
        bool const newer = source < upper_count && (record.sorted_run >= upper_count || source > record.sorted_run);
        if (!own && !newer) continue;
        auto const ranges = tombstones->ranges();
        for (std::size_t index = 0; index != ranges.size() && !(record.key_slot.stored_key < ranges[index].start);
             ++index) {
            std::uint64_t const stamp = tombstones->stamp(index);
            if (!ranges[index].covers(record.key_slot.stored_key) || stamp > stamps.ceiling || stamp > stamps.horizon)
                continue;
            if (own && stamp <= record.stamp) continue;
            if (record.stamp >= stamp || reader_interval(stamps, record.stamp) == reader_interval(stamps, stamp))
                return true;
        }
    }
    return false;
}

/** Whether a merge of @p kind writing @p stamps drops a range tombstone stamped @p stamp: one above
 *  the ceiling, or one at the bottom that every reader reads at or above, once nothing older is
 *  left for it to hide. */
constexpr bool drops_range_tombstone(compaction_kind_t kind, compaction_stamps_t const& stamps,
                                     std::uint64_t stamp) noexcept {
    if (stamp > stamps.ceiling) return true;
    return writes_bottom(kind) && stamp <= stamps.horizon && reader_interval(stamps, stamp) == stamps.pinned_count;
}

/** Bytes the value at sorted @p position takes in its run's values section: through @p value_offsets where the run
 *  carries them, and @p value_stride otherwise, which is zero for values held inline. */
constexpr std::uint32_t stored_value_bytes(stored_value_offset_t const* value_offsets, std::uint64_t value_stride,
                                           std::uint64_t position) noexcept {
    return value_offsets != nullptr ? value_offsets[position + 1] - value_offsets[position]
                                    : static_cast<std::uint32_t>(value_stride);
}

/** Offset of the stored values section in a sorted run of @p key_slots_count key slots, past its
 *  cells, the saved tree of its keys and its value offsets. */
template <typename key_slot_type_>
constexpr std::uint64_t stored_values_offset(sorted_run_bounds_t const& bounds,
                                             std::uint64_t key_slots_count) noexcept {
    // Only cells carrying inline values are saved; the key tree holds every key of the others.
    std::uint64_t cells_bytes = 0;
    if constexpr (!locates_stored_values_concept<key_slot_type_>)
        cells_bytes = key_slots_count * sizeof(key_slot_type_);
    std::uint64_t const keys_bytes = sizeof(st::splus_tree_header_t) +
                                     st::immutable_splus_tree_view<typename key_slot_type_::stored_key_t>::size_bytes(
                                         static_cast<std::size_t>(key_slots_count));
    std::uint64_t offsets_bytes = 0;
    if constexpr (locates_stored_values_concept<key_slot_type_>)
        if (is_variable_width(key_slot_type_::value_layout_k))
            offsets_bytes = round_up_to_multiple((key_slots_count + 1) * sizeof(stored_value_offset_t),
                                                 bounds.alignment_bytes);
    return bounds.header_bytes + round_up_to_multiple(cells_bytes, bounds.alignment_bytes) +
           round_up_to_multiple(keys_bytes, bounds.alignment_bytes) + offsets_bytes;
}

/** Bytes of a sorted run file of @p key_slots_count key slots locating @p stored_values_bytes bytes of stored
 *  values, which for a run of strings are its keys whole and the layers they land in. */
template <typename key_slot_type_>
constexpr std::uint64_t sorted_run_file_bytes(sorted_run_bounds_t const& bounds, std::uint64_t key_slots_count,
                                              std::uint64_t stored_values_bytes) noexcept {
    // A run of strings keeps its heads in a saved S+ tree, then its values, key offsets, keys
    // and layers apart.
    if constexpr (tailed_key_slot_concept<key_slot_type_>) {
        std::uint64_t const heads = sizeof(st::splus_tree_header_t) +
                                    st::immutable_splus_tree_view<st::key128_t>::size_bytes(
                                        static_cast<std::size_t>(key_slots_count));
        return bounds.header_bytes + round_up_to_multiple(heads, bounds.alignment_bytes) +
               round_up_to_multiple(key_slots_count * key_slot_type_::size, bounds.alignment_bytes) +
               round_up_to_multiple((key_slots_count + 1) * sizeof(std::uint32_t), bounds.alignment_bytes) +
               round_up_to_multiple(stored_values_bytes, bounds.alignment_bytes) + bounds.alignment_bytes;
    }
    std::uint64_t const key_slots_end = stored_values_offset<key_slot_type_>(bounds, key_slots_count);
    if constexpr (locates_stored_values_concept<key_slot_type_>)
        return key_slots_end + round_up_to_multiple(stored_values_bytes, bounds.alignment_bytes);
    else return key_slots_end;
}

/** Whether @p bounds admit sorted runs: a file limit and an alignment. */
constexpr bool admits_sorted_runs(sorted_run_bounds_t const& bounds) noexcept {
    return (bounds.file_bytes_limit != 0) & (bounds.alignment_bytes != 0);
}

/**
 *  @brief Writes a survivor into its cell of the new sorted runs, which hold the survivors in
 *      order, with its stamp and its kind.
 *  @param[in] source The survivor with the index of its old sorted run.
 *  @param[in] floor The stamp the survivor's stamp is raised to at least.
 *  @param[out] cell The survivor's cell.
 *  @param[out] stamp The survivor's stamp, raised to @p floor.
 *  @param[out] kind The survivor's kind.
 */
template <typename key_slot_type_>
constexpr void place_key_slot(located_key_slot<key_slot_type_> const& source, std::uint64_t floor, key_slot_type_& cell,
                              std::uint64_t& stamp, std::uint8_t& kind) noexcept {
    stamp = source.stamp > floor ? source.stamp : floor;
    kind = static_cast<std::uint8_t>(source.kind);
    cell = source.key_slot;
}

/**
 *  @brief Lays the value @p bytes a fold wrote for @p record into @p folded, the bytes of a merge's folded values, or
 *      into its inline key slot, so the copy plan and the new run read it where @p record now names it.
 *  @param[in] bytes The value the fold wrote.
 *  @param[in] value_stride Bytes every value of a fixed-width layout takes, which a folded removal fills with zeros.
 *  @param[in] folded_run The index past every source run, which names the folded bytes.
 *  @param[in,out] record The survivor, which now names where its value is.
 *  @param[out] folded The bytes of the merge's folded values, or the inline key slot when the value fits it.
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
 *  @param[in,out] sorted_runs The planned sorted runs, each of which has its range count set.
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
 *  @brief Cuts @p key_slots_count sorted survivors into sorted runs of equal size that each
 *      fit @p bounds.
 *
 *  Every cut aims at the bytes left divided by the runs they still need, so a cut that writes two
 *  or more runs fills each at least halfway, and a run always takes at least one key slot. The
 *  versions of one key never straddle two runs, since a sorted level answers a key from the one run
 *  whose bounds hold it: a cut moves back to where the key begins, or past where it ends when it
 *  began the run.
 *
 *  @param[in] key_slots_count How many survivors there are to cut.
 *  @param[in] bounds The file limit every sorted run holds to.
 *  @param[in] read_prefix Called as @c read_prefix(index, bytes) to write the stored value bytes of the first @c index
 *      survivors.
 *  @param[in] shares_key Called as @c shares_key(index, shared) to say whether survivor @c index carries the key
 *      survivor @c index - 1 does.
 *  @param[out] sorted_runs The planned sorted runs, replacing what it held.
 */
template <typename key_slot_type_, typename prefix_reader_type_, typename key_sharing_type_, typename sorted_runs_type_>
st::status_t plan_sorted_runs(std::uint64_t key_slots_count, sorted_run_bounds_t const& bounds,
                              prefix_reader_type_&& read_prefix, key_sharing_type_&& shares_key,
                              sorted_runs_type_& sorted_runs) noexcept {
    using st::status_t;
    sorted_runs.clear();

    std::uint64_t all_bytes = 0;
    status_t status = read_prefix(key_slots_count, all_bytes);
    if (status != status_t::success_k) return status;

    std::uint64_t first = 0;
    std::uint64_t first_bytes = 0;
    while (first != key_slots_count) {
        std::uint64_t const remaining = key_slots_count - first;
        std::uint64_t const remaining_bytes = sorted_run_file_bytes<key_slot_type_>(bounds, remaining,
                                                                                    all_bytes - first_bytes);
        std::uint64_t const runs_needed = divide_round_up(remaining_bytes, bounds.file_bytes_limit);
        // Every run pays its sections' first pages and its tree's first row, which an equal share
        // must not split.
        std::uint64_t const run_overhead = sorted_run_file_bytes<key_slot_type_>(bounds, 1, 0);
        std::uint64_t const target_bytes =
            runs_needed > 1 ? std::min(divide_round_up(remaining_bytes + (runs_needed - 1) * run_overhead, runs_needed),
                                       bounds.file_bytes_limit)
                            : bounds.file_bytes_limit;

        std::uint64_t taken = remaining;
        std::uint64_t taken_bytes = all_bytes;
        if (sorted_run_file_bytes<key_slot_type_>(bounds, taken, taken_bytes - first_bytes) > target_bytes) {
            std::uint64_t fitting = 1;
            std::uint64_t low = 2;
            std::uint64_t high = taken - 1;
            while (low <= high) {
                std::uint64_t const middle = low + (high - low) / 2;
                std::uint64_t middle_bytes = 0;
                status = read_prefix(first + middle, middle_bytes);
                if (status != status_t::success_k) return status;
                if (sorted_run_file_bytes<key_slot_type_>(bounds, middle, middle_bytes - first_bytes) <= target_bytes) {
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
            std::uint64_t boundary = first + taken;
            while (shared && boundary != first + 1) {
                status = shares_key(--boundary, shared);
                if (status != status_t::success_k) return status;
            }
            // Every survivor of the run so far carries one key, so the run takes that key whole however long it is.
            if (shared)
                for (boundary = first + taken + 1; boundary != key_slots_count; ++boundary) {
                    status = shares_key(boundary, shared);
                    if (status != status_t::success_k) return status;
                    if (!shared) break;
                }
            taken = boundary - first;
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

/** The position a row of @c union_sorted_keys names in a side that lacks its key. */
inline constexpr std::size_t absent_position_k = std::numeric_limits<std::size_t>::max();

/**
 *  @brief Merges two ascending sequences of distinct keys into their union, writing for each row,
 *      in ascending order of keys, the key's position in each side or @c absent_position_k where
 *      the side lacks it.
 *  @param[in] read_first Called as <tt>read_first(index, views)</tt>, filling every one of @p views with
 *      the keys of the first side from @p index on.
 *  @param[in] first_count Keys of the first side.
 *  @param[in] read_second As @p read_first, for the second side.
 *  @param[in] second_count Keys of the second side.
 *  @param[out] first_positions Room for @p first_count plus @p second_count positions in
 *      the first side.
 *  @param[out] second_positions As much room, for positions in the second side.
 *  @return Rows written.
 */
template <typename first_reader_type_, typename second_reader_type_>
std::size_t union_sorted_keys(first_reader_type_&& read_first, std::size_t first_count,
                              second_reader_type_&& read_second, std::size_t second_count,
                              std::span<std::size_t> first_positions,
                              std::span<std::size_t> second_positions) noexcept {
    // Each side is read 64 views at a time, refilled only once its cursor walks off the block.
    constexpr std::size_t block_k = 64;
    std::array<std::string_view, block_k> first_views, second_views;
    std::size_t first_index = 0, second_index = 0, first_block_start = 0, second_block_start = 0;
    std::size_t first_block_end = 0, second_block_end = 0, rows = 0;
    auto const emit = [&](std::size_t first, std::size_t second) noexcept {
        first_positions[rows] = first;
        second_positions[rows] = second;
        ++rows;
    };
    while (first_index != first_count && second_index != second_count) {
        if (first_index == first_block_end) {
            std::size_t const filled = std::min(block_k, first_count - first_index);
            read_first(first_index, std::span<std::string_view> {first_views}.first(filled));
            first_block_start = first_index, first_block_end = first_index + filled;
        }
        if (second_index == second_block_end) {
            std::size_t const filled = std::min(block_k, second_count - second_index);
            read_second(second_index, std::span<std::string_view> {second_views}.first(filled));
            second_block_start = second_index, second_block_end = second_index + filled;
        }
        int const order = first_views[first_index - first_block_start].compare(
            second_views[second_index - second_block_start]);
        if (order == 0) emit(first_index++, second_index++);
        else if (order < 0) emit(first_index++, absent_position_k);
        else emit(absent_position_k, second_index++);
    }
    for (; first_index != first_count; ++first_index) emit(first_index, absent_position_k);
    for (; second_index != second_count; ++second_index) emit(absent_position_k, second_index);
    return rows;
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
 *  @param[in] kind Which compaction this is, which decides whether it writes the bottom level.
 *  @param[in] stamps The stamps that tell which versions a reader still tells apart.
 *  @param[in] merged The merged records, in key order with the versions of a key newest first.
 *  @param[in] kept Whether each merged record survives.
 *  @param[in] merged_count How many records @p merged holds.
 *  @param[in] head The index in @p merged of the surviving change.
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
 *  @param[in] fold The fold that combines the versions into one change.
 *  @param[in] merged The merged records, in key order with the versions of a key newest first.
 *  @param[in] head The index in @p merged of the surviving change.
 *  @param[in] versions The versions to fold, as @c folded_versions found them.
 *  @param[in] value_of Called as <tt>value_of(record, bytes)</tt> to view the value of a merged
 *      record, answering an @c st::status_t, with any bytes it reads allocated from @p arena.
 *  @param[in,out] arena Where the bytes the fold reads and writes are allocated.
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
        using st::status_t;
        using st::vector;
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

        vector<record_t> records;
        vector<record_t> scratch;
        status_t status = records.resize(merged_count);
        if (status != status_t::success_k) return status;
        status = scratch.resize(merged_count);
        if (status != status_t::success_k) return status;

        // Every sorted run's cells are already in sorted order, in one run per sorted run
        vector<std::uint64_t> run_starts;
        status = run_starts.resize(upper.size() + 1);
        if (status != status_t::success_k) return status;
        std::uint64_t run_offset = 0;
        for (std::uint64_t sorted_run = 0; sorted_run != sorted_runs_count; ++sorted_run) {
            sorted_run_index_view<key_slot_type_> const& view = sorted_run < upper.size()
                                                                    ? upper[sorted_run]
                                                                    : lower[sorted_run - upper.size()];
            if (sorted_run <= upper.size()) run_starts[sorted_run] = run_offset;
            stored_value_offset_t const* const offsets = !view.value_offsets.empty() ? view.value_offsets.data()
                                                                                     : nullptr;
            for (std::uint64_t sorted = 0; sorted != view.key_slots_count; ++sorted)
                located_of(records[run_offset + sorted]) = located_key_slot<key_slot_type_> {
                    key_slot_of(view, sorted),
                    static_cast<std::uint32_t>(sorted_run),
                    delta_kind_t::write_k,
                    0,
                    static_cast<std::uint32_t>(sorted),
                    stored_value_bytes(offsets, bounds.value_stride, sorted)};
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

        // A version a range tombstone hides turns into a removal, so the versions below drop
        // and a change folds onto nothing, and then drops itself, as the tombstone stays with
        // the new runs.
        vector<std::uint8_t> hidden;
        if (carries_range_tombstones(upper, lower)) {
            status = hidden.resize(merged_count);
            if (status != status_t::success_k) return status;
            for (std::uint64_t index = 0; index != merged_count; ++index) {
                located_key_slot<key_slot_type_>& record = located_of(scratch[index]);
                hidden[index] = hidden_by_range(upper, lower, record, stamps) ? 1 : 0;
                if (hidden[index]) record.kind = delta_kind_t::remove_k;
            }
        }

        // Survival settles before any fold, so a fold sees every version dropped behind its change
        vector<std::uint8_t> kept;
        status = kept.resize(merged_count);
        if (status != status_t::success_k) return status;
        for (std::uint64_t index = 0; index != merged_count; ++index)
            kept[index] = survives(index != 0 ? scratch.data() + index - 1 : nullptr, scratch[index], kind, stamps);

        // A folded survivor's value, which replaces its own once every survivor is placed
        struct folded_t {
            std::uint64_t survivor = 0;
            std::span<byte_t const> bytes;
        };
        vector<folded_t> folds;
        utilities::local_arena_t arena;
        utilities::arena_allocator<byte_t> arena_bytes = arena.allocator();
        auto const value_of = [&](record_t const& record, std::span<byte_t const>& bytes) noexcept {
            bytes = {};
            if (located_of(record).kind == delta_kind_t::remove_k) return status_t::success_k;
            return read_value(located_of(record), arena_bytes, bytes);
        };
        std::uint64_t survivors = 0;
        for (std::uint64_t index = 0; index != merged_count; ++index) {
            if (!kept[index] || (!hidden.empty() && hidden[index])) continue;
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

        vector<std::uint64_t> prefix;
        if constexpr (locates_stored_values_concept<key_slot_type_>) {
            status = prefix.resize(survivors + 1);
            if (status != status_t::success_k) return status;
            prefix[0] = 0;
            for (std::uint64_t index = 0; index != survivors; ++index)
                prefix[index + 1] = prefix[index] + located_of(records[index]).size;
        }
        // Tails take room in a run file beside the values, so the plan bounds both together.
        vector<std::uint64_t> planned;
        if constexpr (tailed_k) {
            status = planned.resize(survivors + 1);
            if (status != status_t::success_k) return status;
            planned[0] = 0;
            for (std::uint64_t index = 0; index != survivors; ++index)
                planned[index + 1] = planned[index] + (prefix.size() != 0 ? prefix[index + 1] - prefix[index] : 0) +
                                     records[index].tail.size();
        }
        vector<std::uint64_t> const& bounded = tailed_k ? planned : prefix;
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

        status = output.cells.resize(survivors);
        if (status != status_t::success_k) return status;
        status = output.stamps.resize(survivors);
        if (status != status_t::success_k) return status;
        status = output.kinds.resize(survivors);
        if (status != status_t::success_k) return status;
        for (std::uint64_t survivor = 0; survivor != survivors; ++survivor)
            place_key_slot(located_of(records[survivor]), stamps.floor, output.cells[survivor], output.stamps[survivor],
                           output.kinds[survivor]);

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

/** One section a device compactor reads into its scratch: an extent of a volume, landing @c into bytes past the
 *  scratch's start. */
struct device_section_read_t {

    /** The volume the run lives on, which names the medium the transport reads it from. */
    std::uint32_t volume = 0;

    /** The section's range of that medium, aligned as an unbuffered read takes it. */
    utilities::extent_t extent;

    /** Where the bytes land, counted from the start of the scratch. */
    std::uint64_t into = 0;

    /** The run's ID, which names its file on a directory volume. */
    utilities::uuid_t run {};
};

/** A borrowed transport moving run sections from their volumes into a device compactor's scratch, so one compaction
 *  compiled per key slot reads through a transport of any type; an empty one reaches no volume, and the compactor
 *  uploads every run from its memory instead. */
class device_sections_reader_t {
  public:

    /** A reader reaching no volume. */
    device_sections_reader_t() noexcept = default;

    /** Borrows @p reader, which must outlive the compactor holding it. */
    template <typename reader_type_>
    explicit device_sections_reader_t(reader_type_* reader) noexcept
        : context_(reader), read_(&read_through_<reader_type_>), release_(&release_through_<reader_type_>) {}

    /** Whether the reader reaches the volumes at all. */
    explicit operator bool() const noexcept { return context_ != nullptr; }

    /** Reads every one of @p reads into @p scratch, device memory, answering once every byte landed. */
    st::status_t read(std::span<device_section_read_t const> reads, std::span<byte_t> scratch) const noexcept {
        return read_(context_, reads, scratch);
    }

    /** Forgets the scratch it last read into, which the compactor frees next. */
    void release() const noexcept { release_(context_); }

  private:
    using read_t = st::status_t (*)(void*, std::span<device_section_read_t const>, std::span<byte_t>) noexcept;
    using release_t = void (*)(void*) noexcept;

    template <typename reader_type_>
    static st::status_t read_through_(void* context, std::span<device_section_read_t const> reads,
                                      std::span<byte_t> scratch) noexcept {
        return static_cast<reader_type_*>(context)->read(reads, scratch);
    }

    template <typename reader_type_>
    static void release_through_(void* context) noexcept {
        static_cast<reader_type_*>(context)->release();
    }

    static st::status_t refuse_(void*, std::span<device_section_read_t const>, std::span<byte_t>) noexcept {
        return st::status_t::invalid_argument_k;
    }

    static void forget_nothing_(void*) noexcept {}

    /** The borrowed reader. */
    void* context_ = nullptr;

    /** Calls the borrowed reader's @c read as its own type. */
    read_t read_ = &refuse_;

    /** Calls the borrowed reader's @c release as its own type. */
    release_t release_ = &forget_nothing_;
};

/**
 *  @brief Reads run sections into device memory through a ring of its own reaching it, cuFile's among them, on one
 *      queue pair, registering the scratch it reads into once per scratch.
 *
 *  On raw media every volume is one medium, registered at @c make. On a directory volume every run
 *  is a file of its own, registered at the first read of the merge reading it and given back by
 *  @c release after that merge, since a merge's inputs are deallocated once it publishes.
 *
 *  @tparam disk_type_ The ring, which must reach device memory.
 *  @tparam waiter_type_ What a wait for completions does after a poll that moved nothing.
 *  @tparam volume_type_ The directory volume naming the run files, or @c void for raw media alone.
 */
template <typename disk_type_, typename waiter_type_, typename volume_type_ = void>
    requires utilities::ring_concept<disk_type_>
class ring_sections_reader {
  public:

    /** Requests one batch keeps in flight. */
    static constexpr utilities::queue_depth_t depth_k = 32;

    /** Bytes one request moves at most, so a section of any size stays within one transfer of the ring. */
    static constexpr std::uint64_t request_bytes_k = std::uint64_t {64} << 20;

    /** Takes @p disk in, reading volume @c i from the medium @p media[i] names, and leases a queue pair of it. */
    static expected<ring_sections_reader> make(disk_type_&& disk,
                                               std::span<utilities::medium_options_t const> media) noexcept {
        ring_sections_reader reader;
        reader.disk_ = std::move(disk);
        if (reader.media_.resize(media.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "The reader could not track its volumes"};
        for (std::size_t volume = 0; volume != media.size(); ++volume) {
            expected<utilities::registered_medium_t> registered = reader.disk_.register_medium(media[volume]);
            if (!registered) return registered.status();
            reader.media_[volume] = std::move(*registered);
        }
        expected<utilities::queue_pair_t> leased = reader.disk_.acquire_queue_pair(depth_k);
        if (!leased) return leased.status();
        reader.queue_pair_ = std::move(*leased);
        return reader;
    }

    /** Takes @p disk in, reading each run from its own file in @p volume, and leases a pair. */
    template <typename named_volume_type_>
        requires std::same_as<named_volume_type_, volume_type_>
    static expected<ring_sections_reader> make(disk_type_&& disk, named_volume_type_ const& volume) noexcept {
        ring_sections_reader reader;
        reader.disk_ = std::move(disk);
        reader.volume_ = &volume;
        expected<utilities::queue_pair_t> leased = reader.disk_.acquire_queue_pair(depth_k);
        if (!leased) return leased.status();
        reader.queue_pair_ = std::move(*leased);
        return reader;
    }

    ring_sections_reader(ring_sections_reader&& other) noexcept
        : disk_(std::move(other.disk_)), media_(std::move(other.media_)), runs_(std::move(other.runs_)),
          volume_(std::exchange(other.volume_, nullptr)), queue_pair_(std::move(other.queue_pair_)),
          registered_(std::move(other.registered_)) {}
    ring_sections_reader(ring_sections_reader const&) = delete;
    ring_sections_reader& operator=(ring_sections_reader const&) = delete;
    ring_sections_reader& operator=(ring_sections_reader&&) = delete;
    ~ring_sections_reader() noexcept {
        release();
        if (queue_pair_.held()) [[maybe_unused]]
            status_t const released = disk_.release_queue_pair(queue_pair_);
        for (utilities::registered_medium_t& medium : media_)
            if (medium.held()) [[maybe_unused]]
                status_t const detached = disk_.unregister_medium(medium);
    }

    /** Reads every one of @p reads into @p scratch, registering it first when it is new, one batch at a time. */
    st::status_t read(std::span<device_section_read_t const> reads, std::span<byte_t> scratch) noexcept {
        using utilities::completion_t;
        using utilities::request_t;
        if (registered_.held() && registered_.bytes().data() != scratch.data()) release();
        if (!registered_.held()) {
            expected<utilities::registered_memory_t> registered = disk_.register_memory(
                scratch, utilities::memory_space_t::gpu_k);
            if (!registered) return st::status_t::input_output_k;
            registered_ = std::move(*registered);
        }
        std::array<request_t, depth_k> requests {};
        std::array<completion_t, depth_k> completions {};
        std::size_t read = 0;
        std::uint64_t done = 0;
        while (read != reads.size()) {
            std::size_t staged = 0;
            while (staged != depth_k && read != reads.size()) {
                device_section_read_t const& section = reads[read];
                std::uint64_t const left = section.extent.length - done;
                std::uint64_t const length = left < request_bytes_k ? left : request_bytes_k;
                auto [medium, medium_status] = medium_of_(section);
                if (medium_status.failed()) return st::status_t::input_output_k;
                requests[staged] = utilities::read_request(
                    medium, utilities::extent_t {section.extent.offset + done, length},
                    registered_.reference(section.into + done), queue_pair_.token(static_cast<std::uint32_t>(staged)));
                ++staged;
                done += length;
                if (done == section.extent.length) ++read, done = 0;
            }
            for (std::size_t submitted = 0; submitted != staged;) {
                auto [accepted, submit_status] = disk_.submit(
                    queue_pair_, std::span<request_t const> {requests.data() + submitted, staged - submitted});
                if (submit_status.failed()) return st::status_t::input_output_k;
                submitted += accepted;
            }
            auto [completed, poll_status] = utilities::poll_completions_at_least<waiter_type_>(
                disk_, queue_pair_, std::span<completion_t> {completions}, staged);
            if (poll_status.failed() || completed != staged) return st::status_t::input_output_k;
            for (std::size_t index = 0; index != staged; ++index)
                if (!completions[index].completed_in_full()) return st::status_t::input_output_k;
        }
        return st::status_t::success_k;
    }

    /** Deregisters the scratch, which the compactor frees next, and the merge's run files. */
    void release() noexcept {
        if (registered_.held()) [[maybe_unused]]
            status_t const released = disk_.unregister_memory(registered_);
        for (run_medium_t& run : runs_)
            if (run.medium.held()) [[maybe_unused]]
                status_t const detached = disk_.unregister_medium(run.medium);
        runs_.clear();
    }

  private:
    ring_sections_reader() noexcept = default;

    /** One run file a merge reads, registered at its first section. */
    struct run_medium_t {
        utilities::uuid_t run {};
        utilities::registered_medium_t medium;
    };

    /** The medium @p section is read from: its volume's on raw media, else its run's file. */
    expected<utilities::medium_reference_t> medium_of_(device_section_read_t const& section) noexcept {
        if constexpr (!std::is_void_v<volume_type_>)
            if (volume_) {
                // A merge reads a handful of runs, which a scan finds sooner than a table would.
                for (run_medium_t const& known : runs_)
                    if (known.run == section.run) return known.medium.reference();
                char path[utilities::path_max_bytes_k];
                if (status_t const named = volume_->file_path(static_cast<std::uint16_t>(section.volume), section.run,
                                                              path);
                    named.failed())
                    return named;
                utilities::medium_options_t request;
                request.name = path;
                request.access = utilities::medium_access_t::read_only_k;
                expected<utilities::registered_medium_t> registered = disk_.register_medium(request);
                if (!registered) return registered.status();
                if (runs_.push_back(run_medium_t {section.run, std::move(*registered)}) != st::status_t::success_k)
                    return status_t {status_code_t::out_of_memory_k, "The reader could not track another run"};
                return runs_[runs_.size() - 1].medium.reference();
            }
        if (section.volume >= media_.size())
            return status_t {status_code_t::invalid_argument_k, "The section names no volume of this reader"};
        return media_[section.volume].reference();
    }

    /** The ring, owned, and opened for this reader alone. */
    disk_type_ disk_;

    /** The medium of every volume, by volume, on raw media. */
    st::vector<utilities::registered_medium_t> media_;

    /** The run files the current merge read, on a directory volume. */
    st::vector<run_medium_t> runs_;

    /** The directory volume naming the run files, or null on raw media. */
    volume_type_ const* volume_ = nullptr;

    /** The queue pair every batch goes through. */
    utilities::queue_pair_t queue_pair_;

    /** The scratch the reads land in, as the ring registered it. */
    utilities::registered_memory_t registered_;
};

class cuda_compactor_t;

/** The compactor a database's merges run on, picked once at mount by whoever opens it: the calling thread, or a warm
 *  device compactor that opener owns. */
using compactor_ref_t = std::variant<host_compactor_t, cuda_compactor_t*>;

#pragma endregion Device

} // namespace unum::ustore::blobs
