/**
 *  @file test/nvme/single_collection_database_stress.cpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Stress tests running concurrent selects, inserts and removes on a UStoreNVMe single-collection database.
 *
 *  Several threads hammer one database at once, each reading anywhere while it writes and removes only the keys it
 *  owns, so every thread keeps an exact model of its own keys. Every key is written and flushed into a run first, so
 *  the threads read through the memory runs and the runs alike, and once they are joined every key must answer its
 *  owner's model, before and after the last flush. The memory run bounds are small, so flushes publish new runs under
 *  the readers all along, from whichever thread holds the write path.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <array>   // `std::array`
#include <memory>  // `std::unique_ptr`
#include <new>     // `std::nothrow`
#include <span>    // `std::span`
#include <thread>  // `std::thread`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_single_collection_database.hpp" // `basic_single_collection_database`
#include "ustore/blobs/nvme_key_slot.hpp"                   // `key_slot`

#include "nvme/fixture.hpp" // `run_on_transport`, `expectation_t`, `painted`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Shape

/** Threads every case drives the database on at once. */
constexpr std::size_t worker_threads_k = 4;

/** The numbers every case mounts with: three channels for the stack and one for every worker thread. */
constexpr fixture_config_t stress_config_k {.shared_io_channels = 3 + worker_threads_k + 1};

/** Keys every worker owns, writing and removing them alone. */
constexpr std::size_t keys_per_worker_k = 256;

/** Operations every worker runs, about a third each of selects, inserts and removes. */
constexpr std::size_t operations_per_worker_k = 3000;

/** The seed every worker's generator is forked from. */
constexpr std::uint64_t seed_k = 42;

/** The keys every case of this suite drives. */
using stored_key_t = std::uint64_t;

/** A fixture, the channels a database needs, and the volume over them. */
template <typename fixture_type_>
struct stress_stack {
    using fixture_t = fixture_type_;
    using guard_t = typename fixture_t::guard_t;
    using volume_t = typename fixture_t::volume_t;

    /** The disk, the arena and the channels. */
    std::unique_ptr<fixture_t> fixture;

    /** The channel the database writes, flushes and commits on. */
    guard_t writing;

    /** The second channel a compaction writes its new runs on. */
    guard_t merging;

    /** The channel the calling thread reads on. */
    guard_t reading;

    /** The volume every run and blob lands on, pinned so a database may point at it. */
    std::unique_ptr<volume_t> volume;

    /** The channel of the calling thread, which every read of this stack takes. */
    [[nodiscard]] io_channel_t& channel() noexcept { return reading.channel(); }
};

/**
 *  @brief The database shape every case opens, whose small memory runs flush many times under the threads.
 *  @param[in] value_size Bytes every value takes, which is zero for every layout but the fixed one.
 *  @param[in] value_max_size Bytes the largest value takes, which is zero for a fixed-width layout.
 */
[[nodiscard]] single_collection_database_config_t stress_config(stored_value_size_t value_size,
                                                                stored_value_size_t value_max_size) noexcept {
    single_collection_database_config_t config;
    config.collection.concurrency_limit = worker_threads_k;
    config.collection.cache_limit = 1024ull * 1024ull;
    config.collection.memory_runs_max_bytes = 256ull * 1024ull;
    config.collection.memory_runs_max_deltas = 512;
    config.collection.lsm_tree.level_enlarge_factor = 4;
    config.collection.lsm_tree.lsm_level_0.sorted_run.value_size = value_size;
    config.collection.lsm_tree.lsm_level_0.sorted_run.value_max_size = value_max_size;
    config.default_transaction.max_bytes = 256ull * 1024ull;
    config.default_transaction.max_deltas = 512;
    return config;
}

#pragma endregion Shape

#pragma region Fixtures

/** Mounts a fixture through @p mount, takes the three channels a case needs and formats one volume over them. */
template <typename mount_type_>
expected<std::unique_ptr<stress_stack<typename mount_type_::fixture_t>>> make_stack(mount_type_ const& mount) noexcept {
    using stack_t = stress_stack<typename mount_type_::fixture_t>;
    using volume_t = typename stack_t::volume_t;
    auto stack = std::unique_ptr<stack_t> {new (std::nothrow) stack_t {}};
    if (!stack) return status_t {status_code_t::out_of_memory_k, "The stack did not fit"};

    auto [fixture, mount_status] = mount();
    if (mount_status.failed()) return mount_status;
    stack->fixture = std::move(fixture);

    auto [writing, acquire_writing_status] = stack->fixture->channels.acquire();
    if (acquire_writing_status.failed()) return acquire_writing_status;
    stack->writing = std::move(writing);
    auto [merging, acquire_merging_status] = stack->fixture->channels.acquire();
    if (acquire_merging_status.failed()) return acquire_merging_status;
    stack->merging = std::move(merging);
    auto [reading, acquire_reading_status] = stack->fixture->channels.acquire();
    if (acquire_reading_status.failed()) return acquire_reading_status;
    stack->reading = std::move(reading);

    expected<volume_t> volume = stack->fixture->mount_volume(stack->writing.channel());
    if (!volume) return volume.status();
    stack->volume = std::unique_ptr<volume_t> {new (std::nothrow) volume_t {std::move(*volume)}};
    if (!stack->volume) return status_t {status_code_t::out_of_memory_k, "The volume did not fit"};
    return stack;
}

#pragma endregion Fixtures

#pragma region Model

/** Reads every key of @p wanted through one batched select and counts what the model disagrees with. */
template <typename database_type_>
[[nodiscard]] std::size_t check_model(database_type_ const& store, io_channel_t& channel,
                                      std::span<expectation_t const> wanted) {
    std::size_t failures = 0;
    std::size_t const count = wanted.size();
    st::vector<stored_key_t> keys;
    st::vector<lookup_result_t> results;
    st::vector<std::uint64_t> seen;
    failures += expect(keys.resize(count) == st::status_t::success_k);
    failures += expect(results.resize(count) == st::status_t::success_k);
    failures += expect(seen.resize(count) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count; ++index) {
        keys[index] = wanted[index].stored_key;
        results[index] = lookup_result_t::missing_k;
        seen[index] = 0;
    }

    std::size_t mismatches = 0;
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        seen[request] += bytes.size();
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(wanted[request].seed, offset + index)) ++mismatches;
    };
    failures += expect(
        store.select(channel, std::span<stored_key_t const> {keys}, std::span<lookup_result_t> {results}, sink)
            .succeeded());
    failures += expect_eq(mismatches, std::size_t {0});

    for (std::size_t index = 0; index != count; ++index) {
        // A key never written answers missing, and a removed key answers its remove delta; neither carries a value.
        bool const absent_wanted = wanted[index].state != lookup_result_t::written_k;
        bool const absent_seen = results[index] != lookup_result_t::written_k;
        failures += expect(absent_wanted == absent_seen);
        if (!absent_wanted) failures += expect_eq(seen[index], wanted[index].bytes);
    }
    return failures;
}

#pragma endregion Model

#pragma region Stress Cases

/**
 *  @brief Threads select anywhere and insert and remove their own keys at once, and every key ends as its owner left
 *      it.
 *  @param[in] value_size Bytes every value takes, which is zero for every layout but the fixed one.
 *  @param[in] value_max_size Bytes the largest value takes, which is zero for a fixed-width layout.
 */
template <typename key_slot_type_, typename mount_type_>
[[nodiscard]] std::size_t concurrent_operations_keep_their_values(mount_type_ const& mount,
                                                                  stored_value_size_t value_size,
                                                                  stored_value_size_t value_max_size) {
    using stack_t = stress_stack<typename mount_type_::fixture_t>;
    using guard_t = typename stack_t::guard_t;
    using volume_t = typename stack_t::volume_t;
    using database_t = basic_single_collection_database<
        typename key_slot_type_::stored_key_t, volume_t, atomic_reference, waiter_t,
        nvme_store<key_slot_type_, volume_t, atomic_reference, waiter_t, last_write_fold_t, no_device_fold_t>>;
    std::size_t failures = 0;

    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    expected<typename database_t::database_unique_ptr_t> store = database_t::open(
        *(*stack)->volume, (*stack)->writing.channel(), (*stack)->merging.channel(),
        stress_config(value_size, value_max_size), (*stack)->fixture->resources);
    failures += expect(store.has_value());
    if (!store) return failures;
    stored_value_size_t const widest = (*store)->value_max_size();

    std::array<st::vector<expectation_t>, worker_threads_k> models;
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker) {
        failures += expect(models[worker].resize(keys_per_worker_k) == st::status_t::success_k);
        if (failures != 0) return failures;
        for (std::size_t index = 0; index != keys_per_worker_k; ++index)
            models[worker][index].stored_key = static_cast<stored_key_t>(worker * keys_per_worker_k + index);
    }

    // Every key starts written and flushed, so the reads below reach the runs as well as the memory runs.
    st::vector<byte_t> first;
    failures += expect(first.resize(widest) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker)
        for (expectation_t& entry : models[worker]) {
            entry.state = lookup_result_t::written_k;
            entry.bytes = value_size != 0 ? value_size : 1 + entry.stored_key % widest;
            entry.seed = entry.stored_key + widest;
            std::span<byte_t> const painted_value {first.data(), static_cast<std::size_t>(entry.bytes)};
            paint_into(entry, painted_value);
            failures += expect_status((*store)->insert(entry.stored_key, std::span<byte_t const> {painted_value}),
                                      status_code_t::success_k);
        }
    failures += expect((*store)->flush().succeeded());
    if (failures != 0) return failures;

    std::array<std::size_t, worker_threads_k> worker_failures {};
    std::array<std::thread, worker_threads_k> workers;
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker)
        workers[worker] = std::thread {[&, worker]() noexcept {
            expected<guard_t> guard = (*stack)->fixture->channels.acquire();
            worker_failures[worker] += expect(guard.has_value());
            if (!guard) return;

            random_t random = random_t {seed_k}.fork(worker);
            st::vector<expectation_t>& model = models[worker];
            st::vector<byte_t> value;
            worker_failures[worker] += expect(value.resize(widest) == st::status_t::success_k);
            if (worker_failures[worker] != 0) return;

            for (std::size_t operation = 0; operation != operations_per_worker_k; ++operation) {
                std::uint64_t const choice = random.below(3);
                std::size_t const key_index = static_cast<std::size_t>(random.below(keys_per_worker_k));
                expectation_t& entry = model[key_index];

                if (choice == 0) {
                    // A key of another thread may change under the read, so only its status is checked.
                    stored_key_t const anywhere = random.below(worker_threads_k * keys_per_worker_k);
                    bool const owned = anywhere / keys_per_worker_k == worker;
                    stored_key_t const key = owned ? entry.stored_key : anywhere;
                    stored_value_size_t size = 0;
                    status_t const read = (*store)->select(guard->channel(), key, std::span<byte_t> {value}, size);
                    if (read.succeeded()) {
                        if (owned) {
                            worker_failures[worker] += expect(entry.state == lookup_result_t::written_k);
                            worker_failures[worker] += expect_eq(std::uint64_t {size}, entry.bytes);
                            for (std::size_t position = 0; position != size; ++position)
                                worker_failures[worker] += expect(value[position] == painted(entry.seed, position));
                        }
                    }
                    else {
                        worker_failures[worker] += expect(read == status_code_t::not_found_k);
                        if (owned) worker_failures[worker] += expect(entry.state != lookup_result_t::written_k);
                    }
                }
                else if (choice == 1) {
                    entry.state = lookup_result_t::written_k;
                    entry.bytes = value_size != 0 ? value_size : 1 + random.below(widest);
                    entry.seed = random.next();
                    std::span<byte_t> const painted_value {value.data(), static_cast<std::size_t>(entry.bytes)};
                    paint_into(entry, painted_value);
                    worker_failures[worker] += expect_status(
                        (*store)->insert(entry.stored_key, std::span<byte_t const> {painted_value}),
                        status_code_t::success_k);
                }
                else {
                    entry.state = lookup_result_t::removed_k;
                    entry.bytes = 0;
                    worker_failures[worker] += expect_status((*store)->remove(entry.stored_key),
                                                             status_code_t::success_k);
                }
            }
        }};
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker) workers[worker].join();
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker) failures += worker_failures[worker];

    // Once with the last writes still in memory runs, and once more out of the runs alone.
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker)
        failures += check_model(**store, (*stack)->channel(), std::span<expectation_t const> {models[worker]});
    failures += expect((*store)->flush().succeeded());
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker)
        failures += check_model(**store, (*stack)->channel(), std::span<expectation_t const> {models[worker]});
    return failures;
}

/** Values of up to 12 bytes kept inside the key slots. */
template <typename mount_type_>
std::size_t inline_12_bytes_keeps_its_values(mount_type_ const& mount) {
    using slot_t = key_slot<stored_key_t, value_layout_t::inline_12_bytes_k>;
    return concurrent_operations_keep_their_values<slot_t>(mount, 0, 0);
}

/** Values of one width in the stored values section. */
template <typename mount_type_>
std::size_t fixed_width_keeps_its_values(mount_type_ const& mount) {
    using slot_t = key_slot<stored_key_t, value_layout_t::fixed_width_k>;
    return concurrent_operations_keep_their_values<slot_t>(mount, 55, 0);
}

/** Values of any width up to a bound in the stored values section. */
template <typename mount_type_>
std::size_t variable_width_keeps_its_values(mount_type_ const& mount) {
    using slot_t = key_slot<stored_key_t, value_layout_t::variable_width_k>;
    return concurrent_operations_keep_their_values<slot_t>(mount, 0, 129);
}

#pragma endregion Stress Cases

} // namespace unum::ustore::test

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;

    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    st::expected<directory_t> const directory = directory_t::make(options.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    test_environment_t const environment = test_environment_t::of(options, *directory);

    auto const cases = [&](auto const& mount) {
        report(totals, mount, "inline_12_bytes", inline_12_bytes_keeps_its_values);
        report(totals, mount, "fixed_width", fixed_width_keeps_its_values);
        report(totals, mount, "variable_width", variable_width_keeps_its_values);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, stress_config_k, 1, "scdb_stress", cases);

    return suite_exit_code(totals);
}
