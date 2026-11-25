/**
 *  @file test/nvme/single_collection_database_transaction_stress.cpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Stress tests running concurrent transactions on a UStoreNVMe single-collection database and replaying them in
 *      commit order.
 *
 *  Every commit draws a stamp, and the stamps order the commits as they landed. Writers record what they committed
 *  under its stamp, readers record what a read-only transaction saw under the stamp of its own commit, and once the
 *  threads are joined the records are replayed in stamp order: every read must match the model right before its
 *  stamp, and the database must end as the whole replay does.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <algorithm> // `std::sort`
#include <array>     // `std::array`
#include <memory>    // `std::unique_ptr`
#include <new>       // `std::nothrow`
#include <span>      // `std::span`
#include <thread>    // `std::thread`
#include <utility>   // `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_single_collection_database.hpp" // `basic_single_collection_database`, `transaction_stamp_t`
#include "ustore/blobs/nvme_key_slot.hpp"                   // `key_slot`

#include "nvme/fixture.hpp" // `run_on_transport`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;

#pragma region Shape

/** Threads every case drives the database on at once. */
constexpr std::size_t worker_threads_k = 4;

/** The numbers every case mounts with: two queue threads for the stack's three channels, and one
 *  for every worker, as a thread's queues serve one OS thread at a time. */
constexpr fixture_config_t stress_config_k {.queues_threads = 2 + worker_threads_k};

/** Bytes every value takes: one counter, which names the commit that wrote it. */
constexpr std::size_t value_bytes_k = 8;

/** The keys, and the key slots of eight inline bytes, every case of this suite drives. */
using stored_key_t = std::uint64_t;
using inline_slot_t = key_slot<stored_key_t, value_layout_t::inline_12_bytes_k>;

/** A fixture, the channels a database needs, and the volume over them. */
template <typename fixture_type_>
struct stress_stack {
    using fixture_t = fixture_type_;
    using volume_t = typename fixture_t::volume_t;
    using database_t = basic_single_collection_database<
        stored_key_t, volume_t, atomic_reference, waiter_t,
        nvme_store<inline_slot_t, volume_t, atomic_reference, waiter_t, util::last_write_fold_t, no_device_fold_t>>;

    /** The disk, the arena and the channels. */
    std::unique_ptr<fixture_t> fixture;

    /** The channel the database writes, flushes and commits on. */
    util::io_channel_t* writing = nullptr;

    /** The second channel a compaction writes its new runs on. */
    util::io_channel_t* merging = nullptr;

    /** The channel the calling thread reads on. */
    util::io_channel_t* reading = nullptr;

    /** The volume every run and blob lands on, pinned so a database may point at it. */
    std::unique_ptr<volume_t> volume;

    /** The channel of the calling thread, which every read of this stack takes. */
    [[nodiscard]] util::io_channel_t& channel() noexcept { return *reading; }
};

/** The database shape every case opens, whose small memory runs flush many times under the threads. */
[[nodiscard]] single_collection_database_config_t stress_config() noexcept {
    single_collection_database_config_t config;
    config.collection.concurrency_limit = worker_threads_k;
    config.collection.cache_limit = 1024ull * 1024ull;
    config.collection.memory_runs_max_bytes = 256ull * 1024ull;
    config.collection.memory_runs_max_deltas = 2048;
    config.collection.lsm_tree.level_enlarge_factor = 4;
    config.default_transaction.max_bytes = 64ull * 1024ull;
    config.default_transaction.max_deltas = 1024;
    return config;
}

/** Writes @p counter into @p into, little end first, so a read of it needs no alignment. */
void write_counter(std::uint64_t counter, std::span<util::byte_t> into) noexcept {
    for (std::size_t index = 0; index != value_bytes_k; ++index)
        into[index] = static_cast<util::byte_t>((counter >> (index * 8)) & 0xFFull);
}

/** The counter @p from holds, which is zero for a span shorter than one. */
[[nodiscard]] std::uint64_t read_counter(std::span<util::byte_t const> from) noexcept {
    if (from.size() < value_bytes_k) return 0;
    std::uint64_t counter = 0;
    for (std::size_t index = 0; index != value_bytes_k; ++index)
        counter |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(from[index])) << (index * 8);
    return counter;
}

#pragma endregion Shape

#pragma region Fixtures

/** Mounts a fixture through @p mount, takes the three channels a case needs and opens the database over a volume. */
template <typename mount_type_>
util::expected<std::unique_ptr<stress_stack<typename mount_type_::fixture_t>>> make_stack(
    mount_type_ const& mount) noexcept {
    using stack_t = stress_stack<typename mount_type_::fixture_t>;
    using volume_t = typename stack_t::volume_t;
    auto stack = std::unique_ptr<stack_t> {new (std::nothrow) stack_t {}};
    if (!stack) return util::status_t {util::status_code_t::out_of_memory_k, "The stack did not fit"};

    auto [fixture, mount_status] = mount();
    if (mount_status.failed()) return mount_status;
    stack->fixture = std::move(fixture);

    auto [writing, acquire_writing_status] = stack->fixture->channel(0);
    if (acquire_writing_status.failed()) return acquire_writing_status;
    stack->writing = std::move(writing);
    auto [merging, acquire_merging_status] = stack->fixture->channel(1);
    if (acquire_merging_status.failed()) return acquire_merging_status;
    stack->merging = std::move(merging);
    auto [reading, acquire_reading_status] = stack->fixture->channel(2);
    if (acquire_reading_status.failed()) return acquire_reading_status;
    stack->reading = std::move(reading);

    auto [volume, mount_volume_status] = stack->fixture->mount_volume(*stack->writing);
    if (mount_volume_status.failed()) return mount_volume_status;
    stack->volume = std::unique_ptr<volume_t> {new (std::nothrow) volume_t {std::move(volume)}};
    if (!stack->volume) return util::status_t {util::status_code_t::out_of_memory_k, "The volume did not fit"};
    return stack;
}

/** Opens the database every case drives over the volume and the channels of @p stack. */
template <typename stack_type_>
util::expected<typename stack_type_::database_t::database_unique_ptr_t> open_over(stack_type_& stack) noexcept {
    return stack_type_::database_t::open(*stack.volume, *stack.writing, *stack.merging, stress_config(),
                                         *stack.fixture->resources);
}

/**
 *  @brief Reads @p keys through one batched select of @p reader, the database itself or a transaction over it.
 *  @param[out] present Whether every key holds a value.
 *  @param[out] counters The counter every present key holds.
 */
template <typename reader_type_>
util::status_t read_counters(reader_type_& reader, util::io_channel_t& channel, std::span<stored_key_t const> keys,
                             st::vector<bool>& present, st::vector<std::uint64_t>& counters) noexcept {
    st::vector<lookup_result_t> results;
    st::vector<std::array<util::byte_t, value_bytes_k>> values;
    if (results.resize(keys.size()) != st::status_t::success_k ||
        values.resize(keys.size()) != st::status_t::success_k ||
        present.resize(keys.size()) != st::status_t::success_k ||
        counters.resize(keys.size()) != st::status_t::success_k)
        return {util::status_code_t::out_of_memory_k, "No room for the values of a batch"};
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<util::byte_t const> bytes) noexcept {
        for (std::size_t index = 0; index != bytes.size() && offset + index < value_bytes_k; ++index)
            values[request][static_cast<std::size_t>(offset + index)] = bytes[index];
    };
    if (util::status_t const read = reader.select(channel, keys, std::span<lookup_result_t> {results}, sink);
        read.failed())
        return read;
    for (std::size_t index = 0; index != keys.size(); ++index) {
        present[index] = results[index] == lookup_result_t::written_k;
        counters[index] = present[index] ? read_counter(std::span<util::byte_t const> {values[index]}) : 0;
    }
    return {};
}

#pragma endregion Fixtures

#pragma region Atomicity Cases

/**
 *  @brief Threads commit whole chunks of keys at once, and every chunk ends written by exactly one commit.
 *
 *  Every thread writes the same chunks in turn, each chunk with a counter naming the thread and the
 *  chunk, and rolls back and writes again a chunk a conflict refused. A batched read excludes the
 *  write path, so a chunk read whole must hold one counter, whichever commit landed last.
 */
template <typename mount_type_>
void test_chunks_land_whole(mount_type_ const& mount) {
    using stack_t = stress_stack<typename mount_type_::fixture_t>;
    using database_t = typename stack_t::database_t;
    constexpr std::size_t chunks_k = 10;
    constexpr std::size_t keys_per_chunk_k = 100;
    constexpr std::size_t attempts_k = 1024;

    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    auto [store, open_status] = open_over(*stack);
    expect_true(open_status.succeeded());
    if (open_status.failed()) return;

    std::array<std::thread, worker_threads_k> workers;
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker)
        workers[worker] = std::thread {[&, worker]() noexcept {
            auto [guard, acquire_status] = stack->fixture->channel(2 * (2 + worker));
            expect_true(acquire_status.succeeded());
            if (acquire_status.failed()) return;
            auto [changing, begin_status] = store->create_transaction();
            expect_true(begin_status.succeeded());
            if (begin_status.failed()) return;

            std::array<util::byte_t, value_bytes_k> value {};
            st::vector<stored_key_t> keys;
            st::vector<bool> present;
            st::vector<std::uint64_t> counters;
            if (!expect_true(keys.resize(keys_per_chunk_k) == st::status_t::success_k)) return;
            for (std::size_t chunk = 0; chunk != chunks_k; ++chunk) {
                write_counter(chunk * worker_threads_k + worker, std::span<util::byte_t> {value});
                for (std::size_t index = 0; index != keys_per_chunk_k; ++index)
                    keys[index] = static_cast<stored_key_t>(chunk * keys_per_chunk_k + index);
                bool committed = false;
                for (std::size_t attempt = 0; attempt != attempts_k && !committed; ++attempt) {
                    // A refused attempt is rolled back and written again, so its watches start from now.
                    for (std::size_t index = 0; index != keys_per_chunk_k; ++index)
                        expect_true(changing->insert(keys[index], std::span<util::byte_t const> {value}).succeeded());
                    util::status_t const landed = changing->commit();
                    committed = landed.succeeded();
                    if (committed) break;
                    expect_true(landed == util::status_code_t::write_conflict_k);
                    changing->rollback();
                }
                expect_true(committed);

                expect_true(
                    read_counters(*store, *guard, std::span<stored_key_t const> {keys}, present, counters).succeeded());
                for (std::size_t index = 0; index != present.size(); ++index) {
                    expect_true(present[index]);
                    expect_equal(counters[index], counters[0]);
                }
            }
        }};
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker) workers[worker].join();

    st::vector<stored_key_t> keys;
    st::vector<bool> present;
    st::vector<std::uint64_t> counters;
    if (!expect_true(keys.resize(chunks_k * keys_per_chunk_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = static_cast<stored_key_t>(index);
    expect_true(store->flush().succeeded());
    expect_true(
        read_counters(*store, stack->channel(), std::span<stored_key_t const> {keys}, present, counters).succeeded());
    for (std::size_t index = 0; index != keys.size(); ++index) {
        std::size_t const chunk = index / keys_per_chunk_k;
        expect_true(present[index]);
        expect_equal(counters[index], counters[chunk * keys_per_chunk_k]);
        expect_equal(counters[index] / worker_threads_k, std::uint64_t {chunk});
    }
}

#pragma endregion Atomicity Cases

#pragma region Replay Cases

/** What one committed transaction did, filed under the stamp its commit claimed. */
struct record_t {

    /** Whether the transaction wrote, removed or only read. */
    enum class kind_t : std::uint8_t { insert_k, remove_k, read_k };

    /** The stamp the commit claimed, which orders it among every other. */
    transaction_stamp_t stamp = 0;

    /** What the transaction did. */
    kind_t kind = kind_t::read_k;

    /** The keys it touched, in the order it touched them. */
    st::vector<stored_key_t> keys;

    /** Whether a read found each key holding a value. */
    st::vector<bool> present;

    /** The counter written under each key, or the one a read found there. */
    st::vector<std::uint64_t> counters;
};

/** Orders records by the stamps their commits claimed. */
struct record_order_t {
    bool operator()(record_t const* first, record_t const* second) const noexcept {
        return first->stamp < second->stamp;
    }
};

/**
 *  @brief Writers, a remover and readers run transactions at once, and every read replays at its commit's stamp.
 *
 *  A read-only transaction that commits had nothing it watched written since it started, so what it read is the
 *  state every commit stamped before it left. A read a conflict refused is rolled back and recorded nowhere.
 */
template <typename mount_type_>
void test_transactions_replay_in_commit_order(mount_type_ const& mount) {
    using stack_t = stress_stack<typename mount_type_::fixture_t>;
    using database_t = typename stack_t::database_t;
    constexpr std::size_t keys_k = 512;
    constexpr std::size_t transactions_k = 100;
    constexpr std::size_t operations_max_k = 32;
    constexpr std::size_t inserters_k = 1;
    constexpr std::size_t removers_k = 1;

    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    auto [store, open_status] = open_over(*stack);
    expect_true(open_status.succeeded());
    if (open_status.failed()) return;

    std::array<st::vector<record_t>, worker_threads_k> records;
    std::array<std::thread, worker_threads_k> workers;
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker)
        workers[worker] = std::thread {[&, worker]() noexcept {
            auto [guard, acquire_status] = stack->fixture->channel(2 * (2 + worker));
            expect_true(acquire_status.succeeded());
            if (acquire_status.failed()) return;
            auto [changing, begin_status] = store->create_transaction();
            expect_true(begin_status.succeeded());
            if (begin_status.failed()) return;

            split_mix_generator_t random {case_stream_key(worker)};
            record_t::kind_t const kind = worker < inserters_k                ? record_t::kind_t::insert_k
                                          : worker < inserters_k + removers_k ? record_t::kind_t::remove_k
                                                                              : record_t::kind_t::read_k;
            std::array<util::byte_t, value_bytes_k> value {};
            for (std::size_t round = 0; round != transactions_k; ++round) {
                record_t record;
                record.kind = kind;
                std::size_t const operations = 1 + random.below(operations_max_k);
                if (!expect_true(record.keys.resize(operations) == st::status_t::success_k &&
                                 record.counters.resize(operations) == st::status_t::success_k))
                    return;
                for (std::size_t index = 0; index != operations; ++index) {
                    record.keys[index] = random.below(keys_k);
                    record.counters[index] = (worker + 1) * 1'000'000ull + round * operations_max_k + index;
                }

                if (kind == record_t::kind_t::read_k) {
                    expect_true(read_counters(*changing, *guard, std::span<stored_key_t const> {record.keys},
                                              record.present, record.counters)
                                    .succeeded());
                }
                else
                    for (std::size_t index = 0; index != operations; ++index) {
                        write_counter(record.counters[index], std::span<util::byte_t> {value});
                        util::status_t const written = kind == record_t::kind_t::insert_k
                                                           ? changing->insert(record.keys[index],
                                                                              std::span<util::byte_t const> {value})
                                                           : changing->remove(record.keys[index]);
                        expect_true(written.succeeded());
                    }

                util::status_t const committed = changing->commit();
                if (committed.failed()) {
                    expect_true(committed == util::status_code_t::write_conflict_k);
                    changing->rollback();
                    continue;
                }
                record.stamp = changing->last_commit_stamp();
                expect_true(records[worker].push_back(std::move(record)) == st::status_t::success_k);
            }
        }};
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker) workers[worker].join();

    st::vector<record_t const*> ordered;
    std::size_t reads = 0;
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker)
        for (record_t const& record : records[worker]) {
            expect_true(ordered.push_back(&record) == st::status_t::success_k);
            reads += record.kind == record_t::kind_t::read_k;
        }
    std::sort(ordered.begin(), ordered.end(), record_order_t {});
    expect_true(reads != 0);
    for (std::size_t index = 1; index < ordered.size(); ++index)
        expect_true(ordered[index - 1]->stamp < ordered[index]->stamp);

    std::array<bool, keys_k> model_present {};
    std::array<std::uint64_t, keys_k> model_counters {};
    for (record_t const* record : ordered)
        for (std::size_t index = 0; index != record->keys.size(); ++index) {
            std::size_t const key = static_cast<std::size_t>(record->keys[index]);
            switch (record->kind) {
            case record_t::kind_t::insert_k:
                model_present[key] = true;
                model_counters[key] = record->counters[index];
                break;
            case record_t::kind_t::remove_k: model_present[key] = false; break;
            case record_t::kind_t::read_k:
                expect_equal(record->present[index], model_present[key]);
                if (model_present[key]) expect_equal(record->counters[index], model_counters[key]);
                break;
            }
        }

    st::vector<stored_key_t> keys;
    st::vector<bool> present;
    st::vector<std::uint64_t> counters;
    if (!expect_true(keys.resize(keys_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != keys_k; ++index) keys[index] = static_cast<stored_key_t>(index);
    expect_true(store->flush().succeeded());
    expect_true(
        read_counters(*store, stack->channel(), std::span<stored_key_t const> {keys}, present, counters).succeeded());
    for (std::size_t index = 0; index != keys_k; ++index) {
        expect_equal(present[index], model_present[index]);
        if (model_present[index]) expect_equal(counters[index], model_counters[index]);
    }
}

#pragma endregion Replay Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;

    start_test_run(argc, argv);

    auto [directory, directory_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(directory_status)) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }

    auto const cases = [&](auto const& mount) {
        report_on_mount(mount, "test_chunks_land_whole", test_chunks_land_whole);
        report_on_mount(mount, "test_transactions_replay_in_commit_order", test_transactions_replay_in_commit_order);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(transport, directory.path(), stress_config_k, 1,
                         "single_collection_database_transaction_stress", cases);

    return finish_test_run();
}
