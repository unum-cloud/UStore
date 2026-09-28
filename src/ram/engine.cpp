/**
 *  @file src/ram/engine.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Embedded in-memory engine on SmashTable, strictly serializable or snapshot-isolated as configured at open.
 *
 *  Collections keyed by integers, their catalog and the counter numbering them share one partitioned multi-version
 *  store ordered by collection and key. Collections keyed by strings and the names of collections share a second one
 *  ordered by collection and string. Both are built into one commit order, so a transaction spanning them publishes
 *  under one stamp and a snapshot reads both at it. Every exported call visits the isolation once, resolves the one
 *  view it reads - the head, a snapshot or a transaction - and runs a template specialised for both. Snapshots are
 *  pinned readers in a lock-free slot directory and the catalog changes through store transactions, so the engine
 *  adds no lock of its own. A database with a directory persists as one binary image, rewritten whole on a flush and
 *  on close.
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
#include <chrono>      // `std::chrono::steady_clock`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::destroy_n`, `std::unique_ptr`
#include <new>         // `std::nothrow`, placement `new`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::remove_cvref_t`
#include <utility>     // `std::exchange`, `std::forward`, `std::move`
#include <variant>     // `std::in_place_type`, `std::variant`, `std::visit`

#include <fmt/format.h> // `fmt::format_to_n`
#include <forkunion/atomics.hpp>
#include <smashtable/basic_avl_tree.hpp>
#include <smashtable/basic_vector.hpp>
#include <smashtable/partitioned_store.hpp>
#include <smashtable/snapshot_store.hpp>
#include <stringzilla/stringzilla.hpp>

#include <ustore/blobs.h>
#include <ustore/db.h>
#include <ustore/ranges_args.hpp> // `places_arg_t`, `contents_arg_t`, `scans_arg_t`, `strings_arg_t`, `validate_read`
#include <ustore/strings.h>

#include "engine.hpp"                 // `engine_t`, `handle_t`, `handle_cast`
#include "helpers/blocking_files.hpp" // `directory_exists`
#include "helpers/linked_array.hpp"   // `growing_tape_t`, `uninitialized_array`
#include "helpers/linked_memory.hpp"  // `linked_memory_lock_t`
#include "ram/key.hpp"                // `stored_key_t`, `string_key_t`

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

/** A value filed under its key. */
using pair_t = st::mapping<stored_key_t, blob_t>;

/** The ordered core every partition keeps its versions in. */
using tree_t = st::basic_avl_tree<pair_t, stored_key_order_t, st::default_allocator<pair_t>>;

/** Every pair of a database at @p isolation_, in partitions that commit under one shared clock. */
template <isolation_t isolation_>
using ordered_store = st::partitioned_store<st::snapshot_store<tree_t, isolation_>, stored_key_hash_t>;

/** A string of a collection keyed by strings with its value, or a collection name with its record. */
using string_pair_t = st::mapping<string_key_t, blob_t>;

/** The ordered core every partition of the strings store keeps its versions in. */
using string_tree_t = st::basic_avl_tree<string_pair_t, string_key_order_t, st::default_allocator<string_pair_t>>;

/** Every string pair and every collection name of a database at @p isolation_, in the order of its other pairs. */
template <isolation_t isolation_>
using strings_store = st::partitioned_store<st::snapshot_store<string_tree_t, isolation_>, string_key_hash_t>;

/** The order both stores of a database commit in, so one stamp spans a commit reaching both. */
using order_t = ordered_store<isolation_t::snapshot_k>::order_t;

/** A view of every partition pinned at one stamp, recording nothing. */
template <isolation_t isolation_>
using pinned_reader = typename ordered_store<isolation_>::reader_t;

/** A transaction over every partition. */
template <isolation_t isolation_>
using store_transaction = typename ordered_store<isolation_>::transaction_t;

/** A transaction over both stores of a database, which publishes both under one stamp. */
template <isolation_t isolation_>
using stores_transaction = st::transaction_group<ordered_store<isolation_>, strings_store<isolation_>>;

static_assert(ordered_store<isolation_t::strict_serializable_k>::isolation_k == isolation_t::strict_serializable_k,
              "the partitions must keep the strict serializability of their stores");
static_assert(ordered_store<isolation_t::snapshot_k>::isolation_k == isolation_t::snapshot_k,
              "the partitions must keep the snapshot isolation of their stores");
static_assert(strings_store<isolation_t::strict_serializable_k>::isolation_k == isolation_t::strict_serializable_k,
              "the strings must keep the isolation of the pairs they commit with");
static_assert(stores_transaction<isolation_t::snapshot_k>::shares_one_order_k &&
                  stores_transaction<isolation_t::strict_serializable_k>::shares_one_order_k,
              "a transaction over both stores must publish them under one stamp");

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

/** The key of @p text in @p collection, owning a copy of its bytes. */
inline st::expected<string_key_t> string_key_of(ustore_collection_t collection, std::string_view text) noexcept {
    auto [copied, copy_blob_status] = copy_blob(value_view_t {text});
    if (st::failed(copy_blob_status)) return copy_blob_status;
    return string_key_t {collection, std::move(copied)};
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
        auto [claimed, claim_slot_status] = claim_slot_();
        if (st::failed(claim_slot_status)) return claim_slot_status;
        slot_t& slot = *slot_at_(claimed);
        new (&slot.reader) reader_t(store.reader());
        std::uint64_t const serial =
            fu::standard_atomic_ref<std::uint64_t>(creations_).fetch_add(1, st::memory_order_release_k) %
                (std::numeric_limits<std::uint64_t>::max() >> slot_index_bits_k) +
            1;
        std::uint64_t const identifier = (serial << slot_index_bits_k) | claimed;
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

    /** The order both stores commit in. */
    order_t order;

    /** Every pair of the collections keyed by integers, every catalog entry and the collection counter. */
    ordered_store<isolation_> store {order};

    /** Every pair of the collections keyed by strings and the record of every collection name. */
    strings_store<isolation_> strings {order};

    /** Every open snapshot. */
    snapshot_directory<isolation_> snapshots;

    /** Directory the image persists in, empty for a volatile database. */
    sz::string_t directory;
};

/** The calls this engine serves, which stamp every handle it hands out. */
extern engine_t const engine_k;

/** A database handle of the C ABI, holding one isolation chosen at open. */
struct database_t : handle_t {

    /** The database at the isolation its configuration named. */
    std::variant<database_state<isolation_t::strict_serializable_k>, database_state<isolation_t::snapshot_k>> state;

    template <isolation_t isolation_>
    explicit database_t(std::in_place_type_t<database_state<isolation_>> alternative) noexcept : state(alternative) {}
};

/** Leads every transaction handle, so a call tells a transaction of another isolation apart before using it. */
struct transaction_header_t : handle_t {

    /** The isolation of the database that opened the transaction. */
    isolation_t isolation;
};

/** A transaction handle of the C ABI. */
template <isolation_t isolation_>
struct transaction_handle : transaction_header_t {

    /** The transaction over both stores. */
    stores_transaction<isolation_> transaction;

    /** Whether the writes of the transaction are staged. */
    staging_t staging {staging_t::pending_k};

    explicit transaction_handle(stores_transaction<isolation_>&& opened) noexcept
        : transaction_header_t {{&engine_k}, isolation_}, transaction(std::move(opened)) {}

    /** The part of the transaction over the store keyed as @p keying_. */
    template <keying_t keying_>
    auto& part() noexcept {
        return transaction.template participant<keying_ == keying_t::strings_k ? 1 : 0>();
    }
};

/** The transaction behind @p transaction, or null after reporting one of another isolation. */
template <isolation_t isolation_>
transaction_handle<isolation_>* transaction_of(ustore_transaction_t transaction, ustore_error_t* error) noexcept {
    auto* const header = handle_cast<transaction_header_t>(transaction);
    if (header->isolation == isolation_) return static_cast<transaction_handle<isolation_>*>(header);
    *error = {ustore_error_invalid_argument_k, "The transaction belongs to a database of another isolation"};
    return nullptr;
}

#pragma endregion Handles

#pragma region Views

/** Reads what the newest published commit of @p store_type_ shows, recording nothing. */
template <typename store_type_>
struct head_view {

    /** Whether @c watch records anything. */
    static constexpr bool watches_k = false;

    /** The store being read. */
    store_type_ const& store;

    template <typename key_type_, typename found_type_, typename missing_type_>
    store_status_t find(key_type_ const& key, found_type_&& found, missing_type_&& missing) const noexcept {
        return store.find(key, found, missing);
    }

    template <typename key_type_, typename callback_type_>
    store_status_t range(key_type_ const& lower, key_type_ const& upper, callback_type_&& callback) const noexcept {
        return store.range(lower, upper, callback);
    }

    store_status_t prepare(std::size_t) const noexcept { return store_status_t::success_k; }

    template <typename key_type_>
    store_status_t watch(key_type_ const&) const noexcept {
        return store_status_t::success_k;
    }
};

/** Reads at the stamp a snapshot pinned, recording nothing. */
template <typename reader_type_>
struct snapshot_view {

    /** Whether @c watch records anything. */
    static constexpr bool watches_k = false;

    /** The reader of the snapshot. */
    reader_type_ const& reader;

    template <typename key_type_, typename found_type_, typename missing_type_>
    store_status_t find(key_type_ const& key, found_type_&& found, missing_type_&& missing) const noexcept {
        return reader.find(key, found, missing);
    }

    template <typename key_type_, typename callback_type_>
    store_status_t range(key_type_ const& lower, key_type_ const& upper, callback_type_&& callback) const noexcept {
        return reader.range(lower, upper, callback);
    }

    store_status_t prepare(std::size_t) const noexcept { return store_status_t::success_k; }

    template <typename key_type_>
    store_status_t watch(key_type_ const&) const noexcept {
        return store_status_t::success_k;
    }
};

/** Reads at the snapshot of a transaction, which records what it read from @c serializable_k up. */
template <typename transaction_type_>
struct transaction_view {

    /** Whether @c watch records anything. */
    static constexpr bool watches_k = false;

    /** The transaction being read through. */
    transaction_type_& transaction;

    template <typename key_type_, typename found_type_, typename missing_type_>
    store_status_t find(key_type_ const& key, found_type_&& found, missing_type_&& missing) const noexcept {
        return transaction.find(key, found, missing);
    }

    template <typename key_type_, typename callback_type_>
    store_status_t range(key_type_ const& lower, key_type_ const& upper, callback_type_&& callback) const noexcept {
        return transaction.range(lower, upper, callback);
    }

    store_status_t prepare(std::size_t count) const noexcept { return transaction.reserve(count); }

    template <typename key_type_>
    store_status_t watch(key_type_ const&) const noexcept {
        return store_status_t::success_k;
    }
};

/** Reads through a snapshot-isolated transaction, watching every key it hands out. */
template <typename transaction_type_>
struct watching_view {

    /** Whether @c watch records anything. */
    static constexpr bool watches_k = true;

    /** The transaction being read through. */
    transaction_type_& transaction;

    template <typename key_type_, typename found_type_, typename missing_type_>
    store_status_t find(key_type_ const& key, found_type_&& found, missing_type_&& missing) const noexcept {
        return transaction.find_and_watch(key, found, missing);
    }

    template <typename key_type_, typename callback_type_>
    store_status_t range(key_type_ const& lower, key_type_ const& upper, callback_type_&& callback) const noexcept {
        return transaction.range(lower, upper, callback);
    }

    store_status_t prepare(std::size_t count) const noexcept { return transaction.reserve(count); }

    template <typename key_type_>
    store_status_t watch(key_type_ const& key) const noexcept {
        return transaction.watch(key);
    }
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

/** The store of @p state keyed as @p keying_. */
template <keying_t keying_, isolation_t isolation_>
auto& store_of(database_state<isolation_>& state) noexcept {
    if constexpr (keying_ == keying_t::strings_k) return state.strings;
    else return state.store;
}

/** Hands @p action the view of the store keyed as @p keying_ that @p request names, or reports why none exists. */
template <keying_t keying_ = keying_t::integers_k, isolation_t isolation_, typename action_type_>
void with_view(database_state<isolation_>& state, view_request_t const& request, ustore_error_t* error,
               action_type_&& action) noexcept {
    if (request.snapshot) {
        pinned_reader<isolation_> const* const reader = state.snapshots.find(request.snapshot);
        if (!reader) {
            *error = {ustore_error_not_found_k, "The snapshot doesn't exist"};
            return;
        }
        if constexpr (keying_ == keying_t::strings_k) {
            // The directory's reader holds the claim, so the strings read at its stamp without one of their own
            auto const strings = state.strings.reader_at(reader->snapshot());
            return action(snapshot_view {strings});
        }
        else return action(snapshot_view {*reader});
    }
    if (!request.transaction) return action(head_view {store_of<keying_>(state)});

    transaction_handle<isolation_>* const handle = transaction_of<isolation_>(request.transaction, error);
    if (!handle) return;
    bool const watches = isolation_ == isolation_t::snapshot_k &&
                         !(request.options & ustore_option_transaction_dont_watch_k);
    if (watches) return action(watching_view {handle->template part<keying_>()});
    action(transaction_view {handle->template part<keying_>()});
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
    template <typename arguments_type_>
    value_tape(linked_memory_lock_t& arena, arguments_type_& arguments) noexcept
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

/** What the collection a catalog entry lists holds, which the entry's value leads with. */
inline ustore_modality_t modality_of_entry(blob_t const& entry) noexcept {
    return entry.size() == 0 ? ustore_modality_any_k : static_cast<ustore_modality_t>(entry[0]);
}

/** The name the collection a catalog entry lists carries, which follows its modality and is empty for none. */
inline value_view_t name_of_entry(blob_t const& entry) noexcept {
    if (entry.size() <= 1) return {};
    return value_view_t {entry.data() + 1, static_cast<ustore_length_t>(entry.size() - 1)};
}

/** The collections a listing exports, with what each holds and the name it carries. */
struct catalog_listing_t {

    /** The identifier of every collection. */
    uninitialized_array<ustore_collection_t> identifiers;

    /** What every collection holds. */
    uninitialized_array<ustore_modality_t> modalities;

    /** The name of every collection, empty for one without. */
    growing_tape_t names;
};

/** Appends the catalog entry @p pair to @p listing, halting the walk when the arena refuses. */
inline walk_control_t list_entry(catalog_listing_t& listing, pair_t const& pair, ustore_error_t* error) noexcept {
    listing.identifiers.push_back(pair.key.cataloged(), error);
    listing.modalities.push_back(modality_of_entry(pair.mapped), error);
    listing.names.push_back(name_of_entry(pair.mapped), error);
    return error->status ? walk_control_t::halt_k : walk_control_t::resume_k;
}

/** Walks the catalog of @p view into @p listing. */
template <typename view_type_>
store_status_t list_catalog(view_type_ const& view, catalog_listing_t& listing, ustore_error_t* error) noexcept {
    return view.range(stored_key_t::lowest_in(catalog_collection_k), stored_key_t::collection_counter(),
                      [&](pair_t const& pair) noexcept { return list_entry(listing, pair, error); });
}

/** Reads the value of every string of @p texts through @p view into @p tape. */
template <typename view_type_>
store_status_t read_strings(view_type_ const& view, strided_iterator<ustore_collection_t const> collections,
                            strings_arg_t const& texts, std::size_t count, value_tape& tape,
                            ustore_error_t* error) noexcept {
    store_status_t const reserved = view.prepare(count);
    if (st::failed(reserved)) return reserved;
    auto const found = [&](string_pair_t const& pair) noexcept { tape.append(pair.mapped); };
    auto const missing = [&]() noexcept { tape.append_missing(); };
    for (std::size_t index = 0; index != count && !error->status; ++index) {
        ustore_collection_t const collection = collections ? collections[index] : main_collection_k;
        auto [key, key_status] = string_key_of(collection, texts[index]);
        if (st::failed(key_status)) return key_status;
        store_status_t const looked_up = view.find(key, found, missing);
        if (st::failed(looked_up)) return looked_up;
    }
    return store_status_t::success_k;
}

/** Appends up to @p limit strings of @p collection starting with @p prefix, from @p start on, to @p found. */
template <typename view_type_>
store_status_t scan_strings(view_type_ const& view, ustore_collection_t collection, std::string_view prefix,
                            std::string_view start, std::size_t limit, growing_tape_t& found,
                            ustore_error_t* error) noexcept {
    if (limit == 0) return store_status_t::success_k;
    auto [first, first_status] = string_key_of(collection, start > prefix ? start : prefix);
    if (st::failed(first_status)) return first_status;
    std::size_t const found_before = found.lengths().size();
    store_status_t const walked = view.range(
        first, string_key_t::past(collection), [&](string_pair_t const& pair) noexcept -> walk_control_t {
            std::string_view const text = pair.key.view();
            if (!text.starts_with(prefix)) return walk_control_t::halt_k;
            found.push_back(value_view_t {text}, error);
            if (error->status) return walk_control_t::halt_k;
            return found.lengths().size() - found_before == limit ? walk_control_t::halt_k : walk_control_t::resume_k;
        });
    if (st::failed(walked) || !view_type_::watches_k) return walked;

    // A walk holds its partitions, so the strings it handed out are watched once it let go of them
    auto const offsets = found.offsets();
    auto const lengths = found.lengths();
    char const* const tape = reinterpret_cast<char const*>(found.contents().begin().get());
    for (std::size_t index = found_before; index != lengths.size() && !error->status; ++index) {
        std::string_view const text {tape + offsets[index], lengths[index]};
        auto [key, key_status] = string_key_of(collection, text);
        store_status_t const watched = st::failed(key_status) ? key_status : view.watch(key);
        if (st::failed(watched)) return watched;
    }
    return store_status_t::success_k;
}

#pragma endregion Reads

#pragma region Writes

/** Stages @p content under @p key into @p transaction, which may be a store publishing it at once. */
template <typename transaction_type_, typename key_type_>
store_status_t stage_upsert(transaction_type_& transaction, key_type_&& key, value_view_t content) noexcept {
    auto [copied, copy_blob_status] = copy_blob(content);
    if (st::failed(copy_blob_status)) return copy_blob_status;
    using staged_pair_t = st::mapping<std::remove_cvref_t<key_type_>, blob_t>;
    return transaction.upsert(staged_pair_t {std::forward<key_type_>(key), std::move(copied)});
}

/** Stages erasing @p key, watching its absence when @p watch asks and the key is already gone. */
template <typename transaction_type_, typename key_type_>
store_status_t stage_erase(transaction_type_& transaction, key_type_ const& key, watch_t watch) noexcept {
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

/** Stages a batch of strings into @p transaction, where a missing value erases its string. */
template <typename transaction_type_>
store_status_t stage_strings(transaction_type_& transaction, strided_iterator<ustore_collection_t const> collections,
                             strings_arg_t const& texts, contents_arg_t const& contents, watch_t watch) noexcept {
    for (std::size_t index = 0; index != contents.size(); ++index) {
        ustore_collection_t const collection = collections ? collections[index] : main_collection_k;
        auto [key, key_status] = string_key_of(collection, texts[index]);
        if (st::failed(key_status)) return key_status;
        value_view_t const content = contents[index];
        store_status_t const staged = content ? stage_upsert(transaction, std::move(key), content)
                                              : stage_erase(transaction, key, watch);
        if (st::failed(staged)) return staged;
    }
    return store_status_t::success_k;
}

/** Publishes one write into @p store under a stamp of its own, with no transaction to open. */
template <typename store_type_, typename key_type_>
store_status_t write_directly(store_type_& store, key_type_&& key, value_view_t content) noexcept {
    if (content) return stage_upsert(store, std::forward<key_type_>(key), content);
    store_status_t const erased = store.erase(key);
    return erased == store_status_t::key_not_found_k ? store_status_t::success_k : erased;
}

/** Commits @p stage_changes in a transaction of its own, retrying conflicts up to @c commit_attempts_k times. */
template <typename store_type_, typename stage_changes_type_>
store_status_t commit_atomically(store_type_& store, stage_changes_type_&& stage_changes) noexcept {
    auto opened = store.transaction();
    if (!opened) return opened.status();
    return st::commit_with_retries(*opened, commit_attempts_k, stage_changes);
}

/** Commits @p stage_changes over both stores of @p state under one stamp, retrying conflicts up to
 *  @c commit_attempts_k times. */
template <isolation_t isolation_, typename stage_changes_type_>
store_status_t commit_together(database_state<isolation_>& state, stage_changes_type_&& stage_changes) noexcept {
    for (std::size_t attempt = 0; attempt != commit_attempts_k; ++attempt) {
        auto opened = st::make_transaction_group(state.store, state.strings);
        if (!opened) return opened.status();
        store_status_t const committed = opened->commit_with(stage_changes);
        if (!st::conflicted(committed)) return committed;
    }
    return store_status_t::operation_would_block_k;
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

/**
 *  @brief Stages a new collection holding @p modality under @p name and the advanced counter, answering its
 *      identifier through @p created.
 *
 *  The catalog entry's value is `struct { uint8_t modality; char name[]; }`, the name empty for a collection without
 *  one, so a listing and a drop reach the name without walking the names.
 */
template <isolation_t isolation_>
store_status_t stage_collection(store_transaction<isolation_>& transaction, ustore_modality_t modality,
                                std::string_view name, ustore_collection_t& created) noexcept {
    store_status_t const counted = read_counter(transaction, created);
    if (st::failed(counted)) return counted;
    if (created >= collections_limit_k) return store_status_t::capacity_exhausted_k;
    store_status_t const advanced = stage_counter(transaction, created + 1);
    if (st::failed(advanced)) return advanced;
    blob_t entry;
    if (st::failed(entry.resize(1 + name.size()))) return store_status_t::out_of_memory_heap_k;
    entry[0] = static_cast<ustore_byte_t>(modality);
    if (!name.empty()) std::memcpy(entry.data() + 1, name.data(), name.size());
    return transaction.upsert(pair_t {stored_key_t::catalog_entry(created), std::move(entry)});
}

/** Reads what @p collection holds through @p reader, answering @c key_not_found_k for a collection the catalog does
 *  not list. */
template <typename reader_type_>
store_status_t read_modality(reader_type_& reader, ustore_collection_t collection,
                             ustore_modality_t& modality) noexcept {
    modality = ustore_modality_any_k;
    if (collection == main_collection_k) return store_status_t::success_k;
    store_status_t listing = store_status_t::key_not_found_k;
    store_status_t const looked_up = reader.find(
        stored_key_t::catalog_entry(collection),
        [&](pair_t const& entry) noexcept {
            listing = store_status_t::success_k;
            modality = modality_of_entry(entry.mapped);
        },
        st::no_op_t {});
    return st::failed(looked_up) ? looked_up : listing;
}

/** Refuses @p collection unless the catalog lists it keyed as @p keying, answering whether it refused. */
template <isolation_t isolation_>
bool refuse_collection(database_state<isolation_>& state, ustore_collection_t collection, keying_t keying,
                       ustore_error_t* error) noexcept {
    ustore_modality_t modality = ustore_modality_any_k;
    store_status_t const listed = read_modality(state.store, collection, modality);
    if (listed == store_status_t::key_not_found_k) {
        *error = {ustore_error_not_found_k, "No such collection"};
        return true;
    }
    if (reported_failure(listed, error)) return true;
    if (keying_of(modality) == keying) return false;
    *error = {ustore_error_invalid_argument_k,
              keying == keying_t::strings_k ? "The collection holds no strings" : "The collection holds strings"};
    return true;
}

/** Refuses a batch addressing a collection the catalog does not list keyed as @p keying, checking each run of tasks
 *  on one collection once. */
template <isolation_t isolation_>
bool refuse_collections(database_state<isolation_>& state, strided_iterator<ustore_collection_t const> collections,
                        std::size_t count, keying_t keying, ustore_error_t* error) noexcept {
    if (!collections) return count && refuse_collection(state, main_collection_k, keying, error);
    for (std::size_t task = 0; task != count; ++task) {
        bool const repeated = task != 0 && collections[task] == collections[task - 1];
        if (!repeated && refuse_collection(state, collections[task], keying, error)) return true;
    }
    return false;
}

/** Empties a value in place, keeping its key. */
struct empty_value_t {
    template <typename key_type_>
    void operator()(key_type_ const&, blob_t& value) const noexcept {
        [[maybe_unused]] store_status_t const emptied = value.resize(0);
    }
};

/** Stages clearing [ @p lower, @p upper ) as @p mode asks: emptying every value, or erasing every key. */
template <typename transaction_type_, typename key_type_>
store_status_t stage_clear(transaction_type_& transaction, key_type_ const& lower, key_type_ const& upper,
                           ustore_drop_mode_t mode) noexcept {
    if (mode == ustore_drop_vals_k) return transaction.update_range(lower, upper, empty_value_t {});
    return transaction.erase_range(lower, upper, st::no_op_t {});
}

/** Stages what dropping @p collection in @p mode removes from both stores: its pairs, and with the handle its catalog
 *  entry and the name the entry records. */
template <typename pairs_type_, typename strings_type_>
store_status_t stage_drop(pairs_type_& pairs, strings_type_& strings, ustore_collection_t collection,
                          ustore_drop_mode_t mode) noexcept {
    ustore_modality_t modality = ustore_modality_any_k;
    string_key_t name = string_key_t::lowest_in(names_collection_k);
    store_status_t listing = collection == main_collection_k ? store_status_t::success_k
                                                             : store_status_t::key_not_found_k;
    store_status_t const looked_up = pairs.find(
        stored_key_t::catalog_entry(collection),
        [&](pair_t const& entry) noexcept {
            modality = modality_of_entry(entry.mapped);
            auto [text, copy_blob_status] = copy_blob(name_of_entry(entry.mapped));
            listing = copy_blob_status;
            name.text = std::move(text);
        },
        st::no_op_t {});
    if (st::failed(looked_up)) return looked_up;
    if (st::failed(listing)) return listing;

    store_status_t const cleared =
        keying_of(modality) == keying_t::strings_k
            ? stage_clear(strings, string_key_t::lowest_in(collection), string_key_t::past(collection), mode)
            : stage_clear(pairs, stored_key_t::lowest_in(collection), stored_key_t::past(collection), mode);
    if (st::failed(cleared) || mode != ustore_drop_keys_vals_handle_k) return cleared;
    store_status_t const unlisted = pairs.erase(stored_key_t::catalog_entry(collection));
    if (st::failed(unlisted) || name.text.size() == 0) return unlisted;
    return strings.erase(name);
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
inline bool join_path(sz::string_t& path, char const* directory, std::string_view name) noexcept {
    return path.try_assign(sz::string_view_t {directory}) && path.try_append(name.data(), name.size());
}

/** Writes @p blob as its length and its bytes, answering whether all of it reached @p file. */
inline bool write_blob(std::FILE* file, blob_t const& blob) noexcept {
    auto const length = static_cast<ustore_length_t>(blob.size());
    return std::fwrite(&length, sizeof(length), 1, file) == 1 &&
           (length == 0 || std::fwrite(blob.data(), 1, length, file) == length);
}

/** Writes one pair as its key and its value, answering whether all of it reached @p file. */
inline bool write_pair(std::FILE* file, pair_t const& pair) noexcept {
    return std::fwrite(&pair.key, sizeof(stored_key_t), 1, file) == 1 && write_blob(file, pair.mapped);
}

/** Writes one string pair as its collection, its string and its value, answering whether all of it reached
 *  @p file. */
inline bool write_string_pair(std::FILE* file, string_pair_t const& pair) noexcept {
    return std::fwrite(&pair.key.collection, sizeof(ustore_collection_t), 1, file) == 1 &&
           write_blob(file, pair.key.text) && write_blob(file, pair.mapped);
}

/**
 *  @brief Writes the image of @p directory from @p pairs and @p strings, which read at one stamp.
 *
 *  A signature and the key width lead, then every pair, then @c stored_key_t::highest, which no pair is stored
 *  under, and then every string pair up to the end of the file.
 */
template <typename pairs_view_type_, typename strings_view_type_>
void write_image(pairs_view_type_ const& pairs, strings_view_type_ const& strings, char const* directory,
                 ustore_error_t* error) noexcept {
    sz::string_t partial_path;
    sz::string_t final_path;
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
    store_status_t const walked = pairs.range(stored_key_t::lowest(), stored_key_t::highest(),
                                              [&](pair_t const& pair) noexcept -> walk_control_t {
                                                  written = written && write_pair(stream.get(), pair);
                                                  return written ? walk_control_t::resume_k : walk_control_t::halt_k;
                                              });
    if (reported_failure(walked, error)) return;
    stored_key_t const sentinel = stored_key_t::highest();
    written = written && std::fwrite(&sentinel, sizeof(stored_key_t), 1, stream.get()) == 1;
    store_status_t const walked_strings = strings.range(
        string_key_t::lowest_in(main_collection_k), string_key_t::lowest_in(catalog_collection_k),
        [&](string_pair_t const& pair) noexcept -> walk_control_t {
            written = written && write_string_pair(stream.get(), pair);
            return written ? walk_control_t::resume_k : walk_control_t::halt_k;
        });
    if (reported_failure(walked_strings, error)) return;

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

/** Reads a length and that many bytes of @p file into @p blob. */
inline image_step_t load_blob(std::FILE* file, blob_t& blob) noexcept {
    ustore_length_t length = 0;
    if (std::fread(&length, sizeof(length), 1, file) != 1) return image_step_t::truncated_k;
    if (st::failed(blob.resize(length))) return image_step_t::out_of_memory_k;
    if (length != 0 && std::fread(blob.data(), 1, length, file) != length) return image_step_t::truncated_k;
    return image_step_t::loaded_k;
}

/** Loads the next pair of @p file into @p store, finishing at the sentinel that closes the pairs. */
template <isolation_t isolation_>
image_step_t load_pair(std::FILE* file, ordered_store<isolation_>& store) noexcept {
    stored_key_t key;
    if (std::fread(&key, sizeof(stored_key_t), 1, file) != 1) return image_step_t::finished_k;
    if (!stored_key_order_t {}(key, stored_key_t::highest())) return image_step_t::finished_k;
    blob_t blob;
    if (image_step_t const loaded = load_blob(file, blob); loaded != image_step_t::loaded_k) return loaded;
    if (st::failed(store.upsert(pair_t {key, std::move(blob)}))) return image_step_t::refused_k;
    return image_step_t::loaded_k;
}

/** Loads the next string pair of @p file into @p store. */
template <isolation_t isolation_>
image_step_t load_string_pair(std::FILE* file, strings_store<isolation_>& store) noexcept {
    string_key_t key;
    if (std::fread(&key.collection, sizeof(ustore_collection_t), 1, file) != 1) return image_step_t::finished_k;
    if (image_step_t const loaded = load_blob(file, key.text); loaded != image_step_t::loaded_k) return loaded;
    blob_t blob;
    if (image_step_t const loaded = load_blob(file, blob); loaded != image_step_t::loaded_k) return loaded;
    if (st::failed(store.upsert(string_pair_t {std::move(key), std::move(blob)}))) return image_step_t::refused_k;
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

/** Loads the image of the directory of @p state into its stores, leaving them empty when there is none. */
template <isolation_t isolation_>
void read_image(database_state<isolation_>& state, ustore_error_t* error) noexcept {
    sz::string_t path;
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
    if (step == image_step_t::finished_k) do
            step = load_string_pair(stream.get(), state.strings);
        while (step == image_step_t::loaded_k);
    *error = image_step_errors_k[static_cast<std::size_t>(step)];
}

/** Rewrites the image of @p state from its newest published commit, when it has a directory. */
template <isolation_t isolation_>
void persist(database_state<isolation_>& state, ustore_error_t* error) noexcept {
    if (state.directory.empty()) return;
    pinned_reader<isolation_> const reader = state.store.reader();
    auto const strings = state.strings.reader_at(reader.snapshot());
    write_image(snapshot_view {reader}, snapshot_view {strings}, state.directory.c_str(), error);
}

#pragma endregion Image

#pragma region Lifetime

/** Remembers the directory of @p state and loads its image. */
template <isolation_t isolation_>
void open_database(database_state<isolation_>& state, std::string_view directory, ustore_error_t* error) noexcept {
    if (directory.empty()) return;
    if (!state.directory.try_assign(sz::string_view_t {directory})) {
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

/** Serves "clear", removing every pair and keeping the catalog and the names. */
template <isolation_t isolation_>
void clear_pairs(database_state<isolation_>& state, ustore_database_control_t& arguments) noexcept {
    store_status_t const cleared = commit_together(state, [](auto& pairs, auto& strings) noexcept {
        store_status_t const erased = pairs.erase_up_to(stored_key_t::lowest_in(catalog_collection_k), st::no_op_t {});
        if (st::failed(erased)) return erased;
        return strings.erase_up_to(string_key_t::lowest_in(names_collection_k), st::no_op_t {});
    });
    reported_failure(cleared, arguments.error);
}

/** Serves "reset", removing every snapshot, pair, collection and name. */
template <isolation_t isolation_>
void reset_database(database_state<isolation_>& state, ustore_database_control_t& arguments) noexcept {
    state.snapshots.close_every();
    store_status_t const reset = commit_together(state, [](auto& pairs, auto& strings) noexcept {
        store_status_t const erased = pairs.erase_from(stored_key_t::lowest(), st::no_op_t {});
        if (st::failed(erased)) return erased;
        return strings.erase_from(string_key_t::lowest_in(main_collection_k), st::no_op_t {});
    });
    reported_failure(reset, arguments.error);
}

/** Serves "compact", reclaiming unreachable versions of both stores and rewriting the image. */
template <isolation_t isolation_>
void compact_database(database_state<isolation_>& state, ustore_database_control_t& arguments) noexcept {
    auto [vacuumed, vacuum_status] = state.store.vacuum();
    if (reported_failure(vacuum_status, arguments.error)) return;
    auto [vacuumed_strings, vacuum_strings_status] = state.strings.vacuum();
    if (reported_failure(vacuum_strings_status, arguments.error)) return;
    persist(state, arguments.error);
}

/** Serves "info", naming the engine and its isolation. */
template <isolation_t isolation_>
void describe_database(database_state<isolation_>&, ustore_database_control_t& arguments) noexcept {
    *arguments.response = isolation_ == isolation_t::strict_serializable_k
                              ? R"({"engine":"ram","isolation":"strict_serializable"})"
                              : R"({"engine":"ram","isolation":"snapshot"})";
}

/** Serves "usage", counting keys and versions into the arena. */
template <isolation_t isolation_>
void report_usage(database_state<isolation_>& state, ustore_database_control_t& arguments) noexcept {
    linked_memory_lock_t arena = linked_memory(arguments.arena, ustore_options_default_k, arguments.error);
    if (arguments.error->status) return;
    constexpr std::size_t response_capacity_k = 96;
    char* const response = arena.alloc<char>(response_capacity_k, arguments.error).data();
    if (arguments.error->status) return;
    auto const written = fmt::format_to_n(response, response_capacity_k - 1, R"({{"keys":{},"versions":{}}})",
                                          state.store.size() + state.strings.size(),
                                          state.store.versions_count() + state.strings.versions_count());
    *written.out = '\0';
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
    auto [opened, open_status] = state.snapshots.open(state.store);
    if (reported_failure(open_status, arguments.error)) return;
    *arguments.id = opened;
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
    auto const strings = state.strings.reader_at(reader->snapshot());
    write_image(snapshot_view {*reader}, snapshot_view {strings}, arguments.path, arguments.error);
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
    strided_iterator<ustore_collection_t const> const collections {arguments.collections, arguments.collections_stride};
    if (refuse_collections(state, collections, places.size(), keying_t::integers_k, arguments.error)) return;
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
    reported_failure(stage_writes(handle->template part<keying_t::integers_k>(), places, contents, watch),
                     arguments.error);
}

/** Implements @c ustore_write, committing a batch outside a transaction atomically. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_write_t& arguments) noexcept {
    places_arg_t const places = places_of(arguments);
    contents_arg_t const contents = contents_of(arguments);
    validate_write(arguments.transaction, places, contents, arguments.options, arguments.error);
    if (arguments.error->status || !arguments.tasks_count) return;
    strided_iterator<ustore_collection_t const> const collections {arguments.collections, arguments.collections_stride};
    if (refuse_collections(state, collections, places.size(), keying_t::integers_k, arguments.error)) return;
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
    strided_iterator<ustore_collection_t const> const collections {arguments.collections, arguments.collections_stride};
    if (refuse_collections(state, collections, scans.size(), keying_t::integers_k, arguments.error)) return;
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
    strided_iterator<ustore_collection_t const> const collections {arguments.collections, arguments.collections_stride};
    if (refuse_collections(state, collections, samples.size(), keying_t::integers_k, arguments.error)) return;
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
    strided_iterator<ustore_collection_t const> const collections {arguments.collections, arguments.collections_stride};
    if (refuse_collections(state, collections, arguments.tasks_count, keying_t::integers_k, arguments.error)) return;
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

/** Implements @c ustore_collection_list as a walk of the catalog, whose entries carry the modalities and the names. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_collection_list_t& arguments) noexcept {
    if (!arguments.count) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the count"};
        return;
    }
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    catalog_listing_t listing {{arena}, {arena}, {arena}};
    with_view(state, view_of(arguments), arguments.error, [&](auto const& view) noexcept {
        reported_failure(list_catalog(view, listing, arguments.error), arguments.error);
    });
    if (arguments.error->status) return;

    *arguments.count = static_cast<ustore_size_t>(listing.identifiers.size());
    if (arguments.ids) *arguments.ids = listing.identifiers.begin();
    if (arguments.modalities) *arguments.modalities = listing.modalities.begin();
    if (arguments.names_offsets) *arguments.names_offsets = listing.names.offsets().begin().get();
    if (arguments.names) *arguments.names = reinterpret_cast<ustore_char_t*>(listing.names.contents().begin().get());
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

/** Stages a collection named @p name, unless another collection carries the name, and records the name. */
template <isolation_t isolation_, typename strings_type_>
store_status_t stage_named_collection(store_transaction<isolation_>& pairs, strings_type_& strings,
                                      ustore_modality_t modality, std::string_view name,
                                      ustore_collection_t& created) noexcept {
    auto [key, key_status] = string_key_of(names_collection_k, name);
    if (st::failed(key_status)) return key_status;
    bool taken = false;
    store_status_t const looked_up = strings.find(
        key, [&](string_pair_t const&) noexcept { taken = true; }, st::no_op_t {});
    if (st::failed(looked_up)) return looked_up;
    if (taken) return store_status_t::key_already_exists_k;
    store_status_t const staged = stage_collection<isolation_>(pairs, modality, name, created);
    if (st::failed(staged)) return staged;
    name_record_t const record {created, modality};
    return stage_upsert(strings, std::move(key), value_view_t {record.view()});
}

/** Implements @c ustore_collection_create as one commit of the entry, the advanced counter and the name, a named
 *  collection persisted at once as a flushed write would be. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_collection_create_t& arguments) noexcept {
    if (!arguments.id) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the collection"};
        return;
    }
    if (arguments.modality > ustore_modality_pages_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "Unknown modality"};
        return;
    }
    std::string_view const name = string_of(arguments.name, arguments.name_length);
    if (arguments.name && name.empty()) {
        *arguments.error = {ustore_error_invalid_argument_k, "A name is never empty, as the main collection has none"};
        return;
    }

    // Every creation advances the counter, so two creating the same name conflict and the retry finds it taken
    ustore_collection_t created = main_collection_k;
    store_status_t const committed =
        name.empty()
            ? commit_atomically(state.store,
                                [&](store_transaction<isolation_>& pairs) noexcept {
                                    return stage_collection<isolation_>(pairs, arguments.modality, name, created);
                                })
            : commit_together(state, [&](store_transaction<isolation_>& pairs, auto& strings) noexcept {
                  return stage_named_collection<isolation_>(pairs, strings, arguments.modality, name, created);
              });
    if (committed == store_status_t::key_already_exists_k) {
        *arguments.error = {ustore_error_already_exists_k, "A collection already carries the name"};
        return;
    }
    *arguments.error = describe_catalog(committed);
    if (arguments.error->status) return;
    *arguments.id = created;
    if (!name.empty()) persist(state, arguments.error);
}

/** Implements @c ustore_collection_drop as one commit over the collection's window, its catalog entry and its
 *  names. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_collection_drop_t& arguments) noexcept {
    ustore_collection_t const collection = arguments.id;
    ustore_drop_mode_t const mode = arguments.mode;
    if (collection == main_collection_k && mode == ustore_drop_keys_vals_handle_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "The main collection can only be cleared"};
        return;
    }
    if (mode != ustore_drop_keys_vals_handle_k && mode != ustore_drop_keys_vals_k && mode != ustore_drop_vals_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "Unknown drop mode"};
        return;
    }
    store_status_t const committed = commit_together(
        state, [&](auto& pairs, auto& strings) noexcept { return stage_drop(pairs, strings, collection, mode); });
    *arguments.error = describe_catalog(committed);
    if (!arguments.error->status && mode == ustore_drop_keys_vals_handle_k) persist(state, arguments.error);
}

/** Implements @c ustore_collection_find, reading the names at the head or at a snapshot. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_collection_find_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.names || !arguments.ids) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need the names and an output for the identifiers"};
        return;
    }
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    std::size_t const count = arguments.tasks_count;
    strings_arg_t const names {{arguments.names, arguments.names_stride},
                               {arguments.names_lengths, arguments.names_lengths_stride}};
    std::span<ustore_collection_t> const ids = arena.alloc<ustore_collection_t>(count, arguments.error);
    auto modalities = arena.alloc_or_dummy(count, arguments.error, arguments.modalities);
    auto presences = arena.alloc_or_dummy(count, arguments.error, arguments.presences);
    if (arguments.error->status) return;

    view_request_t const request {nullptr, arguments.snapshot, arguments.options};
    with_view<keying_t::strings_k>(state, request, arguments.error, [&](auto const& view) noexcept {
        for (std::size_t index = 0; index != count; ++index) {
            auto [key, key_status] = string_key_of(names_collection_k, names[index]);
            if (reported_failure(key_status, arguments.error)) return;
            ustore_collection_t id = main_collection_k;
            ustore_modality_t modality = ustore_modality_any_k;
            bool present = false;
            bool malformed = false;
            store_status_t const looked_up = view.find(
                key,
                [&](string_pair_t const& named) noexcept {
                    present = true;
                    malformed = !read_name_record(named.mapped, id, modality);
                },
                st::no_op_t {});
            if (reported_failure(looked_up, arguments.error)) return;
            if (malformed) {
                *arguments.error = {ustore_error_malformed_k, "A name holds no collection record"};
                return;
            }
            ids[index] = id;
            modalities[index] = modality;
            presences[index] = present;
        }
    });
    if (!arguments.error->status) *arguments.ids = ids.data();
}

#pragma endregion Collection Calls

#pragma region Strings Calls

/** Implements @c ustore_strings_write, committing a batch outside a transaction atomically. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_strings_write_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.strings) {
        *arguments.error = {ustore_error_invalid_argument_k, "No strings were provided"};
        return;
    }
    if (arguments.transaction && (arguments.options & ustore_option_write_flush_k)) {
        *arguments.error = {ustore_error_invalid_argument_k, "A transaction flushes at commit, not at a write"};
        return;
    }
    strided_iterator<ustore_collection_t const> const collections {arguments.collections, arguments.collections_stride};
    strings_arg_t const texts {{arguments.strings, arguments.strings_stride},
                               {arguments.strings_lengths, arguments.strings_lengths_stride}};
    contents_arg_t const contents {bits_view_t {arguments.values_presences},
                                   {arguments.values_offsets, arguments.values_offsets_stride},
                                   {arguments.values_lengths, arguments.values_lengths_stride},
                                   {arguments.values, arguments.values_stride},
                                   arguments.tasks_count};
    if (refuse_collections(state, collections, arguments.tasks_count, keying_t::strings_k, arguments.error)) return;

    if (arguments.transaction) {
        transaction_handle<isolation_>* const handle = transaction_of<isolation_>(arguments.transaction,
                                                                                  arguments.error);
        if (!handle) return;
        watch_t const watch = isolation_ == isolation_t::snapshot_k &&
                                      !(arguments.options & ustore_option_transaction_dont_watch_k)
                                  ? watch_t::watch_k
                                  : watch_t::dont_watch_k;
        reported_failure(
            stage_strings(handle->template part<keying_t::strings_k>(), collections, texts, contents, watch),
            arguments.error);
        return;
    }

    store_status_t written = store_status_t::success_k;
    if (arguments.tasks_count == 1) {
        auto [key, key_status] = string_key_of(collections ? collections[0] : main_collection_k, texts[0]);
        written = st::failed(key_status) ? key_status : write_directly(state.strings, std::move(key), contents[0]);
    }
    else
        written = commit_atomically(state.strings, [&](auto& transaction) noexcept {
            return stage_strings(transaction, collections, texts, contents, watch_t::dont_watch_k);
        });
    if (reported_failure(written, arguments.error)) return;
    if (arguments.options & ustore_option_write_flush_k) persist(state, arguments.error);
}

/** Implements @c ustore_strings_read, copying every value into the caller's arena once. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_strings_read_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.strings) {
        *arguments.error = {ustore_error_invalid_argument_k, "No strings were provided"};
        return;
    }
    strided_iterator<ustore_collection_t const> const collections {arguments.collections, arguments.collections_stride};
    strings_arg_t const texts {{arguments.strings, arguments.strings_stride},
                               {arguments.strings_lengths, arguments.strings_lengths_stride}};
    if (refuse_collections(state, collections, arguments.tasks_count, keying_t::strings_k, arguments.error)) return;
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    value_tape tape {arena, arguments};
    if (arguments.error->status) return;

    with_view<keying_t::strings_k>(state, view_of(arguments), arguments.error, [&](auto const& view) noexcept {
        store_status_t const read = read_strings(view, collections, texts, arguments.tasks_count, tape,
                                                 arguments.error);
        reported_failure(read, arguments.error);
    });
    if (arguments.error->status) return;
    tape.finish();
}

/** Implements @c ustore_strings_scan as ordered walks of each collection's strings. */
template <isolation_t isolation_>
void serve(database_state<isolation_>& state, ustore_strings_scan_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.count_limits || !arguments.strings) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need the limits and an output for the strings"};
        return;
    }
    std::size_t const count = arguments.tasks_count;
    strided_iterator<ustore_collection_t const> const collections {arguments.collections, arguments.collections_stride};
    strings_arg_t const prefixes {{arguments.prefixes, arguments.prefixes_stride},
                                  {arguments.prefixes_lengths, arguments.prefixes_lengths_stride}};
    strings_arg_t const starts {{arguments.start_strings, arguments.start_strings_stride},
                                {arguments.start_strings_lengths, arguments.start_strings_lengths_stride}};
    strided_iterator<ustore_length_t const> const limits {arguments.count_limits, arguments.count_limits_stride};
    if (refuse_collections(state, collections, count, keying_t::strings_k, arguments.error)) return;
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    auto offsets = arena.alloc_or_dummy(count + 1, arguments.error, arguments.offsets);
    auto counts = arena.alloc_or_dummy(count, arguments.error, arguments.counts);
    if (arguments.error->status) return;

    growing_tape_t found {arena};
    with_view<keying_t::strings_k>(state, view_of(arguments), arguments.error, [&](auto const& view) noexcept {
        for (std::size_t task = 0; task != count; ++task) {
            ustore_collection_t const collection = collections ? collections[task] : main_collection_k;
            std::string_view const prefix = arguments.prefixes ? prefixes[task] : std::string_view {};
            std::string_view const start = arguments.start_strings ? starts[task] : std::string_view {};
            std::size_t const found_before = found.lengths().size();
            offsets[task] = static_cast<ustore_length_t>(found_before);
            store_status_t const walked = scan_strings(view, collection, prefix, start, limits[task], found,
                                                       arguments.error);
            if (reported_failure(walked, arguments.error) || arguments.error->status) return;
            counts[task] = static_cast<ustore_length_t>(found.lengths().size() - found_before);
        }
    });
    if (arguments.error->status) return;
    offsets[count] = static_cast<ustore_length_t>(found.lengths().size());
    if (arguments.strings_offsets) *arguments.strings_offsets = found.offsets().begin().get();
    *arguments.strings = reinterpret_cast<ustore_char_t*>(found.contents().begin().get());
}

#pragma endregion Strings Calls

#pragma region Transaction Calls

/** Opens a transaction handle for @p arguments. */
template <isolation_t isolation_>
void open_transaction(database_state<isolation_>& state, ustore_transaction_init_t& arguments) noexcept {
    auto opened = st::make_transaction_group(state.store, state.strings);
    if (reported_failure(opened.status(), arguments.error)) return;
    auto* const handle = new (std::nothrow) transaction_handle<isolation_>(std::move(*opened));
    if (!handle) {
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to allocate the transaction"};
        return;
    }
    *arguments.transaction = static_cast<handle_t*>(handle);
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
    if (arguments.sequence_number)
        *arguments.sequence_number = handle->template part<keying_t::integers_k>().generation();
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
    database_t& database = *handle_cast<database_t>(arguments.db);
    std::visit([&](auto& state) noexcept { serve(state, arguments); }, database.state);
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
    database->engine = &engine_k;
    open_database(*std::get_if<database_state<isolation_>>(&database->state), directory, error);
    return error->status ? nullptr : database.release();
}

void database_capabilities(ustore_database_capabilities_t& arguments) noexcept {
    if (!arguments.capabilities) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the capabilities"};
        return;
    }
    *arguments.capabilities = ustore_capabilities_t(ustore_supports_transactions_k | ustore_supports_collections_k |
                                                    ustore_supports_snapshots_k);
}

void transaction_free(ustore_transaction_t transaction) noexcept {
    auto* const header = handle_cast<transaction_header_t>(transaction);
    if (header->isolation == isolation_t::snapshot_k)
        return delete static_cast<transaction_handle<isolation_t::snapshot_k>*>(header);
    delete static_cast<transaction_handle<isolation_t::strict_serializable_k>*>(header);
}

void database_free(ustore_database_t database) noexcept {
    std::unique_ptr<database_t> freed {handle_cast<database_t>(database)};
    std::visit([](auto& state) noexcept { close_database(state); }, freed->state);
}

engine_t const engine_k {
    .database_capabilities = &database_capabilities,
    .database_control = &serve_database<ustore_database_control_t>,
    .snapshot_list = &serve_database<ustore_snapshot_list_t>,
    .snapshot_create = &serve_database<ustore_snapshot_create_t>,
    .snapshot_export = &serve_database<ustore_snapshot_export_t>,
    .snapshot_drop = &serve_database<ustore_snapshot_drop_t>,
    .read = &serve_database<ustore_read_t>,
    .write = &serve_database<ustore_write_t>,
    .scan = &serve_database<ustore_scan_t>,
    .sample = &serve_database<ustore_sample_t>,
    .measure = &serve_database<ustore_measure_t>,
    .collection_create = &serve_database<ustore_collection_create_t>,
    .collection_drop = &serve_database<ustore_collection_drop_t>,
    .collection_list = &serve_database<ustore_collection_list_t>,
    .collection_find = &serve_database<ustore_collection_find_t>,
    .strings_write = &serve_database<ustore_strings_write_t>,
    .strings_read = &serve_database<ustore_strings_read_t>,
    .strings_scan = &serve_database<ustore_strings_scan_t>,
    .transaction_init = &serve_database<ustore_transaction_init_t>,
    .transaction_stage = &serve_database<ustore_transaction_stage_t>,
    .transaction_commit = &serve_database<ustore_transaction_commit_t>,
    .transaction_free = &transaction_free,
    .database_free = &database_free,
};

#pragma endregion Dispatch

} // namespace unum::ustore::ram

using namespace unum::ustore;
using namespace unum::ustore::ram;

#pragma region C ABI

void ustore_database_init_ram(ustore_database_init_ram_t* arguments) {
    if (!arguments->db) {
        *arguments->error = {ustore_error_invalid_argument_k, "Need an output for the database"};
        return;
    }
    std::string_view const directory = arguments->directory ? arguments->directory : "";
    if (!directory.empty() && !names_directory(arguments->directory)) {
        *arguments->error = {ustore_error_not_found_k, "The directory doesn't exist"};
        return;
    }
    database_t* database = nullptr;
    switch (arguments->isolation) {
    case ustore_isolation_strict_serializable_k:
        database = make_database<isolation_t::strict_serializable_k>(directory, arguments->error);
        break;
    case ustore_isolation_snapshot_k:
        database = make_database<isolation_t::snapshot_k>(directory, arguments->error);
        break;
    default: *arguments->error = {ustore_error_invalid_argument_k, "Unknown isolation"}; return;
    }
    *arguments->db = static_cast<handle_t*>(database);
}

#pragma endregion C ABI
