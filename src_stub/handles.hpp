/**
 *  @file src_stub/handles.hpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The store and the handles of the stand-in: claims, versions, collections and the
 *      structures every opaque handle of the C interface names.
 *
 *  Every collection is one ordered map from key bytes to the versions of its value, the RAM
 *  engine's semantics in the plainest standard-library shape. A commit appends one version per key
 *  at a stamp one past the head, and a reader sees the newest version at or below its stamp.
 */
#ifndef UNUM_USTORE_STUB_HANDLES_HPP
#define UNUM_USTORE_STUB_HANDLES_HPP

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint32_t`
#include <cstring> // `std::memset`

#include <atomic>             // `std::atomic`
#include <condition_variable> // `std::condition_variable` - a worker waiting for its next deferred call
#include <map>                // `std::map`
#include <mutex>              // `std::mutex`
#include <new>                // `std::align_val_t`, `std::nothrow`
#include <set>                // `std::multiset`, `std::set`
#include <shared_mutex>       // `std::shared_mutex`
#include <string>             // `std::string`
#include <thread>             // `std::thread::id`
#include <utility>            // `std::pair`
#include <vector>             // `std::vector`

#include "ustore/ustore.h"

namespace unum::ustore::stub {

/** The lesser of @p first and @p second, spelled apart from the @c min macro of @c <windows.h>. */
template <typename value_type_>
constexpr value_type_ smaller_of(value_type_ first, value_type_ second) noexcept {
    return second < first ? second : first;
}

/** The greater of @p first and @p second, spelled apart from the @c max macro of @c <windows.h>. */
template <typename value_type_>
constexpr value_type_ larger_of(value_type_ first, value_type_ second) noexcept {
    return first < second ? second : first;
}

using stamp_t = ustore_stamp_t;

/** Tells a live handle of each kind apart from foreign memory, so a view made by neither
 *  constructor is refused rather than followed. */
enum class magic_t : std::uint32_t {
    database_k = 0x55444231,
    snapshot_k = 0x55534e31,
    transaction_k = 0x55545831,
    cursor_k = 0x55435231,
};

/** One call at a time: an executor, an arena, a transaction, a cursor, an ingest or a plan. */
struct claim_t {

    /** The outcome of the call holding the claim, or null while free. */
    std::atomic<void const*> holder {nullptr};

    /** Takes the claim for @p owner, or answers true when @p owner, a deferred call submitted
     *  with it, holds it. */
    bool take(void const* owner) noexcept {
        void const* expected = nullptr;
        if (holder.compare_exchange_strong(expected, owner, std::memory_order_acquire, std::memory_order_relaxed))
            return true;
        return expected == owner;
    }
    void release() noexcept { holder.store(nullptr, std::memory_order_release); }

    /** Lets go of the claim only when @p owner holds it. */
    void release(void const* owner) noexcept {
        void const* expected = owner;
        holder.compare_exchange_strong(expected, nullptr, std::memory_order_release, std::memory_order_relaxed);
    }
    bool held() const noexcept { return holder.load(std::memory_order_acquire) != nullptr; }
};

/** A deferred call handed to the worker of its executor: the routine running it and what
 *  it runs over. */
struct deferred_job_t {
    void (*run)(void* state) = nullptr;
    void* state = nullptr;
};

/** The calls naming a handle, which its free waits out. */
using named_calls_t = std::atomic<std::uint32_t>;

/** A status and its static message, null exactly on success. */
struct status_t {
    ustore_status_t code = ustore_status_success_k;
    char const* message = nullptr;

    bool failed() const noexcept { return code != ustore_status_success_k; }
};

/** One version of one key: present with a value, or a removal, published at a stamp. */
struct version_t {
    stamp_t stamp = 0;
    bool present = false;
    std::string value;
};

/** One committed change a feed hands out. */
struct change_t {
    stamp_t stamp = 0;
    std::string key;
    ustore_change_kind_t kind = ustore_change_write_k;
    std::string value;
};

/** One collection: its identity, and every key with the versions a pinned reader may still need. */
struct collection_t {
    ustore_collection_id_t id {};
    std::string name;
    ustore_modality_t modality = ustore_modality_blobs_k;
    stamp_t created = 0;
    std::map<std::string, std::vector<version_t>> entries;
    std::vector<change_t> changes;
};

/** A write a transaction buffers until it commits, or a head call until it lands. */
struct pending_t {
    bool present = false;
    std::string value;
    ustore_change_kind_t kind = ustore_change_write_k;
    bool watched = true;
};

/** The writes to one collection, by key. */
using overlay_t = std::map<std::string, pending_t>;

/** The writes of a call or a transaction, by collection identifier. */
using batch_t = std::map<std::string, overlay_t>;

/** A window a strictly serializable transaction walked, which a commit inside it conflicts with. */
struct window_t {
    std::string collection;
    std::string low;
    std::string high;
    bool bounded = false;
};

} // namespace unum::ustore::stub

using namespace unum::ustore::stub;

/** What @c ustore_executor_t names: a host executor whose calls run on their callers. */
struct ustore_executor_handle_t {
    claim_t claim;

    /** The thread serving the call holding the executor, so a drain from its
     *  callback is refused. */
    std::atomic<std::thread::id> body_thread {};

    /** Whether @c ustore_executor_for_nvme made it, which keeps its options. */
    bool for_nvme = false;
    ustore_nvme_options_t nvme {};
    ustore_size_t threads = 1;

    /** The thread running the deferred calls of an executor owning its threads, absent for a
     *  caller's pool, whose deferred calls run at submission. */
    std::thread worker;

    /** The deferred call the worker runs next, null while it waits, or the stop job once the
     *  executor frees, handed over under @c job_mutex, as atomic waits would tie the archive to
     *  one C++ runtime. */
    deferred_job_t* job = nullptr;
    std::mutex job_mutex;
    std::condition_variable job_changed;
};

/** What @c ustore_arena_t names: host blocks every call rewinds, each 64-byte aligned as
 *  Arrow reads them. */
struct ustore_arena_handle_t {
    claim_t claim;
    named_calls_t named_calls {0};

    struct block_t {
        std::byte* data = nullptr;
        std::size_t size = 0;
    };
    std::vector<block_t> blocks;

    ustore_arena_handle_t() = default;
    ustore_arena_handle_t(ustore_arena_handle_t const&) = delete;
    ~ustore_arena_handle_t() { rewind(); }

    /** Frees every output of the previous call. */
    void rewind() noexcept {
        for (block_t const& block : blocks) ::operator delete(block.data, std::align_val_t {64});
        blocks.clear();
    }

    /** Whether @p pointer lies inside an output of the previous call. */
    bool contains(void const* pointer) const noexcept {
        auto const* const byte = static_cast<std::byte const*>(pointer);
        for (block_t const& block : blocks)
            if (byte >= block.data && byte < block.data + block.size) return true;
        return false;
    }

    /** Zeroed bytes behind the outputs already written, aligned to 64, or null when
     *  the allocation fails. */
    std::byte* allocate(std::size_t bytes) noexcept {
        std::size_t const size = bytes ? bytes : 1;
        auto* const data = static_cast<std::byte*>(::operator new(size, std::align_val_t {64}, std::nothrow));
        if (!data) return nullptr;
        std::memset(data, 0, size);
        try {
            blocks.push_back({data, size});
        }
        catch (...) {
            ::operator delete(data, std::align_val_t {64});
            return nullptr;
        }
        return data;
    }
};

/** What @c ustore_database_t names: every collection under one lock, and the pins
 *  that keep versions. */
struct ustore_database_handle_t {
    magic_t magic = magic_t::database_k;
    std::shared_mutex mutex;

    ustore_isolation_t isolation = ustore_isolation_strict_serializable_k;
    bool read_only = false;

    /** Directory the image persists in, empty for a volatile database. */
    std::string directory;

    stamp_t head = 0;
    std::map<std::string, collection_t> collections;

    /** Read stamps of every snapshot, transaction, feed and running call,
     *  which compaction keeps. */
    std::multiset<stamp_t> pins;

    /** Stamps the open feeds read from, which keep the changes logged after them. */
    std::multiset<stamp_t> feed_pins;

    std::size_t snapshots_open = 0;

    /** The one live compaction plan, or null. */
    ustore_database_compact_plan_handle_t* live_plan = nullptr;

    named_calls_t named_calls {0};

    /** The opener's hold, and one per transaction, snapshot, cursor and ingest, the last of
     *  which deletes it. */
    std::atomic<std::uint32_t> holds {1};
};

/** What @c ustore_snapshot_t names: a pinned stamp. */
struct ustore_snapshot_handle_t {
    magic_t magic = magic_t::snapshot_k;
    ustore_database_handle_t* database = nullptr;
    stamp_t stamp = 0;
    named_calls_t named_calls {0};
    std::atomic<std::uint32_t> holds {1};
};

/** What @c ustore_transaction_t names: a read stamp, the writes it buffers, and what it watched. */
struct ustore_transaction_handle_t {
    magic_t magic = magic_t::transaction_k;
    ustore_database_handle_t* database = nullptr;
    stamp_t read_stamp = 0;
    bool staged = false;
    batch_t writes;
    std::set<std::pair<std::string, std::string>> reads;
    std::vector<window_t> windows;
    claim_t claim;
    named_calls_t named_calls {0};
    std::atomic<std::uint32_t> holds {1};
};

/** What @c ustore_database_compact_plan_t names: the phase a compaction reached and the
 *  collections it listed. */
struct ustore_database_compact_plan_handle_t {
    ustore_database_handle_t* database = nullptr;
    ustore_executor_handle_t* executor = nullptr;
    std::vector<std::string> collections;
    ustore_phase_t phase = ustore_phase_none_k;

    /** Units of the current phase already done. */
    std::size_t completed = 0;
    claim_t claim;
    named_calls_t named_calls {0};
};

#endif // UNUM_USTORE_STUB_HANDLES_HPP
