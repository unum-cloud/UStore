/**
 * @file blobs_ref.hpp
 * @author Ashot Vardanian
 * @date 26 Jun 2022
 * @addtogroup Cpp
 *
 * @brief C++ bindings for "ukv/db.h".
 */

#pragma once
#include "ukv/ukv.h"
#include "ukv/cpp/types.hpp"      // `arena_t`
#include "ukv/cpp/status.hpp"     // `status_t`
#include "ukv/cpp/sfinae.hpp"     // `location_store_gt`
#include "ukv/cpp/docs_table.hpp" // `docs_table_t`

namespace unum::ukv {

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
 * - (ukv_collection_t?, ukv_key_t, ukv_field_t?): Single KV-pair location.
 * - (ukv_collection_t*, ukv_key_t*, ukv_field_t*): Externally owned range of keys.
 * - (ukv_collection_t[x], ukv_key_t[x], ukv_field_t[x]): On-stack array of addresses.
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
    using length_t = std::conditional_t<is_one_k, ukv_length_t, ptr_range_gt<ukv_length_t>>;

  protected:
    ukv_database_t db_ {};
    ukv_transaction_t txn_ {};
    ukv_snapshot_t snap_ {};
    ukv_arena_t* arena_ {};
    locations_store_t locations_ {};

    template <typename contents_arg_at>
    status_t any_assign(contents_arg_at&&, ukv_options_t) noexcept;

    template <typename expected_at = value_t>
    expected_gt<expected_at> any_get(ukv_options_t) noexcept;

    template <typename expected_at, typename contents_arg_at>
    expected_gt<expected_at> any_gather(contents_arg_at&&, ukv_options_t) noexcept;

  public:
    blobs_ref_gt(ukv_database_t db,
                 ukv_transaction_t txn,
                 ukv_snapshot_t snap,
                 locations_at&& locations,
                 ukv_arena_t* arena) noexcept
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
        return any_get<value_t>(!watch ? ukv_option_transaction_dont_watch_k : ukv_options_default_k);
    }

    operator expected_gt<value_t>() noexcept { return value(); }

    expected_gt<length_t> length(bool watch = true) noexcept {
        return any_get<length_t>(!watch ? ukv_option_transaction_dont_watch_k : ukv_options_default_k);
    }

    /**
     * @brief Checks if requested keys are present in the store.
     * ! Related values may be empty strings.
     */
    expected_gt<present_t> present(bool watch = true) noexcept {
        return any_get<present_t>(!watch ? ukv_option_transaction_dont_watch_k : ukv_options_default_k);
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
                          flush ? ukv_option_write_flush_k : ukv_options_default_k);
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
        ukv_bytes_ptr_t any = reinterpret_cast<ukv_bytes_ptr_t>(this);
        ukv_length_t len = 0;
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

static_assert(blobs_ref_gt<ukv_key_t>::is_one_k);
static_assert(std::is_same_v<blobs_ref_gt<ukv_key_t>::value_t, value_view_t>);
static_assert(blobs_ref_gt<ukv_key_t>::is_one_k);
static_assert(!blobs_ref_gt<places_arg_t>::is_one_k);

template <typename locations_at>
template <typename expected_at>
expected_gt<expected_at> blobs_ref_gt<locations_at>::any_get(ukv_options_t options) noexcept {

    status_t status;
    ukv_length_t* found_offsets = nullptr;
    ukv_length_t* found_lengths = nullptr;
    ukv_bytes_ptr_t found_values = nullptr;
    ukv_octet_t* found_presences = nullptr;
    constexpr bool wants_value = std::is_same_v<value_t, expected_at>;
    constexpr bool wants_length = std::is_same_v<length_t, expected_at>;
    constexpr bool wants_present = std::is_same_v<present_t, expected_at>;

    decltype(auto) locs = locations_.ref();
    auto count = keys_extractor_t {}.count(locs);
    auto keys = keys_extractor_t {}.keys(locs);
    auto collections = keys_extractor_t {}.collections(locs);
    ukv_read_t read {};
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

    ukv_read(&read);

    if (!status)
        return std::move(status);

    if constexpr (wants_length) {
        ptr_range_gt<ukv_length_t> many {found_lengths, found_lengths + count};
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
status_t blobs_ref_gt<locations_at>::any_assign(contents_arg_at&& vals_ref, ukv_options_t options) noexcept {
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

    ukv_write_t write {};
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

    ukv_write(&write);
    return status;
}

} // namespace unum::ukv
