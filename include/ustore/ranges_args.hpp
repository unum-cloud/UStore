/**
 *  @file include/ustore/ranges_args.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Views over the batched arguments of the C ABI, and adapters that turn C++ locations and values into them.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::nullptr_t`
#include <cstring> // `std::strlen`

#include <algorithm>   // `std::min`
#include <concepts>    // `std::same_as`
#include <iterator>    // `std::data`, `std::size`
#include <ranges>      // `std::ranges::contiguous_range`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::conditional_t`, `std::remove_reference_t`
#include <utility>     // `std::move`

#include <ustore/ranges.hpp> // `strided_iterator`
#include <ustore/types.hpp>  // `collection_key_field_t`

namespace unum::ustore {

#pragma region Argument Views

using keys_view_t = strided_range<ustore_key_t const>;
using fields_view_t = strided_range<ustore_str_view_t const>;

/** One task of a batched read or write. */
struct place_t {
    ustore_collection_id_t collection;
    ustore_key_t const& key;
    ustore_str_view_t field;

    collection_key_t collection_key() const noexcept { return {collection, key}; }
    collection_key_field_t collection_key_field() const noexcept { return {collection, key, field}; }
};

/** The locations of a batched read or write, as the C ABI receives them. */
struct places_arg_t {
    using value_type = place_t;
    strided_iterator<ustore_collection_id_t const> collections_begin;
    strided_iterator<ustore_key_t const> keys_begin;
    strided_iterator<ustore_str_view_t const> fields_begin;
    ustore_size_t count {0};

    std::size_t size() const noexcept { return count; }
    place_t operator[](std::size_t index) const noexcept {
        auto const signed_index = static_cast<std::ptrdiff_t>(index);
        ustore_collection_id_t collection = collections_begin ? collections_begin[signed_index]
                                                              : ustore_collection_main_k;
        ustore_key_t const& key = keys_begin[signed_index];
        ustore_str_view_t field = fields_begin ? fields_begin[signed_index] : nullptr;
        return {collection, key, field};
    }

    /** Whether every task addresses the same collection. */
    bool same_collection() const noexcept {
        return strided_range<ustore_collection_id_t const>(collections_begin, count).same_elements();
    }
};

/** The string @p text of @p length, whose length a null terminator gives when zero. */
inline std::string_view string_of(ustore_str_view_t text, ustore_length_t length) noexcept {
    if (!text) return {};
    return {text, length ? length : std::strlen(text)};
}

/** The strings of one batch, as the C ABI receives them. */
struct strings_arg_t {

    /** Pointers to the strings. */
    strided_iterator<ustore_str_view_t const> texts;

    /** Optional lengths of the strings, where a null iterator means they are null-terminated. */
    strided_iterator<ustore_length_t const> lengths;

    /** The string of task @p index. */
    std::string_view operator[](std::size_t index) const noexcept {
        auto const signed_index = static_cast<std::ptrdiff_t>(index);
        ustore_str_view_t const text = texts[signed_index];
        if (!text) return {};
        std::size_t const length = lengths ? lengths[signed_index] : std::strlen(text);
        return {text, length};
    }
};

/** The values of a batched write, as the C ABI receives them. */
struct contents_arg_t {
    using value_type = value_view_t;
    bits_view_t presences_begin;
    strided_iterator<ustore_length_t const> offsets_begin;
    strided_iterator<ustore_length_t const> lengths_begin;
    strided_iterator<ustore_bytes_cptr_t const> contents_begin;
    ustore_size_t count {0};
    ustore_char_t separator {'\0'};

    std::size_t size() const noexcept { return count; }
    value_view_t operator[](std::size_t index) const noexcept {
        auto const signed_index = static_cast<std::ptrdiff_t>(index);
        if (!contents_begin || !contents_begin[signed_index] || (presences_begin && !presences_begin[index])) return {};

        auto const begin = reinterpret_cast<byte_t const*>(contents_begin[signed_index]);
        ustore_length_t const offset = offsets_begin ? offsets_begin[signed_index] : 0u;
        ustore_length_t length = 0;
        if (lengths_begin) length = lengths_begin[signed_index];
        else if (offsets_begin) length = offsets_begin[signed_index + 1] - offset;
        else {
            auto const item = reinterpret_cast<char const*>(begin) + offset;
            while (item[length] != separator) ++length;
        }
        return {begin + offset, length};
    }

    /** Whether the values form one tape indexed by N+1 offsets. */
    bool is_arrow() const noexcept { return contents_begin.repeats() && offsets_begin && !lengths_begin; }

    /** Whether every value starts right where the one before it ends. */
    bool is_continuous() const noexcept {
        value_view_t last = operator[](0);
        for (std::size_t index = 1; index != size(); ++index) {
            value_view_t const value = operator[](index);
            if (value.begin() != last.end()) return false;
            last = value;
        }
        return true;
    }
};

/** One task of a batched scan. */
struct scan_t {
    ustore_collection_id_t collection;
    ustore_key_t min_key;
    ustore_length_t limit;
};

/** The arguments of @c ustore_scan gathered into a structure of arrays for validation. */
struct scans_arg_t {
    strided_iterator<ustore_collection_id_t const> collections;
    strided_iterator<ustore_key_t const> start_keys;
    strided_iterator<ustore_length_t const> limits;
    ustore_size_t count = 0;

    std::size_t size() const noexcept { return count; }
    scan_t operator[](std::size_t index) const noexcept {
        auto const signed_index = static_cast<std::ptrdiff_t>(index);
        ustore_collection_id_t collection = collections ? collections[signed_index] : ustore_collection_main_k;
        ustore_key_t min_key = start_keys ? start_keys[signed_index] : smallest_key_k;
        ustore_length_t limit = limits[signed_index];
        return {collection, min_key, limit};
    }

    /** Whether every task scans the same collection. */
    bool same_collection() const noexcept {
        return strided_range<ustore_collection_id_t const>(collections, count).same_elements();
    }
};

/** One task of a batched sample. */
struct sample_arg_t {
    ustore_collection_id_t collection;
    ustore_length_t limit;
};

/** The arguments of @c ustore_sample gathered into a structure of arrays for validation. */
struct sample_args_t {
    strided_iterator<ustore_collection_id_t const> collections;
    strided_iterator<ustore_length_t const> limits;
    ustore_size_t count = 0;

    std::size_t size() const noexcept { return count; }
    sample_arg_t operator[](std::size_t index) const noexcept {
        auto const signed_index = static_cast<std::ptrdiff_t>(index);
        ustore_collection_id_t collection = collections ? collections[signed_index] : ustore_collection_main_k;
        ustore_length_t limit = limits[signed_index];
        return {collection, limit};
    }

    /** Whether every task samples the same collection. */
    bool same_collection() const noexcept {
        return strided_range<ustore_collection_id_t const>(collections, count).same_elements();
    }
};

/** One task of a batched edge lookup. */
struct find_edge_t {
    ustore_collection_id_t collection;
    ustore_key_t const& vertex_id;
    ustore_vertex_role_t role;
};

/** The vertices of a batched edge lookup, as the C ABI receives them. */
struct find_edges_t {
    using value_type = find_edge_t;
    strided_iterator<ustore_collection_id_t const> collections_begin;
    strided_iterator<ustore_key_t const> vertex_id_begin;
    strided_iterator<ustore_vertex_role_t const> roles_begin;
    ustore_size_t count = 0;

    std::size_t size() const noexcept { return count; }
    find_edge_t operator[](std::size_t index) const noexcept {
        auto const signed_index = static_cast<std::ptrdiff_t>(index);
        ustore_collection_id_t collection = collections_begin ? collections_begin[signed_index]
                                                              : ustore_collection_main_k;
        ustore_key_t const& vertex_id = vertex_id_begin[signed_index];
        ustore_vertex_role_t role = roles_begin ? roles_begin[signed_index] : ustore_vertex_role_any_k;
        return {collection, vertex_id, role};
    }

    /** Whether every task looks inside the same collection. */
    bool same_collection() const noexcept {
        return strided_range<ustore_collection_id_t const>(collections_begin, count).same_elements();
    }
};

/** Edges as three strided columns of sources, targets and edge identifiers. */
template <typename id_>
struct edges_range {

    using id_t = id_;
    using tuple_t = std::conditional_t<std::is_const_v<id_>, edge_t const, edge_t>;
    static_assert(sizeof(tuple_t) == 3 * sizeof(id_));

    strided_range<id_> source_ids;
    strided_range<id_> target_ids;
    strided_range<id_> edge_ids;

    edges_range() = default;

    edges_range(strided_range<id_> sources, strided_range<id_> targets, strided_range<id_> edges) noexcept
        : source_ids(sources), target_ids(targets), edge_ids(edges) {}

    /** Edges between @p sources and @p targets that all carry the default edge identifier. */
    edges_range(strided_range<id_> sources, strided_range<id_> targets) noexcept
        : source_ids(sources), target_ids(targets) {
        static_assert(std::is_const_v<id_>, "Only a read-only view can point at the shared default edge identifier");
        edge_ids = {{&ustore_default_edge_id_k, 0}, 1};
    }

    edges_range(tuple_t* begin, tuple_t* end) noexcept {
        auto const tuples = strided_range<tuple_t>({begin, sizeof(tuple_t)}, static_cast<std::size_t>(end - begin));
        source_ids = tuples.members(&edge_t::source_id);
        target_ids = tuples.members(&edge_t::target_id);
        edge_ids = tuples.members(&edge_t::id);
    }

    std::size_t size() const noexcept { return std::min(source_ids.count(), target_ids.count()); }

    edge_t operator[](std::size_t index) const noexcept {
        edge_t result;
        result.source_id = source_ids[index];
        result.target_id = target_ids[index];
        result.id = edge_ids[index];
        return result;
    }

    operator edges_range<id_ const>() const noexcept { return immutable(); }
    edges_range<id_ const> immutable() const noexcept {
        return {source_ids.immutable(), target_ids.immutable(), edge_ids.immutable()};
    }
};

using edges_span_t = edges_range<ustore_key_t>;
using edges_view_t = edges_range<ustore_key_t const>;

/** Views a contiguous container of @c edge_t as strided edge columns. */
template <std::ranges::contiguous_range tuples_>
    requires std::ranges::sized_range<tuples_>
auto edges(tuples_&& tuples) noexcept {
    using element_t = std::remove_reference_t<std::ranges::range_reference_t<tuples_>>;
    using result_t = std::conditional_t<std::is_const_v<element_t>, edges_view_t, edges_span_t>;
    auto const begin = std::ranges::data(tuples);
    return result_t(begin, begin + std::ranges::size(tuples));
}

#pragma endregion Argument Views

#pragma region Location Adapters

/** Stores a location by value and hands out references to it. */
template <typename location_>
class location_store {
  public:
    using plain_t = location_;
    using store_t = location_;

  private:
    store_t store_ {};

  public:
    location_store() = default;
    location_store(plain_t location) noexcept : store_(std::move(location)) {}

    plain_t& ref() noexcept { return store_; }
    plain_t const& ref() const noexcept { return store_; }
};

/** Widens a single key into a full location stored by value. */
template <key_location_concept location_>
class location_store<location_> : public location_store<collection_key_field_t> {
  public:
    using location_store<collection_key_field_t>::location_store;
};

/** Stores a caller-owned location by pointer, so a batch is never copied. */
template <typename location_>
    requires(!key_location_concept<location_>)
class location_store<location_&> {
  public:
    using plain_t = location_;
    using store_t = location_*;

  private:
    store_t store_ {nullptr};

  public:
    location_store() = default;
    location_store(plain_t& location) noexcept : store_(&location) {}

    plain_t& ref() noexcept { return *store_; }
    plain_t const& ref() const noexcept { return *store_; }
};

/** A batch of locations that all live in one collection the batch does not name itself. */
template <typename locations_>
struct locations_in_collection {

    /** The locations, stored by value or by reference. */
    location_store<locations_> without;

    /** Collection every location lives in. */
    ustore_collection_id_t collection;
};

/** Resolves a location or a batch of locations into the strided keys, collections and fields of the C ABI. */
template <typename locations_>
struct places_arg_extractor {

    using location_t = locations_;
    using element_t = batch_element_t<locations_>;
    static constexpr bool is_one_k = !batch_concept<locations_>;

    ustore_size_t count(location_t const& locations) noexcept {
        if constexpr (is_one_k) return 1;
        else return std::size(locations);
    }

    strided_iterator<ustore_key_t const> keys(location_t const& locations) noexcept {
        if constexpr (std::same_as<element_t, ustore_key_t>) return elements(locations);
        else return elements(locations).members(&element_t::key);
    }

    strided_iterator<ustore_collection_id_t const> collections(location_t const& locations) noexcept {
        if constexpr (has_collection_concept<element_t>) return elements(locations).members(&element_t::collection);
        else return {};
    }

    strided_iterator<ustore_str_view_t const> fields(location_t const& locations) noexcept {
        if constexpr (has_field_concept<element_t>) return elements(locations).members(&element_t::field);
        else return {};
    }

  private:
    static strided_iterator<element_t const> elements(location_t const& locations) noexcept {
        if constexpr (is_one_k) return {&locations, 0};
        else if constexpr (strided_batch_concept<location_t>) return {std::data(locations), locations.stride()};
        else return {std::data(locations), sizeof(element_t)};
    }
};

/** Passes the locations of the C ABI through unchanged. */
template <>
struct places_arg_extractor<places_arg_t> {

    using location_t = places_arg_t;
    static constexpr bool is_one_k = false;

    ustore_size_t count(places_arg_t const& places) noexcept { return places.count; }
    strided_iterator<ustore_key_t const> keys(places_arg_t const& places) noexcept { return places.keys_begin; }
    strided_iterator<ustore_collection_id_t const> collections(places_arg_t const& places) noexcept {
        return places.collections_begin;
    }
    strided_iterator<ustore_str_view_t const> fields(places_arg_t const& places) noexcept {
        return places.fields_begin;
    }
};

/** Resolves a batch through its own extractor and repeats the one collection it lives in. */
template <typename locations_>
struct places_arg_extractor<locations_in_collection<locations_>> {

    using location_t = locations_in_collection<locations_>;
    using base_t = places_arg_extractor<std::remove_reference_t<locations_>>;
    static constexpr bool is_one_k = base_t::is_one_k;

    ustore_size_t count(location_t const& locations) noexcept { return base_t {}.count(locations.without.ref()); }
    strided_iterator<ustore_key_t const> keys(location_t const& locations) noexcept {
        return base_t {}.keys(locations.without.ref());
    }
    strided_iterator<ustore_collection_id_t const> collections(location_t const& locations) noexcept {
        return {&locations.collection};
    }
    strided_iterator<ustore_str_view_t const> fields(location_t const& locations) noexcept {
        return base_t {}.fields(locations.without.ref());
    }
};

template <typename locations_>
struct places_arg_extractor<locations_&> : public places_arg_extractor<locations_> {};

#pragma endregion Location Adapters

#pragma region Content Adapters

/** Resolves values into the strided contents, offsets and lengths of the C ABI, specialized per decayed value type. */
template <typename values_>
struct contents_arg_extractor {};

/** Many null-terminated strings. */
template <>
struct contents_arg_extractor<char const**> {
    strided_iterator<ustore_bytes_cptr_t const> contents(char const** strings) noexcept {
        return {reinterpret_cast<ustore_bytes_cptr_t const*>(strings), sizeof(char const*)};
    }
    strided_iterator<ustore_length_t const> offsets(char const**) noexcept { return {}; }
    strided_iterator<ustore_length_t const> lengths(char const**) noexcept { return {}; }
};

/** One null-terminated string, repeated for every key. */
template <>
struct contents_arg_extractor<char const*> {
    strided_iterator<ustore_bytes_cptr_t const> contents(char const* const& string) noexcept {
        return {reinterpret_cast<ustore_bytes_cptr_t const*>(&string), 0};
    }
    strided_iterator<ustore_length_t const> offsets(char const* const&) noexcept { return {}; }
    strided_iterator<ustore_length_t const> lengths(char const* const&) noexcept { return {}; }
};

/** Many value views laid out one after another, as an array decays into. */
template <typename view_>
    requires std::same_as<std::remove_const_t<view_>, value_view_t>
struct contents_arg_extractor<view_*> {
    strided_iterator<ustore_bytes_cptr_t const> contents(view_* views) noexcept {
        return {views->member_ptr(), sizeof(value_view_t)};
    }
    strided_iterator<ustore_length_t const> offsets(view_*) noexcept { return {}; }
    strided_iterator<ustore_length_t const> lengths(view_* views) noexcept {
        return {views->member_length(), sizeof(value_view_t)};
    }
};

/** One value view, repeated for every key. */
template <>
struct contents_arg_extractor<value_view_t> {
    strided_iterator<ustore_bytes_cptr_t const> contents(value_view_t const& view) noexcept {
        return {view.member_ptr(), 0};
    }
    strided_iterator<ustore_length_t const> offsets(value_view_t const&) noexcept { return {}; }
    strided_iterator<ustore_length_t const> lengths(value_view_t const& view) noexcept {
        return {view.member_length(), 0};
    }
};

/** Passes the values of the C ABI through unchanged. */
template <>
struct contents_arg_extractor<contents_arg_t> {
    strided_iterator<ustore_bytes_cptr_t const> contents(contents_arg_t const& native) noexcept {
        return native.contents_begin;
    }
    strided_iterator<ustore_length_t const> offsets(contents_arg_t const& native) noexcept {
        return native.offsets_begin;
    }
    strided_iterator<ustore_length_t const> lengths(contents_arg_t const& native) noexcept {
        return native.lengths_begin;
    }
};

/** No values, which erases the keys. */
template <>
struct contents_arg_extractor<std::nullptr_t> {
    strided_iterator<ustore_bytes_cptr_t const> contents(std::nullptr_t) noexcept { return {}; }
    strided_iterator<ustore_length_t const> offsets(std::nullptr_t) noexcept { return {}; }
    strided_iterator<ustore_length_t const> lengths(std::nullptr_t) noexcept { return {}; }
};

/** A contiguous container of value views. */
template <std::ranges::contiguous_range views_>
    requires std::same_as<std::ranges::range_value_t<views_>, value_view_t>
struct contents_arg_extractor<views_> {
    strided_iterator<ustore_bytes_cptr_t const> contents(views_ const& views) noexcept {
        if (std::ranges::empty(views)) return {};
        return {std::ranges::data(views)->member_ptr(), sizeof(value_view_t)};
    }
    strided_iterator<ustore_length_t const> offsets(views_ const&) noexcept { return {}; }
    strided_iterator<ustore_length_t const> lengths(views_ const& views) noexcept {
        if (std::ranges::empty(views)) return {};
        return {std::ranges::data(views)->member_length(), sizeof(value_view_t)};
    }
};

#pragma endregion Content Adapters

#pragma region Validation

/** Whether the tasks address a named collection rather than the main one. */
inline bool same_collections_are_named(strided_iterator<ustore_collection_id_t const> collections_begin) noexcept {
    return collections_begin && collections_begin[0] != ustore_collection_main_k;
}

/** Whether every flag set in @p value is also set in @p allowed. */
template <typename enum_, typename allowed_mask_>
bool enum_is_subset(enum_ value, allowed_mask_ allowed) noexcept {
    return (value & ~allowed) == 0;
}

inline void validate_write(ustore_transaction_t const transaction, places_arg_t const& places,
                           contents_arg_t const& contents, ustore_options_t const options,
                           ustore_error_t* error) noexcept {

    auto const allowed_options = ustore_option_transaction_dont_watch_k | ustore_option_dont_discard_memory_k |
                                 ustore_option_write_flush_k;
    if (!enum_is_subset(options, allowed_options)) {
        *error = {ustore_error_invalid_argument_k, "Invalid options"};
        return;
    }
    if (transaction && (options & ustore_option_write_flush_k)) {
        *error = {ustore_error_invalid_argument_k, "A transaction flushes at commit, not at a write"};
        return;
    }
    if (!transaction && (options & ustore_option_transaction_dont_watch_k)) {
        *error = {ustore_error_invalid_argument_k, "Only a transaction watches keys"};
        return;
    }
    if (!places.count) {
        if (places.keys_begin) *error = {ustore_error_invalid_argument_k, "Keys were given for no tasks"};
        return;
    }
    if (!places.keys_begin) {
        *error = {ustore_error_invalid_argument_k, "No keys were provided"};
        return;
    }
    if (!contents.contents_begin && (contents.lengths_begin || contents.offsets_begin))
        *error = {ustore_error_invalid_argument_k, "Lengths or offsets were given without contents"};
}

inline void validate_read(ustore_transaction_t const transaction, places_arg_t const& places,
                          ustore_options_t const options, ustore_error_t* error) noexcept {

    auto const allowed_options = ustore_option_transaction_dont_watch_k | ustore_option_dont_discard_memory_k |
                                 ustore_option_read_shared_memory_k;
    if (!enum_is_subset(options, allowed_options)) {
        *error = {ustore_error_invalid_argument_k, "Invalid options"};
        return;
    }
    if (!transaction && (options & ustore_option_transaction_dont_watch_k)) {
        *error = {ustore_error_invalid_argument_k, "Only a transaction watches keys"};
        return;
    }
    if (!places.keys_begin) *error = {ustore_error_invalid_argument_k, "No keys were provided"};
}

inline void validate_scan([[maybe_unused]] ustore_transaction_t const transaction, scans_arg_t const& scans,
                          ustore_options_t const options, ustore_error_t* error) noexcept {

    auto const allowed_options = ustore_option_transaction_dont_watch_k | ustore_option_dont_discard_memory_k |
                                 ustore_option_read_shared_memory_k | ustore_option_scan_bulk_k;
    if (!enum_is_subset(options, allowed_options)) {
        *error = {ustore_error_invalid_argument_k, "Invalid options"};
        return;
    }
    if (!scans.start_keys) {
        *error = {ustore_error_invalid_argument_k, "No start keys were provided"};
        return;
    }
    if (!scans.limits) *error = {ustore_error_unsupported_k, "Full scans are not supported, paginate with limits"};
}

inline void validate_transaction_begin(ustore_transaction_t const* transaction, ustore_options_t const options,
                                       ustore_error_t* error) noexcept {

    if (!transaction) {
        *error = {ustore_error_invalid_argument_k, "Need an output for the transaction"};
        return;
    }
    if (!enum_is_subset(options, ustore_option_transaction_dont_watch_k))
        *error = {ustore_error_invalid_argument_k, "Invalid options"};
}

inline void validate_transaction_commit(ustore_transaction_t const transaction, ustore_options_t const options,
                                        ustore_error_t* error) noexcept {

    if (!transaction) {
        *error = {ustore_error_uninitialized_k, "Transaction is uninitialized"};
        return;
    }
    if (!enum_is_subset(options, ustore_option_write_flush_k))
        *error = {ustore_error_invalid_argument_k, "Invalid options"};
}

#pragma endregion Validation

} // namespace unum::ustore
