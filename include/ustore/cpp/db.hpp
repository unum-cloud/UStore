/**
 * @file db.hpp
 * @author Ashot Vardanian
 * @date 26 Jun 2022
 * @addtogroup Cpp
 *
 * @brief C++ bindings for "ustore/db.h".
 */

#pragma once
#include <string>  // NULL-terminated names
#include <cstring> // `std::strlen`
#include <memory>  // `std::enable_shared_from_this`

#include "ustore/ustore.h"
#include "ustore/cpp/blobs_collection.hpp"
#include "ustore/cpp/docs_collection.hpp"
#include "ustore/cpp/graph_collection.hpp"

namespace unum::ustore {

using snapshots_list_t = ptr_range_gt<ustore_snapshot_t>;
struct collections_list_t {
    ptr_range_gt<ustore_collection_t> ids;
    strings_tape_iterator_t names;
};

/**
 * @brief A DBMS client for a single thread.
 *
 * May be used not only as a consistency warrant, but also a performance
 * optimization, as batched writes will be stored in a DB-optimal way
 * until being commited, which reduces the preprocessing overhead for DB.
 *
 * @see ACID: https://en.wikipedia.org/wiki/ACID
 *
 * ## Class Specs
 * - Concurrency: Thread-safe, for @b unique arenas.
 *   For details, "Memory Management" section @c blobs_ref_gt
 * - Lifetime: Doesn't commit on destruction. @c txn_guard_t.
 * - Copyable: No.
 * - Exceptions: Never.
 */
class context_t : public std::enable_shared_from_this<context_t> {
  protected:
    ustore_database_t db_ {nullptr};
    ustore_transaction_t txn_ {nullptr};
    ustore_snapshot_t snap_ {};
    arena_t arena_ {nullptr};

  public:
    inline context_t() noexcept : arena_(nullptr) {}
    inline context_t(ustore_database_t db, ustore_transaction_t txn = nullptr, ustore_snapshot_t snap = {}) noexcept
        : db_(db), txn_(txn), snap_(snap), arena_(db) {}
    inline context_t(context_t const&) = delete;
    inline context_t(context_t&& other) noexcept
        : db_(other.db_), txn_(std::exchange(other.txn_, nullptr)), snap_(std::exchange(other.snap_, {})),
          arena_(std::exchange(other.arena_, {nullptr})) {}
    inline context_t& operator=(context_t&& other) noexcept {
        std::swap(db_, other.db_);
        std::swap(txn_, other.txn_);
        std::swap(snap_, other.snap_);
        std::swap(arena_, other.arena_);
        return *this;
    }

    inline ~context_t() noexcept {
        ustore_transaction_free(txn_);
        txn_ = nullptr;

        status_t status;
        ustore_snapshot_drop_t snap_drop {
            .db = db_,
            .error = status.member_ptr(),
            .id = snap_,
        };
        ustore_snapshot_drop(&snap_drop);
        snap_ = 0;
    }

    inline ustore_database_t db() const noexcept { return db_; }
    inline ustore_transaction_t txn() const noexcept { return txn_; }
    inline ustore_snapshot_t snap() const noexcept { return snap_; }
    inline operator ustore_transaction_t() const noexcept { return txn_; }

    inline void set_snapshot(ustore_snapshot_t snap) noexcept { snap_ = snap; }

    blobs_ref_gt<places_arg_t> operator[](strided_range_gt<collection_key_t const> collections_and_keys) noexcept {
        places_arg_t arg;
        arg.collections_begin = collections_and_keys.members(&collection_key_t::collection).begin();
        arg.keys_begin = collections_and_keys.members(&collection_key_t::key).begin();
        arg.count = collections_and_keys.size();
        return {db_, txn_, snap_, std::move(arg), arena_};
    }

    blobs_ref_gt<places_arg_t> operator[](
        strided_range_gt<collection_key_field_t const> collections_and_keys) noexcept {
        places_arg_t arg;
        arg.collections_begin = collections_and_keys.members(&collection_key_field_t::collection).begin();
        arg.keys_begin = collections_and_keys.members(&collection_key_field_t::key).begin();
        arg.fields_begin = collections_and_keys.members(&collection_key_field_t::field).begin();
        arg.count = collections_and_keys.size();
        return {db_, txn_, snap_, std::move(arg), arena_};
    }

    blobs_ref_gt<places_arg_t> operator[](keys_view_t keys) noexcept { //
        places_arg_t arg;
        arg.keys_begin = keys.begin();
        arg.count = keys.size();
        return {db_, txn_, snap_, std::move(arg), arena_};
    }

    template <typename keys_arg_at>
    blobs_ref_gt<keys_arg_at> operator[](keys_arg_at&& keys) noexcept { //
        return blobs_ref_gt<keys_arg_at> {db_, txn_, snap_, std::forward<keys_arg_at>(keys), arena_.member_ptr()};
    }

    expected_gt<blobs_collection_t> operator[](ustore_str_view_t name) noexcept { return find(name); }

    template <typename collection_at = blobs_collection_t>
    collection_at main() noexcept {
        return collection_at {db_, ustore_collection_main_k, txn_, snap_, arena_.member_ptr()};
    }

    expected_gt<collections_list_t> collections() noexcept {
        ustore_size_t count = 0;
        ustore_str_span_t names = nullptr;
        ustore_collection_t* ids = nullptr;
        status_t status;
        ustore_collection_list_t collection_list {};
        collection_list.db = db_;
        collection_list.error = status.member_ptr();
        collection_list.transaction = txn_;
        collection_list.snapshot = snap_;
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

    expected_gt<snapshots_list_t> snapshots() noexcept {
        ustore_size_t count = 0;
        ustore_str_span_t names = nullptr;
        ustore_snapshot_t* ids = nullptr;
        status_t status;
        ustore_snapshot_list_t snapshots_list {};
        snapshots_list.db = db_;
        snapshots_list.error = status.member_ptr();
        snapshots_list.arena = arena_.member_ptr();
        snapshots_list.count = &count;
        snapshots_list.ids = &ids;

        ustore_snapshot_list(&snapshots_list);
        snapshots_list_t result = {ids, ids + count};
        return {std::move(status), std::move(result)};
    }

    expected_gt<bool> contains(std::string_view name) noexcept {

        if (name.empty())
            return true;

        auto maybe_cols = collections();
        if (!maybe_cols)
            return maybe_cols.release_status();

        auto cols = *maybe_cols;
        auto name_it = cols.names;
        auto id_it = cols.ids.begin();
        for (; id_it != cols.ids.end(); ++id_it, ++name_it) {
            if (*name_it != name)
                continue;
            return true;
        }
        return false;
    }

    /**
     * @brief Provides a view of a single collection synchronized with the transaction.
     * @tparam collection_at Can be a @c blobs_collection_t, @c docs_collection_t, @c graph_collection_t.
     */
    template <typename collection_at = blobs_collection_t>
    expected_gt<collection_at> find(std::string_view name = {}) noexcept {

        if (name.empty())
            return collection_at {db_, ustore_collection_main_k, txn_, {}, arena_.member_ptr()};

        auto maybe_cols = collections();
        if (!maybe_cols)
            return maybe_cols.release_status();

        auto cols = *maybe_cols;
        auto name_it = cols.names;
        auto id_it = cols.ids.begin();
        for (; id_it != cols.ids.end(); ++id_it, ++name_it)
            if (*name_it == name)
                return collection_at {db_, *id_it, txn_, {}, arena_.member_ptr()};

        return status_t::status_view("No such collection is present");
    }

    /**
     * @brief Clears the stare of transaction, preserving the underlying memory,
     * cleaning it, and labeling it with a new "sequence number" or "generation".
     *
     * @param snapshot Controls whether a consistent view of the entirety of DB
     *                 must be created for this transaction. Is required for
     *                 long-running analytical tasks with strong consistency
     *                 requirements.
     */
    status_t reset() noexcept {
        status_t status;
        ustore_transaction_init_t txn_init {};
        txn_init.db = db_;
        txn_init.error = status.member_ptr();
        txn_init.transaction = &txn_;

        ustore_transaction_init(&txn_init);
        return status;
    }

    /**
     * @brief Attempts to commit all the updates to the DB.
     * Fails if any single one of the updates fails.
     */
    status_t commit(bool flush = false) noexcept {
        status_t status;
        auto options = flush ? ustore_option_write_flush_k : ustore_options_default_k;
        ustore_transaction_commit_t txn_commit {};
        txn_commit.db = db_;
        txn_commit.error = status.member_ptr();
        txn_commit.transaction = txn_;
        txn_commit.options = options;
        ustore_transaction_commit(&txn_commit);
        return status;
    }

    expected_gt<ustore_sequence_number_t> sequenced_commit(bool flush = false) noexcept {
        status_t status;
        auto options = flush ? ustore_option_write_flush_k : ustore_options_default_k;
        ustore_sequence_number_t sequence_number = std::numeric_limits<ustore_sequence_number_t>::max();
        ustore_transaction_commit_t txn_commit {};
        txn_commit.db = db_;
        txn_commit.error = status.member_ptr();
        txn_commit.transaction = txn_;
        txn_commit.options = options;
        txn_commit.sequence_number = &sequence_number;
        ustore_transaction_commit(&txn_commit);
        return {std::move(status), std::move(sequence_number)};
    }
};

using transaction_t = context_t;

/**
 * @brief DataBase is a "collection of named collections",
 * essentially a transactional @b map<string,map<id,string>>.
 * Or in Python terms: @b dict[str,dict[int,str]].
 *
 * ## Class Specs
 * - Concurrency: @b Thread-Safe, except for `open`, `close`.
 * - Lifetime: @b Must live longer then last collection referencing it.
 * - Copyable: No.
 * - Exceptions: Never.
 */
class database_t : public std::enable_shared_from_this<database_t> {
    ustore_database_t db_ = nullptr;

  public:
    database_t() = default;
    database_t(database_t const&) = delete;
    database_t(database_t&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}
    operator ustore_database_t() const noexcept { return db_; }

    status_t open(ustore_str_view_t config = nullptr) noexcept {
        status_t status;
        ustore_database_init_t database_init {};
        database_init.config = config;
        database_init.db = &db_;
        database_init.error = status.member_ptr();
        ustore_database_init(&database_init);
        return status;
    }

    void close() noexcept {
        ustore_database_free(db_);
        db_ = nullptr;
    }

    ~database_t() noexcept {
        if (db_)
            close();
    }

    expected_gt<context_t> transact() noexcept {

        status_t status {};
        ustore_transaction_t raw {};
        ustore_transaction_init_t txn_init {};
        txn_init.db = db_;
        txn_init.error = status.member_ptr();
        txn_init.transaction = &raw;

        ustore_transaction_init(&txn_init);
        if (!status)
            return {std::move(status), context_t {db_, nullptr}};
        else
            return context_t {db_, raw};
    }

    expected_gt<context_t> snapshot() noexcept {
        status_t status;
        ustore_snapshot_t raw = {};
        ustore_snapshot_create_t snap_create {
            .db = db_,
            .error = status.member_ptr(),
            .id = &raw,
        };
        ustore_snapshot_create(&snap_create);
        if (!status)
            return {std::move(status), context_t {db_, nullptr}};
        else
            return context_t {db_, nullptr, raw};
    }

    template <typename collection_at = blobs_collection_t>
    collection_at main() noexcept {
        return collection_at {db_, ustore_collection_main_k};
    }

    operator blobs_collection_t() noexcept { return main(); }
    expected_gt<blobs_collection_t> operator[](ustore_str_view_t name) noexcept { return find_or_create(name); }
    expected_gt<bool> contains(std::string_view name) noexcept { return context_t {db_, nullptr}.contains(name); }

    template <typename collection_at = blobs_collection_t>
    expected_gt<collection_at> create(ustore_str_view_t name, ustore_str_view_t config = "") noexcept {
        status_t status;
        ustore_collection_t collection = ustore_collection_main_k;
        ustore_collection_create_t collection_init {};
        collection_init.db = db_;
        collection_init.error = status.member_ptr();
        collection_init.name = name;
        collection_init.config = config;
        collection_init.id = &collection;

        ustore_collection_create(&collection_init);
        if (!status)
            return status;
        else
            return collection_at {db_, collection, nullptr};
    }

    template <typename collection_at = blobs_collection_t>
    expected_gt<collection_at> find(std::string_view name = {}) noexcept {
        auto maybe_id = context_t {db_, nullptr}.find(name);
        if (!maybe_id)
            return maybe_id.release_status();
        return collection_at {db_, *maybe_id, nullptr};
    }

    template <typename collection_at = blobs_collection_t>
    expected_gt<collection_at> find_or_create(ustore_str_view_t name) noexcept {
        auto maybe_id = context_t {db_, nullptr}.find(name);
        if (maybe_id)
            return collection_at {db_, *maybe_id, nullptr};
        return create<collection_at>(name);
    }

    status_t drop(std::string_view name) noexcept {
        auto maybe_collection = find(name);
        if (!maybe_collection)
            return maybe_collection.release_status();
        return maybe_collection->drop();
    }

    status_t clear() noexcept {
        auto context = context_t {db_, nullptr};

        status_t status;
        // Remove snapshots
        auto maybe_snaps = context.snapshots();
        auto snaps = *maybe_snaps;

        ustore_snapshot_drop_t snapshot_drop {};
        snapshot_drop.db = db_;
        snapshot_drop.error = status.member_ptr();

        for (auto id : snaps) {
            snapshot_drop.id = id;
            ustore_snapshot_drop(&snapshot_drop);
            if (!status)
                return status;
        }

        // Remove named collections
        auto maybe_cols = context.collections();
        if (!maybe_cols)
            return maybe_cols.release_status();

        auto cols = *maybe_cols;
        ustore_collection_drop_t collection_drop {};
        collection_drop.db = db_;
        collection_drop.error = status.member_ptr();
        collection_drop.mode = ustore_drop_keys_vals_handle_k;

        for (auto id : cols.ids) {
            collection_drop.id = id;
            ustore_collection_drop(&collection_drop);
            if (!status)
                return status;
        }

        // Clear the main collection
        collection_drop.id = ustore_collection_main_k;
        collection_drop.mode = ustore_drop_keys_vals_k;
        ustore_collection_drop(&collection_drop);
        return status;
    }
};

} // namespace unum::ustore
