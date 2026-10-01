/**
 *  @file src/nvme/files_disk.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The one ring a database on files mounts, holding whichever file transport the probe chose.
 *
 *  A database materializes over one disk type, and the transport is known only once the mount has
 *  asked the kernel, so the three file transports are erased here, at the disk. The alternative is
 *  fixed for the life of the mount and the branch is paid once per batch on @c submit and @c poll,
 *  each of which wraps a syscall. Every queue pair, registered memory and registered medium is the shared vocabulary
 *  of @c ring.hpp, so nothing above the disk sees which transport answered.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`

#include <span>    // `std::span`
#include <utility> // `std::move`
#include <variant> // `std::variant`, `std::visit`, `std::in_place_type`

#include "ustore/status.hpp" // `status_t`, `expected`
#include "ustore/types.hpp"  // `byte_t`

#include "nvme/blocking_files_disk.hpp"    // `blocking_files_disk_t`
#include "nvme/linux_aio_files_disk.hpp"   // `linux_aio_files_disk_t`
#include "nvme/linux_uring_files_disk.hpp" // `linux_uring_files_disk_t`

#include "nvme/config.hpp" // `io_transport_t`
#include "nvme/ring.hpp"   // `ring_concept`, `queue_pair_t`, `registered_memory_t`, `registered_medium_t`

namespace unum::ustore::nvme {

#pragma region Disk

/**
 *  @brief The ring of a database on files: the blocking floor, AIO or io_uring, as the mount probed.
 *
 *  The io_uring and AIO alternatives exist on every platform, as stubs that refuse off Linux, so
 *  the type is portable and only the mount's probe decides which alternative is built.
 */
class any_files_disk_t {
  public:

    /** Takes the blocking transport in, which a mount builds where the probe refused a ring. */
    explicit any_files_disk_t(blocking_files_disk_t&& disk) noexcept
        : held_(std::in_place_type<blocking_files_disk_t>, std::move(disk)) {}

    /** Takes the io_uring transport in, which a mount builds where the probe found a ring. */
    explicit any_files_disk_t(linux_uring_files_disk_t&& disk) noexcept
        : held_(std::in_place_type<linux_uring_files_disk_t>, std::move(disk)) {}

    /** Takes the Linux AIO transport in, which a mount builds where the probe refused a ring and found AIO. */
    explicit any_files_disk_t(linux_aio_files_disk_t&& disk) noexcept
        : held_(std::in_place_type<linux_aio_files_disk_t>, std::move(disk)) {}

    any_files_disk_t(any_files_disk_t const&) = delete;
    any_files_disk_t& operator=(any_files_disk_t const&) = delete;
    any_files_disk_t(any_files_disk_t&&) noexcept = default;
    any_files_disk_t& operator=(any_files_disk_t&&) noexcept = default;

    /** The transport held, which is never @c automatic_k. */
    [[nodiscard]] io_transport_t transport() const noexcept {
        if (std::holds_alternative<linux_uring_files_disk_t>(held_)) return io_transport_t::linux_uring_k;
        if (std::holds_alternative<linux_aio_files_disk_t>(held_)) return io_transport_t::linux_aio_k;
        return io_transport_t::blocking_k;
    }

    /** What the held transport accepts. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept {
        return std::visit([](auto const& disk) noexcept { return disk.capabilities(); }, held_);
    }

    /** How the held transport addresses the registered arena; the blocking floor and AIO pin per request. */
    [[nodiscard]] range_pinning_t pinning() const noexcept {
        linux_uring_files_disk_t const* const linux_uring = std::get_if<linux_uring_files_disk_t>(&held_);
        return linux_uring ? linux_uring->pinning() : range_pinning_t::unpinned_k;
    }

#pragma region Queue Pairs

    /** Takes a queue pair on @p depth requests of the held ring for the calling thread. */
    expected<queue_pair_t> acquire_queue_pair(queue_depth_t depth) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.acquire_queue_pair(depth); }, held_);
    }

    /** Binds @p queue pair to the calling thread, which is how a moved queue pair changes hands. */
    status_t bind_queue_pair(queue_pair_t const& queue_pair) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.bind_queue_pair(queue_pair); }, held_);
    }

    /** Takes @p queue pair back once it has nothing outstanding, and empties the handle. */
    status_t release_queue_pair(queue_pair_t& queue_pair) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.release_queue_pair(queue_pair); }, held_);
    }

    /** Requests @p queue pair has submitted and not yet completed. */
    [[nodiscard]] std::size_t outstanding(queue_pair_t const& queue_pair) const noexcept {
        return std::visit([&](auto const& disk) noexcept { return disk.outstanding(queue_pair); }, held_);
    }

#pragma endregion Queue Pairs

#pragma region Registrations

    /** Pins @p memory so requests can address it, refusing @c gpu_k on both file transports. */
    expected<registered_memory_t> register_memory(std::span<byte_t> memory, memory_space_t space) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.register_memory(memory, space); }, held_);
    }

    /** Unpins @p registered memory, refusing while requests still address it. */
    status_t unregister_memory(registered_memory_t& registered_memory) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.unregister_memory(registered_memory); }, held_);
    }

    /** Opens the file @p request names as a medium requests address by slot. */
    expected<registered_medium_t> register_medium(medium_options_t const& request) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.register_medium(request); }, held_);
    }

    /** Closes @p registered medium's file, refusing while requests still name it. */
    status_t unregister_medium(registered_medium_t& registered_medium) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.unregister_medium(registered_medium); }, held_);
    }

#pragma endregion Registrations

#pragma region Ring

    /** Takes as much of @p requests as @p queue pair still has credit for, and never waits. */
    expected<std::size_t> submit(queue_pair_t const& queue_pair, std::span<request_t const> requests) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.submit(queue_pair, requests); }, held_);
    }

    /** Moves @p queue pair's ready completions into @p into, and never waits. */
    expected<std::size_t> poll_completions(queue_pair_t const& queue_pair, std::span<completion_t> into) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.poll_completions(queue_pair, into); }, held_);
    }

    /** Stops @p queue pair's requests that have not started. */
    void cancel(queue_pair_t const& queue_pair) noexcept {
        std::visit([&](auto& disk) noexcept { disk.cancel(queue_pair); }, held_);
    }

    /** Reaps and discards everything @p queue pair has in flight, so the queue pair can be released or handed over. */
    status_t drain(queue_pair_t const& queue_pair) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.drain(queue_pair); }, held_);
    }

#pragma endregion Ring

  private:

    /** The transport the probe chose, fixed for the life of the mount. */
    std::variant<blocking_files_disk_t, linux_uring_files_disk_t, linux_aio_files_disk_t> held_;
};

static_assert(ring_concept<any_files_disk_t>);

#pragma endregion Disk

} // namespace unum::ustore::nvme
