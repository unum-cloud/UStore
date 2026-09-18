/**
 *  @file tests/stress_atomicity.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Stress test that concurrent batched transactions land whole or not at all.
 */

#include <cstddef>      // `std::size_t`
#include <cstdint>      // `std::uint32_t`, `std::uint64_t`
#include <cstdio>       // `stderr`
#include <sys/random.h> // `getentropy`

#include <algorithm>   // `std::ranges::max`
#include <array>       // `std::array`
#include <chrono>      // `std::chrono::steady_clock`
#include <numeric>     // `std::iota`
#include <random>      // `std::mt19937`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::move`

#include <fmt/format.h>

#include <smashtable/basic_vector.hpp>

#include <ustore/ustore.hpp>

#include "executor.hpp"
#include "test.hpp"

using namespace unum::ustore;
using namespace unum;

/** Workload of the suite, each field set by the flag of the same name. */
struct atomicity_options_t {

    /** Wall-clock bound of the whole run, shared evenly by every configuration. */
    std::chrono::nanoseconds duration = std::chrono::seconds {10};

    /** Most write-and-verify rounds one configuration runs. */
    count_t checkpoints {.value = 1'000, .minimum = 1};

    /** Competing thread counts, each run against every batch size. */
    counts_t threads {.values = {4, 8, 13, 16}, .size = 4, .minimum = 1};

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

    /** Generator choosing which batches to erase, seeded once per run. */
    std::mt19937 random_generator;

    /** Failures the thread counted this round, folded in by the main thread once the round ends. */
    std::size_t failures = 0;

    writer_slot_t() noexcept = default;
};

/** What a run allocates once and every configuration shares. */
struct atomicity_run_t {

    /** Database opened once for the whole run. */
    database_t& database;

    /** Main collection, whose arena every verification reuses. */
    blobs_collection_t collection;

    /** Every key a round writes, in order, so each batch is a slice of them. */
    st::basic_vector<ustore_key_t> keys;

    /** One slot per thread of the largest configuration. */
    st::basic_vector<writer_slot_t> slots;
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
[[nodiscard]] std::size_t begin_transaction(database_t& database, transaction_t& transaction) {
    auto maybe_transaction = database.transact();
    std::size_t const failures = expect(maybe_transaction);
    if (failures) return failures;
    transaction = *std::move(maybe_transaction);
    return 0;
}

/** Fills the keys of @p run, seeds one generator per thread, and begins their transactions when the engine has them. */
[[nodiscard]] std::size_t prepare_run(atomicity_run_t& run, atomicity_options_t const& options) {
    std::size_t failures = expect(st::succeeded(run.keys.resize(options.keys.value)));
    failures += expect(st::succeeded(run.slots.resize(std::ranges::max(options.threads.view()))));
    std::iota(run.keys.begin(), run.keys.end(), ustore_key_t {0});
    for (writer_slot_t& slot : run.slots) {
        std::uint32_t seed = 0;
        failures += expect(getentropy(&seed, sizeof(seed)) == 0);
        slot.random_generator.seed(seed);
    }
    if (failures || !run.database.supports_transactions()) return failures;
    for (std::size_t thread = 0; thread != run.slots.size() && !failures; ++thread)
        failures += begin_transaction(run.database, run.slots[thread].transaction);
    return failures;
}

#pragma endregion Setup

#pragma region Round

/** Commits one batch, retrying staging and conflicts until it lands, and fails only when no transaction begins. */
std::size_t commit_batch(transaction_t& transaction, keys_view_t keys, value_view_t value,
                         batch_operation_t operation) {
    while (true) {
        std::size_t const failures = expect(transaction.reset().succeeded());
        if (failures) return failures;
        blobs_collection_t collection = transaction.main();
        bool const staged = operation == batch_operation_t::erase_k ? collection[keys].erase().succeeded()
                                                                    : collection[keys].assign(value).succeeded();
        if (!staged) continue;
        if (transaction.commit().succeeded()) return 0;
    }
}

/** Writes every batch once from @p thread, stopping at its first failure. */
void write_round(writer_slot_t& slot, batches_t const& batches, std::size_t thread) {
    for (std::size_t batch = 0; batch != batches.count() && !slot.failures; ++batch) {
        std::uint64_t const number = batch * batches.threads_count + thread;
        value_view_t const value(reinterpret_cast<byte_t const*>(&number), sizeof(number));
        bool const erases = batches.deletes == round_deletes_t::with_deletes_k &&
                            slot.random_generator() % batches.deletes_periodicity == 0;
        batch_operation_t const operation = erases ? batch_operation_t::erase_k : batch_operation_t::overwrite_k;
        slot.failures += commit_batch(slot.transaction, batches[batch], value, operation);
    }
}

/** Checks that one batch holds one value in all its keys. */
std::size_t verify_batch(blobs_collection_t& collection, keys_view_t keys) {
    auto maybe_retrieved = collection[keys].value();
    std::size_t failures = expect(maybe_retrieved);
    if (!maybe_retrieved) return failures;
    auto const& retrieved = *maybe_retrieved;
    for (std::size_t offset = 1; offset != keys.size(); ++offset) failures += expect(retrieved[0] == retrieved[offset]);
    return failures;
}

/** Runs one round: every thread writes every batch, then the main thread checks what they left and clears it. */
std::size_t run_round(atomicity_run_t& run, executor_t& executor, batches_t const& batches) {
    std::span<writer_slot_t> const slots {run.slots.data(), batches.threads_count};
    auto writer = [&](std::size_t thread) noexcept { write_round(slots[thread], batches, thread); };
    std::size_t failures = expect(st::succeeded(executor.for_threads(writer)));
    for (writer_slot_t& slot : slots) failures += std::exchange(slot.failures, 0);
    for (std::size_t batch = 0; batch != batches.count(); ++batch)
        failures += verify_batch(run.collection, batches[batch]);
    failures += expect(run.database.clear().succeeded());
    return failures;
}

#pragma endregion Round

#pragma region Configurations

/**
 *  @brief Checks that batches written, or periodically erased, by competing threads hold one value in all their keys.
 *
 *  Each thread picks its own value per batch, so once all threads finish a round every batch must hold one value
 *  in all of its keys, whichever thread won it. Every round ends by clearing the database, so the next starts empty.
 *
 *  @param[inout] run Database, keys and thread slots the run allocated once.
 *  @param[inout] executor Pool of exactly the configuration's thread count.
 *  @param[in] batches Batches every round writes.
 *  @param[in] checkpoints Most rounds to run.
 *  @param[in] slice Time after which no further round starts; the first round always runs.
 */
std::size_t insert_atomic_isolated(atomicity_run_t& run, executor_t& executor, batches_t const& batches,
                                   std::size_t checkpoints, std::chrono::nanoseconds slice) {
    auto const deadline = std::chrono::steady_clock::now() + slice;
    std::size_t failures = 0;
    for (std::size_t checkpoint = 0; checkpoint != checkpoints && !failures; ++checkpoint) {
        if (checkpoint != 0 && std::chrono::steady_clock::now() >= deadline) break;
        failures += run_round(run, executor, batches);
    }
    return failures;
}

/** Runs every batch size on one pool of @p threads threads. */
std::size_t inserts_on_threads(atomicity_run_t& run, atomicity_options_t const& options, std::size_t threads,
                               round_deletes_t deletes, std::chrono::nanoseconds slice) {
    auto maybe_executor = executor_t::make(threads);
    std::size_t failures = expect(maybe_executor);
    if (!maybe_executor) return failures;
    for (std::size_t const batch_size : options.batch_sizes.view()) {
        std::span<ustore_key_t const> const keys {run.keys.data(), run.keys.size() / batch_size * batch_size};
        batches_t const batches {keys, batch_size, threads, deletes, options.deletes_periodicity.value};
        failures += insert_atomic_isolated(run, *maybe_executor, batches, options.checkpoints.value, slice);
    }
    return failures;
}

/** Runs every thread count against every batch size, erasing batches when @p deletes asks for it. */
std::size_t database_inserts(atomicity_run_t& run, atomicity_options_t const& options, round_deletes_t deletes) {
    if (!run.database.supports_transactions()) return 0;
    std::size_t const configurations_count = 2 * options.threads.size * options.batch_sizes.size;
    std::chrono::nanoseconds const slice = options.duration / configurations_count;
    std::size_t failures = 0;
    for (std::size_t const threads : options.threads.view())
        failures += inserts_on_threads(run, options, threads, deletes, slice);
    return failures;
}

#pragma endregion Configurations

int main(int argc, char** argv) {
    std::string_view parent;
    atomicity_options_t options;
    std::array<flag_t, 7> const flags {{
        directory_flag(parent),
        {"--duration", "duration", "bound of the whole run, like 10s", &options.duration},
        {"--checkpoints", "count", "most write-and-verify rounds per configuration", &options.checkpoints},
        {"--threads", "counts", "competing thread counts, like 4,8,13,16", &options.threads},
        {"--batch-sizes", "counts", "keys per transaction, like 10,100,150", &options.batch_sizes},
        {"--keys", "count", "keys one round writes", &options.keys},
        {"--deletes-periodicity", "count", "one batch in this many is erased", &options.deletes_periodicity},
    }};
    if (argument_status_t const status = parse_test_arguments(argc, argv, flags); status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;

    auto const maybe_directory = test_directory_t::make(parent);
    if (!maybe_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", parent);
        return 1;
    }
    test_config_t const config {maybe_directory->path()};
    database_t database;
    if (database.open(config.c_str()).failed()) {
        fmt::println(stderr, "Cannot open the database");
        return 1;
    }

    atomicity_run_t run {database, database.main()};
    if (std::size_t const failures = prepare_run(run, options); failures) {
        report("setup", failures);
        return 1;
    }

    std::size_t total = 0;
    total += report("database.inserts", database_inserts(run, options, round_deletes_t::without_deletes_k));
    total += report("database.inserts_and_deletes", database_inserts(run, options, round_deletes_t::with_deletes_k));
    return report("total", total) ? 1 : 0;
}
