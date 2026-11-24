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
#include <limits>      // `std::numeric_limits`
#include <optional>    // `std::optional`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <utility>     // `std::as_const`
#include <vector>      // `std::vector`

#include <fmt/format.h>

#include "ustore/utilities/ranges.hpp" // `tape_string`, `strided_strings`
#include "ustore/ustore.h"

#include "harness.hpp" // `run_test_case`, `expect_true`, `expect_equal`, `open_raw`

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
        if (!expect_status(open_status, util::status_code_t::success_k)) return;
        database = opened;
        ustore_outcome_t outcome {};
        ustore_database_clear_t clear {.database = database, .outcome = &outcome, .mode = ustore_drop_collection_k};
        ustore_database_clear(&clear);
        expect_success(outcome);
    }

    ~fixture_t() noexcept { ustore_database_free(database); }

    /** Creates a collection of @p modality named @p name, answering the nil identifier on failure. */
    ustore_collection_id_t create(char const* name, ustore_modality_t modality) noexcept {
        ustore_collection_id_t collection {};
        ustore_outcome_t outcome {};
        ustore_collection_create_t create {.database = database,
                                           .outcome = &outcome,
                                           .name = util::bytes_of(name),
                                           .modality = modality,
                                           .collection = &collection};
        ustore_collection_create(&create);
        expect_success(outcome);
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

    ustore_strings_view_t view() const noexcept { return util::u32_tape(bytes.data(), offsets.data()); }
};

/** The bytes of @p key. */
std::string cell_of(ustore_key_t const& key) { return std::string(reinterpret_cast<char const*>(key.uuid.bytes), 16); }

/** The bytes of string @p index of @p tape. */
std::string cell_of(ustore_strings_view_t const& tape, std::size_t index) {
    ustore_bytes_view_t const bytes = util::tape_string(tape, index);
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

/** Pulls up to @p wanted keys and values of @p cursor through @p next_ into @p arena, appending
 *  them to @p rows, and answers how many it pulled, none once the pull is refused. */
template <typename next_type_, typename cursor_type_>
ustore_size_t pull_keyed(ustore_database_t database, cursor_type_ cursor, void (*next_)(next_type_*),
                         ustore_arena_t arena, ustore_size_t wanted, rows_t& rows) {
    ustore_outcome_t outcome {};
    ustore_key_t const* keys = nullptr;
    ustore_strings_view_t values {};
    ustore_size_t written = 0;
    next_type_ next {};
    next.database = database, next.outcome = &outcome, next.cursor = cursor, next.keys = &keys;
    next.values = &values, next.written = &written, next.arena = arena, next.wanted = wanted;
    if constexpr (requires { next.outputs_encoding; }) next.outputs_encoding = ustore_page_encoding_xml_k;
    next_(&next);
    if (!expect_success(outcome)) return 0;
    for (ustore_size_t index = 0; index != written; ++index)
        rows.push_back({cell_of(keys[index]), cell_of(values, index)});
    return written;
}

/** Walks @p cursor of @p init_ to its end through @p next_, appending every row to @p rows. */
template <typename cursor_type_, typename init_type_, typename next_type_>
void walk_keyed(ustore_database_t database, ustore_collection_id_t collection, void (*init_)(init_type_*),
                void (*next_)(next_type_*), void (*free_)(cursor_type_), rows_t& rows) {
    cursor_type_ cursor = nullptr;
    ustore_outcome_t outcome {};
    init_type_ init {};
    init.cursor = &cursor, init.database = database, init.outcome = &outcome, init.collection = collection;
    init_(&init);
    if (!expect_success(outcome)) return;
    arena_t arena;
    while (pull_keyed(database, cursor, next_, arena.arena, 64, rows)) {}
    free_(cursor);
}

/** Walks the documents of @p collection to their end, appending every key and whole document to @p
 *  rows. */
void walk_documents(ustore_database_t database, ustore_collection_id_t collection, rows_t& rows) {
    ustore_docs_cursor_t cursor = nullptr;
    ustore_outcome_t outcome {};
    std::array<std::uint32_t, 2> const whole_offsets {0, 0};
    ustore_docs_field_type_t const whole_type = ustore_docs_field_type_json_k;
    ustore_docs_cursor_init_t init {};
    init.cursor = &cursor, init.database = database, init.outcome = &outcome, init.collection = collection;
    init.fields_count = 1, init.fields = util::u32_tape("", whole_offsets.data()), init.columns_types = &whole_type;
    ustore_docs_cursor_init(&init);
    if (!expect_success(outcome)) return;
    arena_t arena;
    for (ustore_size_t count = 1; count;) {
        ustore_docs_batch_view_t const* batches = nullptr;
        ustore_docs_cursor_next_t next {};
        next.database = database, next.outcome = &outcome, next.cursor = cursor, next.batches = &batches;
        next.batches_count = &count, next.arena = arena.arena, next.wanted = 64;
        ustore_docs_cursor_next(&next);
        if (!expect_success(outcome)) break;
        for (ustore_docs_batch_view_t const& batch : std::span<ustore_docs_batch_view_t const> {batches, count})
            for (std::size_t index = 0; index != batch.count; ++index)
                rows.push_back({cell_of(batch.keys[index]), cell_of(batch.columns[0].strings, index)});
    }
    ustore_docs_cursor_free(cursor);
}

/** Reads every row of @p collection of @p modality into @p rows through its cursor, a graph as its edges. */
void dump(ustore_database_t database, ustore_collection_id_t collection, ustore_modality_t modality, rows_t& rows) {
    switch (modality) {
    case ustore_modality_docs_k: return walk_documents(database, collection, rows);
    case ustore_modality_pages_k:
        return walk_keyed(database, collection, &ustore_pages_cursor_init, &ustore_pages_cursor_next,
                          &ustore_pages_cursor_free, rows);
    case ustore_modality_strings_k: break;
    case ustore_modality_graph_k: break;
    default:
        return walk_keyed(database, collection, &ustore_blobs_cursor_init, &ustore_blobs_cursor_next,
                          &ustore_blobs_cursor_free, rows);
    }
    ustore_outcome_t outcome {};
    arena_t arena;
    if (modality == ustore_modality_strings_k) {
        ustore_strings_cursor_t cursor = nullptr;
        ustore_strings_cursor_init_t init {.cursor = &cursor,
                                           .database = database,
                                           .outcome = &outcome,
                                           .collection = collection};
        ustore_strings_cursor_init(&init);
        if (!expect_success(outcome)) return;
        for (ustore_size_t written = 1; written;) {
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
            if (!expect_success(outcome)) break;
            for (ustore_size_t index = 0; index != written; ++index)
                rows.push_back({cell_of(strings, index), cell_of(values[index])});
        }
        ustore_strings_cursor_free(cursor);
        return;
    }
    ustore_graph_vertices_cursor_t cursor = nullptr;
    ustore_graph_vertices_cursor_init_t init {.cursor = &cursor,
                                              .database = database,
                                              .outcome = &outcome,
                                              .collection = collection};
    ustore_graph_vertices_cursor_init(&init);
    if (!expect_success(outcome)) return;
    arena_t edges_arena;
    for (ustore_size_t written = 1; written;) {
        ustore_key_t const* vertices = nullptr;
        ustore_graph_vertices_cursor_next_t next {.database = database,
                                                  .outcome = &outcome,
                                                  .cursor = cursor,
                                                  .keys = &vertices,
                                                  .written = &written,
                                                  .arena = arena.arena,
                                                  .wanted = 64};
        ustore_graph_vertices_cursor_next(&next);
        if (!expect_success(outcome) || !written) break;
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
        expect_success(outcome);
        for (ustore_length_t edge = 0; offsets && edge != offsets[written]; ++edge)
            rows.push_back(
                {cell_of(triples[edge * 3]), cell_of(triples[edge * 3 + 1]), cell_of(triples[edge * 3 + 2])});
    }
    ustore_graph_vertices_cursor_free(cursor);
}

/** Writes @p count blobs, keyed 1 to @p count, each valued "value-" and its key. */
bool fill_blobs(ustore_database_t database, ustore_collection_id_t collection, std::size_t count) {
    std::vector<ustore_key_t> keys;
    tape_t values;
    for (std::size_t index = 1; index <= count; ++index) {
        keys.push_back(util::key_from_integer(index));
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
            keys.push_back(util::key_from_integer(index));
            targets.push_back(util::key_from_integer(index + 1));
            if (modality == ustore_modality_docs_k) texts.push(fmt::format("{{\"index\":{}}}", index));
            else if (modality == ustore_modality_pages_k) texts.push(fmt::format("<p>{}</p>", index));
            else texts.push(fmt::format("text-{:03}", index));
        }
    }
};

/** Fills @p collection of @p modality with @p rows through its own writes. */
bool fill(ustore_database_t database, ustore_collection_id_t collection, ustore_modality_t modality,
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
        ustore_bytes_view_t const bytes = util::tape_string(values, 0);
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
void test_arrow_converters(config_t const&) {
    std::array<ustore_key_t, 3> const keys {util::key_from_integer(1), util::key_from_integer(2),
                                            util::key_from_integer(3)};
    column_t keyed {"w:16", 2, 2};
    keyed.buffers[1] = keys.data();
    keyed.array.offset = 1;
    ustore_keys_view_t view {};
    ustore_size_t count = 0;
    expect_equal(ustore_keys_view_from_arrow(&keyed.schema, &keyed.array, &view, &count), ustore_status_success_k);
    expect_equal(count, 2u);
    expect_true(view.data == keys.data() + 1 && view.stride == sizeof(ustore_key_t));

    std::array<std::uint32_t, 4> const narrow {0, 1, 3, 6};
    std::array<std::uint64_t, 4> const wide {0, 1, 3, 6};
    for (char const* format : {"z", "u", "Z", "U"}) {
        bool const is_wide = format[0] == 'Z' || format[0] == 'U';
        column_t texts {format, 2, 3};
        texts.buffers[1] = is_wide ? static_cast<void const*>(wide.data()) : narrow.data();
        texts.buffers[2] = "abbccc";
        texts.array.offset = 1;
        ustore_strings_view_t strings {};
        expect_equal(ustore_strings_view_from_arrow(&texts.schema, &texts.array, &strings, &count),
                     ustore_status_success_k);
        expect_equal(count, 2u);
        expect_true(cell_of(strings, 0) == "bb" && cell_of(strings, 1) == "ccc");
        expect_equal(strings.layout, is_wide ? ustore_strings_u64_tape_k : ustore_strings_u32_tape_k);
    }

    std::uint8_t const validity = 0b01;
    column_t nulled {"w:16", 2, 2};
    nulled.buffers = {&validity, keys.data(), nullptr};
    nulled.array.null_count = 1;
    expect_equal(ustore_keys_view_from_arrow(&nulled.schema, &nulled.array, &view, &count),
                 ustore_status_invalid_argument_k);
    nulled.array.null_count = -1;
    expect_equal(ustore_keys_view_from_arrow(&nulled.schema, &nulled.array, &view, &count),
                 ustore_status_invalid_argument_k);
    column_t short_of_buffers {"z", 2, 2};
    ustore_strings_view_t strings {};
    expect_equal(ustore_strings_view_from_arrow(&short_of_buffers.schema, &short_of_buffers.array, &strings, &count),
                 ustore_status_malformed_k);
    for (char const* format : {"vz", "vu", "w:8", "i"}) {
        column_t other {format, 2, 3};
        expect_equal(ustore_strings_view_from_arrow(&other.schema, &other.array, &strings, &count),
                     ustore_status_unsupported_k);
    }
    column_t dictionary {"w:16", 2, 2};
    dictionary.schema.dictionary = &keyed.schema;
    expect_equal(ustore_keys_view_from_arrow(&dictionary.schema, &dictionary.array, &view, &count),
                 ustore_status_unsupported_k);
    keyed.array.release = nullptr;
    expect_equal(ustore_keys_view_from_arrow(&keyed.schema, &keyed.array, &view, &count),
                 ustore_status_invalid_argument_k);
}

/** Gathered columns export with their validities: numbers and binary strings in place, booleans
 *  packed into bits, and strings without the null byte ending every row. */
void test_arrow_docs_columns_export(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.database) return;
    ustore_collection_id_t const collection = fixture.create("scientists", ustore_modality_docs_k);
    std::array<ustore_key_t, 10> keys {};
    std::array<char[64], 10> texts {};
    std::array<std::string_view, 10> documents {};
    for (std::size_t row = 0; row != keys.size(); ++row) {
        keys[row] = util::key_from_integer(row + 1);
        char* const end = row == 4 ? fmt::format_to(texts[row], R"({{"born": "unknown"}})")
                                   : fmt::format_to(texts[row], R"({{"alive": {}, "born": {}, "name": "n{}"}})",
                                                    row % 3 == 0, 1900 + row, row);
        documents[row] = {texts[row], end};
    }
    util::strided_strings<std::string_view> const values {util::strided(std::as_const(documents)).begin()};
    ustore_outcome_t outcome {};
    ustore_docs_write_t write {.database = fixture.database,
                               .outcome = &outcome,
                               .count = keys.size(),
                               .collections = {&collection, 0},
                               .keys = {keys.data(), sizeof(ustore_key_t)},
                               .values = values.view()};
    ustore_docs_write(&write);
    if (!expect_success(outcome)) return;

    std::array<std::string_view, 4> const fields {"alive", "born", "name", "name"};
    util::strided_strings<std::string_view> const fields_strings {util::strided(fields).begin()};
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
    if (!expect_success(outcome)) return;

    std::array<ArrowSchema, 4> schemas {};
    std::array<ArrowDeviceArray, 4> arrays {};
    for (std::size_t field = 0; field != fields.size(); ++field) {
        expect_equal(ustore_docs_column_view_to_arrow(&columns[field], keys.size(), types[field], arena,
                                                      &schemas[field], &arrays[field]),
                     ustore_status_success_k);
        expect_true(arrays[field].array.buffers[0] == columns[field].validities);
        expect_equal(arrays[field].array.null_count, -1);
    }
    expect_equal(std::string_view(schemas[0].format), std::string_view("b"));
    expect_equal(std::string_view(schemas[1].format), std::string_view("I"));
    expect_equal(std::string_view(schemas[2].format), std::string_view("u"));
    expect_equal(std::string_view(schemas[3].format), std::string_view("z"));
    expect_true(arrays[1].array.buffers[1] == columns[1].scalars);
    auto const* const alive = static_cast<std::uint8_t const*>(arrays[0].array.buffers[1]);
    auto const* const names = static_cast<std::uint32_t const*>(arrays[2].array.buffers[1]);
    auto const* const name_bytes = static_cast<char const*>(arrays[2].array.buffers[2]);
    for (std::size_t row = 0; row != keys.size(); ++row) {
        if (row == 4) continue;
        char name[8];
        expect_equal(((alive[row / 8] >> (row % 8)) & 1u) != 0, row % 3 == 0);
        expect_equal(std::string_view(name_bytes + names[row], names[row + 1] - names[row]),
                     std::string_view(name, fmt::format_to(name, "n{}", row)));
    }
    expect_equal(names[5] - names[4], 0u);
    expect_equal(ustore_docs_column_view_to_arrow(&columns[0], keys.size(), ustore_docs_field_type_json_k, arena,
                                                  &schemas[0], &arrays[0]),
                 ustore_status_invalid_argument_k);
    for (std::size_t field = 0; field != fields.size(); ++field) {
        arrays[field].array.release(&arrays[field].array);
        schemas[field].release(&schemas[field]);
    }
    ustore_arena_free(arena);
}

/** A text column whose entry lacks the null byte ending it is refused before its copy is carved. */
void test_arrow_export_refuses_an_entry_without_terminator(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.database) return;
    ustore_collection_id_t const collection = fixture.create("unterminated", ustore_modality_blobs_k);
    tape_t empty;
    empty.push("");
    ustore_key_t const key = util::key_from_integer(1);
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {.database = fixture.database,
                                .outcome = &outcome,
                                .count = 1,
                                .collections = {&collection, 0},
                                .keys = {&key, 0},
                                .values = empty.view()};
    ustore_blobs_write(&write);
    if (!expect_success(outcome)) return;

    // The empty value read back gives offsets inside the arena with no byte between them
    arena_t const arena;
    ustore_octet_t const* presences = nullptr;
    ustore_strings_view_t values {};
    ustore_blobs_read_t read {.database = fixture.database,
                              .outcome = &outcome,
                              .count = 1,
                              .collections = {&collection, 0},
                              .keys = {&key, 0},
                              .presences = &presences,
                              .values = &values,
                              .arena = arena.arena};
    ustore_blobs_read(&read);
    if (!expect_success(outcome)) return;
    ustore_docs_column_view_t const column {.validities = presences, .strings = values};
    ArrowSchema schema {};
    ArrowDeviceArray array {};
    expect_equal(
        ustore_docs_column_view_to_arrow(&column, 1, ustore_docs_field_type_string_k, arena.arena, &schema, &array),
        ustore_status_invalid_argument_k);
}

/** A tape whose offsets lie outside the arena is refused, as is one whose count wraps the bytes
 *  of its offsets to the empty run an arena holds anywhere. */
void test_arrow_export_refuses_offsets_outside_the_arena(config_t const&) {
    arena_t const arena;
    tape_t outside;
    outside.push("outside");
    ArrowSchema schema {};
    ArrowDeviceArray array {};
    expect_equal(
        ustore_strings_view_to_arrow(outside.view(), 1, ustore_strings_u32_tape_k, arena.arena, &schema, &array),
        ustore_status_invalid_argument_k);
    std::size_t const wrapping = (std::numeric_limits<std::size_t>::max)() / sizeof(std::uint32_t);
    expect_equal(
        ustore_strings_view_to_arrow(outside.view(), wrapping, ustore_strings_u32_tape_k, arena.arena, &schema, &array),
        ustore_status_invalid_argument_k);
    expect_equal(
        ustore_strings_view_to_arrow(outside.view(), wrapping, ustore_strings_u64_tape_k, arena.arena, &schema, &array),
        ustore_status_invalid_argument_k);
}

/** A cursor's keys and values export in place and read back through the converters until the
 *  arena's next call. */
void test_arrow_exports_read_in_place(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.database) return;
    ustore_collection_id_t const collection = fixture.create("posters", ustore_modality_blobs_k);
    if (!fill_blobs(fixture.database, collection, 100)) return;

    ustore_blobs_cursor_t cursor = nullptr;
    ustore_outcome_t outcome {};
    ustore_blobs_cursor_init_t init {.cursor = &cursor,
                                     .database = fixture.database,
                                     .outcome = &outcome,
                                     .collection = collection};
    ustore_blobs_cursor_init(&init);
    if (!expect_success(outcome)) return;
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
    expect_success(outcome);
    expect_equal(written, 40u);

    ArrowSchema key_schema {}, value_schema {};
    ArrowDeviceArray key_array {}, value_array {};
    expect_equal(ustore_keys_view_to_arrow({keys, sizeof(ustore_key_t)}, written, arena, &key_schema, &key_array),
                 ustore_status_success_k);
    expect_equal(
        ustore_strings_view_to_arrow(values, written, ustore_strings_u32_tape_k, arena, &value_schema, &value_array),
        ustore_status_success_k);
    expect_equal(std::string_view(key_schema.format), std::string_view("w:16"));
    expect_true(key_schema.metadata != nullptr);
    expect_equal(key_array.device_type, ARROW_DEVICE_CPU);

    // A key outside the arena is refused, while a strided run is carved dense behind the outputs
    ustore_key_t const outside = util::key_from_integer(7);
    ArrowSchema refused_schema {};
    ArrowDeviceArray refused_array {};
    expect_equal(ustore_keys_view_to_arrow({&outside, sizeof(ustore_key_t)}, 1, arena, &refused_schema, &refused_array),
                 ustore_status_invalid_argument_k);
    ArrowSchema every_other_schema {};
    ArrowDeviceArray every_other_array {};
    expect_equal(ustore_keys_view_to_arrow({keys, 2 * sizeof(ustore_key_t)}, written / 2, arena, &every_other_schema,
                                           &every_other_array),
                 ustore_status_success_k);
    auto const* const carved = static_cast<ustore_key_t const*>(every_other_array.array.buffers[1]);
    for (std::size_t row = 0; row != written / 2; ++row) expect_true(cell_of(carved[row]) == cell_of(keys[2 * row]));
    every_other_array.array.release(&every_other_array.array);
    every_other_schema.release(&every_other_schema);

    // A tape asked for in the other width is converted behind the outputs
    ArrowSchema wide_schema {};
    ArrowDeviceArray wide_array {};
    expect_equal(
        ustore_strings_view_to_arrow(values, written, ustore_strings_u64_tape_k, arena, &wide_schema, &wide_array),
        ustore_status_success_k);
    expect_equal(std::string_view(wide_schema.format), std::string_view("Z"));
    ustore_strings_view_t widened {};
    ustore_size_t widened_count = 0;
    expect_equal(ustore_strings_view_from_arrow(&wide_schema, &wide_array.array, &widened, &widened_count),
                 ustore_status_success_k);
    expect_true(widened_count == written && cell_of(widened, 39) == cell_of(values, 39));

    // A wide tape narrows from its first byte, and one spanning past 32 bits is refused before its
    // bytes are looked for
    ustore_strings_view_t sliced = widened;
    sliced.data.u64_tape.offsets = widened.data.u64_tape.offsets + 1;
    ArrowSchema sliced_schema {};
    ArrowDeviceArray sliced_array {};
    expect_equal(ustore_strings_view_to_arrow(sliced, written - 1, ustore_strings_u32_tape_k, arena, &sliced_schema,
                                              &sliced_array),
                 ustore_status_success_k);
    ustore_strings_view_t narrowed {};
    ustore_size_t narrowed_count = 0;
    expect_equal(ustore_strings_view_from_arrow(&sliced_schema, &sliced_array.array, &narrowed, &narrowed_count),
                 ustore_status_success_k);
    expect_true(narrowed_count == written - 1 && cell_of(narrowed, 38) == cell_of(values, 39));
    sliced_array.array.release(&sliced_array.array);
    sliced_schema.release(&sliced_schema);
    // The far offsets are gathered as a column of their own arena, as an export reads only offsets
    // an arena holds
    ustore_collection_id_t const far_collection = fixture.create("far", ustore_modality_docs_k);
    std::array<ustore_key_t, 2> const far_keys {util::key_from_integer(1), util::key_from_integer(2)};
    std::array<std::string_view, 2> const far_documents {R"({"offset": 5368709120})", R"({"offset": 9663676416})"};
    util::strided_strings<std::string_view> const far_values {util::strided(far_documents).begin()};
    ustore_docs_write_t far_write {.database = fixture.database,
                                   .outcome = &outcome,
                                   .count = far_keys.size(),
                                   .collections = {&far_collection, 0},
                                   .keys = {far_keys.data(), sizeof(ustore_key_t)},
                                   .values = far_values.view()};
    ustore_docs_write(&far_write);
    expect_success(outcome);
    std::array<std::string_view, 1> const far_fields {"offset"};
    util::strided_strings<std::string_view> const far_fields_strings {util::strided(far_fields).begin()};
    ustore_docs_field_type_t const far_type = ustore_docs_field_type_u64_k;
    arena_t const far_arena;
    ustore_docs_column_view_t const* far_columns = nullptr;
    ustore_docs_gather_t far_gather {.database = fixture.database,
                                     .outcome = &outcome,
                                     .count = far_keys.size(),
                                     .collections = {&far_collection, 0},
                                     .keys = {far_keys.data(), sizeof(ustore_key_t)},
                                     .fields_count = far_fields.size(),
                                     .fields = far_fields_strings.view(),
                                     .columns = &far_columns,
                                     .arena = far_arena.arena,
                                     .outputs_types = &far_type};
    ustore_docs_gather(&far_gather);
    expect_success(outcome);
    ustore_strings_view_t far_view {};
    far_view.layout = ustore_strings_u64_tape_k;
    far_view.data.u64_tape.data = widened.data.u64_tape.data;
    if (far_columns) far_view.data.u64_tape.offsets = reinterpret_cast<std::uint64_t const*>(far_columns[0].scalars);
    ArrowSchema far_schema {};
    ArrowDeviceArray far_array {};
    expect_equal(
        ustore_strings_view_to_arrow(far_view, 1, ustore_strings_u32_tape_k, far_arena.arena, &far_schema, &far_array),
        ustore_status_capacity_exhausted_k);
    wide_array.array.release(&wide_array.array);
    wide_schema.release(&wide_schema);

    ustore_key_t const* const pulled_keys = keys;
    ustore_size_t const pulled = written;
    std::string const first_value = cell_of(values, 0);
    ustore_keys_view_t read_keys {};
    ustore_strings_view_t read_values {};
    ustore_size_t key_count = 0, value_count = 0;
    expect_equal(ustore_keys_view_from_arrow(&key_schema, &key_array.array, &read_keys, &key_count),
                 ustore_status_success_k);
    expect_equal(ustore_strings_view_from_arrow(&value_schema, &value_array.array, &read_values, &value_count),
                 ustore_status_success_k);
    expect_true(read_keys.data == pulled_keys && key_count == pulled && value_count == pulled);

    expect_true(cell_of(read_values, 0) == first_value);
    expect_true(cell_of(read_keys.data[39]) == cell_of(util::key_from_integer(40)));
    key_array.array.release(&key_array.array);
    expect_true(cell_of(read_values, 39) == "value-40");
    value_array.array.release(&value_array.array);
    key_schema.release(&key_schema);
    value_schema.release(&value_schema);
    ustore_arena_free(arena);
    ustore_blobs_cursor_free(cursor);
}

/** What an ingest into a collection of one modality answers on one engine. */
struct ingest_expectation_t {
    std::string_view engine;
    ustore_modality_t modality;
    util::status_code_t status;
};

/** Every engine's answer to an ingest of every modality, one row each, so an engine gaining a
 *  modality flips its own row alone. */
constexpr ingest_expectation_t ingest_expectations_k[] {
    {"ram", ustore_modality_blobs_k, util::status_code_t::success_k},
    {"ram", ustore_modality_docs_k, util::status_code_t::success_k},
    {"ram", ustore_modality_pages_k, util::status_code_t::success_k},
    {"ram", ustore_modality_strings_k, util::status_code_t::success_k},
    {"ram", ustore_modality_graph_k, util::status_code_t::success_k},
    {"rocksdb", ustore_modality_blobs_k, util::status_code_t::success_k},
    {"rocksdb", ustore_modality_docs_k, util::status_code_t::success_k},
    {"rocksdb", ustore_modality_pages_k, util::status_code_t::success_k},
    {"rocksdb", ustore_modality_strings_k, util::status_code_t::unsupported_k},
    {"rocksdb", ustore_modality_graph_k, util::status_code_t::unsupported_k},
    {"leveldb", ustore_modality_blobs_k, util::status_code_t::unsupported_k},
    {"leveldb", ustore_modality_docs_k, util::status_code_t::unsupported_k},
    {"leveldb", ustore_modality_pages_k, util::status_code_t::unsupported_k},
    {"leveldb", ustore_modality_strings_k, util::status_code_t::unsupported_k},
    {"leveldb", ustore_modality_graph_k, util::status_code_t::unsupported_k},
    {"nvme", ustore_modality_blobs_k, util::status_code_t::success_k},
    {"nvme", ustore_modality_docs_k, util::status_code_t::success_k},
    {"nvme", ustore_modality_pages_k, util::status_code_t::success_k},
    {"nvme", ustore_modality_strings_k, util::status_code_t::success_k},
    {"nvme", ustore_modality_graph_k, util::status_code_t::success_k},
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
        else append.keys = keys, append.values = util::tape_slice(rows.texts.view(), first);
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

/** Holds when @p outcome succeeded, for an outcome a call answered by value, failing a check at the
 *  caller's line otherwise. */
bool expect_succeeded(ustore_outcome_t outcome, std::source_location where = std::source_location::current()) {
    return expect_success(outcome, where);
}

/** Whether the engine ingests collections of @p modality, as @c ingest_expectations_k says. */
bool ingests(config_t const& config, ustore_modality_t modality = ustore_modality_blobs_k) noexcept {
    for (ingest_expectation_t const& row : ingest_expectations_k)
        if (row.engine == config.engine() && row.modality == modality)
            return row.status == util::status_code_t::success_k;
    return false;
}

/** The reason a case gated on an ingesting engine reports elsewhere. */
constexpr char const* needs_ingest_k = "needs an engine whose row of `ingest_expectations_k` ingests";

/** Every engine opens an ingest of every modality as its row of @c ingest_expectations_k says. */
case_result_t test_arrow_ingest_per_engine(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.database) return {};
    std::size_t rows_found = 0;
    for (ingest_expectation_t const& expectation : ingest_expectations_k) {
        if (expectation.engine != config.engine()) continue;
        ++rows_found;
        ustore_modality_t const modality = expectation.modality;
        std::string const name = fmt::format("ingested-{}", ustore_modality_name(modality));
        ustore_collection_id_t const collection = fixture.create(name.c_str(), modality);
        ingest_t ingest {fixture.database, collection, ustore_ingest_sorted_k};
        expect_status(status_of(ingest.outcome), expectation.status);
        expect_equal(ingest.ingest != nullptr, expectation.status == util::status_code_t::success_k);
        if (!ingest.ingest) continue;
        modality_rows_t const rows {modality, 2};
        expect_succeeded(ingest.append(modality, rows, 0, 2));
        expect_succeeded(ingest.finish());
        // A finished ingest takes no more rows
        expect_status(status_of(ingest.append(modality, rows, 0, 2)), util::status_code_t::invalid_argument_k);
    }
    if (!rows_found) return case_result_t::skipped("needs a row in `ingest_expectations_k`");
    return {};
}

/** An ingest of every modality, in either order and over several appends, lands what the modality's writes land. */
case_result_t test_arrow_ingest_equals_writes(config_t const& config) {
    if (!ingests(config)) return case_result_t::skipped(needs_ingest_k);
    fixture_t fixture {config};
    if (!fixture.database) return {};
    constexpr std::size_t count_k = 100;
    for (ustore_modality_t const modality : {ustore_modality_blobs_k, ustore_modality_docs_k, ustore_modality_pages_k,
                                             ustore_modality_strings_k, ustore_modality_graph_k}) {
        if (!ingests(config, modality)) continue;
        char const* const name = ustore_modality_name(modality);
        modality_rows_t const rows {modality, count_k};
        ustore_collection_id_t const written_collection = fixture.create(fmt::format("{}-written", name).c_str(),
                                                                         modality);
        if (!fill(fixture.database, written_collection, modality, rows)) return {};
        rows_t from_writes;
        dump(fixture.database, written_collection, modality, from_writes);
        expect_equal(from_writes.size(), count_k);

        for (ustore_ingest_order_t const order : {ustore_ingest_sorted_k, ustore_ingest_unsorted_k}) {
            std::string const ingested_name = fmt::format("{}-ingested-{}", name, ustore_ingest_order_name(order));
            ustore_collection_id_t const ingested = fixture.create(ingested_name.c_str(), modality);
            ingest_t ingest {fixture.database, ingested, order};
            if (!expect_succeeded(ingest.outcome)) return {};
            // Unsorted, the later half arrives first
            std::array<std::size_t, 3> firsts {0, 30, 70};
            if (order == ustore_ingest_unsorted_k) firsts = {70, 30, 0};
            for (std::size_t const first : firsts) {
                std::size_t const count = first == 70 ? 30 : first == 30 ? 40 : 30;
                expect_succeeded(ingest.append(modality, rows, first, count));
            }
            ustore_stamp_t stamp = 0;
            expect_succeeded(ingest.finish(&stamp));
            expect_true(stamp != 0);
            rows_t from_ingest;
            dump(fixture.database, ingested, modality, from_ingest);
            if (from_ingest != from_writes)
                fmt::println(stderr, "  the {} ingest differs from the writes", ingested_name);
            expect_true(from_ingest == from_writes);
        }
    }
    return {};
}

/** Under @c ustore_ingest_unsorted_k a repeated key keeps the last of its rows. */
case_result_t test_arrow_ingest_last_repeat_wins(config_t const& config) {
    if (!ingests(config)) return case_result_t::skipped(needs_ingest_k);
    fixture_t fixture {config};
    if (!fixture.database) return {};
    ustore_collection_id_t const collection = fixture.create("repeated", ustore_modality_blobs_k);
    modality_rows_t rows {ustore_modality_blobs_k, 3};
    rows.keys = {util::key_from_integer(7), util::key_from_integer(7), util::key_from_integer(7)};
    ingest_t ingest {fixture.database, collection, ustore_ingest_unsorted_k};
    expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 0, 2));
    expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 2, 1));
    expect_succeeded(ingest.finish());
    expect_true(stored_value(fixture.database, collection, util::key_from_integer(7)) ==
                std::optional<std::string> {"text-003"});
    return {};
}

/** Under @c ustore_ingest_sorted_k a key below the one before it, in its append or the one before, refuses that
 *  append alone. */
case_result_t test_arrow_ingest_refuses_a_descending_key(config_t const& config) {
    if (!ingests(config)) return case_result_t::skipped(needs_ingest_k);
    fixture_t fixture {config};
    if (!fixture.database) return {};
    ustore_collection_id_t const collection = fixture.create("descending", ustore_modality_blobs_k);
    modality_rows_t rows {ustore_modality_blobs_k, 4};
    std::reverse(rows.keys.begin() + 2, rows.keys.end());
    ingest_t ingest {fixture.database, collection, ustore_ingest_sorted_k};
    expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 0, 2));
    expect_status(status_of(ingest.append(ustore_modality_blobs_k, rows, 2, 2)),
                  util::status_code_t::invalid_argument_k);
    expect_status(status_of(ingest.append(ustore_modality_blobs_k, rows, 0, 1)),
                  util::status_code_t::invalid_argument_k);
    expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 3, 1));
    expect_succeeded(ingest.finish());
    expect_true(!stored_value(fixture.database, collection, util::key_from_integer(4)).has_value());
    expect_true(stored_value(fixture.database, collection, util::key_from_integer(3)) ==
                std::optional<std::string> {"text-004"});
    return {};
}

/** What an accessor appending to its own ingest reads: the ingest, the value it views, and the
 *  outcome of the append it makes from inside the one reading it. */
struct reentering_values_t {
    ustore_ingest_t ingest = nullptr;
    std::string_view value;
    ustore_outcome_t* nested = nullptr;
};

/** Appends nothing to the ingest of @p handle, then views its one value for every index. */
void append_from_accessor(void const* handle, ustore_size_t const*, ustore_size_t count,
                          ustore_bytes_view_t* views) noexcept {
    reentering_values_t const& values = *static_cast<reentering_values_t const*>(handle);
    ustore_ingest_append_t nested {.ingest = values.ingest, .outcome = values.nested};
    ustore_ingest_append(&nested);
    for (ustore_size_t index = 0; index != count; ++index)
        views[index] = {reinterpret_cast<ustore_byte_t const*>(values.value.data()), values.value.size()};
}

/** An ingest serves one call at a time, so an append made while another reads its values is
 *  contended, and the outer append lands as if alone. */
case_result_t test_arrow_ingest_contends_a_second_append(config_t const& config) {
    if (!ingests(config)) return case_result_t::skipped(needs_ingest_k);
    fixture_t fixture {config};
    if (!fixture.database) return {};
    ustore_collection_id_t const collection = fixture.create("reentered", ustore_modality_blobs_k);
    ingest_t ingest {fixture.database, collection, ustore_ingest_sorted_k};
    ustore_outcome_t nested {};
    reentering_values_t const values {.ingest = ingest.ingest, .value = "outer", .nested = &nested};
    ustore_key_t const key = util::key_from_integer(1);
    ustore_outcome_t appended {};
    ustore_ingest_append_t append {.ingest = ingest.ingest, .outcome = &appended, .count = 1, .keys = {&key, 0}};
    append.values.layout = ustore_strings_accessor_k;
    append.values.data.accessor = {.get = &append_from_accessor, .handle = &values};
    ustore_ingest_append(&append);
    expect_succeeded(appended);
    expect_status(status_of(nested), util::status_code_t::contended_k);
    expect_succeeded(ingest.finish());
    expect_true(stored_value(fixture.database, collection, key) == std::optional<std::string> {"outer"});
    return {};
}

/** A clear, a create, a drop, a flush or a compaction an open ingest would outlast answers
 *  contended at once, changing nothing, and serves once the ingest finished; an engine that
 *  ingests nothing never holds one open. */
case_result_t test_arrow_ingest_contends_a_clear(config_t const& config) {
    if (!ingests(config)) return case_result_t::skipped(needs_ingest_k);
    fixture_t fixture {config};
    if (!fixture.database) return {};
    ustore_collection_id_t const collection = fixture.create("cleared", ustore_modality_blobs_k);
    modality_rows_t const rows {ustore_modality_blobs_k, 2};
    fill(fixture.database, collection, ustore_modality_blobs_k, rows);
    std::optional<std::string> const before = stored_value(fixture.database, collection, util::key_from_integer(1));
    ingest_t ingest {fixture.database, collection, ustore_ingest_unsorted_k};
    expect_succeeded(ingest.outcome);
    expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 0, 2));
    for (ustore_drop_mode_t const mode : {ustore_drop_collection_k, ustore_drop_pairs_k, ustore_drop_values_k}) {
        ustore_outcome_t cleared {};
        ustore_database_clear_t clear {.database = fixture.database, .outcome = &cleared, .mode = mode};
        ustore_database_clear(&clear);
        expect_status(status_of(cleared), util::status_code_t::contended_k);
    }
    ustore_outcome_t created {};
    ustore_collection_id_t made {};
    ustore_collection_create_t create {.database = fixture.database,
                                       .outcome = &created,
                                       .name = {reinterpret_cast<uint8_t const*>("later"), 5},
                                       .modality = ustore_modality_blobs_k,
                                       .collection = &made};
    ustore_collection_create(&create);
    expect_status(status_of(created), util::status_code_t::contended_k);
    ustore_outcome_t dropped {};
    ustore_collection_drop_t drop {.database = fixture.database,
                                   .outcome = &dropped,
                                   .collection = collection,
                                   .mode = ustore_drop_pairs_k};
    ustore_collection_drop(&drop);
    expect_status(status_of(dropped), util::status_code_t::contended_k);
    ustore_outcome_t flushed {};
    ustore_database_flush_t flush {.database = fixture.database, .outcome = &flushed};
    ustore_database_flush(&flush);
    expect_status(status_of(flushed), util::status_code_t::contended_k);
    ustore_outcome_t compacted {};
    ustore_database_compact_t compact {.database = fixture.database, .outcome = &compacted};
    ustore_database_compact(&compact);
    expect_status(status_of(compacted), util::status_code_t::contended_k);
    expect_true(stored_value(fixture.database, collection, util::key_from_integer(1)) == before);
    expect_succeeded(ingest.finish());
    ustore_database_flush(&flush);
    expect_succeeded(flushed);
    ustore_outcome_t cleared {};
    ustore_database_clear_t clear {.database = fixture.database, .outcome = &cleared, .mode = ustore_drop_pairs_k};
    ustore_database_clear(&clear);
    expect_succeeded(cleared);
    return {};
}

/** A freed ingest that never finished lands nothing. */
case_result_t test_arrow_ingest_freed_lands_nothing(config_t const& config) {
    if (!ingests(config)) return case_result_t::skipped(needs_ingest_k);
    fixture_t fixture {config};
    if (!fixture.database) return {};
    ustore_collection_id_t const collection = fixture.create("abandoned", ustore_modality_blobs_k);
    modality_rows_t const rows {ustore_modality_blobs_k, 2};
    {
        ingest_t ingest {fixture.database, collection, ustore_ingest_sorted_k};
        expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 0, 2));
    }
    expect_true(!stored_value(fixture.database, collection, util::key_from_integer(1)).has_value());
    return {};
}

/** A snapshot taken before an ingest reads the value the key held before it. */
case_result_t test_arrow_ingest_spares_older_snapshots(config_t const& config) {
    if (!ingests(config)) return case_result_t::skipped(needs_ingest_k);
    fixture_t fixture {config};
    if (!fixture.database) return {};
    ustore_collection_id_t const collection = fixture.create("versioned", ustore_modality_blobs_k);
    if (!fill_blobs(fixture.database, collection, 1)) return {};
    ustore_snapshot_t snapshot = nullptr;
    ustore_outcome_t outcome {};
    ustore_snapshot_init_t init {.snapshot = &snapshot, .database = fixture.database, .outcome = &outcome};
    ustore_snapshot_init(&init);
    if (!expect_success(outcome)) return {};

    modality_rows_t const rows {ustore_modality_blobs_k, 2};
    {
        ingest_t ingest {fixture.database, collection, ustore_ingest_sorted_k};
        expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 0, 2));
        expect_succeeded(ingest.finish());
    }
    std::optional<std::string> const before = stored_value(fixture.database, collection, util::key_from_integer(1),
                                                           ustore_snapshot_view(snapshot));
    expect_true(before == std::optional<std::string> {"value-1"});
    expect_true(stored_value(fixture.database, collection, util::key_from_integer(1)) ==
                std::optional<std::string> {"text-001"});
    ustore_snapshot_free(snapshot);
    return {};
}

/** A transaction that began before an ingest and wrote a key the ingest lands fails at commit. */
case_result_t test_arrow_ingest_conflicts_older_transactions(config_t const& config) {
    if (!ingests(config)) return case_result_t::skipped(needs_ingest_k);
    fixture_t fixture {config};
    if (!fixture.database) return {};
    ustore_collection_id_t const collection = fixture.create("contested", ustore_modality_blobs_k);
    ustore_transaction_t transaction = nullptr;
    ustore_outcome_t outcome {};
    ustore_transaction_init_t init {.transaction = &transaction, .database = fixture.database, .outcome = &outcome};
    ustore_transaction_init(&init);
    if (!expect_success(outcome)) return {};
    ustore_key_t const key = util::key_from_integer(1);
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
    expect_success(outcome);

    modality_rows_t const rows {ustore_modality_blobs_k, 2};
    {
        ingest_t ingest {fixture.database, collection, ustore_ingest_sorted_k};
        expect_succeeded(ingest.append(ustore_modality_blobs_k, rows, 0, 2));
        expect_succeeded(ingest.finish());
    }
    ustore_transaction_commit_t commit {.database = fixture.database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_commit(&commit);
    expect_status(status_of(outcome), util::status_code_t::write_conflict_k);
    ustore_transaction_free(transaction);
    return {};
}

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    start_test_run(argc, argv);

    auto [scratch, scratch_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(scratch_status)) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    char const* const directory = scratch.path();
    if (directory) fmt::println("Will work in directory: {}", directory);
    else fmt::println("Will work with default configuration");
    config_t const config {test_run.settings.engine, directory};

    run_test_case(engine_label_t {"test_arrow_converters"}.c_str(), test_arrow_converters, config);
    run_test_case(engine_label_t {"test_arrow_exports_read_in_place"}.c_str(), test_arrow_exports_read_in_place,
                  config);
    run_test_case(engine_label_t {"test_arrow_docs_columns_export"}.c_str(), test_arrow_docs_columns_export, config);
    run_test_case(engine_label_t {"test_arrow_export_refuses_an_entry_without_terminator"}.c_str(),
                  test_arrow_export_refuses_an_entry_without_terminator, config);
    run_test_case(engine_label_t {"test_arrow_export_refuses_offsets_outside_the_arena"}.c_str(),
                  test_arrow_export_refuses_offsets_outside_the_arena, config);
    run_test_case(engine_label_t {"test_arrow_ingest_per_engine"}.c_str(), test_arrow_ingest_per_engine, config);
    run_test_case(engine_label_t {"test_arrow_ingest_equals_writes"}.c_str(), test_arrow_ingest_equals_writes, config);
    run_test_case(engine_label_t {"test_arrow_ingest_last_repeat_wins"}.c_str(), test_arrow_ingest_last_repeat_wins,
                  config);
    run_test_case(engine_label_t {"test_arrow_ingest_refuses_a_descending_key"}.c_str(),
                  test_arrow_ingest_refuses_a_descending_key, config);
    run_test_case(engine_label_t {"test_arrow_ingest_contends_a_second_append"}.c_str(),
                  test_arrow_ingest_contends_a_second_append, config);
    run_test_case(engine_label_t {"test_arrow_ingest_contends_a_clear"}.c_str(), test_arrow_ingest_contends_a_clear,
                  config);
    run_test_case(engine_label_t {"test_arrow_ingest_freed_lands_nothing"}.c_str(),
                  test_arrow_ingest_freed_lands_nothing, config);
    run_test_case(engine_label_t {"test_arrow_ingest_spares_older_snapshots"}.c_str(),
                  test_arrow_ingest_spares_older_snapshots, config);
    run_test_case(engine_label_t {"test_arrow_ingest_conflicts_older_transactions"}.c_str(),
                  test_arrow_ingest_conflicts_older_transactions, config);

    return finish_test_run();
}
