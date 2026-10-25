/**
 *  @file test/docs/json_paths.cpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief Checks that fields located by StringZilla's subdocuments engine in `ustore/docs/json_paths.hpp` resolve and
 *      convert exactly as yyjson's parse and lookup do.
 *
 *  Random documents mix nested objects and arrays, duplicate and escaped keys, every kind of number, escaped and
 *  non-ASCII strings, comments and non-finite literals. Every field of a fixed set, plain keys and JSON Pointers with
 *  indices, escapes and invalid tokens alike, resolves through both paths, and every value converts into every column
 *  type, so a difference in presence, type, payload, conversion or printed text fails the case.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`, `std::uint8_t`
#include <cstring> // `std::memcmp`

#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <vector>      // `std::vector`

#include <fmt/format.h> // `fmt::println`
#include <yyjson.h>     // `yyjson_val`

#include <stringzilla/capabilities.h> // `sz_capabilities_enabled_cpu`

#include "ustore/docs/json_paths.hpp"       // `resolve_fields`, `append_json_paths`
#include "ustore/docs/json_values.hpp"      // `json_parse`, `json_lookup`, `gather_scalar`, `json_to_string`
#include "ustore/utilities/ram_buffers.hpp" // `arena_free`, `arena_allocator`, `growing_strings`

#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_eq`, `random_t`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::docs;
using namespace unum::ustore::utilities;

namespace unum::ustore::test {

/** The StringZilla kernels this CPU allows, probed once as `src/` does before it resolves. */
inline sz_capability_t stringzilla_capabilities() noexcept {
    static sz_capability_t const capabilities = []() noexcept {
        sz_capability_t probed = 0;
        return sz_capabilities_enabled_cpu(&probed) == sz_success_k ? probed : sz_capability_t {0};
    }();
    return capabilities;
}

/** An arena freed with every block it grew when the test ends. */
class scoped_arena_t {
    ustore_arena_t arena_ = new (std::nothrow) ustore_arena_handle_t {};

  public:
    scoped_arena_t() noexcept = default;
    scoped_arena_t(scoped_arena_t const&) = delete;
    scoped_arena_t& operator=(scoped_arena_t const&) = delete;
    ~scoped_arena_t() noexcept { arena_free(arena_); }

    operator ustore_arena_t() const noexcept { return arena_; }
};

/** Documents as a read answers them: a u64 tape, presence bits, and the count. */
struct documents_batch_t {
    std::vector<std::string> texts;
    std::vector<bool> present;
    std::string bytes;
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint8_t> presences;

    json_documents_t view() {
        bytes.clear(), offsets.assign(1, 0), presences.assign((texts.size() + 7) / 8, 0);
        for (std::size_t index = 0; index != texts.size(); ++index) {
            if (present[index]) bytes += texts[index], presences[index / 8] |= std::uint8_t(1u << (index % 8));
            offsets.push_back(bytes.size());
        }
        json_documents_t documents;
        documents.tape = u64_tape(bytes.data(), offsets.data());
        documents.presences = presences.data();
        documents.count = texts.size();
        return documents;
    }
};

#pragma region Random Documents

/** Writes random lenient JSON: nested containers, duplicate and escaped keys, numbers of every shape and strings. */
class document_writer_t {
    random_t& random_;
    bool lenient_;

    static constexpr std::string_view keys_k[] = {
        "a", "b", "c", "0", "1", "01", "-", "a/b", "t~x", "it's", "q\\\"uote", "\\\\back", "\xC3\xA9", "", "\\u0061",
    };
    static constexpr std::string_view numbers_k[] = {
        "0",
        "-0",
        "7",
        "-7",
        "255",
        "256",
        "-129",
        "65535",
        "1.5",
        "-2.25",
        "1e3",
        "1E-3",
        "-2.5e+2",
        "0.1",
        "3.0",
        "123456789012",
        "4294967296",
        "-2147483649",
        "9223372036854775807",
        "-9223372036854775808",
        "9223372036854775808",
        "18446744073709551615",
        "18446744073709551616",
        "-9223372036854775809",
        "1e308",
        "65519.9",
        "65520",
        "1e-310",
        "100000000000000000000000",
        "0.30000000000000004",
    };
    static constexpr std::string_view strings_k[] = {
        "\"\"",
        "\"plain\"",
        "\"12\"",
        "\"-3.5\"",
        "\"true\"",
        "\"x\\ny\"",
        "\"tab\\there\"",
        "\"quote\\\"d\"",
        "\"back\\\\slash\"",
        "\"sl\\/ash\"",
        "\"\\u00e9t\\u00E9\"",
        "\"\\ud83d\\ude00\"",
        "\"\xC3\xA9t\xC3\xA9\"",
        "\"\\u0031\\u0032\"",
        "\"123e4567-e89b-12d3-a456-426614174000\"",
        "\"123E4567-E89B-12D3-A456-42661417400Z\"",
        "\"\\u0000nul\"",
        "\"http://example.com/a/b\"",
        "\"/* not a comment */\"",
    };
    static constexpr std::string_view lenient_k[] = {"NaN", "Infinity", "-Infinity", "nan", "inf"};

    std::string_view pick(std::span<std::string_view const> pool) noexcept { return pool[random_.below(pool.size())]; }

    void blank(std::string& out) {
        constexpr std::string_view blanks_k[] = {"", "", " ", "\n", "\t ", "\r\n  "};
        out += pick(blanks_k);
        if (lenient_ && random_.below(16) == 0) out += random_.below(2) ? "/* {\"a\": [ */" : "// ] } \"\n";
    }

    void scalar(std::string& out) {
        switch (random_.below(lenient_ ? 7 : 6)) {
        case 0: out += pick(numbers_k); break;
        case 1: out += pick(numbers_k); break;
        case 2:
        case 3: out += pick(strings_k); break;
        case 4: out += random_.below(2) ? "true" : "false"; break;
        case 5: out += "null"; break;
        default: out += pick(lenient_k); break;
        }
    }

  public:
    document_writer_t(random_t& random, bool lenient) noexcept : random_(random), lenient_(lenient) {}

    void value(std::string& out, std::size_t depth) {
        std::uint64_t const kind = depth >= 3 ? 2 : random_.below(4);
        if (kind == 0) {
            out += '{';
            std::size_t const members = random_.below(5);
            for (std::size_t member = 0; member != members; ++member) {
                if (member) out += ',';
                blank(out);
                out += '"', out += pick(keys_k), out += '"';
                blank(out);
                out += ':';
                blank(out);
                value(out, depth + 1);
                blank(out);
            }
            out += '}';
        }
        else if (kind == 1) {
            out += '[';
            std::size_t const elements = random_.below(4);
            for (std::size_t element = 0; element != elements; ++element) {
                if (element) out += ',';
                blank(out);
                value(out, depth + 1);
                blank(out);
            }
            out += ']';
        }
        else scalar(out);
    }

    /** A whole document, an object at its root most of the time. */
    std::string document() {
        std::string out;
        blank(out);
        if (random_.below(8)) {
            out += '{';
            std::size_t const members = 1 + random_.below(6);
            for (std::size_t member = 0; member != members; ++member) {
                if (member) out += ',';
                out += '"', out += pick(keys_k), out += "\":";
                blank(out);
                value(out, 1);
            }
            out += '}';
        }
        else value(out, 0);
        blank(out);
        return out;
    }
};

/** The fields every random batch resolves: keys, pointers with indices, escapes, invalid tokens and the root. */
constexpr std::string_view fields_k[] = {
    "",      "a",     "b",     "0",    "01",  "a/b",  "it's", "q\"uote", "\\back",    "\xC3\xA9", "/",       "/a",
    "/b",    "/0",    "/1",    "/01",  "/-",  "/a/b", "/a/0", "/a/1",    "/a/01",     "/a/-",     "/b/a",    "/c/a/b",
    "/a~1b", "/t~0x", "/it's", "/a~2", "/a~", "/a/",  "/0/a", "/1/0/a",  "/\xC3\xA9", "/q\"uote", "/\\back", "/a/b/c/0",
};

#pragma endregion Random Documents

#pragma region Comparisons

/** Counts the differences between @p oracle and @p candidate in every conversion a gather or a read makes. */
std::size_t compare_values(yyjson_val* oracle, yyjson_val* candidate, bool containers_parsed, ustore_arena_t arena,
                           std::string_view context) {
    std::size_t failures = 0;
    auto const report_difference = [&](char const* what) {
        fmt::println(stderr, "  {} differs for {}", what, context);
        ++failures;
    };
    if (!oracle != !candidate) {
        report_difference("presence");
        return failures;
    }
    if (!oracle) return failures;
    if (yyjson_get_type(oracle) != yyjson_get_type(candidate)) report_difference("type");
    if (yyjson_is_ctn(oracle) && !containers_parsed) return failures;

    auto [oracle_text, oracle_status] = json_dump({oracle, nullptr}, arena);
    auto [candidate_text, candidate_status] = json_dump({candidate, nullptr}, arena);
    if (oracle_status.failed() || candidate_status.failed() || oracle_text.size() != candidate_text.size() ||
        std::memcmp(oracle_text.data(), candidate_text.data(), oracle_text.size()) != 0)
        report_difference("dump");

    auto const compare_cell = [&](char const* what, auto gather) {
        ustore_byte_t oracle_cell[16] = {}, candidate_cell[16] = {};
        conversion_t const oracle_conversion = gather(oracle, oracle_cell);
        conversion_t const candidate_conversion = gather(candidate, candidate_cell);
        if (oracle_conversion != candidate_conversion || std::memcmp(oracle_cell, candidate_cell, 16) != 0)
            report_difference(what);
    };
    compare_cell("bool", [](yyjson_val* value, ustore_byte_t* cell) { return gather_scalar<bool>(value, cell, 0); });
    compare_cell("f64", [](yyjson_val* value, ustore_byte_t* cell) { return gather_scalar<double>(value, cell, 0); });
    compare_cell("f32", [](yyjson_val* value, ustore_byte_t* cell) { return gather_scalar<float>(value, cell, 0); });
    compare_cell("f16", [](yyjson_val* value, ustore_byte_t* cell) { return gather_half(value, cell, 0); });
    compare_cell("i64",
                 [](yyjson_val* value, ustore_byte_t* cell) { return gather_scalar<std::int64_t>(value, cell, 0); });
    compare_cell("i32",
                 [](yyjson_val* value, ustore_byte_t* cell) { return gather_scalar<std::int32_t>(value, cell, 0); });
    compare_cell("i16",
                 [](yyjson_val* value, ustore_byte_t* cell) { return gather_scalar<std::int16_t>(value, cell, 0); });
    compare_cell("i8",
                 [](yyjson_val* value, ustore_byte_t* cell) { return gather_scalar<std::int8_t>(value, cell, 0); });
    compare_cell("u64",
                 [](yyjson_val* value, ustore_byte_t* cell) { return gather_scalar<std::uint64_t>(value, cell, 0); });
    compare_cell("u32",
                 [](yyjson_val* value, ustore_byte_t* cell) { return gather_scalar<std::uint32_t>(value, cell, 0); });
    compare_cell("u16",
                 [](yyjson_val* value, ustore_byte_t* cell) { return gather_scalar<std::uint16_t>(value, cell, 0); });
    compare_cell("u8",
                 [](yyjson_val* value, ustore_byte_t* cell) { return gather_scalar<std::uint8_t>(value, cell, 0); });
    compare_cell("uuid", [](yyjson_val* value, ustore_byte_t* cell) { return gather_uuid(value, cell, 0); });

    printed_number_buffer_t oracle_buffer, candidate_buffer;
    std::string_view oracle_string, candidate_string;
    if (json_to_string(oracle, oracle_buffer, oracle_string) !=
            json_to_string(candidate, candidate_buffer, candidate_string) ||
        oracle_string != candidate_string)
        report_difference("string");
    return failures;
}

/** Resolves every field of @p fields over @p batch through both paths, counting differences, and the batches the
 *  subdocuments path handed back to yyjson in @p fallbacks. */
std::size_t compare_batch(documents_batch_t& batch, std::span<std::string_view const> fields, bool containers_parsed,
                          std::size_t& fallbacks) {
    std::size_t failures = 0;
    scoped_arena_t arena;
    json_documents_t const documents = batch.view();
    std::size_t const cells_count = documents.count * fields.size();
    std::vector<yyjson_val*> values(cells_count);
    auto const cell_of = [&](std::size_t cell) noexcept {
        return field_cell_t {cell % documents.count, cell / documents.count};
    };
    if (!resolve_fields(documents, fields, cells_count, cell_of, containers_parsed, arena, stringzilla_capabilities(),
                        values)) {
        ++fallbacks;
        return failures;
    }
    for (std::size_t document_index = 0; document_index != documents.count; ++document_index) {
        std::string_view const document = documents[document_index];
        auto [parsed, status] = json_parse(document, arena);
        failures += expect(status.succeeded());
        yyjson_val* const root = yyjson_doc_get_root(parsed.handle);
        for (std::size_t field_index = 0; field_index != fields.size(); ++field_index) {
            std::string const context = fmt::format("field '{}' of document {}: {}", fields[field_index],
                                                    document_index, document);
            failures += compare_values(json_lookup(root, fields[field_index]),
                                       values[field_index * documents.count + document_index], containers_parsed, arena,
                                       context);
        }
    }
    return failures;
}

#pragma endregion Comparisons

#pragma region Cases

/** Fields translate into chains of JSONPath prefixes, with index unions, escapes, and nothing for invalid pointers. */
std::size_t json_paths_translation() {
    std::size_t failures = 0;
    scoped_arena_t arena;
    struct translation_t {
        std::string_view field;
        std::vector<std::string_view> paths;
    };
    std::vector<translation_t> const translations {
        {"", {"$"}},
        {"k", {"$['k']"}},
        {"/k", {"$['k']"}},
        {"/a/b~1c", {"$['a']", "$['a']['b/c']"}},
        {"/t~0", {"$['t~']"}},
        {"/0", {"$['0',0]"}},
        {"/a/12", {"$['a']", "$['a']['12',12]"}},
        {"/01", {"$['01']"}},
        {"/-", {"$['-']"}},
        {"/", {"$['']"}},
        {"/a/", {"$['a']", "$['a']['']"}},
        {"it's\\", {"$['it\\'s\\\\']"}},
        {"/\x01", {"$['\\u0001']"}},
        {"a/b", {"$['a/b']"}},
        {"/a~2", {}},
        {"/a~", {}},
    };
    for (translation_t const& translation : translations) {
        growing_strings<arena_allocator<byte_t>> paths {ustore_arena_t {arena}};
        st::basic_vector<char, arena_allocator<char>> path {{ustore_arena_t {arena}}};
        st::basic_vector<std::size_t, arena_allocator<std::size_t>> chain {{ustore_arena_t {arena}}};
        failures += expect(append_json_paths(translation.field, paths, path, chain).succeeded());
        failures += expect_eq(chain.size(), translation.paths.size());
        failures += expect_eq(paths.size(), translation.paths.size());
        for (std::size_t index = 0; index != chain.size() && index != translation.paths.size(); ++index)
            failures += expect_eq(string_of(paths[chain[index]]), translation.paths[index]);
    }

    // Fields sharing a prefix share its path
    growing_strings<arena_allocator<byte_t>> paths {ustore_arena_t {arena}};
    st::basic_vector<char, arena_allocator<char>> path {{ustore_arena_t {arena}}};
    st::basic_vector<std::size_t, arena_allocator<std::size_t>> chain {{ustore_arena_t {arena}}};
    for (std::string_view field : {"/user/id", "/user/name", "user", "/user/name"})
        failures += expect(append_json_paths(field, paths, path, chain).succeeded());
    failures += expect_eq(paths.size(), 3u);
    failures += expect_eq(chain.size(), 7u);
    return failures;
}

/** Slashes inside strings resolve through the engine, while a comment hands its batch back to yyjson wherever it lies,
 *  even inside a container the engine skips, where it could hide brackets and members from its count. */
std::size_t json_paths_comments() {
    std::size_t failures = 0, fallbacks = 0;
    std::string_view const fields[] = {"a", "/a", "/x/y", ""};
    documents_batch_t strict;
    strict.texts = {
        R"({"a": 1})",
        R"({"a": "http://x/y"})",
        R"({"a": "\"//\""})",
        R"({"a\\": "/*", "x": {"y": "\/"}})",
        R"({"a":"https://x//y","b":"ftp://z/*"})",
        R"({"x": {"y": "s3://z", "z": ["//", "/*"]}, "a": 2})",
    };
    strict.present.assign(strict.texts.size(), true);
    failures += compare_batch(strict, fields, true, fallbacks);
    failures += compare_batch(strict, fields, false, fallbacks);
    failures += expect_eq(fallbacks, 0u);

    scoped_arena_t arena;
    for (std::string_view const text : {
             R"({"x": [1 /* ] , "a": 7, "z": [ */ ], "a": 5})",
             "{\"x\": {\"y\": 1 // }, \"a\": 9\n}, \"a\": 6}",
             R"({"a": 1 /* c */})",
             "{\"a\": \"//\" // c\n}",
             R"({"a\\":1/**/})",
             "{\"a\":1,\"b\"://c\n2}",
             "{\"a\":\"x\",// https://\n\"b\":1}",
             "// c\n{\"a\": 3}",
         }) {
        failures += expect(json_parse(text, arena).status().succeeded());
        documents_batch_t batch;
        batch.texts = {R"({"a": 4})", std::string(text)};
        batch.present = {true, true};
        std::size_t handed_back = 0;
        failures += compare_batch(batch, fields, true, handed_back);
        failures += compare_batch(batch, fields, false, handed_back);
        failures += expect_eq(handed_back, 2u);
    }
    return failures;
}

/** Duplicate keys resolve through the first member, as yyjson's lookup does, at every depth. */
std::size_t json_paths_duplicates() {
    std::size_t failures = 0, fallbacks = 0;
    documents_batch_t batch;
    batch.texts = {
        R"({"a": {"x": 1}, "a": {"b": 2}})", R"({"a": 1, "a": 2})",          R"({"a": {"b": 1}, "a": {"b": 2}})",
        R"({"a": [5], "a": {"0": 6}})",      R"({"a": {"0": 7}, "a": [8]})", R"({"a": 3, "a": {"b": 4}})",
        R"([{"a": 1}, {"a": 2}])",
    };
    batch.present.assign(batch.texts.size(), true);
    std::string_view const fields[] = {"a", "/a", "/a/b", "/a/0", "/a/x", "/0/a", "/1/a", ""};
    failures += compare_batch(batch, fields, true, fallbacks);
    failures += compare_batch(batch, fields, false, fallbacks);
    failures += expect_eq(fallbacks, 0u);
    return failures;
}

/** Missing and empty documents hold no field, not even the root. */
std::size_t json_paths_missing() {
    std::size_t failures = 0, fallbacks = 0;
    documents_batch_t batch;
    batch.texts = {R"({"a": 1})", "", R"({"a": 2})", R"({"a": 3})"};
    batch.present = {true, true, false, true};
    std::string_view const fields[] = {"", "a", "/a"};
    failures += compare_batch(batch, fields, true, fallbacks);
    failures += compare_batch(batch, fields, false, fallbacks);
    failures += expect_eq(fallbacks, 0u);
    return failures;
}

/** Selected values yyjson would refuse hand the batch back, so the caller's parse reproduces the refusal. */
std::size_t json_paths_refusals() {
    std::size_t failures = 0;
    std::string_view const refused[] = {
        "{\"a\": \"raw\x01control\"}",
        "{\"a\": \"bad\xC3utf8\"}",
        R"({"a": "\q"})",
        R"({"a": "\ud800"})",
        R"({"a": 01})",
        R"({"a": NaN})",
        R"({"a": [1, 2,]})",
    };
    std::string_view const fields[] = {"a"};
    for (std::string_view text : refused) {
        documents_batch_t batch;
        batch.texts = {std::string(text)};
        batch.present = {true};
        scoped_arena_t arena;
        json_documents_t const documents = batch.view();
        yyjson_val* value = nullptr;
        auto const cell_of = [](std::size_t) noexcept { return field_cell_t {0, 0}; };
        bool const resolved = resolve_fields(documents, fields, 1, cell_of, true, arena, stringzilla_capabilities(),
                                             {&value, 1});
        auto [parsed, status] = json_parse(text, arena);
        // NaN is lenient JSON, which yyjson reads and the engine hands back
        if (status.failed() || !resolved) continue;
        failures += compare_values(json_lookup(yyjson_doc_get_root(parsed.handle), "a"), value, true, arena, text);
    }
    for (std::string_view text : std::span<std::string_view const>(refused).first(4)) {
        documents_batch_t batch;
        batch.texts = {std::string(text)};
        batch.present = {true};
        scoped_arena_t arena;
        yyjson_val* value = nullptr;
        auto const cell_of = [](std::size_t) noexcept { return field_cell_t {0, 0}; };
        failures += expect(
            !resolve_fields(batch.view(), fields, 1, cell_of, true, arena, stringzilla_capabilities(), {&value, 1}));
    }
    return failures;
}

/** Random batches of strict and lenient documents resolve and convert exactly as yyjson does. */
std::size_t json_paths_random(std::uint64_t seed) {
    std::size_t failures = 0, fallbacks = 0, batches = 0;
    random_t random {seed};
    for (std::size_t round = 0; round != 400; ++round) {
        bool const lenient = round % 4 == 3;
        document_writer_t writer {random, lenient};
        documents_batch_t batch;
        std::size_t const count = 1 + random.below(24);
        scoped_arena_t probe;
        while (batch.texts.size() != count) {
            std::string text = writer.document();
            // Only documents yyjson reads are compared, as only those can reach a read
            if (json_parse(std::string_view {text}, probe).status().failed()) continue;
            batch.texts.push_back(std::move(text));
            batch.present.push_back(random.below(10) != 0);
        }
        failures += compare_batch(batch, fields_k, true, fallbacks);
        failures += compare_batch(batch, fields_k, false, fallbacks);
        batches += 2;
    }
    fmt::println("  {} of {} batches handed back to yyjson", fallbacks, batches);
    failures += expect(fallbacks * 2 < batches);
    return failures;
}

#pragma endregion Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    report(totals, "json_paths.translation", json_paths_translation);
    report(totals, "json_paths.comments", json_paths_comments);
    report(totals, "json_paths.duplicates", json_paths_duplicates);
    report(totals, "json_paths.missing", json_paths_missing);
    report(totals, "json_paths.refusals", json_paths_refusals);
    report(totals, "json_paths.random", json_paths_random, std::uint64_t {options.seed.value});

    return suite_exit_code(totals);
}
