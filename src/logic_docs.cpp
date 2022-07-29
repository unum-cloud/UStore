/**
 * @file logic_docs.cpp
 * @author Ashot Vardanian
 *
 * @brief Document storage using "nlohmann/JSON" lib.
 * Sits on top of any @see "ukv.h"-compatible system.
 */

#include <vector>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <charconv> // `std::to_chars`
#include <cstdio>   // `std::snprintf`

#include <nlohmann/json.hpp>

// #include "ukv/docs.hpp"
#include "helpers.hpp"

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

using namespace unum::ukv;
using namespace unum;

using json_t = nlohmann::json;
using json_ptr_t = json_t::json_pointer;

constexpr ukv_format_t internal_format_k = ukv_format_msgpack_k;

static constexpr char const* true_k = "true";
static constexpr char const* false_k = "false";

// Both the variant and the vector wouldn't have `noexcept` default constructors
// if we didn't ingest @c `std::monostate` into the first and wrapped the second
// into an @c `std::optional`.
using heapy_field_t = std::variant<std::monostate, json_t::string_t, json_ptr_t>;
using heapy_fields_t = std::optional<std::vector<heapy_field_t>>;

/*********************************************************/
/*****************	 Primary Functions	  ****************/
/*********************************************************/

value_view_t to_view(char const* str, std::size_t len) noexcept {
    auto ptr = reinterpret_cast<byte_t const*>(str);
    return {ptr, ptr + len};
}

struct export_to_value_t final : public nlohmann::detail::output_adapter_protocol<char>,
                                 public std::enable_shared_from_this<export_to_value_t> {
    value_t* value_ptr = nullptr;

    export_to_value_t() = default;
    export_to_value_t(value_t& value) noexcept : value_ptr(&value) {}

    void write_character(char c) override { value_ptr->push_back(static_cast<byte_t>(c)); }
    void write_characters(char const* s, std::size_t length) override {
        auto ptr = reinterpret_cast<byte_t const*>(s);
        value_ptr->insert(value_ptr->size(), ptr, ptr + length);
    }

    template <typename at>
    void write_scalar(at scalar) {
        write_characters(reinterpret_cast<char const*>(&scalar), sizeof(at));
    }
};

json_t& lookup_field(json_t& json, ukv_str_view_t field, json_t& default_json) noexcept(false) {

    if (!field)
        return json;

    if (field[0] == '/') {
        // This libraries doesn't implement `find` for JSON-Pointers:
        json_ptr_t field_ptr {field};
        return json.contains(field_ptr) ? json.at(field_ptr) : default_json;
    }
    else {
        auto it = json.find(field);
        return it != json.end() ? it.value() : default_json;
    }
}

json_t parse_any(value_view_t bytes, ukv_format_t const c_format, ukv_error_t* c_error) noexcept {

    try {
        auto str = reinterpret_cast<char const*>(bytes.begin());
        auto len = bytes.size();
        switch (c_format) {
        case ukv_format_json_patch_k:
        case ukv_format_json_merge_patch_k:
        case ukv_format_json_k: return json_t::parse(str, str + len, nullptr, true, false);
        case ukv_format_msgpack_k: return json_t::from_msgpack(str, str + len, true, false);
        case ukv_format_bson_k: return json_t::from_bson(str, str + len, true, false);
        case ukv_format_cbor_k: return json_t::from_cbor(str, str + len, true, false);
        case ukv_format_ubjson_k: return json_t::from_ubjson(str, str + len, true, false);
        case ukv_format_binary_k:
            return json_t::binary({reinterpret_cast<std::int8_t const*>(bytes.begin()),
                                   reinterpret_cast<std::int8_t const*>(bytes.end())});
        default: *c_error = "Unsupported input format"; return {};
        }
    }
    catch (...) {
        *c_error = "Failed to parse the input document!";
        return {};
    }
}

/**
 * The JSON package provides a number of simple interfaces, which only work with simplest STL types
 * and always allocate the output objects, without the ability to reuse previously allocated memory,
 * including: `dump`, `to_msgpack`, `to_bson`, `to_cbor`, `to_ubjson`.
 * They have more flexible alternatives in the form of `nlohmann::detail::serializer`s,
 * that will accept our custom adapter. Unfortunately, they require a bogus shared pointer. WHY?!
 */
void dump_any(json_t const& json,
              ukv_format_t const c_format,
              std::shared_ptr<export_to_value_t> const& value,
              ukv_error_t* c_error) noexcept {

    using text_serializer_t = nlohmann::detail::serializer<json_t>;
    using binary_serializer_t = nlohmann::detail::binary_writer<json_t, char>;

    try {
        switch (c_format) {
        case ukv_format_json_k: return text_serializer_t(value, ' ').dump(json, false, false, 0, 0);
        case ukv_format_msgpack_k: return binary_serializer_t(value).write_msgpack(json);
        case ukv_format_bson_k: return binary_serializer_t(value).write_bson(json);
        case ukv_format_cbor_k: return binary_serializer_t(value).write_cbor(json);
        case ukv_format_ubjson_k: return binary_serializer_t(value).write_ubjson(json, true, true);
        case ukv_format_binary_k: {
            switch (json.type()) {
            case json_t::value_t::null: break;
            case json_t::value_t::discarded: break;
            case json_t::value_t::object: *c_error = "Can't export a nested dictionary in binary form!"; break;
            case json_t::value_t::array: *c_error = "Can't export a nested dictionary in binary form!"; break;
            case json_t::value_t::binary: {
                json_t::binary_t const& str = json.get_ref<json_t::binary_t const&>();
                value->write_characters(reinterpret_cast<char const*>(str.data()), str.size());
                break;
            }
            case json_t::value_t::string: {
                json_t::string_t const& str = json.get_ref<json_t::string_t const&>();
                value->write_characters(str.data(), str.size());
                break;
            }
            case json_t::value_t::boolean: value->write_character(json.get<bool>()); break;
            case json_t::value_t::number_integer: value->write_scalar(json.get<std::int64_t>()); break;
            case json_t::value_t::number_unsigned: value->write_scalar(json.get<std::uint64_t>()); break;
            case json_t::value_t::number_float: value->write_scalar(json.get<double>()); break;
            }
        }
        default: *c_error = "Unsupported output format"; break;
        }
    }
    catch (...) {
        *c_error = "Failed to serialize a document!";
    }
}

class serializing_tape_ref_t {
    stl_arena_t& arena_;
    std::shared_ptr<export_to_value_t> shared_exporter_;
    value_t single_doc_buffer_;

  public:
    serializing_tape_ref_t(stl_arena_t& a) noexcept : arena_(a) { arena_.growing_tape.clear(); }

    void push_back(json_t const& doc, ukv_format_t c_format, ukv_error_t* c_error) noexcept(false) {
        if (!shared_exporter_)
            shared_exporter_ = std::make_shared<export_to_value_t>();
        shared_exporter_->value_ptr = &single_doc_buffer_;

        single_doc_buffer_.clear();
        dump_any(doc, c_format, shared_exporter_, c_error);
        if (c_format == ukv_format_json_k)
            single_doc_buffer_.push_back(byte_t {0});

        arena_.growing_tape.push_back(single_doc_buffer_);
    }

    taped_values_view_t view() const noexcept { return arena_.growing_tape; }
};

template <typename callback_at>
read_tasks_soa_t const& read_unique_docs(ukv_t const c_db,
                                         ukv_txn_t const c_txn,
                                         read_tasks_soa_t const& tasks,
                                         strided_iterator_gt<ukv_str_view_t const> fields,
                                         ukv_options_t const c_options,
                                         stl_arena_t& arena,
                                         ukv_error_t* c_error,
                                         callback_at callback) noexcept {

    ukv_arena_t arena_ptr = &arena;
    ukv_val_len_t* binary_docs_lens = nullptr;
    ukv_val_ptr_t binary_docs_begin = nullptr;
    ukv_read(c_db,
             c_txn,
             tasks.count,
             tasks.cols.get(),
             tasks.cols.stride(),
             tasks.keys.get(),
             tasks.keys.stride(),
             c_options,
             &binary_docs_lens,
             &binary_docs_begin,
             &arena_ptr,
             c_error);

    auto binary_docs = taped_values_view_t(binary_docs_lens, binary_docs_begin, tasks.count);
    auto binary_docs_it = binary_docs.begin();

    for (ukv_size_t task_idx = 0; task_idx != tasks.count; ++task_idx, ++binary_docs_it) {
        value_view_t binary_doc = *binary_docs_it;
        json_t parsed = parse_any(binary_doc, internal_format_k, c_error);

        // This error is extremely unlikely, as we have previously accepted the data into the store.
        if (*c_error)
            return tasks;

        ukv_str_view_t field = fields[task_idx];
        callback(task_idx, field, parsed);
    }

    return tasks;
}

template <typename callback_at>
read_tasks_soa_t read_docs(ukv_t const c_db,
                           ukv_txn_t const c_txn,
                           read_tasks_soa_t const& tasks,
                           strided_iterator_gt<ukv_str_view_t const> fields,
                           ukv_options_t const c_options,
                           stl_arena_t& arena,
                           ukv_error_t* c_error,
                           callback_at callback) {

    // Handle the common case of requesting the non-colliding
    // all-ascending input sequences of document IDs received
    // during scans without the sort and extra memory.
    if (all_ascending(tasks.keys, tasks.count))
        return read_unique_docs(c_db, c_txn, tasks, fields, c_options, arena, c_error, callback);

    // If it's not one of the trivial consecutive lookups, we want
    // to sort & deduplicate the entries to minimize the random reads
    // from disk.
    prepare_memory(arena.updated_keys, tasks.count, c_error);
    if (*c_error)
        return tasks;
    for (ukv_size_t doc_idx = 0; doc_idx != tasks.count; ++doc_idx)
        arena.updated_keys[doc_idx] = tasks[doc_idx].location();
    sort_and_deduplicate(arena.updated_keys);

    // There is a chance, all the entries are unique.
    // In such case, let's free-up the memory.
    if (arena.updated_keys.size() == tasks.count) {
        arena.updated_keys.clear();
        return read_unique_docs(c_db, c_txn, tasks, fields, c_options, arena, c_error, callback);
    }

    // Otherwise, let's retrieve the sublist of unique docs,
    // which may be in a very different order from original.
    ukv_arena_t arena_ptr = &arena;
    ukv_val_len_t* binary_docs_lens = nullptr;
    ukv_val_ptr_t binary_docs_begin = nullptr;
    ukv_size_t unique_docs_count = static_cast<ukv_size_t>(arena.updated_keys.size());
    ukv_read(c_db,
             c_txn,
             unique_docs_count,
             &arena.updated_keys[0].collection,
             sizeof(col_key_t),
             &arena.updated_keys[0].key,
             sizeof(col_key_t),
             c_options,
             &binary_docs_lens,
             &binary_docs_begin,
             &arena_ptr,
             c_error);

    // We will later need to locate the data for every separate request.
    // Doing it in O(N) tape iterations every time is too slow.
    // Once we transform to inclusive sums, it will be O(1).
    //      inplace_inclusive_prefix_sum(binary_docs_lens, binary_docs_lens + binary_docs_count);
    // Alternatively we can compensate it with additional memory:
    std::optional<std::vector<json_t>> parsed_docs;
    try {
        parsed_docs = std::vector<json_t>(tasks.count);
    }
    catch (std::bad_alloc const&) {
        *c_error = "Out of memory!";
        return tasks;
    }

    // Parse all the unique documents
    auto binary_docs = taped_values_view_t(binary_docs_lens, binary_docs_begin, tasks.count);
    auto binary_docs_it = binary_docs.begin();
    for (ukv_size_t doc_idx = 0; doc_idx != unique_docs_count; ++doc_idx, ++binary_docs_it) {
        value_view_t binary_doc = *binary_docs_it;
        json_t& parsed = (*parsed_docs)[doc_idx];
        parsed = parse_any(binary_doc, internal_format_k, c_error);

        // This error is extremely unlikely, as we have previously accepted the data into the store.
        if (*c_error)
            return tasks;
    }

    // Join docs and fields with binary search
    for (ukv_size_t task_idx = 0; task_idx != tasks.count; ++task_idx) {
        auto task = tasks[task_idx];
        auto parsed_idx = offset_in_sorted(arena.updated_keys, task.location());
        json_t& parsed = (*parsed_docs)[parsed_idx];
        ukv_str_view_t field = fields[task_idx];
        callback(task_idx, field, parsed);
    }

    auto cnt = static_cast<ukv_size_t>(arena.updated_keys.size());
    auto sub_keys_range = strided_range_gt<col_key_t const>(arena.updated_keys);
    strided_range_gt<ukv_collection_t const> cols = sub_keys_range.members(&col_key_t::collection);
    strided_range_gt<ukv_key_t const> keys = sub_keys_range.members(&col_key_t::key);
    return {cols.begin(), keys.begin(), cnt};
}

void replace_docs( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,
    write_tasks_soa_t const& tasks,
    strided_iterator_gt<ukv_str_view_t const>,
    ukv_options_t const c_options,
    ukv_format_t const c_format,
    stl_arena_t& arena,
    ukv_error_t* c_error) noexcept {

    prepare_memory(arena.updated_vals, tasks.count, c_error);
    if (*c_error)
        return;

    std::shared_ptr<export_to_value_t> heapy_exporter;
    try {
        heapy_exporter = std::make_shared<export_to_value_t>();
    }
    catch (std::bad_alloc const&) {
        *c_error = "Out of memory!";
    }

    for (ukv_size_t doc_idx = 0; doc_idx != tasks.count; ++doc_idx) {
        auto task = tasks[doc_idx];
        auto& serialized = arena.updated_vals[doc_idx];
        if (task.is_deleted()) {
            serialized.reset();
            continue;
        }

        auto parsed = parse_any(task.view(), c_format, c_error);
        if (*c_error)
            return;
        if (parsed.is_discarded()) {
            *c_error = "Couldn't parse inputs";
            return;
        }

        heapy_exporter->value_ptr = &serialized;
        dump_any(parsed, internal_format_k, heapy_exporter, c_error);
        if (*c_error)
            return;
    }

    ukv_val_len_t offset = 0;
    ukv_arena_t arena_ptr = &arena;
    ukv_write( //
        c_db,
        c_txn,
        tasks.count,
        tasks.cols.get(),
        tasks.cols.stride(),
        tasks.keys.get(),
        tasks.keys.stride(),
        arena.updated_vals.front().member_ptr(),
        sizeof(value_t),
        &offset,
        0,
        arena.updated_vals.front().member_length(),
        sizeof(value_t),
        c_options,
        &arena_ptr,
        c_error);
}

void read_modify_write( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,
    write_tasks_soa_t const& tasks,
    strided_iterator_gt<ukv_str_view_t const> fields,
    ukv_options_t const c_options,
    ukv_format_t const c_format,
    stl_arena_t& arena,
    ukv_error_t* c_error) noexcept {

    prepare_memory(arena.updated_keys, tasks.count, c_error);
    if (*c_error)
        return;
    prepare_memory(arena.updated_vals, tasks.count, c_error);
    if (*c_error)
        return;

    serializing_tape_ref_t serializing_tape {arena};
    auto safe_callback = [&](ukv_size_t task_idx, ukv_str_view_t field, json_t& parsed) {
        try {
            json_t parsed_task = parse_any(tasks[task_idx].view(), c_format, c_error);
            if (*c_error)
                return;

            // Apply the patch
            json_t null_object;
            json_t& parsed_part = lookup_field(parsed, field, null_object);
            if (&parsed != &null_object) {
                switch (c_format) {
                case ukv_format_json_patch_k: parsed_part = parsed_part.patch(parsed_task); break;
                case ukv_format_json_merge_patch_k: parsed_part.merge_patch(parsed_task); break;
                default: parsed_part = parsed_task; break;
                }
            }
            else if (c_format != ukv_format_json_patch_k && c_format != ukv_format_json_merge_patch_k) {
                json_t::string_t heapy_field {field};
                parsed = parsed.flatten();
                parsed.emplace(heapy_field, parsed_task);
                parsed = parsed.unflatten();
            }

            // Save onto output tape
            serializing_tape.push_back(parsed_part, c_format, c_error);
            if (*c_error)
                return;
        }
        catch (std::bad_alloc const&) {
            *c_error = "Out of memory!";
        }
    };
    read_tasks_soa_t read_order = read_docs(c_db,
                                            c_txn,
                                            read_tasks_soa_t {tasks.cols, tasks.keys, tasks.count},
                                            fields,
                                            c_options,
                                            arena,
                                            c_error,
                                            safe_callback);

    // By now, the tape contains concatenated updates docs:
    ukv_size_t unique_docs_count = static_cast<ukv_size_t>(read_order.size());
    ukv_val_ptr_t binary_docs_begin = reinterpret_cast<ukv_val_ptr_t>(arena.growing_tape.contents().begin().get());
    ukv_arena_t arena_ptr = &arena;
    ukv_write(c_db,
              c_txn,
              unique_docs_count,
              read_order.cols.get(),
              read_order.cols.stride(),
              read_order.keys.get(),
              read_order.keys.stride(),
              &binary_docs_begin,
              0,
              arena.growing_tape.offsets().begin().get(),
              arena.growing_tape.offsets().stride(),
              arena.growing_tape.lengths().begin().get(),
              arena.growing_tape.lengths().stride(),
              c_options,
              &arena_ptr,
              c_error);
}

void parse_fields(strided_iterator_gt<ukv_str_view_t const> fields,
                  ukv_size_t n,
                  heapy_fields_t& fields_parsed,
                  ukv_error_t* c_error) noexcept {

    try {
        fields_parsed = std::vector<heapy_field_t>(n);
        ukv_str_view_t joined_fields_ptr = *fields;
        for (ukv_size_t field_idx = 0; field_idx != n; ++field_idx) {
            ukv_str_view_t field = fields.repeats() ? joined_fields_ptr : fields[field_idx];
            if (!field && (*c_error = "NULL JSON-Pointers are not allowed!"))
                return;

            heapy_field_t& field_parsed = (*fields_parsed)[field_idx];
            if (field[0] == '/')
                field_parsed = json_ptr_t {field};
            else
                field_parsed = std::string {field};

            joined_fields_ptr += std::strlen(field) + 1;
        }
    }
    catch (nlohmann::json::parse_error const&) {
        *c_error = "Inappropriate field path!";
    }
    catch (std::bad_alloc const&) {
        *c_error = "Out of memory!";
    }
}

void ukv_docs_write( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,
    ukv_size_t const c_tasks_count,

    ukv_collection_t const* c_cols,
    ukv_size_t const c_cols_stride,

    ukv_key_t const* c_keys,
    ukv_size_t const c_keys_stride,

    ukv_str_view_t const* c_fields,
    ukv_size_t const c_fields_stride,

    ukv_options_t const c_options,
    ukv_format_t const c_format,

    ukv_val_ptr_t const* c_vals,
    ukv_size_t const c_vals_stride,

    ukv_val_len_t const* c_offs,
    ukv_size_t const c_offs_stride,

    ukv_val_len_t const* c_lens,
    ukv_size_t const c_lens_stride,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    // If user wants the entire doc in the same format, as the one we use internally,
    // this request can be passed entirely to the underlying Key-Value store.
    strided_iterator_gt<ukv_str_view_t const> fields {c_fields, c_fields_stride};
    auto has_fields = fields && (!fields.repeats() || *fields);
    if (!has_fields && c_format == internal_format_k)
        return ukv_write(c_db,
                         c_txn,
                         c_tasks_count,
                         c_cols,
                         c_cols_stride,
                         c_keys,
                         c_keys_stride,
                         c_vals,
                         c_vals_stride,
                         c_offs,
                         c_offs_stride,
                         c_lens,
                         c_lens_stride,
                         c_options,
                         c_arena,
                         c_error);

    if (!c_db && (*c_error = "DataBase is NULL!"))
        return;

    stl_arena_t& arena = *cast_arena(c_arena, c_error);
    if (*c_error)
        return;

    strided_iterator_gt<ukv_collection_t const> cols {c_cols, c_cols_stride};
    strided_iterator_gt<ukv_key_t const> keys {c_keys, c_keys_stride};
    strided_iterator_gt<ukv_val_ptr_t const> vals {c_vals, c_vals_stride};
    strided_iterator_gt<ukv_val_len_t const> offs {c_offs, c_offs_stride};
    strided_iterator_gt<ukv_val_len_t const> lens {c_lens, c_lens_stride};
    write_tasks_soa_t tasks {cols, keys, vals, offs, lens, c_tasks_count};

    auto func = has_fields || c_format == ukv_format_json_patch_k || c_format == ukv_format_json_merge_patch_k
                    ? &read_modify_write
                    : &replace_docs;

    func(c_db, c_txn, tasks, fields, c_options, c_format, arena, c_error);
}

void ukv_docs_read( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,
    ukv_size_t const c_tasks_count,

    ukv_collection_t const* c_cols,
    ukv_size_t const c_cols_stride,

    ukv_key_t const* c_keys,
    ukv_size_t const c_keys_stride,

    ukv_str_view_t const* c_fields,
    ukv_size_t const c_fields_stride,

    ukv_options_t const c_options,
    ukv_format_t const c_format,

    ukv_val_len_t** c_binary_docs_lens,
    ukv_val_ptr_t* c_binary_docs_begin,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    // If user wants the entire doc in the same format, as the one we use internally,
    // this request can be passed entirely to the underlying Key-Value store.
    strided_iterator_gt<ukv_str_view_t const> fields {c_fields, c_fields_stride};
    auto has_fields = fields && (!fields.repeats() || *fields);
    if (!has_fields && c_format == internal_format_k)
        return ukv_read(c_db,
                        c_txn,
                        c_tasks_count,
                        c_cols,
                        c_cols_stride,
                        c_keys,
                        c_keys_stride,
                        c_options,
                        c_binary_docs_lens,
                        c_binary_docs_begin,
                        c_arena,
                        c_error);

    if (!c_db && (*c_error = "DataBase is NULL!"))
        return;

    stl_arena_t& arena = *cast_arena(c_arena, c_error);
    if (*c_error)
        return;

    strided_iterator_gt<ukv_collection_t const> cols {c_cols, c_cols_stride};
    strided_iterator_gt<ukv_key_t const> keys {c_keys, c_keys_stride};

    // Now, we need to parse all the entries to later export them into a target format.
    // Potentially sampling certain sub-fields again along the way.
    serializing_tape_ref_t serializing_tape {arena};
    json_t null_object;

    auto safe_callback = [&](ukv_size_t, ukv_str_view_t field, json_t& parsed) {
        try {
            json_t& parsed_part = lookup_field(parsed, field, null_object);
            serializing_tape.push_back(parsed_part, c_format, c_error);
            if (*c_error)
                return;
        }
        catch (std::bad_alloc const&) {
            *c_error = "Out of memory!";
        }
    };
    read_docs(c_db, c_txn, {cols, keys, c_tasks_count}, fields, c_options, arena, c_error, safe_callback);

    auto serialized_view = serializing_tape.view();
    *c_binary_docs_lens = serialized_view.lengths();
    *c_binary_docs_begin = serialized_view.contents();
}

void ukv_docs_gist( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,
    ukv_size_t const c_docs_count,

    ukv_collection_t const* c_cols,
    ukv_size_t const c_cols_stride,

    ukv_key_t const* c_keys,
    ukv_size_t const c_keys_stride,

    ukv_options_t const c_options,

    ukv_size_t* c_found_fields_count,
    ukv_str_view_t* c_found_fields,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    ukv_val_len_t* binary_docs_lens = nullptr;
    ukv_val_ptr_t binary_docs_begin = nullptr;
    ukv_read(c_db,
             c_txn,
             c_docs_count,
             c_cols,
             c_cols_stride,
             c_keys,
             c_keys_stride,
             c_options,
             &binary_docs_lens,
             &binary_docs_begin,
             c_arena,
             c_error);
    if (*c_error)
        return;

    stl_arena_t& arena = *cast_arena(c_arena, c_error);
    if (*c_error)
        return;

    strided_iterator_gt<ukv_collection_t const> cols {c_cols, c_cols_stride};
    strided_iterator_gt<ukv_key_t const> keys {c_keys, c_keys_stride};

    taped_values_view_t binary_docs {binary_docs_lens, binary_docs_begin, c_docs_count};
    tape_iterator_t binary_docs_it = binary_docs.begin();

    // Export all the elements into a heap-allocated hash-set, keeping only unique entries
    std::optional<std::unordered_set<std::string>> paths;
    try {
        paths = std::unordered_set<std::string> {};
        for (ukv_size_t doc_idx = 0; doc_idx != c_docs_count; ++doc_idx, ++binary_docs_it) {
            value_view_t binary_doc = *binary_docs_it;
            json_t parsed = parse_any(binary_doc, internal_format_k, c_error);
            if (*c_error)
                return;
            json_t parsed_flat = parsed.flatten();
            paths->reserve(paths->size() + parsed_flat.size());
            for (auto& pair : parsed_flat.items())
                paths->emplace(pair.key());
        }
    }
    catch (std::bad_alloc const&) {
        *c_error = "Out of memory!";
        return;
    }

    // Estimate the final memory consumption on-tape
    std::size_t total_length = 0;
    for (auto const& path : *paths)
        total_length += path.size();
    total_length += paths->size();

    // Reserve memory
    auto tape = prepare_memory(arena.unpacked_tape, total_length, c_error);
    if (*c_error)
        return;

    // Export on to the tape
    *c_found_fields_count = static_cast<ukv_size_t>(paths->size());
    *c_found_fields = reinterpret_cast<ukv_str_view_t>(tape);
    for (auto const& path : *paths)
        std::memcpy(std::exchange(tape, tape + path.size() + 1), path.c_str(), path.size() + 1);
}

void ukv_docs_gather_scalars( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,
    ukv_size_t const c_docs_count,
    ukv_size_t const c_fields_count,

    ukv_collection_t const* c_cols,
    ukv_size_t const c_cols_stride,

    ukv_key_t const* c_keys,
    ukv_size_t const c_keys_stride,

    ukv_str_view_t const* c_fields,
    ukv_size_t const c_fields_stride,

    ukv_type_t const* c_types,
    ukv_size_t const c_types_stride,

    ukv_options_t const c_options,

    ukv_val_ptr_t c_result_bitmap_valid,
    ukv_val_ptr_t c_result_bitmap_converted,
    ukv_val_ptr_t c_result_bitmap_collision,
    ukv_val_ptr_t c_result_scalars,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    // Validate the input arguments
    strided_iterator_gt<ukv_type_t const> types {c_types, c_types_stride};
    for (ukv_size_t field_idx = 0; field_idx != c_fields_count; ++field_idx) {
        ukv_type_t type = types[field_idx];
        switch (type) {
        case ukv_type_bool_k: break;
        case ukv_type_i64_k: break;
        case ukv_type_f64_k: break;
        case ukv_type_uuid_k: break;
        default: *c_error = "Only scalar fields are allowed!"; return;
        }
    }

    // Retrieve the entire documents before we can sample internal fields
    ukv_val_len_t* binary_docs_lens = nullptr;
    ukv_val_ptr_t binary_docs_begin = nullptr;
    ukv_read(c_db,
             c_txn,
             c_docs_count,
             c_cols,
             c_cols_stride,
             c_keys,
             c_keys_stride,
             c_options,
             &binary_docs_lens,
             &binary_docs_begin,
             c_arena,
             c_error);
    if (*c_error)
        return;

    strided_iterator_gt<ukv_collection_t const> cols {c_cols, c_cols_stride};
    strided_iterator_gt<ukv_key_t const> keys {c_keys, c_keys_stride};
    strided_iterator_gt<ukv_str_view_t const> fields {c_fields, c_fields_stride};

    taped_values_view_t binary_docs {binary_docs_lens, binary_docs_begin, c_docs_count};
    tape_iterator_t binary_docs_it = binary_docs.begin();

    // If those pointers were not provided, we can reuse the validity bitmap
    // It will allow us to avoid extra checks later.
    // ! Still, in every sequence of updates, validity is the last bit to be set,
    // ! to avoid overwriting.
    if (!c_result_bitmap_converted)
        c_result_bitmap_converted = c_result_bitmap_valid;
    if (!c_result_bitmap_collision)
        c_result_bitmap_collision = c_result_bitmap_valid;

    // Parse all the field names
    heapy_fields_t heapy_fields;
    parse_fields(fields, c_fields_count, heapy_fields, c_error);
    if (*c_error)
        return;

    json_t const null_object;

    // Go though all the documents extracting and type-checking the relevant parts
    for (ukv_size_t doc_idx = 0; doc_idx != c_docs_count; ++doc_idx, ++binary_docs_it) {
        value_view_t binary_doc = *binary_docs_it;
        json_t parsed = parse_any(binary_doc, internal_format_k, c_error);
        if (*c_error)
            return;

        auto column_bitmap_valid = reinterpret_cast<std::uint8_t*>(c_result_bitmap_valid);
        auto column_bitmap_converted = reinterpret_cast<std::uint8_t*>(c_result_bitmap_converted);
        auto column_bitmap_collision = reinterpret_cast<std::uint8_t*>(c_result_bitmap_collision);
        auto column_scalars = reinterpret_cast<std::uint8_t*>(c_result_scalars);

        for (ukv_size_t field_idx = 0; field_idx != c_fields_count; ++field_idx) {

            // Find this field within document
            ukv_type_t type = types[field_idx];
            heapy_field_t const& name_or_path = (*heapy_fields)[field_idx];
            json_t::iterator found_value_it = parsed.end();
            json_t const& found_value =
                name_or_path.index() //
                    ?
                    // This libraries doesn't implement `find` for JSON-Pointers:
                    (parsed.contains(std::get<2>(name_or_path)) //
                         ? parsed.at(std::get<2>(name_or_path))
                         : null_object)
                    // But with simple names we can query members with iterators:
                    : ((found_value_it = parsed.find(std::get<1>(name_or_path))) != parsed.end() //
                           ? found_value_it.value()
                           : null_object);

            // Resolve output addresses
            std::size_t bytes_per_scalar;
            switch (type) {
            case ukv_type_bool_k: bytes_per_scalar = 1; break;
            case ukv_type_i64_k: bytes_per_scalar = 8; break;
            case ukv_type_f64_k: bytes_per_scalar = 8; break;
            case ukv_type_uuid_k: bytes_per_scalar = 16; break;
            default: bytes_per_scalar = 0; break;
            }

            // Bitmaps are indexed from the last bit within every byte
            // https://arrow.apache.org/docs/format/Columnar.html#validity-bitmaps
            std::uint8_t mask_bitmap = static_cast<std::uint8_t>(1 << (doc_idx % CHAR_BIT));
            std::uint8_t* byte_bitmap_valid = column_bitmap_valid + doc_idx / CHAR_BIT;
            std::uint8_t* byte_bitmap_converted = column_bitmap_converted + doc_idx / CHAR_BIT;
            std::uint8_t* byte_bitmap_collision = column_bitmap_collision + doc_idx / CHAR_BIT;
            std::uint8_t* byte_scalars = column_scalars + doc_idx * bytes_per_scalar;

            // Export the types
            switch (type) {

                // Exporting booleans
            case ukv_type_bool_k:
                switch (found_value.type()) {
                case json_t::value_t::null:
                    *byte_bitmap_converted &= ~mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid &= ~mask_bitmap;
                    break;
                case json_t::value_t::object:
                case json_t::value_t::array:
                case json_t::value_t::string: // TODO
                case json_t::value_t::binary: // TODO
                case json_t::value_t::discarded:
                    *byte_bitmap_converted &= ~mask_bitmap;
                    *byte_bitmap_collision |= mask_bitmap;
                    *byte_bitmap_valid &= ~mask_bitmap;
                    break;
                case json_t::value_t::boolean:
                    *byte_scalars = found_value.get<bool>();
                    *byte_bitmap_converted &= ~mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid |= mask_bitmap;
                    break;
                case json_t::value_t::number_integer:
                    *byte_scalars = found_value.get<std::int64_t>() != 0;
                    *byte_bitmap_converted |= mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid |= mask_bitmap;
                    break;
                case json_t::value_t::number_unsigned:
                    *byte_scalars = found_value.get<std::uint64_t>() != 0;
                    *byte_bitmap_converted |= mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid |= mask_bitmap;
                    break;
                case json_t::value_t::number_float:
                    *byte_scalars = found_value.get<double>() != 0;
                    *byte_bitmap_converted |= mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid |= mask_bitmap;
                    break;
                }
                break;

                // Exporting integers
            case ukv_type_i64_k:
                switch (found_value.type()) {
                case json_t::value_t::null:
                    *byte_bitmap_converted &= ~mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid &= ~mask_bitmap;
                    break;
                case json_t::value_t::object:
                case json_t::value_t::array:
                case json_t::value_t::binary: // TODO
                case json_t::value_t::discarded:
                    *byte_bitmap_converted &= ~mask_bitmap;
                    *byte_bitmap_collision |= mask_bitmap;
                    *byte_bitmap_valid &= ~mask_bitmap;
                    break;
                case json_t::value_t::string: {
                    json_t::string_t const& str = found_value.get_ref<json_t::string_t const&>();
                    std::from_chars_result result = std::from_chars(str.data(),
                                                                    str.data() + str.size(),
                                                                    *reinterpret_cast<std::int64_t*>(byte_scalars));
                    *byte_bitmap_converted |= mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    bool entire_string_is_number = result.ec != std::errc() && result.ptr == str.data() + str.size();
                    if (entire_string_is_number)
                        *byte_bitmap_valid |= mask_bitmap;
                    else
                        *byte_bitmap_valid &= ~mask_bitmap;

                    break;
                }
                case json_t::value_t::boolean:
                    *reinterpret_cast<std::int64_t*>(byte_scalars) = found_value.get<bool>();
                    *byte_bitmap_converted &= ~mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid |= mask_bitmap;
                    break;
                case json_t::value_t::number_integer:
                    *reinterpret_cast<std::int64_t*>(byte_scalars) = found_value.get<std::int64_t>();
                    *byte_bitmap_converted &= ~mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid |= mask_bitmap;
                    break;
                case json_t::value_t::number_unsigned:
                    *reinterpret_cast<std::int64_t*>(byte_scalars) =
                        static_cast<std::int64_t>(found_value.get<std::uint64_t>());
                    *byte_bitmap_converted |= mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid |= mask_bitmap;
                    break;
                case json_t::value_t::number_float:
                    *reinterpret_cast<std::int64_t*>(byte_scalars) =
                        static_cast<std::int64_t>(found_value.get<double>());
                    *byte_bitmap_converted |= mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid |= mask_bitmap;
                    break;
                }
                break;

                // Exporting floats
            case ukv_type_f64_k:
                switch (found_value.type()) {
                case json_t::value_t::null:
                    *byte_bitmap_converted &= ~mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid &= ~mask_bitmap;
                    break;
                case json_t::value_t::object:
                case json_t::value_t::array:
                case json_t::value_t::binary: // TODO
                case json_t::value_t::discarded:
                    *byte_bitmap_converted &= ~mask_bitmap;
                    *byte_bitmap_collision |= mask_bitmap;
                    *byte_bitmap_valid &= ~mask_bitmap;
                    break;
                case json_t::value_t::string: {
                    json_t::string_t const& str = found_value.get_ref<json_t::string_t const&>();
                    char* end = nullptr;
                    *reinterpret_cast<double*>(byte_scalars) = std::strtod(str.data(), &end);
                    *byte_bitmap_converted |= mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;

                    bool entire_string_is_number = end == str.data() + str.size();
                    if (entire_string_is_number)
                        *byte_bitmap_valid |= mask_bitmap;
                    else
                        *byte_bitmap_valid &= ~mask_bitmap;
                    break;
                }
                case json_t::value_t::boolean:
                    *reinterpret_cast<double*>(byte_scalars) = static_cast<double>(found_value.get<bool>());
                    *byte_bitmap_converted &= ~mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid |= mask_bitmap;
                    break;
                case json_t::value_t::number_integer:
                    *reinterpret_cast<double*>(byte_scalars) = static_cast<double>(found_value.get<std::int64_t>());
                    *byte_bitmap_converted |= mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid |= mask_bitmap;
                    break;
                case json_t::value_t::number_unsigned:
                    *reinterpret_cast<double*>(byte_scalars) = static_cast<double>(found_value.get<std::uint64_t>());
                    *byte_bitmap_converted |= mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid |= mask_bitmap;
                    break;
                case json_t::value_t::number_float:
                    *reinterpret_cast<double*>(byte_scalars) = found_value.get<double>();
                    *byte_bitmap_converted &= ~mask_bitmap;
                    *byte_bitmap_collision &= ~mask_bitmap;
                    *byte_bitmap_valid |= mask_bitmap;
                    break;
                }
                break;

                // TODO: Exporting Unique Universal IDentifiers
            case ukv_type_uuid_k:
                *byte_bitmap_converted &= ~mask_bitmap;
                *byte_bitmap_collision &= ~mask_bitmap;
                *byte_bitmap_valid &= ~mask_bitmap;
                break;

            default: break;
            }

            // Jump forward to the next column
            column_bitmap_valid += c_docs_count / CHAR_BIT;
            column_bitmap_converted += c_docs_count / CHAR_BIT;
            column_bitmap_collision += c_docs_count / CHAR_BIT;
            column_scalars += c_docs_count * bytes_per_scalar;
        }
    }
}

void ukv_docs_gather_strings( //
    ukv_t const c_db,
    ukv_txn_t const c_txn,
    ukv_size_t const c_docs_count,
    ukv_size_t const c_fields_count,

    ukv_collection_t const* c_cols,
    ukv_size_t const c_cols_stride,

    ukv_key_t const* c_keys,
    ukv_size_t const c_keys_stride,

    ukv_str_view_t const* c_fields,
    ukv_size_t const c_fields_stride,

    ukv_options_t const c_options,

    ukv_val_len_t** c_binary_docs_lens,
    ukv_str_view_t* c_found_joined_strings,

    ukv_arena_t* c_arena,
    ukv_error_t* c_error) {

    // Retrieve the entire documents before we can sample internal fields
    ukv_val_len_t* binary_docs_lens = nullptr;
    ukv_val_ptr_t binary_docs_begin = nullptr;
    ukv_read(c_db,
             c_txn,
             c_docs_count,
             c_cols,
             c_cols_stride,
             c_keys,
             c_keys_stride,
             c_options,
             &binary_docs_lens,
             &binary_docs_begin,
             c_arena,
             c_error);
    if (*c_error)
        return;

    stl_arena_t& arena = *cast_arena(c_arena, c_error);
    if (*c_error)
        return;

    strided_iterator_gt<ukv_collection_t const> cols {c_cols, c_cols_stride};
    strided_iterator_gt<ukv_key_t const> keys {c_keys, c_keys_stride};
    strided_iterator_gt<ukv_str_view_t const> fields {c_fields, c_fields_stride};

    taped_values_view_t binary_docs {binary_docs_lens, binary_docs_begin, c_docs_count};
    tape_iterator_t binary_docs_it = binary_docs.begin();

    // Parse all the field names
    heapy_fields_t heapy_fields;
    parse_fields(fields, c_fields_count, heapy_fields, c_error);
    if (*c_error)
        return;

    json_t const null_object;
    constexpr std::size_t str_buffer_len_k = 64;
    alignas(str_buffer_len_k) char str_buffer[str_buffer_len_k];

    value_view_t const true_str = to_view(true_k, 5);
    value_view_t const false_str = to_view(false_k, 6);

    // Go though all the documents extracting and type-checking the relevant parts
    for (ukv_size_t doc_idx = 0; doc_idx != c_docs_count; ++doc_idx, ++binary_docs_it) {
        value_view_t binary_doc = *binary_docs_it;
        json_t parsed = parse_any(binary_doc, internal_format_k, c_error);
        if (*c_error)
            return;

        for (ukv_size_t field_idx = 0; field_idx != c_fields_count; ++field_idx) {

            // Find this field within document
            heapy_field_t const& name_or_path = (*heapy_fields)[field_idx];
            json_t::iterator found_value_it = parsed.end();
            json_t const& found_value =
                name_or_path.index() //
                    ?
                    // This libraries doesn't implement `find` for JSON-Pointers:
                    (parsed.contains(std::get<2>(name_or_path)) //
                         ? parsed.at(std::get<2>(name_or_path))
                         : null_object)
                    // But with simple names we can query members with iterators:
                    : ((found_value_it = parsed.find(std::get<1>(name_or_path))) != parsed.end() //
                           ? found_value_it.value()
                           : null_object);

            // Export the found value
            switch (found_value.type()) {
            case json_t::value_t::null:
            case json_t::value_t::discarded:
            case json_t::value_t::object:
            case json_t::value_t::array: arena.growing_tape.push_back({}); break;

            case json_t::value_t::binary: {
                json_t::binary_t const& str = found_value.get_ref<json_t::binary_t const&>();
                arena.growing_tape.push_back(to_view((char*)str.data(), str.size()));
                break;
            }
            case json_t::value_t::string: {
                json_t::string_t const& str = found_value.get_ref<json_t::string_t const&>();
                arena.growing_tape.push_back(to_view(str.c_str(), str.size() + 1));
                break;
            }
            case json_t::value_t::boolean:
                arena.growing_tape.push_back(found_value.get<bool>() ? true_str : false_str);
                break;
            case json_t::value_t::number_integer: {
                auto scalar = found_value.get<std::int64_t>();
                std::to_chars_result result = std::to_chars(str_buffer, str_buffer + str_buffer_len_k, scalar);
                bool fits_null_terminated = result.ec != std::errc() && result.ptr < str_buffer + str_buffer_len_k;
                if (fits_null_terminated) {
                    *result.ptr = '\0';
                    arena.growing_tape.push_back(to_view(str_buffer, result.ptr + 1 - str_buffer));
                }
                else
                    arena.growing_tape.push_back({});
                break;
            }
            case json_t::value_t::number_unsigned: {
                auto scalar = found_value.get<std::uint64_t>();
                std::to_chars_result result = std::to_chars(str_buffer, str_buffer + str_buffer_len_k, scalar);
                bool fits_null_terminated = result.ec != std::errc() && result.ptr < str_buffer + str_buffer_len_k;
                if (fits_null_terminated) {
                    *result.ptr = '\0';
                    arena.growing_tape.push_back(to_view(str_buffer, result.ptr + 1 - str_buffer));
                }
                else
                    arena.growing_tape.push_back({});
                break;
            }
            case json_t::value_t::number_float: {
                auto scalar = found_value.get<double>();
                // Parsing and dumping floating-point numbers is still not fully implemented in STL:
                //  std::to_chars_result result = std::to_chars(&str_buffer[0], str_buffer + str_buffer_len_k,
                //  scalar); bool fits_null_terminated = result.ec != std::errc() && result.ptr < str_buffer +
                //  str_buffer_len_k;
                // Using FMT would cause an extra dependency:
                //  auto end_ptr = fmt::format_to(str_buffer, "{}", scalar);
                //  bool fits_null_terminated = end_ptr < str_buffer + str_buffer_len_k;
                // If we use `std::snprintf`, the result would be NULL-terminted:
                auto result = std::snprintf(str_buffer, str_buffer_len_k, "%f", scalar);
                if (result >= 0)
                    arena.growing_tape.push_back(to_view(str_buffer, result));
                else
                    arena.growing_tape.push_back({});
                break;
            }
            }
        }
    }

    *c_binary_docs_lens = taped_values_view_t(arena.growing_tape).lengths();
    *c_found_joined_strings = reinterpret_cast<ukv_str_view_t>(taped_values_view_t(arena.growing_tape).contents());
}