/**
 *  @file src/nvme/indexed_iterator.hpp
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief A random-access iterator over parallel arrays, materializing one element per index.
 */

#pragma once

#include <cstddef> // `std::ptrdiff_t`, `std::size_t`

#include <compare>  // `std::strong_ordering`
#include <iterator> // `std::random_access_iterator_tag`
#include <ranges>   // `std::ranges::subrange`

namespace unum::ustore::nvme {

/**
 *  @brief Walks a container that answers an index, handing out the element it builds for that index.
 *
 *  Parallel arrays hold no element to point at, so the iterator returns the container's own value
 *  rather than a reference into it, and the container must outlive every iterator over it.
 */
template <typename container_type_>
class indexed_iterator {
  public:
    using container_t = container_type_;
    using iterator_category = std::random_access_iterator_tag;
    using value_type = typename container_t::value_type;
    using difference_type = std::ptrdiff_t;
    using pointer = value_type*;
    using reference = value_type;

    indexed_iterator() = default;

    /** An iterator over @p container, positioned at @p offset. */
    indexed_iterator(container_t container, std::size_t offset) noexcept : container_(container), offset_(offset) {}

    /** The element the container builds for the current index. */
    value_type operator*() const { return container_[offset_]; }

    /** The element the container builds for the index @p delta places ahead. */
    value_type operator[](difference_type delta) const { return container_[offset_ + static_cast<std::size_t>(delta)]; }

    indexed_iterator& operator++() noexcept {
        ++offset_;
        return *this;
    }

    indexed_iterator operator++(int) noexcept {
        indexed_iterator copy = *this;
        ++offset_;
        return copy;
    }

    indexed_iterator& operator--() noexcept {
        --offset_;
        return *this;
    }

    indexed_iterator operator--(int) noexcept {
        indexed_iterator copy = *this;
        --offset_;
        return copy;
    }

    indexed_iterator& operator+=(difference_type delta) noexcept {
        offset_ += static_cast<std::size_t>(delta);
        return *this;
    }

    indexed_iterator& operator-=(difference_type delta) noexcept {
        offset_ -= static_cast<std::size_t>(delta);
        return *this;
    }

    friend indexed_iterator operator+(indexed_iterator iterator, difference_type delta) noexcept {
        return iterator += delta;
    }

    friend indexed_iterator operator+(difference_type delta, indexed_iterator iterator) noexcept {
        return iterator += delta;
    }

    friend indexed_iterator operator-(indexed_iterator iterator, difference_type delta) noexcept {
        return iterator -= delta;
    }

    friend difference_type operator-(indexed_iterator const& first, indexed_iterator const& second) noexcept {
        return static_cast<difference_type>(first.offset_) - static_cast<difference_type>(second.offset_);
    }

    friend bool operator==(indexed_iterator const& first, indexed_iterator const& second) noexcept {
        return first.offset_ == second.offset_;
    }

    friend std::strong_ordering operator<=>(indexed_iterator const& first, indexed_iterator const& second) noexcept {
        return first.offset_ <=> second.offset_;
    }

    /** The index the iterator sits at. */
    std::size_t offset() const noexcept { return offset_; }

  private:

    /** The container answering every index, owned by whoever the iterator was made from. */
    container_t container_ {};

    /** The index the iterator sits at. */
    std::size_t offset_ = 0;
};

/** A run of elements one container builds, between two indices. */
template <typename container_type_>
using indexed_range = std::ranges::subrange<indexed_iterator<container_type_>>;

} // namespace unum::ustore::nvme
