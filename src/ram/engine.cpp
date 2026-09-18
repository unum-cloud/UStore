/**
 *  @file src/ram/engine.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Embedded in-memory engine on SmashTable, strictly serializable or snapshot-isolated as configured at open.
 *
 *  Collections, their catalog and the counter numbering them share one partitioned multi-version store ordered by
 *  collection and key. Every exported call visits the isolation once, resolves the one view it reads - the head, a
 *  snapshot or a transaction - and runs a template specialised for both. Snapshots are pinned readers in a lock-free
 *  slot directory and the catalog changes through store transactions, so the engine adds no lock of its own.
 *  A database with a directory persists as one binary image, rewritten whole on a flush and on close.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstdio>  // `std::FILE`, `std::fopen`, `std::fread`, `std::fwrite`, `std::rename`
#include <cstring> // `std::memcmp`, `std::memcpy`, `std::strlen`

#if defined(_WIN32)
#include <io.h>      // `_commit`, `_fileno`
#include <windows.h> // `MoveFileExA`
#else
#include <unistd.h> // `fsync`
#endif

#include <algorithm>   // `std::max`, `std::min`
#include <array>       // `std::array`
#include <bit>         // `std::bit_width`
#include <charconv>    // `std::to_chars`
#include <chrono>      // `std::chrono::steady_clock`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::destroy_n`, `std::unique_ptr`
#include <new>         // `std::nothrow`, placement `new`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::move`
#include <variant>     // `std::in_place_type`, `std::variant`, `std::visit`

#include <forkunion/atomics.hpp>
#include <smashtable/basic_avl_tree.hpp>
#include <smashtable/basic_vector.hpp>
#include <smashtable/partitioned_store.hpp>
#include <smashtable/snapshot_store.hpp>
#include <stringzilla/stringzilla.hpp>

#include <ustore/blobs.h>
#include <ustore/db.h>
#include <ustore/ranges_args.hpp> // `places_arg_t`, `contents_arg_t`, `scans_arg_t`, `validate_read`

#include "helpers/blocking_files.hpp" // `directory_exists`
#include "helpers/config_loader.hpp"  // `load_config`, `read_json_member`
#include "helpers/linked_array.hpp"   // `uninitialized_array`
#include "helpers/linked_memory.hpp"  // `linked_memory_lock_t`
#include "ram/key.hpp"                // `stored_key_t`

namespace unum::ustore::ram {

#pragma region Store

/** The status every SmashTable call answers with. */
using store_status_t = st::status_t;

/** The isolation level a store instantiation promises. */
using isolation_t = st::isolation_t;

/** Whether a walk carries on after an element. */
using walk_control_t = st::walk_control_t;

/** Whether a transaction's writes sit in the store, invisible. */
using staging_t = st::staging_t;

/** The bytes of one value, owned by the store. */
using blob_t = st::basic_vector<ustore_byte_t>;

/** A value filed under its key. */
using pair_t = st::mapping<stored_key_t, blob_t>;

/** The ordered core every partition keeps its versions in. */
using tree_t = st::basic_avl_tree<pair_t, stored_key_order_t, st::default_allocator<pair_t>>;

/** Every pair of a database at @p isolation_, in partitions that commit under one shared clock. */
template <isolation_t isolation_>
using ordered_store = st::partitioned_store<st::snapshot_store<tree_t, isolation_>, stored_key_hash_t>;

/** A view of every partition pinned at one stamp, recording nothing. */
template <isolation_t isolation_>
using pinned_reader = typename ordered_store<isolation_>::reader_t;

/** A transaction over every partition. */
template <isolation_t isolation_>
using store_transaction = typename ordered_store<isolation_>::transaction_t;

static_assert(ordered_store<isolation_t::strict_serializable_k>::isolation_k == isolation_t::strict_serializable_k,
              "the partitions must keep the strict serializability of their stores");
static_assert(ordered_store<isolation_t::snapshot_k>::isolation_k == isolation_t::snapshot_k,
              "the partitions must keep the snapshot isolation of their stores");

/** How many times an engine-internal commit is attempted before its conflicts reach the caller. */
inline constexpr std::size_t commit_attempts_k = 16;

/** The error of the C ABI a caller reads for @p status. */
constexpr ustore_error_t translate(store_status_t status) noexcept {
    switch (status) {
    case store_status_t::success_k: return {};
    case store_status_t::unknown_k: return {ustore_error_internal_k, "SmashTable failed for an unknown reason"};
    case store_status_t::consistency_k: return {ustore_error_internal_k, "SmashTable found a staged version missing"};
    case store_status_t::write_conflict_k:
        return {ustore_error_write_conflict_k, "Write conflict: a written key was committed over"};
    case store_status_t::read_conflict_k:
        return {ustore_error_read_conflict_k, "Read conflict: a read key was committed over"};
    case store_status_t::phantom_conflict_k:
        return {ustore_error_phantom_conflict_k, "Phantom conflict: a walked range gained or lost a key"};
    case store_status_t::out_of_memory_heap_k: return {ustore_error_out_of_memory_k, "Out of memory"};
    case store_status_t::invalid_argument_k: return {ustore_error_invalid_argument_k, "Invalid argument"};
    case store_status_t::operation_not_permitted_k:
        return {ustore_error_invalid_argument_k, "Operation not permitted in the transaction's state"};
    case store_status_t::operation_would_block_k:
        return {ustore_error_write_conflict_k, "Gave up after every retry met a conflict"};
    case store_status_t::capacity_exhausted_k: return {ustore_error_capacity_exhausted_k, "Capacity exhausted"};
    case store_status_t::input_output_k: return {ustore_error_io_k, "Input or output failed"};
    case store_status_t::key_already_exists_k: return {ustore_error_already_exists_k, "Key already exists"};
    case store_status_t::key_not_found_k: return {ustore_error_not_found_k, "Key not found"};
    }
    return {ustore_error_internal_k, "SmashTable answered an unrecognized status"};
}

/** Reports a failed @p status through @p error, answering whether it failed. */
inline bool reported_failure(store_status_t status, ustore_error_t* error) noexcept {
    if (st::succeeded(status)) return false;
    *error = translate(status);
    return true;
}

/** Whether @p path names an existing directory. */
inline bool names_directory(char const* path) noexcept {
    expected<bool> const exists = directory_exists(path);
    return exists && *exists;
}

/** Copies @p value into a blob the store owns. */
inline st::expected<blob_t> copy_blob(value_view_t value) noexcept {
    blob_t blob;
    if (st::failed(blob.resize(value.size()))) return store_status_t::out_of_memory_heap_k;
    if (value.size() != 0) std::memcpy(blob.data(), value.data(), value.size());
    return blob;
}

/** Views the bytes of a stored blob. */
inline value_view_t view_of(blob_t const& blob) noexcept {
    return value_view_t {blob.data(), static_cast<ustore_length_t>(blob.size())};
}

#pragma endregion Store

#pragma region Atomic Words

/** Loads @p word, observing every store another thread released before. */
template <typename word_type_>
word_type_ acquire_word(word_type_ const& word) noexcept {
    return fu::standard_atomic_ref<word_type_ const>(word).load(st::memory_order_acquire_k);
}

/** Stores @p desired into @p word, so every later acquiring load observes it. */
template <typename word_type_>
void release_word(word_type_& word, word_type_ desired) noexcept {
    fu::standard_atomic_ref<word_type_>(word).store(desired, st::memory_order_release_k);
}

/** Replaces @p expected with @p desired in @p word, answering whether @p word held @p expected. */
template <typename word_type_>
bool exchange_word(word_type_& word, word_type_ expected, word_type_ desired) noexcept {
    return fu::standard_atomic_ref<word_type_>(word).compare_exchange_strong(
        expected, desired, st::memory_order_release_k, st::memory_order_acquire_k);
}

#pragma endregion Atomic Words

#pragma region Snapshot Directory

/** Low bits of a snapshot identifier that address its slot, while the high bits count creations. */
inline constexpr unsigned slot_index_bits_k = 26;

/** The mask selecting the slot index of a snapshot identifier. */
inline constexpr std::uint64_t slot_index_mask_k = (std::uint64_t {1} << slot_index_bits_k) - 1;

/** Slots in the first chunk; every later chunk doubles, so a slot never moves once placed. */
inline constexpr std::uint64_t first_chunk_slots_k = 64;

/** Chunks the directory can hold, together addressing every slot index below @c slot_index_mask_k. */
inline constexpr std::size_t chunks_count_k = 20;

/** Slots every chunk together holds. */
inline constexpr std::uint64_t slots_limit_k = first_chunk_slots_k * ((std::uint64_t {1} << chunks_count_k) - 1);

/** The identifier word of a slot no snapshot holds. */
inline constexpr std::uint64_t free_slot_k = 0;

/** The identifier word of a slot being claimed or released. */
inline constexpr std::uint64_t changing_slot_k = 1;

/** The chunk holding slot @p index and its offset inside that chunk. */
struct slot_position_t {

    /** The chunk, whose capacity is @c first_chunk_slots_k shifted by it. */
    std::size_t chunk;

    /** The offset of the slot inside its chunk. */
    std::size_t offset;
};

/** Where slot @p index lives. */
constexpr slot_position_t position_of(std::uint64_t index) noexcept {
    std::size_t const chunk = static_cast<std::size_t>(std::bit_width(index / first_chunk_slots_k + 1)) - 1;
    return {chunk, static_cast<std::size_t>(index - first_chunk_slots_k * ((std::uint64_t {1} << chunk) - 1))};
}

/** One snapshot of the C ABI, alive while its identifier word names it. */
template <isolation_t isolation_>
struct snapshot_slot {

    /** The identifier of the open snapshot, @c free_slot_k, or @c changing_slot_k. */
    std::uint64_t identifier {free_slot_k};

    /** Storage for the reader, constructed exactly while @c identifier names an open snapshot. */
    union {
        pinned_reader<isolation_> reader;
    };

    snapshot_slot() noexcept {}
    ~snapshot_slot() noexcept {}
};

/**
 *  @brief Every open snapshot, found by identifier without a lock.
 *
 *  Chunks are published once and never move, and a slot's identifier word is claimed and released by
 *  compare-exchange, so a read validates an identifier with one acquiring load. Dropping a snapshot while
 *  another thread reads through it is a caller error, as for every other handle of the C ABI.
 */
template <isolation_t isolation_>
class snapshot_directory {
    using slot_t = snapshot_slot<isolation_>;
    using reader_t = pinned_reader<isolation_>;
    using allocator_t = st::default_allocator<slot_t>;

    /** The published chunks, null until a slot inside one is first claimed. */
    std::array<slot_t*, chunks_count_k> chunks_ {};

    /** Slot indices ever handed out, which bounds every scan. */
    std::uint64_t slots_count_ {0};

    /** Snapshots ever created, so an identifier never names a later snapshot in the same slot. */
    std::uint64_t creations_ {0};

  public:
    snapshot_directory() noexcept = default;
    snapshot_directory(snapshot_directory const&) = delete;
    snapshot_directory& operator=(snapshot_directory const&) = delete;

    ~snapshot_directory() noexcept {
        close_every();
        for (std::size_t chunk = 0; chunk != chunks_count_k; ++chunk) release_chunk_(chunks_[chunk], chunk);
    }

    /** Pins the newest stamp of @p store in a slot and answers its identifier. */
    st::expected<ustore_snapshot_t> open(ordered_store<isolation_> const& store) noexcept {
        st::expected<std::uint64_t> const claimed = claim_slot_();
        if (!claimed) return claimed.status();
        slot_t& slot = *slot_at_(*claimed);
        new (&slot.reader) reader_t(store.reader());
        std::uint64_t const serial =
            fu::standard_atomic_ref<std::uint64_t>(creations_).fetch_add(1, st::memory_order_release_k) %
                (std::numeric_limits<std::uint64_t>::max() >> slot_index_bits_k) +
            1;
        std::uint64_t const identifier = (serial << slot_index_bits_k) | *claimed;
        release_word(slot.identifier, identifier);
        return identifier;
    }

    /** The reader @p identifier names, or null for an identifier naming no open snapshot. */
    pinned_reader<isolation_> const* find(ustore_snapshot_t identifier) const noexcept {
        slot_t const* const slot = slot_named_(identifier);
        if (!slot || acquire_word(slot->identifier) != identifier) return nullptr;
        return &slot->reader;
    }

    /** Releases the snapshot @p identifier names, answering whether one was open. */
    bool close(ustore_snapshot_t identifier) noexcept {
        slot_t* const slot = slot_named_(identifier);
        if (!slot || !exchange_word(slot->identifier, identifier, changing_slot_k)) return false;
        slot->reader.~reader_t();
        release_word(slot->identifier, free_slot_k);
        return true;
    }

    /** Releases every open snapshot. */
    void close_every() noexcept {
        std::uint64_t const count = acquire_word(slots_count_);
        for (std::uint64_t index = 0; index != count; ++index) {
            slot_t const* const slot = slot_at_(index);
            if (!slot) continue;
            [[maybe_unused]] bool const closed = close(acquire_word(slot->identifier));
        }
    }

    /** An upper bound on how many snapshots are open. */
    std::size_t capacity() const noexcept { return static_cast<std::size_t>(acquire_word(slots_count_)); }

    /** Writes up to @p limit open identifiers into @p identifiers and answers how many it wrote. */
    template <typename output_type_>
    std::size_t list(output_type_& identifiers, std::size_t limit) const noexcept {
        std::size_t listed = 0;
        for (std::uint64_t index = 0; index != limit; ++index) {
            slot_t const* const slot = slot_at_(index);
            std::uint64_t const identifier = slot ? acquire_word(slot->identifier) : free_slot_k;
            if (identifier > changing_slot_k) identifiers[listed++] = identifier;
        }
        return listed;
    }

  private:

    /** The slot at @p index, or null while its chunk is unpublished. */
    slot_t* slot_at_(std::uint64_t index) const noexcept {
        slot_position_t const position = position_of(index);
        slot_t* const chunk = acquire_word(chunks_[position.chunk]);
        return chunk ? chunk + position.offset : nullptr;
    }

    /** The slot @p identifier addresses, or null when it addresses none. */
    slot_t* slot_named_(ustore_snapshot_t identifier) const noexcept {
        if ((identifier >> slot_index_bits_k) == 0) return nullptr;
        std::uint64_t const index = identifier & slot_index_mask_k;
        if (index >= slots_limit_k) return nullptr;
        return slot_at_(index);
    }

    /** Claims a released slot, or a fresh one when none is free, and answers its index. */
    st::expected<std::uint64_t> claim_slot_() noexcept {
        std::uint64_t const count = acquire_word(slots_count_);
        for (std::uint64_t index = 0; index != count; ++index) {
            slot_t* const slot = slot_at_(index);
            if (slot && exchange_word(slot->identifier, free_slot_k, changing_slot_k)) return index;
        }
        return claim_fresh_slot_();
    }

    /** Hands out the next never-used slot index, publishing its chunk on first use. */
    st::expected<std::uint64_t> claim_fresh_slot_() noexcept {
        for (std::uint64_t attempt = 0; attempt != slots_limit_k; ++attempt) {
            std::uint64_t const index = fu::atomic_fetch_add_if_at_most(st::atomic_ref<std::uint64_t>(slots_count_),
                                                                        std::uint64_t {1}, slots_limit_k,
                                                                        st::memory_order_release_k);
            if (index == slots_limit_k) return store_status_t::capacity_exhausted_k;
            slot_t* const chunk = publish_chunk_(position_of(index).chunk);
            if (!chunk) return store_status_t::out_of_memory_heap_k;
            if (exchange_word(chunk[position_of(index).offset].identifier, free_slot_k, changing_slot_k)) return index;
        }
        return store_status_t::capacity_exhausted_k;
    }

    /** The chunk at @p chunk, built and published by whichever thread gets there first. */
    slot_t* publish_chunk_(std::size_t chunk) noexcept {
        slot_t* const published = acquire_word(chunks_[chunk]);
        if (published) return published;
        std::size_t const count = static_cast<std::size_t>(first_chunk_slots_k << chunk);
        slot_t* const built = allocator_t {}.allocate(count);
        if (!built) return nullptr;
        for (std::size_t offset = 0; offset != count; ++offset) new (built + offset) slot_t();
        if (exchange_word<slot_t*>(chunks_[chunk], nullptr, built)) return built;
        release_chunk_(built, chunk);
        return acquire_word(chunks_[chunk]);
    }

    /** Frees @p slots, the chunk at @p chunk, whose snapshots are all closed. */
    static void release_chunk_(slot_t* slots, std::size_t chunk) noexcept {
        if (!slots) return;
        std::size_t const count = static_cast<std::size_t>(first_chunk_slots_k << chunk);
        std::destroy_n(slots, count);
        allocator_t {}.deallocate(slots, count);
    }
};

#pragma endregion Snapshot Directory

#pragma region Handles

/** Everything a database at @p isolation_ holds. */
template <isolation_t isolation_>
struct database_state {

    /** Every pair, every catalog entry and the collection counter. */
    ordered_store<isolation_> store;

    /** Every open snapshot. */
    snapshot_directory<isolation_> snapshots;

    /** Directory the image persists in, empty for a volatile database. */
    sz::string directory;
};

/** A database handle of the C ABI, holding one isolation chosen at open. */
struct database_t {

    /** The database at the isolation its configuration named. */
    std::variant<database_state<isolation_t::strict_serializable_k>, database_state<isolation_t::snapshot_k>> state;

    template <isolation_t isolation_>
    explicit database_t(std::in_place_type_t<database_state<isolation_>> alternative) noexcept : state(alternative) {}
};

/** Leads every transaction handle, so a call tells a transaction of another isolation apart before using it. */
struct transaction_header_t {

    /** The isolation of the database that opened the transaction. */
    isolation_t isolation;
};

/** A transaction handle of the C ABI. */
template <isolation_t isolation_>
struct transaction_handle : transaction_header_t {

    /** The transaction over every partition. */
    store_transaction<isolation_> transaction;

    /** Whether the writes of the transaction are staged. */
    staging_t staging {staging_t::pending_k};

    explicit transaction_handle(store_transaction<isolation_>&& opened) noexcept
        : transaction_header_t {isolation_}, transaction(std::move(opened)) {}
};

/** The transaction behind @p transaction, or null after reporting one of another isolation. */
template <isolation_t isolation_>
transaction_handle<isolation_>* transaction_of(ustore_transaction_t transaction, ustore_error_t* error) noexcept {
    auto* const header = static_cast<transaction_header_t*>(transaction);
    if (header->isolation == isolation_) return static_cast<transaction_handle<isolation_>*>(header);
    *error = {ustore_error_invalid_argument_k, "The transaction belongs to a database of another isolation"};
    return nullptr;
}

#pragma endregion Handles

#pragma region Views

/** Reads what the newest published commit shows, recording nothing. */
template <isolation_t isolation_>
struct head_view {

    /** The store being read. */
    ordered_store<isolation_> const& store;

    template <typename found_type_, typename missing_type_>
    store_status_t find(stored_key_t const& key, found_type_&& found, missing_type_&& missing) const noexcept {
        return store.find(key, found, missing);
    }

    template <typename callback_type_>
    store_status_t range(stored_key_t const& lower, stored_key_t const& upper,
                         callback_type_&& callback) const noexcept {
        return store.range(lower, upper, callback);
    }

    store_status_t prepare(std::size_t) const noexcept { return store_status_t::success_k; }

    store_status_t watch(stored_key_t const&) const noexcept { return store_status_t::success_k; }
};

/** Reads at the stamp a snapshot pinned, recording nothing. */
template <isolation_t isolation_>
struct snapshot_view {

    /** The reader of the snapshot. */
    pinned_reader<isolation_> const& reader;

    template <typename found_type_, typename missing_type_>
    store_status_t find(stored_key_t const& key, found_type_&& found, missing_type_&& missing) const noexcept {
        return reader.find(key, found, missing);
    }

    template <typename callback_type_>
    store_status_t range(stored_key_t const& lower, stored_key_t const& upper,
                         callback_type_&& callback) const noexcept {
        return reader.range(lower, upper, callback);
    }

    store_status_t prepare(std::size_t) const noexcept { return store_status_t::success_k; }

    store_status_t watch(stored_key_t const&) const noexcept { return store_status_t::success_k; }
};

/** Reads at the snapshot of a transaction, which records what it read from @c serializable_k up. */
template <isolation_t isolation_>
struct transaction_view {

    /** The transaction being read through. */
    store_transaction<isolation_>& transaction;

    template <typename found_type_, typename missing_type_>
    store_status_t find(stored_key_t const& key, found_type_&& found, missing_type_&& missing) const noexcept {
        return transaction.find(key, found, missing);
    }

    template <typename callback_type_>
    store_status_t range(stored_key_t const& lower, stored_key_t const& upper,
                         callback_type_&& callback) const noexcept {
        return transaction.range(lower, upper, callback);
    }

    store_status_t prepare(std::size_t count) const noexcept { return transaction.reserve(count); }

    store_status_t watch(stored_key_t const&) const noexcept { return store_status_t::success_k; }
};

/** Reads through a snapshot-isolated transaction, watching every key it hands out. */
template <isolation_t isolation_>
struct watching_view {

    /** The transaction being read through. */
    store_transaction<isolation_>& transaction;

    template <typename found_type_, typename missing_type_>
    store_status_t find(stored_key_t const& key, found_type_&& found, missing_type_&& missing) const noexcept {
        return transaction.find_and_watch(key, found, missing);
    }

    template <typename callback_type_>
    store_status_t range(stored_key_t const& lower, stored_key_t const& upper,
                         callback_type_&& callback) const noexcept {
        return transaction.range(lower, upper, callback);
    }

    store_status_t prepare(std::size_t count) const noexcept { return transaction.reserve(count); }

    store_status_t watch(stored_key_t const& key) const noexcept { return transaction.watch(key); }
};

/** The view a read, scan, sample, measure or listing names. */
struct view_request_t {

    /** The transaction to read through, or null. */
    ustore_transaction_t transaction;

    /** The snapshot to read at, which wins over the transaction, or zero. */
    ustore_snapshot_t snapshot;

    /** The options of the call, which decide whether a snapshot-isolated transaction watches. */
    ustore_options_t options;
};

/** Hands @p action the view @p request names, or reports why none exists. */
template <isolation_t isolation_, typename action_type_>
void with_view(database_state<isolation_>& state, view_request_t const& request, ustore_error_t* error,
               action_type_&& action) noexcept {
    if (request.snapshot) {
        pinned_reader<isolation_> const* const reader = state.snapshots.find(request.snapshot);
        if (!reader) {
            *error = {ustore_error_not_found_k, "The snapshot doesn't exist"};
            return;
        }
        return action(snapshot_view<isolation_> {*reader});
    }
    if (!request.transaction) return action(head_view<isolation_> {state.store});

    transaction_handle<isolation_>* const handle = transaction_of<isolation_>(request.transaction, error);
    if (!handle) return;
    bool const watches = isolation_ == isolation_t::snapshot_k &&
                         !(request.options & ustore_option_transaction_dont_watch_k);
    if (watches) return action(watching_view<isolation_> {handle->transaction});
    action(transaction_view<isolation_> {handle->transaction});
}

#pragma endregion Views

#pragma region Output Tapes

/** The columns of a batched read, every value copied into the caller's arena exactly once. */
class value_tape {
    linked_memory_lock_t& arena_;
    ustore_error_t* error_;
    range_or_dummy<bits_span_t> presences_;
    range_or_dummy<std::span<ustore_length_t>> offsets_;
    range_or_dummy<std::span<ustore_length_t>> lengths_;
    std::span<byte_t> contents_ {};
    ustore_byte_t** values_;
    std::size_t count_;
    std::size_t index_ {0};
    std::size_t used_ {0};

    /** Makes room for @p length more bytes, extending the tape in place while it ends its arena block. */
    bool reserve_(std::size_t length) noexcept {
        if (!values_ || used_ + length <= contents_.size()) return true;
        std::size_t const predicted = std::max(contents_.size() * 2, length * (count_ - index_));
        std::size_t const wanted = std::max(predicted, used_ + length);
        if (contents_.size() == 0) contents_ = arena_.alloc<byte_t>(wanted, error_);
        else contents_ = arena_.grow(contents_, wanted - contents_.size(), error_);
        return !error_->status;
    }

  public:
    value_tape(linked_memory_lock_t& arena, ustore_read_t& arguments) noexcept
        : arena_(arena), error_(arguments.error),
          presences_(arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.presences)),
          offsets_(arena.alloc_or_dummy(arguments.tasks_count + 1, arguments.error, arguments.offsets)),
          lengths_(arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.lengths)),
          values_(arguments.values), count_(arguments.tasks_count) {}

    /** Appends a found value. */
    void append(blob_t const& blob) noexcept {
        std::size_t const length = blob.size();
        if (!reserve_(length)) return;
        if (values_ && length != 0) std::memcpy(contents_.data() + used_, blob.data(), length);
        presences_[index_] = true;
        offsets_[index_] = static_cast<ustore_length_t>(used_);
        lengths_[index_] = static_cast<ustore_length_t>(length);
        used_ += length;
        ++index_;
    }

    /** Appends a missing value. */
    void append_missing() noexcept {
        presences_[index_] = false;
        offsets_[index_] = static_cast<ustore_length_t>(used_);
        lengths_[index_] = ustore_length_missing_k;
        ++index_;
    }

    /** Closes the offsets and exports the tape of values. */
    void finish() noexcept {
        offsets_[count_] = static_cast<ustore_length_t>(used_);
        if (values_) *values_ = reinterpret_cast<ustore_byte_t*>(contents_.data());
    }
};

/** The columns of batched scans and samples: offsets and counts per task over one tape of keys. */
class key_tape {
    ustore_error_t* error_;
    range_or_dummy<std::span<ustore_length_t>> offsets_;
    range_or_dummy<std::span<ustore_length_t>> counts_;
    uninitialized_array<ustore_key_t> keys_;
    std::size_t tasks_;

  public:
    key_tape(linked_memory_lock_t& arena, std::size_t tasks, ustore_length_t** offsets, ustore_length_t** counts,
             ustore_error_t* error) noexcept
        : error_(error), offsets_(arena.alloc_or_dummy(tasks + 1, error, offsets)),
          counts_(arena.alloc_or_dummy(tasks, error, counts)), keys_(arena), tasks_(tasks) {}

    /** Sizes the tape for @p keys keys, so a walk within it never moves the tape. */
    void reserve(std::size_t keys) noexcept { keys_.reserve(keys, error_); }

    /** How many keys the tape holds. */
    std::size_t size() const noexcept { return keys_.size(); }

    /** The key at @p index. */
    ustore_key_t& operator[](std::size_t index) noexcept { return keys_[index]; }

    /** Appends @p key, halting the walk when the arena refuses. */
    walk_control_t push(ustore_key_t key) noexcept {
        keys_.push_back(key, error_);
        return error_->status ? walk_control_t::halt_k : walk_control_t::resume_k;
    }

    /** Records where task @p task starts and how many keys it holds, from @p first on. */
    void close_task(std::size_t task, std::size_t first) noexcept {
        offsets_[task] = static_cast<ustore_length_t>(first);
        counts_[task] = static_cast<ustore_length_t>(keys_.size() - first);
    }

    /** Closes the offsets and exports the tape of keys. */
    void finish(ustore_key_t** keys) noexcept {
        offsets_[tasks_] = static_cast<ustore_length_t>(keys_.size());
        *keys = keys_.begin();
    }
};

#pragma endregion Output Tapes

#pragma region Reads

/** Reads every place of @p places through @p view into @p tape. */
template <typename view_type_>
store_status_t read_places(view_type_ const& view, places_arg_t const& places, value_tape& tape,
                           ustore_error_t* error) noexcept {
    store_status_t const reserved = view.prepare(places.size());
    if (st::failed(reserved)) return reserved;
    auto const found = [&](pair_t const& pair) noexcept { tape.append(pair.mapped); };
    auto const missing = [&]() noexcept { tape.append_missing(); };
    for (std::size_t index = 0; index != places.size() && !error->status; ++index) {
        place_t const place = places[index];
        store_status_t const looked_up = view.find(stored_key_t {place.collection, place.key}, found, missing);
        if (st::failed(looked_up)) return looked_up;
    }
    return store_status_t::success_k;
}

/** Watches the keys of @p collection the tape holds from @p first on, which only a watching view records. */
template <typename view_type_>
store_status_t watch_keys(view_type_ const& view, ustore_collection_t collection, key_tape& tape,
                          std::size_t first) noexcept {
    for (std::size_t index = first; index != tape.size(); ++index) {
        store_status_t const watched = view.watch(stored_key_t {collection, tape[index]});
        if (st::failed(watched)) return watched;
    }
    return store_status_t::success_k;
}

/** Appends up to the limit of @p scan keys of its collection, from its start key on. */
template <typename view_type_>
store_status_t scan_window(view_type_ const& view, scan_t scan, key_tape& tape) noexcept {
    std::size_t const first = tape.size();
    if (scan.limit == 0) return store_status_t::success_k;
    store_status_t const walked = view.range(
        stored_key_t {scan.collection, scan.min_key}, stored_key_t::past(scan.collection),
        [&](pair_t const& pair) noexcept -> walk_control_t {
            if (tape.push(pair.key.key) == walk_control_t::halt_k) return walk_control_t::halt_k;
            return tape.size() - first == scan.limit ? walk_control_t::halt_k : walk_control_t::resume_k;
        });
    if (st::failed(walked)) return walked;
    return watch_keys(view, scan.collection, tape, first);
}

/** A splitmix64 generator, drawing the positions a reservoir keeps. */
struct sample_generator_t {
    using result_type = std::uint64_t;

    /** The state every draw advances. */
    std::uint64_t state;

    static constexpr result_type min() noexcept { return 0; }
    static constexpr result_type max() noexcept { return std::numeric_limits<result_type>::max(); }

    result_type operator()() noexcept {
        std::uint64_t mixed = (state += 0x9E3779B97F4A7C15ull);
        mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
        mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
        return mixed ^ (mixed >> 31);
    }
};

/** Appends a uniform reservoir of up to the limit of @p sample keys of its collection. */
template <typename view_type_>
store_status_t sample_window(view_type_ const& view, sample_arg_t sample, sample_generator_t& generator,
                             key_tape& tape) noexcept {
    std::size_t const first = tape.size();
    std::size_t seen = 0;
    store_status_t const walked = view.range(
        stored_key_t::lowest_in(sample.collection), stored_key_t::past(sample.collection),
        [&](pair_t const& pair) noexcept -> walk_control_t {
            std::size_t const slot = seen < sample.limit ? seen : st::draw_below(generator, seen + 1);
            ++seen;
            if (slot >= sample.limit) return walk_control_t::resume_k;
            if (slot == tape.size() - first) return tape.push(pair.key.key);
            tape[first + slot] = pair.key.key;
            return walk_control_t::resume_k;
        });
    if (st::failed(walked)) return walked;
    return watch_keys(view, sample.collection, tape, first);
}

/** The entries and value bytes one measured window holds. */
struct window_measure_t {

    /** Entries in the window. */
    std::size_t cardinality {0};

    /** Bytes of every value in the window. */
    std::size_t value_bytes {0};
};

/** Measures [ @p lower, @p last ], which share one collection. */
template <typename view_type_>
store_status_t measure_window(view_type_ const& view, stored_key_t const& lower, stored_key_t const& last,
                              window_measure_t& measured) noexcept {
    return view.range(lower, stored_key_t::past(lower.collection), [&](pair_t const& pair) noexcept -> walk_control_t {
        if (stored_key_order_t {}(last, pair.key)) return walk_control_t::halt_k;
        ++measured.cardinality;
        measured.value_bytes += pair.mapped.size();
        return walk_control_t::resume_k;
    });
}

/** The collections a listing exports. */
struct catalog_listing_t {

    /** The identifier of every collection. */
    uninitialized_array<ustore_collection_t> identifiers;
};

/** Appends the catalog entry @p pair to @p listing, halting the walk when the arena refuses. */
inline walk_control_t list_entry(catalog_listing_t& listing, pair_t const& pair, ustore_error_t* error) noexcept {
    listing.identifiers.push_back(pair.key.cataloged(), error);
    return error->status ? walk_control_t::halt_k : walk_control_t::resume_k;
}

/** Walks the catalog of @p view into @p listing. */
template <typename view_type_>
store_status_t list_catalog(view_type_ const& view, catalog_listing_t& listing, ustore_error_t* error) noexcept {
    return view.range(stored_key_t::lowest_in(catalog_collection_k), stored_key_t::collection_counter(),
                      [&](pair_t const& pair) noexcept { return list_entry(listing, pair, error); });
}

#pragma endregion Reads

#pragma region Writes

/** Stages @p content under @p key into @p transaction. */
template <typename transaction_type_>
store_status_t stage_upsert(transaction_type_& transaction, stored_key_t const& key, value_view_t content) noexcept {
    auto copied = copy_blob(content);
    if (!copied) return copied.status();
    return transaction.upsert(pair_t {key, std::move(*copied)});
}

/** Stages erasing @p key, watching its absence when @p watch asks and the key is already gone. */
template <typename transaction_type_>
store_status_t stage_erase(transaction_type_& transaction, stored_key_t const& key, watch_t watch) noexcept {
    store_status_t const erased = transaction.erase(key);
    if (erased != store_status_t::key_not_found_k) return erased;
    if (watch == watch_t::dont_watch_k) return store_status_t::success_k;
    return transaction.watch(key);
}

/** Stages a batch into @p transaction, where a missing value erases its key. */
template <typename transaction_type_>
store_status_t stage_writes(transaction_type_& transaction, places_arg_t const& places, contents_arg_t const& contents,
                            watch_t watch) noexcept {
    for (std::size_t index = 0; index != places.size(); ++index) {
        place_t const place = places[index];
        stored_key_t const key {place.collection, place.key};
        value_view_t const content = contents[index];
        store_status_t const staged = content ? stage_upsert(transaction, key, content)
                                              : stage_erase(transaction, key, watch);
        if (st::failed(staged)) return staged;
    }
    return store_status_t::success_k;
}

/** Publishes one write under a stamp of its own, with no transaction to open. */
template <isolation_t isolation_>
store_status_t write_directly(ordered_store<isolation_>& store, stored_key_t const& key,
                              value_view_t content) noexcept {
    if (content) return stage_upsert(store, key, content);
    store_status_t const erased = store.erase(key);
    return erased == store_status_t::key_not_found_k ? store_status_t::success_k : erased;
}

/** Commits @p stage_changes in a transaction of its own, retrying conflicts up to @c commit_attempts_k times. */
template <isolation_t isolation_, typename stage_changes_type_>
store_status_t commit_atomically(ordered_store<isolation_>& store, stage_changes_type_&& stage_changes) noexcept {
    auto opened = store.transaction();
    if (!opened) return opened.status();
    return st::commit_with_retries(*opened, commit_attempts_k, stage_changes);
}

/** Whether any place of @p places addresses the catalog, which only the engine writes. */
inline bool touches_catalog(places_arg_t const& places) noexcept {
    for (std::size_t index = 0; index != places.size(); ++index)
        if (places[index].collection == catalog_collection_k) return true;
    return false;
}

#pragma endregion Writes

#pragma region Catalog

/** Stages the counter at @p next through @p transaction. */
template <typename transaction_type_>
store_status_t stage_counter(transaction_type_& transaction, ustore_collection_t next) noexcept {
    return stage_upsert(
        transaction, stored_key_t::collection_counter(),
        value_view_t {reinterpret_cast<ustore_bytes_cptr_t>(&next), static_cast<ustore_length_t>(sizeof(next))});
}

/** Reads the identifier the next created collection takes. */
template <typename transaction_type_>
store_status_t read_counter(transaction_type_& transaction, ustore_collection_t& next) noexcept {
    next = main_collection_k + 1;
    return transaction.find(
        stored_key_t::collection_counter(),
        [&](pair_t const& pair) noexcept {
            std::memcpy(reinterpret_cast<char*>(&next), pair.mapped.data(), sizeof(next));
        },
        st::no_op_t {});
}

/** Stages a new collection and the advanced counter, answering its identifier through @p created. */
template <isolation_t isolation_>
store_status_t stage_collection(store_transaction<isolation_>& transaction, ustore_collection_t& created) noexcept {
    store_status_t const counted = read_counter(transaction, created);
    if (st::failed(counted)) return counted;
    if (created >= collections_limit_k) return store_status_t::capacity_exhausted_k;
    store_status_t const advanced = stage_counter(transaction, created + 1);
    if (st::failed(advanced)) return advanced;
    return stage_upsert(transaction, stored_key_t::catalog_entry(created), value_view_t::make_empty());
}

/** Answers @c key_not_found_k unless @p collection is the main one or listed in the catalog. */
template <typename transaction_type_>
store_status_t require_listed(transaction_type_& transaction, ustore_collection_t collection) noexcept {
    if (collection == main_collection_k || collection == names_collection_k) return store_status_t::success_k;
    store_status_t listing = store_status_t::key_not_found_k;
    store_status_t const looked_up = transaction.find(
        stored_key_t::catalog_entry(collection), [&](pair_t const&) noexcept { listing = store_status_t::success_k; },
        st::no_op_t {});
    return st::failed(looked_up) ? looked_up : listing;
}

/** Empties @p value in place, keeping its key. */
inline void empty_value(stored_key_t const&, blob_t& value) noexcept {
    [[maybe_unused]] store_status_t const emptied = value.resize(0);
}

/** Stages what dropping @p collection in @p mode removes. */
template <typename transaction_type_>
store_status_t stage_drop(transaction_type_& transaction, ustore_collection_t collection,
                          ustore_drop_mode_t mode) noexcept {
    store_status_t const listed = require_listed(transaction, collection);
    if (st::failed(listed)) return listed;
    stored_key_t const lower = stored_key_t::lowest_in(collection);
    stored_key_t const upper = stored_key_t::past(collection);
    if (mode == ustore_drop_vals_k) return transaction.update_range(lower, upper, empty_value);
    store_status_t const erased = transaction.erase_range(lower, upper, st::no_op_t {});
    if (st::failed(erased) || mode == ustore_drop_keys_vals_k) return erased;
    return transaction.erase(stored_key_t::catalog_entry(collection));
}

#pragma endregion Catalog

#pragma region Image

/** Leads every image, so a file of another format is refused rather than misread. */
inline constexpr char image_signature_k[8] = {'U', 'S', 'T', 'S', 'M', 'T', 'B', '1'};

/** Name of the image inside the database directory. */
inline constexpr std::string_view image_name_k = "/ram.image";

/** Name of the image while it is being written, renamed over the image once whole. */
inline constexpr std::string_view partial_image_name_k = "/ram.image.partial";

#if defined(_WIN32)

/** Flushes the file behind @p file to the drive. */
inline bool sync_file(std::FILE* file) noexcept { return ::_commit(::_fileno(file)) == 0; }

/** Replaces @p target with @p source in one step, overwriting an existing target. */
inline bool replace_file(char const* source, char const* target) noexcept {
    return ::MoveFileExA(source, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
#else

/** Flushes the file behind @p file to the drive. */
inline bool sync_file(std::FILE* file) noexcept { return ::fsync(::fileno(file)) == 0; }

/** Replaces @p target with @p source in one step, overwriting an existing target. */
inline bool replace_file(char const* source, char const* target) noexcept { return std::rename(source, target) == 0; }
#endif

/** Closes a C stream when it leaves scope, unless it was closed explicitly. */
class stream_t {
    std::FILE* handle_ = nullptr;

  public:
    explicit stream_t(std::FILE* handle) noexcept : handle_(handle) {}
    stream_t(stream_t const&) = delete;
    stream_t& operator=(stream_t const&) = delete;

    ~stream_t() noexcept {
        if (handle_) { [[maybe_unused]] int const closed = std::fclose(handle_); }
    }

    /** The stream, or null once closed. */
    std::FILE* get() const noexcept { return handle_; }

    /** Closes the stream now, answering whether everything buffered reached the file. */
    bool close() noexcept { return std::fclose(std::exchange(handle_, nullptr)) == 0; }
};

/** Joins @p directory and @p name into @p path, answering whether it fit in memory. */
inline bool join_path(sz::string& path, char const* directory, std::string_view name) noexcept {
    return path.try_assign(sz::string_view {directory}) && path.try_append(name.data(), name.size());
}

/** Writes one pair as its key, its length and its bytes, answering whether all of it reached @p file. */
inline bool write_pair(std::FILE* file, pair_t const& pair) noexcept {
    auto const length = static_cast<ustore_length_t>(pair.mapped.size());
    return std::fwrite(&pair.key, sizeof(stored_key_t), 1, file) == 1 &&
           std::fwrite(&length, sizeof(length), 1, file) == 1 &&
           (length == 0 || std::fwrite(pair.mapped.data(), 1, length, file) == length);
}

/** Writes every pair @p view reads into the image of @p directory: a signature, the key width, then each pair. */
template <typename view_type_>
void write_image(view_type_ const& view, char const* directory, ustore_error_t* error) noexcept {
    sz::string partial_path;
    sz::string final_path;
    if (!join_path(partial_path, directory, partial_image_name_k) || !join_path(final_path, directory, image_name_k)) {
        *error = {ustore_error_out_of_memory_k, "Failed to build the image path"};
        return;
    }
    stream_t stream {std::fopen(partial_path.c_str(), "wb")};
    if (!stream.get()) {
        *error = {ustore_error_io_k, "Failed to open the image for writing"};
        return;
    }

    std::uint32_t const key_bytes = sizeof(stored_key_t);
    bool written = std::fwrite(image_signature_k, sizeof(image_signature_k), 1, stream.get()) == 1 &&
                   std::fwrite(&key_bytes, sizeof(key_bytes), 1, stream.get()) == 1;
    store_status_t const walked = view.range(stored_key_t::lowest(), stored_key_t::highest(),
                                             [&](pair_t const& pair) noexcept -> walk_control_t {
                                                 written = written && write_pair(stream.get(), pair);
                                                 return written ? walk_control_t::resume_k : walk_control_t::halt_k;
                                             });
    if (reported_failure(walked, error)) return;

    bool const synced = written && std::fflush(stream.get()) == 0 && sync_file(stream.get());
    bool const closed = stream.close();
    if (!synced || !closed) {
        *error = {ustore_error_io_k, "Failed to write the image"};
        return;
    }
    if (!replace_file(partial_path.c_str(), final_path.c_str()))
        *error = {ustore_error_io_k, "Failed to replace the image"};
}

/** What loading one pair of an image came to. */
enum class image_step_t : std::uint8_t { loaded_k, finished_k, truncated_k, out_of_memory_k, refused_k };

/** The error of every image step, success for the two that are not failures. */
inline constexpr std::array<ustore_error_t, 5> image_step_errors_k {{
    {},
    {},
    {ustore_error_malformed_k, "The image is truncated"},
    {ustore_error_out_of_memory_k, "Out of memory while loading the image"},
    {ustore_error_internal_k, "The store refused a pair of the image"},
}};

/** Loads the next pair of @p file into @p store. */
template <isolation_t isolation_>
image_step_t load_pair(std::FILE* file, ordered_store<isolation_>& store) noexcept {
    stored_key_t key;
    if (std::fread(&key, sizeof(stored_key_t), 1, file) != 1) return image_step_t::finished_k;
    ustore_length_t length = 0;
    if (std::fread(&length, sizeof(length), 1, file) != 1) return image_step_t::truncated_k;
    blob_t blob;
    if (st::failed(blob.resize(length))) return image_step_t::out_of_memory_k;
    if (length != 0 && std::fread(blob.data(), 1, length, file) != length) return image_step_t::truncated_k;
    if (st::failed(store.upsert(pair_t {key, std::move(blob)}))) return image_step_t::refused_k;
    return image_step_t::loaded_k;
}

/** Whether @p file opens with the signature and key width of an image. */
inline bool recognize_image(std::FILE* file) noexcept {
    char signature[sizeof(image_signature_k)];
    std::uint32_t key_bytes = 0;
    return std::fread(signature, sizeof(signature), 1, file) == 1 &&
           std::memcmp(signature, image_signature_k, sizeof(signature)) == 0 &&
           std::fread(&key_bytes, sizeof(key_bytes), 1, file) == 1 && key_bytes == sizeof(stored_key_t);
}

/** Loads the image of the directory of @p state into its store, leaving it empty when there is none. */
template <isolation_t isolation_>
void read_image(database_state<isolation_>& state, ustore_error_t* error) noexcept {
    sz::string path;
    if (!join_path(path, state.directory.c_str(), image_name_k)) {
        *error = {ustore_error_out_of_memory_k, "Failed to build the image path"};
        return;
    }
    stream_t stream {std::fopen(path.c_str(), "rb")};
    if (!stream.get()) return;
    if (!recognize_image(stream.get())) {
        *error = {ustore_error_malformed_k, "The image has an unknown format"};
        return;
    }
    image_step_t step = image_step_t::loaded_k;
    while (step == image_step_t::loaded_k) step = load_pair(stream.get(), state.store);
    *error = image_step_errors_k[static_cast<std::size_t>(step)];
}

/** Rewrites the image of @p state from its newest published commit, when it has a directory. */
template <isolation_t isolation_>
void persist(database_state<isolation_>& state, ustore_error_t* error) noexcept {
    if (state.directory.empty()) return;
    pinned_reader<isolation_> const reader = state.store.reader();
    write_image(snapshot_view<isolation_> {reader}, state.directory.c_str(), error);
}

#pragma endregion Image

#pragma region Lifetime

/** Remembers the directory of @p state and loads its image. */
template <isolation_t isolation_>
void open_database(database_state<isolation_>& state, std::string_view directory, ustore_error_t* error) noexcept {
    if (directory.empty()) return;
    if (!state.directory.try_assign(sz::string_view {directory})) {
        *error = {ustore_error_out_of_memory_k, "Failed to copy the directory"};
        return;
    }
    read_image(state, error);
}

/** Releases the snapshots of @p state and persists it, ahead of freeing it. */
template <isolation_t isolation_>
void close_database(database_state<isolation_>& state) noexcept {
    state.snapshots.close_every();
    ustore_error_t error {};
    persist(state, &error);
}

/** Serves "clear", removing every pair and keeping the catalog. */
template <isolation_t isolation_>
void clear_pairs(database_state<isolation_>& state, ustore_database_control_t& arguments) noexcept {
    store_status_t const cleared = commit_atomically(
        state.store, [](store_transaction<isolation_>& transaction) noexcept {
            return transaction.erase_up_to(stored_key_t::lowest_in(catalog_collection_k), st::no_op_t {});
        });
    reported_failure(cleared, arguments.error);
}

/** Serves "reset", removing every snapshot, pair and collection. */
template <isolation_t isolation_>
void reset_database(database_state<isolation_>& state, ustore_database_control_t& arguments) noexcept {
    state.snapshots.close_every();
    store_status_t const reset = commit_atomically(
        state.store, [](store_transaction<isolation_>& transaction) noexcept {
            return transaction.erase_from(stored_key_t::lowest(), st::no_op_t {});
        });
    reported_failure(reset, arguments.error);
}

/** Serves "compact", reclaiming unreachable versions and rewriting the image. */
template <isolation_t isolation_>
void compact_database(database_state<isolation_>& state, ustore_database_control_t& arguments) noexcept {
    auto const vacuumed = state.store.vacuum();
    if (reported_failure(vacuumed.status(), arguments.error)) return;
    persist(state, arguments.error);
}

/** Serves "info", naming the engine and its isolation. */
template <isolation_t isolation_>
void describe_database(database_state<isolation_>&, ustore_database_control_t& arguments) noexcept {
    *arguments.response = isolation_ == isolation_t::strict_serializable_k
                              ? R"({"engine":"ram","isolation":"strict_serializable"})"
                              : R"({"engine":"ram","isolation":"snapshot"})";
}

/** Writes @p text at @p cursor and answers where it ends. */
inline char* append_text(char* cursor, std::string_view text) noexcept {
    std::memcpy(cursor, text.data(), text.size());
    return cursor + text.size();
}

/** Serves "usage", counting keys and versions into the arena. */
template <isolation_t isolation_>
void report_usage(database_state<isolation_>& state, ustore_database_control_t& arguments) noexcept {
    linked_memory_lock_t arena = linked_memory(arguments.arena, ustore_options_default_k, arguments.error);
    if (arguments.error->status) return;
    constexpr std::size_t response_capacity_k = 96;
    char* const response = arena.alloc<char>(response_capacity_k, arguments.error).data();
    if (arguments.error->status) return;
    char* const response_end = response + response_capacity_k;

    char* cursor = append_text(response, R"({"keys":)");
    cursor = std::to_chars(cursor, response_end, state.store.size()).ptr;
    cursor = append_text(cursor, R"(,"versions":)");
    cursor = std::to_chars(cursor, response_end, state.store.versions_count()).ptr;
    cursor = append_text(cursor, "}");
    *cursor = '\0';
    *arguments.response = response;
}

/** One free-form request of @c ustore_database_control. */
template <isolation_t isolation_>
struct control_request {

    /** The name a caller sends. */
    std::string_view name;

    /** Serves the request. */
    void (*serve)(database_state<isolation_>&, ustore_database_control_t&) noexcept;
};

/** Every request @c ustore_database_control serves. */
template <isolation_t isolation_>
inline constexpr std::array<control_request<isolation_>, 5> control_requests_k {{
    {"clear", &clear_pairs<isolation_>},
    {"reset", &reset_database<isolation_>},
    {"compact", &compact_database<isolation_>},
    {"info", &describe_database<isolation_>},
    {"usage", &report_usage<isolation_>},
}};

#pragma endregion Lifetime

#pragma region Argument Views

/** The locations of a read. */
inline places_arg_t places_of(ustore_read_t const& arguments) noexcept {
    return {{arguments.collections, arguments.collections_stride},
            {arguments.keys, arguments.keys_stride},
            {},
            arguments.tasks_count};
}

/** The locations of a write. */
inline places_arg_t places_of(ustore_write_t const& arguments) noexcept {
    return {{arguments.collections, arguments.collections_stride},
            {arguments.keys, arguments.keys_stride},
            {},
            arguments.tasks_count};
}

/** The values of a write. */
inline contents_arg_t contents_of(ustore_write_t const& arguments) noexcept {
    return {bits_view_t {arguments.presences},
            {arguments.offsets, arguments.offsets_stride},
            {arguments.lengths, arguments.lengths_stride},
            {arguments.values, arguments.values_stride},
            arguments.tasks_count};
}

/** The tasks of a scan. */
inline scans_arg_t scans_of(ustore_scan_t const& arguments) noexcept {
    return {{arguments.collections, arguments.collections_stride},
            {arguments.start_keys, arguments.start_keys_stride},
            {arguments.count_limits, arguments.count_limits_stride},
            arguments.tasks_count};
}

/** The tasks of a sample. */
inline sample_args_t samples_of(ustore_sample_t const& arguments) noexcept {
    return {{arguments.collections, arguments.collections_stride},
            {arguments.count_limits, arguments.count_limits_stride},
            arguments.tasks_count};
}

/** The keys @p scans may return together, capped so a huge limit reserves nothing it may never fill. */
inline std::size_t expected_keys(scans_arg_t const& scans) noexcept {
    constexpr std::size_t reservation_cap_k = std::size_t {1} << 16;
    std::size_t expected = 0;
    for (std::size_t task = 0; task != scans.size() && expected < reservation_cap_k; ++task)
        expected += scans[task].limit;
    return std::min(expected, reservation_cap_k);
}

/** The view a call names, from the fields every view-taking argument struct shares. */
template <typename arguments_type_>
view_request_t view_of(arguments_type_ const& arguments) noexcept {
    return {arguments.transaction, arguments.snapshot, arguments.options};
}

#pragma endregion Argument Views

#pragma region Database Calls

/** Implements @c ustore_database_control through the table of known requests. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_database_control_t& arguments) noexcept {
    if (!arguments.request || !arguments.response) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need a request and an output for the response"};
        return;
    }
    *arguments.response = nullptr;
    std::string_view const request {arguments.request};
    for (control_request<isolation_> const& known : control_requests_k<isolation_>)
        if (known.name == request) return known.serve(state, arguments);
    *arguments.error = {ustore_error_invalid_argument_k,
                        "Unknown request: expected clear, reset, compact, info or usage"};
}

#pragma endregion Database Calls

#pragma region Snapshot Calls

/** Implements @c ustore_snapshot_list. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_snapshot_list_t& arguments) noexcept {
    if (!arguments.count) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the count"};
        return;
    }
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    std::size_t const capacity = state.snapshots.capacity();
    auto identifiers = arena.alloc_or_dummy(capacity, arguments.error, arguments.ids);
    if (arguments.error->status) return;
    *arguments.count = static_cast<ustore_size_t>(state.snapshots.list(identifiers, capacity));
}

/** Implements @c ustore_snapshot_create with a reader pinned at the newest published stamp. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_snapshot_create_t& arguments) noexcept {
    if (!arguments.id) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the snapshot"};
        return;
    }
    auto const opened = state.snapshots.open(state.store);
    if (reported_failure(opened.status(), arguments.error)) return;
    *arguments.id = *opened;
}

/** Implements @c ustore_snapshot_export as an image in the export directory. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_snapshot_export_t& arguments) noexcept {
    if (!arguments.path || !names_directory(arguments.path)) {
        *arguments.error = {ustore_error_not_found_k, "The export path isn't a directory"};
        return;
    }
    pinned_reader<isolation_> const* const reader = state.snapshots.find(arguments.id);
    if (!reader) {
        *arguments.error = {ustore_error_not_found_k, "The snapshot doesn't exist"};
        return;
    }
    write_image(snapshot_view<isolation_> {*reader}, arguments.path, arguments.error);
}

/** Implements @c ustore_snapshot_drop. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_snapshot_drop_t& arguments) noexcept {
    if (!state.snapshots.close(arguments.id))
        *arguments.error = {ustore_error_not_found_k, "The snapshot doesn't exist"};
}

#pragma endregion Snapshot Calls

#pragma region Blob Calls

/** Implements @c ustore_read, copying every value into the caller's arena once. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_read_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    places_arg_t const places = places_of(arguments);
    validate_read(arguments.transaction, places, arguments.options, arguments.error);
    if (arguments.error->status) return;
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    value_tape tape {arena, arguments};
    if (arguments.error->status) return;

    with_view(state, view_of(arguments), arguments.error, [&](auto const& view) noexcept {
        reported_failure(read_places(view, places, tape, arguments.error), arguments.error);
    });
    if (arguments.error->status) return;
    tape.finish();
}

/** Stages a write into the transaction it names. */
template <isolation_t isolation_>
void stage_into_transaction(ustore_write_t& arguments, places_arg_t const& places,
                            contents_arg_t const& contents) noexcept {
    transaction_handle<isolation_>* const handle = transaction_of<isolation_>(arguments.transaction, arguments.error);
    if (!handle) return;
    // A strictly serializable erase records the read of its key already, so only snapshot isolation adds a watch.
    watch_t const watch = isolation_ == isolation_t::snapshot_k &&
                                  !(arguments.options & ustore_option_transaction_dont_watch_k)
                              ? watch_t::watch_k
                              : watch_t::dont_watch_k;
    reported_failure(stage_writes(handle->transaction, places, contents, watch), arguments.error);
}

/** Implements @c ustore_write, committing a batch outside a transaction atomically. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_write_t& arguments) noexcept {
    places_arg_t const places = places_of(arguments);
    contents_arg_t const contents = contents_of(arguments);
    validate_write(arguments.transaction, places, contents, arguments.options, arguments.error);
    if (arguments.error->status || !arguments.tasks_count) return;
    if (touches_catalog(places)) {
        *arguments.error = {ustore_error_invalid_argument_k, "The catalog collection is reserved"};
        return;
    }
    if (arguments.transaction) return stage_into_transaction<isolation_>(arguments, places, contents);

    store_status_t const written =
        places.size() == 1
            ? write_directly(state.store, stored_key_t {places[0].collection, places[0].key}, contents[0])
            : commit_atomically(state.store, [&](store_transaction<isolation_>& transaction) noexcept {
                  return stage_writes(transaction, places, contents, watch_t::dont_watch_k);
              });
    if (reported_failure(written, arguments.error)) return;
    if (arguments.options & ustore_option_write_flush_k) persist(state, arguments.error);
}

/** Walks every scan of @p scans through @p view into @p tape. */
template <typename view_type_>
void scan_every(view_type_ const& view, scans_arg_t const& scans, key_tape& tape, ustore_error_t* error) noexcept {
    for (std::size_t task = 0; task != scans.size() && !error->status; ++task) {
        std::size_t const first = tape.size();
        if (reported_failure(scan_window(view, scans[task], tape), error)) return;
        tape.close_task(task, first);
    }
}

/** Implements @c ustore_scan as ordered walks of each collection's window. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_scan_t& arguments) noexcept {
    scans_arg_t const scans = scans_of(arguments);
    validate_scan(arguments.transaction, scans, arguments.options, arguments.error);
    if (arguments.error->status || !arguments.tasks_count) return;
    if (!arguments.keys) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the keys"};
        return;
    }
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    key_tape tape {arena, scans.size(), arguments.offsets, arguments.counts, arguments.error};
    tape.reserve(expected_keys(scans));
    if (arguments.error->status) return;

    with_view(state, view_of(arguments), arguments.error,
              [&](auto const& view) noexcept { scan_every(view, scans, tape, arguments.error); });
    if (arguments.error->status) return;
    tape.finish(arguments.keys);
}

/** Draws a reservoir for every sample of @p samples through @p view into @p tape. */
template <typename view_type_>
void sample_every(view_type_ const& view, sample_args_t const& samples, key_tape& tape,
                  ustore_error_t* error) noexcept {
    auto const now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    sample_generator_t generator {now ^ reinterpret_cast<std::uintptr_t>(&tape)};
    for (std::size_t task = 0; task != samples.size() && !error->status; ++task) {
        std::size_t const first = tape.size();
        if (reported_failure(sample_window(view, samples[task], generator, tape), error)) return;
        tape.close_task(task, first);
    }
}

/** Implements @c ustore_sample as a reservoir filled from a walk of each collection. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_sample_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.count_limits || !arguments.keys) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need count limits and an output for the keys"};
        return;
    }
    sample_args_t const samples = samples_of(arguments);
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    key_tape tape {arena, samples.size(), arguments.offsets, arguments.counts, arguments.error};
    if (arguments.error->status) return;

    with_view(state, view_of(arguments), arguments.error,
              [&](auto const& view) noexcept { sample_every(view, samples, tape, arguments.error); });
    if (arguments.error->status) return;
    tape.finish(arguments.keys);
}

/** The six columns a measurement exports. */
struct measure_columns_t {

    /** Lower bounds on the entries. */
    range_or_dummy<std::span<ustore_size_t>> min_cardinalities;

    /** Upper bounds on the entries. */
    range_or_dummy<std::span<ustore_size_t>> max_cardinalities;

    /** Lower bounds on the value bytes. */
    range_or_dummy<std::span<ustore_size_t>> min_value_bytes;

    /** Upper bounds on the value bytes. */
    range_or_dummy<std::span<ustore_size_t>> max_value_bytes;

    /** Lower bounds on the space. */
    range_or_dummy<std::span<ustore_size_t>> min_space_usages;

    /** Upper bounds on the space. */
    range_or_dummy<std::span<ustore_size_t>> max_space_usages;

    /** Records the exact measure of @p task. */
    void assign(std::size_t task, window_measure_t measured) noexcept {
        min_cardinalities[task] = measured.cardinality;
        max_cardinalities[task] = measured.cardinality;
        min_value_bytes[task] = measured.value_bytes;
        max_value_bytes[task] = measured.value_bytes;
        min_space_usages[task] = measured.value_bytes + measured.cardinality * sizeof(pair_t);
        max_space_usages[task] = std::numeric_limits<ustore_size_t>::max();
    }
};

/** The first key task @p task of a measurement covers. */
inline stored_key_t measured_lower(ustore_measure_t const& arguments, std::size_t task) noexcept {
    auto const index = static_cast<std::ptrdiff_t>(task);
    strided_iterator<ustore_collection_t const> const collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> const start_keys {arguments.start_keys, arguments.start_keys_stride};
    ustore_collection_t const collection = collections ? collections[index] : main_collection_k;
    return start_keys ? stored_key_t {collection, start_keys[index]} : stored_key_t::lowest_in(collection);
}

/** The last key task @p task of a measurement covers. */
inline stored_key_t measured_last(ustore_measure_t const& arguments, std::size_t task) noexcept {
    auto const index = static_cast<std::ptrdiff_t>(task);
    strided_iterator<ustore_collection_t const> const collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> const end_keys {arguments.end_keys, arguments.end_keys_stride};
    ustore_collection_t const collection = collections ? collections[index] : main_collection_k;
    return end_keys ? stored_key_t {collection, end_keys[index]} : stored_key_t::highest_in(collection);
}

/** Measures every task of @p arguments through @p view into @p columns. */
template <typename view_type_>
void measure_every(view_type_ const& view, ustore_measure_t& arguments, measure_columns_t& columns) noexcept {
    for (std::size_t task = 0; task != arguments.tasks_count; ++task) {
        window_measure_t measured;
        store_status_t const walked = measure_window(view, measured_lower(arguments, task),
                                                     measured_last(arguments, task), measured);
        if (reported_failure(walked, arguments.error)) return;
        columns.assign(task, measured);
    }
}

/** Implements @c ustore_measure, exact for entries and value bytes at the view it reads. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_measure_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    std::size_t const count = arguments.tasks_count;
    measure_columns_t columns {
        arena.alloc_or_dummy(count, arguments.error, arguments.min_cardinalities),
        arena.alloc_or_dummy(count, arguments.error, arguments.max_cardinalities),
        arena.alloc_or_dummy(count, arguments.error, arguments.min_value_bytes),
        arena.alloc_or_dummy(count, arguments.error, arguments.max_value_bytes),
        arena.alloc_or_dummy(count, arguments.error, arguments.min_space_usages),
        arena.alloc_or_dummy(count, arguments.error, arguments.max_space_usages),
    };
    if (arguments.error->status) return;
    with_view(state, view_of(arguments), arguments.error,
              [&](auto const& view) noexcept { measure_every(view, arguments, columns); });
}

#pragma endregion Blob Calls

#pragma region Collection Calls

/** Implements @c ustore_collection_list as a walk of the catalog. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_collection_list_t& arguments) noexcept {
    if (!arguments.count) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the count"};
        return;
    }
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    catalog_listing_t listing {{arena}};
    with_view(state, view_of(arguments), arguments.error, [&](auto const& view) noexcept {
        reported_failure(list_catalog(view, listing, arguments.error), arguments.error);
    });
    if (arguments.error->status) return;

    *arguments.count = static_cast<ustore_size_t>(listing.identifiers.size());
    if (arguments.ids) *arguments.ids = listing.identifiers.begin();
}

/** The error of a catalog change refused with @p status. */
constexpr ustore_error_t describe_catalog(store_status_t status) noexcept {
    if (status == store_status_t::key_already_exists_k)
        return {ustore_error_already_exists_k, "Such collection already exists"};
    if (status == store_status_t::key_not_found_k)
        return {ustore_error_not_found_k, "No collection has such identifier"};
    if (status == store_status_t::capacity_exhausted_k)
        return {ustore_error_capacity_exhausted_k, "Collection identifiers are exhausted"};
    return translate(status);
}

/** Implements @c ustore_collection_create as one commit of the entry and the advanced counter. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_collection_create_t& arguments) noexcept {
    if (!arguments.id) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the collection"};
        return;
    }
    ustore_collection_t created = main_collection_k;
    store_status_t const committed = commit_atomically(state.store,
                                                       [&](store_transaction<isolation_>& transaction) noexcept {
                                                           return stage_collection<isolation_>(transaction, created);
                                                       });
    *arguments.error = describe_catalog(committed);
    if (!arguments.error->status) *arguments.id = created;
}

/** Implements @c ustore_collection_drop as one commit over the collection's window and its catalog entry. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_collection_drop_t& arguments) noexcept {
    ustore_collection_t const collection = arguments.id;
    ustore_drop_mode_t const mode = arguments.mode;
    if (collection == catalog_collection_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "The catalog collection is reserved"};
        return;
    }
    if ((collection == main_collection_k || collection == names_collection_k) &&
        mode == ustore_drop_keys_vals_handle_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "This collection can only be cleared"};
        return;
    }
    if (mode != ustore_drop_keys_vals_handle_k && mode != ustore_drop_keys_vals_k && mode != ustore_drop_vals_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "Unknown drop mode"};
        return;
    }
    store_status_t const committed = commit_atomically(
        state.store,
        [&](store_transaction<isolation_>& transaction) noexcept { return stage_drop(transaction, collection, mode); });
    *arguments.error = describe_catalog(committed);
}

#pragma endregion Collection Calls

#pragma region Transaction Calls

/** Opens a transaction handle for @p arguments. */
template <isolation_t isolation_>
void open_transaction(database_state<isolation_>& state, ustore_transaction_init_t& arguments) noexcept {
    auto opened = state.store.transaction();
    if (reported_failure(opened.status(), arguments.error)) return;
    auto* const handle = new (std::nothrow) transaction_handle<isolation_>(std::move(*opened));
    if (!handle) {
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to allocate the transaction"};
        return;
    }
    *arguments.transaction = static_cast<transaction_header_t*>(handle);
}

/** Resets @p transaction onto a fresh snapshot for reuse. */
template <isolation_t isolation_>
void reset_transaction(ustore_transaction_t transaction, ustore_error_t* error) noexcept {
    transaction_handle<isolation_>* const handle = transaction_of<isolation_>(transaction, error);
    if (!handle) return;
    handle->staging = staging_t::pending_k;
    reported_failure(handle->transaction.reset(), error);
}

/** Implements @c ustore_transaction_init. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_transaction_init_t& arguments) noexcept {
    validate_transaction_begin(arguments.transaction, arguments.options, arguments.error);
    if (arguments.error->status) return;
    if (*arguments.transaction) return reset_transaction<isolation_>(*arguments.transaction, arguments.error);
    open_transaction(state, arguments);
}

/** Stages @p handle unless it already is, answering whether its writes sit staged. */
template <isolation_t isolation_>
bool staged(transaction_handle<isolation_>& handle, ustore_error_t* error) noexcept {
    if (handle.staging == staging_t::staged_k) return true;
    if (reported_failure(handle.transaction.stage(), error)) return false;
    handle.staging = staging_t::staged_k;
    return true;
}

/** Implements @c ustore_transaction_stage. */
template <isolation_t isolation_>
void serve(database_state<isolation_>&, ustore_transaction_stage_t& arguments) noexcept {
    if (!arguments.transaction) {
        *arguments.error = {ustore_error_uninitialized_k, "Transaction is uninitialized"};
        return;
    }
    transaction_handle<isolation_>* const handle = transaction_of<isolation_>(arguments.transaction, arguments.error);
    if (!handle || !staged(*handle, arguments.error)) return;
    if (arguments.sequence_number) *arguments.sequence_number = handle->transaction.generation();
}

/** Implements @c ustore_transaction_commit, staging first when the caller did not. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_transaction_commit_t& arguments) noexcept {
    validate_transaction_commit(arguments.transaction, arguments.options, arguments.error);
    if (arguments.error->status) return;
    transaction_handle<isolation_>* const handle = transaction_of<isolation_>(arguments.transaction, arguments.error);
    if (!handle || !staged(*handle, arguments.error)) return;
    if (reported_failure(handle->transaction.commit(), arguments.error)) return;
    handle->staging = staging_t::pending_k;
    if (arguments.sequence_number) *arguments.sequence_number = handle->transaction.commit_stamp();
    if (arguments.options & ustore_option_write_flush_k) persist(state, arguments.error);
}

#pragma endregion Transaction Calls

#pragma region Dispatch

/** Visits the isolation of the database @p arguments names once, and serves the call at it. */
template <typename arguments_type_>
void serve_database(arguments_type_& arguments) noexcept {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "The database is uninitialized"};
        return;
    }
    database_t& database = *static_cast<database_t*>(arguments.db);
    std::visit([&](auto& state) noexcept { serve(state, arguments); }, database.state);
}

/** One isolation a configuration may name. */
struct isolation_name_t {

    /** The name in the configuration, empty for the default. */
    std::string_view name;

    /** The isolation it names. */
    isolation_t isolation;
};

/** Every isolation a configuration may name. */
inline constexpr std::array<isolation_name_t, 3> isolation_names_k {{
    {"", isolation_t::strict_serializable_k},
    {"strict_serializable", isolation_t::strict_serializable_k},
    {"snapshot", isolation_t::snapshot_k},
}};

/** Reads the isolation @p name names into @p isolation, answering whether it names one. */
inline bool parse_isolation(std::string_view name, isolation_t& isolation) noexcept {
    for (isolation_name_t const& known : isolation_names_k) {
        if (known.name != name) continue;
        isolation = known.isolation;
        return true;
    }
    return false;
}

/** Loads the configuration @p text into @p config and @p isolation, refusing what the engine does not support. */
inline void load_configuration(char const* text, config_t& config, isolation_t& isolation,
                               ustore_error_t* error) noexcept {
    if (!text || text[0] == '\0') return;
    auto const loaded = load_config(text, config);
    if (loaded.failed()) {
        *error = loaded;
        return;
    }
    std::string_view requested;
    if (!read_json_member(config.document.root(), "isolation", requested))
        *error = {ustore_error_malformed_k, "The isolation must be a string"};
    else if (!parse_isolation(requested, isolation))
        *error = {ustore_error_invalid_argument_k, "Unknown isolation: expected strict_serializable or snapshot"};
    else if (!config.directory.empty() && !names_directory(config.directory.data()))
        *error = {ustore_error_not_found_k, "The directory doesn't exist"};
    else if (config.data_directories.size() != 0)
        *error = {ustore_error_unsupported_k, "Multiple data directories aren't supported"};
    else if (!config.engine.config_url.empty() || config.engine.config)
        *error = {ustore_error_unsupported_k, "Engine options aren't supported"};
}

/** Allocates a database at @p isolation_ and loads the image of @p directory, or answers null. */
template <isolation_t isolation_>
database_t* make_database(std::string_view directory, ustore_error_t* error) noexcept {
    std::unique_ptr<database_t> database {new (std::nothrow)
                                              database_t(std::in_place_type<database_state<isolation_>>)};
    if (!database) {
        *error = {ustore_error_out_of_memory_k, "Failed to allocate the database"};
        return nullptr;
    }
    open_database(*std::get_if<database_state<isolation_>>(&database->state), directory, error);
    return error->status ? nullptr : database.release();
}

#pragma endregion Dispatch

} // namespace unum::ustore::ram

using namespace unum::ustore;
using namespace unum::ustore::ram;

ustore_collection_t const ustore_collection_main_k = main_collection_k;
ustore_collection_t const ustore_collection_names_k = names_collection_k;
ustore_length_t const ustore_length_missing_k = std::numeric_limits<ustore_length_t>::max();
ustore_key_t const ustore_key_unknown_k = stored_key_t::unknown_key_k;

#pragma region C ABI

void ustore_database_init(ustore_database_init_t* arguments) {
    if (!arguments->db) {
        *arguments->error = {ustore_error_invalid_argument_k, "Need an output for the database"};
        return;
    }
    config_t config;
    isolation_t isolation = isolation_t::strict_serializable_k;
    load_configuration(arguments->config, config, isolation, arguments->error);
    if (arguments->error->status) return;
    *arguments->db = isolation == isolation_t::snapshot_k
                         ? make_database<isolation_t::snapshot_k>(config.directory, arguments->error)
                         : make_database<isolation_t::strict_serializable_k>(config.directory, arguments->error);
}

void ustore_get_metadata(ustore_get_metadata_t* arguments) {
    if (!arguments->db || !arguments->metadata) {
        *arguments->error = {ustore_error_invalid_argument_k, "Need a database and an output for the metadata"};
        return;
    }
    *arguments->metadata = ustore_metadata_t(ustore_supports_transactions_k | ustore_supports_collections_k |
                                             ustore_supports_snapshots_k);
}

void ustore_database_control(ustore_database_control_t* arguments) { serve_database(*arguments); }
void ustore_snapshot_list(ustore_snapshot_list_t* arguments) { serve_database(*arguments); }
void ustore_snapshot_create(ustore_snapshot_create_t* arguments) { serve_database(*arguments); }
void ustore_snapshot_export(ustore_snapshot_export_t* arguments) { serve_database(*arguments); }
void ustore_snapshot_drop(ustore_snapshot_drop_t* arguments) { serve_database(*arguments); }
void ustore_read(ustore_read_t* arguments) { serve_database(*arguments); }
void ustore_write(ustore_write_t* arguments) { serve_database(*arguments); }
void ustore_scan(ustore_scan_t* arguments) { serve_database(*arguments); }
void ustore_sample(ustore_sample_t* arguments) { serve_database(*arguments); }
void ustore_measure(ustore_measure_t* arguments) { serve_database(*arguments); }
void ustore_collection_list(ustore_collection_list_t* arguments) { serve_database(*arguments); }
void ustore_collection_create(ustore_collection_create_t* arguments) { serve_database(*arguments); }
void ustore_collection_drop(ustore_collection_drop_t* arguments) { serve_database(*arguments); }
void ustore_transaction_init(ustore_transaction_init_t* arguments) { serve_database(*arguments); }
void ustore_transaction_stage(ustore_transaction_stage_t* arguments) { serve_database(*arguments); }
void ustore_transaction_commit(ustore_transaction_commit_t* arguments) { serve_database(*arguments); }

void ustore_arena_free(ustore_arena_t arena) { clear_linked_memory(arena); }

void ustore_transaction_free(ustore_transaction_t transaction) {
    if (!transaction) return;
    auto* const header = static_cast<transaction_header_t*>(transaction);
    if (header->isolation == isolation_t::snapshot_k)
        return delete static_cast<transaction_handle<isolation_t::snapshot_k>*>(header);
    delete static_cast<transaction_handle<isolation_t::strict_serializable_k>*>(header);
}

void ustore_database_free(ustore_database_t database) {
    if (!database) return;
    std::unique_ptr<database_t> freed {static_cast<database_t*>(database)};
    std::visit([](auto& state) noexcept { close_database(state); }, freed->state);
}

#pragma endregion C ABI
