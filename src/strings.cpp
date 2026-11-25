/**
 *  @file src/strings.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief Implements <tt>c/ustore/strings.h</tt>: routing the strings calls to their engine, and
 *      the cursors walking the strings of one prefix.
 */

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`

#include <string_view> // `std::string_view`
#include <variant>     // `std::get`, `std::get_if`

#include <stringzilla/stringzilla.hpp> // `sz::string_t`, `sz::string_view_t`

#include "ustore/strings.h" // `ustore_strings_write_t`, `ustore_strings_cursor_next_t`

#include "lib.hpp" // `route`, `open_cursor`, `free_cursor`, `ustore_strings_cursor_handle_t`

namespace unum::ustore {

/** Pulls the next page of @p cursor as @c pull does, narrowing through @p match, the walk's own
 *  match or the face a device pull shows it. */
template <typename match_type_>
util::status_t pull_through(engine_t const& engine, ustore_strings_cursor_next_t const& arguments,
                            ustore_strings_cursor_handle_t& cursor, match_type_ const& match,
                            view_t const& view) noexcept {
    strings_verbs<vendored_residency_t::host_k> const& verbs = *engine.host.strings;
    if (!arguments.strings || !arguments.written)
        return {util::status_code_t::invalid_argument_k, "Needs outputs for the strings and count"};
    if (arguments.values && !verbs.read)
        return {util::status_code_t::unsupported_k, "The engine does not implement this call"};
    util::growing_strings<util::arena_allocator<byte_t>> survivors {util::arena_allocator<byte_t> {view.scratch}};
    st::vector<byte_t, util::arena_allocator<byte_t>> resume_bytes {util::arena_allocator<byte_t> {view.scratch}};
    std::variant<ustore_bytes_view_t, exhausted_t> resume = exhausted_t {};
    if (sz::string_t const* const start = std::get_if<sz::string_t>(&cursor.position); start && arguments.wanted) {
        ustore_bytes_view_t const prefix = util::bytes_of(std::string_view {cursor.prefix});
        if (util::status_t const status = list_matched_strings(
                engine.host, arguments.database, cursor.walk.collection, prefix,
                util::bytes_of(std::string_view {*start}), cursor.position_bound,
                util::bytes_of(std::string_view {cursor.end_string}), cursor.walk.direction, arguments.wanted, match,
                arguments.arena, view, survivors, resume_bytes, resume);
            status.failed())
            return status;
    }
    else if (std::holds_alternative<sz::string_t>(cursor.position)) resume = util::bytes_of(std::string_view {});

    match.rewind_page(arguments.arena);
    std::size_t const written = survivors.size();
    if (written && arguments.values) {
        ustore_strings_view_t const listed = survivors.tape();
        ustore_strings_read_t const read {.database = arguments.database,
                                          .count = written,
                                          .collections = {&cursor.walk.collection, 0},
                                          .values = arguments.values,
                                          .arena = arguments.arena};
        if (util::status_t const status = verbs.read(read, strings_arg_t {listed}, view); status.failed())
            return status;
    }
    if (util::status_t const exported = survivors.export_into(arguments.arena, *arguments.strings); exported.failed())
        return exported;
    sz::string_t* const kept = std::get_if<sz::string_t>(&cursor.position);
    ustore_bytes_view_t const* const next = std::get_if<ustore_bytes_view_t>(&resume);
    if (next && kept && arguments.wanted && kept->reserve(next->length) != sz::status_t::success_k)
        return {util::status_code_t::out_of_memory_k, "No room for the resume string"};
    if (!util::progress_t {arguments.progress, arguments.progress_state, 1}.advance(1))
        return util::progress_cancelled_k;

    // Reserved above, so the resize allocates nothing once the callback let the pull finish
    if (!next) cursor.position = exhausted_t {};
    else if (kept && arguments.wanted && kept->resize(next->length) == sz::status_t::success_k) {
        if (next->length) std::memcpy(kept->data(), next->data, next->length);
        if (written) cursor.position_bound = resume_bound_of(cursor.walk.direction);
    }
    *arguments.written = written;
    return {};
}

util::status_t pull(engine_t const& engine, ustore_strings_cursor_next_t const& arguments,
                    ustore_strings_cursor_handle_t& cursor, view_t const& view) noexcept {
    auto const* const walker = host_engine_of(cursor);
    if (!walker) return {util::status_code_t::device_k, "The cursor walks on another kind of executor"};
    return pull_through(engine, arguments, cursor, walker->match, view);
}

/** Copies @p bytes into @p copy, answering whether they fit in memory. */
bool copy_bytes(ustore_bytes_view_t bytes, sz::string_t& copy) noexcept {
    return copy.assign(sz::string_view_t {util::chars_of(bytes)}) == sz::status_t::success_k;
}

} // namespace unum::ustore

void ustore_strings_write(ustore_strings_write_t* arguments) { route(arguments); }
void ustore_strings_remove(ustore_strings_remove_t* arguments) { route(arguments); }
void ustore_strings_read(ustore_strings_read_t* arguments) { route(arguments); }
void ustore_strings_scan(ustore_strings_scan_t* arguments) { route(arguments); }
void ustore_strings_sample(ustore_strings_sample_t* arguments) { route(arguments); }
void ustore_strings_measure(ustore_strings_measure_t* arguments) { route(arguments); }
void ustore_strings_cursor_next(ustore_strings_cursor_next_t* arguments) { route(arguments); }

void ustore_strings_cursor_init(ustore_strings_cursor_init_t* arguments) {
    auto [cursor,
          status] = open_cursor<ustore_strings_cursor_handle_t, strings_verbs, ustore_strings_scan_t>(*arguments);
    if (status.failed()) return finish(*arguments->outcome, status);
    if (!copy_bytes(arguments->prefix, cursor->prefix) ||
        !copy_bytes(arguments->start_string, std::get<sz::string_t>(cursor->position)) ||
        !copy_bytes(arguments->end_string, cursor->end_string)) {
        free_cursor(cursor);
        return finish(*arguments->outcome, {util::status_code_t::out_of_memory_k, "No room for the prefix and start"});
    }
    *arguments->cursor = cursor;
    finish(*arguments->outcome, {});
}

void ustore_strings_cursor_free(ustore_strings_cursor_t cursor) { free_cursor(cursor); }
