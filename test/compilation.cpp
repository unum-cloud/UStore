/**
 *  @file test/compilation.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Calls the C++ wrappers in each of their accepted forms, so a change that breaks a call site fails to build.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdio>  // `stderr`

#include <array>       // `std::array`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`, `std::is_convertible_v`
#include <utility>     // `std::declval`, `std::forward`, `std::move`

#include <fmt/format.h>

#include <smashtable/basic_vector.hpp>

#include "ustore/ustore.hpp"

#include "harness.hpp" // `suite_exit_code`, `expect`, `suite_totals_t`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum;

/** Whether a value can be assigned through a binary collection that @p receiver_type_ finds. */
template <typename receiver_type_>
concept assigns_through_concept = requires(typename decltype(std::declval<receiver_type_&>().find(
    std::string_view {}))::value_type collection) { collection[key_from_integer(1)].assign("x"); };

/** Whether a deferred read accepts keys and an arena passed as @p keys_type_ and @p arena_type_. */
template <typename keys_type_, typename arena_type_>
concept reads_deferred_concept = requires(deferred_blobs_collection_t const& collection, keys_type_&& keys,
                                          arena_type_&& arena) {
    collection.read(std::forward<keys_type_>(keys), std::forward<arena_type_>(arena));
};

/** Whether a deferred document read accepts keys and an arena passed as @p keys_type_ and @p arena_type_. */
template <typename keys_type_, typename arena_type_>
concept reads_documents_deferred_concept = requires(deferred_docs_collection_t const& collection, keys_type_&& keys,
                                                    arena_type_&& arena) {
    collection.read(std::forward<keys_type_>(keys), std::forward<arena_type_>(arena));
};

/** Whether a deferred document write accepts values and fields passed as @p values_type_ and @p fields_type_. */
template <typename values_type_, typename fields_type_>
concept writes_documents_deferred_concept = requires(deferred_docs_collection_t const& collection,
                                                     values_type_&& values, fields_type_&& fields) {
    collection.write(std::array<ustore_key_t, 1> {}, std::forward<values_type_>(values),
                     std::forward<fields_type_>(fields));
};

/** Whether a deferred gather accepts the types of its columns passed as @p types_type_. */
template <typename types_type_>
concept gathers_deferred_concept = requires(deferred_docs_collection_t const& collection, types_type_&& types,
                                            arena_t& arena) {
    collection.gather(std::array<ustore_key_t, 1> {}, values_tape_t {}, std::forward<types_type_>(types), arena);
};

/** Whether a deferred read of edges accepts vertices and an arena passed as @p vertices_type_ and @p arena_type_. */
template <typename vertices_type_, typename arena_type_>
concept reads_edges_deferred_concept = requires(deferred_graph_collection_t const& collection,
                                                vertices_type_&& vertices, arena_type_&& arena) {
    collection.read_edges(std::forward<vertices_type_>(vertices), std::forward<arena_type_>(arena));
};

/** Whether a deferred write of edges accepts edges passed as @p edges_type_. */
template <typename edges_type_>
concept writes_edges_deferred_concept = requires(deferred_graph_collection_t const& collection, edges_type_&& edges) {
    collection.write_edges(std::forward<edges_type_>(edges));
};

/** Whether a deferred read of strings accepts strings and an arena passed as @p texts_type_ and @p arena_type_. */
template <typename texts_type_, typename arena_type_>
concept reads_strings_deferred_concept = requires(deferred_strings_collection_t const& collection, texts_type_&& texts,
                                                  arena_type_&& arena) {
    collection.read(std::forward<texts_type_>(texts), std::forward<arena_type_>(arena));
};

/** Whether @p result_type_ owns the one-off arena its view reads from: it moves, never copies, and slices off a view
 *  of @p view_type_. */
template <typename result_type_, typename view_type_>
concept owns_arena_concept = std::is_nothrow_move_constructible_v<result_type_> &&
                             !std::is_copy_constructible_v<result_type_> && requires(result_type_ const& result) {
                                 { result.view() } -> std::same_as<view_type_ const&>;
                             };

/** Whether a deferred write of strings accepts strings and keys passed as @p texts_type_ and @p keys_type_. */
template <typename texts_type_, typename keys_type_>
concept writes_strings_deferred_concept = requires(deferred_strings_collection_t const& collection, texts_type_&& texts,
                                                   keys_type_&& keys) {
    collection.write(std::forward<texts_type_>(texts), std::forward<keys_type_>(keys));
};

// A snapshot hands out read-only collections, so nothing writes through it, while a transaction does
static_assert(!assigns_through_concept<snapshot_t>);
static_assert(!assigns_through_concept<snapshot_ref_t>);
static_assert(assigns_through_concept<transaction_t>);

// A deferred verb owns its plain inputs, so borrowed keys never reach it, and borrows the arena handle, so a temporary
// arena, which would die before the call is final, never does either
static_assert(reads_deferred_concept<std::array<ustore_key_t, 2>, arena_t&>);
static_assert(reads_deferred_concept<st::basic_vector<ustore_key_t>, arena_t&>);
static_assert(!reads_deferred_concept<std::array<ustore_key_t, 2>, arena_t>);
static_assert(!reads_deferred_concept<std::span<ustore_key_t const>, arena_t&>);
static_assert(!reads_deferred_concept<keys_view_t, arena_t&>);
static_assert(!reads_deferred_concept<st::basic_vector<ustore_key_t>&, arena_t&>);

// Nor do borrowed documents, fields or column types
static_assert(reads_documents_deferred_concept<std::array<ustore_key_t, 2>, arena_t&>);
static_assert(!reads_documents_deferred_concept<std::array<ustore_key_t, 2>, arena_t>);
static_assert(!reads_documents_deferred_concept<keys_view_t, arena_t&>);
static_assert(writes_documents_deferred_concept<values_tape_t, values_tape_t>);
static_assert(!writes_documents_deferred_concept<values_tape_t&, values_tape_t>);
static_assert(!writes_documents_deferred_concept<values_view_t, values_tape_t>);
static_assert(!writes_documents_deferred_concept<values_tape_t, values_tape_t&>);
static_assert(!writes_documents_deferred_concept<values_tape_t, strided_span<std::string_view const>>);
static_assert(gathers_deferred_concept<std::array<ustore_docs_field_type_t, 1>>);
static_assert(!gathers_deferred_concept<std::span<ustore_docs_field_type_t const>>);

// Nor borrowed vertices or edges
static_assert(reads_edges_deferred_concept<st::basic_vector<ustore_key_t>, arena_t&>);
static_assert(!reads_edges_deferred_concept<std::array<ustore_key_t, 2>, arena_t>);
static_assert(!reads_edges_deferred_concept<std::span<ustore_key_t const>, arena_t&>);
static_assert(writes_edges_deferred_concept<std::array<edge_t, 2>>);
static_assert(writes_edges_deferred_concept<st::basic_vector<edge_t>>);
static_assert(!writes_edges_deferred_concept<st::basic_vector<edge_t>&>);
static_assert(!writes_edges_deferred_concept<edges_view_t>);

// Nor borrowed strings or values
static_assert(reads_strings_deferred_concept<values_tape_t, arena_t&>);
static_assert(!reads_strings_deferred_concept<values_tape_t&, arena_t&>);
static_assert(!reads_strings_deferred_concept<std::span<value_view_t const>, arena_t&>);
static_assert(writes_strings_deferred_concept<values_tape_t, std::array<ustore_key_t, 2>>);
static_assert(!writes_strings_deferred_concept<values_tape_t, values_tape_t>);
static_assert(!writes_strings_deferred_concept<values_tape_t, std::span<ustore_key_t const>>);
static_assert(!writes_strings_deferred_concept<std::span<value_view_t const>, std::array<ustore_key_t, 2>>);

// A shorthand result owns its one-off arena, while a view only borrows one
static_assert(owns_arena_concept<owned_value_t, value_view_t>);
static_assert(owns_arena_concept<owned<values_view_t>, values_view_t>);
static_assert(!owns_arena_concept<value_view_t, value_view_t>);

// A progress callable never throws, as every wrapper is noexcept, and the call borrows it, so a temporary never does
using continuing_progress_t = decltype([](ustore_size_t, ustore_size_t) noexcept { return ustore_continue_k; });
using throwing_progress_t = decltype([](ustore_size_t, ustore_size_t) { return ustore_continue_k; });
static_assert(std::is_convertible_v<continuing_progress_t&, progress_arg_t>);
static_assert(!std::is_convertible_v<continuing_progress_t, progress_arg_t>);
static_assert(!std::is_convertible_v<throwing_progress_t&, progress_arg_t>);

/** Calls every C++ wrapper in each accepted form against one database. */
std::size_t wrappers_compile(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;

    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [main_collection, create_status] = database.create("main");
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
    blobs_collection_t prefixes = *database.find_or_create("prefixes");
    failures += expect(prefixes.at(key_from_integer(42)).assign("purpose").succeeded());
    failures += expect(database["articles"]->at(key_from_integer(42)).assign("of").succeeded());
    failures += expect(database["suffixes"]->at(key_from_integer(42)).assign("life").succeeded());

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
    docs_collection_t documents_collection = *database.find_or_create<docs_collection_t>("docs");
    failures += expect(
        documents_collection[key_from_integer(56)].assign(R"({"hello":"world","answer":42})").succeeded());
    [[maybe_unused]] bool const field_matches =
        documents_collection[collection_key_field_t {key_from_integer(56), "hello"}].value() == "world";

    // The same proxies running on an executor, and deferred verbs taking owned inputs
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 1});
    failures += expect(executor_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database_t::init_ram({}, executor).has_value());
    database_ref_t head_on_executor = database.on(executor);
    blobs_collection_t on_executor = *head_on_executor.find("main");
    failures += expect(on_executor[key_from_integer(42)].assign("purpose of life").succeeded());
    [[maybe_unused]] auto const on_executor_value = on_executor[key_from_integer(42)].value();
    failures += expect(head_on_executor["articles"]->at(key_from_integer(42)).assign("of").succeeded());

    deferred_database_t const deferred_database = database.deferred(executor);
    deferred_blobs_collection_t deferred_main = deferred_database[main_collection];
    std::array<ustore_key_t, 2> const owned_keys {key_from_integer(43), key_from_integer(44)};
    deferred<blobs_read_t> reading = deferred_main.read(owned_keys, arena);
    [[maybe_unused]] status_code_t const polled = reading.status();
    expected<values_view_t> read = std::move(reading).wait();
    failures += expect(read.has_value());
    if (read)
        for ([[maybe_unused]] value_view_t const element : *read) {}
    deferred<blobs_remove_t> removing = deferred_main.remove(owned_keys);
    failures += expect(std::move(removing).wait().succeeded());
    deferred_database_t deferred_head = database.deferred(executor);
    expected<ustore_collection_id_t> created = deferred_head.create("created").wait();
    failures += expect(created.has_value());
    if (created) {
        failures += expect(deferred_head[*created].remove(owned_keys).wait().succeeded());
        failures += expect(deferred_head.drop(*created).wait().succeeded());
    }
    failures += expect(deferred_head.collections(arena).wait().has_value());

    // Deferred documents, graphs and strings, each verb taking owned inputs
    failures += expect(database.find_or_create<docs_collection_t>("docs").has_value());
    deferred_docs_collection_t deferred_documents = *deferred_head.find<docs_collection_t>("docs");
    values_tape_t documents;
    failures += fill_tape(documents, {R"({"hello":"world"})", R"({"hello":"again"})"});
    failures += expect(deferred_documents.write(owned_keys, std::move(documents)).wait().succeeded());
    [[maybe_unused]] expected<values_view_t> const documents_read = deferred_documents.read(owned_keys, arena).wait();
    [[maybe_unused]] expected<sz::strings_view_t> const gist = deferred_documents.gist(owned_keys, arena).wait();
    values_tape_t hello;
    failures += fill_tape(hello, {"hello"});
    [[maybe_unused]] expected<docs_gather_t::finished_t> const gathered =
        deferred_documents.gather(owned_keys, std::move(hello), std::array {ustore_docs_field_type_str_k}, arena)
            .wait();
    failures += expect(deferred_documents.remove(owned_keys).wait().succeeded());
    failures += expect(database.create<graph_collection_t>("graph").has_value());
    deferred_graph_collection_t deferred_graph = *deferred_head.find<graph_collection_t>("graph");
    std::array<edge_t, 1> const owned_edges {edge_t {key_from_integer(43), key_from_integer(44)}};
    failures += expect(deferred_graph.write_edges(owned_edges).wait().succeeded());
    failures += expect(deferred_graph.write_vertices(owned_keys).wait().succeeded());
    [[maybe_unused]] expected<edges_view_t> const edges_read = deferred_graph.read_edges(owned_keys, arena).wait();
    [[maybe_unused]] expected<std::span<ustore_vertex_degree_t const>> const degrees =
        deferred_graph.degrees(owned_keys, arena, ustore_vertex_role_source_k).wait();
    failures += expect(deferred_graph.remove_edges(owned_edges).wait().succeeded());
    failures += expect(deferred_graph.remove_vertices(owned_keys).wait().succeeded());

    failures += expect(database.create<strings_collection_t>("strings").has_value());
    deferred_strings_collection_t deferred_strings = *deferred_head.find<strings_collection_t>("strings");
    values_tape_t texts, asked, string_prefixes, removed_strings;
    failures += fill_tape(texts, {"hello", "help"});
    failures += fill_tape(asked, {"hello"});
    failures += fill_tape(string_prefixes, {"hel"});
    failures += fill_tape(removed_strings, {"help"});
    std::array<ustore_key_t, 2> const string_keys {key_from_integer(1), key_from_integer(2)};
    failures += expect(deferred_strings.write(std::move(texts), string_keys).wait().succeeded());
    [[maybe_unused]] expected<keys_view_t> const strings_read = deferred_strings.read(std::move(asked), arena).wait();
    [[maybe_unused]] expected<strings_scan_t::finished_t> const scanned =
        deferred_strings.scan(std::move(string_prefixes), 8, arena).wait();
    failures += expect(deferred_strings.remove(std::move(removed_strings)).wait().succeeded());

    // Transactions on an executor, and their deferred verbs
    if (database.supports_transactions()) {
        transaction_t transaction = *database.on(executor).transaction();
        [[maybe_unused]] expected<docs_collection_t> const transaction_documents = transaction.find<docs_collection_t>(
            "docs");
        deferred_transaction_t deferred_transaction = transaction.deferred(executor);
        [[maybe_unused]] expected<deferred_blobs_collection_t> const deferred_named = deferred_transaction["docs"];
        failures += expect(deferred_transaction.find("main")->remove(owned_keys).wait().succeeded());
        [[maybe_unused]] expected<read_stamp_t> const staged = deferred_transaction.stage().wait();
        [[maybe_unused]] status_t const rolled_back = deferred_transaction.rollback().wait();
        [[maybe_unused]] expected<commit_stamp_t> const committed = deferred_transaction.commit().wait();
        failures += expect(deferred_transaction.reset().wait().succeeded());
    }

    // Snapshots hand out the read-only twin of every collection
    if (database.supports_snapshots()) {
        snapshot_t snapshot = *database.on(executor).snapshot();
        [[maybe_unused]] read_stamp_t const snapshot_stamp = snapshot.read_stamp();
        [[maybe_unused]] auto const snapshot_value = snapshot.find("main")->at(key_from_integer(42)).value();
        [[maybe_unused]] expected<docs_read_collection_t> const snapshot_documents = snapshot.find<docs_collection_t>(
            "docs");
        [[maybe_unused]] expected<graph_read_collection_t> const snapshot_graph = snapshot.find<graph_collection_t>(
            "graph");
        [[maybe_unused]] expected<strings_read_collection_t> const snapshot_strings =
            snapshot.find<strings_collection_t>("strings");
        deferred_blobs_read_collection_t deferred_snapshot = *snapshot.deferred(executor).find("main");
        failures += expect(deferred_snapshot.read(owned_keys, arena).wait().has_value());
        deferred_docs_read_collection_t deferred_snapshot_documents =
            *snapshot.deferred(executor).find<docs_collection_t>("docs");
        failures += expect(deferred_snapshot_documents.read(owned_keys, arena).wait().has_value());
        if (config.directory()) {
            path_t const saved {config.directory(), "saved"};
            failures += expect(create_directories(saved.c_str()));
            failures += expect(snapshot.deferred(executor).save(saved.c_str()).wait().succeeded());
        }
    }

    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
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
    return suite_exit_code(totals);
}
