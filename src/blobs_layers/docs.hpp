/**
 *  @file src/blobs_layers/docs.hpp
 *  @author Ash Vardanian
 *  @date December 26, 2022
 *  @brief The document verbs of an engine keeping every document as one whole value of JSON text, as
 *      read-modify-writes through its own whole-value verbs.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`
#include <cstring> // `std::memcpy`, `std::memset`, `std::strlen`

#include <algorithm>   // `std::sort`, `std::unique`, `std::lower_bound`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_invocable_r_v`

#include <yyjson.h> // `yyjson_val`, `yyjson_mut_doc`

#include <smashtable/basic_flat_set.hpp> // `st::basic_flat_set`

#include "ustore/docs.h"     // `ustore_docs_write_t`, `ustore_docs_read_t`
#include "ustore/ranges.hpp" // `place_batch_t`, `values_view_t`

#include "ustore/docs/json_patch.hpp"       // `modify`, `modification_t`
#include "ustore/docs/json_paths.hpp"       // `resolve_fields`, `json_documents_t`, `field_cell_t`
#include "ustore/docs/json_values.hpp"      // `json_parse`, `gather_scalar`, `field_size_bytes`, `extract_keys`
#include "ustore/utilities/algorithm.hpp"   // `offset_in_sorted`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_grow`, `arena_allocator`, `growing_strings`

#include "engine.hpp"            // `blobs_verbs_t`, `view_t`, `docs_write_over_blobs`
#include "lib.hpp"               // `stringzilla_capabilities`
#include "utilities/batches.hpp" // `strings_arg_t`, `places_of`
#include "utilities/outputs.hpp" // `read_outputs_t`

namespace unum::ustore {
using namespace unum::ustore::utilities;
using namespace unum::ustore::docs;

#pragma region Gist Paths

/** Longest path @c ustore_docs_gist exports, including the null terminator. */
constexpr std::size_t field_path_length_limit_k = 512;

using field_path_buffer_t = char[field_path_length_limit_k];

#pragma endregion Gist Paths

#pragma region Reading Documents

/** Receives the index of a task, its field, and the binary document it addresses, and answers a status. */
template <typename callback_type_>
concept document_callback =
    std::is_invocable_r_v<status_t, callback_type_&, std::size_t, std::string_view, value_view_t>;

/** The field of task @p index of @p fields, empty for a whole document and for every task without fields. */
static std::string_view field_of(strings_arg_t const& fields, std::size_t index) noexcept {
    return fields ? string_of(fields[index]) : std::string_view {};
}

/** Whether any of @p count tasks addresses a field rather than a whole document, which only parsing serves. */
static bool addresses_fields(strings_arg_t const& fields, std::size_t count) noexcept {
    for (std::size_t index = 0; fields && index != count; ++index)
        if (fields[index].length) return true;
    return false;
}

/** Reads the binary documents at @p places through @p view with the engine's own @p values_verbs, which append to the
 *  scratch of @p view rather than rewinding it, answering the tape and presences as read. */
static expected<json_documents_t> read_document_tape(blobs_verbs_t const& values_verbs, ustore_database_t database,
                                                     place_batch_t const& places, view_t const& view) noexcept {
    json_documents_t documents;
    documents.count = places.count;
    ustore_blobs_read_t read {};
    read.database = database;
    read.count = places.count;
    read.collections = places.collections();
    read.keys = places.keys();
    read.presences = &documents.presences;
    read.values = &documents.tape;
    read.arena = view.scratch;
    if (status_t const status = values_verbs.read(read, view); status.failed()) return status;
    return documents;
}

/** Reads the binary documents at @p places through @p view, as @c read_document_tape does, viewing them as values. */
static expected<values_view_t> read_documents(blobs_verbs_t const& values_verbs, ustore_database_t database,
                                              place_batch_t const& places, view_t const& view) noexcept {
    auto [documents, status] = read_document_tape(values_verbs, database, places, view);
    if (status.failed()) return status;
    return values_view_t {places.count, documents.presences, documents.tape};
}

/** The value of @p document under @p field, parsed with yyjson into @p scratch, or @c nullptr where it is missing. */
static expected<yyjson_val*> parse_field(std::string_view document, std::string_view field,
                                         ustore_arena_t scratch) noexcept {
    auto [parsed, status] = json_parse(document, scratch);
    if (status.failed()) return status;
    return json_lookup(yyjson_doc_get_root(parsed.handle), field);
}

/**
 *  @brief Calls @p callback on the document of every task, in order.
 *
 *  Fails a field modification of a missing document, an update or a patch of a whole document that is missing,
 *  and an insertion of a whole document that already exists.
 */
template <document_callback callback_type_>
static status_t read_each_document(blobs_verbs_t const& values_verbs, ustore_database_t database,
                                   place_batch_t const& places, strings_arg_t const& fields,
                                   modification_t modification, view_t const& view,
                                   callback_type_&& callback) noexcept {

    auto [found, status] = read_documents(values_verbs, database, places, view);
    if (status.failed()) return status;

    bool const replaces_existing = modification == modification_t::update_k || modification == modification_t::patch_k;
    for (std::size_t index = 0; index != places.size(); ++index) {
        value_view_t const document = found[index];
        std::string_view const field = field_of(fields, index);
        if (!document && !field.empty() && modification != modification_t::read_k)
            return {status_code_t::not_found_k, "Modified field belongs to a missing document"};
        if (!document && field.empty() && replaces_existing)
            return {status_code_t::not_found_k, "Updated or patched document is missing"};
        if (document && field.empty() && modification == modification_t::insert_k)
            return {status_code_t::already_exists_k, "Inserted document already exists"};
        if (status_t const handled = callback(index, field, document); handled.failed()) return handled;
    }
    return {};
}

/** The documents a batch of tasks addresses, each distinct one read once, and the document of every task. */
struct task_documents_t {

    /** The documents as read. */
    json_documents_t documents;

    /** The document of every task, or empty when task @c i addresses document @c i. */
    std::span<std::size_t> documents_of_tasks;

    /** The index into @c documents of the document task @p task addresses. */
    std::size_t document_of(std::size_t task) const noexcept {
        return documents_of_tasks.empty() ? task : documents_of_tasks[task];
    }
};

/** Reads every distinct document the tasks at @p places address once. */
static expected<task_documents_t> read_distinct_documents(blobs_verbs_t const& values_verbs, ustore_database_t database,
                                                          place_batch_t const& places, view_t const& view) noexcept {

    task_documents_t read;
    if (all_ascending(places.keys_begin, places.count)) {
        auto [documents, status] = read_document_tape(values_verbs, database, places, view);
        if (status.failed()) return status;
        read.documents = documents;
        return read;
    }

    auto [distinct, distinct_status] = arena_allocate<collection_key_t>(view.scratch, places.count);
    if (distinct_status.failed()) return distinct_status;
    transform_n(places, places.count, distinct.begin(), [](place_t const& place) { return place.collection_key(); });
    std::size_t const distinct_count = sort_and_deduplicate(distinct.begin(), distinct.end());
    if (distinct_count == places.count) {
        auto [documents, status] = read_document_tape(values_verbs, database, places, view);
        if (status.failed()) return status;
        read.documents = documents;
        return read;
    }

    std::span<collection_key_t> const sorted = distinct.first(distinct_count);
    auto const sorted_strided = strided(sorted).immutable();
    place_batch_t distinct_places;
    distinct_places.collections_begin = sorted_strided.members(&collection_key_t::collection).begin();
    distinct_places.keys_begin = sorted_strided.members(&collection_key_t::key).begin();
    distinct_places.count = static_cast<ustore_size_t>(distinct_count);

    auto [documents, documents_status] = read_document_tape(values_verbs, database, distinct_places, view);
    if (documents_status.failed()) return documents_status;
    auto [documents_of_tasks, mapping_status] = arena_allocate<std::size_t>(view.scratch, places.count);
    if (mapping_status.failed()) return mapping_status;
    for (std::size_t index = 0; index != places.size(); ++index)
        documents_of_tasks[index] = offset_in_sorted(sorted, places[index].collection_key());
    read.documents = documents;
    read.documents_of_tasks = documents_of_tasks;
    return read;
}

/** The distinct fields of @p count tasks, sorted, and the index into them of every task's field. */
struct task_fields_t {
    std::span<std::string_view> distinct;
    std::span<std::size_t> fields_of_tasks;
};

/** Collects the distinct fields of @p count tasks of @p fields into @p scratch. */
static expected<task_fields_t> distinct_fields(strings_arg_t const& fields, std::size_t count,
                                               ustore_arena_t scratch) noexcept {
    auto [distinct, distinct_status] = arena_allocate<std::string_view>(scratch, count);
    if (distinct_status.failed()) return distinct_status;
    auto [fields_of_tasks, mapping_status] = arena_allocate<std::size_t>(scratch, count);
    if (mapping_status.failed()) return mapping_status;
    for (std::size_t index = 0; index != count; ++index) distinct[index] = field_of(fields, index);
    std::sort(distinct.begin(), distinct.end());
    std::size_t const distinct_count = static_cast<std::size_t>(std::unique(distinct.begin(), distinct.end()) -
                                                                distinct.begin());
    std::span<std::string_view> const sorted = distinct.first(distinct_count);
    for (std::size_t index = 0; index != count; ++index)
        fields_of_tasks[index] = static_cast<std::size_t>(
            std::lower_bound(sorted.begin(), sorted.end(), field_of(fields, index)) - sorted.begin());
    return task_fields_t {sorted, fields_of_tasks};
}

#pragma endregion Reading Documents

#pragma region Writing

/** Parses, modifies and writes back the documents at @p places through the engine's own @p values_verbs. */
static status_t read_modify_write(blobs_verbs_t const& values_verbs, ustore_database_t database,
                                  place_batch_t const& places, strings_arg_t const& fields, strings_arg_t const& values,
                                  modification_t modification, ustore_docs_field_type_t type,
                                  ustore_durability_t durability, view_t const& view) noexcept {

    ustore_arena_t const scratch = view.scratch;
    growing_strings<arena_allocator<byte_t>> modified {scratch};

    auto modify_one = [&](std::size_t index, std::string_view field, value_view_t binary_document) noexcept {
        auto [document, document_status] = json_parse(binary_document, scratch);
        if (document_status.failed()) return document_status;
        auto [modifier, modifier_status] = any_parse(value_view_t {values[index]}, type, scratch);
        if (modifier_status.failed()) return modifier_status;
        if (!modifier) return status_t {status_code_t::malformed_k, "An empty value is no JSON document"};
        auto [modifier_document, mutable_status] = json_mutable(modifier, scratch);
        if (mutable_status.failed()) return mutable_status;
        if (status_t const status = modify(document, yyjson_mut_doc_get_root(modifier_document), field, modification,
                                           scratch);
            status.failed())
            return status;
        auto [text, dump_status] = json_dump({nullptr, yyjson_mut_doc_get_root(document.mutable_handle)}, scratch);
        if (dump_status.failed()) return dump_status;
        return modified.push_back(text.bytes());
    };

    // A transaction watches the documents it rewrites, whatever the caller asked of the writes
    view_t watching = view;
    watching.watch = ustore_watch_keys_k;
    if (status_t const status = read_each_document(values_verbs, database, places, fields, modification, watching,
                                                   modify_one);
        status.failed())
        return status;

    strings_arg_t modified_values;
    modified_values.view = modified.tape();
    ustore_blobs_write_t write {};
    write.database = database;
    write.count = places.count;
    write.collections = places.collections();
    write.keys = places.keys();
    write.values = modified_values.view;
    write.durability = durability;
    return values_verbs.write(write, modified_values, view);
}

#pragma endregion Writing

#pragma region Gist

/** The paths a gist found so far, sorted in scratch, viewing the bytes of the column that exports them. */
using sorted_paths_t =
    st::basic_flat_set<std::string_view, st::less_t, st::native_row_kit_t, arena_allocator<std::string_view>>;

/** Collects the unique paths to the leaves under @p node, extending the null-terminated @p path. */
static status_t gist_paths(yyjson_val* node, field_path_buffer_t& path, sorted_paths_t& sorted_paths,
                           growing_strings<arena_allocator<byte_t>>& exported_paths) noexcept {

    std::size_t const path_length = std::strlen(path);

    if (yyjson_is_obj(node)) {
        yyjson_obj_iter iterator;
        yyjson_obj_iter_init(node, &iterator);
        while (yyjson_val* const key = yyjson_obj_iter_next(&iterator)) {
            std::string_view const name {yyjson_get_str(key), yyjson_get_len(key)};
            if (path_length + 1 + escaped_length(name) >= field_path_length_limit_k)
                return {status_code_t::capacity_exhausted_k, "Path is too long"};
            path[path_length] = '/';
            *escape_into(name, path + path_length + 1) = '\0';
            if (status_t const status = gist_paths(yyjson_obj_iter_get_val(key), path, sorted_paths, exported_paths);
                status.failed())
                return status;
        }
        path[path_length] = '\0';
        return {};
    }

    if (yyjson_is_arr(node)) {
        std::size_t element_index = 0;
        yyjson_arr_iter iterator;
        yyjson_arr_iter_init(node, &iterator);
        while (yyjson_val* const element = yyjson_arr_iter_next(&iterator)) {
            path[path_length] = '/';
            std::string_view const printed = print_number(path + path_length + 1, path + field_path_length_limit_k,
                                                          element_index++);
            if (printed.empty()) return {status_code_t::capacity_exhausted_k, "Path is too long"};
            if (status_t const status = gist_paths(element, path, sorted_paths, exported_paths); status.failed())
                return status;
        }
        path[path_length] = '\0';
        return {};
    }

    std::string_view const leaf {path, path_length};
    if (sorted_paths.find(leaf)) return {};
    if (status_t const status = exported_paths.push_back(bytes_of(leaf)); status.failed()) return status;

    // The column may move as it grows, leaving behind a copy that stays readable until the scratch rewinds
    return growth_status(sorted_paths.insert(string_of(exported_paths[exported_paths.size() - 1])));
}

#pragma endregion Gist

#pragma region Document Calls

status_t docs_write_over_blobs(blobs_verbs_t const& values_verbs, ustore_docs_write_t const& arguments,
                               strings_arg_t const& fields, strings_arg_t const& values, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!values) return {status_code_t::invalid_argument_k, "No values were provided"};
    std::string_view const id_field = string_of(arguments.id_field);
    if (!arguments.keys.data && id_field.empty())
        return {status_code_t::invalid_argument_k, "Documents without keys need an id field"};

    ustore_arena_t const scratch = view.scratch;
    place_batch_t places = places_of(arguments);
    if (!arguments.keys.data) {
        auto [extracted, status] = arena_allocate<ustore_key_t>(scratch, arguments.count);
        if (status.failed()) return status;
        if (status_t const extracting = extract_keys(values, id_field, scratch, extracted); extracting.failed())
            return extracting;
        places.keys_begin = {extracted.data(), sizeof(ustore_key_t)};
    }

    if (addresses_fields(fields, arguments.count) || arguments.type != ustore_docs_field_type_json_k ||
        arguments.modification != ustore_docs_modify_upsert_k)
        return read_modify_write(values_verbs, arguments.database, places, fields, values,
                                 static_cast<modification_t>(arguments.modification), arguments.type,
                                 arguments.durability, view);

    // Whole JSON documents pass through once they parse, which extracting the keys has already checked
    for (std::size_t index = 0; arguments.keys.data && index != arguments.count; ++index)
        if (status_t const status = json_parse(value_view_t {values[index]}, scratch).status(); status.failed())
            return status;

    ustore_blobs_write_t write {};
    write.database = arguments.database;
    write.count = arguments.count;
    write.collections = arguments.collections;
    write.keys = places.keys();
    write.values = arguments.values;
    write.durability = arguments.durability;
    return values_verbs.write(write, values, view);
}

status_t docs_read_over_blobs(blobs_verbs_t const& values_verbs, ustore_docs_read_t const& arguments,
                              strings_arg_t const& fields, view_t const& view) noexcept {
    if (!arguments.count) return {};

    if (!addresses_fields(fields, arguments.count) && arguments.outputs_type == ustore_docs_field_type_json_k) {
        ustore_blobs_read_t read {};
        read.database = arguments.database;
        read.count = arguments.count;
        read.collections = arguments.collections;
        read.keys = arguments.keys;
        read.presences = arguments.presences;
        read.lengths = arguments.lengths;
        read.values = arguments.values;
        read.arena = arguments.arena;
        return values_verbs.read(read, view);
    }
    if (arguments.outputs_type != ustore_docs_field_type_json_k &&
        arguments.outputs_type != ustore_docs_field_type_str_k)
        return {status_code_t::unsupported_k, "Documents read only as JSON or strings"};

    place_batch_t const places = places_of(arguments);
    auto [outputs, outputs_status] = read_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;

    // A value is present when its field is, and its text stays in scratch until the outputs copy it
    auto export_one = [&](yyjson_val* value) noexcept -> status_t {
        if (!value) {
            outputs.append_missing();
            return {};
        }
        if (arguments.outputs_type == ustore_docs_field_type_json_k || yyjson_is_ctn(value)) {
            auto [text, dump_status] = json_dump({value, nullptr}, view.scratch);
            if (dump_status.failed()) return dump_status;
            outputs.append(text);
            return {};
        }

        printed_number_buffer_t print_buffer;
        std::string_view text;
        [[maybe_unused]] conversion_t const conversion = json_to_string(value, print_buffer, text);
        if (text.data() == print_buffer) {
            auto [printed, printed_status] = arena_allocate<char>(view.scratch, text.size());
            if (printed_status.failed()) return printed_status;
            std::memcpy(printed.data(), text.data(), text.size());
            text = {printed.data(), text.size()};
        }
        outputs.append(text);
        return {};
    };

    auto [read, read_status] = read_distinct_documents(values_verbs, arguments.database, places, view);
    if (read_status.failed()) return read_status;
    auto [task_fields, fields_status] = distinct_fields(fields, arguments.count, view.scratch);
    if (fields_status.failed()) return fields_status;
    auto [values, values_status] = arena_allocate<yyjson_val*>(view.scratch, arguments.count);
    if (values_status.failed()) return values_status;

    // One subdocuments pass resolves every task, and yyjson parses each task's document only when it cannot
    auto const cell_of = [&](std::size_t task) noexcept {
        return field_cell_t {read.document_of(task), task_fields.fields_of_tasks[task]};
    };
    bool const resolved = resolve_fields(read.documents, task_fields.distinct, arguments.count, cell_of, true,
                                         view.scratch, stringzilla_capabilities(), values);
    for (std::size_t task = 0; task != arguments.count; ++task) {
        yyjson_val* value = values[task];
        if (!resolved) {
            auto [parsed, parsed_status] = parse_field(read.documents[read.document_of(task)], field_of(fields, task),
                                                       view.scratch);
            if (parsed_status.failed()) return parsed_status;
            value = parsed;
        }
        if (status_t const status = export_one(value); status.failed()) return status;
    }
    return outputs.finish(arguments);
}

status_t docs_gist_over_blobs(blobs_verbs_t const& values_verbs, ustore_docs_gist_t const& arguments,
                              view_t const& view) noexcept {
    if (arguments.written) *arguments.written = 0;
    if (!arguments.count) return {};

    auto [found, found_status] = read_documents(values_verbs, arguments.database, places_of(arguments), view);
    if (found_status.failed()) return found_status;

    // The paths and their offsets grow side by side in scratch, and only their final lengths reach the arena
    field_path_buffer_t path = {0};
    sorted_paths_t sorted_paths {{}, {view.scratch}};
    growing_strings<arena_allocator<byte_t>> exported_paths {view.scratch};
    for (std::size_t index = 0; index != arguments.count; ++index) {
        auto [document, document_status] = json_parse(found[index], view.scratch);
        if (document_status.failed()) return document_status;
        if (!document) continue;
        if (status_t const status = gist_paths(yyjson_doc_get_root(document.handle), path, sorted_paths,
                                               exported_paths);
            status.failed())
            return status;
    }

    if (arguments.fields)
        if (status_t const status = exported_paths.export_into(arguments.arena, *arguments.fields); status.failed())
            return status;
    if (arguments.written) *arguments.written = static_cast<ustore_size_t>(sorted_paths.size());
    return {};
}

status_t docs_gather_over_blobs(blobs_verbs_t const& values_verbs, ustore_docs_gather_t const& arguments,
                                strings_arg_t const& fields, view_t const& view) noexcept {
    if (!arguments.count || !arguments.fields_count) return {};

    if (!arguments.columns) return {status_code_t::invalid_argument_k, "Need an output for the columns"};
    if (!fields || !arguments.outputs_types)
        return {status_code_t::invalid_argument_k, "Need the fields to gather and the types of their columns"};

    std::size_t const fields_count = arguments.fields_count;
    std::size_t const docs_count = arguments.count;
    auto [documents, documents_status] = read_document_tape(values_verbs, arguments.database, places_of(arguments),
                                                            view);
    if (documents_status.failed()) return documents_status;

    // One subdocuments pass resolves every cell, field after field, where yyjson would parse every document once
    auto [field_names, names_status] = arena_allocate<std::string_view>(view.scratch, fields_count);
    if (names_status.failed()) return names_status;
    for (std::size_t field_index = 0; field_index != fields_count; ++field_index)
        field_names[field_index] = string_of(fields[field_index]);
    auto [values, values_status] = arena_allocate<yyjson_val*>(view.scratch, docs_count * fields_count);
    if (values_status.failed()) return values_status;
    auto const cell_of = [docs_count](std::size_t cell) noexcept {
        return field_cell_t {cell % docs_count, cell / docs_count};
    };
    bool const resolved = resolve_fields(documents, field_names, docs_count * fields_count, cell_of, false,
                                         view.scratch, stringzilla_capabilities(), values);

    // Without it, every document parses once, then the columns fill one after another
    std::span<yyjson_val*> roots;
    if (!resolved) {
        auto [parsed_roots, roots_status] = arena_allocate<yyjson_val*>(view.scratch, docs_count);
        if (roots_status.failed()) return roots_status;
        for (std::size_t document_index = 0; document_index != docs_count; ++document_index) {
            std::string_view const document = documents[document_index];
            auto [parsed, parsed_status] = json_parse(document, view.scratch);
            if (parsed_status.failed()) return parsed_status;
            parsed_roots[document_index] = yyjson_doc_get_root(parsed.handle);
        }
        roots = parsed_roots;
    }

    auto [columns, columns_status] = arena_allocate<ustore_docs_column_view_t>(arguments.arena, fields_count);
    if (columns_status.failed()) return columns_status;
    // The three bitmaps of a column share one allocation, each starting aligned as every output does
    std::size_t const bitmap_stride = round_up_to_multiple(divide_round_up(docs_count, bits_in_byte_k),
                                                           arena_alignment_k);
    printed_number_buffer_t print_buffer;
    for (std::size_t field_index = 0; field_index != fields_count; ++field_index) {
        ustore_docs_field_type_t const type = arguments.outputs_types[field_index];
        std::string_view const field = string_of(fields[field_index]);
        bool const holds_strings = type == ustore_docs_field_type_str_k || type == ustore_docs_field_type_bin_k;
        auto [bitmaps, bitmaps_status] = arena_allocate<ustore_octet_t>(arguments.arena, 3 * bitmap_stride);
        if (bitmaps_status.failed()) return bitmaps_status;
        std::memset(bitmaps.data(), 0, bitmaps.size());
        auto [scalars, scalars_status] = arena_allocate<ustore_byte_t>(arguments.arena,
                                                                       docs_count * field_size_bytes(type));
        if (scalars_status.failed()) return scalars_status;
        growing_strings<arena_allocator<byte_t>> strings {view.scratch};

        for (std::size_t document_index = 0; document_index != docs_count; ++document_index) {
            yyjson_val* const value = resolved ? values[field_index * docs_count + document_index]
                                               : json_lookup(roots[document_index], field);
            ustore_byte_t* const scalar_column = scalars.data();
            conversion_t conversion = conversion_t::collides_k;

            switch (type) {
            case ustore_docs_field_type_bool_k:
                conversion = gather_scalar<bool>(value, scalar_column, document_index);
                break;
            case ustore_docs_field_type_uuid_k: conversion = gather_uuid(value, scalar_column, document_index); break;
            case ustore_docs_field_type_f64_k:
                conversion = gather_scalar<double>(value, scalar_column, document_index);
                break;
            case ustore_docs_field_type_f32_k:
                conversion = gather_scalar<float>(value, scalar_column, document_index);
                break;
            case ustore_docs_field_type_f16_k: conversion = gather_half(value, scalar_column, document_index); break;
            case ustore_docs_field_type_i64_k:
                conversion = gather_scalar<std::int64_t>(value, scalar_column, document_index);
                break;
            case ustore_docs_field_type_i32_k:
                conversion = gather_scalar<std::int32_t>(value, scalar_column, document_index);
                break;
            case ustore_docs_field_type_i16_k:
                conversion = gather_scalar<std::int16_t>(value, scalar_column, document_index);
                break;
            case ustore_docs_field_type_i8_k:
                conversion = gather_scalar<std::int8_t>(value, scalar_column, document_index);
                break;
            case ustore_docs_field_type_u64_k:
                conversion = gather_scalar<std::uint64_t>(value, scalar_column, document_index);
                break;
            case ustore_docs_field_type_u32_k:
                conversion = gather_scalar<std::uint32_t>(value, scalar_column, document_index);
                break;
            case ustore_docs_field_type_u16_k:
                conversion = gather_scalar<std::uint16_t>(value, scalar_column, document_index);
                break;
            case ustore_docs_field_type_u8_k:
                conversion = gather_scalar<std::uint8_t>(value, scalar_column, document_index);
                break;
            case ustore_docs_field_type_str_k:
            case ustore_docs_field_type_bin_k: {
                std::string_view text;
                conversion = json_to_string(value, print_buffer, text);
                // A text column spans a null after every text, and a binary one does not
                if (status_t const status = strings.push_back(bytes_of(text), type == ustore_docs_field_type_str_k);
                    status.failed())
                    return status;
                break;
            }
            default: break;
            }

            std::size_t const slot = document_index / bits_in_byte_k;
            ustore_octet_t const mask = static_cast<ustore_octet_t>(1u << (document_index % bits_in_byte_k));
            record_conversion(conversion, mask, bitmaps[slot], bitmaps[bitmap_stride + slot],
                              bitmaps[2 * bitmap_stride + slot]);
        }

        ustore_strings_view_t column_strings {};
        if (holds_strings)
            if (status_t const status = strings.export_into(arguments.arena, column_strings); status.failed())
                return status;
        columns[field_index] = {
            .validities = bitmaps.data(),
            .conversions = bitmaps.data() + bitmap_stride,
            .collisions = bitmaps.data() + 2 * bitmap_stride,
            .scalars = scalars.data(),
            .strings = column_strings,
        };
    }

    *arguments.columns = columns.data();
    return {};
}

#pragma endregion Document Calls

} // namespace unum::ustore
