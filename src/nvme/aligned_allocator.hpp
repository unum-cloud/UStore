/**
 *  @file src/nvme/aligned_allocator.hpp
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief The allocator of buffers unbuffered I/O accepts, aligned and sized to a block.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdlib> // `std::aligned_alloc`, `std::free`

#if defined(_WIN32)
#include <malloc.h> // `_aligned_malloc`, `_aligned_free`
#endif

#include <limits>      // `std::numeric_limits`
#include <type_traits> // `std::true_type`

namespace unum::ustore::nvme {

/**
 *  @brief Places every allocation on an @p alignment_ boundary, and rounds its size up to one.
 *
 *  Unbuffered reads and writes accept a buffer only when its address and its length are both
 *  multiples of the device's block size, which is what this allocator guarantees for every count.
 *  Exhaustion answers @c nullptr, as every allocator here does.
 */
template <typename value_type_, std::size_t alignment_>
class aligned_allocator {
  public:
    using value_type = value_type_;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;

    /** The boundary every allocation starts on, and the multiple its size is rounded up to. */
    static constexpr std::size_t alignment_k = alignment_;

    static_assert(alignment_k != 0 && (alignment_k & (alignment_k - 1)) == 0,
                  "An alignment is a power of two, as every block size is");

    constexpr aligned_allocator() noexcept = default;

    /** The rebinding conversion @c std::allocator_traits performs; the alignment is fixed. */
    template <typename other_type_>
    constexpr aligned_allocator(aligned_allocator<other_type_, alignment_> const&) noexcept {}

    /** Allocates room for @p count values on an aligned boundary, or answers @c nullptr. */
    [[nodiscard]] value_type_* allocate(std::size_t count) noexcept {
        if (count == 0 || count > max_count_k) return nullptr;
#if defined(_WIN32)
        return static_cast<value_type_*>(::_aligned_malloc(aligned_bytes(count), alignment_k));
#else
        return static_cast<value_type_*>(std::aligned_alloc(alignment_k, aligned_bytes(count)));
#endif
    }

    /** Releases @p pointer, which must come from @c allocate. */
    void deallocate(value_type_* pointer, std::size_t) noexcept {
#if defined(_WIN32)
        ::_aligned_free(pointer);
#else
        std::free(pointer);
#endif
    }

    /** The bytes @p count values occupy once rounded up to the alignment. */
    static constexpr std::size_t aligned_bytes(std::size_t count) noexcept {
        return (count * sizeof(value_type_) + alignment_k - 1) / alignment_k * alignment_k;
    }

    template <typename other_type_>
    constexpr bool operator==(aligned_allocator<other_type_, alignment_> const&) const noexcept {
        return true;
    }

  private:

    /** The largest count whose aligned byte size still fits a @c std::size_t. */
    static constexpr std::size_t max_count_k = (std::numeric_limits<std::size_t>::max() - alignment_k) /
                                               sizeof(value_type_);
};

} // namespace unum::ustore::nvme
