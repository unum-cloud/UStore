/**
 *  @file tools/dataset.cpp
 *  @author Ash Vardanian
 *  @date April 16, 2023
 *  @brief Imports and exports documents and graph edges as NDJSON on yyjson and CSV on StringZilla, and collections of
 *      every modality as Apache Arrow IPC through the codec of <tt>tools/arrow_ipc.hpp</tt>, moving columns in and out
 *      of the views of the C ABI through the converters and exporters of <tt>ustore/arrow.h</tt>.
 */

#include <cerrno>  // `errno`, `EINTR`, `EIO`, `EINVAL`, `ENOMEM`, `ENOTSUP`
#include <cstddef> // `std::size_t`, `std::byte`
#include <cstdint> // `std::int64_t`
#include <cstdio>  // `std::fopen`, `std::fwrite`, `std::fclose`
#include <cstdlib> // `std::free`
#include <cstring> // `std::memcmp`, `std::memcpy`

#if defined(_WIN32)
#include <fcntl.h>    // `_O_RDONLY`, `_O_BINARY`
#include <io.h>       // `_read`, `_write`, `_open`, `_close`
#include <sys/stat.h> // `_S_IREAD`, `_S_IWRITE`
#else
#include <fcntl.h>   // `open`
#include <sys/uio.h> // `writev`, `iovec`
#include <unistd.h>  // `read`, `close`
#endif

#include <algorithm>    // `std::copy`, `std::find`, `std::min`
#include <array>        // `std::array`
#include <new>          // `std::nothrow`
#include <charconv>     // `std::from_chars`, `std::to_chars`
#include <limits>       // `std::numeric_limits`
#include <span>         // `std::span`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::errc`
#include <type_traits>  // `std::is_same_v`
#include <utility>      // `std::move`, `std::swap`, `std::exchange`
#include <variant>      // `std::variant`, `std::visit`

#include <yyjson.h> // `yyjson_read`, `yyjson_mut_doc`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <stringzilla/stringzilla.hpp> // `sz::string_t`, `sz::string_view_t`

#include "ustore/arrow.h"    // `ustore_keys_view_from_arrow`, `ustore_strings_view_to_arrow`
#include "ustore/docs.h"     // `ustore_docs_write_t`, `ustore_docs_read_t`, `ustore_docs_scan_t`
#include "ustore/graph.h"    // `ustore_graph_write_edges_t`, `ustore_graph_scan_vertices_t`
#include "ustore/ranges.hpp" // `bits_view_t`, `tape_string`, `u64_tape`
#include "ustore/types.hpp"  // `sz`, `st`

#include "ustore/utilities/blocking_files.hpp" // `mapped_file_t`

#include "arrow_ipc.hpp" // `ipc_read_prefix`, `ipc_write_batch`, `ipc_batch_to_arrow`
#include "dataset.h"     // `ustore_docs_import_t`, `ustore_graph_export_t`

namespace unum::ustore::tools {

using namespace unum::ustore::utilities;

#pragma region Files

/** The format @p path names through its extension, or @p asked when it names one, staying automatic for a path
 *  naming none, like the standard input of @c -. */
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

/** Whether @p outcome already holds a failure, which every later step of a task skips past. */
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

/** Takes the arena a task brought, or makes a private one on host pages when it brought none, which it frees on
 *  destruction. */
class task_arena_t {
    ustore_arena_t private_arena_ = nullptr;
    ustore_arena_t chosen_ = nullptr;

  public:

    /** Takes @p given, or makes a private arena, reporting a failure to make one into @p outcome. */
    task_arena_t(ustore_arena_t given, ustore_outcome_t* outcome) noexcept : chosen_(given) {
        if (given) return;
        ustore_arena_init_t init {.arena = &private_arena_, .executor = nullptr, .outcome = outcome};
        ustore_arena_init(&init);
        chosen_ = private_arena_;
    }
    task_arena_t(task_arena_t const&) = delete;
    task_arena_t& operator=(task_arena_t const&) = delete;
    ~task_arena_t() noexcept { ustore_arena_free(private_arena_); }

    [[nodiscard]] ustore_arena_t get() const noexcept { return chosen_; }
};

/** Drops a trailing carriage return, so CRLF files read as LF files. */
[[nodiscard]] std::string_view without_carriage_return(sz::string_view_t line) noexcept {
    std::string_view view = line;
    if (!view.empty() && view.back() == '\r') view.remove_suffix(1);
    return view;
}

/** Copies @p path into @p copy, which ends in the null the file calls need, refusing an empty path. */
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

/** Copies @p value into @p root at the path @p field names, creating the objects between; a placed path stays. */
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
void read_header(csv_reader_t& reader, field_list_t fields, sz::string_t& names,
                 st::basic_vector<csv_column_t>& columns, ustore_outcome_t* outcome) noexcept {
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
[[nodiscard]] std::size_t column_index(sz::string_t const& names, st::basic_vector<csv_column_t> const& columns,
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
void resolve_fields(ustore_strings_view_t const& strings, ustore_size_t count,
                    st::basic_vector<std::string_view>& fields, ustore_outcome_t* outcome) noexcept {
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
    st::basic_vector<std::uint64_t> offsets_;

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
    st::basic_vector<csv_column_t> columns;
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
void upsert_edges(ustore_graph_import_t const& task, st::basic_vector<edge_row_t>& edges) noexcept {
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
void stage_edge(ustore_graph_import_t const& task, st::basic_vector<edge_row_t>& edges, edge_row_t edge) noexcept {
    if (st::failed(edges.push_back(edge_row_t {edge}))) {
        *task.outcome = {ustore_status_out_of_memory_k, "Failed to stage an edge"};
        return;
    }
    if (edges.size() >= items_in(task.max_batch_size, sizeof(edge_row_t))) upsert_edges(task, edges);
}

/** Imports one edge per NDJSON line, reading its vertices and edge from integer fields. */
void import_ndjson_edges(ustore_graph_import_t const& task, sz::string_view_t text) noexcept {
    st::basic_vector<edge_row_t> edges;
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
    st::basic_vector<csv_column_t> columns;
    read_header(reader, field_list_t {}, names, columns, task.outcome);
    if (failed(task.outcome)) return;

    std::size_t const source_column = column_index(names, columns, string_of(task.source_id_field));
    std::size_t const target_column = column_index(names, columns, string_of(task.target_id_field));
    std::size_t const edge_column = task.edge_id_field.length
                                        ? column_index(names, columns, string_of(task.edge_id_field))
                                        : std::numeric_limits<std::size_t>::max();
    if (source_column == columns.size() || target_column == columns.size() || edge_column == columns.size()) {
        *task.outcome = {ustore_status_malformed_k, "The CSV header lacks a vertex or edge column"};
        return;
    }

    st::basic_vector<edge_row_t> edges;
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

#pragma region Arrow

/** The schema metadata key naming the modality of an exported collection. */
constexpr std::string_view modality_key_k = "ustore.modality";

/** The schema metadata key naming the collection an export came from. */
constexpr std::string_view collection_key_k = "ustore.collection";

/** The field metadata key marking the XML values of a pages collection. */
constexpr std::string_view page_encoding_key_k = "ustore.page_encoding";

/** Pairs of metadata a schema read off an input may carry across its fields and itself. */
constexpr std::size_t schema_pairs_max_k = 64;

/** Zeros that the padding after every gathered buffer points into, as long as the widest alignment pads. */
constexpr std::byte zeros_k[ipc_alignment_direct_k] {};

/** The outcome a failure of the codec reports. */
[[nodiscard]] ustore_outcome_t outcome_of(ipc_status_t status) noexcept {
    switch (status) {
    case ipc_status_t::success_k: return {};
    case ipc_status_t::truncated_k: return {ustore_status_malformed_k, "The Arrow input ends inside a message"};
    case ipc_status_t::malformed_k: return {ustore_status_malformed_k, "The Arrow input is malformed"};
    case ipc_status_t::unsupported_k:
        return {
            ustore_status_unsupported_k,
            "The Arrow input is compressed, big-endian, dictionary-encoded, a union or a map, or holds nulls or views"};
    case ipc_status_t::too_small_k:
        return {ustore_status_unsupported_k, "The Arrow schema holds too many fields or pairs of metadata"};
    }
    return {ustore_status_internal_k, "The Arrow codec answered an unknown status"};
}

/** The @c errno code a stream callback answers for @p outcome. */
[[nodiscard]] int errno_of(ustore_outcome_t const& outcome) noexcept {
    switch (outcome.status) {
    case ustore_status_out_of_memory_k: return ENOMEM;
    case ustore_status_malformed_k: return EINVAL;
    case ustore_status_unsupported_k: return ENOTSUP;
    default: return EIO;
    }
}

/** One piece of a gathered write. */
struct output_piece_t {
    std::byte const* data;
    std::size_t size;
};

#if defined(_WIN32)

/** Reads up to @p count bytes into @p data, answering how many, zero at the end, or a negative count on a failure. */
[[nodiscard]] std::ptrdiff_t read_some(int descriptor, std::byte* data, std::size_t count) noexcept {
    return ::_read(descriptor, data, static_cast<unsigned>(count < (1u << 30) ? count : (1u << 30)));
}

/** Writes every one of @p pieces in order, answering whether all of them landed. */
[[nodiscard]] bool write_pieces(int descriptor, std::span<output_piece_t const> pieces) noexcept {
    for (output_piece_t const& piece : pieces)
        for (std::size_t done = 0; done != piece.size;) {
            std::size_t const left = piece.size - done;
            int const written = ::_write(descriptor, piece.data + done,
                                         static_cast<unsigned>(left < (1u << 30) ? left : (1u << 30)));
            if (written <= 0) return false;
            done += static_cast<std::size_t>(written);
        }
    return true;
}

[[nodiscard]] int open_for_reading(char const* path) noexcept { return ::_open(path, _O_RDONLY | _O_BINARY); }
[[nodiscard]] int create_for_writing(char const* path) noexcept {
    return ::_open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
}
[[nodiscard]] bool close_descriptor(int descriptor) noexcept { return ::_close(descriptor) == 0; }

#else

/** Reads up to @p count bytes into @p data, answering how many, zero at the end, or a negative count on a failure. */
[[nodiscard]] std::ptrdiff_t read_some(int descriptor, std::byte* data, std::size_t count) noexcept {
    for (;;) {
        ssize_t const got = ::read(descriptor, data, count);
        if (got >= 0 || errno != EINTR) return got;
    }
}

/** Writes every one of @p pieces in order through as few @c writev calls as a short write allows, answering whether all
 *  of them landed. */
[[nodiscard]] bool write_pieces(int descriptor, std::span<output_piece_t const> pieces) noexcept {
    constexpr std::size_t vectors_max_k = 64;
    std::size_t index = 0, done = 0;
    for (;;) {
        while (index != pieces.size() && pieces[index].size == done) ++index, done = 0;
        if (index == pieces.size()) return true;
        std::array<iovec, vectors_max_k> vectors;
        std::size_t count = 0;
        for (std::size_t next = index; next != pieces.size() && count != vectors_max_k; ++next) {
            std::size_t const skip = next == index ? done : 0;
            vectors[count++] = {const_cast<std::byte*>(pieces[next].data) + skip, pieces[next].size - skip};
        }
        ssize_t const written = ::writev(descriptor, vectors.data(), static_cast<int>(count));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        for (std::size_t left = static_cast<std::size_t>(written); left != 0;) {
            std::size_t const step = std::min(left, pieces[index].size - done);
            left -= step, done += step;
            if (done == pieces[index].size) ++index, done = 0;
        }
    }
}

[[nodiscard]] int open_for_reading(char const* path) noexcept { return ::open(path, O_RDONLY | O_CLOEXEC); }
[[nodiscard]] int create_for_writing(char const* path) noexcept {
    return ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
}
[[nodiscard]] bool close_descriptor(int descriptor) noexcept { return ::close(descriptor) == 0; }

#endif

/** A descriptor this process opened, closed on destruction, or none for a negative one. */
class owned_descriptor_t {
    int descriptor_ = -1;

  public:
    owned_descriptor_t() noexcept = default;
    explicit owned_descriptor_t(int descriptor) noexcept : descriptor_(descriptor) {}
    owned_descriptor_t(owned_descriptor_t const&) = delete;
    owned_descriptor_t& operator=(owned_descriptor_t const&) = delete;
    owned_descriptor_t& operator=(owned_descriptor_t&& other) noexcept {
        std::swap(descriptor_, other.descriptor_);
        return *this;
    }
    ~owned_descriptor_t() noexcept {
        if (descriptor_ >= 0) { [[maybe_unused]] bool const closed = close_descriptor(descriptor_); }
    }

    /** Closes the descriptor now, answering whether it closed, which the destructor would ignore. */
    [[nodiscard]] bool close() noexcept { return descriptor_ < 0 || close_descriptor(std::exchange(descriptor_, -1)); }

    [[nodiscard]] int get() const noexcept { return descriptor_; }
};

/** The bytes of an Arrow input: a whole mapped file taken in place, or a descriptor read in exactly the counts the
 *  codec names, which is how standard input, pipes and anything that does not map are read. */
class input_bytes_t {
    utilities::mapped_file_t mapped_;
    std::span<std::byte const> whole_;
    std::size_t cursor_ = 0;

    /** The descriptor read in order, borrowed from standard input or from @c owned_, or negative when mapped. */
    int descriptor_ = -1;

    /** The descriptor this input opened, none for standard input and mappings. */
    owned_descriptor_t owned_;

    /** Bytes @c peek read ahead off the descriptor, which @c take hands out first. */
    std::array<std::byte, 8> peeked_ {};
    std::size_t peeked_size_ = 0;
    std::size_t peeked_taken_ = 0;

    /** Reads off the descriptor into @p data until @p count bytes or the end, answering how many landed. */
    [[nodiscard]] std::size_t read_into(std::byte* data, std::size_t count, ustore_outcome_t* outcome) noexcept {
        std::size_t filled = 0;
        for (; peeked_taken_ != peeked_size_ && filled != count; ++filled) data[filled] = peeked_[peeked_taken_++];
        while (filled != count) {
            std::ptrdiff_t const got = read_some(descriptor_, data + filled, count - filled);
            if (got < 0) {
                *outcome = {ustore_status_io_k, "Failed to read the Arrow input"};
                return filled;
            }
            if (got == 0) break;
            filled += static_cast<std::size_t>(got);
        }
        return filled;
    }

  public:
    input_bytes_t() noexcept = default;
    input_bytes_t(input_bytes_t const&) = delete;
    input_bytes_t& operator=(input_bytes_t const&) = delete;

    /** Opens @p path, mapping it whole, or reading it in order when it is @c - or does not map into bytes. */
    void open(char const* path, ustore_outcome_t* outcome) noexcept {
        if (std::string_view {path} == "-") {
            descriptor_ = 0;
            return;
        }
        // A pipe or a FIFO maps empty or not at all, so it is read in order instead
        auto [file, map_status] = utilities::mapped_file_t::map(path);
        if (map_status.succeeded() && !file.view().empty()) {
            mapped_ = std::move(file);
            std::string_view const bytes = mapped_.view();
            whole_ = {reinterpret_cast<std::byte const*>(bytes.data()), bytes.size()};
            return;
        }
        owned_ = owned_descriptor_t {open_for_reading(path)};
        descriptor_ = owned_.get();
        if (descriptor_ < 0) *outcome = {ustore_status_io_k, "Failed to open the Arrow input"};
    }

    /** Views the first @p count bytes, at most 8, without taking them, fewer only at the end. */
    [[nodiscard]] std::span<std::byte const> peek(std::size_t count, ustore_outcome_t* outcome) noexcept {
        if (descriptor_ < 0) return whole_.first(std::min(count, whole_.size()));
        while (peeked_size_ < count) {
            std::ptrdiff_t const got = read_some(descriptor_, peeked_.data() + peeked_size_, count - peeked_size_);
            if (got < 0) *outcome = {ustore_status_io_k, "Failed to read the Arrow input"};
            if (got <= 0) break;
            peeked_size_ += static_cast<std::size_t>(got);
        }
        return {peeked_.data(), peeked_size_};
    }

    /** Takes the next @p count bytes, viewing the mapping or read into @p storage, and fewer only at the end. */
    [[nodiscard]] std::span<std::byte const> take(std::size_t count, st::basic_vector<std::byte>& storage,
                                                  ustore_outcome_t* outcome) noexcept {
        if (descriptor_ < 0) {
            std::span<std::byte const> const taken = whole_.subspan(cursor_, std::min(count, whole_.size() - cursor_));
            cursor_ += taken.size();
            return taken;
        }
        if (st::failed(storage.resize(st::assume_uninitialized, count))) {
            *outcome = {ustore_status_out_of_memory_k, "Failed to buffer the Arrow input"};
            return {};
        }
        return {storage.data(), read_into(storage.data(), count, outcome)};
    }
};

/** One batch handed out of an input stream, which owns its body when it was read off a descriptor, freed by its
 *  release. */
struct ipc_input_batch_t {
    st::basic_vector<std::byte> body;
    std::array<ipc_node_t, ipc_fields_max_k> nodes {};
    std::array<std::span<std::byte const>, ipc_buffers_max_k> buffers {};
    ipc_arrow_storage_t storage {};
};

/** The schema handed out of an input stream, freed by its release. */
struct ipc_input_schema_t {
    st::basic_vector<char> text;
    ipc_arrow_storage_t storage {};
};

/** Releases a batch of an input stream: its children, then the memory it owns. */
void release_input_batch(ArrowArray* array) noexcept {
    for (std::int64_t index = 0; index != array->n_children; ++index)
        if (array->children[index]->release) array->children[index]->release(array->children[index]);
    delete static_cast<ipc_input_batch_t*>(array->private_data);
    array->release = nullptr;
}

/** Releases the schema of an input stream: its children, then the memory it owns. */
void release_input_schema(ArrowSchema* schema) noexcept {
    for (std::int64_t index = 0; index != schema->n_children; ++index)
        if (schema->children[index]->release) schema->children[index]->release(schema->children[index]);
    delete static_cast<ipc_input_schema_t*>(schema->private_data);
    schema->release = nullptr;
}

/**
 *  @brief The state of the @c ArrowArrayStream an Arrow input is handed over as, which the import call owns and which
 *      outlives every batch, so releasing the stream frees nothing.
 *
 *  The schema is read before the stream is handed over, so the modality of a new collection can come from it.
 */
struct ipc_input_stream_t {
    input_bytes_t* input = nullptr;

    /** The schema message read off a descriptor, which the schema views. */
    st::basic_vector<std::byte> schema_metadata;

    /** The metadata of the message being read, reused across batches. */
    st::basic_vector<std::byte> metadata;

    std::array<ipc_field_t, ipc_fields_max_k> fields {};
    std::array<ipc_key_value_t, schema_pairs_max_k> pairs {};
    ipc_schema_t schema {};

    /** Why the last callback failed. */
    ustore_outcome_t failure {};
};

/** Reads the next message into @p message, its metadata into @p storage, answering false at the end of the stream,
 *  which is its end-of-stream marker or the end of the input between messages, or on a failure in @p outcome. */
[[nodiscard]] bool next_message(input_bytes_t& input, st::basic_vector<std::byte>& storage, ipc_message_t& message,
                                ustore_outcome_t* outcome) noexcept {
    std::array<std::byte, 8> prefix {};
    std::size_t have = 0, wanted = 4, prefix_bytes = 0;
    std::uint32_t metadata_bytes = 0;
    ipc_status_t status = ipc_status_t::truncated_k;
    while (status == ipc_status_t::truncated_k && have < wanted) {
        std::span<std::byte const> const taken = input.take(wanted - have, storage, outcome);
        if (failed(outcome)) return false;
        if (taken.empty() && have == 0) return false;
        if (taken.empty()) break;
        std::memcpy(prefix.data() + have, taken.data(), taken.size());
        have += taken.size();
        status = ipc_read_prefix({prefix.data(), have}, metadata_bytes, prefix_bytes);
        wanted = prefix_bytes;
    }
    if (status != ipc_status_t::success_k) {
        *outcome = outcome_of(status);
        return false;
    }
    if (metadata_bytes == 0) return false;
    std::span<std::byte const> const metadata = input.take(metadata_bytes, storage, outcome);
    if (failed(outcome)) return false;
    status = metadata.size() == metadata_bytes ? ipc_read_message(metadata, message) : ipc_status_t::truncated_k;
    if (status != ipc_status_t::success_k) *outcome = outcome_of(status);
    return status == ipc_status_t::success_k;
}

/** Records @p outcome as the failure of a stream callback and answers its @c errno code. */
[[nodiscard]] int stream_failure(ipc_input_stream_t& state, ustore_outcome_t outcome) noexcept {
    state.failure = outcome;
    return errno_of(outcome);
}

int input_get_schema(ArrowArrayStream* stream, ArrowSchema* out) noexcept {
    ipc_input_stream_t& state = *static_cast<ipc_input_stream_t*>(stream->private_data);
    *out = {};
    ipc_input_schema_t* const holder = new (std::nothrow) ipc_input_schema_t;
    if (!holder) return stream_failure(state, {ustore_status_out_of_memory_k, "Failed to allocate the schema"});
    ipc_status_t status = ipc_status_t::too_small_k;
    for (std::size_t bytes = 1024; status == ipc_status_t::too_small_k && bytes <= (1u << 26); bytes *= 2) {
        if (st::failed(holder->text.resize(bytes))) break;
        holder->storage.text = holder->text;
        status = ipc_schema_to_arrow(state.schema, holder->storage, *out);
    }
    if (status != ipc_status_t::success_k) {
        delete holder;
        *out = {};
        return stream_failure(state, outcome_of(status));
    }
    out->release = &release_input_schema;
    out->private_data = holder;
    return 0;
}

int input_get_next(ArrowArrayStream* stream, ArrowArray* out) noexcept {
    ipc_input_stream_t& state = *static_cast<ipc_input_stream_t*>(stream->private_data);
    *out = {};
    ustore_outcome_t outcome {};
    ipc_message_t message;
    if (!next_message(*state.input, state.metadata, message, &outcome))
        return failed(&outcome) ? stream_failure(state, outcome) : 0;
    if (message.kind != ipc_message_kind_t::record_batch_k)
        return stream_failure(state, {ustore_status_malformed_k, "The Arrow input holds a second schema"});

    ipc_input_batch_t* const batch = new (std::nothrow) ipc_input_batch_t;
    if (!batch) return stream_failure(state, {ustore_status_out_of_memory_k, "Failed to allocate a batch"});
    std::span<std::byte const> const body = state.input->take(static_cast<std::size_t>(message.body_bytes), batch->body,
                                                              &outcome);
    ipc_batch_t decoded;
    ipc_status_t status = ipc_read_batch(message, body, state.schema, batch->nodes, batch->buffers, decoded);
    if (status == ipc_status_t::success_k) status = ipc_validate_batch(state.schema, decoded);
    if (status == ipc_status_t::success_k) status = ipc_batch_to_arrow(state.schema, decoded, batch->storage, *out);
    if (failed(&outcome) || status != ipc_status_t::success_k) {
        delete batch;
        *out = {};
        return stream_failure(state, failed(&outcome) ? outcome : outcome_of(status));
    }
    out->release = &release_input_batch;
    out->private_data = batch;
    return 0;
}

char const* input_get_last_error(ArrowArrayStream* stream) noexcept {
    return static_cast<ipc_input_stream_t*>(stream->private_data)->failure.message;
}

void input_release(ArrowArrayStream* stream) noexcept { stream->release = nullptr; }

/** The value of the schema metadata @p key, empty when absent. */
[[nodiscard]] std::string_view metadata_value(std::span<ipc_key_value_t const> pairs, std::string_view key) noexcept {
    for (ipc_key_value_t const& pair : pairs)
        if (pair.key == key) return pair.value;
    return {};
}

/** The field named @p name, or null. */
[[nodiscard]] ipc_field_t const* field_named(ipc_schema_t const& schema, std::string_view name) noexcept {
    for (ipc_field_t const& field : schema.fields)
        if (field.name == name) return &field;
    return nullptr;
}

/** What @p schema holds: its @c ustore.modality, else what its columns spell, where documents read as blobs, or zero
 *  when neither tells. */
[[nodiscard]] ustore_modality_t modality_of(ipc_schema_t const& schema) noexcept {
    if (std::string_view const named = metadata_value(schema.metadata, modality_key_k); !named.empty())
        return ustore_modality_from_name(bytes_of(named));
    if (field_named(schema, "source") && field_named(schema, "target")) return ustore_modality_graph_k;
    ipc_field_t const* const key = field_named(schema, "key");
    ipc_field_t const* const value = field_named(schema, "value");
    if (!key || !value) return ustore_modality_t {};
    if (ipc_type_is_variable(key->type)) return ustore_modality_strings_k;
    if (metadata_value(value->metadata, page_encoding_key_k) == "xml") return ustore_modality_pages_k;
    return ustore_modality_blobs_k;
}

/** Finds the collection @p name addresses into @p id, answering its modality, or zero when no collection carries the
 *  name. */
[[nodiscard]] ustore_modality_t find_named_collection(ustore_database_t database, std::string_view name,
                                                      ustore_collection_id_t& id, ustore_outcome_t* outcome) noexcept {
    task_arena_t const arena {nullptr, outcome};
    if (failed(outcome)) return ustore_modality_t {};
    std::uint64_t const offsets[2] {0, name.size()};
    ustore_octet_t const* presences = nullptr;
    ustore_collection_id_t const* ids = nullptr;
    ustore_modality_t const* modalities = nullptr;
    ustore_collection_find_t find {};
    find.database = database;
    find.outcome = outcome;
    find.count = 1;
    find.names = u64_tape(name.data(), offsets);
    find.presences = &presences;
    find.collections = &ids;
    find.modalities = &modalities;
    find.arena = arena.get();
    ustore_collection_find(&find);
    if (failed(outcome) || !(presences[0] & 1)) return ustore_modality_t {};
    id = ids[0];
    return modalities[0];
}

/** Writes an Arrow stream or file to a descriptor as batches are exported, gathering every body without a copy and
 *  never seeking, so standard output carries it too. */
class ipc_output_t {
    int descriptor_ = -1;
    ustore_dataset_format_t format_ = ustore_dataset_format_arrow_stream_k;
    ipc_options_t options_ {};
    std::uint64_t position_ = 0;
    st::basic_vector<std::byte> metadata_;
    st::basic_vector<ipc_block_t> blocks_;
    std::array<ipc_gather_t, ipc_buffers_max_k> gathers_ {};

    /** Writes @p pieces, advancing the position past them. */
    void write(std::span<output_piece_t const> pieces, ustore_outcome_t* outcome) noexcept {
        if (!write_pieces(descriptor_, pieces)) {
            *outcome = {ustore_status_io_k, "Failed to write the Arrow output"};
            return;
        }
        for (output_piece_t const& piece : pieces) position_ += piece.size;
    }

    /** Sizes the metadata buffer to @p bytes. */
    [[nodiscard]] bool reserve_metadata(std::size_t bytes, ustore_outcome_t* outcome) noexcept {
        if (!st::failed(metadata_.resize(st::assume_uninitialized, bytes))) return true;
        *outcome = {ustore_status_out_of_memory_k, "Failed to buffer the Arrow metadata"};
        return false;
    }

  public:
    ipc_output_t(int descriptor, ustore_dataset_format_t format, ipc_options_t options) noexcept
        : descriptor_(descriptor), format_(format), options_(options) {}

    /** Writes the file's magic, when writing a file, and the schema message. */
    void begin(ipc_schema_t const& schema, ustore_outcome_t* outcome) noexcept {
        if (format_ == ustore_dataset_format_arrow_file_k) {
            output_piece_t const magic {reinterpret_cast<std::byte const*>(ipc_file_magic_k.data()),
                                        ipc_file_magic_k.size()};
            write({&magic, 1}, outcome);
        }
        std::size_t needed = 0, written = 0;
        ipc_status_t status = ipc_write_schema(schema, options_, position_, {}, needed);
        if (failed(outcome) || !reserve_metadata(needed, outcome)) return;
        if (status == ipc_status_t::too_small_k)
            status = ipc_write_schema(schema, options_, position_, metadata_, written);
        if (status != ipc_status_t::success_k) {
            *outcome = outcome_of(status);
            return;
        }
        output_piece_t const piece {metadata_.data(), written};
        write({&piece, 1}, outcome);
    }

    /** Writes one batch: its metadata, then every buffer of its body in place, each followed by its padding. */
    void append(ipc_schema_t const& schema, ipc_batch_t const& batch, ustore_outcome_t* outcome) noexcept {
        std::size_t needed = 0, written = 0;
        std::uint64_t body_bytes = 0;
        ipc_status_t status = ipc_write_batch(schema, batch, options_, position_, {}, needed, gathers_, body_bytes);
        if (status == ipc_status_t::too_small_k && reserve_metadata(needed, outcome))
            status = ipc_write_batch(schema, batch, options_, position_, metadata_, written, gathers_, body_bytes);
        if (failed(outcome)) return;
        if (status != ipc_status_t::success_k) {
            *outcome = outcome_of(status);
            return;
        }
        std::array<output_piece_t, 1 + 2 * ipc_buffers_max_k> pieces;
        std::size_t count = 0;
        pieces[count++] = {metadata_.data(), written};
        for (std::size_t index = 0; index != batch.buffers.size(); ++index) {
            pieces[count++] = {gathers_[index].bytes.data(), gathers_[index].bytes.size()};
            pieces[count++] = {zeros_k, gathers_[index].padding};
        }
        ipc_block_t const block {static_cast<std::int64_t>(position_), static_cast<std::int32_t>(written),
                                 static_cast<std::int64_t>(body_bytes)};
        write({pieces.data(), count}, outcome);
        if (!failed(outcome) && format_ == ustore_dataset_format_arrow_file_k &&
            st::failed(blocks_.push_back(ipc_block_t {block})))
            *outcome = {ustore_status_out_of_memory_k, "Failed to record the Arrow block"};
    }

    /** Writes the end-of-stream marker and, when writing a file, the footer naming every batch. */
    void finish(ipc_schema_t const& schema, ustore_outcome_t* outcome) noexcept {
        std::array<std::byte, 8> end {};
        std::size_t written = 0;
        [[maybe_unused]] ipc_status_t const ended = ipc_write_end_of_stream(end, written);
        output_piece_t const end_piece {end.data(), written};
        write({&end_piece, 1}, outcome);
        if (failed(outcome) || format_ != ustore_dataset_format_arrow_file_k) return;
        std::span<ipc_block_t const> const blocks {blocks_};
        std::size_t needed = 0;
        ipc_status_t status = ipc_write_footer(schema, blocks, {}, needed);
        if (!reserve_metadata(needed, outcome)) return;
        if (status == ipc_status_t::too_small_k) status = ipc_write_footer(schema, blocks, metadata_, written);
        if (status != ipc_status_t::success_k) {
            *outcome = outcome_of(status);
            return;
        }
        output_piece_t const footer {metadata_.data(), written};
        write({&footer, 1}, outcome);
    }
};

/** The descriptor an export writes, created for a path and closed on destruction, or standard output for @c -. */
class output_descriptor_t {

    /** The descriptor written, borrowed from standard output or from @c owned_. */
    int descriptor_ = -1;

    /** The file this output created, none for standard output. */
    owned_descriptor_t owned_;

  public:

    /** Creates or truncates @p path, or takes standard output for @c -. */
    void open(char const* path, ustore_outcome_t* outcome) noexcept {
        if (std::string_view {path} == "-") {
            descriptor_ = 1;
            return;
        }
        owned_ = owned_descriptor_t {create_for_writing(path)};
        descriptor_ = owned_.get();
        if (descriptor_ < 0) *outcome = {ustore_status_io_k, "Failed to create the Arrow output"};
    }

    /** Closes a created file, reporting a failure the destructor would ignore. */
    void close(ustore_outcome_t* outcome) noexcept {
        if (!owned_.close() && !failed(outcome)) *outcome = {ustore_status_io_k, "Failed to close the Arrow output"};
    }

    [[nodiscard]] int get() const noexcept { return descriptor_; }
};

/** The index a @c modality_columns_t holds for a column the schema left out. */
constexpr std::size_t absent_column_k = static_cast<std::size_t>(-1);

/** Where the columns of a modality sit among the children of an Arrow schema. */
struct modality_columns_t {

    /** Keys of blobs, documents and pages, or the keys strings map onto. */
    std::size_t keys = absent_column_k;

    /** Values of blobs, documents and pages, or the strings of a strings collection. */
    std::size_t strings = absent_column_k;

    /** The sources of edges. */
    std::size_t sources = absent_column_k;

    /** The targets of edges. */
    std::size_t targets = absent_column_k;

    /** The identifiers of edges, which an input may leave out for the default edge. */
    std::size_t edges = absent_column_k;
};

/** Matches the children of @p schema to the columns of @p modality by name, refusing a missing, repeated or unknown
 *  one. */
[[nodiscard]] bool resolve_columns(ArrowSchema const& schema, ustore_modality_t modality, modality_columns_t& columns,
                                   ustore_outcome_t* outcome) noexcept {
    bool const graph = modality == ustore_modality_graph_k;
    bool const strings = modality == ustore_modality_strings_k;
    for (std::int64_t index = 0; index != schema.n_children; ++index) {
        std::string_view const name = schema.children[index]->name ? schema.children[index]->name : "";
        std::size_t* slot = nullptr;
        if (graph)
            slot = name == "source"   ? &columns.sources
                   : name == "target" ? &columns.targets
                   : name == "edge"   ? &columns.edges
                                      : nullptr;
        else if (name == "key") slot = strings ? &columns.strings : &columns.keys;
        else if (name == "value") slot = strings ? &columns.keys : &columns.strings;
        if (!slot || *slot != absent_column_k) {
            *outcome = {ustore_status_invalid_argument_k,
                        "The Arrow input carries a column its modality does not name, or names one twice"};
            return false;
        }
        *slot = static_cast<std::size_t>(index);
    }
    bool const complete = graph ? columns.sources != absent_column_k && columns.targets != absent_column_k
                                : columns.keys != absent_column_k && columns.strings != absent_column_k;
    if (!complete) {
        *outcome = {ustore_status_invalid_argument_k, "The Arrow input lacks a column its modality needs"};
        return false;
    }
    return true;
}

/** The columns of one batch as the views of the C ABI, read in place, those its modality lacks left empty. */
struct batch_views_t {
    ustore_size_t count = 0;
    ustore_keys_view_t keys {};
    ustore_strings_view_t strings {};
    ustore_keys_view_t sources {};
    ustore_keys_view_t targets {};
    ustore_keys_view_t edges {};
};

/** The outcome a converter's @p status reports for a column of an Arrow input. */
[[nodiscard]] ustore_outcome_t column_outcome(ustore_status_t status) noexcept {
    switch (status) {
    case ustore_status_unsupported_k:
        return {status, "An Arrow column is of another type, a dictionary or a view, which no write reads"};
    case ustore_status_malformed_k: return {status, "An Arrow column misses a buffer or is shorter than its batch"};
    default: return {status, "An Arrow column holds a null"};
    }
}

/** Reads the columns of @p batch, laid out as @p schema says, into @p rows in place. */
[[nodiscard]] bool rows_of(ArrowSchema const& schema, ArrowArray const& batch, modality_columns_t const& columns,
                           batch_views_t& rows, ustore_outcome_t* outcome) noexcept {
    rows = {};
    rows.count = static_cast<ustore_size_t>(batch.length);
    ustore_status_t status = batch.n_children == schema.n_children && batch.null_count <= 0 ? ustore_status_success_k
                                                                                            : ustore_status_malformed_k;
    auto const read_keys = [&](std::size_t column, ustore_keys_view_t& keys) noexcept {
        if (status != ustore_status_success_k || column == absent_column_k) return;
        ustore_size_t count = 0;
        status = ustore_keys_view_from_arrow(schema.children[column], batch.children[column], &keys, &count);
        if (status == ustore_status_success_k && count < static_cast<ustore_size_t>(batch.offset + batch.length))
            status = ustore_status_malformed_k;
        if (keys.data) keys.data += batch.offset;
    };
    read_keys(columns.keys, rows.keys);
    read_keys(columns.sources, rows.sources);
    read_keys(columns.targets, rows.targets);
    read_keys(columns.edges, rows.edges);
    if (status == ustore_status_success_k && columns.strings != absent_column_k) {
        ustore_size_t count = 0;
        status = ustore_strings_view_from_arrow(schema.children[columns.strings], batch.children[columns.strings],
                                                &rows.strings, &count);
        if (status == ustore_status_success_k && count < static_cast<ustore_size_t>(batch.offset + batch.length))
            status = ustore_status_malformed_k;
        rows.strings = tape_slice(rows.strings, static_cast<std::size_t>(batch.offset));
    }
    if (status != ustore_status_success_k) *outcome = column_outcome(status);
    return status == ustore_status_success_k;
}

/** Rows one write of a batched import holds at most, halved for a write the head refuses for its size. */
constexpr std::size_t import_chunk_rows_k = 65536;

/** Writes rows @b [first,first+count) of @p rows into @p collection through the writes of @p modality. */
void write_chunk(ustore_database_t database, ustore_collection_id_t collection, ustore_modality_t modality,
                 batch_views_t const& rows, std::size_t first, std::size_t count, ustore_durability_t durability,
                 ustore_outcome_t* outcome) noexcept {
    ustore_collections_view_t const collections {&collection, 0};
    auto const slice = [&](ustore_keys_view_t keys) noexcept {
        return ustore_keys_view_t {keys.data ? keys.data + first : nullptr, keys.stride};
    };
    ustore_strings_view_t const strings = tape_slice(rows.strings, first);
    switch (modality) {
    case ustore_modality_docs_k: {
        ustore_docs_write_t write {.database = database,
                                   .outcome = outcome,
                                   .count = count,
                                   .collections = collections,
                                   .keys = slice(rows.keys),
                                   .values = strings,
                                   .type = ustore_docs_field_type_json_k,
                                   .modification = ustore_docs_modify_upsert_k,
                                   .durability = durability};
        return ustore_docs_write(&write);
    }
    case ustore_modality_pages_k: {
        ustore_pages_import_t import {.database = database,
                                      .outcome = outcome,
                                      .count = count,
                                      .collections = collections,
                                      .keys = slice(rows.keys),
                                      .values = strings,
                                      .encoding = ustore_page_encoding_xml_k,
                                      .durability = durability};
        return ustore_pages_import(&import);
    }
    case ustore_modality_strings_k: {
        ustore_strings_write_t write {.database = database,
                                      .outcome = outcome,
                                      .count = count,
                                      .collections = collections,
                                      .strings = strings,
                                      .values = slice(rows.keys),
                                      .durability = durability};
        return ustore_strings_write(&write);
    }
    case ustore_modality_graph_k: {
        ustore_graph_write_edges_t write {.database = database,
                                          .outcome = outcome,
                                          .count = count,
                                          .collections = collections,
                                          .edges = slice(rows.edges),
                                          .sources = slice(rows.sources),
                                          .targets = slice(rows.targets),
                                          .durability = durability};
        return ustore_graph_write_edges(&write);
    }
    default: {
        ustore_blobs_write_t write {.database = database,
                                    .outcome = outcome,
                                    .count = count,
                                    .collections = collections,
                                    .keys = slice(rows.keys),
                                    .values = strings,
                                    .durability = durability};
        return ustore_blobs_write(&write);
    }
    }
}

/** Writes every row of @p rows in chunks, halving one the head refused for its size, each buffered but the last, which
 *  pays @p durability. */
void write_rows(ustore_database_t database, ustore_collection_id_t collection, ustore_modality_t modality,
                batch_views_t const& rows, ustore_durability_t durability, ustore_outcome_t* outcome) noexcept {
    std::size_t chunk = import_chunk_rows_k;
    for (std::size_t first = 0; first != rows.count;) {
        std::size_t const count = std::min<std::size_t>(chunk, rows.count - first);
        bool const last = first + count == rows.count;
        write_chunk(database, collection, modality, rows, first, count,
                    last ? durability : ustore_durability_buffered_k, outcome);
        if (outcome->status == ustore_status_capacity_exhausted_k && count > 1) {
            *outcome = {};
            chunk = count / 2;
            continue;
        }
        if (failed(outcome)) return;
        first += count;
    }
}

/** Appends @p rows to @p ingest. */
void append_rows(ustore_ingest_t ingest, batch_views_t const& rows, ustore_outcome_t* outcome) noexcept {
    ustore_ingest_append_t append {.ingest = ingest,
                                   .outcome = outcome,
                                   .count = rows.count,
                                   .keys = rows.keys,
                                   .values = rows.strings,
                                   .sources = rows.sources,
                                   .targets = rows.targets,
                                   .edges = rows.edges};
    ustore_ingest_append(&append);
}

/** The modality of @p collection as @p view lists it, or zero with a failure in @p outcome. */
[[nodiscard]] ustore_modality_t listed_modality(ustore_database_t database, ustore_snapshot_t snapshot,
                                                ustore_collection_id_t collection, ustore_outcome_t* outcome) noexcept {
    task_arena_t const arena {nullptr, outcome};
    if (failed(outcome)) return ustore_modality_t {};
    ustore_collection_id_t const* collections = nullptr;
    ustore_modality_t const* modalities = nullptr;
    ustore_size_t written = 0;
    ustore_collection_list_t list {.database = database,
                                   .outcome = outcome,
                                   .snapshot = snapshot,
                                   .collections = &collections,
                                   .modalities = &modalities,
                                   .written = &written,
                                   .arena = arena.get()};
    ustore_collection_list(&list);
    for (ustore_size_t index = 0; !failed(outcome) && index != written; ++index)
        if (collections[index] == collection) return modalities[index];
    if (!failed(outcome)) *outcome = {ustore_status_not_found_k, "The collection does not exist"};
    return ustore_modality_t {};
}

/** The fields of the schema a collection of @p modality exports, their names pointing at literals. */
[[nodiscard]] std::span<ipc_field_t const> export_fields(ustore_modality_t modality) noexcept {
    static constexpr ipc_key_value_t uuid_pairs_k[] {{ipc_extension_name_key_k, ipc_extension_uuid_k},
                                                     {"ARROW:extension:metadata", ""}};
    static constexpr ipc_key_value_t xml_pairs_k[] {{page_encoding_key_k, "xml"}};
    static constexpr ipc_field_t uuid_k {.type = ipc_type_t::fixed_size_binary_k,
                                         .width = 16,
                                         .metadata = uuid_pairs_k};
    static constexpr ipc_field_t keyed_k[] {{.name = "key", .type = uuid_k.type, .width = 16, .metadata = uuid_pairs_k},
                                            {.name = "value", .type = ipc_type_t::binary_k}};
    static constexpr ipc_field_t pages_k[] {{.name = "key", .type = uuid_k.type, .width = 16, .metadata = uuid_pairs_k},
                                            {.name = "value", .type = ipc_type_t::binary_k, .metadata = xml_pairs_k}};
    static constexpr ipc_field_t strings_k[] {
        {.name = "key", .type = ipc_type_t::binary_k},
        {.name = "value", .type = uuid_k.type, .width = 16, .metadata = uuid_pairs_k}};
    static constexpr ipc_field_t graph_k[] {
        {.name = "source", .type = uuid_k.type, .width = 16, .metadata = uuid_pairs_k},
        {.name = "target", .type = uuid_k.type, .width = 16, .metadata = uuid_pairs_k},
        {.name = "edge", .type = uuid_k.type, .width = 16, .metadata = uuid_pairs_k}};
    switch (modality) {
    case ustore_modality_pages_k: return pages_k;
    case ustore_modality_strings_k: return strings_k;
    case ustore_modality_graph_k: return graph_k;
    default: return keyed_k;
    }
}

/** One batch of an export: the columns the exporters handed out of its arena, released together. */
struct export_batch_t {
    std::array<ArrowSchema, 3> schemas {};
    std::array<ArrowDeviceArray, 3> columns {};
    std::size_t count = 0;

    export_batch_t() noexcept = default;
    export_batch_t(export_batch_t const&) = delete;
    export_batch_t& operator=(export_batch_t const&) = delete;
    ~export_batch_t() noexcept {
        for (std::size_t index = 0; index != count; ++index) {
            if (columns[index].array.release) columns[index].array.release(&columns[index].array);
            if (schemas[index].release) schemas[index].release(&schemas[index]);
        }
    }

    /** Exports @p keys of @p rows rows as the next column. */
    void add(ustore_keys_view_t keys, ustore_size_t rows, ustore_arena_t arena, ustore_outcome_t* outcome) noexcept {
        if (failed(outcome)) return;
        ustore_status_t const status = ustore_keys_view_to_arrow(keys, rows, arena, &schemas[count], &columns[count]);
        if (status != ustore_status_success_k) *outcome = {status, "Failed to export the keys of a batch"};
        else ++count;
    }

    /** Exports @p strings of @p rows rows as the next column, as @c binary, which the stream's one schema names. */
    void add(ustore_strings_view_t strings, ustore_size_t rows, ustore_arena_t arena,
             ustore_outcome_t* outcome) noexcept {
        if (failed(outcome)) return;
        ustore_status_t const status = ustore_strings_view_to_arrow(strings, rows, ustore_strings_u32_tape_k, arena,
                                                                    &schemas[count], &columns[count]);
        if (status != ustore_status_success_k) *outcome = {status, "Failed to export the values of a batch"};
        else ++count;
    }
};

/** The cursor an export walks, whose alternative names the modality of its collection. */
using export_cursor_t = std::variant<ustore_blobs_cursor_t, ustore_docs_cursor_t, ustore_pages_cursor_t,
                                     ustore_strings_cursor_t, ustore_graph_vertices_cursor_t>;

void free_cursor(ustore_blobs_cursor_t cursor) noexcept { ustore_blobs_cursor_free(cursor); }
void free_cursor(ustore_docs_cursor_t cursor) noexcept { ustore_docs_cursor_free(cursor); }
void free_cursor(ustore_pages_cursor_t cursor) noexcept { ustore_pages_cursor_free(cursor); }
void free_cursor(ustore_strings_cursor_t cursor) noexcept { ustore_strings_cursor_free(cursor); }
void free_cursor(ustore_graph_vertices_cursor_t cursor) noexcept { ustore_graph_vertices_cursor_free(cursor); }

/** The walk an export takes over a collection of one modality, one batch at a time. */
class export_walk_t {
    ustore_database_t database_ = nullptr;
    ustore_view_t view_ = nullptr;
    ustore_collection_id_t collection_ {};
    export_cursor_t cursor_;

    /** Where a graph's vertices land before their edges are read into the batch's arena. */
    task_arena_t vertices_arena_;

    template <typename cursor_type_, typename init_type_>
    void open_(void (*init_)(init_type_*), ustore_outcome_t* outcome) noexcept {
        init_type_ init {};
        init.cursor = &cursor_.template emplace<cursor_type_>(), init.database = database_, init.outcome = outcome;
        init.view = view_, init.collection = collection_;
        init_(&init);
    }

    template <typename next_type_, typename cursor_type_>
    [[nodiscard]] std::size_t pull_keyed_(void (*next_)(next_type_*), cursor_type_ cursor, ustore_size_t wanted,
                                          ustore_arena_t arena, export_batch_t& batch,
                                          ustore_outcome_t* outcome) noexcept {
        ustore_key_t const* keys = nullptr;
        ustore_strings_view_t values {};
        ustore_size_t rows = 0;
        next_type_ next {};
        next.database = database_, next.outcome = outcome, next.cursor = cursor, next.keys = &keys;
        next.values = &values, next.written = &rows, next.arena = arena, next.wanted = wanted;
        if constexpr (requires { next.outputs_encoding; }) next.outputs_encoding = ustore_page_encoding_xml_k;
        next_(&next);
        if (failed(outcome) || !rows) return 0;
        batch.add(ustore_keys_view_t {keys, sizeof(ustore_key_t)}, rows, arena, outcome);
        batch.add(values, rows, arena, outcome);
        return rows;
    }

    [[nodiscard]] std::size_t pull_strings_(ustore_strings_cursor_t cursor, ustore_size_t wanted, ustore_arena_t arena,
                                            export_batch_t& batch, ustore_outcome_t* outcome) noexcept {
        ustore_strings_view_t strings {};
        ustore_key_t const* values = nullptr;
        ustore_size_t rows = 0;
        ustore_strings_cursor_next_t next {.database = database_,
                                           .outcome = outcome,
                                           .cursor = cursor,
                                           .strings = &strings,
                                           .values = &values,
                                           .written = &rows,
                                           .arena = arena,
                                           .wanted = wanted};
        ustore_strings_cursor_next(&next);
        if (failed(outcome) || !rows) return 0;
        batch.add(strings, rows, arena, outcome);
        batch.add(ustore_keys_view_t {values, sizeof(ustore_key_t)}, rows, arena, outcome);
        return rows;
    }

  public:
    export_walk_t(ustore_database_t database, ustore_view_t view, ustore_collection_id_t collection,
                  ustore_modality_t modality, ustore_outcome_t* outcome) noexcept
        : database_(database), view_(view), collection_(collection), vertices_arena_(nullptr, outcome) {
        if (failed(outcome)) return;
        switch (modality) {
        case ustore_modality_docs_k: open_<ustore_docs_cursor_t>(&ustore_docs_cursor_init, outcome); break;
        case ustore_modality_pages_k: open_<ustore_pages_cursor_t>(&ustore_pages_cursor_init, outcome); break;
        case ustore_modality_strings_k: open_<ustore_strings_cursor_t>(&ustore_strings_cursor_init, outcome); break;
        case ustore_modality_graph_k:
            open_<ustore_graph_vertices_cursor_t>(&ustore_graph_vertices_cursor_init, outcome);
            break;
        default: open_<ustore_blobs_cursor_t>(&ustore_blobs_cursor_init, outcome);
        }
    }
    export_walk_t(export_walk_t const&) = delete;
    export_walk_t& operator=(export_walk_t const&) = delete;
    ~export_walk_t() noexcept {
        std::visit([]<typename cursor_type_>(cursor_type_ cursor) noexcept { free_cursor(cursor); }, cursor_);
    }

    /** Pulls up to @p wanted rows, or the edges of up to @p wanted source vertices, into @p arena and exports them
     *  into @p batch, answering the rows, zero once the walk ends. */
    [[nodiscard]] std::size_t next(ustore_size_t wanted, ustore_arena_t arena, export_batch_t& batch,
                                   ustore_outcome_t* outcome) noexcept {
        return std::visit(
            [&]<typename cursor_type_>(cursor_type_ cursor) noexcept -> std::size_t {
                if constexpr (std::is_same_v<cursor_type_, ustore_docs_cursor_t>)
                    return pull_keyed_(&ustore_docs_cursor_next, cursor, wanted, arena, batch, outcome);
                else if constexpr (std::is_same_v<cursor_type_, ustore_pages_cursor_t>)
                    return pull_keyed_(&ustore_pages_cursor_next, cursor, wanted, arena, batch, outcome);
                else if constexpr (std::is_same_v<cursor_type_, ustore_strings_cursor_t>)
                    return pull_strings_(cursor, wanted, arena, batch, outcome);
                else if constexpr (std::is_same_v<cursor_type_, ustore_graph_vertices_cursor_t>)
                    return next_edges_(cursor, wanted, arena, batch, outcome);
                else return pull_keyed_(&ustore_blobs_cursor_next, cursor, wanted, arena, batch, outcome);
            },
            cursor_);
    }

  private:

    /** Pulls the edges leaving the next pages of up to @p wanted vertices, skipping pages whose vertices leave none,
     *  and exports their sources, targets and identifiers out of the triples. */
    [[nodiscard]] std::size_t next_edges_(ustore_graph_vertices_cursor_t cursor, ustore_size_t wanted,
                                          ustore_arena_t arena, export_batch_t& batch,
                                          ustore_outcome_t* outcome) noexcept {
        for (;;) {
            ustore_key_t const* vertices = nullptr;
            ustore_size_t written = 0;
            ustore_graph_vertices_cursor_next_t next {.database = database_,
                                                      .outcome = outcome,
                                                      .cursor = cursor,
                                                      .keys = &vertices,
                                                      .written = &written,
                                                      .arena = vertices_arena_.get(),
                                                      .wanted = wanted};
            ustore_graph_vertices_cursor_next(&next);
            if (failed(outcome) || !written) return 0;
            ustore_vertex_role_t const role = ustore_vertex_role_source_k;
            ustore_vertex_degree_t const* degrees = nullptr;
            ustore_length_t const* offsets = nullptr;
            ustore_key_t const* triples = nullptr;
            ustore_graph_read_edges_t read {.database = database_,
                                            .outcome = outcome,
                                            .view = view_,
                                            .count = written,
                                            .collections = {&collection_, 0},
                                            .vertices = {vertices, sizeof(ustore_key_t)},
                                            .roles = {&role, 0},
                                            .degrees_per_vertex = &degrees,
                                            .edges_offsets = &offsets,
                                            .edges_per_vertex = &triples,
                                            .arena = arena};
            ustore_graph_read_edges(&read);
            if (failed(outcome)) return 0;
            std::size_t const edges = offsets ? static_cast<std::size_t>(offsets[written]) : 0;
            if (!edges && written == wanted) continue;
            if (!edges) return 0;
            for (std::size_t column = 0; column != 3; ++column)
                batch.add(ustore_keys_view_t {triples + column, 3 * sizeof(ustore_key_t)}, edges, arena, outcome);
            return edges;
        }
    }
};

#pragma endregion Arrow

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
    st::basic_vector<std::string_view> fields;
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
    st::basic_vector<std::string_view> fields;
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

void ustore_dataset_import(ustore_dataset_import_t* task_pointer) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;
    ustore_dataset_import_t const& task = *task_pointer;
    if (task.written) *task.written = 0;

    if (!task.database) {
        *task.outcome = {ustore_status_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (task.ingestion != ustore_dataset_ingestion_batched_k && task.ingestion != ustore_dataset_ingestion_sorted_k &&
        task.ingestion != ustore_dataset_ingestion_unsorted_k) {
        *task.outcome = {ustore_status_invalid_argument_k, "Unknown ingestion"};
        return;
    }
    sz::string_t path;
    copy_path(task.path, path, task.outcome);
    if (failed(task.outcome)) return;
    ustore_dataset_format_t format = format_of(path.view(), task.format);
    if (format == ustore_dataset_format_ndjson_k || format == ustore_dataset_format_csv_k) {
        *task.outcome = {ustore_status_invalid_argument_k, "NDJSON and CSV import through ustore_docs_import"};
        return;
    }
    // Standard input names no format, so it is sniffed: an Arrow file when it starts with the file's magic
    bool const sniffed = format == ustore_dataset_format_automatic_k && path.view() == "-";
    if (format != ustore_dataset_format_arrow_stream_k && format != ustore_dataset_format_arrow_file_k && !sniffed) {
        *task.outcome = {ustore_status_unsupported_k, "Unsupported file format, expected .arrows, .arrow or .feather"};
        return;
    }

    input_bytes_t input;
    input.open(path.c_str(), task.outcome);
    if (failed(task.outcome)) return;
    std::string_view const magic = ipc_file_magic_k.substr(0, 6);
    if (sniffed) {
        std::span<std::byte const> const head = input.peek(magic.size(), task.outcome);
        if (failed(task.outcome)) return;
        bool const is_file = head.size() == magic.size() && std::memcmp(head.data(), magic.data(), magic.size()) == 0;
        format = is_file ? ustore_dataset_format_arrow_file_k : ustore_dataset_format_arrow_stream_k;
    }
    ipc_input_stream_t state;
    state.input = &input;
    if (format == ustore_dataset_format_arrow_file_k) {
        std::span<std::byte const> const head = input.take(ipc_file_magic_k.size(), state.metadata, task.outcome);
        if (failed(task.outcome)) return;
        if (head.size() != ipc_file_magic_k.size() || std::memcmp(head.data(), magic.data(), magic.size()) != 0) {
            *task.outcome = {ustore_status_malformed_k, "The Arrow file does not start with its magic"};
            return;
        }
    }

    ipc_message_t message;
    if (!next_message(input, state.schema_metadata, message, task.outcome)) {
        if (!failed(task.outcome)) *task.outcome = {ustore_status_malformed_k, "The Arrow input holds no schema"};
        return;
    }
    if (message.kind != ipc_message_kind_t::schema_k) {
        *task.outcome = {ustore_status_malformed_k, "The Arrow input starts with a batch, not a schema"};
        return;
    }
    if (ipc_status_t const status = ipc_read_schema(message, state.fields, state.pairs, state.schema);
        status != ipc_status_t::success_k) {
        *task.outcome = outcome_of(status);
        return;
    }

    std::string_view const name = task.collection_name.length ? string_of(task.collection_name)
                                                              : metadata_value(state.schema.metadata, collection_key_k);
    if (name.empty()) {
        *task.outcome = {ustore_status_invalid_argument_k,
                         "Neither the call nor the schema's ustore.collection names it"};
        return;
    }
    ustore_collection_id_t collection {};
    ustore_modality_t const existing = find_named_collection(task.database, name, collection, task.outcome);
    if (failed(task.outcome)) return;
    ustore_modality_t const wanted = task.modality ? task.modality : existing ? existing : modality_of(state.schema);
    if (!wanted) {
        *task.outcome = {ustore_status_invalid_argument_k,
                         "Neither the schema's ustore.modality nor its columns name a modality, so name one"};
        return;
    }
    if (existing && existing != wanted) {
        *task.outcome = {ustore_status_invalid_argument_k, "The collection holds another modality"};
        return;
    }
    if (!existing) {
        ustore_collection_create_t create {};
        create.database = task.database;
        create.outcome = task.outcome;
        create.name = bytes_of(name);
        create.modality = wanted;
        create.collection = &collection;
        ustore_collection_create(&create);
        if (failed(task.outcome)) return;
    }

    ArrowArrayStream stream {&input_get_schema, &input_get_next, &input_get_last_error, &input_release, &state};
    ArrowSchema schema {};
    if (stream.get_schema(&stream, &schema) != 0) *task.outcome = state.failure;
    modality_columns_t columns;
    if (!failed(task.outcome)) [[maybe_unused]]
        bool const resolved = resolve_columns(schema, wanted, columns, task.outcome);
    ustore_size_t written = 0;
    if (!failed(task.outcome) && task.ingestion == ustore_dataset_ingestion_batched_k) {
        // One batch is pulled ahead, so the last write knows it is the one paying for the durability
        ArrowArray current {}, upcoming {};
        if (stream.get_next(&stream, &current) != 0) *task.outcome = state.failure;
        while (!failed(task.outcome) && current.release) {
            if (stream.get_next(&stream, &upcoming) != 0) *task.outcome = state.failure;
            batch_views_t rows;
            if (!failed(task.outcome) && rows_of(schema, current, columns, rows, task.outcome))
                write_rows(task.database, collection, wanted, rows,
                           upcoming.release ? ustore_durability_buffered_k : task.durability, task.outcome);
            if (!failed(task.outcome)) written += rows.count;
            current.release(&current);
            current = upcoming, upcoming = {};
        }
        if (current.release) current.release(&current);
    }
    else if (!failed(task.outcome)) {
        ustore_ingest_t ingest = nullptr;
        ustore_ingest_init_t init {.ingest = &ingest,
                                   .database = task.database,
                                   .outcome = task.outcome,
                                   .collection = collection,
                                   .order = task.ingestion == ustore_dataset_ingestion_sorted_k
                                                ? ustore_ingest_sorted_k
                                                : ustore_ingest_unsorted_k};
        ustore_ingest_init(&init);
        while (!failed(task.outcome)) {
            ArrowArray batch {};
            if (stream.get_next(&stream, &batch) != 0) *task.outcome = state.failure;
            if (failed(task.outcome) || !batch.release) break;
            batch_views_t rows;
            if (rows_of(schema, batch, columns, rows, task.outcome)) append_rows(ingest, rows, task.outcome);
            if (!failed(task.outcome)) written += rows.count;
            batch.release(&batch);
        }
        if (!failed(task.outcome)) {
            ustore_ingest_finish_t finish {.ingest = ingest, .outcome = task.outcome, .durability = task.durability};
            ustore_ingest_finish(&finish);
        }
        ustore_ingest_free(ingest);
        // An ingest lands all of the stream or none of it, so a collection made for it goes with it
        if (failed(task.outcome) && !existing) {
            ustore_outcome_t dropped {};
            ustore_collection_drop_t drop {};
            drop.database = task.database;
            drop.outcome = &dropped;
            drop.collection = collection;
            drop.mode = ustore_drop_collection_k;
            ustore_collection_drop(&drop);
        }
    }
    if (schema.release) schema.release(&schema);
    if (!failed(task.outcome) && task.written) *task.written = written;
}

void ustore_dataset_export(ustore_dataset_export_t* task_pointer) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;
    ustore_dataset_export_t const& task = *task_pointer;
    if (task.written) *task.written = 0;

    if (!task.database) {
        *task.outcome = {ustore_status_uninitialized_k, "Database is uninitialized"};
        return;
    }
    ustore_size_t const alignment = task.alignment ? task.alignment : ipc_alignment_default_k;
    if (alignment != ipc_alignment_default_k && alignment != ipc_alignment_direct_k) {
        *task.outcome = {ustore_status_invalid_argument_k, "Arrow buffers align to 64 or 4096 bytes"};
        return;
    }
    sz::string_t path;
    copy_path(task.path, path, task.outcome);
    if (failed(task.outcome)) return;
    ustore_dataset_format_t format = format_of(path.view(), task.format);
    if (format == ustore_dataset_format_automatic_k && path.view() == "-")
        format = ustore_dataset_format_arrow_stream_k;
    if (format == ustore_dataset_format_ndjson_k || format == ustore_dataset_format_csv_k) {
        *task.outcome = {ustore_status_invalid_argument_k, "NDJSON and CSV export through ustore_docs_export"};
        return;
    }
    if (format != ustore_dataset_format_arrow_stream_k && format != ustore_dataset_format_arrow_file_k) {
        *task.outcome = {ustore_status_unsupported_k, "Unsupported file format, expected .arrows, .arrow or .feather"};
        return;
    }

    // A head export reads one snapshot of its own, as a head cursor reads a removed key as an empty value
    ustore_snapshot_t snapshot = nullptr;
    ustore_view_t view = task.view;
    if (!view) {
        ustore_snapshot_init_t init {.snapshot = &snapshot, .database = task.database, .outcome = task.outcome};
        ustore_snapshot_init(&init);
        view = ustore_snapshot_view(snapshot);
    }
    ustore_modality_t const modality = failed(task.outcome)
                                           ? ustore_modality_t {}
                                           : listed_modality(task.database, snapshot, task.collection, task.outcome);
    std::array<ipc_key_value_t, 2> metadata {
        {{modality_key_k, failed(task.outcome) ? "" : ustore_modality_name(modality)},
         {collection_key_k, string_of(task.collection_name)}}};
    ipc_schema_t const schema {export_fields(modality), std::span(metadata).first(task.collection_name.length ? 2 : 1)};

    output_descriptor_t output;
    if (!failed(task.outcome)) output.open(path.c_str(), task.outcome);
    ipc_output_t writer {output.get(), format, {static_cast<std::uint32_t>(alignment)}};
    if (!failed(task.outcome)) writer.begin(schema, task.outcome);
    ustore_size_t rows = 0;
    {
        export_walk_t walk {task.database, view, task.collection, modality, task.outcome};
        task_arena_t const arena {nullptr, task.outcome};
        ustore_size_t const wanted = task.batch_rows ? task.batch_rows : import_chunk_rows_k;
        while (!failed(task.outcome)) {
            export_batch_t batch;
            std::size_t const count = walk.next(wanted, arena.get(), batch, task.outcome);
            if (failed(task.outcome) || !count) break;
            std::array<ArrowArray*, 3> children {};
            for (std::size_t index = 0; index != batch.count; ++index) children[index] = &batch.columns[index].array;
            std::array<void const*, 1> root_buffers {};
            ArrowArray const root {.length = static_cast<std::int64_t>(count),
                                   .n_buffers = 1,
                                   .n_children = static_cast<std::int64_t>(batch.count),
                                   .buffers = root_buffers.data(),
                                   .children = children.data(),
                                   .release = batch.columns[0].array.release};
            std::array<ipc_node_t, ipc_fields_max_k> nodes {};
            std::array<std::span<std::byte const>, ipc_buffers_max_k> buffers {};
            ipc_batch_t decoded {};
            if (failed(task.outcome)) break;
            if (ipc_status_t const status = ipc_batch_from_arrow(schema, root, nodes, buffers, decoded);
                status != ipc_status_t::success_k)
                *task.outcome = outcome_of(status);
            else writer.append(schema, decoded, task.outcome);
            rows += count;
        }
    }
    if (!failed(task.outcome)) writer.finish(schema, task.outcome);
    ustore_snapshot_free(snapshot);
    output.close(task.outcome);
    if (!failed(task.outcome) && task.written) *task.written = rows;
}

#pragma endregion C Interface
