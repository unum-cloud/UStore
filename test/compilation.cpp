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

#include "harness.hpp" // `run_test_case`, `expect_true`, `expect_near`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum;

/** The reason the stamp cases report on an engine that keeps no snapshots to stamp. */
constexpr char const* needs_stamps_k = "needs stamps";

/** An executor of the suite's engine, since an NVMe database refuses a RAM one with @c device_k. */
expected<executor_t> engine_executor() noexcept {
    engine_names_t names;
    if (unnamed_flag_t const unnamed = read_engine_names(test_run.settings.engine, names); !unnamed.flag.empty())
        return {{}, status_t {status_code_t::invalid_argument_k, "The suite's engine has no such name"}};
    if (names.engine != ustore_engine_nvme_k) return executor_t::for_ram({.threads = 1});
    return executor_t::for_nvme({.nvme = {.transport = names.transport}, .threads = 1});
}

/** The binary, document, graph and strings handles whose verbs answer deferred calls. */
using deferred_blobs_t = blobs_collection_on<ustore_completion_deferred_k, collection_access_t::read_write_k>;
using deferred_blobs_read_t = blobs_collection_on<ustore_completion_deferred_k, collection_access_t::read_only_k>;
using deferred_docs_t = docs_collection_on<ustore_completion_deferred_k, collection_access_t::read_write_k>;
using deferred_graph_t = graph_collection_on<ustore_completion_deferred_k, collection_access_t::read_write_k>;
using deferred_strings_t = strings_collection_on<ustore_completion_deferred_k, collection_access_t::read_write_k>;
using deferred_pages_t = pages_collection_on<ustore_completion_deferred_k, collection_access_t::read_write_k>;

/** Whether @p collection_type_ imports pages, which only a handle that writes does. */
template <typename collection_type_>
concept an_importing_collection = requires(collection_type_ const& collection,
                                           std::array<std::string_view, 1> const& markups) {
    collection.import(keys_view_t {}, markups);
};

/** Whether a value can be assigned through a binary collection that @p receiver_type_ finds. */
template <typename receiver_type_>
concept an_assignable_receiver =
    requires(answer_value_t<decltype(std::declval<receiver_type_&>().find(std::string_view {}))> collection) {
        collection[key_from_integer(1)].assign("x");
    };

/** Whether @p collection_type_ reads keys and an arena passed as @p keys_type_
 *  and @p arena_type_. */
template <typename collection_type_, typename keys_type_, typename arena_type_>
concept a_reading_collection = requires(collection_type_ const& collection, keys_type_&& keys, arena_type_&& arena) {
    collection.read(std::forward<keys_type_>(keys), std::forward<arena_type_>(arena));
};

/** Whether @p collection_type_ removes keys passed as @p keys_type_. */
template <typename collection_type_, typename keys_type_>
concept a_removing_collection = requires(collection_type_ const& collection, keys_type_&& keys) {
    collection.remove(std::forward<keys_type_>(keys));
};

/** Whether @p collection_type_ hands out a proxy, which only a blocking handle does. */
template <typename collection_type_>
concept an_addressable_collection = requires(collection_type_ const& collection) { collection[key_from_integer(1)]; };

/** Whether @p result_type_ owns the one-off arena its view reads from: it moves, never copies, and
 *  hands out a view of @p view_type_. */
template <typename result_type_, typename view_type_>
concept an_arena_owning_result = std::is_nothrow_move_constructible_v<result_type_> &&
                                 !std::is_copy_constructible_v<result_type_> && requires(result_type_ const& result) {
                                     { result.view() } -> std::same_as<view_type_ const&>;
                                 };

/* A snapshot hands out read-only collections, so nothing writes through it, while a transaction
 * does */
static_assert(!an_assignable_receiver<snapshot_ref_t>);
static_assert(an_assignable_receiver<transaction_ref_t>);

/* Every verb is written once: blocking, it answers an expected, and deferred, a call holding its

 * outputs */
static_assert(
    std::same_as<decltype(std::declval<blobs_collection_t const&>().read(keys_view_t {}, std::declval<arena_t&>())),
                 expected<values_view_t>>);
static_assert(
    std::same_as<decltype(std::declval<deferred_blobs_t const&>().read(keys_view_t {}, std::declval<arena_t&>())),
                 deferred<values_view_t>>);
static_assert(std::same_as<decltype(std::declval<blobs_collection_t const&>().remove(keys_view_t {})), status_t>);
static_assert(std::same_as<decltype(std::declval<deferred_blobs_t const&>().remove(keys_view_t {})), deferred<>>);

/* A deferred verb views its inputs as views of any contiguous batch, and an arena by reference,
 * so a temporary arena, which would die before the call is final, never reaches it */
static_assert(a_reading_collection<deferred_blobs_t, std::array<ustore_key_t, 2>&, arena_t&>);
static_assert(a_reading_collection<deferred_blobs_t, st::vector<ustore_key_t>&, arena_t&>);
static_assert(a_reading_collection<deferred_blobs_t, std::span<ustore_key_t const>, arena_t&>);
static_assert(a_reading_collection<deferred_blobs_t, keys_view_t, arena_t&>);
static_assert(!a_reading_collection<deferred_blobs_t, keys_view_t, arena_t>);
static_assert(a_reading_collection<deferred_docs_t, keys_view_t, arena_t&>);
static_assert(a_reading_collection<deferred_strings_t, strings_view_t&, arena_t&>);
static_assert(a_reading_collection<deferred_strings_t, std::span<value_view_t const>, arena_t&>);
static_assert(!a_reading_collection<deferred_strings_t, strings_view_t&, arena_t>);

/* A read-only handle never removes, and a deferred handle hands out no proxy, as a proxy blocks */
static_assert(a_removing_collection<deferred_blobs_t, keys_view_t>);
static_assert(!a_removing_collection<deferred_blobs_read_t, keys_view_t>);
static_assert(!a_removing_collection<blobs_read_collection_t, keys_view_t>);
static_assert(an_addressable_collection<blobs_collection_t>);
static_assert(!an_addressable_collection<deferred_blobs_t>);

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
static_assert(an_importing_collection<pages_collection_t> && an_importing_collection<deferred_pages_t>);
static_assert(!an_importing_collection<pages_read_collection_t>);

/* A feed, an ingest and an Arrow export own their C handles, so they move but never copy */
static_assert(std::is_nothrow_move_constructible_v<changes_cursor_t> &&
              !std::is_copy_constructible_v<changes_cursor_t>);
static_assert(std::is_nothrow_move_constructible_v<ingest_t> && !std::is_copy_constructible_v<ingest_t>);
static_assert(std::is_nothrow_move_constructible_v<arrow_export_t> && !std::is_copy_constructible_v<arrow_export_t>);

/* A shorthand result owns its one-off arena, while a view only views one */
static_assert(an_arena_owning_result<owned_value_t, value_view_t>);
static_assert(an_arena_owning_result<owned<values_view_t>, values_view_t>);
static_assert(!an_arena_owning_result<value_view_t, value_view_t>);

/* A progress callable never throws, as every wrapper is noexcept, and the call views it, so a
 * temporary never does */
using continuing_progress_t = decltype([](ustore_size_t, ustore_size_t) noexcept { return ustore_continue_k; });
using throwing_progress_t = decltype([](ustore_size_t, ustore_size_t) { return ustore_continue_k; });
static_assert(std::is_convertible_v<continuing_progress_t&, progress_t>);
static_assert(!std::is_convertible_v<continuing_progress_t, progress_t>);
static_assert(!std::is_convertible_v<throwing_progress_t&, progress_t>);

/* Nor does a predicate throw */
using admitting_predicate_t = decltype([](std::span<ustore_key_t const>, std::span<ustore_admission_t>) noexcept {});
using throwing_predicate_t = decltype([](std::span<ustore_key_t const>, std::span<ustore_admission_t>) {});
static_assert(a_keys_predicate<admitting_predicate_t>);
static_assert(!a_keys_predicate<throwing_predicate_t>);

/** Calls every C++ wrapper in each accepted form against one database. */
void test_wrappers_compile(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;

    database_t const& head = database;
    expect_true(!head.clear(ustore_drop_collection_k).failed());
    auto [main_collection, create_status] = head.create("main");
    expect_true(!create_status.failed());

    // Single-element access
    expect_true(!main_collection[key_from_integer(42)].assign("purpose of life").failed());
    expect_true(!main_collection.at(key_from_integer(42)).assign("purpose of life").failed());
    auto [single_value, single_status] = main_collection[key_from_integer(42)].value();
    expect_true(!single_status.failed() && single_value == "purpose of life");
    [[maybe_unused]] status_t const single_cleared = main_collection[key_from_integer(42)].clear_values();

    // Many keys to one value
    expect_true(!main_collection[{key_from_integer(43), key_from_integer(44)}].assign("same value").failed());

    // Smart references
    [[maybe_unused]] status_t const cleared =
        main_collection[{key_from_integer(43), key_from_integer(44)}].clear_values();
    [[maybe_unused]] status_t const removed = main_collection[{key_from_integer(43), key_from_integer(44)}].remove();
    auto [present, present_status] = main_collection[{key_from_integer(43), key_from_integer(44)}].present();
    auto [length, length_status] = main_collection[{key_from_integer(43), key_from_integer(44)}].length();
    auto [value, value_status] = main_collection[{key_from_integer(43), key_from_integer(44)}].value();
    expect_true(!present_status.failed() && !length_status.failed() && !value_status.failed());
    [[maybe_unused]] auto const by_array =
        main_collection[std::array<ustore_key_t, 3> {key_from_integer(65), key_from_integer(66), key_from_integer(67)}];
    std::array<ustore_key_t, 4> const four_keys {key_from_integer(65), key_from_integer(66), key_from_integer(67),
                                                 key_from_integer(68)};
    [[maybe_unused]] auto const by_range = main_collection[strided(four_keys)];
    // A range-for keeps a temporary alive only from C++23, so the answer is named before iterating
    auto [listed, listed_status] = main_collection[{key_from_integer(100), key_from_integer(101)}].value();
    expect_true(!listed_status.failed());
    for ([[maybe_unused]] value_view_t const element : listed) {}

    // Named collections
    auto [prefixes, prefixes_status] = head.find_or_create("prefixes");
    if (!expect_true(!prefixes_status.failed())) return;
    expect_true(!prefixes.at(key_from_integer(42)).assign("purpose").failed());
    auto [articles, articles_status] = head.find_or_create("articles");
    auto [suffixes, suffixes_status] = head.find_or_create("suffixes");
    expect_true(!articles_status.failed() && !articles.at(key_from_integer(42)).assign("of").failed());
    expect_true(!suffixes_status.failed() && !suffixes.at(key_from_integer(42)).assign("life").failed());

    // Reusable memory
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return;
    auto [arena_present,
          arena_present_status] = main_collection[{key_from_integer(43), key_from_integer(44)}].present(arena);
    auto [arena_length,
          arena_length_status] = main_collection[{key_from_integer(43), key_from_integer(44)}].length(arena);
    auto [arena_value, arena_value_status] = main_collection[{key_from_integer(43), key_from_integer(44)}].value(arena);
    expect_true(!arena_present_status.failed() && !arena_length_status.failed() && !arena_value_status.failed());
    [[maybe_unused]] expected<values_view_t> const verb_value = main_collection.read(four_keys, arena);

    // Counting and streaming keys
    auto [keys_count, keys_count_status] = main_collection.size();
    auto [pairs_count, pairs_count_status] = main_collection.pairs(key_from_integer(100), key_from_integer(200)).size();
    expect_true(!keys_count_status.failed() && !pairs_count_status.failed());
    keys_range_t keys = main_collection.keys(key_from_integer(100), key_from_integer(200));
    for ([[maybe_unused]] ustore_key_t const key : keys) {}
    expect_true(!keys.status().failed());
    auto [estimates,
          estimates_status] = main_collection.members(key_from_integer(100), key_from_integer(200)).size_estimates();
    expect_true(!estimates_status.failed());

    // Durability and watching
    [[maybe_unused]] status_t const flushed_clear =
        main_collection[{key_from_integer(43), key_from_integer(44)}].clear_values(ustore_durability_flushed_k);
    [[maybe_unused]] status_t const flushed_remove =
        main_collection[{key_from_integer(43), key_from_integer(44)}].remove(ustore_durability_flushed_k);
    auto [untracked_present, untracked_present_status] =
        main_collection[{key_from_integer(43), key_from_integer(44)}].present(arena, ustore_watch_none_k);
    auto [untracked_length, untracked_length_status] =
        main_collection[{key_from_integer(43), key_from_integer(44)}].length(arena, ustore_watch_none_k);
    auto [untracked_value, untracked_value_status] =
        main_collection[{key_from_integer(43), key_from_integer(44)}].value(ustore_watch_none_k);
    expect_true(!untracked_present_status.failed() && !untracked_length_status.failed() &&
                !untracked_value_status.failed());

    // Sub-documents
    auto [documents_collection, documents_collection_status] = head.find_or_create<docs_collection_t>("docs");
    if (!expect_true(!documents_collection_status.failed())) return;
    expect_true(!documents_collection[key_from_integer(56)].assign(R"({"hello":"world","answer":42})").failed());
    auto [field, field_status] = documents_collection[collection_key_field_t {key_from_integer(56), "hello"}].value();
    expect_true(!field_status.failed() && field == R"("world")");

    // The same verbs blocking on an executor, and answering deferred calls that view their inputs
    auto [executor, executor_status] = engine_executor();
    auto [ram_executor, ram_executor_status] = executor_t::for_ram({.threads = 1});
    if (!expect_true(!executor_status.failed() && !ram_executor_status.failed())) return;
    expect_true(!database_t::init_ram({}, ram_executor).status.failed());
    database_ref_t const head_on_executor = database.on(executor);
    auto [on_executor, on_executor_status] = head_on_executor.find("main");
    if (!expect_true(!on_executor_status.failed())) return;
    expect_true(!on_executor[key_from_integer(42)].assign("purpose of life").failed());
    auto [on_executor_value, on_executor_value_status] = on_executor[key_from_integer(42)].value();
    expect_true(!on_executor_value_status.failed());
    auto [articles_on_executor, articles_on_executor_status] = head_on_executor["articles"];
    expect_true(!articles_on_executor_status.failed() &&
                !articles_on_executor.at(key_from_integer(42)).assign("of").failed());

    database_on<ustore_completion_deferred_k> const deferred_head = database.deferred(executor);
    deferred_blobs_t const deferred_main = deferred_head[main_collection.handle()];
    std::array<ustore_key_t, 2> const viewed_keys {key_from_integer(43), key_from_integer(44)};
    std::array<std::string_view, 2> const blobs {"left", "right"};
    expect_true(!deferred_main.write(viewed_keys, blobs).wait().failed());
    deferred<values_view_t> reading = deferred_main.read(viewed_keys, arena);
    [[maybe_unused]] status_code_t const polled = reading.status();
    auto const [read, read_status] = reading.wait();
    expect_true(!read_status.failed());
    if (!read_status.failed())
        for ([[maybe_unused]] value_view_t const element : read) {}
    deferred<> removing = deferred_main.remove(viewed_keys);
    expect_true(!removing.wait().failed());
    expect_true(!main_collection.deferred(executor).clear_values().wait().failed());
    auto const [created, deferred_create_status] = deferred_head.create("created").wait();
    expect_true(!deferred_create_status.failed());
    if (!deferred_create_status.failed()) {
        expect_true(!deferred_head[created.handle()].remove(viewed_keys).wait().failed());
        expect_true(!deferred_head.drop(created.handle()).wait().failed());
    }
    expect_true(!deferred_head.collections(arena).wait().status.failed());
    expect_true(!deferred_head.describe().wait().status.failed());
    expect_true(!deferred_head.flush().wait().failed());

    // Deferred documents, graphs and strings, each verb viewing its inputs
    expect_true(!head.find_or_create<docs_collection_t>("docs").status.failed());
    auto [deferred_documents, deferred_documents_status] = deferred_head.find<docs_collection_t>("docs", arena).wait();
    if (!expect_true(!deferred_documents_status.failed())) return;
    std::array<std::string_view, 2> const documents {R"({"hello":"world"})", R"({"hello":"again"})"};
    expect_true(!deferred_documents.write(viewed_keys, documents).wait().failed());
    [[maybe_unused]] expected<values_view_t> const documents_read = deferred_documents.read(viewed_keys, arena).wait();
    std::array<std::string_view, 1> const hello {"hello"};
    [[maybe_unused]] expected<values_view_t> const fields_read =
        deferred_documents.read(viewed_keys, arena, hello, ustore_docs_field_type_string_k).wait();
    [[maybe_unused]] expected<strings_view_t> const gist = deferred_documents.gist(viewed_keys, arena).wait();
    auto const header = make_table_header().with<std::string_view>("hello");
    [[maybe_unused]] expected<docs_table<std::string_view>> const gathered =
        deferred_documents.gather(viewed_keys, header, arena).wait();
    expect_true(!deferred_documents.remove(viewed_keys).wait().failed());
    expect_true(!head.create<graph_collection_t>("graph").status.failed());
    auto [deferred_graph, deferred_graph_status] = deferred_head.find<graph_collection_t>("graph", arena).wait();
    if (!expect_true(!deferred_graph_status.failed())) return;
    std::array<edge_t, 1> const viewed_edges {edge_t {key_from_integer(43), key_from_integer(44)}};
    ustore_vertex_role_t const leaving = ustore_vertex_role_source_k;
    strided_span<ustore_vertex_role_t const> const roles {{&leaving, 0}, viewed_keys.size()};
    expect_true(!deferred_graph.write_edges(viewed_edges).wait().failed());
    expect_true(!deferred_graph.write_vertices(viewed_keys).wait().failed());
    [[maybe_unused]] expected<edges_view_t> const edges_read = deferred_graph.read_edges(viewed_keys, arena).wait();
    [[maybe_unused]] expected<degrees_view_t> const degrees = deferred_graph.degrees(viewed_keys, arena, roles).wait();
    expect_true(!deferred_graph.remove_edges(viewed_edges).wait().failed());
    expect_true(!deferred_graph.remove_vertices(viewed_keys).wait().failed());

    expect_true(!head.create<strings_collection_t>("strings").status.failed());
    auto [deferred_strings,
          deferred_strings_status] = deferred_head.find<strings_collection_t>("strings", arena).wait();
    if (!expect_true(!deferred_strings_status.failed())) return;
    std::array<std::string_view, 2> const texts {"hello", "help"};
    std::array<std::string_view, 1> const asked {"hello"};
    std::array<std::string_view, 1> const string_prefixes {"hel"};
    std::array<std::string_view, 1> const removed_strings {"help"};
    std::array<ustore_key_t, 2> const string_keys {key_from_integer(1), key_from_integer(2)};
    expect_true(!deferred_strings.write(texts, string_keys).wait().failed());
    [[maybe_unused]] expected<keys_view_t> const strings_read = deferred_strings.read(asked, arena).wait();
    [[maybe_unused]] expected<strings_pages_t> const scanned = deferred_strings.scan(string_prefixes, 8, arena).wait();
    expect_true(!deferred_strings.remove(removed_strings).wait().failed());

    // A plan a cancel leaves live, continued and finished blocking, then deferred
    auto cancelling = [](ustore_size_t, ustore_size_t) noexcept { return ustore_cancel_k; };
    database_compact_plan_t plan;
    [[maybe_unused]] status_t const stopped = head.compact(cancelling, &plan);
    [[maybe_unused]] char const* const phase = ustore_phase_name(plan.phase().phase);
    [[maybe_unused]] status_t const continued = head.compact({}, &plan);
    if (plan.handle()) expect_true(!head.compact_plan_finish(std::move(plan)).failed());
    database_compact_plan_t deferred_plan;
    [[maybe_unused]] status_t const deferred_stopped = deferred_head.compact(cancelling, &deferred_plan).wait();
    if (deferred_plan.handle())
        expect_true(!deferred_head.compact_plan_finish(std::move(deferred_plan)).wait().failed());
    [[maybe_unused]] status_t const whole = head.compact();

    // Transactions on an executor, and their deferred verbs
    if (probed(head.supports_transactions())) {
        auto [transaction, transaction_status] = head_on_executor.transaction();
        if (!expect_true(!transaction_status.failed())) return;
        [[maybe_unused]] expected<docs_collection_t> const transaction_documents = transaction.find<docs_collection_t>(
            "docs");
        transaction_on<ustore_completion_deferred_k> const deferred_transaction = transaction.deferred(executor);
        [[maybe_unused]] expected<deferred_blobs_t> const deferred_named =
            deferred_transaction.find("docs", arena).wait();
        auto [transaction_main, transaction_main_status] = deferred_transaction.find("main", arena).wait();
        expect_true(!transaction_main_status.failed() && !transaction_main.remove(viewed_keys).wait().failed());
        [[maybe_unused]] expected<read_stamp_t> const staged = deferred_transaction.stage().wait();
        [[maybe_unused]] status_t const rolled_back = deferred_transaction.rollback().wait();
        [[maybe_unused]] expected<commit_stamp_t> const committed = deferred_transaction.commit().wait();
        expect_true(!deferred_transaction.reset().wait().status.failed());
    }

    // Snapshots hand out the read-only twin of every collection
    if (probed(head.supports_snapshots())) {
        auto [snapshot, snapshot_status] = head_on_executor.snapshot();
        if (!expect_true(!snapshot_status.failed())) return;
        [[maybe_unused]] read_stamp_t const snapshot_stamp = snapshot.read_stamp();
        auto const [snapshot_main, find_status] = snapshot.find("main");
        [[maybe_unused]] auto [snapshot_value, read_status] = snapshot_main.at(key_from_integer(42)).value();
        expect_true(!find_status.failed() && !read_status.failed());
        [[maybe_unused]] expected<docs_read_collection_t> const snapshot_documents = snapshot.find<docs_collection_t>(
            "docs");
        [[maybe_unused]] expected<graph_read_collection_t> const snapshot_graph = snapshot.find<graph_collection_t>(
            "graph");
        [[maybe_unused]] expected<strings_read_collection_t> const snapshot_strings =
            snapshot.find<strings_collection_t>("strings");
        auto [deferred_snapshot, deferred_snapshot_status] = snapshot.deferred(executor).find("main", arena).wait();
        if (!expect_true(!deferred_snapshot_status.failed())) return;
        expect_true(!deferred_snapshot.read(viewed_keys, arena).wait().status.failed());
        auto [deferred_snapshot_documents, deferred_snapshot_documents_status] =
            snapshot.deferred(executor).find<docs_collection_t>("docs", arena).wait();
        if (!expect_true(!deferred_snapshot_documents_status.failed())) return;
        expect_true(!deferred_snapshot_documents.read(viewed_keys, arena).wait().status.failed());
        if (config.directory()) {
            std::string const saved = fmt::format("{}/{}", config.directory(), "saved");
            expect_true(create_directories(saved.c_str()));
            std::string const target = save_target(config, saved);
            expect_true(!snapshot.deferred(executor).save(target.c_str()).wait().failed());
        }
    }

    expect_true(!head.clear(ustore_drop_collection_k).failed());
}

/** Patterns, weighted scores, a documents filter and a predicate reach the scans and cursors of the
 *  C++ wrappers. */
void test_wrappers_match(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return;
    auto [fruits, fruits_status] = database.find_or_create("fruits");
    if (!expect_true(!fruits_status.failed())) return;
    auto [people, people_status] = database.find_or_create<docs_collection_t>("people");
    if (!expect_true(!people_status.failed())) return;
    std::array<char const*, 4> const names {"Apple", "banana", "cherry", "avocado"};
    std::array<char const*, 4> const documents {R"({"age":25})", R"({"age":40})", R"({"name":"x"})", R"({"age":31})"};
    for (std::size_t index = 0; index != names.size(); ++index) {
        expect_true(!fruits[key_from_integer(index + 1)].assign(names[index]).failed());
        expect_true(!people[key_from_integer(index + 1)].assign(documents[index]).failed());
    }
    receiver_t const receiver = database.receiver();

    std::string_view const substring = "an";
    std::array<std::uint32_t, 2> const offsets {0, static_cast<std::uint32_t>(substring.size())};
    float const weight = 2.0f;
    match_t const weighted {.patterns = {.patterns = util::u32_tape(substring.data(), offsets.data()),
                                         .count = 1,
                                         .kind = ustore_pattern_substrings_k,
                                         .weights = &weight}};
    float const* scores = nullptr;
    auto [scored, scored_status] = scan_keys_through<ustore_blobs_scan_t, &ustore_blobs_scan>(
        receiver, fruits.handle(), smallest_key_k, 8, arena, weighted, &scores);
    expect_true(!scored_status.failed() && scored.size() == 1 && scored[0] == key_from_integer(2));
    expect_true(scores != nullptr);
    if (scores) expect_near(scores[0], 2.0 * (2.0 * 2.2) / (2.0 + 1.2), 1e-3);

    std::string_view const older = "?@.age > 30";
    std::size_t asked = 0;
    auto even_keys = [&](std::span<ustore_key_t const> keys, std::span<ustore_admission_t> admissions) noexcept {
        ++asked;
        for (std::size_t index = 0; index != keys.size(); ++index)
            admissions[index] = keys[index].uuid.bytes[15] % 2 == 0 ? ustore_admit_k : ustore_reject_k;
    };
    match_t const filtered {.filter = bytes_of(older), .predicate = predicate_of(even_keys)};
    auto [cursor, cursor_status] = docs_cursor_t::make(receiver, people.handle(), smallest_key_k, filtered);
    if (!expect_true(!cursor_status.failed())) return;
    auto [batches, batches_status] = cursor.next(8, arena);
    std::size_t rows = 0;
    for (ustore_docs_batch_view_t const& batch : batches) rows += batch.count;
    expect_true(!batches_status.failed() && rows == 2 &&
                batches.back().keys[batches.back().count - 1] == key_from_integer(4));
    expect_true(asked != 0);
    expect_true(!database.clear(ustore_drop_collection_k).failed());
}

/** The samples and measures of strings, documents, graph vertices and pages, blocking and deferred,
 *  and the refusals of a collection holding another modality and of a dropped one. */
void test_wrappers_sample_measure(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    database_t const& head = database;
    expect_true(!head.clear(ustore_drop_collection_k).failed());
    auto [arena, arena_status] = arena_t::make();
    auto [executor, executor_status] = engine_executor();
    auto [words, words_status] = head.create<strings_collection_t>("words");
    auto [people, people_status] = head.create<docs_collection_t>("people");
    auto [graph, graph_status] = head.create<graph_collection_t>("graph");
    if (!expect_true(!arena_status.failed() && !executor_status.failed() && !words_status.failed() &&
                     !people_status.failed() && !graph_status.failed()))
        return;

    std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
    std::array<std::string_view, 2> const texts {"apple", "apricot"};
    std::array<std::string_view, 2> const documents {R"({"a":1})", R"({"a":2})"};
    expect_true(!words.write(texts, keys).failed());
    expect_true(!people.write(keys, documents).failed());
    expect_true(!graph.write_edge(edge_t {keys[0], keys[1]}).failed());

    auto const holds = [](expected<size_estimates_t> const& bounds, std::size_t count) {
        return !bounds.status.failed() && bounds.value.cardinality.min <= count &&
               count <= bounds.value.cardinality.max;
    };
    value_view_t const unbounded = value_view_t::make_empty();
    auto [drawn_words, drawn_words_status] = words.sample(8, arena);
    expect_true(!drawn_words_status.failed() && drawn_words.strings.size() == 2);
    expect_true(holds(words.measure(unbounded, unbounded, arena), 2));
    value_view_t const from = "apricot";
    expect_true(holds(words.deferred(executor).measure(from, unbounded, arena).wait(), 1));

    auto [drawn_people, drawn_people_status] = people.sample(8, arena);
    expect_true(!drawn_people_status.failed() && drawn_people.size() == 2);
    expect_true(holds(people.measure(smallest_key_k, largest_key_k, arena), 2));
    auto [deferred_drawn, deferred_drawn_status] = people.deferred(executor).sample(1, arena).wait();
    expect_true(!deferred_drawn_status.failed() && deferred_drawn.size() == 1);

    auto [drawn_vertices, drawn_vertices_status] = graph.sample_vertices(8, arena);
    expect_true(!drawn_vertices_status.failed() && drawn_vertices.size() == 2);
    expect_true(holds(graph.measure_vertices(smallest_key_k, largest_key_k, arena), 2));
    expect_true(holds(graph.deferred(executor).measure_vertices(keys[1], largest_key_k, arena).wait(), 1));

    // The library refuses a collection of another modality, and one dropped meanwhile
    docs_collection_t const mistyped {head.receiver(), words.handle()};
    expect_status(mistyped.sample(8, arena), status_code_t::invalid_argument_k);
    expect_true(!graph.drop().failed());
    expect_status(graph.sample_vertices(8, arena), status_code_t::not_found_k);
    expect_true(!head.clear(ustore_drop_collection_k).failed());
}

/** Every pages verb, blocking and deferred, with the refusals of the binding and the library. */
case_result_t test_wrappers_pages(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    database_t const& head = database;
    expect_true(!head.clear(ustore_drop_collection_k).failed());
    auto [arena, arena_status] = arena_t::make();
    auto [executor, executor_status] = engine_executor();
    auto [pages, pages_status] = head.create<pages_collection_t>("pages");
    if (!expect_true(!arena_status.failed() && !executor_status.failed() && !pages_status.failed())) return {};

    std::array<ustore_key_t, 1> const page {key_from_integer(7)};
    std::array<std::string_view, 1> const markup {"<doc><p>one</p></doc>"};
    status_t const imported = pages.import(page, markup);
    if (imported == status_code_t::unsupported_k) return case_result_t::skipped(needs_pages_k);
    expect_true(!imported.failed());
    auto const rendered = [&](std::string_view wanted) {
        auto [rendering, read_status] = pages.read(page, arena);
        return expect_true(!read_status.failed() && rendering.size() == 1 && std::string_view {rendering[0]} == wanted);
    };
    rendered("<doc><p>one</p></doc>");

    // An inserted element takes a minted key, which every later edit names
    std::array<std::string_view, 1> const fragment {"<p>two</p>"};
    auto [landed, insert_status] = pages.insert(page, fragment, arena);
    if (!expect_true(!insert_status.failed() && landed.size() == 1)) return {};
    std::array<ustore_key_t, 1> const element {landed[0]};
    rendered("<p>two</p><doc><p>one</p></doc>");
    std::array<std::string_view, 1> const field {"id"}, attribute {"x"};
    expect_true(!pages.set_attributes(page, element, field, attribute).failed());
    rendered(R"(<p id="x">two</p><doc><p>one</p></doc>)");
    expect_true(!pages.remove_attributes(page, element, field).failed());
    rendered("<p>two</p><doc><p>one</p></doc>");
    std::array<ustore_length_t, 1> const at_front {0};
    std::array<std::string_view, 1> const lead {"lead"};
    expect_true(!pages.splice(page, page, at_front, at_front, lead).failed());
    rendered("lead<p>two</p><doc><p>one</p></doc>");
    expect_true(!pages.move(page, element).failed());
    rendered("lead<p>two</p><doc><p>one</p></doc>");
    auto [subtree, subtree_status] = pages.read(page, arena, element, ustore_page_encoding_text_k);
    expect_true(!subtree_status.failed() && std::string_view {subtree[0]} == "two");
    expect_true(!pages.remove(page, element).failed());
    rendered("lead<doc><p>one</p></doc>");

    auto [drawn, drawn_status] = pages.sample(8, arena);
    expect_true(!drawn_status.failed() && drawn.size() == 1 && drawn[0] == page[0]);
    auto [bounds, bounds_status] = pages.measure(smallest_key_k, largest_key_k, arena);
    expect_true(!bounds_status.failed() && bounds.cardinality.min <= 1 && 1 <= bounds.cardinality.max);
    deferred_pages_t const later = pages.deferred(executor);
    expect_true(!later.read(page, arena).wait().status.failed());
    auto [later_drawn, later_drawn_status] = later.sample(8, arena).wait();
    expect_true(!later_drawn_status.failed() && later_drawn.size() == 1);

    // The binding alone sees two columns disagree, and the library refuses the rest
    std::array<std::string_view, 2> const two_markups {"<a/>", "<b/>"};
    std::array<ustore_key_t, 2> const two_parents {page[0], page[0]};
    expect_status(pages.import(page, two_markups), status_code_t::invalid_argument_k);
    expect_status(later.import(page, two_markups).wait(), status_code_t::invalid_argument_k);
    expect_status(pages.insert(page, fragment, arena, two_parents), status_code_t::invalid_argument_k);
    expect_status(pages.remove(page, {}), status_code_t::invalid_argument_k);
    expect_status(pages.read(page, arena, {}, ustore_page_encoding_html_k), status_code_t::unsupported_k);

    // A range of whole pages goes at once
    std::array<ustore_key_t, 1> const past {key_from_integer(8)};
    expect_true(!pages.remove(page, {}, past).failed());
    auto [removed, removed_status] = pages.read(page, arena);
    bits_view_t const found = removed.presences();
    expect_true(!removed_status.failed() && !found[0]);
    expect_true(!head.clear(ustore_drop_collection_k).failed());
    return {};
}

/** Keys, strings and a gathered column cross to Arrow and back in place, and the converters and
 *  exporters refuse what C documents. */
void test_wrappers_arrow(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    database_t const& head = database;
    expect_true(!head.clear(ustore_drop_collection_k).failed());
    auto [arena, arena_status] = arena_t::make();
    auto [blobs, blobs_status] = head.create<blobs_collection_t>("blobs");
    auto [people, people_status] = head.create<docs_collection_t>("people");
    if (!expect_true(!arena_status.failed() && !blobs_status.failed() && !people_status.failed())) return;

    std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
    std::array<std::string_view, 2> const values {"left", "right"};
    std::array<std::string_view, 2> const documents {R"({"age":25})", R"({"age":40})"};
    expect_true(!blobs.write(keys, values).failed());
    expect_true(!people.write(keys, documents).failed());

    // Values a read exported cross out and back without a copy
    auto [read, read_status] = blobs.read(keys, arena);
    if (!expect_true(!read_status.failed())) return;
    auto [strings_column, strings_export_status] = strings_view_to_arrow(read, arena);
    if (!expect_true(!strings_export_status.failed() && strings_column.array().array.length == 2)) return;
    auto [strings_back, strings_back_status] = strings_view_from_arrow(strings_column.schema(),
                                                                       strings_column.array().array);
    if (!expect_true(!strings_back_status.failed() && strings_back.size() == 2)) return;
    expect_true(std::string_view {strings_back[0]} == "left" && std::string_view {strings_back[1]} == "right");

    // Sampled keys sit in the arena, so they export in place
    auto [drawn, drawn_status] = people.sample(8, arena);
    expect_true(!drawn_status.failed() && drawn.size() == 2);
    auto [keys_column, keys_export_status] = keys_view_to_arrow(drawn, arena);
    if (!expect_true(!keys_export_status.failed() && std::string_view {keys_column.schema().format} == "w:16")) return;
    auto [keys_back, keys_back_status] = keys_view_from_arrow(keys_column.schema(), keys_column.array().array);
    expect_true(!keys_back_status.failed() && keys_back.size() == 2 && keys_back[0] == drawn[0]);

    // A gathered column exports with its validities
    auto header = make_table_header().with<std::int32_t>("age");
    auto [table, gather_status] = people.gather(keys, header, arena);
    if (!expect_true(!gather_status.failed())) return;
    auto [ages, ages_status] = docs_column_view_to_arrow(table.column(0), arena);
    expect_true(!ages_status.failed() && std::string_view {ages.schema().format} == "i");
    expect_true(!ages_status.failed() && ages.array().array.length == 2);

    // Released inputs, keys outside the arena and an accessor are refused
    ArrowSchema const released_schema {};
    ArrowArray const released_array {};
    expect_status(keys_view_from_arrow(released_schema, released_array), status_code_t::invalid_argument_k);
    expect_status(strings_view_from_arrow(released_schema, released_array), status_code_t::invalid_argument_k);
    expect_status(keys_view_to_arrow(keys, arena), status_code_t::invalid_argument_k);
    std::array<std::string_view, 1> const accessed {"x"};
    expect_status(strings_view_to_arrow(strings_view_of(accessed), 1, ustore_strings_accessor_k, arena),
                  status_code_t::invalid_argument_k);
    expect_true(!head.clear(ustore_drop_collection_k).failed());
}

/** Ingests land blobs and edges as one commit each, and refuse what C documents. */
case_result_t test_wrappers_ingest(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    database_t const& head = database;
    expect_true(!head.clear(ustore_drop_collection_k).failed());
    auto [blobs, blobs_status] = head.create<blobs_collection_t>("blobs");
    auto [graph, graph_status] = head.create<graph_collection_t>("graph");
    if (!expect_true(!blobs_status.failed() && !graph_status.failed())) return {};

    auto [ingest, ingest_status] = head.ingest(blobs.handle(), ustore_ingest_unsorted_k);
    if (ingest_status == status_code_t::unsupported_k) return case_result_t::skipped("needs ingestion");
    if (!expect_true(!ingest_status.failed())) return {};
    std::array<ustore_key_t, 2> const keys {key_from_integer(2), key_from_integer(1)};
    std::array<std::string_view, 2> const values {"two", "one"};
    std::array<std::string_view, 1> const one_value {"lone"};
    expect_true(!ingest.append(keys, values).failed());
    expect_status(ingest.append(keys, one_value), status_code_t::invalid_argument_k);
    auto [early, early_status] = blobs[keys[1]].present();
    expect_true(!early_status.failed() && !early); // nothing lands before the finish
    expect_true(!ingest.finish().status.failed());
    expect_status(ingest.finish(), status_code_t::invalid_argument_k);
    auto [landed, landed_status] = blobs[keys[1]].value();
    expect_true(!landed_status.failed() && landed == "one");

    // Edges ingest as edges, and a sorted ingest refuses a key that does not ascend
    auto [edges_ingest, edges_ingest_status] = head.ingest(graph.handle());
    if (!expect_true(!edges_ingest_status.failed())) return {};
    std::array<edge_t, 1> const edges {edge_t {keys[1], keys[0]}};
    expect_true(!edges_ingest.append(edges_view_t {edges}).failed());
    expect_true(!edges_ingest.finish().status.failed());
    auto [ingested_degree, ingested_degree_status] = graph.degree(keys[1], ustore_vertex_role_source_k);
    expect_true(!ingested_degree_status.failed() && ingested_degree == 1u);
    {
        // An unfinished ingest holds NVMe's maintenance claim, so it goes before the drop below
        auto [sorted_ingest, sorted_ingest_status] = head.ingest(blobs.handle(), ustore_ingest_sorted_k);
        if (!expect_true(!sorted_ingest_status.failed())) return {};
        expect_status(sorted_ingest.append(keys, values), status_code_t::invalid_argument_k);
    }
    expect_true(!head.clear(ustore_drop_collection_k).failed());
    return {};
}

/** A feed hands out the writes and removals committed after a snapshot's stamp, oldest first, and
 *  refuses a dropped collection. */
case_result_t test_wrappers_changes(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    database_t const& head = database;
    expect_true(!head.clear(ustore_drop_collection_k).failed());
    if (!probed(head.supports_snapshots())) return case_result_t::skipped(needs_stamps_k);
    auto [arena, arena_status] = arena_t::make();
    auto [blobs, blobs_status] = head.create<blobs_collection_t>("blobs");
    auto [dropped, dropped_status] = head.create<blobs_collection_t>("dropped");
    if (!expect_true(!arena_status.failed() && !blobs_status.failed() && !dropped_status.failed())) return {};

    // The snapshot only reads the head's stamp, and goes before the drop it would block
    auto const [since, stamp_status] = [&]() noexcept -> expected<commit_stamp_t> {
        auto [snapshot, snapshot_status] = head.snapshot();
        if (snapshot_status.failed()) return {{}, snapshot_status};
        return {commit_stamp_t {static_cast<ustore_stamp_t>(snapshot.read_stamp())}};
    }();
    if (!expect_true(!stamp_status.failed())) return {};
    auto [feed, feed_status] = head.changes_cursor(blobs.handle(), since);
    if (feed_status == status_code_t::unsupported_k) return case_result_t::skipped(needs_stamps_k);
    if (!expect_true(!feed_status.failed())) return {};
    ustore_key_t const key = key_from_integer(1);
    expect_true(!blobs[key].assign("a").failed());
    expect_true(!blobs[key].remove().failed());
    auto [changes, changes_status] = feed.next_pairs(8, arena);
    if (!expect_true(!changes_status.failed() && changes.keys.size() == 2)) return {};
    expect_true(changes.keys[0] == key && changes.keys[1] == key && changes.stamps[0] < changes.stamps[1]);
    expect_true(changes.kinds[0] == ustore_change_write_k && changes.kinds[1] == ustore_change_removal_k);
    expect_true(std::string_view {ustore_change_kind_name(changes.kinds[1])} == "removal");
    expect_true(std::string_view {changes.values[0]} == "a");
    auto [rest, rest_status] = feed.next(8, arena);
    expect_true(!rest_status.failed() && rest.keys.empty());

    ustore_collection_id_t const gone = dropped.handle();
    expect_true(!dropped.drop().failed());
    expect_status(head.changes_cursor(gone, since), status_code_t::not_found_k);
    expect_true(!head.clear(ustore_drop_collection_k).failed());
    return {};
}

/** The GPU listing answers how many devices there are, filling no more than it is given. */
void test_wrappers_gpu_devices() {
    auto [count, count_status] = gpu_devices({});
    expect_true(!count_status.failed());
    std::array<ustore_gpu_device_t, 4> devices {};
    auto [listed, listed_status] = gpu_devices(devices);
    expect_true(!listed_status.failed() && listed == count);
    for (std::size_t index = 0; index != fu::min_of(listed, devices.size()); ++index)
        expect_true(std::string_view {ustore_gpu_vendor_name(devices[index].vendor)} != "unrecognized");
}

/** The Quickstart of cpp/README.md, line by line. The README's @c main is this function; its @c
 *  assert calls are checks, the first returning early since the rest would dereference a failed
 *  open, and the last split per condition so a failure names the one it broke. */
void test_readme_quickstart() {
    auto [db, open_status] = database_t::init_ram();
    auto [posters, posters_status] = db.create<blobs_collection_t>("posters");
    auto [movies, movies_status] = db.create<docs_collection_t>("movies");
    auto [similar, similar_status] = db.create<graph_collection_t>("similar");
    if (!expect_true(!open_status.failed() && !posters_status.failed() && !movies_status.failed() &&
                     !similar_status.failed()))
        return;

    // One transaction writes a blob, a document and an edge, and commits all three or none
    ustore_key_t const avatar = key_from_integer(1), titanic = key_from_integer(2);
    auto [transaction, transaction_status] = db.transaction();
    transaction_t const& inside = transaction;
    auto [poster_changes, poster_changes_status] = inside["posters"];
    auto [movie_changes, movie_changes_status] = inside.find<docs_collection_t>("movies");
    auto [similar_changes, similar_changes_status] = inside.find<graph_collection_t>("similar");
    status_t const poster = poster_changes.at(avatar).assign("\x89PNG");
    status_t const movie = movie_changes.at(avatar).assign(R"({"title":"Avatar","year":2009})");
    status_t const edge = similar_changes.write_edge(edge_t {avatar, titanic});
    auto const [committed, commit_status] = inside.commit();
    expect_true(!transaction_status.failed() && !poster_changes_status.failed() && !movie_changes_status.failed() &&
                !similar_changes_status.failed() && !poster.failed() && !movie.failed() && !edge.failed() &&
                !commit_status.failed());

    // Reads at the head see the commit: the poster, the title and the one edge leaving Avatar
    auto [stored, stored_status] = posters[avatar].value();
    auto [title,
          title_status] = movies[collection_key_field_t {avatar, "title"}].value(ustore_docs_field_type_string_k);
    auto [degree, degree_status] = similar.degree(avatar, ustore_vertex_role_source_k);
    expect_true(!stored_status.failed() && stored == "\x89PNG");
    expect_true(!title_status.failed() && title == "Avatar");
    expect_true(!degree_status.failed() && degree == 1);
}

int main(int argc, char** argv) {
    start_test_run(argc, argv);

    auto [directory, directory_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(directory_status)) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    config_t const config {test_run.settings.engine, directory.path()};

    run_test_case(engine_label_t {"test_wrappers_compile"}.c_str(), test_wrappers_compile, config);
    run_test_case(engine_label_t {"test_wrappers_match"}.c_str(), test_wrappers_match, config);
    run_test_case(engine_label_t {"test_wrappers_sample_measure"}.c_str(), test_wrappers_sample_measure, config);
    run_test_case(engine_label_t {"test_wrappers_pages"}.c_str(), test_wrappers_pages, config);
    run_test_case(engine_label_t {"test_wrappers_arrow"}.c_str(), test_wrappers_arrow, config);
    run_test_case(engine_label_t {"test_wrappers_ingest"}.c_str(), test_wrappers_ingest, config);
    run_test_case(engine_label_t {"test_wrappers_changes"}.c_str(), test_wrappers_changes, config);
    run_test_case("test_wrappers_gpu_devices", test_wrappers_gpu_devices);
    run_test_case("test_readme_quickstart", test_readme_quickstart);
    return finish_test_run();
}
