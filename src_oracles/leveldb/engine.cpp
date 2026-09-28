/**
 *  @file src_oracles/leveldb/engine.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Embedded persistent key-value store on LevelDB, with collections as key ranges and without transactions.
 *
 *  LevelDB keeps one sorted keyspace, so every stored key leads with a tag naming its range, and the pairs of each
 *  collection follow the tag as one contiguous run. The pairs of collections keyed by integers and by strings, the
 *  names of collections, the catalog of collections and its counter each take a tag of their own, so no key a caller
 *  passes reaches another range.
 */

#include <cstddef> // `offsetof`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <charconv>     // `std::from_chars`
#include <compare>      // `std::strong_ordering`
#include <limits>       // `std::numeric_limits`
#include <memory>       // `std::unique_ptr`
#include <mutex>        // `std::lock_guard`, `std::mutex`
#include <new>          // `std::nothrow`
#include <span>         // `std::span`
#include <string>       // `std::string`, the path, property and value type LevelDB takes
#include <string_view>  // `std::string_view`
#include <system_error> // `std::errc`

#include <fmt/format.h> // `fmt::format_to_n`
#include <leveldb/comparator.h>
#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include <smashtable/basic_hash_table.hpp>

#include <ustore/db.h>
#include <ustore/ranges_args.hpp> // `places_arg_t`, `strings_arg_t`

#include "engine.hpp"                 // `engine_t`, `handle_t`, `handle_cast`
#include "helpers/blocking_files.hpp" // `directory_exists`
#include "helpers/full_scan.hpp"      // `reservoir_sample_iterator`
#include "helpers/linked_array.hpp"   // `growing_tape_t`, `uninitialized_array`

namespace unum::ustore::leveldb {

#pragma region Database

/** The open LevelDB database. */
using native_t = ::leveldb::DB;

/** Status LevelDB answers. */
using native_status_t = ::leveldb::Status;

/** An owned LevelDB iterator. */
using iterator_pointer_t = std::unique_ptr<::leveldb::Iterator>;

/**
 *  @brief The range of the keyspace a stored key lies in, which its leading byte names and which orders the ranges.
 *
 *  @code{.c}
 *  struct integers { uint8_t tag; uint64_t collection; int64_t key; };  // 17 bytes, the value of the pair
 *  struct strings  { uint8_t tag; uint64_t collection; char string[]; }; // 9 bytes and up, the value of the pair
 *  struct names    { uint8_t tag; char name[]; };                       // valued by a `name_record_t`
 *  struct catalog  { uint8_t tag; uint64_t collection; };               // valued by its modality byte and its name
 *  struct counter  { uint8_t tag; };                                    // valued by the next collection's identifier
 *  @endcode
 */
enum class tag_t : std::uint8_t {
    integers_k,
    strings_k,
    names_k,
    catalog_k,
    counter_k,
};

/** Bytes a stored key spends on its tag and its collection, ahead of the key or string within the collection. */
constexpr std::size_t head_bytes_k = 1 + sizeof(ustore_collection_t);

/** A stored key of @p bytes_ bytes, which a slice may view for as long as it lives. */
template <std::size_t bytes_>
struct fixed_key {
    char bytes[bytes_] {};

    [[nodiscard]] ::leveldb::Slice slice() const noexcept { return {bytes, bytes_}; }
};

/** A stored key of a pair of an integer-keyed collection. */
using integer_key_t = fixed_key<head_bytes_k + sizeof(ustore_key_t)>;

/** A stored key of a tag and a collection alone. */
using head_key_t = fixed_key<head_bytes_k>;

/** A stored key of a tag alone. */
using tag_key_t = fixed_key<1>;

/** The tag and the collection alone, which a seek to the first stored key of @p collection in @p tag takes. */
inline head_key_t head_key(tag_t tag, ustore_collection_t collection) noexcept {
    head_key_t stored;
    stored.bytes[0] = static_cast<char>(tag);
    std::memcpy(stored.bytes + 1, &collection, sizeof(collection));
    return stored;
}

/** The stored key of @p key in @p collection, which is keyed by integers. */
inline integer_key_t integer_key(ustore_collection_t collection, ustore_key_t key) noexcept {
    integer_key_t stored;
    stored.bytes[0] = static_cast<char>(tag_t::integers_k);
    std::memcpy(stored.bytes + 1, &collection, sizeof(collection));
    std::memcpy(stored.bytes + head_bytes_k, &key, sizeof(key));
    return stored;
}

/** Writes into @p stored the stored key of @p text in @p collection, which is keyed by strings. */
inline ::leveldb::Slice string_key(ustore_collection_t collection, std::string_view text, std::string& stored) {
    head_key_t const head = head_key(tag_t::strings_k, collection);
    stored.assign(head.bytes, head_bytes_k);
    stored.append(text);
    return stored;
}

/** Writes into @p stored the stored key of the collection name @p name. */
inline ::leveldb::Slice name_key(std::string_view name, std::string& stored) {
    stored.assign(1, static_cast<char>(tag_t::names_k));
    stored.append(name);
    return stored;
}

/** The stored key of the counter every created collection takes its identifier from. */
constexpr tag_key_t counter_key_k {{static_cast<char>(tag_t::counter_k)}};

inline tag_t tag_of(::leveldb::Slice const& stored) noexcept { return static_cast<tag_t>(stored[0]); }

/** The collection @p stored belongs to, whose bytes LevelDB leaves unaligned. */
inline ustore_collection_t collection_of(::leveldb::Slice const& stored) noexcept {
    ustore_collection_t collection;
    std::memcpy(&collection, stored.data() + 1, sizeof(collection));
    return collection;
}

/** The integer key @p stored holds within its collection. */
inline ustore_key_t key_of(::leveldb::Slice const& stored) noexcept {
    ustore_key_t key;
    std::memcpy(&key, stored.data() + head_bytes_k, sizeof(key));
    return key;
}

/** What follows the head of @p stored, from @p head bytes in: the string of a strings key or the text of a name. */
inline ::leveldb::Slice tail_of(::leveldb::Slice const& stored, std::size_t head = head_bytes_k) noexcept {
    return {stored.data() + head, stored.size() - head};
}

/** Whether @p stored lies in the run of @p collection within the range of @p tag. */
inline bool in_run(::leveldb::Slice const& stored, tag_t tag, ustore_collection_t collection) noexcept {
    return tag_of(stored) == tag && collection_of(stored) == collection;
}

/** The range holding the pairs of a collection keyed as @p keying. */
constexpr tag_t pairs_tag(keying_t keying) noexcept {
    return keying == keying_t::strings_k ? tag_t::strings_k : tag_t::integers_k;
}

/** Orders stored keys by tag, then by collection, then integer keys as integers and everything else as bytes. */
struct key_comparator_t final : public ::leveldb::Comparator {

    int Compare(::leveldb::Slice const& left, ::leveldb::Slice const& right) const override {
        tag_t const left_tag = tag_of(left);
        tag_t const right_tag = tag_of(right);
        if (left_tag != right_tag) return left_tag < right_tag ? -1 : 1;
        if (left_tag == tag_t::names_k || left_tag == tag_t::counter_k) return left.compare(right);

        ustore_collection_t const left_collection = collection_of(left);
        ustore_collection_t const right_collection = collection_of(right);
        if (left_collection != right_collection) return left_collection < right_collection ? -1 : 1;

        // A bare head is shorter than every integer key of its collection, so it seeks ahead of all of them
        if (left_tag != tag_t::integers_k || left.size() != right.size()) return tail_of(left).compare(tail_of(right));
        ustore_key_t const left_key = key_of(left);
        ustore_key_t const right_key = key_of(right);
        return left_key < right_key ? -1 : left_key > right_key ? 1 : 0;
    }

    char const* Name() const override { return "ustore.tagged_key"; }

    void FindShortestSeparator(std::string*, ::leveldb::Slice const&) const override {}

    /** Leaves @p key as it is, since LevelDB keeps a successor only when it is physically shorter. */
    void FindShortSuccessor(std::string*) const override {}
};

key_comparator_t const key_comparator_k {};

/** Walks the keys of one integer-keyed collection over @p base, handing the reservoir sampler the key of each. */
class collection_walk_t {
    ::leveldb::Iterator& base_;
    ustore_collection_t collection_;

  public:
    collection_walk_t(::leveldb::Iterator& base, ustore_collection_t collection) noexcept
        : base_(base), collection_(collection) {}

    collection_walk_t* operator->() noexcept { return this; }

    void SeekToFirst() {
        head_key_t const first = head_key(tag_t::integers_k, collection_);
        base_.Seek(first.slice());
    }

    void Next() { base_.Next(); }

    [[nodiscard]] bool Valid() const noexcept {
        return base_.Valid() && in_run(base_.key(), tag_t::integers_k, collection_);
    }

    [[nodiscard]] ::leveldb::Slice key() const noexcept { return tail_of(base_.key()); }
};

/** A snapshot handle, whose address is its identifier. */
struct snapshot_t {

    /** The LevelDB snapshot. */
    ::leveldb::Snapshot const* native = nullptr;
};

/** Identifiers of the live snapshots. */
using snapshots_t = st::hash_set<ustore_snapshot_t>;

/** The calls this engine serves, which stamp every handle it hands out. */
extern engine_t const engine_k;

/** The open database, its options and its snapshots. */
struct database_t : handle_t {

    /** Options the database opened with, reused to create exports. */
    ::leveldb::Options options;

    /** Identifiers of the live snapshots. */
    snapshots_t snapshots;

    /** The open database. */
    std::unique_ptr<native_t> native;

    /** Guards the snapshots. */
    std::mutex mutex;

    /** Serializes changes to the catalog and the names, so a name is checked and taken in one step. */
    std::mutex catalog_mutex;
};

inline ::leveldb::Slice to_slice(value_view_t value) noexcept {
    return {reinterpret_cast<char const*>(value.begin()), value.size()};
}

inline value_view_t to_value(::leveldb::Slice const& slice) noexcept {
    return {reinterpret_cast<ustore_bytes_cptr_t>(slice.data()), static_cast<ustore_length_t>(slice.size())};
}

/** Exports a failed status into @p error, answering whether it failed. */
bool export_error(native_status_t const& status, ustore_error_t* error) noexcept {
    if (status.ok()) return false;

    if (status.IsCorruption()) *error = {ustore_error_malformed_k, "LevelDB found corrupted data"};
    else if (status.IsIOError()) *error = {ustore_error_io_k, "LevelDB failed an I/O operation"};
    else if (status.IsInvalidArgument()) *error = {ustore_error_invalid_argument_k, "LevelDB rejected an argument"};
    else if (status.IsNotFound()) *error = {ustore_error_not_found_k, "LevelDB found no such entry"};
    else if (status.IsNotSupportedError())
        *error = {ustore_error_unsupported_k, "LevelDB does not support the operation"};
    else *error = {ustore_error_internal_k, "LevelDB failed"};
    return true;
}

/** Reads what @p collection holds, answering false after reporting a collection the catalog does not list. */
bool modality_of(database_t& database, ustore_collection_t collection, ustore_modality_t& modality,
                 ustore_error_t* error) {
    modality = ustore_modality_any_k;
    if (collection == ustore_collection_main_k) return true;

    std::string held;
    head_key_t const entry = head_key(tag_t::catalog_k, collection);
    native_status_t const status = database.native->Get(::leveldb::ReadOptions(), entry.slice(), &held);
    if (status.IsNotFound()) {
        *error = {ustore_error_not_found_k, "No such collection"};
        return false;
    }
    if (export_error(status, error)) return false;
    if (held.empty()) {
        *error = {ustore_error_malformed_k, "The catalog entry of a collection is malformed"};
        return false;
    }
    modality = static_cast<ustore_modality_t>(held[0]);
    return true;
}

/** Refuses @p collection unless the catalog lists it keyed as @p keying, answering whether it refused. */
bool refuse_collection(database_t& database, ustore_collection_t collection, keying_t keying, ustore_error_t* error) {
    ustore_modality_t modality = ustore_modality_any_k;
    if (!modality_of(database, collection, modality, error)) return true;
    if (keying_of(modality) == keying) return false;
    *error = {ustore_error_invalid_argument_k,
              keying == keying_t::strings_k ? "The collection holds no strings" : "The collection holds strings"};
    return true;
}

/** Refuses a batch addressing a collection the catalog does not list keyed as @p keying, checking each run of tasks
 *  on one collection once. */
bool refuse_collections(database_t& database, strided_iterator<ustore_collection_t const> collections,
                        std::size_t count, keying_t keying, ustore_error_t* error) {
    if (!collections) return count && refuse_collection(database, ustore_collection_main_k, keying, error);
    for (std::size_t task_index = 0; task_index != count; ++task_index) {
        bool const repeated = task_index != 0 && collections[task_index] == collections[task_index - 1];
        if (!repeated && refuse_collection(database, collections[task_index], keying, error)) return true;
    }
    return false;
}

/** Points @p options at the snapshot @p id, leaving them untouched for the zero identifier. */
void use_snapshot(database_t& database, ustore_snapshot_t id, ::leveldb::ReadOptions& options, ustore_error_t* error) {
    if (!id) return;
    std::lock_guard lock {database.mutex};
    if (database.snapshots.find(id) == database.snapshots.end()) {
        *error = {ustore_error_not_found_k, "The snapshot doesn't exist"};
        return;
    }
    options.snapshot = reinterpret_cast<snapshot_t*>(id)->native;
}

#pragma endregion Database

#pragma region Reads and Writes

/** From this many keys one iterator, sharing a version and its loaded blocks, beats a `Get` that copies each value. */
constexpr std::size_t iterator_read_threshold_k = 16;

void write_one(database_t& database, places_arg_t const& places, contents_arg_t const& contents,
               ::leveldb::WriteOptions const& options, ustore_error_t* error) {

    auto place = places[0];
    auto content = contents[0];
    integer_key_t const key = integer_key(place.collection, place.key);
    native_status_t const status = !content ? database.native->Delete(options, key.slice())
                                            : database.native->Put(options, key.slice(), to_slice(content));
    export_error(status, error);
}

void write_many(database_t& database, places_arg_t const& places, contents_arg_t const& contents,
                ::leveldb::WriteOptions const& options, ustore_error_t* error) {

    ::leveldb::WriteBatch batch;
    for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
        auto place = places[task_index];
        auto content = contents[task_index];
        integer_key_t const key = integer_key(place.collection, place.key);
        if (!content) batch.Delete(key.slice());
        else batch.Put(key.slice(), to_slice(content));
    }

    native_status_t const status = database.native->Write(options, &batch);
    export_error(status, error);
}

/** Reads every place with its own `Get`, handing each value to @p enumerator with its index. */
template <typename value_enumerator_type_>
void get_each(database_t& database, places_arg_t const& places, ::leveldb::ReadOptions const& options,
              value_enumerator_type_&& enumerator, ustore_error_t* error) {

    // `Get` writes only into a string, so one buffer serves the whole batch
    std::string value;
    for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
        integer_key_t const key = integer_key(places[task_index].collection, places[task_index].key);
        native_status_t const status = database.native->Get(options, key.slice(), &value);
        if (status.IsNotFound()) {
            enumerator(task_index, value_view_t {});
            continue;
        }
        if (export_error(status, error)) return;
        enumerator(task_index, to_value(value));
    }
}

/** Reads every place through one iterator, handing each value to @p enumerator straight from its block. */
template <typename value_enumerator_type_>
void seek_each(database_t& database, places_arg_t const& places, ::leveldb::ReadOptions const& options,
               value_enumerator_type_&& enumerator, ustore_error_t* error) {

    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (std::size_t task_index = 0; task_index != places.size(); ++task_index) {
        integer_key_t const key = integer_key(places[task_index].collection, places[task_index].key);
        iterator->Seek(key.slice());
        if (iterator->Valid() && iterator->key().compare(key.slice()) == 0) {
            enumerator(task_index, to_value(iterator->value()));
            continue;
        }
        if (export_error(iterator->status(), error)) return;
        enumerator(task_index, value_view_t {});
    }
}

#pragma endregion Reads and Writes

#pragma region Lifetime

void database_init(ustore_database_init_leveldb_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the database"};
        return;
    }
    if (!arguments.directory) {
        *arguments.error = {ustore_error_invalid_argument_k, "A database needs a directory"};
        return;
    }

    auto [root_exists, directory_exists_status] = directory_exists(arguments.directory);
    if (directory_exists_status.failed()) {
        *arguments.error = directory_exists_status;
        return;
    }
    if (!root_exists) {
        *arguments.error = {ustore_error_not_found_k, "Root isn't a directory"};
        return;
    }

    std::unique_ptr<database_t> database {new (std::nothrow) database_t};
    if (!database) {
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to allocate the database"};
        return;
    }
    database->options.comparator = &key_comparator_k;
    database->options.compression = ::leveldb::kNoCompression;
    database->options.create_if_missing = true;

    native_t* native = nullptr;
    native_status_t const status = native_t::Open(database->options, std::string(arguments.directory), &native);
    if (!status.ok()) {
        *arguments.error = {ustore_error_io_k, "Failed to open LevelDB"};
        return;
    }
    database->native.reset(native);
    database->engine = &engine_k;
    *arguments.db = static_cast<handle_t*>(database.release());
}

void database_capabilities(ustore_database_capabilities_t& arguments) {
    *arguments.capabilities = ustore_capabilities_t(ustore_supports_collections_k | ustore_supports_snapshots_k);
}

#pragma endregion Lifetime

#pragma region Snapshots

void snapshot_list(ustore_snapshot_list_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.count || !arguments.ids) {
        *arguments.error = {ustore_error_invalid_argument_k, "Null outputs specified"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    std::lock_guard lock {database.mutex};
    std::size_t const snapshots_count = database.snapshots.size();
    *arguments.count = static_cast<ustore_size_t>(snapshots_count);

    auto ids = arena.alloc_or_dummy(snapshots_count, arguments.error, arguments.ids);
    if (arguments.error->status) return;

    std::size_t index = 0;
    [[maybe_unused]] auto const walked = database.snapshots.for_each(
        [&](auto const& slot) noexcept { ids[index++] = slot.key(); });
}

void snapshot_create(ustore_snapshot_create_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }

    database_t& database = *handle_cast<database_t>(arguments.db);
    std::lock_guard lock {database.mutex};
    if (database.snapshots.find(*arguments.id) != database.snapshots.end()) {
        *arguments.error = {ustore_error_already_exists_k, "Such snapshot already exists"};
        return;
    }

    std::unique_ptr<snapshot_t> snapshot {new (std::nothrow) snapshot_t};
    if (!snapshot) {
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to allocate the snapshot handle"};
        return;
    }
    snapshot->native = database.native->GetSnapshot();
    if (!snapshot->native) {
        *arguments.error = {ustore_error_internal_k, "Failed to take a snapshot"};
        return;
    }

    auto const id = reinterpret_cast<ustore_snapshot_t>(snapshot.get());
    if (st::failed(database.snapshots.upsert(id))) {
        database.native->ReleaseSnapshot(snapshot->native);
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to register the snapshot"};
        return;
    }
    [[maybe_unused]] snapshot_t* const registered = snapshot.release();
    *arguments.id = id;
}

void snapshot_export(ustore_snapshot_export_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    database_t& database = *handle_cast<database_t>(arguments.db);

    ::leveldb::ReadOptions scan_options;
    scan_options.fill_cache = false;
    use_snapshot(database, arguments.id, scan_options, arguments.error);
    if (arguments.error->status) return;

    native_t* exported_native = nullptr;
    native_status_t status = native_t::Open(database.options, arguments.path, &exported_native);
    if (!status.ok()) {
        *arguments.error = {ustore_error_io_k, "Failed to create the exported database"};
        return;
    }
    std::unique_ptr<native_t> exported {exported_native};

    // Batches the size of a memtable keep writes few without holding the whole export in memory
    ::leveldb::WriteBatch batch;
    iterator_pointer_t iterator {database.native->NewIterator(scan_options)};
    for (iterator->SeekToFirst(); iterator->Valid() && status.ok(); iterator->Next()) {
        batch.Put(iterator->key(), iterator->value());
        if (batch.ApproximateSize() < database.options.write_buffer_size) continue;
        status = exported->Write(::leveldb::WriteOptions(), &batch);
        batch.Clear();
    }
    if (status.ok()) status = iterator->status();
    if (status.ok()) status = exported->Write(::leveldb::WriteOptions(), &batch);
    iterator.reset();
    exported.reset();

    if (!status.ok()) ::leveldb::DestroyDB(arguments.path, database.options);
    export_error(status, arguments.error);
}

void snapshot_drop(ustore_snapshot_drop_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    database_t& database = *handle_cast<database_t>(arguments.db);
    std::lock_guard lock {database.mutex};
    if (!database.snapshots.erase(arguments.id)) {
        *arguments.error = {ustore_error_not_found_k, "The snapshot doesn't exist"};
        return;
    }

    snapshot_t* const snapshot = reinterpret_cast<snapshot_t*>(arguments.id);
    database.native->ReleaseSnapshot(snapshot->native);
    delete snapshot;
}

#pragma endregion Snapshots

#pragma region Reads and Writes

void write(ustore_write_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }

    database_t& database = *handle_cast<database_t>(arguments.db);
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> keys {arguments.keys, arguments.keys_stride};
    strided_iterator<ustore_bytes_cptr_t const> values {arguments.values, arguments.values_stride};
    strided_iterator<ustore_length_t const> offsets {arguments.offsets, arguments.offsets_stride};
    strided_iterator<ustore_length_t const> lengths {arguments.lengths, arguments.lengths_stride};
    bits_view_t presences {arguments.presences};

    places_arg_t places {collections, keys, {}, arguments.tasks_count};
    contents_arg_t contents {presences, offsets, lengths, values, arguments.tasks_count};

    validate_write(arguments.transaction, places, contents, arguments.options, arguments.error);
    if (arguments.error->status) return;
    if (!arguments.tasks_count) return;
    if (refuse_collections(database, collections, arguments.tasks_count, keying_t::integers_k, arguments.error)) return;

    ::leveldb::WriteOptions options;
    options.sync = arguments.options & ustore_option_write_flush_k;

    if (arguments.tasks_count == 1) write_one(database, places, contents, options, arguments.error);
    else write_many(database, places, contents, options, arguments.error);
}

void read(ustore_read_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.tasks_count) return;

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> keys {arguments.keys, arguments.keys_stride};
    places_arg_t places {collections, keys, {}, arguments.tasks_count};

    validate_read(arguments.transaction, places, arguments.options, arguments.error);
    if (arguments.error->status) return;
    if (refuse_collections(database, collections, arguments.tasks_count, keying_t::integers_k, arguments.error)) return;

    ::leveldb::ReadOptions options;
    use_snapshot(database, arguments.snapshot, options, arguments.error);
    if (arguments.error->status) return;

    auto offsets = arena.alloc_or_dummy(places.count + 1, arguments.error, arguments.offsets);
    if (arguments.error->status) return;
    auto lengths = arena.alloc_or_dummy(places.count, arguments.error, arguments.lengths);
    if (arguments.error->status) return;
    auto presences = arena.alloc_or_dummy(places.count, arguments.error, arguments.presences);
    if (arguments.error->status) return;
    bool const needs_export = arguments.values != nullptr;

    uninitialized_array<byte_t> contents(arena);

    // Metadata and data come in one pass, as reading from disk is expensive
    auto data_enumerator = [&](std::size_t index, value_view_t value) {
        presences[index] = bool(value);
        lengths[index] = value ? value.size() : ustore_length_missing_k;
        offsets[index] = contents.size();
        if (needs_export) contents.insert(contents.size(), value.begin(), value.end(), arguments.error);
    };
    if (places.count < iterator_read_threshold_k) get_each(database, places, options, data_enumerator, arguments.error);
    else seek_each(database, places, options, data_enumerator, arguments.error);
    offsets[places.count] = contents.size();
    if (needs_export) *arguments.values = reinterpret_cast<ustore_bytes_ptr_t>(contents.begin());
}

void scan(ustore_scan_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> start_keys {arguments.start_keys, arguments.start_keys_stride};
    strided_iterator<ustore_length_t const> limits {arguments.count_limits, arguments.count_limits_stride};
    scans_arg_t scans {collections, start_keys, limits, arguments.tasks_count};

    validate_scan(arguments.transaction, scans, arguments.options, arguments.error);
    if (arguments.error->status) return;
    if (refuse_collections(database, collections, arguments.tasks_count, keying_t::integers_k, arguments.error)) return;

    auto offsets = arena.alloc_or_dummy(scans.count + 1, arguments.error, arguments.offsets);
    if (arguments.error->status) return;
    auto counts = arena.alloc_or_dummy(scans.count, arguments.error, arguments.counts);
    if (arguments.error->status) return;

    auto const total_keys = reduce_n(scans.limits, scans.count, 0ul);
    auto keys_output = *arguments.keys = arena.alloc<ustore_key_t>(total_keys, arguments.error).data();
    if (arguments.error->status) return;

    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(database, arguments.snapshot, options, arguments.error);
    if (arguments.error->status) return;

    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (ustore_size_t task_index = 0; task_index != arguments.tasks_count; ++task_index) {
        scan_t task = scans[task_index];
        integer_key_t const start = integer_key(task.collection, task.min_key);
        iterator->Seek(start.slice());
        offsets[task_index] = keys_output - *arguments.keys;

        ustore_size_t matched_count = 0;
        while (iterator->Valid() && in_run(iterator->key(), tag_t::integers_k, task.collection) &&
               matched_count != task.limit) {
            *keys_output = key_of(iterator->key());
            ++keys_output;
            ++matched_count;
            iterator->Next();
        }
        if (export_error(iterator->status(), arguments.error)) return;

        counts[task_index] = matched_count;
    }

    offsets[scans.size()] = keys_output - *arguments.keys;
}

void sample(ustore_sample_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.tasks_count) return;

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_length_t const> limits {arguments.count_limits, arguments.count_limits_stride};
    sample_args_t samples {collections, limits, arguments.tasks_count};
    if (refuse_collections(database, collections, arguments.tasks_count, keying_t::integers_k, arguments.error)) return;

    auto offsets = arena.alloc_or_dummy(samples.count + 1, arguments.error, arguments.offsets);
    if (arguments.error->status) return;
    auto counts = arena.alloc_or_dummy(samples.count, arguments.error, arguments.counts);
    if (arguments.error->status) return;

    auto const total_keys = reduce_n(samples.limits, samples.count, 0ul);
    auto keys_output = *arguments.keys = arena.alloc<ustore_key_t>(total_keys, arguments.error).data();
    if (arguments.error->status) return;

    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(database, arguments.snapshot, options, arguments.error);
    if (arguments.error->status) return;

    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (std::size_t task_index = 0; task_index != samples.count; ++task_index) {
        sample_arg_t task = samples[task_index];
        offsets[task_index] = keys_output - *arguments.keys;

        collection_walk_t walk {*iterator, task.collection};
        std::size_t const drawn = reservoir_sample_iterator(walk, std::span {keys_output, task.limit});
        if (export_error(iterator->status(), arguments.error)) return;

        counts[task_index] = static_cast<ustore_length_t>(drawn);
        keys_output += drawn;
    }
    offsets[samples.count] = keys_output - *arguments.keys;
}

void measure(ustore_measure_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    auto min_cardinalities = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.min_cardinalities);
    auto max_cardinalities = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.max_cardinalities);
    auto min_value_bytes = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.min_value_bytes);
    auto max_value_bytes = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.max_value_bytes);
    auto min_space_usages = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.min_space_usages);
    auto max_space_usages = arena.alloc_or_dummy(arguments.tasks_count, arguments.error, arguments.max_space_usages);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    strided_iterator<ustore_collection_t const> collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> start_keys {arguments.start_keys, arguments.start_keys_stride};
    strided_iterator<ustore_key_t const> end_keys {arguments.end_keys, arguments.end_keys_stride};
    if (refuse_collections(database, collections, arguments.tasks_count, keying_t::integers_k, arguments.error)) return;

    // Memory usage describes the whole database, so one property read serves every task
    std::uint64_t memory_bytes = 0;
    std::string memory_usage;
    bool const reported = database.native->GetProperty("leveldb.approximate-memory-usage", &memory_usage);
    char const* const usage_end = memory_usage.data() + memory_usage.size();
    auto const [parsed_end, parse_error] = std::from_chars(memory_usage.data(), usage_end, memory_bytes);
    if (!reported || parse_error != std::errc {} || parsed_end != usage_end) {
        *arguments.error = {ustore_error_internal_k, "LevelDB reported no readable memory usage"};
        return;
    }

    for (ustore_size_t task_index = 0; task_index != arguments.tasks_count; ++task_index) {
        min_cardinalities[task_index] = static_cast<ustore_size_t>(0);
        max_cardinalities[task_index] = static_cast<ustore_size_t>(0);
        min_value_bytes[task_index] = static_cast<ustore_size_t>(0);
        max_value_bytes[task_index] = static_cast<ustore_size_t>(0);

        ustore_collection_t const collection = collections ? collections[task_index] : ustore_collection_main_k;
        integer_key_t const min_key = integer_key(collection, start_keys[task_index]);
        integer_key_t const max_key = integer_key(collection, end_keys[task_index]);
        ::leveldb::Range const range(min_key.slice(), max_key.slice());
        std::uint64_t approximate_size = 0;
        database.native->GetApproximateSizes(&range, 1, &approximate_size);
        min_space_usages[task_index] = approximate_size;
        max_space_usages[task_index] = memory_bytes;
    }
}

#pragma endregion Reads and Writes

#pragma region Collections

/** Clears the run of @p collection, keyed as @p keying, as @p mode asks, in batches the size of a memtable, the last
 *  one synced. */
native_status_t clear_collection(database_t& database, ustore_collection_t collection, keying_t keying,
                                 ustore_drop_mode_t mode) {
    ::leveldb::WriteOptions buffered;
    ::leveldb::WriteOptions synced;
    synced.sync = true;
    ::leveldb::WriteBatch batch;
    native_status_t status;
    tag_t const tag = pairs_tag(keying);
    head_key_t const first = head_key(tag, collection);
    iterator_pointer_t iterator {database.native->NewIterator(::leveldb::ReadOptions())};
    for (iterator->Seek(first.slice()); status.ok() && iterator->Valid(); iterator->Next()) {
        if (!in_run(iterator->key(), tag, collection)) break;
        if (mode == ustore_drop_vals_k) batch.Put(iterator->key(), ::leveldb::Slice());
        else batch.Delete(iterator->key());
        if (batch.ApproximateSize() < database.options.write_buffer_size) continue;
        status = database.native->Write(buffered, &batch);
        batch.Clear();
    }
    if (status.ok()) status = iterator->status();
    if (status.ok()) status = database.native->Write(synced, &batch);
    return status;
}

/** Adds a collection holding @p modality under @p name to @p batch, answering its identifier, or the main one after
 *  reporting why. The catalog entry's value is `struct { uint8_t modality; char name[]; }`, the name empty for none.
 *  The caller holds @c catalog_mutex until the batch is written. */
ustore_collection_t add_collection(database_t& database, ustore_modality_t modality, std::string_view name,
                                   ::leveldb::WriteBatch& batch, ustore_error_t* error) {
    if (modality > ustore_modality_pages_k) {
        *error = {ustore_error_invalid_argument_k, "Unknown modality"};
        return ustore_collection_main_k;
    }

    std::string stored;
    native_status_t const read = database.native->Get(::leveldb::ReadOptions(), counter_key_k.slice(), &stored);
    if (!read.IsNotFound() && export_error(read, error)) return ustore_collection_main_k;
    ustore_collection_t id = 1;
    if (read.ok() && stored.size() != sizeof(id)) {
        *error = {ustore_error_malformed_k, "The collection counter is malformed"};
        return ustore_collection_main_k;
    }
    if (read.ok()) std::memcpy(&id, stored.data(), sizeof(id));

    ustore_collection_t const next = id + 1;
    std::string held(1, static_cast<char>(modality));
    held.append(name);
    head_key_t const entry = head_key(tag_t::catalog_k, id);
    batch.Put(entry.slice(), held);
    batch.Put(counter_key_k.slice(), ::leveldb::Slice(reinterpret_cast<char const*>(&next), sizeof(next)));
    return id;
}

/** Drops @p collection, keyed as @p keying, forgetting first and durably its catalog entry and the name the entry
 *  records, answering whether it did. The caller holds @c catalog_mutex. */
bool drop_collection(database_t& database, ustore_collection_t collection, keying_t keying, ustore_error_t* error) {
    head_key_t const entry = head_key(tag_t::catalog_k, collection);
    std::string held;
    if (export_error(database.native->Get(::leveldb::ReadOptions(), entry.slice(), &held), error)) return false;

    // A crash between the two leaves an unlisted run behind, never a listed or named collection that is gone
    ::leveldb::WriteBatch forgotten;
    forgotten.Delete(entry.slice());
    std::string key;
    if (held.size() > 1) forgotten.Delete(name_key(std::string_view {held}.substr(1), key));
    ::leveldb::WriteOptions synced;
    synced.sync = true;
    if (export_error(database.native->Write(synced, &forgotten), error)) return false;
    return !export_error(clear_collection(database, collection, keying, ustore_drop_keys_vals_k), error);
}

void collection_create(ustore_collection_create_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.id) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the collection"};
        return;
    }
    std::string_view const name = string_of(arguments.name, arguments.name_length);
    if (arguments.name && name.empty()) {
        *arguments.error = {ustore_error_invalid_argument_k, "A name is never empty, as the main collection has none"};
        return;
    }

    // The entry, the counter and the name reach the log in one batch, so a crash keeps all three or none
    database_t& database = *handle_cast<database_t>(arguments.db);
    std::lock_guard lock {database.catalog_mutex};
    std::string key;
    if (!name.empty()) {
        std::string record;
        native_status_t const looked_up = database.native->Get(::leveldb::ReadOptions(), name_key(name, key), &record);
        if (looked_up.ok()) {
            *arguments.error = {ustore_error_already_exists_k, "A collection already carries the name"};
            return;
        }
        if (!looked_up.IsNotFound() && export_error(looked_up, arguments.error)) return;
    }
    ::leveldb::WriteBatch batch;
    ustore_collection_t const id = add_collection(database, arguments.modality, name, batch, arguments.error);
    if (id == ustore_collection_main_k) return;
    name_record_t const named {id, arguments.modality};
    if (!name.empty()) batch.Put(key, ::leveldb::Slice(named.bytes, sizeof(named.bytes)));
    ::leveldb::WriteOptions synced;
    synced.sync = true;
    if (export_error(database.native->Write(synced, &batch), arguments.error)) return;
    *arguments.id = id;
}

void collection_drop(ustore_collection_drop_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (arguments.mode != ustore_drop_keys_vals_handle_k && arguments.mode != ustore_drop_keys_vals_k &&
        arguments.mode != ustore_drop_vals_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "Unknown drop mode"};
        return;
    }
    if (arguments.id == ustore_collection_main_k && arguments.mode == ustore_drop_keys_vals_handle_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "The main collection can only be cleared"};
        return;
    }

    database_t& database = *handle_cast<database_t>(arguments.db);
    ustore_modality_t modality = ustore_modality_any_k;
    if (!modality_of(database, arguments.id, modality, arguments.error)) return;
    if (arguments.mode != ustore_drop_keys_vals_handle_k) {
        export_error(clear_collection(database, arguments.id, keying_of(modality), arguments.mode), arguments.error);
        return;
    }
    std::lock_guard lock {database.catalog_mutex};
    drop_collection(database, arguments.id, keying_of(modality), arguments.error);
}

void collection_list(ustore_collection_list_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.count) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the count"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    database_t& database = *handle_cast<database_t>(arguments.db);
    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(database, arguments.snapshot, options, arguments.error);
    if (arguments.error->status) return;

    // The main collection is never listed, so its entry is where the catalog starts
    head_key_t const first = head_key(tag_t::catalog_k, ustore_collection_main_k);
    iterator_pointer_t iterator {database.native->NewIterator(options)};
    uninitialized_array<ustore_collection_t> ids {arena};
    uninitialized_array<ustore_modality_t> modalities {arena};
    growing_tape_t names {arena};
    for (iterator->Seek(first.slice()); iterator->Valid() && tag_of(iterator->key()) == tag_t::catalog_k;
         iterator->Next()) {
        ::leveldb::Slice const held = iterator->value();
        if (held.empty()) {
            *arguments.error = {ustore_error_malformed_k, "The catalog entry of a collection is malformed"};
            return;
        }
        ids.push_back(collection_of(iterator->key()), arguments.error);
        modalities.push_back(static_cast<ustore_modality_t>(held[0]), arguments.error);
        names.push_back(held.size() == 1 ? value_view_t {} : to_value(tail_of(held, 1)), arguments.error);
        if (arguments.error->status) return;
    }
    if (export_error(iterator->status(), arguments.error)) return;
    *arguments.count = static_cast<ustore_size_t>(ids.size());
    if (arguments.ids) *arguments.ids = ids.data();
    if (arguments.modalities) *arguments.modalities = modalities.data();
    if (arguments.names_offsets) *arguments.names_offsets = names.offsets().begin().get();
    if (arguments.names) *arguments.names = reinterpret_cast<ustore_char_t*>(names.contents().begin().get());
}

void collection_find(ustore_collection_find_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.tasks_count) return;
    if (!arguments.names || !arguments.ids) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need the names and an output for the identifiers"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    database_t& database = *handle_cast<database_t>(arguments.db);
    ::leveldb::ReadOptions options;
    use_snapshot(database, arguments.snapshot, options, arguments.error);
    if (arguments.error->status) return;

    std::size_t const count = arguments.tasks_count;
    strings_arg_t const names {{arguments.names, arguments.names_stride},
                               {arguments.names_lengths, arguments.names_lengths_stride}};
    std::span<ustore_collection_t> const ids = arena.alloc<ustore_collection_t>(count, arguments.error);
    auto modalities = arena.alloc_or_dummy(count, arguments.error, arguments.modalities);
    auto presences = arena.alloc_or_dummy(count, arguments.error, arguments.presences);
    if (arguments.error->status) return;

    std::string key;
    std::string record;
    for (std::size_t index = 0; index != count; ++index) {
        native_status_t const status = database.native->Get(options, name_key(names[index], key), &record);
        ustore_collection_t id = ustore_collection_main_k;
        ustore_modality_t modality = ustore_modality_any_k;
        bool const present = status.ok();
        if (!present && !status.IsNotFound() && export_error(status, arguments.error)) return;
        if (present && !read_name_record(record, id, modality)) {
            *arguments.error = {ustore_error_malformed_k, "A name holds no collection record"};
            return;
        }
        ids[index] = id;
        modalities[index] = modality;
        presences[index] = present;
    }
    *arguments.ids = ids.data();
}

void database_control(ustore_database_control_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.request || !arguments.response) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need a request and an output for the response"};
        return;
    }

    *arguments.response = nullptr;
    if (std::string_view {arguments.request} != "info") {
        *arguments.error = {ustore_error_invalid_argument_k, "Unknown request: expected info"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, ustore_options_default_k, arguments.error);
    if (arguments.error->status) return;
    constexpr std::size_t response_capacity_k = 64;
    char* const response = arena.alloc<char>(response_capacity_k, arguments.error).data();
    if (arguments.error->status) return;
    auto const written = fmt::format_to_n(response, response_capacity_k - 1,
                                          R"({{"engine":"leveldb","version":"{}.{}"}})", ::leveldb::kMajorVersion,
                                          ::leveldb::kMinorVersion);
    *written.out = '\0';
    *arguments.response = response;
}

#pragma endregion Collections

#pragma region Strings

void strings_write(ustore_strings_write_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.tasks_count) return;
    if (!arguments.strings) {
        *arguments.error = {ustore_error_invalid_argument_k, "No strings were provided"};
        return;
    }

    database_t& database = *handle_cast<database_t>(arguments.db);
    strided_iterator<ustore_collection_t const> const collections {arguments.collections, arguments.collections_stride};
    strings_arg_t const texts {{arguments.strings, arguments.strings_stride},
                               {arguments.strings_lengths, arguments.strings_lengths_stride}};
    contents_arg_t const contents {bits_view_t {arguments.values_presences},
                                   {arguments.values_offsets, arguments.values_offsets_stride},
                                   {arguments.values_lengths, arguments.values_lengths_stride},
                                   {arguments.values, arguments.values_stride},
                                   arguments.tasks_count};
    if (refuse_collections(database, collections, arguments.tasks_count, keying_t::strings_k, arguments.error)) return;

    ::leveldb::WriteBatch batch;
    std::string key;
    for (std::size_t index = 0; index != arguments.tasks_count; ++index) {
        ustore_collection_t const collection = collections ? collections[index] : ustore_collection_main_k;
        value_view_t const content = contents[index];
        string_key(collection, texts[index], key);
        if (!content) batch.Delete(key);
        else batch.Put(key, to_slice(content));
    }
    ::leveldb::WriteOptions options;
    options.sync = arguments.options & ustore_option_write_flush_k;
    export_error(database.native->Write(options, &batch), arguments.error);
}

void strings_read(ustore_strings_read_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.tasks_count) return;
    if (!arguments.strings) {
        *arguments.error = {ustore_error_invalid_argument_k, "No strings were provided"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    database_t& database = *handle_cast<database_t>(arguments.db);
    ::leveldb::ReadOptions options;
    use_snapshot(database, arguments.snapshot, options, arguments.error);
    if (arguments.error->status) return;

    strided_iterator<ustore_collection_t const> const collections {arguments.collections, arguments.collections_stride};
    strings_arg_t const texts {{arguments.strings, arguments.strings_stride},
                               {arguments.strings_lengths, arguments.strings_lengths_stride}};
    if (refuse_collections(database, collections, arguments.tasks_count, keying_t::strings_k, arguments.error)) return;

    growing_tape_t exported {arena};
    exported.reserve(arguments.tasks_count, arguments.error);
    if (arguments.error->status) return;
    std::string key;
    std::string value;
    for (std::size_t index = 0; index != arguments.tasks_count; ++index) {
        ustore_collection_t const collection = collections ? collections[index] : ustore_collection_main_k;
        native_status_t const status = database.native->Get(options, string_key(collection, texts[index], key), &value);
        if (!status.IsNotFound() && export_error(status, arguments.error)) return;
        exported.push_back(status.ok() ? to_value(value) : value_view_t {}, arguments.error);
        if (arguments.error->status) return;
    }

    if (arguments.presences) *arguments.presences = exported.presences().get();
    if (arguments.offsets) *arguments.offsets = exported.offsets().begin().get();
    if (arguments.lengths) *arguments.lengths = exported.lengths().begin().get();
    if (arguments.values) *arguments.values = reinterpret_cast<ustore_byte_t*>(exported.contents().begin().get());
}

void strings_scan(ustore_strings_scan_t& arguments) {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "Database is uninitialized"};
        return;
    }
    if (!arguments.tasks_count) return;
    if (!arguments.count_limits || !arguments.strings) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need the limits and an output for the strings"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    database_t& database = *handle_cast<database_t>(arguments.db);
    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(database, arguments.snapshot, options, arguments.error);
    if (arguments.error->status) return;

    std::size_t const count = arguments.tasks_count;
    strided_iterator<ustore_collection_t const> const collections {arguments.collections, arguments.collections_stride};
    strings_arg_t const prefixes {{arguments.prefixes, arguments.prefixes_stride},
                                  {arguments.prefixes_lengths, arguments.prefixes_lengths_stride}};
    strings_arg_t const starts {{arguments.start_strings, arguments.start_strings_stride},
                                {arguments.start_strings_lengths, arguments.start_strings_lengths_stride}};
    strided_iterator<ustore_length_t const> const limits {arguments.count_limits, arguments.count_limits_stride};
    if (refuse_collections(database, collections, count, keying_t::strings_k, arguments.error)) return;

    auto offsets = arena.alloc_or_dummy(count + 1, arguments.error, arguments.offsets);
    auto counts = arena.alloc_or_dummy(count, arguments.error, arguments.counts);
    if (arguments.error->status) return;
    growing_tape_t found {arena};
    std::string first;
    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (std::size_t task = 0; task != count; ++task) {
        ustore_collection_t const collection = collections ? collections[task] : ustore_collection_main_k;
        std::string_view const prefix = arguments.prefixes ? prefixes[task] : std::string_view {};
        std::string_view const start = arguments.start_strings ? starts[task] : std::string_view {};
        std::size_t const found_before = found.lengths().size();
        offsets[task] = static_cast<ustore_length_t>(found_before);

        ::leveldb::Slice const wanted = to_slice(prefix);
        for (iterator->Seek(string_key(collection, start > prefix ? start : prefix, first));
             iterator->Valid() && found.lengths().size() - found_before != limits[task]; iterator->Next()) {
            if (!in_run(iterator->key(), tag_t::strings_k, collection)) break;
            ::leveldb::Slice const text = tail_of(iterator->key());
            if (!text.starts_with(wanted)) break;
            found.push_back(to_value(text), arguments.error);
            if (arguments.error->status) return;
        }
        if (export_error(iterator->status(), arguments.error)) return;
        counts[task] = static_cast<ustore_length_t>(found.lengths().size() - found_before);
    }
    offsets[count] = static_cast<ustore_length_t>(found.lengths().size());
    if (arguments.strings_offsets) *arguments.strings_offsets = found.offsets().begin().get();
    *arguments.strings = reinterpret_cast<ustore_char_t*>(found.contents().begin().get());
}

#pragma endregion Strings

#pragma region Transactions

void transaction_init(ustore_transaction_init_t& arguments) {
    *arguments.error = {ustore_error_unsupported_k, "Transactions are not supported by LevelDB"};
}

void transaction_stage(ustore_transaction_stage_t& arguments) {
    *arguments.error = {ustore_error_unsupported_k, "Transactions are not supported by LevelDB"};
}

void transaction_commit(ustore_transaction_commit_t& arguments) {
    *arguments.error = {ustore_error_unsupported_k, "Transactions are not supported by LevelDB"};
}

#pragma endregion Transactions

#pragma region Memory

void transaction_free(ustore_transaction_t) noexcept {}

void database_free(ustore_database_t handle) noexcept {
    database_t* const database = handle_cast<database_t>(handle);
    // Snapshots a caller never dropped belong to the database, so they go with it
    [[maybe_unused]] auto const released = database->snapshots.for_each([&](auto const& slot) noexcept {
        snapshot_t* const snapshot = reinterpret_cast<snapshot_t*>(slot.key());
        database->native->ReleaseSnapshot(snapshot->native);
        delete snapshot;
    });
    delete database;
}

#pragma endregion Memory

engine_t const engine_k {
    .database_capabilities = &guarded<ustore_database_capabilities_t, &database_capabilities>,
    .database_control = &guarded<ustore_database_control_t, &database_control>,
    .snapshot_list = &guarded<ustore_snapshot_list_t, &snapshot_list>,
    .snapshot_create = &guarded<ustore_snapshot_create_t, &snapshot_create>,
    .snapshot_export = &guarded<ustore_snapshot_export_t, &snapshot_export>,
    .snapshot_drop = &guarded<ustore_snapshot_drop_t, &snapshot_drop>,
    .read = &guarded<ustore_read_t, &read>,
    .write = &guarded<ustore_write_t, &write>,
    .scan = &guarded<ustore_scan_t, &scan>,
    .sample = &guarded<ustore_sample_t, &sample>,
    .measure = &guarded<ustore_measure_t, &measure>,
    .collection_create = &guarded<ustore_collection_create_t, &collection_create>,
    .collection_drop = &guarded<ustore_collection_drop_t, &collection_drop>,
    .collection_list = &guarded<ustore_collection_list_t, &collection_list>,
    .collection_find = &guarded<ustore_collection_find_t, &collection_find>,
    .strings_write = &guarded<ustore_strings_write_t, &strings_write>,
    .strings_read = &guarded<ustore_strings_read_t, &strings_read>,
    .strings_scan = &guarded<ustore_strings_scan_t, &strings_scan>,
    .transaction_init = &guarded<ustore_transaction_init_t, &transaction_init>,
    .transaction_stage = &guarded<ustore_transaction_stage_t, &transaction_stage>,
    .transaction_commit = &guarded<ustore_transaction_commit_t, &transaction_commit>,
    .transaction_free = &transaction_free,
    .database_free = &database_free,
};

} // namespace unum::ustore::leveldb

using namespace unum::ustore;
using namespace unum::ustore::leveldb;

void ustore_database_init_leveldb(ustore_database_init_leveldb_t* arguments) {
    guarded<ustore_database_init_leveldb_t, &database_init>(*arguments);
}
