/**
 *  @file include/ustore/blobs/nvme_database.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Databases of named collections and single-collection databases, with their snapshots, their optimistic
 *      transactions, and the registries and read sets that order and check those transactions.
 *
 *  A database is a catalog and nothing else: it resolves a name to a collection, hands that collection
 *  a channel, and persists the mapping as one named blob of a volume so a reopen finds the same names.
 *  Every byte of data belongs to the collection below it, and every conflict belongs to the registry
 *  beside it; what is left is placement, naming and the order the two are changed in. One claim guards every
 *  call that reaches a collection, since the collection layer is single-threaded by construction - its write
 *  buffer, its arena and its value cache are plain containers that a read mutates as much as a write does. A
 *  call that reaches no collection takes no claim: the entries are pinned on the heap, the catalog is published
 *  into one of two views, and a publication waits until the view it retired has no reader left.
 *
 *  A single-collection database is the smallest thing that can be committed to: one collection, the
 *  registry that orders the commits landing in it, and two blobs on the volume - the schema a reopen
 *  checks itself against, and whatever bytes the owner keeps beside the data. A graph is two of these.
 *  Reads take the caller's channel, so any number of threads read at once; writes take the two channels the
 *  database was opened with, and the write path is its one claim, held as a reader count or as a single writer.
 *
 *  A snapshot copies no run and moves no byte: it flushes the write buffers and counts a reader into every tree,
 *  so no run a collection names can be retired while the snapshot lives, and it carries the commit stamp the
 *  hold was taken at. Every commit ordered before that stamp is inside the snapshot and every later one is not.
 *
 *  Transactions are optimistic. Writes go to a write buffer of the transaction's own and reach the collections
 *  only at the commit, so a rollback costs nothing and a failed stage leaves the changes where a caller can still
 *  log them. Every read is watched from a stamp the registry publishes only once a commit's writes have landed,
 *  never from the counter a commit claims when it begins, since a watch scoped from the claim would be excused
 *  by a commit still in flight. @c commit_record_t issues the stamps and remembers the last commit to write each
 *  key, @c read_set remembers what one transaction watched and from which stamp, and @c transaction_roster_t
 *  holds the live transactions. Nothing takes a lock: a start reads the stamp counter, a commit claims one with
 *  a @c fetch_add and publishes what it wrote with one @c fetch_max per key, and the single exclusion is the
 *  staging queue pair, which a staged transaction holds from @c stage until @c commit.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::int32_t`, `std::uint16_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <algorithm>   // `std::min`
#include <array>       // `std::array`
#include <atomic>      // `std::atomic_thread_fence`, `std::memory_order`
#include <concepts>    // `std::same_as`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`, placement `new`
#include <span>        // `std::span`
#include <type_traits> // `std::remove_reference_t`
#include <utility>     // `std::exchange`, `std::forward`, `std::move`, `std::swap`
#include <variant>     // `std::variant`, `std::visit`, `std::get_if`

#include <forkunion/atomics.hpp>           // `fu::preferred_atomic_ref`
#include <forkunion/capabilities.hpp>      // `fu::preferred_yield_t`
#include <smashtable/basic_hash_table.hpp> // `st::hash_map`, `st::assume_reserved`
#include <smashtable/basic_vector.hpp>     // `st::basic_vector`

#include "ustore/blobs/nvme_collection.hpp"   // `collection`, `collection_snapshot`, `collection_scanner`
#include "ustore/blobs/nvme_key_slot.hpp"     // `key_slot`, `stored_value_size_t`, `collection_id_t`
#include "ustore/blobs/nvme_tree.hpp"         // `lsm_tree_run_record_t`, `database_config_t`
#include "ustore/blobs/nvme_write_buffer.hpp" // `write_buffer`, `write_buffer_status_t`
#include "ustore/utilities/nvme_channels.hpp" // `io_channel_t`, `basic_io_channel_pool`, `pooled_buffer_t`
#include "ustore/utilities/nvme_ring.hpp"     // `extent_t`, `uuid_t`, `constrained_allocator`
#include "ustore/utilities/nvme_volume.hpp"   // `basic_extent_volume`, `basic_volume_router`, `resources_shared_ptr_t`
#include "ustore/utilities/types.hpp"         // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::blobs {

using utilities::any_files_disk_t;
using utilities::basic_extent_volume;
using utilities::basic_io_channel_pool;
using utilities::basic_volume_router;
using utilities::claim_word_t;
using utilities::volume_id_t;
using utilities::writer_claim_k;

#pragma region Read Set

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
inline std::uint64_t watched_key_fingerprint(collection_id_t group_id, stored_key_ const& stored_key) noexcept {
    std::uint64_t mixed = static_cast<std::uint64_t>(stored_key_hash_t {}(stored_key)) ^
                          (static_cast<std::uint64_t>(collection_id_hash_t {}(group_id)) * 0x9e3779b97f4a7c15ull);
    mixed ^= mixed >> 33;
    mixed *= 0xff51afd7ed558ccdull;
    mixed ^= mixed >> 33;
    mixed *= 0xc4ceb9fe1a85ec53ull;
    mixed ^= mixed >> 33;
    return mixed;
}

/** The collection identifier a registry serving one collection records its keys under. */
inline constexpr collection_id_t sole_collection_id_k {};

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
        st::hash_map<stored_key_t, watch_t, stored_key_hash_t, st::equal_to_t, constrained_allocator<std::byte>>;

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
                                                                         st::assume_reserved);
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
            fu::preferred_yield_t {}(staged_by_, expected_free, std::size_t {0});
            expected_free = invalid_transaction_id_k;
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
        fu::preferred_atomic_ref<claim_word_t> const claim {roster_claim_};
        claim_word_t expected_free = 0;
        while (!claim.compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            fu::preferred_yield_t {}(roster_claim_, expected_free, std::size_t {0});
            expected_free = 0;
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
    claim_word_t roster_claim_ = 0;
};

#pragma endregion Roster

#pragma endregion Read Set

#pragma region Database Transaction State

#pragma region Grouped Write Buffer

/**
 *  @brief The uncommitted writes of one transaction, keyed by stored key inside each collection.
 *  @note Owned by one transaction and touched by its thread alone, so nothing here is atomic.
 */
template <typename stored_key_>
class grouped_write_buffer {
  public:
#pragma region Types

    using stored_key_t = stored_key_;
    using group_id_t = collection_id_t;
    using stored_value_offset_in_buffer_t = std::size_t;

    /** The value widths one collection admits, of which exactly one is ever set. */
    struct group_config_t {

        /** The width every value of a fixed-width collection has, zero for a variable-width one. */
        std::size_t value_size;

        /** The width no value of a variable-width collection exceeds, zero for a fixed-width one. */
        std::size_t value_max_size;
    };

    /** What one collection's share of the buffer holds, which a stage sizes a flush against. */
    struct group_info_t {

        /** The collection these writes belong to. */
        group_id_t id;

        /** The keys written inside it. */
        std::size_t elements_count;

        /** The bytes their values occupy. */
        std::size_t elements_size_bytes;
    };

    /** Where one written value sits in the insertions buffer, or that the key was removed. */
    struct stored_value_place_t {

        /** The first byte of the value, @c tombstone_offset_k for a removal and
         *  @c empty_stored_value_offset_k for a value of no bytes. */
        stored_value_offset_in_buffer_t offset;

        /** The bytes of the value. */
        stored_value_size_t size;
    };

    /** One collection's share of the buffer. */
    struct group_t {
        using stored_values_t = st::hash_map<stored_key_t, stored_value_place_t, stored_key_hash_t, st::equal_to_t,
                                             constrained_allocator<std::byte>>;

        /** The value widths the collection admits, which every write is checked against. */
        group_config_t config;

        /** Where each written key's value sits. */
        stored_values_t stored_values;

        /** The bytes those values occupy. */
        std::size_t elements_size_bytes;
    };

    using groups_t =
        st::hash_map<group_id_t, group_t, collection_id_hash_t, st::equal_to_t, constrained_allocator<std::byte>>;

#pragma endregion Types

#pragma region Public Members

    /** One share of the buffer per written collection. */
    groups_t groups_by_id;

    /** The bytes every written value of every collection sits in. */
    st::basic_vector<std::byte, constrained_allocator<std::byte>> insertions_buffer;

    /** The first free byte of @c insertions_buffer. */
    stored_value_offset_in_buffer_t next_inserted_offset = 0;

#pragma endregion Public Members

#pragma region Construction

    inline grouped_write_buffer(std::size_t capacity_bytes, std::size_t elements_max_count,
                                resources_shared_ptr_t resources) noexcept;

    inline grouped_write_buffer(grouped_write_buffer const& other) = delete;
    inline grouped_write_buffer(grouped_write_buffer&& other) noexcept;
    inline grouped_write_buffer& operator=(grouped_write_buffer const& other) = delete;
    inline grouped_write_buffer& operator=(grouped_write_buffer&& other) noexcept;

#pragma endregion Construction

#pragma region Interface Methods

    inline bool empty() const noexcept { return elements_count_ == 0; }
    inline std::size_t count() const noexcept { return elements_count_; }

    /** Hands every written collection's share to @p callback, without staging a list of them. */
    template <typename callback_>
    inline void for_each_group(callback_&& callback) const noexcept;

    inline write_buffer_status_t status(group_id_t id, stored_key_t stored_key) const noexcept;

    inline status_t stored_value_size(group_id_t id, stored_key_t stored_key, stored_value_size_t& size) const noexcept;

    inline status_t select(group_id_t id, stored_key_t stored_key, stored_value_span_t stored_value,
                           stored_value_size_t& size) const noexcept;

    template <typename stored_value_location_>
    inline write_buffer_status_t find(group_id_t id, stored_key_t stored_key,
                                      stored_value_location_& location) const noexcept;
    template <typename stored_value_location_>
    inline void select(stored_value_location_ const& location, stored_value_span_t stored_value,
                       notifier_t notifier) const noexcept;

    template <typename config_provider_>
    inline status_t add(group_id_t id, config_provider_ config_provider, stored_key_t stored_key,
                        stored_value_view_t stored_value) noexcept;
    template <typename config_provider_, typename serializable_stored_value_>
    inline status_t add(group_id_t id, config_provider_ config_provider, stored_key_t stored_key,
                        serializable_stored_value_ const& stored_value) noexcept;
    template <typename config_provider_>
    inline status_t mark_as_removed(group_id_t id, config_provider_ config_provider, stored_key_t stored_key) noexcept;

#pragma endregion Interface Methods

#pragma region Batch Methods

    template <typename stored_value_location_>
    inline status_t find(group_id_t id, std::span<stored_key_t const> stored_keys,
                         std::span<stored_value_location_> locations,
                         missing_locations_t missing_locations = missing_locations_t::invalidate_k) const;
    template <typename stored_value_location_>
    inline void select(std::span<stored_value_location_ const> locations, bytes_span_t buffer,
                       notifier_t notifier) const;

#pragma endregion Batch Methods

#pragma region Validators

    inline status_t check_stored_value(group_config_t const& config, stored_key_t stored_key,
                                       stored_value_size_t stored_value_size) const noexcept;

    inline bool can_fit(std::size_t stored_values_count, std::size_t stored_values_sum_size) const noexcept;

    inline void clear() noexcept;

#pragma endregion Validators

  private:
#pragma region Helper Methods

    /** Finds the share of @p id, making one from @p config_provider when the collection is new. */
    template <typename config_provider_>
    inline status_t reach_group_(group_id_t id, config_provider_ config_provider, group_t*& group) noexcept;

#pragma endregion Helper Methods

#pragma region Private Members

    /** The budget every share of the buffer is charged to. */
    resources_shared_ptr_t resources_;

    /** The bytes @c insertions_buffer holds. */
    std::size_t capacity_bytes_ = 0;

    /** The keys every share together admits. */
    std::size_t elements_max_count_ = 0;

    /** The keys every share together holds. */
    std::size_t elements_count_ = 0;

#pragma endregion Private Members
};

template <typename stored_key_>
inline grouped_write_buffer<stored_key_>::grouped_write_buffer(std::size_t capacity_bytes,
                                                               std::size_t elements_max_count,
                                                               resources_shared_ptr_t resources) noexcept
    : groups_by_id({}, {}, constrained_allocator<std::byte> {resources->memory_constraint}),
      insertions_buffer(constrained_allocator<std::byte> {resources->memory_constraint}), next_inserted_offset(0),
      resources_(resources), capacity_bytes_(capacity_bytes), elements_max_count_(elements_max_count),
      elements_count_(0) {
    // A budget that refuses the bytes leaves no capacity, and `can_fit` then refuses every write.
    if (st::failed(insertions_buffer.resize(capacity_bytes))) capacity_bytes_ = 0;
}

template <typename stored_key_>
inline grouped_write_buffer<stored_key_>::grouped_write_buffer(grouped_write_buffer&& other) noexcept
    : groups_by_id(std::move(other.groups_by_id)), insertions_buffer(std::move(other.insertions_buffer)),
      next_inserted_offset(other.next_inserted_offset), resources_(std::move(other.resources_)),
      capacity_bytes_(other.capacity_bytes_), elements_max_count_(other.elements_max_count_),
      elements_count_(other.elements_count_) {}

template <typename stored_key_>
inline grouped_write_buffer<stored_key_>& grouped_write_buffer<stored_key_>::operator=(
    grouped_write_buffer&& other) noexcept {
    std::swap(groups_by_id, other.groups_by_id);
    std::swap(insertions_buffer, other.insertions_buffer);
    std::swap(next_inserted_offset, other.next_inserted_offset);
    std::swap(resources_, other.resources_);
    std::swap(capacity_bytes_, other.capacity_bytes_);
    std::swap(elements_max_count_, other.elements_max_count_);
    std::swap(elements_count_, other.elements_count_);
    return *this;
}

template <typename stored_key_>
template <typename callback_>
inline void grouped_write_buffer<stored_key_>::for_each_group(callback_&& callback) const noexcept {
    [[maybe_unused]] st::status_t const walked = groups_by_id.for_each([&](auto const& entry) noexcept {
        callback(group_info_t {entry.key(), entry.value().stored_values.size(), entry.value().elements_size_bytes});
    });
}

template <typename stored_key_>
inline write_buffer_status_t grouped_write_buffer<stored_key_>::status(group_id_t id,
                                                                       stored_key_t stored_key) const noexcept {

    auto result = write_buffer_status_t::mismatch_k;
    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) return result;

    iterator.value().stored_values.probe_to_find(stored_key, [&result](auto const& entry) noexcept {
        constexpr write_buffer_status_t results[2] = {
            write_buffer_status_t::found_k,
            write_buffer_status_t::removed_k,
        };
        result = results[entry.value().offset == tombstone_offset_k];
    });

    return result;
}

template <typename stored_key_>
inline status_t grouped_write_buffer<stored_key_>::stored_value_size(group_id_t id, stored_key_t stored_key,
                                                                     stored_value_size_t& size) const noexcept {

    auto status = status_t {status_code_t::not_found_k, "Object not exists"};
    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) return status;

    iterator.value().stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
        if (entry.value().offset != tombstone_offset_k) size = entry.value().size;
        status = status_t {};
    });

    return status;
}

template <typename stored_key_>
inline status_t grouped_write_buffer<stored_key_>::select(group_id_t id, stored_key_t stored_key,
                                                          stored_value_span_t stored_value,
                                                          stored_value_size_t& size) const noexcept {

    status_t status = status_t {status_code_t::not_found_k, "Object not exists"};
    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) return status;

    iterator.value().stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
        auto const& stored_value_place = entry.value();
        if (stored_value_place.offset == tombstone_offset_k) {
            status = status_t {status_code_t::not_found_k, "Object not exists"};
            return;
        }
        if (stored_value_place.offset == empty_stored_value_offset_k) {
            size = 0;
            status = status_t {};
            return;
        }
        if (stored_value.size() < stored_value_place.size) {
            status = status_t {status_code_t::invalid_argument_k, "Buffer is too small"};
            return;
        }

        auto inserted = as_bytes_view(insertions_buffer).subspan(stored_value_place.offset, stored_value_place.size);
        std::memcpy(stored_value.data(), inserted.data(), inserted.size());
        size = inserted.size();
        status = status_t {};
    });

    return status;
}

template <typename stored_key_>
template <typename stored_value_location_>
inline write_buffer_status_t grouped_write_buffer<stored_key_>::find(group_id_t id, stored_key_t stored_key,
                                                                     stored_value_location_& location) const noexcept {

    location.size_bytes = 0;
    location.offset = tombstone_offset_k;
    location.recipient_id = reinterpret_cast<std::uint64_t>(this);
    location.recipient_state_id = 0;

    auto result = write_buffer_status_t::mismatch_k;
    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) return result;

    iterator.value().stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
        auto const& stored_value_place = entry.value();
        location.offset = stored_value_place.offset;
        location.size_bytes = stored_value_place.size;
        constexpr write_buffer_status_t results[2] = {write_buffer_status_t::found_k, write_buffer_status_t::removed_k};
        result = results[stored_value_place.offset == tombstone_offset_k];
    });

    return result;
}

template <typename stored_key_>
template <typename stored_value_location_>
inline void grouped_write_buffer<stored_key_>::select(stored_value_location_ const& location,
                                                      stored_value_span_t stored_value,
                                                      notifier_t notifier) const noexcept {

    if (!location) {
        notifier.fail({status_code_t::invalid_argument_k, "A stored value location naming nothing"});
        return;
    }
    if (stored_value.size() < location.aligned_buffer_size()) {
        notifier.fail({status_code_t::invalid_argument_k, "A buffer shorter than the stored value"});
        return;
    }
    if (location.recipient_id != reinterpret_cast<std::uint64_t>(this)) {
        notifier.fail({status_code_t::invalid_argument_k, "A stored value location of another write buffer"});
        return;
    }
    if (location.recipient_state_id != 0) {
        notifier.fail({status_code_t::invalid_argument_k, "A stored value location of another buffer state"});
        return;
    }

    if (location.size_bytes > 0) {
        auto inserted = as_bytes_view(insertions_buffer).subspan(location.offset, location.size_bytes);
        std::memcpy(stored_value.data() + location.buffer_padding(), inserted.data(), inserted.size());
    }
    notifier.did_one();
}

template <typename stored_key_>
template <typename config_provider_>
inline status_t grouped_write_buffer<stored_key_>::reach_group_(group_id_t id, config_provider_ config_provider,
                                                                group_t*& group) noexcept {

    auto iterator = groups_by_id.find(id);
    if (iterator == groups_by_id.end()) {
        group_config_t config {};
        auto status = config_provider(id, config);
        if (status.failed()) return status;
        if ((config.value_size > 0) & (config.value_max_size > 0))
            return status_t {status_code_t::invalid_argument_k, "Invalid config"};

        group_t fresh {config, {{}, {}, constrained_allocator<std::byte> {resources_->memory_constraint}}, 0};
        if (groups_by_id.reserve_more(1) == groups_t::reserve_result_t::failed_k)
            return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
        groups_by_id.emplace(id, std::move(fresh), st::assume_reserved);
        iterator = groups_by_id.find(id);
        if (iterator == groups_by_id.end())
            return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
    }
    group = &iterator.value_ref();
    return status_t {};
}

template <typename stored_key_>
template <typename config_provider_>
inline status_t grouped_write_buffer<stored_key_>::add(group_id_t id, config_provider_ config_provider,
                                                       stored_key_t stored_key,
                                                       stored_value_view_t stored_value) noexcept {

    group_t* group = nullptr;
    auto status = reach_group_(id, config_provider, group);
    if (status.failed()) return status;

    status = check_stored_value(group->config, stored_key, stored_value.size());
    if (status.failed()) return status;

    if (!can_fit(1, stored_value.size())) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k, "Cache is full"};

    // The room for the key is taken before a byte of the value lands, so a refusal writes nothing.
    if (group->stored_values.reserve_more(1) == group_t::stored_values_t::reserve_result_t::failed_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another key"};

    auto inserted_offset = static_cast<stored_value_offset_in_buffer_t>(empty_stored_value_offset_k);
    if (!stored_value.empty()) {
        inserted_offset = next_inserted_offset;
        std::memcpy(insertions_buffer.data() + inserted_offset, stored_value.data(), stored_value.size());
        next_inserted_offset += stored_value.size();
    }

    stored_value_place_t const place {inserted_offset, static_cast<stored_value_size_t>(stored_value.size())};
    auto insert = [&](auto& unused_slot) noexcept {
        new (&unused_slot.key_ref()) stored_key_t {stored_key};
        new (&unused_slot.value_ref()) stored_value_place_t {place};
        group->elements_size_bytes += stored_value.size();
        ++elements_count_;
    };
    auto update = [&](auto& equal_slot) noexcept {
        group->elements_size_bytes -= equal_slot.value_ref().size;
        equal_slot.value_ref() = place;
        group->elements_size_bytes += stored_value.size();
    };
    [[maybe_unused]] auto const stored = group->stored_values.probe_to_upsert(stored_key, insert, update,
                                                                              st::assume_reserved);

    return status_t {};
}

template <typename stored_key_>
template <typename config_provider_, typename serializable_stored_value_>
inline status_t grouped_write_buffer<stored_key_>::add(group_id_t id, config_provider_ config_provider,
                                                       stored_key_t stored_key,
                                                       serializable_stored_value_ const& stored_value) noexcept {

    group_t* group = nullptr;
    auto status = reach_group_(id, config_provider, group);
    if (status.failed()) return status;

    auto const serialized_size = static_cast<stored_value_size_t>(storage_requirements(stored_value));
    status = check_stored_value(group->config, stored_key, serialized_size);
    if (status.failed()) return status;

    if (!can_fit(1, serialized_size)) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k, "Cache is full"};

    // The room for the key is taken before a byte of the value lands, so a refusal writes nothing.
    if (group->stored_values.reserve_more(1) == group_t::stored_values_t::reserve_result_t::failed_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another key"};

    status = save(stored_value, bytes_span_t {insertions_buffer.data() + next_inserted_offset, serialized_size});
    if (status.failed()) return status;

    stored_value_place_t const place {next_inserted_offset, serialized_size};
    next_inserted_offset += serialized_size;

    auto insert = [&](auto& unused_slot) noexcept {
        new (&unused_slot.key_ref()) stored_key_t {stored_key};
        new (&unused_slot.value_ref()) stored_value_place_t {place};
        group->elements_size_bytes += serialized_size;
        ++elements_count_;
    };
    auto update = [&](auto& equal_slot) noexcept {
        group->elements_size_bytes -= equal_slot.value_ref().size;
        equal_slot.value_ref() = place;
        group->elements_size_bytes += serialized_size;
    };
    [[maybe_unused]] auto const stored = group->stored_values.probe_to_upsert(stored_key, insert, update,
                                                                              st::assume_reserved);

    return status_t {};
}

template <typename stored_key_>
template <typename config_provider_>
inline status_t grouped_write_buffer<stored_key_>::mark_as_removed(group_id_t id, config_provider_ config_provider,
                                                                   stored_key_t stored_key) noexcept {

    group_t* group = nullptr;
    auto status = reach_group_(id, config_provider, group);
    if (status.failed()) return status;

    if (!can_fit(1, 0)) [[unlikely]]
        return status_t {status_code_t::capacity_exhausted_k, "Cache is full"};

    if (group->stored_values.reserve_more(1) == group_t::stored_values_t::reserve_result_t::failed_k)
        return status_t {status_code_t::out_of_memory_k, "No room for another key"};

    stored_value_place_t const place {tombstone_offset_k, 0};
    auto insert = [&](auto& unused_slot) noexcept {
        new (&unused_slot.key_ref()) stored_key_t {stored_key};
        new (&unused_slot.value_ref()) stored_value_place_t {place};
        ++elements_count_;
    };
    auto update = [&](auto& equal_slot) noexcept {
        group->elements_size_bytes -= equal_slot.value_ref().size;
        equal_slot.value_ref() = place;
    };
    [[maybe_unused]] auto const stored = group->stored_values.probe_to_upsert(stored_key, insert, update,
                                                                              st::assume_reserved);

    return status_t {};
}

template <typename stored_key_>
template <typename stored_value_location_>
inline status_t grouped_write_buffer<stored_key_>::find(group_id_t id, std::span<stored_key_t const> stored_keys,
                                                        std::span<stored_value_location_> locations,
                                                        missing_locations_t missing_locations) const {

    if (locations.size() < stored_keys.size())
        return status_t {status_code_t::invalid_argument_k, "Buffer is too small"};

    if (missing_locations == missing_locations_t::invalidate_k)
        for (std::size_t index = 0; index < stored_keys.size(); ++index)
            find<stored_value_location_>(id, stored_keys[index], locations[index]);
    else
        for (std::size_t index = 0; index < stored_keys.size(); ++index) {
            stored_value_location_ location;
            auto const found = find<stored_value_location_>(id, stored_keys[index], location);
            if (found != write_buffer_status_t::mismatch_k) locations[index] = location;
        }

    return status_t {};
}

template <typename stored_key_>
template <typename stored_value_location_>
inline void grouped_write_buffer<stored_key_>::select(std::span<stored_value_location_ const> locations,
                                                      bytes_span_t buffer, notifier_t notifier) const {

    for (std::size_t index = 0, offset = 0; (index < locations.size()) & !notifier.has_failed(); ++index) {
        auto const& location = locations[index];
        auto is_recipient = location.recipient_id == reinterpret_cast<std::uint64_t>(this);
        if (is_recipient) {
            if (location.size_bytes > 0) {
                auto stored_value = buffer.subspan(offset, location.aligned_buffer_size());
                select<stored_value_location_>(location, stored_value, notifier);
            }
            else notifier.did_one();
        }
        offset += location.aligned_buffer_size();
    }
}

template <typename stored_key_>
inline status_t grouped_write_buffer<stored_key_>::check_stored_value(
    group_config_t const& config, stored_key_t stored_key, stored_value_size_t stored_value_size) const noexcept {

    bool valid_key = stored_key != limit<stored_key_t>::max();
    bool valid_fixed_width_stored_value = (config.value_size > 0) & (stored_value_size == config.value_size);
    bool valid_variable_width_stored_value = stored_value_size <= config.value_max_size;
    bool valid = valid_key & (valid_fixed_width_stored_value ^ valid_variable_width_stored_value);
    return valid ? status_t {} : status_t {status_code_t::malformed_k, "Invalid stored value"};
}

template <typename stored_key_>
inline bool grouped_write_buffer<stored_key_>::can_fit(std::size_t stored_values_count,
                                                       std::size_t stored_values_sum_size) const noexcept {

    auto used_memory = next_inserted_offset;
    bool has_element_place = elements_count_ + stored_values_count <= elements_max_count_;
    bool has_enough_space = (used_memory + stored_values_sum_size <= capacity_bytes_) &
                            (used_memory + stored_values_sum_size <= empty_stored_value_offset_k);
    return has_element_place & has_enough_space;
}

template <typename stored_key_>
inline void grouped_write_buffer<stored_key_>::clear() noexcept {
    groups_by_id.clear();
    next_inserted_offset = 0;
    elements_count_ = 0;
}

#pragma endregion Grouped Write Buffer

#pragma region Grouped Read Set

/** The keys one transaction watches, kept apart per collection so two collections never collide. */
template <typename stored_key_>
class grouped_read_set {
  public:
    using stored_key_t = stored_key_;
    using group_id_t = collection_id_t;
    using read_set_t = read_set<stored_key_t>;
    using groups_t =
        st::hash_map<group_id_t, read_set_t, collection_id_hash_t, st::equal_to_t, constrained_allocator<std::byte>>;

    /** A set charging @p resources for the keys of every collection it watches. */
    explicit grouped_read_set(resources_shared_ptr_t resources) noexcept
        : resources(resources), groups({}, {}, constrained_allocator<std::byte> {resources->memory_constraint}) {}

    /** Whether the transaction watches nothing anywhere. */
    bool empty() const noexcept { return count() == 0; }

    /** The watched keys of every collection taken for every purpose in @p purpose. */
    std::size_t count(watch_purpose_t purpose = watch_purpose_t::all_k) const noexcept {
        std::size_t matching = 0;
        [[maybe_unused]] st::status_t const walked = groups.for_each(
            [&](auto const& entry) noexcept { matching += entry.value().count(purpose); });
        return matching;
    }

    /** The watched keys of @p id taken for every purpose in @p purpose. */
    std::size_t count(group_id_t id, watch_purpose_t purpose = watch_purpose_t::all_k) const noexcept {
        auto iterator = groups.find(id);
        return iterator == groups.end() ? 0 : iterator.value().count(purpose);
    }

    /** Makes room for @p count more keys of @p id, so a following @c add takes no allocation. */
    status_t reserve_more(group_id_t id, std::size_t count) noexcept {
        auto iterator = groups.find(id);
        if (iterator == groups.end()) {
            if (groups.reserve_more(1) == groups_t::reserve_result_t::failed_k)
                return {status_code_t::out_of_memory_k, "No room for another watched collection"};
            groups.emplace(id, read_set_t {resources}, st::assume_reserved);
            iterator = groups.find(id);
            if (iterator == groups.end())
                return {status_code_t::out_of_memory_k, "No room for another watched collection"};
        }
        return iterator.value_ref().reserve_more(count);
    }

    /** Watches @p stored_key of @p id from @p watch_stamp, in room @c reserve_more took. */
    void add(group_id_t id, stored_key_t stored_key, transaction_stamp_t watch_stamp, watch_purpose_t purpose,
             st::assume_reserved_t) noexcept {
        auto iterator = groups.find(id);
        if (iterator == groups.end()) return;
        iterator.value_ref().add(stored_key, watch_stamp, purpose, st::assume_reserved);
    }

    /** Drops every watch of every collection, for a transaction starting over. */
    void clear() noexcept { groups.clear(); }

    /** The budget every watched collection's keys are charged to. */
    resources_shared_ptr_t resources;

    /** One watched-key set per collection the transaction touched. */
    groups_t groups;
};

#pragma endregion Grouped Read Set

#pragma region Transaction State

/** Everything one database transaction carries between its start and its commit. */
template <typename stored_key_>
struct database_transaction_state {
    using stored_key_t = stored_key_;
    using write_buffer_t = grouped_write_buffer<stored_key_t>;
    using read_set_t = grouped_read_set<stored_key_t>;

    inline database_transaction_state(write_buffer_t&& write_buffer, resources_shared_ptr_t resources) noexcept
        : changes(std::move(write_buffer)), read_set(resources) {}

    /** Whether the transaction has neither written nor watched anything. */
    inline bool empty() const noexcept { return changes.empty() && read_set.empty(); }

    /** Drops both the writes and the watches, for a rollback or a renewal. */
    inline void clear() noexcept {
        changes.clear();
        read_set.clear();
    }

    /** The writes the transaction has not committed yet. */
    write_buffer_t changes;

    /** The keys the transaction watches for conflicts. */
    read_set_t read_set;
};

#pragma endregion Transaction State

#pragma endregion Database Transaction State

#pragma region Database Transaction Registry

template <typename stored_key_, typename store_type_>
class database_transaction;

#pragma region Store Interface

/**
 *  @brief What a registry needs from the store beneath it to stage and to commit.
 *
 *  Only the commit path is named here; the read accessors a transaction reaches the store through
 *  forward from members that instantiate when a caller uses them, so a store serving reads of one
 *  shape alone still satisfies this.
 */
template <typename store_type_, typename stored_key_type_>
concept transaction_store_concept = requires(store_type_& store, store_type_ const& constant, collection_id_t group_id,
                                             stored_key_type_ stored_key, stored_value_view_t stored_value,
                                             std::size_t count) {
    { store.acquire_write_path() } -> std::same_as<void>;
    { store.release_write_path() } -> std::same_as<void>;
    { constant.contains_collection(group_id) } -> std::same_as<bool>;
    { constant.collection_fits(group_id, count, count) } -> std::same_as<bool>;
    { store.flush_collection(group_id) } -> std::same_as<status_t>;
    { store.insert_in_collection(group_id, stored_key, stored_value) } -> std::same_as<status_t>;
    { store.remove_from_collection(group_id, stored_key) } -> std::same_as<status_t>;
};

#pragma endregion Store Interface

/**
 *  @brief The transactions of one database, the order they commit in and the conflicts that stop them.
 *  @tparam stored_key_ The key every collection of the store is addressed by.
 *  @tparam store_type_ The store the commits land in, which must satisfy @c transaction_store_concept.
 */
template <typename stored_key_, typename store_type_>
class database_transaction_registry {
  public:
#pragma region Types

    using stored_key_t = stored_key_;
    using store_t = store_type_;
    using transaction_t = database_transaction<stored_key_, store_type_>;
    using transaction_state_t = database_transaction_state<stored_key_t>;
    using write_buffer_t = typename transaction_state_t::write_buffer_t;
    using group_info_t = typename write_buffer_t::group_info_t;
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;
    using return_transaction_t = expected<transaction_unique_ptr_t>;

    /** The commit-record slots a registry keeps when its owner names none. */
    static constexpr std::size_t default_conflict_slots_k = 1ull << 16;

    /** The transactions a registry admits at once when its owner names no count. */
    static constexpr std::size_t default_transaction_slots_k = 256;

#pragma endregion Types

#pragma region Construction

    inline database_transaction_registry(store_t& store, resources_shared_ptr_t resources,
                                         std::size_t conflict_slots = default_conflict_slots_k,
                                         std::size_t transaction_slots = default_transaction_slots_k) noexcept
        : store_(&store), resources_(std::move(resources)), record_(conflict_slots, resources_->memory_constraint),
          roster_(transaction_slots, resources_->memory_constraint) {}

    inline database_transaction_registry(database_transaction_registry&&) = delete;
    inline database_transaction_registry(database_transaction_registry const&) = delete;
    inline database_transaction_registry& operator=(database_transaction_registry const&) = delete;
    inline database_transaction_registry& operator=(database_transaction_registry&&) = delete;
    inline ~database_transaction_registry() noexcept { assert(roster_.live() == 0 && "Transactions outlive registry"); }

#pragma endregion Construction

#pragma region Interface Methods

    /** Whether @p id names a transaction this registry holds. */
    inline bool has_transaction(transaction_id_t id) const noexcept { return roster_.contains(id); }

    /** The bytes the conflict record and the roster together occupy, which no commit ever changes. */
    inline std::size_t shared_state_bytes() const noexcept { return record_.size_bytes() + roster_.size_bytes(); }

    inline return_transaction_t create_transaction(transaction_id_t id, transaction_config_t const& config) noexcept;

#pragma endregion Interface Methods

  private:
#pragma region Transactions

    friend store_t;
    friend transaction_t;

    inline status_t stage_(transaction_id_t id, transaction_state_t& data,
                           transaction_stamp_t& try_to_stage_stamp) noexcept;
    inline status_t commit_(transaction_id_t id, transaction_state_t& data,
                            transaction_stamp_t& try_to_commit_stamp) noexcept;
    inline void discard_(transaction_id_t id) noexcept;

    inline transaction_stamp_t request_start_(transaction_id_t id) noexcept;
    inline void invalidate_(transaction_id_t id) noexcept { roster_.stop(id); }
    inline void notify_about_deletion_(transaction_id_t id) noexcept;

    /** The lowest stamp any later commit can claim, which scopes a watch taken now. */
    inline transaction_stamp_t current_stamp_() const noexcept { return record_.watch_stamp(); }

    inline status_t watch_now_(transaction_stamp_t start_stamp, collection_id_t group_id,
                               std::span<stored_key_t const> stored_keys) const noexcept;

#pragma endregion Transactions

#pragma region Helper Methods

    inline bool conflicting_(transaction_state_t const& data) const noexcept;
    inline void record_commit_(write_buffer_t const& changes, transaction_stamp_t commit_stamp) noexcept;
    inline status_t prepare_collections_(write_buffer_t const& changes) noexcept;
    inline status_t apply_(write_buffer_t const& changes) noexcept;

#pragma endregion Helper Methods

#pragma region Store Accessor Methods

    inline void lock_shared_() const noexcept { store_->lock_shared(); }
    inline void unlock_shared_() const noexcept { store_->unlock_shared(); }

    inline auto begin_(auto const& id) const noexcept { return store_->begin(id); }
    inline auto end_(auto const& id) const noexcept { return store_->end(id); }
    inline auto find_(auto const& id, stored_key_t stored_key) const noexcept { return store_->find(id, stored_key); }
    inline auto find_or_next_(auto const& id, stored_key_t stored_key) const noexcept {
        return store_->find_or_next(id, stored_key);
    }

    inline bool has_collection_(auto const& id) const noexcept { return store_->has_collection(id); }
    inline collection_id_t collection_id_(auto const& id) const noexcept { return store_->collection_id(id); }
    inline auto list_collections_() const noexcept { return store_->list_collections(); }
    inline status_t collection_config_(collection_id_t group_id, collection_config_t& config) const noexcept {
        return store_->collection_config(group_id, config);
    }

    inline bool contains_(auto const& id, stored_key_t stored_key) const noexcept {
        return store_->contains(id, stored_key);
    }
    inline status_t stored_value_size_(auto const& id, stored_key_t stored_key,
                                       stored_value_size_t& size) const noexcept {
        return store_->stored_value_size(id, stored_key, size);
    }
    inline stored_value_size_t buffer_size_for_any_stored_value_(auto const& id) const noexcept {
        return store_->buffer_size_for_any_stored_value(id);
    }
    template <typename allocator_>
    inline auto create_stored_value_locations_(auto const& id, std::size_t count) const noexcept {
        return store_->template create_stored_value_locations<allocator_>(id, count);
    }

    inline status_t select_(auto const& id, stored_key_t stored_key, stored_value_span_t stored_value,
                            stored_value_size_t& size) const noexcept {
        return store_->select(id, stored_key, stored_value, size);
    }
    inline status_t find_(auto const& id, std::span<stored_key_t const> stored_keys, auto stored_value_locations,
                          missing_locations_t missing = missing_locations_t::invalidate_k) const noexcept {
        return store_->find(id, stored_keys, stored_value_locations, missing);
    }
    inline status_t select_(auto const& id, auto stored_value_locations, bytes_span_t buffer,
                            notifier_t notifier) const noexcept {
        return store_->select(id, stored_value_locations, buffer, notifier);
    }

#pragma endregion Store Accessor Methods

#pragma region Private Members

    /** The store every commit of this registry lands in, owned by whoever named it. */
    store_t* store_;

    /** The budget the record, the roster and every transaction's writes are charged to. */
    resources_shared_ptr_t resources_;

    /** The order every transaction is placed in, and the last commit to write each key. */
    commit_record_t record_;

    /** The live transactions and the staging queue pair. */
    transaction_roster_t roster_;

#pragma endregion Private Members
};

#pragma region Interface Methods

template <typename stored_key_, typename store_type_>
inline typename database_transaction_registry<stored_key_, store_type_>::return_transaction_t
database_transaction_registry<stored_key_, store_type_>::create_transaction(
    transaction_id_t id, transaction_config_t const& config) noexcept {

    status_t const enrolled = roster_.enroll(id);
    if (enrolled.failed()) return enrolled;

    write_buffer_t changes(config.write_buffer_max_bytes, config.write_buffer_max_elements, resources_);
    transaction_unique_ptr_t transaction {new (std::nothrow) transaction_t {id, *this, std::move(changes), resources_}};
    if (!transaction) {
        roster_.retire(id);
        return status_t {status_code_t::out_of_memory_k, "No room for another transaction"};
    }
    return transaction;
}

#pragma endregion Interface Methods

#pragma region Transactions

template <typename stored_key_, typename store_type_>
inline status_t database_transaction_registry<stored_key_, store_type_>::stage_(
    transaction_id_t id, transaction_state_t& data, transaction_stamp_t& try_to_stage_stamp) noexcept {

    roster_.claim_staging(id);
    store_->acquire_write_path();
    try_to_stage_stamp = record_.watch_stamp();

    // A transaction that neither wrote nor watched anything stages by having nothing to check.
    if (data.empty()) return status_t {};

    if (conflicting_(data)) {
        discard_(id);
        return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    }

    // Without writes, the absence of watched key collisions is the whole of the stage.
    if (data.changes.empty()) return status_t {};

    status_t const prepared = prepare_collections_(data.changes);
    if (prepared.failed()) discard_(id);
    return prepared;
}

template <typename stored_key_, typename store_type_>
inline status_t database_transaction_registry<stored_key_, store_type_>::commit_(
    transaction_id_t id, transaction_state_t& data, transaction_stamp_t& try_to_commit_stamp) noexcept {

    if (!roster_.staged_by(id)) {
        // Staging inside the commit makes both stamps equal, so a staged commit reports one instant.
        transaction_stamp_t staged_stamp = invalid_transaction_stamp_k;
        status_t const staged = stage_(id, data, staged_stamp);
        if (staged.failed()) {
            // The changes stay exactly as they were, so a caller can still log what it could not commit.
            try_to_commit_stamp = record_.watch_stamp();
            return staged;
        }
    }

    transaction_stamp_t const commit_stamp = record_.issue_stamp();
    try_to_commit_stamp = commit_stamp;

    status_t status = status_t {};
    if (!data.changes.empty()) {
        // The warning is published before the writes it warns about, so a reader never sees the
        // second without the first, and a refused write leaves a conflict that never happened.
        record_commit_(data.changes, commit_stamp);
        status = apply_(data.changes);
    }

    // Only now may a transaction start after this commit, because only now has all of it landed.
    record_.publish_stamp(commit_stamp);
    discard_(id);
    if (status.succeeded()) invalidate_(id);
    return status;
}

template <typename stored_key_, typename store_type_>
inline void database_transaction_registry<stored_key_, store_type_>::discard_(
    [[maybe_unused]] transaction_id_t id) noexcept {

    assert(roster_.staged_by(id) && "The transaction is not in committing state");
    store_->release_write_path();
    roster_.release_staging();
}

template <typename stored_key_, typename store_type_>
inline transaction_stamp_t database_transaction_registry<stored_key_, store_type_>::request_start_(
    transaction_id_t id) noexcept {

    // A start consumes no stamp: it only has to order before every commit that follows it.
    transaction_stamp_t const start_stamp = record_.watch_stamp();
    roster_.start(id, start_stamp);
    return start_stamp;
}

template <typename stored_key_, typename store_type_>
inline void database_transaction_registry<stored_key_, store_type_>::notify_about_deletion_(
    transaction_id_t id) noexcept {

    if (roster_.staged_by(id)) discard_(id);
    roster_.stop(id);
    roster_.retire(id);
}

template <typename stored_key_, typename store_type_>
inline status_t database_transaction_registry<stored_key_, store_type_>::watch_now_(
    transaction_stamp_t start_stamp, collection_id_t group_id,
    std::span<stored_key_t const> stored_keys) const noexcept {

    for (stored_key_t const& stored_key : stored_keys)
        if (record_.conflicts(watched_key_fingerprint(group_id, stored_key), start_stamp))
            return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    return status_t {};
}

#pragma endregion Transactions

#pragma region Helper Methods

template <typename stored_key_, typename store_type_>
inline bool database_transaction_registry<stored_key_, store_type_>::conflicting_(
    transaction_state_t const& data) const noexcept {

    bool conflict = false;
    [[maybe_unused]] st::status_t const walked = data.read_set.groups.for_each(
        [&](auto const& group) noexcept -> st::walk_control_t {
            collection_id_t const group_id = group.key();
            [[maybe_unused]] st::status_t const watched = group.value().stored_keys.for_each(
                [&](auto const& watch) noexcept -> st::walk_control_t {
                    conflict = record_.conflicts(watched_key_fingerprint(group_id, watch.key()),
                                                 watch.value().watch_stamp);
                    return conflict ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
                });
            return conflict ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
        });
    return conflict;
}

template <typename stored_key_, typename store_type_>
inline void database_transaction_registry<stored_key_, store_type_>::record_commit_(
    write_buffer_t const& changes, transaction_stamp_t commit_stamp) noexcept {

    [[maybe_unused]] st::status_t const walked = changes.groups_by_id.for_each([&](auto const& group) noexcept {
        collection_id_t const group_id = group.key();
        [[maybe_unused]] st::status_t const written = group.value().stored_values.for_each(
            [&](auto const& entry) noexcept {
                record_.record(watched_key_fingerprint(group_id, entry.key()), commit_stamp);
            });
    });
}

template <typename stored_key_, typename store_type_>
inline status_t database_transaction_registry<stored_key_, store_type_>::prepare_collections_(
    write_buffer_t const& changes) noexcept {

    status_t status = status_t {};
    changes.for_each_group([&](group_info_t const& group) noexcept {
        if (status.failed()) return;
        if (!store_->contains_collection(group.id)) {
            status = status_t {status_code_t::write_conflict_k, "A collection the transaction wrote is gone"};
            return;
        }
        if (!store_->collection_fits(group.id, group.elements_count, group.elements_size_bytes))
            status = store_->flush_collection(group.id);
    });
    return status;
}

template <typename stored_key_, typename store_type_>
inline status_t database_transaction_registry<stored_key_, store_type_>::apply_(
    write_buffer_t const& changes) noexcept {

    status_t status = status_t {};
    [[maybe_unused]] st::status_t const walked = changes.groups_by_id.for_each(
        [&](auto const& group) noexcept -> st::walk_control_t {
            collection_id_t const group_id = group.key();
            [[maybe_unused]] st::status_t const applied = group.value().stored_values.for_each(
                [&](auto const& entry) noexcept -> st::walk_control_t {
                    auto const& place = entry.value();
                    if (place.offset == tombstone_offset_k)
                        status = store_->remove_from_collection(group_id, entry.key());
                    else if (place.offset == empty_stored_value_offset_k)
                        status = store_->insert_in_collection(group_id, entry.key(), stored_value_view_t {});
                    else
                        status = store_->insert_in_collection(
                            group_id, entry.key(),
                            as_bytes_view(changes.insertions_buffer).subspan(place.offset, place.size));
                    return status.failed() ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
                });
            return status.failed() ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
        });
    return status;
}

#pragma endregion Helper Methods

#pragma endregion Database Transaction Registry

#pragma region Database Snapshot

#pragma region Snapshot

/**
 *  @brief The level set of every collection of one database, held together against publication.
 *  @tparam stored_key_ The key every collection of the database is addressed by.
 *  @tparam volume_type_ The volume every run of every held collection lives on.
 */
template <typename stored_key_, typename volume_type_>
class database_snapshot {
  public:

#pragma region Types

    /** The key every collection of the database is addressed by. */
    using stored_key_t = stored_key_;

    /** The volume every run of every held collection lives on. */
    using volume_t = volume_type_;

    /** A collection named by its ID or by its name, which is how every call addresses one. */
    using any_collection_id_t = std::variant<collection_id_t, collection_alphanumeric_id_t>;

    /** One held level set of a collection whose values follow @p value_layout_. */
    template <value_layout_t value_layout_>
    using collection_snapshot_of = collection_snapshot<key_slot<stored_key_t, value_layout_>, volume_t>;

    /** One held level set, of whichever value layout its collection was created with. */
    using any_collection_snapshot_t = std::variant<collection_snapshot_of<value_layout_t::inline_1_byte_k>,
                                                   collection_snapshot_of<value_layout_t::inline_4_bytes_k>,
                                                   collection_snapshot_of<value_layout_t::inline_8_bytes_k>,
                                                   collection_snapshot_of<value_layout_t::inline_16_bytes_k>,
                                                   collection_snapshot_of<value_layout_t::inline_32_bytes_k>,
                                                   collection_snapshot_of<value_layout_t::inline_64_bytes_k>,
                                                   collection_snapshot_of<value_layout_t::fixed_width_k>,
                                                   collection_snapshot_of<value_layout_t::variable_width_k>>;

    /** One collection of the snapshot: the names it answers to, and the level set it holds. */
    struct held_collection_t {

        /** The UUID the database addresses the collection by. */
        collection_id_t id = collection_invalid_id_k;

        /** The name the database addresses the collection by. */
        string_t name;

        /** The level set held, which nothing may retire while this snapshot lives. */
        any_collection_snapshot_t levels;
    };

#pragma endregion Types

#pragma region Construction

    database_snapshot() noexcept = default;
    database_snapshot(database_snapshot const&) = delete;
    database_snapshot& operator=(database_snapshot const&) = delete;
    database_snapshot(database_snapshot&&) noexcept = default;
    database_snapshot& operator=(database_snapshot&&) noexcept = default;

    /** Makes room for @p count collections, so no hold is taken that a later one cannot be tracked beside. */
    status_t reserve(std::size_t count) noexcept {
        if (held_.reserve(count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A snapshot could not track that many collections"};
        return {};
    }

    /**
     *  @brief Takes @p levels in under @p collection_id and @p name, in room @c reserve took.
     *  @param[in] collection_id The UUID the database addresses the collection by.
     *  @param[in] name The name the database addresses the collection by.
     *  @param[in] levels The level set already held, moved in and released when the snapshot dies.
     */
    status_t hold(collection_id_t collection_id, string_view_t name, any_collection_snapshot_t&& levels) noexcept {
        held_collection_t taken;
        taken.id = collection_id;
        if (sz::failed(taken.name.assign(name)))
            return {status_code_t::out_of_memory_k, "A snapshot could not hold a collection's name"};
        taken.levels = std::move(levels);
        if (held_.push_back(std::move(taken)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A snapshot could not track another collection"};
        return {};
    }

#pragma endregion Construction

#pragma region Shape

    /** Collections this snapshot holds a level set of. */
    [[nodiscard]] std::size_t size() const noexcept { return held_.size(); }

    /** Whether @p id names a collection this snapshot holds. */
    [[nodiscard]] bool contains(any_collection_id_t const& id) const noexcept { return held_at_(id) != nullptr; }

    /** The collection at @p index, in the order the snapshot took them. */
    [[nodiscard]] held_collection_t const& at(std::size_t index) const noexcept { return held_[index]; }

    /** Key slots every level of every held collection carries together. */
    [[nodiscard]] std::uint64_t population_count() const noexcept {
        std::uint64_t counted = 0;
        for (std::size_t index = 0; index != held_.size(); ++index)
            counted += std::visit([](auto const& levels) noexcept { return levels.population_count(); },
                                  held_[index].levels);
        return counted;
    }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether the held levels of @p id carry @p stored_key, and how many bytes its value takes.
     *  @param[in] id The collection to look in, named by its ID or by its name.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c success_k.
     */
    [[nodiscard]] select_result_t find(any_collection_id_t const& id, stored_key_t stored_key,
                                       stored_value_size_t& size) const noexcept {
        size = 0;
        held_collection_t const* const found = held_at_(id);
        if (!found) return select_result_t::nothing_k;
        return std::visit([&](auto const& levels) noexcept { return levels.find(stored_key, size); }, found->levels);
    }

    /**
     *  @brief Copies the value @p id held for @p stored_key into @p into, which needs only the value's bytes.
     *  @param[inout] channel The channel the read is submitted and completed on, held by the calling thread.
     *  @param[in] id The collection to read from, named by its ID or by its name.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     */
    status_t select(io_channel_t& channel, any_collection_id_t const& id, stored_key_t stored_key,
                    std::span<byte_t> into, stored_value_size_t& size) const noexcept {
        size = 0;
        held_collection_t const* const found = held_at_(id);
        if (!found) return {status_code_t::not_found_k, "This snapshot holds no collection of that name"};
        return std::visit([&](auto const& levels) noexcept { return levels.select(channel, stored_key, into, size); },
                          found->levels);
    }

#pragma endregion Lookups

  private:

#pragma region Private Methods

    /** The collection @p id names, or null when the snapshot holds none. */
    [[nodiscard]] held_collection_t const* held_at_(any_collection_id_t const& id) const noexcept {
        if (collection_id_t const* const identified = std::get_if<collection_id_t>(&id)) {
            for (std::size_t index = 0; index != held_.size(); ++index)
                if (held_[index].id == *identified) return &held_[index];
            return nullptr;
        }
        collection_alphanumeric_id_t const& named = std::get<collection_alphanumeric_id_t>(id);
        for (std::size_t index = 0; index != held_.size(); ++index)
            if (held_[index].name == named) return &held_[index];
        return nullptr;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** Every collection the snapshot holds, in the order it took them. */
    st::basic_vector<held_collection_t> held_;

#pragma endregion Private Members
};

#pragma endregion Snapshot

#pragma endregion Database Snapshot

#pragma region Database Transaction

#pragma region Transaction

/**
 *  @brief A group of writes and reads over several collections that succeed or fail together.
 *  @tparam stored_key_ The key every collection of the store is addressed by.
 *  @tparam store_type_ The store the writes land in, which must satisfy @c transaction_store_concept.
 *
 *  Nothing here throws: a mutation that outgrows the write buffer answers @c capacity_exhausted_k, and
 *  the caller may commit what it has and start over. The object is reusable after @c commit, after
 *  @c rollback and after @c renew.
 */
template <typename stored_key_, typename store_type_>
class database_transaction {
  public:

#pragma region Types

    /** The key every collection of the store is addressed by. */
    using stored_key_t = stored_key_;

    /** The store every committed write of this transaction lands in. */
    using store_t = store_type_;

    /** The registry that orders this transaction against every other of the same store. */
    using registry_t = database_transaction_registry<stored_key_t, store_t>;

    /** The uncommitted writes and the watched keys this transaction carries. */
    using state_t = database_transaction_state<stored_key_t>;

    /** The uncommitted writes alone, grouped by the collection they belong to. */
    using write_buffer_t = typename state_t::write_buffer_t;

    /** A collection named by its ID or by its name, which is how every call addresses one. */
    using any_collection_id_t = typename store_t::any_collection_id_t;

#pragma endregion Types

#pragma region Construction

    database_transaction(database_transaction const&) = delete;
    database_transaction(database_transaction&&) = delete;
    database_transaction& operator=(database_transaction const&) = delete;
    database_transaction& operator=(database_transaction&&) = delete;

    /**
     *  @brief A transaction of @p id, started at the stamp @p manager hands it.
     *  @param[in] id The identifier the registry enrolled, which this transaction holds until it dies.
     *  @param[inout] manager The registry that orders and conflict-checks it, outliving it.
     *  @param[in] changes The write buffer it fills, already sized to its configuration.
     *  @param[in] resources The budget its watched keys are charged to.
     */
    database_transaction(transaction_id_t id, registry_t& manager, write_buffer_t&& changes,
                         resources_shared_ptr_t resources) noexcept
        : manager_(manager), data_(std::move(changes), std::move(resources)), id_(id),
          start_stamp_(manager.request_start_(id)), phase_(phase_t::started_k) {}

    ~database_transaction() noexcept { manager_.notify_about_deletion_(id_); }

#pragma endregion Construction

#pragma region Lifecycle

    /**
     *  @brief Checks every watched key and takes the store's write path, without applying anything.
     *
     *  A failure keeps the changes, so a caller can still log what it could not stage, and a success
     *  holds the write path until @c commit or @c rollback hands it back.
     */
    status_t stage() noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        transaction_stamp_t staged_stamp = invalid_transaction_stamp_k;
        status_t const staged = manager_.stage_(id_, data_, staged_stamp);
        if (staged.succeeded()) {
            last_stage_stamp_ = staged_stamp;
            phase_ = phase_t::staged_k;
        }
        return staged;
    }

    /**
     *  @brief Stages what is not staged yet and applies every write of this transaction.
     *
     *  @note The write path is handed back whichever way this ends, so the transaction is started
     *      again on return; only a success clears its changes and its watches.
     */
    status_t commit() noexcept {
        status_t const committed = manager_.commit_(id_, data_, last_try_to_commit_stamp_);
        phase_ = phase_t::started_k;
        if (committed.failed()) return committed;

        last_stage_stamp_ = last_try_to_commit_stamp_;
        last_commit_stamp_ = last_try_to_commit_stamp_;
        data_.clear();
        start_stamp_ = manager_.request_start_(id_);
        return {};
    }

    /** Drops every write and every watch, and starts the transaction over from this moment. */
    void rollback() noexcept {
        if (phase_ == phase_t::staged_k) manager_.discard_(id_);
        phase_ = phase_t::started_k;
        data_.clear();
        start_stamp_ = manager_.request_start_(id_);
    }

    /**
     *  @brief Starts over from this moment while keeping the writes, which only a write-only transaction may.
     *  @return @c invalid_argument_k when the transaction has read anything, whose watches cannot move.
     */
    status_t renew() noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (data_.read_set.count(watch_purpose_t::read_k) != 0)
            return {status_code_t::invalid_argument_k, "That transaction has read something"};
        manager_.invalidate_(id_);
        start_stamp_ = manager_.request_start_(id_);
        return {};
    }

    /**
     *  @brief Watches @p stored_keys of @p id for conflicts, without reading or writing any of them.
     *  @param[in] id The collection the keys belong to, named by its ID or by its name.
     *  @param[in] stored_keys The keys to watch, which are dropped by a commit and by a rollback.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t watch_more(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys) noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};

        if (status_t const prepared = watch_now_or_prepare_for_later_<watch_schedule_>(collection_id, stored_keys);
            prepared.failed())
            return prepared;
        watch_later_<watch_schedule_>(collection_id, stored_keys, watch_stamp_for_<watch_schedule_>(),
                                      watch_purpose_t::track_k);
        return {};
    }

#pragma endregion Lifecycle

#pragma region Catalog

    /** Whether the store names the collection @p id. */
    [[nodiscard]] bool has_collection(any_collection_id_t const& id) const noexcept {
        if (!idle_()) return false;
        return manager_.has_collection_(id);
    }

    /** The ID @p id resolves to, or @c collection_invalid_id_k when it names nothing. */
    [[nodiscard]] collection_id_t collection_id(any_collection_id_t const& id) const noexcept {
        if (!idle_()) return collection_invalid_id_k;
        return resolve_(id).id;
    }

    /** The name and the number of every collection of the store. */
    [[nodiscard]] st::basic_vector<collection_ids_t> list_collections() const noexcept {
        if (!idle_()) return {};
        return manager_.list_collections_();
    }

    /** Bytes the largest value @p id admits takes, which is what a caller sizes a read buffer by. */
    [[nodiscard]] stored_value_size_t buffer_size_for_any_stored_value(any_collection_id_t const& id) const noexcept {
        if (!idle_()) return 0;
        return manager_.buffer_size_for_any_stored_value_(id);
    }

    /** Whether @p count more writes of @p bytes still fit in this transaction's own buffer. */
    [[nodiscard]] bool has_available_space_for(std::size_t count, std::size_t bytes) const noexcept {
        return data_.changes.can_fit(count, bytes);
    }

#pragma endregion Catalog

#pragma region Writes

    /** Writes @p stored_value under @p stored_key of @p id into this transaction's own buffer. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t insert(any_collection_id_t const& id, stored_key_t stored_key, stored_value_view_t stored_value) noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        resolved_collection_t const named = resolve_(id);
        collection_id_t const collection_id = named.id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};

        std::array<stored_key_t, 1> const alone {stored_key};
        std::span<stored_key_t const> const watched {alone};
        if (status_t const prepared = watch_now_or_prepare_for_later_<watch_schedule_>(collection_id, watched);
            prepared.failed())
            return prepared;

        transaction_stamp_t const from = watch_stamp_for_<watch_schedule_>();
        status_t const written = data_.changes.add(collection_id, group_config_of_(named.config), stored_key,
                                                   stored_value);
        if (written.succeeded()) watch_later_<watch_schedule_>(collection_id, watched, from, watch_purpose_t::write_k);
        return written;
    }

    /** Writes a removal of @p stored_key of @p id into this transaction's own buffer. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t remove(any_collection_id_t const& id, stored_key_t stored_key) noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        resolved_collection_t const named = resolve_(id);
        collection_id_t const collection_id = named.id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};

        std::array<stored_key_t, 1> const alone {stored_key};
        std::span<stored_key_t const> const watched {alone};
        if (status_t const prepared = watch_now_or_prepare_for_later_<watch_schedule_>(collection_id, watched);
            prepared.failed())
            return prepared;

        transaction_stamp_t const from = watch_stamp_for_<watch_schedule_>();
        status_t const written = data_.changes.mark_as_removed(collection_id, group_config_of_(named.config),
                                                               stored_key);
        if (written.succeeded()) watch_later_<watch_schedule_>(collection_id, watched, from, watch_purpose_t::write_k);
        return written;
    }

#pragma endregion Writes

#pragma region Lookups

    /** Whether this transaction or the store carries @p stored_key of @p id. */
    [[nodiscard]] bool contains(any_collection_id_t const& id, stored_key_t stored_key) const noexcept {
        return contains_uncommitted(id, stored_key) || contains_committed(id, stored_key);
    }

    /** Bytes the value of @p stored_key takes, as this transaction would see it. */
    status_t stored_value_size(any_collection_id_t const& id, stored_key_t stored_key,
                               stored_value_size_t& size) const noexcept {
        status_t const staged = stored_value_size_uncommitted(id, stored_key, size);
        if (staged.succeeded() || !(staged == status_code_t::not_found_k)) return staged;
        return stored_value_size_committed(id, stored_key, size);
    }

    /** Copies the value of @p stored_key into @p into, taking this transaction's own write first. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t select(any_collection_id_t const& id, stored_key_t stored_key, stored_value_span_t into,
                    stored_value_size_t& size) const noexcept {
        status_t const staged = select_uncommitted(id, stored_key, into, size);
        if (staged.succeeded()) return staged;
        return select_committed<watch_schedule_>(id, stored_key, into, size);
    }

    /** Whether this transaction's own buffer carries @p stored_key of @p id. */
    [[nodiscard]] bool contains_uncommitted(any_collection_id_t const& id, stored_key_t stored_key) const noexcept {
        if (!idle_()) return false;
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k) return false;
        return data_.changes.status(collection_id, stored_key) == write_buffer_status_t::found_k;
    }

    /** Bytes the value this transaction wrote for @p stored_key takes. */
    status_t stored_value_size_uncommitted(any_collection_id_t const& id, stored_key_t stored_key,
                                           stored_value_size_t& size) const noexcept {
        if (!idle_()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        return data_.changes.stored_value_size(collection_id, stored_key, size);
    }

    /** Copies the value this transaction wrote for @p stored_key into @p into. */
    status_t select_uncommitted(any_collection_id_t const& id, stored_key_t stored_key, stored_value_span_t into,
                                stored_value_size_t& size) const noexcept {
        if (!idle_()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};
        return data_.changes.select(collection_id, stored_key, into, size);
    }

    /** Whether the store carries @p stored_key of @p id, ignoring what this transaction wrote. */
    [[nodiscard]] bool contains_committed(any_collection_id_t const& id, stored_key_t stored_key) const noexcept {
        if (!idle_()) return false;
        return manager_.contains_(id, stored_key);
    }

    /** Bytes the store holds for @p stored_key of @p id, ignoring what this transaction wrote. */
    status_t stored_value_size_committed(any_collection_id_t const& id, stored_key_t stored_key,
                                         stored_value_size_t& size) const noexcept {
        if (!idle_()) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        return manager_.stored_value_size_(id, stored_key, size);
    }

    /**
     *  @brief Reads @p stored_key of @p id out of the store and watches it for conflicts.
     *  @param[in] id The collection to read from, named by its ID or by its name.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     *
     *  @note The read runs before the watch is taken, so a key the store refuses is never watched.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t select_committed(any_collection_id_t const& id, stored_key_t stored_key, stored_value_span_t into,
                              stored_value_size_t& size) const noexcept {
        if (phase_ != phase_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        collection_id_t const collection_id = resolve_(id).id;
        if (collection_id == collection_invalid_id_k)
            return {status_code_t::not_found_k, "The store names no such collection"};

        transaction_stamp_t const from = watch_stamp_for_<watch_schedule_>();
        if (status_t const read = manager_.select_(collection_id, stored_key, into, size); read.failed()) return read;

        std::array<stored_key_t, 1> const alone {stored_key};
        std::span<stored_key_t const> const watched {alone};
        if (status_t const prepared = watch_now_or_prepare_for_later_<watch_schedule_>(collection_id, watched);
            prepared.failed())
            return prepared;
        watch_later_<watch_schedule_>(collection_id, watched, from, watch_purpose_t::read_k);
        return {};
    }

#pragma endregion Lookups

#pragma region Stamps

    /** The stamp this transaction was started or restarted at, which scopes every watch it takes. */
    [[nodiscard]] transaction_stamp_t start_stamp() const noexcept { return start_stamp_; }

    /** The stamp this transaction last staged at, or @c invalid_transaction_stamp_k while it never has. */
    [[nodiscard]] transaction_stamp_t last_stage_stamp() const noexcept { return last_stage_stamp_; }

    /** The stamp this transaction last committed at, or @c invalid_transaction_stamp_k while it never has. */
    [[nodiscard]] transaction_stamp_t last_commit_stamp() const noexcept { return last_commit_stamp_; }

    /** The stamp this transaction last tried to commit at, whether or not that commit landed. */
    [[nodiscard]] transaction_stamp_t last_try_to_commit_stamp() const noexcept { return last_try_to_commit_stamp_; }

#pragma endregion Stamps

  private:

#pragma region Private Types

    /** Whether this transaction is filling its buffer or holding the store's write path. */
    enum class phase_t : std::uint8_t {

        /** The transaction takes reads and writes, and holds nothing of the store. */
        started_k,

        /** The transaction has checked its watches and holds the store's write path. */
        staged_k,
    };

    /** The value widths one collection admits, which every write of it is checked against. */
    using group_config_t = typename write_buffer_t::group_config_t;

    /** What one lookup of a collection answered, which the next lookup of the same one reuses. */
    struct resolved_collection_t {

        /** The ID the collection answers to, invalid when the store names no such collection. */
        collection_id_t id = collection_invalid_id_k;

        /** The value widths that collection admits, which every write of it is checked against. */
        group_config_t config {};

        /** The catalog version the answer was read at, which a create and a drop both move past. */
        std::uint64_t catalog_version = 0;
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** Whether this transaction is filling its buffer rather than holding the store's write path. */
    [[nodiscard]] bool idle_() const noexcept { return phase_ == phase_t::started_k; }

    /** The store this transaction reads and commits into, which the registry beside it names. */
    [[nodiscard]] store_t& store_() const noexcept { return *manager_.store_; }

    /**
     *  @brief What @p id resolves to, taken from the last answer while the catalog has not moved.
     *  @param[in] id The collection to resolve, named by its ID or by its name.
     *  @return Its ID and its widths, the ID invalid when the store names no such collection.
     *
     *  @note One catalog lookup answers both, and a run of calls over one collection takes one lookup
     *      rather than one per key; a create or a drop moves the version and the next call reads again.
     */
    [[nodiscard]] resolved_collection_t resolve_(any_collection_id_t const& id) const noexcept {
        std::uint64_t const version = store_().catalog_version();
        collection_id_t const* const identified = std::get_if<collection_id_t>(&id);
        if (identified != nullptr && resolved_.id != collection_invalid_id_k && *identified == resolved_.id &&
            version == resolved_.catalog_version)
            return resolved_;

        collection_id_t collection_id = collection_invalid_id_k;
        collection_config_t shape;
        if (store_().resolve_collection(id, collection_id, shape).failed()) return resolved_collection_t {};
        resolved_.id = collection_id;
        resolved_.config = group_config_t {shape.lsm_tree.lsm_level_0.sorted_run.value_size,
                                           shape.lsm_tree.lsm_level_0.sorted_run.value_max_size};
        resolved_.catalog_version = version;
        return resolved_;
    }

    /** Answers the write buffer with @p widths, which the caller resolved in the same lookup. */
    [[nodiscard]] static auto group_config_of_(group_config_t widths) noexcept {
        return [widths](collection_id_t, group_config_t& config) noexcept {
            config = widths;
            return status_t {};
        };
    }

    /** The stamp a watch taken now is scoped from, which the schedule alone decides. */
    template <watch_schedule_t watch_schedule_>
    [[nodiscard]] transaction_stamp_t watch_stamp_for_() const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k) return start_stamp_;
        else if constexpr (watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return manager_.current_stamp_();
        else return invalid_transaction_stamp_k;
    }

    /** Checks @p stored_keys right away, or makes the room a later watch of them needs. */
    template <watch_schedule_t watch_schedule_>
    status_t watch_now_or_prepare_for_later_(collection_id_t collection_id,
                                             std::span<stored_key_t const> stored_keys) const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::now_k)
            return manager_.watch_now_(start_stamp_, collection_id, stored_keys);
        else if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k ||
                           watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return data_.read_set.reserve_more(collection_id, stored_keys.size());
        else return {};
    }

    /** Records every key of @p stored_keys as watched from @p from, in room the step above took. */
    template <watch_schedule_t watch_schedule_>
    void watch_later_(collection_id_t collection_id, std::span<stored_key_t const> stored_keys,
                      transaction_stamp_t from, watch_purpose_t purpose) const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k ||
                      watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            for (stored_key_t const& stored_key : stored_keys)
                data_.read_set.add(collection_id, stored_key, from, purpose, st::assume_reserved);
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The registry that orders this transaction and checks its watches, outliving it. */
    registry_t& manager_;

    /** The writes and the watches this transaction carries, which a read of it also fills. */
    state_t mutable data_;

    /** The collection the last lookup answered, which the next lookup of the same one reuses. */
    resolved_collection_t mutable resolved_;

    /** The identifier the registry enrolled this transaction under. */
    transaction_id_t id_ = invalid_transaction_id_k;

    /** The stamp this transaction was started or restarted at. */
    transaction_stamp_t start_stamp_ = invalid_transaction_stamp_k;

    /** The stamp this transaction last staged at. */
    transaction_stamp_t last_stage_stamp_ = invalid_transaction_stamp_k;

    /** The stamp this transaction last committed at. */
    transaction_stamp_t last_commit_stamp_ = invalid_transaction_stamp_k;

    /** The stamp this transaction last tried to commit at. */
    transaction_stamp_t last_try_to_commit_stamp_ = invalid_transaction_stamp_k;

    /** Whether this transaction is filling its buffer or holding the store's write path. */
    phase_t phase_ = phase_t::started_k;

#pragma endregion Private Members
};

#pragma endregion Transaction

#pragma endregion Database Transaction

#pragma region Database

#pragma region Schema

/** The layout the catalog blob follows, which a database of another version refuses to open. */
inline constexpr std::uint64_t database_schema_version_k = 2;

/** @p text as the bytes a volume names a blob by, without its terminator. */
template <std::size_t length_>
[[nodiscard]] inline blob_name_t blob_name_of(char const (&text)[length_]) noexcept {
    return blob_name_t {reinterpret_cast<byte_t const*>(text), length_ - 1};
}

/** The name the catalog of a database is stored under, on whichever volume placement gave it. */
[[nodiscard]] inline blob_name_t database_schema_name() noexcept { return blob_name_of("ustore.database.schema"); }

/** The name the caller's own bytes are stored under, which the engine never reads. */
[[nodiscard]] inline blob_name_t database_user_data_name() noexcept { return blob_name_of("ustore.database.user"); }

/** Bytes the longest collection name takes, which a record holds inline rather than pointing at. */
inline constexpr std::size_t collection_name_max_bytes_k = 64;

/**
 *  @brief One collection as the catalog blob names it, which is all a mount needs to open it again.
 *
 *  Every field is stored as it lies, so the image is the object representation and no field of it is
 *  ever read through a pointer the layout did not align.
 */
struct collection_record_t {

    /** The UUID the catalog, every commit and the C ABI address the collection by. */
    collection_id_t id = collection_invalid_id_k;

    /** Bytes level zero admits before a compaction is due. */
    std::uint64_t first_level_max_bytes = 0;

    /** How fast the bytes a level admits grow with its number. */
    std::uint64_t level_enlarge_factor = 0;

    /** Bytes the value cache of the collection holds. */
    std::uint64_t cache_limit = 0;

    /** Bytes the write buffer of the collection holds at once. */
    std::uint64_t write_buffer_max_bytes = 0;

    /** Writes the write buffer of the collection holds at once. */
    std::uint64_t write_buffer_max_elements = 0;

    /** Runs of this collection in the image's run list, which follow those of the records before it. */
    std::uint64_t run_count = 0;

    /** Bytes every value takes, zero for a layout whose values differ in width. */
    std::uint32_t value_size = 0;

    /** Bytes the largest value takes, zero for a fixed-width layout. */
    std::uint32_t value_max_size = 0;

    /** The volume the collection's runs live on, which a reopen binds it to again. */
    std::uint16_t volume = 0;

    /** Bytes of @c name_storage the name occupies. */
    std::uint16_t name_bytes = 0;

    /** The name a caller reaches the collection by, held inline and never terminated. */
    std::array<char, collection_name_max_bytes_k> name_storage {};

    /** The name this record carries, as a view into its own bytes. */
    [[nodiscard]] string_view_t name() const noexcept { return string_view_t {name_storage.data(), name_bytes}; }

    /** The collection this record describes, as the collection layer takes its bounds. */
    [[nodiscard]] collection_config_t config() const noexcept {
        collection_config_t config;
        config.cache_limit = static_cast<std::size_t>(cache_limit);
        config.write_buffer_max_bytes = static_cast<std::size_t>(write_buffer_max_bytes);
        config.write_buffer_max_elements = static_cast<std::size_t>(write_buffer_max_elements);
        config.lsm_tree.level_enlarge_factor = static_cast<std::size_t>(level_enlarge_factor);
        config.lsm_tree.lsm_level_0.max_bytes = static_cast<std::size_t>(first_level_max_bytes);
        config.lsm_tree.lsm_level_0.sorted_run.value_size = static_cast<stored_value_size_t>(value_size);
        config.lsm_tree.lsm_level_0.sorted_run.value_max_size = static_cast<stored_value_size_t>(value_max_size);
        return config;
    }
};

#pragma endregion Schema

#pragma region Database

/**
 *  @brief Named collections of one database, the catalog that resolves them and the transactions they share.
 *  @tparam stored_key_ The key every collection of this database is addressed by.
 *  @tparam volume_type_ The volume every run of every collection lives on.
 *
 *  The router and the channels are borrowed and must outlive the database, and so must the budget the
 *  collections charge their memory to. A database is pinned rather than movable, because its registry
 *  and every live transaction name it.
 */
template <typename stored_key_, typename volume_type_>
class basic_database {
  public:

#pragma region Types

    /** The key every collection of this database is addressed by. */
    using stored_key_t = stored_key_;

    /** The volume every run of every collection lives on. */
    using volume_t = volume_type_;

    /** The transport those volumes move bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_t&>().disk())>;

    /** The volumes of this database and the placement that spreads collections over them. */
    using router_t = basic_volume_router<volume_t>;

    /** The queue pairs every call that moves bytes borrows one of. */
    using channel_pool_t = basic_io_channel_pool<disk_t>;

    /** One borrowed queue pair, drained and given back when it dies. */
    using channel_guard_t = typename channel_pool_t::guard_t;

    /** A collection named by its ID or by its name, which is how every call addresses one. */
    using any_collection_id_t = std::variant<collection_id_t, collection_alphanumeric_id_t>;

    /** The transactions of this database, the order they commit in and the conflicts that stop them. */
    using registry_t = database_transaction_registry<stored_key_t, basic_database>;

    /** One transaction over the collections of this database. */
    using transaction_t = database_transaction<stored_key_t, basic_database>;

    /** A live transaction, owned by whoever asked for it and retired when it dies. */
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;

    /** One level set per collection, held so the whole database answers at one moment. */
    using snapshot_t = database_snapshot<stored_key_t, volume_t>;

    /** A collection whose values follow @p value_layout_. */
    template <value_layout_t value_layout_>
    using collection_of = collection<key_slot<stored_key_t, value_layout_>, volume_t>;

    /** One collection, of whichever value layout it was created with. */
    using any_collection_t =
        std::variant<collection_of<value_layout_t::inline_1_byte_k>, collection_of<value_layout_t::inline_4_bytes_k>,
                     collection_of<value_layout_t::inline_8_bytes_k>, collection_of<value_layout_t::inline_16_bytes_k>,
                     collection_of<value_layout_t::inline_32_bytes_k>, collection_of<value_layout_t::inline_64_bytes_k>,
                     collection_of<value_layout_t::fixed_width_k>, collection_of<value_layout_t::variable_width_k>>;

    /** One entry of the catalog: what a collection is called, how it is shaped, and the collection itself. */
    struct collection_entry_t {

        /** The UUID the catalog, every commit and the C ABI address this collection by. */
        collection_id_t id = collection_invalid_id_k;

        /** The name a caller reaches this collection by. */
        string_t name;

        /** The bounds this collection was mounted with. */
        collection_config_t config {};

        /** The volume this collection's runs live on. */
        volume_id_t volume = volume_id_t::unmounted_k;

        /** The collection itself, pinned here so a snapshot may name it. */
        any_collection_t held;
    };

#pragma endregion Types

#pragma region Construction

    basic_database(basic_database const&) = delete;
    basic_database(basic_database&&) = delete;
    basic_database& operator=(basic_database const&) = delete;
    basic_database& operator=(basic_database&&) = delete;

    /** A database charging @p resources for every collection it opens, which @c mount then fills. */
    explicit basic_database(resources_shared_ptr_t resources) noexcept
        : resources_(std::move(resources)), transactions_(*this, resources_) {}

    ~basic_database() noexcept { [[maybe_unused]] status_t const closed = unmount(); }

    /**
     *  @brief Opens every collection @p router already names, or starts an empty catalog.
     *  @param[inout] router The volumes every run lands on, borrowed and outliving the database.
     *  @param[inout] channels The queue pairs every call borrows one of, borrowed and outliving the database.
     *  @param[in] config The write-buffer bounds a transaction takes when its caller names none.
     */
    status_t mount(router_t& router, channel_pool_t& channels, database_config_t const& config) noexcept {
        if (router_ != nullptr) return {status_code_t::already_exists_k, "This database has already mounted"};
        router_ = &router;
        channels_ = &channels;
        config_ = config;

        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        if (status_t const loaded = load_schema_(claimed->channel()); loaded.failed()) return loaded;
        return publish_catalog_();
    }

    /**
     *  @brief Flushes every collection, rewrites the catalog and lets every collection go.
     *
     *  @note The router and the channels are still needed here, so this runs before the mount underneath
     *      is torn down; the destructor calls it for a caller that did not.
     */
    status_t unmount() noexcept {
        if (router_ == nullptr) return {};
        status_t first;
        {
            expected<claimed_channel_t> claimed = claim_channel_();
            if (!claimed) first = claimed.status();
            else {
                for (std::size_t index = 0; index != collections_.size(); ++index)
                    if (status_t const flushed = std::visit(
                            [&](auto& held) noexcept { return held.flush(claimed->channel()); },
                            collections_[index]->held);
                        flushed.failed() && first.succeeded())
                        first = flushed;
                if (status_t const saved = save_schema_(claimed->channel()); saved.failed() && first.succeeded())
                    first = saved;
            }
            // The entries outlive the publication that stops naming them, so no reader is left in one.
            st::basic_vector<entry_pointer_t> released = std::move(collections_);
            [[maybe_unused]] status_t const emptied = publish_catalog_();
        }
        router_ = nullptr;
        channels_ = nullptr;
        return first;
    }

#pragma endregion Construction

#pragma region Catalog

    /** Collections this database names right now. */
    [[nodiscard]] std::size_t size() const noexcept {
        catalog_guard_t const published {*this};
        return published.view().size();
    }

    /** Whether @p id names a collection of this database. */
    [[nodiscard]] bool has_collection(any_collection_id_t const& id) const noexcept {
        catalog_guard_t const published {*this};
        return entry_at_in_(published.view(), id) != nullptr;
    }

    /** The ID @p id resolves to, or @c collection_invalid_id_k when it names nothing. */
    [[nodiscard]] collection_id_t collection_id(any_collection_id_t const& id) const noexcept {
        catalog_guard_t const published {*this};
        collection_entry_t const* const found = entry_at_in_(published.view(), id);
        return found ? found->id : collection_invalid_id_k;
    }

    /** The name @p collection_id answers to, which is empty when it names nothing. */
    [[nodiscard]] string_t collection_name(collection_id_t collection_id) const noexcept {
        catalog_guard_t const published {*this};
        collection_entry_t const* const found = entry_with_id_in_(published.view(), collection_id);
        string_t name;
        if (found) { [[maybe_unused]] sz::status_t const copied = name.assign(found->name.view()); }
        return name;
    }

    /** The bounds @p collection_id was created with, which a transaction checks every write against. */
    status_t collection_config(collection_id_t collection_id, collection_config_t& config) const noexcept {
        catalog_guard_t const published {*this};
        collection_entry_t const* const found = entry_with_id_in_(published.view(), collection_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        config = found->config;
        return {};
    }

    /**
     *  @brief The ID @p id answers to and the bounds it was created with, read in one lookup.
     *  @param[in] id The collection to resolve, named by its ID or by its name.
     *  @param[out] collection_id The ID it answers to, invalid when the database names none.
     *  @param[out] config The bounds it was created with, untouched when the database names none.
     */
    status_t resolve_collection(any_collection_id_t const& id, collection_id_t& collection_id,
                                collection_config_t& config) const noexcept {
        catalog_guard_t const published {*this};
        collection_entry_t const* const found = entry_at_in_(published.view(), id);
        collection_id = found ? found->id : collection_invalid_id_k;
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        config = found->config;
        return {};
    }

    /** How many times the catalog has been published, which a memo of a lookup is checked against. */
    [[nodiscard]] std::uint64_t catalog_version() const noexcept {
        return fu::preferred_atomic_ref<std::uint64_t const> {catalog_version_}.load(std::memory_order_acquire);
    }

    /** The name and the number of every collection, which is short of the full list only when memory runs out. */
    [[nodiscard]] st::basic_vector<collection_ids_t> list_collections() const noexcept {
        catalog_guard_t const published {*this};
        catalog_view_t const& view = published.view();
        st::basic_vector<collection_ids_t> listed;
        if (listed.reserve(view.size()) != st::status_t::success_k) return listed;
        for (std::size_t index = 0; index != view.size(); ++index) {
            collection_ids_t named;
            if (sz::failed(named.name.assign(view[index]->name.view()))) break;
            named.id = view[index]->id;
            if (listed.push_back(std::move(named)) != st::status_t::success_k) break;
        }
        return listed;
    }

    /**
     *  @brief Catalogs a new collection @p name under @p collection_id, shaped by @p config.
     *  @param[in] collection_id The UUID the caller minted, which addresses the collection for as long as it lives.
     *  @param[in] name The name callers reach the collection by, which no other collection may hold.
     *  @param[in] config The write-buffer bounds, the cache bound and the shape of the tree below.
     *
     *  @note The value layout follows from @c config's value size, so a width of eight bytes is stored
     *      inside the key slots and a width of none is a collection of values that differ.
     */
    status_t create_collection(collection_id_t collection_id, string_view_t name,
                               collection_config_t const& config) noexcept {
        if (name.empty()) return status_t {status_code_t::invalid_argument_k, "A collection is named by a byte"};
        if (name.size() > collection_name_max_bytes_k)
            return status_t {status_code_t::invalid_argument_k, "A collection name is shorter than that"};
        if (collection_id == collection_invalid_id_k)
            return status_t {status_code_t::invalid_argument_k, "That ID names no collection"};
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        if (entry_named_in_(claimed_catalog_(), name) != nullptr ||
            entry_with_id_in_(claimed_catalog_(), collection_id) != nullptr)
            return status_t {status_code_t::already_exists_k, "The database already names that collection"};

        auto [placed, optimal_volume_status] = router_->optimal_volume(1);
        if (optimal_volume_status.failed()) return optimal_volume_status;
        value_layout_t const layout = resolve_value_layout(config.lsm_tree.lsm_level_0.sorted_run.value_size);
        auto [mounted, mount_collection_status] = mount_collection_(router_->volume(placed), config, layout);
        if (mount_collection_status.failed()) return mount_collection_status;

        entry_pointer_t entry {new (std::nothrow) collection_entry_t {}};
        if (!entry) return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
        entry->id = collection_id;
        if (sz::failed(entry->name.assign(name)))
            return status_t {status_code_t::out_of_memory_k, "No room for a collection's name"};
        entry->config = config;
        entry->volume = placed;
        entry->held = std::move(mounted);

        if (collections_.push_back(std::move(entry)) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for another collection"};
        if (status_t const saved = save_schema_(claimed->channel()); saved.failed()) {
            collections_.erase(collections_.size() - 1);
            return saved;
        }
        if (status_t const publication = publish_catalog_(); publication.failed()) {
            collections_.erase(collections_.size() - 1);
            return publication;
        }
        return {};
    }

    /** Returns every run of @p id to its volume and forgets the name, which destroys the data. */
    status_t drop_collection(any_collection_id_t const& id) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        std::size_t const position = position_at_(id);
        if (position == collections_.size())
            return {status_code_t::not_found_k, "The database names no such collection"};

        // The catalog stops naming the runs before a single one of them is handed back.
        entry_pointer_t doomed = std::move(collections_[position]);
        collections_.erase(position);
        if (status_t const publication = publish_catalog_(); publication.failed()) {
            [[maybe_unused]] st::status_t const returned = collections_.push_back(std::move(doomed));
            return publication;
        }
        if (status_t const saved = save_schema_(claimed->channel()); saved.failed()) {
            [[maybe_unused]] st::status_t const returned = collections_.push_back(std::move(doomed));
            [[maybe_unused]] status_t const restored = publish_catalog_();
            return saved;
        }
        return std::visit([&](auto& held) noexcept { return held.destroy(claimed->channel()); }, doomed->held);
    }

#pragma endregion Catalog

#pragma region Shape

    /** Keys staged plus key slots every level of every collection holds. */
    [[nodiscard]] std::uint64_t population_count() const noexcept {
        [[maybe_unused]] claim_guard_t const held {claim_};
        std::uint64_t counted = 0;
        for (std::size_t index = 0; index != collections_.size(); ++index)
            counted += std::visit([](auto const& collection) noexcept { return collection.population_count(); },
                                  collections_[index]->held);
        return counted;
    }

    /** Bytes every run of every collection takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept {
        [[maybe_unused]] claim_guard_t const held {claim_};
        std::uint64_t counted = 0;
        for (std::size_t index = 0; index != collections_.size(); ++index)
            counted += std::visit([](auto const& collection) noexcept { return collection.size_bytes(); },
                                  collections_[index]->held);
        return counted;
    }

    /** Bytes the largest value @p id admits takes, which is what a caller sizes a read buffer by. */
    [[nodiscard]] stored_value_size_t buffer_size_for_any_stored_value(any_collection_id_t const& id) const noexcept {
        [[maybe_unused]] claim_guard_t const held {claim_};
        collection_entry_t const* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return 0;
        return std::visit([](auto const& collection) noexcept { return collection.value_max_size(); }, found->held);
    }

#pragma endregion Shape

#pragma region Lookups

    /** Whether @p id carries @p stored_key with a value rather than a tombstone. */
    [[nodiscard]] bool contains(any_collection_id_t const& id, stored_key_t stored_key) const noexcept {
        [[maybe_unused]] claim_guard_t const held {claim_};
        collection_entry_t const* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return false;
        stored_value_size_t size = 0;
        return std::visit([&](auto const& collection) noexcept { return collection.find(stored_key, size); },
                          found->held) == select_result_t::success_k;
    }

    /** Bytes the value of @p stored_key takes in @p id, or @c not_found_k for a miss and an erasure. */
    status_t stored_value_size(any_collection_id_t const& id, stored_key_t stored_key,
                               stored_value_size_t& size) const noexcept {
        size = 0;
        [[maybe_unused]] claim_guard_t const held {claim_};
        collection_entry_t const* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        select_result_t const answered = std::visit(
            [&](auto const& collection) noexcept { return collection.find(stored_key, size); }, found->held);
        if (answered != select_result_t::success_k) return {status_code_t::not_found_k, "That collection misses it"};
        return {};
    }

    /**
     *  @brief Copies the value @p id holds for @p stored_key into @p into, which needs only its bytes.
     *  @param[in] id The collection to read from, named by its ID or by its name.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     */
    status_t select(any_collection_id_t const& id, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) const noexcept {
        size = 0;
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        collection_entry_t const* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit(
            [&](auto const& collection) noexcept {
                return collection.select(claimed->channel(), stored_key, into, size);
            },
            found->held);
    }

    /**
     *  @brief Reads the values of @p stored_keys out of @p id, refilling the channel as completions retire.
     *  @param[in] id The collection to read from, named by its ID or by its name.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     */
    template <typename sink_type_>
    status_t select(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys,
                    std::span<select_result_t> results, sink_type_&& sink) const noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        collection_entry_t const* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit(
            [&](auto const& collection) noexcept {
                return collection.select(claimed->channel(), stored_keys, results, sink);
            },
            found->held);
    }

#pragma endregion Lookups

#pragma region Writes

    /** Stages @p stored_value under @p stored_key of @p id, flushing first when the buffer is full. */
    status_t insert(any_collection_id_t const& id, stored_key_t stored_key, stored_value_view_t stored_value) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        collection_entry_t* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit(
            [&](auto& collection) noexcept { return collection.insert(claimed->channel(), stored_key, stored_value); },
            found->held);
    }

    /** Stages a tombstone under @p stored_key of @p id, flushing first when the buffer is full. */
    status_t remove(any_collection_id_t const& id, stored_key_t stored_key) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        collection_entry_t* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit([&](auto& collection) noexcept { return collection.erase(claimed->channel(), stored_key); },
                          found->held);
    }

    /**
     *  @brief Stages one value per key into @p id, flushing first when they do not all fit.
     *  @param[in] id The collection to write to, named by its ID or by its name.
     *  @param[in] stored_keys The keys to write, in any order and with repeats taking the last value.
     *  @param[in] stored_values The values back to back, in the order their keys name them.
     *  @param[in] sizes Bytes of every value, one per key, which must add up to @p stored_values.
     */
    status_t insert(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys,
                    std::span<byte_t const> stored_values, std::span<stored_value_size_t const> sizes) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        collection_entry_t* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit(
            [&](auto& collection) noexcept {
                return collection.insert(claimed->channel(), stored_keys, stored_values, sizes);
            },
            found->held);
    }

    /** Stages a tombstone under every key of @p stored_keys in @p id. */
    status_t remove(any_collection_id_t const& id, std::span<stored_key_t const> stored_keys) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        collection_entry_t* const found = entry_at_in_(claimed_catalog_(), id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        return std::visit([&](auto& collection) noexcept { return collection.erase(claimed->channel(), stored_keys); },
                          found->held);
    }

#pragma endregion Writes

#pragma region Maintenance

    /** Writes every staged write of every collection as one run of its level zero. */
    status_t flush() noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        status_t first;
        for (std::size_t index = 0; index != collections_.size(); ++index)
            if (status_t const flushed = std::visit([&](auto& held) noexcept { return held.flush(claimed->channel()); },
                                                    collections_[index]->held);
                flushed.failed() && first.succeeded())
                first = flushed;
        // Every run the flushes wrote is sealed by now, so the catalog may name them.
        if (status_t const saved = save_schema_(claimed->channel()); saved.failed() && first.succeeded()) first = saved;
        return first;
    }

    /** Merges one overfull level of every collection that has one, which needs two channels at once. */
    status_t compact() noexcept {
        expected<claimed_channel_t> reading = claim_channel_();
        if (!reading) return reading.status();
        auto [writing, acquire_status] = channels_->acquire();
        if (acquire_status.failed()) return acquire_status;

        schema_write_t recording {this, &reading->channel()};
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = &recording;

        status_t first;
        for (std::size_t index = 0; index != collections_.size(); ++index) {
            auto [merged, compact_status] = std::visit(
                [&](auto& held) noexcept {
                    return held.tree().compact(reading->channel(), writing.channel(),
                                               compaction_placement_t::automatic_k, nullptr, published);
                },
                collections_[index]->held);
            if (compact_status.failed() && first.succeeded()) first = compact_status;
        }
        return first;
    }

    /**
     *  @brief Flushes every collection, then holds the level set each flush published.
     *
     *  @note A hold is what a publication waits on, so a flush, a compaction or another snapshot
     *      taken on the same thread while one is still alive never returns.
     */
    expected<snapshot_t> take_snapshot() noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        snapshot_t taken;
        if (status_t const room = taken.reserve(collections_.size()); room.failed()) return room;

        for (std::size_t index = 0; index != collections_.size(); ++index) {
            collection_entry_t& entry = *collections_[index];
            status_t const held = std::visit(
                [&](auto& collection) noexcept -> status_t {
                    auto [levels, take_snapshot_status] = collection.take_snapshot(claimed->channel());
                    if (take_snapshot_status.failed()) return take_snapshot_status;
                    return taken.hold(entry.id, entry.name.view(),
                                      typename snapshot_t::any_collection_snapshot_t {std::move(levels)});
                },
                entry.held);
            if (held.failed()) return held;
        }
        return taken;
    }

#pragma endregion Maintenance

#pragma region Transactions

    /** A transaction of @p id over every collection, with the write-buffer bounds of @p config. */
    expected<transaction_unique_ptr_t> create_transaction(transaction_id_t id,
                                                          transaction_config_t const& config) noexcept {
        return transactions_.create_transaction(id, config);
    }

    /** A transaction of @p id with the write-buffer bounds this database was mounted with. */
    expected<transaction_unique_ptr_t> create_transaction(transaction_id_t id) noexcept {
        return create_transaction(id, config_.default_transaction);
    }

    /** Whether @p id names a live transaction of this database. */
    [[nodiscard]] bool has_transaction(transaction_id_t id) const noexcept { return transactions_.has_transaction(id); }

#pragma endregion Transactions

#pragma region Write Path

    /**
     *  @brief Takes the database's claim and a channel, which a staged transaction holds until it commits.
     *
     *  @note The claim is what makes a commit atomic to a reader: no @c select of any collection runs
     *      between the first write a commit applies and the last.
     */
    void acquire_write_path() noexcept {
        write_claim_ = claim_guard_t {claim_};
        write_channel_ = channel_guard_t {};
        expected<channel_guard_t> guard = channels_->acquire();
        if (guard) write_channel_ = std::move(*guard);
    }

    /** Hands the channel and the claim back, which a commit and a rollback both do. */
    void release_write_path() noexcept {
        write_channel_ = channel_guard_t {};
        write_claim_ = claim_guard_t {};
    }

    /** Whether @p collection_id still names a collection, which a commit checks before it writes one. */
    [[nodiscard]] bool contains_collection(collection_id_t collection_id) const noexcept {
        return entry_with_id_in_(claimed_catalog_(), collection_id) != nullptr;
    }

    /**
     *  @brief Whether @p count writes of @p bytes fit in the write buffer of @p collection_id as it stands.
     *
     *  @note A refusal only asks the caller to flush first; a collection whose buffer fills anyway
     *      flushes itself rather than losing the write.
     */
    [[nodiscard]] bool collection_fits(collection_id_t collection_id, std::size_t count,
                                       std::size_t bytes) const noexcept {
        collection_entry_t const* const found = entry_with_id_in_(claimed_catalog_(), collection_id);
        if (!found) return false;
        return std::visit(
            [&](auto const& collection) noexcept {
                return collection.staged_count() + count <= collection.config().write_buffer_max_elements &&
                       bytes <= collection.config().write_buffer_max_bytes;
            },
            found->held);
    }

    /** Writes the staged writes of @p collection_id out, on the channel the write path holds. */
    status_t flush_collection(collection_id_t collection_id) noexcept {
        collection_entry_t* const found = entry_with_id_in_(claimed_catalog_(), collection_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        if (!write_channel_.holding()) return {status_code_t::capacity_exhausted_k, "The write path took no channel"};
        return std::visit([&](auto& collection) noexcept { return collection.flush(write_channel_.channel()); },
                          found->held);
    }

    /** Stages @p stored_value under @p stored_key of @p collection_id, on the channel the write path holds. */
    status_t insert_in_collection(collection_id_t collection_id, stored_key_t stored_key,
                                  stored_value_view_t stored_value) noexcept {
        collection_entry_t* const found = entry_with_id_in_(claimed_catalog_(), collection_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        if (!write_channel_.holding()) return {status_code_t::capacity_exhausted_k, "The write path took no channel"};
        return std::visit(
            [&](auto& collection) noexcept {
                return collection.insert(write_channel_.channel(), stored_key, stored_value);
            },
            found->held);
    }

    /** Stages a tombstone under @p stored_key of @p collection_id, on the channel the write path holds. */
    status_t remove_from_collection(collection_id_t collection_id, stored_key_t stored_key) noexcept {
        collection_entry_t* const found = entry_with_id_in_(claimed_catalog_(), collection_id);
        if (!found) return {status_code_t::not_found_k, "The database names no such collection"};
        if (!write_channel_.holding()) return {status_code_t::capacity_exhausted_k, "The write path took no channel"};
        return std::visit(
            [&](auto& collection) noexcept { return collection.erase(write_channel_.channel(), stored_key); },
            found->held);
    }

#pragma endregion Write Path

#pragma region User Data

    /** Bytes the caller's own blob takes, which is zero when it has saved none. */
    [[nodiscard]] std::uint64_t user_data_size_bytes() const noexcept {
        [[maybe_unused]] claim_guard_t const held {claim_};
        expected<volume_blob_t> const found = router_->find(database_user_data_name());
        return found ? found->length : 0;
    }

    /** Replaces the caller's own blob with @p bytes, which the engine never reads. */
    status_t save_user_data(bytes_view_t bytes) noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        return write_blob_(claimed->channel(), database_user_data_name(), blob_kind_t::user_data_k, bytes);
    }

    /** Reads the caller's own blob into @p into, which must hold @c user_data_size_bytes of it. */
    status_t load_user_data(bytes_span_t into) const noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        return read_blob_(claimed->channel(), database_user_data_name(), into);
    }

    /** Forgets the caller's own blob and returns its bytes to the volume that held it. */
    status_t clear_user_data() noexcept {
        expected<claimed_channel_t> claimed = claim_channel_();
        if (!claimed) return claimed.status();
        return router_->drop(claimed->channel(), database_user_data_name());
    }

#pragma endregion User Data

  private:

#pragma region Private Types

    /** One catalog entry, heap-pinned so a snapshot and a live transaction may name it. */
    using entry_pointer_t = std::unique_ptr<collection_entry_t>;

    /** One published catalog, naming every entry the database held when it was laid out. */
    using catalog_view_t = st::basic_vector<collection_entry_t*>;

    /**
     *  @brief Holds one published catalog against republication for the life of the guard.
     *
     *  A lookup that reads nothing but a name, an ID or a shape needs no claim: every entry is
     *  pinned on the heap, so a create and a drop replace only the array naming them, and the
     *  publication that replaces it waits until the view it retired has no reader left inside.
     */
    class catalog_guard_t {
      public:

        /** Counts this thread into whichever view is live, retrying while one is being published. */
        explicit catalog_guard_t(basic_database const& database) noexcept : database_(&database) {
            for (;;) {
                slot_ = database.live_catalog_slot_();
                database.catalog_readers_reference_(slot_).fetch_add(1, std::memory_order_acquire);
                // Pairs with the publisher's fence, so the count and the live slot are never both read stale.
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (database.live_catalog_slot_() == slot_) return;
                database.catalog_readers_reference_(slot_).fetch_sub(1, std::memory_order_release);
            }
        }

        catalog_guard_t(catalog_guard_t const&) = delete;
        catalog_guard_t& operator=(catalog_guard_t const&) = delete;
        catalog_guard_t(catalog_guard_t&&) = delete;
        catalog_guard_t& operator=(catalog_guard_t&&) = delete;

        ~catalog_guard_t() noexcept {
            database_->catalog_readers_reference_(slot_).fetch_sub(1, std::memory_order_release);
        }

        /** The entries this view names, which no publication replaces while the guard lives. */
        [[nodiscard]] catalog_view_t const& view() const noexcept { return database_->catalogs_[slot_]; }

      private:

        /** The database whose catalog this guard holds, borrowed for the guard's life. */
        basic_database const* database_ = nullptr;

        /** Which of the two views this guard counted itself into. */
        std::size_t slot_ = 0;
    };

    /**
     *  @brief The database's one claim, held for the life of the guard.
     *
     *  This is the exclusion the file's brief names, and the only one: a collection admits one caller
     *  at a time, and a commit spans several collections, so the word is the database's.
     */
    class claim_guard_t {
      public:

        claim_guard_t() noexcept = default;
        claim_guard_t(claim_guard_t const&) = delete;
        claim_guard_t& operator=(claim_guard_t const&) = delete;

        /** Takes @p claim, waiting while another caller holds it. */
        explicit claim_guard_t(claim_word_t& claim) noexcept : claim_(&claim) {
            fu::preferred_atomic_ref<claim_word_t> const word {claim};
            claim_word_t expected_free = 0;
            while (
                !word.compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                fu::preferred_yield_t {}(claim, expected_free, std::size_t {0});
                expected_free = 0;
            }
        }

        claim_guard_t(claim_guard_t&& other) noexcept : claim_(std::exchange(other.claim_, nullptr)) {}

        claim_guard_t& operator=(claim_guard_t&& other) noexcept {
            if (this == &other) return *this;
            release_();
            claim_ = std::exchange(other.claim_, nullptr);
            return *this;
        }

        ~claim_guard_t() noexcept { release_(); }

      private:

        /** Hands the claim back, which both the destructor and a move assignment need. */
        void release_() noexcept {
            if (claim_) fu::preferred_atomic_ref<claim_word_t> {*claim_}.store(0, std::memory_order_release);
            claim_ = nullptr;
        }

        /** The word this guard holds, null once moved from. */
        claim_word_t* claim_ = nullptr;
    };

    /** The claim and the channel one call of this database runs under, released in that order. */
    class claimed_channel_t {
      public:

        claimed_channel_t(claim_guard_t&& claim, channel_guard_t&& channel) noexcept
            : claim_(std::move(claim)), channel_(std::move(channel)) {}

        claimed_channel_t(claimed_channel_t&&) noexcept = default;
        claimed_channel_t& operator=(claimed_channel_t&&) noexcept = default;

        /** The channel, which only the thread holding this is on. */
        [[nodiscard]] io_channel_t& channel() const noexcept { return channel_.channel(); }

      private:

        /** The database's claim, given back after the channel. */
        claim_guard_t claim_;

        /** The borrowed queue pair, drained and given back first. */
        channel_guard_t channel_;
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** Which of the two published catalogs a lookup reads right now. */
    [[nodiscard]] std::size_t live_catalog_slot_() const noexcept {
        return static_cast<std::size_t>(
            fu::preferred_atomic_ref<std::int32_t const> {live_catalog_}.load(std::memory_order_acquire));
    }

    /** The word counting the readers of published catalog @p slot. */
    [[nodiscard]] fu::preferred_atomic_ref<readers_count_t> catalog_readers_reference_(
        std::size_t slot) const noexcept {
        return fu::preferred_atomic_ref<readers_count_t> {catalog_readers_[slot]};
    }

    /**
     *  @brief The live catalog as a caller already holding the claim reads it, counting itself into none.
     *
     *  @note Only a claim holder publishes, and it publishes before it lets the claim go, so the view
     *      this answers is both live and in step with @c collections_ for as long as the claim is held.
     */
    [[nodiscard]] catalog_view_t const& claimed_catalog_() const noexcept { return catalogs_[live_catalog_slot_()]; }

    /**
     *  @brief Lays the catalog out into the idle view, makes it live, and waits its readers out.
     *
     *  @note Only a caller holding the claim publishes, so one thread alone writes the two views,
     *      and on return no reader is left inside the view this one retired - which is what lets
     *      a dropped collection be destroyed right after.
     */
    status_t publish_catalog_() noexcept {
        std::size_t const retired = live_catalog_slot_();
        std::size_t const staged = 1 - retired;
        catalog_view_t& building = catalogs_[staged];
        if (building.reserve(collections_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to publish the catalog"};
        building.clear();
        for (std::size_t index = 0; index != collections_.size(); ++index) {
            [[maybe_unused]] st::status_t const named = building.push_back(st::assume_reserved,
                                                                           collections_[index].get());
        }
        fu::preferred_atomic_ref<std::uint64_t> const version {catalog_version_};
        version.store(version.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        fu::preferred_atomic_ref<std::int32_t> {live_catalog_}.store(static_cast<std::int32_t>(staged),
                                                                     std::memory_order_release);
        // Pairs with the reader's fence, so the retired slot's count is read only after the new slot is visible.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        for (readers_count_t readers = catalog_readers_reference_(retired).load(std::memory_order_acquire);
             readers != 0; readers = catalog_readers_reference_(retired).load(std::memory_order_acquire))
            fu::preferred_yield_t {}(catalog_readers_[retired], readers, std::size_t {0});
        return {};
    }

    /** The entry @p collection_id names in @p view, or null when the view names none. */
    [[nodiscard]] static collection_entry_t* entry_with_id_in_(catalog_view_t const& view,
                                                               collection_id_t collection_id) noexcept {
        for (std::size_t index = 0; index != view.size(); ++index)
            if (view[index]->id == collection_id) return view[index];
        return nullptr;
    }

    /** The entry @p name answers to in @p view, or null when the view names none. */
    [[nodiscard]] static collection_entry_t* entry_named_in_(catalog_view_t const& view, string_view_t name) noexcept {
        for (std::size_t index = 0; index != view.size(); ++index)
            if (view[index]->name.view() == name) return view[index];
        return nullptr;
    }

    /** The entry @p id names in @p view, or null when the view names none. */
    [[nodiscard]] static collection_entry_t* entry_at_in_(catalog_view_t const& view,
                                                          any_collection_id_t const& id) noexcept {
        if (collection_id_t const* const identified = std::get_if<collection_id_t>(&id))
            return entry_with_id_in_(view, *identified);
        return entry_named_in_(view, std::get<collection_alphanumeric_id_t>(id).view());
    }

    /** Takes the claim and then a shared channel, which every call that moves bytes runs under. */
    expected<claimed_channel_t> claim_channel_() const noexcept {
        claim_guard_t claim {claim_};
        auto [guard, acquire_status] = channels_->acquire();
        if (acquire_status.failed()) return acquire_status;
        return claimed_channel_t {std::move(claim), std::move(guard)};
    }

    /** Where @p id sits in the catalog, or the catalog's size when it names nothing. */
    [[nodiscard]] std::size_t position_at_(any_collection_id_t const& id) const noexcept {
        if (collection_id_t const* const identified = std::get_if<collection_id_t>(&id)) {
            for (std::size_t index = 0; index != collections_.size(); ++index)
                if (collections_[index]->id == *identified) return index;
            return collections_.size();
        }
        collection_alphanumeric_id_t const& named = std::get<collection_alphanumeric_id_t>(id);
        for (std::size_t index = 0; index != collections_.size(); ++index)
            if (collections_[index]->name == named) return index;
        return collections_.size();
    }

    /** Mounts a collection of @p value_layout_ over @p volume, as one alternative of the variant. */
    template <value_layout_t value_layout_>
    expected<any_collection_t> mounted_as_(volume_t& volume, collection_config_t const& config) const noexcept {
        auto [mounted, mount_status] = collection_of<value_layout_>::mount(volume, config,
                                                                           resources_->memory_constraint);
        if (mount_status.failed()) return mount_status;
        return any_collection_t {std::move(mounted)};
    }

    /** Mounts a collection of the runtime @p value_layout over @p volume. */
    expected<any_collection_t> mount_collection_(volume_t& volume, collection_config_t const& config,
                                                 value_layout_t value_layout) const noexcept {
        switch (value_layout) {
        case value_layout_t::inline_1_byte_k: return mounted_as_<value_layout_t::inline_1_byte_k>(volume, config);
        case value_layout_t::inline_4_bytes_k: return mounted_as_<value_layout_t::inline_4_bytes_k>(volume, config);
        case value_layout_t::inline_8_bytes_k: return mounted_as_<value_layout_t::inline_8_bytes_k>(volume, config);
        case value_layout_t::inline_16_bytes_k: return mounted_as_<value_layout_t::inline_16_bytes_k>(volume, config);
        case value_layout_t::inline_32_bytes_k: return mounted_as_<value_layout_t::inline_32_bytes_k>(volume, config);
        case value_layout_t::inline_64_bytes_k: return mounted_as_<value_layout_t::inline_64_bytes_k>(volume, config);
        case value_layout_t::fixed_width_k: return mounted_as_<value_layout_t::fixed_width_k>(volume, config);
        case value_layout_t::variable_width_k: return mounted_as_<value_layout_t::variable_width_k>(volume, config);
        default: break;
        }
        return status_t {status_code_t::invalid_argument_k, "A collection admits no such value layout"};
    }

    /**
     *  @brief Replaces the blob @p name with @p bytes, sealing it only once they are durable.
     *  @param[inout] channel The channel the bytes are staged and flushed on, held by the calling thread.
     *  @param[in] name The bytes the blob is named by, which no other blob of this database holds.
     *  @param[in] kind What the blob is, which is how a later mount enumerates it.
     *  @param[in] bytes The whole image, which must fit in one block of @p channel.
     */
    status_t write_blob_(io_channel_t& channel, blob_name_t name, blob_kind_t kind,
                         std::span<byte_t const> bytes) noexcept {
        if (bytes.empty()) return {status_code_t::invalid_argument_k, "A named blob carries at least one byte"};
        std::uint64_t const written = round_up_to_multiple(bytes.size(), portable_alignment_k);
        if (written > channel.requests().buffer_bytes())
            return {status_code_t::capacity_exhausted_k, "That blob is longer than one block of a channel"};

        [[maybe_unused]] status_t const forgotten = router_->drop(channel, name);
        auto [reserved, create_status] = router_->create(name, kind, bytes.size());
        if (create_status.failed()) return create_status;
        if (reserved.extent.length < written)
            return {status_code_t::capacity_exhausted_k, "A volume reserved less than one page for that blob"};

        volume_t& volume = router_->volume(reserved.volume);
        basic_write_pipeline<disk_t> writer {volume.disk(), channel, reserved.medium};
        auto [block, acquire_buffer_status] = writer.acquire_buffer();
        if (acquire_buffer_status.failed()) return acquire_buffer_status;
        std::span<byte_t> const room = block.bytes();
        std::memset(room.data(), 0, static_cast<std::size_t>(written));
        std::memcpy(room.data(), bytes.data(), bytes.size());
        if (status_t const queued = writer.enqueue_write(std::move(block), reserved.slice(0, written)); queued.failed())
            return queued;
        if (status_t const durable = writer.make_durable(); durable.failed()) return durable;
        return volume.seal(channel, reserved);
    }

    /** Reads the whole blob @p name into @p into, which must hold its content length. */
    status_t read_blob_(io_channel_t& channel, blob_name_t name, std::span<byte_t> into) const noexcept {
        auto [found, find_status] = router_->find(name);
        if (find_status.failed()) return find_status;
        if (into.size() < found.length) return {status_code_t::invalid_argument_k, "A buffer shorter than the blob"};
        std::uint64_t const read = round_up_to_multiple(found.length, portable_alignment_k);
        if (read > channel.requests().buffer_bytes())
            return {status_code_t::capacity_exhausted_k, "That blob is longer than one block of a channel"};

        volume_t& volume = router_->volume(found.volume);
        basic_read_pipeline<disk_t> reader {volume.disk(), channel, found.medium};
        if (status_t const offered = reader.enqueue_read(found.slice(0, read), 0); offered.failed()) return offered;
        if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
        status_t carried;
        auto sink = [&](completed_buffer_t const& completed) noexcept {
            if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                carried = whole;
                return;
            }
            std::memcpy(into.data(), completed.bytes.data(), static_cast<std::size_t>(found.length));
        };
        if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
        return carried;
    }

    /** Writes the catalog out as the database's schema blob, which every change to it does. */
    status_t save_schema_(io_channel_t& channel) noexcept {
        st::basic_vector<collection_record_t> records;
        st::basic_vector<lsm_tree_run_record_t> runs;
        if (records.reserve(collections_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to lay the catalog out"};
        for (std::size_t index = 0; index != collections_.size(); ++index) {
            collection_entry_t const& entry = *collections_[index];
            collection_record_t record;
            record.id = entry.id;
            std::size_t const named_before = runs.size();
            if (status_t const named = std::visit(
                    [&](auto const& held) noexcept { return record_lsm_tree_runs(held.tree(), runs); }, entry.held);
                named.failed())
                return named;
            record.run_count = runs.size() - named_before;
            if (entry.name.size() > collection_name_max_bytes_k)
                return {status_code_t::capacity_exhausted_k, "A collection name outgrew the catalog's record"};
            std::memcpy(record.name_storage.data(), entry.name.data(), entry.name.size());
            record.name_bytes = static_cast<std::uint16_t>(entry.name.size());
            record.volume = static_cast<std::uint16_t>(entry.volume);
            record.value_size = entry.config.lsm_tree.lsm_level_0.sorted_run.value_size;
            record.value_max_size = entry.config.lsm_tree.lsm_level_0.sorted_run.value_max_size;
            record.first_level_max_bytes = entry.config.lsm_tree.lsm_level_0.max_bytes;
            record.level_enlarge_factor = entry.config.lsm_tree.level_enlarge_factor;
            record.cache_limit = entry.config.cache_limit;
            record.write_buffer_max_bytes = entry.config.write_buffer_max_bytes;
            record.write_buffer_max_elements = entry.config.write_buffer_max_elements;
            if (records.push_back(std::move(record)) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "No room to lay the catalog out"};
        }

        std::uint64_t const version = database_schema_version_k;
        st::basic_vector<byte_t> image;
        if (image.resize(fields_storage_requirements(version, records, runs)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to lay the catalog out"};
        if (status_t const laid = save_fields(bytes_span_t {image}, version, records, runs); laid.failed()) return laid;
        return write_blob_(channel, database_schema_name(), blob_kind_t::schema_k, std::span<byte_t const> {image});
    }

    /** Opens every collection the schema blob names, which is what a mount of an older database does. */
    status_t load_schema_(io_channel_t& channel) noexcept {
        auto [found, find_status] = router_->find(database_schema_name());
        if (find_status.failed()) return find_status == status_code_t::not_found_k ? status_t {} : find_status;

        st::basic_vector<byte_t> image;
        if (image.resize(static_cast<std::size_t>(found.length)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to read the catalog into"};
        if (status_t const read = read_blob_(channel, database_schema_name(), std::span<byte_t> {image}); read.failed())
            return read;

        std::uint64_t version = 0;
        st::basic_vector<collection_record_t> records;
        st::basic_vector<lsm_tree_run_record_t> runs;
        if (status_t const parsed = load_fields(bytes_view_t {image}, version, records, runs); parsed.failed())
            return parsed;
        if (version != database_schema_version_k)
            return {status_code_t::unsupported_k, "The catalog follows another schema version"};

        if (collections_.reserve(records.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for the collections the catalog names"};
        std::size_t named = 0;
        for (std::size_t index = 0; index != records.size(); ++index) {
            if (records[index].run_count > runs.size() - named)
                return {status_code_t::malformed_k, "The catalog names more runs than it carries"};
            std::size_t const count = static_cast<std::size_t>(records[index].run_count);
            if (status_t const opened = adopt_(channel, records[index],
                                               std::span<lsm_tree_run_record_t const> {runs.data() + named, count});
                opened.failed())
                return opened;
            named += count;
        }
        if (named != runs.size()) return {status_code_t::malformed_k, "The catalog carries runs no collection claims"};
        return {};
    }

    /** Opens the collection @p record describes over the runs @p runs names, and takes it into the catalog. */
    status_t adopt_(io_channel_t& channel, collection_record_t const& record,
                    std::span<lsm_tree_run_record_t const> runs) noexcept {
        if (static_cast<std::size_t>(record.volume) >= router_->size())
            return {status_code_t::malformed_k, "The catalog names a volume this mount did not open"};

        collection_config_t const config = record.config();
        value_layout_t const layout = resolve_value_layout(config.lsm_tree.lsm_level_0.sorted_run.value_size);
        auto [mounted, mount_collection_status] = mount_collection_(
            router_->volume(static_cast<volume_id_t>(record.volume)), config, layout);
        if (mount_collection_status.failed()) return mount_collection_status;

        entry_pointer_t entry {new (std::nothrow) collection_entry_t {}};
        if (!entry) return {status_code_t::out_of_memory_k, "No room for another collection"};
        entry->id = record.id;
        if (sz::failed(entry->name.assign(record.name())))
            return {status_code_t::out_of_memory_k, "No room for a collection's name"};
        entry->config = config;
        entry->volume = static_cast<volume_id_t>(record.volume);
        entry->held = std::move(mounted);
        if (status_t const restored = std::visit(
                [&](auto& held) noexcept { return held.tree().restore(channel, runs); }, entry->held);
            restored.failed())
            return restored;
        if (collections_.push_back(std::move(entry)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for another collection"};
        return {};
    }

    /** What a compaction's hook is handed, which is the database and the channel it records through. */
    struct schema_write_t {

        /** The database whose catalog the hook writes out. */
        basic_database* store = nullptr;

        /** The channel the schema blob is staged and sealed on, held by the compacting thread. */
        io_channel_t* channel = nullptr;
    };

    /** Writes the catalog out for a compaction that has published, before it drops what it consumed. */
    static status_t record_schema_(void* context) noexcept {
        schema_write_t const& writing = *static_cast<schema_write_t const*>(context);
        return writing.store->save_schema_(*writing.channel);
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The budget every collection and every transaction of this database charges. */
    resources_shared_ptr_t resources_;

    /** The transactions of this database, which names this store for as long as it lives. */
    registry_t transactions_;

    /** The volumes every run lands on, null until the database is mounted. */
    router_t* router_ = nullptr;

    /** The queue pairs every call that moves bytes takes one of, null until the database is mounted. */
    channel_pool_t* channels_ = nullptr;

    /** The write-buffer bounds a transaction takes when its caller names none. */
    database_config_t config_ {};

    /** Every collection the catalog names, pinned so a snapshot may point into one. */
    st::basic_vector<entry_pointer_t> collections_;

    /** The two published catalogs, of which one is live and the other is the next publication's room. */
    std::array<catalog_view_t, 2> catalogs_ {};

    /** Which of the two published catalogs is live, stored with release and read with acquire. */
    std::int32_t mutable live_catalog_ = 0;

    /** How many catalogs have been published, which is what lets a caller memoize a lookup. */
    std::uint64_t mutable catalog_version_ = 0;

    /** Readers counted into each published catalog, which a publication waits on before it retires one; on a line of
     *  their own, so counting in never evicts the live word every reader checks. */
    alignas(fu::default_alignment_k) std::array<readers_count_t, 2> mutable catalog_readers_ {0, 0};

    /** Zero while nobody is inside a collection, one while a caller is, off the readers' line. */
    alignas(fu::default_alignment_k) claim_word_t mutable claim_ = 0;

    /** The claim a staged transaction holds from its stage until its commit. */
    claim_guard_t write_claim_;

    /** The channel that transaction writes on, which every applied key of its commit uses. */
    channel_guard_t write_channel_;

#pragma endregion Private Members
};

#pragma endregion Database

#pragma region Materialization

/**
 *  @brief The database of a host that keeps its volumes in files, which is the shape the C ABI names.
 *
 *  The disk is fixed here rather than templated because the layer above addresses a database by its
 *  key alone; that disk holds whichever file transport the mount probed, so one materialization
 *  serves the ring and the blocking floor alike, and a host on another medium instantiates
 *  @c basic_database over that volume itself.
 */
template <typename stored_key_>
class database final : public basic_database<stored_key_, basic_extent_volume<any_files_disk_t>> {
  public:

    /** The generic database this one materializes. */
    using base_t = basic_database<stored_key_, basic_extent_volume<any_files_disk_t>>;

    using base_t::base_t;
};

#pragma endregion Materialization

#pragma endregion Database

#pragma region Single Collection Database Transaction State

template <typename stored_key_>
struct single_collection_database_transaction_state {
    using stored_key_t = stored_key_;
    using write_buffer_t = write_buffer<stored_key_t>;
    using read_set_t = read_set<stored_key_t>;

    inline single_collection_database_transaction_state(write_buffer_t&& write_buffer,
                                                        resources_shared_ptr_t resources) noexcept
        : changes(std::forward<write_buffer_t>(write_buffer)), read_set(resources) {}

    inline bool empty() const noexcept { return changes.empty() && read_set.empty(); }

    inline void clear() {
        changes.unsafe_clear();
        read_set.clear();
    }

    write_buffer_t changes;
    read_set_t read_set;
};

#pragma endregion Single Collection Database Transaction State

#pragma region Single Collection Database Transaction Registry

template <typename stored_key_, value_layout_t value_layout_>
class single_collection_database;

template <typename stored_key_, typename store_type_>
class single_collection_database_transaction;

#pragma region Store Interface

/**
 *  @brief What a registry needs from the single collection beneath it to stage and to commit.
 *
 *  A whole write buffer travels across this boundary rather than one key at a time, because the sole
 *  collection admits the batch in one call and has nothing to group it by.
 */
template <typename store_type_, typename write_buffer_type_>
concept single_collection_transaction_store_concept = requires(store_type_& store, store_type_ const& constant,
                                                               write_buffer_type_ const& changes) {
    { store.acquire_write_path() } -> std::same_as<void>;
    { store.release_write_path() } -> std::same_as<void>;
    { constant.collection_fits(changes) } -> std::same_as<bool>;
    { store.flush_collection() } -> std::same_as<status_t>;
    { store.insert_write_buffer(changes) } -> std::same_as<status_t>;
};

#pragma endregion Store Interface

/**
 *  @brief The transactions of one single-collection database, their commit order and their conflicts.
 *  @tparam stored_key_ The key the collection is addressed by.
 *  @tparam store_type_ The collection the commits land in, satisfying @c single_collection_transaction_store_concept.
 */
template <typename stored_key_, typename store_type_>
class single_collection_database_transaction_registry {
  public:
#pragma region Types

    using stored_key_t = stored_key_;
    using store_t = store_type_;
    using transaction_t = single_collection_database_transaction<stored_key_, store_type_>;
    using transaction_state_t = single_collection_database_transaction_state<stored_key_t>;
    using write_buffer_t = typename transaction_state_t::write_buffer_t;
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;
    using return_transaction_t = expected<transaction_unique_ptr_t>;

    /** The commit-record slots a registry keeps when its owner names none. */
    static constexpr std::size_t default_conflict_slots_k = 1ull << 16;

    /** The transactions a registry admits at once when its owner names no count. */
    static constexpr std::size_t default_transaction_slots_k = 256;

#pragma endregion Types

#pragma region Construction

    inline single_collection_database_transaction_registry(
        store_t& store, resources_shared_ptr_t resources, std::size_t conflict_slots = default_conflict_slots_k,
        std::size_t transaction_slots = default_transaction_slots_k) noexcept
        : store_(&store), resources_(std::move(resources)), record_(conflict_slots, resources_->memory_constraint),
          roster_(transaction_slots, resources_->memory_constraint) {}

    inline single_collection_database_transaction_registry(single_collection_database_transaction_registry&&) = delete;
    inline single_collection_database_transaction_registry(single_collection_database_transaction_registry const&) =
        delete;
    inline single_collection_database_transaction_registry& operator=(
        single_collection_database_transaction_registry const&) = delete;
    inline single_collection_database_transaction_registry& operator=(
        single_collection_database_transaction_registry&&) = delete;
    inline ~single_collection_database_transaction_registry() noexcept {
        assert(roster_.live() == 0 && "Transactions outlive registry");
    }

#pragma endregion Construction

#pragma region Interface Methods

    /** Whether @p id names a transaction this registry holds. */
    inline bool has_transaction(transaction_id_t id) const noexcept { return roster_.contains(id); }

    /** The bytes the conflict record and the roster together occupy, which no commit ever changes. */
    inline std::size_t shared_state_bytes() const noexcept { return record_.size_bytes() + roster_.size_bytes(); }

    inline return_transaction_t create_transaction(transaction_id_t id, transaction_config_t const& config,
                                                   std::size_t value_size, std::size_t value_max_size) noexcept;

#pragma endregion Interface Methods

  private:
#pragma region Transactions

    friend store_t;
    friend transaction_t;

    inline status_t stage_(transaction_id_t id, transaction_state_t& data,
                           transaction_stamp_t& try_to_stage_stamp) noexcept;
    inline status_t commit_(transaction_id_t id, transaction_state_t& data,
                            transaction_stamp_t& try_to_commit_stamp) noexcept;
    inline void discard_(transaction_id_t id) noexcept;

    inline transaction_stamp_t request_start_(transaction_id_t id) noexcept;
    inline void invalidate_(transaction_id_t id) noexcept { roster_.stop(id); }
    inline void notify_about_deletion_(transaction_id_t id) noexcept;

    /** The lowest stamp any later commit can claim, which scopes a watch taken now. */
    inline transaction_stamp_t current_stamp_() const noexcept { return record_.watch_stamp(); }

    inline status_t watch_now_(transaction_stamp_t start_stamp,
                               std::span<stored_key_t const> stored_keys) const noexcept;

#pragma endregion Transactions

#pragma region Helper Methods

    inline bool conflicting_(transaction_state_t const& data) const noexcept;
    inline void record_commit_(write_buffer_t const& changes, transaction_stamp_t commit_stamp) noexcept;

#pragma endregion Helper Methods

#pragma region Store Accessor Methods

    inline void lock_shared_() const { store_->lock_shared(); }
    inline void unlock_shared_() const { store_->unlock_shared(); }

    inline auto begin_() const { return store_->begin(); }
    inline auto end_() const { return store_->end(); }
    inline auto find_(stored_key_t stored_key) const { return store_->find(stored_key); }
    inline auto find_or_next_(stored_key_t stored_key) const { return store_->find_or_next(stored_key); }

    inline bool contains_(stored_key_t stored_key) const noexcept { return store_->contains(stored_key); }
    inline status_t stored_value_size_(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        return store_->stored_value_size(stored_key, size);
    }
    inline stored_value_size_t buffer_size_for_any_stored_value_() const noexcept {
        return store_->buffer_size_for_any_stored_value();
    }

    inline status_t select_(stored_key_t stored_key, stored_value_span_t stored_value,
                            stored_value_size_t& size) const noexcept {
        return store_->select(stored_key, stored_value, size);
    }
    inline status_t find_(std::span<stored_key_t const> stored_keys, auto stored_value_locations,
                          missing_locations_t missing = missing_locations_t::invalidate_k) const noexcept {
        return store_->find(stored_keys, stored_value_locations, missing);
    }
    inline status_t select_(auto stored_value_locations, bytes_span_t buffer, notifier_t notifier) const noexcept {
        return store_->select(stored_value_locations, buffer, notifier);
    }

#pragma endregion Store Accessor Methods

#pragma region Private Members

    /** The collection every commit of this registry lands in, owned by whoever named it. */
    store_t* store_;

    /** The budget the record, the roster and every transaction's writes are charged to. */
    resources_shared_ptr_t resources_;

    /** The order every transaction is placed in, and the last commit to write each key. */
    commit_record_t record_;

    /** The live transactions and the staging queue pair. */
    transaction_roster_t roster_;

#pragma endregion Private Members
};

#pragma region Interface Methods

template <typename stored_key_, typename store_type_>
inline typename single_collection_database_transaction_registry<stored_key_, store_type_>::return_transaction_t
single_collection_database_transaction_registry<stored_key_, store_type_>::create_transaction(
    transaction_id_t id, transaction_config_t const& config, std::size_t value_size,
    std::size_t value_max_size) noexcept {

    status_t const enrolled = roster_.enroll(id);
    if (enrolled.failed()) return enrolled;

    write_buffer_t changes(config.write_buffer_max_bytes, config.write_buffer_max_elements, 1, value_size,
                           value_max_size, resources_);
    transaction_unique_ptr_t transaction {new (std::nothrow) transaction_t {id, *this, std::move(changes), resources_}};
    if (!transaction) {
        roster_.retire(id);
        return status_t {status_code_t::out_of_memory_k, "No room for another transaction"};
    }
    return transaction;
}

#pragma endregion Interface Methods

#pragma region Transactions

template <typename stored_key_, typename store_type_>
inline status_t single_collection_database_transaction_registry<stored_key_, store_type_>::stage_(
    transaction_id_t id, transaction_state_t& data, transaction_stamp_t& try_to_stage_stamp) noexcept {

    roster_.claim_staging(id);
    store_->acquire_write_path();
    try_to_stage_stamp = record_.watch_stamp();

    // A transaction that neither wrote nor watched anything stages by having nothing to check.
    if (data.empty()) return status_t {};

    if (conflicting_(data)) {
        discard_(id);
        return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    }

    // Without writes, the absence of watched key collisions is the whole of the stage.
    if (data.changes.empty()) return status_t {};
    if (store_->collection_fits(data.changes)) return status_t {};

    status_t const flushed = store_->flush_collection();
    if (flushed.failed()) discard_(id);
    return flushed;
}

template <typename stored_key_, typename store_type_>
inline status_t single_collection_database_transaction_registry<stored_key_, store_type_>::commit_(
    transaction_id_t id, transaction_state_t& data, transaction_stamp_t& try_to_commit_stamp) noexcept {

    if (!roster_.staged_by(id)) {
        // Staging inside the commit makes both stamps equal, so a staged commit reports one instant.
        transaction_stamp_t staged_stamp = invalid_transaction_stamp_k;
        status_t const staged = stage_(id, data, staged_stamp);
        if (staged.failed()) {
            // The changes stay exactly as they were, so a caller can still log what it could not commit.
            try_to_commit_stamp = record_.watch_stamp();
            return staged;
        }
    }

    transaction_stamp_t const commit_stamp = record_.issue_stamp();
    try_to_commit_stamp = commit_stamp;

    status_t status = status_t {};
    if (!data.changes.empty()) {
        // The warning is published before the writes it warns about, so a reader never sees the
        // second without the first, and a refused write leaves a conflict that never happened.
        record_commit_(data.changes, commit_stamp);
        status = store_->insert_write_buffer(data.changes);
    }

    // Only now may a transaction start after this commit, because only now has all of it landed.
    record_.publish_stamp(commit_stamp);
    discard_(id);
    if (status.succeeded()) invalidate_(id);
    return status;
}

template <typename stored_key_, typename store_type_>
inline void single_collection_database_transaction_registry<stored_key_, store_type_>::discard_(
    [[maybe_unused]] transaction_id_t id) noexcept {

    assert(roster_.staged_by(id) && "The transaction is not in committing state");
    store_->release_write_path();
    roster_.release_staging();
}

template <typename stored_key_, typename store_type_>
inline transaction_stamp_t single_collection_database_transaction_registry<stored_key_, store_type_>::request_start_(
    transaction_id_t id) noexcept {

    // A start consumes no stamp: it only has to order before every commit that follows it.
    transaction_stamp_t const start_stamp = record_.watch_stamp();
    roster_.start(id, start_stamp);
    return start_stamp;
}

template <typename stored_key_, typename store_type_>
inline void single_collection_database_transaction_registry<stored_key_, store_type_>::notify_about_deletion_(
    transaction_id_t id) noexcept {

    if (roster_.staged_by(id)) discard_(id);
    roster_.stop(id);
    roster_.retire(id);
}

template <typename stored_key_, typename store_type_>
inline status_t single_collection_database_transaction_registry<stored_key_, store_type_>::watch_now_(
    transaction_stamp_t start_stamp, std::span<stored_key_t const> stored_keys) const noexcept {

    for (stored_key_t const& stored_key : stored_keys)
        if (record_.conflicts(watched_key_fingerprint(sole_collection_id_k, stored_key), start_stamp))
            return status_t {status_code_t::write_conflict_k, "Keys conflict"};
    return status_t {};
}

#pragma endregion Transactions

#pragma region Helper Methods

template <typename stored_key_, typename store_type_>
inline bool single_collection_database_transaction_registry<stored_key_, store_type_>::conflicting_(
    transaction_state_t const& data) const noexcept {

    bool conflict = false;
    [[maybe_unused]] st::status_t const walked = data.read_set.stored_keys.for_each(
        [&](auto const& watch) noexcept -> st::walk_control_t {
            conflict = record_.conflicts(watched_key_fingerprint(sole_collection_id_k, watch.key()),
                                         watch.value().watch_stamp);
            return conflict ? st::walk_control_t::halt_k : st::walk_control_t::resume_k;
        });
    return conflict;
}

template <typename stored_key_, typename store_type_>
inline void single_collection_database_transaction_registry<stored_key_, store_type_>::record_commit_(
    write_buffer_t const& changes, transaction_stamp_t commit_stamp) noexcept {

    [[maybe_unused]] st::status_t const written = changes.stored_values.for_each([&](auto const& entry) noexcept {
        record_.record(watched_key_fingerprint(sole_collection_id_k, entry.key()), commit_stamp);
    });
}

#pragma endregion Helper Methods

#pragma endregion Single Collection Database Transaction Registry

#pragma region Single Collection Database Snapshot

#pragma region Snapshot

/**
 *  @brief The level set of one single-collection database held against publication, and its stamp.
 *
 *  Move-only, because the hold underneath belongs to exactly one owner, and it borrows the tree it
 *  counted itself into, so the database must outlive every snapshot taken of it.
 */
template <typename key_slot_type_, typename volume_type_>
class single_collection_database_snapshot {
  public:

#pragma region Types

    /** The key slot every run of the held levels holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of the held levels lives on. */
    using volume_t = volume_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The hold itself, which keeps the level set alive for as long as this snapshot lives. */
    using levels_t = collection_snapshot<key_slot_t, volume_t>;

#pragma endregion Types

#pragma region Construction

    single_collection_database_snapshot() noexcept = default;
    single_collection_database_snapshot(single_collection_database_snapshot const&) = delete;
    single_collection_database_snapshot& operator=(single_collection_database_snapshot const&) = delete;
    single_collection_database_snapshot(single_collection_database_snapshot&&) noexcept = default;
    single_collection_database_snapshot& operator=(single_collection_database_snapshot&&) noexcept = default;

    /**
     *  @brief The snapshot @p levels holds, as of @p stamp.
     *  @param[in] levels The hold the collection handed out, moved in and owned from here on.
     *  @param[in] stamp The watermark every commit inside this snapshot orders strictly before.
     */
    [[nodiscard]] static single_collection_database_snapshot of(levels_t&& levels, transaction_stamp_t stamp) noexcept {
        single_collection_database_snapshot taken;
        taken.levels_ = std::move(levels);
        taken.stamp_ = stamp;
        return taken;
    }

#pragma endregion Construction

#pragma region Shape

    /** Whether this snapshot holds a level set. */
    [[nodiscard]] bool holding() const noexcept { return levels_.holding(); }

    /** The watermark every commit inside this snapshot orders strictly before. */
    [[nodiscard]] transaction_stamp_t stamp() const noexcept { return stamp_; }

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
        return levels_.find(stored_key, size);
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[inout] channel The channel the read is submitted and completed on, held by the calling thread.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     */
    status_t select(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) const noexcept {
        return levels_.select(channel, stored_key, into, size);
    }

    /**
     *  @brief Reads the values of @p stored_keys, refilling the channel as completions retire.
     *  @param[inout] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     */
    template <typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<select_result_t> results, sink_type_&& sink) const noexcept {
        return levels_.select(channel, stored_keys, results, std::forward<sink_type_>(sink));
    }

#pragma endregion Lookups

  private:

#pragma region Private Members

    /** The hold itself, released when the snapshot dies. */
    levels_t levels_;

    /** The watermark every commit inside this snapshot orders strictly before. */
    transaction_stamp_t stamp_ = unwritten_stamp_k;

#pragma endregion Private Members
};

#pragma endregion Snapshot

#pragma endregion Single Collection Database Snapshot

#pragma region Single Collection Database Transaction

#pragma region Transaction

/**
 *  @brief An indivisible group of reads and writes over one single-collection database.
 *
 *  Reusable after a @c commit and after a @c rollback, both of which start it again from the moment
 *  they ran. Nothing throws: every modifying call answers a status, and a caller reading
 *  @c status_code_t::write_conflict_k rolls back and does the work again.
 */
template <typename stored_key_, typename store_type_>
class single_collection_database_transaction {
  public:

#pragma region Types

    /** The key the database is addressed by. */
    using stored_key_t = stored_key_;

    /** The database every commit of this transaction lands in. */
    using store_t = store_type_;

    /** The registry that orders this transaction against every other one of the database. */
    using registry_t = single_collection_database_transaction_registry<stored_key_, store_type_>;

    /** The uncommitted changes and the watched keys of this transaction. */
    using transaction_state_t = single_collection_database_transaction_state<stored_key_t>;

    /** The write buffer the uncommitted changes live in. */
    using write_buffer_t = typename transaction_state_t::write_buffer_t;

#pragma endregion Types

#pragma region Construction

    single_collection_database_transaction(single_collection_database_transaction const&) = delete;
    single_collection_database_transaction& operator=(single_collection_database_transaction const&) = delete;
    single_collection_database_transaction(single_collection_database_transaction&&) = delete;
    single_collection_database_transaction& operator=(single_collection_database_transaction&&) = delete;

    /**
     *  @brief Enrols a transaction of @p id, which starts running the moment it is built.
     *  @param[in] id The identifier the registry holds a roster slot under, unique among live transactions.
     *  @param[inout] manager The registry this transaction stages and commits through, outliving it.
     *  @param[in] changes The write buffer the uncommitted writes land in, moved in and owned from here on.
     *  @param[in] resources The budget the watched keys are charged to.
     */
    single_collection_database_transaction(transaction_id_t id, registry_t& manager, write_buffer_t&& changes,
                                           resources_shared_ptr_t resources) noexcept
        : manager_(manager), data_(std::move(changes), std::move(resources)), id_(id) {
        start_stamp_ = manager_.request_start_(id_);
        state_ = state_t::started_k;
    }

    /** Retires the roster slot, handing back the staging queue pair when this transaction still holds it. */
    ~single_collection_database_transaction() noexcept { manager_.notify_about_deletion_(id_); }

#pragma endregion Construction

#pragma region Two Phase Commit

    /**
     *  @brief Checks the watched keys and takes the write path, leaving the commit itself for later.
     *
     *  A failure keeps the changes exactly as they were, so a caller can log what it could not commit,
     *  and hands the staging queue pair back, so the transaction is running again rather than staged.
     */
    status_t stage() noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is not running"};
        transaction_stamp_t staged_stamp = invalid_transaction_stamp_k;
        if (status_t const staged = manager_.stage_(id_, data_, staged_stamp); staged.failed()) return staged;
        last_stage_stamp_ = staged_stamp;
        state_ = state_t::staged_k;
        return {};
    }

    /**
     *  @brief Stages whatever is not staged and lands every change, starting the transaction again.
     *
     *  The registry hands the staging queue pair back whatever it answers, so this transaction is running
     *  again either way; a failure keeps the changes for logging and a @c rollback drops them.
     */
    status_t commit() noexcept {
        if (state_ == state_t::invalid_k) return {status_code_t::invalid_argument_k, "That transaction is not running"};
        status_t const committed = manager_.commit_(id_, data_, last_try_to_commit_stamp_);
        state_ = state_t::started_k;
        if (committed.failed()) return committed;

        last_stage_stamp_ = last_try_to_commit_stamp_;
        last_commit_stamp_ = last_try_to_commit_stamp_;
        data_.clear();
        start_stamp_ = manager_.request_start_(id_);
        return {};
    }

    /** Drops every change and every watch, and starts the transaction again from this moment. */
    void rollback() noexcept {
        if (state_ == state_t::staged_k) manager_.discard_(id_);
        data_.clear();
        start_stamp_ = manager_.request_start_(id_);
        state_ = state_t::started_k;
    }

    /**
     *  @brief Starts the transaction again from this moment, keeping its changes.
     *
     *  Legal only for a transaction that has read nothing, since a read taken before the new start
     *  would be checked against a stamp it never ran at.
     */
    status_t renew() noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (data_.read_set.count(watch_purpose_t::read_k) != 0)
            return {status_code_t::invalid_argument_k, "That transaction has read something"};
        manager_.invalidate_(id_);
        start_stamp_ = manager_.request_start_(id_);
        return {};
    }

#pragma endregion Two Phase Commit

#pragma region Writes

    /** Whether @p count more values of @p bytes together still fit in this transaction's write buffer. */
    [[nodiscard]] bool has_available_space_for(std::size_t count, std::size_t bytes) const noexcept {
        return state_ == state_t::started_k && data_.changes.can_fit(count, bytes);
    }

    /** Writes @p stored_value under @p stored_key, which reaches the database only at the commit. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t insert(stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (!manager_.store_->admits(stored_value.size()))
            return {status_code_t::malformed_k, "This database admits no value of that size"};

        std::span<stored_key_t const> const only {&stored_key, 1};
        if (status_t const prepared = watch_now_or_prepare_<watch_schedule_>(only); prepared.failed()) return prepared;
        transaction_stamp_t const watch_from = watch_stamp_for_<watch_schedule_>();
        if (status_t const staged = data_.changes.add(stored_key, stored_value); staged.failed()) return staged;
        watch_later_<watch_schedule_>(only, watch_from, watch_purpose_t::write_k);
        return {};
    }

    /** Erases @p stored_key, which reaches the database as a tombstone only at the commit. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t erase(stored_key_t stored_key) noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};

        std::span<stored_key_t const> const only {&stored_key, 1};
        if (status_t const prepared = watch_now_or_prepare_<watch_schedule_>(only); prepared.failed()) return prepared;
        transaction_stamp_t const watch_from = watch_stamp_for_<watch_schedule_>();
        if (status_t const staged = data_.changes.mark_as_removed(stored_key); staged.failed()) return staged;
        watch_later_<watch_schedule_>(only, watch_from, watch_purpose_t::write_k);
        return {};
    }

    /** Watches @p stored_keys for conflicts until the next commit or rollback, without reading them. */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t watch_more(std::span<stored_key_t const> stored_keys) noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (status_t const prepared = watch_now_or_prepare_<watch_schedule_>(stored_keys); prepared.failed())
            return prepared;
        watch_later_<watch_schedule_>(stored_keys, watch_stamp_for_<watch_schedule_>(), watch_purpose_t::track_k);
        return {};
    }

#pragma endregion Writes

#pragma region Lookups

    /**
     *  @brief Whether anything carries @p stored_key, watching the database when it is what answered.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c success_k.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    [[nodiscard]] select_result_t find(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        size = 0;
        if (state_ != state_t::started_k) return select_result_t::nothing_k;
        select_result_t const staged = find_uncommitted(stored_key, size);
        if (staged != select_result_t::nothing_k) return staged;

        std::span<stored_key_t const> const only {&stored_key, 1};
        transaction_stamp_t const watch_from = watch_stamp_for_<watch_schedule_>();
        if (watch_now_or_prepare_<watch_schedule_>(only).failed()) return select_result_t::nothing_k;
        select_result_t const carried = manager_.store_->find(stored_key, size);
        watch_later_<watch_schedule_>(only, watch_from, watch_purpose_t::read_k);
        return carried;
    }

    /** What this transaction's own write buffer holds under @p stored_key, without touching the database. */
    [[nodiscard]] select_result_t find_uncommitted(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        size = 0;
        stored_value_size_t held = 0;
        select_result_t const staged = visit_uncommitted_(stored_key, [&](std::span<byte_t const> bytes) noexcept {
            held = static_cast<stored_value_size_t>(bytes.size());
        });
        size = held;
        return staged;
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, from this transaction or from the database.
     *  @param[inout] channel The channel a read of the database is submitted and completed on, held by the caller.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k>
    status_t select(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) const noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        size = 0;
        bool refused = false;
        select_result_t const staged = visit_uncommitted_(stored_key, [&](std::span<byte_t const> bytes) noexcept {
            if (into.size() < bytes.size()) {
                refused = true;
                return;
            }
            if (!bytes.empty()) std::memcpy(into.data(), bytes.data(), bytes.size());
            size = static_cast<stored_value_size_t>(bytes.size());
        });
        if (refused) return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};
        if (staged == select_result_t::erased_k)
            return {status_code_t::not_found_k, "This transaction erased that key"};
        if (staged == select_result_t::success_k) return {};

        // The watch is scoped from before the read, so a commit racing this read is refused by it.
        std::span<stored_key_t const> const only {&stored_key, 1};
        transaction_stamp_t const watch_from = watch_stamp_for_<watch_schedule_>();
        if (status_t const prepared = watch_now_or_prepare_<watch_schedule_>(only); prepared.failed()) return prepared;
        status_t const read = manager_.store_->select(channel, stored_key, into, size);
        watch_later_<watch_schedule_>(only, watch_from, watch_purpose_t::read_k);
        return read;
    }

    /**
     *  @brief Reads the values of @p stored_keys, this transaction's own writes ahead of the database.
     *  @param[inout] channel The channel a read of the database is submitted and completed on, held by the caller.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *
     *  @note A value this transaction wrote arrives as one call carrying all of it, while one the
     *      database answers arrives as one call per block, exactly as a read of the database does.
     */
    template <watch_schedule_t watch_schedule_ = watch_schedule_t::default_k, typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<select_result_t> results, sink_type_&& sink) const noexcept {
        if (state_ != state_t::started_k) return {status_code_t::invalid_argument_k, "That transaction is staged"};
        if (results.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched select answers one result per key"};

        st::basic_vector<stored_key_t> deferred_keys;
        st::basic_vector<std::size_t> deferred_requests;
        if (deferred_keys.reserve(stored_keys.size()) != st::status_t::success_k ||
            deferred_requests.reserve(stored_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A transaction could not track the keys of a batch"};

        for (std::size_t request = 0; request != stored_keys.size(); ++request) {
            select_result_t const staged = visit_uncommitted_(
                stored_keys[request],
                [&](std::span<byte_t const> bytes) noexcept { sink(request, std::uint64_t {0}, bytes); });
            results[request] = staged;
            if (staged != select_result_t::nothing_k) continue;
            [[maybe_unused]] st::status_t const named = deferred_keys.push_back(stored_key_t {stored_keys[request]});
            [[maybe_unused]] st::status_t const tracked = deferred_requests.push_back(std::size_t {request});
        }
        if (deferred_keys.empty()) return {};

        st::basic_vector<select_result_t> deferred_results;
        if (deferred_results.resize(deferred_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A transaction could not track the keys of a batch"};

        std::span<stored_key_t const> const watched {deferred_keys};
        transaction_stamp_t const watch_from = watch_stamp_for_<watch_schedule_>();
        if (status_t const prepared = watch_now_or_prepare_<watch_schedule_>(watched); prepared.failed())
            return prepared;

        auto relay = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            sink(deferred_requests[request], offset, bytes);
        };
        status_t const read = manager_.store_->select(channel, watched, std::span<select_result_t> {deferred_results},
                                                      relay);
        watch_later_<watch_schedule_>(watched, watch_from, watch_purpose_t::read_k);
        if (read.failed()) return read;
        for (std::size_t index = 0; index != deferred_results.size(); ++index)
            results[deferred_requests[index]] = deferred_results[index];
        return {};
    }

#pragma endregion Lookups

#pragma region Stamps

    /** The stamp this transaction was created or started again at, which scopes every watch. */
    [[nodiscard]] transaction_stamp_t start_stamp() const noexcept { return start_stamp_; }

    /** The stamp of the last successful stage. */
    [[nodiscard]] transaction_stamp_t last_stage_stamp() const noexcept { return last_stage_stamp_; }

    /** The stamp of the last successful commit. */
    [[nodiscard]] transaction_stamp_t last_commit_stamp() const noexcept { return last_commit_stamp_; }

    /** The stamp of the last commit attempt, whether it landed or not. */
    [[nodiscard]] transaction_stamp_t last_try_to_commit_stamp() const noexcept { return last_try_to_commit_stamp_; }

#pragma endregion Stamps

  private:

#pragma region Private Types

    /** Where one transaction is between its creation and its commit. */
    enum class state_t : std::uint8_t {

        /** Built but not yet started, and left there by nothing a caller can observe. */
        invalid_k,

        /** Running: it reads, it writes, and it holds no queue pair. */
        started_k,

        /** Staged: it holds the staging queue pair and the write path until it commits or rolls back. */
        staged_k,
    };

#pragma endregion Private Types

#pragma region Private Methods

    /**
     *  @brief Calls @p visitor with the bytes this transaction wrote under @p stored_key.
     *  @param[in] stored_key The key to look for in this transaction's own write buffer.
     *  @param[in] visitor Called once with the staged bytes, which are empty for an empty value.
     *  @return What the write buffer holds, which is @c nothing_k when it holds nothing at all.
     */
    template <typename visitor_type_>
    [[nodiscard]] select_result_t visit_uncommitted_(stored_key_t stored_key, visitor_type_&& visitor) const noexcept {
        select_result_t answer = select_result_t::nothing_k;
        data_.changes.stored_values.probe_to_find(stored_key, [&](auto const& entry) noexcept {
            auto const& place = entry.value();
            if (place.offset == tombstone_offset_k) {
                answer = select_result_t::erased_k;
                return;
            }
            answer = select_result_t::success_k;
            if (place.size == 0) {
                visitor(std::span<byte_t const> {});
                return;
            }
            visitor(std::span<byte_t const> {data_.changes.stored_values_buffer.data() + place.offset, place.size});
        });
        return answer;
    }

    /** The stamp a watch taken under @p watch_schedule_ is scoped from. */
    template <watch_schedule_t watch_schedule_>
    [[nodiscard]] transaction_stamp_t watch_stamp_for_() const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k) return start_stamp_;
        else if constexpr (watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return manager_.current_stamp_();
        else return invalid_transaction_stamp_k;
    }

    /** Checks @p stored_keys now, or makes the room a later watch of them takes no allocation in. */
    template <watch_schedule_t watch_schedule_>
    status_t watch_now_or_prepare_(std::span<stored_key_t const> stored_keys) const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::now_k) return manager_.watch_now_(start_stamp_, stored_keys);
        else if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k ||
                           watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            return data_.read_set.reserve_more(stored_keys.size());
        else return {};
    }

    /** Records a watch of every key of @p stored_keys from @p watch_stamp, in the room prepared for it. */
    template <watch_schedule_t watch_schedule_>
    void watch_later_(std::span<stored_key_t const> stored_keys, transaction_stamp_t watch_stamp,
                      watch_purpose_t purpose) const noexcept {
        if constexpr (watch_schedule_ == watch_schedule_t::on_commit_k ||
                      watch_schedule_ == watch_schedule_t::on_commit_since_operation_k)
            for (stored_key_t const& stored_key : stored_keys)
                data_.read_set.add(stored_key, watch_stamp, purpose, st::assume_reserved);
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The registry this transaction stages and commits through, borrowed and outliving it. */
    registry_t& manager_;

    /** The uncommitted changes and the watched keys, which a read of this transaction also walks. */
    transaction_state_t mutable data_;

    /** The roster slot this transaction holds, released when it dies. */
    transaction_id_t id_ = invalid_transaction_id_k;

    /** The stamp this transaction was created or started again at. */
    transaction_stamp_t start_stamp_ = invalid_transaction_stamp_k;

    /** The stamp of the last successful stage. */
    transaction_stamp_t last_stage_stamp_ = invalid_transaction_stamp_k;

    /** The stamp of the last successful commit. */
    transaction_stamp_t last_commit_stamp_ = invalid_transaction_stamp_k;

    /** The stamp of the last commit attempt, whether it landed or not. */
    transaction_stamp_t last_try_to_commit_stamp_ = invalid_transaction_stamp_k;

    /** Where this transaction is between its creation and its commit. */
    state_t state_ = state_t::invalid_k;

#pragma endregion Private Members
};

#pragma endregion Transaction

#pragma endregion Single Collection Database Transaction

#pragma region Single Collection Database

#pragma region Schema

/** The eight bytes that tell the schema blob of this layer from every other blob of a volume. */
inline constexpr std::uint64_t single_collection_database_magic_k = 0x5343444231303031ull;

/** The layout of the schema record, of which a mount reads exactly one version. */
inline constexpr std::uint32_t single_collection_database_format_version_k = 2;

/**
 *  @brief What a single-collection database writes into its schema blob, and a reopen checks against.
 *
 *  Fixed width and trivially copyable, so the record is the bytes: the volume already carries the
 *  database identity in its own head, and what is left is the shape a reopen must not contradict.
 *  The run records the blob carries after this one are what a reopen puts back into the tree.
 */
struct single_collection_database_schema_record_t {

    /** The magic every schema blob of this layer opens with. */
    std::uint64_t magic = single_collection_database_magic_k;

    /** The layout version this record follows. */
    std::uint32_t format_version = single_collection_database_format_version_k;

    /** The @c value_layout_t the collection was created under. */
    std::uint16_t value_layout = 0;

    /** Written as zero, so the record is the same bytes on every compiler that packs it. */
    std::uint16_t padding = 0;

    /** Bytes the largest value the collection admits takes. */
    std::uint64_t value_max_size = 0;

    /** Runs the blob names after this record, in the order their levels hold them. */
    std::uint64_t run_count = 0;
};

static_assert(sizeof(single_collection_database_schema_record_t) == 32, "The schema record is its own bytes");

#pragma endregion Schema

#pragma region Database

/**
 *  @brief One collection, its transactions, its snapshots and its two blobs, over one volume.
 *
 *  Pinned rather than movable: the registry and every live transaction point back here, so a database
 *  is opened into a @c std::unique_ptr and stays where it was opened.
 */
template <typename key_slot_type_, typename volume_type_>
class basic_single_collection_database {
  public:

#pragma region Types

    /** The key slot every run of this database holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run and every blob of this database lives on. */
    using volume_t = volume_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The transport the volume moves bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_t&>().disk())>;

    /** The collection every write of this database lands in. */
    using collection_t = collection<key_slot_t, volume_t>;

    /** An ordered walk over that collection, staged writes merged in front of every run. */
    using scanner_t = collection_scanner<key_slot_t, volume_t>;

    /** One moment of this database, which a reader holds and a publication waits behind. */
    using snapshot_t = single_collection_database_snapshot<key_slot_t, volume_t>;

    /** One optimistic transaction over this database. */
    using transaction_t = single_collection_database_transaction<stored_key_t, basic_single_collection_database>;

    /** The order every transaction of this database is placed in, and its conflicts. */
    using registry_t = single_collection_database_transaction_registry<stored_key_t, basic_single_collection_database>;

    /** The uncommitted changes of one transaction, as the registry hands them over. */
    using write_buffer_t = write_buffer<stored_key_t>;

    /** A database, which is pinned and therefore always owned through a pointer. */
    using database_unique_ptr_t = std::unique_ptr<basic_single_collection_database>;

    /** A transaction, which is owned by whoever created it and retires when it dies. */
    using transaction_unique_ptr_t = std::unique_ptr<transaction_t>;

    /** How the values of this database are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

#pragma endregion Types

#pragma region Construction

    basic_single_collection_database(basic_single_collection_database const&) = delete;
    basic_single_collection_database& operator=(basic_single_collection_database const&) = delete;
    basic_single_collection_database(basic_single_collection_database&&) = delete;
    basic_single_collection_database& operator=(basic_single_collection_database&&) = delete;

    /** Writes whatever is still staged onto the volume and names every run, so the data outlives this. */
    ~basic_single_collection_database() noexcept {
        write_path_guard_t const held {*this};
        [[maybe_unused]] status_t const flushed = collection_.flush(writing_channel_);
        // A destroyed database dropped its schema, and a close must not write one back over an empty volume.
        if (!volume_.names(schema_blob_name_())) return;
        [[maybe_unused]] status_t const saved = save_schema_();
    }

    /**
     *  @brief Opens a database over @p volume, creating its schema blob when the volume holds none.
     *  @param[inout] volume The volume every run and blob lands on, borrowed and outliving the database.
     *  @param[inout] writing_channel The channel every write, flush and commit runs on, borrowed for the database's
     *      life.
     *  @param[inout] merging_channel The second channel a compaction writes its new runs on, which is not @p
     *      writing_channel.
     *  @param[in] config The collection's bounds and the transaction defaults.
     *  @param[in] resources The budget the conflict record, the roster and every transaction's writes are charged to.
     *  @return The database, or @c malformed_k for a volume whose schema names another value layout.
     */
    static expected<database_unique_ptr_t> open(volume_t& volume, io_channel_t& writing_channel,
                                                io_channel_t& merging_channel,
                                                single_collection_database_config_t const& config,
                                                resources_shared_ptr_t resources) noexcept {
        if (!resources) return status_t {status_code_t::invalid_argument_k, "A database is charged to some budget"};
        if (&writing_channel == &merging_channel)
            return status_t {status_code_t::invalid_argument_k, "A compaction reads and writes on two channels"};

        auto [mounted, mount_collection_status] = collection_t::mount(volume, config.collection,
                                                                      resources->memory_constraint);
        if (mount_collection_status.failed()) return mount_collection_status;

        database_unique_ptr_t opened {new (std::nothrow) basic_single_collection_database {
            volume, writing_channel, merging_channel, config, std::move(resources), std::move(mounted)}};
        if (!opened) return status_t {status_code_t::out_of_memory_k, "The database did not fit"};
        if (status_t const agreed = opened->agree_on_schema_(); agreed.failed()) return agreed;
        return opened;
    }

    /** Drops both blobs and every run, which destroys the data and leaves the volume empty of it. */
    status_t destroy() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        status_t first = drop_blob_(schema_blob_name_());
        if (status_t const dropped = drop_blob_(user_data_blob_name_()); dropped.failed() && first.succeeded())
            first = dropped;
        if (status_t const emptied = collection_.destroy(writing_channel_); emptied.failed() && first.succeeded())
            first = emptied;
        return first;
    }

#pragma endregion Construction

#pragma region Shape

    /** The bounds this database was opened with. */
    [[nodiscard]] single_collection_database_config_t const& config() const noexcept { return config_; }

    /** The collection every write lands in, which a scan walks and a benchmark measures. */
    [[nodiscard]] collection_t const& records() const noexcept { return collection_; }

    /** Bytes the largest value this database admits takes. */
    [[nodiscard]] stored_value_size_t value_max_size() const noexcept { return collection_.value_max_size(); }

    /** Whether a value of @p size bytes is one this database's layout can hold. */
    [[nodiscard]] bool admits(std::size_t size) const noexcept {
        if constexpr (is_fixed(value_layout_k)) return size == collection_.value_max_size();
        else if constexpr (is_inline(value_layout_k)) return size != 0 && size <= collection_.value_max_size();
        else return size <= collection_.value_max_size();
    }

    /** Keys staged plus key slots every level holds, one key counted once per run that holds it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept {
        read_path_guard_t const held {*this};
        return collection_.population_count();
    }

    /** Bytes every run of every level takes on the volume, answered without excluding a writer. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return collection_.size_bytes(); }

    /** Writes staged and not yet flushed. */
    [[nodiscard]] std::size_t staged_count() const noexcept {
        read_path_guard_t const held {*this};
        return collection_.staged_count();
    }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether anything carries @p stored_key, and how many bytes its value takes.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c success_k.
     */
    [[nodiscard]] select_result_t find(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        read_path_guard_t const held {*this};
        return collection_.find(stored_key, size);
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[inout] channel The channel the read is submitted and completed on, held by the calling thread.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     */
    status_t select(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) const noexcept {
        read_path_guard_t const held {*this};
        return collection_.select(channel, stored_key, into, size);
    }

    /**
     *  @brief Reads the values of @p stored_keys, refilling the channel as completions retire.
     *  @param[inout] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     */
    template <typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<select_result_t> results, sink_type_&& sink) const noexcept {
        read_path_guard_t const held {*this};
        return collection_.select(channel, stored_keys, results, std::forward<sink_type_>(sink));
    }

#pragma endregion Lookups

#pragma region Writes

    /** Stages @p stored_value under @p stored_key on the writing channel, flushing when the buffer is full. */
    status_t insert(stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        return collection_.insert(writing_channel_, stored_key, stored_value);
    }

    /** Stages a tombstone under @p stored_key on the writing channel. */
    status_t erase(stored_key_t stored_key) noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        return collection_.erase(writing_channel_, stored_key);
    }

    /**
     *  @brief Stages one value per key on the writing channel, flushing when they do not all fit.
     *  @param[in] stored_keys The keys to write, in any order and with repeats taking the last value.
     *  @param[in] stored_values The values back to back, in the order their keys name them.
     *  @param[in] sizes Bytes of every value, one per key, which must add up to @p stored_values.
     */
    status_t insert(std::span<stored_key_t const> stored_keys, std::span<byte_t const> stored_values,
                    std::span<stored_value_size_t const> sizes) noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        return collection_.insert(writing_channel_, stored_keys, stored_values, sizes);
    }

    /** Stages a tombstone under every key of @p stored_keys on the writing channel. */
    status_t erase(std::span<stored_key_t const> stored_keys) noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        return collection_.erase(writing_channel_, stored_keys);
    }

#pragma endregion Writes

#pragma region Maintenance

    /** Writes every staged write as one run of level zero, leaving the write buffer empty. */
    status_t flush() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        if (status_t const flushed = collection_.flush(writing_channel_); flushed.failed()) return flushed;
        // The run the flush wrote is sealed by now, so the schema may name it.
        return save_schema_();
    }

    /** Whether any level is over the bytes its number admits, answered without excluding a writer. */
    [[nodiscard]] bool needs_compaction() const noexcept { return collection_.needs_compaction(); }

    /** Merges the shallowest level that is over its bounds, reading on one channel and writing on the other. */
    expected<compaction_outcome_t> compact() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        lsm_tree_publication_hook_t published;
        published.notify = &record_schema_;
        published.context = this;
        return collection_.tree().compact(writing_channel_, merging_channel_, compaction_placement_t::automatic_k,
                                          nullptr, published);
    }

    /** Flushes, then holds the level set the flush published together with the stamp it holds it at. */
    expected<snapshot_t> take_snapshot() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        auto [levels, take_snapshot_status] = collection_.take_snapshot(writing_channel_);
        if (take_snapshot_status.failed()) return take_snapshot_status;
        return snapshot_t::of(std::move(levels), registry_.current_stamp_());
    }

#pragma endregion Maintenance

#pragma region Transactions

    /** A transaction of @p id over this database, with the write-buffer bounds @p config names. */
    expected<transaction_unique_ptr_t> create_transaction(transaction_id_t id,
                                                          transaction_config_t const& config) noexcept {
        if (config.write_buffer_max_bytes > config_.collection.write_buffer_max_bytes ||
            config.write_buffer_max_elements > config_.collection.write_buffer_max_elements)
            return status_t {status_code_t::invalid_argument_k,
                             "A transaction may not outgrow the write buffer it commits into"};

        // The write buffer takes exactly one of the two widths, and which one is the layout's own answer.
        std::size_t const exact = is_fixed(value_layout_k) ? collection_.value_max_size() : 0;
        std::size_t const bounded = is_fixed(value_layout_k) ? 0 : collection_.value_max_size();
        return registry_.create_transaction(id, config, exact, bounded);
    }

    /** A transaction of @p id with the write-buffer bounds this database was opened with. */
    expected<transaction_unique_ptr_t> create_transaction(transaction_id_t id) noexcept {
        return create_transaction(id, config_.default_transaction);
    }

    /** Whether @p id names a transaction this database holds. */
    [[nodiscard]] bool has_transaction(transaction_id_t id) const noexcept { return registry_.has_transaction(id); }

#pragma endregion Transactions

#pragma region User Data

    /** Bytes the owner's blob takes beside the data, which is zero when the volume holds none. */
    [[nodiscard]] std::uint64_t user_data_size_bytes() const noexcept {
        expected<volume_blob_t> const found = volume_.find(user_data_blob_name_());
        return found ? found->length : 0;
    }

    /** Replaces the owner's blob with @p bytes, which the volume keeps beside the runs. */
    status_t save_user_data(std::span<byte_t const> bytes) noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        if (bytes.empty()) return drop_blob_(user_data_blob_name_());
        return write_blob_(user_data_blob_name_(), blob_kind_t::user_data_k, bytes);
    }

    /** Copies the owner's blob into @p into, which must be at least as long as the blob. */
    status_t load_user_data(io_channel_t& channel, std::span<byte_t> into) const noexcept {
        auto [found, find_status] = volume_.find(user_data_blob_name_());
        if (find_status.failed()) return find_status;
        if (into.size() < found.length) return {status_code_t::invalid_argument_k, "A buffer shorter than the blob"};
        return read_blob_(channel, found, into.first(static_cast<std::size_t>(found.length)));
    }

    /** Forgets the owner's blob, which a volume holding none reports as @c not_found_k. */
    status_t clear_user_data() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        return drop_blob_(user_data_blob_name_());
    }

#pragma endregion User Data

#pragma region Transaction Store Interface

    /**
     *  @brief Takes the collection for one writer, waiting while another writer or a reader holds it.
     *
     *  Held from the moment a transaction stages until it commits or rolls back, so every reader sees
     *  all of a commit's writes or none of them.
     */
    void acquire_write_path() { acquire_write_path_(); }

    /** Hands the collection back to the readers and the next writer. */
    void release_write_path() { release_write_path_(); }

    /**
     *  @brief Whether the collection could stage @p changes without flushing first.
     *
     *  Counted in keys alone, since the collection flushes on its own when the bytes do not fit, so a
     *  wrong answer here costs a flush and never correctness.
     */
    [[nodiscard]] bool collection_fits(write_buffer_t const& changes) const noexcept {
        return collection_.staged_count() + changes.count() <= config_.collection.write_buffer_max_elements;
    }

    /** Writes every staged write as one run, called by the registry under the write path it took. */
    status_t flush_collection() {
        if (write_path_adoption_.failed()) return write_path_adoption_;
        return collection_.flush(writing_channel_);
    }

    /** Stages every change of @p changes, called by the registry under the write path it took. */
    status_t insert_write_buffer(write_buffer_t const& changes) {
        if (write_path_adoption_.failed()) return write_path_adoption_;
        status_t first;
        [[maybe_unused]] st::status_t const walked = changes.stored_values.for_each([&](auto const& entry) noexcept {
            auto const& place = entry.value();
            status_t const written = place.offset == tombstone_offset_k
                                         ? collection_.erase(writing_channel_, entry.key())
                                         : collection_.insert(writing_channel_, entry.key(),
                                                              changed_bytes_(changes, place));
            if (written.failed() && first.succeeded()) first = written;
            return st::walk_control_t::resume_k;
        });
        return first;
    }

#pragma endregion Transaction Store Interface

  private:

#pragma region Private Types

    /** One reader's hold on the collection, which a writer waits behind and which never nests. */
    class read_path_guard_t {
      public:

        explicit read_path_guard_t(basic_single_collection_database const& owner) noexcept : owner_(&owner) {
            owner_->acquire_read_path_();
        }
        read_path_guard_t(read_path_guard_t const&) = delete;
        read_path_guard_t& operator=(read_path_guard_t const&) = delete;
        ~read_path_guard_t() noexcept { owner_->release_read_path_(); }

      private:

        /** The database whose collection this guard holds, borrowed for the guard's life. */
        basic_single_collection_database const* owner_;
    };

    /** One writer's hold on the collection, which every reader and every other writer waits behind. */
    class write_path_guard_t {
      public:

        explicit write_path_guard_t(basic_single_collection_database& owner) noexcept : owner_(&owner) {
            owner_->acquire_write_path_();
        }
        write_path_guard_t(write_path_guard_t const&) = delete;
        write_path_guard_t& operator=(write_path_guard_t const&) = delete;
        ~write_path_guard_t() noexcept { owner_->release_write_path_(); }

        /** Whether this thread took the channels over, which every write through them needs. */
        status_t adopted() const noexcept { return owner_->write_path_adoption_; }

      private:

        /** The database whose collection this guard holds, borrowed for the guard's life. */
        basic_single_collection_database* owner_;
    };

#pragma endregion Private Types

#pragma region Private Construction

    /** Takes what @c open assembled, and points the registry at the database it will commit into. */
    basic_single_collection_database(volume_t& volume, io_channel_t& writing_channel, io_channel_t& merging_channel,
                                     single_collection_database_config_t const& config,
                                     resources_shared_ptr_t resources, collection_t&& mounted) noexcept
        : volume_(volume), writing_channel_(writing_channel), merging_channel_(merging_channel), config_(config),
          collection_(std::move(mounted)), registry_(*this, std::move(resources)) {}

#pragma endregion Private Construction

#pragma region Write Path

    /** Takes the collection for one reader, waiting while a writer holds it. */
    void acquire_read_path_() const noexcept {
        fu::preferred_atomic_ref<std::int32_t> const claim {write_path_claim_};
        for (;;) {
            claim_word_t readers = claim.load(std::memory_order_relaxed);
            if (readers >= 0 && claim.compare_exchange_strong(readers, readers + 1, std::memory_order_acquire,
                                                              std::memory_order_relaxed))
                return;
            fu::preferred_yield_t {}(write_path_claim_, readers, std::size_t {0});
        }
    }

    /** Counts one reader back out of the collection. */
    void release_read_path_() const noexcept {
        [[maybe_unused]] std::int32_t const before =
            fu::preferred_atomic_ref<claim_word_t> {write_path_claim_}.fetch_sub(1, std::memory_order_release);
    }

    /**
     *  @brief Takes the collection for one writer, waiting while any reader or writer holds it.
     *
     *  A queue pair belongs to the thread that announced itself on it, so the winner adopts both channels
     *  before it writes through them; the claim is what makes that handoff a handoff and not a share.
     */
    void acquire_write_path_() noexcept {
        fu::preferred_atomic_ref<std::int32_t> const claim {write_path_claim_};
        claim_word_t expected_idle = 0;
        while (!claim.compare_exchange_strong(expected_idle, writer_claim_k, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
            fu::preferred_yield_t {}(write_path_claim_, expected_idle, std::size_t {0});
            expected_idle = 0;
        }
        write_path_adoption_ = volume_.disk().bind_queue_pair(writing_channel_.queue_pair());
        if (write_path_adoption_.succeeded())
            write_path_adoption_ = volume_.disk().bind_queue_pair(merging_channel_.queue_pair());
    }

    /** Hands the collection back to the readers and the next writer. */
    void release_write_path_() noexcept {
        fu::preferred_atomic_ref<claim_word_t> {write_path_claim_}.store(0, std::memory_order_release);
    }

#pragma endregion Write Path

#pragma region Private Methods

    /** The bytes @p place names inside @p changes, empty for an empty value. */
    [[nodiscard]] static std::span<byte_t const> changed_bytes_(write_buffer_t const& changes,
                                                                auto const& place) noexcept {
        if (place.size == 0) return {};
        return std::span<byte_t const> {changes.stored_values_buffer.data() + place.offset, place.size};
    }

    /** The name the schema blob answers to on the volume. */
    [[nodiscard]] static blob_name_t schema_blob_name_() noexcept {
        static constexpr char text[] = "single-collection-database.schema";
        return blob_name_t {reinterpret_cast<byte_t const*>(text), sizeof(text) - 1};
    }

    /** The name the owner's blob answers to on the volume. */
    [[nodiscard]] static blob_name_t user_data_blob_name_() noexcept {
        static constexpr char text[] = "single-collection-database.user";
        return blob_name_t {reinterpret_cast<byte_t const*>(text), sizeof(text) - 1};
    }

    /** The shape this database would write into its schema blob, less the runs that follow it there. */
    [[nodiscard]] single_collection_database_schema_record_t wanted_schema_() const noexcept {
        single_collection_database_schema_record_t wanted;
        wanted.value_layout = static_cast<std::uint16_t>(value_layout_k);
        wanted.value_max_size = collection_.value_max_size();
        return wanted;
    }

    /** Reads the schema blob and takes its runs back, or writes the one this database would have written. */
    status_t agree_on_schema_() noexcept {
        write_path_guard_t const held {*this};
        if (held.adopted().failed()) return held.adopted();
        single_collection_database_schema_record_t const wanted = wanted_schema_();

        expected<volume_blob_t> const found = volume_.find(schema_blob_name_());
        if (!found) return write_schema_(std::span<lsm_tree_run_record_t const> {});
        if (found->length < sizeof(wanted))
            return {status_code_t::malformed_k, "The volume's schema blob is shorter than one record"};

        st::basic_vector<byte_t> image;
        if (image.resize(static_cast<std::size_t>(found->length)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to read the schema into"};
        if (status_t const read = read_blob_(writing_channel_, *found, std::span<byte_t> {image}); read.failed())
            return read;

        single_collection_database_schema_record_t carried;
        std::memcpy(&carried, image.data(), sizeof(carried));
        if (carried.magic != single_collection_database_magic_k)
            return {status_code_t::malformed_k, "The volume's schema blob carries another magic"};
        if (carried.format_version != single_collection_database_format_version_k)
            return {status_code_t::unsupported_k, "The volume's schema follows another format version"};
        if (carried.value_layout != wanted.value_layout || carried.value_max_size != wanted.value_max_size)
            return {status_code_t::invalid_argument_k, "The volume holds a collection of another value layout"};
        if (image.size() - sizeof(carried) != carried.run_count * sizeof(lsm_tree_run_record_t))
            return {status_code_t::malformed_k, "The volume's schema names another number of runs"};

        st::basic_vector<lsm_tree_run_record_t> runs;
        if (runs.resize(static_cast<std::size_t>(carried.run_count)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for the runs the schema names"};
        if (!runs.empty())
            std::memcpy(runs.data(), image.data() + sizeof(carried), runs.size() * sizeof(lsm_tree_run_record_t));
        return collection_.tree().restore(writing_channel_, std::span<lsm_tree_run_record_t const> {runs});
    }

    /** Names every run the tree holds in the schema blob, which every publication of a run does. */
    status_t save_schema_() noexcept {
        st::basic_vector<lsm_tree_run_record_t> runs;
        if (status_t const named = record_lsm_tree_runs(collection_.tree(), runs); named.failed()) return named;
        return write_schema_(std::span<lsm_tree_run_record_t const> {runs});
    }

    /** Lays the schema record and @p runs out back to back and replaces the schema blob with them. */
    status_t write_schema_(std::span<lsm_tree_run_record_t const> runs) noexcept {
        single_collection_database_schema_record_t record = wanted_schema_();
        record.run_count = runs.size();
        st::basic_vector<byte_t> image;
        if (image.resize(sizeof(record) + runs.size() * sizeof(lsm_tree_run_record_t)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to lay the schema out"};
        std::memcpy(image.data(), &record, sizeof(record));
        if (!runs.empty())
            std::memcpy(image.data() + sizeof(record), runs.data(), runs.size() * sizeof(lsm_tree_run_record_t));
        return write_blob_(schema_blob_name_(), blob_kind_t::schema_k, std::span<byte_t const> {image});
    }

    /** Writes the schema out for a compaction that has published, before it drops what it consumed. */
    static status_t record_schema_(void* context) noexcept {
        return static_cast<basic_single_collection_database*>(context)->save_schema_();
    }

    /** Forgets @p name, which a volume holding no such blob reports as @c not_found_k. */
    status_t drop_blob_(blob_name_t name) noexcept { return volume_.drop(writing_channel_, name); }

    /** Writes @p bytes as the blob @p name of @p kind on the writing channel, replacing any blob of that name. */
    status_t write_blob_(blob_name_t name, blob_kind_t kind, std::span<byte_t const> bytes) noexcept {
        if (volume_.names(name))
            if (status_t const dropped = volume_.drop(writing_channel_, name); dropped.failed()) return dropped;

        auto [reserved, create_status] = volume_.create(name, kind, bytes.size());
        if (create_status.failed()) return create_status;
        if (status_t const written = fill_blob_(reserved, bytes); written.failed()) {
            [[maybe_unused]] status_t const abandoned = volume_.drop(writing_channel_, name);
            return written;
        }
        if (status_t const sealed = volume_.seal(writing_channel_, reserved); sealed.failed()) {
            [[maybe_unused]] status_t const abandoned = volume_.drop(writing_channel_, name);
            return sealed;
        }
        return {};
    }

    /** Moves @p bytes into the range @p blob reserved, one block at a time and padded to a page. */
    status_t fill_blob_(volume_blob_t const& blob, std::span<byte_t const> bytes) noexcept {
        basic_write_pipeline<disk_t> writer {volume_.disk(), writing_channel_, volume_.medium()};
        std::uint64_t at = 0;
        while (at != bytes.size()) {
            auto [block, acquire_buffer_or_poll_status] = writer.acquire_buffer_or_poll();
            if (acquire_buffer_or_poll_status.failed()) return acquire_buffer_or_poll_status;
            std::uint64_t const room = round_down_to_multiple(block.size(), portable_alignment_k);
            if (room == 0) return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than one page"};

            std::uint64_t const carried = room < bytes.size() - at ? room : bytes.size() - at;
            std::uint64_t const padded = round_up_to_multiple(carried, portable_alignment_k);
            std::span<byte_t> const room_bytes = block.bytes();
            std::memcpy(room_bytes.data(), bytes.data() + at, static_cast<std::size_t>(carried));
            std::memset(room_bytes.data() + carried, 0, static_cast<std::size_t>(padded - carried));
            if (status_t const queued = writer.enqueue_write(std::move(block), blob.slice(at, padded)); queued.failed())
                return queued;
            at += carried;
        }
        return writer.make_durable();
    }

    /** Copies the first @c into.size() bytes of @p blob into @p into, one block at a time. */
    status_t read_blob_(io_channel_t& channel, volume_blob_t const& blob, std::span<byte_t> into) const noexcept {
        std::uint64_t const chunk = round_down_to_multiple(channel.requests().buffer_bytes(), portable_alignment_k);
        if (chunk == 0) return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than one page"};
        std::uint64_t const wanted = round_up_to_multiple(into.size(), portable_alignment_k);

        status_t first;
        auto sink = [&](completed_buffer_t const& completed) noexcept {
            if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                if (first.succeeded()) first = whole;
                return;
            }
            std::uint64_t const at = static_cast<std::uint64_t>(completed.tag) * chunk;
            if (at >= into.size()) return;
            std::uint64_t const room = into.size() - at;
            std::uint64_t const carried = completed.bytes.size() < room ? completed.bytes.size() : room;
            std::memcpy(into.data() + at, completed.bytes.data(), static_cast<std::size_t>(carried));
        };

        basic_read_pipeline<disk_t> reader {volume_.disk(), channel, volume_.medium()};
        std::uint64_t at = 0;
        request_tag_t tag = 0;
        while (at != wanted) {
            while (at != wanted && reader.can_enqueue()) {
                std::uint64_t const length = chunk < wanted - at ? chunk : wanted - at;
                if (status_t const offered = reader.enqueue_read(blob.slice(at, length), tag); offered.failed())
                    return offered;
                at += length;
                ++tag;
            }
            // The offers sit staged until the channel submits them, and a poll_completions waits only on what it did.
            if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
            if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
        }
        return first;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The volume every run and every blob lives on, borrowed and outliving the database. */
    volume_t& volume_;

    /** The channel every write, flush and commit runs on, borrowed and outliving the database. */
    io_channel_t& writing_channel_;

    /** The second channel a compaction writes its new runs on, borrowed and outliving the database. */
    io_channel_t& merging_channel_;

    /** The bounds this database was opened with. */
    single_collection_database_config_t config_ {};

    /** The collection every write lands in. */
    collection_t collection_;

    /** The order every transaction is placed in, and the last commit to write each key. */
    registry_t registry_;

    /** Readers inside the collection, or minus one while a writer holds it. */
    claim_word_t mutable write_path_claim_ = 0;

    /** Whether the writer holding the path took both channels over, written only under the claim. */
    status_t write_path_adoption_;

#pragma endregion Private Members
};

#pragma endregion Database

#pragma endregion Single Collection Database

} // namespace unum::ustore::blobs
