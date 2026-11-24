/**
 *  @file cpp/test.cpp
 *  @author Ash Vardanian
 *  @date November 18, 2026
 *  @brief The smoke test of the C++ wrappers: the README's Quickstart, one round trip through every
 *      modality and handle they own, lookups by name, and deferred calls waited on and awaited,
 *      against both the engine and the brute-force stub.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int64_t`
#include <cstdlib> // `std::abort`

#include <array>       // `std::array`
#include <coroutine>   // `std::suspend_never`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <fmt/format.h>

#include "ustore/ustore.hpp"

namespace us = unum::ustore;

/** Checks that failed so far, which the run's exit code reports. */
static std::size_t failed_checks = 0;

/** Counts and prints a failed check, naming what was expected. */
static void expect_true(bool condition, std::string_view expectation) noexcept {
    if (condition) return;
    ++failed_checks;
    fmt::println(stderr, "- Failed: {}", expectation);
}

/** Counts and prints a failed call with its status, naming what it was doing. */
static void expect_success(us::status_t const& status, std::string_view doing) noexcept {
    if (!status.failed()) return;
    ++failed_checks;
    fmt::println(stderr, "- Failed {}: {}", doing, status.note ? status.note : "no note");
}

/** Runs one case and prints its verdict. */
static void run_test_case(std::string_view name, void (*test_case)()) noexcept {
    std::size_t const failed_before = failed_checks;
    test_case();
    fmt::println("{} {}", failed_checks == failed_before ? "Passed" : "Failed", name);
}

#pragma region Quickstart

/** The README's Quickstart: opens a database in memory, writes a blob, a document and an edge in
 *  one transaction, and reads them back. */
static int quickstart() noexcept {
    auto [database, database_status] = us::database_t::init_ram();
    if (database_status.failed()) return 1;
    ustore_key_t const avatar = us::key_from_integer(1), titanic = us::key_from_integer(2);
    us::database_t const& head = database; // the calling thread
    auto [posters, posters_status] = head.find_or_create("posters");
    auto [movies, movies_status] = head.find_or_create<us::docs_collection_t>("movies");
    auto [similar, similar_status] = head.find_or_create<us::graph_collection_t>("similar");
    if (posters_status.failed() || movies_status.failed() || similar_status.failed()) return 1;

    // One transaction writes a blob, a document and an edge, and commits all three or none.
    auto [transaction, transaction_status] = head.transaction();
    if (transaction_status.failed()) return 1;
    us::transaction_t const& changes = transaction;
    auto [poster_changes, poster_status] = changes.find("posters");
    auto [movie_changes, movie_status] = changes.find<us::docs_collection_t>("movies");
    auto [similar_changes, edge_status] = changes.find<us::graph_collection_t>("similar");
    if (poster_status.failed() || movie_status.failed() || edge_status.failed()) return 1;
    if (poster_changes[avatar].assign("\x89PNG").failed()) return 1;
    if (movie_changes[avatar].assign(R"({"title":"Avatar","year":2009})").failed()) return 1;
    if (similar_changes.write_edge({avatar, titanic}).failed()) return 1;
    if (changes.commit().status.failed()) return 1;

    // Reads at the head see the commit: the poster, the year and the one edge leaving Avatar.
    auto [poster, poster_read_status] = posters[avatar].value();
    auto [year, year_status] = movies[us::collection_key_field_t {avatar, "year"}].value();
    auto [arena, arena_status] = us::arena_t::make();
    if (poster_read_status.failed() || year_status.failed() || arena_status.failed()) return 1;
    auto [edges, edges_status] = similar.read_edges(avatar, arena, ustore_vertex_role_source_k);
    if (edges_status.failed()) return 1;
    bool const committed = poster == us::value_view_t {"\x89PNG"} && year == us::value_view_t {"2009"} &&
                           edges.size() == 1 && edges[0].target == titanic;
    return committed ? 0 : 1;
}

/** The README's Quickstart reads back everything it wrote. */
static void test_readme_quickstart() { expect_true(quickstart() == 0, "The README's Quickstart must read its writes"); }

#pragma endregion Quickstart

#pragma region Modalities

/** A predicate counting the keys it is asked about, which only one thread could call safely, as a
 *  @c mutable lambda would. */
struct counting_predicate_t {
    std::size_t asked = 0;
    bool operator()(ustore_key_t) noexcept { return ++asked != 0; }
};

/** A predicate reading only what it was built with, safe on every executor thread at once. */
struct reading_predicate_t {
    ustore_key_t refused {};
    bool operator()(ustore_key_t key) const noexcept { return key != refused; }
};

/** A batch predicate counting the pages it is asked about, which only one thread could call
 *  safely, as a @c mutable lambda would. */
struct counting_batch_predicate_t {
    std::size_t asked = 0;
    void operator()(std::span<ustore_key_t const>, std::span<ustore_admission_t>) noexcept { ++asked; }
};

/** A batch predicate admitting every key, safe on every executor thread at once. */
struct admitting_batch_predicate_t {
    void operator()(std::span<ustore_key_t const>, std::span<ustore_admission_t> admissions) const noexcept {
        for (ustore_admission_t& admission : admissions) admission = ustore_admit_k;
    }
};

static_assert(!us::key_predicate_handler<counting_predicate_t>, "a predicate runs on several threads at once");
static_assert(us::key_predicate_handler<reading_predicate_t>);
static_assert(!us::keys_predicate_handler<counting_batch_predicate_t>, "a predicate runs on several threads at once");
static_assert(us::keys_predicate_handler<admitting_batch_predicate_t>);

/** Blobs written in a batch read back in place, count exactly, and answer shorthand reads. */
static void test_blobs_round_trip() {
    auto [database, database_status] = us::database_t::init_ram();
    expect_success(database_status, "opening a database");
    auto [blobs, blobs_status] = database.create("blobs");
    expect_success(blobs_status, "creating a collection");
    std::array<ustore_key_t, 3> const keys {us::key_from_integer(1), us::key_from_integer(2), us::key_from_integer(3)};
    std::array<std::string_view, 3> const values {"first", "", "third"};
    expect_success(blobs.write(keys, values), "writing a batch");

    auto [arena, arena_status] = us::arena_t::make();
    expect_success(arena_status, "making an arena");
    auto [read, read_status] = blobs.read(keys, arena);
    expect_success(read_status, "reading a batch");
    expect_true(read.size() == 3 && read[0] == us::value_view_t {"first"} && read[1].empty() && read[1],
                "A batch must read back as written, an empty value present");
    auto [size, size_status] = blobs.size();
    expect_true(!size_status.failed() && size == 3, "The size must count every key exactly");
    auto [bounds, bounds_status] = blobs.size_range();
    expect_success(bounds_status, "estimating the size");
    expect_true(bounds.min <= 3 && 3 <= bounds.max, "The estimated bounds must hold the exact size");
    auto [missing, missing_status] = blobs[us::key_from_integer(4)].value();
    expect_true(!missing_status.failed() && !missing, "A missing key must read as a missing value");
    expect_true(blobs.write(keys, std::span {values}.first(2)) == us::status_code_t::invalid_argument_k,
                "Keys and values of different counts must be refused");

    auto [executor, executor_status] = us::executor_t::for_ram({.threads = 2});
    expect_success(executor_status, "spawning an executor");
    us::keys_range_t pulled = blobs.on(executor).keys(us::key_from_integer(2), us::key_from_integer(4));
    std::size_t bounded = 0;
    for (ustore_key_t const key : pulled) bounded += key == keys[1] || key == keys[2];
    expect_success(pulled.status(), "walking a slice on an executor");
    expect_true(bounded == 2, "A slice must walk the keys from its start up to its end");
    us::pairs_range_t pairs = blobs.pairs();
    std::size_t matched = 0;
    for (auto [key, value] : pairs) matched += key == keys[0] && value == us::value_view_t {"first"};
    expect_success(pairs.status(), "walking every pair on the calling thread");
    expect_true(matched == 1, "A walk over pairs must read every value beside its key");
}

/** A graph's neighbors read as stored, and its presences cover vertices without edges. */
static void test_graph_views() {
    auto [database, database_status] = us::database_t::init_ram();
    expect_success(database_status, "opening a database");
    auto [graph, graph_status] = database.create<us::graph_collection_t>("graph");
    expect_success(graph_status, "creating a graph");
    ustore_key_t const hub = us::key_from_integer(1), spoke = us::key_from_integer(2), lonely = us::key_from_integer(3),
                       absent = us::key_from_integer(4);
    std::array<us::edge_t, 2> const edges {us::edge_t {hub, spoke}, us::edge_t {spoke, hub}};
    expect_success(graph.write_edges(edges), "writing edges");
    expect_success(graph.write_vertex(lonely), "writing a vertex without edges");

    auto [arena, arena_status] = us::arena_t::make();
    expect_success(arena_status, "making an arena");
    auto [neighbors, neighbors_status] = graph.neighbors(hub, arena);
    expect_success(neighbors_status, "reading neighbors");
    std::size_t spokes = 0;
    for (ustore_key_t const neighbor : neighbors) spokes += neighbor == spoke;
    expect_true(neighbors.size() == 2 && spokes == 2, "Both edges must name the spoke, as stored");

    std::array<ustore_key_t, 3> const vertices {hub, lonely, absent};
    auto [presences, presences_status] = graph.present(vertices, arena);
    expect_success(presences_status, "reading presences");
    expect_true(presences[0] && presences[1] && !presences[2], "A vertex without edges must still be present");
    auto [degree, degree_status] = graph.degree(absent);
    expect_true(!degree_status.failed() && degree == ustore_vertex_degree_missing_k,
                "A missing vertex must read its degree as missing");
}

/** Documents read back whole and field by field, and a gathered column converts. */
static void test_docs_round_trip() {
    auto [database, database_status] = us::database_t::init_ram();
    expect_success(database_status, "opening a database");
    auto [docs, docs_status] = database.create<us::docs_collection_t>("docs");
    expect_success(docs_status, "creating a documents collection");
    std::array<ustore_key_t, 2> const keys {us::key_from_integer(1), us::key_from_integer(2)};
    std::array<std::string_view, 2> const documents {R"({"age":7})", R"({"age":9})"};
    expect_success(docs.write(keys, documents), "writing documents");

    auto [arena, arena_status] = us::arena_t::make();
    expect_success(arena_status, "making an arena");
    auto [table, table_status] = docs.gather(keys, us::make_table_header().with<std::int64_t>("age"), arena);
    expect_success(table_status, "gathering a column");
    us::scalar_column_view<std::int64_t> const ages = table.column<0>();
    expect_true(ages.size() == 2 && ages[0].valid && ages[0].value == 7 && ages[1].value == 9,
                "A gathered column must convert every document's field");

    us::table_header<std::int64_t> const header = us::make_table_header().with<std::int64_t>("age");
    auto [cursor, cursor_status] = us::docs_cursor_t::make(docs.receiver(), docs.handle(), us::smallest_key_k, {},
                                                           header.view());
    expect_success(cursor_status, "opening a documents cursor");
    auto [batches, batches_status] = cursor.next(keys.size(), arena);
    expect_success(batches_status, "pulling documents with a column");
    std::size_t rows = 0;
    for (ustore_docs_batch_view_t const& batch : batches) rows += batch.count;
    expect_true(rows == keys.size(), "A documents cursor must pull every row in typed batches");
    auto [count, count_status] = docs.size();
    expect_true(!count_status.failed() && count == 2, "The size must count every document exactly");
}

/** Strings map onto keys, and a prefix streams them in the order of their text. */
static void test_strings_round_trip() {
    auto [database, database_status] = us::database_t::init_ram();
    expect_success(database_status, "opening a database");
    auto [strings, strings_status] = database.create<us::strings_collection_t>("strings");
    expect_success(strings_status, "creating a strings collection");
    expect_success(strings.assign("beta", us::key_from_integer(2)), "assigning a string");
    expect_success(strings.assign("alpha", us::key_from_integer(1)), "assigning a string");
    expect_success(strings.assign("gamma", us::key_from_integer(3)), "assigning a string");

    auto [key, key_status] = strings.value("alpha");
    expect_true(!key_status.failed() && key == us::key_from_integer(1), "A string must map onto its key");
    us::strings_collection_t::prefix_range_t walk = strings.prefixed();
    std::array<std::string_view, 3> const ordered {"alpha", "beta", "gamma"};
    std::size_t position = 0;
    for (std::string_view const text : walk)
        expect_true(position < 3 && text == ordered[position++], "A walk must follow the order of the text");
    expect_success(walk.status(), "walking every string");
    expect_true(position == 3, "A walk must visit every string");
}

#pragma endregion Modalities

#pragma region Lookups and Transactions

/** A lookup by name answers the handle of its modality, refusing a missing name and another
 *  modality, in a blocking and in a deferred receiver alike. */
static void test_named_lookups() {
    auto [executor, executor_status] = us::executor_t::for_ram({.threads = 2});
    expect_success(executor_status, "spawning an executor");
    auto [database, database_status] = us::database_t::init_ram();
    expect_success(database_status, "opening a database");
    auto [graph, graph_status] = database.create<us::graph_collection_t>("graph");
    expect_success(graph_status, "creating a graph");

    auto [found, found_status] = database.find<us::graph_collection_t>("graph");
    expect_true(!found_status.failed() && found.handle() == graph.handle(), "A name must find its collection");
    expect_true(database.find("missing").status == us::status_code_t::not_found_k, "A missing name must not be found");
    expect_true(database.find("graph").status == us::status_code_t::invalid_argument_k,
                "Another modality must be refused");
    auto [present, present_status] = database.contains("graph");
    expect_true(!present_status.failed() && present, "A created name must be contained");

    auto [arena, arena_status] = us::arena_t::make({.executor = executor.handle()});
    expect_success(arena_status, "making an arena");
    us::database_on<ustore_completion_deferred_k> const finding = database.deferred(executor);
    us::deferred<us::graph_collection_on<ustore_completion_deferred_k, us::collection_access_t::read_write_k>> call =
        finding.find<us::graph_collection_t>("graph", arena);
    auto [deferred_found, deferred_status] = call.wait();
    expect_true(!deferred_status.failed() && deferred_found.handle() == graph.handle(),
                "A deferred lookup must answer what a blocking one does");
}

/** The probes report what the engine supports, a transaction commits, and a snapshot pinned
 *  before the commit keeps reading what it saw. */
static void test_transactions_and_snapshots() {
    auto [database, database_status] = us::database_t::init_ram();
    expect_success(database_status, "opening a database");
    auto [transactional, probe_status] = database.supports_transactions();
    expect_success(probe_status, "probing for transactions");
    if (!transactional) return;
    auto [blobs, blobs_status] = database.create("blobs");
    expect_success(blobs_status, "creating a collection");
    ustore_key_t const key = us::key_from_integer(1);
    expect_success(blobs[key].assign("before"), "writing at the head");

    auto [snapshot, snapshot_status] = database.snapshot();
    expect_success(snapshot_status, "pinning a snapshot");
    auto [transaction, transaction_status] = database.transaction();
    expect_success(transaction_status, "beginning a transaction");
    auto [changed, changed_status] = transaction.find("blobs");
    expect_success(changed_status, "finding the collection in the transaction");
    expect_success(changed[key].assign("after"), "writing in the transaction");
    auto [stamp, commit_status] = transaction.commit();
    expect_success(commit_status, "committing");

    auto [head, head_status] = blobs[key].value();
    expect_true(!head_status.failed() && head == us::value_view_t {"after"}, "The head must see the commit");
    auto [pinned, pinned_status] = snapshot.find("blobs");
    expect_success(pinned_status, "finding the collection at the snapshot");
    auto [seen, seen_status] = pinned[key].value();
    expect_true(!seen_status.failed() && seen == us::value_view_t {"before"},
                "A snapshot must keep reading what it pinned");
}

#pragma endregion Lookups and Transactions

#pragma region Deferred Calls

/** A fixed ring of posted polls, the loop a deferred call is awaited on, allocating nothing. */
struct test_loop_t {
    struct posted_t {
        void (*poll)(void*) noexcept;
        void* state;
    };
    std::array<posted_t, 64> ring {};
    std::size_t head = 0, tail = 0;

    void post(void (*poll)(void*) noexcept, void* state) noexcept {
        expect_true(tail - head < ring.size(), "The test loop's ring must never overflow");
        ring[tail++ % ring.size()] = {poll, state};
    }
    void run() noexcept {
        while (head != tail) {
            posted_t const next = ring[head++ % ring.size()];
            next.poll(next.state);
        }
    }
};

/** A coroutine that starts at once and runs to its end on whatever resumes it. */
struct test_task_t {
    struct promise_type {
        test_task_t get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::abort(); }
    };
};

/** Plays an engine whose deferred call finishes only after @c turns_left more turns of the loop,
 *  as a real one finishes while its caller polls. */
struct late_engine_t {
    ustore_outcome_t* outcome = nullptr;
    std::size_t* written = nullptr;
    std::size_t turns_left = 3;
    test_loop_t* loop = nullptr;

    static void turn(void* state) noexcept {
        late_engine_t& engine = *static_cast<late_engine_t*>(state);
        if (--engine.turns_left != 0) return engine.loop->post(&turn, state);
        *engine.written = 42;
        engine.outcome->status = ustore_status_success_k;
    }
};

/** Awaits a deferred read of @p keys in @p blobs on @p loop, counting the values it found into
 *  @p found. */
static test_task_t awaited_read(us::blobs_collection_t const& blobs, us::executor_t const& executor,
                                std::span<ustore_key_t const> keys, us::arena_t& arena, test_loop_t& loop,
                                std::size_t& found) {
    // The deferred handle names the collection the call reads, so it outlives the call
    us::blobs_collection_on<ustore_completion_deferred_k, us::collection_access_t::read_write_k> const reading =
        blobs.deferred(executor);
    us::deferred<us::values_view_t> call = reading.read(keys, arena);
    auto [values, status] = co_await call.wait(loop); // or: call.wait()
    expect_success(status, "awaiting a deferred read");
    for (us::value_view_t const value : values) found += static_cast<bool>(value);
}

/** Reads @p keys from two collections at once on @p loop, as the README's fan-out does, counting
 *  the values both found into @p found. */
static test_task_t awaited_fan_out(us::blobs_collection_t const& east, us::blobs_collection_t const& west,
                                   us::executor_t const& east_executor, us::executor_t const& west_executor,
                                   std::span<ustore_key_t const> keys, test_loop_t& loop, std::size_t& found) {
    auto [east_arena, east_arena_status] = us::arena_t::make({.executor = east_executor.handle()});
    auto [west_arena, west_arena_status] = us::arena_t::make({.executor = west_executor.handle()});
    expect_success(east_arena_status, "making the east arena");
    expect_success(west_arena_status, "making the west arena");
    us::deferred<us::values_view_t> east_call = east.deferred(east_executor).read(keys, east_arena);
    us::deferred<us::values_view_t> west_call = west.deferred(west_executor).read(keys, west_arena);
    auto [east_values, east_status] = co_await east_call.wait(loop); // both reads were already running
    auto [west_values, west_status] = co_await west_call.wait(loop);
    expect_success(east_status, "awaiting the east read");
    expect_success(west_status, "awaiting the west read");
    for (us::value_view_t const value : east_values) found += static_cast<bool>(value);
    for (us::value_view_t const value : west_values) found += static_cast<bool>(value);
}

/** Awaits a call @p engine finishes later on @p loop and writes its answer into @p answered. */
static test_task_t awaited_late_call(late_engine_t& engine, test_loop_t& loop, std::size_t& answered) {
    us::deferred<std::size_t> call([&](ustore_outcome_t* outcome, us::outputs<std::size_t>* written) noexcept {
        engine.outcome = outcome;
        engine.written = &written->value;
        outcome->status = ustore_status_deferred_k;
    });
    loop.post(&late_engine_t::turn, &engine);
    auto [value, status] = co_await call.wait(loop);
    expect_success(status, "awaiting a call that finishes later");
    answered = value;
}

/** Deferred calls answer what blocking ones would, when waited on and when awaited through the
 *  loop's ring, suspending only while the call is not final. */
static void test_deferred_calls() {
    auto [executor, executor_status] = us::executor_t::for_ram({.threads = 2});
    expect_success(executor_status, "spawning an executor");
    auto [database, database_status] = us::database_t::init_ram();
    expect_success(database_status, "opening a database");
    auto [blobs, blobs_status] = database.create("blobs");
    expect_success(blobs_status, "creating a collection");
    std::array<ustore_key_t, 2> const keys {us::key_from_integer(1), us::key_from_integer(2)};
    std::array<std::string_view, 2> const values {"one", "two"};
    us::blobs_collection_on<ustore_completion_deferred_k, us::collection_access_t::read_write_k> const writing =
        blobs.deferred(executor);
    us::deferred<void> written = writing.write(keys, values);
    expect_success(written.wait(), "waiting on a deferred write");

    auto [arena, arena_status] = us::arena_t::make({.executor = executor.handle()});
    expect_success(arena_status, "making an arena");
    test_loop_t loop;
    std::size_t found = 0;
    awaited_read(blobs, executor, keys, arena, loop, found);
    loop.run();
    expect_true(found == 2, "An awaited read must see every value written");

    auto [west_executor, west_executor_status] = us::executor_t::for_ram({.threads = 1});
    expect_success(west_executor_status, "spawning a second executor");
    auto [west, west_status] = database.create("west");
    expect_success(west_status, "creating a second collection");
    expect_success(west.write(keys, values), "writing on the calling thread");
    std::size_t fanned = 0;
    awaited_fan_out(blobs, west, executor, west_executor, keys, loop, fanned);
    loop.run();
    expect_true(fanned == 4, "A fan-out must see every value of both collections");

    late_engine_t engine {.loop = &loop};
    std::size_t answered = 0;
    awaited_late_call(engine, loop, answered);
    expect_true(answered == 0, "An awaited call must not resume before it is final");
    loop.run();
    expect_true(answered == 42 && engine.turns_left == 0, "An awaited call must resume once final, with its outputs");
    expect_true(loop.head == loop.tail, "The loop must drain every poll it was posted");

    us::deferred<void> refused = writing.write(keys, std::span {values}.first(1));
    expect_true(refused.status() == us::status_code_t::invalid_argument_k, "A refused call must be final at once");
}

#pragma endregion Deferred Calls

int main() {
    fmt::println("UStore {}", ustore_version_name());
    run_test_case("test_readme_quickstart", test_readme_quickstart);
    run_test_case("test_blobs_round_trip", test_blobs_round_trip);
    run_test_case("test_graph_views", test_graph_views);
    run_test_case("test_docs_round_trip", test_docs_round_trip);
    run_test_case("test_strings_round_trip", test_strings_round_trip);
    run_test_case("test_named_lookups", test_named_lookups);
    run_test_case("test_transactions_and_snapshots", test_transactions_and_snapshots);
    run_test_case("test_deferred_calls", test_deferred_calls);
    fmt::println("{} checks failed", failed_checks);
    return failed_checks == 0 ? 0 : 1;
}
