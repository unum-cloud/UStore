/**
 *  @file src_stub/lib.hpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief Machinery every modality's stand-in shares: outcomes, claims, arenas, the versioned store
 *      under one lock, views, transactions, scans and the RAM engine's image.
 *
 *  Nothing here is part of the C ABI: each @c <modality>.cpp owns its verbs and includes this for
 *  the parts that have nothing to do with which modality it answers. The one lock per database is
 *  the reason a stand-in can be brute-force and still honest about concurrency: a binding that
 *  shares a handle across threads exercises real synchronization, not a comment promising some.
 *
 *  Every collection is one ordered map from key bytes to the versions of its value, the RAM
 *  engine's semantics in the plainest standard-library shape: blobs, documents and pages keyed
 *  by their 16 bytes, strings by their text, and graphs by the 49-byte half-edges the RAM engine
 *  files, so the image it writes reads back in either library, pages aside. A commit appends one
 *  version per key at a stamp one past the head, and a reader sees the newest version at or
 *  below its stamp.
 *
 *  Every call runs on its caller, a deferred one too, as an executor without threads of its own
 *  runs it at submission, while a call meeting a running one on its executor, arena, transaction or
 *  cursor finishes contended. Every refusal follows the ruled order: a null handle, then invalid
 *  arguments, read-only, unsupported, and contention last.
 */
#ifndef UNUM_USTORE_STUB_LIB_HPP
#define UNUM_USTORE_STUB_LIB_HPP

#include <cctype>  // `std::tolower` - patterns that fold case
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <sys/stat.h> // `stat`, `S_ISDIR` - whether a directory exists

#include <algorithm>    // `std::shuffle`
#include <array>        // `std::array`
#include <atomic>       // `std::atomic`, `std::atomic_ref`
#include <bit>          // `std::bit_cast` - an enum read as its bits
#include <chrono>       // `std::chrono::system_clock`
#include <iterator>     // `std::prev`
#include <map>          // `std::map`
#include <mutex>        // `std::unique_lock`
#include <numeric>      // `std::iota`
#include <new>          // `std::bad_alloc`
#include <random>       // `std::random_device`, `std::mt19937_64`
#include <regex>        // `std::regex` - the patterns of a scan
#include <set>          // `std::multiset`
#include <shared_mutex> // `std::shared_lock`
#include <string>       // `std::string`
#include <string_view>  // `std::string_view`
#include <thread>       // `std::this_thread::yield`
#include <type_traits>  // `std::underlying_type_t`
#include <utility>      // `std::move`
#include <vector>       // `std::vector`

#include "handles.hpp"

namespace unum::ustore::stub {

#pragma region Bytes

/** The run read as text: a path, a name or a key, which are bytes that happen to decode. */
inline std::string_view text_of(ustore_bytes_view_t bytes) noexcept {
    if (!bytes.data) return {};
    return {reinterpret_cast<char const*>(bytes.data), static_cast<std::size_t>(bytes.length)};
}

/** Whether @p bytes spells @p spelling exactly, with no terminator assumed on either side. */
inline bool spelled(ustore_bytes_view_t bytes, std::string_view spelling) noexcept {
    return text_of(bytes) == spelling;
}

/** The 16 bytes of a key, which order as the map orders strings. */
inline std::string bytes_of(ustore_key_t const& key) {
    return {reinterpret_cast<char const*>(key.uuid.bytes), sizeof(key.uuid.bytes)};
}

/** The 16 bytes of a collection identifier, which index the collections of a database. */
inline std::string bytes_of(ustore_collection_id_t const& id) {
    return {reinterpret_cast<char const*>(id.uuid.bytes), sizeof(id.uuid.bytes)};
}

/** The key the first 16 bytes of @p bytes spell, or the nil key for fewer. */
inline ustore_key_t key_of(std::string_view bytes) noexcept {
    ustore_key_t key {};
    if (bytes.size() >= sizeof(key.uuid.bytes)) std::memcpy(key.uuid.bytes, bytes.data(), sizeof(key.uuid.bytes));
    return key;
}

/** The collection identifier 16 bytes spell. */
inline ustore_collection_id_t id_of(std::string_view bytes) noexcept {
    ustore_collection_id_t id {};
    if (bytes.size() >= sizeof(id.uuid.bytes)) std::memcpy(id.uuid.bytes, bytes.data(), sizeof(id.uuid.bytes));
    return id;
}

/** Whether every byte of @p key is zero, the nil UUID. */
inline bool is_nil(ustore_key_t const& key) noexcept {
    for (std::uint8_t byte : key.uuid.bytes)
        if (byte) return false;
    return true;
}

/** Element @p index of a strided run, where a zero stride repeats the first. */
template <typename element_type_>
element_type_ const& strided_element(element_type_ const* data, ustore_size_t stride, std::size_t index) noexcept {
    return *reinterpret_cast<element_type_ const*>(reinterpret_cast<char const*>(data) + index * stride);
}

/** Whether a directory exists at @p path. */
inline bool directory_exists(std::string const& path) noexcept {
    struct stat info {};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

/** Whether @p text holds a NUL byte, which no path the system opens may hold. */
inline bool holds_nul(std::string_view text) noexcept { return text.find('\0') != std::string_view::npos; }

#pragma endregion Bytes

#pragma region Claims and Outcomes

/** Waits until no call names the handle @p named counts for. */
inline void wait_named(named_calls_t const& named) noexcept {
    while (named.load(std::memory_order_acquire)) std::this_thread::yield();
}

/** Stores @p status into @p outcome with release ordering, the note first, so a waiter
 *  reading the status with acquire ordering reads every output too. */
inline void publish(ustore_outcome_t* outcome, ustore_status_t status, char const* note) noexcept {
    if (!outcome) return;
    outcome->message = status == ustore_status_success_k ? nullptr : note;
    std::atomic_ref<ustore_status_t>(outcome->status).store(status, std::memory_order_release);
}

/** Loads the status of @p outcome with acquire ordering. */
inline ustore_status_t load_status(ustore_outcome_t* outcome) noexcept {
    return std::atomic_ref<ustore_status_t>(outcome->status).load(std::memory_order_acquire);
}

/**
 *  @brief A verb's outcome while it runs: deferred from construction, and published once at
 *      destruction with the first refusal recorded, or success.
 *
 *  Every verb declares it first, so it unwinds last: each claim the verb took is let go,
 *  handles first and the executor last, before the final status lets a waiter free or reuse
 *  what the call named.
 */
class call_t {
    ustore_outcome_t* outcome_;
    status_t recorded_ {};
    std::array<claim_t*, 8> claims_ {};
    std::size_t claims_count_ = 0;
    std::array<named_calls_t*, 8> named_ {};
    std::size_t named_count_ = 0;
    ustore_executor_handle_t* executor_ = nullptr;

  public:
    explicit call_t(ustore_outcome_t* outcome) noexcept : outcome_(outcome) {
        if (outcome_) std::atomic_ref<ustore_status_t>(outcome_->status).store(ustore_status_deferred_k);
    }
    call_t(call_t const&) = delete;
    call_t& operator=(call_t const&) = delete;
    ~call_t() noexcept {
        while (claims_count_) claims_[--claims_count_]->release();
        while (named_count_) named_[--named_count_]->fetch_sub(1, std::memory_order_acq_rel);
        if (executor_) {
            executor_->body_thread.store(std::thread::id {}, std::memory_order_relaxed);
            executor_->claim.release();
        }
        publish(outcome_, recorded_.code, recorded_.note);
    }

    /** Records @p status unless an earlier refusal stands. */
    void refuse(ustore_status_t status, char const* note) noexcept {
        if (!recorded_.failed()) recorded_ = {status, note};
    }

    /** Records @p status and answers false, for a guard that answers whether its call may go on. */
    bool fail(ustore_status_t status, char const* note) noexcept { return refuse(status, note), false; }

    /** Records @p status and answers true, for a guard that answers whether it refused the call. */
    bool stop(ustore_status_t status, char const* note) noexcept { return refuse(status, note), true; }

    /** Records @p status when it failed, answering whether it did. */
    bool failed_with(status_t status) noexcept {
        if (status.failed()) refuse(status.code, status.note);
        return status.failed();
    }

    bool failed() const noexcept { return recorded_.failed(); }

    /** What tells this call apart in a claim: its outcome, or itself without one. */
    void const* owner() const noexcept { return outcome_ ? static_cast<void const*>(outcome_) : this; }
    status_t recorded() const noexcept { return recorded_; }

    /** Counts this call against the free of a handle until it is final. */
    void name(named_calls_t* named) noexcept {
        if (!named || named_count_ == named_.size()) return;
        named->fetch_add(1, std::memory_order_acq_rel);
        named_[named_count_++] = named;
    }

    /** Takes @p claim for the call, or refuses it contended with @p note. */
    bool claim(claim_t* claim, char const* note) noexcept {
        if (!claim) return true;
        if (!claim->take(owner())) return fail(ustore_status_contended_k, note);
        claims_[claims_count_++] = claim;
        return true;
    }

    /** Takes @p executor for the call, which a second call from any thread finds contended. */
    bool hold_executor(ustore_executor_handle_t* executor) noexcept {
        if (!executor) return true;
        if (!executor->claim.take(owner())) return fail(ustore_status_contended_k, "Another call holds the executor");
        executor->body_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
        executor_ = executor;
        return true;
    }
};

#pragma endregion Claims and Outcomes

#pragma region Arena Outputs

/** Claims @p arena for @p call, refusing a null one or one serving another call, and rewinds it,
 *  which frees the previous call's outputs, so any of @p inputs still inside it is refused. */
inline ustore_arena_handle_t* claim_arena(call_t& call, ustore_arena_t arena,
                                          std::initializer_list<void const*> inputs = {}) noexcept {
    if (!arena) {
        call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
        return nullptr;
    }
    if (!call.claim(&arena->claim, "Another call holds the arena")) return nullptr;
    if (arena->exports.load(std::memory_order_seq_cst) != 0) {
        call.refuse(ustore_status_contended_k, "An export of the arena's outputs is open");
        return nullptr;
    }
    for (void const* input : inputs)
        if (input && arena->contains(input)) {
            call.refuse(ustore_status_invalid_argument_k, "Input lies inside the arena the call rewinds");
            return nullptr;
        }
    arena->generation.fetch_add(1, std::memory_order_relaxed);
    arena->rewind();
    return arena;
}

/** Carves @p count zeroed values out of @p arena, or null for none; a failed
 *  allocation is recorded. */
template <typename value_type_>
value_type_* carve(call_t& call, ustore_arena_handle_t& arena, std::size_t count) noexcept {
    if (!count) return nullptr;
    std::byte* const bytes = arena.allocate(count * sizeof(value_type_));
    if (!bytes) call.refuse(ustore_status_out_of_memory_k, "Failed to grow the arena");
    return reinterpret_cast<value_type_*>(bytes);
}

/** Whether a tape written into an arena ends every string with a NUL byte its offsets span. */
enum class string_end_t : bool { open_k, terminated_k };

/** Writes @p texts into @p arena as a tape of 32-bit offsets while the bytes fit in 4 GiB and
 *  64-bit beyond, each followed by a NUL byte its offsets span when @p terminated asks. */
template <typename texts_type_>
bool write_tape(call_t& call, ustore_arena_handle_t& arena, texts_type_ const& texts, ustore_strings_view_t& tape,
                string_end_t end = string_end_t::open_k) noexcept {
    tape = {};
    std::size_t total = 0;
    bool const terminated = end == string_end_t::terminated_k;
    for (auto const& text : texts) total += std::string_view(text).size() + (terminated ? 1 : 0);
    std::size_t const count = texts.size();
    auto* const data = reinterpret_cast<std::uint8_t*>(arena.allocate(total));
    if (!data) return call.fail(ustore_status_out_of_memory_k, "Failed to grow the arena");
    bool const wide = total > 0xFFFFFFFFull;
    void* const offsets = arena.allocate((count + 1) * (wide ? 8 : 4));
    if (!offsets) return call.fail(ustore_status_out_of_memory_k, "Failed to grow the arena");
    auto* const wide_offsets = static_cast<std::uint64_t*>(offsets);
    auto* const narrow_offsets = static_cast<std::uint32_t*>(offsets);
    std::size_t written = 0, index = 0;
    for (auto const& text : texts) {
        std::string_view const view(text);
        if (wide) wide_offsets[index] = written;
        else narrow_offsets[index] = static_cast<std::uint32_t>(written);
        if (!view.empty()) std::memcpy(data + written, view.data(), view.size());
        written += view.size();
        if (terminated) data[written++] = 0;
        ++index;
    }
    if (wide) wide_offsets[count] = written;
    else narrow_offsets[count] = static_cast<std::uint32_t>(written);
    tape.layout = wide ? ustore_strings_u64_tape_k : ustore_strings_u32_tape_k;
    if (wide) tape.data.u64_tape = {data, static_cast<std::uint64_t const*>(offsets)};
    else tape.data.u32_tape = {data, static_cast<std::uint32_t const*>(offsets)};
    return true;
}

/** Sets bit @p index of an Arrow bitmap, least significant bit first. */
inline void set_bit(ustore_octet_t* bits, std::size_t index) noexcept {
    bits[index / 8] = static_cast<ustore_octet_t>(bits[index / 8] | (1u << (index % 8)));
}

#pragma endregion Arena Outputs

#pragma region Ragged Inputs

/** A ragged input read into views, with whether the caller gave one at all. */
struct strings_in_t {
    std::vector<std::string_view> items;
    bool given = false;

    std::string_view operator[](std::size_t index) const noexcept { return items[index]; }
    std::size_t size() const noexcept { return items.size(); }
};

/** Refuses a tape of @p count strings whose offsets step back or outrun its bytes. */
template <typename offset_type_>
bool tape_ascends(std::uint8_t const* data, offset_type_ const* offsets, std::size_t count) noexcept {
    for (std::size_t index = 0; index != count; ++index)
        if (offsets[index + 1] < offsets[index]) return false;
    return !count || data || offsets[count] == offsets[0];
}

/** Reads the @p count strings a tape of @p offset_type_ offsets holds, refusing one whose offsets
 *  step back. */
template <typename offset_type_>
bool read_tape(call_t& call, std::uint8_t const* data, offset_type_ const* offsets, std::size_t count,
               strings_in_t& strings) {
    if (!tape_ascends(data, offsets, count))
        return call.fail(ustore_status_invalid_argument_k, "A tape of strings needs ascending offsets");
    strings.items.reserve(count);
    for (std::size_t index = 0; index != count; ++index)
        strings.items.emplace_back(reinterpret_cast<char const*>(data) + offsets[index],
                                   static_cast<std::size_t>(offsets[index + 1] - offsets[index]));
    strings.given = true;
    return true;
}

/** Reads the @p count strings an accessor writes, asking it in blocks of 64 on the calling
 *  thread. */
inline bool read_accessor(call_t& call, ustore_strings_view_t const& view, std::size_t count, strings_in_t& strings) {
    strings.given = true;
    if (!count) return true;
    if (!view.data.accessor.get)
        return call.fail(ustore_status_invalid_argument_k, "An accessor of strings needs its get");
    std::vector<ustore_bytes_view_t> views(count);
    std::array<ustore_size_t, 64> indices {};
    for (std::size_t first = 0; first < count; first += indices.size()) {
        std::size_t const block = smaller_of<std::size_t>(indices.size(), count - first);
        std::iota(indices.begin(), indices.begin() + block, static_cast<ustore_size_t>(first));
        view.data.accessor.get(view.data.accessor.handle, indices.data(), block, views.data() + first);
    }
    strings.items.reserve(count);
    for (ustore_bytes_view_t const& bytes : views) strings.items.push_back(text_of(bytes));
    return true;
}

/**
 *  @brief Reads @p count strings of @p view into @p strings, refusing a layout no constant names,
 *      a descending tape, a tape missing its bytes, and an accessor missing its @c get.
 *
 *  A zeroed view, a u32 tape with null offsets, stands for the input's default and reads as none
 *  given. An accessor runs here, on the calling thread, in blocks of 64, before the call is final.
 */
inline bool read_strings(call_t& call, ustore_strings_view_t const& view, std::size_t count,
                         strings_in_t& strings) noexcept {
    strings = {};
    // A C caller may pass any integer, which C++ may not load as the enum, so its bits are read
    auto const layout = std::bit_cast<std::underlying_type_t<ustore_strings_layout_t>>(view.layout);
    auto const& narrow = view.data.u32_tape;
    auto const& wide = view.data.u64_tape;
    try {
        switch (layout) {
        case ustore_strings_u32_tape_k:
            return !narrow.offsets || read_tape(call, narrow.data, narrow.offsets, count, strings);
        case ustore_strings_u64_tape_k:
            if (wide.offsets) return read_tape(call, wide.data, wide.offsets, count, strings);
            if (count) return call.fail(ustore_status_invalid_argument_k, "A tape of strings needs its offsets");
            strings.given = true;
            return true;
        case ustore_strings_accessor_k: return read_accessor(call, view, count, strings);
        default: return call.fail(ustore_status_invalid_argument_k, "The strings come in an unknown layout");
        }
    }
    catch (...) {
        return call.fail(ustore_status_out_of_memory_k, "Failed to read the strings");
    }
}

/** The first input pointer of a ragged view, which a call checks against the arena it rewinds. */
inline void const* first_input(ustore_strings_view_t const& view) noexcept {
    switch (std::bit_cast<std::underlying_type_t<ustore_strings_layout_t>>(view.layout)) {
    case ustore_strings_u32_tape_k: return view.data.u32_tape.offsets;
    case ustore_strings_u64_tape_k: return view.data.u64_tape.offsets;
    default: return nullptr;
    }
}

/** The bytes of a ragged view, which a call checks against the arena it rewinds. */
inline void const* data_input(ustore_strings_view_t const& view) noexcept {
    switch (std::bit_cast<std::underlying_type_t<ustore_strings_layout_t>>(view.layout)) {
    case ustore_strings_u32_tape_k: return view.data.u32_tape.data;
    case ustore_strings_u64_tape_k: return view.data.u64_tape.data;
    default: return nullptr;
    }
}

#pragma endregion Ragged Inputs

#pragma region Store

/** The newest version of @p history at or below @p stamp, or null. */
inline version_t const* visible_version(std::vector<version_t> const& history, stamp_t stamp) noexcept {
    for (std::size_t index = history.size(); index != 0; --index)
        if (history[index - 1].stamp <= stamp) return history.data() + index - 1;
    return nullptr;
}

#pragma region Holds

/** Lets go of one hold on @p database, deleting it with the last. */
inline void release_database(ustore_database_handle_t* database) noexcept {
    if (database->holds.fetch_sub(1, std::memory_order_acq_rel) == 1) delete database;
}

/** Removes one occurrence of @p stamp from @p pins. */
inline void unpin(std::multiset<stamp_t>& pins, stamp_t stamp) noexcept {
    auto const found = pins.find(stamp);
    if (found != pins.end()) pins.erase(found);
}

/** Lets go of one hold on @p snapshot, unpinning its stamp and deleting it with the last. */
inline void release_snapshot(ustore_snapshot_handle_t* snapshot) noexcept {
    if (snapshot->holds.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    ustore_database_handle_t* const database = snapshot->database;
    {
        std::unique_lock lock(database->mutex);
        unpin(database->pins, snapshot->stamp);
        --database->snapshots_open;
    }
    delete snapshot;
    release_database(database);
}

/** Lets go of one hold on @p transaction, unpinning its stamp and deleting it with the last. */
inline void release_transaction(ustore_transaction_handle_t* transaction) noexcept {
    if (transaction->holds.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    ustore_database_handle_t* const database = transaction->database;
    {
        std::unique_lock lock(database->mutex);
        unpin(database->pins, transaction->read_stamp);
    }
    delete transaction;
    release_database(database);
}

/** The oldest stamp any reader still reads at, or the head. */
inline stamp_t oldest_pin(ustore_database_handle_t const& database) noexcept {
    return database.pins.empty() ? database.head : smaller_of(*database.pins.begin(), database.head);
}

/** Drops the versions of @p history no reader at or above @p oldest reaches: every one older
 *  than the newest at or below it. */
inline void prune(std::vector<version_t>& history, stamp_t oldest) {
    std::size_t keep_from = 0;
    for (std::size_t index = 0; index != history.size(); ++index)
        if (history[index].stamp <= oldest) keep_from = index;
    if (keep_from) history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(keep_from));
}

#pragma endregion Holds

#pragma region Views

/** The tags a view holds in the low bits of its parent's pointer. */
enum class view_tag_t : std::uintptr_t { snapshot_k = 1, watching_k = 2, unwatched_k = 3 };

/** The parent a view names, decoded: the head, a snapshot, or a transaction and its watch. */
struct view_spec_t {
    ustore_snapshot_handle_t* snapshot = nullptr;
    ustore_transaction_handle_t* transaction = nullptr;
    ustore_watch_t watch = ustore_watch_keys_k;
};

/** Tags @p parent with @p tag, allocating nothing. */
inline ustore_view_t tagged_view(void* parent, view_tag_t tag) noexcept {
    if (!parent) return nullptr;
    return reinterpret_cast<ustore_view_t>(reinterpret_cast<std::uintptr_t>(parent) | static_cast<std::uintptr_t>(tag));
}

/** Decodes @p view into @p spec, refusing one neither constructor made or one
 *  reading another database. */
inline bool decode_view(call_t& call, ustore_view_t view, ustore_database_handle_t* database,
                        view_spec_t& spec) noexcept {
    spec = {};
    if (!view) return true;
    auto const bits = reinterpret_cast<std::uintptr_t>(view);
    std::uintptr_t const tag = bits & 3u;
    void* const parent = reinterpret_cast<void*>(bits & ~std::uintptr_t {3});
    if (!tag || !parent)
        return call.fail(ustore_status_invalid_argument_k, "The view was made by neither view constructor");
    if (tag == static_cast<std::uintptr_t>(view_tag_t::snapshot_k)) {
        auto* const snapshot = static_cast<ustore_snapshot_handle_t*>(parent);
        if (snapshot->magic != magic_t::snapshot_k)
            return call.fail(ustore_status_invalid_argument_k, "The view was made by neither view constructor");
        if (database && snapshot->database != database)
            return call.fail(ustore_status_invalid_argument_k, "The view reads another database");
        spec.snapshot = snapshot;
        return true;
    }
    auto* const transaction = static_cast<ustore_transaction_handle_t*>(parent);
    if (transaction->magic != magic_t::transaction_k)
        return call.fail(ustore_status_invalid_argument_k, "The view was made by neither view constructor");
    if (database && transaction->database != database)
        return call.fail(ustore_status_invalid_argument_k, "The view reads another database");
    spec.transaction = transaction;
    spec.watch = tag == static_cast<std::uintptr_t>(view_tag_t::watching_k) ? ustore_watch_keys_k : ustore_watch_none_k;
    return true;
}

/** Whether reads through @p spec record their keys for conflicts: always under strict
 *  serializability, and for a view that watches otherwise. */
inline bool records_reads(view_spec_t const& spec) noexcept {
    if (!spec.transaction) return false;
    return spec.watch == ustore_watch_keys_k ||
           spec.transaction->database->isolation == ustore_isolation_strict_serializable_k;
}

/** Refuses a recording read through a staged transaction, which takes no more watches. */
inline bool refuse_staged_watch(call_t& call, view_spec_t const& spec) noexcept {
    if (spec.transaction && spec.transaction->staged && records_reads(spec))
        return call.stop(ustore_status_invalid_argument_k, "A watching read goes through a staged transaction");
    return false;
}

/** Names and claims what @p spec reads through for @p call: the transaction serves one call
 *  at a time. */
inline bool hold_view(call_t& call, view_spec_t const& spec) noexcept {
    if (spec.transaction) return call.claim(&spec.transaction->claim, "Another call holds the transaction");
    return true;
}

/** Counts @p call against the frees of what @p spec reads through. */
inline void name_view(call_t& call, view_spec_t const& spec) noexcept {
    if (spec.snapshot) call.name(&spec.snapshot->named_calls);
    if (spec.transaction) call.name(&spec.transaction->named_calls);
}

#pragma endregion Views

#pragma region Reading

/** How a walk starts from its bound: at the first key in its direction, at the bound, or
 *  just past it. */
enum class bound_t { none_k, inclusive_k, exclusive_k };

/** One entry a walk listed: its key and a copy of its value. */
struct entry_t {
    std::string key;
    std::string value;
};

/** The entry a seek finds: its key, and its value, null when no entry is left. */
struct found_entry_t {
    std::string key;
    std::string const* value = nullptr;
};

/** What one call reads: a database at a stamp, overlaid with a transaction's writes, recording
 *  what it reads when the transaction watches. Its caller holds the database lock. */
struct reader_t {
    ustore_database_handle_t* database = nullptr;
    stamp_t stamp = 0;
    ustore_transaction_handle_t* transaction = nullptr;
    bool records = false;

    /** The collection @p id names as this reader sees it, or null: a snapshot lists the
     *  collections created before it, while a transaction reaches every collection at the head,
     *  as the RAM engine's do. */
    collection_t const* find(std::string const& id) const noexcept {
        auto const found = database->collections.find(id);
        if (found == database->collections.end() || (!transaction && found->second.created > stamp)) return nullptr;
        return &found->second;
    }

    /** The overlay of the transaction for @p collection, or null. */
    overlay_t const* overlay(collection_t const& collection) const noexcept {
        if (!transaction) return nullptr;
        auto const found = transaction->writes.find(bytes_of(collection.id));
        return found == transaction->writes.end() ? nullptr : &found->second;
    }

    /** Records a read of @p key in @p collection for a commit to validate. */
    void record(collection_t const& collection, std::string const& key) const {
        if (records) transaction->reads.emplace(bytes_of(collection.id), key);
    }

    /** Records a walk of the window from @p low to @p high in @p collection for a
     *  strictly serializable commit. */
    void record_window(window_t window) const {
        if (records && database->isolation == ustore_isolation_strict_serializable_k)
            transaction->windows.push_back(std::move(window));
    }

    /** The value of @p key, or null when absent. */
    std::string const* get(collection_t const& collection, std::string const& key) const {
        record(collection, key);
        return peek(collection, key);
    }

    /** The value of @p key without recording the read. */
    std::string const* peek(collection_t const& collection, std::string const& key) const noexcept {
        if (overlay_t const* const pending = overlay(collection)) {
            auto const found = pending->find(key);
            if (found != pending->end()) return found->second.present ? &found->second.value : nullptr;
        }
        auto const found = collection.entries.find(key);
        if (found == collection.entries.end()) return nullptr;
        version_t const* const version = visible_version(found->second, stamp);
        return version && version->present ? &version->value : nullptr;
    }

    /** The first key at or past @p bound in @p direction, as @p kind says, that this reader
     *  sees, with its value. */
    found_entry_t seek(collection_t const& collection, std::string const& bound, bound_t kind,
                       ustore_scan_direction_t direction) const {
        overlay_t const* const pending = overlay(collection);
        bool const descending = direction == ustore_scan_descending_k;
        std::string cursor = bound;
        bound_t cursor_kind = kind;
        // The base and the overlay each step to their next candidate, the overlay winning a tie
        while (true) {
            std::string const* base_key = nullptr;
            std::string const* base_value = nullptr;
            {
                auto it = start_of(collection.entries, cursor, cursor_kind, direction);
                while (valid(collection.entries, it)) {
                    version_t const* const version = visible_version(it->second, stamp);
                    if (version && version->present) {
                        base_key = &it->first, base_value = &version->value;
                        break;
                    }
                    step(collection.entries, it, direction);
                }
            }
            std::string const* over_key = nullptr;
            pending_t const* over = nullptr;
            if (pending) {
                auto it = start_of(*pending, cursor, cursor_kind, direction);
                if (valid(*pending, it)) over_key = &it->first, over = &it->second;
            }
            if (!base_key && !over_key) return {};
            bool take_over = over_key && (!base_key || (descending ? *over_key >= *base_key : *over_key <= *base_key));
            if (!take_over) return {*base_key, base_value};
            if (over->present) return {*over_key, &over->value};
            // A pending removal hides the base key it shadows, so the walk resumes past it
            cursor = *over_key, cursor_kind = bound_t::exclusive_k;
        }
    }

  private:
    template <typename map_type_>
    static typename map_type_::const_iterator start_of(map_type_ const& map, std::string const& bound, bound_t kind,
                                                       ustore_scan_direction_t direction) {
        if (direction != ustore_scan_descending_k) {
            if (kind == bound_t::none_k) return map.begin();
            return kind == bound_t::inclusive_k ? map.lower_bound(bound) : map.upper_bound(bound);
        }
        // Descending walks end at `map.end()` as their sentinel
        typename map_type_::const_iterator it;
        if (kind == bound_t::none_k) it = map.end();
        else it = kind == bound_t::inclusive_k ? map.upper_bound(bound) : map.lower_bound(bound);
        if (it == map.begin()) return map.end();
        return std::prev(it);
    }
    template <typename map_type_>
    static bool valid(map_type_ const& map, typename map_type_::const_iterator it) noexcept {
        return it != map.end();
    }
    template <typename map_type_>
    static void step(map_type_ const& map, typename map_type_::const_iterator& it,
                     ustore_scan_direction_t direction) noexcept {
        if (direction != ustore_scan_descending_k) {
            ++it;
            return;
        }
        if (it == map.begin()) it = map.end();
        else --it;
    }
};

/** Pins the stamp a call reads at, so compaction keeps its versions while it runs
 *  unlocked between batches. */
class read_pin_t {
    ustore_database_handle_t* database_ = nullptr;
    stamp_t stamp_ = 0;

  public:
    read_pin_t() = default;
    read_pin_t(read_pin_t const&) = delete;
    ~read_pin_t() {
        if (!database_) return;
        std::unique_lock lock(database_->mutex);
        unpin(database_->pins, stamp_);
    }

    /** Pins the head of @p database, answering the stamp. */
    stamp_t pin(ustore_database_handle_t* database) {
        std::unique_lock lock(database->mutex);
        database_ = database, stamp_ = database->head;
        database->pins.insert(stamp_);
        return stamp_;
    }
};

/** The reader a call sees through @p spec over @p database, at @p head_stamp when it
 *  reads the head. */
inline reader_t reader_of(ustore_database_handle_t* database, view_spec_t const& spec, stamp_t head_stamp) noexcept {
    reader_t reader;
    reader.database = database;
    if (spec.snapshot) reader.stamp = spec.snapshot->stamp;
    else if (spec.transaction) {
        reader.stamp = spec.transaction->read_stamp;
        reader.transaction = spec.transaction;
        reader.records = records_reads(spec);
    }
    else reader.stamp = head_stamp;
    return reader;
}

/** Resolves every collection of @p count tasks, refusing a missing one and one of another
 *  modality than @p modality, under the lock @p reader holds, writing them into @p resolved. */
inline bool resolve_collections(call_t& call, reader_t const& reader, ustore_collections_view_t collections,
                                std::size_t count, ustore_modality_t modality,
                                std::vector<collection_t const*>& resolved) {
    resolved.assign(count, nullptr);
    for (std::size_t index = 0; index != count; ++index) {
        ustore_collection_id_t const& id = strided_element(collections.data, collections.stride, index);
        collection_t const* const collection = reader.find(bytes_of(id));
        if (!collection) return call.fail(ustore_status_not_found_k, "No such collection");
        if (collection->modality != modality)
            return call.fail(ustore_status_invalid_argument_k, "The collection holds another modality");
        resolved[index] = collection;
    }
    return true;
}

#pragma endregion Reading

#pragma region Writing

/** The newest stamp @p key of @p collection changed at, or zero. */
inline stamp_t latest_stamp(collection_t const& collection, std::string const& key) noexcept {
    auto const found = collection.entries.find(key);
    if (found == collection.entries.end() || found->second.empty()) return 0;
    return found->second.back().stamp;
}

/** Whether @p key of @p collection is present at the head. */
inline bool present_at_head(collection_t const& collection, std::string const& key) noexcept {
    auto const found = collection.entries.find(key);
    return found != collection.entries.end() && !found->second.empty() && found->second.back().present;
}

/** Applies @p batch to @p database at the next stamp under its exclusive lock, answering the
 *  stamp, logging the changes for the open feeds and dropping the versions no reader reaches. A
 *  collection dropped meanwhile is refused before anything lands. */
inline status_t apply_batch(ustore_database_handle_t& database, batch_t const& batch, stamp_t& commit_stamp) {
    for (auto const& [id, overlay] : batch)
        if (!overlay.empty() && !database.collections.count(id))
            return {ustore_status_not_found_k, "A collection was dropped meanwhile"};
    stamp_t const stamp = database.head + 1;
    bool const logs = !database.feed_pins.empty();
    stamp_t const oldest = smaller_of(oldest_pin(database), stamp);
    for (auto const& [id, overlay] : batch) {
        auto found = database.collections.find(id);
        if (found == database.collections.end()) continue;
        collection_t& collection = found->second;
        for (auto const& [key, pending] : overlay) {
            // A key already gone stays gone, so removing it again publishes nothing
            if (!pending.present && !present_at_head(collection, key)) continue;
            std::vector<version_t>& history = collection.entries[key];
            history.push_back({stamp, pending.present, pending.value});
            prune(history, oldest);
            if (history.size() == 1 && !history.front().present && history.front().stamp <= oldest)
                collection.entries.erase(key);
            if (logs)
                collection.changes.push_back(
                    {stamp, key, pending.present ? pending.kind : ustore_change_removal_k, pending.value});
        }
    }
    database.head = stamp;
    commit_stamp = stamp;
    return {};
}

/** Whether a commit after @p read_stamp changed a key inside @p window. */
inline bool window_changed(ustore_database_handle_t const& database, window_t const& window,
                           stamp_t read_stamp) noexcept {
    auto const found = database.collections.find(window.collection);
    if (found == database.collections.end()) return false;
    for (auto it = found->second.entries.lower_bound(window.low); it != found->second.entries.end(); ++it) {
        if (window.bounded && it->first >= window.high) return false;
        if (!it->second.empty() && it->second.back().stamp > read_stamp) return true;
    }
    return false;
}

/** Validates the writes, reads and windows of @p transaction against every commit after
 *  its read stamp. */
inline status_t validate_transaction(ustore_database_handle_t const& database,
                                     ustore_transaction_handle_t const& transaction) {
    stamp_t const read_stamp = transaction.read_stamp;
    for (auto const& [id, overlay] : transaction.writes) {
        auto const found = database.collections.find(id);
        if (found == database.collections.end()) {
            if (overlay.empty()) continue;
            return {ustore_status_not_found_k, "A collection the transaction touched was dropped"};
        }
        for (auto const& [key, pending] : overlay)
            if (pending.watched && latest_stamp(found->second, key) > read_stamp)
                return {ustore_status_write_conflict_k, "Another commit changed a key this transaction wrote"};
    }
    for (auto const& [id, key] : transaction.reads) {
        auto const found = database.collections.find(id);
        if (found != database.collections.end() && latest_stamp(found->second, key) > read_stamp)
            return {ustore_status_read_conflict_k, "Another commit changed a key this transaction read"};
    }
    if (database.isolation != ustore_isolation_strict_serializable_k) return {};
    for (window_t const& window : transaction.windows)
        if (window_changed(database, window, read_stamp))
            return {ustore_status_phantom_conflict_k, "Another commit changed a window this transaction walked"};
    return {};
}

/** Where a write call lands its changes: the transaction's buffer, or a batch the call commits
 *  at the head. */
struct writer_t {
    ustore_transaction_handle_t* transaction = nullptr;
    bool watched = true;
    batch_t batch;

    /** The buffer the changes of @p collection land in. */
    overlay_t& overlay(collection_t const& collection) {
        std::string const id = bytes_of(collection.id);
        return transaction ? transaction->writes[id] : batch[id];
    }

    void put(collection_t const& collection, std::string const& key, std::string value,
             ustore_change_kind_t kind = ustore_change_write_k) {
        overlay_t& pending = overlay(collection);
        pending[key] = {true, std::move(value), kind, watched};
    }
    void remove(collection_t const& collection, std::string const& key) {
        overlay_t& pending = overlay(collection);
        pending[key] = {false, {}, ustore_change_removal_k, watched};
    }
};

/** Whether a write may leave its keys unwatched outside a transaction, as only graphs
 *  on RAM may. */
enum class unwatched_writes_t : bool { refused_k, admitted_k };

/** Refuses the choices of a write or removal: an unknown durability or watch, a flushed write
 *  inside a transaction, and, when @p unwatched, a write left unwatched outside one. */
inline bool refuse_write_choices(call_t& call, ustore_transaction_t transaction, ustore_durability_t durability,
                                 ustore_watch_t watch,
                                 unwatched_writes_t unwatched = unwatched_writes_t::refused_k) noexcept {
    if (durability != ustore_durability_buffered_k && durability != ustore_durability_flushed_k)
        return call.stop(ustore_status_invalid_argument_k, "Unknown durability");
    if (watch != ustore_watch_keys_k && watch != ustore_watch_none_k)
        return call.stop(ustore_status_invalid_argument_k, "Unknown watch");
    if (transaction && durability == ustore_durability_flushed_k)
        return call.stop(ustore_status_invalid_argument_k, "A transaction flushes at commit, not at a write");
    if (unwatched == unwatched_writes_t::refused_k && !transaction && watch == ustore_watch_none_k)
        return call.stop(ustore_status_invalid_argument_k, "Keys stay unwatched only inside a transaction");
    return false;
}

/** Refuses a transaction of another database, a foreign handle, or a staged one, which
 *  takes no writes. */
inline bool refuse_write_transaction(call_t& call, ustore_transaction_t transaction,
                                     ustore_database_handle_t* database) noexcept {
    if (!transaction) return false;
    if (transaction->magic != magic_t::transaction_k || transaction->database != database)
        return call.stop(ustore_status_invalid_argument_k, "The transaction belongs to another database");
    if (transaction->staged) return call.stop(ustore_status_invalid_argument_k, "The transaction is staged");
    return false;
}

status_t write_image(ustore_database_handle_t& database, stamp_t stamp, std::string const& directory,
                     ustore_progress_callback_t progress = nullptr, void* progress_state = nullptr);

/**
 *  @brief Runs @p stage under the right lock and lands what it wrote: into the transaction's
 *      buffer, or as one commit at the head, persisted when @p durability asks.
 *
 *  @p stage is called as <tt>stage(reader, writer)</tt> and records its own refusals into @p call;
 *  nothing lands once it did.
 */
template <typename stage_type_>
void write_through(call_t& call, ustore_database_handle_t* database, ustore_transaction_t transaction,
                   ustore_watch_t watch, ustore_durability_t durability, stage_type_&& stage) {
    try {
        writer_t writer;
        writer.transaction = transaction;
        writer.watched = watch == ustore_watch_keys_k;
        if (transaction) {
            std::shared_lock lock(database->mutex);
            reader_t reader;
            reader.database = database;
            reader.stamp = transaction->read_stamp;
            reader.transaction = transaction;
            reader.records = writer.watched || database->isolation == ustore_isolation_strict_serializable_k;
            // A failed stage leaves the buffer as it was, so it stages into a copy first
            batch_t const before = transaction->writes;
            stage(reader, writer);
            if (call.failed()) transaction->writes = before;
            return;
        }
        std::unique_lock lock(database->mutex);
        reader_t reader;
        reader.database = database;
        reader.stamp = database->head;
        stage(reader, writer);
        if (call.failed()) return;
        stamp_t stamp = 0;
        if (call.failed_with(apply_batch(*database, writer.batch, stamp))) return;
        if (durability == ustore_durability_flushed_k && !database->directory.empty())
            call.failed_with(write_image(*database, database->head, database->directory));
    }
    catch (std::bad_alloc const&) {
        call.refuse(ustore_status_out_of_memory_k, "Out of memory while writing");
    }
}

#pragma endregion Writing

#pragma region Matching

/** The patterns a scan keeps the values or strings matching, compiled once per call or cursor. */
struct matcher_t {
    bool everything = true;
    ustore_pattern_kind_t kind = ustore_pattern_regex_k;
    bool uncased = false;
    std::vector<std::string> needles;
    std::vector<std::regex> regexes;
    std::vector<float> weights;

    /** Whether @p text matches at least one pattern, adding the BM25 weight of every substring
     *  it holds to @p score. */
    bool matches(std::string_view text, float* score = nullptr) const {
        if (everything) return true;
        std::string folded;
        if (uncased) {
            folded.assign(text);
            for (char& character : folded)
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            text = folded;
        }
        if (kind == ustore_pattern_regex_k) {
            for (std::regex const& regex : regexes)
                if (std::regex_search(text.begin(), text.end(), regex)) return true;
            return false;
        }
        bool matched = false;
        float total = 0;
        for (std::size_t index = 0; index != needles.size(); ++index) {
            std::string const& needle = needles[index];
            std::size_t occurrences = 0;
            for (std::size_t at = text.find(needle); at != std::string_view::npos;
                 at = text.find(needle, at + larger_of<std::size_t>(needle.size(), 1)))
                ++occurrences;
            if (!occurrences) continue;
            matched = true;
            // BM25 with its length normalization left out, as the stand-in keeps
            // no corpus statistics
            constexpr float k1 = 1.2f;
            float const frequency = static_cast<float>(occurrences);
            if (!weights.empty()) total += weights[index] * frequency * (k1 + 1) / (frequency + k1);
        }
        if (score) *score = total;
        return matched;
    }
    bool scored() const noexcept { return !everything && !weights.empty(); }
};

/** Compiles @p patterns into @p matcher, refusing an unknown kind or case sensitivity, weights
 *  beside regexes, and a pattern that does not compile. */
inline bool compile_patterns(call_t& call, ustore_patterns_t const& patterns, matcher_t& matcher) {
    matcher = {};
    auto const kind = std::bit_cast<std::underlying_type_t<ustore_pattern_kind_t>>(patterns.kind);
    auto const sensitivity = std::bit_cast<std::underlying_type_t<ustore_case_sensitivity_t>>(patterns.sensitivity);
    if (kind != ustore_pattern_regex_k && kind != ustore_pattern_substrings_k)
        return call.fail(ustore_status_invalid_argument_k, "Unknown pattern kind");
    if (sensitivity != ustore_cased_k && sensitivity != ustore_uncased_k)
        return call.fail(ustore_status_invalid_argument_k, "Unknown case sensitivity");
    strings_in_t texts;
    if (!read_strings(call, patterns.patterns, patterns.count, texts)) return false;
    if (!texts.given || !patterns.count) return true;
    if (patterns.weights && kind == ustore_pattern_regex_k)
        return call.fail(ustore_status_invalid_argument_k, "Weights score substrings, never regexes");
    matcher.everything = false;
    matcher.kind = patterns.kind;
    matcher.uncased = sensitivity == ustore_uncased_k;
    for (std::size_t index = 0; index != texts.size(); ++index) {
        std::string text(texts[index]);
        if (matcher.uncased && kind == ustore_pattern_substrings_k)
            for (char& character : text)
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if (kind == ustore_pattern_substrings_k) {
            matcher.needles.push_back(std::move(text));
            if (patterns.weights) matcher.weights.push_back(patterns.weights[index]);
            continue;
        }
        try {
            auto flags = std::regex::ECMAScript;
            if (matcher.uncased) flags |= std::regex::icase;
            matcher.regexes.emplace_back(text, flags);
        }
        catch (std::regex_error const&) {
            return call.fail(ustore_status_invalid_argument_k, "A pattern does not compile");
        }
    }
    return true;
}

/** Refuses a predicate cost no constant names. */
inline bool refuse_predicate(call_t& call, ustore_predicate_t const& predicate) noexcept {
    auto const cost = std::bit_cast<std::underlying_type_t<ustore_predicate_cost_t>>(predicate.cost);
    if (cost != ustore_predicate_cost_cheap_k && cost != ustore_predicate_cost_expensive_k)
        return call.stop(ustore_status_invalid_argument_k, "Unknown predicate cost");
    return false;
}

/** Refuses a direction no constant names. */
inline bool refuse_direction(call_t& call, ustore_scan_direction_t direction) noexcept {
    auto const bits = std::bit_cast<std::underlying_type_t<ustore_scan_direction_t>>(direction);
    if (bits != ustore_scan_ascending_k && bits != ustore_scan_descending_k)
        return call.stop(ustore_status_invalid_argument_k, "Unknown scan direction");
    return false;
}

/** Refuses a scan mode no constant names. */
inline bool refuse_mode(call_t& call, ustore_scan_mode_t mode) noexcept {
    auto const bits = std::bit_cast<std::underlying_type_t<ustore_scan_mode_t>>(mode);
    if (bits != ustore_scan_ordered_k && bits != ustore_scan_unordered_k)
        return call.stop(ustore_status_invalid_argument_k, "Unknown scan mode");
    return false;
}

/** Asks @p predicate about @p keys, keeping in @p admitted only the entries it admits. */
inline void ask_predicate(ustore_predicate_t const& predicate, std::vector<ustore_key_t> const& keys,
                          std::vector<bool>& admitted) {
    if (!predicate.callback || keys.empty()) return;
    // A handler writes only the admissions it grants, so every key starts rejected
    std::vector<ustore_admission_t> admissions(keys.size(), ustore_reject_k);
    predicate.callback(keys.data(), admissions.data(), keys.size(), predicate.state);
    for (std::size_t index = 0; index != keys.size(); ++index)
        if (admissions[index] != ustore_admit_k) admitted[index] = false;
}

#pragma endregion Matching

#pragma region Walks

/**
 *  @brief One walk over a collection's entries: where it resumes, where it ends, and which
 *      entries it lists.
 *
 *  The position is an entry key, which a cursor keeps between pulls and a scan between batches. The
 *  @c accept filter tells a vertex of a graph from its half-edges, and the @c stop test ends a
 *  strings walk past its prefix.
 */
struct walk_t {
    std::string position;
    bound_t kind = bound_t::none_k;
    std::string end;
    bool bounded = false;
    ustore_scan_direction_t direction = ustore_scan_ascending_k;

    /** Whether the entry at @p key is one the walk lists. */
    bool (*accept)(std::string const& key, void const* context) = nullptr;

    /** Whether the walk is past its window at @p key. */
    bool (*stop)(std::string const& key, void const* context) = nullptr;
    void const* context = nullptr;

    bool past_end(std::string const& key) const noexcept {
        if (stop && stop(key, context)) return true;
        if (!bounded) return false;
        return direction == ustore_scan_descending_k ? key <= end : key >= end;
    }
};

/** Lists up to @p limit entries of @p walk into @p listed, advancing its position past the last
 *  one, under the lock the caller holds; answers false once the walk reached its end. */
inline bool fetch(reader_t const& reader, collection_t const& collection, walk_t& walk, std::size_t limit,
                  std::vector<entry_t>& listed) {
    listed.clear();
    while (listed.size() < limit) {
        found_entry_t found = reader.seek(collection, walk.position, walk.kind, walk.direction);
        if (!found.value || walk.past_end(found.key)) return false;
        walk.position = found.key, walk.kind = bound_t::exclusive_k;
        if (walk.accept && !walk.accept(found.key, walk.context)) continue;
        reader.record(collection, found.key);
        listed.push_back({std::move(found.key), *found.value});
    }
    return true;
}

/** Records the window a walk starting at @p walk covers, for a strictly serializable commit. */
inline void record_walk(reader_t const& reader, collection_t const& collection, walk_t const& walk) {
    if (!reader.records) return;
    bool const descending = walk.direction == ustore_scan_descending_k;
    std::string const low = descending ? (walk.bounded ? walk.end : std::string {})
                                       : (walk.kind == bound_t::none_k ? std::string {} : walk.position);
    bool bounded = descending ? walk.kind != bound_t::none_k : walk.bounded;
    std::string high = descending ? walk.position + std::string(1, '\0') : walk.end;
    reader.record_window({bytes_of(collection.id), low, high, bounded});
}

/** The filters a walk applies to every batch, unlocked, in the order the headers give: the cheap
 *  predicate, the patterns over what @c text_of answers, the expensive predicate. */
struct sieve_t {
    ustore_predicate_t predicate {};
    matcher_t const* matcher = nullptr;

    /** The key a predicate sees for an entry, as the modality defines it. */
    ustore_key_t (*key_for)(entry_t const& entry) = nullptr;

    /** The text patterns match for an entry: its value, or a string's own text. */
    std::string_view (*text_for)(entry_t const& entry) = nullptr;

    /** An extra verdict on an entry, like a documents filter, which answers false to drop it. */
    bool (*keeps)(entry_t const& entry, void const* context) = nullptr;
    void const* keeps_context = nullptr;
};

/** Narrows @p batch through @p sieve, keeping the survivors with their scores. */
inline void sift(sieve_t const& sieve, std::vector<entry_t>& batch, std::vector<float>& scores) {
    scores.assign(batch.size(), 0.0f);
    std::vector<bool> admitted(batch.size(), true);
    auto ask = [&](ustore_predicate_cost_t cost) {
        if (!sieve.predicate.callback || sieve.predicate.cost != cost) return;
        std::vector<ustore_key_t> keys;
        std::vector<std::size_t> positions;
        for (std::size_t index = 0; index != batch.size(); ++index)
            if (admitted[index]) keys.push_back(sieve.key_for(batch[index])), positions.push_back(index);
        std::vector<bool> verdicts(keys.size(), true);
        ask_predicate(sieve.predicate, keys, verdicts);
        for (std::size_t index = 0; index != positions.size(); ++index)
            if (!verdicts[index]) admitted[positions[index]] = false;
    };
    ask(ustore_predicate_cost_cheap_k);
    for (std::size_t index = 0; index != batch.size(); ++index) {
        if (!admitted[index]) continue;
        if (sieve.matcher && !sieve.matcher->matches(sieve.text_for(batch[index]), &scores[index]))
            admitted[index] = false;
        else if (sieve.keeps && !sieve.keeps(batch[index], sieve.keeps_context)) admitted[index] = false;
    }
    ask(ustore_predicate_cost_expensive_k);
    std::size_t kept = 0;
    for (std::size_t index = 0; index != batch.size(); ++index)
        if (admitted[index]) {
            if (kept != index) batch[kept] = std::move(batch[index]), scores[kept] = scores[index];
            ++kept;
        }
    batch.resize(kept), scores.resize(kept);
}

/** Lists up to @p wanted entries of @p walk through @p sieve, locking the database only to fetch
 *  each batch, appending them to @p listed and their scores to @p scores; answers false once the
 *  collection is gone. */
inline bool list_entries(reader_t const& reader, std::string const& collection_id, walk_t& walk, sieve_t const& sieve,
                         std::size_t wanted, std::vector<entry_t>& listed, std::vector<float>& scores) {
    std::vector<entry_t> batch;
    std::vector<float> batch_scores;
    std::size_t const page = wanted - smaller_of(wanted, listed.size());
    bool more = true;
    bool recorded = false;
    while (more && listed.size() < wanted) {
        {
            std::shared_lock lock(reader.database->mutex);
            collection_t const* const collection = reader.find(collection_id);
            if (!collection) return false;
            if (!recorded) record_walk(reader, *collection, walk), recorded = true;
            // Every page lists as many entries as the call wants, as the engine's walks do, before
            // the sieve narrows it
            std::size_t const limit = sieve.predicate.callback || sieve.matcher || sieve.keeps ? page
                                                                                               : wanted - listed.size();
            more = fetch(reader, *collection, walk, limit, batch);
        }
        sift(sieve, batch, batch_scores);
        for (std::size_t index = 0; index != batch.size() && listed.size() < wanted; ++index) {
            // A page that fills before its batch ends resumes just past its last entry
            if (listed.size() + 1 == wanted && index + 1 != batch.size())
                walk.position = batch[index].key, walk.kind = bound_t::exclusive_k, more = true;
            listed.push_back(std::move(batch[index]));
            scores.push_back(batch_scores[index]);
        }
    }
    return true;
}

#pragma endregion Walks

#pragma region Drawing

/** Draws up to @p wanted of @p population uniformly at random, without repetition. */
template <typename value_type_>
std::vector<value_type_> draw(std::vector<value_type_> population, std::size_t wanted) {
    static std::atomic<std::uint64_t> sequence {0};
    std::mt19937_64 generator(std::random_device {}() ^ sequence.fetch_add(0x9E3779B97F4A7C15ull));
    std::shuffle(population.begin(), population.end(), generator);
    if (population.size() > wanted) population.resize(wanted);
    return population;
}

/** Mints a UUIDv7: Unix milliseconds big-endian, the version and variant bits, then random bits,
 *  so later identifiers order after earlier ones. */
inline std::array<std::uint8_t, 16> mint_uuid_v7() {
    static std::atomic<std::uint64_t> sequence {0};
    std::array<std::uint8_t, 16> bytes {};
    std::random_device entropy;
    for (std::size_t index = 0; index != bytes.size(); index += 4) {
        std::uint32_t const word = entropy();
        std::memcpy(bytes.data() + index, &word, 4);
    }
    auto const milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    for (unsigned index = 0; index != 6; ++index)
        bytes[index] = static_cast<std::uint8_t>(milliseconds >> (8 * (5 - index)));
    // A process-wide sequence in the random bits keeps identifiers minted in one millisecond
    // apart and ordered
    std::uint64_t const order = sequence.fetch_add(1);
    bytes[6] = static_cast<std::uint8_t>(0x70 | ((order >> 8) & 0x0F));
    bytes[7] = static_cast<std::uint8_t>(order);
    bytes[8] = static_cast<std::uint8_t>(0x80 | (bytes[8] & 0x3F));
    return bytes;
}

#pragma endregion Drawing

#pragma region Shared Checks

/** Refuses a null database as uninitialized, the first refusal of every call naming one. */
inline bool refuse_database(call_t& call, ustore_database_t database) noexcept {
    if (!database) return call.stop(ustore_status_uninitialized_k, "The database is not open");
    call.name(&database->named_calls);
    return false;
}

/** Resolves the collections a write names before it claims anything, as its transaction or the
 *  head sees them, so a collection of another modality is refused among the invalid arguments. */
inline bool pre_resolve(call_t& call, ustore_database_t database, ustore_transaction_t transaction,
                        ustore_collections_view_t collections, std::size_t count, ustore_modality_t modality) {
    std::shared_lock lock(database->mutex);
    reader_t reader;
    reader.database = database;
    reader.stamp = transaction ? transaction->read_stamp : database->head;
    reader.transaction = transaction;
    std::vector<collection_t const*> resolved;
    return resolve_collections(call, reader, collections, count, modality, resolved);
}

/** Names and claims the transaction a write joins, which serves one call at a time. */
inline bool hold_transaction(call_t& call, ustore_transaction_t transaction) noexcept {
    if (!transaction) return true;
    call.name(&transaction->named_calls);
    return call.claim(&transaction->claim, "Another call holds the transaction");
}

/** Refuses a change to a database opened read-only. */
inline bool refuse_read_only(call_t& call, ustore_database_t database) noexcept {
    if (database && database->read_only) return call.stop(ustore_status_read_only_k, "The database is read-only");
    return false;
}

/** Refuses keys missing for @p count tasks, or given for none, as every write and removal
 *  of keys does. */
inline bool refuse_write_keys(call_t& call, ustore_keys_view_t keys, std::size_t count) noexcept {
    if (count && !keys.data) return call.stop(ustore_status_invalid_argument_k, "No keys were provided");
    if (!count && keys.data) return call.stop(ustore_status_invalid_argument_k, "Keys were given for no tasks");
    return false;
}

/** Refuses a name or a path holding a length without bytes, which is never read through null. */
inline bool refuse_unbacked(call_t& call, ustore_bytes_view_t bytes) noexcept {
    if (bytes.length && !bytes.data)
        return call.stop(ustore_status_invalid_argument_k, "A run of bytes with a length needs its bytes");
    return false;
}

/** Refuses a missing collections run for @p count tasks. */
inline bool refuse_collections(call_t& call, ustore_collections_view_t collections, std::size_t count) noexcept {
    if (count && !collections.data) return call.stop(ustore_status_invalid_argument_k, "No collections were named");
    return false;
}

/** Gives @p progress one report, answering whether the call continues. */
inline bool keeps_going(ustore_progress_callback_t progress, void* state, std::size_t finished,
                        std::size_t total) noexcept {
    return !progress || progress(finished, total, state) == ustore_continue_k;
}

/** The key of a walk's bound: @p data's entry @p index, or none. */
inline bool bound_key(ustore_keys_view_t keys, std::size_t index, std::string& bound) {
    if (!keys.data) return false;
    bound = bytes_of(strided_element(keys.data, keys.stride, index));
    return true;
}

#pragma endregion Shared Checks

#pragma region Cross Modality Hooks

/** Checks one JSON document an ingest lands, as @c ustore_docs_write does: strict RFC 8259. */
status_t check_document(std::string_view text);

/** Parses @p markup into the stored form of the page @p page_key names, as
 *  @c ustore_pages_import does. */
status_t import_page(std::string_view markup, ustore_key_t const& page_key, std::string& stored);

/** Bytes a graph entry takes: a vertex, a direction byte, a neighbor and an edge. */
constexpr std::size_t half_edge_bytes_k = 49;

/** The entry recording that @p vertex exists. */
inline std::string vertex_entry(ustore_key_t const& vertex) {
    std::string entry(half_edge_bytes_k, '\0');
    std::memcpy(entry.data(), vertex.uuid.bytes, 16);
    return entry;
}

/** The entry of an edge seen from @p vertex in @p direction, 1 outgoing and 2 incoming. */
inline std::string half_edge(ustore_key_t const& vertex, std::uint8_t direction, ustore_key_t const& neighbor,
                             ustore_key_t const& edge) {
    std::string entry(half_edge_bytes_k, '\0');
    std::memcpy(entry.data(), vertex.uuid.bytes, 16);
    entry[16] = static_cast<char>(direction);
    std::memcpy(entry.data() + 17, neighbor.uuid.bytes, 16);
    std::memcpy(entry.data() + 33, edge.uuid.bytes, 16);
    return entry;
}

/** Stages @p count edges into @p writer over @p collection: both vertices, then both half-edges. */
inline void stage_edges(reader_t const& reader, writer_t& writer, collection_t const& collection,
                        ustore_keys_view_t sources, ustore_keys_view_t targets, ustore_keys_view_t edges,
                        std::size_t count) {
    for (std::size_t index = 0; index != count; ++index) {
        ustore_key_t const& source = strided_element(sources.data, sources.stride, index);
        ustore_key_t const& target = strided_element(targets.data, targets.stride, index);
        ustore_key_t const edge = edges.data ? strided_element(edges.data, edges.stride, index)
                                             : ustore_edge_id_default_k;
        for (ustore_key_t const* vertex : {&source, &target}) {
            std::string const entry = vertex_entry(*vertex);
            if (!reader.peek(collection, entry)) writer.put(collection, entry, {});
        }
        writer.put(collection, half_edge(source, 1, target, edge), {});
        writer.put(collection, half_edge(target, 2, source, edge), {});
    }
}

#pragma endregion Cross Modality Hooks

#pragma region Arguments

/** Whether a scan can answer one BM25 score per listed key. */
template <typename arguments_type_>
concept a_call_scoring_matches = requires(arguments_type_ const& arguments) { arguments.scores; };

/** Whether a pull can answer the values of its keys beside them. */
template <typename arguments_type_>
concept a_call_exporting_values = requires(arguments_type_ const& arguments) { arguments.values; };

#pragma endregion Arguments

} // namespace unum::ustore::stub

#endif // UNUM_USTORE_STUB_LIB_HPP
