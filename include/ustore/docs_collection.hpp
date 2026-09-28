/**
 *  @file include/ustore/docs_collection.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief Handle to a collection of JSON documents.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <concepts>         // `std::same_as`
#include <initializer_list> // `std::initializer_list`
#include <type_traits>      // `std::remove_cvref_t`
#include <utility>          // `std::exchange`, `std::forward`, `std::swap`

#include <ustore/blobs_range.hpp> // `blobs_range_t`, `keys_range_t`, `size_range_t`
#include <ustore/docs.h>
#include <ustore/docs_ref.hpp>    // `docs_ref`
#include <ustore/ranges.hpp>      // `batch_concept`, `strided`
#include <ustore/ranges_args.hpp> // `keys_view_t`, `places_arg_t`, `locations_in_collection`
#include <ustore/status.hpp>      // `status_t`, `expected`
#include <ustore/types.hpp>       // `any_arena_t`, `collection_key_field_t`, `has_field_concept`

namespace unum::ustore {

/**
 *  @brief Transactional map from keys to JSON documents, cheap to construct.
 *
 *  A handle addresses either the current state of a collection or its view inside a transaction or a snapshot,
 *  and never outlives its database.
 *  Handles are thread-safe while every thread reads through its own arena, and a copy starts with a new empty arena
 *  attached to the same transaction.
 *  The type of a handle shapes only how values cross it, since documents are stored as JSON whatever they arrive as.
 */
class docs_collection_t {
    ustore_database_t db_ {nullptr};
    ustore_collection_id_t collection_ {ustore_collection_main_k};
    ustore_transaction_t transaction_ {nullptr};
    ustore_snapshot_t snapshot_ {};
    any_arena_t arena_ {nullptr};
    ustore_doc_field_type_t type_ {ustore_doc_field_json_k};

    status_t drop_with(ustore_drop_mode_t mode) noexcept {
        ustore_error_t error {};
        ustore_collection_drop_t collection_drop {};
        collection_drop.db = db_;
        collection_drop.error = &error;
        collection_drop.collection = collection_;
        collection_drop.mode = mode;
        ustore_collection_drop(&collection_drop);
        return status_t {error};
    }

  public:
    docs_collection_t() noexcept = default;

    docs_collection_t(ustore_database_t db, ustore_collection_id_t collection = ustore_collection_main_k,
                      ustore_transaction_t transaction = nullptr, ustore_snapshot_t snapshot = {},
                      ustore_arena_t* arena = nullptr, ustore_doc_field_type_t type = ustore_doc_field_json_k) noexcept
        : db_(db), collection_(collection), transaction_(transaction), snapshot_(snapshot), arena_(db_, arena),
          type_(type) {}

    docs_collection_t(docs_collection_t&& other) noexcept
        : db_(other.db_), collection_(std::exchange(other.collection_, ustore_collection_main_k)),
          transaction_(std::exchange(other.transaction_, nullptr)), snapshot_(std::exchange(other.snapshot_, {})),
          arena_(std::exchange(other.arena_, {nullptr})), type_(std::exchange(other.type_, ustore_doc_field_json_k)) {}

    docs_collection_t& operator=(docs_collection_t&& other) noexcept {
        std::swap(db_, other.db_);
        std::swap(collection_, other.collection_);
        std::swap(transaction_, other.transaction_);
        std::swap(snapshot_, other.snapshot_);
        std::swap(arena_, other.arena_);
        std::swap(type_, other.type_);
        return *this;
    }

    docs_collection_t(docs_collection_t const& other) noexcept
        : db_(other.db_), collection_(other.collection_), transaction_(other.transaction_), snapshot_(other.snapshot_),
          arena_(other.db_), type_(other.type_) {}

    docs_collection_t& operator=(docs_collection_t const& other) noexcept {
        db_ = other.db_;
        collection_ = other.collection_;
        transaction_ = other.transaction_;
        snapshot_ = other.snapshot_;
        arena_ = any_arena_t(other.db_);
        type_ = other.type_;
        return *this;
    }

    operator ustore_collection_id_t() const noexcept { return collection_; }
    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }
    ustore_arena_t* member_arena() noexcept { return arena_.member_ptr(); }
    ustore_database_t db() const noexcept { return db_; }
    ustore_transaction_t transaction() const noexcept { return transaction_; }
    ustore_snapshot_t snapshot() const noexcept { return snapshot_; }

    blobs_range_t members( //
        ustore_key_t min_key = smallest_key_k, ustore_key_t max_key = largest_key_k) const noexcept {
        return {db_, transaction_, snapshot_, collection_, min_key, max_key};
    }

    keys_range_t keys( //
        ustore_key_t min_key = smallest_key_k, ustore_key_t max_key = largest_key_k) const noexcept {
        return {members(min_key, max_key)};
    }

    expected<size_range_t> size_range() const noexcept {
        auto [estimates, size_estimates_status] = members().size_estimates();
        if (size_estimates_status.failed()) return size_estimates_status;
        return estimates.cardinality;
    }

    /** Estimates the number of documents as the middle of @c size_range. */
    expected<std::size_t> size() const noexcept {
        auto [range, size_range_status] = size_range();
        if (size_range_status.failed()) return size_range_status;
        return (range.min + range.max) / 2;
    }

    /** Empties every document, keeping the keys. */
    status_t clear_values() noexcept { return drop_with(ustore_drop_vals_k); }

    /** Removes every document, keeping the collection. */
    status_t clear() noexcept { return drop_with(ustore_drop_keys_vals_k); }

    /** Removes the collection with all its documents. */
    status_t drop() noexcept { return drop_with(ustore_drop_keys_vals_handle_k); }

    docs_ref<places_arg_t> operator[](std::initializer_list<ustore_key_t> keys) noexcept { return at(keys); }
    docs_ref<places_arg_t> at(std::initializer_list<ustore_key_t> keys) noexcept { return at(strided(keys)); }

    docs_ref<places_arg_t> operator[](keys_view_t keys) noexcept { return at(keys); }
    docs_ref<places_arg_t> at(keys_view_t keys) noexcept { return at(keys, type_); }

    docs_ref<places_arg_t> at(keys_view_t keys, ustore_doc_field_type_t type) noexcept {
        places_arg_t places;
        places.collections_begin = &collection_;
        places.keys_begin = keys.begin();
        places.count = keys.size();
        return {db_, transaction_, snapshot_, std::move(places), arena_, type};
    }

    template <typename keys_argument_>
    auto operator[](keys_argument_&& keys) noexcept {
        return at(std::forward<keys_argument_>(keys));
    }

    template <typename keys_argument_>
    auto at(keys_argument_&& keys) noexcept {
        return at(std::forward<keys_argument_>(keys), type_);
    }

    template <typename keys_argument_>
    auto at(keys_argument_&& keys, ustore_doc_field_type_t type) noexcept {
        if constexpr (!batch_concept<keys_argument_>) {
            using plain_t = std::remove_cvref_t<keys_argument_>;
            collection_key_field_t location;
            location.collection = collection_;
            if constexpr (std::same_as<plain_t, ustore_key_t>) location.key = keys;
            else location.key = keys.key;

            if constexpr (has_field_concept<plain_t>) location.field = keys.field;
            return docs_ref<collection_key_field_t> {db_, transaction_, snapshot_, std::move(location), arena_, type};
        }
        else {
            using locations_t = locations_in_collection<keys_argument_>;
            return docs_ref<locations_t> {db_,       transaction_,
                                          snapshot_, locations_t {std::forward<keys_argument_>(keys), collection_},
                                          arena_,    type};
        }
    }
};

} // namespace unum::ustore
