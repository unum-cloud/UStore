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

#include <forkunion/atomics.hpp>   // `fu::standard_atomic_ref`
#include <forkunion/forkunion.hpp> // `fu::topology_t`, `fu::pool_t`

#include "ustore/ustore.hpp"

#include "ustore/utilities/ram_buffers.hpp" // `arena_block_t`, `arena_alignment_k`

#include "abi/fixture.hpp" // `held_scan_t`, `submit_held`, `release_held`, `spawn_executor`, `round_trip`, `edge_of`
#include "engine.hpp"      // `ustore_database_handle_t`
#include "harness.hpp"     // `finish_test_run`, `expect_true`, `expect_status`, `spin_until`
#include "utilities/slices.hpp" // `tasks_per_word_k`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::test::abi;
using namespace unum;

#pragma region Arenas

/** A call needs an arena for its outputs, so a @c NULL one is refused before any engine runs and
 *  its outputs are left @c NULL. An arena init without an output for the arena is refused too. */
void test_arena_null_refused(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "arena_null_refused.blobs",
                                                   ustore_modality_blobs_k);
    expect_true(!blobs_status.failed());

    ustore_key_t const key = key_from_integer(200);
    write_text(database.handle(), blobs, {&key, 1}, "value");
    std::uint32_t const sentinel = 0;
    read_columns_t columns {reinterpret_cast<ustore_octet_t const*>(&sentinel), util::u32_tape(&sentinel, &sentinel)};
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database.handle(), blobs, {&key, 1}, columns, nullptr, outcome);
    ustore_blobs_read(&read);
    expect_status(outcome, status_code_t::invalid_argument_k);
    expect_true(!columns.presences && !tape_written(columns.values));

    ustore_arena_init_t init {.outcome = &outcome};
    ustore_arena_init(&init);
    expect_status(outcome, status_code_t::invalid_argument_k);
}

/** One arena serves a read and then a scan, each call rewinding it and writing whole outputs. */
void test_arena_reuse(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "arena_reuse.blobs", ustore_modality_blobs_k);
    expect_true(!blobs_status.failed());
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return;

    // Three present keys and a missing one past them, which no other case writes between
    std::array<ustore_key_t, 4> const keys {key_from_integer(300), key_from_integer(301), key_from_integer(302),
                                            key_from_integer(303)};
    write_text(database.handle(), blobs, std::span {keys}.first(3), "value");
    read_columns_t columns;
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database.handle(), blobs, keys, columns, arena.handle(), outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    if (!expect_true(columns.presences && tape_written(columns.values))) return;
    expect_equal(columns.presences[0], 0b0111);
    expect_equal(columns.value(2), "value");
    expect_true(columns.value(3).empty());

    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* found = nullptr;
    ustore_blobs_scan_t scan {
        .database = database.handle(),
        .outcome = &outcome,
        .count = 1,
        .collections = {&blobs, 0},
        .start_keys = {&keys[0], 0},
        .offsets = &offsets,
        .keys = &found,
        .arena = arena.handle(),
        .wanted = 3,
    };
    ustore_blobs_scan(&scan);
    expect_success(outcome);
    expect_true(offsets && found && offsets[0] == 0 && offsets[1] == 3);
    if (found) expect_true(std::memcmp(found, keys.data(), 3 * sizeof(ustore_key_t)) == 0);
}

/** A call that fails leaves every output @c NULL, blocking or deferred, whatever the
 *  outputs held before. */
void test_arena_failed_outputs_null(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "arena_failed_outputs_null.blobs",
                                                   ustore_modality_blobs_k);
    expect_true(!blobs_status.failed());
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!spawn_status.failed())) return;
    auto [arena, arena_status] = arena_t::make({.executor = executor});
    expect_true(!arena_status.failed());

    // No collection holds this identifier, so every engine fails the read
    ustore_key_t const key = key_from_integer(500);
    ustore_collection_id_t const missing {uuid_from_integer(0x4242)};
    std::uint32_t const sentinel = 0;
    read_columns_t const stale {reinterpret_cast<ustore_octet_t const*>(&sentinel),
                                util::u32_tape(&sentinel, &sentinel)};
    for (ustore_completion_t const completion :
         std::array {ustore_completion_blocking_k, ustore_completion_deferred_k}) {
        read_columns_t columns = stale;
        ustore_outcome_t outcome {};
        ustore_blobs_read_t read = read_of(database.handle(), blobs, {&key, 1}, columns, arena.handle(), outcome);
        read.executor = executor;
        read.completion = completion;
        read.collections = {&missing, 0};
        ustore_blobs_read(&read);
        ustore_status_t const status = ustore_outcome_wait(&outcome);
        expect_true(status != ustore_status_success_k);
        expect_true(!columns.presences && !tape_written(columns.values));
    }

    // A scan without an output for its keys is refused, and its offsets are left null too
    ustore_length_t const* offsets = reinterpret_cast<ustore_length_t const*>(&sentinel);
    ustore_outcome_t outcome {};
    ustore_blobs_scan_t scan {
        .database = database.handle(),
        .outcome = &outcome,
        .count = 1,
        .collections = {&blobs, 0},
        .offsets = &offsets,
        .arena = arena.handle(),
        .wanted = 1,
    };
    ustore_blobs_scan(&scan);
    expect_status(outcome, status_code_t::invalid_argument_k);
    expect_true(!offsets);
    arena = {};
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** Whether @p output starts at a multiple of 64 bytes, where every output of an arena starts. */
bool aligned(void const* output) noexcept { return reinterpret_cast<std::uintptr_t>(output) % 64 == 0; }

/** Every output starts aligned to 64 bytes, as Apache Arrow buffers do, so typed columns read in
 *  place: the columns of a gather, of a read and the bounds of a measurement. */
void test_arena_aligned_outputs(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "arena_aligned_outputs.blobs",
                                                   ustore_modality_blobs_k);
    expect_true(!blobs_status.failed());
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return;

    // Three documents, the last without a height or a name, so every bitmap of a column differs
    std::array<ustore_key_t, 3> const keys {key_from_integer(800), key_from_integer(801), key_from_integer(802)};
    std::array<std::string_view, 3> const documents {R"({"born":1815,"height":1.5,"name":"ada"})",
                                                     R"({"born":1906,"height":1.6,"name":"grace"})",
                                                     R"({"born":1918})"};
    strided_strings<std::string_view> const values {strided(documents).begin()};
    auto [docs, docs_status] = create_collection(database.handle(), "aligned.docs", ustore_modality_docs_k);
    if (!expect_true(!docs_status.failed())) return;
    ustore_outcome_t outcome {};
    ustore_docs_write_t write {
        .database = database.handle(),
        .outcome = &outcome,
        .count = keys.size(),
        .collections = {&docs, 0},
        .keys = {keys.data(), sizeof(ustore_key_t)},
        .values = values.view(),
    };
    ustore_docs_write(&write);
    expect_success(outcome);

    std::array<std::string_view, 3> const fields {"born", "height", "name"};
    strided_strings<std::string_view> const fields_strings {strided(fields).begin()};
    std::array<ustore_docs_field_type_t, 3> const types {ustore_docs_field_type_u64_k, ustore_docs_field_type_f64_k,
                                                         ustore_docs_field_type_string_k};
    ustore_docs_column_view_t const* columns = nullptr;
    ustore_docs_gather_t gather {
        .database = database.handle(),
        .outcome = &outcome,
        .count = keys.size(),
        .collections = {&docs, 0},
        .keys = {keys.data(), sizeof(ustore_key_t)},
        .fields_count = fields.size(),
        .fields = fields_strings.view(),
        .columns = &columns,
        .arena = arena.handle(),
        .outputs_types = types.data(),
    };
    ustore_docs_gather(&gather);
    expect_success(outcome);
    if (!expect_true(columns != nullptr)) return;
    for (std::size_t field = 0; field != fields.size(); ++field) {
        ustore_docs_column_view_t const& column = columns[field];
        expect_true(aligned(column.validities) && aligned(column.conversions) && aligned(column.collisions));
        expect_true(aligned(column.scalars));
    }
    expect_true(aligned(columns[2].strings.data.u32_tape.offsets) && aligned(columns[2].strings.data.u32_tape.data));
    auto const* const born = reinterpret_cast<std::uint64_t const*>(columns[0].scalars);
    auto const* const heights = reinterpret_cast<double const*>(columns[1].scalars);
    expect_equal(born[2], 1918u);
    expect_equal(heights[1], 1.6);
    expect_equal(columns[1].validities[0], 0b011);
    expect_equal(util::tape_string(columns[2].strings, 0).length, 4u);

    read_columns_t read_columns;
    ustore_blobs_read_t read = read_of(database.handle(), blobs, keys, read_columns, arena.handle(), outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(read_columns.presences && tape_written(read_columns.values));
    expect_true(aligned(read_columns.presences) && aligned(read_columns.values.data.u32_tape.offsets) &&
                aligned(read_columns.values.data.u32_tape.data));

    ustore_key_t const lowest = key_from_integer(0);
    ustore_key_t const highest = key_from_integer(1000);
    std::array<ustore_size_t const*, 6> bounds {};
    ustore_blobs_measure_t measure {
        .database = database.handle(),
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
        .arena = arena.handle(),
    };
    ustore_blobs_measure(&measure);
    expect_success(outcome);
    for (ustore_size_t const* bound : bounds) expect_true(bound && aligned(bound));
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
void expect_only_outputs(ustore_arena_t arena, std::span<output_extent_t> outputs) {
    auto const address = [](void const* pointer) noexcept { return reinterpret_cast<std::uintptr_t>(pointer); };
    std::sort(outputs.begin(), outputs.end(), [&](output_extent_t const& left, output_extent_t const& right) noexcept {
        return address(left.begin) < address(right.begin);
    });
    std::size_t tiled = 0;
    for (util::arena_block_t const* block = util::host_blocks_of(arena)->newest; block; block = block->previous) {
        std::uintptr_t const block_begin = address(block);
        std::uintptr_t end = block_begin + sizeof(util::arena_block_t);
        for (output_extent_t const& output : outputs) {
            if (output.bytes == 0 || address(output.begin) < block_begin ||
                address(output.begin) >= block_begin + block->capacity)
                continue;
            expect_equal(address(output.begin),
                         util::round_up_to_multiple<std::uintptr_t>(end, util::arena_alignment_k));
            end = address(output.begin) + output.bytes;
            ++tiled;
        }
        expect_equal(end, block_begin + block->used);
    }
    expect_equal(tiled, static_cast<std::size_t>(
                            std::count_if(outputs.begin(), outputs.end(),
                                          [](output_extent_t const& output) noexcept { return output.bytes != 0; })));
}

/** Checks that the @p outputs of a one-off @p arena's first call outgrew its first block, which
 *  spans one page, into a second, and still tile both. */
void expect_outgrown_outputs(ustore_arena_t arena, std::span<output_extent_t> outputs) {
    util::arena_block_t const* const newest = util::host_blocks_of(arena)->newest;
    if (!expect_true(newest && newest->previous && !newest->previous->previous)) return;
    expect_equal(newest->previous->capacity, util::arena_page_bytes_k);
    expect_only_outputs(arena, outputs);
}

/** Every call stages what grows in scratch and copies its outputs in at their final lengths, so the
 *  arena it fills holds exactly its outputs, at the head and through a transaction alike: a docs
 *  gather, a graph read, a blob read, a docs read by field and a gist in an arena kept across them,
 *  then a collection list, a key scan, a strings scan, a gather of long strings and a read of many
 *  edges, each in a one-off arena its outputs outgrow. */
void test_arena_outputs_only(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "arena_outputs_only.blobs",
                                                   ustore_modality_blobs_k);
    expect_true(!blobs_status.failed());
    auto [arena, arena_status] = arena_t::make();
    expect_true(!arena_status.failed());
    auto [graph, graph_status] = create_collection(database.handle(), "outputs.graph", ustore_modality_graph_k);
    if (!expect_true(!graph_status.failed())) return;

    // Every document holds a long biography no output holds, which any intermediate left in the
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
    write.database = database.handle();
    write.outcome = &outcome;
    write.count = docs_count;
    write.collections = {&blobs, 0};
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values = values.view();
    ustore_blobs_write(&write);
    expect_success(outcome);
    auto [docs, docs_status] = create_collection(database.handle(), "outputs.docs", ustore_modality_docs_k);
    expect_true(!docs_status.failed());
    ustore_docs_write_t write_documents {
        .database = database.handle(),
        .outcome = &outcome,
        .count = docs_count,
        .collections = {&docs, 0},
        .keys = {keys.data(), sizeof(ustore_key_t)},
        .values = values.view(),
    };
    ustore_docs_write(&write_documents);
    expect_success(outcome);

    // Three edges between three vertices, and a fourth vertex no edge reaches, which
    // reads as missing
    std::array<ustore_key_t, 3> const sources {key_from_integer(1300), key_from_integer(1300), key_from_integer(1301)};
    std::array<ustore_key_t, 3> const targets {key_from_integer(1301), key_from_integer(1302), key_from_integer(1302)};
    ustore_graph_write_edges_t write_edges {
        .database = database.handle(),
        .outcome = &outcome,
        .count = sources.size(),
        .collections = {&graph, 0},
        .sources = {sources.data(), sizeof(ustore_key_t)},
        .targets = {targets.data(), sizeof(ustore_key_t)},
    };
    ustore_graph_write_edges(&write_edges);
    expect_success(outcome);
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
        .database = database.handle(),
        .outcome = &outcome,
        .count = fanned_count,
        .collections = {&graph, 0},
        .sources = {&hub, 0},
        .targets = {fanned.data(), sizeof(ustore_key_t)},
    };
    ustore_graph_write_edges(&write_fan);
    expect_success(outcome);
    std::array<ustore_key_t, many_count> scanned {};
    for (std::size_t index = 0; index != many_count; ++index) scanned[index] = key_from_integer(5000 + index);
    write_text(database.handle(), blobs, scanned, "value");
    auto [strings, strings_status] = create_collection(database.handle(), "outputs.strings", ustore_modality_strings_k);
    expect_true(!strings_status.failed());
    std::array<std::string, many_count> texts {};
    for (std::size_t index = 0; index != many_count; ++index) texts[index] = fmt::format("outputs.string.{:03}", index);
    strided_strings<std::string> const text_strings {strided(std::as_const(texts)).begin()};
    ustore_strings_write_t write_strings {
        .database = database.handle(),
        .outcome = &outcome,
        .count = many_count,
        .collections = {&strings, 0},
        .strings = text_strings.view(),
        .values = {scanned.data(), sizeof(ustore_key_t)},
    };
    ustore_strings_write(&write_strings);
    expect_success(outcome);
    std::string const long_name(5000, 'n');
    if (!expect_true(!create_collection(database.handle(), long_name, ustore_modality_blobs_k).status.failed())) return;

    // The listing exports every collection, earlier cases' included, the long name outgrowing
    // the first page
    {
        auto [one_off, one_off_status] = arena_t::make();
        expect_true(!one_off_status.failed());
        ustore_size_t written = 0;
        ustore_collection_id_t const* ids = nullptr;
        ustore_modality_t const* modalities = nullptr;
        ustore_strings_view_t names {};
        ustore_collection_list_t list {
            .database = database.handle(),
            .outcome = &outcome,
            .collections = &ids,
            .modalities = &modalities,
            .names = &names,
            .written = &written,
            .arena = one_off.handle(),
        };
        ustore_collection_list(&list);
        expect_success(outcome);
        if (!expect_true(written >= 3 && ids && modalities && tape_written(names))) return;
        std::array<output_extent_t, 2> const names_tape = tape_extents(names, written);
        std::array<output_extent_t, 4> listed {{
            {ids, written * sizeof(ustore_collection_id_t)},
            {modalities, written * sizeof(ustore_modality_t)},
            names_tape[0],
            names_tape[1],
        }};
        expect_outgrown_outputs(one_off.handle(), listed);
    }

    // An engine without transactions reads at the head alone
    ustore_transaction_t transaction = nullptr;
    ustore_transaction_init_t begin {.transaction = &transaction, .database = database.handle(), .outcome = &outcome};
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
            .database = database.handle(),
            .outcome = &outcome,
            .view = view,
            .count = docs_count,
            .collections = {&docs, 0},
            .keys = {keys.data(), sizeof(ustore_key_t)},
            .fields_count = fields.size(),
            .fields = fields_strings.view(),
            .columns = &columns,
            .arena = arena.handle(),
            .outputs_types = types.data(),
        };
        ustore_docs_gather(&gather);
        expect_success(outcome);
        expect_true(columns != nullptr);
        if (!columns) continue;
        ustore_docs_column_view_t const& born = columns[0];
        ustore_docs_column_view_t const& name = columns[1];
        auto const* const years = reinterpret_cast<std::uint64_t const*>(born.scalars);
        expect_equal(years[2], 1918u);
        expect_equal(std::string_view {reinterpret_cast<char const*>(util::tape_string(name.strings, 3).data)},
                     "edsger");
        std::size_t const bitmap_bytes = util::divide_round_up(docs_count, util::bits_in_byte_k);
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
        expect_only_outputs(arena.handle(), gathered);

        // The missing vertex spans no edges, so the offsets close at six
        ustore_vertex_degree_t const* degrees = nullptr;
        ustore_length_t const* edges_offsets = nullptr;
        ustore_key_t const* edges = nullptr;
        ustore_graph_read_edges_t read_edges {
            .database = database.handle(),
            .outcome = &outcome,
            .view = view,
            .count = vertices.size(),
            .collections = {&graph, 0},
            .vertices = {vertices.data(), sizeof(ustore_key_t)},
            .degrees_per_vertex = &degrees,
            .edges_offsets = &edges_offsets,
            .edges_per_vertex = &edges,
            .arena = arena.handle(),
        };
        ustore_graph_read_edges(&read_edges);
        expect_success(outcome);
        expect_true(degrees && edges_offsets && edges);
        if (!degrees || !edges_offsets || !edges) continue;
        expect_true(degrees[0] == 2 && degrees[1] == 2 && degrees[2] == 2);
        expect_equal(degrees[3], ustore_vertex_degree_missing_k);
        expect_true(edges_offsets[0] == 0 && edges_offsets[1] == 2 && edges_offsets[2] == 4);
        expect_true(edges_offsets[3] == 6 && edges_offsets[4] == 6);
        std::array<output_extent_t, 3> read {{
            {degrees, vertices.size() * sizeof(ustore_vertex_degree_t)},
            {edges_offsets, (vertices.size() + 1) * sizeof(ustore_length_t)},
            {edges, 6 * 3 * sizeof(ustore_key_t)},
        }};
        expect_only_outputs(arena.handle(), read);

        // A blob read copies every document once, however long, and leaves no room for
        // more behind them
        read_columns_t blob_reads;
        ustore_blobs_read_t read_blobs = read_of(database.handle(), blobs, keys, blob_reads, arena.handle(), outcome);
        read_blobs.view = view;
        ustore_blobs_read(&read_blobs);
        expect_success(outcome);
        expect_true(blob_reads.presences && tape_written(blob_reads.values));
        if (!blob_reads.presences || !tape_written(blob_reads.values)) continue;
        expect_equal(blob_reads.value(3), documents[3]);
        std::array<output_extent_t, 2> const blobs_tape = tape_extents(blob_reads.values, docs_count);
        std::array<output_extent_t, 3> blob_columns {{
            {blob_reads.presences, bitmap_bytes},
            blobs_tape[0],
            blobs_tape[1],
        }};
        expect_only_outputs(arena.handle(), blob_columns);

        // A read by field exports the names alone, the third document missing one
        read_columns_t names;
        std::string_view const name_field = "name";
        strided_strings<std::string_view> const name_fields {{&name_field, 0}};
        ustore_docs_read_t read_names {
            .database = database.handle(),
            .outcome = &outcome,
            .view = view,
            .count = docs_count,
            .collections = {&docs, 0},
            .keys = {keys.data(), sizeof(ustore_key_t)},
            .fields = name_fields.view(),
            .presences = &names.presences,
            .values = &names.values,
            .arena = arena.handle(),
            .outputs_type = ustore_docs_field_type_string_k,
        };
        ustore_docs_read(&read_names);
        expect_success(outcome);
        expect_true(names.presences && tape_written(names.values));
        if (!names.presences || !tape_written(names.values)) continue;
        expect_equal(names.presences[0], 0b1011);
        expect_equal(names.value(1), "grace");
        expect_equal(names.value(3), "edsger");
        std::array<output_extent_t, 2> const names_tape = tape_extents(names.values, docs_count);
        expect_equal(names_tape[1].bytes, std::string_view {"adagraceedsger"}.size());
        std::array<output_extent_t, 3> name_columns {{
            {names.presences, bitmap_bytes},
            names_tape[0],
            names_tape[1],
        }};
        expect_only_outputs(arena.handle(), name_columns);

        // A gist finds three paths, which leave no partial copies of their offsets or
        // of themselves behind
        ustore_size_t written = 0;
        ustore_strings_view_t paths {};
        ustore_docs_gist_t gist {
            .database = database.handle(),
            .outcome = &outcome,
            .view = view,
            .count = docs_count,
            .collections = {&docs, 0},
            .keys = {keys.data(), sizeof(ustore_key_t)},
            .fields = &paths,
            .written = &written,
            .arena = arena.handle(),
        };
        ustore_docs_gist(&gist);
        expect_success(outcome);
        expect_equal(written, 3u);
        expect_true(tape_written(paths));
        if (!tape_written(paths)) continue;
        expect_equal(util::chars_of(util::tape_string(paths, 1)), "/name");
        std::array<output_extent_t, 2> gist_columns = tape_extents(paths, written);
        expect_only_outputs(arena.handle(), gist_columns);

        // Three hundred keys outgrow the first page of a one-off arena, which only the offsets fit
        {
            auto [one_off, one_off_status] = arena_t::make();
            expect_true(!one_off_status.failed());
            ustore_length_t const* offsets = nullptr;
            ustore_key_t const* found = nullptr;
            ustore_blobs_scan_t scan {
                .database = database.handle(),
                .outcome = &outcome,
                .view = view,
                .count = 1,
                .collections = {&blobs, 0},
                .start_keys = {&scanned[0], 0},
                .offsets = &offsets,
                .keys = &found,
                .arena = one_off.handle(),
                .wanted = many_count,
            };
            ustore_blobs_scan(&scan);
            expect_success(outcome);
            expect_true(offsets && found);
            if (!offsets || !found) continue;
            expect_equal(offsets[1], many_count);
            expect_true(std::memcmp(found, scanned.data(), sizeof(scanned)) == 0);
            std::array<output_extent_t, 2> scan_columns {{
                {offsets, 2 * sizeof(ustore_length_t)},
                {found, many_count * sizeof(ustore_key_t)},
            }};
            expect_outgrown_outputs(one_off.handle(), scan_columns);
        }

        // Three hundred strings outgrow it too, after their offsets
        {
            auto [one_off, one_off_status] = arena_t::make();
            expect_true(!one_off_status.failed());
            ustore_length_t const* offsets = nullptr;
            ustore_strings_view_t found {};
            ustore_strings_scan_t scan {
                .database = database.handle(),
                .outcome = &outcome,
                .view = view,
                .count = 1,
                .collections = {&strings, 0},
                .offsets = &offsets,
                .strings = &found,
                .arena = one_off.handle(),
                .wanted = many_count,
            };
            ustore_strings_scan(&scan);
            expect_success(outcome);
            expect_true(offsets && tape_written(found));
            if (!offsets || !tape_written(found)) continue;
            expect_equal(offsets[1], many_count);
            expect_equal(util::chars_of(util::tape_string(found, many_count - 1)), texts[many_count - 1]);
            std::array<output_extent_t, 2> const found_tape = tape_extents(found, many_count);
            std::array<output_extent_t, 3> scan_columns {{
                {offsets, 2 * sizeof(ustore_length_t)},
                found_tape[0],
                found_tape[1],
            }};
            expect_outgrown_outputs(one_off.handle(), scan_columns);
        }

        // Gathering the biographies twice, the second string column outgrows it
        {
            auto [one_off, one_off_status] = arena_t::make();
            expect_true(!one_off_status.failed());
            std::string_view const biography = "bio";
            strided_strings<std::string_view> const biographies {{&biography, 0}};
            std::array<ustore_docs_field_type_t, 2> const text_types {ustore_docs_field_type_string_k,
                                                                      ustore_docs_field_type_string_k};
            ustore_docs_column_view_t const* columns = nullptr;
            ustore_docs_gather_t gather {
                .database = database.handle(),
                .outcome = &outcome,
                .view = view,
                .count = docs_count,
                .collections = {&docs, 0},
                .keys = {keys.data(), sizeof(ustore_key_t)},
                .fields_count = text_types.size(),
                .fields = biographies.view(),
                .columns = &columns,
                .arena = one_off.handle(),
                .outputs_types = text_types.data(),
            };
            ustore_docs_gather(&gather);
            expect_success(outcome);
            expect_true(columns != nullptr);
            if (!columns) continue;
            std::size_t const bitmap_bytes = util::divide_round_up(docs_count, util::bits_in_byte_k);
            std::array<output_extent_t, 11> gathered {};
            gathered[0] = {columns, text_types.size() * sizeof(ustore_docs_column_view_t)};
            for (std::size_t field = 0; field != text_types.size(); ++field) {
                ustore_docs_column_view_t const& column = columns[field];
                std::array<output_extent_t, 2> const column_tape = tape_extents(column.strings, docs_count);
                expect_equal(column_tape[1].bytes, docs_count * 601);
                gathered[1 + 5 * field] = {column.validities, bitmap_bytes};
                gathered[2 + 5 * field] = {column.conversions, bitmap_bytes};
                gathered[3 + 5 * field] = {column.collisions, bitmap_bytes};
                gathered[4 + 5 * field] = column_tape[0];
                gathered[5 + 5 * field] = column_tape[1];
            }
            expect_outgrown_outputs(one_off.handle(), gathered);
        }

        // A hundred edges of the hub outgrow it after the degree and the offsets
        {
            auto [one_off, one_off_status] = arena_t::make();
            expect_true(!one_off_status.failed());
            ustore_vertex_degree_t const* degrees = nullptr;
            ustore_length_t const* edges_offsets = nullptr;
            ustore_key_t const* edges = nullptr;
            ustore_graph_read_edges_t read_fan {
                .database = database.handle(),
                .outcome = &outcome,
                .view = view,
                .count = 1,
                .collections = {&graph, 0},
                .vertices = {&hub, 0},
                .degrees_per_vertex = &degrees,
                .edges_offsets = &edges_offsets,
                .edges_per_vertex = &edges,
                .arena = one_off.handle(),
            };
            ustore_graph_read_edges(&read_fan);
            expect_success(outcome);
            expect_true(degrees && edges_offsets && edges);
            if (!degrees || !edges_offsets || !edges) continue;
            expect_equal(degrees[0], fanned_count);
            std::array<output_extent_t, 3> fan_columns {{
                {degrees, sizeof(ustore_vertex_degree_t)},
                {edges_offsets, 2 * sizeof(ustore_length_t)},
                {edges, fanned_count * 3 * sizeof(ustore_key_t)},
            }};
            expect_outgrown_outputs(one_off.handle(), fan_columns);
        }
    }
    ustore_transaction_free(transaction);
}

#pragma endregion Arenas

#pragma region Deferred Calls

/** A deferred write and then a deferred read into an arena the executor made round-trip the values,
 *  and a blocking read into an arena made for the calling thread reads them back as well. */
void test_executor_deferred_round_trip(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "executor_deferred_round_trip.blobs",
                                                   ustore_modality_blobs_k);
    expect_true(!blobs_status.failed());
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!spawn_status.failed())) return;
    auto [executor_arena, executor_arena_status] = arena_t::make({.executor = executor});
    auto [calling_arena, calling_arena_status] = arena_t::make();
    expect_true(!executor_arena_status.failed() && !calling_arena_status.failed());

    std::array<ustore_key_t, 3> const keys {key_from_integer(71), key_from_integer(72), key_from_integer(73)};
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database.handle();
    write.executor = executor;
    write.outcome = &outcome;
    write.completion = ustore_completion_deferred_k;
    write.count = keys.size();
    write.collections = {&blobs, 0};
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values = stored_values_k.view();
    ustore_blobs_write(&write);
    expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);

    read_columns_t deferred_columns;
    ustore_blobs_read_t deferred_read = read_of(database.handle(), blobs, keys, deferred_columns,
                                                executor_arena.handle(), outcome);
    deferred_read.executor = executor;
    deferred_read.completion = ustore_completion_deferred_k;
    ustore_blobs_read(&deferred_read);
    expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    read_columns_t blocking_columns;
    ustore_blobs_read_t blocking_read = read_of(database.handle(), blobs, keys, blocking_columns,
                                                calling_arena.handle(), outcome);
    ustore_blobs_read(&blocking_read);
    expect_success(outcome);

    for (read_columns_t const& columns : {deferred_columns, blocking_columns}) {
        expect_true(columns.presences && tape_written(columns.values));
        if (!columns.presences || !tape_written(columns.values)) continue;
        expect_equal(columns.presences[0], 0b111);
        for (std::size_t index = 0; index != keys.size(); ++index) expect_equal(columns.value(index), "value");
    }
    executor_arena = {};
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** A deferred call runs from the copy its submission took, so the caller may scribble over its
 *  argument struct at once and the library never writes it back, while polling through
 *  @c ustore_outcome_status reads the call deferred until it is final. */
void test_executor_copied_submission(config_t const&) {
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!spawn_status.failed())) return;

    // The scan holds inside its callback, so it is still running when its struct is scribbled over
    held_scan_t held;
    submit_held(held, executor);
    std::memset(&held.scan, 0xA5, sizeof(held.scan));
    expect_true(ustore_outcome_status(&held.outcome) == ustore_status_deferred_k);
    release_held(held);
    auto const* const scribbled = reinterpret_cast<unsigned char const*>(&held.scan);
    expect_true(std::all_of(scribbled, scribbled + sizeof(held.scan), [](unsigned char byte) { return byte == 0xA5; }));
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
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
void test_arena_free_waits(config_t const&) {
    auto [executor, spawn_status] = spawn_executor();
    fu::topology_t topology;
    fu::pool_t freeing_thread;
    fu::status_t const thread_status = fu::failed(topology.harvest())
                                           ? fu::status_t::topology_unavailable_k
                                           : freeing_thread.spawn(topology, 1, fu::caller_exclusive_k);
    if (!expect_true(!spawn_status.failed() && !fu::failed(thread_status))) return;

    held_scan_t held;
    submit_held(held, executor);
    arena_freeing_t freeing {.arena = std::move(held.arena), .stage = &held.stage};
    fu_generation_t generation = 0;
    fu::status_t const start_status = freeing_thread.unsafe_for_threads(
        [](fu_lambda_context_t context, std::size_t, std::size_t) noexcept {
            free_arena(*static_cast<arena_freeing_t*>(context));
        },
        &freeing, generation);
    expect_true(!fu::failed(start_status));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    expect_true(fu::standard_atomic_ref<std::uint32_t> {freeing.freed}.load(std::memory_order_acquire) == 0);

    fu::standard_atomic_ref<hold_stage_t> {held.stage}.store(hold_stage_t::released_k, std::memory_order_release);
    if (!fu::failed(start_status)) freeing_thread.unsafe_join(generation);
    else free_arena(freeing);
    expect_true(freeing.stage_after_free == hold_stage_t::returned_k);
    expect_true(ustore_outcome_wait(&held.outcome) == ustore_status_success_k);
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** A call on an executor another call holds finishes contended at once, deferred or blocking, and a
 *  drain from inside a callback of the holding call is refused rather than waiting. */
void test_executor_held_contended(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "executor_held_contended.blobs",
                                                   ustore_modality_blobs_k);
    expect_true(!blobs_status.failed());
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!spawn_status.failed())) return;

    held_scan_t held;
    submit_held(held, executor);
    ustore_key_t const key = key_from_integer(74);
    ustore_outcome_t outcome {};
    submit_writes(database.handle(), blobs, executor, {&key, 1}, {&outcome, 1});
    expect_status(outcome, status_code_t::contended_k);
    expect_true(write_blocking(database.handle(), blobs, key, executor) == ustore_status_contended_k);
    expect_true(held.drained == ustore_status_contended_k);
    release_held(held);
    expect_true(write_blocking(database.handle(), blobs, key, executor) == ustore_status_success_k);
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** Runs the blocking scan @p held is armed with on the calling thread, which its callback holds. */
void scan_blocking(held_scan_t& held) noexcept { ustore_blobs_scan(&held.scan); }

/** A blocking call holds its executor from its caller's thread, so a call from another thread
 *  finishes contended until the holder returns, after which the executor serves it. */
void test_executor_contended_from_another_thread(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "executor_contended_from_another_thread.blobs",
                                                   ustore_modality_blobs_k);
    auto [executor, spawn_status] = spawn_executor();
    fu::topology_t topology;
    fu::pool_t holding_thread;
    fu::status_t const thread_status = fu::failed(topology.harvest())
                                           ? fu::status_t::topology_unavailable_k
                                           : holding_thread.spawn(topology, 1, fu::caller_exclusive_k);
    if (!expect_true(!blobs_status.failed() && !spawn_status.failed() && !fu::failed(thread_status))) return;

    held_scan_t held;
    arm_held(held, executor, nullptr, ustore_completion_blocking_k);
    fu_generation_t generation = 0;
    fu::status_t const start_status = holding_thread.unsafe_for_threads(
        [](fu_lambda_context_t context, std::size_t, std::size_t) noexcept {
            scan_blocking(*static_cast<held_scan_t*>(context));
        },
        &held, generation);
    if (!expect_true(!fu::failed(start_status))) return;
    wait_held(held);
    ustore_key_t const key = key_from_integer(1300);
    ustore_outcome_t outcome {};
    submit_writes(database.handle(), blobs, executor, {&key, 1}, {&outcome, 1});
    expect_status(outcome, status_code_t::contended_k);
    expect_true(held.drained == ustore_status_contended_k);
    fu::standard_atomic_ref<hold_stage_t> {held.stage}.store(hold_stage_t::released_k, std::memory_order_release);
    holding_thread.unsafe_join(generation);
    expect_success(held.outcome);
    submit_writes(database.handle(), blobs, executor, {&key, 1}, {&outcome, 1});
    expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** A deferred call on a caller-exclusive executor finishes while its caller only polls its status,
 *  as the executor's thread 0 serves it, and a blocking call afterwards sees what it wrote. */
void test_executor_finishes_by_polling(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "executor_finishes_by_polling.blobs",
                                                   ustore_modality_blobs_k);
    expect_true(!blobs_status.failed());
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!spawn_status.failed())) return;
    auto [arena, arena_status] = arena_t::make({.executor = executor});
    expect_true(!arena_status.failed());

    std::array<ustore_key_t, 3> const keys {key_from_integer(75), key_from_integer(76), key_from_integer(77)};
    ustore_outcome_t written {};
    ustore_blobs_write_t write {};
    write.database = database.handle();
    write.executor = executor;
    write.outcome = &written;
    write.completion = ustore_completion_deferred_k;
    write.count = keys.size();
    write.collections = {&blobs, 0};
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values = stored_values_k.view();
    ustore_blobs_write(&write);
    expect_true(
        spin_until([&written]() noexcept { return ustore_outcome_status(&written) != ustore_status_deferred_k; }));
    expect_success(written);

    read_columns_t columns;
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database.handle(), blobs, keys, columns, arena.handle(), outcome);
    read.executor = executor;
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(columns.presences && columns.presences[0] == 0b111);
    arena = {};
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** A caller that sees a deferred call final may submit the next one at once, naming the same arena,
 *  since a call releases its arena and its executor before its status turns final. */
void test_executor_submit_after_wait(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "executor_submit_after_wait.blobs",
                                                   ustore_modality_blobs_k);
    expect_true(!blobs_status.failed());
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!spawn_status.failed())) return;
    auto [arena, arena_status] = arena_t::make({.executor = executor});
    expect_true(!arena_status.failed());

    ustore_key_t const key = key_from_integer(900);
    write_text(database.handle(), blobs, {&key, 1}, "value");
    std::size_t contended = 0;
    std::size_t failed = 0;
    for (std::size_t round = 0; round != 1000; ++round) {
        read_columns_t columns;
        ustore_outcome_t outcome {};
        ustore_blobs_read_t read = read_of(database.handle(), blobs, {&key, 1}, columns, arena.handle(), outcome);
        read.executor = executor;
        read.completion = ustore_completion_deferred_k;
        ustore_blobs_read(&read);
        ustore_status_t const status = ustore_outcome_wait(&outcome);
        contended += status == ustore_status_contended_k;
        failed += status != ustore_status_success_k;
    }
    expect_equal(contended, 0u);
    expect_equal(failed, 0u);
    arena = {};
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** A drain waits for the call holding the executor, then applies its mood: after a parked drain the
 *  next call wakes the threads, and after a spinning one it runs at once. */
void test_executor_drain(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "executor_drain.blobs", ustore_modality_blobs_k);
    expect_true(!blobs_status.failed());
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!spawn_status.failed())) return;

    ustore_outcome_t outcome {};
    ustore_executor_drain_t drain {.executor = executor, .outcome = &outcome};
    for (ustore_mood_t const mood : {ustore_mood_parked_k, ustore_mood_spinning_k, ustore_mood_parked_k}) {
        ustore_key_t const key = key_from_integer(78 + mood);
        ustore_outcome_t written {};
        submit_writes(database.handle(), blobs, executor, {&key, 1}, {&written, 1});
        drain.mood = mood;
        ustore_executor_drain(&drain);
        expect_success(outcome);
        expect_true(ustore_outcome_wait(&written) == ustore_status_success_k);
    }
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** A host executor drives no stream, so naming, following or checking one is unsupported, while
 *  missing arguments are refused first. */
void test_executor_streams_on_host(config_t const&) {
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!spawn_status.failed())) return;
    ustore_outcome_t outcome {};
    void* stream = &outcome;
    ustore_executor_stream_t named {.executor = executor, .outcome = &outcome, .stream = &stream};
    ustore_executor_stream(&named);
    expect_status(outcome, status_code_t::unsupported_k);
    expect_true(stream == nullptr);
    named.executor = nullptr;
    ustore_executor_stream(&named);
    expect_status(outcome, status_code_t::invalid_argument_k);
    ustore_executor_follow_t follow {.executor = executor, .stream = &outcome, .outcome = &outcome};
    ustore_executor_follow(&follow);
    expect_status(outcome, status_code_t::unsupported_k);
    follow.stream = nullptr;
    ustore_executor_follow(&follow);
    expect_status(outcome, status_code_t::invalid_argument_k);
    ustore_executor_accepts_t accepts {.executor = executor, .stream = &outcome, .outcome = &outcome};
    ustore_executor_accepts(&accepts);
    expect_status(outcome, status_code_t::unsupported_k);
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** A call naming an arena an unfinished call holds finishes contended, and so does one on a
 *  transaction it reads through, each on the calling thread, until the holding call ends. */
void test_executor_contended_arena(config_t const&) {
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!spawn_status.failed())) return;

    held_scan_t held;
    ustore_transaction_handle_t transaction {.engine = &progress_engine_k, .database = &held.database};
    submit_held(held, executor, ustore_transaction_view(&transaction, ustore_watch_keys_k));
    ustore_outcome_t outcome {};
    ustore_blobs_scan_t on_arena = held.scan;
    on_arena.executor = nullptr;
    on_arena.view = nullptr;
    on_arena.outcome = &outcome;
    on_arena.progress = nullptr;
    on_arena.progress_state = nullptr;
    ustore_blobs_scan(&on_arena);
    expect_status(outcome, status_code_t::contended_k);
    ustore_blobs_scan_t on_transaction = on_arena;
    on_transaction.arena = nullptr;
    on_transaction.view = ustore_transaction_view(&transaction, ustore_watch_keys_k);
    auto [own_arena, own_arena_status] = arena_t::make();
    expect_true(!own_arena_status.failed());
    on_transaction.arena = own_arena.handle();
    ustore_blobs_scan(&on_transaction);
    expect_status(outcome, status_code_t::contended_k);

    release_held(held);
    ustore_blobs_scan(&on_arena);
    expect_success(outcome);
    ustore_blobs_scan(&on_transaction);
    expect_success(outcome);
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** A call on a host executor given an arena on a device is refused with @c ustore_status_device_k,
 *  and served once the arena lives on host pages again. A build without CUDA makes no device arena,
 *  so the case moves one by hand. */
void test_executor_arena_on_device(config_t const&) {
    ustore_database_handle_t database {&progress_engine_k};
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return;

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
        .arena = arena.handle(),
        .wanted = 1,
    };
    ustore_arena_t const placed = arena.handle();
    placed->memory = cuda_blocks_t {.device = 0};
    ustore_blobs_scan(&scan);
    expect_status(outcome, status_code_t::device_k);

    placed->memory = host_blocks_t {};
    ustore_blobs_scan(&scan);
    expect_success(outcome);
}

/** Waiting on a deferred call blocks until its status is final, past the moment
 *  its callback returns. */
void test_outcome_wait_blocks(config_t const&) {
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!spawn_status.failed())) return;

    held_scan_t held;
    submit_held(held, executor);
    fu::standard_atomic_ref<hold_stage_t> const stage {held.stage};
    stage.store(hold_stage_t::released_k, std::memory_order_release);
    expect_true(ustore_outcome_wait(&held.outcome) == ustore_status_success_k);
    expect_true(stage.load(std::memory_order_acquire) == hold_stage_t::returned_k);
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** Freeing a database waits for the deferred call naming it, past the moment its callback returns,
 *  though the status of that call may turn final after the free, as it lowers its counts first. */
void test_database_free_waits(config_t const&) {
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!spawn_status.failed())) return;

    held_scan_t held;
    submit_held(held, executor);
    fu::standard_atomic_ref<hold_stage_t> const stage {held.stage};
    stage.store(hold_stage_t::released_k, std::memory_order_release);
    // A second hold keeps the router from deleting the case's own handle once the
    // free stops waiting
    held.database.holds = 2;
    ustore_database_free(&held.database);
    expect_true(stage.load(std::memory_order_acquire) == hold_stage_t::returned_k);
    expect_true(ustore_outcome_wait(&held.outcome) == ustore_status_success_k);
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** Freeing a database waits for a call naming it on no executor, held inside its callback on
 *  another thread, as such a call counts on its handles as an executor call does. */
void test_database_free_waits_without_executor(config_t const&) {
    fu::topology_t topology;
    fu::pool_t holding_thread;
    fu::status_t const thread_status = fu::failed(topology.harvest())
                                           ? fu::status_t::topology_unavailable_k
                                           : holding_thread.spawn(topology, 1, fu::caller_exclusive_k);
    if (!expect_true(!fu::failed(thread_status))) return;

    held_scan_t held;
    arm_held(held, nullptr, nullptr, ustore_completion_blocking_k);
    fu_generation_t generation = 0;
    fu::status_t const start_status = holding_thread.unsafe_for_threads(
        [](fu_lambda_context_t context, std::size_t, std::size_t) noexcept {
            scan_blocking(*static_cast<held_scan_t*>(context));
        },
        &held, generation);
    if (!expect_true(!fu::failed(start_status))) return;
    wait_held(held);
    fu::standard_atomic_ref<hold_stage_t> const stage {held.stage};
    stage.store(hold_stage_t::released_k, std::memory_order_release);
    // A second hold keeps the router from deleting the case's own handle once the
    // free stops waiting
    held.database.holds = 2;
    ustore_database_free(&held.database);
    expect_true(stage.load(std::memory_order_acquire) == hold_stage_t::returned_k);
    holding_thread.unsafe_join(generation);
    expect_success(held.outcome);
}

/** An init runs on an executor as every call does: a blocking one returns with the database open,
 *  and a deferred one runs from its copy, whose original died with the frame that built it, and
 *  writes the database before its outcome is final. */
case_result_t test_database_init_on_executor(config_t const& config) {
    auto [executor, spawn_status] = spawn_executor();
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!spawn_status.failed() && !arena_status.failed())) return {};

    ustore_database_t database = nullptr;
    ustore_outcome_t outcome {};
    open_engine(config.options(), executor, &database, &outcome, ustore_completion_blocking_k);
    expect_success(outcome);
    expect_true(database != nullptr);
    auto [blobs, blobs_status] = create_collection(database, "database_init_on_executor.blobs",
                                                   ustore_modality_blobs_k);
    expect_true(!blobs_status.failed());
    ustore_key_t const key = key_from_integer(1200);
    if (database) {
        write_text(database, blobs, {&key, 1}, "value");
        ustore_database_flush_t flush {.database = database, .executor = executor, .outcome = &outcome};
        ustore_database_flush(&flush);
        expect_success(outcome);
    }
    ustore_database_free(database);

    database = nullptr;
    open_engine(config.options(), executor, &database, &outcome, ustore_completion_deferred_k);
    expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    expect_true(database != nullptr);
    if (database) {
        read_columns_t columns;
        ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena.handle(), outcome);
        ustore_blobs_read(&read);
        expect_success(outcome);
        expect_true(tape_written(columns.values) && columns.value(0) == "value");
    }
    ustore_database_free(database);
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
    return {};
}

/** A read of more keys than one word of tasks, and a scan of as many tasks, split over the
 *  executor's threads, blocking or deferred, and match byte for byte what the same calls hand back
 *  on the calling thread. */
void test_executor_sliced_read(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "executor_sliced_read.blobs",
                                                   ustore_modality_blobs_k);
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!blobs_status.failed() && !spawn_status.failed())) return;

    constexpr std::size_t keys_k = 300;
    std::array<ustore_key_t, keys_k> keys {};
    for (std::size_t index = 0; index != keys_k; ++index) {
        keys[index] = key_from_integer(5000 + index);
        if (index % 3 == 0) continue;
        std::array<char, 32> text {};
        std::size_t const length = fmt::format_to_n(text.data(), text.size(), "value-{}", index).size;
        write_text(database.handle(), blobs, {&keys[index], 1}, {text.data(), length});
    }

    auto [alone_arena, alone_arena_status] = arena_t::make();
    expect_true(!alone_arena_status.failed());
    read_columns_t alone;
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database.handle(), blobs, keys, alone, alone_arena.handle(), outcome);
    ustore_blobs_read(&read);
    if (!expect_success(outcome)) return;

    for (ustore_completion_t const completion : {ustore_completion_blocking_k, ustore_completion_deferred_k}) {
        auto [arena, arena_status] = arena_t::make({.executor = executor});
        expect_true(!arena_status.failed());
        read_columns_t sliced;
        ustore_blobs_read_t on_executor = read_of(database.handle(), blobs, keys, sliced, arena.handle(), outcome);
        on_executor.executor = executor;
        on_executor.completion = completion;
        ustore_blobs_read(&on_executor);
        expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
        if (!sliced.presences || !tape_written(sliced.values)) {
            expect_true(false);
            continue;
        }
        expect_true(std::memcmp(alone.presences, sliced.presences, (keys_k + 7) / 8) == 0);
        for (std::size_t index = 0; index != keys_k; ++index) expect_equal(sliced.value(index), alone.value(index));
    }

    // One scan task per key, each listing the next two keys from it, through the same split
    ustore_length_t const* alone_offsets = nullptr;
    ustore_key_t const* alone_keys = nullptr;
    ustore_blobs_scan_t scan {.database = database.handle(),
                              .outcome = &outcome,
                              .count = keys_k,
                              .collections = {&blobs, 0},
                              .start_keys = {keys.data(), sizeof(ustore_key_t)},
                              .offsets = &alone_offsets,
                              .keys = &alone_keys,
                              .arena = alone_arena.handle(),
                              .wanted = 2};
    ustore_blobs_scan(&scan);
    expect_success(outcome);
    for (ustore_completion_t const completion : {ustore_completion_blocking_k, ustore_completion_deferred_k}) {
        auto [arena, arena_status] = arena_t::make({.executor = executor});
        expect_true(!arena_status.failed());
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* found = nullptr;
        ustore_blobs_scan_t on_executor = scan;
        on_executor.executor = executor, on_executor.completion = completion;
        on_executor.offsets = &offsets, on_executor.keys = &found, on_executor.arena = arena.handle();
        ustore_blobs_scan(&on_executor);
        expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
        if (!offsets || !alone_offsets) {
            expect_true(false);
            continue;
        }
        expect_true(std::memcmp(offsets, alone_offsets, (keys_k + 1) * sizeof(ustore_length_t)) == 0);
        expect_true(std::memcmp(found, alone_keys, alone_offsets[keys_k] * sizeof(ustore_key_t)) == 0);
    }
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** A read of one field and a gather of one column over more documents than one word of tasks
 *  split over the executor's threads and match byte for byte what they hand back without one. */
void test_executor_sliced_docs(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [docs, docs_status] = create_collection(database.handle(), "executor_sliced_docs.docs",
                                                 ustore_modality_docs_k);
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!docs_status.failed() && !spawn_status.failed())) return;

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
    ustore_docs_write_t write {.database = database.handle(),
                               .outcome = &outcome,
                               .count = written,
                               .collections = {&docs, 0},
                               .keys = {written_keys.data(), sizeof(ustore_key_t)},
                               .values = util::u32_tape(texts.data(), offsets.data())};
    ustore_docs_write(&write);
    expect_success(outcome);

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
        ustore_docs_read_t read {.database = database.handle(),
                                 .executor = on,
                                 .outcome = &outcome,
                                 .completion = completion,
                                 .count = docs_k,
                                 .collections = {&docs, 0},
                                 .keys = {keys.data(), sizeof(ustore_key_t)},
                                 .fields = util::u32_tape(field_texts.data(), field_offsets.data()),
                                 .presences = &outputs.presences,
                                 .values = &outputs.values,
                                 .arena = read_arena,
                                 .outputs_type = ustore_docs_field_type_string_k};
        ustore_docs_read(&read);
        bool const read_held = expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
        ustore_docs_gather_t gather {.database = database.handle(),
                                     .executor = on,
                                     .outcome = &outcome,
                                     .completion = completion,
                                     .count = docs_k,
                                     .collections = {&docs, 0},
                                     .keys = {keys.data(), sizeof(ustore_key_t)},
                                     .fields_count = 1,
                                     .fields = util::u32_tape(field_texts.data(), field_offsets.data()),
                                     .columns = &outputs.columns,
                                     .arena = gather_arena,
                                     .outputs_types = &i32_type};
        ustore_docs_gather(&gather);
        return expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k) && read_held;
    };

    auto [alone_read_arena, alone_read_status] = arena_t::make();
    auto [alone_gather_arena, alone_gather_status] = arena_t::make();
    bool const arenas_made = expect_true(!alone_read_status.failed() && !alone_gather_status.failed());
    docs_outputs_t alone;
    bool const gathered = read_and_gather(nullptr, ustore_completion_blocking_k, alone_read_arena.handle(),
                                          alone_gather_arena.handle(), alone);
    if (!expect_true(arenas_made && gathered && alone.presences && alone.columns)) return;
    for (ustore_completion_t const completion : {ustore_completion_blocking_k, ustore_completion_deferred_k}) {
        auto [read_arena, read_status] = arena_t::make({.executor = executor});
        auto [gather_arena, gather_status] = arena_t::make({.executor = executor});
        expect_true(!read_status.failed() && !gather_status.failed());
        docs_outputs_t sliced;
        read_and_gather(executor, completion, read_arena.handle(), gather_arena.handle(), sliced);
        if (!sliced.presences || !sliced.columns) {
            expect_true(false);
            continue;
        }
        expect_true(std::memcmp(alone.presences, sliced.presences, (docs_k + 7) / 8) == 0);
        for (std::size_t index = 0; index != docs_k; ++index)
            expect_equal(util::chars_of(util::tape_string(sliced.values, index)),
                         util::chars_of(util::tape_string(alone.values, index)));
        expect_true(std::memcmp(alone.columns[0].validities, sliced.columns[0].validities, (docs_k + 7) / 8) == 0);
        expect_true(std::memcmp(alone.columns[0].scalars, sliced.columns[0].scalars, docs_k * sizeof(std::int32_t)) ==
                    0);
    }
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** An ingest named on a queue set of four threads keeps its state on the queue set's memory
 *  domain and parses an append of more words of documents than threads on every thread of
 *  the queue set. */
case_result_t test_executor_sliced_ingest(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    auto [docs, docs_status] = create_collection(database.handle(), "executor_sliced_ingest.docs",
                                                 ustore_modality_docs_k);
    auto [executor, spawn_status] = spawn_executor(4);
    if (!expect_true(docs_status.succeeded() && spawn_status.succeeded())) return {};
    ustore_outcome_t outcome {};

    ustore_ingest_t ingest = nullptr;
    ustore_ingest_init_t init {.ingest = &ingest,
                               .database = database.handle(),
                               .executor = executor,
                               .outcome = &outcome,
                               .collection = docs,
                               .order = ustore_ingest_sorted_k};
    ustore_ingest_init(&init);
    if (outcome.status == ustore_status_unsupported_k) {
        expect_true(ustore_executor_free(executor) == ustore_status_success_k);
        return case_result_t::skipped("needs an engine that ingests");
    }
    if (!expect_success(outcome)) return {};
    util::host_blocks_t const* const kept = std::get_if<util::host_blocks_t>(&ingest->kept.memory);
    expect_true(kept && kept->memory_domain == util::host_blocks_of(&executor->scratches.back())->memory_domain);

    constexpr std::size_t docs_k = 8 * util::tasks_per_word_k;
    std::array<ustore_key_t, docs_k> keys {};
    std::array<char, docs_k * 16> texts {};
    std::array<std::uint32_t, docs_k + 1> offsets {};
    for (std::size_t index = 0; index != docs_k; ++index) {
        keys[index] = key_from_integer(9000 + index);
        std::size_t const length = fmt::format_to_n(texts.data() + offsets[index], 16, "{{\"age\":{}}}", index).size;
        offsets[index + 1] = static_cast<std::uint32_t>(offsets[index] + length);
    }
    ustore_ingest_append_t append {.ingest = ingest,
                                   .executor = executor,
                                   .outcome = &outcome,
                                   .completion = ustore_completion_deferred_k,
                                   .count = docs_k,
                                   .keys = {keys.data(), sizeof(ustore_key_t)},
                                   .values = util::u32_tape(texts.data(), offsets.data())};
    ustore_ingest_append(&append);
    expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    for (std::size_t thread = 0; thread != 4; ++thread)
        expect_true(std::get_if<util::host_blocks_t>(&executor->scratches[thread].memory)->newest != nullptr);

    ustore_stamp_t stamp = 0;
    ustore_ingest_finish_t finish {.ingest = ingest,
                                   .executor = executor,
                                   .outcome = &outcome,
                                   .completion = ustore_completion_deferred_k,
                                   .commit_stamp = &stamp};
    ustore_ingest_finish(&finish);
    expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    expect_true(stamp != 0);
    ustore_ingest_free(ingest);
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
    return {};
}

/** A read whose keys lie in the arena it would rewind is refused before it rewinds them, while the
 *  same keys copied out read back. */
void test_arena_refuses_its_own_inputs(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "arena_inputs.blobs", ustore_modality_blobs_k);
    std::array<ustore_key_t, 8> keys {};
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(index);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(blobs_status.succeeded() && arena_status.succeeded()) ||
        !write_text(database.handle(), blobs, keys, "v"))
        return;
    ustore_key_t const start = key_from_integer(0);
    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* scanned = nullptr;
    ustore_outcome_t outcome {};
    ustore_blobs_scan_t scan {.database = database.handle(),
                              .outcome = &outcome,
                              .count = 1,
                              .collections = {&blobs, 0},
                              .start_keys = {&start, 0},
                              .offsets = &offsets,
                              .keys = &scanned,
                              .arena = arena.handle(),
                              .wanted = keys.size()};
    ustore_blobs_scan(&scan);
    if (!expect_success(outcome) || !expect_true(offsets && offsets[1] == keys.size())) return;
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database.handle(), blobs, {scanned, keys.size()}, columns, arena.handle(),
                                       outcome);
    ustore_blobs_read(&read);
    expect_status(outcome, status_code_t::invalid_argument_k);
    expect_true(scanned[keys.size() - 1] == keys.back());
}

/** An open export of an arena's outputs refuses every call naming the arena and its free, both as
 *  contended and leaving the outputs in place, until the last export closes. */
void test_arena_exports_hold_outputs(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "arena_exports.blobs", ustore_modality_blobs_k);
    std::array<ustore_key_t, 4> keys {};
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(index);
    ustore_arena_t arena = nullptr;
    ustore_outcome_t outcome {};
    ustore_arena_init_t init {.arena = &arena, .outcome = &outcome};
    ustore_arena_init(&init);
    if (!expect_success(outcome) || !expect_true(blobs_status.succeeded()) ||
        !write_text(database.handle(), blobs, keys, "v"))
        return;
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database.handle(), blobs, keys, columns, arena, outcome);
    ustore_blobs_read(&read);
    if (!expect_success(outcome)) return;
    std::string_view const exported = columns.value(0);

    expect_true(ustore_arena_export_open(nullptr) == ustore_status_invalid_argument_k);
    expect_true(ustore_arena_export_open(arena) == ustore_status_success_k);
    expect_true(ustore_arena_export_open(arena) == ustore_status_success_k);
    ustore_blobs_read(&read);
    expect_status(outcome, status_code_t::contended_k);
    expect_true(ustore_arena_free(arena) == ustore_status_contended_k);
    ustore_arena_export_close(arena);
    expect_true(ustore_arena_free(arena) == ustore_status_contended_k);
    ustore_arena_export_close(arena);
    // Closing past the last export changes nothing, and the exported outputs read as they were
    ustore_arena_export_close(arena);
    ustore_arena_export_close(nullptr);
    expect_true(exported == "v");
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(ustore_arena_free(arena) == ustore_status_success_k);
}

/** An executor whose threads keep 4 KB of scratch each refuses a read staging more than that with
 *  out_of_memory, which the same read on the default limit serves. */
void test_executor_scratch_limit(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "executor_scratch_limit.blobs",
                                                   ustore_modality_blobs_k);
    std::array<ustore_key_t, 4096> keys {};
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(index);
    if (!expect_true(blobs_status.succeeded()) || !write_text(database.handle(), blobs, keys, "value")) return;
    for (ustore_size_t const scratch_limit : {ustore_size_t {4096}, ustore_size_t {0}}) {
        auto [executor, spawn_status] = spawn_executor(2, scratch_limit);
        if (!expect_true(spawn_status.succeeded())) return;
        ustore_outcome_t outcome {};
        auto [arena, arena_status] = arena_t::make({.executor = executor});
        expect_true(arena_status.succeeded());
        read_columns_t columns;
        ustore_blobs_read_t read = read_of(database.handle(), blobs, keys, columns, arena.handle(), outcome);
        read.executor = executor;
        ustore_blobs_read(&read);
        expect_status(outcome, scratch_limit ? status_code_t::out_of_memory_k : status_code_t::success_k);
        arena = {};
        expect_true(ustore_executor_free(executor) == ustore_status_success_k);
    }
}

/** A read of the edges of more vertices than one word of tasks splits over the executor's threads
 *  and matches byte for byte the degrees, offsets and edges it hands back without one. */
case_result_t test_executor_sliced_graph(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    auto [graph, graph_status] = create_collection(database.handle(), "executor_sliced_graph.graph",
                                                   ustore_modality_graph_k);
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!graph_status.failed() && !spawn_status.failed())) return {};

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
    ustore_graph_write_edges_t write {.database = database.handle(),
                                      .outcome = &outcome,
                                      .count = edges,
                                      .collections = {&graph, 0},
                                      .sources = {sources.data(), sizeof(ustore_key_t)},
                                      .targets = {targets.data(), sizeof(ustore_key_t)}};
    ustore_graph_write_edges(&write);
    expect_success(outcome);

    struct edges_outputs_t {
        ustore_vertex_degree_t const* degrees = nullptr;
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* edges = nullptr;
    };
    auto read_edges = [&](ustore_executor_t on, ustore_completion_t completion, ustore_arena_t arena,
                          edges_outputs_t& outputs) {
        ustore_graph_read_edges_t read {.database = database.handle(),
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
        return expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    };
    auto [alone_arena, alone_arena_status] = arena_t::make();
    bool const arena_made = expect_true(!alone_arena_status.failed());
    edges_outputs_t alone;
    bool const read_alone = read_edges(nullptr, ustore_completion_blocking_k, alone_arena.handle(), alone);
    if (!expect_true(arena_made && read_alone && alone.offsets)) return {};
    for (ustore_completion_t const completion : {ustore_completion_blocking_k, ustore_completion_deferred_k}) {
        auto [arena, arena_status] = arena_t::make({.executor = executor});
        expect_true(!arena_status.failed());
        edges_outputs_t sliced;
        read_edges(executor, completion, arena.handle(), sliced);
        if (!sliced.offsets) {
            expect_true(false);
            continue;
        }
        expect_true(std::memcmp(alone.degrees, sliced.degrees, vertices_k * sizeof(ustore_vertex_degree_t)) == 0);
        expect_true(std::memcmp(alone.offsets, sliced.offsets, (vertices_k + 1) * sizeof(ustore_length_t)) == 0);
        expect_true(std::memcmp(alone.edges, sliced.edges, 3 * alone.offsets[vertices_k] * sizeof(ustore_key_t)) == 0);
    }
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
    return {};
}

/** A scan of more prefixes than one word of tasks splits over the executor's threads and matches
 *  byte for byte the offsets and strings it hands back without one. */
case_result_t test_executor_sliced_strings(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    auto [words, words_status] = create_collection(database.handle(), "executor_sliced_strings.words",
                                                   ustore_modality_strings_k);
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(!words_status.failed() && !spawn_status.failed())) return {};

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
    ustore_strings_write_t write {.database = database.handle(),
                                  .outcome = &outcome,
                                  .count = strings_k,
                                  .collections = {&words, 0},
                                  .strings = util::u32_tape(texts.data(), offsets.data()),
                                  .values = {keys.data(), sizeof(ustore_key_t)}};
    ustore_strings_write(&write);
    expect_success(outcome);

    ustore_length_t const* alone_offsets = nullptr;
    ustore_strings_view_t alone_strings {};
    auto [alone_arena, alone_arena_status] = arena_t::make();
    bool const arena_made = expect_true(!alone_arena_status.failed());
    // Each task's prefix is the first three bytes of one string, as one tape over the stored texts
    std::array<char, strings_k * 3> prefixes {};
    std::array<std::uint32_t, strings_k + 1> prefixes_offsets {};
    for (std::size_t index = 0; index != strings_k; ++index) {
        std::memcpy(prefixes.data() + 3 * index, texts.data() + 4 * index, 3);
        prefixes_offsets[index + 1] = static_cast<std::uint32_t>(3 * (index + 1));
    }
    ustore_strings_scan_t scan {.database = database.handle(),
                                .outcome = &outcome,
                                .count = strings_k,
                                .collections = {&words, 0},
                                .prefixes = util::u32_tape(prefixes.data(), prefixes_offsets.data()),
                                .offsets = &alone_offsets,
                                .strings = &alone_strings,
                                .arena = alone_arena.handle(),
                                .wanted = 4};
    ustore_strings_scan(&scan);
    bool const scanned = expect_success(outcome);
    if (!expect_true(arena_made && scanned && alone_offsets)) return {};
    for (ustore_completion_t const completion : {ustore_completion_blocking_k, ustore_completion_deferred_k}) {
        auto [arena, arena_status] = arena_t::make({.executor = executor});
        expect_true(!arena_status.failed());
        ustore_length_t const* sliced_offsets = nullptr;
        ustore_strings_view_t sliced_strings {};
        ustore_strings_scan_t on_executor = scan;
        on_executor.executor = executor, on_executor.completion = completion, on_executor.arena = arena.handle();
        on_executor.offsets = &sliced_offsets, on_executor.strings = &sliced_strings;
        ustore_strings_scan(&on_executor);
        expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
        if (!sliced_offsets) {
            expect_true(false);
            continue;
        }
        expect_true(std::memcmp(alone_offsets, sliced_offsets, (strings_k + 1) * sizeof(ustore_length_t)) == 0);
        expect_true(sliced_strings.layout == alone_strings.layout);
        for (std::size_t index = 0; index != alone_offsets[strings_k]; ++index)
            expect_equal(util::chars_of(util::tape_string(sliced_strings, index)),
                         util::chars_of(util::tape_string(alone_strings, index)));
    }
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
    return {};
}

#pragma endregion Deferred Calls

#pragma region Placement

/** A RAM executor refuses a memory domain the machine lacks, leaving its output null, and keeps
 *  every arena it makes on the domain of its thread 0. */
void test_executor_memory_domains(config_t const&) {
    fu::topology_t topology;
    if (!expect_true(!fu::failed(topology.harvest()))) return;
    std::int32_t const known = topology.memory_domain(0);
    std::int32_t const unknown = 1 << 20;

    ustore_outcome_t outcome {};
    ustore_executor_t executor = nullptr;
    ustore_executor_for_ram_t refused {.executor = &executor, .outcome = &outcome, .ram = {&unknown, 1}};
    ustore_executor_for_ram(&refused);
    expect_status(status_of(outcome), status_code_t::invalid_argument_k);
    expect_true(executor == nullptr);

    ustore_executor_for_ram_t placed {.executor = &executor, .outcome = &outcome, .ram = {&known, 1}, .threads = 1};
    ustore_executor_for_ram(&placed);
    expect_success(outcome);
    auto [arena, arena_status] = arena_t::make({.executor = executor});
    expect_true(!arena_status.failed());
    if (!arena_status.failed()) expect_equal(util::host_blocks_of(arena.handle())->memory_domain, known);
    arena = {};
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);

    // Zero threads take 7/8 of the cores of every domain the process may allocate on
    ustore_executor_for_ram_t derived {.executor = &executor, .outcome = &outcome};
    ustore_executor_for_ram(&derived);
    expect_success(outcome);
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

/** An NVMe database refuses a RAM executor with @c ustore_status_device_k before claiming it, so a
 *  second call is refused alike rather than contended, while every other engine serves it, and an
 *  NVMe executor serves every engine. */
void test_executor_residency(config_t const& config) {
    auto [database, open_status] = open_raw(config);
    if (!expect_true(open_status.succeeded())) return;
    ustore_outcome_t outcome {};
    ustore_executor_t ram = nullptr;
    ustore_executor_for_ram_t ram_spawn {.executor = &ram, .outcome = &outcome, .threads = 1};
    ustore_executor_for_ram(&ram_spawn);
    expect_success(outcome);
    ustore_executor_t nvme = nullptr;
    ustore_executor_for_nvme_t nvme_spawn {.executor = &nvme,
                                           .outcome = &outcome,
                                           .nvme = {.transport = ustore_transport_blocking_k},
                                           .threads = 1};
    ustore_executor_for_nvme(&nvme_spawn);
    expect_success(outcome);

    status_code_t const on_ram = config.engine() == "nvme" ? status_code_t::device_k : status_code_t::success_k;
    for (int round = 0; round != 2; ++round) {
        ustore_database_flush_t flush {.database = database, .executor = ram, .outcome = &outcome};
        ustore_database_flush(&flush);
        expect_status(outcome, on_ram);
    }
    ustore_database_flush_t flush {.database = database, .executor = nvme, .outcome = &outcome};
    ustore_database_flush(&flush);
    expect_success(outcome);
    expect_true(ustore_executor_free(nvme) == ustore_status_success_k);
    expect_true(ustore_executor_free(ram) == ustore_status_success_k);
    ustore_database_free(database);
}

#pragma endregion Placement

#pragma region Receivers

/** Proxies on an executor read and write as on the calling thread, at the head, in a transaction
 *  and at a snapshot. */
void test_executor_on_receivers(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [executor, executor_status] = spawn_executor_handle();
    if (!expect_true(!executor_status.failed())) return;

    triplet_t triplet;
    auto [collection, create_status] = database.on(executor).create("blobs");
    expect_true(!create_status.failed());
    auto reference = collection[triplet.keys];
    round_trip(reference, triplet);

    if (probed(database.supports_transactions())) {
        auto [transaction, transaction_status] = database.on(executor).transaction();
        if (!expect_true(!transaction_status.failed())) return;
        transaction_ref_t transaction_on_executor = transaction.on(executor);
        blobs_collection_t inside {transaction_on_executor.receiver(), collection.handle()};
        auto transaction_reference = inside[triplet.keys];
        check_equalities(transaction_reference, triplet);
        expect_true(!inside.at(triplet.keys).remove().failed());
        expect_true(!transaction_on_executor.commit().status.failed());
        check_length(reference, missing_length_k);
    }
    if (probed(database.supports_snapshots())) {
        round_trip(reference, triplet);
        auto [snapshot, snapshot_status] = database.on(executor).snapshot();
        if (!expect_true(!snapshot_status.failed())) return;
        snapshot_ref_t snapshot_on_executor = snapshot.on(executor);
        auto snapshot_reference = blobs_read_collection_t {snapshot_on_executor.receiver(),
                                                           collection.handle()}[triplet.keys];
        check_equalities(snapshot_reference, triplet);
    }
    expect_true(!executor.drain(ustore_mood_spinning_k).failed());
}

/** A deferred write through a transaction reaches the head only once the deferred commit finishes,
 *  which answers a stamp no lower than the one the deferred stage read at. */
case_result_t test_executor_deferred_commit(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    if (!probed(database.supports_transactions())) return case_result_t::skipped(needs_transactions_k);
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [executor, executor_status] = spawn_executor_handle();
    if (!expect_true(!executor_status.failed())) return {};

    std::array<ustore_key_t, 2> const keys {key_from_integer(75), key_from_integer(76)};
    std::array<std::string_view, 2> const values {"left", "right"};

    auto [head, create_status] = database.create("blobs");
    expect_true(!create_status.failed());
    auto [transaction, transaction_status] = database.transaction();
    if (!expect_true(!transaction_status.failed())) return {};
    transaction_on<ustore_completion_deferred_k> const deferred_transaction = transaction.deferred(executor);
    blobs_collection_on<ustore_completion_deferred_k, collection_access_t::read_write_k> const inside {
        transaction.receiver().on(executor.handle()), head.handle()};
    expect_true(!inside.write(keys, values).wait().failed());
    auto [leaked, leak_status] = head[keys[0]].present();
    expect_true(!leak_status.failed() && !leaked);

    auto [read_stamp, stage_status] = deferred_transaction.stage().wait();
    expect_true(!stage_status.failed());
    auto [commit_stamp, commit_status] = deferred_transaction.commit().wait();
    expect_true(!commit_status.failed());
    expect_true(static_cast<ustore_stamp_t>(read_stamp) <= static_cast<ustore_stamp_t>(commit_stamp));
    auto [left_present, left_status] = head[keys[0]].present();
    auto [right_present, right_status] = head[keys[1]].present();
    expect_true(!left_status.failed() && !right_status.failed() && left_present && right_present);
    return {};
}

/** A deferred document write and then deferred reads hand back the documents whole, one field of
 *  each as text, and a gathered column whose table views the keys and the header it viewed, and
 *  a deferred removal leaves the keys missing. */
void test_executor_deferred_docs(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [executor, executor_status] = spawn_executor_handle();
    if (!expect_true(!executor_status.failed())) return;
    auto [arena, arena_status] = arena_t::make({.executor = executor.handle()});
    if (!expect_true(!arena_status.failed())) return;

    expect_true(!database.create<docs_collection_t>("docs").status.failed());
    auto [collection, collection_status] = database.deferred(executor).find<docs_collection_t>("docs", arena).wait();
    if (!expect_true(!collection_status.failed())) return;

    std::array<ustore_key_t, 2> const keys {key_from_integer(81), key_from_integer(82)};
    std::array<std::string_view, 2> const documents {flat_documents_k[0], flat_documents_k[1]};
    expect_true(!collection.write(keys, documents).wait().failed());

    auto [read, read_status] = collection.read(keys, arena).wait();
    expect_true(!read_status.failed());
    expect_equal(read.size(), keys.size());
    for (std::size_t index = 0; index != read.size(); ++index)
        expect_true(json_equal(as_text(read[index]), flat_documents_k[index]));

    std::array<std::string_view, 1> const person {"person"};
    auto [people, people_status] = collection.read(keys, arena, person, ustore_docs_field_type_string_k).wait();
    expect_true(!people_status.failed());
    expect_equal(people.size(), keys.size());
    if (people.size() == keys.size()) {
        expect_equal(as_text(people[0]), std::string_view("Alice"));
        expect_equal(as_text(people[1]), std::string_view("Bob"));
    }

    auto const header = make_table_header().with<std::int32_t>("age");
    auto [table, gather_status] = collection.gather(keys, header, arena).wait();
    expect_true(!gather_status.failed());
    expect_equal(table.rows(), keys.size());
    expect_equal(table.columns(), 1u);
    if (table.rows() == keys.size() && table.columns() == 1) {
        auto const ages = table.template column<0>();
        strided_span<std::string_view const> const names = table.header().fields();
        strided_span<ustore_key_t const> const rows = table.index().keys();
        expect_true(ages[0].valid && ages[1].valid);
        expect_equal(ages[0].value, 24);
        expect_equal(ages[1].value, 25);
        expect_equal(names[0], std::string_view("age"));
        expect_true(rows[1] == keys[1]);
    }

    expect_true(!collection.remove(keys).wait().failed());
    auto [removed, removed_status] = collection.read(keys, arena).wait();
    expect_true(!removed_status.failed());
    expect_equal(removed.size(), keys.size());
    for (std::size_t index = 0; index != removed.size(); ++index) expect_true(!removed[index]);
}

/** A deferred write of edges and then deferred reads hand back the edges every vertex leaves,
 *  split by the offsets with none for a missing vertex between two present ones, and the degree
 *  of every vertex. */
case_result_t test_executor_deferred_graph(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [executor, executor_status] = spawn_executor_handle();
    expect_true(!executor_status.failed());
    expect_true(!database.create<graph_collection_t>("graph").status.failed());
    auto [arena, arena_status] = arena_t::make({.executor = executor.handle()});
    if (!expect_true(!arena_status.failed())) return {};
    auto [graph, graph_status] = database.deferred(executor).find<graph_collection_t>("graph", arena).wait();
    if (!expect_true(!graph_status.failed())) return {};
    std::array<edge_t, 3> const triangle {edge_of(1, 2, 9), edge_of(2, 3, 10), edge_of(3, 1, 11)};
    expect_true(!graph.write_edges(triangle).wait().failed());

    std::array<ustore_key_t, 3> const vertices {key_from_integer(1), key_from_integer(4), key_from_integer(2)};
    ustore_vertex_role_t const leaving = ustore_vertex_role_source_k;
    strided_span<ustore_vertex_role_t const> const roles {{&leaving, 0}, vertices.size()};
    auto [read, read_status] = graph.read_edges(vertices, arena, roles).wait();
    expect_true(!read_status.failed());
    expect_equal(read.size(), 2u);
    expect_equal(read.offsets.size(), vertices.size() + 1);
    if (read.offsets.size() == vertices.size() + 1) {
        edges_view_t const leaving_first = read.of_vertex(0), leaving_second = read.of_vertex(2);
        expect_true(leaving_first.size() == 1 && leaving_first[0] == triangle[0]);
        expect_equal(read.of_vertex(1).size(), 0u);
        expect_true(leaving_second.size() == 1 && leaving_second[0] == triangle[1]);
    }

    auto [degrees, degrees_status] = graph.degrees(std::array {key_from_integer(1), key_from_integer(4)}, arena).wait();
    expect_true(!degrees_status.failed());
    expect_equal(degrees.size(), 2u);
    if (degrees.size() == 2) {
        expect_equal(degrees[0], 2u);
        expect_equal(degrees[1], ustore_vertex_degree_missing_k);
    }
    return {};
}

/** A deferred write of strings and then deferred reads hand back their keys, the nil UUID for a
 *  string never written, and a page of strings for every prefix. */
case_result_t test_executor_deferred_strings(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [executor, executor_status] = spawn_executor_handle();
    expect_true(!executor_status.failed());
    expect_true(!database.create<strings_collection_t>("words").status.failed());
    auto [arena, arena_status] = arena_t::make({.executor = executor.handle()});
    if (!expect_true(!arena_status.failed())) return {};
    auto [words, words_status] = database.deferred(executor).find<strings_collection_t>("words", arena).wait();
    if (!expect_true(!words_status.failed())) return {};

    std::array<std::string_view, 3> const texts {"apple", "apricot", "banana"};
    std::array<ustore_key_t, 3> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    expect_true(!words.write(texts, keys).wait().failed());

    std::array<std::string_view, 3> const asked {"banana", "cherry", "apple"};
    auto [read, read_status] = words.read(asked, arena).wait();
    expect_true(!read_status.failed());
    expect_equal(read.size(), 3u);
    if (read.size() == 3) {
        expect_equal(read[0], keys[2]);
        expect_equal(read[1], ustore_key_t {});
        expect_equal(read[2], keys[0]);
    }

    std::array<std::string_view, 2> const prefixes {"ap", "b"};
    auto [scanned, scan_status] = words.scan(prefixes, 4, arena).wait();
    expect_true(!scan_status.failed());
    expect_equal(scanned.pages.size(), 3u);
    expect_equal(scanned.strings.size(), 3u);
    if (scanned.pages.size() == 3 && scanned.strings.size() == 3) {
        expect_equal(scanned.pages[1] - scanned.pages[0], 2u);
        expect_equal(scanned.strings[0], std::string_view("apple"));
        expect_equal(scanned.strings[1], std::string_view("apricot"));
        expect_equal(scanned.strings[2], std::string_view("banana"));
    }
    return {};
}

/** A database opened blocking on an executor keeps a write, and a deferred init stays deferred
 *  until thread 0 serves it, then opens the database holding that write. */
case_result_t test_executor_deferred_open(config_t const& config) {
    if (!config.directory()) return case_result_t::skipped(needs_directory_k);
    auto [executor, executor_status] = spawn_executor_handle();
    if (!expect_true(!executor_status.failed())) return {};

    ustore_key_t const key = key_from_integer(91);
    {
        ustore_database_t opened = nullptr;
        ustore_outcome_t outcome {};
        open_engine(config.options(), executor.handle(), &opened, &outcome);
        if (!expect_success(outcome)) return {};
        database_t database {opened};
        auto [head, head_status] = database.find_or_create("blobs");
        expect_true(!head_status.failed());
        expect_true(!head[key].assign("opened").failed());
        expect_true(!database.flush().failed());
    }

    ustore_database_t reopened = nullptr;
    ustore_outcome_t outcome {};
    open_engine(config.options(), executor.handle(), &reopened, &outcome, ustore_completion_deferred_k);
    ustore_outcome_wait(&outcome);
    if (!expect_success(outcome)) return {};
    database_t database {reopened};
    auto [head, find_status] = database.find("blobs");
    expect_true(!find_status.failed());
    auto [opened, opened_status] = head[key].value();
    expect_true(!opened_status.failed() && opened == "opened");
    return {};
}

/** A deferred receiver call on an executor another call holds finishes contended at once, and
 *  succeeds once the holding call is final. */
void test_executor_contended(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [executor, executor_status] = spawn_executor_handle();
    if (!expect_true(!executor_status.failed())) return;

    auto [arena, arena_status] = arena_t::make({.executor = executor.handle()});
    if (!expect_true(!arena_status.failed())) return;
    std::array<ustore_key_t, 1> const keys {key_from_integer(77)};
    auto [head, head_status] = database.find_or_create("blobs");
    expect_true(!head_status.failed());
    database_on<ustore_completion_deferred_k> const deferred_head = database.deferred(executor);
    blobs_collection_on<ustore_completion_deferred_k, collection_access_t::read_write_k> const collection =
        deferred_head[head.handle()];
    held_scan_t held;
    submit_held(held, executor.handle());
    deferred<values_view_t> refused = collection.read(keys, arena);
    expect_true(refused.status() == status_code_t::contended_k);
    expect_status(refused.wait(), status_code_t::contended_k);
    release_held(held);
    expect_true(!executor.drain().failed());
    expect_true(!collection.read(keys, arena).wait().status.failed());
}

#pragma endregion Receivers

int main(int argc, char** argv) {
    start_test_run(argc, argv);

    auto [directory, directory_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(directory_status)) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    config_t const config {test_run.settings.engine, directory.path()};

    run_test_case(engine_label_t {"test_arena_null_refused"}.c_str(), test_arena_null_refused, config);
    run_test_case(engine_label_t {"test_arena_reuse"}.c_str(), test_arena_reuse, config);
    run_test_case(engine_label_t {"test_arena_failed_outputs_null"}.c_str(), test_arena_failed_outputs_null, config);
    run_test_case(engine_label_t {"test_arena_aligned_outputs"}.c_str(), test_arena_aligned_outputs, config);
    run_test_case(engine_label_t {"test_arena_outputs_only"}.c_str(), test_arena_outputs_only, config);
    run_test_case(engine_label_t {"test_arena_free_waits"}.c_str(), test_arena_free_waits, config);
    run_test_case(engine_label_t {"test_executor_deferred_round_trip"}.c_str(), test_executor_deferred_round_trip,
                  config);
    run_test_case(engine_label_t {"test_executor_copied_submission"}.c_str(), test_executor_copied_submission, config);
    run_test_case(engine_label_t {"test_executor_held_contended"}.c_str(), test_executor_held_contended, config);
    run_test_case(engine_label_t {"test_executor_contended_from_another_thread"}.c_str(),
                  test_executor_contended_from_another_thread, config);
    run_test_case(engine_label_t {"test_executor_finishes_by_polling"}.c_str(), test_executor_finishes_by_polling,
                  config);
    run_test_case(engine_label_t {"test_executor_submit_after_wait"}.c_str(), test_executor_submit_after_wait, config);
    run_test_case(engine_label_t {"test_executor_drain"}.c_str(), test_executor_drain, config);
    run_test_case(engine_label_t {"test_executor_streams_on_host"}.c_str(), test_executor_streams_on_host, config);
    run_test_case(engine_label_t {"test_executor_contended_arena"}.c_str(), test_executor_contended_arena, config);
    run_test_case(engine_label_t {"test_executor_arena_on_device"}.c_str(), test_executor_arena_on_device, config);
    run_test_case(engine_label_t {"test_outcome_wait_blocks"}.c_str(), test_outcome_wait_blocks, config);
    run_test_case(engine_label_t {"test_database_free_waits"}.c_str(), test_database_free_waits, config);
    run_test_case(engine_label_t {"test_database_free_waits_without_executor"}.c_str(),
                  test_database_free_waits_without_executor, config);
    run_test_case(engine_label_t {"test_database_init_on_executor"}.c_str(), test_database_init_on_executor, config);
    run_test_case(engine_label_t {"test_executor_sliced_read"}.c_str(), test_executor_sliced_read, config);
    run_test_case(engine_label_t {"test_executor_sliced_docs"}.c_str(), test_executor_sliced_docs, config);
    run_test_case(engine_label_t {"test_executor_sliced_ingest"}.c_str(), test_executor_sliced_ingest, config);
    run_test_case(engine_label_t {"test_executor_scratch_limit"}.c_str(), test_executor_scratch_limit, config);
    run_test_case(engine_label_t {"test_arena_refuses_its_own_inputs"}.c_str(), test_arena_refuses_its_own_inputs,
                  config);
    run_test_case(engine_label_t {"test_arena_exports_hold_outputs"}.c_str(), test_arena_exports_hold_outputs, config);
    run_test_case(engine_label_t {"test_executor_sliced_graph"}.c_str(), test_executor_sliced_graph, config);
    run_test_case(engine_label_t {"test_executor_sliced_strings"}.c_str(), test_executor_sliced_strings, config);
    run_test_case(engine_label_t {"test_executor_memory_domains"}.c_str(), test_executor_memory_domains, config);
    run_test_case(engine_label_t {"test_executor_residency"}.c_str(), test_executor_residency, config);
    run_test_case(engine_label_t {"test_executor_on_receivers"}.c_str(), test_executor_on_receivers, config);
    run_test_case(engine_label_t {"test_executor_deferred_commit"}.c_str(), test_executor_deferred_commit, config);
    run_test_case(engine_label_t {"test_executor_deferred_docs"}.c_str(), test_executor_deferred_docs, config);
    run_test_case(engine_label_t {"test_executor_deferred_graph"}.c_str(), test_executor_deferred_graph, config);
    run_test_case(engine_label_t {"test_executor_deferred_strings"}.c_str(), test_executor_deferred_strings, config);
    run_test_case(engine_label_t {"test_executor_deferred_open"}.c_str(), test_executor_deferred_open, config);
    run_test_case(engine_label_t {"test_executor_contended"}.c_str(), test_executor_contended, config);
    return finish_test_run();
}
