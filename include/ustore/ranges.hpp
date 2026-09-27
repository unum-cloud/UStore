/**
 *  @file include/ustore/ranges.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Strided iterators, ranges and tapes that view the batched arguments and results of the C ABI.
 *
 *  A strided layout advances by a byte stride instead of @c sizeof, so a zero stride repeats one value.
 *  A joined tape indexes variable-length values by a base pointer and N+1 gapless offsets, and an
 *  embedded tape pairs N offsets with N lengths in any order.
 */

#pragma once

#include <climits> // `CHAR_BIT`
#include <cstddef> // `std::size_t`, `std::ptrdiff_t`
#include <cstring> // `std::memcpy`, `std::memmove`, `std::strlen`

#include <algorithm>   // `std::ranges::sort`, `std::ranges::unique`
#include <compare>     // `std::strong_ordering`
#include <concepts>    // `std::same_as`
#include <functional>  // `std::identity`
#include <iterator>    // `std::random_access_iterator`, `std::sortable`
#include <ranges>      // `std::ranges::contiguous_range`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::remove_cvref_t`, `std::conditional_t`
#include <utility>     // `std::move`

#include <ustore/types.hpp> // `value_view_t`

namespace unum::ustore {

#pragma region Batches

/** A batch of locations or values rather than a single one, recognized by its nested @c value_type. */
template <typename type_>
concept batch_concept = requires { typename std::remove_cvref_t<type_>::value_type; };

/** A batch that carries its own byte stride instead of stepping by the size of its elements. */
template <typename type_>
concept strided_batch_concept = batch_concept<type_> &&
                                requires(std::remove_cvref_t<type_> const& elements) { elements.stride(); };

/** Resolves a single location or value to itself. */
template <typename type_>
struct batch_element {
    using type = std::remove_cvref_t<type_>;
};

/** Resolves a batch to the type of its elements. */
template <batch_concept type_>
struct batch_element<type_> {
    using type = typename std::remove_cvref_t<type_>::value_type;
};

template <typename type_>
using batch_element_t = typename batch_element<type_>::type;

#pragma endregion Batches

#pragma region Strided Ranges

/** A pointer that advances by a byte stride instead of the size of its element.
 *  A zero stride repeats one element, so every task of a batch can share a single argument. */
template <typename element_>
class strided_iterator {
  public:
    using element_t = element_;
    using iterator_category = std::random_access_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = std::remove_cv_t<element_>;
    using pointer = element_*;
    using reference = element_&;

  private:
    element_* raw_ {nullptr};
    ustore_size_t stride_ {0};

    element_* upshift(std::ptrdiff_t bytes) const noexcept { return (element_*)((char*)raw_ + bytes); }
    element_* downshift(std::ptrdiff_t bytes) const noexcept { return (element_*)((char*)raw_ - bytes); }
    std::ptrdiff_t signed_stride() const noexcept { return static_cast<std::ptrdiff_t>(stride_); }

  public:
    strided_iterator(element_* raw = nullptr, ustore_size_t stride = 0) noexcept : raw_(raw), stride_(stride) {}

    element_& operator[](std::ptrdiff_t index) const noexcept { return *upshift(signed_stride() * index); }

    strided_iterator& operator++() noexcept {
        raw_ = upshift(signed_stride());
        return *this;
    }

    strided_iterator& operator--() noexcept {
        raw_ = downshift(signed_stride());
        return *this;
    }

    strided_iterator operator++(int) const noexcept { return {upshift(signed_stride()), stride_}; }
    strided_iterator operator--(int) const noexcept { return {downshift(signed_stride()), stride_}; }
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
        raw_ = upshift(count * signed_stride());
        return *this;
    }

    strided_iterator& operator-=(std::ptrdiff_t count) noexcept {
        raw_ = downshift(count * signed_stride());
        return *this;
    }

    /** The number of strides between two iterators, meaningful only for a stride that is a multiple of the element. */
    std::ptrdiff_t operator-(strided_iterator other) const noexcept {
        if (!stride_) return 0;
        return (raw_ - other.raw_) * static_cast<std::ptrdiff_t>(sizeof(element_)) / signed_stride();
    }

    explicit operator bool() const noexcept { return raw_ != nullptr; }

    /** Whether a zero stride repeats the same element forever. */
    bool repeats() const noexcept { return !stride_; }

    /** Whether the elements sit next to each other without gaps. */
    bool is_continuous() const noexcept { return stride_ == sizeof(element_); }

    ustore_size_t stride() const noexcept { return stride_; }
    element_& operator*() const noexcept { return *raw_; }
    element_* operator->() const noexcept { return raw_; }
    element_* get() const noexcept { return raw_; }

    friend bool operator==(strided_iterator const& first, strided_iterator const& second) noexcept {
        return first.raw_ == second.raw_;
    }

    friend std::strong_ordering operator<=>(strided_iterator const& first, strided_iterator const& second) noexcept {
        return first.raw_ <=> second.raw_;
    }

    /** Views one member of every element with the same stride. */
    template <typename member_, typename parent_>
    auto members(member_ parent_::* member_pointer) const noexcept {
        using member_t = std::conditional_t<std::is_const_v<element_>, member_ const, member_>;
        return strided_iterator<member_t> {raw_ ? &(raw_->*member_pointer) : nullptr, stride_};
    }
};

/** A mutable view of individual bits packed into octets. */
class bits_span_t {
  public:

    /** A proxy for one bit that reads as and assigns from @c bool. */
    class ref_t {
        ustore_octet_t* raw_ = nullptr;
        ustore_octet_t mask_ = 0;

      public:
        ref_t(ustore_octet_t& raw) noexcept : raw_(&raw), mask_(0) {}
        ref_t(ustore_octet_t* raw, ustore_octet_t mask) noexcept : raw_(raw), mask_(mask) {}
        operator bool() const noexcept { return *raw_ & mask_; }
        ref_t& operator=(std::same_as<bool> auto value) noexcept {
            *raw_ = value ? (*raw_ | mask_) : (*raw_ & ~mask_);
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
        return begin_[index / CHAR_BIT] & static_cast<element_t>(1 << (index % CHAR_BIT));
    }
    bool operator[](std::size_t index) const noexcept { return at(index); }

    explicit operator bool() const noexcept { return begin_ != nullptr; }
    element_t* get() const noexcept { return begin_; }
    bool operator==(bits_view_t const& other) const noexcept { return begin_ == other.begin_; }
};

/** A counted run of strided elements. */
template <typename element_>
class strided_range {
  public:
    using element_t = element_;
    using iterator_t = strided_iterator<element_>;
    using value_type = std::remove_cv_t<element_>;
    using reference = element_&;

  private:
    iterator_t begin_ {nullptr};
    ustore_size_t count_ {0};

  public:
    strided_range() = default;
    strided_range(iterator_t begin, std::size_t count) noexcept : begin_(begin), count_(count) {}

    element_* data() const noexcept { return begin_.get(); }
    iterator_t begin() const noexcept { return begin_; }
    iterator_t end() const noexcept { return begin_ + static_cast<std::ptrdiff_t>(count_); }
    element_& at(std::size_t index) const noexcept { return begin_[static_cast<std::ptrdiff_t>(index)]; }
    element_& operator[](std::size_t index) const noexcept { return at(index); }

    strided_range<element_ const> immutable() const noexcept { return {{begin_.get(), begin_.stride()}, count_}; }
    strided_range subspan(std::size_t offset, std::size_t count) const noexcept {
        return {begin_ + static_cast<std::ptrdiff_t>(offset), count};
    }

    bool empty() const noexcept { return !count_; }
    std::size_t size() const noexcept { return count_; }
    ustore_size_t stride() const noexcept { return begin_.stride(); }
    ustore_size_t count() const noexcept { return count_; }
    explicit operator bool() const noexcept { return static_cast<bool>(begin_); }

    /** Views one member of every element with the same stride. */
    template <typename member_, typename parent_>
    auto members(member_ parent_::* member_pointer) const noexcept {
        auto const member_begin = begin_.members(member_pointer);
        using member_t = typename std::remove_cvref_t<decltype(member_begin)>::element_t;
        return strided_range<member_t> {member_begin, count_};
    }

    /** Whether every element equals the first one, which a null or repeating range satisfies trivially. */
    bool same_elements() const noexcept {
        if (!begin_ || begin_.repeats()) return true;
        for (std::ptrdiff_t index = 1; index < static_cast<std::ptrdiff_t>(count_); ++index)
            if (!(begin_[index] == begin_[0])) return false;
        return true;
    }
};

/** Views a contiguous container as a strided range that steps by the size of its elements. */
template <std::ranges::contiguous_range container_>
    requires std::ranges::sized_range<container_>
strided_range<std::remove_reference_t<std::ranges::range_reference_t<container_>>> strided(
    container_&& container) noexcept {
    using element_t = std::ranges::range_value_t<container_>;
    return {{std::ranges::data(container), sizeof(element_t)}, std::ranges::size(container)};
}

/** Views the contiguous elements from @p begin up to @p end as a strided range. */
template <typename element_>
strided_range<element_> strided(element_* begin, element_* end) noexcept {
    return {{begin, sizeof(element_)}, static_cast<std::size_t>(end - begin)};
}

/** A pair of iterators, so that a move-only stream can drive a range-based loop. */
template <typename iterator_>
class range {
    iterator_ begin_ {nullptr};
    iterator_ end_ {nullptr};

  public:
    range() = default;
    range(iterator_ begin, iterator_ end) noexcept : begin_(std::move(begin)), end_(std::move(end)) {}

    iterator_ const& begin() const& noexcept { return begin_; }
    iterator_ const& end() const& noexcept { return end_; }
    iterator_&& begin() && noexcept { return std::move(begin_); }
    iterator_&& end() && noexcept { return std::move(end_); }
};

#pragma endregion Strided Ranges

#pragma region Tapes and Flat Arrays

/** A forward iterator over values packed one after another, which needs only their lengths. */
template <typename chunk_>
class consecutive_chunks_iterator {

    using chunk_t = chunk_;
    using element_t = typename chunk_::value_type;

    ustore_length_t const* lengths_ {nullptr};
    element_t* contents_ {nullptr};

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = chunk_;
    using pointer = void;
    using reference = void;

    consecutive_chunks_iterator() = default;

    template <typename same_size_>
    consecutive_chunks_iterator(ustore_length_t const* lengths, same_size_* contents) noexcept
        : lengths_(lengths), contents_((element_t*)(contents)) {
        static_assert(sizeof(same_size_) == sizeof(element_t));
    }

    consecutive_chunks_iterator& operator++() noexcept {
        contents_ += *lengths_;
        ++lengths_;
        return *this;
    }

    consecutive_chunks_iterator operator++(int) const noexcept { return {lengths_ + 1, contents_ + *lengths_}; }
    chunk_ operator*() const noexcept { return {contents_, *lengths_}; }

    bool operator==(consecutive_chunks_iterator const& other) const noexcept { return lengths_ == other.lengths_; }
};

using consecutive_strs_iterator_t = consecutive_chunks_iterator<std::string_view>;
using consecutive_blobs_iterator_t = consecutive_chunks_iterator<value_view_t>;

/** An iterator over values joined into one tape and indexed by N+1 offsets. */
template <typename chunk_>
class joined_chunks_iterator {

    using chunk_t = chunk_;
    using element_t = typename chunk_::value_type;

    ustore_length_t* offsets_ {nullptr};
    element_t* contents_ {nullptr};

  public:
    using iterator_category = std::random_access_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = chunk_;
    using pointer = void;
    using reference = void;

    joined_chunks_iterator() = default;

    template <typename same_size_>
    joined_chunks_iterator(ustore_length_t* offsets, same_size_* contents) noexcept
        : offsets_(offsets), contents_((element_t*)(contents)) {
        static_assert(sizeof(same_size_) == sizeof(element_t));
    }

    joined_chunks_iterator& operator++() noexcept {
        ++offsets_;
        return *this;
    }

    joined_chunks_iterator operator++(int) const noexcept { return {offsets_ + 1, contents_}; }
    joined_chunks_iterator operator--(int) const noexcept { return {offsets_ - 1, contents_}; }
    chunk_ operator*() const noexcept { return {contents_ + offsets_[0], offsets_[1] - offsets_[0]}; }
    chunk_ operator[](std::size_t index) const noexcept {
        return {contents_ + offsets_[index], offsets_[index + 1] - offsets_[index]};
    }

    bool operator==(joined_chunks_iterator const& other) const noexcept { return offsets_ == other.offsets_; }
};

using joined_strs_iterator_t = joined_chunks_iterator<std::string_view>;
using joined_blobs_iterator_t = joined_chunks_iterator<value_view_t>;

/** Values joined into one tape and indexed by N+1 offsets. */
template <typename chunk_>
class joined_chunks {

    using chunk_t = chunk_;
    using element_t = typename chunk_::value_type;

    ustore_size_t count_ {0};
    ustore_length_t* offsets_ {nullptr};
    element_t* contents_ {nullptr};

  public:
    using value_type = chunk_;

    joined_chunks() = default;

    template <typename same_size_>
    joined_chunks(ustore_size_t count, ustore_length_t* offsets, same_size_* contents) noexcept
        : count_(count), offsets_(offsets), contents_((element_t*)(contents)) {
        static_assert(sizeof(same_size_) == sizeof(element_t));
    }

    joined_chunks_iterator<chunk_> begin() const noexcept { return {offsets_, contents_}; }
    joined_chunks_iterator<chunk_> end() const noexcept { return {offsets_ + count_, contents_}; }
    std::size_t size() const noexcept { return count_; }
    chunk_ operator[](std::size_t index) const noexcept {
        return {contents_ + offsets_[index], offsets_[index + 1] - offsets_[index]};
    }

    ustore_length_t* offsets() const noexcept { return offsets_; }
    element_t* contents() const noexcept { return contents_; }
};

using joined_strs_t = joined_chunks<std::string_view>;
using joined_blobs_t = joined_chunks<value_view_t>;

/** An iterator over values inside one tape, each addressed by its own offset and length. */
template <typename chunk_>
class embedded_chunks_iterator {

    using chunk_t = chunk_;
    using element_t = typename chunk_::value_type;

    ustore_length_t* offsets_ {nullptr};
    ustore_length_t* lengths_ {nullptr};
    element_t* contents_ {nullptr};

  public:
    using iterator_category = std::random_access_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = chunk_;
    using pointer = void;
    using reference = void;

    template <typename same_size_>
    embedded_chunks_iterator(ustore_length_t* offsets, ustore_length_t* lengths, same_size_* contents) noexcept
        : offsets_(offsets), lengths_(lengths), contents_((element_t*)(contents)) {
        static_assert(sizeof(same_size_) == sizeof(element_t));
    }

    embedded_chunks_iterator& operator++() noexcept {
        ++lengths_;
        ++offsets_;
        return *this;
    }

    embedded_chunks_iterator operator++(int) const noexcept { return {offsets_ + 1, lengths_ + 1, contents_}; }
    embedded_chunks_iterator operator--(int) const noexcept { return {offsets_ - 1, lengths_ - 1, contents_}; }
    chunk_ operator*() const noexcept { return {contents_ + *offsets_, *lengths_}; }
    chunk_ operator[](std::size_t index) const noexcept { return {contents_ + offsets_[index], lengths_[index]}; }

    bool operator==(embedded_chunks_iterator const& other) const noexcept { return lengths_ == other.lengths_; }
};

using embedded_strs_iterator_t = embedded_chunks_iterator<std::string_view>;
using embedded_blobs_iterator_t = embedded_chunks_iterator<value_view_t>;

/** Values inside one tape, each addressed by its own offset and length in any order. */
template <typename chunk_>
class embedded_chunks {

    using chunk_t = chunk_;
    using element_t = typename chunk_::value_type;

    ustore_size_t count_ {0};
    ustore_length_t* offsets_ {nullptr};
    ustore_length_t* lengths_ {nullptr};
    element_t* contents_ {nullptr};

  public:
    using value_type = chunk_;

    embedded_chunks() = default;

    template <typename same_size_>
    embedded_chunks(ustore_size_t count, ustore_length_t* offsets, ustore_length_t* lengths,
                    same_size_* contents) noexcept
        : count_(count), offsets_(offsets), lengths_(lengths), contents_((element_t*)(contents)) {
        static_assert(sizeof(same_size_) == sizeof(element_t));
    }

    embedded_chunks_iterator<chunk_> begin() const noexcept { return {offsets_, lengths_, contents_}; }
    embedded_chunks_iterator<chunk_> end() const noexcept { return {offsets_ + count_, lengths_ + count_, contents_}; }
    std::size_t size() const noexcept { return count_; }
    chunk_ operator[](std::size_t index) const noexcept { return {contents_ + offsets_[index], lengths_[index]}; }

    ustore_length_t* offsets() const noexcept { return offsets_; }
    ustore_length_t* lengths() const noexcept { return lengths_; }
    element_t* contents() const noexcept { return contents_; }
};

using embedded_strs_t = embedded_chunks<std::string_view>;
using embedded_blobs_t = embedded_chunks<value_view_t>;

/** Iterates a known number of null-terminated strings joined one after another, as collection listings return. */
class strings_tape_iterator_t {
    ustore_size_t remaining_count_ {0};
    ustore_str_view_t current_ {nullptr};

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = std::string_view;
    using pointer = ustore_char_t**;
    using reference = std::string_view;

    strings_tape_iterator_t(ustore_size_t remaining = 0, ustore_str_view_t current = nullptr) noexcept
        : remaining_count_(remaining), current_(current) {}

    strings_tape_iterator_t& operator++() noexcept {
        current_ += std::strlen(current_) + 1;
        --remaining_count_;
        return *this;
    }

    strings_tape_iterator_t operator++(int) noexcept {
        return {remaining_count_ - 1, current_ + std::strlen(current_) + 1};
    }

    ustore_str_view_t operator*() const noexcept { return current_; }
    bool is_end() const noexcept { return !remaining_count_; }
    ustore_size_t size() const noexcept { return remaining_count_; }
};

#pragma endregion Tapes and Flat Arrays

#pragma region Multiple Dimensions

/** A matrix of scalars with independent byte strides between rows and between columns. */
template <typename scalar_>
class strided_matrix {
  public:
    using scalar_t = scalar_;

  private:
    scalar_* begin_ {nullptr};
    ustore_size_t bytes_between_rows_ {0};
    ustore_size_t bytes_between_columns_ {0};
    ustore_size_t rows_ {0};
    ustore_size_t columns_ {0};

  public:
    strided_matrix() = default;
    strided_matrix(scalar_* begin, std::size_t rows, std::size_t columns, std::size_t bytes_between_rows,
                   std::size_t bytes_between_columns = sizeof(scalar_)) noexcept
        : begin_(begin), bytes_between_rows_(static_cast<ustore_size_t>(bytes_between_rows)),
          bytes_between_columns_(static_cast<ustore_size_t>(bytes_between_columns)),
          rows_(static_cast<ustore_size_t>(rows)), columns_(static_cast<ustore_size_t>(columns)) {}

    std::size_t size() const noexcept { return rows_ * columns_; }
    scalar_ const& operator()(std::size_t row_index, std::size_t column_index) const noexcept {
        strided_range<scalar_ const> const cells = row(row_index);
        return cells[column_index];
    }
    strided_range<scalar_ const> column(std::size_t column_index) const noexcept {
        scalar_ const* first = begin_ + column_index * bytes_between_columns_ / sizeof(scalar_);
        return {{first, bytes_between_rows_}, rows_};
    }
    strided_range<scalar_ const> row(std::size_t row_index) const noexcept {
        scalar_ const* first = begin_ + row_index * bytes_between_rows_ / sizeof(scalar_);
        return {{first, bytes_between_columns_}, columns_};
    }
    std::size_t rows() const noexcept { return rows_; }
    std::size_t columns() const noexcept { return columns_; }
    scalar_ const* data() const noexcept { return begin_; }
};

#pragma endregion Multiple Dimensions

#pragma region Algorithms

/** Reduces the first @p count transformed elements, counting instead of comparing to an end
 *  a repeating iterator never reaches. */
template <typename accumulator_, typename iterator_, typename transform_ = std::identity>
accumulator_ transform_reduce_n(iterator_ begin, std::size_t count, accumulator_ initial, transform_ transform = {}) {
    for (std::size_t index = 0; index != count; ++index) initial += transform(begin[index]);
    return initial;
}

/** Writes the first @p count transformed elements into @p output. */
template <typename output_iterator_, typename iterator_, typename transform_ = std::identity>
void transform_n(iterator_ begin, std::size_t count, output_iterator_ output, transform_ transform = {}) {
    for (std::size_t index = 0; index != count; ++index) output[index] = transform(begin[index]);
}

/** Sums the first @p count elements onto @p initial. */
template <typename accumulator_, typename iterator_>
accumulator_ reduce_n(iterator_ begin, std::size_t count, accumulator_ initial) {
    return transform_reduce_n(begin, count, initial);
}

/** Whether the first @p count elements strictly ascend. */
template <typename iterator_>
bool all_ascending(iterator_ begin, std::size_t count) {
    for (std::size_t index = 1; index < count; ++index)
        if (!(begin[index - 1] < begin[index])) return false;
    return true;
}

/** Sorts the elements, gathers the distinct ones at the front, and returns how many there are. */
template <std::random_access_iterator iterator_>
    requires std::sortable<iterator_>
std::size_t sort_and_deduplicate(iterator_ begin, iterator_ end) {
    std::ranges::sort(begin, end);
    return static_cast<std::size_t>(std::ranges::unique(begin, end).begin() - begin);
}

/** Inserts trivially copyable elements at @p offset of a buffer with spare capacity and returns the new length. */
template <typename element_>
std::size_t trivial_insert( //
    element_* begin, std::size_t old_length, std::size_t offset, element_ const* inserted_begin,
    element_ const* inserted_end) noexcept {

    static_assert(std::is_trivially_copyable_v<element_>);
    auto const inserted_length = static_cast<std::size_t>(inserted_end - inserted_begin);
    auto const following_length = old_length - offset;
    // An empty range may hold null pointers, which `memmove` and `memcpy` must never receive, even for zero bytes
    if (following_length)
        std::memmove(begin + offset + inserted_length, begin + offset, following_length * sizeof(element_));
    if (inserted_length) std::memcpy(begin + offset, inserted_begin, inserted_length * sizeof(element_));
    return old_length + inserted_length;
}

/** Erases trivially copyable elements from a buffer and returns the new length. */
template <typename element_>
std::size_t trivial_erase(element_* begin, std::size_t old_length, std::size_t removed_offset,
                          std::size_t removed_length) noexcept {

    static_assert(std::is_trivially_copyable_v<element_>);
    auto const following_length = old_length - (removed_offset + removed_length);
    std::memmove(begin + removed_offset, begin + removed_offset + removed_length, following_length * sizeof(element_));
    return old_length - removed_length;
}

#pragma endregion Algorithms

} // namespace unum::ustore
