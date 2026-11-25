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
/*  Winsock 2 first keeps Winsock 1.1 out, unless a user's earlier `<windows.h>` brought it in. */
#if !defined(_WINSOCKAPI_)
#include <winsock2.h>
#endif
#include <windows.h> // `DeviceIoControl`
#include <winioctl.h> // `FSCTL_SET_ZERO_DATA`, `FILE_ZERO_DATA_INFORMATION`
#else
#include <fcntl.h>    // `fcntl`, `fallocate`, `F_PUNCHHOLE`, `AT_EMPTY_PATH`
#include <sys/stat.h> // `fstat`, `S_ISBLK`
#include <unistd.h>   // `syscall`
#endif
#if USTORE_OS_LINUX_
#include <linux/falloc.h> // `FALLOC_FL_PUNCH_HOLE`, `FALLOC_FL_KEEP_SIZE`
#include <linux/fs.h>     // `BLKGETSIZE64`, `BLKDISCARD`
#include <sys/ioctl.h>    // `ioctl`
#include <sys/syscall.h>  // `SYS_statx`
#endif

#include <bit>      // `std::has_single_bit`
#include <concepts> // `std::same_as`
#include <span>     // `std::span`
#include <utility>  // `std::move`

#include <smashtable/basic_fixed_pool.hpp> // `st::basic_fixed_pool`
#include <smashtable/basic_vector.hpp>     // `st::vector`

#include "ustore/utilities/blocking_files.hpp" // `native_file_t`, `open_native_file`, `native_file_event_t`
#include "ustore/utilities/nvme_ring.hpp" // `request_t`, `completion_t`, `queue_ref_t`, `extent_t`, `is_address_aligned`
#include "ustore/utilities/ram_buffers.hpp" // `aligned_allocator`, `constrained_allocator`, `resource_constraint`
#include "ustore/utilities/types.hpp"       // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::utilities {

#pragma region Disk

#pragma region Devices

/**
 *  @brief The moves a transport implements, from which @c basic_disk builds a whole ring.
 *
 *  Every call is synchronous and thread-safe for distinct extents, which positioned reads and
 *  writes are on every platform. Media sit in a table sized once by @c reserve_media and filled
 *  at the slot the disk names, so registering one never moves the table under a reader of
 *  another. A transfer names the event of the queue polling it, which the queue's thread owns, so
 *  a device keeps nothing per polling thread. Asynchrony is the ring's business, not the
 *  device's, so a device that already has a queue of its own implements the ring directly instead
 *  of satisfying this.
 */
template <typename device_type_>
concept a_block_device = requires(device_type_& device, device_type_ const& constant, medium_options_t medium,
                                  std::uint16_t index, native_file_event_t const& event, std::size_t count,
                                  extent_t extent, std::span<byte_t> destination, std::span<byte_t const> source) {
    { device.reserve_media(count) } -> std::same_as<status_t>;
    { device.open_medium(index, medium) } -> std::same_as<status_t>;
    { device.close_medium(index) } -> std::same_as<status_t>;
    { device.read_extent(index, extent, destination, event) } -> std::same_as<expected<std::uint64_t>>;
    { device.write_extent(index, extent, source, event) } -> std::same_as<expected<std::uint64_t>>;
    { device.flush(index) } -> std::same_as<status_t>;
    { device.deallocate_extent(index, extent) } -> std::same_as<status_t>;
    { constant.capacity(index) } -> std::same_as<std::uint64_t>;
    { constant.logical_block_bytes(index) } -> std::same_as<std::uint64_t>;
    { constant.capabilities() } -> std::same_as<disk_capabilities_t>;
};

#pragma endregion Devices

#pragma region Configuration

/** What a mount fixes about a disk's ring before any queue holds it. */
struct disk_config_t {

    /** Memory ranges the disk pins at once, which a transport registering one arena keeps at one. */
    std::uint16_t registered_memory_limit = 8;

    /** Targets the disk registers at once, which a namespace keeps at one and a directory raises. */
    std::uint16_t media_limit = 1;
};

#pragma endregion Configuration

#pragma region Queue

/**
 *  @brief One queue of the positioned-I/O floor, owned by a thread: the requests it submitted, the
 *      completions its poll performed, and the event a Windows transfer of that thread waits on.
 *
 *  The queue knows no disk. A call names it through a @c queue_ref_t to whichever disk it reads,
 *  and that disk's poll performs the requests on the thread owning the queue, so the queue pays for
 *  its own I/O. Every file disk serves it besides its own queue, which is how a call without an
 *  executor reads through no ring at all.
 */
class blocking_queue_t {
  public:

    blocking_queue_t() noexcept = default;
    blocking_queue_t(blocking_queue_t const&) = delete;
    blocking_queue_t& operator=(blocking_queue_t const&) = delete;
    blocking_queue_t(blocking_queue_t&&) noexcept = default;
    blocking_queue_t& operator=(blocking_queue_t&&) noexcept = default;

    /**
     *  @brief A queue of @p depth requests whose own blocks are @p range.
     *  @param[in] depth Requests the queue keeps in flight at once.
     *  @param[in] range The thread's blocks, which requests naming @c queue_range_k
     *      address, or empty.
     */
    static expected<blocking_queue_t> make(queue_depth_t depth, std::span<byte_t> range) noexcept {
        if (depth == 0) return status_t {status_code_t::invalid_argument_k, "A queue keeps a request in flight"};
        blocking_queue_t queue;
        if (queue.pending_.resize(depth) != st::status_t::success_k ||
            queue.ready_.resize(depth) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A queue could not size its windows"};
        auto [event, make_event_status] = native_file_event_t::make();
        if (make_event_status.failed()) return make_event_status;
        queue.event_ = std::move(event);
        queue.depth_ = depth;
        queue.pinned_staging_ = range;
        return queue;
    }

    /** Requests the queue keeps in flight at once. */
    [[nodiscard]] queue_depth_t depth() const noexcept { return depth_; }

    /** The thread's blocks, which a request naming @c queue_range_k addresses. */
    [[nodiscard]] std::span<byte_t> range() const noexcept { return pinned_staging_; }

    /** What a Windows transfer of this queue waits on. */
    [[nodiscard]] native_file_event_t const& event() const noexcept { return event_; }

    /** Requests submitted and not yet polled. */
    [[nodiscard]] std::size_t outstanding() const noexcept { return pending_count_ + ready_count_; }

    /** Takes as much of @p requests as the queue has credit for, handing each one taken to @p
     *  view. */
    template <typename view_type_>
    std::size_t take(std::span<request_t const> requests, view_type_&& view) noexcept {
        std::size_t const room = depth_ - outstanding();
        std::size_t const taking = requests.size() < room ? requests.size() : room;
        for (std::size_t index = 0; index != taking; ++index) {
            pending_[pending_count_ + index] = requests[index];
            view(requests[index]);
        }
        pending_count_ += static_cast<std::uint32_t>(taking);
        return taking;
    }

    /** Performs every pending request through @p perform, gives each one's view back through @p
     *  restore, and moves the completions into @p into, oldest first. */
    template <typename perform_type_, typename restore_type_>
    std::size_t poll(std::span<completion_t> into, perform_type_&& perform, restore_type_&& restore) noexcept {
        perform_pending_(perform, restore);
        std::size_t const moving = into.size() < ready_count_ ? into.size() : ready_count_;
        for (std::size_t index = 0; index != moving; ++index) into[index] = ready_[(ready_first_ + index) % depth_];
        ready_first_ = static_cast<std::uint32_t>((ready_first_ + moving) % depth_);
        ready_count_ -= static_cast<std::uint32_t>(moving);
        return moving;
    }

    /** Performs and discards everything outstanding, and forgets a cancellation, so the queue
     *  starts afresh. */
    template <typename perform_type_, typename restore_type_>
    void drain(perform_type_&& perform, restore_type_&& restore) noexcept {
        perform_pending_(perform, restore);
        ready_first_ = 0;
        ready_count_ = 0;
        cancelled_ = 0;
    }

    /** Stops the requests that have not started, which the next poll reports cancelled. */
    void cancel() noexcept { cancelled_ = 1; }

  private:

    /** The completion a request gets when its queue cancelled it or a chain ahead of it was cut. */
    [[nodiscard]] static completion_t cancelled_completion_(request_t const& request) noexcept {
        completion_t completion;
        completion.token = request.token;
        completion.state = completion_state_t::cancelled_k;
        completion.status = {status_code_t::uninitialized_k, "The queue_pair cancelled this request"};
        return completion;
    }

    /**
     *  @brief Performs everything submitted in submission order, staging the completions
     *      for the poll.
     *
     *  Every request runs on the thread that polls it, so a chain gets the ordering it was promised
     *  and nothing here forks: a pool dispatch admits one caller at a time, so fanning a batch out
     *  here would serialize two queues against each other.
     */
    template <typename perform_type_, typename restore_type_>
    void perform_pending_(perform_type_& perform, restore_type_& restore) noexcept {
        std::uint32_t const count = pending_count_;
        bool chain_broken = false;
        for (std::uint32_t index = 0; index != count; ++index) {
            request_t const& request = pending_[index];
            completion_t const completion = cancelled_ != 0 || chain_broken ? cancelled_completion_(request)
                                                                            : perform(request);
            ready_[(ready_first_ + ready_count_ + index) % depth_] = completion;
            restore(request);
            chain_broken = request.chaining == request_chaining_t::chained_k &&
                           (chain_broken || completion.state != completion_state_t::completed_k);
        }
        ready_count_ += count;
        pending_count_ = 0;
    }

    /** Zero while the queue wants its requests, one once it has cancelled them until its drain. */
    std::int32_t cancelled_ = 0;

    /** Requests the queue keeps in flight at once. */
    queue_depth_t depth_ = 0;

    /** Requests submitted and not yet performed, at the front of @c pending_. */
    std::uint32_t pending_count_ = 0;

    /** Where the oldest unreaped completion sits inside @c ready_. */
    std::uint32_t ready_first_ = 0;

    /** Completions performed and not yet polled. */
    std::uint32_t ready_count_ = 0;

    /** The submitted requests. */
    st::vector<request_t> pending_;

    /** The performed completions, read as a circle. */
    st::vector<completion_t> ready_;

    /** What a Windows transfer of this queue waits on, empty elsewhere. */
    native_file_event_t event_;

    /** The thread's blocks, which a request naming @c queue_range_k addresses. */
    std::span<byte_t> pinned_staging_;
};

#pragma endregion Queue

#pragma region Disk

/**
 *  @brief Turns a positioned-I/O device into the submission and completion ring callers drive.
 *
 *  Depth is partitioned rather than pooled: every queue keeps its own depth of requests, so one
 *  queue that stops polling cannot starve another, and a full ring is always the queue's own doing.
 *  A completion lands only in the queue that submitted its request.
 *
 *  Ordering between two submitted requests is never promised. Two writes may reach the medium in
 *  either order, and a flush covers only what the queue pair has already completed, so
 *  durability is written as submit, poll, flush, poll. A bounded sequence that must be ordered
 *  says so with @c request_chaining_t::chained_k, which runs the chain in submission order and
 *  cuts it with @c cancelled_k at the first short or failed result.
 *
 *  @tparam atomic_reference_ The reference the claim, cancellation and in-submitted words
 *      are owned through.
 *  @tparam waiter_type_ What a core does while another thread takes or vacates a record of
 *      the same pool.
 */
template <typename device_type_, template <typename> typename atomic_reference_, typename waiter_type_>
    requires a_block_device<device_type_>
class basic_disk {
  public:

    /** The budget the disk's pools are charged to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    /** The queue a thread owns for this transport. */
    using queue_t = blocking_queue_t;

    /** Holds nothing, as a moved-from disk does. */
    basic_disk() noexcept = default;
    basic_disk(basic_disk const&) = delete;
    basic_disk& operator=(basic_disk const&) = delete;
    basic_disk(basic_disk&&) noexcept = default;
    basic_disk& operator=(basic_disk&&) noexcept = default;

    /**
     *  @brief Mounts @p device behind a ring shaped by @p config.
     *  @param[in] device The transport, moved in and owned by the disk from here on.
     *  @param[in] config The pinned ranges and media the ring holds.
     *  @param[in,out] constraint The budget the range and medium pools are charged to,
     *      outliving the disk.
     */
    static expected<basic_disk> make(device_type_&& device, disk_config_t const& config,
                                     constraint_t& constraint) noexcept {
        if (config.registered_memory_limit >= static_cast<std::uint16_t>(registered_memory_id_t::queue_range_k) ||
            config.media_limit == static_cast<std::uint16_t>(registered_medium_id_t::unregistered_k))
            return status_t {status_code_t::invalid_argument_k, "A ring limit reaches a reserved id"};
        if (config.media_limit == 0) return status_t {status_code_t::invalid_argument_k, "A ring needs a medium"};

        basic_disk disk {std::move(device)};
        allocator_t const allocator {constraint};
        if (status_t const made = make_pool_(disk.registered_memories_, config.registered_memory_limit, allocator);
            made.failed())
            return made;
        if (status_t const made = make_pool_(disk.media_, config.media_limit, allocator); made.failed()) return made;
        if (status_t const reserved = disk.device_.reserve_media(config.media_limit); reserved.failed())
            return reserved;
        disk.capabilities_ = disk.device_.capabilities();
        disk.capabilities_.media_limit = config.media_limit;
        return disk;
    }

    /** What this disk accepts, fixed at mount so no request pays to ask the transport again. */
    [[nodiscard]] disk_capabilities_t capabilities() const noexcept { return capabilities_; }

    /** The transport underneath, for a caller that needs what only the transport knows. */
    [[nodiscard]] device_type_ const& device() const noexcept { return device_; }

#pragma region Queues

    /** Checks @p queue_pair names a queue of this transport, which needs nothing held, since a
     *  request names its medium by this disk's slot and the device reads that slot at the poll. */
    status_t register_media(queue_ref_t const& queue_pair) noexcept {
        if (!queue_of_(queue_pair)) return {status_code_t::invalid_argument_k, "The queue is of another transport"};
        return {};
    }

    /** Requests @p queue_pair has submitted and not yet completed, which its credit bounds. */
    [[nodiscard]] std::size_t outstanding(queue_ref_t const& queue_pair) const noexcept {
        blocking_queue_t const* const queue = queue_of_(queue_pair);
        return queue ? queue->outstanding() : 0;
    }

#pragma endregion Queues

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
            return {status_code_t::invalid_argument_k, "Requests in flight still use the range"};
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
            return {status_code_t::invalid_argument_k, "Requests in flight still use the medium"};
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
    expected<std::size_t> submit(queue_ref_t const& queue_pair, std::span<request_t const> requests) noexcept {
        blocking_queue_t* const queue = queue_of_(queue_pair);
        if (!queue) return status_t {status_code_t::invalid_argument_k, "The queue is of another transport"};
        for (request_t const& request : requests)
            if (request.token.queue_pair() != queue_pair.id())
                return status_t {status_code_t::invalid_argument_k, "A request holds another queue pair's token"};

        return queue->take(requests, [&](request_t const& request) noexcept { count_request_on_slots_(request); });
    }

    /**
     *  @brief Performs what @p queue pair submitted and moves its completions into @p into, never waiting.
     *  @param[in] queue_pair The queue pair polling, held by the calling thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero when the queue pair submitted nothing.
     */
    expected<std::size_t> poll_completions(queue_ref_t const& queue_pair, std::span<completion_t> into) noexcept {
        blocking_queue_t* const queue = queue_of_(queue_pair);
        if (!queue) return status_t {status_code_t::invalid_argument_k, "The queue is of another transport"};
        return queue->poll(
            into, [&](request_t const& request) noexcept { return perform_one_(*queue, request); },
            [&](request_t const& request) noexcept { uncount_request_on_slots_(request); });
    }

    /**
     *  @brief Stops @p queue pair's requests that have not started, which the next poll reports cancelled.
     *  @param[in] queue_pair The queue pair to stop, whose started requests still run to their own end.
     */
    void cancel(queue_ref_t const& queue_pair) noexcept {
        if (blocking_queue_t* const queue = queue_of_(queue_pair); queue) queue->cancel();
    }

    /**
     *  @brief Finishes and discards everything @p queue pair has outstanding, so its buffers may be freed.
     *  @param[in] queue_pair The queue pair to empty, which may be cancelled or still running.
     */
    status_t drain(queue_ref_t const& queue_pair) noexcept {
        blocking_queue_t* const queue = queue_of_(queue_pair);
        if (!queue) return {status_code_t::invalid_argument_k, "The queue is of another transport"};
        queue->drain([&](request_t const& request) noexcept { return perform_one_(*queue, request); },
                     [&](request_t const& request) noexcept { uncount_request_on_slots_(request); });
        return {};
    }

#pragma endregion Ring

  private:

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

    using registered_memory_pool_t = st::basic_fixed_pool<registered_memory_record_t, std::uint16_t, allocator_t>;
    using registered_medium_pool_t = st::basic_fixed_pool<registered_medium_record_t, std::uint16_t, allocator_t>;

    explicit basic_disk(device_type_&& device) noexcept : device_(std::move(device)) {}

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
        lock_claim<atomic_reference_, waiter_type_>(claim);
        std::uint16_t const index = pool.take();
        unlock_claim<atomic_reference_>(claim);
        return index;
    }

    /** Vacates record @p index of @p pool behind @p claim, stepping its generation so older
     *  handles go stale. */
    template <typename pool_type_>
    static void vacate_(pool_type_& pool, std::int32_t& claim, std::uint16_t index) noexcept {
        lock_claim<atomic_reference_, waiter_type_>(claim);
        pool.vacate(index);
        unlock_claim<atomic_reference_>(claim);
    }

    /** The handle a caller holds for record @p index of @p pool. */
    template <typename pool_type_>
    [[nodiscard]] static disk_handle_t handle_of_(pool_type_ const& pool, std::uint16_t index) noexcept {
        typename pool_type_::handle_t const handle = pool.handle_of(index);
        return disk_handle_t {handle.index, handle.generation};
    }

    static atomic_reference_<std::int32_t> in_flight_(registered_memory_record_t& record) noexcept {
        return atomic_reference_<std::int32_t> {record.in_flight};
    }

    static atomic_reference_<std::int32_t> in_flight_(registered_medium_record_t& record) noexcept {
        return atomic_reference_<std::int32_t> {record.in_flight};
    }

    [[nodiscard]] static blocking_queue_t* queue_of_(queue_ref_t const& queue_pair) noexcept {
        return queue_pair.template queue<blocking_queue_t>();
    }

    [[nodiscard]] registered_memory_record_t* resolve_(memory_reference_t place) noexcept {
        return registered_memories_.at({static_cast<std::uint16_t>(place.memory_id), place.generation});
    }

    [[nodiscard]] registered_medium_record_t* resolve_(medium_reference_t place) noexcept {
        return media_.at({static_cast<std::uint16_t>(place.medium_id), place.generation});
    }

    /** Marks the range and the medium of @p request as live, so neither can be taken back under it. */
    void count_request_on_slots_(request_t const& request) noexcept {
        if (registered_memory_record_t* const record = resolve_(request.buffer); record)
            in_flight_(*record).store_add(1, std::memory_order_acquire);
        if (registered_medium_record_t* const record = resolve_(request.medium); record)
            in_flight_(*record).store_add(1, std::memory_order_acquire);
    }

    /** Releases both once the device is done with the bytes of @p request. */
    void uncount_request_on_slots_(request_t const& request) noexcept {
        if (registered_memory_record_t* const record = resolve_(request.buffer); record)
            in_flight_(*record).store_sub(1, std::memory_order_release);
        if (registered_medium_record_t* const record = resolve_(request.medium); record)
            in_flight_(*record).store_sub(1, std::memory_order_release);
    }

    /** Resolves a request's buffer and performs it on the device for @p queue, reporting every
     *  defect per request. */
    [[nodiscard]] completion_t perform_one_(blocking_queue_t const& queue, request_t const& request) noexcept {
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

        disk_capabilities_t const& limits = capabilities_;
        registered_memory_record_t const* const slot = resolve_(request.buffer);
        bool const own = request.buffer.memory_id == registered_memory_id_t::queue_range_k;
        if (!slot && !own) {
            completion.state = completion_state_t::failed_k;
            completion.status = {status_code_t::invalid_argument_k, "The request names no registered range"};
            return completion;
        }
        byte_t* const base = own ? queue.range().data() : slot->base;
        std::uint64_t const length = own ? queue.range().size() : slot->length;
        if (request.buffer.offset > length || request.extent.length > length - request.buffer.offset) {
            completion.state = completion_state_t::failed_k;
            completion.status = {status_code_t::invalid_argument_k, "The request runs past its registered range"};
            return completion;
        }
        byte_t* const address = base + request.buffer.offset;
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
                                        ? device_.read_extent(slot_index, request.extent, buffer, queue.event())
                                        : device_.write_extent(slot_index, request.extent, buffer, queue.event());
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

    /** The transport this disk moves bytes through, owned here. */
    device_type_ device_;

    /** What this disk accepts, read from the transport once at mount. */
    disk_capabilities_t capabilities_;

    /** One record per memory range the ring pins. */
    registered_memory_pool_t registered_memories_;

    /** One record per medium the ring registers. */
    registered_medium_pool_t media_;

    /** Zero while no thread takes or vacates a range record, one while one does. */
    std::int32_t registered_memories_claim_ = 0;

    /** Zero while no thread takes or vacates a medium record, one while one does. */
    std::int32_t media_claim_ = 0;
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
            return {status_code_t::invalid_argument_k, "The slot is not an empty device slot"};
        if (request.length == 0) return {status_code_t::invalid_argument_k, "A volume holds at least one byte"};
        auto [storage, storage_status] = storage_t::make(static_cast<std::size_t>(request.length));
        if (storage_status != st::status_t::success_k ||
            storage.resize(static_cast<std::size_t>(request.length)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A volume of that size does not fit"};
        volumes_[index] = std::move(storage);
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
    expected<std::uint64_t> read_extent(std::uint16_t index, extent_t extent, std::span<byte_t> destination,
                                        native_file_event_t const&) noexcept {
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
    expected<std::uint64_t> write_extent(std::uint16_t index, extent_t extent, std::span<byte_t const> source,
                                         native_file_event_t const&) noexcept {
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
    [[nodiscard]] disk_capabilities_t capabilities() const noexcept {
        disk_capabilities_t reported;
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

#pragma region Platform

/** The two alignments unbuffered access demands, which every filesystem answers differently. */
struct direct_alignment_t {

    /** Alignment the first byte of a registered buffer must meet. */
    std::uint64_t memory = portable_alignment_k;

    /** Alignment an extent's offset and its length must both meet, which is the
     *  medium's logical block. */
    std::uint64_t offset = portable_alignment_k;
};

/** The file access a medium's access asks the opener for: the one conversion from the ring's
 *  vocabulary to the file layer's. */
constexpr file_access_t file_access_of(medium_access_t access) noexcept {
    switch (access) {
    case medium_access_t::read_only_k: return file_access_t::read_only_k;
    case medium_access_t::read_write_k: return file_access_t::read_write_k;
    case medium_access_t::create_read_write_k: return file_access_t::create_read_write_k;
    }
    return file_access_t::read_only_k;
}

#if USTORE_OS_LINUX_

/** The bytes a regular file holds, or the whole medium behind a block device, which @c fstat sizes at zero. */
inline std::uint64_t medium_bytes_of(native_file_t file) noexcept {
    struct stat description {};
    if (::fstat(file, &description) != 0) return 0;
    if (!S_ISBLK(description.st_mode)) return static_cast<std::uint64_t>(description.st_size);
    std::uint64_t bytes = 0;
    return ::ioctl(file, BLKGETSIZE64, &bytes) == 0 ? bytes : 0;
}

#else

/** The bytes the file holds, which is all a medium is where no raw device is addressed. */
inline std::uint64_t medium_bytes_of(native_file_t file) noexcept { return native_file_capacity(file); }

#endif

#if USTORE_OS_WINDOWS_

/** Gives @p extent of the file back, which reads as zeros afterwards. */
inline status_t deallocate_native_file(native_file_t file, extent_t extent) noexcept {
    FILE_ZERO_DATA_INFORMATION range {};
    range.FileOffset.QuadPart = static_cast<LONGLONG>(extent.offset);
    range.BeyondFinalZero.QuadPart = static_cast<LONGLONG>(extent.end());
    DWORD returned = 0;
    if (!DeviceIoControl(file, FSCTL_SET_ZERO_DATA, &range, sizeof(range), nullptr, 0, &returned, nullptr))
        return native_file_failure(::GetLastError(), "The file would not deallocate");
    return {};
}

#elif USTORE_OS_LINUX_

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

#elif USTORE_OS_APPLE_

/** Gives @p extent of a file back as a hole, which reads as zeros afterwards. */
inline status_t deallocate_native_file(native_file_t file, extent_t extent) noexcept {
    fpunchhole_t hole {};
    hole.fp_offset = static_cast<off_t>(extent.offset);
    hole.fp_length = static_cast<off_t>(extent.length);
    if (fcntl(file, F_PUNCHHOLE, &hole) != 0) return native_file_failure(errno, "The file would not punch a hole");
    return {};
}

#else

/** Refuses, since this platform offers no way to give a file's blocks back. */
inline status_t deallocate_native_file(native_file_t, extent_t) noexcept {
    return {status_code_t::unsupported_k, "This platform cannot punch file holes"};
}

#endif

#if USTORE_OS_LINUX_

#pragma region Kernel

/** @c statx_timestamp. */
struct linux_statx_timestamp_t {
    std::int64_t tv_sec;
    std::uint32_t tv_nsec;
    std::int32_t reserved;
};

/** @c statx, with the unbuffered-read alignments Linux 6.1 added. */
struct linux_statx_t {
    std::uint32_t mask;
    std::uint32_t blksize;
    std::uint64_t attributes;
    std::uint32_t nlink;
    std::uint32_t uid;
    std::uint32_t gid;
    std::uint16_t mode;
    std::uint16_t spare0;
    std::uint64_t ino;
    std::uint64_t size;
    std::uint64_t blocks;
    std::uint64_t attributes_mask;
    linux_statx_timestamp_t atime;
    linux_statx_timestamp_t btime;
    linux_statx_timestamp_t ctime;
    linux_statx_timestamp_t mtime;
    std::uint32_t rdev_major;
    std::uint32_t rdev_minor;
    std::uint32_t dev_major;
    std::uint32_t dev_minor;
    std::uint64_t mnt_id;
    std::uint32_t dio_mem_align;
    std::uint32_t dio_offset_align;
    std::uint64_t spare3[12];
};
static_assert(sizeof(linux_statx_t) == 256, "the kernel writes a 256-byte statx");

#pragma endregion Kernel

/** Asks @p file's filesystem what unbuffered access demands, falling back to
 *  the portable numbers. The @c STATX_DIOALIGN mask bit is from Linux 6.1. */
[[nodiscard]] inline direct_alignment_t direct_alignment_of(native_file_t file) noexcept {
    std::uint32_t const dioalign = 0x2000; // STATX_DIOALIGN
    linux_statx_t description {};
    if (::syscall(SYS_statx, file, "", AT_EMPTY_PATH, dioalign, &description) != 0) return {};
    if ((description.mask & dioalign) == 0) return {};
    if (description.dio_mem_align == 0 || description.dio_offset_align == 0) return {};
    return direct_alignment_t {description.dio_mem_align, description.dio_offset_align};
}

#else

/** Answers the portable numbers, which this platform offers no way to ask the filesystem for. */
[[nodiscard]] inline direct_alignment_t direct_alignment_of(native_file_t) noexcept { return {}; }

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
            return {status_code_t::invalid_argument_k, "The slot is not an empty device slot"};
        if (!request.name) return {status_code_t::invalid_argument_k, "A file medium needs a path"};
        auto [file, open_status] = open_native_file(request.name, file_access_of(request.access), buffering_);
        if (open_status.failed()) return open_status;
        if (request.access == medium_access_t::create_read_write_k && request.length != 0)
            if (status_t const sized = truncate_native_file(file, request.length); sized.failed()) {
                [[maybe_unused]] status_t const closed = close_native_file(file);
                return sized;
            }
        files_[index] = file;
        return {};
    }

    /** Closes the file in @p index, leaving its slot free for the next medium. */
    status_t close_medium(std::uint16_t index) noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k)
            return {status_code_t::invalid_argument_k, "The medium is not this device's"};
        return close_native_file(std::exchange(files_[index], invalid_native_file_k));
    }

    /** Reads @p extent of @p index into @p destination, waiting on the polling queue's @p event,
     *  answering the prefix that arrived. */
    expected<std::uint64_t> read_extent(std::uint16_t index, extent_t extent, std::span<byte_t> destination,
                                        native_file_event_t const& event) noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k)
            return status_t {status_code_t::invalid_argument_k, "The medium is not this device's"};
        return read_native_file(files_[index], extent.offset,
                                destination.first(static_cast<std::size_t>(extent.length)), event);
    }

    /** Writes @p source at @p extent of @p index, waiting on the polling queue's @p event,
     *  answering the prefix that landed. */
    expected<std::uint64_t> write_extent(std::uint16_t index, extent_t extent, std::span<byte_t const> source,
                                         native_file_event_t const& event) noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k)
            return status_t {status_code_t::invalid_argument_k, "The medium is not this device's"};
        return write_native_file(files_[index], extent.offset, source.first(static_cast<std::size_t>(extent.length)),
                                 event);
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
        return medium_bytes_of(files_[index]);
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
    [[nodiscard]] disk_capabilities_t capabilities() const noexcept {
        disk_capabilities_t reported;
        std::uint64_t const alignment = buffering_ == file_buffering_t::unbuffered_k ? portable_alignment_k : 1;
        reported.memory_alignment = alignment;
        reported.offset_alignment = alignment;
        reported.length_granularity = alignment;
        reported.max_transfer_bytes = native_transfer_bytes_k;
        reported.reaches_gpu_memory = false;
        return reported;
    }

  private:

    /** Closes every open file, which both the destructor and a move assignment need. */
    void close_all_() noexcept {
        for (std::size_t index = 0; index != files_.size(); ++index)
            if (files_[index] != invalid_native_file_k) [[maybe_unused]]
                status_t const closed = close_native_file(files_[index]);
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
