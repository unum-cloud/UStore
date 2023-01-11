/**
 * @file ranges.hpp
 * @author Ashot Vardanian
 * @date 4 Jul 2022
 * @addtogroup Cpp
 *
 * @brief Smart Pointers, Monads and Range-like templates for C++ bindings.
 *
 * - "Strided": defines the number of bytes to jump until next entry, instead of `sizeof`.
 * - "Joined": Indexes variable-length objects using just base pointer and N+1 offsets,
 *   assuming the next entry starts right after the previous one without gaps.
 * - "Embedded": Extends "Joined" ranges to objects with lengths.
 *   In that case order of elements is irrelevant and we need just N offsets & lengths.
 */

#pragma once
#include <limits.h>  // `CHAR_BIT`
#include <algorithm> // `std::sort`

#include "ukv/cpp/types.hpp" // `value_view_t`

namespace unum::ukv {

/**
 * @brief A smart pointer type with customizable jump length for increments.
 * In other words, it allows a strided data layout, common to HPC apps.
 * Cool @b hint, you can use this to represent an infinite array of repeating
 * values with `stride` equal to zero.
 */
template <typename element_at>
class strided_iterator_gt {
  public:
    using element_t = element_at;
    using iterator_category = std::random_access_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = element_t;
    using pointer = value_type*;
    using reference = value_type&;

  protected:
    element_t* raw_ {nullptr};
    ukv_size_t stride_ {0};

    element_t* upshift(std::ptrdiff_t bytes) const noexcept { return (element_t*)((char*)raw_ + bytes); }
    element_t* downshift(std::ptrdiff_t bytes) const noexcept { return (element_t*)((char*)raw_ - bytes); }

  public:
    strided_iterator_gt(element_t* raw = nullptr, ukv_size_t stride = 0) noexcept : raw_(raw), stride_(stride) {}
    strided_iterator_gt(strided_iterator_gt&&) noexcept = default;
    strided_iterator_gt(strided_iterator_gt const&) noexcept = default;
    strided_iterator_gt& operator=(strided_iterator_gt&&) noexcept = default;
    strided_iterator_gt& operator=(strided_iterator_gt const&) noexcept = default;

    element_t& operator[](ukv_size_t idx) const noexcept { return *upshift(stride_ * idx); }

    strided_iterator_gt& operator++() noexcept {
        raw_ = upshift(stride_);
        return *this;
    }

    strided_iterator_gt& operator--() noexcept {
        raw_ = downshift(stride_);
        return *this;
    }

    strided_iterator_gt operator++(int) const noexcept { return {upshift(stride_), stride_}; }
    strided_iterator_gt operator--(int) const noexcept { return {downshift(stride_), stride_}; }
    strided_iterator_gt operator+(std::ptrdiff_t n) const noexcept { return {upshift(n * stride_), stride_}; }
    strided_iterator_gt operator-(std::ptrdiff_t n) const noexcept { return {downshift(n * stride_), stride_}; }
    strided_iterator_gt& operator+=(std::ptrdiff_t n) noexcept {
        raw_ = upshift(n * stride_);
        return *this;
    }
    strided_iterator_gt& operator-=(std::ptrdiff_t n) noexcept {
        raw_ = downshift(n * stride_);
        return *this;
    }

    /**
     * ! Calling this function with "stride" different from zero or
     * ! non-zero `sizeof(element_t)` multiple will cause Undefined
     * ! behavior.
     */
    std::ptrdiff_t operator-(strided_iterator_gt other) const noexcept {
        return stride_ ? (raw_ - other.raw_) * sizeof(element_t) / stride_ : 0;
    }

    explicit operator bool() const noexcept { return raw_ != nullptr; }
    bool repeats() const noexcept { return !stride_; }
    bool is_continuous() const noexcept { return stride_ == sizeof(element_t); }
    ukv_size_t stride() const noexcept { return stride_; }
    element_t& operator*() const noexcept { return *raw_; }
    element_t* operator->() const noexcept { return raw_; }
    element_t* get() const noexcept { return raw_; }

    bool operator==(strided_iterator_gt const& other) const noexcept { return raw_ == other.raw_; }
    bool operator!=(strided_iterator_gt const& other) const noexcept { return raw_ != other.raw_; }

    template <typename member_at, typename parent_at = element_t>
    auto members(member_at parent_at::*member_ptr) const noexcept {
        using parent_t = std::conditional_t<std::is_const_v<element_t>, parent_at const, parent_at>;
        using member_t = std::conditional_t<std::is_const_v<element_t>, member_at const, member_at>;
        parent_t& first = *raw_;
        member_t& first_member = first.*member_ptr;
        return strided_iterator_gt<member_t> {&first_member, stride()};
    }
};

class bits_span_t {
  public:
    struct ref_t {
        ukv_octet_t* raw = nullptr;
        ukv_octet_t mask = 0;
        ref_t(ukv_octet_t& raw) noexcept : raw(&raw), mask(0) {}
        ref_t(ukv_octet_t* raw, ukv_octet_t mask) noexcept : raw(raw), mask(mask) {}
        operator bool() const noexcept { return *raw & mask; }
        ref_t& operator=(bool value) noexcept {
            *raw = value ? (*raw | mask) : (*raw & ~mask);
            return *this;
        }
    };

    using element_t = ukv_octet_t;
    using value_type = ukv_octet_t;
    using reference = ref_t;

  protected:
    element_t* begin_ {nullptr};

  public:
    bits_span_t(element_t* begin = nullptr) noexcept : begin_(begin) {}
    bits_span_t(bits_span_t&&) noexcept = default;
    bits_span_t(bits_span_t const&) noexcept = default;
    bits_span_t& operator=(bits_span_t&&) noexcept = default;
    bits_span_t& operator=(bits_span_t const&) noexcept = default;

    ref_t at(std::size_t idx) const noexcept {
        return {begin_ + idx / CHAR_BIT, static_cast<element_t>(1 << (idx % CHAR_BIT))};
    }
    ref_t operator[](std::size_t idx) const noexcept { return at(idx); }

    explicit operator bool() const noexcept { return begin_ != nullptr; }
    element_t* get() const noexcept { return begin_; }
    bool operator==(bits_span_t const& other) const noexcept { return begin_ == other.begin_; }
    bool operator!=(bits_span_t const& other) const noexcept { return begin_ != other.begin_; }
};

class bits_view_t {
  public:
    using element_t = ukv_octet_t const;
    using value_type = bool;
    using reference = void;

  protected:
    element_t* begin_ {nullptr};

  public:
    bits_view_t(element_t* begin = nullptr) noexcept : begin_(begin) {}
    bits_view_t(bits_view_t&&) noexcept = default;
    bits_view_t(bits_view_t const&) noexcept = default;
    bits_view_t& operator=(bits_view_t&&) noexcept = default;
    bits_view_t& operator=(bits_view_t const&) noexcept = default;

    bool at(std::size_t idx) const noexcept {
        return begin_[idx / CHAR_BIT] & static_cast<element_t>(1 << (idx % CHAR_BIT));
    }
    bool operator[](std::size_t idx) const noexcept { return at(idx); }

    explicit operator bool() const noexcept { return begin_ != nullptr; }
    element_t* get() const noexcept { return begin_; }
    bool operator==(bits_view_t const& other) const noexcept { return begin_ == other.begin_; }
    bool operator!=(bits_view_t const& other) const noexcept { return begin_ != other.begin_; }
};

template <typename element_at>
class strided_range_gt {
  public:
    using element_t = element_at;
    using iterator_t = strided_iterator_gt<element_t>;
    using value_type = typename iterator_t::value_type;
    using reference = typename iterator_t::reference;

  protected:
    static_assert(!std::is_void_v<element_t>);

    iterator_t begin_ {nullptr};
    ukv_size_t count_ {0};

  public:
    strided_range_gt() = default;
    strided_range_gt(iterator_t begin, std::size_t count) noexcept : begin_(begin), count_(count) {}

    strided_range_gt(strided_range_gt&&) = default;
    strided_range_gt(strided_range_gt const&) = default;
    strided_range_gt& operator=(strided_range_gt&&) = default;
    strided_range_gt& operator=(strided_range_gt const&) = default;

    inline element_t* data() const noexcept { return begin_.get(); }
    inline decltype(auto) begin() const noexcept { return begin_; }
    inline decltype(auto) end() const noexcept { return begin() + static_cast<std::ptrdiff_t>(count_); }
    inline decltype(auto) at(std::size_t i) const noexcept { return begin()[static_cast<std::ptrdiff_t>(i)]; }
    inline decltype(auto) operator[](std::size_t i) const noexcept { return at(i); }

    inline auto immutable() const noexcept {
        return strided_range_gt<element_t const>({begin_.get(), begin_.stride()}, count_);
    }
    inline strided_range_gt subspan(std::size_t offset, std::size_t count) const noexcept {
        return {begin_ + offset, count};
    }

    inline bool empty() const noexcept { return !count_; }
    inline std::size_t size() const noexcept { return count_; }
    inline ukv_size_t stride() const noexcept { return begin_.stride(); }
    inline ukv_size_t count() const noexcept { return count_; }
    inline explicit operator bool() const noexcept { return begin_ != nullptr; }

    template <typename member_at, typename parent_at = element_t>
    inline auto members(member_at parent_at::*member_ptr) const noexcept {
        auto begin_members = begin().members(member_ptr);
        using member_t = typename decltype(begin_members)::value_type;
        return strided_range_gt<member_t> {{begin_members.get(), begin_members.stride()}, count()};
    }

    inline bool same_elements() const noexcept {
        return !begin_ || begin_.repeats() ||
               !transform_reduce_n(begin_, count_, false, [=](ukv_collection_t collection) {
                   return collection != begin_[0];
               });
    }
};

template <typename at, typename alloc_at = std::allocator<at>>
strided_range_gt<at> strided_range(std::vector<at, alloc_at>& vec) noexcept {
    return {{vec.data(), sizeof(at)}, vec.size()};
}

template <typename at, typename alloc_at = std::allocator<at>>
strided_range_gt<at const> strided_range(std::vector<at, alloc_at> const& vec) noexcept {
    return {{vec.data(), sizeof(at)}, vec.size()};
}

template <typename at, std::size_t count_ak>
strided_range_gt<at> strided_range(at (&c_array)[count_ak]) noexcept {
    return {{&c_array[0], sizeof(at)}, count_ak};
}

template <typename at, std::size_t count_ak>
strided_range_gt<at const> strided_range(std::array<at, count_ak> const& array) noexcept {
    return {{array.data(), sizeof(at)}, count_ak};
}

template <typename at>
strided_range_gt<at const> strided_range(std::initializer_list<at> list) noexcept {
    return {{list.begin(), sizeof(at)}, list.size()};
}

template <typename at>
strided_range_gt<at> strided_range(at* begin, at* end) noexcept {
    return {{begin, sizeof(at)}, static_cast<std::size_t>(end - begin)};
}

/**
 * @brief Similar to `std::optional<std::span>`.
 * It's NULL state and "empty string" states are not identical.
 * The NULL state generally reflects missing values.
 */
template <typename pointer_at>
struct indexed_range_gt {
    using pointer_t = pointer_at;
    using element_t = std::remove_pointer_t<pointer_t>;
    using strided_t = strided_range_gt<element_t>;
    using value_type = element_t;
    using reference = element_t&;

    pointer_t begin_ {nullptr};
    pointer_t end_ {nullptr};

    indexed_range_gt() noexcept : begin_(nullptr), end_(nullptr) {}
    indexed_range_gt(pointer_t begin, pointer_t end) noexcept : begin_(begin), end_(end) {}
    indexed_range_gt(pointer_t ptr, std::size_t sz) noexcept : begin_(ptr), end_(ptr + sz) {}

    pointer_t begin() const noexcept { return begin_; }
    pointer_t end() const noexcept { return end_; }
    decltype(auto) operator[](std::size_t i) noexcept { return begin_[i]; }
    decltype(auto) at(std::size_t i) noexcept { return begin_[i]; }
    decltype(auto) operator[](std::size_t i) const noexcept { return begin_[i]; }
    decltype(auto) at(std::size_t i) const noexcept { return begin_[i]; }

    std::size_t size() const noexcept { return end_ - begin_; }
    std::size_t size_bytes() const noexcept { return size() * sizeof(begin_[0]); }
    bool empty() const noexcept { return end_ == begin_; }
    explicit operator bool() const noexcept { return end_ != begin_; }
    operator strided_t() const noexcept { return strided(); }
    strided_t strided() const noexcept {
        return strided_t {{begin_, sizeof(element_t)}, static_cast<ukv_size_t>(size())};
    }

    template <typename other_at>
    indexed_range_gt<other_at> cast() const noexcept {
        return {reinterpret_cast<other_at*>(begin_), size_bytes() / sizeof(other_at)};
    }

    indexed_range_gt<byte_t const> span_bytes() const noexcept {
        return {reinterpret_cast<byte_t const*>(begin_), size_bytes()};
    }
};

template <typename element_at>
using ptr_range_gt = indexed_range_gt<element_at*>;

template <typename pointer_at>
struct range_gt {
    pointer_at begin_ {nullptr};
    pointer_at end_ {nullptr};

    inline pointer_at const& begin() const& noexcept { return begin_; }
    inline pointer_at const& end() const& noexcept { return end_; }
    inline pointer_at&& begin() && noexcept { return std::move(begin_); }
    inline pointer_at&& end() && noexcept { return std::move(end_); }
};

#pragma region Tapes and Flat Arrays

/**
 * @brief A read-only iterator for values packed into a
 * contiguous memory range. Doesn't own underlying memory.
 * Only needs element "lengths", but can only be forward-iterated.
 */
template <typename chunk_at>
class consecutive_chunks_iterator_gt {

    using chunk_t = chunk_at;
    using element_t = typename chunk_t::value_type;

    ukv_length_t const* lengths_ {nullptr};
    element_t* contents_ {nullptr};

  public:
    using iterator_category = std::random_access_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = chunk_t;
    using pointer = void;
    using reference = void;

    consecutive_chunks_iterator_gt() = default;

    template <typename same_size_at>
    consecutive_chunks_iterator_gt(ukv_length_t const* lens, same_size_at* vals) noexcept
        : lengths_(lens), contents_((element_t*)(vals)) {
        static_assert(sizeof(same_size_at) == sizeof(element_t));
    }

    consecutive_chunks_iterator_gt& operator++() noexcept {
        contents_ += *lengths_;
        ++lengths_;
        return *this;
    }

    consecutive_chunks_iterator_gt operator++(int) const noexcept { return {lengths_ + 1, contents_ + *lengths_}; }
    chunk_t operator*() const noexcept { return {contents_, *lengths_}; }
    chunk_t operator[](std::size_t i) const noexcept { return {contents_, *lengths_}; }

    bool operator==(consecutive_chunks_iterator_gt const& other) const noexcept { return lengths_ == other.lengths_; }
    bool operator!=(consecutive_chunks_iterator_gt const& other) const noexcept { return lengths_ != other.lengths_; }
};

using consecutive_strs_iterator_t = consecutive_chunks_iterator_gt<std::string_view>;
using consecutive_blobs_iterator_t = consecutive_chunks_iterator_gt<value_view_t>;

/**
 * @brief A read-only iterator for values packed into a
 * contiguous memory range. Doesn't own underlying memory.
 * Relies on "offsets" to be in an Arrow-compatible form.
 */
template <typename chunk_at>
class joined_chunks_iterator_gt {

    using chunk_t = chunk_at;
    using element_t = typename chunk_t::value_type;

    ukv_length_t* offsets_ {nullptr};
    element_t* contents_ {nullptr};

  public:
    using iterator_category = std::random_access_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = chunk_t;
    using pointer = void;
    using reference = void;

    joined_chunks_iterator_gt() = default;

    template <typename same_size_at>
    joined_chunks_iterator_gt(ukv_length_t* offs, same_size_at* vals) noexcept
        : offsets_(offs), contents_((element_t*)(vals)) {
        static_assert(sizeof(same_size_at) == sizeof(element_t));
    }

    joined_chunks_iterator_gt& operator++() noexcept {
        ++offsets_;
        return *this;
    }

    joined_chunks_iterator_gt operator++(int) const noexcept { return {offsets_ + 1, contents_}; }
    joined_chunks_iterator_gt operator--(int) const noexcept { return {offsets_ - 1, contents_}; }
    chunk_t operator*() const noexcept { return {contents_ + offsets_[0], offsets_[1] - offsets_[0]}; }
    chunk_t operator[](std::size_t i) const noexcept {
        return {contents_ + offsets_[i], offsets_[i + 1] - offsets_[i]};
    }

    bool operator==(joined_chunks_iterator_gt const& other) const noexcept { return offsets_ == other.offsets_; }
    bool operator!=(joined_chunks_iterator_gt const& other) const noexcept { return offsets_ != other.offsets_; }
};

using joined_strs_iterator_t = joined_chunks_iterator_gt<std::string_view>;
using joined_blobs_iterator_t = joined_chunks_iterator_gt<value_view_t>;

template <typename chunk_at>
class joined_chunks_gt {

    using chunk_t = chunk_at;
    using element_t = typename chunk_t::value_type;

    ukv_size_t count_ {0};
    ukv_length_t* offsets_ {nullptr};
    element_t* contents_ {nullptr};

  public:
    using value_type = chunk_t;

    joined_chunks_gt() = default;

    template <typename same_size_at>
    joined_chunks_gt(ukv_size_t elements, ukv_length_t* offs, same_size_at* vals) noexcept
        : count_(elements), offsets_(offs), contents_((element_t*)(vals)) {
        static_assert(sizeof(same_size_at) == sizeof(element_t));
    }

    joined_chunks_iterator_gt<chunk_at> begin() const noexcept { return {offsets_, contents_}; }
    joined_chunks_iterator_gt<chunk_at> end() const noexcept { return {offsets_ + count_, contents_}; }
    std::size_t size() const noexcept { return count_; }
    chunk_t operator[](std::size_t i) const noexcept {
        return {contents_ + offsets_[i], offsets_[i + 1] - offsets_[i]};
    }

    ukv_length_t* offsets() const noexcept { return offsets_; }
    element_t* contents() const noexcept { return contents_; }
};

using joined_strs_t = joined_chunks_gt<std::string_view>;
using joined_blobs_t = joined_chunks_gt<value_view_t>;

/**
 * @brief A read-only iterator for values packed into a
 * contiguous memory range. Doesn't own underlying memory.
 */
template <typename chunk_at>
class embedded_chunks_iterator_gt {

    using chunk_t = chunk_at;
    using element_t = typename chunk_t::value_type;

    ukv_length_t* offsets_ {nullptr};
    ukv_length_t* lengths_ {nullptr};
    element_t* contents_ {nullptr};

  public:
    using iterator_category = std::random_access_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = chunk_t;
    using pointer = void;
    using reference = void;

    template <typename same_size_at>
    embedded_chunks_iterator_gt(ukv_length_t* offs, ukv_length_t* lens, same_size_at* vals) noexcept
        : offsets_(offs), lengths_(lens), contents_((element_t*)(vals)) {
        static_assert(sizeof(same_size_at) == sizeof(element_t));
    }

    embedded_chunks_iterator_gt& operator++() noexcept {
        ++lengths_;
        ++offsets_;
        return *this;
    }

    embedded_chunks_iterator_gt operator++(int) const noexcept { return {lengths_ + 1, offsets_ + 1, contents_}; }
    embedded_chunks_iterator_gt operator--(int) const noexcept { return {lengths_ - 1, offsets_ - 1, contents_}; }
    chunk_t operator*() const noexcept { return {contents_ + *offsets_, *lengths_}; }
    chunk_t operator[](std::size_t i) const noexcept { return {contents_ + offsets_[i], lengths_[i]}; }

    bool operator==(embedded_chunks_iterator_gt const& other) const noexcept { return lengths_ == other.lengths_; }
    bool operator!=(embedded_chunks_iterator_gt const& other) const noexcept { return lengths_ != other.lengths_; }
};

using embedded_strs_iterator_t = embedded_chunks_iterator_gt<std::string_view>;
using embedded_blobs_iterator_t = embedded_chunks_iterator_gt<value_view_t>;

template <typename chunk_at>
class embedded_chunks_gt {

    using chunk_t = chunk_at;
    using element_t = typename chunk_t::value_type;

    ukv_size_t count_ {0};
    ukv_length_t* offsets_ {nullptr};
    ukv_length_t* lengths_ {nullptr};
    element_t* contents_ {nullptr};

  public:
    using value_type = chunk_t;

    embedded_chunks_gt() = default;

    template <typename same_size_at>
    embedded_chunks_gt(ukv_size_t elements, ukv_length_t* offs, ukv_length_t* lens, same_size_at* vals) noexcept
        : count_(elements), offsets_(offs), lengths_(lens), contents_((element_t*)(vals)) {
        static_assert(sizeof(same_size_at) == sizeof(element_t));
    }

    embedded_chunks_iterator_gt<chunk_at> begin() const noexcept { return {offsets_, lengths_, contents_}; }
    embedded_chunks_iterator_gt<chunk_at> end() const noexcept {
        return {offsets_ + count_, lengths_ + count_, contents_};
    }
    std::size_t size() const noexcept { return count_; }
    chunk_t operator[](std::size_t i) const noexcept { return {contents_ + offsets_[i], lengths_[i]}; }

    ukv_length_t* offsets() const noexcept { return offsets_; }
    ukv_length_t* lengths() const noexcept { return lengths_; }
    element_t* contents() const noexcept { return contents_; }
};

using embedded_strs_t = embedded_chunks_gt<std::string_view>;
using embedded_blobs_t = embedded_chunks_gt<value_view_t>;

/**
 * @brief Iterates through a predetermined number of NULL-delimited
 * strings joined one after another in continuous memory.
 * Can be used for `ukv_docs_gist()` or `ukv_collection_list()`.
 */
class strings_tape_iterator_t {
    ukv_size_t remaining_count_ {0};
    ukv_str_view_t current_ {nullptr};

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = std::string_view;
    using pointer = ukv_char_t**;
    using reference = std::string_view;

    strings_tape_iterator_t(ukv_size_t remaining = 0, ukv_str_view_t current = nullptr)
        : remaining_count_(remaining), current_(current) {}

    strings_tape_iterator_t(strings_tape_iterator_t&&) = default;
    strings_tape_iterator_t& operator=(strings_tape_iterator_t&&) = default;

    strings_tape_iterator_t(strings_tape_iterator_t const&) = default;
    strings_tape_iterator_t& operator=(strings_tape_iterator_t const&) = default;

    strings_tape_iterator_t& operator++() noexcept {
        current_ += std::strlen(current_) + 1;
        --remaining_count_;
        return *this;
    }

    strings_tape_iterator_t operator++(int) noexcept {
        return {remaining_count_ - 1, current_ + std::strlen(current_) + 1};
    }

    ukv_str_view_t operator*() const noexcept { return current_; }
    bool is_end() const noexcept { return !remaining_count_; }
    ukv_size_t size() const noexcept { return remaining_count_; }
};

#pragma region Multiple Dimensions

template <typename scalar_at>
class strided_matrix_gt {
  public:
    using scalar_t = scalar_at;
    static_assert(!std::is_void_v<scalar_t>);

  private:
    scalar_t* begin_ {nullptr};
    ukv_size_t bytes_between_rows_ {0};
    ukv_size_t bytes_between_columns_ {0};
    ukv_size_t rows_ {0};
    ukv_size_t columns_ {0};

  public:
    strided_matrix_gt() = default;
    strided_matrix_gt(scalar_t* begin,
                      std::size_t rows,
                      std::size_t columns,
                      std::size_t bytes_between_rows,
                      std::size_t column_stride = sizeof(scalar_t)) noexcept
        : begin_(begin), bytes_between_rows_(static_cast<ukv_size_t>(bytes_between_rows)),
          bytes_between_columns_(static_cast<ukv_size_t>(column_stride)), rows_(static_cast<ukv_size_t>(rows)),
          columns_(static_cast<ukv_size_t>(columns)) {}

    strided_matrix_gt(strided_matrix_gt&&) = default;
    strided_matrix_gt(strided_matrix_gt const&) = default;
    strided_matrix_gt& operator=(strided_matrix_gt&&) = default;
    strided_matrix_gt& operator=(strided_matrix_gt const&) = default;

    inline std::size_t size() const noexcept { return rows_ * columns_; }
    inline decltype(auto) operator()(std::size_t i, std::size_t j) noexcept { return row(i)[j]; }
    inline decltype(auto) operator()(std::size_t i, std::size_t j) const noexcept { return row(i)[j]; }
    inline strided_range_gt<scalar_t const> column(std::size_t j) const noexcept {
        auto begin = begin_ + j * bytes_between_columns_ / sizeof(scalar_t);
        return {{begin, bytes_between_rows_}, rows_};
    }
    inline strided_range_gt<scalar_t const*> row(std::size_t i) const noexcept {
        auto begin = begin_ + i * bytes_between_rows_ / sizeof(scalar_t);
        return {{begin, bytes_between_columns_}, columns_};
    }
    inline std::size_t rows() const noexcept { return rows_; }
    inline std::size_t columns() const noexcept { return columns_; }
    inline scalar_t const* data() const noexcept { return begin_; }
};

#pragma region Algorithms

struct identity_t {
    template <typename at>
    at operator()(at x) const noexcept {
        return x;
    }
};

/**
 * @brief Unlike the `std::accumulate` and `std::transform_reduce` takes an integer `n`
 * instead of the end iterator. This helps with zero-strided iterators.
 */
template <typename element_at, typename iterator_at, typename transform_at = identity_t>
element_at transform_reduce_n(iterator_at begin, std::size_t n, element_at init, transform_at transform = {}) {
    for (std::size_t i = 0; i != n; ++i)
        init += transform(begin[i]);
    return init;
}

template <typename output_iterator_at, typename iterator_at, typename transform_at = identity_t>
void transform_n(iterator_at begin, std::size_t n, output_iterator_at output, transform_at transform = {}) {
    for (std::size_t i = 0; i != n; ++i)
        output[i] = transform(begin[i]);
}

template <typename element_at, typename iterator_at>
element_at reduce_n(iterator_at begin, std::size_t n, element_at init) {
    return transform_reduce_n(begin, n, init, [](auto x) { return x; });
}

template <typename iterator_at>
bool all_ascending(iterator_at begin, std::size_t n) {
    auto previous = begin;
    ++begin;
    for (std::size_t i = 1; i != n; ++i, ++begin)
        if (*begin <= *std::exchange(previous, begin))
            return false;
    return true;
}

template <typename iterator_at>
std::size_t sort_and_deduplicate(iterator_at begin, iterator_at end) {
    std::sort(begin, end);
    return std::unique(begin, end) - begin;
}

template <typename at>
std::size_t trivial_insert( //
    at* begin,
    std::size_t old_length,
    std::size_t offset,
    at const* inserted_begin,
    at const* inserted_end) {

    auto inserted_len = static_cast<std::size_t>(inserted_end - inserted_begin);
    auto following_len = static_cast<std::size_t>(old_length - offset);
    auto new_size = old_length + inserted_len;

    static_assert(std::is_trivially_copy_constructible<at>());
    std::memmove(begin + offset + inserted_len, begin + offset, following_len * sizeof(at));
    std::memcpy(begin + offset, inserted_begin, inserted_len * sizeof(at));
    return new_size;
}

template <typename at>
std::size_t trivial_erase(at* begin, std::size_t old_length, std::size_t removed_offset, std::size_t removed_length) {

    auto following_len = static_cast<std::size_t>(old_length - (removed_offset + removed_length));
    auto new_size = old_length - removed_length;

    static_assert(std::is_trivially_copy_constructible<at>());
    std::memmove(begin + removed_offset, begin + removed_offset + removed_length, following_len * sizeof(at));
    return new_size;
}

} // namespace unum::ukv
