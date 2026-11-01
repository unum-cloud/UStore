/**
 *  @file test/stress_linearizability.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Stress test that concurrent write transactions replay, in commit order, to the state the database holds.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`
#include <cstdio>  // `stderr`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::fill`, `std::max`, `std::ranges::max`
#include <array>     // `std::array`
#include <chrono>    // `std::chrono::steady_clock`
#include <limits>    // `std::numeric_limits`
#include <random>    // `std::mt19937`, `std::uniform_int_distribution`
#include <span>      // `std::span`
#include <utility>   // `std::exchange`, `std::move`

#include <fmt/format.h>

#include <smashtable/basic_vector.hpp>

#include "ustore/ustore.hpp"

#include "ustore/utilities/ram_device.hpp" // `caller_exclusivity_t`, `executor_t`

#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_eq`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::utilities;
using namespace unum;

/** Workload of the suite, each field set by the flag of the same name. */
struct linearizability_options_t {

    /** Wall-clock bound of the whole run, shared evenly by every configuration. */
    std::chrono::nanoseconds time_limit = std::chrono::seconds {10};

    /** Most rounds, each ending at a replayed checkpoint, one configuration runs. */
    count_t checkpoints {.value = 1'000, .minimum = 1};

    /** Writing thread counts, each run against every transaction count. */
    counts_t threads {.values = {2, 3, 4, 5, 6, 7, 8, 9, 10}, .size = 9, .minimum = 1};

    /** Transactions every thread commits between two checkpoints. */
    counts_t transactions {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 50, 100}, 13};

    /** Mean writes one key receives per round, which sizes the key range. */
    count_t writes_per_key {.value = 4, .minimum = 1};
};

/** Writes one transaction stages, split by the odds of an insert against a remove. */
struct operations_mix_t {

    /** Odds of an insert among the writes. */
    std::size_t inserts = 0;

    /** Odds of a remove among the writes. */
    std::size_t removes = 0;

    /** Writes one transaction stages. */
    constexpr std::size_t parts() const noexcept { return inserts + removes; }
};

/** Mixes every configuration runs, from inserts alone to remove-heavy transactions. */
constexpr std::array<operations_mix_t, 12> mixes_k {{
    {1, 0},
    {2, 0},
    {3, 0},
    {4, 0},
    {10, 0},
    {1, 1},
    {2, 1},
    {3, 1},
    {4, 1},
    {10, 1},
    {10, 5},
    {30, 3},
}};

/** Whether a staged write inserts or removes its key. */
enum class operation_code_t : std::uint8_t {
    insert_k,
    remove_k,
};

/** Value an insert writes, stored as its raw bytes. */
using payload_t = std::size_t;

/** One write a thread staged, stamped with the outcome of the transaction holding it. */
struct operation_t {

    /** Integer of the key the write targets, which also indexes the replay; the key is its @c key_of. */
    std::uint64_t key = 0;

    /** Value an insert writes. */
    payload_t value = 0;

    /** Stamp the transaction committed under, or the maximum when it failed. */
    commit_stamp_t stamp {std::numeric_limits<ustore_stamp_t>::max()};

    /** Whether the write inserts or removes. */
    operation_code_t code = operation_code_t::insert_k;

    /** Whether the transaction holding the write committed. */
    bool committed = false;

    /** The bytes of @c value, as an insert stages them. */
    value_view_t value_view() const noexcept {
        return value_view_t {reinterpret_cast<byte_t const*>(&value), sizeof(payload_t)};
    }
};

/** One configuration: a mix at a thread count, each thread committing a number of transactions per round. */
struct workload_t {

    /** Shares of inserts and removes. */
    operations_mix_t mix;

    /** Writing threads. */
    std::size_t threads_count = 1;

    /** Transactions every thread commits per round. */
    std::size_t transactions = 1;

    /** Integer of the largest key a write targets, the smallest being one. */
    std::uint64_t max_key = 1;

    /** Writes one thread logs per round. */
    std::size_t operations_per_thread() const noexcept { return transactions * mix.parts(); }
};

/** What one writing thread keeps across rounds, so a round allocates nothing and shares nothing. */
struct writer_slot_t {

    /** Transaction reset before every commit, reusing the engine's buffers. */
    transaction_t transaction;

    /** Generator of codes, keys and values, seeded once per run. */
    std::mt19937 random_generator;

    /** Failures the thread counted this round, folded in by the main thread once the round ends. */
    std::size_t failures = 0;

    writer_slot_t() noexcept = default;
};

/** What a run allocates once, sized for its largest configuration, and shares with every round. */
struct linearizability_run_t {

    /** Configuration every opening of the database reads. */
    config_t const* config = nullptr;

    /** Database reopened for every thread and transaction count, which bounds the remove deltas clears leave behind. */
    database_t database;

    /** Collection of the open database every transaction writes and every verification reads. */
    blobs_collection_t collection;

    /** Arena every verification reads into, rewound by each. */
    arena_t arena;

    /** One slot per thread of the largest configuration. */
    st::vector<writer_slot_t> slots;

    /** Per-thread logs laid end to end, each holding its thread's writes in staging order. */
    st::vector<operation_t> operations;

    /** The committed write that wins each key in the replay, indexed by key. */
    st::vector<operation_t const*> latest;

    /** Keys the replay leaves inserted, read back in one call. */
    st::vector<ustore_key_t> present_keys;
};

#pragma region Setup

/** Begins the transaction a slot resets before every commit. */
[[nodiscard]] std::size_t begin_transaction(database_t& database, transaction_t& transaction) {
    auto maybe_transaction = database.transaction();
    std::size_t const failures = expect(maybe_transaction);
    if (failures) return failures;
    transaction = *std::move(maybe_transaction);
    return 0;
}

/** Makes the arena of @p run, sizes its buffers for its largest configuration and seeds one generator per thread. */
[[nodiscard]] std::size_t prepare_run(linearizability_run_t& run, linearizability_options_t const& options,
                                      random_t random) {
    std::size_t parts = 0;
    for (operations_mix_t const mix : mixes_k) parts = std::max(parts, mix.parts());
    std::size_t const threads_count = std::ranges::max(options.threads.view());
    std::size_t const operations_count = threads_count * std::ranges::max(options.transactions.view()) * parts;
    std::size_t const keys_count = std::max<std::size_t>(operations_count / options.writes_per_key.value, 1) + 1;

    auto [arena, arena_status] = arena_t::make();
    run.arena = std::move(arena);
    std::size_t failures = expect(arena_status.succeeded());
    failures += expect(st::succeeded(run.slots.resize(threads_count)));
    failures += expect(st::succeeded(run.operations.resize(operations_count)));
    failures += expect(st::succeeded(run.latest.resize(keys_count)));
    failures += expect(st::succeeded(run.present_keys.resize(keys_count)));
    for (std::size_t index = 0; index != run.slots.size(); ++index)
        run.slots[index].random_generator.seed(static_cast<std::uint32_t>(random.fork(index).next()));
    return failures;
}

/** Reopens the database of @p run and begins one transaction per slot, freeing the previous ones first. */
[[nodiscard]] std::size_t reopen_run(linearizability_run_t& run) {
    for (writer_slot_t& slot : run.slots) slot.transaction = transaction_t {};
    if (run.database) run.database.close();
    auto [database, open_status] = open_database<database_t>(*run.config);
    std::size_t failures = expect(open_status.succeeded());
    if (failures) return failures;
    run.database = std::move(database);
    auto [collection, create_status] = run.database.find_or_create("linearizability");
    failures += expect(create_status.succeeded());
    run.collection = collection;
    for (std::size_t thread = 0; thread != run.slots.size() && !failures; ++thread)
        failures += begin_transaction(run.database, run.slots[thread].transaction);
    return failures;
}

#pragma endregion Setup

#pragma region Round

/** Stages and commits one transaction of random writes, stamping each logged write with the commit's outcome. */
std::size_t write_transaction(writer_slot_t& slot, ustore_collection_id_t collection, std::span<operation_t> batch,
                              operations_mix_t mix, std::uniform_int_distribution<std::uint64_t>& key_distribution) {
    std::size_t failures = expect(slot.transaction.reset().succeeded());
    blobs_collection_t inside {slot.transaction.receiver(), collection};
    for (operation_t& operation : batch) {
        operation.code = slot.random_generator() % mix.parts() < mix.inserts ? operation_code_t::insert_k
                                                                             : operation_code_t::remove_k;
        operation.key = key_distribution(slot.random_generator);
        operation.value = slot.random_generator();
        failures += operation.code == operation_code_t::insert_k
                        ? expect(inside[key_from_integer(operation.key)].assign(operation.value_view()).succeeded())
                        : expect(inside[key_from_integer(operation.key)].remove().succeeded());
    }

    auto maybe_stamp = slot.transaction.commit();
    bool const committed = maybe_stamp.has_value();
    commit_stamp_t const stamp = committed ? *maybe_stamp : commit_stamp_t {std::numeric_limits<ustore_stamp_t>::max()};
    for (operation_t& operation : batch) {
        operation.committed = committed;
        operation.stamp = stamp;
    }
    return failures;
}

/** Commits every transaction of one thread's round, logging its writes into @p log. */
void write_round(writer_slot_t& slot, ustore_collection_id_t collection, std::span<operation_t> log,
                 workload_t const& workload) {
    std::uniform_int_distribution<std::uint64_t> key_distribution(1, workload.max_key);
    std::size_t const parts = workload.mix.parts();
    for (std::size_t iteration = 0; iteration != workload.transactions; ++iteration)
        slot.failures += write_transaction(slot, collection, log.subspan(iteration * parts, parts), workload.mix,
                                           key_distribution);
}

/** Keeps, per key, the committed write with the highest stamp, and the later-logged one among equal stamps. */
void replay_round(std::span<operation_t const> operations, std::span<operation_t const*> latest) {
    std::fill(latest.begin(), latest.end(), nullptr);
    for (operation_t const& operation : operations) {
        if (!operation.committed) continue;
        operation_t const*& winner = latest[operation.key];
        if (!winner || winner->stamp <= operation.stamp) winner = &operation;
    }
}

/** Whether the replay leaves the key of @p winner inserted. */
bool leaves_inserted(operation_t const* winner) noexcept {
    return winner && winner->code == operation_code_t::insert_k;
}

/** Checks that @p retrieved holds the payload @p operation inserted. */
std::size_t expect_payload(value_view_t retrieved, operation_t const& operation) {
    std::size_t failures = expect_eq(retrieved.size(), sizeof(payload_t));
    if (retrieved.size() == sizeof(payload_t)) {
        payload_t stored = 0;
        std::memcpy(&stored, retrieved.data(), sizeof(payload_t));
        failures += expect_eq(stored, operation.value);
    }
    if (failures) fmt::println(stderr, "  for key {}", operation.key);
    return failures;
}

/** Checks the database against the replay: every inserted key holds its payload, and no other key is present. */
std::size_t verify_round(linearizability_run_t& run, std::span<operation_t const*> latest) {
    std::size_t present_count = 0;
    for (operation_t const* const winner : latest)
        if (leaves_inserted(winner)) run.present_keys[present_count++] = key_from_integer(winner->key);

    ustore_key_t const* const keys = run.present_keys.data();
    auto maybe_retrieved = run.collection[strided(keys, keys + present_count)].value(run.arena);
    std::size_t failures = expect(maybe_retrieved);
    if (!maybe_retrieved) return failures;

    std::size_t index = 0;
    for (operation_t const* const winner : latest)
        if (leaves_inserted(winner)) failures += expect_payload((*maybe_retrieved)[index++], *winner);
    failures += expect(run.collection.pairs().size() == present_count);
    return failures;
}

/** Runs one round: threads commit their transactions, then the main thread replays their logs, checks and clears. */
std::size_t run_round(linearizability_run_t& run, executor_t& executor, workload_t const& workload) {
    std::size_t const per_thread = workload.operations_per_thread();
    std::span<operation_t> const operations {run.operations.data(), workload.threads_count * per_thread};
    std::span<writer_slot_t> const slots {run.slots.data(), workload.threads_count};
    auto writer = [&](std::size_t thread) noexcept {
        write_round(slots[thread], run.collection, operations.subspan(thread * per_thread, per_thread), workload);
    };
    std::size_t failures = expect(st::succeeded(executor.for_threads(writer)));
    for (writer_slot_t& slot : slots) failures += std::exchange(slot.failures, 0);

    std::span<operation_t const*> const latest {run.latest.data(), static_cast<std::size_t>(workload.max_key) + 1};
    replay_round(operations, latest);
    failures += verify_round(run, latest);
    failures += expect(run.database.clear(ustore_drop_pairs_k).succeeded());
    return failures;
}

#pragma endregion Round

#pragma region Configurations

/**
 *  @brief Checks that the state after each round of concurrent random writes matches their replay in commit order.
 *
 *  Every round ends at a checkpoint where all threads stop, the main thread replays the committed writes in
 *  stamp order, compares the replay against the database, and clears it for the next round.
 *
 *  @param[in,out] run Database, logs and thread slots the run allocated once.
 *  @param[in,out] executor Pool of exactly the workload's thread count.
 *  @param[in] workload Mix, threads and transactions of every round.
 *  @param[in] checkpoints Most rounds to run.
 *  @param[in] slice Time after which no further round starts; the first round always runs.
 */
std::size_t linear_writes(linearizability_run_t& run, executor_t& executor, workload_t const& workload,
                          std::size_t checkpoints, std::chrono::nanoseconds slice) {
    auto const deadline = std::chrono::steady_clock::now() + slice;
    std::size_t failures = 0;
    for (std::size_t checkpoint = 0; checkpoint != checkpoints && !failures; ++checkpoint) {
        if (checkpoint != 0 && std::chrono::steady_clock::now() >= deadline) break;
        failures += run_round(run, executor, workload);
    }
    return failures;
}

/** Runs every mix of inserts and removes at @p threads threads committing @p transactions per round. */
std::size_t linear_writes_mixes(linearizability_run_t& run, executor_t& executor,
                                linearizability_options_t const& options, std::size_t threads, std::size_t transactions,
                                std::chrono::nanoseconds slice) {
    std::size_t failures = 0;
    for (operations_mix_t const mix : mixes_k) {
        std::size_t const writes = mix.parts() * transactions * threads;
        std::uint64_t const max_key = std::max<std::uint64_t>(writes / options.writes_per_key.value, 1);
        workload_t const workload {mix, threads, transactions, max_key};
        failures += linear_writes(run, executor, workload, options.checkpoints.value, slice);
    }
    return failures;
}

/** Runs and reports every transaction count on one pool of @p threads threads, each on a freshly opened database. */
void linear_writes_on_threads(suite_totals_t& totals, linearizability_run_t& run,
                              linearizability_options_t const& options, std::size_t threads,
                              std::chrono::nanoseconds slice) {
    auto maybe_executor = executor_t::make(threads, caller_exclusivity_t::caller_inclusive_k);
    for (std::size_t const transactions : options.transactions.view()) {
        char name[64];
        auto const written = fmt::format_to_n(name, sizeof(name) - 1, "linear_writes.threads_{}.between_{}", threads,
                                              transactions);
        *written.out = '\0';
        if (!totals.selects(name)) continue;
        std::size_t failures = expect(maybe_executor);
        if (!failures) failures += reopen_run(run);
        if (!failures) failures += linear_writes_mixes(run, *maybe_executor, options, threads, transactions, slice);
        report(totals, name, failures);
    }
}

#pragma endregion Configurations

int main(int argc, char** argv) {
    suite_options_t suite;
    linearizability_options_t options;
    std::array<flag_t, 5> const flags {{
        {"--time-limit", "duration", "bound of the whole run, like 10s", &options.time_limit},
        {"--checkpoints", "count", "most replayed rounds per configuration", &options.checkpoints},
        {"--threads", "counts", "writing thread counts, like 2,4,8", &options.threads},
        {"--transactions", "counts", "transactions per thread between checkpoints, like 1,10,100",
         &options.transactions},
        {"--writes-per-key", "count", "mean writes one key receives per round", &options.writes_per_key},
    }};
    if (argument_status_t const status = parse_suite_options(argc, argv, suite, flags);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = suite.totals();

    auto const maybe_directory = directory_t::make(suite.directory);
    if (!maybe_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", suite.directory);
        return 1;
    }
    config_t const config {suite.engine, maybe_directory->path()};
    linearizability_run_t run {&config};
    auto [database, open_status] = open_database<database_t>(*run.config);
    if (open_status.failed()) {
        fmt::println(stderr, "Cannot open the database");
        return 1;
    }
    run.database = std::move(database);
    if (!run.database.supports_transactions()) {
        report(totals, "linear_writes", case_result_t::skipped("the engine does not support transactions"));
        return suite_exit_code(totals);
    }
    if (std::size_t const failures = prepare_run(run, options, suite.random()); failures) {
        report(totals, "setup", failures);
        return suite_exit_code(totals);
    }

    std::size_t const configurations_count = options.threads.size * options.transactions.size * mixes_k.size();
    std::chrono::nanoseconds const slice = options.time_limit / configurations_count;
    for (std::size_t const threads : options.threads.view())
        linear_writes_on_threads(totals, run, options, threads, slice);
    return suite_exit_code(totals);
}
