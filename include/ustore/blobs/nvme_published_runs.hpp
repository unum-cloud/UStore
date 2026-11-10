/**
 *  @file include/ustore/blobs/nvme_published_runs.hpp
 *  @author Ash Vardanian
 *  @date October 25, 2026
 *  @brief The commit ledger the NVMe stores draw their stamps from, and the tree, memory runs and published view of
 *      one store, with every verb that only moves its runs: the commits it prepares and publishes, and the merges,
 *      flushes, compactions and truncations it publishes after.
 *
 *  The ledger pins the stamps readers and views hold, announces readers in its epochs, and lands every commit through
 *  one flat-combining sequencer, so a reader at a published stamp sees every commit at or below it whole.
 *
 *  Every store of the NVMe engine owns one, whatever its keys: @c blobs::nvme_store with memory runs of fixed keys,
 *  and @c strings::nvme_store with memory runs of whole strings, whose runs carry tails. A store keeps its own reads,
 *  seals, watches and drain, and hands the drain to @c published_runs::flush, which lists the memory runs it writes
 *  out and the versions that survive, and publishes whatever level set the drain's tree flush made current.
 *
 *  Whoever replaces a view holds the ledger's leader word, a commit for its swap, and a merge, a
 *  flush or a compaction only around its own; letting the word go, it waits until every reader that
 *  entered before has left, then frees the view and the runs it drops before returning.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`

#include <atomic>  // `std::atomic_thread_fence`, `std::memory_order_acquire`, `std::memory_order_release`
#include <memory>  // `std::construct_at`, `std::destroy_at`
#include <span>    // `std::span`
#include <utility> // `std::exchange`, `std::move`

#include <forkunion/types.hpp> // `fu::default_alignment_k`

#include <smashtable/basic_vector.hpp>   // `st::vector`, `st::assume_reserved`
#include <smashtable/intrusive_list.hpp> // `st::intrusive_list`
#include <smashtable/shared.hpp>         // `st::basic_commit_order`, `st::standard_waiting_policy`

#include "ustore/blobs/nvme_collection_view.hpp" // `collection_view`, `published_view`
#include "ustore/blobs/nvme_memory_run.hpp"      // `kept_versions`, `stamped_delta`
#include "ustore/blobs/nvme_tree.hpp"            // `lsm_tree`, `collection_config_t`, `compaction_stamps_t`
#include "ustore/utilities/algorithm.hpp"        // `radix_sort`, `word_byte`
#include "ustore/utilities/folds.hpp"            // `fold_concept`
#include "ustore/utilities/nvme_channels.hpp"    // `io_channel_t`
#include "ustore/utilities/ram_buffers.hpp" // `local_arena_t`, `arena_allocator`, `resource_constraint`, `constrained_allocator`
#include "ustore/utilities/types.hpp" // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::blobs {

using utilities::constrained_allocator;
using utilities::outside_executor_k;
using utilities::resource_constraint;

/** One object the thread holding the leader word unpublished, linked inside the object's own room until that thread
 *  lets the word go and frees it, so unpublishing allocates nothing and cannot fail. */
struct unpublished_t {

    /** The object unpublished before this one by the same holder of the word. */
    unpublished_t* next = nullptr;

    /** The object, handed to @c destroy. */
    void* object = nullptr;

    /** Frees @c object. */
    void (*destroy)(void* object) noexcept = nullptr;
};

#pragma region Stamps

template <template <typename> typename atomic_reference_, typename waiter_type_>
class commit_ledger;

/** The order a ledger draws its stamps from, shaped as SmashTable's shared default, its committers spinning through
 *  @p waiter_type_ before they park. */
template <typename waiter_type_>
using commit_order = st::basic_commit_order<st::commit_order_t::ring_k, st::commit_order_t::buckets_k,
                                            st::commit_order_t::sharing_k, st::standard_waiting_policy<waiter_type_>>;

/** Whether a pin holds history for a reader, or registers a collection's published view with the ledger. */
enum class pin_kind_t : std::uint8_t {

    /** A transaction or a snapshot, which reads at the stamp it holds. */
    reader_k,

    /** A collection's view, every commit at or below whose stamp the collection holds in runs and not in memory. */
    view_k,

    /** A change feed, which reads every version above the stamp it holds. */
    feed_k,
};

/**
 *  @brief One stamp a reader or a collection's view holds against a ledger, which keeps the history it needs.
 *
 *  Linked into its ledger while held, so taking one allocates nothing and cannot fail. A held pin moves by
 *  relinking under the ledger's claim, and it gives its stamp back when it dies. A reader's stamp never changes
 *  while linked; a view's is stored in place by whoever publishes the view.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class stamp_pin {
  public:

    /** The ledger a pin links into. */
    using ledger_t = commit_ledger<atomic_reference_, waiter_type_>;

    stamp_pin() noexcept = default;
    stamp_pin(stamp_pin const&) = delete;
    stamp_pin& operator=(stamp_pin const&) = delete;
    stamp_pin(stamp_pin&& other) noexcept;
    stamp_pin& operator=(stamp_pin&& other) noexcept;
    ~stamp_pin() noexcept;

    /** Whether this pin holds a stamp. */
    [[nodiscard]] bool held() const noexcept { return ledger_ != nullptr; }

    /** The stamp held, meaningless while the pin holds none. */
    [[nodiscard]] transaction_stamp_t stamp() const noexcept { return stamp_; }

    /** Gives the stamp back, which a pin holding none ignores. */
    void release() noexcept;

  private:
    friend class commit_ledger<atomic_reference_, waiter_type_>;

    /** The ledger this pin is linked into, null while it holds nothing. */
    ledger_t* ledger_ = nullptr;

    /** The neighbours in the ledger's list of pins of the same kind, or this pin itself as a list's head. */
    stamp_pin* previous_ = this;
    stamp_pin* next_ = this;

    /** The stamp held. */
    transaction_stamp_t stamp_ = unwritten_stamp_k;

    /** The fold floor of the store a feed reads, whose address is only compared, so a dropped
     *  store's pin can only hold a merge back; unused by the other kinds. */
    transaction_stamp_t const* folded_ = nullptr;

    /** Which list of the ledger holds this pin. */
    pin_kind_t kind_ = pin_kind_t::reader_k;

    /** A reader's place among the commit order's readers, which holds its low-water mark at or below the stamp. */
    typename commit_order<waiter_type_>::snapshot_claim_t claim_;
};

/** What became of a commit handed to a ledger's sequencer. */
enum class commit_outcome_t : std::uint32_t {

    /** No leader has taken it yet. */
    pending_k,

    /** It landed under @c commit_request_t::stamp. */
    committed_k,

    /** A leader refused it for @c commit_request_t::refusal, and nothing of it landed. */
    refused_k,
};

/** One commit handed to a ledger's sequencer, on its committer's stack for the call: whatever its committer sealed,
 *  and the two steps whichever thread leads runs for it. */
struct commit_request_t {

    /** The request pushed before this one, written before the push. */
    commit_request_t* next = nullptr;

    /** What both steps are handed, owned by the committer. */
    void* context = nullptr;

    /** Checks the commit's watches against the views published so far and lays out every view it publishes, so
     *  @c publish cannot fail; a refusal leaves nothing laid out. */
    status_t (*prepare)(void* context) noexcept = nullptr;

    /** Stamps the sealed runs with the drawn stamp and publishes the views @c prepare laid out. */
    void (*publish)(void* context, transaction_stamp_t stamp) noexcept = nullptr;

    /** Why the leader refused it, meaningful once @c outcome is @c refused_k. */
    status_t refusal;

    /** The stamp it landed under, meaningful once @c outcome is @c committed_k. */
    transaction_stamp_t stamp = unwritten_stamp_k;

    /** Stored with release by the leader, last, after which the request is its committer's again. */
    commit_outcome_t outcome = commit_outcome_t::pending_k;
};

/**
 *  @brief A memory-run record of one collection and the room of the view naming its run, claimed
 *      before the commit takes the leader word, so the leader lays the view out without refusing;
 *      given back unused on destruction.
 *
 *  The store that claimed it fills every member; whoever holds it last hands it to a prepare or
 *  lets it die.
 */
struct memory_run_claim_t {

    memory_run_claim_t() noexcept = default;
    memory_run_claim_t(memory_run_claim_t const&) = delete;
    memory_run_claim_t& operator=(memory_run_claim_t const&) = delete;

    memory_run_claim_t(memory_run_claim_t&& other) noexcept
        : owner(std::exchange(other.owner, nullptr)), view(std::exchange(other.view, nullptr)),
          destroy_view(std::exchange(other.destroy_view, nullptr)), give_back(std::exchange(other.give_back, nullptr)),
          collection(other.collection), record(other.record), generation(other.generation) {}

    memory_run_claim_t& operator=(memory_run_claim_t&& other) noexcept {
        if (this == &other) return *this;
        release();
        owner = std::exchange(other.owner, nullptr);
        view = std::exchange(other.view, nullptr);
        destroy_view = std::exchange(other.destroy_view, nullptr);
        give_back = std::exchange(other.give_back, nullptr);
        collection = other.collection;
        record = other.record;
        generation = other.generation;
        return *this;
    }

    ~memory_run_claim_t() noexcept { release(); }

    /** Whether this holds a record nobody prepared yet. */
    [[nodiscard]] bool held() const noexcept { return give_back != nullptr; }

    /** Gives the record and the view room back through @c owner, as a claim dying unused does. */
    void release() noexcept {
        if (give_back != nullptr) std::exchange(give_back, nullptr)(*this);
    }

    /** What @c give_back resolves the store through: the runs themselves, or the
     *  database naming them. */
    void* owner = nullptr;

    /** The view laid out for the run, which no reader reaches until a leader publishes it. */
    void* view = nullptr;

    /** Destroys @c view without its store, so a claim outliving its collection still frees it. */
    void (*destroy_view)(void* view) noexcept = nullptr;

    /** Returns the record and the view to their store, or frees the view alone once the
     *  store is gone. */
    void (*give_back)(memory_run_claim_t& claimed) noexcept = nullptr;

    /** The collection whose store holds the record, which a database looks the store up by. */
    collection_id_t collection = collection_invalid_id_k;

    /** The record's index in its store's pool. */
    std::uint32_t record = 0;

    /** The generation the record was taken at. */
    std::uint32_t generation = 0;
};

/**
 *  @brief The commit order one database draws its stamps from, every stamp its readers and views pin, the epochs its
 *      readers announce, and the sequencer every commit lands through.
 *
 *  Several databases share one ledger when one commit spans them all, as a graph's two databases do. Pins are
 *  intrusive, so a reader or a collection pins without allocating, and the ledger answers three questions from
 *  them: which stamps a flush or a compaction keeps a version for, the floor stamps below which no reader can
 *  tell apart, and the durable stamp a schema save may promise.
 *
 *  Commits are flat-combined. A committer seals its runs on its own thread and pushes a request; whichever committer
 *  takes the leader word checks, stamps and publishes every pending request in arrival order, publishing each one's
 *  views before its stamp, so stamp order is check order and a reader at a published stamp sees every commit at or
 *  below it whole. Whoever replaces a view holds the leader word, a flush or a compaction only around its swap, and a
 *  staged transaction from its stage to its commit; committers waiting for it park until it is let go, and readers
 *  never touch it. The holder lets the word go first and then frees what it replaced, once every reader that entered
 *  before has left, so a commit returns only after freeing the views its batch replaced and nothing waits for later.
 *
 *  @tparam atomic_reference_ The reference every pending, leader, claim and pinned word is owned through.
 *  @tparam waiter_type_ What a committer's core does between attempts, before it parks on the leader word.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class commit_ledger {
  public:

    /** Spins through @p waiter_type_, then parks on the leader word. */
    using waiting_policy_t = st::standard_waiting_policy<waiter_type_>;

    using order_t = commit_order<waiter_type_>;

    /** One stamp a reader or a view holds against this ledger. */
    using pin_t = stamp_pin<atomic_reference_, waiter_type_>;

    /** The epochs every reader of this ledger's collections announces. */
    using epochs_t = epoch_domain<atomic_reference_, waiter_type_>;

    /** One commit between drawing its stamp and landing it. */
    using commit_in_flight_t = typename order_t::commit_in_flight_t;

    commit_ledger() noexcept {
        views_.kind_ = pin_kind_t::view_k;
        feeds_.kind_ = pin_kind_t::feed_k;
    }
    commit_ledger(commit_ledger const&) = delete;
    commit_ledger& operator=(commit_ledger const&) = delete;
    ~commit_ledger() noexcept {
        assert(readers_.next_ == &readers_ && views_.next_ == &views_ && feeds_.next_ == &feeds_ &&
               "Pins outlive their ledger");
    }

    /** The newest stamp every commit at or below which has landed, which a fresh read answers at; acquired, so the
     *  view a reader loads after it names every commit at or below it. */
    [[nodiscard]] transaction_stamp_t published() const noexcept {
        return static_cast<transaction_stamp_t>(order_.published_stamp());
    }

    /** The stamp the next commit draws, which only the holder of the leader word may ask, since only it draws and it
     *  lands every stamp before drawing the next. */
    [[nodiscard]] transaction_stamp_t next_stamp_leading() const noexcept { return published() + 1; }

    /** The epochs every reader of every collection on this ledger announces, which a view's replacer waits out. */
    [[nodiscard]] epochs_t& epochs() noexcept { return epochs_; }

    /**
     *  @brief Lands @p request with every request pending beside it, combining them on whichever committer takes the
     *      leader word, and answers its stamp once it is published; nothing is held on return.
     *  @note A thread already holding the leader word waits on itself here, as a staged transaction's would.
     */
    expected<transaction_stamp_t> submit(commit_request_t& request) noexcept {
        // A resubmitted request still carries the refusal it came back with, which would return
        // it at once.
        request.outcome = commit_outcome_t::pending_k;
        atomic_reference_<commit_request_t*> const pending {pending_};
        commit_request_t* next = pending.load(std::memory_order_relaxed);
        do request.next = next;
        while (!pending.compare_exchange_weak(next, &request, std::memory_order_release, std::memory_order_relaxed));

        atomic_reference_<commit_outcome_t const> const outcome {request.outcome};
        while (outcome.load(std::memory_order_acquire) == commit_outcome_t::pending_k) {
            if (try_lead_()) {
                combine_();
                stop_leading();
                continue;
            }
            // A leader lets the word go only after landing or refusing every request it took, so this wakes either to
            // its outcome or to the word.
            waiting_policy_t {}.wait_until_changed(&leader_, std::int32_t {1});
        }
        if (request.outcome == commit_outcome_t::refused_k) return request.refusal;
        // Every stamp the leader drew is landed in order, so this returns at once; it holds no lock either way.
        order_.await_published(static_cast<st::commit_stamp_t>(request.stamp));
        return request.stamp;
    }

    /** Takes the leader word, parked while another thread holds it, which a stage, a flush and a compaction do. */
    void lead() noexcept {
        while (!try_lead_()) waiting_policy_t {}.wait_until_changed(&leader_, std::int32_t {1});
    }

    /**
     *  @brief Hands the leader word back, wakes every thread parked on it, then waits until every reader that entered
     *      before has left and frees whatever this holder unpublished.
     *  @note Called outside the epochs, as the wait would otherwise wait for the caller itself.
     */
    void stop_leading() noexcept {
        unpublished_t* unpublished = std::exchange(unpublished_, nullptr);
        atomic_reference_<std::int32_t> {leader_}.store(0, std::memory_order_release);
        waiting_policy_t {}.notify_waiters(&leader_);
        if (!unpublished) return;
        epochs_.wait_for_readers();
        while (unpublished) {
            unpublished_t* const next = unpublished->next;
            unpublished->destroy(unpublished->object);
            unpublished = next;
        }
    }

    /** Takes @p object, which the caller holding the leader word just unpublished, inside its own @p room, and frees it
     *  with @p destroy when that caller lets the word go. */
    void unpublish(unpublished_t& room, void* object, void (*destroy)(void*) noexcept) noexcept {
        room.object = object;
        room.destroy = destroy;
        room.next = unpublished_;
        unpublished_ = &room;
    }

    /** Lands @p request, which its caller prepared while holding the leader word, then every request pending beside
     *  it, hands the word back, and answers the stamp once it is published. */
    transaction_stamp_t commit_leading(commit_request_t& request) noexcept {
        land_(request);
        combine_();
        stop_leading();
        order_.await_published(static_cast<st::commit_stamp_t>(request.stamp));
        return request.stamp;
    }

    /** Waits until every commit at or below @p stamp is published, so its committer reads what it wrote; a commit
     *  sharing the ledger with another store may land before an older one does. */
    void await_published(transaction_stamp_t stamp) noexcept {
        order_.await_published(static_cast<st::commit_stamp_t>(stamp));
    }

    /**
     *  @brief Continues the stamps above @p published, which a mount does before anything reads or commits.
     *  @return @c invalid_argument_k for a stamp past the order's range, or @c already_exists_k once anything drew.
     */
    status_t resume(transaction_stamp_t published) noexcept {
        if (published >= static_cast<transaction_stamp_t>(st::latest_snapshot_k))
            return {status_code_t::invalid_argument_k, "A stamp past what the commit order can draw"};
        st::status_t const resumed = order_.resume(static_cast<st::generation_t>(published));
        if (resumed == st::status_t::success_k) return {};
        return {status_code_t::already_exists_k, "A ledger resumes only before it hands out a stamp"};
    }

    /** Pins @p pin at the newest published stamp for a reader, giving back what it held, and answers that stamp. */
    transaction_stamp_t pin_reader(pin_t& pin) noexcept {
        pin.release();
        guard_t const held {claim_};
        pin.stamp_ = static_cast<transaction_stamp_t>(order_.take_snapshot(pin.claim_));
        pin.kind_ = pin_kind_t::reader_k;
        link_(readers_, pin);
        if (newest_reader_ < pin.stamp_) store_(newest_reader_, pin.stamp_);
        return pin.stamp_;
    }

    /** Registers a collection's view as @p pin, holding memory runs of no commit yet, which a mount does once. */
    void register_view(pin_t& pin) noexcept {
        pin.release();
        guard_t const held {claim_};
        pin.stamp_ = head_stamp_k;
        pin.kind_ = pin_kind_t::view_k;
        link_(views_, pin);
    }

    /** Records that the view @p pin registers holds in memory no commit at or below @p stamp, or no commit at all
     *  for @c head_stamp_k; called by whoever publishes that view, holding the leader word, before the stamp. */
    void bound_view(pin_t& pin, transaction_stamp_t stamp) noexcept { store_(pin.stamp_, stamp); }

    /**
     *  @brief Pins @p pin at @p stamp for a feed of a collection whose merges may have folded every
     *      version at or below @p folded, giving back what it held.
     *  @return Success, or @c write_conflict_k for a stamp below @p folded, leaving @p pin empty.
     */
    status_t pin_feed(pin_t& pin, transaction_stamp_t stamp, transaction_stamp_t const& folded) noexcept {
        pin.release();
        guard_t const held {claim_};
        if (stamp < folded) return {status_code_t::write_conflict_k, "A merge folded the versions after that stamp"};
        pin.stamp_ = stamp;
        pin.kind_ = pin_kind_t::feed_k;
        pin.folded_ = &folded;
        link_(feeds_, pin);
        return {};
    }

    /** @p horizon lowered to the oldest stamp a feed of the collection pins, which its merge keeps
     *  every version above, raising the stamp @p folded that collection's merges may have folded up
     *  to; one claim covers both, so a feed either bounds the merge or finds its fold. */
    [[nodiscard]] transaction_stamp_t fold_horizon(transaction_stamp_t horizon, transaction_stamp_t& folded) noexcept {
        guard_t const held {claim_};
        for (pin_t const* pin = feeds_.next_; pin != &feeds_; pin = pin->next_)
            if (pin->folded_ == &folded) horizon = fu::min_of(horizon, pin->stamp_);
        if (folded < horizon) folded = horizon;
        return horizon;
    }

    /** The newest stamp any reader pins, or @c unwritten_stamp_k while none does. */
    [[nodiscard]] transaction_stamp_t newest_reader() const noexcept { return load_(newest_reader_); }

    /** The oldest stamp a reader may still read at, so versions at or below it are indistinguishable to every one. */
    [[nodiscard]] transaction_stamp_t low_water_mark() const noexcept {
        return static_cast<transaction_stamp_t>(order_.low_water_mark());
    }

    /** The newest stamp every commit at or below which is wholly in runs, which a schema naming those runs may
     *  promise: the least over every registered view, or the published stamp while no view holds memory runs. */
    [[nodiscard]] transaction_stamp_t durable_stamp() noexcept { return horizon_besides(views_); }

    /** The stamp below which no reader and no mount can tell two stamps apart, which a run's column counts from. */
    [[nodiscard]] transaction_stamp_t floor() noexcept { return fu::min_of(low_water_mark(), durable_stamp()); }

    /** As @c floor, ignoring the memory runs of the view @p own_pin registers, which a flush is
     *  about to write out. */
    [[nodiscard]] transaction_stamp_t floor_besides(pin_t const& own_pin) noexcept {
        return fu::min_of(low_water_mark(), horizon_besides(own_pin));
    }

    /**
     *  @brief As @c durable_stamp, ignoring the memory runs of the view @p own_pin registers.
     *
     *  The published stamp is read first: every commit at or below it published its views and bounded them before it
     *  landed, so a bound read after it covers that commit, and a commit above it is past the answer anyway.
     */
    [[nodiscard]] transaction_stamp_t horizon_besides(pin_t const& own_pin) noexcept {
        transaction_stamp_t horizon = published();
        guard_t const held {claim_};
        for (pin_t const* pin = views_.next_; pin != &views_; pin = pin->next_)
            if (pin != &own_pin) horizon = fu::min_of(horizon, load_(pin->stamp_));
        return horizon;
    }

    /** Every stamp a reader pins, ascending and distinct. */
    status_t pinned_readers(st::vector<transaction_stamp_t>& into) noexcept {
        into.clear();
        {
            guard_t const held {claim_};
            for (pin_t const* pin = readers_.next_; pin != &readers_; pin = pin->next_)
                if (into.push_back(transaction_stamp_t {pin->stamp_}) != st::status_t::success_k)
                    return {status_code_t::out_of_memory_k, "No room to list the stamps readers pin"};
        }
        utilities::radix_sort(
            std::span<transaction_stamp_t> {into}, sizeof(transaction_stamp_t),
            [](transaction_stamp_t stamp, std::size_t digit) noexcept { return utilities::word_byte(stamp, digit); },
            [](transaction_stamp_t first, transaction_stamp_t second) noexcept { return first < second; });
        std::size_t kept = 0;
        for (std::size_t index = 0; index != into.size(); ++index)
            if (kept == 0 || into[kept - 1] != into[index]) into[kept++] = into[index];
        while (into.size() != kept) into.pop_back();
        return {};
    }

  private:
    friend class stamp_pin<atomic_reference_, waiter_type_>;

    /** Takes the leader word if nobody holds it. */
    [[nodiscard]] bool try_lead_() noexcept {
        std::int32_t idle = 0;
        return atomic_reference_<std::int32_t> {leader_}.compare_exchange_strong(idle, 1, std::memory_order_acquire,
                                                                                 std::memory_order_relaxed);
    }

    /** Draws @p request's stamp, publishes its views, lands the stamp, and hands the request back. */
    void land_(commit_request_t& request) noexcept {
        [[maybe_unused]] transaction_stamp_t const foreseen = next_stamp_leading();
        commit_in_flight_t in_flight;
        order_.begin_commit(in_flight);
        transaction_stamp_t const stamp = static_cast<transaction_stamp_t>(in_flight.stamp());
        assert(stamp == foreseen && "The leader draws the stamp after the one last published");
        request.publish(request.context, stamp);
        // The leader draws every stamp and lands it before the next, so the order never waits here.
        order_.end_commit(in_flight);
        request.stamp = stamp;
        atomic_reference_<commit_outcome_t> {request.outcome}.store(commit_outcome_t::committed_k,
                                                                    std::memory_order_release);
    }

    /** Prepares and lands every pending request in arrival order. */
    void combine_() noexcept {
        commit_request_t* pushed = atomic_reference_<commit_request_t*> {pending_}.exchange(nullptr,
                                                                                            std::memory_order_acquire);
        commit_request_t* arrived = nullptr;
        while (pushed) {
            commit_request_t* const next = pushed->next;
            pushed->next = arrived;
            arrived = pushed;
            pushed = next;
        }
        while (arrived) {
            commit_request_t& request = *arrived;
            // Read first: once its outcome is stored, the request belongs to its committer again.
            arrived = request.next;
            if (status_t const prepared = request.prepare(request.context); prepared.failed()) {
                request.refusal = prepared;
                atomic_reference_<commit_outcome_t> {request.outcome}.store(commit_outcome_t::refused_k,
                                                                            std::memory_order_release);
                continue;
            }
            land_(request);
        }
    }

    /** Holds the pin lists for the life of the guard. */
    class guard_t {
      public:
        explicit guard_t(std::int32_t& claim) noexcept : claim_(claim) {
            atomic_reference_<std::int32_t> const word {claim_};
            std::int32_t expected_free = 0;
            while (
                !word.compare_exchange_weak(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                waiter_type_ {}(&claim_, expected_free, std::size_t {0});
                expected_free = 0;
            }
        }
        guard_t(guard_t const&) = delete;
        guard_t& operator=(guard_t const&) = delete;
        ~guard_t() noexcept { atomic_reference_<std::int32_t> {claim_}.store(0, std::memory_order_release); }

      private:
        std::int32_t& claim_;
    };

    template <typename word_type_>
    static word_type_ load_(word_type_ const& word) noexcept {
        return atomic_reference_<word_type_ const> {word}.load(std::memory_order_acquire);
    }

    template <typename word_type_>
    static void store_(word_type_& word, word_type_ value) noexcept {
        atomic_reference_<word_type_> {word}.store(value, std::memory_order_release);
    }

    /** Appends @p pin to the list @p head starts, under the claim. */
    void link_(pin_t& head, pin_t& pin) noexcept {
        pin.ledger_ = this;
        pin.previous_ = head.previous_;
        pin.next_ = &head;
        head.previous_->next_ = &pin;
        head.previous_ = &pin;
    }

    /** Unlinks @p pin, and answers the claim it held so the caller retires it outside the claim. */
    void unlink_(pin_t& pin) noexcept {
        guard_t const held {claim_};
        pin.previous_->next_ = pin.next_;
        pin.next_->previous_ = pin.previous_;
        pin.previous_ = pin.next_ = &pin;
        pin.ledger_ = nullptr;
        if (pin.kind_ != pin_kind_t::reader_k) return;
        transaction_stamp_t newest = unwritten_stamp_k;
        for (pin_t const* other = readers_.next_; other != &readers_; other = other->next_)
            if (newest < other->stamp_) newest = other->stamp_;
        store_(newest_reader_, newest);
    }

    /** Moves @p from's place in its list to @p into, under the claim. */
    void relink_(pin_t& from, pin_t& into) noexcept {
        guard_t const held {claim_};
        into.ledger_ = this;
        into.stamp_ = from.stamp_;
        into.folded_ = from.folded_;
        into.kind_ = from.kind_;
        into.previous_ = from.previous_;
        into.next_ = from.next_;
        into.previous_->next_ = &into;
        into.next_->previous_ = &into;
        from.previous_ = from.next_ = &from;
        from.ledger_ = nullptr;
    }

    /** The order every stamp is drawn from and every reader's claim counts into. */
    order_t order_;

    /** The head of the list of readers' pins. */
    pin_t readers_;

    /** The head of the list of views' pins. */
    pin_t views_;

    /** The head of the list of feeds' pins. */
    pin_t feeds_;

    /** Held by whoever links, unlinks or walks a pin list. */
    std::int32_t claim_ = 0;

    /** The newest stamp any reader pins. */
    transaction_stamp_t newest_reader_ = unwritten_stamp_k;

    /** Requests no leader has combined yet, newest first: pushed with release, drained with acquire. */
    alignas(fu::default_alignment_k) commit_request_t* pending_ = nullptr;

    /** One while a thread leads, which commits, publishes views and frees what they replace once it lets go. */
    alignas(fu::default_alignment_k) std::int32_t leader_ = 0;

    /** What the thread holding the leader word unpublished so far, newest first, touched under the word alone. */
    unpublished_t* unpublished_ = nullptr;

    /** The epochs readers of every collection on this ledger announce. */
    epochs_t epochs_;
};

template <template <typename> typename atomic_reference_, typename waiter_type_>
stamp_pin<atomic_reference_, waiter_type_>::stamp_pin(stamp_pin&& other) noexcept {
    if (other.ledger_) {
        other.ledger_->relink_(other, *this);
        claim_ = std::move(other.claim_);
    }
}

template <template <typename> typename atomic_reference_, typename waiter_type_>
stamp_pin<atomic_reference_, waiter_type_>& stamp_pin<atomic_reference_, waiter_type_>::operator=(
    stamp_pin&& other) noexcept {
    if (this == &other) return *this;
    release();
    if (other.ledger_) {
        other.ledger_->relink_(other, *this);
        claim_ = std::move(other.claim_);
    }
    return *this;
}

template <template <typename> typename atomic_reference_, typename waiter_type_>
stamp_pin<atomic_reference_, waiter_type_>::~stamp_pin() noexcept {
    release();
}

template <template <typename> typename atomic_reference_, typename waiter_type_>
void stamp_pin<atomic_reference_, waiter_type_>::release() noexcept {
    if (!ledger_) return;
    ledger_->unlink_(*this);
    claim_.retire();
}

/**
 *  @brief One moment of a store: a stamp pinned in its ledger, which every read through the
 *      snapshot answers at.
 *  @tparam store_type_ The store read, which must outlive the snapshot.
 *
 *  A snapshot holds no view and no level set, so it never delays a publication: every merge
 *  keeps the versions a pinned stamp reads, and every read re-enters the store through the
 *  view published then.
 */
template <typename store_type_>
class pinned_snapshot {
  public:

    /** The store read. */
    using store_t = store_type_;

    pinned_snapshot() noexcept = default;
    pinned_snapshot(pinned_snapshot const&) = delete;
    pinned_snapshot& operator=(pinned_snapshot const&) = delete;
    pinned_snapshot(pinned_snapshot&&) noexcept = default;
    pinned_snapshot& operator=(pinned_snapshot&&) noexcept = default;

    /** The newest stamp @p store published, pinned in its ledger until the snapshot dies. */
    [[nodiscard]] static pinned_snapshot of(store_t& store) noexcept {
        pinned_snapshot taken;
        taken.store_ = &store;
        taken.stamp_ = store.ledger().pin_reader(taken.pin_);
        return taken;
    }

    /** The stamp every read through this snapshot answers at. */
    [[nodiscard]] transaction_stamp_t stamp() const noexcept { return stamp_; }

    /** The store read, borrowed and outliving the snapshot. */
    [[nodiscard]] store_t& store() const noexcept { return *store_; }

  private:

    /** The store read, borrowed and outliving the snapshot. */
    store_t* store_ = nullptr;

    /** The pin that keeps every version at the stamp. */
    typename store_t::pin_t pin_;

    /** The stamp pinned. */
    transaction_stamp_t stamp_ = unwritten_stamp_k;
};

#pragma endregion Stamps

#pragma region Maintenance Claim

/**
 *  @brief The claim a database's maintenance, schema and blob writes run under, which callers park
 *      on rather than spin on, so a caller parked on one holder wakes once that holder lets it go,
 *      whoever takes it next.
 *
 *  The claim is one word kept as the epoch clock of ForkUnion's @c flat_pool: @b odd while held
 *  and @b even while free, stepped only by the take that finds it even and by its holder's
 *  release, so every epoch advances it by two. A take sets the held bit, which leaves an odd word
 *  as it was, and answers by the parity it read; a release adds one.
 *
 *  A committer finding it held returns at once rather than waiting, and every holder looks at the
 *  views again after it lets go. The fence after each release pairs with the fence before each
 *  attempt, so either the committer takes the claim, or the holder that refused it reads the views
 *  the committer published.
 *
 *  @tparam atomic_reference_ The reference the claim's word is owned through.
 *  @tparam waiter_type_ What a caller finding the claim held does between attempts,
 *      before it parks.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class maintenance_claim {
  public:

    /** Spins through @p waiter_type_, then parks on the claim's word. */
    using waiting_policy_t = st::standard_waiting_policy<waiter_type_>;

    /**
     *  @brief The claim of @p owner_type_, held for the life of the guard and handed back through
     *      the owner, which maintains whatever a commit left due meanwhile.
     *
     *  It orders whatever rewrites the schema, a tree or a blob: creates, drops, the maintenance
     *  commits pay for, the explicit flushes and compactions, and the caller's own blob. No read
     *  takes it. A commit takes it once it landed, or returns at once when another caller has it,
     *  since every holder looks at the views again after it lets go.
     */
    template <typename owner_type_>
    class guard {
      public:

        guard() noexcept = default;
        guard(guard const&) = delete;
        guard& operator=(guard const&) = delete;

        /** Takes @p owner's claim, parked while another caller holds it. */
        explicit guard(owner_type_& owner) noexcept : owner_(&owner) { owner.claim_.take(); }

        /** Takes @p owner's claim if no caller holds it, or answers a guard holding nothing. */
        [[nodiscard]] static guard taken_if_free(owner_type_& owner) noexcept {
            guard taken;
            if (owner.claim_.take_if_free()) taken.owner_ = &owner;
            return taken;
        }

        guard(guard&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}

        guard& operator=(guard&& other) noexcept {
            if (this == &other) return *this;
            release_();
            owner_ = std::exchange(other.owner_, nullptr);
            return *this;
        }

        ~guard() noexcept { release_(); }

        /** Whether this guard holds the claim. */
        [[nodiscard]] bool holding() const noexcept { return owner_ != nullptr; }

      private:

        /** Hands the claim back through the owner, which only a guard holding it does. */
        void release_() noexcept {
            if (owner_) std::exchange(owner_, nullptr)->release_claim_();
        }

        /** The owner whose claim this guard holds, null once moved from or never taken. */
        owner_type_* owner_ = nullptr;
    };

    /** Takes the claim, parked on each epoch it finds held until that holder lets it go, counted
     *  among the parked from its first refusal until it holds the claim, so no holder retakes the
     *  claim past it. */
    void take() noexcept {
        atomic_reference_<std::uint32_t> const epoch {epoch_};
        atomic_reference_<std::uint32_t> const parked {parked_};
        // Only the held bit of the old word is tested, so x86 sets it with `lock bts` and reloads
        // the word to park on.
        if (epoch.fetch_or(1, std::memory_order_acquire) % 2 == 0) return;
        // Counted across retries: a holder that reads none parked after its release is seen by
        // the next `fetch_or`.
        parked.store_add(1, std::memory_order_seq_cst);
        while (epoch.fetch_or(1, std::memory_order_acquire) % 2 != 0) {
            std::uint32_t const seen = epoch.load(std::memory_order_relaxed);
            if (seen % 2 != 0) waiting_policy_t {}.wait_until_changed(&epoch_, seen);
        }
        parked.store_sub(1, std::memory_order_relaxed);
    }

    /** Whether a blocking @c take is parked on the claim, which a holder about to retake
     *  it yields to. */
    [[nodiscard]] bool anyone_parked() const noexcept {
        return atomic_reference_<std::uint32_t const> {parked_}.load(std::memory_order_seq_cst) != 0;
    }

    /** Takes the claim if no caller holds it, and answers whether it did. */
    [[nodiscard]] bool take_if_free() noexcept {
        // Pairs with the fence after a release: either this reads the release, or that holder reads
        // what came before.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return atomic_reference_<std::uint32_t> {epoch_}.fetch_or(1, std::memory_order_acquire) % 2 == 0;
    }

    /** Parks until the caller holding the claim now lets it go, or returns at once while
     *  nobody holds it. */
    void wait_until_released() noexcept {
        atomic_reference_<std::uint32_t> const epoch {epoch_};
        std::uint32_t const seen = epoch.load(std::memory_order_acquire);
        if (seen % 2 == 0) return;
        // The wait reloads relaxed; this reload acquires, so a woken caller reads what the holder
        // wrote at once.
        do waiting_policy_t {}.wait_until_changed(&epoch_, seen);
        while (epoch.load(std::memory_order_acquire) == seen);
    }

    /** Hands the claim back, which only its holder does, and wakes every caller parked on it; the
     *  holder looks at the views again only after this. */
    void release() noexcept {
        atomic_reference_<std::uint32_t> {epoch_}.store_add(1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        waiting_policy_t {}.notify_waiters(&epoch_);
    }

  private:

    /** The claim's epoch: odd while held, even while free, stepped by whoever takes or
     *  lets it go. */
    alignas(fu::default_alignment_k) std::uint32_t epoch_ = 0;

    /** How many blocking takes are parked on the epoch right now. */
    std::uint32_t parked_ = 0;
};

#pragma endregion Maintenance Claim

/** How full a collection's memory runs would be with one more commit, against the bounds it was mounted with. */
enum class memory_pressure_t : std::uint8_t {

    /** The commit fits beside them. */
    fits_k,

    /** They are past their bounds, so a flush is due, which the commit that pushed them there runs once it lands. */
    flush_due_k,

    /** They are past twice their bounds, one flush in flight and one buffer behind it, so the committer flushes. */
    stalled_k,
};

/** How many memory runs one view of a collection names, and how many commits they hold together. */
struct memory_shape_t {

    /** The memory runs, which a point read probes one after another. */
    std::size_t runs = 0;

    /** The commits they hold: one per run of tier zero, and F times a run's for one tier above it. */
    std::uint64_t commits = 0;
};

/**
 *  @brief The levels and memory runs of one store behind its published view: what a commit prepares and publishes, and
 *      what the commit that leaves them due merges, flushes and compacts, each replacing the view and freeing what it
 *      replaced once the readers that entered before it leave.
 *
 *  A reader announces itself in the ledger's epochs, loads the published stamp and then the view, and walks immutable
 *  runs with no lock. A merge folds F runs of one tier into one run of the next tier, and a flush writes the memory
 *  runs of one view as one run of level zero, each with no lock held, then takes the leader word only to publish a
 *  view without exactly those runs. Merges, flushes, compactions and truncations are serialized by the store's owner.
 *  The volume, the budget and the ledger are borrowed and must outlive the runs.
 *
 *  @tparam memory_run_type_ The memory run a commit publishes, @c blobs::memory_run or @c strings::memory_run.
 *  @tparam atomic_reference_ The reference the published view, the budget and the ledger own their words through.
 *  @tparam waiter_type_ What a core waiting on a read, a barrier or the ledger does between attempts, before it parks.
 *  @tparam row_kit_type_ The SmashTable kit the tree's runs search with.
 */
template <typename key_slot_type_, typename memory_run_type_, typename volume_type_,
          template <typename> typename atomic_reference_, typename waiter_type_,
          typename row_kit_type_ = st::native_row_kit_t>
class published_runs {
  public:

#pragma region Types

    /** The key slot every run of the tree holds. */
    using key_slot_t = key_slot_type_;

    /** One commit's deltas, stamped, which nothing changes once a view names them. */
    using memory_run_t = memory_run_type_;

    /** The volume every run of the tree lives on. */
    using volume_t = volume_type_;

    /** The tree the flushed writes live in. */
    using tree_t = lsm_tree<key_slot_t, volume_t, atomic_reference_, waiter_type_, row_kit_type_>;

    /** What one slice of a merge of that tree writes into, lent by the owner of the store. */
    using merge_buffers_t = typename tree_t::merge_buffers_t;

    /** One level set of that tree, which a view names. */
    using level_set_t = typename tree_t::level_set_t;

    /** One immutable run of that tree. */
    using sorted_run_t = typename tree_t::sorted_run_t;

    /** The database's pool the runs of that tree live in. */
    using run_pool_t = typename tree_t::run_pool_t;

    /** The stamps the store's writes carry and its readers pin. */
    using ledger_t = commit_ledger<atomic_reference_, waiter_type_>;

    /** One stamp a reader or a view holds against the ledger. */
    using pin_t = stamp_pin<atomic_reference_, waiter_type_>;

    /** One reader inside the ledger's epochs, announced until it dies. */
    using reader_t = typename ledger_t::epochs_t::reader_t;

    /** The budget the memory runs are charged to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    /** The memory runs one view names, newest first. */
    using memory_view_t = collection_view<memory_run_t>;

    /** One version a merge or a flush carries over from a memory run. */
    using stamped_delta_t = stamped_delta<typename memory_run_t::stored_key_t>;

    /** Everything a reader of the store needs, which nothing changes once it is published. */
    struct view_t {

        /** The memory runs, newest first, every stamp of which is newer than any level's. */
        memory_view_t memory_runs;

        /** The levels below them. */
        level_set_t const* levels = nullptr;

        /** The oldest stamp a reader that pins none may read at, raised by every merge to the horizon it kept. */
        transaction_stamp_t oldest_readable = unwritten_stamp_k;

        /** Its room in the ledger's list of what the leader unpublished, once a publication replaces it. */
        unpublished_t unpublished;

        /** The budget the view is charged to, which takes its bytes back once the
         *  ledger frees it. */
        constraint_t* constraint = nullptr;

        /** How its runs are arranged, which a flush, a compaction or a merge of memory runs moves past and a commit,
         *  only putting a newer run ahead, keeps. */
        std::uint64_t layout_generation = 0;
    };

    /** The view a read walks and the stamp it reads at. */
    struct reading_t {

        /** The view loaded after the stamp, which names every commit at or below it. */
        view_t const* view = nullptr;

        /** The stamp the read answers at. */
        transaction_stamp_t stamp = head_stamp_k;
    };

#pragma endregion Types

#pragma region Construction

    published_runs() noexcept = default;
    published_runs(published_runs const&) = delete;
    published_runs& operator=(published_runs const&) = delete;

    published_runs(published_runs&& other) noexcept
        : config_(other.config_), constraint_(std::exchange(other.constraint_, nullptr)),
          ledger_(std::exchange(other.ledger_, nullptr)), tree_(std::move(other.tree_)),
          view_pin_(std::move(other.view_pin_)), view_(std::move(other.view_)),
          current_(std::exchange(other.current_, nullptr)), memory_runs_(std::move(other.memory_runs_)),
          memory_order_(std::exchange(other.memory_order_, memory_order_t {})),
          prepared_view_(std::exchange(other.prepared_view_, nullptr)),
          prepared_run_(std::exchange(other.prepared_run_, absent_run_k)), folded_floor_(other.folded_floor_) {}

    published_runs& operator=(published_runs&& other) noexcept {
        if (this == &other) return *this;
        release_views_();
        config_ = other.config_;
        constraint_ = std::exchange(other.constraint_, nullptr);
        ledger_ = std::exchange(other.ledger_, nullptr);
        tree_ = std::move(other.tree_);
        view_pin_ = std::move(other.view_pin_);
        view_ = std::move(other.view_);
        current_ = std::exchange(other.current_, nullptr);
        memory_runs_ = std::move(other.memory_runs_);
        memory_order_ = std::exchange(other.memory_order_, memory_order_t {});
        prepared_view_ = std::exchange(other.prepared_view_, nullptr);
        prepared_run_ = std::exchange(other.prepared_run_, absent_run_k);
        folded_floor_ = other.folded_floor_;
        return *this;
    }

    /** Gives the published view and any prepared one back to the budget; the memory runs go
     *  with their pool. */
    ~published_runs() noexcept { release_views_(); }

    /**
     *  @brief Mounts an empty tree over @p volume and publishes a view naming it alone.
     *  @param[in,out] volume The volume every run lands on, borrowed and outliving the runs.
     *  @param[in] config The bounds of the memory runs and the shape of the tree below.
     *  @param[in,out] constraint The budget the memory runs are charged to.
     *  @param[in,out] ledger The stamps the store's writes carry and its readers pin, outliving the runs.
     *  @param[in,out] runs The database's pool the tree's runs live in, outliving the store.
     */
    status_t mount(volume_t& volume, collection_config_t const& config, constraint_t& constraint, ledger_t& ledger,
                   run_pool_t& runs) noexcept {
        config_ = config;
        constraint_ = &constraint;
        ledger_ = &ledger;
        if (config.memory_runs_max_deltas == 0)
            return {status_code_t::invalid_argument_k, "A collection holds at least one write in memory"};

        auto [tree, mount_tree_status] = tree_t::mount(volume, config.lsm_tree, runs);
        if (mount_tree_status.failed()) return mount_tree_status;
        tree_ = std::move(tree);
        auto [records, records_status] = memory_run_pool_t::make(memory_run_records_(config), constraint);
        if (records_status.failed()) return records_status;
        memory_runs_ = std::move(records);
        auto [view, view_status] = make_view_(view_t {});
        if (view_status.failed()) return view_status;
        current_ = view;
        current_->levels = &tree_.levels();
        view_.replace(*current_);
        ledger.register_view(view_pin_);
        return {};
    }

    /** Returns every run to the volume and forgets every memory run, which destroys the data and which only a store no
     *  reader reaches and no manifest names may. */
    void destroy() noexcept {
        current_->memory_runs = memory_view_t::of(typename memory_view_t::runs_t {});
        for (std::uint32_t index = memory_order_.pop(memory_runs_); index != memory_order_t::absent_k;
             index = memory_order_.pop(memory_runs_))
            memory_runs_.vacate(memory_runs_.handle_of(memory_runs_[index]));
        ledger_->bound_view(view_pin_, head_stamp_k);
        tree_.destroy();
    }

#pragma endregion Construction

#pragma region Shape

    /** The bounds the store was mounted with. */
    [[nodiscard]] collection_config_t const& config() const noexcept { return config_; }

    /** The budget the memory runs are charged to, borrowed and outliving the runs. */
    [[nodiscard]] constraint_t& constraint() const noexcept { return *constraint_; }

    /** The stamps the store's writes carry and its readers pin, borrowed and outliving the runs. */
    [[nodiscard]] ledger_t& ledger() const noexcept { return *ledger_; }

    /** The tree the flushed writes live in, which a database compacts and names in its schema under its writer. */
    [[nodiscard]] tree_t& tree() noexcept { return tree_; }

    /** The tree the flushed writes live in. */
    [[nodiscard]] tree_t const& tree() const noexcept { return tree_; }

    /** Announces the calling executor thread @p thread_index in the ledger's epochs until the answer dies, which keeps
     *  every view loaded meanwhile and the runs it names readable. */
    [[nodiscard]] reader_t enter(std::size_t thread_index = outside_executor_k) const noexcept {
        return ledger_->epochs().enter(thread_index);
    }

    /** The view published last, which only a caller inside the ledger's epochs may walk. */
    [[nodiscard]] view_t const* view() const noexcept { return view_.load(); }

    /**
     *  @brief The view a read at @p stamp walks, and the stamp it answers at, for a caller inside the ledger's epochs.
     *
     *  A head read takes the published stamp before the view, so the view names every commit at or below it. A merge
     *  published after that stamp was taken may have dropped the version at it, which the view's oldest readable
     *  stamp tells, so the read takes a fresh stamp, at least that horizon, since the merge read it from the
     *  published stamp before publishing. A pinned reader keeps its stamp, which every merge keeps the versions of.
     */
    [[nodiscard]] reading_t reading(transaction_stamp_t stamp) const noexcept {
        for (;;) {
            transaction_stamp_t const at = stamp == head_stamp_k ? ledger_->published() : stamp;
            view_t const* const view = view_.load();
            if (stamp != head_stamp_k || at >= view->oldest_readable) return reading_t {view, at};
        }
    }

    /** Pins @p pin at @p stamp for a feed of the store's changes, so no merge folds a version above
     *  it, refusing with @c write_conflict_k a stamp a merge already folded past. */
    status_t pin_feed(pin_t& pin, transaction_stamp_t stamp) noexcept {
        return ledger_->pin_feed(pin, stamp, folded_floor_);
    }

    /** Deltas the memory runs hold, which no flush has written yet. */
    [[nodiscard]] std::size_t memory_deltas_count() const noexcept {
        reader_t const inside = enter();
        return memory_deltas_(*view_.load());
    }

    /** How many memory runs the published view names, and how many commits they hold, which merges by tier keep
     *  within (F − 1)(T + 1) runs for T = ⌈log_F commits⌉ once every commit returned, F being the enlarge factor. */
    [[nodiscard]] memory_shape_t memory_shape() const noexcept {
        reader_t const inside = enter();
        memory_shape_t shape;
        for (memory_run_t const* run : view_.load()->memory_runs.runs()) {
            std::uint64_t commits = 1;
            for (std::uint32_t tier = 0; tier != run->tier(); ++tier) commits *= config_.lsm_tree.level_enlarge_factor;
            shape.commits += commits;
            ++shape.runs;
        }
        return shape;
    }

    /** Deltas the memory runs hold plus key slots every level holds, one key counted once per run holding it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept {
        reader_t const inside = enter();
        view_t const& view = *view_.load();
        return memory_deltas_(view) + view.levels->population_count();
    }

    /** Bytes every run of every level takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept {
        reader_t const inside = enter();
        return view_.load()->levels->size_bytes();
    }

    /** How full the memory runs of the published view would be with one more commit of @p deltas deltas taking
     *  @p bytes bytes, as @c memory_run::memory_bytes counts them, beside them. */
    [[nodiscard]] memory_pressure_t memory_pressure(std::size_t deltas, std::uint64_t bytes) const noexcept {
        reader_t const inside = enter();
        view_t const& view = *view_.load();
        if (view.memory_runs.runs().empty()) return memory_pressure_t::fits_k;
        std::uint64_t const all_deltas = memory_deltas_(view) + deltas;
        std::uint64_t const all_bytes = memory_bytes_(view) + bytes;
        std::uint64_t const deltas_bound = config_.memory_runs_max_deltas;
        std::uint64_t const bytes_bound = config_.memory_runs_max_bytes;
        // One flush in flight and one buffer filling behind it, or every record but the commits
        // beside them taken.
        if (all_deltas > 2 * deltas_bound || all_bytes > 2 * bytes_bound ||
            view.memory_runs.runs().size() >= merged_memory_runs_(config_))
            return memory_pressure_t::stalled_k;
        if (all_deltas > deltas_bound || all_bytes > bytes_bound) return memory_pressure_t::flush_due_k;
        return memory_pressure_t::fits_k;
    }

    /** Whether the memory runs hold the bounds they were mounted with, so a flush is due. */
    [[nodiscard]] bool flush_due() const noexcept {
        reader_t const inside = enter();
        return flush_due_(*view_.load());
    }

    /** Whether the memory runs are due for a merge, holding F runs of tier zero, or for a flush; the commit that
     *  made them so runs either before it returns. */
    [[nodiscard]] bool maintenance_due() const noexcept {
        reader_t const inside = enter();
        return maintenance_due_(*view_.load());
    }

    /** Whether any level of the tree is over the bytes its number admits. */
    [[nodiscard]] bool needs_compaction() const noexcept {
        reader_t const inside = enter();
        return tree_.needs_compaction(*view_.load()->levels);
    }

#pragma endregion Shape

#pragma region Commits

    /**
     *  @brief Claims a memory-run record and the room of the view naming its run, before the commit
     *      takes the leader word, so @c prepare cannot refuse it.
     *  @return Success, @c capacity_exhausted_k while every record is taken, which the claimer
     *      meets by flushing or by waiting for @c vacated to move, or @c out_of_memory_k when the
     *      budget refuses the view.
     */
    status_t try_claim(memory_run_claim_t& claimed) noexcept {
        claimed.release();
        auto [record, claim_status] = memory_runs_.try_claim();
        if (claim_status.failed()) return claim_status;
        // Every run a view names holds a record of its own, so the pool's capacity bounds the
        // runs it names.
        typename memory_view_t::runs_t runs;
        if (runs.reserve(memory_runs_.capacity()) != st::status_t::success_k) {
            memory_runs_.vacate(record);
            return {status_code_t::out_of_memory_k, "No room to lay out a view"};
        }
        auto [view, view_status] = make_view_(view_t {memory_view_t::of(std::move(runs)), nullptr});
        if (view_status.failed()) {
            memory_runs_.vacate(record);
            return view_status;
        }
        claimed.owner = this;
        claimed.view = view;
        claimed.destroy_view = &destroy_view_;
        claimed.give_back = &give_back_claim_;
        claimed.record = record.index;
        claimed.generation = record.generation;
        return {};
    }

    /** Takes back the record and the view room of @p claimed, which this store claimed
     *  and nobody prepared. */
    void give_back(memory_run_claim_t& claimed) noexcept {
        destroy_view_(std::exchange(claimed.view, nullptr));
        memory_runs_.vacate(memory_run_handle_t {claimed.record, claimed.generation});
    }

    /** The count of memory-run records vacated so far, which a claimer finding every one
     *  taken waits on. */
    [[nodiscard]] std::uint32_t const& vacated() const noexcept { return memory_runs_.vacated(); }

    /**
     *  @brief Lays out the view naming @p claimed's run ahead of the current ones, out of the room
     *      the claim reserved, so @c publish cannot fail.
     *  @note Called by the thread holding the ledger's leader word, which then publishes or forgets
     *      it before it lets the word go.
     */
    void prepare(memory_run_claim_t&& claimed) noexcept {
        forget_prepared();
        assert(claimed.held() && claimed.view != nullptr && "A commit prepares the record its store claimed");
        view_t* const view = static_cast<view_t*>(std::exchange(claimed.view, nullptr));
        claimed.give_back = nullptr;
        memory_run_handle_t const record {claimed.record, claimed.generation};
        view->memory_runs.lay_out(memory_runs_[record.index].run, current_->memory_runs.runs());
        view->levels = current_->levels;
        view->oldest_readable = current_->oldest_readable;
        view->layout_generation = current_->layout_generation;
        prepared_view_ = view;
        prepared_run_ = record;
    }

    /** Moves @p run, stamped @p stamp, into the record @c prepare took and publishes the view it
     *  laid out, which the leader does before it publishes @p stamp itself, so a reader at the
     *  stamp finds the run. */
    void publish(memory_run_t&& run, transaction_stamp_t stamp) noexcept {
        assert(prepared_view_ && "A commit publishes only what its leader prepared");
        // The durable horizon stays below the oldest commit that only memory holds.
        if (current_->memory_runs.runs().empty()) ledger_->bound_view(view_pin_, stamp - 1);
        memory_runs_[prepared_run_.index].run = std::move(run);
        memory_order_.push(memory_runs_, std::exchange(prepared_run_, absent_run_k).index);
        replace_view_(std::exchange(prepared_view_, nullptr));
    }

    /** Drops the view @c prepare laid out and gives its record back, which the leader does for a
     *  commit it then refuses. */
    void forget_prepared() noexcept {
        if (prepared_view_ != nullptr) destroy_view_(std::exchange(prepared_view_, nullptr));
        memory_runs_.vacate(std::exchange(prepared_run_, absent_run_k));
    }

    /** Opens the runs @p runs names as the tree, each run an ingest stamped taking its stamp again,
     *  which a mount does before anything reads. */
    status_t restore(io_channel_t& channel, std::span<lsm_tree_run_record_t const> runs) noexcept {
        publication_t publication;
        publication.owner = this;
        return tree_.restore(channel, runs, publication.hook());
    }

#pragma endregion Commits

#pragma region Maintenance

    /**
     *  @brief Merges F memory runs of one tier into one run of the next tier, F being the level
     *      enlarge factor, until no tier holds F runs, the lowest tier and its oldest runs first.
     *  @param[in] fold What a change delta does, which composes the changes no reader tells apart.
     *
     *  Every merge reads immutable runs with no lock held, keeps the versions a pinned reader needs
     *  and every version above the durable horizon besides this store's own runs, and takes the
     *  leader word only to publish a view naming the merged run in place of exactly those runs;
     *  letting the word go, it waits for the readers that could hold them, then frees them before
     *  the next merge.
     *
     *  @note The owner serializes it with flushes, compactions and truncations of the store, and
     *      the calling thread is inside no epoch.
     */
    template <utilities::fold_concept fold_type_>
    status_t merge_memory_runs(fold_type_ const& fold) noexcept {
        std::size_t const fan_in = config_.lsm_tree.level_enlarge_factor;
        // Every pass reuses the room the first one grew.
        st::vector<memory_run_t const*> group;
        st::vector<transaction_stamp_t> readers;
        // The replaced runs, freed once the publication waited out every reader holding one.
        st::vector<memory_run_handle_t> away;
        stamped_deltas_t versions {constrained_allocator<stamped_delta_t, atomic_reference_> {*constraint_}};
        // The changes a merge folds sit here until the merged run copies them.
        utilities::local_arena_t folded;
        utilities::arena_allocator<byte_t> folded_bytes = folded.allocator();
        if (group.resize(fan_in) != st::status_t::success_k || away.reserve(fan_in) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A merge could not list its memory runs"};
        for (;;) {
            {
                reader_t const inside = enter();
                std::span<memory_run_t const* const> const due = due_group_(view_.load()->memory_runs.runs());
                if (due.empty()) return {};
                // Only the store's owner frees memory runs, so these outlive the reader.
                for (std::size_t index = 0; index != fan_in; ++index) group[index] = due[index];
            }

            if (status_t const listed = ledger_->pinned_readers(readers); listed.failed()) return listed;
            away.clear();
            versions.clear();
            utilities::arena_rewind(folded_bytes.arena);
            // Claimed before the merge is built, so a full pool skips it, leaving the runs to the
            // flush the stall runs.
            auto [record, claim_status] = memory_runs_.try_claim();
            if (claim_status == status_code_t::capacity_exhausted_k) return {};
            if (claim_status.failed()) return claim_status;
            compaction_stamps_t survival;
            survival.pinned = readers.data();
            survival.pinned_count = readers.size();
            // The flush of this run prints a manifest promising the other collections' horizon,
            // and a mount truncated there needs every version at or below it, as the flush
            // itself keeps them.
            survival.horizon = ledger_->fold_horizon(ledger_->horizon_besides(view_pin_), folded_floor_);
            if (status_t const kept = kept_versions(std::span<memory_run_t const* const> {group}, survival, fold,
                                                    versions, folded_bytes);
                kept.failed()) {
                memory_runs_.vacate(record);
                return kept;
            }
            auto [merged, merged_status] = merged_run_(std::span<memory_run_t const* const> {group},
                                                       std::span<stamped_delta_t const> {versions});
            if (merged_status.failed()) {
                memory_runs_.vacate(record);
                return merged_status;
            }
            memory_runs_[record.index].run = std::move(merged);
            if (status_t const published = publish_merged_(std::span<memory_run_t const* const> {group}, record, away,
                                                           survival.horizon);
                published.failed()) {
                memory_runs_.vacate(record);
                return published;
            }
            // The merged run views the replaced runs' buffers, so it takes them before they go.
            for (memory_run_handle_t const replaced : away) {
                memory_runs_[record.index].run.inherit(std::move(memory_runs_[replaced.index].run));
                memory_runs_.vacate(replaced);
            }
        }
    }

    /**
     *  @brief Writes the memory runs of the published view as runs of level zero with no lock held,
     *      then publishes a view naming those runs instead of exactly the memory runs, keeping
     *      every memory run committed meanwhile.
     *  @param[in] drain Called as <tt>drain(tree, flushed, survival, published)</tt>, answering what
     *      the flush answered.
     *  @note The owner serializes flushes, compactions and truncations of one store, and the
     *      calling thread is inside no epoch, since the publication waits for every reader that
     *      could hold a run it frees.
     *
     *  @p drain gets the memory runs to write, newest first, the versions a pinned reader or a
     *  mount still needs, and the hook to hand @c lsm_tree::flush, which it calls on @c tree once
     *  it laid the runs out.
     */
    template <typename drain_type_>
    status_t flush(drain_type_&& drain) noexcept {
        publication_t publication;
        publication.owner = this;
        {
            reader_t const inside = enter();
            std::span<memory_run_t const* const> const runs = view_.load()->memory_runs.runs();
            if (runs.empty()) return {};
            // Only the store's owner frees memory runs, merging or flushing one at a time, so these outlive the reader.
            if (publication.flushed.resize(runs.size()) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A flush could not list its memory runs"};
            for (std::size_t index = 0; index != runs.size(); ++index) publication.flushed[index] = runs[index];
        }

        // A replaced version survives as a merge would keep it: for a pinned reader, or for a mount promising the
        // durable horizon this flush leaves; at or below both, every stamp reads alike, so the run packs only above.
        st::vector<transaction_stamp_t> readers;
        if (status_t const listed = ledger_->pinned_readers(readers); listed.failed()) return listed;
        compaction_stamps_t survival;
        survival.pinned = readers.data();
        survival.pinned_count = readers.size();
        survival.horizon = ledger_->fold_horizon(ledger_->horizon_besides(view_pin_), folded_floor_);
        survival.floor = fu::min_of(ledger_->floor_besides(view_pin_), survival.horizon);
        publication.oldest_readable = survival.horizon;
        return drain(tree_, std::span<memory_run_t const* const> {publication.flushed}, survival, publication.hook());
    }

    /**
     *  @brief Merges the shallowest level that is over its bounds into the level below it.
     *  @param[in,out] reading_channel The channel the merge gathers source values on, held by the calling thread.
     *  @param[in,out] writing_channel The channel the merge writes its new runs on, which is not @p reading_channel.
     *  @param[in] compactor The compactor the database picked at open: the calling thread, or its warm device
     *      compactor.
     *  @param[in] fold What a change delta does, which composes the changes no reader tells apart.
     *  @param[in] device_fold What composes the changes of the merge on a device, or @c no_device_fold_t where nothing
     *      does, which keeps every merge holding a change on the host.
     *  @param[in] published What to call once the merged set is live and before a consumed run is dropped.
     *  @param[in] fan_out The threads a merge on the host fans its slices out over, or one slice on
     *      the calling thread.
     */
    template <utilities::fold_concept fold_type_, typename device_fold_type_>
    expected<compaction_outcome_t> compact(io_channel_t& reading_channel, io_channel_t& writing_channel,
                                           std::span<merge_buffers_t> buffers, compactor_ref_t const& compactor,
                                           fold_type_ const& fold, device_fold_type_ const& device_fold,
                                           lsm_tree_publication_hook_t const& published,
                                           compaction_fan_out_t const& fan_out = compaction_fan_out_t {}) noexcept {
        // A pinned reader keeps the version it reads, and the durable horizon keeps the version a mount falls back to.
        st::vector<transaction_stamp_t> readers;
        if (status_t const listed = ledger_->pinned_readers(readers); listed.failed()) return listed;
        compaction_stamps_t stamps;
        stamps.pinned = readers.data();
        stamps.pinned_count = readers.size();
        stamps.horizon = ledger_->fold_horizon(ledger_->durable_stamp(), folded_floor_);
        stamps.floor = fu::min_of(ledger_->floor(), stamps.horizon);
        publication_t publication;
        publication.owner = this;
        publication.oldest_readable = stamps.horizon;
        publication.chained = &published;
        return tree_.compact(reading_channel, writing_channel, buffers, compaction_placement_t::automatic_k, compactor,
                             nullptr, fold, device_fold, publication.hook(), stamps, fan_out);
    }

    /**
     *  @brief Publishes @p runs, written at stamp zero, as one commit of their own, stamped with the stamp the commit
     *      draws and placed so no shallower level holds a key of theirs, once the store holds no memory run.
     *  @param[in,out] runs The runs in ascending order of their keys, emptied: moved into the tree,
     *      or given back.
     *  @param[in] flush Called as @c flush() before every attempt, writing the memory runs out as the store does.
     *  @return The stamp every version of @p runs carries, the stamp published for no runs, or the status that stopped
     *      it, @c contended_k once memory runs kept reappearing.
     *  @note The owner serializes it with flushes, compactions and truncations of the store, and the calling thread is
     *      inside no epoch.
     */
    template <typename runs_type_, typename flush_type_>
    expected<transaction_stamp_t> publish_ingest(runs_type_& runs, flush_type_&& flush) noexcept {
        if (runs.empty()) return ledger_->published();
        expected<transaction_stamp_t> landed = status_t {status_code_t::contended_k,
                                                         "Commits kept refilling the memory runs an ingest waits out"};
        for (std::size_t attempt = 0; attempt != ingest_attempts_k; ++attempt) {
            if (status_t const flushed = flush(); flushed.failed()) {
                landed = flushed;
                break;
            }
            ledger_->lead();
            // A commit landing after the flush put memory runs ahead of the levels, whose stamps sit below the
            // ingest's.
            if (!current_->memory_runs.runs().empty()) {
                ledger_->stop_leading();
                continue;
            }
            landed = publish_ingest_leading_(std::span<sorted_run_t> {runs});
            break;
        }
        // A set the tree refused gave back the runs it took, and the rest are still attached here.
        for (sorted_run_t const& run : runs)
            if (run.attached()) tree_.volume().deallocate(run.extent());
        runs.clear();
        return landed;
    }

    /** Drops every version a mount found stamped above @p ceiling, which only a store holding no memory run takes. */
    template <utilities::fold_concept fold_type_>
    status_t truncate_above(io_channel_t& reading_channel, io_channel_t& writing_channel,
                            std::span<merge_buffers_t> buffers, transaction_stamp_t ceiling, fold_type_ const& fold,
                            lsm_tree_publication_hook_t const& published) noexcept {
        // Merges before the mount may have folded anything at or below the stamp it resumes from.
        folded_floor_ = fu::max_of(folded_floor_, ceiling);
        {
            reader_t const inside = enter();
            view_t const& taken = *view_.load();
            if (!taken.memory_runs.runs().empty())
                return {status_code_t::invalid_argument_k, "A collection truncates only its runs"};
            if (taken.levels->newest_stamp() <= ceiling) return {};
        }
        publication_t publication;
        publication.owner = this;
        publication.chained = &published;
        return tree_.truncate_above(reading_channel, writing_channel, buffers, ceiling, fold, publication.hook());
    }

#pragma endregion Maintenance

  private:

#pragma region Private Types

    /** The versions one merge carries over, before and after they are sorted into a run's order. */
    using stamped_deltas_t = st::vector<stamped_delta_t, constrained_allocator<stamped_delta_t, atomic_reference_>>;

    /** The order the store published its memory runs in, threaded through their records. */
    using memory_order_t = st::intrusive_list<std::uint32_t>;

    /** One memory run in a record that never moves, so a view names the run by its address. */
    struct memory_run_record_t {

        /** The run, which a view names once a commit or a merge published it. */
        memory_run_t run;

        /** Stepped by every vacate, as the pool asks. */
        std::uint32_t generation = 0;

        /** The record published after this one, or none. */
        std::uint32_t next = memory_order_t::absent_k;

        /** The record published before this one, or none. */
        std::uint32_t previous = memory_order_t::absent_k;

        memory_run_record_t() noexcept = default;
        memory_run_record_t(memory_run_record_t&& other) noexcept : run(std::move(other.run)) {}

        /** Carries the run alone, so a record keeps its generation and its place in the order. */
        memory_run_record_t& operator=(memory_run_record_t&& other) noexcept {
            run = std::move(other.run);
            return *this;
        }
    };

    /** The records of a store's memory runs, charged to its budget. */
    using memory_run_pool_t = lsm_record_pool<memory_run_record_t, std::uint32_t, atomic_reference_, waiter_type_>;

    /** One memory-run record and the generation it was taken at. */
    using memory_run_handle_t = typename memory_run_pool_t::handle_t;

    /** A handle naming no record. */
    static constexpr memory_run_handle_t absent_run_k {memory_run_pool_t::absent_k, 0};

    /** What a view is allocated and freed through, charged to the store's budget. */
    using view_allocator_t = constrained_allocator<view_t, atomic_reference_>;

    /** What a publication of a new level set hands its hooks: the memory runs it wrote out, the view naming the set,
     *  and the hook it calls once no reader is left in the views before it. */
    struct publication_t {

        /** The runs publishing. */
        published_runs* owner = nullptr;

        /** The memory runs a flush wrote out, newest first, as the view it read named them; none for a merge. */
        st::vector<memory_run_t const*> flushed;

        /** The horizon the merge kept, which the published view raises its oldest readable stamp to. */
        transaction_stamp_t oldest_readable = unwritten_stamp_k;

        /** The view laid out under the leader word just before the set becomes current. */
        view_t* next = nullptr;

        /** The records of the flushed runs, unlinked under the leader word and freed once no reader
         *  can hold one. */
        st::vector<memory_run_handle_t> freed;

        /** The hook of whoever records the new set, called last, or null for none. */
        lsm_tree_publication_hook_t const* chained = nullptr;

        /** The hooks a tree calls with this publication as their context. */
        [[nodiscard]] lsm_tree_publication_hook_t hook() noexcept {
            lsm_tree_publication_hook_t hook;
            hook.prepare = &prepare_levels_;
            hook.notify = &publish_levels_;
            hook.context = this;
            return hook;
        }
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** Times an ingest flushes and looks again for memory runs before it gives up. */
    static constexpr std::size_t ingest_attempts_k = 8;

    /** What the commit of an ingest hands its publication: the view naming the
     *  tree's prepared set. */
    struct ingest_publication_t {

        /** The runs publishing. */
        published_runs* owner = nullptr;

        /** The view naming the set the tree prepared. */
        view_t* next_view = nullptr;
    };

    /** Publishes the view of an ingest under the stamp its commit drew, which the ingested runs carry already. */
    static void publish_ingest_view_(void* context, transaction_stamp_t) noexcept {
        ingest_publication_t& ingestion = *static_cast<ingest_publication_t*>(context);
        published_runs& owner = *ingestion.owner;
        owner.tree_.adopt();
        ingestion.next_view->levels = &owner.tree_.levels();
        owner.replace_view_(std::exchange(ingestion.next_view, nullptr));
    }

    /** Stamps @p runs with the stamp the next commit draws, lays out the set and the view naming them and commits,
     *  which only the thread holding the leader word does, letting it go on every path. */
    expected<transaction_stamp_t> publish_ingest_leading_(std::span<sorted_run_t> runs) noexcept {
        transaction_stamp_t const stamp = ledger_->next_stamp_leading();
        for (sorted_run_t& run : runs)
            if (status_t const stamped = run.assign_stamp(stamp); stamped.failed()) {
                ledger_->stop_leading();
                return stamped;
            }
        ingest_publication_t ingestion;
        ingestion.owner = this;
        auto [view, view_status] = next_view_({}, current_->oldest_readable, current_->layout_generation + 1);
        if (view_status.failed()) {
            ledger_->stop_leading();
            return view_status;
        }
        ingestion.next_view = view;
        if (status_t const prepared = tree_.prepare_ingest(runs); prepared.failed()) {
            destroy_view_(view);
            ledger_->stop_leading();
            return prepared;
        }
        commit_request_t request;
        request.context = &ingestion;
        request.publish = &publish_ingest_view_;
        transaction_stamp_t const landed = ledger_->commit_leading(request);
        // Letting the leader word go waited out every reader of the replaced view, so the replaced set is unreachable.
        tree_.retire();
        return landed;
    }

    /** The run of the next tier that @p versions, kept of the runs of @p group, make with every
     *  range tombstone of @p group. */
    expected<memory_run_t> merged_run_(std::span<memory_run_t const* const> group,
                                       std::span<stamped_delta_t const> versions) const noexcept {
        typename memory_run_t::allocator_t const allocator {*constraint_};
        auto [tombstones, tombstones_status] = gathered_range_tombstones(group, allocator);
        if (tombstones_status.failed()) return tombstones_status;
        return memory_run_t::merged(group, versions, std::move(tombstones), group.back()->oldest_stamp(),
                                    group.back()->tier(), allocator);
    }

    /** Deltas every memory run of @p view holds. */
    [[nodiscard]] static std::size_t memory_deltas_(view_t const& view) noexcept {
        std::size_t deltas = 0;
        for (memory_run_t const* run : view.memory_runs.runs()) deltas += run->size();
        return deltas;
    }

    /** Bytes every memory run of @p view charges against the bounds, as @c memory_run::memory_bytes counts them. */
    [[nodiscard]] static std::uint64_t memory_bytes_(view_t const& view) noexcept {
        std::uint64_t bytes = 0;
        for (memory_run_t const* run : view.memory_runs.runs()) bytes += run->memory_bytes();
        return bytes;
    }

    /** Whether the memory runs of @p view hold the bounds they were mounted with, or as many
     *  runs as stall a commit, past which a merge may find no record to claim and only a
     *  flush frees them. */
    [[nodiscard]] bool flush_due_(view_t const& view) const noexcept {
        return memory_deltas_(view) >= config_.memory_runs_max_deltas ||
               memory_bytes_(view) >= config_.memory_runs_max_bytes ||
               view.memory_runs.runs().size() >= merged_memory_runs_(config_);
    }

    /** Whether @p view is due for a merge, holding F runs of tier zero, or for a flush. */
    [[nodiscard]] bool maintenance_due_(view_t const& view) const noexcept {
        std::size_t newest = 0;
        for (memory_run_t const* run : view.memory_runs.runs()) {
            if (run->tier() != 0) break;
            ++newest;
        }
        return newest >= config_.lsm_tree.level_enlarge_factor || flush_due_(view);
    }

    /** Tiers memory runs reach while commits stall only past twice the delta bound, T = ⌈log_F(2
     *  × max deltas)⌉. */
    [[nodiscard]] static std::size_t memory_run_tiers_(collection_config_t const& config) noexcept {
        std::size_t tiers = 0;
        for (std::uint64_t covered = 1; covered < 2 * std::uint64_t {config.memory_runs_max_deltas};
             covered *= config.lsm_tree.level_enlarge_factor)
            ++tiers;
        return tiers;
    }

    /** Memory runs a merged view holds at most, (F − 1)(T + 1), past which commits stall
     *  until a flush. */
    [[nodiscard]] static std::size_t merged_memory_runs_(collection_config_t const& config) noexcept {
        return (config.lsm_tree.level_enlarge_factor - 1) * (memory_run_tiers_(config) + 1);
    }

    /** Memory-run records a store holds, (F − 1)(T + 1) + concurrency limit + F: a merged view, the
     *  commits landing past its stall, and the run one merge adds before it frees F. */
    [[nodiscard]] static std::size_t memory_run_records_(collection_config_t const& config) noexcept {
        return merged_memory_runs_(config) + config.concurrency_limit + config.lsm_tree.level_enlarge_factor;
    }

    /**
     *  @brief The oldest F runs of the lowest tier holding F of @p runs, newest first, or none.
     *
     *  Commits prepend runs of tier zero and a merge puts its run where the oldest it replaced sat, so tiers never
     *  fall from the newest run to the oldest and every tier's runs sit side by side.
     */
    [[nodiscard]] std::span<memory_run_t const* const> due_group_(
        std::span<memory_run_t const* const> runs) const noexcept {
        std::size_t const fan_in = config_.lsm_tree.level_enlarge_factor;
        for (std::size_t first = 0; first != runs.size();) {
            std::size_t end = first;
            while (end != runs.size() && runs[end]->tier() == runs[first]->tier()) ++end;
            if (end - first >= fan_in) return runs.subspan(end - fan_in, fan_in);
            first = end;
        }
        return {};
    }

    /** A view naming @p runs, below levels its caller fills in, arranged as @p layout_generation
     *  says, charged to the budget, which no reader reaches until it is published. */
    expected<view_t*> next_view_(std::span<memory_run_t const* const> runs, transaction_stamp_t oldest_readable,
                                 std::uint64_t layout_generation) noexcept {
        typename memory_view_t::runs_t named;
        if (named.resize(runs.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to lay out a view"};
        for (std::size_t index = 0; index != runs.size(); ++index) named[index] = runs[index];
        auto [view, view_status] = make_view_(view_t {memory_view_t::of(std::move(named)), nullptr, oldest_readable});
        if (view_status.failed()) return view_status;
        view->layout_generation = layout_generation;
        return view;
    }

    /** Allocates @p laid_out on the budget, where no reader reaches it until it is published. */
    expected<view_t*> make_view_(view_t&& laid_out) noexcept {
        view_allocator_t allocator {*constraint_};
        view_t* const view = allocator.allocate(1);
        if (view == nullptr) return status_t {status_code_t::out_of_memory_k, "The budget refused a view"};
        std::construct_at(view, std::move(laid_out));
        view->constraint = constraint_;
        return view;
    }

    /** Destroys @p view and gives its bytes back to the budget it was charged to, which the ledger
     *  calls once no reader can hold it. */
    static void destroy_view_(void* view) noexcept {
        view_t* const held = static_cast<view_t*>(view);
        view_allocator_t allocator {*held->constraint};
        std::destroy_at(held);
        allocator.deallocate(held, 1);
    }

    /** Gives @p claimed back to the runs that claimed it, which a claim taken outside any
     *  database names as owner. */
    static void give_back_claim_(memory_run_claim_t& claimed) noexcept {
        static_cast<published_runs*>(claimed.owner)->give_back(claimed);
    }

    /** Destroys the published view and a prepared one, which only an owner no
     *  reader reaches does. */
    void release_views_() noexcept {
        if (prepared_view_ != nullptr) destroy_view_(std::exchange(prepared_view_, nullptr));
        if (current_ != nullptr) destroy_view_(std::exchange(current_, nullptr));
    }

    /** Publishes @p newer in place of the current view, which only the thread holding the leader word does, and hands
     *  the view it replaces to the ledger, which frees it once that thread lets the word go and its readers left. */
    void replace_view_(view_t* newer) noexcept {
        view_.replace(*newer);
        view_t* const older = std::exchange(current_, newer);
        ledger_->unpublish(older->unpublished, older, &destroy_view_);
    }

    /** The record holding @p run, which the store owns, found by a walk in publication order. */
    [[nodiscard]] memory_run_handle_t record_of_(memory_run_t const* run) noexcept {
        std::uint32_t index = memory_order_.oldest;
        while (&memory_runs_[index].run != run) index = memory_runs_[index].next;
        return memory_runs_.handle_of(memory_runs_[index]);
    }

    /** Unlinks the record holding @p run from the publication order and answers it, for its caller
     *  to free once no reader can hold the run. */
    [[nodiscard]] memory_run_handle_t unlink_(memory_run_t const* run) noexcept {
        memory_run_handle_t const record = record_of_(run);
        [[maybe_unused]] st::status_t const erased = memory_order_.erase(memory_runs_, record.index);
        return record;
    }

    /**
     *  @brief Takes the leader word and lays out the view the tree's next level set will be named by: the current
     *      memory runs but those the publication wrote out, which are the oldest, since commits only prepend.
     *  @return Success with the word held, or the status that made the tree abandon its set, with the word let go.
     */
    static status_t prepare_levels_(void* context) noexcept {
        publication_t& publication = *static_cast<publication_t*>(context);
        published_runs& owner = *publication.owner;
        owner.ledger_->lead();
        std::span<memory_run_t const* const> const runs = owner.current_->memory_runs.runs();
        std::size_t const kept = runs.size() - publication.flushed.size();
        assert(runs.size() >= publication.flushed.size() &&
               (publication.flushed.empty() || runs[kept] == publication.flushed[0]) &&
               "A flush writes out the oldest memory runs");
        if (publication.freed.reserve(publication.flushed.size()) != st::status_t::success_k) {
            owner.ledger_->stop_leading();
            return {status_code_t::out_of_memory_k, "A flush could not take its memory runs over"};
        }
        auto [next, next_status] = owner.next_view_(
            runs.first(kept), fu::max_of(owner.current_->oldest_readable, publication.oldest_readable),
            owner.current_->layout_generation + 1);
        if (next_status.failed()) {
            owner.ledger_->stop_leading();
            return next_status;
        }
        publication.next = next;
        return {};
    }

    /** Publishes the view @c prepare_levels_ laid out, naming the tree's new level set, takes the flushed runs over
     *  and bounds the durable stamp by what memory still holds, lets the leader word go, which waits until no reader
     *  is left that could hold a flushed run or the replaced set, frees the runs, and calls the hook it chains to. */
    static status_t publish_levels_(void* context) noexcept {
        publication_t& publication = *static_cast<publication_t*>(context);
        published_runs& owner = *publication.owner;
        publication.next->levels = &owner.tree_.levels();
        owner.replace_view_(std::exchange(publication.next, nullptr));
        if (!publication.flushed.empty()) {
            for (memory_run_t const* flushed : publication.flushed) [[maybe_unused]]
                st::status_t const taken = publication.freed.push_back(st::assume_reserved, owner.unlink_(flushed));
            std::span<memory_run_t const* const> const kept = owner.current_->memory_runs.runs();
            owner.ledger_->bound_view(owner.view_pin_, kept.empty() ? head_stamp_k : kept.back()->oldest_stamp() - 1);
            publication.flushed.clear();
        }
        // Every reader that could hold a flushed run entered before the view replaced above, which this waits out.
        owner.ledger_->stop_leading();
        for (memory_run_handle_t const freed : publication.freed) owner.memory_runs_.vacate(freed);
        publication.freed.clear();
        lsm_tree_publication_hook_t const* const chained = publication.chained;
        if (chained == nullptr || chained->notify == nullptr) return {};
        return chained->notify(chained->context);
    }

    /**
     *  @brief Takes the leader word, publishes a view naming the run of @p merged where exactly the
     *      runs of @p group sat, unlinks their records into @p away, and lets the word go, which
     *      returns once no reader can reach them.
     *  @param[in] group The runs merged, newest first, which commits since may have put newer
     *      runs ahead of.
     *  @param[in] merged The record of the run that replaces them, linked as the newest
     *      the store published.
     *  @param[out] away Room reserved for the records of @p group, which the caller frees.
     *  @param[in] horizon The horizon the merge kept versions above, which a head reader below
     *      it reads past.
     */
    status_t publish_merged_(std::span<memory_run_t const* const> group, memory_run_handle_t merged,
                             st::vector<memory_run_handle_t>& away, transaction_stamp_t horizon) noexcept {
        ledger_->lead();
        std::span<memory_run_t const* const> const runs = current_->memory_runs.runs();
        std::size_t first = 0;
        while (runs[first] != group.front()) ++first;
        assert(first + group.size() <= runs.size() && runs[first + group.size() - 1] == group.back() &&
               "A merged group stays side by side, as only commits prepend and only its owner merges or flushes");
        typename memory_view_t::runs_t laid_out;
        if (laid_out.resize(runs.size() - group.size() + 1) != st::status_t::success_k) {
            ledger_->stop_leading();
            return {status_code_t::out_of_memory_k, "No room to lay out a merged view"};
        }
        for (std::size_t index = 0; index != first; ++index) laid_out[index] = runs[index];
        laid_out[first] = &memory_runs_[merged.index].run;
        for (std::size_t index = first + group.size(); index != runs.size(); ++index)
            laid_out[index - group.size() + 1] = runs[index];
        auto [next, next_status] = make_view_(view_t {memory_view_t::of(std::move(laid_out)), current_->levels,
                                                      fu::max_of(current_->oldest_readable, horizon)});
        if (next_status.failed()) {
            ledger_->stop_leading();
            return next_status;
        }
        next->layout_generation = current_->layout_generation + 1;
        replace_view_(next);
        for (memory_run_t const* replaced : group) [[maybe_unused]]
            st::status_t const taken = away.push_back(st::assume_reserved, unlink_(replaced));
        memory_order_.push(memory_runs_, merged.index);
        ledger_->stop_leading();
        return {};
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The bounds the store was mounted with. */
    collection_config_t config_ {};

    /** The budget the memory runs are charged to, null until the runs are mounted. */
    constraint_t* constraint_ = nullptr;

    /** The stamps the store's writes carry and its readers pin, borrowed and outliving the runs. */
    ledger_t* ledger_ = nullptr;

    /** The tree the flushed writes live in. */
    tree_t tree_;

    /** Registers the published view with the ledger, bounding the durable stamp below the oldest memory run. */
    pin_t view_pin_;

    /** The view readers load, which @c current_ owns. */
    published_view<view_t, atomic_reference_> view_;

    /** The view published last, allocated on the budget and replaced only under the
     *  ledger's leader word. */
    view_t* current_ = nullptr;

    /** The records of every memory run the store holds, charged to the budget, freed by the flush
     *  that writes them out or the merge that replaces them, once no reader can reach them. */
    memory_run_pool_t memory_runs_;

    /** The records the current view names, oldest published first, which a flush and
     *  a merge unlink. */
    memory_order_t memory_order_;

    /** The view a leader laid out for the commit it prepared, published or forgotten before it lets the word go. */
    view_t* prepared_view_ = nullptr;

    /** The record that view names for the commit's run, taken by @c prepare and linked
     *  by @c publish. */
    memory_run_handle_t prepared_run_ = absent_run_k;

    /** The stamp at or below which a merge of the store may have folded or dropped versions, so a
     *  feed below it is refused; touched under the ledger's claim. */
    transaction_stamp_t folded_floor_ = 0;

#pragma endregion Private Members
};

} // namespace unum::ustore::blobs
