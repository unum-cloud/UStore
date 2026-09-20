/**
 *  @file src/nvme/disk.hpp
 *  @author Ash Vardanian
 *  @date September 18, 2026
 *  @brief The ring machinery every NVMe disk shares, over a device that moves positioned bytes.
 *
 *  A disk pairs a volume with a transport. The volume part - naming, allocation, the catalog - sits
 *  above; what lives here is the transport part every disk repeats: leases, per-owner credit,
 *  registered memory, batching and completion routing. A transport supplies only the moves a device
 *  makes against a target, and @c basic_disk turns them into the ring of @c ring.hpp.
 *
 *  Depth on the blocking floor comes from leases rather than threads: a submitted batch waits in the
 *  owner's window and the owner's own @c reap performs it, so the owner pays for its own I/O on the
 *  thread that asked for it. Nothing is parked, nothing is woken, and nothing forks.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <atomic>     // `std::memory_order`
#include <concepts>   // `std::same_as`
#include <functional> // `std::hash`
#include <span>       // `std::span`
#include <thread>     // `std::this_thread::get_id`
#include <utility>    // `std::exchange`, `std::move`

#include <forkunion/atomics.hpp>       // `fu::preferred_atomic_ref`
#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/extent.hpp" // `extent_t`, `is_address_aligned`
#include "nvme/ring.hpp"   // `request_t`, `completion_t`, `ring_owner_t`, `registration_t`, `descriptor_t`

namespace unum::ustore::nvme {

#pragma region Devices

/**
 *  @brief The moves a transport implements, from which @c basic_disk builds a whole ring.
 *
 *  Every call is synchronous and thread-safe for distinct extents, which positioned reads and writes
 *  are on every platform. Asynchrony is the ring's business, not the device's, so a device that
 *  already has a queue of its own implements the ring directly instead of satisfying this.
 */
template <typename device_type_>
concept block_device_concept = requires(device_type_& device, device_type_ const& constant, target_request_t target,
                                        std::uint16_t index, extent_t extent, std::span<byte_t> destination,
                                        std::span<byte_t const> source) {
    { device.attach_target(target) } -> std::same_as<expected<std::uint16_t>>;
    { device.detach_target(index) } -> std::same_as<status_t>;
    { device.read_extent(index, extent, destination) } -> std::same_as<expected<std::uint64_t>>;
    { device.write_extent(index, extent, source) } -> std::same_as<expected<std::uint64_t>>;
    { device.flush(index) } -> std::same_as<status_t>;
    { constant.capacity(index) } -> std::same_as<std::uint64_t>;
    { constant.capabilities() } -> std::same_as<ring_capabilities_t>;
};

/** Identifies the thread holding a lease, so a foreign issuer trips an assertion and not a driver. */
[[nodiscard]] inline std::uint64_t issuing_thread_identity() noexcept {
    return static_cast<std::uint64_t>(std::hash<std::thread::id> {}(std::this_thread::get_id()));
}

#pragma endregion Devices

#pragma region Configuration

/** What a mount fixes about a disk's ring before any owner leases a share of it. */
struct disk_config_t {

    /** Leases the ring hands out at once, bounded on a real controller by its queue-pair count. */
    std::uint16_t owners_limit = 1;

    /** Requests one lease may keep in flight, which is the credit an owner spends and reaps back. */
    std::uint32_t depth_limit = 64;

    /** Memory ranges the disk pins at once, which a transport registering one slab keeps at one. */
    std::uint16_t registrations_limit = 8;

    /** Targets the disk registers at once, which a namespace keeps at one and a directory raises. */
    std::uint16_t targets_limit = 1;
};

#pragma endregion Configuration

#pragma region Disk

/**
 *  @brief Turns a positioned-I/O device into the submission and completion ring callers drive.
 *
 *  Depth is partitioned rather than pooled: every lease reserves @c depth_limit requests of its own,
 *  so one owner that stops reaping cannot starve another, and a full ring is always the owner's own
 *  doing. Completions are routed by the owner bits of their token and never appear in another
 *  owner's reap.
 *
 *  Ordering between two submitted requests is never promised. Two writes may reach the medium in
 *  either order, and a flush covers only what the owner has already reaped, so durability is written
 *  as submit, reap, flush, reap. A bounded sequence that must be ordered says so with
 *  @c request_chaining_t::chained_k, which runs the chain in submission order and cuts it with
 *  @c cancelled_k at the first short or failed result.
 */
template <typename device_type_>
    requires block_device_concept<device_type_>
class basic_disk {
  public:

    basic_disk(basic_disk const&) = delete;
    basic_disk& operator=(basic_disk const&) = delete;
    basic_disk(basic_disk&&) noexcept = default;
    basic_disk& operator=(basic_disk&&) noexcept = default;

    /**
     *  @brief Mounts @p device behind a ring shaped by @p config.
     *  @param[in] device The transport, moved in and owned by the disk from here on.
     *  @param[in] config Leases, per-lease depth, and the pinned ranges and targets the ring holds.
     */
    static expected<basic_disk> make(device_type_&& device, disk_config_t const& config) noexcept {
        if (config.owners_limit == 0 || config.depth_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A ring needs a lease and a request of depth"};
        if (config.owners_limit == static_cast<std::uint16_t>(owner_id_t::unowned_k) ||
            config.registrations_limit == static_cast<std::uint16_t>(registration_id_t::unregistered_k) ||
            config.targets_limit == static_cast<std::uint16_t>(descriptor_id_t::unregistered_k))
            return status_t {status_code_t::invalid_argument_k, "A ring keeps one identifier for an empty handle"};
        if (config.targets_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A ring needs a target to move bytes against"};

        std::size_t const window = static_cast<std::size_t>(config.owners_limit) * config.depth_limit;
        basic_disk disk {std::move(device), config};
        if (status_t const sized = size_(disk.owners_, config.owners_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.registrations_, config.registrations_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.targets_, config.targets_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.pending_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.ready_, window); sized.failed()) return sized;
        disk.capabilities_ = disk.device_.capabilities();
        disk.capabilities_.depth_limit = config.depth_limit;
        disk.capabilities_.owners_limit = config.owners_limit;
        disk.capabilities_.targets_limit = config.targets_limit;
        return disk;
    }

    /** What this disk accepts, fixed at mount so no request pays to ask the transport again. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept { return capabilities_; }

    /** The transport underneath, for a caller that needs what only the transport knows. */
    [[nodiscard]] device_type_ const& device() const noexcept { return device_; }

#pragma region Leases

    /**
     *  @brief Takes a lease on @p depth requests of the ring for the calling thread.
     *  @param[in] depth Requests this owner may keep in flight, at most the mount's limit.
     *  @return The lease, or @c capacity_exhausted_k when every lease is already out.
     */
    expected<ring_owner_t> open(std::uint32_t depth) noexcept {
        if (depth == 0 || depth > depth_limit_)
            return status_t {status_code_t::invalid_argument_k, "A lease asks for more depth than the ring has"};
        for (std::size_t index = 0; index != owners_.size(); ++index) {
            owner_slot_t& slot = owners_[index];
            std::int32_t expected_free = 0;
            if (!claim_(slot.leased)
                     .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            slot.cancelled = 0;
            slot.depth = depth;
            slot.pending_count = 0;
            slot.ready_first = 0;
            slot.ready_count = 0;
            slot.issuer = issuing_thread_identity();
            return ring_owner_t::leased(static_cast<owner_id_t>(index), slot.generation, depth);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every lease of this ring is already held"};
    }

    /**
     *  @brief Binds @p owner to the calling thread, which is how a moved lease changes hands.
     *  @param[in] owner The lease this thread has just taken over, with nothing outstanding on it.
     *
     *  @note A lease with work in flight cannot be handed over, since the requests were issued from
     *      a thread that is no longer the one that would reap them.
     */
    status_t adopt(ring_owner_t const& owner) noexcept {
        owner_slot_t* const slot = slot_of_(owner);
        if (!slot) return {status_code_t::invalid_argument_k, "The lease is not this ring's"};
        if (slot->pending_count != 0 || slot->ready_count != 0)
            return {status_code_t::invalid_argument_k, "Drain the lease before handing it over"};
        slot->issuer = issuing_thread_identity();
        return {};
    }

    /**
     *  @brief Takes @p owner's lease back once it has nothing outstanding, and empties the handle.
     *  @param[inout] owner The lease to return, surrendered on success and untouched on failure.
     */
    status_t release(ring_owner_t& owner) noexcept {
        owner_slot_t* const slot = slot_of_(owner);
        if (!slot) return {status_code_t::invalid_argument_k, "The lease is not this ring's"};
        if (slot->pending_count != 0 || slot->ready_count != 0)
            return {status_code_t::invalid_argument_k, "Drain the lease before releasing it"};
        ++slot->generation;
        slot->depth = 0;
        slot->issuer = 0;
        claim_(slot->leased).store(0, std::memory_order_release);
        owner.surrender();
        return {};
    }

    /** Requests @p owner has submitted and not yet reaped, which its credit bounds. */
    [[nodiscard]] std::size_t in_flight(ring_owner_t const& owner) const noexcept {
        owner_slot_t const* const slot = slot_of_(owner);
        return slot ? slot->pending_count + slot->ready_count : 0;
    }

#pragma endregion Leases

#pragma region Registrations

    /**
     *  @brief Pins @p chunk so requests can address it, and says whether this transport reaches it.
     *  @param[in] chunk Memory the caller owns and keeps alive until it deregisters the range.
     *  @param[in] space Where that memory lives; a disk without GPUDirect refuses @c device_k here.
     */
    expected<registration_t> register_chunk(std::span<byte_t> chunk, memory_space_t space) noexcept {
        if (chunk.empty()) return status_t {status_code_t::invalid_argument_k, "An empty range registers nothing"};
        if (space == memory_space_t::device_k && !device_.capabilities().reaches_device_memory)
            return status_t {status_code_t::unsupported_k, "This transport cannot address device memory"};
        for (std::size_t index = 0; index != registrations_.size(); ++index) {
            registration_slot_t& slot = registrations_[index];
            std::int32_t expected_free = 0;
            if (!claim_(slot.registered)
                     .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            slot.base = chunk.data();
            slot.length = chunk.size();
            slot.space = space;
            return registration_t::registered(static_cast<registration_id_t>(index), slot.generation, chunk, space);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every registration slot of this ring is taken"};
    }

    /**
     *  @brief Unpins @p registration, refusing while it is still a live target of the device.
     *  @param[inout] registration The range to give back, surrendered on success.
     */
    status_t deregister_chunk(registration_t& registration) noexcept {
        registration_slot_t* const slot = slot_of_(registration);
        if (!slot) return {status_code_t::invalid_argument_k, "The registration is not this ring's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "The range is still a target of requests in flight"};
        ++slot->generation;
        slot->base = nullptr;
        slot->length = 0;
        claim_(slot->registered).store(0, std::memory_order_release);
        registration.surrender();
        return {};
    }

    /**
     *  @brief Registers the target @p request names, so requests can address it by slot.
     *  @param[in] request The path, label or namespace to attach, and what the caller may do to it.
     *  @return The handle, or @c capacity_exhausted_k when every target slot is taken.
     */
    expected<descriptor_t> register_target(target_request_t const& request) noexcept {
        for (std::size_t index = 0; index != targets_.size(); ++index) {
            target_slot_t& slot = targets_[index];
            std::int32_t expected_free = 0;
            if (!claim_(slot.registered)
                     .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            expected<std::uint16_t> const attached = device_.attach_target(request);
            if (!attached) {
                claim_(slot.registered).store(0, std::memory_order_release);
                return attached.status();
            }
            slot.index = *attached;
            return descriptor_t::registered(static_cast<descriptor_id_t>(index), slot.generation,
                                            device_.capacity(slot.index));
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every target slot of this ring is taken"};
    }

    /**
     *  @brief Releases @p descriptor, refusing while requests against it are still in flight.
     *  @param[inout] descriptor The target to give back, surrendered on success.
     */
    status_t deregister_target(descriptor_t& descriptor) noexcept {
        target_slot_t* const slot = slot_of_(descriptor);
        if (!slot) return {status_code_t::invalid_argument_k, "The target is not this ring's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "The target is still named by requests in flight"};
        if (status_t const detached = device_.detach_target(slot->index); detached.failed()) return detached;
        ++slot->generation;
        slot->index = 0;
        claim_(slot->registered).store(0, std::memory_order_release);
        descriptor.surrender();
        return {};
    }

#pragma endregion Registrations

#pragma region Ring

    /**
     *  @brief Takes as much of @p requests as @p owner still has credit for, and never waits.
     *  @param[in] owner The lease submitting, held by the calling thread.
     *  @param[in] requests The batch, of which a prefix is taken.
     *  @return How many were taken, which is zero on a full ring and never a reason to block.
     *
     *  @note Only a token that is not this lease's is refused here, since it would misroute a
     *      completion. Every other defect - a stale registration, a misaligned extent, an oversized
     *      transfer - is reported on that one request's completion, which is where a real ring
     *      reports it and where a batch of many keeps its other entries.
     */
    expected<std::size_t> submit(ring_owner_t const& owner, std::span<request_t const> requests) noexcept {
        owner_slot_t* const slot = slot_of_(owner);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The lease is not this ring's"};
        assert(slot->issuer == issuing_thread_identity() && "Only the thread holding a lease submits on it");
        for (request_t const& request : requests)
            if (request.token.owner() != owner.id() || request.token.generation() != owner.generation())
                return status_t {status_code_t::invalid_argument_k, "A request carries another lease's token"};

        std::size_t const room = slot->depth - (slot->pending_count + slot->ready_count);
        std::size_t const taking = requests.size() < room ? requests.size() : room;
        std::uint32_t const base = window_of_(owner.id());
        for (std::size_t index = 0; index != taking; ++index) {
            request_t const& request = requests[index];
            pending_[base + slot->pending_count + index] = request;
            borrow_(request);
        }
        slot->pending_count += static_cast<std::uint32_t>(taking);
        return taking;
    }

    /**
     *  @brief Performs what @p owner submitted and moves its completions into @p into, never waiting.
     *  @param[in] owner The lease reaping, held by the calling thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero when the owner submitted nothing.
     */
    expected<std::size_t> reap(ring_owner_t const& owner, std::span<completion_t> into) noexcept {
        owner_slot_t* const slot = slot_of_(owner);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The lease is not this ring's"};
        assert(slot->issuer == issuing_thread_identity() && "Only the thread holding a lease reaps on it");
        std::uint32_t const base = window_of_(owner.id());
        perform_(*slot, base);

        std::size_t const moving = into.size() < slot->ready_count ? into.size() : slot->ready_count;
        for (std::size_t index = 0; index != moving; ++index)
            into[index] = ready_[base + (slot->ready_first + index) % slot->depth];
        slot->ready_first = static_cast<std::uint32_t>((slot->ready_first + moving) % slot->depth);
        slot->ready_count -= static_cast<std::uint32_t>(moving);
        return moving;
    }

    /**
     *  @brief Stops @p owner's requests that have not started, which the next reap reports cancelled.
     *  @param[in] owner The lease to stop, whose started requests still run to their own end.
     */
    void cancel(ring_owner_t const& owner) noexcept {
        owner_slot_t* const slot = slot_of_(owner);
        if (!slot) return;
        cancelled_(*slot).store(1, std::memory_order_release);
    }

    /**
     *  @brief Finishes and discards everything @p owner has outstanding, so its buffers may be freed.
     *  @param[in] owner The lease to empty, which may be cancelled or still running.
     */
    status_t drain(ring_owner_t const& owner) noexcept {
        owner_slot_t* const slot = slot_of_(owner);
        if (!slot) return {status_code_t::invalid_argument_k, "The lease is not this ring's"};
        std::uint32_t const base = window_of_(owner.id());
        perform_(*slot, base);
        slot->ready_first = 0;
        slot->ready_count = 0;
        return {};
    }

#pragma endregion Ring

  private:

    /** One lease's share of the ring: its credit, its window and the flag that stops it. */
    struct owner_slot_t {

        /** Zero while the slot is free, one while a lease holds it. */
        std::int32_t leased = 0;

        /** Zero while the owner wants its requests, one once it has cancelled them. */
        std::int32_t cancelled = 0;

        /** Raised every time the slot is leased again, so a stale token routes nowhere. */
        std::uint16_t generation = 0;

        /** Requests this lease may keep in flight, fixed when the lease was taken. */
        std::uint32_t depth = 0;

        /** Requests submitted and not yet performed, at the front of the window. */
        std::uint32_t pending_count = 0;

        /** Where this owner's oldest unreaped completion sits inside its window. */
        std::uint32_t ready_first = 0;

        /** Completions performed and not yet reaped. */
        std::uint32_t ready_count = 0;

        /** The thread that took the lease, which every submit and reap is checked against. */
        std::uint64_t issuer = 0;
    };

    /** One pinned memory range, and the count of requests the device may still be writing into it. */
    struct registration_slot_t {

        /** Zero while the slot is free, one while a range is pinned in it. */
        std::int32_t registered = 0;

        /** Requests accepted against this range and not yet performed. */
        std::int32_t in_flight = 0;

        /** Raised every time the slot is used again, so a stale place resolves to nothing. */
        std::uint16_t generation = 0;

        /** Where the pinned memory lives. */
        memory_space_t space = memory_space_t::host_k;

        /** First byte of the pinned range, owned by the caller that registered it. */
        byte_t* base = nullptr;

        /** Bytes in the pinned range. */
        std::uint64_t length = 0;
    };

    /** One registered target, and the count of requests still naming it. */
    struct target_slot_t {

        /** Zero while the slot is free, one while a target is registered in it. */
        std::int32_t registered = 0;

        /** Requests accepted against this target and not yet performed. */
        std::int32_t in_flight = 0;

        /** Raised every time the slot is used again, so a stale place resolves to nothing. */
        std::uint16_t generation = 0;

        /** The transport's own handle for the target, which only the device interprets. */
        std::uint16_t index = 0;
    };

    basic_disk(device_type_&& device, disk_config_t const& config) noexcept
        : device_(std::move(device)), depth_limit_(config.depth_limit) {}

    /** Grows @p elements to @p count default slots, reporting an allocation failure as a status. */
    template <typename element_type_>
    static status_t size_(st::basic_vector<element_type_>& elements, std::size_t count) noexcept {
        if (elements.resize(count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A ring could not allocate its slots"};
        return {};
    }

    static fu::preferred_atomic_ref<std::int32_t> claim_(std::int32_t& word) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {word};
    }

    static fu::preferred_atomic_ref<std::int32_t> cancelled_(owner_slot_t& slot) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {slot.cancelled};
    }

    static fu::preferred_atomic_ref<std::int32_t> in_flight_(registration_slot_t& slot) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {slot.in_flight};
    }

    static fu::preferred_atomic_ref<std::int32_t> in_flight_(target_slot_t& slot) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {slot.in_flight};
    }

    /** Where one lease's requests and completions sit inside the shared windows. */
    [[nodiscard]] std::uint32_t window_of_(owner_id_t id) const noexcept {
        return static_cast<std::uint32_t>(id) * depth_limit_;
    }

    [[nodiscard]] owner_slot_t* slot_of_(ring_owner_t const& owner) noexcept {
        if (!owner.held() || static_cast<std::size_t>(owner.id()) >= owners_.size()) return nullptr;
        owner_slot_t& slot = owners_[static_cast<std::size_t>(owner.id())];
        return slot.leased != 0 && slot.generation == owner.generation() ? &slot : nullptr;
    }

    [[nodiscard]] owner_slot_t const* slot_of_(ring_owner_t const& owner) const noexcept {
        if (!owner.held() || static_cast<std::size_t>(owner.id()) >= owners_.size()) return nullptr;
        owner_slot_t const& slot = owners_[static_cast<std::size_t>(owner.id())];
        return slot.leased != 0 && slot.generation == owner.generation() ? &slot : nullptr;
    }

    [[nodiscard]] registration_slot_t* slot_of_(registration_t const& registration) noexcept {
        if (!registration.held() || static_cast<std::size_t>(registration.id()) >= registrations_.size())
            return nullptr;
        registration_slot_t& slot = registrations_[static_cast<std::size_t>(registration.id())];
        return slot.registered != 0 && slot.generation == registration.generation() ? &slot : nullptr;
    }

    [[nodiscard]] target_slot_t* slot_of_(descriptor_t const& descriptor) noexcept {
        if (!descriptor.held() || static_cast<std::size_t>(descriptor.id()) >= targets_.size()) return nullptr;
        target_slot_t& slot = targets_[static_cast<std::size_t>(descriptor.id())];
        return slot.registered != 0 && slot.generation == descriptor.generation() ? &slot : nullptr;
    }

    /** Marks the range and the target of @p request as live, so neither can be taken back under it. */
    void borrow_(request_t const& request) noexcept {
        if (registration_slot_t* const slot = resolve_(request.buffer); slot)
            in_flight_(*slot).add(1, std::memory_order_acquire);
        if (target_slot_t* const slot = resolve_(request.target); slot)
            in_flight_(*slot).add(1, std::memory_order_acquire);
    }

    /** Releases both once the device is done with the bytes of @p request. */
    void restore_(request_t const& request) noexcept {
        if (registration_slot_t* const slot = resolve_(request.buffer); slot)
            in_flight_(*slot).sub(1, std::memory_order_release);
        if (target_slot_t* const slot = resolve_(request.target); slot)
            in_flight_(*slot).sub(1, std::memory_order_release);
    }

    [[nodiscard]] registration_slot_t* resolve_(buffer_place_t place) noexcept {
        if (static_cast<std::size_t>(place.registration) >= registrations_.size()) return nullptr;
        registration_slot_t& slot = registrations_[static_cast<std::size_t>(place.registration)];
        return slot.registered != 0 && slot.generation == place.generation ? &slot : nullptr;
    }

    [[nodiscard]] target_slot_t* resolve_(target_place_t place) noexcept {
        if (static_cast<std::size_t>(place.descriptor) >= targets_.size()) return nullptr;
        target_slot_t& slot = targets_[static_cast<std::size_t>(place.descriptor)];
        return slot.registered != 0 && slot.generation == place.generation ? &slot : nullptr;
    }

    /** Where inside the shared completion window the @p index -th fresh completion of a lease goes. */
    [[nodiscard]] static std::size_t ready_place_(owner_slot_t const& slot, std::uint32_t base,
                                                  std::size_t index) noexcept {
        return base + (slot.ready_first + slot.ready_count + index) % slot.depth;
    }

    /** The completion a request gets when its owner cancelled it, or a chain ahead of it was cut. */
    [[nodiscard]] static completion_t cancelled_completion_(request_t const& request) noexcept {
        completion_t completion;
        completion.token = request.token;
        completion.state = completion_state_t::cancelled_k;
        completion.status = {status_code_t::uninitialized_k, "The owner cancelled this request"};
        return completion;
    }

    /** Resolves a request's buffer and performs it on the device, reporting every defect per request. */
    [[nodiscard]] completion_t perform_one_(request_t const& request) noexcept {
        completion_t completion;
        completion.token = request.token;
        target_slot_t const* const target = resolve_(request.target);
        if (!target) {
            completion.state = completion_state_t::failed_k;
            completion.status = {status_code_t::invalid_argument_k, "The request names no registered target"};
            return completion;
        }
        if (request.kind == request_kind_t::flush_k) {
            completion.status = device_.flush(target->index);
            completion.state = completion.status.failed() ? completion_state_t::failed_k
                                                          : completion_state_t::completed_k;
            return completion;
        }

        ring_capabilities_t const& limits = capabilities_;
        registration_slot_t const* const slot = resolve_(request.buffer);
        if (!slot) {
            completion.state = completion_state_t::failed_k;
            completion.status = {status_code_t::invalid_argument_k, "The request names no registered range"};
            return completion;
        }
        if (request.buffer.offset > slot->length || request.extent.length > slot->length - request.buffer.offset) {
            completion.state = completion_state_t::failed_k;
            completion.status = {status_code_t::invalid_argument_k, "The request runs past its registered range"};
            return completion;
        }
        byte_t* const address = slot->base + request.buffer.offset;
        if (!is_address_aligned(address, limits.memory_alignment) ||
            !request.extent.aligned_to(limits.offset_alignment) ||
            (request.extent.length % limits.length_granularity) != 0) {
            completion.state = completion_state_t::failed_k;
            completion.status = {status_code_t::invalid_argument_k, "The request does not meet the disk's alignment"};
            return completion;
        }
        if (limits.max_transfer_bytes != 0 && request.extent.length > limits.max_transfer_bytes) {
            completion.state = completion_state_t::failed_k;
            completion.status = {status_code_t::invalid_argument_k, "The request is larger than one transfer"};
            return completion;
        }

        std::span<byte_t> const buffer {address, static_cast<std::size_t>(request.extent.length)};
        expected<std::uint64_t> const moved = request.kind == request_kind_t::read_k
                                                  ? device_.read_extent(target->index, request.extent, buffer)
                                                  : device_.write_extent(target->index, request.extent, buffer);
        if (!moved) {
            completion.state = completion_state_t::failed_k;
            completion.status = moved.status();
            return completion;
        }
        completion.transferred = *moved;
        completion.state = *moved == request.extent.length ? completion_state_t::completed_k
                                                           : completion_state_t::partial_k;
        return completion;
    }

    /**
     *  @brief Performs everything one lease has submitted, and stages the completions for its reap.
     *
     *  Every request runs on the thread that reaps it, in submission order, so a chain gets the
     *  ordering it was promised and nothing here forks. Depth comes from leases rather than threads:
     *  a pool dispatch blocks until all its threads return and admits one caller at a time, so
     *  fanning a batch out here would serialize two owners against each other, and a batch issued
     *  from inside a background job would fork a pool that is already forked.
     */
    void perform_(owner_slot_t& slot, std::uint32_t base) noexcept {
        std::uint32_t const count = slot.pending_count;
        if (count == 0) return;

        bool const cancelled = cancelled_(slot).load(std::memory_order_acquire) != 0;
        bool cut = false;
        for (std::uint32_t index = 0; index != count; ++index) {
            request_t const& request = pending_[base + index];
            completion_t const completion = cancelled || cut ? cancelled_completion_(request) : perform_one_(request);
            ready_[ready_place_(slot, base, index)] = completion;
            restore_(request);
            cut = request.chaining == request_chaining_t::chained_k &&
                  (cut || completion.state != completion_state_t::completed_k);
        }

        slot.ready_count += count;
        slot.pending_count = 0;
    }

    /** The transport this disk moves bytes through, owned here. */
    device_type_ device_;

    /** What this disk accepts, read from the transport once at mount. */
    ring_capabilities_t capabilities_;

    /** Requests one lease may keep in flight, and the stride of every lease's window. */
    std::uint32_t depth_limit_ = 0;

    /** One slot per lease the ring hands out. */
    st::basic_vector<owner_slot_t> owners_;

    /** One slot per memory range the ring pins. */
    st::basic_vector<registration_slot_t> registrations_;

    /** One slot per target the ring registers. */
    st::basic_vector<target_slot_t> targets_;

    /** Every lease's submitted requests, in windows of @c depth_limit_. */
    st::basic_vector<request_t> pending_;

    /** Every lease's performed completions, in windows of @c depth_limit_ read as a circle. */
    st::basic_vector<completion_t> ready_;
};

#pragma endregion Disk

} // namespace unum::ustore::nvme
