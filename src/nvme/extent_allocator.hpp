/**
 *  @file src/nvme/extent_allocator.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The best-fit free-space map of one contiguous region, rebuilt at every mount.
 *
 *  The map never reaches the medium. A mount replays the catalog and reserves the extent of every
 *  entry it names, so an extent no entry names is free by construction and a crash between a write
 *  and the catalog that publishes it leaks nothing. Best fit is what the write pattern deserves: a
 *  blob is written once and dropped whole, so coalescing is complete and fragmentation follows the
 *  size mix alone rather than the order the calls arrived in.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`

#include "nvme/extent.hpp" // `extent_t`, `is_power_of_two`, `rounded_up_to`

namespace unum::ustore::nvme {

#pragma region Granularities

/** Bytes from which an allocation rounds its offset as well as its length. */
inline constexpr std::uint64_t large_allocation_bytes_k = std::uint64_t {8} << 20;

/** The offset alignment such an allocation takes, coarse enough for a stripe and an erase block. */
inline constexpr std::uint64_t large_allocation_alignment_k = std::uint64_t {1} << 20;

#pragma endregion Granularities

#pragma region Allocator

/**
 *  @brief The free ranges of one contiguous region, handed out by best fit and coalesced on return.
 *
 *  Two granularities, not one: every allocation rounds its length up to the disk's own granularity,
 *  and only an allocation of @c large_allocation_bytes_k or more rounds its offset up to the coarse
 *  alignment as well, since rounding a kilobyte of blob to a megabyte would waste three orders of
 *  magnitude. The map holds no allocation, only what is left, so it is rebuilt by @c reserve at
 *  mount and never written down.
 */
class extent_allocator_t {
  public:

    extent_allocator_t() noexcept = default;
    extent_allocator_t(extent_allocator_t&&) noexcept = default;
    extent_allocator_t& operator=(extent_allocator_t&&) noexcept = default;
    extent_allocator_t(extent_allocator_t const&) = delete;
    extent_allocator_t& operator=(extent_allocator_t const&) = delete;

    /**
     *  @brief Builds an empty map of @p region, refusing a shape no allocation could satisfy.
     *  @param[in] region The bytes the volume hands out, which begins and ends on @p granularity.
     *  @param[in] granularity The multiple every allocated length takes, a power of two.
     *  @param[in] large_alignment The multiple a large allocation's offset takes, at least @p granularity.
     *  @return The map, or @c status_code_t::invalid_argument_k for a shape it cannot serve.
     */
    static expected<extent_allocator_t> make(extent_t region, std::uint64_t granularity,
                                             std::uint64_t large_alignment) noexcept {
        if (!is_power_of_two(granularity) || !is_power_of_two(large_alignment))
            return status_t {status_code_t::invalid_argument_k, "An allocation granularity is a power of two"};
        if (large_alignment < granularity)
            return status_t {status_code_t::invalid_argument_k, "A large allocation is aligned no more finely"};
        if (!region.aligned_to(granularity) || region.offset > UINT64_MAX - region.length)
            return status_t {status_code_t::invalid_argument_k, "A region begins and ends on its granularity"};

        extent_allocator_t allocator;
        allocator.region_ = region;
        allocator.granularity_ = granularity;
        allocator.large_alignment_ = large_alignment;
        allocator.free_bytes_ = region.length;
        if (!region.empty() && allocator.free_ranges_.push_back(extent_t {region}) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "The free map would not hold the region"};
        return allocator;
    }

    /**
     *  @brief Takes @p extent out of the map verbatim, which is how a mount replays the catalog.
     *  @param[in] extent The range one catalog entry occupies, already rounded when it was allocated.
     *  @return Success, or @c status_code_t::malformed_k when the catalog claims it twice or claims
     *      bytes the volume does not have.
     */
    status_t reserve(extent_t extent) noexcept {
        if (extent.empty()) return {};
        if (!region_.contains(extent))
            return status_t {status_code_t::malformed_k, "A cataloged extent falls outside the volume"};
        for (std::size_t index = 0; index != free_ranges_.size(); ++index)
            if (free_ranges_[index].contains(extent)) return carve_(index, extent);
        return status_t {status_code_t::malformed_k, "Two cataloged extents overlap"};
    }

    /**
     *  @brief Hands out the range that leaves the least behind, rounded as the two granularities ask.
     *  @param[in] length Bytes the caller needs, rounded up to the granularity before the search.
     *  @return The range, or @c status_code_t::capacity_exhausted_k when no free range fits.
     */
    expected<extent_t> allocate(std::uint64_t length) noexcept {
        if (length == 0) return status_t {status_code_t::invalid_argument_k, "An allocation of no bytes names nothing"};
        std::uint64_t const rounded = rounded_up_to(length, granularity_);
        if (rounded < length)
            return status_t {status_code_t::capacity_exhausted_k, "The rounded length does not fit an address"};
        std::uint64_t const alignment = rounded >= large_allocation_bytes_k ? large_alignment_ : granularity_;

        // A linear scan, since the range count is bounded by the number of live blobs.
        std::size_t chosen = free_ranges_.size();
        std::uint64_t chosen_leftover = 0;
        std::uint64_t chosen_offset = 0;
        for (std::size_t index = 0; index != free_ranges_.size(); ++index) {
            extent_t const range = free_ranges_[index];
            std::uint64_t const start = rounded_up_to(range.offset, alignment);
            if (start < range.offset || start > range.end()) continue;
            std::uint64_t const reachable = range.end() - start;
            if (reachable < rounded) continue;
            std::uint64_t const leftover = reachable - rounded;
            if (chosen != free_ranges_.size() && leftover >= chosen_leftover) continue;
            chosen = index;
            chosen_leftover = leftover;
            chosen_offset = start;
        }
        if (chosen == free_ranges_.size())
            return status_t {status_code_t::capacity_exhausted_k, "No free range fits the allocation"};

        extent_t const taken {chosen_offset, rounded};
        status_t const carved = carve_(chosen, taken);
        if (carved.failed()) return carved;
        return taken;
    }

    /**
     *  @brief Gives @p extent back, merging it with the free range before it and the one after it.
     *  @param[in] extent The range one allocation held, exactly as it was handed out.
     *  @return Success, or @c status_code_t::invalid_argument_k when any of those bytes is free already.
     */
    status_t release(extent_t extent) noexcept {
        if (extent.empty()) return {};
        if (!region_.contains(extent))
            return status_t {status_code_t::invalid_argument_k, "A released extent falls outside the volume"};

        std::size_t position = 0;
        while (position != free_ranges_.size() && free_ranges_[position].offset < extent.offset) ++position;
        if (position != 0 && free_ranges_[position - 1].end() > extent.offset)
            return status_t {status_code_t::invalid_argument_k, "A released extent is free already"};
        if (position != free_ranges_.size() && free_ranges_[position].offset < extent.end())
            return status_t {status_code_t::invalid_argument_k, "A released extent is free already"};

        if (position != 0 && free_ranges_[position - 1].end() == extent.offset) {
            free_ranges_[position - 1].length += extent.length;
            if (position != free_ranges_.size() && free_ranges_[position - 1].end() == free_ranges_[position].offset) {
                free_ranges_[position - 1].length += free_ranges_[position].length;
                erase_at_(position);
            }
        }
        else if (position != free_ranges_.size() && extent.end() == free_ranges_[position].offset)
            free_ranges_[position] = extent_t {extent.offset, extent.length + free_ranges_[position].length};
        else {
            status_t const inserted = insert_at_(position, extent);
            if (inserted.failed()) return inserted;
        }
        free_bytes_ += extent.length;
        return {};
    }

    /** Bytes no allocation holds, which the alignment padding of a large allocation counts among. */
    [[nodiscard]] std::uint64_t free_bytes() const noexcept { return free_bytes_; }

    /** Bytes of the longest free range, which bounds the largest allocation that can still succeed. */
    [[nodiscard]] std::uint64_t largest_free_bytes() const noexcept {
        std::uint64_t longest = 0;
        for (std::size_t index = 0; index != free_ranges_.size(); ++index)
            if (free_ranges_[index].length > longest) longest = free_ranges_[index].length;
        return longest;
    }

    /** Bytes of the region the map hands out, free and allocated alike. */
    [[nodiscard]] std::uint64_t capacity() const noexcept { return region_.length; }

  private:

    /** Takes @p taken out of the free range at @p position, leaving its head and its tail behind. */
    status_t carve_(std::size_t position, extent_t taken) noexcept {
        extent_t const range = free_ranges_[position];
        extent_t const head {range.offset, taken.offset - range.offset};
        extent_t const tail {taken.end(), range.end() - taken.end()};
        if (!head.empty() && !tail.empty()) {
            status_t const inserted = insert_at_(position + 1, tail);
            if (inserted.failed()) return inserted;
            free_ranges_[position] = head;
        }
        else if (!head.empty()) free_ranges_[position] = head;
        else if (!tail.empty()) free_ranges_[position] = tail;
        else erase_at_(position);
        free_bytes_ -= taken.length;
        return {};
    }

    /** Puts @p range at @p position, shifting the later ranges up, since the vector has no insert. */
    status_t insert_at_(std::size_t position, extent_t range) noexcept {
        if (free_ranges_.push_back(extent_t {}) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "The free map would not grow"};
        for (std::size_t index = free_ranges_.size() - 1; index != position; --index)
            free_ranges_[index] = free_ranges_[index - 1];
        free_ranges_[position] = range;
        return {};
    }

    /** Drops the range at @p position, shifting the later ranges down, since the vector has no erase. */
    void erase_at_(std::size_t position) noexcept {
        for (std::size_t index = position + 1; index != free_ranges_.size(); ++index)
            free_ranges_[index - 1] = free_ranges_[index];
        free_ranges_.pop_back();
    }

    /** The free ranges, disjoint, non-adjacent and sorted by offset. */
    st::basic_vector<extent_t> free_ranges_;

    /** The region every range sits inside, which no allocation ever leaves. */
    extent_t region_;

    /** The multiple every allocated length takes. */
    std::uint64_t granularity_ = 1;

    /** The multiple a large allocation's offset takes. */
    std::uint64_t large_alignment_ = 1;

    /** Bytes the free ranges add up to, kept as they change rather than summed on demand. */
    std::uint64_t free_bytes_ = 0;
};

#pragma endregion Allocator

} // namespace unum::ustore::nvme
