/**
 *  @file test/tools.cpp
 *  @author Ash Vardanian
 *  @date April 16, 2023
 *  @brief Tests importing and exporting documents and graph edges through NDJSON and CSV files, and the CLI on top.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int64_t`
#include <cstdio>  // `stderr`, `std::fopen`, `std::fread`, `std::fwrite`, `std::remove`
#include <cstdlib> // `std::free`, `std::system`

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

#include "ustore/db.h"
#include "ustore/ranges.hpp" // `strided_strings`, `u64_tape`
#include "ustore/strings.h"

#include "dataset.h"
#include "harness.hpp" // `suite_exit_code`, `create_directories`, `expect`

namespace unum::ustore::test {

constexpr std::int64_t rows_count_k = 1000;
constexpr ustore_size_t max_batch_size_k = 64 * 1024;

constexpr char const* documents_ndjson_k = "documents.ndjson";
constexpr char const* documents_csv_k = "documents.csv";
constexpr char const* edges_ndjson_k = "edges.ndjson";
constexpr char const* edges_csv_k = "edges.csv";
constexpr char const* exported_ndjson_k = "exported.ndjson";
constexpr char const* exported_csv_k = "exported.csv";
constexpr char const* exported_parquet_k = "exported.parquet";

constexpr std::string_view selected_paths_k[] = {
    "id",
    "id_str",
    "/user/id",
    "/user/followers_count",
    "/quoted_status/id",
    "/quoted_status/user",
    "/quoted_status/entities/hashtags",
    "/quoted_status/entities/media/0/id",
};

constexpr std::string_view selected_columns_k[] = {"id", "id_str", "user_id", "geo_type"};

constexpr std::string_view source_field_k = "id";
constexpr std::string_view target_field_k = "user_id";
constexpr std::string_view edge_field_k = "user_followers_count";

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
    path_t files;

    /** Documents as NDJSON. */
    path_t documents_ndjson;

    /** Documents as CSV. */
    path_t documents_csv;

    /** Edges as NDJSON. */
    path_t edges_ndjson;

    /** Edges as CSV. */
    path_t edges_csv;

    /** Export target in NDJSON. */
    path_t exported_ndjson;

    /** Export target in CSV. */
    path_t exported_csv;

    /** Names every path under @p test_directory. */
    explicit fixture_paths_t(char const* test_directory) noexcept
        : directory(test_directory), files(test_directory, "files"),
          documents_ndjson(files.c_str(), documents_ndjson_k), documents_csv(files.c_str(), documents_csv_k),
          edges_ndjson(files.c_str(), edges_ndjson_k), edges_csv(files.c_str(), edges_csv_k),
          exported_ndjson(files.c_str(), exported_ndjson_k), exported_csv(files.c_str(), exported_csv_k) {}
};

/** Appends @p piece to @p text, answering whether it fit. */
[[nodiscard]] bool append(sz::string_t& text, std::string_view piece) noexcept {
    return text.append(piece.data(), piece.size()) == sz::status_t::success_k;
}

/** Appends the decimal digits of @p number to @p text, answering whether they fit. */
[[nodiscard]] bool append(sz::string_t& text, std::int64_t number) noexcept {
    char digits[24];
    std::to_chars_result const printed = std::to_chars(digits, digits + sizeof(digits), number);
    return text.append(digits, static_cast<std::size_t>(printed.ptr - digits)) == sz::status_t::success_k;
}

/** Appends the NDJSON fixture document with key @p id, without a line break. */
[[nodiscard]] bool append_ndjson_document(sz::string_t& text, std::int64_t id) noexcept {
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
[[nodiscard]] bool append_csv_row(sz::string_t& text, std::int64_t id) noexcept {
    std::string_view const geo = id % 3 == 0 ? R"("Point, ""exact""")" : id % 3 == 1 ? "Point" : "";
    return append(text, id) && append(text, ",id-") && append(text, id) && append(text, ",") && append(text, id % 97) &&
           append(text, ",") && append(text, id * 3) && append(text, ",") && append(text, geo) &&
           append(text, ",false,true\n");
}

/** Appends the JSON document an import makes of the CSV fixture row with key @p id. */
[[nodiscard]] bool append_csv_document(sz::string_t& text, std::int64_t id) noexcept {
    std::string_view const geo = id % 3 == 0 ? R"("Point, \"exact\"")" : id % 3 == 1 ? R"("Point")" : "null";
    return append(text, R"({"id":)") && append(text, id) && append(text, R"(,"id_str":"id-)") && append(text, id) &&
           append(text, R"(","user_id":)") && append(text, id % 97) && append(text, R"(,"user_followers_count":)") &&
           append(text, id * 3) && append(text, R"(,"geo_type":)") && append(text, geo) &&
           append(text, R"(,"favorited":false,"retweeted":true})");
}

/** Writes @p content to the file at @p path, answering whether every byte landed. */
[[nodiscard]] bool write_file(char const* path, sz::string_t const& content) noexcept {
    std::FILE* const file = std::fopen(path, "wb");
    if (!file) return false;
    bool const written = std::fwrite(content.data(), 1, content.size(), file) == content.size();
    return std::fclose(file) == 0 && written;
}

/** Reads the whole file at @p path into @p content, answering whether it loaded. */
[[nodiscard]] bool read_file(char const* path, sz::string_t& content) noexcept {
    std::FILE* const file = std::fopen(path, "rb");
    if (!file) return false;
    bool loaded = std::fseek(file, 0, SEEK_END) == 0;
    long const size = loaded ? std::ftell(file) : -1;
    loaded = size >= 0 && std::fseek(file, 0, SEEK_SET) == 0 &&
             content.resize(static_cast<std::size_t>(size)) == sz::status_t::success_k;
    loaded = loaded && std::fread(content.data(), 1, content.size(), file) == content.size();
    [[maybe_unused]] int const closed = std::fclose(file);
    return loaded;
}

/** Writes the document and edge fixtures, in NDJSON and CSV, where @p paths names them. */
[[nodiscard]] bool write_fixtures(fixture_paths_t const& paths) noexcept {
    sz::string_t documents_ndjson;
    sz::string_t documents_csv;
    sz::string_t edges_ndjson;
    sz::string_t edges_csv;
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

    /** Opens the database @p config describes and answers whether it opened. */
    [[nodiscard]] bool open(config_t const& config) noexcept {
        auto [opened, open_status] = open_raw(config);
        if (open_status.failed()) {
            fmt::println(stderr, "  failed to open the database: {}", open_status.message);
            return false;
        }
        database_ = opened;
        return true;
    }

    /** Answers whether the engine reports named collections. */
    [[nodiscard]] bool supports_collections() noexcept {
        ustore_outcome_t outcome {};
        ustore_database_description_t description {};
        ustore_database_describe_t request {};
        request.database = database_;
        request.outcome = &outcome;
        request.description = &description;
        ustore_database_describe(&request);
        bool const supported = outcome.status == ustore_status_success_k &&
                               (description.capabilities & ustore_capabilities_collections_k);
        return supported;
    }

    /** Removes every key and value of the main collection. */
    void clear() noexcept {
        ustore_outcome_t outcome {};
        ustore_collection_drop_t drop {};
        drop.database = database_;
        drop.outcome = &outcome;
        drop.collection = ustore_collection_main_k;
        drop.mode = ustore_drop_pairs_k;
        ustore_collection_drop(&drop);
    }

    /** Creates an anonymous graph collection, answering its ID, or the main one after reporting why it could not. */
    [[nodiscard]] ustore_collection_id_t create_graph(ustore_outcome_t& outcome) noexcept {
        ustore_collection_id_t graph = ustore_collection_main_k;
        ustore_collection_create_t create {};
        create.database = database_;
        create.outcome = &outcome;
        create.modality = ustore_modality_graph_k;
        create.collection = &graph;
        ustore_collection_create(&create);
        return graph;
    }

    /** Drops @p collection together with its name. */
    void drop(ustore_collection_id_t collection, ustore_outcome_t& outcome) noexcept {
        ustore_collection_drop_t drop {};
        drop.database = database_;
        drop.outcome = &outcome;
        drop.collection = collection;
        drop.mode = ustore_drop_collection_k;
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
[[nodiscard]] yyjson_val* lookup(yyjson_val* object, std::string_view field) noexcept {
    return field.starts_with('/') ? yyjson_ptr_getn(object, field.data(), field.size())
                                  : yyjson_obj_getn(object, field.data(), field.size());
}

/** Serializes @p value minified into @p output, so equal values of equal key order compare equal as text. */
[[nodiscard]] bool canonical(yyjson_val* value, sz::string_t& output) noexcept {
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
    bool const serialized = text && output.append(text, length) == sz::status_t::success_k;
    std::free(text);
    yyjson_mut_doc_free(builder);
    return serialized;
}

/** Parses all of @p text as the decimal integer an exported key spells into @p key, answering whether it parsed. */
[[nodiscard]] bool parse_key(std::string_view text, std::uint64_t& key) noexcept {
    std::from_chars_result const parsed = std::from_chars(text.data(), text.data() + text.size(), key);
    return !text.empty() && parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size();
}

/** Whether @p database holds a collection named @p name, resolved through a one-off arena. */
[[nodiscard]] bool lists_collection(ustore_database_t database, char const* name) noexcept {
    ustore_outcome_t outcome {};
    ustore_arena_t arena = nullptr;
    ustore_arena_init_t arena_init {.arena = &arena, .executor = nullptr, .outcome = &outcome};
    ustore_arena_init(&arena_init);
    if (outcome.status != ustore_status_success_k) return false;

    std::string_view const text {name};
    std::uint64_t const offsets[2] {0, text.size()};
    ustore_octet_t const* presences = nullptr;
    ustore_collection_id_t const* ids = nullptr;
    ustore_collection_find_t find {};
    find.database = database;
    find.outcome = &outcome;
    find.count = 1;
    find.names = u64_tape(text.data(), offsets);
    find.presences = &presences;
    find.collections = &ids;
    find.arena = arena;
    ustore_collection_find(&find);
    bool const found = outcome.status == ustore_status_success_k && (presences[0] & 1);
    ustore_arena_free(arena);
    return found;
}

#pragma endregion Helpers

#pragma region Checks

/** One exported edge, compared after sorting, with the integers its keys spell, as the fixture's keys all do. */
struct edge_row_t {
    std::uint64_t source;
    std::uint64_t target;
    std::uint64_t edge;
};

/** Checks that the edges exported to @p path are exactly the fixture edges. */
[[nodiscard]] std::size_t check_edges(char const* path) noexcept {
    std::size_t failures = 0;
    sz::string_t content;
    st::basic_vector<edge_row_t> edges;
    if (!read_file(path, content)) return expect(!"the exported edges are readable");

    std::size_t line_index = 0;
    for (sz::string_view_t line : sz::split(content.view(), "\n")) {
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
            yyjson_val* const source = lookup(root, source_field_k);
            yyjson_val* const target = lookup(root, target_field_k);
            yyjson_val* const label = lookup(root, edge_field_k);
            parsed = yyjson_is_uint(source) && yyjson_is_uint(target) && yyjson_is_uint(label);
            edge = {yyjson_get_uint(source), yyjson_get_uint(target), yyjson_get_uint(label)};
        }
        failures += expect(parsed);
        if (!parsed || st::failed(edges.push_back(edge_row_t {edge}))) return failures + 1;
    }

    std::sort(edges.begin(), edges.end(),
              [](edge_row_t const& left, edge_row_t const& right) noexcept { return left.source < right.source; });
    failures += expect_eq(edges.size(), static_cast<std::size_t>(rows_count_k));
    for (std::size_t index = 0; index != edges.size(); ++index) {
        std::uint64_t const id = index + 1;
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
[[nodiscard]] std::size_t check_document(std::uint64_t key, std::string_view text, char const* input,
                                         field_selection_t selection) noexcept {
    std::size_t failures = expect(key >= 1 && key <= rows_count_k);
    sz::string_t expected_text;
    auto const id = static_cast<std::int64_t>(key);
    bool const generated = is_csv(input) ? append_csv_document(expected_text, id)
                                         : append_ndjson_document(expected_text, id);
    if (!generated) return failures + 1;

    json_document_t const exported {yyjson_read(text.data(), text.size(), 0)};
    json_document_t const expected {yyjson_read(expected_text.data(), expected_text.size(), 0)};
    yyjson_val* const exported_root = yyjson_doc_get_root(exported.handle);
    yyjson_val* const expected_root = yyjson_doc_get_root(expected.handle);
    failures += expect(yyjson_is_obj(exported_root));

    sz::string_t exported_value;
    sz::string_t expected_value;
    if (selection == field_selection_t::whole_k) {
        failures += expect(canonical(exported_root, exported_value) && canonical(expected_root, expected_value));
        failures += expect(exported_value == expected_value);
        return failures;
    }

    std::span<std::string_view const> const fields = is_csv(input)
                                                         ? std::span<std::string_view const>(selected_columns_k)
                                                         : std::span<std::string_view const>(selected_paths_k);
    for (std::string_view const field : fields) {
        failures += expect(canonical(lookup(exported_root, field), exported_value) &&
                           canonical(lookup(expected_root, field), expected_value));
        failures += expect(exported_value.size() != 0);
        failures += expect(exported_value == expected_value);
    }
    failures += expect(yyjson_obj_get(exported_root, is_csv(input) ? "retweeted" : "favorited") == nullptr);
    return failures;
}

/** Checks that the documents exported to @p output match the fixture rows of @p input, one per key. */
[[nodiscard]] std::size_t check_documents(char const* output, char const* input, field_selection_t selection) noexcept {
    std::size_t failures = 0;
    sz::string_t content;
    if (!read_file(output, content)) return expect(!"the exported documents are readable");

    std::size_t rows = 0;
    sz::string_t unescaped;
    for (sz::string_view_t line : sz::split(content.view(), "\n")) {
        if (line.size() == 0) continue;
        std::string_view const row = line;
        if (is_csv(output)) {
            if (row == "_id,doc") continue;
            std::size_t const comma = row.find(',');
            std::uint64_t key = 0;
            bool const quoted = comma != std::string_view::npos && row.size() >= comma + 3 && row[comma + 1] == '"' &&
                                row.back() == '"';
            failures += expect(quoted && parse_key(row.substr(0, comma), key));
            if (!quoted) continue;
            unescaped.clear();
            std::string_view separator;
            sz::string_view_t const inner {row.data() + comma + 2, row.size() - comma - 3};
            for (sz::string_view_t segment : sz::split(inner, "\"\"")) {
                failures += expect(append(unescaped, separator) && append(unescaped, segment));
                separator = "\"";
            }
            failures += check_document(key, unescaped, input, selection);
        }
        else {
            json_document_t const document {yyjson_read(row.data(), row.size(), 0)};
            yyjson_val* const root = yyjson_doc_get_root(document.handle);
            yyjson_val* const key = yyjson_obj_get(root, "_id");
            sz::string_t text;
            failures += expect(yyjson_is_uint(key) && canonical(yyjson_obj_get(root, "doc"), text));
            if (!yyjson_is_uint(key)) continue;
            failures += check_document(yyjson_get_uint(key), text, input, selection);
        }
        ++rows;
    }
    failures += expect_eq(rows, static_cast<std::size_t>(rows_count_k));
    return failures;
}

#pragma endregion Checks

#pragma region Round Trips

/** Imports edges from @p input, exports them to @p output, and checks the export holds exactly the fixture edges. */
[[nodiscard]] std::size_t graph_round_trip(config_t const& config, char const* input, char const* output) noexcept {
    test_database_t database;
    if (!database.open(config)) return 1;
    database.clear();

    std::size_t failures = 0;
    ustore_outcome_t outcome {};
    ustore_collection_id_t const graph = database.create_graph(outcome);
    failures += expect_success(outcome);

    ustore_graph_import_t importing {};
    importing.database = database.get();
    importing.outcome = &outcome;
    importing.collection = graph;
    importing.path = bytes_of(input);
    importing.max_batch_size = max_batch_size_k;
    importing.source_id_field = bytes_of(source_field_k);
    importing.target_id_field = bytes_of(target_field_k);
    importing.edge_id_field = bytes_of(edge_field_k);
    ustore_graph_import(&importing);
    failures += expect_success(outcome);

    ustore_graph_export_t exporting {};
    exporting.database = database.get();
    exporting.outcome = &outcome;
    exporting.collection = graph;
    exporting.path = bytes_of(output);
    exporting.max_batch_size = max_batch_size_k;
    exporting.source_id_field = bytes_of(source_field_k);
    exporting.target_id_field = bytes_of(target_field_k);
    exporting.edge_id_field = bytes_of(edge_field_k);
    ustore_graph_export(&exporting);
    failures += expect_success(outcome);

    failures += check_edges(output);
    [[maybe_unused]] int const removed = std::remove(output);
    database.drop(graph, outcome);
    failures += expect_success(outcome);
    return failures;
}

/** Imports documents from @p input, exports them to @p output, and checks the rows or fields @p selection keeps. */
[[nodiscard]] std::size_t documents_round_trip(config_t const& config, char const* input, char const* output,
                                               field_selection_t selection) noexcept {
    test_database_t database;
    if (!database.open(config)) return 1;
    database.clear();

    std::span<std::string_view const> const fields = is_csv(input)
                                                         ? std::span<std::string_view const>(selected_columns_k)
                                                         : std::span<std::string_view const>(selected_paths_k);
    bool const selects = selection == field_selection_t::selected_k;
    strided_strings<std::string_view> const names {strided(fields).begin()};

    std::size_t failures = 0;
    ustore_outcome_t outcome {};
    ustore_docs_import_t importing {};
    importing.database = database.get();
    importing.outcome = &outcome;
    importing.collection = ustore_collection_main_k;
    importing.path = bytes_of(input);
    importing.max_batch_size = max_batch_size_k;
    importing.fields_count = selects ? fields.size() : 0;
    importing.fields = selects ? names.view() : ustore_strings_view_t {};
    importing.id_field = bytes_of("id");
    ustore_docs_import(&importing);
    failures += expect_success(outcome);

    ustore_docs_export_t exporting {};
    exporting.database = database.get();
    exporting.outcome = &outcome;
    exporting.collection = ustore_collection_main_k;
    exporting.path = bytes_of(output);
    exporting.max_batch_size = max_batch_size_k;
    exporting.fields_count = importing.fields_count;
    exporting.fields = importing.fields;
    ustore_docs_export(&exporting);
    failures += expect_success(outcome);

    failures += check_documents(output, input, selection);
    [[maybe_unused]] int const removed = std::remove(output);
    database.clear();
    return failures;
}

#pragma endregion Round Trips

#pragma region Invalid Tasks

/** Graph import rejects a task without a path, source, target or database, and accepts one without edges. */
[[nodiscard]] std::size_t graph_import_rejects_invalid_tasks(config_t const& config, char const* input) noexcept {
    test_database_t database;
    if (!database.open(config)) return 1;
    database.clear();

    std::size_t failures = 0;
    ustore_outcome_t outcome {};
    ustore_collection_id_t const graph = database.create_graph(outcome);
    failures += expect_success(outcome);
    ustore_graph_import_t valid {};
    valid.database = database.get();
    valid.outcome = &outcome;
    valid.collection = graph;
    valid.path = bytes_of(input);
    valid.max_batch_size = max_batch_size_k;
    valid.source_id_field = bytes_of(source_field_k);
    valid.target_id_field = bytes_of(target_field_k);
    valid.edge_id_field = bytes_of(edge_field_k);

    ustore_graph_import_t without_path = valid;
    without_path.path = {};
    ustore_graph_import(&without_path);
    failures += expect_failure(outcome);

    ustore_graph_import_t without_source = valid;
    without_source.source_id_field = {};
    ustore_graph_import(&without_source);
    failures += expect_failure(outcome);

    ustore_graph_import_t without_target = valid;
    without_target.target_id_field = {};
    ustore_graph_import(&without_target);
    failures += expect_failure(outcome);

    ustore_graph_import_t without_edge = valid;
    without_edge.edge_id_field = {};
    ustore_graph_import(&without_edge);
    failures += expect_success(outcome);

    ustore_graph_import_t without_database = valid;
    without_database.database = nullptr;
    ustore_graph_import(&without_database);
    failures += expect_failure(outcome);

    database.drop(graph, outcome);
    failures += expect_success(outcome);
    return failures;
}

/** Graph export rejects a task without a path, source, target or database, or with a pointer field. */
[[nodiscard]] std::size_t graph_export_rejects_invalid_tasks(config_t const& config, char const* output) noexcept {
    test_database_t database;
    if (!database.open(config)) return 1;
    database.clear();

    std::size_t failures = 0;
    ustore_outcome_t outcome {};
    ustore_graph_export_t valid {};
    valid.database = database.get();
    valid.outcome = &outcome;
    valid.collection = ustore_collection_main_k;
    valid.path = bytes_of(output);
    valid.max_batch_size = max_batch_size_k;
    valid.source_id_field = bytes_of(source_field_k);
    valid.target_id_field = bytes_of(target_field_k);
    valid.edge_id_field = bytes_of(edge_field_k);

    ustore_graph_export_t without_path = valid;
    without_path.path = {};
    ustore_graph_export(&without_path);
    failures += expect_failure(outcome);

    ustore_graph_export_t without_source = valid;
    without_source.source_id_field = {};
    ustore_graph_export(&without_source);
    failures += expect_failure(outcome);

    ustore_graph_export_t without_target = valid;
    without_target.target_id_field = {};
    ustore_graph_export(&without_target);
    failures += expect_failure(outcome);

    ustore_graph_export_t with_pointer_field = valid;
    with_pointer_field.source_id_field = bytes_of("/id");
    ustore_graph_export(&with_pointer_field);
    failures += expect_failure(outcome);

    ustore_graph_export_t without_edge = valid;
    without_edge.edge_id_field = {};
    ustore_graph_export(&without_edge);
    failures += expect_success(outcome);

    ustore_graph_export_t without_database = valid;
    without_database.database = nullptr;
    ustore_graph_export(&without_database);
    failures += expect_failure(outcome);

    [[maybe_unused]] int const removed = std::remove(output);
    return failures;
}

/** Document import rejects a task without a path, fields, an accessor, database or an identifier among the fields. */
[[nodiscard]] std::size_t docs_import_rejects_invalid_tasks(config_t const& config, char const* input) noexcept {
    test_database_t database;
    if (!database.open(config)) return 1;
    database.clear();

    std::size_t failures = 0;
    ustore_outcome_t outcome {};
    strided_strings<std::string_view> const names {strided(selected_columns_k).begin()};
    ustore_docs_import_t valid {};
    valid.database = database.get();
    valid.outcome = &outcome;
    valid.collection = ustore_collection_main_k;
    valid.path = bytes_of(input);
    valid.max_batch_size = max_batch_size_k;
    valid.fields_count = std::size(selected_columns_k);
    valid.fields = names.view();
    valid.id_field = bytes_of("id");

    ustore_docs_import_t without_path = valid;
    without_path.path = {};
    ustore_docs_import(&without_path);
    failures += expect_failure(outcome);

    ustore_docs_import_t without_count = valid;
    without_count.fields_count = 0;
    ustore_docs_import(&without_count);
    failures += expect_failure(outcome);

    ustore_docs_import_t without_fields = valid;
    without_fields.fields = {};
    ustore_docs_import(&without_fields);
    failures += expect_failure(outcome);

    ustore_docs_import_t without_accessor = valid;
    without_accessor.fields.data.accessor.get = nullptr;
    ustore_docs_import(&without_accessor);
    failures += expect_failure(outcome);

    ustore_docs_import_t without_id_among_fields = valid;
    without_id_among_fields.id_field = bytes_of("missing");
    ustore_docs_import(&without_id_among_fields);
    failures += expect_failure(outcome);

    ustore_docs_import_t without_database = valid;
    without_database.database = nullptr;
    ustore_docs_import(&without_database);
    failures += expect_failure(outcome);

    database.clear();
    return failures;
}

/** Document export rejects a task without a path, fields, an accessor or database, or with an unknown format. */
[[nodiscard]] std::size_t docs_export_rejects_invalid_tasks(config_t const& config, char const* output) noexcept {
    test_database_t database;
    if (!database.open(config)) return 1;
    database.clear();

    std::size_t failures = 0;
    ustore_outcome_t outcome {};
    strided_strings<std::string_view> const names {strided(selected_columns_k).begin()};
    ustore_docs_export_t valid {};
    valid.database = database.get();
    valid.outcome = &outcome;
    valid.collection = ustore_collection_main_k;
    valid.path = bytes_of(output);
    valid.max_batch_size = max_batch_size_k;
    valid.fields_count = std::size(selected_columns_k);
    valid.fields = names.view();

    ustore_docs_export_t without_path = valid;
    without_path.path = {};
    ustore_docs_export(&without_path);
    failures += expect_failure(outcome);

    ustore_docs_export_t without_count = valid;
    without_count.fields_count = 0;
    ustore_docs_export(&without_count);
    failures += expect_failure(outcome);

    ustore_docs_export_t without_fields = valid;
    without_fields.fields = {};
    ustore_docs_export(&without_fields);
    failures += expect_failure(outcome);

    ustore_docs_export_t without_accessor = valid;
    without_accessor.fields.data.accessor.get = nullptr;
    ustore_docs_export(&without_accessor);
    failures += expect_failure(outcome);

    path_t const unknown_format_output {config.directory(), exported_parquet_k};
    ustore_docs_export_t with_unknown_format = valid;
    with_unknown_format.path = bytes_of(unknown_format_output.c_str());
    ustore_docs_export(&with_unknown_format);
    failures += expect_failure(outcome);

    ustore_docs_export_t without_database = valid;
    without_database.database = nullptr;
    ustore_docs_export(&without_database);
    failures += expect_failure(outcome);

    return failures;
}

#pragma endregion Invalid Tasks

#pragma region CLI

/** Runs @p command with @p arguments through the system's shell and answers zero exactly when it exited with zero. */
template <typename... arguments_type_>
    requires(std::same_as<arguments_type_, char const*> && ...)
[[nodiscard]] int run_command(char const* command, arguments_type_... arguments) noexcept {
    // A word with a space would split under both `sh` and `cmd.exe`, and the fixture paths carry none
    sz::string_t line;
    bool const built = append(line, command) && ((append(line, " ") && append(line, arguments)) && ...);
    return built ? std::system(line.c_str()) : -1;
}

/** Creates, imports into, exports and drops a named collection, then imports and exports the main one. */
[[nodiscard]] std::size_t cli_round_trip(fixture_paths_t const& paths, config_t const& config,
                                         char const* cli) noexcept {
    std::size_t failures = 0;
    char const* const directory = paths.directory;
    // The engine's name is a literal or a word of `argv`, so it ends in a null either way
    char const* const engine = config.engine().data();
    char const* const collection = "collection1";
    char const* const input = paths.documents_ndjson.c_str();
    char const* const output = paths.exported_ndjson.c_str();
    bool supports_collections = false;
    {
        test_database_t database;
        failures += expect(database.open(config));
        supports_collections = database.supports_collections();
    }

    if (supports_collections) {
        failures += expect_eq(run_command(cli, "--engine", engine, "--directory", directory, "collection", "create",
                                          "--collection", collection),
                              0);
        {
            test_database_t database;
            failures += expect(database.open(config) && lists_collection(database.get(), collection));
        }

        failures += expect_eq(
            run_command(cli, "--engine", engine, "--directory", directory, "collection", "import", "--input", input,
                        "--id-field", "id", "--memory-limit", "1MB", "--collection", collection),
            0);
        failures += expect_eq(run_command(cli, "--engine", engine, "--directory", directory, "collection", "export",
                                          "--output", output, "--memory-limit", "1MB", "--collection", collection),
                              0);
        failures += check_documents(output, input, field_selection_t::whole_k);
        [[maybe_unused]] int const removed_named = std::remove(output);

        failures += expect_eq(run_command(cli, "--engine", engine, "--directory", directory, "collection", "drop",
                                          "--collection", collection),
                              0);
        {
            test_database_t database;
            failures += expect(database.open(config) && !lists_collection(database.get(), collection));
        }
    }

    failures += expect_eq(run_command(cli, "--engine", engine, "--directory", directory, "collection", "import",
                                      "--input", input, "--id-field", "id", "--memory-limit", "1MB"),
                          0);
    failures += expect_eq(run_command(cli, "--engine", engine, "--directory", directory, "collection", "export",
                                      "--output", output, "--memory-limit", "1MB"),
                          0);
    failures += check_documents(output, input, field_selection_t::whole_k);
    [[maybe_unused]] int const removed_main = std::remove(output);
    return failures;
}

#pragma endregion CLI

/** Runs every case against the fixtures written under the directory of @p config, and the CLI at @p cli unless it is
 *  empty. */
void run_cases(suite_totals_t& totals, config_t const& config, std::string_view cli) noexcept {
    fixture_paths_t const paths {config.directory()};
    if (!write_fixtures(paths)) {
        report(totals, "tools.fixtures", 1);
        return;
    }

    char const* const documents_ndjson = paths.documents_ndjson.c_str();
    char const* const documents_csv = paths.documents_csv.c_str();
    char const* const edges_ndjson = paths.edges_ndjson.c_str();
    char const* const edges_csv = paths.edges_csv.c_str();
    char const* const exported_ndjson = paths.exported_ndjson.c_str();
    char const* const exported_csv = paths.exported_csv.c_str();

    report(totals, "graph.ndjson_to_ndjson", graph_round_trip, config, edges_ndjson, exported_ndjson);
    report(totals, "graph.ndjson_to_csv", graph_round_trip, config, edges_ndjson, exported_csv);
    report(totals, "graph.csv_to_ndjson", graph_round_trip, config, edges_csv, exported_ndjson);
    report(totals, "graph.csv_to_csv", graph_round_trip, config, edges_csv, exported_csv);

    field_selection_t const whole = field_selection_t::whole_k;
    field_selection_t const selected = field_selection_t::selected_k;
    report(totals, "docs.whole.ndjson_to_ndjson", documents_round_trip, config, documents_ndjson, exported_ndjson,
           whole);
    report(totals, "docs.whole.ndjson_to_csv", documents_round_trip, config, documents_ndjson, exported_csv, whole);
    report(totals, "docs.whole.csv_to_ndjson", documents_round_trip, config, documents_csv, exported_ndjson, whole);
    report(totals, "docs.whole.csv_to_csv", documents_round_trip, config, documents_csv, exported_csv, whole);
    report(totals, "docs.selected.ndjson_to_ndjson", documents_round_trip, config, documents_ndjson, exported_ndjson,
           selected);
    report(totals, "docs.selected.ndjson_to_csv", documents_round_trip, config, documents_ndjson, exported_csv,
           selected);
    report(totals, "docs.selected.csv_to_ndjson", documents_round_trip, config, documents_csv, exported_ndjson,
           selected);
    report(totals, "docs.selected.csv_to_csv", documents_round_trip, config, documents_csv, exported_csv, selected);

    report(totals, "invalid.graph_import.ndjson", graph_import_rejects_invalid_tasks, config, edges_ndjson);
    report(totals, "invalid.graph_import.csv", graph_import_rejects_invalid_tasks, config, edges_csv);
    report(totals, "invalid.graph_export.ndjson", graph_export_rejects_invalid_tasks, config, exported_ndjson);
    report(totals, "invalid.graph_export.csv", graph_export_rejects_invalid_tasks, config, exported_csv);
    report(totals, "invalid.docs_import.ndjson", docs_import_rejects_invalid_tasks, config, documents_ndjson);
    report(totals, "invalid.docs_import.csv", docs_import_rejects_invalid_tasks, config, documents_csv);
    report(totals, "invalid.docs_export.ndjson", docs_export_rejects_invalid_tasks, config, exported_ndjson);
    report(totals, "invalid.docs_export.csv", docs_export_rejects_invalid_tasks, config, exported_csv);

    // The CLI path views into `argv`, so it ends in a null terminator
    if (cli.empty()) report(totals, "tools.cli", case_result_t::skipped("needs --cli"));
    else report(totals, "tools.cli", cli_round_trip, paths, config, cli.data());
}

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;
    suite_options_t options;
    std::string_view cli;
    std::array<flag_t, 1> const flags {{
        {"--cli", "path", "the CLI executable its case runs, skipped without it", &cli},
    }};
    if (argument_status_t const status = parse_suite_options(argc, argv, options, flags);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    auto const maybe_directory = directory_t::make(options.directory);
    if (!maybe_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", options.directory);
        return 1;
    }
    char const* const directory = maybe_directory->path();
    config_t const config {options.engine, directory};

    if (directory) run_cases(totals, config, cli);
    else report(totals, "tools", case_result_t::skipped(needs_directory_k));
    return suite_exit_code(totals);
}
