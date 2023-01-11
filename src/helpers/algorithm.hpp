/**
 * @file algorithm.hpp
 * @author Ashot Vardanian
 *
 * @brief STL algorithms adjusted for strided iterators.
 */
#pragma once
#include <algorithm> // `std::sort`
#include <numeric>   // `std::accumulate`
#include <forward_list>

namespace unum::ukv {

template <typename range_at, typename comparable_at>
inline range_at equal_subrange(range_at range, comparable_at&& comparable) {
    auto p = std::equal_range(range.begin(), range.end(), comparable);
    return range_at {p.first, p.second};
}

template <typename element_at, typename alloc_at = std::allocator<element_at>>
void sort_and_deduplicate(std::vector<element_at, alloc_at>& elems) {
    elems.erase(elems.begin() + sort_and_deduplicate(elems.begin(), elems.end()), elems.end());
}

template <typename container_at, typename comparable_at>
std::size_t offset_in_sorted(container_at const& elems, comparable_at const& wanted) {
    return std::lower_bound(elems.begin(), elems.end(), wanted) - elems.begin();
}

template <typename element_at>
element_at inplace_inclusive_prefix_sum(element_at* begin, element_at* const end) {
    element_at sum = 0;
    for (; begin != end; ++begin)
        sum += std::exchange(*begin, *begin + sum);
    return sum;
}

/**
 * @brief In many "modality" implementations, we may have batches of requests,
 * where distinct queries map into the same entries. In that case, the trivial
 * "gather+scatter" operation gets two more stages: deduplication and join.
 *
 */
inline void deduplicate_gather_join_scatter() {
}

} // namespace unum::ukv
