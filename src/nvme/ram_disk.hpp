/**
 *  @file src/nvme/ram_disk.hpp
 *  @author Ash Vardanian
 *  @date September 18, 2026
 *  @brief A volume in memory behind the same ring, so a suite exercises the contract without a device.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/aligned_allocator.hpp" // `aligned_allocator`
#include "nvme/disk.hpp"              // `basic_disk`
#include "nvme/extent.hpp"            // `extent_t`, `portable_alignment_k`
#include "nvme/ring.hpp"              // `ring_capabilities_t`

namespace unum::ustore::nvme {

#pragma region Device

/**
 *  @brief Fixed-size volumes in host memory, copied into and out of at absolute offsets.
 *
 *  A volume never grows, so a request that reaches its end moves a prefix and stops, which is the
 *  partial transfer a real device reports at the end of a namespace. The alignments it demands are a
 *  mount parameter rather than a property of memory, so a suite can hold a transport to the numbers
 *  an @c O_DIRECT file or an NVMe namespace would report without owning either.
 */
class ram_device_t {
  public:

    ram_device_t() noexcept = default;
    ram_device_t(ram_device_t const&) = delete;
    ram_device_t& operator=(ram_device_t const&) = delete;
    ram_device_t(ram_device_t&&) noexcept = default;
    ram_device_t& operator=(ram_device_t&&) noexcept = default;

    /**
     *  @brief Makes a device whose volumes demand @p alignment of every request.
     *  @param[in] alignment Memory, offset and length alignment the ring holds requests to.
     *
     *  @note No volume is allocated here; a mount registers each one as a medium, so one device
     *      serves a directory of many volumes exactly as a file device does.
     */
    static expected<ram_device_t> make(std::uint64_t alignment = 1) noexcept {
        if (!is_power_of_two(alignment))
            return status_t {status_code_t::invalid_argument_k, "An alignment is a power of two"};
        ram_device_t device;
        device.alignment_ = alignment;
        return device;
    }

    /**
     *  @brief Allocates a volume of @c request.length bytes, which no write grows.
     *  @param[in] request The bytes to allocate; its name labels the volume and is not parsed.
     *  @return The slot the volume took, which every request against it names.
     */
    expected<std::uint16_t> open_medium(medium_options_t const& request) noexcept {
        if (request.length == 0)
            return status_t {status_code_t::invalid_argument_k, "A volume holds at least one byte"};
        auto storage = storage_t::make(static_cast<std::size_t>(request.length));
        if (!storage) return status_t {status_code_t::out_of_memory_k, "A volume of that size does not fit"};
        if (storage->resize(static_cast<std::size_t>(request.length)) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A volume of that size does not fit"};

        for (std::size_t index = 0; index != volumes_.size(); ++index)
            if (volumes_[index].empty()) {
                volumes_[index] = std::move(*storage);
                return static_cast<std::uint16_t>(index);
            }
        if (volumes_.push_back(std::move(*storage)) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A device could not track another medium"};
        return static_cast<std::uint16_t>(volumes_.size() - 1);
    }

    /** Releases the volume in @p index, leaving its slot free for the next medium. */
    status_t close_medium(std::uint16_t index) noexcept {
        if (index >= volumes_.size() || volumes_[index].empty())
            return {status_code_t::invalid_argument_k, "The medium is not this device's"};
        volumes_[index] = storage_t {};
        return {};
    }

    /** Reads @p extent of @p index into @p destination, answering the prefix inside the volume. */
    expected<std::uint64_t> read_extent(std::uint16_t index, extent_t extent, std::span<byte_t> destination) noexcept {
        if (index >= volumes_.size() || volumes_[index].empty())
            return status_t {status_code_t::invalid_argument_k, "The medium is not this device's"};
        storage_t& volume = volumes_[index];
        extent_t const reachable = extent.clipped_to(volume.size());
        if (reachable.length != 0)
            std::memcpy(destination.data(), volume.data() + reachable.offset,
                        static_cast<std::size_t>(reachable.length));
        return reachable.length;
    }

    /** Writes @p source at @p extent of @p index, answering the prefix that fit. */
    expected<std::uint64_t> write_extent(std::uint16_t index, extent_t extent,
                                         std::span<byte_t const> source) noexcept {
        if (index >= volumes_.size() || volumes_[index].empty())
            return status_t {status_code_t::invalid_argument_k, "The medium is not this device's"};
        storage_t& volume = volumes_[index];
        extent_t const reachable = extent.clipped_to(volume.size());
        if (reachable.length != 0)
            std::memcpy(volume.data() + reachable.offset, source.data(), static_cast<std::size_t>(reachable.length));
        return reachable.length;
    }

    /** Succeeds without work, since a volume in memory has nothing below it to reach. */
    status_t flush(std::uint16_t index) noexcept {
        if (index >= volumes_.size() || volumes_[index].empty())
            return {status_code_t::invalid_argument_k, "The medium is not this device's"};
        return {};
    }

    /** The bytes the volume in @p index holds, fixed when it was attached. */
    [[nodiscard]] std::uint64_t capacity(std::uint16_t index) const noexcept {
        return index < volumes_.size() ? volumes_[index].size() : 0;
    }

    /** What this transport accepts, which is the alignment the mount asked it to demand. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept {
        ring_capabilities_t reported;
        reported.memory_alignment = alignment_;
        reported.offset_alignment = alignment_;
        reported.length_granularity = alignment_;
        reported.max_transfer_bytes = 0;
        reported.reaches_gpu_memory = false;
        return reported;
    }

  private:

    /** One volume, aligned so a mount demanding the portable alignment can still address offset zero. */
    using storage_t = st::basic_vector<byte_t, aligned_allocator<byte_t, portable_alignment_k>>;

    /** One entry per registered medium, empty where a slot is free. */
    st::basic_vector<storage_t> volumes_;

    /** Memory, offset and length alignment the ring holds requests to. */
    std::uint64_t alignment_ = 1;
};

/** The ring a suite drives without a device, satisfying the same contract as every disk. */
using ram_disk_t = basic_disk<ram_device_t>;

#pragma endregion Device

} // namespace unum::ustore::nvme
