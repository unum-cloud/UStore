/**
 *  @file test/abi/stress_linearizability.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Stress test that concurrent write transactions replay, in commit order, to the state
 *      the database holds.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`
#include <cstdio>  // `stderr`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::fill`, `std::ranges::max`
#include <array>     // `std::array`
#include <chrono>    // `std::chrono::steady_clock`
#include <limits>    // `std::numeric_limits`
#include <span>      // `std::span`
#include <utility>   // `std::move`

#include <fmt/format.h>

#include <smashtable/basic_vector.hpp>

#include "ustore/ustore.hpp"

#include "ustore/utilities/ram_device.hpp" // `caller_exclusivity_t`, `executor_t`

#include "harness.hpp" // `run_test_case`, `expect_true`, `expect_equal`, `split_mix_generator_t`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::utilities;
using namespace unum;

/** Workload of the suite, each field set by the setting of the same name. */
struct linearizability_options_t {

    /** Wall-clock bound of the whole run, shared evenly by every configuration. */
    std::chrono::milliseconds time_limit = std::chrono::seconds {10};

    /** Most rounds, each ending at a replayed checkpoint, one configuration runs. */
    count_t checkpoints {.value = 1'000, .minimum = 1};

    /** Writing thread counts, each run against every transaction count. */
    counts_t thread_counts {.values = {2, 3, 4, 5, 6, 7, 8, 9, 10}, .size = 9, .minimum = 1};

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

    /** Integer of the key the write targets, which also indexes the replay; the key is
     *  its @c key_of. */
    std::uint64_t key = 0;

    /** Value an insert writes. */
    payload_t value = 0;

    /** Stamp the transaction committed under, or the maximum when it failed. */
    commit_stamp_t stamp {(std::numeric_limits<ustore_stamp_t>::max)()};

    /** Whether the write inserts or removes. */
    operation_code_t code = operation_code_t::insert_k;

    /** Whether the transaction holding the write committed. */
    bool committed = false;

    /** The bytes of @c value, as an insert stages them. */
    value_view_t value_view() const noexcept {
        return value_view_t {reinterpret_cast<byte_t const*>(&value), sizeof(payload_t)};
    }
};

/** One configuration: a mix at a thread count, each thread committing a number of
 *  transactions per round. */
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

    /** Generator of codes, keys and values, seeded once per case. */
    split_mix_generator_t random_generator {0};

    writer_slot_t() noexcept = default;
};

/** What a run allocates once, sized for its largest configuration, and shares with every round. */
struct linearizability_run_t {

    /** Configuration every opening of the database reads. */
    config_t const* config = nullptr;

    /** Database reopened for every thread and transaction count, which bounds the remove deltas
     *  clears leave behind. */
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
bool begin_transaction(database_t& database, transaction_t& transaction) {
    auto maybe_transaction = database.on().transaction();
    if (!expect_true(maybe_transaction.has_value())) return false;
    transaction = *std::move(maybe_transaction);
    return true;
}

/** Makes the arena of @p run and sizes its buffers for its largest configuration. */
bool prepare_run(linearizability_run_t& run, linearizability_options_t const& options) {
    std::size_t parts = 0;
    for (operations_mix_t const mix : mixes_k) parts = fu::max_of(parts, mix.parts());
    std::size_t const threads_count = (std::ranges::max)(options.thread_counts.view());
    std::size_t const operations_count = threads_count * (std::ranges::max)(options.transactions.view()) * parts;
    std::size_t const keys_count = fu::max_of<std::size_t>(operations_count / options.writes_per_key.value, 1) + 1;

    auto [arena, arena_status] = arena_t::make();
    run.arena = std::move(arena);
    return expect_true(arena_status.succeeded()) && expect_true(st::succeeded(run.slots.resize(threads_count))) &&
           expect_true(st::succeeded(run.operations.resize(operations_count))) &&
           expect_true(st::succeeded(run.latest.resize(keys_count))) &&
           expect_true(st::succeeded(run.present_keys.resize(keys_count)));
}

/** Reopens the database of @p run and begins one transaction per slot, freeing the
 *  previous ones first. */
bool reopen_run(linearizability_run_t& run) {
    for (writer_slot_t& slot : run.slots) slot.transaction = transaction_t {};
    if (run.database) run.database.close();
    auto [database, open_status] = open_database<database_t>(*run.config);
    if (!expect_true(open_status.succeeded())) return false;
    run.database = std::move(database);
    auto [collection, create_status] = run.database.on().find_or_create("linearizability");
    if (!expect_true(create_status.succeeded())) return false;
    run.collection = collection;
    for (writer_slot_t& slot : run.slots)
        if (!begin_transaction(run.database, slot.transaction)) return false;
    return true;
}

#pragma endregion Setup

#pragma region Round

/** Stages and commits one transaction of random writes to keys up to @p max_key , stamping each
 *  logged write with the commit's outcome. */
void write_transaction(writer_slot_t& slot, ustore_collection_id_t collection, std::span<operation_t> batch,
                       operations_mix_t mix, std::uint64_t max_key) {
    expect_true(slot.transaction.on().reset().has_value());
    blobs_collection_t inside {slot.transaction.receiver(), collection};
    for (operation_t& operation : batch) {
        operation.code = slot.random_generator.below(mix.parts()) < mix.inserts ? operation_code_t::insert_k
                                                                                : operation_code_t::remove_k;
        operation.key = 1 + slot.random_generator.below(max_key);
        operation.value = slot.random_generator();
        // A stage refused for a busy channel changed nothing, and another thread gives
        // the channel back
        status_t staged;
        do {
            staged = operation.code == operation_code_t::insert_k
                         ? inside[key_from_integer(operation.key)].assign(operation.value_view())
                         : inside[key_from_integer(operation.key)].remove();
        } while (staged == status_code_t::capacity_exhausted_k);
        expect_true(staged.succeeded());
    }

    auto maybe_stamp = slot.transaction.on().commit();
    bool const committed = maybe_stamp.has_value();
    commit_stamp_t const stamp = committed ? *maybe_stamp
                                           : commit_stamp_t {(std::numeric_limits<ustore_stamp_t>::max)()};
    for (operation_t& operation : batch) {
        operation.committed = committed;
        operation.stamp = stamp;
    }
}

/** Commits every transaction of one thread's round, logging its writes into @p log. */
void write_round(writer_slot_t& slot, ustore_collection_id_t collection, std::span<operation_t> log,
                 workload_t const& workload) {
    std::size_t const parts = workload.mix.parts();
    for (std::size_t iteration = 0; iteration != workload.transactions; ++iteration)
        write_transaction(slot, collection, log.subspan(iteration * parts, parts), workload.mix, workload.max_key);
}

/** Keeps, per key, the committed write with the highest stamp, and the later-logged one
 *  among equal stamps. */
void replay_round(std::span<operation_t const> operations, std::span<operation_t const*> latest) {
    std::fill(latest.begin(), latest.end(), nullptr);
    for (operation_t const& operation : operations) {
        if (!operation.committed) continue;
        operation_t const* const winner = latest[operation.key];
        if (!winner || winner->stamp <= operation.stamp) latest[operation.key] = &operation;
    }
}

/** Whether the replay leaves the key of @p winner inserted. */
bool leaves_inserted(operation_t const* winner) noexcept {
    return winner && winner->code == operation_code_t::insert_k;
}

/** Checks that @p retrieved holds the payload @p operation inserted. */
void expect_payload(value_view_t retrieved, operation_t const& operation) {
    if (!expect_equal(retrieved.size(), sizeof(payload_t), "for key {}", operation.key)) return;
    payload_t stored = 0;
    std::memcpy(&stored, retrieved.data(), sizeof(payload_t));
    expect_equal(stored, operation.value, "for key {}", operation.key);
}

/** Checks the database against the replay: every inserted key holds its payload, and no other
 *  key is present. */
void verify_round(linearizability_run_t& run, std::span<operation_t const*> latest) {
    std::size_t present_count = 0;
    for (operation_t const* const winner : latest)
        if (leaves_inserted(winner)) run.present_keys[present_count++] = key_from_integer(winner->key);

    ustore_key_t const* const keys = run.present_keys.data();
    auto maybe_retrieved = run.collection[strided(keys, keys + present_count)].value(run.arena);
    if (!expect_true(maybe_retrieved.has_value())) return;

    std::size_t index = 0;
    for (operation_t const* const winner : latest)
        if (leaves_inserted(winner)) expect_payload((*maybe_retrieved)[index++], *winner);
    expect_true(run.collection.pairs().size() == present_count);
}

/** Runs one round: threads commit their transactions, then the main thread replays their logs,
 *  checks and clears. */
void run_round(linearizability_run_t& run, executor_t& executor, workload_t const& workload) {
    std::size_t const per_thread = workload.operations_per_thread();
    std::span<operation_t> const operations {run.operations.data(), workload.threads_count * per_thread};
    std::span<writer_slot_t> const slots {run.slots.data(), workload.threads_count};
    auto writer = [&](std::size_t thread) noexcept {
        write_round(slots[thread], run.collection, operations.subspan(thread * per_thread, per_thread), workload);
    };
    expect_true(st::succeeded(executor.for_threads(writer)));

    std::span<operation_t const*> const latest {run.latest.data(), static_cast<std::size_t>(workload.max_key) + 1};
    replay_round(operations, latest);
    verify_round(run, latest);
    expect_true(run.database.on().clear(ustore_drop_pairs_k).succeeded());
}

#pragma endregion Round

#pragma region Configurations

/**
 *  @brief Checks that the state after each round of concurrent random writes matches their replay
 *      in commit order.
 *
 *  Every round ends at a checkpoint where all threads stop, the main thread replays the
 *  committed writes in stamp order, compares the replay against the database, and clears it for
 *  the next round, which does not start after one failed a check.
 *
 *  @param[in,out] run Database, logs and thread slots the run allocated once.
 *  @param[in,out] executor Pool of exactly the workload's thread count.
 *  @param[in] workload Mix, threads and transactions of every round.
 *  @param[in] checkpoints Most rounds to run.
 *  @param[in] slice Time after which no further round starts; the first round always runs.
 */
void linear_writes(linearizability_run_t& run, executor_t& executor, workload_t const& workload,
                   std::size_t checkpoints, std::chrono::nanoseconds slice) {
    auto const deadline = std::chrono::steady_clock::now() + slice;
    std::size_t const failed_before = test_run.failed_checks.load();
    for (std::size_t checkpoint = 0; checkpoint != checkpoints && test_run.failed_checks.load() == failed_before;
         ++checkpoint) {
        if (checkpoint != 0 && std::chrono::steady_clock::now() >= deadline) break;
        run_round(run, executor, workload);
    }
}

/** Runs every mix of inserts and removes at @p threads threads committing
 *  @p transactions per round. */
void linear_writes_mixes(linearizability_run_t& run, executor_t& executor, linearizability_options_t const& options,
                         std::size_t threads, std::size_t transactions, std::chrono::nanoseconds slice) {
    for (operations_mix_t const mix : mixes_k) {
        std::size_t const writes = mix.parts() * transactions * threads;
        std::uint64_t const max_key = fu::max_of<std::uint64_t>(writes / options.writes_per_key.value, 1);
        workload_t const workload {mix, threads, transactions, max_key};
        linear_writes(run, executor, workload, options.checkpoints.value, slice);
    }
}

/** Runs every mix at @p threads threads committing @p transactions per round on @p executor , null
 *  when the pool would not start, over a freshly opened database whose writers draw from the
 *  streams of this case. */
void test_linear_writes(linearizability_run_t& run, linearizability_options_t const& options, executor_t* executor,
                        std::size_t threads, std::size_t transactions, std::chrono::nanoseconds slice) {
    if (!expect_true(executor != nullptr) || !reopen_run(run)) return;
    for (std::size_t index = 0; index != run.slots.size(); ++index)
        run.slots[index].random_generator = split_mix_generator_t {case_stream_key(index)};
    linear_writes_mixes(run, *executor, options, threads, transactions, slice);
}

/** Runs every transaction count on one pool of @p threads threads, each as a case of its own. */
void linear_writes_on_threads(linearizability_run_t& run, linearizability_options_t const& options, std::size_t threads,
                              std::chrono::nanoseconds slice) {
    auto maybe_executor = executor_t::make(threads, caller_exclusivity_t::caller_inclusive_k);
    executor_t* const executor = maybe_executor ? &*maybe_executor : nullptr;
    for (std::size_t const transactions : options.transactions.view()) {
        char label[96];
        auto const written = fmt::format_to_n(label, sizeof(label) - 1, "test_linear_writes({}, {})", threads,
                                              transactions);
        *written.out = '\0';
        run_test_case(label, test_linear_writes, run, options, executor, threads, transactions, slice);
    }
}

#pragma endregion Configurations

/** Reads the counts setting @p variable or @p flag into @p counts , exiting on one that
 *  does not parse. */
void read_counts_setting(int argc, char** argv, char const* variable, std::string_view flag, counts_t& counts) {
    char const* text = setting_text(argc, argv, variable, flag);
    if (text && parse_counts(text, counts) != argument_status_t::parsed_k)
        exit_unparsed(variable, text, "comma-separated counts like 2,4,8");
}

/** Reads the count setting @p variable or @p flag into @p count , within its bounds, exiting on one
 *  that does not parse. */
void read_bounded_setting(int argc, char** argv, char const* variable, std::string_view flag, count_t& count) {
    char const* text = setting_text(argc, argv, variable, flag);
    if (text && parse_count(text, count) != argument_status_t::parsed_k)
        exit_unparsed(variable, text, "a positive count");
}

int main(int argc, char** argv) {
    start_test_run(argc, argv);
    linearizability_options_t options;
    options.time_limit = read_duration_setting(argc, argv, "USTORE_TIME_LIMIT", "--time-limit", options.time_limit);
    read_bounded_setting(argc, argv, "USTORE_CHECKPOINTS", "--checkpoints", options.checkpoints);
    read_counts_setting(argc, argv, "USTORE_THREAD_COUNTS", "--thread-counts", options.thread_counts);
    read_counts_setting(argc, argv, "USTORE_TRANSACTIONS", "--transactions", options.transactions);
    read_bounded_setting(argc, argv, "USTORE_WRITES_PER_KEY", "--writes-per-key", options.writes_per_key);

    auto const maybe_directory = scratch_directory_t::make(test_run.settings.directory);
    if (!maybe_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    config_t const config {test_run.settings.engine, maybe_directory->path()};
    linearizability_run_t run {&config};
    auto [database, open_status] = open_database<database_t>(*run.config);
    if (open_status.failed()) {
        fmt::println(stderr, "Cannot open the database");
        return 1;
    }
    run.database = std::move(database);
    if (!run.database.on().supports_transactions()) {
        run_test_case("test_linear_writes", [] { return case_result_t::skipped(needs_transactions_k); });
        return finish_test_run();
    }
    // The setup runs outside any case, so a filter never leaves it out, and a check it fails
    // fails the run
    if (!prepare_run(run, options)) return finish_test_run();

    std::size_t const configurations_count = options.thread_counts.size * options.transactions.size * mixes_k.size();
    std::chrono::nanoseconds const slice = options.time_limit / configurations_count;
    for (std::size_t const threads : options.thread_counts.view())
        linear_writes_on_threads(run, options, threads, slice);
    return finish_test_run();
}
