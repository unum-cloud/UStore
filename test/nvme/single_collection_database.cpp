/**
 *  @file test/nvme/single_collection_database.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief One collection with transactions over the ring: what a commit shows, what a conflict keeps.
 *
 *  The results this file exists for are @c commit_lands_all_or_nothing and
 *  @c transactions_serialize_under_threads. A reader passing over every key while a commit lands must
 *  see the whole of that commit or none of it, and several threads racing over overlapping keys must
 *  end at a count no lost update could produce.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcmp`

#include <array>   // `std::array`
#include <memory>  // `std::unique_ptr`
#include <new>     // `std::nothrow`
#include <span>    // `std::span`
#include <thread>  // `std::thread`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/blobs/nvme_collection.hpp" // `collection_scanner`
#include "ustore/blobs/nvme_key_slot.hpp"   // `key_slot`
#include "ustore/blobs/nvme_database.hpp"   // `basic_single_collection_database`

#include "nvme/fixture.hpp" // `basic_fixture`, `run_on_transport`, `expectation_t`, `painted`

namespace unum::ustore::test {

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Shape

/** The numbers every case mounts with: channels for a compaction to hold two, the case one, and every worker thread
 *  one. */
constexpr fixture_config_t scdb_config_k {.shared_io_channels = 8};

/** Threads the concurrency cases drive the database on. */
constexpr std::size_t worker_threads_k = 4;

/** The largest value the variable-width databases under test admit. */
constexpr stored_value_size_t value_max_size_k = 4096;

/** The keys and the key slots every case of this suite drives. */
using stored_key_t = std::uint64_t;
using variable_slot_t = key_slot<stored_key_t, value_layout_t::variable_width_k>;

/** The databases and the scanners every case drives over the volume of @p fixture_type_. */
template <typename fixture_type_>
struct scdb_types {
    using fixture_t = fixture_type_;
    using guard_t = typename fixture_t::guard_t;
    using volume_t = typename fixture_t::volume_t;
    using variable_database_t = basic_single_collection_database<variable_slot_t, volume_t>;
    using variable_scanner_t = collection_scanner<variable_slot_t, volume_t>;

    /** A fixture, the channels a database needs, and the volume over them. */
    struct stack_t {

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
};

/**
 *  @brief The database shape a case opens, whose first level fills after a handful of flushes.
 *  @param[in] value_size Bytes every value takes, which is zero for every layout but the fixed one.
 *  @param[in] value_max_size Bytes the largest value takes, which is zero for a fixed-width layout.
 *  @param[in] level_zero_bytes Bytes level zero admits before a compaction is due.
 */
[[nodiscard]] single_collection_database_config_t test_config(stored_value_size_t value_size,
                                                              stored_value_size_t value_max_size,
                                                              std::uint64_t level_zero_bytes) noexcept {
    single_collection_database_config_t config;
    config.collection.concurrency_limit = 1;
    config.collection.cache_limit = 1024ull * 1024ull;
    config.collection.write_buffer_max_bytes = 8ull * 1024ull * 1024ull;
    config.collection.write_buffer_max_elements = 8192;
    config.collection.lsm_tree.level_enlarge_factor = 4;
    config.collection.lsm_tree.lsm_level_0.max_bytes = level_zero_bytes;
    config.collection.lsm_tree.lsm_level_0.sorted_run.value_size = value_size;
    config.collection.lsm_tree.lsm_level_0.sorted_run.value_max_size = value_max_size;
    config.default_transaction.write_buffer_max_bytes = 256ull * 1024ull;
    config.default_transaction.write_buffer_max_elements = 1024;
    return config;
}

/** Bytes one counter of the serializability case takes. */
constexpr std::size_t counter_bytes_k = 8;

/** Writes @p counter into @p into, little end first, so a read of it needs no alignment. */
void write_counter(std::uint64_t counter, std::span<byte_t> into) noexcept {
    for (std::size_t index = 0; index != counter_bytes_k; ++index)
        into[index] = static_cast<byte_t>((counter >> (index * 8)) & 0xFFull);
}

/** The counter @p from carries, which is zero for a span shorter than one. */
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

/** Opens a database of @p database_type_ over the volume and the channels of @p stack. */
template <typename database_type_, typename stack_type_>
expected<typename database_type_::database_unique_ptr_t> open_over(
    stack_type_& stack, single_collection_database_config_t const& config) noexcept {
    return database_type_::open(*stack.volume, stack.writing.channel(), stack.merging.channel(), config,
                                stack.fixture->resources);
}

#pragma endregion Fixtures

#pragma region Model

/** Writes every entry of @p wanted through @p store, one call per key. */
template <typename database_type_>
[[nodiscard]] std::size_t write_model(database_type_& store, std::span<expectation_t const> wanted) {
    std::size_t failures = 0;
    st::basic_vector<byte_t> value;
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        expectation_t const& entry = wanted[index];
        if (entry.state == select_result_t::erased_k) {
            failures += expect(store.erase(entry.stored_key).succeeded());
            continue;
        }
        failures += expect(value.resize(static_cast<std::size_t>(entry.bytes)) == st::status_t::success_k);
        for (std::uint64_t position = 0; position != entry.bytes; ++position)
            value[static_cast<std::size_t>(position)] = painted(entry.seed, position);
        failures += expect(store.insert(entry.stored_key, std::span<byte_t const> {value}).succeeded());
    }
    return failures;
}

/** Reads every key of @p wanted through one batched select and counts what the model disagrees with. */
template <typename readable_type_>
[[nodiscard]] std::size_t check_model(readable_type_ const& source, io_channel_t& channel,
                                      std::span<expectation_t const> wanted) {
    std::size_t failures = 0;
    std::size_t const count = wanted.size();
    st::basic_vector<stored_key_t> keys;
    st::basic_vector<select_result_t> results;
    st::basic_vector<std::uint64_t> seen;
    failures += expect(keys.resize(count) == st::status_t::success_k);
    failures += expect(results.resize(count) == st::status_t::success_k);
    failures += expect(seen.resize(count) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count; ++index) {
        keys[index] = wanted[index].stored_key;
        results[index] = select_result_t::nothing_k;
        seen[index] = 0;
    }

    std::size_t mismatches = 0;
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        seen[request] += bytes.size();
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(wanted[request].seed, offset + index)) ++mismatches;
    };
    failures += expect(
        source.select(channel, std::span<stored_key_t const> {keys}, std::span<select_result_t> {results}, sink)
            .succeeded());
    failures += expect_eq(mismatches, std::size_t {0});

    for (std::size_t index = 0; index != count; ++index) {
        failures += expect(results[index] == wanted[index].state);
        if (wanted[index].state == select_result_t::success_k) failures += expect_eq(seen[index], wanted[index].bytes);
    }
    return failures;
}

#pragma endregion Model

#pragma region Round Trip Cases

/** Writes and reads one database of @p key_slot_type_, which is how every layout is covered. */
template <typename key_slot_type_, typename mount_type_>
[[nodiscard]] std::size_t layout_round_trip(mount_type_ const& mount, stored_value_size_t value_size,
                                            stored_value_size_t value_max_size) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using database_t = basic_single_collection_database<key_slot_type_, typename types_t::volume_t>;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 300;

    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    single_collection_database_config_t const config = test_config(value_size, value_max_size,
                                                                   16ull * 1024ull * 1024ull);
    expected<typename database_t::database_unique_ptr_t> store = open_over<database_t>(**stack, config);
    failures += expect(store.has_value());
    if (!store) return failures;

    stored_value_size_t const widest = (*store)->value_max_size();
    st::basic_vector<expectation_t> model;
    failures += expect(model.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index) * 3;
        entry.state = index % 13 == 0 ? select_result_t::erased_k : select_result_t::success_k;
        entry.bytes = entry.state != select_result_t::success_k ? 0 : value_size != 0 ? value_size : 1 + index % widest;
        entry.seed = index + widest;
        model[index] = entry;
    }

    failures += write_model(**store, std::span<expectation_t const> {model});
    failures += check_model(**store, (*stack)->channel(), std::span<expectation_t const> {model});
    failures += expect((*store)->flush().succeeded());
    failures += expect_eq((*store)->staged_count(), std::size_t {0});
    failures += check_model(**store, (*stack)->channel(), std::span<expectation_t const> {model});
    return failures;
}

/** Every value layout the database serves round trips through the write buffer and the tree. */
template <typename mount_type_>
std::size_t every_layout_round_trips(mount_type_ const& mount) {
    std::size_t failures = 0;
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_1_byte_k>>(mount, 0, 0);
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_8_bytes_k>>(mount, 0, 0);
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::inline_64_bytes_k>>(mount, 0, 0);
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::fixed_width_k>>(mount, 512, 0);
    failures += layout_round_trip<key_slot<stored_key_t, value_layout_t::variable_width_k>>(mount, 0, value_max_size_k);
    return failures;
}

/** The name the schema blob of this layer answers to, which a crash case removes by hand. */
[[nodiscard]] blob_name_t schema_blob_name() noexcept {
    static constexpr char text[] = "single-collection-database.schema";
    return blob_name_t {reinterpret_cast<byte_t const*>(text), sizeof(text) - 1};
}

/** Sorted runs @p volume names right now, which a reopen neither adds to nor loses. */
template <typename volume_type_>
[[nodiscard]] std::size_t run_count_of(volume_type_ const& volume) noexcept {
    std::size_t counted = 0;
    volume.for_each(blob_kind_t::sorted_run_k, [&](blob_name_t, volume_blob_t const&) noexcept { ++counted; });
    return counted;
}

/** Bytes every sorted run of @p volume takes, which a schema that never landed leaves behind. */
template <typename volume_type_>
[[nodiscard]] std::uint64_t run_bytes_of(volume_type_ const& volume) noexcept {
    std::uint64_t counted = 0;
    volume.for_each(blob_kind_t::sorted_run_k,
                    [&](blob_name_t, volume_blob_t const& blob) noexcept { counted += blob.extent.length; });
    return counted;
}

/** Runs the schema blob on @p volume names, counted off the length its fixed-width records fix. */
template <typename volume_type_>
[[nodiscard]] std::size_t runs_named_by_schema(volume_type_ const& volume) noexcept {
    expected<volume_blob_t> const found = volume.find(schema_blob_name());
    if (!found || found->length < sizeof(single_collection_database_schema_record_t)) return 0;
    return static_cast<std::size_t>((found->length - sizeof(single_collection_database_schema_record_t)) /
                                    sizeof(lsm_tree_run_record_t));
}

/**
 *  @brief Writes a model of @p key_slot_type_, closes over its volume and reads every byte back after a reopen.
 *  @param[in] value_size Bytes every value takes, which is zero for every layout but the fixed one.
 *  @param[in] value_max_size Bytes the largest value takes, which is zero for a fixed-width layout.
 *  @param[in] level_zero_bytes Bytes level zero admits, low enough that two flushed rounds overfill it.
 *
 *  The tree is given depth before the close - two flushed rounds merged into a second level, and a third
 *  round flushed over them - so a reopen that took back only level zero would lose the keys the merge buried.
 */
template <typename key_slot_type_, typename mount_type_>
[[nodiscard]] std::size_t layout_survives_reopen(mount_type_ const& mount, stored_value_size_t value_size,
                                                 stored_value_size_t value_max_size, std::uint64_t level_zero_bytes) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using database_t = basic_single_collection_database<key_slot_type_, typename types_t::volume_t>;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 400;

    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    single_collection_database_config_t const config = test_config(value_size, value_max_size, level_zero_bytes);
    st::basic_vector<expectation_t> model;
    failures += expect(model.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;

    std::size_t depth = 0;
    std::size_t shallowest_runs = 0;
    std::uint64_t population = 0;
    {
        expected<typename database_t::database_unique_ptr_t> store = open_over<database_t>(**stack, config);
        failures += expect(store.has_value());
        if (!store) return failures;
        stored_value_size_t const widest = (*store)->value_max_size();

        for (std::uint64_t round = 0; round != 2; ++round) {
            for (std::size_t index = 0; index != count_k; ++index) {
                expectation_t entry;
                entry.stored_key = static_cast<stored_key_t>(index) * 3;
                entry.state = select_result_t::success_k;
                entry.bytes = value_size != 0 ? value_size : 1 + index % widest;
                entry.seed = index + round * 7919;
                model[index] = entry;
            }
            failures += write_model(**store, std::span<expectation_t const> {model});
            failures += expect((*store)->flush().succeeded());
        }

        expected<compaction_outcome_t> const merged = (*store)->compact();
        failures += expect(merged.has_value());
        if (!merged) return failures;
        failures += expect(*merged == compaction_outcome_t::published_k);

        // A merge records its new set before it drops what it consumed, so neither outlives the other.
        failures += expect_eq(runs_named_by_schema(*(*stack)->volume), run_count_of(*(*stack)->volume));

        // Two overlapping rounds go back onto level zero, so age alone decides which run answers a key,
        // and the keys neither round names can only be answered from the level the merge wrote.
        for (std::uint64_t again = 0; again != 2; ++again) {
            st::basic_vector<expectation_t> batch;
            std::size_t const stride = again == 0 ? 3 : 6;
            for (std::size_t index = 0; index < count_k; index += stride) {
                expectation_t entry = model[index];
                entry.state = again == 0 && index % 9 == 0 ? select_result_t::erased_k : select_result_t::success_k;
                entry.bytes = entry.state != select_result_t::success_k ? 0
                              : value_size != 0                         ? value_size
                                                                        : 1 + (index + 5 + again) % widest;
                entry.seed = index + 104729 + again * 31;
                model[index] = entry;
                failures += expect(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
            }
            failures += write_model(**store, std::span<expectation_t const> {batch});
            failures += expect((*store)->flush().succeeded());
        }
        failures += check_model(**store, (*stack)->channel(), std::span<expectation_t const> {model});

        depth = (*store)->records().tree().snapshot().size();
        shallowest_runs = (*store)->records().tree().snapshot().level(0).size();
        population = (*store)->population_count();
        failures += expect(depth > std::size_t {1});
        failures += expect(shallowest_runs > std::size_t {1});
        if (failures != 0) return failures;
    }

    expected<typename database_t::database_unique_ptr_t> store = open_over<database_t>(**stack, config);
    failures += expect(store.has_value());
    if (!store) return failures;
    failures += expect_eq((*store)->records().tree().snapshot().size(), depth);
    failures += expect_eq((*store)->records().tree().snapshot().level(0).size(), shallowest_runs);
    failures += expect_eq((*store)->population_count(), population);
    failures += check_model(**store, (*stack)->channel(), std::span<expectation_t const> {model});
    return failures;
}

/** Every layout the database serves comes back byte for byte, and out of every level, after a reopen. */
template <typename mount_type_>
std::size_t every_layout_survives_reopen(mount_type_ const& mount) {
    std::size_t failures = 0;
    failures += layout_survives_reopen<key_slot<stored_key_t, value_layout_t::inline_8_bytes_k>>(mount, 0, 0,
                                                                                                 8ull * 1024ull);
    failures += layout_survives_reopen<key_slot<stored_key_t, value_layout_t::fixed_width_k>>(mount, 512, 0,
                                                                                              64ull * 1024ull);
    failures += layout_survives_reopen<key_slot<stored_key_t, value_layout_t::variable_width_k>>(
        mount, 0, value_max_size_k, 64ull * 1024ull);
    return failures;
}

/**
 *  @brief A schema that never reached the medium wastes its runs, and one naming a missing run is refused.
 *
 *  A flush seals the run first and records it second, so the only state a crash between the two leaves
 *  is a run nobody names. The other order would leave a schema naming bytes the volume never got, which
 *  the second half forces by hand to show what a reopen does with it.
 */
template <typename mount_type_>
std::size_t crash_between_seal_and_schema(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using variable_database_t = typename types_t::variable_database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    single_collection_database_config_t const config = test_config(0, value_max_size_k, 16ull * 1024ull * 1024ull);

    {
        expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
        failures += expect_status(stack, status_code_t::success_k);
        if (!stack) return failures;

        {
            expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(
                **stack, config);
            failures += expect(store.has_value());
            if (!store) return failures;
            st::basic_vector<byte_t> value;
            failures += expect(value.resize(200) == st::status_t::success_k);
            for (std::size_t index = 0; index != value.size(); ++index) value[index] = painted(5, index);
            for (stored_key_t key = 0; key != 64; ++key)
                failures += expect((*store)->insert(key, std::span<byte_t const> {value}).succeeded());
            failures += expect((*store)->flush().succeeded());

            // A flush seals its run and records it second, so the schema already names every run on the volume.
            failures += expect_eq(runs_named_by_schema(*(*stack)->volume), run_count_of(*(*stack)->volume));
        }

        std::size_t const runs = run_count_of(*(*stack)->volume);
        std::uint64_t const wasted = run_bytes_of(*(*stack)->volume);
        failures += expect(runs != std::size_t {0});
        failures += expect(wasted != std::uint64_t {0});

        // The schema write that would have named those runs never landed.
        failures += expect((*stack)->volume->drop((*stack)->writing.channel(), schema_blob_name()).succeeded());

        expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                             config);
        failures += expect(store.has_value());
        if (!store) return failures;
        failures += expect_eq((*store)->population_count(), std::uint64_t {0});
        failures += expect_eq(run_count_of(*(*stack)->volume), runs);
        failures += expect_eq(run_bytes_of(*(*stack)->volume), wasted);

        // The volume is still a volume: a write over it flushes and answers.
        st::basic_vector<byte_t> value;
        failures += expect(value.resize(64) == st::status_t::success_k);
        for (std::size_t index = 0; index != value.size(); ++index) value[index] = painted(9, index);
        failures += expect((*store)->insert(stored_key_t {1000}, std::span<byte_t const> {value}).succeeded());
        failures += expect((*store)->flush().succeeded());
        st::basic_vector<byte_t> back;
        failures += expect(back.resize(value.size()) == st::status_t::success_k);
        stored_value_size_t size = 0;
        failures += expect(
            (*store)->select((*stack)->channel(), stored_key_t {1000}, std::span<byte_t> {back}, size).succeeded());
        failures += expect_eq(std::size_t {size}, value.size());
        failures += expect_eq(std::memcmp(back.data(), value.data(), value.size()), 0);
    }

    {
        expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
        failures += expect_status(stack, status_code_t::success_k);
        if (!stack) return failures;

        {
            expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(
                **stack, config);
            failures += expect(store.has_value());
            if (!store) return failures;
            st::basic_vector<byte_t> value;
            failures += expect(value.resize(200) == st::status_t::success_k);
            for (std::size_t index = 0; index != value.size(); ++index) value[index] = painted(5, index);
            for (stored_key_t key = 0; key != 64; ++key)
                failures += expect((*store)->insert(key, std::span<byte_t const> {value}).succeeded());
            failures += expect((*store)->flush().succeeded());
        }

        std::array<byte_t, blob_name_max_bytes_k> named {};
        std::size_t named_bytes = 0;
        (*stack)->volume->for_each(blob_kind_t::sorted_run_k, [&](blob_name_t name, volume_blob_t const&) noexcept {
            if (named_bytes != 0) return;
            std::memcpy(named.data(), name.data(), name.size());
            named_bytes = name.size();
        });
        failures += expect(named_bytes != std::size_t {0});
        if (named_bytes == 0) return failures;

        // The bytes of the one run the schema names never reached the environment.
        failures += expect(
            (*stack)->volume->drop((*stack)->writing.channel(), blob_name_t {named.data(), named_bytes}).succeeded());

        expected<typename variable_database_t::database_unique_ptr_t> const store = open_over<variable_database_t>(
            **stack, config);
        failures += expect_status(store, status_code_t::not_found_k);
    }
    return failures;
}

/** A reopen takes back what the database itself wrote to the volume, and refuses another config. */
template <typename mount_type_>
std::size_t reopen_agrees_on_its_schema(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using variable_database_t = typename types_t::variable_database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    single_collection_database_config_t const config = test_config(0, value_max_size_k, 16ull * 1024ull * 1024ull);
    st::basic_vector<byte_t> owner_bytes;
    failures += expect(owner_bytes.resize(3000) == st::status_t::success_k);
    for (std::size_t index = 0; index != owner_bytes.size(); ++index) owner_bytes[index] = painted(77, index);

    {
        expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                             config);
        failures += expect(store.has_value());
        if (!store) return failures;

        st::basic_vector<byte_t> value;
        failures += expect(value.resize(200) == st::status_t::success_k);
        for (std::size_t index = 0; index != value.size(); ++index) value[index] = painted(5, index);
        for (stored_key_t key = 0; key != 64; ++key)
            failures += expect((*store)->insert(key, std::span<byte_t const> {value}).succeeded());
        failures += expect((*store)->flush().succeeded());
        failures += expect((*store)->save_user_data(std::span<byte_t const> {owner_bytes}).succeeded());
        failures += expect_eq((*store)->user_data_size_bytes(), std::uint64_t {owner_bytes.size()});
    }

    // The runs the flush published are still named on the volume the database was closed over.
    std::size_t const runs = run_count_of(*(*stack)->volume);
    failures += expect(runs != 0);

    {
        expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                             config);
        failures += expect(store.has_value());
        if (!store) return failures;

        failures += expect_eq((*store)->user_data_size_bytes(), std::uint64_t {owner_bytes.size()});
        st::basic_vector<byte_t> back;
        failures += expect(back.resize(owner_bytes.size()) == st::status_t::success_k);
        failures += expect((*store)->load_user_data((*stack)->channel(), std::span<byte_t> {back}).succeeded());
        failures += expect_eq(std::memcmp(back.data(), owner_bytes.data(), owner_bytes.size()), 0);

        // The runs above are back in the tree, so every key the close left behind answers again.
        failures += expect_eq((*store)->population_count(), std::uint64_t {64});
        failures += expect_eq(run_count_of(*(*stack)->volume), runs);

        failures += expect((*store)->clear_user_data().succeeded());
        failures += expect_eq((*store)->user_data_size_bytes(), std::uint64_t {0});
    }

    // A volume whose schema names one shape refuses a database of another.
    single_collection_database_config_t narrower = config;
    narrower.collection.lsm_tree.lsm_level_0.sorted_run.value_max_size = value_max_size_k / 2;
    expected<typename variable_database_t::database_unique_ptr_t> refused = open_over<variable_database_t>(**stack,
                                                                                                           narrower);
    failures += expect_status(refused, status_code_t::invalid_argument_k);
    return failures;
}

#pragma endregion Round Trip Cases

#pragma region Transaction Cases

/**
 *  @brief A reader passing over every key sees the whole of a commit or none of it.
 *
 *  Every pass is one batched select, which holds the read path for all of its keys, so a pass that
 *  carried two generations at once would be a commit landing in the middle of a reader.
 */
template <typename mount_type_>
std::size_t commit_lands_all_or_nothing(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using guard_t = typename types_t::guard_t;
    using variable_database_t = typename types_t::variable_database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 48;
    constexpr std::size_t value_bytes_k = 96;
    constexpr std::size_t commits_k = 120;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    single_collection_database_config_t const config = test_config(0, value_max_size_k, 64ull * 1024ull * 1024ull);
    expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                         config);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::basic_vector<byte_t> value;
    failures += expect(value.resize(value_bytes_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != value_bytes_k; ++index) value[index] = painted(1, index);
    for (stored_key_t key = 0; key != keys_k; ++key)
        failures += expect((*store)->insert(key, std::span<byte_t const> {value}).succeeded());
    failures += expect((*store)->flush().succeeded());
    if (failures != 0) return failures;

    std::size_t writer_failures = 0;
    std::thread writer {[&]() noexcept {
        expected<guard_t> guard = (*stack)->fixture->channels.acquire();
        writer_failures += expect(guard.has_value());
        if (!guard) return;

        expected<typename variable_database_t::transaction_unique_ptr_t> changing = (*store)->create_transaction(7);
        writer_failures += expect(changing.has_value());
        if (!changing) return;

        st::basic_vector<byte_t> painted_value;
        writer_failures += expect(painted_value.resize(value_bytes_k) == st::status_t::success_k);
        for (std::size_t round = 0; round != commits_k; ++round) {
            std::uint64_t const generation = 1 + round % 2;
            for (std::size_t index = 0; index != value_bytes_k; ++index)
                painted_value[index] = painted(generation, index);
            for (stored_key_t key = 0; key != keys_k; ++key)
                writer_failures += expect(
                    (*changing)->insert(key, std::span<byte_t const> {painted_value}).succeeded());
            writer_failures += expect((*changing)->commit().succeeded());
        }
    }};

    std::size_t torn = 0;
    std::size_t unknown = 0;
    std::size_t passes = 0;
    st::basic_vector<stored_key_t> keys;
    st::basic_vector<select_result_t> results;
    st::basic_vector<byte_t> carried;
    failures += expect(keys.resize(keys_k) == st::status_t::success_k);
    failures += expect(results.resize(keys_k) == st::status_t::success_k);
    failures += expect(carried.resize(keys_k * value_bytes_k) == st::status_t::success_k);
    for (stored_key_t key = 0; key != keys_k; ++key) keys[static_cast<std::size_t>(key)] = key;

    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        for (std::size_t index = 0; index != bytes.size(); ++index)
            carried[request * value_bytes_k + static_cast<std::size_t>(offset) + index] = bytes[index];
    };
    while (passes != 400) {
        failures += expect((*store)
                               ->select((*stack)->channel(), std::span<stored_key_t const> {keys},
                                        std::span<select_result_t> {results}, sink)
                               .succeeded());
        std::size_t generations = 0;
        for (std::uint64_t generation = 1; generation != 3; ++generation) {
            std::size_t matching = 0;
            for (std::size_t request = 0; request != keys_k; ++request) {
                std::size_t same = 0;
                for (std::size_t index = 0; index != value_bytes_k; ++index)
                    same += carried[request * value_bytes_k + index] == painted(generation, index);
                matching += same == value_bytes_k;
            }
            generations += matching != 0;
            if (matching != 0 && matching != keys_k) ++torn;
        }
        unknown += generations == 0;
        ++passes;
    }
    writer.join();
    failures += writer_failures;
    failures += expect_eq(torn, std::size_t {0});
    failures += expect_eq(unknown, std::size_t {0});
    return failures;
}

/** A read-write conflict refuses the stage and leaves the changes where a caller can still log them. */
template <typename mount_type_>
std::size_t conflict_keeps_its_changes(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using variable_database_t = typename types_t::variable_database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr stored_key_t contested_k = 11;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    single_collection_database_config_t const config = test_config(0, value_max_size_k, 64ull * 1024ull * 1024ull);
    expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                         config);
    failures += expect(store.has_value());
    if (!store) return failures;

    std::array<byte_t, counter_bytes_k> original {};
    write_counter(100, std::span<byte_t> {original});
    failures += expect((*store)->insert(contested_k, std::span<byte_t const> {original}).succeeded());
    failures += expect((*store)->flush().succeeded());

    expected<typename variable_database_t::transaction_unique_ptr_t> first = (*store)->create_transaction(1);
    expected<typename variable_database_t::transaction_unique_ptr_t> second = (*store)->create_transaction(2);
    failures += expect(first.has_value() && second.has_value());
    if (!first || !second) return failures;

    // Both read the key before either writes it, which is the shape a lost update takes.
    std::array<byte_t, counter_bytes_k> seen {};
    stored_value_size_t size = 0;
    failures += expect((*first)->select((*stack)->channel(), contested_k, std::span<byte_t> {seen}, size).succeeded());
    failures += expect_eq(read_counter(std::span<byte_t const> {seen}), std::uint64_t {100});
    failures += expect((*second)->select((*stack)->channel(), contested_k, std::span<byte_t> {seen}, size).succeeded());

    std::array<byte_t, counter_bytes_k> from_first {};
    std::array<byte_t, counter_bytes_k> from_second {};
    write_counter(101, std::span<byte_t> {from_first});
    write_counter(202, std::span<byte_t> {from_second});
    failures += expect((*first)->insert(contested_k, std::span<byte_t const> {from_first}).succeeded());
    failures += expect((*second)->insert(contested_k, std::span<byte_t const> {from_second}).succeeded());

    failures += expect((*first)->commit().succeeded());
    status_t const refused = (*second)->stage();
    failures += expect_status(refused, status_code_t::write_conflict_k);

    // A stage that was not refused holds the write path, and every check below reads through it.
    if (refused.succeeded()) (*second)->rollback();

    // The refused stage keeps the changes, so the caller can still read back what it could not commit.
    stored_value_size_t kept = 0;
    failures += expect((*second)->find_uncommitted(contested_k, kept) == select_result_t::success_k);
    failures += expect_eq(static_cast<std::size_t>(kept), counter_bytes_k);
    failures += expect((*second)->select((*stack)->channel(), contested_k, std::span<byte_t> {seen}, size).succeeded());
    failures += expect_eq(read_counter(std::span<byte_t const> {seen}), std::uint64_t {202});

    // The database still carries only what the winner wrote.
    failures += expect((*store)->select((*stack)->channel(), contested_k, std::span<byte_t> {seen}, size).succeeded());
    failures += expect_eq(read_counter(std::span<byte_t const> {seen}), std::uint64_t {101});

    // Rolling back and doing the work again is what the loser is supposed to do next.
    (*second)->rollback();
    failures += expect((*second)->find_uncommitted(contested_k, kept) == select_result_t::nothing_k);
    failures += expect((*second)->select((*stack)->channel(), contested_k, std::span<byte_t> {seen}, size).succeeded());
    write_counter(read_counter(std::span<byte_t const> {seen}) + 1, std::span<byte_t> {from_second});
    failures += expect((*second)->insert(contested_k, std::span<byte_t const> {from_second}).succeeded());
    failures += expect((*second)->commit().succeeded());
    failures += expect((*store)->select((*stack)->channel(), contested_k, std::span<byte_t> {seen}, size).succeeded());
    failures += expect_eq(read_counter(std::span<byte_t const> {seen}), std::uint64_t {102});

    // A transaction that only read the contested key, and wrote elsewhere, is refused just the same.
    expected<typename variable_database_t::transaction_unique_ptr_t> observing = (*store)->create_transaction(4);
    expected<typename variable_database_t::transaction_unique_ptr_t> overwriting = (*store)->create_transaction(5);
    failures += expect(observing.has_value() && overwriting.has_value());
    if (!observing || !overwriting) return failures;

    failures += expect(
        (*observing)->select((*stack)->channel(), contested_k, std::span<byte_t> {seen}, size).succeeded());
    failures += expect((*observing)->insert(stored_key_t {99}, std::span<byte_t const> {from_first}).succeeded());
    failures += expect((*overwriting)->insert(contested_k, std::span<byte_t const> {from_second}).succeeded());
    failures += expect((*overwriting)->commit().succeeded());
    status_t const observed = (*observing)->stage();
    failures += expect_status(observed, status_code_t::write_conflict_k);
    if (observed.succeeded()) (*observing)->rollback();
    failures += expect((*store)->find(stored_key_t {99}, size) == select_result_t::nothing_k);
    return failures;
}

/** A rollback drops the changes, and a renew keeps them but only for a transaction that has read nothing. */
template <typename mount_type_>
std::size_t rollback_and_renew_behave(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using variable_database_t = typename types_t::variable_database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    single_collection_database_config_t const config = test_config(0, value_max_size_k, 64ull * 1024ull * 1024ull);
    expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                         config);
    failures += expect(store.has_value());
    if (!store) return failures;

    expected<typename variable_database_t::transaction_unique_ptr_t> changing = (*store)->create_transaction(3);
    failures += expect(changing.has_value());
    if (!changing) return failures;

    std::array<byte_t, counter_bytes_k> value {};
    write_counter(7, std::span<byte_t> {value});
    stored_value_size_t size = 0;

    // A rollback leaves neither the change nor a trace of it in the database.
    failures += expect((*changing)->insert(stored_key_t {1}, std::span<byte_t const> {value}).succeeded());
    (*changing)->rollback();
    failures += expect((*changing)->find_uncommitted(stored_key_t {1}, size) == select_result_t::nothing_k);
    failures += expect((*store)->find(stored_key_t {1}, size) == select_result_t::nothing_k);

    // A renew keeps the changes of a transaction that has only written.
    failures += expect((*changing)->insert(stored_key_t {2}, std::span<byte_t const> {value}).succeeded());
    failures += expect((*changing)->renew().succeeded());
    failures += expect((*changing)->find_uncommitted(stored_key_t {2}, size) == select_result_t::success_k);
    failures += expect((*changing)->commit().succeeded());
    failures += expect((*store)->find(stored_key_t {2}, size) == select_result_t::success_k);

    // A transaction that has read something may not renew, whatever it has written since.
    std::array<byte_t, counter_bytes_k> seen {};
    failures += expect(
        (*changing)->select((*stack)->channel(), stored_key_t {2}, std::span<byte_t> {seen}, size).succeeded());
    failures += expect_status((*changing)->renew(), status_code_t::invalid_argument_k);

    // Nor may a staged transaction, and the staging queue pair is handed back by the rollback and not by the renew.
    (*changing)->rollback();
    failures += expect((*changing)->insert(stored_key_t {3}, std::span<byte_t const> {value}).succeeded());
    failures += expect((*changing)->stage().succeeded());
    failures += expect_status((*changing)->renew(), status_code_t::invalid_argument_k);
    (*changing)->rollback();
    failures += expect((*store)->find(stored_key_t {3}, size) == select_result_t::nothing_k);

    // The transaction is running again after all of that, so it still commits.
    failures += expect((*changing)->insert(stored_key_t {4}, std::span<byte_t const> {value}).succeeded());
    failures += expect((*changing)->commit().succeeded());
    failures += expect((*store)->find(stored_key_t {4}, size) == select_result_t::success_k);
    return failures;
}

/**
 *  @brief Several threads running transactions over overlapping keys end where no update was lost.
 *
 *  Every round reads a counter, writes it back raised by one and commits, retrying what a conflict
 *  refuses. A lost update would leave the counters short of the commits that landed.
 */
template <typename mount_type_>
std::size_t transactions_serialize_under_threads(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using guard_t = typename types_t::guard_t;
    using variable_database_t = typename types_t::variable_database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 4;
    constexpr std::size_t rounds_k = 60;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    single_collection_database_config_t const config = test_config(0, value_max_size_k, 64ull * 1024ull * 1024ull);
    expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                         config);
    failures += expect(store.has_value());
    if (!store) return failures;

    std::array<byte_t, counter_bytes_k> zero {};
    write_counter(0, std::span<byte_t> {zero});
    for (stored_key_t key = 0; key != keys_k; ++key)
        failures += expect((*store)->insert(key, std::span<byte_t const> {zero}).succeeded());
    failures += expect((*store)->flush().succeeded());
    if (failures != 0) return failures;

    std::array<std::size_t, worker_threads_k> landed {};
    std::array<std::size_t, worker_threads_k> worker_failures {};
    std::array<std::thread, worker_threads_k> workers;
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker)
        workers[worker] = std::thread {[&, worker]() noexcept {
            expected<guard_t> guard = (*stack)->fixture->channels.acquire();
            worker_failures[worker] += expect(guard.has_value());
            if (!guard) return;

            expected<typename variable_database_t::transaction_unique_ptr_t> changing = (*store)->create_transaction(
                static_cast<transaction_id_t>(worker + 1));
            worker_failures[worker] += expect(changing.has_value());
            if (!changing) return;

            std::array<byte_t, counter_bytes_k> seen {};
            std::array<byte_t, counter_bytes_k> raised {};
            for (std::size_t round = 0; round != rounds_k; ++round) {
                stored_key_t const key = static_cast<stored_key_t>((round + worker) % keys_k);
                for (std::size_t attempt = 0; attempt != 256; ++attempt) {
                    stored_value_size_t size = 0;
                    if ((*changing)->select(guard->channel(), key, std::span<byte_t> {seen}, size).failed()) {
                        ++worker_failures[worker];
                        break;
                    }
                    write_counter(read_counter(std::span<byte_t const> {seen}) + 1, std::span<byte_t> {raised});
                    if ((*changing)->insert(key, std::span<byte_t const> {raised}).failed()) {
                        ++worker_failures[worker];
                        break;
                    }
                    status_t const committed = (*changing)->commit();
                    if (committed.succeeded()) {
                        ++landed[worker];
                        break;
                    }
                    if (!(committed == status_code_t::write_conflict_k)) ++worker_failures[worker];
                    (*changing)->rollback();
                }
            }
        }};
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker) workers[worker].join();

    std::size_t commits = 0;
    for (std::size_t worker = 0; worker != worker_threads_k; ++worker) {
        failures += worker_failures[worker];
        commits += landed[worker];
    }
    failures += expect_eq(commits, worker_threads_k * rounds_k);

    std::uint64_t counted = 0;
    for (stored_key_t key = 0; key != keys_k; ++key) {
        std::array<byte_t, counter_bytes_k> seen {};
        stored_value_size_t size = 0;
        failures += expect((*store)->select((*stack)->channel(), key, std::span<byte_t> {seen}, size).succeeded());
        counted += read_counter(std::span<byte_t const> {seen});
    }
    failures += expect_eq(counted, std::uint64_t {commits});
    return failures;
}

#pragma endregion Transaction Cases

#pragma region Snapshot Cases

/**
 *  @brief A snapshot answers the moment it was taken at while another thread publishes over it.
 *
 *  The writer blocks inside its own publication until the snapshot is let go, which is what makes the
 *  reads in the middle meaningful: the level set the snapshot holds cannot have been retired under it.
 *  Nothing on this thread may touch the database until the writer is joined, since the writer holds the
 *  write path for the whole of the flush it is blocked in.
 */
template <typename mount_type_>
std::size_t snapshot_sees_its_moment(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using guard_t = typename types_t::guard_t;
    using variable_database_t = typename types_t::variable_database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t bytes_k = 700;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    single_collection_database_config_t const config = test_config(0, value_max_size_k, 64ull * 1024ull * 1024ull);
    expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                         config);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::basic_vector<byte_t> older;
    st::basic_vector<byte_t> newer;
    failures += expect(older.resize(bytes_k) == st::status_t::success_k);
    failures += expect(newer.resize(bytes_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != bytes_k; ++index) older[index] = painted(11, index);
    for (std::size_t index = 0; index != bytes_k; ++index) newer[index] = painted(12, index);

    for (stored_key_t key = 0; key != 8; ++key)
        failures += expect((*store)->insert(key, std::span<byte_t const> {older}).succeeded());

    // A snapshot flushes first, so what was staged is inside it and nothing is left behind.
    expected<typename variable_database_t::snapshot_t> taken = (*store)->take_snapshot();
    failures += expect(taken.has_value());
    if (!taken) return failures;
    failures += expect_eq(taken->population_count(), std::uint64_t {8});
    failures += expect_eq((*store)->staged_count(), std::size_t {0});

    std::size_t writer_failures = 0;
    std::thread writer {[&]() noexcept {
        expected<guard_t> guard = (*stack)->fixture->channels.acquire();
        writer_failures += expect(guard.has_value());
        if (!guard) return;
        for (stored_key_t key = 0; key != 8; ++key)
            writer_failures += expect((*store)->insert(key, std::span<byte_t const> {newer}).succeeded());
        writer_failures += expect((*store)->insert(stored_key_t {100}, std::span<byte_t const> {newer}).succeeded());
        writer_failures += expect((*store)->flush().succeeded());
    }};

    st::basic_vector<byte_t> into;
    failures += expect(into.resize(bytes_k) == st::status_t::success_k);
    for (stored_key_t key = 0; key != 8; ++key) {
        stored_value_size_t size = 0;
        failures += expect(taken->select((*stack)->channel(), key, std::span<byte_t> {into}, size).succeeded());
        failures += expect_eq(static_cast<std::size_t>(size), bytes_k);
        failures += expect_eq(std::memcmp(into.data(), older.data(), bytes_k), 0);
    }

    // A key written after the snapshot was taken is not a key the snapshot has.
    stored_value_size_t missing = 0;
    failures += expect(taken->find(stored_key_t {100}, missing) == select_result_t::nothing_k);

    // Letting the hold go is what lets the writer's publication retire the set it replaced.
    taken = expected<typename variable_database_t::snapshot_t> {status_t {status_code_t::uninitialized_k, "Let go"}};
    writer.join();
    failures += writer_failures;
    failures += expect((*store)->find(stored_key_t {100}, missing) == select_result_t::success_k);

    // The database itself moved on, and every key of it now carries the newer bytes.
    for (stored_key_t key = 0; key != 8; ++key) {
        stored_value_size_t size = 0;
        failures += expect((*store)->select((*stack)->channel(), key, std::span<byte_t> {into}, size).succeeded());
        failures += expect_eq(std::memcmp(into.data(), newer.data(), bytes_k), 0);
    }
    return failures;
}

#pragma endregion Snapshot Cases

#pragma region Scan Cases

/** An ordered scan visits every key once, across the staged writes and several levels. */
template <typename mount_type_>
std::size_t scan_visits_every_key_in_order(mount_type_ const& mount) {
    using types_t = scdb_types<typename mount_type_::fixture_t>;
    using variable_database_t = typename types_t::variable_database_t;
    using variable_scanner_t = typename types_t::variable_scanner_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t keys_k = 800;
    constexpr std::size_t rounds_k = 4;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    single_collection_database_config_t const config = test_config(0, value_max_size_k, 64ull * 1024ull);
    expected<typename variable_database_t::database_unique_ptr_t> store = open_over<variable_database_t>(**stack,
                                                                                                         config);
    failures += expect(store.has_value());
    if (!store) return failures;

    st::basic_vector<expectation_t> model;
    failures += expect(model.resize(keys_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != keys_k; ++index) model[index].stored_key = static_cast<stored_key_t>(index);

    auto apply = [&](std::span<expectation_t const> batch) noexcept {
        for (std::size_t index = 0; index != batch.size(); ++index)
            model[static_cast<std::size_t>(batch[index].stored_key)] = batch[index];
    };

    for (std::size_t round = 0; round != rounds_k; ++round) {
        st::basic_vector<expectation_t> batch;
        for (std::size_t index = round; index < keys_k; index += rounds_k) {
            expectation_t entry;
            entry.stored_key = static_cast<stored_key_t>(index);
            entry.state = select_result_t::success_k;
            entry.bytes = 40 + index % 97;
            entry.seed = index * 31 + round;
            failures += expect(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
        }
        failures += write_model(**store, std::span<expectation_t const> {batch});
        failures += expect((*store)->flush().succeeded());
        apply(std::span<expectation_t const> {batch});
        if (failures != 0) return failures;
    }

    // One merge puts a second level under the first, so the scan picks between levels as well as runs.
    failures += expect((*store)->needs_compaction());
    expected<compaction_outcome_t> const merged = (*store)->compact();
    failures += expect(merged.has_value());
    if (!merged) return failures;
    failures += expect(*merged == compaction_outcome_t::published_k);

    // What stays staged is what the scan must merge in front of every run.
    {
        st::basic_vector<expectation_t> batch;
        for (std::size_t index = 0; index < keys_k; index += 25) {
            expectation_t entry;
            entry.stored_key = static_cast<stored_key_t>(index);
            entry.state = index % 50 == 0 ? select_result_t::erased_k : select_result_t::success_k;
            entry.bytes = entry.state == select_result_t::success_k ? 13 + index % 29 : 0;
            entry.seed = index * 7 + 3;
            failures += expect(batch.push_back(expectation_t {entry}) == st::status_t::success_k);
        }
        failures += write_model(**store, std::span<expectation_t const> {batch});
        apply(std::span<expectation_t const> {batch});
    }
    failures += expect((*store)->staged_count() != 0);

    expected<variable_scanner_t> scan = variable_scanner_t::over((*store)->records());
    failures += expect(scan.has_value());
    if (!scan) return failures;

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
        scan->advance();
    }
    failures += expect_eq(visited, keys_k);
    failures += expect_eq(out_of_order, std::size_t {0});
    failures += expect_eq(wrong_state, std::size_t {0});
    failures += expect_eq(wrong_size, std::size_t {0});

    failures += check_model(**store, (*stack)->channel(), std::span<expectation_t const> {model});
    return failures;
}

#pragma endregion Scan Cases

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
        report(totals, mount, "every_layout", every_layout_round_trips);
        report(totals, mount, "reopen_agrees", reopen_agrees_on_its_schema);
        report(totals, mount, "every_layout_survives_reopen", every_layout_survives_reopen);
        report(totals, mount, "crash_before_schema", crash_between_seal_and_schema);
        report(totals, mount, "commit_all_or_nothing", commit_lands_all_or_nothing);
        report(totals, mount, "conflict_keeps_changes", conflict_keeps_its_changes);
        report(totals, mount, "rollback_and_renew", rollback_and_renew_behave);
        report(totals, mount, "serializes_under_threads", transactions_serialize_under_threads);
        report(totals, mount, "snapshot_holds_its_moment", snapshot_sees_its_moment);
        report(totals, mount, "scan_in_order", scan_visits_every_key_in_order);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, scdb_config_k, 1, "scdb", cases);

    return suite_exit_code(totals);
}
