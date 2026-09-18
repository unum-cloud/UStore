/**
 *  @file tests/test_tools.cpp
 *  @author Ash Vardanian
 *  @date April 16, 2023
 *  @brief Tests importing and exporting documents and graph edges through NDJSON and CSV files, and the CLI on top.
 */

#include <cstddef>    // `std::size_t`
#include <cstdint>    // `std::int64_t`
#include <cstdio>     // `stderr`
#include <cstdlib>    // `std::free`
#include <cstring>    // `std::strcmp`
#include <fcntl.h>    // `open`
#include <sys/stat.h> // `fstat`
#include <sys/wait.h> // `waitpid`
#include <unistd.h>   // `close`, `execl`, `fork`, `read`, `unlink`, `write`

#include <algorithm>    // `std::sort`
#include <array>        // `std::array`
#include <charconv>     // `std::from_chars`, `std::to_chars`
#include <concepts>     // `std::same_as`
#include <iterator>     // `std::size`
#include <span>         // `std::span`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::errc`

#include <fmt/format.h>
#include <yyjson.h>

#include <smashtable/basic_vector.hpp>
#include <stringzilla/stringzilla.hpp>

#include <ustore/db.h>
#include <ustore/strings.h>

#include "dataset.h"
#include "test.hpp"

namespace unum::ustore {

constexpr std::int64_t rows_count_k = 1000;
constexpr ustore_size_t max_batch_size_k = 64 * 1024;

constexpr char const* documents_ndjson_k = "documents.ndjson";
constexpr char const* documents_csv_k = "documents.csv";
constexpr char const* edges_ndjson_k = "edges.ndjson";
constexpr char const* edges_csv_k = "edges.csv";
constexpr char const* exported_ndjson_k = "exported.ndjson";
constexpr char const* exported_csv_k = "exported.csv";
constexpr char const* exported_parquet_k = "exported.parquet";

constexpr ustore_str_view_t selected_paths_k[] = {
    "id",
    "id_str",
    "/user/id",
    "/user/followers_count",
    "/quoted_status/id",
    "/quoted_status/user",
    "/quoted_status/entities/hashtags",
    "/quoted_status/entities/media/0/id",
};

constexpr ustore_str_view_t selected_columns_k[] = {"id", "id_str", "user_id", "geo_type"};

constexpr ustore_str_view_t source_field_k = "id";
constexpr ustore_str_view_t target_field_k = "user_id";
constexpr ustore_str_view_t edge_field_k = "user_followers_count";

/** Whether a document case keeps whole rows or only selected fields. */
enum class field_selection_t : bool {
    whole_k,
    selected_k,
};

#pragma region Fixtures

/** Where the database and every file of the suite live, all under the test directory. */
struct fixture_paths_t {

    /** Directory the database opens in. */
    char const* directory;

    /** Subdirectory holding the fixtures and the exports. */
    test_path_t files;

    /** Documents as NDJSON. */
    test_path_t documents_ndjson;

    /** Documents as CSV. */
    test_path_t documents_csv;

    /** Edges as NDJSON. */
    test_path_t edges_ndjson;

    /** Edges as CSV. */
    test_path_t edges_csv;

    /** Export target in NDJSON. */
    test_path_t exported_ndjson;

    /** Export target in CSV. */
    test_path_t exported_csv;

    /** Names every path under @p test_directory. */
    explicit fixture_paths_t(char const* test_directory) noexcept
        : directory(test_directory), files(test_directory, "files"),
          documents_ndjson(files.c_str(), documents_ndjson_k), documents_csv(files.c_str(), documents_csv_k),
          edges_ndjson(files.c_str(), edges_ndjson_k), edges_csv(files.c_str(), edges_csv_k),
          exported_ndjson(files.c_str(), exported_ndjson_k), exported_csv(files.c_str(), exported_csv_k) {}
};

/** Appends @p piece to @p text, answering whether it fit. */
[[nodiscard]] bool append(sz::string& text, std::string_view piece) noexcept {
    return text.try_append(piece.data(), piece.size());
}

/** Appends the decimal digits of @p number to @p text, answering whether they fit. */
[[nodiscard]] bool append(sz::string& text, std::int64_t number) noexcept {
    char digits[24];
    std::to_chars_result const printed = std::to_chars(digits, digits + sizeof(digits), number);
    return text.try_append(digits, static_cast<std::size_t>(printed.ptr - digits));
}

/** Appends the NDJSON fixture document with key @p id, without a line break. */
[[nodiscard]] bool append_ndjson_document(sz::string& text, std::int64_t id) noexcept {
    std::int64_t const user = id % 97;
    return append(text, R"({"id":)") && append(text, id) && append(text, R"(,"id_str":")") && append(text, id) &&
           append(text, R"(","user":{"id":)") && append(text, user) && append(text, R"(,"followers_count":)") &&
           append(text, id * 3) && append(text, R"(},"quoted_status":{"id":)") && append(text, id + 100000) &&
           append(text, R"(,"user":{"id":)") && append(text, user) && append(text, R"(,"name":"user-)") &&
           append(text, user) && append(text, R"("},"entities":{"hashtags":["tag-)") && append(text, id % 5) &&
           append(text, R"(","tag-)") && append(text, id % 7) && append(text, R"("],"media":[{"id":)") &&
           append(text, id * 7) && append(text, R"(,"sizes":{"small":{"w":340,"h":340}}}]}},"favorited":false})");
}

/** Appends the CSV fixture row with key @p id, line break included. */
[[nodiscard]] bool append_csv_row(sz::string& text, std::int64_t id) noexcept {
    std::string_view const geo = id % 3 == 0 ? R"("Point, ""exact""")" : id % 3 == 1 ? "Point" : "";
    return append(text, id) && append(text, ",id-") && append(text, id) && append(text, ",") && append(text, id % 97) &&
           append(text, ",") && append(text, id * 3) && append(text, ",") && append(text, geo) &&
           append(text, ",false,true\n");
}

/** Appends the JSON document an import makes of the CSV fixture row with key @p id. */
[[nodiscard]] bool append_csv_document(sz::string& text, std::int64_t id) noexcept {
    std::string_view const geo = id % 3 == 0 ? R"("Point, \"exact\"")" : id % 3 == 1 ? R"("Point")" : "null";
    return append(text, R"({"id":)") && append(text, id) && append(text, R"(,"id_str":"id-)") && append(text, id) &&
           append(text, R"(","user_id":)") && append(text, id % 97) && append(text, R"(,"user_followers_count":)") &&
           append(text, id * 3) && append(text, R"(,"geo_type":)") && append(text, geo) &&
           append(text, R"(,"favorited":false,"retweeted":true})");
}

/** Writes @p content to the file at @p path, answering whether every byte landed. */
[[nodiscard]] bool write_file(char const* path, sz::string const& content) noexcept {
    int const descriptor = open(path, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
    if (descriptor == -1) return false;
    char const* cursor = content.data();
    std::size_t remaining = content.size();
    while (remaining != 0) {
        ssize_t const written = write(descriptor, cursor, remaining);
        if (written <= 0) break;
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return close(descriptor) == 0 && remaining == 0;
}

/** Reads the whole file at @p path into @p content, answering whether it loaded. */
[[nodiscard]] bool read_file(char const* path, sz::string& content) noexcept {
    int const descriptor = open(path, O_RDONLY);
    if (descriptor == -1) return false;
    struct stat metadata {};
    bool loaded = fstat(descriptor, &metadata) == 0 && content.try_resize(static_cast<std::size_t>(metadata.st_size));
    std::size_t filled = 0;
    while (loaded && filled != content.size()) {
        ssize_t const got = read(descriptor, content.data() + filled, content.size() - filled);
        loaded = got > 0;
        filled += loaded ? static_cast<std::size_t>(got) : 0;
    }
    [[maybe_unused]] int const closed = close(descriptor);
    return loaded;
}

/** Writes the document and edge fixtures, in NDJSON and CSV, where @p paths names them. */
[[nodiscard]] bool write_fixtures(fixture_paths_t const& paths) noexcept {
    sz::string documents_ndjson;
    sz::string documents_csv;
    sz::string edges_ndjson;
    sz::string edges_csv;
    bool written = append(documents_csv, "id,id_str,user_id,user_followers_count,geo_type,favorited,retweeted\n") &&
                   append(edges_csv, "id,user_id,user_followers_count\n");
    for (std::int64_t id = 1; written && id <= rows_count_k; ++id) {
        written = append_ndjson_document(documents_ndjson, id) && append(documents_ndjson, "\n") &&
                  append_csv_row(documents_csv, id) && append(edges_ndjson, R"({"id":)") && append(edges_ndjson, id) &&
                  append(edges_ndjson, R"(,"user_id":)") && append(edges_ndjson, id % 97) &&
                  append(edges_ndjson, R"(,"user_followers_count":)") && append(edges_ndjson, id * 3) &&
                  append(edges_ndjson, "}\n") && append(edges_csv, id) && append(edges_csv, ",") &&
                  append(edges_csv, id % 97) && append(edges_csv, ",") && append(edges_csv, id * 3) &&
                  append(edges_csv, "\n");
    }
    return written && create_directories(paths.files.c_str()) &&
           write_file(paths.documents_ndjson.c_str(), documents_ndjson) &&
           write_file(paths.documents_csv.c_str(), documents_csv) &&
           write_file(paths.edges_ndjson.c_str(), edges_ndjson) && write_file(paths.edges_csv.c_str(), edges_csv);
}

/** Whether @p path names a CSV file. */
[[nodiscard]] bool is_csv(char const* path) noexcept { return std::string_view {path}.ends_with(".csv"); }

#pragma endregion Fixtures

#pragma region Helpers

/** A database opened in one directory, freed on destruction. */
class test_database_t {

    /** The open database, or null until @c open succeeds. */
    ustore_database_t database_ = nullptr;

  public:
    test_database_t() noexcept = default;
    test_database_t(test_database_t const&) = delete;
    test_database_t& operator=(test_database_t const&) = delete;
    ~test_database_t() noexcept {
        if (database_) ustore_database_free(database_);
    }

    /** Opens the database in @p directory and answers whether it opened. */
    [[nodiscard]] bool open(char const* directory) noexcept {
        test_config_t const config {directory};
        ustore_error_t error {};
        ustore_database_init_t opening {};
        opening.config = config.c_str();
        opening.db = &database_;
        opening.error = &error;
        ustore_database_init(&opening);
        if (!error.status) return true;
        fmt::println(stderr, "  failed to open the database: {}", error.message);
        return false;
    }

    /** Answers whether the engine reports named collections. */
    [[nodiscard]] bool supports_collections() noexcept {
        ustore_error_t error {};
        ustore_metadata_t metadata {};
        ustore_get_metadata_t request {};
        request.db = database_;
        request.error = &error;
        request.metadata = &metadata;
        ustore_get_metadata(&request);
        bool const supported = !error.status && (metadata & ustore_supports_collections_k);
        return supported;
    }

    /** Removes every key and value of the main collection. */
    void clear() noexcept {
        ustore_error_t error {};
        ustore_collection_drop_t drop {};
        drop.db = database_;
        drop.error = &error;
        drop.id = ustore_collection_main_k;
        drop.mode = ustore_drop_keys_vals_k;
        ustore_collection_drop(&drop);
    }

    /** The open database handle. */
    [[nodiscard]] ustore_database_t get() const noexcept { return database_; }
};

/** Owns a parsed yyjson document. */
struct json_document_t {

    /** The parsed document, or null when parsing failed. */
    yyjson_doc* handle = nullptr;

    /** Takes ownership of @p document. */
    explicit json_document_t(yyjson_doc* document) noexcept : handle(document) {}
    json_document_t(json_document_t const&) = delete;
    json_document_t& operator=(json_document_t const&) = delete;
    ~json_document_t() noexcept { yyjson_doc_free(handle); }
};

/** Finds @p field in @p object, as a JSON pointer when it starts with a slash and as a key otherwise. */
[[nodiscard]] yyjson_val* lookup(yyjson_val* object, ustore_str_view_t field) noexcept {
    return field[0] == '/' ? yyjson_ptr_get(object, field) : yyjson_obj_get(object, field);
}

/** Serializes @p value minified into @p output, so equal values of equal key order compare equal as text. */
[[nodiscard]] bool canonical(yyjson_val* value, sz::string& output) noexcept {
    output.clear();
    if (!value) return true;
    yyjson_mut_doc* const builder = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* const copy = builder ? yyjson_val_mut_copy(builder, value) : nullptr;
    std::size_t length = 0;
    char* text = nullptr;
    if (copy) {
        yyjson_mut_doc_set_root(builder, copy);
        text = yyjson_mut_write(builder, 0, &length);
    }
    bool const serialized = text && output.try_append(text, length);
    std::free(text);
    yyjson_mut_doc_free(builder);
    return serialized;
}

/** Parses all of @p text as a decimal key into @p key, answering whether it parsed. */
[[nodiscard]] bool parse_key(std::string_view text, ustore_key_t& key) noexcept {
    std::from_chars_result const parsed = std::from_chars(text.data(), text.data() + text.size(), key);
    return !text.empty() && parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size();
}

/** Whether @p database holds a collection named @p name, which the strings modality resolves. */
[[nodiscard]] bool lists_collection(ustore_database_t database, char const* name) noexcept {
    ustore_arena_t arena = nullptr;
    ustore_error_t error {};
    ustore_collection_t const names = ustore_collection_names_k;
    ustore_str_view_t const text = name;
    auto const text_length = static_cast<ustore_length_t>(std::strlen(name));
    ustore_length_t* lengths = nullptr;
    ustore_strings_read_t read {};
    read.db = database;
    read.error = &error;
    read.arena = &arena;
    read.tasks_count = 1;
    read.collections = &names;
    read.strings = &text;
    read.strings_lengths = &text_length;
    read.lengths = &lengths;
    ustore_strings_read(&read);
    bool const found = !error.status && lengths[0] == sizeof(ustore_collection_t);
    ustore_arena_free(arena);
    return found;
}

#pragma endregion Helpers

#pragma region Checks

/** One exported edge, compared after sorting. */
struct edge_row_t {
    ustore_key_t source;
    ustore_key_t target;
    ustore_key_t edge;
};

/** Checks that the edges exported to @p path are exactly the fixture edges. */
[[nodiscard]] std::size_t check_edges(char const* path) noexcept {
    std::size_t failures = 0;
    sz::string content;
    st::basic_vector<edge_row_t> edges;
    if (!read_file(path, content)) return expect(!"the exported edges are readable");

    std::size_t line_index = 0;
    for (sz::string_view line : sz::split(content.view(), "\n")) {
        if (line.size() == 0 || (is_csv(path) && line_index++ == 0)) continue;
        edge_row_t edge {};
        bool parsed = false;
        if (is_csv(path)) {
            std::size_t const first = line.find(',');
            std::size_t const second = first >= line.size() ? line.size() : line.find(',', first + 1);
            std::string_view const row = line;
            parsed = second < line.size() && parse_key(row.substr(0, first), edge.source) &&
                     parse_key(row.substr(first + 1, second - first - 1), edge.target) &&
                     parse_key(row.substr(second + 1), edge.edge);
        }
        else {
            json_document_t const document {yyjson_read(line.data(), line.size(), 0)};
            yyjson_val* const root = yyjson_doc_get_root(document.handle);
            yyjson_val* const source = yyjson_obj_get(root, source_field_k);
            yyjson_val* const target = yyjson_obj_get(root, target_field_k);
            yyjson_val* const label = yyjson_obj_get(root, edge_field_k);
            parsed = yyjson_is_int(source) && yyjson_is_int(target) && yyjson_is_int(label);
            edge = {yyjson_get_sint(source), yyjson_get_sint(target), yyjson_get_sint(label)};
        }
        failures += expect(parsed);
        if (!parsed || st::failed(edges.push_back(edge_row_t {edge}))) return failures + 1;
    }

    std::sort(edges.begin(), edges.end(),
              [](edge_row_t const& left, edge_row_t const& right) noexcept { return left.source < right.source; });
    failures += expect_eq(edges.size(), static_cast<std::size_t>(rows_count_k));
    for (std::size_t index = 0; index != edges.size(); ++index) {
        ustore_key_t const id = static_cast<ustore_key_t>(index) + 1;
        edge_row_t const& edge = edges[index];
        if (edge.source == id && edge.target == id % 97 && edge.edge == id * 3) continue;
        failures += expect_eq(edge.source, id);
        failures += expect_eq(edge.target, id % 97);
        failures += expect_eq(edge.edge, id * 3);
        break;
    }
    return failures;
}

/** Checks one exported document against the fixture row of its key. */
[[nodiscard]] std::size_t check_document(ustore_key_t key, std::string_view text, char const* input,
                                         field_selection_t selection) noexcept {
    std::size_t failures = expect(key >= 1 && key <= rows_count_k);
    sz::string expected_text;
    bool const generated = is_csv(input) ? append_csv_document(expected_text, key)
                                         : append_ndjson_document(expected_text, key);
    if (!generated) return failures + 1;

    json_document_t const exported {yyjson_read(text.data(), text.size(), 0)};
    json_document_t const expected {yyjson_read(expected_text.data(), expected_text.size(), 0)};
    yyjson_val* const exported_root = yyjson_doc_get_root(exported.handle);
    yyjson_val* const expected_root = yyjson_doc_get_root(expected.handle);
    failures += expect(yyjson_is_obj(exported_root));

    sz::string exported_value;
    sz::string expected_value;
    if (selection == field_selection_t::whole_k) {
        failures += expect(canonical(exported_root, exported_value) && canonical(expected_root, expected_value));
        failures += expect(std::string_view(exported_value.data(), exported_value.size()) ==
                           std::string_view(expected_value.data(), expected_value.size()));
        return failures;
    }

    std::span<ustore_str_view_t const> const fields = is_csv(input)
                                                          ? std::span<ustore_str_view_t const>(selected_columns_k)
                                                          : std::span<ustore_str_view_t const>(selected_paths_k);
    for (ustore_str_view_t field : fields) {
        failures += expect(canonical(lookup(exported_root, field), exported_value) &&
                           canonical(lookup(expected_root, field), expected_value));
        failures += expect(exported_value.size() != 0);
        failures += expect(std::string_view(exported_value.data(), exported_value.size()) ==
                           std::string_view(expected_value.data(), expected_value.size()));
    }
    failures += expect(yyjson_obj_get(exported_root, is_csv(input) ? "retweeted" : "favorited") == nullptr);
    return failures;
}

/** Checks that the documents exported to @p output match the fixture rows of @p input, one per key. */
[[nodiscard]] std::size_t check_documents(char const* output, char const* input, field_selection_t selection) noexcept {
    std::size_t failures = 0;
    sz::string content;
    if (!read_file(output, content)) return expect(!"the exported documents are readable");

    std::size_t rows = 0;
    sz::string unescaped;
    for (sz::string_view line : sz::split(content.view(), "\n")) {
        if (line.size() == 0) continue;
        std::string_view const row = line;
        if (is_csv(output)) {
            if (row == "_id,doc") continue;
            std::size_t const comma = row.find(',');
            ustore_key_t key = 0;
            bool const quoted = comma != std::string_view::npos && row.size() >= comma + 3 && row[comma + 1] == '"' &&
                                row.back() == '"';
            failures += expect(quoted && parse_key(row.substr(0, comma), key));
            if (!quoted) continue;
            unescaped.clear();
            std::string_view separator;
            sz::string_view const inner {row.data() + comma + 2, row.size() - comma - 3};
            for (sz::string_view segment : sz::split(inner, "\"\"")) {
                failures += expect(append(unescaped, separator) && unescaped.try_append(segment));
                separator = "\"";
            }
            failures += check_document(key, unescaped, input, selection);
        }
        else {
            json_document_t const document {yyjson_read(row.data(), row.size(), 0)};
            yyjson_val* const root = yyjson_doc_get_root(document.handle);
            yyjson_val* const key = yyjson_obj_get(root, "_id");
            sz::string text;
            failures += expect(yyjson_is_int(key) && canonical(yyjson_obj_get(root, "doc"), text));
            if (!yyjson_is_int(key)) continue;
            failures += check_document(yyjson_get_sint(key), text, input, selection);
        }
        ++rows;
    }
    failures += expect_eq(rows, static_cast<std::size_t>(rows_count_k));
    return failures;
}

#pragma endregion Checks

#pragma region Round Trips

/** Imports edges from @p input, exports them to @p output, and checks the export holds exactly the fixture edges. */
[[nodiscard]] std::size_t graph_round_trip(char const* directory, char const* input, char const* output) noexcept {
    test_database_t database;
    if (!database.open(directory)) return 1;
    database.clear();

    std::size_t failures = 0;
    ustore_error_t error {};
    ustore_graph_import_t importing {};
    importing.db = database.get();
    importing.error = &error;
    importing.collection = ustore_collection_main_k;
    importing.path = input;
    importing.max_batch_size = max_batch_size_k;
    importing.source_id_field = source_field_k;
    importing.target_id_field = target_field_k;
    importing.edge_id_field = edge_field_k;
    ustore_graph_import(&importing);
    failures += expect_no_error(error);

    ustore_graph_export_t exporting {};
    exporting.db = database.get();
    exporting.error = &error;
    exporting.collection = ustore_collection_main_k;
    exporting.path = output;
    exporting.max_batch_size = max_batch_size_k;
    exporting.source_id_field = source_field_k;
    exporting.target_id_field = target_field_k;
    exporting.edge_id_field = edge_field_k;
    ustore_graph_export(&exporting);
    failures += expect_no_error(error);

    failures += check_edges(output);
    [[maybe_unused]] int const removed = unlink(output);
    database.clear();
    return failures;
}

/** Imports documents from @p input, exports them to @p output, and checks the rows or fields @p selection keeps. */
[[nodiscard]] std::size_t documents_round_trip(char const* directory, char const* input, char const* output,
                                               field_selection_t selection) noexcept {
    test_database_t database;
    if (!database.open(directory)) return 1;
    database.clear();

    std::span<ustore_str_view_t const> const fields = is_csv(input)
                                                          ? std::span<ustore_str_view_t const>(selected_columns_k)
                                                          : std::span<ustore_str_view_t const>(selected_paths_k);
    std::size_t const fields_count = selection == field_selection_t::selected_k ? fields.size() : 0;

    std::size_t failures = 0;
    ustore_error_t error {};
    ustore_docs_import_t importing {};
    importing.db = database.get();
    importing.error = &error;
    importing.collection = ustore_collection_main_k;
    importing.path = input;
    importing.max_batch_size = max_batch_size_k;
    importing.fields_count = fields_count;
    importing.fields = fields_count ? fields.data() : nullptr;
    importing.fields_stride = sizeof(ustore_str_view_t);
    importing.id_field = "id";
    ustore_docs_import(&importing);
    failures += expect_no_error(error);

    ustore_docs_export_t exporting {};
    exporting.db = database.get();
    exporting.error = &error;
    exporting.collection = ustore_collection_main_k;
    exporting.path = output;
    exporting.max_batch_size = max_batch_size_k;
    exporting.fields_count = fields_count;
    exporting.fields = fields_count ? fields.data() : nullptr;
    exporting.fields_stride = sizeof(ustore_str_view_t);
    ustore_docs_export(&exporting);
    failures += expect_no_error(error);

    failures += check_documents(output, input, selection);
    [[maybe_unused]] int const removed = unlink(output);
    database.clear();
    return failures;
}

#pragma endregion Round Trips

#pragma region Invalid Tasks

/** Graph import rejects a task without a path, source, target or database, and accepts one without edges. */
[[nodiscard]] std::size_t graph_import_rejects_invalid_tasks(char const* directory, char const* input) noexcept {
    test_database_t database;
    if (!database.open(directory)) return 1;
    database.clear();

    std::size_t failures = 0;
    ustore_error_t error {};
    ustore_graph_import_t valid {};
    valid.db = database.get();
    valid.error = &error;
    valid.collection = ustore_collection_main_k;
    valid.path = input;
    valid.max_batch_size = max_batch_size_k;
    valid.source_id_field = source_field_k;
    valid.target_id_field = target_field_k;
    valid.edge_id_field = edge_field_k;

    ustore_graph_import_t without_path = valid;
    without_path.path = nullptr;
    ustore_graph_import(&without_path);
    failures += expect_error(error);

    ustore_graph_import_t without_source = valid;
    without_source.source_id_field = nullptr;
    ustore_graph_import(&without_source);
    failures += expect_error(error);

    ustore_graph_import_t without_target = valid;
    without_target.target_id_field = nullptr;
    ustore_graph_import(&without_target);
    failures += expect_error(error);

    ustore_graph_import_t without_edge = valid;
    without_edge.edge_id_field = nullptr;
    ustore_graph_import(&without_edge);
    failures += expect_no_error(error);

    ustore_graph_import_t without_database = valid;
    without_database.db = nullptr;
    ustore_graph_import(&without_database);
    failures += expect_error(error);

    database.clear();
    return failures;
}

/** Graph export rejects a task without a path, source, target or database, or with a pointer field. */
[[nodiscard]] std::size_t graph_export_rejects_invalid_tasks(char const* directory, char const* output) noexcept {
    test_database_t database;
    if (!database.open(directory)) return 1;
    database.clear();

    std::size_t failures = 0;
    ustore_error_t error {};
    ustore_graph_export_t valid {};
    valid.db = database.get();
    valid.error = &error;
    valid.collection = ustore_collection_main_k;
    valid.path = output;
    valid.max_batch_size = max_batch_size_k;
    valid.source_id_field = source_field_k;
    valid.target_id_field = target_field_k;
    valid.edge_id_field = edge_field_k;

    ustore_graph_export_t without_path = valid;
    without_path.path = nullptr;
    ustore_graph_export(&without_path);
    failures += expect_error(error);

    ustore_graph_export_t without_source = valid;
    without_source.source_id_field = nullptr;
    ustore_graph_export(&without_source);
    failures += expect_error(error);

    ustore_graph_export_t without_target = valid;
    without_target.target_id_field = nullptr;
    ustore_graph_export(&without_target);
    failures += expect_error(error);

    ustore_graph_export_t with_pointer_field = valid;
    with_pointer_field.source_id_field = "/id";
    ustore_graph_export(&with_pointer_field);
    failures += expect_error(error);

    ustore_graph_export_t without_edge = valid;
    without_edge.edge_id_field = nullptr;
    ustore_graph_export(&without_edge);
    failures += expect_no_error(error);

    ustore_graph_export_t without_database = valid;
    without_database.db = nullptr;
    ustore_graph_export(&without_database);
    failures += expect_error(error);

    [[maybe_unused]] int const removed = unlink(output);
    return failures;
}

/** Document import rejects a task without a path, fields, stride, database or an identifier among the fields. */
[[nodiscard]] std::size_t docs_import_rejects_invalid_tasks(char const* directory, char const* input) noexcept {
    test_database_t database;
    if (!database.open(directory)) return 1;
    database.clear();

    std::size_t failures = 0;
    ustore_error_t error {};
    ustore_docs_import_t valid {};
    valid.db = database.get();
    valid.error = &error;
    valid.collection = ustore_collection_main_k;
    valid.path = input;
    valid.max_batch_size = max_batch_size_k;
    valid.fields_count = std::size(selected_columns_k);
    valid.fields = selected_columns_k;
    valid.fields_stride = sizeof(ustore_str_view_t);
    valid.id_field = "id";

    ustore_docs_import_t without_path = valid;
    without_path.path = nullptr;
    ustore_docs_import(&without_path);
    failures += expect_error(error);

    ustore_docs_import_t without_count = valid;
    without_count.fields_count = 0;
    ustore_docs_import(&without_count);
    failures += expect_error(error);

    ustore_docs_import_t without_fields = valid;
    without_fields.fields = nullptr;
    ustore_docs_import(&without_fields);
    failures += expect_error(error);

    ustore_docs_import_t without_stride = valid;
    without_stride.fields_stride = 0;
    ustore_docs_import(&without_stride);
    failures += expect_error(error);

    ustore_docs_import_t without_id_among_fields = valid;
    without_id_among_fields.id_field = "missing";
    ustore_docs_import(&without_id_among_fields);
    failures += expect_error(error);

    ustore_docs_import_t without_database = valid;
    without_database.db = nullptr;
    ustore_docs_import(&without_database);
    failures += expect_error(error);

    database.clear();
    return failures;
}

/** Document export rejects a task without a path, fields, stride or database, or with an unknown format. */
[[nodiscard]] std::size_t docs_export_rejects_invalid_tasks(char const* directory, char const* output) noexcept {
    test_database_t database;
    if (!database.open(directory)) return 1;
    database.clear();

    std::size_t failures = 0;
    ustore_error_t error {};
    ustore_docs_export_t valid {};
    valid.db = database.get();
    valid.error = &error;
    valid.collection = ustore_collection_main_k;
    valid.path = output;
    valid.max_batch_size = max_batch_size_k;
    valid.fields_count = std::size(selected_columns_k);
    valid.fields = selected_columns_k;
    valid.fields_stride = sizeof(ustore_str_view_t);

    ustore_docs_export_t without_path = valid;
    without_path.path = nullptr;
    ustore_docs_export(&without_path);
    failures += expect_error(error);

    ustore_docs_export_t without_count = valid;
    without_count.fields_count = 0;
    ustore_docs_export(&without_count);
    failures += expect_error(error);

    ustore_docs_export_t without_fields = valid;
    without_fields.fields = nullptr;
    ustore_docs_export(&without_fields);
    failures += expect_error(error);

    ustore_docs_export_t without_stride = valid;
    without_stride.fields_stride = 0;
    ustore_docs_export(&without_stride);
    failures += expect_error(error);

    test_path_t const unknown_format_output {directory, exported_parquet_k};
    ustore_docs_export_t with_unknown_format = valid;
    with_unknown_format.path = unknown_format_output.c_str();
    ustore_docs_export(&with_unknown_format);
    failures += expect_error(error);

    ustore_docs_export_t without_database = valid;
    without_database.db = nullptr;
    ustore_docs_export(&without_database);
    failures += expect_error(error);

    return failures;
}

#pragma endregion Invalid Tasks

#pragma region CLI

/** Runs @p command with @p arguments in a child process and answers its exit code, or -1 when it did not exit. */
template <typename... arguments_type_>
    requires(std::same_as<arguments_type_, char const*> && ...)
[[nodiscard]] int run_command(char const* command, arguments_type_... arguments) noexcept {
    pid_t const child = fork();
    if (child == -1) return -1;
    if (child == 0) {
        execl(command, command, arguments..., static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

/** Creates, imports into, exports and drops a named collection, then imports and exports the main one. */
[[nodiscard]] std::size_t cli_round_trip(fixture_paths_t const& paths, char const* cli) noexcept {
    std::size_t failures = 0;
    char const* const directory = paths.directory;
    char const* const collection = "collection1";
    char const* const input = paths.documents_ndjson.c_str();
    char const* const output = paths.exported_ndjson.c_str();
    bool supports_collections = false;
    {
        test_database_t database;
        failures += expect(database.open(directory));
        supports_collections = database.supports_collections();
    }

    if (supports_collections) {
        failures += expect_eq(
            run_command(cli, "--directory", directory, "collection", "create", "--collection", collection), 0);
        {
            test_database_t database;
            failures += expect(database.open(directory) && lists_collection(database.get(), collection));
        }

        failures += expect_eq(run_command(cli, "--directory", directory, "collection", "import", "--input", input,
                                          "--id-field", "id", "--memory-limit", "1MiB", "--collection", collection),
                              0);
        failures += expect_eq(run_command(cli, "--directory", directory, "collection", "export", "--output", output,
                                          "--memory-limit", "1MiB", "--collection", collection),
                              0);
        failures += check_documents(output, input, field_selection_t::whole_k);
        [[maybe_unused]] int const removed_named = unlink(output);

        failures += expect_eq(
            run_command(cli, "--directory", directory, "collection", "drop", "--collection", collection), 0);
        {
            test_database_t database;
            failures += expect(database.open(directory) && !lists_collection(database.get(), collection));
        }
    }

    failures += expect_eq(run_command(cli, "--directory", directory, "collection", "import", "--input", input,
                                      "--id-field", "id", "--memory-limit", "1MiB"),
                          0);
    failures += expect_eq(run_command(cli, "--directory", directory, "collection", "export", "--output", output,
                                      "--memory-limit", "1MiB"),
                          0);
    failures += check_documents(output, input, field_selection_t::whole_k);
    [[maybe_unused]] int const removed_main = unlink(output);
    return failures;
}

#pragma endregion CLI

/** Runs every case against the fixtures written under @p directory, and the CLI at @p cli unless it is empty. */
[[nodiscard]] std::size_t run_cases(char const* directory, std::string_view cli) noexcept {
    fixture_paths_t const paths {directory};
    if (!write_fixtures(paths)) return report("tools.fixtures", 1);

    char const* const documents_ndjson = paths.documents_ndjson.c_str();
    char const* const documents_csv = paths.documents_csv.c_str();
    char const* const edges_ndjson = paths.edges_ndjson.c_str();
    char const* const edges_csv = paths.edges_csv.c_str();
    char const* const exported_ndjson = paths.exported_ndjson.c_str();
    char const* const exported_csv = paths.exported_csv.c_str();

    std::size_t total = 0;
    total += report("graph.ndjson_to_ndjson", graph_round_trip(directory, edges_ndjson, exported_ndjson));
    total += report("graph.ndjson_to_csv", graph_round_trip(directory, edges_ndjson, exported_csv));
    total += report("graph.csv_to_ndjson", graph_round_trip(directory, edges_csv, exported_ndjson));
    total += report("graph.csv_to_csv", graph_round_trip(directory, edges_csv, exported_csv));

    field_selection_t const whole = field_selection_t::whole_k;
    field_selection_t const selected = field_selection_t::selected_k;
    total += report("docs.whole.ndjson_to_ndjson",
                    documents_round_trip(directory, documents_ndjson, exported_ndjson, whole));
    total += report("docs.whole.ndjson_to_csv", documents_round_trip(directory, documents_ndjson, exported_csv, whole));
    total += report("docs.whole.csv_to_ndjson", documents_round_trip(directory, documents_csv, exported_ndjson, whole));
    total += report("docs.whole.csv_to_csv", documents_round_trip(directory, documents_csv, exported_csv, whole));
    total += report("docs.selected.ndjson_to_ndjson",
                    documents_round_trip(directory, documents_ndjson, exported_ndjson, selected));
    total += report("docs.selected.ndjson_to_csv",
                    documents_round_trip(directory, documents_ndjson, exported_csv, selected));
    total += report("docs.selected.csv_to_ndjson",
                    documents_round_trip(directory, documents_csv, exported_ndjson, selected));
    total += report("docs.selected.csv_to_csv", documents_round_trip(directory, documents_csv, exported_csv, selected));

    total += report("invalid.graph_import.ndjson", graph_import_rejects_invalid_tasks(directory, edges_ndjson));
    total += report("invalid.graph_import.csv", graph_import_rejects_invalid_tasks(directory, edges_csv));
    total += report("invalid.graph_export.ndjson", graph_export_rejects_invalid_tasks(directory, exported_ndjson));
    total += report("invalid.graph_export.csv", graph_export_rejects_invalid_tasks(directory, exported_csv));
    total += report("invalid.docs_import.ndjson", docs_import_rejects_invalid_tasks(directory, documents_ndjson));
    total += report("invalid.docs_import.csv", docs_import_rejects_invalid_tasks(directory, documents_csv));
    total += report("invalid.docs_export.ndjson", docs_export_rejects_invalid_tasks(directory, exported_ndjson));
    total += report("invalid.docs_export.csv", docs_export_rejects_invalid_tasks(directory, exported_csv));

    // The CLI path views into `argv`, so it ends in a null terminator
    total += cli.empty() ? report("tools.cli", "needs --cli") : report("tools.cli", cli_round_trip(paths, cli.data()));
    return total;
}

} // namespace unum::ustore

int main(int argc, char** argv) {
    using namespace unum::ustore;
    std::string_view parent;
    std::string_view cli;
    std::array<flag_t, 2> const flags {{
        directory_flag(parent),
        {"--cli", "path", "the CLI executable its case runs, skipped without it", &cli},
    }};
    if (argument_status_t const status = parse_test_arguments(argc, argv, flags); status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;

    auto const maybe_directory = test_directory_t::make(parent);
    if (!maybe_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", parent);
        return 1;
    }
    char const* const directory = maybe_directory->path();

    std::size_t total = 0;
    total += directory ? run_cases(directory, cli) : report("tools", needs_directory_k);
    return report("total", total) ? 1 : 0;
}
