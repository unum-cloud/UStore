/**
 *  @file cpp/ustore/ranges.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Strided iterators, spans and tapes that view the batched arguments and results of the C ABI, the
 *      adapters that turn C++ locations and values into them, and the owned batches deferred calls take.
 *
 *  A strided layout advances by a byte stride instead of @c sizeof, so a zero stride repeats one
 *  value. A tape indexes variable-length values by a base pointer and N+1 gapless offsets of 32 or
 *  64 bits, as every call exports them into its arena, and any other run of strings reaches the C
 *  ABI through an accessor.
 */

#pragma once

#include <climits> // `CHAR_BIT`
#include <cstddef> // `std::size_t`, `std::ptrdiff_t`

#include <algorithm>   // `std::ranges::sort`, `std::ranges::unique`
#include <array>       // `std::array`
#include <compare>     // `std::strong_ordering`
#include <concepts>    // `std::same_as`
#include <functional>  // `std::identity`
#include <iterator>    // `std::data`, `std::size`, `std::random_access_iterator`, `std::sortable`
#include <ranges>      // `std::ranges::contiguous_range`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::remove_cvref_t`, `std::conditional_t`, `std::remove_reference_t`
#include <utility>     // `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <stringzilla/stringzilla.hpp> // `sz::strings_t`, `sz::strings_view_t`, `sz::indexed_container_iterator`

#include "ustore/types.hpp" // `value_view_t`, `collection_key_field_t`, `edge_t`

namespace unum::ustore {

#pragma region Batches

/** A batch of locations or values rather than a single one, recognized by its nested @c value_type. */
template <typename batch_type_>
concept batch_concept = requires { typename std::remove_cvref_t<batch_type_>::value_type; };

/** A batch that carries its own byte stride instead of stepping by the size of its elements. */
template <typename batch_type_>
concept strided_batch_concept = batch_concept<batch_type_> &&
                                requires(std::remove_cvref_t<batch_type_> const& elements) { elements.stride(); };

/** Resolves a single location or value to itself. */
template <typename batch_type_>
struct batch_element {
    using type = std::remove_cvref_t<batch_type_>;
};

/** Resolves a batch to the type of its elements. */
template <batch_concept batch_type_>
struct batch_element<batch_type_> {
    using type = typename std::remove_cvref_t<batch_type_>::value_type;
};

template <typename batch_type_>
using batch_element_of = typename batch_element<batch_type_>::type;

/** Elements a deferred call can own, moved into it whole: a vector of them, or a fixed array, which spans exactly the
 *  bytes of its elements. */
template <typename array_type_, typename element_type_>
concept owned_array_concept =
    std::same_as<array_type_, st::vector<element_type_>> ||
    std::same_as<array_type_, std::array<element_type_, sizeof(array_type_) / sizeof(element_type_)>>;

/** Values, strings or fields a deferred call owns and passes to the C ABI as a tape. */
using values_tape_t = sz::strings_t;

#pragma endregion Batches

#pragma region Strided Spans

/** A pointer that advances by a byte stride instead of the size of its element.
 *  A zero stride repeats one element, so every task of a batch can share a single argument. */
template <typename element_type_>
class strided_iterator {
  public:
    using element_t = element_type_;
    using iterator_category = std::random_access_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = std::remove_cv_t<element_type_>;
    using pointer = element_type_*;
    using reference = element_type_&;

  private:
    element_type_* pointer_ {nullptr};
    ustore_size_t stride_ {0};

    element_type_* upshift(std::ptrdiff_t bytes) const noexcept { return (element_type_*)((char*)pointer_ + bytes); }
    element_type_* downshift(std::ptrdiff_t bytes) const noexcept { return (element_type_*)((char*)pointer_ - bytes); }
    std::ptrdiff_t signed_stride() const noexcept { return static_cast<std::ptrdiff_t>(stride_); }

  public:
    strided_iterator(element_type_* pointer = nullptr, ustore_size_t stride = 0) noexcept
        : pointer_(pointer), stride_(stride) {}

    element_type_& operator[](std::ptrdiff_t index) const noexcept { return *upshift(signed_stride() * index); }

    strided_iterator& operator++() noexcept {
        pointer_ = upshift(signed_stride());
        return *this;
    }

    strided_iterator& operator--() noexcept {
        pointer_ = downshift(signed_stride());
        return *this;
    }

    strided_iterator operator++(int) noexcept {
        strided_iterator const previous = *this;
        pointer_ = upshift(signed_stride());
        return previous;
    }

    strided_iterator operator--(int) noexcept {
        strided_iterator const previous = *this;
        pointer_ = downshift(signed_stride());
        return previous;
    }

    strided_iterator operator+(std::ptrdiff_t count) const noexcept {
        return {upshift(count * signed_stride()), stride_};
    }
    strided_iterator operator-(std::ptrdiff_t count) const noexcept {
        return {downshift(count * signed_stride()), stride_};
    }
    friend strided_iterator operator+(std::ptrdiff_t count, strided_iterator iterator) noexcept {
        return iterator + count;
    }

    strided_iterator& operator+=(std::ptrdiff_t count) noexcept {
        pointer_ = upshift(count * signed_stride());
        return *this;
    }

    strided_iterator& operator-=(std::ptrdiff_t count) noexcept {
        pointer_ = downshift(count * signed_stride());
        return *this;
    }

    /** The number of strides between two iterators, meaningful only for a stride that is a multiple of the element. */
    std::ptrdiff_t operator-(strided_iterator other) const noexcept {
        if (!stride_) return 0;
        return (pointer_ - other.pointer_) * static_cast<std::ptrdiff_t>(sizeof(element_type_)) / signed_stride();
    }

    explicit operator bool() const noexcept { return pointer_ != nullptr; }

    /** Whether a zero stride repeats the same element forever. */
    bool repeats() const noexcept { return !stride_; }

    /** Whether the elements sit next to each other without gaps. */
    bool is_continuous() const noexcept { return stride_ == sizeof(element_type_); }

    ustore_size_t stride() const noexcept { return stride_; }
    element_type_& operator*() const noexcept { return *pointer_; }
    element_type_* operator->() const noexcept { return pointer_; }
    element_type_* get() const noexcept { return pointer_; }

    friend bool operator==(strided_iterator const& first, strided_iterator const& second) noexcept {
        return first.pointer_ == second.pointer_;
    }

    friend std::strong_ordering operator<=>(strided_iterator const& first, strided_iterator const& second) noexcept {
        return first.pointer_ <=> second.pointer_;
    }

    /** Views one member of every element with the same stride. */
    template <typename member_type_, typename parent_type_>
    auto members(member_type_ parent_type_::* member_pointer) const noexcept {
        using member_t = std::conditional_t<std::is_const_v<element_type_>, member_type_ const, member_type_>;
        return strided_iterator<member_t> {pointer_ ? &(pointer_->*member_pointer) : nullptr, stride_};
    }
};

/** A mutable view of individual bits packed into octets. */
class bits_span_t {
  public:

    /** A proxy for one bit that reads as and assigns from @c bool. */
    class ref_t {
        ustore_octet_t* octet_ = nullptr;
        ustore_octet_t mask_ = 0;

      public:
        ref_t(ustore_octet_t& octet) noexcept : octet_(&octet), mask_(0) {}
        ref_t(ustore_octet_t* octet, ustore_octet_t mask) noexcept : octet_(octet), mask_(mask) {}
        operator bool() const noexcept { return *octet_ & mask_; }
        ref_t& operator=(std::same_as<bool> auto value) noexcept {
            *octet_ = value ? (*octet_ | mask_) : (*octet_ & ~mask_);
            return *this;
        }
    };

    using element_t = ustore_octet_t;
    using value_type = ustore_octet_t;
    using reference = ref_t;

  private:
    element_t* begin_ {nullptr};

  public:
    bits_span_t(element_t* begin = nullptr) noexcept : begin_(begin) {}

    ref_t at(std::size_t index) const noexcept {
        return {begin_ + index / CHAR_BIT, static_cast<element_t>(1 << (index % CHAR_BIT))};
    }
    ref_t operator[](std::size_t index) const noexcept { return at(index); }

    explicit operator bool() const noexcept { return begin_ != nullptr; }
    element_t* get() const noexcept { return begin_; }
    bool operator==(bits_span_t const& other) const noexcept { return begin_ == other.begin_; }
};

/** A read-only view of individual bits packed into octets. */
class bits_view_t {
  public:
    using element_t = ustore_octet_t const;
    using value_type = bool;
    using reference = void;

  private:
    element_t* begin_ {nullptr};

  public:
    bits_view_t(element_t* begin = nullptr) noexcept : begin_(begin) {}

    bool at(std::size_t index) const noexcept {
        return begin_[index / CHAR_BIT] & static_cast<ustore_octet_t>(1 << (index % CHAR_BIT));
    }
    bool operator[](std::size_t index) const noexcept { return at(index); }

    explicit operator bool() const noexcept { return begin_ != nullptr; }
    element_t* get() const noexcept { return begin_; }
    bool operator==(bits_view_t const& other) const noexcept { return begin_ == other.begin_; }
};

/** A counted run of strided elements, writable unless @p element_type_ is @c const, like
 *  @c std::span. */
template <typename element_type_>
class strided_span {
  public:
    using element_t = element_type_;
    using iterator_t = strided_iterator<element_type_>;
    using value_type = std::remove_cv_t<element_type_>;
    using reference = element_type_&;

  private:
    iterator_t begin_ {nullptr};
    ustore_size_t count_ {0};

  public:
    strided_span() = default;
    strided_span(iterator_t begin, ustore_size_t count) noexcept : begin_(begin), count_(count) {}

    element_type_* data() const noexcept { return begin_.get(); }
    iterator_t begin() const noexcept { return begin_; }
    iterator_t end() const noexcept { return begin_ + static_cast<std::ptrdiff_t>(count_); }
    element_type_& at(std::size_t index) const noexcept { return begin_[static_cast<std::ptrdiff_t>(index)]; }
    element_type_& operator[](std::size_t index) const noexcept { return at(index); }

    strided_span<element_type_ const> immutable() const noexcept { return {{begin_.get(), begin_.stride()}, count_}; }
    strided_span subspan(std::size_t offset, std::size_t count) const noexcept {
        return {begin_ + static_cast<std::ptrdiff_t>(offset), count};
    }

    bool empty() const noexcept { return !count_; }
    std::size_t size() const noexcept { return count_; }
    ustore_size_t stride() const noexcept { return begin_.stride(); }
    ustore_size_t count() const noexcept { return count_; }
    explicit operator bool() const noexcept { return static_cast<bool>(begin_); }

    /** Views one member of every element with the same stride. */
    template <typename member_type_, typename parent_type_>
    auto members(member_type_ parent_type_::* member_pointer) const noexcept {
        auto const member_begin = begin_.members(member_pointer);
        using member_t = typename std::remove_cvref_t<decltype(member_begin)>::element_t;
        return strided_span<member_t> {member_begin, count_};
    }

    /** Whether every element equals the first one, which a null or repeating span satisfies trivially. */
    bool same_elements() const noexcept {
        if (!begin_ || begin_.repeats()) return true;
        for (std::ptrdiff_t index = 1; index < static_cast<std::ptrdiff_t>(count_); ++index)
            if (!(begin_[index] == begin_[0])) return false;
        return true;
    }
};

/** Views a contiguous container as a strided span that steps by the size of its elements. */
template <std::ranges::contiguous_range container_type_>
    requires std::ranges::sized_range<container_type_>
strided_span<std::remove_reference_t<std::ranges::range_reference_t<container_type_>>> strided(
    container_type_&& container) noexcept {
    using element_t = std::ranges::range_value_t<container_type_>;
    return {{std::ranges::data(container), sizeof(element_t)}, std::ranges::size(container)};
}

/** Views the contiguous elements from @p begin up to @p end as a strided span. */
template <typename element_type_>
strided_span<element_type_> strided(element_type_* begin, element_type_* end) noexcept {
    return {{begin, sizeof(element_type_)}, static_cast<std::size_t>(end - begin)};
}

/** Iterates the strided run @p view of the C ABI describes, such as a @c ustore_keys_view_t, null when it has no
 *  data. */
template <typename view_type_>
    requires requires(view_type_ view) {
        { *view.data };
        { view.stride } -> std::convertible_to<ustore_size_t>;
    }
strided_iterator<std::remove_reference_t<decltype(*view_type_ {}.data)>> strided(view_type_ view) noexcept {
    return {view.data, view.stride};
}

/** A pair of iterators, so that a move-only stream can drive a range-based loop. */
template <typename iterator_type_>
class range {
    iterator_type_ begin_ {};
    iterator_type_ end_ {};

  public:
    range() = default;
    range(iterator_type_ begin, iterator_type_ end) noexcept : begin_(std::move(begin)), end_(std::move(end)) {}

    iterator_type_ const& begin() const& noexcept { return begin_; }
    iterator_type_ const& end() const& noexcept { return end_; }
    iterator_type_&& begin() && noexcept { return std::move(begin_); }
    iterator_type_&& end() && noexcept { return std::move(end_); }
};

#pragma endregion Strided Spans

#pragma region Argument Views

using keys_view_t = strided_span<ustore_key_t const>;

/** One task of a batched read or write. */
struct place_t {
    ustore_collection_id_t collection;
    ustore_key_t const& key;
    std::string_view field;

    collection_key_t collection_key() const noexcept { return {collection, key}; }
};

/** The locations of a batched read or write, as the C ABI receives them. */
struct place_batch_t {
    using value_type = place_t;
    strided_iterator<ustore_collection_id_t const> collections_begin;
    strided_iterator<ustore_key_t const> keys_begin;
    strided_iterator<std::string_view const> fields_begin;
    ustore_size_t count {0};

    std::size_t size() const noexcept { return count; }
    place_t operator[](std::size_t index) const noexcept {
        auto const signed_index = static_cast<std::ptrdiff_t>(index);
        ustore_collection_id_t collection = collections_begin ? collections_begin[signed_index]
                                                              : ustore_collection_id_t {};
        ustore_key_t const& key = keys_begin[signed_index];
        std::string_view field = fields_begin ? fields_begin[signed_index] : std::string_view {};
        return {collection, key, field};
    }

    /** Whether every task addresses the same collection. */
    bool same_collection() const noexcept {
        return strided_span<ustore_collection_id_t const>(collections_begin, count).same_elements();
    }

    /** The collections as the C ABI takes them. */
    ustore_collections_view_t collections() const noexcept {
        return {collections_begin.get(), collections_begin.stride()};
    }

    /** The keys as the C ABI takes them. */
    ustore_keys_view_t keys() const noexcept { return {keys_begin.get(), keys_begin.stride()}; }
};

/** Edges as three strided columns of sources, targets and edge identifiers, which the offsets of the read that
 *  exported them split by vertex. */
template <typename id_type_>
struct edges_span {

    using id_t = id_type_;
    using tuple_t = std::conditional_t<std::is_const_v<id_type_>, edge_t const, edge_t>;
    static_assert(sizeof(tuple_t) == 3 * sizeof(id_type_));

    strided_span<id_type_> sources;
    strided_span<id_type_> targets;
    strided_span<id_type_> ids;

    /** One offset per vertex of the read plus one, counted in edges, where a missing vertex spans none; empty for edges
     *  no read exported. */
    std::span<ustore_length_t const> offsets;

    edges_span() = default;

    edges_span(strided_span<id_type_> sources, strided_span<id_type_> targets, strided_span<id_type_> ids) noexcept
        : sources(sources), targets(targets), ids(ids) {}

    /** Edges between @p sources and @p targets that all carry the default edge identifier. */
    edges_span(strided_span<id_type_> sources, strided_span<id_type_> targets) noexcept
        : sources(sources), targets(targets) {
        static_assert(std::is_const_v<id_type_>,
                      "Only a read-only view can point at the shared default edge identifier");
        ids = {{&ustore_edge_id_default_k, 0}, 1};
    }

    edges_span(tuple_t* begin, tuple_t* end, std::span<ustore_length_t const> offsets = {}) noexcept
        : offsets(offsets) {
        auto const tuples = strided_span<tuple_t>({begin, sizeof(tuple_t)}, static_cast<std::size_t>(end - begin));
        sources = tuples.members(&edge_t::source);
        targets = tuples.members(&edge_t::target);
        ids = tuples.members(&edge_t::id);
    }

    std::size_t size() const noexcept { return fu::min_of(sources.count(), targets.count()); }

    edge_t operator[](std::size_t index) const noexcept {
        edge_t result;
        result.source = sources[index];
        result.target = targets[index];
        result.id = ids[index];
        return result;
    }

    /** The edges of the vertex at @p index of the read that exported them. */
    edges_span of_vertex(std::size_t index) const noexcept {
        std::size_t const begin = offsets[index], count = offsets[index + 1] - begin;
        return {sources.subspan(begin, count), targets.subspan(begin, count), ids.subspan(begin, count)};
    }

    operator edges_span<id_type_ const>() const noexcept { return immutable(); }
    edges_span<id_type_ const> immutable() const noexcept {
        edges_span<id_type_ const> result {sources.immutable(), targets.immutable(), ids.immutable()};
        result.offsets = offsets;
        return result;
    }
};

using edges_view_t = edges_span<ustore_key_t const>;

/** The edges a read of @p count vertices exported into its arena, split by vertex through its @p offsets. */
inline edges_view_t exported_edges(ustore_key_t const* edges, ustore_length_t const* offsets,
                                   std::size_t count) noexcept {
    if (!offsets) return {};
    edge_t const* const begin = reinterpret_cast<edge_t const*>(edges);
    return {begin, begin + offsets[count], {offsets, count + 1}};
}

/** Views a contiguous container of @c edge_t as strided edge columns. */
template <std::ranges::contiguous_range tuples_type_>
    requires std::ranges::sized_range<tuples_type_>
auto edges(tuples_type_&& tuples) noexcept {
    using element_t = std::remove_reference_t<std::ranges::range_reference_t<tuples_type_>>;
    using result_t = std::conditional_t<std::is_const_v<element_t>, edges_view_t, edges_span<ustore_key_t>>;
    auto const begin = std::ranges::data(tuples);
    return result_t(begin, begin + std::ranges::size(tuples));
}

#pragma endregion Argument Views

#pragma region Location Adapters

/** Stores a location by value and hands out references to it. */
template <typename location_type_>
class location_store {
  public:
    using plain_t = location_type_;
    using store_t = location_type_;

  private:
    store_t store_ {};

  public:
    location_store() = default;
    location_store(plain_t location) noexcept : store_(std::move(location)) {}

    plain_t& ref() noexcept { return store_; }
    plain_t const& ref() const noexcept { return store_; }
};

/** Widens a single key into a full location stored by value. */
template <key_location_concept location_type_>
class location_store<location_type_> : public location_store<collection_key_field_t> {
  public:
    using location_store<collection_key_field_t>::location_store;
};

/** Stores a caller-owned location by pointer, so a batch is never copied. */
template <typename location_type_>
    requires(!key_location_concept<location_type_>)
class location_store<location_type_&> {
  public:
    using plain_t = location_type_;
    using store_t = location_type_*;

  private:
    store_t store_ {nullptr};

  public:
    location_store() = default;
    location_store(plain_t& location) noexcept : store_(&location) {}

    plain_t& ref() noexcept { return *store_; }
    plain_t const& ref() const noexcept { return *store_; }
};

/** A batch of locations that all live in one collection the batch does not name itself. */
template <typename locations_type_>
struct locations_in_collection {

    /** The locations, stored by value or by reference. */
    location_store<locations_type_> without;

    /** Collection every location lives in. */
    ustore_collection_id_t collection;
};

/** Resolves a location or a batch of locations into the strided keys, collections and fields of the C ABI. */
template <typename locations_type_>
struct places_arg_extractor {

    using location_t = locations_type_;
    using element_t = batch_element_of<locations_type_>;
    static constexpr bool is_one_k = !batch_concept<locations_type_>;

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

    strided_iterator<std::string_view const> fields(location_t const& locations) noexcept {
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
struct places_arg_extractor<place_batch_t> {

    using location_t = place_batch_t;
    static constexpr bool is_one_k = false;

    ustore_size_t count(place_batch_t const& places) noexcept { return places.count; }
    strided_iterator<ustore_key_t const> keys(place_batch_t const& places) noexcept { return places.keys_begin; }
    strided_iterator<ustore_collection_id_t const> collections(place_batch_t const& places) noexcept {
        return places.collections_begin;
    }
    strided_iterator<std::string_view const> fields(place_batch_t const& places) noexcept {
        return places.fields_begin;
    }
};

/** Resolves a batch through its own extractor and repeats the one collection it lives in. */
template <typename locations_type_>
struct places_arg_extractor<locations_in_collection<locations_type_>> {

    using location_t = locations_in_collection<locations_type_>;
    using base_t = places_arg_extractor<std::remove_reference_t<locations_type_>>;
    static constexpr bool is_one_k = base_t::is_one_k;

    ustore_size_t count(location_t const& locations) noexcept { return base_t {}.count(locations.without.ref()); }
    strided_iterator<ustore_key_t const> keys(location_t const& locations) noexcept {
        return base_t {}.keys(locations.without.ref());
    }
    strided_iterator<ustore_collection_id_t const> collections(location_t const& locations) noexcept {
        return {&locations.collection};
    }
    strided_iterator<std::string_view const> fields(location_t const& locations) noexcept {
        return base_t {}.fields(locations.without.ref());
    }
};

template <typename locations_type_>
struct places_arg_extractor<locations_type_&> : public places_arg_extractor<locations_type_> {};

#pragma endregion Location Adapters

#pragma region Strings

/** An element a batch of strings may hold: a value view, a null-terminated string, or anything that converts into a
 *  string view. */
template <typename element_type_>
concept string_like_concept = std::same_as<element_type_, value_view_t> ||
                              std::is_convertible_v<element_type_ const&, std::string_view>;

/** The bytes @p text spells as an element of a batch, where a missing value and a null string spell empty ones. */
template <string_like_concept text_type_>
ustore_bytes_view_t bytes_of(text_type_ const& text) noexcept {
    if constexpr (std::same_as<text_type_, value_view_t>) return text.bytes();
    else if constexpr (std::is_convertible_v<text_type_ const&, char const*>) {
        char const* const string = text;
        return string ? bytes_of(std::string_view {string}) : ustore_bytes_view_t {};
    }
    else return bytes_of(std::string_view {text});
}

using utilities::u32_tape;
using utilities::u64_tape;

/** The tape @p strings pass to the C ABI as, or a zeroed view, the input's default, when they hold none. */
inline ustore_strings_view_t tape_of(values_tape_t const& strings) noexcept {
    sz_strings_span_t const& span = strings.raw;
    if (!span.count) return {};
    if (span.layout == sz_strings_u64_tape_k) return u64_tape(span.data.u64_tape.data, span.data.u64_tape.offsets);
    return u32_tape(span.data.u32_tape.data, span.data.u32_tape.offsets);
}

/** StringZilla's view of the @p count strings of an output @p tape, of either offset width. */
inline sz::strings_view_t strings_of(ustore_strings_view_t const& tape, std::size_t count) noexcept {
    if (tape.layout == ustore_strings_u64_tape_k)
        return {reinterpret_cast<char const*>(tape.data.u64_tape.data), tape.data.u64_tape.offsets, count};
    return {reinterpret_cast<char const*>(tape.data.u32_tape.data), tape.data.u32_tape.offsets, count};
}

/** The string at @p index of an output @p tape, read through the offsets its layout names. */
inline ustore_bytes_view_t tape_string(ustore_strings_view_t const& tape, std::size_t index) noexcept {
    if (tape.layout == ustore_strings_u64_tape_k) {
        std::uint64_t const* const offsets = tape.data.u64_tape.offsets;
        return {tape.data.u64_tape.data + offsets[index], offsets[index + 1] - offsets[index]};
    }
    std::uint32_t const* const offsets = tape.data.u32_tape.offsets;
    return {tape.data.u32_tape.data + offsets[index], offsets[index + 1] - offsets[index]};
}

/** The strings of an output @p tape from @p first on, viewing the same bytes, as a slice keeps its parent's offsets. */
inline ustore_strings_view_t tape_slice(ustore_strings_view_t tape, std::size_t first) noexcept {
    if (tape.layout == ustore_strings_u64_tape_k) {
        if (tape.data.u64_tape.offsets) tape.data.u64_tape.offsets += first;
    }
    else if (tape.data.u32_tape.offsets) tape.data.u32_tape.offsets += first;
    return tape;
}

/**
 *  @brief Lends a strided run of string-like elements to the C ABI through a @c ustore_strings_view_t accessor, where
 *      a zero stride repeats the first element for every task.
 *
 *  The view it hands out points at the object itself, so the object outlives every call given the view.
 */
template <string_like_concept element_type_>
class strided_strings {
    strided_iterator<element_type_ const> elements_ {};

    static void get_(void const* handle, ustore_size_t const* indices, ustore_size_t count,
                     ustore_bytes_view_t* views) noexcept {
        strided_iterator<element_type_ const> const elements = static_cast<strided_strings const*>(handle)->elements_;
        for (ustore_size_t index = 0; index != count; ++index)
            views[index] = bytes_of(elements[static_cast<std::ptrdiff_t>(indices[index])]);
    }

  public:
    strided_strings() noexcept = default;
    explicit strided_strings(strided_iterator<element_type_ const> elements) noexcept : elements_(elements) {}

    /** The accessor over the run, or a zeroed view, the input's default, for a null run. */
    ustore_strings_view_t view() const noexcept {
        ustore_strings_view_t view {};
        if (!elements_) return view;
        view.layout = ustore_strings_accessor_k;
        view.data.accessor = {&get_, this};
        return view;
    }
};

#pragma endregion Strings

#pragma region Read Results

/** The lengths a read exports into an arena beside its presence bits, where an absent value reads as
 *  @c missing_length_k, and a read that left its presences out reports every value present. */
class lengths_view_t {
    ustore_size_t count_ {0};
    ustore_octet_t const* presences_ {nullptr};
    ustore_length_t const* lengths_ {nullptr};

  public:
    using value_type = ustore_length_t;

    lengths_view_t() = default;
    lengths_view_t(ustore_size_t count, ustore_octet_t const* presences, ustore_length_t const* lengths) noexcept
        : count_(count), presences_(presences), lengths_(lengths) {}

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return !count_; }

    ustore_length_t operator[](std::size_t index) const noexcept {
        if (presences_ && !bits_view_t {presences_}[index]) return missing_length_k;
        return lengths_[index];
    }

    sz::indexed_container_iterator<lengths_view_t> begin() const noexcept { return {*this, 0}; }
    sz::indexed_container_iterator<lengths_view_t> end() const noexcept { return {*this, size()}; }

    bits_view_t presences() const noexcept { return {presences_}; }
    ustore_length_t const* lengths() const noexcept { return lengths_; }
};

/** The values a read exports into an arena as Apache Arrow does: presence bits and the tape of values, of either
 *  offset width, where an absent value reads as a missing view. */
class values_view_t {
    ustore_octet_t const* presences_ {nullptr};
    sz::strings_view_t strings_ {strings_of({}, 0)};

  public:
    using value_type = value_view_t;

    values_view_t() = default;
    values_view_t(ustore_size_t count, ustore_octet_t const* presences, ustore_strings_view_t tape) noexcept
        : presences_(presences), strings_(strings_of(tape, count)) {}

    std::size_t size() const noexcept { return strings_.size(); }
    bool empty() const noexcept { return !strings_.size(); }

    value_view_t operator[](std::size_t index) const noexcept {
        if (presences_ && !bits_view_t {presences_}[index]) return {};
        return std::string_view {strings_[index]};
    }

    sz::indexed_container_iterator<values_view_t> begin() const noexcept { return {*this, 0}; }
    sz::indexed_container_iterator<values_view_t> end() const noexcept { return {*this, size()}; }

    bits_view_t presences() const noexcept { return {presences_}; }

    /** The tape the values live in, where a missing value is an empty one, which a write takes back as it is. */
    ustore_strings_view_t tape() const noexcept {
        sz_strings_view_t const& view = strings_.raw;
        if (view.layout == sz_strings_u64_tape_k) return u64_tape(view.data.u64_tape.data, view.data.u64_tape.offsets);
        return u32_tape(view.data.u32_tape.data, view.data.u32_tape.offsets);
    }
};

#pragma endregion Read Results

#pragma region Value Adapters

/** Resolves values into the @c ustore_strings_view_t of the C ABI, specialized per decayed value type, keeping what
 *  an accessor reads for as long as the extractor lives. */
template <typename values_type_>
struct values_arg_extractor {};

/** Many null-terminated strings. */
template <>
struct values_arg_extractor<char const**> {
    strided_strings<char const*> strings;

    ustore_strings_view_t view(char const** values) noexcept {
        strings = strided_strings<char const*> {{values, sizeof(char const*)}};
        return strings.view();
    }
};

/** One null-terminated string, repeated for every key. */
template <>
struct values_arg_extractor<char const*> {
    strided_strings<char const*> strings;

    ustore_strings_view_t view(char const* const& value) noexcept {
        strings = strided_strings<char const*> {{&value, 0}};
        return strings.view();
    }
};

/** Many value views laid out one after another, as an array decays into. */
template <typename view_type_>
    requires std::same_as<std::remove_const_t<view_type_>, value_view_t>
struct values_arg_extractor<view_type_*> {
    strided_strings<value_view_t> strings;

    ustore_strings_view_t view(view_type_* values) noexcept {
        strings = strided_strings<value_view_t> {{values, sizeof(value_view_t)}};
        return strings.view();
    }
};

/** One value view, repeated for every key. */
template <>
struct values_arg_extractor<value_view_t> {
    strided_strings<value_view_t> strings;

    ustore_strings_view_t view(value_view_t const& value) noexcept {
        strings = strided_strings<value_view_t> {{&value, 0}};
        return strings.view();
    }
};

/** Strings the C ABI reads as they are, a tape or an accessor. */
template <>
struct values_arg_extractor<ustore_strings_view_t> {
    ustore_strings_view_t view(ustore_strings_view_t const& values) noexcept { return values; }
};

/** Values a read exported, written back as the tape they arrived in. */
template <>
struct values_arg_extractor<values_view_t> {
    ustore_strings_view_t view(values_view_t const& values) noexcept { return values.tape(); }
};

/** Values owned in one block behind their offsets, passed as a tape. */
template <>
struct values_arg_extractor<values_tape_t> {
    ustore_strings_view_t view(values_tape_t const& values) noexcept { return tape_of(values); }
};

/** A contiguous container of value views or of strings, read through an accessor. */
template <std::ranges::contiguous_range strings_type_>
    requires std::ranges::sized_range<strings_type_> && string_like_concept<std::ranges::range_value_t<strings_type_>>
struct values_arg_extractor<strings_type_> {
    using element_t = std::ranges::range_value_t<strings_type_>;
    strided_strings<element_t> strings;

    ustore_strings_view_t view(strings_type_ const& values) noexcept {
        if (std::ranges::empty(values)) return {};
        strings = strided_strings<element_t> {{std::ranges::data(values), sizeof(element_t)}};
        return strings.view();
    }
};

#pragma endregion Value Adapters

#pragma region Algorithms

/** Writes the first @p count transformed elements into @p output. */
template <typename output_iterator_type_, typename iterator_type_, typename transform_type_ = std::identity>
void transform_n(iterator_type_ begin, std::size_t count, output_iterator_type_ output,
                 transform_type_ transform = {}) {
    for (std::size_t index = 0; index != count; ++index) output[index] = transform(begin[index]);
}

/** Whether the first @p count elements strictly ascend. */
template <typename iterator_type_>
bool all_ascending(iterator_type_ begin, std::size_t count) {
    for (std::size_t index = 1; index < count; ++index)
        if (!(begin[index - 1] < begin[index])) return false;
    return true;
}

/** Sorts the elements, gathers the distinct ones at the front, and returns how many there are. */
template <std::random_access_iterator iterator_type_>
    requires std::sortable<iterator_type_>
std::size_t sort_and_deduplicate(iterator_type_ begin, iterator_type_ end) {
    std::ranges::sort(begin, end);
    return static_cast<std::size_t>(std::ranges::unique(begin, end).begin() - begin);
}

#pragma endregion Algorithms

} // namespace unum::ustore
