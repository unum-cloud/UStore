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

#include "ustore/blobs_collection.hpp"   // `blobs_collection_t`
#include "ustore/blobs_ref.hpp"          // `blobs_ref`
#include "ustore/docs_collection.hpp"    // `docs_collection_t`
#include "ustore/graph_collection.hpp"   // `graph_collection_t`
#include "ustore/ranges.hpp"             // `strided_range`
#include "ustore/ranges_args.hpp"        // `keys_view_t`, `places_arg_t`
#include "ustore/status.hpp"             // `status_t`, `expected`
#include "ustore/strings_collection.hpp" // `strings_collection_t`
#include "ustore/types.hpp"              // `arena_t`, `collection_key_t`, `collection_key_field_t`
#include "ustore/ustore.h"

namespace unum::ustore {

using collections_list_t = std::span<ustore_collection_id_t>;

/**
 *  @brief Reads the collection @p name was given at the head, or at @p snapshot when it is set.
 *
 *  Names resolve in two phases, so this is the only place a name reaches the database, and every other
 *  call addresses its collection by the identifier this returns. An engine without collections of its
 *  own carries no names, so it answers as for a name no collection carries.
 */
inline expected<ustore_collection_id_t> collection_named(ustore_database_t database, ustore_snapshot_t snapshot,
                                                         std::string_view name) noexcept {
    ustore_str_view_t const text = name.data();
    ustore_length_t const length = static_cast<ustore_length_t>(name.size());
    ustore_octet_t presence = 0;
    ustore_collection_id_t collection = ustore_collection_main_k;
    ustore_outcome_t outcome {};
    ustore_collection_find_t find {};
    find.database = database;
    find.outcome = &outcome;
    find.snapshot = snapshot;
    find.count = 1;
    find.names = &text;
    find.names_lengths = &length;
    find.presences = &presence;
    find.collections = &collection;
    find.collections_stride = sizeof(ustore_collection_id_t);
    ustore_collection_find(&find);
    if (outcome.status == ustore_status_unsupported_k || (outcome.status == ustore_status_success_k && !(presence & 1)))
        return status_t {status_code_t::not_found_k, "No such collection"};
    if (outcome.status != ustore_status_success_k) return status_t {outcome};
    return collection;
}

/** A transaction or snapshot context for one thread.
 *
 *  Batched writes are buffered in the layout the engine prefers until the commit, so a context also
 *  saves preprocessing. It never commits on destruction, never copies, and its proxies share one arena. */
class context_t {
  protected:
    ustore_database_t database_ {nullptr};
    ustore_transaction_t transaction_ {nullptr};
    ustore_snapshot_t snapshot_ {};
    arena_t arena_;

  public:
    context_t() noexcept = default;
    context_t(ustore_database_t database, ustore_transaction_t transaction = nullptr,
              ustore_snapshot_t snapshot = {}) noexcept
        : database_(database), transaction_(transaction), snapshot_(snapshot) {}
    context_t(context_t const&) = delete;
    context_t(context_t&& other) noexcept
        : database_(other.database_), transaction_(std::exchange(other.transaction_, nullptr)),
          snapshot_(std::exchange(other.snapshot_, {})), arena_(std::move(other.arena_)) {}

    context_t& operator=(context_t&& other) noexcept {
        std::swap(database_, other.database_);
        std::swap(transaction_, other.transaction_);
        std::swap(snapshot_, other.snapshot_);
        std::swap(arena_, other.arena_);
        return *this;
    }

    ~context_t() noexcept { ustore_transaction_free(transaction_); }

    ustore_database_t database() const noexcept { return database_; }
    ustore_transaction_t transaction() const noexcept { return transaction_; }
    ustore_snapshot_t snapshot() const noexcept { return snapshot_; }
    operator ustore_transaction_t() const noexcept { return transaction_; }

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

    /** Lists the collections at the head, or at the snapshot of a snapshot context, since a listing never joins a
     *  transaction. */
    expected<collections_list_t> collections() noexcept {
        ustore_size_t count = 0;
        ustore_collection_id_t* ids = nullptr;
        ustore_outcome_t outcome {};
        ustore_collection_list_t collection_list {};
        collection_list.database = database_;
        collection_list.outcome = &outcome;
        collection_list.snapshot = snapshot_;
        collection_list.count = &count;
        collection_list.collections = &ids;
        collection_list.arena = arena_.member_ptr();

        ustore_collection_list(&collection_list);
        if (outcome.status != ustore_status_success_k) return status_t {outcome};
        return collections_list_t {ids, ids + count};
    }

    /** Whether a collection named @p name exists, where the empty name is the main collection. */
    expected<bool> contains(std::string_view name) noexcept {
        if (name.empty()) return true;

        auto [found, lookup_status] = collection_named(database_, snapshot_, name);
        if (lookup_status.succeeded()) return true;
        if (lookup_status == status_code_t::not_found_k) return false;
        return lookup_status;
    }

    /**
     *  Opens a view of one collection that follows this context's transaction or snapshot.
     *  @tparam collection_ One of @c blobs_collection_t, @c docs_collection_t or @c graph_collection_t.
     */
    template <typename collection_ = blobs_collection_t>
    expected<collection_> find(std::string_view name = {}) noexcept {
        if (name.empty())
            return collection_ {database_, ustore_collection_main_k, transaction_, snapshot_, arena_.member_ptr()};

        auto [found, lookup_status] = collection_named(database_, snapshot_, name);
        if (lookup_status.failed()) return lookup_status;
        return collection_ {database_, found, transaction_, snapshot_, arena_.member_ptr()};
    }

    /** Opens a transaction, or clears the one held for reuse, keeping its memory and starting a new generation. */
    status_t reset() noexcept {
        ustore_outcome_t outcome {};
        ustore_transaction_init_t transaction_init {};
        transaction_init.transaction = &transaction_;
        transaction_init.database = database_;
        transaction_init.outcome = &outcome;

        ustore_transaction_init(&transaction_init);
        return status_t {outcome};
    }

    /** Withdraws a stage, releasing what it claimed while keeping the reads and writes, so the transaction can stage
     *  or commit again. */
    status_t rollback() noexcept {
        ustore_outcome_t outcome {};
        ustore_transaction_rollback_t transaction_rollback {};
        transaction_rollback.database = database_;
        transaction_rollback.outcome = &outcome;
        transaction_rollback.transaction = transaction_;

        ustore_transaction_rollback(&transaction_rollback);
        return status_t {outcome};
    }

    /**
     *  Commits every update of the transaction, or none of them if any one fails.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    status_t commit(ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        ustore_outcome_t outcome {};
        ustore_transaction_commit_t transaction_commit {};
        transaction_commit.database = database_;
        transaction_commit.outcome = &outcome;
        transaction_commit.transaction = transaction_;
        transaction_commit.durability = durability;
        ustore_transaction_commit(&transaction_commit);
        return status_t {outcome};
    }

    /**
     *  Commits like @c commit and returns the stamp the commit published under.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    expected<ustore_stamp_t> stamped_commit(ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        ustore_outcome_t outcome {};
        ustore_stamp_t stamp = std::numeric_limits<ustore_stamp_t>::max();
        ustore_transaction_commit_t transaction_commit {};
        transaction_commit.database = database_;
        transaction_commit.outcome = &outcome;
        transaction_commit.transaction = transaction_;
        transaction_commit.stamp = &stamp;
        transaction_commit.durability = durability;
        ustore_transaction_commit(&transaction_commit);
        if (outcome.status != ustore_status_success_k) return status_t {outcome};
        return stamp;
    }
};

using transaction_t = context_t;

/** A point-in-time view of the whole database, which reads through it see until its destructor frees it. It never
 *  copies, and goes before its database closes. */
class snapshot_t : public context_t {
    snapshot_t(ustore_database_t database, ustore_snapshot_t snapshot) noexcept
        : context_t(database, nullptr, snapshot) {}

  public:
    snapshot_t() noexcept = default;
    snapshot_t(snapshot_t&&) noexcept = default;
    snapshot_t& operator=(snapshot_t&&) noexcept = default;
    ~snapshot_t() noexcept { ustore_snapshot_free(snapshot_); }

    /** Pins a point-in-time view of @p database, which the snapshot frees when it goes. */
    static expected<snapshot_t> make(ustore_database_t database) noexcept {
        ustore_outcome_t outcome {};
        ustore_snapshot_t raw = nullptr;
        ustore_snapshot_init_t snapshot_init {
            .snapshot = &raw,
            .database = database,
            .outcome = &outcome,
        };
        ustore_snapshot_init(&snapshot_init);
        if (outcome.status != ustore_status_success_k) return status_t {outcome};
        return snapshot_t {database, raw};
    }

    /** A transaction opened here would shadow the snapshot in every read, so a snapshot opens none. */
    status_t reset() noexcept = delete;

    /** Exports the snapshot as a new database at @p path. */
    status_t export_to(ustore_str_view_t path) noexcept {
        ustore_outcome_t outcome {};
        ustore_snapshot_export_t snapshot_export {};
        snapshot_export.database = database_;
        snapshot_export.outcome = &outcome;
        snapshot_export.snapshot = snapshot_;
        snapshot_export.path = path;
        ustore_snapshot_export(&snapshot_export);
        return status_t {outcome};
    }
};

/** A database as a transactional map from collection names to collections of binary values.
 *  It is thread-safe except for @c open and @c close, never copies, and outlives its collections. */
class database_t {
    ustore_database_t database_ = nullptr;

    ustore_capabilities_t capabilities_() noexcept {
        ustore_outcome_t outcome {};
        ustore_database_description_t description {};
        ustore_database_describe_t request {};
        request.database = database_;
        request.outcome = &outcome;
        request.description = &description;
        ustore_database_describe(&request);
        return description.capabilities;
    }

    /** Opens the engine behind @p init with @p options, whose outputs this call fills in. */
    template <typename options_type_>
    static expected<database_t> open_(void (*init)(options_type_*), options_type_ options) noexcept {
        database_t database;
        ustore_outcome_t outcome {};
        options.database = &database.database_;
        options.outcome = &outcome;
        init(&options);
        if (outcome.status != ustore_status_success_k) return status_t {outcome};
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
        context_t transaction {database_};
        if (status_t const begun = transaction.reset(); begun.failed()) return begun;
        return transaction;
    }

    /** Pins a point-in-time view of the database, which the returned snapshot frees when it goes. */
    expected<snapshot_t> snapshot() noexcept { return snapshot_t::make(database_); }

    template <typename collection_ = blobs_collection_t>
    collection_ main() noexcept {
        return collection_ {database_, ustore_collection_main_k};
    }

    operator blobs_collection_t() noexcept { return main(); }
    expected<blobs_collection_t> operator[](std::string_view name) noexcept { return find_or_create(name); }

    /** Whether a collection named @p name exists at the head, where the empty name is the main collection. */
    expected<bool> contains(std::string_view name) noexcept {
        if (name.empty()) return true;

        auto [found, lookup_status] = collection_named(database_, nullptr, name);
        if (lookup_status.succeeded()) return true;
        if (lookup_status == status_code_t::not_found_k) return false;
        return lookup_status;
    }

    /** Creates a collection of @p modality under @p name, which no other collection may carry. */
    template <typename collection_ = blobs_collection_t>
    expected<collection_> create(std::string_view name, ustore_modality_t modality = ustore_modality_any_k) noexcept {
        ustore_outcome_t outcome {};
        ustore_collection_id_t collection = ustore_collection_main_k;
        ustore_collection_create_t create {};
        create.database = database_;
        create.outcome = &outcome;
        create.name = name.data();
        create.name_length = static_cast<ustore_length_t>(name.size());
        create.modality = modality;
        create.collection = &collection;
        ustore_collection_create(&create);
        if (outcome.status != ustore_status_success_k) return status_t {outcome};
        return collection_ {database_, collection, nullptr};
    }

    template <typename collection_ = blobs_collection_t>
    expected<collection_> find(std::string_view name = {}) noexcept {
        if (name.empty()) return collection_ {database_, ustore_collection_main_k};

        auto [found, find_status] = collection_named(database_, nullptr, name);
        if (find_status.failed()) return find_status;
        return collection_ {database_, found};
    }

    template <typename collection_ = blobs_collection_t>
    expected<collection_> find_or_create(std::string_view name) noexcept {
        if (auto found = find<collection_>(name); found) return found;
        return create<collection_>(name);
    }

    /** Drops the collection @p name addresses, which takes the name with it. */
    status_t drop(std::string_view name) noexcept {
        auto [collection, find_status] = collection_named(database_, nullptr, name);
        if (find_status.failed()) return find_status;

        ustore_outcome_t outcome {};
        ustore_collection_drop_t drop {};
        drop.database = database_;
        drop.outcome = &outcome;
        drop.collection = collection;
        drop.mode = ustore_drop_keys_vals_handle_k;
        ustore_collection_drop(&drop);
        return status_t {outcome};
    }

    /** Drops every collection, whose names go with them, then empties the main collection. Snapshots belong to
     *  their holders and stay. */
    status_t clear() noexcept {
        context_t context {database_, nullptr};
        ustore_outcome_t outcome {};

        auto [collection_ids, list_collections_status] = context.collections();
        if (list_collections_status.failed()) return list_collections_status;

        ustore_collection_drop_t collection_drop {};
        collection_drop.database = database_;
        collection_drop.outcome = &outcome;
        collection_drop.mode = ustore_drop_keys_vals_handle_k;
        for (ustore_collection_id_t const collection : collection_ids) {
            collection_drop.collection = collection;
            ustore_collection_drop(&collection_drop);
            if (outcome.status != ustore_status_success_k) return status_t {outcome};
        }

        collection_drop.collection = ustore_collection_main_k;
        collection_drop.mode = ustore_drop_keys_vals_k;
        ustore_collection_drop(&collection_drop);
        return status_t {outcome};
    }
};

} // namespace unum::ustore
