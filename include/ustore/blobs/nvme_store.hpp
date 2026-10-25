/**
 *  @file include/ustore/blobs/nvme_store.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The NVMe store of one collection: immutable memory runs and a value cache in front of an LSM tree of sorted
 *      runs, the one view every read runs against, and the ordered walk over any value layout.
 *
 *  Three places can answer a key, and they are consulted newest first: the memory runs hold what has
 *  not been flushed, the value cache holds what a point read already paid for, and the tree holds the
 *  rest. Only the third moves bytes, and it moves them through the level set one view names, so a
 *  batched select reaches the queue pair's depth rather than the size of whatever batch it was handed.
 *  Nothing here allocates an I/O buffer. A caller's span is never registered memory, so the bytes are
 *  copied out of the channel's own blocks either way, and that one copy is the whole of the read path.
 *
 *  A value cache hit is copied out of a slot the cache refills in place, between two loads of the slot's sequence,
 *  so a read that finds a key there costs one hash probe and one copy, and a displaced value is never freed apart.
 *  The bound is a count rather than a byte total, derived once from the budget and
 *  the width the collection's values take.
 *
 *  A read copies no run and moves no byte to find its place: it announces itself on its own slot of the ledger's
 *  epochs, loads the published stamp and then the view, and walks the immutable runs the view names. A
 *  publication replaces the view beside rather than in place, so the runs a reader walks stay readable until
 *  it leaves. A snapshot is a pinned stamp, which every merge keeps the versions of.
 *
 *  A run answers a key slot by sorted position, so an ordered walk is a merge over one cursor per run, memory
 *  runs included, and nothing is materialized. Where several runs carry the same key the newest memory run
 *  wins, then the shallowest level, and inside a level that may overlap the youngest run. There is no prefetcher,
 *  and adding one would be a mistake rather than an optimization: a scan that wants bytes hands the next keys to
 *  @c nvme_store::select, which already refills the ring as completions retire, while a second pipeline reading
 *  ahead on the same channel would poll the first one's completions and drop them.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::max`, `std::min`, `std::sort`, `std::stable_sort`
#include <array>     // `std::array`
#include <atomic>    // `std::memory_order_acquire`
#include <bit>       // `std::bit_floor`
#include <memory>    // `std::unique_ptr`
#include <new>       // `std::nothrow`
#include <span>      // `std::span`
#include <utility>   // `std::exchange`, `std::move`, `std::swap`

#include <smashtable/basic_hash_table.hpp> // `st::hash_map`, `st::assume_reserved`
#include <smashtable/basic_ring.hpp>       // `st::basic_ring`, `st::ring_eviction_t`
#include <smashtable/basic_vector.hpp>     // `st::basic_vector`
#include <smashtable/shared.hpp>           // `st::commit_order_t`, `st::latest_snapshot_k`

#include "ustore/blobs/nvme_collection_view.hpp" // `collection_view`, `epoch_domain`, `published_view`
#include "ustore/blobs/nvme_memory_run.hpp"      // `keyed_delta`, `memory_run`, `sealed_deltas`
#include "ustore/blobs/nvme_key_slot.hpp"        // `limit`, `stored_value_size_t`, `value_layout_t`
#include "ustore/blobs/nvme_tree.hpp"            // `lsm_tree`, `lsm_tree_position_t`, `collection_config_t`
#include "ustore/utilities/folds.hpp"            // `fold_concept`, `fold_delta_pair`, `last_write_fold_t`
#include "ustore/utilities/nvme_channels.hpp"    // `io_channel_t`
#include "ustore/utilities/nvme_ring.hpp"        // `resource_constraint`, `constrained_allocator`
#include "ustore/utilities/ram_buffers.hpp"      // `local_arena_t`, `arena_allocator`
#include "ustore/utilities/types.hpp"            // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::blobs {

using utilities::constrained_allocator;
using utilities::constrained_buffer;
using utilities::outside_executor_k;
using utilities::resource_constraint;

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

/**
 *  @brief The commit order one database draws its stamps from, every stamp its readers and views pin, the epochs its
 *      readers announce, and the sequencer every commit lands through.
 *
 *  Several databases share one ledger when one commit spans them all, as a graph's two databases do. Pins are
 *  intrusive, so a reader or a collection pins without allocating, and the ledger answers three questions from
 *  them: which stamps a flush or a compaction keeps a version for, the floor stamps below which no reader can
 *  tell apart, and the durable stamp a catalog save may promise.
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

    commit_ledger() noexcept { views_.kind_ = pin_kind_t::view_k; }
    commit_ledger(commit_ledger const&) = delete;
    commit_ledger& operator=(commit_ledger const&) = delete;
    ~commit_ledger() noexcept {
        assert(readers_.next_ == &readers_ && views_.next_ == &views_ && "Pins outlive their ledger");
    }

    /** The newest stamp every commit at or below which has landed, which a fresh read answers at; acquired, so the
     *  view a reader loads after it names every commit at or below it. */
    [[nodiscard]] transaction_stamp_t published() const noexcept {
        return static_cast<transaction_stamp_t>(order_.published_stamp());
    }

    /** The epochs every reader of every collection on this ledger announces, which a view's replacer waits out. */
    [[nodiscard]] epochs_t& epochs() noexcept { return epochs_; }

    /**
     *  @brief Lands @p request with every request pending beside it, combining them on whichever committer takes the
     *      leader word, and answers its stamp once it is published; nothing is held on return.
     *  @note A thread already holding the leader word waits on itself here, as a staged transaction's would.
     */
    expected<transaction_stamp_t> submit(commit_request_t& request) noexcept {
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

    /** The newest stamp any reader pins, or @c unwritten_stamp_k while none does. */
    [[nodiscard]] transaction_stamp_t newest_reader() const noexcept { return load_(newest_reader_); }

    /** The oldest stamp a reader may still read at, so versions at or below it are indistinguishable to every one. */
    [[nodiscard]] transaction_stamp_t low_water_mark() const noexcept {
        return static_cast<transaction_stamp_t>(order_.low_water_mark());
    }

    /** The newest stamp every commit at or below which is wholly in runs, which a catalog naming those runs may
     *  promise: the least over every registered view, or the published stamp while no view holds memory runs. */
    [[nodiscard]] transaction_stamp_t durable_stamp() noexcept { return horizon_besides(views_); }

    /** The stamp below which no reader and no mount can tell two stamps apart, which a run's column counts from. */
    [[nodiscard]] transaction_stamp_t floor() noexcept { return std::min(low_water_mark(), durable_stamp()); }

    /** As @c floor, ignoring the memory runs of the view @p own registers, which a flush is about to write out. */
    [[nodiscard]] transaction_stamp_t floor_besides(pin_t const& own) noexcept {
        return std::min(low_water_mark(), horizon_besides(own));
    }

    /**
     *  @brief As @c durable_stamp, ignoring the memory runs of the view @p own registers.
     *
     *  The published stamp is read first: every commit at or below it published its views and bounded them before it
     *  landed, so a bound read after it covers that commit, and a commit above it is past the answer anyway.
     */
    [[nodiscard]] transaction_stamp_t horizon_besides(pin_t const& own) noexcept {
        transaction_stamp_t horizon = published();
        guard_t const held {claim_};
        for (pin_t const* pin = views_.next_; pin != &views_; pin = pin->next_)
            if (pin != &own) horizon = std::min(horizon, load_(pin->stamp_));
        return horizon;
    }

    /** Every stamp a reader pins, ascending and distinct. */
    status_t pinned_readers(st::basic_vector<transaction_stamp_t>& into) noexcept {
        into.clear();
        {
            guard_t const held {claim_};
            for (pin_t const* pin = readers_.next_; pin != &readers_; pin = pin->next_)
                if (into.push_back(transaction_stamp_t {pin->stamp_}) != st::status_t::success_k)
                    return {status_code_t::out_of_memory_k, "No room to list the stamps readers pin"};
        }
        std::sort(into.begin(), into.end());
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
        commit_in_flight_t in_flight;
        order_.begin_commit(in_flight);
        transaction_stamp_t const stamp = static_cast<transaction_stamp_t>(in_flight.stamp());
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
        if (pin.kind_ == pin_kind_t::view_k) return;
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

#pragma endregion Stamps

#pragma region Value Cache

#pragma region Cache

/**
 *  @brief Values of versions point reads already paid I/O for, keyed by the key and the stamp of the version, so a
 *      flush or a compaction moving a version leaves its entry right and nothing is ever cleared.
 *
 *  A fixed array of slots, a slot per hash of the version, each owning its words from its first fill until the cache
 *  dies and refilled in place. A fill moves the slot's sequence to odd, writes the version, and moves the sequence on
 *  to the next even value; a probe copies the version out between two loads of the sequence and keeps the copy only
 *  when both read the same even value. Neither ever waits, a probe writes nothing, and a displaced version is simply
 *  overwritten, so nothing is freed until the cache is. Every word moves atomically, so a probe racing a fill copies a
 *  torn version it then drops rather than racing on memory. The words are charged to the budget the cache was made
 *  with, which is borrowed and must outlive the cache.
 *
 *  @tparam atomic_reference_ The reference every word of a slot and the budget are owned through.
 */
template <typename stored_key_type_, template <typename> typename atomic_reference_>
class value_cache {
  public:

#pragma region Types

    /** The key the cache addresses its values by. */
    using stored_key_t = stored_key_type_;

#pragma endregion Types

#pragma region Construction

    value_cache() noexcept = default;
    value_cache(value_cache const&) = delete;
    value_cache& operator=(value_cache const&) = delete;
    value_cache(value_cache&& other) noexcept = default;

    value_cache& operator=(value_cache&& other) noexcept {
        if (this == &other) return *this;
        std::swap(slots_, other.slots_);
        std::swap(slot_words_, other.slot_words_);
        return *this;
    }

    /** Frees the words of every slot filled so far, which only a cache no reader reaches may. */
    ~value_cache() noexcept {
        words_allocator_t allocator {slots_.get_allocator()};
        for (std::uint64_t* words : slots_)
            if (words) allocator.deallocate(words, slot_words_);
    }

    /**
     *  @brief A cache holding what @p capacity_bytes of @p constraint afford of values @p value_size wide.
     *  @param[in] capacity_bytes Bytes the cache may take, where zero leaves it holding nothing.
     *  @param[in] value_size Bytes one value takes, which is its widest for a variable-width layout.
     *  @param[inout] constraint The budget the cached bytes are charged to, which must outlive the cache.
     */
    [[nodiscard]] static value_cache of(std::size_t capacity_bytes, stored_value_size_t value_size,
                                        resource_constraint<atomic_reference_>& constraint) noexcept {
        value_cache cache;
        cache.slots_ = slots_t {constrained_allocator<std::uint64_t*, atomic_reference_> {constraint}};
        if (capacity_bytes == 0 || value_size == 0) return cache;
        std::size_t const slot_words = value_word_k + words_of_(value_size);
        std::size_t const affordable = capacity_bytes / (slot_words * sizeof(std::uint64_t) + sizeof(std::uint64_t*));
        if (affordable == 0) return cache;
        // A slot is found by masking the hash, so the budget is rounded down to a power of two of slots.
        if (cache.slots_.resize(std::bit_floor(affordable)) != st::status_t::success_k) return cache;
        for (std::uint64_t*& slot : cache.slots_) slot = nullptr;
        cache.slot_words_ = slot_words;
        return cache;
    }

#pragma endregion Construction

#pragma region Lookups

    /** The entries the cache holds at once, zero when it caches nothing. */
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

    /**
     *  @brief Copies the value cached for the version of @p stored_key stamped @p stamp into @p into, when it fits.
     *  @param[out] size Bytes copied, untouched unless the copy is kept.
     *  @return Whether the slot held that version from the first load of its sequence to the last, so the copy is kept.
     */
    [[nodiscard]] bool copy(stored_key_t const& stored_key, transaction_stamp_t stamp, std::span<byte_t> into,
                            stored_value_size_t& size) const noexcept {
        if (slots_.empty()) return false;
        std::uint64_t const* const words =
            atomic_reference_<std::uint64_t* const> {slots_[slot_of_(stored_key, stamp)]}.load(
                std::memory_order_acquire);
        if (!words) return false;
        std::uint64_t const sequence = load_(words[sequence_word_k], std::memory_order_acquire);
        // Zero is a slot no fill finished yet, and an odd sequence one a fill is writing.
        if (sequence == 0 || sequence % 2 != 0 || load_(words[stamp_word_k]) != stamp) return false;
        stored_key_t cached {};
        load_bytes_(words + key_word_k, std::as_writable_bytes(std::span<stored_key_t, 1> {&cached, 1}));
        std::uint64_t const cached_size = load_(words[size_word_k]);
        if (!(cached == stored_key) || cached_size > into.size()) return false;
        load_bytes_(words + value_word_k, into.first(static_cast<std::size_t>(cached_size)));
        // Orders every load above before the sequence is read again, as a fill's release fence orders its stores.
        std::atomic_thread_fence(std::memory_order_acquire);
        if (load_(words[sequence_word_k]) != sequence) return false;
        size = static_cast<stored_value_size_t>(cached_size);
        return true;
    }

    /** Caches @p stored_value as the version of @p stored_key stamped @p stamp in the slot that version hashes to,
     *  overwriting whatever it held; a fill finding another fill in the slot, or the budget refusing the slot's first
     *  words, caches nothing. */
    void add(stored_key_t const& stored_key, transaction_stamp_t stamp, std::span<byte_t const> stored_value) noexcept {
        if (slots_.empty() || words_of_(stored_value.size()) > slot_words_ - value_word_k) return;
        std::uint64_t* const words = words_of_slot_(slots_[slot_of_(stored_key, stamp)]);
        if (!words) return;
        atomic_reference_<std::uint64_t> const sequence {words[sequence_word_k]};
        std::uint64_t stable = sequence.load(std::memory_order_relaxed);
        if (stable % 2 != 0 ||
            !sequence.compare_exchange_strong(stable, stable + 1, std::memory_order_relaxed, std::memory_order_relaxed))
            return;
        // Orders the odd sequence before every store below, which a probe's acquire fence pairs with.
        std::atomic_thread_fence(std::memory_order_release);
        store_(words[stamp_word_k], stamp);
        store_(words[size_word_k], stored_value.size());
        store_bytes_(words + key_word_k, std::as_bytes(std::span<stored_key_t const, 1> {&stored_key, 1}));
        store_bytes_(words + value_word_k, stored_value);
        sequence.store(stable + 2, std::memory_order_release);
    }

#pragma endregion Lookups

  private:

#pragma region Private Types

    /** One pointer per slot to the words it owns, null until its first fill, charged to the budget. */
    using slots_t = st::basic_vector<std::uint64_t*, constrained_allocator<std::uint64_t*, atomic_reference_>>;

    /** Allocates the words of a slot from the same budget. */
    using words_allocator_t = constrained_allocator<std::uint64_t, atomic_reference_>;

#pragma endregion Private Types

#pragma region Private Methods

    /** Words @p bytes take, rounded up. */
    [[nodiscard]] static constexpr std::size_t words_of_(std::size_t bytes) noexcept {
        return (bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
    }

    [[nodiscard]] std::size_t slot_of_(stored_key_t const& stored_key, transaction_stamp_t stamp) const noexcept {
        return (stored_key_hash_t {}(stored_key) ^ st::hash<transaction_stamp_t> {}(stamp)) & (slots_.size() - 1);
    }

    /** The words @p slot owns, allocated with a zero header by the first fill and kept until the cache dies, or null
     *  when the budget refuses them; two first fills racing keep the words of whichever publishes first. */
    [[nodiscard]] std::uint64_t* words_of_slot_(std::uint64_t*& slot) noexcept {
        atomic_reference_<std::uint64_t*> const published {slot};
        if (std::uint64_t* const words = published.load(std::memory_order_acquire)) return words;
        words_allocator_t allocator {slots_.get_allocator()};
        std::uint64_t* const allocated = allocator.allocate(slot_words_);
        if (!allocated) return nullptr;
        std::memset(allocated, 0, value_word_k * sizeof(std::uint64_t));
        std::uint64_t* first = nullptr;
        if (published.compare_exchange_strong(first, allocated, std::memory_order_acq_rel, std::memory_order_acquire))
            return allocated;
        allocator.deallocate(allocated, slot_words_);
        return first;
    }

    [[nodiscard]] static std::uint64_t load_(std::uint64_t const& word,
                                             std::memory_order order = std::memory_order_relaxed) noexcept {
        return atomic_reference_<std::uint64_t const> {word}.load(order);
    }

    static void store_(std::uint64_t& word, std::uint64_t value) noexcept {
        atomic_reference_<std::uint64_t> {word}.store(value, std::memory_order_relaxed);
    }

    /** Stores @p bytes into the words from @p words on, one atomic word at a time, the last padded with zeros. */
    static void store_bytes_(std::uint64_t* words, std::span<byte_t const> bytes) noexcept {
        for (std::size_t offset = 0; offset < bytes.size(); offset += sizeof(std::uint64_t)) {
            std::uint64_t word = 0;
            std::memcpy(&word, bytes.data() + offset, std::min(sizeof(word), bytes.size() - offset));
            store_(words[offset / sizeof(word)], word);
        }
    }

    /** Fills @p bytes from the words from @p words on, one atomic word at a time. */
    static void load_bytes_(std::uint64_t const* words, std::span<byte_t> bytes) noexcept {
        for (std::size_t offset = 0; offset < bytes.size(); offset += sizeof(std::uint64_t)) {
            std::uint64_t const word = load_(words[offset / sizeof(word)]);
            std::memcpy(bytes.data() + offset, &word, std::min(sizeof(word), bytes.size() - offset));
        }
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The word of a slot holding its sequence: zero before its first fill, odd while a fill writes it. */
    static constexpr std::size_t sequence_word_k = 0;

    /** The word of a slot holding the stamp of the version it caches. */
    static constexpr std::size_t stamp_word_k = 1;

    /** The word of a slot holding how many bytes the value takes. */
    static constexpr std::size_t size_word_k = 2;

    /** The first word of a slot holding the key. */
    static constexpr std::size_t key_word_k = 3;

    /** The first word of a slot holding the value, after the key. */
    static constexpr std::size_t value_word_k = key_word_k + (sizeof(stored_key_t) + sizeof(std::uint64_t) - 1) /
                                                                 sizeof(std::uint64_t);

    /** The slots, each naming the words it owns or null. */
    slots_t slots_;

    /** Words every slot owns: the header, the key and the widest value. */
    std::size_t slot_words_ = 0;

#pragma endregion Private Members
};

#pragma endregion Cache

#pragma endregion Value Cache

#pragma region Store

#pragma region Vocabulary

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

/** One commit's deltas for one collection, sealed off every shared path by its committer, and the run the leader
 *  stamps them into, both charged to a budget whose word @p atomic_reference_ owns; both outlive the request carrying
 *  them only until the commit lands or is refused. */
template <typename stored_key_type_, template <typename> typename atomic_reference_>
struct sealed_commit {

    /** The deltas, sorted and copied, which the leader moves into @c run once it draws the stamp. */
    sealed_deltas<stored_key_type_, constrained_allocator<byte_t, atomic_reference_>> deltas;

    /** The run they become, allocated by the committer, owned by the store once published. */
    std::unique_ptr<memory_run<stored_key_type_, constrained_allocator<byte_t, atomic_reference_>>> run;
};

/** One key a scan hands out: what it carries at the scan's stamp and how wide its value is. */
template <typename stored_key_type_>
struct scan_entry {

    /** The key. */
    stored_key_type_ stored_key {};

    /** Whether the key carries a value or a removal. */
    lookup_result_t state = lookup_result_t::missing_k;

    /** Bytes the value takes, zero for a removal and for an empty value. */
    stored_value_size_t size = 0;
};

/** Keys one page of a scan holds, as many as one page of memory does, which bounds how long a commit waits. */
template <typename stored_key_type_>
constexpr std::size_t scan_page_keys() noexcept {
    return utilities::arena_page_bytes_k / sizeof(scan_entry<stored_key_type_>);
}

template <typename key_slot_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, utilities::fold_concept fold_type_ = utilities::last_write_fold_t>
class nvme_store_cursor;

#pragma endregion Vocabulary

#pragma region Store

/**
 *  @brief The store of one collection: immutable memory runs and a value cache in front of an LSM tree of sorted runs,
 *      behind one published view.
 *
 *  A reader announces itself in its ledger's epochs, loads the published stamp and then the view, and walks immutable
 *  runs with no lock. A committer seals its deltas into one memory run on its own thread, and the ledger's leader
 *  publishes a view naming it ahead of the others. A merge folds F runs of one tier into one run of the next tier,
 *  and a flush writes the memory runs of one view as one run of level zero, each with no lock held, then takes the
 *  leader word only to publish a view without exactly those runs. Whoever replaces a view frees the view and the runs
 *  it no longer names before returning, once every reader that entered before has left. Merges, flushes, compactions
 *  and truncations of one store are serialized by its owner. The volume, the budget and every channel are borrowed
 *  and must outlive the store, and a channel belongs to the thread holding it.
 *
 *  The fold decides what a change delta does: a read collects a key's versions newest first down to the first that
 *  closes the chain and applies them, and every merge composes the changes no reader tells apart. A store of blobs
 *  folds with @c last_write_fold_t, writes no change, and reads and merges exactly as if it had no fold.
 *
 *  @tparam atomic_reference_ The reference the published view, the cache, the budget and the ledger own their words
 *      through.
 *  @tparam waiter_type_ What a core waiting on a read, a barrier or the ledger does between attempts, before it parks.
 */
template <typename key_slot_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, utilities::fold_concept fold_type_ = utilities::last_write_fold_t>
class nvme_store {
  public:

#pragma region Types

    /** The key slot every run of this store holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of this store lives on. */
    using volume_t = volume_type_;

    /** What a change delta of this store does, which reads and merges fold with. */
    using fold_t = fold_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The tree the flushed writes of this collection live in. */
    using tree_t = lsm_tree<key_slot_t, volume_t, atomic_reference_, waiter_type_>;

    /** One level set of that tree, which a view names. */
    using level_set_t = typename tree_t::level_set_t;

    /** The values of versions point reads paid I/O for, which this collection keeps. */
    using value_cache_t = value_cache<stored_key_t, atomic_reference_>;

    /** The stamps this collection's writes carry and its readers pin. */
    using ledger_t = commit_ledger<atomic_reference_, waiter_type_>;

    /** One stamp a reader or a view of this collection holds against the ledger. */
    using pin_t = stamp_pin<atomic_reference_, waiter_type_>;

    /** The budget the memory runs and the cache are charged to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    /** Where the sections of one flushed run sit inside its blob. */
    using sorted_run_layout_t = sorted_run_layout<key_slot_t>;

    /** One delta a commit hands over: the key, its kind, and its bytes. */
    using keyed_delta_t = keyed_delta<stored_key_t>;

    /** One commit's deltas, sorted and copied, before they are stamped. */
    using sealed_deltas_t = sealed_deltas<stored_key_t, constrained_allocator<byte_t, atomic_reference_>>;

    /** One commit's deltas, stamped, which nothing changes once a view names them. */
    using memory_run_t = memory_run<stored_key_t, constrained_allocator<byte_t, atomic_reference_>>;

    /** The memory runs one view names, newest first. */
    using memory_view_t = collection_view<memory_run_t>;

    /** One commit's deltas for this store between their sealing and their landing. */
    using sealed_commit_t = sealed_commit<stored_key_t, atomic_reference_>;

    /** One version a merge or a flush carries over from a memory run. */
    using stamped_delta_t = stamped_delta<stored_key_t>;

    /** How the values of this collection are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so reading one needs no I/O. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

    /** Everything a reader of this collection needs, which nothing changes once it is published. */
    struct view_t {

        /** The memory runs, newest first, every stamp of which is newer than any level's. */
        memory_view_t memory_runs;

        /** The levels below them. */
        level_set_t const* levels = nullptr;

        /** The oldest stamp a reader that pins none may read at, raised by every merge to the horizon it kept. */
        transaction_stamp_t oldest_readable = unwritten_stamp_k;

        /** Its room in the ledger's list of what the leader unpublished, once a publication replaces it. */
        unpublished_t unpublished;
    };

#pragma endregion Types

#pragma region Construction

    nvme_store() noexcept = default;
    nvme_store(nvme_store const&) = delete;
    nvme_store& operator=(nvme_store const&) = delete;
    nvme_store(nvme_store&&) noexcept = default;
    nvme_store& operator=(nvme_store&&) noexcept = default;

    /**
     *  @brief An empty store over @p volume, whose memory is charged to @p constraint.
     *  @param[inout] volume The volume every run lands on, borrowed and outliving the store.
     *  @param[in] config The bounds of the memory runs, the cache bound and the shape of the tree below.
     *  @param[inout] constraint The budget the memory runs and the cache are charged to.
     *  @param[inout] ledger The stamps the store's writes carry and its readers pin, outliving the store.
     */
    static expected<nvme_store> mount(volume_t& volume, collection_config_t const& config, constraint_t& constraint,
                                      ledger_t& ledger) noexcept {
        sorted_run_config_t const& values = config.lsm_tree.lsm_level_0.sorted_run;
        nvme_store mounted;
        mounted.config_ = config;
        mounted.constraint_ = &constraint;
        mounted.ledger_ = &ledger;
        mounted.value_size_ = is_fixed(value_layout_k) ? values.value_size : 0;
        mounted.value_max_size_ = is_inline(value_layout_k)
                                      ? static_cast<stored_value_size_t>(value_layout_k)
                                      : (is_fixed(value_layout_k) ? values.value_size : values.value_max_size);
        if (mounted.value_max_size_ == 0)
            return status_t {status_code_t::invalid_argument_k, "A collection admits values of some width"};
        if (config.memory_runs_max_deltas == 0)
            return status_t {status_code_t::invalid_argument_k, "A collection holds at least one write in memory"};
        mounted.cache_ = value_cache_t::of(holds_values_inline_k ? 0 : config.cache_limit, mounted.value_max_size_,
                                           constraint);

        auto [tree, mount_tree_status] = tree_t::mount(volume, config.lsm_tree, config.memory_runs_max_bytes);
        if (mount_tree_status.failed()) return mount_tree_status;
        mounted.tree_ = std::move(tree);
        mounted.current_.reset(new (std::nothrow) view_t {});
        if (!mounted.current_) return status_t {status_code_t::out_of_memory_k, "A collection could not hold a view"};
        mounted.current_->levels = &mounted.tree_.levels();
        mounted.view_.replace(*mounted.current_);
        ledger.register_view(mounted.view_pin_);
        return mounted;
    }

    /** Whether a collection of @p modality whose values follow @p layout is one this store keeps: a modality its fold
     *  folds, in the value layout of its key slots. */
    [[nodiscard]] static bool serves(ustore_modality_t modality, value_layout_t layout) noexcept {
        return fold_t::serves(modality) && layout == value_layout_k;
    }

    /** Returns every run to the volume and forgets every memory run, which destroys the data and which only a
     *  collection no reader reaches and no commit names may. */
    status_t destroy(io_channel_t& channel) noexcept {
        current_->memory_runs = memory_view_t::of(typename memory_view_t::runs_t {});
        memory_runs_.clear();
        ledger_->bound_view(view_pin_, head_stamp_k);
        return tree_.destroy(channel);
    }

#pragma endregion Construction

#pragma region Shape

    /** The bounds this collection was mounted with. */
    [[nodiscard]] collection_config_t const& config() const noexcept { return config_; }

    /** The tree the flushed writes live in, which a database compacts and names in its catalog under its writer. */
    [[nodiscard]] tree_t& tree() noexcept { return tree_; }

    /** The tree the flushed writes live in. */
    [[nodiscard]] tree_t const& tree() const noexcept { return tree_; }

    /** Bytes the largest value this collection admits takes. */
    [[nodiscard]] stored_value_size_t value_max_size() const noexcept { return value_max_size_; }

    /** Deltas the memory runs hold, which no flush has written yet. */
    [[nodiscard]] std::size_t memory_deltas_count() const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        return memory_deltas_(*view_.load());
    }

    /** How many memory runs the published view names, and how many commits they hold, which merges by tier keep
     *  within (F − 1)(T + 1) runs for T = ⌈log_F commits⌉ once every commit returned, F being the enlarge factor. */
    [[nodiscard]] memory_shape_t memory_shape() const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        memory_shape_t shape;
        for (memory_run_t const* run : view_.load()->memory_runs.runs()) {
            std::uint64_t commits = 1;
            for (std::uint32_t tier = 0; tier != run->tier(); ++tier) commits *= config_.lsm_tree.level_enlarge_factor;
            shape.commits += commits;
            ++shape.runs;
        }
        return shape;
    }

    /** Whether no memory run and no level holds a key. */
    [[nodiscard]] bool empty() const noexcept { return population_count() == 0; }

    /** Deltas the memory runs hold plus key slots every level holds, one key counted once per run holding it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        view_t const& view = *view_.load();
        return memory_deltas_(view) + view.levels->population_count();
    }

    /** Bytes every run of every level takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        return view_.load()->levels->size_bytes();
    }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether anything carries @p stored_key, and how many bytes its value takes.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c written_k.
     *  @param[in] stamp The stamp a reader is pinned at, or @c head_stamp_k for the published one.
     *  @param[in] thread_index The executor thread reading, which announces on its own epoch slot.
     *  @return What the key carries, or @c incomplete_k for a fold's chain reaching a level whose values only a
     *      @c select reads.
     */
    [[nodiscard]] lookup_result_t find(stored_key_t stored_key, stored_value_size_t& size,
                                       transaction_stamp_t stamp = head_stamp_k,
                                       std::size_t thread_index = outside_executor_k) const noexcept {
        size = 0;
        reader_t const inside = ledger_->epochs().enter(thread_index);
        reading_t const read = reading_(stamp);
        delta_view_t found;
        transaction_stamp_t version = unwritten_stamp_k;
        if (read.view->memory_runs.find(stored_key, read.stamp, found, version)) {
            if (found.kind == delta_kind_t::change_k) return find_folded_(read, stored_key, size);
            if (found.kind == delta_kind_t::remove_k) return lookup_result_t::removed_k;
            size = static_cast<stored_value_size_t>(found.bytes.size());
            return lookup_result_t::written_k;
        }
        lookup_result_t const in_levels = find_in_levels_(*read.view->levels, stored_key, read.stamp, size, version);
        if (in_levels == lookup_result_t::changed_k) return find_folded_(read, stored_key, size);
        return selected_(in_levels);
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[inout] channel The channel the read is submitted and completed on, held by the calling thread.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     *  @param[in] stamp The stamp a reader is pinned at, or @c head_stamp_k for the published one.
     *  @param[in] thread_index The executor thread reading, which announces on its own epoch slot.
     *  @return @c not_found_k for a miss or a removal, @c input_output_k for a run shorter than its index claims.
     *
     *  Not @c const: a read that pays I/O files the version it read in the value cache.
     */
    status_t select(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into, stored_value_size_t& size,
                    transaction_stamp_t stamp = head_stamp_k, std::size_t thread_index = outside_executor_k) noexcept {
        size = 0;
        reader_t const inside = ledger_->epochs().enter(thread_index);
        reading_t const read = reading_(stamp);
        delta_view_t found;
        if (read.view->memory_runs.find(stored_key, read.stamp, found)) {
            if (found.kind == delta_kind_t::change_k) return select_folded_(channel, read, stored_key, into, size);
            if (found.kind == delta_kind_t::remove_k) return {status_code_t::not_found_k, "A commit removed that key"};
            if (into.size() < found.bytes.size())
                return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};
            if (!found.bytes.empty()) std::memcpy(into.data(), found.bytes.data(), found.bytes.size());
            size = static_cast<stored_value_size_t>(found.bytes.size());
            return {};
        }

        level_set_t const& levels = *read.view->levels;
        stored_value_size_t wanted = 0;
        transaction_stamp_t version = unwritten_stamp_k;
        lookup_result_t const in_levels = find_in_levels_(levels, stored_key, read.stamp, wanted, version);
        if (in_levels == lookup_result_t::changed_k) return select_folded_(channel, read, stored_key, into, size);
        if (in_levels != lookup_result_t::written_k) return {status_code_t::not_found_k, "No level carries that key"};
        if (into.size() < wanted) return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};

        if constexpr (!holds_values_inline_k)
            if (cache_.copy(stored_key, version, into, size)) return {};

        std::uint64_t delivered = 0;
        auto sink = [&](std::size_t, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            if (!bytes.empty()) std::memcpy(into.data() + offset, bytes.data(), bytes.size());
            delivered += bytes.size();
        };
        std::array<stored_key_t, 1> const keys {stored_key};
        std::array<lookup_result_t, 1> answers {lookup_result_t::missing_k};
        if (status_t const read_status = tree_.read_values(channel, levels, std::span<stored_key_t const> {keys},
                                                           std::span<lookup_result_t> {answers}, sink, read.stamp);
            read_status.failed())
            return read_status;
        // One key has no rest of a batch to keep, so a block short of its last one fails the whole call.
        if (delivered != wanted) return {status_code_t::io_k, "A run is shorter than its index claims"};
        size = wanted;

        if constexpr (!holds_values_inline_k) cache_.add(stored_key, version, into.first(size));
        return {};
    }

    /**
     *  @brief Reads the values of @p stored_keys, refilling the channel as completions retire.
     *  @param[inout] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from a removal.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *  @param[in] stamp The stamp a reader is pinned at, or @c head_stamp_k for the published one.
     *  @param[in] thread_index The executor thread reading, which announces on its own epoch slot.
     *  @return Success, or the failure that stopped the batch rather than one of its keys.
     *
     *  @note A value larger than one block arrives as several calls, each carrying the bytes of the
     *      value before it, and a key whose value is present and empty is reported once with no bytes.
     *  @note The bytes a batched read moves are never cached, since caching one would mean staging the
     *      whole value in a buffer this path exists to do without.
     */
    template <typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<lookup_result_t> results, sink_type_&& sink, transaction_stamp_t stamp = head_stamp_k,
                    std::size_t thread_index = outside_executor_k) const noexcept {
        if (results.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched select answers one result per key"};
        reader_t const inside = ledger_->epochs().enter(thread_index);
        reading_t const read = reading_(stamp);
        level_set_t const& levels = *read.view->levels;

        // The tree takes its keys contiguous, so the request each deferred key came from travels beside it.
        deferred_keys_t deferred_keys {constrained_allocator<stored_key_t, atomic_reference_> {*constraint_}};
        deferred_requests_t deferred_requests {constrained_allocator<std::size_t, atomic_reference_> {*constraint_}};
        if (deferred_keys.reserve(stored_keys.size()) != st::status_t::success_k ||
            deferred_requests.reserve(stored_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A collection could not track the keys of a batch"};

        // A cached value is copied out of its slot before the sink sees it, into one buffer the whole batch reuses.
        constrained_buffer<atomic_reference_> cached_value {
            constrained_allocator<byte_t, atomic_reference_> {*constraint_}};
        // A key whose newest version is a fold's change is folded on its own once the batch's reads have retired.
        deferred_requests_t folded_requests {constrained_allocator<std::size_t, atomic_reference_> {*constraint_}};
        for (std::size_t request = 0; request != stored_keys.size(); ++request) {
            delta_view_t found;
            if (read.view->memory_runs.find(stored_keys[request], read.stamp, found)) {
                if (found.kind == delta_kind_t::change_k) {
                    results[request] = lookup_result_t::missing_k;
                    if (folded_requests.push_back(std::size_t {request}) != st::status_t::success_k)
                        return {status_code_t::out_of_memory_k, "A collection could not track the keys of a batch"};
                }
                else if (found.kind == delta_kind_t::remove_k) results[request] = lookup_result_t::removed_k;
                else {
                    results[request] = lookup_result_t::written_k;
                    sink(request, std::uint64_t {0}, found.bytes);
                }
                continue;
            }
            if constexpr (!holds_values_inline_k)
                if (cache_.capacity() != 0) {
                    stored_value_size_t wanted = 0;
                    stored_value_size_t copied = 0;
                    transaction_stamp_t version = unwritten_stamp_k;
                    bool const cached = find_in_levels_(levels, stored_keys[request], read.stamp, wanted, version) ==
                                            lookup_result_t::written_k &&
                                        cached_value.resize(wanted) == st::status_t::success_k &&
                                        cache_.copy(stored_keys[request], version, std::span<byte_t> {cached_value},
                                                    copied);
                    if (cached) {
                        results[request] = lookup_result_t::written_k;
                        sink(request, std::uint64_t {0}, std::span<byte_t const> {cached_value}.first(copied));
                        continue;
                    }
                }
            results[request] = lookup_result_t::missing_k;
            [[maybe_unused]] st::status_t const named = deferred_keys.push_back(stored_key_t {stored_keys[request]});
            [[maybe_unused]] st::status_t const tracked = deferred_requests.push_back(std::size_t {request});
        }
        if (deferred_keys.empty())
            return select_folded_(channel, read, stored_keys, std::span<std::size_t const> {folded_requests}, results,
                                  sink);

        // The tree answers in its own vocabulary and the sink is told nothing about sizes, so the bytes every key
        // owed are counted here and compared once the read has run.
        deferred_answers_t answers {constrained_allocator<lookup_result_t, atomic_reference_> {*constraint_}};
        deferred_delivered_t delivered {constrained_allocator<std::uint64_t, atomic_reference_> {*constraint_}};
        if (answers.resize(deferred_keys.size()) != st::status_t::success_k ||
            delivered.resize(deferred_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A collection could not track the keys of a batch"};
        for (std::size_t index = 0; index != deferred_keys.size(); ++index) {
            answers[index] = lookup_result_t::missing_k;
            delivered[index] = 0;
        }

        auto relay = [&](std::size_t index, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            delivered[index] += bytes.size();
            sink(deferred_requests[index], offset, bytes);
        };
        status_t const read_status = tree_.read_values(channel, levels, std::span<stored_key_t const> {deferred_keys},
                                                       std::span<lookup_result_t> {answers}, relay, read.stamp);

        std::size_t truncated = 0;
        for (std::size_t index = 0; index != deferred_keys.size(); ++index) {
            lookup_result_t& result = results[deferred_requests[index]];
            if (answers[index] == lookup_result_t::missing_k) continue;
            if (answers[index] == lookup_result_t::removed_k) {
                result = lookup_result_t::removed_k;
                continue;
            }
            if (answers[index] == lookup_result_t::changed_k) {
                if (folded_requests.push_back(std::size_t {deferred_requests[index]}) != st::status_t::success_k)
                    return {status_code_t::out_of_memory_k, "A collection could not track the keys of a batch"};
                continue;
            }
            stored_value_size_t wanted = 0;
            transaction_stamp_t version = unwritten_stamp_k;
            [[maybe_unused]] lookup_result_t const carried = find_in_levels_(levels, deferred_keys[index], read.stamp,
                                                                             wanted, version);
            if (delivered[index] == wanted) result = lookup_result_t::written_k;
            else {
                result = lookup_result_t::incomplete_k;
                ++truncated;
            }
        }

        // A block short of what it asked for fails its own key; a cancelled or refused one fails the batch.
        if (read_status.failed() && (truncated == 0 || !(read_status == status_code_t::io_k))) return read_status;
        return select_folded_(channel, read, stored_keys, std::span<std::size_t const> {folded_requests}, results,
                              sink);
    }

#pragma endregion Lookups

#pragma region Writes

    /** Whether a value of @p size bytes is one this collection's layout can hold, which @c seal checks. */
    [[nodiscard]] bool admits(std::size_t size) const noexcept { return admits_(size); }

    /**
     *  @brief Sorts and copies @p deltas into one commit's sealed deltas for this store, on the committer's thread
     *      and off every shared path, and allocates the run they become; no deltas seal into no run.
     *  @param[in] deltas The commit's deltas for this store, one per key, which this sorts in place: writes of an
     *      admitted width, removals, and a fold's changes, which only a run file's length bounds unless they sit
     *      inline in a key slot.
     */
    expected<sealed_commit_t> seal(std::span<keyed_delta_t> deltas) const noexcept {
        for (keyed_delta_t const& each : deltas)
            if (!admits_(each.delta))
                return status_t {status_code_t::malformed_k, "This collection admits no delta of that size"};
        sealed_commit_t sealed;
        if (deltas.empty()) return sealed;
        auto [sealed_deltas, seal_status] = sealed_deltas_t::seal(
            deltas, constrained_allocator<byte_t, atomic_reference_> {*constraint_});
        if (seal_status.failed()) return seal_status;
        sealed.deltas = std::move(sealed_deltas);
        sealed.run.reset(new (std::nothrow) memory_run_t {});
        if (!sealed.run) return status_t {status_code_t::out_of_memory_k, "No room for another memory run"};
        return sealed;
    }

    /** How full the memory runs of the published view would be with @p sealed beside them. */
    [[nodiscard]] memory_pressure_t memory_pressure(sealed_commit_t const& sealed) const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        view_t const& view = *view_.load();
        if (view.memory_runs.runs().empty()) return memory_pressure_t::fits_k;
        std::uint64_t const deltas = memory_deltas_(view) + sealed.deltas.size();
        std::uint64_t const bytes = memory_bytes_(view) + sealed.deltas.values_bytes();
        std::uint64_t const deltas_bound = config_.memory_runs_max_deltas;
        std::uint64_t const bytes_bound = config_.memory_runs_max_bytes;
        // One flush in flight and one buffer filling behind it.
        if (deltas > 2 * deltas_bound || bytes > 2 * bytes_bound) return memory_pressure_t::stalled_k;
        if (deltas > deltas_bound || bytes > bytes_bound) return memory_pressure_t::flush_due_k;
        return memory_pressure_t::fits_k;
    }

    /**
     *  @brief Lays out the view naming @p sealed's run ahead of the current ones, so @c publish cannot fail.
     *  @note Called by the thread holding the ledger's leader word, which then publishes or forgets it before it lets
     *      the word go.
     */
    status_t prepare(sealed_commit_t& sealed) noexcept {
        forget_prepared();
        auto [view, view_status] = next_view_(sealed.run.get(), current_->memory_runs.runs(),
                                              current_->oldest_readable);
        if (view_status.failed() || memory_runs_.reserve(memory_runs_.size() + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to prepare a commit"};
        view->levels = current_->levels;
        prepared_view_ = std::move(view);
        return {};
    }

    /** Stamps @p sealed's run with @p stamp, publishes the view @c prepare laid out, and takes the run over, which the
     *  leader does before it publishes @p stamp itself, so a reader at the stamp finds the run. */
    void publish(sealed_commit_t& sealed, transaction_stamp_t stamp) noexcept {
        assert(prepared_view_ && "A commit publishes only what its leader prepared");
        expected<memory_run_t> stamped = memory_run_t::stamped(
            std::move(sealed.deltas), stamp, constrained_allocator<byte_t, atomic_reference_> {*constraint_});
        assert(stamped.has_value() && "A drawn stamp packs into no words");
        if (stamped) *sealed.run = std::move(*stamped);
        // The durable horizon stays below the oldest commit that only memory holds.
        if (current_->memory_runs.runs().empty()) ledger_->bound_view(view_pin_, stamp - 1);
        replace_view_(std::move(prepared_view_));
        [[maybe_unused]] st::status_t const owned = memory_runs_.push_back(st::assume_reserved, std::move(sealed.run));
    }

    /** Drops the view @c prepare laid out, which the leader does for a commit it then refuses. */
    void forget_prepared() noexcept { prepared_view_.reset(); }

    /** Lands @p sealed as a commit of its own through the ledger's sequencer, and returns once it is published; a
     *  commit sealing no run draws no stamp. */
    status_t commit(sealed_commit_t& sealed) noexcept {
        if (!sealed.run) return {};
        alone_t alone {this, &sealed};
        commit_request_t request;
        request.context = &alone;
        request.prepare = &prepare_alone_;
        request.publish = &publish_alone_;
        return ledger_->submit(request).status();
    }

    /**
     *  @brief Whether a commit newer than @p from wrote @p stored_key, which only the newest write of a key decides.
     *
     *  Answered from the memory runs first, then from the runs newest first, skipping every run whose newest stamp
     *  is not past @p from, since no version it holds can be.
     */
    [[nodiscard]] bool written_since(stored_key_t stored_key, transaction_stamp_t from) const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        view_t const& view = *view_.load();
        for (memory_run_t const* run : view.memory_runs.runs()) {
            if (run->newest_stamp() <= from) return false;
            std::size_t const position = run->lower_bound(stored_key);
            if (position != run->size() && run->stored_key(position) == stored_key) return run->stamp(position) > from;
        }
        level_set_t const& taken = *view.levels;
        for (std::size_t number = 0; number != taken.size(); ++number) {
            auto const& level = taken.level(number);
            if (level.order() == lsm_level_order_t::sorted_k) {
                lsm_level_position_t position;
                if (level.find(stored_key, position) == lookup_result_t::missing_k) continue;
                auto const& run = level.run(position.run_index);
                return run.stamp(position.key_slot_position) > from;
            }
            for (std::size_t index = level.size(); index-- != 0;) {
                auto const& run = level.run(index);
                if (run.newest_stamp() <= from) continue;
                if (stored_key < run.lower_stored_key() || run.upper_stored_key() < stored_key) continue;
                std::uint64_t const position = run.position_of(stored_key);
                if (position != run.population_count()) return run.stamp(position) > from;
            }
        }
        return false;
    }

    /**
     *  @brief Whether a commit newer than @p from wrote any key from @p lower to @p upper inclusive, which is what a
     *      walk over the range watches, so a key inserted or removed where it found none conflicts too.
     *
     *  Every version counts, not only the newest, since a key written twice since @p from was written since. A run
     *  is skipped by its newest stamp.
     */
    [[nodiscard]] bool range_written_since(stored_key_t lower, stored_key_t upper,
                                           transaction_stamp_t from) const noexcept {
        if (upper < lower) return false;
        reader_t const inside = ledger_->epochs().enter();
        view_t const& view = *view_.load();
        if (view.memory_runs.range_written_since(lower, upper, from)) return true;
        level_set_t const& taken = *view.levels;
        for (std::size_t number = 0; number != taken.size(); ++number) {
            auto const& level = taken.level(number);
            for (std::size_t index = 0; index != level.size(); ++index) {
                auto const& run = level.run(index);
                if (run.newest_stamp() <= from) continue;
                if (upper < run.lower_stored_key() || run.upper_stored_key() < lower) continue;
                for (std::uint64_t position = run.lower_bound(lower);
                     position != run.population_count() && !(upper < run.key_slot(position).stored_key); ++position)
                    if (run.stamp(position) > from) return true;
            }
        }
        return false;
    }

    /** Opens the runs @p runs names as this collection's tree, which a mount does before anything reads. */
    status_t restore(io_channel_t& channel, std::span<lsm_tree_run_record_t const> runs) noexcept {
        publication_t publication;
        publication.owner = this;
        return tree_.restore(channel, runs, publication.hook());
    }

    /** Writes @p stored_value under @p stored_key as a commit of its own, flushing first when the memory runs are past
     *  their bounds, which the caller serializes with every other flush. */
    status_t insert(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        keyed_delta_t delta {stored_key, delta_view_t {delta_kind_t::write_k, stored_value}};
        return commit_alone_(channel, seal(std::span<keyed_delta_t> {&delta, 1}));
    }

    /** Removes @p stored_key as a commit of its own, flushing first as @c insert does. */
    status_t remove(io_channel_t& channel, stored_key_t stored_key) noexcept {
        keyed_delta_t delta {stored_key, delta_view_t {delta_kind_t::remove_k, {}}};
        return commit_alone_(channel, seal(std::span<keyed_delta_t> {&delta, 1}));
    }

    /**
     *  @brief Writes one value per key as one commit of its own, flushing first as @c insert does.
     *  @param[inout] channel The channel a flush writes on, held by the writer.
     *  @param[in] stored_keys The keys to write, in any order and with repeats taking the last value.
     *  @param[in] stored_values The values back to back, in the order their keys name them.
     *  @param[in] sizes Bytes of every value, one per key, which must add up to @p stored_values.
     */
    status_t insert(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<byte_t const> stored_values, std::span<stored_value_size_t const> sizes) noexcept {
        return commit_alone_(channel, seal_writes(stored_keys, stored_values, sizes));
    }

    /** Removes every key of @p stored_keys as one commit of its own, flushing first as @c insert does. */
    status_t remove(io_channel_t& channel, std::span<stored_key_t const> stored_keys) noexcept {
        return commit_alone_(channel, seal_removals(stored_keys));
    }

    /** Seals one value per key of @p stored_keys, a repeated key taking its last value, as @c insert takes them. */
    expected<sealed_commit_t> seal_writes(std::span<stored_key_t const> stored_keys,
                                          std::span<byte_t const> stored_values,
                                          std::span<stored_value_size_t const> sizes) const noexcept {
        if (status_t const checked = check_batch(stored_keys, stored_values, sizes); checked.failed()) return checked;
        auto [deltas, deltas_status] = deltas_of_(stored_keys.size());
        if (deltas_status.failed()) return deltas_status;
        std::size_t offset = 0;
        for (std::size_t index = 0; index != stored_keys.size(); ++index) {
            deltas[index] = keyed_delta_t {
                stored_keys[index], delta_view_t {delta_kind_t::write_k, stored_values.subspan(offset, sizes[index])}};
            offset += sizes[index];
        }
        return seal(last_of_each_key_(deltas));
    }

    /** Seals a removal of every key of @p stored_keys, as @c remove takes them. */
    expected<sealed_commit_t> seal_removals(std::span<stored_key_t const> stored_keys) const noexcept {
        auto [deltas, deltas_status] = deltas_of_(stored_keys.size());
        if (deltas_status.failed()) return deltas_status;
        for (std::size_t index = 0; index != stored_keys.size(); ++index)
            deltas[index] = keyed_delta_t {stored_keys[index], delta_view_t {delta_kind_t::remove_k, {}}};
        return seal(last_of_each_key_(deltas));
    }

    /** Whether @p sizes names one admitted width per key of @p stored_keys and adds up to @p stored_values. */
    status_t check_batch(std::span<stored_key_t const> stored_keys, std::span<byte_t const> stored_values,
                         std::span<stored_value_size_t const> sizes) const noexcept {
        if (sizes.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched insert names the size of every value"};
        std::uint64_t total = 0;
        for (std::size_t index = 0; index != sizes.size(); ++index) {
            if (!admits_(sizes[index]))
                return {status_code_t::malformed_k, "This collection admits no value of that size"};
            total += sizes[index];
        }
        if (total != stored_values.size())
            return {status_code_t::invalid_argument_k, "The named sizes do not add up to the values"};
        return {};
    }

#pragma endregion Writes

#pragma region Maintenance

    /**
     *  @brief Merges F memory runs of one tier into one run of the next tier, F being the level enlarge factor, until
     *      no tier holds F runs, the lowest tier and its oldest runs first.
     *
     *  Every merge reads immutable runs with no lock held, keeps the versions a pinned reader needs and every version
     *  above the stamp published when it began, and takes the leader word only to publish a view naming the merged run
     *  in place of exactly those runs; letting the word go, it waits for the readers that could hold them, then frees
     *  them before the next merge.
     *  @note The owner serializes it with flushes, compactions and truncations of this collection, and the calling
     *      thread is inside no epoch.
     */
    status_t merge_memory_runs() noexcept {
        std::size_t const fan_in = config_.lsm_tree.level_enlarge_factor;
        st::basic_vector<memory_run_t const*> group;
        if (group.resize(fan_in) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A merge could not list its memory runs"};
        for (;;) {
            {
                reader_t const inside = ledger_->epochs().enter();
                std::span<memory_run_t const* const> const due = due_group_(view_.load()->memory_runs.runs());
                if (due.empty()) return {};
                // Only this collection's owner frees memory runs, so these outlive the reader.
                for (std::size_t index = 0; index != fan_in; ++index) group[index] = due[index];
            }

            st::basic_vector<transaction_stamp_t> readers;
            if (status_t const listed = ledger_->pinned_readers(readers); listed.failed()) return listed;
            compaction_stamps_t survival;
            survival.pinned = readers.data();
            survival.pinned_count = readers.size();
            // Nothing a memory run holds is durable, so no mount needs a version; a head reader needs those above this.
            survival.horizon = ledger_->published();
            stamped_deltas_t versions {constrained_allocator<stamped_delta_t, atomic_reference_> {*constraint_}};
            // The changes a merge folds sit here until the merged run copies them.
            utilities::local_arena_t folded;
            utilities::arena_allocator<byte_t> folded_bytes = folded.allocator();
            if (status_t const kept = kept_versions(std::span<memory_run_t const* const> {group}, survival, fold_,
                                                    versions, folded_bytes);
                kept.failed())
                return kept;
            auto [merged, merged_status] = memory_run_t::merged(
                std::span<stamped_delta_t const> {versions}, group.back()->oldest_stamp(), group.back()->tier(),
                constrained_allocator<byte_t, atomic_reference_> {*constraint_});
            if (merged_status.failed()) return merged_status;
            std::unique_ptr<memory_run_t> merged_run {new (std::nothrow) memory_run_t {std::move(merged)}};
            // The replaced runs, freed here once the publication waited out every reader that could hold one.
            st::basic_vector<std::unique_ptr<memory_run_t>> away;
            if (!merged_run || away.reserve(fan_in) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "No room to merge memory runs"};
            if (status_t const published = publish_merged_(std::span<memory_run_t const* const> {group},
                                                           std::move(merged_run), away, survival.horizon);
                published.failed())
                return published;
        }
    }

    /** Whether the memory runs hold the bounds they were mounted with, so a flush is due. */
    [[nodiscard]] bool flush_due() const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        return flush_due_(*view_.load());
    }

    /** Whether the memory runs are due for a merge, holding F runs of tier zero, or for a flush; the commit that
     *  made them so runs either before it returns. */
    [[nodiscard]] bool maintenance_due() const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        return maintenance_due_(*view_.load());
    }

    /**
     *  @brief Writes the memory runs of the published view as one run of level zero with no lock held, then publishes
     *      a view naming that run instead of exactly those runs, keeping every memory run committed meanwhile.
     *  @note The owner serializes flushes, compactions and truncations of one collection, and the calling thread is
     *      inside no epoch, since the publication waits for every reader that could hold a run it frees.
     */
    status_t flush(io_channel_t& channel) noexcept {
        publication_t publication;
        publication.owner = this;
        {
            reader_t const inside = ledger_->epochs().enter();
            view_t const& taken = *view_.load();
            std::span<memory_run_t const* const> const runs = taken.memory_runs.runs();
            if (runs.empty()) return {};
            // Only this collection's owner frees memory runs, merging or flushing one at a time, so these outlive the
            // reader.
            if (publication.flushed.resize(runs.size()) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A flush could not list its memory runs"};
            for (std::size_t index = 0; index != runs.size(); ++index) publication.flushed[index] = runs[index];
        }

        // A replaced version survives as a merge would keep it: for a pinned reader, or for a mount promising the
        // durable horizon this flush leaves; at or below both, every stamp reads alike, so the run packs only above.
        st::basic_vector<transaction_stamp_t> readers;
        if (status_t const listed = ledger_->pinned_readers(readers); listed.failed()) return listed;
        compaction_stamps_t survival;
        survival.pinned = readers.data();
        survival.pinned_count = readers.size();
        survival.horizon = ledger_->horizon_besides(view_pin_);
        survival.floor = ledger_->floor_besides(view_pin_);
        publication.oldest_readable = survival.horizon;

        drained_slots_t key_slots {constrained_allocator<key_slot_t, atomic_reference_> {*constraint_}};
        drained_stamps_t stamps {constrained_allocator<std::uint64_t, atomic_reference_> {*constraint_}};
        drained_kinds_t kinds {constrained_allocator<std::uint8_t, atomic_reference_> {*constraint_}};
        drained_values_t stored_values {constrained_allocator<byte_t, atomic_reference_> {*constraint_}};
        utilities::local_arena_t folded;
        utilities::arena_allocator<byte_t> folded_bytes = folded.allocator();
        if (status_t const drained = drain_(std::span<memory_run_t const* const> {publication.flushed}, survival,
                                            folded_bytes, key_slots, stamps, kinds, stored_values);
            drained.failed())
            return drained;
        return tree_.flush(channel, std::span<key_slot_t const> {key_slots}, std::span<std::uint64_t const> {stamps},
                           std::span<std::uint8_t const> {kinds}, survival.floor,
                           std::span<byte_t const> {stored_values}, value_max_size_, publication.hook());
    }

    /** Whether any level of the tree is over the bytes its number admits. */
    [[nodiscard]] bool needs_compaction() const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        return tree_.needs_compaction(*view_.load()->levels);
    }

    /**
     *  @brief Merges the shallowest level that is over its bounds into the level below it.
     *  @param[inout] reading_channel The channel the merge gathers source values on, held by the calling thread.
     *  @param[inout] writing_channel The channel the merge writes its new runs on, which is not @p reading_channel.
     *  @param[in] devices The devices the executor named, the first of which a large merge runs on, empty for the host.
     *  @param[in] published What to call once the merged set is live and before a consumed run is dropped.
     */
    expected<compaction_outcome_t> compact(
        io_channel_t& reading_channel, io_channel_t& writing_channel, std::span<int const> devices,
        lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {}) noexcept {
        // A pinned reader keeps the version it reads, and the durable horizon keeps the version a mount falls back to.
        st::basic_vector<transaction_stamp_t> readers;
        if (status_t const listed = ledger_->pinned_readers(readers); listed.failed()) return listed;
        compaction_stamps_t stamps;
        stamps.pinned = readers.data();
        stamps.pinned_count = readers.size();
        stamps.horizon = ledger_->durable_stamp();
        stamps.floor = ledger_->floor();
        publication_t publication;
        publication.owner = this;
        publication.oldest_readable = stamps.horizon;
        publication.chained = &published;
        return tree_.compact(reading_channel, writing_channel, compaction_placement_t::automatic_k, devices, nullptr,
                             fold_, publication.hook(), stamps);
    }

    /** Drops every version a mount found stamped above @p ceiling, which only a collection holding no memory run
     *  takes. */
    status_t truncate_above(io_channel_t& reading_channel, io_channel_t& writing_channel, transaction_stamp_t ceiling,
                            lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {}) noexcept {
        {
            reader_t const inside = ledger_->epochs().enter();
            view_t const& taken = *view_.load();
            if (!taken.memory_runs.runs().empty())
                return {status_code_t::invalid_argument_k, "A collection truncates only its runs"};
            if (taken.levels->newest_stamp() <= ceiling) return {};
        }
        publication_t publication;
        publication.owner = this;
        publication.chained = &published;
        return tree_.truncate_above(reading_channel, writing_channel, ceiling, fold_, publication.hook());
    }

#pragma endregion Maintenance

  private:

    friend class nvme_store_cursor<key_slot_t, volume_t, atomic_reference_, waiter_type_, fold_t>;

#pragma region Private Types

    /** One reader inside the ledger's epochs, announced until it dies. */
    using reader_t = typename ledger_t::epochs_t::reader_t;

    /** One commit of this store alone, as the sequencer hands it to its two steps. */
    struct alone_t {

        /** The store committed to. */
        nvme_store* owner = nullptr;

        /** What its committer sealed. */
        sealed_commit_t* sealed = nullptr;
    };

    /** The view a read walks and the stamp it reads at. */
    struct reading_t {

        /** The view loaded after the stamp, which names every commit at or below it. */
        view_t const* view = nullptr;

        /** The stamp the read answers at. */
        transaction_stamp_t stamp = head_stamp_k;
    };

    /** What a publication of a new level set hands its hooks: the memory runs it wrote out, the view naming the set,
     *  and the hook it calls once no reader is left in the views before it. */
    struct publication_t {

        /** The store publishing. */
        nvme_store* owner = nullptr;

        /** The memory runs a flush wrote out, newest first, as the view it read named them; none for a merge. */
        st::basic_vector<memory_run_t const*> flushed;

        /** The horizon the merge kept, which the published view raises its oldest readable stamp to. */
        transaction_stamp_t oldest_readable = unwritten_stamp_k;

        /** The view laid out under the leader word just before the set becomes current. */
        std::unique_ptr<view_t> next;

        /** The flushed runs, taken over under the leader word and freed once no reader can hold one. */
        st::basic_vector<std::unique_ptr<memory_run_t>> freed;

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

    /** The key slots one flush writes, in ascending order of their keys. */
    using drained_slots_t = st::basic_vector<key_slot_t, constrained_allocator<key_slot_t, atomic_reference_>>;

    /** The values one flush writes, back to back in the order the key slots locate them. */
    using drained_values_t = st::basic_vector<byte_t, constrained_allocator<byte_t, atomic_reference_>>;

    /** The stamps of the key slots one flush writes, in the same order. */
    using drained_stamps_t = st::basic_vector<std::uint64_t, constrained_allocator<std::uint64_t, atomic_reference_>>;

    /** The @c delta_kind_t of the key slots one flush writes, in the same order. */
    using drained_kinds_t = st::basic_vector<std::uint8_t, constrained_allocator<std::uint8_t, atomic_reference_>>;

    /** The versions one merge or one flush carries over, before and after they are sorted into a run's order. */
    using stamped_deltas_t =
        st::basic_vector<stamped_delta_t, constrained_allocator<stamped_delta_t, atomic_reference_>>;

    /** The deltas one commit of its own hands over. */
    using deltas_t = st::basic_vector<keyed_delta_t, constrained_allocator<keyed_delta_t, atomic_reference_>>;

    /** The keys of a batch that neither a memory run nor the cache answered. */
    using deferred_keys_t = st::basic_vector<stored_key_t, constrained_allocator<stored_key_t, atomic_reference_>>;

    /** The caller's request every deferred key came from. */
    using deferred_requests_t = st::basic_vector<std::size_t, constrained_allocator<std::size_t, atomic_reference_>>;

    /** What the tree answered for every deferred key. */
    using deferred_answers_t =
        st::basic_vector<lookup_result_t, constrained_allocator<lookup_result_t, atomic_reference_>>;

    /** Bytes the tree delivered for every deferred key. */
    using deferred_delivered_t =
        st::basic_vector<std::uint64_t, constrained_allocator<std::uint64_t, atomic_reference_>>;

#pragma endregion Private Types

#pragma region Private Methods

    /** Whether a written value of @p size bytes is one this collection's layout can hold. */
    [[nodiscard]] bool admits_(std::size_t size) const noexcept {
        if constexpr (is_fixed(value_layout_k)) return size == value_size_;
        else return size <= value_max_size_;
    }

    /** Whether this collection's layout can hold @p delta: a write of an admitted width, or a removal or a change
     *  that fits the key slot where values sit inline, the written-value bounds binding writes alone. */
    [[nodiscard]] bool admits_(delta_view_t const& delta) const noexcept {
        if (delta.kind == delta_kind_t::write_k) return admits_(delta.bytes.size());
        if constexpr (holds_values_inline_k) return delta.bytes.size() <= value_max_size_;
        else return true;
    }

    /** Deltas every memory run of @p view holds. */
    [[nodiscard]] static std::size_t memory_deltas_(view_t const& view) noexcept {
        std::size_t deltas = 0;
        for (memory_run_t const* run : view.memory_runs.runs()) deltas += run->size();
        return deltas;
    }

    /** Value bytes every memory run of @p view holds. */
    [[nodiscard]] static std::uint64_t memory_bytes_(view_t const& view) noexcept {
        std::uint64_t bytes = 0;
        for (memory_run_t const* run : view.memory_runs.runs()) bytes += run->values_bytes();
        return bytes;
    }

    /**
     *  @brief The view a read at @p stamp walks, and the stamp it answers at.
     *
     *  A head read takes the published stamp before the view, so the view names every commit at or below it. A merge
     *  published after that stamp was taken may have dropped the version at it, which the view's oldest readable
     *  stamp tells, so the read takes a fresh stamp, at least that horizon, since the merge read it from the
     *  published stamp before publishing. A pinned reader keeps its stamp, which every merge keeps the versions of.
     */
    [[nodiscard]] reading_t reading_(transaction_stamp_t stamp) const noexcept {
        for (;;) {
            transaction_stamp_t const at = stamp == head_stamp_k ? ledger_->published() : stamp;
            view_t const* const view = view_.load();
            if (stamp != head_stamp_k || at >= view->oldest_readable) return reading_t {view, at};
        }
    }

    /** What @p levels answer for @p stored_key at @p stamp, the bytes its value takes, and the stamp of @p version;
     *  a change answers its own bytes, which the versions below it complete. */
    [[nodiscard]] lookup_result_t find_in_levels_(level_set_t const& levels, stored_key_t stored_key,
                                                  transaction_stamp_t stamp, stored_value_size_t& size,
                                                  transaction_stamp_t& version) const noexcept {
        size = 0;
        lsm_tree_position_t position;
        lookup_result_t const found = tree_.find(levels, stored_key, position, stamp);
        if (found == lookup_result_t::missing_k || found == lookup_result_t::removed_k) return found;
        auto const& level = levels.level(position.level_number);
        size = static_cast<stored_value_size_t>(level.value_window(position.level_position).bytes);
        version = level.run(position.level_position.run_index).stamp(position.level_position.key_slot_position);
        return found;
    }

    /** What a lookup of the levels answering @p found tells a caller, a change aside. */
    [[nodiscard]] static lookup_result_t selected_(lookup_result_t found) noexcept {
        if (found == lookup_result_t::missing_k) return lookup_result_t::missing_k;
        if (found == lookup_result_t::removed_k) return lookup_result_t::removed_k;
        return lookup_result_t::written_k;
    }

    /**
     *  @brief Folds the versions of @p stored_key at or below @p read's stamp, collected newest first down to the first
     *      that closes the chain, into the value a reader at that stamp sees.
     *  @param[in] bytes_of Called as @c bytes_of(run, slot, bytes) for a version a level holds, viewing its bytes in
     *      the key slot or in @p arena, answering a status.
     *  @param[out] value The folded value, viewing a memory run, a key slot, or @p arena.
     *  @return What the key carries, or the status a read or the fold refused it with.
     */
    template <typename bytes_of_type_>
    expected<lookup_result_t> fold_versions_(reading_t const& read, stored_key_t stored_key, bytes_of_type_&& bytes_of,
                                             utilities::arena_allocator<byte_t>& arena,
                                             std::span<byte_t const>& value) const noexcept {
        value = {};
        st::basic_vector<delta_view_t, utilities::arena_allocator<delta_view_t>> chain {
            utilities::arena_allocator<delta_view_t> {arena}};
        delta_view_t closing {delta_kind_t::remove_k, {}};
        bool closed = false;
        for (transaction_stamp_t at = read.stamp;;) {
            delta_view_t version;
            transaction_stamp_t stamp = unwritten_stamp_k;
            if (!read.view->memory_runs.find(stored_key, at, version, stamp)) {
                lsm_tree_position_t position;
                lookup_result_t const found = tree_.find(*read.view->levels, stored_key, position, at);
                if (found == lookup_result_t::missing_k) break;
                auto const& run =
                    read.view->levels->level(position.level_number).run(position.level_position.run_index);
                std::uint64_t const sorted = position.level_position.key_slot_position;
                stamp = run.stamp(sorted);
                version.kind = run.kind(sorted);
                key_slot_t const& slot = run.key_slot(sorted);
                if (slot.size != 0)
                    if (status_t const read_bytes = bytes_of(run, slot, version.bytes); read_bytes.failed())
                        return read_bytes;
            }
            if (version.kind != delta_kind_t::change_k || fold_.closes(version)) {
                closing = version;
                closed = true;
                break;
            }
            if (chain.push_back(delta_view_t {version}) != st::status_t::success_k)
                return status_t {status_code_t::out_of_memory_k, "No room to collect a key's changes"};
            // Versions of one key carry distinct stamps, so the next one older sits at or below this stamp less one.
            if (stamp == unwritten_stamp_k) break;
            at = stamp - 1;
        }

        // A change that closes the chain folds onto nothing, as a chain no version closes does.
        if (closed && closing.kind == delta_kind_t::change_k) {
            if (chain.push_back(delta_view_t {closing}) != st::status_t::success_k)
                return status_t {status_code_t::out_of_memory_k, "No room to collect a key's changes"};
            closing = delta_view_t {delta_kind_t::remove_k, {}};
        }
        if (chain.empty()) {
            if (!closed) return lookup_result_t::missing_k;
            if (closing.kind == delta_kind_t::remove_k) return lookup_result_t::removed_k;
            value = closing.bytes;
            return lookup_result_t::written_k;
        }
        std::reverse(chain.begin(), chain.end());
        std::span<byte_t const> const base = closing.kind == delta_kind_t::write_k ? closing.bytes
                                                                                   : std::span<byte_t const> {};
        auto [applied, apply_status] = fold_.apply(base, std::span<delta_view_t const> {chain}, arena);
        if (apply_status.failed()) return apply_status;
        value = applied;
        return lookup_result_t::written_k;
    }

    /** As @c find answers for a key whose newest version is a change, folding it with no I/O, so a chain reaching a
     *  level whose values sit apart from their key slots answers @c incomplete_k. */
    [[nodiscard]] lookup_result_t find_folded_(reading_t const& read, stored_key_t stored_key,
                                               stored_value_size_t& size) const noexcept {
        utilities::local_arena_t scratch;
        utilities::arena_allocator<byte_t> arena = scratch.allocator();
        std::span<byte_t const> value;
        auto bytes_of = [](auto const&, key_slot_t const& slot, std::span<byte_t const>& bytes) noexcept -> status_t {
            if constexpr (holds_values_inline_k) {
                bytes = std::span<byte_t const> {slot.data, slot.size};
                return {};
            }
            else return {status_code_t::io_k, "A change's value is read through a channel"};
        };
        expected<lookup_result_t> const folded = fold_versions_(read, stored_key, bytes_of, arena, value);
        if (!folded) return lookup_result_t::incomplete_k;
        if (*folded == lookup_result_t::written_k) size = static_cast<stored_value_size_t>(value.size());
        return *folded;
    }

    /** The bytes of the version @p slot of @p run locates, read on @p channel into @p arena where they sit apart from
     *  the key slot. */
    template <typename run_type_>
    static status_t read_bytes_(io_channel_t& channel, run_type_ const& run, key_slot_t const& slot,
                                utilities::arena_allocator<byte_t>& arena, std::span<byte_t const>& bytes) noexcept {
        if constexpr (holds_values_inline_k) {
            bytes = std::span<byte_t const> {slot.data, slot.size};
            return {};
        }
        else {
            byte_t* const into = slot.size != 0 ? arena.allocate(slot.size) : nullptr;
            if (slot.size != 0 && into == nullptr)
                return {status_code_t::out_of_memory_k, "No room to read a change's value"};
            if (status_t const read = run.read_value(channel, slot, std::span<byte_t> {into, slot.size}); read.failed())
                return read;
            bytes = std::span<byte_t const> {into, slot.size};
            return {};
        }
    }

    /** As @c select answers for a key whose newest version is a change, reading the chain's values on @p channel. */
    status_t select_folded_(io_channel_t& channel, reading_t const& read, stored_key_t stored_key,
                            std::span<byte_t> into, stored_value_size_t& size) const noexcept {
        utilities::local_arena_t scratch;
        utilities::arena_allocator<byte_t> arena = scratch.allocator();
        std::span<byte_t const> value;
        auto bytes_of = [&](auto const& run, key_slot_t const& slot, std::span<byte_t const>& bytes) noexcept {
            return read_bytes_(channel, run, slot, arena, bytes);
        };
        auto [folded, fold_status] = fold_versions_(read, stored_key, bytes_of, arena, value);
        if (fold_status.failed()) return fold_status;
        if (folded != lookup_result_t::written_k) return {status_code_t::not_found_k, "No version carries that key"};
        if (into.size() < value.size()) return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};
        if (!value.empty()) std::memcpy(into.data(), value.data(), value.size());
        size = static_cast<stored_value_size_t>(value.size());
        return {};
    }

    /** Folds the keys of @p stored_keys that @p requests names, whose newest versions are changes, once a batched
     *  select's reads retired, and hands each value to @p sink whole. */
    template <typename sink_type_>
    status_t select_folded_(io_channel_t& channel, reading_t const& read, std::span<stored_key_t const> stored_keys,
                            std::span<std::size_t const> requests, std::span<lookup_result_t> results,
                            sink_type_& sink) const noexcept {
        for (std::size_t const request : requests) {
            utilities::local_arena_t scratch;
            utilities::arena_allocator<byte_t> arena = scratch.allocator();
            std::span<byte_t const> value;
            auto bytes_of = [&](auto const& run, key_slot_t const& slot, std::span<byte_t const>& bytes) noexcept {
                return read_bytes_(channel, run, slot, arena, bytes);
            };
            auto [folded, fold_status] = fold_versions_(read, stored_keys[request], bytes_of, arena, value);
            if (fold_status.failed()) return fold_status;
            results[request] = folded;
            if (folded == lookup_result_t::written_k) sink(request, std::uint64_t {0}, value);
        }
        return {};
    }

    /** A view naming @p newest, when not null, ahead of @p older, below levels its caller fills in, which no reader
     *  reaches until it is published. */
    static expected<std::unique_ptr<view_t>> next_view_(memory_run_t const* newest,
                                                        std::span<memory_run_t const* const> older,
                                                        transaction_stamp_t oldest_readable) noexcept {
        std::size_t const ahead = newest != nullptr ? 1 : 0;
        typename memory_view_t::runs_t runs;
        if (runs.resize(older.size() + ahead) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to lay out a view"};
        if (newest != nullptr) runs[0] = newest;
        for (std::size_t index = 0; index != older.size(); ++index) runs[index + ahead] = older[index];
        std::unique_ptr<view_t> view {new (std::nothrow)
                                          view_t {memory_view_t::of(std::move(runs)), nullptr, oldest_readable}};
        if (!view) return status_t {status_code_t::out_of_memory_k, "No room to lay out a view"};
        return view;
    }

    /** Publishes @p newer in place of the current view, which only the thread holding the leader word does, and hands
     *  the view it replaces to the ledger, which frees it once that thread lets the word go and its readers left. */
    void replace_view_(std::unique_ptr<view_t> newer) noexcept {
        view_.replace(*newer);
        view_t* const older = std::exchange(current_, std::move(newer)).release();
        ledger_->unpublish(older->unpublished, older, &destroy_view_);
    }

    static void destroy_view_(void* view) noexcept { delete static_cast<view_t*>(view); }

    /**
     *  @brief Takes the leader word and lays out the view the tree's next level set will be named by: the current
     *      memory runs but those the publication wrote out, which are the oldest, since commits only prepend.
     *  @return Success with the word held, or the status that made the tree abandon its set, with the word let go.
     */
    static status_t prepare_levels_(void* context) noexcept {
        publication_t& publication = *static_cast<publication_t*>(context);
        nvme_store& owner = *publication.owner;
        owner.ledger_->lead();
        std::span<memory_run_t const* const> const runs = owner.current_->memory_runs.runs();
        std::size_t const kept = runs.size() - publication.flushed.size();
        assert(runs.size() >= publication.flushed.size() &&
               (publication.flushed.empty() || runs[kept] == publication.flushed[0]) &&
               "A flush writes out the oldest memory runs");
        auto [next, next_status] = next_view_(nullptr, runs.first(kept),
                                              std::max(owner.current_->oldest_readable, publication.oldest_readable));
        if (next_status.succeeded() && publication.freed.reserve(publication.flushed.size()) != st::status_t::success_k)
            next_status = status_t {status_code_t::out_of_memory_k, "A flush could not take its memory runs over"};
        if (next_status.failed()) {
            owner.ledger_->stop_leading();
            return next_status;
        }
        publication.next = std::move(next);
        return {};
    }

    /** Publishes the view @c prepare_levels_ laid out, naming the tree's new level set, takes the flushed runs over
     *  and bounds the durable stamp by what memory still holds, lets the leader word go, which waits until no reader
     *  is left that could hold a flushed run or the replaced set, frees the runs, and calls the hook it chains to. */
    static status_t publish_levels_(void* context) noexcept {
        publication_t& publication = *static_cast<publication_t*>(context);
        nvme_store& owner = *publication.owner;
        publication.next->levels = &owner.tree_.levels();
        owner.replace_view_(std::move(publication.next));
        if (!publication.flushed.empty()) {
            // The flushed runs are the oldest, which the store owns first.
            for (std::size_t index = 0; index != publication.flushed.size(); ++index) [[maybe_unused]]
                st::status_t const taken = publication.freed.push_back(st::assume_reserved,
                                                                       std::move(owner.memory_runs_[index]));
            for (std::size_t index = 0; index != publication.flushed.size(); ++index) owner.memory_runs_.erase(0);
            std::span<memory_run_t const* const> const kept = owner.current_->memory_runs.runs();
            owner.ledger_->bound_view(owner.view_pin_, kept.empty() ? head_stamp_k : kept.back()->oldest_stamp() - 1);
            publication.flushed.clear();
        }
        // Every reader that could hold a flushed run entered before the view replaced above, which this waits out.
        owner.ledger_->stop_leading();
        publication.freed.clear();
        lsm_tree_publication_hook_t const* const chained = publication.chained;
        if (chained == nullptr || chained->notify == nullptr) return {};
        return chained->notify(chained->context);
    }

    /** Room for @p count deltas of one commit of its own. */
    expected<deltas_t> deltas_of_(std::size_t count) const noexcept {
        deltas_t deltas {constrained_allocator<keyed_delta_t, atomic_reference_> {*constraint_}};
        if (deltas.resize(count) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to hand a batch over"};
        return deltas;
    }

    /** Keeps the last delta of every key of @p deltas, in their order, which a batch naming a key twice takes. */
    [[nodiscard]] static std::span<keyed_delta_t> last_of_each_key_(deltas_t& deltas) noexcept {
        std::stable_sort(deltas.begin(), deltas.end(), [](keyed_delta_t const& first, keyed_delta_t const& second) {
            return first.stored_key < second.stored_key;
        });
        std::size_t kept = 0;
        for (std::size_t index = 0; index != deltas.size(); ++index) {
            bool const repeated = index + 1 != deltas.size() &&
                                  deltas[index + 1].stored_key == deltas[index].stored_key;
            if (!repeated) deltas[kept++] = deltas[index];
        }
        return std::span<keyed_delta_t> {deltas.data(), kept};
    }

    /** Lands @p sealed as one commit of its own, flushing first when the memory runs are past their bounds, which
     *  the caller serializes with every other flush, and returns once it is published. */
    status_t commit_alone_(io_channel_t& channel, expected<sealed_commit_t> sealed) noexcept {
        if (!sealed) return sealed.status();
        if (sealed->run && memory_pressure(*sealed) != memory_pressure_t::fits_k)
            if (status_t const flushed = flush(channel); flushed.failed()) return flushed;
        return commit(*sealed);
    }

    static status_t prepare_alone_(void* context) noexcept {
        alone_t const& alone = *static_cast<alone_t const*>(context);
        return alone.owner->prepare(*alone.sealed);
    }

    static void publish_alone_(void* context, transaction_stamp_t stamp) noexcept {
        alone_t const& alone = *static_cast<alone_t const*>(context);
        alone.owner->publish(*alone.sealed, stamp);
    }

    /**
     *  @brief Lays the deltas of @p flushed out as one run's key slots, kinds and the bytes they locate.
     *  @param[in] flushed The memory runs, newest first.
     *  @param[in] survival Which older versions a reader or a mount still needs, as a merge would decide.
     *  @param[inout] arena Where the values of folded changes sit until the run is written.
     *  @param[out] key_slots The key slots in ascending order of their keys, as a run takes them.
     *  @param[out] stamps The stamp of every key slot, the versions of one key newest first.
     *  @param[out] kinds The @c delta_kind_t of every key slot, in the same order.
     *  @param[out] stored_values The values back to back, empty for a layout holding them inline.
     */
    status_t drain_(std::span<memory_run_t const* const> flushed, compaction_stamps_t const& survival,
                    utilities::arena_allocator<byte_t>& arena, drained_slots_t& key_slots, drained_stamps_t& stamps,
                    drained_kinds_t& kinds, drained_values_t& stored_values) const noexcept {
        stamped_deltas_t versions {constrained_allocator<stamped_delta_t, atomic_reference_> {*constraint_}};
        if (status_t const kept = kept_versions(flushed, survival, fold_, versions, arena); kept.failed()) return kept;
        if (key_slots.reserve(versions.size()) != st::status_t::success_k ||
            stamps.reserve(versions.size()) != st::status_t::success_k ||
            kinds.reserve(versions.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A flush could not hold its key slots"};
        std::uint64_t total = 0;
        for (stamped_delta_t const& version : versions) total += version.delta.bytes.size();

        if constexpr (!holds_values_inline_k)
            if (stored_values.resize(static_cast<std::size_t>(total)) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A flush could not hold its values"};
        std::uint64_t location = sorted_run_layout_t::values_offset(versions.size());
        std::uint64_t written = 0;
        for (stamped_delta_t const& version : versions) {
            std::span<byte_t const> const bytes = version.delta.bytes;
            key_slot_t built {};
            built.stored_key = version.stored_key;
            built.size = static_cast<stored_value_size_t>(bytes.size());
            if constexpr (holds_values_inline_k) {
                // A merge may compose changes wider than any one of them, which a key slot cannot hold.
                if (bytes.size() > sizeof(built.data))
                    return {status_code_t::capacity_exhausted_k, "A folded change outgrows its key slot"};
                if (!bytes.empty()) std::memcpy(built.data, bytes.data(), bytes.size());
            }
            else {
                built.location = static_cast<stored_value_offset_t>(location);
                if (!bytes.empty()) std::memcpy(stored_values.data() + written, bytes.data(), bytes.size());
                location += bytes.size();
                written += bytes.size();
            }
            [[maybe_unused]] st::status_t const placed = key_slots.push_back(std::move(built));
            [[maybe_unused]] st::status_t const stamped = stamps.push_back(std::uint64_t {version.stamp});
            [[maybe_unused]] st::status_t const kinded = kinds.push_back(static_cast<std::uint8_t>(version.delta.kind));
        }
        return {};
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

    /** Whether the memory runs of @p view hold the bounds they were mounted with. */
    [[nodiscard]] bool flush_due_(view_t const& view) const noexcept {
        return memory_deltas_(view) >= config_.memory_runs_max_deltas ||
               memory_bytes_(view) >= config_.memory_runs_max_bytes;
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

    /**
     *  @brief Takes the leader word, publishes a view naming @p merged where exactly the runs of @p group sat, moves
     *      those runs into @p away, and lets the word go, which returns once no reader can reach them.
     *  @param[in] group The runs merged, newest first, which commits since may have put newer runs ahead of.
     *  @param[out] away Room reserved for the runs of @p group, which the caller frees.
     *  @param[in] horizon The stamp published when the merge began, which a head reader below it reads past.
     */
    status_t publish_merged_(std::span<memory_run_t const* const> group, std::unique_ptr<memory_run_t> merged,
                             st::basic_vector<std::unique_ptr<memory_run_t>>& away,
                             transaction_stamp_t horizon) noexcept {
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
        laid_out[first] = merged.get();
        for (std::size_t index = first + group.size(); index != runs.size(); ++index)
            laid_out[index - group.size() + 1] = runs[index];
        std::unique_ptr<view_t> next {new (std::nothrow) view_t {memory_view_t::of(std::move(laid_out)),
                                                                 current_->levels,
                                                                 std::max(current_->oldest_readable, horizon),
                                                                 {}}};
        if (!next) {
            ledger_->stop_leading();
            return {status_code_t::out_of_memory_k, "No room to lay out a merged view"};
        }
        // The runs the collection owns run oldest first, opposite to the view, so the group's oldest sits here.
        std::size_t const oldest = runs.size() - first - group.size();
        replace_view_(std::move(next));
        for (std::size_t index = 0; index != group.size(); ++index) [[maybe_unused]]
            st::status_t const taken = away.push_back(st::assume_reserved, std::move(memory_runs_[oldest + index]));
        memory_runs_[oldest] = std::move(merged);
        for (std::size_t index = 1; index != group.size(); ++index) memory_runs_.erase(oldest + 1);
        ledger_->stop_leading();
        return {};
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The bounds this collection was mounted with. */
    collection_config_t config_ {};

    /** The budget the memory runs and the cache are charged to, null until the collection is mounted. */
    constraint_t* constraint_ = nullptr;

    /** The tree the flushed writes live in. */
    tree_t tree_;

    /** The stamps this collection's writes carry and its readers pin, borrowed and outliving the collection. */
    ledger_t* ledger_ = nullptr;

    /** Registers the published view with the ledger, bounding the durable stamp below the oldest memory run. */
    pin_t view_pin_;

    /** The view readers load, which @c current_ owns. */
    published_view<view_t, atomic_reference_> view_;

    /** The view published last, owned here and replaced only under the ledger's leader word. */
    std::unique_ptr<view_t> current_;

    /** Every memory run the current view names, oldest first, freed by the flush that writes them out or the merge
     *  that replaces them, once no reader can reach them. */
    st::basic_vector<std::unique_ptr<memory_run_t>> memory_runs_;

    /** The view a leader laid out for the commit it prepared, published or forgotten before it lets the word go. */
    std::unique_ptr<view_t> prepared_view_;

    /** The values of versions point reads paid I/O for, never cleared. */
    value_cache_t cache_;

    /** Bytes every value takes, zero for a layout whose values differ in width. */
    stored_value_size_t value_size_ = 0;

    /** Bytes the largest value this collection admits takes. */
    stored_value_size_t value_max_size_ = 0;

    /** What a change delta does, which reads and merges fold with. */
    [[no_unique_address]] fold_t fold_ {};

#pragma endregion Private Members
};

#pragma endregion Store

#pragma endregion Store

#pragma region Store Cursor

#pragma region Cursor

/**
 *  @brief Every key of one store in ascending order, each with what it is and how wide it is, read a page at a time.
 *
 *  Each page is read inside the ledger's epochs from whichever view is published, and the epochs are left between
 *  pages, so a commit, merge, flush or compaction replacing a view waits for at most one page of a scan. Every page
 *  reads at the one stamp the scan is pinned at, whose versions every merge keeps, and starts at the key past the
 *  page before it, which that page found without handing it out, so no key is visited twice or skipped. A head scan
 *  pins the published stamp it starts at for as long as it lives.
 */
template <typename key_slot_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, utilities::fold_concept fold_type_>
class nvme_store_cursor {
  public:

#pragma region Types

    /** The key slot every run of the walked store holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of the walked store lives on. */
    using volume_t = volume_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The store this cursor walks. */
    using nvme_store_t = nvme_store<key_slot_t, volume_t, atomic_reference_, waiter_type_, fold_type_>;

    /** The view of that store the walk reads. */
    using view_t = typename nvme_store_t::view_t;

    /** One memory run of that view. */
    using memory_run_t = typename nvme_store_t::memory_run_t;

    /** One key of a page. */
    using entry_t = scan_entry<stored_key_t>;

    /** Keys one page holds. */
    static constexpr std::size_t page_keys_k = scan_page_keys<stored_key_t>();

#pragma endregion Types

#pragma region Construction

    nvme_store_cursor() noexcept = default;
    nvme_store_cursor(nvme_store_cursor const&) = delete;
    nvme_store_cursor& operator=(nvme_store_cursor const&) = delete;
    nvme_store_cursor(nvme_store_cursor&&) noexcept = default;
    nvme_store_cursor& operator=(nvme_store_cursor&&) noexcept = default;

    /** A walk over @p source positioned on its first key not below @p from, where @p source must outlive the cursor,
     *  seeing what a reader pinned at @p stamp sees, announced on executor thread @p thread_index's epoch slot while
     *  it reads a page, the first of which it reads before returning. */
    static expected<nvme_store_cursor> over(nvme_store_t const& source, stored_key_t from = limit<stored_key_t>::min(),
                                            transaction_stamp_t stamp = head_stamp_k,
                                            std::size_t thread_index = outside_executor_k) noexcept {
        nvme_store_cursor cursor;
        cursor.source_ = &source;
        cursor.thread_index_ = thread_index;
        cursor.stamp_ = stamp == head_stamp_k ? source.ledger_->pin_reader(cursor.pin_) : stamp;
        if (status_t const filled = cursor.fill_(from); filled.failed()) return filled;
        return cursor;
    }

#pragma endregion Construction

#pragma region Walk

    /** Whether every key has been visited. */
    [[nodiscard]] bool reached_end() const noexcept { return at_ == page().size() && filled_ <= page_keys_k; }

    /** The key the scan sits on, meaningless once the walk has ended. */
    [[nodiscard]] stored_key_t stored_key() const noexcept { return page_[at_].stored_key; }

    /** Whether the key the scan sits on carries a value or a removal, @c missing_k once the walk has ended. */
    [[nodiscard]] lookup_result_t state() const noexcept {
        return reached_end() ? lookup_result_t::missing_k : page_[at_].state;
    }

    /** Bytes the value of that key takes, which is zero for a removal and for an empty value. */
    [[nodiscard]] stored_value_size_t size() const noexcept { return reached_end() ? 0 : page_[at_].size; }

    /** The keys of the page the scan sits on, every one of which comes before the keys of the next page. */
    [[nodiscard]] std::span<entry_t const> page() const noexcept {
        return std::span<entry_t const> {page_.data(), std::min(filled_, page_keys_k)};
    }

    /** Whether a page follows the one the scan sits on, and in @p from the key it starts at. */
    [[nodiscard]] bool follows(stored_key_t& from) const noexcept {
        if (filled_ <= page_keys_k) return false;
        from = page_[page_keys_k].stored_key;
        return true;
    }

    /** Moves to the next key, reading the next page once this one is spent. */
    status_t advance() noexcept {
        if (at_ != page().size()) ++at_;
        stored_key_t from {};
        if (at_ != page().size() || !follows(from)) return {};
        return fill_(from);
    }

#pragma endregion Walk

  private:

#pragma region Private Types

    /** One reader inside the scanned collection's epochs, announced while a page is read. */
    using reader_t = typename nvme_store_t::ledger_t::epochs_t::reader_t;

    /** Where one run of one level the scan walks has got to. */
    struct run_cursor_t {

        /** The level the run belongs to, zero being the one flushes land on. */
        std::uint32_t level_number = 0;

        /** The run inside that level, older first where the runs overlap. */
        std::uint32_t run_index = 0;

        /** The next sorted position inside that run. */
        std::uint64_t position = 0;

        /** Key slots the run holds, past which the cursor is spent. */
        std::uint64_t population = 0;
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** Reads the keys from @p from on into the page inside the ledger's epochs, from the view published now, with one
     *  key past the page beside it, which the next page starts at. */
    status_t fill_(stored_key_t from) noexcept {
        reader_t const inside = source_->ledger_->epochs().enter(thread_index_);
        // Every version at or below the pinned stamp stays in whichever view a merge publishes later.
        view_ = source_->view_.load();
        at_ = 0;
        filled_ = 0;
        status_t const positioned = position_(from);
        while (positioned.succeeded() && filled_ != page_.size() && next_(page_[filled_])) ++filled_;
        view_ = nullptr;
        return positioned;
    }

    /** Places a cursor on the first key not below @p from in every memory run and every run of every level. */
    status_t position_(stored_key_t from) noexcept {
        std::span<memory_run_t const* const> const memory_runs = view_->memory_runs.runs();
        if (memory_positions_.resize(memory_runs.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A scan could not hold a cursor per memory run"};
        for (std::size_t index = 0; index != memory_runs.size(); ++index)
            memory_positions_[index] = memory_runs[index]->lower_bound(from);

        typename nvme_store_t::level_set_t const& levels = *view_->levels;
        std::size_t runs = 0;
        for (std::size_t number = 0; number != levels.size(); ++number) runs += levels.level(number).size();
        cursors_.clear();
        if (cursors_.reserve(runs) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A scan could not hold a cursor per run"};
        for (std::size_t number = 0; number != levels.size(); ++number) {
            auto const& level = levels.level(number);
            for (std::size_t index = 0; index != level.size(); ++index) {
                run_cursor_t cursor;
                cursor.level_number = static_cast<std::uint32_t>(number);
                cursor.run_index = static_cast<std::uint32_t>(index);
                cursor.population = level.run(index).population_count();
                cursor.position = lower_bound_(level.run(index), cursor.population, from);
                [[maybe_unused]] st::status_t const placed = cursors_.push_back(std::move(cursor));
            }
        }
        return {};
    }

    /** Moves the cursors past the next key, taking its newest version at or below the scan's stamp into @p entry and
     *  skipping the versions behind it and any key whose every version is newer; answers whether a key was left. */
    bool next_(entry_t& entry) noexcept {
        std::span<memory_run_t const* const> const memory_runs = view_->memory_runs.runs();
        for (;;) {
            stored_key_t smallest = limit<stored_key_t>::max();
            bool any = false;
            for (std::size_t index = 0; index != memory_runs.size(); ++index) {
                if (memory_positions_[index] == memory_runs[index]->size()) continue;
                stored_key_t const head = memory_runs[index]->stored_key(memory_positions_[index]);
                if (any && !(head < smallest)) continue;
                smallest = head;
                any = true;
            }
            for (std::size_t index = 0; index != cursors_.size(); ++index) {
                if (cursors_[index].position == cursors_[index].population) continue;
                stored_key_t const head = key_slot_at_(cursors_[index]).stored_key;
                if (any && !(head < smallest)) continue;
                smallest = head;
                any = true;
            }
            if (!any) return false;

            entry.stored_key = smallest;
            bool const taken = take_newest_memory_run_(entry) || take_youngest_run_(entry);

            // Every run holds the versions of a key side by side, so each cursor passes all of them at once.
            for (std::size_t index = 0; index != memory_runs.size(); ++index)
                while (memory_positions_[index] != memory_runs[index]->size() &&
                       memory_runs[index]->stored_key(memory_positions_[index]) == smallest)
                    ++memory_positions_[index];
            for (std::size_t index = 0; index != cursors_.size(); ++index)
                while (cursors_[index].position != cursors_[index].population &&
                       key_slot_at_(cursors_[index]).stored_key == smallest)
                    ++cursors_[index].position;
            if (taken) return true;
        }
    }

    /** The first of the @p population sorted positions of @p run whose key is not below @p from. */
    template <typename run_type_>
    [[nodiscard]] static std::uint64_t lower_bound_(run_type_ const& run, std::uint64_t population,
                                                    stored_key_t from) noexcept {
        std::uint64_t low = 0;
        std::uint64_t high = population;
        while (low != high) {
            std::uint64_t const middle = low + (high - low) / 2;
            if (run.key_slot(middle).stored_key < from) low = middle + 1;
            else high = middle;
        }
        return low;
    }

    /** The key slot @p cursor sits on, which must not be spent. */
    [[nodiscard]] key_slot_t const& key_slot_at_(run_cursor_t const& cursor) const noexcept {
        return view_->levels->level(cursor.level_number).run(cursor.run_index).key_slot(cursor.position);
    }

    /** Takes the newest memory run carrying a version of @p entry's key at or below the scan's stamp as its answer,
     *  which outranks every level; answers whether any does. */
    bool take_newest_memory_run_(entry_t& entry) noexcept {
        std::span<memory_run_t const* const> const memory_runs = view_->memory_runs.runs();
        for (std::size_t index = 0; index != memory_runs.size(); ++index) {
            memory_run_t const& run = *memory_runs[index];
            std::size_t position = memory_positions_[index];
            // A merged run holds a key's versions newest first, and the first at or below the stamp answers.
            while (position != run.size() && run.stored_key(position) == entry.stored_key &&
                   run.stamp(position) > stamp_)
                ++position;
            if (position == run.size() || !(run.stored_key(position) == entry.stored_key)) continue;
            delta_view_t const delta = run.delta(position);
            take_(delta.kind, delta.bytes.size(), entry);
            return true;
        }
        return false;
    }

    /** Takes the newest version of @p entry's key at or below the scan's stamp, which does @p kind and carries @p size
     *  bytes, as its answer, folding the versions below a change. */
    void take_(delta_kind_t kind, std::size_t size, entry_t& entry) noexcept {
        entry.size = 0;
        if (kind == delta_kind_t::change_k)
            entry.state = source_->find_folded_(typename nvme_store_t::reading_t {view_, stamp_}, entry.stored_key,
                                                entry.size);
        else if (kind == delta_kind_t::remove_k) entry.state = lookup_result_t::removed_k;
        else {
            entry.state = lookup_result_t::written_k;
            entry.size = static_cast<stored_value_size_t>(size);
        }
    }

    /**
     *  @brief Takes the shallowest and, inside a level, the youngest run carrying a version of @p entry's key at or
     *      below the scan's stamp as its answer, the newest such version inside it.
     *  @return Whether any run carries one.
     */
    bool take_youngest_run_(entry_t& entry) noexcept {
        std::size_t chosen = cursors_.size();
        std::uint64_t chosen_position = 0;
        for (std::size_t index = 0; index != cursors_.size(); ++index) {
            run_cursor_t const& cursor = cursors_[index];
            if (chosen != cursors_.size() && !outranks_(cursor, cursors_[chosen])) continue;
            auto const& run = view_->levels->level(cursor.level_number).run(cursor.run_index);
            std::uint64_t position = cursor.position;
            while (position != cursor.population && run.key_slot(position).stored_key == entry.stored_key &&
                   run.stamp(position) > stamp_)
                ++position;
            if (position == cursor.population || !(run.key_slot(position).stored_key == entry.stored_key)) continue;
            chosen = index;
            chosen_position = position;
        }
        if (chosen == cursors_.size()) return false;
        run_cursor_t const& cursor = cursors_[chosen];
        auto const& run = view_->levels->level(cursor.level_number).run(cursor.run_index);
        take_(run.kind(chosen_position), run.key_slot(chosen_position).size, entry);
        return true;
    }

    /** Whether @p first carries a newer version of a key than @p second does. */
    [[nodiscard]] static bool outranks_(run_cursor_t const& first, run_cursor_t const& second) noexcept {
        if (first.level_number != second.level_number) return first.level_number < second.level_number;
        return first.run_index > second.run_index;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The store walked, which outlives the cursor. */
    nvme_store_t const* source_ = nullptr;

    /** The published stamp a head scan reads at, which keeps its versions in every merge while the scan lives. */
    typename nvme_store_t::pin_t pin_;

    /** The stamp the scan reads at, which sees the newest version of every key at or below it. */
    transaction_stamp_t stamp_ = head_stamp_k;

    /** The executor thread the scan announces on while it reads a page. */
    std::size_t thread_index_ = outside_executor_k;

    /** The view the page being read walks, null between pages, when no reader keeps it alive. */
    view_t const* view_ = nullptr;

    /** The next sorted position inside every memory run of the view, newest run first. */
    st::basic_vector<std::size_t> memory_positions_;

    /** One cursor per run of every level the view names. */
    st::basic_vector<run_cursor_t> cursors_;

    /** The keys of the page, then the key the next page starts at. */
    std::array<entry_t, page_keys_k + 1> page_ {};

    /** Entries the page read, a page and one more while another page follows. */
    std::size_t filled_ = 0;

    /** The entry of the page the scan sits on. */
    std::size_t at_ = 0;

#pragma endregion Private Members
};

#pragma endregion Cursor

#pragma endregion Store Cursor

} // namespace unum::ustore::blobs
