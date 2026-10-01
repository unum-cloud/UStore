/**
 *  @file include/ustore/blobs_collection.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief A handle to one binary collection, from which value proxies and key ranges are made.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <concepts>         // `std::same_as`
#include <initializer_list> // `std::initializer_list`
#include <type_traits>      // `std::remove_cvref_t`
#include <utility>          // `std::exchange`, `std::forward`, `std::swap`

#include "ustore/blobs_range.hpp" // `blobs_range_t`, `keys_range_t`, `pairs_range_t`, `size_range_t`
#include "ustore/blobs_ref.hpp"   // `blobs_ref`
#include "ustore/db.h"
#include "ustore/ranges.hpp"      // `batch_concept`, `strided`
#include "ustore/ranges_args.hpp" // `keys_view_t`, `places_arg_t`, `locations_in_collection`
#include "ustore/status.hpp"      // `status_t`, `expected`
#include "ustore/types.hpp"       // `any_arena_t`, `collection_key_field_t`, `has_field_concept`

namespace unum::ustore {

/** A handle to one binary collection, cheap to construct, that addresses either the latest state or
 *  the view of a transaction or snapshot.
 *
 *  Proxies made from one handle share its arena, so a handle serves one thread at a time. A copy stays
 *  bound to the same transaction but starts with an arena of its own, and the handle must not outlive
 *  its database. */
class blobs_collection_t {
    ustore_database_t database_ {nullptr};
    ustore_collection_id_t collection_ {ustore_collection_main_k};
    ustore_transaction_t transaction_ {nullptr};
    ustore_snapshot_t snapshot_ {};
    any_arena_t arena_ {nullptr};

  public:
    blobs_collection_t() noexcept : arena_(nullptr) {}
    blobs_collection_t(ustore_database_t database, ustore_collection_id_t collection = ustore_collection_main_k,
                       ustore_transaction_t transaction = nullptr, ustore_snapshot_t snapshot = {},
                       ustore_arena_t* arena = nullptr) noexcept
        : database_(database), collection_(collection), transaction_(transaction), snapshot_(snapshot),
          arena_(database_, arena) {}

    blobs_collection_t(blobs_collection_t&& other) noexcept
        : database_(other.database_), collection_(std::exchange(other.collection_, ustore_collection_main_k)),
          transaction_(std::exchange(other.transaction_, nullptr)), snapshot_(std::exchange(other.snapshot_, {})),
          arena_(std::exchange(other.arena_, {nullptr})) {}

    blobs_collection_t& operator=(blobs_collection_t&& other) noexcept {
        std::swap(database_, other.database_);
        std::swap(collection_, other.collection_);
        std::swap(transaction_, other.transaction_);
        std::swap(snapshot_, other.snapshot_);
        std::swap(arena_, other.arena_);
        return *this;
    }

    blobs_collection_t(blobs_collection_t const& other) noexcept
        : database_(other.database_), collection_(other.collection_), transaction_(other.transaction_),
          snapshot_(other.snapshot_), arena_(other.database_) {}

    blobs_collection_t& operator=(blobs_collection_t const& other) noexcept {
        database_ = other.database_;
        collection_ = other.collection_;
        transaction_ = other.transaction_;
        snapshot_ = other.snapshot_;
        arena_ = any_arena_t(other.database_);
        return *this;
    }

    operator ustore_collection_id_t() const noexcept { return collection_; }
    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }
    ustore_arena_t* member_arena() noexcept { return arena_.member_ptr(); }
    ustore_database_t db() const noexcept { return database_; }
    ustore_transaction_t txn() const noexcept { return transaction_; }
    ustore_snapshot_t snap() const noexcept { return snapshot_; }

    blobs_range_t members(ustore_key_t min_key = smallest_key_k, ustore_key_t max_key = largest_key_k) const noexcept {
        return {database_, transaction_, snapshot_, collection_, min_key, max_key};
    }

    keys_range_t keys(ustore_key_t min_key = smallest_key_k, ustore_key_t max_key = largest_key_k) const noexcept {
        return {members(min_key, max_key)};
    }

    pairs_range_t items(ustore_key_t min_key = smallest_key_k, ustore_key_t max_key = largest_key_k) const noexcept {
        return {members(min_key, max_key)};
    }

    expected<size_range_t> size_range() const noexcept {
        auto [estimates, size_estimates_status] = members().size_estimates();
        if (size_estimates_status.failed()) return size_estimates_status;
        return estimates.cardinality;
    }

    /** Counts the keys by streaming through all of them. */
    expected<std::size_t> size() const noexcept { return keys().size(); }

    status_t clear_values() noexcept { return drop(ustore_drop_vals_k); }
    status_t clear() noexcept { return drop(ustore_drop_keys_vals_k); }
    status_t drop() noexcept { return drop(ustore_drop_keys_vals_handle_k); }

    blobs_ref<places_arg_t> operator[](std::initializer_list<ustore_key_t> keys) noexcept { return at(keys); }
    blobs_ref<places_arg_t> at(std::initializer_list<ustore_key_t> keys) noexcept { return at(strided(keys)); }

    blobs_ref<places_arg_t> operator[](keys_view_t keys) noexcept { return at(keys); }
    blobs_ref<places_arg_t> at(keys_view_t keys) noexcept {
        places_arg_t arg;
        arg.collections_begin = &collection_;
        arg.keys_begin = keys.begin();
        arg.count = keys.size();
        return {database_, transaction_, snapshot_, std::move(arg), arena_};
    }

    template <typename keys_arg_>
    auto operator[](keys_arg_&& keys) noexcept {
        return at(std::forward<keys_arg_>(keys));
    }

    /** Addresses one location or a batch of them inside this collection, whose own collection wins. */
    template <typename keys_arg_>
    auto at(keys_arg_&& keys) noexcept {
        using plain_t = std::remove_cvref_t<keys_arg_>;
        if constexpr (!batch_concept<keys_arg_>) {
            collection_key_field_t arg;
            arg.collection = collection_;
            if constexpr (std::same_as<plain_t, ustore_key_t>) arg.key = keys;
            else arg.key = keys.key;
            if constexpr (has_field_concept<plain_t>) arg.field = keys.field;
            return blobs_ref<collection_key_field_t> {database_, transaction_, snapshot_, std::move(arg), arena_};
        }
        else {
            using locations_t = locations_in_collection<keys_arg_>;
            return blobs_ref<locations_t> {database_, transaction_, snapshot_,
                                           locations_t {std::forward<keys_arg_>(keys), collection_}, arena_};
        }
    }

  private:
    status_t drop(ustore_drop_mode_t mode) noexcept {
        ustore_error_t error {};
        ustore_collection_drop_t collection_drop {};
        collection_drop.db = database_;
        collection_drop.error = &error;
        collection_drop.collection = collection_;
        collection_drop.mode = mode;
        ustore_collection_drop(&collection_drop);
        return status_t {error};
    }
};

} // namespace unum::ustore
