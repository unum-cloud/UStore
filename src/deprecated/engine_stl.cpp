/**
 * @file engine_stl.cpp
 * @author Ashot Vardanian
 *
 * @brief Embedded In-Memory Key-Value Store implementation using only @b STL.
 * This is not the fastest, not the smartest possible solution for @b ACID KVS,
 * but is a good reference design for educational purposes.
 *
 * Deficiencies:
 * - Global Lock.
 * - No support for range queries.
 * - Keeps watch of all the deleted keys throughout the history.
 */

#include <string_view>
#include <string>
#include <vector>
#include <map>           // Collection names
#include <unordered_map> // Watched keys generations
#include <set>           // Primary entries container
#include <shared_mutex>  // Syncing access to entries container
#include <mutex>         // `std::unique_lock`
#include <numeric>       // `std::accumulate`
#include <atomic>        // Thread-safe generation counters
#include <stdio.h>       // Saving/reading from disk
#include <filesystem>    // Enumerating the directory

#include <fmt/core.h>

#include "ukv/db.h"
#include "ukv/cpp/ranges_args.hpp"   // `places_arg_t`
#include "helpers/linked_memory.hpp" // `linked_memory_lock_t`
#include "helpers/file.hpp"          // `file_handle_t`

/*********************************************************/
/*****************   Structures & Consts  ****************/
/*********************************************************/

ukv_collection_t const ukv_collection_main_k = 0;
ukv_length_t const ukv_length_missing_k = std::numeric_limits<ukv_length_t>::max();
ukv_key_t const ukv_key_unknown_k = std::numeric_limits<ukv_key_t>::max();
bool const ukv_supports_transactions_k = true;
bool const ukv_supports_named_collections_k = true;
bool const ukv_supports_snapshots_k = false;

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

using namespace unum::ukv;
using namespace unum;
namespace fs = std::filesystem;

using allocator_t = std::allocator<byte_t>;
using byte_ptr_t = byte_t*;
using generation_t = std::int64_t;

constexpr generation_t missing_data_generation_k = std::numeric_limits<generation_t>::min();

struct database_t;
struct collection_t;
struct transaction_t;

struct entry_t {
    ukv_collection_t collection {ukv_collection_main_k};
    ukv_key_t key {ukv_key_unknown_k};
    mutable generation_t generation {missing_data_generation_k};
    mutable byte_ptr_t value_begin {nullptr};
    mutable byte_ptr_t value_end {nullptr};

    entry_t() noexcept = default;
    entry_t(collection_key_t col_key) noexcept : collection(col_key.collection), key(col_key.key) {}
    entry_t(entry_t const&) = delete;
    entry_t& operator=(entry_t const&) = delete;

    entry_t(entry_t&& other) noexcept
        : collection(other.collection), key(other.key), generation(other.generation),
          value_begin(std::exchange(other.value_begin, nullptr)), value_end(std::exchange(other.value_end, nullptr)) {}

    entry_t& operator=(entry_t&& other) noexcept {
        std::swap(collection, other.collection);
        std::swap(key, other.key);
        std::swap(generation, other.generation);
        std::swap(value_begin, other.value_begin);
        std::swap(value_end, other.value_end);
        return *this;
    }

    ~entry_t() noexcept { release_blob(); }

    collection_key_t collection_key() const noexcept { return {collection, key}; }
    bool is_deleted() const noexcept { return !value_begin && !value_end; }
    bool is_empty() const noexcept { return value_begin == value_end; }
    explicit operator bool() const noexcept { return !is_deleted(); }
    operator value_view_t() const noexcept {
        return !is_deleted() ? value_view_t {value_begin, value_end} : value_view_t {};
    }

    void release_blob() const noexcept {
        if (value_end != value_begin)
            allocator_t {}.deallocate(value_begin, value_end - value_begin);
        value_begin = value_end = nullptr;
        generation = missing_data_generation_k;
    }

    bool assign_empty(generation_t generation) const noexcept {
        return assign_blob(value_view_t {(byte_ptr_t)(this), (byte_ptr_t)(this)}, generation);
    }
    bool assign_null(generation_t generation) const noexcept { return assign_blob(value_view_t {}, generation); }

    bool alloc_blob(std::size_t length, generation_t g) const noexcept {
        generation = g;
        if (value_end - value_begin == length)
            return true;

        release_blob();
        if (length) {
            auto begin = allocator_t {}.allocate(length);
            if (!begin)
                return false;
            value_begin = begin;
            value_end = begin + length;
        }
        generation = g;
        return true;
    }

    bool assign_blob(value_view_t value, generation_t g) const noexcept {
        if (!alloc_blob(value.size(), g))
            return false;

        if (value.size()) {
            std::memcpy(value_begin, value.data(), value.size());
            return true;
        }
        else {
            // Dirty-hack - using self-reference to distinguish empty (zero
            // length) value from deleted one.
            value_begin = value_end = value ? (byte_ptr_t)(this) : nullptr;
            generation = g;
            return true;
        }
    }

    void swap_blob(entry_t const& other) const noexcept {
        std::swap(generation, other.generation);
        std::swap(value_begin, other.value_begin);
        std::swap(value_end, other.value_end);
    }
};

struct entry_compare_t {
    using is_transparent = void;
    bool operator()(entry_t const& a, entry_t const& b) const noexcept {
        return a.collection == b.collection ? a.key < b.key : a.collection < b.collection;
    }
    bool operator()(entry_t const& a, collection_key_t b) const noexcept {
        return a.collection == b.collection ? a.key < b.key : a.collection < b.collection;
    }
    bool operator()(collection_key_t a, entry_t const& b) const noexcept {
        return a.collection == b.collection ? a.key < b.key : a.collection < b.collection;
    }
    bool operator()(entry_t const& a, ukv_collection_t b) const noexcept { return a.collection < b; }
    bool operator()(ukv_collection_t a, entry_t const& b) const noexcept { return a < b.collection; }
};

using entry_allocator_t = std::allocator<entry_t>;
using entries_set_t = std::set<entry_t, entry_compare_t, entry_allocator_t>;

using collection_ptr_t = std::unique_ptr<collection_t>;

struct string_hash_t {
    using stl_t = std::hash<std::string_view>;
    using is_transparent = void;

    auto operator()(const char* str) const { return stl_t {}(str); }
    auto operator()(std::string_view str) const { return stl_t {}(str); }
    auto operator()(std::string const& str) const { return stl_t {}(str); }
};

struct string_eq_t : public std::equal_to<std::string_view> {
    using is_transparent = void;
};

struct string_less_t : public std::less<std::string_view> {
    using is_transparent = void;
};

struct database_t {
    std::shared_mutex mutex;
    entries_set_t entries;

    /**
     * @brief A variable-size set of named collections.
     * It's cleaner to implement it with heterogenous lookups as
     * an @c std::unordered_map, but it requires GCC11 and C++20.
     */
    std::map<std::string, ukv_collection_t, string_less_t> names;
    /**
     * @brief The generation/transactions ID of the most recent update.
     * This can be updated even outside of the main @p `mutex` on HEAD state.
     */
    std::atomic<generation_t> youngest_generation {0};
    /**
     * @brief Path on disk, from which the data will be read.
     * When closed, we will try saving the DB on disk.
     */
    std::string persisted_path;

    void reserve_entry_nodes(std::size_t) {
        //  pairs.reserve(pairs.size() + n);
    }

    ukv_collection_t new_collection() const noexcept {
        bool is_new = false;
        ukv_collection_t new_handle = ukv_collection_main_k;
        while (!is_new) {
            auto top = static_cast<std::uint64_t>(std::rand());
            auto bottom = static_cast<std::uint64_t>(std::rand());
            new_handle = static_cast<ukv_collection_t>((top << 32) | bottom);
            is_new = new_handle != ukv_collection_main_k;
            for (auto const& [name, existing_handle] : names)
                is_new &= new_handle != existing_handle;
        }
        return new_handle;
    }
};

/**
 * @brief Solves the problem of modulo arithmetic and `generation_t` overflow.
 * Still works correctly, when `max` has overflown, but `min` hasn't yet,
 * so `min` can be bigger than `max`.
 */
inline bool entry_was_overwritten(generation_t entry_generation,
                                  generation_t transaction_generation,
                                  generation_t youngest_generation) noexcept {

    return transaction_generation <= youngest_generation
               ? ((entry_generation >= transaction_generation) & (entry_generation <= youngest_generation))
               : ((entry_generation >= transaction_generation) | (entry_generation <= youngest_generation));
}

/*********************************************************/
/*****************	 Writing to Disk	  ****************/
/*********************************************************/

template <typename entries_iterator_at>
void write_entries(file_handle_t const& handle,
                   entries_iterator_at begin,
                   entries_iterator_at end,
                   ukv_error_t* c_error) {

    for (; begin != end; ++begin) {
        entry_t const& entry = *begin;
        if (!entry)
            continue;

        auto saved_len = std::fwrite(&entry.collection, sizeof(ukv_collection_t), 1, handle);
        return_error_if_m(saved_len == 1, c_error, 0, "Write partially failed on collection.");

        saved_len = std::fwrite(&entry.key, sizeof(ukv_key_t), 1, handle);
        return_error_if_m(saved_len == 1, c_error, 0, "Write partially failed on key.");

        auto buf = value_view_t(entry);
        auto buf_len = static_cast<ukv_length_t>(buf.size());
        saved_len = std::fwrite(&buf_len, sizeof(ukv_length_t), 1, handle);
        return_error_if_m(saved_len == 1, c_error, 0, "Write partially failed on value len.");

        saved_len = std::fwrite(buf.data(), sizeof(byte_t), buf.size(), handle);
        return_error_if_m(saved_len == buf.size(), c_error, 0, "Write partially failed on value.");
    }
}

template <typename entries_iterator_at>
void read_entries(file_handle_t const& handle, entries_iterator_at output, ukv_error_t* c_error) {

    while (std::feof(handle) == 0) {
        entry_t entry;

        // An empty row may contain no content
        auto read_len = std::fread(&entry.collection, sizeof(ukv_collection_t), 1, handle);
        if (read_len == 0)
            break;
        return_error_if_m(read_len <= 1, c_error, 0, "Read yielded unexpected result on key.");

        // .. but if the row exists, it shouldn't be partial
        read_len = std::fread(&entry.key, sizeof(ukv_key_t), 1, handle);
        return_error_if_m(read_len == 1, c_error, 0, "Read partially failed on key.");

        auto buf_len = ukv_length_t(0);
        read_len = std::fread(&buf_len, sizeof(ukv_length_t), 1, handle);
        return_error_if_m(read_len == 1, c_error, 0, "Read partially failed on value len.");

        return_error_if_m(entry.alloc_blob(buf_len, 0),
                        c_error,
                        out_of_memory_k,
                        "Failed to allocate memory for new node");
        read_len = std::fread(entry.value_begin, sizeof(byte_t), buf_len, handle);
        return_error_if_m(read_len == buf_len, c_error, 0, "Read partially failed on value.");

        *output = std::move(entry);
        ++output;
    }
}

void write(database_t const& db, std::string const& path, ukv_error_t* c_error) {
    // Using the classical C++ IO mechanisms is a bad tone in the modern world.
    // They are ugly and, more importantly, painfully slow.
    // https://www.reddit.com/r/cpp_questions/comments/e2xia9/performance_comparison_of_various_ways_of_reading/
    //
    // So instead we stick to the LibC way of doing things.
    // POSIX API would have been even better, but LibC will provide
    // higher portability for this reference implementation.
    // https://www.ibm.com/docs/en/i/7.1?topic=functions-fopen-open-files
    file_handle_t handle;
    if ((*c_error = handle.open(path.c_str(), "wb+").release_error()))
        return;

    // Print stats about the overall dataset:
    // https://fmt.dev/latest/api.html#_CPPv4IDpEN3fmt5printEvPNSt4FILEE13format_stringIDp1TEDpRR1T
    std::fprintf(handle, "Total Items: %zu\n", db.entries.size());
    std::fprintf(handle, "Named Collections: %zu\n", db.names.size());
    for (auto const& name_and_handle : db.names)
        std::fprintf(handle,
                     "-%s: 0x%016zx\n",
                     name_and_handle.first.c_str(),
                     static_cast<std::size_t>(name_and_handle.second));
    std::fprintf(handle, "\n");

    // Save the entries
    write_entries(handle, db.entries.begin(), db.entries.end(), c_error);
    return_if_error_m(c_error);

    // Close the file
    log_error_m(c_error, 0, handle.close().release_error());
}

void read(database_t& db, std::string const& path, ukv_error_t* c_error) {
    db.entries.clear();
    db.names.clear();

    // Check if file even exists
    if (!std::filesystem::exists(path))
        return;

    // Similar to serialization, we don't use STL here
    file_handle_t handle;
    if ((*c_error = handle.open(path.c_str(), "rb+").release_error()))
        return;

    // Get the collection size, to preallocate entries
    char line_buffer[256];
    while (std::fgets(line_buffer, sizeof(line_buffer), handle) != NULL) {
        // Check if it is a collection description
        auto line_length = std::find(line_buffer, line_buffer + sizeof(line_buffer), '\n') - line_buffer;
        if (line_length == 0)
            break;
        if (line_buffer[0] == '-') {
            auto name_length = std::find(line_buffer + 1, line_buffer + sizeof(line_buffer), ':') - line_buffer;
            auto name = std::string_view(line_buffer + 1, name_length);
            auto id_str_begin = line_buffer + line_length - 16;
            auto id_str_end = line_buffer + line_length;
            auto id = std::strtoull(id_str_begin, &id_str_end, 16);
            db.names.emplace(name, id);
        }
        else
            // Skip metadata rows
            continue;
    }

    // Load the entries
    read_entries(handle, std::inserter(db.entries, db.entries.end()), c_error);
    return_if_error_m(c_error);

    // Close the file
    log_error_m(c_error, 0, handle.close().release_error());
}

/*********************************************************/
/*****************	 Read/Write Head/Txn  ****************/
/*********************************************************/

void populate( //
    places_arg_t places,
    contents_arg_t contents,
    generation_t generation,
    entries_set_t& entries,
    ukv_error_t* c_error) noexcept {

    safe_section("Building batch tree", c_error, [&] {
        for (std::size_t i = 0; i != places.size(); ++i) {
            auto place = places[i];
            auto content = contents[i];
            entry_t entry {place.collection_key()};
            auto entry_allocated = entry.assign_blob(content, generation);
            return_error_if_m(entry_allocated, c_error, out_of_memory_k, "Couldn't allocate a blob");
            entries.insert(std::move(entry));
        }
    });
}

void write( //
    database_t& db,
    places_arg_t places,
    contents_arg_t contents,
    ukv_options_t const c_options,
    ukv_error_t* c_error) noexcept {

    // In here we don't care about the consistency,
    // just the fact of either writing all values or not.
    // So we can build the entries before the write lock
    // and not check generations afterwards.
    entries_set_t entries;
    auto generation = ++db.youngest_generation;
    populate(places, contents, generation, entries, c_error);
    return_if_error_m(c_error);

    std::unique_lock _ {db.mutex};
    merge_overwrite(db.entries, entries);

    // TODO: Degrade the lock to "shared" state before starting expensive IO
    if (c_options & ukv_option_write_flush_k)
        write(db, db.persisted_path, c_error);
}

void write( //
    transaction_t& txn,
    places_arg_t places,
    contents_arg_t contents,
    ukv_options_t const c_options,
    ukv_error_t* c_error) noexcept {

    // No need for locking here, until we commit, unless, of course,
    // a collection is being deleted.
    database_t& db = *txn.db_ptr;
    bool dont_watch = c_options & ukv_option_transaction_dont_watch_k;

    // Track potential future changes
    if (!dont_watch) {
        std::shared_lock _ {db.mutex};
        safe_section("Copying new value", c_error, [&] {
            for (std::size_t i = 0; i != places.size(); ++i) {
                auto place = places[i];
                auto db_iterator = db.entries.find(place.collection_key());
                auto last_generation =
                    db_iterator != db.entries.end() ? db_iterator->generation : missing_data_generation_k;
                txn.watched.insert_or_assign(place.collection_key(), last_generation);
            }
        });
    }

    populate(places, contents, txn.generation, txn.changes, c_error);
}

template <typename value_enumerator_at>
void read_under_lock( //
    database_t& db,
    places_arg_t tasks,
    ukv_options_t const,
    value_enumerator_at enumerator,
    ukv_error_t*) noexcept {

    for (std::size_t i = 0; i != tasks.size(); ++i) {
        place_t place = tasks[i];
        auto db_iterator = db.entries.find(place.collection_key());
        auto value = db_iterator != db.entries.end() ? value_view_t(*db_iterator) : value_view_t {};
        enumerator(i, value);
    }
}

template <typename value_enumerator_at>
void read_under_lock( //
    transaction_t& txn,
    places_arg_t tasks,
    ukv_options_t const c_options,
    value_enumerator_at enumerator,
    ukv_error_t* c_error) noexcept {

    database_t& db = *txn.db_ptr;
    generation_t const youngest_generation = db.youngest_generation.load();
    bool const dont_watch = c_options & ukv_option_transaction_dont_watch_k;

    for (std::size_t i = 0; i != tasks.size(); ++i) {
        place_t place = tasks[i];

        // Some keys may already be overwritten inside of transaction
        if (auto txn_iterator = txn.changes.find(place.collection_key()); txn_iterator != txn.changes.end())
            enumerator(i, value_view_t(*txn_iterator));

        // Others should be pulled from the main store
        else if (auto db_iterator = db.entries.find(place.collection_key()); db_iterator != db.entries.end()) {

            if (entry_was_overwritten(db_iterator->generation, txn.generation, youngest_generation) &&
                (*c_error = "Requested key was already overwritten since the start of the transaction!"))
                return;

            enumerator(i, value_view_t(*db_iterator));
            if (!dont_watch)
                txn.watched.emplace(place.collection_key(), db_iterator->generation);
        }

        // But some will be missing
        else {
            enumerator(i, value_view_t {});

            if (!dont_watch)
                txn.watched.emplace(place.collection_key(), missing_data_generation_k);
        }
    }
}

void scan( //
    database_t& db,
    scans_arg_t tasks,
    ukv_options_t const options,
    ukv_length_t** c_found_offsets,
    ukv_length_t** c_found_counts,
    ukv_key_t** c_found_keys,
    linked_memory_lock_t& arena,
    ukv_error_t* c_error) noexcept {

    std::shared_lock _ {db.mutex};

    // 1. Allocate a tape for all the values to be fetched
    auto offsets = arena.alloc_or_dummy(tasks.count + 1, c_error, c_found_offsets);
    return_if_error_m(c_error);
    auto counts = arena.alloc_or_dummy(tasks.count, c_error, c_found_counts);
    return_if_error_m(c_error);

    auto total_keys = reduce_n(tasks.limits, tasks.count, 0ul);
    auto keys_output = *c_found_keys = arena.alloc<ukv_key_t>(total_keys, c_error).begin();
    return_if_error_m(c_error);

    // 2. Fetch the data
    for (std::size_t i = 0; i != tasks.size(); ++i) {
        scan_t scan = tasks[i];
        offsets[i] = keys_output - *c_found_keys;

        ukv_length_t j = 0;
        auto db_iterator = db.entries.lower_bound(collection_key_t {scan.collection, scan.min_key});
        while (j != scan.limit &&                            //
               db_iterator != db.entries.end() &&            //
               db_iterator->collection == scan.collection && //
               db_iterator->key < scan.max_key) {

            if (db_iterator->is_deleted()) {
                ++db_iterator;
                continue;
            }

            *keys_output = db_iterator->key;
            ++db_iterator;
            ++keys_output;
            ++j;
        }

        counts[i] = j;
    }
    offsets[tasks.size()] = keys_output - *c_found_keys;
}

void scan( //
    transaction_t& txn,
    scans_arg_t tasks,
    ukv_options_t const c_options,
    ukv_length_t** c_found_offsets,
    ukv_length_t** c_found_counts,
    ukv_key_t** c_found_keys,
    linked_memory_lock_t& arena,
    ukv_error_t* c_error) noexcept {

    database_t& db = *txn.db_ptr;
    std::shared_lock _ {db.mutex};
    bool const dont_watch = c_options & ukv_option_transaction_dont_watch_k;

    // 1. Allocate a tape for all the values to be fetched
    auto offsets = arena.alloc_or_dummy(tasks.count + 1, c_error, c_found_offsets);
    return_if_error_m(c_error);
    auto counts = arena.alloc_or_dummy(tasks.count, c_error, c_found_counts);
    return_if_error_m(c_error);

    auto total_keys = reduce_n(tasks.limits, tasks.count, 0ul);
    auto keys_output = *c_found_keys = arena.alloc<ukv_key_t>(total_keys, c_error).begin();
    return_if_error_m(c_error);

    // 2. Fetch the data
    for (std::size_t i = 0; i != tasks.size(); ++i) {
        scan_t scan = tasks[i];
        offsets[i] = keys_output - *c_found_keys;

        ukv_length_t j = 0;
        auto db_iterator = db.entries.lower_bound(collection_key_t {scan.collection, scan.min_key});
        auto txn_iterator = txn.changes.lower_bound(collection_key_t {scan.collection, scan.min_key});
        while (j != scan.limit && db_iterator != db.entries.end() && db_iterator->collection == scan.collection) {

            // Check if the key was already removed:
            auto join_iterator = txn.changes.find(db_iterator->collection_key());
            if (db_iterator->is_deleted() || (join_iterator != txn.changes.end() && join_iterator->is_deleted())) {
                ++db_iterator;
                continue;
            }

            // Compare against the incoming inserted keys:
            bool check_in_txn = txn_iterator != txn.changes.end() && txn_iterator->collection == scan.collection;
            if (check_in_txn && txn_iterator->key <= db_iterator->key) {
                *keys_output = txn_iterator->key;
                ++keys_output;
                ++txn_iterator;
                ++j;
                continue;
            }

            // Make sure we haven't reached the end keys
            if (db_iterator->key >= scan.max_key)
                break;

            // Export from the main store:
            *keys_output = db_iterator->key;
            ++keys_output;
            ++db_iterator;
            ++j;
        }

        // As in any `set_union`, don't forget the tail :)
        while (j != scan.limit &&                             //
               txn_iterator != txn.changes.end() &&           //
               txn_iterator->collection == scan.collection && //
               txn_iterator->key < scan.max_key) {
            *keys_output = txn_iterator->key;
            ++keys_output;
            ++txn_iterator;
            ++j;
        }

        counts[i] = j;
    }
    offsets[tasks.size()] = keys_output - *c_found_keys;
}

/*********************************************************/
/*****************	    C Interface 	  ****************/
/*********************************************************/

void ukv_database_init(ukv_database_init_t* c_ptr) {

    ukv_database_init_t& c = *c_ptr;
    safe_section("Initializing DBMS", c.error, [&] {
        auto db_ptr = new database_t {};
        auto len = c.config ? std::strlen(c.config) : 0;
        if (len) {
            db_ptr->persisted_path = std::string(c.config, len);
            read(*db_ptr, db_ptr->persisted_path, c.error);
        }
        *c.db = db_ptr;
    });
}

void ukv_read(ukv_read_t* c_ptr) {

    ukv_read_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    if (!c.tasks_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    database_t& db = *reinterpret_cast<database_t*>(c.db);
    transaction_t& txn = *reinterpret_cast<transaction_t*>(c.transaction);
    strided_iterator_gt<ukv_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ukv_key_t const> keys {c.keys, c.keys_stride};
    places_arg_t places {collections, keys, {}, c.tasks_count};
    validate_read(c.transaction, places, c.options, c.error);
    return_if_error_m(c.error);

    bool const needs_export = c.values != nullptr;

    // 1. Allocate a tape for all the values to be pulled
    auto offs = arena.alloc_or_dummy(places.count + 1, c.error, c.offsets);
    return_if_error_m(c.error);
    auto lens = arena.alloc_or_dummy(places.count, c.error, c.lengths);
    return_if_error_m(c.error);
    auto presences = arena.alloc_or_dummy(places.count, c.error, c.presences);
    return_if_error_m(c.error);

    // 2. Pull metadata
    ukv_length_t total_length = 0;
    auto meta_enumerator = [&](std::size_t i, value_view_t value) {
        presences[i] = bool(value);
        offs[i] = total_length;
        lens[i] = value ? static_cast<ukv_length_t>(value.size()) : ukv_length_missing_k;
        total_length += static_cast<ukv_length_t>(value.size());
    };

    std::shared_lock _ {db.mutex};
    c.transaction //
        ? read_under_lock(txn, places, c.options, meta_enumerator, c.error)
        : read_under_lock(db, places, c.options, meta_enumerator, c.error);
    offs[places.count] = total_length;
    if (!needs_export)
        return;

    // 3. Pull the data, once we know the total length
    auto tape = arena.alloc<byte_t>(total_length, c.error).begin();
    auto data_enumerator = [&](std::size_t i, value_view_t value) {
        std::memcpy(std::exchange(tape, tape + value.size()), value.begin(), value.size());
    };

    *c.values = reinterpret_cast<ukv_byte_t*>(tape);
    c.transaction //
        ? read_under_lock(txn, places, c.options, data_enumerator, c.error)
        : read_under_lock(db, places, c.options, data_enumerator, c.error);
}

void ukv_write(ukv_write_t* c_ptr) {

    ukv_write_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    if (!c.tasks_count)
        return;

    database_t& db = *reinterpret_cast<database_t*>(c.db);
    transaction_t& txn = *reinterpret_cast<transaction_t*>(c.transaction);
    strided_iterator_gt<ukv_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ukv_key_t const> keys {c.keys, c.keys_stride};
    strided_iterator_gt<ukv_bytes_cptr_t const> vals {c.values, c.values_stride};
    strided_iterator_gt<ukv_length_t const> offs {c.offsets, c.offsets_stride};
    strided_iterator_gt<ukv_length_t const> lens {c.lengths, c.lengths_stride};
    bits_view_t presences {c.presences};

    places_arg_t places {collections, keys, {}, c.tasks_count};
    contents_arg_t contents {presences, offs, lens, vals, c.tasks_count};

    validate_write(c.transaction, places, contents, c.options, c.error);
    return_if_error_m(c.error);

    return c.transaction //
               ? write(txn, places, contents, c.options, c.error)
               : write(db, places, contents, c.options, c.error);
}

void ukv_scan(ukv_scan_t* c_ptr) {

    ukv_scan_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    if (!c.tasks_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    database_t& db = *reinterpret_cast<database_t*>(c.db);
    transaction_t& txn = *reinterpret_cast<transaction_t*>(c.transaction);
    strided_iterator_gt<ukv_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ukv_key_t const> start_keys {c.start_keys, c.start_keys_stride};
    strided_iterator_gt<ukv_length_t const> lens {c.count_limits, c.count_limits_stride};
    scans_arg_t scans {collections, start_keys, lens, c.tasks_count};

    validate_scan(c.transaction, scans, c.options, c.error);
    return_if_error_m(c.error);

    return c.transaction //
               ? scan(txn, scans, c.options, c.offsets, c.counts, c.keys, arena, c.error)
               : scan(db, scans, c.options, c.offsets, c.counts, c.keys, arena, c.error);
}

void ukv_measure(ukv_measure_t* c_ptr) {

    ukv_measure_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    if (!c.tasks_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    auto min_cardinalities = arena.alloc_or_dummy(c.tasks_count, c.error, c.min_cardinalities);
    auto max_cardinalities = arena.alloc_or_dummy(c.tasks_count, c.error, c.max_cardinalities);
    auto min_value_bytes = arena.alloc_or_dummy(c.tasks_count, c.error, c.min_value_bytes);
    auto max_value_bytes = arena.alloc_or_dummy(c.tasks_count, c.error, c.max_value_bytes);
    auto min_space_usages = arena.alloc_or_dummy(c.tasks_count, c.error, c.min_space_usages);
    auto max_space_usages = arena.alloc_or_dummy(c.tasks_count, c.error, c.max_space_usages);
    return_if_error_m(c.error);

    database_t& db = *reinterpret_cast<database_t*>(c.db);
    transaction_t& txn = *reinterpret_cast<transaction_t*>(c.transaction);
    strided_iterator_gt<ukv_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ukv_key_t const> start_keys {c.start_keys, c.start_keys_stride};
    strided_iterator_gt<ukv_key_t const> end_keys {c.end_keys, c.end_keys_stride};

    std::shared_lock _ {db.mutex};

    for (ukv_size_t i = 0; i != c.tasks_count; ++i) {
        ukv_collection_t const collection = collections[i];
        ukv_key_t const min_key = start_keys[i];
        ukv_key_t const max_key = end_keys[i];
        std::size_t deleted_count = 0;

        // Estimate the presence in the main store
        std::size_t main_count = 0;
        std::size_t main_bytes = 0;
        auto min_iterator = db.entries.lower_bound(collection_key_t {collection, min_key});
        auto max_iterator = db.entries.upper_bound(collection_key_t {collection, max_key});
        for (; min_iterator != max_iterator; ++min_iterator) {
            if (min_iterator->is_deleted()) {
                ++deleted_count;
                continue;
            }
            ++main_count;
            main_bytes += value_view_t(*min_iterator).size();
        }

        // Estimate the metrics from within a transaction
        std::size_t txn_count = 0;
        std::size_t txn_bytes = 0;
        if (c.transaction) {
            auto min_iterator = txn.changes.lower_bound(collection_key_t {collection, min_key});
            auto max_iterator = txn.changes.upper_bound(collection_key_t {collection, max_key});
            for (; min_iterator != max_iterator; ++min_iterator, ++txn_count)
                txn_bytes += value_view_t(*min_iterator).size();
        }

        //
        ukv_size_t estimate[6];
        min_cardinalities[i] = estimate[0] = static_cast<ukv_size_t>(main_count);
        max_cardinalities[i] = estimate[1] = static_cast<ukv_size_t>(main_count + txn_count);
        min_value_bytes[i] = estimate[2] = static_cast<ukv_size_t>(main_bytes);
        max_value_bytes[i] = estimate[3] = static_cast<ukv_size_t>(main_bytes + txn_bytes);
        min_space_usages[i] = estimate[4] = estimate[0] * (sizeof(ukv_key_t) + sizeof(ukv_length_t)) + estimate[2];
        max_space_usages[i] = estimate[5] =
            (estimate[1] + deleted_count) * (sizeof(ukv_key_t) + sizeof(ukv_length_t)) + estimate[3];
    }
}

/*********************************************************/
/*****************	Collections Management	****************/
/*********************************************************/

void ukv_collection_create(ukv_collection_create_t* c_ptr) {

    ukv_collection_create_t& c = *c_ptr;
    auto name_len = c.name ? std::strlen(c.name) : 0;
    return_error_if_m(name_len, c.error, args_wrong_k, "Default collection is always present");
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    database_t& db = *reinterpret_cast<database_t*>(c.db);
    std::unique_lock _ {db.mutex};

    std::string_view collection_name {c.name, name_len};
    auto collection_it = db.names.find(collection_name);
    return_error_if_m(collection_it == db.names.end(), c.error, args_wrong_k, "Such collection already exists!");

    auto new_collection = db.new_collection();
    safe_section("Inserting new collection", c.error, [&] { db.names.emplace(collection_name, new_collection); });
    *c.id = new_collection;
}

void ukv_collection_drop(ukv_collection_drop_t* c_ptr) {

    ukv_collection_drop_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");

    bool invalidate = c.mode == ukv_drop_keys_vals_handle_k;
    return_error_if_m(c.id != ukv_collection_main_k || !invalidate,
                    c.error,
                    args_combo_k,
                    "Default collection can't be invalidated.");

    database_t& db = *reinterpret_cast<database_t*>(c.db);
    std::unique_lock _ {db.mutex};

    auto [begin, end] = db.entries.equal_range(c.id);
    if (c.mode == ukv_drop_keys_vals_handle_k) {
        db.entries.erase(begin, end);
        for (auto it = db.names.begin(); it != db.names.end(); ++it) {
            if (c.id != it->second)
                continue;
            db.names.erase(it);
            break;
        }
    }

    else if (c.mode == ukv_drop_keys_vals_k)
        db.entries.erase(begin, end);

    else if (c.mode == ukv_drop_vals_k) {
        generation_t gen = ++db.youngest_generation;
        for (; begin != end; ++begin)
            begin->assign_empty(gen);
    }
}

void ukv_collection_list(ukv_collection_list_t* c_ptr) {

    ukv_collection_list_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    return_error_if_m(c.count && c.names, c.error, args_combo_k, "Need names and outputs!");

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    database_t& db = *reinterpret_cast<database_t*>(c.db);
    std::shared_lock _ {db.mutex};
    std::size_t collections_count = db.names.size();
    *c.count = static_cast<ukv_size_t>(collections_count);

    // Every string will be null-terminated
    std::size_t strings_length = 0;
    for (auto const& name_and_handle : db.names)
        strings_length += name_and_handle.first.size() + 1;
    auto names = arena.alloc<char>(strings_length, c.error).begin();
    *c.names = names;
    return_if_error_m(c.error);

    // For every collection we also need to export IDs and offsets
    auto ids = arena.alloc_or_dummy(collections_count, c.error, c.ids);
    return_if_error_m(c.error);
    auto offs = arena.alloc_or_dummy(collections_count + 1, c.error, c.offsets);
    return_if_error_m(c.error);

    std::size_t i = 0;
    for (auto const& name_and_handle : db.names) {
        auto len = name_and_handle.first.size();
        std::memcpy(names, name_and_handle.first.data(), len);
        names[len] = '\0';
        ids[i] = name_and_handle.second;
        offs[i] = static_cast<ukv_length_t>(names - *c.names);
        names += len + 1;
        ++i;
    }
    offs[i] = static_cast<ukv_length_t>(names - *c.names);
}

void ukv_database_control(ukv_database_control_t* c_ptr) {

    ukv_database_control_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    return_error_if_m(c.request, c.error, uninitialized_state_k, "Request is uninitialized");

    *c.response = NULL;
    log_error_m(c.error, missing_feature_k, "Controls aren't supported in this implementation!");
}

/*********************************************************/
/*****************		Transactions	  ****************/
/*********************************************************/

void ukv_transaction_init(ukv_transaction_init_t* c_ptr) {

    ukv_transaction_init_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    validate_transaction_begin(c.transaction, c.options, c.error);
    return_if_error_m(c.error);

    database_t& db = *reinterpret_cast<database_t*>(c.db);
    safe_section("Initializing transaction state", c.error, [&] {
        if (!*c.transaction)
            *c.transaction = new transaction_t();
    });
    return_if_error_m(c.error);

    transaction_t& txn = *reinterpret_cast<transaction_t*>(*c.transaction);
    txn.db_ptr = &db;
    txn.generation = ++db.youngest_generation;
    txn.watched.clear();
    txn.changes.clear();
}

void ukv_transaction_commit(ukv_transaction_commit_t* c_ptr) {

    ukv_transaction_commit_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    database_t& db = *reinterpret_cast<database_t*>(c.db);

    validate_transaction_commit(c.transaction, c.options, c.error);
    return_if_error_m(c.error);
    transaction_t& txn = *reinterpret_cast<transaction_t*>(c.transaction);

    // This write may fail with out-of-memory errors, if Hash-Tables
    // bucket allocation fails, but no values will be copied, only moved.
    std::unique_lock _ {db.mutex};
    generation_t const youngest_generation = db.youngest_generation.load();

    // 1. Check for changes in DBMS
    for (auto const& [collection_key, watched_generation] : txn.watched) {
        auto db_iterator = db.entries.find(collection_key);
        bool missing = db_iterator == db.entries.end();
        if (watched_generation == missing_data_generation_k) {
            return_error_if_m(missing, c.error, consistency_k, "WATCH-ed key was added");
        }
        else {
            return_error_if_m(!missing, c.error, consistency_k, "WATCH-ed key was deleted");
            bool untouched = db_iterator->generation == watched_generation;
            return_error_if_m(untouched, c.error, consistency_k, "WATCH-ed key was updated");
        }
    }

    // 2. Import the data, removing the older version beforehand
    merge_overwrite(db.entries, txn.changes);

    // TODO: Degrade the lock to "shared" state before starting expensive IO
    if (c.options & ukv_option_write_flush_k)
        write(db, db.persisted_path, c.error);
}

/*********************************************************/
/*****************	  Memory Management   ****************/
/*********************************************************/

void ukv_arena_free(ukv_arena_t c_arena) {
    if (!c_arena)
        return;
    linked_memory_lock_t& arena = *reinterpret_cast<linked_memory_lock_t*>(c_arena);
    delete &arena;
}

void ukv_transaction_free(ukv_transaction_t const c_txn) {
    if (!c_txn)
        return;
    transaction_t& txn = *reinterpret_cast<transaction_t*>(c_txn);
    delete &txn;
}

void ukv_database_free(ukv_database_t c_db) {
    if (!c_db)
        return;

    database_t& db = *reinterpret_cast<database_t*>(c_db);
    if (!db.persisted_path.empty()) {
        ukv_error_t c_error = nullptr;
        write(db, db.persisted_path, &c_error);
    }

    delete &db;
}

void ukv_error_free(ukv_error_t) {
}
