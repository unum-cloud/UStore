/**
 *  @file include/ustore/blobs/nvme_collection.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief One collection: a write buffer and a value cache in front of an LSM tree of sorted runs, the snapshots
 *      every read runs against, and the ordered walk over any value layout.
 *
 *  Three places can answer a key, and they are consulted newest first: the write buffer holds what has
 *  not been flushed, the value cache holds what a point read already paid for, and the tree holds the
 *  rest. Only the third moves bytes, and it moves them through one snapshot of the level set, so a
 *  batched select reaches the queue pair's depth rather than the size of whatever batch it was handed.
 *  Nothing here allocates an I/O buffer. A caller's span is never registered memory, so the bytes are
 *  copied out of the channel's own blocks either way, and that one copy is the whole of the read path.
 *
 *  A value cache hit is served as a view of the bytes the cache already owns rather than as a copy into a buffer
 *  the caller had to size first, so a batched read that finds a key there costs one hash probe and no
 *  allocation at all. The bound is a count rather than a byte total, derived once from the budget and
 *  the width the collection's values take.
 *
 *  Taking a snapshot copies no run and moves no byte: it counts a reader into the live level set, which
 *  a later publication replaces beside rather than in place, so the runs a snapshot names stay
 *  readable until it is let go. What a snapshot cannot see is the write buffer, which is why a caller
 *  taking one flushes first. Every read of an LSM tree lives in the snapshot and nowhere else.
 *
 *  A run answers a key slot by sorted position, so an ordered walk is a merge over one cursor per run and one
 *  over the staged writes, and nothing is materialized but the staged keys, which the write buffer's
 *  own bound already caps. Where several runs carry the same key the shallowest level wins, and inside
 *  a level that may overlap the youngest run does; the staged writes outrank every run. There is no prefetcher,
 *  and adding one would be a mistake rather than an optimization: a scan that wants bytes hands the next keys to
 *  @c collection::select, which already refills the ring as completions retire, while a second pipeline reading
 *  ahead on the same channel would poll the first one's completions and drop them.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::min`, `std::sort`
#include <array>     // `std::array`
#include <atomic>    // `std::memory_order_acquire`
#include <bit>       // `std::bit_floor`
#include <limits>    // `std::numeric_limits`
#include <span>      // `std::span`
#include <utility>   // `std::exchange`, `std::move`, `std::swap`

#include <forkunion/atomics.hpp>      // `fu::preferred_atomic_ref`
#include <forkunion/capabilities.hpp> // `fu::preferred_yield_t`

#include <smashtable/basic_hash_table.hpp> // `st::hash_map`, `st::assume_reserved`
#include <smashtable/basic_ring.hpp>       // `st::basic_ring`, `st::ring_eviction_t`
#include <smashtable/basic_vector.hpp>     // `st::basic_vector`
#include <smashtable/shared.hpp>           // `st::commit_order_t`, `st::latest_snapshot_k`

#include "ustore/blobs/nvme_key_slot.hpp"     // `limit`, `stored_value_size_t`, `value_layout_t`
#include "ustore/blobs/nvme_tree.hpp"         // `lsm_tree`, `lsm_tree_position_t`, `collection_config_t`
#include "ustore/utilities/nvme_channels.hpp" // `io_channel_t`
#include "ustore/utilities/nvme_ring.hpp"     // `resource_constraint_t`, `constrained_allocator`
#include "ustore/utilities/types.hpp"         // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::blobs {

using utilities::constrained_allocator;
using utilities::constrained_buffer_t;
using utilities::resource_constraint_t;

#pragma region Stamps

class commit_ledger_t;

/** Whether a pin holds history for a reader, or holds the durable horizon below a write buffer's oldest write. */
enum class pin_kind_t : std::uint8_t {

    /** A transaction or a snapshot, which reads at the stamp it holds. */
    reader_k,

    /** A write buffer, whose writes above the stamp it holds are not in any run yet. */
    staging_k,
};

/**
 *  @brief One stamp a reader or a write buffer holds against a ledger, which keeps the history it needs.
 *
 *  Linked into its ledger while held, so taking one allocates nothing and cannot fail. A held pin moves by
 *  relinking under the ledger's claim, and it gives its stamp back when it dies.
 */
class stamp_pin_t {
  public:
    stamp_pin_t() noexcept = default;
    stamp_pin_t(stamp_pin_t const&) = delete;
    stamp_pin_t& operator=(stamp_pin_t const&) = delete;
    inline stamp_pin_t(stamp_pin_t&& other) noexcept;
    inline stamp_pin_t& operator=(stamp_pin_t&& other) noexcept;
    inline ~stamp_pin_t() noexcept;

    /** Whether this pin holds a stamp. */
    [[nodiscard]] bool held() const noexcept { return ledger_ != nullptr; }

    /** The stamp held, meaningless while the pin holds none. */
    [[nodiscard]] transaction_stamp_t stamp() const noexcept { return stamp_; }

    /** Gives the stamp back, which a pin holding none ignores. */
    inline void release() noexcept;

  private:
    friend class commit_ledger_t;

    /** The ledger this pin is linked into, null while it holds nothing. */
    commit_ledger_t* ledger_ = nullptr;

    /** The neighbours in the ledger's list of pins of the same kind, or this pin itself as a list's head. */
    stamp_pin_t* previous_ = this;
    stamp_pin_t* next_ = this;

    /** The stamp held. */
    transaction_stamp_t stamp_ = unwritten_stamp_k;

    /** Which list of the ledger holds this pin. */
    pin_kind_t kind_ = pin_kind_t::reader_k;

    /** A reader's place among the commit order's readers, which holds its low-water mark at or below the stamp. */
    st::commit_order_t::snapshot_claim_t claim_;
};

/**
 *  @brief The commit order one database draws its stamps from, and every stamp its readers and write buffers pin.
 *
 *  Several databases share one ledger when one commit spans them all, as a graph's two databases do. Pins are
 *  intrusive, so a reader or a write buffer pins without allocating, and the ledger answers three questions from
 *  them: which stamps a flush or a compaction keeps a version for, the floor stamps below which no reader can
 *  tell apart, and the durable stamp a catalog save may promise.
 */
class commit_ledger_t {
  public:
    using order_t = st::commit_order_t;

    /** One commit between drawing its stamp and landing it. */
    using commit_in_flight_t = typename order_t::commit_in_flight_t;

    commit_ledger_t() noexcept { staging_.kind_ = pin_kind_t::staging_k; }
    commit_ledger_t(commit_ledger_t const&) = delete;
    commit_ledger_t& operator=(commit_ledger_t const&) = delete;
    ~commit_ledger_t() noexcept {
        assert(readers_.next_ == &readers_ && staging_.next_ == &staging_ && "Pins outlive their ledger");
    }

    /** The newest stamp every commit at or below which has landed, which a fresh read answers at. */
    [[nodiscard]] transaction_stamp_t published() const noexcept {
        return static_cast<transaction_stamp_t>(order_.published_stamp());
    }

    /** Draws the stamp @p commit lands under, which its caller does only once its room is reserved. */
    transaction_stamp_t begin_commit(commit_in_flight_t& commit) noexcept {
        order_.begin_commit(commit);
        return static_cast<transaction_stamp_t>(commit.stamp());
    }

    /** Publishes @p commit once every write of it has landed. */
    void end_commit(commit_in_flight_t& commit) noexcept { order_.end_commit(commit); }

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
    transaction_stamp_t pin_reader(stamp_pin_t& pin) noexcept {
        pin.release();
        guard_t const held {claim_};
        pin.stamp_ = static_cast<transaction_stamp_t>(order_.take_snapshot(pin.claim_));
        pin.kind_ = pin_kind_t::reader_k;
        link_(readers_, pin);
        if (newest_reader_ < pin.stamp_) store_(newest_reader_, pin.stamp_);
        return pin.stamp_;
    }

    /** Pins @p pin at @p stamp for a write buffer whose writes above it are in no run, giving back what it held. */
    void pin_staging(stamp_pin_t& pin, transaction_stamp_t stamp) noexcept {
        pin.release();
        guard_t const held {claim_};
        pin.stamp_ = stamp;
        pin.kind_ = pin_kind_t::staging_k;
        link_(staging_, pin);
        store_(staging_count_, staging_count_ + 1);
    }

    /**
     *  @brief Whether a reader that has not been refused, or a write buffer other than the one @p own pins, pins
     *      a stamp at or above @p low and below @p high.
     *
     *  A version superseded at @p high and written at @p low is read by exactly those readers, and a mount falls
     *  back to it exactly when it promises a stamp between, which no pin taken from here on can do once @p high
     *  is published.
     */
    [[nodiscard]] bool pinned_between(transaction_stamp_t low, transaction_stamp_t high,
                                      stamp_pin_t const& own) const noexcept {
        transaction_stamp_t const refused = refused_below();
        guard_t const held {claim_};
        for (stamp_pin_t const* pin = readers_.next_; pin != &readers_; pin = pin->next_)
            if (pin->stamp_ >= refused && pin->stamp_ >= low && pin->stamp_ < high) return true;
        for (stamp_pin_t const* pin = staging_.next_; pin != &staging_; pin = pin->next_)
            if (pin != &own && pin->stamp_ >= low && pin->stamp_ < high) return true;
        return false;
    }

    /** The newest stamp any reader pins, or @c unwritten_stamp_k while none does. */
    [[nodiscard]] transaction_stamp_t newest_reader() const noexcept { return load_(newest_reader_); }

    /** How many write buffers hold writes no run carries yet. */
    [[nodiscard]] std::size_t staging_pins() const noexcept { return load_(staging_count_); }

    /** The oldest stamp a reader may still read at, so versions at or below it are indistinguishable to every one. */
    [[nodiscard]] transaction_stamp_t low_water_mark() const noexcept {
        return static_cast<transaction_stamp_t>(order_.low_water_mark());
    }

    /** The newest stamp every commit at or below which is wholly in runs, which a catalog naming those runs may
     *  promise: the oldest pin of a write buffer, or the published stamp while no buffer holds a write. */
    [[nodiscard]] transaction_stamp_t durable_stamp() const noexcept { return horizon_besides(staging_); }

    /** The stamp below which no reader and no mount can tell two stamps apart, which a run's column counts from. */
    [[nodiscard]] transaction_stamp_t floor() const noexcept { return std::min(low_water_mark(), durable_stamp()); }

    /** As @c floor, ignoring the write buffer @p own pins, which a flush of that buffer is about to empty. */
    [[nodiscard]] transaction_stamp_t floor_besides(stamp_pin_t const& own) const noexcept {
        return std::min(low_water_mark(), horizon_besides(own));
    }

    /** As @c durable_stamp, ignoring the write buffer @p own pins, which a flush of that buffer is about to empty. */
    [[nodiscard]] transaction_stamp_t horizon_besides(stamp_pin_t const& own) const noexcept {
        guard_t const held {claim_};
        transaction_stamp_t horizon = published();
        for (stamp_pin_t const* pin = staging_.next_; pin != &staging_; pin = pin->next_)
            if (pin != &own && pin->stamp_ < horizon) horizon = pin->stamp_;
        return horizon;
    }

    /** Every stamp a reader that has not been refused pins, ascending and distinct. */
    status_t pinned_readers(st::basic_vector<transaction_stamp_t>& into) const noexcept {
        into.clear();
        transaction_stamp_t const refused = refused_below();
        {
            guard_t const held {claim_};
            for (stamp_pin_t const* pin = readers_.next_; pin != &readers_; pin = pin->next_)
                if (pin->stamp_ >= refused &&
                    into.push_back(transaction_stamp_t {pin->stamp_}) != st::status_t::success_k)
                    return {status_code_t::out_of_memory_k, "No room to list the stamps readers pin"};
        }
        std::sort(into.begin(), into.end());
        std::size_t kept = 0;
        for (std::size_t index = 0; index != into.size(); ++index)
            if (kept == 0 || into[kept - 1] != into[index]) into[kept++] = into[index];
        while (into.size() != kept) into.pop_back();
        return {};
    }

    /** The stamp below which a reader is refused, since the history it needs outgrew the memory budget. */
    [[nodiscard]] transaction_stamp_t refused_below() const noexcept { return load_(refused_below_); }

    /** Refuses every reader pinned below @p stamp from here on, which only ever rises. */
    void refuse_below(transaction_stamp_t stamp) noexcept {
        fu::preferred_atomic_ref<transaction_stamp_t> const word {refused_below_};
        transaction_stamp_t seen = word.load(std::memory_order_relaxed);
        while (seen < stamp &&
               !word.compare_exchange_weak(seen, stamp, std::memory_order_release, std::memory_order_relaxed)) {}
    }

  private:
    friend class stamp_pin_t;

    /** Holds the pin lists for the life of the guard. */
    class guard_t {
      public:
        explicit guard_t(std::int32_t& claim) noexcept : claim_(claim) {
            fu::preferred_atomic_ref<std::int32_t> const word {claim_};
            std::int32_t expected_free = 0;
            while (
                !word.compare_exchange_weak(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                fu::preferred_yield_t {}(claim_, expected_free, std::size_t {0});
                expected_free = 0;
            }
        }
        guard_t(guard_t const&) = delete;
        guard_t& operator=(guard_t const&) = delete;
        ~guard_t() noexcept { fu::preferred_atomic_ref<std::int32_t> {claim_}.store(0, std::memory_order_release); }

      private:
        std::int32_t& claim_;
    };

    template <typename word_type_>
    static word_type_ load_(word_type_ const& word) noexcept {
        return fu::preferred_atomic_ref<word_type_ const> {word}.load(std::memory_order_acquire);
    }

    template <typename word_type_>
    static void store_(word_type_& word, word_type_ value) noexcept {
        fu::preferred_atomic_ref<word_type_> {word}.store(value, std::memory_order_release);
    }

    /** Appends @p pin to the list @p head starts, under the claim. */
    void link_(stamp_pin_t& head, stamp_pin_t& pin) noexcept {
        pin.ledger_ = this;
        pin.previous_ = head.previous_;
        pin.next_ = &head;
        head.previous_->next_ = &pin;
        head.previous_ = &pin;
    }

    /** Unlinks @p pin, and answers the claim it held so the caller retires it outside the claim. */
    void unlink_(stamp_pin_t& pin) noexcept {
        guard_t const held {claim_};
        pin.previous_->next_ = pin.next_;
        pin.next_->previous_ = pin.previous_;
        pin.previous_ = pin.next_ = &pin;
        pin.ledger_ = nullptr;
        if (pin.kind_ == pin_kind_t::staging_k) {
            store_(staging_count_, staging_count_ - 1);
            return;
        }
        transaction_stamp_t newest = unwritten_stamp_k;
        for (stamp_pin_t const* other = readers_.next_; other != &readers_; other = other->next_)
            if (newest < other->stamp_) newest = other->stamp_;
        store_(newest_reader_, newest);
    }

    /** Moves @p from's place in its list to @p into, under the claim. */
    void relink_(stamp_pin_t& from, stamp_pin_t& into) noexcept {
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
    stamp_pin_t readers_;

    /** The head of the list of write buffers' pins. */
    stamp_pin_t staging_;

    /** Held by whoever links, unlinks or walks a pin list. */
    std::int32_t mutable claim_ = 0;

    /** Write buffers pinned right now. */
    std::size_t staging_count_ = 0;

    /** The newest stamp any reader pins. */
    transaction_stamp_t newest_reader_ = unwritten_stamp_k;

    /** The stamp below which a reader is refused. */
    transaction_stamp_t refused_below_ = unwritten_stamp_k;
};

inline stamp_pin_t::stamp_pin_t(stamp_pin_t&& other) noexcept {
    if (other.ledger_) {
        other.ledger_->relink_(other, *this);
        claim_ = std::move(other.claim_);
    }
}

inline stamp_pin_t& stamp_pin_t::operator=(stamp_pin_t&& other) noexcept {
    if (this == &other) return *this;
    release();
    if (other.ledger_) {
        other.ledger_->relink_(other, *this);
        claim_ = std::move(other.claim_);
    }
    return *this;
}

inline stamp_pin_t::~stamp_pin_t() noexcept { release(); }

inline void stamp_pin_t::release() noexcept {
    if (!ledger_) return;
    ledger_->unlink_(*this);
    claim_.retire();
}

#pragma endregion Stamps

#pragma region Value Cache

#pragma region Cache

/**
 *  @brief Values of recently read keys, bounded by a count and evicted oldest first.
 *
 *  One collection owns one cache, whose bytes are charged to the budget the cache was made with. The
 *  budget is borrowed and must outlive the cache.
 */
template <typename stored_key_type_, value_layout_t value_layout_>
class value_cache {
  public:

#pragma region Types

    /** The key the cache addresses its values by. */
    using stored_key_t = stored_key_type_;

    /** The layout this cache serves, which fixes how its capacity was derived. */
    static constexpr value_layout_t value_layout_k = value_layout_;

#pragma endregion Types

#pragma region Construction

    value_cache() noexcept = default;
    value_cache(value_cache const&) = delete;
    value_cache& operator=(value_cache const&) = delete;

    value_cache(value_cache&& other) noexcept
        : constraint_(std::exchange(other.constraint_, nullptr)), cached_(std::move(other.cached_)),
          oldest_first_(std::move(other.oldest_first_)), capacity_(std::exchange(other.capacity_, 0)) {}

    value_cache& operator=(value_cache&& other) noexcept {
        if (this == &other) return *this;
        std::swap(constraint_, other.constraint_);
        std::swap(cached_, other.cached_);
        std::swap(oldest_first_, other.oldest_first_);
        std::swap(capacity_, other.capacity_);
        return *this;
    }

    /**
     *  @brief A cache holding what @p capacity_bytes of @p constraint afford of values @p value_size wide.
     *  @param[in] capacity_bytes Bytes the cache may take, where zero leaves it holding nothing.
     *  @param[in] value_size Bytes one value takes, which is its widest for a variable-width layout.
     *  @param[inout] constraint The budget the cached bytes are charged to, which must outlive the cache.
     */
    [[nodiscard]] static value_cache of(std::size_t capacity_bytes, stored_value_size_t value_size,
                                        resource_constraint_t& constraint) noexcept {
        value_cache cache;
        cache.constraint_ = &constraint;
        cache.cached_ = cached_t {stored_key_hash_t {}, st::equal_to_t {},
                                  constrained_allocator<std::byte> {constraint}};
        if (capacity_bytes == 0 || value_size == 0) return cache;

        std::size_t const bytes_per_element = bytes_per_element_k + value_size;
        std::size_t const affordable = std::min(capacity_bytes / bytes_per_element, oldest_first_t::capacity_limit_k);
        if (affordable == 0) return cache;

        // The eviction ring takes a power-of-two count of keys and refuses any other, so the budget
        // is rounded down to one rather than silently leaving the cache holding nothing.
        cache.capacity_ = std::bit_floor(affordable);
        auto oldest_first = oldest_first_t::make(cache.capacity_, constrained_allocator<stored_key_t> {constraint});
        if (!oldest_first) {
            cache.capacity_ = 0;
            return cache;
        }
        cache.oldest_first_ = std::move(*oldest_first);
        if (cache.cached_.reserve(cache.capacity_) == cached_t::reserve_result_t::failed_k) cache.capacity_ = 0;
        return cache;
    }

#pragma endregion Construction

#pragma region Lookups

    /** The entries the cache holds at once, zero when it caches nothing. */
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /** The entries the cache holds right now. */
    [[nodiscard]] std::size_t size() const noexcept { return cached_.size(); }

    /**
     *  @brief Calls @p visitor with the bytes cached under @p stored_key, which the cache still owns.
     *  @return Whether the key was cached, which is the only case the visitor is called in.
     */
    template <typename visitor_type_>
    [[nodiscard]] bool visit(stored_key_t stored_key, visitor_type_&& visitor) const noexcept {
        bool seen = false;
        cached_.probe_to_find(stored_key, [&](auto const& slot) noexcept {
            constrained_buffer_t const& buffer = slot.value();
            visitor(std::span<byte_t const> {buffer});
            seen = true;
        });
        return seen;
    }

    /** Caches @p stored_value under @p stored_key, evicting the oldest entry once the bound is reached. */
    void add(stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        if (capacity_ == 0) return;

        constrained_buffer_t buffer {constrained_allocator<std::byte> {*constraint_}};
        if (st::failed(buffer.resize(stored_value.size()))) return;
        if (!stored_value.empty()) std::memcpy(buffer.data(), stored_value.data(), stored_value.size());

        bool replaced = false;
        cached_.probe_to_find(stored_key, [&](auto& slot) noexcept {
            slot.value_ref() = std::move(buffer);
            replaced = true;
        });
        if (replaced) return;

        stored_key_t evicted {};
        if (oldest_first_.push_evicting(stored_key_t {stored_key}, evicted) == st::ring_eviction_t::evicted_oldest_k) {
            [[maybe_unused]] bool const dropped = cached_.erase(evicted);
        }
        cached_.emplace(stored_key, std::move(buffer), st::assume_reserved);
    }

    /** Drops every cached value, which a flush does because a staged write has replaced them. */
    void clear() noexcept {
        cached_.clear();
        oldest_first_.clear();
    }

#pragma endregion Lookups

  private:

#pragma region Private Types

    /** The cached values, addressed by their stored key. */
    using cached_t = st::hash_map<stored_key_t, constrained_buffer_t, stored_key_hash_t, st::equal_to_t,
                                  constrained_allocator<std::byte>>;

    /** The cached keys in the order they arrived. */
    using oldest_first_t = st::basic_ring<stored_key_t, constrained_allocator<stored_key_t>>;

#pragma endregion Private Types

#pragma region Private Members

    /** Bytes one entry takes beyond its value: the key twice, in the table and in the ring, and the buffer header. */
    static constexpr std::size_t bytes_per_element_k = 2 * sizeof(stored_key_t) + sizeof(constrained_buffer_t);

    /** The budget every cached byte is charged to, null until the cache is made. */
    resource_constraint_t* constraint_ = nullptr;

    /** The cached values, addressed by their stored key. */
    cached_t cached_;

    /** The cached keys in the order they arrived, whose oldest an insertion at the bound evicts. */
    oldest_first_t oldest_first_;

    /** The entries the cache holds at once, zero when it caches nothing. */
    std::size_t capacity_ = 0;

#pragma endregion Private Members
};

#pragma endregion Cache

#pragma endregion Value Cache

#pragma region Collection Snapshot

#pragma region Vocabulary

/** What one key of a select answered, which separates a miss from an erasure and from a truncated run. */
enum class select_result_t : std::uint8_t {

    /** Neither the write buffer nor any level carries that key. */
    nothing_k,

    /** A tombstone carries that key, so its value has been erased. */
    erased_k,

    /** Every byte the key slot claims arrived. */
    success_k,

    /** The medium moved fewer bytes than the key slot claims, so this one key failed. */
    incomplete_k,
};

#pragma endregion Vocabulary

#pragma region Snapshot

/**
 *  @brief One level set held against publication, and the point and batched reads that run against it.
 *
 *  A snapshot borrows the tree it counted itself into, so the tree must outlive it. It is move-only
 *  because the count it holds belongs to exactly one owner.
 */
template <typename key_slot_type_, typename volume_type_>
class collection_snapshot {
  public:

#pragma region Types

    /** The key slot every run of the held levels holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of the held levels lives on. */
    using volume_t = volume_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The tree this snapshot holds a level set of. */
    using tree_t = lsm_tree<key_slot_t, volume_t>;

    /** The hold itself, which keeps the level set alive for as long as this snapshot lives. */
    using levels_t = typename tree_t::snapshot_t;

    /** How the values of this collection are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so reading one needs no I/O. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

#pragma endregion Types

#pragma region Construction

    collection_snapshot() noexcept = default;
    collection_snapshot(collection_snapshot const&) = delete;
    collection_snapshot& operator=(collection_snapshot const&) = delete;

    collection_snapshot(collection_snapshot&& other) noexcept
        : tree_(std::exchange(other.tree_, nullptr)), levels_(std::move(other.levels_)),
          stamp_(std::exchange(other.stamp_, head_stamp_k)) {}

    collection_snapshot& operator=(collection_snapshot&& other) noexcept {
        if (this == &other) return *this;
        tree_ = std::exchange(other.tree_, nullptr);
        levels_ = std::move(other.levels_);
        stamp_ = std::exchange(other.stamp_, head_stamp_k);
        return *this;
    }

    /** Holds the live level set of @p tree, which must outlive the snapshot, read as a reader pinned at @p stamp. */
    [[nodiscard]] static collection_snapshot of(tree_t const& tree, transaction_stamp_t stamp = head_stamp_k) noexcept {
        collection_snapshot taken;
        taken.tree_ = &tree;
        taken.levels_ = tree.snapshot();
        taken.stamp_ = stamp;
        return taken;
    }

#pragma endregion Construction

#pragma region Shape

    /** Whether this snapshot holds a level set. */
    [[nodiscard]] bool holding() const noexcept { return tree_ != nullptr && levels_.holding(); }

    /** The level set held, which a scan walks the runs of. */
    [[nodiscard]] levels_t const& levels() const noexcept { return levels_; }

    /** Key slots every level holds together, one key counted once per run that holds it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept { return levels_.population_count(); }

    /** Bytes every run of every level takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return levels_.size_bytes(); }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether the held levels carry @p stored_key, and how many bytes its value takes.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c success_k.
     */
    [[nodiscard]] select_result_t find(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        size = 0;
        lsm_tree_position_t position;
        find_result_t const found = tree_->find(levels_, stored_key, position, stamp_);
        if (found == find_result_t::nothing_k) return select_result_t::nothing_k;
        if (found == find_result_t::stored_key_k) return select_result_t::erased_k;
        size = static_cast<stored_value_size_t>(
            levels_.level(position.level_number).value_window(position.level_position).bytes);
        return select_result_t::success_k;
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[inout] channel The channel the read is submitted and completed on, held by the calling thread.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     *  @return @c not_found_k for a miss or an erasure, @c input_output_k for a run shorter than its index claims.
     */
    status_t select(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) const noexcept {
        size = 0;
        stored_value_size_t wanted = 0;
        if (find(stored_key, wanted) != select_result_t::success_k)
            return {status_code_t::not_found_k, "No level of this snapshot carries that key"};
        if (into.size() < wanted) return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};

        std::uint64_t delivered = 0;
        auto sink = [&](std::size_t, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            if (!bytes.empty()) std::memcpy(into.data() + offset, bytes.data(), bytes.size());
            delivered += bytes.size();
        };
        std::array<stored_key_t, 1> const keys {stored_key};
        std::array<find_result_t, 1> answers {find_result_t::nothing_k};
        if (status_t const read = tree_->read_values(channel, levels_, std::span<stored_key_t const> {keys},
                                                     std::span<find_result_t> {answers}, sink, stamp_);
            read.failed())
            return read;

        // One key has no rest of a batch to keep, so a block short of its last one fails the whole call.
        if (delivered != wanted) return {status_code_t::io_k, "A run is shorter than its index claims"};
        size = wanted;
        return {};
    }

    /**
     *  @brief Reads the values of @p stored_keys, refilling the channel as completions retire.
     *  @param[inout] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *  @return Success, or the failure that stopped the batch rather than one of its keys.
     *
     *  @note A value larger than one block arrives as several calls, each carrying the bytes of the
     *      value before it, and a key whose value is present and empty is reported once with no bytes.
     */
    template <typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<select_result_t> results, sink_type_&& sink) const noexcept {
        if (results.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched select answers one result per key"};

        // The tree answers in its own vocabulary and the sink is told nothing about sizes, so the
        // bytes every key owed are counted here and compared once the read has run.
        st::basic_vector<find_result_t> answers;
        st::basic_vector<std::uint64_t> delivered;
        if (answers.resize(stored_keys.size()) != st::status_t::success_k ||
            delivered.resize(stored_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A snapshot could not track the keys of a batch"};
        for (std::size_t request = 0; request != stored_keys.size(); ++request) {
            answers[request] = find_result_t::nothing_k;
            delivered[request] = 0;
        }

        auto relay = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            delivered[request] += bytes.size();
            sink(request, offset, bytes);
        };
        status_t const read = tree_->read_values(channel, levels_, stored_keys, std::span<find_result_t> {answers},
                                                 relay, stamp_);

        std::size_t truncated = 0;
        for (std::size_t request = 0; request != stored_keys.size(); ++request) {
            if (answers[request] == find_result_t::nothing_k) {
                results[request] = select_result_t::nothing_k;
                continue;
            }
            if (answers[request] == find_result_t::stored_key_k) {
                results[request] = select_result_t::erased_k;
                continue;
            }
            stored_value_size_t wanted = 0;
            [[maybe_unused]] select_result_t const carried = find(stored_keys[request], wanted);
            if (delivered[request] == wanted) results[request] = select_result_t::success_k;
            else {
                results[request] = select_result_t::incomplete_k;
                ++truncated;
            }
        }

        // A block short of what it asked for fails its own key; a cancelled or refused one fails the batch.
        if (read.failed() && (truncated == 0 || !(read == status_code_t::io_k))) return read;
        return {};
    }

#pragma endregion Lookups

  private:

#pragma region Private Members

    /** The tree the held level set belongs to, borrowed and outliving the snapshot. */
    tree_t const* tree_ = nullptr;

    /** The hold itself, released when the snapshot dies. */
    levels_t levels_;

    /** The stamp the snapshot reads at, which sees the newest version at or below it. */
    transaction_stamp_t stamp_ = head_stamp_k;

#pragma endregion Private Members
};

#pragma endregion Snapshot

#pragma endregion Collection Snapshot

#pragma region Collection

#pragma region Collection

/**
 *  @brief A write buffer and a value cache in front of an LSM tree of sorted runs.
 *
 *  The volume, the budget and every channel are borrowed and must outlive the collection. A channel belongs
 *  to the thread holding it for as long as it holds it, so every call that can move bytes takes one
 *  rather than reaching for a shared handle.
 */
template <typename key_slot_type_, typename volume_type_>
class collection {
  public:

#pragma region Types

    /** The key slot every run of this collection holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of this collection lives on. */
    using volume_t = volume_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The tree the flushed writes of this collection live in. */
    using tree_t = lsm_tree<key_slot_t, volume_t>;

    /** One level set of that tree held against publication. */
    using snapshot_t = collection_snapshot<key_slot_t, volume_t>;

    /** The values of recently read keys this collection keeps. */
    using value_cache_t = value_cache<stored_key_t, key_slot_t::value_layout_k>;

    /** Where the sections of one flushed run sit inside its blob. */
    using sorted_run_layout_t = sorted_run_layout<key_slot_t>;

    /** How the values of this collection are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so reading one needs no I/O. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

    /** One staged write as a scan sees it, without the bytes a select would have to read. */
    struct staged_entry_t {

        /** The key the write named. */
        stored_key_t stored_key = stored_key_t {};

        /** Bytes the staged value takes, which is zero for an erasure and for an empty value. */
        stored_value_size_t size = 0;

        /** Whether the write stored a value or erased the key. */
        select_result_t state = select_result_t::nothing_k;
    };

#pragma endregion Types

#pragma region Construction

    collection() noexcept = default;
    collection(collection const&) = delete;
    collection& operator=(collection const&) = delete;
    collection(collection&&) noexcept = default;
    collection& operator=(collection&&) noexcept = default;

    /**
     *  @brief An empty collection over @p volume, whose memory is charged to @p constraint.
     *  @param[inout] volume The volume every run lands on, borrowed and outliving the collection.
     *  @param[in] config The write buffer bounds, the cache bound and the shape of the tree below.
     *  @param[inout] constraint The budget the write buffer and the cache are charged to.
     *  @param[inout] ledger The stamps the collection's writes carry and its readers pin, outliving the collection.
     */
    static expected<collection> mount(volume_t& volume, collection_config_t const& config,
                                      resource_constraint_t& constraint, commit_ledger_t& ledger) noexcept {
        sorted_run_config_t const& values = config.lsm_tree.lsm_level_0.sorted_run;
        collection mounted;
        mounted.config_ = config;
        mounted.constraint_ = &constraint;
        mounted.ledger_ = &ledger;
        mounted.value_size_ = is_fixed(value_layout_k) ? values.value_size : 0;
        mounted.value_max_size_ = is_inline(value_layout_k)
                                      ? static_cast<stored_value_size_t>(value_layout_k)
                                      : (is_fixed(value_layout_k) ? values.value_size : values.value_max_size);
        if (mounted.value_max_size_ == 0)
            return status_t {status_code_t::invalid_argument_k, "A collection admits values of some width"};

        // A staged value is located by a 32-bit offset whose two largest words name an erasure and an
        // empty value, so the write buffer can never grow to where a real offset would read as either.
        mounted.staged_bytes_limit_ = config.write_buffer_max_bytes;
        if (mounted.staged_bytes_limit_ >= empty_stored_value_offset_k)
            mounted.staged_bytes_limit_ = empty_stored_value_offset_k - 1;
        mounted.staged_count_limit_ = config.write_buffer_max_elements;
        if (mounted.staged_count_limit_ == 0)
            return status_t {status_code_t::invalid_argument_k, "A collection stages at least one write"};

        mounted.staged_ = staged_t {stored_key_hash_t {}, st::equal_to_t {},
                                    constrained_allocator<std::byte> {constraint}};
        mounted.arena_ = arena_t {constrained_allocator<byte_t> {constraint}};
        mounted.superseded_ = superseded_versions_t {constrained_allocator<superseded_t> {constraint}};
        mounted.superseded_heads_ = superseded_heads_t {stored_key_hash_t {}, st::equal_to_t {},
                                                        constrained_allocator<std::byte> {constraint}};
        mounted.cache_ = value_cache_t::of(holds_values_inline_k ? 0 : config.cache_limit, mounted.value_max_size_,
                                           constraint);

        auto [tree, mount_tree_status] = tree_t::mount(volume, config.lsm_tree);
        if (mount_tree_status.failed()) return mount_tree_status;
        mounted.tree_ = std::move(tree);
        return mounted;
    }

    /** Returns every run to the volume and forgets every staged write, which destroys the data. */
    status_t destroy(io_channel_t& channel) noexcept {
        status_t const dropped = tree_.destroy(channel);
        forget_staged_();
        cache_guard_t const held {cache_claim_};
        cache_.clear();
        return dropped;
    }

#pragma endregion Construction

#pragma region Shape

    /** The bounds this collection was mounted with. */
    [[nodiscard]] collection_config_t const& config() const noexcept { return config_; }

    /** The tree the flushed writes live in, which a database compacts and exports through. */
    [[nodiscard]] tree_t& tree() noexcept { return tree_; }

    /** The tree the flushed writes live in. */
    [[nodiscard]] tree_t const& tree() const noexcept { return tree_; }

    /** Bytes the largest value this collection admits takes. */
    [[nodiscard]] stored_value_size_t value_max_size() const noexcept { return value_max_size_; }

    /** Writes staged and not yet flushed. */
    [[nodiscard]] std::size_t staged_count() const noexcept { return staged_.size(); }

    /** Whether nothing is staged and no level holds a key. */
    [[nodiscard]] bool empty() const noexcept { return staged_.empty() && tree_.snapshot().population_count() == 0; }

    /** Keys staged plus key slots every level holds, one key counted once per run that holds it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept {
        return staged_.size() + tree_.snapshot().population_count();
    }

    /** Bytes every run of every level takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return tree_.snapshot().size_bytes(); }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether anything carries @p stored_key, and how many bytes its value takes.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c success_k.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     */
    [[nodiscard]] select_result_t find(stored_key_t stored_key, stored_value_size_t& size,
                                       transaction_stamp_t stamp = head_stamp_k) const noexcept {
        size = 0;
        staged_place_t staged;
        if (staged_place_(stored_key, stamp, staged)) {
            if (staged.erased()) return select_result_t::erased_k;
            size = staged.size;
            return select_result_t::success_k;
        }
        return snapshot_t::of(tree_, stamp).find(stored_key, size);
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[inout] channel The channel the read is submitted and completed on, held by the calling thread.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     *  @return @c not_found_k for a miss or an erasure, @c input_output_k for a run shorter than its index claims.
     */
    status_t select(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into, stored_value_size_t& size,
                    transaction_stamp_t stamp = head_stamp_k) const noexcept {
        size = 0;
        staged_place_t staged;
        if (staged_place_(stored_key, stamp, staged)) {
            if (staged.erased()) return {status_code_t::not_found_k, "A staged write erased that key"};
            if (into.size() < staged.size)
                return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};
            std::span<byte_t const> const bytes = staged_bytes_(staged);
            if (!bytes.empty()) std::memcpy(into.data(), bytes.data(), bytes.size());
            size = staged.size;
            return {};
        }

        // The cache holds the newest flushed version, which only a reader at or past every stamp may take.
        bool const current = stamp >= newest_stamp_;
        if constexpr (!holds_values_inline_k) {
            bool refused = false;
            cache_guard_t const held {cache_claim_};
            bool const cached = current && cache_.visit(stored_key, [&](std::span<byte_t const> bytes) noexcept {
                if (into.size() < bytes.size()) {
                    refused = true;
                    return;
                }
                if (!bytes.empty()) std::memcpy(into.data(), bytes.data(), bytes.size());
                size = static_cast<stored_value_size_t>(bytes.size());
            });
            if (refused) return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};
            if (cached) return {};
        }

        if (status_t const read = snapshot_t::of(tree_, stamp).select(channel, stored_key, into, size); read.failed())
            return read;
        if constexpr (!holds_values_inline_k) {
            if (!current) return {};
            cache_guard_t const held {cache_claim_};
            cache_.add(stored_key, std::span<byte_t const> {into.data(), size});
        }
        return {};
    }

    /**
     *  @brief Reads the values of @p stored_keys, refilling the channel as completions retire.
     *  @param[inout] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     *  @return Success, or the failure that stopped the batch rather than one of its keys.
     *
     *  @note A value larger than one block arrives as several calls, each carrying the bytes of the
     *      value before it, and a key whose value is present and empty is reported once with no bytes.
     *  @note The bytes a batched read moves are never cached, since caching one would mean staging the
     *      whole value in a buffer this path exists to do without.
     */
    template <typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<select_result_t> results, sink_type_&& sink,
                    transaction_stamp_t stamp = head_stamp_k) const noexcept {
        if (results.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched select answers one result per key"};
        bool const current = stamp >= newest_stamp_;

        // The tree takes its keys contiguous, so the request each deferred key came from travels beside it.
        deferred_keys_t deferred_keys {constrained_allocator<stored_key_t> {*constraint_}};
        deferred_requests_t deferred_requests {constrained_allocator<std::size_t> {*constraint_}};
        if (deferred_keys.reserve(stored_keys.size()) != st::status_t::success_k ||
            deferred_requests.reserve(stored_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A collection could not track the keys of a batch"};

        for (std::size_t request = 0; request != stored_keys.size(); ++request) {
            staged_place_t staged;
            if (staged_place_(stored_keys[request], stamp, staged)) {
                if (staged.erased()) results[request] = select_result_t::erased_k;
                else {
                    results[request] = select_result_t::success_k;
                    sink(request, std::uint64_t {0}, staged_bytes_(staged));
                }
                continue;
            }
            if constexpr (!holds_values_inline_k) {
                cache_guard_t const held {cache_claim_};
                bool const cached = current &&
                                    cache_.visit(stored_keys[request], [&](std::span<byte_t const> bytes) noexcept {
                                        sink(request, std::uint64_t {0}, bytes);
                                    });
                if (cached) {
                    results[request] = select_result_t::success_k;
                    continue;
                }
            }
            results[request] = select_result_t::nothing_k;
            [[maybe_unused]] st::status_t const named = deferred_keys.push_back(stored_key_t {stored_keys[request]});
            [[maybe_unused]] st::status_t const tracked = deferred_requests.push_back(std::size_t {request});
        }
        if (deferred_keys.empty()) return {};

        deferred_results_t deferred_results {constrained_allocator<select_result_t> {*constraint_}};
        if (deferred_results.resize(deferred_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A collection could not track the keys of a batch"};

        auto relay = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            sink(deferred_requests[request], offset, bytes);
        };
        if (status_t const read = snapshot_t::of(tree_, stamp)
                                      .select(channel, std::span<stored_key_t const> {deferred_keys},
                                              std::span<select_result_t> {deferred_results}, relay);
            read.failed())
            return read;
        for (std::size_t index = 0; index != deferred_results.size(); ++index)
            results[deferred_requests[index]] = deferred_results[index];
        return {};
    }

#pragma endregion Lookups

#pragma region Writes

    /** Whether a value of @p size bytes is one this collection's layout can hold, which @c stage assumes. */
    [[nodiscard]] bool admits(std::size_t size) const noexcept { return admits_(size); }

    /**
     *  @brief Makes room for @p count more writes of @p bytes together, flushing first when they do not fit.
     *  @return Success, after which staging that many writes cannot fail, or the status that refused the room.
     *
     *  @note A commit prepares every collection it writes before it draws its stamp, so a refusal leaves
     *      nothing applied and nothing published.
     */
    status_t prepare(io_channel_t& channel, std::size_t count, std::uint64_t bytes) noexcept {
        if (status_t const room = make_room_(channel, count, bytes); room.failed()) return room;
        std::uint64_t const wanted = arena_used_ + bytes;
        if (arena_.size() < wanted && arena_.resize(static_cast<std::size_t>(wanted)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A write buffer could not hold another value"};
        if (staged_.reserve(staged_.size() + count) == staged_t::reserve_result_t::failed_k)
            return {status_code_t::out_of_memory_k, "A write buffer could not name another key"};

        // A replaced version is kept only in room taken here; when the budget refuses it, the buffer flushes, which
        // leaves nothing older to replace, and the readers that could still have read one are refused instead.
        superseded_room_ = 0;
        if (superseded_.reserve(superseded_.size() + count) == st::status_t::success_k &&
            superseded_heads_.reserve(superseded_heads_.size() + count) !=
                superseded_heads_t::reserve_result_t::failed_k) {
            superseded_room_ = count;
            return {};
        }
        ledger_->refuse_below(ledger_->published() + 1);
        return flush(channel);
    }

    /** Stages @p stored_value under @p stored_key as written by the commit at @p stamp, in room @c prepare took. */
    void stage(stored_key_t stored_key, std::span<byte_t const> stored_value, transaction_stamp_t stamp) noexcept {
        staged_place_t place;
        place.offset = static_cast<stored_value_offset_t>(arena_used_);
        place.size = static_cast<stored_value_size_t>(stored_value.size());
        place.stamp = stamp;
        if (!stored_value.empty()) std::memcpy(arena_.data() + arena_used_, stored_value.data(), stored_value.size());
        arena_used_ += stored_value.size();
        stage_(stored_key, place);
    }

    /** Stages a tombstone under @p stored_key as written by the commit at @p stamp, in room @c prepare took. */
    void stage_erasure(stored_key_t stored_key, transaction_stamp_t stamp) noexcept {
        staged_place_t place;
        place.offset = tombstone_offset_k;
        place.stamp = stamp;
        stage_(stored_key, place);
    }

    /**
     *  @brief Whether a commit newer than @p from wrote @p stored_key, which only the newest write of a key decides.
     *
     *  Answered from the write buffer first, then from the runs newest first, skipping every run whose newest stamp
     *  is not past @p from, since no version it holds can be.
     */
    [[nodiscard]] bool written_since(stored_key_t stored_key, transaction_stamp_t from) const noexcept {
        if (newest_stamp_ <= from) return false;
        staged_place_t staged;
        if (staged_place_(stored_key, staged)) return staged.stamp > from;
        typename tree_t::snapshot_t const taken = tree_.snapshot();
        for (std::size_t number = 0; number != taken.size(); ++number) {
            auto const& level = taken.level(number);
            if (level.order() == lsm_level_order_t::sorted_k) {
                lsm_level_position_t position;
                if (level.find(stored_key, position) == find_result_t::nothing_k) continue;
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
     *      walk over the range watches, so a key inserted or erased where it found none conflicts too.
     *
     *  Every version counts, not only the newest, since a key written twice since @p from was written since. A walk
     *  over the whole collection stops at the collection's newest stamp, and a run is skipped by its own.
     */
    [[nodiscard]] bool range_written_since(stored_key_t lower, stored_key_t upper,
                                           transaction_stamp_t from) const noexcept {
        if (newest_stamp_ <= from || upper < lower) return false;
        bool written = false;
        [[maybe_unused]] st::status_t const walked = staged_.for_each([&](auto const& slot) noexcept {
            written = !(slot.key() < lower) && !(upper < slot.key()) && slot.value().stamp > from;
            return written ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
        });
        if (written) return true;
        typename tree_t::snapshot_t const taken = tree_.snapshot();
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

    /** Opens the runs @p runs names as this collection's tree, and continues its stamps above theirs. */
    status_t restore(io_channel_t& channel, std::span<lsm_tree_run_record_t const> runs) noexcept {
        if (status_t const restored = tree_.restore(channel, runs); restored.failed()) return restored;
        newest_stamp_ = tree_.newest_stamp();
        return {};
    }

    /** Stages @p stored_value under @p stored_key at @p stamp, flushing first when the write buffer is full. */
    status_t insert(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t const> stored_value,
                    transaction_stamp_t stamp) noexcept {
        if (!admits_(stored_value.size()))
            return {status_code_t::malformed_k, "This collection admits no value of that size"};
        if (status_t const room = prepare(channel, 1, stored_value.size()); room.failed()) return room;
        stage(stored_key, stored_value, stamp);
        return {};
    }

    /** Stages a tombstone under @p stored_key at @p stamp, flushing first when the write buffer is full. */
    status_t erase(io_channel_t& channel, stored_key_t stored_key, transaction_stamp_t stamp) noexcept {
        if (status_t const room = prepare(channel, 1, 0); room.failed()) return room;
        stage_erasure(stored_key, stamp);
        return {};
    }

    /**
     *  @brief Stages one value per key at @p stamp, flushing first when the write buffer cannot hold them all.
     *  @param[inout] channel The channel a flush writes on, held by the calling thread.
     *  @param[in] stored_keys The keys to write, in any order and with repeats taking the last value.
     *  @param[in] stored_values The values back to back, in the order their keys name them.
     *  @param[in] sizes Bytes of every value, one per key, which must add up to @p stored_values.
     *  @param[in] stamp The commit every one of these writes belongs to.
     */
    status_t insert(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<byte_t const> stored_values, std::span<stored_value_size_t const> sizes,
                    transaction_stamp_t stamp) noexcept {
        if (status_t const checked = check_batch(stored_keys, stored_values, sizes); checked.failed()) return checked;
        if (status_t const room = prepare(channel, stored_keys.size(), stored_values.size()); room.failed())
            return room;
        stage(stored_keys, stored_values, sizes, stamp);
        return {};
    }

    /** Stages a tombstone under every key of @p stored_keys at @p stamp, flushing first when they do not fit. */
    status_t erase(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                   transaction_stamp_t stamp) noexcept {
        if (status_t const room = prepare(channel, stored_keys.size(), 0); room.failed()) return room;
        for (std::size_t index = 0; index != stored_keys.size(); ++index) stage_erasure(stored_keys[index], stamp);
        return {};
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

    /** Stages one value per key at @p stamp, in room @c prepare took for a batch @c check_batch accepted. */
    void stage(std::span<stored_key_t const> stored_keys, std::span<byte_t const> stored_values,
               std::span<stored_value_size_t const> sizes, transaction_stamp_t stamp) noexcept {
        std::uint64_t offset = 0;
        for (std::size_t index = 0; index != stored_keys.size(); ++index) {
            stage(stored_keys[index], stored_values.subspan(static_cast<std::size_t>(offset), sizes[index]), stamp);
            offset += sizes[index];
        }
    }

    /** The newest stamp any write of this collection carries, staged or flushed, which only ever rises. */
    [[nodiscard]] transaction_stamp_t newest_stamp() const noexcept { return newest_stamp_; }

#pragma endregion Writes

#pragma region Maintenance

    /** Writes every staged write as one run of level zero, leaving the write buffer empty. */
    status_t flush(io_channel_t& channel) noexcept {
        if (staged_.empty()) return {};
        drained_slots_t key_slots {constrained_allocator<key_slot_t> {*constraint_}};
        drained_stamps_t stamps {constrained_allocator<std::uint64_t> {*constraint_}};
        drained_values_t stored_values {constrained_allocator<byte_t> {*constraint_}};
        // A replaced version survives as a merge would keep it: for a pinned reader, or for a mount promising the
        // durable horizon this flush leaves; at or below both, every stamp reads alike, so the run packs only above.
        st::basic_vector<transaction_stamp_t> readers;
        if (status_t const listed = ledger_->pinned_readers(readers); listed.failed()) return listed;
        compaction_stamps_t survival;
        survival.pinned = readers.data();
        survival.pinned_count = readers.size();
        survival.horizon = ledger_->horizon_besides(staging_pin_);
        survival.floor = ledger_->floor_besides(staging_pin_);
        if (status_t const drained = drain_(key_slots, stamps, survival, stored_values); drained.failed())
            return drained;
        if (status_t const published = tree_.flush(channel, std::span<key_slot_t const> {key_slots},
                                                   std::span<std::uint64_t const> {stamps}, survival.floor,
                                                   std::span<byte_t const> {stored_values}, value_max_size_);
            published.failed())
            return published;
        forget_staged_();

        // A cached value the flush has just replaced would outrank the run that now carries it.
        cache_guard_t const held {cache_claim_};
        cache_.clear();
        return {};
    }

    /** Whether any level of the tree is over the bytes its number admits. */
    [[nodiscard]] bool needs_compaction() const noexcept { return tree_.needs_compaction(); }

    /**
     *  @brief Merges the shallowest level that is over its bounds into the level below it.
     *  @param[inout] reading_channel The channel the merge gathers source values on, held by the calling thread.
     *  @param[inout] writing_channel The channel the merge writes its new runs on, which is not @p reading_channel.
     *  @param[in] published What to call once the merged set is live and before a consumed run is dropped.
     */
    expected<compaction_outcome_t> compact(
        io_channel_t& reading_channel, io_channel_t& writing_channel,
        lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {}) noexcept {
        // A pinned reader keeps the version it reads, and the durable horizon keeps the version a mount falls back to.
        st::basic_vector<transaction_stamp_t> readers;
        if (status_t const listed = ledger_->pinned_readers(readers); listed.failed()) return listed;
        compaction_stamps_t stamps;
        stamps.pinned = readers.data();
        stamps.pinned_count = readers.size();
        stamps.horizon = ledger_->durable_stamp();
        stamps.floor = ledger_->floor();
        return tree_.compact(reading_channel, writing_channel, compaction_placement_t::automatic_k, nullptr, published,
                             stamps);
    }

    /** Drops every version a mount found stamped above @p ceiling, which only a collection holding no write takes. */
    status_t truncate_above(io_channel_t& reading_channel, io_channel_t& writing_channel, transaction_stamp_t ceiling,
                            lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {}) noexcept {
        if (!staged_.empty()) return {status_code_t::invalid_argument_k, "A collection truncates only its runs"};
        if (newest_stamp_ <= ceiling) return {};
        if (status_t const truncated = tree_.truncate_above(reading_channel, writing_channel, ceiling, published);
            truncated.failed())
            return truncated;
        newest_stamp_ = tree_.newest_stamp();
        return {};
    }

    /** Flushes the write buffer, then holds the level set the flush published. */
    expected<snapshot_t> take_snapshot(io_channel_t& channel) noexcept {
        if (status_t const flushed = flush(channel); flushed.failed()) return flushed;
        return snapshot_t::of(tree_);
    }

    /** Lists the newest staged version of every key at or below @p stamp in ascending order of its key, which is what
     *  a scan merges levels with; a key whose every staged version is newer is left to the levels. */
    status_t staged_in_order(st::basic_vector<staged_entry_t>& into,
                             transaction_stamp_t stamp = head_stamp_k) const noexcept {
        into.clear();
        if (into.reserve(staged_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A scan could not list the staged writes"};
        [[maybe_unused]] st::status_t const walked = staged_.for_each([&](auto const& slot) noexcept {
            staged_place_t place;
            if (!staged_place_(slot.key(), stamp, place)) return st::walk_control_t::resume_k;
            staged_entry_t entry;
            entry.stored_key = slot.key();
            entry.size = place.erased() ? 0 : place.size;
            entry.state = place.erased() ? select_result_t::erased_k : select_result_t::success_k;
            [[maybe_unused]] st::status_t const listed = into.push_back(std::move(entry));
            return st::walk_control_t::resume_k;
        });
        std::sort(into.begin(), into.end(), [](staged_entry_t const& first, staged_entry_t const& second) noexcept {
            return first.stored_key < second.stored_key;
        });
        return {};
    }

#pragma endregion Maintenance

  private:

#pragma region Private Types

    /** Where one staged write put its bytes, or that it erased the key, and the commit it belongs to. */
    struct staged_place_t {

        /** First byte of the value inside the arena, or @c tombstone_offset_k for an erasure. */
        stored_value_offset_t offset = 0;

        /** Bytes the value occupies, which is zero for an empty value and for an erasure. */
        stored_value_size_t size = 0;

        /** The stamp of the commit that wrote it. */
        transaction_stamp_t stamp = unwritten_stamp_k;

        /** Whether the write erased its key. */
        [[nodiscard]] bool erased() const noexcept { return offset == tombstone_offset_k; }
    };

    static_assert(sizeof(staged_place_t) == 16, "A staged write's slot carries its stamp in the bytes it had");

    /** The staged writes, addressed by their key, of which the last write of a key wins. */
    using staged_t =
        st::hash_map<stored_key_t, staged_place_t, stored_key_hash_t, st::equal_to_t, constrained_allocator<std::byte>>;

    /** One version a staged write replaced, with the index of the version of the same key it replaced in turn. */
    struct superseded_t {
        stored_key_t stored_key;
        staged_place_t place;
        std::uint32_t older = 0;
    };

    /** Ends a chain of replaced versions. */
    static constexpr std::uint32_t no_older_superseded_k = std::numeric_limits<std::uint32_t>::max();

    /** The replaced versions, appended in the order they were replaced. */
    using superseded_versions_t = st::basic_vector<superseded_t, constrained_allocator<superseded_t>>;

    /** The newest replaced version of each key, by its index in @c superseded_versions_t. */
    using superseded_heads_t =
        st::hash_map<stored_key_t, std::uint32_t, stored_key_hash_t, st::equal_to_t, constrained_allocator<std::byte>>;

    /** The bytes every staged value was copied into, appended to and never rewritten. */
    using arena_t = st::basic_vector<byte_t, constrained_allocator<byte_t>>;

    /** The key slots one flush writes, in ascending order of their keys. */
    using drained_slots_t = st::basic_vector<key_slot_t, constrained_allocator<key_slot_t>>;

    /** The values one flush writes, back to back in the order the key slots locate them. */
    using drained_values_t = st::basic_vector<byte_t, constrained_allocator<byte_t>>;

    /** The stamps of the key slots one flush writes, in the same order. */
    using drained_stamps_t = st::basic_vector<std::uint64_t, constrained_allocator<std::uint64_t>>;

    /** One version a flush writes, with its key, before the versions are sorted into a run's order. */
    struct drained_version_t {

        /** The key the version belongs to. */
        stored_key_t stored_key;

        /** Where its bytes sit in the arena and the stamp it carries. */
        staged_place_t place;
    };

    /** The versions one flush writes. */
    using drained_versions_t = st::basic_vector<drained_version_t, constrained_allocator<drained_version_t>>;

    /** The keys of a batch that neither the write buffer nor the cache answered. */
    using deferred_keys_t = st::basic_vector<stored_key_t, constrained_allocator<stored_key_t>>;

    /** The caller's request every deferred key came from. */
    using deferred_requests_t = st::basic_vector<std::size_t, constrained_allocator<std::size_t>>;

    /** What the tree answered for every deferred key. */
    using deferred_results_t = st::basic_vector<select_result_t, constrained_allocator<select_result_t>>;

#pragma endregion Private Types

#pragma region Private Methods

    /**
     *  @brief Whether a value of @p size bytes is one this collection's layout can hold.
     *
     *  @note An inline layout takes no empty value, since a key slot holding zero bytes is the
     *      tombstone itself and no width is left to tell the two apart.
     */
    [[nodiscard]] bool admits_(std::size_t size) const noexcept {
        if constexpr (is_fixed(value_layout_k)) return size == value_size_;
        else if constexpr (holds_values_inline_k) return size != 0 && size <= value_max_size_;
        else return size <= value_max_size_;
    }

    /** The staged write under @p stored_key, and whether one is staged at all. */
    [[nodiscard]] bool staged_place_(stored_key_t stored_key, staged_place_t& place) const noexcept {
        bool found = false;
        staged_.probe_to_find(stored_key, [&](auto const& slot) noexcept {
            place = slot.value();
            found = true;
        });
        return found;
    }

    /** Where the newest staged version of @p stored_key at or below @p stamp sits, if the write buffer holds one. */
    [[nodiscard]] bool staged_place_(stored_key_t stored_key, transaction_stamp_t stamp,
                                     staged_place_t& place) const noexcept {
        if (!staged_place_(stored_key, place)) return false;
        if (place.stamp <= stamp) return true;
        bool found = false;
        superseded_heads_.probe_to_find(stored_key, [&](auto const& slot) noexcept {
            for (std::uint32_t index = slot.value(); index != no_older_superseded_k; index = superseded_[index].older)
                if (superseded_[index].place.stamp <= stamp) {
                    place = superseded_[index].place;
                    found = true;
                    return;
                }
        });
        return found;
    }

    /** The arena bytes @p place names, which is an empty span for an empty value and for an erasure. */
    [[nodiscard]] std::span<byte_t const> staged_bytes_(staged_place_t const& place) const noexcept {
        if (place.size == 0) return {};
        return std::span<byte_t const> {arena_.data() + place.offset, place.size};
    }

    /** Whether @p count more writes of @p bytes together still fit in the write buffer, the kept replaced versions
     *  counted among its writes. */
    [[nodiscard]] bool can_stage_(std::size_t count, std::uint64_t bytes) const noexcept {
        return staged_.size() + superseded_.size() + count <= staged_count_limit_ &&
               arena_used_ + bytes <= staged_bytes_limit_;
    }

    /**
     *  @brief Flushes when @p count writes of @p bytes do not fit, and refuses only what no arena offset can locate.
     *
     *  An empty write buffer takes a commit larger than its bounds whole, past them, so a commit never lands in
     *  pieces; the next write flushes it.
     */
    status_t make_room_(io_channel_t& channel, std::size_t count, std::uint64_t bytes) noexcept {
        if (can_stage_(count, bytes)) return {};
        if (status_t const flushed = flush(channel); flushed.failed()) return flushed;
        if (bytes < empty_stored_value_offset_k) return {};
        return {status_code_t::capacity_exhausted_k, "That batch is longer than a write buffer can locate"};
    }

    /** Names @p place under @p stored_key, in room @c prepare took, keeping the version it replaces, and pins the
     *  durable horizon below the first write a fresh buffer takes. */
    void stage_(stored_key_t stored_key, staged_place_t place) noexcept {
        if (staged_.empty()) ledger_->pin_staging(staging_pin_, place.stamp - 1);
        auto insert = [&](auto& unused_slot) noexcept {
            new (&unused_slot.key_ref()) stored_key_t {stored_key};
            new (&unused_slot.value_ref()) staged_place_t {place};
        };
        auto update = [&](auto& equal_slot) noexcept {
            staged_place_t const replaced = equal_slot.value();
            if (replaced.stamp != place.stamp && superseded_room_ != 0) supersede_(stored_key, replaced);
            equal_slot.value_ref() = place;
        };
        [[maybe_unused]] auto const stored = staged_.probe_to_upsert(stored_key, insert, update, st::assume_reserved);
        if (newest_stamp_ < place.stamp) newest_stamp_ = place.stamp;
    }

    /**
     *  @brief Keeps @p replaced as the newest of the replaced versions of @p stored_key, in room @c prepare took.
     *
     *  A version is kept while the one replacing it is unpublished, since a reader pinning now reads it. Once that is
     *  published, a version is read only by a pin between the two, and none taken later lands there, so the newest
     *  kept version is overwritten in place when no pin needs it, which bounds a hot key's chain while no one reads.
     *  A mount needs the same versions: another buffer's pin between the two is a commit a crash may drop.
     */
    void supersede_(stored_key_t stored_key, staged_place_t const& replaced) noexcept {
        std::uint32_t const appended = static_cast<std::uint32_t>(superseded_.size());
        std::uint32_t older = no_older_superseded_k;
        bool overwritten = false;
        auto insert = [&](auto& unused_slot) noexcept {
            new (&unused_slot.key_ref()) stored_key_t {stored_key};
            new (&unused_slot.value_ref()) std::uint32_t {appended};
        };
        auto update = [&](auto& equal_slot) noexcept {
            superseded_t& newest = superseded_[equal_slot.value()];
            if (replaced.stamp <= ledger_->published() &&
                !ledger_->pinned_between(newest.place.stamp, replaced.stamp, staging_pin_)) {
                newest.place = replaced;
                overwritten = true;
                return;
            }
            older = equal_slot.value();
            equal_slot.value_ref() = appended;
        };
        [[maybe_unused]] auto const linked = superseded_heads_.probe_to_upsert(stored_key, insert, update,
                                                                               st::assume_reserved);
        if (overwritten) return;
        [[maybe_unused]] st::status_t const kept = superseded_.push_back(superseded_t {stored_key, replaced, older});
        --superseded_room_;
    }

    /** Empties the write buffer, which both a flush and a destruction do, and lets its pin go. */
    void forget_staged_() noexcept {
        staged_.clear();
        superseded_.clear();
        superseded_heads_.clear();
        superseded_room_ = 0;
        arena_used_ = 0;
        staging_pin_.release();
    }

    /** Holds @c cache_ for the life of the guard, waiting while another reader visits or fills it. */
    class cache_guard_t {
      public:
        explicit cache_guard_t(std::int32_t& claim) noexcept : claim_(claim) {
            fu::preferred_atomic_ref<std::int32_t> const word {claim_};
            std::int32_t expected_free = 0;
            while (
                !word.compare_exchange_weak(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                fu::preferred_yield_t {}(claim_, expected_free, std::size_t {0});
                expected_free = 0;
            }
        }
        cache_guard_t(cache_guard_t const&) = delete;
        cache_guard_t& operator=(cache_guard_t const&) = delete;
        ~cache_guard_t() noexcept {
            fu::preferred_atomic_ref<std::int32_t> {claim_}.store(0, std::memory_order_release);
        }

      private:
        std::int32_t& claim_;
    };

    /**
     *  @brief Lays the staged writes out as one run's key slots and the bytes they locate.
     *  @param[out] key_slots The key slots in ascending order of their keys, as a run takes them.
     *  @param[out] stamps The stamp of every key slot, the versions of one key newest first.
     *  @param[in] survival Which replaced versions a reader or a mount still needs, as a merge would decide.
     *  @param[out] stored_values The values back to back, empty for a layout holding them inline.
     *
     *  @note Each slot carries its arena offset until the values are packed, so the order is found
     *      once by sorting the slots themselves and no permutation is ever built.
     */
    status_t drain_(drained_slots_t& key_slots, drained_stamps_t& stamps, compaction_stamps_t const& survival,
                    drained_values_t& stored_values) const noexcept {
        std::size_t const count = staged_.size() + superseded_.size();
        drained_versions_t versions {constrained_allocator<drained_version_t> {*constraint_}};
        if (versions.reserve(count) != st::status_t::success_k || key_slots.reserve(count) != st::status_t::success_k ||
            stamps.reserve(count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A flush could not hold its key slots"};
        [[maybe_unused]] st::status_t const walked = staged_.for_each([&](auto const& slot) noexcept {
            [[maybe_unused]] st::status_t const placed = versions.push_back(
                drained_version_t {slot.key(), slot.value()});
            return st::walk_control_t::resume_k;
        });
        for (superseded_t const& replaced : superseded_) [[maybe_unused]]
            st::status_t const placed = versions.push_back(drained_version_t {replaced.stored_key, replaced.place});
        std::sort(versions.begin(), versions.end(),
                  [](drained_version_t const& first, drained_version_t const& second) {
                      if (first.stored_key < second.stored_key) return true;
                      if (second.stored_key < first.stored_key) return false;
                      return first.place.stamp > second.place.stamp;
                  });

        for (std::size_t index = 0; index != versions.size(); ++index) {
            staged_place_t const& place = versions[index].place;
            bool const shadowed = index != 0 && versions[index - 1].stored_key == versions[index].stored_key &&
                                  !keeps_older_version(survival, versions[index - 1].place.stamp, place.stamp);
            if (shadowed) continue;
            key_slot_t built {};
            built.stored_key = versions[index].stored_key;
            if constexpr (holds_values_inline_k) {
                if (!place.erased()) {
                    built.size = place.size;
                    if (place.size != 0) std::memcpy(built.data, arena_.data() + place.offset, place.size);
                }
            }
            else {
                if (place.erased()) built.make_tombstone();
                else {
                    built.size = place.size;
                    built.location = static_cast<stored_value_offset_t>(place.offset);
                }
            }
            [[maybe_unused]] st::status_t const placed = key_slots.push_back(std::move(built));
            [[maybe_unused]] st::status_t const stamped = stamps.push_back(std::uint64_t {place.stamp});
        }

        if constexpr (!holds_values_inline_k) {
            std::uint64_t total = 0;
            for (std::size_t index = 0; index != key_slots.size(); ++index)
                if (!key_slots[index].is_tombstone()) total += key_slots[index].size;
            if (stored_values.resize(static_cast<std::size_t>(total)) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A flush could not hold its values"};

            std::uint64_t location = sorted_run_layout_t::values_offset(key_slots.size());
            std::uint64_t written = 0;
            for (std::size_t index = 0; index != key_slots.size(); ++index) {
                key_slot_t& built = key_slots[index];
                if (built.is_tombstone()) continue;
                if (built.size == 0) {
                    built.location = empty_stored_value_offset_k;
                    continue;
                }
                std::memcpy(stored_values.data() + written, arena_.data() + built.location, built.size);
                built.location = static_cast<stored_value_offset_t>(location);
                location += built.size;
                written += built.size;
            }
        }
        return {};
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The bounds this collection was mounted with. */
    collection_config_t config_ {};

    /** The budget the write buffer and the cache are charged to, null until the collection is mounted. */
    resource_constraint_t* constraint_ = nullptr;

    /** The tree the flushed writes live in. */
    tree_t tree_;

    /** The staged writes, addressed by their key. */
    staged_t staged_;

    /** The bytes every staged value was copied into. */
    arena_t arena_;

    /** Bytes of the arena the staged writes have taken. */
    std::uint64_t arena_used_ = 0;

    /** The newest stamp any write of this collection carries, staged or flushed. */
    transaction_stamp_t newest_stamp_ = unwritten_stamp_k;

    /** The stamps this collection's writes carry and its readers pin, borrowed and outliving the collection. */
    commit_ledger_t* ledger_ = nullptr;

    /** Holds the durable horizon below the oldest commit the write buffer holds, linked while it holds any. */
    stamp_pin_t staging_pin_;

    /** The versions staged writes replaced while a reader or a mount could still need them, oldest first. */
    superseded_versions_t superseded_;

    /** The index in @c superseded_ of each key's newest replaced version. */
    superseded_heads_t superseded_heads_;

    /** How many more replaced versions the room @c prepare took still holds. */
    std::size_t superseded_room_ = 0;

    /** The values of recently read keys, filled by point reads and emptied by a flush. */
    value_cache_t mutable cache_;

    /** Held by whoever visits, fills or empties @c cache_, since readers sharing the collection fill it too. */
    std::int32_t mutable cache_claim_ = 0;

    /** Bytes every value takes, zero for a layout whose values differ in width. */
    stored_value_size_t value_size_ = 0;

    /** Bytes the largest value this collection admits takes. */
    stored_value_size_t value_max_size_ = 0;

    /** Writes the write buffer holds at once. */
    std::size_t staged_count_limit_ = 0;

    /** Bytes the write buffer holds at once. */
    std::uint64_t staged_bytes_limit_ = 0;

#pragma endregion Private Members
};

#pragma endregion Collection

#pragma endregion Collection

#pragma region Collection Scanner

#pragma region Scanner

/**
 *  @brief Every key of one collection in ascending order, each with what it is and how wide it is.
 *
 *  The scanner holds the level set it walks for its whole life, so a publication on another thread
 *  waits for it; a scan is taken, walked and dropped rather than kept beside a writer.
 */
template <typename key_slot_type_, typename volume_type_>
class collection_scanner {
  public:

#pragma region Types

    /** The key slot every run of the scanned collection holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of the scanned collection lives on. */
    using volume_t = volume_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The collection this scanner walks. */
    using collection_t = collection<key_slot_t, volume_t>;

    /** One level set of that collection's tree held against publication. */
    using snapshot_t = collection_snapshot<key_slot_t, volume_t>;

    /** One staged write as the merge sees it. */
    using staged_entry_t = typename collection_t::staged_entry_t;

#pragma endregion Types

#pragma region Construction

    collection_scanner() noexcept = default;
    collection_scanner(collection_scanner const&) = delete;
    collection_scanner& operator=(collection_scanner const&) = delete;
    collection_scanner(collection_scanner&&) noexcept = default;
    collection_scanner& operator=(collection_scanner&&) noexcept = default;

    /** A scan of @p source positioned on its first key not below @p from, where @p source must outlive the scanner,
     *  seeing what a reader pinned at @p stamp sees. */
    static expected<collection_scanner> over(collection_t const& source, stored_key_t from = limit<stored_key_t>::min(),
                                             transaction_stamp_t stamp = head_stamp_k) noexcept {
        collection_scanner scanner;
        scanner.stamp_ = stamp;
        if (status_t const listed = source.staged_in_order(scanner.staged_, stamp); listed.failed()) return listed;
        while (scanner.staged_at_ != scanner.staged_.size() && scanner.staged_[scanner.staged_at_].stored_key < from)
            ++scanner.staged_at_;
        scanner.levels_ = snapshot_t::of(source.tree(), stamp);

        std::size_t runs = 0;
        for (std::size_t number = 0; number != scanner.levels_.levels().size(); ++number)
            runs += scanner.levels_.levels().level(number).size();
        if (scanner.cursors_.reserve(runs) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A scan could not hold a cursor per run"};

        for (std::size_t number = 0; number != scanner.levels_.levels().size(); ++number) {
            auto const& level = scanner.levels_.levels().level(number);
            for (std::size_t index = 0; index != level.size(); ++index) {
                run_cursor_t cursor;
                cursor.level_number = static_cast<std::uint32_t>(number);
                cursor.run_index = static_cast<std::uint32_t>(index);
                cursor.population = level.run(index).population_count();
                cursor.position = lower_bound_(level.run(index), cursor.population, from);
                [[maybe_unused]] st::status_t const placed = scanner.cursors_.push_back(std::move(cursor));
            }
        }
        scanner.advance();
        return scanner;
    }

#pragma endregion Construction

#pragma region Walk

    /** Whether every key has been visited. */
    [[nodiscard]] bool reached_end() const noexcept { return state_ == select_result_t::nothing_k; }

    /** The key the scan sits on, meaningless once the walk has ended. */
    [[nodiscard]] stored_key_t stored_key() const noexcept { return stored_key_; }

    /** Whether the key the scan sits on carries a value or a tombstone. */
    [[nodiscard]] select_result_t state() const noexcept { return state_; }

    /** Bytes the value of that key takes, which is zero for a tombstone and for an empty value. */
    [[nodiscard]] stored_value_size_t size() const noexcept { return size_; }

    /** Moves to the next key, taking its newest version at or below the scan's stamp and skipping the versions behind
     *  it, and skipping a key whose every version is newer. */
    void advance() noexcept {
        for (;;) {
            stored_key_t smallest = limit<stored_key_t>::max();
            bool any = false;
            if (staged_at_ != staged_.size()) {
                smallest = staged_[staged_at_].stored_key;
                any = true;
            }
            for (std::size_t index = 0; index != cursors_.size(); ++index) {
                if (cursors_[index].position == cursors_[index].population) continue;
                stored_key_t const head = key_slot_at_(cursors_[index]).stored_key;
                if (any && !(head < smallest)) continue;
                smallest = head;
                any = true;
            }
            if (!any) {
                state_ = select_result_t::nothing_k;
                size_ = 0;
                return;
            }

            stored_key_ = smallest;
            bool taken = false;
            if (staged_at_ != staged_.size() && staged_[staged_at_].stored_key == smallest) {
                take_staged_();
                taken = true;
                ++staged_at_;
            }
            else taken = take_youngest_run_(smallest);

            // A run holds every version of a key side by side, so each cursor passes all of them at once.
            for (std::size_t index = 0; index != cursors_.size(); ++index)
                while (cursors_[index].position != cursors_[index].population &&
                       key_slot_at_(cursors_[index]).stored_key == smallest)
                    ++cursors_[index].position;
            if (taken) return;
        }
    }

#pragma endregion Walk

  private:

#pragma region Private Types

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
        return levels_.levels().level(cursor.level_number).run(cursor.run_index).key_slot(cursor.position);
    }

    /** Takes the staged write as the answer, which outranks every run that carries the same key. */
    void take_staged_() noexcept {
        state_ = staged_[staged_at_].state;
        size_ = staged_[staged_at_].size;
    }

    /**
     *  @brief Takes the shallowest and, inside a level, the youngest run carrying a version of @p smallest at or below
     *      the scan's stamp as the answer, the newest such version inside it.
     *  @return Whether any run carries one.
     */
    bool take_youngest_run_(stored_key_t smallest) noexcept {
        std::size_t chosen = cursors_.size();
        std::uint64_t chosen_position = 0;
        for (std::size_t index = 0; index != cursors_.size(); ++index) {
            run_cursor_t const& cursor = cursors_[index];
            if (chosen != cursors_.size() && !outranks_(cursor, cursors_[chosen])) continue;
            auto const& run = levels_.levels().level(cursor.level_number).run(cursor.run_index);
            std::uint64_t position = cursor.position;
            while (position != cursor.population && run.key_slot(position).stored_key == smallest &&
                   run.stamp(position) > stamp_)
                ++position;
            if (position == cursor.population || !(run.key_slot(position).stored_key == smallest)) continue;
            chosen = index;
            chosen_position = position;
        }
        if (chosen == cursors_.size()) return false;
        run_cursor_t const& cursor = cursors_[chosen];
        key_slot_t const& slot =
            levels_.levels().level(cursor.level_number).run(cursor.run_index).key_slot(chosen_position);
        state_ = slot.is_tombstone() ? select_result_t::erased_k : select_result_t::success_k;
        size_ = slot.is_tombstone() ? 0 : slot.size;
        return true;
    }

    /** Whether @p first carries a newer version of a key than @p second does. */
    [[nodiscard]] static bool outranks_(run_cursor_t const& first, run_cursor_t const& second) noexcept {
        if (first.level_number != second.level_number) return first.level_number < second.level_number;
        return first.run_index > second.run_index;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The level set the scan walks, held for the whole life of the scanner. */
    snapshot_t levels_;

    /** Every staged write in ascending order of its key. */
    st::basic_vector<staged_entry_t> staged_;

    /** One cursor per run of every level the snapshot holds. */
    st::basic_vector<run_cursor_t> cursors_;

    /** The staged write the merge would take next. */
    std::size_t staged_at_ = 0;

    /** The key the scan sits on. */
    stored_key_t stored_key_ = stored_key_t {};

    /** What that key carries, which reads as @c nothing_k once the walk has ended. */
    select_result_t state_ = select_result_t::nothing_k;

    /** Bytes the value of that key takes. */
    stored_value_size_t size_ = 0;

    /** The stamp the scan reads at, which sees the newest version of every key at or below it. */
    transaction_stamp_t stamp_ = head_stamp_k;

#pragma endregion Private Members
};

#pragma endregion Scanner

#pragma endregion Collection Scanner

} // namespace unum::ustore::blobs
