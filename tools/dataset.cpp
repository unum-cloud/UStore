/**
 *  @file tools/dataset.cpp
 *  @author Ash Vardanian
 *  @date April 16, 2023
 *  @brief Imports and exports documents and graph edges as NDJSON on yyjson and CSV on StringZilla.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int64_t`
#include <cstdio>  // `std::fopen`, `std::fwrite`, `std::fclose`
#include <cstdlib> // `std::free`
#include <cstring> // `std::strcmp`

#include <charconv>     // `std::from_chars`, `std::to_chars`
#include <limits>       // `std::numeric_limits`
#include <span>         // `std::span`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::errc`

#include <yyjson.h> // `yyjson_read`, `yyjson_mut_doc`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <stringzilla/stringzilla.hpp> // `sz::string_t`, `sz::string_view_t`

#include "ustore/blobs.h"   // `ustore_scan_t`, `ustore_read_t`
#include "ustore/docs.h"    // `ustore_docs_write_t`, `ustore_docs_read_t`
#include "ustore/graph.h"   // `ustore_graph_upsert_edges_t`
#include "ustore/types.hpp" // `sz`, `st`

#include "dataset.h"                  // `ustore_docs_import_t`, `ustore_graph_export_t`
#include "helpers/blocking_files.hpp" // `mapped_file_t`

namespace unum::ustore::tools {

#pragma region Files

/** The file formats the tools read and write, chosen by extension. */
enum class dataset_format_t {
    ndjson_k,
    csv_k,
    unknown_k,
};

/** The format @p path names through its extension. */
[[nodiscard]] dataset_format_t format_of(ustore_str_view_t path) noexcept {
    std::string_view const view {path};
    if (view.ends_with(".ndjson")) return dataset_format_t::ndjson_k;
    if (view.ends_with(".csv")) return dataset_format_t::csv_k;
    return dataset_format_t::unknown_k;
}

/** A file written through a buffer that is flushed whenever it reaches a byte threshold. */
class output_file_t {
    std::FILE* file_ = nullptr;
    std::size_t threshold_ = 0;
    sz::string_t buffer_;

  public:
    output_file_t() noexcept = default;
    output_file_t(output_file_t const&) = delete;
    output_file_t& operator=(output_file_t const&) = delete;
    ~output_file_t() noexcept {
        if (file_) { [[maybe_unused]] int const closed = std::fclose(file_); }
    }

    /** Creates or truncates @p path, reporting a failure into @p error. */
    void create(ustore_str_view_t path, std::size_t threshold, ustore_error_t* error) noexcept {
        file_ = std::fopen(path, "wb");
        threshold_ = threshold;
        if (!file_) *error = {ustore_error_io_k, "Failed to create the output file"};
    }

    /** Buffers @p text, writing the buffer out once it reaches the threshold; does nothing after an error. */
    void append(std::string_view text, ustore_error_t* error) noexcept {
        if (error->status) return;
        if (!buffer_.try_append(text.data(), text.size())) {
            *error = {ustore_error_out_of_memory_k, "Failed to buffer the output"};
            return;
        }
        if (buffer_.size() >= threshold_) flush(error);
    }

    /** Buffers @p key as the decimal digits of the integer it spells, or as quoted UUID text when it spells none. */
    void append_key(ustore_key_t key, ustore_error_t* error) noexcept {
        if (auto [integer, integer_status] = uuid_to_integer(key.uuid); integer_status.succeeded()) {
            char digits[24];
            std::to_chars_result const printed = std::to_chars(digits, digits + sizeof(digits), integer);
            append({digits, static_cast<std::size_t>(printed.ptr - digits)}, error);
            return;
        }
        char quoted[uuid_text_length_k + 2];
        quoted[0] = '"';
        uuid_to_text(key.uuid, std::span(quoted).subspan<1, uuid_text_length_k>());
        quoted[uuid_text_length_k + 1] = '"';
        append({quoted, sizeof(quoted)}, error);
    }

    /** Writes the whole buffer out and empties it, reporting a short or failed write into @p error. */
    void flush(ustore_error_t* error) noexcept {
        if (std::fwrite(buffer_.data(), 1, buffer_.size(), file_) != buffer_.size()) {
            *error = {ustore_error_io_k, "Failed to write the output file"};
            return;
        }
        buffer_.clear();
    }

    /** Flushes the buffer and closes the file, reporting either failure into @p error. */
    void finish(ustore_error_t* error) noexcept {
        if (!error->status) flush(error);
        int const closed = std::fclose(file_);
        file_ = nullptr;
        if (closed != 0 && !error->status) *error = {ustore_error_io_k, "Failed to close the output file"};
    }
};

/** Owns a private arena for a task that brought none, and frees it on destruction. */
class task_arena_t {
    ustore_arena_t private_arena_ = nullptr;
    ustore_arena_t* chosen_ = nullptr;

  public:
    explicit task_arena_t(ustore_arena_t* given) noexcept : chosen_(given ? given : &private_arena_) {}
    task_arena_t(task_arena_t const&) = delete;
    task_arena_t& operator=(task_arena_t const&) = delete;
    ~task_arena_t() noexcept {
        if (private_arena_) ustore_arena_free(private_arena_);
    }

    [[nodiscard]] ustore_arena_t* get() const noexcept { return chosen_; }
};

/** Drops a trailing carriage return, so CRLF files read as LF files. */
[[nodiscard]] std::string_view without_carriage_return(sz::string_view_t line) noexcept {
    std::string_view view = line;
    if (!view.empty() && view.back() == '\r') view.remove_suffix(1);
    return view;
}

#pragma endregion Files

#pragma region JSON

/** Owns a parsed yyjson document. */
struct json_document_t {
    yyjson_doc* handle = nullptr;

    explicit json_document_t(yyjson_doc* document) noexcept : handle(document) {}
    json_document_t(json_document_t const&) = delete;
    json_document_t& operator=(json_document_t const&) = delete;
    ~json_document_t() noexcept { yyjson_doc_free(handle); }
};

/** Owns a yyjson document under construction. */
struct json_builder_t {
    yyjson_mut_doc* handle = nullptr;

    explicit json_builder_t(yyjson_mut_doc* document) noexcept : handle(document) {}
    json_builder_t(json_builder_t const&) = delete;
    json_builder_t& operator=(json_builder_t const&) = delete;
    ~json_builder_t() noexcept { yyjson_mut_doc_free(handle); }
};

/** Owns the text yyjson serialized. */
struct json_text_t {
    char* handle = nullptr;

    explicit json_text_t(char* text) noexcept : handle(text) {}
    json_text_t(json_text_t const&) = delete;
    json_text_t& operator=(json_text_t const&) = delete;
    ~json_text_t() noexcept { std::free(handle); }
};

/** Field names as the C ABI passes them: a strided array of C strings. */
struct field_list_t {
    ustore_str_view_t const* begin = nullptr;
    ustore_size_t stride = 0;
    ustore_size_t count = 0;

    [[nodiscard]] ustore_str_view_t operator[](std::size_t index) const noexcept {
        return *reinterpret_cast<ustore_str_view_t const*>(reinterpret_cast<char const*>(begin) + index * stride);
    }
};

/** Resolves a field of @p object, as a JSON pointer when it starts with a slash and as a plain name otherwise. */
[[nodiscard]] static yyjson_val* lookup(yyjson_val* object, ustore_str_view_t field) noexcept {
    return field[0] == '/' ? yyjson_ptr_get(object, field) : yyjson_obj_get(object, field);
}

/** Reads the key @p field of @p object holds, a non-negative integer or UUID text, answering whether it held one. */
[[nodiscard]] bool read_key(yyjson_val* object, ustore_str_view_t field, ustore_key_t& key) noexcept {
    yyjson_val* const value = lookup(object, field);
    if (yyjson_is_uint(value)) {
        key = key_from_integer(yyjson_get_uint(value));
        return true;
    }
    if (!yyjson_is_str(value)) return false;
    auto [uuid, uuid_status] = uuid_from_text({yyjson_get_str(value), yyjson_get_len(value)});
    key.uuid = uuid;
    return uuid_status.succeeded();
}

/** Finds or adds the object member @p key of @p parent, answering null when a non-object already holds the key. */
[[nodiscard]] yyjson_mut_val* child_object(yyjson_mut_doc* builder, yyjson_mut_val* parent,
                                           std::string_view key) noexcept {
    yyjson_mut_val* const existing = yyjson_mut_obj_getn(parent, key.data(), key.size());
    if (existing) return yyjson_mut_is_obj(existing) ? existing : nullptr;
    yyjson_mut_val* const name = yyjson_mut_strncpy(builder, key.data(), key.size());
    yyjson_mut_val* const child = yyjson_mut_obj(builder);
    if (!name || !child || !yyjson_mut_obj_add(parent, name, child)) return nullptr;
    return child;
}

/** Copies @p value into @p root at the path @p field names, creating the objects between; a placed path stays. */
void place(yyjson_mut_doc* builder, yyjson_mut_val* root, ustore_str_view_t field, yyjson_val* value) noexcept {
    yyjson_mut_val* parent = root;
    std::string_view remaining {field};
    if (field[0] == '/') {
        remaining.remove_prefix(1);
        for (std::size_t slash = remaining.find('/'); slash != std::string_view::npos; slash = remaining.find('/')) {
            parent = child_object(builder, parent, {remaining.data(), slash});
            if (!parent) return;
            remaining.remove_prefix(slash + 1);
        }
    }
    if (yyjson_mut_obj_getn(parent, remaining.data(), remaining.size())) return;
    yyjson_mut_val* const name = yyjson_mut_strncpy(builder, remaining.data(), remaining.size());
    yyjson_mut_val* const copy = yyjson_val_mut_copy(builder, value);
    if (name && copy) { [[maybe_unused]] bool const added = yyjson_mut_obj_add(parent, name, copy); }
}

/** Serializes the root of @p builder into @p output. */
void append_json(yyjson_mut_doc* builder, sz::string_t& output, ustore_error_t* error) noexcept {
    std::size_t length = 0;
    json_text_t const text {yyjson_mut_write(builder, 0, &length)};
    if (!text.handle) {
        *error = {ustore_error_out_of_memory_k, "Failed to serialize the document"};
        return;
    }
    if (!output.try_append(text.handle, length)) *error = {ustore_error_out_of_memory_k, "Failed to buffer a document"};
}

/** Serializes the @p fields of @p document into @p output, nesting JSON pointers into objects and skipping missing
 * fields. */
void project(yyjson_val* document, field_list_t fields, sz::string_t& output, ustore_error_t* error) noexcept {
    json_builder_t const builder {yyjson_mut_doc_new(nullptr)};
    yyjson_mut_val* const root = builder.handle ? yyjson_mut_obj(builder.handle) : nullptr;
    if (!root) {
        *error = {ustore_error_out_of_memory_k, "Failed to allocate a JSON document"};
        return;
    }
    yyjson_mut_doc_set_root(builder.handle, root);
    for (std::size_t index = 0; index != fields.count; ++index) {
        yyjson_val* const value = lookup(document, fields[index]);
        if (value) place(builder.handle, root, fields[index], value);
    }
    append_json(builder.handle, output, error);
}

#pragma endregion JSON

#pragma region CSV

/** How a CSV field ended. */
enum class csv_boundary_t {
    next_field_k,
    end_of_record_k,
    malformed_k,
};

/** Walks CSV records field by field, unquoting quoted fields into a scratch string. */
class csv_reader_t {
    sz::string_view_t text_;
    std::size_t position_ = 0;
    sz::string_t unquoted_;

    /** Steps over the comma or the line break after a field and answers which one it was. */
    [[nodiscard]] csv_boundary_t consume_delimiter() noexcept {
        if (position_ >= text_.size()) return csv_boundary_t::end_of_record_k;
        char const delimiter = text_[position_];
        if (delimiter == ',') {
            ++position_;
            return csv_boundary_t::next_field_k;
        }
        if (delimiter != '\r' && delimiter != '\n') return csv_boundary_t::malformed_k;
        if (delimiter == '\r') ++position_;
        if (position_ < text_.size() && text_[position_] == '\n') ++position_;
        return csv_boundary_t::end_of_record_k;
    }

  public:
    explicit csv_reader_t(sz::string_view_t text) noexcept : text_(text) {}

    /** Skips empty lines and answers whether any record remains. */
    [[nodiscard]] bool has_record() noexcept {
        while (position_ < text_.size() && (text_[position_] == '\n' || text_[position_] == '\r')) ++position_;
        return position_ < text_.size();
    }

    /** Reads the next field into @p field, which stays valid until the next call. */
    [[nodiscard]] csv_boundary_t next(std::string_view& field) noexcept {
        char const* const data = text_.data();
        std::size_t const size = text_.size();
        if (position_ < size && data[position_] == '"') {
            unquoted_.clear();
            std::size_t cursor = position_ + 1;
            while (true) {
                std::size_t const quote = text_.find('"', cursor);
                if (quote >= size) return csv_boundary_t::malformed_k;
                if (!unquoted_.try_append(data + cursor, quote - cursor)) return csv_boundary_t::malformed_k;
                if (quote + 1 == size || data[quote + 1] != '"') {
                    position_ = quote + 1;
                    break;
                }
                if (!unquoted_.try_push_back('"')) return csv_boundary_t::malformed_k;
                cursor = quote + 2;
            }
            field = unquoted_;
            return consume_delimiter();
        }
        std::size_t end = text_.find_first_of(sz::string_view_t {",\r\n"}, position_);
        if (end > size) end = size;
        field = {data + position_, end - position_};
        position_ = end;
        return consume_delimiter();
    }
};

/** Whether an import keeps a CSV column. */
enum class column_use_t : bool {
    skipped_k,
    kept_k,
};

/** One header column, whose name lives in a shared names buffer. */
struct csv_column_t {
    std::size_t offset;
    std::size_t length;
    column_use_t use;
};

/** Reads the header record into @p names and @p columns, keeping the columns @p fields names, or all without fields. */
void read_header(csv_reader_t& reader, field_list_t fields, sz::string_t& names,
                 st::basic_vector<csv_column_t>& columns, ustore_error_t* error) noexcept {
    if (!reader.has_record()) {
        *error = {ustore_error_malformed_k, "The CSV file has no header"};
        return;
    }
    for (csv_boundary_t boundary = csv_boundary_t::next_field_k; boundary == csv_boundary_t::next_field_k;) {
        std::string_view name;
        boundary = reader.next(name);
        if (boundary == csv_boundary_t::malformed_k) {
            *error = {ustore_error_malformed_k, "Malformed CSV header"};
            return;
        }
        column_use_t use = fields.count == 0 ? column_use_t::kept_k : column_use_t::skipped_k;
        for (std::size_t index = 0; index != fields.count && use == column_use_t::skipped_k; ++index) {
            std::string_view field {fields[index]};
            if (field.starts_with('/')) field.remove_prefix(1);
            if (field == name) use = column_use_t::kept_k;
        }
        csv_column_t const column {names.size(), name.size(), use};
        if (!names.try_append(name.data(), name.size()) || st::failed(columns.push_back(csv_column_t {column}))) {
            *error = {ustore_error_out_of_memory_k, "Failed to allocate the CSV header"};
            return;
        }
    }
}

/** Converts a CSV field to JSON: empty as null, then booleans, integers and reals, and text otherwise. */
[[nodiscard]] yyjson_mut_val* csv_value(yyjson_mut_doc* builder, std::string_view field) noexcept {
    if (field.empty()) return yyjson_mut_null(builder);
    if (field == "true") return yyjson_mut_bool(builder, true);
    if (field == "false") return yyjson_mut_bool(builder, false);
    char const* const end = field.data() + field.size();
    std::int64_t integer = 0;
    std::from_chars_result const as_integer = std::from_chars(field.data(), end, integer);
    if (as_integer.ec == std::errc {} && as_integer.ptr == end) return yyjson_mut_sint(builder, integer);
    double real = 0;
    std::from_chars_result const as_real = std::from_chars(field.data(), end, real);
    if (as_real.ec == std::errc {} && as_real.ptr == end) return yyjson_mut_real(builder, real);
    return yyjson_mut_strncpy(builder, field.data(), field.size());
}

/** Parses a CSV field holding a non-negative integer or UUID text into @p key, answering whether it held one. */
[[nodiscard]] bool csv_key(std::string_view field, ustore_key_t& key) noexcept {
    auto [uuid, uuid_status] = uuid_from_text(field);
    key.uuid = uuid;
    return uuid_status.succeeded();
}

/** Answers the index of the column named @p field, ignoring a leading slash, or the column count when absent. */
[[nodiscard]] std::size_t column_index(sz::string_t const& names, st::basic_vector<csv_column_t> const& columns,
                                       ustore_str_view_t field) noexcept {
    std::string_view name {field};
    if (name.starts_with('/')) name.remove_prefix(1);
    std::size_t index = 0;
    for (; index != columns.size(); ++index)
        if (std::string_view {names.data() + columns[index].offset, columns[index].length} == name) break;
    return index;
}

/** Writes @p text as one quoted CSV field, doubling the quotes inside. */
void append_quoted(output_file_t& output, sz::string_view_t text, ustore_error_t* error) noexcept {
    output.append("\"", error);
    std::string_view separator;
    for (sz::string_view_t segment : sz::split(text, "\"")) {
        output.append(separator, error);
        output.append(segment, error);
        separator = "\"\"";
    }
    output.append("\"", error);
}

#pragma endregion CSV

#pragma region Validation

/** Rejects a partly filled @p fields, and one holding a null name; an empty list keeps whole rows. */
void validate_fields(field_list_t fields, ustore_error_t* error) noexcept {
    if (fields.count == 0 && !fields.begin) return;
    if (fields.count == 0) {
        *error = {ustore_error_invalid_argument_k, "Fields count must be set along with fields"};
        return;
    }
    if (!fields.begin) {
        *error = {ustore_error_invalid_argument_k, "Fields must be set along with their count"};
        return;
    }
    if (fields.stride == 0) {
        *error = {ustore_error_invalid_argument_k, "Fields stride must be set along with fields"};
        return;
    }
    for (std::size_t index = 0; index != fields.count; ++index) {
        if (!fields[index]) {
            *error = {ustore_error_invalid_argument_k, "Fields must not be null"};
            return;
        }
    }
}

/** Answers whether an exported column name is a letter or underscore followed by letters, digits, spaces, dashes or
 * underscores. */
[[nodiscard]] bool is_column_name(ustore_str_view_t name) noexcept {
    auto const is_letter = [](char character) noexcept {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || character == '_';
    };
    if (!is_letter(name[0])) return false;
    for (char const* cursor = name + 1; *cursor; ++cursor) {
        char const character = *cursor;
        if (!is_letter(character) && !(character >= '0' && character <= '9') && character != ' ' && character != '-')
            return false;
    }
    return true;
}

#pragma endregion Validation

#pragma region Documents

/** One staged document, whose text lives in the batch at @c offset until the batch resolves @c value. */
struct staged_document_t {
    std::size_t offset;
    ustore_bytes_cptr_t value;
    ustore_length_t length;
};

/** Documents staged for one batched upsert. */
class document_batch_t {
    sz::string_t text_;
    st::basic_vector<staged_document_t> documents_;

  public:

    /** The buffer every document is appended to. */
    [[nodiscard]] sz::string_t& text() noexcept { return text_; }

    /** Bytes the staged documents and the text appended after them occupy. */
    [[nodiscard]] std::size_t bytes() const noexcept { return text_.size(); }

    /** Stages the text appended since the previous document as one more document. */
    void commit(ustore_error_t* error) noexcept {
        std::size_t const start = documents_.size() == 0 ? 0 : documents_.back().offset + documents_.back().length;
        auto const length = static_cast<ustore_length_t>(text_.size() - start);
        if (st::failed(documents_.push_back(staged_document_t {start, nullptr, length})))
            *error = {ustore_error_out_of_memory_k, "Failed to stage a document"};
    }

    /** Upserts every staged document as one batch and empties the batch. */
    void flush(ustore_docs_import_t const& task, ustore_arena_t* arena) noexcept {
        if (documents_.size() == 0) return;
        for (staged_document_t& document : documents_)
            document.value = reinterpret_cast<ustore_bytes_cptr_t>(text_.data() + document.offset);

        ustore_docs_write_t write {};
        write.db = task.db;
        write.error = task.error;
        write.arena = arena;
        write.options = task.options;
        write.tasks_count = documents_.size();
        write.type = ustore_doc_field_json_k;
        write.modification = ustore_doc_modify_upsert_k;
        write.collections = &task.collection;
        write.lengths = &documents_.data()->length;
        write.lengths_stride = sizeof(staged_document_t);
        write.values = &documents_.data()->value;
        write.values_stride = sizeof(staged_document_t);
        write.id_field = task.id_field;
        ustore_docs_write(&write);

        text_.clear();
        documents_.clear();
    }
};

/** Imports one document per NDJSON line, whole or projected onto @c task.fields. */
void import_ndjson_documents(ustore_docs_import_t const& task, sz::string_view_t text, ustore_arena_t* arena) noexcept {
    field_list_t const fields {task.fields, task.fields_stride, task.fields_count};
    document_batch_t batch;
    for (sz::string_view_t line : sz::split(text, "\n")) {
        std::string_view const row = without_carriage_return(line);
        if (row.empty()) continue;

        if (fields.count == 0) {
            if (!batch.text().try_append(row.data(), row.size()))
                *task.error = {ustore_error_out_of_memory_k, "Failed to buffer a document"};
        }
        else {
            json_document_t const document {yyjson_read(row.data(), row.size(), 0)};
            yyjson_val* const root = yyjson_doc_get_root(document.handle);
            if (!yyjson_is_obj(root)) {
                *task.error = {ustore_error_malformed_k, "The input file holds a line that is not a JSON object"};
                return;
            }
            project(root, fields, batch.text(), task.error);
        }
        if (task.error->status) return;

        batch.commit(task.error);
        if (task.error->status) return;
        if (batch.bytes() >= task.max_batch_size) batch.flush(task, arena);
        if (task.error->status) return;
    }
    batch.flush(task, arena);
}

/** Imports one document per CSV record, keeping the columns the header marked kept. */
void import_csv_documents(ustore_docs_import_t const& task, sz::string_view_t text, ustore_arena_t* arena) noexcept {
    field_list_t const fields {task.fields, task.fields_stride, task.fields_count};
    csv_reader_t reader {text};
    sz::string_t names;
    st::basic_vector<csv_column_t> columns;
    read_header(reader, fields, names, columns, task.error);
    if (task.error->status) return;

    document_batch_t batch;
    while (reader.has_record()) {
        json_builder_t const builder {yyjson_mut_doc_new(nullptr)};
        yyjson_mut_val* const root = builder.handle ? yyjson_mut_obj(builder.handle) : nullptr;
        if (!root) {
            *task.error = {ustore_error_out_of_memory_k, "Failed to allocate a JSON document"};
            return;
        }
        yyjson_mut_doc_set_root(builder.handle, root);

        csv_boundary_t boundary = csv_boundary_t::next_field_k;
        for (std::size_t index = 0; boundary == csv_boundary_t::next_field_k; ++index) {
            std::string_view field;
            boundary = reader.next(field);
            if (boundary == csv_boundary_t::malformed_k) {
                *task.error = {ustore_error_malformed_k, "Malformed CSV record"};
                return;
            }
            if (index >= columns.size() || columns[index].use == column_use_t::skipped_k) continue;
            yyjson_mut_val* const name = yyjson_mut_strncpy(builder.handle, names.data() + columns[index].offset,
                                                            columns[index].length);
            yyjson_mut_val* const value = csv_value(builder.handle, field);
            if (!name || !value || !yyjson_mut_obj_add(root, name, value)) {
                *task.error = {ustore_error_out_of_memory_k, "Failed to allocate a JSON document"};
                return;
            }
        }

        append_json(builder.handle, batch.text(), task.error);
        if (task.error->status) return;
        batch.commit(task.error);
        if (task.error->status) return;
        if (batch.bytes() >= task.max_batch_size) batch.flush(task, arena);
        if (task.error->status) return;
    }
    batch.flush(task, arena);
}

/**
 *  @brief Scans the collection of @p task in key order, handing every batch of keys to @p consume.
 *
 *  Stops at the end of the collection, at the first failure, or once a batch comes back short.
 *  @param[in] task The export task naming the database, the collection, the options and the error.
 *  @param[in] batch_size Keys one scan returns, which bounds the memory the batch occupies.
 *  @param[in] consume Receives each batch of keys and reports its own failures into the task's error.
 */
template <typename task_type_, typename consumer_type_>
void scan_keys(task_type_ const& task, ustore_length_t batch_size, consumer_type_&& consume) noexcept {
    task_arena_t const scan_arena {nullptr};
    ustore_key_t start = smallest_key_k;
    while (true) {
        ustore_length_t* counts = nullptr;
        ustore_key_t* keys = nullptr;
        ustore_scan_t scan {};
        scan.db = task.db;
        scan.error = task.error;
        scan.arena = scan_arena.get();
        scan.options = task.options;
        scan.tasks_count = 1;
        scan.collections = &task.collection;
        scan.start_keys = &start;
        scan.count_limits = &batch_size;
        scan.counts = &counts;
        scan.keys = &keys;
        ustore_scan(&scan);
        if (task.error->status || counts[0] == 0) return;

        ustore_length_t const count = counts[0];
        ustore_key_t const last = keys[count - 1];
        consume(std::span<ustore_key_t const>(keys, count));
        if (task.error->status || count < batch_size || last == largest_key_k) return;
        start = key_after(last);
    }
}

/** Keys a document export reads per batch, bounding the memory one read returns. */
constexpr ustore_length_t documents_per_read_k = 4096;

/** Clamps a byte budget to a positive count of @p item_size items. */
[[nodiscard]] ustore_length_t items_in(ustore_size_t bytes, std::size_t item_size) noexcept {
    ustore_size_t const items = bytes / item_size;
    if (items == 0) return 1;
    return items > std::numeric_limits<ustore_length_t>::max() ? std::numeric_limits<ustore_length_t>::max()
                                                               : static_cast<ustore_length_t>(items);
}

#pragma endregion Documents

#pragma region Graph

/** One edge in the layout the graph modality reads through strides. */
struct edge_row_t {
    ustore_key_t source;
    ustore_key_t target;
    ustore_key_t edge;
};

/** Upserts every staged edge as one batch and empties the batch. */
void upsert_edges(ustore_graph_import_t const& task, ustore_arena_t* arena,
                  st::basic_vector<edge_row_t>& edges) noexcept {
    if (edges.size() == 0) return;
    ustore_graph_upsert_edges_t upsert {};
    upsert.db = task.db;
    upsert.error = task.error;
    upsert.arena = arena;
    upsert.options = task.options;
    upsert.tasks_count = edges.size();
    upsert.collections = &task.collection;
    upsert.edges_ids = &edges.data()->edge;
    upsert.edges_stride = sizeof(edge_row_t);
    upsert.sources_ids = &edges.data()->source;
    upsert.sources_stride = sizeof(edge_row_t);
    upsert.targets_ids = &edges.data()->target;
    upsert.targets_stride = sizeof(edge_row_t);
    ustore_graph_upsert_edges(&upsert);
    edges.clear();
}

/** Stages @p edge and upserts the batch once it fills the byte budget of @p task. */
void stage_edge(ustore_graph_import_t const& task, ustore_arena_t* arena, st::basic_vector<edge_row_t>& edges,
                edge_row_t edge) noexcept {
    if (st::failed(edges.push_back(edge_row_t {edge}))) {
        *task.error = {ustore_error_out_of_memory_k, "Failed to stage an edge"};
        return;
    }
    if (edges.size() >= items_in(task.max_batch_size, sizeof(edge_row_t))) upsert_edges(task, arena, edges);
}

/** Imports one edge per NDJSON line, reading its vertices and edge from integer fields. */
void import_ndjson_edges(ustore_graph_import_t const& task, sz::string_view_t text, ustore_arena_t* arena) noexcept {
    st::basic_vector<edge_row_t> edges;
    for (sz::string_view_t line : sz::split(text, "\n")) {
        std::string_view const row = without_carriage_return(line);
        if (row.empty()) continue;

        json_document_t const document {yyjson_read(row.data(), row.size(), 0)};
        yyjson_val* const root = yyjson_doc_get_root(document.handle);
        edge_row_t edge {{}, {}, ustore_default_edge_id_k};
        if (!yyjson_is_obj(root) || !read_key(root, task.source_id_field, edge.source) ||
            !read_key(root, task.target_id_field, edge.target) ||
            (task.edge_id_field && !read_key(root, task.edge_id_field, edge.edge))) {
            *task.error = {ustore_error_malformed_k,
                           "The input file holds a line without integer or UUID vertex or edge fields"};
            return;
        }
        stage_edge(task, arena, edges, edge);
        if (task.error->status) return;
    }
    upsert_edges(task, arena, edges);
}

/** Imports one edge per CSV record, reading its vertices and edge from the columns the header names. */
void import_csv_edges(ustore_graph_import_t const& task, sz::string_view_t text, ustore_arena_t* arena) noexcept {
    csv_reader_t reader {text};
    sz::string_t names;
    st::basic_vector<csv_column_t> columns;
    read_header(reader, field_list_t {}, names, columns, task.error);
    if (task.error->status) return;

    std::size_t const source_column = column_index(names, columns, task.source_id_field);
    std::size_t const target_column = column_index(names, columns, task.target_id_field);
    std::size_t const edge_column = task.edge_id_field ? column_index(names, columns, task.edge_id_field)
                                                       : std::numeric_limits<std::size_t>::max();
    if (source_column == columns.size() || target_column == columns.size() || edge_column == columns.size()) {
        *task.error = {ustore_error_malformed_k, "The CSV header lacks a vertex or edge column"};
        return;
    }

    st::basic_vector<edge_row_t> edges;
    while (reader.has_record()) {
        edge_row_t edge {{}, {}, ustore_default_edge_id_k};
        std::size_t parsed = 0;
        csv_boundary_t boundary = csv_boundary_t::next_field_k;
        for (std::size_t index = 0; boundary == csv_boundary_t::next_field_k; ++index) {
            std::string_view field;
            boundary = reader.next(field);
            if (boundary == csv_boundary_t::malformed_k) {
                *task.error = {ustore_error_malformed_k, "Malformed CSV record"};
                return;
            }
            if (index == source_column) parsed += csv_key(field, edge.source);
            if (index == target_column) parsed += csv_key(field, edge.target);
            if (index == edge_column) parsed += csv_key(field, edge.edge);
        }
        if (parsed != (task.edge_id_field ? 3u : 2u)) {
            *task.error = {ustore_error_malformed_k,
                           "The CSV file holds a record without integer vertex or edge fields"};
            return;
        }
        stage_edge(task, arena, edges, edge);
        if (task.error->status) return;
    }
    upsert_edges(task, arena, edges);
}

#pragma endregion Graph

} // namespace unum::ustore::tools

#pragma region C Interface

void ustore_docs_import(ustore_docs_import_t* task_pointer) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;
    ustore_docs_import_t const& task = *task_pointer;

    if (!task.db) {
        *task.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    field_list_t const fields {task.fields, task.fields_stride, task.fields_count};
    validate_fields(fields, task.error);
    if (task.error->status) return;
    if (!task.id_field) {
        *task.error = {ustore_error_invalid_argument_k, "Id field must be initialized"};
        return;
    }
    if (fields.count != 0) {
        std::size_t index = 0;
        while (index != fields.count && std::strcmp(fields[index], task.id_field) != 0) ++index;
        if (index == fields.count) {
            *task.error = {ustore_error_invalid_argument_k, "Fields must contain the id field"};
            return;
        }
    }
    if (!task.path) {
        *task.error = {ustore_error_invalid_argument_k, "Null path specified"};
        return;
    }
    if (task.max_batch_size == 0) {
        *task.error = {ustore_error_invalid_argument_k, "Max batch size is 0"};
        return;
    }
    dataset_format_t const format = format_of(task.path);
    if (format == dataset_format_t::unknown_k) {
        *task.error = {ustore_error_unsupported_k, "Unsupported file format, expected .ndjson or .csv"};
        return;
    }

    auto [file, map_status] = mapped_file_t::map(task.path);
    if (map_status.failed()) {
        *task.error = map_status;
        return;
    }
    task_arena_t const arena {task.arena};
    if (format == dataset_format_t::ndjson_k) import_ndjson_documents(task, file.view(), arena.get());
    else import_csv_documents(task, file.view(), arena.get());
}

void ustore_docs_export(ustore_docs_export_t* task_pointer) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;
    ustore_docs_export_t const& task = *task_pointer;

    if (!task.db) {
        *task.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    field_list_t const fields {task.fields, task.fields_stride, task.fields_count};
    validate_fields(fields, task.error);
    if (task.error->status) return;
    if (!task.path) {
        *task.error = {ustore_error_invalid_argument_k, "Null path specified"};
        return;
    }
    if (task.max_batch_size == 0) {
        *task.error = {ustore_error_invalid_argument_k, "Max batch size is 0"};
        return;
    }
    dataset_format_t const format = format_of(task.path);
    if (format == dataset_format_t::unknown_k) {
        *task.error = {ustore_error_unsupported_k, "Unsupported file format, expected .ndjson or .csv"};
        return;
    }

    output_file_t output;
    output.create(task.path, task.max_batch_size, task.error);
    if (task.error->status) return;
    if (format == dataset_format_t::csv_k) output.append("_id,doc\n", task.error);

    task_arena_t const arena {task.arena};
    sz::string_t projected;
    auto const export_batch = [&](std::span<ustore_key_t const> keys) noexcept {
        ustore_length_t* offsets = nullptr;
        ustore_length_t* lengths = nullptr;
        ustore_bytes_ptr_t values = nullptr;
        ustore_docs_read_t read {};
        read.db = task.db;
        read.error = task.error;
        read.arena = arena.get();
        read.options = task.options;
        read.type = ustore_doc_field_json_k;
        read.tasks_count = keys.size();
        read.collections = &task.collection;
        read.keys = keys.data();
        read.keys_stride = sizeof(ustore_key_t);
        read.offsets = &offsets;
        read.lengths = &lengths;
        read.values = &values;
        ustore_docs_read(&read);
        if (task.error->status) return;

        for (std::size_t index = 0; index != keys.size() && !task.error->status; ++index) {
            if (lengths[index] == ustore_length_missing_k) continue;
            sz::string_view_t document {reinterpret_cast<char const*>(values) + offsets[index], lengths[index]};
            while (document.size() != 0 && document[document.size() - 1] == '\n')
                document = sz::string_view_t(document.data(), document.size() - 1);
            if (fields.count != 0) {
                json_document_t const parsed {yyjson_read(document.data(), document.size(), 0)};
                projected.clear();
                project(yyjson_doc_get_root(parsed.handle), fields, projected, task.error);
                document = projected.view();
            }
            if (format == dataset_format_t::ndjson_k) {
                output.append("{\"_id\":", task.error);
                output.append_key(keys[index], task.error);
                output.append(",\"doc\":", task.error);
                output.append(document, task.error);
                output.append("}\n", task.error);
            }
            else {
                output.append_key(keys[index], task.error);
                output.append(",", task.error);
                append_quoted(output, document, task.error);
                output.append("\n", task.error);
            }
        }
    };
    scan_keys(task, documents_per_read_k, export_batch);
    output.finish(task.error);
}

void ustore_graph_import(ustore_graph_import_t* task_pointer) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;
    ustore_graph_import_t const& task = *task_pointer;

    if (!task.db) {
        *task.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!task.source_id_field || !task.target_id_field) {
        *task.error = {ustore_error_invalid_argument_k, "Source and target id fields must be initialized"};
        return;
    }
    if (!task.path) {
        *task.error = {ustore_error_invalid_argument_k, "Null path specified"};
        return;
    }
    if (task.max_batch_size == 0) {
        *task.error = {ustore_error_invalid_argument_k, "Max batch size is 0"};
        return;
    }
    dataset_format_t const format = format_of(task.path);
    if (format == dataset_format_t::unknown_k) {
        *task.error = {ustore_error_unsupported_k, "Unsupported file format, expected .ndjson or .csv"};
        return;
    }

    auto [file, map_status] = mapped_file_t::map(task.path);
    if (map_status.failed()) {
        *task.error = map_status;
        return;
    }
    task_arena_t const arena {task.arena};
    if (format == dataset_format_t::ndjson_k) import_ndjson_edges(task, file.view(), arena.get());
    else import_csv_edges(task, file.view(), arena.get());
}

void ustore_graph_export(ustore_graph_export_t* task_pointer) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;
    ustore_graph_export_t const& task = *task_pointer;

    if (!task.db) {
        *task.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!task.source_id_field || !task.target_id_field) {
        *task.error = {ustore_error_invalid_argument_k, "Source and target id fields must be initialized"};
        return;
    }
    if (!is_column_name(task.source_id_field) || !is_column_name(task.target_id_field) ||
        (task.edge_id_field && !is_column_name(task.edge_id_field))) {
        *task.error = {ustore_error_invalid_argument_k,
                       "Exported field names start with A-Z, a-z or '_' and hold only A-Z, a-z, 0-9, ' ', '-' or '_'"};
        return;
    }
    if (!task.path) {
        *task.error = {ustore_error_invalid_argument_k, "Null path specified"};
        return;
    }
    if (task.max_batch_size == 0) {
        *task.error = {ustore_error_invalid_argument_k, "Max batch size is 0"};
        return;
    }
    dataset_format_t const format = format_of(task.path);
    if (format == dataset_format_t::unknown_k) {
        *task.error = {ustore_error_unsupported_k, "Unsupported file format, expected .ndjson or .csv"};
        return;
    }

    output_file_t output;
    output.create(task.path, task.max_batch_size, task.error);
    if (task.error->status) return;
    if (format == dataset_format_t::csv_k) {
        output.append(task.source_id_field, task.error);
        output.append(",", task.error);
        output.append(task.target_id_field, task.error);
        if (task.edge_id_field) {
            output.append(",", task.error);
            output.append(task.edge_id_field, task.error);
        }
        output.append("\n", task.error);
    }

    task_arena_t const arena {task.arena};
    ustore_vertex_role_t const role = ustore_vertex_source_k;
    auto const export_batch = [&](std::span<ustore_key_t const> vertices) noexcept {
        ustore_vertex_degree_t* degrees = nullptr;
        ustore_key_t* edges = nullptr;
        ustore_graph_find_edges_t find {};
        find.db = task.db;
        find.error = task.error;
        find.arena = arena.get();
        find.options = task.options;
        find.tasks_count = vertices.size();
        find.collections = &task.collection;
        find.vertices = vertices.data();
        find.vertices_stride = sizeof(ustore_key_t);
        find.roles = &role;
        find.degrees_per_vertex = &degrees;
        find.edges_per_vertex = &edges;
        ustore_graph_find_edges(&find);
        if (task.error->status) return;

        std::size_t triples = 0;
        for (std::size_t index = 0; index != vertices.size(); ++index)
            if (degrees[index] != ustore_vertex_degree_missing_k) triples += degrees[index];

        for (std::size_t triple = 0; triple != triples && !task.error->status; ++triple) {
            ustore_key_t const* const ids = edges + triple * 3;
            if (format == dataset_format_t::ndjson_k) {
                output.append("{\"", task.error);
                output.append(task.source_id_field, task.error);
                output.append("\":", task.error);
                output.append_key(ids[0], task.error);
                output.append(",\"", task.error);
                output.append(task.target_id_field, task.error);
                output.append("\":", task.error);
                output.append_key(ids[1], task.error);
                if (task.edge_id_field) {
                    output.append(",\"", task.error);
                    output.append(task.edge_id_field, task.error);
                    output.append("\":", task.error);
                    output.append_key(ids[2], task.error);
                }
                output.append("}\n", task.error);
            }
            else {
                output.append_key(ids[0], task.error);
                output.append(",", task.error);
                output.append_key(ids[1], task.error);
                if (task.edge_id_field) {
                    output.append(",", task.error);
                    output.append_key(ids[2], task.error);
                }
                output.append("\n", task.error);
            }
        }
    };
    scan_keys(task, items_in(task.max_batch_size, 3 * sizeof(ustore_key_t)), export_batch);
    output.finish(task.error);
}

#pragma endregion C Interface
