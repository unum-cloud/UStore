/**
 *  @file cpp/ustore/arrow.hpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief Views of Apache Arrow columns as the keys and strings every call takes, and in-place
 *      exports of the keys, strings and gathered columns a call wrote into an arena.
 */

#pragma once

#include <utility> // `std::swap`

#include "ustore/arrow.h"
#include "ustore/docs_table.hpp" // `dynamic_column_view_t`
#include "ustore/ranges.hpp"     // `keys_view_t`, `values_view_t`
#include "ustore/status.hpp"     // `status_t`, `expected`
#include "ustore/types.hpp"      // `arena_t`

namespace unum::ustore {

/** The status a converter or an exporter of @c ustore/arrow.h returned, with its C name as the
 *  message. */
inline status_t arrow_status_(ustore_status_t status) noexcept {
    return status_t {static_cast<status_code_t>(status), ustore_status_name(status)};
}

/**
 *  @brief One column exported as Arrow, reading the arena it was exported from until that arena's
 *      next call or free, which releases its own nodes on destruction.
 *
 *  It moves but never copies, as Arrow moves a structure by copying it and marking the source
 *  released, and a consumer taking ownership of @c schema and @c array does the same.
 */
class arrow_export_t {
    ArrowSchema schema_ {};
    ArrowDeviceArray array_ {};

  public:
    arrow_export_t() noexcept = default;
    arrow_export_t(arrow_export_t const&) = delete;
    arrow_export_t& operator=(arrow_export_t const&) = delete;
    arrow_export_t(arrow_export_t&& other) noexcept : schema_(other.schema_), array_(other.array_) {
        other.schema_.release = nullptr;
        other.array_.array.release = nullptr;
    }
    arrow_export_t& operator=(arrow_export_t&& other) noexcept {
        std::swap(schema_, other.schema_);
        std::swap(array_, other.array_);
        return *this;
    }
    ~arrow_export_t() noexcept {
        if (schema_.release) schema_.release(&schema_);
        if (array_.array.release) array_.array.release(&array_.array);
    }

    /** The column's type, whose @c name is empty for the caller to point at its own. */
    ArrowSchema& schema() noexcept { return schema_; }

    /** The column's buffers and the device holding them. */
    ArrowDeviceArray& array() noexcept { return array_; }
};

/**
 *  @brief Views the 16-byte keys of one Arrow @c fixed_size_binary(16) column in place, tagged
 *      @c arrow.uuid or not, for as long as the column lives.
 *
 *  @par Statuses:
 *  - @c status_code_t::invalid_argument_k A missing or released input, or one holding a null.
 *  - @c status_code_t::unsupported_k Another format, a dictionary or a view.
 *  - @c status_code_t::malformed_k A negative length or offset, or a missing buffer.
 */
inline expected<keys_view_t> keys_view_from_arrow(ArrowSchema const& schema, ArrowArray const& array) noexcept {
    ustore_keys_view_t keys {};
    ustore_size_t count = 0;
    if (ustore_status_t const status = ustore_keys_view_from_arrow(&schema, &array, &keys, &count);
        status != ustore_status_success_k)
        return arrow_status_(status);
    return keys_view_t {{keys.data, keys.stride}, count};
}

/** Views the byte strings of one Arrow @c binary, @c utf8, @c large_binary or @c large_utf8
 *  column in place as a tape, for as long as the column lives, refusing what
 *  @c keys_view_from_arrow refuses. */
inline expected<values_view_t> strings_view_from_arrow(ArrowSchema const& schema, ArrowArray const& array) noexcept {
    ustore_strings_view_t strings {};
    ustore_size_t count = 0;
    if (ustore_status_t const status = ustore_strings_view_from_arrow(&schema, &array, &strings, &count);
        status != ustore_status_success_k)
        return arrow_status_(status);
    return values_view_t {count, nullptr, strings};
}

/**
 *  @brief Exports @p keys a call wrote into @p arena as one Arrow @c fixed_size_binary(16) column
 *      tagged @c arrow.uuid, in place, copying keys of another stride dense into @p arena first.
 *
 *  @par Statuses:
 *  - @c status_code_t::invalid_argument_k Dense keys lie outside @p arena.
 *  - @c status_code_t::contended_k Keys of another stride met a call filling @p arena.
 *  - @c status_code_t::device_k No executor spawns on the arena's device, or it has no kernels.
 *  - @c status_code_t::out_of_memory_k An allocation failed.
 */
inline expected<arrow_export_t> keys_view_to_arrow(keys_view_t keys, arena_t& arena) noexcept {
    arrow_export_t exported;
    ustore_keys_view_t const view {keys.data(), keys.stride()};
    if (ustore_status_t const status = ustore_keys_view_to_arrow(view, keys.size(), arena, &exported.schema(),
                                                                 &exported.array());
        status != ustore_status_success_k)
        return arrow_status_(status);
    return exported;
}

/**
 *  @brief Exports the @p count strings of a @p tape a call wrote into @p arena as one Arrow
 *      column of @p layout, @c binary for 32-bit offsets and @c large_binary for 64-bit ones, in
 *      place when the tape already has it.
 *
 *  @par Statuses:
 *  - @c status_code_t::invalid_argument_k An accessor or an unknown @p layout.
 *  - @c status_code_t::contended_k A tape of the other width met a call filling @p arena.
 *  - @c status_code_t::capacity_exhausted_k The strings span more bytes than 32-bit offsets reach.
 *  - @c status_code_t::device_k No executor spawns on the arena's device, or it has no kernels.
 */
inline expected<arrow_export_t> strings_view_to_arrow(ustore_strings_view_t tape, std::size_t count,
                                                      ustore_strings_layout_t layout, arena_t& arena) noexcept {
    arrow_export_t exported;
    if (ustore_status_t const status = ustore_strings_view_to_arrow(tape, count, layout, arena, &exported.schema(),
                                                                    &exported.array());
        status != ustore_status_success_k)
        return arrow_status_(status);
    return exported;
}

/** Exports @p values a call wrote into @p arena as one Arrow column in the layout of their tape,
 *  in place, leaving out their presence bits. */
inline expected<arrow_export_t> strings_view_to_arrow(values_view_t const& values, arena_t& arena) noexcept {
    ustore_strings_view_t const tape = values.tape();
    return strings_view_to_arrow(tape, values.size(), tape.layout, arena);
}

/**
 *  @brief Exports one column a gather wrote into @p arena as an Arrow column whose validity buffer
 *      is the column's validities, in place where Arrow lays the column out alike.
 *
 *  @par Statuses:
 *  - @c status_code_t::invalid_argument_k The column's type is no column type, or it names memory
 *    outside @p arena.
 *  - @c status_code_t::contended_k A call fills @p arena, or another export carves it.
 *  - @c status_code_t::device_k No executor spawns on the arena's device, or it has no kernels.
 *  - @c status_code_t::out_of_memory_k An allocation failed.
 */
inline expected<arrow_export_t> docs_column_view_to_arrow(dynamic_column_view_t const& column,
                                                          arena_t& arena) noexcept {
    arrow_export_t exported;
    if (ustore_status_t const status = ustore_docs_column_view_to_arrow(&column.arrays(), column.size(), column.type(),
                                                                        arena, &exported.schema(), &exported.array());
        status != ustore_status_success_k)
        return arrow_status_(status);
    return exported;
}

} // namespace unum::ustore
