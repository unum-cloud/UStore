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
#include <cstdio>  // `std::fopen`, `std::fclose`
#include <cstring> // `std::memcmp`, `std::memcpy`

#include <array>       // `std::array`
#include <chrono>      // `std::chrono::steady_clock`
#include <filesystem> // `std::filesystem::directory_iterator`, `std::filesystem::remove`, `std::filesystem::resize_file`
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

#include "ustore/blobs/nvme_database.hpp"     // `basic_database`
#include "ustore/strings/nvme_store.hpp"      // `strings::nvme_store`, `strings::nvme_transaction`
#include "ustore/utilities/nvme_manifest.hpp" // `manifest_t`

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

/** The database and its transactions every case drives over the volume of @p fixture_type_. */
template <typename fixture_type_>
struct database_types {
    using fixture_t = fixture_type_;
    using guard_t = typename fixture_t::guard_t;
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

#pragma endregion Shape

#pragma region Fixtures

/** Mounts the volume of @p stack again, as a process starting after a close or a crash does. */
template <typename stack_type_>
status_t remount_volume(stack_type_& stack) noexcept {
    stack.volume = decltype(stack.volume) {};
    auto [guard, acquire_status] = stack.fixture->channels.acquire();
    if (acquire_status.failed()) return acquire_status;
    auto [volume, mount_status] = stack.fixture->mount_volume(guard.channel());
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
    if (status_t const mounted = store->mount(stack.volume, stack.fixture->channels, test_database_config());
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
case_result_t catalog_survives_reopen(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
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

    // A merge publishes its new set before it drops what it consumed, so the manifest names every
    // run file left.
    failures += expect_eq(run_count_of(*(*stack)->fixture), run_files_of(*(*stack)->fixture));
    for (std::uint64_t key = 0; key < keys_k; key += 3) failures += write_painted(*(*stack)->store, named, key, 2);
    failures += expect((*stack)->store->flush().succeeded());
    if (failures != 0) return failures;

    // The runs and their bytes are what a reopen must take back, level for level.
    std::size_t const runs_before = run_count_of(*(*stack)->fixture);
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
    failures += expect_eq(run_count_of(*(*stack)->fixture), runs_before);
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
        failures += expect(remount_volume(**stack).succeeded());
        failures += expect_status(narrow.mount((*stack)->volume, (*stack)->fixture->channels, test_database_config()),
                                  status_code_t::malformed_k);
    }

    // A database listing its stores in another order mounts every collection in the store its tag names.
    using reordered_t = typename types_t::template basic_database_of<
        typename types_t::template blobs_store_of<value_layout_t::variable_width_k>,
        typename types_t::template blobs_store_of<value_layout_t::fixed_width_k>,
        typename types_t::template blobs_store_of<value_layout_t::inline_12_bytes_k>>;
    {
        reordered_t reordered {(*stack)->fixture->resources};
        failures += expect(remount_volume(**stack).succeeded());
        failures += expect(
            reordered.mount((*stack)->volume, (*stack)->fixture->channels, test_database_config()).succeeded());
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

/** A range a transaction removes hides the keys its runs hold through a flush, a reopen and a
 *  merge, a write inside it before the removal drops with it, and one after the removal lands. */
template <typename mount_type_>
case_result_t range_removal_survives_reopen(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    std::size_t failures = 0;
    constexpr std::uint64_t keys_k = 200;
    constexpr stored_key_t start_k = 50;
    constexpr stored_key_t end_k = 160;
    constexpr stored_key_t dropped_k = 150;
    constexpr stored_key_t kept_k = 120;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_config_t config = test_config(8, 0);
    config.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;
    expected<collection_id_t> const ranged = create_named(*(*stack)->store, "ranged", config);
    failures += expect(ranged.has_value());
    if (!ranged) return failures;
    any_collection_id_t const id {*ranged};
    for (std::uint64_t round = 0; round != 2; ++round) {
        for (std::uint64_t key = 0; key != keys_k; ++key) {
            std::array<byte_t, 8> const counter = counted(key + round);
            failures += expect(
                (*stack)->store->insert(id, static_cast<stored_key_t>(key), stored_value_view_t {counter}).succeeded());
        }
        failures += expect((*stack)->store->flush().succeeded());
    }
    failures += expect((*stack)->store->compact().succeeded());

    expected<typename database_t::transaction_unique_ptr_t> transaction = (*stack)->store->create_transaction();
    failures += expect(transaction.has_value());
    if (!transaction) return failures;
    std::array<byte_t, 8> const before = counted(7000);
    std::array<byte_t, 8> const after = counted(9000);
    failures += expect((*transaction)->insert(id, dropped_k, stored_value_view_t {before}).succeeded());
    failures += expect((*transaction)->remove_range(id, start_k, end_k).succeeded());
    failures += expect((*transaction)->insert(id, kept_k, stored_value_view_t {after}).succeeded());
    failures += expect((*transaction)->lookup_uncommitted(*ranged, dropped_k) == lookup_result_t::removed_k);
    failures += expect((*transaction)->lookup_uncommitted(*ranged, start_k) == lookup_result_t::removed_k);
    failures += expect((*transaction)->lookup_uncommitted(*ranged, end_k) == lookup_result_t::missing_k);
    failures += expect((*transaction)->commit().succeeded());

    auto const check = [&](database_t& store) {
        std::size_t checked = 0;
        std::array<byte_t, 8> into {};
        for (std::uint64_t key = 0; key != keys_k; ++key) {
            stored_value_size_t size = 0;
            status_t const read = store.select(id, static_cast<stored_key_t>(key), std::span<byte_t> {into}, size);
            if (key == kept_k) {
                checked += expect(read.succeeded());
                checked += expect_eq(counter_of(std::span<byte_t const> {into.data(), size}), std::uint64_t {9000});
            }
            else if (key >= start_k && key < end_k) checked += expect(read == status_code_t::not_found_k);
            else {
                checked += expect(read.succeeded());
                checked += expect_eq(counter_of(std::span<byte_t const> {into.data(), size}), key + 1);
            }
        }
        return checked;
    };
    failures += check(*(*stack)->store);
    failures += expect((*stack)->store->flush().succeeded());
    failures += check(*(*stack)->store);
    (*transaction).reset();
    failures += expect((*stack)->store->unmount().succeeded());
    (*stack)->store.reset();

    expected<std::unique_ptr<database_t>> reopened = open_database(**stack);
    failures += expect(reopened.has_value());
    if (!reopened) return failures;
    failures += check(**reopened);
    failures += expect((*reopened)->flush().succeeded());
    failures += expect((*reopened)->compact().succeeded());
    failures += check(**reopened);
    return failures;
}

/** A strings collection, as the engine's names system collection is one, survives a reopen: strings sharing their
 *  15-byte heads map onto the same 16-byte keys, answered from the 32-byte cells of a flushed run, whose header records
 *  the width of its cells and holds no values section. */
template <typename mount_type_>
case_result_t strings_survive_reopen(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    using strings_store_t = typename types_t::strings_store_t;
    using key_slot_t = typename strings_store_t::key_slot_t;
    constexpr std::size_t strings_k = 64;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
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

/** A range of strings a transaction removes, and one reaching past the last string, hide the
 *  strings flushed below them through a flush, a merge and a reopen; a write before the removal
 *  drops with it and one after it lands. */
template <typename mount_type_>
case_result_t strings_range_removal_survives_reopen(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    using strings_store_t = typename types_t::strings_store_t;
    using key_slot_t = typename strings_store_t::key_slot_t;
    using value_t = std::array<byte_t, key_slot_t::size>;
    constexpr std::size_t strings_k = 100;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;

    collection_id_t words;
    std::memcpy(words.uuid.bytes, uuid_generator().bytes, sizeof(words.uuid.bytes));
    collection_config_t config = test_config(sizeof(ustore_uuid_t), 0);
    config.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;
    failures += expect(
        (*stack)
            ->store->create_collection(words, "words", static_cast<std::uint16_t>(value_layout_t::uuid_k), config)
            .succeeded());
    if (failures != 0) return failures;
    auto const text_of = [](std::size_t index) { return fmt::format("item-{:03}", index); };
    auto const value_of = [](std::size_t seed) {
        value_t value {};
        for (std::size_t position = 0; position != value.size(); ++position) value[position] = painted(seed, position);
        return value;
    };
    auto const commit = [&](database_t& store, auto&& writes) {
        strings::nvme_transaction<database_t, strings_store_t> written {store};
        auto [held, begin_status] = store.create_transaction(test_database_config().default_transaction);
        std::size_t committed = expect(begin_status.succeeded());
        if (committed != 0) return committed;
        written.join(*held);
        committed += writes(written);
        committed += expect(written.commit(*held).succeeded());
        return committed;
    };
    for (std::size_t round = 0; round != 2; ++round) {
        failures += commit(*(*stack)->store, [&](auto& written) {
            std::size_t staged = 0;
            for (std::size_t index = 0; index != strings_k; ++index) {
                std::string const text = text_of(index);
                value_t const value = value_of(index);
                staged += expect(
                    written.write(words, text, delta_view_t {delta_kind_t::write_k, value}, invalid_transaction_stamp_k)
                        .succeeded());
            }
            return staged;
        });
        failures += expect((*stack)->store->flush().succeeded());
    }
    failures += expect((*stack)->store->compact().succeeded());

    std::string const start = text_of(40), end = text_of(60), tail = text_of(90);
    std::string const dropped = text_of(50), kept = text_of(45);
    value_t const before = value_of(7000), after = value_of(9000);
    failures += commit(*(*stack)->store, [&](auto& written) {
        std::size_t staged = 0;
        staged += expect(
            written.write(words, dropped, delta_view_t {delta_kind_t::write_k, before}, invalid_transaction_stamp_k)
                .succeeded());
        staged += expect(written.remove_range(words, start, end).succeeded());
        staged += expect(written.remove_range(words, tail, {}).succeeded());
        staged += expect(
            written.write(words, kept, delta_view_t {delta_kind_t::write_k, after}, invalid_transaction_stamp_k)
                .succeeded());
        delta_view_t own;
        staged += expect(written.uncommitted(words, dropped, own) && own.kind == delta_kind_t::remove_k);
        staged += expect(written.uncommitted(words, text_of(99), own) && own.kind == delta_kind_t::remove_k);
        staged += expect(!written.uncommitted(words, text_of(60), own));
        return staged;
    });

    auto const check = [&](database_t& store) {
        std::size_t checked = 0;
        checked += expect(
            store
                .visit_store(
                    any_collection_id_t {words},
                    [&]<typename store_type_>(store_type_& held) noexcept -> status_t {
                        if constexpr (!std::is_same_v<store_type_, strings_store_t>)
                            return {status_code_t::invalid_argument_k, "Another store"};
                        else {
                            for (std::size_t index = 0; index != strings_k; ++index) {
                                value_t value {};
                                lookup_result_t const found = held.find(text_of(index), value);
                                bool const hidden = (index >= 40 && index < 60) || index >= 90;
                                if (index == 45) checked += expect(value == after);
                                else if (hidden) checked += expect(found != lookup_result_t::written_k);
                                else checked += expect(value == value_of(index));
                            }
                            // A walk down starts at the last string spared.
                            strings::strings_page_t page;
                            checked += expect(
                                held.read_page_down({}, {}, head_stamp_k, outside_executor_k, page).succeeded());
                            std::string_view previous;
                            std::size_t written = 0;
                            for (std::size_t index = 0; index != page.size(); ++index) {
                                std::string_view const text = page.text(index);
                                if (index != 0) checked += expect(text < previous);
                                previous = text;
                                if (page.listed[index].state != lookup_result_t::written_k) continue;
                                if (written++ == 0) checked += expect(text == text_of(89));
                                checked += expect(!(text >= start && text < end) || text == kept);
                            }
                            checked += expect(written != 0);
                            return {};
                        }
                    })
                .succeeded());
        return checked;
    };
    failures += check(*(*stack)->store);
    failures += expect((*stack)->store->flush().succeeded());
    failures += check(*(*stack)->store);
    failures += expect((*stack)->store->unmount().succeeded());
    (*stack)->store.reset();

    expected<std::unique_ptr<database_t>> reopened = open_database(**stack);
    failures += expect(reopened.has_value());
    if (!reopened) return failures;
    failures += check(**reopened);
    failures += commit(**reopened, [&](auto& written) {
        return expect(
            written.write(words, kept, delta_view_t {delta_kind_t::write_k, after}, invalid_transaction_stamp_k)
                .succeeded());
    });
    failures += expect((*reopened)->flush().succeeded());
    failures += expect((*reopened)->compact().succeeded());
    failures += check(**reopened);
    return failures;
}

/** A dropped collection is gone after a reopen, and the bytes its runs held are free again. */
template <typename mount_type_>
case_result_t dropped_collection_is_gone(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    auto const& fixture = *(*stack)->fixture;

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
    failures += expect(run_bytes_of(fixture) != std::uint64_t {0});

    // What a drop must reclaim are runs this database took back from its schema rather than wrote itself.
    failures += expect((*stack)->store->unmount().succeeded());
    (*stack)->store.reset();
    expected<std::unique_ptr<database_t>> reopened = open_database(**stack);
    failures += expect(reopened.has_value());
    if (!reopened) return failures;

    failures += expect(run_bytes_of(fixture) != std::uint64_t {0});
    failures += expect((*reopened)->drop_collection(any_collection_id_t {*doomed}).succeeded());
    failures += expect(!(*reopened)->has_collection(any_collection_id_t {owned_name("doomed")}));
    failures += expect_eq(run_bytes_of(fixture), std::uint64_t {0});
    failures += expect_eq(run_files_of(fixture), std::size_t {0});
    failures += expect_status((*reopened)->drop_collection(any_collection_id_t {*doomed}), status_code_t::not_found_k);

    // The name returns under a new UUID in the record the drop freed, which the old UUID misses.
    expected<collection_id_t> const reborn = create_named(**reopened, "doomed", test_config(8, 0));
    failures += expect(reborn.has_value());
    if (!reborn) return failures;
    failures += expect(!(*reopened)->has_collection(any_collection_id_t {*doomed}));
    failures += expect((*reopened)->collection_name(*doomed).empty());
    failures += expect((*reopened)->collection_id(any_collection_id_t {owned_name("doomed")}) == *reborn);
    failures += expect((*reopened)->drop_collection(any_collection_id_t {*reborn}).succeeded());

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
case_result_t crash_between_seal_and_schema(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    std::size_t failures = 0;

    {
        expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
        failures += expect_status(stack, status_code_t::success_k);
        if (!stack) return failures;
        auto const& fixture = *(*stack)->fixture;

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

        // A flush writes its runs and publishes the manifest second, so the manifest already
        // names every run.
        failures += expect_eq(run_count_of(fixture), run_files_of(fixture));
        failures += expect((*stack)->store->unmount().succeeded());
        (*stack)->store.reset();

        std::size_t const runs = run_count_of(fixture);
        failures += expect(runs != std::size_t {0});
        failures += expect(run_bytes_of(fixture) != std::uint64_t {0});

        // A mount keeps every run the schema names.
        {
            expected<std::unique_ptr<database_t>> kept = open_database(**stack);
            failures += expect(kept.has_value());
            if (!kept) return failures;
            failures += expect_eq(run_count_of(fixture), runs);
            failures += expect((*kept)->unmount().succeeded());
        }

        // No manifest that would have named those runs ever landed.
        {
            std::array<char, 96> name {};
            auto const written = fmt::format_to_n(name.data(), name.size() - 1, "{}.ustore.json", fixture.suite);
            *written.out = '\0';
            path_t const manifest {fixture.data_directory_path(0).c_str(), name.data()};
            failures += expect(std::filesystem::remove(manifest.c_str()));
        }

        expected<std::unique_ptr<database_t>> reopened = open_database(**stack);
        failures += expect(reopened.has_value());
        if (!reopened) return failures;
        failures += expect_eq((*reopened)->size(), std::size_t {0});
        failures += expect(!(*reopened)->has_collection(any_collection_id_t {owned_name("lost")}));
        failures += expect_eq(run_files_of(fixture), std::size_t {0});

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
        auto const& fixture = *(*stack)->fixture;

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

        manifest_t const manifest = manifest_on_disk(fixture);
        failures += expect(!manifest.files.empty());
        if (manifest.files.empty()) return failures;

        // The file of one run the manifest names never reached the environment.
        char name[path_max_bytes_k];
        failures += expect(manifest.file_name(manifest.files[0].run, name).succeeded());
        path_t const run_path {fixture.data_directory_path(0).c_str(), name};
        failures += expect(std::filesystem::remove(run_path.c_str()));

        expected<std::unique_ptr<database_t>> const reopened = open_database(**stack);
        failures += expect(!reopened.has_value());
    }
    return failures;
}

/** A collection is listed from the stamp its create drew and up to the one its unlisting drew,
 *  so a reader pinned before the create or past the unlisting finds no store tag, and one pinned
 *  between still does. */
template <typename mount_type_>
std::size_t collections_list_by_stamp(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;

    transaction_stamp_t const before = store.ledger().published();
    expected<collection_id_t> const listed = create_named(store, "listed", test_config(8, 0));
    failures += expect(listed.has_value());
    if (!listed) return failures;
    transaction_stamp_t const created = store.ledger().published();
    std::uint16_t tag = 0;
    failures += expect_status(store.listed_store_tag(*listed, before, tag), status_code_t::not_found_k);
    failures += expect(store.listed_store_tag(*listed, created, tag).succeeded());
    failures += expect(store.listed_store_tag(*listed, head_stamp_k, tag).succeeded());
    failures += expect_eq(tag, static_cast<std::uint16_t>(value_layout_t::inline_12_bytes_k));

    failures += expect(store.unlist_collection(*listed).succeeded());
    failures += expect(store.listed_store_tag(*listed, created, tag).succeeded());
    failures += expect_status(store.listed_store_tag(*listed, head_stamp_k, tag), status_code_t::not_found_k);
    failures += expect_status(store.listed_store_tag(*listed, store.ledger().published(), tag),
                              status_code_t::not_found_k);
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
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    std::size_t failures = 0;
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
    failures += expect(capture_data_directory(*(*stack)->fixture).succeeded());
    (*stack)->store.reset();
    (*stack)->volume = typename types_t::volume_t {};
    failures += expect(restore_data_directory(*(*stack)->fixture).succeeded());
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

/**
 *  @brief A merge of memory runs keeps every version the flush after it promises a mount.
 *
 *  The merged collection writes a key twice around a commit of a second collection that stays in
 *  memory, so the manifest its flush publishes is durable only below that commit. A crash then
 *  truncates there, and the first write of the key, which the merge had folded under the second, is
 *  what the mount must still find.
 */
template <typename mount_type_>
case_result_t merges_keep_what_the_flush_promises(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;

    // Two runs of tier zero merge, and the merged run with one more delta makes a flush due.
    collection_config_t merging = test_config(8, 0);
    merging.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;
    merging.memory_runs_max_deltas = 2;
    expected<collection_id_t> const merged = create_named(store, "merged", merging);
    expected<collection_id_t> const beside = create_named(store, "beside", test_config(8, 0));
    failures += expect(merged.has_value() && beside.has_value());
    if (failures != 0) return failures;
    any_collection_id_t const id {*merged};

    auto write = [&](any_collection_id_t const& written_id, stored_key_t key, std::uint64_t value) noexcept {
        std::array<byte_t, 8> const counter = counted(value);
        return expect(store.insert(written_id, key, stored_value_view_t {counter}).succeeded());
    };
    failures += write(id, stored_key_t {0}, 1);
    failures += write(any_collection_id_t {*beside}, stored_key_t {0}, 2);
    failures += write(id, stored_key_t {0}, 3);
    failures += write(id, stored_key_t {1}, 4);

    std::array<byte_t, 8> into {};
    stored_value_size_t size = 0;
    failures += expect(store.select(id, stored_key_t {0}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(counter_of(std::span<byte_t const> {into.data(), size}), std::uint64_t {3});
    failures += expect(capture_data_directory(*(*stack)->fixture).succeeded());
    (*stack)->store.reset();
    (*stack)->volume = typename types_t::volume_t {};
    failures += expect(restore_data_directory(*(*stack)->fixture).succeeded());
    expected<std::unique_ptr<database_t>> reopened = open_database(**stack);
    failures += expect(reopened.has_value());
    if (failures != 0) return failures;

    // The second collection's commit was never durable, so the mount resumes below it, at
    // the first write.
    failures += expect((*reopened)->select(id, stored_key_t {0}, std::span<byte_t> {into}, size).succeeded());
    failures += expect_eq(counter_of(std::span<byte_t const> {into.data(), size}), std::uint64_t {1});
    failures += expect(!(*reopened)->contains(any_collection_id_t {*beside}, stored_key_t {0}));
    return failures;
}

/** The bytes a caller saves next to the database survive a reopen and a clearing. */
template <typename mount_type_>
case_result_t user_data_round_trips(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
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

/** A database over either volume, which an archive saves from a directory onto raw
 *  media and back. */
template <typename fixture_type_>
struct archive_types {
    using volume_t = any_volume<typename fixture_type_::disk_t, waiter_t, volume_allocator_t>;
    using store_t = nvme_store<key_slot<stored_key_t, value_layout_t::variable_width_k>, volume_t, atomic_reference,
                               waiter_t, last_write_fold_t, no_device_fold_t>;
    using database_t = basic_database<stored_key_t, volume_t, atomic_reference, waiter_t, st::native_row_kit_t,
                                      layout_store_tags<store_t>, store_t>;
};

/** Steps the serial SHA-256 kernels, counting every block it hashes into @c blocks. */
struct counted_sha256_t {
    strings_kit_sha256<serial_strings_kit_t> kernels;
    std::size_t* blocks = nullptr;

    void init(sz_sha256_state_t& state) const noexcept { kernels.init(state); }

    void update(sz_sha256_state_t& state, std::span<byte_t const> bytes) const noexcept {
        ++*blocks;
        kernels.update(state, bytes);
    }

    [[nodiscard]] sha256_t digest(sz_sha256_state_t const& state) const noexcept { return kernels.digest(state); }
};

/** Mounts a volume over @p address and a database over it, which checks the runs with
 *  @p digester, answering both or why either was refused. */
template <typename types_type_, typename fixture_type_,
          typename digester_type_ = strings_kit_sha256<serial_strings_kit_t>>
status_t open_archive(fixture_type_& fixture, io_channel_t& channel, char const* database, char const* address,
                      typename types_type_::volume_t& volume, std::unique_ptr<typename types_type_::database_t>& store,
                      digester_type_ const& digester = {}) noexcept {
    auto [parsed, parse_status] = parse_data_address(address);
    if (parse_status.failed()) return parse_status;
    data_address_t const addresses[] {parsed};
    volume_config_t config;
    config.database = string_view_t {database};
    config.data_directories = addresses;
    config.runs_capacity = fixture.config.runs_capacity;
    auto [mounted, mount_status] = types_type_::volume_t::mount(
        fixture.disk, channel, config, volume_allocator_t {fixture.resources->memory_constraint});
    if (mount_status.failed()) return mount_status;
    volume = std::move(mounted);
    store.reset(new (std::nothrow) typename types_type_::database_t {fixture.resources});
    if (!store) return {status_code_t::out_of_memory_k, "The database did not fit"};
    return store->mount(volume, fixture.channels, test_database_config(), {}, digester);
}

/** Saves the head of @p store as the database @p database at @p address, answering what
 *  the save placed. */
template <typename database_type_>
expected<typename database_type_::archive_report_t> save_head(database_type_& store, char const* database,
                                                              char const* address) noexcept {
    auto [parsed, parse_status] = parse_data_address(address);
    if (parse_status.failed()) return parse_status;
    data_address_t const addresses[] {parsed};
    volume_config_t target;
    target.database = string_view_t {database};
    target.data_directories = addresses;
    expected<typename database_type_::snapshot_t> pinned = store.pin_stamp();
    if (!pinned) return pinned.status();
    return store.save(
        target, pinned->stamp(), [](std::uint64_t, std::uint64_t) noexcept { return true; },
        strings_kit_sha256<serial_strings_kit_t> {});
}

/** Counts the keys below @p count whose values in @p store differ from the painted ones of
 *  round @p round. */
template <typename database_type_>
[[nodiscard]] std::size_t differing_keys(database_type_& store, collection_id_t collection, std::uint64_t count) {
    std::size_t differing = 0;
    std::array<byte_t, 512> into {};
    for (std::uint64_t key = 0; key != count; ++key) {
        stored_value_size_t size = 0;
        status_t const read = store.select(any_collection_id_t {collection}, static_cast<stored_key_t>(key),
                                           std::span<byte_t> {into}, size);
        bool same = read.succeeded() && size == blob_bytes_of(key);
        for (std::size_t position = 0; same && position != size; ++position)
            same = into[position] == painted(key, position);
        differing += !same;
    }
    return differing;
}

/** A database saved from a directory onto a raw medium twice writes only its new runs the second
 *  time, naming every run's SHA-256; the raw copy restores every byte, saving it back into a
 *  directory restores them again, only the first open of a restore hashes its runs, and a run
 *  whose bytes were corrupted is refused. */
template <typename mount_type_>
case_result_t archive_saves_incrementally_both_ways(mount_type_ const& mount) {
    using types_t = archive_types<typename mount_type_::fixture_t>;
    using volume_t = typename types_t::volume_t;
    using database_t = typename types_t::database_t;
    if (mount.transport == transport_t::memory_k || mount.transport == transport_t::device_k)
        return case_result_t::skipped("needs a file standing in for a device");
    std::size_t failures = 0;
    auto [fixture, mount_status] = mount();
    failures += expect(mount_status.succeeded());
    if (mount_status.failed()) return failures;
    auto [guard, acquire_status] = fixture->channels.acquire();
    failures += expect(acquire_status.succeeded());
    if (acquire_status.failed()) return failures;
    io_channel_t& channel = guard.channel();

    path_t const source = fixture->data_directory_path(0), restored = fixture->data_directory_path(1);
    path_t const device {fixture->directory, "archive.raw"}, corrupted {fixture->directory, "corrupted"};
    {
        std::FILE* const file = std::fopen(device.c_str(), "wb");
        if (file) std::fclose(file);
        std::error_code error;
        std::filesystem::resize_file(device.c_str(), 4 * slot_bytes_k, error);
        failures += expect(!error);
    }
    std::array<char, path_capacity_k + 8> raw {};
    *fmt::format_to_n(raw.data(), raw.size() - 1, "{}@0", device.c_str()).out = '\0';

    collection_id_t collection;
    typename database_t::archive_report_t first, second;
    {
        volume_t volume;
        std::unique_ptr<database_t> store;
        failures += expect(
            open_archive<types_t>(*fixture, channel, "source", source.c_str(), volume, store).succeeded());
        if (failures != 0) return failures;
        auto [created, create_status] = create_named(*store, "blobs", test_config(0, value_max_size_k));
        failures += expect(create_status.succeeded());
        if (create_status.failed()) return failures;
        collection = created;
        st::vector<byte_t> value;
        failures += expect(value.resize(512) == st::status_t::success_k);
        auto write = [&](std::uint64_t begin, std::uint64_t end) noexcept {
            for (std::uint64_t key = begin; key != end; ++key) {
                for (std::size_t position = 0; position != blob_bytes_of(key); ++position)
                    value[position] = painted(key, position);
                failures += expect(store
                                       ->insert(any_collection_id_t {collection}, static_cast<stored_key_t>(key),
                                                stored_value_view_t {value.data(), blob_bytes_of(key)})
                                       .succeeded());
            }
            failures += expect(store->flush().succeeded());
        };
        write(0, 200);
        auto [saved, save_status] = save_head(*store, "archived", raw.data());
        failures += expect(save_status.succeeded());
        first = saved;
        failures += expect(first.written != 0 && first.kept == 0);

        // The second save keeps every run the first wrote and writes only the run of the new keys.
        write(200, 300);
        auto [again, again_status] = save_head(*store, "archived", raw.data());
        failures += expect(again_status.succeeded());
        second = again;
        failures += expect_eq(second.kept, first.written);
        failures += expect(second.written != 0 && second.written < first.written + second.written);
        failures += expect(store->unmount().succeeded());
    }
    {
        volume_t volume;
        std::unique_ptr<database_t> store;
        failures += expect(open_archive<types_t>(*fixture, channel, "archived", raw.data(), volume, store).succeeded());
        if (failures != 0) return failures;
        failures += expect_eq(differing_keys(*store, collection, 300), std::size_t {0});
        for (manifest_file_t const& file : volume.manifest().files) failures += expect(!file.sha256.empty());

        // The raw copy saves back into a directory, which restores every byte again.
        auto [back, back_status] = save_head(*store, "archived", restored.c_str());
        failures += expect(back_status.succeeded());
        failures += expect(back.written + back.linked == first.written + second.written);
        std::error_code error;
        std::filesystem::create_directory(corrupted.c_str(), error);
        failures += expect(save_head(*store, "archived", corrupted.c_str()).has_value());
        failures += expect(store->unmount().succeeded());
    }
    for (std::size_t open = 0; open != 2; ++open) {
        std::size_t hashed_blocks = 0;
        volume_t volume;
        std::unique_ptr<database_t> store;
        failures += expect(open_archive<types_t>(*fixture, channel, "archived", restored.c_str(), volume, store,
                                                 counted_sha256_t {{}, &hashed_blocks})
                               .succeeded());
        if (failures != 0) return failures;
        failures += expect((hashed_blocks != 0) == (open == 0));
        failures += expect_eq(differing_keys(*store, collection, 300), std::size_t {0});
        failures += expect(store->unmount().succeeded());
    }

    // One flipped byte in the middle of the largest run of a restore never opened fails it.
    std::filesystem::path largest;
    std::uintmax_t largest_bytes = 0;
    std::error_code error;
    for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(corrupted.c_str(), error))
        if (entry.path().extension() == ".ustore" && entry.file_size() > largest_bytes) {
            largest = entry.path();
            largest_bytes = entry.file_size();
        }
    failures += expect(largest_bytes != 0);
    std::FILE* const file = std::fopen(largest.c_str(), "r+b");
    failures += expect(file != nullptr);
    if (!file) return failures;
    long const middle = static_cast<long>(largest_bytes / 2);
    failures += expect(std::fseek(file, middle, SEEK_SET) == 0);
    int const byte = std::fgetc(file);
    failures += expect(std::fseek(file, middle, SEEK_SET) == 0);
    failures += expect(std::fputc(byte ^ 0xFF, file) != EOF);
    std::fclose(file);
    volume_t volume;
    std::unique_ptr<database_t> store;
    failures += expect_status(open_archive<types_t>(*fixture, channel, "archived", corrupted.c_str(), volume, store),
                              status_code_t::malformed_k);
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

/** A transaction writing one collection hands its buffer to the run its commit lands, so a read of
 *  the run's values views the very bytes the transaction wrote, which no seal copied; one filling
 *  less than half its buffer is copied, so a run never holds twice what its bounds count. */
template <typename mount_type_>
std::size_t single_collection_commit_keeps_its_bytes(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t keys_k = 6;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;
    expected<collection_id_t> const own = create_named(store, "own", test_config(0, value_max_size_k));
    failures += expect(own.has_value());
    if (failures != 0) return failures;

    // Commits a transaction of @p capacity bytes over the keys of @p round, answering how many
    // values a read views outside the bytes the transaction wrote.
    auto moved_values = [&](std::size_t capacity, std::size_t round) {
        std::size_t moved = keys_k;
        transaction_config_t shape;
        shape.max_bytes = capacity;
        shape.max_deltas = keys_k;
        expected<typename database_t::transaction_unique_ptr_t> transaction = store.create_transaction(shape);
        failures += expect(transaction.has_value());
        if (!transaction) return moved;
        std::array<stored_key_t, keys_k> keys {};
        std::array<byte_t const*, keys_k> written {};
        for (std::size_t key = 0; key != keys_k; ++key) {
            std::array<byte_t, 512> value {};
            std::size_t const width = blob_bytes_of(key);
            for (std::size_t position = 0; position != width; ++position)
                value[position] = painted(key + round, position);
            keys[key] = static_cast<stored_key_t>(key);
            failures += expect(
                (*transaction)
                    ->insert(any_collection_id_t {*own}, keys[key], stored_value_view_t {value.data(), width})
                    .succeeded());
            delta_view_t held;
            failures += expect((*transaction)->uncommitted_delta(*own, keys[key], held));
            written[key] = held.bytes.data();
        }
        failures += expect((*transaction)->commit().succeeded());

        std::array<lookup_result_t, keys_k> results {};
        std::array<byte_t const*, keys_k> viewed {};
        failures += expect(
            store
                .select(any_collection_id_t {*own}, std::span<stored_key_t const> {keys},
                        std::span<lookup_result_t> {results},
                        [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
                            if (offset == 0) viewed[request] = bytes.data();
                        })
                .succeeded());
        moved = 0;
        for (std::size_t key = 0; key != keys_k; ++key)
            moved += results[key] != lookup_result_t::written_k || viewed[key] != written[key];
        return moved;
    };
    std::size_t filled = 0;
    for (std::size_t key = 0; key != keys_k; ++key) filled += blob_bytes_of(key);
    failures += expect_eq(moved_values(filled, 0), std::size_t {0});
    failures += expect_eq(moved_values(filled * 4, 1), keys_k);
    return failures;
}

/** Transactions writing one collection, and some writing a second beside it, land tier-zero runs
 *  that merge four at a time, then flush, and every key reads back what its last commit wrote, from
 *  the merged runs and from the level. */
template <typename mount_type_>
std::size_t merged_and_flushed_values_read_back(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t rounds_k = 64;
    constexpr std::uint64_t keys_k = 40;
    std::size_t failures = 0;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;
    expected<collection_id_t> const own = create_named(store, "own", test_config(0, value_max_size_k));
    expected<collection_id_t> const beside = create_named(store, "beside", test_config(0, value_max_size_k));
    failures += expect(own.has_value());
    failures += expect(beside.has_value());
    if (failures != 0) return failures;

    // The round that last wrote each key, plus one, so zero stands for a key never written.
    std::array<std::uint64_t, keys_k> own_rounds {};
    std::array<std::uint64_t, keys_k> beside_rounds {};
    auto paint = [](std::uint64_t key, std::uint64_t round, std::array<byte_t, 512>& value) noexcept {
        std::size_t const width = blob_bytes_of(key + round);
        for (std::size_t position = 0; position != width; ++position) value[position] = painted(key + round, position);
        return width;
    };
    auto read_back = [&](collection_id_t collection, std::span<std::uint64_t const> rounds) {
        std::size_t mismatches = 0;
        for (std::uint64_t key = 0; key != keys_k; ++key) {
            if (rounds[key] == 0) continue;
            std::array<byte_t, 512> wanted {};
            std::size_t const width = paint(key, rounds[key] - 1, wanted);
            std::array<byte_t, 512> into {};
            stored_value_size_t size = 0;
            status_t const selected = store.select(any_collection_id_t {collection}, static_cast<stored_key_t>(key),
                                                   std::span<byte_t> {into}, size);
            mismatches += selected.failed() || size != width || std::memcmp(into.data(), wanted.data(), width) != 0;
        }
        return expect_eq(mismatches, std::size_t {0});
    };

    for (std::uint64_t round = 0; round != rounds_k && failures == 0; ++round) {
        // Sized to the widest values, so a buffer of one collection is over half full and lent.
        transaction_config_t shape;
        shape.max_deltas = (round % 6 == 5 ? 2 : 1) * (1 + round % 5);
        shape.max_bytes = shape.max_deltas * blob_bytes_of(96);
        expected<typename database_t::transaction_unique_ptr_t> transaction = store.create_transaction(shape);
        failures += expect(transaction.has_value());
        if (!transaction) break;
        for (std::uint64_t step = 0; step != 1 + round % 5; ++step) {
            stored_key_t const key = static_cast<stored_key_t>((round * 7 + step * 3) % keys_k);
            std::array<byte_t, 512> value {};
            std::size_t const width = paint(key, round, value);
            failures += expect((*transaction)
                                   ->insert(any_collection_id_t {*own}, key, stored_value_view_t {value.data(), width})
                                   .succeeded());
            own_rounds[key] = round + 1;
            if (round % 6 != 5) continue;
            failures += expect(
                (*transaction)
                    ->insert(any_collection_id_t {*beside}, key, stored_value_view_t {value.data(), width})
                    .succeeded());
            beside_rounds[key] = round + 1;
        }
        failures += expect((*transaction)->commit().succeeded());
        if (round % 16 == 15) failures += read_back(*own, own_rounds) + read_back(*beside, beside_rounds);
    }
    failures += expect(store.memory_shape(any_collection_id_t {*own}).runs < rounds_k / 4);
    failures += expect(store.flush().succeeded());
    failures += read_back(*own, own_rounds) + read_back(*beside, beside_rounds);
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

/**
 *  @brief Threads writing one key per commit into a collection with a small memory-run
 *      pool all land.
 *
 *  The pool holds 29 records, so the threads take every record long before the bounds make a flush
 *  due, and a writer past the last record must wait for the flush that gives records back instead
 *  of being refused.
 */
template <typename mount_type_>
std::size_t head_writes_wait_for_records(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr std::size_t threads_k = 8;
    constexpr std::size_t writes_k = 200;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;
    expected<collection_id_t> const written = create_named(store, "written", test_config(8, 0));
    failures += expect(written.has_value());
    if (!written) return failures;
    any_collection_id_t const id {*written};

    std::size_t refused {0};
    std::array<std::thread, threads_k> writers;
    for (std::size_t writer = 0; writer != threads_k; ++writer)
        writers[writer] = std::thread {[&, writer]() noexcept {
            for (std::size_t step = 0; step != writes_k; ++step) {
                stored_key_t const key = static_cast<stored_key_t>(writer * writes_k + step);
                std::array<byte_t, 8> const counter = counted(key);
                if (status_t const inserted = store.insert(id, key, stored_value_view_t {counter}); inserted.failed()) {
                    if (atomic_reference<std::size_t> {refused}.fetch_add(1, std::memory_order_relaxed) == 0)
                        fmt::println(stderr, "  first refused write: {}", inserted.message);
                }
            }
        }};
    for (std::thread& writer : writers) writer.join();
    failures += expect_eq(atomic_reference<std::size_t> {refused}.load(std::memory_order_relaxed), std::size_t {0});

    std::size_t missing = 0;
    std::array<byte_t, 8> into {};
    for (stored_key_t key = 0; key != threads_k * writes_k; ++key) {
        stored_value_size_t size = 0;
        if (store.select(id, key, std::span<byte_t> {into}, size).failed() ||
            counter_of(std::span<byte_t const> {into.data(), size}) != key)
            ++missing;
    }
    failures += expect_eq(missing, std::size_t {0});
    return failures;
}

/**
 *  @brief Transactions writing only strings land when more of them pass the stall check than the
 *      pool has records.
 *
 *  An open ingest holds the claim, so no merge or flush frees a record. The strings collection is
 *  filled to one run below the stall bound, and a staged transaction holds the leader word while
 *  eight writers seal past the stall check. Their requests would land one record past the pool's
 *  29, so a writer must wait for one instead of having its share refused, and every write lands
 *  once the claim goes back.
 */
template <typename mount_type_>
std::size_t strings_shares_wait_for_records(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    using strings_store_t = typename types_t::strings_store_t;
    using strings_transaction_t = strings::nvme_transaction<database_t, strings_store_t>;
    using key_slot_t = typename strings_store_t::key_slot_t;
    std::size_t failures = 0;
    constexpr std::size_t writers_k = 8;
    // One run below the stall bound of test_config, (F - 1)(T + 1) runs for F = 4 and T = 7.
    constexpr std::size_t filled_runs_k = 23;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;
    collection_id_t names;
    std::memcpy(names.uuid.bytes, uuid_generator().bytes, sizeof(names.uuid.bytes));
    failures += expect(store
                           .create_collection(names, "names", static_cast<std::uint16_t>(value_layout_t::uuid_k),
                                              test_config(sizeof(ustore_uuid_t), 0))
                           .succeeded());
    expected<collection_id_t> const held = create_named(store, "held", test_config(8, 0));
    failures += expect(held.has_value());
    if (failures != 0) return failures;

    auto text_of = [](std::size_t index) { return fmt::format("string-{}", index); };
    auto value_of = [](std::size_t index) {
        std::array<byte_t, key_slot_t::size> value {};
        for (std::size_t position = 0; position != value.size(); ++position) value[position] = painted(index, position);
        return value;
    };
    // Writes string @p index into @p written over a fresh transaction, which the caller
    // stages or commits.
    auto begin_write = [&](strings_transaction_t& written, std::size_t index) {
        auto [transaction, begin_status] = store.create_transaction();
        if (begin_status.failed()) return expected<typename database_t::transaction_unique_ptr_t> {begin_status};
        written.join(*transaction);
        std::array<byte_t, key_slot_t::size> const value = value_of(index);
        if (status_t const wrote = written.write(names, text_of(index), delta_view_t {delta_kind_t::write_k, value},
                                                 invalid_transaction_stamp_k);
            wrote.failed())
            return expected<typename database_t::transaction_unique_ptr_t> {wrote};
        return expected<typename database_t::transaction_unique_ptr_t> {std::move(transaction)};
    };

    std::size_t refused {0};
    std::size_t started {0};
    std::array<std::thread, writers_k> writers;
    {
        typename database_t::ingest_t ingest;
        failures += expect(store.begin_ingest(any_collection_id_t {*held}, ingest).succeeded());
        for (std::size_t index = 0; index != filled_runs_k; ++index) {
            strings_transaction_t written {store};
            auto transaction = begin_write(written, index);
            failures += expect(transaction.has_value() && written.commit(**transaction).succeeded());
        }
        failures += expect_eq(store.memory_shape(any_collection_id_t {names}).runs, filled_runs_k);
        if (failures != 0) return failures;

        strings_transaction_t staged_strings {store};
        auto staged = begin_write(staged_strings, filled_runs_k);
        failures += expect(staged.has_value() && staged_strings.stage(**staged).succeeded());
        if (failures != 0) return failures;
        for (std::size_t writer = 0; writer != writers_k; ++writer)
            writers[writer] = std::thread {[&, writer]() noexcept {
                strings_transaction_t written {store};
                auto transaction = begin_write(written, filled_runs_k + 1 + writer);
                atomic_reference<std::size_t> {started}.fetch_add(1, std::memory_order_release);
                status_t const committed = transaction ? written.commit(**transaction) : transaction.status();
                if (committed.failed() &&
                    atomic_reference<std::size_t> {refused}.fetch_add(1, std::memory_order_relaxed) == 0)
                    fmt::println(stderr, "  first refused share: {}", committed.message);
            }};
        // Every writer seals past the stall check and parks behind the staged transaction or
        // on a record.
        while (atomic_reference<std::size_t> {started}.load(std::memory_order_acquire) != writers_k)
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        failures += expect((*staged)->commit().succeeded());
    }
    for (std::thread& writer : writers) writer.join();
    failures += expect_eq(atomic_reference<std::size_t> {refused}.load(std::memory_order_relaxed), std::size_t {0});

    std::size_t missing = 0;
    status_t const visited = store.visit_store(
        any_collection_id_t {names}, [&]<typename store_type_>(store_type_& visited_store) noexcept -> status_t {
            if constexpr (std::is_same_v<store_type_, strings_store_t>)
                for (std::size_t index = 0; index != filled_runs_k + 1 + writers_k; ++index) {
                    std::array<byte_t, key_slot_t::size> value {};
                    if (visited_store.find(text_of(index), value) != lookup_result_t::written_k ||
                        value != value_of(index))
                        ++missing;
                }
            return {};
        });
    failures += expect(visited.succeeded());
    failures += expect_eq(missing, std::size_t {0});
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
        std::uint64_t const bound = (fan_in - 1) * (tiers + 1);
        if (shape.runs > bound)
            fmt::println(stderr, "  {} runs past {} after {} commits", shape.runs, bound, shape.commits);
        failures += expect(shape.runs <= bound);
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

#pragma region Change Feed Cases

/** One change a feed handed out, with the counter its value carries, or zero for a removal. */
struct fed_change_t {
    stored_change<stored_key_t> change;
    std::uint64_t counter = 0;
};

/** Pulls changes of @p id after @p handed into @p fed, @p wanted at a time, advancing @p handed. */
template <typename database_type_>
std::size_t pull_changes(database_type_& store, any_collection_id_t const& id, stored_change<stored_key_t>& handed,
                         std::size_t wanted, st::vector<fed_change_t>& fed) {
    std::size_t failures = 0;
    for (;;) {
        std::size_t pulled = 0;
        stored_change<stored_key_t> const after = handed;
        auto const take = [&]<typename read_type_>(stored_change<stored_key_t> const& change, std::uint64_t value_bytes,
                                                   read_type_&& read) noexcept -> status_t {
            std::array<byte_t, 8> bytes {};
            if (value_bytes > bytes.size()) return {status_code_t::invalid_argument_k, "A value wider than a counter"};
            std::span<byte_t> const value {bytes.data(), static_cast<std::size_t>(value_bytes)};
            if (status_t const copied = read(value); copied.failed()) return copied;
            if (fed.push_back(fed_change_t {change, counter_of(value)}) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "No room for the changes fed"};
            handed = change;
            ++pulled;
            return {};
        };
        failures += expect(store.changes_after(id, after, wanted, take).succeeded());
        failures += expect(pulled <= wanted);
        if (failures != 0 || pulled == 0) return failures;
    }
}

/** Whether @p fed matches @p logged change for change, stamps, kinds and counters alike. */
[[nodiscard]] std::size_t expect_fed(st::vector<fed_change_t> const& fed, st::vector<fed_change_t> const& logged) {
    std::size_t failures = expect_eq(fed.size(), logged.size());
    for (std::size_t index = 0; index != fed.size() && index != logged.size(); ++index) {
        stored_change<stored_key_t> const& got = fed[index].change;
        stored_change<stored_key_t> const& wanted = logged[index].change;
        failures += expect(got.stamp == wanted.stamp && got.stored_key == wanted.stored_key && got.kind == wanted.kind);
        failures += expect_eq(fed[index].counter, logged[index].counter);
    }
    return failures;
}

/** A feed pinned below a history of writes, removals and a range removal reads every version back
 *  in commit order across flushes and merges, which keep the versions above its stamp; the range
 *  removal comes back as the removal of every key it hid a value of, among its commit's writes by
 *  key, and not for a key its commit wrote again. */
template <typename mount_type_>
std::size_t feed_reads_across_flushes_and_merges(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr stored_key_t keys_k = 6, ranged_from_k = 3, ranged_to_k = 8, removed_k = 3, beside_k = 2;
    constexpr stored_key_t rewritten_k = 4, hidden_k = 5;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;
    collection_config_t config = test_config(8, 0);
    config.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;
    expected<collection_id_t> const fed_collection = create_named(store, "fed", config);
    failures += expect(fed_collection.has_value());
    if (!fed_collection) return failures;
    any_collection_id_t const id {*fed_collection};

    for (stored_key_t key = 0; key != keys_k; ++key) {
        std::array<byte_t, 8> const counter = counted(key + 100);
        failures += expect(store.insert(id, key, stored_value_view_t {counter}).succeeded());
    }
    transaction_stamp_t const since = store.ledger().published();
    typename database_t::pin_t pin;
    failures += expect(store.pin_feed(id, pin, since).succeeded());

    st::vector<fed_change_t> logged;
    auto const log = [&](stored_key_t key, delta_kind_t kind, std::uint64_t counter) {
        stored_change<stored_key_t> const change {store.ledger().published(), key, kind};
        return expect(logged.push_back(fed_change_t {change, counter}) == st::status_t::success_k);
    };
    for (std::uint64_t round = 1; round != 3; ++round) {
        for (stored_key_t key = 0; key != keys_k; ++key) {
            std::array<byte_t, 8> const counter = counted(key + 100 * round);
            failures += expect(store.insert(id, key, stored_value_view_t {counter}).succeeded());
            failures += log(key, delta_kind_t::write_k, key + 100 * round);
        }
        failures += expect(store.flush().succeeded());
    }
    failures += expect(store.compact().succeeded());
    failures += expect(store.remove(id, removed_k).succeeded());
    failures += log(removed_k, delta_kind_t::remove_k, 0);

    expected<typename database_t::transaction_unique_ptr_t> transaction = store.create_transaction();
    failures += expect(transaction.has_value());
    if (!transaction) return failures;
    std::array<byte_t, 8> const beside = counted(2000);
    std::array<byte_t, 8> const rewritten = counted(4000);
    failures += expect((*transaction)->insert(id, beside_k, stored_value_view_t {beside}).succeeded());
    failures += expect((*transaction)->remove_range(id, ranged_from_k, ranged_to_k).succeeded());
    failures += expect((*transaction)->insert(id, rewritten_k, stored_value_view_t {rewritten}).succeeded());
    failures += expect((*transaction)->commit().succeeded());
    failures += log(beside_k, delta_kind_t::write_k, 2000);
    failures += log(rewritten_k, delta_kind_t::write_k, 4000);
    failures += log(hidden_k, delta_kind_t::remove_k, 0);
    (*transaction).reset();
    failures += expect(store.flush().succeeded());
    failures += expect(store.compact().succeeded());
    if (failures != 0) return failures;

    // Three at a time resumes inside the last commit, and a second pass rereads the same history.
    for (std::size_t wanted : {std::size_t {3}, std::size_t {1000}}) {
        st::vector<fed_change_t> fed;
        stored_change<stored_key_t> handed {since, std::numeric_limits<stored_key_t>::max()};
        failures += pull_changes(store, id, handed, wanted, fed);
        failures += expect_fed(fed, logged);
    }
    return failures;
}

/** A feed keeps every version above its stamp out of a flush's reach until it is freed, while a
 *  second collection folds beside it; afterwards a flush folds the versions again, and a feed below
 *  the stamp it folded at is refused while one at the head opens. */
template <typename mount_type_>
std::size_t freed_feed_lets_merges_fold(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    std::size_t failures = 0;
    constexpr stored_key_t key_k = 1;
    constexpr std::uint64_t rewrites_k = 4;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    failures += expect_status(stack, status_code_t::success_k);
    if (!stack) return failures;
    database_t& store = *(*stack)->store;
    expected<collection_id_t> const folded = create_named(store, "folded", test_config(8, 0));
    failures += expect(folded.has_value());
    if (!folded) return failures;
    expected<collection_id_t> const beside = create_named(store, "beside", test_config(8, 0));
    failures += expect(beside.has_value());
    if (!beside) return failures;
    any_collection_id_t const id {*folded};
    auto const rewrite = [&](any_collection_id_t const& written_id, std::uint64_t first) {
        std::size_t written = 0;
        for (std::uint64_t round = first; round != first + rewrites_k; ++round) {
            std::array<byte_t, 8> const counter = counted(round);
            written += expect(store.insert(written_id, key_k, stored_value_view_t {counter}).succeeded());
        }
        return written;
    };

    failures += rewrite(id, 0);
    transaction_stamp_t const since = store.ledger().published();
    typename database_t::pin_t pin;
    failures += expect(store.pin_feed(id, pin, since).succeeded());
    failures += rewrite(id, rewrites_k);
    failures += expect(store.flush().succeeded());
    std::uint64_t const pinned_population = store.population_count();
    // The version the head read at the feed's stamp, and every rewrite above it.
    failures += expect_eq(pinned_population, rewrites_k + 1);
    failures += rewrite(any_collection_id_t {*beside}, 0);
    failures += expect(store.flush().succeeded());
    failures += expect_eq(store.population_count(), pinned_population + 1);
    st::vector<fed_change_t> fed;
    stored_change<stored_key_t> handed {since, std::numeric_limits<stored_key_t>::max()};
    failures += pull_changes(store, id, handed, 2, fed);
    failures += expect_eq(fed.size(), std::size_t {rewrites_k});

    pin.release();
    failures += rewrite(id, 2 * rewrites_k);
    failures += expect(store.flush().succeeded());
    failures += expect_eq(store.population_count(), pinned_population + 2);
    failures += expect_status(store.pin_feed(id, pin, since), status_code_t::write_conflict_k);
    failures += expect(!pin.held());
    failures += expect(store.pin_feed(id, pin, store.ledger().published()).succeeded());
    return failures;
}

#pragma endregion Change Feed Cases

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
        report(totals, mount, "range_removal_survives_reopen", range_removal_survives_reopen);
        report(totals, mount, "strings_range_removal_survives_reopen", strings_range_removal_survives_reopen);
        report(totals, mount, "dropped_collection_is_gone", dropped_collection_is_gone);
        report(totals, mount, "collections_list_by_stamp", collections_list_by_stamp);
        report(totals, mount, "crash_before_schema", crash_between_seal_and_schema);
        report(totals, mount, "crash_keeps_a_prefix", crash_keeps_a_prefix_of_commits);
        report(totals, mount, "merges_keep_what_the_flush_promises", merges_keep_what_the_flush_promises);
        report(totals, mount, "user_data", user_data_round_trips);
        report(totals, mount, "archive_both_ways", archive_saves_incrementally_both_ways);
        report(totals, mount, "commit_is_all_or_nothing", commit_is_all_or_nothing);
        report(totals, mount, "single_collection_commit_keeps_its_bytes", single_collection_commit_keeps_its_bytes);
        report(totals, mount, "merged_and_flushed_values_read_back", merged_and_flushed_values_read_back);
        report(totals, mount, "conflict_spares_the_other", conflict_leaves_the_other_collection_untouched);
        report(totals, mount, "rollback_and_renew", rollback_and_renew_behave);
        report(totals, mount, "snapshot_holds_its_moment", snapshot_holds_its_moment);
        report(totals, mount, "runs_scan_one_at_a_time", runs_scan_one_at_a_time);
        report(totals, mount, "drop_proceeds_beside_the_snapshot", drop_proceeds_beside_the_snapshot);
        report(totals, mount, "threads_commit_serializably", threads_commit_serializably);
        report(totals, mount, "head_writes_wait_for_records", head_writes_wait_for_records);
        report(totals, mount, "strings_shares_wait_for_records", strings_shares_wait_for_records);
        report(totals, mount, "readers_beside_committers_and_flushes", readers_beside_committers_and_flushes);
        report(totals, mount, "lookups_outrun_the_write_path", lookups_outrun_the_write_path);
        report(totals, mount, "feed_reads_across_flushes_and_merges", feed_reads_across_flushes_and_merges);
        report(totals, mount, "freed_feed_lets_merges_fold", freed_feed_lets_merges_fold);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, database_config_k, 1, "database", cases);

    return suite_exit_code(totals);
}
