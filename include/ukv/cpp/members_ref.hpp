/**
 * @file members_ref.hpp
 * @author Ashot Vardanian
 * @date 26 Jun 2022
 * @brief C++ bindings for @see "ukv/db.h".
 */

#pragma once
#include "ukv/ukv.h"
#include "ukv/cpp/types.hpp"  // `arena_t`
#include "ukv/cpp/status.hpp" // `status_t`
#include "ukv/cpp/sfinae.hpp" // `location_store_gt`

namespace unum::ukv {

template <typename locations_store_t>
class members_ref_gt;

/**
 * @brief A proxy object, that allows both lookups and writes
 * with `[]` and assignment operators for a batch of keys
 * simultaneously.
 * Following assignment combinations are possible:
 * > one value to many keys
 * > many values to many keys
 * > one value to one key
 * The only impossible combination is assigning many values to one key.
 *
 * @tparam locations_at Type describing the address of a value in DBMS.
 * > (ukv_collection_t?, ukv_key_t, ukv_field_t?): Single KV-pair location.
 * > (ukv_collection_t*, ukv_key_t*, ukv_field_t*): Externally owned range of keys.
 * > (ukv_collection_t[x], ukv_key_t[x], ukv_field_t[x]): On-stack array of addresses.
 *
 * @section Memory Management
 * Every "container" that overloads the @b [] operator has an internal "arena",
 * that is shared between all the `members_ref_gt`s produced from it. That will
 * work great, unless:
 * > multiple threads are working with same collection handle or transaction.
 * > reading responses interleaves with new requests, which gobbles temporary memory.
 * For those cases, you can create a separate `arena_t` and pass it to `.on(...)`
 * member function. In such HPC environments we would recommend to @b reuse one such
 * are on every thread.
 *
 * @section Class Specs
 * > Copyable: Yes.
 * > Exceptions: Never.
 */
template <typename locations_at>
class members_ref_gt {
  public:
    static_assert(!std::is_rvalue_reference_v<locations_at>, //
                  "The internal object can't be an R-value Reference");

    using locations_store_t = location_store_gt<locations_at>;
    using locations_plain_t = typename locations_store_t::plain_t;
    using keys_extractor_t = keys_arg_extractor_gt<locations_plain_t>;
    static constexpr bool is_one_k = keys_extractor_t::is_one_k;

    using value_t = std::conditional_t<is_one_k, value_view_t, taped_values_view_t>;
    using present_t = std::conditional_t<is_one_k, bool, strided_range_gt<bool>>;
    using length_t = std::conditional_t<is_one_k, ukv_val_len_t, indexed_range_gt<ukv_val_len_t*>>;

  protected:
    ukv_t db_ = nullptr;
    ukv_txn_t txn_ = nullptr;
    ukv_arena_t* arena_ = nullptr;
    locations_store_t locations_;
    ukv_format_t format_ = ukv_format_binary_k;

    template <typename values_arg_at>
    status_t any_assign(values_arg_at&&, ukv_options_t) noexcept;
    expected_gt<value_t> any_get(ukv_options_t) noexcept;

  public:
    members_ref_gt(ukv_t db,
                   ukv_txn_t txn,
                   locations_store_t locations,
                   arena_t& arena,
                   ukv_format_t format = ukv_format_binary_k) noexcept
        : db_(db), txn_(txn), arena_(arena.member_ptr()), locations_(locations), format_(format) {}

    members_ref_gt(members_ref_gt&&) = default;
    members_ref_gt& operator=(members_ref_gt&&) = default;
    members_ref_gt(members_ref_gt const&) = default;
    members_ref_gt& operator=(members_ref_gt const&) = default;

    arena_t& arena() const noexcept { return *reinterpret_cast<arena_t*>(arena_); }
    members_ref_gt& on(arena_t& arena) noexcept {
        arena_ = arena.member_ptr();
        return *this;
    }

    members_ref_gt& as(ukv_format_t format) noexcept {
        format_ = format;
        return *this;
    }

    expected_gt<value_t> value(bool track = false) noexcept {
        return any_get(track ? ukv_option_read_track_k : ukv_options_default_k);
    }

    operator expected_gt<value_t>() noexcept { return value(); }

    expected_gt<length_t> length(bool track = false) noexcept {
        auto options = (track ? ukv_option_read_track_k : ukv_options_default_k) | ukv_option_read_lengths_k;
        auto maybe = any_get(static_cast<ukv_options_t>(options));
        if (!maybe)
            return maybe.release_status();

        if constexpr (is_one_k)
            return length_t {*maybe ? static_cast<length_t>(maybe->size()) : ukv_val_len_missing_k};
        else {
            auto found_lengths = maybe->lengths();
            auto count = keys_extractor_t {}.count(locations_.ref());
            return length_t {found_lengths, found_lengths + count};
        }
    }

    /**
     * @brief Checks if requested keys are present in the store.
     * ! Related values may be empty strings.
     */
    expected_gt<present_t> present(bool track = false) noexcept {

        auto maybe = length(track);
        if (!maybe)
            return maybe.release_status();

        if constexpr (is_one_k)
            return present_t {*maybe != ukv_val_len_missing_k};
        else {
            // Transform the `found_lengths` into booleans.
            auto found_lengths = maybe->begin();
            auto count = keys_extractor_t {}.count(locations_.ref());
            std::transform(found_lengths, found_lengths + count, found_lengths, [](ukv_val_len_t len) {
                return len != ukv_val_len_missing_k;
            });

            // Cast assuming "Little-Endian" architecture
            auto last_byte_offset = 0; // sizeof(ukv_val_len_t) - sizeof(bool);
            auto booleans = reinterpret_cast<bool*>(found_lengths);
            return present_t {booleans + last_byte_offset, sizeof(ukv_val_len_t), count};
        }
    }

    /**
     * @brief Pair-wise assigns values to keys located in this proxy objects.
     * @param flush Pass true, if you need the data to be persisted before returning.
     * @return status_t Non-NULL if only an error had occurred.
     */
    template <typename values_arg_at>
    status_t assign(values_arg_at&& vals, bool flush = false) noexcept {
        return any_assign(std::forward<values_arg_at>(vals), flush ? ukv_option_write_flush_k : ukv_options_default_k);
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
        ukv_val_ptr_t any = reinterpret_cast<ukv_val_ptr_t>(this);
        ukv_val_len_t len = 0;
        values_arg_t arg {
            .contents_begin = {&any},
            .offsets_begin = {},
            .lengths_begin = {&len},
        };
        return assign(arg, flush);
    }

    template <typename values_arg_at>
    members_ref_gt& operator=(values_arg_at&& vals) noexcept(false) {
        auto status = assign(std::forward<values_arg_at>(vals));
        status.throw_unhandled();
        return *this;
    }

    members_ref_gt& operator=(nullptr_t) noexcept(false) {
        auto status = erase();
        status.throw_unhandled();
        return *this;
    }

    locations_plain_t& locations() noexcept { return locations_.ref(); }
    locations_plain_t& locations() const noexcept { return locations_.ref(); }
};

static_assert(members_ref_gt<ukv_key_t>::is_one_k);
static_assert(std::is_same_v<members_ref_gt<ukv_key_t>::value_t, value_view_t>);
static_assert(members_ref_gt<ukv_key_t>::is_one_k);
static_assert(!members_ref_gt<keys_arg_t>::is_one_k);

template <typename locations_at>
expected_gt<typename members_ref_gt<locations_at>::value_t> //
members_ref_gt<locations_at>::any_get(ukv_options_t options) noexcept {
    status_t status;
    ukv_val_len_t* found_lengths = nullptr;
    ukv_val_ptr_t found_values = nullptr;

    decltype(auto) locs = locations_.ref();
    auto count = keys_extractor_t {}.count(locs);
    auto keys = keys_extractor_t {}.keys(locs);
    auto cols = keys_extractor_t {}.cols(locs);
    auto fields = keys_extractor_t {}.fields(locs);
    auto has_fields = fields && (!fields.repeats() || *fields);

    if (has_fields || format_ != ukv_format_binary_k)
        ukv_docs_read( //
            db_,
            txn_,
            count,
            cols.get(),
            cols.stride(),
            keys.get(),
            keys.stride(),
            fields.get(),
            fields.stride(),
            options,
            format_,
            &found_lengths,
            &found_values,
            arena_,
            status.member_ptr());
    else
        ukv_read( //
            db_,
            txn_,
            count,
            cols.get(),
            cols.stride(),
            keys.get(),
            keys.stride(),
            options,
            &found_lengths,
            &found_values,
            arena_,
            status.member_ptr());

    if (!status)
        return status;

    if constexpr (is_one_k)
        return value_view_t {found_values, *found_lengths};
    else
        return taped_values_view_t {found_lengths, found_values, count};
}

template <typename locations_at>
template <typename values_arg_at>
status_t members_ref_gt<locations_at>::any_assign(values_arg_at&& vals_ref, ukv_options_t options) noexcept {
    status_t status;
    using value_extractor_t = values_arg_extractor_gt<std::remove_reference_t<values_arg_at>>;

    decltype(auto) locs = locations_.ref();
    auto count = keys_extractor_t {}.count(locs);
    auto keys = keys_extractor_t {}.keys(locs);
    auto cols = keys_extractor_t {}.cols(locs);
    auto fields = keys_extractor_t {}.fields(locs);
    auto has_fields = fields && (!fields.repeats() || *fields);

    auto vals = vals_ref;
    auto contents = value_extractor_t {}.contents(vals);
    auto offsets = value_extractor_t {}.offsets(vals);
    auto lengths = value_extractor_t {}.lengths(vals);

    if (has_fields || format_ != ukv_format_binary_k)
        ukv_docs_write( //
            db_,
            txn_,
            count,
            cols.get(),
            cols.stride(),
            keys.get(),
            keys.stride(),
            fields.get(),
            fields.stride(),
            options,
            format_,
            contents.get(),
            contents.stride(),
            offsets.get(),
            offsets.stride(),
            lengths.get(),
            lengths.stride(),
            arena_,
            status.member_ptr());
    else
        ukv_write( //
            db_,
            txn_,
            count,
            cols.get(),
            cols.stride(),
            keys.get(),
            keys.stride(),
            contents.get(),
            contents.stride(),
            offsets.get(),
            offsets.stride(),
            lengths.get(),
            lengths.stride(),
            options,
            arena_,
            status.member_ptr());
    return status;
}

} // namespace unum::ukv
