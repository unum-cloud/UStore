/**
 * @file engine_umem.cpp
 * @author Ashot Vardanian
 *
 * @brief Embedded In-Memory Key-Value Store built on @b AVL trees or STL.
 * This implementation uses straightforward approach to implement concurrency.
 * It keeps all the pairs sorted and is pretty fast for a BST-based container.
 */

#include <stdio.h> // Saving/reading from disk

#include <map>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <mutex>      // `std::unique_lock`
#include <numeric>    // `std::accumulate`
#include <atomic>     // Thread-safe generation counters
#include <filesystem> // Enumerating the directory
#include <fstream>    // Passing file contents to JSON parser

#include <ucset/consistent_set.hpp> // `ucset::consistent_set_gt`
#include <ucset/consistent_avl.hpp> // `ucset::consistent_avl_gt`
#include <ucset/locked.hpp>         // `ucset::locked_gt`
#include <ucset/partitioned.hpp>    // `ucset::partitioned_gt`

#include <nlohmann/json.hpp>       // `nlohmann::json`
#include <arrow/io/file.h>         // `arrow::io::ReadableFile`
#include <parquet/stream_reader.h> // `parquet::StreamReader`
#include <parquet/stream_writer.h> // `parquet::StreamWriter`

#include "ukv/db.h"
#include "helpers/file.hpp"
#include "helpers/linked_memory.hpp" // `linked_memory_t`
#include "helpers/linked_array.hpp"  // `unintialized_vector_gt`
#include "ukv/cpp/ranges_args.hpp"   // `places_arg_t`

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

using namespace unum::ucset;
using namespace unum::ukv;
using namespace unum;

namespace stdfs = std::filesystem;
using json_t = nlohmann::json;

using blob_allocator_t = std::allocator<byte_t>;

static constexpr char const* config_name_k = "config_umem.json";

struct pair_t {
    collection_key_t collection_key;
    value_view_t range;

    pair_t() = default;
    pair_t(pair_t const&) = delete;
    pair_t& operator=(pair_t const&) = delete;

    pair_t(collection_key_t collection_key) noexcept : collection_key(collection_key) {}

    pair_t(collection_key_t collection_key, value_view_t other, ukv_error_t* c_error) noexcept
        : collection_key(collection_key) {
        if (other.size()) {
            auto begin = blob_allocator_t {}.allocate(other.size());
            return_error_if_m(begin != nullptr, c_error, out_of_memory_k, "Failed to copy a blob");
            range = {begin, other.size()};
            std::memcpy(begin, other.begin(), other.size());
        }
        else
            range = other;
    }

    ~pair_t() noexcept {
        if (range.size())
            blob_allocator_t {}.deallocate((byte_t*)range.data(), range.size());
        range = {};
    }

    pair_t(pair_t&& other) noexcept
        : collection_key(other.collection_key), range(std::exchange(other.range, value_view_t {})) {}

    pair_t& operator=(pair_t&& other) noexcept {
        std::swap(collection_key, other.collection_key);
        std::swap(range, other.range);
        return *this;
    }

    operator collection_key_t() const noexcept { return collection_key; }
    explicit operator bool() const noexcept { return range; }
};

struct pair_compare_t {
    using value_type = collection_key_t;
    bool operator()(collection_key_t const& a, collection_key_t const& b) const noexcept { return a < b; }
    bool operator()(collection_key_t const& a, ukv_collection_t b) const noexcept { return a.collection < b; }
    bool operator()(ukv_collection_t a, collection_key_t const& b) const noexcept { return a < b.collection; }
};

/*********************************************************/
/*****************  Using Consistent Sets ****************/
/*********************************************************/

// using ucset_t = ucset_gt<pair_t, pair_compare_t>;
// using ucset_t = consistent_avl_gt<pair_t, pair_compare_t>;
// using ucset_t = partitioned_gt< //
//     ucset_gt<pair_t, pair_compare_t>,
//     std::hash<collection_key_t>,
//     std::shared_mutex,
//     64>;
using ucset_t = locked_gt<consistent_set_gt<pair_t, pair_compare_t>, std::shared_mutex>;
using transaction_t = typename ucset_t::transaction_t;
using generation_t = typename ucset_t::generation_t;

template <typename set_or_transaction_at, typename callback_at>
ucset::status_t find_and_watch(set_or_transaction_at& set_or_transaction,
                               collection_key_t collection_key,
                               ukv_options_t options,
                               callback_at&& callback) noexcept {

    if constexpr (!std::is_same<set_or_transaction_at, ucset_t>()) {
        bool dont_watch = options & ukv_option_transaction_dont_watch_k;
        if (!dont_watch)
            if (auto watch_status = set_or_transaction.watch(collection_key); !watch_status)
                return watch_status;
    }

    auto find_status = set_or_transaction.find(
        collection_key,
        [&](pair_t const& pair) noexcept { callback(pair.range); },
        [&]() noexcept { callback(value_view_t {}); });
    return find_status;
}

template <typename set_or_transaction_at, typename callback_at>
ucset::status_t scan_and_watch(set_or_transaction_at& set_or_transaction,
                               collection_key_t start,
                               std::size_t range_limit,
                               ukv_options_t options,
                               callback_at&& callback) noexcept {

    std::size_t match_idx = 0;
    collection_key_t previous = start;
    bool reached_end = false;
    auto watch_status = ucset::status_t();
    auto callback_pair = [&](pair_t const& pair) noexcept {
        reached_end = pair.collection_key.collection != previous.collection;
        if (reached_end)
            return;

        if constexpr (!std::is_same<set_or_transaction_at, ucset_t>()) {
            bool dont_watch = options & ukv_option_transaction_dont_watch_k;
            if (!dont_watch)
                if (watch_status = set_or_transaction.watch(pair); !watch_status)
                    return;
        }

        callback(pair);
        previous.key = pair.collection_key.key;
        ++match_idx;
    };

    auto find_status = set_or_transaction.find(start, callback_pair, {});
    if (!find_status)
        return find_status;
    if (!watch_status)
        return watch_status;

    while (match_idx != range_limit && !reached_end) {
        find_status = set_or_transaction.upper_bound(previous, callback_pair, [&]() noexcept { reached_end = true; });
        if (!find_status)
            return find_status;
        if (!watch_status)
            return watch_status;
    }

    return {};
}

template <typename set_or_transaction_at, typename callback_at>
ucset::status_t scan_full(set_or_transaction_at& set_or_transaction, callback_at&& callback) noexcept {

    collection_key_t previous {
        std::numeric_limits<ukv_collection_t>::min(),
        std::numeric_limits<ukv_key_t>::min(),
    };
    while (true) {
        auto callback_pair = [&](pair_t const& pair) noexcept {
            callback(pair);
            previous = pair.collection_key;
        };

        auto reached_end = false;
        auto callback_nothing = [&]() noexcept {
            reached_end = true;
        };

        auto status = set_or_transaction.upper_bound(previous, callback_pair, callback_nothing);
        if (reached_end)
            break;
        if (!status)
            return status;
    }

    return {};
}

/*********************************************************/
/***************** Collections Management ****************/
/*********************************************************/

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
    /**
     * @brief Rarely-used mutex for global reorganizations, like:
     * - Removing existing collections or adding new ones.
     * - Listing present collections.
     */
    std::shared_mutex restructuring_mutex;

    /**
     * @brief Primary database state.
     */
    ucset_t pairs;

    /**
     * @brief A variable-size set of named collections.
     * It's cleaner to implement it with heterogenous lookups as
     * an @c std::unordered_map, but it requires GCC11 and C++20.
     */
    std::map<std::string, ukv_collection_t, string_less_t> names;

    /**
     * @brief Path on disk, from which the data will be read.
     * When closed, we will try saving the DB on disk.
     */
    std::string persisted_directory;

    database_t(ucset_t&& set) noexcept(false) : pairs(std::move(set)) {}

    database_t(database_t&& other) noexcept
        : pairs(std::move(other.pairs)), names(std::move(other.names)),
          persisted_directory(std::move(other.persisted_directory)) {}
};

ukv_collection_t new_collection(database_t& db) noexcept {
    bool is_new = false;
    ukv_collection_t new_handle = ukv_collection_main_k;
    while (!is_new) {
        auto top = static_cast<std::uint64_t>(std::rand());
        auto bottom = static_cast<std::uint64_t>(std::rand());
        new_handle = static_cast<ukv_collection_t>((top << 32) | bottom);
        is_new = new_handle != ukv_collection_main_k;
        for (auto const& [name, existing_handle] : db.names)
            is_new &= new_handle != existing_handle;
    }
    return new_handle;
}

void export_error_code(ucset::status_t code, ukv_error_t* c_error) noexcept {
    if (!code)
        *c_error = "Faced error!";
}

/*********************************************************/
/*****************	 Writing to Disk	  ****************/
/*********************************************************/

void write_collection( //
    database_t const& db,
    ukv_collection_t collection_id,
    std::string const& collection_path,
    ukv_error_t* c_error) noexcept(false) {

    std::shared_ptr<arrow::io::FileOutputStream> out_file;
    PARQUET_ASSIGN_OR_THROW(out_file, arrow::io::FileOutputStream::Open(collection_path));

    parquet::schema::NodeVector columns {};
    columns.push_back(parquet::schema::PrimitiveNode::Make( //
        "key",
        parquet::Repetition::REQUIRED,
        parquet::Type::INT64,
        parquet::ConvertedType::INT_64));
    columns.push_back(parquet::schema::PrimitiveNode::Make( //
        "value",
        parquet::Repetition::OPTIONAL,
        parquet::Type::BYTE_ARRAY,
        parquet::ConvertedType::UTF8));
    auto schema = std::static_pointer_cast<parquet::schema::GroupNode>(
        parquet::schema::GroupNode::Make("schema", parquet::Repetition::REQUIRED, columns));
    parquet::WriterProperties::Builder builder;
    parquet::StreamWriter os {parquet::ParquetFileWriter::Open(out_file, schema, builder.build())};

    collection_key_t min(collection_id, std::numeric_limits<ukv_key_t>::min());
    collection_key_t max(collection_id, std::numeric_limits<ukv_key_t>::max());
    auto status = db.pairs.range(min, max, [&](pair_t& pair) noexcept {
        std::optional<std::string_view> value;
        if (pair.range.size())
            value = std::string_view(pair.range);
        os << pair.collection_key.key << value << parquet::EndRow;
    });
    export_error_code(status, c_error);
    return_if_error_m(c_error);
}

void write(database_t const& db, std::string const& dir_path, ukv_error_t* c_error) noexcept(false) {

    // Check if the source directory even exists
    if (!std::filesystem::is_directory(dir_path))
        return;

    auto main_path = stdfs::path(dir_path) / ".parquet";
    write_collection(db, ukv_collection_main_k, main_path, c_error);
    return_if_error_m(c_error);

    for (auto const& collection : db.names) {
        auto const& collection_name = collection.first;
        auto const collection_path = stdfs::path(dir_path) / (collection_name + ".parquet");
        write_collection(db, collection.second, collection_path, c_error);
        return_if_error_m(c_error);
    }
}

bool ends_with(std::string_view str, std::string_view suffix) noexcept {
    return str.size() >= suffix.size() &&
           0 == str.compare(str.size() - suffix.size(), suffix.size(), suffix.data(), suffix.size());
}

void read(database_t& db, std::string const& path, ukv_error_t* c_error) noexcept(false) {

    // Clear the DB, before refilling it
    db.names.clear();
    auto status = db.pairs.clear();
    export_error_code(status, c_error);
    return_if_error_m(c_error);

    // Check if the source directory even exists
    if (!std::filesystem::is_directory(path))
        return;

    // Loop over all persisted collections, reading them one by one
    std::string_view extension {".parquet"};
    for (auto const& dir_entry : std::filesystem::directory_iterator {path}) {
        auto const& collection_path = dir_entry.path();
        std::string collection_name = collection_path.filename();
        if (!ends_with(collection_name, extension))
            continue;

        collection_name.resize(collection_name.size() - extension.size());
        ukv_collection_t collection_id = collection_name.empty() ? ukv_collection_main_k : new_collection(db);
        if (!collection_name.empty())
            db.names.emplace(collection_name, collection_id);

        std::shared_ptr<arrow::io::ReadableFile> in_file;
        PARQUET_ASSIGN_OR_THROW(in_file, arrow::io::ReadableFile::Open(collection_path));
        parquet::StreamReader os {parquet::ParquetFileReader::Open(in_file)};

        ukv_key_t key;
        std::optional<std::string> value;
        while (!os.eof()) {
            os >> key >> value >> parquet::EndRow;

            pair_t pair;
            pair.collection_key.collection = collection_id;
            pair.collection_key.key = key;

            // Converting to our internal representation would require a copy
            if (value) {
                auto buf_len = value->size();
                auto buf_ptr = blob_allocator_t {}.allocate(buf_len);
                return_error_if_m(buf_ptr != nullptr, c_error, out_of_memory_k, "Failed to allocate a blob");
                pair.range = value_view_t {buf_ptr, buf_len};
                std::memcpy(buf_ptr, value->data(), value->size());
            }
            else
                pair.range = value_view_t::make_empty();

            auto status = db.pairs.upsert(std::move(pair));
            export_error_code(status, c_error);
            return_if_error_m(c_error);
        }
    }
}

/*********************************************************/
/*****************	    C Interface 	  ****************/
/*********************************************************/

void ukv_database_init(ukv_database_init_t* c_ptr) {

    ukv_database_init_t& c = *c_ptr;
    safe_section("Initializing DBMS", c.error, [&] {
        auto maybe_pairs = ucset_t::make();
        return_error_if_m(maybe_pairs, c.error, error_unknown_k, "Couldn't build consistent set");
        auto db = database_t(std::move(maybe_pairs).value());
        auto db_ptr = std::make_unique<database_t>(std::move(db)).release();
        auto len = c.config ? std::strlen(c.config) : 0;
        if (len) {

            // Check if the directory contains a config
            stdfs::path root = c.config;
            stdfs::file_status root_status = stdfs::status(root);
            return_error_if_m(root_status.type() == stdfs::file_type::directory,
                              c.error,
                              args_wrong_k,
                              "Root isn't a directory");
            stdfs::path config_path = stdfs::path(root) / config_name_k;
            stdfs::file_status config_status = stdfs::status(config_path);
            if (config_status.type() == stdfs::file_type::not_found) {
                log_warning_m(
                    "Configuration file is missing under the path %s. "
                    "Default will be used\n",
                    config_path.c_str());
            }
            else {
                std::ifstream ifs(config_path.c_str());
                json_t js = json_t::parse(ifs);
            }

            db_ptr->persisted_directory = std::string(c.config, len);
            read(*db_ptr, db_ptr->persisted_directory, c.error);
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

    // 1. Allocate a tape for all the values to be pulled
    growing_tape_t tape(arena);
    tape.reserve(places.size(), c.error);
    return_if_error_m(c.error);
    auto back_inserter = [&](value_view_t value) noexcept {
        tape.push_back(value, c.error);
    };

    // 2. Pull the data
    for (std::size_t task_idx = 0; task_idx != places.size(); ++task_idx) {
        place_t place = places[task_idx];
        collection_key_t key = place.collection_key();
        auto status = c.transaction //
                          ? find_and_watch(txn, key, c.options, back_inserter)
                          : find_and_watch(db.pairs, key, c.options, back_inserter);
        if (!status)
            return export_error_code(status, c.error);
    }

    // 3. Export the results
    if (c.presences)
        *c.presences = tape.presences().get();
    if (c.offsets)
        *c.offsets = tape.offsets().begin().get();
    if (c.lengths)
        *c.lengths = tape.lengths().begin().get();
    if (c.values)
        *c.values = (ukv_bytes_ptr_t)tape.contents().begin().get();
}

void ukv_write(ukv_write_t* c_ptr) {

    ukv_write_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    if (!c.tasks_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

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

    // Writes are the only operations that significantly differ
    // in terms of transactional and batch operations.
    // The latter will also differ depending on the number
    // pairs you are working with - one or more.
    if (c.transaction) {
        bool dont_watch = c.options & ukv_option_transaction_dont_watch_k;
        for (std::size_t i = 0; i != places.size(); ++i) {
            place_t place = places[i];
            value_view_t content = contents[i];
            collection_key_t key = place.collection_key();
            if (!dont_watch)
                if (auto watch_status = txn.watch(key); !watch_status)
                    return export_error_code(watch_status, c.error);

            ucset::status_t status;
            if (content) {
                pair_t pair {key, content, c.error};
                return_if_error_m(c.error);
                status = txn.upsert(std::move(pair));
            }
            else
                status = txn.erase(key);

            if (!status)
                return export_error_code(status, c.error);
        }
        return;
    }

    // Non-transactional but atomic batch-write operation.
    // It requires producing a copy of input data.
    else if (c.tasks_count > 1) {
        uninitialized_array_gt<pair_t> copies(places.count, arena, c.error);
        return_if_error_m(c.error);
        initialized_range_gt<pair_t> copies_constructed(copies);

        for (std::size_t i = 0; i != places.size(); ++i) {
            place_t place = places[i];
            value_view_t content = contents[i];
            collection_key_t key = place.collection_key();

            pair_t pair {key, content, c.error};
            return_if_error_m(c.error);
            copies[i] = std::move(pair);
        }

        auto status = db.pairs.upsert(std::make_move_iterator(copies.begin()), std::make_move_iterator(copies.end()));
        return export_error_code(status, c.error);
    }

    // Just a single non-batch write
    else {
        place_t place = places[0];
        value_view_t content = contents[0];
        collection_key_t key = place.collection_key();

        pair_t pair {key, content, c.error};
        return_if_error_m(c.error);
        auto status = db.pairs.upsert(std::move(pair));
        return export_error_code(status, c.error);
    }
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

    // 1. Allocate a tape for all the values to be fetched
    auto offsets = arena.alloc_or_dummy(scans.count + 1, c.error, c.offsets);
    return_if_error_m(c.error);
    auto counts = arena.alloc_or_dummy(scans.count, c.error, c.counts);
    return_if_error_m(c.error);

    auto total_keys = reduce_n(scans.limits, scans.count, 0ul);
    auto keys_output = *c.keys = arena.alloc<ukv_key_t>(total_keys, c.error).begin();
    return_if_error_m(c.error);

    // 2. Fetch the data
    for (std::size_t task_idx = 0; task_idx != scans.count; ++task_idx) {
        scan_t scan = scans[task_idx];
        offsets[task_idx] = keys_output - *c.keys;

        ukv_length_t matched_pairs_count = 0;
        auto found_pair = [&](pair_t const& pair) noexcept {
            *keys_output = pair.collection_key.key;
            ++keys_output;
            ++matched_pairs_count;
        };

        auto previous_key = collection_key_t {scan.collection, scan.min_key};
        auto status = c.transaction //
                          ? scan_and_watch(txn, previous_key, scan.limit, c.options, found_pair)
                          : scan_and_watch(db.pairs, previous_key, scan.limit, c.options, found_pair);
        if (!status)
            return export_error_code(status, c.error);

        counts[task_idx] = matched_pairs_count;
    }
    offsets[scans.count] = keys_output - *c.keys;
}

struct key_from_pair_t {
    ukv_key_t* key_ptr;
    key_from_pair_t(ukv_key_t* key) : key_ptr(key) {}
    key_from_pair_t& operator=(pair_t const& pair) {
        *key_ptr = pair.collection_key.key;
        return *this;
    };
};

struct key_iterator_t {
    using value_type = ukv_key_t;
    using pointer = value_type*;
    using reference = value_type&;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::random_access_iterator_tag;

    ukv_key_t* begin_;
    key_iterator_t(ukv_key_t* key) : begin_(key) {}
    key_from_pair_t operator[](std::size_t idx) { return &begin_[idx]; };
};

void ukv_sample(ukv_sample_t* c_ptr) {

    ukv_sample_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    return_error_if_m(!c.transaction, c.error, uninitialized_state_k, "Transaction sampling aren't supported!");
    if (!c.tasks_count)
        return;

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    database_t& db = *reinterpret_cast<database_t*>(c.db);
    strided_iterator_gt<ukv_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ukv_length_t const> lens {c.count_limits, c.count_limits_stride};
    sample_args_t samples {collections, lens, c.tasks_count};

    auto offsets = arena.alloc_or_dummy(samples.count + 1, c.error, c.offsets);
    return_if_error_m(c.error);
    auto counts = arena.alloc_or_dummy(samples.count, c.error, c.counts);
    return_if_error_m(c.error);

    auto total_keys = reduce_n(samples.limits, samples.count, 0ul);
    auto keys_output = *c.keys = arena.alloc<ukv_key_t>(total_keys, c.error).begin();
    return_if_error_m(c.error);

    for (std::size_t task_idx = 0; task_idx != samples.count; ++task_idx) {
        sample_arg_t task = samples[task_idx];
        offsets[task_idx] = keys_output - *c.keys;

        std::random_device random_device;
        std::mt19937 random_generator(random_device());
        std::size_t seen = 0;
        key_iterator_t iter(keys_output);
        collection_key_t min(task.collection, std::numeric_limits<ukv_key_t>::min());
        collection_key_t max(task.collection, std::numeric_limits<ukv_key_t>::max());

        auto status = db.pairs.sample_range(min, max, random_generator, seen, task.limit, iter);
        export_error_code(status, c.error);
        return_if_error_m(c.error);

        counts[task_idx] = task.limit;
        keys_output += task.limit;
    }
    offsets[samples.count] = keys_output - *c.keys;
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

    for (ukv_size_t i = 0; i != c.tasks_count; ++i) {
        auto collection = collections[i];
        ukv_key_t const min_key = start_keys[i];
        ukv_key_t const max_key = end_keys[i];

        collection_key_t min(collection, min_key);
        collection_key_t max(collection, max_key);

        std::size_t cardinality = 0;
        std::size_t value_bytes = 0;
        std::size_t space_usage = 0;
        auto status = db.pairs.range(min, max, [&](pair_t& pair) noexcept {
            ++cardinality;
            value_bytes += pair.range.size();
            space_usage += pair.range.size() + sizeof(pair_t);
        });
        export_error_code(status, c.error);
        return_if_error_m(c.error);

        min_cardinalities[i] = static_cast<ukv_size_t>(cardinality);
        max_cardinalities[i] = std::numeric_limits<ukv_size_t>::max();
        min_value_bytes[i] = value_bytes;
        max_value_bytes[i] = std::numeric_limits<ukv_size_t>::max();
        min_space_usages[i] = space_usage;
        max_space_usages[i] = std::numeric_limits<ukv_size_t>::max();
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
    std::unique_lock _ {db.restructuring_mutex};

    std::string_view collection_name {c.name, name_len};
    auto collection_it = db.names.find(collection_name);
    return_error_if_m(collection_it == db.names.end(), c.error, args_wrong_k, "Such collection already exists!");

    auto new_collection_id = new_collection(db);
    safe_section("Inserting new collection", c.error, [&] { db.names.emplace(collection_name, new_collection_id); });
    *c.id = new_collection_id;
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
    std::unique_lock _ {db.restructuring_mutex};

    if (c.mode == ukv_drop_keys_vals_handle_k) {
        auto status = db.pairs.erase_range(c.id, c.id + 1, no_op_t {});
        if (!status)
            return export_error_code(status, c.error);

        for (auto it = db.names.begin(); it != db.names.end(); ++it) {
            if (c.id != it->second)
                continue;
            db.names.erase(it);
            break;
        }
    }

    else if (c.mode == ukv_drop_keys_vals_k) {
        auto status = db.pairs.erase_range(c.id, c.id + 1, no_op_t {});
        return export_error_code(status, c.error);
    }

    else if (c.mode == ukv_drop_vals_k) {
        auto status = db.pairs.range(c.id, c.id + 1, [&](pair_t& pair) noexcept {
            pair = pair_t {pair.collection_key, value_view_t::make_empty(), nullptr};
        });
        return export_error_code(status, c.error);
    }
}

void ukv_collection_list(ukv_collection_list_t* c_ptr) {

    ukv_collection_list_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    return_error_if_m(c.count && c.names, c.error, args_combo_k, "Need names and outputs!");

    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    database_t& db = *reinterpret_cast<database_t*>(c.db);
    std::shared_lock _ {db.restructuring_mutex};
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
        if (*c.transaction)
            return;

        auto maybe_txn = db.pairs.transaction();
        return_error_if_m(maybe_txn, c.error, error_unknown_k, "Couldn't start a transaction");
        *c.transaction = std::make_unique<transaction_t>(std::move(maybe_txn).value()).release();
    });
    return_if_error_m(c.error);

    transaction_t& txn = *reinterpret_cast<transaction_t*>(*c.transaction);
    auto status = txn.reset();
    return export_error_code(status, c.error);
}

void ukv_transaction_commit(ukv_transaction_commit_t* c_ptr) {

    ukv_transaction_commit_t& c = *c_ptr;
    return_error_if_m(c.db, c.error, uninitialized_state_k, "DataBase is uninitialized");
    database_t& db = *reinterpret_cast<database_t*>(c.db);

    validate_transaction_commit(c.transaction, c.options, c.error);
    return_if_error_m(c.error);
    transaction_t& txn = *reinterpret_cast<transaction_t*>(c.transaction);
    auto status = txn.stage();
    if (!status)
        return export_error_code(status, c.error);
    status = txn.commit();
    if (!status)
        return export_error_code(status, c.error);

    if (c.sequence_number)
        *c.sequence_number = txn.generation();

    // TODO: Degrade the lock to "shared" state before starting expensive IO
    if (c.options & ukv_option_write_flush_k)
        safe_section("Saving to disk", c.error, [&] { write(db, db.persisted_directory, c.error); });
}

/*********************************************************/
/*****************	  Memory Management   ****************/
/*********************************************************/

void ukv_arena_free(ukv_arena_t c_arena) {
    clear_linked_memory(c_arena);
}

void ukv_transaction_free(ukv_transaction_t const c_transaction) {
    if (!c_transaction)
        return;
    transaction_t& txn = *reinterpret_cast<transaction_t*>(c_transaction);
    delete &txn;
}

void ukv_database_free(ukv_database_t c_db) {
    if (!c_db)
        return;

    database_t& db = *reinterpret_cast<database_t*>(c_db);
    if (!db.persisted_directory.empty()) {
        ukv_error_t c_error = nullptr;
        safe_section("Saving to disk", &c_error, [&] { write(db, db.persisted_directory, &c_error); });
    }

    delete &db;
}

void ukv_error_free(ukv_error_t) {
}
