/**
 *  @file test/nvme/transaction_registry.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief What the optimistic transaction layer promises: the conflicts it catches, and that it invents none.
 *
 *  The results this file exists for are @c serializable_under_threads, @c write_write_one_winner and
 *  @c disjoint_transactions_never_conflict. A missed write turns two concurrent transactions into a lost update,
 *  which no single-threaded case can show, so the money-moving case drives real threads at overlapping keys and
 *  checks the one quantity a half-applied or lost commit cannot preserve - the total. An invented conflict is the
 *  opposite failure, so two transactions writing a thousand keys each must both land.
 *
 *  Nothing here touches a disk. The registry reaches its store through a handful of calls, so the store is a
 *  table of words and stamps in memory and every case runs at memory speed under both sanitizers.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <array>   // `std::array`
#include <atomic>  // `std::memory_order`
#include <memory>  // `std::make_shared`, `std::unique_ptr`
#include <span>    // `std::span`
#include <thread>  // `std::thread`
#include <utility> // `std::move`
#include <vector>  // `std::vector`

#include <forkunion/atomics.hpp> // `fu::standard_atomic_ref`
#include <forkunion/types.hpp>   // `fu::standard_yield_t`

#include "ustore/blobs/nvme_database.hpp" // `database_transaction_registry`, `commit_ledger`

#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_eq`

namespace unum::ustore {

using namespace unum::ustore::blobs;
using namespace unum::ustore::test;
using namespace unum::ustore::utilities;

#pragma region Shape

/** The keys, and the values, every case of this suite moves. */
using stored_key_t = std::uint64_t;

/** The atomic reference the registry and the store are instantiated with: the portable one. */
template <typename value_type_>
using atomic_reference = fu::standard_atomic_ref<value_type_>;

/** The waiter the registry and the store are instantiated with, yielding between attempts. */
using waiter_t = fu::standard_yield_t;

using commit_ledger_t = commit_ledger<atomic_reference, waiter_t>;
using stamp_pin_t = stamp_pin<atomic_reference, waiter_t>;
using resource_constraint_t = resource_constraint<atomic_reference>;
using resources_t = shared_resources<atomic_reference, waiter_t>;
using resources_shared_ptr_t = resources_shared_ptr<atomic_reference, waiter_t>;

/** The sole collection the multi-collection registry is driven over here. */
constexpr collection_id_t collection_k {uuid_from_integer(7)};

/** Bytes one value of this suite occupies, which is one word. */
constexpr std::size_t value_bytes_k = sizeof(std::uint64_t);

/** Keys the store admits: wide enough for two disjoint bands of a thousand keys, and the money-moving accounts. */
constexpr std::size_t accounts_k = 4096;

/** Accounts the money-moving case draws from, few enough that its transfers overlap. */
constexpr std::size_t contended_accounts_k = 64;

/** The balance every account starts at, so the total is known before a single transaction runs. */
constexpr std::uint64_t initial_balance_k = 1000;

/** Bytes one transaction's deltas hold. */
constexpr std::size_t deltas_bytes_k = 64 * 1024;

/** Keys one transaction's deltas hold. */
constexpr std::size_t deltas_max_count_k = 2048;

/** What a store does with the writes a commit hands it, so a refusal can be asked for. */
enum class store_behaviour_t {

    /** Every write lands. */
    accepting_k,

    /** Every collection refuses the room a commit asks for, so the commit fails before it draws a stamp. */
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
 *  @brief The narrowest store a registry can commit into: one collection of words, each beside the stamp of the
 *      commit that last wrote it, held in memory.
 *
 *  Balances move through atomic references because a transaction reads them outside the write path and writes
 *  them inside it, which is where the real collection layer keeps its own synchronization.
 */
class test_store_t {
  public:

    /** A store holding @c accounts_k balances, each at @c initial_balance_k and written by no commit. */
    test_store_t() noexcept {
        for (std::size_t index = 0; index != accounts_k; ++index) balances_[index] = initial_balance_k;
    }

    /** The order every commit into this store draws its stamp from. */
    commit_ledger_t& ledger() noexcept { return ledger_; }

    /** Whether a commit newer than @p from wrote @p stored_key, exactly. */
    bool written_since(collection_id_t, stored_key_t stored_key, transaction_stamp_t from) const noexcept {
        return stamps_[stored_key] > from;
    }

    /** Whether a commit since @p from wrote any account from @p lower to @p upper inclusive. */
    bool range_written_since(collection_id_t, stored_key_t lower, stored_key_t upper,
                             transaction_stamp_t from) const noexcept {
        for (stored_key_t account = lower; account <= upper && account < accounts_k; ++account)
            if (stamps_[account] > from) return true;
        return false;
    }

    /** Seals @p written for the commit that follows, flushing the collection first when it reports itself full. */
    status_t seal_collection(collection_id_t collection_id,
                             transaction_deltas<stored_key_t, atomic_reference, waiter_t> const& written,
                             sealed_commit<stored_key_t, atomic_reference>& sealed) noexcept {
        if (behaviour == store_behaviour_t::always_flushing_k || behaviour == store_behaviour_t::refusing_flushes_k) {
            ++flushes;
            if (behaviour == store_behaviour_t::refusing_flushes_k)
                return {status_code_t::device_k, "Refusing to flush"};
        }
        st::basic_vector<keyed_delta<stored_key_t>> deltas;
        written.for_each_delta(collection_id, [&](stored_key_t stored_key, delta_view_t delta) noexcept {
            [[maybe_unused]] st::status_t const kept = deltas.push_back(keyed_delta<stored_key_t> {stored_key, delta});
        });
        if (deltas.empty()) return {};
        auto [sealed_deltas, seal_status] =
            blobs::sealed_deltas<stored_key_t, constrained_allocator<byte_t, atomic_reference>>::seal(
                std::span<keyed_delta<stored_key_t>> {deltas},
                constrained_allocator<byte_t, atomic_reference> {budget_});
        if (seal_status.failed()) return seal_status;
        sealed.deltas = std::move(sealed_deltas);
        return {};
    }

    /** Takes the sealed changes for the commit that follows, or refuses them. */
    status_t prepare_collection(collection_id_t, sealed_commit<stored_key_t, atomic_reference>&) noexcept {
        if (behaviour == store_behaviour_t::refusing_applies_k) return {status_code_t::device_k, "Refusing to write"};
        return {};
    }

    /** Writes every sealed change at @p stamp, a removal as a zero balance. */
    void publish_collection(collection_id_t, sealed_commit<stored_key_t, atomic_reference>& sealed,
                            transaction_stamp_t stamp) noexcept {
        for (std::size_t position = 0; position != sealed.deltas.size(); ++position) {
            if (behaviour == store_behaviour_t::pausing_in_apply_k) wait_for_the_suite_();
            stored_key_t const stored_key = sealed.deltas.stored_key(position);
            delta_view_t const delta = sealed.deltas.delta(position);
            std::uint64_t word = 0;
            if (delta.kind == delta_kind_t::write_k && delta.bytes.size() == value_bytes_k)
                std::memcpy(&word, delta.bytes.data(), value_bytes_k);
            stamps_[stored_key] = stamp;
            balance_(stored_key).store(word, std::memory_order_release);
        }
    }

    /** Forgets nothing, since preparing takes nothing. */
    void forget_collection(collection_id_t) noexcept {}

    /** Maintains nothing, since balances live in place. */
    status_t maintain(collection_id_t) noexcept { return {}; }

    /** The balance @p stored_key holds right now, which a later commit may already have moved. */
    std::uint64_t balance(stored_key_t stored_key) const noexcept {
        return balance_(stored_key).load(std::memory_order_acquire);
    }

    /** The balances of every contended account added up, which no serializable run ever changes. */
    std::uint64_t total() const noexcept {
        std::uint64_t sum = 0;
        for (std::size_t index = 0; index != contended_accounts_k; ++index) sum += balance(index);
        return sum;
    }

    /** Whether a write is waiting inside the store, with its commit's stamp already drawn. */
    bool paused() const noexcept {
        return atomic_reference<std::int32_t const> {paused_}.load(std::memory_order_acquire) != 0;
    }

    /** Lets the waiting write finish. */
    void resume() noexcept { atomic_reference<std::int32_t> {resumed_}.store(1, std::memory_order_release); }

    /** What the store does with the writes a commit hands it. */
    store_behaviour_t behaviour = store_behaviour_t::accepting_k;

    /** The flushes the store was asked for, which a stage against a full collection raises. */
    std::size_t flushes = 0;

  private:

    /** Raises the flag and waits, so the suite can run while this commit is half applied. */
    void wait_for_the_suite_() noexcept {
        atomic_reference<std::int32_t> {paused_}.store(1, std::memory_order_release);
        [[maybe_unused]] bool const woke = spin_until([this]() noexcept {
            return atomic_reference<std::int32_t const> {resumed_}.load(std::memory_order_acquire) != 0;
        });
        atomic_reference<std::int32_t> {paused_}.store(0, std::memory_order_release);
    }

    atomic_reference<std::uint64_t> balance_(stored_key_t stored_key) noexcept {
        return atomic_reference<std::uint64_t> {balances_[stored_key]};
    }

    atomic_reference<std::uint64_t const> balance_(stored_key_t stored_key) const noexcept {
        return atomic_reference<std::uint64_t const> {balances_[stored_key]};
    }

    /** The stamps every commit draws. */
    commit_ledger_t ledger_;

    /** The budget sealed changes are charged to. */
    resource_constraint_t budget_ {std::size_t {1} << 30};

    /** One word per account, read by any thread and written by the leader. */
    std::uint64_t balances_[accounts_k] {};

    /** The stamp of the commit that last wrote each account, read and written by the leader. */
    transaction_stamp_t stamps_[accounts_k] {};

    /** Raised while a write waits for the suite. */
    std::int32_t paused_ = 0;

    /** Raised by the suite to let a waiting write finish. */
    std::int32_t resumed_ = 0;
};

using registry_t = database_transaction_registry<stored_key_t, test_store_t, atomic_reference, waiter_t>;

static_assert(transaction_store_concept<test_store_t, stored_key_t, atomic_reference, waiter_t>,
              "The store must serve the commit path");

#pragma endregion Store

#pragma region Transaction

/**
 *  @brief The front end the suite drives the registry through, holding one transaction's own state.
 *
 *  Every call the registry expects from a front end appears here once, which is what the specialization
 *  is for: the contract is the list of members below, not a comment describing it.
 */
template <>
class blobs::database_transaction<stored_key_t, test_store_t, atomic_reference, waiter_t> {
  public:
    using state_t = registry_t::transaction_state_t;
    using transaction_deltas_t = registry_t::transaction_deltas_t;

    /** The widths the sole collection admits, which every write is checked against. */
    static constexpr transaction_deltas_t::value_widths_t widths_k {value_bytes_k, 0};

    /** What the transaction is allowed to do next. */
    enum class phase_t {

        /** Reading, writing and watching. */
        started_k,

        /** Staged, holding the ledger's leader word until it commits or rolls back. */
        staged_k,
    };

    database_transaction(registry_t& registry, transaction_deltas_t&& deltas, resources_shared_ptr_t resources) noexcept
        : registry_(&registry), data_(std::move(deltas), resources) {
        start_stamp_ = registry_->request_start_(pin_);
    }

    database_transaction(database_transaction const&) = delete;
    database_transaction& operator=(database_transaction const&) = delete;

    ~database_transaction() noexcept {
        if (phase_ == phase_t::staged_k) registry_->discard_(data_);
    }

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

    /** Writes @p word under @p stored_key, watching the key on @p watch_schedule_ beside the write. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::on_commit_k>
    status_t insert(stored_key_t stored_key, std::uint64_t word) noexcept {
        std::byte payload[value_bytes_k];
        std::memcpy(payload, &word, value_bytes_k);
        return data_.deltas.write(collection_k, widths_k, stored_key, stored_value_view_t {payload, value_bytes_k},
                                  written_watch_<watch_schedule_>());
    }

    /** Drops @p stored_key, watching the key on @p watch_schedule_ beside the write. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::on_commit_k>
    status_t remove(stored_key_t stored_key) noexcept {
        return data_.deltas.remove(collection_k, widths_k, stored_key, written_watch_<watch_schedule_>());
    }

    /** Checks the watched keys and makes room in the store, holding the write path once it answers. */
    status_t stage() noexcept {
        transaction_stamp_t staged_stamp = invalid_transaction_stamp_k;
        status_t const status = registry_->stage_(data_, start_stamp_, staged_stamp);
        if (status.succeeded()) phase_ = phase_t::staged_k;
        return status;
    }

    /** Stages unless already staged, then writes the changes into the store. */
    status_t commit() noexcept {
        if (phase_ != phase_t::staged_k)
            if (status_t const staged = stage(); staged.failed()) return staged;
        last_commit_stamp_ = registry_->commit_staged_(data_);
        phase_ = phase_t::started_k;
        data_.clear();
        start_stamp_ = registry_->request_start_(pin_);
        return {};
    }

    /** Drops every change and every watch, leaving the registry with no trace of the transaction. */
    void rollback() noexcept {
        if (phase_ == phase_t::staged_k) registry_->discard_(data_);
        phase_ = phase_t::started_k;
        data_.clear();
        start_stamp_ = registry_->request_start_(pin_);
    }

    /** Starts over with the changes kept, which a caller retrying a conflict asks for. */
    void renew() noexcept {
        if (phase_ == phase_t::staged_k) registry_->discard_(data_);
        phase_ = phase_t::started_k;
        data_.read_set.clear();
        start_stamp_ = registry_->request_start_(pin_);
        data_.deltas.rescope_watches(start_stamp_);
    }

    /** The stamp of the last commit this transaction landed. */
    transaction_stamp_t last_commit_stamp() const noexcept { return last_commit_stamp_; }

  private:

    /** The stamp a watch on @p watch_schedule_ is scoped from. */
    template <watch_schedule_t watch_schedule_>
    transaction_stamp_t watch_stamp_() const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return registry_->current_stamp_();
        else return start_stamp_;
    }

    /** The watch a write on @p watch_schedule_ keeps beside itself, none for a schedule that checks no later. */
    template <watch_schedule_t watch_schedule_>
    transaction_stamp_t written_watch_() const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k ||
                      watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return watch_stamp_<watch_schedule_>();
        else return invalid_transaction_stamp_k;
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

    /** The registry every stage and every commit goes through. */
    registry_t* registry_;

    /** The writes and the watches the transaction carries. */
    state_t data_;

    /** The start stamp, pinned against the store's ledger. */
    stamp_pin_t pin_;

    /** The stamp the current attempt started at. */
    transaction_stamp_t start_stamp_ = invalid_transaction_stamp_k;

    /** The stamp the last commit landed at. */
    transaction_stamp_t last_commit_stamp_ = invalid_transaction_stamp_k;

    /** What the transaction is allowed to do next. */
    phase_t phase_ = phase_t::started_k;
};

using transaction_t = database_transaction<stored_key_t, test_store_t, atomic_reference, waiter_t>;

#pragma endregion Transaction

#pragma region Fixture

/** A registry over a store of words, with the resources both are charged to. */
struct fixture_t {

    /** The budget every transaction is charged to. */
    resources_shared_ptr_t resources;

    /** The collection every commit of the fixture lands in. */
    test_store_t store;

    /** The registry under test. */
    registry_t registry;

    fixture_t() noexcept
        : resources(std::make_shared<resources_t>(1ull << 30, 0)), store(), registry(store, resources) {}

    /** A transaction, or nothing when the registry refuses it. */
    expected<registry_t::transaction_unique_ptr_t> open() noexcept {
        transaction_config_t const config {deltas_bytes_k, deltas_max_count_k};
        return registry.create_transaction(config);
    }
};

#pragma endregion Fixture

#pragma region Deltas Cases

/** The deltas a transaction files its writes in, driven here apart from any registry. */
using transaction_deltas_t = transaction_deltas<stored_key_t, atomic_reference, waiter_t>;

/** A second collection, whose keys the deltas keep apart from those of @c collection_k. */
constexpr collection_id_t other_collection_k {uuid_from_integer(8)};

/** The last delta of a key wins inside its own collection alone, an empty value reads back empty, a value past the
 *  collection's width is refused, and every byte returns to the budget once the deltas die. */
static std::size_t deltas_read_back_per_collection() {
    std::size_t failures = 0;
    resources_shared_ptr_t const resources = std::make_shared<resources_t>(1ull << 20, 0);
    {
        transaction_deltas_t deltas(deltas_bytes_k, deltas_max_count_k, resources);
        transaction_deltas_t::value_widths_t const widths {0, 4};
        std::array<byte_t, 3> const first {byte_t {1}, byte_t {2}, byte_t {3}};
        std::array<byte_t, 2> const second {byte_t {7}, byte_t {8}};
        std::array<byte_t, 5> too_wide {};
        failures += expect(deltas.write(collection_k, widths, 1, first).succeeded());
        failures += expect(deltas.write(other_collection_k, widths, 1, first).succeeded());
        failures += expect(deltas.remove(collection_k, widths, 1).succeeded());
        failures += expect(deltas.write(collection_k, widths, 1, second).succeeded());
        failures += expect(deltas.remove(collection_k, widths, 2).succeeded());
        failures += expect(deltas.write(collection_k, widths, 3, stored_value_view_t {}).succeeded());
        failures += expect_status(deltas.write(collection_k, widths, 4, too_wide), status_code_t::malformed_k);
        failures += expect_eq(deltas.count(), std::size_t {4});
        failures += expect_eq(deltas.count(collection_k), std::size_t {3});
        failures += expect(deltas.lookup(collection_k, 2) == lookup_result_t::removed_k);
        failures += expect(deltas.lookup(collection_k, 4) == lookup_result_t::missing_k);
        failures += expect(deltas.lookup(other_collection_k, 2) == lookup_result_t::missing_k);

        std::array<byte_t, 4> into {};
        stored_value_size_t size = 0;
        failures += expect(deltas.select(collection_k, 1, into, size).succeeded());
        failures += expect_eq(std::size_t {size}, second.size());
        failures += expect(std::memcmp(into.data(), second.data(), second.size()) == 0);
        failures += expect(deltas.select(other_collection_k, 1, into, size).succeeded());
        failures += expect_eq(std::size_t {size}, first.size());
        failures += expect(std::memcmp(into.data(), first.data(), first.size()) == 0);
        failures += expect(deltas.select(collection_k, 3, into, size).succeeded());
        failures += expect_eq(std::size_t {size}, std::size_t {0});
        failures += expect_status(deltas.select(collection_k, 2, into, size), status_code_t::not_found_k);
    }
    failures += expect_eq(resources->memory_constraint.used(), std::size_t {0});
    return failures;
}

/** Deltas holding as many keys as they admit refuse the next key with @c capacity_exhausted_k and keep the rest. */
static std::size_t deltas_refuse_past_their_bound() {
    std::size_t failures = 0;
    resources_shared_ptr_t const resources = std::make_shared<resources_t>(1ull << 20, 0);
    constexpr std::size_t bound_k = 8;
    transaction_deltas_t deltas(deltas_bytes_k, bound_k, resources);
    transaction_deltas_t::value_widths_t const widths {value_bytes_k, 0};
    for (stored_key_t key = 0; key != bound_k; ++key)
        failures += expect(deltas.remove(collection_k, widths, key).succeeded());
    failures += expect_status(deltas.remove(collection_k, widths, bound_k), status_code_t::capacity_exhausted_k);
    failures += expect_eq(deltas.count(), bound_k);
    failures += expect(deltas.lookup(collection_k, bound_k - 1) == lookup_result_t::removed_k);
    failures += expect(deltas.lookup(collection_k, bound_k) == lookup_result_t::missing_k);
    return failures;
}

#pragma endregion Deltas Cases

#pragma region Ordering Cases

/** A request that checks nothing and publishes nothing, which the sequencer still gives a stamp of its own. */
static status_t prepare_nothing(void*) noexcept { return {}; }
static void publish_nothing(void*, transaction_stamp_t) noexcept {}

/** Commits of many threads, combined by whichever of them leads, draw distinct stamps, and the watermark covers every
 *  one of them once all returned. */
static std::size_t commit_stamps_are_distinct() {
    std::size_t failures = 0;
    commit_ledger_t ledger;

    constexpr std::size_t threads_k = 8;
    constexpr std::size_t per_thread_k = 20000;
    std::vector<std::vector<transaction_stamp_t>> claimed(threads_k);
    std::atomic<std::size_t> unpublished {0};
    std::vector<std::thread> workers;
    workers.reserve(threads_k);
    for (std::size_t thread_index = 0; thread_index != threads_k; ++thread_index)
        workers.emplace_back([&, thread_index] {
            claimed[thread_index].reserve(per_thread_k);
            for (std::size_t issue = 0; issue != per_thread_k; ++issue) {
                commit_request_t request;
                request.prepare = &prepare_nothing;
                request.publish = &publish_nothing;
                expected<transaction_stamp_t> const stamp = ledger.submit(request);
                if (stamp) claimed[thread_index].push_back(*stamp);
                // The stamp is published on return, so the committer reads what it wrote.
                if (!stamp || ledger.published() < *stamp) unpublished.fetch_add(1, std::memory_order_relaxed);
            }
        });
    for (std::thread& worker : workers) worker.join();

    std::vector<bool> seen(threads_k * per_thread_k + 1, false);
    std::size_t repeats = 0;
    transaction_stamp_t highest = unwritten_stamp_k;
    for (std::vector<transaction_stamp_t> const& batch : claimed)
        for (transaction_stamp_t const stamp : batch) {
            repeats += seen[stamp];
            seen[stamp] = true;
            if (stamp > highest) highest = stamp;
        }

    failures += expect_eq(repeats, std::size_t {0});
    failures += expect_eq(unpublished.load(), std::size_t {0});
    failures += expect_eq(highest, transaction_stamp_t {threads_k * per_thread_k});
    failures += expect_eq(ledger.published(), highest);
    return failures;
}

/** What a collection's first commit into empty memory does to its view's pin, as a request's publication. */
struct bounding_t {
    commit_ledger_t* ledger = nullptr;
    stamp_pin_t* view = nullptr;
};

static void publish_bounding(void* context, transaction_stamp_t stamp) noexcept {
    bounding_t const& bounding = *static_cast<bounding_t const*>(context);
    bounding.ledger->bound_view(*bounding.view, stamp - 1);
}

/** Readers and views pin stamps without allocating, and the ledger answers from exactly those pins. */
static std::size_t pins_answer_the_ledger() {
    std::size_t failures = 0;
    commit_ledger_t ledger;
    failures += expect(ledger.resume(41).succeeded());
    failures += expect(ledger.resume(43).failed());

    stamp_pin_t reader;
    failures += expect_eq(ledger.pin_reader(reader), transaction_stamp_t {41});
    failures += expect_eq(ledger.newest_reader(), transaction_stamp_t {41});
    failures += expect_eq(ledger.durable_stamp(), transaction_stamp_t {41});

    // A registered view holding no memory runs bounds nothing; its first commit bounds it below that commit.
    stamp_pin_t view;
    ledger.register_view(view);
    failures += expect_eq(ledger.durable_stamp(), transaction_stamp_t {41});
    bounding_t bounding {&ledger, &view};
    commit_request_t request;
    request.context = &bounding;
    request.prepare = &prepare_nothing;
    request.publish = &publish_bounding;
    expected<transaction_stamp_t> const landed = ledger.submit(request);
    failures += expect(landed && *landed == transaction_stamp_t {42});
    failures += expect_eq(ledger.published(), transaction_stamp_t {42});
    failures += expect_eq(ledger.durable_stamp(), transaction_stamp_t {41});
    failures += expect_eq(ledger.horizon_besides(view), transaction_stamp_t {42});

    // A pin moves by relinking, so the moved-to object answers and the moved-from one holds nothing.
    stamp_pin_t moved {std::move(reader)};
    failures += expect(!reader.held() && moved.held());
    stamp_pin_t later;
    failures += expect_eq(ledger.pin_reader(later), transaction_stamp_t {42});
    st::basic_vector<transaction_stamp_t> pinned;
    failures += expect(ledger.pinned_readers(pinned).succeeded());
    failures += expect_eq(pinned.size(), std::size_t {2});
    failures += expect(pinned.size() == 2 && pinned[0] == 41 && pinned[1] == 42);

    moved.release();
    failures += expect_eq(ledger.newest_reader(), transaction_stamp_t {42});
    later.release();
    failures += expect_eq(ledger.newest_reader(), unwritten_stamp_k);
    // A flush that writes every memory run out bounds nothing again, and an unregistered view counts for nothing.
    ledger.bound_view(view, head_stamp_k);
    failures += expect_eq(ledger.durable_stamp(), transaction_stamp_t {42});
    ledger.bound_view(view, 30);
    view.release();
    failures += expect_eq(ledger.durable_stamp(), transaction_stamp_t {42});
    return failures;
}

#pragma endregion Ordering Cases

#pragma region Conflict Cases

/** A key watched before another transaction wrote it refuses to stage. */
static std::size_t read_write_conflict_is_caught() {
    std::size_t failures = 0;
    fixture_t fixture;

    auto reader = fixture.open();
    auto writer = fixture.open();
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

    auto reader = fixture.open();
    auto writer = fixture.open();
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

    auto first = fixture.open();
    auto second = fixture.open();
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
    failures += expect_eq(second->get()->state().deltas.count(), std::size_t {1});
    second->get()->rollback();
    return failures;
}

/** Two transactions writing a thousand watched keys each, disjoint, both land: no conflict is ever invented. */
static std::size_t disjoint_transactions_never_conflict() {
    std::size_t failures = 0;
    fixture_t fixture;
    constexpr std::size_t band_k = 1000;
    constexpr std::size_t rounds_k = 20;

    std::size_t refused = 0;
    for (std::size_t round = 0; round != rounds_k; ++round) {
        auto first = fixture.open();
        auto second = fixture.open();
        failures += expect(first.has_value() && second.has_value());
        if (failures != 0) return failures;
        for (std::size_t key = 0; key != band_k; ++key) {
            failures += expect(first->get()->insert(key, round).succeeded());
            failures += expect(second->get()->insert(band_k + key, round).succeeded());
            failures += expect(first->get()
                                   ->watch<watch_schedule_t::on_commit_k>(2 * band_k + key, watch_purpose_t::read_k)
                                   .succeeded());
        }
        refused += first->get()->commit().failed();
        refused += second->get()->commit().failed();
    }
    failures += expect_eq(refused, std::size_t {0});
    failures += expect_eq(fixture.store.balance(band_k - 1), std::uint64_t {rounds_k - 1});
    failures += expect_eq(fixture.store.balance(2 * band_k - 1), std::uint64_t {rounds_k - 1});
    return failures;
}

/** Each of the four schedules watches exactly what it says it does. */
static std::size_t watch_schedules_behave() {
    std::size_t failures = 0;

    // `never_k` ignores a collision outright.
    {
        fixture_t fixture;
        auto reader = fixture.open();
        auto writer = fixture.open();
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
        auto reader = fixture.open();
        auto writer = fixture.open();
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
        auto reader = fixture.open();
        auto writer = fixture.open();
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
        auto reader = fixture.open();
        auto writer = fixture.open();
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
            auto second_writer = fixture.open();
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

    auto transaction = fixture.open();
    auto reader = fixture.open();
    failures += expect(transaction.has_value() && reader.has_value());
    if (failures != 0) return failures;

    // The reader watches what the doomed commit is about to write, from before it runs.
    failures += expect(reader->get()->watch<watch_schedule_t::on_commit_k>(3, watch_purpose_t::read_k).succeeded());
    failures += expect(transaction->get()->insert(3, 7).succeeded());
    failures += expect(transaction->get()->insert(4, 8).succeeded());

    fixture.store.behaviour = store_behaviour_t::refusing_applies_k;
    failures += expect(transaction->get()->commit().failed());
    failures += expect_eq(transaction->get()->state().deltas.count(), std::size_t {2});

    // The room was refused before a stamp was drawn, so nothing landed and a reader spanning the failure stages.
    failures += expect(reader->get()->stage().succeeded());
    reader->get()->rollback();

    // Retrying is what renewal is for: the changes stay and only the watches and the stamp restart.
    transaction->get()->renew();
    failures += expect_eq(transaction->get()->state().deltas.count(), std::size_t {2});

    // A refused stage leaves them too, which is the other half of the same promise.
    fixture.store.behaviour = store_behaviour_t::refusing_flushes_k;
    failures += expect_status(transaction->get()->stage(), status_code_t::device_k);
    failures += expect_eq(transaction->get()->state().deltas.count(), std::size_t {2});

    // The refused stage handed the write path back, so the retry can still take it.
    fixture.store.behaviour = store_behaviour_t::accepting_k;
    failures += expect(transaction->get()->commit().succeeded());
    failures += expect_eq(fixture.store.balance(3), std::uint64_t {7});
    failures += expect_eq(fixture.store.balance(4), std::uint64_t {8});
    return failures;
}

/** A stage that has to flush first does, and hands the write path back once the commit lands. */
static std::size_t refused_flush_releases_the_channel() {
    std::size_t failures = 0;
    fixture_t fixture;
    fixture.store.behaviour = store_behaviour_t::always_flushing_k;

    auto transaction = fixture.open();
    failures += expect(transaction.has_value());
    if (failures != 0) return failures;
    failures += expect(transaction->get()->insert(3, 7).succeeded());

    failures += expect(transaction->get()->stage().succeeded());
    failures += expect_eq(fixture.store.flushes, std::size_t {1});
    failures += expect(transaction->get()->commit().succeeded());

    // The write path was handed back, so the next transaction can still take it.
    auto second = fixture.open();
    failures += expect(second.has_value());
    if (second.has_value()) {
        failures += expect(second->get()->insert(4, 8).succeeded());
        failures += expect(second->get()->commit().succeeded());
    }
    return failures;
}

/** Rollback and renewal leave the registry with no trace of the transaction, and its pin goes with it. */
static std::size_t rollback_and_renew_leave_no_trace() {
    std::size_t failures = 0;
    fixture_t fixture;

    {
        auto transaction = fixture.open();
        failures += expect(transaction.has_value());
        if (failures != 0) return failures;

        failures += expect(transaction->get()->insert(3, 7).succeeded());
        failures += expect(transaction->get()->stage().succeeded());
        transaction->get()->rollback();
        failures += expect_eq(transaction->get()->state().deltas.count(), std::size_t {0});
        failures += expect_eq(transaction->get()->state().read_set.count(), std::size_t {0});

        // Renewal keeps the changes and drops only the watches.
        failures += expect(transaction->get()->insert(3, 9).succeeded());
        failures += expect(transaction->get()->stage().succeeded());
        transaction->get()->renew();
        failures += expect_eq(transaction->get()->state().deltas.count(), std::size_t {1});
        failures += expect_eq(transaction->get()->state().read_set.count(), std::size_t {0});
        failures += expect(transaction->get()->commit().succeeded());
        failures += expect_eq(fixture.store.balance(3), std::uint64_t {9});
        failures += expect_eq(fixture.store.ledger().newest_reader(), fixture.store.ledger().published());
    }

    // The transaction is gone with its object, and so is its pin.
    failures += expect_eq(fixture.store.ledger().newest_reader(), unwritten_stamp_k);
    return failures;
}

/** A watcher beside twenty thousand commits is refused by exactly the one that wrote its key, and a watcher of a
 *  key nobody wrote is refused by none, since the store answers from the stamps it keeps per key. */
static std::size_t long_watchers_stay_exact() {
    std::size_t failures = 0;
    fixture_t fixture;

    auto lingering = fixture.open();
    auto untouched = fixture.open();
    failures += expect(lingering.has_value() && untouched.has_value());
    if (failures != 0) return failures;
    // The watched key is written by the very first of the commits below, and by nothing after it.
    failures += expect(lingering->get()->watch<watch_schedule_t::on_commit_k>(1, watch_purpose_t::read_k).succeeded());
    failures += expect(untouched->get()->watch<watch_schedule_t::on_commit_k>(0, watch_purpose_t::read_k).succeeded());

    for (std::size_t round = 0; round != 20000; ++round) {
        auto writer = fixture.open();
        if (!writer.has_value()) {
            failures += expect(false);
            break;
        }
        failures += expect(writer->get()->insert(round == 0 ? 1 : 2 + round % (accounts_k - 2), round).succeeded());
        if (writer->get()->commit().failed()) failures += expect(false);
    }

    failures += expect_status(lingering->get()->stage(), status_code_t::write_conflict_k);
    lingering->get()->rollback();
    failures += expect(untouched->get()->stage().succeeded());
    untouched->get()->rollback();
    return failures;
}

#pragma endregion Lifecycle Cases

#pragma region Concurrency Cases

/**
 *  @brief Threads moving money between overlapping accounts, checked against the one total.
 *
 *  A lost update leaves the total short or long, and a half-applied commit leaves it short by the
 *  half that landed, so the sum is the whole result. Every transaction reads two balances outside
 *  the write path and writes both inside it, which is the shape the stamps have to survive.
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
            auto opened = fixture.open();
            if (!opened.has_value()) return;
            transaction_t& transaction = *opened->get();

            std::uint64_t scramble = thread_index * 0x9e3779b97f4a7c15ull + 1;
            for (std::size_t transfer = 0; transfer != transfers_k; ++transfer) {
                scramble ^= scramble << 13;
                scramble ^= scramble >> 7;
                scramble ^= scramble << 17;
                stored_key_t const from = scramble % contended_accounts_k;
                stored_key_t const to = (scramble / contended_accounts_k) % contended_accounts_k;
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

    // A layer that refused everything would pass the total while proving nothing, so it must not.
    failures += expect(commits > 0);
    return failures;
}

/**
 *  @brief A transaction starting while a commit is half applied orders before it, not after it.
 *
 *  This is the ordering the money-moving case can only catch by luck, so it is driven here by hand: a
 *  commit is held inside the store with its stamp drawn but its writes not yet landed, a reader starts in
 *  exactly that window and reads the stale balance, and the stage that follows has to refuse it. Starting the
 *  reader at the drawn stamp instead of the published one lets it through with a value that was never committed.
 */
static std::size_t start_during_apply_still_conflicts() {
    std::size_t failures = 0;
    fixture_t fixture;
    fixture.store.behaviour = store_behaviour_t::pausing_in_apply_k;

    auto writer = fixture.open();
    failures += expect(writer.has_value());
    if (failures != 0) return failures;

    std::size_t committed = 0;
    failures += expect(writer->get()->insert(5, 77).succeeded());
    std::thread committing([&] { committed = writer->get()->commit().succeeded(); });

    // The commit is now past its stamp and has not written a byte.
    failures += expect(spin_until([&] { return fixture.store.paused(); }));

    auto reader = fixture.open();
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

/** Threads opening and closing transactions against one ledger leave no pin behind. */
static std::size_t pins_survive_churn() {
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
                auto transaction = fixture.open();
                if (!transaction.has_value()) continue;
                ++opened[thread_index];
                if (transaction->get()->insert((thread_index * rounds_k + round) % accounts_k, round).succeeded())
                    [[maybe_unused]]
                    status_t const committed = transaction->get()->commit();
            }
        });
    for (std::thread& worker : workers) worker.join();

    std::size_t total_opened = 0;
    for (std::size_t const count : opened) total_opened += count;
    failures += expect_eq(total_opened, threads_k * rounds_k);
    failures += expect_eq(fixture.store.ledger().newest_reader(), unwritten_stamp_k);
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

    report(totals, "deltas.per_collection", deltas_read_back_per_collection);
    report(totals, "deltas.bound", deltas_refuse_past_their_bound);
    report(totals, "stamps.distinct", commit_stamps_are_distinct);
    report(totals, "ledger.pins", pins_answer_the_ledger);
    report(totals, "conflict.read_write", read_write_conflict_is_caught);
    report(totals, "conflict.untouched_commits", untouched_key_commits);
    report(totals, "conflict.write_write_one_winner", write_write_one_winner);
    report(totals, "conflict.disjoint_never", disjoint_transactions_never_conflict);
    report(totals, "conflict.watch_schedules", watch_schedules_behave);
    report(totals, "lifecycle.failed_commit_keeps", failed_commit_keeps_changes);
    report(totals, "lifecycle.flush_releases_channel", refused_flush_releases_the_channel);
    report(totals, "lifecycle.rollback_and_renew", rollback_and_renew_leave_no_trace);
    report(totals, "lifecycle.long_watchers_exact", long_watchers_stay_exact);
    report(totals, "threads.serializable", serializable_under_threads);
    report(totals, "threads.start_during_apply", start_during_apply_still_conflicts);
    report(totals, "threads.pins_churn", pins_survive_churn);

    return suite_exit_code(totals);
}
