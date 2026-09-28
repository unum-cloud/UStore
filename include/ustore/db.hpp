/**
 *  @file include/ustore/db.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief The database and transaction handles of the C++ wrappers.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`

#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::forward`, `std::move`, `std::swap`

#include <ustore/blobs_collection.hpp>   // `blobs_collection_t`
#include <ustore/blobs_ref.hpp>          // `blobs_ref`
#include <ustore/docs_collection.hpp>    // `docs_collection_t`
#include <ustore/graph_collection.hpp>   // `graph_collection_t`
#include <ustore/ranges.hpp>             // `strided_range`
#include <ustore/ranges_args.hpp>        // `keys_view_t`, `places_arg_t`
#include <ustore/status.hpp>             // `status_t`, `expected`
#include <ustore/strings_collection.hpp> // `strings_collection_t`
#include <ustore/types.hpp>              // `arena_t`, `collection_key_t`, `collection_key_field_t`
#include <ustore/ustore.h>

namespace unum::ustore {

using snapshots_list_t = std::span<ustore_snapshot_t>;

using collections_list_t = std::span<ustore_collection_t>;

/**
 *  @brief Reads the collection @p name was given, through the strings modality that holds every name.
 *
 *  Names resolve in two phases, so this is the only place a name reaches the database, and every other
 *  call addresses its collection by the identifier this returns.
 */
inline expected<ustore_collection_t> collection_named(ustore_database_t database, ustore_transaction_t transaction,
                                                      ustore_arena_t* arena, std::string_view name) noexcept {
    strings_collection_t names {database, ustore_collection_names_k, transaction, {}, arena};
    auto [stored, read_status] = names.value(name);
    if (read_status.failed()) return read_status;
    if (stored.size() != sizeof(ustore_collection_t))
        return status_t {status_code_t::not_found_k, "No such collection"};

    ustore_collection_t found = ustore_collection_main_k;
    std::memcpy(&found, stored.data(), sizeof(found));
    return found;
}

/** Records that @p name addresses @p collection, or removes the name when no collection is given. */
inline status_t name_collection(ustore_database_t database, ustore_arena_t* arena, std::string_view name,
                                ustore_collection_t const* collection) noexcept {
    strings_collection_t names {database, ustore_collection_names_k, nullptr, {}, arena};
    if (!collection) return names.erase(name);
    return names.assign(name, value_view_t {reinterpret_cast<byte_t const*>(collection), sizeof(*collection)});
}

/** Whether @p status only says the engine keeps no collections of its own, so no name can be stored. */
inline bool without_collections(status_t status) noexcept { return status.code == status_code_t::unsupported_k; }

/** A transaction or snapshot context for one thread.
 *
 *  Batched writes are buffered in the layout the engine prefers until the commit, so a context also
 *  saves preprocessing. It never commits on destruction, never copies, and its proxies share one arena. */
class context_t {
  protected:
    ustore_database_t database_ {nullptr};
    ustore_transaction_t transaction_ {nullptr};
    ustore_snapshot_t snapshot_ {};
    arena_t arena_ {nullptr};

  public:
    context_t() noexcept : arena_(nullptr) {}
    context_t(ustore_database_t database, ustore_transaction_t transaction = nullptr,
              ustore_snapshot_t snapshot = {}) noexcept
        : database_(database), transaction_(transaction), snapshot_(snapshot), arena_(database) {}
    context_t(context_t const&) = delete;
    context_t(context_t&& other) noexcept
        : database_(other.database_), transaction_(std::exchange(other.transaction_, nullptr)),
          snapshot_(std::exchange(other.snapshot_, {})), arena_(std::exchange(other.arena_, {nullptr})) {}

    context_t& operator=(context_t&& other) noexcept {
        std::swap(database_, other.database_);
        std::swap(transaction_, other.transaction_);
        std::swap(snapshot_, other.snapshot_);
        std::swap(arena_, other.arena_);
        return *this;
    }

    ~context_t() noexcept {
        ustore_transaction_free(transaction_);
        transaction_ = nullptr;
    }

    ustore_database_t db() const noexcept { return database_; }
    ustore_transaction_t txn() const noexcept { return transaction_; }
    ustore_snapshot_t snap() const noexcept { return snapshot_; }
    operator ustore_transaction_t() const noexcept { return transaction_; }

    void set_snapshot(ustore_snapshot_t snapshot) noexcept { snapshot_ = snapshot; }

    blobs_ref<places_arg_t> operator[](strided_range<collection_key_t const> collections_and_keys) noexcept {
        places_arg_t arg;
        arg.collections_begin = collections_and_keys.members(&collection_key_t::collection).begin();
        arg.keys_begin = collections_and_keys.members(&collection_key_t::key).begin();
        arg.count = collections_and_keys.size();
        return {database_, transaction_, snapshot_, std::move(arg), arena_};
    }

    blobs_ref<places_arg_t> operator[](strided_range<collection_key_field_t const> collections_and_keys) noexcept {
        places_arg_t arg;
        arg.collections_begin = collections_and_keys.members(&collection_key_field_t::collection).begin();
        arg.keys_begin = collections_and_keys.members(&collection_key_field_t::key).begin();
        arg.fields_begin = collections_and_keys.members(&collection_key_field_t::field).begin();
        arg.count = collections_and_keys.size();
        return {database_, transaction_, snapshot_, std::move(arg), arena_};
    }

    blobs_ref<places_arg_t> operator[](keys_view_t keys) noexcept {
        places_arg_t arg;
        arg.keys_begin = keys.begin();
        arg.count = keys.size();
        return {database_, transaction_, snapshot_, std::move(arg), arena_};
    }

    template <typename keys_arg_>
    blobs_ref<keys_arg_> operator[](keys_arg_&& keys) noexcept {
        return blobs_ref<keys_arg_> {database_, transaction_, snapshot_, std::forward<keys_arg_>(keys),
                                     arena_.member_ptr()};
    }

    expected<blobs_collection_t> operator[](ustore_str_view_t name) noexcept { return find(name); }

    template <typename collection_ = blobs_collection_t>
    collection_ main() noexcept {
        return collection_ {database_, ustore_collection_main_k, transaction_, snapshot_, arena_.member_ptr()};
    }

    expected<collections_list_t> collections() noexcept {
        ustore_size_t count = 0;
        ustore_collection_t* ids = nullptr;
        ustore_error_t error {};
        ustore_collection_list_t collection_list {};
        collection_list.db = database_;
        collection_list.error = &error;
        collection_list.transaction = transaction_;
        collection_list.snapshot = snapshot_;
        collection_list.arena = arena_.member_ptr();
        collection_list.count = &count;
        collection_list.ids = &ids;

        ustore_collection_list(&collection_list);
        if (error.status) return status_t {error};
        return collections_list_t {ids, ids + count};
    }

    expected<snapshots_list_t> snapshots() noexcept {
        ustore_size_t count = 0;
        ustore_snapshot_t* ids = nullptr;
        ustore_error_t error {};
        ustore_snapshot_list_t snapshots_list {};
        snapshots_list.db = database_;
        snapshots_list.error = &error;
        snapshots_list.arena = arena_.member_ptr();
        snapshots_list.count = &count;
        snapshots_list.ids = &ids;

        ustore_snapshot_list(&snapshots_list);
        if (error.status) return status_t {error};
        return snapshots_list_t {ids, ids + count};
    }

    /** Whether a collection named @p name exists, where the empty name is the main collection. */
    expected<bool> contains(std::string_view name) noexcept {
        if (name.empty()) return true;

        auto [found, lookup_status] = collection_named(database_, transaction_, arena_.member_ptr(), name);
        if (lookup_status.succeeded()) return true;
        if (lookup_status == status_code_t::not_found_k || without_collections(lookup_status)) return false;
        return lookup_status;
    }

    /**
     *  Opens a view of one collection that follows this context's transaction.
     *  @tparam collection_ One of @c blobs_collection_t, @c docs_collection_t or @c graph_collection_t.
     */
    template <typename collection_ = blobs_collection_t>
    expected<collection_> find(std::string_view name = {}) noexcept {
        if (name.empty())
            return collection_ {database_, ustore_collection_main_k, transaction_, {}, arena_.member_ptr()};

        auto [found, lookup_status] = collection_named(database_, transaction_, arena_.member_ptr(), name);
        if (lookup_status.succeeded()) return collection_ {database_, found, transaction_, {}, arena_.member_ptr()};
        if (without_collections(lookup_status)) return status_t {status_code_t::not_found_k, "No such collection"};
        return lookup_status;
    }

    /** Clears the transaction for reuse, keeping its memory and starting a new generation. */
    status_t reset() noexcept {
        ustore_error_t error {};
        ustore_transaction_init_t transaction_init {};
        transaction_init.db = database_;
        transaction_init.error = &error;
        transaction_init.transaction = &transaction_;

        ustore_transaction_init(&transaction_init);
        return status_t {error};
    }

    /**
     *  Commits every update of the transaction, or none of them if any one fails.
     *  @param[in] flush Whether the call returns only after the data reaches the disk.
     */
    status_t commit(flush_t flush = flush_t::dont_flush_k) noexcept {
        ustore_error_t error {};
        ustore_transaction_commit_t transaction_commit {};
        transaction_commit.db = database_;
        transaction_commit.error = &error;
        transaction_commit.transaction = transaction_;
        transaction_commit.options = to_options(flush);
        ustore_transaction_commit(&transaction_commit);
        return status_t {error};
    }

    /**
     *  Commits like @c commit and returns the sequence number the commit received.
     *  @param[in] flush Whether the call returns only after the data reaches the disk.
     */
    expected<ustore_sequence_number_t> sequenced_commit(flush_t flush = flush_t::dont_flush_k) noexcept {
        ustore_error_t error {};
        ustore_sequence_number_t sequence_number = std::numeric_limits<ustore_sequence_number_t>::max();
        ustore_transaction_commit_t transaction_commit {};
        transaction_commit.db = database_;
        transaction_commit.error = &error;
        transaction_commit.transaction = transaction_;
        transaction_commit.options = to_options(flush);
        transaction_commit.sequence_number = &sequence_number;
        ustore_transaction_commit(&transaction_commit);
        if (error.status) return status_t {error};
        return sequence_number;
    }

    /** Exports the snapshot as a new database at @p path. */
    status_t export_to(ustore_str_view_t path) noexcept {
        ustore_error_t error {};
        ustore_snapshot_export_t snapshot_export {};
        snapshot_export.db = database_;
        snapshot_export.error = &error;
        snapshot_export.id = snapshot_;
        snapshot_export.path = path;
        ustore_snapshot_export(&snapshot_export);
        return status_t {error};
    }
};

using transaction_t = context_t;

/** A database as a transactional map from collection names to collections of binary values.
 *  It is thread-safe except for @c open and @c close, never copies, and outlives its collections. */
class database_t {
    ustore_database_t database_ = nullptr;

    ustore_capabilities_t capabilities_() noexcept {
        ustore_error_t error {};
        ustore_capabilities_t capabilities {};
        ustore_database_capabilities_t request {};
        request.db = database_;
        request.capabilities = &capabilities;
        request.error = &error;
        ustore_database_capabilities(&request);
        return capabilities;
    }

    /** Opens the engine behind @p init with @p options, whose outputs this call fills in. */
    template <typename options_type_>
    static expected<database_t> open_(void (*init)(options_type_*), options_type_ options) noexcept {
        database_t database;
        ustore_error_t error {};
        options.db = &database.database_;
        options.error = &error;
        init(&options);
        if (error.status) return status_t {error};
        return database;
    }

  public:
    database_t() = default;
    database_t(database_t const&) = delete;
    database_t(database_t&& other) noexcept : database_(std::exchange(other.database_, nullptr)) {}
    explicit database_t(ustore_database_t adopted) noexcept : database_(adopted) {}
    database_t& operator=(database_t&& other) noexcept {
        std::swap(database_, other.database_);
        return *this;
    }
    operator ustore_database_t() const noexcept { return database_; }

    /** Opens the in-memory engine, which persists as one image when @p options name a directory. */
    static expected<database_t> open_ram(ustore_database_init_ram_t options = {}) noexcept {
        return open_(ustore_database_init_ram, options);
    }

    /** Opens the RocksDB engine in the directory @p options name. */
    static expected<database_t> open_rocksdb(ustore_database_init_rocksdb_t options) noexcept {
        return open_(ustore_database_init_rocksdb, options);
    }

    /** Opens the LevelDB engine in the directory @p options name. */
    static expected<database_t> open_leveldb(ustore_database_init_leveldb_t options) noexcept {
        return open_(ustore_database_init_leveldb, options);
    }

    /** Opens the NVMe engine over the directories @p options name. */
    static expected<database_t> open_nvme(ustore_database_init_nvme_t options) noexcept {
        return open_(ustore_database_init_nvme, options);
    }

    void close() noexcept {
        ustore_database_free(database_);
        database_ = nullptr;
    }

    ~database_t() noexcept {
        if (database_) close();
    }

    bool supports_collections() noexcept { return capabilities_() & ustore_supports_collections_k; }
    bool supports_transactions() noexcept { return capabilities_() & ustore_supports_transactions_k; }
    bool supports_snapshots() noexcept { return capabilities_() & ustore_supports_snapshots_k; }

    expected<context_t> transact() noexcept {
        ustore_error_t error {};
        ustore_transaction_t raw {};
        ustore_transaction_init_t transaction_init {};
        transaction_init.db = database_;
        transaction_init.error = &error;
        transaction_init.transaction = &raw;

        ustore_transaction_init(&transaction_init);
        if (error.status) return status_t {error};
        return context_t {database_, raw};
    }

    expected<context_t> snapshot() noexcept {
        ustore_error_t error {};
        ustore_snapshot_t raw = {};
        ustore_snapshot_create_t snapshot_create {
            .db = database_,
            .error = &error,
            .id = &raw,
        };
        ustore_snapshot_create(&snapshot_create);
        if (error.status) return status_t {error};
        return context_t {database_, nullptr, raw};
    }

    status_t drop_snapshot(ustore_snapshot_t id) noexcept {
        ustore_error_t error {};
        ustore_snapshot_drop_t snapshot_drop {
            .db = database_,
            .error = &error,
            .id = id,
        };
        ustore_snapshot_drop(&snapshot_drop);
        return status_t {error};
    }

    template <typename collection_ = blobs_collection_t>
    collection_ main() noexcept {
        return collection_ {database_, ustore_collection_main_k};
    }

    operator blobs_collection_t() noexcept { return main(); }
    expected<blobs_collection_t> operator[](ustore_str_view_t name) noexcept { return find_or_create(name); }
    expected<bool> contains(std::string_view name) noexcept { return context_t {database_, nullptr}.contains(name); }

    /** Creates a collection and records @p name for it, in the two phases a name always resolves through. */
    template <typename collection_ = blobs_collection_t>
    expected<collection_> create(ustore_str_view_t name, ustore_str_view_t config = "") noexcept {
        std::string_view const wanted {name ? name : ""};
        if (wanted.empty())
            return status_t {status_code_t::invalid_argument_k, "The main collection has no name and always exists"};

        arena_t arena {database_};
        auto [taken, lookup_status] = collection_named(database_, nullptr, arena.member_ptr(), wanted);
        if (lookup_status.succeeded())
            return status_t {status_code_t::already_exists_k, "Such collection already exists"};
        if (lookup_status != status_code_t::not_found_k && !without_collections(lookup_status)) return lookup_status;

        ustore_error_t error {};
        ustore_collection_t collection = ustore_collection_main_k;
        ustore_collection_create_t collection_create {};
        collection_create.db = database_;
        collection_create.error = &error;
        collection_create.config = config;
        collection_create.id = &collection;

        ustore_collection_create(&collection_create);
        if (error.status) return status_t {error};

        status_t const named = name_collection(database_, arena.member_ptr(), wanted, &collection);
        if (named.failed()) return named;
        return collection_ {database_, collection, nullptr};
    }

    template <typename collection_ = blobs_collection_t>
    expected<collection_> find(std::string_view name = {}) noexcept {
        auto [found, find_status] = context_t {database_, nullptr}.find(name);
        if (find_status.failed()) return find_status;
        return collection_ {database_, found, nullptr};
    }

    template <typename collection_ = blobs_collection_t>
    expected<collection_> find_or_create(ustore_str_view_t name) noexcept {
        auto maybe_found = context_t {database_, nullptr}.find(name);
        if (maybe_found) return collection_ {database_, *maybe_found, nullptr};
        return create<collection_>(name);
    }

    status_t drop(std::string_view name) noexcept {
        auto [found, find_status] = find(name);
        if (find_status.failed()) return find_status;
        status_t const dropped = found.drop();
        if (dropped.failed()) return dropped;

        arena_t arena {database_};
        return name_collection(database_, arena.member_ptr(), name, nullptr);
    }

    /** Drops every snapshot and collection, then empties the names and the main collection. */
    status_t clear() noexcept {
        context_t context {database_, nullptr};
        ustore_error_t error {};

        auto [snapshot_ids, list_snapshots_status] = context.snapshots();
        if (list_snapshots_status.failed()) return list_snapshots_status;

        ustore_snapshot_drop_t snapshot_drop {};
        snapshot_drop.db = database_;
        snapshot_drop.error = &error;
        for (ustore_snapshot_t const id : snapshot_ids) {
            snapshot_drop.id = id;
            ustore_snapshot_drop(&snapshot_drop);
            if (error.status) return status_t {error};
        }

        auto [collection_ids, list_collections_status] = context.collections();
        if (list_collections_status.failed()) return list_collections_status;

        ustore_collection_drop_t collection_drop {};
        collection_drop.db = database_;
        collection_drop.error = &error;
        collection_drop.mode = ustore_drop_keys_vals_handle_k;
        for (ustore_collection_t const id : collection_ids) {
            collection_drop.id = id;
            ustore_collection_drop(&collection_drop);
            if (error.status) return status_t {error};
        }

        // The names go with the collections they addressed, and an engine without them has none to clear
        collection_drop.id = ustore_collection_names_k;
        collection_drop.mode = ustore_drop_keys_vals_k;
        ustore_collection_drop(&collection_drop);
        if (error.status && error.status != ustore_error_unsupported_k) return status_t {error};
        error = {};

        collection_drop.id = ustore_collection_main_k;
        collection_drop.mode = ustore_drop_keys_vals_k;
        ustore_collection_drop(&collection_drop);
        return status_t {error};
    }
};

} // namespace unum::ustore
