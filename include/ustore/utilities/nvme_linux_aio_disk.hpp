/**
 *  @file include/ustore/utilities/nvme_linux_aio_disk.hpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The ring Linux keeps where io_uring is refused: @c O_DIRECT files through raw AIO calls.
 *
 *  Nothing here links libaio. A context is set up with @c io_setup, fed with @c io_submit,
 *  answered through @c io_getevents and taken down with @c io_destroy, which is the whole
 *  interface. Docker's default seccomp profile, gVisor and @c io_uring_disabled refuse io_uring
 *  and keep these four, and so does every kernel below the 6.12 the io_uring transport needs.
 *
 *  A context belongs to a queue of one thread rather than to the disk, and is sized to the queue's
 *  depth, so the kernel always has room for what the queue has credit for. Unlike a ring, a context
 *  registers nothing, so a disk has nothing to register into it: a control block holds its
 *  descriptor, read from the disk's media at submission, and its address, and the kernel pins the
 *  pages of every request it starts.
 *
 *  AIO has no links, so a chain lives in user space: a request that waits for its predecessor is
 *  held in its request_slot and submitted by the poll that sees the predecessor transfer in
 *  full, or cancelled by the poll that sees it fall short. The kernel cancels nothing it
 *  accepted on a regular file, so a cancel stops only what is still held or staged, and a drain
 *  waits out the rest.
 */

#pragma once

#include <cerrno>  // `errno`, `EAGAIN`, `EINTR`, `EINVAL`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include "ustore/types.h" // `USTORE_OS_LINUX_`

#if USTORE_OS_LINUX_
#include <linux/aio_abi.h> // `aio_context_t`, `iocb`, `io_event`, `IOCB_CMD_PREAD`, `IOCB_CMD_FDSYNC`
#include <sys/syscall.h>   // `__NR_io_setup`, `__NR_io_submit`, `__NR_io_getevents`, `__NR_io_destroy`
#include <unistd.h>        // `syscall`
#endif

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::move`, `std::exchange`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/nvme_disks.hpp" // `blocking_queue_t`, `file_access_of`, `medium_bytes_of`, `direct_alignment_of`
#include "ustore/utilities/nvme_ring.hpp" // `request_t`, `completion_t`, `queue_ref_t`, `portable_alignment_k`, `is_address_aligned`
#include "ustore/utilities/types.hpp" // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::utilities {

#pragma region Configuration

/** What a mount fixes about a Linux AIO disk before any queue submits on it. */
struct linux_aio_disk_config_t {

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

#if USTORE_OS_LINUX_

#pragma region Queue

template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_aio_files_disk;

/**
 *  @brief One AIO context a thread owns: its request_slots, the control blocks it stages, the
 *      events it reaps into, and the completions the disk answers itself.
 *
 *  Depth is the number worth tuning, as it is for io_uring: one thread at a depth of 256 reads 4 KB
 *  blocks about thirty times faster than @c pread on the same thread, since the queue rather than
 *  the thread keeps a drive busy.
 */
class linux_aio_queue_t {
  public:

    linux_aio_queue_t() noexcept = default;
    linux_aio_queue_t(linux_aio_queue_t const&) = delete;
    linux_aio_queue_t& operator=(linux_aio_queue_t const&) = delete;

    linux_aio_queue_t(linux_aio_queue_t&& other) noexcept
        : context_(std::exchange(other.context_, aio_context_t {0})),
          submitted_requests_(std::move(other.submitted_requests_)), request_slots_(std::move(other.request_slots_)),
          staged_(std::move(other.staged_)), events_(std::move(other.events_)),
          completions_made_by_queue_(std::move(other.completions_made_by_queue_)),
          pinned_staging_(other.pinned_staging_), depth_(other.depth_), free_request_slots_(other.free_request_slots_),
          staged_count_(other.staged_count_), outstanding_(other.outstanding_),
          completions_made_by_queue_first_(other.completions_made_by_queue_first_),
          completions_made_by_queue_count_(other.completions_made_by_queue_count_) {}

    linux_aio_queue_t& operator=(linux_aio_queue_t&& other) noexcept {
        if (this == &other) return *this;
        teardown_(context_);
        context_ = std::exchange(other.context_, aio_context_t {0});
        submitted_requests_ = std::move(other.submitted_requests_);
        request_slots_ = std::move(other.request_slots_);
        staged_ = std::move(other.staged_);
        events_ = std::move(other.events_);
        completions_made_by_queue_ = std::move(other.completions_made_by_queue_);
        pinned_staging_ = other.pinned_staging_;
        depth_ = other.depth_;
        free_request_slots_ = other.free_request_slots_;
        staged_count_ = other.staged_count_;
        outstanding_ = other.outstanding_;
        completions_made_by_queue_first_ = other.completions_made_by_queue_first_;
        completions_made_by_queue_count_ = other.completions_made_by_queue_count_;
        return *this;
    }

    ~linux_aio_queue_t() noexcept { teardown_(context_); }

    /**
     *  @brief Sets one context up with room for @p depth requests.
     *  @param[in] depth Requests the queue keeps in flight at once.
     *  @param[in] range The thread's blocks, which requests naming @c queue_range_k
     *      address, or empty.
     *  @return The queue, or @c capacity_exhausted_k when the contexts outgrow the
     *      @c fs.aio-max-nr sysctl.
     */
    static expected<linux_aio_queue_t> make(queue_depth_t depth, std::span<byte_t> range) noexcept {
        if (depth == 0 || depth > depth_ceiling_k)
            return status_t {status_code_t::invalid_argument_k, "A context holds between one and 65536 requests"};
        linux_aio_queue_t queue;
        if (queue.submitted_requests_.resize(depth) != st::status_t::success_k ||
            queue.request_slots_.resize(depth) != st::status_t::success_k ||
            queue.staged_.resize(depth) != st::status_t::success_k ||
            queue.events_.resize(depth) != st::status_t::success_k ||
            queue.completions_made_by_queue_.resize(depth) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A context could not size its windows"};
        if (::syscall(__NR_io_setup, static_cast<unsigned>(depth), &queue.context_) != 0) {
            if (errno == EAGAIN)
                return status_t {status_code_t::capacity_exhausted_k, "The contexts outgrow the fs.aio-max-nr sysctl"};
            return native_file_failure(errno, "The kernel refused an AIO context");
        }
        for (std::uint32_t request_slot = 0; request_slot != depth; ++request_slot)
            queue.request_slots_[request_slot] = request_slot;
        queue.depth_ = depth;
        queue.free_request_slots_ = depth;
        queue.pinned_staging_ = range;
        return queue;
    }

    /** Requests the queue keeps in flight at once. */
    [[nodiscard]] queue_depth_t depth() const noexcept { return depth_; }

  private:

    template <template <typename> typename atomic_reference_, typename waiter_type_>
    friend class linux_aio_files_disk;

    /** One request from the moment it is taken until its completion is handed back, in a
     *  request_slot of its queue. */
    struct submitted_request_t {

        /** The control block @c io_submit copies, whose @c aio_data names this request_slot
         *  in the event. */
        ::iocb control {};

        /** The request as it was taken, since an event holds only the request_slot
         *  and the result. */
        request_t request;

        /** The request_slot of the request held back until this one transfers in full,
         *  or @c no_request_slot_k. */
        std::uint32_t successor = no_request_slot_k;
    };

    /** The request_slot that names no request, which ends a chain. */
    static constexpr std::uint32_t no_request_slot_k = ~std::uint32_t {0};

    /** Requests one context may hold, which the default @c fs.aio-max-nr also caps for
     *  the whole machine. */
    static constexpr queue_depth_t depth_ceiling_k = 65536;

    /** Takes @p context down, which waits out whatever the kernel still holds for it. */
    static void teardown_(aio_context_t& context) noexcept {
        if (context == 0) return;
        [[maybe_unused]] long const destroyed = ::syscall(__NR_io_destroy, context);
        context = 0;
    }

    /** The kernel's context, zero before it is set up and after it is taken down. */
    aio_context_t context_ = 0;

    /** The request_slots, whose control blocks never move once built. */
    st::vector<submitted_request_t> submitted_requests_;

    /** The free request_slots, stacked at the front. */
    st::vector<std::uint32_t> request_slots_;

    /** The control blocks awaiting @c io_submit, in order at the front. */
    st::vector<::iocb*> staged_;

    /** Room for the events one @c io_getevents copies out. */
    st::vector<::io_event> events_;

    /** The completions the disk answered itself, read as a circle. */
    st::vector<completion_t> completions_made_by_queue_;

    /** The thread's blocks, which a request naming @c queue_range_k addresses. */
    std::span<byte_t> pinned_staging_;

    /** Requests the queue keeps in flight at once, and the depth of its context. */
    queue_depth_t depth_ = 0;

    /** Seats no request occupies, at the front of @c request_slots_. */
    std::uint32_t free_request_slots_ = 0;

    /** Control blocks built and not yet accepted, at the front of @c staged_. */
    std::uint32_t staged_count_ = 0;

    /** Requests the kernel accepted and has not yet answered for. */
    std::uint32_t outstanding_ = 0;

    /** Where the oldest unreaped local completion sits inside @c completions_made_by_queue_. */
    std::uint32_t completions_made_by_queue_first_ = 0;

    /** Completions the disk answered itself, which no event ever held. */
    std::uint32_t completions_made_by_queue_count_ = 0;
};

#pragma endregion Queue

#pragma region Disk

/**
 *  @brief Linux AIO over @c O_DIRECT files registered as media, one context per queue set queue.
 *
 *  As with the io_uring disk, the queue is the kernel's and only the bookkeeping is the disk's own:
 *  @c submit builds one control block per request and hands the batch over in one @c io_submit, and
 *  @c poll_completions asks @c io_getevents for whatever landed with a minimum of zero, so neither
 *  waits on a completion. @c io_submit itself may still block where the filesystem allocates blocks
 *  for a write into a hole, or where the device queue is full. It serves a @c blocking_queue_t too,
 *  with positioned transfers on the polling thread.
 *
 *  A defect the disk can see before the kernel does fails that one request's completion and
 *  leaves its neighbours alone, and so does a control block @c io_submit refuses on its own, such
 *  as a write to a medium opened for reading. A kernel out of room keeps the rest staged for the
 *  next poll, so the batch is still taken whole. Only a token that is not this queue's fails the
 *  whole @c submit.
 *
 *  @tparam atomic_reference_ The reference the claim and in-submitted words are owned through.
 *  @tparam waiter_type_ What a draining core does after a poll that moved nothing.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_aio_files_disk {
  public:

    /** The queue a thread owns for this transport. */
    using queue_t = linux_aio_queue_t;

    linux_aio_files_disk() noexcept = default;
    linux_aio_files_disk(linux_aio_files_disk const&) = delete;
    linux_aio_files_disk& operator=(linux_aio_files_disk const&) = delete;
    linux_aio_files_disk(linux_aio_files_disk&&) noexcept = default;

    linux_aio_files_disk& operator=(linux_aio_files_disk&& other) noexcept {
        if (this == &other) return *this;
        close_all_();
        registered_memories_ = std::move(other.registered_memories_);
        media_ = std::move(other.media_);
        buffering_ = other.buffering_;
        return *this;
    }

    ~linux_aio_files_disk() noexcept { close_all_(); }

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
     *  @brief Sizes the tables of ranges and media the queue sets' contexts address.
     *  @param[in] config The ranges and media recorded at once, and the buffering.
     *
     *  @note No file is opened here; a mount registers each one as a medium, so one disk serves a
     *      directory of many sorted runs rather than one disk per run.
     */
    static expected<linux_aio_files_disk> make(linux_aio_disk_config_t const& config) noexcept {
        if (config.registered_memory_limit >= static_cast<std::uint16_t>(registered_memory_id_t::queue_range_k) ||
            config.media_limit == static_cast<std::uint16_t>(registered_medium_id_t::unregistered_k))
            return status_t {status_code_t::invalid_argument_k, "A disk limit reaches a reserved id"};
        if (config.media_limit == 0 || config.registered_memory_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A disk needs a medium and a range"};

        linux_aio_files_disk disk;
        disk.buffering_ = config.buffering;
        if (status_t const sized = size_(disk.registered_memories_, config.registered_memory_limit); sized.failed())
            return sized;
        if (status_t const sized = size_(disk.media_, config.media_limit); sized.failed()) return sized;
        return disk;
    }

    /** What this disk accepts, which the tightest alignment across its registered media fixes. */
    [[nodiscard]] disk_capabilities_t capabilities() const noexcept {
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
        disk_capabilities_t reported;
        reported.memory_alignment = memory != 0 ? memory : unregistered;
        reported.offset_alignment = offset != 0 ? offset : unregistered;
        reported.length_granularity = reported.offset_alignment;
        reported.max_transfer_bytes = max_transfer_bytes_k;
        reported.depth_limit = linux_aio_queue_t::depth_ceiling_k;
        reported.media_limit = static_cast<std::uint16_t>(media_.size());
        reported.reaches_gpu_memory = false;
        return reported;
    }

#pragma region Queues

    /**
     *  @brief Checks that @p queue_pair names a queue this disk serves, which needs nothing
     *      installed: a request reads its descriptor from this disk's media at submission.
     *  @param[in] queue_pair The queue the calling thread owns.
     */
    status_t register_media(queue_ref_t const& queue_pair) noexcept {
        if (queue_pair.template queue<blocking_queue_t>() || queue_of_(queue_pair)) return {};
        return {status_code_t::invalid_argument_k, "The queue is of another transport"};
    }

    /** Requests @p queue_pair has submitted and not yet completed, which its credit bounds. */
    [[nodiscard]] std::size_t outstanding(queue_ref_t const& queue_pair) const noexcept {
        if (blocking_queue_t const* const blocking = queue_pair.template queue<blocking_queue_t>(); blocking)
            return blocking->outstanding();
        linux_aio_queue_t const* const queue = queue_of_(queue_pair);
        return queue ? outstanding_requests_(*queue) : 0;
    }

#pragma endregion Queues

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
            return registered_memory_t {disk_handle_t {static_cast<std::uint16_t>(index), slot.generation}, memory,
                                        space};
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every registered memory slot is taken"};
    }

    /**
     *  @brief Gives @p registered_memory back, refusing while the kernel may still write into it.
     *  @param[in,out] registered_memory The range to give back, surrendered on success.
     */
    status_t unregister_memory(registered_memory_t& registered_memory) noexcept {
        registered_memory_slot_t* const slot = slot_of_(registered_memory);
        if (!slot) return {status_code_t::invalid_argument_k, "The registered memory is not this disk's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "Requests in flight still use the range"};
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
            auto [file, open_status] = open_native_file(request.name, file_access_of(request.access), buffering_);
            if (open_status.failed()) {
                claim_(slot.registered).store(0, std::memory_order_release);
                return open_status;
            }
            if (request.access == medium_access_t::create_read_write_k && request.length != 0)
                if (status_t const sized = truncate_native_file(file, request.length); sized.failed()) {
                    [[maybe_unused]] status_t const closed = close_native_file(file);
                    claim_(slot.registered).store(0, std::memory_order_release);
                    return sized;
                }
            direct_alignment_t const direct = direct_alignment_of(file);
            direct_alignment_t const alignment = buffering_ == file_buffering_t::unbuffered_k
                                                     ? direct
                                                     : direct_alignment_t {1, 1};
            slot.file = file;
            slot.capacity = medium_bytes_of(file);
            slot.memory_alignment = alignment.memory;
            slot.offset_alignment = alignment.offset;
            return registered_medium_t {disk_handle_t {static_cast<std::uint16_t>(index), slot.generation},
                                        slot.capacity, direct.offset};
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every medium slot of this disk is taken"};
    }

    /**
     *  @brief Closes @p registered_medium, refusing while requests against it are still in flight.
     *  @param[in,out] registered_medium The medium to give back, surrendered on success.
     */
    status_t unregister_medium(registered_medium_t& registered_medium) noexcept {
        registered_medium_slot_t* const slot = slot_of_(registered_medium);
        if (!slot) return {status_code_t::invalid_argument_k, "The medium is not this disk's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "Requests in flight still use the medium"};
        [[maybe_unused]] status_t const closed = close_native_file(slot->file);
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
     *  @param[in] queue_pair The queue submitting, owned by the calling thread.
     *  @param[in] requests The batch, of which a prefix is taken.
     *  @return How many were taken, which is zero on a full queue and never a reason to block.
     *
     *  @note A request chained to its predecessor waits in its request_slot rather than in
     *      the kernel, and a request after a defect in the same chain is answered as
     *      cancelled right away.
     */
    expected<std::size_t> submit(queue_ref_t const& queue_pair, std::span<request_t const> requests) noexcept {
        for (request_t const& request : requests)
            if (request.token.queue_pair() != queue_pair.id())
                return status_t {status_code_t::invalid_argument_k, "A request holds another queue pair's token"};
        if (blocking_queue_t* const blocking = queue_pair.template queue<blocking_queue_t>(); blocking)
            return blocking->take(requests,
                                  [&](request_t const& request) noexcept { count_request_on_slots_(request); });
        linux_aio_queue_t* const queue = queue_of_(queue_pair);
        if (!queue) return status_t {status_code_t::invalid_argument_k, "The queue is of another transport"};

        std::size_t const credit = queue->depth_ - outstanding_requests_(*queue);
        std::size_t const taking = requests.size() < credit ? requests.size() : credit;
        std::uint32_t linked = linux_aio_queue_t::no_request_slot_k;
        bool chain_broken = false;
        for (std::size_t index = 0; index != taking; ++index) {
            request_t const& request = requests[index];
            if (chain_broken) {
                push_completion_made_by_queue_(*queue, cancelled_completion_(request.token));
                chain_broken = chains_next_(request);
                continue;
            }
            status_t const defect = inspect_(queue->pinned_staging_, request);
            if (defect.failed()) {
                push_completion_made_by_queue_(*queue, failed_completion_(request.token, defect));
                linked = linux_aio_queue_t::no_request_slot_k;
                chain_broken = chains_next_(request);
                continue;
            }
            if (request.kind == request_kind_t::deallocate_k) {
                bool const chained_into = linked != linux_aio_queue_t::no_request_slot_k;
                linked = linux_aio_queue_t::no_request_slot_k;
                status_t const deallocated = chained_into
                                                 ? status_t {status_code_t::invalid_argument_k,
                                                             "A deallocation runs at submission, outside chains"}
                                                 : deallocate_native_file(resolve_(request.medium)->file,
                                                                          request.extent);
                push_completion_made_by_queue_(*queue, completion_run_by_queue_(request.token, deallocated));
                chain_broken = chains_next_(request) && deallocated.failed();
                continue;
            }
            std::uint32_t const request_slot = take_free_request_slot_(*queue);
            submitted_request_t& submitted = queue->submitted_requests_[request_slot];
            submitted.request = request;
            build_(queue->pinned_staging_, submitted, request_slot);
            count_request_on_slots_(request);
            if (linked != linux_aio_queue_t::no_request_slot_k)
                queue->submitted_requests_[linked].successor = request_slot;
            else stage_(*queue, request_slot);
            linked = chains_next_(request) ? request_slot : linux_aio_queue_t::no_request_slot_k;
        }
        issue_(*queue);
        return taking;
    }

    /**
     *  @brief Copies what the kernel answered into @p into, and submits what that released.
     *  @param[in] queue_pair The queue polling, owned by the calling thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero when nothing has landed.
     */
    expected<std::size_t> poll_completions(queue_ref_t const& queue_pair, std::span<completion_t> into) noexcept {
        if (blocking_queue_t* const blocking = queue_pair.template queue<blocking_queue_t>(); blocking)
            return blocking->poll(
                into, [&](request_t const& request) noexcept { return perform_blocking_(*blocking, request); },
                [&](request_t const& request) noexcept { uncount_request_on_slots_(request); });
        linux_aio_queue_t* const queue = queue_of_(queue_pair);
        if (!queue) return status_t {status_code_t::invalid_argument_k, "The queue is of another transport"};
        return reap_(*queue, into, 0L);
    }

    /**
     *  @brief Moves at least one completion of @p queue_pair into @p into, sleeping in the kernel
     *      until one lands.
     *  @param[in] queue_pair The queue waiting, owned by the thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero only once nothing is outstanding.
     *
     *  @note The disk's own completions and whatever is still staged come first, so @c io_getevents
     *      parks the thread with a minimum of one only while the kernel holds every request.
     */
    expected<std::size_t> wait_completions(queue_ref_t const& queue_pair, std::span<completion_t> into) noexcept {
        if (into.empty()) return status_t {status_code_t::invalid_argument_k, "A wait needs room for a completion"};
        if (queue_pair.template queue<blocking_queue_t>()) return poll_completions(queue_pair, into);
        linux_aio_queue_t* const queue = queue_of_(queue_pair);
        if (!queue) return status_t {status_code_t::invalid_argument_k, "The queue is of another transport"};
        for (;;) {
            if (outstanding_requests_(*queue) == 0) return std::size_t {0};
            long const minimum = queue->completions_made_by_queue_count_ == 0 && queue->staged_count_ == 0 ? 1L : 0L;
            if (minimum != 0 && queue->outstanding_ == 0)
                return status_t {status_code_t::io_k, "The kernel never took the queued requests"};
            auto [moved, reap_status] = reap_(*queue, into, minimum);
            if (reap_status.failed()) return reap_status;
            if (moved != 0) return moved;
        }
    }

    /**
     *  @brief Answers everything @p queue_pair holds back from the kernel as cancelled.
     *  @param[in] queue_pair The queue to stop, whose accepted requests run to their end.
     *
     *  @note A regular file registers no cancel callback, so @c io_cancel could only fail; what
     *      stops are the requests still waiting on a predecessor and those a full kernel left
     *      staged.
     */
    void cancel(queue_ref_t const& queue_pair) noexcept {
        if (blocking_queue_t* const blocking = queue_pair.template queue<blocking_queue_t>(); blocking)
            return blocking->cancel();
        linux_aio_queue_t* const queue = queue_of_(queue_pair);
        if (!queue) return;
        std::uint32_t const staged = queue->staged_count_;
        queue->staged_count_ = 0;
        for (std::uint32_t index = 0; index != staged; ++index) {
            std::uint32_t const request_slot = static_cast<std::uint32_t>(queue->staged_[index]->aio_data);
            completion_t const cancelled = cancelled_completion_(
                queue->submitted_requests_[request_slot].request.token);
            push_completion_made_by_queue_(*queue, retire_(*queue, request_slot, cancelled));
        }
        for (std::uint32_t request_slot = 0; request_slot != queue->depth_; ++request_slot) {
            std::uint32_t const successor = queue->submitted_requests_[request_slot].successor;
            if (successor == linux_aio_queue_t::no_request_slot_k) continue;
            queue->submitted_requests_[request_slot].successor = linux_aio_queue_t::no_request_slot_k;
            cancel_linked_chain_(*queue, successor);
        }
    }

    /**
     *  @brief Finishes and discards everything @p queue_pair has outstanding, freeing its buffers.
     *  @param[in] queue_pair The queue to empty, which may be cancelled or still running.
     */
    status_t drain(queue_ref_t const& queue_pair) noexcept {
        if (blocking_queue_t* const blocking = queue_pair.template queue<blocking_queue_t>(); blocking) {
            blocking->drain([&](request_t const& request) noexcept { return perform_blocking_(*blocking, request); },
                            [&](request_t const& request) noexcept { uncount_request_on_slots_(request); });
            return {};
        }
        linux_aio_queue_t const* const queue = queue_of_(queue_pair);
        if (!queue) return {status_code_t::invalid_argument_k, "The queue is of another transport"};
        std::array<completion_t, drain_batch_k> discarded {};
        for (std::size_t spins = 0; spins != drain_spin_limit_k; ++spins) {
            if (outstanding_requests_(*queue) == 0) return {};
            auto [moved, poll_completions_status] = poll_completions(queue_pair, std::span<completion_t> {discarded});
            if (poll_completions_status.failed()) return poll_completions_status;
            if (moved != 0) continue;
            if (spins == drain_patience_k) cancel(queue_pair);
            waiter_type_ {}(&spins, spins, std::size_t {0});
        }
        return {status_code_t::io_k, "The queue would not drain"};
    }

#pragma endregion Ring

  private:

    using submitted_request_t = linux_aio_queue_t::submitted_request_t;

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

        /** The open file, which every control block against this medium holds. */
        native_file_t file = invalid_native_file_k;

        /** Bytes the medium held at registration: a regular file grows past it, a block device falls short. */
        std::uint64_t capacity = 0;

        /** Alignment this filesystem demands of the first byte of a buffer. */
        std::uint64_t memory_alignment = portable_alignment_k;

        /** Alignment this filesystem demands of an extent's offset and its length. */
        std::uint64_t offset_alignment = portable_alignment_k;
    };

    /** The largest extent one control block holds, kept well inside the kernel's own limit. */
    static constexpr std::uint64_t max_transfer_bytes_k = 1024ull * 1024ull * 1024ull;

    /** Completions one drain discards at a time. */
    static constexpr std::size_t drain_batch_k = 32;

    /** Fruitless polls a drain tolerates before it stops what is still held back. */
    static constexpr std::size_t drain_patience_k = 1024;

    /** Polls a drain performs before it calls the context broken. */
    static constexpr std::size_t drain_spin_limit_k = std::size_t {1} << 24;

    /** Grows @p elements to @p count default slots, reporting an allocation failure as a status. */
    template <typename element_type_>
    static status_t size_(st::vector<element_type_>& elements, std::size_t count) noexcept {
        if (elements.resize(count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A disk could not allocate its slots"};
        return {};
    }

    static atomic_reference_<std::int32_t> claim_(std::int32_t& word) noexcept {
        return atomic_reference_<std::int32_t> {word};
    }

    static atomic_reference_<std::int32_t> in_flight_(registered_memory_slot_t& slot) noexcept {
        return atomic_reference_<std::int32_t> {slot.in_flight};
    }

    static atomic_reference_<std::int32_t> in_flight_(registered_medium_slot_t& slot) noexcept {
        return atomic_reference_<std::int32_t> {slot.in_flight};
    }

    [[nodiscard]] static linux_aio_queue_t* queue_of_(queue_ref_t const& queue_pair) noexcept {
        return queue_pair.template queue<linux_aio_queue_t>();
    }

    /** Closes every open medium, which both the destructor and a move assignment need. */
    void close_all_() noexcept {
        for (std::size_t index = 0; index != media_.size(); ++index)
            if (media_[index].file != invalid_native_file_k) [[maybe_unused]]
                status_t const closed = close_native_file(media_[index].file);
        media_.clear();
    }

#pragma region Bookkeeping

    /** Copies what the kernel answered into @p into, waiting for @p minimum events, then submits
     *  what that released and appends the disk's own completions. */
    expected<std::size_t> reap_(linux_aio_queue_t& queue, std::span<completion_t> into, long minimum) noexcept {
        std::size_t moved = 0;
        if (queue.outstanding_ != 0 && !into.empty()) {
            std::size_t const room = into.size() < queue.outstanding_ ? into.size() : queue.outstanding_;
            long const landed = ::syscall(__NR_io_getevents, queue.context_, minimum, static_cast<long>(room),
                                          queue.events_.data(), nullptr);
            if (landed < 0 && errno != EINTR)
                return native_file_failure(errno, "The kernel would not hand completions back");
            for (long index = 0; index < landed; ++index) {
                ::io_event const& event = queue.events_[static_cast<std::size_t>(index)];
                std::uint32_t const request_slot = static_cast<std::uint32_t>(event.data);
                --queue.outstanding_;
                into[moved++] = retire_(queue, request_slot,
                                        translate_(queue.submitted_requests_[request_slot], event.res));
            }
        }
        issue_(queue);
        moved += drain_completions_made_by_queue_(queue, into.subspan(moved));
        return moved;
    }

    /** Requests taken and not yet handed back: every occupied request_slot and every
     *  unreaped local completion. */
    [[nodiscard]] static std::size_t outstanding_requests_(linux_aio_queue_t const& queue) noexcept {
        return (queue.depth_ - queue.free_request_slots_) + queue.completions_made_by_queue_count_;
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
        return claim_(slot.registered).load(std::memory_order_acquire) != 0 && slot.generation == place.generation
                   ? &slot
                   : nullptr;
    }

    /** The bytes @p place names: the submitting queue's own @p range for @c queue_range_k, else a
     *  recorded range, or nothing. */
    [[nodiscard]] std::span<byte_t> memory_of_(std::span<byte_t> range, memory_reference_t place) noexcept {
        if (place.memory_id == registered_memory_id_t::queue_range_k) return range;
        registered_memory_slot_t const* const slot = resolve_(place);
        return slot ? std::span<byte_t> {slot->base, static_cast<std::size_t>(slot->length)} : std::span<byte_t> {};
    }

    /** Marks the range and the medium of @p request as live, so neither can be taken back under it. */
    void count_request_on_slots_(request_t const& request) noexcept {
        if (registered_memory_slot_t* const slot = resolve_(request.buffer); slot)
            in_flight_(*slot).store_add(1, std::memory_order_acquire);
        if (registered_medium_slot_t* const slot = resolve_(request.medium); slot)
            in_flight_(*slot).store_add(1, std::memory_order_acquire);
    }

    /** Releases both once the kernel is done with the bytes of @p request. */
    void uncount_request_on_slots_(request_t const& request) noexcept {
        if (registered_memory_slot_t* const slot = resolve_(request.buffer); slot)
            in_flight_(*slot).store_sub(1, std::memory_order_release);
        if (registered_medium_slot_t* const slot = resolve_(request.medium); slot)
            in_flight_(*slot).store_sub(1, std::memory_order_release);
    }

    /** Pops a free request_slot of @p queue, which its credit guarantees there is. */
    static std::uint32_t take_free_request_slot_(linux_aio_queue_t& queue) noexcept {
        return queue.request_slots_[--queue.free_request_slots_];
    }

    /** Pushes @p request_slot back onto @p queue's free stack, forgetting any request
     *  it held back. */
    static void free_request_slot_(linux_aio_queue_t& queue, std::uint32_t request_slot) noexcept {
        queue.submitted_requests_[request_slot].successor = linux_aio_queue_t::no_request_slot_k;
        queue.request_slots_[queue.free_request_slots_++] = request_slot;
    }

    /** Queues the control block of @p request_slot for the next @c io_submit, behind everything
     *  staged before it. */
    static void stage_(linux_aio_queue_t& queue, std::uint32_t request_slot) noexcept {
        queue.staged_[queue.staged_count_++] = &queue.submitted_requests_[request_slot].control;
    }

    /** Places @p completion at the back of @p queue's own completions, which poll drains after the
     *  kernel's. */
    static void push_completion_made_by_queue_(linux_aio_queue_t& queue, completion_t const& completion) noexcept {
        queue.completions_made_by_queue_[(queue.completions_made_by_queue_first_ +
                                          queue.completions_made_by_queue_count_) %
                                         queue.depth_] = completion;
        ++queue.completions_made_by_queue_count_;
    }

    /** Moves what the disk answered itself into @p into, oldest first. */
    static std::size_t drain_completions_made_by_queue_(linux_aio_queue_t& queue,
                                                        std::span<completion_t> into) noexcept {
        std::size_t const moving = into.size() < queue.completions_made_by_queue_count_
                                       ? into.size()
                                       : queue.completions_made_by_queue_count_;
        for (std::size_t index = 0; index != moving; ++index)
            into[index] =
                queue.completions_made_by_queue_[(queue.completions_made_by_queue_first_ + index) % queue.depth_];
        queue.completions_made_by_queue_first_ = static_cast<std::uint32_t>(
            (queue.completions_made_by_queue_first_ + moving) % queue.depth_);
        queue.completions_made_by_queue_count_ -= static_cast<std::uint32_t>(moving);
        return moving;
    }

    /**
     *  @brief Frees @p request_slot, whose request ended as @p completion, and
     *      settles its successor.
     *  @return The completion itself, for the caller to hand back or queue locally.
     */
    completion_t retire_(linux_aio_queue_t& queue, std::uint32_t request_slot,
                         completion_t const& completion) noexcept {
        submitted_request_t const& submitted = queue.submitted_requests_[request_slot];
        std::uint32_t const successor = submitted.successor;
        uncount_request_on_slots_(submitted.request);
        free_request_slot_(queue, request_slot);
        if (successor == linux_aio_queue_t::no_request_slot_k) return completion;
        if (completion.completed_in_full()) stage_(queue, successor);
        else cancel_linked_chain_(queue, successor);
        return completion;
    }

    /** Answers the request in @p request_slot and every request held behind it as cancelled. */
    void cancel_linked_chain_(linux_aio_queue_t& queue, std::uint32_t request_slot) noexcept {
        while (request_slot != linux_aio_queue_t::no_request_slot_k) {
            submitted_request_t const& submitted = queue.submitted_requests_[request_slot];
            std::uint32_t const successor = submitted.successor;
            push_completion_made_by_queue_(queue, cancelled_completion_(submitted.request.token));
            uncount_request_on_slots_(submitted.request);
            free_request_slot_(queue, request_slot);
            request_slot = successor;
        }
    }

#pragma endregion Bookkeeping

#pragma region Requests

    /** The completion a request gets when its predecessor fell short, or its queue cancelled it. */
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

    /** The completion of a request the disk ran itself, failed for @p outcome's reason if any. */
    [[nodiscard]] static completion_t completion_run_by_queue_(request_token_t token, status_t outcome) noexcept {
        if (outcome.failed()) return failed_completion_(token, outcome);
        completion_t completion;
        completion.token = token;
        return completion;
    }

    /** Whether the request that follows @p request in its batch waits for this one to transfer in full. */
    [[nodiscard]] static bool chains_next_(request_t const& request) noexcept {
        return request.chaining == request_chaining_t::chained_k;
    }

    /** Whatever would corrupt a control block rather than merely fail on the device, @p range being
     *  the submitting queue's own blocks. */
    status_t inspect_(std::span<byte_t> range, request_t const& request) noexcept {
        registered_medium_slot_t const* const medium = resolve_(request.medium);
        if (!medium) return {status_code_t::invalid_argument_k, "The request names no registered medium"};
        if (request.kind == request_kind_t::flush_k || request.kind == request_kind_t::deallocate_k) return {};
        std::span<byte_t> const memory = memory_of_(range, request.buffer);
        if (memory.empty()) return {status_code_t::invalid_argument_k, "The request names no registered range"};
        if (request.buffer.offset > memory.size() || request.extent.length > memory.size() - request.buffer.offset)
            return {status_code_t::invalid_argument_k, "The request runs past its registered range"};
        if (request.extent.length > max_transfer_bytes_k)
            return {status_code_t::invalid_argument_k, "The request is larger than one transfer"};
        byte_t const* const address = memory.data() + request.buffer.offset;
        if (!is_address_aligned(address, medium->memory_alignment) ||
            !request.extent.aligned_to(medium->offset_alignment))
            return {status_code_t::invalid_argument_k, "The request does not meet the disk's alignment"};
        return {};
    }

    /** Writes the control block of the request in @p submitted, which its event names by
     *  @p request_slot, @p range being the submitting queue's own blocks. */
    void build_(std::span<byte_t> range, submitted_request_t& submitted, std::uint32_t request_slot) noexcept {
        request_t const& request = submitted.request;
        ::iocb& control = submitted.control;
        control = ::iocb {};
        control.aio_data = request_slot;
        control.aio_fildes = static_cast<std::uint32_t>(resolve_(request.medium)->file);
        if (request.kind == request_kind_t::flush_k) {
            control.aio_lio_opcode = static_cast<std::uint16_t>(IOCB_CMD_FDSYNC);
            return;
        }
        std::span<byte_t> const memory = memory_of_(range, request.buffer);
        control.aio_lio_opcode = static_cast<std::uint16_t>(request.kind == request_kind_t::read_k ? IOCB_CMD_PREAD
                                                                                                   : IOCB_CMD_PWRITE);
        control.aio_buf = reinterpret_cast<std::uint64_t>(memory.data() + request.buffer.offset);
        control.aio_nbytes = request.extent.length;
        control.aio_offset = static_cast<std::int64_t>(request.extent.offset);
    }

    /** Performs @p request of a blocking queue with a positioned transfer on the polling thread. */
    [[nodiscard]] completion_t perform_blocking_(blocking_queue_t const& queue, request_t const& request) noexcept {
        if (status_t const defect = inspect_(queue.range(), request); defect.failed())
            return failed_completion_(request.token, defect);
        native_file_t const file = resolve_(request.medium)->file;
        if (request.kind == request_kind_t::flush_k)
            return completion_run_by_queue_(request.token, flush_native_file(file));
        if (request.kind == request_kind_t::deallocate_k)
            return completion_run_by_queue_(request.token, deallocate_native_file(file, request.extent));
        std::span<byte_t> const buffer = memory_of_(queue.range(), request.buffer)
                                             .subspan(static_cast<std::size_t>(request.buffer.offset),
                                                      static_cast<std::size_t>(request.extent.length));
        auto [moved, move_status] = request.kind == request_kind_t::read_k
                                        ? read_native_file(file, request.extent.offset, buffer, queue.event())
                                        : write_native_file(file, request.extent.offset, buffer, queue.event());
        if (move_status.failed()) return failed_completion_(request.token, move_status);
        completion_t completion;
        completion.token = request.token;
        completion.transferred_bytes = moved;
        completion.state = moved == request.extent.length ? completion_state_t::completed_k
                                                          : completion_state_t::partial_k;
        return completion;
    }

    /**
     *  @brief Hands the staged control blocks of @p queue to the kernel in order, until it has all
     *      or no room.
     *
     *  @c io_submit stops at the first control block it cannot start and answers how many it
     *  took, or the error of that one when it took none. A full kernel leaves the rest staged
     *  for the next poll; any other refusal belongs to that one request, so it fails alone and
     *  the batch behind it goes on. A flush a kernel older than 4.18 refuses with @c EINVAL runs
     *  as @c fdatasync instead.
     */
    void issue_(linux_aio_queue_t& queue) noexcept {
        ::iocb** const staged = queue.staged_.data();
        while (queue.staged_count_ != 0) {
            long const accepted = ::syscall(__NR_io_submit, queue.context_, static_cast<long>(queue.staged_count_),
                                            staged);
            if (accepted > 0) {
                std::uint32_t const taken = static_cast<std::uint32_t>(accepted);
                queue.outstanding_ += taken;
                queue.staged_count_ -= taken;
                for (std::uint32_t index = 0; index != queue.staged_count_; ++index)
                    staged[index] = staged[taken + index];
                continue;
            }
            int const reason = accepted < 0 ? errno : EAGAIN;
            if (reason == EINTR) continue;
            if (reason == EAGAIN) return;
            std::uint32_t const request_slot = static_cast<std::uint32_t>(staged[0]->aio_data);
            --queue.staged_count_;
            for (std::uint32_t index = 0; index != queue.staged_count_; ++index) staged[index] = staged[index + 1];
            push_completion_made_by_queue_(
                queue,
                retire_(queue, request_slot, refused_completion_(queue.submitted_requests_[request_slot], reason)));
        }
    }

    /** The completion of a control block @c io_submit refused on its own, for the @p reason it gave. */
    [[nodiscard]] static completion_t refused_completion_(submitted_request_t const& submitted, int reason) noexcept {
        if (submitted.request.kind != request_kind_t::flush_k || reason != EINVAL)
            return failed_completion_(submitted.request.token,
                                      native_file_failure(reason, "The kernel refused this request"));
        status_t const flushed = flush_native_file(static_cast<native_file_t>(submitted.control.aio_fildes));
        return flushed.failed() ? failed_completion_(submitted.request.token, flushed) : translate_(submitted, 0);
    }

    /** Turns one event into the completion its queue reads, by what the kernel returned. */
    [[nodiscard]] static completion_t translate_(submitted_request_t const& submitted, std::int64_t result) noexcept {
        completion_t completion;
        completion.token = submitted.request.token;
        if (result >= 0) {
            std::uint64_t const length = submitted.request.kind == request_kind_t::flush_k
                                             ? 0
                                             : submitted.request.extent.length;
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

    /** One slot per memory range the disk records. */
    st::vector<registered_memory_slot_t> registered_memories_;

    /** One slot per medium the disk opens. */
    st::vector<registered_medium_slot_t> media_;

    /** Whether the page cache stands in the path, which fixes what a medium with no alignment reports. */
    file_buffering_t buffering_ = file_buffering_t::unbuffered_k;
};

#pragma endregion Disk

#else

#pragma region Absent

/** The queue on a platform without Linux AIO, which a queue set never makes. */
class linux_aio_queue_t {
  public:

    /** Refuses the queue, since this platform has no context to set up. */
    static expected<linux_aio_queue_t> make(queue_depth_t, std::span<byte_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Requests the queue keeps in flight, which is none. */
    [[nodiscard]] queue_depth_t depth() const noexcept { return 0; }
};

/**
 *  @brief The same disk on a platform without Linux AIO, which mounts nowhere and refuses cleanly.
 *
 *  The type exists so a caller compiles against it unconditionally and chooses at mount, where a
 *  refused @c make sends it to the blocking floor rather than to a build-time branch.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_aio_files_disk {
  public:

    /** The queue a thread owns for this transport. */
    using queue_t = linux_aio_queue_t;

    /** Answers that this platform has no AIO to offer. */
    [[nodiscard]] static linux_aio_probe_t probe() noexcept { return linux_aio_probe_t::refused_k; }

    /** Refuses the mount, since this platform has no context to mount on. */
    static expected<linux_aio_files_disk> make(linux_aio_disk_config_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** What this disk would accept, which is nothing at all. */
    [[nodiscard]] disk_capabilities_t capabilities() const noexcept { return {}; }

    /** Refuses the registration, since no queue of this transport was ever made. */
    status_t register_media(queue_ref_t const&) noexcept {
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
    expected<std::size_t> submit(queue_ref_t const&, std::span<request_t const>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Answers nothing, since nothing was ever submitted. */
    expected<std::size_t> poll_completions(queue_ref_t const&, std::span<completion_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Waits for nothing, since nothing was ever submitted. */
    expected<std::size_t> wait_completions(queue_ref_t const&, std::span<completion_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Stops nothing, since nothing was ever submitted. */
    void cancel(queue_ref_t const&) noexcept {}

    /** Refuses the drain, since no queue of this transport was ever made. */
    status_t drain(queue_ref_t const&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no Linux AIO"};
    }

    /** Answers that nothing is outstanding, which is always true here. */
    [[nodiscard]] std::size_t outstanding(queue_ref_t const&) const noexcept { return 0; }
};

#pragma endregion Absent

#endif

} // namespace unum::ustore::utilities
