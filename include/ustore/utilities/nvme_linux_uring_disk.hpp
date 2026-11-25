/**
 *  @file include/ustore/utilities/nvme_linux_uring_disk.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The ring Linux offers: @c O_DIRECT files driven through raw @c io_uring syscalls.
 *
 *  Nothing here links liburing. A ring is set up with @c io_uring_setup, mapped with @c mmap, fed
 *  with @c io_uring_register and entered with @c io_uring_enter, which is the whole interface and
 *  needs Linux 6.12 or newer, the floor the other transports keep too.
 *
 *  A ring belongs to a queue of one thread rather than to the disk, so no ring is shared between
 *  threads entering it at once, and every queue set reads every disk through rings of its own. The
 *  ring pins its thread's blocks once, when the queue set is made; the locked-memory cap may refuse
 *  that pin, and a refused ring addresses the blocks by pointer.
 *
 *  A medium is recorded on the disk, which counts a medium epoch, and a call registers the disk's
 *  media on its ring before submitting: the ring rewrites its file table only when it last
 *  registered another disk or an older epoch, so a warm call makes no registration at all.
 */

#pragma once

#include <cassert> // `assert`
#include <cerrno>  // `errno`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memset`

#include "ustore/types.h" // `USTORE_OS_LINUX_`

#if USTORE_OS_LINUX_
#include <sys/mman.h>    // `mmap`, `munmap`, `MAP_SHARED`
#include <sys/syscall.h> // `SYS_gettid`
#include <sys/uio.h>     // `iovec`
#include <unistd.h>      // `syscall`, `close`
#endif

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/nvme_disks.hpp" // `file_access_of`, `medium_bytes_of`, `direct_alignment_of`, `deallocate_native_file`
#include "ustore/utilities/nvme_ring.hpp" // `request_t`, `completion_t`, `queue_ref_t`, `portable_alignment_k`, `is_address_aligned`
#include "ustore/utilities/types.hpp" // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::utilities {

#pragma region Configuration

/** What a mount fixes about an io_uring disk before any ring holds it. */
struct linux_uring_disk_config_t {

    /** Ranges the disk records at once, which the rings reach by pointer. */
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

    /** The kernel has no io_uring, or none that holds the flags this transport needs. */
    too_old_k,
};

/** Whether the kernel holds a ring's blocks pinned, which the locked-memory cap decides when the
 *  ring is made. */
enum class staging_pinning_t : std::uint8_t {

    /** Pinned once when the ring was made, so a request names a table slot and the kernel pins
     *  nothing per request. */
    pinned_k,

    /** The pin exceeded the locked-memory cap, so a request holds its pointer and the kernel
     *  pins per request. */
    unpinned_k,
};

/** The largest buffer io_uring pins in one table slot, refusing a longer one with @c EFAULT. */
inline constexpr std::uint64_t linux_uring_registered_range_bytes_k = 1024ull * 1024ull * 1024ull;

#pragma endregion Configuration

#if USTORE_OS_LINUX_

#pragma region Kernel

/** @c IORING_SETUP_*: the io_uring setup flags every ring of this transport takes, with
 *  @c R_DISABLED from Linux 5.10, @c COOP_TASKRUN and @c TASKRUN_FLAG from Linux 5.19, and
 *  @c NO_SQARRAY from Linux 6.6. */
enum linux_uring_setup_t : std::uint32_t {
    linux_uring_setup_r_disabled_k = 1u << 6,
    linux_uring_setup_coop_taskrun_k = 1u << 8,
    linux_uring_setup_taskrun_flag_k = 1u << 9,
    linux_uring_setup_no_sqarray_k = 1u << 16,
};

/** @c IOSQE_*: per-entry flags. */
enum linux_uring_entry_flag_t : std::uint8_t {
    linux_uring_entry_io_link_k = 1u << 2,
};

/** @c io_uring_sqe, the 64-byte submission entry, with the members this transport writes. */
struct linux_uring_sqe_t {
    std::uint8_t opcode;
    std::uint8_t entry_flags;
    std::uint16_t ioprio;
    std::int32_t fd;
    std::uint64_t off;
    std::uint64_t addr;
    std::uint32_t len;
    union {
        std::uint32_t fsync_flags;
        std::uint32_t cancel_flags;
    };
    std::uint64_t user_data;
    std::uint16_t buf_index;
    std::uint16_t personality;
    std::int32_t file_index;
    std::uint64_t addr3;
    std::uint64_t padding;
};
static_assert(sizeof(linux_uring_sqe_t) == 64, "the kernel reads 64-byte submission entries");

/** @c io_uring_cqe, the 16-byte completion entry. */
struct linux_uring_cqe_t {
    std::uint64_t user_data;
    std::int32_t res;
    std::uint32_t completion_flags;
};
static_assert(sizeof(linux_uring_cqe_t) == 16, "the kernel writes 16-byte completion entries");

/** @c io_sqring_offsets, where the submission ring's words sit in its mapping. */
struct linux_uring_submission_offsets_t {
    std::uint32_t head_offset;
    std::uint32_t tail_offset;
    std::uint32_t ring_mask_offset;
    std::uint32_t ring_entries_offset;
    std::uint32_t flags_offset;
    std::uint32_t dropped_offset;
    std::uint32_t array_offset;
    std::uint32_t reserved;
    std::uint64_t reserved2;
};
static_assert(sizeof(linux_uring_submission_offsets_t) == 40, "the kernel fills 40 bytes of submission offsets");

/** @c io_cqring_offsets, where the completion ring's words sit in its mapping. */
struct linux_uring_completion_offsets_t {
    std::uint32_t head_offset;
    std::uint32_t tail_offset;
    std::uint32_t ring_mask_offset;
    std::uint32_t ring_entries_offset;
    std::uint32_t overflow_offset;
    std::uint32_t cqes_offset;
    std::uint32_t flags_offset;
    std::uint32_t reserved;
    std::uint64_t reserved2;
};
static_assert(sizeof(linux_uring_completion_offsets_t) == 40, "the kernel fills 40 bytes of completion offsets");

/** @c io_uring_params, what @c io_uring_setup takes and fills. */
struct linux_uring_params_t {
    std::uint32_t sq_entries, cq_entries, setup_flags, sq_thread_cpu, sq_thread_idle, features, wq_fd;
    std::uint32_t resv[3];
    linux_uring_submission_offsets_t sq_off;
    linux_uring_completion_offsets_t cq_off;
};
static_assert(sizeof(linux_uring_params_t) == 120, "the kernel takes 120 bytes of ring parameters");

/** @c io_uring_rsrc_register, a sparse table of buffers or files. */
struct linux_uring_rsrc_register_t {
    std::uint32_t nr, flags;
    std::uint64_t resv2, data, tags;
};
static_assert(sizeof(linux_uring_rsrc_register_t) == 32, "the kernel takes 32-byte table registrations");

/** @c io_uring_rsrc_update, a run of files written into a table. */
struct linux_uring_rsrc_update_t {
    std::uint32_t offset, resv;
    std::uint64_t data;
};
static_assert(sizeof(linux_uring_rsrc_update_t) == 16, "the kernel takes 16-byte table updates");

/** @c io_uring_rsrc_update2, a run of buffers or files written into a table. */
struct linux_uring_rsrc_update2_t {
    std::uint32_t offset, resv;
    std::uint64_t data, tags;
    std::uint32_t nr, resv2;
};
static_assert(sizeof(linux_uring_rsrc_update2_t) == 32, "the kernel takes 32-byte table updates");

#pragma endregion Kernel

#pragma region Queue

template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_uring_files_disk;

/** Which media list a queue's registered-file table holds: the disk's media identity, 64 random
 *  bits drawn when the disk was made, and the disk's media epoch under it; zeros for none. */
struct media_list_key_t {
    std::uint64_t identity = 0;
    epoch_t epoch = 0;
    friend bool operator==(media_list_key_t const&, media_list_key_t const&) noexcept = default;
};

static_assert(sizeof(media_list_key_t) == 16, "A media list key is two words");

/**
 *  @brief One io_uring instance a thread owns: its rings, its credit, the requests the kernel
 *      holds for it, and the file table it registers a disk's media in.
 *
 *  The buffer table pins the thread's blocks once, at creation, one slot per gibibyte the kernel
 *  pins at most; a locked-memory cap that refuses the pin leaves the blocks addressed by pointer.
 *  The file table holds the media of the disk the queue registered last, rewritten only when a call
 *  holds another disk or that disk's media moved, so a warm call registers nothing. An idle queue
 *  keeps its last disk's files referenced until its next registration.
 *
 *  No @c SINGLE_ISSUER and so no @c DEFER_TASKRUN: the queue set is made on the thread creating the
 *  executor and submitted on by the pool thread owning the queue. The kernel raises a flag when
 *  completions wait on task work, so polling enters only then.
 */
class linux_uring_queue_t {
  public:

    linux_uring_queue_t() noexcept = default;
    linux_uring_queue_t(linux_uring_queue_t const&) = delete;
    linux_uring_queue_t& operator=(linux_uring_queue_t const&) = delete;

    linux_uring_queue_t(linux_uring_queue_t&& other) noexcept
        : ring_(std::exchange(other.ring_, ring_t {})), submitted_requests_(std::move(other.submitted_requests_)),
          completions_made_by_queue_(std::move(other.completions_made_by_queue_)), files_(std::move(other.files_)),
          pinned_staging_(other.pinned_staging_),
          media_list_key_(std::exchange(other.media_list_key_, media_list_key_t {})), depth_(other.depth_),
          outstanding_(other.outstanding_), completions_made_by_queue_first_(other.completions_made_by_queue_first_),
          completions_made_by_queue_count_(other.completions_made_by_queue_count_), pinning_(other.pinning_),
          requests_across_registered_slots_(std::exchange(other.requests_across_registered_slots_, 0)) {}

    linux_uring_queue_t& operator=(linux_uring_queue_t&& other) noexcept {
        if (this == &other) return *this;
        teardown_(ring_);
        ring_ = std::exchange(other.ring_, ring_t {});
        submitted_requests_ = std::move(other.submitted_requests_);
        completions_made_by_queue_ = std::move(other.completions_made_by_queue_);
        files_ = std::move(other.files_);
        pinned_staging_ = other.pinned_staging_;
        media_list_key_ = std::exchange(other.media_list_key_, media_list_key_t {});
        depth_ = other.depth_;
        outstanding_ = other.outstanding_;
        completions_made_by_queue_first_ = other.completions_made_by_queue_first_;
        completions_made_by_queue_count_ = other.completions_made_by_queue_count_;
        pinning_ = other.pinning_;
        requests_across_registered_slots_ = std::exchange(other.requests_across_registered_slots_, 0);
        return *this;
    }

    ~linux_uring_queue_t() noexcept { teardown_(ring_); }

    /**
     *  @brief Sets one ring up with room for @p depth requests and pins @p range into
     *      its buffer table.
     *  @param[in] depth Requests the queue keeps in flight at once.
     *  @param[in] range The thread's blocks, which requests naming @c queue_range_k
     *      address, or empty.
     */
    static expected<linux_uring_queue_t> make(queue_depth_t depth, std::span<byte_t> range) noexcept {
        if (depth == 0 || depth > depth_ceiling_k)
            return status_t {status_code_t::invalid_argument_k, "A ring holds between one and 16384 requests"};
        linux_uring_queue_t queue;
        if (queue.submitted_requests_.resize(depth) != st::status_t::success_k ||
            queue.completions_made_by_queue_.resize(depth) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A ring could not size its windows"};
        if (status_t const created = create_(queue.ring_, depth); created.failed()) return created;
        queue.depth_ = depth;
        queue.pinned_staging_ = range;
        queue.pinning_ = range.empty() ? staging_pinning_t::unpinned_k : pin_(queue.ring_, range);
        return queue;
    }

    /** Requests the queue keeps in flight at once. */
    [[nodiscard]] queue_depth_t depth() const noexcept { return depth_; }

    /** How the ring addresses the thread's blocks, settled when it was made. */
    [[nodiscard]] staging_pinning_t pinning() const noexcept { return pinning_; }

    /** Which media list the file table holds, which a warm call leaves as it was. */
    [[nodiscard]] media_list_key_t media_list_key() const noexcept { return media_list_key_; }

    /** Requests sent unpinned while this queue's blocks were pinned, as each crossed a gibibyte
     *  that no one table slot names; only the thread owning the queue counts them. */
    [[nodiscard]] std::uint64_t requests_across_registered_slots() const noexcept {
        return requests_across_registered_slots_;
    }

  private:

    template <template <typename> typename atomic_reference_, typename waiter_type_>
    friend class linux_uring_files_disk;

    /** The shared submission and completion rings of one io_uring instance. */
    struct ring_t {

        /** The ring's descriptor, @c -1 before it is created and after it is taken down. */
        int file = -1;

        /** The shared submission and completion rings, mapped as one region. */
        void* shared = nullptr;

        /** Bytes the shared mapping spans, which only the unmapping needs. */
        std::size_t shared_bytes = 0;

        /** The submission entries, mapped apart from the rings that index them. */
        linux_uring_sqe_t* submission_entries = nullptr;

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
        linux_uring_cqe_t* completion_entries = nullptr;
    };

    /** What one outstanding request left behind, since a completion holds only its
     *  token and result. */
    struct submitted_request_t {

        /** The token the submission entry held, which the kernel hands back verbatim. */
        request_token_t token;

        /** Bytes the extent asked for, which separates a whole transfer from a partial one. */
        std::uint64_t length = 0;

        /** The range the request viewed, given back when its completion is completed. */
        memory_reference_t buffer;

        /** The medium the request viewed, given back when its completion is completed. */
        medium_reference_t medium;
    };

    /** Entries the availability probe asks for, which is the smallest ring the kernel
     *  rounds up to. */
    static constexpr unsigned probe_entries_k = 4;

    /** Completion seats per request of depth, so a burst of completions rarely
     *  overflows the queue. */
    static constexpr unsigned completion_slack_k = 4;

    /** Completion seats the kernel allows at most, which bounds the depth a queue set
     *  may ask for. */
    static constexpr queue_depth_t depth_ceiling_k = 16384;

    /** The flags every ring of this transport is set up with, and the probe tests for. */
    static constexpr unsigned ring_setup_flags_k = linux_uring_setup_no_sqarray_k | linux_uring_setup_coop_taskrun_k |
                                                   linux_uring_setup_taskrun_flag_k;

    /** Sets a ring of @p entries up as @p parameters ask, answering its descriptor or a negative
     *  failure. */
    static long setup_(unsigned entries, linux_uring_params_t& parameters) noexcept {
        long const setup_syscall = 425 + linux_syscall_offset_k; // __NR_io_uring_setup
        return ::syscall(setup_syscall, entries, &parameters);
    }

    /** Submits @p to_submit entries of @p ring and waits for @p min_complete completions as @p
     *  flags say. */
    static long enter_(ring_t const& ring, unsigned to_submit, unsigned min_complete,
                       std::uint32_t flags = 0u) noexcept {
        long const enter_syscall = 426 + linux_syscall_offset_k; // __NR_io_uring_enter
        return ::syscall(enter_syscall, ring.file, to_submit, min_complete, flags, nullptr, 0);
    }

    /** Issues the registration @p opcode on @p ring with @p count of @p arguments. */
    static long register_(ring_t const& ring, std::uint32_t opcode, void const* arguments, unsigned count) noexcept {
        long const register_syscall = 427 + linux_syscall_offset_k; // __NR_io_uring_register
        return ::syscall(register_syscall, ring.file, opcode, arguments, count);
    }

    /** Sets one ring of @p depth up and maps it. */
    static status_t create_(ring_t& ring, queue_depth_t depth) noexcept {
        std::uint32_t const cqsize = 1u << 3;                 // IORING_SETUP_CQSIZE
        std::uint32_t const single_mmap = 1u << 0;            // IORING_FEAT_SINGLE_MMAP
        std::uint64_t const submission_ring = 0;              // IORING_OFF_SQ_RING
        std::uint64_t const submission_entries = 0x10000000u; // IORING_OFF_SQES
        linux_uring_params_t parameters {};
        parameters.setup_flags = ring_setup_flags_k | cqsize;
        std::uint64_t const wanted = static_cast<std::uint64_t>(depth) * completion_slack_k;
        parameters.cq_entries = static_cast<unsigned>(wanted < 65536u ? wanted : 65536u);
        long const opened = setup_(depth, parameters);
        if (opened < 0) return native_file_failure(errno, "The kernel refused an io_uring ring");
        ring.file = static_cast<int>(opened);
        if ((parameters.features & single_mmap) == 0) {
            teardown_(ring);
            return {status_code_t::unsupported_k, "This kernel maps the two rings apart"};
        }

        std::size_t const submission_bytes = parameters.sq_off.array_offset + parameters.sq_entries * sizeof(unsigned);
        std::size_t const completion_bytes = parameters.cq_off.cqes_offset +
                                             parameters.cq_entries * sizeof(linux_uring_cqe_t);
        ring.shared_bytes = submission_bytes > completion_bytes ? submission_bytes : completion_bytes;
        ring.shared = ::mmap(nullptr, ring.shared_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring.file,
                             submission_ring);
        if (ring.shared == MAP_FAILED) {
            ring.shared = nullptr;
            teardown_(ring);
            return {status_code_t::out_of_memory_k, "The ring would not map"};
        }
        ring.submission_entries_bytes = parameters.sq_entries * sizeof(linux_uring_sqe_t);
        void* const entries = ::mmap(nullptr, ring.submission_entries_bytes, PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_POPULATE, ring.file, submission_entries);
        if (entries == MAP_FAILED) {
            teardown_(ring);
            return {status_code_t::out_of_memory_k, "The submission entries would not map"};
        }
        ring.submission_entries = static_cast<linux_uring_sqe_t*>(entries);

        byte_t* const shared = static_cast<byte_t*>(ring.shared);
        ring.submission_head = reinterpret_cast<unsigned*>(shared + parameters.sq_off.head_offset);
        ring.submission_tail = reinterpret_cast<unsigned*>(shared + parameters.sq_off.tail_offset);
        ring.submission_flags = reinterpret_cast<unsigned*>(shared + parameters.sq_off.flags_offset);
        ring.submission_mask = *reinterpret_cast<unsigned*>(shared + parameters.sq_off.ring_mask_offset);
        ring.submission_entries_count = parameters.sq_entries;
        ring.completion_head = reinterpret_cast<unsigned*>(shared + parameters.cq_off.head_offset);
        ring.completion_tail = reinterpret_cast<unsigned*>(shared + parameters.cq_off.tail_offset);
        ring.completion_mask = *reinterpret_cast<unsigned*>(shared + parameters.cq_off.ring_mask_offset);
        ring.completion_entries = reinterpret_cast<linux_uring_cqe_t*>(shared + parameters.cq_off.cqes_offset);
        return {};
    }

    /** Pins @p range into @p ring's buffer table as one slot per gibibyte, or answers that the cap
     *  refused it. */
    static staging_pinning_t pin_(ring_t const& ring, std::span<byte_t> range) noexcept {
        st::vector<::iovec> vectors;
        std::size_t const pieces = divide_round_up(range.size(), linux_uring_registered_range_bytes_k);
        if (vectors.resize(pieces) != st::status_t::success_k) return staging_pinning_t::unpinned_k;
        for (std::size_t piece = 0; piece != pieces; ++piece) {
            std::size_t const offset = piece * linux_uring_registered_range_bytes_k;
            std::size_t const left = range.size() - offset;
            vectors[piece].iov_base = range.data() + offset;
            vectors[piece].iov_len = left < linux_uring_registered_range_bytes_k
                                         ? left
                                         : static_cast<std::size_t>(linux_uring_registered_range_bytes_k);
        }
        std::uint32_t const register_buffers = 0; // IORING_REGISTER_BUFFERS
        return register_(ring, register_buffers, vectors.data(), static_cast<unsigned>(pieces)) >= 0
                   ? staging_pinning_t::pinned_k
                   : staging_pinning_t::unpinned_k;
    }

    /** Widens the file table to @p width sparse slots when it is narrower, which costs two
     *  registrations once. */
    status_t reserve_files_(std::size_t width) noexcept {
        if (files_.size() >= width) return {};
        std::uint32_t const unregister_files = 3;      // IORING_UNREGISTER_FILES
        std::uint32_t const files2 = 13;               // IORING_REGISTER_FILES2, from Linux 5.13
        std::uint32_t const register_sparse = 1u << 0; // IORING_RSRC_REGISTER_SPARSE, from Linux 5.19
        if (!files_.empty()) [[maybe_unused]]
            long const unregistered = register_(ring_, unregister_files, nullptr, 0);
        media_list_key_ = media_list_key_t {};
        if (files_.resize(width, invalid_native_file_k) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A ring could not widen its file table"};
        linux_uring_rsrc_register_t table {};
        table.flags = register_sparse;
        table.nr = static_cast<std::uint32_t>(width);
        if (register_(ring_, files2, &table, sizeof(table)) < 0) {
            files_.clear();
            return native_file_failure(errno, "The ring would not take a file table");
        }
        return {};
    }

    /** Unmaps and closes @p ring, giving its pins and files back at once. */
    static void teardown_(ring_t& ring) noexcept {
        if (ring.submission_entries) ::munmap(ring.submission_entries, ring.submission_entries_bytes);
        if (ring.shared) ::munmap(ring.shared, ring.shared_bytes);
        if (ring.file >= 0) ::close(ring.file);
        ring = ring_t {};
    }

    /** The ring itself. */
    ring_t ring_;

    /** The requests the kernel holds for this queue. */
    st::vector<submitted_request_t> submitted_requests_;

    /** The completions the disk answered itself, read as a circle. */
    st::vector<completion_t> completions_made_by_queue_;

    /** The file table as last written, one descriptor per slot of the disk registered last. */
    st::vector<int> files_;

    /** The thread's blocks, which a request naming @c queue_range_k addresses. */
    std::span<byte_t> pinned_staging_;

    /** Which media list the file table holds. */
    media_list_key_t media_list_key_;

    /** Requests the queue keeps in flight at once. */
    queue_depth_t depth_ = 0;

    /** Requests the kernel holds and has not yet answered for. */
    std::uint32_t outstanding_ = 0;

    /** Where the oldest unreaped local completion sits inside @c completions_made_by_queue_. */
    std::uint32_t completions_made_by_queue_first_ = 0;

    /** Completions the disk answered itself, which no submission entry ever held. */
    std::uint32_t completions_made_by_queue_count_ = 0;

    /** How the ring addresses the thread's blocks. */
    staging_pinning_t pinning_ = staging_pinning_t::unpinned_k;

    /** Requests sent unpinned across a pinned gibibyte, as
     *  @c requests_across_registered_slots reports. */
    std::uint64_t requests_across_registered_slots_ = 0;
};

#pragma endregion Queue

#pragma region Disk

/**
 *  @brief The ring Linux offers, over one or more @c O_DIRECT files registered as media.
 *
 *  This is not a @c basic_disk over a blocking device: the queue is the kernel's, so the disk
 *  drives it directly and only the bookkeeping is its own. @c submit writes submission entries and
 *  enters once, @c poll reads whatever landed from the shared ring and enters, with @c min_complete
 *  of zero, only when the kernel flags completion work waiting on the task, and neither ever waits.
 *  The rings belong to queue sets; the disk owns its media and their epoch, which @c register_media
 *  writes into a ring's file table. It serves a @c blocking_queue_t too, with positioned transfers
 *  on the polling thread.
 *
 *  A defect the ring can see before the kernel does - a stale registered memory, an extent past its
 *  range, a length past one transfer, an alignment the filesystem refuses - never fails the batch.
 *  It becomes that one request's completion and leaves its neighbours alone, which is what a chain
 *  of submission entries would do anyway. Only a token that is not this queue's fails the whole @c
 *  submit, since a misrouted completion is unrecoverable.
 *
 *  @tparam atomic_reference_ The reference the claim, epoch and in-submitted words
 *      are owned through.
 *  @tparam waiter_type_ What a draining core does after a poll that moved nothing.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_uring_files_disk {
  public:

    /** The queue a thread owns for this transport. */
    using queue_t = linux_uring_queue_t;

    linux_uring_files_disk() noexcept = default;
    linux_uring_files_disk(linux_uring_files_disk const&) = delete;
    linux_uring_files_disk& operator=(linux_uring_files_disk const&) = delete;
    linux_uring_files_disk(linux_uring_files_disk&&) noexcept = default;

    linux_uring_files_disk& operator=(linux_uring_files_disk&& other) noexcept {
        if (this == &other) return *this;
        close_all_();
        registered_memories_ = std::move(other.registered_memories_);
        media_ = std::move(other.media_);
        media_epoch_ = other.media_epoch_;
        media_identity_ = other.media_identity_;
        buffering_ = other.buffering_;
        return *this;
    }

    ~linux_uring_files_disk() noexcept { close_all_(); }

    /**
     *  @brief Sets one tiny ring up and takes it down again, which is the only honest availability test.
     *  @param[in] flags The setup flags tried, which a test widens to one the kernel refuses.
     *  @return Whether the kernel offers the flags this transport needs, or why it does not.
     */
    [[nodiscard]] static linux_uring_probe_t probe(
        std::uint32_t flags = linux_uring_queue_t::ring_setup_flags_k) noexcept {
        linux_uring_params_t parameters {};
        parameters.setup_flags = flags;
        long const opened = linux_uring_queue_t::setup_(linux_uring_queue_t::probe_entries_k, parameters);
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
     *  @brief Sizes the tables of ranges and media the queue sets' rings hold.
     *  @param[in] config The ranges and media recorded at once, and the buffering.
     *
     *  @note No file is opened here; a mount registers each one as a medium, so one disk serves a
     *      directory of many sorted runs rather than one disk per run.
     */
    static expected<linux_uring_files_disk> make(linux_uring_disk_config_t const& config) noexcept {
        if (config.registered_memory_limit >= static_cast<std::uint16_t>(registered_memory_id_t::queue_range_k) ||
            config.media_limit == static_cast<std::uint16_t>(registered_medium_id_t::unregistered_k))
            return status_t {status_code_t::invalid_argument_k, "A ring limit reaches a reserved id"};
        if (config.media_limit == 0 || config.registered_memory_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A ring needs a medium and a range"};

        linux_uring_files_disk disk;
        disk.buffering_ = config.buffering;
        uuid128_t const drawn = uuid_generator();
        if (!drawn.is_valid())
            return status_t {status_code_t::io_k, "The system refused entropy for the media identity"};
        disk.media_identity_ = uuid_to_u64(drawn) | 1u;
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
        reported.max_transfer_bytes = native_transfer_bytes_k;
        reported.depth_limit = linux_uring_queue_t::depth_ceiling_k;
        reported.media_limit = static_cast<std::uint16_t>(media_.size());
        reported.reaches_gpu_memory = false;
        return reported;
    }

#pragma region Queues

    /**
     *  @brief Writes this disk's media into @p queue_pair's file table when it holds another disk
     *      or an older medium epoch, which is no system call at all once warm.
     *  @param[in] queue_pair The queue the calling thread owns, with nothing outstanding on it.
     */
    status_t register_media(queue_ref_t const& queue_pair) noexcept {
        if (queue_pair.template queue<blocking_queue_t>()) return {};
        linux_uring_queue_t* const queue = queue_of_(queue_pair);
        if (!queue) return {status_code_t::invalid_argument_k, "The queue is of another transport"};
        media_list_key_t const wanted {media_identity_, epoch_(media_epoch_).load(std::memory_order_acquire)};
        if (queue->media_list_key_ == wanted) return {};
        if (status_t const reserved = queue->reserve_files_(media_.size()); reserved.failed()) return reserved;

        std::uint32_t const files_update = 6; // IORING_REGISTER_FILES_UPDATE
        std::span<int> const files {queue->files_};
        linux_uring_rsrc_update_t update {};
        update.data = reinterpret_cast<std::uint64_t>(files.data());
        for (;;) {
            for (std::size_t index = 0; index != files.size(); ++index) {
                registered_medium_slot_t* const slot = index < media_.size() ? &media_[index] : nullptr;
                bool const live = slot && claim_(slot->registered).load(std::memory_order_acquire) != 0;
                files[index] = live ? atomic_reference_<native_file_t> {slot->file}.load(std::memory_order_acquire)
                                    : invalid_native_file_k;
            }
            if (linux_uring_queue_t::register_(queue->ring_, files_update, &update,
                                               static_cast<unsigned>(files.size())) >= 0)
                break;
            // A medium closed after the table read it, and the next read finds it cleared
            if (errno != EBADF) return native_file_failure(errno, "The ring would not take its registered media");
        }
        queue->media_list_key_ = wanted;
        return {};
    }

    /** Requests @p queue_pair has submitted and not yet completed, which its credit bounds. */
    [[nodiscard]] std::size_t outstanding(queue_ref_t const& queue_pair) const noexcept {
        if (blocking_queue_t const* const blocking = queue_pair.template queue<blocking_queue_t>(); blocking)
            return blocking->outstanding();
        linux_uring_queue_t const* const queue = queue_pair.template queue<linux_uring_queue_t>();
        return queue ? queue->outstanding_ + queue->completions_made_by_queue_count_ : 0;
    }

#pragma endregion Queues

#pragma region Registrations

    /**
     *  @brief Records @p memory so requests can address it by slot, which the rings
     *      reach by pointer.
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
        return status_t {status_code_t::capacity_exhausted_k, "Every registered_memory slot of this ring is taken"};
    }

    /**
     *  @brief Gives @p registered memory back, refusing while the kernel may still be writing into it.
     *  @param[in,out] registered_memory The range to give back, surrendered on success.
     */
    status_t unregister_memory(registered_memory_t& registered_memory) noexcept {
        registered_memory_slot_t* const slot = slot_of_(registered_memory);
        if (!slot) return {status_code_t::invalid_argument_k, "The registered_memory is not this ring's"};
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
            atomic_reference_<native_file_t> {slot.file}.store(file, std::memory_order_release);
            slot.capacity = medium_bytes_of(file);
            slot.memory_alignment = alignment.memory;
            slot.offset_alignment = alignment.offset;
            epoch_(media_epoch_).store_add(1, std::memory_order_release);
            return registered_medium_t {disk_handle_t {static_cast<std::uint16_t>(index), slot.generation},
                                        slot.capacity, direct.offset};
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every medium slot of this ring is taken"};
    }

    /**
     *  @brief Closes @p registered medium, refusing while requests against it are still in flight.
     *  @param[in,out] registered_medium The medium to give back, surrendered on success.
     *
     *  @note A ring that held the file keeps it referenced until its next registration, which the
     *      raised epoch makes rewrite the table.
     */
    status_t unregister_medium(registered_medium_t& registered_medium) noexcept {
        registered_medium_slot_t* const slot = slot_of_(registered_medium);
        if (!slot) return {status_code_t::invalid_argument_k, "The medium is not this ring's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "Requests in flight still use the medium"};
        native_file_t const file = atomic_reference_<native_file_t> {slot->file}.exchange(invalid_native_file_k,
                                                                                          std::memory_order_acq_rel);
        [[maybe_unused]] status_t const closed = close_native_file(file);
        ++slot->generation;
        slot->capacity = 0;
        claim_(slot->registered).store(0, std::memory_order_release);
        epoch_(media_epoch_).store_add(1, std::memory_order_release);
        registered_medium.surrender();
        return {};
    }

#pragma endregion Registrations

#pragma region Ring

    /**
     *  @brief Takes as much of @p requests as @p queue_pair still has credit for, and never waits.
     *  @param[in] queue_pair The queue submitting, owned by the calling thread, which this holds
     *      the disk into.
     *  @param[in] requests The batch, of which a prefix is taken.
     *  @return How many were taken, which is zero on a full ring and never a reason to block.
     */
    expected<std::size_t> submit(queue_ref_t const& queue_pair, std::span<request_t const> requests) noexcept {
        for (request_t const& request : requests)
            if (request.token.queue_pair() != queue_pair.id())
                return status_t {status_code_t::invalid_argument_k, "A request holds another queue pair's token"};
        if (blocking_queue_t* const blocking = queue_pair.template queue<blocking_queue_t>(); blocking)
            return blocking->take(requests,
                                  [&](request_t const& request) noexcept { count_request_on_slots_(request); });
        linux_uring_queue_t* const queue = queue_of_(queue_pair);
        if (!queue) return status_t {status_code_t::invalid_argument_k, "The queue is of another transport"};
        // A medium a call registers after its hold, as a volume does for a new run file, still
        // reaches the table
        if (status_t const held = register_media(queue_pair); held.failed()) return held;

        linux_uring_queue_t::ring_t& ring = queue->ring_;
        std::size_t const credit = queue->depth_ - (queue->outstanding_ + queue->completions_made_by_queue_count_);
        unsigned const consumed = atomic_reference_<unsigned> {*ring.submission_head}.load(std::memory_order_acquire);
        unsigned const published = *ring.submission_tail;
        std::size_t const seats = ring.submission_entries_count - (published - consumed);
        std::size_t const room = credit < seats ? credit : seats;
        std::size_t const taking = requests.size() < room ? requests.size() : room;

        submission_chain_t chain;
        std::size_t queued = 0;
        bool chain_broken = false;
        for (std::size_t index = 0; index != taking; ++index) {
            request_t const& request = requests[index];
            if (chain_broken) {
                chain.unlink();
                push_completion_made_by_queue_(*queue, cancelled_completion_(request.token));
                chain_broken = chains_next_(request);
                continue;
            }
            status_t const defect = inspect_(queue->pinned_staging_, request);
            if (defect.failed()) {
                chain.unlink();
                push_completion_made_by_queue_(*queue, failed_completion_(request.token, defect));
                chain_broken = chains_next_(request);
                continue;
            }
            if (request.kind == request_kind_t::deallocate_k) {
                bool const chained_into = chain.linked != nullptr;
                chain.unlink();
                status_t const deallocated = chained_into
                                                 ? status_t {status_code_t::invalid_argument_k,
                                                             "A deallocation runs at submission, outside chains"}
                                                 : deallocate_native_file(resolve_(request.medium)->file,
                                                                          request.extent);
                push_completion_made_by_queue_(*queue, completion_run_by_queue_(request.token, deallocated));
                chain_broken = chains_next_(request) && deallocated.failed();
                continue;
            }
            linux_uring_sqe_t& entry = ring.submission_entries[(published + queued) & ring.submission_mask];
            build_(*queue, entry, request);
            count_request_on_slots_(request);
            track_(*queue, request);
            chain.linked = chains_next_(request) ? &entry : nullptr;
            ++queued;
            chain_broken = false;
        }
        chain.unlink();

        if (queued != 0) {
            atomic_reference_<unsigned> {*ring.submission_tail}.store(static_cast<unsigned>(published + queued),
                                                                      std::memory_order_release);
            std::size_t accepted = 0;
            while (accepted != queued) {
                long const entered = linux_uring_queue_t::enter_(ring, static_cast<unsigned>(queued - accepted), 0u);
                if (entered > 0) {
                    accepted += static_cast<std::size_t>(entered);
                    continue;
                }
                if (entered < 0 && errno == EINTR) continue;
                break;
            }
            if (accepted != queued)
                refuse_tail_(*queue, static_cast<unsigned>(published + accepted), queued - accepted);
        }
        return taking;
    }

    /**
     *  @brief Runs this ring's pending completion work and copies what landed into @p into.
     *  @param[in] queue_pair The queue polling, owned by the calling thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero when nothing has landed.
     *
     *  @note The call enters the ring only when the kernel raised @c IORING_SQ_TASKRUN, and then
     *      with @c min_complete of zero, so it flushes that task work and returns without
     *      waiting for anything.
     */
    expected<std::size_t> poll_completions(queue_ref_t const& queue_pair, std::span<completion_t> into) noexcept {
        if (blocking_queue_t* const blocking = queue_pair.template queue<blocking_queue_t>(); blocking)
            return blocking->poll(
                into, [&](request_t const& request) noexcept { return perform_blocking_(*blocking, request); },
                [&](request_t const& request) noexcept { uncount_request_on_slots_(request.buffer, request.medium); });
        linux_uring_queue_t* const queue = queue_of_(queue_pair);
        if (!queue) return status_t {status_code_t::invalid_argument_k, "The queue is of another transport"};
        linux_uring_queue_t::ring_t& ring = queue->ring_;

        std::size_t moved = 0;
        if (queue->outstanding_ != 0 && runs_task_work_(ring)) enter_for_completions_(ring);
        moved += poll_completions_(*queue, into);
        if (overflowed_(ring) && moved != into.size()) {
            enter_for_completions_(ring);
            moved += poll_completions_(*queue, into.subspan(moved));
        }
        moved += drain_completions_made_by_queue_(*queue, into.subspan(moved));
        return moved;
    }

    /**
     *  @brief Moves at least one completion of @p queue_pair into @p into, sleeping in the kernel
     *      until one lands.
     *  @param[in] queue_pair The queue waiting, owned by the thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero only once nothing is outstanding.
     *
     *  @note The ring is entered with @c min_complete of one, which runs the task work a
     *      cooperative ring defers and parks the thread until a completion posts.
     */
    expected<std::size_t> wait_completions(queue_ref_t const& queue_pair, std::span<completion_t> into) noexcept {
        if (into.empty()) return status_t {status_code_t::invalid_argument_k, "A wait needs room for a completion"};
        linux_uring_queue_t* const queue = queue_of_(queue_pair);
        for (;;) {
            auto [moved, poll_completions_status] = poll_completions(queue_pair, into);
            if (poll_completions_status.failed()) return poll_completions_status;
            if (moved != 0 || !queue || queue->outstanding_ == 0) return moved;
            if (status_t const entered = enter_for_one_completion_(queue->ring_); entered.failed()) return entered;
        }
    }

    /**
     *  @brief Asks the kernel to stop everything @p queue_pair has in flight, which
     *      poll reports cancelled.
     *  @param[in] queue_pair The queue to stop, whose started requests still run to their own end.
     *
     *  @note The cancelling entry posts a completion of its own, which poll recognises and drops,
     *      so the queue still sees exactly one completion per request it submitted.
     */
    void cancel(queue_ref_t const& queue_pair) noexcept {
        if (blocking_queue_t* const blocking = queue_pair.template queue<blocking_queue_t>(); blocking)
            return blocking->cancel();
        linux_uring_queue_t* const queue = queue_of_(queue_pair);
        if (!queue || queue->outstanding_ == 0) return;
        linux_uring_queue_t::ring_t& ring = queue->ring_;
        unsigned const consumed = atomic_reference_<unsigned> {*ring.submission_head}.load(std::memory_order_acquire);
        unsigned const published = *ring.submission_tail;
        if (published - consumed >= ring.submission_entries_count) return;

        std::uint8_t const async_cancel = 14;     // IORING_OP_ASYNC_CANCEL
        std::uint32_t const cancel_any = 1u << 2; // IORING_ASYNC_CANCEL_ANY, from Linux 5.19
        linux_uring_sqe_t& entry = ring.submission_entries[published & ring.submission_mask];
        std::memset(&entry, 0, sizeof(entry));
        entry.opcode = async_cancel;
        entry.cancel_flags = cancel_any;
        entry.user_data = cancel_marker_k;
        atomic_reference_<unsigned> {*ring.submission_tail}.store(published + 1, std::memory_order_release);
        [[maybe_unused]] long const entered = linux_uring_queue_t::enter_(ring, 1u, 0u);
    }

    /**
     *  @brief Finishes and discards everything @p queue_pair has outstanding, so its buffers
     *      may be freed.
     *  @param[in] queue_pair The queue to empty, which may be cancelled or still running.
     */
    status_t drain(queue_ref_t const& queue_pair) noexcept {
        if (blocking_queue_t* const blocking = queue_pair.template queue<blocking_queue_t>(); blocking) {
            blocking->drain(
                [&](request_t const& request) noexcept { return perform_blocking_(*blocking, request); },
                [&](request_t const& request) noexcept { uncount_request_on_slots_(request.buffer, request.medium); });
            return {};
        }
        linux_uring_queue_t const* const queue = queue_of_(queue_pair);
        if (!queue) return {status_code_t::invalid_argument_k, "The queue is of another transport"};
        return drain_by_polling<waiter_type_>(*this, queue_pair);
    }

#pragma endregion Ring

  private:

    using submitted_request_t = linux_uring_queue_t::submitted_request_t;

    /** The chain one submission queues, held by its last entry while it links to the next one. */
    struct submission_chain_t {

        /** The last entry queued, while it links to the next request, or null. */
        linux_uring_sqe_t* linked = nullptr;

        /** Drops the link from @c linked, as the request that would have followed it never came. */
        void unlink() noexcept {
            if (!linked) return;
            linked->entry_flags = static_cast<std::uint8_t>(linked->entry_flags & ~linux_uring_entry_io_link_k);
            linked = nullptr;
        }
    };

    /** One recorded memory range, and the count of requests the kernel may still be writing into
     *  it. */
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

        /** The open file, which every ring holding this disk installs at this slot's own table
         *  index. */
        native_file_t file = invalid_native_file_k;

        /** Bytes the medium held at registration: a regular file grows past it, a block device
         *  falls short. */
        std::uint64_t capacity = 0;

        /** Alignment this filesystem demands of the first byte of a buffer. */
        std::uint64_t memory_alignment = portable_alignment_k;

        /** Alignment this filesystem demands of an extent's offset and its length. */
        std::uint64_t offset_alignment = portable_alignment_k;
    };

    /** The completion a cancelling entry posts for itself, which no queue can ever mint. */
    static constexpr std::uint64_t cancel_marker_k = ~std::uint64_t {0};

    /** Grows @p elements to @p count default slots, reporting an allocation failure as a status. */
    template <typename element_type_>
    static status_t size_(st::vector<element_type_>& elements, std::size_t count) noexcept {
        if (elements.resize(count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A ring could not allocate its slots"};
        return {};
    }

    static atomic_reference_<std::int32_t> claim_(std::int32_t& word) noexcept {
        return atomic_reference_<std::int32_t> {word};
    }

    static atomic_reference_<epoch_t> epoch_(epoch_t& word) noexcept { return atomic_reference_<epoch_t> {word}; }

    static atomic_reference_<std::int32_t> in_flight_(registered_memory_slot_t& slot) noexcept {
        return atomic_reference_<std::int32_t> {slot.in_flight};
    }

    static atomic_reference_<std::int32_t> in_flight_(registered_medium_slot_t& slot) noexcept {
        return atomic_reference_<std::int32_t> {slot.in_flight};
    }

    [[nodiscard]] static linux_uring_queue_t* queue_of_(queue_ref_t const& queue_pair) noexcept {
        return queue_pair.template queue<linux_uring_queue_t>();
    }

#pragma region Rings

    /** Runs the completion work the kernel queued for @p ring, without waiting. */
    static void enter_for_completions_(linux_uring_queue_t::ring_t const& ring) noexcept {
        std::uint32_t const getevents = 1u << 0; // IORING_ENTER_GETEVENTS
        [[maybe_unused]] long const entered = linux_uring_queue_t::enter_(ring, 0u, 0u, getevents);
    }

    /** Parks the calling thread in @p ring until a completion posts, running deferred task work. */
    static status_t enter_for_one_completion_(linux_uring_queue_t::ring_t const& ring) noexcept {
        std::uint32_t const getevents = 1u << 0; // IORING_ENTER_GETEVENTS
        long const entered = linux_uring_queue_t::enter_(ring, 0u, 1u, getevents);
        // A signal, or an overflow the next poll flushes, leaves the wait to the caller's next turn
        if (entered < 0 && errno != EINTR && errno != EAGAIN && errno != EBUSY)
            return native_file_failure(errno, "The ring would not wait for a completion");
        return {};
    }

    /** Whether the kernel queued completion work that only an entry into the ring runs. */
    [[nodiscard]] static bool runs_task_work_(linux_uring_queue_t::ring_t const& ring) noexcept {
        unsigned const task_run = 1u << 2; // IORING_SQ_TASKRUN, from Linux 5.19
        unsigned const flags = atomic_reference_<unsigned> {*ring.submission_flags}.load(std::memory_order_acquire);
        return (flags & task_run) != 0;
    }

    /** Whether the kernel is holding completions back because the queue had no room for them. */
    [[nodiscard]] static bool overflowed_(linux_uring_queue_t::ring_t const& ring) noexcept {
        unsigned const completion_overflow = 1u << 1; // IORING_SQ_CQ_OVERFLOW
        unsigned const flags = atomic_reference_<unsigned> {*ring.submission_flags}.load(std::memory_order_acquire);
        return (flags & completion_overflow) != 0;
    }

    /** Closes every open medium, which both the destructor and a move assignment need. */
    void close_all_() noexcept {
        for (std::size_t index = 0; index != media_.size(); ++index)
            if (media_[index].file != invalid_native_file_k) [[maybe_unused]]
                status_t const closed = close_native_file(media_[index].file);
        media_.clear();
    }

#pragma endregion Rings

#pragma region Bookkeeping

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

    /** Releases both once the kernel is done with the bytes of @p buffer on @p medium. */
    void uncount_request_on_slots_(memory_reference_t buffer, medium_reference_t medium) noexcept {
        if (registered_memory_slot_t* const slot = resolve_(buffer); slot)
            in_flight_(*slot).store_sub(1, std::memory_order_release);
        if (registered_medium_slot_t* const slot = resolve_(medium); slot)
            in_flight_(*slot).store_sub(1, std::memory_order_release);
    }

    /** Remembers what @p request will need when only its token and result come back. */
    static void track_(linux_uring_queue_t& queue, request_t const& request) noexcept {
        submitted_request_t& submitted = queue.submitted_requests_[queue.outstanding_];
        submitted.token = request.token;
        submitted.length = request.kind == request_kind_t::flush_k ? 0 : request.extent.length;
        submitted.buffer = request.buffer;
        submitted.medium = request.medium;
        ++queue.outstanding_;
    }

    /** Takes @p token out of @p queue's outstanding set, moving the last entry into the gap. */
    static expected<submitted_request_t> retire_(linux_uring_queue_t& queue, request_token_t token) noexcept {
        for (std::uint32_t index = 0; index != queue.outstanding_; ++index) {
            submitted_request_t& submitted = queue.submitted_requests_[index];
            if (!(submitted.token == token)) continue;
            submitted_request_t const taken = submitted;
            submitted = queue.submitted_requests_[queue.outstanding_ - 1];
            --queue.outstanding_;
            return taken;
        }
        return status_t {status_code_t::not_found_k, "A completion carries an unknown token"};
    }

    /** Places @p completion at the back of @p queue's own completions, which poll drains after the
     *  kernel's. */
    static void push_completion_made_by_queue_(linux_uring_queue_t& queue, completion_t const& completion) noexcept {
        queue.completions_made_by_queue_[(queue.completions_made_by_queue_first_ +
                                          queue.completions_made_by_queue_count_) %
                                         queue.depth_] = completion;
        ++queue.completions_made_by_queue_count_;
    }

    /** Moves what the disk answered itself into @p into, oldest first. */
    static std::size_t drain_completions_made_by_queue_(linux_uring_queue_t& queue,
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

    /** Whatever would corrupt a submission entry rather than merely fail on the device, @p range
     *  being the submitting queue's own blocks. */
    status_t inspect_(std::span<byte_t> range, request_t const& request) noexcept {
        registered_medium_slot_t const* const medium = resolve_(request.medium);
        if (!medium) return {status_code_t::invalid_argument_k, "The request names no registered medium"};
        if (request.kind == request_kind_t::flush_k || request.kind == request_kind_t::deallocate_k) return {};
        std::span<byte_t> const memory = memory_of_(range, request.buffer);
        if (memory.empty()) return {status_code_t::invalid_argument_k, "The request names no registered range"};
        if (request.buffer.offset > memory.size() || request.extent.length > memory.size() - request.buffer.offset)
            return {status_code_t::invalid_argument_k, "The request runs past its registered range"};
        if (request.extent.length > native_transfer_bytes_k)
            return {status_code_t::invalid_argument_k, "The request is larger than one transfer"};
        byte_t const* const address = memory.data() + request.buffer.offset;
        if (!is_address_aligned(address, medium->memory_alignment) ||
            !request.extent.aligned_to(medium->offset_alignment))
            return {status_code_t::invalid_argument_k, "The request does not meet the disk's alignment"};
        return {};
    }

    /** Writes @p request into @p entry, addressing the medium by table index and the queue's pinned
     *  blocks by buffer index, and every other range by pointer. */
    void build_(linux_uring_queue_t& queue, linux_uring_sqe_t& entry, request_t const& request) noexcept {
        std::uint8_t const fixed_file = 1u << 0; // IOSQE_FIXED_FILE
        std::uint8_t const flush = 3;            // IORING_OP_FSYNC
        std::uint32_t const datasync = 1u << 0;  // IORING_FSYNC_DATASYNC
        std::uint8_t const read_pinned = 4;      // IORING_OP_READ_FIXED
        std::uint8_t const write_pinned = 5;     // IORING_OP_WRITE_FIXED
        std::uint8_t const read_unpinned = 22;   // IORING_OP_READ, from Linux 5.6
        std::uint8_t const write_unpinned = 23;  // IORING_OP_WRITE, from Linux 5.6
        std::memset(&entry, 0, sizeof(entry));
        entry.entry_flags = fixed_file;
        if (request.chaining == request_chaining_t::chained_k) entry.entry_flags |= linux_uring_entry_io_link_k;
        entry.fd = static_cast<std::int32_t>(request.medium.medium_id);
        entry.user_data = request.token.bits;
        if (request.kind == request_kind_t::flush_k) {
            entry.opcode = flush;
            entry.fsync_flags = datasync;
            return;
        }
        std::span<byte_t> const memory = memory_of_(queue.pinned_staging_, request.buffer);
        entry.off = request.extent.offset;
        entry.addr = reinterpret_cast<std::uint64_t>(memory.data() + request.buffer.offset);
        entry.len = static_cast<std::uint32_t>(request.extent.length);
        bool const own = request.buffer.memory_id == registered_memory_id_t::queue_range_k;
        std::uint64_t const piece = request.buffer.offset / linux_uring_registered_range_bytes_k;
        bool const across = request.buffer.offset % linux_uring_registered_range_bytes_k + request.extent.length >
                            linux_uring_registered_range_bytes_k;
        bool const pinned = own && queue.pinning_ == staging_pinning_t::pinned_k;
        // A request across a gibibyte boundary spans two table slots, which no fixed transfer names
        if (pinned && across) ++queue.requests_across_registered_slots_;
        if (pinned && !across) {
            entry.opcode = request.kind == request_kind_t::read_k ? read_pinned : write_pinned;
            entry.buf_index = static_cast<std::uint16_t>(piece);
            return;
        }
        entry.opcode = request.kind == request_kind_t::read_k ? read_unpinned : write_unpinned;
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

    /** Takes @p count entries the kernel would not accept back out of @p queue's ring and fails
     *  them. */
    void refuse_tail_(linux_uring_queue_t& queue, unsigned tail, std::size_t count) noexcept {
        atomic_reference_<unsigned> {*queue.ring_.submission_tail}.store(tail, std::memory_order_release);
        status_t const refused {status_code_t::io_k, "The kernel would not accept this request"};
        for (std::size_t index = 0; index != count; ++index) {
            submitted_request_t const& submitted = queue.submitted_requests_[queue.outstanding_ - 1];
            completion_t const completion = failed_completion_(submitted.token, refused);
            uncount_request_on_slots_(submitted.buffer, submitted.medium);
            --queue.outstanding_;
            push_completion_made_by_queue_(queue, completion);
        }
    }

    /** Turns one ring entry into the completion its queue reads, by what the kernel returned. */
    [[nodiscard]] static completion_t translate_(submitted_request_t const& submitted, int reason) noexcept {
        completion_t completion;
        completion.token = submitted.token;
        if (reason >= 0) {
            completion.transferred_bytes = static_cast<std::uint64_t>(reason);
            completion.state = completion.transferred_bytes == submitted.length ? completion_state_t::completed_k
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
    std::size_t poll_completions_(linux_uring_queue_t& queue, std::span<completion_t> into) noexcept {
        linux_uring_queue_t::ring_t& ring = queue.ring_;
        unsigned const taken = *ring.completion_head;
        unsigned head = taken;
        unsigned const posted = atomic_reference_<unsigned> {*ring.completion_tail}.load(std::memory_order_acquire);
        std::size_t moved = 0;
        while (head != posted && moved != into.size()) {
            linux_uring_cqe_t const& entry = ring.completion_entries[head & ring.completion_mask];
            ++head;
            auto const [submitted, retire_status] = retire_(queue, request_token_t {entry.user_data});
            if (retire_status.failed()) continue;
            into[moved] = translate_(submitted, entry.res);
            uncount_request_on_slots_(submitted.buffer, submitted.medium);
            ++moved;
        }
        if (head != taken) atomic_reference_<unsigned> {*ring.completion_head}.store(head, std::memory_order_release);
        return moved;
    }

#pragma endregion Requests

    /** One slot per memory range the disk records. */
    st::vector<registered_memory_slot_t> registered_memories_;

    /** One slot per medium the disk opens, whose index is its slot in every holding ring's file
     *  table. */
    st::vector<registered_medium_slot_t> media_;

    /** Raised whenever a medium is opened or closed, so a ring holding this disk knows its table
     *  is behind. */
    epoch_t media_epoch_ = 1;

    /** Drawn at @c make, so a disk re-made where a freed one lived never matches a ring's stale
     *  table. */
    std::uint64_t media_identity_ = 0;

    /** Whether the page cache stands in the path, which fixes what a medium with no alignment reports. */
    file_buffering_t buffering_ = file_buffering_t::unbuffered_k;
};

#pragma endregion Disk

#else

#pragma region Absent

/** The queue on a platform that has no io_uring, which a queue set never makes. */
class linux_uring_queue_t {
  public:

    /** Refuses the queue, since this platform has no ring. */
    static expected<linux_uring_queue_t> make(queue_depth_t, std::span<byte_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Requests the queue keeps in flight, which is none. */
    [[nodiscard]] queue_depth_t depth() const noexcept { return 0; }

    /** Answers that nothing is pinned, since nothing was ever made. */
    [[nodiscard]] staging_pinning_t pinning() const noexcept { return staging_pinning_t::unpinned_k; }

    /** Answers that no request crossed a pinned gibibyte, since nothing was ever pinned. */
    [[nodiscard]] std::uint64_t requests_across_registered_slots() const noexcept { return 0; }
};

/**
 *  @brief The same disk on a platform that has no io_uring, which mounts nowhere and refuses cleanly.
 *
 *  The type exists so a caller compiles against it unconditionally and chooses at mount, where a
 *  refused @c make sends it to the blocking floor rather than to a build-time branch.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_uring_files_disk {
  public:

    /** The queue a thread owns for this transport. */
    using queue_t = linux_uring_queue_t;

    /** Answers that this platform has no ring to offer. */
    [[nodiscard]] static linux_uring_probe_t probe(std::uint32_t = 0) noexcept {
        return linux_uring_probe_t::refused_k;
    }

    /** Refuses the mount, since this platform has no ring to mount on. */
    static expected<linux_uring_files_disk> make(linux_uring_disk_config_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** What this disk would accept, which is nothing at all. */
    [[nodiscard]] disk_capabilities_t capabilities() const noexcept { return {}; }

    /** Refuses the registration, since there is no ring to register media on. */
    status_t register_media(queue_ref_t const&) noexcept {
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
    expected<std::size_t> submit(queue_ref_t const&, std::span<request_t const>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Answers nothing, since nothing was ever submitted. */
    expected<std::size_t> poll_completions(queue_ref_t const&, std::span<completion_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Waits for nothing, since nothing was ever submitted. */
    expected<std::size_t> wait_completions(queue_ref_t const&, std::span<completion_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Stops nothing, since nothing was ever submitted. */
    void cancel(queue_ref_t const&) noexcept {}

    /** Refuses the drain, since no queue pair was ever handed out. */
    status_t drain(queue_ref_t const&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no io_uring"};
    }

    /** Answers that nothing is outstanding, which is always true here. */
    [[nodiscard]] std::size_t outstanding(queue_ref_t const&) const noexcept { return 0; }
};

#pragma endregion Absent

#endif

} // namespace unum::ustore::utilities
