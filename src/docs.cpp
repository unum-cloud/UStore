/**
 *  @file src/docs.cpp
 *  @author Ash Vardanian
 *  @date December 26, 2022
 *  @brief Implements <tt>c/ustore/docs.h</tt>: routes every document call to the docs verbs of the engine serving its
 *      database, and spells the document field types and modifications.
 */

#include "ustore/docs.h" // `ustore_docs_field_type_t`, `ustore_docs_modification_t`

#include "docs_layer.hpp" // `docs_batches_t`, `json_documents_t`
#include "engine.hpp"     // `engine_t`, `docs_verbs<vendored_residency_t::host_k>`
#include "lib.hpp"        // `route`, `open_cursor`, `free_cursor`, `enumerator_spelling`, `name_in`, `value_in`

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

/** Copies the fields and column types @p arguments name into the arena @p cursor compiled its
 *  match in, refusing missing fields and unknown types, so every pull gathers them as they
 *  were at init. */
status_t copy_columns(ustore_docs_cursor_init_t const& arguments, ustore_docs_cursor_handle_t& cursor) noexcept {
    std::size_t const count = arguments.fields_count;
    if (!count) return {};
    ustore_arena_t const compiled = &cursor.cursor.compiled;
    strings_arg_t fields;
    if (status_t const gathered = gather_strings({arguments.fields, count}, compiled, fields); gathered.failed())
        return gathered;
    if (!fields || !arguments.columns_types)
        return {status_code_t::invalid_argument_k, "Need the fields to gather and the types of their columns"};
    auto [types, types_status] = arena_allocate<ustore_docs_field_type_t>(compiled, count);
    if (types_status.failed()) return types_status;
    auto [names, names_status] = arena_allocate<std::string_view>(compiled, count);
    if (names_status.failed()) return names_status;
    for (std::size_t field = 0; field != count; ++field) {
        types[field] = arguments.columns_types[field];
        bool known = false;
        for (enumerator_spelling<ustore_docs_field_type_t> const& spelling : docs_field_type_names_k)
            known = known || spelling.value == types[field];
        if (!known) return {status_code_t::invalid_argument_k, "Unknown type of a column"};
        std::string_view const name = string_of(fields[field]);
        auto [copied, copied_status] = arena_copy<char>(compiled, std::span<char const> {name});
        if (copied_status.failed()) return copied_status;
        names[field] = std::string_view {copied.begin(), copied.end()};
    }
    cursor.columns = {names, types};
    return {};
}

/** Fills @p batches with one batch of @p survivors, the keys a device pull's match kept, reading
 *  their whole documents through the host verbs of @p engine when a column gathers. */
status_t fill_survivors(engine_t const& engine, ustore_docs_cursor_next_t const& arguments,
                        ustore_docs_cursor_handle_t const& cursor, keyed_survivors_t const& survivors,
                        docs_batches_t& batches, view_t const& view) noexcept {
    auto read_documents = [&](std::span<std::uint32_t const> picked, json_documents_t& documents) noexcept {
        if (!engine.host.docs->read)
            return status_t {status_code_t::unsupported_k, "The engine does not read documents"};
        auto [keys, keys_status] = arena_allocate<ustore_key_t>(view.scratch, picked.size());
        if (keys_status.failed()) return keys_status;
        for (std::size_t index = 0; index != picked.size(); ++index) keys[index] = survivors[picked[index]];
        documents = {};
        documents.count = picked.size();
        ustore_docs_read_t const read {.database = arguments.database,
                                       .count = picked.size(),
                                       .collections = {&cursor.cursor.collection, 0},
                                       .keys = {keys.data(), sizeof(ustore_key_t)},
                                       .presences = &documents.presences,
                                       .values = &documents.tape,
                                       .arena = view.scratch};
        return engine.host.docs->read(read, strings_arg_t {}, view);
    };
    std::span<ustore_key_t const> const kept {survivors.begin(), survivors.end()};
    return batches.fill_one<strings_kit_of<cpu_ladder_floor_k>>(kept, read_documents, view.scratch);
}

/**
 *  @brief Pulls the next keys of @p cursor as @c pull does, narrowing through @p match, the walk's
 *      own match or the face a device pull shows it.
 *
 *  A host pull hands the engine's docs scan the batches to fill, which an NVMe engine splits over
 *  its executor's threads and every other engine fills on the calling thread; a device pull lists
 *  its survivors first and fills one batch with them. A pull whose range kept no row moves on to
 *  the next range, so only the walk's end hands out none.
 */
template <typename match_type_>
status_t pull_through(engine_t const& engine, ustore_docs_cursor_next_t const& arguments,
                      ustore_docs_cursor_handle_t& cursor, match_type_ const& match, view_t const& view) noexcept {
    if (!arguments.batches || !arguments.batches_count)
        return {status_code_t::invalid_argument_k, "Need outputs for the batches and their count"};
    constexpr bool host_k = std::is_same_v<match_type_, compiled_match<vendored_residency_t::host_k>>;
    docs_batches_t batches {.columns = cursor.columns,
                            .arena = arguments.arena,
                            .strings = arguments.database->ceiling.strings,
                            .resume = cursor.position};
    if constexpr (host_k) batches.match = match.matches_everything() ? nullptr : &match;
    ustore_key_t const* const end = cursor.end_key ? std::addressof(*cursor.end_key) : nullptr;
    for (ustore_key_t const* start = std::get_if<ustore_key_t>(&batches.resume); start && arguments.wanted;
         start = std::get_if<ustore_key_t>(&batches.resume)) {
        match.rewind_page(arguments.arena);
        ustore_key_t const from = *start;
        if constexpr (host_k) {
            ustore_length_t const* offsets = nullptr;
            ustore_key_t const* keys = nullptr;
            ustore_docs_scan_t const scan {.database = arguments.database,
                                           .count = 1,
                                           .collections = {&cursor.cursor.collection, 0},
                                           .start_keys = {&from, 0},
                                           .end_keys = {end, 0},
                                           .direction = cursor.cursor.direction,
                                           .offsets = &offsets,
                                           .keys = &keys,
                                           .arena = arguments.arena,
                                           .wanted = arguments.wanted};
            if (status_t const status = engine.host.docs->scan(scan, &batches, view); status.failed()) return status;
        }
        else {
            keyed_survivors_t survivors {arena_allocator<ustore_key_t> {view.scratch}};
            std::variant<ustore_key_t, exhausted_t> resume;
            if (status_t const status = list_matched_keys<ustore_docs_scan_t>(
                    engine.host, arguments.database, cursor.cursor.collection, from, end, cursor.cursor.direction,
                    arguments.wanted, match, arguments.arena, view, survivors, resume);
                status.failed())
                return status;
            match.rewind_page(arguments.arena);
            if (status_t const status = fill_survivors(engine, arguments, cursor, survivors, batches, view);
                status.failed())
                return status;
            batches.resume = resume;
        }
        if (batches.rows()) break;
    }
    if (!progress_t {arguments.progress, arguments.progress_state, 1}.advance(1)) return progress_cancelled_k;
    cursor.position = batches.resume;
    batches.finish(arguments.batches, arguments.batches_count);
    return {};
}

status_t pull(engine_t const& engine, ustore_docs_cursor_next_t const& arguments, ustore_docs_cursor_handle_t& cursor,
              view_t const& view) noexcept {
    auto const* const walker = host_engine_of(cursor);
    if (!walker) return {status_code_t::device_k, "The cursor walks on another kind of executor"};
    return pull_through(engine, arguments, cursor, walker->match, view);
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
    if (status_t const copied = copy_columns(*arguments, *cursor); copied.failed()) {
        free_cursor(cursor);
        *arguments->cursor = nullptr;
        return finish(*arguments->outcome, copied);
    }
    cursor->position = arguments->start_key;
    *arguments->cursor = cursor;
    finish(*arguments->outcome, {});
}

void ustore_docs_cursor_free(ustore_docs_cursor_t cursor) { free_cursor(cursor); }
void ustore_docs_sample(ustore_docs_sample_t* arguments) { route(arguments); }
void ustore_docs_measure(ustore_docs_measure_t* arguments) { route(arguments); }

char const* ustore_docs_field_type_name(ustore_docs_field_type_t field_type) {
    return name_in(docs_field_type_names_k, field_type);
}

ustore_status_t ustore_docs_field_type_from_name(ustore_bytes_view_t name, ustore_docs_field_type_t* field_type) {
    return value_in(docs_field_type_names_k, name, field_type);
}

char const* ustore_docs_modification_name(ustore_docs_modification_t modification) {
    return name_in(docs_modification_names_k, modification);
}

ustore_status_t ustore_docs_modification_from_name(ustore_bytes_view_t name, ustore_docs_modification_t* modification) {
    return value_in(docs_modification_names_k, name, modification);
}
