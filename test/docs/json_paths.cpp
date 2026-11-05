/**
 *  @file test/docs/json_paths.cpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief Checks that fields extracted by StringZilla's subdocuments engine in `ustore/docs/json_paths.hpp` hold what
 *      yyjson's parse and lookup find in strict JSON, and that lenient JSON reads as malformed.
 *
 *  Random documents mix nested objects and arrays, duplicate and escaped keys, every kind of number, escaped and
 *  non-ASCII strings, comments and non-finite literals. Every field of a fixed set, plain keys and JSON Pointers with
 *  indices, escapes and invalid tokens alike, extracts and resolves through both, and every value converts into every
 *  column type, so a difference in presence, type, payload, conversion or printed text fails the case. A container
 *  prints minified with its strings and numbers as written, so it is compared once yyjson reads it back.
 *
 *  The row steps of the device gather in @c ustore/docs/vram_gather.cuh run host-compiled over the
 *  same batches, on a page laid out from the same subdocuments pass a device walk makes, and every
 *  row they do not leave to the host must match what the host gather and read export, while a batch
 *  the host refuses must be left to it.
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

#include <stringzilla/capabilities.h> // `sz_capabilities_compiled_cpu_mask`, `sz_capabilities_enabled_cpu`

#include "ustore/docs/json_paths.hpp"       // `extract_fields`, `json_path_of`, `json_text_of`
#include "ustore/docs/vram_gather.cuh"      // `gather_page_t`, `gather_scalar_rows`, `measure_text_rows`
#include "ustore/docs/json_values.hpp"      // `json_parse`, `json_lookup`, `gather_scalar`, `json_to_string`
#include "ustore/utilities/ram_buffers.hpp" // `arena_free`, `arena_allocator`, `strings_ladder`

#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_eq`, `random_t`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::docs;
using namespace unum::ustore::utilities;

namespace unum::ustore::test {

/** The widest StringZilla kernels this build compiled. */
using strings_kit_t = strings_ladder<sz_capabilities_compiled_cpu_mask()>::strings_kit_t;

/** The StringZilla capabilities this machine runs among those the build compiled, which the engines
 *  compile for, while @c strings_kit_t keeps to a tier the build's own flags guarantee. */
sz_capability_t strings_capabilities() noexcept {
    sz_capability_t capabilities = sz_cap_serial_k;
    if (sz_capabilities_enabled_cpu(&capabilities) != sz_success_k) capabilities = sz_cap_serial_k;
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

/** Which JSON a writer spells: strict RFC 8259, or lenient with comments and the non-finite numbers
 *  yyjson admits. */
enum class json_dialect_t : bool { strict_k, lenient_k };

/** Writes random JSON of a dialect: nested containers, duplicate and escaped keys, numbers of every
 *  shape and strings. */
class document_writer_t {
    random_t& random_;
    json_dialect_t dialect_;

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

    void blank(std::string& text) {
        constexpr std::string_view blanks_k[] = {"", "", " ", "\n", "\t ", "\r\n  "};
        text += pick(blanks_k);
        if (dialect_ == json_dialect_t::lenient_k && random_.below(16) == 0)
            text += random_.below(2) ? "/* {\"a\": [ */" : "// ] } \"\n";
    }

    void scalar(std::string& text) {
        switch (random_.below(dialect_ == json_dialect_t::lenient_k ? 7 : 6)) {
        case 0: text += pick(numbers_k); break;
        case 1: text += pick(numbers_k); break;
        case 2:
        case 3: text += pick(strings_k); break;
        case 4: text += random_.below(2) ? "true" : "false"; break;
        case 5: text += "null"; break;
        default: text += pick(lenient_k); break;
        }
    }

  public:
    document_writer_t(random_t& random, json_dialect_t dialect) noexcept : random_(random), dialect_(dialect) {}

    void value(std::string& text, std::size_t depth) {
        std::uint64_t const kind = depth >= 3 ? 2 : random_.below(4);
        if (kind == 0) {
            text += '{';
            std::size_t const members = random_.below(5);
            for (std::size_t member = 0; member != members; ++member) {
                if (member) text += ',';
                blank(text);
                text += '"', text += pick(keys_k), text += '"';
                blank(text);
                text += ':';
                blank(text);
                value(text, depth + 1);
                blank(text);
            }
            text += '}';
        }
        else if (kind == 1) {
            text += '[';
            std::size_t const elements = random_.below(4);
            for (std::size_t element = 0; element != elements; ++element) {
                if (element) text += ',';
                blank(text);
                value(text, depth + 1);
                blank(text);
            }
            text += ']';
        }
        else scalar(text);
    }

    /** A whole document, an object at its root most of the time. */
    std::string document() {
        std::string text;
        blank(text);
        if (random_.below(8)) {
            text += '{';
            std::size_t const members = 1 + random_.below(6);
            for (std::size_t member = 0; member != members; ++member) {
                if (member) text += ',';
                text += '"', text += pick(keys_k), text += "\":";
                blank(text);
                value(text, 1);
            }
            text += '}';
        }
        else value(text, 0);
        blank(text);
        return text;
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

/** The value yyjson parsed @p value into, as an extraction would hold it, missing for @c nullptr. */
json_cell_t cell_of_parsed(yyjson_val* value) noexcept {
    json_cell_t cell;
    if (!value) return cell;
    switch (yyjson_get_type(value)) {
    case YYJSON_TYPE_NULL: cell.token = json_token_t::null_k; break;
    case YYJSON_TYPE_BOOL: cell.token = yyjson_get_bool(value) ? json_token_t::true_k : json_token_t::false_k; break;
    case YYJSON_TYPE_STR:
        cell.token = json_token_t::string_k;
        cell.bytes = {yyjson_get_str(value), yyjson_get_len(value)};
        break;
    case YYJSON_TYPE_OBJ: cell.token = json_token_t::object_k; break;
    case YYJSON_TYPE_ARR: cell.token = json_token_t::array_k; break;
    case YYJSON_TYPE_NUM:
        cell.token = json_token_t::number_k;
        if (yyjson_is_real(value)) cell.number = yyjson_get_real(value);
        else if (yyjson_is_sint(value)) cell.number = yyjson_get_sint(value);
        else cell.number = yyjson_get_uint(value);
        break;
    default: break;
    }
    return cell;
}

/** Counts the differences between @p oracle and @p candidate in every conversion a gather or a read makes. */
std::size_t compare_values(yyjson_val* oracle, json_cell_t const& candidate, ustore_arena_t arena,
                           std::string_view context) {
    std::size_t failures = 0;
    auto const report_difference = [&](char const* what) {
        fmt::println(stderr, "  {} differs for {}", what, context);
        ++failures;
    };
    json_cell_t const expected = cell_of_parsed(oracle);
    if (expected.token != candidate.token) {
        report_difference("token");
        return failures;
    }
    if (!oracle) return failures;

    // A container prints with its strings and numbers as written, so yyjson reads it back before the texts compare
    auto [oracle_text, oracle_status] = json_dump({oracle, nullptr}, arena);
    auto [candidate_text, candidate_status] = json_text_of<strings_kit_t>(candidate, arena);
    if (candidate_status.succeeded() && yyjson_is_ctn(oracle)) {
        auto [reparsed, reparsed_status] = json_parse(candidate_text, arena);
        if (reparsed_status.failed()) report_difference("minified container");
        else {
            auto [redumped, redumped_status] = json_dump({yyjson_doc_get_root(reparsed.handle), nullptr}, arena);
            candidate_text = redumped;
            candidate_status = redumped_status;
        }
    }
    if (oracle_status.failed() || candidate_status.failed() || oracle_text.size() != candidate_text.size() ||
        std::memcmp(oracle_text.data(), candidate_text.data(), oracle_text.size()) != 0)
        report_difference("text");

    auto const compare_cell = [&](char const* what, auto gather) {
        ustore_byte_t oracle_cell[16] = {}, candidate_cell[16] = {};
        conversion_t const oracle_conversion = gather(expected, oracle_cell);
        conversion_t const candidate_conversion = gather(candidate, candidate_cell);
        if (oracle_conversion != candidate_conversion || std::memcmp(oracle_cell, candidate_cell, 16) != 0)
            report_difference(what);
    };
    using cell_t = json_cell_t const&;
    compare_cell("bool", [](cell_t value, ustore_byte_t* cell) { return gather_scalar<bool>(value, cell, 0); });
    compare_cell("f64", [](cell_t value, ustore_byte_t* cell) { return gather_scalar<double>(value, cell, 0); });
    compare_cell("f32", [](cell_t value, ustore_byte_t* cell) { return gather_scalar<float>(value, cell, 0); });
    compare_cell("f16", [](cell_t value, ustore_byte_t* cell) { return gather_half(value, cell, 0); });
    compare_cell("i64", [](cell_t value, ustore_byte_t* cell) { return gather_scalar<std::int64_t>(value, cell, 0); });
    compare_cell("i32", [](cell_t value, ustore_byte_t* cell) { return gather_scalar<std::int32_t>(value, cell, 0); });
    compare_cell("i16", [](cell_t value, ustore_byte_t* cell) { return gather_scalar<std::int16_t>(value, cell, 0); });
    compare_cell("i8", [](cell_t value, ustore_byte_t* cell) { return gather_scalar<std::int8_t>(value, cell, 0); });
    compare_cell("u64", [](cell_t value, ustore_byte_t* cell) { return gather_scalar<std::uint64_t>(value, cell, 0); });
    compare_cell("u32", [](cell_t value, ustore_byte_t* cell) { return gather_scalar<std::uint32_t>(value, cell, 0); });
    compare_cell("u16", [](cell_t value, ustore_byte_t* cell) { return gather_scalar<std::uint16_t>(value, cell, 0); });
    compare_cell("u8", [](cell_t value, ustore_byte_t* cell) { return gather_scalar<std::uint8_t>(value, cell, 0); });
    compare_cell("uuid", [](cell_t value, ustore_byte_t* cell) { return gather_uuid(value, cell, 0); });

    printed_number_buffer_t oracle_buffer, candidate_buffer;
    std::string_view oracle_string, candidate_string;
    if (json_to_string(expected, oracle_buffer, oracle_string) !=
            json_to_string(candidate, candidate_buffer, candidate_string) ||
        oracle_string != candidate_string)
        report_difference("string");
    return failures;
}

/** Rows the device steps converted and rows they left to the host, over all the suite's batches. */
struct device_rows_tally_t {
    std::size_t compared = 0;
    std::size_t left_to_host = 0;
};

device_rows_tally_t device_rows_tally;

/** Every column type a gather converts into. */
constexpr ustore_docs_field_type_t gathered_types_k[] = {
    ustore_docs_field_type_f64_k,  ustore_docs_field_type_f32_k,    ustore_docs_field_type_f16_k,
    ustore_docs_field_type_i64_k,  ustore_docs_field_type_i32_k,    ustore_docs_field_type_i16_k,
    ustore_docs_field_type_i8_k,   ustore_docs_field_type_u64_k,    ustore_docs_field_type_u32_k,
    ustore_docs_field_type_u16_k,  ustore_docs_field_type_u8_k,     ustore_docs_field_type_bool_k,
    ustore_docs_field_type_uuid_k, ustore_docs_field_type_binary_k, ustore_docs_field_type_string_k,
};

/** Converts @p value into one cell of @p type as the host gather does. */
conversion_t gather_on_host(ustore_docs_field_type_t type, json_cell_t const& value, ustore_byte_t* cell) noexcept {
    switch (type) {
    case ustore_docs_field_type_bool_k: return gather_scalar<bool>(value, cell, 0);
    case ustore_docs_field_type_uuid_k: return gather_uuid(value, cell, 0);
    case ustore_docs_field_type_f64_k: return gather_scalar<double>(value, cell, 0);
    case ustore_docs_field_type_f32_k: return gather_scalar<float>(value, cell, 0);
    case ustore_docs_field_type_f16_k: return gather_half(value, cell, 0);
    case ustore_docs_field_type_i64_k: return gather_scalar<std::int64_t>(value, cell, 0);
    case ustore_docs_field_type_i32_k: return gather_scalar<std::int32_t>(value, cell, 0);
    case ustore_docs_field_type_i16_k: return gather_scalar<std::int16_t>(value, cell, 0);
    case ustore_docs_field_type_i8_k: return gather_scalar<std::int8_t>(value, cell, 0);
    case ustore_docs_field_type_u64_k: return gather_scalar<std::uint64_t>(value, cell, 0);
    case ustore_docs_field_type_u32_k: return gather_scalar<std::uint32_t>(value, cell, 0);
    case ustore_docs_field_type_u16_k: return gather_scalar<std::uint16_t>(value, cell, 0);
    case ustore_docs_field_type_u8_k: return gather_scalar<std::uint8_t>(value, cell, 0);
    default: return conversion_t::collides_k;
    }
}

/** The conversion a column's three bits record for @p row. */
conversion_t recorded_conversion(ustore_octet_t const* bitmaps, std::size_t stride, std::size_t row) noexcept {
    auto const bit = [&](std::size_t bitmap) noexcept {
        return (bitmaps[bitmap * stride + row / 8] >> (row % 8)) & 1u;
    };
    if (bit(0)) return bit(1) ? conversion_t::converted_k : conversion_t::exact_k;
    return bit(2) ? conversion_t::collides_k : conversion_t::missing_k;
}

/** The text the host exports for @p value as @p text prints it, and its conversion, in @p arena. */
conversion_t print_on_host(cell_text_t text, json_cell_t const& value, ustore_arena_t arena,
                           std::string_view& printed) noexcept {
    bool const container = value.token == json_token_t::object_k || value.token == json_token_t::array_k;
    if (text == cell_text_t::field_json_k || (text == cell_text_t::field_text_k && container)) {
        auto [json, json_status] = json_text_of<strings_kit_t>(value, arena);
        printed = json_status.failed() ? std::string_view {} : std::string_view {json.c_str(), json.size()};
        return conversion_t::exact_k;
    }
    auto [buffer, buffer_status] = arena_allocate<char>(arena, printed_number_length_limit_k);
    printed_number_buffer_t number;
    std::string_view printed_text;
    conversion_t const conversion = json_to_string(value, number, printed_text);
    if (buffer_status.failed()) return conversion;
    if (printed_text.data() == number) {
        std::memcpy(buffer.data(), number, printed_text.size());
        printed_text = {buffer.data(), printed_text.size()};
    }
    printed = printed_text;
    return conversion;
}

/** Runs the device gather's row steps host-compiled over @p documents, on a page laid out from one
 *  subdocuments pass as a device walk stores it, and compares every row they convert with @p cells,
 *  which the host extracted for @p fields, the host's @p status refusing the batch. */
std::size_t compare_device_rows(json_documents_t const& documents, std::span<std::string_view const> fields,
                                json_cells_t const& cells, status_t const& status, ustore_arena_t arena) {
    std::size_t failures = 0;
    std::uint32_t const rows = static_cast<std::uint32_t>(documents.count);
    auto [compiled, compiled_status] = compile_field_paths(fields, arena);
    failures += expect(compiled_status.succeeded());
    if (compiled_status.failed()) return failures;
    std::size_t const paths_count = compiled.paths.size();

    // The page as a walk leaves it: the documents back to back, each document's nodes in its slots
    auto [offsets, offsets_status] = arena_allocate<std::uint64_t>(arena, rows + 1);
    auto [found, found_status] = arena_allocate<std::uint32_t>(arena, rows);
    auto [matches, matches_status] = arena_allocate<sz_match_t>(arena, std::size_t {rows} * paths_count + 1);
    auto [walked, walked_status] = arena_allocate<sz_string_view_t>(arena, rows);
    auto [walked_rows, walked_rows_status] = arena_allocate<std::size_t>(arena, rows);
    failures += expect(offsets_status.succeeded() && found_status.succeeded() && matches_status.succeeded() &&
                       walked_status.succeeded() && walked_rows_status.succeeded());
    if (failures) return failures;
    offsets[0] = 0;
    std::size_t walked_count = 0;
    for (std::uint32_t row = 0; row != rows; ++row) {
        found[row] = 0;
        offsets[row + 1] = offsets[row] + documents[row].size();
        if (documents[row].empty()) continue;
        walked[walked_count] = stringzilla_view(documents[row]);
        walked_rows[walked_count++] = row;
    }
    auto [bytes, bytes_status] = arena_allocate<char>(arena, static_cast<std::size_t>(offsets[rows]));
    failures += expect(bytes_status.succeeded());
    if (failures) return failures;
    for (std::uint32_t row = 0; row != rows; ++row)
        if (!documents[row].empty())
            std::memcpy(bytes.data() + offsets[row], documents[row].data(), documents[row].size());

    std::uint32_t walk_refused = 0, any_refused = 0;
    if (paths_count && walked_count) {
        sz_strings_view_t paths_view {};
        sz_strings_from_string_views(compiled.paths.data(), paths_count, &paths_view);
        sz_allocator_t allocator = stringzilla_allocator(arena);
        subdocuments_engine_t engine;
        failures += expect(engine.compile(paths_view, allocator, strings_capabilities()) == sz_success_k);
        auto [walk_matches, walk_matches_status] = arena_allocate<sz_match_t>(arena, walked_count * paths_count + 1);
        auto [walk_offsets, walk_offsets_status] = arena_allocate<sz_size_t>(arena, walked_count + 1);
        failures += expect(walk_matches_status.succeeded() && walk_offsets_status.succeeded());
        if (failures) return failures;
        sz_strings_view_t walked_view {};
        sz_strings_from_string_views(walked.data(), walked_count, &walked_view);
        // A walk failing on a malformed document leaves the call to the host
        if (engine.find(walked_view, walk_matches, walk_offsets) != sz_success_k) walk_refused = 1;
        for (std::size_t walk = 0; !walk_refused && walk != walked_count; ++walk) {
            std::size_t const row = walked_rows[walk];
            for (std::size_t match = walk_offsets[walk]; match != walk_offsets[walk + 1]; ++match) {
                if (found[row] == paths_count) {
                    walk_refused = 1;
                    break;
                }
                matches[row * paths_count + found[row]++] = walk_matches[match];
            }
        }
    }
    gather_page_t const page {
        bytes.data(), offsets.data(), matches.data(), found.data(), static_cast<std::uint32_t>(paths_count),
        rows,         nullptr};
    auto const path_of = [&](std::size_t field) noexcept {
        std::size_t const path = compiled.path_of_field[field];
        return path == no_path_k ? no_cell_path_k : static_cast<std::uint32_t>(path);
    };

    std::size_t const stride = (rows + 7) / 8;
    auto [bitmaps, bitmaps_status] = arena_allocate<ustore_octet_t>(arena, 3 * stride);
    auto [presences, presences_status] = arena_allocate<ustore_octet_t>(arena, stride);
    auto [lengths, lengths_status] = arena_allocate<ustore_length_t>(arena, rows);
    auto [scalars, scalars_status] = arena_allocate<ustore_byte_t>(arena, std::size_t {rows} * uuid_bytes_k);
    auto [measured, measured_status] = arena_allocate<tape_source_t>(arena, rows);
    auto [text_offsets, text_offsets_status] = arena_allocate<std::uint64_t>(arena, rows + 1);
    failures += expect(bitmaps_status.succeeded() && presences_status.succeeded() && lengths_status.succeeded() &&
                       scalars_status.succeeded() && measured_status.succeeded() && text_offsets_status.succeeded());
    if (failures) return failures;

    for (std::size_t field = 0; field != fields.size(); ++field) {
        auto const host_cell = [&](std::size_t row) noexcept { return cells[field * rows + row]; };
        auto const report_row = [&](char const* what, std::size_t row) {
            fmt::println(stderr, "  device {} differs for field '{}' of document {}: {}", what, fields[field], row,
                         documents[row]);
            ++failures;
        };
        // An output a lane refused leaves the whole call to the host, while others still compare
        auto const settle = [&](std::uint32_t lane_refused) {
            any_refused |= lane_refused;
            if (walk_refused || lane_refused) ++device_rows_tally.left_to_host;
            return !walk_refused && !lane_refused && status.succeeded();
        };
        for (ustore_docs_field_type_t const type : gathered_types_k) {
            std::uint32_t lane_refused = 0;
            std::size_t const width = field_size_bytes(type);
            std::memset(bitmaps.data(), 0xA5, bitmaps.size());
            if (width) {
                gather_scalars_t const column {bitmaps.data(), scalars.data(), stride, type, path_of(field)};
                for (std::uint32_t first = 0; first < rows; first += gather_lane_rows_k)
                    lane_refused |= gather_scalar_rows(page, column, first);
                if (!settle(lane_refused)) continue;
                for (std::size_t row = 0; row != rows; ++row) {
                    ustore_byte_t expected_cell[uuid_bytes_k] = {};
                    conversion_t const expected = gather_on_host(type, host_cell(row), expected_cell);
                    if (recorded_conversion(bitmaps.data(), stride, row) != expected) report_row("conversion", row);
                    if (std::memcmp(expected_cell, scalars.data() + row * width, width) != 0) report_row("scalar", row);
                    ++device_rows_tally.compared;
                }
                continue;
            }
            for (cell_text_t const text :
                 {type == ustore_docs_field_type_string_k ? cell_text_t::column_text_k : cell_text_t::column_binary_k,
                  type == ustore_docs_field_type_string_k ? cell_text_t::field_json_k : cell_text_t::field_text_k}) {
                bool const column = text == cell_text_t::column_text_k || text == cell_text_t::column_binary_k;
                gather_texts_t texts;
                texts.text = text;
                texts.path = path_of(field);
                texts.bitmaps = column ? bitmaps.data() : nullptr;
                texts.bitmap_stride = stride;
                texts.presences = column ? nullptr : presences.data();
                texts.lengths = column ? nullptr : lengths.data();
                texts.measured = measured.data();
                lane_refused = 0;
                for (std::uint32_t first = 0; first < rows; first += gather_lane_rows_k)
                    lane_refused |= measure_text_rows(page, texts, first);
                if (!settle(lane_refused)) continue;
                // The tape kernels' scan, on the host
                text_offsets[0] = 0;
                for (std::size_t row = 0; row != rows; ++row)
                    text_offsets[row + 1] = text_offsets[row] + measured[row].length;
                auto [text_bytes, text_bytes_status] = arena_allocate<char>(
                    arena, static_cast<std::size_t>(text_offsets[rows]));
                failures += expect(text_bytes_status.succeeded());
                texts.offsets = text_offsets.data();
                texts.bytes = text_bytes.data();
                for (std::uint32_t row = 0; row != rows; ++row) write_text_row(page, texts, row);
                for (std::size_t row = 0; row != rows; ++row) {
                    json_cell_t const value = host_cell(row);
                    std::string_view expected_text;
                    conversion_t const expected = print_on_host(text, value, arena, expected_text);
                    std::string_view const written {
                        text_bytes.data() + text_offsets[row],
                        static_cast<std::size_t>(text_offsets[row + 1] - text_offsets[row])};
                    if (column) {
                        if (recorded_conversion(bitmaps.data(), stride, row) != expected) report_row("conversion", row);
                        bool const terminated = text == cell_text_t::column_text_k;
                        if (written.size() != expected_text.size() + terminated ||
                            written.substr(0, expected_text.size()) != expected_text ||
                            (terminated && written.back() != '\0'))
                            report_row("text", row);
                    }
                    else {
                        bool const present = value.token != json_token_t::missing_k;
                        if (((presences[row / 8] >> (row % 8)) & 1u) != present) report_row("presence", row);
                        if (lengths[row] != written.size()) report_row("length", row);
                        if (present && written != expected_text) report_row("read text", row);
                        if (!present && !written.empty()) report_row("missing text", row);
                    }
                    ++device_rows_tally.compared;
                }
            }
        }
    }
    // A batch the host refuses must never convert on a device
    if (status.failed() && !walk_refused && !any_refused) {
        fmt::println(stderr, "  device converted a batch the host refused: {}", status.message);
        ++failures;
    }
    return failures;
}

/** Extracts every field of @p fields over @p batch and compares it with yyjson's lookup, answering the status of the
 *  extraction in @p status, which a batch of lenient documents may leave malformed. */
std::size_t compare_batch(documents_batch_t& batch, std::span<std::string_view const> fields, status_t& status) {
    std::size_t failures = 0;
    scoped_arena_t arena;
    json_documents_t const documents = batch.view();
    std::size_t const cells_count = documents.count * fields.size();
    auto const cell_of = [&](std::size_t cell) noexcept {
        return field_cell_t {cell % documents.count, cell / documents.count};
    };
    json_cells_t cells;
    status = extract_fields<strings_kit_t>(documents, fields, cells_count, cell_of, strings_capabilities(), arena,
                                           cells);
    failures += compare_device_rows(documents, fields, cells, status, arena);
    if (status.failed()) return failures;
    for (std::size_t document_index = 0; document_index != documents.count; ++document_index) {
        std::string_view const document = documents[document_index];
        auto [parsed, parsed_status] = json_parse(document, arena);
        failures += expect(parsed_status.succeeded());
        yyjson_val* const root = parsed ? yyjson_doc_get_root(parsed.handle) : nullptr;
        for (std::size_t field_index = 0; field_index != fields.size(); ++field_index) {
            std::string const context = fmt::format("field '{}' of document {}: {}", fields[field_index],
                                                    document_index, document);
            failures += compare_values(json_lookup(root, fields[field_index]),
                                       cells[field_index * documents.count + document_index], arena, context);
        }
    }
    return failures;
}

#pragma endregion Comparisons

#pragma region Cases

/** Fields translate into one JSONPath each, with index unions and escapes, and into none for invalid pointers. */
std::size_t json_paths_translation() {
    std::size_t failures = 0;
    scoped_arena_t arena;
    struct translation_t {
        std::string_view field;
        std::string_view path;
    };
    std::vector<translation_t> const translations {
        {"", "$"},
        {"k", "$['k']"},
        {"/k", "$['k']"},
        {"/a/b~1c", "$['a']['b/c']"},
        {"/t~0", "$['t~']"},
        {"/0", "$['0',0]"},
        {"/a/12", "$['a']['12',12]"},
        {"/01", "$['01']"},
        {"/-", "$['-']"},
        {"/", "$['']"},
        {"/a/", "$['a']['']"},
        {"it's\\", "$['it\\'s\\\\']"},
        {"/\x01", "$['\\u0001']"},
        {"a/b", "$['a/b']"},
        {"/a~2", ""},
        {"/a~", ""},
    };
    for (translation_t const& translation : translations) {
        st::vector<char, arena_allocator<char>> path {{ustore_arena_t {arena}}};
        auto [valid, status] = json_path_of(translation.field, path);
        failures += expect(status.succeeded());
        failures += expect_eq(valid, !translation.path.empty());
        if (valid) failures += expect_eq(std::string_view(path.begin(), path.end()), translation.path);
    }
    return failures;
}

/** Slashes inside strings extract as written, while a comment anywhere, even inside a container no path enters, makes
 *  the document malformed, as a write refuses it too. */
std::size_t json_paths_comments() {
    std::size_t failures = 0;
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
    status_t status;
    failures += compare_batch(strict, fields, status);
    failures += expect(status.succeeded());

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
        // Writes parse strictly, so no such document reaches a read
        failures += expect(json_parse(text, arena).status() == status_code_t::malformed_k);
        documents_batch_t batch;
        batch.texts = {R"({"a": 4})", std::string(text)};
        batch.present = {true, true};
        failures += compare_batch(batch, fields, status);
        failures += expect(status == status_code_t::malformed_k);
    }
    return failures;
}

/** Duplicate keys resolve through the first member, as yyjson's lookup does, at every depth. */
std::size_t json_paths_duplicates() {
    std::size_t failures = 0;
    documents_batch_t batch;
    batch.texts = {
        R"({"a": {"x": 1}, "a": {"b": 2}})", R"({"a": 1, "a": 2})",          R"({"a": {"b": 1}, "a": {"b": 2}})",
        R"({"a": [5], "a": {"0": 6}})",      R"({"a": {"0": 7}, "a": [8]})", R"({"a": 3, "a": {"b": 4}})",
        R"([{"a": 1}, {"a": 2}])",
    };
    batch.present.assign(batch.texts.size(), true);
    std::string_view const fields[] = {"a", "/a", "/a/b", "/a/0", "/a/x", "/0/a", "/1/a", ""};
    status_t status;
    failures += compare_batch(batch, fields, status);
    failures += expect(status.succeeded());
    return failures;
}

/** Missing and empty documents hold no field, not even the root. */
std::size_t json_paths_missing() {
    std::size_t failures = 0;
    documents_batch_t batch;
    batch.texts = {R"({"a": 1})", "", R"({"a": 2})", R"({"a": 3})"};
    batch.present = {true, true, false, true};
    std::string_view const fields[] = {"", "a", "/a"};
    status_t status;
    failures += compare_batch(batch, fields, status);
    failures += expect(status.succeeded());
    return failures;
}

/** A selected value yyjson would refuse, and NaN, which only lenient JSON spells, read as malformed, while a selected
 *  container reads as written. */
std::size_t json_paths_refusals() {
    std::size_t failures = 0;
    std::string_view const refused[] = {
        "{\"a\": \"raw\x01control\"}",
        "{\"a\": \"bad\xC3utf8\"}",
        R"({"a": "\q"})",
        R"({"a": "\ud800"})",
        R"({"a": 01})",
        R"({"a": NaN})",
    };
    std::string_view const fields[] = {"a"};
    for (std::string_view text : refused) {
        documents_batch_t batch;
        batch.texts = {std::string(text)};
        batch.present = {true};
        status_t status;
        failures += compare_batch(batch, fields, status);
        failures += expect(status == status_code_t::malformed_k);
    }

    // A selected container is not validated inside, which only documents no write would take could tell
    documents_batch_t lazy;
    lazy.texts = {R"({"a": [1, 2,]})"};
    lazy.present = {true};
    scoped_arena_t arena;
    json_cells_t cells;
    auto const first_cell = [](std::size_t) noexcept { return field_cell_t {0, 0}; };
    failures += expect(
        extract_fields<strings_kit_t>(lazy.view(), fields, 1, first_cell, strings_capabilities(), arena, cells)
            .succeeded());
    failures += expect(cells[0].token == json_token_t::array_k);
    return failures;
}

/** Big numbers, reals at the edges of plain notation and escapes print as yyjson prints them at the top level. */
std::size_t json_paths_printing() {
    std::size_t failures = 0;
    documents_batch_t batch;
    batch.texts = {
        R"({"a": 100})",
        R"({"a": 1e2})",
        R"({"a": -0.0})",
        R"({"a": 1e21})",
        R"({"a": 1e22})",
        R"({"a": 1e-6})",
        R"({"a": 1.5e-7})",
        R"({"a": 2.5})",
        R"({"a": 1e308})",
        R"({"a": "\u001f\/x"})",
        R"({"a": 1E+3})",
        R"({"a": 0.1})",
        R"({"a": 12345678901234567890123})",
    };
    batch.present.assign(batch.texts.size(), true);
    std::string_view const fields[] = {"a"};
    status_t status;
    failures += compare_batch(batch, fields, status);
    failures += expect(status.succeeded());
    return failures;
}

/** Random batches of documents a write takes extract and convert exactly as yyjson resolves them; the lenient writer's
 *  comments and non-finite literals never pass the strict parse a write makes, so its documents that do are strict. */
std::size_t json_paths_random(std::uint64_t seed) {
    std::size_t failures = 0;
    random_t random {seed};
    for (std::size_t round = 0; round != 400; ++round) {
        document_writer_t writer {random, round % 4 == 3 ? json_dialect_t::lenient_k : json_dialect_t::strict_k};
        documents_batch_t batch;
        std::size_t const count = 1 + random.below(24);
        scoped_arena_t probe;
        while (batch.texts.size() != count) {
            std::string text = writer.document();
            // Only documents a write takes are compared, as only those can reach a read
            if (json_parse(std::string_view {text}, probe).status().failed()) continue;
            batch.texts.push_back(std::move(text));
            batch.present.push_back(random.below(10) != 0);
        }
        status_t status;
        failures += compare_batch(batch, fields_k, status);
        failures += expect(status.succeeded());
    }
    // The device steps must convert most rows themselves, or the comparison above checks nothing
    fmt::println("  device rows: {} compared, {} outputs left to the host", device_rows_tally.compared,
                 device_rows_tally.left_to_host);
    failures += expect(device_rows_tally.compared > device_rows_tally.left_to_host);
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
    report(totals, "json_paths.printing", json_paths_printing);
    report(totals, "json_paths.random", json_paths_random, std::uint64_t {options.seed.value});

    return suite_exit_code(totals);
}
