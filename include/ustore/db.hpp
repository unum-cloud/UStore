/**
 *  @file include/ustore/db.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief The database and transaction handles of the C++ wrappers.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <limits>      // `std::numeric_limits`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::forward`, `std::move`, `std::swap`

#include <ustore/blobs_collection.hpp> // `blobs_collection_t`
#include <ustore/blobs_ref.hpp>        // `blobs_ref`
#include <ustore/docs_collection.hpp>  // `docs_collection_t`
#include <ustore/graph_collection.hpp> // `graph_collection_t`
#include <ustore/ranges.hpp>           // `ptr_range`, `strided_range`, `strings_tape_iterator_t`
#include <ustore/ranges_args.hpp>      // `keys_view_t`, `places_arg_t`
#include <ustore/status.hpp>           // `status_t`, `expected`
#include <ustore/types.hpp>            // `arena_t`, `collection_key_t`, `collection_key_field_t`
#include <ustore/ustore.h>

namespace unum::ustore {

using snapshots_list_t = ptr_range<ustore_snapshot_t>;

/** The named collections of a database, identifiers and names in the same order. */
struct collections_list_t {
    ptr_range<ustore_collection_t> ids;
    strings_tape_iterator_t names;
};

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
        ustore_str_span_t names = nullptr;
        ustore_collection_t* ids = nullptr;
        status_t status;
        ustore_collection_list_t collection_list {};
        collection_list.db = database_;
        collection_list.error = status.member_ptr();
        collection_list.transaction = transaction_;
        collection_list.snapshot = snapshot_;
        collection_list.arena = arena_.member_ptr();
        collection_list.count = &count;
        collection_list.ids = &ids;
        collection_list.names = &names;

        ustore_collection_list(&collection_list);
        collections_list_t result;
        result.ids = {ids, ids + count};
        result.names = {count, names};
        return {std::move(status), std::move(result)};
    }

    expected<snapshots_list_t> snapshots() noexcept {
        ustore_size_t count = 0;
        ustore_snapshot_t* ids = nullptr;
        status_t status;
        ustore_snapshot_list_t snapshots_list {};
        snapshots_list.db = database_;
        snapshots_list.error = status.member_ptr();
        snapshots_list.arena = arena_.member_ptr();
        snapshots_list.count = &count;
        snapshots_list.ids = &ids;

        ustore_snapshot_list(&snapshots_list);
        return {std::move(status), snapshots_list_t {ids, ids + count}};
    }

    /** Whether a collection named @p name exists, where the empty name is the main collection. */
    expected<bool> contains(std::string_view name) noexcept {
        if (name.empty()) return true;

        auto maybe_listed = collections();
        if (!maybe_listed) return maybe_listed.release_status();

        strings_tape_iterator_t names = maybe_listed->names;
        for (std::size_t index = 0; index != maybe_listed->ids.size(); ++index, ++names)
            if (*names == name) return true;
        return false;
    }

    /**
     *  Opens a view of one collection that follows this context's transaction.
     *  @tparam collection_ One of @c blobs_collection_t, @c docs_collection_t or @c graph_collection_t.
     */
    template <typename collection_ = blobs_collection_t>
    expected<collection_> find(std::string_view name = {}) noexcept {
        if (name.empty())
            return collection_ {database_, ustore_collection_main_k, transaction_, {}, arena_.member_ptr()};

        auto maybe_listed = collections();
        if (!maybe_listed) return maybe_listed.release_status();

        strings_tape_iterator_t names = maybe_listed->names;
        for (ustore_collection_t const id : maybe_listed->ids) {
            if (*names == name) return collection_ {database_, id, transaction_, {}, arena_.member_ptr()};
            ++names;
        }
        return status_t::status_view("No such collection is present");
    }

    /** Clears the transaction for reuse, keeping its memory and starting a new generation. */
    status_t reset() noexcept {
        status_t status;
        ustore_transaction_init_t transaction_init {};
        transaction_init.db = database_;
        transaction_init.error = status.member_ptr();
        transaction_init.transaction = &transaction_;

        ustore_transaction_init(&transaction_init);
        return status;
    }

    /**
     *  Commits every update of the transaction, or none of them if any one fails.
     *  @param[in] flush Whether the call returns only after the data reaches the disk.
     */
    status_t commit(flush_t flush = flush_t::dont_flush_k) noexcept {
        status_t status;
        ustore_transaction_commit_t transaction_commit {};
        transaction_commit.db = database_;
        transaction_commit.error = status.member_ptr();
        transaction_commit.transaction = transaction_;
        transaction_commit.options = to_options(flush);
        ustore_transaction_commit(&transaction_commit);
        return status;
    }

    /**
     *  Commits like @c commit and returns the sequence number the commit received.
     *  @param[in] flush Whether the call returns only after the data reaches the disk.
     */
    expected<ustore_sequence_number_t> sequenced_commit(flush_t flush = flush_t::dont_flush_k) noexcept {
        status_t status;
        ustore_sequence_number_t sequence_number = std::numeric_limits<ustore_sequence_number_t>::max();
        ustore_transaction_commit_t transaction_commit {};
        transaction_commit.db = database_;
        transaction_commit.error = status.member_ptr();
        transaction_commit.transaction = transaction_;
        transaction_commit.options = to_options(flush);
        transaction_commit.sequence_number = &sequence_number;
        ustore_transaction_commit(&transaction_commit);
        return {std::move(status), std::move(sequence_number)};
    }

    /** Exports the snapshot as a new database at @p path. */
    status_t export_to(ustore_str_view_t path) noexcept {
        status_t status;
        ustore_snapshot_export_t snapshot_export {};
        snapshot_export.db = database_;
        snapshot_export.error = status.member_ptr();
        snapshot_export.id = snapshot_;
        snapshot_export.path = path;
        ustore_snapshot_export(&snapshot_export);
        return status;
    }
};

using transaction_t = context_t;

/** A database as a transactional map from collection names to collections of binary values.
 *  It is thread-safe except for @c open and @c close, never copies, and outlives its collections. */
class database_t {
    ustore_database_t database_ = nullptr;

    ustore_metadata_t get_metadata() noexcept {
        status_t status {};
        ustore_metadata_t metadata {};
        ustore_get_metadata_t request {};
        request.db = database_;
        request.metadata = &metadata;
        request.error = status.member_ptr();
        ustore_get_metadata(&request);
        return metadata;
    }

  public:
    database_t() = default;
    database_t(database_t const&) = delete;
    database_t(database_t&& other) noexcept : database_(std::exchange(other.database_, nullptr)) {}
    operator ustore_database_t() const noexcept { return database_; }

    status_t open(ustore_str_view_t config = nullptr) noexcept {
        status_t status;
        ustore_database_init_t database_init {};
        database_init.config = config;
        database_init.db = &database_;
        database_init.error = status.member_ptr();
        ustore_database_init(&database_init);
        return status;
    }

    void close() noexcept {
        ustore_database_free(database_);
        database_ = nullptr;
    }

    ~database_t() noexcept {
        if (database_) close();
    }

    bool supports_named_collections() noexcept { return get_metadata() & ustore_supports_named_collections_k; }
    bool supports_transactions() noexcept { return get_metadata() & ustore_supports_transactions_k; }
    bool supports_snapshots() noexcept { return get_metadata() & ustore_supports_snapshots_k; }

    expected<context_t> transact() noexcept {
        status_t status {};
        ustore_transaction_t raw {};
        ustore_transaction_init_t transaction_init {};
        transaction_init.db = database_;
        transaction_init.error = status.member_ptr();
        transaction_init.transaction = &raw;

        ustore_transaction_init(&transaction_init);
        if (!status) return {std::move(status), context_t {database_, nullptr}};
        return context_t {database_, raw};
    }

    expected<context_t> snapshot() noexcept {
        status_t status;
        ustore_snapshot_t raw = {};
        ustore_snapshot_create_t snapshot_create {
            .db = database_,
            .error = status.member_ptr(),
            .id = &raw,
        };
        ustore_snapshot_create(&snapshot_create);
        if (!status) return {std::move(status), context_t {database_, nullptr}};
        return context_t {database_, nullptr, raw};
    }

    status_t drop_snapshot(ustore_snapshot_t id) noexcept {
        status_t status;
        ustore_snapshot_drop_t snapshot_drop {
            .db = database_,
            .error = status.member_ptr(),
            .id = id,
        };
        ustore_snapshot_drop(&snapshot_drop);
        return status;
    }

    template <typename collection_ = blobs_collection_t>
    collection_ main() noexcept {
        return collection_ {database_, ustore_collection_main_k};
    }

    operator blobs_collection_t() noexcept { return main(); }
    expected<blobs_collection_t> operator[](ustore_str_view_t name) noexcept { return find_or_create(name); }
    expected<bool> contains(std::string_view name) noexcept { return context_t {database_, nullptr}.contains(name); }

    template <typename collection_ = blobs_collection_t>
    expected<collection_> create(ustore_str_view_t name, ustore_str_view_t config = "") noexcept {
        status_t status;
        ustore_collection_t collection = ustore_collection_main_k;
        ustore_collection_create_t collection_create {};
        collection_create.db = database_;
        collection_create.error = status.member_ptr();
        collection_create.name = name;
        collection_create.config = config;
        collection_create.id = &collection;

        ustore_collection_create(&collection_create);
        if (!status) return std::move(status);
        return collection_ {database_, collection, nullptr};
    }

    template <typename collection_ = blobs_collection_t>
    expected<collection_> find(std::string_view name = {}) noexcept {
        auto maybe_found = context_t {database_, nullptr}.find(name);
        if (!maybe_found) return maybe_found.release_status();
        return collection_ {database_, *maybe_found, nullptr};
    }

    template <typename collection_ = blobs_collection_t>
    expected<collection_> find_or_create(ustore_str_view_t name) noexcept {
        auto maybe_found = context_t {database_, nullptr}.find(name);
        if (maybe_found) return collection_ {database_, *maybe_found, nullptr};
        return create<collection_>(name);
    }

    status_t drop(std::string_view name) noexcept {
        auto maybe_collection = find(name);
        if (!maybe_collection) return maybe_collection.release_status();
        return maybe_collection->drop();
    }

    /** Drops every snapshot and named collection, then empties the main collection. */
    status_t clear() noexcept {
        context_t context {database_, nullptr};
        status_t status;

        auto maybe_snapshots = context.snapshots();
        if (!maybe_snapshots) return maybe_snapshots.release_status();

        ustore_snapshot_drop_t snapshot_drop {};
        snapshot_drop.db = database_;
        snapshot_drop.error = status.member_ptr();
        for (ustore_snapshot_t const id : *maybe_snapshots) {
            snapshot_drop.id = id;
            ustore_snapshot_drop(&snapshot_drop);
            if (!status) return status;
        }

        auto maybe_listed = context.collections();
        if (!maybe_listed) return maybe_listed.release_status();

        ustore_collection_drop_t collection_drop {};
        collection_drop.db = database_;
        collection_drop.error = status.member_ptr();
        collection_drop.mode = ustore_drop_keys_vals_handle_k;
        for (ustore_collection_t const id : maybe_listed->ids) {
            collection_drop.id = id;
            ustore_collection_drop(&collection_drop);
            if (!status) return status;
        }

        collection_drop.id = ustore_collection_main_k;
        collection_drop.mode = ustore_drop_keys_vals_k;
        ustore_collection_drop(&collection_drop);
        return status;
    }
};

} // namespace unum::ustore
