/**
 *  @file src/docs_layer.hpp
 *  @author Ash Vardanian
 *  @date December 26, 2022
 *  @brief The document verbs of an engine keeping every document as one whole value of JSON text, as
 *      read-modify-writes through the reads and writes of its documents.
 *
 *  Documents are stored as JSON text. Whole-document reads and upserts pass straight through to the engine's own
 *  reads and writes. Field reads and gathers extract their fields with one StringZilla subdocuments pass over the
 *  documents read, with no other parser. Gists and modifications parse every document with yyjson. All of it lives in
 *  the scratch of the call, so the arena of the request holds only its outputs.
 *
 *  Every call takes the documents of one engine as an object answering <tt>read(places, view)</tt>, the
 *  binary documents at @c places as a @c json_documents_t appended to the scratch, and
 *  <tt>write(places, values, durability, view)</tt>. The @c *_over_blobs verbs bind them to the unchecked
 *  whole-value verbs of one engine, and the in-memory engine binds them to its documents stores
 *  inside one transaction.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`
#include <cstring> // `std::memcpy`, `std::memset`, `std::strlen`

#include <algorithm>   // `std::unique`, `std::lower_bound`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::type_identity`, `std::is_same_v`

#include <yyjson.h> // `yyjson_val`

#include <smashtable/basic_flat_set.hpp> // `st::basic_flat_set`

#include "ustore/docs.h"     // `ustore_docs_write_t`, `ustore_docs_read_t`
#include "ustore/ranges.hpp" // `place_batch_t`, `values_view_t`

#include "ustore/docs/json_fold.hpp"        // `json_delta_t`, `apply_delta`, `modifier_text`, `document_state_t`
#include "ustore/docs/json_patch.hpp"       // `modify`, `modification_t`
#include "ustore/docs/json_paths.hpp"       // `extract_fields`, `json_documents_t`, `field_cell_t`, `json_text_of`
#include "ustore/docs/json_values.hpp"      // `json_parse`, `gather_scalar`, `field_size_bytes`, `extract_keys`
#include "ustore/utilities/algorithm.hpp"   // `offset_in_sorted`, `heap_sort`, `radix_sort`, `word_byte`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_allocator`, `growing_strings`

#include "engine.hpp"            // `blobs_verbs<home_t::host_k>`, `view_t`
#include "utilities/batches.hpp" // `strings_arg_t`, `places_of`
#include "utilities/outputs.hpp" // `read_outputs_t`
#include "utilities/slices.hpp"  // `for_task_slices`, `slice_slots`

namespace unum::ustore {
using namespace unum::ustore::utilities;
using namespace unum::ustore::docs;

#pragma region Gist Paths

/** Longest path @c ustore_docs_gist exports, including the null terminator. */
inline constexpr std::size_t field_path_length_limit_k = 512;

using field_path_buffer_t = char[field_path_length_limit_k];

#pragma endregion Gist Paths

#pragma region Reading Documents

/** The field of task @p index of @p fields, empty for a whole document and for every task without fields. */
inline std::string_view field_of(strings_arg_t const& fields, std::size_t index) noexcept {
    return fields ? string_of(fields[index]) : std::string_view {};
}

/** Whether any of @p count tasks addresses a field rather than a whole document, which only parsing serves. */
inline bool addresses_fields(strings_arg_t const& fields, std::size_t count) noexcept {
    for (std::size_t index = 0; fields && index != count; ++index)
        if (fields[index].length) return true;
    return false;
}

/** Reads the binary documents at @p places through @p view from @p documents, viewing them as values. */
template <typename documents_type_>
expected<values_view_t> read_documents(documents_type_ const& documents, place_batch_t const& places,
                                       view_t const& view) noexcept {
    auto [read, status] = documents.read(places, view);
    if (status.failed()) return status;
    return values_view_t {places.count, read.presences, read.tape};
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

/** Reads every distinct document the tasks at @p places address once from @p source. */
template <typename documents_type_>
expected<task_documents_t> read_distinct_documents(documents_type_ const& source, place_batch_t const& places,
                                                   view_t const& view) noexcept {

    task_documents_t read;
    if (all_ascending(places.keys_begin, places.count)) {
        auto [documents, status] = source.read(places, view);
        if (status.failed()) return status;
        read.documents = documents;
        return read;
    }

    auto [distinct, distinct_status] = arena_allocate<collection_key_t>(view.scratch, places.count);
    if (distinct_status.failed()) return distinct_status;
    transform_n(places, places.count, distinct.begin(), [](place_t const& place) { return place.collection_key(); });
    std::size_t const distinct_count = sort_and_deduplicate(distinct.begin(), distinct.end());
    if (distinct_count == places.count) {
        auto [documents, status] = source.read(places, view);
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

    auto [documents, documents_status] = source.read(distinct_places, view);
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
inline expected<task_fields_t> distinct_fields(strings_arg_t const& fields, std::size_t count,
                                               ustore_arena_t scratch) noexcept {
    auto [distinct, distinct_status] = arena_allocate<std::string_view>(scratch, count);
    if (distinct_status.failed()) return distinct_status;
    auto [fields_of_tasks, mapping_status] = arena_allocate<std::size_t>(scratch, count);
    if (mapping_status.failed()) return mapping_status;
    for (std::size_t index = 0; index != count; ++index) distinct[index] = field_of(fields, index);
    utilities::heap_sort(distinct,
                         [](std::string_view first, std::string_view second) noexcept { return first < second; });
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

/** One task of a docs write: the document it addresses, its delta, and its place in the call,
 *  which orders the deltas of one document. */
struct document_task_t {
    ustore_collection_id_t collection;
    ustore_key_t key;
    json_delta_t delta;
    std::size_t sequence = 0;
};

/** The deltas one docs write makes to one document, in task order. */
struct document_deltas_t {
    ustore_collection_id_t collection;
    ustore_key_t key;
    std::span<json_delta_t> deltas;

    /** Whether the deltas depend on the document before them, which only a leading whole write
     *  makes irrelevant. */
    [[nodiscard]] bool reads_base() const noexcept {
        return deltas.front().modification != delta_modification_t::write_k;
    }

    /** Whether the document the deltas leave is known whole, past a whole write among them. */
    [[nodiscard]] bool writes_whole() const noexcept {
        for (json_delta_t const& delta : deltas)
            if (delta.modification == delta_modification_t::write_k) return true;
        return false;
    }
};

/**
 *  @brief Turns every task at @p places into its delta and groups the deltas per document, each
 *      document's in task order, so a replay of one group applies every task of the call on that
 *      document, each seeing the ones before.
 *
 *  A whole upsert becomes a write of the document, which a whole JSON upsert passes through once
 *  it parses as JSON, as extracting the keys of @p arguments has checked when it names none.
 */
inline expected<std::span<document_deltas_t>> group_document_deltas(ustore_docs_write_t const& arguments,
                                                                    place_batch_t const& places,
                                                                    strings_arg_t const& fields,
                                                                    strings_arg_t const& values,
                                                                    ustore_arena_t scratch) noexcept {
    ustore_docs_modification_t const modification = arguments.modification;
    ustore_docs_field_type_t const type = arguments.type;
    std::size_t const count = places.size();
    auto [tasks, tasks_status] = arena_allocate<document_task_t>(scratch, count);
    if (tasks_status.failed()) return tasks_status;
    for (std::size_t index = 0; index != count; ++index) {
        std::string_view const field = field_of(fields, index);
        value_view_t const value {values[index]};
        json_delta_t delta {0, static_cast<delta_modification_t>(modification), field, {}};
        if (field.empty() && delta.modification == delta_modification_t::upsert_k) {
            delta.modification = delta_modification_t::write_k;
            // Keys extracted from the documents already parsed every one of them
            if (type == ustore_docs_field_type_json_k && arguments.keys.data)
                if (status_t const parsed = json_parse(value, scratch).status(); parsed.failed()) return parsed;
        }
        auto [text, text_status] = modifier_text(value, type, scratch);
        if (text_status.failed()) return text_status;
        delta.value = text;
        tasks[index] = document_task_t {places[index].collection, places[index].key, delta, index};
    }
    // The collection, the key and the batch position read as one big-endian number, a total order.
    constexpr std::size_t uuid_bytes_k = sizeof(ustore_uuid_t);
    auto const digit_of = [](document_task_t const& task, std::size_t digit) noexcept -> std::size_t {
        if (digit < uuid_bytes_k) return task.collection.uuid.bytes[digit];
        if (digit < 2 * uuid_bytes_k) return task.key.uuid.bytes[digit - uuid_bytes_k];
        return utilities::word_byte(task.sequence, digit - 2 * uuid_bytes_k);
    };
    auto const precedes = [](document_task_t const& first, document_task_t const& second) noexcept {
        if (first.collection != second.collection) return first.collection < second.collection;
        if (first.key != second.key) return first.key < second.key;
        return first.sequence < second.sequence;
    };
    utilities::radix_sort(tasks, 2 * uuid_bytes_k + sizeof(std::uint64_t), digit_of, precedes);

    // The deltas of every document sit side by side in task order, which each document's span views
    auto [deltas, deltas_status] = arena_allocate<json_delta_t>(scratch, count);
    if (deltas_status.failed()) return deltas_status;
    auto [documents, documents_status] = arena_allocate<document_deltas_t>(scratch, count);
    if (documents_status.failed()) return documents_status;
    std::size_t documents_count = 0;
    for (std::size_t first = 0; first != count;) {
        std::size_t end = first + 1;
        while (end != count && tasks[end].collection == tasks[first].collection && tasks[end].key == tasks[first].key)
            ++end;
        for (std::size_t index = first; index != end; ++index) deltas[index] = tasks[index].delta;
        documents[documents_count++] = document_deltas_t {tasks[first].collection, tasks[first].key,
                                                          deltas.subspan(first, end - first)};
        first = end;
    }
    return documents.first(documents_count);
}

/** Replays @p deltas over @p document in order, as @c apply_delta does one by one, but parsing the
 *  document once before the first delta that modifies it and printing it once after the last,
 *  failing as the first failing delta does. */
inline status_t replay_deltas(document_state_t& document, std::span<json_delta_t const> deltas,
                              ustore_arena_t arena) noexcept {
    // The tree holds the document from the first modification on, until a whole write replaces it
    json_t tree;
    for (json_delta_t const& delta : deltas) {
        if (delta.modification == delta_modification_t::write_k ||
            delta.modification == delta_modification_t::remove_k) {
            tree = {};
            if (status_t const applied = apply_delta(document, delta, arena); applied.failed()) return applied;
            continue;
        }
        auto const modification = static_cast<modification_t>(delta.modification);
        bool const whole = delta.field.empty();
        if (!document.exists() && !whole)
            return {status_code_t::not_found_k, "Modified field belongs to a missing document"};
        if (!document.exists() && (modification == modification_t::update_k || modification == modification_t::patch_k))
            return {status_code_t::not_found_k, "Updated or patched document is missing"};
        if (document.exists() && whole && modification == modification_t::insert_k)
            return {status_code_t::already_exists_k, "Inserted document already exists"};
        if (!tree.mutable_handle) {
            value_view_t const base = document.exists() ? value_view_t(text_of_bytes(document.bytes)) : value_view_t();
            auto [parsed, parse_status] = json_parse(base, arena);
            if (parse_status.failed()) return parse_status;
            tree = parsed;
        }
        auto [modifier, modifier_status] = any_parse(value_view_t(delta.value), ustore_docs_field_type_json_k, arena);
        if (modifier_status.failed()) return modifier_status;
        if (!modifier) return {status_code_t::malformed_k, "An empty value is no JSON document"};
        auto [modifier_document, mutable_status] = json_mutable(modifier, arena);
        if (mutable_status.failed()) return mutable_status;
        if (status_t const modified = modify(tree, yyjson_mut_doc_get_root(modifier_document), delta.field,
                                             modification, arena);
            modified.failed())
            return modified;
        document.bytes = std::span<byte_t const>(empty_document_k, 0);
    }
    if (!tree.mutable_handle) return {};
    auto [text, dump_status] = json_dump({nullptr, yyjson_mut_doc_get_root(tree.mutable_handle)}, arena);
    if (dump_status.failed()) return dump_status;
    document.bytes = text.empty() ? std::span<byte_t const>(empty_document_k, 0) : bytes_of_text(text);
    return {};
}

/** Replays the deltas of every task at @p places over its document, read once through @p documents,
 *  and writes every distinct document back once, failing as the first failing delta does and
 *  writing nothing then. */
template <typename documents_type_>
status_t read_modify_write(documents_type_ const& documents, ustore_docs_write_t const& arguments,
                           place_batch_t const& places, strings_arg_t const& fields, strings_arg_t const& values,
                           view_t const& view) noexcept {

    ustore_arena_t const scratch = view.scratch;
    auto [grouped, grouped_status] = group_document_deltas(arguments, places, fields, values, scratch);
    if (grouped_status.failed()) return grouped_status;
    auto const grouped_strided = strided(grouped).immutable();
    place_batch_t distinct;
    distinct.collections_begin = grouped_strided.members(&document_deltas_t::collection).begin();
    distinct.keys_begin = grouped_strided.members(&document_deltas_t::key).begin();
    distinct.count = static_cast<ustore_size_t>(grouped.size());

    // A transaction watches the documents it rewrites, whatever the caller asked of the writes
    view_t watching = view;
    watching.watch = ustore_watch_keys_k;
    auto [found, found_status] = read_documents(documents, distinct, watching);
    if (found_status.failed()) return found_status;

    growing_strings<arena_allocator<byte_t>> modified {scratch};
    for (std::size_t index = 0; index != grouped.size(); ++index) {
        value_view_t const current = found[index];
        document_state_t replayed;
        if (current)
            replayed.bytes = current.size() ? std::span<byte_t const> {current.begin(), current.size()}
                                            : std::span<byte_t const> {empty_document_k, 0};
        if (status_t const applied = replay_deltas(replayed, grouped[index].deltas, scratch); applied.failed())
            return applied;
        if (status_t const kept = modified.push_back(value_view_t {replayed.bytes}.bytes()); kept.failed()) return kept;
    }

    strings_arg_t modified_values;
    modified_values.view = modified.tape();
    return documents.write(distinct, modified_values, arguments.durability, view);
}

#pragma endregion Writing

#pragma region Gist

/** The paths a gist found so far, sorted in scratch, viewing the bytes of the column that exports them. */
using sorted_paths_t =
    st::basic_flat_set<std::string_view, st::less_t, st::native_row_kit_t, arena_allocator<std::string_view>>;

/** Collects the unique paths to the leaves under @p node, extending the null-terminated @p path. */
inline status_t gist_paths(yyjson_val* node, field_path_buffer_t& path, sorted_paths_t& sorted_paths,
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

/** Copies the texts @p text_of answers for @p count cells into @p arena once, behind offsets as
 *  narrow as their @p total bytes allow, each followed by a null byte the text then spans when @p
 *  end asks for one. */
template <typename text_of_type_>
status_t export_texts(ustore_arena_t arena, std::size_t count, std::size_t total, string_end_t end,
                      text_of_type_&& text_of, ustore_strings_view_t& tape) noexcept {
    if (!count) return {};
    auto const export_as = [&]<typename offset_type_>(std::type_identity<offset_type_>) noexcept {
        auto [offsets, offsets_status] = arena_allocate<offset_type_>(arena, count + 1);
        if (offsets_status.failed()) return offsets_status;
        auto [bytes, bytes_status] = arena_allocate<byte_t>(arena, total);
        if (bytes_status.failed()) return bytes_status;
        std::size_t written = 0;
        offsets[0] = 0;
        for (std::size_t index = 0; index != count; ++index) {
            std::string_view const text = text_of(index);
            if (!text.empty()) std::memcpy(bytes.data() + written, text.data(), text.size());
            written += text.size();
            if (end == string_end_t::null_terminated_k) bytes[written++] = byte_t {0};
            offsets[index + 1] = static_cast<offset_type_>(written);
        }
        if constexpr (std::is_same_v<offset_type_, std::uint32_t>) tape = u32_tape(bytes.data(), offsets.data());
        else tape = u64_tape(bytes.data(), offsets.data());
        return status_t {};
    };
    if (total <= (std::numeric_limits<std::uint32_t>::max)()) return export_as(std::type_identity<std::uint32_t> {});
    return export_as(std::type_identity<std::uint64_t> {});
}

/** Implements @c ustore_docs_write over @p documents, passing whole JSON upserts through and modifying the rest as
 *  read-modify-writes. */
template <typename documents_type_>
status_t write_documents(documents_type_ const& documents, ustore_docs_write_t const& arguments,
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
        return read_modify_write(documents, arguments, places, fields, values, view);

    // Whole JSON documents pass through once they parse, which extracting the keys has already checked
    for (std::size_t index = 0; arguments.keys.data && index != arguments.count; ++index)
        if (status_t const status = json_parse(value_view_t {values[index]}, scratch).status(); status.failed())
            return status;
    return documents.write(places, values, arguments.durability, view);
}

/** Whether a read of @p arguments asks for whole JSON documents only, which the engine reads as stored. */
inline bool reads_whole_documents(ustore_docs_read_t const& arguments, strings_arg_t const& fields) noexcept {
    return !addresses_fields(fields, arguments.count) && arguments.outputs_type == ustore_docs_field_type_json_k;
}

/** Implements @c ustore_docs_read over @p source for every read but one of whole JSON documents, exporting the
 *  fields its tasks address as JSON or as strings. */
template <strings_kit_concept strings_kit_type_, typename documents_type_>
status_t read_document_fields(documents_type_ const& source, ustore_docs_read_t const& arguments,
                              strings_arg_t const& fields, view_t const& view) noexcept {
    if (arguments.outputs_type != ustore_docs_field_type_json_k &&
        arguments.outputs_type != ustore_docs_field_type_string_k)
        return {status_code_t::unsupported_k, "Documents read only as JSON or strings"};

    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_read(places); status.failed()) return status;
    auto [outputs, outputs_status] = read_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;

    // A value is present when its field is, and its text stays in its slice's scratch until copied
    auto export_one = [&](json_cell_t const& value, std::size_t task, ustore_arena_t scratch) noexcept -> status_t {
        if (value.token == json_token_t::missing_k) {
            outputs.assign_missing(task);
            return {};
        }
        bool const container = value.token == json_token_t::object_k || value.token == json_token_t::array_k;
        if (arguments.outputs_type == ustore_docs_field_type_json_k || container) {
            auto [text, text_status] = json_text_of<strings_kit_type_>(value, scratch);
            if (text_status.failed()) return text_status;
            outputs.assign(task, text);
            return {};
        }

        printed_number_buffer_t print_buffer;
        std::string_view text;
        [[maybe_unused]] conversion_t const conversion = json_to_string(value, print_buffer, text);
        if (text.data() == print_buffer) {
            auto [printed, printed_status] = arena_allocate<char>(scratch, text.size());
            if (printed_status.failed()) return printed_status;
            std::memcpy(printed.data(), text.data(), text.size());
            text = {printed.data(), text.size()};
        }
        outputs.assign(task, text);
        return {};
    };

    auto [read, read_status] = read_distinct_documents(source, places, view);
    if (read_status.failed()) return read_status;
    auto [task_fields, fields_status] = distinct_fields(fields, arguments.count, view.scratch);
    if (fields_status.failed()) return fields_status;

    // One subdocuments pass extracts every task's value
    auto const cell_of = [&](std::size_t task) noexcept {
        return field_cell_t {read.document_of(task), task_fields.fields_of_tasks[task]};
    };
    json_cells_t values;
    if (status_t const extracted = extract_fields<strings_kit_type_>(
            read.documents, task_fields.distinct, arguments.count, cell_of, arguments.database->ceiling.strings,
            view.scratch, values);
        extracted.failed())
        return extracted;
    auto export_slice = [&](std::size_t first, std::size_t length, view_t const& slice) noexcept -> status_t {
        for (std::size_t task = first; task != first + length; ++task)
            if (status_t const status = export_one(values[task], task, slice.scratch); status.failed()) return status;
        return {};
    };
    if (status_t const exported = for_task_slices(view, arguments.count, export_slice); exported.failed())
        return exported;
    outputs.assigned(arguments.count);
    return outputs.finish(arguments);
}

/** Implements @c ustore_docs_gist over @p documents. */
template <typename documents_type_>
status_t gist_documents(documents_type_ const& documents, ustore_docs_gist_t const& arguments,
                        view_t const& view) noexcept {
    if (arguments.written) *arguments.written = 0;
    if (!arguments.count) return {};

    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_read(places); status.failed()) return status;
    auto [found, found_status] = read_documents(documents, places, view);
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

/** Implements @c ustore_docs_gather over @p source. */
template <strings_kit_concept strings_kit_type_, typename documents_type_>
status_t gather_documents(documents_type_ const& source, ustore_docs_gather_t const& arguments,
                          strings_arg_t const& fields, view_t const& view) noexcept {
    if (!arguments.count || !arguments.fields_count) return {};

    if (!arguments.columns) return {status_code_t::invalid_argument_k, "Need an output for the columns"};
    if (!fields || !arguments.outputs_types)
        return {status_code_t::invalid_argument_k, "Need the fields to gather and the types of their columns"};

    std::size_t const fields_count = arguments.fields_count;
    std::size_t const docs_count = arguments.count;
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_read(places); status.failed()) return status;
    auto [documents, documents_status] = source.read(places, view);
    if (documents_status.failed()) return documents_status;

    // One subdocuments pass extracts every cell, field after field
    auto [field_names, names_status] = arena_allocate<std::string_view>(view.scratch, fields_count);
    if (names_status.failed()) return names_status;
    for (std::size_t field_index = 0; field_index != fields_count; ++field_index)
        field_names[field_index] = string_of(fields[field_index]);
    auto const cell_of = [docs_count](std::size_t cell) noexcept {
        return field_cell_t {cell % docs_count, cell / docs_count};
    };
    json_cells_t values;
    if (status_t const extracted = extract_fields<strings_kit_type_>(documents, field_names, docs_count * fields_count,
                                                                     cell_of, arguments.database->ceiling.strings,
                                                                     view.scratch, values);
        extracted.failed())
        return extracted;

    auto [columns, columns_status] = arena_allocate<ustore_docs_column_view_t>(arguments.arena, fields_count);
    if (columns_status.failed()) return columns_status;
    // The three bitmaps of a column share one allocation, each starting aligned as every output does
    std::size_t const bitmap_stride = round_up_to_multiple(divide_round_up(docs_count, bits_in_byte_k),
                                                           arena_alignment_k);
    auto [texts_bytes_of_slices, slots_status] = arena_allocate<std::size_t>(view.scratch, slice_slots(view));
    if (slots_status.failed()) return slots_status;
    printed_number_buffer_t print_buffer;
    for (std::size_t field_index = 0; field_index != fields_count; ++field_index) {
        ustore_docs_field_type_t const type = arguments.outputs_types[field_index];
        bool const holds_strings = type == ustore_docs_field_type_string_k || type == ustore_docs_field_type_binary_k;
        auto [bitmaps, bitmaps_status] = arena_allocate<ustore_octet_t>(arguments.arena, 3 * bitmap_stride);
        if (bitmaps_status.failed()) return bitmaps_status;
        std::memset(bitmaps.data(), 0, bitmaps.size());
        auto [scalars, scalars_status] = arena_allocate<ustore_byte_t>(arguments.arena,
                                                                       docs_count * field_size_bytes(type));
        if (scalars_status.failed()) return scalars_status;
        // A text column spans a null after every text, and a binary one does not
        string_end_t const end = type == ustore_docs_field_type_string_k ? string_end_t::null_terminated_k
                                                                         : string_end_t::bare_k;
        auto convert_slice = [&](std::size_t first, std::size_t length, view_t const& slice) noexcept -> status_t {
            printed_number_buffer_t print_buffer;
            std::size_t& slice_texts_bytes = texts_bytes_of_slices[slice.thread_index];
            for (std::size_t document_index = first; document_index != first + length; ++document_index) {
                json_cell_t const value = values[field_index * docs_count + document_index];
                ustore_byte_t* const scalar_column = scalars.data();
                conversion_t conversion = conversion_t::collides_k;

                switch (type) {
                case ustore_docs_field_type_bool_k:
                    conversion = gather_scalar<bool>(value, scalar_column, document_index);
                    break;
                case ustore_docs_field_type_uuid_k:
                    conversion = gather_uuid(value, scalar_column, document_index);
                    break;
                case ustore_docs_field_type_f64_k:
                    conversion = gather_scalar<double>(value, scalar_column, document_index);
                    break;
                case ustore_docs_field_type_f32_k:
                    conversion = gather_scalar<float>(value, scalar_column, document_index);
                    break;
                case ustore_docs_field_type_f16_k:
                    conversion = gather_half(value, scalar_column, document_index);
                    break;
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
                case ustore_docs_field_type_string_k:
                case ustore_docs_field_type_binary_k: {
                    std::string_view text;
                    conversion = json_to_string(value, print_buffer, text);
                    slice_texts_bytes += text.size() + (end == string_end_t::null_terminated_k);
                    break;
                }
                default: break;
                }

                std::size_t const slot = document_index / bits_in_byte_k;
                ustore_octet_t const mask = static_cast<ustore_octet_t>(1u << (document_index % bits_in_byte_k));
                record_conversion(conversion, mask, bitmaps[slot], bitmaps[bitmap_stride + slot],
                                  bitmaps[2 * bitmap_stride + slot]);
            }
            return {};
        };
        for (std::size_t& slice_bytes : texts_bytes_of_slices) slice_bytes = 0;
        if (status_t const converted = for_task_slices(view, docs_count, convert_slice); converted.failed())
            return converted;
        std::size_t texts_bytes = 0;
        for (std::size_t const slice_bytes : texts_bytes_of_slices) texts_bytes += slice_bytes;

        // The texts are sized first, so a second conversion copies each straight into the arena
        ustore_strings_view_t column_strings {};
        auto const text_of = [&](std::size_t document_index) noexcept {
            std::string_view text;
            [[maybe_unused]] conversion_t const converted = json_to_string(
                values[field_index * docs_count + document_index], print_buffer, text);
            return text;
        };
        if (holds_strings)
            if (status_t const status = export_texts(arguments.arena, docs_count, texts_bytes, end, text_of,
                                                     column_strings);
                status.failed())
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

#pragma region Documents over Blobs

/** The documents of an engine keeping each as one whole value, read and written through @c values_verbs, the
 *  unchecked whole-value verbs of that one engine. */
struct documents_in_values_t {
    blobs_verbs<home_t::host_k> const& values_verbs;
    ustore_database_t database;

    /** Reads the binary documents at @p places through @p view, appending to its scratch rather than rewinding it. */
    expected<json_documents_t> read(place_batch_t const& places, view_t const& view) const noexcept {
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

    /** Writes @p values whole at @p places. */
    status_t write(place_batch_t const& places, strings_arg_t const& values, ustore_durability_t durability,
                   view_t const& view) const noexcept {
        ustore_blobs_write_t write {};
        write.database = database;
        write.count = places.count;
        write.collections = places.collections();
        write.keys = places.keys();
        write.values = values.view;
        write.durability = durability;
        return values_verbs.write(write, values, view);
    }
};

/** The document verbs of an engine keeping every document as one whole value of JSON text, bound to its unchecked
 *  whole-value verbs @p values_verbs_ and its resolver @p resolve_, which refuses every collection holding another
 *  modality first. */
template <blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_>
status_t docs_write_over_blobs(ustore_docs_write_t const& arguments, strings_arg_t const& fields,
                               strings_arg_t const& values, view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                       ustore_modality_docs_k, view);
    if (resolved.failed()) return resolved;
    return write_documents(documents_in_values_t {values_verbs_, arguments.database}, arguments, fields, values, view);
}

template <strings_kit_concept strings_kit_type_, blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_>
status_t docs_read_over_blobs(ustore_docs_read_t const& arguments, strings_arg_t const& fields,
                              view_t const& view) noexcept {
    if (!arguments.count) return {};
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                       ustore_modality_docs_k, view);
    if (resolved.failed()) return resolved;
    if (!reads_whole_documents(arguments, fields))
        return read_document_fields<strings_kit_type_>(documents_in_values_t {values_verbs_, arguments.database},
                                                       arguments, fields, view);
    ustore_blobs_read_t read {};
    read.database = arguments.database;
    read.count = arguments.count;
    read.collections = arguments.collections;
    read.keys = arguments.keys;
    read.presences = arguments.presences;
    read.lengths = arguments.lengths;
    read.values = arguments.values;
    read.arena = arguments.arena;
    return values_verbs_.read(read, view);
}

template <blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_>
status_t docs_gist_over_blobs(ustore_docs_gist_t const& arguments, view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                       ustore_modality_docs_k, view);
    if (resolved.failed()) return resolved;
    return gist_documents(documents_in_values_t {values_verbs_, arguments.database}, arguments, view);
}

template <strings_kit_concept strings_kit_type_, blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_>
status_t docs_gather_over_blobs(ustore_docs_gather_t const& arguments, strings_arg_t const& fields,
                                view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                       ustore_modality_docs_k, view);
    if (resolved.failed()) return resolved;
    return gather_documents<strings_kit_type_>(documents_in_values_t {values_verbs_, arguments.database}, arguments,
                                               fields, view);
}

#pragma endregion Documents over Blobs

} // namespace unum::ustore
