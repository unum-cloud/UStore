/**
 *  @file src/strings.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief Implements `c/ustore/strings.h`: routing the strings calls to their engine, and the cursors walking the
 *      strings of one prefix.
 */

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`

#include <string_view> // `std::string_view`

#include <stringzilla/stringzilla.hpp> // `sz::string_t`, `sz::string_view_t`

#include "ustore/strings.h" // `ustore_strings_write_t`, `ustore_strings_cursor_next_t`

#include "lib.hpp" // `route`, `open_cursor`, `free_cursor`, `strings_cursor_t`

namespace unum::ustore {

status_t pull(engine_t const& engine, ustore_strings_cursor_next_t const& arguments, strings_cursor_t& cursor,
              view_t const& view) noexcept {
    if (!arguments.strings || !arguments.written)
        return {status_code_t::invalid_argument_k, "Need outputs for the strings and the count written"};
    if (arguments.values && !engine.strings_read)
        return {status_code_t::unsupported_k, "The engine does not implement this call"};
    ustore_bytes_view_t const prefix = bytes_of(std::string_view {cursor.prefix});
    ustore_bytes_view_t const start = bytes_of(std::string_view {cursor.start});
    ustore_length_t const* offsets = nullptr;
    strings_arg_t listed;
    if (!cursor.exhausted && arguments.wanted) {
        ustore_strings_scan_t const scan {.database = arguments.database,
                                          .count = 1,
                                          .collections = {&cursor.collection, 0},
                                          .offsets = &offsets,
                                          .strings = &listed.view,
                                          .arena = view.scratch,
                                          .wanted = arguments.wanted};
        status_t const status = engine.strings_scan(scan, strings_arg_t::from_views(&prefix),
                                                    strings_arg_t::from_views(&start), view);
        if (status.failed()) return status;
    }
    std::size_t const written = offsets ? offsets[1] : 0;
    if (written && arguments.values) {
        ustore_strings_read_t const read {.database = arguments.database,
                                          .count = written,
                                          .collections = {&cursor.collection, 0},
                                          .values = arguments.values,
                                          .arena = arguments.arena};
        if (status_t const status = engine.strings_read(read, listed, view); status.failed()) return status;
    }
    std::size_t bytes = 0;
    for (std::size_t index = 0; index != written; ++index) bytes += listed[index].length;
    auto const strings = [&](std::size_t index) noexcept { return listed[index]; };
    if (status_t const exported = export_strings(arguments.arena, written, bytes, strings, *arguments.strings);
        exported.failed())
        return exported;
    ustore_bytes_view_t const last = written ? listed[written - 1] : ustore_bytes_view_t {};
    if (written && cursor.start.reserve(last.length + 1) != sz::status_t::success_k)
        return {status_code_t::out_of_memory_k, "Failed to keep the string the walk resumes past"};
    if (!progress_t {arguments.progress, arguments.progress_state, 1}.advance(1)) return progress_cancelled_k;

    // Reserved above, so the resize allocates nothing once the callback let the pull finish
    if (written < arguments.wanted) cursor.exhausted = true;
    else if (written && cursor.start.resize(last.length + 1) == sz::status_t::success_k) {
        if (last.length) std::memcpy(cursor.start.data(), last.data, last.length);
        cursor.start[last.length] = 0;
    }
    *arguments.written = written;
    return {};
}

/** Copies @p bytes into @p copy, answering whether they fit in memory. */
bool copy_bytes(ustore_bytes_view_t bytes, sz::string_t& copy) noexcept {
    return copy.assign(sz::string_view_t {string_of(bytes)}) == sz::status_t::success_k;
}

} // namespace unum::ustore

void ustore_strings_write(ustore_strings_write_t* arguments) { route<&engine_t::strings_write>(arguments); }
void ustore_strings_remove(ustore_strings_remove_t* arguments) { route<&engine_t::strings_remove>(arguments); }
void ustore_strings_read(ustore_strings_read_t* arguments) { route<&engine_t::strings_read>(arguments); }
void ustore_strings_scan(ustore_strings_scan_t* arguments) { route<&engine_t::strings_scan>(arguments); }
void ustore_strings_cursor_next(ustore_strings_cursor_next_t* arguments) { route<&engine_t::strings_scan>(arguments); }

void ustore_strings_cursor_init(ustore_strings_cursor_init_t* arguments) {
    auto [cursor, status] = open_cursor<ustore_strings_cursor_handle_t, &engine_t::strings_scan>(*arguments);
    if (status.failed()) return finish(*arguments->outcome, status);
    if (!copy_bytes(arguments->prefix, cursor->prefix) || !copy_bytes(arguments->start_string, cursor->start)) {
        free_cursor(cursor);
        return finish(*arguments->outcome,
                      {status_code_t::out_of_memory_k, "Failed to copy the prefix and the start of the walk"});
    }
    *arguments->cursor = cursor;
    finish(*arguments->outcome, {});
}

void ustore_strings_cursor_free(ustore_strings_cursor_t cursor) { free_cursor(cursor); }
