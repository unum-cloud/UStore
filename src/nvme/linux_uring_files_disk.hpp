/**
 *  @file src/nvme/linux_uring_files_disk.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The ring Linux offers: @c O_DIRECT files driven through raw @c io_uring syscalls.
 *
 *  Nothing here links liburing. A ring is set up with @c io_uring_setup, mapped with @c mmap, fed
 *  with @c io_uring_register and entered with @c io_uring_enter, which is the whole interface and
 *  needs Linux 6.12 or newer for @c DEFER_TASKRUN, @c SINGLE_ISSUER and @c NO_SQARRAY together.
 *
 *  A ring belongs to one queue pair rather than to the disk, because @c SINGLE_ISSUER binds a ring to one
 *  task and answers @c -EEXIST to every other, for @c io_uring_enter and @c io_uring_register alike.
 *  The binding happens at @c io_uring_setup unless the ring is set up disabled, so every ring is
 *  created with @c IORING_SETUP_R_DISABLED at mount - where mapping and the sparse tables can still
 *  fail loudly - and bound to its thread by @c IORING_REGISTER_ENABLE_RINGS when the queue pair is taken.
 *
 *  Because only the bound thread may register on its own ring, a registered buffer or medium is
 *  recorded on the disk and copied into a ring's tables by that ring's own thread, at the submission
 *  that first needs it. The disk counts registered memory and medium epochs; a ring rewrites its table
 *  when its epoch is behind. Pinning the ranges is an optimisation the locked-memory cap may refuse,
 *  since every ring is charged for the whole arena; a ring refused the pin addresses them by pointer.
 */

#pragma once

#include <cassert> // `assert`
#include <cerrno>  // `errno`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memset`

#if defined(__linux__)
#include <fcntl.h>          // `AT_EMPTY_PATH`
#include <linux/io_uring.h> // `io_uring_sqe`, `io_uring_cqe`, `IORING_SETUP_DEFER_TASKRUN`
#include <linux/stat.h>     // `struct statx`, `STATX_DIOALIGN`
#include <sys/mman.h>       // `mmap`, `munmap`, `MAP_SHARED`
#include <sys/syscall.h>    // `__NR_io_uring_setup`, `__NR_io_uring_enter`, `__NR_statx`
#include <sys/uio.h>        // `iovec`
#include <unistd.h>         // `syscall`, `close`, `gettid`
#endif

#include <array>   // `std::array`
#include <atomic>  // `std::atomic_ref`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <forkunion/atomics.hpp>       // `fu::preferred_atomic_ref`
#include <forkunion/capabilities.hpp>  // `fu::preferred_yield_t`
#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/blocking_files_disk.hpp" // `native_file_t`, `open_native_file`, `native_file_capacity`
#include "nvme/extent.hpp"              // `extent_t`, `portable_alignment_k`
#include "nvme/ring.hpp"                // `request_t`, `completion_t`, `queue_pair_t`

namespace unum::ustore::nvme {

#pragma region Configuration

/**
 *  @brief What a mount fixes about an io_uring disk before any queue pair queue pairs a share of it.
 *
 *  Depth is the one number worth tuning: sixteen outstanding reads already beat @c pread elevenfold
 *  on one namespace, sixty-four reach sixteenfold, and everything past a hundred and twenty-eight is
 *  flat, so the default sits where the curve does.
 */
struct linux_uring_disk_config_t {

    /** Leases the disk hands out at once, which is also the number of rings it creates. */
    std::uint16_t queue_pairs_limit = 1;

    /** Requests one queue pair may keep in flight, which is the credit a queue pair spends and polls back. */
    queue_depth_t depth_limit = 64;

    /** Memory ranges the disk pins at once, which every ring keeps a table slot for. */
    std::uint16_t registered_memory_limit = 8;

    /** Targets the disk registers at once, which a directory of sorted runs raises above one. */
    std::uint16_t media_limit = 1;

    /** Whether the page cache stands between the ring and the medium. */
    file_buffering_t buffering = file_buffering_t::unbuffered_k;
};

/** Whether this machine offers the ring, which a mount asks once before it chooses a transport. */
enum class linux_uring_probe_t : std::uint8_t {

    /** The kernel set a ring up with every flag this transport needs. */
    available_k,

    /** The kernel has io_uring and refused it, which a seccomp profile or @c io_uring_disabled does. */
    refused_k,

    /** The kernel has no io_uring, or none that carries the flags this transport needs. */
    too_old_k,
};

/** Whether the kernel holds the registered ranges pinned, which the locked-memory cap decides at first submission. */
enum class range_pinning_t : std::uint8_t {

    /** Pinned once at registered memory, so a request names a table slot and the kernel pins nothing per request. */
    pinned_k,

    /** The pin exceeded the locked-memory cap, so a request carries its pointer and the kernel pins per request. */
    unpinned_k,
};

#pragma endregion Configuration

#if defined(__linux__)

#pragma region Kernel

/*  Older distribution headers carry the syscall and miss the newer constants, so the few this
 *  transport needs are spelled out rather than made a build-time requirement. */
#if !defined(IORING_SETUP_SINGLE_ISSUER)
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif
#if !defined(IORING_SETUP_DEFER_TASKRUN)
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif
#if !defined(IORING_SETUP_NO_SQARRAY)
#define IORING_SETUP_NO_SQARRAY (1U << 16)
#endif
#if !defined(IORING_RSRC_REGISTER_SPARSE)
#define IORING_RSRC_REGISTER_SPARSE (1U << 0)
#endif
#if !defined(IORING_ASYNC_CANCEL_ANY)
#define IORING_ASYNC_CANCEL_ANY (1U << 2)
#endif
#if !defined(IORING_SQ_CQ_OVERFLOW)
#define IORING_SQ_CQ_OVERFLOW (1U << 1)
#endif

/** The two alignments unbuffered access demands, which every filesystem answers differently. */
struct direct_alignment_t {

    /** Alignment the first byte of a registered buffer must meet. */
    std::uint64_t memory = portable_alignment_k;

    /** Alignment an extent's offset and its length must both meet. */
    std::uint64_t offset = portable_alignment_k;
};

#if defined(STATX_DIOALIGN)

/** Asks @p file's filesystem what unbuffered access demands, falling back to the portable numbers. */
[[nodiscard]] inline direct_alignment_t direct_alignment_of(native_file_t file) noexcept {
    struct statx description {};
    if (::syscall(__NR_statx, file, "", AT_EMPTY_PATH, STATX_DIOALIGN, &description) != 0) return {};
    if ((description.stx_mask & STATX_DIOALIGN) == 0) return {};
    if (description.stx_dio_mem_align == 0 || description.stx_dio_offset_align == 0) return {};
    return direct_alignment_t {description.stx_dio_mem_align, description.stx_dio_offset_align};
}

#else

/** Answers the portable numbers, since the build headers cannot ask for the filesystem's own. */
[[nodiscard]] inline direct_alignment_t direct_alignment_of(native_file_t) noexcept { return {}; }

#endif

#pragma endregion Kernel

#pragma region Disk

/**
 *  @brief The ring Linux offers, over one or more @c O_DIRECT files registered as media.
 *
 *  This is not a @c basic_disk over a blocking device: the queue is the kernel's, so the disk drives
 *  it directly and only the bookkeeping is its own. @c submit writes submission entries and enters
 *  once, @c poll enters with @c min_complete of zero to run the deferred completion work and copies
 *  whatever landed, and neither ever waits.
 *
 *  A defect the ring can see before the kernel does - a stale registered memory, an extent past its
 *  range, a length past one transfer, an alignment the filesystem refuses - never fails the batch.
 *  It becomes that one request's completion and leaves its neighbours alone, which is what a chain
 *  of submission entries would do anyway. Only a token that is not this queue pair's fails the whole
 *  @c submit, since a misrouted completion is unrecoverable.
 */
class linux_uring_files_disk_t {
  public:

    linux_uring_files_disk_t() noexcept = default;
    linux_uring_files_disk_t(linux_uring_files_disk_t const&) = delete;
    linux_uring_files_disk_t& operator=(linux_uring_files_disk_t const&) = delete;
    linux_uring_files_disk_t(linux_uring_files_disk_t&&) noexcept = default;

    linux_uring_files_disk_t& operator=(linux_uring_files_disk_t&& other) noexcept {
        if (this == &other) return *this;
        close_all_();
        queue_pairs_ = std::move(other.queue_pairs_);
        rings_ = std::move(other.rings_);
        registered_memories_ = std::move(other.registered_memories_);
        media_ = std::move(other.media_);
        flights_ = std::move(other.flights_);
        locals_ = std::move(other.locals_);
        vectors_ = std::move(other.vectors_);
        registered_media_ = std::move(other.registered_media_);
        registered_memories_epoch_ = other.registered_memories_epoch_;
        media_epoch_ = other.media_epoch_;
        pin_refused_epoch_ = other.pin_refused_epoch_;
        depth_limit_ = other.depth_limit_;
        queue_pairs_limit_ = other.queue_pairs_limit_;
        registered_memories_limit_ = other.registered_memories_limit_;
        media_limit_ = other.media_limit_;
        buffering_ = other.buffering_;
        return *this;
    }

    ~linux_uring_files_disk_t() noexcept { close_all_(); }

    /**
     *  @brief Sets one tiny ring up and takes it down again, which is the only honest availability test.
     *  @return Whether the kernel offers the flags this transport needs, or why it does not.
     */
    [[nodiscard]] static linux_uring_probe_t probe() noexcept {
        io_uring_params parameters {};
        parameters.flags = ring_setup_flags_k;
        long const opened = ::syscall(__NR_io_uring_setup, probe_entries_k, &parameters);
        if (opened >= 0) {
            ::close(static_cast<int>(opened));
            return linux_uring_probe_t::available_k;
        }
        switch (errno) {
        case ENOSYS:
        case EINVAL: return linux_uring_probe_t::too_old_k;
        default: return linux_uring_probe_t::refused_k;
        }
    }

    /**
     *  @brief Creates one disabled ring per queue pair and gives each the sparse tables it will fill.
     *  @param[in] config Leases, per-queue pair depth, the ranges and media pinned, and the buffering.
     *
     *  @note No file is opened here; a mount registers each one as a medium, so one disk serves a
     *      directory of many sorted runs rather than one disk per run.
     */
    static expected<linux_uring_files_disk_t> make(linux_uring_disk_config_t const& config) noexcept {
        if (config.queue_pairs_limit == 0 || config.depth_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A ring needs a queue pair and a request of depth"};
        if (config.queue_pairs_limit == static_cast<std::uint16_t>(queue_pair_id_t::unallocated_k) ||
            config.registered_memory_limit == static_cast<std::uint16_t>(registered_memory_id_t::unregistered_k) ||
            config.media_limit == static_cast<std::uint16_t>(registered_medium_id_t::unregistered_k))
            return status_t {status_code_t::invalid_argument_k, "A ring keeps one identifier for an empty handle"};
        if (config.media_limit == 0 || config.registered_memory_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A ring needs a medium and a range to move between"};
        if (config.depth_limit > depth_ceiling_k)
            return status_t {status_code_t::invalid_argument_k, "A ring holds fewer requests than that"};

        std::size_t const window = static_cast<std::size_t>(config.queue_pairs_limit) * config.depth_limit;
        linux_uring_files_disk_t disk;
        disk.depth_limit_ = config.depth_limit;
        disk.queue_pairs_limit_ = config.queue_pairs_limit;
        disk.registered_memories_limit_ = config.registered_memory_limit;
        disk.media_limit_ = config.media_limit;
        disk.buffering_ = config.buffering;
        if (status_t const sized = size_(disk.queue_pairs_, config.queue_pairs_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.rings_, config.queue_pairs_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.registered_memories_, config.registered_memory_limit); sized.failed())
            return sized;
        if (status_t const sized = size_(disk.media_, config.media_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.flights_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.locals_, window); sized.failed()) return sized;
        if (status_t const sized = size_(
                disk.vectors_, static_cast<std::size_t>(config.queue_pairs_limit) * config.registered_memory_limit);
            sized.failed())
            return sized;
        if (status_t const sized = size_(disk.registered_media_,
                                         static_cast<std::size_t>(config.queue_pairs_limit) * config.media_limit);
            sized.failed())
            return sized;
        for (std::size_t index = 0; index != disk.rings_.size(); ++index)
            if (status_t const created = disk.create_ring_(disk.rings_[index]); created.failed()) return created;
        return disk;
    }

    /** What this disk accepts, which the tightest alignment across its registered media fixes. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept {
        std::uint64_t memory = 0;
        std::uint64_t offset = 0;
        for (std::size_t index = 0; index != media_.size(); ++index) {
            registered_medium_slot_t const& slot = media_[index];
            if (slot.registered == 0) continue;
            memory = slot.memory_alignment > memory ? slot.memory_alignment : memory;
            offset = slot.offset_alignment > offset ? slot.offset_alignment : offset;
        }
        std::uint64_t const unregistered = buffering_ == file_buffering_t::unbuffered_k ? portable_alignment_k
                                                                                        : std::uint64_t {1};
        ring_capabilities_t reported;
        reported.memory_alignment = memory != 0 ? memory : unregistered;
        reported.offset_alignment = offset != 0 ? offset : unregistered;
        reported.length_granularity = reported.offset_alignment;
        reported.max_transfer_bytes = max_transfer_bytes_k;
        reported.depth_limit = depth_limit_;
        reported.queue_pairs_limit = queue_pairs_limit_;
        reported.media_limit = media_limit_;
        reported.reaches_gpu_memory = false;
        return reported;
    }

    /** How the rings address the registered ranges, settled by the first ring the cap refused at this epoch. */
    [[nodiscard]] range_pinning_t pinning() const noexcept {
        return pin_refused_epoch_ == registered_memories_epoch_ ? range_pinning_t::unpinned_k
                                                                : range_pinning_t::pinned_k;
    }

#pragma region Queue Pairs

    /**
     *  @brief Takes a queue pair on @p depth requests of one ring, and binds that ring to this thread.
     *  @param[in] depth Requests this queue pair may keep in flight, at most the mount's limit.
     *  @return The queue pair, or @c capacity_exhausted_k when every ring is already out.
     */
    expected<queue_pair_t> acquire_queue_pair(queue_depth_t depth) noexcept {
        if (depth == 0 || depth > depth_limit_)
            return status_t {status_code_t::invalid_argument_k, "A queue pair asks for more depth than the ring has"};
        thread_identity_t const identity = bound_thread_identity_();
        for (std::size_t index = 0; index != queue_pairs_.size(); ++index) {
            queue_pair_slot_t& slot = queue_pairs_[index];
            std::int32_t expected_free = 0;
            if (!claim_(slot.leased)
                     .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            if (status_t const bound = bind_ring_(rings_[index], identity); bound.failed()) {
                claim_(slot.leased).store(0, std::memory_order_release);
                return bound;
            }
            slot.depth = depth;
            slot.flights_count = 0;
            slot.locals_first = 0;
            slot.locals_count = 0;
            slot.bound_thread = identity;
            return queue_pair_t::leased(static_cast<queue_pair_id_t>(index), slot.generation, depth);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every queue pair of this ring is already held"};
    }

    /**
     *  @brief Binds @p queue pair and its ring to the calling thread, which is how a moved queue pair changes hands.
     *  @param[in] queue pair The queue pair this thread has just taken over, with nothing outstanding on it.
     *
     *  @note A ring cannot change its bound thread, so a handover to another thread builds a fresh ring in
     *      the queue pair's slot; that is why the queue pair must be empty before it moves.
     */
    status_t bind_queue_pair(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        if (slot->flights_count != 0 || slot->locals_count != 0)
            return {status_code_t::invalid_argument_k, "Drain the queue pair before handing it over"};
        thread_identity_t const identity = bound_thread_identity_();
        if (status_t const bound = bind_ring_(rings_[static_cast<std::size_t>(queue_pair.id())], identity);
            bound.failed())
            return bound;
        slot->bound_thread = identity;
        return {};
    }

    /**
     *  @brief Takes @p queue pair back once it has nothing outstanding, and empties the handle.
     *  @param[inout] queue pair The queue pair to return, surrendered on success and untouched on failure.
     */
    status_t release_queue_pair(queue_pair_t& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        if (slot->flights_count != 0 || slot->locals_count != 0)
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
        return slot ? slot->flights_count + slot->locals_count : 0;
    }

#pragma endregion Queue Pairs

#pragma region Registrations

    /**
     *  @brief Records @p memory so requests can address it by slot, and raises the registered memory epoch.
     *  @param[in] memory Memory the caller owns and keeps alive until it deregisters the range.
     *  @param[in] space Where that memory lives; this transport reaches host memory alone.
     *
     *  @note The kernel learns of the range on each ring's own thread, at the next submission there,
     *      since @c io_uring_register answers @c -EEXIST to anyone but the ring's bound_thread.
     */
    expected<registered_memory_t> register_memory(std::span<byte_t> memory, memory_space_t space) noexcept {
        if (memory.empty()) return status_t {status_code_t::invalid_argument_k, "An empty range registers nothing"};
        if (space == memory_space_t::gpu_k)
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
            epoch_(registered_memories_epoch_).add(1, std::memory_order_release);
            return registered_memory_t::registered(static_cast<registered_memory_id_t>(index), slot.generation, memory,
                                                   space);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every registered_memory slot of this ring is taken"};
    }

    /**
     *  @brief Gives @p registered memory back, refusing while the kernel may still be writing into it.
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
        epoch_(registered_memories_epoch_).add(1, std::memory_order_release);
        registered_memory.surrender();
        return {};
    }

    /**
     *  @brief Opens the file @p request names and records it, reading the alignments it demands.
     *  @param[in] request The path, the bytes to create it with, and what the caller may do to it.
     *  @return The handle, or @c capacity_exhausted_k when every medium slot is taken.
     */
    expected<registered_medium_t> register_medium(medium_options_t const& request) noexcept {
        if (!request.name) return status_t {status_code_t::invalid_argument_k, "A file medium needs a path"};
        for (std::size_t index = 0; index != media_.size(); ++index) {
            registered_medium_slot_t& slot = media_[index];
            std::int32_t expected_free = 0;
            if (!claim_(slot.registered)
                     .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            auto [file, open_native_file_status] = open_native_file(request.name, request.access, buffering_);
            if (open_native_file_status.failed()) {
                claim_(slot.registered).store(0, std::memory_order_release);
                return open_native_file_status;
            }
            if (request.access == medium_access_t::create_read_write_k && request.length != 0)
                if (status_t const sized = truncate_native_file(file, request.length); sized.failed()) {
                    close_native_file(file);
                    claim_(slot.registered).store(0, std::memory_order_release);
                    return sized;
                }
            direct_alignment_t const alignment = buffering_ == file_buffering_t::unbuffered_k
                                                     ? direct_alignment_of(file)
                                                     : direct_alignment_t {1, 1};
            slot.file = file;
            slot.capacity = native_file_capacity(file);
            slot.memory_alignment = alignment.memory;
            slot.offset_alignment = alignment.offset;
            epoch_(media_epoch_).add(1, std::memory_order_release);
            return registered_medium_t::registered(static_cast<registered_medium_id_t>(index), slot.generation,
                                                   slot.capacity);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every medium slot of this ring is taken"};
    }

    /**
     *  @brief Closes @p registered medium, refusing while requests against it are still in flight.
     *  @param[inout] registered medium The medium to give back, surrendered on success.
     */
    status_t unregister_medium(registered_medium_t& registered_medium) noexcept {
        registered_medium_slot_t* const slot = slot_of_(registered_medium);
        if (!slot) return {status_code_t::invalid_argument_k, "The medium is not this ring's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "The medium is still named by requests in flight"};
        close_native_file(slot->file);
        ++slot->generation;
        slot->file = invalid_native_file_k;
        slot->capacity = 0;
        claim_(slot->registered).store(0, std::memory_order_release);
        epoch_(media_epoch_).add(1, std::memory_order_release);
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
     */
    expected<std::size_t> submit(queue_pair_t const& queue_pair, std::span<request_t const> requests) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        assert(slot->bound_thread == bound_thread_identity_() && "Only the thread holding a queue pair submits on it");
        for (request_t const& request : requests)
            if (request.token.queue_pair() != queue_pair.id() || request.token.generation() != queue_pair.generation())
                return status_t {status_code_t::invalid_argument_k, "A request carries another queue pair's token"};

        std::size_t const seat = static_cast<std::size_t>(queue_pair.id());
        ring_t& ring = rings_[seat];
        if (status_t const carried = carry_tables_(ring, seat); carried.failed()) return carried;

        std::uint32_t const base = window_of_(queue_pair.id());
        std::size_t const credit = slot->depth - (slot->flights_count + slot->locals_count);
        unsigned const consumed = std::atomic_ref<unsigned> {*ring.submission_head}.load(std::memory_order_acquire);
        unsigned const published = *ring.submission_tail;
        std::size_t const seats = ring.submission_entries_count - (published - consumed);
        std::size_t const room = credit < seats ? credit : seats;
        std::size_t const taking = requests.size() < room ? requests.size() : room;

        io_uring_sqe* linked = nullptr;
        std::size_t queued = 0;
        bool cut = false;
        for (std::size_t index = 0; index != taking; ++index) {
            request_t const& request = requests[index];
            if (cut) {
                unlink_(linked);
                push_local_(*slot, base, cancelled_completion_(request.token));
                cut = chains_next_(request);
                continue;
            }
            status_t const defect = inspect_(request);
            if (defect.failed()) {
                unlink_(linked);
                push_local_(*slot, base, failed_completion_(request.token, defect));
                cut = chains_next_(request);
                continue;
            }
            io_uring_sqe& entry = ring.submission_entries[(published + queued) & ring.submission_mask];
            build_(entry, request, ring.pinning);
            borrow_(request);
            track_(*slot, base, request);
            linked = chains_next_(request) ? &entry : nullptr;
            ++queued;
            cut = false;
        }
        unlink_(linked);

        if (queued != 0) {
            std::atomic_ref<unsigned> {*ring.submission_tail}.store(static_cast<unsigned>(published + queued),
                                                                    std::memory_order_release);
            std::size_t accepted = 0;
            while (accepted != queued) {
                long const entered = ::syscall(__NR_io_uring_enter, ring.file, static_cast<unsigned>(queued - accepted),
                                               0u, 0u, nullptr, 0);
                if (entered > 0) {
                    accepted += static_cast<std::size_t>(entered);
                    continue;
                }
                if (entered < 0 && errno == EINTR) continue;
                break;
            }
            if (accepted != queued)
                refuse_tail_(*slot, base, ring, static_cast<unsigned>(published + accepted), queued - accepted);
        }
        return taking;
    }

    /**
     *  @brief Runs this ring's deferred completion work and copies what landed into @p into.
     *  @param[in] queue pair The queue pair polling, held by the calling thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero when nothing has landed.
     *
     *  @note The entry carries @c min_complete of zero, so the call performs the work a deferred ring
     *      only performs on its owning thread and returns without waiting for anything.
     */
    expected<std::size_t> poll_completions(queue_pair_t const& queue_pair, std::span<completion_t> into) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        assert(slot->bound_thread == bound_thread_identity_() && "Only the thread holding a queue pair polls on it");
        std::uint32_t const base = window_of_(queue_pair.id());
        ring_t& ring = rings_[static_cast<std::size_t>(queue_pair.id())];

        std::size_t moved = 0;
        if (slot->flights_count != 0) enter_for_completions_(ring);
        moved += poll_completions_(*slot, base, ring, into);
        if (overflowed_(ring) && moved != into.size()) {
            enter_for_completions_(ring);
            moved += poll_completions_(*slot, base, ring, into.subspan(moved));
        }
        moved += drain_locals_(*slot, base, into.subspan(moved));
        return moved;
    }

    /**
     *  @brief Asks the kernel to stop everything @p queue pair has in flight, which poll reports cancelled.
     *  @param[in] queue pair The queue pair to stop, whose started requests still run to their own end.
     *
     *  @note The cancelling entry posts a completion of its own, which poll recognises and drops, so
     *      the queue pair still sees exactly one completion per request it submitted.
     */
    void cancel(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot || slot->flights_count == 0) return;
        ring_t& ring = rings_[static_cast<std::size_t>(queue_pair.id())];
        unsigned const consumed = std::atomic_ref<unsigned> {*ring.submission_head}.load(std::memory_order_acquire);
        unsigned const published = *ring.submission_tail;
        if (published - consumed >= ring.submission_entries_count) return;

        io_uring_sqe& entry = ring.submission_entries[published & ring.submission_mask];
        std::memset(&entry, 0, sizeof(entry));
        entry.opcode = IORING_OP_ASYNC_CANCEL;
        entry.cancel_flags = IORING_ASYNC_CANCEL_ANY;
        entry.user_data = cancel_marker_k;
        std::atomic_ref<unsigned> {*ring.submission_tail}.store(published + 1, std::memory_order_release);
        [[maybe_unused]] long const entered = ::syscall(__NR_io_uring_enter, ring.file, 1u, 0u, 0u, nullptr, 0);
    }

    /**
     *  @brief Finishes and discards everything @p queue pair has outstanding, so its buffers may be freed.
     *  @param[in] queue pair The queue pair to empty, which may be cancelled or still running.
     */
    status_t drain(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t const* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        std::array<completion_t, drain_batch_k> discarded {};
        for (std::size_t spins = 0; spins != drain_spin_limit_k; ++spins) {
            if (slot->flights_count == 0 && slot->locals_count == 0) return {};
            auto [moved, poll_completions_status] = poll_completions(queue_pair, std::span<completion_t> {discarded});
            if (poll_completions_status.failed()) return poll_completions_status;
            if (moved != 0) continue;
            if (spins == drain_patience_k) cancel(queue_pair);
            fu::preferred_yield_t {}(spins, spins, std::size_t {0});
        }
        return {status_code_t::input_output_k, "The queue pair would not drain"};
    }

#pragma endregion Ring

  private:

    /** One io_uring instance, entered and registered on by exactly one thread for its whole life. */
    struct ring_t {

        /** The ring's registered medium, @c -1 before it is created and after it is taken down. */
        int file = -1;

        /** The shared submission and completion rings, mapped as one region. */
        void* shared = nullptr;

        /** Bytes the shared mapping spans, which only the unmapping needs. */
        std::size_t shared_bytes = 0;

        /** The submission entries, mapped apart from the rings that index them. */
        io_uring_sqe* submission_entries = nullptr;

        /** Bytes the submission entry mapping spans. */
        std::size_t submission_entries_bytes = 0;

        /** Where the kernel publishes how much of the submission queue it has consumed. */
        unsigned* submission_head = nullptr;

        /** Where this thread publishes how much of the submission queue it has filled. */
        unsigned* submission_tail = nullptr;

        /** Where the kernel raises @c IORING_SQ_CQ_OVERFLOW and its siblings. */
        unsigned* submission_flags = nullptr;

        /** Index mask of the submission queue, one less than its power-of-two length. */
        unsigned submission_mask = 0;

        /** Entries the submission queue holds, which bounds one batch. */
        unsigned submission_entries_count = 0;

        /** Where this thread publishes how many completions it has taken. */
        unsigned* completion_head = nullptr;

        /** Where the kernel publishes how many completions it has posted. */
        unsigned* completion_tail = nullptr;

        /** Index mask of the completion queue, one less than its power-of-two length. */
        unsigned completion_mask = 0;

        /** The completion entries themselves. */
        io_uring_cqe* completion_entries = nullptr;

        /** The thread this ring is bound to, zero while it is still disabled. */
        thread_identity_t bound_thread = 0;

        /** The registered memory epoch this ring's buffer table was last written from. */
        std::int32_t carried_registered_memories = 0;

        /** The medium epoch this ring's file table was last written from. */
        std::int32_t carried_media = 0;

        /** How this ring addresses the registered ranges, settled when its buffer table was last written. */
        range_pinning_t pinning = range_pinning_t::pinned_k;
    };

    /** One queue pair's share of the disk: its ring, its credit and the two queues it polls from. */
    struct queue_pair_slot_t {

        /** Zero while the slot is free, one while a queue pair holds it. */
        std::int32_t leased = 0;

        /** Raised every time the slot is leased again, so a stale token routes nowhere. */
        queue_pair_generation_t generation = 0;

        /** Requests this queue pair may keep in flight, fixed when the queue pair was taken. */
        queue_depth_t depth = 0;

        /** Requests the kernel holds for this queue pair and has not yet answered for. */
        std::uint32_t flights_count = 0;

        /** Where this queue pair's oldest unreaped local completion sits inside its window. */
        std::uint32_t locals_first = 0;

        /** Completions the disk answered itself, which no submission entry ever carried. */
        std::uint32_t locals_count = 0;

        /** The thread that took the queue pair, which every submit and poll is checked against. */
        thread_identity_t bound_thread = 0;
    };

    /** One pinned memory range, and the count of requests the kernel may still be writing into it. */
    struct registered_memory_slot_t {

        /** Zero while the slot is free, one while a range is recorded in it. */
        std::int32_t registered = 0;

        /** Requests accepted against this range and not yet answered for. */
        std::int32_t in_flight = 0;

        /** Raised every time the slot is used again, so a stale place resolves to nothing. */
        queue_pair_generation_t generation = 0;

        /** Where the pinned memory lives, which is host memory for every range this disk takes. */
        memory_space_t space = memory_space_t::host_k;

        /** First byte of the range, owned by the caller that registered it. */
        byte_t* base = nullptr;

        /** Bytes in the range. */
        std::uint64_t length = 0;
    };

    /** One open medium, its alignments, and the count of requests still naming it. */
    struct registered_medium_slot_t {

        /** Zero while the slot is free, one while a file is open in it. */
        std::int32_t registered = 0;

        /** Requests accepted against this medium and not yet answered for. */
        std::int32_t in_flight = 0;

        /** Raised every time the slot is used again, so a stale place resolves to nothing. */
        queue_pair_generation_t generation = 0;

        /** The open file, which every ring installs at this slot's own table index. */
        native_file_t file = invalid_native_file_k;

        /** Bytes the medium held at registered_memory: a regular file grows past it, a block device falls short. */
        std::uint64_t capacity = 0;

        /** Alignment this filesystem demands of the first byte of a buffer. */
        std::uint64_t memory_alignment = portable_alignment_k;

        /** Alignment this filesystem demands of an extent's offset and its length. */
        std::uint64_t offset_alignment = portable_alignment_k;
    };

    /** What one outstanding request left behind, since a completion carries only its token and result. */
    struct flight_t {

        /** The token the submission entry carried, which the kernel hands back verbatim. */
        request_token_t token;

        /** Bytes the extent asked for, which separates a whole transfer from a partial one. */
        std::uint64_t length = 0;

        /** The range the request borrowed, given back when its completion is completed. */
        memory_reference_t buffer;

        /** The medium the request borrowed, given back when its completion is completed. */
        medium_reference_t medium;
    };

    /** Entries the availability probe asks for, which is the smallest ring the kernel rounds up to. */
    static constexpr unsigned probe_entries_k = 4;

    /** Completion seats per request of depth, so a burst of completions rarely overflows the queue. */
    static constexpr unsigned completion_slack_k = 4;

    /** Completion seats the kernel allows at most, which bounds the depth a mount may ask for. */
    static constexpr queue_depth_t depth_ceiling_k = 16384;

    /** The largest extent one submission entry carries, kept well inside the kernel's own limit. */
    static constexpr std::uint64_t max_transfer_bytes_k = 1024ull * 1024ull * 1024ull;

    /** The completion a cancelling entry posts for itself, which no queue pair can ever mint. */
    static constexpr std::uint64_t cancel_marker_k = ~std::uint64_t {0};

    /** Completions one drain discards at a time. */
    static constexpr std::size_t drain_batch_k = 32;

    /** Fruitless polls a drain tolerates before it asks the kernel to stop what is left. */
    static constexpr std::size_t drain_patience_k = 1024;

    /** Reaps a drain performs before it calls the ring broken. */
    static constexpr std::size_t drain_spin_limit_k = std::size_t {1} << 24;

    /** The flags every ring of this transport is set up with, and the probe tests for. */
    static constexpr unsigned ring_setup_flags_k = IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SINGLE_ISSUER |
                                                   IORING_SETUP_NO_SQARRAY | IORING_SETUP_R_DISABLED;

    /** Grows @p elements to @p count default slots, reporting an allocation failure as a status. */
    template <typename element_type_>
    static status_t size_(st::basic_vector<element_type_>& elements, std::size_t count) noexcept {
        if (elements.resize(count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A ring could not allocate its slots"};
        return {};
    }

    /** The kernel's name for the calling thread, since a ring binds to a task and a @c pthread_t is reused. */
    [[nodiscard]] static thread_identity_t bound_thread_identity_() noexcept {
        return static_cast<std::uint64_t>(::gettid());
    }

    static fu::preferred_atomic_ref<std::int32_t> claim_(std::int32_t& word) noexcept {
        return fu::preferred_atomic_ref<epoch_t> {word};
    }

    static fu::preferred_atomic_ref<epoch_t> epoch_(epoch_t& word) noexcept {
        return fu::preferred_atomic_ref<epoch_t> {word};
    }

    static fu::preferred_atomic_ref<std::int32_t> in_flight_(registered_memory_slot_t& slot) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {slot.in_flight};
    }

    static fu::preferred_atomic_ref<std::int32_t> in_flight_(registered_medium_slot_t& slot) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {slot.in_flight};
    }

#pragma region Rings

    /** Sets one ring up disabled, maps it, and gives it the sparse buffer and file tables. */
    status_t create_ring_(ring_t& ring) noexcept {
        io_uring_params parameters {};
        parameters.flags = ring_setup_flags_k | IORING_SETUP_CQSIZE;
        std::uint64_t const wanted = static_cast<std::uint64_t>(depth_limit_) * completion_slack_k;
        parameters.cq_entries = static_cast<unsigned>(wanted < 65536u ? wanted : 65536u);
        long const opened = ::syscall(__NR_io_uring_setup, depth_limit_, &parameters);
        if (opened < 0) return native_file_failure(errno, "The kernel refused an io_uring ring");
        ring.file = static_cast<int>(opened);
        if ((parameters.features & IORING_FEAT_SINGLE_MMAP) == 0) {
            teardown_ring_(ring);
            return {status_code_t::unsupported_k, "This kernel maps the two rings apart"};
        }

        std::size_t const submission_bytes = parameters.sq_off.array + parameters.sq_entries * sizeof(unsigned);
        std::size_t const completion_bytes = parameters.cq_off.cqes + parameters.cq_entries * sizeof(io_uring_cqe);
        ring.shared_bytes = submission_bytes > completion_bytes ? submission_bytes : completion_bytes;
        ring.shared = ::mmap(nullptr, ring.shared_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring.file,
                             IORING_OFF_SQ_RING);
        if (ring.shared == MAP_FAILED) {
            ring.shared = nullptr;
            teardown_ring_(ring);
            return {status_code_t::out_of_memory_k, "The ring would not map"};
        }
        ring.submission_entries_bytes = parameters.sq_entries * sizeof(io_uring_sqe);
        void* const entries = ::mmap(nullptr, ring.submission_entries_bytes, PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_POPULATE, ring.file, IORING_OFF_SQES);
        if (entries == MAP_FAILED) {
            teardown_ring_(ring);
            return {status_code_t::out_of_memory_k, "The submission entries would not map"};
        }
        ring.submission_entries = static_cast<io_uring_sqe*>(entries);

        byte_t* const shared = static_cast<byte_t*>(ring.shared);
        ring.submission_head = reinterpret_cast<unsigned*>(shared + parameters.sq_off.head);
        ring.submission_tail = reinterpret_cast<unsigned*>(shared + parameters.sq_off.tail);
        ring.submission_flags = reinterpret_cast<unsigned*>(shared + parameters.sq_off.flags);
        ring.submission_mask = *reinterpret_cast<unsigned*>(shared + parameters.sq_off.ring_mask);
        ring.submission_entries_count = parameters.sq_entries;
        ring.completion_head = reinterpret_cast<unsigned*>(shared + parameters.cq_off.head);
        ring.completion_tail = reinterpret_cast<unsigned*>(shared + parameters.cq_off.tail);
        ring.completion_mask = *reinterpret_cast<unsigned*>(shared + parameters.cq_off.ring_mask);
        ring.completion_entries = reinterpret_cast<io_uring_cqe*>(shared + parameters.cq_off.cqes);

        io_uring_rsrc_register table {};
        table.flags = IORING_RSRC_REGISTER_SPARSE;
        table.nr = registered_memories_limit_;
        if (::syscall(__NR_io_uring_register, ring.file, IORING_REGISTER_BUFFERS2, &table, sizeof(table)) < 0) {
            status_t const refused = native_file_failure(errno, "The ring would not take a buffer table");
            teardown_ring_(ring);
            return refused;
        }
        table.nr = media_limit_;
        if (::syscall(__NR_io_uring_register, ring.file, IORING_REGISTER_FILES2, &table, sizeof(table)) < 0) {
            status_t const refused = native_file_failure(errno, "The ring would not take a file table");
            teardown_ring_(ring);
            return refused;
        }
        ring.carried_registered_memories = 0;
        ring.carried_media = 0;
        return {};
    }

    /**
     *  @brief Unmaps and closes one ring, leaving the slot ready for a fresh one.
     *
     *  Only the bound thread may give the pins back at once; a ring closed by any other thread leaves
     *  them to the kernel's exit work, which releases them later, and the next pin falls back until then.
     */
    static void teardown_ring_(ring_t& ring) noexcept {
        if (ring.file >= 0 && ring.bound_thread == bound_thread_identity_()) {
            [[maybe_unused]] long const unpinned = ::syscall(__NR_io_uring_register, ring.file,
                                                             IORING_UNREGISTER_BUFFERS, nullptr, 0);
        }
        if (ring.submission_entries) ::munmap(ring.submission_entries, ring.submission_entries_bytes);
        if (ring.shared) ::munmap(ring.shared, ring.shared_bytes);
        if (ring.file >= 0) ::close(ring.file);
        ring = ring_t {};
    }

    /**
     *  @brief Makes @p identity the one thread that may enter and register on @p ring.
     *
     *  A ring set up disabled binds to whoever enables it, so the first queue pair costs one register
     *  call. A ring already bound elsewhere cannot change hands, so a handover builds a fresh one,
     *  which is only legal because a queue pair moves with nothing outstanding on it. A ring whose last
     *  build failed is built again here rather than left dead in its slot.
     */
    status_t bind_ring_(ring_t& ring, thread_identity_t identity) noexcept {
        if (ring.bound_thread == identity) return {};
        if (ring.bound_thread != 0) teardown_ring_(ring);
        if (ring.file < 0)
            if (status_t const created = create_ring_(ring); created.failed()) return created;
        if (::syscall(__NR_io_uring_register, ring.file, IORING_REGISTER_ENABLE_RINGS, nullptr, 0) < 0)
            return native_file_failure(errno, "The ring would not bind to this thread");
        ring.bound_thread = identity;
        return {};
    }

    /** Writes the disk's registered ranges and media into @p ring's tables when it is behind. */
    status_t carry_tables_(ring_t& ring, std::size_t seat) noexcept {
        epoch_t const registered_memories = epoch_(registered_memories_epoch_).load(std::memory_order_acquire);
        if (ring.carried_registered_memories != registered_memories) {
            auto [pinned, pin_ranges_status] = pin_ranges_(ring, seat, registered_memories);
            if (pin_ranges_status.failed()) return pin_ranges_status;
            ring.pinning = pinned;
            ring.carried_registered_memories = registered_memories;
        }

        std::int32_t const media = epoch_(media_epoch_).load(std::memory_order_acquire);
        if (ring.carried_media != media) {
            int* const files = &registered_media_[seat * media_limit_];
            for (std::size_t index = 0; index != media_limit_; ++index) {
                registered_medium_slot_t& slot = media_[index];
                bool const live = claim_(slot.registered).load(std::memory_order_acquire) != 0;
                files[index] = live ? slot.file : -1;
            }
            io_uring_rsrc_update update {};
            update.data = reinterpret_cast<std::uint64_t>(files);
            if (::syscall(__NR_io_uring_register, ring.file, IORING_REGISTER_FILES_UPDATE, &update, media_limit_) < 0)
                return native_file_failure(errno, "The ring would not take its registered media");
            ring.carried_media = media;
        }
        return {};
    }

    /**
     *  @brief Pins the disk's ranges into @p ring's table, or answers that the locked-memory cap refused them.
     *
     *  The kernel charges every ring for every range, so the cap is met after a few rings; the first
     *  refusal at an epoch is remembered and later rings skip the attempt, which would pin and unpin
     *  the whole arena only to be refused again.
     */
    expected<range_pinning_t> pin_ranges_(ring_t& ring, std::size_t seat, std::int32_t registered_memories) noexcept {
        if (epoch_(pin_refused_epoch_).load(std::memory_order_acquire) == registered_memories)
            return range_pinning_t::unpinned_k;
        ::iovec* const vectors = &vectors_[seat * registered_memories_limit_];
        for (std::size_t index = 0; index != registered_memories_limit_; ++index) {
            registered_memory_slot_t& slot = registered_memories_[index];
            bool const live = claim_(slot.registered).load(std::memory_order_acquire) != 0;
            vectors[index].iov_base = live ? slot.base : nullptr;
            vectors[index].iov_len = live ? static_cast<std::size_t>(slot.length) : 0;
        }
        io_uring_rsrc_update2 update {};
        update.nr = registered_memories_limit_;
        update.data = reinterpret_cast<std::uint64_t>(vectors);
        long const took = ::syscall(__NR_io_uring_register, ring.file, IORING_REGISTER_BUFFERS_UPDATE, &update,
                                    sizeof(update));
        if (took == static_cast<long>(registered_memories_limit_)) return range_pinning_t::pinned_k;
        if (took < 0 && errno != ENOMEM)
            return native_file_failure(errno, "The ring would not take its registered ranges");
        epoch_(pin_refused_epoch_).store(registered_memories, std::memory_order_release);
        return range_pinning_t::unpinned_k;
    }

    /** Runs the completion work a deferred ring only performs on its own thread, without waiting. */
    static void enter_for_completions_(ring_t& ring) noexcept {
        [[maybe_unused]] long const entered = ::syscall(__NR_io_uring_enter, ring.file, 0u, 0u, IORING_ENTER_GETEVENTS,
                                                        nullptr, 0);
    }

    /** Whether the kernel is holding completions back because the queue had no room for them. */
    [[nodiscard]] static bool overflowed_(ring_t const& ring) noexcept {
        unsigned const flags = std::atomic_ref<unsigned> {*ring.submission_flags}.load(std::memory_order_acquire);
        return (flags & IORING_SQ_CQ_OVERFLOW) != 0;
    }

    /** Closes every ring and every open medium, which both the destructor and a move assignment need. */
    void close_all_() noexcept {
        for (std::size_t index = 0; index != rings_.size(); ++index) teardown_ring_(rings_[index]);
        for (std::size_t index = 0; index != media_.size(); ++index)
            if (media_[index].file != invalid_native_file_k) close_native_file(media_[index].file);
        rings_.clear();
        media_.clear();
    }

#pragma endregion Rings

#pragma region Bookkeeping

    /** Where one queue pair's outstanding requests and local completions sit inside the shared windows. */
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

    /** Marks the range and the medium of @p request as live, so neither can be taken back under it. */
    void borrow_(request_t const& request) noexcept {
        if (registered_memory_slot_t* const slot = resolve_(request.buffer); slot)
            in_flight_(*slot).add(1, std::memory_order_acquire);
        if (registered_medium_slot_t* const slot = resolve_(request.medium); slot)
            in_flight_(*slot).add(1, std::memory_order_acquire);
    }

    /** Releases both once the kernel is done with the bytes of the request @p flight remembers. */
    void restore_(flight_t const& flight) noexcept {
        if (registered_memory_slot_t* const slot = resolve_(flight.buffer); slot)
            in_flight_(*slot).sub(1, std::memory_order_release);
        if (registered_medium_slot_t* const slot = resolve_(flight.medium); slot)
            in_flight_(*slot).sub(1, std::memory_order_release);
    }

    /** Remembers what @p request will need when only its token and result come back. */
    void track_(queue_pair_slot_t& slot, std::uint32_t base, request_t const& request) noexcept {
        flight_t& flight = flights_[base + slot.flights_count];
        flight.token = request.token;
        flight.length = request.kind == request_kind_t::flush_k ? 0 : request.extent.length;
        flight.buffer = request.buffer;
        flight.medium = request.medium;
        ++slot.flights_count;
    }

    /** Takes @p token out of @p slot's outstanding set, moving the last entry into the gap. */
    expected<flight_t> retire_(queue_pair_slot_t& slot, std::uint32_t base, request_token_t token) noexcept {
        for (std::uint32_t index = 0; index != slot.flights_count; ++index) {
            flight_t& flight = flights_[base + index];
            if (!(flight.token == token)) continue;
            flight_t const taken = flight;
            flight = flights_[base + slot.flights_count - 1];
            --slot.flights_count;
            return taken;
        }
        return status_t {status_code_t::not_found_k, "A completion carries a token nothing is outstanding for"};
    }

    /** Places @p completion at the back of @p slot's own queue, which poll drains after the kernel's. */
    void push_local_(queue_pair_slot_t& slot, std::uint32_t base, completion_t const& completion) noexcept {
        locals_[base + (slot.locals_first + slot.locals_count) % slot.depth] = completion;
        ++slot.locals_count;
    }

    /** Moves what the disk answered itself into @p into, oldest first. */
    std::size_t drain_locals_(queue_pair_slot_t& slot, std::uint32_t base, std::span<completion_t> into) noexcept {
        std::size_t const moving = into.size() < slot.locals_count ? into.size() : slot.locals_count;
        for (std::size_t index = 0; index != moving; ++index)
            into[index] = locals_[base + (slot.locals_first + index) % slot.depth];
        slot.locals_first = static_cast<std::uint32_t>((slot.locals_first + moving) % slot.depth);
        slot.locals_count -= static_cast<std::uint32_t>(moving);
        return moving;
    }

#pragma endregion Bookkeeping

#pragma region Requests

    /** The completion a request gets when a chain ahead of it was cut before it reached the kernel. */
    [[nodiscard]] static completion_t cancelled_completion_(request_token_t token) noexcept {
        completion_t completion;
        completion.token = token;
        completion.state = completion_state_t::cancelled_k;
        completion.status = {status_code_t::uninitialized_k, "The queue_pair cancelled this request"};
        return completion;
    }

    /** The completion a request gets when the disk saw a defect the kernel would never see. */
    [[nodiscard]] static completion_t failed_completion_(request_token_t token, status_t reason) noexcept {
        completion_t completion;
        completion.token = token;
        completion.state = completion_state_t::failed_k;
        completion.status = reason;
        return completion;
    }

    /** Whether the request that follows @p request in its batch waits for this one to transfer in full. */
    [[nodiscard]] static bool chains_next_(request_t const& request) noexcept {
        return request.chaining == request_chaining_t::chained_k;
    }

    /** Whatever would corrupt a submission entry rather than merely fail on the device. */
    status_t inspect_(request_t const& request) noexcept {
        registered_medium_slot_t const* const medium = resolve_(request.medium);
        if (!medium) return {status_code_t::invalid_argument_k, "The request names no registered medium"};
        if (request.kind == request_kind_t::flush_k) return {};
        registered_memory_slot_t const* const memory = resolve_(request.buffer);
        if (!memory) return {status_code_t::invalid_argument_k, "The request names no registered range"};
        if (request.buffer.offset > memory->length || request.extent.length > memory->length - request.buffer.offset)
            return {status_code_t::invalid_argument_k, "The request runs past its registered range"};
        if (request.extent.length > max_transfer_bytes_k)
            return {status_code_t::invalid_argument_k, "The request is larger than one transfer"};
        byte_t const* const address = memory->base + request.buffer.offset;
        if (!is_address_aligned(address, medium->memory_alignment) ||
            !request.extent.aligned_to(medium->offset_alignment))
            return {status_code_t::invalid_argument_k, "The request does not meet the disk's alignment"};
        return {};
    }

    /** Writes @p request into @p entry, addressing the medium by table index and the buffer as @p pinning allows. */
    void build_(io_uring_sqe& entry, request_t const& request, range_pinning_t pinning) noexcept {
        std::memset(&entry, 0, sizeof(entry));
        entry.flags = IOSQE_FIXED_FILE;
        if (request.chaining == request_chaining_t::chained_k) entry.flags |= IOSQE_IO_LINK;
        entry.fd = static_cast<std::int32_t>(request.medium.medium_id);
        entry.user_data = request.token.bits;
        if (request.kind == request_kind_t::flush_k) {
            entry.opcode = IORING_OP_FSYNC;
            entry.fsync_flags = IORING_FSYNC_DATASYNC;
            return;
        }
        registered_memory_slot_t const* const memory = resolve_(request.buffer);
        entry.off = request.extent.offset;
        entry.addr = reinterpret_cast<std::uint64_t>(memory->base + request.buffer.offset);
        entry.len = static_cast<std::uint32_t>(request.extent.length);
        switch (pinning) {
        case range_pinning_t::pinned_k:
            entry.opcode = request.kind == request_kind_t::read_k ? IORING_OP_READ_FIXED : IORING_OP_WRITE_FIXED;
            entry.buf_index = static_cast<std::uint16_t>(request.buffer.memory_id);
            return;
        case range_pinning_t::unpinned_k:
            entry.opcode = request.kind == request_kind_t::read_k ? IORING_OP_READ : IORING_OP_WRITE;
            return;
        }
    }

    /** Drops the link from @p linked, since the request that would have followed it never arrived. */
    static void unlink_(io_uring_sqe*& linked) noexcept {
        if (!linked) return;
        linked->flags = static_cast<std::uint8_t>(linked->flags & ~IOSQE_IO_LINK);
        linked = nullptr;
    }

    /** Takes @p count entries the kernel would not accept back out of the queue and fails them. */
    void refuse_tail_(queue_pair_slot_t& slot, std::uint32_t base, ring_t& ring, unsigned tail,
                      std::size_t count) noexcept {
        std::atomic_ref<unsigned> {*ring.submission_tail}.store(tail, std::memory_order_release);
        status_t const refused {status_code_t::input_output_k, "The kernel would not accept this request"};
        for (std::size_t index = 0; index != count; ++index) {
            flight_t const& flight = flights_[base + slot.flights_count - 1];
            completion_t const completion = failed_completion_(flight.token, refused);
            restore_(flight);
            --slot.flights_count;
            push_local_(slot, base, completion);
        }
    }

    /** Turns one completion entry into the completion its queue pair reads, by what the kernel returned. */
    [[nodiscard]] static completion_t translate_(flight_t const& flight, int reason) noexcept {
        completion_t completion;
        completion.token = flight.token;
        if (reason >= 0) {
            completion.transferred_bytes = static_cast<std::uint64_t>(reason);
            completion.state = completion.transferred_bytes == flight.length ? completion_state_t::completed_k
                                                                             : completion_state_t::partial_k;
            return completion;
        }
        if (reason == -ECANCELED) {
            completion.state = completion_state_t::cancelled_k;
            completion.status = {status_code_t::uninitialized_k, "The queue_pair cancelled this request"};
            return completion;
        }
        completion.state = completion_state_t::failed_k;
        completion.status = native_file_failure(-reason, "The device refused this request");
        return completion;
    }

    /** Copies what the completion queue holds into @p into, dropping the entries nobody asked for. */
    std::size_t poll_completions_(queue_pair_slot_t& slot, std::uint32_t base, ring_t& ring,
                                  std::span<completion_t> into) noexcept {
        unsigned const taken = *ring.completion_head;
        unsigned head = taken;
        unsigned const posted = std::atomic_ref<unsigned> {*ring.completion_tail}.load(std::memory_order_acquire);
        std::size_t moved = 0;
        while (head != posted && moved != into.size()) {
            io_uring_cqe const& entry = ring.completion_entries[head & ring.completion_mask];
            ++head;
            expected<flight_t> const flight = retire_(slot, base, request_token_t {entry.user_data});
            if (!flight) continue;
            into[moved] = translate_(*flight, entry.res);
            restore_(*flight);
            ++moved;
        }
        if (head != taken) std::atomic_ref<unsigned> {*ring.completion_head}.store(head, std::memory_order_release);
        return moved;
    }

#pragma endregion Requests

    /** One slot per queue pair the disk hands out. */
    st::basic_vector<queue_pair_slot_t> queue_pairs_;

    /** One ring per queue pair, since a deferred single-bound thread ring belongs to exactly one thread. */
    st::basic_vector<ring_t> rings_;

    /** One slot per memory range the disk records. */
    st::basic_vector<registered_memory_slot_t> registered_memories_;

    /** One slot per medium the disk opens. */
    st::basic_vector<registered_medium_slot_t> media_;

    /** Every queue pair's outstanding requests, in windows of @c depth_limit_. */
    st::basic_vector<flight_t> flights_;

    /** Every queue pair's own completions, in windows of @c depth_limit_ read as a circle. */
    st::basic_vector<completion_t> locals_;

    /** One buffer table per ring, written by that ring's own thread. */
    st::basic_vector<::iovec> vectors_;

    /** One file table per ring, written by that ring's own thread. */
    st::basic_vector<int> registered_media_;

    /** Raised whenever a range is recorded or given back, so a ring knows its table is behind. */
    epoch_t registered_memories_epoch_ = 1;

    /** Raised whenever a medium is opened or closed, so a ring knows its table is behind. */
    epoch_t media_epoch_ = 1;

    /** The registered memory epoch at which the locked-memory cap refused a pin, or zero while none was refused. */
    epoch_t pin_refused_epoch_ = 0;

    /** Requests one queue pair may keep in flight, and the stride of every queue pair's window. */
    queue_depth_t depth_limit_ = 0;

    /** Leases the disk hands out at once, which is also the number of rings it holds. */
    std::uint16_t queue_pairs_limit_ = 0;

    /** Memory ranges the disk records at once, and the width of every ring's buffer table. */
    std::uint16_t registered_memories_limit_ = 0;

    /** Targets the disk opens at once, and the width of every ring's file table. */
    std::uint16_t media_limit_ = 0;

    /** Whether the page cache stands in the path, which fixes what a medium with no alignment reports. */
    file_buffering_t buffering_ = file_buffering_t::unbuffered_k;
};

#pragma endregion Disk

#else

#pragma region Absent

/**
 *  @brief The same disk on a platform that has no io_uring, which mounts nowhere and refuses cleanly.
 *
 *  The type exists so a caller compiles against it unconditionally and chooses at mount, where a
 *  refused @c make sends it to the blocking floor rather than to a build-time branch.
 */
class linux_uring_files_disk_t {
  public:

    /** Answers that this platform has no ring to offer. */
    [[nodiscard]] static linux_uring_probe_t probe() noexcept { return linux_uring_probe_t::refused_k; }

    /** Refuses the mount, since this platform has no ring to mount on. */
    static expected<linux_uring_files_disk_t> make(linux_uring_disk_config_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** What this disk would accept, which is nothing at all. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept { return {}; }

    /** Answers that nothing is pinned, since nothing was ever registered. */
    [[nodiscard]] range_pinning_t pinning() const noexcept { return range_pinning_t::unpinned_k; }

    /** Refuses the queue pair, since there is no ring to queue pair a share of. */
    expected<queue_pair_t> acquire_queue_pair(std::uint32_t) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Refuses the handover, since no queue pair was ever handed out. */
    status_t bind_queue_pair(queue_pair_t const&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Refuses the return, since no queue pair was ever handed out. */
    status_t release_queue_pair(queue_pair_t&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Refuses the range, since no ring would ever address it. */
    expected<registered_memory_t> register_memory(std::span<byte_t>, memory_space_t) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Refuses the range, since none was ever recorded. */
    status_t unregister_memory(registered_memory_t&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Refuses the medium, since no ring would ever address it. */
    expected<registered_medium_t> register_medium(medium_options_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Refuses the medium, since none was ever opened. */
    status_t unregister_medium(registered_medium_t&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Takes nothing, since there is no queue to take it into. */
    expected<std::size_t> submit(queue_pair_t const&, std::span<request_t const>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Answers nothing, since nothing was ever submitted. */
    expected<std::size_t> poll_completions(queue_pair_t const&, std::span<completion_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Stops nothing, since nothing was ever submitted. */
    void cancel(queue_pair_t const&) noexcept {}

    /** Refuses the drain, since no queue pair was ever handed out. */
    status_t drain(queue_pair_t const&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Answers that nothing is outstanding, which is always true here. */
    [[nodiscard]] std::size_t outstanding(queue_pair_t const&) const noexcept { return 0; }
};

#pragma endregion Absent

#endif

static_assert(ring_concept<linux_uring_files_disk_t>, "The io_uring transport is a ring like every other");

} // namespace unum::ustore::nvme
