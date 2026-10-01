/**
 *  @file src/nvme/linux_aio_files_disk.hpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The ring Linux keeps where io_uring is refused: @c O_DIRECT files through raw AIO calls.
 *
 *  Nothing here links libaio. A context is set up with @c io_setup, fed with @c io_submit,
 *  answered through @c io_getevents and taken down with @c io_destroy, which is the whole
 *  interface. Docker's default seccomp profile, gVisor and @c io_uring_disabled refuse io_uring
 *  and keep these four, and so does every kernel below the 6.12 the io_uring transport needs.
 *
 *  A context belongs to one queue pair and is sized to the mount's depth, so the kernel always has
 *  room for what a queue pair has credit for. Unlike a ring, a context binds to no thread, so a
 *  queue pair changes hands without rebuilding anything, and there is nothing to register: a
 *  control block carries its descriptor and its address, and the kernel pins the pages of every
 *  request it starts.
 *
 *  AIO has no links, so a chain lives in user space: a request that waits for its predecessor is
 *  held in its seat and submitted by the poll that sees the predecessor transfer in full, or
 *  cancelled by the poll that sees it fall short. The kernel cancels nothing it accepted on a
 *  regular file, so a cancel stops only what is still held or staged, and a drain waits out the
 *  rest.
 */

#pragma once

#include <cassert> // `assert`
#include <cerrno>  // `errno`, `EAGAIN`, `EINTR`, `EINVAL`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#if defined(__linux__)
#include <linux/aio_abi.h> // `aio_context_t`, `iocb`, `io_event`, `IOCB_CMD_PREAD`, `IOCB_CMD_FDSYNC`
#include <sys/syscall.h>   // `__NR_io_setup`, `__NR_io_submit`, `__NR_io_getevents`, `__NR_io_destroy`
#include <unistd.h>        // `syscall`
#endif

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <forkunion/atomics.hpp>       // `fu::preferred_atomic_ref`
#include <forkunion/capabilities.hpp>  // `fu::preferred_yield_t`
#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/status.hpp" // `status_t`, `status_code_t`, `expected`
#include "ustore/types.hpp"  // `byte_t`

#include "nvme/blocking_files_disk.hpp"    // `native_file_t`, `open_native_file`, `flush_native_file`
#include "nvme/disk.hpp"                   // `calling_thread_identity`
#include "nvme/linux_uring_files_disk.hpp" // `direct_alignment_of`, `direct_alignment_t`

#include "nvme/extent.hpp" // `extent_t`, `portable_alignment_k`
#include "nvme/ring.hpp"   // `request_t`, `completion_t`, `queue_pair_t`

namespace unum::ustore::nvme {

#pragma region Configuration

/**
 *  @brief What a mount fixes about a Linux AIO disk before any queue pair takes a share of it.
 *
 *  Depth is the number worth tuning, as it is for io_uring: one thread at a depth of 256 reads
 *  4 KB blocks about thirty times faster than @c pread on the same thread, since the queue rather
 *  than the thread keeps a drive busy.
 */
struct linux_aio_disk_config_t {

    /** Leases the disk hands out at once, which is also the number of contexts it sets up. */
    std::uint16_t queue_pairs_limit = 1;

    /** Requests one queue pair may keep in flight, which is the credit a queue pair spends and polls back. */
    queue_depth_t depth_limit = 64;

    /** Memory ranges the disk records at once, which the kernel never sees. */
    std::uint16_t registered_memory_limit = 8;

    /** Targets the disk registers at once, which a directory of sorted runs raises above one. */
    std::uint16_t media_limit = 1;

    /** Whether the page cache stands between the context and the medium, which makes every submission synchronous. */
    file_buffering_t buffering = file_buffering_t::unbuffered_k;
};

/** Whether this machine offers Linux AIO, which a mount asks once io_uring was refused. */
enum class linux_aio_probe_t : std::uint8_t {

    /** The kernel set a context up and took it down again. */
    available_k,

    /** The kernel was built without AIO, or a seccomp profile refused it. */
    refused_k,
};

#pragma endregion Configuration

#if defined(__linux__)

#pragma region Disk

/**
 *  @brief Linux AIO over @c O_DIRECT files registered as media, one context per queue pair.
 *
 *  As with the io_uring disk, the queue is the kernel's and only the bookkeeping is the disk's
 *  own: @c submit builds one control block per request and hands the batch over in one
 *  @c io_submit, and @c poll_completions asks @c io_getevents for whatever landed with a minimum
 *  of zero, so neither waits on a completion. @c io_submit itself may still block where the
 *  filesystem allocates blocks for a write into a hole, or where the device queue is full.
 *
 *  A defect the disk can see before the kernel does fails that one request's completion and leaves
 *  its neighbours alone, and so does a control block @c io_submit refuses on its own, such as a
 *  write to a medium opened for reading. A kernel out of room keeps the rest staged for the next
 *  poll, so the batch is still taken whole. Only a token that is not this queue pair's fails the
 *  whole @c submit.
 */
class linux_aio_files_disk_t {
  public:

    linux_aio_files_disk_t() noexcept = default;
    linux_aio_files_disk_t(linux_aio_files_disk_t const&) = delete;
    linux_aio_files_disk_t& operator=(linux_aio_files_disk_t const&) = delete;
    linux_aio_files_disk_t(linux_aio_files_disk_t&&) noexcept = default;

    linux_aio_files_disk_t& operator=(linux_aio_files_disk_t&& other) noexcept {
        if (this == &other) return *this;
        close_all_();
        queue_pairs_ = std::move(other.queue_pairs_);
        registered_memories_ = std::move(other.registered_memories_);
        media_ = std::move(other.media_);
        flights_ = std::move(other.flights_);
        seats_ = std::move(other.seats_);
        staged_ = std::move(other.staged_);
        events_ = std::move(other.events_);
        locals_ = std::move(other.locals_);
        depth_limit_ = other.depth_limit_;
        buffering_ = other.buffering_;
        return *this;
    }

    ~linux_aio_files_disk_t() noexcept { close_all_(); }

    /**
     *  @brief Sets one context up and takes it down again, the only honest availability test.
     *  @return Whether the kernel offers AIO to this process.
     */
    [[nodiscard]] static linux_aio_probe_t probe() noexcept {
        aio_context_t context = 0;
        if (::syscall(__NR_io_setup, 1u, &context) != 0) return linux_aio_probe_t::refused_k;
        [[maybe_unused]] long const destroyed = ::syscall(__NR_io_destroy, context);
        return linux_aio_probe_t::available_k;
    }

    /**
     *  @brief Sets one context up per queue pair, each deep enough for the mount's whole depth.
     *  @param[in] config Leases, per-queue pair depth, the ranges and media, and the buffering.
     *
     *  @note No file is opened here; a mount registers each one as a medium, so one disk serves a
     *      directory of many sorted runs rather than one disk per run.
     */
    static expected<linux_aio_files_disk_t> make(linux_aio_disk_config_t const& config) noexcept {
        if (config.queue_pairs_limit == 0 || config.depth_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A context needs a queue pair and a request of depth"};
        if (config.queue_pairs_limit == static_cast<std::uint16_t>(queue_pair_id_t::unallocated_k) ||
            config.registered_memory_limit == static_cast<std::uint16_t>(registered_memory_id_t::unregistered_k) ||
            config.media_limit == static_cast<std::uint16_t>(registered_medium_id_t::unregistered_k))
            return status_t {status_code_t::invalid_argument_k, "A disk keeps one identifier for an empty handle"};
        if (config.media_limit == 0 || config.registered_memory_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A disk needs a medium and a range to move between"};
        if (config.depth_limit > depth_ceiling_k)
            return status_t {status_code_t::invalid_argument_k, "A context holds fewer requests than that"};

        std::size_t const window = static_cast<std::size_t>(config.queue_pairs_limit) * config.depth_limit;
        linux_aio_files_disk_t disk;
        disk.depth_limit_ = config.depth_limit;
        disk.buffering_ = config.buffering;
        if (status_t const sized = size_(disk.queue_pairs_, config.queue_pairs_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.registered_memories_, config.registered_memory_limit); sized.failed())
            return sized;
        if (status_t const sized = size_(disk.media_, config.media_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.flights_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.seats_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.staged_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.events_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.locals_, window); sized.failed()) return sized;
        for (std::size_t index = 0; index != disk.queue_pairs_.size(); ++index) {
            if (::syscall(__NR_io_setup, static_cast<unsigned>(config.depth_limit),
                          &disk.queue_pairs_[index].context) == 0)
                continue;
            if (errno == EAGAIN)
                return status_t {status_code_t::capacity_exhausted_k, "The contexts outgrow the fs.aio-max-nr sysctl"};
            return native_file_failure(errno, "The kernel refused an AIO context");
        }
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
        reported.queue_pairs_limit = static_cast<std::uint16_t>(queue_pairs_.size());
        reported.media_limit = static_cast<std::uint16_t>(media_.size());
        reported.reaches_gpu_memory = false;
        return reported;
    }

#pragma region Queue Pairs

    /**
     *  @brief Takes a queue pair on @p depth requests of one context for the calling thread.
     *  @param[in] depth Requests this queue pair may keep in flight, at most the mount's limit.
     *  @return The queue pair, or @c capacity_exhausted_k when every context is already out.
     */
    expected<queue_pair_t> acquire_queue_pair(queue_depth_t depth) noexcept {
        if (depth == 0 || depth > depth_limit_)
            return status_t {status_code_t::invalid_argument_k, "A queue pair asks for more depth than the disk has"};
        for (std::size_t index = 0; index != queue_pairs_.size(); ++index) {
            queue_pair_slot_t& slot = queue_pairs_[index];
            std::int32_t expected_free = 0;
            if (!claim_(slot.leased)
                     .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            std::uint32_t const base = window_of_(static_cast<queue_pair_id_t>(index));
            for (std::uint32_t seat = 0; seat != depth; ++seat) seats_[base + seat] = seat;
            slot.depth = depth;
            slot.free_seats = depth;
            slot.staged_count = 0;
            slot.flights_count = 0;
            slot.locals_first = 0;
            slot.locals_count = 0;
            slot.bound_thread = calling_thread_identity();
            return queue_pair_t::leased(static_cast<queue_pair_id_t>(index), slot.generation, depth);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every queue pair of this disk is already held"};
    }

    /**
     *  @brief Binds @p queue_pair to the calling thread, which is how a moved one changes hands.
     *  @param[in] queue_pair The queue pair this thread has taken over, with nothing outstanding.
     *
     *  @note A context answers any thread of the process, so nothing is rebuilt; the check is only
     *      that no request issued from the previous thread is still owed a poll.
     */
    status_t bind_queue_pair(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this disk's"};
        if (outstanding_(*slot) != 0)
            return {status_code_t::invalid_argument_k, "Drain the queue pair before handing it over"};
        slot->bound_thread = calling_thread_identity();
        return {};
    }

    /**
     *  @brief Takes @p queue_pair back once it has nothing outstanding, and empties the handle.
     *  @param[inout] queue_pair The queue pair to return, surrendered on success only.
     */
    status_t release_queue_pair(queue_pair_t& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this disk's"};
        if (outstanding_(*slot) != 0)
            return {status_code_t::invalid_argument_k, "Drain the queue pair before releasing it"};
        ++slot->generation;
        slot->depth = 0;
        slot->free_seats = 0;
        slot->bound_thread = 0;
        claim_(slot->leased).store(0, std::memory_order_release);
        queue_pair.surrender();
        return {};
    }

    /** Requests @p queue_pair has submitted and not yet completed, which its credit bounds. */
    [[nodiscard]] std::size_t outstanding(queue_pair_t const& queue_pair) const noexcept {
        queue_pair_slot_t const* const slot = slot_of_(queue_pair);
        return slot ? outstanding_(*slot) : 0;
    }

#pragma endregion Queue Pairs

#pragma region Registrations

    /**
     *  @brief Records @p memory so requests can address it by slot; the kernel never learns of it.
     *  @param[in] memory Memory the caller owns and keeps alive until it deregisters the range.
     *  @param[in] space Where that memory lives; this transport reaches host memory alone.
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
            return registered_memory_t::registered(static_cast<registered_memory_id_t>(index), slot.generation, memory,
                                                   space);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every registered memory slot of this disk is taken"};
    }

    /**
     *  @brief Gives @p registered_memory back, refusing while the kernel may still write into it.
     *  @param[inout] registered_memory The range to give back, surrendered on success.
     */
    status_t unregister_memory(registered_memory_t& registered_memory) noexcept {
        registered_memory_slot_t* const slot = slot_of_(registered_memory);
        if (!slot) return {status_code_t::invalid_argument_k, "The registered memory is not this disk's"};
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
            return registered_medium_t::registered(static_cast<registered_medium_id_t>(index), slot.generation,
                                                   slot.capacity);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every medium slot of this disk is taken"};
    }

    /**
     *  @brief Closes @p registered_medium, refusing while requests against it are still in flight.
     *  @param[inout] registered_medium The medium to give back, surrendered on success.
     */
    status_t unregister_medium(registered_medium_t& registered_medium) noexcept {
        registered_medium_slot_t* const slot = slot_of_(registered_medium);
        if (!slot) return {status_code_t::invalid_argument_k, "The medium is not this disk's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "The medium is still named by requests in flight"};
        close_native_file(slot->file);
        ++slot->generation;
        slot->file = invalid_native_file_k;
        slot->capacity = 0;
        claim_(slot->registered).store(0, std::memory_order_release);
        registered_medium.surrender();
        return {};
    }

#pragma endregion Registrations

#pragma region Ring

    /**
     *  @brief Takes as much of @p requests as @p queue_pair still has credit for, and never waits.
     *  @param[in] queue_pair The queue pair submitting, held by the calling thread.
     *  @param[in] requests The batch, of which a prefix is taken.
     *  @return How many were taken, which is zero on a full queue pair and never a reason to block.
     *
     *  @note A request chained to its predecessor waits in its seat rather than in the kernel, and
     *      a request after a defect in the same chain is answered as cancelled right away.
     */
    expected<std::size_t> submit(queue_pair_t const& queue_pair, std::span<request_t const> requests) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The queue pair is not this disk's"};
        assert(slot->bound_thread == calling_thread_identity() && "Only the thread holding a queue pair submits on it");
        for (request_t const& request : requests)
            if (request.token.queue_pair() != queue_pair.id() || request.token.generation() != queue_pair.generation())
                return status_t {status_code_t::invalid_argument_k, "A request carries another queue pair's token"};

        std::uint32_t const base = window_of_(queue_pair.id());
        std::size_t const credit = slot->depth - outstanding_(*slot);
        std::size_t const taking = requests.size() < credit ? requests.size() : credit;
        std::uint32_t linked = no_seat_k;
        bool cut = false;
        for (std::size_t index = 0; index != taking; ++index) {
            request_t const& request = requests[index];
            if (cut) {
                push_local_(*slot, base, cancelled_completion_(request.token));
                cut = chains_next_(request);
                continue;
            }
            status_t const defect = inspect_(request);
            if (defect.failed()) {
                push_local_(*slot, base, failed_completion_(request.token, defect));
                linked = no_seat_k;
                cut = chains_next_(request);
                continue;
            }
            std::uint32_t const seat = take_seat_(*slot, base);
            flight_t& flight = flights_[base + seat];
            flight.request = request;
            build_(flight, seat);
            borrow_(request);
            if (linked != no_seat_k) flights_[base + linked].successor = seat;
            else stage_(*slot, base, seat);
            linked = chains_next_(request) ? seat : no_seat_k;
        }
        issue_(*slot, base);
        return taking;
    }

    /**
     *  @brief Copies what the kernel answered into @p into, and submits what that released.
     *  @param[in] queue_pair The queue pair polling, held by the thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero when nothing has landed.
     */
    expected<std::size_t> poll_completions(queue_pair_t const& queue_pair, std::span<completion_t> into) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The queue pair is not this disk's"};
        assert(slot->bound_thread == calling_thread_identity() && "Only the thread holding a queue pair polls on it");
        std::uint32_t const base = window_of_(queue_pair.id());

        std::size_t moved = 0;
        if (slot->flights_count != 0 && !into.empty()) {
            std::size_t const room = into.size() < slot->flights_count ? into.size() : slot->flights_count;
            long const landed = ::syscall(__NR_io_getevents, slot->context, 0L, static_cast<long>(room), &events_[base],
                                          nullptr);
            if (landed < 0 && errno != EINTR)
                return native_file_failure(errno, "The kernel would not hand completions back");
            for (long index = 0; index < landed; ++index) {
                ::io_event const& event = events_[base + static_cast<std::size_t>(index)];
                std::uint32_t const seat = static_cast<std::uint32_t>(event.data);
                --slot->flights_count;
                into[moved++] = retire_(*slot, base, seat, translate_(flights_[base + seat], event.res));
            }
        }
        issue_(*slot, base);
        moved += drain_locals_(*slot, base, into.subspan(moved));
        return moved;
    }

    /**
     *  @brief Answers everything @p queue_pair holds back from the kernel as cancelled.
     *  @param[in] queue_pair The queue pair to stop, whose accepted requests run to their end.
     *
     *  @note A regular file registers no cancel callback, so @c io_cancel could only fail; what
     *      stops are the requests still waiting on a predecessor and those a full kernel left
     *      staged.
     */
    void cancel(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return;
        std::uint32_t const base = window_of_(queue_pair.id());
        std::uint32_t const staged = slot->staged_count;
        slot->staged_count = 0;
        for (std::uint32_t index = 0; index != staged; ++index) {
            std::uint32_t const seat = static_cast<std::uint32_t>(staged_[base + index]->aio_data);
            completion_t const cancelled = cancelled_completion_(flights_[base + seat].request.token);
            push_local_(*slot, base, retire_(*slot, base, seat, cancelled));
        }
        for (std::uint32_t seat = 0; seat != slot->depth; ++seat) {
            std::uint32_t const successor = flights_[base + seat].successor;
            if (successor == no_seat_k) continue;
            flights_[base + seat].successor = no_seat_k;
            cut_chain_(*slot, base, successor);
        }
    }

    /**
     *  @brief Finishes and discards everything @p queue_pair has outstanding, freeing its buffers.
     *  @param[in] queue_pair The queue pair to empty, which may be cancelled or still running.
     */
    status_t drain(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t const* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this disk's"};
        std::array<completion_t, drain_batch_k> discarded {};
        for (std::size_t spins = 0; spins != drain_spin_limit_k; ++spins) {
            if (outstanding_(*slot) == 0) return {};
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

    /** One queue pair's share of the disk: its context, its seats, and the queues it polls from. */
    struct queue_pair_slot_t {

        /** Zero while the slot is free, one while a queue pair holds it. */
        std::int32_t leased = 0;

        /** Raised every time the slot is leased again, so a stale token routes nowhere. */
        queue_pair_generation_t generation = 0;

        /** Requests this queue pair may keep in flight, fixed when the queue pair was taken. */
        queue_depth_t depth = 0;

        /** The kernel's context, set up at mount for the mount's whole depth and kept across leases. */
        aio_context_t context = 0;

        /** Seats no request occupies, stacked at the front of this queue pair's seat window. */
        std::uint32_t free_seats = 0;

        /** Control blocks built and not yet accepted, in order at the front of this queue pair's staging window. */
        std::uint32_t staged_count = 0;

        /** Requests the kernel accepted and has not yet answered for. */
        std::uint32_t flights_count = 0;

        /** Where this queue pair's oldest unreaped local completion sits inside its window. */
        std::uint32_t locals_first = 0;

        /** Completions the disk answered itself, which no event ever carried. */
        std::uint32_t locals_count = 0;

        /** The thread that took the queue pair, which every submit and poll is checked against. */
        thread_identity_t bound_thread = 0;
    };

    /** One recorded memory range, and the count of requests the kernel may still be writing into it. */
    struct registered_memory_slot_t {

        /** Zero while the slot is free, one while a range is recorded in it. */
        std::int32_t registered = 0;

        /** Requests accepted against this range and not yet answered for. */
        std::int32_t in_flight = 0;

        /** Raised every time the slot is used again, so a stale place resolves to nothing. */
        queue_pair_generation_t generation = 0;

        /** Where the memory lives, which is host memory for every range this disk takes. */
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

        /** The open file, which every control block against this medium carries. */
        native_file_t file = invalid_native_file_k;

        /** Bytes the medium held at registration: a regular file grows past it, a block device falls short. */
        std::uint64_t capacity = 0;

        /** Alignment this filesystem demands of the first byte of a buffer. */
        std::uint64_t memory_alignment = portable_alignment_k;

        /** Alignment this filesystem demands of an extent's offset and its length. */
        std::uint64_t offset_alignment = portable_alignment_k;
    };

    /** One request from the moment it is taken until its completion is handed back, in a seat of its queue pair. */
    struct flight_t {

        /** The control block @c io_submit copies, whose @c aio_data names this seat in the event. */
        ::iocb control {};

        /** The request as it was taken, since an event carries only the seat and the result. */
        request_t request;

        /** The seat of the request held back until this one transfers in full, or @c no_seat_k. */
        std::uint32_t successor = no_seat_k;
    };

    /** The seat that names no request, which ends a chain. */
    static constexpr std::uint32_t no_seat_k = ~std::uint32_t {0};

    /** Requests one context may hold, which the default @c fs.aio-max-nr also caps for the whole machine. */
    static constexpr queue_depth_t depth_ceiling_k = 65536;

    /** The largest extent one control block carries, kept well inside the kernel's own limit. */
    static constexpr std::uint64_t max_transfer_bytes_k = 1024ull * 1024ull * 1024ull;

    /** Completions one drain discards at a time. */
    static constexpr std::size_t drain_batch_k = 32;

    /** Fruitless polls a drain tolerates before it stops what is still held back. */
    static constexpr std::size_t drain_patience_k = 1024;

    /** Polls a drain performs before it calls the context broken. */
    static constexpr std::size_t drain_spin_limit_k = std::size_t {1} << 24;

    /** Grows @p elements to @p count default slots, reporting an allocation failure as a status. */
    template <typename element_type_>
    static status_t size_(st::basic_vector<element_type_>& elements, std::size_t count) noexcept {
        if (elements.resize(count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A disk could not allocate its slots"};
        return {};
    }

    static fu::preferred_atomic_ref<std::int32_t> claim_(std::int32_t& word) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {word};
    }

    static fu::preferred_atomic_ref<std::int32_t> in_flight_(registered_memory_slot_t& slot) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {slot.in_flight};
    }

    static fu::preferred_atomic_ref<std::int32_t> in_flight_(registered_medium_slot_t& slot) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {slot.in_flight};
    }

    /** Takes every context down, which waits out whatever the kernel still holds, then closes every medium. */
    void close_all_() noexcept {
        for (std::size_t index = 0; index != queue_pairs_.size(); ++index) {
            if (queue_pairs_[index].context == 0) continue;
            [[maybe_unused]] long const destroyed = ::syscall(__NR_io_destroy, queue_pairs_[index].context);
        }
        for (std::size_t index = 0; index != media_.size(); ++index)
            if (media_[index].file != invalid_native_file_k) close_native_file(media_[index].file);
        queue_pairs_.clear();
        media_.clear();
    }

#pragma region Bookkeeping

    /** Where one queue pair's seats, staging, events and local completions sit inside the shared windows. */
    [[nodiscard]] std::uint32_t window_of_(queue_pair_id_t id) const noexcept {
        return static_cast<std::uint32_t>(id) * depth_limit_;
    }

    /** Requests taken and not yet handed back: every occupied seat and every unreaped local completion. */
    [[nodiscard]] static std::size_t outstanding_(queue_pair_slot_t const& slot) noexcept {
        return (slot.depth - slot.free_seats) + slot.locals_count;
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

    /** Releases both once the kernel is done with the bytes of @p request. */
    void restore_(request_t const& request) noexcept {
        if (registered_memory_slot_t* const slot = resolve_(request.buffer); slot)
            in_flight_(*slot).sub(1, std::memory_order_release);
        if (registered_medium_slot_t* const slot = resolve_(request.medium); slot)
            in_flight_(*slot).sub(1, std::memory_order_release);
    }

    /** Pops a free seat of @p slot's window, which its credit guarantees there is. */
    std::uint32_t take_seat_(queue_pair_slot_t& slot, std::uint32_t base) noexcept {
        return seats_[base + --slot.free_seats];
    }

    /** Pushes @p seat back onto @p slot's free stack, forgetting any request it held back. */
    void give_seat_(queue_pair_slot_t& slot, std::uint32_t base, std::uint32_t seat) noexcept {
        flights_[base + seat].successor = no_seat_k;
        seats_[base + slot.free_seats++] = seat;
    }

    /** Queues the control block of @p seat for the next @c io_submit, behind everything staged before it. */
    void stage_(queue_pair_slot_t& slot, std::uint32_t base, std::uint32_t seat) noexcept {
        staged_[base + slot.staged_count++] = &flights_[base + seat].control;
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

    /**
     *  @brief Frees @p seat, whose request ended as @p completion, and settles its successor.
     *  @return The completion itself, for the caller to hand back or queue locally.
     */
    completion_t retire_(queue_pair_slot_t& slot, std::uint32_t base, std::uint32_t seat,
                         completion_t const& completion) noexcept {
        flight_t const& flight = flights_[base + seat];
        std::uint32_t const successor = flight.successor;
        restore_(flight.request);
        give_seat_(slot, base, seat);
        if (successor == no_seat_k) return completion;
        if (completion.completed_in_full()) stage_(slot, base, successor);
        else cut_chain_(slot, base, successor);
        return completion;
    }

    /** Answers the request in @p seat and every request held behind it as cancelled. */
    void cut_chain_(queue_pair_slot_t& slot, std::uint32_t base, std::uint32_t seat) noexcept {
        while (seat != no_seat_k) {
            flight_t const& flight = flights_[base + seat];
            std::uint32_t const successor = flight.successor;
            push_local_(slot, base, cancelled_completion_(flight.request.token));
            restore_(flight.request);
            give_seat_(slot, base, seat);
            seat = successor;
        }
    }

#pragma endregion Bookkeeping

#pragma region Requests

    /** The completion a request gets when its predecessor fell short, or its queue pair cancelled it. */
    [[nodiscard]] static completion_t cancelled_completion_(request_token_t token) noexcept {
        completion_t completion;
        completion.token = token;
        completion.state = completion_state_t::cancelled_k;
        completion.status = {status_code_t::uninitialized_k, "The queue pair cancelled this request"};
        return completion;
    }

    /** The completion a request gets when the disk or @c io_submit refused it before it started. */
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

    /** Whatever would corrupt a control block rather than merely fail on the device. */
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

    /** Writes the control block of the request in @p flight, which its event names by @p seat. */
    void build_(flight_t& flight, std::uint32_t seat) noexcept {
        request_t const& request = flight.request;
        ::iocb& control = flight.control;
        control = ::iocb {};
        control.aio_data = seat;
        control.aio_fildes = static_cast<std::uint32_t>(resolve_(request.medium)->file);
        if (request.kind == request_kind_t::flush_k) {
            control.aio_lio_opcode = static_cast<std::uint16_t>(IOCB_CMD_FDSYNC);
            return;
        }
        registered_memory_slot_t const* const memory = resolve_(request.buffer);
        control.aio_lio_opcode = static_cast<std::uint16_t>(request.kind == request_kind_t::read_k ? IOCB_CMD_PREAD
                                                                                                   : IOCB_CMD_PWRITE);
        control.aio_buf = reinterpret_cast<std::uint64_t>(memory->base + request.buffer.offset);
        control.aio_nbytes = request.extent.length;
        control.aio_offset = static_cast<std::int64_t>(request.extent.offset);
    }

    /**
     *  @brief Hands the staged control blocks to the kernel in order, until it has all or no room.
     *
     *  @c io_submit stops at the first control block it cannot start and answers how many it
     *  took, or the error of that one when it took none. A full kernel leaves the rest staged
     *  for the next poll; any other refusal belongs to that one request, so it fails alone and
     *  the batch behind it goes on. A flush a kernel older than 4.18 refuses with @c EINVAL runs
     *  as @c fdatasync instead.
     */
    void issue_(queue_pair_slot_t& slot, std::uint32_t base) noexcept {
        ::iocb** const staged = &staged_[base];
        while (slot.staged_count != 0) {
            long const accepted = ::syscall(__NR_io_submit, slot.context, static_cast<long>(slot.staged_count), staged);
            if (accepted > 0) {
                std::uint32_t const taken = static_cast<std::uint32_t>(accepted);
                slot.flights_count += taken;
                slot.staged_count -= taken;
                for (std::uint32_t index = 0; index != slot.staged_count; ++index)
                    staged[index] = staged[taken + index];
                continue;
            }
            int const reason = accepted < 0 ? errno : EAGAIN;
            if (reason == EINTR) continue;
            if (reason == EAGAIN) return;
            std::uint32_t const seat = static_cast<std::uint32_t>(staged[0]->aio_data);
            --slot.staged_count;
            for (std::uint32_t index = 0; index != slot.staged_count; ++index) staged[index] = staged[index + 1];
            push_local_(slot, base, retire_(slot, base, seat, refused_completion_(flights_[base + seat], reason)));
        }
    }

    /** The completion of a control block @c io_submit refused on its own, for the @p reason it gave. */
    [[nodiscard]] static completion_t refused_completion_(flight_t const& flight, int reason) noexcept {
        if (flight.request.kind != request_kind_t::flush_k || reason != EINVAL)
            return failed_completion_(flight.request.token,
                                      native_file_failure(reason, "The kernel refused this request"));
        status_t const flushed = flush_native_file(static_cast<native_file_t>(flight.control.aio_fildes));
        return flushed.failed() ? failed_completion_(flight.request.token, flushed) : translate_(flight, 0);
    }

    /** Turns one event into the completion its queue pair reads, by what the kernel returned. */
    [[nodiscard]] static completion_t translate_(flight_t const& flight, std::int64_t result) noexcept {
        completion_t completion;
        completion.token = flight.request.token;
        if (result >= 0) {
            std::uint64_t const length = flight.request.kind == request_kind_t::flush_k ? 0
                                                                                        : flight.request.extent.length;
            completion.transferred_bytes = static_cast<std::uint64_t>(result);
            completion.state = completion.transferred_bytes == length ? completion_state_t::completed_k
                                                                      : completion_state_t::partial_k;
            return completion;
        }
        completion.state = completion_state_t::failed_k;
        completion.status = native_file_failure(static_cast<int>(-result), "The device refused this request");
        return completion;
    }

#pragma endregion Requests

    /** One slot per queue pair the disk hands out, each with its own context. */
    st::basic_vector<queue_pair_slot_t> queue_pairs_;

    /** One slot per memory range the disk records. */
    st::basic_vector<registered_memory_slot_t> registered_memories_;

    /** One slot per medium the disk opens. */
    st::basic_vector<registered_medium_slot_t> media_;

    /** Every queue pair's seats, in windows of @c depth_limit_, whose control blocks never move once built. */
    st::basic_vector<flight_t> flights_;

    /** Every queue pair's free seats, stacked in windows of @c depth_limit_. */
    st::basic_vector<std::uint32_t> seats_;

    /** Every queue pair's control blocks awaiting @c io_submit, in windows of @c depth_limit_. */
    st::basic_vector<::iocb*> staged_;

    /** Every queue pair's room for the events one @c io_getevents copies out, in windows of @c depth_limit_. */
    st::basic_vector<::io_event> events_;

    /** Every queue pair's own completions, in windows of @c depth_limit_ read as a circle. */
    st::basic_vector<completion_t> locals_;

    /** Requests one queue pair may keep in flight, the depth of every context, and the stride of every window. */
    queue_depth_t depth_limit_ = 0;

    /** Whether the page cache stands in the path, which fixes what a medium with no alignment reports. */
    file_buffering_t buffering_ = file_buffering_t::unbuffered_k;
};

#pragma endregion Disk

#else

#pragma region Absent

/**
 *  @brief The same disk on a platform without Linux AIO, which mounts nowhere and refuses cleanly.
 *
 *  The type exists so a caller compiles against it unconditionally and chooses at mount, where a
 *  refused @c make sends it to the blocking floor rather than to a build-time branch.
 */
class linux_aio_files_disk_t {
  public:

    /** Answers that this platform has no AIO to offer. */
    [[nodiscard]] static linux_aio_probe_t probe() noexcept { return linux_aio_probe_t::refused_k; }

    /** Refuses the mount, since this platform has no context to mount on. */
    static expected<linux_aio_files_disk_t> make(linux_aio_disk_config_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** What this disk would accept, which is nothing at all. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept { return {}; }

    /** Refuses the queue pair, since there is no context to take a share of. */
    expected<queue_pair_t> acquire_queue_pair(std::uint32_t) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Refuses the handover, since no queue pair was ever handed out. */
    status_t bind_queue_pair(queue_pair_t const&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Refuses the return, since no queue pair was ever handed out. */
    status_t release_queue_pair(queue_pair_t&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Refuses the range, since no context would ever address it. */
    expected<registered_memory_t> register_memory(std::span<byte_t>, memory_space_t) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Refuses the range, since none was ever recorded. */
    status_t unregister_memory(registered_memory_t&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Refuses the medium, since no context would ever address it. */
    expected<registered_medium_t> register_medium(medium_options_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Refuses the medium, since none was ever opened. */
    status_t unregister_medium(registered_medium_t&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Takes nothing, since there is no context to take it into. */
    expected<std::size_t> submit(queue_pair_t const&, std::span<request_t const>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Answers nothing, since nothing was ever submitted. */
    expected<std::size_t> poll_completions(queue_pair_t const&, std::span<completion_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Stops nothing, since nothing was ever submitted. */
    void cancel(queue_pair_t const&) noexcept {}

    /** Refuses the drain, since no queue pair was ever handed out. */
    status_t drain(queue_pair_t const&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Answers that nothing is outstanding, which is always true here. */
    [[nodiscard]] std::size_t outstanding(queue_pair_t const&) const noexcept { return 0; }
};

#pragma endregion Absent

#endif

static_assert(ring_concept<linux_aio_files_disk_t>, "The Linux AIO transport is a ring like every other");

} // namespace unum::ustore::nvme
