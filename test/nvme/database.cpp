/**
 *  @file test/nvme/database.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The database over the collections: what the catalog persists, and what a commit makes visible.
 *
 *  The results this file exists for are @c test_commit_is_all_or_nothing and @c
 *  test_threads_commit_serializably. A transaction spanning two collections must never be seen half
 *  applied by a reader running beside it, and several threads racing over the same keys must leave
 *  a state one serial order explains.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstdio>  // `std::fopen`, `std::fclose`
#include <cstring> // `std::memcmp`, `std::memcpy`

#include <array>      // `std::array`
#include <chrono>     // `std::chrono::steady_clock`
#include <filesystem> // `std::filesystem::directory_iterator`, `std::filesystem::remove`, `std::filesystem::resize_file`
#include <limits>     // `std::numeric_limits`
#include <memory>     // `std::unique_ptr`
#include <new>        // `std::nothrow`
#include <optional>   // `std::optional`
#include <span>       // `std::span`
#include <string>     // `std::string`
#include <string_view> // `std::string_view`
#include <thread>      // `std::thread`
#include <type_traits> // `std::is_same_v`
#include <utility>     // `std::move`

#include <forkunion/capabilities.hpp>  // `fu::preferred_yield_t`
#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_database.hpp"     // `basic_database`
#include "ustore/strings/nvme_store.hpp"      // `strings::nvme_store`, `strings::nvme_transaction`
#include "ustore/utilities/nvme_manifest.hpp" // `manifest_t`

#include "nvme/allocation_watch.hpp" // `allocation_watch_g`
#include "nvme/fixture.hpp"          // `run_on_transport`, `painted`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;

#pragma region Shape

/** The numbers every case mounts with: a thread for the case and one for every thread it
 *  spawns. */
constexpr fixture_config_t database_config_k {.queues_threads = 9};

/** The largest value the variable-width collections under test admit. */
constexpr stored_value_size_t value_max_size_k = 4096;

/** The keys every case of this suite drives, and the two ways a collection is named. */
using stored_key_t = std::uint64_t;
using any_collection_id_t = std::variant<collection_id_t, collection_alphanumeric_id_t>;

/** The database and its transactions every case drives over the volume of @p fixture_type_. */
template <typename fixture_type_>
struct database_types {
    using fixture_t = fixture_type_;
    using volume_t = typename fixture_t::volume_t;
    template <value_layout_t value_layout_>
    using blobs_store_of = nvme_store<key_slot<stored_key_t, value_layout_>, volume_t, atomic_reference, waiter_t,
                                      util::last_write_fold_t, no_device_fold_t>;
    using strings_kit_t = util::strings_ladder<sz_capabilities_compiled_cpu_mask()>::strings_kit_t;
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

/** The collection name @p text, owned, as a lookup by name takes it; a suite's literals always fit. */
util::string_t owned_name(std::string_view text) noexcept {
    util::string_t owned;
    [[maybe_unused]] sz::status_t const assigned = owned.assign(util::string_view_t {text});
    return owned;
}

/** Creates the collection @p name in @p store on @p queues under an ID minted for it, answering
 *  the ID or why it was refused. */
template <typename store_type_>
util::expected<collection_id_t> create_named(store_type_& store, util::nvme_queues_ref_t queues,
                                             util::string_view_t name, collection_config_t const& config) noexcept {
    collection_id_t collection_id;
    std::memcpy(collection_id.uuid.bytes, util::uuid_generator().bytes, sizeof(collection_id.uuid.bytes));
    value_layout_t const layout = resolve_value_layout(config.lsm_tree.lsm_level_0.sorted_run.value_size);
    if (util::status_t const created = store.create_collection(collection_id, name, static_cast<std::uint16_t>(layout),
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
[[nodiscard]] std::array<util::byte_t, 8> counted(std::uint64_t value) noexcept {
    std::array<util::byte_t, 8> bytes {};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

/** The number @p bytes holds, which is zero for a span of another width. */
[[nodiscard]] std::uint64_t counter_of(std::span<util::byte_t const> bytes) noexcept {
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

/** Writes @p key into all three collections of @p store on @p queues, painted from @p round. */
template <typename database_type_>
void write_painted(database_type_& store, util::nvme_queues_ref_t queues, painted_collections_t const& named,
                   std::uint64_t key, std::uint64_t round) {
    std::array<util::byte_t, 8> const counter = counted(key * 7 + round);
    expect_true(store
                    .insert(any_collection_id_t {named.numbers}, static_cast<stored_key_t>(key),
                            stored_value_view_t {counter}, queues)
                    .succeeded());

    std::array<util::byte_t, 512> value {};
    std::size_t const width = blob_bytes_of(key);
    for (std::size_t position = 0; position != width; ++position)
        value[position] = painted(key + round * 1000, position);
    expect_true(store
                    .insert(any_collection_id_t {named.blobs}, static_cast<stored_key_t>(key),
                            stored_value_view_t {value.data(), width}, queues)
                    .succeeded());

    for (std::size_t position = 0; position != value.size(); ++position)
        value[position] = painted(key + round * 1000 + 99, position);
    expect_true(store
                    .insert(any_collection_id_t {named.fixed}, static_cast<stored_key_t>(key),
                            stored_value_view_t {value}, queues)
                    .succeeded());
}

/** Reads @p key back out of all three collections on @p queues and checks every byte against
 *  @p round. */
template <typename database_type_>
void check_painted(database_type_& store, util::nvme_queues_ref_t queues, painted_collections_t const& named,
                   std::uint64_t key, std::uint64_t round) {
    std::array<util::byte_t, 1024> into {};
    stored_value_size_t size = 0;
    expect_true(store
                    .select(any_collection_id_t {named.numbers}, static_cast<stored_key_t>(key),
                            std::span<util::byte_t> {into}, size, head_stamp_k, queues)
                    .succeeded());
    expect_equal(counter_of(std::span<util::byte_t const> {into.data(), size}), key * 7 + round);

    std::size_t mismatches = 0;
    expect_true(store
                    .select(any_collection_id_t {named.blobs}, static_cast<stored_key_t>(key),
                            std::span<util::byte_t> {into}, size, head_stamp_k, queues)
                    .succeeded());
    expect_equal(std::size_t {size}, blob_bytes_of(key));
    for (std::uint64_t position = 0; position != size; ++position)
        if (into[static_cast<std::size_t>(position)] != painted(key + round * 1000, position)) ++mismatches;

    expect_true(store
                    .select(any_collection_id_t {named.fixed}, static_cast<stored_key_t>(key),
                            std::span<util::byte_t> {into}, size, head_stamp_k, queues)
                    .succeeded());
    expect_equal(std::size_t {size}, std::size_t {512});
    for (std::uint64_t position = 0; position != size; ++position)
        if (into[static_cast<std::size_t>(position)] != painted(key + round * 1000 + 99, position)) ++mismatches;
    expect_equal(mismatches, std::size_t {0});
}

#pragma endregion Shape

#pragma region Fixtures

/** Mounts the volume of @p stack again, as a process starting after a close or a crash does. */
template <typename stack_type_>
util::status_t remount_volume(stack_type_& stack) noexcept {
    stack.volume = {};
    auto [guard, acquire_status] = stack.fixture->channel(0);
    if (acquire_status.failed()) return acquire_status;
    auto [volume, mount_status] = stack.fixture->mount_volume(*guard);
    if (mount_status.failed()) return mount_status;
    stack.volume = std::move(volume);
    return {};
}

/** Mounts the volume of @p stack again and a database over it and the channels of @p stack. */
template <typename stack_type_>
util::expected<std::unique_ptr<typename stack_type_::database_t>> open_database(stack_type_& stack) noexcept {
    using database_t = typename stack_type_::database_t;
    if (util::status_t const remounted = remount_volume(stack); remounted.failed()) return remounted;
    auto store = std::unique_ptr<database_t> {new (std::nothrow) database_t {*stack.fixture->resources}};
    if (!store) return util::status_t {util::status_code_t::out_of_memory_k, "The database did not fit"};
    if (util::status_t const mounted = store->mount(stack.volume, util::nvme_queues_ref_t {&stack.fixture->queues, 0},
                                                    test_database_config());
        mounted.failed())
        return mounted;
    return store;
}

/** Mounts a fixture through @p mount and opens one database over its volume. */
template <typename mount_type_>
util::expected<std::unique_ptr<typename database_types<typename mount_type_::fixture_t>::stack_t>> make_stack(
    mount_type_ const& mount) noexcept {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    auto stack = std::unique_ptr<stack_t> {new (std::nothrow) stack_t {}};
    if (!stack) return util::status_t {util::status_code_t::out_of_memory_k, "The stack did not fit"};

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

/** Two queue sets of eight threads read one database at once, every thread on its own ring: each
 *  read's sink waits until all sixteen threads are inside a read, which threads sharing a few
 *  channels would never reach, and then until a ninth thread of each queue set wrote the user data,
 *  which a write waiting on a channel some reader holds would never do. */
template <typename mount_type_>
void test_queues_read_without_waiting(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t queues_threads_k = 8, readers_k = 2 * queues_threads_k, keys_k = 64;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const fixture_queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;
    auto const [blobs, create_status] = create_named(store, fixture_queues, "blobs", test_config(0, value_max_size_k));
    if (!expect_true(create_status.succeeded())) return;
    std::array<util::byte_t, 64> value {};
    for (std::size_t index = 0; index != keys_k; ++index)
        expect_true(store
                        .insert(any_collection_id_t {blobs}, static_cast<stored_key_t>(index),
                                stored_value_view_t {value}, fixture_queues)
                        .succeeded());
    expect_true(store.flush(fixture_queues).succeeded());

    util::io_transport_t const transport = queues_transport_of<typename mount_type_::fixture_t::disk_t>();
    std::array<util::nvme_queues_t, 2> queue_sets;
    std::array<fu_memory_domain_id_t, queues_threads_k + 1> anywhere;
    anywhere.fill(-1);
    for (util::nvme_queues_t& queue_set : queue_sets) {
        auto [made, make_status] = util::nvme_queues_t::make(transport, mount.config.channels(), anywhere);
        if (!expect_true(make_status.succeeded())) return;
        queue_set = std::move(made);
    }

    std::size_t inside = 0;
    bool written = false;
    std::size_t timed_out = 0;
    st::vector<std::thread> readers;
    for (std::size_t reader = 0; reader != readers_k; ++reader)
        expect_true(readers.push_back(std::thread {[&, reader]() noexcept {
            util::nvme_queues_ref_t const queues {&queue_sets[reader / queues_threads_k], reader % queues_threads_k};
            std::array<stored_key_t, 4> keys {};
            for (std::size_t index = 0; index != keys.size(); ++index)
                keys[index] = static_cast<stored_key_t>((reader * keys.size() + index) % keys_k);
            std::array<lookup_result_t, 4> answers {};
            bool arrived = false;
            auto sink = [&](std::size_t, std::uint64_t, std::span<util::byte_t const>) noexcept {
                if (arrived) return;
                arrived = true;
                atomic_reference<std::size_t> {inside}.fetch_add(1, std::memory_order_acq_rel);
                auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                while (!atomic_reference<bool> {written}.load(std::memory_order_acquire))
                    if (std::chrono::steady_clock::now() > deadline) {
                        atomic_reference<std::size_t> {timed_out}.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    else std::this_thread::yield();
            };
            expect_true(store
                            .select(any_collection_id_t {blobs}, std::span<stored_key_t const> {keys},
                                    std::span<lookup_result_t> {answers}, sink, head_stamp_k, queues)
                            .succeeded());
        }}) == st::status_t::success_k);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (atomic_reference<std::size_t> {inside}.load(std::memory_order_acquire) != readers_k &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    for (util::nvme_queues_t& queue_set : queue_sets) {
        util::nvme_queues_ref_t const writer {&queue_set, queues_threads_k};
        expect_true(store.save_user_data(bytes_view_t {value}, writer).succeeded());
    }
    atomic_reference<bool> {written}.store(true, std::memory_order_release);
    for (std::size_t index = 0; index != readers.size(); ++index) readers[index].join();
    expect_equal(timed_out, std::size_t {0});
    expect_equal(inside, readers_k);
}

/** Several named collections of different layouts take writes and answer them back. */
template <typename mount_type_>
void test_named_collections_round_trip(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;

    auto const [numbers, numbers_status] = create_named(store, queues, "numbers", test_config(8, 0));
    auto const [blobs, blobs_status] = create_named(store, queues, "blobs", test_config(0, value_max_size_k));
    auto const [fixed, fixed_status] = create_named(store, queues, "fixed", test_config(512, 0));
    expect_true(numbers_status.succeeded());
    expect_true(blobs_status.succeeded());
    if (!expect_true(fixed_status.succeeded())) return;

    // A name is taken once, and a collection nobody created is named by nothing.
    expect_status(create_named(store, queues, "numbers", test_config(8, 0)), util::status_code_t::already_exists_k);
    expect_true(store.has_collection(any_collection_id_t {owned_name("blobs")}));
    expect_true(!store.has_collection(any_collection_id_t {owned_name("missing")}));
    expect_equal(store.collection_id(any_collection_id_t {owned_name("numbers")}), numbers);
    expect_equal(store.list_collections().size(), std::size_t {3});
    expect_equal(store.size(), std::size_t {3});

    constexpr std::size_t keys_k = 200;
    st::vector<util::byte_t> value;
    expect_true(value.resize(value_max_size_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != keys_k; ++index) {
        std::array<util::byte_t, 8> const counter = counted(index * 7 + 1);
        expect_true(store
                        .insert(any_collection_id_t {numbers}, static_cast<stored_key_t>(index),
                                stored_value_view_t {counter}, queues)
                        .succeeded());

        std::size_t const width = 1 + index % 1000;
        for (std::size_t position = 0; position != width; ++position) value[position] = painted(index, position);
        expect_true(store
                        .insert(any_collection_id_t {blobs}, static_cast<stored_key_t>(index),
                                stored_value_view_t {value.data(), width}, queues)
                        .succeeded());

        for (std::size_t position = 0; position != 512; ++position) value[position] = painted(index + 99, position);
        expect_true(store
                        .insert(any_collection_id_t {fixed}, static_cast<stored_key_t>(index),
                                stored_value_view_t {value.data(), 512}, queues)
                        .succeeded());
    }
    expect_true(store.flush(queues).succeeded());

    // A value of the wrong width is refused by the layout rather than stored short.
    expect_status(
        store.insert(any_collection_id_t {fixed}, stored_key_t {9999}, stored_value_view_t {value.data(), 511}, queues),
        util::status_code_t::malformed_k);

    st::vector<util::byte_t> into;
    expect_true(into.resize(value_max_size_k) == st::status_t::success_k);
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index != keys_k; ++index) {
        stored_value_size_t size = 0;
        expect_true(store
                        .select(any_collection_id_t {numbers}, static_cast<stored_key_t>(index),
                                std::span<util::byte_t> {into}, size, head_stamp_k, queues)
                        .succeeded());
        if (counter_of(std::span<util::byte_t const> {into.data(), size}) != index * 7 + 1) ++mismatches;

        expect_true(store
                        .select(any_collection_id_t {blobs}, static_cast<stored_key_t>(index),
                                std::span<util::byte_t> {into}, size, head_stamp_k, queues)
                        .succeeded());
        if (size != 1 + index % 1000) ++mismatches;
        for (std::uint64_t position = 0; position != size; ++position)
            if (into[static_cast<std::size_t>(position)] != painted(index, position)) ++mismatches;

        expect_true(store
                        .select(any_collection_id_t {fixed}, static_cast<stored_key_t>(index),
                                std::span<util::byte_t> {into}, size, head_stamp_k, queues)
                        .succeeded());
        if (size != 512) ++mismatches;
    }
    expect_equal(mismatches, std::size_t {0});

    // One batched select answers every key of a collection at once, misses included.
    std::array<stored_key_t, 3> const batch {stored_key_t {0}, stored_key_t {keys_k - 1}, stored_key_t {keys_k + 5}};
    std::array<lookup_result_t, 3> answers {lookup_result_t::missing_k, lookup_result_t::missing_k,
                                            lookup_result_t::missing_k};
    std::size_t held = 0;
    auto sink = [&](std::size_t, std::uint64_t, std::span<util::byte_t const> bytes) noexcept { held += bytes.size(); };
    expect_true(store
                    .select(any_collection_id_t {blobs}, std::span<stored_key_t const> {batch},
                            std::span<lookup_result_t> {answers}, sink, head_stamp_k, queues)
                    .succeeded());
    expect_true(answers[0] == lookup_result_t::written_k);
    expect_true(answers[1] == lookup_result_t::written_k);
    expect_true(answers[2] == lookup_result_t::missing_k);
    expect_equal(held, std::size_t {1} + (1 + (keys_k - 1) % 1000));

    // A removal hides the key in the collection it names and leaves the others alone.
    expect_true(store.remove(any_collection_id_t {numbers}, stored_key_t {0}, queues).succeeded());
    expect_true(!store.contains(any_collection_id_t {numbers}, stored_key_t {0}));
    expect_true(store.contains(any_collection_id_t {blobs}, stored_key_t {0}));
}

/**
 *  @brief The catalog and every byte the collections held survive a close and a reopen.
 *
 *  Three layouts are written twice, merged into a second level and written over once more, so a reopen
 *  that took back only the newest runs, or only level zero, would answer the wrong bytes for the keys
 *  the merge buried.
 */
template <typename mount_type_>
case_result_t test_catalog_survives_reopen(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    constexpr std::uint64_t keys_k = 300;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return {};
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};

    collection_config_t numbers_config = test_config(8, 0);
    collection_config_t blobs_config = test_config(0, value_max_size_k);
    collection_config_t fixed_config = test_config(512, 0);
    // The smallest factor makes level zero due at two flushed runs, so two rounds give the tree depth.
    numbers_config.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;
    blobs_config.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;
    fixed_config.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;

    auto const [numbers, numbers_status] = create_named(*stack->store, queues, "numbers", numbers_config);
    auto const [blobs, blobs_status] = create_named(*stack->store, queues, "blobs", blobs_config);
    auto const [fixed, fixed_status] = create_named(*stack->store, queues, "fixed", fixed_config);
    expect_true(numbers_status.succeeded());
    expect_true(blobs_status.succeeded());
    if (!expect_true(fixed_status.succeeded())) return {};
    painted_collections_t const named {numbers, blobs, fixed};

    // Two flushed rounds are merged into a second level, and a third round lands on level zero over them.
    for (std::uint64_t round = 0; round != 2; ++round) {
        for (std::uint64_t key = 0; key != keys_k; ++key) write_painted(*stack->store, queues, named, key, round);
        expect_true(stack->store->flush(queues).succeeded());
    }
    expect_true(stack->store->compact({}, queues).succeeded());

    // A merge publishes its new set before it drops what it consumed, so the manifest names every
    // run file left.
    expect_equal(run_count_of(*stack->fixture), run_files_of(*stack->fixture));
    for (std::uint64_t key = 0; key < keys_k; key += 3) write_painted(*stack->store, queues, named, key, 2);
    if (!expect_true(stack->store->flush(queues).succeeded())) return {};

    // The runs and their bytes are what a reopen must take back, level for level.
    std::size_t const runs_before = run_count_of(*stack->fixture);
    std::uint64_t const bytes_before = stack->store->size_bytes();
    expect_true(runs_before != std::size_t {0});

    // A key of every third is answered from level zero, and every other key from the level below it.
    auto round_of = [](std::uint64_t key) noexcept -> std::uint64_t { return key % 3 == 0 ? 2 : 1; };
    for (std::uint64_t key = 0; key != keys_k; ++key) check_painted(*stack->store, queues, named, key, round_of(key));

    expect_true(stack->store->unmount(queues).succeeded());
    stack->store.reset();

    auto [reopened, reopen_status] = open_database(*stack);
    expect_true(reopen_status.succeeded());
    if (reopen_status.failed()) return {};
    database_t& store = *reopened;

    expect_equal(store.size(), std::size_t {3});
    expect_equal(store.collection_id(any_collection_id_t {owned_name("numbers")}), numbers);
    expect_equal(store.collection_id(any_collection_id_t {owned_name("blobs")}), blobs);
    expect_true(store.collection_name(blobs) == owned_name("blobs"));

    collection_config_t restored;
    expect_true(store.collection_config(blobs, restored).succeeded());
    expect_equal(restored.lsm_tree.lsm_level_0.sorted_run.value_max_size, value_max_size_k);
    expect_equal(restored.memory_runs_max_deltas, std::size_t {4096});
    expect_equal(store.buffer_size_for_any_stored_value(any_collection_id_t {numbers}), stored_value_size_t {12});
    std::uint16_t store_tag = 0;
    expect_true(store.collection_store_tag(blobs, store_tag).succeeded());
    expect_equal(store_tag, static_cast<std::uint16_t>(value_layout_t::variable_width_k));

    // Every run the close left behind is back, and the reads below tell their levels apart.
    expect_equal(run_count_of(*stack->fixture), runs_before);
    expect_equal(store.size_bytes(), bytes_before);
    for (std::uint64_t key = 0; key != keys_k; ++key) check_painted(store, queues, named, key, round_of(key));

    // A reopened collection takes writes and answers them, which is what makes the catalog useful.
    for (std::uint64_t key = 0; key != 4; ++key) write_painted(store, queues, named, key, 3);
    for (std::uint64_t key = 0; key != 4; ++key) check_painted(store, queues, named, key, 3);
    expect_true(store.unmount(queues).succeeded());
    reopened.reset();

    // A database of fewer stores refuses a catalog naming a store it lacks, and leaves it whole for one that has.
    using numbers_only_t = typename types_t::template basic_database_of<
        typename types_t::template blobs_store_of<value_layout_t::inline_12_bytes_k>>;
    {
        numbers_only_t narrow {*stack->fixture->resources};
        expect_true(remount_volume(*stack).succeeded());
        expect_status(
            narrow.mount(stack->volume, util::nvme_queues_ref_t {&stack->fixture->queues, 0}, test_database_config()),
            util::status_code_t::malformed_k);
    }

    // A database listing its stores in another order mounts every collection in the store its tag names.
    using reordered_t = typename types_t::template basic_database_of<
        typename types_t::template blobs_store_of<value_layout_t::variable_width_k>,
        typename types_t::template blobs_store_of<value_layout_t::fixed_width_k>,
        typename types_t::template blobs_store_of<value_layout_t::inline_12_bytes_k>>;
    {
        reordered_t reordered {*stack->fixture->resources};
        expect_true(remount_volume(*stack).succeeded());
        expect_true(
            reordered.mount(stack->volume, util::nvme_queues_ref_t {&stack->fixture->queues, 0}, test_database_config())
                .succeeded());
        expect_equal(reordered.size(), std::size_t {3});
        expect_equal(reordered.buffer_size_for_any_stored_value(any_collection_id_t {numbers}),
                     stored_value_size_t {12});
        for (std::uint64_t key = 0; key != 4; ++key) check_painted(reordered, queues, named, key, 3);
        expect_true(reordered.unmount(queues).succeeded());
    }
    auto [again, again_status] = open_database(*stack);
    expect_true(again_status.succeeded());
    if (again_status.failed()) return {};
    expect_equal(again->size(), std::size_t {3});
    for (std::uint64_t key = 0; key != 4; ++key) check_painted(*again, queues, named, key, 3);
    return {};
}

/** A range a transaction removes hides the keys its runs hold through a flush, a reopen and a
 *  merge, a write inside it before the removal drops with it, and one after the removal lands. */
template <typename mount_type_>
case_result_t test_range_removal_survives_reopen(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    constexpr std::uint64_t keys_k = 200;
    constexpr stored_key_t start_k = 50;
    constexpr stored_key_t end_k = 160;
    constexpr stored_key_t dropped_k = 150;
    constexpr stored_key_t kept_k = 120;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return {};
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};

    collection_config_t config = test_config(8, 0);
    config.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;
    auto const [ranged, create_status] = create_named(*stack->store, queues, "ranged", config);
    expect_true(create_status.succeeded());
    if (create_status.failed()) return {};
    any_collection_id_t const id {ranged};
    for (std::uint64_t round = 0; round != 2; ++round) {
        for (std::uint64_t key = 0; key != keys_k; ++key) {
            std::array<util::byte_t, 8> const counter = counted(key + round);
            expect_true(stack->store->insert(id, static_cast<stored_key_t>(key), stored_value_view_t {counter}, queues)
                            .succeeded());
        }
        expect_true(stack->store->flush(queues).succeeded());
    }
    expect_true(stack->store->compact({}, queues).succeeded());

    auto [transaction, begin_status] = stack->store->create_transaction();
    expect_true(begin_status.succeeded());
    if (begin_status.failed()) return {};
    std::array<util::byte_t, 8> const before = counted(7000);
    std::array<util::byte_t, 8> const after = counted(9000);
    expect_true(transaction->insert(id, dropped_k, stored_value_view_t {before}).succeeded());
    expect_true(transaction->remove_range(id, start_k, end_k).succeeded());
    expect_true(transaction->insert(id, kept_k, stored_value_view_t {after}).succeeded());
    expect_true(transaction->lookup_uncommitted(ranged, dropped_k) == lookup_result_t::removed_k);
    expect_true(transaction->lookup_uncommitted(ranged, start_k) == lookup_result_t::removed_k);
    expect_true(transaction->lookup_uncommitted(ranged, end_k) == lookup_result_t::missing_k);
    expect_true(transaction->commit(queues).succeeded());

    auto const check = [&](database_t& store) {
        std::array<util::byte_t, 8> into {};
        for (std::uint64_t key = 0; key != keys_k; ++key) {
            stored_value_size_t size = 0;
            util::status_t const read = store.select(id, static_cast<stored_key_t>(key), std::span<util::byte_t> {into},
                                                     size, head_stamp_k, queues);
            if (key == kept_k) {
                expect_true(read.succeeded());
                expect_equal(counter_of(std::span<util::byte_t const> {into.data(), size}), std::uint64_t {9000});
            }
            else if (key >= start_k && key < end_k) expect_true(read == util::status_code_t::not_found_k);
            else {
                expect_true(read.succeeded());
                expect_equal(counter_of(std::span<util::byte_t const> {into.data(), size}), key + 1);
            }
        }
    };
    check(*stack->store);
    expect_true(stack->store->flush(queues).succeeded());
    check(*stack->store);
    transaction.reset();
    expect_true(stack->store->unmount(queues).succeeded());
    stack->store.reset();

    auto [reopened, reopen_status] = open_database(*stack);
    expect_true(reopen_status.succeeded());
    if (reopen_status.failed()) return {};
    check(*reopened);
    expect_true(reopened->flush(queues).succeeded());
    expect_true(reopened->compact({}, queues).succeeded());
    check(*reopened);
    return {};
}

/** A strings collection, as the engine's names system collection is one, survives a reopen: strings sharing their
 *  15-byte heads map onto the same 16-byte keys, answered from the 32-byte cells of a flushed run, whose header records
 *  the width of its cells and holds no values section. */
template <typename mount_type_>
case_result_t test_strings_survive_reopen(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    using strings_store_t = typename types_t::strings_store_t;
    using key_slot_t = typename strings_store_t::key_slot_t;
    constexpr std::size_t strings_k = 64;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return {};
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};

    collection_id_t names;
    std::memcpy(names.uuid.bytes, util::uuid_generator().bytes, sizeof(names.uuid.bytes));
    if (!expect_true(stack->store
                         ->create_collection(names, "names", static_cast<std::uint16_t>(value_layout_t::uuid_k),
                                             test_config(sizeof(ustore_uuid_t), 0), collection_invalid_id_k, queues)
                         .succeeded()))
        return {};

    std::array<std::string, strings_k> texts;
    std::array<std::array<util::byte_t, key_slot_t::size>, strings_k> keys;
    for (std::size_t index = 0; index != strings_k; ++index) {
        texts[index] = "collection-name" + std::to_string(index);
        for (std::size_t position = 0; position != key_slot_t::size; ++position)
            keys[index][position] = painted(index, position);
    }
    {
        strings::nvme_transaction<database_t, strings_store_t> written {*stack->store};
        auto [held, begin_status] = stack->store->create_transaction(test_database_config().default_transaction);
        if (!expect_true(begin_status.succeeded())) return {};
        written.join(*held);
        for (std::size_t index = 0; index != strings_k; ++index)
            expect_true(written
                            .write(names, texts[index], util::delta_view_t {util::delta_kind_t::write_k, keys[index]},
                                   invalid_transaction_stamp_k)
                            .succeeded());
        expect_true(written.commit(*held, queues).succeeded());
    }
    expect_true(stack->store->flush(queues).succeeded());
    expect_true(stack->store->unmount(queues).succeeded());
    stack->store.reset();

    auto [reopened, reopen_status] = open_database(*stack);
    expect_true(reopen_status.succeeded());
    if (reopen_status.failed()) return {};
    expect_equal(reopened->collection_id(any_collection_id_t {owned_name("names")}), names);
    util::status_t const visited = reopened->visit_store(
        any_collection_id_t {names}, [&]<typename store_type_>(store_type_& store) noexcept -> util::status_t {
            if constexpr (!std::is_same_v<store_type_, strings_store_t>)
                return {util::status_code_t::invalid_argument_k, "A strings collection mounted in another store"};
            else {
                typename strings_store_t::tree_t::level_t const& level = store.tree().levels().level(0);
                expect_equal(level.size(), std::size_t {1});
                if (level.size() == 1) {
                    expect_equal(level.run(0).header().key_slot_bytes, std::uint32_t {32});
                    expect_true(level.run(0).header().value_layout == value_layout_t::uuid_k);
                    expect_equal(level.run(0).header().stored_values_bytes, std::uint64_t {0});
                }
                for (std::size_t index = 0; index != strings_k; ++index) {
                    std::array<util::byte_t, key_slot_t::size> value {};
                    expect_true(store.find(texts[index], value) == lookup_result_t::written_k);
                    expect_true(value == keys[index]);
                }
                std::array<util::byte_t, key_slot_t::size> missing {};
                expect_true(store.find("collection-name", missing) == lookup_result_t::missing_k);
                return {};
            }
        });
    expect_true(visited.succeeded());
    return {};
}

/** A range of strings a transaction removes, and one reaching past the last string, hide the
 *  strings flushed below them through a flush, a merge and a reopen; a write before the removal
 *  drops with it and one after it lands. */
template <typename mount_type_>
case_result_t test_strings_range_removal_survives_reopen(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    using strings_store_t = typename types_t::strings_store_t;
    using key_slot_t = typename strings_store_t::key_slot_t;
    using value_t = std::array<util::byte_t, key_slot_t::size>;
    constexpr std::size_t strings_k = 100;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return {};
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};

    collection_id_t words;
    std::memcpy(words.uuid.bytes, util::uuid_generator().bytes, sizeof(words.uuid.bytes));
    collection_config_t config = test_config(sizeof(ustore_uuid_t), 0);
    config.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;
    if (!expect_true(stack->store
                         ->create_collection(words, "words", static_cast<std::uint16_t>(value_layout_t::uuid_k), config,
                                             collection_invalid_id_k, queues)
                         .succeeded()))
        return {};
    auto const text_of = [](std::size_t index) { return fmt::format("item-{:03}", index); };
    auto const value_of = [](std::size_t seed) {
        value_t value {};
        for (std::size_t position = 0; position != value.size(); ++position) value[position] = painted(seed, position);
        return value;
    };
    auto const commit = [&](database_t& store, auto&& writes) {
        strings::nvme_transaction<database_t, strings_store_t> written {store};
        auto [held, begin_status] = store.create_transaction(test_database_config().default_transaction);
        if (!expect_true(begin_status.succeeded())) return;
        written.join(*held);
        writes(written);
        expect_true(written.commit(*held, queues).succeeded());
    };
    for (std::size_t round = 0; round != 2; ++round) {
        commit(*stack->store, [&](auto& written) {
            for (std::size_t index = 0; index != strings_k; ++index) {
                std::string const text = text_of(index);
                value_t const value = value_of(index);
                expect_true(written
                                .write(words, text, util::delta_view_t {util::delta_kind_t::write_k, value},
                                       invalid_transaction_stamp_k)
                                .succeeded());
            }
        });
        expect_true(stack->store->flush(queues).succeeded());
    }
    expect_true(stack->store->compact({}, queues).succeeded());

    std::string const start = text_of(40), end = text_of(60), tail = text_of(90);
    std::string const dropped = text_of(50), kept = text_of(45);
    value_t const before = value_of(7000), after = value_of(9000);
    commit(*stack->store, [&](auto& written) {
        expect_true(written
                        .write(words, dropped, util::delta_view_t {util::delta_kind_t::write_k, before},
                               invalid_transaction_stamp_k)
                        .succeeded());
        expect_true(written.remove_range(words, start, end).succeeded());
        expect_true(written.remove_range(words, tail, {}).succeeded());
        expect_true(written
                        .write(words, kept, util::delta_view_t {util::delta_kind_t::write_k, after},
                               invalid_transaction_stamp_k)
                        .succeeded());
        util::delta_view_t own;
        expect_true(written.uncommitted(words, dropped, own) && own.kind == util::delta_kind_t::remove_k);
        expect_true(written.uncommitted(words, text_of(99), own) && own.kind == util::delta_kind_t::remove_k);
        expect_true(!written.uncommitted(words, text_of(60), own));
    });

    auto const check = [&](database_t& store) {
        expect_true(store
                        .visit_store(any_collection_id_t {words},
                                     [&]<typename store_type_>(store_type_& held) noexcept -> util::status_t {
                                         if constexpr (!std::is_same_v<store_type_, strings_store_t>)
                                             return {util::status_code_t::invalid_argument_k, "Another store"};
                                         else {
                                             for (std::size_t index = 0; index != strings_k; ++index) {
                                                 value_t value {};
                                                 lookup_result_t const found = held.find(text_of(index), value);
                                                 bool const hidden = (index >= 40 && index < 60) || index >= 90;
                                                 if (index == 45) expect_true(value == after);
                                                 else if (hidden) expect_true(found != lookup_result_t::written_k);
                                                 else expect_true(value == value_of(index));
                                             }
                                             // A walk down starts at the last string spared.
                                             util::local_arena_t walk;
                                             typename store_type_::page_t page {walk.allocator().arena};
                                             expect_true(
                                                 held.read_page_down({}, {}, head_stamp_k, 0, page).succeeded());
                                             std::string_view previous;
                                             std::size_t written = 0;
                                             for (std::size_t index = 0; index != page.size(); ++index) {
                                                 std::string_view const text = page.text(index);
                                                 if (index != 0) expect_true(text < previous);
                                                 previous = text;
                                                 if (page.listed[index].state != lookup_result_t::written_k) continue;
                                                 if (written++ == 0) expect_true(text == text_of(89));
                                                 expect_true(!(text >= start && text < end) || text == kept);
                                             }
                                             expect_true(written != 0);
                                             return {};
                                         }
                                     })
                        .succeeded());
    };
    check(*stack->store);
    expect_true(stack->store->flush(queues).succeeded());
    check(*stack->store);
    expect_true(stack->store->unmount(queues).succeeded());
    stack->store.reset();

    auto [reopened, reopen_status] = open_database(*stack);
    expect_true(reopen_status.succeeded());
    if (reopen_status.failed()) return {};
    check(*reopened);
    commit(*reopened, [&](auto& written) {
        return expect_true(written
                               .write(words, kept, util::delta_view_t {util::delta_kind_t::write_k, after},
                                      invalid_transaction_stamp_k)
                               .succeeded());
    });
    expect_true(reopened->flush(queues).succeeded());
    expect_true(reopened->compact({}, queues).succeeded());
    check(*reopened);
    return {};
}

/** A dropped collection is gone after a reopen, and the bytes its runs held are free again. */
template <typename mount_type_>
case_result_t test_dropped_collection_is_gone(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return {};
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    auto const& fixture = *stack->fixture;

    auto const [kept, kept_status] = create_named(*stack->store, queues, "kept", test_config(8, 0));
    auto const [doomed, doomed_status] = create_named(*stack->store, queues, "doomed",
                                                      test_config(0, value_max_size_k));
    expect_true(kept_status.succeeded());
    if (!expect_true(doomed_status.succeeded())) return {};

    st::vector<util::byte_t> value;
    expect_true(value.resize(value_max_size_k) == st::status_t::success_k);
    for (std::size_t index = 0; index != 400; ++index) {
        for (std::size_t position = 0; position != value.size(); ++position) value[position] = painted(index, position);
        expect_true(stack->store
                        ->insert(any_collection_id_t {doomed}, static_cast<stored_key_t>(index),
                                 stored_value_view_t {value}, queues)
                        .succeeded());
    }
    expect_true(stack->store->flush(queues).succeeded());
    expect_true(run_bytes_of(fixture) != std::uint64_t {0});

    // What a drop must reclaim are runs this database took back from its schema rather than wrote itself.
    expect_true(stack->store->unmount(queues).succeeded());
    stack->store.reset();
    auto [reopened, reopen_status] = open_database(*stack);
    expect_true(reopen_status.succeeded());
    if (reopen_status.failed()) return {};

    expect_true(run_bytes_of(fixture) != std::uint64_t {0});
    expect_true(reopened->drop_collection(any_collection_id_t {doomed}, collection_invalid_id_k, queues).succeeded());
    expect_true(!reopened->has_collection(any_collection_id_t {owned_name("doomed")}));
    expect_equal(run_bytes_of(fixture), std::uint64_t {0});
    expect_equal(run_files_of(fixture), std::size_t {0});
    expect_status(reopened->drop_collection(any_collection_id_t {doomed}, collection_invalid_id_k, queues),
                  util::status_code_t::not_found_k);

    // The name returns under a new UUID in the record the drop freed, which the old UUID misses.
    auto const [reborn, reborn_status] = create_named(*reopened, queues, "doomed", test_config(8, 0));
    expect_true(reborn_status.succeeded());
    if (reborn_status.failed()) return {};
    expect_true(!reopened->has_collection(any_collection_id_t {doomed}));
    expect_true(reopened->collection_name(doomed).empty());
    expect_true(reopened->collection_id(any_collection_id_t {owned_name("doomed")}) == reborn);
    expect_true(reopened->drop_collection(any_collection_id_t {reborn}, collection_invalid_id_k, queues).succeeded());

    expect_true(reopened->unmount(queues).succeeded());
    reopened.reset();
    auto [again, again_status] = open_database(*stack);
    expect_true(again_status.succeeded());
    if (again_status.failed()) return {};

    expect_equal(again->size(), std::size_t {1});
    expect_true(!again->has_collection(any_collection_id_t {owned_name("doomed")}));
    expect_true(again->has_collection(any_collection_id_t {owned_name("kept")}));

    // The bytes the dropped runs held are handed out again rather than lost to the catalog.
    auto const [fresh, fresh_status] = create_named(*again, queues, "fresh", test_config(0, value_max_size_k));
    expect_true(fresh_status.succeeded());
    if (fresh_status.failed()) return {};
    for (std::size_t index = 0; index != 400; ++index) {
        for (std::size_t position = 0; position != value.size(); ++position) value[position] = painted(index, position);
        expect_true(again
                        ->insert(any_collection_id_t {fresh}, static_cast<stored_key_t>(index),
                                 stored_value_view_t {value}, queues)
                        .succeeded());
    }
    expect_true(again->flush(queues).succeeded());
    return {};
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
case_result_t test_crash_between_seal_and_schema(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);

    {
        auto [stack, make_status] = make_stack(mount);
        expect_status(make_status, util::status_code_t::success_k);
        if (make_status.failed()) return {};
        util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
        auto const& fixture = *stack->fixture;

        auto const [lost, lost_status] = create_named(*stack->store, queues, "lost", test_config(8, 0));
        expect_true(lost_status.succeeded());
        if (lost_status.failed()) return {};
        for (std::uint64_t key = 0; key != 200; ++key) {
            std::array<util::byte_t, 8> const counter = counted(key);
            expect_true(stack->store
                            ->insert(any_collection_id_t {lost}, static_cast<stored_key_t>(key),
                                     stored_value_view_t {counter}, queues)
                            .succeeded());
        }
        expect_true(stack->store->flush(queues).succeeded());

        // A flush writes its runs and publishes the manifest second, so the manifest already
        // names every run.
        expect_equal(run_count_of(fixture), run_files_of(fixture));
        expect_true(stack->store->unmount(queues).succeeded());
        stack->store.reset();

        std::size_t const runs = run_count_of(fixture);
        expect_true(runs != std::size_t {0});
        expect_true(run_bytes_of(fixture) != std::uint64_t {0});

        // A mount keeps every run the schema names.
        {
            auto [kept, kept_status] = open_database(*stack);
            expect_true(kept_status.succeeded());
            if (kept_status.failed()) return {};
            expect_equal(run_count_of(fixture), runs);
            expect_true(kept->unmount(queues).succeeded());
        }

        // No manifest that would have named those runs ever landed.
        {
            std::array<char, 96> name {};
            auto const written = fmt::format_to_n(name.data(), name.size() - 1, "{}.ustore.json", fixture.suite);
            *written.out = '\0';
            std::string const manifest = fmt::format("{}/{}", fixture.data_directory_path(0).c_str(), name.data());
            expect_true(std::filesystem::remove(manifest.c_str()));
        }

        auto [reopened, reopen_status] = open_database(*stack);
        expect_true(reopen_status.succeeded());
        if (reopen_status.failed()) return {};
        expect_equal(reopened->size(), std::size_t {0});
        expect_true(!reopened->has_collection(any_collection_id_t {owned_name("lost")}));
        expect_equal(run_files_of(fixture), std::size_t {0});

        // The volume is still a volume: a fresh collection writes, flushes and answers over it.
        auto const [fresh, fresh_status] = create_named(*reopened, queues, "fresh", test_config(8, 0));
        expect_true(fresh_status.succeeded());
        if (fresh_status.failed()) return {};
        std::array<util::byte_t, 8> const counter = counted(77);
        expect_true(
            reopened->insert(any_collection_id_t {fresh}, stored_key_t {1}, stored_value_view_t {counter}, queues)
                .succeeded());
        expect_true(reopened->flush(queues).succeeded());
        std::array<util::byte_t, 8> into {};
        stored_value_size_t size = 0;
        expect_true(reopened
                        ->select(any_collection_id_t {fresh}, stored_key_t {1}, std::span<util::byte_t> {into}, size,
                                 head_stamp_k, queues)
                        .succeeded());
        expect_equal(counter_of(std::span<util::byte_t const> {into.data(), size}), std::uint64_t {77});
    }

    {
        auto [stack, make_status] = make_stack(mount);
        expect_status(make_status, util::status_code_t::success_k);
        if (make_status.failed()) return {};
        util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
        auto const& fixture = *stack->fixture;

        auto const [torn, torn_status] = create_named(*stack->store, queues, "torn", test_config(8, 0));
        expect_true(torn_status.succeeded());
        if (torn_status.failed()) return {};
        for (std::uint64_t key = 0; key != 200; ++key) {
            std::array<util::byte_t, 8> const counter = counted(key);
            expect_true(stack->store
                            ->insert(any_collection_id_t {torn}, static_cast<stored_key_t>(key),
                                     stored_value_view_t {counter}, queues)
                            .succeeded());
        }
        expect_true(stack->store->flush(queues).succeeded());
        expect_true(stack->store->unmount(queues).succeeded());
        stack->store.reset();

        util::manifest_t const manifest = manifest_on_disk(fixture);
        expect_true(!manifest.files.empty());
        if (manifest.files.empty()) return {};

        // The file of one run the manifest names never reached the environment.
        char name[util::path_max_bytes_k];
        expect_true(manifest.file_name(manifest.files[0].run, name).succeeded());
        std::string const run_path = fmt::format("{}/{}", fixture.data_directory_path(0).c_str(), name);
        expect_true(std::filesystem::remove(run_path.c_str()));

        auto [reopened, reopen_status] = open_database(*stack);
        expect_true(reopen_status.failed());
    }
    return {};
}

/** A collection is listed from the stamp its create drew and up to the one its unlisting drew,
 *  so a reader pinned before the create or past the unlisting finds no store tag, and one pinned
 *  between still does. */
template <typename mount_type_>
void test_collections_list_by_stamp(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;

    transaction_stamp_t const before = store.ledger().published();
    auto const [listed, create_status] = create_named(store, queues, "listed", test_config(8, 0));
    expect_true(create_status.succeeded());
    if (create_status.failed()) return;
    transaction_stamp_t const created = store.ledger().published();
    std::uint16_t tag = 0;
    expect_status(store.listed_store_tag(listed, before, tag), util::status_code_t::not_found_k);
    expect_true(store.listed_store_tag(listed, created, tag).succeeded());
    expect_true(store.listed_store_tag(listed, head_stamp_k, tag).succeeded());
    expect_equal(tag, static_cast<std::uint16_t>(value_layout_t::inline_12_bytes_k));

    expect_true(store.unlist_collection(listed).succeeded());
    expect_true(store.listed_store_tag(listed, created, tag).succeeded());
    expect_status(store.listed_store_tag(listed, head_stamp_k, tag), util::status_code_t::not_found_k);
    expect_status(store.listed_store_tag(listed, store.ledger().published(), tag), util::status_code_t::not_found_k);
}

/**
 *  @brief A crash between a flush and the catalog save loses a suffix of commits and never half of one.
 *
 *  A commit lands in two collections; the first flushes it on its own while the second still
 *  buffers it, and a catalog save then names the first's run. The medium as it stands then is what
 *  a crash leaves: a reopen of it drops the commit from the run that holds it, every commit after
 *  it too, and keeps every commit before it whole.
 */
template <typename mount_type_>
case_result_t test_crash_keeps_a_prefix_of_commits(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return {};
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;

    // The first collection flushes once its buffer holds its few writes, the second never does on its own.
    collection_config_t narrow = test_config(8, 0);
    narrow.memory_runs_max_deltas = 4;
    auto const [first, first_status] = create_named(store, queues, "first", narrow);
    auto const [second, second_status] = create_named(store, queues, "second", test_config(8, 0));
    if (!expect_true(first_status.succeeded() && second_status.succeeded())) return {};
    any_collection_id_t const left {first};
    any_collection_id_t const right {second};

    auto commit_both = [&](std::uint64_t value) noexcept {
        auto [transaction, begin_status] = store.create_transaction();
        if (!expect_true(begin_status.succeeded())) return;
        std::array<util::byte_t, 8> const counter = counted(value);
        expect_true(transaction->insert(left, stored_key_t {0}, stored_value_view_t {counter}).succeeded());
        expect_true(transaction->insert(right, stored_key_t {0}, stored_value_view_t {counter}).succeeded());
        expect_true(transaction->commit(queues).succeeded());
    };
    commit_both(1);
    expect_true(store.flush(queues).succeeded());
    commit_both(2);
    for (std::uint64_t key = 100; key != 104; ++key) {
        std::array<util::byte_t, 8> const counter = counted(key);
        expect_true(
            store.insert(left, static_cast<stored_key_t>(key), stored_value_view_t {counter}, queues).succeeded());
    }
    expect_true(create_named(store, queues, "saved", test_config(8, 0)).has_value());

    // Before the crash both collections answer the second commit, the first from a run, the second from its buffer.
    std::array<util::byte_t, 8> into {};
    stored_value_size_t size = 0;
    expect_true(
        store.select(left, stored_key_t {0}, std::span<util::byte_t> {into}, size, head_stamp_k, queues).succeeded());
    expect_equal(counter_of(std::span<util::byte_t const> {into.data(), size}), std::uint64_t {2});
    expect_true(capture_data_directory(*stack->fixture).succeeded());
    stack->store.reset();
    stack->volume = typename types_t::volume_t {};
    expect_true(restore_data_directory(*stack->fixture).succeeded());
    auto [reopened, reopen_status] = open_database(*stack);
    if (!expect_true(reopen_status.succeeded())) return {};

    // The first commit is whole, the second gone from both sides, and the writes after it gone with it.
    expect_true(reopened->has_collection(any_collection_id_t {owned_name("saved")}));
    expect_true(reopened->select(left, stored_key_t {0}, std::span<util::byte_t> {into}, size, head_stamp_k, queues)
                    .succeeded());
    expect_equal(counter_of(std::span<util::byte_t const> {into.data(), size}), std::uint64_t {1});
    expect_true(reopened->select(right, stored_key_t {0}, std::span<util::byte_t> {into}, size, head_stamp_k, queues)
                    .succeeded());
    expect_equal(counter_of(std::span<util::byte_t const> {into.data(), size}), std::uint64_t {1});
    for (std::uint64_t key = 100; key != 104; ++key) expect_true(!reopened->contains(left, key));

    // A commit after the reopen lands under a fresh stamp and reads back.
    std::array<util::byte_t, 8> const counter = counted(3);
    expect_true(reopened->insert(left, stored_key_t {0}, stored_value_view_t {counter}, queues).succeeded());
    expect_true(reopened->select(left, stored_key_t {0}, std::span<util::byte_t> {into}, size, head_stamp_k, queues)
                    .succeeded());
    expect_equal(counter_of(std::span<util::byte_t const> {into.data(), size}), std::uint64_t {3});
    return {};
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
case_result_t test_merges_keep_what_the_flush_promises(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return {};
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;

    // Two runs of tier zero merge, and the merged run with one more delta makes a flush due.
    collection_config_t merging = test_config(8, 0);
    merging.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;
    merging.memory_runs_max_deltas = 2;
    auto const [merged, merged_status] = create_named(store, queues, "merged", merging);
    auto const [beside, beside_status] = create_named(store, queues, "beside", test_config(8, 0));
    if (!expect_true(merged_status.succeeded() && beside_status.succeeded())) return {};
    any_collection_id_t const id {merged};

    auto write = [&](any_collection_id_t const& written_id, stored_key_t key, std::uint64_t value) noexcept {
        std::array<util::byte_t, 8> const counter = counted(value);
        return expect_true(store.insert(written_id, key, stored_value_view_t {counter}, queues).succeeded());
    };
    write(id, stored_key_t {0}, 1);
    write(any_collection_id_t {beside}, stored_key_t {0}, 2);
    write(id, stored_key_t {0}, 3);
    write(id, stored_key_t {1}, 4);

    std::array<util::byte_t, 8> into {};
    stored_value_size_t size = 0;
    expect_true(
        store.select(id, stored_key_t {0}, std::span<util::byte_t> {into}, size, head_stamp_k, queues).succeeded());
    expect_equal(counter_of(std::span<util::byte_t const> {into.data(), size}), std::uint64_t {3});
    expect_true(capture_data_directory(*stack->fixture).succeeded());
    stack->store.reset();
    stack->volume = typename types_t::volume_t {};
    expect_true(restore_data_directory(*stack->fixture).succeeded());
    auto [reopened, reopen_status] = open_database(*stack);
    if (!expect_true(reopen_status.succeeded())) return {};

    // The second collection's commit was never durable, so the mount resumes below it, at
    // the first write.
    expect_true(
        reopened->select(id, stored_key_t {0}, std::span<util::byte_t> {into}, size, head_stamp_k, queues).succeeded());
    expect_equal(counter_of(std::span<util::byte_t const> {into.data(), size}), std::uint64_t {1});
    expect_true(!reopened->contains(any_collection_id_t {beside}, stored_key_t {0}));
    return {};
}

/** The bytes a caller saves next to the database survive a reopen and a clearing. */
template <typename mount_type_>
case_result_t test_user_data_round_trips(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    if (mount.transport == transport_t::memory_k) return case_result_t::skipped(memory_volumes_vanish_k);
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return {};
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};

    st::vector<util::byte_t> saved;
    expect_true(saved.resize(3000) == st::status_t::success_k);
    for (std::size_t index = 0; index != saved.size(); ++index) saved[index] = painted(77, index);
    expect_true(stack->store->save_user_data(bytes_view_t {saved}, queues).succeeded());
    expect_equal(stack->store->user_data_size_bytes(), std::uint64_t {3000});

    expect_true(stack->store->unmount(queues).succeeded());
    stack->store.reset();
    auto [reopened, reopen_status] = open_database(*stack);
    expect_true(reopen_status.succeeded());
    if (reopen_status.failed()) return {};

    st::vector<util::byte_t> loaded;
    expect_true(loaded.resize(3000) == st::status_t::success_k);
    expect_true(reopened->load_user_data(bytes_span_t {loaded}, queues).succeeded());
    expect_equal(std::memcmp(loaded.data(), saved.data(), saved.size()), 0);

    expect_true(reopened->clear_user_data(queues).succeeded());
    expect_equal(reopened->user_data_size_bytes(), std::uint64_t {0});
    expect_true(reopened->load_user_data(bytes_span_t {loaded}, queues) == util::status_code_t::not_found_k);
    return {};
}

/** A database over either volume, which an archive saves from a directory onto raw
 *  media and back. */
template <typename fixture_type_>
struct archive_types {
    using volume_t = util::any_volume<typename fixture_type_::disk_t, waiter_t, volume_allocator_t>;
    using store_t = nvme_store<key_slot<stored_key_t, value_layout_t::variable_width_k>, volume_t, atomic_reference,
                               waiter_t, util::last_write_fold_t, no_device_fold_t>;
    using database_t = basic_database<stored_key_t, volume_t, atomic_reference, waiter_t, st::native_row_kit_t,
                                      layout_store_tags<store_t>, store_t>;
};

/** Steps the serial SHA-256 kernels, counting every block it hashes into @c blocks. */
struct counted_sha256_t {
    util::strings_kit_sha256<util::serial_strings_kit_t> kernels;
    std::size_t* blocks = nullptr;

    void init(sz_sha256_state_t& state) const noexcept { kernels.init(state); }

    void update(sz_sha256_state_t& state, std::span<util::byte_t const> bytes) const noexcept {
        ++*blocks;
        kernels.update(state, bytes);
    }

    [[nodiscard]] util::sha256_t digest(sz_sha256_state_t const& state) const noexcept { return kernels.digest(state); }
};

/** Mounts a volume over @p address and a database over it, which checks the runs with
 *  @p digester, answering both or why either was refused. */
template <typename types_type_, typename fixture_type_,
          typename digester_type_ = util::strings_kit_sha256<util::serial_strings_kit_t>>
util::status_t open_archive(fixture_type_& fixture, util::io_channel_t& channel, char const* database,
                            char const* address, typename types_type_::volume_t& volume,
                            std::unique_ptr<typename types_type_::database_t>& store,
                            digester_type_ const& digester = {}) noexcept {
    auto [parsed, parse_status] = util::parse_data_address(address);
    if (parse_status.failed()) return parse_status;
    util::data_address_t const addresses[] {parsed};
    util::volume_config_t config;
    config.database = util::string_view_t {database};
    config.data_directories = addresses;
    config.runs_capacity = fixture.config.runs_capacity;
    auto [mounted, mount_status] = types_type_::volume_t::mount(
        fixture.disk, channel, config, volume_allocator_t {fixture.resources->memory_constraint});
    if (mount_status.failed()) return mount_status;
    volume = std::move(mounted);
    store.reset(new (std::nothrow) typename types_type_::database_t {*fixture.resources});
    if (!store) return {util::status_code_t::out_of_memory_k, "The database did not fit"};
    return store->mount(volume, util::nvme_queues_ref_t {&fixture.queues, 0}, test_database_config(), {}, digester);
}

/** Saves the head of @p store on @p queues as the database @p database at @p address,
 *  answering what the save placed. */
template <typename database_type_>
util::expected<typename database_type_::archive_report_t> save_head(database_type_& store,
                                                                    util::nvme_queues_ref_t queues,
                                                                    char const* database,
                                                                    char const* address) noexcept {
    auto [parsed, parse_status] = util::parse_data_address(address);
    if (parse_status.failed()) return parse_status;
    util::data_address_t const addresses[] {parsed};
    util::volume_config_t target;
    target.database = util::string_view_t {database};
    target.data_directories = addresses;
    auto [pinned, pin_status] = store.pin_stamp();
    if (pin_status.failed()) return pin_status;
    return store.save(
        target, pinned.stamp(), [](std::uint64_t, std::uint64_t) noexcept { return true; },
        util::strings_kit_sha256<util::serial_strings_kit_t> {}, queues);
}

/** Counts the keys below @p count whose values in @p store, read on @p queues, differ from the
 *  painted ones of round @p round. */
template <typename database_type_>
[[nodiscard]] std::size_t differing_keys(database_type_& store, util::nvme_queues_ref_t queues,
                                         collection_id_t collection, std::uint64_t count) {
    std::size_t differing = 0;
    std::array<util::byte_t, 512> into {};
    for (std::uint64_t key = 0; key != count; ++key) {
        stored_value_size_t size = 0;
        util::status_t const read = store.select(any_collection_id_t {collection}, static_cast<stored_key_t>(key),
                                                 std::span<util::byte_t> {into}, size, head_stamp_k, queues);
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
case_result_t test_archive_saves_incrementally_both_ways(mount_type_ const& mount) {
    using types_t = archive_types<typename mount_type_::fixture_t>;
    using volume_t = typename types_t::volume_t;
    using database_t = typename types_t::database_t;
    if (mount.transport == transport_t::memory_k || mount.transport == transport_t::device_k)
        return case_result_t::skipped("needs a file standing in for a device");
    auto [fixture, mount_status] = mount();
    expect_true(mount_status.succeeded());
    if (mount_status.failed()) return {};
    auto [guard, acquire_status] = fixture->channel(0);
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return {};
    util::io_channel_t& channel = *guard;
    util::nvme_queues_ref_t const queues {&fixture->queues, 0};

    std::string const source = fixture->data_directory_path(0), restored = fixture->data_directory_path(1);
    std::string const device = fmt::format("{}/archive.raw", fixture->directory);
    std::string const corrupted = fmt::format("{}/corrupted", fixture->directory);
    {
        std::FILE* const file = std::fopen(device.c_str(), "wb");
        if (file) std::fclose(file);
        std::error_code error;
        std::filesystem::resize_file(device.c_str(), 4 * util::slot_bytes_k, error);
        expect_true(!error);
    }
    std::string const raw = fmt::format("{}@0+4GiB", device);

    collection_id_t collection;
    typename database_t::archive_report_t first, second;
    {
        volume_t volume;
        std::unique_ptr<database_t> store;
        if (!expect_true(open_archive<types_t>(*fixture, channel, "source", source.c_str(), volume, store).succeeded()))
            return {};
        auto [created, create_status] = create_named(*store, queues, "blobs", test_config(0, value_max_size_k));
        expect_true(create_status.succeeded());
        if (create_status.failed()) return {};
        collection = created;
        st::vector<util::byte_t> value;
        expect_true(value.resize(512) == st::status_t::success_k);
        auto write = [&](std::uint64_t begin, std::uint64_t end) noexcept {
            for (std::uint64_t key = begin; key != end; ++key) {
                for (std::size_t position = 0; position != blob_bytes_of(key); ++position)
                    value[position] = painted(key, position);
                expect_true(store
                                ->insert(any_collection_id_t {collection}, static_cast<stored_key_t>(key),
                                         stored_value_view_t {value.data(), blob_bytes_of(key)}, queues)
                                .succeeded());
            }
            expect_true(store->flush(queues).succeeded());
        };
        write(0, 200);
        auto [saved, save_status] = save_head(*store, queues, "archived", raw.data());
        expect_true(save_status.succeeded());
        first = saved;
        expect_true(first.written != 0 && first.kept == 0);

        // The second save keeps every run the first wrote and writes only the run of the new keys.
        write(200, 300);
        auto [again, again_status] = save_head(*store, queues, "archived", raw.data());
        expect_true(again_status.succeeded());
        second = again;
        expect_equal(second.kept, first.written);
        expect_true(second.written != 0 && second.written < first.written + second.written);
        expect_true(store->unmount(queues).succeeded());
    }
    {
        volume_t volume;
        std::unique_ptr<database_t> store;
        if (!expect_true(open_archive<types_t>(*fixture, channel, "archived", raw.data(), volume, store).succeeded()))
            return {};
        expect_equal(differing_keys(*store, queues, collection, 300), std::size_t {0});
        for (util::manifest_file_t const& file : volume.manifest().files) expect_true(!file.sha256.empty());

        // The raw copy saves back into a directory, which restores every byte again.
        auto [back, back_status] = save_head(*store, queues, "archived", restored.c_str());
        expect_true(back_status.succeeded());
        expect_true(back.written + back.linked == first.written + second.written);
        std::error_code error;
        std::filesystem::create_directory(corrupted.c_str(), error);
        expect_true(save_head(*store, queues, "archived", corrupted.c_str()).has_value());
        expect_true(store->unmount(queues).succeeded());
    }
    for (std::size_t open = 0; open != 2; ++open) {
        std::size_t hashed_blocks = 0;
        volume_t volume;
        std::unique_ptr<database_t> store;
        if (!expect_true(open_archive<types_t>(*fixture, channel, "archived", restored.c_str(), volume, store,
                                               counted_sha256_t {{}, &hashed_blocks})
                             .succeeded()))
            return {};
        expect_true((hashed_blocks != 0) == (open == 0));
        expect_equal(differing_keys(*store, queues, collection, 300), std::size_t {0});
        expect_true(store->unmount(queues).succeeded());
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
    expect_true(largest_bytes != 0);
    std::FILE* const file = std::fopen(largest.c_str(), "r+b");
    expect_true(file != nullptr);
    if (!file) return {};
    long const middle = static_cast<long>(largest_bytes / 2);
    expect_true(std::fseek(file, middle, SEEK_SET) == 0);
    int const byte = std::fgetc(file);
    expect_true(std::fseek(file, middle, SEEK_SET) == 0);
    expect_true(std::fputc(byte ^ 0xFF, file) != EOF);
    std::fclose(file);
    volume_t volume;
    std::unique_ptr<database_t> store;
    expect_status(open_archive<types_t>(*fixture, channel, "archived", corrupted.c_str(), volume, store),
                  util::status_code_t::malformed_k);
    return {};
}

#pragma endregion Catalog Cases

#pragma region Transaction Cases

/**
 *  @brief A transaction spanning two collections is never seen half applied.
 *
 *  Each round writes a whole band of keys into both collections, and their memory-run bounds are narrower
 *  than a band, so every commit lands past the bounds and flushes the band before it, and a reader has
 *  runs publishing beside it. The reader holds one level set per collection at one moment and counts
 *  every key that one of them holds and the other does not.
 */
template <typename mount_type_>
void test_commit_is_all_or_nothing(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t rounds_k = 10;
    constexpr std::size_t band_k = 24;
    constexpr std::size_t keys_k = rounds_k * band_k;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;

    collection_config_t narrow = test_config(8, 0);
    narrow.memory_runs_max_deltas = 6;
    narrow.memory_runs_max_bytes = 8ull * 1024ull;
    auto const [first, first_status] = create_named(store, queues, "first", narrow);
    auto const [second, second_status] = create_named(store, queues, "second", narrow);
    expect_true(first_status.succeeded());
    if (!expect_true(second_status.succeeded())) return;

    bool writing {true};
    std::size_t half_applied {0};
    std::size_t observations {0};
    std::thread reader {[&]() noexcept {
        while (atomic_reference<bool> {writing}.load(std::memory_order_acquire)) {
            auto [taken, pin_status] = store.pin_stamp();
            if (pin_status.failed()) continue;
            stored_value_size_t left = 0;
            stored_value_size_t right = 0;
            for (std::size_t key = 0; key != keys_k; ++key) {
                lookup_result_t const here = taken.find(any_collection_id_t {first}, static_cast<stored_key_t>(key),
                                                        left);
                lookup_result_t const there = taken.find(any_collection_id_t {second}, static_cast<stored_key_t>(key),
                                                         right);
                if ((here == lookup_result_t::written_k) != (there == lookup_result_t::written_k))
                    atomic_reference<std::size_t> {half_applied}.fetch_add(1, std::memory_order_relaxed);
            }
            atomic_reference<std::size_t> {observations}.fetch_add(1, std::memory_order_relaxed);
        }
    }};

    for (std::size_t round = 0; round != rounds_k; ++round) {
        auto [transaction, begin_status] = store.create_transaction();
        expect_true(begin_status.succeeded());
        if (begin_status.failed()) break;
        for (std::size_t step = 0; step != band_k; ++step) {
            stored_key_t const key = static_cast<stored_key_t>(round * band_k + step);
            std::array<util::byte_t, 8> const counter = counted(key);
            expect_true(
                transaction->insert(any_collection_id_t {first}, key, stored_value_view_t {counter}).succeeded());
            expect_true(
                transaction->insert(any_collection_id_t {second}, key, stored_value_view_t {counter}).succeeded());
        }
        expect_true(transaction->commit(queues).succeeded());
    }
    atomic_reference<bool> {writing}.store(false, std::memory_order_release);
    reader.join();

    expect_equal(atomic_reference<std::size_t> {half_applied}.load(std::memory_order_relaxed), std::size_t {0});
    expect_true(atomic_reference<std::size_t> {observations}.load(std::memory_order_relaxed) != 0);

    // Every write of every committed transaction is in the store afterwards.
    st::vector<util::byte_t> into;
    expect_true(into.resize(8) == st::status_t::success_k);
    std::size_t mismatches = 0;
    for (std::size_t key = 0; key != keys_k; ++key) {
        stored_value_size_t size = 0;
        expect_true(store
                        .select(any_collection_id_t {first}, static_cast<stored_key_t>(key),
                                std::span<util::byte_t> {into}, size, head_stamp_k, queues)
                        .succeeded());
        if (counter_of(std::span<util::byte_t const> {into.data(), size}) != key) ++mismatches;
        expect_true(store
                        .select(any_collection_id_t {second}, static_cast<stored_key_t>(key),
                                std::span<util::byte_t> {into}, size, head_stamp_k, queues)
                        .succeeded());
        if (counter_of(std::span<util::byte_t const> {into.data(), size}) != key) ++mismatches;
    }
    expect_equal(mismatches, std::size_t {0});
}

/** A transaction writing one collection hands its buffer to the run its commit lands, so a read of
 *  the run's values views the very bytes the transaction wrote, which no seal copied; one filling
 *  less than half its buffer is copied, so a run never holds twice what its bounds count. */
template <typename mount_type_>
void test_single_collection_commit_keeps_its_bytes(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t keys_k = 6;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;
    auto const [own, create_status] = create_named(store, queues, "own", test_config(0, value_max_size_k));
    if (!expect_true(create_status.succeeded())) return;

    // Commits a transaction of @p capacity bytes over the keys of @p round, answering how many
    // values a read views outside the bytes the transaction wrote.
    auto moved_values = [&](std::size_t capacity, std::size_t round) {
        std::size_t moved = keys_k;
        transaction_config_t shape;
        shape.max_bytes = capacity;
        shape.max_deltas = keys_k;
        auto [transaction, begin_status] = store.create_transaction(shape);
        expect_true(begin_status.succeeded());
        if (begin_status.failed()) return moved;
        std::array<stored_key_t, keys_k> keys {};
        std::array<util::byte_t const*, keys_k> written {};
        for (std::size_t key = 0; key != keys_k; ++key) {
            std::array<util::byte_t, 512> value {};
            std::size_t const width = blob_bytes_of(key);
            for (std::size_t position = 0; position != width; ++position)
                value[position] = painted(key + round, position);
            keys[key] = static_cast<stored_key_t>(key);
            expect_true(
                transaction->insert(any_collection_id_t {own}, keys[key], stored_value_view_t {value.data(), width})
                    .succeeded());
            util::delta_view_t held;
            expect_true(transaction->uncommitted_delta(own, keys[key], held));
            written[key] = held.bytes.data();
        }
        expect_true(transaction->commit(queues).succeeded());

        std::array<lookup_result_t, keys_k> results {};
        std::array<util::byte_t const*, keys_k> viewed {};
        expect_true(
            store
                .select(
                    any_collection_id_t {own}, std::span<stored_key_t const> {keys},
                    std::span<lookup_result_t> {results},
                    [&](std::size_t request, std::uint64_t offset, std::span<util::byte_t const> bytes) noexcept {
                        if (offset == 0) viewed[request] = bytes.data();
                    },
                    head_stamp_k, queues)
                .succeeded());
        moved = 0;
        for (std::size_t key = 0; key != keys_k; ++key)
            moved += results[key] != lookup_result_t::written_k || viewed[key] != written[key];
        return moved;
    };
    std::size_t filled = 0;
    for (std::size_t key = 0; key != keys_k; ++key) filled += blob_bytes_of(key);
    expect_equal(moved_values(filled, 0), std::size_t {0});
    expect_equal(moved_values(filled * 4, 1), keys_k);
}

/** Transactions writing one collection, and some writing a second beside it, land tier-zero runs
 *  that merge four at a time, then flush, and every key reads back what its last commit wrote, from
 *  the merged runs and from the level. */
template <typename mount_type_>
void test_merged_and_flushed_values_read_back(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t rounds_k = 64;
    constexpr std::uint64_t keys_k = 40;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;
    auto const [own, own_status] = create_named(store, queues, "own", test_config(0, value_max_size_k));
    auto const [beside, beside_status] = create_named(store, queues, "beside", test_config(0, value_max_size_k));
    expect_true(own_status.succeeded());
    if (!expect_true(beside_status.succeeded())) return;

    // The round that last wrote each key, plus one, so zero stands for a key never written.
    std::array<std::uint64_t, keys_k> own_rounds {};
    std::array<std::uint64_t, keys_k> beside_rounds {};
    auto paint = [](std::uint64_t key, std::uint64_t round, std::array<util::byte_t, 512>& value) noexcept {
        std::size_t const width = blob_bytes_of(key + round);
        for (std::size_t position = 0; position != width; ++position) value[position] = painted(key + round, position);
        return width;
    };
    auto read_back = [&](collection_id_t collection, std::span<std::uint64_t const> rounds) {
        std::size_t mismatches = 0;
        for (std::uint64_t key = 0; key != keys_k; ++key) {
            if (rounds[key] == 0) continue;
            std::array<util::byte_t, 512> wanted {};
            std::size_t const width = paint(key, rounds[key] - 1, wanted);
            std::array<util::byte_t, 512> into {};
            stored_value_size_t size = 0;
            util::status_t const selected = store.select(any_collection_id_t {collection},
                                                         static_cast<stored_key_t>(key), std::span<util::byte_t> {into},
                                                         size, head_stamp_k, queues);
            mismatches += selected.failed() || size != width || std::memcmp(into.data(), wanted.data(), width) != 0;
        }
        return expect_equal(mismatches, std::size_t {0});
    };

    for (std::uint64_t round = 0; round != rounds_k; ++round) {
        // Sized to the widest values, so a buffer of one collection is over half full and adopted.
        transaction_config_t shape;
        shape.max_deltas = (round % 6 == 5 ? 2 : 1) * (1 + round % 5);
        shape.max_bytes = shape.max_deltas * blob_bytes_of(96);
        auto [transaction, begin_status] = store.create_transaction(shape);
        expect_true(begin_status.succeeded());
        if (begin_status.failed()) break;
        for (std::uint64_t step = 0; step != 1 + round % 5; ++step) {
            stored_key_t const key = static_cast<stored_key_t>((round * 7 + step * 3) % keys_k);
            std::array<util::byte_t, 512> value {};
            std::size_t const width = paint(key, round, value);
            expect_true(transaction->insert(any_collection_id_t {own}, key, stored_value_view_t {value.data(), width})
                            .succeeded());
            own_rounds[key] = round + 1;
            if (round % 6 != 5) continue;
            expect_true(
                transaction->insert(any_collection_id_t {beside}, key, stored_value_view_t {value.data(), width})
                    .succeeded());
            beside_rounds[key] = round + 1;
        }
        if (!expect_true(transaction->commit(queues).succeeded())) break;
        if (round % 16 != 15) continue;
        bool const own_held = read_back(own, own_rounds);
        if (!read_back(beside, beside_rounds) || !own_held) break;
    }
    expect_true(store.memory_shape(any_collection_id_t {own}).runs < rounds_k / 4);
    expect_true(store.flush(queues).succeeded());
    read_back(own, own_rounds);
    read_back(beside, beside_rounds);
}

/** A transaction that conflicts on one collection stages nothing, and leaves the other untouched. */
template <typename mount_type_>
void test_conflict_leaves_the_other_collection_untouched(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;

    auto const [contested, contested_status] = create_named(store, queues, "contested", test_config(8, 0));
    auto const [quiet, quiet_status] = create_named(store, queues, "quiet", test_config(8, 0));
    expect_true(contested_status.succeeded());
    if (!expect_true(quiet_status.succeeded())) return;

    auto [loser, loser_status] = store.create_transaction();
    auto [winner, winner_status] = store.create_transaction();
    expect_true(loser_status.succeeded());
    if (!expect_true(winner_status.succeeded())) return;

    // Both transactions start before either commits, so the second to stage sees the first's write.
    std::array<util::byte_t, 8> const mine = counted(111);
    std::array<util::byte_t, 8> const theirs = counted(222);
    expect_true(
        loser->insert(any_collection_id_t {contested}, stored_key_t {1}, stored_value_view_t {mine}).succeeded());
    expect_true(loser->insert(any_collection_id_t {quiet}, stored_key_t {2}, stored_value_view_t {mine}).succeeded());

    expect_true(
        winner->insert(any_collection_id_t {contested}, stored_key_t {1}, stored_value_view_t {theirs}).succeeded());
    expect_true(winner->commit(queues).succeeded());

    expect_status(loser->stage(queues), util::status_code_t::write_conflict_k);
    expect_status(loser->commit(queues), util::status_code_t::write_conflict_k);

    // The quiet collection never saw the refused transaction's write.
    expect_true(!store.contains(any_collection_id_t {quiet}, stored_key_t {2}));
    st::vector<util::byte_t> into;
    expect_true(into.resize(8) == st::status_t::success_k);
    stored_value_size_t size = 0;
    expect_true(store
                    .select(any_collection_id_t {contested}, stored_key_t {1}, std::span<util::byte_t> {into}, size,
                            head_stamp_k, queues)
                    .succeeded());
    expect_equal(counter_of(std::span<util::byte_t const> {into.data(), size}), std::uint64_t {222});

    // Rolled back, the refused transaction is usable again and its second try lands.
    loser->rollback();
    expect_true(loser->insert(any_collection_id_t {quiet}, stored_key_t {2}, stored_value_view_t {mine}).succeeded());
    expect_true(loser->commit(queues).succeeded());
    expect_true(store.contains(any_collection_id_t {quiet}, stored_key_t {2}));
}

/** A rollback drops the writes, and a renewal keeps them while a read refuses one. */
template <typename mount_type_>
void test_rollback_and_renew_behave(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using transaction_t = typename types_t::transaction_t;
    using stack_t = typename types_t::stack_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;

    auto const [only, create_status] = create_named(store, queues, "only", test_config(8, 0));
    expect_true(create_status.succeeded());
    if (create_status.failed()) return;

    auto [transaction, begin_status] = store.create_transaction();
    expect_true(begin_status.succeeded());
    if (begin_status.failed()) return;
    transaction_t& handler = *transaction;
    std::array<util::byte_t, 8> const counter = counted(7);

    // A rollback drops what was written and leaves the transaction ready for more.
    expect_true(
        handler.insert(any_collection_id_t {only}, stored_key_t {1}, stored_value_view_t {counter}).succeeded());
    expect_true(handler.contains_uncommitted(any_collection_id_t {only}, stored_key_t {1}));
    handler.rollback();
    expect_true(!handler.contains_uncommitted(any_collection_id_t {only}, stored_key_t {1}));
    expect_true(handler.commit(queues).succeeded());
    expect_true(!store.contains(any_collection_id_t {only}, stored_key_t {1}));

    // A renewal keeps the writes of a transaction that read nothing, and its start stamp moves.
    expect_true(
        handler.insert(any_collection_id_t {only}, stored_key_t {2}, stored_value_view_t {counter}).succeeded());
    transaction_stamp_t const before = handler.start_stamp();
    expect_true(handler.renew().succeeded());
    expect_true(handler.start_stamp() >= before);
    expect_true(handler.contains_uncommitted(any_collection_id_t {only}, stored_key_t {2}));
    expect_true(handler.commit(queues).succeeded());
    expect_true(store.contains(any_collection_id_t {only}, stored_key_t {2}));
    expect_true(handler.last_commit_stamp() != invalid_transaction_stamp_k);

    // A transaction that has read something cannot move its watches, so a renewal refuses it.
    st::vector<util::byte_t> into;
    expect_true(into.resize(8) == st::status_t::success_k);
    stored_value_size_t size = 0;
    expect_true(handler.select(any_collection_id_t {only}, stored_key_t {2}, stored_value_span_t {into}, size, queues)
                    .succeeded());
    expect_status(handler.renew(), util::status_code_t::invalid_argument_k);

    // A staged transaction takes no more writes until it commits or rolls back.
    expect_true(handler.stage(queues).succeeded());
    expect_true(handler.insert(any_collection_id_t {only}, stored_key_t {3}, stored_value_view_t {counter}) ==
                util::status_code_t::invalid_argument_k);
    expect_status(handler.stage(queues), util::status_code_t::invalid_argument_k);
    handler.rollback();
    expect_true(
        handler.insert(any_collection_id_t {only}, stored_key_t {3}, stored_value_view_t {counter}).succeeded());
    expect_true(handler.commit(queues).succeeded());
    expect_true(store.contains(any_collection_id_t {only}, stored_key_t {3}));
}

/** A snapshot keeps answering the database of its moment while later writes land beside it. */
template <typename mount_type_>
void test_snapshot_holds_its_moment(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;

    auto const [left, left_status] = create_named(store, queues, "left", test_config(8, 0));
    auto const [right, right_status] = create_named(store, queues, "right", test_config(0, value_max_size_k));
    expect_true(left_status.succeeded());
    if (!expect_true(right_status.succeeded())) return;

    std::array<util::byte_t, 8> const older = counted(1);
    std::array<util::byte_t, 8> const newer = counted(2);
    expect_true(
        store.insert(any_collection_id_t {left}, stored_key_t {1}, stored_value_view_t {older}, queues).succeeded());
    expect_true(
        store.insert(any_collection_id_t {right}, stored_key_t {1}, stored_value_view_t {older}, queues).succeeded());

    auto [taken, pin_status] = store.pin_stamp();
    if (!expect_true(pin_status.succeeded())) return;
    expect_equal(store.population_count(), std::uint64_t {2});

    // Later commits and a flush land beside the snapshot, which publishes over nothing it waits for.
    expect_true(
        store.insert(any_collection_id_t {left}, stored_key_t {1}, stored_value_view_t {newer}, queues).succeeded());
    expect_true(
        store.insert(any_collection_id_t {left}, stored_key_t {2}, stored_value_view_t {newer}, queues).succeeded());
    expect_true(store.remove(any_collection_id_t {right}, stored_key_t {1}, queues).succeeded());

    st::vector<util::byte_t> into;
    expect_true(into.resize(8) == st::status_t::success_k);
    for (std::size_t round = 0; round != 2; ++round) {
        stored_value_size_t size = 0;
        expect_true(
            taken.select(any_collection_id_t {left}, stored_key_t {1}, std::span<util::byte_t> {into}, size, queues)
                .succeeded());
        expect_equal(counter_of(std::span<util::byte_t const> {into.data(), size}), std::uint64_t {1});
        expect_true(taken.find(any_collection_id_t {left}, stored_key_t {2}, size) == lookup_result_t::missing_k);
        expect_true(taken.find(any_collection_id_t {right}, stored_key_t {1}, size) == lookup_result_t::written_k);
        // The second round reads what the flush wrote, which kept every version the snapshot pins.
        if (round == 0) expect_true(store.flush(queues).succeeded());
    }
    expect_true(taken.contains(any_collection_id_t {owned_name("right")}));
    expect_true(!taken.contains(any_collection_id_t {owned_name("absent")}));

    // The store itself has moved on, which is what makes the snapshot's answers its own.
    stored_value_size_t size = 0;
    expect_true(store
                    .select(any_collection_id_t {left}, stored_key_t {1}, std::span<util::byte_t> {into}, size,
                            head_stamp_k, queues)
                    .succeeded());
    expect_equal(counter_of(std::span<util::byte_t const> {into.data(), size}), std::uint64_t {2});
    expect_true(!store.contains(any_collection_id_t {right}, stored_key_t {1}));

    // Letting the snapshot go unpins the versions the flush below may drop
    taken = {};
    expect_true(store.flush(queues).succeeded());
    expect_true(store
                    .select(any_collection_id_t {left}, stored_key_t {1}, std::span<util::byte_t> {into}, size,
                            head_stamp_k, queues)
                    .succeeded());
    expect_equal(counter_of(std::span<util::byte_t const> {into.data(), size}), std::uint64_t {2});
    expect_true(!store.contains(any_collection_id_t {right}, stored_key_t {1}));
}

/** An unordered scan walks every run on its own: a key two runs hold comes out twice, a removal
 *  comes out as one, a commit leaves the counted runs where they were, and a flush rearranges them,
 *  which the next run's walk reports. */
template <typename mount_type_>
void test_runs_scan_one_at_a_time(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;
    auto const [created, create_status] = create_named(store, queues, "runs", test_config(0, value_max_size_k));
    expect_true(create_status.succeeded());
    if (create_status.failed()) return;
    any_collection_id_t const runs {created};

    std::array<util::byte_t, 8> const value = counted(7);
    for (std::uint64_t key = 1; key <= 4; ++key)
        expect_true(store.insert(runs, stored_key_t {key}, stored_value_view_t {value}, queues).succeeded());
    expect_true(store.flush(queues).succeeded());
    // Two memory runs above the flushed one, fewer than a merge of memory runs by tier takes.
    expect_true(store.insert(runs, stored_key_t {2}, stored_value_view_t {value}, queues).succeeded());
    expect_true(store.remove(runs, stored_key_t {3}, queues).succeeded());

    auto const [counted_runs, count_status] = store.runs_count(runs);
    expect_true(count_status.succeeded());
    if (count_status.failed()) return;
    expect_true(counted_runs.count >= 3);
    std::array<std::size_t, 5> written {};
    std::array<std::size_t, 5> removed {};
    auto const tally = [&](stored_key_t key, lookup_result_t state, stored_value_size_t size) noexcept {
        std::size_t const index = static_cast<std::size_t>(key) < written.size() ? static_cast<std::size_t>(key) : 0;
        if (state == lookup_result_t::written_k && size == value.size()) ++written[index];
        if (state == lookup_result_t::removed_k) ++removed[index];
        return true;
    };
    for (std::uint64_t run = 0; run != counted_runs.count; ++run)
        expect_true(store.scan_run(runs, counted_runs, run, stored_key_t {1}, tally).succeeded());
    expect_equal(written[1], std::size_t {1});
    expect_equal(written[2], std::size_t {2});
    expect_equal(written[3], std::size_t {1});
    expect_equal(written[4], std::size_t {1});
    expect_equal(removed[3], std::size_t {1});
    expect_equal(written[0], std::size_t {0});

    // A commit only puts a newer run ahead, while a flush rearranges the runs the count named.
    expect_true(store.insert(runs, stored_key_t {5}, stored_value_view_t {value}, queues).succeeded());
    expect_true(store.scan_run(runs, counted_runs, 0, stored_key_t {1}, tally).succeeded());
    expect_true(store.flush(queues).succeeded());
    expect_true(store.scan_run(runs, counted_runs, 0, stored_key_t {1}, tally) == util::status_code_t::contended_k);
    expect_true(store.scan_run(runs, counted_runs, counted_runs.count, stored_key_t {1}, tally) ==
                util::status_code_t::invalid_argument_k);
}

/** A split of one entry from before the first key, or from past the last walking down, holds that
 *  entry and stops at the key after it; four tasks of sixteen entries leave none empty, and a warm
 *  split makes no call of @c operator new. */
template <typename mount_type_>
void test_split_holds_its_first_entries(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;
    auto const [created, create_status] = create_named(store, queues, "split", test_config(0, value_max_size_k));
    expect_true(create_status.succeeded());
    if (create_status.failed()) return;
    any_collection_id_t const walked {created};

    constexpr std::uint64_t keys_k = 64;
    std::array<util::byte_t, 8> const value = counted(7);
    for (std::uint64_t key = 1; key <= keys_k; ++key)
        expect_true(store.insert(walked, stored_key_t {key}, stored_value_view_t {value}, queues).succeeded());
    expect_true(store.flush(queues).succeeded());
    auto [read, read_status] = store.read_collection(walked, head_stamp_k);
    expect_true(read_status.succeeded());
    if (read_status.failed()) return;

    constexpr stored_key_t largest_k = (std::numeric_limits<stored_key_t>::max)();
    std::array<stored_key_t, 4> starts {};
    std::array<std::uint64_t, 4> entries {};
    std::array<std::uint64_t, 4> bytes {};
    std::optional<stored_key_t> stop;
    ustore_arena_handle_t scratch {};
    auto split_once = [&](stored_key_t from, stored_key_t bound, scan_direction_t direction, std::uint64_t wanted,
                          std::size_t capacity) noexcept {
        util::arena_rewind(&scratch);
        return store.split(read, &scratch, from, bound, direction, wanted, std::span {starts}.first(capacity),
                           std::span {entries}.first(capacity), std::span {bytes}.first(capacity), stop);
    };

    auto [ascending, ascending_status] = split_once(0, largest_k, scan_direction_t::ascending_k, 1, 1);
    expect_true(ascending_status.succeeded());
    expect_equal(ascending, std::size_t {1});
    expect_equal(entries[0], std::uint64_t {1});
    expect_true(stop == stored_key_t {2});

    auto [descending, descending_status] = split_once(largest_k, 0, scan_direction_t::descending_k, 1, 1);
    expect_true(descending_status.succeeded());
    expect_equal(descending, std::size_t {1});
    expect_equal(entries[0], std::uint64_t {1});
    expect_true(stop == stored_key_t {keys_k - 1});

    // The first call can spill into a second arena block, which the rewinds after it keep
    for (std::size_t round = 0; round != 2; ++round) {
        auto [warmed, warm_status] = split_once(0, largest_k, scan_direction_t::ascending_k, 16, 4);
        expect_true(warm_status.succeeded() && warmed != 0);
    }
    std::size_t const before = allocation_watch_g.calls;
    auto [tasks, tasks_status] = split_once(0, largest_k, scan_direction_t::ascending_k, 16, 4);
    expect_equal(allocation_watch_g.calls - before, std::size_t {0});
    expect_true(tasks_status.succeeded());
    expect_true(stop.has_value());
    std::uint64_t covered = 0;
    for (std::size_t task = 0; task != tasks; ++task) {
        expect_true(entries[task] != 0, "task {}", task);
        if (task) expect_true(starts[task - 1] < starts[task], "task {}", task);
        covered += entries[task];
    }
    expect_true(covered >= 16 && covered < keys_k);
    util::arena_free_blocks(scratch);
}

/** A head read of one collection through one reading pins no stamp in the ledger: its lookups,
 *  its copy and its walk run with no reader listed, the walk's visitor included. */
template <typename mount_type_>
void test_head_reading_pins_nothing(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;
    auto const [created, create_status] = create_named(store, queues, "unpinned", test_config(0, value_max_size_k));
    expect_true(create_status.succeeded());
    if (create_status.failed()) return;
    any_collection_id_t const unpinned {created};

    std::array<util::byte_t, 8> const value = counted(7);
    for (std::uint64_t key = 1; key <= 4; ++key)
        expect_true(store.insert(unpinned, stored_key_t {key}, stored_value_view_t {value}, queues).succeeded());
    expect_true(store.flush(queues).succeeded());
    expect_true(store.insert(unpinned, stored_key_t {5}, stored_value_view_t {value}, queues).succeeded());

    auto [read, read_status] = store.read_collection(unpinned, head_stamp_k);
    expect_true(read_status.succeeded());
    if (read_status.failed()) return;
    stored_value_size_t size = 0;
    expect_true(store.find(read, stored_key_t {1}, size) == lookup_result_t::written_k);
    expect_equal(size, static_cast<stored_value_size_t>(value.size()));
    expect_true(store.stored_value_size(read, stored_key_t {5}, size, queues).succeeded());
    std::array<util::byte_t, 8> copied {};
    expect_true(store.select(read, stored_key_t {3}, std::span<util::byte_t> {copied}, size, queues).succeeded());
    expect_true(copied == value);
    std::size_t walked = 0;
    std::size_t pinned_inside = 0;
    util::local_arena_t walk;
    expect_true(store
                    .scan(
                        read, walk.allocator().arena, stored_key_t {1},
                        [&](stored_key_t, lookup_result_t state, stored_value_size_t) noexcept {
                            walked += state == lookup_result_t::written_k;
                            pinned_inside += store.ledger().newest_reader() != unwritten_stamp_k;
                            return true;
                        },
                        queues)
                    .succeeded());
    expect_equal(walked, std::size_t {5});
    expect_equal(pinned_inside, std::size_t {0});
    st::vector<transaction_stamp_t> pinned;
    expect_true(store.ledger().pinned_readers(pinned).succeeded());
    expect_equal(pinned.size(), std::size_t {0});
}

/** A warm batched select of keys in level zero, in the value cache and in a memory run makes no
 *  call of @c operator new, rewinding one scratch between calls as <tt>src/</tt> does, and a sized
 *  sink hears every size once from the one descent of its key, before the bytes it asks for. */
template <typename mount_type_>
void test_warm_batched_select_allocates_nothing(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;
    auto const [created, create_status] = create_named(store, queues, "warm", test_config(0, value_max_size_k));
    expect_true(create_status.succeeded());
    if (create_status.failed()) return;
    any_collection_id_t const warm {created};

    constexpr std::size_t keys_k = 64;
    std::array<stored_key_t, keys_k> keys {};
    std::array<util::byte_t, value_max_size_k> value {};
    std::size_t expected_bytes = 0;
    for (std::size_t index = 0; index != keys_k; ++index) {
        keys[index] = stored_key_t {index};
        std::size_t const width = blob_bytes_of(index);
        for (std::size_t position = 0; position != width; ++position) value[position] = painted(index, position);
        expect_true(store.insert(warm, keys[index], stored_value_view_t {value.data(), width}, queues).succeeded());
        expected_bytes += width;
    }
    expect_true(store.flush(queues).succeeded());
    // A single select files its value in the cache; a write after the flush stays in memory.
    std::array<util::byte_t, value_max_size_k> into {};
    stored_value_size_t size = 0;
    expect_true(store.select(warm, keys[0], std::span<util::byte_t> {into}, size, head_stamp_k, queues).succeeded());
    expect_true(store.insert(warm, keys[1], stored_value_view_t {value.data(), blob_bytes_of(1)}, queues).succeeded());

    auto [read, read_status] = store.read_collection(warm, head_stamp_k);
    expect_true(read_status.succeeded());
    if (read_status.failed()) return;
    std::array<lookup_result_t, keys_k> results {};
    std::size_t held = 0;
    auto sink = [&](std::size_t, std::uint64_t, std::span<util::byte_t const> bytes) noexcept { held += bytes.size(); };
    ustore_arena_handle_t scratch {};
    auto select_once = [&]() noexcept {
        held = 0;
        util::arena_rewind(&scratch);
        return store.select(read, &scratch, std::span<stored_key_t const> {keys}, std::span<lookup_result_t> {results},
                            sink, queues);
    };
    // The first call can spill into a second arena block, and a rewind keeps only the newest block,
    // so the arena settles on the second call.
    expect_true(select_once().succeeded());
    expect_true(select_once().succeeded());
    std::size_t const before = allocation_watch_g.calls;
    expect_true(select_once().succeeded());
    std::size_t const allocated = allocation_watch_g.calls - before;
    fmt::println("    warm batched select: {} calls of operator new", allocated);
    expect_equal(allocated, std::size_t {0});
    expect_equal(held, expected_bytes);

    // A sized sink hears every size once, before its bytes, and a declined value moves no bytes
    std::array<std::size_t, keys_k> sizes_heard {};
    std::size_t landed_unheard = 0;
    bool declining = false;
    sized_sink heard {.sized =
                          [&](std::size_t request, stored_value_size_t size) noexcept {
                              ++sizes_heard[request];
                              expect_equal(std::size_t {size}, blob_bytes_of(request));
                              return !declining;
                          },
                      .landed =
                          [&](std::size_t request, std::uint64_t, std::span<util::byte_t const> bytes) noexcept {
                              landed_unheard += sizes_heard[request] == 0;
                              held += bytes.size();
                          }};
    for (bool const declines : {false, true}) {
        declining = declines, held = 0, landed_unheard = 0, sizes_heard = {};
        util::arena_rewind(&scratch);
        expect_true(store
                        .select(read, &scratch, std::span<stored_key_t const> {keys},
                                std::span<lookup_result_t> {results}, heard, queues)
                        .succeeded());
        for (std::size_t index = 0; index != keys_k; ++index) {
            expect_equal(sizes_heard[index], std::size_t {1}, "key {}", index);
            expect_true(results[index] == lookup_result_t::written_k, "key {}", index);
        }
        expect_equal(landed_unheard, std::size_t {0});
        expect_equal(held, declines ? std::size_t {0} : expected_bytes);
    }
    util::arena_free_blocks(scratch);
}

/**
 *  @brief Dropping a collection waits for no snapshot, since a snapshot is a pinned stamp that holds no level set,
 *      and the snapshot finds the collection gone afterwards.
 *
 *  The drop runs on a second thread while the snapshot lives, and must finish before the snapshot goes.
 */
template <typename mount_type_>
void test_drop_proceeds_beside_the_snapshot(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;

    auto const [doomed, create_status] = create_named(store, queues, "doomed", test_config(8, 0));
    if (!expect_true(create_status.succeeded())) return;
    std::array<util::byte_t, 8> const value = counted(1);
    expect_true(
        store.insert(any_collection_id_t {doomed}, stored_key_t {1}, stored_value_view_t {value}, queues).succeeded());

    bool dropped {false};
    bool succeeded {false};
    std::thread dropper;
    {
        auto [taken, pin_status] = store.pin_stamp();
        expect_true(pin_status.succeeded());
        if (pin_status.failed()) return;
        dropper = std::thread {[&]() noexcept {
            util::nvme_queues_ref_t const dropper_queues {queues.queues, 1};
            util::status_t const drop_status = store.drop_collection(any_collection_id_t {doomed},
                                                                     collection_invalid_id_k, dropper_queues);
            atomic_reference<bool> {succeeded}.store(drop_status.succeeded(), std::memory_order_relaxed);
            atomic_reference<bool> {dropped}.store(true, std::memory_order_release);
        }};
        dropper.join();
        stored_value_size_t size = 0;
        expect_true(taken.find(any_collection_id_t {doomed}, stored_key_t {1}, size) == lookup_result_t::missing_k);
    }
    expect_true(atomic_reference<bool> {dropped}.load(std::memory_order_acquire));
    expect_true(atomic_reference<bool> {succeeded}.load(std::memory_order_relaxed));
    expect_true(!store.has_collection(any_collection_id_t {doomed}));
}

/**
 *  @brief Several threads committing over the same two collections leave a state one serial order explains.
 *
 *  Every attempt reads both counters, writes both back one higher and commits; a conflict is retried.
 *  A lost update would leave a counter below the commits that landed, and the two counters would part.
 */
template <typename mount_type_>
void test_threads_commit_serializably(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using transaction_t = typename types_t::transaction_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t threads_k = 4;
    constexpr std::size_t increments_k = 25;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;

    auto const [left, left_status] = create_named(store, queues, "left", test_config(8, 0));
    auto const [right, right_status] = create_named(store, queues, "right", test_config(8, 0));
    expect_true(left_status.succeeded());
    if (!expect_true(right_status.succeeded())) return;

    std::array<util::byte_t, 8> const zero = counted(0);
    expect_true(
        store.insert(any_collection_id_t {left}, stored_key_t {0}, stored_value_view_t {zero}, queues).succeeded());
    expect_true(
        store.insert(any_collection_id_t {right}, stored_key_t {0}, stored_value_view_t {zero}, queues).succeeded());

    std::size_t landed {0};
    std::size_t refused {0};
    std::array<std::thread, threads_k> workers;
    for (std::size_t worker = 0; worker != threads_k; ++worker)
        workers[worker] = std::thread {[&, worker]() noexcept {
            util::nvme_queues_ref_t const worker_queues {queues.queues, 1 + worker};
            auto [transaction, begin_status] = store.create_transaction();
            if (!expect_true(begin_status.succeeded())) return;
            transaction_t& handler = *transaction;
            std::array<util::byte_t, 8> buffer {};

            for (std::size_t step = 0; step != increments_k; ++step) {
                bool committed = false;
                while (!committed) {
                    stored_value_size_t size = 0;
                    util::status_t read = handler.template select<watch_schedule_t::on_commit_k>(
                        any_collection_id_t {left}, stored_key_t {0}, stored_value_span_t {buffer}, size,
                        worker_queues);
                    if (!expect_true(read.succeeded())) return;
                    std::uint64_t const here = counter_of(std::span<util::byte_t const> {buffer.data(), size});

                    read = handler.template select<watch_schedule_t::on_commit_k>(
                        any_collection_id_t {right}, stored_key_t {0}, stored_value_span_t {buffer}, size,
                        worker_queues);
                    if (!expect_true(read.succeeded())) return;
                    std::uint64_t const there = counter_of(std::span<util::byte_t const> {buffer.data(), size});

                    std::array<util::byte_t, 8> const raised_here = counted(here + 1);
                    std::array<util::byte_t, 8> const raised_there = counted(there + 1);
                    util::status_t written = handler.insert(any_collection_id_t {left}, stored_key_t {0},
                                                            stored_value_view_t {raised_here});
                    if (written.succeeded())
                        written = handler.insert(any_collection_id_t {right}, stored_key_t {0},
                                                 stored_value_view_t {raised_there});
                    if (!expect_true(written.succeeded())) return;

                    util::status_t const commit = handler.commit(worker_queues);
                    if (commit.succeeded()) {
                        atomic_reference<std::size_t> {landed}.fetch_add(1, std::memory_order_relaxed);
                        committed = true;
                        continue;
                    }
                    if (!expect_status(commit, util::status_code_t::write_conflict_k)) return;
                    atomic_reference<std::size_t> {refused}.fetch_add(1, std::memory_order_relaxed);
                    handler.rollback();
                }
            }
        }};
    for (std::size_t worker = 0; worker != threads_k; ++worker) workers[worker].join();

    expect_equal(atomic_reference<std::size_t> {landed}.load(std::memory_order_relaxed), threads_k * increments_k);

    st::vector<util::byte_t> into;
    expect_true(into.resize(8) == st::status_t::success_k);
    stored_value_size_t size = 0;
    expect_true(store
                    .select(any_collection_id_t {left}, stored_key_t {0}, std::span<util::byte_t> {into}, size,
                            head_stamp_k, queues)
                    .succeeded());
    std::uint64_t const here = counter_of(std::span<util::byte_t const> {into.data(), size});
    expect_true(store
                    .select(any_collection_id_t {right}, stored_key_t {0}, std::span<util::byte_t> {into}, size,
                            head_stamp_k, queues)
                    .succeeded());
    std::uint64_t const there = counter_of(std::span<util::byte_t const> {into.data(), size});

    // Every commit that landed raised both counters exactly once, so no update was lost.
    expect_equal(here, static_cast<std::uint64_t>(threads_k * increments_k));
    expect_equal(there, here);
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
void test_head_writes_wait_for_records(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t threads_k = 8;
    constexpr std::size_t writes_k = 200;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;
    auto const [written, create_status] = create_named(store, queues, "written", test_config(8, 0));
    expect_true(create_status.succeeded());
    if (create_status.failed()) return;
    any_collection_id_t const id {written};

    std::size_t refused {0};
    std::array<std::thread, threads_k> writers;
    for (std::size_t writer = 0; writer != threads_k; ++writer)
        writers[writer] = std::thread {[&, writer]() noexcept {
            util::nvme_queues_ref_t const writer_queues {queues.queues, 1 + writer};
            for (std::size_t step = 0; step != writes_k; ++step) {
                stored_key_t const key = static_cast<stored_key_t>(writer * writes_k + step);
                std::array<util::byte_t, 8> const counter = counted(key);
                if (util::status_t const inserted = store.insert(id, key, stored_value_view_t {counter}, writer_queues);
                    inserted.failed()) {
                    if (atomic_reference<std::size_t> {refused}.fetch_add(1, std::memory_order_relaxed) == 0)
                        fmt::println(stderr, "  first refused write: {}", inserted.note);
                }
            }
        }};
    for (std::thread& writer : writers) writer.join();
    expect_equal(atomic_reference<std::size_t> {refused}.load(std::memory_order_relaxed), std::size_t {0});

    std::size_t missing = 0;
    std::array<util::byte_t, 8> into {};
    for (stored_key_t key = 0; key != threads_k * writes_k; ++key) {
        stored_value_size_t size = 0;
        if (store.select(id, key, std::span<util::byte_t> {into}, size, head_stamp_k, queues).failed() ||
            counter_of(std::span<util::byte_t const> {into.data(), size}) != key)
            ++missing;
    }
    expect_equal(missing, std::size_t {0});
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
void test_strings_shares_wait_for_records(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    using strings_store_t = typename types_t::strings_store_t;
    using strings_transaction_t = strings::nvme_transaction<database_t, strings_store_t>;
    using key_slot_t = typename strings_store_t::key_slot_t;
    constexpr std::size_t writers_k = 8;
    // One run below the stall bound of test_config, (F - 1)(T + 1) runs for F = 4 and T = 7.
    constexpr std::size_t filled_runs_k = 23;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;
    collection_id_t names;
    std::memcpy(names.uuid.bytes, util::uuid_generator().bytes, sizeof(names.uuid.bytes));
    expect_true(store
                    .create_collection(names, "names", static_cast<std::uint16_t>(value_layout_t::uuid_k),
                                       test_config(sizeof(ustore_uuid_t), 0), collection_invalid_id_k, queues)
                    .succeeded());
    auto const [held, create_status] = create_named(store, queues, "held", test_config(8, 0));
    if (!expect_true(create_status.succeeded())) return;

    auto text_of = [](std::size_t index) { return fmt::format("string-{}", index); };
    auto value_of = [](std::size_t index) {
        std::array<util::byte_t, key_slot_t::size> value {};
        for (std::size_t position = 0; position != value.size(); ++position) value[position] = painted(index, position);
        return value;
    };
    // Writes string @p index into @p written over a fresh transaction, which the caller
    // stages or commits.
    auto begin_write = [&](strings_transaction_t& written, std::size_t index) {
        auto [transaction, begin_status] = store.create_transaction();
        if (begin_status.failed()) return util::expected<typename database_t::transaction_unique_ptr_t> {begin_status};
        written.join(*transaction);
        std::array<util::byte_t, key_slot_t::size> const value = value_of(index);
        if (util::status_t const wrote = written.write(names, text_of(index),
                                                       util::delta_view_t {util::delta_kind_t::write_k, value},
                                                       invalid_transaction_stamp_k);
            wrote.failed())
            return util::expected<typename database_t::transaction_unique_ptr_t> {wrote};
        return util::expected<typename database_t::transaction_unique_ptr_t> {std::move(transaction)};
    };

    std::size_t refused {0};
    std::size_t started {0};
    std::array<std::thread, writers_k> writers;
    {
        typename database_t::ingest_t ingest;
        expect_true(store.begin_ingest(any_collection_id_t {held}, ingest).succeeded());
        for (std::size_t index = 0; index != filled_runs_k; ++index) {
            strings_transaction_t written {store};
            auto transaction = begin_write(written, index);
            expect_true(transaction.has_value() && written.commit(**transaction, queues).succeeded());
        }
        if (!expect_equal(store.memory_shape(any_collection_id_t {names}).runs, filled_runs_k)) return;

        strings_transaction_t staged_strings {store};
        auto staged = begin_write(staged_strings, filled_runs_k);
        if (!expect_true(staged.has_value() && staged_strings.stage(**staged, queues).succeeded())) return;
        for (std::size_t writer = 0; writer != writers_k; ++writer)
            writers[writer] = std::thread {[&, writer]() noexcept {
                strings_transaction_t written {store};
                auto transaction = begin_write(written, filled_runs_k + 1 + writer);
                atomic_reference<std::size_t> {started}.fetch_add(1, std::memory_order_release);
                util::status_t const committed =
                    transaction ? written.commit(**transaction, util::nvme_queues_ref_t {queues.queues, 1 + writer})
                                : transaction.status();
                if (committed.failed() &&
                    atomic_reference<std::size_t> {refused}.fetch_add(1, std::memory_order_relaxed) == 0)
                    fmt::println(stderr, "  first refused share: {}", committed.note);
            }};
        // Every writer seals past the stall check and parks behind the staged transaction or
        // on a record.
        while (atomic_reference<std::size_t> {started}.load(std::memory_order_acquire) != writers_k)
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        expect_true((*staged)->commit(queues).succeeded());
    }
    for (std::thread& writer : writers) writer.join();
    expect_equal(atomic_reference<std::size_t> {refused}.load(std::memory_order_relaxed), std::size_t {0});

    std::size_t missing = 0;
    util::status_t const visited = store.visit_store(
        any_collection_id_t {names}, [&]<typename store_type_>(store_type_& visited_store) noexcept -> util::status_t {
            if constexpr (std::is_same_v<store_type_, strings_store_t>)
                for (std::size_t index = 0; index != filled_runs_k + 1 + writers_k; ++index) {
                    std::array<util::byte_t, key_slot_t::size> value {};
                    if (visited_store.find(text_of(index), value) != lookup_result_t::written_k ||
                        value != value_of(index))
                        ++missing;
                }
            return {};
        });
    expect_true(visited.succeeded());
    expect_equal(missing, std::size_t {0});
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
void test_readers_beside_committers_and_flushes(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t committers_k = 2;
    constexpr std::size_t readers_k = 4;
    constexpr std::uint64_t rounds_k = 1500;
    constexpr std::uint64_t keys_k = 64;
    constexpr std::uint64_t no_round_k = (std::numeric_limits<std::uint64_t>::max)();

    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;

    // Memory runs of a few dozen deltas, so the committers merge, flush and compact beside the flusher.
    collection_config_t config = test_config(0, value_max_size_k);
    config.memory_runs_max_deltas = 32;
    std::array<collection_id_t, committers_k> named {};
    for (std::size_t committer = 0; committer != committers_k; ++committer) {
        auto const [created, create_status] = create_named(
            store, queues, committer == 0 ? util::string_view_t {"left"} : util::string_view_t {"right"}, config);
        expect_true(create_status.succeeded());
        if (create_status.failed()) return;
        named[committer] = created;
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
            util::nvme_queues_ref_t const committer_queues {queues.queues, 1 + committer};
            std::array<util::byte_t, 128> value {};
            for (std::uint64_t round = 0; round != rounds_k; ++round) {
                std::size_t const bytes = round_bytes_of(round);
                std::memcpy(value.data(), &round, sizeof(round));
                for (std::size_t position = sizeof(round); position != bytes; ++position)
                    value[position] = painted(round, position);
                atomic_reference<std::uint64_t> {started[committer]}.store(round, std::memory_order_release);
                if (store
                        .insert(any_collection_id_t {named[committer]}, stored_key_t {round % keys_k},
                                stored_value_view_t {value.data(), bytes}, committer_queues)
                        .failed())
                    atomic_reference<std::size_t> {wrong}.fetch_add(1, std::memory_order_relaxed);
                atomic_reference<std::uint64_t> {returned[committer]}.store(round, std::memory_order_release);
            }
            atomic_reference<std::size_t> {committing}.fetch_sub(1, std::memory_order_release);
        }};

    std::thread flusher {[&]() noexcept {
        util::nvme_queues_ref_t const flusher_queues {queues.queues, 1 + committers_k};
        while (atomic_reference<std::size_t> {committing}.load(std::memory_order_acquire) != 0) {
            if (store.flush(flusher_queues).failed())
                atomic_reference<std::size_t> {wrong}.fetch_add(1, std::memory_order_relaxed);
            if (store.needs_compaction() && store.compact({}, flusher_queues).failed())
                atomic_reference<std::size_t> {wrong}.fetch_add(1, std::memory_order_relaxed);
        }
    }};

    std::array<std::thread, readers_k> readers;
    for (std::size_t reader = 0; reader != readers_k; ++reader)
        readers[reader] = std::thread {[&, reader]() noexcept {
            util::nvme_queues_ref_t const reader_queues {queues.queues, 2 + committers_k + reader};
            std::array<util::byte_t, 128> into {};
            for (std::uint64_t step = reader;
                 atomic_reference<std::size_t> {committing}.load(std::memory_order_acquire) != 0; ++step) {
                std::size_t const committer = step % committers_k;
                std::uint64_t const key = (step * 7) % keys_k;
                std::uint64_t const before = atomic_reference<std::uint64_t> {returned[committer]}.load(
                    std::memory_order_acquire);
                stored_value_size_t size = 0;
                util::status_t const read = store.select(any_collection_id_t {named[committer]}, stored_key_t {key},
                                                         std::span<util::byte_t> {into}, size, head_stamp_k,
                                                         reader_queues);
                std::uint64_t const after = atomic_reference<std::uint64_t> {started[committer]}.load(
                    std::memory_order_acquire);
                atomic_reference<std::size_t> {reads}.fetch_add(1, std::memory_order_relaxed);

                // The last round that wrote this key and returned before the read began, if any did.
                bool const owed = before != no_round_k && before >= key;
                std::uint64_t const least = owed ? before - (before - key) % keys_k : no_round_k;
                if (read.failed()) {
                    if (owed || !(read == util::status_code_t::not_found_k))
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

    expect_equal(atomic_reference<std::size_t> {wrong}.load(std::memory_order_relaxed), std::size_t {0});
    expect_true(atomic_reference<std::size_t> {reads}.load(std::memory_order_relaxed) != 0);

    // Once every commit returned, a point read probes at most (F − 1)(T + 1) memory runs, T = ⌈log_F commits⌉.
    std::uint64_t const fan_in = config.lsm_tree.level_enlarge_factor;
    for (std::size_t committer = 0; committer != committers_k; ++committer) {
        memory_shape_t const shape = store.memory_shape(any_collection_id_t {named[committer]});
        std::uint64_t tiers = 0;
        for (std::uint64_t covered = 1; covered < shape.commits; covered *= fan_in) ++tiers;
        std::uint64_t const bound = (fan_in - 1) * (tiers + 1);
        if (shape.runs > bound)
            fmt::println(stderr, "  {} runs past {} after {} commits", shape.runs, bound, shape.commits);
        expect_true(shape.runs <= bound);
    }

    // Once everything returned, every key holds its last round, wherever the flushes left it.
    std::array<util::byte_t, 128> into {};
    for (std::size_t committer = 0; committer != committers_k; ++committer)
        for (std::uint64_t key = 0; key != keys_k; ++key) {
            stored_value_size_t size = 0;
            expect_true(store
                            .select(any_collection_id_t {named[committer]}, stored_key_t {key},
                                    std::span<util::byte_t> {into}, size, head_stamp_k, queues)
                            .succeeded());
            std::uint64_t round = 0;
            std::memcpy(&round, into.data(), sizeof(round));
            expect_equal(round, (rounds_k - 1) - (rounds_k - 1 - key) % keys_k);
        }
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
void test_lookups_outrun_the_write_path(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    constexpr std::size_t readers_k = 8;
    constexpr std::size_t rounds_k = 5'000;
    constexpr std::size_t churns_k = 64;
    constexpr std::chrono::seconds patience_k {5};

    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;

    auto const [left, left_status] = create_named(store, queues, "left", test_config(8, 0));
    expect_true(left_status.succeeded());
    if (left_status.failed()) return;

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
                if (store.collection_id(by_name) != left)
                    atomic_reference<std::size_t> {mismatched}.fetch_add(1, std::memory_order_relaxed);
                collection_config_t shape;
                if (store.collection_config(left, shape).failed())
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
    expect_true(progressed >= readers_k * rounds_k);

    // Every create and every drop republishes the catalog with the readers already inside one.
    for (std::size_t round = 0; round != churns_k; ++round) {
        auto const [created, created_status] = create_named(store, queues, "churn", test_config(8, 0));
        expect_true(created_status.succeeded());
        if (created_status.failed()) break;
        expect_true(store.drop_collection(any_collection_id_t {created}, collection_invalid_id_k, queues).succeeded());
    }

    atomic_reference<reader_phase_t> {phase}.store(reader_phase_t::retiring_k, std::memory_order_relaxed);
    for (std::size_t reader = 0; reader != readers_k; ++reader) readers[reader].join();

    expect_equal(atomic_reference<std::size_t> {mismatched}.load(std::memory_order_relaxed), std::size_t {0});
    expect_true(store.collection_id(by_name) == left);
    expect_true(!store.has_collection(churned));
}

#pragma endregion Transaction Cases

#pragma region Change Feed Cases

/** One change a feed handed out, with the counter its value holds, or zero for a removal. */
struct fed_change_t {
    stored_change<stored_key_t> change;
    std::uint64_t counter = 0;
};

/** Pulls changes of @p id after @p handed into @p fed on @p queues, @p wanted at a time,
 *  advancing @p handed. */
template <typename database_type_>
void pull_changes(database_type_& store, util::nvme_queues_ref_t queues, any_collection_id_t const& id,
                  stored_change<stored_key_t>& handed, std::size_t wanted, st::vector<fed_change_t>& fed) {
    for (;;) {
        std::size_t pulled = 0;
        stored_change<stored_key_t> const after = handed;
        auto const take = [&]<typename read_type_>(stored_change<stored_key_t> const& change, std::uint64_t value_bytes,
                                                   read_type_&& read) noexcept -> util::status_t {
            std::array<util::byte_t, 8> bytes {};
            if (value_bytes > bytes.size())
                return {util::status_code_t::invalid_argument_k, "A value wider than a counter"};
            std::span<util::byte_t> const value {bytes.data(), static_cast<std::size_t>(value_bytes)};
            if (util::status_t const copied = read(value); copied.failed()) return copied;
            if (fed.push_back(fed_change_t {change, counter_of(value)}) != st::status_t::success_k)
                return {util::status_code_t::out_of_memory_k, "No room for the changes fed"};
            handed = change;
            ++pulled;
            return {};
        };
        bool const read = expect_true(store.changes_after(id, after, wanted, take, queues).succeeded());
        if (!expect_true(pulled <= wanted) || !read || pulled == 0) return;
    }
}

/** Whether @p fed matches @p logged change for change, stamps, kinds and counters alike. */
void expect_fed(st::vector<fed_change_t> const& fed, st::vector<fed_change_t> const& logged) {
    expect_equal(fed.size(), logged.size());
    for (std::size_t index = 0; index != fed.size() && index != logged.size(); ++index) {
        stored_change<stored_key_t> const& got = fed[index].change;
        stored_change<stored_key_t> const& wanted = logged[index].change;
        expect_true(got.stamp == wanted.stamp && got.stored_key == wanted.stored_key && got.kind == wanted.kind);
        expect_equal(fed[index].counter, logged[index].counter);
    }
}

/** A feed pinned below a history of writes, removals and a range removal reads every version back
 *  in commit order across flushes and merges, which keep the versions above its stamp; the range
 *  removal comes back as the removal of every key it hid a value of, among its commit's writes by
 *  key, and not for a key its commit wrote again. */
template <typename mount_type_>
void test_feed_reads_across_flushes_and_merges(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    constexpr stored_key_t keys_k = 6, ranged_from_k = 3, ranged_to_k = 8, removed_k = 3, beside_k = 2;
    constexpr stored_key_t rewritten_k = 4, hidden_k = 5;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;
    collection_config_t config = test_config(8, 0);
    config.lsm_tree.level_enlarge_factor = storage_limits_t::level_min_enlarge_factor_k;
    auto const [fed_collection, create_status] = create_named(store, queues, "fed", config);
    expect_true(create_status.succeeded());
    if (create_status.failed()) return;
    any_collection_id_t const id {fed_collection};

    for (stored_key_t key = 0; key != keys_k; ++key) {
        std::array<util::byte_t, 8> const counter = counted(key + 100);
        expect_true(store.insert(id, key, stored_value_view_t {counter}, queues).succeeded());
    }
    transaction_stamp_t const since = store.ledger().published();
    typename database_t::pin_t pin;
    expect_true(store.pin_feed(id, pin, since).succeeded());

    st::vector<fed_change_t> logged;
    auto const log = [&](stored_key_t key, util::delta_kind_t kind, std::uint64_t counter) {
        stored_change<stored_key_t> const change {store.ledger().published(), key, kind};
        return expect_true(logged.push_back(fed_change_t {change, counter}) == st::status_t::success_k);
    };
    for (std::uint64_t round = 1; round != 3; ++round) {
        for (stored_key_t key = 0; key != keys_k; ++key) {
            std::array<util::byte_t, 8> const counter = counted(key + 100 * round);
            expect_true(store.insert(id, key, stored_value_view_t {counter}, queues).succeeded());
            log(key, util::delta_kind_t::write_k, key + 100 * round);
        }
        expect_true(store.flush(queues).succeeded());
    }
    expect_true(store.compact({}, queues).succeeded());
    expect_true(store.remove(id, removed_k, queues).succeeded());
    log(removed_k, util::delta_kind_t::remove_k, 0);

    auto [transaction, begin_status] = store.create_transaction();
    expect_true(begin_status.succeeded());
    if (begin_status.failed()) return;
    std::array<util::byte_t, 8> const beside = counted(2000);
    std::array<util::byte_t, 8> const rewritten = counted(4000);
    expect_true(transaction->insert(id, beside_k, stored_value_view_t {beside}).succeeded());
    expect_true(transaction->remove_range(id, ranged_from_k, ranged_to_k).succeeded());
    expect_true(transaction->insert(id, rewritten_k, stored_value_view_t {rewritten}).succeeded());
    expect_true(transaction->commit(queues).succeeded());
    log(beside_k, util::delta_kind_t::write_k, 2000);
    log(rewritten_k, util::delta_kind_t::write_k, 4000);
    log(hidden_k, util::delta_kind_t::remove_k, 0);
    transaction.reset();
    expect_true(store.flush(queues).succeeded());
    if (!expect_true(store.compact({}, queues).succeeded())) return;

    // Three at a time resumes inside the last commit, and a second pass rereads the same history.
    for (std::size_t wanted : {std::size_t {3}, std::size_t {1000}}) {
        st::vector<fed_change_t> fed;
        stored_change<stored_key_t> handed {since, (std::numeric_limits<stored_key_t>::max)()};
        pull_changes(store, queues, id, handed, wanted, fed);
        expect_fed(fed, logged);
    }
}

/** A feed keeps every version above its stamp out of a flush's reach until it is freed, while a
 *  second collection folds beside it; afterwards a flush folds the versions again, and a feed below
 *  the stamp it folded at is refused while one at the head opens. */
template <typename mount_type_>
void test_freed_feed_lets_merges_fold(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using database_t = typename types_t::database_t;
    using stack_t = typename types_t::stack_t;
    constexpr stored_key_t key_k = 1;
    constexpr std::uint64_t rewrites_k = 4;
    auto [stack, make_status] = make_stack(mount);
    expect_status(make_status, util::status_code_t::success_k);
    if (make_status.failed()) return;
    util::nvme_queues_ref_t const queues {&stack->fixture->queues, 0};
    database_t& store = *stack->store;
    auto const [folded, folded_status] = create_named(store, queues, "folded", test_config(8, 0));
    expect_true(folded_status.succeeded());
    if (folded_status.failed()) return;
    auto const [beside, beside_status] = create_named(store, queues, "beside", test_config(8, 0));
    expect_true(beside_status.succeeded());
    if (beside_status.failed()) return;
    any_collection_id_t const id {folded};
    auto const rewrite = [&](any_collection_id_t const& written_id, std::uint64_t first) {
        for (std::uint64_t round = first; round != first + rewrites_k; ++round) {
            std::array<util::byte_t, 8> const counter = counted(round);
            expect_true(store.insert(written_id, key_k, stored_value_view_t {counter}, queues).succeeded());
        }
    };

    rewrite(id, 0);
    transaction_stamp_t const since = store.ledger().published();
    typename database_t::pin_t pin;
    expect_true(store.pin_feed(id, pin, since).succeeded());
    rewrite(id, rewrites_k);
    expect_true(store.flush(queues).succeeded());
    std::uint64_t const pinned_population = store.population_count();
    // The version the head read at the feed's stamp, and every rewrite above it.
    expect_equal(pinned_population, rewrites_k + 1);
    rewrite(any_collection_id_t {beside}, 0);
    expect_true(store.flush(queues).succeeded());
    expect_equal(store.population_count(), pinned_population + 1);
    st::vector<fed_change_t> fed;
    stored_change<stored_key_t> handed {since, (std::numeric_limits<stored_key_t>::max)()};
    pull_changes(store, queues, id, handed, 2, fed);
    expect_equal(fed.size(), std::size_t {rewrites_k});

    pin.release();
    rewrite(id, 2 * rewrites_k);
    expect_true(store.flush(queues).succeeded());
    expect_equal(store.population_count(), pinned_population + 2);
    expect_status(store.pin_feed(id, pin, since), util::status_code_t::write_conflict_k);
    expect_true(!pin.held());
    expect_true(store.pin_feed(id, pin, store.ledger().published()).succeeded());
}

#pragma endregion Change Feed Cases

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
        report_on_mount(mount, "test_named_collections_round_trip", test_named_collections_round_trip);
        report_on_mount(mount, "test_queues_read_without_waiting", test_queues_read_without_waiting);
        report_on_mount(mount, "test_catalog_survives_reopen", test_catalog_survives_reopen);
        report_on_mount(mount, "test_strings_survive_reopen", test_strings_survive_reopen);
        report_on_mount(mount, "test_range_removal_survives_reopen", test_range_removal_survives_reopen);
        report_on_mount(mount, "test_strings_range_removal_survives_reopen",
                        test_strings_range_removal_survives_reopen);
        report_on_mount(mount, "test_dropped_collection_is_gone", test_dropped_collection_is_gone);
        report_on_mount(mount, "test_collections_list_by_stamp", test_collections_list_by_stamp);
        report_on_mount(mount, "test_crash_between_seal_and_schema", test_crash_between_seal_and_schema);
        report_on_mount(mount, "test_crash_keeps_a_prefix_of_commits", test_crash_keeps_a_prefix_of_commits);
        report_on_mount(mount, "test_merges_keep_what_the_flush_promises", test_merges_keep_what_the_flush_promises);
        report_on_mount(mount, "test_user_data_round_trips", test_user_data_round_trips);
        report_on_mount(mount, "test_archive_saves_incrementally_both_ways",
                        test_archive_saves_incrementally_both_ways);
        report_on_mount(mount, "test_commit_is_all_or_nothing", test_commit_is_all_or_nothing);
        report_on_mount(mount, "test_single_collection_commit_keeps_its_bytes",
                        test_single_collection_commit_keeps_its_bytes);
        report_on_mount(mount, "test_merged_and_flushed_values_read_back", test_merged_and_flushed_values_read_back);
        report_on_mount(mount, "test_conflict_leaves_the_other_collection_untouched",
                        test_conflict_leaves_the_other_collection_untouched);
        report_on_mount(mount, "test_rollback_and_renew_behave", test_rollback_and_renew_behave);
        report_on_mount(mount, "test_snapshot_holds_its_moment", test_snapshot_holds_its_moment);
        report_on_mount(mount, "test_runs_scan_one_at_a_time", test_runs_scan_one_at_a_time);
        report_on_mount(mount, "test_split_holds_its_first_entries", test_split_holds_its_first_entries);
        report_on_mount(mount, "test_head_reading_pins_nothing", test_head_reading_pins_nothing);
        report_on_mount(mount, "test_warm_batched_select_allocates_nothing",
                        test_warm_batched_select_allocates_nothing);
        report_on_mount(mount, "test_drop_proceeds_beside_the_snapshot", test_drop_proceeds_beside_the_snapshot);
        report_on_mount(mount, "test_threads_commit_serializably", test_threads_commit_serializably);
        report_on_mount(mount, "test_head_writes_wait_for_records", test_head_writes_wait_for_records);
        report_on_mount(mount, "test_strings_shares_wait_for_records", test_strings_shares_wait_for_records);
        report_on_mount(mount, "test_readers_beside_committers_and_flushes",
                        test_readers_beside_committers_and_flushes);
        report_on_mount(mount, "test_lookups_outrun_the_write_path", test_lookups_outrun_the_write_path);
        report_on_mount(mount, "test_feed_reads_across_flushes_and_merges", test_feed_reads_across_flushes_and_merges);
        report_on_mount(mount, "test_freed_feed_lets_merges_fold", test_freed_feed_lets_merges_fold);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(transport, directory.path(), database_config_k, 1, "database", cases);

    return finish_test_run();
}
