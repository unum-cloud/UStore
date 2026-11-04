/**
 *  @file include/ustore/utilities/nvme_disks.hpp
 *  @author Ash Vardanian
 *  @date September 18, 2026
 *  @brief The ring machinery every NVMe disk shares, the volume in memory, and the portable floor of positioned
 *      reads and writes on files.
 *
 *  A disk pairs a volume with a transport. The volume part - naming, allocation, the catalog - sits
 *  above; what lives here is the transport part every disk repeats: queue pairs, per-queue pair credit,
 *  registered memory, batching and completion routing. A transport supplies only the moves a device
 *  makes against a medium, and @c basic_disk turns them into the ring of @c nvme_ring.hpp.
 *
 *  Depth on the blocking floor comes from queue pairs rather than threads: a submitted batch
 *  waits in the queue pair's window and the queue pair's own @c poll performs it, so the queue
 *  pair pays for its own I/O on the thread that asked for it. Nothing is parked, nothing is
 *  woken, and nothing forks.
 *
 *  The memory disk keeps a volume in memory behind the same ring, so a suite exercises the contract
 *  without a device. The blocking floor is the transport every platform has. POSIX issues @c pread
 *  and @c pwrite, Windows issues @c ReadFile and @c WriteFile with an @c OVERLAPPED offset, and
 *  neither keeps a file cursor, so the same registered medium serves every executor thread at once.
 *  Depth comes from those threads rather than from a device queue, which is why the floor is a
 *  floor and not a medium.
 */

#pragma once

#include <cerrno>  // `errno`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include "ustore/types.h" // `USTORE_OS_APPLE_`, `USTORE_OS_LINUX_`, `USTORE_OS_WINDOWS_`

#if USTORE_OS_WINDOWS_
#include <windows.h>  // `ReadFile`, `WriteFile`, `OVERLAPPED`
#include <winioctl.h> // `FSCTL_SET_ZERO_DATA`, `FILE_ZERO_DATA_INFORMATION`
#else
#include <fcntl.h>    // `open`, `O_RDONLY`, `O_DIRECT`, `fallocate`, `AT_EMPTY_PATH`
#include <sys/stat.h> // `fstat`, `S_ISBLK`
#include <unistd.h>   // `pread`, `pwrite`, `ftruncate`, `close`, `syscall`
#endif
#if USTORE_OS_LINUX_
#include <linux/falloc.h> // `FALLOC_FL_PUNCH_HOLE`, `FALLOC_FL_KEEP_SIZE`
#include <linux/fs.h>     // `BLKGETSIZE64`, `BLKDISCARD`
#include <linux/stat.h>   // `struct statx`, `STATX_DIOALIGN`
#include <sys/ioctl.h>    // `ioctl`
#include <sys/syscall.h>  // `__NR_statx`
#endif

#include <bit>      // `std::has_single_bit`
#include <concepts> // `std::same_as`
#include <limits>   // `std::numeric_limits`
#include <span>     // `std::span`
#include <utility>  // `std::move`

#include <smashtable/basic_slot_pool.hpp> // `st::basic_slot_pool`
#include <smashtable/basic_vector.hpp>    // `st::vector`

#include "ustore/utilities/nvme_ring.hpp" // `request_t`, `completion_t`, `queue_pair_t`, `extent_t`, `is_address_aligned`
#include "ustore/utilities/ram_buffers.hpp" // `aligned_allocator`, `constrained_allocator`, `resource_constraint`
#include "ustore/utilities/types.hpp"       // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::utilities {

#pragma region Disk

#pragma region Devices

/**
 *  @brief The moves a transport implements, from which @c basic_disk builds a whole ring.
 *
 *  Every call is synchronous and thread-safe for distinct extents, which positioned reads and
 *  writes are on every platform. Media sit in a table sized once by @c reserve_media and filled at
 *  the slot the disk names, so registering one never moves the table under a reader of another.
 *  Asynchrony is the ring's business, not the device's, so a device that already has a queue of its
 *  own implements the ring directly instead of satisfying this.
 */
template <typename device_type_>
concept block_device_concept = requires(device_type_& device, device_type_ const& constant, medium_options_t medium,
                                        std::uint16_t index, std::size_t count, extent_t extent,
                                        std::span<byte_t> destination, std::span<byte_t const> source) {
    { device.reserve_media(count) } -> std::same_as<status_t>;
    { device.open_medium(index, medium) } -> std::same_as<status_t>;
    { device.close_medium(index) } -> std::same_as<status_t>;
    { device.read_extent(index, extent, destination) } -> std::same_as<expected<std::uint64_t>>;
    { device.write_extent(index, extent, source) } -> std::same_as<expected<std::uint64_t>>;
    { device.flush(index) } -> std::same_as<status_t>;
    { device.deallocate_extent(index, extent) } -> std::same_as<status_t>;
    { constant.capacity(index) } -> std::same_as<std::uint64_t>;
    { constant.logical_block_bytes(index) } -> std::same_as<std::uint64_t>;
    { constant.capabilities() } -> std::same_as<ring_capabilities_t>;
};

/** The thread index a caller outside every executor reads with, past any executor's threads, so it shares the
 *  channels and the epoch slots threads of no executor take. */
inline constexpr std::size_t outside_executor_k = std::numeric_limits<std::size_t>::max();

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
 *  Depth is partitioned rather than pooled: every queue pair reserves @c depth_limit requests of
 *  its own, so one queue pair that stops polling cannot starve another, and a full ring is always
 *  the queue pair's own doing. Completions are routed by the queue pair bits of their token and
 *  never appear in another queue pair's poll.
 *
 *  Ordering between two submitted requests is never promised. Two writes may reach the medium in
 *  either order, and a flush covers only what the queue pair has already completed, so
 *  durability is written as submit, poll, flush, poll. A bounded sequence that must be ordered
 *  says so with @c request_chaining_t::chained_k, which runs the chain in submission order and
 *  cuts it with @c cancelled_k at the first short or failed result.
 *
 *  @tparam atomic_reference_ The reference the claim, cancellation and in-flight words are owned through.
 *  @tparam waiter_type_ What a core does while another thread takes or vacates a record of
 *      the same pool.
 */
template <typename device_type_, template <typename> typename atomic_reference_, typename waiter_type_>
    requires block_device_concept<device_type_>
class basic_disk {
  public:

    /** The budget the disk's pools are charged to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    basic_disk(basic_disk const&) = delete;
    basic_disk& operator=(basic_disk const&) = delete;
    basic_disk(basic_disk&&) noexcept = default;
    basic_disk& operator=(basic_disk&&) noexcept = default;

    /**
     *  @brief Mounts @p device behind a ring shaped by @p config.
     *  @param[in] device The transport, moved in and owned by the disk from here on.
     *  @param[in] config Leases, per-queue pair depth, and the pinned ranges and media the ring holds.
     *  @param[in,out] constraint The budget the queue pair, range and medium pools are charged to,
     *      outliving the disk.
     */
    static expected<basic_disk> make(device_type_&& device, disk_config_t const& config,
                                     constraint_t& constraint) noexcept {
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
        allocator_t const allocator {constraint};
        if (status_t const made = make_pool_(disk.queue_pairs_, config.queue_pairs_limit, allocator); made.failed())
            return made;
        if (status_t const made = make_pool_(disk.registered_memories_, config.registered_memory_limit, allocator);
            made.failed())
            return made;
        if (status_t const made = make_pool_(disk.media_, config.media_limit, allocator); made.failed()) return made;
        if (status_t const reserved = disk.device_.reserve_media(config.media_limit); reserved.failed())
            return reserved;
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
        std::uint16_t const index = take_(queue_pairs_, queue_pairs_claim_);
        if (index == queue_pair_pool_t::absent_k)
            return status_t {status_code_t::capacity_exhausted_k, "Every queue pair of this ring is already held"};
        queue_pair_record_t& record = queue_pairs_[index];
        record.cancelled = 0;
        record.depth = depth;
        record.pending_count = 0;
        record.ready_first = 0;
        record.ready_count = 0;
        return queue_pair_t {handle_of_(queue_pairs_, index), depth};
    }

    /**
     *  @brief Binds @p queue pair to the calling thread, which is how a moved queue pair changes hands.
     *  @param[in] queue_pair The queue pair this thread has just taken over, with nothing outstanding on it.
     *
     *  @note A queue pair with work in flight cannot be handed over, since the requests were issued from
     *      a thread that is no longer the one that would poll them.
     */
    status_t bind_queue_pair(queue_pair_t const& queue_pair) noexcept {
        queue_pair_record_t* const record = record_of_(queue_pair);
        if (!record) return {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        if (record->pending_count != 0 || record->ready_count != 0)
            return {status_code_t::invalid_argument_k, "Drain the queue pair before handing it over"};
        return {};
    }

    /**
     *  @brief Takes @p queue pair back once it has nothing outstanding, and empties the handle.
     *  @param[in,out] queue_pair The queue pair to return, surrendered on success and untouched on failure.
     */
    status_t release_queue_pair(queue_pair_t& queue_pair) noexcept {
        queue_pair_record_t* const record = record_of_(queue_pair);
        if (!record) return {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        if (record->pending_count != 0 || record->ready_count != 0)
            return {status_code_t::invalid_argument_k, "Drain the queue pair before releasing it"};
        record->depth = 0;
        vacate_(queue_pairs_, queue_pairs_claim_, static_cast<std::uint16_t>(queue_pair.id()));
        queue_pair.surrender();
        return {};
    }

    /** Requests @p queue pair has submitted and not yet completed, which its credit bounds. */
    [[nodiscard]] std::size_t outstanding(queue_pair_t const& queue_pair) const noexcept {
        queue_pair_record_t const* const record = record_of_(queue_pair);
        return record ? record->pending_count + record->ready_count : 0;
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
        std::uint16_t const index = take_(registered_memories_, registered_memories_claim_);
        if (index == registered_memory_pool_t::absent_k)
            return status_t {status_code_t::capacity_exhausted_k, "Every registered_memory slot of this ring is taken"};
        registered_memory_record_t& record = registered_memories_[index];
        record.base = memory.data();
        record.length = memory.size();
        record.space = space;
        return registered_memory_t {handle_of_(registered_memories_, index), memory, space};
    }

    /**
     *  @brief Unpins @p registered memory, refusing while it is still a live medium of the device.
     *  @param[in,out] registered_memory The range to give back, surrendered on success.
     */
    status_t unregister_memory(registered_memory_t& registered_memory) noexcept {
        registered_memory_record_t* const record = resolve_(registered_memory.reference());
        if (!record) return {status_code_t::invalid_argument_k, "The registered_memory is not this ring's"};
        if (in_flight_(*record).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "The range is still a medium of requests in flight"};
        record->base = nullptr;
        record->length = 0;
        vacate_(registered_memories_, registered_memories_claim_, static_cast<std::uint16_t>(registered_memory.id()));
        registered_memory.surrender();
        return {};
    }

    /**
     *  @brief Registers the medium @p request names, so requests can address it by slot.
     *  @param[in] request The path, label or namespace to attach, and what the caller may do to it.
     *  @return The handle, or @c capacity_exhausted_k when every medium slot is taken.
     */
    expected<registered_medium_t> register_medium(medium_options_t const& request) noexcept {
        std::uint16_t const index = take_(media_, media_claim_);
        if (index == registered_medium_pool_t::absent_k)
            return status_t {status_code_t::capacity_exhausted_k, "Every medium slot of this ring is taken"};
        if (status_t const opened = device_.open_medium(index, request); opened.failed()) {
            vacate_(media_, media_claim_, index);
            return opened;
        }
        return registered_medium_t {handle_of_(media_, index), device_.capacity(index),
                                    device_.logical_block_bytes(index)};
    }

    /**
     *  @brief Releases @p registered medium, refusing while requests against it are still in flight.
     *  @param[in,out] registered_medium The medium to give back, surrendered on success.
     */
    status_t unregister_medium(registered_medium_t& registered_medium) noexcept {
        registered_medium_record_t* const record = resolve_(registered_medium.reference());
        if (!record) return {status_code_t::invalid_argument_k, "The medium is not this ring's"};
        if (in_flight_(*record).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "The medium is still named by requests in flight"};
        std::uint16_t const index = static_cast<std::uint16_t>(registered_medium.id());
        if (status_t const detached = device_.close_medium(index); detached.failed()) return detached;
        vacate_(media_, media_claim_, index);
        registered_medium.surrender();
        return {};
    }

#pragma endregion Registrations

#pragma region Ring

    /**
     *  @brief Takes as much of @p requests as @p queue pair still has credit for, and never waits.
     *  @param[in] queue_pair The queue pair submitting, held by the calling thread.
     *  @param[in] requests The batch, of which a prefix is taken.
     *  @return How many were taken, which is zero on a full ring and never a reason to block.
     *
     *  @note Only a token that is not this queue pair's is refused here, since it would misroute a
     *      completion. Every other defect - a stale registered memory, a misaligned extent, an
     *      oversized transfer - is reported on that one request's completion, which is where a real
     *      ring reports it and where a batch of many keeps its other entries.
     */
    expected<std::size_t> submit(queue_pair_t const& queue_pair, std::span<request_t const> requests) noexcept {
        queue_pair_record_t* const record = record_of_(queue_pair);
        if (!record) return status_t {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        for (request_t const& request : requests)
            if (request.token.queue_pair() != queue_pair.id() || request.token.generation() != queue_pair.generation())
                return status_t {status_code_t::invalid_argument_k, "A request carries another queue pair's token"};

        std::size_t const room = record->depth - (record->pending_count + record->ready_count);
        std::size_t const taking = requests.size() < room ? requests.size() : room;
        std::uint32_t const base = window_of_(queue_pair.id());
        for (std::size_t index = 0; index != taking; ++index) {
            request_t const& request = requests[index];
            pending_[base + record->pending_count + index] = request;
            borrow_(request);
        }
        record->pending_count += static_cast<std::uint32_t>(taking);
        return taking;
    }

    /**
     *  @brief Performs what @p queue pair submitted and moves its completions into @p into, never waiting.
     *  @param[in] queue_pair The queue pair polling, held by the calling thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero when the queue pair submitted nothing.
     */
    expected<std::size_t> poll_completions(queue_pair_t const& queue_pair, std::span<completion_t> into) noexcept {
        queue_pair_record_t* const record = record_of_(queue_pair);
        if (!record) return status_t {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        std::uint32_t const base = window_of_(queue_pair.id());
        perform_(*record, base);

        std::size_t const moving = into.size() < record->ready_count ? into.size() : record->ready_count;
        for (std::size_t index = 0; index != moving; ++index)
            into[index] = ready_[base + (record->ready_first + index) % record->depth];
        record->ready_first = static_cast<std::uint32_t>((record->ready_first + moving) % record->depth);
        record->ready_count -= static_cast<std::uint32_t>(moving);
        return moving;
    }

    /**
     *  @brief Stops @p queue pair's requests that have not started, which the next poll reports cancelled.
     *  @param[in] queue_pair The queue pair to stop, whose started requests still run to their own end.
     */
    void cancel(queue_pair_t const& queue_pair) noexcept {
        queue_pair_record_t* const record = record_of_(queue_pair);
        if (!record) return;
        cancelled_(*record).store(1, std::memory_order_release);
    }

    /**
     *  @brief Finishes and discards everything @p queue pair has outstanding, so its buffers may be freed.
     *  @param[in] queue_pair The queue pair to empty, which may be cancelled or still running.
     */
    status_t drain(queue_pair_t const& queue_pair) noexcept {
        queue_pair_record_t* const record = record_of_(queue_pair);
        if (!record) return {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        std::uint32_t const base = window_of_(queue_pair.id());
        perform_(*record, base);
        record->ready_first = 0;
        record->ready_count = 0;
        return {};
    }

#pragma endregion Ring

  private:

    /** One queue pair's share of the ring: its credit, its window and the word that stops it. */
    struct queue_pair_record_t {

        /** Stepped every time the record is vacated, so a stale token routes nowhere. */
        queue_pair_generation_t generation = 0;

        /** Zero while the queue pair wants its requests, one once it has cancelled them. */
        std::int32_t cancelled = 0;

        /** Requests this queue pair may keep in flight, fixed when the queue pair was taken. */
        queue_depth_t depth = 0;

        /** Requests submitted and not yet performed, at the front of the window. */
        std::uint32_t pending_count = 0;

        /** Where this queue pair's oldest unreaped completion sits inside its window. */
        std::uint32_t ready_first = 0;

        /** Completions performed and not yet completed. */
        std::uint32_t ready_count = 0;
    };

    /** One pinned memory range, and the count of requests the device may still be writing into it. */
    struct registered_memory_record_t {

        /** Stepped every time the record is vacated, so a stale place resolves to nothing. */
        queue_pair_generation_t generation = 0;

        /** Where the pinned memory lives. */
        memory_space_t space = memory_space_t::host_k;

        /** Requests accepted against this range and not yet performed. */
        std::int32_t in_flight = 0;

        /** First byte of the pinned range, owned by the caller that registered it. */
        byte_t* base = nullptr;

        /** Bytes in the pinned range. */
        std::uint64_t length = 0;
    };

    /** One registered medium, whose index is also the device's slot for it, and the count of
     *  requests naming it. */
    struct registered_medium_record_t {

        /** Stepped every time the record is vacated, so a stale place resolves to nothing. */
        queue_pair_generation_t generation = 0;

        /** Requests accepted against this medium and not yet performed. */
        std::int32_t in_flight = 0;
    };

    /** The allocator every pool of the disk is charged through. */
    using allocator_t = constrained_allocator<byte_t, atomic_reference_>;

    using queue_pair_pool_t = st::basic_slot_pool<queue_pair_record_t, std::uint16_t, allocator_t>;
    using registered_memory_pool_t = st::basic_slot_pool<registered_memory_record_t, std::uint16_t, allocator_t>;
    using registered_medium_pool_t = st::basic_slot_pool<registered_medium_record_t, std::uint16_t, allocator_t>;

    basic_disk(device_type_&& device, disk_config_t const& config) noexcept
        : device_(std::move(device)), depth_limit_(config.depth_limit) {}

    /** Grows @p elements to @p count default slots, reporting an allocation failure as a status. */
    template <typename element_type_>
    static status_t size_(st::vector<element_type_>& elements, std::size_t count) noexcept {
        if (elements.resize(count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A ring could not allocate its slots"};
        return {};
    }

    /** Makes @p pool of @p capacity free records charged through @p allocator. */
    template <typename pool_type_>
    static status_t make_pool_(pool_type_& pool, std::size_t capacity, allocator_t const& allocator) noexcept {
        auto [made, make_status] = pool_type_::make(capacity, allocator);
        if (make_status != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A ring could not allocate its slots"};
        pool = std::move(made);
        return {};
    }

    /** Takes a record of @p pool behind @p claim, which serializes it with every other take
     *  and vacate there. */
    template <typename pool_type_>
    static std::uint16_t take_(pool_type_& pool, std::int32_t& claim) noexcept {
        lock_(claim);
        std::uint16_t const index = pool.take();
        unlock_(claim);
        return index;
    }

    /** Vacates record @p index of @p pool behind @p claim, stepping its generation so older
     *  handles go stale. */
    template <typename pool_type_>
    static void vacate_(pool_type_& pool, std::int32_t& claim, std::uint16_t index) noexcept {
        lock_(claim);
        pool.vacate(index);
        unlock_(claim);
    }

    static void lock_(std::int32_t& claim) noexcept {
        atomic_reference_<std::int32_t> const word {claim};
        std::int32_t expected_free = 0;
        while (!word.compare_exchange_weak(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            waiter_type_ {}(&claim, expected_free, std::size_t {0});
            expected_free = 0;
        }
    }

    static void unlock_(std::int32_t& claim) noexcept {
        atomic_reference_<std::int32_t> {claim}.store(0, std::memory_order_release);
    }

    /** The handle a caller holds for record @p index of @p pool. */
    template <typename pool_type_>
    [[nodiscard]] static disk_handle_t handle_of_(pool_type_ const& pool, std::uint16_t index) noexcept {
        typename pool_type_::handle_t const handle = pool.handle_of(index);
        return disk_handle_t {handle.index, handle.generation};
    }

    static atomic_reference_<std::int32_t> cancelled_(queue_pair_record_t& record) noexcept {
        return atomic_reference_<std::int32_t> {record.cancelled};
    }

    static atomic_reference_<std::int32_t> in_flight_(registered_memory_record_t& record) noexcept {
        return atomic_reference_<std::int32_t> {record.in_flight};
    }

    static atomic_reference_<std::int32_t> in_flight_(registered_medium_record_t& record) noexcept {
        return atomic_reference_<std::int32_t> {record.in_flight};
    }

    /** Where one queue pair's requests and completions sit inside the shared windows. */
    [[nodiscard]] std::uint32_t window_of_(queue_pair_id_t id) const noexcept {
        return static_cast<std::uint32_t>(id) * depth_limit_;
    }

    [[nodiscard]] queue_pair_record_t* record_of_(queue_pair_t const& queue_pair) noexcept {
        return queue_pairs_.at({static_cast<std::uint16_t>(queue_pair.id()), queue_pair.generation()});
    }

    [[nodiscard]] queue_pair_record_t const* record_of_(queue_pair_t const& queue_pair) const noexcept {
        return queue_pairs_.at({static_cast<std::uint16_t>(queue_pair.id()), queue_pair.generation()});
    }

    [[nodiscard]] registered_memory_record_t* resolve_(memory_reference_t place) noexcept {
        return registered_memories_.at({static_cast<std::uint16_t>(place.memory_id), place.generation});
    }

    [[nodiscard]] registered_medium_record_t* resolve_(medium_reference_t place) noexcept {
        return media_.at({static_cast<std::uint16_t>(place.medium_id), place.generation});
    }

    /** Marks the range and the medium of @p request as live, so neither can be taken back under it. */
    void borrow_(request_t const& request) noexcept {
        if (registered_memory_record_t* const record = resolve_(request.buffer); record)
            in_flight_(*record).add(1, std::memory_order_acquire);
        if (registered_medium_record_t* const record = resolve_(request.medium); record)
            in_flight_(*record).add(1, std::memory_order_acquire);
    }

    /** Releases both once the device is done with the bytes of @p request. */
    void restore_(request_t const& request) noexcept {
        if (registered_memory_record_t* const record = resolve_(request.buffer); record)
            in_flight_(*record).sub(1, std::memory_order_release);
        if (registered_medium_record_t* const record = resolve_(request.medium); record)
            in_flight_(*record).sub(1, std::memory_order_release);
    }

    /** Where inside the shared completion window the @p index -th fresh completion of a queue pair goes. */
    [[nodiscard]] static std::size_t ready_place_(queue_pair_record_t const& record, std::uint32_t base,
                                                  std::size_t index) noexcept {
        return base + (record.ready_first + record.ready_count + index) % record.depth;
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
        registered_medium_record_t const* const medium = resolve_(request.medium);
        std::uint16_t const slot_index = static_cast<std::uint16_t>(request.medium.medium_id);
        if (!medium) {
            completion.state = completion_state_t::failed_k;
            completion.status = {status_code_t::invalid_argument_k, "The request names no registered medium"};
            return completion;
        }
        if (request.kind == request_kind_t::flush_k || request.kind == request_kind_t::deallocate_k) {
            completion.status = request.kind == request_kind_t::flush_k
                                    ? device_.flush(slot_index)
                                    : device_.deallocate_extent(slot_index, request.extent);
            completion.state = completion.status.failed() ? completion_state_t::failed_k
                                                          : completion_state_t::completed_k;
            return completion;
        }

        ring_capabilities_t const& limits = capabilities_;
        registered_memory_record_t const* const slot = resolve_(request.buffer);
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
                                        ? device_.read_extent(slot_index, request.extent, buffer)
                                        : device_.write_extent(slot_index, request.extent, buffer);
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
     *  Every request runs on the thread that polls it, in submission order, so a chain gets
     *  the ordering it was promised and nothing here forks. Depth comes from queue pairs
     *  rather than threads:
     *  a pool dispatch blocks until all its threads return and admits one caller at a time, so
     *  fanning a batch out here would serialize two queue pairs against each other, and a batch
     *  issued from inside a background job would fork a pool that is already forked.
     */
    void perform_(queue_pair_record_t& record, std::uint32_t base) noexcept {
        std::uint32_t const count = record.pending_count;
        if (count == 0) return;

        bool const cancelled = cancelled_(record).load(std::memory_order_acquire) != 0;
        bool chain_broken = false;
        for (std::uint32_t index = 0; index != count; ++index) {
            request_t const& request = pending_[base + index];
            completion_t const completion = cancelled || chain_broken ? cancelled_completion_(request)
                                                                      : perform_one_(request);
            ready_[ready_place_(record, base, index)] = completion;
            restore_(request);
            chain_broken = request.chaining == request_chaining_t::chained_k &&
                           (chain_broken || completion.state != completion_state_t::completed_k);
        }

        record.ready_count += count;
        record.pending_count = 0;
    }

    /** The transport this disk moves bytes through, owned here. */
    device_type_ device_;

    /** What this disk accepts, read from the transport once at mount. */
    ring_capabilities_t capabilities_;

    /** Requests one queue pair may keep in flight, and the stride of every queue pair's window. */
    queue_depth_t depth_limit_ = 0;

    /** One record per queue pair the ring hands out. */
    queue_pair_pool_t queue_pairs_;

    /** One record per memory range the ring pins. */
    registered_memory_pool_t registered_memories_;

    /** One record per medium the ring registers. */
    registered_medium_pool_t media_;

    /** Zero while no thread takes or vacates a queue pair record, one while one does. */
    std::int32_t queue_pairs_claim_ = 0;

    /** Zero while no thread takes or vacates a range record, one while one does. */
    std::int32_t registered_memories_claim_ = 0;

    /** Zero while no thread takes or vacates a medium record, one while one does. */
    std::int32_t media_claim_ = 0;

    /** Every queue pair's submitted requests, in windows of @c depth_limit_. */
    st::vector<request_t> pending_;

    /** Every queue pair's performed completions, in windows of @c depth_limit_ read as a circle. */
    st::vector<completion_t> ready_;
};

#pragma endregion Disk

#pragma endregion Disk

#pragma region Memory Disk

#pragma region Device

/**
 *  @brief Fixed-size volumes in host memory, copied into and out of at absolute offsets.
 *
 *  A volume never grows, so a request that reaches its end moves a prefix and stops, which is the
 *  partial transfer a real device reports at the end of a namespace. The alignments it demands are a
 *  mount parameter rather than a property of memory, so a suite can hold a transport to the numbers
 *  an @c O_DIRECT file or an NVMe namespace would report without owning either.
 */
class memory_device_t {
  public:

    memory_device_t() noexcept = default;
    memory_device_t(memory_device_t const&) = delete;
    memory_device_t& operator=(memory_device_t const&) = delete;
    memory_device_t(memory_device_t&&) noexcept = default;
    memory_device_t& operator=(memory_device_t&&) noexcept = default;

    /**
     *  @brief Makes a device whose volumes demand @p alignment of every request.
     *  @param[in] alignment Memory, offset and length alignment the ring holds requests to.
     *
     *  @note No volume is allocated here; a mount registers each one as a medium, so one device
     *      serves a directory of many volumes exactly as a file device does.
     */
    static expected<memory_device_t> make(std::uint64_t alignment = 1) noexcept {
        if (!std::has_single_bit(alignment))
            return status_t {status_code_t::invalid_argument_k, "An alignment is a power of two"};
        memory_device_t device;
        device.alignment_ = alignment;
        return device;
    }

    /** Sizes the table to @p count empty volumes, once, before any medium is opened. */
    status_t reserve_media(std::size_t count) noexcept {
        if (volumes_.resize(count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A device could not track its media"};
        return {};
    }

    /**
     *  @brief Allocates a volume of @c request.length bytes in slot @p index, which no write grows.
     *  @param[in] index The empty slot the disk took for the medium.
     *  @param[in] request The bytes to allocate; its name labels the volume and is not parsed.
     */
    status_t open_medium(std::uint16_t index, medium_options_t const& request) noexcept {
        if (index >= volumes_.size() || !volumes_[index].empty())
            return {status_code_t::invalid_argument_k, "The slot is not an empty one of this device"};
        if (request.length == 0) return {status_code_t::invalid_argument_k, "A volume holds at least one byte"};
        auto storage = storage_t::make(static_cast<std::size_t>(request.length));
        if (!storage) return {status_code_t::out_of_memory_k, "A volume of that size does not fit"};
        if (storage->resize(static_cast<std::size_t>(request.length)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A volume of that size does not fit"};
        volumes_[index] = std::move(*storage);
        return {};
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

    /** Succeeds without work, since a volume in memory keeps every byte whether or not it
     *  is still wanted. */
    status_t deallocate_extent(std::uint16_t index, extent_t) noexcept {
        if (index >= volumes_.size() || volumes_[index].empty())
            return {status_code_t::invalid_argument_k, "The medium is not this device's"};
        return {};
    }

    /** The bytes the volume in @p index holds, fixed when it was attached. */
    [[nodiscard]] std::uint64_t capacity(std::uint16_t index) const noexcept {
        return index < volumes_.size() ? volumes_[index].size() : 0;
    }

    /** The alignment the mount demands, standing in for a logical block, or the portable one when
     *  it demands none. */
    [[nodiscard]] std::uint64_t logical_block_bytes(std::uint16_t) const noexcept {
        return alignment_ > 1 ? alignment_ : portable_alignment_k;
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
    using storage_t = st::vector<byte_t, aligned_allocator<byte_t, portable_alignment_k>>;

    /** One entry per slot the disk may register, empty where a slot is free. */
    st::vector<storage_t> volumes_;

    /** Memory, offset and length alignment the ring holds requests to. */
    std::uint64_t alignment_ = 1;
};

/** The ring a suite drives without a device, satisfying the same contract as every disk. */
template <template <typename> typename atomic_reference_, typename waiter_type_>
using memory_disk = basic_disk<memory_device_t, atomic_reference_, waiter_type_>;

#pragma endregion Device

#pragma endregion Memory Disk

#pragma region Blocking Files Disk

#pragma region Access

/** Whether the page cache stands between the ring and the medium. */
enum class file_buffering_t : std::uint8_t {

    /** The kernel caches, which costs a copy and accepts any alignment. */
    buffered_k,

    /** The kernel does not cache, which needs aligned memory, offsets and lengths. */
    unbuffered_k,
};

#pragma endregion Access

#pragma region Platform

/** The two alignments unbuffered access demands, which every filesystem answers differently. */
struct direct_alignment_t {

    /** Alignment the first byte of a registered buffer must meet. */
    std::uint64_t memory = portable_alignment_k;

    /** Alignment an extent's offset and its length must both meet, which is the
     *  medium's logical block. */
    std::uint64_t offset = portable_alignment_k;
};

#if USTORE_OS_WINDOWS_

/** The handle one open file is addressed by. */
using native_file_t = void*;

/** The handle value that marks a file that never opened. */
inline native_file_t const invalid_native_file_k = INVALID_HANDLE_VALUE;

/** Opens @p path, or reports why the platform refused. */
inline expected<native_file_t> open_native_file(char const* path, medium_access_t access,
                                                file_buffering_t buffering) noexcept {
    DWORD const rights = access == medium_access_t::read_only_k ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
    DWORD const creation = access == medium_access_t::create_read_write_k ? OPEN_ALWAYS : OPEN_EXISTING;
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (buffering == file_buffering_t::unbuffered_k) flags |= FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
    native_file_t const file = CreateFileA(path, rights, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, creation, flags,
                                           nullptr);
    if (file == invalid_native_file_k) return status_t {status_code_t::io_k, "The file would not open"};
    return file;
}

/** Closes @p file, which must have opened. */
inline void close_native_file(native_file_t file) noexcept { CloseHandle(file); }

/** Reads at most @p buffer bytes from @p offset, answering how many arrived. */
inline expected<std::uint64_t> read_native_file(native_file_t file, std::uint64_t offset,
                                                std::span<byte_t> buffer) noexcept {
    std::uint64_t moved = 0;
    while (moved != buffer.size()) {
        OVERLAPPED position {};
        std::uint64_t const at = offset + moved;
        position.Offset = static_cast<DWORD>(at & 0xFFFFFFFFull);
        position.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD arrived = 0;
        DWORD const asking = static_cast<DWORD>(buffer.size() - moved);
        if (!ReadFile(file, buffer.data() + moved, asking, &arrived, &position)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return status_t {status_code_t::io_k, "A positioned read failed"};
        }
        if (arrived == 0) break;
        moved += arrived;
    }
    return moved;
}

/** Writes at most @p buffer bytes at @p offset, answering how many landed. */
inline expected<std::uint64_t> write_native_file(native_file_t file, std::uint64_t offset,
                                                 std::span<byte_t const> buffer) noexcept {
    std::uint64_t moved = 0;
    while (moved != buffer.size()) {
        OVERLAPPED position {};
        std::uint64_t const at = offset + moved;
        position.Offset = static_cast<DWORD>(at & 0xFFFFFFFFull);
        position.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD landed = 0;
        DWORD const asking = static_cast<DWORD>(buffer.size() - moved);
        if (!WriteFile(file, buffer.data() + moved, asking, &landed, &position))
            return status_t {status_code_t::io_k, "A positioned write failed"};
        if (landed == 0) break;
        moved += landed;
    }
    return moved;
}

/** Makes every landed write durable. */
inline status_t flush_native_file(native_file_t file) noexcept {
    if (!FlushFileBuffers(file)) return {status_code_t::io_k, "The file would not flush"};
    return {};
}

/** The bytes the file currently holds. */
[[nodiscard]] inline std::uint64_t native_file_capacity(native_file_t file) noexcept {
    LARGE_INTEGER size {};
    return GetFileSizeEx(file, &size) ? static_cast<std::uint64_t>(size.QuadPart) : 0;
}

/** Resizes the file to @p bytes, which a mount does once to preallocate a volume. */
inline status_t truncate_native_file(native_file_t file, std::uint64_t bytes) noexcept {
    LARGE_INTEGER position {};
    position.QuadPart = static_cast<LONGLONG>(bytes);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN) || !SetEndOfFile(file))
        return {status_code_t::io_k, "The file would not resize"};
    return {};
}

/** Gives @p extent of the file back, which reads as zeros afterwards. */
inline status_t deallocate_native_file(native_file_t file, extent_t extent) noexcept {
    FILE_ZERO_DATA_INFORMATION range {};
    range.FileOffset.QuadPart = static_cast<LONGLONG>(extent.offset);
    range.BeyondFinalZero.QuadPart = static_cast<LONGLONG>(extent.end());
    DWORD returned = 0;
    if (!DeviceIoControl(file, FSCTL_SET_ZERO_DATA, &range, sizeof(range), nullptr, 0, &returned, nullptr))
        return {status_code_t::io_k, "The file would not deallocate"};
    return {};
}

/** Answers the portable numbers, which this platform offers no way to ask the filesystem for. */
[[nodiscard]] inline direct_alignment_t direct_alignment_of(native_file_t) noexcept { return {}; }

#else

/** The registered medium one open file is addressed by. */
using native_file_t = int;

/** The registered medium value that marks a file that never opened. */
inline native_file_t const invalid_native_file_k = -1;

/** Translates the platform's failure into the status the ring reports. */
inline status_t native_file_failure(int reason, char const* message) noexcept {
    switch (reason) {
    case ENOSPC:
    case EDQUOT: return {status_code_t::capacity_exhausted_k, message};
    case ENOENT: return {status_code_t::not_found_k, message};
    case EEXIST: return {status_code_t::already_exists_k, message};
    case EINVAL:
    case EBADF: return {status_code_t::invalid_argument_k, message};
    case ENOMEM: return {status_code_t::out_of_memory_k, message};
    case EIO: return {status_code_t::io_k, message};
    default: return {status_code_t::device_k, message};
    }
}

#if USTORE_OS_LINUX_

/** The open flag that takes the page cache out of the path. */
[[nodiscard]] inline int unbuffered_open_flag() noexcept { return O_DIRECT; }

/** The bytes a regular file holds, or the whole medium behind a block device, which @c fstat sizes at zero. */
[[nodiscard]] inline std::uint64_t native_file_capacity(native_file_t file) noexcept {
    struct stat description {};
    if (::fstat(file, &description) != 0) return 0;
    if (!S_ISBLK(description.st_mode)) return static_cast<std::uint64_t>(description.st_size);
    std::uint64_t bytes = 0;
    return ::ioctl(file, BLKGETSIZE64, &bytes) == 0 ? bytes : 0;
}

#else

/** The open flag that takes the page cache out of the path, which this platform sets afterwards. */
[[nodiscard]] inline int unbuffered_open_flag() noexcept { return 0; }

/** The bytes a regular file holds, which is all this platform's @c fstat answers. */
[[nodiscard]] inline std::uint64_t native_file_capacity(native_file_t file) noexcept {
    struct stat description {};
    return ::fstat(file, &description) == 0 ? static_cast<std::uint64_t>(description.st_size) : 0;
}

#endif

#if USTORE_OS_APPLE_

/** Asks for unbuffered access on a platform whose open flag cannot express it. */
inline void request_unbuffered_access(native_file_t file) noexcept { fcntl(file, F_NOCACHE, 1); }

/** Makes every landed write durable, past the drive's own cache. */
inline status_t flush_native_file(native_file_t file) noexcept {
    if (fcntl(file, F_FULLFSYNC) != 0) return native_file_failure(errno, "The file would not flush");
    return {};
}

#else

/** Asks for unbuffered access, which this platform already granted at open. */
inline void request_unbuffered_access(native_file_t) noexcept {}

/** Makes every landed write durable, without waiting on the metadata that does not matter. */
inline status_t flush_native_file(native_file_t file) noexcept {
    if (fdatasync(file) != 0) return native_file_failure(errno, "The file would not flush");
    return {};
}

#endif

/** Opens @p path, or reports why the platform refused. */
inline expected<native_file_t> open_native_file(char const* path, medium_access_t access,
                                                file_buffering_t buffering) noexcept {
    int flags = access == medium_access_t::read_only_k ? O_RDONLY : O_RDWR;
    if (access == medium_access_t::create_read_write_k) flags |= O_CREAT;
    if (buffering == file_buffering_t::unbuffered_k) flags |= unbuffered_open_flag();
    native_file_t const file = ::open(path, flags, 0644);
    if (file == invalid_native_file_k) return native_file_failure(errno, "The file would not open");
    if (buffering == file_buffering_t::unbuffered_k) request_unbuffered_access(file);
    return file;
}

/** Closes @p file, which must have opened. */
inline void close_native_file(native_file_t file) noexcept { ::close(file); }

/** Reads at most @p buffer bytes from @p offset, answering how many arrived. */
inline expected<std::uint64_t> read_native_file(native_file_t file, std::uint64_t offset,
                                                std::span<byte_t> buffer) noexcept {
    std::uint64_t moved = 0;
    while (moved != buffer.size()) {
        ssize_t const arrived = ::pread(file, buffer.data() + moved, buffer.size() - moved,
                                        static_cast<off_t>(offset + moved));
        if (arrived < 0) {
            if (errno == EINTR) continue;
            return native_file_failure(errno, "A positioned read failed");
        }
        if (arrived == 0) break;
        moved += static_cast<std::uint64_t>(arrived);
    }
    return moved;
}

/** Writes at most @p buffer bytes at @p offset, answering how many landed. */
inline expected<std::uint64_t> write_native_file(native_file_t file, std::uint64_t offset,
                                                 std::span<byte_t const> buffer) noexcept {
    std::uint64_t moved = 0;
    while (moved != buffer.size()) {
        ssize_t const landed = ::pwrite(file, buffer.data() + moved, buffer.size() - moved,
                                        static_cast<off_t>(offset + moved));
        if (landed < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOSPC || errno == EDQUOT) break;
            return native_file_failure(errno, "A positioned write failed");
        }
        if (landed == 0) break;
        moved += static_cast<std::uint64_t>(landed);
    }
    return moved;
}

/** Resizes a regular file to @p bytes, which a mount does once to preallocate a volume; a block device is refused. */
inline status_t truncate_native_file(native_file_t file, std::uint64_t bytes) noexcept {
    struct stat description {};
    if (::fstat(file, &description) == 0 && S_ISBLK(description.st_mode))
        return {status_code_t::invalid_argument_k, "A block device keeps the capacity of its medium"};
    if (::ftruncate(file, static_cast<off_t>(bytes)) != 0)
        return native_file_failure(errno, "The file would not resize");
    return {};
}

#if USTORE_OS_LINUX_

/** Gives @p extent back: a hole punched in a regular file, a discard on a block device. */
inline status_t deallocate_native_file(native_file_t file, extent_t extent) noexcept {
    struct stat description {};
    if (::fstat(file, &description) != 0) return native_file_failure(errno, "The file would not describe itself");
    if (S_ISBLK(description.st_mode)) {
        std::uint64_t range[2] = {extent.offset, extent.length};
        if (::ioctl(file, BLKDISCARD, &range) != 0) return native_file_failure(errno, "The device would not discard");
        return {};
    }
    if (::fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(extent.offset),
                    static_cast<off_t>(extent.length)) != 0)
        return native_file_failure(errno, "The file would not punch a hole");
    return {};
}

/** Asks @p file's filesystem what unbuffered access demands, falling back to
 *  the portable numbers. */
[[nodiscard]] inline direct_alignment_t direct_alignment_of(native_file_t file) noexcept {
    struct statx description {};
    if (::syscall(__NR_statx, file, "", AT_EMPTY_PATH, STATX_DIOALIGN, &description) != 0) return {};
    if ((description.stx_mask & STATX_DIOALIGN) == 0) return {};
    if (description.stx_dio_mem_align == 0 || description.stx_dio_offset_align == 0) return {};
    return direct_alignment_t {description.stx_dio_mem_align, description.stx_dio_offset_align};
}

#elif USTORE_OS_APPLE_

/** Gives @p extent of a file back as a hole, which reads as zeros afterwards. */
inline status_t deallocate_native_file(native_file_t file, extent_t extent) noexcept {
    fpunchhole_t hole {};
    hole.fp_offset = static_cast<off_t>(extent.offset);
    hole.fp_length = static_cast<off_t>(extent.length);
    if (fcntl(file, F_PUNCHHOLE, &hole) != 0) return native_file_failure(errno, "The file would not punch a hole");
    return {};
}

/** Answers the portable numbers, which this platform offers no way to ask the filesystem for. */
[[nodiscard]] inline direct_alignment_t direct_alignment_of(native_file_t) noexcept { return {}; }

#else

/** Refuses, since this platform offers no way to give a file's blocks back. */
inline status_t deallocate_native_file(native_file_t, extent_t) noexcept {
    return {status_code_t::unsupported_k, "This platform cannot punch a hole in a file"};
}

/** Answers the portable numbers, which this platform offers no way to ask the filesystem for. */
[[nodiscard]] inline direct_alignment_t direct_alignment_of(native_file_t) noexcept { return {}; }

#endif

#endif

#pragma endregion Platform

#pragma region Device

/**
 *  @brief One open file, read and written at absolute offsets by any number of threads at once.
 *
 *  Neither @c pread nor an @c OVERLAPPED @c ReadFile moves a shared cursor, so the device needs no
 *  lock of its own and a queue pair per thread is the only concurrency control. A short result is
 *  not an error here: a read that meets the end of the file, and a write that meets a full volume,
 *  answer the prefix they moved and let the ring report it as a partial transfer.
 */
class blocking_files_device_t {
  public:

    blocking_files_device_t() noexcept = default;
    blocking_files_device_t(blocking_files_device_t const&) = delete;
    blocking_files_device_t& operator=(blocking_files_device_t const&) = delete;

    blocking_files_device_t(blocking_files_device_t&& other) noexcept
        : files_(std::move(other.files_)), buffering_(other.buffering_) {}

    blocking_files_device_t& operator=(blocking_files_device_t&& other) noexcept {
        if (this == &other) return *this;
        close_all_();
        files_ = std::move(other.files_);
        buffering_ = other.buffering_;
        return *this;
    }

    ~blocking_files_device_t() noexcept { close_all_(); }

    /**
     *  @brief Makes a device that opens its files with @p buffering.
     *  @param[in] buffering Whether the page cache stands in the path, which fixes the alignments.
     *
     *  @note No file is opened here; a mount registers each one as a medium, so a directory of many
     *      sorted runs is one device rather than one device per run.
     */
    static expected<blocking_files_device_t> make(file_buffering_t buffering) noexcept {
        blocking_files_device_t device;
        device.buffering_ = buffering;
        return device;
    }

    /** Sizes the table to @p count closed files, once, before any medium is opened. */
    status_t reserve_media(std::size_t count) noexcept {
        if (files_.resize(count, invalid_native_file_k) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A device could not track its media"};
        return {};
    }

    /**
     *  @brief Opens the file @p request names in slot @p index, creating and sizing it when it
     *      asks for that.
     *  @param[in] index The empty slot the disk took for the medium.
     *  @param[in] request The path, the bytes to create it with, and what the caller may do to it.
     */
    status_t open_medium(std::uint16_t index, medium_options_t const& request) noexcept {
        if (index >= files_.size() || files_[index] != invalid_native_file_k)
            return {status_code_t::invalid_argument_k, "The slot is not an empty one of this device"};
        if (!request.name) return {status_code_t::invalid_argument_k, "A file medium needs a path"};
        auto [file, open_native_file_status] = open_native_file(request.name, request.access, buffering_);
        if (open_native_file_status.failed()) return open_native_file_status;
        if (request.access == medium_access_t::create_read_write_k && request.length != 0)
            if (status_t const sized = truncate_native_file(file, request.length); sized.failed()) {
                close_native_file(file);
                return sized;
            }
        files_[index] = file;
        return {};
    }

    /** Closes the file in @p index, leaving its slot free for the next medium. */
    status_t close_medium(std::uint16_t index) noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k)
            return {status_code_t::invalid_argument_k, "The medium is not this device's"};
        close_native_file(files_[index]);
        files_[index] = invalid_native_file_k;
        return {};
    }

    /** Reads @p extent of @p index into @p destination, answering the prefix that arrived. */
    expected<std::uint64_t> read_extent(std::uint16_t index, extent_t extent, std::span<byte_t> destination) noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k)
            return status_t {status_code_t::invalid_argument_k, "The medium is not this device's"};
        return read_native_file(files_[index], extent.offset,
                                destination.first(static_cast<std::size_t>(extent.length)));
    }

    /** Writes @p source at @p extent of @p index, answering the prefix that landed. */
    expected<std::uint64_t> write_extent(std::uint16_t index, extent_t extent,
                                         std::span<byte_t const> source) noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k)
            return status_t {status_code_t::invalid_argument_k, "The medium is not this device's"};
        return write_native_file(files_[index], extent.offset, source.first(static_cast<std::size_t>(extent.length)));
    }

    /** Makes every write @p index has already answered durable. */
    status_t flush(std::uint16_t index) noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k)
            return {status_code_t::invalid_argument_k, "The medium is not this device's"};
        return flush_native_file(files_[index]);
    }

    /** Gives @p extent of @p index back: a hole punched in a file, a discard on a block device. */
    status_t deallocate_extent(std::uint16_t index, extent_t extent) noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k)
            return {status_code_t::invalid_argument_k, "The medium is not this device's"};
        return deallocate_native_file(files_[index], extent);
    }

    /** The bytes @p index currently holds: a regular file grows past it, a block device falls short. */
    [[nodiscard]] std::uint64_t capacity(std::uint16_t index) const noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k) return 0;
        return native_file_capacity(files_[index]);
    }

    /** The offset alignment unbuffered access to @p index demands, which is its logical block. */
    [[nodiscard]] std::uint64_t logical_block_bytes(std::uint16_t index) const noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k) return portable_alignment_k;
        return direct_alignment_of(files_[index]).offset;
    }

    /** Preallocates the regular file in @p index to @p bytes, which a mount does once; a block device is refused. */
    status_t resize(std::uint16_t index, std::uint64_t bytes) noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k)
            return {status_code_t::invalid_argument_k, "The medium is not this device's"};
        return truncate_native_file(files_[index], bytes);
    }

    /** What this transport accepts, which unbuffered access tightens to the portable alignment. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept {
        ring_capabilities_t reported;
        std::uint64_t const alignment = buffering_ == file_buffering_t::unbuffered_k ? portable_alignment_k : 1;
        reported.memory_alignment = alignment;
        reported.offset_alignment = alignment;
        reported.length_granularity = alignment;
        reported.max_transfer_bytes = max_transfer_bytes_k;
        reported.reaches_gpu_memory = false;
        return reported;
    }

  private:

    /** The largest extent one positioned call carries, kept well inside every platform's own limit. */
    static constexpr std::uint64_t max_transfer_bytes_k = 1024ull * 1024ull * 1024ull;

    /** Closes every open file, which both the destructor and a move assignment need. */
    void close_all_() noexcept {
        for (std::size_t index = 0; index != files_.size(); ++index)
            if (files_[index] != invalid_native_file_k) close_native_file(files_[index]);
        files_.clear();
    }

    /** One entry per slot the disk may register, @c invalid_native_file_k where a slot is free. */
    st::vector<native_file_t> files_;

    /** Whether the page cache stands in the path, which fixes the reported alignments. */
    file_buffering_t buffering_ = file_buffering_t::buffered_k;
};

/** The ring every platform has: positioned reads and writes, performed on the thread that polls. */
template <template <typename> typename atomic_reference_, typename waiter_type_>
using blocking_files_disk = basic_disk<blocking_files_device_t, atomic_reference_, waiter_type_>;

#pragma endregion Device

#pragma endregion Blocking Files Disk

} // namespace unum::ustore::utilities
