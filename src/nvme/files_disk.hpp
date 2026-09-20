/**
 *  @file src/nvme/files_disk.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The one ring a database on files mounts, holding whichever file transport the probe chose.
 *
 *  A database materializes over one disk type, and the transport is known only once the mount has
 *  asked the kernel, so the two file transports are erased here, at the disk. The alternative is
 *  fixed for the life of the mount and the branch is paid once per batch on @c submit and @c reap,
 *  each of which wraps a syscall. Every lease, registration and descriptor is the shared vocabulary
 *  of @c ring.hpp, so nothing above the disk sees which transport answered.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`

#include <span>    // `std::span`
#include <utility> // `std::move`
#include <variant> // `std::variant`, `std::visit`, `std::in_place_type`

#include <ustore/status.hpp> // `status_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/blocking_files_disk.hpp" // `blocking_files_disk_t`
#include "nvme/config.hpp"              // `io_discipline_t`
#include "nvme/ring.hpp"                // `ring_concept`, `ring_owner_t`, `registration_t`, `descriptor_t`
#include "nvme/uring_files_disk.hpp"    // `uring_files_disk_t`

namespace unum::ustore::nvme {

#pragma region Disk

/**
 *  @brief The ring of a database on files: the blocking floor or io_uring, whichever the mount probed.
 *
 *  The uring alternative exists on every platform, as a stub that refuses where the kernel has no
 *  ring, so the type is portable and only the mount's probe decides which alternative is built.
 */
class any_files_disk_t {
  public:

    /** Takes the blocking transport in, which a mount builds where the probe refused a ring. */
    explicit any_files_disk_t(blocking_files_disk_t&& disk) noexcept
        : held_(std::in_place_type<blocking_files_disk_t>, std::move(disk)) {}

    /** Takes the io_uring transport in, which a mount builds where the probe found a ring. */
    explicit any_files_disk_t(uring_files_disk_t&& disk) noexcept
        : held_(std::in_place_type<uring_files_disk_t>, std::move(disk)) {}

    any_files_disk_t(any_files_disk_t const&) = delete;
    any_files_disk_t& operator=(any_files_disk_t const&) = delete;
    any_files_disk_t(any_files_disk_t&&) noexcept = default;
    any_files_disk_t& operator=(any_files_disk_t&&) noexcept = default;

    /** The transport held, which is never @c automatic_k. */
    [[nodiscard]] io_discipline_t discipline() const noexcept {
        return std::holds_alternative<uring_files_disk_t>(held_) ? io_discipline_t::uring_k
                                                                 : io_discipline_t::blocking_k;
    }

    /** What the held transport accepts. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept {
        return std::visit([](auto const& disk) noexcept { return disk.capabilities(); }, held_);
    }

    /** How the held transport addresses the registered slab; the blocking floor pins per request. */
    [[nodiscard]] range_pinning_t pinning() const noexcept {
        uring_files_disk_t const* const uring = std::get_if<uring_files_disk_t>(&held_);
        return uring ? uring->pinning() : range_pinning_t::unpinned_k;
    }

#pragma region Leases

    /** Takes a lease on @p depth requests of the held ring for the calling thread. */
    expected<ring_owner_t> open(std::uint32_t depth) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.open(depth); }, held_);
    }

    /** Binds @p owner to the calling thread, which is how a moved lease changes hands. */
    status_t adopt(ring_owner_t const& owner) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.adopt(owner); }, held_);
    }

    /** Takes @p owner's lease back once it has nothing outstanding, and empties the handle. */
    status_t release(ring_owner_t& owner) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.release(owner); }, held_);
    }

    /** Requests @p owner has submitted and not yet reaped. */
    [[nodiscard]] std::size_t in_flight(ring_owner_t const& owner) const noexcept {
        return std::visit([&](auto const& disk) noexcept { return disk.in_flight(owner); }, held_);
    }

#pragma endregion Leases

#pragma region Registrations

    /** Pins @p chunk so requests can address it, refusing @c device_k on both file transports. */
    expected<registration_t> register_chunk(std::span<byte_t> chunk, memory_space_t space) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.register_chunk(chunk, space); }, held_);
    }

    /** Unpins @p registration, refusing while requests still address it. */
    status_t deregister_chunk(registration_t& registration) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.deregister_chunk(registration); }, held_);
    }

    /** Opens the file @p request names as a target requests address by slot. */
    expected<descriptor_t> register_target(target_request_t const& request) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.register_target(request); }, held_);
    }

    /** Closes @p descriptor's file, refusing while requests still name it. */
    status_t deregister_target(descriptor_t& descriptor) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.deregister_target(descriptor); }, held_);
    }

#pragma endregion Registrations

#pragma region Ring

    /** Takes as much of @p requests as @p owner still has credit for, and never waits. */
    expected<std::size_t> submit(ring_owner_t const& owner, std::span<request_t const> requests) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.submit(owner, requests); }, held_);
    }

    /** Moves @p owner's ready completions into @p into, and never waits. */
    expected<std::size_t> reap(ring_owner_t const& owner, std::span<completion_t> into) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.reap(owner, into); }, held_);
    }

    /** Stops @p owner's requests that have not started. */
    void cancel(ring_owner_t const& owner) noexcept {
        std::visit([&](auto& disk) noexcept { disk.cancel(owner); }, held_);
    }

    /** Reaps and discards everything @p owner has in flight, so the lease can be released or handed over. */
    status_t drain(ring_owner_t const& owner) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.drain(owner); }, held_);
    }

#pragma endregion Ring

  private:

    /** The transport the probe chose, fixed for the life of the mount. */
    std::variant<blocking_files_disk_t, uring_files_disk_t> held_;
};

static_assert(ring_concept<any_files_disk_t>);

#pragma endregion Disk

} // namespace unum::ustore::nvme
