/**
 *  @file test/tools/dataset.cpp
 *  @author Ash Vardanian
 *  @date April 16, 2023
 *  @brief Tests importing and exporting documents and graph edges through NDJSON and CSV files,
 *      collections of every modality through Arrow streams and files, and the CLI on top: its
 *      usage, its one-line flag errors, its variables, and its stop on SIGINT.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int64_t`
#include <cstdio>  // `stderr`, `std::fopen`, `std::fread`, `std::fwrite`, `std::remove`
#include <cstdlib> // `std::free`, `std::system`
#include <cstring> // `std::memcmp`

#include <filesystem> // `std::filesystem::remove_all`, `std::filesystem::create_directories`

#include <algorithm>    // `std::sort`
#include <array>        // `std::array`
#include <charconv>     // `std::from_chars`, `std::to_chars`
#include <concepts>     // `std::same_as`
#include <iterator>     // `std::size`
#include <span>         // `std::span`
#include <string>       // `std::string`, `std::to_string`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::errc`
#include <utility>      // `std::move`
#include <vector>       // `std::vector`

#include <fmt/format.h>
#include <yyjson.h>

#include <smashtable/basic_vector.hpp>
#include <stringzilla/stringzilla.hpp>

#include "ustore/db.h"
#include "ustore/ranges.hpp" // `strided_strings`, `u64_tape`
#include "ustore/strings.h"

#if !USTORE_OS_WINDOWS_
#include <csignal>    // `SIGINT`, `SIGPIPE`, `std::signal`
#include <fcntl.h>    // `open`, `fcntl`, `O_NONBLOCK`
#include <poll.h>     // `poll`, `pollfd`
#include <sys/stat.h> // `mkfifo`, `stat`
#include <sys/wait.h> // `waitpid`, `WIFEXITED`, `WEXITSTATUS`
#include <unistd.h>   // `fork`, `execv`, `pipe`, `dup2`, `read`, `write`
#endif

#include "arrow_ipc.hpp" // `ipc_read_prefix`, `ipc_write_batch`, `ipc_read_footer`
#include "dataset.h"
#include "harness.hpp" // `run_test_case`, `setting_text`, `create_directories`, `expect_true`

namespace unum::ustore::test {

using namespace unum::ustore::tools;
using namespace unum::ustore::utilities;

/** The reason every case driving the CLI reports when the suite ran without the @c CLI setting. */
constexpr char const* needs_cli_k = "needs USTORE_CLI";

/** The reason the manifest cases report on a build whose CLI opens no NVMe engine. */
constexpr char const* needs_nvme_engine_k = "needs ustore_engine_nvme_k";

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
    std::string files;

    /** Documents as NDJSON. */
    std::string documents_ndjson;

    /** Documents as CSV. */
    std::string documents_csv;

    /** Edges as NDJSON. */
    std::string edges_ndjson;

    /** Edges as CSV. */
    std::string edges_csv;

    /** Export target in NDJSON. */
    std::string exported_ndjson;

    /** Export target in CSV. */
    std::string exported_csv;

    /** Names every path under @p test_directory. */
    explicit fixture_paths_t(char const* test_directory) noexcept
        : directory(test_directory), files(fmt::format("{}/files", test_directory)),
          documents_ndjson(fmt::format("{}/{}", files, documents_ndjson_k)),
          documents_csv(fmt::format("{}/{}", files, documents_csv_k)),
          edges_ndjson(fmt::format("{}/{}", files, edges_ndjson_k)),
          edges_csv(fmt::format("{}/{}", files, edges_csv_k)),
          exported_ndjson(fmt::format("{}/{}", files, exported_ndjson_k)),
          exported_csv(fmt::format("{}/{}", files, exported_csv_k)) {}
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

#pragma region Test Database

/** A database opened in one directory, freed on destruction. */
class test_database_t {

    /** The open database, or null until @c open succeeds. */
    ustore_database_t database_ = nullptr;

    /** The arena of @c arena, or null until a case asks for it. */
    ustore_arena_t arena_ = nullptr;

  public:
    test_database_t() noexcept = default;
    test_database_t(test_database_t const&) = delete;
    test_database_t& operator=(test_database_t const&) = delete;
    ~test_database_t() noexcept {
        ustore_arena_free(arena_);
        if (database_) ustore_database_free(database_);
    }

    /** Opens the database @p config describes and answers whether it opened. */
    [[nodiscard]] bool open(config_t const& config) noexcept {
        auto [opened, open_status] = open_raw(config);
        if (!expect_status(open_status, status_code_t::success_k)) return false;
        database_ = opened;
        return true;
    }

    /** The arena every task of a case fills, made on first use and freed with the database. */
    [[nodiscard]] ustore_arena_t arena() noexcept {
        if (!arena_) {
            ustore_outcome_t outcome {};
            ustore_arena_init_t init {.arena = &arena_, .executor = nullptr, .outcome = &outcome};
            ustore_arena_init(&init);
        }
        return arena_;
    }

    /** Drops every collection, so a case starts from an empty database. */
    void clear() noexcept {
        ustore_outcome_t outcome {};
        ustore_database_clear_t clear {};
        clear.database = database_;
        clear.outcome = &outcome;
        clear.mode = ustore_drop_collection_k;
        ustore_database_clear(&clear);
    }

    /** Creates an anonymous collection holding @p modality, answering its ID, or the nil UUID after
     *  reporting why it could not. */
    [[nodiscard]] ustore_collection_id_t create(ustore_modality_t modality, ustore_outcome_t& outcome) noexcept {
        ustore_collection_id_t created {};
        ustore_collection_create_t create {};
        create.database = database_;
        create.outcome = &outcome;
        create.modality = modality;
        create.collection = &created;
        ustore_collection_create(&create);
        return created;
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

/** Finds @p field in @p object, as a JSON pointer when it starts with a slash and as
 *  a key otherwise. */
[[nodiscard]] yyjson_val* lookup(yyjson_val* object, std::string_view field) noexcept {
    return field.starts_with('/') ? yyjson_ptr_getn(object, field.data(), field.size())
                                  : yyjson_obj_getn(object, field.data(), field.size());
}

/** Serializes @p value minified into @p output, so equal values of equal key order compare
 *  equal as text. */
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

/** Parses all of @p text as the decimal integer an exported key spells into @p key, answering
 *  whether it parsed. */
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

#pragma endregion Test Database

#pragma region Checks

/** One exported edge, compared after sorting, with the integers its keys spell, as the fixture's
 *  keys all do. */
struct edge_row_t {
    std::uint64_t source;
    std::uint64_t target;
    std::uint64_t edge;
};

/** Checks that the edges exported to @p path are exactly the fixture edges. */
void check_edges(char const* path) noexcept {
    sz::string_t content;
    st::vector<edge_row_t> edges;
    if (!expect_true(read_file(path, content), "the exported edges are readable")) return;

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
        if (!expect_true(parsed) ||
            !expect_true(!st::failed(edges.push_back(edge_row_t {edge})), "no room for an edge"))
            return;
    }

    std::sort(edges.begin(), edges.end(),
              [](edge_row_t const& left, edge_row_t const& right) noexcept { return left.source < right.source; });
    expect_equal(edges.size(), static_cast<std::size_t>(rows_count_k));
    for (std::size_t index = 0; index != edges.size(); ++index) {
        std::uint64_t const id = index + 1;
        edge_row_t const& edge = edges[index];
        if (edge.source == id && edge.target == id % 97 && edge.edge == id * 3) continue;
        expect_equal(edge.source, id);
        expect_equal(edge.target, id % 97);
        expect_equal(edge.edge, id * 3);
        break;
    }
}

/** Checks one exported document against the fixture row of its key. */
void check_document(std::uint64_t key, std::string_view text, char const* input, field_selection_t selection) noexcept {
    expect_true(key >= 1 && key <= rows_count_k);
    sz::string_t expected_text;
    auto const id = static_cast<std::int64_t>(key);
    bool const generated = is_csv(input) ? append_csv_document(expected_text, id)
                                         : append_ndjson_document(expected_text, id);
    if (!expect_true(generated, "the fixture row of key {} regenerates", key)) return;

    json_document_t const exported {yyjson_read(text.data(), text.size(), 0)};
    json_document_t const expected {yyjson_read(expected_text.data(), expected_text.size(), 0)};
    yyjson_val* const exported_root = yyjson_doc_get_root(exported.handle);
    yyjson_val* const expected_root = yyjson_doc_get_root(expected.handle);
    expect_true(yyjson_is_obj(exported_root));

    sz::string_t exported_value;
    sz::string_t expected_value;
    if (selection == field_selection_t::whole_k) {
        expect_true(canonical(exported_root, exported_value) && canonical(expected_root, expected_value));
        expect_true(exported_value == expected_value);
        return;
    }

    std::span<std::string_view const> const fields = is_csv(input)
                                                         ? std::span<std::string_view const>(selected_columns_k)
                                                         : std::span<std::string_view const>(selected_paths_k);
    for (std::string_view const field : fields) {
        expect_true(canonical(lookup(exported_root, field), exported_value) &&
                    canonical(lookup(expected_root, field), expected_value));
        expect_true(exported_value.size() != 0);
        expect_true(exported_value == expected_value);
    }
    expect_true(yyjson_obj_get(exported_root, is_csv(input) ? "retweeted" : "favorited") == nullptr);
}

/** Checks that the documents exported to @p output match the fixture rows of @p input,
 *  one per key. */
void check_documents(char const* output, char const* input, field_selection_t selection) noexcept {
    sz::string_t content;
    if (!expect_true(read_file(output, content), "the exported documents are readable")) return;

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
            expect_true(quoted && parse_key(row.substr(0, comma), key));
            if (!quoted) continue;
            unescaped.clear();
            std::string_view separator;
            sz::string_view_t const inner {row.data() + comma + 2, row.size() - comma - 3};
            for (sz::string_view_t segment : sz::split(inner, "\"\"")) {
                expect_true(append(unescaped, separator) && append(unescaped, segment));
                separator = "\"";
            }
            check_document(key, unescaped, input, selection);
        }
        else {
            json_document_t const document {yyjson_read(row.data(), row.size(), 0)};
            yyjson_val* const root = yyjson_doc_get_root(document.handle);
            yyjson_val* const key = yyjson_obj_get(root, "_id");
            sz::string_t text;
            expect_true(yyjson_is_uint(key) && canonical(yyjson_obj_get(root, "doc"), text));
            if (!yyjson_is_uint(key)) continue;
            check_document(yyjson_get_uint(key), text, input, selection);
        }
        ++rows;
    }
    expect_equal(rows, static_cast<std::size_t>(rows_count_k));
}

#pragma endregion Checks

#pragma region Round Trips

/** Imports edges from @p input, exports them to @p output, and checks the export holds exactly
 *  the fixture edges. */
void test_graph_round_trip(config_t const& config, char const* input, char const* output) noexcept {
    test_database_t database;
    if (!database.open(config)) return;
    database.clear();

    ustore_outcome_t outcome {};
    ustore_collection_id_t const graph = database.create(ustore_modality_graph_k, outcome);
    expect_success(outcome);

    ustore_graph_import_t importing {};
    importing.database = database.get();
    importing.arena = database.arena();
    importing.outcome = &outcome;
    importing.collection = graph;
    importing.path = bytes_of(input);
    importing.max_batch_size = max_batch_size_k;
    importing.source_id_field = bytes_of(source_field_k);
    importing.target_id_field = bytes_of(target_field_k);
    importing.edge_id_field = bytes_of(edge_field_k);
    ustore_graph_import(&importing);
    expect_success(outcome);

    ustore_graph_export_t exporting {};
    exporting.database = database.get();
    exporting.arena = database.arena();
    exporting.outcome = &outcome;
    exporting.collection = graph;
    exporting.path = bytes_of(output);
    exporting.max_batch_size = max_batch_size_k;
    exporting.source_id_field = bytes_of(source_field_k);
    exporting.target_id_field = bytes_of(target_field_k);
    exporting.edge_id_field = bytes_of(edge_field_k);
    ustore_graph_export(&exporting);
    expect_success(outcome);

    check_edges(output);
    [[maybe_unused]] int const removed = std::remove(output);
    database.drop(graph, outcome);
    expect_success(outcome);
}

/** Imports documents from @p input, exports them to @p output, and checks the rows or fields
 *  @p selection keeps. */
void test_documents_round_trip(config_t const& config, char const* input, char const* output,
                               field_selection_t selection) noexcept {
    test_database_t database;
    if (!database.open(config)) return;
    database.clear();

    std::span<std::string_view const> const fields = is_csv(input)
                                                         ? std::span<std::string_view const>(selected_columns_k)
                                                         : std::span<std::string_view const>(selected_paths_k);
    bool const selects = selection == field_selection_t::selected_k;
    strided_strings<std::string_view> const names {strided(fields).begin()};

    ustore_outcome_t outcome {};
    ustore_collection_id_t const documents = database.create(ustore_modality_docs_k, outcome);
    expect_success(outcome);

    ustore_docs_import_t importing {};
    importing.database = database.get();
    importing.arena = database.arena();
    importing.outcome = &outcome;
    importing.collection = documents;
    importing.path = bytes_of(input);
    importing.max_batch_size = max_batch_size_k;
    importing.fields_count = selects ? fields.size() : 0;
    importing.fields = selects ? names.view() : ustore_strings_view_t {};
    importing.id_field = bytes_of("id");
    ustore_docs_import(&importing);
    expect_success(outcome);

    ustore_docs_export_t exporting {};
    exporting.database = database.get();
    exporting.arena = database.arena();
    exporting.outcome = &outcome;
    exporting.collection = documents;
    exporting.path = bytes_of(output);
    exporting.max_batch_size = max_batch_size_k;
    exporting.fields_count = importing.fields_count;
    exporting.fields = importing.fields;
    ustore_docs_export(&exporting);
    expect_success(outcome);

    check_documents(output, input, selection);
    [[maybe_unused]] int const removed = std::remove(output);
    database.clear();
}

#pragma endregion Round Trips

#pragma region Invalid Tasks

/** Graph import rejects a task without a path, source, target or database, and accepts
 *  one without edges. */
void test_graph_import_rejects_invalid_tasks(config_t const& config, char const* input) noexcept {
    test_database_t database;
    if (!database.open(config)) return;
    database.clear();

    ustore_outcome_t outcome {};
    ustore_collection_id_t const graph = database.create(ustore_modality_graph_k, outcome);
    expect_success(outcome);
    ustore_graph_import_t valid {};
    valid.database = database.get();
    valid.arena = database.arena();
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
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_graph_import_t without_source = valid;
    without_source.source_id_field = {};
    ustore_graph_import(&without_source);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_graph_import_t without_target = valid;
    without_target.target_id_field = {};
    ustore_graph_import(&without_target);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_graph_import_t without_edge = valid;
    without_edge.edge_id_field = {};
    ustore_graph_import(&without_edge);
    expect_success(outcome);

    ustore_graph_import_t without_database = valid;
    without_database.database = nullptr;
    ustore_graph_import(&without_database);
    expect_status(outcome, status_code_t::uninitialized_k);

    database.drop(graph, outcome);
    expect_success(outcome);
}

/** Graph export rejects a task without a path, source, target or database, or with
 *  a pointer field. */
void test_graph_export_rejects_invalid_tasks(config_t const& config, char const* output) noexcept {
    test_database_t database;
    if (!database.open(config)) return;
    database.clear();

    ustore_outcome_t outcome {};
    ustore_collection_id_t const graph = database.create(ustore_modality_graph_k, outcome);
    expect_success(outcome);
    ustore_graph_export_t valid {};
    valid.database = database.get();
    valid.arena = database.arena();
    valid.outcome = &outcome;
    valid.collection = graph;
    valid.path = bytes_of(output);
    valid.max_batch_size = max_batch_size_k;
    valid.source_id_field = bytes_of(source_field_k);
    valid.target_id_field = bytes_of(target_field_k);
    valid.edge_id_field = bytes_of(edge_field_k);

    ustore_graph_export_t without_path = valid;
    without_path.path = {};
    ustore_graph_export(&without_path);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_graph_export_t without_source = valid;
    without_source.source_id_field = {};
    ustore_graph_export(&without_source);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_graph_export_t without_target = valid;
    without_target.target_id_field = {};
    ustore_graph_export(&without_target);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_graph_export_t with_pointer_field = valid;
    with_pointer_field.source_id_field = bytes_of("/id");
    ustore_graph_export(&with_pointer_field);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_graph_export_t without_edge = valid;
    without_edge.edge_id_field = {};
    ustore_graph_export(&without_edge);
    expect_success(outcome);

    ustore_graph_export_t without_database = valid;
    without_database.database = nullptr;
    ustore_graph_export(&without_database);
    expect_status(outcome, status_code_t::uninitialized_k);

    [[maybe_unused]] int const removed = std::remove(output);
}

/** Document import rejects a task without a path, fields, an accessor, database or an identifier
 *  among the fields. */
void test_docs_import_rejects_invalid_tasks(config_t const& config, char const* input) noexcept {
    test_database_t database;
    if (!database.open(config)) return;
    database.clear();

    ustore_outcome_t outcome {};
    strided_strings<std::string_view> const names {strided(selected_columns_k).begin()};
    ustore_collection_id_t const documents = database.create(ustore_modality_docs_k, outcome);
    expect_success(outcome);
    ustore_docs_import_t valid {};
    valid.database = database.get();
    valid.arena = database.arena();
    valid.outcome = &outcome;
    valid.collection = documents;
    valid.path = bytes_of(input);
    valid.max_batch_size = max_batch_size_k;
    valid.fields_count = std::size(selected_columns_k);
    valid.fields = names.view();
    valid.id_field = bytes_of("id");

    ustore_docs_import_t without_path = valid;
    without_path.path = {};
    ustore_docs_import(&without_path);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_docs_import_t without_count = valid;
    without_count.fields_count = 0;
    ustore_docs_import(&without_count);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_docs_import_t without_fields = valid;
    without_fields.fields = {};
    ustore_docs_import(&without_fields);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_docs_import_t without_accessor = valid;
    without_accessor.fields.data.accessor.get = nullptr;
    ustore_docs_import(&without_accessor);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_docs_import_t without_id_among_fields = valid;
    without_id_among_fields.id_field = bytes_of("missing");
    ustore_docs_import(&without_id_among_fields);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_docs_import_t without_database = valid;
    without_database.database = nullptr;
    ustore_docs_import(&without_database);
    expect_status(outcome, status_code_t::uninitialized_k);

    database.clear();
}

/** Document export rejects a task without a path, fields, an accessor or database, or with
 *  an unknown format. */
void test_docs_export_rejects_invalid_tasks(config_t const& config, char const* output) noexcept {
    test_database_t database;
    if (!database.open(config)) return;
    database.clear();

    ustore_outcome_t outcome {};
    strided_strings<std::string_view> const names {strided(selected_columns_k).begin()};
    ustore_collection_id_t const documents = database.create(ustore_modality_docs_k, outcome);
    expect_success(outcome);
    ustore_docs_export_t valid {};
    valid.database = database.get();
    valid.arena = database.arena();
    valid.outcome = &outcome;
    valid.collection = documents;
    valid.path = bytes_of(output);
    valid.max_batch_size = max_batch_size_k;
    valid.fields_count = std::size(selected_columns_k);
    valid.fields = names.view();

    ustore_docs_export_t without_path = valid;
    without_path.path = {};
    ustore_docs_export(&without_path);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_docs_export_t without_count = valid;
    without_count.fields_count = 0;
    ustore_docs_export(&without_count);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_docs_export_t without_fields = valid;
    without_fields.fields = {};
    ustore_docs_export(&without_fields);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_docs_export_t without_accessor = valid;
    without_accessor.fields.data.accessor.get = nullptr;
    ustore_docs_export(&without_accessor);
    expect_status(outcome, status_code_t::invalid_argument_k);

    std::string const unknown_format_output = fmt::format("{}/{}", config.directory(), exported_parquet_k);
    ustore_docs_export_t with_unknown_format = valid;
    with_unknown_format.path = bytes_of(unknown_format_output.c_str());
    ustore_docs_export(&with_unknown_format);
    expect_status(outcome, status_code_t::unsupported_k);

    ustore_docs_export_t without_database = valid;
    without_database.database = nullptr;
    ustore_docs_export(&without_database);
    expect_status(outcome, status_code_t::uninitialized_k);
}

#pragma endregion Invalid Tasks

#pragma region CLI

/** Runs @p command with @p arguments through the system's shell and answers zero exactly when it
 *  exited with zero. */
template <typename... arguments_type_>
    requires(std::same_as<arguments_type_, char const*> && ...)
[[nodiscard]] int run_command(char const* command, arguments_type_... arguments) noexcept {
    // A word with a space would split under both `sh` and `cmd.exe`, and the fixture
    // paths hold none
    sz::string_t line;
    bool const built = append(line, command) && ((append(line, " ") && append(line, arguments)) && ...);
    return built ? std::system(line.c_str()) : -1;
}

/** Creates, imports into, exports and drops a named collection, and refuses an import
 *  naming no collection. */
void test_cli_round_trip(fixture_paths_t const& paths, config_t const& config, char const* executable) noexcept {
    char const* const directory = paths.directory;
    // The engine's name is a literal or a word of `argv`, so it ends in a null either way
    char const* const engine = config.engine().data();
    char const* const collection = "collection1";
    char const* const input = paths.documents_ndjson.c_str();
    char const* const output = paths.exported_ndjson.c_str();
    expect_equal(run_command(executable, "--engine", engine, "--directory", directory, "collection", "create",
                             "--collection", collection),
                 0);
    {
        test_database_t database;
        expect_true(database.open(config) && lists_collection(database.get(), collection));
    }

    expect_equal(run_command(executable, "--engine", engine, "--directory", directory, "collection", "import",
                             "--input", input, "--id-field", "id", "--memory-limit", "1M", "--collection", collection),
                 0);
    expect_equal(run_command(executable, "--engine", engine, "--directory", directory, "collection", "export",
                             "--output", output, "--memory-limit", "1M", "--collection", collection),
                 0);
    check_documents(output, input, field_selection_t::whole_k);
    [[maybe_unused]] int const removed_named = std::remove(output);

    expect_equal(run_command(executable, "--engine", engine, "--directory", directory, "collection", "drop",
                             "--collection", collection),
                 0);
    {
        test_database_t database;
        expect_true(database.open(config) && !lists_collection(database.get(), collection));
    }

    // Documents live only in collections of documents, so an import naming none is refused
    expect_true(run_command(executable, "--engine", engine, "--directory", directory, "collection", "import", "--input",
                            input, "--id-field", "id", "--memory-limit", "1M") != 0);
}

#pragma endregion CLI

#pragma region Arrow

/** Rows each Arrow round trip writes, enough for an export to answer more than one batch. */
constexpr std::size_t arrow_rows_k = 3000;

/** The field metadata of a key tagged as Arrow's UUID extension. */
constexpr ipc_key_value_t uuid_metadata_k[] {{ipc_extension_name_key_k, ipc_extension_uuid_k}};

/** The field metadata of the XML values of a pages collection. */
constexpr ipc_key_value_t page_metadata_k[] {{"ustore.page_encoding", "xml"}};

/** The cells of one row, each the bytes of its column. */
using arrow_row_t = std::vector<std::string>;

/** A table to write as an Arrow stream: its schema, whose metadata views constants,
 *  and its rows. */
struct arrow_table_t {
    std::vector<ipc_field_t> fields;
    std::vector<ipc_key_value_t> metadata;
    std::vector<arrow_row_t> rows;
};

/** The 16 bytes of the UUID spelling @p integer, as <tt>uuid.UUID(int=n).bytes</tt> and
 *  @c key_from_integer do. */
[[nodiscard]] std::string uuid_cell(std::uint64_t integer) {
    std::string cell(16, '\0');
    for (std::size_t index = 0; index != 8; ++index) cell[15 - index] = char((integer >> (8 * index)) & 0xFF);
    return cell;
}

/** A key column tagged as Arrow's UUID extension. */
[[nodiscard]] ipc_field_t uuid_field(std::string_view name) noexcept {
    return {.name = name, .type = ipc_type_t::fixed_size_binary_k, .width = 16, .metadata = uuid_metadata_k};
}

/** A column of byte strings. */
[[nodiscard]] ipc_field_t binary_field(std::string_view name) noexcept {
    return {.name = name, .type = ipc_type_t::binary_k};
}

/** The table a round trip of @p modality writes, its keys shuffled; blobs hold no
 *  @c ustore.modality, so their modality comes from the columns. */
[[nodiscard]] arrow_table_t modality_table(ustore_modality_t modality) {
    arrow_table_t table;
    if (modality != ustore_modality_blobs_k)
        table.metadata.push_back({"ustore.modality", ustore_modality_name(modality)});
    switch (modality) {
    case ustore_modality_strings_k: table.fields = {binary_field("key"), uuid_field("value")}; break;
    case ustore_modality_graph_k:
        table.fields = {uuid_field("source"), uuid_field("target"), uuid_field("edge")};
        break;
    case ustore_modality_pages_k: {
        ipc_field_t value = binary_field("value");
        value.metadata = page_metadata_k;
        table.fields = {uuid_field("key"), value};
        break;
    }
    default: table.fields = {uuid_field("key"), binary_field("value")}; break;
    }
    for (std::size_t index = 0; index != arrow_rows_k; ++index) {
        std::uint64_t const key = (index * 7919) % arrow_rows_k + 1;
        std::string const digits = std::to_string(key);
        switch (modality) {
        case ustore_modality_docs_k: table.rows.push_back({uuid_cell(key), R"({"id":)" + digits + "}"}); break;
        case ustore_modality_strings_k: table.rows.push_back({"name-" + digits, uuid_cell(key)}); break;
        case ustore_modality_graph_k:
            table.rows.push_back({uuid_cell(key), uuid_cell(key % 13 + 1), uuid_cell(100000 + key)});
            break;
        case ustore_modality_pages_k: table.rows.push_back({uuid_cell(key), "<p>" + digits + "</p>"}); break;
        default: table.rows.push_back({uuid_cell(key), std::string("blob\0", 5) + digits}); break;
        }
    }
    return table;
}

/** Writes @p table as an Arrow stream of one batch to @p path through the codec, answering
 *  whether it landed. */
[[nodiscard]] bool write_arrow_stream(char const* path, arrow_table_t const& table) {
    ipc_schema_t const schema {table.fields, table.metadata};
    std::size_t const columns = table.fields.size();
    std::vector<std::string> data(columns);
    std::vector<std::vector<std::int32_t>> offsets(columns, std::vector<std::int32_t> {0});
    for (arrow_row_t const& row : table.rows)
        for (std::size_t column = 0; column != columns; ++column) {
            data[column] += row[column];
            offsets[column].push_back(static_cast<std::int32_t>(data[column].size()));
        }
    std::vector<ipc_node_t> nodes(columns, ipc_node_t {static_cast<std::int64_t>(table.rows.size()), 0});
    std::vector<std::span<std::byte const>> buffers;
    for (std::size_t column = 0; column != columns; ++column) {
        buffers.push_back({});
        if (ipc_type_is_variable(table.fields[column].type))
            buffers.push_back(std::as_bytes(std::span(offsets[column])));
        buffers.push_back(std::as_bytes(std::span(data[column])));
    }
    ipc_batch_t const batch {static_cast<std::int64_t>(table.rows.size()), nodes, buffers};

    std::vector<std::byte> stream;
    std::size_t needed = 0, written = 0;
    std::uint64_t body_bytes = 0;
    std::array<ipc_gather_t, ipc_buffers_max_k> gathers {};
    if (ipc_write_schema(schema, {}, 0, {}, needed) != ipc_status_t::too_small_k) return false;
    stream.resize(needed);
    if (ipc_write_schema(schema, {}, 0, stream, written) != ipc_status_t::success_k) return false;
    if (ipc_write_batch(schema, batch, {}, stream.size(), {}, needed, gathers, body_bytes) != ipc_status_t::too_small_k)
        return false;
    std::size_t const start = stream.size();
    stream.resize(start + needed);
    if (ipc_write_batch(schema, batch, {}, start, std::span(stream).subspan(start), written, gathers, body_bytes) !=
        ipc_status_t::success_k)
        return false;
    for (std::size_t index = 0; index != buffers.size(); ++index) {
        stream.insert(stream.end(), gathers[index].bytes.begin(), gathers[index].bytes.end());
        stream.resize(stream.size() + gathers[index].padding);
    }
    stream.resize(stream.size() + 8);
    if (ipc_write_end_of_stream(std::span(stream).last(8), written) != ipc_status_t::success_k) return false;

    std::FILE* const file = std::fopen(path, "wb");
    if (!file) return false;
    bool const landed = std::fwrite(stream.data(), 1, stream.size(), file) == stream.size();
    return std::fclose(file) == 0 && landed;
}

/** What reading an Arrow output found: its schema's modality and fields, and every row. */
struct arrow_read_t {
    std::string modality;
    std::vector<std::string> names;
    std::vector<arrow_row_t> rows;
    std::size_t batches = 0;
    std::vector<std::size_t> body_offsets;
};

/** Reads the Arrow stream, or the file when it starts with the magic, in @p bytes with the
 *  codec, failing a check for anything it refuses, and, for a stream, for any byte past its
 *  end-of-stream marker. */
void read_arrow(std::span<std::byte const> bytes, arrow_read_t& read) {
    bool const is_file = bytes.size() >= 8 && std::memcmp(bytes.data(), ipc_file_magic_k.data(), 8) == 0;
    std::size_t cursor = is_file ? 8 : 0;
    std::array<ipc_field_t, ipc_fields_max_k> fields {};
    std::array<ipc_key_value_t, 32> pairs {};
    ipc_schema_t schema {};
    bool schema_read = true;
    for (;;) {
        std::uint32_t metadata_bytes = 0;
        std::size_t prefix_bytes = 0;
        if (!expect_true(ipc_read_prefix(bytes.subspan(cursor), metadata_bytes, prefix_bytes) ==
                         ipc_status_t::success_k))
            return;
        cursor += prefix_bytes;
        if (metadata_bytes == 0) break;
        ipc_message_t message;
        if (!expect_true(ipc_read_message(bytes.subspan(cursor, metadata_bytes), message) == ipc_status_t::success_k))
            return;
        cursor += metadata_bytes;
        if (message.kind == ipc_message_kind_t::schema_k) {
            schema_read = expect_true(ipc_read_schema(message, fields, pairs, schema) == ipc_status_t::success_k);
            for (ipc_key_value_t const& pair : schema.metadata)
                if (pair.key == "ustore.modality") read.modality = pair.value;
            for (ipc_field_t const& field : schema.fields) read.names.emplace_back(field.name);
            continue;
        }
        std::array<ipc_node_t, ipc_fields_max_k> nodes {};
        std::array<std::span<std::byte const>, ipc_buffers_max_k> buffers {};
        ipc_batch_t batch {};
        std::size_t const body = static_cast<std::size_t>(message.body_bytes);
        read.body_offsets.push_back(cursor);
        bool const batch_read = expect_true(
            ipc_read_batch(message, bytes.subspan(cursor, fu::min_of(body, bytes.size() - cursor)), schema, nodes,
                           buffers, batch) == ipc_status_t::success_k);
        bool const batch_valid = expect_true(ipc_validate_batch(schema, batch) == ipc_status_t::success_k);
        cursor += body;
        if (!schema_read || !batch_read || !batch_valid) return;
        ++read.batches;
        for (std::int64_t row = 0; row != batch.length; ++row) {
            arrow_row_t cells;
            std::size_t buffer = 0;
            for (ipc_field_t const& field : schema.fields) {
                std::span<std::byte const> data;
                if (ipc_type_is_variable(field.type)) {
                    std::span<std::byte const> const offsets = batch.buffers[buffer + 1];
                    bool const wide = ipc_offset_bytes(field.type) == 8;
                    std::size_t const width = wide ? 8 : 4;
                    std::int64_t const begin = wide ? ipc_load_little<std::int64_t>(offsets.data() + row * width)
                                                    : ipc_load_little<std::int32_t>(offsets.data() + row * width);
                    std::int64_t const end = wide ? ipc_load_little<std::int64_t>(offsets.data() + (row + 1) * width)
                                                  : ipc_load_little<std::int32_t>(offsets.data() + (row + 1) * width);
                    data = batch.buffers[buffer + 2].subspan(std::size_t(begin), std::size_t(end - begin));
                    buffer += 3;
                }
                else {
                    std::size_t const width = ipc_value_bytes(field);
                    data = batch.buffers[buffer + 1].subspan(std::size_t(row) * width, width);
                    buffer += 2;
                }
                cells.emplace_back(reinterpret_cast<char const*>(data.data()), data.size());
            }
            read.rows.push_back(std::move(cells));
        }
    }
    if (!is_file) expect_equal(cursor, bytes.size());
}

/** Reads the Arrow output at @p path into @p read. */
void read_arrow_file(char const* path, arrow_read_t& read) {
    sz::string_t content;
    if (!expect_true(read_file(path, content), "the Arrow output is readable")) return;
    read_arrow(std::as_bytes(std::span(content)), read);
}

/** Checks that @p actual holds the rows of @p wanted, in any order. */
void expect_same_rows(std::vector<arrow_row_t> actual, std::vector<arrow_row_t> wanted) {
    std::sort(actual.begin(), actual.end());
    std::sort(wanted.begin(), wanted.end());
    if (expect_equal(actual.size(), wanted.size())) expect_true(actual == wanted);
}

/** Imports a stream of @p modality through the CLI, by path or, for blobs, through standard input
 *  with the modality read off its columns, exports it to <tt>.arrows</tt>, and checks every row and the
 *  schema's modality come back. */
void test_arrow_round_trip(fixture_paths_t const& paths, config_t const& config, char const* executable,
                           ustore_modality_t modality) {
    std::string const name = std::string("arrow_") + ustore_modality_name(modality);
    std::string const input = fmt::format("{}/{}", paths.files.c_str(), (name + ".in.arrows").c_str());
    std::string const output = fmt::format("{}/{}", paths.files.c_str(), (name + ".out.arrows").c_str());
    arrow_table_t const table = modality_table(modality);
    if (!expect_true(write_arrow_stream(input.c_str(), table), "the Arrow input is written")) return;

    char const* const engine = config.engine().data();
    bool const piped = modality == ustore_modality_blobs_k;
    expect_equal(run_command(executable, "--engine", engine, "--directory", paths.directory, "collection", "import",
                             "--collection", name.c_str(), "--input", piped ? "-" : input.c_str(), piped ? "<" : "",
                             piped ? input.c_str() : ""),
                 0);
    expect_equal(run_command(executable, "--engine", engine, "--directory", paths.directory, "collection", "export",
                             "--collection", name.c_str(), "--output", output.c_str(), "--batch-rows", "1000"),
                 0);
    arrow_read_t read;
    read_arrow_file(output.c_str(), read);
    expect_equal(read.modality, std::string(ustore_modality_name(modality)));
    expect_true(read.batches > 1);
    expect_same_rows(read.rows, table.rows);
    [[maybe_unused]] int const removed_input = std::remove(input.c_str());
    [[maybe_unused]] int const removed_output = std::remove(output.c_str());
}

/** An export to <tt>--output -</tt> writes only Arrow bytes to standard output, a stream ending at its
 *  marker, and its status line to standard error. */
void test_arrow_stdout_only_arrow(fixture_paths_t const& paths, config_t const& config, char const* executable) {
    std::string const input = fmt::format("{}/{}", paths.files.c_str(), "stdout.in.arrows");
    std::string const output = fmt::format("{}/{}", paths.files.c_str(), "stdout.out");
    std::string const errors = fmt::format("{}/{}", paths.files.c_str(), "stdout.err");
    arrow_table_t const table = modality_table(ustore_modality_docs_k);
    if (!expect_true(write_arrow_stream(input.c_str(), table), "the Arrow input is written")) return;

    char const* const engine = config.engine().data();
    expect_equal(run_command(executable, "--engine", engine, "--directory", paths.directory, "collection", "import",
                             "--collection", "stdout", "--input", input.c_str()),
                 0);
    expect_equal(run_command(executable, "--engine", engine, "--directory", paths.directory, "collection", "export",
                             "--collection", "stdout", "--output", "-", ">", output.c_str(), "2>", errors.c_str()),
                 0);
    arrow_read_t read;
    read_arrow_file(output.c_str(), read);
    expect_same_rows(read.rows, table.rows);
    sz::string_t status;
    expect_true(read_file(errors.c_str(), status) && status.view().find("Exported") != sz::string_view_t::npos);
    for (std::string const* path : {&input, &output, &errors}) {
        [[maybe_unused]] int const removed = std::remove(path->c_str());
    }
}

/** An <tt>.arrow</tt> file exported at 4096 bytes starts every body on a page and imports back whole. */
void test_arrow_file_direct_alignment(fixture_paths_t const& paths, config_t const& config, char const* executable) {
    std::string const input = fmt::format("{}/{}", paths.files.c_str(), "direct.in.arrows");
    std::string const file = fmt::format("{}/{}", paths.files.c_str(), "direct.arrow");
    std::string const output = fmt::format("{}/{}", paths.files.c_str(), "direct.out.arrows");
    arrow_table_t const table = modality_table(ustore_modality_graph_k);
    if (!expect_true(write_arrow_stream(input.c_str(), table), "the Arrow input is written")) return;

    char const* const engine = config.engine().data();
    expect_equal(run_command(executable, "--engine", engine, "--directory", paths.directory, "collection", "import",
                             "--collection", "direct", "--input", input.c_str()),
                 0);
    expect_equal(run_command(executable, "--engine", engine, "--directory", paths.directory, "collection", "export",
                             "--collection", "direct", "--output", file.c_str(), "--alignment", "4096"),
                 0);
    sz::string_t content;
    if (!expect_true(read_file(file.c_str(), content), "the Arrow file is readable")) return;
    std::span<std::byte const> const bytes = std::as_bytes(std::span(content));
    std::array<ipc_field_t, ipc_fields_max_k> fields {};
    std::array<ipc_key_value_t, 32> pairs {};
    std::array<ipc_block_t, 64> blocks {};
    ipc_schema_t schema {};
    std::size_t blocks_count = 0;
    expect_true(ipc_read_footer(bytes, fields, pairs, schema, blocks, blocks_count) == ipc_status_t::success_k);
    arrow_read_t read;
    read_arrow(bytes, read);
    expect_equal(blocks_count, read.batches);
    for (std::size_t index = 0; index != fu::min_of(blocks_count, read.body_offsets.size()); ++index) {
        expect_equal(std::size_t(blocks[index].offset + blocks[index].metadata_bytes), read.body_offsets[index]);
        expect_equal(read.body_offsets[index] % ipc_alignment_direct_k, 0u);
    }
    expect_same_rows(read.rows, table.rows);

    expect_equal(run_command(executable, "--engine", engine, "--directory", paths.directory, "collection", "import",
                             "--collection", "direct_copy", "--input", file.c_str()),
                 0);
    expect_equal(run_command(executable, "--engine", engine, "--directory", paths.directory, "collection", "export",
                             "--collection", "direct_copy", "--output", output.c_str()),
                 0);
    arrow_read_t copied;
    read_arrow_file(output.c_str(), copied);
    expect_equal(copied.modality, std::string("graph"));
    expect_same_rows(copied.rows, table.rows);
    for (std::string const* path : {&input, &file, &output}) {
        [[maybe_unused]] int const removed = std::remove(path->c_str());
    }
}

/** A bulk import either lands every row, or, on an engine that ingests no stream, fails cleanly
 *  with nothing written to standard output and drops the collection it created. */
void test_arrow_bulk_lands_or_refuses(fixture_paths_t const& paths, config_t const& config, char const* executable) {
    std::string const input = fmt::format("{}/{}", paths.files.c_str(), "bulk.in.arrows");
    std::string const output = fmt::format("{}/{}", paths.files.c_str(), "bulk.out.arrows");
    std::string const printed = fmt::format("{}/{}", paths.files.c_str(), "bulk.out");
    std::string const errors = fmt::format("{}/{}", paths.files.c_str(), "bulk.err");
    arrow_table_t const table = modality_table(ustore_modality_blobs_k);
    if (!expect_true(write_arrow_stream(input.c_str(), table), "the Arrow input is written")) return;

    char const* const engine = config.engine().data();
    int const bulk = run_command(executable, "--engine", engine, "--directory", paths.directory, "collection", "import",
                                 "--collection", "bulk", "--input", input.c_str(), "--ingest", "unsorted", ">",
                                 printed.c_str(), "2>", errors.c_str());
    sz::string_t out_text, error_text;
    expect_true(read_file(printed.c_str(), out_text) && out_text.size() == 0);
    expect_true(read_file(errors.c_str(), error_text));
    if (bulk == 0) {
        expect_equal(run_command(executable, "--engine", engine, "--directory", paths.directory, "collection", "export",
                                 "--collection", "bulk", "--output", output.c_str()),
                     0);
        arrow_read_t read;
        read_arrow_file(output.c_str(), read);
        expect_same_rows(read.rows, table.rows);
    }
    else {
        // The ingest created the collection, so its refusal drops it again
        expect_true(error_text.view().find("ingests nothing") != sz::string_view_t::npos);
        test_database_t database;
        expect_true(database.open(config) && !lists_collection(database.get(), "bulk"));
    }
    for (std::string const* path : {&input, &output, &printed, &errors}) {
        [[maybe_unused]] int const removed = std::remove(path->c_str());
    }
}

/** Three sorted blob inputs whose keys overlap import as one, each key landing once with the value
 *  of the last input holding it: merged through a sorted ingest, or written in input order on an
 *  engine that ingests no stream. */
void test_arrow_sorted_inputs_merge(fixture_paths_t const& paths, config_t const& config, char const* executable) {
    // Inputs hold keys [1,100], [50,150] and [100,200], so 50-99 and 100-150 repeat
    constexpr std::uint64_t firsts[3] {1, 50, 100}, lasts[3] {100, 150, 200};
    std::string inputs[3];
    std::vector<arrow_row_t> wanted(200);
    for (std::size_t input = 0; input != 3; ++input) {
        arrow_table_t table;
        table.fields = {uuid_field("key"), binary_field("value")};
        for (std::uint64_t key = firsts[input]; key <= lasts[input]; ++key) {
            arrow_row_t row {uuid_cell(key), std::string(1, char('a' + input)) + std::to_string(key)};
            wanted[key - 1] = row;
            table.rows.push_back(std::move(row));
        }
        inputs[input] = fmt::format("{}/merge.{}.arrows", paths.files.c_str(), input);
        if (!expect_true(write_arrow_stream(inputs[input].c_str(), table), "an Arrow input is written")) return;
    }
    std::string const listed = inputs[0] + "," + inputs[1] + "," + inputs[2];
    std::string const output = fmt::format("{}/{}", paths.files.c_str(), "merge.out.arrows");
    std::string const errors = fmt::format("{}/{}", paths.files.c_str(), "merge.err");
    char const* const engine = config.engine().data();
    int const sorted = run_command(executable, "--engine", engine, "--directory", paths.directory, "collection",
                                   "import", "--collection", "merged", "--input", listed.c_str(), "--ingest", "sorted",
                                   "2>", errors.c_str());
    sz::string_t error_text;
    expect_true(read_file(errors.c_str(), error_text));
    if (sorted != 0) {
        expect_true(error_text.view().find("ingests nothing") != sz::string_view_t::npos);
        expect_equal(run_command(executable, "--engine", engine, "--directory", paths.directory, "collection", "import",
                                 "--collection", "merged", "--input", listed.c_str()),
                     0);
    }
    expect_equal(run_command(executable, "--engine", engine, "--directory", paths.directory, "collection", "export",
                             "--collection", "merged", "--output", output.c_str()),
                 0);
    arrow_read_t read;
    read_arrow_file(output.c_str(), read);
    expect_same_rows(read.rows, wanted);
    for (std::string const& path : inputs) [[maybe_unused]]
        int const removed = std::remove(path.c_str());
    for (std::string const* path : {&output, &errors}) [[maybe_unused]]
        int const removed = std::remove(path->c_str());
}

/** The name and modality of every named collection the CLI lists in @p directory on @p engine,
 *  sorted, as a snapshot export skips the unnamed ones. */
void list_collections(char const* executable, char const* engine, char const* directory, char const* listing,
                      std::vector<std::string>& collections) {
    expect_equal(
        run_command(executable, "--engine", engine, "--directory", directory, "collection", "list", ">", listing), 0);
    sz::string_t text;
    expect_true(read_file(listing, text));
    for (sz::string_view_t line : sz::split(text.view(), "\n")) {
        std::string_view const row = line;
        std::size_t const tab = row.find('\t');
        if (tab != std::string_view::npos && row.substr(tab + 1).front() != '\t')
            collections.emplace_back(row.substr(tab + 1));
    }
    std::sort(collections.begin(), collections.end());
    [[maybe_unused]] int const removed = std::remove(listing);
}

/** A snapshot export of every modality, imported on NVMe when the build has it and on the suite's
 *  engine otherwise, holds the same collections with the same rows, and a second export into the
 *  same directory is refused. */
void test_snapshot_export_import(fixture_paths_t const& paths, config_t const& config, char const* executable) {
    char const* const engine = config.engine().data();
    std::string const snapshot = fmt::format("{}/{}", paths.files.c_str(), "snapshot");
    std::string const target = fmt::format("{}/{}", paths.files.c_str(), "snapshot_target");
    std::string const listing = fmt::format("{}/{}", paths.files.c_str(), "snapshot.list");
    std::error_code error;
    std::filesystem::remove_all(snapshot.c_str(), error);
    std::filesystem::remove_all(target.c_str(), error);
    std::filesystem::create_directories(target.c_str(), error);

    constexpr ustore_modality_t modalities[] {ustore_modality_blobs_k, ustore_modality_docs_k,
                                              ustore_modality_strings_k, ustore_modality_graph_k,
                                              ustore_modality_pages_k};
    for (ustore_modality_t const modality : modalities) {
        std::string const name = std::string("snapshot_") + ustore_modality_name(modality);
        std::string const input = fmt::format("{}/{}", paths.files.c_str(), (name + ".arrows").c_str());
        if (!expect_true(write_arrow_stream(input.c_str(), modality_table(modality)), "the Arrow input is written"))
            return;
        expect_equal(run_command(executable, "--engine", engine, "--directory", paths.directory, "collection", "import",
                                 "--collection", name.c_str(), "--input", input.c_str()),
                     0);
        [[maybe_unused]] int const removed = std::remove(input.c_str());
    }
    expect_equal(run_command(executable, "--engine", engine, "--directory", paths.directory, "snapshot", "export",
                             snapshot.c_str()),
                 0);
    expect_true(run_command(executable, "--engine", engine, "--directory", paths.directory, "snapshot", "export",
                            snapshot.c_str(), "2>", listing.c_str()) != 0);

    // The NVMe engine opens only where the build has it, which a listing of an
    // empty directory probes
    bool const has_nvme = run_command(executable, "--engine", "nvme", "--directory", target.c_str(), "collection",
                                      "list", ">", listing.c_str(), "2>&1") == 0;
    char const* const target_engine = has_nvme ? "nvme" : engine;
    expect_equal(run_command(executable, "--engine", target_engine, "--directory", target.c_str(), "snapshot", "import",
                             snapshot.c_str()),
                 0);

    std::vector<std::string> source_collections, target_collections;
    list_collections(executable, engine, paths.directory, listing.c_str(), source_collections);
    list_collections(executable, target_engine, target.c_str(), listing.c_str(), target_collections);
    expect_true(source_collections.size() >= std::size(modalities));
    expect_true(source_collections == target_collections);
    for (std::string const& collection : source_collections) {
        std::string const name = collection.substr(0, collection.find('\t'));
        std::string const source_output = fmt::format("{}/{}", paths.files.c_str(), "snapshot.source.arrows");
        std::string const target_output = fmt::format("{}/{}", paths.files.c_str(), "snapshot.target.arrows");
        expect_equal(run_command(executable, "--engine", engine, "--directory", paths.directory, "collection", "export",
                                 "--collection", name.c_str(), "--output", source_output.c_str()),
                     0);
        expect_equal(run_command(executable, "--engine", target_engine, "--directory", target.c_str(), "collection",
                                 "export", "--collection", name.c_str(), "--output", target_output.c_str()),
                     0);
        arrow_read_t source_read, target_read;
        read_arrow_file(source_output.c_str(), source_read);
        read_arrow_file(target_output.c_str(), target_read);
        expect_equal(target_read.modality, source_read.modality);
        expect_same_rows(target_read.rows, source_read.rows);
    }
    std::filesystem::remove_all(snapshot.c_str(), error);
    std::filesystem::remove_all(target.c_str(), error);
}

#pragma endregion Arrow

#pragma region Manifests

/** The run files a manifest at @p path names, as sorted "path offset bytes level" lines, empty when
 *  it does not parse. */
[[nodiscard]] std::vector<std::string> manifest_files(char const* path) {
    std::vector<std::string> files;
    yyjson_doc* const document = yyjson_read_file(path, YYJSON_READ_NOFLAG, nullptr, nullptr);
    yyjson_val* const listed = yyjson_obj_get(yyjson_doc_get_root(document), "files");
    std::size_t index = 0, count = 0;
    yyjson_val* file = nullptr;
    yyjson_arr_foreach(listed, index, count, file) {
        char const* const name = yyjson_get_str(yyjson_obj_get(file, "path"));
        files.push_back(fmt::format("{} {} {} {}", name ? name : "", yyjson_get_uint(yyjson_obj_get(file, "offset")),
                                    yyjson_get_uint(yyjson_obj_get(file, "bytes")),
                                    yyjson_get_uint(yyjson_obj_get(file, "level"))));
    }
    yyjson_doc_free(document);
    std::sort(files.begin(), files.end());
    return files;
}

/** The collections a manifest at @p path records, as sorted "name store value_size
 *  value_max_size" lines. */
[[nodiscard]] std::vector<std::string> manifest_collections(char const* path) {
    std::vector<std::string> collections;
    yyjson_doc* const document = yyjson_read_file(path, YYJSON_READ_NOFLAG, nullptr, nullptr);
    yyjson_val* const listed = yyjson_obj_get(yyjson_doc_get_root(document), "collections");
    std::size_t index = 0, count = 0;
    yyjson_val* collection = nullptr;
    yyjson_arr_foreach(listed, index, count, collection) {
        char const* const name = yyjson_get_str(yyjson_obj_get(collection, "name"));
        collections.push_back(fmt::format("{} {} {} {}", name ? name : "",
                                          yyjson_get_uint(yyjson_obj_get(collection, "store")),
                                          yyjson_get_uint(yyjson_obj_get(collection, "value_size")),
                                          yyjson_get_uint(yyjson_obj_get(collection, "value_max_size"))));
    }
    yyjson_doc_free(document);
    std::sort(collections.begin(), collections.end());
    return collections;
}

/** The manifest command prints the manifest a database opens by, and recover rebuilds one naming
 *  the same run files from the runs alone, every one on level 0, and the same collections from the
 *  collections system collection. */
[[nodiscard]] case_result_t test_manifest_print_and_recover(fixture_paths_t const& paths, char const* executable) {
    std::string const directory = fmt::format("{}/{}", paths.files.c_str(), "recovered");
    std::array<char, nvme_manifest_capacity_k> manifest_buffer {};
    std::string_view const manifest_view = nvme_manifest_in(directory.c_str(), manifest_buffer);
    std::string const manifest {manifest_view};
    std::string const database {nvme_database_name_k};
    std::string const printed = fmt::format("{}/{}", paths.files.c_str(), "recovered.printed.json");
    std::string const original = fmt::format("{}/{}", paths.files.c_str(), "recovered.original.json");
    std::error_code error;
    std::filesystem::remove_all(directory.c_str(), error);
    std::filesystem::create_directories(directory.c_str(), error);
    // The NVMe engine opens only where the build has it, which a listing of an
    // empty directory probes
    if (run_command(executable, "--engine", "nvme", "--directory", directory.c_str(), "collection", "list", ">",
                    printed.c_str(), "2>&1") != 0)
        return case_result_t::skipped(needs_nvme_engine_k);
    expect_equal(run_command(executable, "--engine", "nvme", "--directory", directory.c_str(), "collection", "create",
                             "--collection", "people", "--modality", "blobs", "2>", printed.c_str()),
                 0);

    expect_equal(run_command(executable, "manifest", manifest.c_str(), ">", printed.c_str()), 0);
    expect_equal(
        run_command(executable, "manifest", directory.c_str(), "--database", database.c_str(), ">", original.c_str()),
        0);
    expect_true(manifest_files(original.c_str()) == manifest_files(manifest.c_str()));
    std::vector<std::string> const before = manifest_files(manifest.c_str());
    expect_true(!before.empty());
    expect_true(manifest_files(printed.c_str()) == before);
    expect_true(run_command(executable, "recover", directory.c_str(), "--database", database.c_str(), "2>",
                            printed.c_str()) != 0);

    std::filesystem::rename(manifest.c_str(), original.c_str(), error);
    expect_true(!error);
    expect_equal(
        run_command(executable, "recover", directory.c_str(), "--database", database.c_str(), "2>", printed.c_str()),
        0);
    std::vector<std::string> expected_files;
    for (std::string const& line : before) expected_files.push_back(line.substr(0, line.rfind(' ')) + " 0");
    expect_true(manifest_files(manifest.c_str()) == expected_files);
    // A system collection without runs is left for the next open to reserve again
    std::vector<std::string> const collections = manifest_collections(original.c_str());
    std::vector<std::string> const recovered = manifest_collections(manifest.c_str());
    expect_true(std::includes(collections.begin(), collections.end(), recovered.begin(), recovered.end()));
    for (std::string const& line : collections)
        if (line.front() != '_') expect_true(std::binary_search(recovered.begin(), recovered.end(), line));
    expect_true(std::any_of(recovered.begin(), recovered.end(),
                            [](std::string const& line) { return line.starts_with("_collections "); }));
    expect_equal(run_command(executable, "--engine", "nvme", "--directory", directory.c_str(), "collection", "list",
                             ">", printed.c_str()),
                 0);
    sz::string_t listing;
    expect_true(read_file(printed.c_str(), listing) &&
                std::string_view {listing}.find("people") != std::string_view::npos);
    std::filesystem::remove_all(directory.c_str(), error);
    return {};
}

/** Recover walks the slots of a raw medium and sizes every run, strings ones too, from its header
 *  alone, which must match the bytes the manifest named it by. */
[[nodiscard]] case_result_t test_raw_recover_sizes_strings_runs(fixture_paths_t const& paths, char const* executable) {
    constexpr std::uint64_t slot_bytes = std::uint64_t {1} << 30;
    constexpr std::uint64_t slots = 8;
    std::string const device = fmt::format("{}/{}", paths.files.c_str(), "recovered.raw");
    std::string const input = fmt::format("{}/{}", paths.files.c_str(), "recovered.strings.arrows");
    std::string const printed = fmt::format("{}/{}", paths.files.c_str(), "recovered.raw.printed.json");
    std::string const original = fmt::format("{}/{}", paths.files.c_str(), "recovered.raw.original.json");
    std::string const address = std::string(device.c_str()) + "@0+8GiB";
    std::string const database {nvme_database_name_k};
    std::error_code error;
    std::filesystem::remove(device.c_str(), error);
    if (std::FILE* const created = std::fopen(device.c_str(), "wb")) std::fclose(created);
    std::filesystem::resize_file(device.c_str(), slots * slot_bytes, error);
    if (error) return case_result_t::skipped("needs a sparse stand-in medium");
    // The NVMe engine opens only where the build has it, which a listing of an empty medium probes
    if (run_command(executable, "--engine", "nvme", "--directory", address.c_str(), "collection", "list", ">",
                    printed.c_str(), "2>&1") != 0) {
        std::filesystem::remove(device.c_str(), error);
        return case_result_t::skipped(needs_nvme_engine_k);
    }
    if (!expect_true(write_arrow_stream(input.c_str(), modality_table(ustore_modality_strings_k)),
                     "the Arrow input is written"))
        return {};
    expect_equal(run_command(executable, "--engine", "nvme", "--directory", address.c_str(), "collection", "import",
                             "--collection", "words", "--input", input.c_str()),
                 0);

    expect_equal(
        run_command(executable, "manifest", address.c_str(), "--database", database.c_str(), ">", original.c_str()), 0);
    std::vector<std::string> const before = manifest_files(original.c_str());
    expect_true(!before.empty());
    expect_equal(
        run_command(executable, "recover", address.c_str(), "--database", database.c_str(), "2>", printed.c_str()), 0);
    expect_equal(
        run_command(executable, "manifest", address.c_str(), "--database", database.c_str(), ">", printed.c_str()), 0);
    std::vector<std::string> expected_files;
    for (std::string const& line : before) expected_files.push_back(line.substr(0, line.rfind(' ')) + " 0");
    expect_true(manifest_files(printed.c_str()) == expected_files);
    std::filesystem::remove(device.c_str(), error);
    std::filesystem::remove(input.c_str(), error);
    return {};
}

#pragma endregion Manifests

#pragma region CLI Conventions
#if !USTORE_OS_WINDOWS_

/** A CLI process with its standard streams on pipes: what the test writes, and what it reads. */
struct cli_process_t {
    pid_t pid = -1;
    int input = -1;
    int output = -1;
    int errors = -1;
};

/** Starts @p arguments, the executable first, with every @c NAME=value of @p variables added to its
 *  environment and its three standard streams on pipes. */
[[nodiscard]] cli_process_t start_cli(std::vector<std::string> const& arguments,
                                      std::vector<std::string> const& variables = {}) {
    int input[2], output[2], errors[2];
    if (::pipe(input) != 0 || ::pipe(output) != 0 || ::pipe(errors) != 0) return {};
    pid_t const pid = ::fork();
    if (pid == 0) {
        ::dup2(input[0], 0), ::dup2(output[1], 1), ::dup2(errors[1], 2);
        for (int descriptor : {input[0], input[1], output[0], output[1], errors[0], errors[1]}) ::close(descriptor);
        for (std::string const& variable : variables) ::putenv(const_cast<char*>(variable.c_str()));
        std::vector<char*> words;
        for (std::string const& argument : arguments) words.push_back(const_cast<char*>(argument.c_str()));
        words.push_back(nullptr);
        ::execv(words[0], words.data());
        ::_exit(127);
    }
    ::close(input[0]), ::close(output[1]), ::close(errors[1]);
    return {pid, input[1], output[0], errors[0]};
}

/** Appends what @p descriptor holds now to @p into, answering false once it reached its end. */
[[nodiscard]] bool drain_some(int descriptor, std::string& into) {
    char buffer[4096];
    ssize_t const got = ::read(descriptor, buffer, sizeof(buffer));
    if (got <= 0) return false;
    into.append(buffer, static_cast<std::size_t>(got));
    return true;
}

/** What a finished CLI run left: its exit status, or 128 plus the signal that ended it, and its two
 *  output streams. */
struct cli_run_t {
    int status = -1;
    std::string output;
    std::string errors;
};

/** Closes the input of @p process, reads both of its outputs to their ends and waits for it. */
[[nodiscard]] cli_run_t finish_cli(cli_process_t process, std::string errors_so_far = {}) {
    cli_run_t run;
    run.errors = std::move(errors_so_far);
    ::close(process.input);
    pollfd streams[2] {{process.output, POLLIN, 0}, {process.errors, POLLIN, 0}};
    std::size_t open = 2;
    while (open && ::poll(streams, 2, -1) > 0)
        for (pollfd& stream : streams) {
            if (stream.fd < 0 || !(stream.revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (drain_some(stream.fd, stream.fd == process.output ? run.output : run.errors)) continue;
            ::close(stream.fd);
            stream.fd = -1;
            --open;
        }
    int status = 0;
    ::waitpid(process.pid, &status, 0);
    run.status = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    return run;
}

/** Runs @p arguments to its end with @p variables in its environment and nothing on its input. */
[[nodiscard]] cli_run_t run_cli(std::vector<std::string> const& arguments,
                                std::vector<std::string> const& variables = {}) {
    cli_process_t const process = start_cli(arguments, variables);
    if (process.pid < 0) return {};
    return finish_cli(process);
}

/** @c --help prints the usage, each flag beside its default's variable, and exits with status 0. */
void test_cli_help_lists_variables(char const* executable) {
    cli_run_t const run = run_cli({executable, "--help"});
    expect_equal(run.status, 0);
    expect_true(run.output.find("Usage: ") == 0);
    expect_true(run.output.find("--directory <path>") != std::string::npos);
    expect_true(run.output.find("[env: USTORE_DIRECTORY]") != std::string::npos);
    expect_true(run.output.find("[env: USTORE_MEMORY_LIMIT]") != std::string::npos);
    expect_true(run.errors.empty());
}

/** A value that does not parse, given as a flag or a variable, prints one line naming the grammar
 *  and exits with 1 before anything opens. */
void test_cli_bad_value_one_line(char const* executable) {
    cli_run_t const flag = run_cli({executable, "--batch-rows", "0", "collection", "list"});
    expect_equal(flag.status, 1);
    expect_equal(flag.errors, std::string("--batch-rows=\"0\" does not parse, expected a positive count\n"));

    cli_run_t const bytes = run_cli({executable, "--memory-limit=64MB", "collection", "list"});
    expect_equal(bytes.status, 1);
    expect_equal(bytes.errors,
                 std::string("--memory-limit=\"64MB\" does not parse, expected bytes like 4096 or 24G\n"));

    cli_run_t const named = run_cli({executable, "--engine", "mongo", "collection", "list"});
    expect_equal(named.status, 1);
    expect_equal(named.errors,
                 std::string("--engine=\"mongo\" does not parse, expected ram, rocksdb, leveldb or nvme\n"));

    cli_run_t const variable = run_cli({executable, "collection", "list"}, {"USTORE_BATCH_ROWS=many"});
    expect_equal(variable.status, 1);
    expect_equal(variable.errors,
                 std::string("USTORE_BATCH_ROWS=\"many\" does not parse, expected a positive count\n"));
}

/** The engine and the directory come from @c USTORE_ENGINE and @c USTORE_DIRECTORY when no flag
 *  names them, and a flag overrides its variable. */
void test_cli_reads_variables(fixture_paths_t const& paths, config_t const& config, char const* executable) {
    std::string const engine = "USTORE_ENGINE=" + std::string(config.engine());
    std::string const directory = std::string("USTORE_DIRECTORY=") + paths.directory;
    cli_run_t const created = run_cli({executable, "collection", "create", "--collection", "from_variables"},
                                      {engine, directory});
    expect_equal(created.status, 0);
    {
        test_database_t database;
        expect_true(database.open(config) && lists_collection(database.get(), "from_variables"));
    }
    cli_run_t const overridden = run_cli({executable, "--collection", "from_variables", "collection", "drop"},
                                         {engine, directory, "USTORE_COLLECTION=missing"});
    expect_equal(overridden.status, 0);
    {
        test_database_t database;
        expect_true(database.open(config) && !lists_collection(database.get(), "from_variables"));
    }
}

/** Waits up to a minute for @p process to print a line holding @p marker to standard error,
 *  collecting what it printed into @p errors. */
[[nodiscard]] bool await_errors(cli_process_t const& process, std::string_view marker, std::string& errors) {
    for (int waited = 0; waited != 600 && errors.find(marker) == std::string::npos; ++waited) {
        pollfd stream {process.errors, POLLIN, 0};
        if (::poll(&stream, 1, 100) > 0 && !drain_some(process.errors, errors)) return false;
    }
    return errors.find(marker) != std::string::npos;
}

/** Writes all of @p bytes to @p descriptor, answering false once the reader went away. */
[[nodiscard]] bool write_all(int descriptor, std::string_view bytes) {
    while (!bytes.empty()) {
        ssize_t const put = ::write(descriptor, bytes.data(), bytes.size());
        if (put <= 0) return false;
        bytes.remove_prefix(static_cast<std::size_t>(put));
    }
    return true;
}

/** An import from standard input that SIGINT interrupts after its first progress line stops at the
 *  next batch, prints "stopped in import" and exits with 130; a stream of one row per batch, fed in
 *  two halves, holds it at a barrier until the signal lands. */
void test_cli_stops_on_sigint(fixture_paths_t const& paths, config_t const& config, char const* executable) {
    std::string const table_path = fmt::format("{}/{}", paths.files.c_str(), "stopping.in.arrows");
    std::string const batches_path = fmt::format("{}/{}", paths.files.c_str(), "stopping.batches.arrows");
    if (!expect_true(write_arrow_stream(table_path.c_str(), modality_table(ustore_modality_blobs_k)),
                     "the Arrow input is written"))
        return;
    std::string const engine(config.engine());
    bool const imported = expect_equal(
        run_cli({executable, "--engine", engine, "--directory", paths.directory, "collection", "import", "--collection",
                 "stopping_source", "--input", table_path.c_str()})
            .status,
        0);
    bool const exported = expect_equal(
        run_cli({executable, "--engine", engine, "--directory", paths.directory, "collection", "export", "--collection",
                 "stopping_source", "--output", batches_path.c_str(), "--batch-rows", "1"})
            .status,
        0);
    sz::string_t stream;
    if (!imported || !exported || !expect_true(read_file(batches_path.c_str(), stream), "the batches are written"))
        return;
    std::string_view const bytes = stream.view();

    // The CLI exits once it stops, so the second half may find no reader
    auto const previous_pipe = std::signal(SIGPIPE, SIG_IGN);
    cli_process_t const process = start_cli({executable, "--engine", engine, "--directory", paths.directory,
                                             "collection", "import", "--collection", "stopping_target", "--input",
                                             "-"});
    std::string errors;
    bool const fed = process.pid > 0 && write_all(process.input, bytes.substr(0, bytes.size() / 2));
    expect_true(fed && await_errors(process, "import: ", errors));
    if (process.pid > 0) ::kill(process.pid, SIGINT);
    [[maybe_unused]] bool const rest = process.pid > 0 && write_all(process.input, bytes.substr(bytes.size() / 2));
    cli_run_t const run = process.pid > 0 ? finish_cli(process, std::move(errors)) : cli_run_t {};
    std::signal(SIGPIPE, previous_pipe);

    expect_equal(run.status, 130);
    expect_true(run.errors.find("stopped in import\n") != std::string::npos);
    expect_true(run.errors.find("Imported") == std::string::npos);
    [[maybe_unused]] int const removed_table = std::remove(table_path.c_str());
    [[maybe_unused]] int const removed_batches = std::remove(batches_path.c_str());
}

/** An export SIGINT stops leaves a file already at its target as it was, byte for byte, and no
 *  temporary beside it: a FIFO at the temporary's name holds the export at a barrier once the pipe
 *  fills, until the signal lands. */
void test_cli_stop_keeps_existing_target(fixture_paths_t const& paths, config_t const& config, char const* executable) {
    std::string const input = fmt::format("{}/{}", paths.files.c_str(), "keeping.in.arrows");
    std::string const target = fmt::format("{}/{}", paths.files.c_str(), "keeping.out.arrows");
    std::string const temporary = fmt::format("{}/{}", paths.files.c_str(), "keeping.out.arrows.tmp");
    if (!expect_true(write_arrow_stream(input.c_str(), modality_table(ustore_modality_blobs_k)),
                     "the Arrow input is written"))
        return;
    std::string const engine(config.engine());
    expect_equal(run_cli({executable, "--engine", engine, "--directory", paths.directory, "collection", "import",
                          "--collection", "keeping_source", "--input", input.c_str()})
                     .status,
                 0);

    std::string const existing = "bytes an earlier export left, which a stopped one must keep\n";
    std::FILE* const file = std::fopen(target.c_str(), "wb");
    expect_true(file && std::fwrite(existing.data(), 1, existing.size(), file) == existing.size());
    if (file) expect_equal(std::fclose(file), 0);
    if (!expect_equal(::mkfifo(temporary.c_str(), 0600), 0)) return;

    cli_process_t const process = start_cli({executable, "--engine", engine, "--directory", paths.directory,
                                             "collection", "export", "--collection", "keeping_source", "--output",
                                             target.c_str(), "--batch-rows", "1"});
    int const reader = ::open(temporary.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    std::string errors;
    expect_true(process.pid > 0 && reader >= 0 && await_errors(process, "export: ", errors));
    if (process.pid > 0) ::kill(process.pid, SIGINT);
    if (reader >= 0) {
        // Draining lets the blocked write finish, so the export reaches its next barrier and stops
        ::fcntl(reader, F_SETFL, ::fcntl(reader, F_GETFL) & ~O_NONBLOCK);
        std::string drained;
        while (drain_some(reader, drained)) {}
        ::close(reader);
    }
    cli_run_t const run = process.pid > 0 ? finish_cli(process, std::move(errors)) : cli_run_t {};

    expect_equal(run.status, 130);
    expect_true(run.errors.find("stopped in export\n") != std::string::npos);
    sz::string_t kept;
    expect_true(read_file(target.c_str(), kept) && kept.view() == sz::string_view_t {existing});
    struct ::stat left {};
    expect_true(::stat(temporary.c_str(), &left) != 0);
    [[maybe_unused]] int const removed_input = std::remove(input.c_str());
    [[maybe_unused]] int const removed_target = std::remove(target.c_str());
    [[maybe_unused]] int const removed_temporary = std::remove(temporary.c_str());
}

#endif
#pragma endregion CLI Conventions

/** Runs every case against the fixtures written under the directory of @p config, and the CLI at
 *  @p executable unless it is empty. */
void run_cases(config_t const& config, std::string_view executable) noexcept {
    fixture_paths_t const paths {config.directory()};
    if (!write_fixtures(paths)) {
        run_test_case("write_fixtures", [] { expect_true(false, "the fixtures are written"); });
        return;
    }

    char const* const documents_ndjson = paths.documents_ndjson.c_str();
    char const* const documents_csv = paths.documents_csv.c_str();
    char const* const edges_ndjson = paths.edges_ndjson.c_str();
    char const* const edges_csv = paths.edges_csv.c_str();
    char const* const exported_ndjson = paths.exported_ndjson.c_str();
    char const* const exported_csv = paths.exported_csv.c_str();

    run_test_case(engine_label_t {"test_graph_round_trip", "edges_ndjson, exported_ndjson"}.c_str(),
                  test_graph_round_trip, config, edges_ndjson, exported_ndjson);
    run_test_case(engine_label_t {"test_graph_round_trip", "edges_ndjson, exported_csv"}.c_str(), test_graph_round_trip,
                  config, edges_ndjson, exported_csv);
    run_test_case(engine_label_t {"test_graph_round_trip", "edges_csv, exported_ndjson"}.c_str(), test_graph_round_trip,
                  config, edges_csv, exported_ndjson);
    run_test_case(engine_label_t {"test_graph_round_trip", "edges_csv, exported_csv"}.c_str(), test_graph_round_trip,
                  config, edges_csv, exported_csv);

    field_selection_t const whole = field_selection_t::whole_k;
    field_selection_t const selected = field_selection_t::selected_k;
    run_test_case(engine_label_t {"test_documents_round_trip", "documents_ndjson, exported_ndjson, whole"}.c_str(),
                  test_documents_round_trip, config, documents_ndjson, exported_ndjson, whole);
    run_test_case(engine_label_t {"test_documents_round_trip", "documents_ndjson, exported_csv, whole"}.c_str(),
                  test_documents_round_trip, config, documents_ndjson, exported_csv, whole);
    run_test_case(engine_label_t {"test_documents_round_trip", "documents_csv, exported_ndjson, whole"}.c_str(),
                  test_documents_round_trip, config, documents_csv, exported_ndjson, whole);
    run_test_case(engine_label_t {"test_documents_round_trip", "documents_csv, exported_csv, whole"}.c_str(),
                  test_documents_round_trip, config, documents_csv, exported_csv, whole);
    run_test_case(engine_label_t {"test_documents_round_trip", "documents_ndjson, exported_ndjson, selected"}.c_str(),
                  test_documents_round_trip, config, documents_ndjson, exported_ndjson, selected);
    run_test_case(engine_label_t {"test_documents_round_trip", "documents_ndjson, exported_csv, selected"}.c_str(),
                  test_documents_round_trip, config, documents_ndjson, exported_csv, selected);
    run_test_case(engine_label_t {"test_documents_round_trip", "documents_csv, exported_ndjson, selected"}.c_str(),
                  test_documents_round_trip, config, documents_csv, exported_ndjson, selected);
    run_test_case(engine_label_t {"test_documents_round_trip", "documents_csv, exported_csv, selected"}.c_str(),
                  test_documents_round_trip, config, documents_csv, exported_csv, selected);

    run_test_case(engine_label_t {"test_graph_import_rejects_invalid_tasks", "edges_ndjson"}.c_str(),
                  test_graph_import_rejects_invalid_tasks, config, edges_ndjson);
    run_test_case(engine_label_t {"test_graph_import_rejects_invalid_tasks", "edges_csv"}.c_str(),
                  test_graph_import_rejects_invalid_tasks, config, edges_csv);
    run_test_case(engine_label_t {"test_graph_export_rejects_invalid_tasks", "exported_ndjson"}.c_str(),
                  test_graph_export_rejects_invalid_tasks, config, exported_ndjson);
    run_test_case(engine_label_t {"test_graph_export_rejects_invalid_tasks", "exported_csv"}.c_str(),
                  test_graph_export_rejects_invalid_tasks, config, exported_csv);
    run_test_case(engine_label_t {"test_docs_import_rejects_invalid_tasks", "documents_ndjson"}.c_str(),
                  test_docs_import_rejects_invalid_tasks, config, documents_ndjson);
    run_test_case(engine_label_t {"test_docs_import_rejects_invalid_tasks", "documents_csv"}.c_str(),
                  test_docs_import_rejects_invalid_tasks, config, documents_csv);
    run_test_case(engine_label_t {"test_docs_export_rejects_invalid_tasks", "exported_ndjson"}.c_str(),
                  test_docs_export_rejects_invalid_tasks, config, exported_ndjson);
    run_test_case(engine_label_t {"test_docs_export_rejects_invalid_tasks", "exported_csv"}.c_str(),
                  test_docs_export_rejects_invalid_tasks, config, exported_csv);

    // The CLI path views into `argv`, so it ends in a null terminator
    if (executable.empty())
        run_test_case(engine_label_t {"test_cli_round_trip"}.c_str(),
                      [] { return case_result_t::skipped(needs_cli_k); });
    else
        run_test_case(engine_label_t {"test_cli_round_trip"}.c_str(), test_cli_round_trip, paths, config,
                      executable.data());
#if !USTORE_OS_WINDOWS_
    if (executable.empty()) {
        run_test_case("test_cli_help_lists_variables", [] { return case_result_t::skipped(needs_cli_k); });
        run_test_case("test_cli_bad_value_one_line", [] { return case_result_t::skipped(needs_cli_k); });
        run_test_case(engine_label_t {"test_cli_reads_variables"}.c_str(),
                      [] { return case_result_t::skipped(needs_cli_k); });
        run_test_case(engine_label_t {"test_cli_stops_on_sigint"}.c_str(),
                      [] { return case_result_t::skipped(needs_cli_k); });
        run_test_case(engine_label_t {"test_cli_stop_keeps_existing_target"}.c_str(),
                      [] { return case_result_t::skipped(needs_cli_k); });
    }
    else {
        run_test_case("test_cli_help_lists_variables", test_cli_help_lists_variables, executable.data());
        run_test_case("test_cli_bad_value_one_line", test_cli_bad_value_one_line, executable.data());
        run_test_case(engine_label_t {"test_cli_reads_variables"}.c_str(), test_cli_reads_variables, paths, config,
                      executable.data());
        run_test_case(engine_label_t {"test_cli_stops_on_sigint"}.c_str(), test_cli_stops_on_sigint, paths, config,
                      executable.data());
        run_test_case(engine_label_t {"test_cli_stop_keeps_existing_target"}.c_str(),
                      test_cli_stop_keeps_existing_target, paths, config, executable.data());
    }
#endif

    constexpr ustore_modality_t modalities[] {ustore_modality_blobs_k, ustore_modality_docs_k,
                                              ustore_modality_strings_k, ustore_modality_graph_k,
                                              ustore_modality_pages_k};
    constexpr char const* modality_constants[] {"ustore_modality_blobs_k", "ustore_modality_docs_k",
                                                "ustore_modality_strings_k", "ustore_modality_graph_k",
                                                "ustore_modality_pages_k"};
    for (std::size_t index = 0; index != std::size(modalities); ++index) {
        engine_label_t const label {"test_arrow_round_trip", modality_constants[index]};
        if (executable.empty()) run_test_case(label.c_str(), [] { return case_result_t::skipped(needs_cli_k); });
        else run_test_case(label.c_str(), test_arrow_round_trip, paths, config, executable.data(), modalities[index]);
    }
    if (executable.empty()) {
        run_test_case(engine_label_t {"test_arrow_stdout_only_arrow"}.c_str(),
                      [] { return case_result_t::skipped(needs_cli_k); });
        run_test_case(engine_label_t {"test_arrow_file_direct_alignment"}.c_str(),
                      [] { return case_result_t::skipped(needs_cli_k); });
        run_test_case(engine_label_t {"test_arrow_bulk_lands_or_refuses"}.c_str(),
                      [] { return case_result_t::skipped(needs_cli_k); });
        run_test_case(engine_label_t {"test_arrow_sorted_inputs_merge"}.c_str(),
                      [] { return case_result_t::skipped(needs_cli_k); });
        run_test_case(engine_label_t {"test_snapshot_export_import"}.c_str(),
                      [] { return case_result_t::skipped(needs_cli_k); });
        run_test_case("test_manifest_print_and_recover", [] { return case_result_t::skipped(needs_cli_k); });
        run_test_case("test_raw_recover_sizes_strings_runs", [] { return case_result_t::skipped(needs_cli_k); });
    }
    else {
        run_test_case(engine_label_t {"test_arrow_stdout_only_arrow"}.c_str(), test_arrow_stdout_only_arrow, paths,
                      config, executable.data());
        run_test_case(engine_label_t {"test_arrow_file_direct_alignment"}.c_str(), test_arrow_file_direct_alignment,
                      paths, config, executable.data());
        run_test_case(engine_label_t {"test_arrow_bulk_lands_or_refuses"}.c_str(), test_arrow_bulk_lands_or_refuses,
                      paths, config, executable.data());
        run_test_case(engine_label_t {"test_arrow_sorted_inputs_merge"}.c_str(), test_arrow_sorted_inputs_merge, paths,
                      config, executable.data());
        run_test_case(engine_label_t {"test_snapshot_export_import"}.c_str(), test_snapshot_export_import, paths,
                      config, executable.data());
        run_test_case("test_manifest_print_and_recover", test_manifest_print_and_recover, paths, executable.data());
        run_test_case("test_raw_recover_sizes_strings_runs", test_raw_recover_sizes_strings_runs, paths,
                      executable.data());
    }
}

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;
    start_test_run(argc, argv);
    char const* const cli = setting_text(argc, argv, "USTORE_CLI", "--cli");

    auto const maybe_directory = scratch_directory_t::make(test_run.settings.directory);
    if (!maybe_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    char const* const directory = maybe_directory->path();
    config_t const config {test_run.settings.engine, directory};

    if (directory) run_cases(config, cli ? cli : "");
    else run_test_case("run_cases", [] { return case_result_t::skipped(needs_directory_k); });
    return finish_test_run();
}
