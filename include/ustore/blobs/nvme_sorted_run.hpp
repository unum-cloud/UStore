/**
 *  @file include/ustore/blobs/nvme_sorted_run.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief One immutable run of an LSM level: a header, the index of its key slots, and the values they locate.
 *
 *  A run is one extent of one volume and nothing else, a file of its own on a filesystem or a
 *  stretch of a slot on raw media, so opening it takes the extent a manifest names and every read
 *  afterwards is an extent plus a submit. The index is read into memory at open rather than mapped,
 *  because a namespace has no page cache to map through and a mapped read cannot report a short
 *  transfer, which is exactly what a truncated run looks like.
 *
 *  The sections are disjoint, so the write path needs no ordering between them: it queues the
 *  header, the cells, the value offsets, the values and the trailer of stamps and kinds, or the
 *  sections of a run of strings, into one extent and polls them all; the flush belongs to the
 *  publication of the manifest that names the run. A delta's kind tells a removal, an empty value
 *  and a fold's change apart, so no size or location carries a sentinel.
 *
 *  A cell holds a fixed-width key and nothing of the value it names, which the cell's sorted
 *  position locates: by stride for fixed-width values, and through the value offsets column
 *  for variable-width ones, so the values of any range of positions read as one tape of
 *  offsets and bytes. A lookup descends a saved S+ tree of the keys and lands on the rank,
 *  which is the cell's position. A cell that is its key alone is not saved, as the tree's
 *  leaves hold every key:
 *
 *  @code{.c}
 *  struct sorted_run {                       // every section starts on a 4096-byte boundary
 *      sorted_run_header_t header;           // 120 bytes, format_version = 1, branching = keys per tree row
 *      key_slot cells[n];                    // sorted order, inline values only
 *      splus_tree keys;                      // saved immutable_splus_tree<stored key> over n keys
 *      u32 value_offsets[n + 1];             // variable width only; [0] = 0, [n] = stored_values_bytes
 *      byte_t values[stored_values_bytes];   // sorted order, fixed-width ones found by stride
 *      byte_t stamps[stamps_bytes];          // trailer: the saved stamps column
 *      byte_t kinds[kinds_bytes];            // and the saved kinds column right after it
 *      range_tombstone ranges[t];            // t = range_tombstones_count, ascending by start
 *      u64 range_stamps[t];                  // the raw stamp of each, never raised to the floor
 *  };
 *  @endcode
 *
 *  A range tombstone hides every key from its start up to its exclusive end for versions older than
 *  it, and the run's key bounds cover its tombstones, so a level's runs stay disjoint and a merge
 *  picks every run they reach.
 *
 *  A run of strings files every key under its 16-byte head, its leading 15 bytes and its length,
 *  and searches the heads as an S+ tree saved in the run. Keys are kept whole on an Arrow tape, and
 *  a tie of equal heads wider than one row of the tree gets a layer, an S+ tree over the heads of
 *  the next 15 bytes of its keys, nested as deep as its keys keep tying, so a lookup descends the
 *  heads and the layers and compares at most one row of whole keys:
 *
 *  @code{.c}
 *  struct strings_run {                      // every section starts on a 4096-byte boundary
 *      sorted_run_header_t header;           // 120 bytes, key_tape_bytes and layers_bytes size the rest
 *      splus_tree heads;                     // saved immutable_splus_tree<key128_t> over n heads
 *      uuid128_t values[n];                  // Arrow fixed_size_binary(16), in sorted order
 *      u32 key_offsets[n + 1];               // Arrow string column of the whole keys
 *      char keys[key_tape_bytes];
 *      byte_t stamps[stamps_bytes];          // trailer: the saved stamps column
 *      byte_t kinds[kinds_bytes];            // and the saved kinds column right after it
 *      head_layers_header_t layers_header;   // the layers, layers_bytes in all
 *      head_layer_t layers[layers_count];    // by first position, then by depth
 *      splus_tree layer_heads[layers_count]; // each word-aligned, where its layer names
 *      u64 range_stamps[t];                  // t = range_tombstones_count, ascending by start
 *      u32 range_offsets[2 * t + 1];         // tombstone i spans [2i, 2i + 1) and [2i + 1, 2i + 2) of the tape
 *      char range_tape[];                    // start and end strings, an empty end reaching past the last string
 *  };
 *  @endcode
 *
 *  The value offsets and every section of a run of strings are read into memory at open with
 *  the index, so a lookup compares whole keys with no I/O and a point read of a value held
 *  apart costs one.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <array>       // `std::array`
#include <concepts>    // `std::convertible_to`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::remove_reference_t`
#include <utility>     // `std::declval`, `std::move`

#include <smashtable/basic_vector.hpp>         // `st::vector`
#include <smashtable/immutable_splus_tree.hpp> // `st::immutable_splus_tree`, `st::immutable_splus_tree_view`

#include "ustore/blobs/nvme_compaction.hpp"   // `stamps_column_t`, `kinds_column_t`
#include "ustore/blobs/nvme_key_slot.hpp"     // `key_slot`, `value_layout_t`
#include "ustore/strings/string_keys.hpp"     // `head_of`, `head_prefix_bytes_k`
#include "ustore/utilities/nvme_channels.hpp" // `io_channel_t`, `basic_read_pipeline`
#include "ustore/utilities/nvme_ring.hpp" // `read_request`, `request_t`, `completion_t`, `extent_t`, `portable_alignment_k`
#include "ustore/utilities/nvme_slots.hpp"  // `magic_of`
#include "ustore/utilities/nvme_volume.hpp" // `medium_extent_t`
#include "ustore/utilities/types.hpp"       // `status_t`, `status_code_t`, `expected`, `byte_t`, `uuid128_t`

namespace unum::ustore::blobs {

using utilities::basic_read_pipeline;
using utilities::basic_write_pipeline;
using utilities::completed_buffer_t;
using utilities::completion_t;
using utilities::extent_t;
using utilities::io_channel_t;
using utilities::magic_of;
using utilities::medium_extent_t;
using utilities::portable_alignment_k;
using utilities::request_t;
using utilities::request_tag_t;

#pragma region Vocabulary

/** The word the first eight bytes of a sorted run carry. */
inline constexpr std::uint64_t sorted_run_magic_k = magic_of("USTORERN");

/** The layout the engine writes, which a run of another version is not opened by. */
inline constexpr std::uint32_t sorted_run_format_version_k = 1;

#pragma pack(push, 1)

/** The first bytes of a sorted run, from which every other offset inside it is derived. */
struct sorted_run_header_t {

    /** @c sorted_run_magic_k, so a run is told apart from any other bytes of a medium. */
    std::uint64_t magic = 0;

    /** The layout the rest of the run follows, @c sorted_run_format_version_k for this one. */
    std::uint32_t format_version = 0;

    /** Bytes of one key slot, so a run of another key shape is refused rather than misread. */
    std::uint32_t key_slot_bytes = 0;

    /** Key slots the index holds, padding cells excluded. */
    std::uint64_t key_slots_count = 0;

    /** Bytes of the stored values section, before its alignment padding. */
    std::uint64_t stored_values_bytes = 0;

    /** Largest value the collection that wrote this run admits. */
    std::uint32_t value_max_size = 0;

    /** Cells in one row of the index, which a reader lays the rows out by. */
    std::uint16_t branching = 0;

    /** How the values of this run are stored, which must match the reader's key slot. */
    value_layout_t value_layout = value_layout_t::unknown_k;

    /** The identifier this run is named by, which a directory volume also names its file by. */
    uuid128_t content_id;

    /** Bytes the saved stamps column takes in the trailer, before its alignment padding. */
    std::uint64_t stamps_bytes = 0;

    /** The stamp every stamp of the run was raised to at least, which its column counts from. */
    std::uint64_t stamps_floor = 0;

    /** The newest stamp any key slot of the run carries, or the floor for a run of none. */
    std::uint64_t newest_stamp = 0;

    /** Bytes the saved kinds column takes in the trailer, right after the stamps and before their alignment padding. */
    std::uint64_t kinds_bytes = 0;

    /** Bytes of the key tape, every key of a run of strings whole, zero for a run
     *  of fixed-width keys. */
    std::uint32_t key_tape_bytes = 0;

    /** Bytes of the layers section of a run of strings, before its alignment padding, zero
     *  for fixed-width keys. */
    std::uint32_t layers_bytes = 0;

    /** The collection that wrote the run, so a run found without a manifest goes back to it. */
    uuid128_t collection;

    /** Range tombstones the section at the end of the run holds. */
    std::uint32_t range_tombstones_count = 0;

    /** Bytes that section takes, before its alignment padding. */
    std::uint32_t range_tombstones_bytes = 0;
};

#pragma pack(pop)

static_assert(sizeof(sorted_run_header_t) == 120, "A run's header is 120 packed bytes");
static_assert(offsetof(sorted_run_header_t, key_slots_count) == 16 && offsetof(sorted_run_header_t, content_id) == 40 &&
                  offsetof(sorted_run_header_t, key_tape_bytes) == 88 &&
                  offsetof(sorted_run_header_t, layers_bytes) == 92 &&
                  offsetof(sorted_run_header_t, collection) == 96 &&
                  offsetof(sorted_run_header_t, range_tombstones_count) == 112 &&
                  offsetof(sorted_run_header_t, range_tombstones_bytes) == 116,
              "A run's header keeps its field offsets");

/** Where one key slot's value sits inside its volume, as an unbuffered read has to ask for it. */
struct value_window_t {

    /** The volume-absolute range a read covers, empty for a value that needs none. */
    extent_t extent;

    /** Bytes of that range before the value's first byte. */
    std::uint32_t skipped = 0;

    /** Bytes the value itself occupies. */
    std::uint32_t bytes = 0;
};

/**
 *  @brief Where the values of a range of sorted positions sit inside their volume, with the value offsets locating
 *      them, as two unbuffered reads ask for them: an Arrow strings column and a StringZilla tape once read.
 *
 *  A whole run reads with no rebase, as its offsets start at zero; a slice subtracts @c first_offset from each.
 */
struct value_tape_window_t {

    /** The volume-absolute pages covering the value offsets of the range and the one past it, empty for a run
     *  finding its values by stride. */
    extent_t offsets;

    /** The volume-absolute pages covering the range's values, empty for a range of no bytes. */
    extent_t values;

    /** Bytes of the offsets pages before the range's first offset. */
    std::uint32_t offsets_skipped = 0;

    /** Bytes of the values pages before the range's first byte. */
    std::uint32_t values_skipped = 0;

    /** Where the range's first value starts inside the run's values section, subtracted when rebasing. */
    std::uint32_t first_offset = 0;

    /** Bytes the range's values occupy. */
    std::uint32_t bytes = 0;
};

static_assert(sizeof(value_tape_window_t) == 48 && offsetof(value_tape_window_t, values) == 16 &&
                  offsetof(value_tape_window_t, offsets_skipped) == 32 &&
                  offsetof(value_tape_window_t, first_offset) == 40,
              "A value tape window is two extents and four words");

/** One tie of equal heads in a run of strings wider than one row of the heads, searched by the
 *  heads of the next bytes of its keys. */
struct head_layer_t {

    /** The first sorted position of the tie. */
    std::uint64_t first = 0;

    /** Keys the tie holds. */
    std::uint32_t count = 0;

    /** How many heads deep its own heads start: one for the bytes past the first head, two
     *  past the second. */
    std::uint32_t depth = 0;

    /** Where the saved S+ tree of its heads starts, past the layers list. */
    std::uint64_t heads_offset = 0;

    /** Bytes that saved tree takes. */
    std::uint64_t heads_bytes = 0;
};

static_assert(sizeof(head_layer_t) == 32, "A head layer is 32 bytes");

/** What the layers section of a run of strings opens with. */
struct head_layers_header_t {

    /** Layers the list right after holds. */
    std::uint64_t layers_count = 0;

    /** Bytes the saved trees of every layer take together, right after the list. */
    std::uint64_t trees_bytes = 0;
};

/** The sorted positions @b [first,last) of a run that a key's heads alone leave
 *  to compare whole. */
struct key_positions_t {

    /** The first position. */
    std::uint64_t first = 0;

    /** One past the last position. */
    std::uint64_t last = 0;
};

/** The sections of a run of strings laid out in memory before they are written, none for a run
 *  of fixed keys. */
struct strings_sections_t {

    /** The saved S+ tree of the heads. */
    st::vector<std::uint64_t> heads;

    /** One 16-byte value per key in sorted order. */
    st::vector<byte_t> values;

    /** One offset per key into @c keys and one past the last. */
    st::vector<std::uint32_t> key_offsets;

    /** Every key whole, back to back. */
    st::vector<byte_t> keys;

    /** The layers section: its header, the list, and the saved trees. */
    st::vector<std::uint64_t> layers;
};

/** A run's extent allocated, with the header and the columns it will carry laid out:
 *  what @c reserve hands @c write_sections and then @c open, so only the first needs
 *  the maintenance claim. */
struct sorted_run_reservation_t {

    /** The header the run will carry, from which every offset of its sections follows. */
    sorted_run_header_t header {};

    /** The extent allocated for the run, durable once a manifest naming it is published. */
    medium_extent_t extent;

    /** The stamps the trailer saves, packed from the header's floor. */
    stamps_column_t stamps;

    /** The kinds the trailer saves right after the stamps. */
    kinds_column_t kinds;

    /** The sections of a run of strings, laid out to the bytes they are written as. */
    strings_sections_t strings;

    /** The saved S+ tree of a run of fixed-width keys. */
    st::vector<std::uint64_t> keys;

    /** The range tombstones section, laid out as written, none for a run without any. */
    st::vector<byte_t> range_tombstones;
};

#pragma endregion Vocabulary

#pragma region Layout

/**
 *  @brief Where the header, the index, the value offsets, the stored values and the trailer of a
 *      sorted run of fixed-width keys sit inside its extent, each section on a page boundary.
 *
 *  Every offset follows from the counts the header carries, so a header never carries the offsets, and a reader
 *  cannot be told a position its own arithmetic disagrees with.
 */
template <typename key_slot_type_>
class sorted_run_layout {
  public:

    /** The key slot the cells of the index hold. */
    using key_slot_t = key_slot_type_;

    /** How the values of a run of these key slots are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so the run carries no values section. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

    /** Whether the run locates its values through a value offsets column, which only variable widths need. */
    static constexpr bool carries_value_offsets_k = is_variable_width(value_layout_k);

    /** Whether the run saves its cells, needed for inline values alone; the tree holds the keys. */
    static constexpr bool keeps_cells_k = holds_values_inline_k;

    /** The saved S+ tree of the keys, searched in place. */
    using keys_view_t = st::immutable_splus_tree_view<typename key_slot_t::stored_key_t>;

    /** Keys one row of the saved tree holds, which the header records. */
    static constexpr std::uint64_t branching_k = keys_view_t::keys_per_row_k;

    /** Bytes the header occupies, padded out to what an unbuffered read accepts. */
    [[nodiscard]] static constexpr std::uint64_t header_bytes() noexcept {
        return round_up_to_multiple(sizeof(sorted_run_header_t), portable_alignment_k);
    }

    /** Bytes the cells of @p key_slots_count key slots occupy, before the
     *  section's alignment padding. */
    [[nodiscard]] static constexpr std::uint64_t index_bytes(std::uint64_t key_slots_count) noexcept {
        return keeps_cells_k ? key_slots_count * sizeof(key_slot_t) : 0;
    }

    /** Bytes the index section occupies, which one read of it covers whole. */
    [[nodiscard]] static constexpr std::uint64_t index_section_bytes(std::uint64_t key_slots_count) noexcept {
        return round_up_to_multiple(index_bytes(key_slots_count), portable_alignment_k);
    }

    /** First byte of the index inside the extent. */
    [[nodiscard]] static constexpr std::uint64_t index_offset() noexcept { return header_bytes(); }

    /** Bytes the value offsets take: one per key slot and one past the last, none without the column. */
    [[nodiscard]] static constexpr std::uint64_t value_offsets_bytes(std::uint64_t key_slots_count) noexcept {
        return carries_value_offsets_k ? (key_slots_count + 1) * sizeof(stored_value_offset_t) : 0;
    }

    /** Bytes the saved S+ tree of @p key_slots_count keys takes. */
    [[nodiscard]] static constexpr std::uint64_t keys_saved_bytes(std::uint64_t key_slots_count) noexcept {
        return sizeof(st::splus_tree_header_t) + keys_view_t::size_bytes(static_cast<std::size_t>(key_slots_count));
    }

    /** First byte of the saved tree of the keys, right after the cells. */
    [[nodiscard]] static constexpr std::uint64_t keys_offset(std::uint64_t key_slots_count) noexcept {
        return index_offset() + index_section_bytes(key_slots_count);
    }

    /** Where the leaf rows of the saved tree of @p key_slots_count keys start in its section: the
     *  last rows of the saved form, holding every key in sorted order. */
    [[nodiscard]] static constexpr std::uint64_t keys_leaves_offset(std::uint64_t key_slots_count) noexcept {
        std::uint64_t const leaves = divide_round_up(key_slots_count, branching_k);
        return keys_saved_bytes(key_slots_count) - leaves * keys_view_t::format_t::bytes_per_row_k;
    }

    /** Bytes the section of the saved tree of the keys occupies. */
    [[nodiscard]] static constexpr std::uint64_t keys_section_bytes(std::uint64_t key_slots_count) noexcept {
        return round_up_to_multiple(keys_saved_bytes(key_slots_count), portable_alignment_k);
    }

    /** First byte of the value offsets inside the extent, right after the saved tree
     *  of the keys. */
    [[nodiscard]] static constexpr std::uint64_t value_offsets_offset(std::uint64_t key_slots_count) noexcept {
        return keys_offset(key_slots_count) + keys_section_bytes(key_slots_count);
    }

    /** First byte of the stored values inside the extent, right after the value
     *  offsets locating them. */
    [[nodiscard]] static constexpr std::uint64_t values_offset(std::uint64_t key_slots_count) noexcept {
        return value_offsets_offset(key_slots_count) +
               round_up_to_multiple(value_offsets_bytes(key_slots_count), portable_alignment_k);
    }

    /** Bytes the stored values section occupies, which is none for values held inline. */
    [[nodiscard]] static constexpr std::uint64_t values_section_bytes(std::uint64_t stored_values_bytes) noexcept {
        return holds_values_inline_k ? 0 : round_up_to_multiple(stored_values_bytes, portable_alignment_k);
    }

    /** First byte of the trailer, which holds the saved stamps column after the stored values. */
    [[nodiscard]] static constexpr std::uint64_t stamps_offset(std::uint64_t key_slots_count,
                                                               std::uint64_t stored_values_bytes) noexcept {
        return values_offset(key_slots_count) + values_section_bytes(stored_values_bytes);
    }

    /** Bytes the trailer occupies: the saved stamps, then the saved kinds right after them, which
     *  are read at any alignment, so both share one read and the pages of one. */
    [[nodiscard]] static constexpr std::uint64_t trailer_bytes(std::uint64_t stamps_bytes,
                                                               std::uint64_t kinds_bytes) noexcept {
        return round_up_to_multiple(stamps_bytes + kinds_bytes, portable_alignment_k);
    }

    /** Bytes the whole run occupies but its range tombstones, which is the length the extent of a
     *  run of none is allocated with. */
    [[nodiscard]] static constexpr std::uint64_t blob_bytes(std::uint64_t key_slots_count,
                                                            std::uint64_t stored_values_bytes,
                                                            std::uint64_t stamps_bytes,
                                                            std::uint64_t kinds_bytes) noexcept {
        return stamps_offset(key_slots_count, stored_values_bytes) + trailer_bytes(stamps_bytes, kinds_bytes);
    }

    /** Bytes @p count range tombstones take after the trailer, their bounds and then their stamps,
     *  before the section's padding. */
    [[nodiscard]] static constexpr std::uint64_t range_tombstones_bytes(std::uint64_t count) noexcept {
        return count * (sizeof(range_tombstone<typename key_slot_t::stored_key_t>) + sizeof(std::uint64_t));
    }

    /** First byte of the range tombstones of the run @p header opens, right after its trailer. */
    [[nodiscard]] static constexpr std::uint64_t range_tombstones_offset(sorted_run_header_t const& header) noexcept {
        return blob_bytes(header.key_slots_count, header.stored_values_bytes, header.stamps_bytes, header.kinds_bytes);
    }

    /** Bytes the whole run @p header opens occupies, its range tombstones included, which a walk
     *  over raw slots steps by. */
    [[nodiscard]] static constexpr std::uint64_t blob_bytes(sorted_run_header_t const& header) noexcept {
        return range_tombstones_offset(header) +
               round_up_to_multiple(header.range_tombstones_bytes, portable_alignment_k);
    }
};

/** Where the header, the heads, the values, the key tape, the trailer and the head layers of a
 *  run of strings sit inside its extent, each section on a page boundary and the layers running
 *  to the extent's end. */
template <typename key_slot_type_>
class strings_run_layout {
  public:

    /** The key slot a run of strings is written from: a head and the 16-byte value it maps onto. */
    using key_slot_t = key_slot_type_;

    /** The heads of a run searched in place, as the run saved them. */
    using heads_view_t = st::immutable_splus_tree_view<st::key128_t>;

    /** Keys one row of the heads holds, which a tie of equal heads outgrows to earn a layer. */
    static constexpr std::uint64_t keys_per_row_k = heads_view_t::keys_per_row_k;

    /** Bytes of one value. */
    static constexpr std::uint64_t value_bytes_k = key_slot_t::size;

    /** Bytes the header occupies, padded out to what an unbuffered read accepts. */
    [[nodiscard]] static constexpr std::uint64_t header_bytes() noexcept {
        return round_up_to_multiple(sizeof(sorted_run_header_t), portable_alignment_k);
    }

    /** Bytes the saved S+ tree of @p count heads takes. */
    [[nodiscard]] static constexpr std::uint64_t heads_saved_bytes(std::uint64_t count) noexcept {
        return sizeof(st::splus_tree_header_t) + heads_view_t::size_bytes(static_cast<std::size_t>(count));
    }

    /** First byte of the heads inside the extent. */
    [[nodiscard]] static constexpr std::uint64_t heads_offset() noexcept { return header_bytes(); }

    /** First byte of the values, right after the heads. */
    [[nodiscard]] static constexpr std::uint64_t values_offset(std::uint64_t count) noexcept {
        return heads_offset() + round_up_to_multiple(heads_saved_bytes(count), portable_alignment_k);
    }

    /** First byte of the key offsets, right after the values. */
    [[nodiscard]] static constexpr std::uint64_t key_offsets_offset(std::uint64_t count) noexcept {
        return values_offset(count) + round_up_to_multiple(count * value_bytes_k, portable_alignment_k);
    }

    /** First byte of the key tape, right after the offsets locating it. */
    [[nodiscard]] static constexpr std::uint64_t keys_offset(std::uint64_t count) noexcept {
        return key_offsets_offset(count) +
               round_up_to_multiple((count + 1) * sizeof(std::uint32_t), portable_alignment_k);
    }

    /** First byte of the trailer, right after the key tape. */
    [[nodiscard]] static constexpr std::uint64_t stamps_offset(std::uint64_t count, std::uint64_t keys_bytes) noexcept {
        return keys_offset(count) + round_up_to_multiple(keys_bytes, portable_alignment_k);
    }

    /** Bytes the trailer occupies: the saved stamps, then the saved kinds right after them. */
    [[nodiscard]] static constexpr std::uint64_t trailer_bytes(std::uint64_t stamps_bytes,
                                                               std::uint64_t kinds_bytes) noexcept {
        return round_up_to_multiple(stamps_bytes + kinds_bytes, portable_alignment_k);
    }

    /** First byte of the layers section, right after the trailer. */
    [[nodiscard]] static constexpr std::uint64_t layers_offset(std::uint64_t count, std::uint64_t keys_bytes,
                                                               std::uint64_t stamps_bytes,
                                                               std::uint64_t kinds_bytes) noexcept {
        return stamps_offset(count, keys_bytes) + trailer_bytes(stamps_bytes, kinds_bytes);
    }

    /** Bytes the whole run occupies, its layers section taking @p layers_bytes. */
    [[nodiscard]] static constexpr std::uint64_t blob_bytes(std::uint64_t count, std::uint64_t keys_bytes,
                                                            std::uint64_t stamps_bytes, std::uint64_t kinds_bytes,
                                                            std::uint64_t layers_bytes) noexcept {
        return layers_offset(count, keys_bytes, stamps_bytes, kinds_bytes) +
               round_up_to_multiple(layers_bytes, portable_alignment_k);
    }

    /** First byte of the range tombstones of the run @p header opens, right after its layers. */
    [[nodiscard]] static constexpr std::uint64_t range_tombstones_offset(sorted_run_header_t const& header) noexcept {
        return blob_bytes(header.key_slots_count, header.key_tape_bytes, header.stamps_bytes, header.kinds_bytes,
                          header.layers_bytes);
    }

    /** Bytes the whole run @p header opens occupies, its range tombstones included, which a walk
     *  over raw slots steps by. */
    [[nodiscard]] static constexpr std::uint64_t blob_bytes(sorted_run_header_t const& header) noexcept {
        return range_tombstones_offset(header) +
               round_up_to_multiple(header.range_tombstones_bytes, portable_alignment_k);
    }
};

/**
 *  @brief Packs @p stamps, each raised to @p floor, into the column a run saves in its trailer.
 *  @param[in] stamps The stamps of the run's key slots.
 *  @param[in] floor The stamp each is raised to at least.
 *  @param[out] newest The newest stamp packed, or @p floor for none.
 */
inline expected<stamps_column_t> pack_stamps(std::span<std::uint64_t const> stamps, std::uint64_t floor,
                                             std::uint64_t& newest) noexcept {
    newest = floor;
    for (std::uint64_t const stamp : stamps)
        if (stamp > newest) newest = stamp;
    std::array<std::uint64_t, 64> raised {};
    auto fill = [&](std::size_t position) noexcept {
        std::size_t const count = fu::min_of(raised.size(), stamps.size() - position);
        for (std::size_t index = 0; index != count; ++index)
            raised[index] = fu::max_of(stamps[position + index], floor);
        return st::expected<std::span<std::uint64_t const>>(std::span<std::uint64_t const> {raised.data(), count},
                                                            st::status_t::success_k);
    };
    st::expected<stamps_column_t> packed = stamps_column_t::make(stamps.size(), floor, newest, fill);
    if (!packed) return status_t {status_code_t::out_of_memory_k, "No room to pack a run's stamps"};
    return std::move(*packed);
}

/** Packs @p kinds, each a @c delta_kind_t, into the column a run saves in its trailer after the stamps, from the least
 *  kind present, so a run of writes alone spends no bits on it. */
inline expected<kinds_column_t> pack_kinds(std::span<std::uint8_t const> kinds) noexcept {
    st::expected<kinds_column_t> packed = kinds_column_t::make(kinds);
    if (!packed) return status_t {status_code_t::out_of_memory_k, "No room to pack a run's kinds"};
    return std::move(*packed);
}

#pragma endregion Layout

#pragma region Sorted Run

/** Values a run write takes piecewise: @c size bytes in all, @c gather filling @c into with the
 *  bytes from @c from on, so a flush copies each value once, from where its memory runs hold it
 *  straight into a write block. */
template <typename values_type_>
concept gathered_values_concept = requires(values_type_ const& values, std::uint64_t from, std::span<byte_t> into) {
    { values.size() } noexcept -> std::convertible_to<std::uint64_t>;
    { values.gather(from, into) } noexcept;
};

/**
 *  @brief One immutable run of key slots and the values they locate, held as one extent
 *      of one volume.
 *
 *  The run borrows the volume, and owns only its index, its columns, and for strings its key
 *  tape and layers. Every call that moves bytes takes the channel the calling thread holds,
 *  since a queue pair belongs to one thread while it is held and a run outlives every thread
 *  that ever reads it.
 *
 *  @tparam waiter_type_ What the core does while it waits for an index read or a barrier.
 *  @tparam row_kit_type_ The SmashTable kit the run's trees search with.
 */
template <typename key_slot_type_, typename volume_type_, typename waiter_type_,
          typename row_kit_type_ = st::native_row_kit_t>
class lsm_sorted_run {
  public:

#pragma region Types

    /** The key slot the cells of the index hold. */
    using key_slot_t = key_slot_type_;

    /** The volume the run's extent lives on. */
    using volume_t = volume_type_;

    /** The key the run orders its key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The range tombstones the run carries at its end, over whole strings for a run of strings. */
    using range_tombstones_t =
        std::conditional_t<tailed_key_slot_concept<key_slot_t>, blobs::range_tombstones<std::string_view>,
                           blobs::range_tombstones<stored_key_t>>;

    /** The transport the volume moves bytes through. */
    using disk_t = typename volume_type_::disk_t;

    /** Where the sections of this run sit inside its extent. */
    using sorted_run_layout_t = sorted_run_layout<key_slot_t>;

    /** The pipeline every read of this run refills. */
    using read_pipeline_t = basic_read_pipeline<disk_t>;

    /** One block of one value's read in flight, filed in its channel's ring under its tag. */
    using read_piece_t = io_channel_t::read_piece_t;

    /** The pipeline the write path stages its three sections through. */
    using write_pipeline_t = basic_write_pipeline<disk_t, waiter_type_>;

    /** How the values of this run are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so reading one needs no I/O. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

    /** Whether the run locates its values through a value offsets column, which only variable widths need. */
    static constexpr bool carries_value_offsets_k = sorted_run_layout_t::carries_value_offsets_k;

    /** Whether the run keeps its cells, needed for inline values alone; the tree holds the keys. */
    static constexpr bool keeps_cells_k = sorted_run_layout_t::keeps_cells_k;

    /** Whether the run holds strings: heads in an S+ tree, whole keys on a tape and layers
     *  under wide ties. */
    static constexpr bool tailed_k = tailed_key_slot_concept<key_slot_t>;

    /** Where the sections of a run of strings sit inside its extent, which only a run of
     *  strings instantiates. */
    using strings_layout_t = strings_run_layout<key_slot_t>;

    /** The SmashTable kit every search of the run's trees runs, which the saved bytes
     *  never depend on. */
    using row_kit_t = row_kit_type_;

    /** The heads of a run of strings, or of one of its layers, searched in place. */
    using heads_view_t =
        st::immutable_splus_tree_view<st::key128_t, st::keys_per_row<st::key128_t>(st::default_row_bytes_k), row_kit_t>;

    /** The saved S+ tree every lookup descends: of the keys, or of a run of strings' heads. */
    using keys_view_t =
        st::immutable_splus_tree_view<stored_key_t, st::keys_per_row<stored_key_t>(st::default_row_bytes_k), row_kit_t>;

#pragma endregion Types

#pragma region Construction

    lsm_sorted_run() noexcept = default;
    lsm_sorted_run(lsm_sorted_run const&) = delete;
    lsm_sorted_run& operator=(lsm_sorted_run const&) = delete;

    lsm_sorted_run(lsm_sorted_run&& other) noexcept
        : volume_(std::exchange(other.volume_, nullptr)), extent_(other.extent_), header_(other.header_),
          cells_(std::move(other.cells_)), value_offsets_(std::move(other.value_offsets_)),
          stamps_(std::move(other.stamps_)), kinds_(std::move(other.kinds_)), tree_words_(std::move(other.tree_words_)),
          tree_(std::exchange(other.tree_, keys_view_t {})), values_(std::move(other.values_)),
          key_offsets_(std::move(other.key_offsets_)), keys_(std::move(other.keys_)), layers_(std::move(other.layers_)),
          layer_words_(std::move(other.layer_words_)), assigned_stamp_(std::exchange(other.assigned_stamp_, 0)),
          range_tombstones_(std::move(other.range_tombstones_)) {
        other.extent_ = medium_extent_t {};
        other.header_ = sorted_run_header_t {};
    }

    lsm_sorted_run& operator=(lsm_sorted_run&& other) noexcept {
        if (this == &other) return *this;
        volume_ = std::exchange(other.volume_, nullptr);
        extent_ = other.extent_;
        header_ = other.header_;
        cells_ = std::move(other.cells_);
        value_offsets_ = std::move(other.value_offsets_);
        stamps_ = std::move(other.stamps_);
        kinds_ = std::move(other.kinds_);
        tree_words_ = std::move(other.tree_words_);
        tree_ = std::exchange(other.tree_, keys_view_t {});
        values_ = std::move(other.values_);
        key_offsets_ = std::move(other.key_offsets_);
        keys_ = std::move(other.keys_);
        layers_ = std::move(other.layers_);
        layer_words_ = std::move(other.layer_words_);
        assigned_stamp_ = std::exchange(other.assigned_stamp_, 0);
        range_tombstones_ = std::move(other.range_tombstones_);
        other.extent_ = medium_extent_t {};
        other.header_ = sorted_run_header_t {};
        return *this;
    }

#pragma endregion Construction

#pragma region Pool Fields

    /** Steps each time the run pool vacates this record, so a stale run handle
     *  resolves to nothing. */
    std::uint16_t generation = 0;

    /** Level sets of its tree that name this run, at most two; the run is dropped when the
     *  last one stops. */
    std::uint16_t level_sets_naming = 0;

    /** The identifier this run was written under. */
    [[nodiscard]] uuid128_t content_id() const noexcept { return header_.content_id; }

    /** The collection that wrote this run. */
    [[nodiscard]] uuid128_t collection() const noexcept { return header_.collection; }

#pragma endregion Pool Fields

#pragma region Setup and Teardown

    /**
     *  @brief Writes a whole run into a fresh extent of @p volume, which is @c reserve,
     *      @c write_sections and @c open in a row; the run is durable once a manifest naming
     *      it is published.
     *  @param[in,out] volume The volume the extent is allocated on, borrowed and outliving the run.
     *  @param[in,out] channel The channel the sections are queued on, held by the calling thread.
     *  @param[in] collection The collection writing the run, which its header keeps.
     *  @param[in] content_id The identifier the run is named by, which no other run carries.
     *  @param[in] key_slots The key slots in ascending order of their keys, the versions of one key newest first, which
     *      the index is laid from.
     *  @param[in] stamps The stamp of every key slot, in the same order.
     *  @param[in] kinds The @c delta_kind_t of every key slot, in the same order.
     *  @param[in] floor The stamp every stamp is raised to at least, since no reader can tell those below it apart.
     *  @param[in] stored_values The values back to back in sorted order, which for a fixed-width
     *      run are @p value_max_size bytes per key slot, removals included, as one span or as a
     *      @c gathered_values_concept that gathers them a block at a time.
     *  @param[in] value_offsets Where each key slot's value starts inside @p stored_values and one past the last, for
     *      a variable-width run alone; none otherwise.
     *  @param[in] value_max_size The largest value the writing collection admits, which is the width of every value
     *      of a fixed-width run, kept for a reader.
     *  @param[in] tails The tails of the keys the key slots' heads cut short, in the same order, which key slots of
     *      one head order by; none for a run of whole keys.
     *  @param[in] tombstones The range tombstones the run carries, none for a run of strings.
     *  @return The run, opened again from the volume, or the status that stopped the write with its
     *      extent given back.
     */
    template <typename values_type_ = std::span<byte_t const>>
    static expected<lsm_sorted_run> write(volume_type_& volume, io_channel_t& channel, uuid128_t const& collection,
                                          uuid128_t const& content_id, std::span<key_slot_t const> key_slots,
                                          std::span<std::uint64_t const> stamps, std::span<std::uint8_t const> kinds,
                                          std::uint64_t floor, values_type_ const& stored_values,
                                          std::span<stored_value_offset_t const> value_offsets,
                                          stored_value_size_t value_max_size, key_tails_column_t const& tails = {},
                                          range_tombstones_t const& tombstones = range_tombstones_t {}) noexcept {
        auto [reservation, reserve_status] = reserve(volume, channel, collection, content_id, key_slots, stamps, kinds,
                                                     floor, stored_values, value_offsets, value_max_size, tails,
                                                     tombstones);
        if (reserve_status.failed()) return reserve_status;
        status_t const written = write_sections(volume, channel, reservation, key_slots, stored_values, value_offsets);
        expected<lsm_sorted_run> opened = written.failed() ? expected<lsm_sorted_run> {written}
                                                           : open(volume, channel, reservation.extent);
        if (!opened) volume.deallocate(reservation.extent);
        return opened;
    }

    /**
     *  @brief Checks a run, lays out its header and columns and allocates its extent on @p volume,
     *      which changes the volume's placement and so runs under the maintenance claim; takes what
     *      @c write takes.
     *  @return The reservation @c write_sections fills, or the status that refused the run with nothing reserved.
     */
    template <typename values_type_ = std::span<byte_t const>>
    static expected<sorted_run_reservation_t> reserve(
        volume_type_& volume, io_channel_t& channel, uuid128_t const& collection, uuid128_t const& content_id,
        std::span<key_slot_t const> key_slots, std::span<std::uint64_t const> stamps,
        std::span<std::uint8_t const> kinds, std::uint64_t floor, values_type_ const& stored_values,
        std::span<stored_value_offset_t const> value_offsets, stored_value_size_t value_max_size,
        key_tails_column_t const& tails = {}, range_tombstones_t const& tombstones = range_tombstones_t {}) noexcept {
        if (!content_id.is_valid())
            return status_t {status_code_t::invalid_argument_k, "A run is named by a valid identifier"};
        if (status_t const checked = check_values_(stored_values.size(), value_offsets, key_slots.size(),
                                                   value_max_size);
            checked.failed())
            return checked;
        if (stamps.size() != key_slots.size() || kinds.size() != key_slots.size())
            return status_t {status_code_t::invalid_argument_k, "A run takes one stamp and one kind per key slot"};
        if (status_t const checked = check_tails_(tails, key_slots.size()); checked.failed()) return checked;
        for (std::size_t index = 1; index < key_slots.size(); ++index) {
            int const order = key_order_(key_slots, tails, index);
            if (order < 0)
                return status_t {status_code_t::invalid_argument_k, "A run takes its key slots in ascending order"};
            if (order == 0 && stamps[index] >= stamps[index - 1])
                return status_t {status_code_t::invalid_argument_k, "A run takes the versions of a key newest first"};
        }

        sorted_run_reservation_t reservation;
        std::uint64_t newest = floor;
        auto [column, pack_status] = pack_stamps(stamps, floor, newest);
        if (pack_status.failed()) return pack_status;
        reservation.stamps = std::move(column);
        auto [kinds_column, pack_kinds_status] = pack_kinds(kinds);
        if (pack_kinds_status.failed()) return pack_kinds_status;
        reservation.kinds = std::move(kinds_column);
        if (block_bytes_of_(channel) < sorted_run_layout_t::header_bytes())
            return status_t {status_code_t::capacity_exhausted_k, "A channel's block is shorter than a run's header"};

        sorted_run_header_t& header = reservation.header;
        header.magic = sorted_run_magic_k;
        header.format_version = sorted_run_format_version_k;
        header.key_slot_bytes = static_cast<std::uint32_t>(sizeof(key_slot_t));
        header.key_slots_count = key_slots.size();
        header.stored_values_bytes = stored_values.size();
        header.value_max_size = value_max_size;
        header.branching = static_cast<std::uint16_t>(sorted_run_layout_t::branching_k);
        header.value_layout = value_layout_k;
        header.content_id = content_id;
        header.stamps_bytes = reservation.stamps.saved_bytes();
        header.stamps_floor = floor;
        header.newest_stamp = newest;
        header.kinds_bytes = reservation.kinds.saved_bytes();
        header.collection = collection;
        if (status_t const laid = lay_out_range_tombstones(tombstones, reservation.range_tombstones); laid.failed())
            return laid;
        if (reservation.range_tombstones.size() > (std::numeric_limits<std::uint32_t>::max)())
            return status_t {status_code_t::capacity_exhausted_k, "A run's range tombstones outgrow its header"};
        header.range_tombstones_count = static_cast<std::uint32_t>(tombstones.size());
        header.range_tombstones_bytes = static_cast<std::uint32_t>(reservation.range_tombstones.size());

        std::uint64_t reserved_bytes = 0;
        if constexpr (tailed_k) {
            if (status_t const laid = lay_out_strings_(key_slots, tails, reservation.strings); laid.failed())
                return laid;
            header.branching = static_cast<std::uint16_t>(strings_layout_t::keys_per_row_k);
            std::uint64_t const layers_bytes = reservation.strings.layers.size() * sizeof(std::uint64_t);
            if (reservation.strings.keys.size() > (std::numeric_limits<std::uint32_t>::max)() ||
                layers_bytes > (std::numeric_limits<std::uint32_t>::max)())
                return status_t {status_code_t::capacity_exhausted_k,
                                 "A run of strings outgrows what its header counts"};
            header.key_tape_bytes = static_cast<std::uint32_t>(reservation.strings.keys.size());
            header.layers_bytes = static_cast<std::uint32_t>(layers_bytes);
            reserved_bytes = strings_layout_t::blob_bytes(header);
        }
        else {
            if (status_t const saved = save_keys(key_slots, reservation.keys); saved.failed()) return saved;
            reserved_bytes = sorted_run_layout_t::blob_bytes(header);
        }
        auto [allocated, allocate_status] = volume.allocate(content_id, reserved_bytes);
        if (allocate_status.failed()) return allocate_status;
        reservation.extent = allocated;
        return reservation;
    }

    /**
     *  @brief Stages every section of a run into the extent @p reservation holds and drains them.
     *
     *  The sections are the header, the cells, the value offsets, the values and the trailer, or
     *  those of a run of strings. Writing them touches no placement and so needs no claim; the
     *  flush belongs to the publication of the manifest naming the run.
     *
     *  @param[in,out] channel The channel the sections are queued on.
     *  @param[in] reservation What @c reserve answered for the same key slots, values and tails.
     *  @return Success, or the status that stopped the write, after which the caller
     *      deallocates the extent.
     */
    template <typename values_type_ = std::span<byte_t const>>
    static status_t write_sections(volume_type_& volume, io_channel_t& channel,
                                   sorted_run_reservation_t const& reservation, std::span<key_slot_t const> key_slots,
                                   values_type_ const& stored_values,
                                   std::span<stored_value_offset_t const> value_offsets) noexcept {
        std::uint64_t const block_bytes = block_bytes_of_(channel);
        if (block_bytes < sorted_run_layout_t::header_bytes())
            return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than a run's header"};
        medium_extent_t const& extent = reservation.extent;
        sorted_run_header_t const& header = reservation.header;
        auto [writer_channel, writer_channel_status] = channel.acquire();
        if (writer_channel_status.failed()) return writer_channel_status;
        write_pipeline_t writer {volume.disk(), std::move(writer_channel), extent.medium};

        std::uint64_t const head_bytes = sorted_run_layout_t::header_bytes();
        auto [head, acquire_buffer_or_poll_status] = writer.acquire_buffer_or_poll();
        if (acquire_buffer_or_poll_status.failed()) return acquire_buffer_or_poll_status;
        std::memset(head.bytes().data(), 0, static_cast<std::size_t>(head_bytes));
        std::memcpy(head.bytes().data(), &header, sizeof(header));
        if (status_t const queued = writer.enqueue_write(std::move(head), extent.slice(0, head_bytes)); queued.failed())
            return queued;
        if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;

        if constexpr (tailed_k) {
            if (status_t const written = write_strings_(writer, extent, header, reservation.strings); written.failed())
                return written;
            if (status_t const trailer = write_trailer(writer, extent, header, reservation.stamps, reservation.kinds);
                trailer.failed())
                return trailer;
            if (status_t const ranges = write_range_tombstones(writer, extent, header,
                                                               std::span<byte_t const> {reservation.range_tombstones});
                ranges.failed())
                return ranges;
            return writer.poll_all_completions();
        }

        if (status_t const index = write_index(writer, extent, header, key_slots,
                                               std::span<std::uint64_t const> {reservation.keys});
            index.failed())
            return index;

        if (status_t const offsets = write_value_offsets(writer, extent, header, value_offsets); offsets.failed())
            return offsets;
        if (status_t const values = write_bytes_(
                writer, extent, sorted_run_layout_t::values_offset(header.key_slots_count),
                sorted_run_layout_t::values_section_bytes(header.stored_values_bytes), stored_values);
            values.failed())
            return values;
        if (status_t const trailer = write_trailer(writer, extent, header, reservation.stamps, reservation.kinds);
            trailer.failed())
            return trailer;
        if (status_t const ranges = write_range_tombstones(writer, extent, header,
                                                           std::span<byte_t const> {reservation.range_tombstones});
            ranges.failed())
            return ranges;
        return writer.poll_all_completions();
    }

    /**
     *  @brief Reads the header, the whole index, the value offsets and the trailer of the run
     *      at @p extent, or every section of a run of strings, which is everything a run
     *      keeps in memory.
     *  @param[in,out] volume The volume the run reads through afterwards, borrowed and outliving it.
     *  @param[in,out] channel The channel the header and the index are read on, held by the calling thread.
     *  @param[in] extent Where the run lies, as an allocation or a manifest handed it over.
     *  @return The run, or @c status_code_t::malformed_k for an extent shorter than the
     *      run it claims.
     */
    static expected<lsm_sorted_run> open(volume_type_& volume, io_channel_t& channel,
                                         medium_extent_t const& extent) noexcept {
        lsm_sorted_run run;
        run.volume_ = &volume;
        run.extent_ = extent;
        if (status_t const head = run.read_header_(channel); head.failed()) return head;
        if constexpr (tailed_k) {
            if (status_t const strings = run.read_strings_(channel); strings.failed()) return strings;
        }
        else {
            if (status_t const index = run.read_index_(channel); index.failed()) return index;
            if (status_t const offsets = run.read_value_offsets_(channel); offsets.failed()) return offsets;
        }
        if (status_t const trailer = run.read_trailer_(channel); trailer.failed()) return trailer;
        if (status_t const ranges = run.read_range_tombstones_(channel); ranges.failed()) return ranges;
        return run;
    }

    /**
     *  @brief The run a merge just wrote at @p extent, built from what the merge still holds rather
     *      than read back from the volume, which leaves it as @c open would.
     *  @param[in,out] volume The volume the run reads through afterwards, borrowed
     *      and outliving it.
     *  @param[in] extent Where the merge wrote the run.
     *  @param[in] header The header the merge wrote.
     *  @param[in] cells The cells in sorted order, which a run keeping its cells copies.
     *  @param[in] tree_words The saved tree of the keys, as @c save_keys laid it.
     *  @param[in] value_offsets The value offsets the merge wrote, none but for a variable width.
     *  @param[in] stamps The stamps column the merge packed for the trailer.
     *  @param[in] kinds The kinds column the merge packed for the trailer.
     *  @param[in] tombstones The range tombstones the merge wrote at the run's end.
     */
    static expected<lsm_sorted_run> adopt(volume_type_& volume, medium_extent_t const& extent,
                                          sorted_run_header_t const& header,
                                          [[maybe_unused]] std::span<key_slot_t const> cells,
                                          st::vector<std::uint64_t> tree_words,
                                          st::vector<stored_value_offset_t> value_offsets, stamps_column_t stamps,
                                          kinds_column_t kinds, range_tombstones_t tombstones) noexcept
        requires(!tailed_k)
    {
        lsm_sorted_run run;
        run.volume_ = &volume;
        run.extent_ = extent;
        run.header_ = header;
        if constexpr (keeps_cells_k)
            if (run.cells_.assign(cells) != st::status_t::success_k)
                return status_t {status_code_t::out_of_memory_k, "A run could not hold its index"};
        run.tree_words_ = std::move(tree_words);
        st::expected<keys_view_t> const tree = keys_view_t::make(
            std::as_bytes(std::span<std::uint64_t const> {run.tree_words_}));
        if (!tree || tree->size() != header.key_slots_count)
            return status_t {status_code_t::internal_k, "A merged run's tree disagrees with it"};
        run.tree_ = *tree;
        run.value_offsets_ = std::move(value_offsets);
        run.stamps_ = std::move(stamps);
        run.kinds_ = std::move(kinds);
        run.range_tombstones_ = std::move(tombstones);
        return run;
    }

#pragma endregion Setup and Teardown

#pragma region Lookups

    /** Whether this run holds an index at all, which a default-constructed one does not. */
    [[nodiscard]] bool attached() const noexcept { return volume_ != nullptr; }

    /** Key slots the run holds, padding cells excluded. */
    [[nodiscard]] std::uint64_t population_count() const noexcept { return header_.key_slots_count; }

    /** Bytes the run's extent takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return extent_.extent.length; }

    /** The header the run was written or opened with, which records the width of its cells and its value layout. */
    [[nodiscard]] sorted_run_header_t const& header() const noexcept { return header_; }

    /** Where the run lies, which a manifest names and a volume deallocates. */
    [[nodiscard]] medium_extent_t const& extent() const noexcept { return extent_; }

    /** The sorted cells of a run whose cells carry values, which a compaction reads in place. */
    [[nodiscard]] std::span<key_slot_t const> cells() const noexcept
        requires keeps_cells_k
    {
        return std::span<key_slot_t const> {cells_};
    }

    /** Where each key slot's value starts inside the values section and one past the last, in sorted order, which a
     *  compaction reads in place; none for a run finding its values by stride or holding them inline. */
    [[nodiscard]] std::span<stored_value_offset_t const> value_offsets() const noexcept {
        return std::span<stored_value_offset_t const> {value_offsets_};
    }

    /** The key slot at sorted @p position, below @c population_count, from the cells or tree. */
    [[nodiscard]] key_slot_t key_slot(std::uint64_t position) const noexcept
        requires(!tailed_k)
    {
        if constexpr (keeps_cells_k) return cells_[static_cast<std::size_t>(position)];
        else {
            key_slot_t slot {};
            slot.stored_key = tree_.select(static_cast<std::size_t>(position));
            return slot;
        }
    }

    /** The stamp of the key slot at sorted @p position, which must be below @c population_count. */
    [[nodiscard]] std::uint64_t stamp(std::uint64_t position) const noexcept {
        return stamps_[static_cast<std::size_t>(position)];
    }

    /** The stamp of every key slot in sorted order, which a compaction reads in place. */
    [[nodiscard]] stamps_column_t const& stamps() const noexcept { return stamps_; }

    /** What the key slot at sorted @p position does, which must be below @c population_count. */
    [[nodiscard]] delta_kind_t kind(std::uint64_t position) const noexcept {
        return static_cast<delta_kind_t>(kinds_[static_cast<std::size_t>(position)]);
    }

    /** The kind of every key slot in sorted order, which a compaction reads in place. */
    [[nodiscard]] kinds_column_t const& kinds() const noexcept { return kinds_; }

    /** The newest stamp any key slot or range tombstone of the run carries, which a check of what
     *  changed since a stamp skips by. */
    [[nodiscard]] std::uint64_t newest_stamp() const noexcept {
        return fu::max_of(header_.newest_stamp, range_tombstones_.newest_stamp());
    }

    /** The range tombstones the run carries, ascending by start. */
    [[nodiscard]] range_tombstones_t const& range_tombstones() const noexcept { return range_tombstones_; }

    /** Whether the run holds a key slot or a range tombstone, so its bounds name keys. */
    [[nodiscard]] bool holds_keys() const noexcept {
        return header_.key_slots_count != 0 || !range_tombstones_.empty();
    }

    /** The stamp every stamp of the run was raised to at least. */
    [[nodiscard]] std::uint64_t stamps_floor() const noexcept { return header_.stamps_floor; }

    /** The stamp @c assign_stamp gave every version of a run its extent stamps zero, or zero for a
     *  run that carries its own stamps, which a schema records beside the run. */
    [[nodiscard]] std::uint64_t assigned_stamp() const noexcept { return assigned_stamp_; }

    /** Bytes the stamps column takes in memory, which is all a run spends on its stamps. */
    [[nodiscard]] std::uint64_t stamps_bytes() const noexcept { return stamps_.size_bytes(); }

    /** The least key the run holds or a range tombstone of it hides, by head for a run of strings,
     *  or the greatest key of the type for an empty run. */
    [[nodiscard]] stored_key_t lower_stored_key() const noexcept {
        stored_key_t hidden = limit<stored_key_t>::max_k;
        if constexpr (tailed_k) {
            if (!range_tombstones_.empty()) hidden = strings::head_of(range_tombstones_.range(0).start);
        }
        else hidden = range_tombstones_.lower_stored_key();
        if (header_.key_slots_count == 0) return hidden;
        stored_key_t first {};
        if constexpr (tailed_k) first = head(0);
        else first = key_slot(0).stored_key;
        return hidden < first ? hidden : first;
    }

    /** The greatest key the run holds or a range tombstone of it hides, by head for a run of
     *  strings, the head of an exclusive end standing for every string below it, or the least key
     *  of the type for an empty run. */
    [[nodiscard]] stored_key_t upper_stored_key() const noexcept {
        stored_key_t hidden = limit<stored_key_t>::min_k;
        if constexpr (tailed_k) {
            for (auto const& range : range_tombstones_.ranges()) {
                stored_key_t const reach = range.end.empty() ? limit<stored_key_t>::max_k : strings::head_of(range.end);
                if (hidden < reach) hidden = reach;
            }
        }
        else hidden = range_tombstones_.upper_stored_key();
        if (header_.key_slots_count == 0) return hidden;
        stored_key_t last {};
        if constexpr (tailed_k) last = head(header_.key_slots_count - 1);
        else last = key_slot(header_.key_slots_count - 1).stored_key;
        return last < hidden ? hidden : last;
    }

    /** The least whole string a run of strings holds or hides. */
    [[nodiscard]] std::string_view lower_string() const noexcept
        requires tailed_k
    {
        std::string_view const hidden = range_tombstones_.empty() ? std::string_view {}
                                                                  : range_tombstones_.range(0).start;
        if (header_.key_slots_count == 0) return hidden;
        return !range_tombstones_.empty() && hidden < key(0) ? hidden : key(0);
    }

    /** Whether every key of this run orders before every key of @p other, by whole keys for a run
     *  of strings, so two runs of one level may share a head where one's last key and the other's
     *  first differ past it. */
    [[nodiscard]] bool precedes(lsm_sorted_run const& other) const noexcept {
        if (!holds_keys() || !other.holds_keys()) return true;
        if constexpr (tailed_k) {
            // A tombstone's end is exclusive, so it may reach the first string of the next run.
            std::string_view const next = other.lower_string();
            if (header_.key_slots_count != 0 && !(key(header_.key_slots_count - 1) < next)) return false;
            for (auto const& range : range_tombstones_.ranges())
                if (range.end.empty() || next < range.end) return false;
            return true;
        }
        else return upper_stored_key() < other.lower_stored_key();
    }

    /** The head of the string at sorted @p position of a run of strings, which must be below
     *  @c population_count. */
    [[nodiscard]] st::key128_t head(std::uint64_t position) const noexcept
        requires tailed_k
    {
        return heads_view_().select(static_cast<std::size_t>(position));
    }

    /** The 16-byte value of the string at sorted @p position of a run of strings. */
    [[nodiscard]] std::span<byte_t const> value(std::uint64_t position) const noexcept
        requires tailed_k
    {
        return std::span<byte_t const> {values_}.subspan(
            static_cast<std::size_t>(position * strings_layout_t::value_bytes_k), strings_layout_t::value_bytes_k);
    }

    /** The whole string at sorted @p position of a run of strings. */
    [[nodiscard]] std::string_view key(std::uint64_t position) const noexcept
        requires tailed_k
    {
        std::size_t const begin = key_offsets_[static_cast<std::size_t>(position)];
        std::size_t const end = key_offsets_[static_cast<std::size_t>(position) + 1];
        return std::string_view {reinterpret_cast<char const*>(keys_.data()) + begin, end - begin};
    }

    /** One offset per string of a run of strings into @c key_tape and one past the last: an
     *  Arrow string column. */
    [[nodiscard]] std::span<std::uint32_t const> key_offsets() const noexcept
        requires tailed_k
    {
        return std::span<std::uint32_t const> {key_offsets_};
    }

    /** Every string of a run of strings whole, back to back in sorted order. */
    [[nodiscard]] std::span<byte_t const> key_tape() const noexcept
        requires tailed_k
    {
        return std::span<byte_t const> {keys_};
    }

    /** The layers of a run of strings, by first position and then by depth. */
    [[nodiscard]] std::span<head_layer_t const> layers() const noexcept
        requires tailed_k
    {
        return std::span<head_layer_t const> {layers_};
    }

    /**
     *  @brief The sorted positions of a run of strings whose heads, and whose layer heads as deep
     *      as a layer splits them, equal those of @p text, found by descending the S+ tree of the
     *      heads and then the layers.
     *  @return At most one row of keys, or the versions of one key, which a lookup compares whole
     *      one by one.
     */
    [[nodiscard]] key_positions_t key_range(std::string_view text) const noexcept
        requires tailed_k
    {
        heads_view_t const heads = heads_view_();
        st::key128_t const wanted = strings::head_of(text);
        key_positions_t range {heads.rank(wanted), rank_above_(heads, wanted)};
        for (std::uint32_t depth = 1; range.last - range.first > strings_layout_t::keys_per_row_k; ++depth) {
            std::size_t const skipped = std::size_t {depth} * strings::head_prefix_bytes_k;
            head_layer_t const* const layer = layer_at_(range.first, depth);
            // A wide tie without a layer holds the versions of one key, which no deeper
            // head tells apart.
            if (layer == nullptr || text.size() <= skipped) break;
            heads_view_t const deeper_heads = layer_view_(*layer);
            st::key128_t const deeper = strings::head_of(text.substr(skipped));
            range = key_positions_t {layer->first + deeper_heads.rank(deeper),
                                     layer->first + rank_above_(deeper_heads, deeper)};
        }
        return range;
    }

    /** The first sorted position of a run of strings whose key is not below @p text, comparing at
     *  most one row of whole keys once the heads and the layers narrowed the range. */
    [[nodiscard]] std::uint64_t lower_bound(std::string_view text) const noexcept
        requires tailed_k
    {
        key_positions_t const range = key_range(text);
        // A range wider than one row holds one key's versions, so one compare places all of them.
        if (range.last - range.first > strings_layout_t::keys_per_row_k)
            return key(range.first) < text ? range.last : range.first;
        std::uint64_t position = range.first;
        while (position != range.last && key(position) < text) ++position;
        return position;
    }

    /** The first sorted position whose key is not below @p stored_key, or @c population_count when
     *  every key is, the rank the saved tree of the keys lands on. */
    [[nodiscard]] std::uint64_t lower_bound(stored_key_t stored_key) const noexcept
        requires(!tailed_k)
    {
        return tree_.rank(stored_key);
    }

    /** The saved S+ tree every lookup descends: of the keys, or of a run of strings' heads. */
    [[nodiscard]] keys_view_t const& keys() const noexcept { return tree_; }

    /** The saved tree of the keys, or of a run of strings' heads, which a merge views in place with
     *  its own kit. */
    [[nodiscard]] std::span<std::byte const> keys_bytes() const noexcept {
        return std::as_bytes(std::span<std::uint64_t const> {tree_words_});
    }

    /** Where @p stored_key sits in sorted order, or @c population_count when the run holds no such key. */
    [[nodiscard]] std::uint64_t position_of(stored_key_t stored_key) const noexcept
        requires(!tailed_k)
    {
        std::uint64_t const low = lower_bound(stored_key);
        if (low == header_.key_slots_count) return header_.key_slots_count;
        return key_slot(low).stored_key == stored_key ? low : header_.key_slots_count;
    }

    /**
     *  @brief Whether the run carries @p stored_key, and whether that key slot locates a value.
     *  @param[in] stored_key The key to look for.
     *  @param[out] position Where the key sits, or @c population_count when the run holds no such key.
     */
    [[nodiscard]] lookup_result_t find(stored_key_t stored_key, std::uint64_t& position) const noexcept
        requires(!tailed_k)
    {
        position = position_of(stored_key);
        if (position == header_.key_slots_count) return lookup_result_t::missing_k;
        return result_at_(position);
    }

    /** As @c find, answering the newest version of @p stored_key stamped at or below @p at, which is what a reader
     *  pinned at @p at sees; the versions of one key sit newest first. */
    [[nodiscard]] lookup_result_t find(stored_key_t stored_key, std::uint64_t at,
                                       std::uint64_t& position) const noexcept
        requires(!tailed_k)
    {
        position = position_of(stored_key);
        while (position != header_.key_slots_count && stamp(position) > at) {
            ++position;
            if (position != header_.key_slots_count && !(key_slot(position).stored_key == stored_key))
                position = header_.key_slots_count;
        }
        if (position == header_.key_slots_count) return lookup_result_t::missing_k;
        return result_at_(position);
    }

    /** Calls @p visitor with the position and the key slot of every key, in ascending order of keys. */
    template <typename visitor_type_>
    void for_each_key_slot(visitor_type_&& visitor) const noexcept
        requires(!tailed_k)
    {
        for (std::uint64_t position = 0; position != header_.key_slots_count; ++position)
            visitor(position, key_slot(position));
    }

    /** Where the value of the key slot at sorted @p position sits, empty for one that needs no read. */
    [[nodiscard]] value_window_t value_window(std::uint64_t position) const noexcept {
        value_window_t window;
        if constexpr (tailed_k) {
            window.bytes = static_cast<std::uint32_t>(strings_layout_t::value_bytes_k);
            return window;
        }
        else if constexpr (holds_values_inline_k) {
            window.bytes = key_slot(position).size;
            return window;
        }
        else {
            std::uint64_t const begin = value_begin_(position);
            std::uint64_t const end = value_end_(position);
            if (begin == end) return window;
            std::uint64_t const first = sorted_run_layout_t::values_offset(header_.key_slots_count) + begin;
            std::uint64_t const aligned = round_down_to_multiple(first, portable_alignment_k);
            std::uint64_t const last = round_up_to_multiple(first + (end - begin), portable_alignment_k);
            window.extent = extent_.slice(aligned, last - aligned);
            window.skipped = static_cast<std::uint32_t>(first - aligned);
            window.bytes = static_cast<std::uint32_t>(end - begin);
            return window;
        }
    }

    /** Where the values of the key slots at sorted positions @b [begin,end) sit, with the value offsets locating them,
     *  as one read of each section asks for them; empty for values held inline. */
    [[nodiscard]] value_tape_window_t value_tape_window(std::uint64_t begin, std::uint64_t end) const noexcept {
        value_tape_window_t window;
        if constexpr (!holds_values_inline_k) {
            std::uint64_t const count = header_.key_slots_count;
            if constexpr (carries_value_offsets_k) {
                std::uint64_t const first = sorted_run_layout_t::value_offsets_offset(count) +
                                            begin * sizeof(stored_value_offset_t);
                std::uint64_t const aligned = round_down_to_multiple(first, portable_alignment_k);
                std::uint64_t const last = round_up_to_multiple(
                    first + (end - begin + 1) * sizeof(stored_value_offset_t), portable_alignment_k);
                window.offsets = extent_.slice(aligned, last - aligned);
                window.offsets_skipped = static_cast<std::uint32_t>(first - aligned);
            }
            std::uint64_t const values_begin = value_begin_(begin);
            std::uint64_t const values_end = value_begin_(end);
            window.first_offset = static_cast<std::uint32_t>(values_begin);
            window.bytes = static_cast<std::uint32_t>(values_end - values_begin);
            if (values_end != values_begin) {
                std::uint64_t const first = sorted_run_layout_t::values_offset(count) + values_begin;
                std::uint64_t const aligned = round_down_to_multiple(first, portable_alignment_k);
                std::uint64_t const last = round_up_to_multiple(first + window.bytes, portable_alignment_k);
                window.values = extent_.slice(aligned, last - aligned);
                window.values_skipped = static_cast<std::uint32_t>(first - aligned);
            }
        }
        return window;
    }

#pragma endregion Lookups

#pragma region Stamps

    /**
     *  @brief Gives every version of a run written at stamp zero the stamp @p stamp, in memory
     *      alone, which an ingest does once its commit draws the stamp and a mount does again from
     *      the manifest, as the extent keeps its zeros.
     *  @return Success, @c invalid_argument_k for a zero @p stamp or a run carrying stamps of its
     *      own, or @c out_of_memory_k with the run unchanged.
     */
    status_t assign_stamp(std::uint64_t stamp) noexcept {
        if (stamp == 0) return {status_code_t::invalid_argument_k, "A run is assigned a stamp above zero"};
        if (header_.newest_stamp != 0 || assigned_stamp_ != 0)
            return {status_code_t::invalid_argument_k, "Only a run written at stamp zero is assigned a stamp"};
        st::expected<stamps_column_t> column = stamps_column_t::make(stamps_.size(), stamp);
        if (!column) return {status_code_t::out_of_memory_k, "A run could not hold its assigned stamps"};
        stamps_ = std::move(*column);
        header_.stamps_floor = stamp;
        header_.newest_stamp = stamp;
        assigned_stamp_ = stamp;
        return {};
    }

#pragma endregion Stamps

#pragma region Batched Reads

    /**
     *  @brief Reads the values of the key slots at @p positions, refilling the channel as completions retire.
     *  @param[in,out] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] positions Sorted positions of the key slots whose values are wanted.
     *  @param[in] sink Called as @c sink(request, offset, bytes) while the channel still owns the block.
     *  @return Success, or the first failure any of the blocks reported.
     *
     *  @note A value larger than one block arrives as several calls, each carrying the bytes of the
     *      value before it, and a key slot that locates nothing is reported once with no bytes.
     */
    template <typename sink_type_>
    status_t read_values(io_channel_t& channel, std::span<std::uint64_t const> positions,
                         sink_type_&& sink) const noexcept {
        if constexpr (tailed_k) {
            for (std::size_t request = 0; request != positions.size(); ++request)
                sink(request, std::uint64_t {0}, value(positions[request]));
            return {};
        }
        else if constexpr (holds_values_inline_k) {
            for (std::size_t request = 0; request != positions.size(); ++request) {
                key_slot_t const& slot = key_slot(positions[request]);
                sink(request, std::uint64_t {0}, std::span<byte_t const> {slot.data, slot.size});
            }
            return {};
        }
        else
            return read_windows_(
                channel, positions.size(),
                [&](std::size_t request) noexcept { return value_window(positions[request]); }, sink);
    }

    /**
     *  @brief Copies the value of the key slot at sorted @p position into @p into, which holds at least its bytes.
     *  @param[in,out] channel The channel the read is submitted and completed on, held by the calling thread.
     *  @param[in] position The key slot's place in the run's sorted order.
     *  @param[out] into Room for the value's bytes.
     *  @return Success, or the first failure any of the blocks reported.
     */
    status_t read_value(io_channel_t& channel, std::uint64_t position, std::span<byte_t> into) const noexcept {
        value_window_t const window = value_window(position);
        if (into.size() < window.bytes) return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};
        auto copy = [&](std::size_t, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            if (!bytes.empty()) std::memcpy(into.data() + offset, bytes.data(), bytes.size());
        };
        if constexpr (tailed_k) {
            copy(0, 0, value(position));
            return {};
        }
        else if constexpr (holds_values_inline_k) {
            key_slot_t const& slot = key_slot(position);
            copy(0, 0, std::span<byte_t const> {slot.data, slot.size});
            return {};
        }
        else return read_windows_(channel, 1, [&](std::size_t) noexcept { return window; }, copy);
    }

#pragma endregion Batched Reads

  private:

#pragma region Private Methods

    /** Where the value of the key slot at sorted @p position starts inside the values section, which for @p position
     *  at @c population_count is the section's end. */
    [[nodiscard]] std::uint64_t value_begin_(std::uint64_t position) const noexcept {
        if constexpr (carries_value_offsets_k) return value_offsets_[static_cast<std::size_t>(position)];
        else return position * header_.value_max_size;
    }

    /** Where the value of the key slot at sorted @p position ends inside the values section, at its start for a
     *  removal of a fixed width, whose stride holds no value. */
    [[nodiscard]] std::uint64_t value_end_(std::uint64_t position) const noexcept {
        if constexpr (carries_value_offsets_k) return value_offsets_[static_cast<std::size_t>(position) + 1];
        else {
            std::uint64_t const begin = value_begin_(position);
            return kind(position) == delta_kind_t::remove_k ? begin : begin + header_.value_max_size;
        }
    }

    /** Refuses @p stored_values_bytes and @p value_offsets unless they lay out the values of
     *  @p key_slots_count key slots as this layout stores them: none inline, @p value_max_size
     *  bytes each at a fixed width, and located by one ascending offset per key slot and one past
     *  the last at a variable width. */
    static status_t check_values_(std::uint64_t stored_values_bytes,
                                  std::span<stored_value_offset_t const> value_offsets, std::size_t key_slots_count,
                                  stored_value_size_t value_max_size) noexcept {
        if constexpr (carries_value_offsets_k) {
            if (value_offsets.size() != key_slots_count + 1 || value_offsets.front() != 0 ||
                value_offsets.back() != stored_values_bytes)
                return {status_code_t::invalid_argument_k,
                        "A run takes one value offset per key slot and one past them"};
            for (std::size_t index = 1; index != value_offsets.size(); ++index)
                if (value_offsets[index] < value_offsets[index - 1])
                    return {status_code_t::invalid_argument_k, "A run takes its value offsets ascending"};
            return {};
        }
        else {
            if (!value_offsets.empty())
                return {status_code_t::invalid_argument_k, "Only a variable-width run takes value offsets"};
            std::uint64_t const expected_bytes = holds_values_inline_k
                                                     ? 0
                                                     : std::uint64_t {key_slots_count} * value_max_size;
            if (stored_values_bytes != expected_bytes)
                return {status_code_t::invalid_argument_k,
                        holds_values_inline_k ? "An inline run carries no stored values"
                                              : "A fixed-width run takes one value of its width per key slot"};
            return {};
        }
    }

    /** What the key slot at @p position answers a lookup by its kind. */
    [[nodiscard]] lookup_result_t result_at_(std::uint64_t position) const noexcept {
        switch (kind(position)) {
        case delta_kind_t::remove_k: return lookup_result_t::removed_k;
        case delta_kind_t::change_k: return lookup_result_t::changed_k;
        default: return lookup_result_t::written_k;
        }
    }

    /** Reads the @p count values @p window_of names by request, refilling the channel as completions retire, as
     *  @c read_values does for the values of a run that locates them. */
    template <typename window_of_type_, typename sink_type_>
    status_t read_windows_(io_channel_t& channel, std::size_t count, window_of_type_&& window_of,
                           sink_type_&& sink) const noexcept {
        std::uint64_t const block_bytes = round_down_to_multiple(channel.requests().buffer_bytes(),
                                                                 portable_alignment_k);
        if (block_bytes == 0)
            return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than one page"};

        if (count > (std::numeric_limits<std::uint32_t>::max)())
            return {status_code_t::invalid_argument_k, "A batched read takes at most 2^32 values"};
        std::span<read_piece_t> const pieces = channel.read_pieces();
        std::size_t const depth = pieces.size();

        auto [reader_channel, reader_channel_status] = channel.acquire();
        if (reader_channel_status.failed()) return reader_channel_status;
        read_pipeline_t reader {volume_->disk(), std::move(reader_channel), extent_.medium};
        status_t first;
        auto adapter = [&](completed_buffer_t const& completed) noexcept {
            read_piece_t const& piece = pieces[completed.tag % depth];
            if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                if (first.succeeded()) first = whole;
                return;
            }
            sink(piece.request, piece.offset, completed.bytes.subspan(piece.skipped, piece.carried));
        };

        std::size_t request = 0;
        std::uint64_t chunk = 0;
        request_tag_t tag = 0;
        while (request != count || reader.outstanding() != 0) {
            while (request != count && reader.can_enqueue()) {
                value_window_t const window = window_of(request);
                if (window.extent.length == 0) {
                    sink(request, std::uint64_t {0}, std::span<byte_t const> {});
                    ++request;
                    chunk = 0;
                    continue;
                }
                std::uint64_t const at = chunk * block_bytes;
                std::uint64_t const room = window.extent.length - at;
                std::uint64_t const length = block_bytes < room ? block_bytes : room;
                pieces[tag % depth] = piece_of_(window, request, at, length);
                extent_t const piece {window.extent.offset + at, length};
                if (status_t const queued = reader.enqueue_read(piece, tag); queued.failed()) {
                    if (reader.outstanding() == 0) return queued;
                    break;
                }
                ++tag;
                if (at + length == window.extent.length) {
                    ++request;
                    chunk = 0;
                }
                else ++chunk;
            }
            if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
            auto [polled, wait_completions_status] = reader.wait_completions(adapter);
            if (wait_completions_status.failed()) return wait_completions_status;
        }
        return first;
    }

    /** The piece of @p window that the block at @p at of @p length carries, for @p request. */
    [[nodiscard]] static read_piece_t piece_of_(value_window_t const& window, std::size_t request, std::uint64_t at,
                                                std::uint64_t length) noexcept {
        std::uint64_t const wanted_first = window.skipped;
        std::uint64_t const wanted_last = window.skipped + window.bytes;
        std::uint64_t const first = at > wanted_first ? at : wanted_first;
        std::uint64_t const last = at + length < wanted_last ? at + length : wanted_last;
        read_piece_t piece;
        piece.request = static_cast<std::uint32_t>(request);
        piece.skipped = static_cast<std::uint32_t>(first - at);
        piece.carried = static_cast<std::uint32_t>(last > first ? last - first : 0);
        piece.offset = static_cast<std::uint32_t>(first - wanted_first);
        return piece;
    }

    /** Refuses @p tails unless they are none, or one offset per one of @p key_slots_count key slots and one past the
     *  last, ascending from zero to the bytes they locate. */
    static status_t check_tails_(key_tails_column_t const& tails, std::size_t key_slots_count) noexcept {
        if (tails.offsets.empty()) {
            if (!tails.bytes.empty()) return {status_code_t::invalid_argument_k, "Tails come with their offsets"};
            return {};
        }
        if (tails.offsets.size() != key_slots_count + 1 || tails.offsets.front() != 0 ||
            tails.offsets.back() != tails.bytes.size())
            return {status_code_t::invalid_argument_k, "A run takes one tail offset per key slot and one past them"};
        for (std::size_t index = 1; index != tails.offsets.size(); ++index)
            if (tails.offsets[index] < tails.offsets[index - 1])
                return {status_code_t::invalid_argument_k, "A run takes its tail offsets ascending"};
        return {};
    }

    /** How the key of key slot @p index of @p key_slots orders against the one before it: by head, then by tail. */
    static int key_order_(std::span<key_slot_t const> key_slots, key_tails_column_t const& tails,
                          std::size_t index) noexcept {
        if (key_slots[index].stored_key < key_slots[index - 1].stored_key) return -1;
        if (key_slots[index - 1].stored_key < key_slots[index].stored_key) return 1;
        return tails.at(index).compare(tails.at(index - 1));
    }

    /** Stages @p bytes zero-padded to @p section bytes at @p offset of @p extent, a block at a
     *  time, gathering each block's share when @p bytes is a @c gathered_values_concept. */
    template <typename bytes_type_ = std::span<byte_t const>>
    static status_t write_bytes_(write_pipeline_t& writer, medium_extent_t const& extent, std::uint64_t offset,
                                 std::uint64_t section, bytes_type_ const& bytes) noexcept {
        for (std::uint64_t written = 0; written != section;) {
            auto [block, acquire_buffer_or_poll_status] = writer.acquire_buffer_or_poll();
            if (acquire_buffer_or_poll_status.failed()) return acquire_buffer_or_poll_status;
            std::uint64_t const block_bytes = round_down_to_multiple(block.size(), portable_alignment_k);
            if (block_bytes == 0) return {status_code_t::capacity_exhausted_k, "A channel's block holds no page"};
            std::uint64_t const room = section - written;
            std::uint64_t const chunk = block_bytes < room ? block_bytes : room;
            std::span<byte_t> const payload = block.bytes().first(static_cast<std::size_t>(chunk));
            std::uint64_t const left = bytes.size() > written ? bytes.size() - written : 0;
            std::uint64_t const carried = left < chunk ? left : chunk;
            if constexpr (gathered_values_concept<bytes_type_>) {
                if (carried != 0) bytes.gather(written, payload.first(static_cast<std::size_t>(carried)));
            }
            else if (carried != 0)
                std::memcpy(payload.data(), bytes.data() + written, static_cast<std::size_t>(carried));
            std::memset(payload.data() + carried, 0, static_cast<std::size_t>(chunk - carried));
            if (status_t const queued = writer.enqueue_write(std::move(block), extent.slice(offset + written, chunk));
                queued.failed())
                return queued;
            if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;
            written += chunk;
        }
        return {};
    }

    /** Where the trailer of the run @p header describes starts, for either shape of run. */
    [[nodiscard]] static std::uint64_t stamps_offset_(sorted_run_header_t const& header) noexcept {
        if constexpr (tailed_k) return strings_layout_t::stamps_offset(header.key_slots_count, header.key_tape_bytes);
        else return sorted_run_layout_t::stamps_offset(header.key_slots_count, header.stored_values_bytes);
    }

    /** Where the sections of the run @p header describes end. */
    [[nodiscard]] static std::uint64_t sections_end_(sorted_run_header_t const& header) noexcept {
        if constexpr (tailed_k) return strings_layout_t::blob_bytes(header);
        else return sorted_run_layout_t::blob_bytes(header);
    }

    /** The heads of this run of strings, searched in place over the words read at open. */
    [[nodiscard]] heads_view_t const& heads_view_() const noexcept
        requires tailed_k
    {
        return tree_;
    }

    /** The saved bytes of the heads of @p layer, which must be one of this run's. */
    [[nodiscard]] std::span<std::byte const> layer_bytes_(head_layer_t const& layer) const noexcept {
        return std::as_bytes(std::span<std::uint64_t const> {layer_words_})
            .subspan(static_cast<std::size_t>(layer.heads_offset), static_cast<std::size_t>(layer.heads_bytes));
    }

    /** The heads of @p layer, searched in place. */
    [[nodiscard]] heads_view_t layer_view_(head_layer_t const& layer) const noexcept {
        return *heads_view_t::make(layer_bytes_(layer));
    }

    /** The layer of the tie starting at sorted @p first whose heads start @p depth heads deep, or
     *  null for none. */
    [[nodiscard]] head_layer_t const* layer_at_(std::uint64_t first, std::uint32_t depth) const noexcept {
        std::size_t low = 0;
        std::size_t high = layers_.size();
        while (low != high) {
            std::size_t const middle = low + (high - low) / 2;
            head_layer_t const& layer = layers_[middle];
            if (layer.first < first || (layer.first == first && layer.depth < depth)) low = middle + 1;
            else high = middle;
        }
        if (low == layers_.size() || layers_[low].first != first || layers_[low].depth != depth) return nullptr;
        return &layers_[low];
    }

    /** How many heads of @p heads are not above @p head. */
    [[nodiscard]] static std::uint64_t rank_above_(heads_view_t const& heads, st::key128_t head) noexcept {
        if (head == limit<st::key128_t>::max_k) return heads.size();
        st::key128_t const next = head.low != limit<std::uint64_t>::max_k ? st::key128_t {head.high, head.low + 1}
                                                                          : st::key128_t {head.high + 1, 0};
        return heads.rank(next);
    }

    /** Bytes of a key that @p head holds: all of a short key's, and the leading ones of
     *  a long key's. */
    [[nodiscard]] static std::size_t lead_bytes_(st::key128_t const& head) noexcept {
        std::size_t const length = strings::head_length_of(head);
        return length < strings::head_prefix_bytes_k ? length : strings::head_prefix_bytes_k;
    }

    /** Lays out the heads, the values, the key tape and the layers of a run of strings from
     *  its key slots, whose heads hold the leading bytes of every key, and @p tails, the
     *  bytes past them. */
    static status_t lay_out_strings_(std::span<key_slot_t const> key_slots, key_tails_column_t const& tails,
                                     strings_sections_t& sections) noexcept {
        std::size_t const count = key_slots.size();
        std::uint64_t keys_bytes = 0;
        for (std::size_t index = 0; index != count; ++index)
            keys_bytes += lead_bytes_(key_slots[index].stored_key) + tails.at(index).size();
        if (keys_bytes > (std::numeric_limits<std::uint32_t>::max)())
            return {status_code_t::capacity_exhausted_k, "A run's keys outgrow what its offsets reach"};
        st::vector<st::key128_t> heads;
        if (heads.resize(count) != st::status_t::success_k ||
            sections.key_offsets.resize(count + 1) != st::status_t::success_k ||
            sections.keys.resize(static_cast<std::size_t>(keys_bytes)) != st::status_t::success_k ||
            sections.values.resize(count * strings_layout_t::value_bytes_k) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run of strings could not lay its keys out"};

        std::uint32_t written = 0;
        for (std::size_t index = 0; index != count; ++index) {
            key_slot_t const& slot = key_slots[index];
            std::byte head_bytes[sizeof(st::key128_t)];
            slot.stored_key.to_bytes(head_bytes);
            std::size_t const lead = lead_bytes_(slot.stored_key);
            std::string_view const tail = tails.at(index);
            sections.key_offsets[index] = written;
            if (lead != 0) std::memcpy(sections.keys.data() + written, head_bytes, lead);
            if (!tail.empty()) std::memcpy(sections.keys.data() + written + lead, tail.data(), tail.size());
            written += static_cast<std::uint32_t>(lead + tail.size());
            std::memcpy(sections.values.data() + index * strings_layout_t::value_bytes_k, slot.data,
                        strings_layout_t::value_bytes_k);
            heads[index] = slot.stored_key;
        }
        sections.key_offsets[count] = written;
        if (status_t const saved = save_tree_(std::span<st::key128_t const> {heads}, sections.heads); saved.failed())
            return saved;

        st::vector<head_layer_t> layers;
        st::vector<std::uint64_t> trees;
        if (status_t const laid = lay_out_layers_(sections, std::span<st::key128_t const> {heads}, 0, 1, layers, trees);
            laid.failed())
            return laid;
        head_layers_header_t const layers_header {layers.size(), trees.size() * sizeof(std::uint64_t)};
        std::size_t const header_words = sizeof(layers_header) / sizeof(std::uint64_t);
        std::size_t const list_words = layers.size() * sizeof(head_layer_t) / sizeof(std::uint64_t);
        if (sections.layers.resize(header_words + list_words + trees.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run of strings could not lay its layers out"};
        std::memcpy(sections.layers.data(), &layers_header, sizeof(layers_header));
        if (!layers.empty())
            std::memcpy(sections.layers.data() + header_words, layers.data(), layers.size() * sizeof(head_layer_t));
        if (!trees.empty())
            std::memcpy(sections.layers.data() + header_words + list_words, trees.data(),
                        trees.size() * sizeof(std::uint64_t));
        return {};
    }

    /** Saves the S+ tree of @p keys, which ascend, into @p words. */
    template <typename key_type_>
    static status_t save_tree_(std::span<key_type_ const> keys, st::vector<std::uint64_t>& words) noexcept {
        using tree_t =
            st::immutable_splus_tree<key_type_, st::keys_per_row<key_type_>(st::default_row_bytes_k), row_kit_t>;
        st::expected<tree_t> tree = tree_t::make(keys);
        if (!tree) return {status_code_t::out_of_memory_k, "A run could not build the tree of its keys"};
        std::size_t const saved = tree->saved_bytes();
        if (words.resize(divide_round_up(saved, sizeof(std::uint64_t))) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not save the tree of its keys"};
        if (tree->save(std::as_writable_bytes(std::span<std::uint64_t> {words})) != st::status_t::success_k)
            return {status_code_t::internal_k, "A tree of keys outgrew the bytes it reported"};
        return {};
    }

    /**
     *  @brief Lays out a layer for every tie of @p heads, which start at sorted @p base and ascend,
     *      wider than one row and not one key's versions alone, over the heads @p depth heads deep,
     *      and the layers nested in each.
     *  @param[in] sections The keys of the run, whole.
     *  @param[out] layers The layers, by first position and then by depth.
     *  @param[out] trees The saved trees of their heads, back to back.
     */
    static status_t lay_out_layers_(strings_sections_t const& sections, std::span<st::key128_t const> heads,
                                    std::uint64_t base, std::uint32_t depth, st::vector<head_layer_t>& layers,
                                    st::vector<std::uint64_t>& trees) noexcept {
        std::size_t const skipped = std::size_t {depth} * strings::head_prefix_bytes_k;
        for (std::size_t first = 0; first != heads.size();) {
            std::size_t last = first + 1;
            while (last != heads.size() && heads[last] == heads[first]) ++last;
            std::size_t const tie = last - first;
            std::uint64_t const tie_first = base + first;
            bool const wide = tie > strings_layout_t::keys_per_row_k;
            if (wide && key_of_(sections, tie_first) != key_of_(sections, tie_first + tie - 1)) {
                st::vector<st::key128_t> deeper;
                if (deeper.resize(tie) != st::status_t::success_k)
                    return {status_code_t::out_of_memory_k, "A run of strings could not lay out a layer"};
                for (std::size_t index = 0; index != tie; ++index)
                    deeper[index] = strings::head_of(key_of_(sections, tie_first + index).substr(skipped));
                st::vector<std::uint64_t> saved;
                if (status_t const tree = save_tree_(std::span<st::key128_t const> {deeper}, saved); tree.failed())
                    return tree;
                head_layer_t layer;
                layer.first = tie_first;
                layer.count = static_cast<std::uint32_t>(tie);
                layer.depth = depth;
                layer.heads_offset = trees.size() * sizeof(std::uint64_t);
                layer.heads_bytes = saved.size() * sizeof(std::uint64_t);
                if (layers.push_back(std::move(layer)) != st::status_t::success_k ||
                    trees.reserve(trees.size() + saved.size()) != st::status_t::success_k)
                    return {status_code_t::out_of_memory_k, "A run of strings could not lay out a layer"};
                for (std::uint64_t const word : saved) [[maybe_unused]]
                    st::status_t const placed = trees.push_back(st::assume_reserved, std::uint64_t {word});
                if (status_t const nested = lay_out_layers_(sections, std::span<st::key128_t const> {deeper}, tie_first,
                                                            depth + 1, layers, trees);
                    nested.failed())
                    return nested;
            }
            first = last;
        }
        return {};
    }

    /** The whole key at sorted @p position of the run @p sections lays out. */
    [[nodiscard]] static std::string_view key_of_(strings_sections_t const& sections, std::uint64_t position) noexcept {
        std::size_t const begin = sections.key_offsets[static_cast<std::size_t>(position)];
        std::size_t const end = sections.key_offsets[static_cast<std::size_t>(position) + 1];
        return std::string_view {reinterpret_cast<char const*>(sections.keys.data()) + begin, end - begin};
    }

    /** Stages the heads, the values, the key offsets, the key tape and the layers of a
     *  run of strings. */
    static status_t write_strings_(write_pipeline_t& writer, medium_extent_t const& extent,
                                   sorted_run_header_t const& header, strings_sections_t const& sections) noexcept {
        std::uint64_t const count = header.key_slots_count;
        if (status_t const heads = write_bytes_(
                writer, extent, strings_layout_t::heads_offset(),
                strings_layout_t::values_offset(count) - strings_layout_t::heads_offset(),
                std::as_bytes(std::span<std::uint64_t const> {sections.heads}));
            heads.failed())
            return heads;
        if (status_t const values = write_bytes_(
                writer, extent, strings_layout_t::values_offset(count),
                strings_layout_t::key_offsets_offset(count) - strings_layout_t::values_offset(count),
                std::span<byte_t const> {sections.values});
            values.failed())
            return values;
        if (status_t const offsets = write_bytes_(
                writer, extent, strings_layout_t::key_offsets_offset(count),
                strings_layout_t::keys_offset(count) - strings_layout_t::key_offsets_offset(count),
                std::as_bytes(std::span<std::uint32_t const> {sections.key_offsets}));
            offsets.failed())
            return offsets;
        if (status_t const keys = write_bytes_(
                writer, extent, strings_layout_t::keys_offset(count),
                strings_layout_t::stamps_offset(count, header.key_tape_bytes) - strings_layout_t::keys_offset(count),
                std::span<byte_t const> {sections.keys});
            keys.failed())
            return keys;
        std::span<std::byte const> const layers = std::as_bytes(std::span<std::uint64_t const> {sections.layers});
        return write_bytes_(
            writer, extent,
            strings_layout_t::layers_offset(count, header.key_tape_bytes, header.stamps_bytes, header.kinds_bytes),
            round_up_to_multiple(std::uint64_t {layers.size()}, portable_alignment_k), layers);
    }

    /** Bytes of one block of @p channel that a section is staged in, a whole number of pages. */
    [[nodiscard]] static std::uint64_t block_bytes_of_(io_channel_t& channel) noexcept {
        return round_down_to_multiple(channel.requests().buffer_bytes(), portable_alignment_k);
    }

  public:

    /** Saves the S+ tree of the keys of @p key_slots, which ascend, into @p words. */
    static status_t save_keys(std::span<key_slot_t const> key_slots, st::vector<std::uint64_t>& words) noexcept
        requires(!tailed_k)
    {
        st::vector<stored_key_t> keys;
        if (keys.resize(key_slots.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not gather its keys"};
        for (std::size_t index = 0; index != key_slots.size(); ++index) keys[index] = key_slots[index].stored_key;
        return save_tree_(std::span<stored_key_t const> {keys}, words);
    }

    /** Writes @p cells, in sorted order, and @p keys, their saved tree, into the extent of the run
     *  @p header opens. */
    static status_t write_index(write_pipeline_t& writer, medium_extent_t const& extent,
                                sorted_run_header_t const& header, std::span<key_slot_t const> cells,
                                std::span<std::uint64_t const> keys) noexcept {
        std::uint64_t const count = header.key_slots_count;
        if (cells.size() != count || keys.size_bytes() < sorted_run_layout_t::keys_saved_bytes(count))
            return {status_code_t::invalid_argument_k, "A run's index takes one cell per key slot and their tree"};
        if (status_t const written = write_bytes_(writer, extent, sorted_run_layout_t::index_offset(),
                                                  sorted_run_layout_t::index_section_bytes(count),
                                                  std::as_bytes(cells));
            written.failed())
            return written;
        return write_bytes_(writer, extent, sorted_run_layout_t::keys_offset(count),
                            sorted_run_layout_t::keys_section_bytes(count), std::as_bytes(keys));
    }

    /** Stages @p value_offsets as the value offsets section of the run @p header describes, none for a run finding
     *  its values by stride or holding them inline, which a flush and a merge both write. */
    static status_t write_value_offsets(write_pipeline_t& writer, medium_extent_t const& extent,
                                        sorted_run_header_t const& header,
                                        std::span<stored_value_offset_t const> value_offsets) noexcept {
        std::uint64_t const offsets_bytes = sorted_run_layout_t::value_offsets_bytes(header.key_slots_count);
        if (value_offsets.size_bytes() != offsets_bytes)
            return {status_code_t::internal_k, "A run's value offsets disagree with the key slots its header names"};
        return write_bytes_(writer, extent, sorted_run_layout_t::value_offsets_offset(header.key_slots_count),
                            round_up_to_multiple(offsets_bytes, portable_alignment_k),
                            std::span<byte_t const> {reinterpret_cast<byte_t const*>(value_offsets.data()),
                                                     static_cast<std::size_t>(offsets_bytes)});
    }

    /** Stages @p stamps, then @p kinds, as the trailer of the run @p header describes, which a flush and a merge both
     *  write. */
    static status_t write_trailer(write_pipeline_t& writer, medium_extent_t const& extent,
                                  sorted_run_header_t const& header, stamps_column_t const& stamps,
                                  kinds_column_t const& kinds) noexcept {
        st::vector<byte_t> saved;
        if (saved.resize(static_cast<std::size_t>(header.stamps_bytes + header.kinds_bytes)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not lay its trailer out"};
        std::span<byte_t> const room {saved};
        std::size_t const kinds_begin = static_cast<std::size_t>(header.stamps_bytes);
        if (stamps.save(room.first(kinds_begin)) != st::status_t::success_k ||
            kinds.save(room.subspan(kinds_begin, static_cast<std::size_t>(header.kinds_bytes))) !=
                st::status_t::success_k)
            return {status_code_t::internal_k, "A run's columns outgrew the bytes its header names"};

        return write_bytes_(writer, extent, stamps_offset_(header),
                            round_up_to_multiple(header.stamps_bytes + header.kinds_bytes, portable_alignment_k),
                            std::span<byte_t const> {saved});
    }

    /** Lays @p tombstones out into @p into as the section at the end of a run: the bounds and
     *  then the stamps for fixed-width keys, and the stamps, the offsets and the tape of the
     *  bounds for strings. */
    static status_t lay_out_range_tombstones(range_tombstones_t const& tombstones, st::vector<byte_t>& into) noexcept {
        using range_t = typename range_tombstones_t::range_t;
        std::size_t const count = tombstones.size();
        std::size_t const stamps_bytes = count * sizeof(std::uint64_t);
        if constexpr (tailed_k) {
            std::size_t tape_bytes = 0;
            for (range_t const& range : tombstones.ranges()) tape_bytes += range.start.size() + range.end.size();
            std::size_t const offsets_bytes = count == 0 ? 0 : (2 * count + 1) * sizeof(std::uint32_t);
            if (into.resize(stamps_bytes + offsets_bytes + tape_bytes) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A run could not lay its range tombstones out"};
            if (count == 0) return {};
            std::memcpy(into.data(), tombstones.stamps().data(), stamps_bytes);
            std::uint32_t offset = 0;
            std::size_t written = stamps_bytes + offsets_bytes;
            auto const placed = [&](std::size_t slot, std::string_view text) noexcept {
                std::memcpy(into.data() + stamps_bytes + slot * sizeof(std::uint32_t), &offset, sizeof(offset));
                if (!text.empty()) std::memcpy(into.data() + written, text.data(), text.size());
                written += text.size();
                offset += static_cast<std::uint32_t>(text.size());
            };
            for (std::size_t index = 0; index != count; ++index) {
                placed(2 * index, tombstones.range(index).start);
                placed(2 * index + 1, tombstones.range(index).end);
            }
            placed(2 * count, {});
            return {};
        }
        else {
            std::size_t const bounds_bytes = count * sizeof(range_t);
            if (into.resize(bounds_bytes + stamps_bytes) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A run could not lay its range tombstones out"};
            if (count == 0) return {};
            std::memcpy(into.data(), tombstones.ranges().data(), bounds_bytes);
            std::memcpy(into.data() + bounds_bytes, tombstones.stamps().data(), stamps_bytes);
            return {};
        }
    }

    /** Stages @p laid_out, the section @c lay_out_range_tombstones laid, at the end of the run
     *  @p header describes, staging nothing for a run of none. */
    static status_t write_range_tombstones(write_pipeline_t& writer, medium_extent_t const& extent,
                                           sorted_run_header_t const& header,
                                           std::span<byte_t const> laid_out) noexcept {
        if (header.range_tombstones_count == 0) return {};
        return write_bytes_(writer, extent, range_tombstones_offset_(header),
                            round_up_to_multiple(header.range_tombstones_bytes, portable_alignment_k), laid_out);
    }

  private:

    /** Reads the first page of the extent and accepts it only as a run this reader can serve. */
    status_t read_header_(io_channel_t& channel) noexcept {
        std::uint64_t const head_bytes = sorted_run_layout_t::header_bytes();
        if (channel.requests().buffer_bytes() < head_bytes)
            return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than a run's header"};
        extent_t const piece = extent_.slice(0, head_bytes);
        if (piece.length != head_bytes) return {status_code_t::malformed_k, "The run is shorter than its header"};

        auto [reader_channel, reader_channel_status] = channel.acquire();
        if (reader_channel_status.failed()) return reader_channel_status;
        read_pipeline_t reader {volume_->disk(), std::move(reader_channel), extent_.medium};
        if (status_t const offered = reader.enqueue_read(piece, 0); offered.failed()) return offered;
        if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;

        sorted_run_header_t header {};
        status_t moved;
        auto sink = [&](completed_buffer_t const& completed) noexcept {
            if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                moved = whole;
                return;
            }
            std::memcpy(&header, completed.bytes.data(), sizeof(header));
        };
        if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
        if (moved.failed()) return {status_code_t::malformed_k, "A run's header did not read whole"};

        if (header.magic != sorted_run_magic_k) return {status_code_t::malformed_k, "The extent holds no sorted run"};
        if (header.format_version != sorted_run_format_version_k)
            return {status_code_t::unsupported_k, "The run follows another format version"};
        if (header.key_slot_bytes != sizeof(key_slot_t) || header.value_layout != value_layout_k)
            return {status_code_t::malformed_k, "The run holds key slots of another shape"};
        std::uint64_t const keys_per_row = tailed_k ? strings_layout_t::keys_per_row_k
                                                    : sorted_run_layout_t::branching_k;
        if (header.branching != keys_per_row)
            return {status_code_t::malformed_k, "The run's tree rows match no tree of this build"};
        if (tailed_k != (header.layers_bytes >= sizeof(head_layers_header_t)))
            return {status_code_t::malformed_k, "The run's layers do not match its key slots"};
        if (carries_value_offsets_k && header.stored_values_bytes > (std::numeric_limits<stored_value_offset_t>::max)())
            return {status_code_t::malformed_k, "The run claims more values than its offsets reach"};
        if (is_fixed(value_layout_k) && header.stored_values_bytes != header.key_slots_count * header.value_max_size)
            return {status_code_t::malformed_k, "The run's values are not one width per key slot"};
        if (sections_end_(header) > extent_.extent.length)
            return {status_code_t::malformed_k, "The run claims more bytes than its extent holds"};
        if (header.newest_stamp < header.stamps_floor)
            return {status_code_t::malformed_k, "The run's newest stamp is below its floor"};

        header_ = header;
        return {};
    }

    /** Reads the cells and the saved tree of the keys into owned memory, which a short transfer
     *  makes a truncated run, refusing a tree its header or its cells disagree with. */
    status_t read_index_(io_channel_t& channel) noexcept {
        std::uint64_t const count = header_.key_slots_count;
        if constexpr (keeps_cells_k)
            if (status_t const cells = read_cells_(channel); cells.failed()) return cells;
        if (tree_words_.resize(static_cast<std::size_t>(sorted_run_layout_t::keys_section_bytes(count) /
                                                        sizeof(std::uint64_t))) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold the tree of its keys"};
        if (status_t const read = read_bytes_(channel, sorted_run_layout_t::keys_offset(count),
                                              std::as_writable_bytes(std::span<std::uint64_t> {tree_words_}));
            read.failed())
            return read;
        st::expected<keys_view_t> const tree = keys_view_t::make(
            std::as_bytes(std::span<std::uint64_t const> {tree_words_}));
        if (!tree || tree->size() != count) return {status_code_t::malformed_k, "A run's tree disagrees with it"};
        if constexpr (keeps_cells_k)
            for (std::uint64_t position = 0; position != count; ++position)
                if (!(tree->select(static_cast<std::size_t>(position)) ==
                      cells_[static_cast<std::size_t>(position)].stored_key))
                    return {status_code_t::malformed_k, "A run's tree disagrees with its cells"};
        tree_ = *tree;
        return {};
    }

    /** Reads the cells whole into owned memory, which a short transfer makes a truncated run. */
    status_t read_cells_(io_channel_t& channel) noexcept {
        if (cells_.resize(static_cast<std::size_t>(header_.key_slots_count)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its index"};
        std::uint64_t const section = sorted_run_layout_t::index_section_bytes(header_.key_slots_count);
        if (section == 0) return {};

        auto [block, acquire_status] = channel.streams().acquire();
        if (acquire_status.failed()) return acquire_status;
        std::uint64_t const room = round_down_to_multiple(block.size(), portable_alignment_k);
        if (room == 0) return {status_code_t::capacity_exhausted_k, "A channel's streaming block holds no index page"};

        std::uint64_t const wanted = sorted_run_layout_t::index_bytes(header_.key_slots_count);
        byte_t* const into = reinterpret_cast<byte_t*>(cells_.data());
        for (std::uint64_t read = 0; read != section;) {
            std::uint64_t const chunk = room < section - read ? room : section - read;
            extent_t const piece = extent_.slice(sorted_run_layout_t::index_offset() + read, chunk);
            if (piece.length != chunk) return {status_code_t::malformed_k, "The run is shorter than its index"};

            std::array<request_t, 1> const batch {
                read_request(extent_.medium, piece, block.reference(), channel.next_token())};
            auto [taken, submit_status] = volume_->disk().submit(channel.queue_pair(),
                                                                 std::span<request_t const> {batch});
            if (submit_status.failed()) return submit_status;
            if (taken != 1) return {status_code_t::io_k, "The ring would not take an index read"};
            auto [completed, wait_completions_status] = utilities::wait_completions<waiter_type_>(
                volume_->disk(), channel.queue_pair(), channel.completion_buffer());
            if (wait_completions_status.failed()) return wait_completions_status;
            if (completed == 0) return {status_code_t::io_k, "An index read never completed"};
            completion_t const& completion = channel.completion_buffer().front();
            if (!completion.completed_in_full())
                return completion.status.failed()
                           ? completion.status
                           : status_t {status_code_t::malformed_k, "The run is shorter than its index"};

            if (read < wanted) {
                std::uint64_t const usable = wanted - read < chunk ? wanted - read : chunk;
                std::memcpy(into + read, block.bytes().data(), static_cast<std::size_t>(usable));
            }
            read += chunk;
        }
        return {};
    }

    /** Reads the @p into.size() bytes from @p offset of the extent, a whole number of pages,
     *  into @p into. */
    status_t read_bytes_(io_channel_t& channel, std::uint64_t offset, std::span<byte_t> into) noexcept {
        std::uint64_t const section = into.size();
        std::uint64_t const block = round_down_to_multiple(channel.requests().buffer_bytes(), portable_alignment_k);
        if (block == 0) return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than one page"};
        auto [reader_channel, reader_channel_status] = channel.acquire();
        if (reader_channel_status.failed()) return reader_channel_status;
        read_pipeline_t reader {volume_->disk(), std::move(reader_channel), extent_.medium};
        status_t moved;
        auto sink = [&](completed_buffer_t const& completed) noexcept {
            if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                if (moved.succeeded()) moved = whole;
                return;
            }
            std::uint64_t const at = static_cast<std::uint64_t>(completed.tag) * block;
            std::memcpy(into.data() + at, completed.bytes.data(), completed.bytes.size());
        };
        request_tag_t tag = 0;
        for (std::uint64_t read = 0; read != section;) {
            while (read != section && reader.can_enqueue()) {
                std::uint64_t const length = block < section - read ? block : section - read;
                extent_t const piece = extent_.slice(offset + read, length);
                if (piece.length != length) return {status_code_t::malformed_k, "The run is shorter than it claims"};
                if (status_t const offered = reader.enqueue_read(piece, tag++); offered.failed()) return offered;
                read += length;
            }
            if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
            if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
        }
        if (moved.failed()) return {status_code_t::malformed_k, "A run's section did not read whole"};
        return {};
    }

    /** Reads the value offsets whole into owned memory, refusing any its header disagrees with, which a run finding
     *  its values by stride or holding them inline has none of. */
    status_t read_value_offsets_(io_channel_t& channel) noexcept {
        if constexpr (!carries_value_offsets_k) return {};
        else {
            std::uint64_t const count = header_.key_slots_count;
            std::uint64_t const section = round_up_to_multiple(sorted_run_layout_t::value_offsets_bytes(count),
                                                               portable_alignment_k);
            if (value_offsets_.resize(static_cast<std::size_t>(section / sizeof(stored_value_offset_t))) !=
                st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A run could not hold its value offsets"};
            if (status_t const read = read_bytes_(channel, sorted_run_layout_t::value_offsets_offset(count),
                                                  std::span<byte_t> {reinterpret_cast<byte_t*>(value_offsets_.data()),
                                                                     static_cast<std::size_t>(section)});
                read.failed())
                return read;
            // The section's last page is padded past the offsets, which the run keeps no copy of.
            if (value_offsets_.resize(static_cast<std::size_t>(count + 1)) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A run could not hold its value offsets"};
            bool ascending = value_offsets_.front() == 0 && value_offsets_.back() == header_.stored_values_bytes;
            for (std::size_t index = 1; ascending && index != value_offsets_.size(); ++index)
                ascending = value_offsets_[index - 1] <= value_offsets_[index];
            if (!ascending) return {status_code_t::malformed_k, "A run's value offsets disagree with its values"};
            return {};
        }
    }

    /** Reads the heads, the values, the key tape and the layers of a run of strings into owned
     *  memory, refusing any its header disagrees with. */
    status_t read_strings_(io_channel_t& channel) noexcept {
        std::uint64_t const count = header_.key_slots_count;
        std::uint64_t const heads_section = strings_layout_t::values_offset(count) - strings_layout_t::heads_offset();
        if (tree_words_.resize(static_cast<std::size_t>(heads_section / sizeof(std::uint64_t))) !=
            st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its heads"};
        if (status_t const read = read_bytes_(channel, strings_layout_t::heads_offset(),
                                              std::as_writable_bytes(std::span<std::uint64_t> {tree_words_}));
            read.failed())
            return read;
        st::expected<heads_view_t> const heads = heads_view_t::make(
            std::as_bytes(std::span<std::uint64_t const> {tree_words_}));
        if (!heads || heads->size() != count) return {status_code_t::malformed_k, "A run's heads disagree with it"};
        tree_ = *heads;

        std::uint64_t const values_section = strings_layout_t::key_offsets_offset(count) -
                                             strings_layout_t::values_offset(count);
        if (values_.resize(static_cast<std::size_t>(values_section)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its values"};
        if (status_t const read = read_bytes_(channel, strings_layout_t::values_offset(count),
                                              std::span<byte_t> {values_});
            read.failed())
            return read;
        // The section's last page is padded past the values, which the run keeps no copy of.
        if (values_.resize(static_cast<std::size_t>(count * strings_layout_t::value_bytes_k)) !=
            st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its values"};

        std::uint64_t const offsets_section = strings_layout_t::keys_offset(count) -
                                              strings_layout_t::key_offsets_offset(count);
        if (key_offsets_.resize(static_cast<std::size_t>(offsets_section / sizeof(std::uint32_t))) !=
            st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its key offsets"};
        if (status_t const read = read_bytes_(channel, strings_layout_t::key_offsets_offset(count),
                                              std::as_writable_bytes(std::span<std::uint32_t> {key_offsets_}));
            read.failed())
            return read;
        if (key_offsets_.resize(static_cast<std::size_t>(count + 1)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its key offsets"};
        bool ascending = key_offsets_.front() == 0 && key_offsets_.back() == header_.key_tape_bytes;
        for (std::size_t index = 1; ascending && index != key_offsets_.size(); ++index)
            ascending = key_offsets_[index - 1] <= key_offsets_[index];
        if (!ascending) return {status_code_t::malformed_k, "A run's key offsets disagree with its keys"};

        std::uint64_t const keys_section = strings_layout_t::stamps_offset(count, header_.key_tape_bytes) -
                                           strings_layout_t::keys_offset(count);
        if (keys_.resize(static_cast<std::size_t>(keys_section)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its keys"};
        if (status_t const read = read_bytes_(channel, strings_layout_t::keys_offset(count), std::span<byte_t> {keys_});
            read.failed())
            return read;
        if (keys_.resize(static_cast<std::size_t>(header_.key_tape_bytes)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its keys"};
        return read_layers_(channel);
    }

    /** Reads the layers section of a run of strings, @c layers_bytes from past the trailer. */
    status_t read_layers_(io_channel_t& channel) noexcept {
        std::uint64_t const offset = strings_layout_t::layers_offset(header_.key_slots_count, header_.key_tape_bytes,
                                                                     header_.stamps_bytes, header_.kinds_bytes);
        std::uint64_t const section = round_up_to_multiple(header_.layers_bytes, portable_alignment_k);
        st::vector<std::uint64_t> words;
        if (words.resize(static_cast<std::size_t>(section / sizeof(std::uint64_t))) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its layers"};
        if (status_t const read = read_bytes_(channel, offset,
                                              std::as_writable_bytes(std::span<std::uint64_t> {words}));
            read.failed())
            return read;

        head_layers_header_t layers_header;
        std::memcpy(&layers_header, words.data(), sizeof(layers_header));
        std::uint64_t const list_bytes = layers_header.layers_count * sizeof(head_layer_t);
        if (layers_header.layers_count > section / sizeof(head_layer_t) ||
            sizeof(layers_header) + list_bytes + layers_header.trees_bytes != header_.layers_bytes ||
            layers_header.trees_bytes % sizeof(std::uint64_t) != 0)
            return {status_code_t::malformed_k, "A run's layers claim more bytes than they hold"};
        byte_t const* const bytes = reinterpret_cast<byte_t const*>(words.data());
        if (layers_.resize(static_cast<std::size_t>(layers_header.layers_count)) != st::status_t::success_k ||
            layer_words_.resize(static_cast<std::size_t>(layers_header.trees_bytes / sizeof(std::uint64_t))) !=
                st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its layers"};
        if (list_bytes != 0)
            std::memcpy(layers_.data(), bytes + sizeof(layers_header), static_cast<std::size_t>(list_bytes));
        if (layers_header.trees_bytes != 0)
            std::memcpy(layer_words_.data(), bytes + sizeof(layers_header) + list_bytes,
                        static_cast<std::size_t>(layers_header.trees_bytes));

        for (head_layer_t const& layer : layers_) {
            if (layer.heads_offset % sizeof(std::uint64_t) != 0 ||
                layer.heads_offset + layer.heads_bytes > layers_header.trees_bytes ||
                layer.first + layer.count > header_.key_slots_count || layer.depth == 0)
                return {status_code_t::malformed_k, "A run's layer reaches past its run"};
            st::expected<heads_view_t> const heads = heads_view_t::make(layer_bytes_(layer));
            if (!heads || heads->size() != layer.count)
                return {status_code_t::malformed_k, "A run's layer heads disagree with it"};
        }
        return {};
    }

    /** Reads the trailer and unpacks the stamps and the kinds into owned memory, refusing any its header disagrees
     *  with. */
    status_t read_trailer_(io_channel_t& channel) noexcept {
        std::uint64_t const section = round_up_to_multiple(header_.stamps_bytes + header_.kinds_bytes,
                                                           portable_alignment_k);
        std::uint64_t const offset = stamps_offset_(header_);
        st::vector<byte_t> saved;
        if (saved.resize(static_cast<std::size_t>(section)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its trailer"};
        if (status_t const read = read_bytes_(channel, offset, std::span<byte_t> {saved}); read.failed()) return read;

        st::expected<st::immutable_packed_array_view<std::uint64_t>> view =
            st::immutable_packed_array_view<std::uint64_t>::make(
                std::span<byte_t const> {saved.data(), static_cast<std::size_t>(header_.stamps_bytes)});
        if (!view || view->size() != header_.key_slots_count || view->base() != header_.stamps_floor)
            return {status_code_t::malformed_k, "A run's stamps disagree with its header"};
        std::array<std::uint64_t, 64> unpacked {};
        auto fill = [&](std::size_t position) noexcept {
            std::size_t const count = fu::min_of(unpacked.size(), view->size() - position);
            for (std::size_t index = 0; index != count; ++index) unpacked[index] = (*view)[position + index];
            return st::expected<std::span<std::uint64_t const>>(std::span<std::uint64_t const> {unpacked.data(), count},
                                                                st::status_t::success_k);
        };
        st::expected<stamps_column_t> column = stamps_column_t::make(view->size(), header_.stamps_floor,
                                                                     header_.newest_stamp, fill);
        if (!column)
            return column.status() == st::status_t::out_of_memory_heap_k
                       ? status_t {status_code_t::out_of_memory_k, "A run could not hold its stamps"}
                       : status_t {status_code_t::malformed_k, "A run carries a stamp past its newest"};
        stamps_ = std::move(*column);

        st::expected<st::immutable_packed_array_view<std::uint8_t>> kinds =
            st::immutable_packed_array_view<std::uint8_t>::make(std::span<byte_t const> {
                saved.data() + header_.stamps_bytes, static_cast<std::size_t>(header_.kinds_bytes)});
        if (!kinds || kinds->size() != header_.key_slots_count)
            return {status_code_t::malformed_k, "A run's kinds disagree with its header"};
        std::array<std::uint8_t, 64> unpacked_kinds {};
        auto fill_kinds = [&](std::size_t position) noexcept {
            std::size_t const count = fu::min_of(unpacked_kinds.size(), kinds->size() - position);
            for (std::size_t index = 0; index != count; ++index) {
                unpacked_kinds[index] = (*kinds)[position + index];
                if (unpacked_kinds[index] > static_cast<std::uint8_t>(delta_kind_t::change_k))
                    return st::expected<std::span<std::uint8_t const>>(st::status_t::invalid_argument_k);
            }
            return st::expected<std::span<std::uint8_t const>>(
                std::span<std::uint8_t const> {unpacked_kinds.data(), count}, st::status_t::success_k);
        };
        // Rebuilt as wide as it was saved, so the kinds the column can hold read the same.
        std::uint64_t const widest = kinds->bits() != 0 ? (std::uint64_t {1} << kinds->bits()) - 1 : 0;
        st::expected<kinds_column_t> kinds_column = kinds_column_t::make(
            kinds->size(), kinds->base(), static_cast<std::uint8_t>(kinds->base() + widest), fill_kinds);
        if (!kinds_column)
            return kinds_column.status() == st::status_t::out_of_memory_heap_k
                       ? status_t {status_code_t::out_of_memory_k, "A run could not hold its kinds"}
                       : status_t {status_code_t::malformed_k, "A run carries a kind no delta has"};
        kinds_ = std::move(*kinds_column);
        return {};
    }

    /** Reads the range tombstones at the end of the run into owned memory, refusing bounds that
     *  disagree with the header or come out of order. */
    status_t read_range_tombstones_(io_channel_t& channel) noexcept {
        using range_t = typename range_tombstones_t::range_t;
        std::size_t const count = header_.range_tombstones_count;
        if (count == 0) return {};
        st::vector<byte_t> saved;
        if (saved.resize(static_cast<std::size_t>(
                round_up_to_multiple(header_.range_tombstones_bytes, portable_alignment_k))) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its range tombstones"};
        if (status_t const read = read_bytes_(channel, range_tombstones_offset_(header_), std::span<byte_t> {saved});
            read.failed())
            return read;
        st::vector<range_t> ranges;
        st::vector<std::uint64_t> stamps;
        if (ranges.resize(count) != st::status_t::success_k || stamps.resize(count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its range tombstones"};
        std::size_t const stamps_bytes = count * sizeof(std::uint64_t);
        if constexpr (tailed_k) {
            std::size_t const offsets_bytes = (2 * count + 1) * sizeof(std::uint32_t);
            if (stamps_bytes + offsets_bytes > header_.range_tombstones_bytes)
                return {status_code_t::malformed_k, "A run's range tombstones outgrow their section"};
            std::memcpy(stamps.data(), saved.data(), stamps_bytes);
            std::size_t const tape_bytes = header_.range_tombstones_bytes - stamps_bytes - offsets_bytes;
            char const* const tape = reinterpret_cast<char const*>(saved.data() + stamps_bytes + offsets_bytes);
            auto const offset_of = [&](std::size_t slot) noexcept {
                std::uint32_t offset = 0;
                std::memcpy(&offset, saved.data() + stamps_bytes + slot * sizeof(std::uint32_t), sizeof(offset));
                return std::size_t {offset};
            };
            for (std::size_t slot = 0; slot != 2 * count; ++slot)
                if (offset_of(slot) > offset_of(slot + 1) || offset_of(slot + 1) > tape_bytes)
                    return {status_code_t::malformed_k, "A run's range tombstones disagree with their tape"};
            for (std::size_t index = 0; index != count; ++index)
                ranges[index] = range_t {
                    std::string_view {tape + offset_of(2 * index), offset_of(2 * index + 1) - offset_of(2 * index)},
                    std::string_view {tape + offset_of(2 * index + 1),
                                      offset_of(2 * index + 2) - offset_of(2 * index + 1)}};
        }
        else {
            std::size_t const bounds_bytes = count * sizeof(range_t);
            if (bounds_bytes + stamps_bytes != header_.range_tombstones_bytes)
                return {status_code_t::malformed_k, "A run's range tombstones disagree with their section"};
            std::memcpy(ranges.data(), saved.data(), bounds_bytes);
            std::memcpy(stamps.data(), saved.data() + bounds_bytes, stamps_bytes);
        }
        for (std::size_t index = 1; index != ranges.size(); ++index)
            if (ranges[index].start < ranges[index - 1].start)
                return {status_code_t::malformed_k, "A run's range tombstones are out of order"};
        auto [tombstones, tombstones_status] = range_tombstones_t::make(std::span<range_t const> {ranges},
                                                                        std::span<std::uint64_t const> {stamps});
        if (tombstones_status.failed())
            return tombstones_status == status_code_t::out_of_memory_k
                       ? tombstones_status
                       : status_t {status_code_t::malformed_k, "A run's range tombstones end before they start"};
        range_tombstones_ = std::move(tombstones);
        return {};
    }

    /** Where the range tombstones of the run @p header describes start, for either shape of run. */
    [[nodiscard]] static std::uint64_t range_tombstones_offset_(sorted_run_header_t const& header) noexcept {
        if constexpr (tailed_k) return strings_layout_t::range_tombstones_offset(header);
        else return sorted_run_layout_t::range_tombstones_offset(header);
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The volume every read of this run goes through, null until the run is attached or once it is moved from. */
    volume_type_* volume_ = nullptr;

    /** Where the run lies, which every request of it is sliced out of. */
    medium_extent_t extent_;

    /** The header of the run, from which every offset inside the extent follows. */
    sorted_run_header_t header_ {};

    /** The sorted cells, read whole at open for a run whose cells carry values, none otherwise. */
    st::vector<key_slot_t> cells_;

    /** One offset per key slot into the values section and one past the last, read whole at open, none for a run
     *  finding its values by stride or holding them inline. */
    st::vector<stored_value_offset_t> value_offsets_;

    /** The stamp of every key slot in sorted order, packed from the run's floor, read whole at open. */
    stamps_column_t stamps_;

    /** The @c delta_kind_t of every key slot in sorted order, packed from the least present, read whole at open. */
    kinds_column_t kinds_;

    /** The saved S+ tree of the keys, or of a run of strings' heads, read whole at open,
     *  word-aligned for the view. */
    st::vector<std::uint64_t> tree_words_;

    /** The view over @c tree_words_, whose buffer a move hands over unchanged. */
    keys_view_t tree_ {};

    /** One 16-byte value per key of a run of strings in sorted order, read whole at open. */
    st::vector<byte_t> values_;

    /** One offset per key of a run of strings into @c keys_ and one past the last, read
     *  whole at open. */
    st::vector<std::uint32_t> key_offsets_;

    /** Every key of a run of strings whole, back to back in sorted order, read whole at open. */
    st::vector<byte_t> keys_;

    /** The layers of a run of strings, by first position and then by depth, read whole at open. */
    st::vector<head_layer_t> layers_;

    /** The saved S+ trees of those layers' heads, word-aligned for the views over them. */
    st::vector<std::uint64_t> layer_words_;

    /** The stamp @c assign_stamp gave the run, zero while its stamps are the ones
     *  its extent carries. */
    std::uint64_t assigned_stamp_ = 0;

    /** The range tombstones the run carries, read whole at open. */
    range_tombstones_t range_tombstones_;

#pragma endregion Private Members
};

#pragma endregion Sorted Run

} // namespace unum::ustore::blobs
