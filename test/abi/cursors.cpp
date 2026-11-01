/**
 *  @file test/abi/cursors.cpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief Checks cursors and filtered scans of the C ABI: whole walks, resumption after writes,
 *      snapshots, predicates, patterns and filters.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`
#include <cstdio>  // `stderr`

#include <algorithm>        // `std::copy`, `std::equal`, `std::min`, `std::sort`
#include <array>            // `std::array`
#include <chrono>           // `std::chrono::milliseconds`
#include <initializer_list> // `std::initializer_list`
#include <span>             // `std::span`
#include <string_view>      // `std::string_view`
#include <thread>           // `std::this_thread::sleep_for`

#include <fmt/format.h>

#include <forkunion/atomics.hpp> // `fu::standard_atomic_ref`

#include "ustore/ustore.hpp"

#include "ustore/utilities/ram_device.hpp" // `caller_exclusivity_t`, `executor_t`

#include "abi/fixture.hpp" // `write_text`, `held_scan_t`, `submit_held`, `open_cursor`, `pull_of`, `expect_page`
#include "harness.hpp"     // `suite_exit_code`, `expect`, `expect_eq`, `expect_near`, `expect_status`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::test::abi;
using namespace unum::ustore::utilities;
using namespace unum;

#pragma region Cursors

/** A blobs cursor walks a collection whole in pages with the values of its keys, and a strings
 *  cursor walks one prefix whole, each pulling nothing once a page came back short. */
case_result_t cursor_whole_walk(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "cursor.walk", ustore_modality_blobs_k);
    auto [strings, strings_status] = create_collection(database, "cursor.walk.strings", ustore_modality_strings_k);
    failures += expect(blobs_status.succeeded() && strings_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 10> keys;
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(100 + index);
    failures += write_text(database, blobs, keys, "walked");
    auto [cursor, cursor_status] = open_cursor(database, blobs);
    failures += expect(cursor_status.succeeded());
    if (failures != 0) return failures;
    ustore_outcome_t outcome {};
    for (std::size_t first : {0u, 4u, 8u, 10u, 10u}) {
        pulled_keys_t pulled;
        ustore_blobs_cursor_next_t next = pull_of(database, cursor, pulled, arena, 4, outcome);
        ustore_blobs_cursor_next(&next);
        failures += expect_success(outcome);
        std::span<ustore_key_t const> const wanted {keys.data() + first, std::min<std::size_t>(4, 10 - first)};
        failures += expect_page(pulled, wanted, "walked");
    }
    ustore_blobs_cursor_free(cursor);

    std::array<std::string_view, 4> const texts {"b:1", "a:3", "a:1", "a:2"};
    std::array<ustore_key_t, 4> const text_keys {key_from_integer(21), key_from_integer(13), key_from_integer(11),
                                                 key_from_integer(12)};
    strided_strings<std::string_view> const accessed {strided(texts).begin()};
    ustore_strings_write_t write {.database = database,
                                  .outcome = &outcome,
                                  .count = texts.size(),
                                  .collections = {&strings, 0},
                                  .strings = accessed.view(),
                                  .values = {text_keys.data(), sizeof(ustore_key_t)}};
    ustore_strings_write(&write);
    failures += expect_success(outcome);
    ustore_strings_cursor_t walk = nullptr;
    ustore_strings_cursor_init_t init {.cursor = &walk,
                                       .database = database,
                                       .outcome = &outcome,
                                       .collection = strings,
                                       .prefix = bytes_of("a:")};
    ustore_strings_cursor_init(&init);
    failures += expect_success(outcome);
    std::array<std::string_view, 3> const prefixed {"a:1", "a:2", "a:3"};
    std::array<ustore_key_t, 3> const prefixed_keys {text_keys[2], text_keys[3], text_keys[1]};
    for (std::size_t first : {0u, 2u, 3u}) {
        ustore_strings_view_t found {};
        ustore_key_t const* values = nullptr;
        ustore_size_t written = 0;
        ustore_strings_cursor_next_t next {.database = database,
                                           .outcome = &outcome,
                                           .cursor = walk,
                                           .strings = &found,
                                           .values = &values,
                                           .written = &written,
                                           .arena = arena,
                                           .wanted = 2};
        ustore_strings_cursor_next(&next);
        failures += expect_success(outcome);
        failures += expect_eq(written, std::min<std::size_t>(2, 3 - first));
        for (std::size_t index = 0; index != written && first + index != prefixed.size(); ++index) {
            failures += expect_eq(string_of(tape_string(found, index)), prefixed[first + index]);
            failures += expect_eq(values[index], prefixed_keys[first + index]);
        }
    }
    ustore_strings_cursor_free(walk);
    return failures;
}

/** A cursor keeps only the key its next pull starts at, so keys written behind it stay behind and
 *  keys written ahead of it come, until a short page exhausts it. */
case_result_t cursor_resume_after_writes(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    auto [collection, create_status] = create_collection(database, "cursor.resume", ustore_modality_blobs_k);
    failures += expect(create_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 4> const written {key_from_integer(10), key_from_integer(20), key_from_integer(30),
                                               key_from_integer(40)};
    failures += write_text(database, collection, written, "first");
    auto [cursor, cursor_status] = open_cursor(database, collection);
    failures += expect(cursor_status.succeeded());
    if (failures != 0) return failures;
    ustore_outcome_t outcome {};
    pulled_keys_t pulled;
    ustore_blobs_cursor_next_t next = pull_of(database, cursor, pulled, arena, 2, outcome);
    ustore_blobs_cursor_next(&next);
    failures += expect_success(outcome);
    failures += expect_page(pulled, {written.data(), 2}, "first");

    std::array<ustore_key_t, 2> const later {key_from_integer(15), key_from_integer(35)};
    failures += write_text(database, collection, later, "later");
    next.wanted = 8;
    ustore_blobs_cursor_next(&next);
    failures += expect_success(outcome);
    failures += expect_eq(pulled.written, 3u);
    if (pulled.written == 3) {
        failures += expect_eq(pulled.keys[0], key_from_integer(30));
        failures += expect_eq(pulled.keys[1], key_from_integer(35));
        failures += expect_eq(pulled.value(1), "later");
        failures += expect_eq(pulled.keys[2], key_from_integer(40));
    }

    ustore_key_t const beyond = key_from_integer(50);
    failures += write_text(database, collection, {&beyond, 1}, "beyond");
    ustore_blobs_cursor_next(&next);
    failures += expect_success(outcome);
    failures += expect_eq(pulled.written, 0u);
    failures += expect(pulled.keys == nullptr && !tape_written(pulled.values));
    ustore_blobs_cursor_free(cursor);
    return failures;
}

/** A cursor through a snapshot walks what the snapshot saw, and holds the snapshot after its
 *  holder frees it. */
case_result_t cursor_snapshot_view(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_snapshots()) return case_result_t::skipped("the engine keeps no snapshots", failures);
    auto [collection, create_status] = create_collection(database, "cursor.snapshot", ustore_modality_blobs_k);
    failures += expect(create_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 3> const seen {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    failures += write_text(database, collection, seen, "seen");
    ustore_outcome_t outcome {};
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&snapshot_init);
    failures += expect_success(outcome);
    ustore_key_t const unseen = key_from_integer(4);
    failures += write_text(database, collection, {&unseen, 1}, "unseen");
    ustore_blobs_remove_t remove {.database = database,
                                  .outcome = &outcome,
                                  .count = 1,
                                  .collections = {&collection, 0},
                                  .keys = {&seen[1], 0}};
    ustore_blobs_remove(&remove);
    failures += expect_success(outcome);

    auto [cursor, cursor_status] = open_cursor(database, collection, ustore_snapshot_view(snapshot));
    failures += expect(cursor_status.succeeded());
    ustore_snapshot_free(snapshot);
    if (failures != 0) return failures;
    pulled_keys_t pulled;
    ustore_blobs_cursor_next_t next = pull_of(database, cursor, pulled, arena, 8, outcome);
    ustore_blobs_cursor_next(&next);
    failures += expect_success(outcome);
    failures += expect_page(pulled, seen, "seen");
    ustore_blobs_cursor_free(cursor);
    return failures;
}

/** What a thread freeing a cursor saw of the deferred pull naming it once the free returned. */
struct cursor_freeing_t {
    ustore_blobs_cursor_t cursor = nullptr;
    ustore_outcome_t* outcome = nullptr;
    ustore_status_t status_after_free = ustore_status_deferred_k;
    std::uint32_t freed = 0;
};

/** Frees the cursor of @p freeing, then records the status of the pull that named it. */
void free_cursor(cursor_freeing_t& freeing) noexcept {
    ustore_blobs_cursor_free(freeing.cursor);
    freeing.status_after_free = ustore_outcome_status(freeing.outcome);
    fu::standard_atomic_ref<std::uint32_t> {freeing.freed}.store(1, std::memory_order_release);
}

/** Freeing a cursor waits until the deferred pull naming it is final, here a pull held behind
 *  an unfinished call. */
case_result_t cursor_free_waits(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [executor, spawn_status] = spawn_executor(0);
    st::expected<executor_t> freeing_thread = executor_t::make(1, caller_exclusivity_t::caller_exclusive_k);
    auto [collection, create_status] = create_collection(database, "cursor.free", ustore_modality_blobs_k);
    failures += expect(spawn_status.succeeded() && freeing_thread.has_value() && create_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
    failures += write_text(database, collection, keys, "held");
    auto [cursor, cursor_status] = open_cursor(database, collection);
    failures += expect(arena_status.succeeded() && cursor_status.succeeded());
    if (failures != 0) return failures;

    held_scan_t held;
    failures += submit_held(held, executor);
    ustore_outcome_t outcome {};
    pulled_keys_t pulled;
    ustore_blobs_cursor_next_t next = pull_of(database, cursor, pulled, arena, 8, outcome);
    next.executor = executor;
    next.completion = ustore_completion_deferred_k;
    ustore_blobs_cursor_next(&next);

    cursor_freeing_t freeing {.cursor = cursor, .outcome = &outcome};
    st::expected<fu_generation_t> const generation = freeing_thread->start<&free_cursor>(freeing);
    failures += expect(generation.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    failures += expect(fu::standard_atomic_ref<std::uint32_t> {freeing.freed}.load(std::memory_order_acquire) == 0);

    failures += release_held(held);
    if (generation) freeing_thread->join(*generation);
    else free_cursor(freeing);
    failures += expect(freeing.status_after_free == ustore_status_success_k);
    failures += expect_page(pulled, keys, "held");
    ustore_executor_free(executor);
    return failures;
}

#pragma endregion Cursors

#pragma region Filtered Scans

/** Counts the calls of a predicate admitting every key whose last byte is even, and the keys it
 *  was asked about. */
struct even_keys_predicate_t {
    std::size_t calls = 0;
    std::size_t asked = 0;
};

/** Admits every key of @p keys whose last byte is even, counting the call in @p state. */
void admit_even_keys(ustore_key_t const* keys, ustore_admission_t* admissions, ustore_size_t count,
                     void* state) noexcept {
    even_keys_predicate_t& counted = *static_cast<even_keys_predicate_t*>(state);
    ++counted.calls;
    counted.asked += count;
    for (ustore_size_t index = 0; index != count; ++index)
        admissions[index] = keys[index].uuid.bytes[15] % 2 == 0 ? ustore_admit_k : ustore_reject_k;
}

/** A predicate narrows every scan page by page: each page of @c wanted listed keys asks it
 *  once, cheap or expensive, and the scan keeps listing until @c wanted keys survive or
 *  the collection ends. */
std::size_t scan_predicate_per_page(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "scan_predicate_per_page.blobs", ustore_modality_blobs_k);
    auto [graph, graph_status] = create_collection(database, "scan_predicate_per_page.graph", ustore_modality_graph_k);
    failures += expect(blobs_status.succeeded() && graph_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 10> keys {};
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(1 + index);
    failures += write_text(database, blobs, keys, "value");
    ustore_outcome_t outcome {};
    ustore_graph_write_vertices_t write_vertices {.database = database,
                                                  .outcome = &outcome,
                                                  .count = keys.size(),
                                                  .collections = {&graph, 0},
                                                  .vertices = {keys.data(), sizeof(ustore_key_t)}};
    ustore_graph_write_vertices(&write_vertices);
    failures += expect_success(outcome);

    for (ustore_predicate_cost_t const cost : {ustore_predicate_cheap_k, ustore_predicate_expensive_k}) {
        even_keys_predicate_t counted;
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* found = nullptr;
        ustore_blobs_scan_t scan {.database = database,
                                  .outcome = &outcome,
                                  .count = 1,
                                  .collections = {&blobs, 0},
                                  .predicate = {&admit_even_keys, &counted, cost},
                                  .offsets = &offsets,
                                  .keys = &found,
                                  .arena = arena,
                                  .wanted = 3};
        ustore_blobs_scan(&scan);
        failures += expect_success(outcome);
        if (failures != 0) return failures;
        // Pages [1,3] and [4,6] keep 2, 4 and 6, so the third page is never listed
        failures += expect_eq(offsets[1], 3u);
        failures += expect(found[0] == keys[1] && found[1] == keys[3] && found[2] == keys[5]);
        failures += expect_eq(counted.calls, 2u);
        failures += expect_eq(counted.asked, 6u);

        even_keys_predicate_t vertices_counted;
        ustore_graph_scan_vertices_t vertices {.database = database,
                                               .outcome = &outcome,
                                               .count = 1,
                                               .collections = {&graph, 0},
                                               .predicate = {&admit_even_keys, &vertices_counted, cost},
                                               .offsets = &offsets,
                                               .keys = &found,
                                               .arena = arena,
                                               .wanted = 4};
        ustore_graph_scan_vertices(&vertices);
        failures += expect_success(outcome);
        if (failures != 0) return failures;
        // Pages [1,4], [5,8] and the short [9,10] keep the five even vertices, of which four fit
        failures += expect_eq(offsets[1], 4u);
        failures += expect(found[3] == keys[7]);
        failures += expect_eq(vertices_counted.calls, 2u);
    }

    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* found = nullptr;
    ustore_blobs_scan_t refused {.database = database,
                                 .outcome = &outcome,
                                 .count = 1,
                                 .collections = {&blobs, 0},
                                 .predicate = {&admit_even_keys, nullptr, static_cast<ustore_predicate_cost_t>(7)},
                                 .offsets = &offsets,
                                 .keys = &found,
                                 .arena = arena,
                                 .wanted = 3};
    ustore_blobs_scan(&refused);
    failures += expect_status(status_t {outcome}, status_code_t::invalid_argument_k);
    return failures;
}

/** An unordered scan of a collection written once lists the same set of keys as the ordered one. */
std::size_t scan_unordered_matches_ordered(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    auto [other_arena, other_arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded() && other_arena_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "scan_unordered.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    if (failures != 0) return failures;
    std::array<ustore_key_t, 40> keys {};
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(7000 + index * 13);
    failures += write_text(database, blobs, keys, "value");

    auto list = [&](ustore_scan_mode_t mode, ustore_arena_t into, std::span<ustore_key_t const>& listed) {
        ustore_outcome_t outcome {};
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* found = nullptr;
        ustore_blobs_scan_t scan {.database = database,
                                  .outcome = &outcome,
                                  .count = 1,
                                  .collections = {&blobs, 0},
                                  .offsets = &offsets,
                                  .keys = &found,
                                  .arena = into,
                                  .wanted = keys.size() * 2,
                                  .mode = mode};
        ustore_blobs_scan(&scan);
        std::size_t const listing_failures = expect_success(outcome) + expect(offsets != nullptr);
        if (!listing_failures) listed = {found + offsets[0], found + offsets[1]};
        return listing_failures;
    };
    std::span<ustore_key_t const> ordered, unordered;
    failures += list(ustore_scan_ordered_k, arena, ordered);
    failures += list(ustore_scan_unordered_k, other_arena, unordered);
    if (failures != 0) return failures;
    failures += expect_eq(ordered.size(), keys.size());
    std::array<ustore_key_t, keys.size()> sorted {};
    failures += expect_eq(unordered.size(), keys.size());
    if (failures != 0) return failures;
    std::copy(unordered.begin(), unordered.end(), sorted.begin());
    std::sort(sorted.begin(), sorted.end());
    failures += expect(std::equal(sorted.begin(), sorted.end(), ordered.begin(), ordered.end()));
    return failures;
}

/** Documents, pages and vertices each walk through a cursor of their own, page after page, with a
 *  predicate compiled once at init narrowing every page, and documents and pages read their values
 *  in the same pull. */
std::size_t cursors_per_modality(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    auto [docs, docs_status] = create_collection(database, "cursors.docs", ustore_modality_docs_k);
    auto [graph, graph_status] = create_collection(database, "cursors.graph", ustore_modality_graph_k);
    auto [pages, pages_status] = create_collection(database, "cursors.pages", ustore_modality_pages_k);
    failures += expect(docs_status.succeeded() && graph_status.succeeded() && pages_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 5> keys {};
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(2400 + index);
    std::string_view const text = R"({"a":1})";
    strided_strings<std::string_view> const documents {{&text, 0}};
    ustore_outcome_t outcome {};
    ustore_docs_write_t write_documents {.database = database,
                                         .outcome = &outcome,
                                         .count = keys.size(),
                                         .collections = {&docs, 0},
                                         .keys = {keys.data(), sizeof(ustore_key_t)},
                                         .values = documents.view()};
    ustore_docs_write(&write_documents);
    failures += expect_success(outcome);
    ustore_graph_write_vertices_t write_vertices {.database = database,
                                                  .outcome = &outcome,
                                                  .count = keys.size(),
                                                  .collections = {&graph, 0},
                                                  .vertices = {keys.data(), sizeof(ustore_key_t)}};
    ustore_graph_write_vertices(&write_vertices);
    failures += expect_success(outcome);
    std::string_view const markup = "<a/>";
    strided_strings<std::string_view> const markups {{&markup, 0}};
    ustore_pages_import_t import {};
    import.database = database, import.outcome = &outcome, import.count = keys.size(), import.collections = {&pages, 0};
    import.keys = {keys.data(), sizeof(ustore_key_t)}, import.values = markups.view();
    import.encoding = ustore_page_encoding_xml_k;
    ustore_pages_import(&import);
    failures += expect_success(outcome);
    if (failures != 0) return failures;

    // Keys 2400 to 2404 hold three even ones, which pages of two hand out as two and then one
    auto walk = [&]<typename init_type_, typename next_type_, typename cursor_type_>(
                    void (*init)(init_type_*), void (*next)(next_type_*), void (*free)(cursor_type_),
                    ustore_collection_id_t collection) {
        std::size_t walk_failures = 0;
        even_keys_predicate_t counted;
        cursor_type_ cursor = nullptr;
        init_type_ opened {};
        opened.cursor = &cursor, opened.database = database, opened.outcome = &outcome;
        opened.collection = collection, opened.start_key = keys[0];
        opened.predicate = {&admit_even_keys, &counted, ustore_predicate_expensive_k};
        init(&opened);
        walk_failures += expect_success(outcome);
        if (walk_failures) return walk_failures;
        std::array<std::size_t, 3> const pulled {2, 1, 0};
        for (std::size_t const expected_count : pulled) {
            ustore_key_t const* found = nullptr;
            ustore_strings_view_t values {};
            ustore_size_t written = 0;
            next_type_ pull {};
            pull.database = database, pull.outcome = &outcome, pull.cursor = cursor, pull.keys = &found;
            pull.written = &written, pull.arena = arena, pull.wanted = 2;
            if constexpr (requires { pull.values; }) pull.values = &values;
            if constexpr (requires { pull.outputs_encoding; }) pull.outputs_encoding = ustore_page_encoding_xml_k;
            next(&pull);
            walk_failures += expect_success(outcome);
            walk_failures += expect_eq(written, expected_count);
            if (written) walk_failures += expect(found[0].uuid.bytes[15] % 2 == 0);
            if constexpr (requires { pull.values; })
                if (written) walk_failures += expect(tape_written(values));
        }
        free(cursor);
        return walk_failures;
    };
    failures += walk(&ustore_docs_cursor_init, &ustore_docs_cursor_next, &ustore_docs_cursor_free, docs);
    failures += walk(&ustore_pages_cursor_init, &ustore_pages_cursor_next, &ustore_pages_cursor_free, pages);
    failures += walk(&ustore_graph_vertices_cursor_init, &ustore_graph_vertices_cursor_next,
                     &ustore_graph_vertices_cursor_free, graph);
    return failures;
}

/** Patterns narrow blobs and strings scans by their values, filters narrow documents and pages, a
 *  pattern that does not compile is refused with StringZilla's message, and a cursor keeps the
 *  patterns it compiled at init. */
std::size_t scan_patterns_and_filters(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "scan_patterns.blobs", ustore_modality_blobs_k);
    auto [docs, docs_status] = create_collection(database, "scan_patterns.docs", ustore_modality_docs_k);
    auto [pages, pages_status] = create_collection(database, "scan_patterns.pages", ustore_modality_pages_k);
    failures += expect(blobs_status.succeeded() && docs_status.succeeded() && pages_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 4> keys {};
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(3100 + index);
    std::array<std::string_view, 4> const fruits {"Apple", "banana", "cherry", "avocado"};
    std::array<std::string_view, 4> const documents {R"({"age":25})", R"({"age":40})", R"({"name":"x"})",
                                                     R"({"age":31})"};
    std::array<std::string_view, 4> const markups {"<a><b/></a>", "<a/>", "<c><b>t</b></c>", "<a><c/></a>"};
    ustore_outcome_t outcome {};
    for (std::size_t index = 0; index != keys.size(); ++index)
        failures += write_text(database, blobs, {&keys[index], 1}, fruits[index]);
    strided_strings<std::string_view> const documents_view {strided(documents).begin()};
    ustore_docs_write_t write_documents {.database = database,
                                         .outcome = &outcome,
                                         .count = keys.size(),
                                         .collections = {&docs, 0},
                                         .keys = {keys.data(), sizeof(ustore_key_t)},
                                         .values = documents_view.view()};
    ustore_docs_write(&write_documents);
    failures += expect_success(outcome);
    strided_strings<std::string_view> const markups_view {strided(markups).begin()};
    ustore_pages_import_t import {};
    import.database = database, import.outcome = &outcome, import.count = keys.size(), import.collections = {&pages, 0};
    import.keys = {keys.data(), sizeof(ustore_key_t)}, import.values = markups_view.view();
    import.encoding = ustore_page_encoding_xml_k;
    ustore_pages_import(&import);
    failures += expect_success(outcome);
    if (failures != 0) return failures;

    // Lists through @p scan with its match fields set by @p narrow, comparing with the indices
    // of @p wanted
    auto expect_matched = [&]<typename scan_type_>(void (*scan)(scan_type_*), ustore_collection_id_t collection,
                                                   auto&& narrow, std::initializer_list<std::size_t> wanted,
                                                   status_code_t refused = status_code_t::success_k) {
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* found = nullptr;
        scan_type_ arguments {};
        arguments.database = database, arguments.outcome = &outcome, arguments.count = 1;
        arguments.collections = {&collection, 0}, arguments.offsets = &offsets, arguments.keys = &found;
        arguments.arena = arena, arguments.wanted = 8;
        narrow(arguments);
        scan(&arguments);
        if (refused != status_code_t::success_k) return expect_status(status_t {outcome}, refused);
        std::size_t listing_failures = expect_success(outcome) + expect(offsets != nullptr);
        if (listing_failures) return listing_failures;
        listing_failures += expect_eq(static_cast<std::size_t>(offsets[1]), wanted.size());
        std::size_t position = 0;
        for (std::size_t const index : wanted)
            if (position < offsets[1]) listing_failures += expect(found[position++] == keys[index]);
        return listing_failures;
    };
    // Every call reuses one pair of offsets, read only while the call compiles its patterns
    std::array<std::uint32_t, 2> offsets {};
    auto patterns_of = [&](std::string_view const& text, ustore_pattern_kind_t kind,
                           ustore_case_sensitivity_t sensitivity = ustore_cased_k) {
        offsets = {0, static_cast<std::uint32_t>(text.size())};
        return ustore_patterns_t {.patterns = u32_tape(text.data(), offsets.data()),
                                  .count = 1,
                                  .kind = kind,
                                  .sensitivity = sensitivity};
    };
    std::string_view const starts_with_a = "^a";
    std::string_view const an = "an";
    std::string_view const unbalanced = "(";
    failures += expect_matched(
        &ustore_blobs_scan, blobs,
        [&](ustore_blobs_scan_t& scan) { scan.patterns = patterns_of(starts_with_a, ustore_pattern_regex_k); }, {3});
    failures += expect_matched(&ustore_blobs_scan, blobs,
                               [&](ustore_blobs_scan_t& scan) {
                                   scan.patterns = patterns_of(starts_with_a, ustore_pattern_regex_k, ustore_uncased_k);
                               },
                               {0, 3});
    failures += expect_matched(
        &ustore_blobs_scan, blobs,
        [&](ustore_blobs_scan_t& scan) { scan.patterns = patterns_of(an, ustore_pattern_substrings_k); }, {1});
    failures += expect_matched(
        &ustore_blobs_scan, blobs,
        [&](ustore_blobs_scan_t& scan) { scan.patterns = patterns_of(unbalanced, ustore_pattern_regex_k); }, {},
        status_code_t::invalid_argument_k);

    // A weighted substring scores BM25 with no length normalization: "banana" holds "an" twice,
    // overlapping or not
    float const weight = 2.0f;
    float const* scores = nullptr;
    failures += expect_matched(&ustore_blobs_scan, blobs,
                               [&](ustore_blobs_scan_t& scan) {
                                   scan.patterns = patterns_of(an, ustore_pattern_substrings_k);
                                   scan.patterns.weights = &weight;
                                   scan.scores = &scores;
                               },
                               {1});
    failures += expect(scores != nullptr);
    if (scores) failures += expect_near(scores[0], 2.0 * (2.0 * 2.2) / (2.0 + 1.2));
    failures += expect_matched(
        &ustore_blobs_scan, blobs,
        [&](ustore_blobs_scan_t& scan) {
            scan.patterns = patterns_of(starts_with_a, ustore_pattern_regex_k);
            scan.patterns.weights = &weight;
        },
        {}, status_code_t::invalid_argument_k);

    auto [strings, strings_status] = create_collection(database, "scan_patterns.strings", ustore_modality_strings_k);
    failures += expect(strings_status.succeeded());
    strided_strings<std::string_view> const fruits_view {strided(fruits).begin()};
    ustore_strings_write_t write_strings {.database = database,
                                          .outcome = &outcome,
                                          .count = keys.size(),
                                          .collections = {&strings, 0},
                                          .strings = fruits_view.view(),
                                          .values = {keys.data(), sizeof(ustore_key_t)}};
    ustore_strings_write(&write_strings);
    failures += expect_success(outcome);
    ustore_length_t const* strings_offsets = nullptr;
    ustore_strings_view_t listed {};
    ustore_strings_scan_t strings_scan {.database = database,
                                        .outcome = &outcome,
                                        .count = 1,
                                        .collections = {&strings, 0},
                                        .patterns = patterns_of(starts_with_a, ustore_pattern_regex_k),
                                        .offsets = &strings_offsets,
                                        .strings = &listed,
                                        .arena = arena,
                                        .wanted = 8};
    ustore_strings_scan(&strings_scan);
    failures += expect_success(outcome);
    // Strings come in byte order, where "Apple" sorts first but only "avocado" starts with
    // a lowercase "a"
    failures += expect(strings_offsets && strings_offsets[1] == 1 && tape_string(listed, 0).length == 7);

    std::string_view const older = "?@.age > 30";
    std::string_view const holds_b = "//b";
    failures += expect_matched(&ustore_docs_scan, docs,
                               [&](ustore_docs_scan_t& scan) { scan.filter = bytes_of(older); }, {1, 3});
    failures += expect_matched(&ustore_pages_scan, pages,
                               [&](ustore_pages_scan_t& scan) { scan.filter = bytes_of(holds_b); }, {0, 2});

    ustore_blobs_cursor_t cursor = nullptr;
    ustore_blobs_cursor_init_t opened {.cursor = &cursor,
                                       .database = database,
                                       .outcome = &outcome,
                                       .collection = blobs,
                                       .start_key = keys[0],
                                       .patterns = patterns_of(an, ustore_pattern_substrings_k)};
    ustore_blobs_cursor_init(&opened);
    failures += expect_success(outcome);
    if (failures != 0) return failures;
    ustore_key_t const* found = nullptr;
    ustore_size_t written = 0;
    ustore_blobs_cursor_next_t pull {.database = database,
                                     .outcome = &outcome,
                                     .cursor = cursor,
                                     .keys = &found,
                                     .written = &written,
                                     .arena = arena,
                                     .wanted = 1};
    ustore_blobs_cursor_next(&pull);
    failures += expect_success(outcome);
    failures += expect(written == 1 && found[0] == keys[1]);
    ustore_blobs_cursor_next(&pull);
    failures += expect_success(outcome);
    failures += expect_eq(written, 0u);
    ustore_blobs_cursor_free(cursor);
    return failures;
}

#pragma endregion Filtered Scans

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

    report(totals, "scan.predicate_per_page", scan_predicate_per_page, config);
    report(totals, "scan.unordered_matches_ordered", scan_unordered_matches_ordered, config);
    report(totals, "cursor.per_modality", cursors_per_modality, config);
    report(totals, "scan.patterns_and_filters", scan_patterns_and_filters, config);
    report(totals, "cursor.whole_walk", cursor_whole_walk, config);
    report(totals, "cursor.resume_after_writes", cursor_resume_after_writes, config);
    report(totals, "cursor.snapshot_view", cursor_snapshot_view, config);
    report(totals, "cursor.free_waits", cursor_free_waits, config);
    return suite_exit_code(totals);
}
