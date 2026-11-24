/**
 *  @file test/abi/stress_atomicity.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Stress test that concurrent batched transactions land whole or not at all.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstdio>  // `stderr`

#include <algorithm> // `std::ranges::max`
#include <chrono>    // `std::chrono::steady_clock`
#include <span>      // `std::span`
#include <utility>   // `std::move`

#include <fmt/format.h>

#include <smashtable/basic_vector.hpp>

#include "ustore/ustore.hpp"

#include "ustore/utilities/ram_device.hpp" // `caller_exclusivity_t`, `ram_executor_t`

#include "harness.hpp" // `run_test_case`, `expect_true`, `split_mix_generator_t`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum;

/** Workload of the suite, each field set by the setting of the same name. */
struct atomicity_options_t {

    /** Wall-clock bound of the whole run, shared evenly by every configuration. */
    std::chrono::milliseconds time_limit = std::chrono::seconds {10};

    /** Most write-and-verify rounds one configuration runs. */
    count_t checkpoints {.value = 1'000, .minimum = 1};

    /** Competing thread counts, each run against every batch size. */
    counts_t thread_counts {.values = {4, 8, 13, 16}, .size = 4, .minimum = 1};

    /** Consecutive keys one transaction changes. */
    counts_t batch_sizes {.values = {10, 100, 150}, .size = 3, .minimum = 1};

    /** Keys one round writes, split into consecutive batches. */
    count_t keys {100'000};

    /** One batch in this many is erased rather than overwritten by the deleting case. */
    count_t deletes_periodicity {3};
};

/** Whether a transaction overwrites its batch or erases it. */
enum class batch_operation_t : bool {
    overwrite_k,
    erase_k,
};

/** Whether a configuration ever erases a batch instead of overwriting it. */
enum class round_deletes_t : bool {
    without_deletes_k,
    with_deletes_k,
};

/** What one writing thread keeps across rounds, so a round allocates nothing and shares nothing. */
struct writer_slot_t {

    /** Transaction reset before every attempt, reusing the engine's buffers. */
    transaction_t transaction;

    /** Generator choosing which batches to erase, seeded once per case. */
    split_mix_generator_t random_generator {0};

    writer_slot_t() noexcept = default;
};

/** What a run allocates once and every configuration shares. */
struct atomicity_run_t {

    /** Database opened once for the whole run. */
    database_t& database;

    /** Collection every batch lands in and every verification reads. */
    blobs_collection_t collection;

    /** Arena every verification reads into, rewound by each. */
    arena_t arena;

    /** Every key a round writes, in order, so each batch is a slice of them. */
    st::vector<ustore_key_t> keys;

    /** One slot per thread of the largest configuration. */
    st::vector<writer_slot_t> slots;
};

/** The batches of one configuration, laid end to end over the keys of the run. */
struct batches_t {

    /** Keys of every whole batch, in order. */
    std::span<ustore_key_t const> keys;

    /** Keys one transaction changes. */
    std::size_t size = 1;

    /** Competing threads, whose values stay apart. */
    std::size_t threads_count = 1;

    /** Whether the round erases a batch now and then instead of overwriting it. */
    round_deletes_t deletes = round_deletes_t::without_deletes_k;

    /** One batch in this many is erased, read only when @c deletes asks for erases. */
    std::size_t deletes_periodicity = 1;

    /** Number of whole batches. */
    std::size_t count() const noexcept { return keys.size() / size; }

    /** The keys of batch @p index. */
    keys_view_t operator[](std::size_t index) const noexcept {
        ustore_key_t const* const first = keys.data() + index * size;
        return strided(first, first + size);
    }
};

#pragma region Setup

/** Begins the transaction a slot resets before every attempt. */
bool begin_transaction(database_t& database, transaction_t& transaction) {
    auto [begun, begin_status] = database.transaction();
    if (!expect_true(!begin_status.failed())) return false;
    transaction = std::move(begun);
    return true;
}

/** Makes the arena of @p run, fills its keys, and begins one transaction per thread when the
 *  engine has them. */
bool prepare_run(atomicity_run_t& run, atomicity_options_t const& options) {
    auto [arena, arena_status] = arena_t::make();
    run.arena = std::move(arena);
    if (!expect_true(!arena_status.failed()) || !expect_true(st::succeeded(run.keys.resize(options.keys.value))) ||
        !expect_true(st::succeeded(run.slots.resize((std::ranges::max)(options.thread_counts.view())))))
        return false;
    for (std::size_t index = 0; index != run.keys.size(); ++index) run.keys[index] = key_from_integer(index);
    if (!probed(run.database.supports_transactions())) return true;
    for (writer_slot_t& slot : run.slots)
        if (!begin_transaction(run.database, slot.transaction)) return false;
    return true;
}

#pragma endregion Setup

#pragma region Round

/** Commits one batch, retrying staging and conflicts until it lands, and fails only when
 *  no transaction begins. */
bool commit_batch(transaction_t& transaction, ustore_collection_id_t target, keys_view_t keys, value_view_t value,
                  batch_operation_t operation) {
    while (true) {
        if (!expect_true(!transaction.reset().status.failed())) return false;
        blobs_collection_t collection {transaction.receiver(), target};
        bool const staged = operation == batch_operation_t::erase_k ? !collection[keys].remove().failed()
                                                                    : !collection[keys].assign(value).failed();
        if (!staged) continue;
        if (!transaction.commit().status.failed()) return true;
    }
}

/** Writes every batch once from @p thread, stopping at its first failure. */
void write_round(writer_slot_t& slot, ustore_collection_id_t collection, batches_t const& batches, std::size_t thread) {
    for (std::size_t batch = 0; batch != batches.count(); ++batch) {
        std::uint64_t const number = batch * batches.threads_count + thread;
        value_view_t const value(reinterpret_cast<util::byte_t const*>(&number), sizeof(number));
        bool const erases = batches.deletes == round_deletes_t::with_deletes_k &&
                            slot.random_generator.below(batches.deletes_periodicity) == 0;
        batch_operation_t const operation = erases ? batch_operation_t::erase_k : batch_operation_t::overwrite_k;
        if (!commit_batch(slot.transaction, collection, batches[batch], value, operation)) return;
    }
}

/** Checks that one batch holds one value in all its keys, reading them into @p arena. */
void verify_batch(blobs_collection_t& collection, arena_t& arena, keys_view_t keys) {
    auto const [retrieved, retrieve_status] = collection[keys].value(arena);
    if (!expect_true(!retrieve_status.failed())) return;
    for (std::size_t offset = 1; offset != keys.size(); ++offset) expect_true(retrieved[0] == retrieved[offset]);
}

/** Runs one round: every thread writes every batch, then the main thread checks what they left
 *  and clears it. */
void run_round(atomicity_run_t& run, util::ram_executor_t& executor, batches_t const& batches) {
    std::span<writer_slot_t> const slots {run.slots.data(), batches.threads_count};
    auto writer = [&](std::size_t thread) noexcept {
        write_round(slots[thread], run.collection.handle(), batches, thread);
    };
    expect_true(st::succeeded(executor.for_threads(writer)));
    for (std::size_t batch = 0; batch != batches.count(); ++batch)
        verify_batch(run.collection, run.arena, batches[batch]);
    expect_true(!run.database.clear(ustore_drop_pairs_k).failed());
}

#pragma endregion Round

#pragma region Configurations

/**
 *  @brief Checks that batches written, or periodically erased, by competing threads hold one value
 *      in all their keys.
 *
 *  Each thread picks its own value per batch, so once all threads finish a round every batch must
 *  hold one value in all of its keys, whichever thread won it. Every round ends by clearing the
 *  database, so the next starts empty, and no round starts after one failed a check.
 *
 *  @param[in,out] run Database, keys and thread slots the run allocated once.
 *  @param[in,out] executor Pool of exactly the configuration's thread count.
 *  @param[in] batches Batches every round writes.
 *  @param[in] checkpoints Most rounds to run.
 *  @param[in] slice Time after which no further round starts; the first round always runs.
 */
void insert_atomic_isolated(atomicity_run_t& run, util::ram_executor_t& executor, batches_t const& batches,
                            std::size_t checkpoints, std::chrono::nanoseconds slice) {
    auto const deadline = std::chrono::steady_clock::now() + slice;
    std::size_t const failed_before = test_run.failed_checks.load();
    for (std::size_t checkpoint = 0; checkpoint != checkpoints && test_run.failed_checks.load() == failed_before;
         ++checkpoint) {
        if (checkpoint != 0 && std::chrono::steady_clock::now() >= deadline) break;
        run_round(run, executor, batches);
    }
}

/** Runs every batch size on one pool of @p threads threads. */
void inserts_on_threads(atomicity_run_t& run, atomicity_options_t const& options, std::size_t threads,
                        round_deletes_t deletes, std::chrono::nanoseconds slice) {
    auto [executor, executor_status] = util::ram_executor_t::make(threads,
                                                                  util::caller_exclusivity_t::caller_inclusive_k);
    if (!expect_true(!st::failed(executor_status))) return;
    for (std::size_t const batch_size : options.batch_sizes.view()) {
        std::span<ustore_key_t const> const keys {run.keys.data(), run.keys.size() / batch_size * batch_size};
        batches_t const batches {keys, batch_size, threads, deletes, options.deletes_periodicity.value};
        insert_atomic_isolated(run, executor, batches, options.checkpoints.value, slice);
    }
}

/** Runs every thread count against every batch size, erasing batches when @p deletes
 *  asks for it. */
case_result_t test_database_inserts(atomicity_run_t& run, atomicity_options_t const& options, round_deletes_t deletes) {
    if (!probed(run.database.supports_transactions())) return case_result_t::skipped(needs_transactions_k);
    for (std::size_t index = 0; index != run.slots.size(); ++index)
        run.slots[index].random_generator = split_mix_generator_t {case_stream_key(index)};
    std::size_t const configurations_count = 2 * options.thread_counts.size * options.batch_sizes.size;
    std::chrono::nanoseconds const slice = options.time_limit / configurations_count;
    for (std::size_t const threads : options.thread_counts.view())
        inserts_on_threads(run, options, threads, deletes, slice);
    return {};
}

#pragma endregion Configurations

/** Reads the counts setting @p variable or @p flag into @p counts , exiting on one that
 *  does not parse. */
void read_counts_setting(int argc, char** argv, char const* variable, std::string_view flag, counts_t& counts) {
    char const* text = setting_text(argc, argv, variable, flag);
    if (text && parse_counts(text, counts) != argument_status_t::parsed_k)
        exit_unparsed(variable, text, "comma-separated counts like 4,8,13,16");
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
    atomicity_options_t options;
    options.time_limit = read_duration_setting(argc, argv, "USTORE_TIME_LIMIT", "--time-limit", options.time_limit);
    read_bounded_setting(argc, argv, "USTORE_CHECKPOINTS", "--checkpoints", options.checkpoints);
    read_counts_setting(argc, argv, "USTORE_THREAD_COUNTS", "--thread-counts", options.thread_counts);
    read_counts_setting(argc, argv, "USTORE_BATCH_SIZES", "--batch-sizes", options.batch_sizes);
    read_bounded_setting(argc, argv, "USTORE_KEYS", "--keys", options.keys);
    read_bounded_setting(argc, argv, "USTORE_DELETES_PERIODICITY", "--deletes-periodicity",
                         options.deletes_periodicity);

    auto [directory, directory_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(directory_status)) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    config_t const config {test_run.settings.engine, directory.path()};
    auto [database, open_status] = open_database<database_t>(config);
    if (open_status.failed()) {
        fmt::println(stderr, "Cannot open the database");
        return 1;
    }

    auto [collection, create_status] = database.find_or_create("atomicity");
    if (create_status.failed()) {
        fmt::println(stderr, "Cannot create the collection");
        return 1;
    }

    // The setup runs outside any case, so a filter never leaves it out, and a check it fails
    // fails the run
    atomicity_run_t run {database, collection};
    if (!prepare_run(run, options)) return finish_test_run();

    run_test_case(engine_label_t {"test_database_inserts", "round_deletes_t::without_deletes_k"}.c_str(),
                  test_database_inserts, run, options, round_deletes_t::without_deletes_k);
    run_test_case(engine_label_t {"test_database_inserts", "round_deletes_t::with_deletes_k"}.c_str(),
                  test_database_inserts, run, options, round_deletes_t::with_deletes_k);
    return finish_test_run();
}
