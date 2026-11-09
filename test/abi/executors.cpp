/**
 *  @file test/abi/executors.cpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief Checks the arena, outcome and executor contracts of the C ABI: where outputs land, how
 *      an executor serves one call at a time, and how the C++ receivers dispatch on an executor.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int32_t`, `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`
#include <cstdio>  // `stderr`
#include <cstring> // `std::memcmp`, `std::memset`

#include <algorithm>   // `std::all_of`, `std::count_if`, `std::sort`
#include <array>       // `std::array`
#include <chrono>      // `std::chrono::milliseconds`
#include <span>        // `std::span`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <thread>      // `std::this_thread::sleep_for`, `std::this_thread::yield`
#include <utility>     // `std::as_const`, `std::move`

#include <fmt/format.h>

#include <forkunion/atomics.hpp> // `fu::standard_atomic_ref`

#include "ustore/ustore.hpp"

#include "ustore/utilities/ram_buffers.hpp" // `arena_block_t`, `arena_alignment_k`
#include "ustore/utilities/ram_device.hpp"  // `caller_exclusivity_t`, `executor_t`

#include "abi/fixture.hpp" // `held_scan_t`, `submit_held`, `release_held`, `spawn_executor`, `round_trip`, `edge_of`
#include "engine.hpp"      // `ustore_database_handle_t`
#include "harness.hpp"     // `suite_exit_code`, `expect`, `expect_failure`, `expect_status`, `spin_until`
#include "utilities/slices.hpp" // `tasks_per_word_k`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::test::abi;
using namespace unum::ustore::utilities;
using namespace unum;

#pragma region Arenas

/** A call needs an arena for its outputs, so a @c NULL one is refused before any engine runs and
 *  its outputs are left @c NULL. An arena init without an output for the arena is refused too. */
std::size_t arena_null_refused(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "arena_null_refused.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());

    ustore_key_t const key = key_from_integer(200);
    failures += write_text(database, blobs, {&key, 1}, "value");
    std::uint32_t const sentinel = 0;
    read_columns_t columns {reinterpret_cast<ustore_octet_t const*>(&sentinel), u32_tape(&sentinel, &sentinel)};
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, nullptr, outcome);
    ustore_blobs_read(&read);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);
    failures += expect(!columns.presences && !tape_written(columns.values));

    ustore_arena_init_t init {.outcome = &outcome};
    ustore_arena_init(&init);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);
    return failures;
}

/** One arena serves a read and then a scan, each call rewinding it and writing whole outputs. */
std::size_t arena_reuse(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "arena_reuse.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;

    // Three present keys and a missing one past them, which no other case writes between
    std::array<ustore_key_t, 4> const keys {key_from_integer(300), key_from_integer(301), key_from_integer(302),
                                            key_from_integer(303)};
    failures += write_text(database, blobs, std::span {keys}.first(3), "value");
    read_columns_t columns;
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences && tape_written(columns.values));
    if (failures != 0) return failures;
    failures += expect_eq(columns.presences[0], 0b0111);
    failures += expect_eq(columns.value(2), "value");
    failures += expect(columns.value(3).empty());

    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* found = nullptr;
    ustore_blobs_scan_t scan {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .collections = {&blobs, 0},
        .start_keys = {&keys[0], 0},
        .offsets = &offsets,
        .keys = &found,
        .arena = arena,
        .wanted = 3,
    };
    ustore_blobs_scan(&scan);
    failures += expect_success(outcome);
    failures += expect(offsets && found && offsets[0] == 0 && offsets[1] == 3);
    if (found) failures += expect(std::memcmp(found, keys.data(), 3 * sizeof(ustore_key_t)) == 0);
    return failures;
}

/** A call that fails leaves every output @c NULL, blocking or deferred, whatever the
 *  outputs held before. */
std::size_t arena_failed_outputs_null(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "arena_failed_outputs_null.blobs",
                                                   ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    auto [executor, spawn_status] = spawn_executor();
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());

    // No collection carries this identifier, so every engine fails the read
    ustore_key_t const key = key_from_integer(500);
    ustore_collection_id_t const missing {uuid_from_integer(0x4242)};
    std::uint32_t const sentinel = 0;
    read_columns_t const stale {reinterpret_cast<ustore_octet_t const*>(&sentinel), u32_tape(&sentinel, &sentinel)};
    for (ustore_completion_t const completion :
         std::array {ustore_completion_blocking_k, ustore_completion_deferred_k}) {
        read_columns_t columns = stale;
        ustore_outcome_t outcome {};
        ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
        read.executor = executor;
        read.completion = completion;
        read.collections = {&missing, 0};
        ustore_blobs_read(&read);
        ustore_status_t const status = ustore_outcome_wait(&outcome);
        failures += expect(status != ustore_status_success_k);
        failures += expect(!columns.presences && !tape_written(columns.values));
    }

    // A scan without an output for its keys is refused, and its offsets are left null too
    ustore_length_t const* offsets = reinterpret_cast<ustore_length_t const*>(&sentinel);
    ustore_outcome_t outcome {};
    ustore_blobs_scan_t scan {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .collections = {&blobs, 0},
        .offsets = &offsets,
        .arena = arena,
        .wanted = 1,
    };
    ustore_blobs_scan(&scan);
    failures += expect_failure(outcome);
    failures += expect(!offsets);
    ustore_executor_free(executor);
    return failures;
}

/** Whether @p output starts at a multiple of 64 bytes, where every output of an arena starts. */
bool aligned(void const* output) noexcept { return reinterpret_cast<std::uintptr_t>(output) % 64 == 0; }

/** Every output starts aligned to 64 bytes, as Apache Arrow buffers do, so typed columns read in
 *  place: the columns of a gather, of a read and the bounds of a measurement. */
std::size_t arena_aligned_outputs(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "arena_aligned_outputs.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;

    // Three documents, the last without a height or a name, so every bitmap of a column differs
    std::array<ustore_key_t, 3> const keys {key_from_integer(800), key_from_integer(801), key_from_integer(802)};
    std::array<std::string_view, 3> const documents {R"({"born":1815,"height":1.5,"name":"ada"})",
                                                     R"({"born":1906,"height":1.6,"name":"grace"})",
                                                     R"({"born":1918})"};
    strided_strings<std::string_view> const values {strided(documents).begin()};
    auto [docs, docs_status] = create_collection(database, "aligned.docs", ustore_modality_docs_k);
    failures += expect(docs_status.succeeded());
    if (failures != 0) return failures;
    ustore_outcome_t outcome {};
    ustore_docs_write_t write {
        .database = database,
        .outcome = &outcome,
        .count = keys.size(),
        .collections = {&docs, 0},
        .keys = {keys.data(), sizeof(ustore_key_t)},
        .values = values.view(),
    };
    ustore_docs_write(&write);
    failures += expect_success(outcome);

    std::array<std::string_view, 3> const fields {"born", "height", "name"};
    strided_strings<std::string_view> const fields_strings {strided(fields).begin()};
    std::array<ustore_docs_field_type_t, 3> const types {ustore_docs_field_type_u64_k, ustore_docs_field_type_f64_k,
                                                         ustore_docs_field_type_string_k};
    ustore_docs_column_view_t const* columns = nullptr;
    ustore_docs_gather_t gather {
        .database = database,
        .outcome = &outcome,
        .count = keys.size(),
        .collections = {&docs, 0},
        .keys = {keys.data(), sizeof(ustore_key_t)},
        .fields_count = fields.size(),
        .fields = fields_strings.view(),
        .columns = &columns,
        .arena = arena,
        .outputs_types = types.data(),
    };
    ustore_docs_gather(&gather);
    failures += expect_success(outcome);
    failures += expect(columns != nullptr);
    if (failures != 0) return failures;
    for (std::size_t field = 0; field != fields.size(); ++field) {
        ustore_docs_column_view_t const& column = columns[field];
        failures += expect(aligned(column.validities) && aligned(column.conversions) && aligned(column.collisions));
        failures += expect(aligned(column.scalars));
    }
    failures += expect(aligned(columns[2].strings.data.u32_tape.offsets) &&
                       aligned(columns[2].strings.data.u32_tape.data));
    auto const* const born = reinterpret_cast<std::uint64_t const*>(columns[0].scalars);
    auto const* const heights = reinterpret_cast<double const*>(columns[1].scalars);
    failures += expect_eq(born[2], 1918u);
    failures += expect_eq(heights[1], 1.6);
    failures += expect_eq(columns[1].validities[0], 0b011);
    failures += expect_eq(tape_string(columns[2].strings, 0).length, 4u);

    read_columns_t read_columns;
    ustore_blobs_read_t read = read_of(database, blobs, keys, read_columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(read_columns.presences && tape_written(read_columns.values));
    failures += expect(aligned(read_columns.presences) && aligned(read_columns.values.data.u32_tape.offsets) &&
                       aligned(read_columns.values.data.u32_tape.data));

    ustore_key_t const lowest = key_from_integer(0);
    ustore_key_t const highest = key_from_integer(1000);
    std::array<ustore_size_t const*, 6> bounds {};
    ustore_blobs_measure_t measure {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .collections = {&blobs, 0},
        .start_keys = {&lowest, 0},
        .end_keys = {&highest, 0},
        .min_cardinalities = &bounds[0],
        .max_cardinalities = &bounds[1],
        .min_value_bytes = &bounds[2],
        .max_value_bytes = &bounds[3],
        .min_space_usages = &bounds[4],
        .max_space_usages = &bounds[5],
        .arena = arena,
    };
    ustore_blobs_measure(&measure);
    failures += expect_success(outcome);
    for (ustore_size_t const* bound : bounds) failures += expect(bound && aligned(bound));
    return failures;
}

/** Where one output of a call starts in its arena and how many bytes it takes. */
struct output_extent_t {
    void const* begin = nullptr;
    std::size_t bytes = 0;
};

/** The offsets and then the bytes of an output @p tape of @p count strings, as two outputs
 *  of its call. */
std::array<output_extent_t, 2> tape_extents(ustore_strings_view_t const& tape, std::size_t count) noexcept {
    if (tape.layout == ustore_strings_u64_tape_k)
        return {{{tape.data.u64_tape.offsets, (count + 1) * sizeof(std::uint64_t)},
                 {tape.data.u64_tape.data, tape.data.u64_tape.offsets[count]}}};
    return {{{tape.data.u32_tape.offsets, (count + 1) * sizeof(std::uint32_t)},
             {tape.data.u32_tape.data, tape.data.u32_tape.offsets[count]}}};
}

/** Checks that the non-empty @p outputs tile every block of @p arena from its header to the last
 *  byte it handed out, each where alignment puts the allocation after the one before, so the arena
 *  holds nothing else: no growth slack, and no partial copy of an output that outgrew a block. */
std::size_t expect_only_outputs(ustore_arena_t arena, std::span<output_extent_t> outputs) {
    std::size_t failures = 0;
    auto const address = [](void const* pointer) noexcept { return reinterpret_cast<std::uintptr_t>(pointer); };
    std::sort(outputs.begin(), outputs.end(), [&](output_extent_t const& left, output_extent_t const& right) noexcept {
        return address(left.begin) < address(right.begin);
    });
    std::size_t tiled = 0;
    for (arena_block_t const* block = host_blocks_of(arena)->newest; block; block = block->previous) {
        std::uintptr_t const block_begin = address(block);
        std::uintptr_t end = block_begin + sizeof(arena_block_t);
        for (output_extent_t const& output : outputs) {
            if (output.bytes == 0 || address(output.begin) < block_begin ||
                address(output.begin) >= block_begin + block->capacity)
                continue;
            failures += expect_eq(address(output.begin), round_up_to_multiple<std::uintptr_t>(end, arena_alignment_k));
            end = address(output.begin) + output.bytes;
            ++tiled;
        }
        failures += expect_eq(end, block_begin + block->used);
    }
    failures += expect_eq(tiled, static_cast<std::size_t>(std::count_if(
                                     outputs.begin(), outputs.end(),
                                     [](output_extent_t const& output) noexcept { return output.bytes != 0; })));
    return failures;
}

/** Checks that the @p outputs of a one-off @p arena's first call outgrew its first block, which
 *  spans one page, into a second, and still tile both. */
std::size_t expect_outgrown_outputs(ustore_arena_t arena, std::span<output_extent_t> outputs) {
    std::size_t failures = 0;
    arena_block_t const* const newest = host_blocks_of(arena)->newest;
    failures += expect(newest && newest->previous && !newest->previous->previous);
    if (failures != 0) return failures;
    failures += expect_eq(newest->previous->capacity, arena_page_bytes_k);
    return failures + expect_only_outputs(arena, outputs);
}

/** Every call stages what grows in scratch and copies its outputs in at their final lengths, so the
 *  arena it fills holds exactly its outputs, at the head and through a transaction alike: a docs
 *  gather, a graph read, a blob read, a docs read by field and a gist in an arena kept across them,
 *  then a collection list, a key scan, a strings scan, a gather of long strings and a read of many
 *  edges, each in a one-off arena its outputs outgrow. */
std::size_t arena_outputs_only(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "arena_outputs_only.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    auto [graph, graph_status] = create_collection(database, "outputs.graph", ustore_modality_graph_k);
    failures += expect(graph_status.succeeded());
    if (failures != 0) return failures;

    // Every document carries a long biography no output holds, which any intermediate left in the
    // arena would show
    constexpr std::size_t docs_count = 4;
    std::array<ustore_key_t, docs_count> const keys {key_from_integer(1100), key_from_integer(1101),
                                                     key_from_integer(1102), key_from_integer(1103)};
    std::array<std::string, docs_count> const documents {
        fmt::format(R"({{"born":1815,"name":"ada","bio":"{:x>600}"}})", ""),
        fmt::format(R"({{"born":1906,"name":"grace","bio":"{:x>600}"}})", ""),
        fmt::format(R"({{"born":1918,"bio":"{:x>600}"}})", ""),
        fmt::format(R"({{"name":"edsger","bio":"{:x>600}"}})", ""),
    };
    strided_strings<std::string> const values {strided(documents).begin()};
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database;
    write.outcome = &outcome;
    write.count = docs_count;
    write.collections = {&blobs, 0};
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values = values.view();
    ustore_blobs_write(&write);
    failures += expect_success(outcome);
    auto [docs, docs_status] = create_collection(database, "outputs.docs", ustore_modality_docs_k);
    failures += expect(docs_status.succeeded());
    ustore_docs_write_t write_documents {
        .database = database,
        .outcome = &outcome,
        .count = docs_count,
        .collections = {&docs, 0},
        .keys = {keys.data(), sizeof(ustore_key_t)},
        .values = values.view(),
    };
    ustore_docs_write(&write_documents);
    failures += expect_success(outcome);

    // Three edges between three vertices, and a fourth vertex no edge reaches, which
    // reads as missing
    std::array<ustore_key_t, 3> const sources {key_from_integer(1300), key_from_integer(1300), key_from_integer(1301)};
    std::array<ustore_key_t, 3> const targets {key_from_integer(1301), key_from_integer(1302), key_from_integer(1302)};
    ustore_graph_write_edges_t write_edges {
        .database = database,
        .outcome = &outcome,
        .count = sources.size(),
        .collections = {&graph, 0},
        .sources = {sources.data(), sizeof(ustore_key_t)},
        .targets = {targets.data(), sizeof(ustore_key_t)},
    };
    ustore_graph_write_edges(&write_edges);
    failures += expect_success(outcome);
    std::array<ustore_key_t, 4> const vertices {key_from_integer(1300), key_from_integer(1301), key_from_integer(1302),
                                                key_from_integer(1399)};

    // A hub of a hundred edges, three hundred keys and strings, and a long name, each
    // outgrowing one page
    constexpr std::size_t fanned_count = 100;
    constexpr std::size_t many_count = 300;
    ustore_key_t const hub = key_from_integer(1400);
    std::array<ustore_key_t, fanned_count> fanned {};
    for (std::size_t index = 0; index != fanned_count; ++index) fanned[index] = key_from_integer(1401 + index);
    ustore_graph_write_edges_t write_fan {
        .database = database,
        .outcome = &outcome,
        .count = fanned_count,
        .collections = {&graph, 0},
        .sources = {&hub, 0},
        .targets = {fanned.data(), sizeof(ustore_key_t)},
    };
    ustore_graph_write_edges(&write_fan);
    failures += expect_success(outcome);
    std::array<ustore_key_t, many_count> scanned {};
    for (std::size_t index = 0; index != many_count; ++index) scanned[index] = key_from_integer(5000 + index);
    failures += write_text(database, blobs, scanned, "value");
    auto [strings, strings_status] = create_collection(database, "outputs.strings", ustore_modality_strings_k);
    failures += expect(strings_status.succeeded());
    std::array<std::string, many_count> texts {};
    for (std::size_t index = 0; index != many_count; ++index) texts[index] = fmt::format("outputs.string.{:03}", index);
    strided_strings<std::string> const text_strings {strided(std::as_const(texts)).begin()};
    ustore_strings_write_t write_strings {
        .database = database,
        .outcome = &outcome,
        .count = many_count,
        .collections = {&strings, 0},
        .strings = text_strings.view(),
        .values = {scanned.data(), sizeof(ustore_key_t)},
    };
    ustore_strings_write(&write_strings);
    failures += expect_success(outcome);
    std::string const long_name(5000, 'n');
    failures += expect(create_collection(database, long_name, ustore_modality_blobs_k).has_value());
    if (failures != 0) return failures;

    // The listing exports every collection, earlier cases' included, the long name outgrowing
    // the first page
    {
        auto [one_off, one_off_status] = arena_t::make();
        failures += expect(one_off_status.succeeded());
        ustore_size_t written = 0;
        ustore_collection_id_t const* ids = nullptr;
        ustore_modality_t const* modalities = nullptr;
        ustore_strings_view_t names {};
        ustore_collection_list_t list {
            .database = database,
            .outcome = &outcome,
            .collections = &ids,
            .modalities = &modalities,
            .names = &names,
            .written = &written,
            .arena = one_off,
        };
        ustore_collection_list(&list);
        failures += expect_success(outcome);
        failures += expect(written >= 3 && ids && modalities && tape_written(names));
        if (failures != 0) return failures;
        std::array<output_extent_t, 2> const names_tape = tape_extents(names, written);
        std::array<output_extent_t, 4> listed {{
            {ids, written * sizeof(ustore_collection_id_t)},
            {modalities, written * sizeof(ustore_modality_t)},
            names_tape[0],
            names_tape[1],
        }};
        failures += expect_outgrown_outputs(one_off, listed);
    }

    // An engine without transactions reads at the head alone
    ustore_transaction_t transaction = nullptr;
    ustore_transaction_init_t begin {.transaction = &transaction, .database = database, .outcome = &outcome};
    ustore_transaction_init(&begin);
    outcome = {};
    std::array<ustore_view_t, 2> const views {nullptr, ustore_transaction_view(transaction, ustore_watch_keys_k)};
    for (ustore_view_t const view : std::span {views}.first(transaction ? 2 : 1)) {
        std::array<std::string_view, 2> const fields {"born", "name"};
        strided_strings<std::string_view> const fields_strings {strided(fields).begin()};
        std::array<ustore_docs_field_type_t, 2> const types {ustore_docs_field_type_u64_k,
                                                             ustore_docs_field_type_string_k};
        ustore_docs_column_view_t const* columns = nullptr;
        ustore_docs_gather_t gather {
            .database = database,
            .outcome = &outcome,
            .view = view,
            .count = docs_count,
            .collections = {&docs, 0},
            .keys = {keys.data(), sizeof(ustore_key_t)},
            .fields_count = fields.size(),
            .fields = fields_strings.view(),
            .columns = &columns,
            .arena = arena,
            .outputs_types = types.data(),
        };
        ustore_docs_gather(&gather);
        failures += expect_success(outcome);
        failures += expect(columns != nullptr);
        if (!columns) continue;
        ustore_docs_column_view_t const& born = columns[0];
        ustore_docs_column_view_t const& name = columns[1];
        auto const* const years = reinterpret_cast<std::uint64_t const*>(born.scalars);
        failures += expect_eq(years[2], 1918u);
        failures += expect_eq(std::string_view {reinterpret_cast<char const*>(tape_string(name.strings, 3).data)},
                              "edsger");
        std::size_t const bitmap_bytes = divide_round_up(docs_count, bits_in_byte_k);
        std::array<output_extent_t, 2> const name_tape = tape_extents(name.strings, docs_count);
        std::array<output_extent_t, 10> gathered {{
            {columns, fields.size() * sizeof(ustore_docs_column_view_t)},
            {born.validities, bitmap_bytes},
            {born.conversions, bitmap_bytes},
            {born.collisions, bitmap_bytes},
            {born.scalars, docs_count * sizeof(std::uint64_t)},
            {name.validities, bitmap_bytes},
            {name.conversions, bitmap_bytes},
            {name.collisions, bitmap_bytes},
            name_tape[0],
            name_tape[1],
        }};
        failures += expect_only_outputs(arena, gathered);

        // The missing vertex spans no edges, so the offsets close at six
        ustore_vertex_degree_t const* degrees = nullptr;
        ustore_length_t const* edges_offsets = nullptr;
        ustore_key_t const* edges = nullptr;
        ustore_graph_read_edges_t read_edges {
            .database = database,
            .outcome = &outcome,
            .view = view,
            .count = vertices.size(),
            .collections = {&graph, 0},
            .vertices = {vertices.data(), sizeof(ustore_key_t)},
            .degrees_per_vertex = &degrees,
            .edges_offsets = &edges_offsets,
            .edges_per_vertex = &edges,
            .arena = arena,
        };
        ustore_graph_read_edges(&read_edges);
        failures += expect_success(outcome);
        failures += expect(degrees && edges_offsets && edges);
        if (!degrees || !edges_offsets || !edges) continue;
        failures += expect(degrees[0] == 2 && degrees[1] == 2 && degrees[2] == 2);
        failures += expect_eq(degrees[3], ustore_vertex_degree_missing_k);
        failures += expect(edges_offsets[0] == 0 && edges_offsets[1] == 2 && edges_offsets[2] == 4);
        failures += expect(edges_offsets[3] == 6 && edges_offsets[4] == 6);
        std::array<output_extent_t, 3> read {{
            {degrees, vertices.size() * sizeof(ustore_vertex_degree_t)},
            {edges_offsets, (vertices.size() + 1) * sizeof(ustore_length_t)},
            {edges, 6 * 3 * sizeof(ustore_key_t)},
        }};
        failures += expect_only_outputs(arena, read);

        // A blob read copies every document once, however long, and leaves no room for
        // more behind them
        read_columns_t blob_reads;
        ustore_blobs_read_t read_blobs = read_of(database, blobs, keys, blob_reads, arena, outcome);
        read_blobs.view = view;
        ustore_blobs_read(&read_blobs);
        failures += expect_success(outcome);
        failures += expect(blob_reads.presences && tape_written(blob_reads.values));
        if (!blob_reads.presences || !tape_written(blob_reads.values)) continue;
        failures += expect_eq(blob_reads.value(3), documents[3]);
        std::array<output_extent_t, 2> const blobs_tape = tape_extents(blob_reads.values, docs_count);
        std::array<output_extent_t, 3> blob_columns {{
            {blob_reads.presences, bitmap_bytes},
            blobs_tape[0],
            blobs_tape[1],
        }};
        failures += expect_only_outputs(arena, blob_columns);

        // A read by field exports the names alone, the third document missing one
        read_columns_t names;
        std::string_view const name_field = "name";
        strided_strings<std::string_view> const name_fields {{&name_field, 0}};
        ustore_docs_read_t read_names {
            .database = database,
            .outcome = &outcome,
            .view = view,
            .count = docs_count,
            .collections = {&docs, 0},
            .keys = {keys.data(), sizeof(ustore_key_t)},
            .fields = name_fields.view(),
            .presences = &names.presences,
            .values = &names.values,
            .arena = arena,
            .outputs_type = ustore_docs_field_type_string_k,
        };
        ustore_docs_read(&read_names);
        failures += expect_success(outcome);
        failures += expect(names.presences && tape_written(names.values));
        if (!names.presences || !tape_written(names.values)) continue;
        failures += expect_eq(names.presences[0], 0b1011);
        failures += expect_eq(names.value(1), "grace");
        failures += expect_eq(names.value(3), "edsger");
        std::array<output_extent_t, 2> const names_tape = tape_extents(names.values, docs_count);
        failures += expect_eq(names_tape[1].bytes, std::string_view {"adagraceedsger"}.size());
        std::array<output_extent_t, 3> name_columns {{
            {names.presences, bitmap_bytes},
            names_tape[0],
            names_tape[1],
        }};
        failures += expect_only_outputs(arena, name_columns);

        // A gist finds three paths, which leave no partial copies of their offsets or
        // of themselves behind
        ustore_size_t written = 0;
        ustore_strings_view_t paths {};
        ustore_docs_gist_t gist {
            .database = database,
            .outcome = &outcome,
            .view = view,
            .count = docs_count,
            .collections = {&docs, 0},
            .keys = {keys.data(), sizeof(ustore_key_t)},
            .fields = &paths,
            .written = &written,
            .arena = arena,
        };
        ustore_docs_gist(&gist);
        failures += expect_success(outcome);
        failures += expect_eq(written, 3u);
        failures += expect(tape_written(paths));
        if (!tape_written(paths)) continue;
        failures += expect_eq(string_of(tape_string(paths, 1)), "/name");
        std::array<output_extent_t, 2> gist_columns = tape_extents(paths, written);
        failures += expect_only_outputs(arena, gist_columns);

        // Three hundred keys outgrow the first page of a one-off arena, which only the offsets fit
        {
            auto [one_off, one_off_status] = arena_t::make();
            failures += expect(one_off_status.succeeded());
            ustore_length_t const* offsets = nullptr;
            ustore_key_t const* found = nullptr;
            ustore_blobs_scan_t scan {
                .database = database,
                .outcome = &outcome,
                .view = view,
                .count = 1,
                .collections = {&blobs, 0},
                .start_keys = {&scanned[0], 0},
                .offsets = &offsets,
                .keys = &found,
                .arena = one_off,
                .wanted = many_count,
            };
            ustore_blobs_scan(&scan);
            failures += expect_success(outcome);
            failures += expect(offsets && found);
            if (!offsets || !found) continue;
            failures += expect_eq(offsets[1], many_count);
            failures += expect(std::memcmp(found, scanned.data(), sizeof(scanned)) == 0);
            std::array<output_extent_t, 2> scan_columns {{
                {offsets, 2 * sizeof(ustore_length_t)},
                {found, many_count * sizeof(ustore_key_t)},
            }};
            failures += expect_outgrown_outputs(one_off, scan_columns);
        }

        // Three hundred strings outgrow it too, after their offsets
        {
            auto [one_off, one_off_status] = arena_t::make();
            failures += expect(one_off_status.succeeded());
            ustore_length_t const* offsets = nullptr;
            ustore_strings_view_t found {};
            ustore_strings_scan_t scan {
                .database = database,
                .outcome = &outcome,
                .view = view,
                .count = 1,
                .collections = {&strings, 0},
                .offsets = &offsets,
                .strings = &found,
                .arena = one_off,
                .wanted = many_count,
            };
            ustore_strings_scan(&scan);
            failures += expect_success(outcome);
            failures += expect(offsets && tape_written(found));
            if (!offsets || !tape_written(found)) continue;
            failures += expect_eq(offsets[1], many_count);
            failures += expect_eq(string_of(tape_string(found, many_count - 1)), texts[many_count - 1]);
            std::array<output_extent_t, 2> const found_tape = tape_extents(found, many_count);
            std::array<output_extent_t, 3> scan_columns {{
                {offsets, 2 * sizeof(ustore_length_t)},
                found_tape[0],
                found_tape[1],
            }};
            failures += expect_outgrown_outputs(one_off, scan_columns);
        }

        // Gathering the biographies twice, the second string column outgrows it
        {
            auto [one_off, one_off_status] = arena_t::make();
            failures += expect(one_off_status.succeeded());
            std::string_view const biography = "bio";
            strided_strings<std::string_view> const biographies {{&biography, 0}};
            std::array<ustore_docs_field_type_t, 2> const text_types {ustore_docs_field_type_string_k,
                                                                      ustore_docs_field_type_string_k};
            ustore_docs_column_view_t const* columns = nullptr;
            ustore_docs_gather_t gather {
                .database = database,
                .outcome = &outcome,
                .view = view,
                .count = docs_count,
                .collections = {&docs, 0},
                .keys = {keys.data(), sizeof(ustore_key_t)},
                .fields_count = text_types.size(),
                .fields = biographies.view(),
                .columns = &columns,
                .arena = one_off,
                .outputs_types = text_types.data(),
            };
            ustore_docs_gather(&gather);
            failures += expect_success(outcome);
            failures += expect(columns != nullptr);
            if (!columns) continue;
            std::size_t const bitmap_bytes = divide_round_up(docs_count, bits_in_byte_k);
            std::array<output_extent_t, 11> gathered {};
            gathered[0] = {columns, text_types.size() * sizeof(ustore_docs_column_view_t)};
            for (std::size_t field = 0; field != text_types.size(); ++field) {
                ustore_docs_column_view_t const& column = columns[field];
                std::array<output_extent_t, 2> const column_tape = tape_extents(column.strings, docs_count);
                failures += expect_eq(column_tape[1].bytes, docs_count * 601);
                gathered[1 + 5 * field] = {column.validities, bitmap_bytes};
                gathered[2 + 5 * field] = {column.conversions, bitmap_bytes};
                gathered[3 + 5 * field] = {column.collisions, bitmap_bytes};
                gathered[4 + 5 * field] = column_tape[0];
                gathered[5 + 5 * field] = column_tape[1];
            }
            failures += expect_outgrown_outputs(one_off, gathered);
        }

        // A hundred edges of the hub outgrow it after the degree and the offsets
        {
            auto [one_off, one_off_status] = arena_t::make();
            failures += expect(one_off_status.succeeded());
            ustore_vertex_degree_t const* degrees = nullptr;
            ustore_length_t const* edges_offsets = nullptr;
            ustore_key_t const* edges = nullptr;
            ustore_graph_read_edges_t read_fan {
                .database = database,
                .outcome = &outcome,
                .view = view,
                .count = 1,
                .collections = {&graph, 0},
                .vertices = {&hub, 0},
                .degrees_per_vertex = &degrees,
                .edges_offsets = &edges_offsets,
                .edges_per_vertex = &edges,
                .arena = one_off,
            };
            ustore_graph_read_edges(&read_fan);
            failures += expect_success(outcome);
            failures += expect(degrees && edges_offsets && edges);
            if (!degrees || !edges_offsets || !edges) continue;
            failures += expect_eq(degrees[0], fanned_count);
            std::array<output_extent_t, 3> fan_columns {{
                {degrees, sizeof(ustore_vertex_degree_t)},
                {edges_offsets, 2 * sizeof(ustore_length_t)},
                {edges, fanned_count * 3 * sizeof(ustore_key_t)},
            }};
            failures += expect_outgrown_outputs(one_off, fan_columns);
        }
    }
    ustore_transaction_free(transaction);
    return failures;
}

#pragma endregion Arenas

#pragma region Deferred Calls

/** A deferred write and then a deferred read into an arena the executor made round-trip the values,
 *  and a blocking read into an arena made for the calling thread reads them back as well. */
std::size_t executor_deferred_round_trip(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "executor_deferred_round_trip.blobs",
                                                   ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    auto [executor, spawn_status] = spawn_executor();
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;
    auto [executor_arena, executor_arena_status] = arena_t::make(executor);
    auto [calling_arena, calling_arena_status] = arena_t::make();
    failures += expect(executor_arena_status.succeeded() && calling_arena_status.succeeded());

    std::array<ustore_key_t, 3> const keys {key_from_integer(71), key_from_integer(72), key_from_integer(73)};
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database;
    write.executor = executor;
    write.outcome = &outcome;
    write.completion = ustore_completion_deferred_k;
    write.count = keys.size();
    write.collections = {&blobs, 0};
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values = stored_values_k.view();
    ustore_blobs_write(&write);
    failures += expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);

    read_columns_t deferred_columns;
    ustore_blobs_read_t deferred_read = read_of(database, blobs, keys, deferred_columns, executor_arena, outcome);
    deferred_read.executor = executor;
    deferred_read.completion = ustore_completion_deferred_k;
    ustore_blobs_read(&deferred_read);
    failures += expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    read_columns_t blocking_columns;
    ustore_blobs_read_t blocking_read = read_of(database, blobs, keys, blocking_columns, calling_arena, outcome);
    ustore_blobs_read(&blocking_read);
    failures += expect_success(outcome);

    for (read_columns_t const& columns : {deferred_columns, blocking_columns}) {
        failures += expect(columns.presences && tape_written(columns.values));
        if (!columns.presences || !tape_written(columns.values)) continue;
        failures += expect_eq(columns.presences[0], 0b111);
        for (std::size_t index = 0; index != keys.size(); ++index) failures += expect_eq(columns.value(index), "value");
    }
    ustore_executor_free(executor);
    return failures;
}

/** A deferred call runs from the copy its submission took, so the caller may scribble over its
 *  argument struct at once and the library never writes it back, while polling through
 *  @c ustore_outcome_status reads the call deferred until it is final. */
std::size_t executor_copied_submission(config_t const&) {
    std::size_t failures = 0;
    auto [executor, spawn_status] = spawn_executor();
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;

    // The scan holds inside its callback, so it is still running when its struct is scribbled over
    held_scan_t held;
    failures += submit_held(held, executor);
    std::memset(&held.scan, 0xA5, sizeof(held.scan));
    failures += expect(ustore_outcome_status(&held.outcome) == ustore_status_deferred_k);
    failures += release_held(held);
    auto const* const scribbled = reinterpret_cast<unsigned char const*>(&held.scan);
    failures += expect(
        std::all_of(scribbled, scribbled + sizeof(held.scan), [](unsigned char byte) { return byte == 0xA5; }));
    ustore_executor_free(executor);
    return failures;
}

/** Writes @c stored_value_k under @p key in @p collection as a blocking call on @p executor,
 *  answering the call's status. */
ustore_status_t write_blocking(ustore_database_t database, ustore_collection_id_t const& collection,
                               ustore_key_t const& key, ustore_executor_t executor) noexcept {
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database;
    write.executor = executor;
    write.outcome = &outcome;
    write.count = 1;
    write.collections = {&collection, 0};
    write.keys = {&key, 0};
    write.values = stored_values_k.view();
    ustore_blobs_write(&write);
    return outcome.status;
}

/** What a thread freeing an arena saw of the held call naming it once the free returned. */
struct arena_freeing_t {
    arena_t arena;
    hold_stage_t const* stage = nullptr;
    hold_stage_t stage_after_free = hold_stage_t::submitted_k;
    std::uint32_t freed = 0;
};

/** Frees the arena of @p freeing, then records how far the call naming it got. */
void free_arena(arena_freeing_t& freeing) noexcept {
    { arena_t const freed = std::move(freeing.arena); }
    freeing.stage_after_free = fu::standard_atomic_ref<hold_stage_t const> {*freeing.stage}.load(
        std::memory_order_acquire);
    fu::standard_atomic_ref<std::uint32_t> {freeing.freed}.store(1, std::memory_order_release);
}

/** Freeing an arena waits until the call naming it ends, here a scan held inside its callback,
 *  which has returned by then. Its status may turn final only after the free, as a call lowers its
 *  counts before it stores the status. */
std::size_t arena_free_waits(config_t const&) {
    std::size_t failures = 0;
    auto [executor, spawn_status] = spawn_executor();
    st::expected<executor_t> freeing_thread = executor_t::make(1, caller_exclusivity_t::caller_exclusive_k);
    failures += expect(spawn_status.succeeded() && freeing_thread.has_value());
    if (failures != 0) return failures;

    held_scan_t held;
    failures += submit_held(held, executor);
    arena_freeing_t freeing {.arena = std::move(held.arena), .stage = &held.stage};
    st::expected<fu_generation_t> const generation = freeing_thread->start<&free_arena>(freeing);
    failures += expect(generation.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    failures += expect(fu::standard_atomic_ref<std::uint32_t> {freeing.freed}.load(std::memory_order_acquire) == 0);

    fu::standard_atomic_ref<hold_stage_t> {held.stage}.store(hold_stage_t::released_k, std::memory_order_release);
    if (generation) freeing_thread->join(*generation);
    else free_arena(freeing);
    failures += expect(freeing.stage_after_free == hold_stage_t::returned_k);
    failures += expect(ustore_outcome_wait(&held.outcome) == ustore_status_success_k);
    ustore_executor_free(executor);
    return failures;
}

/** A call on an executor another call holds finishes contended at once, deferred or blocking, and a
 *  drain from inside a callback of the holding call is refused rather than waiting. */
std::size_t executor_held_contended(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "executor_held_contended.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    auto [executor, spawn_status] = spawn_executor();
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;

    held_scan_t held;
    failures += submit_held(held, executor);
    ustore_key_t const key = key_from_integer(74);
    ustore_outcome_t outcome {};
    submit_writes(database, blobs, executor, {&key, 1}, {&outcome, 1});
    failures += expect_status(outcome, status_code_t::contended_k);
    failures += expect(write_blocking(database, blobs, key, executor) == ustore_status_contended_k);
    failures += expect(held.drained == ustore_status_contended_k);
    failures += release_held(held);
    failures += expect(write_blocking(database, blobs, key, executor) == ustore_status_success_k);
    ustore_executor_free(executor);
    return failures;
}

/** Runs the blocking scan @p held is armed with on the calling thread, which its callback holds. */
void scan_blocking(held_scan_t& held) noexcept { ustore_blobs_scan(&held.scan); }

/** A blocking call holds its executor from its caller's thread, so a call from another thread
 *  finishes contended until the holder returns, after which the executor serves it. */
std::size_t executor_contended_from_another_thread(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "executor_contended_from_another_thread.blobs",
                                                   ustore_modality_blobs_k);
    auto [executor, spawn_status] = spawn_executor();
    st::expected<executor_t> holding_thread = executor_t::make(1, caller_exclusivity_t::caller_exclusive_k);
    failures += expect(blobs_status.succeeded() && spawn_status.succeeded() && holding_thread.has_value());
    if (failures != 0) return failures;

    held_scan_t held;
    failures += arm_held(held, executor, nullptr, ustore_completion_blocking_k);
    st::expected<fu_generation_t> const generation = holding_thread->start<&scan_blocking>(held);
    failures += expect(generation.has_value());
    if (failures != 0) return failures;
    failures += wait_held(held);
    ustore_key_t const key = key_from_integer(1300);
    ustore_outcome_t outcome {};
    submit_writes(database, blobs, executor, {&key, 1}, {&outcome, 1});
    failures += expect_status(outcome, status_code_t::contended_k);
    failures += expect(held.drained == ustore_status_contended_k);
    fu::standard_atomic_ref<hold_stage_t> {held.stage}.store(hold_stage_t::released_k, std::memory_order_release);
    holding_thread->join(*generation);
    failures += expect_success(held.outcome);
    submit_writes(database, blobs, executor, {&key, 1}, {&outcome, 1});
    failures += expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    ustore_executor_free(executor);
    return failures;
}

/** A deferred call on a caller-exclusive executor finishes while its caller only polls its status,
 *  as the executor's thread 0 serves it, and a blocking call afterwards sees what it wrote. */
std::size_t executor_finishes_by_polling(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "executor_finishes_by_polling.blobs",
                                                   ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    auto [executor, spawn_status] = spawn_executor();
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());

    std::array<ustore_key_t, 3> const keys {key_from_integer(75), key_from_integer(76), key_from_integer(77)};
    ustore_outcome_t written {};
    ustore_blobs_write_t write {};
    write.database = database;
    write.executor = executor;
    write.outcome = &written;
    write.completion = ustore_completion_deferred_k;
    write.count = keys.size();
    write.collections = {&blobs, 0};
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values = stored_values_k.view();
    ustore_blobs_write(&write);
    failures += expect(
        spin_until([&written]() noexcept { return ustore_outcome_status(&written) != ustore_status_deferred_k; }));
    failures += expect_success(written);

    read_columns_t columns;
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
    read.executor = executor;
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences && columns.presences[0] == 0b111);
    ustore_executor_free(executor);
    return failures;
}

/** A caller that sees a deferred call final may submit the next one at once, naming the same arena,
 *  since a call releases its arena and its executor before its status turns final. */
std::size_t executor_submit_after_wait(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "executor_submit_after_wait.blobs",
                                                   ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    auto [executor, spawn_status] = spawn_executor();
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());

    ustore_key_t const key = key_from_integer(900);
    failures += write_text(database, blobs, {&key, 1}, "value");
    std::size_t contended = 0;
    std::size_t failed = 0;
    for (std::size_t round = 0; round != 1000; ++round) {
        read_columns_t columns;
        ustore_outcome_t outcome {};
        ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
        read.executor = executor;
        read.completion = ustore_completion_deferred_k;
        ustore_blobs_read(&read);
        ustore_status_t const status = ustore_outcome_wait(&outcome);
        contended += status == ustore_status_contended_k;
        failed += status != ustore_status_success_k;
    }
    failures += expect_eq(contended, 0u);
    failures += expect_eq(failed, 0u);
    ustore_executor_free(executor);
    return failures;
}

/** A drain waits for the call holding the executor, then applies its mood: after a parked drain the
 *  next call wakes the threads, and after a spinning one it runs at once. */
std::size_t executor_drain(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "executor_drain.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    auto [executor, spawn_status] = spawn_executor();
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;

    ustore_outcome_t outcome {};
    ustore_executor_drain_t drain {.executor = executor, .outcome = &outcome};
    for (ustore_mood_t const mood : {ustore_mood_parked_k, ustore_mood_spinning_k, ustore_mood_parked_k}) {
        ustore_key_t const key = key_from_integer(78 + mood);
        ustore_outcome_t written {};
        submit_writes(database, blobs, executor, {&key, 1}, {&written, 1});
        drain.mood = mood;
        ustore_executor_drain(&drain);
        failures += expect_success(outcome);
        failures += expect(ustore_outcome_wait(&written) == ustore_status_success_k);
    }
    ustore_executor_free(executor);
    return failures;
}

/** A call naming an arena an unfinished call holds finishes contended, and so does one on a
 *  transaction it reads through, each on the calling thread, until the holding call ends. */
std::size_t executor_contended_arena(config_t const&) {
    std::size_t failures = 0;
    auto [executor, spawn_status] = spawn_executor();
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;

    held_scan_t held;
    ustore_transaction_handle_t transaction {.engine = &progress_engine_k, .database = &held.database};
    failures += submit_held(held, executor, ustore_transaction_view(&transaction, ustore_watch_keys_k));
    ustore_outcome_t outcome {};
    ustore_blobs_scan_t on_arena = held.scan;
    on_arena.executor = nullptr;
    on_arena.view = nullptr;
    on_arena.outcome = &outcome;
    on_arena.progress = nullptr;
    on_arena.progress_state = nullptr;
    ustore_blobs_scan(&on_arena);
    failures += expect_status(outcome, status_code_t::contended_k);
    ustore_blobs_scan_t on_transaction = on_arena;
    on_transaction.arena = nullptr;
    on_transaction.view = ustore_transaction_view(&transaction, ustore_watch_keys_k);
    auto [own_arena, own_arena_status] = arena_t::make();
    failures += expect(own_arena_status.succeeded());
    on_transaction.arena = own_arena;
    ustore_blobs_scan(&on_transaction);
    failures += expect_status(outcome, status_code_t::contended_k);

    failures += release_held(held);
    ustore_blobs_scan(&on_arena);
    failures += expect_success(outcome);
    ustore_blobs_scan(&on_transaction);
    failures += expect_success(outcome);
    ustore_executor_free(executor);
    return failures;
}

/** Holds every thread of a lender's pool in one dispatch until released, so a borrower finds the
 *  pool running it. */
struct lender_hold_t {
    std::size_t entered = 0;
    std::uint32_t released = 0;
};

/** Holds one thread of the lender's pool until @p hold is released. */
void hold_lender_thread(lender_hold_t& hold) noexcept {
    fu::standard_atomic_ref<std::size_t> {hold.entered}.add(1, std::memory_order_acq_rel);
    fu::standard_atomic_ref<std::uint32_t> const released {hold.released};
    while (released.load(std::memory_order_acquire) == 0) std::this_thread::yield();
}

/** Tasks in every dispatch of a lender's own. */
constexpr std::size_t lender_tasks_k = 16;

/** A lender dispatching on its own pool over and over until the borrower's writes are final, and
 *  what it saw. */
struct lender_loop_t {
    executor_t& pool;
    std::size_t writes_expected = 0;
    std::size_t writes_finished = 0;
    std::size_t dispatched = 0;
    std::size_t refused = 0;
    std::size_t failed = 0;
    std::size_t tasks = 0;
};

/** Dispatches on the lender's pool until every write of @p lending is final, counting the
 *  dispatches the pool refused while it ran a borrower's call. */
void lend_until_finished(lender_loop_t& lending) noexcept {
    fu::standard_atomic_ref<std::size_t> const finished {lending.writes_finished};
    do {
        st::status_t const status = lending.pool.for_n(lender_tasks_k, [&lending](std::size_t, std::size_t) noexcept {
            fu::standard_atomic_ref<std::size_t> {lending.tasks}.add(1, std::memory_order_relaxed);
        });
        if (status == st::status_t::operation_would_block_k) ++lending.refused;
        else if (st::failed(status)) ++lending.failed;
        else ++lending.dispatched;
    } while (finished.load(std::memory_order_acquire) != lending.writes_expected);
}

/** A borrowed pool runs a deferred call at submission, as its lender keeps its dispatches and
 *  joins, and a call that forks nothing succeeds while the lender's dispatch holds the pool, while
 *  one splitting its batch is refused rather than forking over the lender. While the lender keeps
 *  dispatching from another thread, every write lands once retried past a refusal, and once the
 *  executor is freed the lender dispatches on its pool alone again. */
std::size_t executor_on_pool(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "executor_on_pool.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    st::expected<executor_t> lender = executor_t::make(2, caller_exclusivity_t::caller_exclusive_k);
    st::expected<executor_t> lending_thread = executor_t::make(1, caller_exclusivity_t::caller_exclusive_k);
    failures += expect(lender.has_value() && lending_thread.has_value());
    if (failures != 0) return failures;

    ustore_executor_t executor = nullptr;
    ustore_outcome_t outcome {};
    ustore_executor_on_pool_t lend {.executor = &executor, .outcome = &outcome, .pool = lender->view().pool()};
    ustore_executor_on_pool(&lend);
    failures += expect_success(outcome);
    if (failures != 0) return failures;

    lender_hold_t hold;
    st::expected<fu_generation_t> const holding = lender->start<&hold_lender_thread>(hold);
    failures += expect(holding.has_value());
    if (failures != 0) return failures;
    fu::standard_atomic_ref<std::size_t> const entered {hold.entered};
    failures += expect(spin_until([&entered]() noexcept { return entered.load(std::memory_order_acquire) == 2; }));
    ustore_key_t const held_key = key_from_integer(700);
    ustore_outcome_t written {};
    submit_writes(database, blobs, executor, {&held_key, 1}, {&written, 1});
    failures += expect_success(written);
    failures += expect(write_blocking(database, blobs, held_key, executor) == ustore_status_success_k);
    std::array<ustore_key_t, 2 * tasks_per_word_k> sliced_keys {};
    for (std::size_t index = 0; index != sliced_keys.size(); ++index) sliced_keys[index] = key_from_integer(index);
    auto [sliced_arena, sliced_arena_status] = arena_t::make(executor);
    failures += expect(sliced_arena_status.succeeded());
    read_columns_t sliced_columns;
    ustore_blobs_read_t sliced = read_of(database, blobs, sliced_keys, sliced_columns, sliced_arena, outcome);
    sliced.executor = executor;
    ustore_blobs_read(&sliced);
    failures += expect_status(outcome, status_code_t::contended_k);
    fu::standard_atomic_ref<std::uint32_t> {hold.released}.store(1, std::memory_order_release);
    lender->join(*holding);

    constexpr std::size_t writes_k = 64;
    lender_loop_t lending {.pool = *lender, .writes_expected = writes_k};
    st::expected<fu_generation_t> const looping = lending_thread->start<&lend_until_finished>(lending);
    failures += expect(looping.has_value());
    if (failures != 0) return failures;
    std::array<ustore_key_t, writes_k> keys {};
    for (std::size_t index = 0; index != writes_k; ++index) {
        keys[index] = key_from_integer(100 + index);
        ustore_status_t status = ustore_status_contended_k;
        while (status == ustore_status_contended_k) {
            ustore_outcome_t written {};
            submit_writes(database, blobs, executor, {&keys[index], 1}, {&written, 1});
            status = ustore_outcome_wait(&written);
        }
        failures += expect(status == ustore_status_success_k);
        fu::standard_atomic_ref<std::size_t> {lending.writes_finished}.add(1, std::memory_order_release);
    }
    lending_thread->join(*looping);
    failures += expect(lending.failed == 0);
    failures += expect(lending.tasks == lending.dispatched * lender_tasks_k);

    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
    read.executor = executor;
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences != nullptr);
    for (std::size_t octet = 0; columns.presences && octet != writes_k / 8; ++octet)
        failures += expect(columns.presences[octet] == 0xFF);
    ustore_executor_free(executor);

    std::size_t const tasks_before = lending.tasks;
    lending.writes_finished = writes_k;
    lend_until_finished(lending);
    failures += expect(lending.tasks == tasks_before + lender_tasks_k);
    return failures;
}

/** A call on a host executor given an arena on a device is refused with @c ustore_status_device_k,
 *  and served once the arena lives on host pages again. A build without CUDA makes no device arena,
 *  so the case moves one by hand. */
std::size_t executor_arena_on_device(config_t const&) {
    std::size_t failures = 0;
    ustore_database_handle_t database {&progress_engine_k};
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;

    ustore_collection_id_t const blobs {};
    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* keys = nullptr;
    ustore_outcome_t outcome {};
    ustore_blobs_scan_t scan {
        .database = &database,
        .outcome = &outcome,
        .count = 1,
        .collections = {&blobs, 0},
        .offsets = &offsets,
        .keys = &keys,
        .arena = arena,
        .wanted = 1,
    };
    ustore_arena_t const placed = arena;
    placed->memory = cuda_blocks_t {.device = 0};
    ustore_blobs_scan(&scan);
    failures += expect_status(outcome, status_code_t::device_k);

    placed->memory = host_blocks_t {};
    ustore_blobs_scan(&scan);
    failures += expect_success(outcome);
    return failures;
}

/** Waiting on a deferred call blocks until its status is final, past the moment
 *  its callback returns. */
std::size_t outcome_wait_blocks(config_t const&) {
    std::size_t failures = 0;
    auto [executor, spawn_status] = spawn_executor();
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;

    held_scan_t held;
    failures += submit_held(held, executor);
    fu::standard_atomic_ref<hold_stage_t> const stage {held.stage};
    stage.store(hold_stage_t::released_k, std::memory_order_release);
    failures += expect(ustore_outcome_wait(&held.outcome) == ustore_status_success_k);
    failures += expect(stage.load(std::memory_order_acquire) == hold_stage_t::returned_k);
    ustore_executor_free(executor);
    return failures;
}

/** Freeing a database waits for the deferred call naming it, past the moment its callback returns,
 *  though the status of that call may turn final after the free, as it lowers its counts first. */
std::size_t database_free_waits(config_t const&) {
    std::size_t failures = 0;
    auto [executor, spawn_status] = spawn_executor();
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;

    held_scan_t held;
    failures += submit_held(held, executor);
    fu::standard_atomic_ref<hold_stage_t> const stage {held.stage};
    stage.store(hold_stage_t::released_k, std::memory_order_release);
    // A second hold keeps the router from deleting the case's own handle once the
    // free stops waiting
    held.database.holds = 2;
    ustore_database_free(&held.database);
    failures += expect(stage.load(std::memory_order_acquire) == hold_stage_t::returned_k);
    failures += expect(ustore_outcome_wait(&held.outcome) == ustore_status_success_k);
    ustore_executor_free(executor);
    return failures;
}

/** An init runs on an executor as every call does: a blocking one returns with the database open,
 *  and a deferred one runs from its copy, whose original died with the frame that built it, and
 *  writes the database before its outcome is final. */
case_result_t database_init_on_executor(config_t const& config) {
    std::size_t failures = 0;
    auto [executor, spawn_status] = spawn_executor();
    auto [arena, arena_status] = arena_t::make();
    failures += expect(spawn_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;

    ustore_database_t database = nullptr;
    ustore_outcome_t outcome {};
    open_engine(config.options(), &database, &outcome, executor, ustore_completion_blocking_k);
    failures += expect_success(outcome);
    failures += expect(database != nullptr);
    auto [blobs, blobs_status] = create_collection(database, "database_init_on_executor.blobs",
                                                   ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    ustore_key_t const key = key_from_integer(1200);
    if (database) failures += write_text(database, blobs, {&key, 1}, "value");
    ustore_database_free(database);

    database = nullptr;
    open_engine(config.options(), &database, &outcome, executor, ustore_completion_deferred_k);
    failures += expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    failures += expect(database != nullptr);
    if (database) {
        read_columns_t columns;
        ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
        ustore_blobs_read(&read);
        failures += expect_success(outcome);
        failures += expect(tape_written(columns.values) && columns.value(0) == "value");
    }
    ustore_database_free(database);
    ustore_executor_free(executor);
    return failures;
}

/** A read of more keys than one word of tasks, and a scan of as many tasks, split over the
 *  executor's threads, blocking or deferred, and match byte for byte what the same calls hand back
 *  on the calling thread. */
std::size_t executor_sliced_read(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "executor_sliced_read.blobs", ustore_modality_blobs_k);
    auto [executor, spawn_status] = spawn_executor();
    failures += expect(blobs_status.succeeded() && spawn_status.succeeded());
    if (failures != 0) return failures;

    constexpr std::size_t keys_k = 300;
    std::array<ustore_key_t, keys_k> keys {};
    for (std::size_t index = 0; index != keys_k; ++index) {
        keys[index] = key_from_integer(5000 + index);
        if (index % 3 == 0) continue;
        std::array<char, 32> text {};
        std::size_t const length = fmt::format_to_n(text.data(), text.size(), "value-{}", index).size;
        failures += write_text(database, blobs, {&keys[index], 1}, {text.data(), length});
    }

    auto [alone_arena, alone_arena_status] = arena_t::make();
    failures += expect(alone_arena_status.succeeded());
    read_columns_t alone;
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database, blobs, keys, alone, alone_arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    if (failures != 0) return failures;

    for (ustore_completion_t const completion : {ustore_completion_blocking_k, ustore_completion_deferred_k}) {
        auto [arena, arena_status] = arena_t::make(executor);
        failures += expect(arena_status.succeeded());
        read_columns_t sliced;
        ustore_blobs_read_t on_executor = read_of(database, blobs, keys, sliced, arena, outcome);
        on_executor.executor = executor;
        on_executor.completion = completion;
        ustore_blobs_read(&on_executor);
        failures += expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
        if (!sliced.presences || !tape_written(sliced.values)) {
            failures += expect(false);
            continue;
        }
        failures += expect(std::memcmp(alone.presences, sliced.presences, (keys_k + 7) / 8) == 0);
        for (std::size_t index = 0; index != keys_k; ++index)
            failures += expect_eq(sliced.value(index), alone.value(index));
    }

    // One scan task per key, each listing the next two keys from it, through the same split
    ustore_length_t const* alone_offsets = nullptr;
    ustore_key_t const* alone_keys = nullptr;
    ustore_blobs_scan_t scan {.database = database,
                              .outcome = &outcome,
                              .count = keys_k,
                              .collections = {&blobs, 0},
                              .start_keys = {keys.data(), sizeof(ustore_key_t)},
                              .offsets = &alone_offsets,
                              .keys = &alone_keys,
                              .arena = alone_arena,
                              .wanted = 2};
    ustore_blobs_scan(&scan);
    failures += expect_success(outcome);
    for (ustore_completion_t const completion : {ustore_completion_blocking_k, ustore_completion_deferred_k}) {
        auto [arena, arena_status] = arena_t::make(executor);
        failures += expect(arena_status.succeeded());
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* found = nullptr;
        ustore_blobs_scan_t on_executor = scan;
        on_executor.executor = executor, on_executor.completion = completion;
        on_executor.offsets = &offsets, on_executor.keys = &found, on_executor.arena = arena;
        ustore_blobs_scan(&on_executor);
        failures += expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
        if (!offsets || !alone_offsets) {
            failures += expect(false);
            continue;
        }
        failures += expect(std::memcmp(offsets, alone_offsets, (keys_k + 1) * sizeof(ustore_length_t)) == 0);
        failures += expect(std::memcmp(found, alone_keys, alone_offsets[keys_k] * sizeof(ustore_key_t)) == 0);
    }
    ustore_executor_free(executor);
    return failures;
}

/** A read of one field and a gather of one column over more documents than one word of tasks
 *  split over the executor's threads and match byte for byte what they hand back without one. */
std::size_t executor_sliced_docs(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [docs, docs_status] = create_collection(database, "executor_sliced_docs.docs", ustore_modality_docs_k);
    auto [executor, spawn_status] = spawn_executor();
    failures += expect(docs_status.succeeded() && spawn_status.succeeded());
    if (failures != 0) return failures;

    // Every third key holds no document, so presences and validities both have gaps
    constexpr std::size_t docs_k = 200;
    std::array<ustore_key_t, docs_k> keys {};
    std::array<ustore_key_t, docs_k> written_keys {};
    std::array<char, docs_k * 16> texts {};
    std::array<std::uint32_t, docs_k + 1> offsets {};
    std::size_t written = 0;
    for (std::size_t index = 0; index != docs_k; ++index) {
        keys[index] = key_from_integer(7000 + index);
        if (index % 3 == 0) continue;
        written_keys[written] = keys[index];
        std::size_t const length = fmt::format_to_n(texts.data() + offsets[written], 16, "{{\"age\":{}}}", index).size;
        offsets[written + 1] = static_cast<std::uint32_t>(offsets[written] + length);
        ++written;
    }
    ustore_outcome_t outcome {};
    ustore_docs_write_t write {.database = database,
                               .outcome = &outcome,
                               .count = written,
                               .collections = {&docs, 0},
                               .keys = {written_keys.data(), sizeof(ustore_key_t)},
                               .values = u32_tape(texts.data(), offsets.data())};
    ustore_docs_write(&write);
    failures += expect_success(outcome);

    char const age[] = "age";
    std::array<std::uint32_t, docs_k + 1> field_offsets {};
    std::array<char, docs_k * 3> field_texts {};
    for (std::size_t index = 0; index != docs_k; ++index) {
        std::memcpy(field_texts.data() + 3 * index, age, 3);
        field_offsets[index + 1] = static_cast<std::uint32_t>(3 * (index + 1));
    }
    ustore_docs_field_type_t const i32_type = ustore_docs_field_type_i32_k;
    struct docs_outputs_t {
        ustore_octet_t const* presences = nullptr;
        ustore_strings_view_t values {};
        ustore_docs_column_view_t const* columns = nullptr;
    };
    auto read_and_gather = [&](ustore_executor_t on, ustore_completion_t completion, ustore_arena_t read_arena,
                               ustore_arena_t gather_arena, docs_outputs_t& outputs) {
        ustore_docs_read_t read {.database = database,
                                 .executor = on,
                                 .outcome = &outcome,
                                 .completion = completion,
                                 .count = docs_k,
                                 .collections = {&docs, 0},
                                 .keys = {keys.data(), sizeof(ustore_key_t)},
                                 .fields = u32_tape(field_texts.data(), field_offsets.data()),
                                 .presences = &outputs.presences,
                                 .values = &outputs.values,
                                 .arena = read_arena,
                                 .outputs_type = ustore_docs_field_type_string_k};
        ustore_docs_read(&read);
        std::size_t checked = expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
        ustore_docs_gather_t gather {.database = database,
                                     .executor = on,
                                     .outcome = &outcome,
                                     .completion = completion,
                                     .count = docs_k,
                                     .collections = {&docs, 0},
                                     .keys = {keys.data(), sizeof(ustore_key_t)},
                                     .fields_count = 1,
                                     .fields = u32_tape(field_texts.data(), field_offsets.data()),
                                     .columns = &outputs.columns,
                                     .arena = gather_arena,
                                     .outputs_types = &i32_type};
        ustore_docs_gather(&gather);
        return checked + expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    };

    auto [alone_read_arena, alone_read_status] = arena_t::make();
    auto [alone_gather_arena, alone_gather_status] = arena_t::make();
    failures += expect(alone_read_status.succeeded() && alone_gather_status.succeeded());
    docs_outputs_t alone;
    failures += read_and_gather(nullptr, ustore_completion_blocking_k, alone_read_arena, alone_gather_arena, alone);
    if (failures != 0 || !alone.presences || !alone.columns) return failures + expect(false);
    for (ustore_completion_t const completion : {ustore_completion_blocking_k, ustore_completion_deferred_k}) {
        auto [read_arena, read_status] = arena_t::make(executor);
        auto [gather_arena, gather_status] = arena_t::make(executor);
        failures += expect(read_status.succeeded() && gather_status.succeeded());
        docs_outputs_t sliced;
        failures += read_and_gather(executor, completion, read_arena, gather_arena, sliced);
        if (!sliced.presences || !sliced.columns) {
            failures += expect(false);
            continue;
        }
        failures += expect(std::memcmp(alone.presences, sliced.presences, (docs_k + 7) / 8) == 0);
        for (std::size_t index = 0; index != docs_k; ++index)
            failures += expect_eq(string_of(tape_string(sliced.values, index)),
                                  string_of(tape_string(alone.values, index)));
        failures += expect(std::memcmp(alone.columns[0].validities, sliced.columns[0].validities, (docs_k + 7) / 8) ==
                           0);
        failures += expect(
            std::memcmp(alone.columns[0].scalars, sliced.columns[0].scalars, docs_k * sizeof(std::int32_t)) == 0);
    }
    ustore_executor_free(executor);
    return failures;
}

/** A read of the edges of more vertices than one word of tasks splits over the executor's threads
 *  and matches byte for byte the degrees, offsets and edges it hands back without one. */
case_result_t executor_sliced_graph(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [graph, graph_status] = create_collection(database, "executor_sliced_graph.graph", ustore_modality_graph_k);
    auto [executor, spawn_status] = spawn_executor();
    failures += expect(graph_status.succeeded() && spawn_status.succeeded());
    if (failures != 0) return failures;

    // A chain over the even vertices: each odd one is missing and each even one has two neighbors
    constexpr std::size_t vertices_k = 300;
    std::array<ustore_key_t, vertices_k> vertices {};
    std::array<ustore_key_t, vertices_k> sources {}, targets {};
    std::size_t edges = 0;
    for (std::size_t index = 0; index != vertices_k; ++index) {
        vertices[index] = key_from_integer(9000 + index);
        if (index % 2 != 0 || index + 2 >= vertices_k) continue;
        sources[edges] = key_from_integer(9000 + index), targets[edges] = key_from_integer(9000 + index + 2);
        ++edges;
    }
    ustore_outcome_t outcome {};
    ustore_graph_write_edges_t write {.database = database,
                                      .outcome = &outcome,
                                      .count = edges,
                                      .collections = {&graph, 0},
                                      .sources = {sources.data(), sizeof(ustore_key_t)},
                                      .targets = {targets.data(), sizeof(ustore_key_t)}};
    ustore_graph_write_edges(&write);
    failures += expect_success(outcome);

    struct edges_outputs_t {
        ustore_vertex_degree_t const* degrees = nullptr;
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* edges = nullptr;
    };
    auto read_edges = [&](ustore_executor_t on, ustore_completion_t completion, ustore_arena_t arena,
                          edges_outputs_t& outputs) {
        ustore_graph_read_edges_t read {.database = database,
                                        .executor = on,
                                        .outcome = &outcome,
                                        .completion = completion,
                                        .count = vertices_k,
                                        .collections = {&graph, 0},
                                        .vertices = {vertices.data(), sizeof(ustore_key_t)},
                                        .degrees_per_vertex = &outputs.degrees,
                                        .edges_offsets = &outputs.offsets,
                                        .edges_per_vertex = &outputs.edges,
                                        .arena = arena};
        ustore_graph_read_edges(&read);
        return expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    };
    auto [alone_arena, alone_arena_status] = arena_t::make();
    failures += expect(alone_arena_status.succeeded());
    edges_outputs_t alone;
    failures += read_edges(nullptr, ustore_completion_blocking_k, alone_arena, alone);
    if (failures != 0 || !alone.offsets) return failures + expect(false);
    for (ustore_completion_t const completion : {ustore_completion_blocking_k, ustore_completion_deferred_k}) {
        auto [arena, arena_status] = arena_t::make(executor);
        failures += expect(arena_status.succeeded());
        edges_outputs_t sliced;
        failures += read_edges(executor, completion, arena, sliced);
        if (!sliced.offsets) {
            failures += expect(false);
            continue;
        }
        failures += expect(std::memcmp(alone.degrees, sliced.degrees, vertices_k * sizeof(ustore_vertex_degree_t)) ==
                           0);
        failures += expect(std::memcmp(alone.offsets, sliced.offsets, (vertices_k + 1) * sizeof(ustore_length_t)) == 0);
        failures += expect(
            std::memcmp(alone.edges, sliced.edges, 3 * alone.offsets[vertices_k] * sizeof(ustore_key_t)) == 0);
    }
    ustore_executor_free(executor);
    return failures;
}

/** A scan of more prefixes than one word of tasks splits over the executor's threads and matches
 *  byte for byte the offsets and strings it hands back without one. */
case_result_t executor_sliced_strings(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [words, words_status] = create_collection(database, "executor_sliced_strings.words",
                                                   ustore_modality_strings_k);
    auto [executor, spawn_status] = spawn_executor();
    failures += expect(words_status.succeeded() && spawn_status.succeeded());
    if (failures != 0) return failures;

    // Strings w000 to w299, and per task the prefix naming its tens, so the pages of tasks overlap
    constexpr std::size_t strings_k = 300;
    std::array<char, strings_k * 4> texts {};
    std::array<std::uint32_t, strings_k + 1> offsets {};
    std::array<ustore_key_t, strings_k> keys {};
    for (std::size_t index = 0; index != strings_k; ++index) {
        fmt::format_to_n(texts.data() + 4 * index, 4, "w{:03}", index);
        offsets[index + 1] = static_cast<std::uint32_t>(4 * (index + 1));
        keys[index] = key_from_integer(11000 + index);
    }
    ustore_outcome_t outcome {};
    ustore_strings_write_t write {.database = database,
                                  .outcome = &outcome,
                                  .count = strings_k,
                                  .collections = {&words, 0},
                                  .strings = u32_tape(texts.data(), offsets.data()),
                                  .values = {keys.data(), sizeof(ustore_key_t)}};
    ustore_strings_write(&write);
    failures += expect_success(outcome);

    ustore_length_t const* alone_offsets = nullptr;
    ustore_strings_view_t alone_strings {};
    auto [alone_arena, alone_arena_status] = arena_t::make();
    failures += expect(alone_arena_status.succeeded());
    // Each task's prefix is the first three bytes of one string, as one tape over the stored texts
    std::array<char, strings_k * 3> prefixes {};
    std::array<std::uint32_t, strings_k + 1> prefixes_offsets {};
    for (std::size_t index = 0; index != strings_k; ++index) {
        std::memcpy(prefixes.data() + 3 * index, texts.data() + 4 * index, 3);
        prefixes_offsets[index + 1] = static_cast<std::uint32_t>(3 * (index + 1));
    }
    ustore_strings_scan_t scan {.database = database,
                                .outcome = &outcome,
                                .count = strings_k,
                                .collections = {&words, 0},
                                .prefixes = u32_tape(prefixes.data(), prefixes_offsets.data()),
                                .offsets = &alone_offsets,
                                .strings = &alone_strings,
                                .arena = alone_arena,
                                .wanted = 4};
    ustore_strings_scan(&scan);
    failures += expect_success(outcome);
    if (failures != 0 || !alone_offsets) return failures + expect(false);
    for (ustore_completion_t const completion : {ustore_completion_blocking_k, ustore_completion_deferred_k}) {
        auto [arena, arena_status] = arena_t::make(executor);
        failures += expect(arena_status.succeeded());
        ustore_length_t const* sliced_offsets = nullptr;
        ustore_strings_view_t sliced_strings {};
        ustore_strings_scan_t on_executor = scan;
        on_executor.executor = executor, on_executor.completion = completion, on_executor.arena = arena;
        on_executor.offsets = &sliced_offsets, on_executor.strings = &sliced_strings;
        ustore_strings_scan(&on_executor);
        failures += expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
        if (!sliced_offsets) {
            failures += expect(false);
            continue;
        }
        failures += expect(std::memcmp(alone_offsets, sliced_offsets, (strings_k + 1) * sizeof(ustore_length_t)) == 0);
        failures += expect(sliced_strings.layout == alone_strings.layout);
        for (std::size_t index = 0; index != alone_offsets[strings_k]; ++index)
            failures += expect_eq(string_of(tape_string(sliced_strings, index)),
                                  string_of(tape_string(alone_strings, index)));
    }
    ustore_executor_free(executor);
    return failures;
}

#pragma endregion Deferred Calls

#pragma region Placement

/** A RAM executor refuses a memory domain the machine lacks, leaving its output null, as does one
 *  borrowing a pool, and keeps every arena it makes on the domain of its thread 0. */
std::size_t executor_memory_domains(config_t const&) {
    std::size_t failures = 0;
    st::expected<topology_t> topology = topology_t::make();
    st::expected<executor_t> lender = executor_t::make(1, caller_exclusivity_t::caller_exclusive_k);
    failures += expect(topology.has_value() && lender.has_value());
    if (failures != 0) return failures;
    std::int32_t const known = topology->memory_domain(0);
    std::int32_t const unknown = 1 << 20;

    ustore_outcome_t outcome {};
    ustore_executor_t executor = nullptr;
    ustore_executor_for_ram_t refused {.executor = &executor, .outcome = &outcome, .ram = {&unknown, 1}};
    ustore_executor_for_ram(&refused);
    failures += expect_status(status_t {outcome}, status_code_t::invalid_argument_k);
    failures += expect(executor == nullptr);
    ustore_executor_on_pool_t lent {.executor = &executor,
                                    .outcome = &outcome,
                                    .ram = {&unknown, 1},
                                    .pool = lender->view().pool()};
    ustore_executor_on_pool(&lent);
    failures += expect_status(status_t {outcome}, status_code_t::invalid_argument_k);
    failures += expect(executor == nullptr);

    ustore_executor_for_ram_t placed {.executor = &executor, .outcome = &outcome, .ram = {&known, 1}, .threads = 1};
    ustore_executor_for_ram(&placed);
    failures += expect_success(outcome);
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());
    if (arena_status.succeeded()) failures += expect_eq(host_blocks_of(arena)->memory_domain, known);
    ustore_executor_free(executor);

    // Zero threads take 7/8 of the cores of every domain the process may allocate on
    ustore_executor_for_ram_t derived {.executor = &executor, .outcome = &outcome};
    ustore_executor_for_ram(&derived);
    failures += expect_success(outcome);
    ustore_executor_free(executor);
    return failures;
}

#pragma endregion Placement

#pragma region Receivers

/** Proxies on an executor read and write as on the calling thread, at the head, in a transaction
 *  and at a snapshot. */
std::size_t executor_on_receivers(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(executor_status.succeeded());
    if (failures != 0) return failures;

    triplet_t triplet;
    auto [collection, create_status] = database.on(executor).create("blobs");
    failures += expect(create_status.succeeded());
    auto reference = collection[triplet.keys];
    failures += round_trip(reference, triplet);

    if (database.supports_transactions()) {
        transaction_t transaction = *database.on(executor).transaction();
        transaction_ref_t transaction_on_executor = transaction.on(executor);
        blobs_collection_t inside {transaction_on_executor.receiver(), collection};
        auto transaction_reference = inside[triplet.keys];
        failures += check_equalities(transaction_reference, triplet);
        failures += expect(inside.at(triplet.keys).remove().succeeded());
        failures += expect(transaction_on_executor.commit().has_value());
        failures += check_length(reference, missing_length_k);
    }
    if (database.supports_snapshots()) {
        failures += round_trip(reference, triplet);
        snapshot_t snapshot = *database.on(executor).snapshot();
        snapshot_ref_t snapshot_on_executor = snapshot.on(executor);
        auto snapshot_reference = blobs_read_collection_t {snapshot_on_executor.receiver(), collection}[triplet.keys];
        failures += check_equalities(snapshot_reference, triplet);
    }
    failures += expect(executor.drain(ustore_mood_spinning_k).succeeded());
    return failures;
}

/** A deferred write through a transaction reaches the head only once the deferred commit finishes,
 *  which answers a stamp no lower than the one the deferred stage read at. */
case_result_t executor_deferred_commit(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(executor_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 2> const keys {key_from_integer(75), key_from_integer(76)};
    values_tape_t values;
    failures += fill_tape(values, {"left", "right"});

    auto [head, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());
    transaction_t transaction = *database.transaction();
    deferred_transaction_t deferred_transaction = transaction.deferred(executor);
    deferred_blobs_collection_t inside {transaction.receiver().on(executor), head};
    failures += expect(inside.write(keys, std::move(values)).wait().succeeded());
    failures += expect(!*head[keys[0]].present());

    auto [read_stamp, stage_status] = deferred_transaction.stage().wait();
    failures += expect(stage_status.succeeded());
    auto [commit_stamp, commit_status] = deferred_transaction.commit().wait();
    failures += expect(commit_status.succeeded());
    failures += expect(static_cast<ustore_stamp_t>(read_stamp) <= static_cast<ustore_stamp_t>(commit_stamp));
    failures += expect(*head[keys[0]].present() && *head[keys[1]].present());
    return failures;
}

/** A deferred document write and then deferred reads hand back the documents whole, one field of
 *  each as text, and a gathered column whose table outlives every move of its answer, and a
 *  deferred removal leaves the keys missing. */
std::size_t executor_deferred_docs(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(executor_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;

    failures += expect(database.create<docs_collection_t>("docs").has_value());
    auto [collection, collection_status] = database.deferred(executor).find<docs_collection_t>("docs");
    failures += expect(collection_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 2> const keys {key_from_integer(81), key_from_integer(82)};
    values_tape_t documents;
    failures += fill_tape(documents, {flat_documents_k[0], flat_documents_k[1]});
    failures += expect(collection.write(keys, std::move(documents)).wait().succeeded());

    auto [read, read_status] = collection.read(keys, arena).wait();
    failures += expect(read_status.succeeded());
    failures += expect_eq(read.size(), keys.size());
    for (std::size_t index = 0; index != read.size(); ++index)
        failures += expect(json_equal(as_text(read[index]), flat_documents_k[index]));

    values_tape_t person;
    failures += fill_tape(person, {"person"});
    auto [people,
          people_status] = collection.read(keys, arena, std::move(person), ustore_docs_field_type_string_k).wait();
    failures += expect(people_status.succeeded());
    failures += expect_eq(people.size(), keys.size());
    if (people.size() == keys.size()) {
        failures += expect_eq(as_text(people[0]), std::string_view("Alice"));
        failures += expect_eq(as_text(people[1]), std::string_view("Bob"));
    }

    values_tape_t age;
    failures += fill_tape(age, {"age"});
    auto [gathered, gather_status] =
        collection.gather(keys, std::move(age), std::array {ustore_docs_field_type_i32_k}, arena).wait();
    failures += expect(gather_status.succeeded());
    docs_gather_t::finished_t const moved = std::move(gathered);
    failures += expect_eq(moved.table.rows(), keys.size());
    failures += expect_eq(moved.table.columns(), 1u);
    if (moved.table.rows() == keys.size() && moved.table.columns() == 1) {
        auto const ages = moved.table.column(0).as<i32_t>();
        strided_span<std::string_view const> const names = moved.table.header().fields();
        strided_span<ustore_key_t const> const rows = moved.table.index().keys();
        failures += expect(ages[0].valid && ages[1].valid);
        failures += expect_eq(ages[0].value, 24);
        failures += expect_eq(ages[1].value, 25);
        failures += expect_eq(names[0], std::string_view("age"));
        failures += expect(rows[1] == keys[1]);
    }

    failures += expect(collection.remove(keys).wait().succeeded());
    auto [removed, removed_status] = collection.read(keys, arena).wait();
    failures += expect(removed_status.succeeded());
    failures += expect_eq(removed.size(), keys.size());
    for (std::size_t index = 0; index != removed.size(); ++index) failures += expect(!removed[index]);
    return failures;
}

/** A deferred write of edges and then deferred reads hand back the edges every vertex leaves,
 *  split by the offsets with none for a missing vertex between two present ones, and the degree
 *  of every vertex. */
case_result_t executor_deferred_graph(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(executor_status.succeeded());
    failures += expect(database.create<graph_collection_t>("graph").has_value());
    auto [graph, graph_status] = database.deferred(executor).find<graph_collection_t>("graph");
    failures += expect(graph_status.succeeded());
    if (failures != 0) return failures;

    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    std::array<edge_t, 3> const triangle {edge_of(1, 2, 9), edge_of(2, 3, 10), edge_of(3, 1, 11)};
    failures += expect(graph.write_edges(triangle).wait().succeeded());

    std::array<ustore_key_t, 3> const vertices {key_from_integer(1), key_from_integer(4), key_from_integer(2)};
    auto [read, read_status] = graph.read_edges(vertices, arena, ustore_vertex_role_source_k).wait();
    failures += expect(read_status.succeeded());
    failures += expect_eq(read.size(), 2u);
    failures += expect_eq(read.offsets.size(), vertices.size() + 1);
    if (read.offsets.size() == vertices.size() + 1) {
        edges_view_t const leaving_first = read.of_vertex(0), leaving_second = read.of_vertex(2);
        failures += expect(leaving_first.size() == 1 && leaving_first[0] == triangle[0]);
        failures += expect_eq(read.of_vertex(1).size(), 0u);
        failures += expect(leaving_second.size() == 1 && leaving_second[0] == triangle[1]);
    }

    auto [degrees, degrees_status] = graph.degrees(std::array {key_from_integer(1), key_from_integer(4)}, arena).wait();
    failures += expect(degrees_status.succeeded());
    failures += expect_eq(degrees.size(), 2u);
    if (degrees.size() == 2) {
        failures += expect_eq(degrees[0], 2u);
        failures += expect_eq(degrees[1], ustore_vertex_degree_missing_k);
    }
    return failures;
}

/** A deferred write of strings and then deferred reads hand back their keys, the nil UUID for a
 *  string never written, and a page of strings for every prefix. */
case_result_t executor_deferred_strings(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(executor_status.succeeded());
    failures += expect(database.create<strings_collection_t>("words").has_value());
    auto [words, words_status] = database.deferred(executor).find<strings_collection_t>("words");
    failures += expect(words_status.succeeded());
    if (failures != 0) return failures;

    values_tape_t texts;
    failures += fill_tape(texts, {"apple", "apricot", "banana"});
    std::array<ustore_key_t, 3> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    failures += expect(words.write(std::move(texts), keys).wait().succeeded());

    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    values_tape_t asked;
    failures += fill_tape(asked, {"banana", "cherry", "apple"});
    auto [read, read_status] = words.read(std::move(asked), arena).wait();
    failures += expect(read_status.succeeded());
    failures += expect_eq(read.size(), 3u);
    if (read.size() == 3) {
        failures += expect_eq(read[0], keys[2]);
        failures += expect_eq(read[1], ustore_key_t {});
        failures += expect_eq(read[2], keys[0]);
    }

    values_tape_t prefixes;
    failures += fill_tape(prefixes, {"ap", "b"});
    auto [scanned, scan_status] = words.scan(std::move(prefixes), 4, arena).wait();
    failures += expect(scan_status.succeeded());
    failures += expect_eq(scanned.pages.size(), 3u);
    failures += expect_eq(scanned.strings.size(), 3u);
    if (scanned.pages.size() == 3 && scanned.strings.size() == 3) {
        failures += expect_eq(scanned.pages[1] - scanned.pages[0], 2u);
        failures += expect_eq(scanned.strings[0], sz::string_view_t("apple"));
        failures += expect_eq(scanned.strings[1], sz::string_view_t("apricot"));
        failures += expect_eq(scanned.strings[2], sz::string_view_t("banana"));
    }
    return failures;
}

/** A database opened blocking on an executor keeps a write, and a deferred init stays deferred
 *  until thread 0 serves it, then opens the database holding that write. */
case_result_t executor_deferred_open(config_t const& config) {
    if (!config.directory()) return case_result_t::skipped(needs_directory_k);
    std::size_t failures = 0;
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(executor_status.succeeded());
    if (failures != 0) return failures;

    ustore_key_t const key = key_from_integer(91);
    {
        ustore_database_t opened = nullptr;
        ustore_outcome_t outcome {};
        open_engine(config.options(), &opened, &outcome, executor);
        failures += expect_success(outcome);
        if (failures != 0) return failures;
        database_t database {opened};
        auto [head, head_status] = database["blobs"];
        failures += expect(head_status.succeeded());
        failures += expect(head[key].assign("opened").succeeded());
    }

    ustore_database_t reopened = nullptr;
    ustore_outcome_t outcome {};
    open_engine(config.options(), &reopened, &outcome, executor, ustore_completion_deferred_k);
    ustore_outcome_wait(&outcome);
    failures += expect_success(outcome);
    if (failures != 0) return failures;
    database_t database {reopened};
    auto [head, find_status] = database.find("blobs");
    failures += expect(find_status.succeeded());
    failures += expect(head[key].value() == "opened");
    return failures;
}

/** A deferred receiver call on an executor another call holds finishes contended at once, and
 *  succeeds once the holding call is final. */
std::size_t executor_contended(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(executor_status.succeeded());
    if (failures != 0) return failures;

    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    std::array<ustore_key_t, 1> const keys {key_from_integer(77)};
    auto [head, head_status] = database["blobs"];
    failures += expect(head_status.succeeded());
    deferred_database_t const deferred_database = database.deferred(executor);
    deferred_blobs_collection_t collection = deferred_database[head];
    held_scan_t held;
    failures += submit_held(held, executor);
    deferred<blobs_read_t> refused = collection.read(keys, arena);
    failures += expect(refused.status() == status_code_t::contended_k);
    failures += expect_status(std::move(refused).wait(), status_code_t::contended_k);
    failures += release_held(held);
    failures += expect(executor.drain().succeeded());
    failures += expect(collection.read(keys, arena).wait().has_value());
    return failures;
}

#pragma endregion Receivers

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    auto const maybe_directory = directory_t::make(options.directory);
    if (!maybe_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", options.directory);
        return 1;
    }
    config_t const config {options.engine, maybe_directory->path()};

    report(totals, "arena.null_refused", arena_null_refused, config);
    report(totals, "arena.reuse", arena_reuse, config);
    report(totals, "arena.failed_outputs_null", arena_failed_outputs_null, config);
    report(totals, "arena.aligned_outputs", arena_aligned_outputs, config);
    report(totals, "arena.outputs_only", arena_outputs_only, config);
    report(totals, "arena.free_waits", arena_free_waits, config);
    report(totals, "executor.deferred_round_trip", executor_deferred_round_trip, config);
    report(totals, "executor.copied_submission", executor_copied_submission, config);
    report(totals, "executor.held_contended", executor_held_contended, config);
    report(totals, "executor.contended_from_another_thread", executor_contended_from_another_thread, config);
    report(totals, "executor.finishes_by_polling", executor_finishes_by_polling, config);
    report(totals, "executor.submit_after_wait", executor_submit_after_wait, config);
    report(totals, "executor.drain", executor_drain, config);
    report(totals, "executor.contended_arena", executor_contended_arena, config);
    report(totals, "executor.on_pool", executor_on_pool, config);
    report(totals, "executor.arena_on_device", executor_arena_on_device, config);
    report(totals, "outcome.wait_blocks", outcome_wait_blocks, config);
    report(totals, "database.free_waits", database_free_waits, config);
    report(totals, "database.init_on_executor", database_init_on_executor, config);
    report(totals, "executor.sliced_read", executor_sliced_read, config);
    report(totals, "executor.sliced_docs", executor_sliced_docs, config);
    report(totals, "executor.sliced_graph", executor_sliced_graph, config);
    report(totals, "executor.sliced_strings", executor_sliced_strings, config);
    report(totals, "executor.memory_domains", executor_memory_domains, config);
    report(totals, "executor.on_receivers", executor_on_receivers, config);
    report(totals, "executor.deferred_commit", executor_deferred_commit, config);
    report(totals, "executor.deferred_docs", executor_deferred_docs, config);
    report(totals, "executor.deferred_graph", executor_deferred_graph, config);
    report(totals, "executor.deferred_strings", executor_deferred_strings, config);
    report(totals, "executor.deferred_open", executor_deferred_open, config);
    report(totals, "executor.contended", executor_contended, config);
    return suite_exit_code(totals);
}
