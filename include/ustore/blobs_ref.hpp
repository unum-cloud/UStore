/**
 * @file blobs_ref.hpp
 * @author Ashot Vardanian
 * @date 26 Jun 2022
 * @addtogroup Cpp
 *
 * @brief C++ bindings for "ustore/db.h".
 */

#pragma once
#include "ustore/ustore.h"
#include "ustore/cpp/types.hpp"      // `arena_t`
#include "ustore/cpp/status.hpp"     // `status_t`
#include "ustore/cpp/sfinae.hpp"     // `location_store_gt`
#include "ustore/cpp/docs_table.hpp" // `docs_table_t`

namespace unum::ustore {

template <typename locations_store_t>
class blobs_ref_gt;

/**
 * @brief A proxy object, that allows both lookups and writes
 * with `[]` and assignment operators for a batch of keys
 * simultaneously.
 *
 * Following assignment combinations are possible:
 * - one value to many keys
 * - many values to many keys
 * - one value to one key
 * The only impossible combination is assigning many values to one key.
 *
 * @tparam locations_at Type describing the address of a value in DBMS.
 * - (ustore_collection_t?, ustore_key_t, ustore_field_t?): Single KV-pair location.
 * - (ustore_collection_t*, ustore_key_t*, ustore_field_t*): Externally owned range of keys.
 * - (ustore_collection_t[x], ustore_key_t[x], ustore_field_t[x]): On-stack array of addresses.
 *
 * ## Memory Management
 *
 * Every "container" that overloads the @b [] operator has an internal "arena",
 * that is shared between all the @c blobs_ref_gt's produced from it. That will
 * work great, unless:
 * - multiple threads are working with same collection handle or transaction.
 * - reading responses interleaves with new requests, which gobbles temporary memory.
 * For those cases, you can create a separate @c arena_t and pass it to `.on(...)`
 * member function. In such HPC environments we would recommend to @b reuse one such
 * are on every thread.
 *
 * ## Class Specs
 * - Copyable: Yes.
 * - Exceptions: Never.
 */
template <typename locations_at>
class blobs_ref_gt {
  public:
    static_assert(!std::is_rvalue_reference_v<locations_at>, //
                  "The internal object can't be an R-value Reference");

    using locations_store_t = location_store_gt<locations_at>;
    using locations_plain_t = typename locations_store_t::plain_t;
    using keys_extractor_t = places_arg_extractor_gt<locations_plain_t>;
    static constexpr bool is_one_k = keys_extractor_t::is_one_k;

    using value_t = std::conditional_t<is_one_k, value_view_t, embedded_blobs_t>;
    using present_t = std::conditional_t<is_one_k, bool, bits_span_t>;
    using length_t = std::conditional_t<is_one_k, ustore_length_t, ptr_range_gt<ustore_length_t>>;

  protected:
    ustore_database_t db_ {};
    ustore_transaction_t txn_ {};
    ustore_snapshot_t snap_ {};
    ustore_arena_t* arena_ {};
    locations_store_t locations_ {};

    template <typename contents_arg_at>
    status_t any_assign(contents_arg_at&&, ustore_options_t) noexcept;

    template <typename expected_at = value_t>
    expected_gt<expected_at> any_get(ustore_options_t) noexcept;

    template <typename expected_at, typename contents_arg_at>
    expected_gt<expected_at> any_gather(contents_arg_at&&, ustore_options_t) noexcept;

  public:
    blobs_ref_gt(ustore_database_t db,
                 ustore_transaction_t txn,
                 ustore_snapshot_t snap,
                 locations_at&& locations,
                 ustore_arena_t* arena) noexcept
        : db_(db), txn_(txn), snap_(snap), arena_(arena), locations_(std::forward<locations_at>(locations)) {}

    blobs_ref_gt(blobs_ref_gt&&) = default;
    blobs_ref_gt& operator=(blobs_ref_gt&&) = default;
    blobs_ref_gt(blobs_ref_gt const&) = default;
    blobs_ref_gt& operator=(blobs_ref_gt const&) = default;

    blobs_ref_gt& on(arena_t& arena) noexcept {
        arena_ = arena.member_ptr();
        return *this;
    }

    expected_gt<value_t> value(bool watch = true) noexcept {
        return any_get<value_t>(!watch ? ustore_option_transaction_dont_watch_k : ustore_options_default_k);
    }

    operator expected_gt<value_t>() noexcept { return value(); }

    expected_gt<length_t> length(bool watch = true) noexcept {
        return any_get<length_t>(!watch ? ustore_option_transaction_dont_watch_k : ustore_options_default_k);
    }

    /**
     * @brief Checks if requested keys are present in the store.
     * ! Related values may be empty strings.
     */
    expected_gt<present_t> present(bool watch = true) noexcept {
        return any_get<present_t>(!watch ? ustore_option_transaction_dont_watch_k : ustore_options_default_k);
    }

    /**
     * @brief Pair-wise assigns values to keys located in this proxy objects.
     * @param vals values to be assigned.
     * @param flush Pass true, if you need the data to be persisted before returning.
     * @return status_t Non-NULL if only an error had occurred.
     */
    template <typename contents_arg_at>
    status_t assign(contents_arg_at&& vals, bool flush = false) noexcept {
        return any_assign(std::forward<contents_arg_at>(vals),
                          flush ? ustore_option_write_flush_k : ustore_options_default_k);
    }

    /**
     * @brief Removes both the keys and the associated values.
     * @param flush Pass true, if you need the data to be persisted before returning.
     * @return status_t Non-NULL if only an error had occurred.
     */
    status_t erase(bool flush = false) noexcept { //
        return assign(nullptr, flush);
    }

    /**
     * @brief Keeps the keys, but clears the contents of associated values.
     * @param flush Pass true, if you need the data to be persisted before returning.
     * @return status_t Non-NULL if only an error had occurred.
     */
    status_t clear(bool flush = false) noexcept {
        ustore_bytes_ptr_t any = reinterpret_cast<ustore_bytes_ptr_t>(this);
        ustore_length_t len = 0;
        contents_arg_t arg {};
        arg.offsets_begin = {};
        arg.lengths_begin = {&len};
        arg.contents_begin = {&any};
        arg.count = 1;
        return assign(arg, flush);
    }

    template <typename contents_arg_at>
    blobs_ref_gt& operator=(contents_arg_at&& vals) noexcept(false) {
        auto status = assign(std::forward<contents_arg_at>(vals));
        status.throw_unhandled();
        return *this;
    }

    blobs_ref_gt& operator=(std::nullptr_t) noexcept(false) {
        auto status = erase();
        status.throw_unhandled();
        return *this;
    }

    locations_plain_t& locations() noexcept { return locations_.ref(); }
    locations_plain_t& locations() const noexcept { return locations_.ref(); }
};

static_assert(blobs_ref_gt<ustore_key_t>::is_one_k);
static_assert(std::is_same_v<blobs_ref_gt<ustore_key_t>::value_t, value_view_t>);
static_assert(blobs_ref_gt<ustore_key_t>::is_one_k);
static_assert(!blobs_ref_gt<places_arg_t>::is_one_k);

template <typename locations_at>
template <typename expected_at>
expected_gt<expected_at> blobs_ref_gt<locations_at>::any_get(ustore_options_t options) noexcept {

    status_t status;
    ustore_length_t* found_offsets = nullptr;
    ustore_length_t* found_lengths = nullptr;
    ustore_bytes_ptr_t found_values = nullptr;
    ustore_octet_t* found_presences = nullptr;
    constexpr bool wants_value = std::is_same_v<value_t, expected_at>;
    constexpr bool wants_length = std::is_same_v<length_t, expected_at>;
    constexpr bool wants_present = std::is_same_v<present_t, expected_at>;

    decltype(auto) locs = locations_.ref();
    auto count = keys_extractor_t {}.count(locs);
    auto keys = keys_extractor_t {}.keys(locs);
    auto collections = keys_extractor_t {}.collections(locs);
    ustore_read_t read {};
    read.db = db_;
    read.error = status.member_ptr();
    read.transaction = txn_;
    read.snapshot = snap_;
    read.arena = arena_;
    read.options = options;
    read.tasks_count = count;
    read.collections = collections.get();
    read.collections_stride = collections.stride();
    read.keys = keys.get();
    read.keys_stride = keys.stride();
    read.presences = wants_present ? &found_presences : nullptr;
    read.offsets = wants_value ? &found_offsets : nullptr;
    read.lengths = wants_value || wants_length ? &found_lengths : nullptr;
    read.values = wants_value ? &found_values : nullptr;

    ustore_read(&read);

    if (!status)
        return std::move(status);

    if constexpr (wants_length) {
        ptr_range_gt<ustore_length_t> many {found_lengths, found_lengths + count};
        if constexpr (is_one_k)
            return many[0];
        else
            return many;
    }
    else if constexpr (wants_present) {
        bits_span_t many {found_presences};
        if constexpr (is_one_k)
            return bool(many[0]);
        else
            return many;
    }
    else {
        embedded_blobs_t many {count, found_offsets, found_lengths, found_values};
        if constexpr (is_one_k)
            return many[0];
        else
            return many;
    }
}

template <typename locations_at>
template <typename contents_arg_at>
status_t blobs_ref_gt<locations_at>::any_assign(contents_arg_at&& vals_ref, ustore_options_t options) noexcept {
    status_t status;
    using value_extractor_t = contents_arg_extractor_gt<std::remove_reference_t<contents_arg_at>>;

    decltype(auto) locs = locations_.ref();
    auto count = keys_extractor_t {}.count(locs);
    auto keys = keys_extractor_t {}.keys(locs);
    auto collections = keys_extractor_t {}.collections(locs);

    auto vals = vals_ref;
    auto contents = value_extractor_t {}.contents(vals);
    auto offsets = value_extractor_t {}.offsets(vals);
    auto lengths = value_extractor_t {}.lengths(vals);

    ustore_write_t write {};
    write.db = db_;
    write.error = status.member_ptr();
    write.transaction = txn_;
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
