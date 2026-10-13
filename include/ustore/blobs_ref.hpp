/**
 *  @file include/ustore/blobs_ref.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief Proxies that read, and write, the binary values at a location or a batch of locations.
 */

#pragma once

#include <type_traits> // `std::conditional_t`, `std::decay_t`, `std::is_same_v`
#include <utility>     // `std::forward`, `std::move`

#include "ustore/ranges.hpp" // `values_view_t`, `lengths_view_t`, `bits_view_t`, `location_store`
#include "ustore/status.hpp" // `status_t`, `expected`
#include "ustore/types.hpp"  // `arena_t`, `one_off_t`, `in_one_off_arena`, `receiver_t`
#include "ustore/ustore.h"

namespace unum::ustore {

/**
 *  A proxy that reads the values at one location or at a batch of locations, through a snapshot, a transaction or
 *  the head, and never writes them.
 *
 *  A read given an arena answers views into it, valid until the next call given that arena. The same read without one
 *  is shorthand: it makes a one-off arena and answers a result owning it, or a scalar copied out of it, which is
 *  slower than reusing an arena and keeps no state anywhere.
 *
 *  @tparam locations_type_ A single location, a batch of locations, or a reference to a batch the caller owns.
 */
template <typename locations_type_>
class blobs_read_ref {
  public:
    static_assert(!std::is_rvalue_reference_v<locations_type_>, "The locations can't be stored as an rvalue reference");

    using locations_store_t = location_store<locations_type_>;
    using locations_plain_t = typename locations_store_t::plain_t;
    using keys_extractor_t = places_arg_extractor<locations_plain_t>;
    static constexpr bool is_one_k = keys_extractor_t::is_one_k;

    using value_t = std::conditional_t<is_one_k, value_view_t, values_view_t>;
    using present_t = std::conditional_t<is_one_k, bool, bits_view_t>;
    using length_t = std::conditional_t<is_one_k, ustore_length_t, lengths_view_t>;

  protected:
    receiver_t receiver_ {};
    locations_store_t locations_store_ {};

    /** Reads the locations into @p arena, exporting the tape of values only when @p answer_type_ is @c value_t. */
    template <typename answer_type_>
    expected<answer_type_> read_(arena_t& arena, ustore_watch_t watch) noexcept;

  public:
    blobs_read_ref(receiver_t receiver, locations_type_&& locations) noexcept
        : receiver_(receiver), locations_store_(std::forward<locations_type_>(locations)) {}

    /** Reads the values into @p arena. */
    expected<value_t> value(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return read_<value_t>(arena, watch);
    }

    /** Reads the values into a one-off arena, which the answer owns. */
    expected<one_off_t<value_t>> value(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept { return value(arena, watch); });
    }

    /** Reads the lengths of the values into @p arena. */
    expected<length_t> length(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return read_<length_t>(arena, watch);
    }

    /** Reads the lengths of the values into a one-off arena. */
    expected<one_off_t<length_t>> length(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept { return length(arena, watch); });
    }

    /** Checks which keys are present into @p arena, where a present value may still be empty. */
    expected<present_t> present(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return read_<present_t>(arena, watch);
    }

    /** Checks which keys are present into a one-off arena. */
    expected<one_off_t<present_t>> present(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept { return present(arena, watch); });
    }

    locations_plain_t& locations() noexcept { return locations_store_.ref(); }
    locations_plain_t const& locations() const noexcept { return locations_store_.ref(); }
};

/**
 *  A proxy that reads or writes the values at one location or at a batch of locations, at the head or through a
 *  transaction. One value may go to one key or to many keys, and many values to as many keys.
 *
 *  @tparam locations_type_ A single location, a batch of locations, or a reference to a batch the caller owns.
 */
template <typename locations_type_>
class blobs_ref : public blobs_read_ref<locations_type_> {
    using base_t = blobs_read_ref<locations_type_>;

  public:
    using base_t::base_t;

    /**
     *  Assigns values to the keys pairwise, or one value to every key.
     *  @param[in] values Values to assign.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    template <typename values_argument_type_>
    status_t assign(values_argument_type_&& values,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept;

    /**
     *  Removes the keys together with their values.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    status_t remove(ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        decltype(auto) locations = this->locations_store_.ref();
        using keys_extractor_t = typename base_t::keys_extractor_t;
        strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(locations);
        strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(locations);

        ustore_outcome_t outcome {};
        ustore_blobs_remove_t remove {};
        remove.database = this->receiver_.database;
        remove.executor = this->receiver_.executor;
        remove.outcome = &outcome;
        remove.transaction = this->receiver_.transaction;
        remove.count = keys_extractor_t {}.count(locations);
        remove.collections = {collections.get(), collections.stride()};
        remove.keys = {keys.get(), keys.stride()};
        remove.durability = durability;
        ustore_blobs_remove(&remove);
        return status_t {outcome};
    }

    /**
     *  Keeps the keys but empties their values.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    status_t clear_values(ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return assign(value_view_t::make_empty(), durability);
    }
};

static_assert(blobs_read_ref<ustore_key_t>::is_one_k);
static_assert(std::is_same_v<blobs_read_ref<ustore_key_t>::value_t, value_view_t>);
static_assert(!blobs_read_ref<place_batch_t>::is_one_k);

template <typename locations_type_>
template <typename answer_type_>
expected<answer_type_> blobs_read_ref<locations_type_>::read_(arena_t& arena, ustore_watch_t watch) noexcept {

    decltype(auto) locations = locations_store_.ref();
    ustore_size_t const count = keys_extractor_t {}.count(locations);
    strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(locations);
    strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(locations);

    ustore_octet_t const* presences = nullptr;
    ustore_length_t const* lengths = nullptr;
    ustore_strings_view_t values {};
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read {};
    read.database = receiver_.database;
    read.executor = receiver_.executor;
    read.outcome = &outcome;
    read.view = receiver_.view(watch);
    read.count = count;
    read.collections = {collections.get(), collections.stride()};
    read.keys = {keys.get(), keys.stride()};
    read.presences = &presences;
    read.lengths = std::is_same_v<answer_type_, length_t> ? &lengths : nullptr;
    read.values = std::is_same_v<answer_type_, value_t> ? &values : nullptr;
    read.arena = arena;

    ustore_blobs_read(&read);
    if (status_t const status {outcome}; status.failed()) return status;

    if constexpr (std::is_same_v<answer_type_, length_t>) {
        lengths_view_t const found {count, presences, lengths};
        if constexpr (is_one_k) return found[0];
        else return found;
    }
    else if constexpr (std::is_same_v<answer_type_, present_t>) {
        bits_view_t const present {presences};
        if constexpr (is_one_k) return present[0];
        else return present;
    }
    else {
        values_view_t const found {count, presences, values};
        if constexpr (is_one_k) return found[0];
        else return found;
    }
}

template <typename locations_type_>
template <typename values_argument_type_>
status_t blobs_ref<locations_type_>::assign(values_argument_type_&& values_ref,
                                            ustore_durability_t durability) noexcept {

    // Arrays decay into pointers, so the extractor sees one pointer object whose address an accessor can stride over
    using values_t = std::decay_t<values_argument_type_>;
    using keys_extractor_t = typename base_t::keys_extractor_t;
    values_t const& values = values_ref;
    values_arg_extractor<values_t> values_extractor;

    decltype(auto) locations = this->locations_store_.ref();
    strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(locations);
    strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(locations);

    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = this->receiver_.database;
    write.executor = this->receiver_.executor;
    write.outcome = &outcome;
    write.transaction = this->receiver_.transaction;
    write.count = keys_extractor_t {}.count(locations);
    write.collections = {collections.get(), collections.stride()};
    write.keys = {keys.get(), keys.stride()};
    write.values = values_extractor.view(values);
    write.durability = durability;

    ustore_blobs_write(&write);
    return status_t {outcome};
}

} // namespace unum::ustore
