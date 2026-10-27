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
#include <variant>     // `std::get`, `std::get_if`

#include <stringzilla/stringzilla.hpp> // `sz::string_t`, `sz::string_view_t`

#include "ustore/strings.h" // `ustore_strings_write_t`, `ustore_strings_cursor_next_t`

#include "lib.hpp" // `route`, `open_cursor`, `free_cursor`, `ustore_strings_cursor_handle_t`

namespace unum::ustore {

status_t pull(engine_t const& engine, ustore_strings_cursor_next_t const& arguments,
              ustore_strings_cursor_handle_t& cursor, view_t const& view) noexcept {
    auto const* const walker = host_engine_of(cursor);
    if (!walker) return {status_code_t::device_k, "The cursor walks on another kind of executor"};
    strings_verbs<home_t::host_k> const& verbs = *walker->verbs;
    if (!arguments.strings || !arguments.written)
        return {status_code_t::invalid_argument_k, "Need outputs for the strings and the count written"};
    if (arguments.values && !verbs.read)
        return {status_code_t::unsupported_k, "The engine does not implement this call"};
    growing_strings<arena_allocator<byte_t>> survivors {arena_allocator<byte_t> {view.scratch}};
    st::basic_vector<byte_t, arena_allocator<byte_t>> resume_bytes {arena_allocator<byte_t> {view.scratch}};
    std::variant<ustore_bytes_view_t, exhausted_t> resume = exhausted_t {};
    if (sz::string_t const* const start = std::get_if<sz::string_t>(&cursor.position); start && arguments.wanted) {
        ustore_bytes_view_t const prefix = bytes_of(std::string_view {cursor.prefix});
        if (status_t const status = list_matched_strings(
                engine.host, arguments.database, cursor.cursor.collection, prefix, bytes_of(std::string_view {*start}),
                arguments.wanted, walker->match, arguments.arena, view, survivors, resume_bytes, resume);
            status.failed())
            return status;
    }
    else if (std::holds_alternative<sz::string_t>(cursor.position)) resume = bytes_of(std::string_view {});

    arena_rewind(arguments.arena);
    std::size_t const written = survivors.size();
    if (written && arguments.values) {
        ustore_strings_view_t const listed = survivors.tape();
        ustore_strings_read_t const read {.database = arguments.database,
                                          .count = written,
                                          .collections = {&cursor.cursor.collection, 0},
                                          .values = arguments.values,
                                          .arena = arguments.arena};
        if (status_t const status = verbs.read(read, strings_arg_t {listed}, view); status.failed()) return status;
    }
    if (status_t const exported = survivors.export_into(arguments.arena, *arguments.strings); exported.failed())
        return exported;
    sz::string_t* const kept = std::get_if<sz::string_t>(&cursor.position);
    ustore_bytes_view_t const* const next = std::get_if<ustore_bytes_view_t>(&resume);
    if (next && kept && arguments.wanted && kept->reserve(next->length) != sz::status_t::success_k)
        return {status_code_t::out_of_memory_k, "Failed to keep the string the walk resumes at"};
    if (!progress_t {arguments.progress, arguments.progress_state, 1}.advance(1)) return progress_cancelled_k;

    // Reserved above, so the resize allocates nothing once the callback let the pull finish
    if (!next) cursor.position = exhausted_t {};
    else if (kept && arguments.wanted && kept->resize(next->length) == sz::status_t::success_k && next->length)
        std::memcpy(kept->data(), next->data, next->length);
    *arguments.written = written;
    return {};
}

/** Copies @p bytes into @p copy, answering whether they fit in memory. */
bool copy_bytes(ustore_bytes_view_t bytes, sz::string_t& copy) noexcept {
    return copy.assign(sz::string_view_t {string_of(bytes)}) == sz::status_t::success_k;
}

} // namespace unum::ustore

void ustore_strings_write(ustore_strings_write_t* arguments) { route(arguments); }
void ustore_strings_remove(ustore_strings_remove_t* arguments) { route(arguments); }
void ustore_strings_read(ustore_strings_read_t* arguments) { route(arguments); }
void ustore_strings_scan(ustore_strings_scan_t* arguments) { route(arguments); }
void ustore_strings_cursor_next(ustore_strings_cursor_next_t* arguments) { route(arguments); }

void ustore_strings_cursor_init(ustore_strings_cursor_init_t* arguments) {
    auto [cursor,
          status] = open_cursor<ustore_strings_cursor_handle_t, strings_verbs, ustore_strings_scan_t>(*arguments);
    if (status.failed()) return finish(*arguments->outcome, status);
    if (!copy_bytes(arguments->prefix, cursor->prefix) ||
        !copy_bytes(arguments->start_string, std::get<sz::string_t>(cursor->position))) {
        free_cursor(cursor);
        return finish(*arguments->outcome,
                      {status_code_t::out_of_memory_k, "Failed to copy the prefix and the start of the walk"});
    }
    *arguments->cursor = cursor;
    finish(*arguments->outcome, {});
}

void ustore_strings_cursor_free(ustore_strings_cursor_t cursor) { free_cursor(cursor); }
