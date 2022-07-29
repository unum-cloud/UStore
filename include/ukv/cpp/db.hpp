/**
 * @file db.hpp
 * @author Ashot Vardanian
 * @date 26 Jun 2022
 * @brief C++ bindings for @see "ukv/db.h".
 */

#pragma once
#include <string>  // NULL-terminated names
#include <cstring> // `std::strlen`
#include <memory>  // `std::enable_shared_from_this`

#include "ukv/ukv.h"
#include "ukv/cpp/members_ref.hpp"
#include "ukv/cpp/graph_ref.hpp"
#include "ukv/cpp/keys_stream.hpp"

namespace unum::ukv {

/**
 * @brief Collection is persistent associative container,
 * essentially a transactional @b `map<id,string>`.
 * Or in Python terms: @b `dict[int,str]`.
 *
 * Generally cheap to construct. Can address @b both collections
 * "HEAD" state, as well as some "snapshot"/"transaction" view.
 *
 * @section Class Specs
 * > Concurrency: Thread-safe, for @b unique arenas.
 *   For details, @see `members_ref_gt` @section "Memory Management"
 * > Lifetime: @b Must live shorter then the DB it belongs to.
 * > Copyable: No.
 * > Exceptions: Only the `size` method.
 *
 * @section Formats
 * Formats @b loosely describe the data stored in the collection
 * and @b exactly define the communication through this exact handle.
 * Example: Same collection can accept similar formats, such
 * as `ukv_format_json_k` and `ukv_format_msgpack_k`. Both will be
 * converted into some internal hierarchical representation
 * in "Document Collections", and can later be queried with
 * any "Document Format".
 */
class collection_t {
    ukv_t db_ = nullptr;
    ukv_collection_t col_ = ukv_default_collection_k;
    ukv_txn_t txn_ = nullptr;
    arena_t arena_;
    ukv_format_t format_ = ukv_format_binary_k;

  public:
    inline collection_t() noexcept : arena_(nullptr) {}
    inline collection_t(ukv_t db_ptr,
                        ukv_collection_t col_ptr = ukv_default_collection_k,
                        ukv_txn_t txn = nullptr,
                        ukv_format_t format = ukv_format_binary_k) noexcept
        : db_(db_ptr), col_(col_ptr), txn_(txn), arena_(db_), format_(format) {}

    inline collection_t(collection_t&& other) noexcept
        : db_(other.db_), col_(std::exchange(other.col_, ukv_default_collection_k)),
          txn_(std::exchange(other.txn_, nullptr)), arena_(std::exchange(other.arena_, {nullptr})),
          format_(std::exchange(other.format_, ukv_format_binary_k)) {}

    inline ~collection_t() noexcept {
        ukv_collection_free(db_, col_);
        col_ = nullptr;
    }
    inline collection_t& operator=(collection_t&& other) noexcept {
        std::swap(db_, other.db_);
        std::swap(col_, other.col_);
        std::swap(txn_, other.txn_);
        std::swap(arena_, other.arena_);
        std::swap(format_, other.format_);
        return *this;
    }

    inline operator ukv_collection_t() const noexcept { return col_; }
    inline ukv_collection_t* member_ptr() noexcept { return &col_; }
    inline ukv_t db() const noexcept { return db_; }
    inline ukv_txn_t txn() const noexcept { return txn_; }
    inline graph_ref_t as_graph() noexcept { return {db_, txn_, col_, arena_}; }
    inline collection_t& as(ukv_format_t format) noexcept {
        format_ = format;
        return *this;
    }

    inline keys_range_t keys(ukv_key_t min_key = std::numeric_limits<ukv_key_t>::min(),
                             ukv_key_t max_key = ukv_key_unknown_k,
                             std::size_t read_ahead = keys_stream_t::default_read_ahead_k) const noexcept {
        return {db_, txn_, col_, min_key, max_key, read_ahead};
    }

    inline expected_gt<size_range_t> size_range() const noexcept {
        auto maybe = keys().find_size();
        return {maybe.release_status(), std::move(maybe->cardinality)};
    }

    std::size_t size() const noexcept(false) {
        auto maybe = size_range();
        maybe.throw_unhandled();
        return (maybe->min + maybe->max) / 2;
    }

    inline members_ref_gt<keys_arg_t> operator[](keys_view_t const& keys) noexcept { return at(keys); }
    inline members_ref_gt<keys_arg_t> at(keys_view_t const& keys) noexcept {
        keys_arg_t arg;
        arg.collections_begin = &col_;
        arg.keys_begin = keys.begin();
        arg.count = keys.size();
        return {db_, txn_, {std::move(arg)}, arena_, format_};
    }

    template <typename keys_arg_at>
    auto operator[](keys_arg_at&& keys) noexcept { //
        return at(std::forward<keys_arg_at>(keys));
    }

    template <typename keys_arg_at>
    auto at(keys_arg_at&& keys) noexcept { //
        constexpr bool is_one_k = is_one<keys_arg_at>();
        if constexpr (is_one_k) {
            using result_t = members_ref_gt<col_key_field_t>;
            using plain_t = std::remove_reference_t<keys_arg_at>;
            // ? We may want to warn the users, that the collection property will be shadowed:
            // static_assert(!sfinae_has_collection_gt<plain_t>::value, "Overwriting existing collection!");
            col_key_field_t arg;
            arg.collection = col_;
            if constexpr (std::is_integral_v<plain_t>)
                arg.key = keys;
            else
                arg.key = keys.key;

            if constexpr (sfinae_has_field_gt<plain_t>::value)
                arg.field = keys.field;
            return result_t {db_, txn_, arg, arena_, format_};
        }
        else {
            using locations_t = locations_in_collection_gt<keys_arg_at>;
            using result_t = members_ref_gt<locations_t>;
            return result_t {db_, txn_, locations_t {std::forward<keys_arg_at>(keys), col_}, arena_, format_};
        }
    }
};

/**
 * @brief Transaction in a classical DBMS sense.
 *
 * May be used not only as a consistency warrant, but also a performance
 * optimization, as batched writes will be stored in a DB-optimal way
 * until being commited, which reduces the preprocessing overhead for DB.
 * For details, @see ACID: https://en.wikipedia.org/wiki/ACID
 *
 * @section Class Specs
 * > Concurrency: Thread-safe, for @b unique arenas.
 *   For details, @see `members_ref_gt` @section "Memory Management"
 * > Lifetime: Doesn't commit on destruction. @see `txn_guard_t`.
 * > Copyable: No.
 * > Exceptions: Never.
 */
class txn_t {
    ukv_t db_ = nullptr;
    ukv_txn_t txn_ = nullptr;
    arena_t arena_;

  public:
    inline txn_t() noexcept : arena_(nullptr) {}
    inline txn_t(ukv_t db, ukv_txn_t txn) noexcept : db_(db), txn_(txn), arena_(db) {}
    inline txn_t(txn_t const&) = delete;
    inline txn_t(txn_t&& other) noexcept
        : db_(other.db_), txn_(std::exchange(other.txn_, nullptr)), arena_(std::exchange(other.arena_, {nullptr})) {}

    inline txn_t& operator=(txn_t&& other) noexcept {
        std::swap(db_, other.db_);
        std::swap(txn_, other.txn_);
        std::swap(arena_, other.arena_);
        return *this;
    }

    inline ukv_t db() const noexcept { return db_; }
    inline operator ukv_txn_t() const noexcept { return txn_; }

    inline ~txn_t() noexcept {
        ukv_txn_free(db_, txn_);
        txn_ = nullptr;
    }

    members_ref_gt<keys_arg_t> operator[](strided_range_gt<col_key_t const> cols_and_keys) noexcept {
        keys_arg_t arg;
        arg.collections_begin = cols_and_keys.members(&col_key_t::collection).begin();
        arg.keys_begin = cols_and_keys.members(&col_key_t::key).begin();
        arg.count = cols_and_keys.size();
        return {db_, txn_, std::move(arg), arena_};
    }

    members_ref_gt<keys_arg_t> operator[](strided_range_gt<col_key_field_t const> cols_and_keys) noexcept {
        keys_arg_t arg;
        arg.collections_begin = cols_and_keys.members(&col_key_field_t::collection).begin();
        arg.keys_begin = cols_and_keys.members(&col_key_field_t::key).begin();
        arg.fields_begin = cols_and_keys.members(&col_key_field_t::field).begin();
        arg.count = cols_and_keys.size();
        return {db_, txn_, std::move(arg), arena_};
    }

    members_ref_gt<keys_arg_t> operator[](keys_view_t keys) noexcept { //
        keys_arg_t arg;
        arg.keys_begin = keys.begin();
        arg.count = keys.size();
        return {db_, txn_, std::move(arg), arena_};
    }

    template <typename keys_arg_at>
    members_ref_gt<keys_arg_at> operator[](keys_arg_at keys) noexcept { //
        return {db_, txn_, std::move(keys), arena_};
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
    status_t reset(bool snapshot = false) noexcept {
        status_t status;
        auto options = snapshot ? ukv_option_txn_snapshot_k : ukv_options_default_k;
        ukv_txn_begin(db_, 0, options, &txn_, status.member_ptr());
        return status;
    }

    /**
     * @brief Attempts to commit all the updates to the DB.
     * Fails if any single one of the updates fails.
     */
    status_t commit(bool flush = false) noexcept {
        status_t status;
        auto options = flush ? ukv_option_write_flush_k : ukv_options_default_k;
        ukv_txn_commit(txn_, options, status.member_ptr());
        return status;
    }

    expected_gt<collection_t> operator[](ukv_str_view_t name) noexcept { return collection(name); }
    operator expected_gt<collection_t>() noexcept { return collection(""); }

    /**
     * @brief Provides a view of a single collection synchronized with the transaction.
     */
    expected_gt<collection_t> collection(ukv_str_view_t name = "") noexcept {
        status_t status;
        ukv_collection_t col = nullptr;
        ukv_collection_open(db_, name, nullptr, &col, status.member_ptr());
        if (!status)
            return status;
        else
            return collection_t {db_, col, txn_};
    }
};

/**
 * @brief DataBase is a "collection of named collections",
 * essentially a transactional @b `map<string,map<id,string>>`.
 * Or in Python terms: @b `dict[str,dict[int,str]]`.
 *
 * @section Class Specs
 * > Concurrency: @b Thread-Safe, except for `open`, `close`.
 * > Lifetime: @b Must live longer then last collection referencing it.
 * > Copyable: No.
 * > Exceptions: Never.
 */
class db_t : public std::enable_shared_from_this<db_t> {
    ukv_t db_ = nullptr;

  public:
    db_t() = default;
    db_t(db_t const&) = delete;
    db_t(db_t&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

    status_t open(std::string const& config = "") {
        status_t status;
        ukv_open(config.c_str(), &db_, status.member_ptr());
        return status;
    }

    void close() {
        ukv_free(db_);
        db_ = nullptr;
    }

    ~db_t() {
        if (db_)
            close();
    }

    inline operator ukv_t() const noexcept { return db_; }

    /**
     * @brief Checks if a collection with requested `name` is present in the DB.
     * @param memory Temporary memory required for storing the execution results.
     */
    expected_gt<bool> contains(std::string_view name, arena_t& memory) noexcept {
        if (name.empty())
            return true;

        status_t status;
        ukv_size_t count = 0;
        ukv_str_view_t names = nullptr;
        ukv_collection_list(db_, &count, &names, memory.member_ptr(), status.member_ptr());
        if (!status)
            return status;

        while (count) {
            auto len = std::strlen(names);
            auto found = std::string_view(names, len);
            if (found == name)
                return true;

            names += len + 1;
            --count;
        }
        return false;
    }

    /**
     * @brief Checks if a collection with requested `name` is present in the DB.
     */
    expected_gt<bool> contains(std::string_view name) noexcept {
        arena_t arena(db_);
        return contains(name, arena);
    }

    expected_gt<collection_t> operator[](ukv_str_view_t name) noexcept { return collection(name); }
    operator expected_gt<collection_t>() noexcept { return collection(""); }
    expected_gt<collection_t> operator*() noexcept { return collection(""); }

    expected_gt<collection_t> collection(ukv_str_view_t name = "", ukv_format_t format = ukv_format_binary_k) noexcept {
        status_t status;
        ukv_collection_t col = nullptr;
        ukv_collection_open(db_, name, nullptr, &col, status.member_ptr());
        if (!status)
            return status;
        else
            return collection_t {db_, col, nullptr, format};
    }

    status_t remove(ukv_str_view_t name) noexcept {
        status_t status;
        ukv_collection_remove(db_, name, status.member_ptr());
        return status;
    }

    expected_gt<txn_t> transact() {
        status_t status;
        ukv_txn_t raw = nullptr;
        ukv_txn_begin(db_, 0, ukv_options_default_k, &raw, status.member_ptr());
        if (!status)
            return {std::move(status), txn_t {db_, nullptr}};
        else
            return txn_t {db_, raw};
    }
};

} // namespace unum::ukv
