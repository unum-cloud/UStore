/**
 *  @file test/nvme/single_collection_database_transaction.cpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Unit tests for transactions over a single UStoreNVMe single-collection database.
 *
 *  Every case runs on every value layout and every transport: transactions are created and refused by ID,
 *  insert and remove alone, in pairs over disjoint and over intersecting keys, from two threads at once, watch
 *  reads and writes on every schedule, and stage before they commit, roll back or die.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcmp`

#include <array>       // `std::array`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <thread>      // `std::thread`
#include <utility>     // `std::move`

#include <fmt/format.h>

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/blobs/nvme_single_collection_database.hpp" // `basic_single_collection_database`, `watch_schedule_t`
#include "ustore/blobs/nvme_key_slot.hpp"                   // `key_slot`, `value_layout_t`

#include "nvme/fixture.hpp" // `run_on_transport`, `expectation_t`, `painted`, `paint_into`

namespace unum::ustore::test {

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Shape

/** The numbers every case mounts with: channels for a compaction to hold two, the case one, and spares. */
constexpr fixture_config_t transactions_config_k {.shared_io_channels = 6};

/** Keys every case writes, split in halves between transactions, whose disjoint halves never conflict. */
constexpr std::size_t population_k = 1000;

/** Every this many values is empty in the layouts whose values vary in width. */
constexpr std::size_t empty_value_step_k = 10;

/** The keys every case of this suite drives. */
using stored_key_t = std::uint64_t;

/**
 *  @brief One value layout a case runs on, with the value widths its database is opened with.
 *  @tparam key_slot_type_ The key slot the layout stores.
 *  @tparam value_size_ Bytes of every value, which is zero for every layout but the fixed one.
 *  @tparam value_max_size_ Bytes of the largest value, which only the variable-width layout sets.
 */
template <typename key_slot_type_, stored_value_size_t value_size_, stored_value_size_t value_max_size_>
struct layout {
    using key_slot_t = key_slot_type_;
    static constexpr stored_value_size_t value_size_k = value_size_;
    static constexpr stored_value_size_t value_max_size_k = value_max_size_;
};

using inline_1_byte_layout_t = layout<key_slot<stored_key_t, value_layout_t::inline_1_byte_k>, 0, 0>;
using inline_64_bytes_layout_t = layout<key_slot<stored_key_t, value_layout_t::inline_64_bytes_k>, 0, 0>;
using fixed_width_layout_t = layout<key_slot<stored_key_t, value_layout_t::fixed_width_k>, 4097, 0>;
using variable_width_layout_t = layout<key_slot<stored_key_t, value_layout_t::variable_width_k>, 0, 4096>;

/** The database and the stack every case drives over the volume of @p fixture_type_ in @p layout_type_. */
template <typename layout_type_, typename fixture_type_>
struct transactions_types {
    using fixture_t = fixture_type_;
    using guard_t = typename fixture_t::guard_t;
    using volume_t = typename fixture_t::volume_t;
    using key_slot_t = typename layout_type_::key_slot_t;
    using database_t =
        basic_single_collection_database<typename key_slot_t::stored_key_t, volume_t, atomic_reference, waiter_t,
                                         nvme_store<key_slot_t, volume_t, atomic_reference, waiter_t>>;
    using database_pointer_t = typename database_t::database_unique_ptr_t;
    using transaction_pointer_t = typename database_t::transaction_unique_ptr_t;

    /** A fixture, the channels a database needs, the volume over them, and the database itself. */
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

        /** The database under test, which dies before the volume it points at. */
        database_pointer_t store;

        /** The channel of the calling thread, which every read of this stack takes. */
        [[nodiscard]] io_channel_t& channel() noexcept { return reading.channel(); }
    };
};

/** A database shape whose write buffers hold a whole population, so nothing here waits on a flush. */
[[nodiscard]] single_collection_database_config_t test_config(stored_value_size_t value_size,
                                                              stored_value_size_t value_max_size) noexcept {
    single_collection_database_config_t config;
    config.collection.concurrency_limit = 2;
    config.collection.cache_limit = 1024ull * 1024ull;
    config.collection.memory_runs_max_bytes = 16ull * 1024ull * 1024ull;
    config.collection.memory_runs_max_deltas = 8192;
    config.collection.lsm_tree.level_enlarge_factor = 8;
    config.collection.lsm_tree.lsm_level_0.sorted_run.value_size = value_size;
    config.collection.lsm_tree.lsm_level_0.sorted_run.value_max_size = value_max_size;
    config.default_transaction.max_bytes = 16ull * 1024ull * 1024ull;
    config.default_transaction.max_deltas = 4096;
    return config;
}

#pragma endregion Shape

#pragma region Fixtures

/** Mounts a fixture through @p mount, formats one volume over three channels, and opens a database on it. */
template <typename layout_type_, typename mount_type_>
expected<std::unique_ptr<typename transactions_types<layout_type_, typename mount_type_::fixture_t>::stack_t>>
make_stack(mount_type_ const& mount) noexcept {
    using types_t = transactions_types<layout_type_, typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using volume_t = typename types_t::volume_t;
    using database_t = typename types_t::database_t;
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

    single_collection_database_config_t const config = test_config(layout_type_::value_size_k,
                                                                   layout_type_::value_max_size_k);
    expected<typename types_t::database_pointer_t> store = database_t::open(
        *stack->volume, stack->writing.channel(), stack->merging.channel(), config, stack->fixture->resources);
    if (!store) return store.status();
    stack->store = std::move(*store);
    return stack;
}

#pragma endregion Fixtures

#pragma region Model

/**
 *  @brief The values a case writes and what every key should answer right now.
 *
 *  Every key starts out missing; a landed insert makes it present and a landed removal leaves its remove delta.
 */
struct model_t {

    /** The value every key is written with, which answers @c written_k once it lands. */
    st::basic_vector<expectation_t> planned;

    /** What a lookup of every key should answer now. */
    st::basic_vector<lookup_result_t> states;

    /** Marks the keys between @p first and @p last as present. */
    void present(std::size_t first, std::size_t last) noexcept {
        for (std::size_t index = first; index != last; ++index) states[index] = lookup_result_t::written_k;
    }

    /** Marks the keys between @p first and @p last as removed. */
    void removed(std::size_t first, std::size_t last) noexcept {
        for (std::size_t index = first; index != last; ++index) states[index] = lookup_result_t::removed_k;
    }
};

/** Plans a value for every key of the population, every key missing, in the widths @p store admits. */
template <typename layout_type_, typename database_type_>
[[nodiscard]] std::size_t plan_model(database_type_ const& store, model_t& model) {
    std::size_t failures = 0;
    failures += expect(model.planned.resize(population_k) == st::status_t::success_k);
    failures += expect(model.states.resize(population_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    stored_value_size_t const widest = store.value_max_size();
    for (std::size_t index = 0; index != population_k; ++index) {
        expectation_t entry;
        entry.stored_key = static_cast<stored_key_t>(index);
        entry.state = lookup_result_t::written_k;
        if (layout_type_::value_size_k != 0) entry.bytes = layout_type_::value_size_k;
        else if (layout_type_::value_max_size_k != 0 && index % empty_value_step_k == empty_value_step_k - 1)
            entry.bytes = 0;
        else entry.bytes = 1 + index % widest;
        entry.seed = index * 31 + 7;
        model.planned[index] = entry;
        model.states[index] = lookup_result_t::missing_k;
    }
    return failures;
}

/** Inserts the planned value of every key between @p first and @p last through @p writer under @p schedule_. */
template <watch_schedule_t schedule_ = watch_schedule_t::default_k, typename writer_type_>
[[nodiscard]] std::size_t insert_range(writer_type_& writer, model_t const& model, std::size_t first,
                                       std::size_t last) {
    std::size_t failures = 0;
    st::basic_vector<byte_t> value;
    for (std::size_t index = first; index != last; ++index) {
        expectation_t const& entry = model.planned[index];
        failures += expect(value.resize(static_cast<std::size_t>(entry.bytes)) == st::status_t::success_k);
        paint_into(entry, std::span<byte_t> {value});
        failures += expect(
            writer.template insert<schedule_>(entry.stored_key, std::span<byte_t const> {value}).succeeded());
    }
    return failures;
}

/** Inserts the planned value of every key between @p first and @p last straight into the database @p store. */
template <typename database_type_>
[[nodiscard]] std::size_t preload_range(database_type_& store, model_t const& model, std::size_t first,
                                        std::size_t last) {
    std::size_t failures = 0;
    st::basic_vector<byte_t> value;
    for (std::size_t index = first; index != last; ++index) {
        expectation_t const& entry = model.planned[index];
        failures += expect(value.resize(static_cast<std::size_t>(entry.bytes)) == st::status_t::success_k);
        paint_into(entry, std::span<byte_t> {value});
        failures += expect(store.insert(entry.stored_key, std::span<byte_t const> {value}).succeeded());
    }
    return failures;
}

/** Removes every key between @p first and @p last through the transaction @p writer. */
template <typename writer_type_>
[[nodiscard]] std::size_t remove_range(writer_type_& writer, std::size_t first, std::size_t last) {
    std::size_t failures = 0;
    for (std::size_t index = first; index != last; ++index)
        failures += expect(writer.remove(static_cast<stored_key_t>(index)).succeeded());
    return failures;
}

/** Reads one key of the database @p source, which takes no watch. */
template <watch_schedule_t schedule_, typename source_type_>
status_t read_one(source_type_& source, io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into,
                  stored_value_size_t& size) noexcept {
    return source.select(channel, stored_key, into, size);
}

/** Reads one key through the transaction @p source, watching it under @p schedule_. */
template <watch_schedule_t schedule_, typename key_type_, typename store_type_>
status_t read_one(single_collection_database_transaction<key_type_, store_type_, atomic_reference, waiter_t>& source,
                  io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into,
                  stored_value_size_t& size) noexcept {
    return source.template select<schedule_>(channel, stored_key, into, size);
}

/** Reads a batch of keys of the database @p source, which takes no watch. */
template <watch_schedule_t schedule_, typename source_type_, typename sink_type_>
status_t read_batch(source_type_ const& source, io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<lookup_result_t> results, sink_type_&& sink) noexcept {
    return source.select(channel, stored_keys, results, sink);
}

/** Reads a batch of keys through the transaction @p source, watching them under @p schedule_. */
template <watch_schedule_t schedule_, typename key_type_, typename store_type_, typename sink_type_>
status_t read_batch(
    single_collection_database_transaction<key_type_, store_type_, atomic_reference, waiter_t> const& source,
    io_channel_t& channel, std::span<stored_key_t const> stored_keys, std::span<lookup_result_t> results,
    sink_type_&& sink) noexcept {
    return source.template select<schedule_>(channel, stored_keys, results, sink);
}

/**
 *  @brief Reads every key of the population from @p source and counts what the model disagrees with.
 *
 *  One batched read covers every key, and every present key is read again on its own, so both read paths of
 *  a database and of a transaction answer the same model.
 */
template <watch_schedule_t schedule_ = watch_schedule_t::default_k, typename source_type_>
[[nodiscard]] std::size_t check_model(source_type_& source, io_channel_t& channel, model_t const& model) {
    std::size_t failures = 0;
    st::basic_vector<stored_key_t> keys;
    st::basic_vector<lookup_result_t> results;
    st::basic_vector<std::uint64_t> seen;
    failures += expect(keys.resize(population_k) == st::status_t::success_k);
    failures += expect(results.resize(population_k) == st::status_t::success_k);
    failures += expect(seen.resize(population_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != population_k; ++index) {
        keys[index] = model.planned[index].stored_key;
        results[index] = lookup_result_t::missing_k;
        seen[index] = 0;
    }

    std::size_t mismatches = 0;
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        seen[request] += bytes.size();
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(model.planned[request].seed, offset + index)) ++mismatches;
    };
    failures += expect(read_batch<schedule_>(source, channel, std::span<stored_key_t const> {keys},
                                             std::span<lookup_result_t> {results}, sink)
                           .succeeded());
    failures += expect_eq(mismatches, std::size_t {0});

    st::basic_vector<byte_t> value;
    std::size_t wrong_states = 0;
    std::size_t wrong_values = 0;
    for (std::size_t index = 0; index != population_k; ++index) {
        if (results[index] != model.states[index]) ++wrong_states;
        if (model.states[index] != lookup_result_t::written_k) continue;
        expectation_t const& entry = model.planned[index];
        if (seen[index] != entry.bytes) ++wrong_values;
        failures += expect(value.resize(static_cast<std::size_t>(entry.bytes) + 1) == st::status_t::success_k);
        stored_value_size_t size = 0;
        if (read_one<schedule_>(source, channel, entry.stored_key, std::span<byte_t> {value}, size).failed() ||
            size != entry.bytes) {
            ++wrong_values;
            continue;
        }
        for (std::size_t position = 0; position != size; ++position)
            if (value[position] != painted(entry.seed, position)) {
                ++wrong_values;
                break;
            }
    }
    failures += expect_eq(wrong_states, std::size_t {0});
    failures += expect_eq(wrong_values, std::size_t {0});
    return failures;
}

#pragma endregion Model

#pragma region Cases

/** A transaction commits and rolls back with nothing in it, and pins its start stamp only while it lives. */
template <typename layout_type_, typename mount_type_>
std::size_t create_transaction(mount_type_ const& mount) {
    using types_t = transactions_types<layout_type_, typename mount_type_::fixture_t>;
    std::size_t failures = 0;
    expected<std::unique_ptr<typename types_t::stack_t>> stack = make_stack<layout_type_>(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    auto& store = *(*stack)->store;

    expected<typename types_t::transaction_pointer_t> first = store.create_transaction();
    failures += expect(first.has_value());
    if (!first) return failures;
    (*first)->rollback();
    failures += expect((*first)->commit().succeeded());
    failures += expect((*first)->commit().succeeded());
    (*first)->rollback();

    // A live transaction pins the stamp it started at, and gives it back when it dies.
    expected<typename types_t::transaction_pointer_t> second = store.create_transaction();
    failures += expect(second.has_value());
    failures += expect_eq(store.ledger().newest_reader(), store.ledger().published());
    if (first) first->reset();
    if (second) second->reset();
    failures += expect_eq(store.ledger().newest_reader(), unwritten_stamp_k);
    expected<typename types_t::transaction_pointer_t> reborn = store.create_transaction();
    failures += expect(reborn.has_value());
    return failures;
}

/** Two transactions live side by side, and either commits empty as often as it is asked to. */
template <typename layout_type_, typename mount_type_>
std::size_t create_transactions(mount_type_ const& mount) {
    using types_t = transactions_types<layout_type_, typename mount_type_::fixture_t>;
    std::size_t failures = 0;
    expected<std::unique_ptr<typename types_t::stack_t>> stack = make_stack<layout_type_>(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    auto& store = *(*stack)->store;

    expected<typename types_t::transaction_pointer_t> first = store.create_transaction();
    expected<typename types_t::transaction_pointer_t> second = store.create_transaction();
    failures += expect(first.has_value());
    failures += expect(second.has_value());
    if (!first || !second) return failures;
    for (std::size_t round = 0; round != 2; ++round) {
        failures += expect((*first)->commit().succeeded());
        failures += expect((*second)->commit().succeeded());
    }
    return failures;
}

/** One transaction's inserts land only at its commit, and a rollback drops the ones it had not committed. */
template <typename layout_type_, typename mount_type_>
std::size_t insert_by_one_transaction(mount_type_ const& mount) {
    using types_t = transactions_types<layout_type_, typename mount_type_::fixture_t>;
    std::size_t failures = 0;
    expected<std::unique_ptr<typename types_t::stack_t>> stack = make_stack<layout_type_>(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    auto& store = *(*stack)->store;
    io_channel_t& channel = (*stack)->channel();
    model_t model;
    failures += plan_model<layout_type_>(store, model);
    expected<typename types_t::transaction_pointer_t> changing = store.create_transaction();
    failures += expect(changing.has_value());
    if (failures != 0) return failures;
    constexpr std::size_t half_k = population_k / 2;

    failures += insert_range(**changing, model, 0, half_k);
    failures += check_model(store, channel, model);
    failures += expect((*changing)->commit().succeeded());
    model.present(0, half_k);
    failures += check_model(store, channel, model);

    failures += insert_range(**changing, model, half_k, population_k);
    (*changing)->rollback();
    failures += check_model(store, channel, model);

    failures += insert_range(**changing, model, half_k, population_k);
    failures += expect((*changing)->commit().succeeded());
    model.present(half_k, population_k);
    failures += check_model(store, channel, model);
    return failures;
}

/** Two transactions inserting disjoint halves land one after the other, each at its own commit. */
template <typename layout_type_, typename mount_type_>
std::size_t insert_by_two_transactions(mount_type_ const& mount) {
    using types_t = transactions_types<layout_type_, typename mount_type_::fixture_t>;
    std::size_t failures = 0;
    expected<std::unique_ptr<typename types_t::stack_t>> stack = make_stack<layout_type_>(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    auto& store = *(*stack)->store;
    io_channel_t& channel = (*stack)->channel();
    model_t model;
    failures += plan_model<layout_type_>(store, model);
    expected<typename types_t::transaction_pointer_t> first = store.create_transaction();
    expected<typename types_t::transaction_pointer_t> second = store.create_transaction();
    failures += expect(first.has_value() && second.has_value());
    if (failures != 0) return failures;
    constexpr std::size_t half_k = population_k / 2;

    failures += insert_range(**first, model, 0, half_k);
    failures += insert_range(**second, model, half_k, population_k);
    failures += check_model(store, channel, model);
    failures += expect((*first)->commit().succeeded());
    model.present(0, half_k);
    failures += check_model(store, channel, model);
    failures += expect((*second)->commit().succeeded());
    model.present(half_k, population_k);
    failures += check_model(store, channel, model);
    return failures;
}

/** Two transactions writing one shared key: the second to commit is refused, and so is one that read it. */
template <typename layout_type_, typename mount_type_>
std::size_t insert_by_two_intersected_transactions(mount_type_ const& mount) {
    using types_t = transactions_types<layout_type_, typename mount_type_::fixture_t>;
    std::size_t failures = 0;
    expected<std::unique_ptr<typename types_t::stack_t>> stack = make_stack<layout_type_>(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    auto& store = *(*stack)->store;
    io_channel_t& channel = (*stack)->channel();
    model_t model;
    failures += plan_model<layout_type_>(store, model);
    expected<typename types_t::transaction_pointer_t> first = store.create_transaction();
    expected<typename types_t::transaction_pointer_t> second = store.create_transaction();
    failures += expect(first.has_value() && second.has_value());
    if (failures != 0) return failures;
    constexpr std::size_t half_k = population_k / 2;

    failures += insert_range(**first, model, 0, half_k);
    failures += insert_range(**second, model, half_k - 1, population_k);
    failures += check_model(store, channel, model);
    failures += expect((*first)->commit().succeeded());
    model.present(0, half_k);
    failures += check_model(store, channel, model);
    failures += expect_status((*second)->commit(), status_code_t::write_conflict_k);
    failures += check_model(store, channel, model);
    (*second)->rollback();

    // A read is watched just as a write is, so rewriting what the second one read refuses its commit.
    st::basic_vector<byte_t> value;
    failures += expect(value.resize(8192) == st::status_t::success_k);
    for (std::size_t index = half_k - 1; index != population_k; ++index) {
        stored_value_size_t size = 0;
        [[maybe_unused]] status_t const read = (*second)->select(channel, static_cast<stored_key_t>(index),
                                                                 std::span<byte_t> {value}, size);
    }
    failures += insert_range(**first, model, 0, half_k);
    failures += expect((*first)->commit().succeeded());
    failures += expect_status((*second)->commit(), status_code_t::write_conflict_k);
    return failures;
}

/** One transaction's removals land only at its commit, and a rollback drops the ones it had not committed. */
template <typename layout_type_, typename mount_type_>
std::size_t remove_by_one_transaction(mount_type_ const& mount) {
    using types_t = transactions_types<layout_type_, typename mount_type_::fixture_t>;
    std::size_t failures = 0;
    expected<std::unique_ptr<typename types_t::stack_t>> stack = make_stack<layout_type_>(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    auto& store = *(*stack)->store;
    io_channel_t& channel = (*stack)->channel();
    model_t model;
    failures += plan_model<layout_type_>(store, model);
    failures += preload_range(store, model, 0, population_k);
    model.present(0, population_k);
    expected<typename types_t::transaction_pointer_t> changing = store.create_transaction();
    failures += expect(changing.has_value());
    if (failures != 0) return failures;
    constexpr std::size_t half_k = population_k / 2;

    failures += remove_range(**changing, half_k, population_k);
    failures += check_model(store, channel, model);
    failures += expect((*changing)->commit().succeeded());
    model.removed(half_k, population_k);
    failures += check_model(store, channel, model);

    failures += remove_range(**changing, 0, half_k);
    (*changing)->rollback();
    failures += check_model(store, channel, model);

    failures += remove_range(**changing, 0, half_k);
    failures += expect((*changing)->commit().succeeded());
    model.removed(0, half_k);
    failures += check_model(store, channel, model);
    return failures;
}

/** Two transactions removing disjoint halves land one after the other, each at its own commit. */
template <typename layout_type_, typename mount_type_>
std::size_t remove_by_two_transactions(mount_type_ const& mount) {
    using types_t = transactions_types<layout_type_, typename mount_type_::fixture_t>;
    std::size_t failures = 0;
    expected<std::unique_ptr<typename types_t::stack_t>> stack = make_stack<layout_type_>(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    auto& store = *(*stack)->store;
    io_channel_t& channel = (*stack)->channel();
    model_t model;
    failures += plan_model<layout_type_>(store, model);
    failures += preload_range(store, model, 0, population_k);
    model.present(0, population_k);
    expected<typename types_t::transaction_pointer_t> first = store.create_transaction();
    expected<typename types_t::transaction_pointer_t> second = store.create_transaction();
    failures += expect(first.has_value() && second.has_value());
    if (failures != 0) return failures;
    constexpr std::size_t half_k = population_k / 2;

    failures += remove_range(**first, 0, half_k);
    failures += remove_range(**second, half_k, population_k);
    failures += check_model(store, channel, model);
    failures += expect((*first)->commit().succeeded());
    model.removed(0, half_k);
    failures += check_model(store, channel, model);
    failures += expect((*second)->commit().succeeded());
    model.removed(half_k, population_k);
    failures += check_model(store, channel, model);
    return failures;
}

/** Two transactions removing one shared key: the second to commit is refused and removes nothing. */
template <typename layout_type_, typename mount_type_>
std::size_t remove_by_two_intersected_transactions(mount_type_ const& mount) {
    using types_t = transactions_types<layout_type_, typename mount_type_::fixture_t>;
    std::size_t failures = 0;
    expected<std::unique_ptr<typename types_t::stack_t>> stack = make_stack<layout_type_>(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    auto& store = *(*stack)->store;
    io_channel_t& channel = (*stack)->channel();
    model_t model;
    failures += plan_model<layout_type_>(store, model);
    failures += preload_range(store, model, 0, population_k);
    model.present(0, population_k);
    expected<typename types_t::transaction_pointer_t> first = store.create_transaction();
    expected<typename types_t::transaction_pointer_t> second = store.create_transaction();
    failures += expect(first.has_value() && second.has_value());
    if (failures != 0) return failures;
    constexpr std::size_t half_k = population_k / 2;

    failures += remove_range(**first, 0, half_k + 1);
    failures += remove_range(**second, half_k, population_k);
    failures += check_model(store, channel, model);
    failures += expect((*second)->commit().succeeded());
    model.removed(half_k, population_k);
    failures += check_model(store, channel, model);
    failures += expect_status((*first)->commit(), status_code_t::write_conflict_k);
    failures += check_model(store, channel, model);
    return failures;
}

/** Two transactions inserting disjoint halves from two threads at once both land. */
template <typename layout_type_, typename mount_type_>
std::size_t insert_by_two_concurrent_transactions(mount_type_ const& mount) {
    using types_t = transactions_types<layout_type_, typename mount_type_::fixture_t>;
    std::size_t failures = 0;
    expected<std::unique_ptr<typename types_t::stack_t>> stack = make_stack<layout_type_>(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    auto& store = *(*stack)->store;
    io_channel_t& channel = (*stack)->channel();
    model_t model;
    failures += plan_model<layout_type_>(store, model);
    if (failures != 0) return failures;
    constexpr std::size_t half_k = population_k / 2;

    std::size_t worker_failures[2] = {0, 0};
    auto work = [&](std::size_t worker) noexcept {
        expected<typename types_t::transaction_pointer_t> changing = store.create_transaction();
        worker_failures[worker] += expect(changing.has_value());
        if (!changing) return;
        std::size_t const first = worker == 0 ? 0 : half_k;
        worker_failures[worker] += insert_range(**changing, model, first, first + half_k);
        worker_failures[worker] += expect((*changing)->commit().succeeded());
    };
    std::thread first_worker {work, std::size_t {0}};
    std::thread second_worker {work, std::size_t {1}};
    first_worker.join();
    second_worker.join();
    failures += worker_failures[0] + worker_failures[1];

    model.present(0, population_k);
    failures += check_model(store, channel, model);
    return failures;
}

/** Writes and reads are watched on every schedule, refused when watched and admitted when not. */
template <typename layout_type_, typename mount_type_>
std::size_t watch_transactions(mount_type_ const& mount) {
    using types_t = transactions_types<layout_type_, typename mount_type_::fixture_t>;
    std::size_t failures = 0;
    expected<std::unique_ptr<typename types_t::stack_t>> stack = make_stack<layout_type_>(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    auto& store = *(*stack)->store;
    io_channel_t& channel = (*stack)->channel();
    model_t model;
    failures += plan_model<layout_type_>(store, model);
    failures += preload_range(store, model, 0, population_k);
    model.present(0, population_k);
    expected<typename types_t::transaction_pointer_t> first = store.create_transaction();
    expected<typename types_t::transaction_pointer_t> second = store.create_transaction();
    failures += expect(first.has_value() && second.has_value());
    if (failures != 0) return failures;
    auto& one = **first;
    auto& other = **second;
    constexpr std::size_t half_k = population_k / 2;
    std::array<stored_key_t, 1> const first_key {stored_key_t {0}};
    std::span<stored_key_t const> const watched {first_key};

    // Writes are watched, so the second writer of the shared key is refused.
    failures += insert_range(one, model, 0, half_k + 1);
    failures += insert_range(other, model, half_k, population_k);
    failures += expect(one.commit().succeeded());
    failures += expect_status(other.commit(), status_code_t::write_conflict_k);
    other.rollback();

    // Reads are watched too, so a commit over what the second one read refuses it.
    failures += insert_range(one, model, 0, 1);
    failures += check_model(other, channel, model);
    failures += expect(one.commit().succeeded());
    failures += expect_status(other.commit(), status_code_t::write_conflict_k);
    other.rollback();

    // Writes taken under the never schedule collide without a word.
    failures += insert_range<watch_schedule_t::never_k>(one, model, 0, half_k + 1);
    failures += insert_range<watch_schedule_t::never_k>(other, model, half_k, population_k);
    failures += expect(one.commit().succeeded());
    failures += expect(other.commit().succeeded());

    // And so do reads taken under it.
    failures += insert_range<watch_schedule_t::never_k>(one, model, 0, 1);
    failures += check_model<watch_schedule_t::never_k>(other, channel, model);
    failures += expect(one.commit().succeeded());
    failures += expect(other.commit().succeeded());

    // A watch added later checks at once, at the commit from the start, or at the commit from the call.
    failures += insert_range(one, model, 0, 1);
    failures += expect(one.commit().succeeded());
    failures += expect(other.template watch_more<watch_schedule_t::now_k>(watched).failed());
    failures += expect(other.template watch_more<watch_schedule_t::on_commit_k>(watched).succeeded());
    failures += expect_status(other.commit(), status_code_t::write_conflict_k);
    other.rollback();
    failures += insert_range(one, model, 0, 1);
    failures += expect(one.commit().succeeded());
    failures += expect(other.template watch_more<watch_schedule_t::on_commit_since_operation_k>(watched).succeeded());
    failures += expect(other.commit().succeeded());
    return failures;
}

/** A staged transaction lands at its commit, and nothing of it lands after a rollback or after it dies. */
template <typename layout_type_, typename mount_type_>
std::size_t stage_and_commit(mount_type_ const& mount) {
    using types_t = transactions_types<layout_type_, typename mount_type_::fixture_t>;
    std::size_t failures = 0;
    expected<std::unique_ptr<typename types_t::stack_t>> stack = make_stack<layout_type_>(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    auto& store = *(*stack)->store;
    io_channel_t& channel = (*stack)->channel();
    model_t model;
    failures += plan_model<layout_type_>(store, model);
    expected<typename types_t::transaction_pointer_t> changing = store.create_transaction();
    failures += expect(changing.has_value());
    if (failures != 0) return failures;
    constexpr std::size_t half_k = population_k / 2;

    failures += insert_range(**changing, model, 0, half_k);
    failures += expect((*changing)->stage().succeeded());
    failures += expect((*changing)->commit().succeeded());
    model.present(0, half_k);
    failures += check_model(store, channel, model);

    failures += insert_range(**changing, model, half_k, population_k);
    failures += expect((*changing)->stage().succeeded());
    (*changing)->rollback();
    failures += check_model(store, channel, model);

    // A staged transaction that dies hands the write path back and lands nothing.
    failures += insert_range(**changing, model, half_k, population_k);
    failures += expect((*changing)->stage().succeeded());
    changing->reset();
    failures += check_model(store, channel, model);
    return failures;
}

#pragma endregion Cases

#pragma region Driver

/** A case label of one layout, the layout's name joined with the case's. */
struct layout_case_name_t {
    char text[96] = {};

    layout_case_name_t(std::string_view layout_name, std::string_view case_name) noexcept {
        auto const written = fmt::format_to_n(text, sizeof(text) - 1, "{}.{}", layout_name, case_name);
        *written.out = '\0';
    }
};

/** Runs every case on @p mount for one layout named @p layout_name. */
template <typename layout_type_, typename mount_type_>
void run_layout(suite_totals_t& totals, mount_type_ const& mount, std::string_view layout_name) {
    report(totals, mount, layout_case_name_t {layout_name, "create_transaction"}.text,
           create_transaction<layout_type_, mount_type_>);
    report(totals, mount, layout_case_name_t {layout_name, "create_transactions"}.text,
           create_transactions<layout_type_, mount_type_>);
    report(totals, mount, layout_case_name_t {layout_name, "insert"}.text,
           insert_by_one_transaction<layout_type_, mount_type_>);
    report(totals, mount, layout_case_name_t {layout_name, "insert_by_two"}.text,
           insert_by_two_transactions<layout_type_, mount_type_>);
    report(totals, mount, layout_case_name_t {layout_name, "insert_by_two_intersected"}.text,
           insert_by_two_intersected_transactions<layout_type_, mount_type_>);
    report(totals, mount, layout_case_name_t {layout_name, "remove"}.text,
           remove_by_one_transaction<layout_type_, mount_type_>);
    report(totals, mount, layout_case_name_t {layout_name, "remove_by_two"}.text,
           remove_by_two_transactions<layout_type_, mount_type_>);
    report(totals, mount, layout_case_name_t {layout_name, "remove_by_two_intersected"}.text,
           remove_by_two_intersected_transactions<layout_type_, mount_type_>);
    report(totals, mount, layout_case_name_t {layout_name, "insert_by_two_concurrent"}.text,
           insert_by_two_concurrent_transactions<layout_type_, mount_type_>);
    report(totals, mount, layout_case_name_t {layout_name, "watch"}.text,
           watch_transactions<layout_type_, mount_type_>);
    report(totals, mount, layout_case_name_t {layout_name, "stage_and_commit"}.text,
           stage_and_commit<layout_type_, mount_type_>);
}

#pragma endregion Driver

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
        run_layout<inline_1_byte_layout_t>(totals, mount, "inline_1_byte");
        run_layout<inline_64_bytes_layout_t>(totals, mount, "inline_64_bytes");
        run_layout<fixed_width_layout_t>(totals, mount, "fixed_width");
        run_layout<variable_width_layout_t>(totals, mount, "variable_width");
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, transactions_config_k, 1, "scdb_transactions", cases);

    return suite_exit_code(totals);
}
