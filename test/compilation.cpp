/**
 *  @file test/compilation.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Calls the C++ wrappers in each of their accepted forms, so a change that breaks a call
 *      site fails to build.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int32_t`
#include <cstdio>  // `stderr`

#include <array>       // `std::array`
#include <concepts>    // `std::same_as`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_convertible_v`, `std::is_move_constructible_v`
#include <utility>     // `std::declval`, `std::forward`, `std::move`

#include <fmt/format.h>

#include <smashtable/basic_vector.hpp>

#include "ustore/ustore.hpp"

#include "harness.hpp" // `suite_exit_code`, `expect`, `suite_totals_t`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum;

/** The binary, document, graph and strings handles whose verbs answer deferred calls. */
using deferred_blobs_t = blobs_collection_on<ustore_completion_deferred_k, collection_access_t::read_write_k>;
using deferred_blobs_read_t = blobs_collection_on<ustore_completion_deferred_k, collection_access_t::read_only_k>;
using deferred_docs_t = docs_collection_on<ustore_completion_deferred_k, collection_access_t::read_write_k>;
using deferred_graph_t = graph_collection_on<ustore_completion_deferred_k, collection_access_t::read_write_k>;
using deferred_strings_t = strings_collection_on<ustore_completion_deferred_k, collection_access_t::read_write_k>;
using deferred_pages_t = pages_collection_on<ustore_completion_deferred_k, collection_access_t::read_write_k>;

/** Whether @p collection_type_ imports pages, which only a handle that writes does. */
template <typename collection_type_>
concept imports_concept = requires(collection_type_ const& collection, std::array<std::string_view, 1> const& markups) {
    collection.import(keys_view_t {}, markups);
};

/** Whether a value can be assigned through a binary collection that @p receiver_type_ finds. */
template <typename receiver_type_>
concept assigns_through_concept = requires(typename decltype(std::declval<receiver_type_&>().find(
    std::string_view {}))::value_type collection) { collection[key_from_integer(1)].assign("x"); };

/** Whether @p collection_type_ reads keys and an arena passed as @p keys_type_
 *  and @p arena_type_. */
template <typename collection_type_, typename keys_type_, typename arena_type_>
concept reads_concept = requires(collection_type_ const& collection, keys_type_&& keys, arena_type_&& arena) {
    collection.read(std::forward<keys_type_>(keys), std::forward<arena_type_>(arena));
};

/** Whether @p collection_type_ removes keys passed as @p keys_type_. */
template <typename collection_type_, typename keys_type_>
concept removes_concept = requires(collection_type_ const& collection, keys_type_&& keys) {
    collection.remove(std::forward<keys_type_>(keys));
};

/** Whether @p collection_type_ hands out a proxy, which only a blocking handle does. */
template <typename collection_type_>
concept addresses_concept = requires(collection_type_ const& collection) { collection[key_from_integer(1)]; };

/** Whether @p result_type_ owns the one-off arena its view reads from: it moves, never copies, and
 *  hands out a view of @p view_type_. */
template <typename result_type_, typename view_type_>
concept owns_arena_concept = std::is_nothrow_move_constructible_v<result_type_> &&
                             !std::is_copy_constructible_v<result_type_> && requires(result_type_ const& result) {
                                 { result.view() } -> std::same_as<view_type_ const&>;
                             };

/* A snapshot hands out read-only collections, so nothing writes through it, while a transaction
 * does */
static_assert(!assigns_through_concept<snapshot_ref_t>);
static_assert(assigns_through_concept<transaction_ref_t>);

/* Every verb is written once: blocking, it answers an outcome, and deferred, a call holding its
 * outputs */
static_assert(
    std::same_as<decltype(std::declval<blobs_collection_t const&>().read(keys_view_t {}, std::declval<arena_t&>())),
                 expected<values_view_t>>);
static_assert(
    std::same_as<decltype(std::declval<deferred_blobs_t const&>().read(keys_view_t {}, std::declval<arena_t&>())),
                 deferred<values_view_t>>);
static_assert(std::same_as<decltype(std::declval<blobs_collection_t const&>().remove(keys_view_t {})), status_t>);
static_assert(std::same_as<decltype(std::declval<deferred_blobs_t const&>().remove(keys_view_t {})), deferred<>>);

/* A deferred verb borrows its inputs as views of any contiguous batch, and an arena by reference,
 * so a temporary arena, which would die before the call is final, never reaches it */
static_assert(reads_concept<deferred_blobs_t, std::array<ustore_key_t, 2>&, arena_t&>);
static_assert(reads_concept<deferred_blobs_t, st::vector<ustore_key_t>&, arena_t&>);
static_assert(reads_concept<deferred_blobs_t, std::span<ustore_key_t const>, arena_t&>);
static_assert(reads_concept<deferred_blobs_t, keys_view_t, arena_t&>);
static_assert(!reads_concept<deferred_blobs_t, keys_view_t, arena_t>);
static_assert(reads_concept<deferred_docs_t, keys_view_t, arena_t&>);
static_assert(reads_concept<deferred_strings_t, values_tape_t&, arena_t&>);
static_assert(reads_concept<deferred_strings_t, std::span<value_view_t const>, arena_t&>);
static_assert(!reads_concept<deferred_strings_t, values_tape_t&, arena_t>);

/* A read-only handle never removes, and a deferred handle hands out no proxy, as a proxy blocks */
static_assert(removes_concept<deferred_blobs_t, keys_view_t>);
static_assert(!removes_concept<deferred_blobs_read_t, keys_view_t>);
static_assert(!removes_concept<blobs_read_collection_t, keys_view_t>);
static_assert(addresses_concept<blobs_collection_t>);
static_assert(!addresses_concept<deferred_blobs_t>);

/* A deferred call never moves, as the library writes into it, while a plan moves, as its finish
 * consumes it */
static_assert(!std::is_move_constructible_v<deferred<values_view_t>> && !std::is_copy_constructible_v<deferred<>>);
static_assert(std::is_nothrow_move_constructible_v<database_compact_plan_t> &&
              !std::is_copy_constructible_v<database_compact_plan_t>);

/* Samples, measures and the pages verbs are written once as well, and a read-only pages handle
 * never imports */
static_assert(std::same_as<decltype(std::declval<deferred_docs_t const&>().sample(1, std::declval<arena_t&>())),
                           deferred<std::span<ustore_key_t const>>>);
static_assert(std::same_as<decltype(std::declval<graph_collection_t const&>().measure_vertices(
                               smallest_key_k, largest_key_k, std::declval<arena_t&>())),
                           expected<size_estimates_t>>);
static_assert(std::same_as<decltype(std::declval<deferred_strings_t const&>().sample(1, std::declval<arena_t&>())),
                           deferred<strings_pages_t>>);
static_assert(
    std::same_as<decltype(std::declval<deferred_pages_t const&>().read(keys_view_t {}, std::declval<arena_t&>())),
                 deferred<values_view_t>>);
static_assert(imports_concept<pages_collection_t> && imports_concept<deferred_pages_t>);
static_assert(!imports_concept<pages_read_collection_t>);

/* A feed, an ingest and an Arrow export own their C handles, so they move but never copy */
static_assert(std::is_nothrow_move_constructible_v<changes_cursor_t> &&
              !std::is_copy_constructible_v<changes_cursor_t>);
static_assert(std::is_nothrow_move_constructible_v<ingest_t> && !std::is_copy_constructible_v<ingest_t>);
static_assert(std::is_nothrow_move_constructible_v<arrow_export_t> && !std::is_copy_constructible_v<arrow_export_t>);

/* A shorthand result owns its one-off arena, while a view only borrows one */
static_assert(owns_arena_concept<owned_value_t, value_view_t>);
static_assert(owns_arena_concept<owned<values_view_t>, values_view_t>);
static_assert(!owns_arena_concept<value_view_t, value_view_t>);

/* A progress callable never throws, as every wrapper is noexcept, and the call borrows it, so a
 * temporary never does */
using continuing_progress_t = decltype([](ustore_size_t, ustore_size_t) noexcept { return ustore_continue_k; });
using throwing_progress_t = decltype([](ustore_size_t, ustore_size_t) { return ustore_continue_k; });
static_assert(std::is_convertible_v<continuing_progress_t&, progress_arg_t>);
static_assert(!std::is_convertible_v<continuing_progress_t, progress_arg_t>);
static_assert(!std::is_convertible_v<throwing_progress_t&, progress_arg_t>);

/* Nor does a predicate throw */
using admitting_predicate_t = decltype([](std::span<ustore_key_t const>, std::span<ustore_admission_t>) noexcept {});
using throwing_predicate_t = decltype([](std::span<ustore_key_t const>, std::span<ustore_admission_t>) {});
static_assert(predicate_concept<admitting_predicate_t>);
static_assert(!predicate_concept<throwing_predicate_t>);

/** Calls every C++ wrapper in each accepted form against one database. */
std::size_t wrappers_compile(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;

    database_ref_t const head = database.on();
    failures += expect(head.clear(ustore_drop_collection_k).succeeded());
    auto [main_collection, create_status] = head.create("main");
    failures += expect(create_status.succeeded());

    // Single-element access
    failures += expect(main_collection[key_from_integer(42)].assign("purpose of life").succeeded());
    failures += expect(main_collection.at(key_from_integer(42)).assign("purpose of life").succeeded());
    [[maybe_unused]] bool const matches = *main_collection[key_from_integer(42)].value() == "purpose of life";
    [[maybe_unused]] status_t const single_cleared = main_collection[key_from_integer(42)].clear_values();

    // Many keys to one value
    failures += expect(main_collection[{key_from_integer(43), key_from_integer(44)}].assign("same value").succeeded());

    // Smart references
    [[maybe_unused]] status_t const cleared =
        main_collection[{key_from_integer(43), key_from_integer(44)}].clear_values();
    [[maybe_unused]] status_t const removed = main_collection[{key_from_integer(43), key_from_integer(44)}].remove();
    [[maybe_unused]] auto const present = main_collection[{key_from_integer(43), key_from_integer(44)}].present();
    [[maybe_unused]] auto const length = main_collection[{key_from_integer(43), key_from_integer(44)}].length();
    [[maybe_unused]] auto const value = main_collection[{key_from_integer(43), key_from_integer(44)}].value();
    [[maybe_unused]] auto const by_array =
        main_collection[std::array<ustore_key_t, 3> {key_from_integer(65), key_from_integer(66), key_from_integer(67)}];
    std::array<ustore_key_t, 4> const four_keys {key_from_integer(65), key_from_integer(66), key_from_integer(67),
                                                 key_from_integer(68)};
    [[maybe_unused]] auto const by_range = main_collection[strided(four_keys)];
    // A range-for keeps a temporary alive only from C++23, so the answer is named before iterating
    auto const listed = main_collection[{key_from_integer(100), key_from_integer(101)}].value();
    for ([[maybe_unused]] value_view_t const element : *listed) {}

    // Named collections
    blobs_collection_t prefixes = *head.find_or_create("prefixes");
    failures += expect(prefixes.at(key_from_integer(42)).assign("purpose").succeeded());
    failures += expect(head["articles"]->at(key_from_integer(42)).assign("of").succeeded());
    failures += expect(head["suffixes"]->at(key_from_integer(42)).assign("life").succeeded());

    // Reusable memory
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    [[maybe_unused]] auto const arena_present = main_collection[{key_from_integer(43), key_from_integer(44)}].present(
        arena);
    [[maybe_unused]] auto const arena_length = main_collection[{key_from_integer(43), key_from_integer(44)}].length(
        arena);
    [[maybe_unused]] auto const arena_value = main_collection[{key_from_integer(43), key_from_integer(44)}].value(
        arena);
    [[maybe_unused]] expected<values_view_t> const verb_value = main_collection.read(four_keys, arena);

    // Counting and streaming keys
    [[maybe_unused]] auto const keys_count = main_collection.size();
    [[maybe_unused]] auto const pairs_count =
        main_collection.pairs(key_from_integer(100), key_from_integer(200)).size();
    auto maybe_stream = main_collection.members(key_from_integer(100), key_from_integer(200)).keys_begin();
    failures += expect(maybe_stream);
    if (maybe_stream)
        for (keys_stream_t& stream = *maybe_stream; !stream.is_end(); ++stream) {
            [[maybe_unused]] ustore_key_t const key = stream.key();
        }
    [[maybe_unused]] auto const cardinality =
        main_collection.members(key_from_integer(100), key_from_integer(200)).size_estimates()->cardinality;

    // Durability and watching
    [[maybe_unused]] status_t const flushed_clear =
        main_collection[{key_from_integer(43), key_from_integer(44)}].clear_values(ustore_durability_flushed_k);
    [[maybe_unused]] status_t const flushed_remove =
        main_collection[{key_from_integer(43), key_from_integer(44)}].remove(ustore_durability_flushed_k);
    [[maybe_unused]] auto const untracked_present =
        main_collection[{key_from_integer(43), key_from_integer(44)}].present(arena, ustore_watch_none_k);
    [[maybe_unused]] auto const untracked_length = main_collection[{key_from_integer(43), key_from_integer(44)}].length(
        arena, ustore_watch_none_k);
    [[maybe_unused]] auto const untracked_value = main_collection[{key_from_integer(43), key_from_integer(44)}].value(
        ustore_watch_none_k);

    // Sub-documents
    docs_collection_t documents_collection = *head.find_or_create<docs_collection_t>("docs");
    failures += expect(
        documents_collection[key_from_integer(56)].assign(R"({"hello":"world","answer":42})").succeeded());
    [[maybe_unused]] bool const field_matches =
        documents_collection[collection_key_field_t {key_from_integer(56), "hello"}].value() == "world";

    // The same verbs blocking on an executor, and answering deferred calls that borrow their inputs
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 1});
    failures += expect(executor_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database_t::init_ram({}, executor).has_value());
    database_ref_t const head_on_executor = database.on(executor);
    blobs_collection_t on_executor = *head_on_executor.find("main");
    failures += expect(on_executor[key_from_integer(42)].assign("purpose of life").succeeded());
    [[maybe_unused]] auto const on_executor_value = on_executor[key_from_integer(42)].value();
    failures += expect(head_on_executor["articles"]->at(key_from_integer(42)).assign("of").succeeded());

    database_on<ustore_completion_deferred_k> const deferred_head = database.deferred(executor);
    deferred_blobs_t const deferred_main = deferred_head[main_collection];
    std::array<ustore_key_t, 2> const borrowed_keys {key_from_integer(43), key_from_integer(44)};
    values_tape_t blobs;
    failures += fill_tape(blobs, {"left", "right"});
    failures += expect(deferred_main.write(borrowed_keys, blobs).wait().succeeded());
    deferred<values_view_t> reading = deferred_main.read(borrowed_keys, arena);
    [[maybe_unused]] status_code_t const polled = reading.status();
    expected<values_view_t> read = reading.wait();
    failures += expect(read.has_value());
    if (read)
        for ([[maybe_unused]] value_view_t const element : *read) {}
    deferred<> removing = deferred_main.remove(borrowed_keys);
    failures += expect(removing.wait().succeeded());
    failures += expect(main_collection.deferred(executor).clear_values().wait().succeeded());
    expected<deferred_blobs_t> created = deferred_head.create("created").wait();
    failures += expect(created.has_value());
    if (created) {
        failures += expect(deferred_head[*created].remove(borrowed_keys).wait().succeeded());
        failures += expect(deferred_head.drop(*created).wait().succeeded());
    }
    failures += expect(deferred_head.collections(arena).wait().has_value());
    failures += expect(deferred_head.describe().wait().has_value());
    failures += expect(deferred_head.flush().wait().succeeded());

    // Deferred documents, graphs and strings, each verb borrowing its inputs
    failures += expect(head.find_or_create<docs_collection_t>("docs").has_value());
    deferred_docs_t const deferred_documents = *deferred_head.find<docs_collection_t>("docs");
    values_tape_t documents;
    failures += fill_tape(documents, {R"({"hello":"world"})", R"({"hello":"again"})"});
    failures += expect(deferred_documents.write(borrowed_keys, documents).wait().succeeded());
    [[maybe_unused]] expected<values_view_t> const documents_read =
        deferred_documents.read(borrowed_keys, arena).wait();
    values_tape_t hello;
    failures += fill_tape(hello, {"hello"});
    [[maybe_unused]] expected<values_view_t> const fields_read =
        deferred_documents.read(borrowed_keys, arena, hello, ustore_docs_field_type_string_k).wait();
    [[maybe_unused]] expected<sz::strings_view_t> const gist = deferred_documents.gist(borrowed_keys, arena).wait();
    auto const header = make_table_header().with<std::string_view>("hello");
    [[maybe_unused]] expected<docs_table<std::string_view>> const gathered =
        deferred_documents.gather(borrowed_keys, header, arena).wait();
    failures += expect(deferred_documents.remove(borrowed_keys).wait().succeeded());
    failures += expect(head.create<graph_collection_t>("graph").has_value());
    deferred_graph_t const deferred_graph = *deferred_head.find<graph_collection_t>("graph");
    std::array<edge_t, 1> const borrowed_edges {edge_t {key_from_integer(43), key_from_integer(44)}};
    ustore_vertex_role_t const leaving = ustore_vertex_role_source_k;
    strided_span<ustore_vertex_role_t const> const roles {{&leaving, 0}, borrowed_keys.size()};
    failures += expect(deferred_graph.write_edges(borrowed_edges).wait().succeeded());
    failures += expect(deferred_graph.write_vertices(borrowed_keys).wait().succeeded());
    [[maybe_unused]] expected<edges_view_t> const edges_read = deferred_graph.read_edges(borrowed_keys, arena).wait();
    [[maybe_unused]] expected<degrees_view_t> const degrees =
        deferred_graph.degrees(borrowed_keys, arena, roles).wait();
    failures += expect(deferred_graph.remove_edges(borrowed_edges).wait().succeeded());
    failures += expect(deferred_graph.remove_vertices(borrowed_keys).wait().succeeded());

    failures += expect(head.create<strings_collection_t>("strings").has_value());
    deferred_strings_t const deferred_strings = *deferred_head.find<strings_collection_t>("strings");
    values_tape_t texts, asked, string_prefixes, removed_strings;
    failures += fill_tape(texts, {"hello", "help"});
    failures += fill_tape(asked, {"hello"});
    failures += fill_tape(string_prefixes, {"hel"});
    failures += fill_tape(removed_strings, {"help"});
    std::array<ustore_key_t, 2> const string_keys {key_from_integer(1), key_from_integer(2)};
    failures += expect(deferred_strings.write(texts, string_keys).wait().succeeded());
    [[maybe_unused]] expected<keys_view_t> const strings_read = deferred_strings.read(asked, arena).wait();
    [[maybe_unused]] expected<strings_pages_t> const scanned = deferred_strings.scan(string_prefixes, 8, arena).wait();
    failures += expect(deferred_strings.remove(removed_strings).wait().succeeded());

    // A plan a cancel leaves live, continued and finished blocking, then deferred
    auto cancelling = [](ustore_size_t, ustore_size_t) noexcept { return ustore_cancel_k; };
    database_compact_plan_t plan;
    [[maybe_unused]] status_t const stopped = head.compact(cancelling, &plan);
    [[maybe_unused]] char const* const phase = ustore_phase_name(plan.phase().phase);
    [[maybe_unused]] status_t const continued = head.compact({}, &plan);
    if (plan.handle()) failures += expect(head.compact_plan_finish(std::move(plan)).succeeded());
    database_compact_plan_t deferred_plan;
    [[maybe_unused]] status_t const deferred_stopped = deferred_head.compact(cancelling, &deferred_plan).wait();
    if (deferred_plan.handle())
        failures += expect(deferred_head.compact_plan_finish(std::move(deferred_plan)).wait().succeeded());
    [[maybe_unused]] status_t const whole = head.compact();

    // Transactions on an executor, and their deferred verbs
    if (head.supports_transactions()) {
        transaction_t transaction = *head_on_executor.transaction();
        [[maybe_unused]] expected<docs_collection_t> const transaction_documents =
            transaction.on().find<docs_collection_t>("docs");
        transaction_on<ustore_completion_deferred_k> const deferred_transaction = transaction.deferred(executor);
        [[maybe_unused]] expected<deferred_blobs_t> const deferred_named = deferred_transaction["docs"];
        failures += expect(deferred_transaction.find("main")->remove(borrowed_keys).wait().succeeded());
        [[maybe_unused]] expected<read_stamp_t> const staged = deferred_transaction.stage().wait();
        [[maybe_unused]] status_t const rolled_back = deferred_transaction.rollback().wait();
        [[maybe_unused]] expected<commit_stamp_t> const committed = deferred_transaction.commit().wait();
        failures += expect(deferred_transaction.reset().wait().has_value());
    }

    // Snapshots hand out the read-only twin of every collection
    if (head.supports_snapshots()) {
        snapshot_t snapshot = *head_on_executor.snapshot();
        [[maybe_unused]] read_stamp_t const snapshot_stamp = snapshot.read_stamp();
        [[maybe_unused]] auto const snapshot_value = snapshot.on().find("main")->at(key_from_integer(42)).value();
        [[maybe_unused]] expected<docs_read_collection_t> const snapshot_documents =
            snapshot.on().find<docs_collection_t>("docs");
        [[maybe_unused]] expected<graph_read_collection_t> const snapshot_graph =
            snapshot.on().find<graph_collection_t>("graph");
        [[maybe_unused]] expected<strings_read_collection_t> const snapshot_strings =
            snapshot.on().find<strings_collection_t>("strings");
        deferred_blobs_read_t const deferred_snapshot = *snapshot.deferred(executor).find("main");
        failures += expect(deferred_snapshot.read(borrowed_keys, arena).wait().has_value());
        auto const deferred_snapshot_documents = *snapshot.deferred(executor).find<docs_collection_t>("docs");
        failures += expect(deferred_snapshot_documents.read(borrowed_keys, arena).wait().has_value());
        if (config.directory()) {
            path_t const saved {config.directory(), "saved"};
            failures += expect(create_directories(saved.c_str()));
            path_t const target = save_target(config, saved);
            failures += expect(snapshot.deferred(executor).save(target.c_str()).wait().succeeded());
        }
    }

    failures += expect(head.clear(ustore_drop_collection_k).succeeded());
    return failures;
}

/** Patterns, weighted scores, a documents filter and a predicate reach the scans and cursors of the
 *  C++ wrappers. */
std::size_t wrappers_match(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    blobs_collection_t fruits = *database.on().find_or_create("fruits");
    docs_collection_t people = *database.on().find_or_create<docs_collection_t>("people");
    if (failures != 0) return failures;
    std::array<char const*, 4> const names {"Apple", "banana", "cherry", "avocado"};
    std::array<char const*, 4> const documents {R"({"age":25})", R"({"age":40})", R"({"name":"x"})", R"({"age":31})"};
    for (std::size_t index = 0; index != names.size(); ++index) {
        failures += expect(fruits[key_from_integer(index + 1)].assign(names[index]).succeeded());
        failures += expect(people[key_from_integer(index + 1)].assign(documents[index]).succeeded());
    }
    receiver_t const receiver = database.receiver();

    std::string_view const substring = "an";
    std::array<std::uint32_t, 2> const offsets {0, static_cast<std::uint32_t>(substring.size())};
    float const weight = 2.0f;
    match_t const weighted {.patterns = {.patterns = u32_tape(substring.data(), offsets.data()),
                                         .count = 1,
                                         .kind = ustore_pattern_substrings_k,
                                         .weights = &weight}};
    float const* scores = nullptr;
    auto [scored, scored_status] = scan_keys_through<ustore_blobs_scan_t, &ustore_blobs_scan>(
        receiver, fruits, smallest_key_k, 8, arena, weighted, &scores);
    failures += expect(scored_status.succeeded() && scored.size() == 1 && scored[0] == key_from_integer(2));
    failures += expect(scores != nullptr);
    if (scores) failures += expect_near(scores[0], 2.0 * (2.0 * 2.2) / (2.0 + 1.2));

    std::string_view const older = "?@.age > 30";
    std::size_t asked = 0;
    auto even_keys = [&](std::span<ustore_key_t const> keys, std::span<ustore_admission_t> admissions) noexcept {
        ++asked;
        for (std::size_t index = 0; index != keys.size(); ++index)
            admissions[index] = keys[index].uuid.bytes[15] % 2 == 0 ? ustore_admit_k : ustore_reject_k;
    };
    match_t const filtered {.filter = bytes_of(older), .predicate = predicate_of(even_keys)};
    auto [cursor, cursor_status] = docs_cursor_t::make(receiver, people, smallest_key_k, filtered);
    failures += expect(cursor_status.succeeded());
    if (failures != 0) return failures;
    auto [page, page_status] = cursor.next_pairs(8, arena);
    failures += expect(page_status.succeeded() && page.keys.size() == 2 && page.keys[1] == key_from_integer(4));
    failures += expect(asked != 0);
    failures += expect(database.on().clear(ustore_drop_collection_k).succeeded());
    return failures;
}

/** The samples and measures of strings, documents, graph vertices and pages, blocking and deferred,
 *  and the refusals of a collection holding another modality and of a dropped one. */
std::size_t wrappers_sample_measure(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    database_ref_t const head = database.on();
    failures += expect(head.clear(ustore_drop_collection_k).succeeded());
    auto [arena, arena_status] = arena_t::make();
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 1});
    auto words = head.create<strings_collection_t>("words");
    auto people = head.create<docs_collection_t>("people");
    auto graph = head.create<graph_collection_t>("graph");
    failures += expect(arena_status.succeeded() && executor_status.succeeded() && words && people && graph);
    if (failures != 0) return failures;

    std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
    values_tape_t texts, documents;
    failures += fill_tape(texts, {"apple", "apricot"});
    failures += fill_tape(documents, {R"({"a":1})", R"({"a":2})"});
    failures += expect(words->write(texts, keys).succeeded());
    failures += expect(people->write(keys, documents).succeeded());
    failures += expect(graph->write_edge(edge_t {keys[0], keys[1]}).succeeded());

    auto const holds = [](expected<size_estimates_t> const& bounds, std::size_t count) {
        return bounds && bounds->cardinality.min <= count && count <= bounds->cardinality.max;
    };
    value_view_t const unbounded = value_view_t::make_empty();
    auto [drawn_words, drawn_words_status] = words->sample(8, arena);
    failures += expect(drawn_words_status.succeeded() && drawn_words.strings.size() == 2);
    failures += expect(holds(words->measure(unbounded, unbounded, arena), 2));
    value_view_t const from = "apricot";
    failures += expect(holds(words->deferred(executor).measure(from, unbounded, arena).wait(), 1));

    auto [drawn_people, drawn_people_status] = people->sample(8, arena);
    failures += expect(drawn_people_status.succeeded() && drawn_people.size() == 2);
    failures += expect(holds(people->measure(smallest_key_k, largest_key_k, arena), 2));
    failures += expect_eq(people->deferred(executor).sample(1, arena).wait()->size(), 1u);

    auto [drawn_vertices, drawn_vertices_status] = graph->sample_vertices(8, arena);
    failures += expect(drawn_vertices_status.succeeded() && drawn_vertices.size() == 2);
    failures += expect(holds(graph->measure_vertices(smallest_key_k, largest_key_k, arena), 2));
    failures += expect(holds(graph->deferred(executor).measure_vertices(keys[1], largest_key_k, arena).wait(), 1));

    // The library refuses a collection of another modality, and one dropped meanwhile
    docs_collection_t const mistyped {head.receiver(), *words};
    failures += expect_status(mistyped.sample(8, arena), status_code_t::invalid_argument_k);
    failures += expect(graph->drop().succeeded());
    failures += expect_status(graph->sample_vertices(8, arena), status_code_t::not_found_k);
    failures += expect(head.clear(ustore_drop_collection_k).succeeded());
    return failures;
}

/** Every pages verb, blocking and deferred, with the refusals of the binding and the library. */
case_result_t wrappers_pages(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    database_ref_t const head = database.on();
    failures += expect(head.clear(ustore_drop_collection_k).succeeded());
    auto [arena, arena_status] = arena_t::make();
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 1});
    auto [pages, pages_status] = head.create<pages_collection_t>("pages");
    failures += expect(arena_status.succeeded() && executor_status.succeeded() && pages_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 1> const page {key_from_integer(7)};
    std::array<std::string_view, 1> const markup {"<doc><p>one</p></doc>"};
    status_t const imported = pages.import(page, markup);
    if (imported == status_code_t::unsupported_k)
        return case_result_t::skipped("the engine serves no pages yet", failures);
    failures += expect(imported.succeeded());
    auto const rendered = [&](std::string_view wanted) {
        auto [rendering, read_status] = pages.read(page, arena);
        return expect(read_status.succeeded() && rendering.size() == 1 && std::string_view {rendering[0]} == wanted);
    };
    failures += rendered("<doc><p>one</p></doc>");

    // An inserted element takes a minted key, which every later edit names
    std::array<std::string_view, 1> const fragment {"<p>two</p>"};
    auto [landed, insert_status] = pages.insert(page, fragment, arena);
    failures += expect(insert_status.succeeded() && landed.size() == 1);
    if (failures != 0) return failures;
    std::array<ustore_key_t, 1> const element {landed[0]};
    failures += rendered("<p>two</p><doc><p>one</p></doc>");
    std::array<std::string_view, 1> const field {"id"}, attribute {"x"};
    failures += expect(pages.set_attributes(page, element, field, attribute).succeeded());
    failures += rendered(R"(<p id="x">two</p><doc><p>one</p></doc>)");
    failures += expect(pages.remove_attributes(page, element, field).succeeded());
    failures += rendered("<p>two</p><doc><p>one</p></doc>");
    std::array<ustore_length_t, 1> const at_front {0};
    std::array<std::string_view, 1> const lead {"lead"};
    failures += expect(pages.splice(page, page, at_front, at_front, lead).succeeded());
    failures += rendered("lead<p>two</p><doc><p>one</p></doc>");
    failures += expect(pages.move(page, element).succeeded());
    failures += rendered("lead<p>two</p><doc><p>one</p></doc>");
    auto [subtree, subtree_status] = pages.read(page, arena, element, ustore_page_encoding_text_k);
    failures += expect(subtree_status.succeeded() && std::string_view {subtree[0]} == "two");
    failures += expect(pages.remove(page, element).succeeded());
    failures += rendered("lead<doc><p>one</p></doc>");

    auto [drawn, drawn_status] = pages.sample(8, arena);
    failures += expect(drawn_status.succeeded() && drawn.size() == 1 && drawn[0] == page[0]);
    auto [bounds, bounds_status] = pages.measure(smallest_key_k, largest_key_k, arena);
    failures += expect(bounds_status.succeeded() && bounds.cardinality.min <= 1 && 1 <= bounds.cardinality.max);
    deferred_pages_t const later = pages.deferred(executor);
    failures += expect(later.read(page, arena).wait().has_value());
    failures += expect_eq(later.sample(8, arena).wait()->size(), 1u);

    // The binding alone sees two columns disagree, and the library refuses the rest
    std::array<std::string_view, 2> const two_markups {"<a/>", "<b/>"};
    std::array<ustore_key_t, 2> const two_parents {page[0], page[0]};
    failures += expect_status(pages.import(page, two_markups), status_code_t::invalid_argument_k);
    failures += expect_status(later.import(page, two_markups).wait(), status_code_t::invalid_argument_k);
    failures += expect_status(pages.insert(page, fragment, arena, two_parents), status_code_t::invalid_argument_k);
    failures += expect_status(pages.remove(page, {}), status_code_t::invalid_argument_k);
    failures += expect_status(pages.read(page, arena, {}, ustore_page_encoding_html_k), status_code_t::unsupported_k);

    // A range of whole pages goes at once
    std::array<ustore_key_t, 1> const past {key_from_integer(8)};
    failures += expect(pages.remove(page, {}, past).succeeded());
    auto [removed, removed_status] = pages.read(page, arena);
    bits_view_t const found = removed.presences();
    failures += expect(removed_status.succeeded() && !found[0]);
    failures += expect(head.clear(ustore_drop_collection_k).succeeded());
    return failures;
}

/** Keys, strings and a gathered column cross to Arrow and back in place, and the converters and
 *  exporters refuse what C documents. */
std::size_t wrappers_arrow(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    database_ref_t const head = database.on();
    failures += expect(head.clear(ustore_drop_collection_k).succeeded());
    auto [arena, arena_status] = arena_t::make();
    auto blobs = head.create<blobs_collection_t>("blobs");
    auto people = head.create<docs_collection_t>("people");
    failures += expect(arena_status.succeeded() && blobs && people);
    if (failures != 0) return failures;

    std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
    values_tape_t values, documents;
    failures += fill_tape(values, {"left", "right"});
    failures += fill_tape(documents, {R"({"age":25})", R"({"age":40})"});
    failures += expect(blobs->write(keys, values).succeeded());
    failures += expect(people->write(keys, documents).succeeded());

    // Values a read exported cross out and back without a copy
    auto [read, read_status] = blobs->read(keys, arena);
    failures += expect(read_status.succeeded());
    if (failures != 0) return failures;
    auto [strings_column, strings_export_status] = strings_view_to_arrow(read, arena);
    failures += expect(strings_export_status.succeeded() && strings_column.array().array.length == 2);
    if (failures != 0) return failures;
    auto [strings_back, strings_back_status] = strings_view_from_arrow(strings_column.schema(),
                                                                       strings_column.array().array);
    failures += expect(strings_back_status.succeeded() && strings_back.size() == 2);
    if (failures != 0) return failures;
    failures += expect(std::string_view {strings_back[0]} == "left" && std::string_view {strings_back[1]} == "right");

    // Sampled keys sit in the arena, so they export in place
    auto [drawn, drawn_status] = people->sample(8, arena);
    failures += expect(drawn_status.succeeded() && drawn.size() == 2);
    auto [keys_column, keys_export_status] = keys_view_to_arrow(drawn, arena);
    failures += expect(keys_export_status.succeeded() && std::string_view {keys_column.schema().format} == "w:16");
    if (failures != 0) return failures;
    auto [keys_back, keys_back_status] = keys_view_from_arrow(keys_column.schema(), keys_column.array().array);
    failures += expect(keys_back_status.succeeded() && keys_back.size() == 2 && keys_back[0] == drawn[0]);

    // A gathered column exports with its validities
    auto header = make_table_header().with<std::int32_t>("age");
    auto [table, gather_status] = people->gather(keys, header, arena);
    failures += expect(gather_status.succeeded());
    if (failures != 0) return failures;
    auto [ages, ages_status] = docs_column_view_to_arrow(table.column(0), arena);
    failures += expect(ages_status.succeeded() && std::string_view {ages.schema().format} == "i");
    failures += expect(ages_status.succeeded() && ages.array().array.length == 2);

    // Released inputs, keys outside the arena and an accessor are refused
    ArrowSchema const released_schema {};
    ArrowArray const released_array {};
    failures += expect_status(keys_view_from_arrow(released_schema, released_array), status_code_t::invalid_argument_k);
    failures += expect_status(strings_view_from_arrow(released_schema, released_array),
                              status_code_t::invalid_argument_k);
    failures += expect_status(keys_view_to_arrow(keys, arena), status_code_t::invalid_argument_k);
    std::array<std::string_view, 1> const accessed {"x"};
    failures += expect_status(strings_view_to_arrow(strings_view_of(accessed), 1, ustore_strings_accessor_k, arena),
                              status_code_t::invalid_argument_k);
    failures += expect(head.clear(ustore_drop_collection_k).succeeded());
    return failures;
}

/** Ingests land blobs and edges as one commit each, and refuse what C documents. */
case_result_t wrappers_ingest(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    database_ref_t const head = database.on();
    failures += expect(head.clear(ustore_drop_collection_k).succeeded());
    auto blobs = head.create<blobs_collection_t>("blobs");
    auto graph = head.create<graph_collection_t>("graph");
    failures += expect(blobs && graph);
    if (failures != 0) return failures;

    auto [ingest, ingest_status] = head.ingest(*blobs, ustore_ingest_unsorted_k);
    if (ingest_status == status_code_t::unsupported_k)
        return case_result_t::skipped("the engine ingests nothing", failures);
    failures += expect(ingest_status.succeeded());
    if (failures != 0) return failures;
    std::array<ustore_key_t, 2> const keys {key_from_integer(2), key_from_integer(1)};
    values_tape_t values, one_value;
    failures += fill_tape(values, {"two", "one"});
    failures += fill_tape(one_value, {"lone"});
    failures += expect(ingest.append(keys, values).succeeded());
    failures += expect_status(ingest.append(keys, one_value), status_code_t::invalid_argument_k);
    auto const early = (*blobs)[keys[1]].present();
    failures += expect(early && !*early); // nothing lands before the finish
    failures += expect(ingest.finish().has_value());
    failures += expect_status(ingest.finish(), status_code_t::invalid_argument_k);
    auto const landed = (*blobs)[keys[1]].value();
    failures += expect(landed && *landed == "one");

    // Edges ingest as edges, and a sorted ingest refuses a key that does not ascend
    auto [edges_ingest, edges_ingest_status] = head.ingest(*graph);
    failures += expect(edges_ingest_status.succeeded());
    if (failures != 0) return failures;
    std::array<edge_t, 1> const edges {edge_t {keys[1], keys[0]}};
    failures += expect(edges_ingest.append(edges_view_t {edges}).succeeded());
    failures += expect(edges_ingest.finish().has_value());
    failures += expect_eq(*graph->degree(keys[1], ustore_vertex_role_source_k), 1u);
    {
        // An unfinished ingest holds NVMe's maintenance claim, so it goes before the drop below
        auto [sorted_ingest, sorted_ingest_status] = head.ingest(*blobs, ustore_ingest_sorted_k);
        failures += expect(sorted_ingest_status.succeeded());
        if (failures != 0) return failures;
        failures += expect_status(sorted_ingest.append(keys, values), status_code_t::invalid_argument_k);
    }
    failures += expect(head.clear(ustore_drop_collection_k).succeeded());
    return failures;
}

/** A feed hands out the writes and removals committed after a snapshot's stamp, oldest first, and
 *  refuses a dropped collection. */
case_result_t wrappers_changes(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    database_ref_t const head = database.on();
    failures += expect(head.clear(ustore_drop_collection_k).succeeded());
    if (!head.supports_snapshots()) return case_result_t::skipped("the engine keeps no stamps", failures);
    auto [arena, arena_status] = arena_t::make();
    auto blobs = head.create<blobs_collection_t>("blobs");
    auto dropped = head.create<blobs_collection_t>("dropped");
    failures += expect(arena_status.succeeded() && blobs && dropped);
    if (failures != 0) return failures;

    // The snapshot only reads the head's stamp, and goes before the drop it would block
    expected<commit_stamp_t> const head_stamp = [&]() noexcept -> expected<commit_stamp_t> {
        auto [snapshot, snapshot_status] = head.snapshot();
        if (snapshot_status.failed()) return snapshot_status;
        return commit_stamp_t {static_cast<ustore_stamp_t>(snapshot.read_stamp())};
    }();
    failures += expect(head_stamp.has_value());
    if (failures != 0) return failures;
    commit_stamp_t const since = *head_stamp;
    auto [feed, feed_status] = head.changes_cursor(*blobs, since);
    if (feed_status == status_code_t::unsupported_k)
        return case_result_t::skipped("the engine keeps no stamps", failures);
    failures += expect(feed_status.succeeded());
    if (failures != 0) return failures;
    ustore_key_t const key = key_from_integer(1);
    failures += expect((*blobs)[key].assign("a").succeeded());
    failures += expect((*blobs)[key].remove().succeeded());
    auto [changes, changes_status] = feed.next_pairs(8, arena);
    failures += expect(changes_status.succeeded() && changes.keys.size() == 2);
    if (failures != 0) return failures;
    failures += expect(changes.keys[0] == key && changes.keys[1] == key && changes.stamps[0] < changes.stamps[1]);
    failures += expect(changes.kinds[0] == ustore_change_write_k && changes.kinds[1] == ustore_change_removal_k);
    failures += expect(std::string_view {ustore_change_kind_name(changes.kinds[1])} == "removal");
    failures += expect(std::string_view {changes.values[0]} == "a");
    auto [rest, rest_status] = feed.next(8, arena);
    failures += expect(rest_status.succeeded() && rest.keys.empty());

    ustore_collection_id_t const gone = *dropped;
    failures += expect(dropped->drop().succeeded());
    failures += expect_status(head.changes_cursor(gone, since), status_code_t::not_found_k);
    failures += expect(head.clear(ustore_drop_collection_k).succeeded());
    return failures;
}

/** The GPU listing answers how many devices there are, filling no more than it is given. */
std::size_t wrappers_gpu_devices() {
    std::size_t failures = 0;
    auto [count, count_status] = executor_handle_t::gpu_devices();
    failures += expect(count_status.succeeded());
    std::array<ustore_gpu_device_t, 4> devices {};
    auto [listed, listed_status] = executor_handle_t::gpu_devices(devices);
    failures += expect(listed_status.succeeded() && listed == count);
    for (std::size_t index = 0; index != fu::min_of(listed, devices.size()); ++index)
        failures += expect(std::string_view {ustore_gpu_vendor_name(devices[index].vendor)} != "unrecognized");
    return failures;
}

/** The Quickstart of cpp/README.md, line by line. The README's @c main is this function, counting
 *  failures where it returns zero; its @c assert calls are @c expect calls, the first returning
 *  early since the rest would dereference a failed open, and the last split per condition so a
 *  failure names the one it broke. */
std::size_t quickstart() {
    std::size_t failures = 0;
    auto [db, open_status] = database_t::init_ram();
    auto posters = db.on().create<blobs_collection_t>("posters");
    auto movies = db.on().create<docs_collection_t>("movies");
    auto similar = db.on().create<graph_collection_t>("similar");
    failures += expect(open_status.succeeded() && posters && movies && similar);
    if (failures != 0) return failures;

    // One transaction writes a blob, a document and an edge, and commits all three or none
    ustore_key_t const avatar = key_from_integer(1), titanic = key_from_integer(2);
    auto [transaction, transaction_status] = db.on().transaction();
    transaction_ref_t const inside = transaction.on();
    status_t const poster = inside["posters"]->at(avatar).assign("\x89PNG");
    status_t const movie = inside.find<docs_collection_t>("movies")->at(avatar).assign(
        R"({"title":"Avatar","year":2009})");
    status_t const edge = inside.find<graph_collection_t>("similar")->write_edge(edge_t {avatar, titanic});
    expected<commit_stamp_t> const committed = inside.commit();
    failures += expect(transaction_status.succeeded() && poster.succeeded() && movie.succeeded() && edge.succeeded() &&
                       committed);

    // Reads at the head see the commit: the poster, the title and the one edge leaving Avatar
    auto const stored = (*posters)[avatar].value();
    auto const title = (*movies)[collection_key_field_t {avatar, "title"}].value(ustore_docs_field_type_string_k);
    auto const degree = similar->degree(avatar, ustore_vertex_role_source_k);
    failures += expect(stored && *stored == "\x89PNG");
    failures += expect(title && *title == "Avatar");
    failures += expect(degree && *degree == 1);
    return failures;
}

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

    report(totals, "wrappers.compile", wrappers_compile, config);
    report(totals, "wrappers.match", wrappers_match, config);
    report(totals, "wrappers.sample_measure", wrappers_sample_measure, config);
    report(totals, "wrappers.pages", wrappers_pages, config);
    report(totals, "wrappers.arrow", wrappers_arrow, config);
    report(totals, "wrappers.ingest", wrappers_ingest, config);
    report(totals, "wrappers.changes", wrappers_changes, config);
    report(totals, "wrappers.gpu_devices", wrappers_gpu_devices);
    report(totals, "quickstart", quickstart);
    return suite_exit_code(totals);
}
