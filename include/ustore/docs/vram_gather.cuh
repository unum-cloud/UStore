/**
 *  @file include/ustore/docs/vram_gather.cuh
 *  @author Ash Vardanian
 *  @date November 5, 2026
 *  @brief The documents of a gather or a field read as a device holds them, the matches its
 *      walks stored, and the steps its kernels share, constexpr so the host checks them against
 *      its own gather.
 *
 *  The shared steps find a cell's token, convert eight rows of a scalar column, and measure and
 *  write the rows of a text output. Every document keeps as many match slots as the call compiled
 *  paths, as a path entering only first members selects at most one node of a document, and a walk
 *  stores how many it filled. A cell is the token its row's document holds under its column's path,
 *  or under its row's own path for a read. One lane converts eight rows, so it writes one byte of
 *  each bitmap whole. A text output takes two launches around the offsets scan of the tape kernels:
 *  the first measures every row, the second writes it where its offset says. The Arrow export of a
 *  column takes one more pass where Arrow lays it out otherwise: booleans packed into bits, and
 *  texts without the null byte ending every row.
 */

#pragma once

#include <cstdint> // `std::uint8_t`, `std::uint16_t`, `std::uint32_t`, `std::uint64_t`

#include <array>       // `std::array`
#include <bit>         // `std::bit_cast`
#include <string_view> // `std::string_view`

#include <stringzilla/types.h> // `sz_match_t`

#include "ustore/docs/json_cells.hpp" // `cell_scalar`, `cell_half`, `cell_uuid`, `cell_text`, `record_conversion`
#include "ustore/utilities/tapes.hpp" // `tape_source_t`

namespace unum::ustore::docs {

/** The path a row reads where its field compiles into none, so its cell is missing. */
inline constexpr std::uint32_t no_cell_path_k = 0xFFFFFFFFu;

/** Rows one lane of a gather converts, one byte of every bitmap. */
inline constexpr std::uint32_t gather_lane_rows_k = 8;

/** Threads of every gather kernel's block. */
inline constexpr unsigned gather_block_threads_k = 256;

/** The documents of a device gather, one per row, and the matches the walks stored for each. */
struct gather_page_t {

    /** The bytes of every document, back to back. */
    char const* bytes = nullptr;

    /** One offset per document and one past the last. */
    std::uint64_t const* offsets = nullptr;

    /** @c paths_count match slots per document, of which @c found fill the first. */
    sz_match_t const* matches = nullptr;

    /** How many slots of every document a walk filled, zero for one no walk read. */
    std::uint32_t const* found = nullptr;

    /** Paths the call compiled, the match slots of every document. */
    std::uint32_t paths_count = 0;

    /** Documents, one per row of every output. */
    std::uint32_t rows = 0;

    /** Raised by a lane meeting a cell only the host converts. */
    std::uint32_t* refused = nullptr;

    /** The token of @p row's document under @p path, empty where document or field is missing. */
    constexpr std::string_view cell(std::uint32_t row, std::uint32_t path) const noexcept {
        if (path == no_cell_path_k) return {};
        sz_match_t const* const slots = matches + std::uint64_t {row} * paths_count;
        for (std::uint32_t slot = 0; slot != found[row]; ++slot)
            if (slots[slot].needle_index == path)
                return {bytes + offsets[row] + slots[slot].byte_offset,
                        static_cast<std::size_t>(slots[slot].byte_length)};
        return {};
    }
};

/** One scalar column of a device gather: its three bitmaps @c bitmap_stride bytes apart, its
 *  scalars, its type and its field's path. */
struct gather_scalars_t {
    ustore_octet_t* bitmaps = nullptr;
    ustore_byte_t* scalars = nullptr;
    std::uint64_t bitmap_stride = 0;
    ustore_docs_field_type_t type = ustore_docs_field_type_null_k;
    std::uint32_t path = no_cell_path_k;
};

/** One text output of a device gather or field read: how it prints, which path every row reads, and
 *  where it writes each row's bits, length and bytes. */
struct gather_texts_t {

    /** How the rows print. */
    cell_text_t text = cell_text_t::column_text_k;

    /** The path of a column's every row. */
    std::uint32_t path = no_cell_path_k;

    /** The path of every row of a read, or null for a column. */
    std::uint32_t const* paths = nullptr;

    /** A column's three bitmaps, @c bitmap_stride bytes apart, or null for a read. */
    ustore_octet_t* bitmaps = nullptr;
    std::uint64_t bitmap_stride = 0;

    /** A read's presence bits and lengths, each null where it was not asked for. */
    ustore_octet_t* presences = nullptr;
    ustore_length_t* lengths = nullptr;

    /** Every row's length, which the tape kernels scan into @c offsets. */
    utilities::tape_source_t* measured = nullptr;

    /** Where every row's bytes start, and the bytes, written once the measured rows are scanned. */
    std::uint64_t const* offsets = nullptr;
    char* bytes = nullptr;

    /** The path @p row reads. */
    constexpr std::uint32_t path_of(std::uint32_t row) const noexcept { return paths ? paths[row] : path; }
};

/** Writes @p scalar as bytes at @p row of @p scalars, as devices store no wider type via bytes. */
template <typename scalar_type_>
constexpr void put_scalar(ustore_byte_t* scalars, std::uint32_t row, scalar_type_ scalar) noexcept {
    std::array<ustore_byte_t, sizeof(scalar_type_)> const bytes =
        std::bit_cast<std::array<ustore_byte_t, sizeof(scalar_type_)>>(scalar);
    for (std::size_t index = 0; index != bytes.size(); ++index)
        scalars[row * sizeof(scalar_type_) + index] = bytes[index];
}

/** Converts the rows of @p column from @p first_row on, @c gather_lane_rows_k of them or as many as
 *  remain, through @p convert, answering whether one is only the host's to convert. */
template <typename convert_type_>
constexpr std::uint32_t convert_scalar_rows(gather_page_t const& page, gather_scalars_t const& column,
                                            std::uint32_t first_row, convert_type_ const& convert) noexcept {
    ustore_octet_t valid = 0, converted = 0, collides = 0;
    std::uint32_t refused = 0;
    std::uint32_t const end = page.rows - first_row < gather_lane_rows_k ? page.rows : first_row + gather_lane_rows_k;
    for (std::uint32_t row = first_row; row != end; ++row) {
        conversion_t const conversion = convert(page.cell(row, column.path), row, refused);
        record_conversion(conversion, static_cast<ustore_octet_t>(1u << (row - first_row)), valid, converted, collides);
    }
    std::uint64_t const slot = first_row / gather_lane_rows_k;
    column.bitmaps[slot] = valid;
    column.bitmaps[column.bitmap_stride + slot] = converted;
    column.bitmaps[2 * column.bitmap_stride + slot] = collides;
    return refused;
}

/** Converts the rows of a column of @p scalar_type_ as @c gather_scalar does. */
template <typename scalar_type_>
constexpr std::uint32_t gather_scalar_rows_as(gather_page_t const& page, gather_scalars_t const& column,
                                              std::uint32_t first_row) noexcept {
    return convert_scalar_rows(page, column, first_row,
                               [&](std::string_view token, std::uint32_t row, std::uint32_t& refused) noexcept {
                                   scalar_type_ scalar {};
                                   conversion_t const conversion = cell_scalar(token, scalar, refused);
                                   put_scalar(column.scalars, row, scalar);
                                   return conversion;
                               });
}

/** Converts the rows of @p column from @p first_row on into its scalars and one byte of
 *  each bitmap, as the host gather converts them, answering whether one is only the
 *  host's to convert. */
constexpr std::uint32_t gather_scalar_rows(gather_page_t const& page, gather_scalars_t const& column,
                                           std::uint32_t first_row) noexcept {
    switch (column.type) {
    case ustore_docs_field_type_bool_k: return gather_scalar_rows_as<bool>(page, column, first_row);
    case ustore_docs_field_type_f64_k: return gather_scalar_rows_as<double>(page, column, first_row);
    case ustore_docs_field_type_f32_k: return gather_scalar_rows_as<float>(page, column, first_row);
    case ustore_docs_field_type_i64_k: return gather_scalar_rows_as<std::int64_t>(page, column, first_row);
    case ustore_docs_field_type_i32_k: return gather_scalar_rows_as<std::int32_t>(page, column, first_row);
    case ustore_docs_field_type_i16_k: return gather_scalar_rows_as<std::int16_t>(page, column, first_row);
    case ustore_docs_field_type_i8_k: return gather_scalar_rows_as<std::int8_t>(page, column, first_row);
    case ustore_docs_field_type_u64_k: return gather_scalar_rows_as<std::uint64_t>(page, column, first_row);
    case ustore_docs_field_type_u32_k: return gather_scalar_rows_as<std::uint32_t>(page, column, first_row);
    case ustore_docs_field_type_u16_k: return gather_scalar_rows_as<std::uint16_t>(page, column, first_row);
    case ustore_docs_field_type_u8_k: return gather_scalar_rows_as<std::uint8_t>(page, column, first_row);
    case ustore_docs_field_type_f16_k:
        return convert_scalar_rows(page, column, first_row,
                                   [&](std::string_view token, std::uint32_t row, std::uint32_t& refused) noexcept {
                                       std::uint16_t half = 0;
                                       conversion_t const conversion = cell_half(token, half, refused);
                                       put_scalar(column.scalars, row, half);
                                       return conversion;
                                   });
    case ustore_docs_field_type_uuid_k:
        return convert_scalar_rows(page, column, first_row,
                                   [&](std::string_view token, std::uint32_t row, std::uint32_t& refused) noexcept {
                                       std::array<std::uint8_t, uuid_bytes_k> uuid {};
                                       conversion_t const conversion = cell_uuid(token, uuid, refused);
                                       put_scalar(column.scalars, row, uuid);
                                       return conversion;
                                   });
    default: return 1;
    }
}

/** Measures the rows of @p texts from @p first_row on, @c gather_lane_rows_k of them or as many as
 *  remain: each row's length for the offsets scan, and a column's bitmaps or a read's presences and
 *  lengths, answering whether one is only the host's to print. */
constexpr std::uint32_t measure_text_rows(gather_page_t const& page, gather_texts_t const& texts,
                                          std::uint32_t first_row) noexcept {
    ustore_octet_t valid = 0, converted = 0, collides = 0, present = 0;
    std::uint32_t refused = 0;
    std::uint32_t const end = page.rows - first_row < gather_lane_rows_k ? page.rows : first_row + gather_lane_rows_k;
    for (std::uint32_t row = first_row; row != end; ++row) {
        std::string_view const token = page.cell(row, texts.path_of(row));
        text_writer_t writer;
        conversion_t const conversion = cell_text(token, texts.text, writer, refused);
        ustore_octet_t const mask = static_cast<ustore_octet_t>(1u << (row - first_row));
        record_conversion(conversion, mask, valid, converted, collides);
        present = static_cast<ustore_octet_t>(present | (token.empty() ? 0 : mask));
        texts.measured[row].length = writer.length;
        if (texts.lengths) texts.lengths[row] = writer.length;
    }
    std::uint64_t const slot = first_row / gather_lane_rows_k;
    if (texts.bitmaps) {
        texts.bitmaps[slot] = valid;
        texts.bitmaps[texts.bitmap_stride + slot] = converted;
        texts.bitmaps[2 * texts.bitmap_stride + slot] = collides;
    }
    if (texts.presences) texts.presences[slot] = present;
    return refused;
}

/** Writes row @p row of @p texts where its offset says, once every row was measured and scanned. */
constexpr void write_text_row(gather_page_t const& page, gather_texts_t const& texts, std::uint32_t row) noexcept {
    std::uint32_t refused = 0;
    text_writer_t writer {texts.bytes + texts.offsets[row], 0};
    [[maybe_unused]] conversion_t const conversion = cell_text(page.cell(row, texts.path_of(row)), texts.text, writer,
                                                               refused);
}

/** Packs the eight booleans of @p bools from row @p first_row on, as many as @p rows leaves, into
 *  one byte of bits, the first row in the lowest, as Arrow packs a boolean column. */
constexpr ustore_octet_t pack_bools(ustore_byte_t const* bools, std::uint64_t rows, std::uint64_t first_row) noexcept {
    ustore_octet_t packed = 0;
    std::uint64_t const end = rows - first_row < gather_lane_rows_k ? rows : first_row + gather_lane_rows_k;
    for (std::uint64_t row = first_row; row != end; ++row)
        packed = static_cast<ustore_octet_t>(packed | (bools[row] ? 1u << (row - first_row) : 0u));
    return packed;
}

/** A text column whose every row ends in a null byte, and the column Arrow takes without them,
 *  behind offsets of @p offset_type_ counted from the first row. */
template <typename offset_type_>
struct terminated_texts {
    ustore_byte_t const* bytes = nullptr;
    offset_type_ const* offsets = nullptr;
    ustore_byte_t* bare_bytes = nullptr;
    offset_type_* bare_offsets = nullptr;
    std::uint64_t rows = 0;
};

/** Copies row @p row of @p texts without its null byte, writing its offset, and after the last row
 *  the closing one. */
template <typename offset_type_>
constexpr void drop_terminator(terminated_texts<offset_type_> const& texts, std::uint64_t row) noexcept {
    offset_type_ const first = texts.offsets[0];
    offset_type_ const begin = texts.offsets[row], end = texts.offsets[row + 1];
    offset_type_ const bare_begin = static_cast<offset_type_>(begin - first - row);
    texts.bare_offsets[row] = bare_begin;
    if (row + 1 == texts.rows) texts.bare_offsets[texts.rows] = static_cast<offset_type_>(end - first - texts.rows);
    for (offset_type_ byte = begin; byte + 1 < end; ++byte)
        texts.bare_bytes[bare_begin + (byte - begin)] = texts.bytes[byte];
}

#if defined(__CUDACC__) || defined(__HIP__)

/** The index of this thread among a gather launch's threads. */
__device__ inline std::uint32_t gather_thread_slot() noexcept { return blockIdx.x * blockDim.x + threadIdx.x; }

/** Converts eight rows of @p column per lane, refusing the page where only the host converts. */
__device__ inline void gather_scalars(gather_page_t const& page, gather_scalars_t const& column) noexcept {
    std::uint32_t const first_row = gather_thread_slot() * gather_lane_rows_k;
    if (first_row >= page.rows) return;
    if (gather_scalar_rows(page, column, first_row)) atomicOr(page.refused, 1u);
}

/** Measures eight rows of @p texts per lane, refusing the page where only the host prints. */
__device__ inline void measure_texts(gather_page_t const& page, gather_texts_t const& texts) noexcept {
    std::uint32_t const first_row = gather_thread_slot() * gather_lane_rows_k;
    if (first_row >= page.rows) return;
    if (measure_text_rows(page, texts, first_row)) atomicOr(page.refused, 1u);
}

/** Writes one row of @p texts per lane. */
__device__ inline void write_texts(gather_page_t const& page, gather_texts_t const& texts) noexcept {
    std::uint32_t const row = gather_thread_slot();
    if (row < page.rows) write_text_row(page, texts, row);
}

/** Packs eight of @p rows booleans into one byte of @p packed per lane. */
__device__ inline void pack_bool_bytes(ustore_byte_t const* bools, std::uint64_t rows,
                                       ustore_octet_t* packed) noexcept {
    std::uint64_t const first_row = std::uint64_t {gather_thread_slot()} * gather_lane_rows_k;
    if (first_row < rows) packed[first_row / gather_lane_rows_k] = pack_bools(bools, rows, first_row);
}

/** Copies one row of @p texts per lane without its null byte. */
template <typename offset_type_>
__device__ inline void drop_terminators(terminated_texts<offset_type_> const& texts) noexcept {
    std::uint64_t const row = gather_thread_slot();
    if (row < texts.rows) drop_terminator(texts, row);
}

#endif

} // namespace unum::ustore::docs
