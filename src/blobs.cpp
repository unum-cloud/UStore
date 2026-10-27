/**
 *  @file src/blobs.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief Implements <tt>c/ustore/blobs.h</tt>: routing the binary calls to their engine, and the
 *      cursors walking the keys of one collection.
 */

#include <cstddef> // `std::size_t`

#include <variant> // `std::get_if`

#include "ustore/blobs.h" // `ustore_blobs_write_t`, `ustore_blobs_cursor_next_t`

#include "lib.hpp" // `route`, `open_cursor`, `free_cursor`, `ustore_blobs_cursor_handle_t`

namespace unum::ustore {

status_t pull(engine_t const& engine, ustore_blobs_cursor_next_t const& arguments, ustore_blobs_cursor_handle_t& cursor,
              view_t const& view) noexcept {
    if (!arguments.keys || !arguments.written)
        return {status_code_t::invalid_argument_k, "Need outputs for the keys and the count written"};
    if (arguments.values && !engine.host.blobs->read)
        return {status_code_t::unsupported_k, "The engine does not implement this call"};
    keyed_survivors_t survivors {arena_allocator<ustore_key_t> {view.scratch}};
    std::variant<ustore_key_t, exhausted_t> resume;
    if (status_t const status = list_cursor_keys<ustore_blobs_scan_t>(
            engine, arguments.database, cursor, arguments.wanted, arguments.arena, view, survivors, resume);
        status.failed())
        return status;
    arena_rewind(arguments.arena);
    std::size_t const written = survivors.size();
    if (written && arguments.values) {
        ustore_blobs_read_t const read {.database = arguments.database,
                                        .count = written,
                                        .collections = {&cursor.cursor.collection, 0},
                                        .keys = {survivors.data(), sizeof(ustore_key_t)},
                                        .values = arguments.values,
                                        .arena = arguments.arena};
        if (status_t const status = engine.host.blobs->read(read, view); status.failed()) return status;
    }
    auto [keys, keys_status] = arena_copy<ustore_key_t>(arguments.arena, survivors);
    if (keys_status.failed()) return keys_status;
    if (!progress_t {arguments.progress, arguments.progress_state, 1}.advance(1)) return progress_cancelled_k;
    cursor.position = resume;
    *arguments.keys = keys.data();
    *arguments.written = written;
    return {};
}

} // namespace unum::ustore

void ustore_blobs_write(ustore_blobs_write_t* arguments) { route(arguments); }
void ustore_blobs_remove(ustore_blobs_remove_t* arguments) { route(arguments); }
void ustore_blobs_read(ustore_blobs_read_t* arguments) { route(arguments); }
void ustore_blobs_scan(ustore_blobs_scan_t* arguments) { route(arguments); }
void ustore_blobs_sample(ustore_blobs_sample_t* arguments) { route(arguments); }
void ustore_blobs_measure(ustore_blobs_measure_t* arguments) { route(arguments); }
void ustore_blobs_cursor_next(ustore_blobs_cursor_next_t* arguments) { route(arguments); }

void ustore_blobs_cursor_init(ustore_blobs_cursor_init_t* arguments) {
    auto [cursor, status] = open_cursor<ustore_blobs_cursor_handle_t, blobs_verbs, ustore_blobs_scan_t>(*arguments);
    if (status.failed()) return finish(*arguments->outcome, status);
    cursor->position = arguments->start_key;
    *arguments->cursor = cursor;
    finish(*arguments->outcome, {});
}

void ustore_blobs_cursor_free(ustore_blobs_cursor_t cursor) { free_cursor(cursor); }
