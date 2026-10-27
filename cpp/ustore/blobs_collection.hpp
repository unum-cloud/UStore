/**
 *  @file cpp/ustore/blobs_collection.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief Handles to one binary collection, from which value proxies and key ranges are made, and their deferred
 *      twins, whose verbs submit to an executor.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <concepts>         // `std::same_as`
#include <initializer_list> // `std::initializer_list`
#include <type_traits>      // `std::remove_cvref_t`
#include <utility>          // `std::forward`, `std::move`

#include "ustore/blobs_range.hpp" // `blobs_range_t`, `keys_range_t`, `pairs_range_t`, `size_range_t`
#include "ustore/blobs_ref.hpp"   // `blobs_read_ref`, `blobs_ref`
#include "ustore/db.h"
#include "ustore/ranges.hpp" // `batch_concept`, `strided`, `place_batch_t`, `values_view_t`, `owned_array_concept`
#include "ustore/status.hpp" // `status_t`, `expected`, `deferred`
#include "ustore/types.hpp"  // `arena_t`, `receiver_t`, `collection_key_field_t`

namespace unum::ustore {

class deferred_blobs_read_collection_t;
class deferred_blobs_collection_t;

#pragma region Collections

/** A handle to one binary collection that only reads, cheap to construct and to copy, which addresses the latest
 *  state or the view of a transaction or snapshot. It holds no state of its own, so any number of threads share it,
 *  and it never outlives its database. */
class blobs_read_collection_t {
  protected:
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};

    /** A @p reference_ proxy over one location or a batch of them inside this collection, whose own collection wins
     *  over the one a location names. */
    template <template <typename> typename reference_, typename keys_argument_type_>
    auto address_(keys_argument_type_&& keys) noexcept {
        using plain_t = std::remove_cvref_t<keys_argument_type_>;
        if constexpr (!batch_concept<keys_argument_type_>) {
            collection_key_field_t location;
            location.collection = collection_;
            if constexpr (std::same_as<plain_t, ustore_key_t>) location.key = keys;
            else location.key = keys.key;
            if constexpr (has_field_concept<plain_t>) location.field = keys.field;
            return reference_<collection_key_field_t> {receiver_, std::move(location)};
        }
        else {
            using locations_t = locations_in_collection<keys_argument_type_>;
            return reference_<locations_t> {receiver_,
                                            locations_t {std::forward<keys_argument_type_>(keys), collection_}};
        }
    }

    /** A @p reference_ proxy over a strided batch of keys inside this collection. */
    template <template <typename> typename reference_>
    reference_<place_batch_t> address_keys_(keys_view_t keys) noexcept {
        place_batch_t places;
        places.collections_begin = &collection_;
        places.keys_begin = keys.begin();
        places.count = keys.size();
        return {receiver_, std::move(places)};
    }

  public:

    /** The handle a snapshot hands out for this kind of collection, which only reads. */
    using read_only_t = blobs_read_collection_t;

    /** The twin a deferred receiver hands out, whose verbs answer deferred calls. */
    using deferred_t = deferred_blobs_read_collection_t;

    /** What the collections this kind of handle addresses hold, which a creation records and a lookup checks. */
    static constexpr ustore_modality_t modality_k = ustore_modality_blobs_k;

    blobs_read_collection_t() noexcept = default;
    blobs_read_collection_t(receiver_t receiver, ustore_collection_id_t collection) noexcept
        : receiver_(receiver), collection_(collection) {}

    operator ustore_collection_id_t() const noexcept { return collection_; }
    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }

    blobs_range_t members(ustore_key_t start_key = smallest_key_k,
                          ustore_key_t end_key = largest_key_k) const noexcept {
        return {receiver_, collection_, start_key, end_key};
    }

    keys_range_t keys(ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) const noexcept {
        return {members(start_key, end_key)};
    }

    pairs_range_t pairs(ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) const noexcept {
        return {members(start_key, end_key)};
    }

    expected<size_range_t> size_range() const noexcept {
        auto [estimates, size_estimates_status] = members().size_estimates();
        if (size_estimates_status.failed()) return size_estimates_status;
        return estimates.cardinality;
    }

    /** Counts the keys by streaming through all of them. */
    expected<std::size_t> size() const noexcept { return keys().size(); }

    /**
     *  @brief Reads the values of a batch of @p keys into @p arena, the verb that reuses one arena call after call.
     *  @param[in] keys Keys to read, in any batch the @c [] operator takes.
     *  @param[in] arena Memory the values land in, which the answer views until its next call.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     */
    template <batch_concept keys_argument_type_>
    expected<values_view_t> read(keys_argument_type_&& keys, arena_t& arena,
                                 ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return at(std::forward<keys_argument_type_>(keys)).value(arena, watch);
    }

    blobs_read_ref<place_batch_t> operator[](std::initializer_list<ustore_key_t> keys) noexcept { return at(keys); }
    blobs_read_ref<place_batch_t> at(std::initializer_list<ustore_key_t> keys) noexcept { return at(strided(keys)); }

    blobs_read_ref<place_batch_t> operator[](keys_view_t keys) noexcept { return at(keys); }
    blobs_read_ref<place_batch_t> at(keys_view_t keys) noexcept { return address_keys_<blobs_read_ref>(keys); }

    template <typename keys_argument_type_>
    auto operator[](keys_argument_type_&& keys) noexcept {
        return at(std::forward<keys_argument_type_>(keys));
    }

    /** Addresses one location or a batch of them inside this collection, whose own collection wins. */
    template <typename keys_argument_type_>
    auto at(keys_argument_type_&& keys) noexcept {
        return address_<blobs_read_ref>(std::forward<keys_argument_type_>(keys));
    }
};

/** A handle to one binary collection, cheap to construct, that reads and writes the latest state or the view of a
 *  transaction. */
class blobs_collection_t : public blobs_read_collection_t {

    status_t drop_with_(ustore_drop_mode_t mode) noexcept {
        ustore_outcome_t outcome {};
        ustore_collection_drop_t collection_drop {};
        collection_drop.database = receiver_.database;
        collection_drop.executor = receiver_.executor;
        collection_drop.outcome = &outcome;
        collection_drop.collection = collection_;
        collection_drop.mode = mode;
        ustore_collection_drop(&collection_drop);
        return status_t {outcome};
    }

  public:
    using blobs_read_collection_t::blobs_read_collection_t;

    /** The twin a deferred receiver hands out, whose verbs answer deferred calls. */
    using deferred_t = deferred_blobs_collection_t;

    status_t clear_values() noexcept { return drop_with_(ustore_drop_values_k); }
    status_t clear() noexcept { return drop_with_(ustore_drop_pairs_k); }
    status_t drop() noexcept { return drop_with_(ustore_drop_collection_k); }

    blobs_ref<place_batch_t> operator[](std::initializer_list<ustore_key_t> keys) noexcept { return at(keys); }
    blobs_ref<place_batch_t> at(std::initializer_list<ustore_key_t> keys) noexcept { return at(strided(keys)); }

    blobs_ref<place_batch_t> operator[](keys_view_t keys) noexcept { return at(keys); }
    blobs_ref<place_batch_t> at(keys_view_t keys) noexcept { return address_keys_<blobs_ref>(keys); }

    template <typename keys_argument_type_>
    auto operator[](keys_argument_type_&& keys) noexcept {
        return at(std::forward<keys_argument_type_>(keys));
    }

    /** Addresses one location or a batch of them inside this collection, whose own collection wins. */
    template <typename keys_argument_type_>
    auto at(keys_argument_type_&& keys) noexcept {
        return address_<blobs_ref>(std::forward<keys_argument_type_>(keys));
    }
};

#pragma endregion Collections

#pragma region Deferred Verbs

/** What a deferred @c ustore_blobs_read keeps beside its keys: the collection the arguments point to and the outputs
 *  the call writes into the arena it borrows. */
struct blobs_read_t {
    using arguments_t = ustore_blobs_read_t;
    using answer_t = expected<values_view_t>;

    /** Collection every key lives in. */
    ustore_collection_id_t collection {};

    /** Number of keys, the length of every output. */
    ustore_size_t count = 0;

    /** Outputs for the presences and the tape of values, inside the arena. */
    ustore_octet_t const* presences = nullptr;
    ustore_strings_view_t values {};

    static void submit(arguments_t& arguments) noexcept { ustore_blobs_read(&arguments); }
    answer_t finish() && noexcept { return values_view_t {count, presences, values}; }
};

/** What a deferred @c ustore_blobs_write keeps beside its keys and values: the collection. */
struct blobs_write_t {
    using arguments_t = ustore_blobs_write_t;
    using answer_t = status_t;

    /** Collection every key lives in. */
    ustore_collection_id_t collection {};

    static void submit(arguments_t& arguments) noexcept { ustore_blobs_write(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

/** What a deferred @c ustore_blobs_remove keeps beside its keys: the collection. */
struct blobs_remove_t {
    using arguments_t = ustore_blobs_remove_t;
    using answer_t = status_t;

    /** Collection every key lives in. */
    ustore_collection_id_t collection {};

    static void submit(arguments_t& arguments) noexcept { ustore_blobs_remove(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

#pragma endregion Deferred Verbs

#pragma region Deferred Collections

/** A binary collection that only reads, whose verbs submit to an executor and answer deferred calls, as a snapshot
 *  offers it. Every plain input a verb reads moves into the call, so nothing it borrowed can go before the call is
 *  final, while an arena is borrowed, since freeing it waits for the call. */
class deferred_blobs_read_collection_t {
  protected:
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};

  public:
    deferred_blobs_read_collection_t() noexcept = default;
    explicit deferred_blobs_read_collection_t(receiver_t receiver, ustore_collection_id_t collection) noexcept
        : receiver_(receiver), collection_(collection) {}

    operator ustore_collection_id_t() const noexcept { return collection_; }

    /**
     *  @brief Reads the values of @p keys into @p arena.
     *  @param[in] keys Keys to read, owned by the call until it is final.
     *  @param[in] arena Memory the values land in, borrowed until the call is final, which the answer views.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     */
    template <owned_array_concept<ustore_key_t> keys_type_>
    deferred<blobs_read_t> read(keys_type_ keys, arena_t& arena,
                                ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return deferred<blobs_read_t>::submit(
            [&](blobs_read_t& verb, ustore_blobs_read_t& arguments, keys_type_ const& owned) noexcept {
                verb.collection = collection_;
                verb.count = owned.size();
                arguments.database = receiver_.database;
                arguments.executor = receiver_.executor;
                arguments.view = receiver_.view(watch);
                arguments.count = owned.size();
                arguments.collections = {&verb.collection, 0};
                arguments.keys = {owned.data(), sizeof(ustore_key_t)};
                arguments.presences = &verb.presences;
                arguments.values = &verb.values;
                arguments.arena = arena;
                return status_t {};
            },
            std::move(keys));
    }
};

/** A binary collection whose verbs submit to an executor and answer deferred calls, reading and writing the head or
 *  a transaction. */
class deferred_blobs_collection_t : public deferred_blobs_read_collection_t {
  public:
    using deferred_blobs_read_collection_t::deferred_blobs_read_collection_t;

    /**
     *  @brief Writes the values of @p values to @p keys pairwise.
     *  @param[in] keys Keys to write, owned by the call until it is final.
     *  @param[in] values One value per key, owned by the call until it is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The @p keys and @p values differ in count.
     */
    template <owned_array_concept<ustore_key_t> keys_type_>
    deferred<blobs_write_t> write(keys_type_ keys, values_tape_t values,
                                  ustore_durability_t durability = ustore_durability_buffered_k) const noexcept {
        return deferred<blobs_write_t>::submit(
            [&](blobs_write_t& verb, ustore_blobs_write_t& arguments, keys_type_ const& owned_keys,
                values_tape_t const& owned_values) noexcept {
                if (owned_keys.size() != owned_values.size())
                    return status_t {status_code_t::invalid_argument_k, "Every key of a write needs its own value"};
                verb.collection = collection_;
                arguments.database = receiver_.database;
                arguments.executor = receiver_.executor;
                arguments.transaction = receiver_.transaction;
                arguments.count = owned_keys.size();
                arguments.collections = {&verb.collection, 0};
                arguments.keys = {owned_keys.data(), sizeof(ustore_key_t)};
                arguments.values = tape_of(owned_values);
                arguments.durability = durability;
                return status_t {};
            },
            std::move(keys), std::move(values));
    }

    /**
     *  @brief Removes @p keys together with their values.
     *  @param[in] keys Keys to remove, owned by the call until it is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    template <owned_array_concept<ustore_key_t> keys_type_>
    deferred<blobs_remove_t> remove(keys_type_ keys,
                                    ustore_durability_t durability = ustore_durability_buffered_k) const noexcept {
        return deferred<blobs_remove_t>::submit(
            [&](blobs_remove_t& verb, ustore_blobs_remove_t& arguments, keys_type_ const& owned) noexcept {
                verb.collection = collection_;
                arguments.database = receiver_.database;
                arguments.executor = receiver_.executor;
                arguments.transaction = receiver_.transaction;
                arguments.count = owned.size();
                arguments.collections = {&verb.collection, 0};
                arguments.keys = {owned.data(), sizeof(ustore_key_t)};
                arguments.durability = durability;
                return status_t {};
            },
            std::move(keys));
    }
};

#pragma endregion Deferred Collections

} // namespace unum::ustore
