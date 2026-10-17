/**
 *  @file test/nvme/transaction_registry.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief What the optimistic transaction layer promises: the conflicts it catches and the ones it invents.
 *
 *  The results this file exists for are @c serializable_under_threads and @c write_write_one_winner. A
 *  conflict record that misses a write turns two concurrent transactions into a lost update, which no
 *  single-threaded case can show, so the money-moving case drives real threads at overlapping keys and
 *  checks the one quantity a half-applied or lost commit cannot preserve - the total.
 *
 *  Nothing here touches a disk. The registry reaches its store through a handful of calls, so the store
 *  is a table of words in memory and every case runs at memory speed under both sanitizers.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <array>       // `std::array`
#include <atomic>      // `std::memory_order`
#include <memory>      // `std::make_shared`, `std::unique_ptr`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <thread>      // `std::thread`
#include <utility>     // `std::move`
#include <vector>      // `std::vector`

#include <forkunion/atomics.hpp> // `fu::preferred_atomic_ref`

#include "ustore/blobs/nvme_database.hpp" // `database_transaction_registry`

#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_eq`

namespace unum::ustore {

using namespace unum::ustore::blobs;
using namespace unum::ustore::test;
using namespace unum::ustore::utilities;

#pragma region Shape

/** The keys, and the values, every case of this suite moves. */
using stored_key_t = std::uint64_t;

/** The sole collection the multi-collection registry is driven over here. */
constexpr collection_id_t collection_k {uuid_from_integer(7)};

/** Bytes one value of this suite occupies, which is one word. */
constexpr std::size_t value_bytes_k = sizeof(std::uint64_t);

/** Keys the store admits, which is also the width of the money-moving case. */
constexpr std::size_t accounts_k = 64;

/** The balance every account starts at, so the total is known before a single transaction runs. */
constexpr std::uint64_t initial_balance_k = 1000;

/** Bytes one transaction's write buffer holds. */
constexpr std::size_t write_buffer_bytes_k = 64 * 1024;

/** Keys one transaction's write buffer holds. */
constexpr std::size_t write_buffer_elements_k = 1024;

/** Commit-record slots the cases build their registries with, wide enough that distinct keys part ways. */
constexpr std::size_t conflict_slots_k = 1u << 14;

/** Transactions one registry of this suite admits at once. */
constexpr std::size_t transaction_slots_k = 64;

/** What a store does with the writes a commit hands it, so a refusal can be asked for. */
enum class store_behaviour_t {

    /** Every write lands. */
    accepting_k,

    /** Every write is refused, which is what a commit that fails must survive. */
    refusing_applies_k,

    /** Every collection reports itself full and flushes, so a stage makes room before it commits. */
    always_flushing_k,

    /** Every collection reports itself full and the flush is refused, so the stage fails. */
    refusing_flushes_k,

    /** The first write waits for the suite, so a case can act with a commit caught mid-apply. */
    pausing_in_apply_k,
};

#pragma endregion Shape

#pragma region Store

/**
 *  @brief The narrowest store a registry can commit into: one collection of words, held in memory.
 *
 *  Balances move through atomic references because a transaction reads them outside the staging queue pair
 *  and writes them inside it, which is where the real collection layer keeps its own synchronization.
 */
class test_store_t {
  public:

    /** A store holding @c accounts_k balances, each at @c initial_balance_k. */
    test_store_t() noexcept {
        for (std::size_t index = 0; index != accounts_k; ++index) balances_[index] = initial_balance_k;
    }

    /** Takes the store's write path, which the registry holds for one staged transaction at a time. */
    void acquire_write_path() noexcept { ++write_path_depth_; }

    /** Hands the store's write path back. */
    void release_write_path() noexcept { --write_path_depth_; }

    /** Whether the collection @p group_id names still exists. */
    bool contains_collection(collection_id_t group_id) const noexcept { return group_id == collection_k; }

    /** Whether @p count values of @p bytes still fit in the collection's own write buffer. */
    bool collection_fits(collection_id_t, std::size_t, std::size_t) const noexcept {
        return behaviour != store_behaviour_t::always_flushing_k && behaviour != store_behaviour_t::refusing_flushes_k;
    }

    /** Empties the collection's write buffer onto its runs. */
    status_t flush_collection(collection_id_t) noexcept {
        ++flushes;
        if (behaviour == store_behaviour_t::refusing_flushes_k) return {status_code_t::device_k, "Refusing to flush"};
        return {};
    }

    /** Writes @p stored_value under @p stored_key. */
    status_t insert_in_collection(collection_id_t, stored_key_t stored_key, stored_value_view_t stored_value) noexcept {
        if (behaviour == store_behaviour_t::refusing_applies_k) return {status_code_t::device_k, "Refusing to write"};
        if (stored_key >= accounts_k) return {status_code_t::invalid_argument_k, "Key out of range"};
        if (behaviour == store_behaviour_t::pausing_in_apply_k) wait_for_the_suite_();
        std::uint64_t word = 0;
        if (stored_value.size() == value_bytes_k) std::memcpy(&word, stored_value.data(), value_bytes_k);
        balance_(stored_key).store(word, std::memory_order_release);
        return {};
    }

    /** Drops whatever @p stored_key held. */
    status_t remove_from_collection(collection_id_t, stored_key_t stored_key) noexcept {
        if (behaviour == store_behaviour_t::refusing_applies_k) return {status_code_t::device_k, "Refusing to write"};
        if (stored_key >= accounts_k) return {status_code_t::invalid_argument_k, "Key out of range"};
        balance_(stored_key).store(0, std::memory_order_release);
        return {};
    }

    /** The balance @p stored_key holds right now, which a later commit may already have moved. */
    std::uint64_t balance(stored_key_t stored_key) const noexcept {
        return balance_(stored_key).load(std::memory_order_acquire);
    }

    /** The balances of every account added up, which no serializable run ever changes. */
    std::uint64_t total() const noexcept {
        std::uint64_t sum = 0;
        for (std::size_t index = 0; index != accounts_k; ++index) sum += balance(index);
        return sum;
    }

    /** Whether a write is waiting inside the store, with its commit's stamp already claimed. */
    bool paused() const noexcept {
        return fu::preferred_atomic_ref<std::int32_t const> {paused_}.load(std::memory_order_acquire) != 0;
    }

    /** Lets the waiting write finish. */
    void resume() noexcept { fu::preferred_atomic_ref<std::int32_t> {resumed_}.store(1, std::memory_order_release); }

    /** What the store does with the writes a commit hands it. */
    store_behaviour_t behaviour = store_behaviour_t::accepting_k;

    /** The flushes the store was asked for, which a stage against a full collection raises. */
    std::size_t flushes = 0;

  private:

    /** Raises the flag and waits, so the suite can run while this commit is half applied. */
    void wait_for_the_suite_() noexcept {
        fu::preferred_atomic_ref<std::int32_t> {paused_}.store(1, std::memory_order_release);
        [[maybe_unused]] bool const woke = spin_until([this]() noexcept {
            return fu::preferred_atomic_ref<std::int32_t const> {resumed_}.load(std::memory_order_acquire) != 0;
        });
        fu::preferred_atomic_ref<std::int32_t> {paused_}.store(0, std::memory_order_release);
    }

    fu::preferred_atomic_ref<std::uint64_t> balance_(stored_key_t stored_key) noexcept {
        return fu::preferred_atomic_ref<std::uint64_t> {balances_[stored_key]};
    }

    fu::preferred_atomic_ref<std::uint64_t const> balance_(stored_key_t stored_key) const noexcept {
        return fu::preferred_atomic_ref<std::uint64_t const> {balances_[stored_key]};
    }

    /** One word per account, read outside the staging queue pair and written inside it. */
    std::uint64_t balances_[accounts_k] {};

    /** The write paths taken and not yet handed back, which the queue pair keeps at one. */
    std::size_t write_path_depth_ = 0;

    /** Raised while a write waits for the suite. */
    std::int32_t paused_ = 0;

    /** Raised by the suite to let a waiting write finish. */
    std::int32_t resumed_ = 0;
};

using registry_t = database_transaction_registry<stored_key_t, test_store_t>;

static_assert(transaction_store_concept<test_store_t, stored_key_t>, "The store must serve the commit path");

#pragma endregion Store

#pragma region Transaction

/**
 *  @brief The front end the suite drives the registry through, holding one transaction's own state.
 *
 *  Every call the registry expects from a front end appears here once, which is what the specialization
 *  is for: the contract is the list of members below, not a comment describing it.
 */
template <>
class blobs::database_transaction<stored_key_t, test_store_t> {
  public:
    using state_t = registry_t::transaction_state_t;
    using write_buffer_t = registry_t::write_buffer_t;

    /** What the transaction is allowed to do next. */
    enum class phase_t {

        /** Reading, writing and watching. */
        started_k,

        /** Staged, holding the store's write path until it commits or rolls back. */
        staged_k,
    };

    database_transaction(transaction_id_t id, registry_t& registry, write_buffer_t&& changes,
                         resources_shared_ptr_t resources) noexcept
        : id_(id), registry_(&registry), data_(std::move(changes), resources) {
        start_stamp_ = registry_->request_start_(id_);
    }

    database_transaction(database_transaction const&) = delete;
    database_transaction& operator=(database_transaction const&) = delete;

    ~database_transaction() noexcept { registry_->notify_about_deletion_(id_); }

    /** The stamp the transaction started at, which scopes every @c on_commit_k watch it takes. */
    transaction_stamp_t start_stamp() const noexcept { return start_stamp_; }

    /** The writes the transaction has not committed yet. */
    state_t const& state() const noexcept { return data_; }

    /** Watches @p stored_key on @p schedule for @p purpose, the way the real front end does. */
    template <watch_schedule_t watch_schedule_>
    status_t watch(stored_key_t stored_key, watch_purpose_t purpose) noexcept {
        std::array<stored_key_t, 1> const only {stored_key};
        status_t status = watch_now_or_prepare_for_later_<watch_schedule_>(only);
        if (status.failed()) return status;
        watch_later_<watch_schedule_>(only, watch_stamp_<watch_schedule_>(), purpose);
        return status;
    }

    /** Writes @p word under @p stored_key, watching the key on @p watch_schedule_ as a write. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::on_commit_k>
    status_t insert(stored_key_t stored_key, std::uint64_t word) noexcept {
        std::array<stored_key_t, 1> const only {stored_key};
        status_t status = watch_now_or_prepare_for_later_<watch_schedule_>(only);
        if (status.failed()) return status;

        transaction_stamp_t const watch_stamp = watch_stamp_<watch_schedule_>();
        std::byte payload[value_bytes_k];
        std::memcpy(payload, &word, value_bytes_k);
        status = data_.changes.add(collection_k, group_config_, stored_key,
                                   stored_value_view_t {payload, value_bytes_k});
        watch_later_<watch_schedule_>(only, watch_stamp, watch_purpose_t::write_k);
        return status;
    }

    /** Drops @p stored_key, watching the key on @p watch_schedule_ as a write. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::on_commit_k>
    status_t remove(stored_key_t stored_key) noexcept {
        std::array<stored_key_t, 1> const only {stored_key};
        status_t status = watch_now_or_prepare_for_later_<watch_schedule_>(only);
        if (status.failed()) return status;

        transaction_stamp_t const watch_stamp = watch_stamp_<watch_schedule_>();
        status = data_.changes.mark_as_removed(collection_k, group_config_, stored_key);
        watch_later_<watch_schedule_>(only, watch_stamp, watch_purpose_t::write_k);
        return status;
    }

    /** Checks the watched keys and makes room in the store, holding the write path once it answers. */
    status_t stage() noexcept {
        transaction_stamp_t staged_stamp = invalid_transaction_stamp_k;
        status_t const status = registry_->stage_(id_, data_, staged_stamp);
        if (status.succeeded()) phase_ = phase_t::staged_k;
        return status;
    }

    /** Stages unless already staged, then writes the changes into the store. */
    status_t commit() noexcept {
        status_t const status = registry_->commit_(id_, data_, last_commit_stamp_);
        phase_ = phase_t::started_k;
        if (status.succeeded()) {
            data_.clear();
            start_stamp_ = registry_->request_start_(id_);
        }
        return status;
    }

    /** Drops every change and every watch, leaving the registry with no trace of the transaction. */
    void rollback() noexcept {
        if (phase_ == phase_t::staged_k) registry_->discard_(id_);
        phase_ = phase_t::started_k;
        data_.clear();
        registry_->invalidate_(id_);
        start_stamp_ = registry_->request_start_(id_);
    }

    /** Starts over with the changes kept, which a caller retrying a conflict asks for. */
    void renew() noexcept {
        if (phase_ == phase_t::staged_k) registry_->discard_(id_);
        phase_ = phase_t::started_k;
        data_.read_set.clear();
        registry_->invalidate_(id_);
        start_stamp_ = registry_->request_start_(id_);
    }

    /** The stamp of the last commit this transaction attempted. */
    transaction_stamp_t last_commit_stamp() const noexcept { return last_commit_stamp_; }

  private:

    /** The widths the sole collection admits, which every write is checked against. */
    static status_t group_config_(collection_id_t, write_buffer_t::group_config_t& config) noexcept {
        config = {value_bytes_k, 0};
        return {};
    }

    /** The stamp a watch on @p watch_schedule_ is scoped from. */
    template <watch_schedule_t watch_schedule_>
    transaction_stamp_t watch_stamp_() const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return registry_->current_stamp_();
        else return start_stamp_;
    }

    /** Checks @p stored_keys now, or makes room to check them at the commit. */
    template <watch_schedule_t watch_schedule_>
    status_t watch_now_or_prepare_for_later_(std::span<stored_key_t const> stored_keys) noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::now_k)
            return registry_->watch_now_(start_stamp_, collection_k, stored_keys);
        else if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k ||
                           watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return data_.read_set.reserve_more(collection_k, stored_keys.size());
        else return status_t {};
    }

    /** Records @p stored_keys as watched from @p watch_stamp, in room the preparation took. */
    template <watch_schedule_t watch_schedule_>
    void watch_later_(std::span<stored_key_t const> stored_keys, transaction_stamp_t watch_stamp,
                      watch_purpose_t purpose) noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k ||
                      watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            for (stored_key_t const stored_key : stored_keys)
                data_.read_set.add(collection_k, stored_key, watch_stamp, purpose, st::assume_reserved);
    }

    /** The identifier the registry knows this transaction by. */
    transaction_id_t id_;

    /** The registry every stage and every commit goes through. */
    registry_t* registry_;

    /** The writes and the watches the transaction carries. */
    state_t data_;

    /** The stamp the current attempt started at. */
    transaction_stamp_t start_stamp_ = invalid_transaction_stamp_k;

    /** The stamp the last commit attempt took. */
    transaction_stamp_t last_commit_stamp_ = invalid_transaction_stamp_k;

    /** What the transaction is allowed to do next. */
    phase_t phase_ = phase_t::started_k;
};

using transaction_t = database_transaction<stored_key_t, test_store_t>;

#pragma endregion Transaction

#pragma region Fixture

/** A registry over a store of words, with the resources all three are charged to. */
struct fixture_t {

    /** The budget the registry, the record and every transaction are charged to. */
    resources_shared_ptr_t resources;

    /** The collection every commit of the fixture lands in. */
    test_store_t store;

    /** The registry under test. */
    registry_t registry;

    fixture_t() noexcept
        : resources(std::make_shared<resources_t>(1ull << 30, 0)), store(),
          registry(store, resources, conflict_slots_k, transaction_slots_k) {}

    /** A transaction of @p id, or nothing when the registry refuses it. */
    expected<registry_t::transaction_unique_ptr_t> open(transaction_id_t id) noexcept {
        transaction_config_t const config {write_buffer_bytes_k, write_buffer_elements_k};
        return registry.create_transaction(id, config);
    }
};

#pragma endregion Fixture

#pragma region Ordering Cases

/** The commit stamp is a counter, so no two commits of any thread count ever share one. */
static std::size_t commit_stamps_are_distinct() {
    std::size_t failures = 0;

    resource_constraint_t budget;
    commit_record_t record(conflict_slots_k, budget);

    constexpr std::size_t threads_k = 8;
    constexpr std::size_t per_thread_k = 20000;
    std::vector<std::vector<transaction_stamp_t>> claimed(threads_k);
    std::vector<std::thread> workers;
    workers.reserve(threads_k);
    for (std::size_t thread_index = 0; thread_index != threads_k; ++thread_index)
        workers.emplace_back([&, thread_index] {
            claimed[thread_index].reserve(per_thread_k);
            for (std::size_t issue = 0; issue != per_thread_k; ++issue)
                claimed[thread_index].push_back(record.issue_stamp());
        });
    for (std::thread& worker : workers) worker.join();

    std::vector<bool> seen(threads_k * per_thread_k + first_transaction_stamp_k + 1, false);
    std::size_t repeats = 0;
    transaction_stamp_t highest = unwritten_stamp_k;
    for (std::vector<transaction_stamp_t> const& batch : claimed)
        for (transaction_stamp_t const stamp : batch) {
            repeats += seen[stamp];
            seen[stamp] = true;
            if (stamp > highest) highest = stamp;
        }

    failures += expect_eq(repeats, std::size_t {0});
    failures += expect(highest == threads_k * per_thread_k + first_transaction_stamp_k - 1);

    // A watch is scoped from the published watermark, which no unpublished claim has moved.
    failures += expect(record.watch_stamp() == first_transaction_stamp_k);
    record.publish_stamp(highest);
    failures += expect(record.watch_stamp() == highest + 1);
    return failures;
}

/** An unwritten key conflicts with nobody, and a written one conflicts with every earlier watch. */
static std::size_t record_orders_writes_against_watches() {
    std::size_t failures = 0;

    resource_constraint_t budget;
    commit_record_t record(conflict_slots_k, budget);
    std::uint64_t const key = watched_key_fingerprint<stored_key_t>(collection_k, 11);

    transaction_stamp_t const watched_from = record.watch_stamp();
    failures += expect(!record.conflicts(key, watched_from));

    transaction_stamp_t const commit_stamp = record.issue_stamp();
    record.record(key, commit_stamp);
    failures += expect(record.conflicts(key, watched_from));

    // A watch taken after that commit has landed is not troubled by it.
    record.publish_stamp(commit_stamp);
    failures += expect(!record.conflicts(key, record.watch_stamp()));

    transaction_stamp_t const newer_stamp = record.issue_stamp();
    record.record(key, newer_stamp);
    record.publish_stamp(newer_stamp);

    // A commit arriving late never lowers what a newer one recorded, so the newer write stays seen.
    record.record(key, commit_stamp);
    failures += expect(record.conflicts(key, newer_stamp));
    failures += expect(!record.conflicts(key, record.watch_stamp()));
    return failures;
}

/** The roster hands one identifier to exactly one claimant, however many ask at once. */
static std::size_t roster_admits_one_claimant_per_identifier() {
    std::size_t failures = 0;

    resource_constraint_t budget;
    transaction_roster_t roster(transaction_slots_k, budget);

    constexpr std::size_t threads_k = 8;
    std::vector<std::size_t> enrolled(threads_k, 0);
    std::vector<std::thread> workers;
    workers.reserve(threads_k);
    for (std::size_t thread_index = 0; thread_index != threads_k; ++thread_index)
        workers.emplace_back([&, thread_index] {
            for (transaction_id_t id = 1; id <= 16; ++id) enrolled[thread_index] += roster.enroll(id).succeeded();
        });
    for (std::thread& worker : workers) worker.join();

    std::size_t winners = 0;
    for (std::size_t const count : enrolled) winners += count;
    failures += expect_eq(winners, std::size_t {16});
    failures += expect_eq(roster.live(), std::size_t {16});

    for (transaction_id_t id = 1; id <= 16; ++id) roster.retire(id);
    failures += expect_eq(roster.live(), std::size_t {0});
    return failures;
}

#pragma endregion Ordering Cases

#pragma region Conflict Cases

/** A key watched before another transaction wrote it refuses to stage. */
static std::size_t read_write_conflict_is_caught() {
    std::size_t failures = 0;
    fixture_t fixture;

    auto reader = fixture.open(1);
    auto writer = fixture.open(2);
    failures += expect(reader.has_value() && writer.has_value());
    if (failures != 0) return failures;

    failures += expect(reader->get()->watch<watch_schedule_t::on_commit_k>(5, watch_purpose_t::read_k).succeeded());
    failures += expect(writer->get()->insert(5, 42).succeeded());
    failures += expect(writer->get()->commit().succeeded());

    status_t const staged = reader->get()->stage();
    failures += expect(staged.failed());
    failures += expect_status(staged, status_code_t::write_conflict_k);
    reader->get()->rollback();
    return failures;
}

/** A key nobody else touched stages and commits. */
static std::size_t untouched_key_commits() {
    std::size_t failures = 0;
    fixture_t fixture;

    auto reader = fixture.open(1);
    auto writer = fixture.open(2);
    failures += expect(reader.has_value() && writer.has_value());
    if (failures != 0) return failures;

    failures += expect(reader->get()->watch<watch_schedule_t::on_commit_k>(5, watch_purpose_t::read_k).succeeded());
    failures += expect(writer->get()->insert(6, 42).succeeded());
    failures += expect(writer->get()->commit().succeeded());

    failures += expect(reader->get()->stage().succeeded());
    failures += expect(reader->get()->commit().succeeded());
    failures += expect_eq(fixture.store.balance(6), std::uint64_t {42});
    return failures;
}

/** Two transactions writing one key leave exactly one winner. */
static std::size_t write_write_one_winner() {
    std::size_t failures = 0;
    fixture_t fixture;

    auto first = fixture.open(1);
    auto second = fixture.open(2);
    failures += expect(first.has_value() && second.has_value());
    if (failures != 0) return failures;

    failures += expect(first->get()->insert(9, 111).succeeded());
    failures += expect(second->get()->insert(9, 222).succeeded());

    std::size_t winners = 0;
    winners += first->get()->commit().succeeded();
    winners += second->get()->commit().succeeded();
    failures += expect_eq(winners, std::size_t {1});
    failures += expect_eq(fixture.store.balance(9), std::uint64_t {111});

    // The loser's writes survive its refusal, which is what a caller logging them relies on.
    failures += expect_eq(second->get()->state().changes.count(), std::size_t {1});
    second->get()->rollback();
    return failures;
}

/** Each of the four schedules watches exactly what it says it does. */
static std::size_t watch_schedules_behave() {
    std::size_t failures = 0;

    // `never_k` ignores a collision outright.
    {
        fixture_t fixture;
        auto reader = fixture.open(1);
        auto writer = fixture.open(2);
        failures += expect(reader.has_value() && writer.has_value());
        if (failures == 0) {
            failures += expect(reader->get()->watch<watch_schedule_t::never_k>(5, watch_purpose_t::read_k).succeeded());
            failures += expect(writer->get()->insert(5, 1).succeeded());
            failures += expect(writer->get()->commit().succeeded());
            failures += expect(reader->get()->stage().succeeded());
            failures += expect_eq(reader->get()->state().read_set.count(), std::size_t {0});
            reader->get()->rollback();
        }
    }

    // `now_k` answers in place: clean before the commit, refused after it.
    {
        fixture_t fixture;
        auto reader = fixture.open(1);
        auto writer = fixture.open(2);
        failures += expect(reader.has_value() && writer.has_value());
        if (failures == 0) {
            failures += expect(reader->get()->watch<watch_schedule_t::now_k>(5, watch_purpose_t::read_k).succeeded());
            failures += expect(writer->get()->insert(5, 1).succeeded());
            failures += expect(writer->get()->commit().succeeded());
            status_t const late = reader->get()->watch<watch_schedule_t::now_k>(5, watch_purpose_t::read_k);
            failures += expect_status(late, status_code_t::write_conflict_k);
            // Nothing was recorded for later, so the stage itself has nothing to refuse.
            failures += expect_eq(reader->get()->state().read_set.count(), std::size_t {0});
            failures += expect(reader->get()->stage().succeeded());
            reader->get()->rollback();
        }
    }

    // `on_commit_k` is scoped from the transaction start, so a commit before the watch still counts.
    {
        fixture_t fixture;
        auto reader = fixture.open(1);
        auto writer = fixture.open(2);
        failures += expect(reader.has_value() && writer.has_value());
        if (failures == 0) {
            failures += expect(writer->get()->insert(5, 1).succeeded());
            failures += expect(writer->get()->commit().succeeded());
            failures += expect(
                reader->get()->watch<watch_schedule_t::on_commit_k>(5, watch_purpose_t::read_k).succeeded());
            status_t const staged = reader->get()->stage();
            failures += expect_status(staged, status_code_t::write_conflict_k);
            reader->get()->rollback();
        }
    }

    // `on_commit_since_operation_k` is scoped from the operation, so that same commit does not count.
    {
        fixture_t fixture;
        auto reader = fixture.open(1);
        auto writer = fixture.open(2);
        failures += expect(reader.has_value() && writer.has_value());
        if (failures == 0) {
            failures += expect(writer->get()->insert(5, 1).succeeded());
            failures += expect(writer->get()->commit().succeeded());
            failures += expect(reader->get()
                                   ->watch<watch_schedule_t::on_commit_since_operation_k>(5, watch_purpose_t::read_k)
                                   .succeeded());
            failures += expect(reader->get()->stage().succeeded());
            reader->get()->rollback();

            // A commit after the operation does count, which is the half that must still refuse.
            auto second_writer = fixture.open(3);
            failures += expect(second_writer.has_value());
            if (second_writer.has_value()) {
                failures += expect(
                    reader->get()
                        ->watch<watch_schedule_t::on_commit_since_operation_k>(5, watch_purpose_t::read_k)
                        .succeeded());
                failures += expect(second_writer->get()->insert(5, 2).succeeded());
                failures += expect(second_writer->get()->commit().succeeded());
                failures += expect_status(reader->get()->stage(), status_code_t::write_conflict_k);
                reader->get()->rollback();
            }
        }
    }

    return failures;
}

#pragma endregion Conflict Cases

#pragma region Lifecycle Cases

/** A commit the store refuses leaves the changes exactly as they were, so a caller can log them. */
static std::size_t failed_commit_keeps_changes() {
    std::size_t failures = 0;
    fixture_t fixture;

    auto transaction = fixture.open(1);
    auto reader = fixture.open(2);
    failures += expect(transaction.has_value() && reader.has_value());
    if (failures != 0) return failures;

    // The reader watches what the doomed commit is about to write, from before it runs.
    failures += expect(reader->get()->watch<watch_schedule_t::on_commit_k>(3, watch_purpose_t::read_k).succeeded());
    failures += expect(transaction->get()->insert(3, 7).succeeded());
    failures += expect(transaction->get()->insert(4, 8).succeeded());

    fixture.store.behaviour = store_behaviour_t::refusing_applies_k;
    failures += expect(transaction->get()->commit().failed());
    failures += expect_eq(transaction->get()->state().changes.count(), std::size_t {2});

    // The keys were published before the writes that never finished, so a reader spanning the
    // failure is refused rather than shown a batch that may have landed halfway.
    failures += expect_status(reader->get()->stage(), status_code_t::write_conflict_k);
    reader->get()->rollback();

    // Retrying is what renewal is for: the changes stay and only the watches and the stamp restart.
    transaction->get()->renew();
    failures += expect_eq(transaction->get()->state().changes.count(), std::size_t {2});

    // A refused stage leaves them too, which is the other half of the same promise.
    fixture.store.behaviour = store_behaviour_t::refusing_flushes_k;
    failures += expect_status(transaction->get()->stage(), status_code_t::device_k);
    failures += expect_eq(transaction->get()->state().changes.count(), std::size_t {2});

    // The refused stage handed the write path back, so the retry can still take it.
    fixture.store.behaviour = store_behaviour_t::accepting_k;
    failures += expect(transaction->get()->commit().succeeded());
    failures += expect_eq(fixture.store.balance(3), std::uint64_t {7});
    failures += expect_eq(fixture.store.balance(4), std::uint64_t {8});
    return failures;
}

/** A stage that cannot make room reports the refusal and hands the write path back. */
static std::size_t refused_flush_releases_the_channel() {
    std::size_t failures = 0;
    fixture_t fixture;
    fixture.store.behaviour = store_behaviour_t::always_flushing_k;

    auto transaction = fixture.open(1);
    failures += expect(transaction.has_value());
    if (failures != 0) return failures;
    failures += expect(transaction->get()->insert(3, 7).succeeded());

    failures += expect(transaction->get()->stage().succeeded());
    failures += expect_eq(fixture.store.flushes, std::size_t {1});
    failures += expect(transaction->get()->commit().succeeded());

    // The queue pair was handed back, so the next transaction can still take it.
    auto second = fixture.open(2);
    failures += expect(second.has_value());
    if (second.has_value()) {
        failures += expect(second->get()->insert(4, 8).succeeded());
        failures += expect(second->get()->commit().succeeded());
    }
    return failures;
}

/** Rollback and renewal leave the registry with no trace of the transaction. */
static std::size_t rollback_and_renew_leave_no_trace() {
    std::size_t failures = 0;
    fixture_t fixture;

    {
        auto transaction = fixture.open(1);
        failures += expect(transaction.has_value());
        if (failures != 0) return failures;

        failures += expect(transaction->get()->insert(3, 7).succeeded());
        failures += expect(transaction->get()->stage().succeeded());
        transaction->get()->rollback();
        failures += expect_eq(transaction->get()->state().changes.count(), std::size_t {0});
        failures += expect_eq(transaction->get()->state().read_set.count(), std::size_t {0});

        // Renewal keeps the changes and drops only the watches.
        failures += expect(transaction->get()->insert(3, 9).succeeded());
        failures += expect(transaction->get()->stage().succeeded());
        transaction->get()->renew();
        failures += expect_eq(transaction->get()->state().changes.count(), std::size_t {1});
        failures += expect_eq(transaction->get()->state().read_set.count(), std::size_t {0});
        failures += expect(transaction->get()->commit().succeeded());
        failures += expect_eq(fixture.store.balance(3), std::uint64_t {9});

        failures += expect(fixture.registry.has_transaction(1));
    }

    // The transaction is gone with its object, and so is its slot.
    failures += expect(!fixture.registry.has_transaction(1));
    return failures;
}

/** One identifier names one transaction, and a second request for it is refused. */
static std::size_t duplicate_identifier_is_refused() {
    std::size_t failures = 0;
    fixture_t fixture;

    auto first = fixture.open(1);
    failures += expect(first.has_value());
    auto second = fixture.open(1);
    failures += expect(second.failed());
    failures += expect_status(second, status_code_t::already_exists_k);
    return failures;
}

/** The registry refuses a transaction past the slots it was built with, rather than growing. */
static std::size_t roster_refuses_past_its_slots() {
    std::size_t failures = 0;
    resources_shared_ptr_t resources = std::make_shared<resources_t>(1ull << 30, 0);
    test_store_t store;
    registry_t registry(store, resources, conflict_slots_k, 2);

    transaction_config_t const config {write_buffer_bytes_k, write_buffer_elements_k};
    auto first = registry.create_transaction(1, config);
    auto second = registry.create_transaction(2, config);
    auto third = registry.create_transaction(3, config);
    failures += expect(first.has_value() && second.has_value());
    failures += expect(third.failed());
    failures += expect_status(third, status_code_t::capacity_exhausted_k);
    return failures;
}

/** The committed-key record is a fixed table, so a long transaction beside many commits never grows it. */
static std::size_t commit_record_is_bounded() {
    std::size_t failures = 0;
    fixture_t fixture;

    auto lingering = fixture.open(1);
    failures += expect(lingering.has_value());
    if (failures != 0) return failures;
    // The watched key is written by the very first of the commits below, and by nothing after it.
    failures += expect(lingering->get()->watch<watch_schedule_t::on_commit_k>(1, watch_purpose_t::read_k).succeeded());

    std::size_t const before = fixture.registry.shared_state_bytes();
    for (std::size_t round = 0; round != 20000; ++round) {
        auto writer = fixture.open(2);
        if (!writer.has_value()) {
            failures += expect(false);
            break;
        }
        failures += expect(writer->get()->insert(round == 0 ? 1 : 2 + round % (accounts_k - 2), round).succeeded());
        if (writer->get()->commit().failed()) failures += expect(false);
    }
    std::size_t const after = fixture.registry.shared_state_bytes();

    failures += expect_eq(after, before);
    failures += expect_eq(after, conflict_slots_k * sizeof(transaction_stamp_t) + transaction_slots_k * 16);

    // Twenty thousand commits later the very first of them is still remembered, so bounding the
    // record cost nothing of the conflict it exists for.
    failures += expect_status(lingering->get()->stage(), status_code_t::write_conflict_k);
    lingering->get()->rollback();
    return failures;
}

#pragma endregion Lifecycle Cases

#pragma region Concurrency Cases

/**
 *  @brief Threads moving money between overlapping accounts, checked against the one total.
 *
 *  A lost update leaves the total short or long, and a half-applied commit leaves it short by the
 *  half that landed, so the sum is the whole result. Every transaction reads two balances outside
 *  the staging queue pair and writes both inside it, which is the shape a conflict record has to survive.
 */
static std::size_t serializable_under_threads() {
    std::size_t failures = 0;
    fixture_t fixture;

    constexpr std::size_t threads_k = 8;
    constexpr std::size_t transfers_k = 300;
    constexpr std::uint64_t amount_k = 1;

    std::uint64_t const total_before = fixture.store.total();
    std::vector<std::size_t> committed(threads_k, 0);
    std::vector<std::size_t> refused(threads_k, 0);
    std::vector<std::thread> workers;
    workers.reserve(threads_k);

    for (std::size_t thread_index = 0; thread_index != threads_k; ++thread_index)
        workers.emplace_back([&, thread_index] {
            auto opened = fixture.open(static_cast<transaction_id_t>(thread_index + 1));
            if (!opened.has_value()) return;
            transaction_t& transaction = *opened->get();

            std::uint64_t scramble = thread_index * 0x9e3779b97f4a7c15ull + 1;
            for (std::size_t transfer = 0; transfer != transfers_k; ++transfer) {
                scramble ^= scramble << 13;
                scramble ^= scramble >> 7;
                scramble ^= scramble << 17;
                stored_key_t const from = scramble % accounts_k;
                stored_key_t const to = (scramble / accounts_k) % accounts_k;
                if (from == to) continue;

                // The read is watched from the transaction start, so any commit in between refuses it.
                if (transaction.watch<watch_schedule_t::on_commit_k>(from, watch_purpose_t::read_k).failed()) continue;
                if (transaction.watch<watch_schedule_t::on_commit_k>(to, watch_purpose_t::read_k).failed()) continue;
                std::uint64_t const from_balance = fixture.store.balance(from);
                std::uint64_t const to_balance = fixture.store.balance(to);
                if (from_balance < amount_k) {
                    transaction.rollback();
                    continue;
                }

                if (transaction.insert(from, from_balance - amount_k).failed() ||
                    transaction.insert(to, to_balance + amount_k).failed()) {
                    transaction.rollback();
                    continue;
                }

                if (transaction.commit().succeeded()) ++committed[thread_index];
                else {
                    ++refused[thread_index];
                    transaction.rollback();
                }
            }
        });
    for (std::thread& worker : workers) worker.join();

    std::size_t commits = 0;
    std::size_t refusals = 0;
    for (std::size_t index = 0; index != threads_k; ++index) {
        commits += committed[index];
        refusals += refused[index];
    }

    failures += expect_eq(fixture.store.total(), total_before);
    failures += expect(commits + refusals > 0);

    // A record that refused everything would pass the total while proving nothing, so it must not.
    failures += expect(commits > 0);
    return failures;
}

/**
 *  @brief A transaction starting while a commit is half applied orders before it, not after it.
 *
 *  This is the ordering the money-moving case can only catch by luck, so it is driven here by hand: a
 *  commit is held inside the store with its stamp claimed and its keys recorded but its writes not yet
 *  landed, a reader starts in exactly that window and reads the stale balance, and the stage that
 *  follows has to refuse it. Opening the watermark before the writes land instead of after lets that
 *  reader through with a value that was never committed.
 */
static std::size_t start_during_apply_still_conflicts() {
    std::size_t failures = 0;
    fixture_t fixture;
    fixture.store.behaviour = store_behaviour_t::pausing_in_apply_k;

    auto writer = fixture.open(1);
    failures += expect(writer.has_value());
    if (failures != 0) return failures;

    std::size_t committed = 0;
    failures += expect(writer->get()->insert(5, 77).succeeded());
    std::thread committing([&] { committed = writer->get()->commit().succeeded(); });

    // The commit is now past its stamp and past its record, and has not written a byte.
    failures += expect(spin_until([&] { return fixture.store.paused(); }));

    auto reader = fixture.open(2);
    failures += expect(reader.has_value());
    std::uint64_t seen = 0;
    if (reader.has_value()) {
        failures += expect(reader->get()->watch<watch_schedule_t::on_commit_k>(5, watch_purpose_t::read_k).succeeded());
        seen = fixture.store.balance(5);
    }

    fixture.store.resume();
    committing.join();

    failures += expect_eq(committed, std::size_t {1});
    failures += expect_eq(fixture.store.balance(5), std::uint64_t {77});

    // The reader really did read the value the commit was about to replace, and must be refused for it.
    failures += expect_eq(seen, initial_balance_k);
    if (reader.has_value()) {
        fixture.store.behaviour = store_behaviour_t::accepting_k;
        failures += expect_status(reader->get()->stage(), status_code_t::write_conflict_k);
        reader->get()->rollback();
    }
    return failures;
}

/** Threads opening and closing transactions against one roster leave it empty and unbroken. */
static std::size_t roster_survives_churn() {
    std::size_t failures = 0;
    fixture_t fixture;

    constexpr std::size_t threads_k = 8;
    constexpr std::size_t rounds_k = 400;
    std::vector<std::size_t> opened(threads_k, 0);
    std::vector<std::thread> workers;
    workers.reserve(threads_k);

    for (std::size_t thread_index = 0; thread_index != threads_k; ++thread_index)
        workers.emplace_back([&, thread_index] {
            for (std::size_t round = 0; round != rounds_k; ++round) {
                auto transaction = fixture.open(static_cast<transaction_id_t>(thread_index * rounds_k + round + 1));
                if (!transaction.has_value()) continue;
                ++opened[thread_index];
                if (transaction->get()->insert(round % accounts_k, round).succeeded()) [[maybe_unused]]
                    status_t const committed = transaction->get()->commit();
            }
        });
    for (std::thread& worker : workers) worker.join();

    std::size_t total_opened = 0;
    for (std::size_t const count : opened) total_opened += count;
    failures += expect(total_opened > 0);
    failures += expect(!fixture.registry.has_transaction(1));
    return failures;
}

#pragma endregion Concurrency Cases

} // namespace unum::ustore

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;
    using namespace unum::ustore::blobs;
    using namespace unum::ustore::utilities;

    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    report(totals, "stamps.distinct", commit_stamps_are_distinct);
    report(totals, "record.orders_writes", record_orders_writes_against_watches);
    report(totals, "roster.one_claimant", roster_admits_one_claimant_per_identifier);
    report(totals, "conflict.read_write", read_write_conflict_is_caught);
    report(totals, "conflict.untouched_commits", untouched_key_commits);
    report(totals, "conflict.write_write_one_winner", write_write_one_winner);
    report(totals, "conflict.watch_schedules", watch_schedules_behave);
    report(totals, "lifecycle.failed_commit_keeps", failed_commit_keeps_changes);
    report(totals, "lifecycle.flush_releases_channel", refused_flush_releases_the_channel);
    report(totals, "lifecycle.rollback_and_renew", rollback_and_renew_leave_no_trace);
    report(totals, "lifecycle.duplicate_id", duplicate_identifier_is_refused);
    report(totals, "lifecycle.roster_refuses", roster_refuses_past_its_slots);
    report(totals, "lifecycle.record_bounded", commit_record_is_bounded);
    report(totals, "threads.serializable", serializable_under_threads);
    report(totals, "threads.start_during_apply", start_during_apply_still_conflicts);
    report(totals, "threads.roster_churn", roster_survives_churn);

    return suite_exit_code(totals);
}
