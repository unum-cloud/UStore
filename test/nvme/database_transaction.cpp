/**
 *  @file test/nvme/database_transaction.cpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Unit tests for transactions spanning several collections of a UStoreNVMe database.
 *
 *  Every case creates an inline, a fixed-width and a variable-width collection, writes a band of keys into them
 *  through one or two transactions, and checks what the database and each transaction answer afterwards. The
 *  suite builds as plain C++ over the device compactor wherever the build carries one.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <array>       // `std::array`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <thread>      // `std::thread`
#include <utility>     // `std::move`
#include <variant>     // `std::variant`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_database.hpp"     // `basic_database`
#include "ustore/blobs/nvme_transactions.hpp" // `watch_schedule_t`

#include "nvme/fixture.hpp" // `run_on_transport`, `painted`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Shape

/** The numbers every case mounts with: channels for a compaction to hold two, and one for every thread a case
 *  spawns. */
constexpr fixture_config_t transaction_config_k {.shared_io_channels = 8};

/** Keys every case writes into each collection. */
constexpr std::size_t population_k = 1000;

/** Every this many keys holds an empty value in the variable-width collection. */
constexpr std::size_t empty_value_step_k = 50;

/** Bytes every value of the fixed-width collection takes. */
constexpr stored_value_size_t fixed_width_k = 24;

/** Bytes the largest value of the variable-width collection takes. */
constexpr stored_value_size_t variable_width_max_k = 1000;

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
                                      last_write_fold_t, no_device_fold_t>;
    template <typename... store_types_>
    using basic_database_of = basic_database<stored_key_t, volume_t, atomic_reference, waiter_t, st::native_row_kit_t,
                                             layout_store_tags<store_types_...>, store_types_...>;
    using database_t = basic_database_of<
        blobs_store_of<value_layout_t::inline_12_bytes_k>, blobs_store_of<value_layout_t::inline_44_bytes_k>,
        blobs_store_of<value_layout_t::fixed_width_k>, blobs_store_of<value_layout_t::variable_width_k>>;
    using transaction_t = typename database_t::transaction_t;
    using transaction_unique_ptr_t = typename database_t::transaction_unique_ptr_t;

    /** A fixture, its volume, and the database mounted over the volume. */
    struct stack_t {

        /** The disk, the arena, the channels and the one volume. */
        std::unique_ptr<fixture_t> fixture;

        /** The volume of the database. */
        volume_t volume;

        /** The database, destroyed before the mount it borrows. */
        std::unique_ptr<database_t> store;
    };
};

/** The three collections every case creates, one per value layout a transaction groups its writes by. */
enum class shape_t : std::uint8_t {

    /** Values of one byte, kept inside the key slot. */
    inline_k = 0,

    /** Values of @c fixed_width_k bytes, in the stored values section. */
    fixed_k = 1,

    /** Values of any width up to @c variable_width_max_k, some of them empty. */
    variable_k = 2,
};

/** Every shape, in the order a case creates its collections. */
constexpr std::array<shape_t, 3> every_shape_k {shape_t::inline_k, shape_t::fixed_k, shape_t::variable_k};

/** The name the collection of @p shape is created under. */
[[nodiscard]] constexpr std::string_view name_of(shape_t shape) noexcept {
    switch (shape) {
    case shape_t::inline_k: return "inline";
    case shape_t::fixed_k: return "fixed_width";
    case shape_t::variable_k: return "variable_width";
    }
    return "";
}

/** Bytes the value of @p key takes in the collection of @p shape. */
[[nodiscard]] constexpr std::size_t width_of(shape_t shape, stored_key_t key) noexcept {
    switch (shape) {
    case shape_t::inline_k: return 1;
    case shape_t::fixed_k: return fixed_width_k;
    case shape_t::variable_k:
        return key % empty_value_step_k == empty_value_step_k - 1 ? 0 : 1 + (key * 37) % variable_width_max_k;
    }
    return 0;
}

/** The seed the value of @p key in the collection of @p shape is painted from, distinct for every pair. */
[[nodiscard]] constexpr std::uint64_t seed_of(shape_t shape, stored_key_t key) noexcept {
    return key * every_shape_k.size() + static_cast<std::uint64_t>(shape);
}

/** A buffer as wide as the widest value of any collection, which every write and every read reuses. */
using value_buffer_t = std::array<byte_t, variable_width_max_k>;

/** The value of @p key in the collection of @p shape, painted into @p buffer. */
[[nodiscard]] stored_value_view_t value_of(shape_t shape, stored_key_t key, value_buffer_t& buffer) noexcept {
    std::size_t const width = width_of(shape, key);
    for (std::size_t position = 0; position != width; ++position)
        buffer[position] = painted(seed_of(shape, key), position);
    return stored_value_view_t {buffer.data(), width};
}

/** Positions of @p bytes differing from the value of @p key in the collection of @p shape, counting from @p offset. */
[[nodiscard]] std::size_t mismatches_in(shape_t shape, stored_key_t key, std::uint64_t offset,
                                        std::span<byte_t const> bytes) noexcept {
    std::size_t mismatches = 0;
    for (std::size_t position = 0; position != bytes.size(); ++position)
        if (bytes[position] != painted(seed_of(shape, key), offset + position)) ++mismatches;
    return mismatches;
}

/** The collection shape a case creates for @p shape. */
[[nodiscard]] collection_config_t collection_config_of(shape_t shape) noexcept {
    collection_config_t config;
    config.concurrency_limit = 1;
    config.cache_limit = 1024ull * 1024ull;
    config.memory_runs_max_bytes = 1024ull * 1024ull;
    config.memory_runs_max_deltas = 4096;
    config.lsm_tree.level_enlarge_factor = 4;
    config.lsm_tree.lsm_level_0.sorted_run.value_size = shape == shape_t::inline_k  ? 1
                                                        : shape == shape_t::fixed_k ? fixed_width_k
                                                                                    : 0;
    config.lsm_tree.lsm_level_0.sorted_run.value_max_size = shape == shape_t::variable_k ? variable_width_max_k : 0;
    return config;
}

/** The transaction shape every case creates, which holds a whole case's writes. */
[[nodiscard]] database_config_t test_database_config() noexcept {
    database_config_t config;
    config.default_transaction.max_bytes = 16ull * 1024ull * 1024ull;
    config.default_transaction.max_deltas = 16384;
    return config;
}

/** The collection name @p text, owned, as a lookup by name takes it; a suite's literals always fit. */
[[nodiscard]] string_t owned_name(std::string_view text) noexcept {
    string_t owned;
    [[maybe_unused]] sz::status_t const assigned = owned.assign(string_view_t {text});
    return owned;
}

/** A collection ID minted for a case, which no other collection of any database carries. */
[[nodiscard]] collection_id_t minted_collection_id() noexcept {
    collection_id_t collection_id;
    std::memcpy(collection_id.uuid.bytes, uuid_generator().bytes, sizeof(collection_id.uuid.bytes));
    return collection_id;
}

/** The IDs of the three collections one case created, indexed by shape. */
struct collections_t {

    /** The ID of every collection, in the order of @c every_shape_k. */
    std::array<collection_id_t, 3> ids {collection_invalid_id_k, collection_invalid_id_k, collection_invalid_id_k};

    /** The collection of @p shape, as every call addresses one. */
    [[nodiscard]] any_collection_id_t operator[](shape_t shape) const noexcept {
        return any_collection_id_t {ids[static_cast<std::size_t>(shape)]};
    }
};

/** Whether a key takes part in a step of a case. */
using key_filter_t = bool (*)(stored_key_t) noexcept;

/** Every key. */
[[nodiscard]] bool every_key(stored_key_t) noexcept { return true; }

/** No key. */
[[nodiscard]] bool no_key(stored_key_t) noexcept { return false; }

/** The keys of the first half. */
[[nodiscard]] bool first_half(stored_key_t key) noexcept { return key < population_k / 2; }

/** The keys of the second half. */
[[nodiscard]] bool second_half(stored_key_t key) noexcept { return key >= population_k / 2; }

/** The keys of the second half and the last key of the first, so it overlaps @c first_half by one. */
[[nodiscard]] bool second_half_and_one(stored_key_t key) noexcept { return key + 1 >= population_k / 2; }

/** Writes the keys @p filter picks into the collection of @p shape through @p writer, a database or a transaction,
 *  watching them through @p watch_schedule_ when it is a transaction. */
template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k, typename writer_type_>
void insert_keys(writer_type_& writer, collections_t const& named, shape_t shape, key_filter_t filter) noexcept {
    value_buffer_t buffer {};
    for (stored_key_t key = 0; key != population_k; ++key) {
        if (!filter(key)) continue;
        stored_value_view_t const value = value_of(shape, key, buffer);
        if constexpr (watch_schedule_ == watch_schedule_t::default_k)
            expect_true(writer.insert(named[shape], key, value).succeeded());
        else expect_true(writer.template insert<watch_schedule_>(named[shape], key, value).succeeded());
    }
}

/** Removes the keys @p filter picks from the collection of @p shape through @p writer, watched as @c insert_keys
 *  watches them. */
template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k, typename writer_type_>
void remove_keys(writer_type_& writer, collections_t const& named, shape_t shape, key_filter_t filter) noexcept {
    for (stored_key_t key = 0; key != population_k; ++key) {
        if (!filter(key)) continue;
        if constexpr (watch_schedule_ == watch_schedule_t::default_k)
            expect_true(writer.remove(named[shape], key).succeeded());
        else expect_true(writer.template remove<watch_schedule_>(named[shape], key).succeeded());
    }
}

/** Checks that @p store carries exactly the keys @p filter picks in the collection of @p shape, key by key and in one
 *  batch, each with every byte of its value. */
template <typename database_type_>
void check_database(database_type_& store, collections_t const& named, shape_t shape, key_filter_t filter) {
    std::size_t mismatches = 0;
    value_buffer_t buffer {};
    for (stored_key_t key = 0; key != population_k; ++key) {
        stored_value_size_t size = 0;
        if (!filter(key)) {
            expect_true(!store.contains(named[shape], key));
            expect_true(store.stored_value_size(named[shape], key, size).failed());
            continue;
        }
        expect_true(store.contains(named[shape], key));
        expect_true(store.stored_value_size(named[shape], key, size).succeeded());
        expect_equal(std::size_t {size}, width_of(shape, key));
        expect_true(store.select(named[shape], key, std::span<byte_t> {buffer}, size).succeeded());
        expect_equal(std::size_t {size}, width_of(shape, key));
        mismatches += mismatches_in(shape, key, 0, std::span<byte_t const> {buffer.data(), size});
    }

    // One batch answers the same as the keys one at a time.
    st::vector<stored_key_t> keys;
    st::vector<lookup_result_t> results;
    st::vector<std::size_t> arrived;
    expect_true(keys.resize(population_k) == st::status_t::success_k);
    expect_true(results.resize(population_k) == st::status_t::success_k);
    if (!expect_true(arrived.resize(population_k) == st::status_t::success_k)) return;
    for (std::size_t index = 0; index != population_k; ++index) {
        keys[index] = static_cast<stored_key_t>(index);
        arrived[index] = 0;
    }
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        arrived[request] += bytes.size();
        mismatches += mismatches_in(shape, keys[request], offset, bytes);
    };
    expect_true(
        store.select(named[shape], std::span<stored_key_t const> {keys}, std::span<lookup_result_t> {results}, sink)
            .succeeded());
    for (std::size_t index = 0; index != population_k; ++index) {
        stored_key_t const key = keys[index];
        if (!filter(key)) {
            expect_true(results[index] != lookup_result_t::written_k);
            continue;
        }
        expect_true(results[index] == lookup_result_t::written_k);
        expect_equal(arrived[index], width_of(shape, key));
    }
    expect_equal(mismatches, std::size_t {0});
}

/** Checks that @p transaction sees exactly the keys @p filter picks in the collection of @p shape, reading each one
 *  through the watch schedule @p watch_schedule_. */
template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k, typename transaction_type_>
void check_transaction(transaction_type_& transaction, collections_t const& named, shape_t shape, key_filter_t filter) {
    std::size_t mismatches = 0;
    value_buffer_t buffer {};
    for (stored_key_t key = 0; key != population_k; ++key) {
        stored_value_size_t size = 0;
        if (!filter(key)) {
            expect_true(!transaction.contains(named[shape], key));
            continue;
        }
        expect_true(transaction.contains(named[shape], key));
        expect_true(transaction.stored_value_size(named[shape], key, size).succeeded());
        expect_equal(std::size_t {size}, width_of(shape, key));
        expect_true(transaction.template select<watch_schedule_>(named[shape], key, stored_value_span_t {buffer}, size)
                        .succeeded());
        expect_equal(std::size_t {size}, width_of(shape, key));
        mismatches += mismatches_in(shape, key, 0, std::span<byte_t const> {buffer.data(), size});
    }
    expect_equal(mismatches, std::size_t {0});
}

/** Checks every collection of @p store against one filter per shape, in the order of @c every_shape_k. */
template <typename database_type_>
void check_every_database(database_type_& store, collections_t const& named, key_filter_t inline_filter,
                          key_filter_t fixed_filter, key_filter_t variable_filter) {
    check_database(store, named, shape_t::inline_k, inline_filter);
    check_database(store, named, shape_t::fixed_k, fixed_filter);
    check_database(store, named, shape_t::variable_k, variable_filter);
}

/** Checks every collection @p transaction sees against one filter per shape, in the order of @c every_shape_k. */
template <typename transaction_type_>
void check_every_transaction(transaction_type_& transaction, collections_t const& named, key_filter_t inline_filter,
                             key_filter_t fixed_filter, key_filter_t variable_filter) {
    check_transaction(transaction, named, shape_t::inline_k, inline_filter);
    check_transaction(transaction, named, shape_t::fixed_k, fixed_filter);
    check_transaction(transaction, named, shape_t::variable_k, variable_filter);
}

#pragma endregion Shape

#pragma region Fixtures

/** Mounts a fixture through @p mount, mounts its volume and opens one database over it. */
template <typename mount_type_>
expected<std::unique_ptr<typename database_types<typename mount_type_::fixture_t>::stack_t>> make_stack(
    mount_type_ const& mount) noexcept {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using database_t = typename types_t::database_t;
    auto stack = std::unique_ptr<stack_t> {new (std::nothrow) stack_t {}};
    if (!stack) return status_t {status_code_t::out_of_memory_k, "The stack did not fit"};

    auto [fixture, mount_status] = mount();
    if (mount_status.failed()) return mount_status;
    stack->fixture = std::move(fixture);

    {
        auto [guard, acquire_status] = stack->fixture->channels.acquire();
        if (acquire_status.failed()) return acquire_status;
        auto [volume, mount_volume_status] = stack->fixture->mount_volume(guard.channel());
        if (mount_volume_status.failed()) return mount_volume_status;
        stack->volume = std::move(volume);
    }

    stack->store = std::unique_ptr<database_t> {new (std::nothrow) database_t {stack->fixture->resources}};
    if (!stack->store) return status_t {status_code_t::out_of_memory_k, "The database did not fit"};
    if (status_t const mounted = stack->store->mount(stack->volume, stack->fixture->channels, test_database_config());
        mounted.failed())
        return mounted;
    return stack;
}

/** Creates the three collections of every case in @p store, answering their IDs, and whether every
 *  one was created. */
template <typename database_type_>
bool create_collections(database_type_& store, collections_t& named) {
    bool created_all = true;
    for (shape_t const shape : every_shape_k) {
        collection_id_t const collection_id = minted_collection_id();
        std::string_view const name = name_of(shape);
        collection_config_t const config = collection_config_of(shape);
        value_layout_t const layout = resolve_value_layout(config.lsm_tree.lsm_level_0.sorted_run.value_size);
        status_t const created = store.create_collection(collection_id, string_view_t {name},
                                                         static_cast<std::uint16_t>(layout), config);
        created_all = expect_true(created.succeeded()) && created_all;
        named.ids[static_cast<std::size_t>(shape)] = collection_id;
    }
    return created_all;
}

#pragma endregion Fixtures

#pragma region Cases

/** A transaction commits and rolls back with nothing in it, and pins its start stamp only while it lives. */
template <typename mount_type_>
void test_create_transaction(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;

    expected<transaction_unique_ptr_t> first = store.create_transaction();
    expect_true(first.has_value());
    if (!first) return;
    (*first)->rollback();
    expect_true((*first)->commit().succeeded());
    expect_true((*first)->commit().succeeded());
    (*first)->rollback();

    // A live transaction pins the stamp it started at, and gives it back when it dies.
    expected<transaction_unique_ptr_t> second = store.create_transaction();
    expect_true(second.has_value());
    expect_equal(store.ledger().newest_reader(), store.ledger().published());
    first = status_t {status_code_t::cancelled_k};
    second = status_t {status_code_t::cancelled_k};
    expect_equal(store.ledger().newest_reader(), unwritten_stamp_k);
    expected<transaction_unique_ptr_t> again = store.create_transaction();
    expect_true(again.has_value());
}

/** Two empty transactions live side by side and commit, twice each. */
template <typename mount_type_>
void test_create_transactions(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;

    expected<transaction_unique_ptr_t> first = store.create_transaction();
    expected<transaction_unique_ptr_t> second = store.create_transaction();
    expect_true(first.has_value());
    if (!expect_true(second.has_value())) return;
    expect_true((*first)->commit().succeeded());
    expect_status((*second)->commit(), status_code_t::success_k);
    expect_true((*first)->commit().succeeded());
    expect_status((*second)->commit(), status_code_t::success_k);
}

/** Writes stay in the transaction until it commits, a rollback drops them, and a commit makes them visible. */
template <typename mount_type_>
void test_insert_by_one_transaction(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;
    collections_t named;
    create_collections(store, named);
    expected<transaction_unique_ptr_t> created = store.create_transaction();
    if (!expect_true(created.has_value())) return;
    auto& transaction = **created;

    for (shape_t const shape : every_shape_k) {
        std::string_view const name = name_of(shape);
        expect_true(transaction.has_collection(any_collection_id_t {owned_name(name)}));
    }

    // Written, but not yet committed.
    insert_keys(transaction, named, shape_t::inline_k, first_half);
    insert_keys(transaction, named, shape_t::fixed_k, first_half);
    check_every_database(store, named, no_key, no_key, no_key);
    check_transaction(transaction, named, shape_t::variable_k, no_key);

    expect_true(transaction.commit().succeeded());
    check_every_database(store, named, first_half, first_half, no_key);
    check_every_transaction(transaction, named, first_half, first_half, no_key);

    // The transaction sees its own second band over the committed first one, and the database does not.
    insert_keys(transaction, named, shape_t::inline_k, second_half);
    insert_keys(transaction, named, shape_t::fixed_k, second_half);
    insert_keys(transaction, named, shape_t::variable_k, every_key);
    check_every_database(store, named, first_half, first_half, no_key);
    check_every_transaction(transaction, named, every_key, every_key, every_key);

    transaction.rollback();
    check_every_database(store, named, first_half, first_half, no_key);
    check_every_transaction(transaction, named, first_half, first_half, no_key);

    insert_keys(transaction, named, shape_t::inline_k, second_half);
    insert_keys(transaction, named, shape_t::fixed_k, second_half);
    insert_keys(transaction, named, shape_t::variable_k, every_key);
    expect_true(transaction.commit().succeeded());
    check_every_database(store, named, every_key, every_key, every_key);
    check_every_transaction(transaction, named, every_key, every_key, every_key);

    // A collection the database never created takes no write.
    value_buffer_t buffer {};
    expect_true(transaction
                    .insert(any_collection_id_t {minted_collection_id()}, stored_key_t {0},
                            value_of(shape_t::variable_k, 0, buffer))
                    .failed());
}

/** Two transactions writing disjoint halves both commit, and the database holds both. */
template <typename mount_type_>
void test_insert_by_two_transactions(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;
    collections_t named;
    create_collections(store, named);
    expected<transaction_unique_ptr_t> first = store.create_transaction();
    expected<transaction_unique_ptr_t> second = store.create_transaction();
    expect_true(first.has_value());
    if (!expect_true(second.has_value())) return;

    check_every_database(store, named, no_key, no_key, no_key);
    for (shape_t const shape : every_shape_k) {
        insert_keys(**first, named, shape, first_half);
        insert_keys(**second, named, shape, second_half);
    }
    expect_true((*first)->commit().succeeded());
    expect_status((*second)->commit(), status_code_t::success_k);
    check_every_database(store, named, every_key, every_key, every_key);
}

/** Of two transactions writing overlapping bands, the second to commit conflicts and lands nothing. */
template <typename mount_type_>
void test_insert_by_two_intersected_transactions(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;
    collections_t named;
    create_collections(store, named);
    expected<transaction_unique_ptr_t> first = store.create_transaction();
    expected<transaction_unique_ptr_t> second = store.create_transaction();
    expect_true(first.has_value());
    if (!expect_true(second.has_value())) return;

    insert_keys(**first, named, shape_t::inline_k, first_half);
    insert_keys(**first, named, shape_t::fixed_k, every_key);
    insert_keys(**first, named, shape_t::variable_k, first_half);
    insert_keys(**second, named, shape_t::inline_k, second_half_and_one);
    insert_keys(**second, named, shape_t::variable_k, second_half);

    expect_true((*first)->commit().succeeded());
    expect_status((*second)->commit(), status_code_t::write_conflict_k);
    check_every_database(store, named, first_half, every_key, first_half);
}

/** A transaction's removals land with its commit and vanish with its rollback. */
template <typename mount_type_>
void test_remove_by_one_transaction(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;
    collections_t named;
    create_collections(store, named);
    expected<transaction_unique_ptr_t> created = store.create_transaction();
    if (!expect_true(created.has_value())) return;
    auto& transaction = **created;

    insert_keys(transaction, named, shape_t::inline_k, first_half);
    insert_keys(transaction, named, shape_t::fixed_k, every_key);
    insert_keys(transaction, named, shape_t::variable_k, first_half);
    expect_true(transaction.commit().succeeded());

    for (shape_t const shape : every_shape_k) remove_keys(transaction, named, shape, second_half);
    expect_true(transaction.commit().succeeded());
    check_every_database(store, named, first_half, first_half, first_half);

    for (shape_t const shape : every_shape_k) remove_keys(transaction, named, shape, first_half);
    transaction.rollback();
    check_every_database(store, named, first_half, first_half, first_half);
}

/** Two transactions removing disjoint halves both commit, and the database is left empty. */
template <typename mount_type_>
void test_remove_by_two_transactions(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;
    collections_t named;
    create_collections(store, named);
    // The population lands first, since a head write is a commit a transaction started before it would watch.
    for (shape_t const shape : every_shape_k) insert_keys(store, named, shape, every_key);
    expected<transaction_unique_ptr_t> first = store.create_transaction();
    expected<transaction_unique_ptr_t> second = store.create_transaction();
    expect_true(first.has_value());
    if (!expect_true(second.has_value())) return;

    for (shape_t const shape : every_shape_k) {
        remove_keys(**first, named, shape, first_half);
        remove_keys(**second, named, shape, second_half);
    }
    expect_true((*first)->commit().succeeded());
    expect_status((*second)->commit(), status_code_t::success_k);
    check_every_database(store, named, no_key, no_key, no_key);
}

/** Of two transactions removing overlapping bands, the second to commit conflicts and removes nothing. */
template <typename mount_type_>
void test_remove_by_two_intersected_transactions(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;
    collections_t named;
    create_collections(store, named);
    // The population lands first, since a head write is a commit a transaction started before it would watch.
    for (shape_t const shape : every_shape_k) insert_keys(store, named, shape, every_key);
    expected<transaction_unique_ptr_t> first = store.create_transaction();
    expected<transaction_unique_ptr_t> second = store.create_transaction();
    expect_true(first.has_value());
    if (!expect_true(second.has_value())) return;

    for (shape_t const shape : every_shape_k) remove_keys(**first, named, shape, first_half);
    remove_keys(**second, named, shape_t::inline_k, second_half);
    remove_keys(**second, named, shape_t::fixed_k, second_half);
    remove_keys(**second, named, shape_t::variable_k, second_half_and_one);

    expect_true((*first)->commit().succeeded());
    expect_status((*second)->commit(), status_code_t::write_conflict_k);
    check_every_database(store, named, second_half, second_half, second_half);
}

/** Two transactions on two threads, writing different collections, both commit and both land whole. */
template <typename mount_type_>
void test_insert_by_two_concurrent_transactions(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;
    collections_t named;
    if (!create_collections(store, named)) return;

    auto const writer = [&](std::span<shape_t const> shapes) noexcept {
        expected<transaction_unique_ptr_t> transaction = store.create_transaction();
        if (!expect_true(transaction.has_value())) return;
        for (shape_t const shape : shapes) insert_keys(**transaction, named, shape, every_key);
        expect_true((*transaction)->commit().succeeded());
    };
    std::array<shape_t, 1> const fixed_only {shape_t::fixed_k};
    std::array<shape_t, 2> const the_others {shape_t::inline_k, shape_t::variable_k};
    std::thread first {[&]() noexcept { writer(fixed_only); }};
    std::thread second {[&]() noexcept { writer(the_others); }};
    first.join();
    second.join();
    check_every_database(store, named, every_key, every_key, every_key);
}

/** Every watch schedule refuses or admits the commit it promises to, for writes, reads and explicit watches. */
template <typename mount_type_>
void test_watch_transactions(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;
    collections_t named;
    create_collections(store, named);
    for (shape_t const shape : every_shape_k) insert_keys(store, named, shape, every_key);
    expected<transaction_unique_ptr_t> first_created = store.create_transaction();
    expected<transaction_unique_ptr_t> second_created = store.create_transaction();
    expect_true(first_created.has_value());
    if (!expect_true(second_created.has_value())) return;
    auto& first = **first_created;
    auto& second = **second_created;
    value_buffer_t buffer {};
    std::array<stored_key_t, 1> const zero {0};

    // Watched writes overlapping on one key: the second to commit conflicts.
    for (stored_key_t key = 0; key != population_k / 2 + 1; ++key)
        expect_true(first.insert(named[shape_t::inline_k], key, value_of(shape_t::inline_k, key, buffer)).succeeded());
    for (stored_key_t key = population_k / 2; key != population_k; ++key)
        expect_true(second.insert(named[shape_t::inline_k], key, value_of(shape_t::inline_k, key, buffer)).succeeded());
    expect_true(first.commit().succeeded());
    expect_status(second.commit(), status_code_t::write_conflict_k);
    second.rollback();

    // A watched read of a key another transaction then writes and commits: the reader conflicts.
    expect_true(
        first.insert(named[shape_t::fixed_k], stored_key_t {0}, value_of(shape_t::fixed_k, 0, buffer)).succeeded());
    check_transaction(second, named, shape_t::fixed_k, every_key);
    expect_true(first.commit().succeeded());
    expect_status(second.commit(), status_code_t::write_conflict_k);
    second.rollback();

    // Unwatched writes overlapping on one key: both commit.
    for (stored_key_t key = 0; key != population_k / 2 + 1; ++key)
        expect_true(first
                        .template insert<watch_schedule_t::never_k>(named[shape_t::variable_k], key,
                                                                    value_of(shape_t::variable_k, key, buffer))
                        .succeeded());
    for (stored_key_t key = population_k / 2; key != population_k; ++key)
        expect_true(second
                        .template insert<watch_schedule_t::never_k>(named[shape_t::variable_k], key,
                                                                    value_of(shape_t::variable_k, key, buffer))
                        .succeeded());
    expect_true(first.commit().succeeded());
    expect_true(second.commit().succeeded());

    // An unwatched write beside an unwatched read: both commit.
    expect_true(first
                    .template insert<watch_schedule_t::never_k>(named[shape_t::variable_k], stored_key_t {0},
                                                                value_of(shape_t::variable_k, 0, buffer))
                    .succeeded());
    check_transaction<watch_schedule_t::never_k>(second, named, shape_t::variable_k, every_key);
    expect_true(first.commit().succeeded());
    expect_true(second.commit().succeeded());

    // An explicit watch checked now fails at once, and one checked on commit refuses the commit.
    second.rollback();
    expect_true(first.insert(named[shape_t::variable_k], stored_key_t {0}, value_of(shape_t::variable_k, 0, buffer))
                    .succeeded());
    expect_true(first.commit().succeeded());
    expect_true(second
                    .template watch_more<watch_schedule_t::now_k>(named[shape_t::variable_k],
                                                                  std::span<stored_key_t const> {zero})
                    .failed());
    expect_true(second
                    .template watch_more<watch_schedule_t::on_commit_k>(named[shape_t::variable_k],
                                                                        std::span<stored_key_t const> {zero})
                    .succeeded());
    expect_status(second.commit(), status_code_t::write_conflict_k);
    second.rollback();

    // A watch scoped from the operation ignores a commit that landed before it.
    expect_true(first.insert(named[shape_t::variable_k], stored_key_t {0}, value_of(shape_t::variable_k, 0, buffer))
                    .succeeded());
    expect_true(first.commit().succeeded());
    expect_true(second
                    .template watch_more<watch_schedule_t::on_commit_since_operation_k>(
                        named[shape_t::variable_k], std::span<stored_key_t const> {zero})
                    .succeeded());
    expect_true(second.commit().succeeded());

    // A read scoped from the operation pins the stamp it read at past a flush, and a later commit
    // of the key refuses.
    expect_true(store.insert(named[shape_t::variable_k], stored_key_t {0}, value_of(shape_t::variable_k, 1, buffer))
                    .succeeded());
    stored_value_size_t size = 0;
    expect_true(second
                    .template select<watch_schedule_t::on_commit_since_operation_k>(
                        named[shape_t::variable_k], stored_key_t {0}, stored_value_span_t {buffer}, size)
                    .succeeded());
    expect_equal(mismatches_in(shape_t::variable_k, 1, 0, std::span<byte_t const> {buffer.data(), size}),
                 std::size_t {0});
    transaction_stamp_t const pinned_stamp = store.ledger().published();
    expect_equal(store.ledger().newest_reader(), pinned_stamp);
    expect_true(store.insert(named[shape_t::variable_k], stored_key_t {0}, value_of(shape_t::variable_k, 0, buffer))
                    .succeeded());
    expect_true(store.flush().succeeded());
    expect_equal(store.ledger().newest_reader(), pinned_stamp);
    expect_status(second.commit(), status_code_t::write_conflict_k);
}

/** A staged transaction commits whole, a rolled-back one lands nothing, and a dropped one lands nothing either. */
template <typename mount_type_>
void test_stage_and_commit(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;
    collections_t named;
    create_collections(store, named);
    expected<transaction_unique_ptr_t> created = store.create_transaction();
    if (!expect_true(created.has_value())) return;

    for (shape_t const shape : every_shape_k) insert_keys(**created, named, shape, first_half);
    expect_true((*created)->stage().succeeded());
    expect_true((*created)->commit().succeeded());
    check_every_transaction(**created, named, first_half, first_half, first_half);

    for (shape_t const shape : every_shape_k) insert_keys(**created, named, shape, second_half);
    expect_true((*created)->stage().succeeded());
    (*created)->rollback();
    check_every_transaction(**created, named, first_half, first_half, first_half);

    for (shape_t const shape : every_shape_k) insert_keys(**created, named, shape, second_half);
    expect_true((*created)->stage().succeeded());
    created = status_t {status_code_t::cancelled_k};
    check_every_database(store, named, first_half, first_half, first_half);
}

/** Twenty transactions started together, each writing its own band of fifty keys with watches, all commit: validation
 *  is exact per key, so a thousand disjoint keys raise no conflict whatever lands beside them. */
template <typename mount_type_>
void test_disjoint_bands_never_conflict(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    constexpr std::size_t bands_k = 20;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;
    collections_t named;
    if (!create_collections(store, named)) return;
    // Every key is present first, so each band rewrites keys the others read nothing of.
    for (shape_t const shape : every_shape_k) insert_keys(store, named, shape, every_key);

    // Each transaction buffers its own band alone, so twenty of them fit the budget one whole case's shape takes.
    std::size_t const band_keys = population_k / bands_k;
    transaction_config_t band_shape;
    band_shape.max_bytes = band_keys * fixed_width_k;
    band_shape.max_deltas = band_keys;
    std::array<transaction_unique_ptr_t, bands_k> transactions;
    for (std::size_t band = 0; band != bands_k; ++band) {
        expected<transaction_unique_ptr_t> created = store.create_transaction(band_shape);
        expect_true(created.has_value());
        if (!created) return;
        transactions[band] = std::move(*created);
    }
    value_buffer_t buffer {};
    for (std::size_t band = 0; band != bands_k; ++band)
        for (stored_key_t key = band * band_keys; key != (band + 1) * band_keys; ++key) {
            stored_value_size_t size = 0;
            expect_true(transactions[band]
                            ->select(named[shape_t::fixed_k], key, stored_value_span_t {buffer}, size)
                            .succeeded());
            expect_true(transactions[band]
                            ->insert(named[shape_t::fixed_k], key, value_of(shape_t::fixed_k, key, buffer))
                            .succeeded());
        }
    std::size_t conflicts = 0;
    for (std::size_t band = 0; band != bands_k; ++band)
        conflicts += transactions[band]->commit() == status_code_t::write_conflict_k;
    expect_equal(conflicts, std::size_t {0});
    check_every_database(store, named, every_key, every_key, every_key);
}

/** A transaction reads what it started at, past a later commit of the key, a flush and a merge alike, and a watch on
 *  that key then refuses its commit. */
template <typename mount_type_>
void test_reads_at_start(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;
    collections_t named;
    create_collections(store, named);
    for (shape_t const shape : every_shape_k) insert_keys(store, named, shape, every_key);
    expected<transaction_unique_ptr_t> reader = store.create_transaction();
    if (!expect_true(reader.has_value())) return;

    // The later commit writes every fixed-width value one byte off, so the original reads back only from history.
    value_buffer_t buffer {};
    for (stored_key_t key = 0; key != population_k; ++key) {
        stored_value_view_t const value = value_of(shape_t::fixed_k, key + 1, buffer);
        expect_true(store.insert(named[shape_t::fixed_k], key, value).succeeded());
    }
    check_transaction(**reader, named, shape_t::fixed_k, every_key);
    expect_true(store.flush().succeeded());
    check_transaction(**reader, named, shape_t::fixed_k, every_key);
    for (std::size_t pass = 0; pass != 4 && store.needs_compaction(); ++pass) expect_true(store.compact().succeeded());
    check_transaction(**reader, named, shape_t::fixed_k, every_key);
    expect_status((*reader)->commit(), status_code_t::write_conflict_k);

    // Started again, it reads the head, and with the pin gone a merge keeps the newest version alone.
    (*reader)->rollback();
    std::size_t mismatches = 0;
    for (stored_key_t key = 0; key != population_k; ++key) {
        stored_value_size_t size = 0;
        expect_true((*reader)->select(named[shape_t::fixed_k], key, stored_value_span_t {buffer}, size).succeeded());
        mismatches += mismatches_in(shape_t::fixed_k, key + 1, 0, std::span<byte_t const> {buffer.data(), size});
    }
    expect_equal(mismatches, std::size_t {0});
}

/** A transaction that walked a range conflicts with a commit inserting a key it never found inside that range, and
 *  not with one landing outside it. */
template <typename mount_type_>
void test_range_watches_catch_phantoms(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;
    collections_t named;
    create_collections(store, named);
    insert_keys(store, named, shape_t::inline_k, first_half);
    expected<transaction_unique_ptr_t> walker = store.create_transaction();
    if (!expect_true(walker.has_value())) return;
    value_buffer_t buffer {};

    // A phantom inside the walked range refuses the walker, even with the key absent before.
    expect_true((*walker)->watch_range(named[shape_t::inline_k], stored_key_t {600}, stored_key_t {700}).succeeded());
    expect_true(store.insert(named[shape_t::inline_k], 650, value_of(shape_t::inline_k, 650, buffer)).succeeded());
    expect_true((*walker)->insert(named[shape_t::inline_k], 0, value_of(shape_t::inline_k, 0, buffer)).succeeded());
    expect_status((*walker)->commit(), status_code_t::write_conflict_k);
    (*walker)->rollback();

    // One landing past the range, and one after the flush that moved the range into a run, leave it alone.
    expect_true((*walker)->watch_range(named[shape_t::inline_k], stored_key_t {600}, stored_key_t {700}).succeeded());
    expect_true(store.flush().succeeded());
    expect_true(store.insert(named[shape_t::inline_k], 701, value_of(shape_t::inline_k, 701, buffer)).succeeded());
    expect_true((*walker)->insert(named[shape_t::inline_k], 0, value_of(shape_t::inline_k, 0, buffer)).succeeded());
    expect_true((*walker)->commit().succeeded());

    // A removal inside the range is a commit landing there too.
    expect_true((*walker)->watch_range(named[shape_t::inline_k], stored_key_t {600}, stored_key_t {700}).succeeded());
    expect_true(store.remove(named[shape_t::inline_k], 650).succeeded());
    expect_true((*walker)->insert(named[shape_t::inline_k], 0, value_of(shape_t::inline_k, 0, buffer)).succeeded());
    expect_status((*walker)->commit(), status_code_t::write_conflict_k);
}

/** A transaction watching a key for existence commits over a change delta folded onto the key
 *  since its start, and conflicts with a removal or a whole write of it, which a key watch would
 *  conflict with alike. */
template <typename mount_type_>
void test_existence_watches_pass_changes(mount_type_ const& mount) {
    using types_t = database_types<typename mount_type_::fixture_t>;
    using stack_t = typename types_t::stack_t;
    using transaction_unique_ptr_t = typename types_t::transaction_unique_ptr_t;
    expected<std::unique_ptr<stack_t>> stack = make_stack(mount);
    expect_status(stack, status_code_t::success_k);
    if (!stack) return;
    auto& store = *(*stack)->store;
    collections_t named;
    create_collections(store, named);
    insert_keys(store, named, shape_t::fixed_k, first_half);
    expect_true(store.flush().succeeded());
    any_collection_id_t const id = named[shape_t::fixed_k];
    expected<transaction_unique_ptr_t> watcher = store.create_transaction();
    expected<transaction_unique_ptr_t> changer = store.create_transaction();
    if (!expect_true(watcher.has_value() && changer.has_value())) return;
    value_buffer_t buffer {};
    stored_key_t const edited = 7;

    auto watch_and_commit = [&](auto&& meddle) {
        expect_true((*watcher)->watch_existence(id, std::span<stored_key_t const> {&edited, 1}).succeeded());
        meddle();
        expect_true((*watcher)->insert(id, 1, value_of(shape_t::fixed_k, 1, buffer)).succeeded());
        status_t const committed = (*watcher)->commit();
        if (committed.failed()) (*watcher)->rollback();
        return committed;
    };

    // A change delta committed since the watch leaves the key standing.
    status_t const changed_status = watch_and_commit([&] {
        expect_true(
            (*changer)->change(id, edited, value_of(shape_t::fixed_k, 2, buffer), last_write_fold_t {}).succeeded());
        expect_true((*changer)->commit().succeeded());
    });
    expect_true(changed_status.succeeded());

    // A whole write and a removal since the watch each refuse it.
    status_t const written_status = watch_and_commit(
        [&] { expect_true(store.insert(id, edited, value_of(shape_t::fixed_k, 3, buffer)).succeeded()); });
    expect_status(written_status, status_code_t::write_conflict_k);
    status_t const removed_status = watch_and_commit([&] { expect_true(store.remove(id, edited).succeeded()); });
    expect_status(removed_status, status_code_t::write_conflict_k);
}

#pragma endregion Cases

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
        report_on_mount(mount, "test_create_transaction", test_create_transaction);
        report_on_mount(mount, "test_create_transactions", test_create_transactions);
        report_on_mount(mount, "test_insert_by_one_transaction", test_insert_by_one_transaction);
        report_on_mount(mount, "test_insert_by_two_transactions", test_insert_by_two_transactions);
        report_on_mount(mount, "test_insert_by_two_intersected_transactions",
                        test_insert_by_two_intersected_transactions);
        report_on_mount(mount, "test_remove_by_one_transaction", test_remove_by_one_transaction);
        report_on_mount(mount, "test_remove_by_two_transactions", test_remove_by_two_transactions);
        report_on_mount(mount, "test_remove_by_two_intersected_transactions",
                        test_remove_by_two_intersected_transactions);
        report_on_mount(mount, "test_insert_by_two_concurrent_transactions",
                        test_insert_by_two_concurrent_transactions);
        report_on_mount(mount, "test_watch_transactions", test_watch_transactions);
        report_on_mount(mount, "test_stage_and_commit", test_stage_and_commit);
        report_on_mount(mount, "test_disjoint_bands_never_conflict", test_disjoint_bands_never_conflict);
        report_on_mount(mount, "test_reads_at_start", test_reads_at_start);
        report_on_mount(mount, "test_range_watches_catch_phantoms", test_range_watches_catch_phantoms);
        report_on_mount(mount, "test_existence_watches_pass_changes", test_existence_watches_pass_changes);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(transport, directory->path(), transaction_config_k, 1, "database_transaction", cases);

    return finish_test_run();
}
