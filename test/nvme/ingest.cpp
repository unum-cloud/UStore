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

#include <algorithm> // `std::min`
#include <array>     // `std::array`
#include <memory>    // `std::unique_ptr`
#include <new>       // `std::nothrow`
#include <span>      // `std::span`
#include <utility>   // `std::move`
#include <variant>   // `std::visit`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/blobs/nvme_database.hpp"   // `basic_database`
#include "ustore/strings/nvme_store.hpp"    // `strings::nvme_store`
#include "ustore/utilities/nvme_volume.hpp" // `resources_t`

#include "nvme/fixture.hpp" // `basic_fixture`, `run_on_transport`

namespace unum::ustore::test {

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Shape

/** The numbers every case mounts with. */
constexpr fixture_config_t ingest_config_k {.shared_io_channels = 8};

/** The keys every case of this suite drives, and the two ways a collection is named. */
using stored_key_t = std::uint64_t;
using any_collection_id_t = std::variant<collection_id_t, collection_alphanumeric_id_t>;

/** One row an ingest lands. */
using row_t = keyed_delta<stored_key_t>;

/** The router, the database and its transactions every case drives over the volume of @p fixture_type_. */
template <typename fixture_type_>
struct database_types {
    using fixture_t = fixture_type_;
    using guard_t = typename fixture_t::guard_t;
    using volume_t = typename fixture_t::volume_t;
    using router_t = basic_volume_router<volume_t>;
    template <value_layout_t value_layout_>
    using blobs_store_of = nvme_store<key_slot<stored_key_t, value_layout_>, volume_t, atomic_reference, waiter_t,
                                      last_write_fold_t, no_device_fold_t>;
    using strings_store_t = strings::nvme_store<volume_t, atomic_reference, waiter_t>;
    template <typename... stores_>
    using database_of =
        basic_database<stored_key_t, volume_t, atomic_reference, waiter_t, layout_store_tags<stores_...>, stores_...>;
    using database_t =
        database_of<strings_store_t, blobs_store_of<value_layout_t::inline_12_bytes_k>,
                    blobs_store_of<value_layout_t::inline_44_bytes_k>, blobs_store_of<value_layout_t::fixed_width_k>,
                    blobs_store_of<value_layout_t::variable_width_k>>;
    using transaction_t = typename database_t::transaction_t;

    /** A fixture, the router over its one volume, and the database mounted over the router. */
    struct stack_t {

        /** The disk, the arena, the channels and the one volume. */
        std::unique_ptr<fixture_t> fixture;

        /** The volumes of the database, of which this stack opens one. */
        router_t router;

        /** The database, destroyed before the mount it borrows. */
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

/** Creates the collection @p name in @p store under an ID minted for it, answering the ID or why it was refused. */
template <typename store_type_>
expected<collection_id_t> create_named(store_type_& store, string_view_t name,
                                       collection_config_t const& config) noexcept {
    collection_id_t collection_id;
    std::memcpy(collection_id.uuid.bytes, uuid_generator().bytes, sizeof(collection_id.uuid.bytes));
    value_layout_t const layout = resolve_value_layout(config.lsm_tree.lsm_level_0.sorted_run.value_size);
    if (status_t const created = store.create_collection(collection_id, name, static_cast<std::uint16_t>(layout),
                                                         config);
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

/** The number @p bytes carries, which is zero for a span of another width. */
[[nodiscard]] std::uint64_t counter_of(std::span<byte_t const> bytes) noexcept {
    std::uint64_t value = 0;
    if (bytes.size() == sizeof(value)) std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

/** Sorted runs @p volume names right now, which a reopen neither adds to nor loses. */
template <typename volume_type_>
[[nodiscard]] std::size_t run_count_of(volume_type_ const& volume) noexcept {
    std::size_t counted = 0;
    volume.for_each(blob_kind_t::sorted_run_k, [&](blob_name_t, volume_blob_t const&) noexcept { ++counted; });
    return counted;
}

/** Mounts a database over the router and the channels of @p stack, which a reopen does a second time over the same
 *  volume. */
template <typename stack_type_>
expected<std::unique_ptr<typename stack_type_::database_t>> open_database(stack_type_& stack) noexcept {
    using database_t = typename stack_type_::database_t;
    auto store = std::unique_ptr<database_t> {new (std::nothrow) database_t {stack.fixture->resources}};
    if (!store) return status_t {status_code_t::out_of_memory_k, "The database did not fit"};
    if (status_t const mounted = store->mount(stack.router, stack.fixture->channels, test_database_config());
        mounted.failed())
        return mounted;
    return store;
}

/** Mounts a fixture through @p mount, adopts its volume into a router and opens one database over it. */
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

    auto [built, make_status] = types_t::router_t::make();
    if (make_status.failed()) return make_status;
    stack->router = std::move(built);

    auto [guard, acquire_status] = stack->fixture->channels.acquire();
    if (acquire_status.failed()) return acquire_status;
    expected<typename types_t::volume_t> volume = stack->fixture->mount_volume(guard.channel());
    if (!volume) return volume.status();
    if (status_t const adopted = stack->router.bind_queue_pair(std::move(*volume)); adopted.failed()) return adopted;

    auto [store, open_database_status] = open_database(*stack);
    if (open_database_status.failed()) return open_database_status;
    stack->store = std::move(store);
    return stack;
}

/** Copies the whole medium of volume zero of @p fixture into @p image, block by block on @p channel, which is what a
 *  crash would leave on it at this moment. */
template <typename fixture_type_>
status_t capture_medium(fixture_type_& fixture, io_channel_t& channel, st::basic_vector<byte_t>& image) noexcept {
    std::uint64_t const capacity = fixture.config.volume_bytes;
    std::uint64_t const block = round_down_to_multiple(channel.requests().buffer_bytes(), portable_alignment_k);
    if (block == 0) return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than one page"};
    if (image.resize(static_cast<std::size_t>(capacity)) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "No room for the image of a medium"};
    basic_read_pipeline<typename fixture_type_::disk_t> reader {fixture.disk, channel, fixture.media[0].reference()};
    for (std::uint64_t offset = 0; offset < capacity; offset += block) {
        std::uint64_t const length = capacity - offset < block ? capacity - offset : block;
        if (status_t const offered = reader.enqueue_read(extent_t {offset, length}, 0); offered.failed())
            return offered;
        if (status_t const submitted = reader.submit(); submitted.failed()) return submitted;
        status_t carried;
        auto sink = [&](completed_buffer_t const& completed) noexcept {
            if (status_t const whole = require_completed_in_full(completed); whole.failed()) carried = whole;
            else std::memcpy(image.data() + offset, completed.bytes.data(), static_cast<std::size_t>(length));
        };
        if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
        if (carried.failed()) return carried;
    }
    return {};
}

/** Writes @p image back over the whole medium of volume zero of @p fixture, as a restart after a crash finds it. */
template <typename fixture_type_>
status_t restore_medium(fixture_type_& fixture, io_channel_t& channel, std::span<byte_t const> image) noexcept {
    basic_write_pipeline<typename fixture_type_::disk_t, waiter_t> writer {fixture.disk, channel,
                                                                           fixture.media[0].reference()};
    std::uint64_t offset = 0;
    while (offset != image.size()) {
        auto [buffer, acquire_status] = writer.acquire_buffer_or_poll();
        if (acquire_status.failed()) return acquire_status;
        std::uint64_t const room = round_down_to_multiple(buffer.size(), portable_alignment_k);
        if (room == 0) return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than one page"};
        std::uint64_t const length = image.size() - offset < room ? image.size() - offset : room;
        std::memcpy(buffer.bytes().data(), image.data() + offset, static_cast<std::size_t>(length));
        if (status_t const queued = writer.enqueue_write(std::move(buffer), extent_t {offset, length}); queued.failed())
            return queued;
        if (status_t const submitted = writer.submit(); submitted.failed()) return submitted;
        offset += length;
    }
    return writer.make_durable();
}

/** Mounts volume zero of the fixture of @p stack again into a fresh router, as a process starting after a crash
 *  does. */
template <typename stack_type_>
status_t remount_volume(stack_type_& stack) noexcept {
    auto [built, make_status] = basic_volume_router<typename stack_type_::database_t::volume_t>::make();
    if (make_status.failed()) return make_status;
    stack.router = std::move(built);
    auto [guard, acquire_status] = stack.fixture->channels.acquire();
    if (acquire_status.failed()) return acquire_status;
    auto volume = stack.fixture->mount_volume(guard.channel(), 0, volume_recovery_t::refuse_k);
    if (!volume) return volume.status();
    return stack.router.bind_queue_pair(std::move(*volume));
}

#pragma endregion Shape

#pragma region Rows

/** The counters every row of a case carries, one per key, which outlive the rows viewing them. */
struct counted_rows_t {
    st::basic_vector<std::array<byte_t, 8>> values;
    st::basic_vector<row_t> rows;
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
[[nodiscard]] st::basic_vector<stored_key_t> keys_between(stored_key_t first, stored_key_t last) noexcept {
    st::basic_vector<stored_key_t> keys;
    [[maybe_unused]] st::status_t const sized = keys.resize(static_cast<std::size_t>(last - first));
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = first + index;
    return keys;
}

/** Ingests @p rows into @p collection of @p store in appends of @p batch rows, as an engine hands over the batches of
 *  a stream, at @p durability. */
template <typename database_type_>
expected<transaction_stamp_t> ingest_rows(database_type_& store, collection_id_t collection, counted_rows_t const& rows,
                                          ustore_durability_t durability, std::size_t batch = 7) noexcept {
    typename database_type_::ingest_t ingest;
    if (status_t const begun = store.begin_ingest(any_collection_id_t {collection}, ingest); begun.failed())
        return begun;
    std::span<row_t const> const all {rows.rows};
    for (std::size_t first = 0; first < all.size(); first += batch) {
        status_t const written = store.write_ingest(ingest, all.subspan(first, std::min(batch, all.size() - first)));
        if (written.failed()) {
            store.drop_ingest(ingest);
            return written;
        }
    }
    return store.publish_ingest(ingest, durability);
}

/** Keys of @p keys @p store answers @p value for. */
template <typename reader_type_>
[[nodiscard]] std::size_t count_valued(reader_type_& reader, collection_id_t collection,
                                       std::span<stored_key_t const> keys, std::uint64_t value) noexcept {
    std::size_t found = 0;
    for (stored_key_t const key : keys) {
        std::array<byte_t, 8> into {};
        stored_value_size_t size = 0;
        if (reader.select(any_collection_id_t {collection}, key, std::span<byte_t> {into}, size).failed()) continue;
        found += counter_of(std::span<byte_t const> {into.data(), size}) == value;
    }
    return found;
}

/** Runs every level of @p collection's tree holds, shallowest first, into @p into. */
template <typename database_type_>
status_t level_runs(database_type_& store, collection_id_t collection, st::basic_vector<std::size_t>& into) noexcept {
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
std::size_t empty_collection_lands_on_a_sorted_level(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    expected<collection_id_t> const fresh = create_named(*(*stack)->store, "fresh", test_config(8, 0));
    failures += expect(fresh.has_value());
    if (!fresh) return failures;

    st::basic_vector<stored_key_t> const keys = keys_between(0, 300);
    expected<transaction_stamp_t> const landed = ingest_rows(
        *(*stack)->store, *fresh, rows_of(std::span<stored_key_t const> {keys}, 9), ustore_durability_buffered_k);
    failures += expect(landed.has_value());
    st::basic_vector<std::size_t> runs;
    failures += expect(level_runs(*(*stack)->store, *fresh, runs).succeeded());
    failures += expect(runs.size() > 1);
    if (runs.size() <= 1) return failures;
    failures += expect_eq(runs[0], std::size_t {0});
    failures += expect_eq(runs[runs.size() - 1], std::size_t {1});
    failures += expect_eq(count_valued(*(*stack)->store, *fresh, std::span<stored_key_t const> {keys}, 9), keys.size());
    return failures;
}

/** Keys a run on level one already holds take an ingest on level zero, above it, so the ingest answers them. */
template <typename mount_type_>
std::size_t overlap_at_level_one_lands_on_level_zero(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    typename types_t::database_t& store = *(*stack)->store;
    expected<collection_id_t> const deep = create_named(store, "deep", test_config(8, 0));
    failures += expect(deep.has_value());
    if (!deep) return failures;

    // Level zero is due at four runs, so four flushes and a merge leave every key on level one.
    st::basic_vector<stored_key_t> const keys = keys_between(0, 100);
    for (std::uint64_t round = 0; round != 4; ++round) {
        for (stored_key_t const key : keys) {
            std::array<byte_t, 8> const counter = counted(round);
            failures += expect(
                store.insert(any_collection_id_t {*deep}, key, stored_value_view_t {counter}).succeeded());
        }
        failures += expect(store.flush().succeeded());
    }
    failures += expect(store.compact().succeeded());
    st::basic_vector<std::size_t> runs;
    failures += expect(level_runs(store, *deep, runs).succeeded());
    failures += expect(runs.size() > 1 && runs[0] == 0 && runs[1] != 0);

    failures += expect(
        ingest_rows(store, *deep, rows_of(std::span<stored_key_t const> {keys}, 42), ustore_durability_buffered_k)
            .has_value());
    failures += expect(level_runs(store, *deep, runs).succeeded());
    failures += expect(!runs.empty() && runs[0] == 1);
    failures += expect_eq(count_valued(store, *deep, std::span<stored_key_t const> {keys}, 42), keys.size());
    return failures;
}

/** Appends far smaller than a run land together in one run, not one run each. */
template <typename mount_type_>
std::size_t appends_share_one_run(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    expected<collection_id_t> const coalesced = create_named(*(*stack)->store, "coalesced", test_config(8, 0));
    failures += expect(coalesced.has_value());
    if (!coalesced) return failures;

    st::basic_vector<stored_key_t> const keys = keys_between(0, 8000);
    failures += expect(ingest_rows(*(*stack)->store, *coalesced, rows_of(std::span<stored_key_t const> {keys}, 3),
                                   ustore_durability_buffered_k, 1000)
                           .has_value());
    failures += expect_eq(run_count_of((*stack)->router.volume(volume_id_t {0})), std::size_t {1});
    failures += expect_eq(count_valued(*(*stack)->store, *coalesced, std::span<stored_key_t const> {keys}, 3),
                          keys.size());
    return failures;
}

#pragma endregion Placement Cases

#pragma region Durability Cases

/** What a crash right after an ingest leaves: all of it or none of it when buffered, all of it when flushed. */
template <typename mount_type_>
std::size_t crash_after_ingest(mount_type_ const& mount, ustore_durability_t durability) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using guard_t = typename types_t::guard_t;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    expected<collection_id_t> const landed = create_named(*(*stack)->store, "landed", test_config(8, 0));
    failures += expect(landed.has_value());
    if (!landed) return failures;
    st::basic_vector<stored_key_t> const keys = keys_between(0, 200);
    failures += expect(
        ingest_rows(*(*stack)->store, *landed, rows_of(std::span<stored_key_t const> {keys}, 5), durability)
            .has_value());

    st::basic_vector<byte_t> image;
    {
        expected<guard_t> guard = (*stack)->fixture->channels.acquire();
        failures += expect(guard.has_value());
        if (!guard) return failures;
        failures += expect(capture_medium(*(*stack)->fixture, guard->channel(), image).succeeded());
    }
    (*stack)->store.reset();
    {
        expected<guard_t> guard = (*stack)->fixture->channels.acquire();
        failures += expect(guard.has_value());
        if (!guard) return failures;
        failures += expect(
            restore_medium(*(*stack)->fixture, guard->channel(), std::span<byte_t const> {image}).succeeded());
    }
    failures += expect(remount_volume(**stack).succeeded());
    expected<std::unique_ptr<database_t>> reopened = open_database(**stack);
    failures += expect(reopened.has_value());
    if (!reopened) return failures;
    std::size_t const kept = count_valued(**reopened, *landed, std::span<stored_key_t const> {keys}, 5);
    if (durability == ustore_durability_flushed_k) failures += expect_eq(kept, keys.size());
    else failures += expect(kept == 0 || kept == keys.size());
    return failures;
}

/** A buffered ingest a crash catches before any flush reopens whole or not at all. */
template <typename mount_type_>
case_result_t buffered_ingest_is_all_or_nothing(mount_type_ const& mount) {
    if (mount.transport == transport_t::device_k)
        return case_result_t::skipped("a block device's medium is not one a case may copy", 0);
    return crash_after_ingest(mount, ustore_durability_buffered_k);
}

/** A flushed ingest survives a crash right after it returns. */
template <typename mount_type_>
case_result_t flushed_ingest_survives_reopen(mount_type_ const& mount) {
    if (mount.transport == transport_t::device_k)
        return case_result_t::skipped("a block device's medium is not one a case may copy", 0);
    return crash_after_ingest(mount, ustore_durability_flushed_k);
}

#pragma endregion Durability Cases

#pragma region Isolation Cases

/** A snapshot pinned before an ingest reads the value the key held before it. */
template <typename mount_type_>
std::size_t snapshot_reads_the_older_value(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;
    expected<collection_id_t> const held = create_named(store, "held", test_config(8, 0));
    failures += expect(held.has_value());
    if (!held) return failures;

    std::array<byte_t, 8> const older = counted(1);
    failures += expect(
        store.insert(any_collection_id_t {*held}, stored_key_t {5}, stored_value_view_t {older}).succeeded());
    expected<typename database_t::snapshot_t> taken = store.pin_stamp();
    failures += expect(taken.has_value());
    if (!taken) return failures;
    std::array<stored_key_t, 1> const key {5};
    failures += expect(
        ingest_rows(store, *held, rows_of(std::span<stored_key_t const> {key}, 2), ustore_durability_buffered_k)
            .has_value());

    failures += expect_eq(count_valued(*taken, *held, std::span<stored_key_t const> {key}, 1), std::size_t {1});
    failures += expect_eq(count_valued(store, *held, std::span<stored_key_t const> {key}, 2), std::size_t {1});
    return failures;
}

/** A transaction that started before an ingest and wrote one of its keys fails at commit. */
template <typename mount_type_>
std::size_t older_transaction_conflicts(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;
    expected<collection_id_t> const contested = create_named(store, "contested", test_config(8, 0));
    failures += expect(contested.has_value());
    if (!contested) return failures;

    expected<typename database_t::transaction_unique_ptr_t> older = store.create_transaction();
    failures += expect(older.has_value());
    if (!older) return failures;
    std::array<byte_t, 8> const mine = counted(1);
    failures += expect(
        (*older)->insert(any_collection_id_t {*contested}, stored_key_t {5}, stored_value_view_t {mine}).succeeded());
    std::array<stored_key_t, 1> const key {5};
    failures += expect(
        ingest_rows(store, *contested, rows_of(std::span<stored_key_t const> {key}, 2), ustore_durability_buffered_k)
            .has_value());
    failures += expect_status((*older)->commit(), status_code_t::write_conflict_k);
    failures += expect_eq(count_valued(store, *contested, std::span<stored_key_t const> {key}, 2), std::size_t {1});
    return failures;
}

#pragma endregion Isolation Cases

#pragma region Refusal Cases

/** Keys that do not strictly ascend are refused, leaving nothing of the ingest behind, no run included. */
template <typename mount_type_>
std::size_t descending_key_is_refused(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    expected<collection_id_t> const refused = create_named(*(*stack)->store, "refused", test_config(8, 0));
    failures += expect(refused.has_value());
    if (!refused) return failures;

    st::basic_vector<stored_key_t> keys = keys_between(0, 20);
    keys[19] = 3;
    counted_rows_t const rows = rows_of(std::span<stored_key_t const> {keys}, 7);
    expected<transaction_stamp_t> const landed = ingest_rows(*(*stack)->store, *refused, rows,
                                                             ustore_durability_buffered_k);
    failures += expect_status(landed, status_code_t::invalid_argument_k);
    failures += expect_eq(count_valued(*(*stack)->store, *refused, std::span<stored_key_t const> {keys}, 7),
                          std::size_t {0});
    failures += expect_eq(run_count_of((*stack)->router.volume(volume_id_t {0})), std::size_t {0});
    return failures;
}

#pragma endregion Refusal Cases

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
        report(totals, mount, "empty_lands_on_a_sorted_level", empty_collection_lands_on_a_sorted_level);
        report(totals, mount, "overlap_at_level_one_lands_on_zero", overlap_at_level_one_lands_on_level_zero);
        report(totals, mount, "appends_share_one_run", appends_share_one_run);
        report(totals, mount, "buffered_is_all_or_nothing", buffered_ingest_is_all_or_nothing);
        report(totals, mount, "flushed_survives_reopen", flushed_ingest_survives_reopen);
        report(totals, mount, "snapshot_reads_the_older_value", snapshot_reads_the_older_value);
        report(totals, mount, "older_transaction_conflicts", older_transaction_conflicts);
        report(totals, mount, "descending_key_is_refused", descending_key_is_refused);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, ingest_config_k, 1, "ingest", cases);

    return suite_exit_code(totals);
}
