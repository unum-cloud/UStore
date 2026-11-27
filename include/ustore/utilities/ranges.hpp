/**
 *  @file include/ustore/utilities/ranges.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Strided iterators, spans and tapes that view the batched arguments and results of the
 *      C ABI inside the engines, and the few algorithms they run over them.
 *
 *  A strided layout advances by a byte stride instead of @c sizeof, so a zero stride repeats one
 *  value. A tape indexes variable-length values by a base pointer and N+1 gapless offsets of 32 or
 *  64 bits, as every call exports them into its arena.
 */

#pragma once

#include <cassert> // `assert`
#include <climits> // `CHAR_BIT`
#include <cstddef> // `std::size_t`, `std::ptrdiff_t`

#include <algorithm>   // `std::ranges::sort`, `std::ranges::unique`
#include <compare>     // `std::strong_ordering`
#include <concepts>    // `std::same_as`, `std::convertible_to`
#include <functional>  // `std::identity`
#include <iterator>    // `std::random_access_iterator`, `std::sortable`
#include <ranges>      // `std::ranges::contiguous_range`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::remove_cv_t`, `std::conditional_t`, `std::remove_reference_t`

#include <forkunion/types.hpp>         // `fu::min_of`
#include <stringzilla/strings.hpp>     // `sz::strings_view_t`, `sz::indexed_container_iterator`

#include "ustore/ustore.h"
#include "ustore/utilities/types.hpp" // `value_view_t`, `collection_key_t`, `edge_t`

namespace unum::ustore::utilities {

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

    /** The number of strides between two iterators, meaningful only for a stride that is a multiple
     *  of the element. */
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
    strided_iterator<std::conditional_t<std::is_const_v<element_type_>, member_type_ const, member_type_>> members(
        member_type_ parent_type_::* member_pointer) const noexcept {
        return {pointer_ ? &(pointer_->*member_pointer) : nullptr, stride_};
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

/** Whether @p container_type_ lays out elements a pointer to @p element_type_ addresses, back to
 *  back. */
template <typename container_type_, typename element_type_>
concept a_contiguous_range_of =
    std::ranges::contiguous_range<container_type_> && std::ranges::sized_range<container_type_> &&
    std::same_as<std::ranges::range_value_t<container_type_>, std::remove_cv_t<element_type_>> &&
    requires(container_type_& container) {
        { std::ranges::data(container) } -> std::convertible_to<element_type_*>;
    };

/** A counted run of strided elements, writable unless @p element_type_ is @c const,
 *  like @c std::span. */
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

    /** Views a contiguous container of the same elements, stepping by their size, as @c std::span
     *  does. */
    template <typename container_type_>
        requires a_contiguous_range_of<container_type_, element_type_>
    strided_span(container_type_&& container) noexcept
        : begin_(std::ranges::data(container), sizeof(element_type_)), count_(std::ranges::size(container)) {}

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
    strided_span<std::conditional_t<std::is_const_v<element_type_>, member_type_ const, member_type_>> members(
        member_type_ parent_type_::* member_pointer) const noexcept {
        return {begin_.members(member_pointer), count_};
    }

    /** Whether every element equals the first one, which a null or repeating span satisfies
     *  trivially. */
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

/** Iterates the strided run @p view of the C ABI describes, such as a @c ustore_keys_view_t, null
 *  when it has no data. */
template <typename view_type_>
    requires requires(view_type_ view) {
        { *view.data };
        { view.stride } -> std::convertible_to<ustore_size_t>;
    }
strided_iterator<std::remove_reference_t<decltype(*view_type_ {}.data)>> strided(view_type_ view) noexcept {
    return {view.data, view.stride};
}

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
        std::ptrdiff_t const signed_index = static_cast<std::ptrdiff_t>(index);
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

/** Edges as three strided columns of sources, targets and edge identifiers, which the offsets of
 *  the read that exported them split by vertex. */
template <typename id_type_>
struct edges_span {

    using id_t = id_type_;
    using tuple_t = std::conditional_t<std::is_const_v<id_type_>, edge_t const, edge_t>;

    strided_span<id_type_> sources;
    strided_span<id_type_> targets;
    strided_span<id_type_> ids;

    /** One offset per vertex of the read plus one, counted in edges, where a missing vertex spans
     *  none; empty for edges no read exported. */
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
        strided_span<tuple_t> const tuples({begin, sizeof(tuple_t)}, static_cast<std::size_t>(end - begin));
        sources = tuples.members(&edge_t::source);
        targets = tuples.members(&edge_t::target);
        ids = tuples.members(&edge_t::id);
    }

    /** Views a contiguous container of edges as strided edge columns. */
    template <typename tuples_type_>
        requires a_contiguous_range_of<tuples_type_, tuple_t>
    edges_span(tuples_type_&& tuples) noexcept
        : edges_span(std::ranges::data(tuples), std::ranges::data(tuples) + std::ranges::size(tuples)) {}

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

#pragma endregion Argument Views

#pragma region Tapes

/** The offset where string @p index of an output @p tape starts, read at the width its layout
 *  names. */
inline std::size_t tape_offset(ustore_strings_view_t const& tape, std::size_t index) noexcept {
    if (tape.layout == ustore_strings_u64_tape_k) return static_cast<std::size_t>(tape.data.u64_tape.offsets[index]);
    return tape.data.u32_tape.offsets[index];
}

/** The string at @p index of an output @p tape, read through the offsets its layout names. */
inline ustore_bytes_view_t tape_string(ustore_strings_view_t const& tape, std::size_t index) noexcept {
    std::size_t const begin = tape_offset(tape, index), end = tape_offset(tape, index + 1);
    assert(end >= begin && "A tape's offsets must not decrease");
    if (tape.layout == ustore_strings_u64_tape_k) return {tape.data.u64_tape.data + begin, end - begin};
    return {tape.data.u32_tape.data + begin, end - begin};
}

/** The strings of an output @p tape from @p first on, viewing the same bytes, as a slice keeps its
 *  parent's offsets. */
inline ustore_strings_view_t tape_slice(ustore_strings_view_t tape, std::size_t first) noexcept {
    if (tape.layout == ustore_strings_u64_tape_k) {
        if (tape.data.u64_tape.offsets) tape.data.u64_tape.offsets += first;
    }
    else if (tape.data.u32_tape.offsets) tape.data.u32_tape.offsets += first;
    return tape;
}

/**
 *  @brief Views a strided run of texts for a call through an accessor, where a zero stride repeats
 *      the first text for every task.
 *
 *  The view it hands out points at the object itself, so the object outlives every call
 *  given the view.
 */
template <typename element_type_>
class strided_strings {
    strided_iterator<element_type_ const> elements_ {};

    static void get_(void const* handle, ustore_size_t const* indices, ustore_size_t count,
                     ustore_bytes_view_t* views) noexcept {
        strided_iterator<element_type_ const> const elements = static_cast<strided_strings const*>(handle)->elements_;
        for (ustore_size_t index = 0; index != count; ++index)
            views[index] = bytes_of(std::string_view {elements[static_cast<std::ptrdiff_t>(indices[index])]});
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

/** StringZilla's view of the @p count strings of an output @p tape, of either offset width. */
inline sz::strings_view_t strings_of(ustore_strings_view_t const& tape, std::size_t count) noexcept {
    if (tape.layout == ustore_strings_u64_tape_k)
        return {reinterpret_cast<char const*>(tape.data.u64_tape.data), tape.data.u64_tape.offsets, count};
    return {reinterpret_cast<char const*>(tape.data.u32_tape.data), tape.data.u32_tape.offsets, count};
}

/** The values a read exports into an arena as Apache Arrow does: presence bits and the tape of
 *  values, of either offset width, where an absent value reads as a missing view. */
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

    /** The tape the values live in, where a missing value is an empty one, which a write takes back
     *  as it is. */
    ustore_strings_view_t tape() const noexcept {
        sz_strings_view_t const& view = strings_.raw;
        if (view.layout == sz_strings_u64_tape_k) return u64_tape(view.data.u64_tape.data, view.data.u64_tape.offsets);
        return u32_tape(view.data.u32_tape.data, view.data.u32_tape.offsets);
    }
};

#pragma endregion Tapes

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

} // namespace unum::ustore::utilities
