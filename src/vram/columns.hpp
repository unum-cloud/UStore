/**
 *  @file src/vram/columns.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The output columns of every read and listing verb, and the uploader moving them from the bounce into a
 *      device arena, which both upload towers end with.
 *
 *  Each column is carved off the call's device arena, copied there once and repointed, so the caller's outputs name
 *  device memory. The upload tower moves a host verb's outputs this way, as does a filtered scan on a device.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uintptr_t`

#include <span> // `std::span`, `std::as_bytes`

#include "ustore/docs/json_values.hpp"      // `field_size_bytes`
#include "ustore/utilities/ram_buffers.hpp" // `arena_copy`, `device_blocks_t`, `divide_round_up`
#include "ustore/utilities/vram_device.hpp" // `carve`, `gpu_executor_concept`

namespace unum::ustore {

/** Moves output columns from the bounce into one device arena: each is carved there, copied once and repointed. */
template <gpu_executor_concept executor_type_>
class column_uploader {
  public:
    column_uploader(utilities::device_blocks_t& blocks, executor_type_& executor) noexcept
        : blocks_(blocks), executor_(executor) {}

    /** Moves the @p count elements @p column points to into the arena, leaving an empty column null as the host does,
     *  and repoints it there. */
    template <typename element_type_>
    void move(element_type_ const*& column, std::size_t count) noexcept {
        if (status_.failed() || !column) return;
        if (!count) {
            column = nullptr;
            return;
        }
        auto [device, device_status] = utilities::carve<element_type_>(blocks_, executor_, count);
        if ((status_ = device_status).failed()) return;
        std::span<element_type_ const> const host {column, count};
        status_ = executor_.upload_async(reinterpret_cast<std::uintptr_t>(device.data()), std::as_bytes(host));
        column = device.data();
    }

    /** Moves the column an output points to, where a null output was not asked for. */
    template <typename element_type_>
    void move_output(element_type_ const** output, std::size_t count) noexcept {
        if (output) move(*output, count);
    }

    /** Moves a tape of @p count strings, its offsets and then the bytes they span. */
    void move(ustore_strings_view_t& strings, std::size_t count) noexcept {
        if (!count) return;
        if (strings.layout == ustore_strings_u32_tape_k) move_tape(strings.data.u32_tape, count);
        else if (strings.layout == ustore_strings_u64_tape_k) move_tape(strings.data.u64_tape, count);
    }

    /** Copies @p count elements of @p column into the bounce, so the copy can be repointed before it moves. */
    template <typename element_type_>
    std::span<element_type_> copy_into_bounce(element_type_ const* column, std::size_t count) noexcept {
        if (status_.failed()) return {};
        auto [copy, copy_status] = utilities::arena_copy<element_type_>(executor_.bounce(), {column, count});
        if ((status_ = copy_status).failed()) return {};
        status_ = executor_.pin_bounce();
        return copy;
    }

    status_t status() const noexcept { return status_; }

  private:
    template <typename tape_type_>
    void move_tape(tape_type_& tape, std::size_t count) noexcept {
        std::size_t const bytes = tape.offsets ? static_cast<std::size_t>(tape.offsets[count]) : 0;
        move(tape.data, bytes);
        move(tape.offsets, count + 1);
    }

    utilities::device_blocks_t& blocks_;
    executor_type_& executor_;
    status_t status_;
};

/** How many entries the @p count + 1 offsets an @p output points to list, read on the host before they move. */
inline std::size_t listed_by(ustore_length_t const* const* output, ustore_size_t count) noexcept {
    return output && *output ? static_cast<std::size_t>((*output)[count]) : 0;
}

/** The output columns of a read: presence bits, lengths and values, one per task. */
template <typename arguments_type_, typename uploader_type_>
    requires requires(arguments_type_ const& arguments) {
        arguments.presences;
        arguments.lengths;
        arguments.values;
    }
void output_columns(arguments_type_ const& arguments, uploader_type_& uploader) noexcept {
    std::size_t const count = static_cast<std::size_t>(arguments.count);
    uploader.move_output(arguments.presences, divide_round_up(count, bits_in_byte_k));
    uploader.move_output(arguments.lengths, count);
    if (arguments.values) uploader.move(*arguments.values, count);
}

/** The output columns of a listing: offsets per task, and the keys, with their scores where asked, they split. */
template <typename arguments_type_, typename uploader_type_>
    requires requires(arguments_type_ const& arguments) {
        arguments.offsets;
        arguments.keys;
    }
void output_columns(arguments_type_ const& arguments, uploader_type_& uploader) noexcept {
    std::size_t const listed = listed_by(arguments.offsets, arguments.count);
    uploader.move_output(arguments.keys, listed);
    if constexpr (requires { arguments.scores; }) uploader.move_output(arguments.scores, listed);
    uploader.move_output(arguments.offsets, static_cast<std::size_t>(arguments.count) + 1);
}

/** The output columns of a strings read: presence bits and one key per string. */
template <typename uploader_type_>
void output_columns(ustore_strings_read_t const& arguments, uploader_type_& uploader) noexcept {
    std::size_t const count = static_cast<std::size_t>(arguments.count);
    uploader.move_output(arguments.presences, divide_round_up(count, bits_in_byte_k));
    uploader.move_output(arguments.values, count);
}

/** The output columns of a strings scan: offsets per task, and the strings, with their scores where asked. */
template <typename uploader_type_>
void output_columns(ustore_strings_scan_t const& arguments, uploader_type_& uploader) noexcept {
    std::size_t const listed = listed_by(arguments.offsets, arguments.count);
    if (arguments.strings) uploader.move(*arguments.strings, listed);
    uploader.move_output(arguments.scores, listed);
    uploader.move_output(arguments.offsets, static_cast<std::size_t>(arguments.count) + 1);
}

/** The output columns of an edges read: degrees per vertex, and the edges, three keys each, their offsets split. */
template <typename uploader_type_>
void output_columns(ustore_graph_read_edges_t const& arguments, uploader_type_& uploader) noexcept {
    std::size_t const count = static_cast<std::size_t>(arguments.count);
    uploader.move_output(arguments.degrees_per_vertex, count);
    uploader.move_output(arguments.edges_per_vertex, 3 * listed_by(arguments.edges_offsets, arguments.count));
    uploader.move_output(arguments.edges_offsets, count + 1);
}

/** The output columns of a gather: every field's three bitmaps, scalars and strings, then the views naming them,
 *  repointed in a copy before they move. */
template <typename uploader_type_>
void output_columns(ustore_docs_gather_t const& arguments, uploader_type_& uploader) noexcept {
    if (!arguments.columns || !*arguments.columns) return;
    std::size_t const rows = static_cast<std::size_t>(arguments.count);
    std::size_t const fields = static_cast<std::size_t>(arguments.fields_count);
    std::size_t const bitmap_bytes = divide_round_up(rows, bits_in_byte_k);
    std::span<ustore_docs_column_view_t> const views = uploader.copy_into_bounce(*arguments.columns, fields);
    for (std::size_t field = 0; field != views.size(); ++field) {
        ustore_docs_column_view_t& view = views[field];
        uploader.move(view.validities, bitmap_bytes);
        uploader.move(view.conversions, bitmap_bytes);
        uploader.move(view.collisions, bitmap_bytes);
        uploader.move(view.scalars, rows * docs::field_size_bytes(arguments.outputs_types[field]));
        uploader.move(view.strings, rows);
    }
    ustore_docs_column_view_t const* moved = views.data();
    uploader.move(moved, views.size());
    *arguments.columns = moved;
}

/** Moves every output column of @p hosted, which a host verb wrote into @p executor's bounce, into @p blocks, the
 *  call's device arena, one copy per column, and waits once. */
template <typename arguments_type_, gpu_executor_concept executor_type_>
status_t upload_output_columns(arguments_type_ const& hosted, utilities::device_blocks_t& blocks,
                               executor_type_& executor) noexcept {
    if (status_t const pinned = executor.pin_bounce(); pinned.failed()) return pinned;
    column_uploader<executor_type_> uploader {blocks, executor};
    output_columns(hosted, uploader);
    // Copies already queued read the bounce, which the next call rewinds, so a failure still waits for them
    status_t const waited = executor.wait();
    return uploader.status().failed() ? uploader.status() : waited;
}

} // namespace unum::ustore
