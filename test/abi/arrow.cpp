/**
 *  @file test/abi/arrow.cpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief Tests of @c arrow.h: Arrow columns read into views, views exported as columns pinning their arena, and the
 *      ingest of @c db.h on every engine.
 *
 *  Every case runs on a fresh database of the engine @c --engine names, reading columns through the C data interface
 *  alone, as a consumer with no Arrow library would.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int64_t`, `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstdio>  // `stderr`

#include <algorithm>   // `std::reverse`
#include <array>       // `std::array`
#include <optional>    // `std::optional`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <utility>     // `std::as_const`
#include <vector>      // `std::vector`

#include <fmt/format.h>

#include "ustore/ranges.hpp" // `tape_string`
#include "ustore/ustore.h"

#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_eq`, `open_raw`

using namespace unum::ustore;
using namespace unum::ustore::test;

/** The number behind @p status, so a failed check prints it. */
constexpr int format_as(ustore_status_t status) noexcept { return status; }

/** The number behind @p layout, so a failed check prints it. */
constexpr int format_as(ustore_strings_layout_t layout) noexcept { return layout; }

namespace unum::ustore::test {

/** The cells of one row, each the bytes of its column. */
using row_t = std::vector<std::string>;

/** Every row of a collection, in the order its cursor walks them. */
using rows_t = std::vector<row_t>;

/** An open database, emptied of every collection, freed with the case. */
struct fixture_t {

    /** The open database. */
    ustore_database_t database = nullptr;

    fixture_t(fixture_t const&) = delete;
    fixture_t& operator=(fixture_t const&) = delete;

    explicit fixture_t(config_t const& config) noexcept {
        auto [opened, open_status] = open_raw(config);
        if (open_status.failed()) {
            fmt::println(stderr, "  failed to open the database: {}", open_status.message);
            return;
        }
        database = opened;
        ustore_outcome_t outcome {};
        ustore_database_clear_t clear {.database = database, .outcome = &outcome, .mode = ustore_drop_collection_k};
        ustore_database_clear(&clear);
        if (outcome.status != ustore_status_success_k)
            fmt::println(stderr, "  failed to clear the database: {}", outcome.message);
    }

    ~fixture_t() noexcept { ustore_database_free(database); }

    /** Creates a collection of @p modality named @p name, answering the nil identifier on failure. */
    ustore_collection_id_t create(char const* name, ustore_modality_t modality) noexcept {
        ustore_collection_id_t collection {};
        ustore_outcome_t outcome {};
        ustore_collection_create_t create {.database = database,
                                           .outcome = &outcome,
                                           .name = bytes_of(name),
                                           .modality = modality,
                                           .collection = &collection};
        ustore_collection_create(&create);
        if (outcome.status != ustore_status_success_k)
            fmt::println(stderr, "  failed to create {}: {}", name, outcome.message);
        return collection;
    }
};

/** A tape of 32-bit offsets over strings, owning their bytes. */
struct tape_t {
    std::string bytes;
    std::vector<std::uint32_t> offsets {0};

    void push(std::string_view text) {
        bytes += text;
        offsets.push_back(static_cast<std::uint32_t>(bytes.size()));
    }

    ustore_strings_view_t view() const noexcept { return utilities::u32_tape(bytes.data(), offsets.data()); }
};

/** The bytes of @p key. */
std::string cell_of(ustore_key_t const& key) { return std::string(reinterpret_cast<char const*>(key.uuid.bytes), 16); }

/** The bytes of string @p index of @p tape. */
std::string cell_of(ustore_strings_view_t const& tape, std::size_t index) {
    ustore_bytes_view_t const bytes = tape_string(tape, index);
    return std::string(reinterpret_cast<char const*>(bytes.data), bytes.length);
}

/** An arena of the calling thread, freed with the case unless handed to @c ustore_arena_free earlier. */
struct arena_t {
    ustore_arena_t arena = nullptr;

    arena_t() noexcept {
        ustore_outcome_t outcome {};
        ustore_arena_init_t init {.arena = &arena, .outcome = &outcome};
        ustore_arena_init(&init);
    }
    arena_t(arena_t const&) = delete;
    arena_t& operator=(arena_t const&) = delete;
    ~arena_t() noexcept { ustore_arena_free(arena); }
};

/** Pulls up to @p wanted keys and values of @p cursor through @p next_ into @p arena, appending them to @p rows. */
template <typename next_type_, typename cursor_type_>
ustore_size_t pull_keyed(ustore_database_t database, cursor_type_ cursor, void (*next_)(next_type_*),
                         ustore_arena_t arena, ustore_size_t wanted, rows_t& rows, std::size_t& failures) {
    ustore_outcome_t outcome {};
    ustore_key_t const* keys = nullptr;
    ustore_strings_view_t values {};
    ustore_size_t written = 0;
    next_type_ next {};
    next.database = database, next.outcome = &outcome, next.cursor = cursor, next.keys = &keys;
    next.values = &values, next.written = &written, next.arena = arena, next.wanted = wanted;
    if constexpr (requires { next.outputs_encoding; }) next.outputs_encoding = ustore_page_encoding_xml_k;
    next_(&next);
    failures += expect_success(outcome);
    for (ustore_size_t index = 0; index != written; ++index)
        rows.push_back({cell_of(keys[index]), cell_of(values, index)});
    return written;
}

/** Walks @p cursor of @p init_ to its end through @p next_, appending every row to @p rows. */
template <typename cursor_type_, typename init_type_, typename next_type_>
std::size_t walk_keyed(ustore_database_t database, ustore_collection_id_t collection, void (*init_)(init_type_*),
                       void (*next_)(next_type_*), void (*free_)(cursor_type_), rows_t& rows) {
    std::size_t failures = 0;
    cursor_type_ cursor = nullptr;
    ustore_outcome_t outcome {};
    init_type_ init {};
    init.cursor = &cursor, init.database = database, init.outcome = &outcome, init.collection = collection;
    init_(&init);
    if ((failures += expect_success(outcome))) return failures;
    arena_t arena;
    while (pull_keyed(database, cursor, next_, arena.arena, 64, rows, failures) && !failures) {}
    free_(cursor);
    return failures;
}

/** Reads every row of @p collection of @p modality into @p rows through its cursor, a graph as its edges. */
std::size_t dump(ustore_database_t database, ustore_collection_id_t collection, ustore_modality_t modality,
                 rows_t& rows) {
    switch (modality) {
    case ustore_modality_docs_k:
        return walk_keyed(database, collection, &ustore_docs_cursor_init, &ustore_docs_cursor_next,
                          &ustore_docs_cursor_free, rows);
    case ustore_modality_pages_k:
        return walk_keyed(database, collection, &ustore_pages_cursor_init, &ustore_pages_cursor_next,
                          &ustore_pages_cursor_free, rows);
    case ustore_modality_strings_k: break;
    case ustore_modality_graph_k: break;
    default:
        return walk_keyed(database, collection, &ustore_blobs_cursor_init, &ustore_blobs_cursor_next,
                          &ustore_blobs_cursor_free, rows);
    }
    std::size_t failures = 0;
    ustore_outcome_t outcome {};
    arena_t arena;
    if (modality == ustore_modality_strings_k) {
        ustore_strings_cursor_t cursor = nullptr;
        ustore_strings_cursor_init_t init {.cursor = &cursor,
                                           .database = database,
                                           .outcome = &outcome,
                                           .collection = collection};
        ustore_strings_cursor_init(&init);
        if ((failures += expect_success(outcome))) return failures;
        for (ustore_size_t written = 1; written && !failures;) {
            ustore_strings_view_t strings {};
            ustore_key_t const* values = nullptr;
            ustore_strings_cursor_next_t next {.database = database,
                                               .outcome = &outcome,
                                               .cursor = cursor,
                                               .strings = &strings,
                                               .values = &values,
                                               .written = &written,
                                               .arena = arena.arena,
                                               .wanted = 64};
            ustore_strings_cursor_next(&next);
            failures += expect_success(outcome);
            for (ustore_size_t index = 0; index != written; ++index)
                rows.push_back({cell_of(strings, index), cell_of(values[index])});
        }
        ustore_strings_cursor_free(cursor);
        return failures;
    }
    ustore_graph_vertices_cursor_t cursor = nullptr;
    ustore_graph_vertices_cursor_init_t init {.cursor = &cursor,
                                              .database = database,
                                              .outcome = &outcome,
                                              .collection = collection};
    ustore_graph_vertices_cursor_init(&init);
    if ((failures += expect_success(outcome))) return failures;
    arena_t edges_arena;
    for (ustore_size_t written = 1; written && !failures;) {
        ustore_key_t const* vertices = nullptr;
        ustore_graph_vertices_cursor_next_t next {.database = database,
                                                  .outcome = &outcome,
                                                  .cursor = cursor,
                                                  .keys = &vertices,
                                                  .written = &written,
                                                  .arena = arena.arena,
                                                  .wanted = 64};
        ustore_graph_vertices_cursor_next(&next);
        failures += expect_success(outcome);
        if (!written || failures) break;
        ustore_vertex_role_t const role = ustore_vertex_role_source_k;
        ustore_vertex_degree_t const* degrees = nullptr;
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* triples = nullptr;
        ustore_graph_read_edges_t read {.database = database,
                                        .outcome = &outcome,
                                        .count = written,
                                        .collections = {&collection, 0},
                                        .vertices = {vertices, sizeof(ustore_key_t)},
                                        .roles = {&role, 0},
                                        .degrees_per_vertex = &degrees,
                                        .edges_offsets = &offsets,
                                        .edges_per_vertex = &triples,
                                        .arena = edges_arena.arena};
        ustore_graph_read_edges(&read);
        failures += expect_success(outcome);
        for (ustore_length_t edge = 0; offsets && edge != offsets[written]; ++edge)
            rows.push_back(
                {cell_of(triples[edge * 3]), cell_of(triples[edge * 3 + 1]), cell_of(triples[edge * 3 + 2])});
    }
    ustore_graph_vertices_cursor_free(cursor);
    return failures;
}

/** Writes @p count blobs, keyed 1 to @p count, each valued "value-" and its key. */
std::size_t fill_blobs(ustore_database_t database, ustore_collection_id_t collection, std::size_t count) {
    std::vector<ustore_key_t> keys;
    tape_t values;
    for (std::size_t index = 1; index <= count; ++index) {
        keys.push_back(key_from_integer(index));
        values.push(fmt::format("value-{}", index));
    }
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {.database = database,
                                .outcome = &outcome,
                                .count = count,
                                .collections = {&collection, 0},
                                .keys = {keys.data(), sizeof(ustore_key_t)},
                                .values = values.view()};
    ustore_blobs_write(&write);
    return expect_success(outcome);
}

/** The rows a collection of @p modality holds, keyed 1 to @p count, as @c fill writes and an ingest appends them. */
struct modality_rows_t {
    std::vector<ustore_key_t> keys, targets;
    tape_t texts;

    modality_rows_t(ustore_modality_t modality, std::size_t count) {
        for (std::size_t index = 1; index <= count; ++index) {
            keys.push_back(key_from_integer(index));
            targets.push_back(key_from_integer(index + 1));
            if (modality == ustore_modality_docs_k) texts.push(fmt::format("{{\"index\":{}}}", index));
            else if (modality == ustore_modality_pages_k) texts.push(fmt::format("<p>{}</p>", index));
            else texts.push(fmt::format("text-{:03}", index));
        }
    }
};

/** Fills @p collection of @p modality with @p rows through its own writes. */
std::size_t fill(ustore_database_t database, ustore_collection_id_t collection, ustore_modality_t modality,
                 modality_rows_t const& rows) {
    ustore_size_t const count = rows.keys.size();
    ustore_outcome_t outcome {};
    ustore_collections_view_t const collections {&collection, 0};
    ustore_keys_view_t const keys {rows.keys.data(), sizeof(ustore_key_t)};
    switch (modality) {
    case ustore_modality_docs_k: {
        ustore_docs_write_t write {.database = database,
                                   .outcome = &outcome,
                                   .count = count,
                                   .collections = collections,
                                   .keys = keys,
                                   .values = rows.texts.view()};
        ustore_docs_write(&write);
    } break;
    case ustore_modality_pages_k: {
        ustore_pages_import_t import {.database = database,
                                      .outcome = &outcome,
                                      .count = count,
                                      .collections = collections,
                                      .keys = keys,
                                      .values = rows.texts.view(),
                                      .encoding = ustore_page_encoding_xml_k};
        ustore_pages_import(&import);
    } break;
    case ustore_modality_strings_k: {
        ustore_strings_write_t write {.database = database,
                                      .outcome = &outcome,
                                      .count = count,
                                      .collections = collections,
                                      .strings = rows.texts.view(),
                                      .values = keys};
        ustore_strings_write(&write);
    } break;
    case ustore_modality_graph_k: {
        ustore_graph_write_edges_t write {.database = database,
                                          .outcome = &outcome,
                                          .count = count,
                                          .collections = collections,
                                          .sources = keys,
                                          .targets = {rows.targets.data(), sizeof(ustore_key_t)}};
        ustore_graph_write_edges(&write);
    } break;
    default: {
        ustore_blobs_write_t write {.database = database,
                                    .outcome = &outcome,
                                    .count = count,
                                    .collections = collections,
                                    .keys = keys,
                                    .values = rows.texts.view()};
        ustore_blobs_write(&write);
    }
    }
    return expect_success(outcome);
}

/** The value @p key holds in @p collection as @p view sees it, or an empty optional for a missing key. */
std::optional<std::string> stored_value(ustore_database_t database, ustore_collection_id_t collection, ustore_key_t key,
                                        ustore_view_t view = nullptr) {
    ustore_arena_t arena = nullptr;
    ustore_outcome_t outcome {};
    ustore_arena_init_t init {.arena = &arena, .outcome = &outcome};
    ustore_arena_init(&init);
    ustore_octet_t const* presences = nullptr;
    ustore_strings_view_t values {};
    ustore_blobs_read_t read {.database = database,
                              .outcome = &outcome,
                              .view = view,
                              .count = 1,
                              .collections = {&collection, 0},
                              .keys = {&key, 0},
                              .presences = &presences,
                              .values = &values,
                              .arena = arena};
    ustore_blobs_read(&read);
    std::optional<std::string> value;
    if (outcome.status == ustore_status_success_k && presences && (presences[0] & 1)) {
        ustore_bytes_view_t const bytes = tape_string(values, 0);
        value.emplace(reinterpret_cast<char const*>(bytes.data), bytes.length);
    }
    ustore_arena_free(arena);
    return value;
}

/** A column over memory the case owns, shaped by hand: its schema, its array and their buffers. */
struct column_t {
    std::array<void const*, 3> buffers {};
    ArrowSchema schema {};
    ArrowArray array {};

    column_t(char const* format, std::int64_t length, std::int64_t buffers_count) noexcept {
        schema = {.format = format, .name = "", .release = &release_schema};
        array = {.length = length, .n_buffers = buffers_count, .buffers = buffers.data(), .release = &release_array};
    }

    static void release_schema(ArrowSchema* schema) noexcept { schema->release = nullptr; }
    static void release_array(ArrowArray* array) noexcept { array->release = nullptr; }
};

/** Converters read keys and tapes in place at the column's offset, and refuse nulls, dictionaries, views, other
 *  formats, released inputs and missing buffers. */
std::size_t arrow_converters(config_t const&) {
    std::size_t failures = 0;
    std::array<ustore_key_t, 3> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    column_t keyed {"w:16", 2, 2};
    keyed.buffers[1] = keys.data();
    keyed.array.offset = 1;
    ustore_keys_view_t view {};
    ustore_size_t count = 0;
    failures += expect_eq(ustore_keys_view_from_arrow(&keyed.schema, &keyed.array, &view, &count),
                          ustore_status_success_k);
    failures += expect_eq(count, 2u);
    failures += expect(view.data == keys.data() + 1 && view.stride == sizeof(ustore_key_t));

    std::array<std::uint32_t, 4> const narrow {0, 1, 3, 6};
    std::array<std::uint64_t, 4> const wide {0, 1, 3, 6};
    for (char const* format : {"z", "u", "Z", "U"}) {
        bool const is_wide = format[0] == 'Z' || format[0] == 'U';
        column_t texts {format, 2, 3};
        texts.buffers[1] = is_wide ? static_cast<void const*>(wide.data()) : narrow.data();
        texts.buffers[2] = "abbccc";
        texts.array.offset = 1;
        ustore_strings_view_t strings {};
        failures += expect_eq(ustore_strings_view_from_arrow(&texts.schema, &texts.array, &strings, &count),
                              ustore_status_success_k);
        failures += expect_eq(count, 2u);
        failures += expect(cell_of(strings, 0) == "bb" && cell_of(strings, 1) == "ccc");
        failures += expect_eq(strings.layout, is_wide ? ustore_strings_u64_tape_k : ustore_strings_u32_tape_k);
    }

    std::uint8_t const validity = 0b01;
    column_t nulled {"w:16", 2, 2};
    nulled.buffers = {&validity, keys.data(), nullptr};
    nulled.array.null_count = 1;
    failures += expect_eq(ustore_keys_view_from_arrow(&nulled.schema, &nulled.array, &view, &count),
                          ustore_status_invalid_argument_k);
    nulled.array.null_count = -1;
    failures += expect_eq(ustore_keys_view_from_arrow(&nulled.schema, &nulled.array, &view, &count),
                          ustore_status_invalid_argument_k);
    column_t short_of_buffers {"z", 2, 2};
    ustore_strings_view_t strings {};
    failures += expect_eq(
        ustore_strings_view_from_arrow(&short_of_buffers.schema, &short_of_buffers.array, &strings, &count),
        ustore_status_malformed_k);
    for (char const* format : {"vz", "vu", "w:8", "i"}) {
        column_t other {format, 2, 3};
        failures += expect_eq(ustore_strings_view_from_arrow(&other.schema, &other.array, &strings, &count),
                              ustore_status_unsupported_k);
    }
    column_t dictionary {"w:16", 2, 2};
    dictionary.schema.dictionary = &keyed.schema;
    failures += expect_eq(ustore_keys_view_from_arrow(&dictionary.schema, &dictionary.array, &view, &count),
                          ustore_status_unsupported_k);
    keyed.array.release = nullptr;
    failures += expect_eq(ustore_keys_view_from_arrow(&keyed.schema, &keyed.array, &view, &count),
                          ustore_status_invalid_argument_k);
    return failures;
}

/** Gathered columns export with their validities: numbers and binary strings in place, booleans
 *  packed into bits, and strings without the null byte ending every row. */
std::size_t arrow_docs_columns_export(config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.database) return failures + 1;
    ustore_collection_id_t const collection = fixture.create("scientists", ustore_modality_docs_k);
    std::array<ustore_key_t, 10> keys {};
    std::array<char[64], 10> texts {};
    std::array<std::string_view, 10> documents {};
    for (std::size_t row = 0; row != keys.size(); ++row) {
        keys[row] = key_from_integer(row + 1);
        char* const end = row == 4 ? fmt::format_to(texts[row], R"({{"born": "unknown"}})")
                                   : fmt::format_to(texts[row], R"({{"alive": {}, "born": {}, "name": "n{}"}})",
                                                    row % 3 == 0, 1900 + row, row);
        documents[row] = {texts[row], end};
    }
    strided_strings<std::string_view> const values {strided(std::as_const(documents)).begin()};
    ustore_outcome_t outcome {};
    ustore_docs_write_t write {.database = fixture.database,
                               .outcome = &outcome,
                               .count = keys.size(),
                               .collections = {&collection, 0},
                               .keys = {keys.data(), sizeof(ustore_key_t)},
                               .values = values.view()};
    ustore_docs_write(&write);
    if ((failures += expect_success(outcome))) return failures;

    std::array<std::string_view, 4> const fields {"alive", "born", "name", "name"};
    strided_strings<std::string_view> const fields_strings {strided(fields).begin()};
    std::array<ustore_docs_field_type_t, 4> const types {ustore_docs_field_type_bool_k, ustore_docs_field_type_u32_k,
                                                         ustore_docs_field_type_string_k,
                                                         ustore_docs_field_type_binary_k};
    ustore_arena_t arena = nullptr;
    ustore_arena_init_t made {.arena = &arena, .outcome = &outcome};
    ustore_arena_init(&made);
    ustore_docs_column_view_t const* columns = nullptr;
    ustore_docs_gather_t gather {.database = fixture.database,
                                 .outcome = &outcome,
                                 .count = keys.size(),
                                 .collections = {&collection, 0},
                                 .keys = {keys.data(), sizeof(ustore_key_t)},
                                 .fields_count = fields.size(),
                                 .fields = fields_strings.view(),
                                 .columns = &columns,
                                 .arena = arena,
                                 .outputs_types = types.data()};
    ustore_docs_gather(&gather);
    if ((failures += expect_success(outcome))) return failures;

    std::array<ArrowSchema, 4> schemas {};
    std::array<ArrowDeviceArray, 4> arrays {};
    for (std::size_t field = 0; field != fields.size(); ++field) {
        failures += expect_eq(ustore_docs_column_view_to_arrow(&columns[field], keys.size(), types[field], arena,
                                                               &schemas[field], &arrays[field]),
                              ustore_status_success_k);
        failures += expect(arrays[field].array.buffers[0] == columns[field].validities);
        failures += expect_eq(arrays[field].array.null_count, -1);
    }
    failures += expect_eq(std::string_view(schemas[0].format), std::string_view("b"));
    failures += expect_eq(std::string_view(schemas[1].format), std::string_view("I"));
    failures += expect_eq(std::string_view(schemas[2].format), std::string_view("u"));
    failures += expect_eq(std::string_view(schemas[3].format), std::string_view("z"));
    failures += expect(arrays[1].array.buffers[1] == columns[1].scalars);
    auto const* const alive = static_cast<std::uint8_t const*>(arrays[0].array.buffers[1]);
    auto const* const names = static_cast<std::uint32_t const*>(arrays[2].array.buffers[1]);
    auto const* const name_bytes = static_cast<char const*>(arrays[2].array.buffers[2]);
    for (std::size_t row = 0; row != keys.size(); ++row) {
        if (row == 4) continue;
        char name[8];
        failures += expect_eq(((alive[row / 8] >> (row % 8)) & 1u) != 0, row % 3 == 0);
        failures += expect_eq(std::string_view(name_bytes + names[row], names[row + 1] - names[row]),
                              std::string_view(name, fmt::format_to(name, "n{}", row)));
    }
    failures += expect_eq(names[5] - names[4], 0u);
    failures += expect_eq(ustore_docs_column_view_to_arrow(&columns[0], keys.size(), ustore_docs_field_type_json_k,
                                                           arena, &schemas[0], &arrays[0]),
                          ustore_status_invalid_argument_k);
    for (std::size_t field = 0; field != fields.size(); ++field) {
        arrays[field].array.release(&arrays[field].array);
        schemas[field].release(&schemas[field]);
    }
    ustore_arena_free(arena);
    return failures;
}

/** A cursor's keys and values export in place, read back through the converters, pin the arena against the next
 *  call until released, and outlive the arena's free. */
std::size_t arrow_exports_pin_the_arena(config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.database) return failures + 1;
    ustore_collection_id_t const collection = fixture.create("posters", ustore_modality_blobs_k);
    if ((failures += fill_blobs(fixture.database, collection, 100))) return failures;

    ustore_blobs_cursor_t cursor = nullptr;
    ustore_outcome_t outcome {};
    ustore_blobs_cursor_init_t init {.cursor = &cursor,
                                     .database = fixture.database,
                                     .outcome = &outcome,
                                     .collection = collection};
    ustore_blobs_cursor_init(&init);
    if ((failures += expect_success(outcome))) return failures;
    ustore_arena_t arena = nullptr;
    ustore_arena_init_t made {.arena = &arena, .outcome = &outcome};
    ustore_arena_init(&made);
    ustore_key_t const* keys = nullptr;
    ustore_strings_view_t values {};
    ustore_size_t written = 0;
    ustore_blobs_cursor_next_t next {.database = fixture.database,
                                     .outcome = &outcome,
                                     .cursor = cursor,
                                     .keys = &keys,
                                     .values = &values,
                                     .written = &written,
                                     .arena = arena,
                                     .wanted = 40};
    ustore_blobs_cursor_next(&next);
    failures += expect_success(outcome);
    failures += expect_eq(written, 40u);

    ArrowSchema key_schema {}, value_schema {};
    ArrowDeviceArray key_array {}, value_array {};
    failures += expect_eq(
        ustore_keys_view_to_arrow({keys, sizeof(ustore_key_t)}, written, arena, &key_schema, &key_array),
        ustore_status_success_k);
    failures += expect_eq(
        ustore_strings_view_to_arrow(values, written, ustore_strings_u32_tape_k, arena, &value_schema, &value_array),
        ustore_status_success_k);
    failures += expect_eq(std::string_view(key_schema.format), std::string_view("w:16"));
    failures += expect(key_schema.metadata != nullptr);
    failures += expect_eq(key_array.device_type, ARROW_DEVICE_CPU);

    // A key outside the arena is refused, while a strided run is carved dense behind the outputs
    ustore_key_t const outside = key_from_integer(7);
    ArrowSchema refused_schema {};
    ArrowDeviceArray refused_array {};
    failures += expect_eq(
        ustore_keys_view_to_arrow({&outside, sizeof(ustore_key_t)}, 1, arena, &refused_schema, &refused_array),
        ustore_status_invalid_argument_k);
    ArrowSchema every_other_schema {};
    ArrowDeviceArray every_other_array {};
    failures += expect_eq(ustore_keys_view_to_arrow({keys, 2 * sizeof(ustore_key_t)}, written / 2, arena,
                                                    &every_other_schema, &every_other_array),
                          ustore_status_success_k);
    auto const* const carved = static_cast<ustore_key_t const*>(every_other_array.array.buffers[1]);
    for (std::size_t row = 0; row != written / 2; ++row)
        failures += expect(cell_of(carved[row]) == cell_of(keys[2 * row]));
    every_other_array.array.release(&every_other_array.array);
    every_other_schema.release(&every_other_schema);

    // A tape asked for in the other width is converted behind the outputs
    ArrowSchema wide_schema {};
    ArrowDeviceArray wide_array {};
    failures += expect_eq(
        ustore_strings_view_to_arrow(values, written, ustore_strings_u64_tape_k, arena, &wide_schema, &wide_array),
        ustore_status_success_k);
    failures += expect_eq(std::string_view(wide_schema.format), std::string_view("Z"));
    ustore_strings_view_t widened {};
    ustore_size_t widened_count = 0;
    failures += expect_eq(ustore_strings_view_from_arrow(&wide_schema, &wide_array.array, &widened, &widened_count),
                          ustore_status_success_k);
    failures += expect(widened_count == written && cell_of(widened, 39) == cell_of(values, 39));

    // A wide tape narrows from its first byte, and one spanning past 32 bits is refused before its
    // bytes are looked for
    ustore_strings_view_t sliced = widened;
    sliced.data.u64_tape.offsets = widened.data.u64_tape.offsets + 1;
    ArrowSchema sliced_schema {};
    ArrowDeviceArray sliced_array {};
    failures += expect_eq(ustore_strings_view_to_arrow(sliced, written - 1, ustore_strings_u32_tape_k, arena,
                                                       &sliced_schema, &sliced_array),
                          ustore_status_success_k);
    ustore_strings_view_t narrowed {};
    ustore_size_t narrowed_count = 0;
    failures += expect_eq(
        ustore_strings_view_from_arrow(&sliced_schema, &sliced_array.array, &narrowed, &narrowed_count),
        ustore_status_success_k);
    failures += expect(narrowed_count == written - 1 && cell_of(narrowed, 38) == cell_of(values, 39));
    sliced_array.array.release(&sliced_array.array);
    sliced_schema.release(&sliced_schema);
    std::uint64_t const far_offsets[] {5ull << 30, (5ull << 30) + (std::uint64_t {1} << 32)};
    ustore_strings_view_t far {};
    far.layout = ustore_strings_u64_tape_k;
    far.data.u64_tape.data = widened.data.u64_tape.data, far.data.u64_tape.offsets = far_offsets;
    ArrowSchema far_schema {};
    ArrowDeviceArray far_array {};
    failures += expect_eq(
        ustore_strings_view_to_arrow(far, 1, ustore_strings_u32_tape_k, arena, &far_schema, &far_array),
        ustore_status_capacity_exhausted_k);
    wide_array.array.release(&wide_array.array);
    wide_schema.release(&wide_schema);

    // While exported, the arena refuses the next pull, which clears its outputs
    ustore_key_t const* const pulled_keys = keys;
    ustore_size_t const pulled = written;
    std::string const first_value = cell_of(values, 0);
    ustore_blobs_cursor_next(&next);
    failures += expect_status(status_t {outcome}, status_code_t::contended_k);

    ustore_keys_view_t read_keys {};
    ustore_strings_view_t read_values {};
    ustore_size_t key_count = 0, value_count = 0;
    failures += expect_eq(ustore_keys_view_from_arrow(&key_schema, &key_array.array, &read_keys, &key_count),
                          ustore_status_success_k);
    failures += expect_eq(ustore_strings_view_from_arrow(&value_schema, &value_array.array, &read_values, &value_count),
                          ustore_status_success_k);
    failures += expect(read_keys.data == pulled_keys && key_count == pulled && value_count == pulled);

    // Freed while exported, the arena lives until the last release
    ustore_arena_free(arena);
    failures += expect(cell_of(read_values, 0) == first_value);
    failures += expect(cell_of(read_keys.data[39]) == cell_of(key_from_integer(40)));
    key_array.array.release(&key_array.array);
    failures += expect(cell_of(read_values, 39) == "value-40");
    value_array.array.release(&value_array.array);
    key_schema.release(&key_schema);
    value_schema.release(&value_schema);
    ustore_blobs_cursor_free(cursor);
    return failures;
}

/** What an ingest of two rows answers on one engine. */
struct ingest_expectation_t {
    std::string_view engine;
    status_code_t status;
};

/** Every engine's answer to an ingest, one row each, so an engine gaining ingests flips its own row alone. */
constexpr ingest_expectation_t ingest_expectations_k[] {
    {"ram", status_code_t::success_k},
    {"rocksdb", status_code_t::unsupported_k},
    {"leveldb", status_code_t::unsupported_k},
    {"nvme", status_code_t::success_k},
};

/** An ingest the case frees on destruction. */
struct ingest_t {
    ustore_ingest_t ingest = nullptr;
    ustore_outcome_t outcome {};

    ingest_t(ustore_database_t database, ustore_collection_id_t collection, ustore_ingest_order_t order) noexcept {
        ustore_ingest_init_t init {.ingest = &ingest,
                                   .database = database,
                                   .outcome = &outcome,
                                   .collection = collection,
                                   .order = order};
        ustore_ingest_init(&init);
    }
    ingest_t(ingest_t const&) = delete;
    ingest_t& operator=(ingest_t const&) = delete;
    ~ingest_t() noexcept { ustore_ingest_free(ingest); }

    /** Appends rows @b [first,first+count) of @p rows, keyed or as edges by @p modality. */
    ustore_outcome_t append(ustore_modality_t modality, modality_rows_t const& rows, std::size_t first,
                            std::size_t count) noexcept {
        ustore_outcome_t appended {};
        ustore_keys_view_t const keys {rows.keys.data() + first, sizeof(ustore_key_t)};
        ustore_ingest_append_t append {.ingest = ingest, .outcome = &appended, .count = count};
        if (modality == ustore_modality_graph_k)
            append.sources = keys, append.targets = {rows.targets.data() + first, sizeof(ustore_key_t)};
        else append.keys = keys, append.values = tape_slice(rows.texts.view(), first);
        ustore_ingest_append(&append);
        return appended;
    }

    /** Lands every row appended, answering the outcome and the stamp. */
    ustore_outcome_t finish(ustore_stamp_t* stamp = nullptr) noexcept {
        ustore_outcome_t finished {};
        ustore_ingest_finish_t finish {.ingest = ingest, .outcome = &finished, .commit_stamp = stamp};
        ustore_ingest_finish(&finish);
        return finished;
    }
};

/** Counts a failure unless @p outcome succeeded, for an outcome a call answered. */
std::size_t expect_succeeded(ustore_outcome_t outcome) { return expect_success(outcome); }

/** Whether the engine @p config ingests, as its row of @c ingest_expectations_k says. */
bool ingests(config_t const& config) noexcept {
    for (ingest_expectation_t const& row : ingest_expectations_k)
        if (row.engine == config.engine()) return row.status == status_code_t::success_k;
    return false;
}

/** The reason a case gated on an ingesting engine reports elsewhere. */
constexpr char const* needs_ingest_k = "needs an engine whose row of `ingest_expectations_k` ingests";

/** Every engine opens an ingest as its row of @c ingest_expectations_k says. */
case_result_t arrow_ingest_per_engine(config_t const& config) {
    ingest_expectation_t const* expectation = nullptr;
    for (ingest_expectation_t const& row : ingest_expectations_k)
        if (row.engine == config.engine()) expectation = &row;
    if (!expectation) return case_result_t::skipped("the engine has no row in `ingest_expectations_k`");
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.database) return failures + 1;
    ustore_collection_id_t const collection = fixture.create("ingested", ustore_modality_blobs_k);
    ingest_t ingest {fixture.database, collection, ustore_ingest_sorted_k};
    failures += expect_status(status_t {ingest.outcome}, expectation->status);
    failures += expect_eq(ingest.ingest != nullptr, expectation->status == status_code_t::success_k);
    if (!ingest.ingest) return failures;
    modality_rows_t const rows {ustore_modality_blobs_k, 2};
    failures += expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 0, 2));
    failures += expect_succeeded(ingest.finish());
    // A finished ingest takes no more rows
    failures += expect_status(status_t {ingest.append(ustore_modality_blobs_k, rows, 0, 2)},
                              status_code_t::invalid_argument_k);
    return failures;
}

/** An ingest of every modality, in either order and over several appends, lands what the modality's writes land. */
case_result_t arrow_ingest_equals_writes(config_t const& config) {
    if (!ingests(config)) return case_result_t::skipped(needs_ingest_k);
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.database) return failures + 1;
    constexpr std::size_t count_k = 100;
    for (ustore_modality_t const modality : {ustore_modality_blobs_k, ustore_modality_docs_k, ustore_modality_pages_k,
                                             ustore_modality_strings_k, ustore_modality_graph_k}) {
        char const* const name = ustore_modality_name(modality);
        modality_rows_t const rows {modality, count_k};
        ustore_collection_id_t const written_collection = fixture.create(fmt::format("{}-written", name).c_str(),
                                                                         modality);
        if ((failures += fill(fixture.database, written_collection, modality, rows))) return failures;
        rows_t from_writes;
        failures += dump(fixture.database, written_collection, modality, from_writes);
        failures += expect_eq(from_writes.size(), count_k);

        for (ustore_ingest_order_t const order : {ustore_ingest_sorted_k, ustore_ingest_unsorted_k}) {
            std::string const ingested_name = fmt::format("{}-ingested-{}", name, ustore_ingest_order_name(order));
            ustore_collection_id_t const ingested = fixture.create(ingested_name.c_str(), modality);
            ingest_t ingest {fixture.database, ingested, order};
            if ((failures += expect_succeeded(ingest.outcome))) return failures;
            // Unsorted, the later half arrives first
            std::array<std::size_t, 3> firsts {0, 30, 70};
            if (order == ustore_ingest_unsorted_k) firsts = {70, 30, 0};
            for (std::size_t const first : firsts) {
                std::size_t const count = first == 70 ? 30 : first == 30 ? 40 : 30;
                failures += expect_succeeded(ingest.append(modality, rows, first, count));
            }
            ustore_stamp_t stamp = 0;
            failures += expect_succeeded(ingest.finish(&stamp));
            failures += expect(stamp != 0);
            rows_t from_ingest;
            failures += dump(fixture.database, ingested, modality, from_ingest);
            if (from_ingest != from_writes)
                fmt::println(stderr, "  the {} ingest differs from the writes", ingested_name);
            failures += expect(from_ingest == from_writes);
        }
    }
    return failures;
}

/** Under @c ustore_ingest_unsorted_k a repeated key keeps the last of its rows. */
case_result_t arrow_ingest_last_repeat_wins(config_t const& config) {
    if (!ingests(config)) return case_result_t::skipped(needs_ingest_k);
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.database) return failures + 1;
    ustore_collection_id_t const collection = fixture.create("repeated", ustore_modality_blobs_k);
    modality_rows_t rows {ustore_modality_blobs_k, 3};
    rows.keys = {key_from_integer(7), key_from_integer(7), key_from_integer(7)};
    ingest_t ingest {fixture.database, collection, ustore_ingest_unsorted_k};
    failures += expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 0, 2));
    failures += expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 2, 1));
    failures += expect_succeeded(ingest.finish());
    failures += expect(stored_value(fixture.database, collection, key_from_integer(7)) ==
                       std::optional<std::string> {"text-003"});
    return failures;
}

/** Under @c ustore_ingest_sorted_k a key below the one before it, in its append or the one before, refuses that
 *  append alone. */
case_result_t arrow_ingest_refuses_a_descending_key(config_t const& config) {
    if (!ingests(config)) return case_result_t::skipped(needs_ingest_k);
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.database) return failures + 1;
    ustore_collection_id_t const collection = fixture.create("descending", ustore_modality_blobs_k);
    modality_rows_t rows {ustore_modality_blobs_k, 4};
    std::reverse(rows.keys.begin() + 2, rows.keys.end());
    ingest_t ingest {fixture.database, collection, ustore_ingest_sorted_k};
    failures += expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 0, 2));
    failures += expect_status(status_t {ingest.append(ustore_modality_blobs_k, rows, 2, 2)},
                              status_code_t::invalid_argument_k);
    failures += expect_status(status_t {ingest.append(ustore_modality_blobs_k, rows, 0, 1)},
                              status_code_t::invalid_argument_k);
    failures += expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 3, 1));
    failures += expect_succeeded(ingest.finish());
    failures += expect(!stored_value(fixture.database, collection, key_from_integer(4)).has_value());
    failures += expect(stored_value(fixture.database, collection, key_from_integer(3)) ==
                       std::optional<std::string> {"text-004"});
    return failures;
}

/** A freed ingest that never finished lands nothing. */
case_result_t arrow_ingest_freed_lands_nothing(config_t const& config) {
    if (!ingests(config)) return case_result_t::skipped(needs_ingest_k);
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.database) return failures + 1;
    ustore_collection_id_t const collection = fixture.create("abandoned", ustore_modality_blobs_k);
    modality_rows_t const rows {ustore_modality_blobs_k, 2};
    {
        ingest_t ingest {fixture.database, collection, ustore_ingest_sorted_k};
        failures += expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 0, 2));
    }
    failures += expect(!stored_value(fixture.database, collection, key_from_integer(1)).has_value());
    return failures;
}

/** A snapshot taken before an ingest reads the value the key held before it. */
case_result_t arrow_ingest_spares_older_snapshots(config_t const& config) {
    if (!ingests(config)) return case_result_t::skipped(needs_ingest_k);
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.database) return failures + 1;
    ustore_collection_id_t const collection = fixture.create("versioned", ustore_modality_blobs_k);
    if ((failures += fill_blobs(fixture.database, collection, 1))) return failures;
    ustore_snapshot_t snapshot = nullptr;
    ustore_outcome_t outcome {};
    ustore_snapshot_init_t init {.snapshot = &snapshot, .database = fixture.database, .outcome = &outcome};
    ustore_snapshot_init(&init);
    if ((failures += expect_success(outcome))) return failures;

    modality_rows_t const rows {ustore_modality_blobs_k, 2};
    {
        ingest_t ingest {fixture.database, collection, ustore_ingest_sorted_k};
        failures += expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 0, 2));
        failures += expect_succeeded(ingest.finish());
    }
    std::optional<std::string> const before = stored_value(fixture.database, collection, key_from_integer(1),
                                                           ustore_snapshot_view(snapshot));
    failures += expect(before == std::optional<std::string> {"value-1"});
    failures += expect(stored_value(fixture.database, collection, key_from_integer(1)) ==
                       std::optional<std::string> {"text-001"});
    ustore_snapshot_free(snapshot);
    return failures;
}

/** A transaction that began before an ingest and wrote a key the ingest lands fails at commit. */
case_result_t arrow_ingest_conflicts_older_transactions(config_t const& config) {
    if (!ingests(config)) return case_result_t::skipped(needs_ingest_k);
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.database) return failures + 1;
    ustore_collection_id_t const collection = fixture.create("contested", ustore_modality_blobs_k);
    ustore_transaction_t transaction = nullptr;
    ustore_outcome_t outcome {};
    ustore_transaction_init_t init {.transaction = &transaction, .database = fixture.database, .outcome = &outcome};
    ustore_transaction_init(&init);
    if ((failures += expect_success(outcome))) return failures;
    ustore_key_t const key = key_from_integer(1);
    tape_t values;
    values.push("staged");
    ustore_blobs_write_t write {.database = fixture.database,
                                .outcome = &outcome,
                                .transaction = transaction,
                                .count = 1,
                                .collections = {&collection, 0},
                                .keys = {&key, 0},
                                .values = values.view()};
    ustore_blobs_write(&write);
    failures += expect_success(outcome);

    modality_rows_t const rows {ustore_modality_blobs_k, 2};
    {
        ingest_t ingest {fixture.database, collection, ustore_ingest_sorted_k};
        failures += expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 0, 2));
        failures += expect_succeeded(ingest.finish());
    }
    ustore_transaction_commit_t commit {.database = fixture.database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_commit(&commit);
    failures += expect_status(status_t {outcome}, status_code_t::write_conflict_k);
    ustore_transaction_free(transaction);
    return failures;
}

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    auto const maybe_test_directory = directory_t::make(options.directory);
    if (!maybe_test_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", options.directory);
        return 1;
    }
    char const* const directory = maybe_test_directory->path();
    if (directory) fmt::println("Will work in directory: {}", directory);
    else fmt::println("Will work with default configuration");
    config_t const config {options.engine, directory};

    report(totals, "arrow.converters", arrow_converters, config);
    report(totals, "arrow.exports_pin_the_arena", arrow_exports_pin_the_arena, config);
    report(totals, "arrow.docs_columns_export", arrow_docs_columns_export, config);
    report(totals, "arrow.ingest_per_engine", arrow_ingest_per_engine, config);
    report(totals, "arrow.ingest_equals_writes", arrow_ingest_equals_writes, config);
    report(totals, "arrow.ingest_last_repeat_wins", arrow_ingest_last_repeat_wins, config);
    report(totals, "arrow.ingest_refuses_a_descending_key", arrow_ingest_refuses_a_descending_key, config);
    report(totals, "arrow.ingest_freed_lands_nothing", arrow_ingest_freed_lands_nothing, config);
    report(totals, "arrow.ingest_spares_older_snapshots", arrow_ingest_spares_older_snapshots, config);
    report(totals, "arrow.ingest_conflicts_older_transactions", arrow_ingest_conflicts_older_transactions, config);

    return suite_exit_code(totals);
}
