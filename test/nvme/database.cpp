/**
 *  @file test/nvme/database.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The database over the collections: what the catalog persists, and what a commit makes visible.
 *
 *  The results this file exists for are @c commit_is_all_or_nothing and @c threads_commit_serializably.
 *  A transaction spanning two collections must never be seen half applied by a reader running beside
 *  it, and several threads racing over the same keys must leave a state one serial order explains.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcmp`, `std::memcpy`

#include <array>       // `std::array`
#include <chrono>      // `std::chrono::steady_clock`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <thread>      // `std::thread`
#include <type_traits> // `std::is_same_v`
#include <utility>     // `std::move`

#include <forkunion/capabilities.hpp>  // `fu::preferred_yield_t`
#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_database.hpp"    // `basic_database`
#include "ustore/strings/nvme_store.hpp"     // `strings::nvme_store`, `strings::nvme_transaction`
#include "ustore/utilities/nvme_catalog.hpp" // `blob_kind_t`, `blob_name_t`
#include "ustore/utilities/nvme_volume.hpp"  // `basic_volume_router`, `volume_blob_t`, `volume_id_t`

#include "nvme/fixture.hpp" // `run_on_transport`, `painted`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Shape

/** The numbers every case mounts with: channels for a compaction to hold two, and one for every thread a case
 *  spawns. */
constexpr fixture_config_t database_config_k {.shared_io_channels = 8};

/** The largest value the variable-width collections under test admit. */
constexpr stored_value_size_t value_max_size_k = 4096;

/** The keys every case of this suite drives, and the two ways a collection is named. */
using stored_key_t = std::uint64_t;
using any_collection_id_t = std::variant<collection_id_t, collection_alphanumeric_id_t>;

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
    using strings_kit_t = utilities::strings_ladder<sz_capabilities_compiled_cpu_mask()>::strings_kit_t;
    using strings_store_t = strings::nvme_store<volume_t, atomic_reference, waiter_t, strings_kit_t>;
    template <typename... store_types_>
    using basic_database_of = basic_database<stored_key_t, volume_t, atomic_reference, waiter_t,
                                             layout_store_tags<store_types_...>, store_types_...>;
    using database_t = basic_database_of<strings_store_t, blobs_store_of<value_layout_t::inline_12_bytes_k>,
                                         blobs_store_of<value_layout_t::inline_44_bytes_k>,
                                         blobs_store_of<value_layout_t::fixed_width_k>,
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

/** The collection name @p text, owned, as a lookup by name takes it; a suite's literals always fit. */
string_t owned_name(std::string_view text) noexcept {
    string_t owned;
    [[maybe_unused]] sz::status_t const assigned = owned.assign(string_view_t {text});
    return owned;
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

/** Bytes the variable-width value of @p key takes, which differs from key to key so a short read shows. */
[[nodiscard]] std::size_t blob_bytes_of(std::uint64_t key) noexcept { return static_cast<std::size_t>(200 + key % 97); }

/** The three collections one reopen case writes, which is one per value layout it covers. */
struct painted_collections_t {

    /** The inline collection, whose value is one counter. */
    collection_id_t numbers = collection_invalid_id_k;

    /** The variable-width collection, whose values differ in length from key to key. */
    collection_id_t blobs = collection_invalid_id_k;

    /** The fixed-width collection, whose values are always the same length. */
    collection_id_t fixed = collection_invalid_id_k;
};

/** Writes @p key into all three collections of @p store, painted from @p round. */
template <typename database_type_>
[[nodiscard]] std::size_t write_painted(database_type_& store, painted_collections_t const& named, std::uint64_t key,
                                        std::uint64_t round) {
    std::size_t failures = 0;
    std::array<byte_t, 8> const counter = counted(key * 7 + round);
    failures += expect(
        store.insert(any_collection_id_t {named.numbers}, static_cast<stored_key_t>(key), stored_value_view_t {counter})
            .succeeded());

    std::array<byte_t, 512> value {};
    std::size_t const width = blob_bytes_of(key);
    for (std::size_t position = 0; position != width; ++position)
        value[position] = painted(key + round * 1000, position);
    failures += expect(store
                           .insert(any_collection_id_t {named.blobs}, static_cast<stored_key_t>(key),
                                   stored_value_view_t {value.data(), width})
                           .succeeded());

    for (std::size_t position = 0; position != value.size(); ++position)
        value[position] = painted(key + round * 1000 + 99, position);
    failures += expect(
        store.insert(any_collection_id_t {named.fixed}, static_cast<stored_key_t>(key), stored_value_view_t {value})
            .succeeded());
    return failures;
}

/** Reads @p key back out of all three collections and checks every byte against @p round. */
template <typename database_type_>
[[nodiscard]] std::size_t check_painted(database_type_& store, painted_collections_t const& named, std::uint64_t key,
                                        std::uint64_t round) {
    std::size_t failures = 0;
    std::array<byte_t, 1024> into {};
    stored_value_size_t size = 0;
    failures += expect(
        store
            .select(any_collection_id_t {named.numbers}, static_cast<stored_key_t>(key), std::span<byte_t> {into}, size)
            .succeeded());
    failures += expect_eq(counter_of(std::span<byte_t const> {into.data(), size}), key * 7 + round);

    std::size_t mismatches = 0;
    failures += expect(
        store.select(any_collection_id_t {named.blobs}, static_cast<stored_key_t>(key), std::span<byte_t> {into}, size)
            .succeeded());
    failures += expect_eq(std::size_t {size}, blob_bytes_of(key));
    for (std::uint64_t position = 0; position != size; ++position)
        if (into[static_cast<std::size_t>(position)] != painted(key + round * 1000, position)) ++mismatches;

    failures += expect(
        store.select(any_collection_id_t {named.fixed}, static_cast<stored_key_t>(key), std::span<byte_t> {into}, size)
            .succeeded());
    failures += expect_eq(std::size_t {size}, std::size_t {512});
    for (std::uint64_t position = 0; position != size; ++position)
        if (into[static_cast<std::size_t>(position)] != painted(key + round * 1000 + 99, position)) ++mismatches;
    failures += expect_eq(mismatches, std::size_t {0});
    return failures;
}

/** Sorted runs @p volume names right now, which a reopen neither adds to nor loses. */
template <typename volume_type_>
[[nodiscard]] std::size_t run_count_of(volume_type_ const& volume) noexcept {
    std::size_t counted = 0;
    volume.for_each(blob_kind_t::sorted_run_k, [&](blob_name_t, volume_blob_t const&) noexcept { ++counted; });
    return counted;
}

/** Runs the copy of the catalog blob on @p volume that save @p generation wrote names, counted off the length its
 *  fixed-width records fix, for a database whose runs no ingest stamped. */
template <typename volume_type_>
[[nodiscard]] std::size_t runs_named_by_schema(volume_type_ const& volume, std::size_t collections,
                                               std::uint64_t generation) noexcept {
    expected<volume_blob_t> const found = volume.find(database_schema_name(schema_copy_of(generation)));
    // The version, then the counts of the records, of the runs and of the ingested runs.
    std::size_t const fixed = 4 * sizeof(std::uint64_t) + sizeof(schema_stamps_t) +
                              collections * sizeof(collection_record_t);
    if (!found || found->length < fixed) return 0;
    return static_cast<std::size_t>((found->length - fixed) / sizeof(lsm_tree_run_record_t));
}

/** Bytes every sorted run of @p volume takes, which is what dropping a collection hands back. */
template <typename volume_type_>
[[nodiscard]] std::uint64_t run_bytes_of(volume_type_ const& volume) noexcept {
    std::uint64_t counted = 0;
    volume.for_each(blob_kind_t::sorted_run_k,
                    [&](blob_name_t, volume_blob_t const& blob) noexcept { counted += blob.extent.length; });
    return counted;
}

#pragma endregion Shape

#pragma region Fixtures

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
status_t capture_medium(fixture_type_& fixture, io_channel_t& channel, st::vector<byte_t>& image) noexcept {
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
    auto [built, make_status] = decltype(stack.router)::make();
    if (make_status.failed()) return make_status;
    stack.router = std::move(built);
    auto [guard, acquire_status] = stack.fixture->channels.acquire();
    if (acquire_status.failed()) return acquire_status;
    auto volume = stack.fixture->mount_volume(guard.channel(), 0, volume_recovery_t::refuse_k);
    if (!volume) return volume.status();
    return stack.router.bind_queue_pair(std::move(*volume));
}

#pragma endregion Fixtures

#pragma region Catalog Cases

/** Several named collections of different layouts take writes and answer them back. */
template <typename mount_type_>
std::size_t named_collections_round_trip(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;

    expected<collection_id_t> const numbers = create_named(store, "numbers", test_config(8, 0));
    expected<collection_id_t> const blobs = create_named(store, "blobs", test_config(0, value_max_size_k));
    expected<collection_id_t> const fixed = create_named(store, "fixed", test_config(512, 0));
    failures += expect(numbers.has_value());
    failures += expect(blobs.has_value());
    failures += expect(fixed.has_value());
    if (failures != 0) return failures;

    // A name is taken once, and a collection nobody created is named by nothing.
    failures += expect_status(create_named(store, "numbers", test_config(8, 0)), status_code_t::already_exists_k);
    failures += expect(store.has_collection(any_collection_id_t {owned_name("blobs")}));
    failures += expect(!store.has_collection(any_collection_id_t {owned_name("missing")}));
    failures += expect_eq(store.collection_id(any_collection_id_t {owned_name("numbers")}), *numbers);
    failures += expect_eq(store.list_collections().size(), std::size_t {3});
    failures += expect_eq(store.size(), std::size_t {3});

    constexpr std::size_t keys_k = 200;
    st::vector<byte_t> value;
    failures += expect(value.resize(value_max_size_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != keys_k; ++index) {
        std::array<byte_t, 8> const counter = counted(index * 7 + 1);
        failures += expect(
            store
                .insert(any_collection_id_t {*numbers}, static_cast<stored_key_t>(index), stored_value_view_t {counter})
                .succeeded());

        std::size_t const width = 1 + index % 1000;
        for (std::size_t position = 0; position != width; ++position) value[position] = painted(index, position);
        failures += expect(store
                               .insert(any_collection_id_t {*blobs}, static_cast<stored_key_t>(index),
                                       stored_value_view_t {value.data(), width})
                               .succeeded());

        for (std::size_t position = 0; position != 512; ++position) value[position] = painted(index + 99, position);
        failures += expect(store
                               .insert(any_collection_id_t {*fixed}, static_cast<stored_key_t>(index),
                                       stored_value_view_t {value.data(), 512})
                               .succeeded());
    }
    failures += expect(store.flush().succeeded());

    // A value of the wrong width is refused by the layout rather than stored short.
    failures += expect_status(
        store.insert(any_collection_id_t {*fixed}, stored_key_t {9999}, stored_value_view_t {value.data(), 511}),
        status_code_t::malformed_k);

    st::vector<byte_t> into;
    failures += expect(into.resize(value_max_size_k) == st::status_t::success_k);
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index != keys_k; ++index) {
        stored_value_size_t size = 0;
        failures += expect(store
                               .select(any_collection_id_t {*numbers}, static_cast<stored_key_t>(index),
                                       std::span<byte_t> {into}, size)
                               .succeeded());
        if (counter_of(std::span<byte_t const> {into.data(), size}) != index * 7 + 1) ++mismatches;

        failures += expect(
            store.select(any_collection_id_t {*blobs}, static_cast<stored_key_t>(index), std::span<byte_t> {into}, size)
                .succeeded());
        if (size != 1 + index % 1000) ++mismatches;
        for (std::uint64_t position = 0; position != size; ++position)
            if (into[static_cast<std::size_t>(position)] != painted(index, position)) ++mismatches;

        failures += expect(
            store.select(any_collection_id_t {*fixed}, static_cast<stored_key_t>(index), std::span<byte_t> {into}, size)
                .succeeded());
        if (size != 512) ++mismatches;
    }
    failures += expect_eq(mismatches, std::size_t {0});

    // One batched select answers every key of a collection at once, misses included.
    std::array<stored_key_t, 3> const batch {stored_key_t {0}, stored_key_t {keys_k - 1}, stored_key_t {keys_k + 5}};
    std::array<lookup_result_t, 3> answers {lookup_result_t::missing_k, lookup_result_t::missing_k,
                                            lookup_result_t::missing_k};
    std::size_t carried = 0;
    auto sink = [&](std::size_t, std::uint64_t, std::span<byte_t const> bytes) noexcept { carried += bytes.size(); };
    failures += expect(store
                           .select(any_collection_id_t {*blobs}, std::span<stored_key_t const> {batch},
                                   std::span<lookup_result_t> {answers}, sink)
                           .succeeded());
    failures += expect(answers[0] == lookup_result_t::written_k);
    failures += expect(answers[1] == lookup_result_t::written_k);
    failures += expect(answers[2] == lookup_result_t::missing_k);
    failures += expect_eq(carried, std::size_t {1} + (1 + (keys_k - 1) % 1000));

    // A removal hides the key in the collection it names and leaves the others alone.
    failures += expect(store.remove(any_collection_id_t {*numbers}, stored_key_t {0}).succeeded());
    failures += expect(!store.contains(any_collection_id_t {*numbers}, stored_key_t {0}));
    failures += expect(store.contains(any_collection_id_t {*blobs}, stored_key_t {0}));
    return failures;
}

/**
 *  @brief The catalog and every byte the collections held survive a close and a reopen.
 *
 *  Three layouts are written twice, merged into a second level and written over once more, so a reopen
 *  that took back only the newest runs, or only level zero, would answer the wrong bytes for the keys
 *  the merge buried.
 */
template <typename mount_type_>
std::size_t catalog_survives_reopen(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::uint64_t keys_k = 300;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t numbers_config = test_config(8, 0);
    collection_config_t blobs_config = test_config(0, value_max_size_k);
    collection_config_t fixed_config = test_config(512, 0);
    // The smallest factor makes level zero due at two flushed runs, so two rounds give the tree depth.
    numbers_config.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;
    blobs_config.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;
    fixed_config.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;

    expected<collection_id_t> const numbers = create_named(*(*stack)->store, "numbers", numbers_config);
    expected<collection_id_t> const blobs = create_named(*(*stack)->store, "blobs", blobs_config);
    expected<collection_id_t> const fixed = create_named(*(*stack)->store, "fixed", fixed_config);
    failures += expect(numbers.has_value());
    failures += expect(blobs.has_value());
    failures += expect(fixed.has_value());
    if (failures != 0) return failures;
    painted_collections_t const named {*numbers, *blobs, *fixed};

    // Two flushed rounds are merged into a second level, and a third round lands on level zero over them.
    for (std::uint64_t round = 0; round != 2; ++round) {
        for (std::uint64_t key = 0; key != keys_k; ++key)
            failures += write_painted(*(*stack)->store, named, key, round);
        failures += expect((*stack)->store->flush().succeeded());
    }
    failures += expect((*stack)->store->compact().succeeded());

    // A merge records its new set before it drops what it consumed, so neither outlives the other.
    failures += expect_eq(
        runs_named_by_schema((*stack)->router.volume(volume_id_t {0}), 3, (*stack)->store->schema_generation()),
        run_count_of((*stack)->router.volume(volume_id_t {0})));
    for (std::uint64_t key = 0; key < keys_k; key += 3) failures += write_painted(*(*stack)->store, named, key, 2);
    failures += expect((*stack)->store->flush().succeeded());
    if (failures != 0) return failures;

    // The runs and their bytes are what a reopen must take back, level for level.
    std::size_t const runs_before = run_count_of((*stack)->router.volume(volume_id_t {0}));
    std::uint64_t const bytes_before = (*stack)->store->size_bytes();
    failures += expect(runs_before != std::size_t {0});

    // A key of every third is answered from level zero, and every other key from the level below it.
    auto round_of = [](std::uint64_t key) noexcept -> std::uint64_t { return key % 3 == 0 ? 2 : 1; };
    for (std::uint64_t key = 0; key != keys_k; ++key)
        failures += check_painted(*(*stack)->store, named, key, round_of(key));

    failures += expect((*stack)->store->unmount().succeeded());
    (*stack)->store.reset();

    expected<std::unique_ptr<database_t>> reopened = open_database(**stack);
    failures += expect(reopened.has_value());
    if (!reopened) return failures;
    database_t& store = **reopened;

    failures += expect_eq(store.size(), std::size_t {3});
    failures += expect_eq(store.collection_id(any_collection_id_t {owned_name("numbers")}), *numbers);
    failures += expect_eq(store.collection_id(any_collection_id_t {owned_name("blobs")}), *blobs);
    failures += expect(store.collection_name(*blobs) == owned_name("blobs"));

    collection_config_t restored;
    failures += expect(store.collection_config(*blobs, restored).succeeded());
    failures += expect_eq(restored.lsm_tree.lsm_level_0.sorted_run.value_max_size, value_max_size_k);
    failures += expect_eq(restored.memory_runs_max_deltas, std::size_t {4096});
    failures += expect_eq(store.buffer_size_for_any_stored_value(any_collection_id_t {*numbers}),
                          stored_value_size_t {12});
    std::uint16_t store_tag = 0;
    failures += expect(store.collection_store_tag(*blobs, store_tag).succeeded());
    failures += expect_eq(store_tag, static_cast<std::uint16_t>(value_layout_t::variable_width_k));

    // Every run the close left behind is back, and the reads below tell their levels apart.
    failures += expect_eq(run_count_of((*stack)->router.volume(volume_id_t {0})), runs_before);
    failures += expect_eq(store.size_bytes(), bytes_before);
    for (std::uint64_t key = 0; key != keys_k; ++key) failures += check_painted(store, named, key, round_of(key));

    // A reopened collection takes writes and answers them, which is what makes the catalog useful.
    for (std::uint64_t key = 0; key != 4; ++key) failures += write_painted(store, named, key, 3);
    for (std::uint64_t key = 0; key != 4; ++key) failures += check_painted(store, named, key, 3);
    failures += expect(store.unmount().succeeded());
    (*reopened).reset();

    // A database of fewer stores refuses a catalog naming a store it lacks, and leaves it whole for one that has.
    using numbers_only_t = typename types_t::template basic_database_of<
        typename types_t::template blobs_store_of<value_layout_t::inline_12_bytes_k>>;
    {
        numbers_only_t narrow {(*stack)->fixture->resources};
        failures += expect_status(narrow.mount((*stack)->router, (*stack)->fixture->channels, test_database_config()),
                                  status_code_t::malformed_k);
    }

    // A database listing its stores in another order mounts every collection in the store its tag names.
    using reordered_t = typename types_t::template basic_database_of<
        typename types_t::template blobs_store_of<value_layout_t::variable_width_k>,
        typename types_t::template blobs_store_of<value_layout_t::fixed_width_k>,
        typename types_t::template blobs_store_of<value_layout_t::inline_12_bytes_k>>;
    {
        reordered_t reordered {(*stack)->fixture->resources};
        failures += expect(
            reordered.mount((*stack)->router, (*stack)->fixture->channels, test_database_config()).succeeded());
        failures += expect_eq(reordered.size(), std::size_t {3});
        failures += expect_eq(reordered.buffer_size_for_any_stored_value(any_collection_id_t {*numbers}),
                              stored_value_size_t {12});
        for (std::uint64_t key = 0; key != 4; ++key) failures += check_painted(reordered, named, key, 3);
        failures += expect(reordered.unmount().succeeded());
    }
    expected<std::unique_ptr<database_t>> again = open_database(**stack);
    failures += expect(again.has_value());
    if (!again) return failures;
    failures += expect_eq((*again)->size(), std::size_t {3});
    for (std::uint64_t key = 0; key != 4; ++key) failures += check_painted(**again, named, key, 3);
    return failures;
}

/** A strings collection, as the engine's names system collection is one, survives a reopen: strings sharing their
 *  15-byte heads map onto the same 16-byte keys, answered from the 32-byte cells of a flushed run, whose header records
 *  the width of its cells and holds no values section. */
template <typename mount_type_>
std::size_t strings_survive_reopen(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    using strings_store_t = typename types_t::strings_store_t;
    using key_slot_t = typename strings_store_t::key_slot_t;
    constexpr std::size_t strings_k = 64;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_id_t names;
    std::memcpy(names.uuid.bytes, uuid_generator().bytes, sizeof(names.uuid.bytes));
    failures += expect((*stack)
                           ->store
                           ->create_collection(names, "names", static_cast<std::uint16_t>(value_layout_t::uuid_k),
                                               test_config(sizeof(ustore_uuid_t), 0))
                           .succeeded());
    if (failures != 0) return failures;

    std::array<std::string, strings_k> texts;
    std::array<std::array<byte_t, key_slot_t::size>, strings_k> keys;
    for (std::size_t index = 0; index != strings_k; ++index) {
        texts[index] = "collection-name" + std::to_string(index);
        for (std::size_t position = 0; position != key_slot_t::size; ++position)
            keys[index][position] = painted(index, position);
    }
    {
        strings::nvme_transaction<database_t, strings_store_t> written {*(*stack)->store};
        auto [held, begin_status] = (*stack)->store->create_transaction(test_database_config().default_transaction);
        failures += expect(begin_status.succeeded());
        if (failures != 0) return failures;
        written.join(*held);
        for (std::size_t index = 0; index != strings_k; ++index)
            failures += expect(written
                                   .write(names, texts[index], delta_view_t {delta_kind_t::write_k, keys[index]},
                                          invalid_transaction_stamp_k)
                                   .succeeded());
        failures += expect(written.commit(*held).succeeded());
    }
    failures += expect((*stack)->store->flush().succeeded());
    failures += expect((*stack)->store->unmount().succeeded());
    (*stack)->store.reset();

    expected<std::unique_ptr<database_t>> reopened = open_database(**stack);
    failures += expect(reopened.has_value());
    if (!reopened) return failures;
    failures += expect_eq((*reopened)->collection_id(any_collection_id_t {owned_name("names")}), names);
    status_t const visited = (*reopened)->visit_store(
        any_collection_id_t {names}, [&]<typename store_type_>(store_type_& store) noexcept -> status_t {
            if constexpr (!std::is_same_v<store_type_, strings_store_t>)
                return {status_code_t::invalid_argument_k, "A strings collection mounted in another store"};
            else {
                typename strings_store_t::tree_t::level_t const& level = store.tree().levels().level(0);
                failures += expect_eq(level.size(), std::size_t {1});
                if (level.size() == 1) {
                    failures += expect_eq(level.run(0).header().key_slot_bytes, std::uint32_t {32});
                    failures += expect(level.run(0).header().value_layout == value_layout_t::uuid_k);
                    failures += expect_eq(level.run(0).header().stored_values_bytes, std::uint64_t {0});
                }
                for (std::size_t index = 0; index != strings_k; ++index) {
                    std::array<byte_t, key_slot_t::size> value {};
                    failures += expect(store.find(texts[index], value) == lookup_result_t::written_k);
                    failures += expect(value == keys[index]);
                }
                std::array<byte_t, key_slot_t::size> missing {};
                failures += expect(store.find("collection-name", missing) == lookup_result_t::missing_k);
                return {};
            }
        });
    failures += expect(visited.succeeded());
    return failures;
}

/** A dropped collection is gone after a reopen, and the bytes its runs held are free again. */
template <typename mount_type_>
std::size_t dropped_collection_is_gone(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using volume_t = typename types_t::volume_t;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    volume_t& volume = (*stack)->router.volume(volume_id_t {0});

    expected<collection_id_t> const kept = create_named(*(*stack)->store, "kept", test_config(8, 0));
    expected<collection_id_t> const doomed = create_named(*(*stack)->store, "doomed", test_config(0, value_max_size_k));
    failures += expect(kept.has_value());
    failures += expect(doomed.has_value());
    if (failures != 0) return failures;

    st::vector<byte_t> value;
    failures += expect(value.resize(value_max_size_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != 400; ++index) {
        for (std::size_t position = 0; position != value.size(); ++position) value[position] = painted(index, position);
        failures += expect(
            (*stack)
                ->store
                ->insert(any_collection_id_t {*doomed}, static_cast<stored_key_t>(index), stored_value_view_t {value})
                .succeeded());
    }
    failures += expect((*stack)->store->flush().succeeded());
    failures += expect(run_bytes_of(volume) != std::uint64_t {0});

    // What a drop must reclaim are runs this database took back from its schema rather than wrote itself.
    failures += expect((*stack)->store->unmount().succeeded());
    (*stack)->store.reset();
    expected<std::unique_ptr<database_t>> reopened = open_database(**stack);
    failures += expect(reopened.has_value());
    if (!reopened) return failures;

    std::uint64_t const held_bytes = run_bytes_of(volume);
    std::uint64_t const free_before = volume.free_bytes();
    failures += expect(held_bytes != std::uint64_t {0});
    failures += expect((*reopened)->drop_collection(any_collection_id_t {*doomed}).succeeded());
    failures += expect(!(*reopened)->has_collection(any_collection_id_t {owned_name("doomed")}));
    failures += expect_eq(run_bytes_of(volume), std::uint64_t {0});
    failures += expect(volume.free_bytes() - free_before >= held_bytes);
    failures += expect_status((*reopened)->drop_collection(any_collection_id_t {*doomed}), status_code_t::not_found_k);

    failures += expect((*reopened)->unmount().succeeded());
    (*reopened).reset();
    expected<std::unique_ptr<database_t>> again = open_database(**stack);
    failures += expect(again.has_value());
    if (!again) return failures;

    failures += expect_eq((*again)->size(), std::size_t {1});
    failures += expect(!(*again)->has_collection(any_collection_id_t {owned_name("doomed")}));
    failures += expect((*again)->has_collection(any_collection_id_t {owned_name("kept")}));

    // The bytes the dropped runs held are handed out again rather than lost to the catalog.
    expected<collection_id_t> const fresh = create_named(**again, "fresh", test_config(0, value_max_size_k));
    failures += expect(fresh.has_value());
    if (!fresh) return failures;
    for (std::size_t index = 0; index != 400; ++index) {
        for (std::size_t position = 0; position != value.size(); ++position) value[position] = painted(index, position);
        failures += expect(
            (*again)
                ->insert(any_collection_id_t {*fresh}, static_cast<stored_key_t>(index), stored_value_view_t {value})
                .succeeded());
    }
    failures += expect((*again)->flush().succeeded());
    return failures;
}

/**
 *  @brief A mount sweeps away the runs no schema names and keeps the ones it does, and a schema naming a missing run
 *      is refused.
 *
 *  A flush seals the run first and records it second, so the only state a crash between the two leaves
 *  is a run nobody names. The other order would leave a schema naming bytes the volume never got, which
 *  the second half forces by hand to show what a reopen does with it.
 */
template <typename mount_type_>
std::size_t crash_between_seal_and_schema(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;

    {
        expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
        failures += expect_status(stack, status_code_t::success_k);
        if (!stack) return failures;
        volume_t& volume = (*stack)->router.volume(volume_id_t {0});

        expected<collection_id_t> const lost = create_named(*(*stack)->store, "lost", test_config(8, 0));
        failures += expect(lost.has_value());
        if (!lost) return failures;
        for (std::uint64_t key = 0; key != 200; ++key) {
            std::array<byte_t, 8> const counter = counted(key);
            failures += expect(
                (*stack)
                    ->store
                    ->insert(any_collection_id_t {*lost}, static_cast<stored_key_t>(key), stored_value_view_t {counter})
                    .succeeded());
        }
        failures += expect((*stack)->store->flush().succeeded());

        // A flush seals its runs and records them second, so the catalog already names every run.
        failures += expect_eq(runs_named_by_schema(volume, 1, (*stack)->store->schema_generation()),
                              run_count_of(volume));
        failures += expect((*stack)->store->unmount().succeeded());
        (*stack)->store.reset();

        std::size_t const runs = run_count_of(volume);
        failures += expect(runs != std::size_t {0});
        failures += expect(run_bytes_of(volume) != std::uint64_t {0});

        // A mount keeps every run the schema names.
        {
            expected<std::unique_ptr<database_t>> kept = open_database(**stack);
            failures += expect(kept.has_value());
            if (!kept) return failures;
            failures += expect_eq(run_count_of(volume), runs);
            failures += expect((*kept)->unmount().succeeded());
        }

        // No copy of the schema that would have named those runs ever landed.
        {
            expected<guard_t> guard = (*stack)->fixture->channels.acquire();
            failures += expect(guard.has_value());
            if (!guard) return failures;
            for (std::size_t copy = 0; copy != 2; ++copy)
                if (volume.names(database_schema_name(copy)))
                    failures += expect(volume.drop(guard->channel(), database_schema_name(copy)).succeeded());
        }

        expected<std::unique_ptr<database_t>> reopened = open_database(**stack);
        failures += expect(reopened.has_value());
        if (!reopened) return failures;
        failures += expect_eq((*reopened)->size(), std::size_t {0});
        failures += expect(!(*reopened)->has_collection(any_collection_id_t {owned_name("lost")}));
        failures += expect_eq(run_count_of(volume), std::size_t {0});
        failures += expect_eq(run_bytes_of(volume), std::uint64_t {0});

        // The volume is still a volume: a fresh collection writes, flushes and answers over it.
        expected<collection_id_t> const fresh = create_named(**reopened, "fresh", test_config(8, 0));
        failures += expect(fresh.has_value());
        if (!fresh) return failures;
        std::array<byte_t, 8> const counter = counted(77);
        failures += expect((*reopened)
                               ->insert(any_collection_id_t {*fresh}, stored_key_t {1}, stored_value_view_t {counter})
                               .succeeded());
        failures += expect((*reopened)->flush().succeeded());
        std::array<byte_t, 8> into {};
        stored_value_size_t size = 0;
        failures += expect((*reopened)
                               ->select(any_collection_id_t {*fresh}, stored_key_t {1}, std::span<byte_t> {into}, size)
                               .succeeded());
        failures += expect_eq(counter_of(std::span<byte_t const> {into.data(), size}), std::uint64_t {77});
    }

    {
        expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
        failures += expect_status(stack, status_code_t::success_k);
        if (!stack) return failures;
        volume_t& volume = (*stack)->router.volume(volume_id_t {0});

        expected<collection_id_t> const torn = create_named(*(*stack)->store, "torn", test_config(8, 0));
        failures += expect(torn.has_value());
        if (!torn) return failures;
        for (std::uint64_t key = 0; key != 200; ++key) {
            std::array<byte_t, 8> const counter = counted(key);
            failures += expect(
                (*stack)
                    ->store
                    ->insert(any_collection_id_t {*torn}, static_cast<stored_key_t>(key), stored_value_view_t {counter})
                    .succeeded());
        }
        failures += expect((*stack)->store->flush().succeeded());
        failures += expect((*stack)->store->unmount().succeeded());
        (*stack)->store.reset();

        std::array<byte_t, blob_name_max_bytes_k> named {};
        std::size_t named_bytes = 0;
        volume.for_each(blob_kind_t::sorted_run_k, [&](blob_name_t name, volume_blob_t const&) noexcept {
            if (named_bytes != 0) return;
            std::memcpy(named.data(), name.data(), name.size());
            named_bytes = name.size();
        });
        failures += expect(named_bytes != std::size_t {0});
        if (named_bytes == 0) return failures;

        // The bytes of one run the schema names never reached the environment.
        {
            expected<guard_t> guard = (*stack)->fixture->channels.acquire();
            failures += expect(guard.has_value());
            if (!guard) return failures;
            failures += expect(volume.drop(guard->channel(), blob_name_t {named.data(), named_bytes}).succeeded());
        }

        expected<std::unique_ptr<database_t>> const reopened = open_database(**stack);
        failures += expect_status(reopened, status_code_t::not_found_k);
    }
    return failures;
}

/**
 *  @brief A crash between a flush and the catalog save loses a suffix of commits and never half of one.
 *
 *  A commit lands in two collections; the first flushes it on its own while the second still buffers it, and a catalog
 *  save then names the first's run. The medium as it stands then is what a crash leaves: a reopen of it drops the
 *  commit from the run that carries it, every commit after it too, and keeps every commit before it whole.
 */
template <typename mount_type_>
case_result_t crash_keeps_a_prefix_of_commits(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using guard_t = typename types_t::guard_t;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    if (mount.transport == transport_t::device_k)
        return case_result_t::skipped("a block device's medium is not one a case may copy", failures);
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;

    // The first collection flushes once its buffer holds its few writes, the second never does on its own.
    collection_config_t narrow = test_config(8, 0);
    narrow.memory_runs_max_deltas = 4;
    expected<collection_id_t> const first = create_named(store, "first", narrow);
    expected<collection_id_t> const second = create_named(store, "second", test_config(8, 0));
    failures += expect(first.has_value() && second.has_value());
    if (failures != 0) return failures;
    any_collection_id_t const left {*first};
    any_collection_id_t const right {*second};

    auto commit_both = [&](std::uint64_t value) noexcept {
        expected<typename database_t::transaction_unique_ptr_t> transaction = store.create_transaction();
        if (!transaction) return std::size_t {1};
        std::array<byte_t, 8> const counter = counted(value);
        std::size_t missed = 0;
        missed += expect((*transaction)->insert(left, stored_key_t {0}, stored_value_view_t {counter}).succeeded());
        missed += expect((*transaction)->insert(right, stored_key_t {0}, stored_value_view_t {counter}).succeeded());
        missed += expect((*transaction)->commit().succeeded());
        return missed;
    };
    failures += commit_both(1);
    failures += expect(store.flush().succeeded());
    failures += commit_both(2);
    for (std::uint64_t key = 100; key != 104; ++key) {
        std::array<byte_t, 8> const counter = counted(key);
        failures += expect(
            store.insert(left, static_cast<stored_key_t>(key), stored_value_view_t {counter}).succeeded());
    }
    failures += expect(create_named(store, "saved", test_config(8, 0)).has_value());

    // Before the crash both collections answer the second commit, the first from a run, the second from its buffer.
    std::array<byte_t, 8> into {};
    stored_value_size_t size = 0;
    failures += expect(store.select(left, stored_key_t {0}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(counter_of(std::span<byte_t const> {into.data(), size}), std::uint64_t {2});
    st::vector<byte_t> image;
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
    if (failures != 0) return failures;

    // The first commit is whole, the second gone from both sides, and the writes after it gone with it.
    failures += expect((*reopened)->has_collection(any_collection_id_t {owned_name("saved")}));
    failures += expect((*reopened)->select(left, stored_key_t {0}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(counter_of(std::span<byte_t const> {into.data(), size}), std::uint64_t {1});
    failures += expect((*reopened)->select(right, stored_key_t {0}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(counter_of(std::span<byte_t const> {into.data(), size}), std::uint64_t {1});
    for (std::uint64_t key = 100; key != 104; ++key) failures += expect(!(*reopened)->contains(left, key));

    // A commit after the reopen lands under a fresh stamp and reads back.
    std::array<byte_t, 8> const counter = counted(3);
    failures += expect((*reopened)->insert(left, stored_key_t {0}, stored_value_view_t {counter}).succeeded());
    failures += expect((*reopened)->select(left, stored_key_t {0}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(counter_of(std::span<byte_t const> {into.data(), size}), std::uint64_t {3});
    return failures;
}

/** The bytes a caller saves next to the database survive a reopen and a clearing. */
template <typename mount_type_>
std::size_t user_data_round_trips(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    st::vector<byte_t> saved;
    failures += expect(saved.resize(3000) == st::status_t::success_k);
    for (std::size_t index = 0; index != saved.size(); ++index) saved[index] = painted(77, index);
    failures += expect((*stack)->store->save_user_data(bytes_view_t {saved}).succeeded());
    failures += expect_eq((*stack)->store->user_data_size_bytes(), std::uint64_t {3000});

    failures += expect((*stack)->store->unmount().succeeded());
    (*stack)->store.reset();
    expected<std::unique_ptr<database_t>> reopened = open_database(**stack);
    failures += expect(reopened.has_value());
    if (!reopened) return failures;

    st::vector<byte_t> loaded;
    failures += expect(loaded.resize(3000) == st::status_t::success_k);
    failures += expect((*reopened)->load_user_data(bytes_span_t {loaded}).succeeded());
    failures += expect_eq(std::memcmp(loaded.data(), saved.data(), saved.size()), 0);

    failures += expect((*reopened)->clear_user_data().succeeded());
    failures += expect_eq((*reopened)->user_data_size_bytes(), std::uint64_t {0});
    failures += expect((*reopened)->load_user_data(bytes_span_t {loaded}) == status_code_t::not_found_k);
    return failures;
}

#pragma endregion Catalog Cases

#pragma region Transaction Cases

/**
 *  @brief A transaction spanning two collections is never seen half applied.
 *
 *  Each round writes a whole band of keys into both collections, and their memory-run bounds are narrower
 *  than a band, so every commit lands past the bounds and flushes the band before it, and a reader has
 *  runs publishing beside it. The reader holds one level set per collection at one moment and counts
 *  every key that one of them carries and the other does not.
 */
template <typename mount_type_>
std::size_t commit_is_all_or_nothing(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t rounds_k = 10;
    constexpr std::size_t band_k = 24;
    constexpr std::size_t keys_k = rounds_k * band_k;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;

    collection_config_t narrow = test_config(8, 0);
    narrow.memory_runs_max_deltas = 6;
    narrow.memory_runs_max_bytes = 8ull * 1024ull;
    expected<collection_id_t> const first = create_named(store, "first", narrow);
    expected<collection_id_t> const second = create_named(store, "second", narrow);
    failures += expect(first.has_value());
    failures += expect(second.has_value());
    if (failures != 0) return failures;

    bool writing {true};
    std::size_t half_applied {0};
    std::size_t observations {0};
    std::thread reader {[&]() noexcept {
        while (atomic_reference<bool> {writing}.load(std::memory_order_acquire)) {
            expected<typename database_t::snapshot_t> taken = store.pin_stamp();
            if (!taken) continue;
            stored_value_size_t left = 0;
            stored_value_size_t right = 0;
            for (std::size_t key = 0; key != keys_k; ++key) {
                lookup_result_t const here = taken->find(any_collection_id_t {*first}, static_cast<stored_key_t>(key),
                                                         left);
                lookup_result_t const there = taken->find(any_collection_id_t {*second}, static_cast<stored_key_t>(key),
                                                          right);
                if ((here == lookup_result_t::written_k) != (there == lookup_result_t::written_k))
                    atomic_reference<std::size_t> {half_applied}.fetch_add(1, std::memory_order_relaxed);
            }
            atomic_reference<std::size_t> {observations}.fetch_add(1, std::memory_order_relaxed);
        }
    }};

    for (std::size_t round = 0; round != rounds_k; ++round) {
        expected<typename database_t::transaction_unique_ptr_t> transaction = store.create_transaction();
        failures += expect(transaction.has_value());
        if (!transaction) break;
        for (std::size_t step = 0; step != band_k; ++step) {
            stored_key_t const key = static_cast<stored_key_t>(round * band_k + step);
            std::array<byte_t, 8> const counter = counted(key);
            failures += expect(
                (*transaction)->insert(any_collection_id_t {*first}, key, stored_value_view_t {counter}).succeeded());
            failures += expect(
                (*transaction)->insert(any_collection_id_t {*second}, key, stored_value_view_t {counter}).succeeded());
        }
        failures += expect((*transaction)->commit().succeeded());
    }
    atomic_reference<bool> {writing}.store(false, std::memory_order_release);
    reader.join();

    failures += expect_eq(atomic_reference<std::size_t> {half_applied}.load(std::memory_order_relaxed),
                          std::size_t {0});
    failures += expect(atomic_reference<std::size_t> {observations}.load(std::memory_order_relaxed) != 0);

    // Every write of every committed transaction is in the store afterwards.
    st::vector<byte_t> into;
    failures += expect(into.resize(8) == st::status_t::success_k);
    std::size_t mismatches = 0;
    for (std::size_t key = 0; key != keys_k; ++key) {
        stored_value_size_t size = 0;
        failures += expect(
            store.select(any_collection_id_t {*first}, static_cast<stored_key_t>(key), std::span<byte_t> {into}, size)
                .succeeded());
        if (counter_of(std::span<byte_t const> {into.data(), size}) != key) ++mismatches;
        failures += expect(
            store.select(any_collection_id_t {*second}, static_cast<stored_key_t>(key), std::span<byte_t> {into}, size)
                .succeeded());
        if (counter_of(std::span<byte_t const> {into.data(), size}) != key) ++mismatches;
    }
    failures += expect_eq(mismatches, std::size_t {0});
    return failures;
}

/** A transaction that conflicts on one collection stages nothing, and leaves the other untouched. */
template <typename mount_type_>
std::size_t conflict_leaves_the_other_collection_untouched(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;

    expected<collection_id_t> const contested = create_named(store, "contested", test_config(8, 0));
    expected<collection_id_t> const quiet = create_named(store, "quiet", test_config(8, 0));
    failures += expect(contested.has_value());
    failures += expect(quiet.has_value());
    if (failures != 0) return failures;

    expected<typename database_t::transaction_unique_ptr_t> loser = store.create_transaction();
    expected<typename database_t::transaction_unique_ptr_t> winner = store.create_transaction();
    failures += expect(loser.has_value());
    failures += expect(winner.has_value());
    if (failures != 0) return failures;

    // Both transactions start before either commits, so the second to stage sees the first's write.
    std::array<byte_t, 8> const mine = counted(111);
    std::array<byte_t, 8> const theirs = counted(222);
    failures += expect(
        (*loser)->insert(any_collection_id_t {*contested}, stored_key_t {1}, stored_value_view_t {mine}).succeeded());
    failures += expect(
        (*loser)->insert(any_collection_id_t {*quiet}, stored_key_t {2}, stored_value_view_t {mine}).succeeded());

    failures += expect((*winner)
                           ->insert(any_collection_id_t {*contested}, stored_key_t {1}, stored_value_view_t {theirs})
                           .succeeded());
    failures += expect((*winner)->commit().succeeded());

    failures += expect_status((*loser)->stage(), status_code_t::write_conflict_k);
    failures += expect_status((*loser)->commit(), status_code_t::write_conflict_k);

    // The quiet collection never saw the refused transaction's write.
    failures += expect(!store.contains(any_collection_id_t {*quiet}, stored_key_t {2}));
    st::vector<byte_t> into;
    failures += expect(into.resize(8) == st::status_t::success_k);
    stored_value_size_t size = 0;
    failures += expect(
        store.select(any_collection_id_t {*contested}, stored_key_t {1}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(counter_of(std::span<byte_t const> {into.data(), size}), std::uint64_t {222});

    // Rolled back, the refused transaction is usable again and its second try lands.
    (*loser)->rollback();
    failures += expect(
        (*loser)->insert(any_collection_id_t {*quiet}, stored_key_t {2}, stored_value_view_t {mine}).succeeded());
    failures += expect((*loser)->commit().succeeded());
    failures += expect(store.contains(any_collection_id_t {*quiet}, stored_key_t {2}));
    return failures;
}

/** A rollback drops the writes, and a renewal keeps them while a read refuses one. */
template <typename mount_type_>
std::size_t rollback_and_renew_behave(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using transaction_t = typename types_t::transaction_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;

    expected<collection_id_t> const only = create_named(store, "only", test_config(8, 0));
    failures += expect(only.has_value());
    if (!only) return failures;

    expected<typename database_t::transaction_unique_ptr_t> transaction = store.create_transaction();
    failures += expect(transaction.has_value());
    if (!transaction) return failures;
    transaction_t& handler = **transaction;
    std::array<byte_t, 8> const counter = counted(7);

    // A rollback drops what was written and leaves the transaction ready for more.
    failures += expect(
        handler.insert(any_collection_id_t {*only}, stored_key_t {1}, stored_value_view_t {counter}).succeeded());
    failures += expect(handler.contains_uncommitted(any_collection_id_t {*only}, stored_key_t {1}));
    handler.rollback();
    failures += expect(!handler.contains_uncommitted(any_collection_id_t {*only}, stored_key_t {1}));
    failures += expect(handler.commit().succeeded());
    failures += expect(!store.contains(any_collection_id_t {*only}, stored_key_t {1}));

    // A renewal keeps the writes of a transaction that read nothing, and its start stamp moves.
    failures += expect(
        handler.insert(any_collection_id_t {*only}, stored_key_t {2}, stored_value_view_t {counter}).succeeded());
    transaction_stamp_t const before = handler.start_stamp();
    failures += expect(handler.renew().succeeded());
    failures += expect(handler.start_stamp() >= before);
    failures += expect(handler.contains_uncommitted(any_collection_id_t {*only}, stored_key_t {2}));
    failures += expect(handler.commit().succeeded());
    failures += expect(store.contains(any_collection_id_t {*only}, stored_key_t {2}));
    failures += expect(handler.last_commit_stamp() != invalid_transaction_stamp_k);

    // A transaction that has read something cannot move its watches, so a renewal refuses it.
    st::vector<byte_t> into;
    failures += expect(into.resize(8) == st::status_t::success_k);
    stored_value_size_t size = 0;
    failures += expect(
        handler.select(any_collection_id_t {*only}, stored_key_t {2}, stored_value_span_t {into}, size).succeeded());
    failures += expect_status(handler.renew(), status_code_t::invalid_argument_k);

    // A staged transaction takes no more writes until it commits or rolls back.
    failures += expect(handler.stage().succeeded());
    failures += expect(handler.insert(any_collection_id_t {*only}, stored_key_t {3}, stored_value_view_t {counter}) ==
                       status_code_t::invalid_argument_k);
    failures += expect_status(handler.stage(), status_code_t::invalid_argument_k);
    handler.rollback();
    failures += expect(
        handler.insert(any_collection_id_t {*only}, stored_key_t {3}, stored_value_view_t {counter}).succeeded());
    failures += expect(handler.commit().succeeded());
    failures += expect(store.contains(any_collection_id_t {*only}, stored_key_t {3}));
    return failures;
}

/** A snapshot keeps answering the database of its moment while later writes land beside it. */
template <typename mount_type_>
std::size_t snapshot_holds_its_moment(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;

    expected<collection_id_t> const left = create_named(store, "left", test_config(8, 0));
    expected<collection_id_t> const right = create_named(store, "right", test_config(0, value_max_size_k));
    failures += expect(left.has_value());
    failures += expect(right.has_value());
    if (failures != 0) return failures;

    std::array<byte_t, 8> const older = counted(1);
    std::array<byte_t, 8> const newer = counted(2);
    failures += expect(
        store.insert(any_collection_id_t {*left}, stored_key_t {1}, stored_value_view_t {older}).succeeded());
    failures += expect(
        store.insert(any_collection_id_t {*right}, stored_key_t {1}, stored_value_view_t {older}).succeeded());

    expected<typename database_t::snapshot_t> taken = store.pin_stamp();
    failures += expect(taken.has_value());
    if (!taken) return failures;
    failures += expect_eq(store.population_count(), std::uint64_t {2});

    // Later commits and a flush land beside the snapshot, which publishes over nothing it waits for.
    failures += expect(
        store.insert(any_collection_id_t {*left}, stored_key_t {1}, stored_value_view_t {newer}).succeeded());
    failures += expect(
        store.insert(any_collection_id_t {*left}, stored_key_t {2}, stored_value_view_t {newer}).succeeded());
    failures += expect(store.remove(any_collection_id_t {*right}, stored_key_t {1}).succeeded());

    st::vector<byte_t> into;
    failures += expect(into.resize(8) == st::status_t::success_k);
    for (std::size_t round = 0; round != 2; ++round) {
        stored_value_size_t size = 0;
        failures += expect(
            taken->select(any_collection_id_t {*left}, stored_key_t {1}, std::span<byte_t> {into}, size).succeeded());
        failures += expect_eq(counter_of(std::span<byte_t const> {into.data(), size}), std::uint64_t {1});
        failures += expect(taken->find(any_collection_id_t {*left}, stored_key_t {2}, size) ==
                           lookup_result_t::missing_k);
        failures += expect(taken->find(any_collection_id_t {*right}, stored_key_t {1}, size) ==
                           lookup_result_t::written_k);
        // The second round reads what the flush wrote, which kept every version the snapshot pins.
        if (round == 0) failures += expect(store.flush().succeeded());
    }
    failures += expect(taken->contains(any_collection_id_t {owned_name("right")}));
    failures += expect(!taken->contains(any_collection_id_t {owned_name("absent")}));

    // The store itself has moved on, which is what makes the snapshot's answers its own.
    stored_value_size_t size = 0;
    failures += expect(
        store.select(any_collection_id_t {*left}, stored_key_t {1}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(counter_of(std::span<byte_t const> {into.data(), size}), std::uint64_t {2});
    failures += expect(!store.contains(any_collection_id_t {*right}, stored_key_t {1}));

    taken = expected<typename database_t::snapshot_t> {status_t {status_code_t::uninitialized_k, "Let go"}};
    failures += expect(store.flush().succeeded());
    failures += expect(
        store.select(any_collection_id_t {*left}, stored_key_t {1}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(counter_of(std::span<byte_t const> {into.data(), size}), std::uint64_t {2});
    failures += expect(!store.contains(any_collection_id_t {*right}, stored_key_t {1}));
    return failures;
}

/** An unordered scan walks every run on its own: a key two runs carry comes out twice, a removal comes out as one, a
 *  commit leaves the counted runs where they were, and a flush rearranges them, which the next run's walk reports. */
template <typename mount_type_>
std::size_t runs_scan_one_at_a_time(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;
    expected<collection_id_t> const created = create_named(store, "runs", test_config(0, value_max_size_k));
    failures += expect(created.has_value());
    if (!created) return failures;
    any_collection_id_t const runs {*created};

    std::array<byte_t, 8> const value = counted(7);
    for (std::uint64_t key = 1; key <= 4; ++key)
        failures += expect(store.insert(runs, stored_key_t {key}, stored_value_view_t {value}).succeeded());
    failures += expect(store.flush().succeeded());
    // Two memory runs above the flushed one, fewer than a merge of memory runs by tier takes.
    failures += expect(store.insert(runs, stored_key_t {2}, stored_value_view_t {value}).succeeded());
    failures += expect(store.remove(runs, stored_key_t {3}).succeeded());

    expected<counted_runs_t> const counted_runs = store.runs_count(runs);
    failures += expect(counted_runs.has_value());
    if (!counted_runs) return failures;
    failures += expect(counted_runs->count >= 3);
    std::array<std::size_t, 5> written {};
    std::array<std::size_t, 5> removed {};
    auto const tally = [&](stored_key_t key, lookup_result_t state, stored_value_size_t size) noexcept {
        std::size_t const index = static_cast<std::size_t>(key) < written.size() ? static_cast<std::size_t>(key) : 0;
        if (state == lookup_result_t::written_k && size == value.size()) ++written[index];
        if (state == lookup_result_t::removed_k) ++removed[index];
        return true;
    };
    for (std::uint64_t run = 0; run != counted_runs->count; ++run)
        failures += expect(store.scan_run(runs, *counted_runs, run, stored_key_t {1}, tally).succeeded());
    failures += expect_eq(written[1], std::size_t {1});
    failures += expect_eq(written[2], std::size_t {2});
    failures += expect_eq(written[3], std::size_t {1});
    failures += expect_eq(written[4], std::size_t {1});
    failures += expect_eq(removed[3], std::size_t {1});
    failures += expect_eq(written[0], std::size_t {0});

    // A commit only puts a newer run ahead, while a flush rearranges the runs the count named.
    failures += expect(store.insert(runs, stored_key_t {5}, stored_value_view_t {value}).succeeded());
    failures += expect(store.scan_run(runs, *counted_runs, 0, stored_key_t {1}, tally).succeeded());
    failures += expect(store.flush().succeeded());
    failures += expect(store.scan_run(runs, *counted_runs, 0, stored_key_t {1}, tally) == status_code_t::contended_k);
    failures += expect(store.scan_run(runs, *counted_runs, counted_runs->count, stored_key_t {1}, tally) ==
                       status_code_t::invalid_argument_k);
    return failures;
}

/**
 *  @brief Dropping a collection waits for no snapshot, since a snapshot is a pinned stamp that holds no level set,
 *      and the snapshot finds the collection gone afterwards.
 *
 *  The drop runs on a second thread while the snapshot lives, and must finish before the snapshot goes.
 */
template <typename mount_type_>
std::size_t drop_proceeds_beside_the_snapshot(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;

    expected<collection_id_t> const doomed = create_named(store, "doomed", test_config(8, 0));
    failures += expect(doomed.has_value());
    if (failures != 0) return failures;
    std::array<byte_t, 8> const value = counted(1);
    failures += expect(
        store.insert(any_collection_id_t {*doomed}, stored_key_t {1}, stored_value_view_t {value}).succeeded());

    bool dropped {false};
    bool succeeded {false};
    std::thread dropper;
    {
        expected<typename database_t::snapshot_t> taken = store.pin_stamp();
        failures += expect(taken.has_value());
        if (!taken) return failures;
        dropper = std::thread {[&]() noexcept {
            atomic_reference<bool> {succeeded}.store(store.drop_collection(any_collection_id_t {*doomed}).succeeded(),
                                                     std::memory_order_relaxed);
            atomic_reference<bool> {dropped}.store(true, std::memory_order_release);
        }};
        dropper.join();
        stored_value_size_t size = 0;
        failures += expect(taken->find(any_collection_id_t {*doomed}, stored_key_t {1}, size) ==
                           lookup_result_t::missing_k);
    }
    failures += expect(atomic_reference<bool> {dropped}.load(std::memory_order_acquire));
    failures += expect(atomic_reference<bool> {succeeded}.load(std::memory_order_relaxed));
    failures += expect(!store.has_collection(any_collection_id_t {*doomed}));
    return failures;
}

/**
 *  @brief Several threads committing over the same two collections leave a state one serial order explains.
 *
 *  Every attempt reads both counters, writes both back one higher and commits; a conflict is retried.
 *  A lost update would leave a counter below the commits that landed, and the two counters would part.
 */
template <typename mount_type_>
std::size_t threads_commit_serializably(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using transaction_t = typename types_t::transaction_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t threads_k = 4;
    constexpr std::size_t increments_k = 25;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;

    expected<collection_id_t> const left = create_named(store, "left", test_config(8, 0));
    expected<collection_id_t> const right = create_named(store, "right", test_config(8, 0));
    failures += expect(left.has_value());
    failures += expect(right.has_value());
    if (failures != 0) return failures;

    std::array<byte_t, 8> const zero = counted(0);
    failures += expect(
        store.insert(any_collection_id_t {*left}, stored_key_t {0}, stored_value_view_t {zero}).succeeded());
    failures += expect(
        store.insert(any_collection_id_t {*right}, stored_key_t {0}, stored_value_view_t {zero}).succeeded());

    std::size_t landed {0};
    std::size_t refused {0};
    std::size_t thread_failures {0};
    std::array<std::thread, threads_k> workers;
    for (std::size_t worker = 0; worker != threads_k; ++worker)
        workers[worker] = std::thread {[&]() noexcept {
            expected<typename database_t::transaction_unique_ptr_t> transaction = store.create_transaction();
            if (!transaction) {
                atomic_reference<std::size_t> {thread_failures}.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            transaction_t& handler = **transaction;
            std::array<byte_t, 8> buffer {};

            for (std::size_t step = 0; step != increments_k; ++step) {
                bool committed = false;
                while (!committed) {
                    stored_value_size_t size = 0;
                    status_t read = handler.template select<watch_schedule_t::on_commit_k>(
                        any_collection_id_t {*left}, stored_key_t {0}, stored_value_span_t {buffer}, size);
                    if (read.failed()) {
                        atomic_reference<std::size_t> {thread_failures}.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    std::uint64_t const here = counter_of(std::span<byte_t const> {buffer.data(), size});

                    read = handler.template select<watch_schedule_t::on_commit_k>(
                        any_collection_id_t {*right}, stored_key_t {0}, stored_value_span_t {buffer}, size);
                    if (read.failed()) {
                        atomic_reference<std::size_t> {thread_failures}.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    std::uint64_t const there = counter_of(std::span<byte_t const> {buffer.data(), size});

                    std::array<byte_t, 8> const raised_here = counted(here + 1);
                    std::array<byte_t, 8> const raised_there = counted(there + 1);
                    status_t written = handler.insert(any_collection_id_t {*left}, stored_key_t {0},
                                                      stored_value_view_t {raised_here});
                    if (written.succeeded())
                        written = handler.insert(any_collection_id_t {*right}, stored_key_t {0},
                                                 stored_value_view_t {raised_there});
                    if (written.failed()) {
                        atomic_reference<std::size_t> {thread_failures}.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }

                    status_t const commit = handler.commit();
                    if (commit.succeeded()) {
                        atomic_reference<std::size_t> {landed}.fetch_add(1, std::memory_order_relaxed);
                        committed = true;
                        continue;
                    }
                    if (!(commit == status_code_t::write_conflict_k)) {
                        atomic_reference<std::size_t> {thread_failures}.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    atomic_reference<std::size_t> {refused}.fetch_add(1, std::memory_order_relaxed);
                    handler.rollback();
                }
            }
        }};
    for (std::size_t worker = 0; worker != threads_k; ++worker) workers[worker].join();

    failures += expect_eq(atomic_reference<std::size_t> {thread_failures}.load(std::memory_order_relaxed),
                          std::size_t {0});
    failures += expect_eq(atomic_reference<std::size_t> {landed}.load(std::memory_order_relaxed),
                          threads_k * increments_k);

    st::vector<byte_t> into;
    failures += expect(into.resize(8) == st::status_t::success_k);
    stored_value_size_t size = 0;
    failures += expect(
        store.select(any_collection_id_t {*left}, stored_key_t {0}, std::span<byte_t> {into}, size).succeeded());
    std::uint64_t const here = counter_of(std::span<byte_t const> {into.data(), size});
    failures += expect(
        store.select(any_collection_id_t {*right}, stored_key_t {0}, std::span<byte_t> {into}, size).succeeded());
    std::uint64_t const there = counter_of(std::span<byte_t const> {into.data(), size});

    // Every commit that landed raised both counters exactly once, so no update was lost.
    failures += expect_eq(here, static_cast<std::uint64_t>(threads_k * increments_k));
    failures += expect_eq(there, here);
    return failures;
}

/** Bytes the value a committer writes in round @p round takes, which differs from round to round. */
[[nodiscard]] std::size_t round_bytes_of(std::uint64_t round) noexcept {
    return static_cast<std::size_t>(16 + round % 61);
}

/**
 *  @brief Readers walking two collections while two committers write one each, and a third thread flushes and
 *      compacts both, never see a version older than one returned before the read began, nor a torn value.
 *
 *  Committer @c c writes round @c r of its collection under key @c r % keys, its value the round followed by bytes
 *  painted from it, and publishes the round it started and the round that returned. A reader takes the returned
 *  round before a read and the started round after it, so the round it reads is at least the last returned one of
 *  that key and at most the started one, whichever runs, levels or cache answered.
 */
template <typename mount_type_>
std::size_t readers_beside_committers_and_flushes(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t committers_k = 2;
    constexpr std::size_t readers_k = 4;
    constexpr std::uint64_t rounds_k = 1500;
    constexpr std::uint64_t keys_k = 64;
    constexpr std::uint64_t no_round_k = std::numeric_limits<std::uint64_t>::max();

    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;

    // Memory runs of a few dozen deltas, so the committers merge, flush and compact beside the flusher.
    collection_config_t config = test_config(0, value_max_size_k);
    config.memory_runs_max_deltas = 32;
    std::array<collection_id_t, committers_k> named {};
    for (std::size_t committer = 0; committer != committers_k; ++committer) {
        expected<collection_id_t> const created = create_named(
            store, committer == 0 ? string_view_t {"left"} : string_view_t {"right"}, config);
        failures += expect(created.has_value());
        if (!created) return failures;
        named[committer] = *created;
    }

    std::array<std::uint64_t, committers_k> started {};
    std::array<std::uint64_t, committers_k> returned {};
    for (std::size_t committer = 0; committer != committers_k; ++committer) {
        atomic_reference<std::uint64_t> {started[committer]}.store(no_round_k, std::memory_order_relaxed);
        atomic_reference<std::uint64_t> {returned[committer]}.store(no_round_k, std::memory_order_relaxed);
    }
    std::size_t committing {committers_k};
    std::size_t wrong {0};
    std::size_t reads {0};

    std::array<std::thread, committers_k> committers;
    for (std::size_t committer = 0; committer != committers_k; ++committer)
        committers[committer] = std::thread {[&, committer]() noexcept {
            std::array<byte_t, 128> value {};
            for (std::uint64_t round = 0; round != rounds_k; ++round) {
                std::size_t const bytes = round_bytes_of(round);
                std::memcpy(value.data(), &round, sizeof(round));
                for (std::size_t position = sizeof(round); position != bytes; ++position)
                    value[position] = painted(round, position);
                atomic_reference<std::uint64_t> {started[committer]}.store(round, std::memory_order_release);
                if (store
                        .insert(any_collection_id_t {named[committer]}, stored_key_t {round % keys_k},
                                stored_value_view_t {value.data(), bytes})
                        .failed())
                    atomic_reference<std::size_t> {wrong}.fetch_add(1, std::memory_order_relaxed);
                atomic_reference<std::uint64_t> {returned[committer]}.store(round, std::memory_order_release);
            }
            atomic_reference<std::size_t> {committing}.fetch_sub(1, std::memory_order_release);
        }};

    std::thread flusher {[&]() noexcept {
        while (atomic_reference<std::size_t> {committing}.load(std::memory_order_acquire) != 0) {
            if (store.flush().failed()) atomic_reference<std::size_t> {wrong}.fetch_add(1, std::memory_order_relaxed);
            if (store.needs_compaction() && store.compact().failed())
                atomic_reference<std::size_t> {wrong}.fetch_add(1, std::memory_order_relaxed);
        }
    }};

    std::array<std::thread, readers_k> readers;
    for (std::size_t reader = 0; reader != readers_k; ++reader)
        readers[reader] = std::thread {[&, reader]() noexcept {
            std::array<byte_t, 128> into {};
            for (std::uint64_t step = reader;
                 atomic_reference<std::size_t> {committing}.load(std::memory_order_acquire) != 0; ++step) {
                std::size_t const committer = step % committers_k;
                std::uint64_t const key = (step * 7) % keys_k;
                std::uint64_t const before = atomic_reference<std::uint64_t> {returned[committer]}.load(
                    std::memory_order_acquire);
                stored_value_size_t size = 0;
                status_t const read = store.select(any_collection_id_t {named[committer]}, stored_key_t {key},
                                                   std::span<byte_t> {into}, size);
                std::uint64_t const after = atomic_reference<std::uint64_t> {started[committer]}.load(
                    std::memory_order_acquire);
                atomic_reference<std::size_t> {reads}.fetch_add(1, std::memory_order_relaxed);

                // The last round that wrote this key and returned before the read began, if any did.
                bool const owed = before != no_round_k && before >= key;
                std::uint64_t const least = owed ? before - (before - key) % keys_k : no_round_k;
                if (read.failed()) {
                    if (owed || !(read == status_code_t::not_found_k))
                        atomic_reference<std::size_t> {wrong}.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                std::uint64_t round = 0;
                if (size >= sizeof(round)) std::memcpy(&round, into.data(), sizeof(round));
                bool torn = size != round_bytes_of(round) || round % keys_k != key || round > after;
                if (owed && round < least) torn = true;
                for (std::size_t position = sizeof(round); !torn && position != size; ++position)
                    torn = into[position] != painted(round, position);
                if (torn) atomic_reference<std::size_t> {wrong}.fetch_add(1, std::memory_order_relaxed);
            }
        }};

    for (std::thread& committer : committers) committer.join();
    flusher.join();
    for (std::thread& reader : readers) reader.join();

    failures += expect_eq(atomic_reference<std::size_t> {wrong}.load(std::memory_order_relaxed), std::size_t {0});
    failures += expect(atomic_reference<std::size_t> {reads}.load(std::memory_order_relaxed) != 0);

    // Once every commit returned, a point read probes at most (F − 1)(T + 1) memory runs, T = ⌈log_F commits⌉.
    std::uint64_t const fan_in = config.lsm_tree.level_enlarge_factor;
    for (std::size_t committer = 0; committer != committers_k; ++committer) {
        memory_shape_t const shape = store.memory_shape(any_collection_id_t {named[committer]});
        std::uint64_t tiers = 0;
        for (std::uint64_t covered = 1; covered < shape.commits; covered *= fan_in) ++tiers;
        failures += expect(shape.runs <= (fan_in - 1) * (tiers + 1));
    }

    // Once everything returned, every key holds its last round, wherever the flushes left it.
    std::array<byte_t, 128> into {};
    for (std::size_t committer = 0; committer != committers_k; ++committer)
        for (std::uint64_t key = 0; key != keys_k; ++key) {
            stored_value_size_t size = 0;
            failures += expect(
                store.select(any_collection_id_t {named[committer]}, stored_key_t {key}, std::span<byte_t> {into}, size)
                    .succeeded());
            std::uint64_t round = 0;
            std::memcpy(&round, into.data(), sizeof(round));
            failures += expect_eq(round, (rounds_k - 1) - (rounds_k - 1 - key) % keys_k);
        }
    return failures;
}

/** Whether the looking-up threads of the contention case keep asking or wind down. */
enum class reader_phase_t : std::uint8_t {

    /** The readers keep looking collections up. */
    racing_k,

    /** The readers leave their loop and join. */
    retiring_k,
};

/**
 *  @brief Catalog lookups answer while one thread holds the write path, and while names come and go.
 *
 *  A lookup that took the database's one claim would queue behind the write path rather than answer,
 *  so the first phase counts nothing at all the moment that claim comes back. The second publishes a
 *  catalog under live readers, which is what retiring a view has to survive.
 */
template <typename mount_type_>
std::size_t lookups_outrun_the_write_path(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t readers_k = 8;
    constexpr std::size_t rounds_k = 5'000;
    constexpr std::size_t churns_k = 64;
    constexpr std::chrono::seconds patience_k {5};

    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;

    expected<collection_id_t> const left = create_named(store, "left", test_config(8, 0));
    failures += expect(left.has_value());
    if (!left) return failures;

    any_collection_id_t const by_name {owned_name("left")};
    any_collection_id_t const churned {owned_name("churn")};
    reader_phase_t phase {reader_phase_t::racing_k};
    std::size_t answered {0};
    std::size_t mismatched {0};

    // The leader word is taken before the first reader starts, so a lookup that wants it counts nothing at all.
    store.ledger().lead();
    std::array<std::thread, readers_k> readers;
    for (std::size_t reader = 0; reader != readers_k; ++reader)
        readers[reader] = std::thread {[&]() noexcept {
            while (atomic_reference<reader_phase_t> {phase}.load(std::memory_order_relaxed) ==
                   reader_phase_t::racing_k) {
                if (store.collection_id(by_name) != *left)
                    atomic_reference<std::size_t> {mismatched}.fetch_add(1, std::memory_order_relaxed);
                collection_config_t shape;
                if (store.collection_config(*left, shape).failed())
                    atomic_reference<std::size_t> {mismatched}.fetch_add(1, std::memory_order_relaxed);
                [[maybe_unused]] bool const seen = store.has_collection(churned);
                atomic_reference<std::size_t> {answered}.fetch_add(1, std::memory_order_relaxed);
            }
        }};

    std::chrono::steady_clock::time_point const started = std::chrono::steady_clock::now();
    std::size_t progressed = 0;
    while (std::chrono::steady_clock::now() - started < patience_k) {
        progressed = atomic_reference<std::size_t> {answered}.load(std::memory_order_relaxed);
        if (progressed >= readers_k * rounds_k) break;
        fu::preferred_yield_t {}(answered, progressed, std::size_t {0});
    }
    store.ledger().stop_leading();
    failures += expect(progressed >= readers_k * rounds_k);

    // Every create and every drop republishes the catalog with the readers already inside one.
    for (std::size_t round = 0; round != churns_k; ++round) {
        expected<collection_id_t> const created = create_named(store, "churn", test_config(8, 0));
        failures += expect(created.has_value());
        if (!created) break;
        failures += expect(store.drop_collection(any_collection_id_t {*created}).succeeded());
    }

    atomic_reference<reader_phase_t> {phase}.store(reader_phase_t::retiring_k, std::memory_order_relaxed);
    for (std::size_t reader = 0; reader != readers_k; ++reader) readers[reader].join();

    failures += expect_eq(atomic_reference<std::size_t> {mismatched}.load(std::memory_order_relaxed), std::size_t {0});
    failures += expect(store.collection_id(by_name) == *left);
    failures += expect(!store.has_collection(churned));
    return failures;
}

#pragma endregion Transaction Cases

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
        report(totals, mount, "named_collections", named_collections_round_trip);
        report(totals, mount, "catalog_survives_reopen", catalog_survives_reopen);
        report(totals, mount, "strings_survive_reopen", strings_survive_reopen);
        report(totals, mount, "dropped_collection_is_gone", dropped_collection_is_gone);
        report(totals, mount, "crash_before_schema", crash_between_seal_and_schema);
        report(totals, mount, "crash_keeps_a_prefix", crash_keeps_a_prefix_of_commits);
        report(totals, mount, "user_data", user_data_round_trips);
        report(totals, mount, "commit_is_all_or_nothing", commit_is_all_or_nothing);
        report(totals, mount, "conflict_spares_the_other", conflict_leaves_the_other_collection_untouched);
        report(totals, mount, "rollback_and_renew", rollback_and_renew_behave);
        report(totals, mount, "snapshot_holds_its_moment", snapshot_holds_its_moment);
        report(totals, mount, "runs_scan_one_at_a_time", runs_scan_one_at_a_time);
        report(totals, mount, "drop_proceeds_beside_the_snapshot", drop_proceeds_beside_the_snapshot);
        report(totals, mount, "threads_commit_serializably", threads_commit_serializably);
        report(totals, mount, "readers_beside_committers_and_flushes", readers_beside_committers_and_flushes);
        report(totals, mount, "lookups_outrun_the_write_path", lookups_outrun_the_write_path);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, database_config_k, 1, "database", cases);

    return suite_exit_code(totals);
}
