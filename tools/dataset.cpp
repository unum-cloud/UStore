/**
 *  @file tools/dataset.cpp
 *  @author Ash Vardanian
 *  @date April 16, 2023
 *  @brief Imports and exports documents and graph edges as NDJSON on yyjson and CSV on StringZilla,
 *      moving columns in and out of the views of the C ABI; whole collections ride Apache Arrow IPC
 *      in <tt>tools/dataset_arrow.cpp</tt>.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int64_t`, `std::uint64_t`
#include <cstdio>  // `std::fopen`, `std::fwrite`, `std::fclose`
#include <cstdlib> // `std::free`

#include <algorithm>    // `std::find`
#include <charconv>     // `std::from_chars`, `std::to_chars`
#include <limits>       // `std::numeric_limits`
#include <span>         // `std::span`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::errc`

#include <yyjson.h> // `yyjson_read`, `yyjson_mut_doc`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <stringzilla/stringzilla.hpp> // `sz::string_t`, `sz::string_view_t`, `sz::split`

#include "ustore/docs.h"     // `ustore_docs_write_t`, `ustore_docs_read_t`, `ustore_docs_scan_t`
#include "ustore/graph.h"    // `ustore_graph_write_edges_t`, `ustore_graph_scan_vertices_t`
#include "ustore/ranges.hpp" // `bits_view_t`, `tape_string`, `u64_tape`
#include "ustore/types.hpp"  // `sz`, `st`

#include "ustore/utilities/blocking_files.hpp" // `mapped_file_t`

#include "dataset.h"   // `ustore_docs_import_t`, `ustore_graph_export_t`
#include "dataset.hpp" // `format_of`, `failed`, `copy_path`, `place`, `task_arena_t`

namespace unum::ustore::tools {

using namespace unum::ustore::utilities;

#pragma region Files

[[nodiscard]] ustore_dataset_format_t format_of(std::string_view path, ustore_dataset_format_t asked) noexcept {
    if (asked != ustore_dataset_format_automatic_k) return asked;
    if (path.ends_with(".ndjson")) return ustore_dataset_format_ndjson_k;
    if (path.ends_with(".csv")) return ustore_dataset_format_csv_k;
    if (path.ends_with(".arrows")) return ustore_dataset_format_arrow_stream_k;
    if (path.ends_with(".arrow") || path.ends_with(".feather")) return ustore_dataset_format_arrow_file_k;
    return ustore_dataset_format_automatic_k;
}

/** The text format @p path names, refusing every other into @p outcome, as the document and edge calls read only
 *  text. */
[[nodiscard]] ustore_dataset_format_t text_format_of(std::string_view path, ustore_outcome_t* outcome) noexcept {
    ustore_dataset_format_t const format = format_of(path, ustore_dataset_format_automatic_k);
    if (format != ustore_dataset_format_ndjson_k && format != ustore_dataset_format_csv_k)
        *outcome = {ustore_status_unsupported_k, "Unsupported file format, expected .ndjson or .csv"};
    return format;
}

[[nodiscard]] bool failed(ustore_outcome_t const* outcome) noexcept {
    return outcome->status != ustore_status_success_k;
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

    /** Creates or truncates @p path, reporting a failure into @p outcome. */
    void create(char const* path, std::size_t threshold, ustore_outcome_t* outcome) noexcept {
        file_ = std::fopen(path, "wb");
        threshold_ = threshold;
        if (!file_) *outcome = {ustore_status_io_k, "Failed to create the output file"};
    }

    /** Buffers @p text, writing the buffer out once it reaches the threshold; does nothing after a failure. */
    void append(std::string_view text, ustore_outcome_t* outcome) noexcept {
        if (failed(outcome)) return;
        if (buffer_.append(text.data(), text.size()) != sz::status_t::success_k) {
            *outcome = {ustore_status_out_of_memory_k, "Failed to buffer the output"};
            return;
        }
        if (buffer_.size() >= threshold_) flush(outcome);
    }

    /** Buffers @p key as the decimal digits of the integer it spells, or as quoted UUID text when it spells none. */
    void append_key(ustore_key_t key, ustore_outcome_t* outcome) noexcept {
        if (auto [integer, integer_status] = uuid_to_integer(key.uuid); integer_status.succeeded()) {
            char digits[24];
            std::to_chars_result const printed = std::to_chars(digits, digits + sizeof(digits), integer);
            append({digits, static_cast<std::size_t>(printed.ptr - digits)}, outcome);
            return;
        }
        char quoted[uuid_text_length_k + 2];
        quoted[0] = '"';
        uuid_to_text(key.uuid, std::span(quoted).subspan<1, uuid_text_length_k>());
        quoted[uuid_text_length_k + 1] = '"';
        append({quoted, sizeof(quoted)}, outcome);
    }

    /** Writes the whole buffer out and empties it, reporting a short or failed write into @p outcome. */
    void flush(ustore_outcome_t* outcome) noexcept {
        if (std::fwrite(buffer_.data(), 1, buffer_.size(), file_) != buffer_.size()) {
            *outcome = {ustore_status_io_k, "Failed to write the output file"};
            return;
        }
        buffer_.clear();
    }

    /** Flushes the buffer and closes the file, reporting either failure into @p outcome. */
    void finish(ustore_outcome_t* outcome) noexcept {
        if (!failed(outcome)) flush(outcome);
        int const closed = std::fclose(file_);
        file_ = nullptr;
        if (closed != 0 && !failed(outcome)) *outcome = {ustore_status_io_k, "Failed to close the output file"};
    }
};

/** Drops a trailing carriage return, so CRLF files read as LF files. */
[[nodiscard]] std::string_view without_carriage_return(sz::string_view_t line) noexcept {
    std::string_view view = line;
    if (!view.empty() && view.back() == '\r') view.remove_suffix(1);
    return view;
}

void copy_path(ustore_bytes_view_t path, sz::string_t& copy, ustore_outcome_t* outcome) noexcept {
    if (!path.length) {
        *outcome = {ustore_status_invalid_argument_k, "Empty path specified"};
        return;
    }
    if (copy.assign(sz::string_view_t {string_of(path)}) != sz::status_t::success_k)
        *outcome = {ustore_status_out_of_memory_k, "Failed to copy the path"};
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

/** Field names, resolved once from the strings the C ABI passes. */
using field_list_t = std::span<std::string_view const>;

/** Resolves a field of @p object, as a JSON pointer when it starts with a slash and as a plain name otherwise. */
[[nodiscard]] static yyjson_val* lookup(yyjson_val* object, std::string_view field) noexcept {
    return field.starts_with('/') ? yyjson_ptr_getn(object, field.data(), field.size())
                                  : yyjson_obj_getn(object, field.data(), field.size());
}

/** Reads the key @p field of @p object holds, a non-negative integer or UUID text, answering whether it held one. */
[[nodiscard]] bool read_key(yyjson_val* object, std::string_view field, ustore_key_t& key) noexcept {
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

void place(yyjson_mut_doc* builder, yyjson_mut_val* root, std::string_view field, yyjson_val* value) noexcept {
    yyjson_mut_val* parent = root;
    std::string_view remaining = field;
    if (field.starts_with('/')) {
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
void append_json(yyjson_mut_doc* builder, sz::string_t& output, ustore_outcome_t* outcome) noexcept {
    std::size_t length = 0;
    json_text_t const text {yyjson_mut_write(builder, 0, &length)};
    if (!text.handle) {
        *outcome = {ustore_status_out_of_memory_k, "Failed to serialize the document"};
        return;
    }
    if (output.append(text.handle, length) != sz::status_t::success_k)
        *outcome = {ustore_status_out_of_memory_k, "Failed to buffer a document"};
}

/** Serializes the @p fields of @p document into @p output, nesting JSON pointers into objects and skipping missing
 *  fields. */
void project(yyjson_val* document, field_list_t fields, sz::string_t& output, ustore_outcome_t* outcome) noexcept {
    json_builder_t const builder {yyjson_mut_doc_new(nullptr)};
    yyjson_mut_val* const root = builder.handle ? yyjson_mut_obj(builder.handle) : nullptr;
    if (!root) {
        *outcome = {ustore_status_out_of_memory_k, "Failed to allocate a JSON document"};
        return;
    }
    yyjson_mut_doc_set_root(builder.handle, root);
    for (std::string_view const field : fields) {
        yyjson_val* const value = lookup(document, field);
        if (value) place(builder.handle, root, field, value);
    }
    append_json(builder.handle, output, outcome);
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
                if (unquoted_.append(data + cursor, quote - cursor) != sz::status_t::success_k)
                    return csv_boundary_t::malformed_k;
                if (quote + 1 == size || data[quote + 1] != '"') {
                    position_ = quote + 1;
                    break;
                }
                if (unquoted_.push_back('"') != sz::status_t::success_k) return csv_boundary_t::malformed_k;
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
void read_header(csv_reader_t& reader, field_list_t fields, sz::string_t& names, st::vector<csv_column_t>& columns,
                 ustore_outcome_t* outcome) noexcept {
    if (!reader.has_record()) {
        *outcome = {ustore_status_malformed_k, "The CSV file has no header"};
        return;
    }
    for (csv_boundary_t boundary = csv_boundary_t::next_field_k; boundary == csv_boundary_t::next_field_k;) {
        std::string_view name;
        boundary = reader.next(name);
        if (boundary == csv_boundary_t::malformed_k) {
            *outcome = {ustore_status_malformed_k, "Malformed CSV header"};
            return;
        }
        column_use_t use = fields.empty() ? column_use_t::kept_k : column_use_t::skipped_k;
        for (std::string_view field : fields) {
            if (field.starts_with('/')) field.remove_prefix(1);
            if (field == name) use = column_use_t::kept_k;
        }
        csv_column_t const column {names.size(), name.size(), use};
        if (names.append(name.data(), name.size()) != sz::status_t::success_k ||
            st::failed(columns.push_back(csv_column_t {column}))) {
            *outcome = {ustore_status_out_of_memory_k, "Failed to allocate the CSV header"};
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

/** Answers the index of the column named @p name, ignoring a leading slash, or the column count when absent. */
[[nodiscard]] std::size_t column_index(sz::string_t const& names, st::vector<csv_column_t> const& columns,
                                       std::string_view name) noexcept {
    if (name.starts_with('/')) name.remove_prefix(1);
    std::size_t index = 0;
    for (; index != columns.size(); ++index)
        if (std::string_view {names.data() + columns[index].offset, columns[index].length} == name) break;
    return index;
}

/** Writes @p text as one quoted CSV field, doubling the quotes inside. */
void append_quoted(output_file_t& output, sz::string_view_t text, ustore_outcome_t* outcome) noexcept {
    output.append("\"", outcome);
    std::string_view separator;
    for (sz::string_view_t segment : sz::split(text, "\"")) {
        output.append(separator, outcome);
        output.append(segment, outcome);
        separator = "\"\"";
    }
    output.append("\"", outcome);
}

#pragma endregion CSV

#pragma region Validation

/**
 *  @brief Resolves the @p count names of @p strings into @p fields, where no names keep whole rows.
 *
 *  Rejects a partly filled list, an unknown layout and an empty name, and calls an accessor once per name.
 */
void resolve_fields(ustore_strings_view_t const& strings, ustore_size_t count, st::vector<std::string_view>& fields,
                    ustore_outcome_t* outcome) noexcept {
    bool const given = strings.layout != ustore_strings_u32_tape_k || strings.data.u32_tape.offsets;
    if (count == 0 && !given) return;
    if (count == 0) {
        *outcome = {ustore_status_invalid_argument_k, "Fields count must be set along with fields"};
        return;
    }
    if (strings.layout != ustore_strings_u32_tape_k && strings.layout != ustore_strings_u64_tape_k &&
        strings.layout != ustore_strings_accessor_k) {
        *outcome = {ustore_status_invalid_argument_k, "The fields come in an unknown layout"};
        return;
    }
    if (!given || (strings.layout == ustore_strings_u64_tape_k && !strings.data.u64_tape.offsets) ||
        (strings.layout == ustore_strings_accessor_k && !strings.data.accessor.get)) {
        *outcome = {ustore_status_invalid_argument_k, "Fields must be set along with their count"};
        return;
    }
    if (st::failed(fields.resize(count))) {
        *outcome = {ustore_status_out_of_memory_k, "Failed to allocate the fields"};
        return;
    }
    for (ustore_size_t index = 0; index != count; ++index) {
        ustore_bytes_view_t name {};
        if (strings.layout == ustore_strings_accessor_k)
            strings.data.accessor.get(strings.data.accessor.handle, &index, 1, &name);
        else name = tape_string(strings, index);
        if (!name.length) {
            *outcome = {ustore_status_invalid_argument_k, "Fields must not be empty"};
            return;
        }
        fields[index] = string_of(name);
    }
}

/** Answers whether an exported column name is a letter or underscore followed by letters, digits, spaces, dashes or
 *  underscores. */
[[nodiscard]] bool is_column_name(std::string_view name) noexcept {
    auto const is_letter = [](char character) noexcept {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || character == '_';
    };
    if (name.empty() || !is_letter(name.front())) return false;
    for (char const character : name.substr(1))
        if (!is_letter(character) && !(character >= '0' && character <= '9') && character != ' ' && character != '-')
            return false;
    return true;
}

#pragma endregion Validation

#pragma region Documents

/** Documents staged for one batched upsert, as a tape of 64-bit offsets over their text. */
class document_batch_t {
    sz::string_t text_;
    st::vector<std::uint64_t> offsets_;

  public:

    /** The buffer every document is appended to. */
    [[nodiscard]] sz::string_t& text() noexcept { return text_; }

    /** Bytes the staged documents and the text appended after them occupy. */
    [[nodiscard]] std::size_t bytes() const noexcept { return text_.size(); }

    /** Stages the text appended since the previous document as one more document. */
    void commit(ustore_outcome_t* outcome) noexcept {
        if ((offsets_.size() == 0 && st::failed(offsets_.push_back(std::uint64_t {0}))) ||
            st::failed(offsets_.push_back(std::uint64_t {text_.size()})))
            *outcome = {ustore_status_out_of_memory_k, "Failed to stage a document"};
    }

    /** Upserts every staged document as one batch and empties the batch. */
    void flush(ustore_docs_import_t const& task) noexcept {
        if (offsets_.size() == 0) return;
        ustore_docs_write_t write {};
        write.database = task.database;
        write.outcome = task.outcome;
        write.durability = task.durability;
        write.count = offsets_.size() - 1;
        write.type = ustore_docs_field_type_json_k;
        write.modification = ustore_docs_modify_upsert_k;
        write.collections = {&task.collection, 0};
        write.values = u64_tape(text_.data(), offsets_.data());
        write.id_field = task.id_field;
        ustore_docs_write(&write);

        text_.clear();
        offsets_.clear();
    }
};

/** Imports one document per NDJSON line, whole or projected onto @p fields. */
void import_ndjson_documents(ustore_docs_import_t const& task, field_list_t fields, sz::string_view_t text) noexcept {
    document_batch_t batch;
    for (sz::string_view_t line : sz::split(text, "\n")) {
        std::string_view const row = without_carriage_return(line);
        if (row.empty()) continue;

        if (fields.empty()) {
            if (batch.text().append(row.data(), row.size()) != sz::status_t::success_k)
                *task.outcome = {ustore_status_out_of_memory_k, "Failed to buffer a document"};
        }
        else {
            json_document_t const document {yyjson_read(row.data(), row.size(), 0)};
            yyjson_val* const root = yyjson_doc_get_root(document.handle);
            if (!yyjson_is_obj(root)) {
                *task.outcome = {ustore_status_malformed_k, "The input file holds a line that is not a JSON object"};
                return;
            }
            project(root, fields, batch.text(), task.outcome);
        }
        if (failed(task.outcome)) return;

        batch.commit(task.outcome);
        if (failed(task.outcome)) return;
        if (batch.bytes() >= task.max_batch_size) batch.flush(task);
        if (failed(task.outcome)) return;
    }
    batch.flush(task);
}

/** Imports one document per CSV record, keeping the columns the header marked kept. */
void import_csv_documents(ustore_docs_import_t const& task, field_list_t fields, sz::string_view_t text) noexcept {
    csv_reader_t reader {text};
    sz::string_t names;
    st::vector<csv_column_t> columns;
    read_header(reader, fields, names, columns, task.outcome);
    if (failed(task.outcome)) return;

    document_batch_t batch;
    while (reader.has_record()) {
        json_builder_t const builder {yyjson_mut_doc_new(nullptr)};
        yyjson_mut_val* const root = builder.handle ? yyjson_mut_obj(builder.handle) : nullptr;
        if (!root) {
            *task.outcome = {ustore_status_out_of_memory_k, "Failed to allocate a JSON document"};
            return;
        }
        yyjson_mut_doc_set_root(builder.handle, root);

        csv_boundary_t boundary = csv_boundary_t::next_field_k;
        for (std::size_t index = 0; boundary == csv_boundary_t::next_field_k; ++index) {
            std::string_view field;
            boundary = reader.next(field);
            if (boundary == csv_boundary_t::malformed_k) {
                *task.outcome = {ustore_status_malformed_k, "Malformed CSV record"};
                return;
            }
            if (index >= columns.size() || columns[index].use == column_use_t::skipped_k) continue;
            yyjson_mut_val* const name = yyjson_mut_strncpy(builder.handle, names.data() + columns[index].offset,
                                                            columns[index].length);
            yyjson_mut_val* const value = csv_value(builder.handle, field);
            if (!name || !value || !yyjson_mut_obj_add(root, name, value)) {
                *task.outcome = {ustore_status_out_of_memory_k, "Failed to allocate a JSON document"};
                return;
            }
        }

        append_json(builder.handle, batch.text(), task.outcome);
        if (failed(task.outcome)) return;
        batch.commit(task.outcome);
        if (failed(task.outcome)) return;
        if (batch.bytes() >= task.max_batch_size) batch.flush(task);
        if (failed(task.outcome)) return;
    }
    batch.flush(task);
}

/**
 *  @brief Scans the collection of @p task in key order through @p scan_, the scan of its modality, handing every batch
 *      of keys to @p consume.
 *
 *  The keys land in a private arena, since @p consume reads them into the arena of the task, which a call never takes
 *  inputs from. Stops at the end of the collection, at the first failure, or once a batch comes back short.
 *  @param[in] task The export task naming the database, the collection, the view and the outcome.
 *  @param[in] batch_size Keys one scan returns, which bounds the memory the batch occupies.
 *  @param[in] consume Receives each batch of keys and reports its own failures into the task's outcome.
 */
template <typename scan_type_, void (*scan_)(scan_type_*), typename task_type_, typename consumer_type_>
void scan_keys(task_type_ const& task, ustore_size_t batch_size, consumer_type_&& consume) noexcept {
    task_arena_t const keys_arena {nullptr, task.outcome};
    if (failed(task.outcome)) return;
    ustore_key_t start = smallest_key_k;
    while (true) {
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* page = nullptr;
        scan_type_ scan {};
        scan.database = task.database;
        scan.outcome = task.outcome;
        scan.view = task.view;
        scan.count = 1;
        scan.collections = {&task.collection, 0};
        scan.start_keys = {&start, 0};
        scan.offsets = &offsets;
        scan.keys = &page;
        scan.arena = keys_arena.get();
        scan.wanted = batch_size;
        scan_(&scan);
        if (failed(task.outcome) || !offsets || offsets[1] == offsets[0]) return;

        ustore_length_t const count = offsets[1] - offsets[0];
        ustore_key_t const* const keys = page + offsets[0];
        ustore_key_t const last = keys[count - 1];
        consume(std::span<ustore_key_t const>(keys, count));
        if (failed(task.outcome) || count < batch_size || last == largest_key_k) return;
        start = key_after(last);
    }
}

/** Keys a document export reads per batch, bounding the memory one read returns. */
constexpr ustore_size_t documents_per_read_k = 4096;

/** Clamps a byte budget to a positive count of @p item_size items. */
[[nodiscard]] ustore_size_t items_in(ustore_size_t bytes, std::size_t item_size) noexcept {
    ustore_size_t const items = bytes / item_size;
    return items ? items : 1;
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
void upsert_edges(ustore_graph_import_t const& task, st::vector<edge_row_t>& edges) noexcept {
    if (edges.size() == 0) return;
    ustore_graph_write_edges_t write {};
    write.database = task.database;
    write.outcome = task.outcome;
    write.durability = task.durability;
    write.count = edges.size();
    write.collections = {&task.collection, 0};
    write.edges = {&edges.data()->edge, sizeof(edge_row_t)};
    write.sources = {&edges.data()->source, sizeof(edge_row_t)};
    write.targets = {&edges.data()->target, sizeof(edge_row_t)};
    ustore_graph_write_edges(&write);
    edges.clear();
}

/** Stages @p edge and upserts the batch once it fills the byte budget of @p task. */
void stage_edge(ustore_graph_import_t const& task, st::vector<edge_row_t>& edges, edge_row_t edge) noexcept {
    if (st::failed(edges.push_back(edge_row_t {edge}))) {
        *task.outcome = {ustore_status_out_of_memory_k, "Failed to stage an edge"};
        return;
    }
    if (edges.size() >= items_in(task.max_batch_size, sizeof(edge_row_t))) upsert_edges(task, edges);
}

/** Imports one edge per NDJSON line, reading its vertices and edge from integer fields. */
void import_ndjson_edges(ustore_graph_import_t const& task, sz::string_view_t text) noexcept {
    st::vector<edge_row_t> edges;
    for (sz::string_view_t line : sz::split(text, "\n")) {
        std::string_view const row = without_carriage_return(line);
        if (row.empty()) continue;

        json_document_t const document {yyjson_read(row.data(), row.size(), 0)};
        yyjson_val* const root = yyjson_doc_get_root(document.handle);
        edge_row_t edge {{}, {}, ustore_edge_id_default_k};
        if (!yyjson_is_obj(root) || !read_key(root, string_of(task.source_id_field), edge.source) ||
            !read_key(root, string_of(task.target_id_field), edge.target) ||
            (task.edge_id_field.length && !read_key(root, string_of(task.edge_id_field), edge.edge))) {
            *task.outcome = {ustore_status_malformed_k,
                             "The input file holds a line without integer or UUID vertex or edge fields"};
            return;
        }
        stage_edge(task, edges, edge);
        if (failed(task.outcome)) return;
    }
    upsert_edges(task, edges);
}

/** Imports one edge per CSV record, reading its vertices and edge from the columns the header names. */
void import_csv_edges(ustore_graph_import_t const& task, sz::string_view_t text) noexcept {
    csv_reader_t reader {text};
    sz::string_t names;
    st::vector<csv_column_t> columns;
    read_header(reader, field_list_t {}, names, columns, task.outcome);
    if (failed(task.outcome)) return;

    std::size_t const source_column = column_index(names, columns, string_of(task.source_id_field));
    std::size_t const target_column = column_index(names, columns, string_of(task.target_id_field));
    std::size_t const edge_column = task.edge_id_field.length
                                        ? column_index(names, columns, string_of(task.edge_id_field))
                                        : (std::numeric_limits<std::size_t>::max)();
    if (source_column == columns.size() || target_column == columns.size() || edge_column == columns.size()) {
        *task.outcome = {ustore_status_malformed_k, "The CSV header lacks a vertex or edge column"};
        return;
    }

    st::vector<edge_row_t> edges;
    while (reader.has_record()) {
        edge_row_t edge {{}, {}, ustore_edge_id_default_k};
        std::size_t parsed = 0;
        csv_boundary_t boundary = csv_boundary_t::next_field_k;
        for (std::size_t index = 0; boundary == csv_boundary_t::next_field_k; ++index) {
            std::string_view field;
            boundary = reader.next(field);
            if (boundary == csv_boundary_t::malformed_k) {
                *task.outcome = {ustore_status_malformed_k, "Malformed CSV record"};
                return;
            }
            if (index == source_column) parsed += csv_key(field, edge.source);
            if (index == target_column) parsed += csv_key(field, edge.target);
            if (index == edge_column) parsed += csv_key(field, edge.edge);
        }
        if (parsed != (task.edge_id_field.length ? 3u : 2u)) {
            *task.outcome = {ustore_status_malformed_k,
                             "The CSV file holds a record without integer vertex or edge fields"};
            return;
        }
        stage_edge(task, edges, edge);
        if (failed(task.outcome)) return;
    }
    upsert_edges(task, edges);
}

#pragma endregion Graph

} // namespace unum::ustore::tools

#pragma region C Interface

void ustore_docs_import(ustore_docs_import_t* task_pointer) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;
    ustore_docs_import_t const& task = *task_pointer;

    if (!task.database) {
        *task.outcome = {ustore_status_uninitialized_k, "Database is uninitialized"};
        return;
    }
    st::vector<std::string_view> fields;
    resolve_fields(task.fields, task.fields_count, fields, task.outcome);
    if (failed(task.outcome)) return;
    std::string_view const id_field = string_of(task.id_field);
    if (id_field.empty()) {
        *task.outcome = {ustore_status_invalid_argument_k, "Id field must be initialized"};
        return;
    }
    if (fields.size() != 0 && std::find(fields.begin(), fields.end(), id_field) == fields.end()) {
        *task.outcome = {ustore_status_invalid_argument_k, "Fields must contain the id field"};
        return;
    }
    sz::string_t path;
    copy_path(task.path, path, task.outcome);
    if (failed(task.outcome)) return;
    if (task.max_batch_size == 0) {
        *task.outcome = {ustore_status_invalid_argument_k, "Max batch size is 0"};
        return;
    }
    ustore_dataset_format_t const format = text_format_of(path.view(), task.outcome);
    if (failed(task.outcome)) return;

    auto [file, map_status] = utilities::mapped_file_t::map(path.c_str());
    if (map_status.failed()) {
        *task.outcome = map_status;
        return;
    }
    if (format == ustore_dataset_format_ndjson_k) import_ndjson_documents(task, fields, file.view());
    else import_csv_documents(task, fields, file.view());
}

void ustore_docs_export(ustore_docs_export_t* task_pointer) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;
    ustore_docs_export_t const& task = *task_pointer;

    if (!task.database) {
        *task.outcome = {ustore_status_uninitialized_k, "Database is uninitialized"};
        return;
    }
    st::vector<std::string_view> fields;
    resolve_fields(task.fields, task.fields_count, fields, task.outcome);
    if (failed(task.outcome)) return;
    sz::string_t path;
    copy_path(task.path, path, task.outcome);
    if (failed(task.outcome)) return;
    if (task.max_batch_size == 0) {
        *task.outcome = {ustore_status_invalid_argument_k, "Max batch size is 0"};
        return;
    }
    ustore_dataset_format_t const format = text_format_of(path.view(), task.outcome);
    if (failed(task.outcome)) return;

    task_arena_t const arena {task.arena, task.outcome};
    if (failed(task.outcome)) return;
    output_file_t output;
    output.create(path.c_str(), task.max_batch_size, task.outcome);
    if (failed(task.outcome)) return;
    if (format == ustore_dataset_format_csv_k) output.append("_id,doc\n", task.outcome);

    sz::string_t projected;
    auto const export_batch = [&](std::span<ustore_key_t const> keys) noexcept {
        ustore_octet_t const* presences = nullptr;
        ustore_strings_view_t values {};
        ustore_docs_read_t read {};
        read.database = task.database;
        read.outcome = task.outcome;
        read.view = task.view;
        read.count = keys.size();
        read.collections = {&task.collection, 0};
        read.keys = {keys.data(), sizeof(ustore_key_t)};
        read.presences = &presences;
        read.values = &values;
        read.arena = arena.get();
        read.outputs_type = ustore_docs_field_type_json_k;
        ustore_docs_read(&read);
        if (failed(task.outcome)) return;

        bits_view_t const present {presences};
        for (std::size_t index = 0; index != keys.size() && !failed(task.outcome); ++index) {
            if (!present[index]) continue;
            sz::string_view_t document {string_of(tape_string(values, index))};
            while (document.size() != 0 && document[document.size() - 1] == '\n')
                document = sz::string_view_t(document.data(), document.size() - 1);
            if (fields.size() != 0) {
                json_document_t const parsed {yyjson_read(document.data(), document.size(), 0)};
                projected.clear();
                project(yyjson_doc_get_root(parsed.handle), fields, projected, task.outcome);
                document = projected.view();
            }
            if (format == ustore_dataset_format_ndjson_k) {
                output.append("{\"_id\":", task.outcome);
                output.append_key(keys[index], task.outcome);
                output.append(",\"doc\":", task.outcome);
                output.append(document, task.outcome);
                output.append("}\n", task.outcome);
            }
            else {
                output.append_key(keys[index], task.outcome);
                output.append(",", task.outcome);
                append_quoted(output, document, task.outcome);
                output.append("\n", task.outcome);
            }
        }
    };
    scan_keys<ustore_docs_scan_t, &ustore_docs_scan>(task, documents_per_read_k, export_batch);
    output.finish(task.outcome);
}

void ustore_graph_import(ustore_graph_import_t* task_pointer) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;
    ustore_graph_import_t const& task = *task_pointer;

    if (!task.database) {
        *task.outcome = {ustore_status_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!task.source_id_field.length || !task.target_id_field.length) {
        *task.outcome = {ustore_status_invalid_argument_k, "Source and target id fields must be initialized"};
        return;
    }
    sz::string_t path;
    copy_path(task.path, path, task.outcome);
    if (failed(task.outcome)) return;
    if (task.max_batch_size == 0) {
        *task.outcome = {ustore_status_invalid_argument_k, "Max batch size is 0"};
        return;
    }
    ustore_dataset_format_t const format = text_format_of(path.view(), task.outcome);
    if (failed(task.outcome)) return;

    auto [file, map_status] = utilities::mapped_file_t::map(path.c_str());
    if (map_status.failed()) {
        *task.outcome = map_status;
        return;
    }
    if (format == ustore_dataset_format_ndjson_k) import_ndjson_edges(task, file.view());
    else import_csv_edges(task, file.view());
}

void ustore_graph_export(ustore_graph_export_t* task_pointer) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;
    ustore_graph_export_t const& task = *task_pointer;

    if (!task.database) {
        *task.outcome = {ustore_status_uninitialized_k, "Database is uninitialized"};
        return;
    }
    std::string_view const source_id_field = string_of(task.source_id_field);
    std::string_view const target_id_field = string_of(task.target_id_field);
    std::string_view const edge_id_field = string_of(task.edge_id_field);
    if (source_id_field.empty() || target_id_field.empty()) {
        *task.outcome = {ustore_status_invalid_argument_k, "Source and target id fields must be initialized"};
        return;
    }
    if (!is_column_name(source_id_field) || !is_column_name(target_id_field) ||
        (!edge_id_field.empty() && !is_column_name(edge_id_field))) {
        *task.outcome = {
            ustore_status_invalid_argument_k,
            "Exported field names start with A-Z, a-z or '_' and hold only A-Z, a-z, 0-9, ' ', '-' or '_'"};
        return;
    }
    sz::string_t path;
    copy_path(task.path, path, task.outcome);
    if (failed(task.outcome)) return;
    if (task.max_batch_size == 0) {
        *task.outcome = {ustore_status_invalid_argument_k, "Max batch size is 0"};
        return;
    }
    ustore_dataset_format_t const format = text_format_of(path.view(), task.outcome);
    if (failed(task.outcome)) return;

    task_arena_t const arena {task.arena, task.outcome};
    if (failed(task.outcome)) return;
    output_file_t output;
    output.create(path.c_str(), task.max_batch_size, task.outcome);
    if (failed(task.outcome)) return;
    if (format == ustore_dataset_format_csv_k) {
        output.append(source_id_field, task.outcome);
        output.append(",", task.outcome);
        output.append(target_id_field, task.outcome);
        if (!edge_id_field.empty()) {
            output.append(",", task.outcome);
            output.append(edge_id_field, task.outcome);
        }
        output.append("\n", task.outcome);
    }

    ustore_size_t const batch_size = items_in(task.max_batch_size, 3 * sizeof(ustore_key_t));
    ustore_vertex_role_t const role = ustore_vertex_role_source_k;
    auto const export_batch = [&](std::span<ustore_key_t const> vertices) noexcept {
        ustore_vertex_degree_t const* degrees = nullptr;
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* edges = nullptr;
        ustore_graph_read_edges_t read {};
        read.database = task.database;
        read.outcome = task.outcome;
        read.view = task.view;
        read.count = vertices.size();
        read.collections = {&task.collection, 0};
        read.vertices = {vertices.data(), sizeof(ustore_key_t)};
        read.roles = {&role, 0};
        read.degrees_per_vertex = &degrees;
        read.edges_offsets = &offsets;
        read.edges_per_vertex = &edges;
        read.arena = arena.get();
        ustore_graph_read_edges(&read);
        if (failed(task.outcome)) return;

        std::size_t const triples = offsets ? offsets[vertices.size()] : 0;
        for (std::size_t triple = 0; triple != triples && !failed(task.outcome); ++triple) {
            ustore_key_t const* const ids = edges + triple * 3;
            if (format == ustore_dataset_format_ndjson_k) {
                output.append("{\"", task.outcome);
                output.append(source_id_field, task.outcome);
                output.append("\":", task.outcome);
                output.append_key(ids[0], task.outcome);
                output.append(",\"", task.outcome);
                output.append(target_id_field, task.outcome);
                output.append("\":", task.outcome);
                output.append_key(ids[1], task.outcome);
                if (!edge_id_field.empty()) {
                    output.append(",\"", task.outcome);
                    output.append(edge_id_field, task.outcome);
                    output.append("\":", task.outcome);
                    output.append_key(ids[2], task.outcome);
                }
                output.append("}\n", task.outcome);
            }
            else {
                output.append_key(ids[0], task.outcome);
                output.append(",", task.outcome);
                output.append_key(ids[1], task.outcome);
                if (!edge_id_field.empty()) {
                    output.append(",", task.outcome);
                    output.append_key(ids[2], task.outcome);
                }
                output.append("\n", task.outcome);
            }
        }
    };
    scan_keys<ustore_graph_scan_vertices_t, &ustore_graph_scan_vertices>(task, batch_size, export_batch);
    output.finish(task.outcome);
}

ustore_dataset_format_t ustore_dataset_format_of(ustore_bytes_view_t path, ustore_dataset_format_t asked) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;
    return format_of(string_of(path), asked);
}

#pragma endregion C Interface
