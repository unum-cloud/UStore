/**
 *  @file cpp/ustore/blobs_ref.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief The one body of every binary verb, and the proxies that read, and write, the values at a
 *      location or a batch of locations through them.
 */

#pragma once

#include <type_traits> // `std::conditional_t`, `std::decay_t`, `std::is_rvalue_reference_v`
#include <utility>     // `std::forward`

#include "ustore/ranges.hpp" // `values_view_t`, `lengths_view_t`, `bits_view_t`, `locations_places_`, `first_or_all_`
#include "ustore/status.hpp" // `status_t`, `expected`, `answer`, `called_`
#include "ustore/types.hpp"  // `arena_t`, `one_off_answer_of`, `in_one_off_arena`, `receiver_t`, `collection_access_t`
#include "ustore/ustore.h"

namespace unum::ustore {

#pragma region Verbs

/**
 *  @brief Reads what @p answer_type_ asks of the values at @p places into @p arena: the values,
 *      their lengths, or only whether each is present.
 *  @param[in] receiver The database, executor, and transaction or snapshot the read sees.
 *  @param[in] places Collections and keys to read, borrowed until the call is final.
 *  @param[in] arena Memory the answer lands in, borrowed until the call is final, which
 *      the answer views.
 *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
 */
template <ustore_completion_t completion_, typename answer_type_>
answer<completion_, answer_type_> blobs_read_(receiver_t const& receiver, place_batch_t const& places, arena_t& arena,
                                              ustore_watch_t watch) noexcept {
    return called_<completion_, answer_type_>([&](ustore_outcome_t* outcome, outputs<answer_type_>* written) noexcept {
        written->count = places.count;
        ustore_blobs_read_t read {};
        read.database = receiver.database;
        read.executor = receiver.executor;
        read.outcome = outcome;
        read.completion = completion_;
        read.view = receiver.view(watch);
        read.count = places.count;
        read.collections = places.collections();
        read.keys = places.keys();
        read.presences = &written->presences;
        read.lengths = lengths_output_(*written);
        read.values = values_output_(*written);
        read.arena = arena;
        ustore_blobs_read(&read);
    });
}

/**
 *  @brief Writes @p values to @p places pairwise, or one value to every place.
 *  @param[in] values Values to write, borrowed until the call is final.
 *  @param[in] durability Whether the call finishes only after the data reaches the disk.
 */
template <ustore_completion_t completion_>
answer<completion_, void> blobs_write_(receiver_t const& receiver, place_batch_t const& places,
                                       ustore_strings_view_t values, ustore_durability_t durability) noexcept {
    return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_blobs_write_t write {};
        write.database = receiver.database;
        write.executor = receiver.executor;
        write.outcome = outcome;
        write.completion = completion_;
        write.transaction = receiver.transaction;
        write.count = places.count;
        write.collections = places.collections();
        write.keys = places.keys();
        write.values = values;
        write.durability = durability;
        ustore_blobs_write(&write);
    });
}

/** Removes the keys at @p places together with their values. */
template <ustore_completion_t completion_>
answer<completion_, void> blobs_remove_(receiver_t const& receiver, place_batch_t const& places,
                                        ustore_durability_t durability) noexcept {
    return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_blobs_remove_t remove {};
        remove.database = receiver.database;
        remove.executor = receiver.executor;
        remove.outcome = outcome;
        remove.completion = completion_;
        remove.transaction = receiver.transaction;
        remove.count = places.count;
        remove.collections = places.collections();
        remove.keys = places.keys();
        remove.durability = durability;
        ustore_blobs_remove(&remove);
    });
}

#pragma endregion Verbs

#pragma region Proxies

/**
 *  @brief A proxy that reads the values at one location or at a batch of locations, through a
 *      snapshot, a transaction or the head, and writes them too when @p access_ allows. One value
 *      may go to one key or to many keys, and many values to as many keys.
 *
 *  A read given an arena answers views into it, valid until the next call given that arena. The
 *  same read without one is shorthand: it makes a one-off arena and answers a result owning it, or
 *  a scalar copied out of it, which is slower than reusing an arena and keeps no state anywhere.
 *  Every call blocks.
 *
 *  @tparam locations_type_ A single location, a batch of locations, or a reference to a batch
 *      the caller owns.
 */
template <typename locations_type_, collection_access_t access_ = collection_access_t::read_write_k>
class blobs_ref {
  public:
    static_assert(!std::is_rvalue_reference_v<locations_type_>, "The locations can't be stored as an rvalue reference");

    using locations_store_t = location_store<locations_type_>;
    using locations_plain_t = typename locations_store_t::plain_t;
    static constexpr bool is_one_k = places_arg_extractor<locations_plain_t>::is_one_k;
    static constexpr bool writes_k = access_ == collection_access_t::read_write_k;

    using value_t = std::conditional_t<is_one_k, value_view_t, values_view_t>;
    using present_t = std::conditional_t<is_one_k, bool, bits_view_t>;
    using length_t = std::conditional_t<is_one_k, ustore_length_t, lengths_view_t>;

  private:
    receiver_t receiver_ {};
    locations_store_t locations_store_ {};

    place_batch_t places_() const noexcept { return locations_places_(locations_store_.ref()); }

  public:
    blobs_ref(receiver_t receiver, locations_type_&& locations) noexcept
        : receiver_(receiver), locations_store_(std::forward<locations_type_>(locations)) {}

    /** Reads the values into @p arena. */
    expected<value_t> value(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return first_or_all_<value_t>(
            blobs_read_<ustore_completion_blocking_k, values_view_t>(receiver_, places_(), arena, watch));
    }

    /** Reads the values into a one-off arena, which the answer owns. */
    expected<one_off_answer_of<value_t>> value(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept { return value(arena, watch); });
    }

    /** Reads the lengths of the values into @p arena. */
    expected<length_t> length(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return first_or_all_<length_t>(
            blobs_read_<ustore_completion_blocking_k, lengths_view_t>(receiver_, places_(), arena, watch));
    }

    /** Reads the lengths of the values into a one-off arena. */
    expected<one_off_answer_of<length_t>> length(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept { return length(arena, watch); });
    }

    /** Checks which keys are present into @p arena, where a present value may still be empty. */
    expected<present_t> present(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return first_or_all_<present_t>(
            blobs_read_<ustore_completion_blocking_k, bits_view_t>(receiver_, places_(), arena, watch));
    }

    /** Checks which keys are present into a one-off arena. */
    expected<one_off_answer_of<present_t>> present(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept { return present(arena, watch); });
    }

    /**
     *  Assigns values to the keys pairwise, or one value to every key.
     *  @param[in] values Values to assign.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    template <typename values_argument_type_>
    status_t assign(values_argument_type_&& values,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        values_arg_extractor<std::decay_t<values_argument_type_>> extractor;
        return blobs_write_<ustore_completion_blocking_k>(receiver_, places_(), extractor.view(values), durability);
    }

    /**
     *  Removes the keys together with their values.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    status_t remove(ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        return blobs_remove_<ustore_completion_blocking_k>(receiver_, places_(), durability);
    }

    /**
     *  Keeps the keys but empties their values.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    status_t clear_values(ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        return assign(value_view_t::make_empty(), durability);
    }

    locations_plain_t& locations() noexcept { return locations_store_.ref(); }
    locations_plain_t const& locations() const noexcept { return locations_store_.ref(); }
};

/** A proxy that only reads, as snapshots hand out. */
template <typename locations_type_>
using blobs_read_ref = blobs_ref<locations_type_, collection_access_t::read_only_k>;

static_assert(blobs_read_ref<ustore_key_t>::is_one_k);
static_assert(std::is_same_v<blobs_read_ref<ustore_key_t>::value_t, value_view_t>);
static_assert(!blobs_read_ref<place_batch_t>::is_one_k);

#pragma endregion Proxies

} // namespace unum::ustore
