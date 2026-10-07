/**
 *  @file include/ustore/blobs_ref.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief A proxy that reads and writes the binary values at a location or a batch of locations.
 */

#pragma once

#include <cstddef> // `std::byte`

#include <span>        // `std::span`
#include <type_traits> // `std::conditional_t`, `std::decay_t`
#include <utility>     // `std::forward`, `std::move`

#include "ustore/ranges.hpp"      // `embedded_blobs_t`, `bits_span_t`, `owning_view`, `read_spans_t`
#include "ustore/ranges_args.hpp" // `location_store`, `places_arg_extractor`
#include "ustore/status.hpp"      // `status_t`, `expected`
#include "ustore/types.hpp"       // `arena_t`
#include "ustore/ustore.h"

namespace unum::ustore {

/**
 *  A proxy that reads or writes the values at one location or at a batch of locations.
 *
 *  One value may go to one key or to many keys, and many values to as many keys. Every proxy made by
 *  one container shares that container's arena, so threads and interleaved requests whose results must
 *  survive the next call hand in their own arena through @c on. A batch result owns its presences and
 *  offsets, while its tape of values stays in that arena.
 *
 *  @tparam locations_ A single location, a batch of locations, or a reference to a batch the caller owns.
 */
template <typename locations_>
class blobs_ref {
  public:
    static_assert(!std::is_rvalue_reference_v<locations_>, "The locations can't be stored as an rvalue reference");

    using locations_store_t = location_store<locations_>;
    using locations_plain_t = typename locations_store_t::plain_t;
    using keys_extractor_t = places_arg_extractor<locations_plain_t>;
    static constexpr bool is_one_k = keys_extractor_t::is_one_k;

    using value_t = std::conditional_t<is_one_k, value_view_t, owning_view<embedded_blobs_t>>;
    using present_t = std::conditional_t<is_one_k, bool, owning_view<bits_span_t>>;
    using length_t = std::conditional_t<is_one_k, ustore_length_t, owning_view<std::span<ustore_length_t>>>;

  protected:
    ustore_database_t database_ {};
    ustore_transaction_t transaction_ {};
    ustore_snapshot_t snapshot_ {};
    ustore_arena_t* arena_ {};
    locations_store_t locations_store_ {};

    template <typename contents_arg_>
    status_t any_assign(contents_arg_&& values_ref, ustore_durability_t durability) noexcept;

    template <typename expected_>
    expected<expected_> any_get(ustore_watch_t watch) noexcept;

  public:
    blobs_ref(ustore_database_t database, ustore_transaction_t transaction, ustore_snapshot_t snapshot,
              locations_&& locations, ustore_arena_t* arena) noexcept
        : database_(database), transaction_(transaction), snapshot_(snapshot), arena_(arena),
          locations_store_(std::forward<locations_>(locations)) {}

    /** Redirects the results of every later call into @p arena. */
    blobs_ref& on(arena_t& arena) noexcept {
        arena_ = arena.member_ptr();
        return *this;
    }

    expected<value_t> value(ustore_watch_t watch = ustore_watch_keys_k) noexcept { return any_get<value_t>(watch); }

    operator expected<value_t>() noexcept { return value(); }

    expected<length_t> length(ustore_watch_t watch = ustore_watch_keys_k) noexcept { return any_get<length_t>(watch); }

    /** Checks which keys are present, where a present value may still be empty. */
    expected<present_t> present(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return any_get<present_t>(watch);
    }

    /**
     *  Assigns values to the keys pairwise, or one value to every key.
     *  @param[in] values Values to assign.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    template <typename contents_arg_>
    status_t assign(contents_arg_&& values, ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_assign(std::forward<contents_arg_>(values), durability);
    }

    /**
     *  Removes the keys together with their values.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    status_t erase(ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        decltype(auto) locations = locations_store_.ref();
        strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(locations);
        strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(locations);

        ustore_outcome_t outcome {};
        ustore_remove_t remove {};
        remove.database = database_;
        remove.outcome = &outcome;
        remove.transaction = transaction_;
        remove.count = keys_extractor_t {}.count(locations);
        remove.collections = collections.get();
        remove.collections_stride = collections.stride();
        remove.keys = keys.get();
        remove.keys_stride = keys.stride();
        remove.durability = durability;
        ustore_remove(&remove);
        return status_t {outcome};
    }

    /**
     *  Keeps the keys but empties their values.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    status_t clear(ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return assign(value_view_t::make_empty(), durability);
    }

    locations_plain_t& locations() noexcept { return locations_store_.ref(); }
    locations_plain_t const& locations() const noexcept { return locations_store_.ref(); }
};

static_assert(blobs_ref<ustore_key_t>::is_one_k);
static_assert(std::is_same_v<blobs_ref<ustore_key_t>::value_t, value_view_t>);
static_assert(!blobs_ref<places_arg_t>::is_one_k);

template <typename locations_>
template <typename expected_>
expected<expected_> blobs_ref<locations_>::any_get(ustore_watch_t watch) noexcept {

    decltype(auto) locations = locations_store_.ref();
    ustore_size_t const count = keys_extractor_t {}.count(locations);
    strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(locations);
    strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(locations);

    // A single key reads into the stack, and a batch into one buffer its result owns
    alignas(ustore_length_t) std::byte single[read_spans_t::bytes(1)] {};
    st::basic_vector<std::byte> batch;
    if constexpr (!is_one_k)
        if (st::failed(batch.resize(read_spans_t::bytes(count))))
            return status_t {status_code_t::out_of_memory_k, "Failed to allocate the spans of a read"};
    read_spans_t const spans = read_spans_t::carve(is_one_k ? single : batch.data(), count);

    ustore_byte_t* found_values = nullptr;
    ustore_outcome_t outcome {};
    ustore_read_t read {};
    read.database = database_;
    read.outcome = &outcome;
    read.view = view_through(transaction_, snapshot_, watch);
    read.count = count;
    read.collections = collections.get();
    read.collections_stride = collections.stride();
    read.keys = keys.get();
    read.keys_stride = keys.stride();
    read.presences = spans.presences;
    read.offsets = spans.offsets;
    read.values = std::is_same_v<expected_, value_t> ? &found_values : nullptr;
    read.arena = arena_;

    ustore_read(&read);
    if (outcome.status != ustore_status_success_k) return status_t {outcome};
    spans.derive_lengths(count);

    if constexpr (std::is_same_v<expected_, length_t>) {
        if constexpr (is_one_k) return ustore_length_t {spans.lengths[0]};
        else return length_t {std::span<ustore_length_t> {spans.lengths, count}, std::move(batch)};
    }
    else if constexpr (std::is_same_v<expected_, present_t>) {
        if constexpr (is_one_k) return bool(bits_span_t {spans.presences}[0]);
        else return present_t {bits_span_t {spans.presences}, std::move(batch)};
    }
    else {
        embedded_blobs_t const many {count, spans.offsets, spans.lengths, found_values};
        if constexpr (is_one_k) return value_view_t {many[0]};
        else return value_t {many, std::move(batch)};
    }
}

template <typename locations_>
template <typename contents_arg_>
status_t blobs_ref<locations_>::any_assign(contents_arg_&& values_ref, ustore_durability_t durability) noexcept {

    // Arrays decay into pointers, so the extractor sees one pointer object whose address the C ABI can stride over
    using values_t = std::decay_t<contents_arg_>;
    using value_extractor_t = contents_arg_extractor<values_t>;
    values_t const& values = values_ref;

    decltype(auto) locations = locations_store_.ref();
    strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(locations);
    strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(locations);
    strided_iterator<ustore_bytes_cptr_t const> const contents = value_extractor_t {}.contents(values);
    strided_iterator<ustore_length_t const> const starts = value_extractor_t {}.starts(values);
    strided_iterator<ustore_length_t const> const lengths = value_extractor_t {}.lengths(values);

    ustore_outcome_t outcome {};
    ustore_write_t write {};
    write.database = database_;
    write.outcome = &outcome;
    write.transaction = transaction_;
    write.count = keys_extractor_t {}.count(locations);
    write.collections = collections.get();
    write.collections_stride = collections.stride();
    write.keys = keys.get();
    write.keys_stride = keys.stride();
    write.starts = starts.get();
    write.starts_stride = starts.stride();
    write.lengths = lengths.get();
    write.lengths_stride = lengths.stride();
    write.values = contents.get();
    write.values_stride = contents.stride();
    write.durability = durability;

    ustore_write(&write);
    return status_t {outcome};
}

} // namespace unum::ustore
