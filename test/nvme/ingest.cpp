/**
 *  @file test/nvme/ingest.cpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief An ingest of a collection: where its runs land, what a crash leaves of it, and what readers and transactions
 *      from before it see.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <array>   // `std::array`
#include <memory>  // `std::unique_ptr`
#include <new>     // `std::nothrow`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_database.hpp" // `basic_database`
#include "ustore/strings/nvme_store.hpp"  // `strings::nvme_store`

#include "nvme/fixture.hpp" // `run_on_transport`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Shape

/** The numbers every case mounts with. */
constexpr fixture_config_t ingest_config_k {.queues_threads = 8};

/** The keys every case of this suite drives, and the two ways a collection is named. */
using stored_key_t = std::uint64_t;
using any_collection_id_t = std::variant<collection_id_t, collection_alphanumeric_id_t>;

/** One row an ingest lands. */
using row_t = keyed_delta<stored_key_t>;

/** The database and its transactions every case drives over the volume of @p fixture_type_. */
template <typename fixture_type_>
struct database_types {
    using fixture_t = fixture_type_;
    using volume_t = typename fixture_t::volume_t;
    template <value_layout_t value_layout_>
    using blobs_store_of = nvme_store<key_slot<stored_key_t, value_layout_>, volume_t, atomic_reference, waiter_t,
                                      last_write_fold_t, no_device_fold_t>;
    using strings_kit_t = utilities::strings_ladder<sz_capabilities_compiled_cpu_mask()>::strings_kit_t;
    using strings_store_t = strings::nvme_store<volume_t, atomic_reference, waiter_t, strings_kit_t>;
    template <typename... store_types_>
    using basic_database_of = basic_database<stored_key_t, volume_t, atomic_reference, waiter_t, st::native_row_kit_t,
                                             layout_store_tags<store_types_...>, store_types_...>;
    using database_t = basic_database_of<strings_store_t, blobs_store_of<value_layout_t::inline_12_bytes_k>,
                                         blobs_store_of<value_layout_t::inline_44_bytes_k>,
                                         blobs_store_of<value_layout_t::fixed_width_k>,
                                         blobs_store_of<value_layout_t::variable_width_k>>;
    using transaction_t = typename database_t::transaction_t;

    /** A fixture, its volume, and the database mounted over the volume. */
    struct stack_t {

        /** The disk, the arena, the channels and the one volume. */
        std::unique_ptr<fixture_t> fixture;

        /** The volume of the database, which every reopen mounts again. */
        volume_t volume;

        /** The database, destroyed before the mount it views. */
        std::unique_ptr<database_t> store;

        /** The database this stack opens, which a reopen opens again. */
        using database_t = database_types::database_t;
    };
};

/**
 *  @brief The collection shape a case creates, whose value size alone decides the value layout.
 *  @param[in] value_size Bytes every value takes, which is zero for a collection of values that differ.
 *  @param[in] value_max_size Bytes the largest value takes, which is zero for a layout of one width.
 */
[[nodiscard]] collection_config_t test_config(stored_value_size_t value_size,
                                              stored_value_size_t value_max_size) noexcept {
    collection_config_t config;
    config.concurrency_limit = 1;
    config.cache_limit = 1024ull * 1024ull;
    config.memory_runs_max_bytes = 1024ull * 1024ull;
    config.memory_runs_max_deltas = 4096;
    config.lsm_tree.level_enlarge_factor = 4;
    config.lsm_tree.lsm_level_0.sorted_run.value_size = value_size;
    config.lsm_tree.lsm_level_0.sorted_run.value_max_size = value_max_size;
    return config;
}

/** Creates the collection @p name in @p store on @p queues under an ID minted for it, answering
 *  the ID or why it was refused. */
template <typename store_type_>
expected<collection_id_t> create_named(store_type_& store, nvme_queues_ref_t queues, string_view_t name,
                                       collection_config_t const& config) noexcept {
    collection_id_t collection_id;
    std::memcpy(collection_id.uuid.bytes, uuid_generator().bytes, sizeof(collection_id.uuid.bytes));
    value_layout_t const layout = resolve_value_layout(config.lsm_tree.lsm_level_0.sorted_run.value_size);
    if (status_t const created = store.create_collection(collection_id, name, static_cast<std::uint16_t>(layout),
                                                         config, collection_invalid_id_k, queues);
        created.failed())
        return created;
    return collection_id;
}

/** The transaction shape every case creates, which holds a whole case's writes. */
[[nodiscard]] database_config_t test_database_config() noexcept {
    database_config_t config;
    config.default_transaction.max_bytes = 1024ull * 1024ull;
    config.default_transaction.max_deltas = 4096;
    return config;
}

/** @p value as the eight bytes an inline collection of that width stores. */
[[nodiscard]] std::array<byte_t, 8> counted(std::uint64_t value) noexcept {
    std::array<byte_t, 8> bytes {};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

/** The number @p bytes holds, which is zero for a span of another width. */
[[nodiscard]] std::uint64_t counter_of(std::span<byte_t const> bytes) noexcept {
    std::uint64_t value = 0;
    if (bytes.size() == sizeof(value)) std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

/** Mounts the volume of @p stack again, dropping the one it held, as a process starting over the
 *  same media does. */
template <typename stack_type_>
status_t remount_volume(stack_type_& stack) noexcept {
    stack.volume = typename stack_type_::database_t::volume_t {};
    auto [guard, acquire_status] = stack.fixture->channel(0);
    if (acquire_status.failed()) return acquire_status;
    auto [volume, mount_status] = stack.fixture->mount_volume(*guard);
    if (mount_status.failed()) return mount_status;
    stack.volume = std::move(volume);
    return {};
}

/** Mounts the volume of @p stack again and a database over it and the channels of @p stack. */
template <typename stack_type_>
expected<std::unique_ptr<typename stack_type_::database_t>> open_database(stack_type_& stack) noexcept {
    using database_t = typename stack_type_::database_t;
    if (status_t const remounted = remount_volume(stack); remounted.failed()) return remounted;
    auto store = std::unique_ptr<database_t> {new (std::nothrow) database_t {stack.fixture->resources}};
    if (!store) return status_t {status_code_t::out_of_memory_k, "The database did not fit"};
    if (status_t const mounted = store->mount(stack.volume, nvme_queues_ref_t {&stack.fixture->queues, 0},
                                              test_database_config());
        mounted.failed())
        return mounted;
    return store;
}

/** Mounts a fixture through @p mount and opens one database over its volume. */
template <typename mount_type_>
expected<std::unique_ptr<typename database_types<typename mount_type_::fixture_t>::stack_t>> make_stack(
    mount_type_ const& mount) noexcept {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    auto stack = std::unique_ptr<stack_t> {new (std::nothrow) stack_t {}};
    if (!stack) return status_t {status_code_t::out_of_memory_k, "The stack did not fit"};

    auto [fixture, mount_status] = mount();
    if (mount_status.failed()) return mount_status;
    stack->fixture = std::move(fixture);

    auto [store, open_database_status] = open_database(*stack);
    if (open_database_status.failed()) return open_database_status;
    stack->store = std::move(store);
    return stack;
}

#pragma endregion Shape

#pragma region Rows

/** The counters every row of a case holds, one per key, which outlive the rows viewing them. */
struct counted_rows_t {
    st::vector<std::array<byte_t, 8>> values;
    st::vector<row_t> rows;
};

/** Rows writing @p value under every key of @p keys, in that order. */
[[nodiscard]] counted_rows_t rows_of(std::span<stored_key_t const> keys, std::uint64_t value) noexcept {
    counted_rows_t made;
    [[maybe_unused]] st::status_t const sized_values = made.values.resize(keys.size());
    [[maybe_unused]] st::status_t const sized_rows = made.rows.resize(keys.size());
    for (std::size_t index = 0; index != keys.size(); ++index) {
        made.values[index] = counted(value);
        made.rows[index] = row_t {keys[index],
                                  delta_view_t {delta_kind_t::write_k, std::span<byte_t const> {made.values[index]}}};
    }
    return made;
}

/** The keys from @p first up to @p last, excluded. */
[[nodiscard]] st::vector<stored_key_t> keys_between(stored_key_t first, stored_key_t last) noexcept {
    st::vector<stored_key_t> keys;
    [[maybe_unused]] st::status_t const sized = keys.resize(static_cast<std::size_t>(last - first));
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = first + index;
    return keys;
}

/** Ingests @p rows into @p collection of @p store on @p queues in appends of @p batch rows, as an
 *  engine hands over the batches of a stream, at @p durability. */
template <typename database_type_>
expected<transaction_stamp_t> ingest_rows(database_type_& store, nvme_queues_ref_t queues, collection_id_t collection,
                                          counted_rows_t const& rows, ustore_durability_t durability,
                                          std::size_t batch = 7) noexcept {
    typename database_type_::ingest_t ingest;
    if (status_t const begun = store.begin_ingest(any_collection_id_t {collection}, ingest); begun.failed())
        return begun;
    std::span<row_t const> const rows_view {rows.rows};
    for (std::size_t first = 0; first < rows_view.size(); first += batch) {
        status_t const written = store.write_ingest(
            ingest, rows_view.subspan(first, fu::min_of(batch, rows_view.size() - first)), queues);
        if (written.failed()) {
            store.drop_ingest(ingest);
            return written;
        }
    }
    ingest_outcome_t const published = store.publish_ingest(ingest, durability, queues);
    if (published.status.failed()) return published.status;
    return published.stamp;
}

/** Keys of @p keys @p store answers @p value for at @p stamp, reading on @p queues. */
template <typename database_type_>
[[nodiscard]] std::size_t count_valued(database_type_& store, nvme_queues_ref_t queues, collection_id_t collection,
                                       std::span<stored_key_t const> keys, std::uint64_t value,
                                       transaction_stamp_t stamp = head_stamp_k) noexcept {
    std::size_t found = 0;
    for (stored_key_t const key : keys) {
        std::array<byte_t, 8> into {};
        stored_value_size_t size = 0;
        if (store.select(any_collection_id_t {collection}, key, std::span<byte_t> {into}, size, stamp, queues).failed())
            continue;
        found += counter_of(std::span<byte_t const> {into.data(), size}) == value;
    }
    return found;
}

/** Runs every level of @p collection's tree holds, shallowest first, into @p into. */
template <typename database_type_>
status_t level_runs(database_type_& store, collection_id_t collection, st::vector<std::size_t>& into) noexcept {
    return store.visit_store(any_collection_id_t {collection},
                             [&]<typename held_type_>(held_type_ const& held) noexcept -> status_t {
                                 typename held_type_::level_set_t const& levels = held.tree().levels();
                                 if (into.resize(levels.size()) != st::status_t::success_k)
                                     return {status_code_t::out_of_memory_k, "No room to count the levels"};
                                 for (std::size_t number = 0; number != levels.size(); ++number)
                                     into[number] = levels.level(number).size();
                                 return {};
                             });
}

#pragma endregion Rows

#pragma region Placement Cases

/** An empty collection takes an ingest on a sorted level, leaving level zero empty, and answers every row. */
template <typename mount_type_>
void test_empty_collection_lands_on_a_sorted_level(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    nvme_queues_ref_t const queues {&(*stack)->fixture->queues, 0};
    expected<collection_id_t> const fresh = create_named(*(*stack)->store, queues, "fresh", test_config(8, 0));
    expect_true(fresh.has_value());
    if (!fresh) return;

    st::vector<stored_key_t> const keys = keys_between(0, 300);
    expected<transaction_stamp_t> const landed = ingest_rows(*(*stack)->store, queues, *fresh,
                                                             rows_of(std::span<stored_key_t const> {keys}, 9),
                                                             ustore_durability_buffered_k);
    expect_true(landed.has_value());
    st::vector<std::size_t> runs;
    expect_true(level_runs(*(*stack)->store, *fresh, runs).succeeded());
    expect_true(runs.size() > 1);
    if (runs.size() <= 1) return;
    expect_equal(runs[0], std::size_t {0});
    expect_equal(runs[runs.size() - 1], std::size_t {1});
    expect_equal(count_valued(*(*stack)->store, queues, *fresh, std::span<stored_key_t const> {keys}, 9), keys.size());
}

/** Keys a run on level one already holds take an ingest on level zero, above it, so the ingest answers them. */
template <typename mount_type_>
void test_overlap_at_level_one_lands_on_level_zero(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    nvme_queues_ref_t const queues {&(*stack)->fixture->queues, 0};
    typename types_t::database_t& store = *(*stack)->store;
    expected<collection_id_t> const deep = create_named(store, queues, "deep", test_config(8, 0));
    expect_true(deep.has_value());
    if (!deep) return;

    // Level zero is due at four runs, so four flushes and a merge leave every key on level one.
    st::vector<stored_key_t> const keys = keys_between(0, 100);
    for (std::uint64_t round = 0; round != 4; ++round) {
        for (stored_key_t const key : keys) {
            std::array<byte_t, 8> const counter = counted(round);
            expect_true(
                store.insert(any_collection_id_t {*deep}, key, stored_value_view_t {counter}, queues).succeeded());
        }
        expect_true(store.flush(queues).succeeded());
    }
    expect_true(store.compact({}, queues).succeeded());
    st::vector<std::size_t> runs;
    expect_true(level_runs(store, *deep, runs).succeeded());
    expect_true(runs.size() > 1 && runs[0] == 0 && runs[1] != 0);

    expect_true(ingest_rows(store, queues, *deep, rows_of(std::span<stored_key_t const> {keys}, 42),
                            ustore_durability_buffered_k)
                    .has_value());
    expect_true(level_runs(store, *deep, runs).succeeded());
    expect_true(!runs.empty() && runs[0] == 1);
    expect_equal(count_valued(store, queues, *deep, std::span<stored_key_t const> {keys}, 42), keys.size());
}

/** Appends far smaller than a run land together in one run, not one run each. */
template <typename mount_type_>
void test_appends_share_one_run(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    nvme_queues_ref_t const queues {&(*stack)->fixture->queues, 0};
    expected<collection_id_t> const coalesced = create_named(*(*stack)->store, queues, "coalesced", test_config(8, 0));
    expect_true(coalesced.has_value());
    if (!coalesced) return;

    st::vector<stored_key_t> const keys = keys_between(0, 8000);
    expect_true(ingest_rows(*(*stack)->store, queues, *coalesced, rows_of(std::span<stored_key_t const> {keys}, 3),
                            ustore_durability_buffered_k, 1000)
                    .has_value());
    st::vector<std::size_t> runs;
    expect_true(level_runs(*(*stack)->store, *coalesced, runs).succeeded());
    std::size_t total_runs = 0;
    for (std::size_t const level : runs) total_runs += level;
    expect_equal(total_runs, std::size_t {1});
    expect_equal(count_valued(*(*stack)->store, queues, *coalesced, std::span<stored_key_t const> {keys}, 3),
                 keys.size());
}

#pragma endregion Placement Cases

#pragma region Durability Cases

/** What a crash right after an ingest leaves: all of it or none of it when buffered, all of it when flushed. */
template <typename mount_type_>
void crash_after_ingest(mount_type_ const& mount, ustore_durability_t durability) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    nvme_queues_ref_t const queues {&(*stack)->fixture->queues, 0};
    expected<collection_id_t> const landed = create_named(*(*stack)->store, queues, "landed", test_config(8, 0));
    expect_true(landed.has_value());
    if (!landed) return;
    st::vector<stored_key_t> const keys = keys_between(0, 200);
    expect_true(
        ingest_rows(*(*stack)->store, queues, *landed, rows_of(std::span<stored_key_t const> {keys}, 5), durability)
            .has_value());

    expect_true(capture_data_directory(*(*stack)->fixture).succeeded());
    (*stack)->store.reset();
    (*stack)->volume = typename types_t::volume_t {};
    expect_true(restore_data_directory(*(*stack)->fixture).succeeded());
    expected<std::unique_ptr<database_t>> reopened = open_database(**stack);
    expect_true(reopened.has_value());
    if (!reopened) return;
    std::size_t const kept = count_valued(**reopened, queues, *landed, std::span<stored_key_t const> {keys}, 5);
    if (durability == ustore_durability_flushed_k) expect_equal(kept, keys.size());
    else expect_true(kept == 0 || kept == keys.size());
}

/** The reason a crash case reports on a block device, whose medium it cannot copy aside. */
constexpr char const* needs_copyable_medium_k = "needs a medium a case may copy, not a block device";

/** A buffered ingest a crash catches before any flush reopens whole or not at all. */
template <typename mount_type_>
case_result_t test_buffered_ingest_is_all_or_nothing(mount_type_ const& mount) {
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    if (mount.transport == transport_t::device_k) return case_result_t::skipped(needs_copyable_medium_k);
    crash_after_ingest(mount, ustore_durability_buffered_k);
    return {};
}

/** A flushed ingest survives a crash right after it returns. */
template <typename mount_type_>
case_result_t test_flushed_ingest_survives_reopen(mount_type_ const& mount) {
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    if (mount.transport == transport_t::device_k) return case_result_t::skipped(needs_copyable_medium_k);
    crash_after_ingest(mount, ustore_durability_flushed_k);
    return {};
}

#pragma endregion Durability Cases

#pragma region Isolation Cases

/** A snapshot pinned before an ingest reads the value the key held before it. */
template <typename mount_type_>
void test_snapshot_reads_the_older_value(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    nvme_queues_ref_t const queues {&(*stack)->fixture->queues, 0};
    database_t& store = *(*stack)->store;
    expected<collection_id_t> const held = create_named(store, queues, "held", test_config(8, 0));
    expect_true(held.has_value());
    if (!held) return;

    std::array<byte_t, 8> const older = counted(1);
    expect_true(
        store.insert(any_collection_id_t {*held}, stored_key_t {5}, stored_value_view_t {older}, queues).succeeded());
    expected<typename database_t::snapshot_t> taken = store.pin_stamp();
    expect_true(taken.has_value());
    if (!taken) return;
    std::array<stored_key_t, 1> const key {5};
    expect_true(
        ingest_rows(store, queues, *held, rows_of(std::span<stored_key_t const> {key}, 2), ustore_durability_buffered_k)
            .has_value());

    expect_equal(count_valued(store, queues, *held, std::span<stored_key_t const> {key}, 1, taken->stamp()),
                 std::size_t {1});
    expect_equal(count_valued(store, queues, *held, std::span<stored_key_t const> {key}, 2), std::size_t {1});
}

/** A transaction that started before an ingest and wrote one of its keys fails at commit. */
template <typename mount_type_>
void test_older_transaction_conflicts(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    nvme_queues_ref_t const queues {&(*stack)->fixture->queues, 0};
    database_t& store = *(*stack)->store;
    expected<collection_id_t> const contested = create_named(store, queues, "contested", test_config(8, 0));
    expect_true(contested.has_value());
    if (!contested) return;

    expected<typename database_t::transaction_unique_ptr_t> older = store.create_transaction();
    expect_true(older.has_value());
    if (!older) return;
    std::array<byte_t, 8> const mine = counted(1);
    expect_true(
        (*older)->insert(any_collection_id_t {*contested}, stored_key_t {5}, stored_value_view_t {mine}).succeeded());
    std::array<stored_key_t, 1> const key {5};
    expect_true(ingest_rows(store, queues, *contested, rows_of(std::span<stored_key_t const> {key}, 2),
                            ustore_durability_buffered_k)
                    .has_value());
    expect_status((*older)->commit(queues), status_code_t::write_conflict_k);
    expect_equal(count_valued(store, queues, *contested, std::span<stored_key_t const> {key}, 2), std::size_t {1});
}

#pragma endregion Isolation Cases

#pragma region Refusal Cases

/** Keys that do not strictly ascend are refused, leaving nothing of the ingest behind, no run included. */
template <typename mount_type_>
void test_descending_key_is_refused(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    nvme_queues_ref_t const queues {&(*stack)->fixture->queues, 0};
    expected<collection_id_t> const refused = create_named(*(*stack)->store, queues, "refused", test_config(8, 0));
    expect_true(refused.has_value());
    if (!refused) return;

    st::vector<stored_key_t> keys = keys_between(0, 20);
    keys[19] = 3;
    counted_rows_t const rows = rows_of(std::span<stored_key_t const> {keys}, 7);
    expected<transaction_stamp_t> const landed = ingest_rows(*(*stack)->store, queues, *refused, rows,
                                                             ustore_durability_buffered_k);
    expect_status(landed, status_code_t::invalid_argument_k);
    expect_equal(count_valued(*(*stack)->store, queues, *refused, std::span<stored_key_t const> {keys}, 7),
                 std::size_t {0});
    expect_equal(run_count_of(*(*stack)->fixture), std::size_t {0});
    expect_equal(run_files_of(*(*stack)->fixture), std::size_t {0});
}

#pragma endregion Refusal Cases

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
        report_on_mount(mount, "test_empty_collection_lands_on_a_sorted_level",
                        test_empty_collection_lands_on_a_sorted_level);
        report_on_mount(mount, "test_overlap_at_level_one_lands_on_level_zero",
                        test_overlap_at_level_one_lands_on_level_zero);
        report_on_mount(mount, "test_appends_share_one_run", test_appends_share_one_run);
        report_on_mount(mount, "test_buffered_ingest_is_all_or_nothing", test_buffered_ingest_is_all_or_nothing);
        report_on_mount(mount, "test_flushed_ingest_survives_reopen", test_flushed_ingest_survives_reopen);
        report_on_mount(mount, "test_snapshot_reads_the_older_value", test_snapshot_reads_the_older_value);
        report_on_mount(mount, "test_older_transaction_conflicts", test_older_transaction_conflicts);
        report_on_mount(mount, "test_descending_key_is_refused", test_descending_key_is_refused);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(transport, directory->path(), ingest_config_k, 1, "ingest", cases);

    return finish_test_run();
}
