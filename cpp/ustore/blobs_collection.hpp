/**
 *  @file cpp/ustore/blobs_collection.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief Handles to one binary collection, whose verbs block or answer deferred calls as their
 *      receiver chose, and from which value proxies and key ranges are made.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <initializer_list> // `std::initializer_list`
#include <iterator>         // `std::size`
#include <utility>          // `std::forward`

#include "ustore/blobs_range.hpp" // `blobs_range_t`, `keys_range_t`, `pairs_range_t`, `size_range_t`
#include "ustore/blobs_ref.hpp"   // `blobs_ref`, `blobs_read_`, `blobs_write_`, `blobs_remove_`
#include "ustore/db.h"
#include "ustore/ranges.hpp" // `batch_concept`, `strided`, `places_in`, `keys_view_t`, `strings_batch_concept`
#include "ustore/status.hpp" // `expected`, `answer`, `refused_`, `defers`
#include "ustore/types.hpp"  // `arena_t`, `receiver_t`, `collection_access_t`, `location_in`, `collection_drop_`

namespace unum::ustore {

/**
 *  @brief A handle to one binary collection, cheap to construct and to copy, which addresses
 *      the head or the view of a transaction or snapshot, and whose verbs answer as
 *      @p completion_ says.
 *
 *  A blocking handle answers an @c outcome, while a deferred one answers a @c deferred call,
 *  borrowing the handle, its inputs and its arena until the call is final. A handle with
 *  @c collection_access_t::read_only_k only reads, as a snapshot hands out. It holds no state of
 *  its own, so any number of threads share it, and it never outlives its database.
 */
template <ustore_completion_t completion_, collection_access_t access_>
class blobs_collection_on {
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};

    static constexpr bool blocks_k = !defers<completion_>;
    static constexpr bool writes_k = access_ == collection_access_t::read_write_k;

    /** A proxy over one location inside this collection, whose own collection wins over the one it
     *  names. */
    template <typename key_argument_type_>
        requires(!batch_concept<key_argument_type_>)
    blobs_ref<collection_key_field_t, access_> address_(key_argument_type_ const& key) const noexcept {
        return {receiver_, location_in(collection_, key)};
    }

    /** A proxy over a batch of locations inside this collection. */
    template <batch_concept keys_argument_type_>
    auto address_(keys_argument_type_&& keys) const noexcept {
        using locations_t = locations_in_collection<keys_argument_type_>;
        return blobs_ref<locations_t, access_> {receiver_,
                                                locations_t {std::forward<keys_argument_type_>(keys), collection_}};
    }

  public:
    /** The handle a snapshot hands out for this kind of collection, which only reads. */
    using read_only_t = blobs_collection_on<completion_, collection_access_t::read_only_k>;

    /** The same kind of handle, completing its verbs as @p other_completion_ says. */
    template <ustore_completion_t other_completion_>
    using completed_as = blobs_collection_on<other_completion_, access_>;

    /** What the collections this kind of handle addresses hold, which a creation records and a
     *  lookup checks. */
    static constexpr ustore_modality_t modality_k = ustore_modality_blobs_k;

    blobs_collection_on() noexcept = default;
    blobs_collection_on(receiver_t receiver, ustore_collection_id_t collection) noexcept
        : receiver_(receiver), collection_(collection) {}

    operator ustore_collection_id_t() const noexcept { return collection_; }
    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }
    receiver_t receiver() const noexcept { return receiver_; }

    /** The same collection, whose verbs block on @p executor, or on the calling
     *  thread without one. */
    completed_as<ustore_completion_blocking_k> on(ustore_executor_t executor) const noexcept {
        return {receiver_.on(executor), collection_};
    }

    /** The same collection, whose verbs submit to @p executor and answer deferred calls. */
    completed_as<ustore_completion_deferred_k> deferred(ustore_executor_t executor) const noexcept {
        return {receiver_.on(executor), collection_};
    }

#pragma region Verbs

    /**
     *  @brief Reads the values of @p keys into @p arena, the verb that reuses one arena
     *      call after call.
     *  @param[in] keys Keys to read, borrowed until the call is final.
     *  @param[in] arena Memory the values land in, borrowed until the call is final, which
     *      the answer views.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     */
    answer<completion_, values_view_t> read(keys_view_t keys, arena_t& arena,
                                            ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return blobs_read_<completion_, values_view_t>(receiver_, places_in(collection_, keys), arena, watch);
    }

    /**
     *  @brief Writes @p values to @p keys pairwise.
     *  @param[in] keys Keys to write, borrowed until the call is final.
     *  @param[in] values One value per key, borrowed until the call is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The @p keys and @p values differ in count.
     */
    template <strings_batch_concept values_type_>
    answer<completion_, void> write(keys_view_t keys, values_type_ const& values,
                                    ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        // The call has one count, so only this side sees the keys and the values disagree
        if (keys.size() != std::size(values))
            return refused_<completion_, void>({status_code_t::invalid_argument_k, "Every key needs its own value"});
        return blobs_write_<completion_>(receiver_, places_in(collection_, keys), strings_view_of(values), durability);
    }

    /**
     *  @brief Removes @p keys together with their values.
     *  @param[in] keys Keys to remove, borrowed until the call is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    answer<completion_, void> remove(keys_view_t keys,
                                     ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        return blobs_remove_<completion_>(receiver_, places_in(collection_, keys), durability);
    }

    /** Removes every value and keeps the keys. */
    answer<completion_, void> clear_values() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_values_k);
    }

    /** Removes every key and value and keeps the collection. */
    answer<completion_, void> clear() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_pairs_k);
    }

    /** Removes the collection with its name and every pair it holds. */
    answer<completion_, void> drop() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_collection_k);
    }

#pragma endregion Verbs

#pragma region Shorthand

    blobs_range_t members(ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) const noexcept
        requires blocks_k
    {
        return {receiver_, collection_, start_key, end_key};
    }

    keys_range_t keys(ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) const noexcept
        requires blocks_k
    {
        return {members(start_key, end_key)};
    }

    pairs_range_t pairs(ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) const noexcept
        requires blocks_k
    {
        return {members(start_key, end_key)};
    }

    expected<size_range_t> size_range() const noexcept
        requires blocks_k
    {
        auto [estimates, size_estimates_status] = members().size_estimates();
        if (size_estimates_status.failed()) return size_estimates_status;
        return estimates.cardinality;
    }

    /** Counts the keys by streaming through all of them. */
    expected<std::size_t> size() const noexcept
        requires blocks_k
    {
        return keys().size();
    }

    blobs_ref<place_batch_t, access_> operator[](std::initializer_list<ustore_key_t> keys) const noexcept
        requires blocks_k
    {
        return at(keys);
    }

    blobs_ref<place_batch_t, access_> at(std::initializer_list<ustore_key_t> keys) const noexcept
        requires blocks_k
    {
        return at(keys_view_t {strided(keys)});
    }

    blobs_ref<place_batch_t, access_> operator[](keys_view_t keys) const noexcept
        requires blocks_k
    {
        return at(keys);
    }

    blobs_ref<place_batch_t, access_> at(keys_view_t keys) const noexcept
        requires blocks_k
    {
        return {receiver_, places_in(collection_, keys)};
    }

    template <typename keys_argument_type_>
    auto operator[](keys_argument_type_&& keys) const noexcept
        requires blocks_k
    {
        return at(std::forward<keys_argument_type_>(keys));
    }

    /** Addresses one location or a batch of them inside this collection, whose
     *  own collection wins. */
    template <typename keys_argument_type_>
    auto at(keys_argument_type_&& keys) const noexcept
        requires blocks_k
    {
        return address_(std::forward<keys_argument_type_>(keys));
    }

#pragma endregion Shorthand
};

/** A binary collection whose verbs block, reading and writing the head or a transaction. */
using blobs_collection_t = blobs_collection_on<ustore_completion_blocking_k, collection_access_t::read_write_k>;

/** A binary collection whose verbs block and only read, as a snapshot hands out. */
using blobs_read_collection_t = blobs_collection_on<ustore_completion_blocking_k, collection_access_t::read_only_k>;

} // namespace unum::ustore
