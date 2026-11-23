/**
 *  @file test/nvme/single_collection_database.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief One collection with transactions over the ring: what a commit shows, what a conflict keeps.
 *
 *  The results this file exists for are @c test_commit_lands_all_or_nothing
 *  and @c test_transactions_serialize_under_threads. A reader passing over
 *  every key while a commit lands must see the whole of that commit or
 *  none of it, and several threads racing over overlapping keys must end
 *  at a count no lost update could produce.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcmp`

#include <array>       // `std::array`
#include <filesystem>  // `std::filesystem::remove`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <thread>      // `std::thread`
#include <type_traits> // `std::remove_reference_t`
#include <utility>     // `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_store.hpp"                      // `nvme_store`, `nvme_store_cursor`
#include "ustore/blobs/nvme_key_slot.hpp"                   // `key_slot`
#include "ustore/blobs/nvme_single_collection_database.hpp" // `basic_single_collection_database`

#include "nvme/fixture.hpp" // `run_on_transport`, `expectation_t`, `painted`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Shape

/** The numbers every case mounts with: channels for a compaction to hold two, the case one, and every worker thread
 *  one. */
constexpr fixture_config_t scdb_config_k {.queues_threads = 8};

/** Threads the concurrency cases drive the database on. */
constexpr std::size_t worker_threads_k = 4;

/** The largest value the variable-width databases under test admit. */
constexpr stored_value_size_t value_max_size_k = 4096;

/** The keys and the key slots every case of this suite drives. */
using stored_key_t = std::uint64_t;
using variable_slot_t = key_slot<stored_key_t, value_layout_t::variable_width_k>;

/** The databases and the cursors every case drives over the volume of @p fixture_type_. */
template <typename fixture_type_>
struct scdb_types {
    using fixture_t = fixture_type_;
    using volume_t = typename fixture_t::volume_t;
    using variable_database_t = basic_single_collection_database<
        stored_key_t, volume_t, atomic_reference, waiter_t,
        nvme_store<variable_slot_t, volume_t, atomic_reference, waiter_t, last_write_fold_t, no_device_fold_t>>;
    using variable_cursor_t =
        nvme_store_cursor<variable_slot_t, volume_t, atomic_reference, waiter_t, last_write_fold_t, no_device_fold_t>;

    /** A fixture, the channels a database needs, and the volume over them. */
    struct stack_t {

        /** The disk, the arena and the channels. */
        std::unique_ptr<fixture_t> fixture;

        /** The channel the database writes, flushes and commits on. */
        io_channel_t* writing = nullptr;

        /** The second channel a compaction writes its new runs on. */
        io_channel_t* merging = nullptr;

        /** The channel the calling thread reads on. */
        io_channel_t* reading = nullptr;

        /** The volume every run and blob lands on, pinned so a database may point at it. */
        std::unique_ptr<volume_t> volume;

        /** The channel of the calling thread, which every read of this stack takes. */
        [[nodiscard]] io_channel_t& channel() noexcept { return *reading; }
    };
};

/**
 *  @brief The database shape a case opens, whose first level fills after a handful of flushes.
 *  @param[in] value_size Bytes every value takes, which is zero for every layout but the fixed one.
 *  @param[in] value_max_size Bytes the largest value takes, which is zero for a fixed-width layout.
 */
[[nodiscard]] single_collection_database_config_t test_config(stored_value_size_t value_size,
                                                              stored_value_size_t value_max_size) noexcept {
    single_collection_database_config_t config;
    config.collection.concurrency_limit = 1;
    config.collection.cache_limit = 1024ull * 1024ull;
    config.collection.memory_runs_max_bytes = 8ull * 1024ull * 1024ull;
    config.collection.memory_runs_max_deltas = 8192;
    config.collection.lsm_tree.level_enlarge_factor = 4;
    config.collection.lsm_tree.lsm_level_0.sorted_run.value_size = value_size;
    config.collection.lsm_tree.lsm_level_0.sorted_run.value_max_size = value_max_size;
    config.default_transaction.max_bytes = 256ull * 1024ull;
    config.default_transaction.max_deltas = 1024;
    return config;
}

/** Bytes one counter of the serializability case takes. */
constexpr std::size_t counter_bytes_k = 8;

/** Writes @p counter into @p into, little end first, so a read of it needs no alignment. */
void write_counter(std::uint64_t counter, std::span<byte_t> into) noexcept {
    for (std::size_t index = 0; index != counter_bytes_k; ++index)
        into[index] = static_cast<byte_t>((counter >> (index * 8)) & 0xFFull);
}

/** The counter @p from holds, which is zero for a span shorter than one. */
[[nodiscard]] std::uint64_t read_counter(std::span<byte_t const> from) noexcept {
    if (from.size() < counter_bytes_k) return 0;
    std::uint64_t counter = 0;
    for (std::size_t index = 0; index != counter_bytes_k; ++index)
        counter |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(from[index])) << (index * 8);
    return counter;
}

#pragma endregion Shape

#pragma region Fixtures

/** Mounts a fixture through @p mount, takes the three channels a case needs and formats one volume over them. */
template <typename mount_type_>
expected<std::unique_ptr<typename scdb_types<typename mount_type_::fixture_t>::stack_t>> make_stack(
    mount_type_ const& mount) noexcept {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using volume_t = typename types_t::volume_t;
    auto stack = std::unique_ptr<stack_t> {new (std::nothrow) stack_t {}};
    if (!stack) return status_t {status_code_t::out_of_memory_k, "The stack did not fit"};

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

    expected<volume_t> volume = stack->fixture->mount_volume(*stack->writing);
    if (!volume) return volume.status();
    stack->volume = std::unique_ptr<volume_t> {new (std::nothrow) volume_t {std::move(*volume)}};
    if (!stack->volume) return status_t {status_code_t::out_of_memory_k, "The volume did not fit"};
    return stack;
}

/** Mounts the volume of @p stack again and opens a database of @p database_type_ over it and the
 *  channels of @p stack, as a process starting after a close or a crash does. */
template <typename database_type_, typename stack_type_>
expected<typename database_type_::database_unique_ptr_t> open_over(
    stack_type_& stack, single_collection_database_config_t const& config) noexcept {
    using volume_t = typename std::remove_reference_t<decltype(*stack.volume)>;
    *stack.volume = volume_t {};
    auto [volume, mount_status] = stack.fixture->mount_volume(*stack.writing);
    if (mount_status.failed()) return mount_status;
    *stack.volume = std::move(volume);
    return database_type_::open(*stack.volume, *stack.writing, *stack.merging, config, stack.fixture->resources);
}

#pragma endregion Fixtures

#pragma region Model

/** Writes every entry of @p wanted through @p store, one call per key. */
template <typename database_type_>
void write_model(database_type_& store, std::span<expectation_t const> wanted) {
    st::vector<byte_t> value;
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        expectation_t const& entry = wanted[index];
        if (entry.state == lookup_result_t::removed_k) {
            expect_true(store.remove(entry.stored_key).succeeded());
            continue;
        }
        expect_true(value.resize(static_cast<std::size_t>(entry.bytes)) == st::status_t::success_k);
        for (std::uint64_t position = 0; position != entry.bytes; ++position)
            value[static_cast<std::size_t>(position)] = painted(entry.seed, position);
        expect_true(store.insert(entry.stored_key, std::span<byte_t const> {value}).succeeded());
    }
}

/** Reads every key of @p wanted through one batched select and counts what the model disagrees with. */
template <typename readable_type_>
void check_model(readable_type_ const& source, io_channel_t& channel, std::span<expectation_t const> wanted) {
    std::size_t const count = wanted.size();
    st::vector<stored_key_t> keys;
    st::vector<lookup_result_t> results;
    st::vector<std::uint64_t> seen;
    expect_true(keys.resize(count) == st::status_t::success_k);
    expect_true(results.resize(count) == st::status_t::success_k);
    if (!expect_true(seen.resize(count) == st::status_t::success_k)) return;
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
    expect_true(source.select(channel, std::span<stored_key_t const> {keys}, std::span<lookup_result_t> {results}, sink)
                    .succeeded());
    expect_equal(mismatches, std::size_t {0});

    for (std::size_t index = 0; index != count; ++index) {
        expect_true(results[index] == wanted[index].state);
        if (wanted[index].state == lookup_result_t::written_k) expect_equal(seen[index], wanted[index].bytes);
    }
}

#pragma endregion Model

#pragma region Round Trip Cases

/** Writes and reads one database of @p key_slot_type_, which is how every layout is covered. */
template <typename key_slot_type_, typename mount_type_>
void layout_round_trip(mount_type_ const& mount, stored_value_size_t value_size, stored_value_size_t value_max_size) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using volume_t = typename types_t::volume_t;
    using database_t = basic_single_collection_database<
        typename key_slot_type_::stored_key_t, volume_t, atomic_reference, waiter_t,
        nvme_store<key_slot_type_, volume_t, atomic_reference, waiter_t, last_write_fold_t, no_device_fold_t>>;
    constexpr std::size_t count_k = 300;

    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;

    single_collection_database_config_t const config = test_config(value_size, value_max_size);
    expected<typename database_t::database_unique_ptr_t> store = open_over<database_t>(**stack, config);
    expect_true(store.has_value());
    if (!store) return;

    stored_value_size_t const widest = (*store)->value_max_size();
    st::vector<expectation_t> model;
    if (!expect_true(model.resize(count_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 3;
        entry.state = index % 13 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
        entry.bytes = entry.state != lookup_result_t::written_k ? 0 : value_size != 0 ? value_size : 1 + index % widest;
        entry.seed = index + widest;
        model[index] = entry;
    }

    write_model(**store, std::span<expectation_t const> {model});
    check_model(**store, (*stack)->channel(), std::span<expectation_t const> {model});
    expect_true((*store)->flush().succeeded());
    expect_equal((*store)->memory_deltas_count(), std::size_t {0});
    check_model(**store, (*stack)->channel(), std::span<expectation_t const> {model});
}

/** Every value layout the database serves round trips through the memory runs and the tree. */
template <typename mount_type_>
void test_every_layout_round_trips(mount_type_ const& mount) {
    layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_12_bytes_k>>(mount, 0, 0);
    layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_44_bytes_k>>(mount, 0, 0);
    layout_round_trip<key_slot<stored_key_t, value_layout_t::fixed_width_k>>(mount, 512, 0);
    layout_round_trip<key_slot<stored_key_t, value_layout_t::variable_width_k>>(mount, 0, value_max_size_k);
}

/**
 *  @brief Writes a model of @p key_slot_type_, closes over its volume and reads every byte back after a reopen.
 *  @param[in] value_size Bytes every value takes, which is zero for every layout but the fixed one.
 *  @param[in] value_max_size Bytes the largest value takes, which is zero for a fixed-width layout.
 *
 *  The tree is given depth before the close - two flushed rounds merged into a second level, and a third
 *  round flushed over them - so a reopen that took back only level zero would lose the keys the merge buried.
 */
template <typename key_slot_type_, typename mount_type_>
void layout_survives_reopen(mount_type_ const& mount, stored_value_size_t value_size,
                            stored_value_size_t value_max_size) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using volume_t = typename types_t::volume_t;
    using database_t = basic_single_collection_database<
        typename key_slot_type_::stored_key_t, volume_t, atomic_reference, waiter_t,
        nvme_store<key_slot_type_, volume_t, atomic_reference, waiter_t, last_write_fold_t, no_device_fold_t>>;
    constexpr std::size_t count_k = 400;

    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;

    // The smallest factor makes level zero due at two flushed runs, so two rounds give the tree depth.
    single_collection_database_config_t config = test_config(value_size, value_max_size);
    config.collection.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;
    st::vector<expectation_t> model;
    if (!expect_true(model.resize(count_k) == st::status_t::success_k)) return;

    std::size_t depth = 0;
    std::size_t shallowest_runs = 0;
    std::uint64_t population = 0;
    {
        expected<typename database_t::database_unique_ptr_t> store = open_over<database_t>(**stack, config);
        expect_true(store.has_value());
        if (!store) return;
        stored_value_size_t const widest = (*store)->value_max_size();

        for (std::uint64_t round = 0; round != 2; ++round) {
            for (std::size_t index = 0; index != count_k; ++index) {
                expectation_t entry;
                entry.stored_key = static_cast<stored_key_t>(index) * 3;
                entry.state = lookup_result_t::written_k;
                entry.bytes = value_size != 0 ? value_size : 1 + index % widest;
                entry.seed = index + round * 7919;
                model[index] = entry;
            }
            write_model(**store, std::span<expectation_t const> {model});
            expect_true((*store)->flush().succeeded());
        }

        expected<compaction_outcome_t> const merged = (*store)->compact();
        expect_true(merged.has_value());
        if (!merged) return;
        expect_true(*merged == compaction_outcome_t::published_k);

        // A merge publishes its new set before it drops what it consumed, so the manifest names
        // every run file left.
        expect_equal(run_count_of(*(*stack)->fixture), run_files_of(*(*stack)->fixture));

        // Two overlapping rounds go back onto level zero, so age alone decides which run answers a key,
        // and the keys neither round names can only be answered from the level the merge wrote.
        for (std::uint64_t again = 0; again != 2; ++again) {
            st::vector<expectation_t> batch;
            std::size_t const stride = again == 0 ? 3 : 6;
            for (std::size_t index = 0; index < count_k; index += stride) {
                expectation_t entry = model[index];
                entry.state = again == 0 && index % 9 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
                entry.bytes = entry.state != lookup_result_t::written_k ? 0
                              : value_size != 0                         ? value_size
                                                                        : 1 + (index + 5 + again) % widest;
                entry.seed = index + 104729 + again * 31;
                model[index] = entry;
                expect_true(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
            }
            write_model(**store, std::span<expectation_t const> {batch});
            expect_true((*store)->flush().succeeded());
        }
        check_model(**store, (*stack)->channel(), std::span<expectation_t const> {model});

        depth = (*store)->records().tree().levels().size();
        shallowest_runs = (*store)->records().tree().levels().level(0).size();
        population = (*store)->population_count();
        expect_true(depth > std::size_t {1});
        if (!expect_true(shallowest_runs > std::size_t {1})) return;
    }

    expected<typename database_t::database_unique_ptr_t> store = open_over<database_t>(**stack, config);
    expect_true(store.has_value());
    if (!store) return;
    expect_equal((*store)->records().tree().levels().size(), depth);
    expect_equal((*store)->records().tree().levels().level(0).size(), shallowest_runs);
    expect_equal((*store)->population_count(), population);
    check_model(**store, (*stack)->channel(), std::span<expectation_t const> {model});
}

/** Every layout the database serves comes back byte for byte, and out of every level, after a reopen. */
template <typename mount_type_>
case_result_t test_every_layout_survives_reopen(mount_type_ const& mount) {
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    layout_survives_reopen<key_slot<stored_key_t, value_layout_t::inline_12_bytes_k>>(mount, 0, 0);
    layout_survives_reopen<key_slot<stored_key_t, value_layout_t::fixed_width_k>>(mount, 512, 0);
    layout_survives_reopen<key_slot<stored_key_t, value_layout_t::variable_width_k>>(mount, 0, value_max_size_k);
    return {};
}

/**
 *  @brief Runs no manifest ever named are unlinked at the next mount, and a manifest naming a
 *      missing run is refused on every reopen, as a refused mount publishes nothing.
 *
 *  A flush writes the run first and publishes the manifest second, so the only state a crash
 *  between the two leaves is a run nobody names. The other order would leave a manifest
 *  naming bytes the volume never got, which the second half forces by hand to show what a
 *  reopen does with it.
 */
template <typename mount_type_>
case_result_t test_crash_between_seal_and_schema(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using variable_database_t = typename types_t::variable_database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    single_collection_database_config_t const config = test_config(0, value_max_size_k);

    {
        expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
        expect_status(stack, status_code_t::success_k);
        if (!stack) return {};

        {
            expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(
                **stack, config);
            expect_true(store.has_value());
            if (!store) return {};
            st::vector<byte_t> value;
            expect_true(value.resize(200) == st::status_t::success_k);
            for (std::size_t index = 0; index != value.size(); ++index) value[index] = painted(5, index);
            for (stored_key_t key = 0; key != 64; ++key)
                expect_true((*store)->insert(key, std::span<byte_t const> {value}).succeeded());
            expect_true((*store)->flush().succeeded());

            // A flush writes its run and publishes the manifest second, so the manifest names
            // every run file.
            expect_equal(run_count_of(*(*stack)->fixture), run_files_of(*(*stack)->fixture));
        }

        expect_true(run_count_of(*(*stack)->fixture) != std::size_t {0});
        expect_true(run_bytes_of(*(*stack)->fixture) != std::uint64_t {0});

        // No manifest that would have named those runs ever landed.
        std::array<char, 96> name {};
        auto const written = fmt::format_to_n(name.data(), name.size() - 1, "{}.ustore.json", (*stack)->fixture->suite);
        *written.out = '\0';
        std::string const manifest = fmt::format("{}/{}", (*stack)->fixture->data_directory_path(0), name.data());
        expect_true(std::filesystem::remove(manifest.c_str()));

        expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                             config);
        expect_true(store.has_value());
        if (!store) return {};
        expect_equal((*store)->population_count(), std::uint64_t {0});
        // The mount unlinked the run files no manifest names.
        expect_equal(run_files_of(*(*stack)->fixture), std::size_t {0});

        // The volume is still a volume: a write over it flushes and answers.
        st::vector<byte_t> value;
        expect_true(value.resize(64) == st::status_t::success_k);
        for (std::size_t index = 0; index != value.size(); ++index) value[index] = painted(9, index);
        expect_true((*store)->insert(stored_key_t {1000}, std::span<byte_t const> {value}).succeeded());
        expect_true((*store)->flush().succeeded());
        st::vector<byte_t> back;
        expect_true(back.resize(value.size()) == st::status_t::success_k);
        stored_value_size_t size = 0;
        expect_true(
            (*store)->select((*stack)->channel(), stored_key_t {1000}, std::span<byte_t> {back}, size).succeeded());
        expect_equal(std::size_t {size}, value.size());
        expect_equal(std::memcmp(back.data(), value.data(), value.size()), 0);
    }

    {
        expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
        expect_status(stack, status_code_t::success_k);
        if (!stack) return {};

        {
            expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(
                **stack, config);
            expect_true(store.has_value());
            if (!store) return {};
            st::vector<byte_t> value;
            expect_true(value.resize(200) == st::status_t::success_k);
            for (std::size_t index = 0; index != value.size(); ++index) value[index] = painted(5, index);
            for (stored_key_t round = 0; round != 2; ++round) {
                for (stored_key_t key = round * 64; key != round * 64 + 64; ++key)
                    expect_true((*store)->insert(key, std::span<byte_t const> {value}).succeeded());
                expect_true((*store)->flush().succeeded());
            }
        }
        expect_equal(run_count_of(*(*stack)->fixture), std::size_t {2});

        // The file of one of the two runs the manifest names never reached the environment.
        manifest_t const manifest = manifest_on_disk(*(*stack)->fixture);
        char name[path_max_bytes_k];
        expect_true(manifest.file_name(manifest.files[0].run, name).succeeded());
        std::string const run_path = fmt::format("{}/{}", (*stack)->fixture->data_directory_path(0), name);
        expect_true(std::filesystem::remove(run_path.c_str()));

        // A refused mount publishes no manifest naming the other run alone, so it refuses again.
        for (std::size_t attempt = 0; attempt != 2; ++attempt) {
            expected<typename variable_database_t::database_unique_ptr_t> const store = open_over<variable_database_t>(
                **stack, config);
            expect_true(!store.has_value());
        }
    }
    return {};
}

/** A reopen takes back what the database itself wrote to the volume, and refuses another config. */
template <typename mount_type_>
case_result_t test_reopen_agrees_on_its_schema(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using variable_database_t = typename types_t::variable_database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return {};

    single_collection_database_config_t const config = test_config(0, value_max_size_k);
    st::vector<byte_t> owner_bytes;
    expect_true(owner_bytes.resize(3000) == st::status_t::success_k);
    for (std::size_t index = 0; index != owner_bytes.size(); ++index) owner_bytes[index] = painted(77, index);

    {
        expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                             config);
        expect_true(store.has_value());
        if (!store) return {};

        st::vector<byte_t> value;
        expect_true(value.resize(200) == st::status_t::success_k);
        for (std::size_t index = 0; index != value.size(); ++index) value[index] = painted(5, index);
        for (stored_key_t key = 0; key != 64; ++key)
            expect_true((*store)->insert(key, std::span<byte_t const> {value}).succeeded());
        expect_true((*store)->flush().succeeded());
        expect_true((*store)->save_user_data(std::span<byte_t const> {owner_bytes}).succeeded());
        expect_equal((*store)->user_data_size_bytes(), std::uint64_t {owner_bytes.size()});
    }

    // The runs the flush published are still named on the volume the database was closed over.
    std::size_t const runs = run_count_of(*(*stack)->fixture);
    expect_true(runs != 0);

    {
        expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                             config);
        expect_true(store.has_value());
        if (!store) return {};

        expect_equal((*store)->user_data_size_bytes(), std::uint64_t {owner_bytes.size()});
        st::vector<byte_t> back;
        expect_true(back.resize(owner_bytes.size()) == st::status_t::success_k);
        expect_true((*store)->load_user_data((*stack)->channel(), std::span<byte_t> {back}).succeeded());
        expect_equal(std::memcmp(back.data(), owner_bytes.data(), owner_bytes.size()), 0);

        // The runs above are back in the tree, so every key the close left behind answers again.
        expect_equal((*store)->population_count(), std::uint64_t {64});
        expect_equal(run_count_of(*(*stack)->fixture), runs);

        expect_true((*store)->clear_user_data().succeeded());
        expect_equal((*store)->user_data_size_bytes(), std::uint64_t {0});
    }

    // A volume whose manifest names one shape refuses a database of another.
    single_collection_database_config_t narrower = config;
    narrower.collection.lsm_tree.lsm_level_0.sorted_run.value_max_size = value_max_size_k / 2;
    expected<typename variable_database_t::database_unique_ptr_t> refused = open_over<variable_database_t>(**stack,
                                                                                                           narrower);
    expect_status(refused, status_code_t::invalid_argument_k);
    return {};
}

#pragma endregion Round Trip Cases

#pragma region Transaction Cases

/**
 *  @brief A reader passing over every key sees the whole of a commit or none of it.
 *
 *  Every pass is one batched select, which holds the read path for all of its keys, so a pass that
 *  held two versions at once would be a commit landing in the middle of a reader.
 */
template <typename mount_type_>
void test_commit_lands_all_or_nothing(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using variable_database_t = typename types_t::variable_database_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t keys_k = 48;
    constexpr std::size_t value_bytes_k = 96;
    constexpr std::size_t commits_k = 120;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;

    single_collection_database_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                         config);
    expect_true(store.has_value());
    if (!store) return;

    st::vector<byte_t> value;
    expect_true(value.resize(value_bytes_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != value_bytes_k; ++index) value[index] = painted(1, index);
    for (stored_key_t key = 0; key != keys_k; ++key)
        expect_true((*store)->insert(key, std::span<byte_t const> {value}).succeeded());
    if (!expect_true((*store)->flush().succeeded())) return;

    std::thread writer {[&]() noexcept {
        expected<io_channel_t*> guard = (*stack)->fixture->channel(3);
        expect_true(guard.has_value());
        if (!guard) return;

        expected<typename variable_database_t::transaction_unique_ptr_t> changing = (*store)->create_transaction();
        expect_true(changing.has_value());
        if (!changing) return;

        st::vector<byte_t> painted_value;
        expect_true(painted_value.resize(value_bytes_k) == st::status_t::success_k);
        for (std::size_t round = 0; round != commits_k; ++round) {
            std::uint64_t const version = 1 + round % 2;
            for (std::size_t index = 0; index != value_bytes_k; ++index) painted_value[index] = painted(version, index);
            for (stored_key_t key = 0; key != keys_k; ++key)
                expect_true((*changing)->insert(key, std::span<byte_t const> {painted_value}).succeeded());
            expect_true((*changing)->commit().succeeded());
        }
    }};

    std::size_t torn = 0;
    std::size_t unknown = 0;
    std::size_t passes = 0;
    st::vector<stored_key_t> keys;
    st::vector<lookup_result_t> results;
    st::vector<byte_t> held;
    expect_true(keys.resize(keys_k) == st::status_t::success_k);
    expect_true(results.resize(keys_k) == st::status_t::success_k);
    expect_true(held.resize(keys_k * value_bytes_k) == st::status_t::success_k);
    for (stored_key_t key = 0; key != keys_k; ++key) keys[static_cast<std::size_t>(key)] = key;

    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        for (std::size_t index = 0; index != bytes.size(); ++index)
            held[request * value_bytes_k + static_cast<std::size_t>(offset) + index] = bytes[index];
    };
    while (passes != 400) {
        expect_true((*store)
                        ->select((*stack)->channel(), std::span<stored_key_t const> {keys},
                                 std::span<lookup_result_t> {results}, sink)
                        .succeeded());
        std::size_t versions_seen = 0;
        for (std::uint64_t version = 1; version != 3; ++version) {
            std::size_t matching = 0;
            for (std::size_t request = 0; request != keys_k; ++request) {
                std::size_t same = 0;
                for (std::size_t index = 0; index != value_bytes_k; ++index)
                    same += held[request * value_bytes_k + index] == painted(version, index);
                matching += same == value_bytes_k;
            }
            versions_seen += matching != 0;
            if (matching != 0 && matching != keys_k) ++torn;
        }
        unknown += versions_seen == 0;
        ++passes;
    }
    writer.join();
    expect_equal(torn, std::size_t {0});
    expect_equal(unknown, std::size_t {0});
}

/** A read-write conflict refuses the stage and leaves the changes where a caller can still log them. */
template <typename mount_type_>
void test_conflict_keeps_its_changes(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using variable_database_t = typename types_t::variable_database_t;
    using stack_t = typename types_t::stack_t;
    constexpr stored_key_t contested_k = 11;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;

    single_collection_database_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                         config);
    expect_true(store.has_value());
    if (!store) return;

    std::array<byte_t, counter_bytes_k> original {};
    write_counter(100, std::span<byte_t> {original});
    expect_true((*store)->insert(contested_k, std::span<byte_t const> {original}).succeeded());
    expect_true((*store)->flush().succeeded());

    expected<typename variable_database_t::transaction_unique_ptr_t> first = (*store)->create_transaction();
    expected<typename variable_database_t::transaction_unique_ptr_t> second = (*store)->create_transaction();
    expect_true(first.has_value() && second.has_value());
    if (!first || !second) return;

    // Both read the key before either writes it, which is the shape a lost update takes.
    std::array<byte_t, counter_bytes_k> seen {};
    stored_value_size_t size = 0;
    expect_true((*first)->select((*stack)->channel(), contested_k, std::span<byte_t> {seen}, size).succeeded());
    expect_equal(read_counter(std::span<byte_t const> {seen}), std::uint64_t {100});
    expect_true((*second)->select((*stack)->channel(), contested_k, std::span<byte_t> {seen}, size).succeeded());

    std::array<byte_t, counter_bytes_k> from_first {};
    std::array<byte_t, counter_bytes_k> from_second {};
    write_counter(101, std::span<byte_t> {from_first});
    write_counter(202, std::span<byte_t> {from_second});
    expect_true((*first)->insert(contested_k, std::span<byte_t const> {from_first}).succeeded());
    expect_true((*second)->insert(contested_k, std::span<byte_t const> {from_second}).succeeded());

    expect_true((*first)->commit().succeeded());
    status_t const refused = (*second)->stage();
    expect_status(refused, status_code_t::write_conflict_k);

    // A stage that was not refused holds the write path, and every check below reads through it.
    if (refused.succeeded()) (*second)->rollback();

    // The refused stage keeps the changes, so the caller can still read back what it could not commit.
    stored_value_size_t kept = 0;
    expect_true((*second)->find_uncommitted(contested_k, kept) == lookup_result_t::written_k);
    expect_equal(static_cast<std::size_t>(kept), counter_bytes_k);
    expect_true((*second)->select((*stack)->channel(), contested_k, std::span<byte_t> {seen}, size).succeeded());
    expect_equal(read_counter(std::span<byte_t const> {seen}), std::uint64_t {202});

    // The database still holds only what the winner wrote.
    expect_true((*store)->select((*stack)->channel(), contested_k, std::span<byte_t> {seen}, size).succeeded());
    expect_equal(read_counter(std::span<byte_t const> {seen}), std::uint64_t {101});

    // Rolling back and doing the work again is what the loser is supposed to do next.
    (*second)->rollback();
    expect_true((*second)->find_uncommitted(contested_k, kept) == lookup_result_t::missing_k);
    expect_true((*second)->select((*stack)->channel(), contested_k, std::span<byte_t> {seen}, size).succeeded());
    write_counter(read_counter(std::span<byte_t const> {seen}) + 1, std::span<byte_t> {from_second});
    expect_true((*second)->insert(contested_k, std::span<byte_t const> {from_second}).succeeded());
    expect_true((*second)->commit().succeeded());
    expect_true((*store)->select((*stack)->channel(), contested_k, std::span<byte_t> {seen}, size).succeeded());
    expect_equal(read_counter(std::span<byte_t const> {seen}), std::uint64_t {102});

    // A transaction that only read the contested key, and wrote elsewhere, is refused just the same.
    expected<typename variable_database_t::transaction_unique_ptr_t> observing = (*store)->create_transaction();
    expected<typename variable_database_t::transaction_unique_ptr_t> overwriting = (*store)->create_transaction();
    expect_true(observing.has_value() && overwriting.has_value());
    if (!observing || !overwriting) return;

    expect_true((*observing)->select((*stack)->channel(), contested_k, std::span<byte_t> {seen}, size).succeeded());
    expect_true((*observing)->insert(stored_key_t {99}, std::span<byte_t const> {from_first}).succeeded());
    expect_true((*overwriting)->insert(contested_k, std::span<byte_t const> {from_second}).succeeded());
    expect_true((*overwriting)->commit().succeeded());
    status_t const observed = (*observing)->stage();
    expect_status(observed, status_code_t::write_conflict_k);
    if (observed.succeeded()) (*observing)->rollback();
    expect_true((*store)->find(stored_key_t {99}, size) == lookup_result_t::missing_k);
}

/** A rollback drops the changes, and a renew keeps them but only for a transaction that has read nothing. */
template <typename mount_type_>
void test_rollback_and_renew_behave(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using variable_database_t = typename types_t::variable_database_t;
    using stack_t = typename types_t::stack_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;

    single_collection_database_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                         config);
    expect_true(store.has_value());
    if (!store) return;

    expected<typename variable_database_t::transaction_unique_ptr_t> changing = (*store)->create_transaction();
    expect_true(changing.has_value());
    if (!changing) return;

    std::array<byte_t, counter_bytes_k> value {};
    write_counter(7, std::span<byte_t> {value});
    stored_value_size_t size = 0;

    // A rollback leaves neither the change nor a trace of it in the database.
    expect_true((*changing)->insert(stored_key_t {1}, std::span<byte_t const> {value}).succeeded());
    (*changing)->rollback();
    expect_true((*changing)->find_uncommitted(stored_key_t {1}, size) == lookup_result_t::missing_k);
    expect_true((*store)->find(stored_key_t {1}, size) == lookup_result_t::missing_k);

    // A renew keeps the changes of a transaction that has only written.
    expect_true((*changing)->insert(stored_key_t {2}, std::span<byte_t const> {value}).succeeded());
    expect_true((*changing)->renew().succeeded());
    expect_true((*changing)->find_uncommitted(stored_key_t {2}, size) == lookup_result_t::written_k);
    expect_true((*changing)->commit().succeeded());
    expect_true((*store)->find(stored_key_t {2}, size) == lookup_result_t::written_k);

    // A transaction that has read something may not renew, whatever it has written since.
    std::array<byte_t, counter_bytes_k> seen {};
    expect_true((*changing)->select((*stack)->channel(), stored_key_t {2}, std::span<byte_t> {seen}, size).succeeded());
    expect_status((*changing)->renew(), status_code_t::invalid_argument_k);

    // Nor may a staged transaction, and the stage's queue pair is handed back by the rollback and not by the renew.
    (*changing)->rollback();
    expect_true((*changing)->insert(stored_key_t {3}, std::span<byte_t const> {value}).succeeded());
    expect_true((*changing)->stage().succeeded());
    expect_status((*changing)->renew(), status_code_t::invalid_argument_k);
    (*changing)->rollback();
    expect_true((*store)->find(stored_key_t {3}, size) == lookup_result_t::missing_k);

    // The transaction is running again after all of that, so it still commits.
    expect_true((*changing)->insert(stored_key_t {4}, std::span<byte_t const> {value}).succeeded());
    expect_true((*changing)->commit().succeeded());
    expect_true((*store)->find(stored_key_t {4}, size) == lookup_result_t::written_k);
}

/**
 *  @brief Several threads running transactions over overlapping keys end where no update was lost.
 *
 *  Every round reads a counter, writes it back raised by one and commits, retrying what a conflict
 *  refuses. A lost update would leave the counters short of the commits that landed.
 */
template <typename mount_type_>
void test_transactions_serialize_under_threads(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using variable_database_t = typename types_t::variable_database_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t keys_k = 4;
    constexpr std::size_t rounds_k = 60;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;

    single_collection_database_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                         config);
    expect_true(store.has_value());
    if (!store) return;

    std::array<byte_t, counter_bytes_k> zero {};
    write_counter(0, std::span<byte_t> {zero});
    for (stored_key_t key = 0; key != keys_k; ++key)
        expect_true((*store)->insert(key, std::span<byte_t const> {zero}).succeeded());
    if (!expect_true((*store)->flush().succeeded())) return;

    std::array<std::size_t, worker_threads_k> landed {};
    std::array<std::thread, worker_threads_k> workers;
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker)
        workers[worker] = std::thread {[&, worker]() noexcept {
            expected<io_channel_t*> guard = (*stack)->fixture->channel(3 + worker);
            expect_true(guard.has_value());
            if (!guard) return;

            expected<typename variable_database_t::transaction_unique_ptr_t> changing = (*store)->create_transaction();
            expect_true(changing.has_value());
            if (!changing) return;

            std::array<byte_t, counter_bytes_k> seen {};
            std::array<byte_t, counter_bytes_k> raised {};
            for (std::size_t round = 0; round != rounds_k; ++round) {
                stored_key_t const key = static_cast<stored_key_t>((round + worker) % keys_k);
                for (std::size_t attempt = 0; attempt != 256; ++attempt) {
                    stored_value_size_t size = 0;
                    if (!expect_true((*changing)->select(**guard, key, std::span<byte_t> {seen}, size).succeeded()))
                        break;
                    write_counter(read_counter(std::span<byte_t const> {seen}) + 1, std::span<byte_t> {raised});
                    if (!expect_true((*changing)->insert(key, std::span<byte_t const> {raised}).succeeded())) break;
                    status_t const committed = (*changing)->commit();
                    if (committed.succeeded()) {
                        ++landed[worker];
                        break;
                    }
                    expect_status(committed, status_code_t::write_conflict_k);
                    (*changing)->rollback();
                }
            }
        }};
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker) workers[worker].join();

    std::size_t commits = 0;
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker) { commits += landed[worker]; }
    expect_equal(commits, worker_threads_k * rounds_k);

    std::uint64_t counted = 0;
    for (stored_key_t key = 0; key != keys_k; ++key) {
        std::array<byte_t, counter_bytes_k> seen {};
        stored_value_size_t size = 0;
        expect_true((*store)->select((*stack)->channel(), key, std::span<byte_t> {seen}, size).succeeded());
        counted += read_counter(std::span<byte_t const> {seen});
    }
    expect_equal(counted, std::uint64_t {commits});
}

#pragma endregion Transaction Cases

#pragma region Snapshot Cases

/**
 *  @brief A snapshot answers the moment it was taken at while another thread commits and flushes over it.
 *
 *  The writer waits for no snapshot: the reads in the middle run beside its commits and its flush, and see the
 *  older bytes because the snapshot pins their stamp, which the flush keeps a version for.
 */
template <typename mount_type_>
void test_snapshot_sees_its_moment(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using variable_database_t = typename types_t::variable_database_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t bytes_k = 700;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;

    single_collection_database_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                         config);
    expect_true(store.has_value());
    if (!store) return;

    st::vector<byte_t> older;
    st::vector<byte_t> newer;
    expect_true(older.resize(bytes_k) == st::status_t::success_k);
    expect_true(newer.resize(bytes_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != bytes_k; ++index) older[index] = painted(11, index);
    for (std::size_t index = 0; index != bytes_k; ++index) newer[index] = painted(12, index);

    for (stored_key_t key = 0; key != 8; ++key)
        expect_true((*store)->insert(key, std::span<byte_t const> {older}).succeeded());

    // A snapshot flushes nothing: what the memory runs hold is inside it already.
    expected<typename variable_database_t::snapshot_t> taken = (*store)->pin_stamp();
    expect_true(taken.has_value());
    if (!taken) return;
    expect_equal((*store)->population_count(), std::uint64_t {8});
    expect_equal((*store)->memory_deltas_count(), std::size_t {8});

    std::thread writer {[&]() noexcept {
        expected<io_channel_t*> guard = (*stack)->fixture->channel(3);
        expect_true(guard.has_value());
        if (!guard) return;
        for (stored_key_t key = 0; key != 8; ++key)
            expect_true((*store)->insert(key, std::span<byte_t const> {newer}).succeeded());
        expect_true((*store)->insert(stored_key_t {100}, std::span<byte_t const> {newer}).succeeded());
        expect_true((*store)->flush().succeeded());
    }};

    st::vector<byte_t> into;
    expect_true(into.resize(bytes_k) == st::status_t::success_k);
    for (stored_key_t key = 0; key != 8; ++key) {
        stored_value_size_t size = 0;
        expect_true(taken->select((*stack)->channel(), key, std::span<byte_t> {into}, size).succeeded());
        expect_equal(static_cast<std::size_t>(size), bytes_k);
        expect_equal(std::memcmp(into.data(), older.data(), bytes_k), 0);
    }

    // A key written after the snapshot was taken is not a key the snapshot has.
    stored_value_size_t missing = 0;
    expect_true(taken->find(stored_key_t {100}, missing) == lookup_result_t::missing_k);

    taken = expected<typename variable_database_t::snapshot_t> {status_t {status_code_t::uninitialized_k, "Let go"}};
    writer.join();
    expect_true((*store)->find(stored_key_t {100}, missing) == lookup_result_t::written_k);

    // The database itself moved on, and every key of it now holds the newer bytes.
    for (stored_key_t key = 0; key != 8; ++key) {
        stored_value_size_t size = 0;
        expect_true((*store)->select((*stack)->channel(), key, std::span<byte_t> {into}, size).succeeded());
        expect_equal(std::memcmp(into.data(), newer.data(), bytes_k), 0);
    }
}

#pragma endregion Snapshot Cases

#pragma region Scan Cases

/** An ordered scan visits every key once, across the memory runs and several levels. */
template <typename mount_type_>
void test_scan_visits_every_key_in_order(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using variable_database_t = typename types_t::variable_database_t;
    using variable_cursor_t = typename types_t::variable_cursor_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t keys_k = 800;
    constexpr std::size_t rounds_k = 4;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;

    single_collection_database_config_t const config = test_config(0, value_max_size_k);
    expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                         config);
    expect_true(store.has_value());
    if (!store) return;

    st::vector<expectation_t> model;
    if (!expect_true(model.resize(keys_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != keys_k; ++index) model[index].stored_key = static_cast<stored_key_t>(index);

    auto apply = [&](std::span<expectation_t const> batch) noexcept {
        for (std::size_t index = 0; index != batch.size(); ++index)
            model[static_cast<std::size_t>(batch[index].stored_key)] = batch[index];
    };

    for (std::size_t round = 0; round != rounds_k; ++round) {
        st::vector<expectation_t> batch;
        for (std::size_t index = round; index < keys_k; index += rounds_k) {
            expectation_t entry;
            entry.stored_key = static_cast<stored_key_t>(index);
            entry.state = lookup_result_t::written_k;
            entry.bytes = 40 + index % 97;
            entry.seed = index * 31 + round;
            expect_true(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
        }
        write_model(**store, std::span<expectation_t const> {batch});
        bool const flushed = expect_true((*store)->flush().succeeded());
        apply(std::span<expectation_t const> {batch});
        if (!flushed) return;
    }

    // One merge puts a second level under the first, so the scan picks between levels as well as runs.
    expect_true((*store)->needs_compaction());
    expected<compaction_outcome_t> const merged = (*store)->compact();
    expect_true(merged.has_value());
    if (!merged) return;
    expect_true(*merged == compaction_outcome_t::published_k);

    // What stays in memory runs is what the scan must merge in front of every run.
    {
        st::vector<expectation_t> batch;
        for (std::size_t index = 0; index < keys_k; index += 25) {
            expectation_t entry;
            entry.stored_key = static_cast<stored_key_t>(index);
            entry.state = index % 50 == 0 ? lookup_result_t::removed_k : lookup_result_t::written_k;
            entry.bytes = entry.state == lookup_result_t::written_k ? 13 + index % 29 : 0;
            entry.seed = index * 7 + 3;
            expect_true(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
        }
        write_model(**store, std::span<expectation_t const> {batch});
        apply(std::span<expectation_t const> {batch});
    }
    expect_true((*store)->memory_deltas_count() != 0);

    utilities::local_arena_t walk;
    expected<variable_cursor_t> scan = variable_cursor_t::over((*store)->records(), walk.allocator().arena);
    expect_true(scan.has_value());
    if (!scan) return;

    // The walk is bounded so a merge that forgets to advance a cursor is a result and not a hang.
    constexpr std::size_t visits_limit_k = keys_k + 64;
    std::size_t visited = 0;
    std::size_t out_of_order = 0;
    std::size_t wrong_state = 0;
    std::size_t wrong_size = 0;
    stored_key_t previous = 0;
    while (!scan->reached_end() && visited != visits_limit_k) {
        if (visited != 0 && !(previous < scan->stored_key())) ++out_of_order;
        previous = scan->stored_key();
        expectation_t const& wanted = model[static_cast<std::size_t>(previous)];
        if (scan->state() != wanted.state) ++wrong_state;
        if (static_cast<std::uint64_t>(scan->size()) != wanted.bytes) ++wrong_size;
        ++visited;
        expect_true(scan->advance().succeeded());
    }
    expect_equal(visited, keys_k);
    expect_equal(out_of_order, std::size_t {0});
    expect_equal(wrong_state, std::size_t {0});
    expect_equal(wrong_size, std::size_t {0});

    check_model(**store, (*stack)->channel(), std::span<expectation_t const> {model});
}

#pragma endregion Scan Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;

    start_test_run(argc, argv);

    st::expected<scratch_directory_t> const directory = scratch_directory_t::make(test_run.settings.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }

    auto const cases = [&](auto const& mount) {
        report_on_mount(mount, "test_every_layout_round_trips", test_every_layout_round_trips);
        report_on_mount(mount, "test_reopen_agrees_on_its_schema", test_reopen_agrees_on_its_schema);
        report_on_mount(mount, "test_every_layout_survives_reopen", test_every_layout_survives_reopen);
        report_on_mount(mount, "test_crash_between_seal_and_schema", test_crash_between_seal_and_schema);
        report_on_mount(mount, "test_commit_lands_all_or_nothing", test_commit_lands_all_or_nothing);
        report_on_mount(mount, "test_conflict_keeps_its_changes", test_conflict_keeps_its_changes);
        report_on_mount(mount, "test_rollback_and_renew_behave", test_rollback_and_renew_behave);
        report_on_mount(mount, "test_transactions_serialize_under_threads", test_transactions_serialize_under_threads);
        report_on_mount(mount, "test_snapshot_sees_its_moment", test_snapshot_sees_its_moment);
        report_on_mount(mount, "test_scan_visits_every_key_in_order", test_scan_visits_every_key_in_order);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(transport, directory->path(), scdb_config_k, 1, "single_collection_database", cases);

    return finish_test_run();
}
