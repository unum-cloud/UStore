/**
 *  @file src/nvme/disk.hpp
 *  @author Ash Vardanian
 *  @date September 18, 2026
 *  @brief The ring machinery every NVMe disk shares, over a device that moves positioned bytes.
 *
 *  A disk pairs a volume with a transport. The volume part - naming, allocation, the catalog - sits
 *  above; what lives here is the transport part every disk repeats: queue pairs, per-queue pair credit,
 *  registered memory, batching and completion routing. A transport supplies only the moves a device
 *  makes against a medium, and @c basic_disk turns them into the ring of @c ring.hpp.
 *
 *  Depth on the blocking floor comes from queue pairs rather than threads: a submitted batch waits in the
 *  queue pair's window and the queue pair's own @c poll performs it, so the queue pair pays for its own I/O on the
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

#include "ustore/status.hpp" // `status_t`, `status_code_t`, `expected`
#include "ustore/types.hpp"  // `byte_t`

#include "nvme/extent.hpp" // `extent_t`, `is_address_aligned`
#include "nvme/ring.hpp"   // `request_t`, `completion_t`, `queue_pair_t`, `registered_memory_t`, `registered_medium_t`

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
concept block_device_concept = requires(device_type_& device, device_type_ const& constant, medium_options_t medium,
                                        std::uint16_t index, extent_t extent, std::span<byte_t> destination,
                                        std::span<byte_t const> source) {
    { device.open_medium(medium) } -> std::same_as<expected<std::uint16_t>>;
    { device.close_medium(index) } -> std::same_as<status_t>;
    { device.read_extent(index, extent, destination) } -> std::same_as<expected<std::uint64_t>>;
    { device.write_extent(index, extent, source) } -> std::same_as<expected<std::uint64_t>>;
    { device.flush(index) } -> std::same_as<status_t>;
    { constant.capacity(index) } -> std::same_as<std::uint64_t>;
    { constant.capabilities() } -> std::same_as<ring_capabilities_t>;
};

/** Identifies the thread holding a queue pair, so a foreign bound thread trips an assertion and not a driver. */
[[nodiscard]] inline thread_identity_t calling_thread_identity() noexcept {
    return static_cast<std::uint64_t>(std::hash<std::thread::id> {}(std::this_thread::get_id()));
}

#pragma endregion Devices

#pragma region Configuration

/** What a mount fixes about a disk's ring before any queue pair queue pairs a share of it. */
struct disk_config_t {

    /** Leases the ring hands out at once, bounded on a real controller by its queue-pair count. */
    std::uint16_t queue_pairs_limit = 1;

    /** Requests one queue pair may keep in flight, which is the credit a queue pair spends and polls back. */
    queue_depth_t depth_limit = 64;

    /** Memory ranges the disk pins at once, which a transport registering one arena keeps at one. */
    std::uint16_t registered_memory_limit = 8;

    /** Targets the disk registers at once, which a namespace keeps at one and a directory raises. */
    std::uint16_t media_limit = 1;
};

#pragma endregion Configuration

#pragma region Disk

/**
 *  @brief Turns a positioned-I/O device into the submission and completion ring callers drive.
 *
 *  Depth is partitioned rather than pooled: every queue pair reserves @c depth_limit requests of its own,
 *  so one queue pair that stops polling cannot starve another, and a full ring is always the queue pair's own
 *  doing. Completions are routed by the queue pair bits of their token and never appear in another
 *  queue pair's poll.
 *
 *  Ordering between two submitted requests is never promised. Two writes may reach the medium in
 *  either order, and a flush covers only what the queue pair has already completed, so durability is written
 *  as submit, poll, flush, poll. A bounded sequence that must be ordered says so with
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
     *  @param[in] config Leases, per-queue pair depth, and the pinned ranges and media the ring holds.
     */
    static expected<basic_disk> make(device_type_&& device, disk_config_t const& config) noexcept {
        if (config.queue_pairs_limit == 0 || config.depth_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A ring needs a queue pair and a request of depth"};
        if (config.queue_pairs_limit == static_cast<std::uint16_t>(queue_pair_id_t::unallocated_k) ||
            config.registered_memory_limit == static_cast<std::uint16_t>(registered_memory_id_t::unregistered_k) ||
            config.media_limit == static_cast<std::uint16_t>(registered_medium_id_t::unregistered_k))
            return status_t {status_code_t::invalid_argument_k, "A ring keeps one identifier for an empty handle"};
        if (config.media_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A ring needs a medium to move bytes against"};

        std::size_t const window = static_cast<std::size_t>(config.queue_pairs_limit) * config.depth_limit;
        basic_disk disk {std::move(device), config};
        if (status_t const sized = size_(disk.queue_pairs_, config.queue_pairs_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.registered_memories_, config.registered_memory_limit); sized.failed())
            return sized;
        if (status_t const sized = size_(disk.media_, config.media_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.pending_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.ready_, window); sized.failed()) return sized;
        disk.capabilities_ = disk.device_.capabilities();
        disk.capabilities_.depth_limit = config.depth_limit;
        disk.capabilities_.queue_pairs_limit = config.queue_pairs_limit;
        disk.capabilities_.media_limit = config.media_limit;
        return disk;
    }

    /** What this disk accepts, fixed at mount so no request pays to ask the transport again. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept { return capabilities_; }

    /** The transport underneath, for a caller that needs what only the transport knows. */
    [[nodiscard]] device_type_ const& device() const noexcept { return device_; }

#pragma region Queue Pairs

    /**
     *  @brief Takes a queue pair on @p depth requests of the ring for the calling thread.
     *  @param[in] depth Requests this queue pair may keep in flight, at most the mount's limit.
     *  @return The queue pair, or @c capacity_exhausted_k when every queue pair is already out.
     */
    expected<queue_pair_t> acquire_queue_pair(queue_depth_t depth) noexcept {
        if (depth == 0 || depth > depth_limit_)
            return status_t {status_code_t::invalid_argument_k, "A queue pair asks for more depth than the ring has"};
        for (std::size_t index = 0; index != queue_pairs_.size(); ++index) {
            queue_pair_slot_t& slot = queue_pairs_[index];
            std::int32_t expected_free = 0;
            if (!claim_(slot.leased)
                     .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            slot.cancelled = 0;
            slot.depth = depth;
            slot.pending_count = 0;
            slot.ready_first = 0;
            slot.ready_count = 0;
            slot.bound_thread = calling_thread_identity();
            return queue_pair_t::leased(static_cast<queue_pair_id_t>(index), slot.generation, depth);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every queue pair of this ring is already held"};
    }

    /**
     *  @brief Binds @p queue pair to the calling thread, which is how a moved queue pair changes hands.
     *  @param[in] queue pair The queue pair this thread has just taken over, with nothing outstanding on it.
     *
     *  @note A queue pair with work in flight cannot be handed over, since the requests were issued from
     *      a thread that is no longer the one that would poll them.
     */
    status_t bind_queue_pair(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        if (slot->pending_count != 0 || slot->ready_count != 0)
            return {status_code_t::invalid_argument_k, "Drain the queue pair before handing it over"};
        slot->bound_thread = calling_thread_identity();
        return {};
    }

    /**
     *  @brief Takes @p queue pair back once it has nothing outstanding, and empties the handle.
     *  @param[inout] queue pair The queue pair to return, surrendered on success and untouched on failure.
     */
    status_t release_queue_pair(queue_pair_t& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        if (slot->pending_count != 0 || slot->ready_count != 0)
            return {status_code_t::invalid_argument_k, "Drain the queue pair before releasing it"};
        ++slot->generation;
        slot->depth = 0;
        slot->bound_thread = 0;
        claim_(slot->leased).store(0, std::memory_order_release);
        queue_pair.surrender();
        return {};
    }

    /** Requests @p queue pair has submitted and not yet completed, which its credit bounds. */
    [[nodiscard]] std::size_t outstanding(queue_pair_t const& queue_pair) const noexcept {
        queue_pair_slot_t const* const slot = slot_of_(queue_pair);
        return slot ? slot->pending_count + slot->ready_count : 0;
    }

#pragma endregion Queue Pairs

#pragma region Registrations

    /**
     *  @brief Pins @p memory so requests can address it, and says whether this transport reaches it.
     *  @param[in] memory Memory the caller owns and keeps alive until it deregisters the range.
     *  @param[in] space Where that memory lives; a disk without GPUDirect refuses @c gpu_k here.
     */
    expected<registered_memory_t> register_memory(std::span<byte_t> memory, memory_space_t space) noexcept {
        if (memory.empty()) return status_t {status_code_t::invalid_argument_k, "An empty range registers nothing"};
        if (space == memory_space_t::gpu_k && !device_.capabilities().reaches_gpu_memory)
            return status_t {status_code_t::unsupported_k, "This transport cannot address device memory"};
        for (std::size_t index = 0; index != registered_memories_.size(); ++index) {
            registered_memory_slot_t& slot = registered_memories_[index];
            std::int32_t expected_free = 0;
            if (!claim_(slot.registered)
                     .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            slot.base = memory.data();
            slot.length = memory.size();
            slot.space = space;
            return registered_memory_t::registered(static_cast<registered_memory_id_t>(index), slot.generation, memory,
                                                   space);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every registered_memory slot of this ring is taken"};
    }

    /**
     *  @brief Unpins @p registered memory, refusing while it is still a live medium of the device.
     *  @param[inout] registered memory The range to give back, surrendered on success.
     */
    status_t unregister_memory(registered_memory_t& registered_memory) noexcept {
        registered_memory_slot_t* const slot = slot_of_(registered_memory);
        if (!slot) return {status_code_t::invalid_argument_k, "The registered_memory is not this ring's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "The range is still a medium of requests in flight"};
        ++slot->generation;
        slot->base = nullptr;
        slot->length = 0;
        claim_(slot->registered).store(0, std::memory_order_release);
        registered_memory.surrender();
        return {};
    }

    /**
     *  @brief Registers the medium @p request names, so requests can address it by slot.
     *  @param[in] request The path, label or namespace to attach, and what the caller may do to it.
     *  @return The handle, or @c capacity_exhausted_k when every medium slot is taken.
     */
    expected<registered_medium_t> register_medium(medium_options_t const& request) noexcept {
        for (std::size_t index = 0; index != media_.size(); ++index) {
            registered_medium_slot_t& slot = media_[index];
            std::int32_t expected_free = 0;
            if (!claim_(slot.registered)
                     .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            auto [attached, open_medium_status] = device_.open_medium(request);
            if (open_medium_status.failed()) {
                claim_(slot.registered).store(0, std::memory_order_release);
                return open_medium_status;
            }
            slot.index = attached;
            return registered_medium_t::registered(static_cast<registered_medium_id_t>(index), slot.generation,
                                                   device_.capacity(slot.index));
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every medium slot of this ring is taken"};
    }

    /**
     *  @brief Releases @p registered medium, refusing while requests against it are still in flight.
     *  @param[inout] registered medium The medium to give back, surrendered on success.
     */
    status_t unregister_medium(registered_medium_t& registered_medium) noexcept {
        registered_medium_slot_t* const slot = slot_of_(registered_medium);
        if (!slot) return {status_code_t::invalid_argument_k, "The medium is not this ring's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "The medium is still named by requests in flight"};
        if (status_t const detached = device_.close_medium(slot->index); detached.failed()) return detached;
        ++slot->generation;
        slot->index = 0;
        claim_(slot->registered).store(0, std::memory_order_release);
        registered_medium.surrender();
        return {};
    }

#pragma endregion Registrations

#pragma region Ring

    /**
     *  @brief Takes as much of @p requests as @p queue pair still has credit for, and never waits.
     *  @param[in] queue pair The queue pair submitting, held by the calling thread.
     *  @param[in] requests The batch, of which a prefix is taken.
     *  @return How many were taken, which is zero on a full ring and never a reason to block.
     *
     *  @note Only a token that is not this queue pair's is refused here, since it would misroute a
     *      completion. Every other defect - a stale registered memory, a misaligned extent, an oversized
     *      transfer - is reported on that one request's completion, which is where a real ring
     *      reports it and where a batch of many keeps its other entries.
     */
    expected<std::size_t> submit(queue_pair_t const& queue_pair, std::span<request_t const> requests) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        assert(slot->bound_thread == calling_thread_identity() && "Only the thread holding a queue pair submits on it");
        for (request_t const& request : requests)
            if (request.token.queue_pair() != queue_pair.id() || request.token.generation() != queue_pair.generation())
                return status_t {status_code_t::invalid_argument_k, "A request carries another queue pair's token"};

        std::size_t const room = slot->depth - (slot->pending_count + slot->ready_count);
        std::size_t const taking = requests.size() < room ? requests.size() : room;
        std::uint32_t const base = window_of_(queue_pair.id());
        for (std::size_t index = 0; index != taking; ++index) {
            request_t const& request = requests[index];
            pending_[base + slot->pending_count + index] = request;
            borrow_(request);
        }
        slot->pending_count += static_cast<std::uint32_t>(taking);
        return taking;
    }

    /**
     *  @brief Performs what @p queue pair submitted and moves its completions into @p into, never waiting.
     *  @param[in] queue pair The queue pair polling, held by the calling thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero when the queue pair submitted nothing.
     */
    expected<std::size_t> poll_completions(queue_pair_t const& queue_pair, std::span<completion_t> into) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        assert(slot->bound_thread == calling_thread_identity() && "Only the thread holding a queue pair polls on it");
        std::uint32_t const base = window_of_(queue_pair.id());
        perform_(*slot, base);

        std::size_t const moving = into.size() < slot->ready_count ? into.size() : slot->ready_count;
        for (std::size_t index = 0; index != moving; ++index)
            into[index] = ready_[base + (slot->ready_first + index) % slot->depth];
        slot->ready_first = static_cast<std::uint32_t>((slot->ready_first + moving) % slot->depth);
        slot->ready_count -= static_cast<std::uint32_t>(moving);
        return moving;
    }

    /**
     *  @brief Stops @p queue pair's requests that have not started, which the next poll reports cancelled.
     *  @param[in] queue pair The queue pair to stop, whose started requests still run to their own end.
     */
    void cancel(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return;
        cancelled_(*slot).store(1, std::memory_order_release);
    }

    /**
     *  @brief Finishes and discards everything @p queue pair has outstanding, so its buffers may be freed.
     *  @param[in] queue pair The queue pair to empty, which may be cancelled or still running.
     */
    status_t drain(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        std::uint32_t const base = window_of_(queue_pair.id());
        perform_(*slot, base);
        slot->ready_first = 0;
        slot->ready_count = 0;
        return {};
    }

#pragma endregion Ring

  private:

    /** One queue pair's share of the ring: its credit, its window and the flag that stops it. */
    struct queue_pair_slot_t {

        /** Zero while the slot is free, one while a queue pair holds it. */
        std::int32_t leased = 0;

        /** Zero while the queue pair wants its requests, one once it has cancelled them. */
        std::int32_t cancelled = 0;

        /** Raised every time the slot is leased again, so a stale token routes nowhere. */
        queue_pair_generation_t generation = 0;

        /** Requests this queue pair may keep in flight, fixed when the queue pair was taken. */
        queue_depth_t depth = 0;

        /** Requests submitted and not yet performed, at the front of the window. */
        std::uint32_t pending_count = 0;

        /** Where this queue pair's oldest unreaped completion sits inside its window. */
        std::uint32_t ready_first = 0;

        /** Completions performed and not yet completed. */
        std::uint32_t ready_count = 0;

        /** The thread that took the queue pair, which every submit and poll is checked against. */
        thread_identity_t bound_thread = 0;
    };

    /** One pinned memory range, and the count of requests the device may still be writing into it. */
    struct registered_memory_slot_t {

        /** Zero while the slot is free, one while a range is pinned in it. */
        std::int32_t registered = 0;

        /** Requests accepted against this range and not yet performed. */
        std::int32_t in_flight = 0;

        /** Raised every time the slot is used again, so a stale place resolves to nothing. */
        queue_pair_generation_t generation = 0;

        /** Where the pinned memory lives. */
        memory_space_t space = memory_space_t::host_k;

        /** First byte of the pinned range, owned by the caller that registered it. */
        byte_t* base = nullptr;

        /** Bytes in the pinned range. */
        std::uint64_t length = 0;
    };

    /** One registered medium, and the count of requests still naming it. */
    struct registered_medium_slot_t {

        /** Zero while the slot is free, one while a medium is registered in it. */
        std::int32_t registered = 0;

        /** Requests accepted against this medium and not yet performed. */
        std::int32_t in_flight = 0;

        /** Raised every time the slot is used again, so a stale place resolves to nothing. */
        queue_pair_generation_t generation = 0;

        /** The transport's own handle for the medium, which only the device interprets. */
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

    static fu::preferred_atomic_ref<std::int32_t> cancelled_(queue_pair_slot_t& slot) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {slot.cancelled};
    }

    static fu::preferred_atomic_ref<std::int32_t> in_flight_(registered_memory_slot_t& slot) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {slot.in_flight};
    }

    static fu::preferred_atomic_ref<std::int32_t> in_flight_(registered_medium_slot_t& slot) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {slot.in_flight};
    }

    /** Where one queue pair's requests and completions sit inside the shared windows. */
    [[nodiscard]] std::uint32_t window_of_(queue_pair_id_t id) const noexcept {
        return static_cast<std::uint32_t>(id) * depth_limit_;
    }

    [[nodiscard]] queue_pair_slot_t* slot_of_(queue_pair_t const& queue_pair) noexcept {
        if (!queue_pair.held() || static_cast<std::size_t>(queue_pair.id()) >= queue_pairs_.size()) return nullptr;
        queue_pair_slot_t& slot = queue_pairs_[static_cast<std::size_t>(queue_pair.id())];
        return slot.leased != 0 && slot.generation == queue_pair.generation() ? &slot : nullptr;
    }

    [[nodiscard]] queue_pair_slot_t const* slot_of_(queue_pair_t const& queue_pair) const noexcept {
        if (!queue_pair.held() || static_cast<std::size_t>(queue_pair.id()) >= queue_pairs_.size()) return nullptr;
        queue_pair_slot_t const& slot = queue_pairs_[static_cast<std::size_t>(queue_pair.id())];
        return slot.leased != 0 && slot.generation == queue_pair.generation() ? &slot : nullptr;
    }

    [[nodiscard]] registered_memory_slot_t* slot_of_(registered_memory_t const& registered_memory) noexcept {
        if (!registered_memory.held() ||
            static_cast<std::size_t>(registered_memory.id()) >= registered_memories_.size())
            return nullptr;
        registered_memory_slot_t& slot = registered_memories_[static_cast<std::size_t>(registered_memory.id())];
        return slot.registered != 0 && slot.generation == registered_memory.generation() ? &slot : nullptr;
    }

    [[nodiscard]] registered_medium_slot_t* slot_of_(registered_medium_t const& registered_medium) noexcept {
        if (!registered_medium.held() || static_cast<std::size_t>(registered_medium.id()) >= media_.size())
            return nullptr;
        registered_medium_slot_t& slot = media_[static_cast<std::size_t>(registered_medium.id())];
        return slot.registered != 0 && slot.generation == registered_medium.generation() ? &slot : nullptr;
    }

    /** Marks the range and the medium of @p request as live, so neither can be taken back under it. */
    void borrow_(request_t const& request) noexcept {
        if (registered_memory_slot_t* const slot = resolve_(request.buffer); slot)
            in_flight_(*slot).add(1, std::memory_order_acquire);
        if (registered_medium_slot_t* const slot = resolve_(request.medium); slot)
            in_flight_(*slot).add(1, std::memory_order_acquire);
    }

    /** Releases both once the device is done with the bytes of @p request. */
    void restore_(request_t const& request) noexcept {
        if (registered_memory_slot_t* const slot = resolve_(request.buffer); slot)
            in_flight_(*slot).sub(1, std::memory_order_release);
        if (registered_medium_slot_t* const slot = resolve_(request.medium); slot)
            in_flight_(*slot).sub(1, std::memory_order_release);
    }

    [[nodiscard]] registered_memory_slot_t* resolve_(memory_reference_t place) noexcept {
        if (static_cast<std::size_t>(place.memory_id) >= registered_memories_.size()) return nullptr;
        registered_memory_slot_t& slot = registered_memories_[static_cast<std::size_t>(place.memory_id)];
        return slot.registered != 0 && slot.generation == place.generation ? &slot : nullptr;
    }

    [[nodiscard]] registered_medium_slot_t* resolve_(medium_reference_t place) noexcept {
        if (static_cast<std::size_t>(place.medium_id) >= media_.size()) return nullptr;
        registered_medium_slot_t& slot = media_[static_cast<std::size_t>(place.medium_id)];
        return slot.registered != 0 && slot.generation == place.generation ? &slot : nullptr;
    }

    /** Where inside the shared completion window the @p index -th fresh completion of a queue pair goes. */
    [[nodiscard]] static std::size_t ready_place_(queue_pair_slot_t const& slot, std::uint32_t base,
                                                  std::size_t index) noexcept {
        return base + (slot.ready_first + slot.ready_count + index) % slot.depth;
    }

    /** The completion a request gets when its queue pair cancelled it, or a chain ahead of it was cut. */
    [[nodiscard]] static completion_t cancelled_completion_(request_t const& request) noexcept {
        completion_t completion;
        completion.token = request.token;
        completion.state = completion_state_t::cancelled_k;
        completion.status = {status_code_t::uninitialized_k, "The queue_pair cancelled this request"};
        return completion;
    }

    /** Resolves a request's buffer and performs it on the device, reporting every defect per request. */
    [[nodiscard]] completion_t perform_one_(request_t const& request) noexcept {
        completion_t completion;
        completion.token = request.token;
        registered_medium_slot_t const* const medium = resolve_(request.medium);
        if (!medium) {
            completion.state = completion_state_t::failed_k;
            completion.status = {status_code_t::invalid_argument_k, "The request names no registered medium"};
            return completion;
        }
        if (request.kind == request_kind_t::flush_k) {
            completion.status = device_.flush(medium->index);
            completion.state = completion.status.failed() ? completion_state_t::failed_k
                                                          : completion_state_t::completed_k;
            return completion;
        }

        ring_capabilities_t const& limits = capabilities_;
        registered_memory_slot_t const* const slot = resolve_(request.buffer);
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
        auto [moved, move_status] = request.kind == request_kind_t::read_k
                                        ? device_.read_extent(medium->index, request.extent, buffer)
                                        : device_.write_extent(medium->index, request.extent, buffer);
        if (move_status.failed()) {
            completion.state = completion_state_t::failed_k;
            completion.status = move_status;
            return completion;
        }
        completion.transferred_bytes = moved;
        completion.state = moved == request.extent.length ? completion_state_t::completed_k
                                                          : completion_state_t::partial_k;
        return completion;
    }

    /**
     *  @brief Performs everything one queue pair has submitted, and stages the completions for its poll.
     *
     *  Every request runs on the thread that polls it, in submission order, so a chain gets the
     *  ordering it was promised and nothing here forks. Depth comes from queue pairs rather than threads:
     *  a pool dispatch blocks until all its threads return and admits one caller at a time, so
     *  fanning a batch out here would serialize two queue pairs against each other, and a batch issued
     *  from inside a background job would fork a pool that is already forked.
     */
    void perform_(queue_pair_slot_t& slot, std::uint32_t base) noexcept {
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

    /** Requests one queue pair may keep in flight, and the stride of every queue pair's window. */
    queue_depth_t depth_limit_ = 0;

    /** One slot per queue pair the ring hands out. */
    st::basic_vector<queue_pair_slot_t> queue_pairs_;

    /** One slot per memory range the ring pins. */
    st::basic_vector<registered_memory_slot_t> registered_memories_;

    /** One slot per medium the ring registers. */
    st::basic_vector<registered_medium_slot_t> media_;

    /** Every queue pair's submitted requests, in windows of @c depth_limit_. */
    st::basic_vector<request_t> pending_;

    /** Every queue pair's performed completions, in windows of @c depth_limit_ read as a circle. */
    st::basic_vector<completion_t> ready_;
};

#pragma endregion Disk

} // namespace unum::ustore::nvme
