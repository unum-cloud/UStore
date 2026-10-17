/**
 *  @file src/blobs.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief Implements `c/ustore/blobs.h`: routing the binary calls to their engine, and the cursors walking the keys
 *      of one collection.
 */

#include <cstddef> // `std::size_t`

#include "ustore/blobs.h" // `ustore_blobs_write_t`, `ustore_blobs_cursor_next_t`

#include "lib.hpp" // `route`, `open_cursor`, `free_cursor`, `blobs_cursor_t`

namespace unum::ustore {

status_t pull(engine_t const& engine, ustore_blobs_cursor_next_t const& arguments, blobs_cursor_t& cursor,
              view_t const& view) noexcept {
    if (!arguments.keys || !arguments.written)
        return {status_code_t::invalid_argument_k, "Need outputs for the keys and the count written"};
    if (arguments.values && !engine.blobs_read)
        return {status_code_t::unsupported_k, "The engine does not implement this call"};
    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* listed = nullptr;
    if (!cursor.exhausted && arguments.wanted) {
        ustore_blobs_scan_t const scan {.database = arguments.database,
                                        .count = 1,
                                        .collections = {&cursor.collection, 0},
                                        .start_keys = {&cursor.start_key, 0},
                                        .offsets = &offsets,
                                        .keys = &listed,
                                        .arena = view.scratch,
                                        .wanted = arguments.wanted};
        if (status_t const status = engine.blobs_scan(scan, view); status.failed()) return status;
    }
    std::size_t const written = offsets ? offsets[1] : 0;
    if (written && arguments.values) {
        ustore_blobs_read_t const read {.database = arguments.database,
                                        .count = written,
                                        .collections = {&cursor.collection, 0},
                                        .keys = {listed, sizeof(ustore_key_t)},
                                        .values = arguments.values,
                                        .arena = arguments.arena};
        if (status_t const status = engine.blobs_read(read, view); status.failed()) return status;
    }
    auto [keys, keys_status] = arena_copy<ustore_key_t>(arguments.arena, {listed, written});
    if (keys_status.failed()) return keys_status;
    if (!progress_t {arguments.progress, arguments.progress_state, 1}.advance(1)) return progress_cancelled_k;

    // A page short of what it wanted reached the end of the collection, and so did one ending at the last key
    ustore_key_t const* const last = written ? listed + written - 1 : nullptr;
    if (written < arguments.wanted || (last && *last == largest_key_k)) cursor.exhausted = true;
    else if (last) cursor.start_key = key_after(*last);
    *arguments.keys = keys.data();
    *arguments.written = written;
    return {};
}

} // namespace unum::ustore

void ustore_blobs_write(ustore_blobs_write_t* arguments) { route<&engine_t::blobs_write>(arguments); }
void ustore_blobs_remove(ustore_blobs_remove_t* arguments) { route<&engine_t::blobs_remove>(arguments); }
void ustore_blobs_read(ustore_blobs_read_t* arguments) { route<&engine_t::blobs_read>(arguments); }
void ustore_blobs_scan(ustore_blobs_scan_t* arguments) { route<&engine_t::blobs_scan>(arguments); }
void ustore_blobs_sample(ustore_blobs_sample_t* arguments) { route<&engine_t::blobs_sample>(arguments); }
void ustore_blobs_measure(ustore_blobs_measure_t* arguments) { route<&engine_t::blobs_measure>(arguments); }
void ustore_blobs_cursor_next(ustore_blobs_cursor_next_t* arguments) { route<&engine_t::blobs_scan>(arguments); }

void ustore_blobs_cursor_init(ustore_blobs_cursor_init_t* arguments) {
    auto [cursor, status] = open_cursor<ustore_blobs_cursor_handle_t, &engine_t::blobs_scan>(*arguments);
    if (status.failed()) return finish(*arguments->outcome, status);
    cursor->start_key = arguments->start_key;
    *arguments->cursor = cursor;
    finish(*arguments->outcome, {});
}

void ustore_blobs_cursor_free(ustore_blobs_cursor_t cursor) { free_cursor(cursor); }
