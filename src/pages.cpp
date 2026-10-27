/**
 *  @file src/pages.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief Implements `c/ustore/pages.h`: routes every page call to the pages verbs of the engine serving its database,
 *      and spells the page encodings.
 */

#include "ustore/pages.h" // `ustore_page_encoding_t`

#include "engine.hpp" // `engine_t`, `pages_verbs<home_t::host_k>`
#include "lib.hpp"    // `route`, `open_cursor`, `free_cursor`, `enumerator_spelling`, `name_in`, `value_in`

namespace unum::ustore {

#pragma region Names

/** The canonical spelling of every page encoding. */
constexpr enumerator_spelling<ustore_page_encoding_t> page_encoding_names_k[] {
    {ustore_page_encoding_html_k, "html"},         {ustore_page_encoding_xml_k, "xml"},
    {ustore_page_encoding_markdown_k, "markdown"}, {ustore_page_encoding_text_k, "text"},
    {ustore_page_encoding_json_k, "json"},
};

#pragma endregion Names

status_t pull(engine_t const& engine, ustore_pages_cursor_next_t const& arguments, ustore_pages_cursor_handle_t& cursor,
              view_t const& view) noexcept {
    if (!arguments.keys || !arguments.written)
        return {status_code_t::invalid_argument_k, "Need outputs for the keys and the count written"};
    if (arguments.values && !engine.host.pages->read)
        return {status_code_t::unsupported_k, "The engine does not implement this call"};
    keyed_survivors_t survivors {arena_allocator<ustore_key_t> {view.scratch}};
    std::variant<ustore_key_t, exhausted_t> resume;
    if (status_t const status = list_cursor_keys<ustore_pages_scan_t>(
            engine, arguments.database, cursor, arguments.wanted, arguments.arena, view, survivors, resume);
        status.failed())
        return status;
    arena_rewind(arguments.arena);
    std::size_t const written = survivors.size();
    if (written && arguments.values) {
        ustore_pages_read_t const read {.database = arguments.database,
                                        .count = written,
                                        .collections = {&cursor.cursor.collection, 0},
                                        .keys = {survivors.data(), sizeof(ustore_key_t)},
                                        .values = arguments.values,
                                        .arena = arguments.arena,
                                        .outputs_encoding = arguments.outputs_encoding};
        if (status_t const status = engine.host.pages->read(read, view); status.failed()) return status;
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

void ustore_pages_import(ustore_pages_import_t* arguments) { route(arguments); }
void ustore_pages_insert(ustore_pages_insert_t* arguments) { route(arguments); }
void ustore_pages_remove(ustore_pages_remove_t* arguments) { route(arguments); }
void ustore_pages_move(ustore_pages_move_t* arguments) { route(arguments); }
void ustore_pages_splice(ustore_pages_splice_t* arguments) { route(arguments); }
void ustore_pages_set_attributes(ustore_pages_set_attributes_t* arguments) { route(arguments); }
void ustore_pages_remove_attributes(ustore_pages_remove_attributes_t* arguments) { route(arguments); }
void ustore_pages_read(ustore_pages_read_t* arguments) { route(arguments); }
void ustore_pages_scan(ustore_pages_scan_t* arguments) { route(arguments); }

void ustore_pages_cursor_next(ustore_pages_cursor_next_t* arguments) { route(arguments); }

void ustore_pages_cursor_init(ustore_pages_cursor_init_t* arguments) {
    auto [cursor, status] = open_cursor<ustore_pages_cursor_handle_t, pages_verbs, ustore_pages_scan_t>(*arguments);
    if (status.failed()) return finish(*arguments->outcome, status);
    cursor->position = arguments->start_key;
    *arguments->cursor = cursor;
    finish(*arguments->outcome, {});
}

void ustore_pages_cursor_free(ustore_pages_cursor_t cursor) { free_cursor(cursor); }

char const* ustore_page_encoding_name(ustore_page_encoding_t encoding) {
    return name_in(page_encoding_names_k, encoding);
}

ustore_page_encoding_t ustore_page_encoding_from_name(ustore_bytes_view_t name) {
    return value_in(page_encoding_names_k, name);
}
