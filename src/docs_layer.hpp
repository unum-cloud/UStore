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

#include "ustore/docs.h"               // `ustore_docs_write_t`, `ustore_docs_read_t`
#include "ustore/utilities/ranges.hpp" // `place_batch_t`, `values_view_t`

#include "ustore/docs/json_fold.hpp"      // `json_delta_t`, `apply_delta`, `modifier_text`, `document_state_t`
#include "ustore/docs/json_patch.hpp"     // `modify`, `modification_t`
#include "ustore/docs/json_paths.hpp"     // `extract_fields`, `json_documents_t`, `field_cell_t`, `json_text_of`
#include "ustore/docs/json_values.hpp"    // `json_parse`, `gather_scalar`, `field_size_bytes`, `extract_keys`
#include "ustore/utilities/algorithm.hpp" // `offset_in_sorted`, `heap_sort`, `radix_sort`, `word_byte`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_allocator`, `arena_copy`, `arena_rewind`, `growing_strings`

#include "engine.hpp"            // `blobs_verbs<vendored_residency_t::host_k>`, `view_t`, `docs_columns_t`
#include "match.hpp"             // `compiled_match`, `candidates_t`, `exhausted_t`
#include "utilities/batches.hpp" // `strings_arg_t`, `places_of`
#include "utilities/outputs.hpp" // `read_outputs_t`
#include "utilities/slices.hpp"  // `for_task_slices`, `slice_slots`, `tasks_per_word_k`

namespace unum::ustore {
using namespace unum::ustore::docs;

#pragma region Gist Paths

/** Longest path @c ustore_docs_gist exports, including the null terminator. */
inline constexpr std::size_t field_path_length_limit_k = 512;

using field_path_buffer_t = char[field_path_length_limit_k];

#pragma endregion Gist Paths

#pragma region Reading Documents

/** The field of task @p index of @p fields, empty for a whole document and for every task without fields. */
inline std::string_view field_of(strings_arg_t const& fields, std::size_t index) noexcept {
    return fields ? util::chars_of(fields[index]) : std::string_view {};
}

/** Whether any of @p count tasks addresses a field rather than a whole document, which only parsing serves. */
inline bool addresses_fields(strings_arg_t const& fields, std::size_t count) noexcept {
    for (std::size_t index = 0; fields && index != count; ++index)
        if (fields[index].length) return true;
    return false;
}

/** Reads the binary documents at @p places through @p view from @p documents, viewing them as values. */
template <typename documents_type_>
util::expected<util::values_view_t> read_documents(documents_type_ const& documents, util::place_batch_t const& places,
                                                   view_t const& view) noexcept {
    auto [read, status] = documents.read(places, view);
    if (status.failed()) return status;
    return util::values_view_t {places.count, read.presences, read.tape};
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
util::expected<task_documents_t> read_distinct_documents(documents_type_ const& source,
                                                         util::place_batch_t const& places,
                                                         view_t const& view) noexcept {

    task_documents_t read;
    if (util::all_ascending(places.keys_begin, places.count)) {
        auto [documents, status] = source.read(places, view);
        if (status.failed()) return status;
        read.documents = documents;
        return read;
    }

    auto [distinct, distinct_status] = util::arena_allocate<util::collection_key_t>(view.scratch, places.count);
    if (distinct_status.failed()) return distinct_status;
    util::transform_n(places, places.count, distinct.begin(),
                      [](util::place_t const& place) { return place.collection_key(); });
    std::size_t const distinct_count = util::sort_and_deduplicate(distinct.begin(), distinct.end());
    if (distinct_count == places.count) {
        auto [documents, status] = source.read(places, view);
        if (status.failed()) return status;
        read.documents = documents;
        return read;
    }

    std::span<util::collection_key_t> const sorted = distinct.first(distinct_count);
    auto const sorted_strided = util::strided(sorted).immutable();
    util::place_batch_t distinct_places;
    distinct_places.collections_begin = sorted_strided.members(&util::collection_key_t::collection).begin();
    distinct_places.keys_begin = sorted_strided.members(&util::collection_key_t::key).begin();
    distinct_places.count = static_cast<ustore_size_t>(distinct_count);

    auto [documents, documents_status] = source.read(distinct_places, view);
    if (documents_status.failed()) return documents_status;
    auto [documents_of_tasks, mapping_status] = util::arena_allocate<std::size_t>(view.scratch, places.count);
    if (mapping_status.failed()) return mapping_status;
    for (std::size_t index = 0; index != places.size(); ++index)
        documents_of_tasks[index] = util::offset_in_sorted(sorted, places[index].collection_key());
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
inline util::expected<task_fields_t> distinct_fields(strings_arg_t const& fields, std::size_t count,
                                                     ustore_arena_t scratch) noexcept {
    auto [distinct, distinct_status] = util::arena_allocate<std::string_view>(scratch, count);
    if (distinct_status.failed()) return distinct_status;
    auto [fields_of_tasks, mapping_status] = util::arena_allocate<std::size_t>(scratch, count);
    if (mapping_status.failed()) return mapping_status;
    for (std::size_t index = 0; index != count; ++index) distinct[index] = field_of(fields, index);
    util::heap_sort(distinct, [](std::string_view first, std::string_view second) noexcept { return first < second; });
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

/** Turns the whole upsert @p delta into a write of the document, first checking that a JSON
 *  @p value parses when @p arguments names its keys, as extracting them parses it otherwise. */
inline util::status_t turn_into_write(json_delta_t& delta, util::value_view_t const value,
                                      ustore_docs_write_t const& arguments, ustore_arena_t scratch) noexcept {
    delta.modification = delta_modification_t::write_k;
    if (arguments.type != ustore_docs_field_type_json_k || !arguments.keys.data) return {};
    return json_parse(value, scratch).status();
}

/**
 *  @brief Turns every task at @p places into its delta and groups the deltas per document, each
 *      document's in task order, so a replay of one group applies every task of the call on that
 *      document, each seeing the ones before.
 *
 *  A whole upsert becomes a write of the document, which a whole JSON upsert passes through once
 *  it parses as JSON, as extracting the keys of @p arguments has checked when it names none.
 */
inline util::expected<std::span<document_deltas_t>> group_document_deltas(ustore_docs_write_t const& arguments,
                                                                          util::place_batch_t const& places,
                                                                          strings_arg_t const& fields,
                                                                          strings_arg_t const& values,
                                                                          ustore_arena_t scratch) noexcept {
    ustore_docs_modification_t const modification = arguments.modification;
    ustore_docs_field_type_t const type = arguments.type;
    std::size_t const count = places.size();
    auto [tasks, tasks_status] = util::arena_allocate<document_task_t>(scratch, count);
    if (tasks_status.failed()) return tasks_status;
    for (std::size_t index = 0; index != count; ++index) {
        std::string_view const field = field_of(fields, index);
        util::value_view_t const value {values[index]};
        json_delta_t delta {0, static_cast<delta_modification_t>(modification), field, {}};
        if (field.empty() && delta.modification == delta_modification_t::upsert_k) {
            util::status_t const turned = turn_into_write(delta, value, arguments, scratch);
            if (turned.failed()) return turned;
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
        return util::word_byte(task.sequence, digit - 2 * uuid_bytes_k);
    };
    auto const precedes = [](document_task_t const& first, document_task_t const& second) noexcept {
        if (first.collection != second.collection) return first.collection < second.collection;
        if (first.key != second.key) return first.key < second.key;
        return first.sequence < second.sequence;
    };
    util::radix_sort(tasks, 2 * uuid_bytes_k + sizeof(std::uint64_t), digit_of, precedes);

    // The deltas of every document sit side by side in task order, which each document's span views
    auto [deltas, deltas_status] = util::arena_allocate<json_delta_t>(scratch, count);
    if (deltas_status.failed()) return deltas_status;
    auto [documents, documents_status] = util::arena_allocate<document_deltas_t>(scratch, count);
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
inline util::status_t replay_deltas(document_state_t& document, std::span<json_delta_t const> deltas,
                                    ustore_arena_t arena) noexcept {
    // The tree holds the document from the first modification on, until a whole write replaces it
    json_t tree;
    for (json_delta_t const& delta : deltas) {
        if (delta.modification == delta_modification_t::write_k ||
            delta.modification == delta_modification_t::remove_k) {
            tree = {};
            if (util::status_t const applied = apply_delta(document, delta, arena); applied.failed()) return applied;
            continue;
        }
        auto const modification = static_cast<modification_t>(delta.modification);
        bool const whole = delta.field.empty();
        if (!document.exists() && !whole)
            return {util::status_code_t::not_found_k, "Modified field belongs to a missing document"};
        if (!document.exists() && (modification == modification_t::update_k || modification == modification_t::patch_k))
            return {util::status_code_t::not_found_k, "Updated or patched document is missing"};
        if (document.exists() && whole && modification == modification_t::insert_k)
            return {util::status_code_t::already_exists_k, "Inserted document already exists"};
        if (!tree.mutable_handle) {
            util::value_view_t const base = document.exists() ? util::value_view_t(util::chars_of(document.bytes))
                                                              : util::value_view_t();
            auto [parsed, parse_status] = json_parse(base, arena);
            if (parse_status.failed()) return parse_status;
            tree = parsed;
        }
        auto [modifier, modifier_status] = any_parse(util::value_view_t(delta.value), ustore_docs_field_type_json_k,
                                                     arena);
        if (modifier_status.failed()) return modifier_status;
        if (!modifier) return {util::status_code_t::malformed_k, "An empty value is no JSON document"};
        auto [modifier_document, mutable_status] = json_mutable(modifier, arena);
        if (mutable_status.failed()) return mutable_status;
        if (util::status_t const modified = modify(tree, yyjson_mut_doc_get_root(modifier_document), delta.field,
                                                   modification, arena);
            modified.failed())
            return modified;
        document.bytes = std::span<util::byte_t const>(empty_document_k, 0);
    }
    if (!tree.mutable_handle) return {};
    auto [text, dump_status] = json_dump({nullptr, yyjson_mut_doc_get_root(tree.mutable_handle)}, arena);
    if (dump_status.failed()) return dump_status;
    document.bytes = text.empty() ? std::span<util::byte_t const>(empty_document_k, 0) : bytes_of_text(text);
    return {};
}

/** Replays the deltas of every task at @p places over its document, read once through @p documents,
 *  and writes every distinct document back once, failing as the first failing delta does and
 *  writing nothing then. */
template <typename documents_type_>
util::status_t read_modify_write(documents_type_ const& documents, ustore_docs_write_t const& arguments,
                                 util::place_batch_t const& places, strings_arg_t const& fields,
                                 strings_arg_t const& values, view_t const& view) noexcept {

    ustore_arena_t const scratch = view.scratch;
    auto [grouped, grouped_status] = group_document_deltas(arguments, places, fields, values, scratch);
    if (grouped_status.failed()) return grouped_status;
    auto const grouped_strided = util::strided(grouped).immutable();
    util::place_batch_t distinct;
    distinct.collections_begin = grouped_strided.members(&document_deltas_t::collection).begin();
    distinct.keys_begin = grouped_strided.members(&document_deltas_t::key).begin();
    distinct.count = static_cast<ustore_size_t>(grouped.size());

    // A transaction watches the documents it rewrites, whatever the caller asked of the writes
    view_t watching = view;
    watching.watch = ustore_watch_keys_k;
    auto [found, found_status] = read_documents(documents, distinct, watching);
    if (found_status.failed()) return found_status;

    util::growing_strings<util::arena_allocator<util::byte_t>> modified {scratch};
    for (std::size_t index = 0; index != grouped.size(); ++index) {
        util::value_view_t const current = found[index];
        document_state_t replayed;
        if (current)
            replayed.bytes = current.size() ? std::span<util::byte_t const> {current.begin(), current.size()}
                                            : std::span<util::byte_t const> {empty_document_k, 0};
        if (util::status_t const applied = replay_deltas(replayed, grouped[index].deltas, scratch); applied.failed())
            return applied;
        if (util::status_t const kept = modified.push_back(util::value_view_t {replayed.bytes}.bytes()); kept.failed())
            return kept;
    }

    strings_arg_t modified_values;
    modified_values.view = modified.tape();
    return documents.write(distinct, modified_values, arguments.durability, view);
}

#pragma endregion Writing

#pragma region Gist

/** The paths a gist found so far, sorted in scratch, viewing the bytes of the column that exports them. */
using sorted_paths_t =
    st::basic_flat_set<std::string_view, st::less_t, st::native_row_kit_t, util::arena_allocator<std::string_view>>;

/** Collects the unique paths to the leaves under @p node, extending the null-terminated @p path. */
inline util::status_t gist_paths(yyjson_val* node, field_path_buffer_t& path, sorted_paths_t& sorted_paths,
                                 util::growing_strings<util::arena_allocator<util::byte_t>>& exported_paths) noexcept {

    std::size_t const path_length = std::strlen(path);

    if (yyjson_is_obj(node)) {
        yyjson_obj_iter iterator;
        yyjson_obj_iter_init(node, &iterator);
        while (yyjson_val* const key = yyjson_obj_iter_next(&iterator)) {
            std::string_view const name {yyjson_get_str(key), yyjson_get_len(key)};
            if (path_length + 1 + escaped_length(name) >= field_path_length_limit_k)
                return {util::status_code_t::capacity_exhausted_k, "Path is too long"};
            path[path_length] = '/';
            *escape_into(name, path + path_length + 1) = '\0';
            if (util::status_t const status = gist_paths(yyjson_obj_iter_get_val(key), path, sorted_paths,
                                                         exported_paths);
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
            if (printed.empty()) return {util::status_code_t::capacity_exhausted_k, "Path is too long"};
            if (util::status_t const status = gist_paths(element, path, sorted_paths, exported_paths); status.failed())
                return status;
        }
        path[path_length] = '\0';
        return {};
    }

    std::string_view const leaf {path, path_length};
    if (sorted_paths.find(leaf)) return {};
    if (util::status_t const status = exported_paths.push_back(util::bytes_of(leaf)); status.failed()) return status;

    // The column may move as it grows, leaving behind a copy that stays readable until the scratch rewinds
    return util::growth_status(sorted_paths.insert(util::chars_of(exported_paths[exported_paths.size() - 1])));
}

#pragma endregion Gist

#pragma region Document Calls

/** Copies the texts @p text_of answers for @p count cells into @p arena once, behind offsets as
 *  narrow as their @p total bytes allow, each followed by a null byte the text then spans when @p
 *  end asks for one. */
template <typename text_of_type_>
util::status_t export_texts(ustore_arena_t arena, std::size_t count, std::size_t total, util::string_end_t end,
                            text_of_type_&& text_of, ustore_strings_view_t& tape) noexcept {
    if (!count) return {};
    auto const export_as = [&]<typename offset_type_>(std::type_identity<offset_type_>) noexcept {
        auto [offsets, offsets_status] = util::arena_allocate<offset_type_>(arena, count + 1);
        if (offsets_status.failed()) return offsets_status;
        auto [bytes, bytes_status] = util::arena_allocate<util::byte_t>(arena, total);
        if (bytes_status.failed()) return bytes_status;
        std::size_t written = 0;
        offsets[0] = 0;
        for (std::size_t index = 0; index != count; ++index) {
            std::string_view const text = text_of(index);
            if (!text.empty()) std::memcpy(bytes.data() + written, text.data(), text.size());
            written += text.size();
            if (end == util::string_end_t::null_terminated_k) bytes[written++] = util::byte_t {0};
            offsets[index + 1] = static_cast<offset_type_>(written);
        }
        if constexpr (std::is_same_v<offset_type_, std::uint32_t>) tape = util::u32_tape(bytes.data(), offsets.data());
        else tape = util::u64_tape(bytes.data(), offsets.data());
        return util::status_t {};
    };
    if (total <= (std::numeric_limits<std::uint32_t>::max)()) return export_as(std::type_identity<std::uint32_t> {});
    return export_as(std::type_identity<std::uint64_t> {});
}

/** Implements @c ustore_docs_write over @p documents, passing whole JSON upserts through and modifying the rest as
 *  read-modify-writes. */
template <typename documents_type_>
util::status_t write_documents(documents_type_ const& documents, ustore_docs_write_t const& arguments,
                               strings_arg_t const& fields, strings_arg_t const& values, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!values) return {util::status_code_t::invalid_argument_k, "No values were provided"};
    std::string_view const id_field = util::chars_of(arguments.id_field);
    if (!arguments.keys.data && id_field.empty())
        return {util::status_code_t::invalid_argument_k, "Documents without keys need an id field"};

    ustore_arena_t const scratch = view.scratch;
    util::place_batch_t places = util::places_of(arguments);
    if (!arguments.keys.data) {
        auto [extracted, status] = util::arena_allocate<ustore_key_t>(scratch, arguments.count);
        if (status.failed()) return status;
        if (util::status_t const extracting = extract_keys(values, id_field, scratch, extracted); extracting.failed())
            return extracting;
        places.keys_begin = {extracted.data(), sizeof(ustore_key_t)};
    }

    if (addresses_fields(fields, arguments.count) || arguments.type != ustore_docs_field_type_json_k ||
        arguments.modification != ustore_docs_modify_upsert_k)
        return read_modify_write(documents, arguments, places, fields, values, view);

    // Whole JSON documents pass through once they parse, which extracting the keys has already checked
    for (std::size_t index = 0; arguments.keys.data && index != arguments.count; ++index)
        if (util::status_t const status = json_parse(util::value_view_t {values[index]}, scratch).status();
            status.failed())
            return status;
    return documents.write(places, values, arguments.durability, view);
}

/** Whether a read of @p arguments asks for whole JSON documents only, which the engine reads as stored. */
inline bool reads_whole_documents(ustore_docs_read_t const& arguments, strings_arg_t const& fields) noexcept {
    return !addresses_fields(fields, arguments.count) && arguments.outputs_type == ustore_docs_field_type_json_k;
}

/** Implements @c ustore_docs_read over @p source for every read but one of whole JSON documents, exporting the
 *  fields its tasks address as JSON or as strings. */
template <util::strings_kit_concept strings_kit_type_, typename documents_type_>
util::status_t read_document_fields(documents_type_ const& source, ustore_docs_read_t const& arguments,
                                    strings_arg_t const& fields, view_t const& view) noexcept {
    if (arguments.outputs_type != ustore_docs_field_type_json_k &&
        arguments.outputs_type != ustore_docs_field_type_string_k)
        return {util::status_code_t::unsupported_k, "Documents read only as JSON or strings"};

    util::place_batch_t const places = util::places_of(arguments);
    if (util::status_t const status = validate_read(places); status.failed()) return status;
    auto [outputs, outputs_status] = util::read_outputs_t::make(arguments, view.scratch);
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
            auto [printed, printed_status] = util::arena_allocate<char>(scratch, text.size());
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
    if (util::status_t const extracted = extract_fields<strings_kit_type_>(
            read.documents, task_fields.distinct, arguments.count, cell_of, arguments.database->ceiling.strings,
            view.scratch, values);
        extracted.failed())
        return extracted;
    auto export_slice = [&](std::size_t first, std::size_t length, std::size_t,
                            view_t const& slice) noexcept -> status_t {
        for (std::size_t task = first; task != first + length; ++task)
            if (util::status_t const status = export_one(values[task], task, slice.scratch); status.failed())
                return status;
        return {};
    };
    if (util::status_t const exported = util::for_task_slices(view, arguments.count, export_slice); exported.failed())
        return exported;
    outputs.assigned(arguments.count);
    return outputs.finish(arguments);
}

/** Implements @c ustore_docs_gist over @p documents. */
template <typename documents_type_>
util::status_t gist_documents(documents_type_ const& documents, ustore_docs_gist_t const& arguments,
                              view_t const& view) noexcept {
    if (arguments.written) *arguments.written = 0;
    if (!arguments.count) return {};

    util::place_batch_t const places = util::places_of(arguments);
    if (util::status_t const status = validate_read(places); status.failed()) return status;
    auto [found, found_status] = read_documents(documents, places, view);
    if (found_status.failed()) return found_status;

    // The paths and their offsets grow side by side in scratch, and only their final lengths reach the arena
    field_path_buffer_t path = {0};
    sorted_paths_t sorted_paths {{}, {view.scratch}};
    util::growing_strings<util::arena_allocator<util::byte_t>> exported_paths {view.scratch};
    for (std::size_t index = 0; index != arguments.count; ++index) {
        auto [document, document_status] = json_parse(found[index], view.scratch);
        if (document_status.failed()) return document_status;
        if (!document) continue;
        if (util::status_t const status = gist_paths(yyjson_doc_get_root(document.handle), path, sorted_paths,
                                                     exported_paths);
            status.failed())
            return status;
    }

    if (arguments.fields)
        if (util::status_t const status = exported_paths.export_into(arguments.arena, *arguments.fields);
            status.failed())
            return status;
    if (arguments.written) *arguments.written = static_cast<ustore_size_t>(sorted_paths.size());
    return {};
}

/**
 *  @brief Reads the documents at @p places from @p source in flights, staged in the scratch of
 *      the first slice, and hands each to @p visit on the calling thread as
 *      <tt>visit(documents, flight_begin)</tt>.
 *
 *  A flight is a word of words of keys per slice of @p view, read by one fork of the slices.
 *  After each flight the scratch of every slice rewinds, but the call's own, so a document lives
 *  only until its flight is visited, a warm flight allocates nothing, and the call's arena
 *  receives nothing but its outputs. A call without an executor stages in its own scratch.
 */
template <typename documents_type_, typename visit_type_>
util::status_t read_in_flights(documents_type_ const& source, util::place_batch_t const& places, view_t const& view,
                               visit_type_&& visit) noexcept {
    // A slice drains its ring once a flight, so a share of the default ring depth squared, a word
    // of words, spends one round in every depth of rounds on that drain
    std::size_t const flight = util::slice_slots(view) * util::tasks_per_word_k * util::tasks_per_word_k;
    view_t staged = view;
    staged.scratch = view.executor ? &view.executor->scratches[0] : view.scratch;
    for (std::size_t begin = 0; begin < places.count; begin += flight) {
        auto const signed_begin = static_cast<std::ptrdiff_t>(begin);
        util::place_batch_t flight_places = places;
        flight_places.collections_begin = places.collections_begin ? places.collections_begin + signed_begin
                                                                   : places.collections_begin;
        flight_places.keys_begin = places.keys_begin + signed_begin;
        flight_places.count = static_cast<ustore_size_t>(fu::min_of<std::size_t>(flight, places.count - begin));
        auto [documents, read_status] = source.read(flight_places, staged);
        if (read_status.failed()) return read_status;
        if (util::status_t const visited = visit(documents, begin); visited.failed()) return visited;
        for (std::size_t thread = 0; view.executor && thread != view.executor->scratches.size(); ++thread)
            if (&view.executor->scratches[thread] != view.scratch)
                util::arena_rewind(&view.executor->scratches[thread]);
    }
    return {};
}

/** The cells of one gathered field, staged in the call's scratch until the last flight is read:
 *  the three bitmaps one stride apart, the scalars, and every text. */
struct column_fragments_t {
    std::span<ustore_octet_t> bitmaps;
    std::span<ustore_byte_t> scalars;
    std::span<std::string_view> texts;
};

/** Views @p value as JSON text in @p text: as its document spells it, or a string quoted and
 *  escaped again into @p scratch, as its cell holds the body unescaped. */
inline conversion_t json_text(json_cell_t const& value, ustore_arena_t scratch, std::string_view& text) noexcept {
    if (value.token == json_token_t::missing_k) return conversion_t::missing_k;
    if (value.token != json_token_t::string_k) {
        text = value.bytes;
        return conversion_t::exact_k;
    }
    auto const escape_of = [](char character) noexcept -> char {
        switch (character) {
        case '"': return '"';
        case '\\': return '\\';
        case '\b': return 'b';
        case '\f': return 'f';
        case '\n': return 'n';
        case '\r': return 'r';
        case '\t': return 't';
        default: return static_cast<unsigned char>(character) < 0x20 ? 'u' : 0;
        }
    };
    std::size_t length = 2;
    for (char const character : value.bytes) {
        char const escape = escape_of(character);
        length += escape == 'u' ? 6 : escape ? 2 : 1;
    }
    auto [quoted, quoted_status] = util::arena_allocate<char>(scratch, length);
    if (quoted_status.failed()) return conversion_t::collides_k;
    char constexpr hex_digits_k[] = "0123456789abcdef";
    std::size_t at = 0;
    quoted[at++] = '"';
    for (char const character : value.bytes) {
        char const escape = escape_of(character);
        if (!escape) {
            quoted[at++] = character;
            continue;
        }
        quoted[at++] = '\\';
        quoted[at++] = escape;
        if (escape != 'u') continue;
        auto const code = static_cast<unsigned char>(character);
        quoted[at++] = '0', quoted[at++] = '0', quoted[at++] = hex_digits_k[code >> 4],
        quoted[at++] = hex_digits_k[code & 0xF];
    }
    quoted[at++] = '"';
    text = std::string_view {quoted.data(), at};
    return conversion_t::exact_k;
}

/** Converts @p value into row @p row of the @p scalars of a column of @p type, or views the
 *  text of a string, binary or JSON column in @p text, which may point into @p print_buffer or
 *  into @p scratch. */
inline conversion_t gather_cell(ustore_docs_field_type_t type, json_cell_t const& value, ustore_byte_t* scalars,
                                std::size_t row, printed_number_buffer_t& print_buffer, std::string_view& text,
                                ustore_arena_t scratch) noexcept {
    switch (type) {
    case ustore_docs_field_type_json_k: return json_text(value, scratch, text);
    case ustore_docs_field_type_bool_k: return gather_scalar<bool>(value, scalars, row);
    case ustore_docs_field_type_uuid_k: return gather_uuid(value, scalars, row);
    case ustore_docs_field_type_f64_k: return gather_scalar<double>(value, scalars, row);
    case ustore_docs_field_type_f32_k: return gather_scalar<float>(value, scalars, row);
    case ustore_docs_field_type_f16_k: return gather_half(value, scalars, row);
    case ustore_docs_field_type_i64_k: return gather_scalar<std::int64_t>(value, scalars, row);
    case ustore_docs_field_type_i32_k: return gather_scalar<std::int32_t>(value, scalars, row);
    case ustore_docs_field_type_i16_k: return gather_scalar<std::int16_t>(value, scalars, row);
    case ustore_docs_field_type_i8_k: return gather_scalar<std::int8_t>(value, scalars, row);
    case ustore_docs_field_type_u64_k: return gather_scalar<std::uint64_t>(value, scalars, row);
    case ustore_docs_field_type_u32_k: return gather_scalar<std::uint32_t>(value, scalars, row);
    case ustore_docs_field_type_u16_k: return gather_scalar<std::uint16_t>(value, scalars, row);
    case ustore_docs_field_type_u8_k: return gather_scalar<std::uint8_t>(value, scalars, row);
    case ustore_docs_field_type_string_k:
    case ustore_docs_field_type_binary_k: return json_to_string(value, print_buffer, text);
    default: return conversion_t::collides_k;
    }
}

/** Copies the staged cells of every field into the columns of @p arguments in their arena, each
 *  staged row landing at the task @p tasks names for it: the bitmaps, the scalars, and the texts
 *  once, a text column spanning a null after every text. */
inline util::status_t fragments_into_columns(std::span<column_fragments_t const> fragments,
                                             std::span<std::size_t const> tasks, ustore_docs_gather_t const& arguments,
                                             ustore_arena_t scratch) noexcept {
    std::size_t const docs_count = arguments.count;
    auto [rows_of_tasks, rows_status] = util::arena_allocate<std::size_t>(scratch, docs_count);
    if (rows_status.failed()) return rows_status;
    for (std::size_t row = 0; row != docs_count; ++row) rows_of_tasks[tasks[row]] = row;
    auto [columns, columns_status] = util::arena_allocate<ustore_docs_column_view_t>(arguments.arena, fragments.size());
    if (columns_status.failed()) return columns_status;
    for (std::size_t field_index = 0; field_index != fragments.size(); ++field_index) {
        column_fragments_t const& staged = fragments[field_index];
        std::size_t const bitmap_stride = staged.bitmaps.size() / 3;
        std::size_t const scalar_bytes = staged.scalars.size() / docs_count;
        auto [bitmaps, bitmaps_status] = util::arena_allocate<ustore_octet_t>(arguments.arena, staged.bitmaps.size());
        if (bitmaps_status.failed()) return bitmaps_status;
        std::memset(bitmaps.data(), 0, bitmaps.size());
        auto [scalars, scalars_status] = util::arena_allocate<ustore_byte_t>(arguments.arena, staged.scalars.size());
        if (scalars_status.failed()) return scalars_status;
        for (std::size_t task = 0; task != docs_count; ++task) {
            std::size_t const row = rows_of_tasks[task];
            for (std::size_t bitmap = 0; bitmap != 3; ++bitmap)
                bitmaps[bitmap * bitmap_stride + task / util::bits_in_byte_k] |= static_cast<ustore_octet_t>(
                    ((staged.bitmaps[bitmap * bitmap_stride + row / util::bits_in_byte_k] >>
                      (row % util::bits_in_byte_k)) &
                     1u)
                    << (task % util::bits_in_byte_k));
            if (scalar_bytes)
                std::memcpy(scalars.data() + task * scalar_bytes, staged.scalars.data() + row * scalar_bytes,
                            scalar_bytes);
        }
        util::string_end_t const end = arguments.outputs_types[field_index] == ustore_docs_field_type_string_k
                                           ? util::string_end_t::null_terminated_k
                                           : util::string_end_t::bare_k;
        std::size_t texts_bytes = 0;
        for (std::string_view const text : staged.texts)
            texts_bytes += text.size() + (end == util::string_end_t::null_terminated_k);
        ustore_strings_view_t column_strings {};
        auto const text_of = [&](std::size_t task) noexcept { return staged.texts[rows_of_tasks[task]]; };
        if (!staged.texts.empty())
            if (util::status_t const status = export_texts(arguments.arena, docs_count, texts_bytes, end, text_of,
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

/**
 *  @brief Implements @c ustore_docs_gather over @p source, extracting the fields of each flight of
 *      documents as it lands.
 *
 *  One flight of documents is staged at a time, and every slice extracts the cells of its share of
 *  the flight into the fragments of each field, staged in the call's scratch, so only the cells
 *  outlive their documents and reach the arena once the last flight is read.
 */
template <util::strings_kit_concept strings_kit_type_, typename documents_type_>
util::status_t gather_documents(documents_type_ const& source, ustore_docs_gather_t const& arguments,
                                strings_arg_t const& fields, view_t const& view) noexcept {
    if (!arguments.count || !arguments.fields_count) return {};

    if (!arguments.columns) return {util::status_code_t::invalid_argument_k, "Need an output for the columns"};
    if (!fields || !arguments.outputs_types)
        return {util::status_code_t::invalid_argument_k, "Needs the gathered fields and column types"};

    std::size_t const fields_count = arguments.fields_count;
    std::size_t const docs_count = arguments.count;
    util::place_batch_t const places = util::places_of(arguments);
    if (util::status_t const status = validate_read(places); status.failed()) return status;
    // Flights cut from the places sorted once read neighbours, and rows stage in that order
    auto [sorted, sorted_status] = sort_places(places, view.scratch);
    if (sorted_status.failed()) return sorted_status;

    auto [field_names, names_status] = util::arena_allocate<std::string_view>(view.scratch, fields_count);
    if (names_status.failed()) return names_status;
    for (std::size_t field_index = 0; field_index != fields_count; ++field_index)
        field_names[field_index] = util::chars_of(fields[field_index]);
    // The three bitmaps of a column share one allocation, each starting aligned as every output does
    std::size_t const bitmap_stride = util::round_up_to_multiple(
        util::divide_round_up(docs_count, util::bits_in_byte_k), util::arena_alignment_k);
    auto [fragments, fragments_status] = util::arena_allocate<column_fragments_t>(view.scratch, fields_count);
    if (fragments_status.failed()) return fragments_status;
    for (std::size_t field_index = 0; field_index != fields_count; ++field_index) {
        ustore_docs_field_type_t const type = arguments.outputs_types[field_index];
        bool const holds_strings = !field_size_bytes(type);
        auto [bitmaps, bitmaps_status] = util::arena_allocate<ustore_octet_t>(view.scratch, 3 * bitmap_stride);
        if (bitmaps_status.failed()) return bitmaps_status;
        std::memset(bitmaps.data(), 0, bitmaps.size());
        auto [scalars, scalars_status] = util::arena_allocate<ustore_byte_t>(view.scratch,
                                                                             docs_count * field_size_bytes(type));
        if (scalars_status.failed()) return scalars_status;
        auto [texts, texts_status] = util::arena_allocate<std::string_view>(view.scratch,
                                                                            holds_strings ? docs_count : 0);
        if (texts_status.failed()) return texts_status;
        fragments[field_index] = {bitmaps, scalars, texts};
    }

    // A slice copies its texts out of the documents into its scratch, which each flight rewinds
    auto extract_slice = [&](json_documents_t const& documents, std::size_t flight_begin, std::size_t first,
                             std::size_t length, view_t const& slice) noexcept -> status_t {
        auto const cell_of = [first, length](std::size_t cell) noexcept {
            return field_cell_t {first + cell % length, cell / length};
        };
        json_cells_t values;
        if (util::status_t const extracted = extract_fields<strings_kit_type_>(
                documents, field_names, length * fields_count, cell_of, arguments.database->ceiling.strings,
                slice.scratch, values);
            extracted.failed())
            return extracted;
        printed_number_buffer_t print_buffer;
        for (std::size_t field_index = 0; field_index != fields_count; ++field_index) {
            column_fragments_t const& staged = fragments[field_index];
            for (std::size_t offset = 0; offset != length; ++offset) {
                std::size_t const row = flight_begin + first + offset;
                std::string_view text;
                conversion_t const conversion = gather_cell(
                    arguments.outputs_types[field_index], values[field_index * length + offset], staged.scalars.data(),
                    row, print_buffer, text, slice.scratch);
                std::size_t const slot = row / util::bits_in_byte_k;
                ustore_octet_t const mask = static_cast<ustore_octet_t>(1u << (row % util::bits_in_byte_k));
                record_conversion(conversion, mask, staged.bitmaps[slot], staged.bitmaps[bitmap_stride + slot],
                                  staged.bitmaps[2 * bitmap_stride + slot]);
                if (staged.texts.empty()) continue;
                auto [copied, copied_status] = util::arena_copy<char>(slice.scratch, std::span<char const> {text});
                if (copied_status.failed()) return copied_status;
                staged.texts[row] = std::string_view {copied.begin(), copied.end()};
            }
        }
        return {};
    };
    // The flight's texts move into the call's scratch before the slices' scratches rewind
    auto keep_texts = [&](std::size_t flight_begin, std::size_t flight_count) noexcept -> status_t {
        for (column_fragments_t const& staged : fragments) {
            std::span<std::string_view> const texts = staged.texts.empty()
                                                          ? staged.texts
                                                          : staged.texts.subspan(flight_begin, flight_count);
            std::size_t bytes = 0;
            for (std::string_view const text : texts) bytes += text.size();
            auto [kept, kept_status] = util::arena_allocate<char>(view.scratch, bytes);
            if (kept_status.failed()) return kept_status;
            for (std::size_t index = 0, at = 0; index != texts.size(); at += texts[index].size(), ++index) {
                if (!texts[index].empty()) std::memcpy(kept.data() + at, texts[index].data(), texts[index].size());
                texts[index] = {kept.data() + at, texts[index].size()};
            }
        }
        return {};
    };
    auto visit_flight = [&](json_documents_t const& documents, std::size_t flight_begin) noexcept -> status_t {
        auto flight_slice = [&](std::size_t first, std::size_t length, std::size_t, view_t const& slice) noexcept {
            return extract_slice(documents, flight_begin, first, length, slice);
        };
        if (util::status_t const extracted = util::for_task_slices(view, documents.count, flight_slice);
            extracted.failed())
            return extracted;
        return keep_texts(flight_begin, documents.count);
    };
    if (util::status_t const read = read_in_flights(source, sorted.places, view, visit_flight); read.failed())
        return read;
    return fragments_into_columns(std::span<column_fragments_t const> {fragments},
                                  std::span<std::size_t const> {sorted.tasks}, arguments, view.scratch);
}

#pragma endregion Document Calls

#pragma region Cursor Batches

/** One task's batch of a docs cursor's pull, carved at the bounds of its task before the tasks
 *  run. */
struct docs_batch_t {
    std::span<ustore_key_t> keys;
    ustore_docs_column_view_t* columns = nullptr;
    std::size_t rows = 0;
};

/** The room one column of one batch was carved with, writable where its view only reads, and the
 *  text bytes it holds of how many it may. */
struct docs_column_room_t {
    ustore_octet_t* bitmaps = nullptr;
    std::size_t bitmap_stride = 0;
    ustore_byte_t* scalars = nullptr;
    util::byte_t* texts = nullptr;
    std::uint32_t* narrow_offsets = nullptr;
    std::uint64_t* wide_offsets = nullptr;
    std::size_t used = 0;
    std::size_t capacity = 0;
};

/**
 *  @brief The batches one pull of a docs cursor fills, one per task, with a row per key the match
 *      keeps and a cell of every column the cursor gathers.
 *
 *  The calling thread carves every batch from the pull's arena at its task's bounds, once, before
 *  the tasks run; then each task narrows and appends its rounds of keys to its own batch alone, so
 *  no thread grows the arena.
 */
struct docs_batches_t {

    /** The columns every row gathers. */
    docs_columns_t columns;

    /** The host match the rounds ask, or null for one a device pull asked already. */
    compiled_match<vendored_residency_t::host_k> const* match = nullptr;

    /** The arena holding every output of the pull. */
    ustore_arena_t arena = nullptr;

    /** The StringZilla capabilities the fields are extracted with. */
    sz_capability_t strings {};

    /** Where the walk after this pull starts, which the engine sets. */
    std::variant<ustore_key_t, exhausted_t> resume;

    /** The batches, one per task. */
    std::span<docs_batch_t> batches;

    /** The room of every column of every batch, batch after batch. */
    std::span<docs_column_room_t> rooms;

    /** The views the pull hands out, in the arena. */
    std::span<ustore_docs_batch_view_t> views;

    /** Whether the match reads values, so its engine serves one task at a time. */
    bool reads_values() const noexcept { return match && match->reads_values(); }

    /** Rows batch @p task may still take below @p limit, its share of the pull. */
    std::size_t room(std::size_t task, std::size_t limit) const noexcept { return limit - batches[task].rows; }

    /** Whether field @p field is the whole document, copied as it is. */
    bool whole(std::size_t field) const noexcept {
        return columns.fields[field].empty() && columns.types[field] == ustore_docs_field_type_json_k;
    }

    /**
     *  @brief Carves one batch per entry of @p max_rows from the arena, each room for that
     *      many rows and for the texts of documents of @p max_bytes bytes in all, its
     *      bookkeeping in @p scratch.
     *
     *  A whole document's text is its bytes, and a field's at most its document's or
     *  a printed number.
     */
    util::status_t carve(std::span<std::uint64_t const> max_rows, std::span<std::uint64_t const> max_bytes,
                         ustore_arena_t scratch) noexcept {
        std::size_t const count = max_rows.size();
        std::size_t const fields = columns.fields.size();
        auto [carved, carved_status] = util::arena_allocate<docs_batch_t>(scratch, count);
        if (carved_status.failed()) return carved_status;
        auto [carved_rooms, rooms_status] = util::arena_allocate<docs_column_room_t>(scratch, count * fields);
        if (rooms_status.failed()) return rooms_status;
        auto [carved_views, views_status] = util::arena_allocate<ustore_docs_batch_view_t>(arena, count);
        if (views_status.failed()) return views_status;
        batches = carved, rooms = carved_rooms, views = carved_views;
        for (std::size_t task = 0; task != count; ++task) {
            std::size_t const rows = static_cast<std::size_t>(max_rows[task]);
            docs_batch_t& batch = batches[task];
            batch = {};
            if (!rows) continue;
            auto [keys, keys_status] = util::arena_allocate<ustore_key_t>(arena, rows);
            if (keys_status.failed()) return keys_status;
            auto [views_of_columns, columns_status] = util::arena_allocate<ustore_docs_column_view_t>(arena, fields);
            if (columns_status.failed()) return columns_status;
            batch.keys = keys, batch.columns = views_of_columns.data();
            std::size_t const bitmap_stride = util::round_up_to_multiple(
                util::divide_round_up(rows, util::bits_in_byte_k), util::arena_alignment_k);
            for (std::size_t field = 0; field != fields; ++field) {
                ustore_docs_field_type_t const type = columns.types[field];
                auto [bitmaps, bitmaps_status] = util::arena_allocate<ustore_octet_t>(arena, 3 * bitmap_stride);
                if (bitmaps_status.failed()) return bitmaps_status;
                std::memset(bitmaps.data(), 0, bitmaps.size());
                auto [scalars, scalars_status] = util::arena_allocate<ustore_byte_t>(arena,
                                                                                     rows * field_size_bytes(type));
                if (scalars_status.failed()) return scalars_status;
                ustore_docs_column_view_t& column = batch.columns[field];
                column = {.validities = bitmaps.data(),
                          .conversions = bitmaps.data() + bitmap_stride,
                          .collisions = bitmaps.data() + 2 * bitmap_stride,
                          .scalars = scalars.data(),
                          .strings = {}};
                docs_column_room_t& room = rooms[task * fields + field];
                room = {.bitmaps = bitmaps.data(), .bitmap_stride = bitmap_stride, .scalars = scalars.data()};
                if (field_size_bytes(type)) continue;
                std::size_t const capacity = static_cast<std::size_t>(max_bytes[task]) +
                                             (whole(field) ? 0 : rows * (printed_number_length_limit_k + 1));
                if (util::status_t const status = carve_texts_(rows, capacity, room, column); status.failed())
                    return status;
            }
        }
        return {};
    }

    /** Carves the texts of one column of @p rows rows into @p room, @p capacity bytes and their
     *  offsets, as narrow as the bytes allow, which @p column views. */
    util::status_t carve_texts_(std::size_t rows, std::size_t capacity, docs_column_room_t& room,
                                ustore_docs_column_view_t& column) noexcept {
        auto [bytes, bytes_status] = util::arena_allocate<util::byte_t>(arena, capacity);
        if (bytes_status.failed()) return bytes_status;
        room.texts = bytes.data(), room.capacity = capacity;
        if (capacity > (std::numeric_limits<std::uint32_t>::max)()) {
            auto [offsets, offsets_status] = util::arena_allocate<std::uint64_t>(arena, rows + 1);
            if (offsets_status.failed()) return offsets_status;
            offsets[0] = 0;
            room.wide_offsets = offsets.data();
            column.strings = util::u64_tape(bytes.data(), offsets.data());
            return {};
        }
        auto [offsets, offsets_status] = util::arena_allocate<std::uint32_t>(arena, rows + 1);
        if (offsets_status.failed()) return offsets_status;
        offsets[0] = 0;
        room.narrow_offsets = offsets.data();
        column.strings = util::u32_tape(bytes.data(), offsets.data());
        return {};
    }

    /**
     *  @brief Keeps among one round's @p keys those the match admits: the cheap predicate first,
     *      then a filter over the documents read through @p read, then the expensive predicate.
     *  @param[in] read Called as <tt>read(picked, documents)</tt> when the match or a column reads
     *      the documents, reading those of the keys at the indices @p picked into a tape in
     *      @p scratch, in that order.
     *  @param[out] kept The indices of the kept keys, in their order.
     *  @param[out] document_of The index into @p documents of the document of every key read.
     */
    template <typename read_type_>
    util::status_t narrow(std::span<ustore_key_t const> keys, read_type_&& read, ustore_arena_t scratch,
                          candidates_t& kept, json_documents_t& documents,
                          std::span<std::uint32_t>& document_of) const noexcept {
        auto [indices, indices_status] = util::arena_allocate<std::uint32_t>(scratch, keys.size());
        if (indices_status.failed()) return indices_status;
        for (std::size_t index = 0; index != keys.size(); ++index) indices[index] = static_cast<std::uint32_t>(index);
        kept = {indices, {}, keys.size()};
        documents = {};
        if (match)
            if (util::status_t const asked = match->ask(ustore_predicate_cost_cheap_k, keys, kept, scratch);
                asked.failed())
                return asked;
        if (kept.count && (!columns.fields.empty() || reads_values())) {
            auto [of, of_status] = util::arena_allocate<std::uint32_t>(scratch, keys.size());
            if (of_status.failed()) return of_status;
            document_of = of;
            for (std::size_t index = 0; index != kept.count; ++index)
                document_of[kept.indices[index]] = static_cast<std::uint32_t>(index);
            if (util::status_t const status = read(std::span<std::uint32_t const> {indices.data(), kept.count},
                                                   documents);
                status.failed())
                return status;
            if (reads_values())
                if (util::status_t const selected = match->select(strings_arg_t {documents.tape}, kept, scratch);
                    selected.failed())
                    return selected;
        }
        if (match)
            if (util::status_t const asked = match->ask(ustore_predicate_cost_expensive_k, keys, kept, scratch);
                asked.failed())
                return asked;
        return {};
    }

    /** Appends to batch @p task a row per key of @p keys that @p kept names, with the cells of
     *  every column, extracted from @p documents in one pass in @p scratch, where @p document_of
     *  finds each key's document. */
    template <util::strings_kit_concept strings_kit_type_>
    util::status_t append(std::size_t task, std::span<ustore_key_t const> keys, candidates_t const& kept,
                          json_documents_t const& documents, std::span<std::uint32_t const> document_of,
                          ustore_arena_t scratch) noexcept {
        docs_batch_t& batch = batches[task];
        std::size_t const count = kept.count;
        if (!count) return {};
        if (batch.rows + count > batch.keys.size())
            return {util::status_code_t::internal_k, "A task kept more rows than its runs"};
        std::size_t const first_row = batch.rows;
        for (std::size_t index = 0; index != count; ++index) batch.keys[first_row + index] = keys[kept.indices[index]];

        // Every field but the whole documents comes out of one extraction pass
        std::size_t const fields = columns.fields.size();
        std::size_t extracted_count = 0;
        for (std::size_t field = 0; field != fields; ++field) extracted_count += !whole(field);
        auto [extracted, extracted_status] = util::arena_allocate<std::string_view>(scratch, extracted_count);
        if (extracted_status.failed()) return extracted_status;
        for (std::size_t field = 0, at = 0; field != fields; ++field)
            if (!whole(field)) extracted[at++] = columns.fields[field];
        auto const document_of_row = [&](std::size_t row) noexcept { return document_of[kept.indices[row]]; };
        json_cells_t values;
        if (extracted_count) {
            auto const cell_of = [&](std::size_t cell) noexcept {
                return field_cell_t {document_of_row(cell % count), cell / count};
            };
            if (util::status_t const status = extract_fields<strings_kit_type_>(
                    documents, std::span<std::string_view const> {extracted}, count * extracted_count, cell_of, strings,
                    scratch, values);
                status.failed())
                return status;
        }

        printed_number_buffer_t print_buffer;
        for (std::size_t field = 0, extracted_index = 0; field != fields; ++field) {
            ustore_docs_field_type_t const type = columns.types[field];
            docs_column_room_t& room = rooms[task * fields + field];
            std::size_t const width = field_size_bytes(type);
            if (width) std::memset(room.scalars + first_row * width, 0, count * width);
            std::size_t const terminator = type == ustore_docs_field_type_string_k;
            for (std::size_t index = 0; index != count; ++index) {
                std::size_t const row = first_row + index;
                std::string_view text;
                if (whole(field)) text = documents[document_of_row(index)];
                conversion_t const conversion = whole(field)
                                                    ? (text.empty() ? conversion_t::missing_k : conversion_t::exact_k)
                                                    : gather_cell(type, values[extracted_index * count + index],
                                                                  room.scalars, row, print_buffer, text, scratch);
                std::size_t const slot = row / util::bits_in_byte_k;
                ustore_octet_t const mask = static_cast<ustore_octet_t>(1u << (row % util::bits_in_byte_k));
                std::size_t const stride = room.bitmap_stride;
                record_conversion(conversion, mask, room.bitmaps[slot], room.bitmaps[stride + slot],
                                  room.bitmaps[2 * stride + slot]);
                if (width) continue;
                if (!is_valid(conversion)) text = {};
                if (room.used + text.size() + terminator > room.capacity)
                    return {util::status_code_t::capacity_exhausted_k, "A folded document outgrew its runs"};
                if (!text.empty()) std::memcpy(room.texts + room.used, text.data(), text.size());
                room.used += text.size();
                if (terminator) room.texts[room.used++] = util::byte_t {0};
                if (room.wide_offsets) room.wide_offsets[row + 1] = room.used;
                else room.narrow_offsets[row + 1] = static_cast<std::uint32_t>(room.used);
            }
            extracted_index += !whole(field);
        }
        batch.rows += count;
        return {};
    }

    /** Rows every batch holds together. */
    std::size_t rows() const noexcept {
        std::size_t total = 0;
        for (docs_batch_t const& batch : batches) total += batch.rows;
        return total;
    }

    /** Hands the batches holding rows out through @p output and @p count, in task order. */
    void finish(ustore_docs_batch_view_t const** output, ustore_size_t* count) const noexcept {
        std::size_t written = 0;
        for (docs_batch_t const& batch : batches)
            if (batch.rows) views[written++] = {batch.rows, batch.keys.data(), batch.columns};
        *output = written ? views.data() : nullptr;
        *count = written;
    }

    /** Fills one batch on the calling thread from the @p listed keys, narrowing them and reading
     *  the documents of those left through @p read, as @c narrow reads. */
    template <util::strings_kit_concept strings_kit_type_, typename read_type_>
    util::status_t fill_one(std::span<ustore_key_t const> listed, read_type_&& read, ustore_arena_t scratch) noexcept {
        candidates_t kept;
        json_documents_t documents;
        std::span<std::uint32_t> document_of;
        if (util::status_t const status = narrow(listed, read, scratch, kept, documents, document_of); status.failed())
            return status;
        std::uint64_t bytes = 0;
        for (std::size_t index = 0; documents.count && index != kept.count; ++index)
            bytes += documents[document_of[kept.indices[index]]].size();
        std::uint64_t const rows = kept.count;
        if (util::status_t const status = carve({&rows, 1}, {&bytes, 1}, scratch); status.failed()) return status;
        return append<strings_kit_type_>(0, listed, kept, documents, document_of, scratch);
    }
};

/** Fills @p batches, where it is not null, with the one batch of the one task of a cursor's pull @p
 *  arguments on the calling thread, listing its keys through @p scan and reading the whole
 *  documents of those the match lets through @p read, the verbs of one engine called as
 *  <tt>scan(listing, view)</tt> and <tt>read(reading, view)</tt>. */
template <util::strings_kit_concept strings_kit_type_, typename scan_type_, typename read_type_>
util::status_t fill_one_batch(ustore_docs_scan_t const& arguments, docs_batches_t& batches, scan_type_&& scan,
                              read_type_&& read, view_t const& view) noexcept {
    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* keys = nullptr;
    ustore_docs_scan_t listing = arguments;
    listing.offsets = &offsets, listing.keys = &keys, listing.arena = view.scratch;
    if (util::status_t const status = scan(listing, view); status.failed()) return status;
    std::span<ustore_key_t const> const listed {keys, offsets ? static_cast<std::size_t>(offsets[1]) : 0};
    util::strided_iterator<ustore_collection_id_t const> const collections = util::strided(arguments.collections);
    auto read_documents = [&](std::span<std::uint32_t const> picked, json_documents_t& documents) noexcept {
        auto [picked_keys, picked_status] = util::arena_allocate<ustore_key_t>(view.scratch, picked.size());
        if (picked_status.failed()) return picked_status;
        for (std::size_t index = 0; index != picked.size(); ++index) picked_keys[index] = listed[picked[index]];
        documents = {};
        documents.count = picked.size();
        ustore_docs_read_t const reading {.database = arguments.database,
                                          .count = picked.size(),
                                          .collections = {&collections[0], 0},
                                          .keys = {picked_keys.data(), sizeof(ustore_key_t)},
                                          .presences = &documents.presences,
                                          .values = &documents.tape,
                                          .arena = view.scratch};
        return read(reading, view);
    };
    if (util::status_t const status = batches.fill_one<strings_kit_type_>(listed, read_documents, view.scratch);
        status.failed())
        return status;
    // A listing short of what it wanted reached the range's end, as did one ending at the last key
    bool const descending = arguments.direction == ustore_scan_descending_k;
    ustore_key_t const& edge = descending ? util::smallest_key_k : util::largest_key_k;
    if (listed.size() < arguments.wanted || listed.empty() || listed.back() == edge) batches.resume = exhausted_t {};
    else batches.resume = descending ? util::key_before(listed.back()) : util::key_after(listed.back());
    return {};
}

#pragma endregion Cursor Batches

#pragma region Documents over Blobs

/** The documents of an engine keeping each as one whole value, read and written through @c values_verbs, the
 *  unchecked whole-value verbs of that one engine. */
struct documents_in_values_t {
    blobs_verbs<vendored_residency_t::host_k> const& values_verbs;
    ustore_database_t database;

    /** Reads the binary documents at @p places through @p view, appending to its scratch rather than rewinding it. */
    util::expected<json_documents_t> read(util::place_batch_t const& places, view_t const& view) const noexcept {
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
        if (util::status_t const status = values_verbs.read(read, view); status.failed()) return status;
        return documents;
    }

    /** Writes @p values whole at @p places. */
    util::status_t write(util::place_batch_t const& places, strings_arg_t const& values, ustore_durability_t durability,
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
template <blobs_verbs<vendored_residency_t::host_k> const& values_verbs_, auto resolve_>
util::status_t docs_write_over_blobs(ustore_docs_write_t const& arguments, strings_arg_t const& fields,
                                     strings_arg_t const& values, view_t const& view) noexcept {
    util::status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                             ustore_modality_docs_k, view);
    if (resolved.failed()) return resolved;
    return write_documents(documents_in_values_t {values_verbs_, arguments.database}, arguments, fields, values, view);
}

template <util::strings_kit_concept strings_kit_type_, blobs_verbs<vendored_residency_t::host_k> const& values_verbs_,
          auto resolve_>
util::status_t docs_read_over_blobs(ustore_docs_read_t const& arguments, strings_arg_t const& fields,
                                    view_t const& view) noexcept {
    if (!arguments.count) return {};
    util::status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
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

template <blobs_verbs<vendored_residency_t::host_k> const& values_verbs_, auto resolve_>
util::status_t docs_gist_over_blobs(ustore_docs_gist_t const& arguments, view_t const& view) noexcept {
    util::status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                             ustore_modality_docs_k, view);
    if (resolved.failed()) return resolved;
    return gist_documents(documents_in_values_t {values_verbs_, arguments.database}, arguments, view);
}

template <util::strings_kit_concept strings_kit_type_, blobs_verbs<vendored_residency_t::host_k> const& values_verbs_,
          auto resolve_>
util::status_t docs_gather_over_blobs(ustore_docs_gather_t const& arguments, strings_arg_t const& fields,
                                      view_t const& view) noexcept {
    util::status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                             ustore_modality_docs_k, view);
    if (resolved.failed()) return resolved;
    return gather_documents<strings_kit_type_>(documents_in_values_t {values_verbs_, arguments.database}, arguments,
                                               fields, view);
}

/** The docs scan of an engine keeping every document as one whole value, which lists keys alone, or
 *  fills the one batch of a cursor's pull serially when @p batches is not null. */
template <util::strings_kit_concept strings_kit_type_, blobs_verbs<vendored_residency_t::host_k> const& values_verbs_,
          auto resolve_>
util::status_t docs_scan_over_blobs(ustore_docs_scan_t const& arguments, docs_batches_t* batches,
                                    view_t const& view) noexcept {
    if (!batches) return scan_over_blobs<values_verbs_, resolve_, ustore_modality_docs_k>(arguments, view);
    return fill_one_batch<strings_kit_type_>(
        arguments, *batches,
        [](ustore_docs_scan_t const& listing, view_t const& listing_view) noexcept {
            return scan_over_blobs<values_verbs_, resolve_, ustore_modality_docs_k>(listing, listing_view);
        },
        [](ustore_docs_read_t const& reading, view_t const& reading_view) noexcept {
            return docs_read_over_blobs<strings_kit_type_, values_verbs_, resolve_>(reading, strings_arg_t {},
                                                                                    reading_view);
        },
        view);
}

#pragma endregion Documents over Blobs

} // namespace unum::ustore
