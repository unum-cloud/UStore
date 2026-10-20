/**
 *  @file include/ustore/blobs/collection_view.hpp
 *  @author Ash Vardanian
 *  @date October 18, 2026
 *  @brief The immutable list of runs a collection's readers walk, and the one word it is published through.
 *
 *  A view names runs newest first, and their stamps never overlap, so the first run carrying a key holds its newest
 *  version and a run whose newest stamp is not past a watch ends the search. A commit publishes a new view holding
 *  its run ahead of the old view's runs, with one release store; a reader loads the published stamp, then the view
 *  with acquire, and walks it with no lock and no write to any shared line.
 *
 *  Whoever replaces a view frees it in the same call, once every reader that entered before the swap has left, which
 *  an epoch domain waits for. A reader announces itself by storing to a slot its thread alone holds, so entering and
 *  leaving write no line another reader writes, and only a waiting writer reads every slot.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <atomic>  // `std::atomic_thread_fence`, `std::memory_order_acquire`, `std::memory_order_release`
#include <memory>  // `std::allocator_traits`, `std::unique_ptr`
#include <new>     // `std::nothrow`
#include <span>    // `std::span`
#include <utility> // `std::exchange`, `std::move`, `std::swap`

#include <forkunion/atomics.hpp>      // `fu::preferred_atomic_ref`
#include <forkunion/capabilities.hpp> // `fu::preferred_yield_t`
#include <forkunion/types.hpp>        // `fu::default_alignment_k`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <smashtable/shared.hpp>       // `st::default_allocator`, `st::hash`

#include "ustore/blobs/nvme_key_slot.hpp"  // `transaction_stamp_t`, `unwritten_stamp_k`
#include "ustore/utilities/folds.hpp"      // `delta_view_t`
#include "ustore/utilities/nvme_disks.hpp" // `calling_thread_identity`, `thread_identity_t`
#include "ustore/utilities/types.hpp"      // `expected`, `status_t`

namespace unum::ustore::blobs {

using utilities::calling_thread_identity;
using utilities::delta_view_t;
using utilities::thread_identity_t;

/**
 *  @brief The runs one collection's readers see, newest first, which nothing changes once it is published.
 *  @tparam run_type_ A run answering @c size, @c stored_key, @c delta, @c stamp, @c lower_bound and the newest and
 *      oldest stamps it carries.
 */
template <typename run_type_, typename allocator_type_ = st::default_allocator<byte_t>>
class collection_view {
  public:
    using run_t = run_type_;
    using stored_key_t = typename run_t::stored_key_t;
    using allocator_t = allocator_type_;

    /** The runs a view names, newest first. */
    using runs_t = st::basic_vector<run_t const*,
                                    typename std::allocator_traits<allocator_t>::template rebind_alloc<run_t const*>>;

    collection_view() noexcept = default;
    collection_view(collection_view&&) noexcept = default;
    collection_view& operator=(collection_view&&) noexcept = default;
    collection_view(collection_view const&) = delete;
    collection_view& operator=(collection_view const&) = delete;

    /**
     *  @brief A view naming @p newest ahead of every run @p older names, changing neither.
     *  @retval invalid_argument_k A run whose oldest stamp is not past every stamp @p older holds.
     *  @retval out_of_memory_k The list of runs could not be allocated.
     */
    static expected<collection_view> extended(collection_view const& older, run_t const& newest,
                                              allocator_t allocator = {}) noexcept {
        if (newest.oldest_stamp() <= older.newest_stamp())
            return status_t {status_code_t::invalid_argument_k, "A newer run starts past every stamp of the view"};
        collection_view view {typename runs_t::allocator_t(allocator)};
        if (view.runs_.resize(older.runs_.size() + 1) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to extend a view"};
        view.runs_[0] = &newest;
        for (std::size_t index = 0; index != older.runs_.size(); ++index) view.runs_[index + 1] = older.runs_[index];
        return view;
    }

    /** A view naming @p runs, listed newest first with stamps that never overlap, which a commit lays out before
     *  its own run is stamped so that publishing it allocates nothing. */
    [[nodiscard]] static collection_view of(runs_t&& runs) noexcept { return collection_view {std::move(runs)}; }

    /** The runs, newest first. */
    [[nodiscard]] std::span<run_t const* const> runs() const noexcept { return runs_; }

    /** The newest stamp any run carries, or @c unwritten_stamp_k for a view of none. */
    [[nodiscard]] transaction_stamp_t newest_stamp() const noexcept {
        return runs_.empty() ? unwritten_stamp_k : runs_[0]->newest_stamp();
    }

    /**
     *  @brief The newest version of @p stored_key stamped at or below @p at, which is what a reader at @p at sees.
     *  @param[out] found What that version does and its bytes, untouched when there is none.
     *  @return Whether any run carries such a version, a removal included.
     */
    [[nodiscard]] bool find(stored_key_t const& stored_key, transaction_stamp_t at,
                            delta_view_t& found) const noexcept {
        transaction_stamp_t version = unwritten_stamp_k;
        return find(stored_key, at, found, version);
    }

    /** As @c find, also answering in @p version the stamp of the version found, below which a fold reads on. */
    [[nodiscard]] bool find(stored_key_t const& stored_key, transaction_stamp_t at, delta_view_t& found,
                            transaction_stamp_t& version) const noexcept {
        for (run_t const* run : runs_) {
            if (run->oldest_stamp() > at) continue;
            for (std::size_t position = run->lower_bound(stored_key);
                 position != run->size() && run->stored_key(position) == stored_key; ++position)
                if (run->stamp(position) <= at) {
                    found = run->delta(position);
                    version = run->stamp(position);
                    return true;
                }
        }
        return false;
    }

    /** Whether a commit newer than @p from wrote @p stored_key, which only the key's newest version decides. */
    [[nodiscard]] bool written_since(stored_key_t const& stored_key, transaction_stamp_t from) const noexcept {
        for (run_t const* run : runs_) {
            if (run->newest_stamp() <= from) return false;
            std::size_t const position = run->lower_bound(stored_key);
            if (position != run->size() && run->stored_key(position) == stored_key) return run->stamp(position) > from;
        }
        return false;
    }

    /** Whether a commit newer than @p from wrote any key from @p lower to @p upper inclusive, so a key inserted or
     *  removed where a walk found none conflicts too. */
    [[nodiscard]] bool range_written_since(stored_key_t const& lower, stored_key_t const& upper,
                                           transaction_stamp_t from) const noexcept {
        if (upper < lower) return false;
        for (run_t const* run : runs_) {
            if (run->newest_stamp() <= from) return false;
            for (std::size_t position = run->lower_bound(lower);
                 position != run->size() && !(upper < run->stored_key(position)); ++position)
                if (run->stamp(position) > from) return true;
        }
        return false;
    }

  private:
    explicit collection_view(typename runs_t::allocator_t allocator) noexcept : runs_(std::move(allocator)) {}
    explicit collection_view(runs_t&& runs) noexcept : runs_(std::move(runs)) {}

    /** The runs, newest first, borrowed from the collection, which frees them only once no reader can reach them. */
    runs_t runs_;
};

/** The one word a collection publishes its view through: replaced with release by the single publisher, and
 *  loaded with acquire by every reader. */
template <typename view_type_>
class published_view {
  public:
    using view_t = view_type_;

    published_view() noexcept = default;
    published_view(published_view const&) = delete;
    published_view& operator=(published_view const&) = delete;

    /** Takes the word over, which only an owner no reader reaches yet may do. */
    published_view(published_view&& other) noexcept : view_(std::exchange(other.view_, nullptr)) {}

    /** Swaps the words, which only owners no reader reaches yet may do. */
    published_view& operator=(published_view&& other) noexcept {
        std::swap(view_, other.view_);
        return *this;
    }

    /** The view published last, whose runs and their bytes are visible to the caller in whole. */
    [[nodiscard]] view_t const* load() const noexcept {
        return fu::preferred_atomic_ref<view_t const* const> {view_}.load(std::memory_order_acquire);
    }

    /** Publishes @p newer and answers the view it replaces, which the caller frees once every reader that entered
     *  before has left. */
    view_t const* replace(view_t const& newer) noexcept {
        fu::preferred_atomic_ref<view_t const*> const word {view_};
        // Relaxed, since only the publisher stores this word and it reads its own last store.
        view_t const* const older = word.load(std::memory_order_relaxed);
        word.store(&newer, std::memory_order_release);
        return older;
    }

  private:

    /** The published view, on a line of its own so a publication evicts no neighbour readers hold. */
    alignas(fu::default_alignment_k) view_t const* view_ = nullptr;
};

/** One thread's announcement, alone on its lines, so a reader entering writes nothing another reader writes. */
struct alignas(fu::default_alignment_k) epoch_slot_t {

    /** The epoch its thread entered at, zero while the thread is outside; stored by that thread alone. */
    std::uint64_t announced = 0;

    /** Readers its thread holds at once, so a nested one neither announces nor leaves; that thread's alone. */
    std::size_t depth = 0;

    /** The thread outside the executor holding the slot, zero while it is free; claimed by that thread's outermost
     *  reader and given back when it leaves, and always zero on an executor thread's own slot. */
    thread_identity_t holder = 0;
};

/**
 *  @brief The readers of every view one ledger's collections publish, which a writer that replaced a view waits out
 *      before it frees the view.
 *
 *  A reader loads the epoch, stores it to its own slot, fences, and only then loads a view; leaving stores zero. An
 *  executor thread takes the slot its index names and writes no shared word. Any other thread's outermost reader
 *  claims a free shared slot, starting at the one its identity hashes to, so a thread keeps finding the line it
 *  claimed last, and gives it back when it leaves; the shared slots double only when every one is held at once, so
 *  their count is bounded by the most outside readers ever inside together. A writer publishes, moves the epoch on
 *  with release, fences, and waits on every slot announcing an older epoch: a reader the scan missed fenced after the
 *  writer did and loads the new view, and one that announced the moved epoch read it from the writer and loads the
 *  new view too. Readers never wait; @c wait_for_readers waits for them, called only outside the domain.
 */
class epoch_domain {
  public:

    /** One reader inside the domain, announced until it dies, on the thread that entered. */
    class reader_t {
      public:
        reader_t() noexcept = default;
        reader_t(reader_t const&) = delete;
        reader_t& operator=(reader_t const&) = delete;

        reader_t(reader_t&& other) noexcept
            : domain_(std::exchange(other.domain_, nullptr)), slot_(std::exchange(other.slot_, nullptr)) {}

        reader_t& operator=(reader_t&& other) noexcept {
            leave_();
            domain_ = std::exchange(other.domain_, nullptr);
            slot_ = std::exchange(other.slot_, nullptr);
            return *this;
        }

        ~reader_t() noexcept { leave_(); }

      private:
        friend class epoch_domain;

        reader_t(epoch_domain& domain, epoch_slot_t& slot) noexcept : domain_(&domain), slot_(&slot) {}

        void leave_() noexcept {
            if (!slot_) return;
            if (slot_ == &domain_->spilled_)
                fu::preferred_atomic_ref<std::size_t> {slot_->depth}.fetch_sub(1, std::memory_order_release);
            else if (--slot_->depth == 0) {
                fu::preferred_atomic_ref<std::uint64_t> {slot_->announced}.store(0, std::memory_order_release);
                // Released after the depth, which the next holder's claim acquires; other threads try to claim the
                // word meanwhile, so even its holder reads it atomically.
                fu::preferred_atomic_ref<thread_identity_t> const holder {slot_->holder};
                if (holder.load(std::memory_order_relaxed) != 0) holder.store(0, std::memory_order_release);
            }
            slot_ = nullptr;
        }

        /** The domain entered, null once left. */
        epoch_domain* domain_ = nullptr;

        /** The slot announcing this reader, null once left. */
        epoch_slot_t* slot_ = nullptr;
    };

    epoch_domain() noexcept = default;
    epoch_domain(epoch_domain&&) = delete;
    epoch_domain& operator=(epoch_domain&&) = delete;
    epoch_domain(epoch_domain const&) = delete;
    epoch_domain& operator=(epoch_domain const&) = delete;

    /** Opens one slot per executor thread, indexed by the thread, which a mount does before anything enters. */
    status_t reserve_threads(std::size_t threads) noexcept {
        executor_slots_.reset(new (std::nothrow) epoch_slot_t[threads]);
        executor_threads_ = executor_slots_ ? threads : 0;
        if (!executor_slots_) return {status_code_t::out_of_memory_k, "No room for a slot per executor thread"};
        return {};
    }

    /** Announces a thread outside the executor on a shared slot its outermost reader claims, before it loads a view. */
    [[nodiscard]] reader_t enter() noexcept {
        thread_identity_t const identity = calling_thread_identity();
        for (shared_slots_t const* shared = load_(shared_);; shared = load_(shared_)) {
            if (epoch_slot_t* const slot = claimed_(shared, identity)) return announce_(*slot);
            if (!grown_(shared)) break;
        }
        // A thread no slot could be allocated for is counted on the one spilled slot, which only makes writers wait.
        fu::preferred_atomic_ref<std::size_t> {spilled_.depth}.fetch_add(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return reader_t {*this, spilled_};
    }

    /** Announces executor thread @p thread_index on the slot @c reserve_threads opened for it, or a thread outside
     *  the executor as @c enter does for an index past its threads. */
    [[nodiscard]] reader_t enter(std::size_t thread_index) noexcept {
        if (thread_index >= executor_threads_) return enter();
        return announce_(executor_slots_[thread_index]);
    }

    /**
     *  @brief Returns once every reader that entered before the call has left, so whatever the caller unpublished
     *      before it, or saw another thread unpublish, no reader can reach any more.
     *  @note Never called by a thread inside this domain, which would wait for itself.
     */
    void wait_for_readers() noexcept {
        // Released, so a reader announcing the moved epoch loads every view published before the call.
        std::uint64_t const moved =
            fu::preferred_atomic_ref<std::uint64_t> {epoch_}.fetch_add(1, std::memory_order_release) + 1;
        // Pairs with a reader's fence: either this reads its announcement, or the reader loads the new view.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        for (std::size_t spilled = load_(spilled_.depth); spilled != 0; spilled = load_(spilled_.depth))
            fu::preferred_yield_t {}(spilled_.depth, spilled, std::size_t {0});
        for_each_slot_([moved](epoch_slot_t& slot) noexcept {
            for (std::uint64_t announced = load_(slot.announced); announced != 0 && announced < moved;
                 announced = load_(slot.announced))
                fu::preferred_yield_t {}(slot.announced, announced, std::size_t {0});
        });
    }

  private:

    /** The slots threads outside the executor claim, a power of two of them, which a domain whose every slot is held
     *  replaces whole with a table twice as long naming the same slots first. */
    struct shared_slots_t {

        /** One less than the slots. */
        std::size_t mask = 0;

        /** The slots, each pinned in a block @c blocks_ owns. */
        std::unique_ptr<epoch_slot_t*[]> slots;
    };

    template <typename word_type_>
    [[nodiscard]] static word_type_ load_(word_type_ const& word) noexcept {
        return fu::preferred_atomic_ref<word_type_ const> {word}.load(std::memory_order_acquire);
    }

    /** Announces the slot's thread, unless one of its readers already did. */
    reader_t announce_(epoch_slot_t& slot) noexcept {
        if (slot.depth++ == 0) {
            std::uint64_t const epoch = load_(epoch_);
            fu::preferred_atomic_ref<std::uint64_t> {slot.announced}.store(epoch, std::memory_order_relaxed);
            // Pairs with a waiting writer's fence: either it reads this announcement, or this reader loads its view.
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        return reader_t {*this, slot};
    }

    /**
     *  @brief The shared slot of @p shared @p identity already holds, so a nested reader keeps it, or one it claims
     *      now, probing from the slot its identity hashes to; null while every slot is held.
     *
     *  Only the thread @p identity names stores it into a slot, so finding it there is never stale, and a slot freed
     *  ahead of the one held only makes a nested reader announce twice, which is as safe.
     */
    [[nodiscard]] static epoch_slot_t* claimed_(shared_slots_t const* shared, thread_identity_t identity) noexcept {
        if (!shared) return nullptr;
        std::size_t const home = st::hash<thread_identity_t> {}(identity)&shared->mask;
        for (std::size_t step = 0; step <= shared->mask; ++step) {
            epoch_slot_t& slot = *shared->slots[(home + step) & shared->mask];
            fu::preferred_atomic_ref<thread_identity_t> const holder {slot.holder};
            thread_identity_t held = holder.load(std::memory_order_relaxed);
            if (held == identity) return &slot;
            if (held == 0 &&
                holder.compare_exchange_strong(held, identity, std::memory_order_acquire, std::memory_order_relaxed))
                return &slot;
        }
        return nullptr;
    }

    /**
     *  @brief Publishes a table twice as long as @p seen, naming its slots first, unless another thread already
     *      replaced it; answers false once no room is left for one.
     *  @note A reader still probing @p seen keeps finding every slot it names, since no slot or table is ever freed
     *      before the domain.
     */
    bool grown_(shared_slots_t const* seen) noexcept {
        fu::preferred_atomic_ref<std::int32_t> const claim {growing_};
        for (std::int32_t idle = 0;
             !claim.compare_exchange_weak(idle, 1, std::memory_order_acquire, std::memory_order_relaxed); idle = 0)
            fu::preferred_yield_t {}(growing_, idle, std::size_t {0});
        bool grown = shared_ != seen;
        if (!grown) grown = append_(seen ? seen->mask + 1 : 0);
        claim.store(0, std::memory_order_release);
        return grown;
    }

    /** Publishes a table of the @p count slots the current one names and as many new ones, at least one, under the
     *  growing claim. */
    bool append_(std::size_t count) noexcept {
        std::size_t const added = count != 0 ? count : 1;
        std::unique_ptr<epoch_slot_t[]> block {new (std::nothrow) epoch_slot_t[added]};
        std::unique_ptr<shared_slots_t> table {new (std::nothrow) shared_slots_t {}};
        if (!block || !table || blocks_.reserve(blocks_.size() + 1) != st::status_t::success_k ||
            tables_.reserve(tables_.size() + 1) != st::status_t::success_k)
            return false;
        table->slots.reset(new (std::nothrow) epoch_slot_t*[count + added]);
        if (!table->slots) return false;
        table->mask = count + added - 1;
        for (std::size_t index = 0; index != count; ++index) table->slots[index] = shared_->slots[index];
        for (std::size_t index = 0; index != added; ++index) table->slots[count + index] = &block[index];
        shared_slots_t* const published = table.get();
        [[maybe_unused]] st::status_t const kept_block = blocks_.push_back(st::assume_reserved, std::move(block));
        [[maybe_unused]] st::status_t const kept_table = tables_.push_back(st::assume_reserved, std::move(table));
        fu::preferred_atomic_ref<shared_slots_t*> {shared_}.store(published, std::memory_order_release);
        return true;
    }

    /** Calls @p visitor with every slot a reader may announce on, the spilled one aside. */
    template <typename visitor_type_>
    void for_each_slot_(visitor_type_&& visitor) noexcept {
        for (std::size_t index = 0; index != executor_threads_; ++index) visitor(executor_slots_[index]);
        shared_slots_t const* const shared = load_(shared_);
        for (std::size_t index = 0; shared && index <= shared->mask; ++index) visitor(*shared->slots[index]);
    }

    /** The epoch readers announce, moved on by every writer that waits for them, never zero. */
    alignas(fu::default_alignment_k) std::uint64_t epoch_ = 1;

    /** The shared slots threads outside the executor claim, replaced whole once every one is held. */
    alignas(fu::default_alignment_k) shared_slots_t* shared_ = nullptr;

    /** One while a thread replaces the shared slots. */
    std::int32_t growing_ = 0;

    /** Every table of shared slots ever published, kept while a reader may still probe one, touched under
     *  @c growing_. */
    st::basic_vector<std::unique_ptr<shared_slots_t>> tables_;

    /** Every block of shared slots, touched under @c growing_. */
    st::basic_vector<std::unique_ptr<epoch_slot_t[]>> blocks_;

    /** One slot per executor thread, opened before anything enters. */
    std::unique_ptr<epoch_slot_t[]> executor_slots_;

    /** Executor threads with a slot of their own. */
    std::size_t executor_threads_ = 0;

    /** The slot threads share when no slot of their own could be allocated, counting them in @c depth. */
    epoch_slot_t spilled_;
};

} // namespace unum::ustore::blobs
