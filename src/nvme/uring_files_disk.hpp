/**
 *  @file src/nvme/uring_files_disk.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The ring Linux offers: @c O_DIRECT files driven through raw @c io_uring syscalls.
 *
 *  Nothing here links liburing. A ring is set up with @c io_uring_setup, mapped with @c mmap, fed
 *  with @c io_uring_register and entered with @c io_uring_enter, which is the whole interface and
 *  needs Linux 6.12 or newer for @c DEFER_TASKRUN, @c SINGLE_ISSUER and @c NO_SQARRAY together.
 *
 *  A ring belongs to one lease rather than to the disk, because @c SINGLE_ISSUER binds a ring to one
 *  task and answers @c -EEXIST to every other, for @c io_uring_enter and @c io_uring_register alike.
 *  The binding happens at @c io_uring_setup unless the ring is set up disabled, so every ring is
 *  created with @c IORING_SETUP_R_DISABLED at mount - where mapping and the sparse tables can still
 *  fail loudly - and bound to its thread by @c IORING_REGISTER_ENABLE_RINGS when the lease is taken.
 *
 *  Because only the bound thread may register on its own ring, a registered buffer or target is
 *  recorded on the disk and copied into a ring's tables by that ring's own thread, at the submission
 *  that first needs it. The disk counts registration and target epochs; a ring rewrites its table
 *  when its epoch is behind. Pinning the ranges is an optimisation the locked-memory cap may refuse,
 *  since every ring is charged for the whole slab; a ring refused the pin addresses them by pointer.
 */

#pragma once

/** Whether this translation unit can reach the ring at all, which only Linux offers. */
#if defined(__linux__) && __has_include(<linux/io_uring.h>)
#define USTORE_NVME_HAS_URING 1
#else
#define USTORE_NVME_HAS_URING 0
#endif

#include <cassert> // `assert`
#include <cerrno>  // `errno`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memset`

#if USTORE_NVME_HAS_URING
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
#include <thread>  // `std::this_thread::yield`
#include <utility> // `std::move`

#include <forkunion/atomics.hpp>       // `fu::preferred_atomic_ref`
#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/blocking_files_disk.hpp" // `native_file_t`, `open_native_file`, `native_file_capacity`
#include "nvme/extent.hpp"              // `extent_t`, `portable_alignment_k`
#include "nvme/ring.hpp"                // `request_t`, `completion_t`, `ring_owner_t`

namespace unum::ustore::nvme {

#pragma region Configuration

/**
 *  @brief What a mount fixes about an io_uring disk before any owner leases a share of it.
 *
 *  Depth is the one number worth tuning: sixteen outstanding reads already beat @c pread elevenfold
 *  on one namespace, sixty-four reach sixteenfold, and everything past a hundred and twenty-eight is
 *  flat, so the default sits where the curve does.
 */
struct uring_disk_config_t {

    /** Leases the disk hands out at once, which is also the number of rings it creates. */
    std::uint16_t owners_limit = 1;

    /** Requests one lease may keep in flight, which is the credit an owner spends and reaps back. */
    std::uint32_t depth_limit = 64;

    /** Memory ranges the disk pins at once, which every ring keeps a table slot for. */
    std::uint16_t registrations_limit = 8;

    /** Targets the disk registers at once, which a directory of sorted runs raises above one. */
    std::uint16_t targets_limit = 1;

    /** Whether the page cache stands between the ring and the medium. */
    file_buffering_t buffering = file_buffering_t::unbuffered_k;
};

/** Whether this machine offers the ring, which a mount asks once before it chooses a transport. */
enum class uring_probe_t : std::uint8_t {

    /** The kernel set a ring up with every flag this transport needs. */
    available_k,

    /** The kernel has io_uring and refused it, which a seccomp profile or @c io_uring_disabled does. */
    refused_k,

    /** The kernel has no io_uring, or none that carries the flags this transport needs. */
    too_old_k,
};

/** Whether the kernel holds the registered ranges pinned, which the locked-memory cap decides at first submission. */
enum class range_pinning_t : std::uint8_t {

    /** Pinned once at registration, so a request names a table slot and the kernel pins nothing per request. */
    pinned_k,

    /** The pin exceeded the locked-memory cap, so a request carries its pointer and the kernel pins per request. */
    unpinned_k,
};

#pragma endregion Configuration

#if USTORE_NVME_HAS_URING

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
 *  @brief The ring Linux offers, over one or more @c O_DIRECT files registered as targets.
 *
 *  This is not a @c basic_disk over a blocking device: the queue is the kernel's, so the disk drives
 *  it directly and only the bookkeeping is its own. @c submit writes submission entries and enters
 *  once, @c reap enters with @c min_complete of zero to run the deferred completion work and copies
 *  whatever landed, and neither ever waits.
 *
 *  A defect the ring can see before the kernel does - a stale registration, an extent past its
 *  range, a length past one transfer, an alignment the filesystem refuses - never fails the batch.
 *  It becomes that one request's completion and leaves its neighbours alone, which is what a chain
 *  of submission entries would do anyway. Only a token that is not this lease's fails the whole
 *  @c submit, since a misrouted completion is unrecoverable.
 */
class uring_files_disk_t {
  public:

    uring_files_disk_t() noexcept = default;
    uring_files_disk_t(uring_files_disk_t const&) = delete;
    uring_files_disk_t& operator=(uring_files_disk_t const&) = delete;
    uring_files_disk_t(uring_files_disk_t&&) noexcept = default;

    uring_files_disk_t& operator=(uring_files_disk_t&& other) noexcept {
        if (this == &other) return *this;
        close_all_();
        owners_ = std::move(other.owners_);
        rings_ = std::move(other.rings_);
        registrations_ = std::move(other.registrations_);
        targets_ = std::move(other.targets_);
        flights_ = std::move(other.flights_);
        locals_ = std::move(other.locals_);
        vectors_ = std::move(other.vectors_);
        descriptors_ = std::move(other.descriptors_);
        registrations_epoch_ = other.registrations_epoch_;
        targets_epoch_ = other.targets_epoch_;
        pin_refused_epoch_ = other.pin_refused_epoch_;
        depth_limit_ = other.depth_limit_;
        owners_limit_ = other.owners_limit_;
        registrations_limit_ = other.registrations_limit_;
        targets_limit_ = other.targets_limit_;
        buffering_ = other.buffering_;
        return *this;
    }

    ~uring_files_disk_t() noexcept { close_all_(); }

    /**
     *  @brief Sets one tiny ring up and takes it down again, which is the only honest availability test.
     *  @return Whether the kernel offers the flags this transport needs, or why it does not.
     */
    [[nodiscard]] static uring_probe_t probe() noexcept {
        io_uring_params parameters {};
        parameters.flags = ring_setup_flags_k;
        long const opened = ::syscall(__NR_io_uring_setup, probe_entries_k, &parameters);
        if (opened >= 0) {
            ::close(static_cast<int>(opened));
            return uring_probe_t::available_k;
        }
        switch (errno) {
        case ENOSYS:
        case EINVAL: return uring_probe_t::too_old_k;
        default: return uring_probe_t::refused_k;
        }
    }

    /**
     *  @brief Creates one disabled ring per lease and gives each the sparse tables it will fill.
     *  @param[in] config Leases, per-lease depth, the ranges and targets pinned, and the buffering.
     *
     *  @note No file is opened here; a mount registers each one as a target, so one disk serves a
     *      directory of many sorted runs rather than one disk per run.
     */
    static expected<uring_files_disk_t> make(uring_disk_config_t const& config) noexcept {
        if (config.owners_limit == 0 || config.depth_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A ring needs a lease and a request of depth"};
        if (config.owners_limit == static_cast<std::uint16_t>(owner_id_t::unowned_k) ||
            config.registrations_limit == static_cast<std::uint16_t>(registration_id_t::unregistered_k) ||
            config.targets_limit == static_cast<std::uint16_t>(descriptor_id_t::unregistered_k))
            return status_t {status_code_t::invalid_argument_k, "A ring keeps one identifier for an empty handle"};
        if (config.targets_limit == 0 || config.registrations_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A ring needs a target and a range to move between"};
        if (config.depth_limit > depth_ceiling_k)
            return status_t {status_code_t::invalid_argument_k, "A ring holds fewer requests than that"};

        std::size_t const window = static_cast<std::size_t>(config.owners_limit) * config.depth_limit;
        uring_files_disk_t disk;
        disk.depth_limit_ = config.depth_limit;
        disk.owners_limit_ = config.owners_limit;
        disk.registrations_limit_ = config.registrations_limit;
        disk.targets_limit_ = config.targets_limit;
        disk.buffering_ = config.buffering;
        if (status_t const sized = size_(disk.owners_, config.owners_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.rings_, config.owners_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.registrations_, config.registrations_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.targets_, config.targets_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.flights_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.locals_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.vectors_,
                                         static_cast<std::size_t>(config.owners_limit) * config.registrations_limit);
            sized.failed())
            return sized;
        if (status_t const sized = size_(disk.descriptors_,
                                         static_cast<std::size_t>(config.owners_limit) * config.targets_limit);
            sized.failed())
            return sized;
        for (std::size_t index = 0; index != disk.rings_.size(); ++index)
            if (status_t const created = disk.create_ring_(disk.rings_[index]); created.failed()) return created;
        return disk;
    }

    /** What this disk accepts, which the tightest alignment across its registered targets fixes. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept {
        std::uint64_t memory = 0;
        std::uint64_t offset = 0;
        for (std::size_t index = 0; index != targets_.size(); ++index) {
            target_slot_t const& slot = targets_[index];
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
        reported.owners_limit = owners_limit_;
        reported.targets_limit = targets_limit_;
        reported.reaches_device_memory = false;
        return reported;
    }

    /** How the rings address the registered ranges, settled by the first ring the cap refused at this epoch. */
    [[nodiscard]] range_pinning_t pinning() const noexcept {
        return pin_refused_epoch_ == registrations_epoch_ ? range_pinning_t::unpinned_k : range_pinning_t::pinned_k;
    }

#pragma region Leases

    /**
     *  @brief Takes a lease on @p depth requests of one ring, and binds that ring to this thread.
     *  @param[in] depth Requests this owner may keep in flight, at most the mount's limit.
     *  @return The lease, or @c capacity_exhausted_k when every ring is already out.
     */
    expected<ring_owner_t> open(std::uint32_t depth) noexcept {
        if (depth == 0 || depth > depth_limit_)
            return status_t {status_code_t::invalid_argument_k, "A lease asks for more depth than the ring has"};
        std::uint64_t const identity = bound_thread_identity_();
        for (std::size_t index = 0; index != owners_.size(); ++index) {
            owner_slot_t& slot = owners_[index];
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
            slot.issuer = identity;
            return ring_owner_t::leased(static_cast<owner_id_t>(index), slot.generation, depth);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every lease of this ring is already held"};
    }

    /**
     *  @brief Binds @p owner and its ring to the calling thread, which is how a moved lease changes hands.
     *  @param[in] owner The lease this thread has just taken over, with nothing outstanding on it.
     *
     *  @note A ring cannot change its issuer, so a handover to another thread builds a fresh ring in
     *      the lease's slot; that is why the lease must be empty before it moves.
     */
    status_t adopt(ring_owner_t const& owner) noexcept {
        owner_slot_t* const slot = slot_of_(owner);
        if (!slot) return {status_code_t::invalid_argument_k, "The lease is not this ring's"};
        if (slot->flights_count != 0 || slot->locals_count != 0)
            return {status_code_t::invalid_argument_k, "Drain the lease before handing it over"};
        std::uint64_t const identity = bound_thread_identity_();
        if (status_t const bound = bind_ring_(rings_[static_cast<std::size_t>(owner.id())], identity); bound.failed())
            return bound;
        slot->issuer = identity;
        return {};
    }

    /**
     *  @brief Takes @p owner's lease back once it has nothing outstanding, and empties the handle.
     *  @param[inout] owner The lease to return, surrendered on success and untouched on failure.
     */
    status_t release(ring_owner_t& owner) noexcept {
        owner_slot_t* const slot = slot_of_(owner);
        if (!slot) return {status_code_t::invalid_argument_k, "The lease is not this ring's"};
        if (slot->flights_count != 0 || slot->locals_count != 0)
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
        return slot ? slot->flights_count + slot->locals_count : 0;
    }

#pragma endregion Leases

#pragma region Registrations

    /**
     *  @brief Records @p chunk so requests can address it by slot, and raises the registration epoch.
     *  @param[in] chunk Memory the caller owns and keeps alive until it deregisters the range.
     *  @param[in] space Where that memory lives; this transport reaches host memory alone.
     *
     *  @note The kernel learns of the range on each ring's own thread, at the next submission there,
     *      since @c io_uring_register answers @c -EEXIST to anyone but the ring's issuer.
     */
    expected<registration_t> register_chunk(std::span<byte_t> chunk, memory_space_t space) noexcept {
        if (chunk.empty()) return status_t {status_code_t::invalid_argument_k, "An empty range registers nothing"};
        if (space == memory_space_t::device_k)
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
            epoch_(registrations_epoch_).add(1, std::memory_order_release);
            return registration_t::registered(static_cast<registration_id_t>(index), slot.generation, chunk, space);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every registration slot of this ring is taken"};
    }

    /**
     *  @brief Gives @p registration back, refusing while the kernel may still be writing into it.
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
        epoch_(registrations_epoch_).add(1, std::memory_order_release);
        registration.surrender();
        return {};
    }

    /**
     *  @brief Opens the file @p request names and records it, reading the alignments it demands.
     *  @param[in] request The path, the bytes to create it with, and what the caller may do to it.
     *  @return The handle, or @c capacity_exhausted_k when every target slot is taken.
     */
    expected<descriptor_t> register_target(target_request_t const& request) noexcept {
        if (!request.name) return status_t {status_code_t::invalid_argument_k, "A file target needs a path"};
        for (std::size_t index = 0; index != targets_.size(); ++index) {
            target_slot_t& slot = targets_[index];
            std::int32_t expected_free = 0;
            if (!claim_(slot.registered)
                     .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            expected<native_file_t> const file = open_native_file(request.name, request.access, buffering_);
            if (!file) {
                claim_(slot.registered).store(0, std::memory_order_release);
                return file.status();
            }
            if (request.access == target_access_t::create_read_write_k && request.length != 0)
                if (status_t const sized = truncate_native_file(*file, request.length); sized.failed()) {
                    close_native_file(*file);
                    claim_(slot.registered).store(0, std::memory_order_release);
                    return sized;
                }
            direct_alignment_t const alignment = buffering_ == file_buffering_t::unbuffered_k
                                                     ? direct_alignment_of(*file)
                                                     : direct_alignment_t {1, 1};
            slot.file = *file;
            slot.capacity = native_file_capacity(*file);
            slot.memory_alignment = alignment.memory;
            slot.offset_alignment = alignment.offset;
            epoch_(targets_epoch_).add(1, std::memory_order_release);
            return descriptor_t::registered(static_cast<descriptor_id_t>(index), slot.generation, slot.capacity);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every target slot of this ring is taken"};
    }

    /**
     *  @brief Closes @p descriptor, refusing while requests against it are still in flight.
     *  @param[inout] descriptor The target to give back, surrendered on success.
     */
    status_t deregister_target(descriptor_t& descriptor) noexcept {
        target_slot_t* const slot = slot_of_(descriptor);
        if (!slot) return {status_code_t::invalid_argument_k, "The target is not this ring's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "The target is still named by requests in flight"};
        close_native_file(slot->file);
        ++slot->generation;
        slot->file = invalid_native_file_k;
        slot->capacity = 0;
        claim_(slot->registered).store(0, std::memory_order_release);
        epoch_(targets_epoch_).add(1, std::memory_order_release);
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
     */
    expected<std::size_t> submit(ring_owner_t const& owner, std::span<request_t const> requests) noexcept {
        owner_slot_t* const slot = slot_of_(owner);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The lease is not this ring's"};
        assert(slot->issuer == bound_thread_identity_() && "Only the thread holding a lease submits on it");
        for (request_t const& request : requests)
            if (request.token.owner() != owner.id() || request.token.generation() != owner.generation())
                return status_t {status_code_t::invalid_argument_k, "A request carries another lease's token"};

        std::size_t const seat = static_cast<std::size_t>(owner.id());
        ring_t& ring = rings_[seat];
        if (status_t const carried = carry_tables_(ring, seat); carried.failed()) return carried;

        std::uint32_t const base = window_of_(owner.id());
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
     *  @param[in] owner The lease reaping, held by the calling thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero when nothing has landed.
     *
     *  @note The entry carries @c min_complete of zero, so the call performs the work a deferred ring
     *      only performs on its owning thread and returns without waiting for anything.
     */
    expected<std::size_t> reap(ring_owner_t const& owner, std::span<completion_t> into) noexcept {
        owner_slot_t* const slot = slot_of_(owner);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The lease is not this ring's"};
        assert(slot->issuer == bound_thread_identity_() && "Only the thread holding a lease reaps on it");
        std::uint32_t const base = window_of_(owner.id());
        ring_t& ring = rings_[static_cast<std::size_t>(owner.id())];

        std::size_t moved = 0;
        if (slot->flights_count != 0) enter_for_completions_(ring);
        moved += harvest_(*slot, base, ring, into);
        if (overflowed_(ring) && moved != into.size()) {
            enter_for_completions_(ring);
            moved += harvest_(*slot, base, ring, into.subspan(moved));
        }
        moved += drain_locals_(*slot, base, into.subspan(moved));
        return moved;
    }

    /**
     *  @brief Asks the kernel to stop everything @p owner has in flight, which reap reports cancelled.
     *  @param[in] owner The lease to stop, whose started requests still run to their own end.
     *
     *  @note The cancelling entry posts a completion of its own, which reap recognises and drops, so
     *      the owner still sees exactly one completion per request it submitted.
     */
    void cancel(ring_owner_t const& owner) noexcept {
        owner_slot_t* const slot = slot_of_(owner);
        if (!slot || slot->flights_count == 0) return;
        ring_t& ring = rings_[static_cast<std::size_t>(owner.id())];
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
     *  @brief Finishes and discards everything @p owner has outstanding, so its buffers may be freed.
     *  @param[in] owner The lease to empty, which may be cancelled or still running.
     */
    status_t drain(ring_owner_t const& owner) noexcept {
        owner_slot_t const* const slot = slot_of_(owner);
        if (!slot) return {status_code_t::invalid_argument_k, "The lease is not this ring's"};
        std::array<completion_t, drain_batch_k> discarded {};
        for (std::size_t spins = 0; spins != drain_spin_limit_k; ++spins) {
            if (slot->flights_count == 0 && slot->locals_count == 0) return {};
            expected<std::size_t> const moved = reap(owner, std::span<completion_t> {discarded});
            if (!moved) return moved.status();
            if (*moved != 0) continue;
            if (spins == drain_patience_k) cancel(owner);
            std::this_thread::yield();
        }
        return {status_code_t::io_k, "The lease would not drain"};
    }

#pragma endregion Ring

  private:

    /** One io_uring instance, entered and registered on by exactly one thread for its whole life. */
    struct ring_t {

        /** The ring's descriptor, @c -1 before it is created and after it is taken down. */
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
        std::uint64_t issuer = 0;

        /** The registration epoch this ring's buffer table was last written from. */
        std::int32_t carried_registrations = 0;

        /** The target epoch this ring's file table was last written from. */
        std::int32_t carried_targets = 0;

        /** How this ring addresses the registered ranges, settled when its buffer table was last written. */
        range_pinning_t pinning = range_pinning_t::pinned_k;
    };

    /** One lease's share of the disk: its ring, its credit and the two queues it reaps from. */
    struct owner_slot_t {

        /** Zero while the slot is free, one while a lease holds it. */
        std::int32_t leased = 0;

        /** Raised every time the slot is leased again, so a stale token routes nowhere. */
        std::uint16_t generation = 0;

        /** Requests this lease may keep in flight, fixed when the lease was taken. */
        std::uint32_t depth = 0;

        /** Requests the kernel holds for this lease and has not yet answered for. */
        std::uint32_t flights_count = 0;

        /** Where this owner's oldest unreaped local completion sits inside its window. */
        std::uint32_t locals_first = 0;

        /** Completions the disk answered itself, which no submission entry ever carried. */
        std::uint32_t locals_count = 0;

        /** The thread that took the lease, which every submit and reap is checked against. */
        std::uint64_t issuer = 0;
    };

    /** One pinned memory range, and the count of requests the kernel may still be writing into it. */
    struct registration_slot_t {

        /** Zero while the slot is free, one while a range is recorded in it. */
        std::int32_t registered = 0;

        /** Requests accepted against this range and not yet answered for. */
        std::int32_t in_flight = 0;

        /** Raised every time the slot is used again, so a stale place resolves to nothing. */
        std::uint16_t generation = 0;

        /** Where the pinned memory lives, which is host memory for every range this disk takes. */
        memory_space_t space = memory_space_t::host_k;

        /** First byte of the range, owned by the caller that registered it. */
        byte_t* base = nullptr;

        /** Bytes in the range. */
        std::uint64_t length = 0;
    };

    /** One open target, its alignments, and the count of requests still naming it. */
    struct target_slot_t {

        /** Zero while the slot is free, one while a file is open in it. */
        std::int32_t registered = 0;

        /** Requests accepted against this target and not yet answered for. */
        std::int32_t in_flight = 0;

        /** Raised every time the slot is used again, so a stale place resolves to nothing. */
        std::uint16_t generation = 0;

        /** The open file, which every ring installs at this slot's own table index. */
        native_file_t file = invalid_native_file_k;

        /** Bytes the target held at registration: a regular file grows past it, a block device falls short. */
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

        /** The range the request borrowed, given back when its completion is reaped. */
        buffer_place_t buffer;

        /** The target the request borrowed, given back when its completion is reaped. */
        target_place_t target;
    };

    /** Entries the availability probe asks for, which is the smallest ring the kernel rounds up to. */
    static constexpr unsigned probe_entries_k = 4;

    /** Completion seats per request of depth, so a burst of completions rarely overflows the queue. */
    static constexpr unsigned completion_slack_k = 4;

    /** Completion seats the kernel allows at most, which bounds the depth a mount may ask for. */
    static constexpr std::uint32_t depth_ceiling_k = 16384;

    /** The largest extent one submission entry carries, kept well inside the kernel's own limit. */
    static constexpr std::uint64_t max_transfer_bytes_k = 1024ull * 1024ull * 1024ull;

    /** The completion a cancelling entry posts for itself, which no lease can ever mint. */
    static constexpr std::uint64_t cancel_marker_k = ~std::uint64_t {0};

    /** Completions one drain discards at a time. */
    static constexpr std::size_t drain_batch_k = 32;

    /** Fruitless reaps a drain tolerates before it asks the kernel to stop what is left. */
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
    [[nodiscard]] static std::uint64_t bound_thread_identity_() noexcept {
        return static_cast<std::uint64_t>(::gettid());
    }

    static fu::preferred_atomic_ref<std::int32_t> claim_(std::int32_t& word) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {word};
    }

    static fu::preferred_atomic_ref<std::int32_t> epoch_(std::int32_t& word) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {word};
    }

    static fu::preferred_atomic_ref<std::int32_t> in_flight_(registration_slot_t& slot) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {slot.in_flight};
    }

    static fu::preferred_atomic_ref<std::int32_t> in_flight_(target_slot_t& slot) noexcept {
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
        table.nr = registrations_limit_;
        if (::syscall(__NR_io_uring_register, ring.file, IORING_REGISTER_BUFFERS2, &table, sizeof(table)) < 0) {
            status_t const refused = native_file_failure(errno, "The ring would not take a buffer table");
            teardown_ring_(ring);
            return refused;
        }
        table.nr = targets_limit_;
        if (::syscall(__NR_io_uring_register, ring.file, IORING_REGISTER_FILES2, &table, sizeof(table)) < 0) {
            status_t const refused = native_file_failure(errno, "The ring would not take a file table");
            teardown_ring_(ring);
            return refused;
        }
        ring.carried_registrations = 0;
        ring.carried_targets = 0;
        return {};
    }

    /**
     *  @brief Unmaps and closes one ring, leaving the slot ready for a fresh one.
     *
     *  Only the bound thread may give the pins back at once; a ring closed by any other thread leaves
     *  them to the kernel's exit work, which releases them later, and the next pin falls back until then.
     */
    static void teardown_ring_(ring_t& ring) noexcept {
        if (ring.file >= 0 && ring.issuer == bound_thread_identity_()) {
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
     *  A ring set up disabled binds to whoever enables it, so the first lease costs one register
     *  call. A ring already bound elsewhere cannot change hands, so a handover builds a fresh one,
     *  which is only legal because a lease moves with nothing outstanding on it. A ring whose last
     *  build failed is built again here rather than left dead in its slot.
     */
    status_t bind_ring_(ring_t& ring, std::uint64_t identity) noexcept {
        if (ring.issuer == identity) return {};
        if (ring.issuer != 0) teardown_ring_(ring);
        if (ring.file < 0)
            if (status_t const created = create_ring_(ring); created.failed()) return created;
        if (::syscall(__NR_io_uring_register, ring.file, IORING_REGISTER_ENABLE_RINGS, nullptr, 0) < 0)
            return native_file_failure(errno, "The ring would not bind to this thread");
        ring.issuer = identity;
        return {};
    }

    /** Writes the disk's registered ranges and targets into @p ring's tables when it is behind. */
    status_t carry_tables_(ring_t& ring, std::size_t seat) noexcept {
        std::int32_t const registrations = epoch_(registrations_epoch_).load(std::memory_order_acquire);
        if (ring.carried_registrations != registrations) {
            expected<range_pinning_t> const pinned = pin_ranges_(ring, seat, registrations);
            if (!pinned) return pinned.status();
            ring.pinning = *pinned;
            ring.carried_registrations = registrations;
        }

        std::int32_t const targets = epoch_(targets_epoch_).load(std::memory_order_acquire);
        if (ring.carried_targets != targets) {
            int* const files = &descriptors_[seat * targets_limit_];
            for (std::size_t index = 0; index != targets_limit_; ++index) {
                target_slot_t& slot = targets_[index];
                bool const live = claim_(slot.registered).load(std::memory_order_acquire) != 0;
                files[index] = live ? slot.file : -1;
            }
            io_uring_rsrc_update update {};
            update.data = reinterpret_cast<std::uint64_t>(files);
            if (::syscall(__NR_io_uring_register, ring.file, IORING_REGISTER_FILES_UPDATE, &update, targets_limit_) < 0)
                return native_file_failure(errno, "The ring would not take its registered targets");
            ring.carried_targets = targets;
        }
        return {};
    }

    /**
     *  @brief Pins the disk's ranges into @p ring's table, or answers that the locked-memory cap refused them.
     *
     *  The kernel charges every ring for every range, so the cap is met after a few rings; the first
     *  refusal at an epoch is remembered and later rings skip the attempt, which would pin and unpin
     *  the whole slab only to be refused again.
     */
    expected<range_pinning_t> pin_ranges_(ring_t& ring, std::size_t seat, std::int32_t registrations) noexcept {
        if (epoch_(pin_refused_epoch_).load(std::memory_order_acquire) == registrations)
            return range_pinning_t::unpinned_k;
        ::iovec* const vectors = &vectors_[seat * registrations_limit_];
        for (std::size_t index = 0; index != registrations_limit_; ++index) {
            registration_slot_t& slot = registrations_[index];
            bool const live = claim_(slot.registered).load(std::memory_order_acquire) != 0;
            vectors[index].iov_base = live ? slot.base : nullptr;
            vectors[index].iov_len = live ? static_cast<std::size_t>(slot.length) : 0;
        }
        io_uring_rsrc_update2 update {};
        update.nr = registrations_limit_;
        update.data = reinterpret_cast<std::uint64_t>(vectors);
        long const took = ::syscall(__NR_io_uring_register, ring.file, IORING_REGISTER_BUFFERS_UPDATE, &update,
                                    sizeof(update));
        if (took == static_cast<long>(registrations_limit_)) return range_pinning_t::pinned_k;
        if (took < 0 && errno != ENOMEM)
            return native_file_failure(errno, "The ring would not take its registered ranges");
        epoch_(pin_refused_epoch_).store(registrations, std::memory_order_release);
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

    /** Closes every ring and every open target, which both the destructor and a move assignment need. */
    void close_all_() noexcept {
        for (std::size_t index = 0; index != rings_.size(); ++index) teardown_ring_(rings_[index]);
        for (std::size_t index = 0; index != targets_.size(); ++index)
            if (targets_[index].file != invalid_native_file_k) close_native_file(targets_[index].file);
        rings_.clear();
        targets_.clear();
    }

#pragma endregion Rings

#pragma region Bookkeeping

    /** Where one lease's outstanding requests and local completions sit inside the shared windows. */
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

    /** Marks the range and the target of @p request as live, so neither can be taken back under it. */
    void borrow_(request_t const& request) noexcept {
        if (registration_slot_t* const slot = resolve_(request.buffer); slot)
            in_flight_(*slot).add(1, std::memory_order_acquire);
        if (target_slot_t* const slot = resolve_(request.target); slot)
            in_flight_(*slot).add(1, std::memory_order_acquire);
    }

    /** Releases both once the kernel is done with the bytes of the request @p flight remembers. */
    void restore_(flight_t const& flight) noexcept {
        if (registration_slot_t* const slot = resolve_(flight.buffer); slot)
            in_flight_(*slot).sub(1, std::memory_order_release);
        if (target_slot_t* const slot = resolve_(flight.target); slot)
            in_flight_(*slot).sub(1, std::memory_order_release);
    }

    /** Remembers what @p request will need when only its token and result come back. */
    void track_(owner_slot_t& slot, std::uint32_t base, request_t const& request) noexcept {
        flight_t& flight = flights_[base + slot.flights_count];
        flight.token = request.token;
        flight.length = request.kind == request_kind_t::flush_k ? 0 : request.extent.length;
        flight.buffer = request.buffer;
        flight.target = request.target;
        ++slot.flights_count;
    }

    /** Takes @p token out of @p slot's outstanding set, moving the last entry into the gap. */
    expected<flight_t> retire_(owner_slot_t& slot, std::uint32_t base, request_token_t token) noexcept {
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

    /** Places @p completion at the back of @p slot's own queue, which reap drains after the kernel's. */
    void push_local_(owner_slot_t& slot, std::uint32_t base, completion_t const& completion) noexcept {
        locals_[base + (slot.locals_first + slot.locals_count) % slot.depth] = completion;
        ++slot.locals_count;
    }

    /** Moves what the disk answered itself into @p into, oldest first. */
    std::size_t drain_locals_(owner_slot_t& slot, std::uint32_t base, std::span<completion_t> into) noexcept {
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
        completion.status = {status_code_t::uninitialized_k, "The owner cancelled this request"};
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
        target_slot_t const* const target = resolve_(request.target);
        if (!target) return {status_code_t::invalid_argument_k, "The request names no registered target"};
        if (request.kind == request_kind_t::flush_k) return {};
        registration_slot_t const* const chunk = resolve_(request.buffer);
        if (!chunk) return {status_code_t::invalid_argument_k, "The request names no registered range"};
        if (request.buffer.offset > chunk->length || request.extent.length > chunk->length - request.buffer.offset)
            return {status_code_t::invalid_argument_k, "The request runs past its registered range"};
        if (request.extent.length > max_transfer_bytes_k)
            return {status_code_t::invalid_argument_k, "The request is larger than one transfer"};
        byte_t const* const address = chunk->base + request.buffer.offset;
        if (!is_address_aligned(address, target->memory_alignment) ||
            !request.extent.aligned_to(target->offset_alignment))
            return {status_code_t::invalid_argument_k, "The request does not meet the disk's alignment"};
        return {};
    }

    /** Writes @p request into @p entry, addressing the target by table index and the buffer as @p pinning allows. */
    void build_(io_uring_sqe& entry, request_t const& request, range_pinning_t pinning) noexcept {
        std::memset(&entry, 0, sizeof(entry));
        entry.flags = IOSQE_FIXED_FILE;
        if (request.chaining == request_chaining_t::chained_k) entry.flags |= IOSQE_IO_LINK;
        entry.fd = static_cast<std::int32_t>(request.target.descriptor);
        entry.user_data = request.token.bits;
        if (request.kind == request_kind_t::flush_k) {
            entry.opcode = IORING_OP_FSYNC;
            entry.fsync_flags = IORING_FSYNC_DATASYNC;
            return;
        }
        registration_slot_t const* const chunk = resolve_(request.buffer);
        entry.off = request.extent.offset;
        entry.addr = reinterpret_cast<std::uint64_t>(chunk->base + request.buffer.offset);
        entry.len = static_cast<std::uint32_t>(request.extent.length);
        switch (pinning) {
        case range_pinning_t::pinned_k:
            entry.opcode = request.kind == request_kind_t::read_k ? IORING_OP_READ_FIXED : IORING_OP_WRITE_FIXED;
            entry.buf_index = static_cast<std::uint16_t>(request.buffer.registration);
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
    void refuse_tail_(owner_slot_t& slot, std::uint32_t base, ring_t& ring, unsigned tail, std::size_t count) noexcept {
        std::atomic_ref<unsigned> {*ring.submission_tail}.store(tail, std::memory_order_release);
        status_t const refused {status_code_t::io_k, "The kernel would not accept this request"};
        for (std::size_t index = 0; index != count; ++index) {
            flight_t const& flight = flights_[base + slot.flights_count - 1];
            completion_t const completion = failed_completion_(flight.token, refused);
            restore_(flight);
            --slot.flights_count;
            push_local_(slot, base, completion);
        }
    }

    /** Turns one completion entry into the completion its owner reads, by what the kernel returned. */
    [[nodiscard]] static completion_t translate_(flight_t const& flight, int reason) noexcept {
        completion_t completion;
        completion.token = flight.token;
        if (reason >= 0) {
            completion.transferred = static_cast<std::uint64_t>(reason);
            completion.state = completion.transferred == flight.length ? completion_state_t::completed_k
                                                                       : completion_state_t::partial_k;
            return completion;
        }
        if (reason == -ECANCELED) {
            completion.state = completion_state_t::cancelled_k;
            completion.status = {status_code_t::uninitialized_k, "The owner cancelled this request"};
            return completion;
        }
        completion.state = completion_state_t::failed_k;
        completion.status = native_file_failure(-reason, "The device refused this request");
        return completion;
    }

    /** Copies what the completion queue holds into @p into, dropping the entries nobody asked for. */
    std::size_t harvest_(owner_slot_t& slot, std::uint32_t base, ring_t& ring, std::span<completion_t> into) noexcept {
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

    /** One slot per lease the disk hands out. */
    st::basic_vector<owner_slot_t> owners_;

    /** One ring per lease, since a deferred single-issuer ring belongs to exactly one thread. */
    st::basic_vector<ring_t> rings_;

    /** One slot per memory range the disk records. */
    st::basic_vector<registration_slot_t> registrations_;

    /** One slot per target the disk opens. */
    st::basic_vector<target_slot_t> targets_;

    /** Every lease's outstanding requests, in windows of @c depth_limit_. */
    st::basic_vector<flight_t> flights_;

    /** Every lease's own completions, in windows of @c depth_limit_ read as a circle. */
    st::basic_vector<completion_t> locals_;

    /** One buffer table per ring, written by that ring's own thread. */
    st::basic_vector<::iovec> vectors_;

    /** One file table per ring, written by that ring's own thread. */
    st::basic_vector<int> descriptors_;

    /** Raised whenever a range is recorded or given back, so a ring knows its table is behind. */
    std::int32_t registrations_epoch_ = 1;

    /** Raised whenever a target is opened or closed, so a ring knows its table is behind. */
    std::int32_t targets_epoch_ = 1;

    /** The registration epoch at which the locked-memory cap refused a pin, or zero while none was refused. */
    std::int32_t pin_refused_epoch_ = 0;

    /** Requests one lease may keep in flight, and the stride of every lease's window. */
    std::uint32_t depth_limit_ = 0;

    /** Leases the disk hands out at once, which is also the number of rings it holds. */
    std::uint16_t owners_limit_ = 0;

    /** Memory ranges the disk records at once, and the width of every ring's buffer table. */
    std::uint16_t registrations_limit_ = 0;

    /** Targets the disk opens at once, and the width of every ring's file table. */
    std::uint16_t targets_limit_ = 0;

    /** Whether the page cache stands in the path, which fixes what a target with no alignment reports. */
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
class uring_files_disk_t {
  public:

    /** Answers that this platform has no ring to offer. */
    [[nodiscard]] static uring_probe_t probe() noexcept { return uring_probe_t::refused_k; }

    /** Refuses the mount, since this platform has no ring to mount on. */
    static expected<uring_files_disk_t> make(uring_disk_config_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** What this disk would accept, which is nothing at all. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept { return {}; }

    /** Answers that nothing is pinned, since nothing was ever registered. */
    [[nodiscard]] range_pinning_t pinning() const noexcept { return range_pinning_t::unpinned_k; }

    /** Refuses the lease, since there is no ring to lease a share of. */
    expected<ring_owner_t> open(std::uint32_t) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Refuses the handover, since no lease was ever handed out. */
    status_t adopt(ring_owner_t const&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Refuses the return, since no lease was ever handed out. */
    status_t release(ring_owner_t&) noexcept { return {status_code_t::unsupported_k, "This platform has no io_uring"}; }

    /** Refuses the range, since no ring would ever address it. */
    expected<registration_t> register_chunk(std::span<byte_t>, memory_space_t) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Refuses the range, since none was ever recorded. */
    status_t deregister_chunk(registration_t&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Refuses the target, since no ring would ever address it. */
    expected<descriptor_t> register_target(target_request_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Refuses the target, since none was ever opened. */
    status_t deregister_target(descriptor_t&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Takes nothing, since there is no queue to take it into. */
    expected<std::size_t> submit(ring_owner_t const&, std::span<request_t const>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Answers nothing, since nothing was ever submitted. */
    expected<std::size_t> reap(ring_owner_t const&, std::span<completion_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Stops nothing, since nothing was ever submitted. */
    void cancel(ring_owner_t const&) noexcept {}

    /** Refuses the drain, since no lease was ever handed out. */
    status_t drain(ring_owner_t const&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Answers that nothing is outstanding, which is always true here. */
    [[nodiscard]] std::size_t in_flight(ring_owner_t const&) const noexcept { return 0; }
};

#pragma endregion Absent

#endif

static_assert(ring_concept<uring_files_disk_t>, "The io_uring transport is a ring like every other");

} // namespace unum::ustore::nvme
