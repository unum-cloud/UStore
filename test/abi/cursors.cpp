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

#include <algorithm>        // `std::copy`, `std::equal`, `std::sort`
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

#include "abi/fixture.hpp" // `write_text`, `held_scan_t`, `wait_held`, `open_cursor`, `pull_of`, `expect_page`
#include "harness.hpp"     // `finish_test_run`, `expect_true`, `expect_equal`, `expect_near`, `expect_status`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::test::abi;
using namespace unum;

#pragma region Cursors

/** A blobs cursor walks a collection whole in pages with the values of its keys, and a strings
 *  cursor walks one prefix whole, each pulling nothing once a page came back short. */
case_result_t test_cursor_whole_walk(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!open_status.failed() && !arena_status.failed())) return {};
    auto [blobs, blobs_status] = create_collection(database.handle(), "cursor.walk", ustore_modality_blobs_k);
    auto [strings, strings_status] = create_collection(database.handle(), "cursor.walk.strings",
                                                       ustore_modality_strings_k);
    if (!expect_true(!blobs_status.failed() && !strings_status.failed())) return {};

    std::array<ustore_key_t, 10> keys;
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(100 + index);
    write_text(database.handle(), blobs, keys, "walked");
    auto [cursor, cursor_status] = open_cursor(database.handle(), blobs);
    if (!expect_true(!cursor_status.failed())) return {};
    ustore_outcome_t outcome {};
    for (std::size_t first : {0u, 4u, 8u, 10u, 10u}) {
        pulled_keys_t pulled;
        ustore_blobs_cursor_next_t next = pull_of(database.handle(), cursor, pulled, arena.handle(), 4, outcome);
        ustore_blobs_cursor_next(&next);
        expect_success(outcome);
        std::span<ustore_key_t const> const wanted {keys.data() + first, fu::min_of<std::size_t>(4, 10 - first)};
        expect_page(pulled, wanted, "walked");
    }
    ustore_blobs_cursor_free(cursor);

    std::array<std::string_view, 4> const texts {"b:1", "a:3", "a:1", "a:2"};
    std::array<ustore_key_t, 4> const text_keys {key_from_integer(21), key_from_integer(13), key_from_integer(11),
                                                 key_from_integer(12)};
    strided_strings<std::string_view> const accessed {strided(texts).begin()};
    ustore_strings_write_t write {.database = database.handle(),
                                  .outcome = &outcome,
                                  .count = texts.size(),
                                  .collections = {&strings, 0},
                                  .strings = accessed.view(),
                                  .values = {text_keys.data(), sizeof(ustore_key_t)}};
    ustore_strings_write(&write);
    expect_success(outcome);
    ustore_strings_cursor_t walk = nullptr;
    ustore_strings_cursor_init_t init {.cursor = &walk,
                                       .database = database.handle(),
                                       .outcome = &outcome,
                                       .collection = strings,
                                       .prefix = bytes_of("a:")};
    ustore_strings_cursor_init(&init);
    expect_success(outcome);
    std::array<std::string_view, 3> const prefixed {"a:1", "a:2", "a:3"};
    std::array<ustore_key_t, 3> const prefixed_keys {text_keys[2], text_keys[3], text_keys[1]};
    for (std::size_t first : {0u, 2u, 3u}) {
        ustore_strings_view_t found {};
        ustore_key_t const* values = nullptr;
        ustore_size_t written = 0;
        ustore_strings_cursor_next_t next {.database = database.handle(),
                                           .outcome = &outcome,
                                           .cursor = walk,
                                           .strings = &found,
                                           .values = &values,
                                           .written = &written,
                                           .arena = arena.handle(),
                                           .wanted = 2};
        ustore_strings_cursor_next(&next);
        expect_success(outcome);
        expect_equal(written, fu::min_of<std::size_t>(2, 3 - first));
        for (std::size_t index = 0; index != written && first + index != prefixed.size(); ++index) {
            expect_equal(util::chars_of(util::tape_string(found, index)), prefixed[first + index]);
            expect_equal(values[index], prefixed_keys[first + index]);
        }
    }
    ustore_strings_cursor_free(walk);
    return {};
}

/** A cursor keeps only the key its next pull starts at, so keys written behind it stay behind and
 *  keys written ahead of it come, until a short page exhausts it. */
case_result_t test_cursor_resume_after_writes(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!open_status.failed() && !arena_status.failed())) return {};
    auto [collection, create_status] = create_collection(database.handle(), "cursor.resume", ustore_modality_blobs_k);
    if (!expect_true(!create_status.failed())) return {};

    std::array<ustore_key_t, 4> const written {key_from_integer(10), key_from_integer(20), key_from_integer(30),
                                               key_from_integer(40)};
    write_text(database.handle(), collection, written, "first");
    auto [cursor, cursor_status] = open_cursor(database.handle(), collection);
    if (!expect_true(!cursor_status.failed())) return {};
    ustore_outcome_t outcome {};
    pulled_keys_t pulled;
    ustore_blobs_cursor_next_t next = pull_of(database.handle(), cursor, pulled, arena.handle(), 2, outcome);
    ustore_blobs_cursor_next(&next);
    expect_success(outcome);
    expect_page(pulled, {written.data(), 2}, "first");

    std::array<ustore_key_t, 2> const later {key_from_integer(15), key_from_integer(35)};
    write_text(database.handle(), collection, later, "later");
    next.wanted = 8;
    ustore_blobs_cursor_next(&next);
    expect_success(outcome);
    expect_equal(pulled.written, 3u);
    if (pulled.written == 3) {
        expect_equal(pulled.keys[0], key_from_integer(30));
        expect_equal(pulled.keys[1], key_from_integer(35));
        expect_equal(pulled.value(1), "later");
        expect_equal(pulled.keys[2], key_from_integer(40));
    }

    ustore_key_t const beyond = key_from_integer(50);
    write_text(database.handle(), collection, {&beyond, 1}, "beyond");
    ustore_blobs_cursor_next(&next);
    expect_success(outcome);
    expect_equal(pulled.written, 0u);
    expect_true(pulled.keys == nullptr && !tape_written(pulled.values));
    ustore_blobs_cursor_free(cursor);
    return {};
}

/** A cursor through a snapshot walks what the snapshot saw, and holds the snapshot after its
 *  holder frees it. */
case_result_t test_cursor_snapshot_view(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!open_status.failed() && !arena_status.failed())) return {};
    if (!probed(database.supports_snapshots())) return case_result_t::skipped(needs_snapshots_k);
    auto [collection, create_status] = create_collection(database.handle(), "cursor.snapshot", ustore_modality_blobs_k);
    if (!expect_true(!create_status.failed())) return {};

    std::array<ustore_key_t, 3> const seen {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    write_text(database.handle(), collection, seen, "seen");
    ustore_outcome_t outcome {};
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database.handle(), .outcome = &outcome};
    ustore_snapshot_init(&snapshot_init);
    if (!expect_success(outcome)) return {};
    ustore_key_t const unseen = key_from_integer(4);
    write_text(database.handle(), collection, {&unseen, 1}, "unseen");
    ustore_blobs_remove_t remove {.database = database.handle(),
                                  .outcome = &outcome,
                                  .count = 1,
                                  .collections = {&collection, 0},
                                  .keys = {&seen[1], 0}};
    ustore_blobs_remove(&remove);
    expect_success(outcome);

    auto [cursor, cursor_status] = open_cursor(database.handle(), collection, ustore_snapshot_view(snapshot));
    bool const opened = expect_true(!cursor_status.failed());
    ustore_snapshot_free(snapshot);
    if (!opened) return {};
    pulled_keys_t pulled;
    ustore_blobs_cursor_next_t next = pull_of(database.handle(), cursor, pulled, arena.handle(), 8, outcome);
    ustore_blobs_cursor_next(&next);
    expect_success(outcome);
    expect_page(pulled, seen, "seen");
    ustore_blobs_cursor_free(cursor);
    return {};
}

/** What a thread freeing a cursor saw of the deferred pull naming it once the free returned. */
struct cursor_freeing_t {
    ustore_blobs_cursor_t cursor = nullptr;
    hold_stage_t const* stage = nullptr;
    hold_stage_t stage_after_free = hold_stage_t::submitted_k;
    std::uint32_t freed = 0;
};

/** Frees the cursor of @p freeing, then records how far the pull naming it got. */
void free_cursor(cursor_freeing_t& freeing) noexcept {
    ustore_blobs_cursor_free(freeing.cursor);
    freeing.stage_after_free = fu::standard_atomic_ref<hold_stage_t const> {*freeing.stage}.load(
        std::memory_order_acquire);
    fu::standard_atomic_ref<std::uint32_t> {freeing.freed}.store(1, std::memory_order_release);
}

/** Freeing a cursor waits until the deferred pull naming it ends, here a pull held inside its
 *  progress callback, which has returned before the free does. */
case_result_t test_cursor_free_waits(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    auto [executor, spawn_status] = spawn_executor();
    auto [freeing_thread, thread_status] = util::executor_t::make(1, util::caller_exclusivity_t::caller_exclusive_k);
    auto [collection, create_status] = create_collection(database.handle(), "cursor.free", ustore_modality_blobs_k);
    if (!expect_true(!spawn_status.failed() && !st::failed(thread_status) && !create_status.failed())) return {};
    auto [arena, arena_status] = arena_t::make({.executor = executor});
    std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
    write_text(database.handle(), collection, keys, "held");
    auto [cursor, cursor_status] = open_cursor(database.handle(), collection);
    if (!expect_true(!arena_status.failed() && !cursor_status.failed())) return {};

    held_scan_t held {.executor = executor};
    ustore_outcome_t outcome {};
    pulled_keys_t pulled;
    ustore_blobs_cursor_next_t next = pull_of(database.handle(), cursor, pulled, arena.handle(), 8, outcome);
    next.executor = executor;
    next.completion = ustore_completion_deferred_k;
    next.progress = &hold_until_released;
    next.progress_state = &held;
    ustore_blobs_cursor_next(&next);
    wait_held(held);

    cursor_freeing_t freeing {.cursor = cursor, .stage = &held.stage};
    auto const [generation, start_status] = freeing_thread.start<&free_cursor>(freeing);
    expect_true(!st::failed(start_status));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    expect_true(fu::standard_atomic_ref<std::uint32_t> {freeing.freed}.load(std::memory_order_acquire) == 0);

    fu::standard_atomic_ref<hold_stage_t> {held.stage}.store(hold_stage_t::released_k, std::memory_order_release);
    if (!st::failed(start_status)) freeing_thread.join(generation);
    else free_cursor(freeing);
    expect_true(freeing.stage_after_free == hold_stage_t::returned_k);
    expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    expect_page(pulled, keys, "held");
    ustore_executor_free(executor);
    return {};
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
void test_scan_predicate_per_page(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!open_status.failed() && !arena_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "scan_predicate_per_page.blobs",
                                                   ustore_modality_blobs_k);
    auto [graph, graph_status] = create_collection(database.handle(), "scan_predicate_per_page.graph",
                                                   ustore_modality_graph_k);
    if (!expect_true(!blobs_status.failed() && !graph_status.failed())) return;

    std::array<ustore_key_t, 10> keys {};
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(1 + index);
    write_text(database.handle(), blobs, keys, "value");
    ustore_outcome_t outcome {};
    ustore_graph_write_vertices_t write_vertices {.database = database.handle(),
                                                  .outcome = &outcome,
                                                  .count = keys.size(),
                                                  .collections = {&graph, 0},
                                                  .vertices = {keys.data(), sizeof(ustore_key_t)}};
    ustore_graph_write_vertices(&write_vertices);
    expect_success(outcome);

    for (ustore_predicate_cost_t const cost : {ustore_predicate_cost_cheap_k, ustore_predicate_cost_expensive_k}) {
        even_keys_predicate_t counted;
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* found = nullptr;
        ustore_blobs_scan_t scan {.database = database.handle(),
                                  .outcome = &outcome,
                                  .count = 1,
                                  .collections = {&blobs, 0},
                                  .predicate = {&admit_even_keys, &counted, cost},
                                  .offsets = &offsets,
                                  .keys = &found,
                                  .arena = arena.handle(),
                                  .wanted = 3};
        ustore_blobs_scan(&scan);
        if (!expect_success(outcome)) return;
        // Pages [1,3] and [4,6] keep 2, 4 and 6, so the third page is never listed
        expect_equal(offsets[1], 3u);
        expect_true(found[0] == keys[1] && found[1] == keys[3] && found[2] == keys[5]);
        expect_equal(counted.calls, 2u);
        expect_equal(counted.asked, 6u);

        even_keys_predicate_t vertices_counted;
        ustore_graph_scan_vertices_t vertices {.database = database.handle(),
                                               .outcome = &outcome,
                                               .count = 1,
                                               .collections = {&graph, 0},
                                               .predicate = {&admit_even_keys, &vertices_counted, cost},
                                               .offsets = &offsets,
                                               .keys = &found,
                                               .arena = arena.handle(),
                                               .wanted = 4};
        ustore_graph_scan_vertices(&vertices);
        if (!expect_success(outcome)) return;
        // Pages [1,4], [5,8] and the short [9,10] keep the five even vertices, of which four fit
        expect_equal(offsets[1], 4u);
        expect_true(found[3] == keys[7]);
        expect_equal(vertices_counted.calls, 2u);
    }

    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* found = nullptr;
    ustore_blobs_scan_t refused {.database = database.handle(),
                                 .outcome = &outcome,
                                 .count = 1,
                                 .collections = {&blobs, 0},
                                 .predicate = {&admit_even_keys, nullptr, static_cast<ustore_predicate_cost_t>(7)},
                                 .offsets = &offsets,
                                 .keys = &found,
                                 .arena = arena.handle(),
                                 .wanted = 3};
    ustore_blobs_scan(&refused);
    expect_status(status_of(outcome), status_code_t::invalid_argument_k);
}

/** An unordered scan of a collection written once lists the same set of keys as the ordered one. */
void test_scan_unordered_matches_ordered(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    auto [other_arena, other_arena_status] = arena_t::make();
    if (!expect_true(!open_status.failed() && !arena_status.failed() && !other_arena_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "scan_unordered.blobs", ustore_modality_blobs_k);
    if (!expect_true(!blobs_status.failed())) return;
    std::array<ustore_key_t, 40> keys {};
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(7000 + index * 13);
    write_text(database.handle(), blobs, keys, "value");

    auto list = [&](ustore_scan_mode_t mode, ustore_arena_t into, std::span<ustore_key_t const>& listed) {
        ustore_outcome_t outcome {};
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* found = nullptr;
        ustore_blobs_scan_t scan {.database = database.handle(),
                                  .outcome = &outcome,
                                  .count = 1,
                                  .collections = {&blobs, 0},
                                  .offsets = &offsets,
                                  .keys = &found,
                                  .arena = into,
                                  .wanted = keys.size() * 2,
                                  .mode = mode};
        ustore_blobs_scan(&scan);
        bool const answered = expect_success(outcome) && expect_true(offsets != nullptr);
        if (answered) listed = {found + offsets[0], found + offsets[1]};
        return answered;
    };
    std::span<ustore_key_t const> ordered, unordered;
    bool const listed_ordered = list(ustore_scan_ordered_k, arena.handle(), ordered);
    if (!list(ustore_scan_unordered_k, other_arena.handle(), unordered) || !listed_ordered) return;
    expect_equal(ordered.size(), keys.size());
    std::array<ustore_key_t, keys.size()> sorted {};
    if (!expect_equal(unordered.size(), keys.size())) return;
    std::copy(unordered.begin(), unordered.end(), sorted.begin());
    std::sort(sorted.begin(), sorted.end());
    expect_true(std::equal(sorted.begin(), sorted.end(), ordered.begin(), ordered.end()));
}

/** Documents, pages and vertices each walk through a cursor of their own, page after page, with a
 *  predicate compiled once at init narrowing every page, and documents and pages read their values
 *  in the same pull. */
void test_cursors_per_modality(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!open_status.failed() && !arena_status.failed())) return;
    auto [docs, docs_status] = create_collection(database.handle(), "cursors.docs", ustore_modality_docs_k);
    auto [graph, graph_status] = create_collection(database.handle(), "cursors.graph", ustore_modality_graph_k);
    auto [pages, pages_status] = create_collection(database.handle(), "cursors.pages", ustore_modality_pages_k);
    if (!expect_true(!docs_status.failed() && !graph_status.failed() && !pages_status.failed())) return;

    std::array<ustore_key_t, 5> keys {};
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(2400 + index);
    std::string_view const text = R"({"a":1})";
    strided_strings<std::string_view> const documents {{&text, 0}};
    ustore_outcome_t outcome {};
    ustore_docs_write_t write_documents {.database = database.handle(),
                                         .outcome = &outcome,
                                         .count = keys.size(),
                                         .collections = {&docs, 0},
                                         .keys = {keys.data(), sizeof(ustore_key_t)},
                                         .values = documents.view()};
    ustore_docs_write(&write_documents);
    expect_success(outcome);
    ustore_graph_write_vertices_t write_vertices {.database = database.handle(),
                                                  .outcome = &outcome,
                                                  .count = keys.size(),
                                                  .collections = {&graph, 0},
                                                  .vertices = {keys.data(), sizeof(ustore_key_t)}};
    ustore_graph_write_vertices(&write_vertices);
    expect_success(outcome);
    std::string_view const markup = "<a/>";
    strided_strings<std::string_view> const markups {{&markup, 0}};
    ustore_pages_import_t import {};
    import.database = database.handle(), import.outcome = &outcome, import.count = keys.size(),
    import.collections = {&pages, 0};
    import.keys = {keys.data(), sizeof(ustore_key_t)}, import.values = markups.view();
    import.encoding = ustore_page_encoding_xml_k;
    ustore_pages_import(&import);
    if (!expect_success(outcome)) return;

    // Keys 2400 to 2404 hold three even ones, which pages of two hand out as two and then one
    auto walk = [&]<typename init_type_, typename next_type_, typename cursor_type_>(
                    void (*init)(init_type_*), void (*next)(next_type_*), void (*free)(cursor_type_),
                    ustore_collection_id_t collection) {
        even_keys_predicate_t counted;
        cursor_type_ cursor = nullptr;
        init_type_ opened {};
        opened.cursor = &cursor, opened.database = database.handle(), opened.outcome = &outcome;
        opened.collection = collection, opened.start_key = keys[0];
        opened.predicate = {&admit_even_keys, &counted, ustore_predicate_cost_expensive_k};
        init(&opened);
        if (!expect_success(outcome)) return;
        std::array<std::size_t, 3> const pulled {2, 1, 0};
        for (std::size_t const expected_count : pulled) {
            ustore_key_t const* found = nullptr;
            ustore_strings_view_t values {};
            ustore_size_t written = 0;
            next_type_ pull {};
            pull.database = database.handle(), pull.outcome = &outcome, pull.cursor = cursor, pull.keys = &found;
            pull.written = &written, pull.arena = arena.handle(), pull.wanted = 2;
            if constexpr (requires { pull.values; }) pull.values = &values;
            if constexpr (requires { pull.outputs_encoding; }) pull.outputs_encoding = ustore_page_encoding_xml_k;
            next(&pull);
            expect_success(outcome);
            expect_equal(written, expected_count);
            if (written) expect_true(found[0].uuid.bytes[15] % 2 == 0);
            if constexpr (requires { pull.values; })
                if (written) expect_true(tape_written(values));
        }
        free(cursor);
    };
    walk(&ustore_docs_cursor_init, &ustore_docs_cursor_next, &ustore_docs_cursor_free, docs);
    walk(&ustore_pages_cursor_init, &ustore_pages_cursor_next, &ustore_pages_cursor_free, pages);
    walk(&ustore_graph_vertices_cursor_init, &ustore_graph_vertices_cursor_next, &ustore_graph_vertices_cursor_free,
         graph);
}

/** Patterns narrow blobs and strings scans by their values, filters narrow documents and pages, a
 *  pattern that does not compile is refused with StringZilla's message, and a cursor keeps the
 *  patterns it compiled at init. */
void test_scan_patterns_and_filters(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!open_status.failed() && !arena_status.failed())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "scan_patterns.blobs", ustore_modality_blobs_k);
    auto [docs, docs_status] = create_collection(database.handle(), "scan_patterns.docs", ustore_modality_docs_k);
    auto [pages, pages_status] = create_collection(database.handle(), "scan_patterns.pages", ustore_modality_pages_k);
    if (!expect_true(!blobs_status.failed() && !docs_status.failed() && !pages_status.failed())) return;

    std::array<ustore_key_t, 4> keys {};
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(3100 + index);
    std::array<std::string_view, 4> const fruits {"Apple", "banana", "cherry", "avocado"};
    std::array<std::string_view, 4> const documents {R"({"age":25})", R"({"age":40})", R"({"name":"x"})",
                                                     R"({"age":31})"};
    std::array<std::string_view, 4> const markups {"<a><b/></a>", "<a/>", "<c><b>t</b></c>", "<a><c/></a>"};
    ustore_outcome_t outcome {};
    for (std::size_t index = 0; index != keys.size(); ++index)
        write_text(database.handle(), blobs, {&keys[index], 1}, fruits[index]);
    strided_strings<std::string_view> const documents_view {strided(documents).begin()};
    ustore_docs_write_t write_documents {.database = database.handle(),
                                         .outcome = &outcome,
                                         .count = keys.size(),
                                         .collections = {&docs, 0},
                                         .keys = {keys.data(), sizeof(ustore_key_t)},
                                         .values = documents_view.view()};
    ustore_docs_write(&write_documents);
    expect_success(outcome);
    strided_strings<std::string_view> const markups_view {strided(markups).begin()};
    ustore_pages_import_t import {};
    import.database = database.handle(), import.outcome = &outcome, import.count = keys.size(),
    import.collections = {&pages, 0};
    import.keys = {keys.data(), sizeof(ustore_key_t)}, import.values = markups_view.view();
    import.encoding = ustore_page_encoding_xml_k;
    ustore_pages_import(&import);
    if (!expect_success(outcome)) return;

    // Lists through @p scan with its match fields set by @p narrow, comparing with the indices
    // of @p wanted
    auto expect_matched = [&]<typename scan_type_>(void (*scan)(scan_type_*), ustore_collection_id_t collection,
                                                   auto&& narrow, std::initializer_list<std::size_t> wanted,
                                                   status_code_t refused = status_code_t::success_k) {
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* found = nullptr;
        scan_type_ arguments {};
        arguments.database = database.handle(), arguments.outcome = &outcome, arguments.count = 1;
        arguments.collections = {&collection, 0}, arguments.offsets = &offsets, arguments.keys = &found;
        arguments.arena = arena.handle(), arguments.wanted = 8;
        narrow(arguments);
        scan(&arguments);
        if (refused != status_code_t::success_k) {
            expect_status(outcome, refused);
            return;
        }
        if (!expect_success(outcome) || !expect_true(offsets != nullptr)) return;
        expect_equal(static_cast<std::size_t>(offsets[1]), wanted.size());
        std::size_t position = 0;
        for (std::size_t const index : wanted)
            if (position < offsets[1]) expect_true(found[position++] == keys[index]);
    };
    // Every call reuses one pair of offsets, read only while the call compiles its patterns
    std::array<std::uint32_t, 2> offsets {};
    auto patterns_of = [&](std::string_view const& text, ustore_pattern_kind_t kind,
                           ustore_case_sensitivity_t sensitivity = ustore_cased_k) {
        offsets = {0, static_cast<std::uint32_t>(text.size())};
        return ustore_patterns_t {.patterns = util::u32_tape(text.data(), offsets.data()),
                                  .count = 1,
                                  .kind = kind,
                                  .sensitivity = sensitivity};
    };
    std::string_view const starts_with_a = "^a";
    std::string_view const an = "an";
    std::string_view const unbalanced = "(";
    expect_matched(
        &ustore_blobs_scan, blobs,
        [&](ustore_blobs_scan_t& scan) { scan.patterns = patterns_of(starts_with_a, ustore_pattern_regex_k); }, {3});
    expect_matched(&ustore_blobs_scan, blobs,
                   [&](ustore_blobs_scan_t& scan) {
                       scan.patterns = patterns_of(starts_with_a, ustore_pattern_regex_k, ustore_uncased_k);
                   },
                   {0, 3});
    expect_matched(&ustore_blobs_scan, blobs,
                   [&](ustore_blobs_scan_t& scan) { scan.patterns = patterns_of(an, ustore_pattern_substrings_k); },
                   {1});
    expect_matched(
        &ustore_blobs_scan, blobs,
        [&](ustore_blobs_scan_t& scan) { scan.patterns = patterns_of(unbalanced, ustore_pattern_regex_k); }, {},
        status_code_t::invalid_argument_k);

    // A weighted substring scores BM25 with no length normalization: "banana" holds "an" twice,
    // overlapping or not
    float const weight = 2.0f;
    float const* scores = nullptr;
    expect_matched(&ustore_blobs_scan, blobs,
                   [&](ustore_blobs_scan_t& scan) {
                       scan.patterns = patterns_of(an, ustore_pattern_substrings_k);
                       scan.patterns.weights = &weight;
                       scan.scores = &scores;
                   },
                   {1});
    expect_true(scores != nullptr);
    if (scores) expect_near(scores[0], 2.0 * (2.0 * 2.2) / (2.0 + 1.2), 1e-3);
    expect_matched(
        &ustore_blobs_scan, blobs,
        [&](ustore_blobs_scan_t& scan) {
            scan.patterns = patterns_of(starts_with_a, ustore_pattern_regex_k);
            scan.patterns.weights = &weight;
        },
        {}, status_code_t::invalid_argument_k);

    auto [strings, strings_status] = create_collection(database.handle(), "scan_patterns.strings",
                                                       ustore_modality_strings_k);
    expect_true(!strings_status.failed());
    strided_strings<std::string_view> const fruits_view {strided(fruits).begin()};
    ustore_strings_write_t write_strings {.database = database.handle(),
                                          .outcome = &outcome,
                                          .count = keys.size(),
                                          .collections = {&strings, 0},
                                          .strings = fruits_view.view(),
                                          .values = {keys.data(), sizeof(ustore_key_t)}};
    ustore_strings_write(&write_strings);
    expect_success(outcome);
    ustore_length_t const* strings_offsets = nullptr;
    ustore_strings_view_t listed {};
    ustore_strings_scan_t strings_scan {.database = database.handle(),
                                        .outcome = &outcome,
                                        .count = 1,
                                        .collections = {&strings, 0},
                                        .patterns = patterns_of(starts_with_a, ustore_pattern_regex_k),
                                        .offsets = &strings_offsets,
                                        .strings = &listed,
                                        .arena = arena.handle(),
                                        .wanted = 8};
    ustore_strings_scan(&strings_scan);
    expect_success(outcome);
    // Strings come in byte order, where "Apple" sorts first but only "avocado" starts with
    // a lowercase "a"
    expect_true(strings_offsets && strings_offsets[1] == 1 && util::tape_string(listed, 0).length == 7);

    std::string_view const older = "?@.age > 30";
    std::string_view const holds_b = "//b";
    expect_matched(&ustore_docs_scan, docs, [&](ustore_docs_scan_t& scan) { scan.filter = bytes_of(older); }, {1, 3});
    expect_matched(&ustore_pages_scan, pages, [&](ustore_pages_scan_t& scan) { scan.filter = bytes_of(holds_b); },
                   {0, 2});

    ustore_blobs_cursor_t cursor = nullptr;
    ustore_blobs_cursor_init_t opened {.cursor = &cursor,
                                       .database = database.handle(),
                                       .outcome = &outcome,
                                       .collection = blobs,
                                       .start_key = keys[0],
                                       .patterns = patterns_of(an, ustore_pattern_substrings_k)};
    ustore_blobs_cursor_init(&opened);
    if (!expect_success(outcome)) return;
    ustore_key_t const* found = nullptr;
    ustore_size_t written = 0;
    ustore_blobs_cursor_next_t pull {.database = database.handle(),
                                     .outcome = &outcome,
                                     .cursor = cursor,
                                     .keys = &found,
                                     .written = &written,
                                     .arena = arena.handle(),
                                     .wanted = 1};
    ustore_blobs_cursor_next(&pull);
    expect_success(outcome);
    expect_true(written == 1 && found[0] == keys[1]);
    ustore_blobs_cursor_next(&pull);
    expect_success(outcome);
    expect_equal(written, 0u);
    ustore_blobs_cursor_free(cursor);
}

#pragma endregion Filtered Scans

int main(int argc, char** argv) {
    start_test_run(argc, argv);

    auto [directory, directory_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(directory_status)) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    config_t const config {test_run.settings.engine, directory.path()};

    run_test_case(engine_label_t {"test_scan_predicate_per_page"}.c_str(), test_scan_predicate_per_page, config);
    run_test_case(engine_label_t {"test_scan_unordered_matches_ordered"}.c_str(), test_scan_unordered_matches_ordered,
                  config);
    run_test_case(engine_label_t {"test_cursors_per_modality"}.c_str(), test_cursors_per_modality, config);
    run_test_case(engine_label_t {"test_scan_patterns_and_filters"}.c_str(), test_scan_patterns_and_filters, config);
    run_test_case(engine_label_t {"test_cursor_whole_walk"}.c_str(), test_cursor_whole_walk, config);
    run_test_case(engine_label_t {"test_cursor_resume_after_writes"}.c_str(), test_cursor_resume_after_writes, config);
    run_test_case(engine_label_t {"test_cursor_snapshot_view"}.c_str(), test_cursor_snapshot_view, config);
    run_test_case(engine_label_t {"test_cursor_free_waits"}.c_str(), test_cursor_free_waits, config);
    return finish_test_run();
}
