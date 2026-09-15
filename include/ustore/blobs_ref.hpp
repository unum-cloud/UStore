/**
 *  @file include/ustore/blobs_ref.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief A proxy that reads and writes the binary values at a location or a batch of locations.
 */

#pragma once

#include <cstddef> // `std::nullptr_t`

#include <type_traits> // `std::conditional_t`, `std::decay_t`
#include <utility>     // `std::forward`, `std::move`

#include <ustore/ranges.hpp>      // `embedded_blobs_t`, `bits_span_t`
#include <ustore/ranges_args.hpp> // `location_store`, `places_arg_extractor`
#include <ustore/status.hpp>      // `status_t`, `expected`
#include <ustore/types.hpp>       // `arena_t`
#include <ustore/ustore.h>

namespace unum::ustore {

/**
 *  A proxy that reads or writes the values at one location or at a batch of locations.
 *
 *  One value may go to one key or to many keys, and many values to as many keys. Every proxy made by
 *  one container shares that container's arena, so threads and interleaved requests whose results must
 *  survive the next call hand in their own arena through @c on.
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

    using value_t = std::conditional_t<is_one_k, value_view_t, embedded_blobs_t>;
    using present_t = std::conditional_t<is_one_k, bool, bits_span_t>;
    using length_t = std::conditional_t<is_one_k, ustore_length_t, ptr_range<ustore_length_t>>;

  protected:
    ustore_database_t database_ {};
    ustore_transaction_t transaction_ {};
    ustore_snapshot_t snapshot_ {};
    ustore_arena_t* arena_ {};
    locations_store_t locations_store_ {};

    template <typename contents_arg_>
    status_t any_assign(contents_arg_&& values_ref, ustore_options_t options) noexcept;

    template <typename expected_>
    expected<expected_> any_get(ustore_options_t options) noexcept;

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

    expected<value_t> value(watch_t watch = watch_t::watch_k) noexcept { return any_get<value_t>(to_options(watch)); }

    operator expected<value_t>() noexcept { return value(); }

    expected<length_t> length(watch_t watch = watch_t::watch_k) noexcept {
        return any_get<length_t>(to_options(watch));
    }

    /** Checks which keys are present, where a present value may still be empty. */
    expected<present_t> present(watch_t watch = watch_t::watch_k) noexcept {
        return any_get<present_t>(to_options(watch));
    }

    /**
     *  Assigns values to the keys pairwise, or one value to every key.
     *  @param[in] values Values to assign.
     *  @param[in] flush Whether the call returns only after the data reaches the disk.
     */
    template <typename contents_arg_>
    status_t assign(contents_arg_&& values, flush_t flush = flush_t::dont_flush_k) noexcept {
        return any_assign(std::forward<contents_arg_>(values), to_options(flush));
    }

    /**
     *  Removes the keys together with their values.
     *  @param[in] flush Whether the call returns only after the data reaches the disk.
     */
    status_t erase(flush_t flush = flush_t::dont_flush_k) noexcept { return assign(nullptr, flush); }

    /**
     *  Keeps the keys but empties their values.
     *  @param[in] flush Whether the call returns only after the data reaches the disk.
     */
    status_t clear(flush_t flush = flush_t::dont_flush_k) noexcept {
        auto const any = reinterpret_cast<ustore_bytes_ptr_t>(this);
        ustore_length_t const length = 0;
        contents_arg_t arg {};
        arg.lengths_begin = {&length};
        arg.contents_begin = {&any};
        arg.count = 1;
        return assign(arg, flush);
    }

    locations_plain_t& locations() noexcept { return locations_store_.ref(); }
    locations_plain_t const& locations() const noexcept { return locations_store_.ref(); }
};

static_assert(blobs_ref<ustore_key_t>::is_one_k);
static_assert(std::is_same_v<blobs_ref<ustore_key_t>::value_t, value_view_t>);
static_assert(!blobs_ref<places_arg_t>::is_one_k);

template <typename locations_>
template <typename expected_>
expected<expected_> blobs_ref<locations_>::any_get(ustore_options_t options) noexcept {

    status_t status;
    ustore_length_t* found_offsets = nullptr;
    ustore_length_t* found_lengths = nullptr;
    ustore_bytes_ptr_t found_values = nullptr;
    ustore_octet_t* found_presences = nullptr;

    decltype(auto) locations = locations_store_.ref();
    auto const count = keys_extractor_t {}.count(locations);
    auto const keys = keys_extractor_t {}.keys(locations);
    auto const collections = keys_extractor_t {}.collections(locations);

    ustore_read_t read {};
    read.db = database_;
    read.error = status.member_ptr();
    read.transaction = transaction_;
    read.snapshot = snapshot_;
    read.arena = arena_;
    read.options = options;
    read.tasks_count = count;
    read.collections = collections.get();
    read.collections_stride = collections.stride();
    read.keys = keys.get();
    read.keys_stride = keys.stride();
    read.presences = std::is_same_v<expected_, present_t> ? &found_presences : nullptr;
    read.offsets = std::is_same_v<expected_, value_t> ? &found_offsets : nullptr;
    read.lengths = std::is_same_v<expected_, value_t> || std::is_same_v<expected_, length_t> ? &found_lengths : nullptr;
    read.values = std::is_same_v<expected_, value_t> ? &found_values : nullptr;

    ustore_read(&read);
    if (!status) return std::move(status);

    if constexpr (std::is_same_v<expected_, length_t>) {
        ptr_range<ustore_length_t> const many {found_lengths, found_lengths + count};
        if constexpr (is_one_k) return ustore_length_t {many[0]};
        else return ptr_range<ustore_length_t> {many};
    }
    else if constexpr (std::is_same_v<expected_, present_t>) {
        bits_span_t const many {found_presences};
        if constexpr (is_one_k) return bool(many[0]);
        else return bits_span_t {many};
    }
    else {
        embedded_blobs_t const many {count, found_offsets, found_lengths, found_values};
        if constexpr (is_one_k) return value_view_t {many[0]};
        else return embedded_blobs_t {many};
    }
}

template <typename locations_>
template <typename contents_arg_>
status_t blobs_ref<locations_>::any_assign(contents_arg_&& values_ref, ustore_options_t options) noexcept {

    // Arrays decay into pointers, so the extractor sees one pointer object whose address the C ABI can stride over
    using values_t = std::decay_t<contents_arg_>;
    using value_extractor_t = contents_arg_extractor<values_t>;
    values_t const& values = values_ref;

    decltype(auto) locations = locations_store_.ref();
    auto const count = keys_extractor_t {}.count(locations);
    auto const keys = keys_extractor_t {}.keys(locations);
    auto const collections = keys_extractor_t {}.collections(locations);

    auto const contents = value_extractor_t {}.contents(values);
    auto const offsets = value_extractor_t {}.offsets(values);
    auto const lengths = value_extractor_t {}.lengths(values);

    status_t status;
    ustore_write_t write {};
    write.db = database_;
    write.error = status.member_ptr();
    write.transaction = transaction_;
    write.arena = arena_;
    write.options = options;
    write.tasks_count = count;
    write.collections = collections.get();
    write.collections_stride = collections.stride();
    write.keys = keys.get();
    write.keys_stride = keys.stride();
    write.offsets = offsets.get();
    write.offsets_stride = offsets.stride();
    write.lengths = lengths.get();
    write.lengths_stride = lengths.stride();
    write.values = contents.get();
    write.values_stride = contents.stride();

    ustore_write(&write);
    return status;
}

} // namespace unum::ustore
