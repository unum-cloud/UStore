/**
 *  @file src/nvme/read_set.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The keys a transaction watches, the stamps every commit records against them, and the live roster.
 *
 *  Three pieces of shared state sit here. @c commit_record_t issues the stamp every transaction is
 *  ordered by and remembers the last commit to write each key, entirely in read-modify-writes.
 *  @c read_set remembers what one transaction watched and from which stamp, and belongs to that
 *  transaction alone. @c transaction_roster_t holds the live transactions, and is where the layer's
 *  two claims live: the staging queue pair, and the word enrolment scans under.
 *
 *  No mutex, and nothing on the path that stages or commits waits for anything but the queue pair.
 */
#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`

#include <algorithm> // `std::min`
#include <atomic>    // `std::memory_order`
#include <limits>    // `std::numeric_limits`
#include <new>       // placement `new`
#include <thread>    // `std::this_thread::yield`

#include <forkunion/atomics.hpp>           // `fu::preferred_atomic_ref`
#include <smashtable/basic_hash_table.hpp> // `st::hash_map`, `st::assume_reserved_t`
#include <smashtable/basic_vector.hpp>     // `st::basic_vector`

#include "nvme/resources.hpp" // `resources_shared_ptr_t`
#include "nvme/types.hpp"     // `transaction_id_t`, `constrained_allocator`

namespace unum::ustore::nvme {

#pragma region Ordering

/**
 *  @brief The order every start and every commit of one registry is placed in, issued by a counter.
 *
 *  The process clock cannot serve here: @c clock_t is @c std::chrono::system_clock, which is neither
 *  steady nor distinct across threads, so two commits can share an instant and a record keyed by one
 *  would keep a single winner and lose the other's keys without ever reporting it.
 */
using transaction_stamp_t = std::uint64_t;

/** The stamp of a key no commit has ever written, which orders before every issued stamp. */
inline constexpr transaction_stamp_t unwritten_stamp_k = 0;

/** The first stamp a registry issues, so an unwritten key never collides with a real commit. */
inline constexpr transaction_stamp_t first_transaction_stamp_k = 1;

/** The stamp of a transaction that has not started, which orders after every issued stamp. */
inline constexpr transaction_stamp_t invalid_transaction_stamp_k = std::numeric_limits<transaction_stamp_t>::max();

/**
 *  @brief The commit-record slot a key of one collection falls in.
 *
 *  Mixed through the 64-bit avalanche so keys that differ in one bit land far apart, because the
 *  record has no keys to compare against and a clustered hash would turn neighbours into conflicts.
 */
template <typename stored_key_>
inline std::uint64_t watched_key_fingerprint(collection_numeric_id_t group_id, stored_key_ const& stored_key) noexcept {
    std::uint64_t mixed = static_cast<std::uint64_t>(st::hash<stored_key_> {}(stored_key)) ^
                          (static_cast<std::uint64_t>(group_id) * 0x9e3779b97f4a7c15ull);
    mixed ^= mixed >> 33;
    mixed *= 0xff51afd7ed558ccdull;
    mixed ^= mixed >> 33;
    mixed *= 0xc4ceb9fe1a85ec53ull;
    mixed ^= mixed >> 33;
    return mixed;
}

/** The collection identifier a registry serving one collection records its keys under. */
inline constexpr collection_numeric_id_t sole_collection_id_k = 0;

#pragma endregion Ordering

#pragma region Watches

/** When a transaction checks its watched keys for conflicts. */
enum class watch_schedule_t : std::uint8_t {

    /** Ignores key collisions. */
    never_k,

    /** Checks in place, at the moment of the call. */
    now_k,

    /** Checks between the transaction start and its commit. */
    on_commit_k,

    /** Checks between the operation and the commit. */
    on_commit_since_operation_k,

    /** Schedule used when none is specified. */
    default_k = on_commit_k,
};

/** Why a transaction watches a key, which a second watch of the same key widens. */
enum class watch_purpose_t : std::uint8_t {

    /** The transaction read the key. */
    read_k = 0x01,

    /** The transaction wrote the key. */
    write_k = 0x02,

    /** The transaction tracks the key without reading or writing it. */
    track_k = 0x04,

    /** Every purpose, which a count with no filter asks for. */
    all_k = read_k | write_k | track_k,
};

/** The purposes of either watch. */
constexpr watch_purpose_t operator|(watch_purpose_t first, watch_purpose_t second) noexcept {
    return static_cast<watch_purpose_t>(static_cast<std::uint8_t>(first) | static_cast<std::uint8_t>(second));
}

/** The purposes both watches share. */
constexpr watch_purpose_t operator&(watch_purpose_t first, watch_purpose_t second) noexcept {
    return static_cast<watch_purpose_t>(static_cast<std::uint8_t>(first) & static_cast<std::uint8_t>(second));
}

/**
 *  @brief The keys one transaction watches, each with the stamp it was first watched from and why.
 *
 *  Watching the same key twice keeps the earlier stamp and widens the purpose, so a key read and
 *  then written conflicts on both counts from the earlier of the two moments.
 */
template <typename stored_key_>
class read_set {
  public:
    using stored_key_t = stored_key_;

    /** From which stamp a key was watched, and why. */
    struct watch_t {

        /** The earliest stamp any watch of this key was scoped from. */
        transaction_stamp_t watch_stamp;

        /** Every purpose the watches of this key were taken for. */
        watch_purpose_t purpose;
    };

    using stored_keys_t =
        st::hash_map<stored_key_t, watch_t, st::hash<stored_key_t>, st::equal_to_t, constrained_allocator<std::byte>>;

    /** A set charging @p resources for the keys it holds. */
    explicit read_set(resources_shared_ptr_t const& resources) noexcept
        : stored_keys({}, {}, constrained_allocator<std::byte> {resources->memory_constraint}) {}

    /** Whether the transaction watches nothing. */
    bool empty() const noexcept { return stored_keys.empty(); }

    /** The watched keys taken for every purpose in @p purpose. */
    std::size_t count(watch_purpose_t purpose = watch_purpose_t::all_k) const noexcept {
        if (purpose == watch_purpose_t::all_k) return stored_keys.size();
        std::size_t matching = 0;
        [[maybe_unused]] st::status_t const walked = stored_keys.for_each(
            [&](auto const& entry) noexcept { matching += (entry.value().purpose & purpose) == purpose; });
        return matching;
    }

    /** Makes room for @p count more keys, so a following @c add takes no allocation. */
    status_t reserve_more(std::size_t count) noexcept {
        if (stored_keys.reserve_more(count) == stored_keys_t::reserve_result_t::failed_k)
            return {status_code_t::out_of_memory_k, "No room for the keys a transaction watches"};
        return {};
    }

    /**
     *  @brief Watches @p stored_key from @p watch_stamp for @p purpose, in room @c reserve_more took.
     *  @param[in] stored_key The key the transaction watches.
     *  @param[in] watch_stamp The stamp this watch is scoped from, which only lowers a key's stamp.
     *  @param[in] purpose Why this watch was taken, which only widens a key's purpose.
     */
    void add(stored_key_t stored_key, transaction_stamp_t watch_stamp, watch_purpose_t purpose,
             st::assume_reserved_t) noexcept {
        auto insert = [&](auto& unused_slot) noexcept {
            new (&unused_slot.key_ref()) stored_key_t {stored_key};
            new (&unused_slot.value_ref()) watch_t {watch_stamp, purpose};
        };
        auto update = [&](auto& equal_slot) noexcept {
            watch_t& watch = equal_slot.value_ref();
            watch.watch_stamp = std::min(watch_stamp, watch.watch_stamp);
            watch.purpose = watch.purpose | purpose;
        };
        [[maybe_unused]] auto const stored = stored_keys.probe_to_upsert(stored_key, insert, update,
                                                                         st::assume_reserved_t {});
    }

    /** Drops every watch, for a transaction starting over. */
    void clear() noexcept { stored_keys.clear(); }

    /** The watched keys, charged to the budget the set was made with. */
    stored_keys_t stored_keys;
};

#pragma endregion Watches

#pragma region Commit Record

/**
 *  @brief The order every transaction of one registry is placed in, and the last commit to write each key.
 *
 *  A commit claims its stamp with one @c fetch_add and records what it wrote with one @c fetch_max per
 *  key into a table of a fixed slot count, so a commit allocates nothing, publishes in a single
 *  read-modify-write and is never reclaimed by anyone. That fixed count is the whole bound: a
 *  transaction running beside ten million commits leaves the record exactly the size it was built at.
 *
 *  Two stamps, not one. A commit claims from @c issue_stamp the moment it begins, while a watch is
 *  scoped from @c watch_stamp, which only moves once a commit's writes have landed. A transaction
 *  starting between the two therefore orders at or before the commit in flight and is refused by it,
 *  rather than being excused by a stamp that commit had claimed but not yet honoured.
 *
 *  That single watermark is enough only because the registry above serializes its commits with the
 *  staging queue pair, so at most one commit ever sits between claiming a stamp and publishing it.
 *
 *  Two keys sharing a slot share a stamp, so a collision reports a conflict neither transaction had
 *  and can never hide one either of them did have. The record errs toward refusing, never toward
 *  admitting, which is the only direction an optimistic layer may err in.
 */
class commit_record_t {
  public:

    /** A record of @p slots stamps charging @p constraint, holding no slots when it refuses them. */
    explicit commit_record_t(std::size_t slots, resource_constraint_t& constraint) noexcept
        : stamps_(constrained_allocator<transaction_stamp_t> {constraint}), next_stamp_(first_transaction_stamp_k),
          published_stamp_(first_transaction_stamp_k) {
        std::size_t const rounded = round_up_to_power_of_two_(slots);
        if (st::failed(stamps_.resize(rounded, unwritten_stamp_k))) return;
        slot_mask_ = rounded - 1;
    }

    commit_record_t(commit_record_t const&) = delete;
    commit_record_t& operator=(commit_record_t const&) = delete;

    /** The slots the record holds, which is zero when the budget refused them. */
    std::size_t slots() const noexcept { return stamps_.size(); }

    /** The bytes the record occupies, which no number of commits ever changes. */
    std::size_t size_bytes() const noexcept { return stamps_.size() * sizeof(transaction_stamp_t); }

    /** Claims the next stamp, which orders after every stamp claimed before it. */
    transaction_stamp_t issue_stamp() noexcept {
        return fu::preferred_atomic_ref<transaction_stamp_t> {next_stamp_}.fetch_add(1, std::memory_order_acq_rel);
    }

    /** Opens @p stamp to the transactions that start next, which its caller does once its writes land. */
    void publish_stamp(transaction_stamp_t stamp) noexcept {
        [[maybe_unused]] transaction_stamp_t const before =
            fu::preferred_atomic_ref<transaction_stamp_t> {published_stamp_}.fetch_max(stamp + 1,
                                                                                       std::memory_order_acq_rel);
    }

    /**
     *  @brief The stamp a watch taken now is scoped from.
     *
     *  Every commit whose writes have landed orders strictly before it, and every commit still in
     *  flight orders at or after it, so a watch taken here is refused by the one and not by the other.
     */
    transaction_stamp_t watch_stamp() const noexcept {
        return fu::preferred_atomic_ref<transaction_stamp_t const> {published_stamp_}.load(std::memory_order_acquire);
    }

    /**
     *  @brief Records that a commit at @p stamp wrote the key @p fingerprint names.
     *
     *  Acquire-release, so the commit's writes into the collections cannot be observed before the
     *  stamp that warns about them; a reader therefore sees the warning first or the writes never.
     */
    void record(std::uint64_t fingerprint, transaction_stamp_t stamp) noexcept {
        if (stamps_.empty()) return;
        [[maybe_unused]] transaction_stamp_t const before =
            fu::preferred_atomic_ref<transaction_stamp_t> {stamps_[fingerprint & slot_mask_]}.fetch_max(
                stamp, std::memory_order_acq_rel);
    }

    /** Whether a commit at or after @p watch_stamp wrote the key @p fingerprint names. */
    bool conflicts(std::uint64_t fingerprint, transaction_stamp_t watch_stamp) const noexcept {
        if (stamps_.empty()) return true;
        transaction_stamp_t const written =
            fu::preferred_atomic_ref<transaction_stamp_t const> {stamps_[fingerprint & slot_mask_]}.load(
                std::memory_order_acquire);
        return written >= watch_stamp;
    }

  private:

    /** The smallest power of two at least @p wanted, which turns the slot index into a mask. */
    static std::size_t round_up_to_power_of_two_(std::size_t wanted) noexcept {
        std::size_t rounded = 1;
        while (rounded < wanted) rounded <<= 1;
        return rounded;
    }

    /** The last commit stamp of every slot, indexed by a fingerprint masked into range. */
    st::basic_vector<transaction_stamp_t, constrained_allocator<transaction_stamp_t>> stamps_;

    /** The stamp the next claim takes, moved only by @c issue_stamp. */
    transaction_stamp_t next_stamp_;

    /** The first stamp whose writes have not landed, moved only by @c publish_stamp. */
    transaction_stamp_t published_stamp_;

    /** The mask that folds a fingerprint onto a slot, zero while the record holds none. */
    std::size_t slot_mask_ = 0;
};

#pragma endregion Commit Record

#pragma region Roster

/**
 *  @brief The live transactions of one registry, each holding a slot for as long as it exists.
 *
 *  Most of what moves here excludes nobody. A transaction's start stamp is written by its own thread
 *  and read by any other through one atomic reference, and the staging queue pair is a single
 *  compare-exchange whose winner holds it from @c stage until @c commit or @c rollback, because those
 *  are separate calls from its owner rather than one operation.
 *
 *  @c enroll and @c retire are the exception, and they take the roster claim. "One identifier, one
 *  slot" is membership of a set, and a set cannot be entered wait-free with a scan and a claim: two
 *  callers enrolling one identifier can each scan a prefix the other had not yet written, so both see
 *  a clean array and both win. Resolving that by index does not close it either, because the two scans
 *  see each other asymmetrically - the earlier scanner reads a slot the later claimant has not reached.
 *  Only a single atomic point of decision would, and a free slot has to be found before it can be
 *  claimed. So enrolment scans and claims under one word, taken twice per transaction - once when it
 *  is created and once when it is destroyed - and never on the path that stages or commits.
 */
class transaction_roster_t {
  public:

    /** A roster of @p slots transactions charging @p constraint, holding none when it refuses them. */
    explicit transaction_roster_t(std::size_t slots, resource_constraint_t& constraint) noexcept
        : slots_(constrained_allocator<transaction_slot_t> {constraint}) {
        [[maybe_unused]] st::status_t const resized = slots_.resize(slots, transaction_slot_t {});
    }

    transaction_roster_t(transaction_roster_t const&) = delete;
    transaction_roster_t& operator=(transaction_roster_t const&) = delete;

    /** The transactions the roster admits at once, which is zero when the budget refused them. */
    std::size_t slots() const noexcept { return slots_.size(); }

    /** The bytes the roster occupies, which no number of transactions ever changes. */
    std::size_t size_bytes() const noexcept { return slots_.size() * sizeof(transaction_slot_t); }

    /** Whether @p id holds a slot. */
    bool contains(transaction_id_t id) const noexcept { return index_of_(id) != slots_.size(); }

    /** The transactions holding a slot right now. */
    std::size_t live() const noexcept {
        std::size_t held = 0;
        for (std::size_t index = 0; index != slots_.size(); ++index)
            held += identity_(index).load(std::memory_order_acquire) != invalid_transaction_id_k;
        return held;
    }

    /** Claims a slot for @p id, or reports the duplicate or the exhausted roster. */
    status_t enroll(transaction_id_t id) noexcept {
        if (id == invalid_transaction_id_k) return {status_code_t::invalid_argument_k, "Invalid id"};

        take_roster_claim_();
        std::size_t free_slot = slots_.size();
        bool duplicate = false;
        for (std::size_t index = 0; index != slots_.size() && !duplicate; ++index) {
            transaction_id_t const held = identity_(index).load(std::memory_order_relaxed);
            duplicate = held == id;
            if ((held == invalid_transaction_id_k) & (free_slot == slots_.size())) free_slot = index;
        }
        bool const enrolled = !duplicate && free_slot != slots_.size();
        if (enrolled) {
            start_stamp_(free_slot).store(invalid_transaction_stamp_k, std::memory_order_relaxed);
            identity_(free_slot).store(id, std::memory_order_release);
        }
        release_roster_claim_();

        if (duplicate) return {status_code_t::already_exists_k, "Object already exists"};
        if (!enrolled) return {status_code_t::capacity_exhausted_k, "No room for another transaction"};
        return {};
    }

    /** Releases the slot @p id holds, leaving the roster with no trace of it. */
    void retire(transaction_id_t id) noexcept {
        take_roster_claim_();
        std::size_t const index = index_of_(id);
        if (index != slots_.size()) release_(index);
        release_roster_claim_();
    }

    /** Records that @p id started at @p stamp. */
    void start(transaction_id_t id, transaction_stamp_t stamp) noexcept {
        std::size_t const index = index_of_(id);
        if (index != slots_.size()) start_stamp_(index).store(stamp, std::memory_order_release);
    }

    /** Records that @p id no longer runs, without releasing its slot. */
    void stop(transaction_id_t id) noexcept { start(id, invalid_transaction_stamp_k); }

    /** The stamp @p id started at, or @c invalid_transaction_stamp_k while it has not started. */
    transaction_stamp_t start_stamp(transaction_id_t id) const noexcept {
        std::size_t const index = index_of_(id);
        return index != slots_.size() ? start_stamp_(index).load(std::memory_order_acquire)
                                      : invalid_transaction_stamp_k;
    }

    /**
     *  @brief Takes the staging queue pair for @p id, waiting while another transaction holds it.
     *
     *  The queue pair is held from @c stage until @c commit or @c rollback, which are separate calls from
     *  the owner, so the room a stage found in the store's write buffers is still there when the
     *  commit fills it. It is also what lets one published watermark speak for every commit.
     */
    void claim_staging(transaction_id_t id) noexcept {
        fu::preferred_atomic_ref<transaction_id_t> const staging {staged_by_};
        transaction_id_t expected_free = invalid_transaction_id_k;
        while (
            !staging.compare_exchange_strong(expected_free, id, std::memory_order_acquire, std::memory_order_relaxed)) {
            expected_free = invalid_transaction_id_k;
            std::this_thread::yield();
        }
    }

    /** Hands the staging queue pair back. */
    void release_staging() noexcept {
        fu::preferred_atomic_ref<transaction_id_t> {staged_by_}.store(invalid_transaction_id_k,
                                                                      std::memory_order_release);
    }

    /** Whether @p id holds the staging queue pair. */
    bool staged_by(transaction_id_t id) const noexcept {
        return fu::preferred_atomic_ref<transaction_id_t const> {staged_by_}.load(std::memory_order_acquire) == id;
    }

  private:

    /** One transaction's place in the roster, claimed and published by its identifier alone. */
    struct transaction_slot_t {

        /** The transaction holding the slot, or @c invalid_transaction_id_k while it is free. */
        transaction_id_t id = invalid_transaction_id_k;

        /** The stamp that transaction started at, or @c invalid_transaction_stamp_k while it has not. */
        transaction_stamp_t start_stamp = invalid_transaction_stamp_k;
    };

    /** Takes the roster claim, waiting while another caller enrols or retires. */
    void take_roster_claim_() noexcept {
        fu::preferred_atomic_ref<std::int32_t> const claim {roster_claim_};
        std::int32_t expected_free = 0;
        while (!claim.compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            expected_free = 0;
            std::this_thread::yield();
        }
    }

    /** Hands the roster claim back. */
    void release_roster_claim_() noexcept {
        fu::preferred_atomic_ref<std::int32_t> {roster_claim_}.store(0, std::memory_order_release);
    }

    /** The slot @p id holds, or the slot count when it holds none. */
    std::size_t index_of_(transaction_id_t id) const noexcept {
        for (std::size_t index = 0; index != slots_.size(); ++index)
            if (identity_(index).load(std::memory_order_acquire) == id) return index;
        return slots_.size();
    }

    /** Frees the slot at @p index for the next claimant, after retiring what it held. */
    void release_(std::size_t index) noexcept {
        start_stamp_(index).store(invalid_transaction_stamp_k, std::memory_order_relaxed);
        identity_(index).store(invalid_transaction_id_k, std::memory_order_release);
    }

    fu::preferred_atomic_ref<transaction_id_t> identity_(std::size_t index) noexcept {
        return fu::preferred_atomic_ref<transaction_id_t> {slots_[index].id};
    }

    fu::preferred_atomic_ref<transaction_id_t const> identity_(std::size_t index) const noexcept {
        return fu::preferred_atomic_ref<transaction_id_t const> {slots_[index].id};
    }

    fu::preferred_atomic_ref<transaction_stamp_t> start_stamp_(std::size_t index) noexcept {
        return fu::preferred_atomic_ref<transaction_stamp_t> {slots_[index].start_stamp};
    }

    fu::preferred_atomic_ref<transaction_stamp_t const> start_stamp_(std::size_t index) const noexcept {
        return fu::preferred_atomic_ref<transaction_stamp_t const> {slots_[index].start_stamp};
    }

    /** The slot of every transaction the roster admits, claimed by identifier. */
    st::basic_vector<transaction_slot_t, constrained_allocator<transaction_slot_t>> slots_;

    /** The transaction holding the staging queue pair, or @c invalid_transaction_id_k while none does. */
    transaction_id_t staged_by_ = invalid_transaction_id_k;

    /** One while a caller is enrolling or retiring a transaction, zero otherwise. */
    std::int32_t roster_claim_ = 0;
};

#pragma endregion Roster

} // namespace unum::ustore::nvme
