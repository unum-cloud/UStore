/**
 *  @file src/docs.cpp
 *  @author Ash Vardanian
 *  @date December 26, 2022
 *  @brief Implements <tt>c/ustore/docs.h</tt>: routes every document call to the docs verbs of the engine serving its
 *      database, and spells the document field types and modifications.
 */

#include "ustore/docs.h" // `ustore_docs_field_type_t`, `ustore_docs_modification_t`

#include "engine.hpp" // `engine_t`, `docs_verbs<home_t::host_k>`
#include "lib.hpp"    // `route`, `open_cursor`, `free_cursor`, `enumerator_spelling`, `name_in`, `value_in`

namespace unum::ustore {

#pragma region Names

/** The canonical spelling of every document field type. */
constexpr enumerator_spelling<ustore_docs_field_type_t> docs_field_type_names_k[] {
    {ustore_docs_field_type_json_k, "json"},     {ustore_docs_field_type_null_k, "null"},
    {ustore_docs_field_type_bool_k, "bool"},     {ustore_docs_field_type_uuid_k, "uuid"},
    {ustore_docs_field_type_f64_k, "f64"},       {ustore_docs_field_type_f32_k, "f32"},
    {ustore_docs_field_type_f16_k, "f16"},       {ustore_docs_field_type_i64_k, "i64"},
    {ustore_docs_field_type_i32_k, "i32"},       {ustore_docs_field_type_i16_k, "i16"},
    {ustore_docs_field_type_i8_k, "i8"},         {ustore_docs_field_type_u64_k, "u64"},
    {ustore_docs_field_type_u32_k, "u32"},       {ustore_docs_field_type_u16_k, "u16"},
    {ustore_docs_field_type_u8_k, "u8"},         {ustore_docs_field_type_binary_k, "binary"},
    {ustore_docs_field_type_string_k, "string"},
};

/** The canonical spelling of every document modification. */
constexpr enumerator_spelling<ustore_docs_modification_t> docs_modification_names_k[] {
    {ustore_docs_modify_upsert_k, "upsert"}, {ustore_docs_modify_update_k, "update"},
    {ustore_docs_modify_insert_k, "insert"}, {ustore_docs_modify_patch_k, "patch"},
    {ustore_docs_modify_merge_k, "merge"},
};

#pragma endregion Names

status_t pull(engine_t const& engine, ustore_docs_cursor_next_t const& arguments, ustore_docs_cursor_handle_t& cursor,
              view_t const& view) noexcept {
    if (!arguments.keys || !arguments.written)
        return {status_code_t::invalid_argument_k, "Need outputs for the keys and the count written"};
    if (arguments.values && !engine.host.docs->read)
        return {status_code_t::unsupported_k, "The engine does not implement this call"};
    keyed_survivors_t survivors {arena_allocator<ustore_key_t> {view.scratch}};
    std::variant<ustore_key_t, exhausted_t> resume;
    if (status_t const status = list_cursor_keys<ustore_docs_scan_t>(
            engine, arguments.database, cursor, arguments.wanted, arguments.arena, view, survivors, resume);
        status.failed())
        return status;
    arena_rewind(arguments.arena);
    std::size_t const written = survivors.size();
    if (written && arguments.values) {
        ustore_docs_read_t const read {.database = arguments.database,
                                       .count = written,
                                       .collections = {&cursor.cursor.collection, 0},
                                       .keys = {survivors.data(), sizeof(ustore_key_t)},
                                       .values = arguments.values,
                                       .arena = arguments.arena};
        if (status_t const status = engine.host.docs->read(read, strings_arg_t {}, view); status.failed())
            return status;
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

void ustore_docs_write(ustore_docs_write_t* arguments) { route(arguments); }
void ustore_docs_read(ustore_docs_read_t* arguments) { route(arguments); }
void ustore_docs_gist(ustore_docs_gist_t* arguments) { route(arguments); }
void ustore_docs_gather(ustore_docs_gather_t* arguments) { route(arguments); }
void ustore_docs_remove(ustore_docs_remove_t* arguments) { route(arguments); }
void ustore_docs_scan(ustore_docs_scan_t* arguments) { route(arguments); }

void ustore_docs_cursor_next(ustore_docs_cursor_next_t* arguments) { route(arguments); }

void ustore_docs_cursor_init(ustore_docs_cursor_init_t* arguments) {
    auto [cursor, status] = open_cursor<ustore_docs_cursor_handle_t, docs_verbs, ustore_docs_scan_t>(*arguments);
    if (status.failed()) return finish(*arguments->outcome, status);
    cursor->position = arguments->start_key;
    *arguments->cursor = cursor;
    finish(*arguments->outcome, {});
}

void ustore_docs_cursor_free(ustore_docs_cursor_t cursor) { free_cursor(cursor); }
void ustore_docs_sample(ustore_docs_sample_t* arguments) { route(arguments); }

char const* ustore_docs_field_type_name(ustore_docs_field_type_t field_type) {
    return name_in(docs_field_type_names_k, field_type);
}

ustore_docs_field_type_t ustore_docs_field_type_from_name(ustore_bytes_view_t name) {
    return value_in(docs_field_type_names_k, name);
}

char const* ustore_docs_modification_name(ustore_docs_modification_t modification) {
    return name_in(docs_modification_names_k, modification);
}

ustore_docs_modification_t ustore_docs_modification_from_name(ustore_bytes_view_t name) {
    return value_in(docs_modification_names_k, name);
}
