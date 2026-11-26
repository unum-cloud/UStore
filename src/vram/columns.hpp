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

#include <concepts> // `std::convertible_to`
#include <span>     // `std::span`, `std::as_bytes`

#include "ustore/docs/json_cells.hpp"       // `field_size_bytes`
#include "ustore/utilities/ram_buffers.hpp" // `arena_copy`, `device_blocks_t`, `divide_round_up`
#include "ustore/utilities/vram_device.hpp" // `carve`, `a_gpu_stream`

#include "utilities/arguments.hpp" // `a_call_answering_scores`, `a_call_holding_keys`, `a_call_holding_values`

namespace unum::ustore {

/** Moves output columns from the bounce into one device arena: each is carved there, copied once and repointed. */
template <util::a_gpu_stream executor_type_>
class column_uploader {
  public:
    column_uploader(util::device_blocks_t& blocks, executor_type_& executor) noexcept
        : blocks_(blocks), executor_(executor) {}

    /** Moves the @p count elements @p column points to into the arena, answering where they
     *  landed, or nothing for an empty column, which the host leaves null. */
    template <typename element_type_>
    [[nodiscard]] std::span<element_type_ const> move(element_type_ const* column, std::size_t count) noexcept {
        if (status_.failed() || !column || !count) return {};
        auto [device, device_status] = util::carve<element_type_>(blocks_, executor_, count);
        if ((status_ = device_status).failed()) return {};
        std::span<element_type_ const> const host {column, count};
        status_ = executor_.upload_async(reinterpret_cast<std::uintptr_t>(device.data()), std::as_bytes(host));
        return device;
    }

    /** Moves the column an output points to, where a null output was not asked for. */
    template <typename element_type_>
    void move_output(element_type_ const** output, std::size_t count) noexcept {
        if (output) *output = move(*output, count).data();
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
        auto [copy, copy_status] = util::arena_copy<element_type_>(executor_.memory.bounce.arena(), {column, count});
        if ((status_ = copy_status).failed()) return {};
        status_ = executor_.memory.bounce.pin();
        return copy;
    }

    util::status_t status() const noexcept { return status_; }

  private:
    template <typename tape_type_>
    void move_tape(tape_type_& tape, std::size_t count) noexcept {
        std::size_t const bytes = tape.offsets ? static_cast<std::size_t>(tape.offsets[count]) : 0;
        tape.data = move(tape.data, bytes).data();
        tape.offsets = move(tape.offsets, count + 1).data();
    }

    util::device_blocks_t& blocks_;
    executor_type_& executor_;
    util::status_t status_;
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
    uploader.move_output(arguments.presences, util::divide_round_up(count, util::bits_in_byte_k));
    uploader.move_output(arguments.lengths, count);
    if (arguments.values) uploader.move(*arguments.values, count);
}

/** The output columns of a listing: offsets per task, and the keys, with their scores where asked, they split. */
template <typename arguments_type_, typename uploader_type_>
    requires requires(arguments_type_ const& arguments) {
        { *arguments.offsets } -> std::convertible_to<ustore_length_t const*>;
        arguments.keys;
    }
void output_columns(arguments_type_ const& arguments, uploader_type_& uploader) noexcept {
    std::size_t const listed = listed_by(arguments.offsets, arguments.count);
    uploader.move_output(arguments.keys, listed);
    if constexpr (a_call_answering_scores<arguments_type_>) uploader.move_output(arguments.scores, listed);
    uploader.move_output(arguments.offsets, static_cast<std::size_t>(arguments.count) + 1);
}

/** A pull's output columns: the keys or strings walked and values read, as many as it wrote. */
template <typename arguments_type_, typename uploader_type_>
    requires requires(arguments_type_ const& arguments) {
        arguments.cursor;
        *arguments.written;
    }
void output_columns(arguments_type_ const& arguments, uploader_type_& uploader) noexcept {
    std::size_t const written = static_cast<std::size_t>(*arguments.written);
    if constexpr (a_call_holding_keys<arguments_type_>) uploader.move_output(arguments.keys, written);
    if constexpr (a_call_holding_strings<arguments_type_>) uploader.move(*arguments.strings, written);
    if constexpr (a_call_holding_ragged_values<arguments_type_>) {
        if (arguments.values) uploader.move(*arguments.values, written);
    }
    else if constexpr (a_call_holding_values<arguments_type_>) uploader.move_output(arguments.values, written);
}

/** The output columns of a strings read: presence bits and one key per string. */
template <typename uploader_type_>
void output_columns(ustore_strings_read_t const& arguments, uploader_type_& uploader) noexcept {
    std::size_t const count = static_cast<std::size_t>(arguments.count);
    uploader.move_output(arguments.presences, util::divide_round_up(count, util::bits_in_byte_k));
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
    std::size_t const bitmap_bytes = util::divide_round_up(rows, util::bits_in_byte_k);
    std::span<ustore_docs_column_view_t> const views = uploader.copy_into_bounce(*arguments.columns, fields);
    for (std::size_t field = 0; field != views.size(); ++field) {
        ustore_docs_column_view_t& view = views[field];
        view.validities = uploader.move(view.validities, bitmap_bytes).data();
        view.conversions = uploader.move(view.conversions, bitmap_bytes).data();
        view.collisions = uploader.move(view.collisions, bitmap_bytes).data();
        view.scalars =
            uploader.move(view.scalars, rows * docs::field_size_bytes(arguments.outputs_types[field])).data();
        uploader.move(view.strings, rows);
    }
    *arguments.columns = uploader.move(views.data(), views.size()).data();
}

/** The output columns of a docs pull: every batch's keys and columns, then the views naming them,
 *  each view repointed in a copy before it moves. */
template <typename arguments_type_, typename uploader_type_>
    requires std::is_same_v<arguments_type_, ustore_docs_cursor_next_t>
void output_columns(arguments_type_ const& arguments, uploader_type_& uploader) noexcept {
    if (!*arguments.batches_count) return;
    docs_columns_t const& requested = arguments.cursor->columns;
    std::size_t const fields = requested.fields.size();
    std::span<ustore_docs_batch_view_t> const batches = uploader.copy_into_bounce(
        *arguments.batches, static_cast<std::size_t>(*arguments.batches_count));
    for (ustore_docs_batch_view_t& batch : batches) {
        std::size_t const rows = static_cast<std::size_t>(batch.count);
        std::size_t const bitmap_bytes = util::divide_round_up(rows, util::bits_in_byte_k);
        batch.keys = uploader.move(batch.keys, rows).data();
        std::span<ustore_docs_column_view_t> const columns = uploader.copy_into_bounce(batch.columns, fields);
        for (std::size_t field = 0; field != columns.size(); ++field) {
            ustore_docs_column_view_t& column = columns[field];
            column.validities = uploader.move(column.validities, bitmap_bytes).data();
            column.conversions = uploader.move(column.conversions, bitmap_bytes).data();
            column.collisions = uploader.move(column.collisions, bitmap_bytes).data();
            column.scalars =
                uploader.move(column.scalars, rows * docs::field_size_bytes(requested.types[field])).data();
            uploader.move(column.strings, rows);
        }
        batch.columns = uploader.move(columns.data(), columns.size()).data();
    }
    *arguments.batches = uploader.move(batches.data(), batches.size()).data();
}

/** Moves every output column of @p hosted, which a host verb wrote into @p executor's bounce, into @p blocks, the
 *  call's device arena, one copy per column, and waits once. */
template <typename arguments_type_, util::a_gpu_stream executor_type_>
util::status_t upload_output_columns(arguments_type_ const& hosted, util::device_blocks_t& blocks,
                                     executor_type_& executor) noexcept {
    if (util::status_t const pinned = executor.memory.bounce.pin(); pinned.failed()) return pinned;
    column_uploader<executor_type_> uploader {blocks, executor};
    output_columns(hosted, uploader);
    // Copies already queued read the bounce, which the next call rewinds, so a failure still waits for them
    util::status_t const waited = executor.wait();
    return uploader.status().failed() ? uploader.status() : waited;
}

} // namespace unum::ustore
