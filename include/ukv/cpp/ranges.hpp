/**
 * @file utility.hpp
 * @author Ashot Vardanian
 * @date 4 Jul 2022
 *
 * @brief Smart Pointers, Monads and Range-like abstractions for C++ bindings.
 */

#pragma once
#include "ukv/cpp/types.hpp" // `value_view_t`

namespace unum::ukv {

/**
 * @brief A smart pointer type with customizable jump length for increments.
 * In other words, it allows a strided data layout, common to HPC apps.
 * Cool @b hint, you can use this to represent an infinite array of repeating
 * values with `stride` equal to zero.
 */
template <typename object_at>
class strided_iterator_gt {

    object_at* raw_ = nullptr;
    ukv_size_t stride_ = 0;

    object_at* upshift(std::ptrdiff_t bytes) const noexcept { return (object_at*)((char*)raw_ + bytes); }
    object_at* downshift(std::ptrdiff_t bytes) const noexcept { return (object_at*)((char*)raw_ - bytes); }

  public:
    using iterator_category = std::random_access_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = object_at;
    using pointer = value_type*;
    using reference = value_type&;

    inline strided_iterator_gt(object_at* raw = nullptr, ukv_size_t stride = 0) noexcept : raw_(raw), stride_(stride) {}
    inline object_at& operator[](ukv_size_t idx) const noexcept { return *upshift(stride_ * idx); }

    inline strided_iterator_gt& operator++() noexcept {
        raw_ = upshift(stride_);
        return *this;
    }

    inline strided_iterator_gt& operator--() noexcept {
        raw_ = downshift(stride_);
        return *this;
    }

    inline strided_iterator_gt operator++(int) const noexcept { return {upshift(stride_), stride_}; }
    inline strided_iterator_gt operator--(int) const noexcept { return {downshift(stride_), stride_}; }
    inline strided_iterator_gt operator+(std::ptrdiff_t n) const noexcept { return {upshift(n * stride_), stride_}; }
    inline strided_iterator_gt operator-(std::ptrdiff_t n) const noexcept { return {downshift(n * stride_), stride_}; }
    inline strided_iterator_gt& operator+=(std::ptrdiff_t n) noexcept {
        raw_ = upshift(n * stride_);
        return *this;
    }
    inline strided_iterator_gt& operator-=(std::ptrdiff_t n) noexcept {
        raw_ = downshift(n * stride_);
        return *this;
    }

    /**
     * ! Calling this function with "stride" different from zero or
     * ! non-zero `sizeof(object_at)` multiple will cause Undefined
     * ! Behaviour.
     */
    inline std::ptrdiff_t operator-(strided_iterator_gt other) const noexcept {
        return stride_ ? (raw_ - other.raw_) * sizeof(object_at) / stride_ : 0;
    }

    inline operator bool() const noexcept { return raw_ != nullptr; }
    inline bool repeats() const noexcept { return !stride_; }
    inline ukv_size_t stride() const noexcept { return stride_; }
    inline object_at& operator*() const noexcept { return *raw_; }
    inline object_at* operator->() const noexcept { return raw_; }
    inline object_at* get() const noexcept { return raw_; }

    inline bool operator==(strided_iterator_gt const& other) const noexcept { return raw_ == other.raw_; }
    inline bool operator!=(strided_iterator_gt const& other) const noexcept { return raw_ != other.raw_; }

    template <typename member_at, typename parent_at = object_at>
    inline auto members(member_at parent_at::*member_ptr) const noexcept {
        using parent_t = std::conditional_t<std::is_const_v<object_at>, parent_at const, parent_at>;
        using member_t = std::conditional_t<std::is_const_v<object_at>, member_at const, member_at>;
        parent_t& first = *raw_;
        member_t& first_member = first.*member_ptr;
        return strided_iterator_gt<member_t> {&first_member, stride()};
    }
};

template <typename object_at>
class strided_range_gt {

    object_at* begin_ = nullptr;
    ukv_size_t stride_ = 0;
    ukv_size_t count_ = 0;

  public:
    using value_type = object_at;

    strided_range_gt() = default;
    strided_range_gt(object_at& single, std::size_t repeats = 1) noexcept
        : begin_(&single), stride_(0), count_(static_cast<ukv_size_t>(repeats)) {}
    strided_range_gt(object_at* begin, object_at* end) noexcept
        : begin_(begin), stride_(sizeof(object_at)), count_(end - begin) {}
    strided_range_gt(object_at* begin, std::size_t stride, std::size_t count) noexcept
        : begin_(begin), stride_(static_cast<ukv_size_t>(stride)), count_(static_cast<ukv_size_t>(count)) {}

    strided_range_gt(std::vector<object_at>& vec) noexcept
        : begin_(vec.data()), stride_(sizeof(object_at)), count_(vec.size()) {}
    strided_range_gt(std::vector<std::remove_const_t<object_at>> const& vec) noexcept
        : begin_(vec.data()), stride_(sizeof(object_at)), count_(vec.size()) {}

    template <std::size_t count_ak>
    strided_range_gt(object_at (&c_array)[count_ak]) noexcept
        : begin_(&c_array[0]), stride_(sizeof(object_at)), count_(count_ak) {}

    template <std::size_t count_ak>
    strided_range_gt(std::array<object_at, count_ak> const& array) noexcept
        : begin_(array.data()), stride_(sizeof(object_at)), count_(count_ak) {}

    strided_range_gt(strided_range_gt&&) = default;
    strided_range_gt(strided_range_gt const&) = default;
    strided_range_gt& operator=(strided_range_gt&&) = default;
    strided_range_gt& operator=(strided_range_gt const&) = default;

    inline strided_iterator_gt<object_at> begin() const noexcept { return {begin_, stride_}; }
    inline strided_iterator_gt<object_at> end() const noexcept { return begin() + static_cast<std::ptrdiff_t>(count_); }
    inline object_at& at(std::size_t i) const noexcept { return *(begin() + static_cast<std::ptrdiff_t>(i)); }
    inline object_at& operator[](std::size_t i) const noexcept { return at(i); }
    inline object_at* data() const noexcept { return begin_; }

    inline auto immutable() const noexcept { return strided_range_gt<object_at const>(begin_, stride_, count_); }
    inline strided_range_gt subspan(std::size_t offset, std::size_t count) const noexcept {
        return {begin_ + offset * stride_, stride_, count};
    }

    inline bool empty() const noexcept { return !count_; }
    inline std::size_t size() const noexcept { return count_; }
    inline ukv_size_t stride() const noexcept { return stride_; }
    inline ukv_size_t count() const noexcept { return count_; }
    inline operator bool() const noexcept { return begin_ != nullptr; }

    template <typename member_at, typename parent_at = object_at>
    inline auto members(member_at parent_at::*member_ptr) const noexcept {
        auto begin_members = begin().members(member_ptr);
        using member_t = typename decltype(begin_members)::value_type;
        return strided_range_gt<member_t> {begin_members.get(), begin_members.stride(), count()};
    }
};

/**
 * @brief Similar to `std::optional<std::span>`.
 * It's NULL state and "empty string" states are not identical.
 * The NULL state generally reflects missing values.
 */
template <typename pointer_at>
struct indexed_range_gt {
    pointer_at begin_ = nullptr;
    pointer_at end_ = nullptr;

    inline pointer_at begin() const noexcept { return begin_; }
    inline pointer_at end() const noexcept { return end_; }
    inline decltype(auto) operator[](std::size_t i) const noexcept { return begin_[i]; }
    inline decltype(auto) at(std::size_t i) const noexcept { return begin_[i]; }

    inline std::size_t size() const noexcept { return end_ - begin_; }
    inline bool empty() const noexcept { return end_ == begin_; }
    inline operator bool() const noexcept { return end_ != begin_; }
    inline auto strided() const noexcept {
        using element_t = std::remove_pointer_t<pointer_at>;
        using strided_t = strided_range_gt<element_t>;
        return strided_t {begin_, sizeof(element_t), static_cast<ukv_size_t>(size())};
    }
};

template <typename pointer_at>
struct range_gt {
    pointer_at begin_ = nullptr;
    pointer_at end_ = nullptr;

    inline pointer_at const& begin() const& noexcept { return begin_; }
    inline pointer_at const& end() const& noexcept { return end_; }
    inline pointer_at&& begin() && noexcept { return std::move(begin_); }
    inline pointer_at&& end() && noexcept { return std::move(end_); }
};

#pragma region - Tapes

/**
 * @brief A read-only iterator for values packed into a
 * contiguous memory range. Doesn't own underlying memory.
 */
class tape_iterator_t {

    ukv_val_len_t const* lengths_ = nullptr;
    ukv_val_ptr_t contents_ = nullptr;

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = value_view_t;
    using pointer = void;
    using reference = void;

    inline tape_iterator_t(ukv_val_ptr_t ptr, ukv_size_t elements) noexcept
        : lengths_(reinterpret_cast<ukv_val_len_t*>(ptr)), contents_(ptr + sizeof(ukv_val_len_t) * elements) {}

    inline tape_iterator_t(ukv_val_len_t const* lens, ukv_val_ptr_t vals) noexcept : lengths_(lens), contents_(vals) {}

    inline tape_iterator_t& operator++() noexcept {
        if (*lengths_ != ukv_val_len_missing_k)
            contents_ += *lengths_;
        ++lengths_;
        return *this;
    }

    inline tape_iterator_t operator++(int) const noexcept {
        return {lengths_ + 1, *lengths_ != ukv_val_len_missing_k ? contents_ + *lengths_ : contents_};
    }

    inline value_view_t operator*() const noexcept { return {contents_, *lengths_}; }

    inline bool operator==(tape_iterator_t const& other) const noexcept { return lengths_ == other.lengths_; }
    inline bool operator!=(tape_iterator_t const& other) const noexcept { return lengths_ != other.lengths_; }
};

class taped_values_view_t {
    ukv_val_len_t* lengths_ = nullptr;
    ukv_val_ptr_t contents_ = nullptr;
    ukv_size_t count_ = 0;

  public:
    inline taped_values_view_t() = default;
    inline taped_values_view_t(ukv_val_len_t* lens, ukv_val_ptr_t vals, ukv_size_t elements) noexcept
        : lengths_(lens), contents_(vals), count_(elements) {}

    inline tape_iterator_t begin() const noexcept { return {lengths_, contents_}; }
    inline tape_iterator_t end() const noexcept { return {lengths_ + count_, contents_}; }
    inline std::size_t size() const noexcept { return count_; }

    ukv_val_len_t* lengths() const noexcept { return lengths_; }
    ukv_val_ptr_t contents() const noexcept { return contents_; }
};

#pragma region - Multiple Dimensions

template <typename object_at>
class strided_matrix_gt {

    object_at* begin_ = nullptr;
    ukv_size_t stride_ = 0;
    ukv_size_t rows_ = 0;
    ukv_size_t cols_ = 0;

  public:
    strided_matrix_gt() = default;
    strided_matrix_gt(object_at* begin, std::size_t rows, std::size_t cols, std::size_t stride) noexcept
        : begin_(begin), stride_(static_cast<ukv_size_t>(stride)), rows_(static_cast<ukv_size_t>(rows)),
          cols_(static_cast<ukv_size_t>(cols)) {}

    strided_matrix_gt(strided_matrix_gt&&) = default;
    strided_matrix_gt(strided_matrix_gt const&) = default;
    strided_matrix_gt& operator=(strided_matrix_gt&&) = default;
    strided_matrix_gt& operator=(strided_matrix_gt const&) = default;

    inline std::size_t size() const noexcept { return rows_ * cols_; }
    inline object_at& operator()(std::size_t i, std::size_t j) noexcept { return row(i)[j]; }
    inline object_at const& operator()(std::size_t i, std::size_t j) const noexcept { return row(i)[j]; }
    inline strided_range_gt<object_at> col(std::size_t j) const noexcept { return {begin_ + j, stride_, rows_}; }
    inline indexed_range_gt<object_at> row(std::size_t i) const noexcept {
        return {begin_ + i * stride_ / sizeof(object_at), cols_};
    }
    inline std::size_t rows() const noexcept { return rows_; }
    inline std::size_t cols() const noexcept { return cols_; }
};

#pragma region - Algorithms

/**
 * @brief Unlike the `std::accumulate` and `std::transform_reduce` takes an integer `n`
 * instead of the end iterator. This helps with zero-strided iterators.
 */
template <typename element_at, typename transform_at, typename iterator_at>
element_at transform_reduce_n(iterator_at begin, std::size_t n, element_at init, transform_at transform) {
    for (std::size_t i = 0; i != n; ++i, ++begin)
        init += transform(*begin);
    return init;
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

#pragma region - Aliases and Packs

using keys_view_t = strided_range_gt<ukv_key_t const>;
using fields_view_t = strided_range_gt<ukv_str_view_t const>;

/**
 * Working with batched data is ugly in C++.
 * This handle doesn't help in the general case,
 * but at least allow reusing the arguments.
 */
struct keys_arg_t {
    using value_type = col_key_field_t;
    strided_iterator_gt<ukv_collection_t const> collections_begin;
    strided_iterator_gt<ukv_key_t const> keys_begin;
    strided_iterator_gt<ukv_str_view_t const> fields_begin;
    ukv_size_t count = 0;
};

/**
 * Working with batched data is ugly in C++.
 * This handle doesn't help in the general case,
 * but at least allow reusing the arguments.
 */
struct values_arg_t {
    using value_type = value_view_t;
    strided_iterator_gt<ukv_val_ptr_t const> contents_begin;
    strided_iterator_gt<ukv_val_len_t const> offsets_begin;
    strided_iterator_gt<ukv_val_len_t const> lengths_begin;
};

template <typename id_at>
struct edges_range_gt {

    using id_t = id_at;
    using tuple_t = std::conditional_t<std::is_const_v<id_t>, edge_t const, edge_t>;
    static_assert(sizeof(tuple_t) == 3 * sizeof(id_t));

    strided_range_gt<id_t> source_ids;
    strided_range_gt<id_t> target_ids;
    strided_range_gt<id_t> edge_ids;

    inline edges_range_gt() = default;
    inline edges_range_gt(strided_range_gt<id_t> sources,
                          strided_range_gt<id_t> targets,
                          strided_range_gt<id_t> edges = {ukv_default_edge_id_k}) noexcept
        : source_ids(sources), target_ids(targets), edge_ids(edges) {}

    inline edges_range_gt(tuple_t* ptr, tuple_t* end) noexcept {
        auto strided = strided_range_gt<tuple_t>(ptr, end);
        source_ids = strided.members(&edge_t::source_id);
        target_ids = strided.members(&edge_t::target_id);
        edge_ids = strided.members(&edge_t::id);
    }

    inline edges_range_gt(std::vector<edge_t> const& edges) noexcept
        : edges_range_gt(edges.data(), edges.data() + edges.size()) {}

    inline std::size_t size() const noexcept { return std::min(source_ids.count(), target_ids.count()); }

    inline edge_t operator[](std::size_t i) const noexcept {
        edge_t result;
        result.source_id = source_ids[i];
        result.target_id = target_ids[i];
        result.id = edge_ids[i];
        return result;
    }
};

using edges_span_t = edges_range_gt<ukv_key_t>;
using edges_view_t = edges_range_gt<ukv_key_t const>;

} // namespace unum::ukv
