/**
 *  @file include/ustore/utilities/nvme_linux_cufile_disk.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The ring NVIDIA's cuFile offers: @c O_DIRECT files read into and written from device or host memory.
 *
 *  Nothing here links cuFile or includes its headers. @c cufile_api_t opens @c libcufile.so.0 once,
 *  as the process's GPU drivers load it and hand it to every disk, and the few structures it passes
 *  are declared here, so every build compiles this disk and only a machine with the library mounts
 *  it; release 1.19 and later need no @c libcuda of their own. A queue set's queue holds one batch
 *  of @c cuFileBatchIOSetUp sized to its depth, set up the first time a disk holds it, @c submit
 *  hands staged requests to @c cuFileBatchIOSubmit, and @c poll_completions asks
 *  @c cuFileBatchIOGetStatus for whatever landed with a minimum of zero, so neither waits.
 *
 *  With the @c nvidia_fs kernel module loaded, bytes move by DMA between the drive and device
 *  memory. Without it, compat mode moves them through host memory inside the library, which
 *  the driver must allow through @c CUFILE_PARAM_PROPERTIES_ALLOW_COMPAT_MODE. Like Linux AIO,
 *  a batch has no links, so a chain lives in user space, and cuFile has no flush, so a flush
 *  runs @c fdatasync on the submitting thread. Device memory is registered and moved under the
 *  CUDA context current on the thread that registers it.
 *
 *  The cuFile driver is process-wide and opened at the first mount; it is never closed here, since the library closes
 *  it at exit and another mount of the same process may still hold it.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int64_t`, `std::uint64_t`

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::move`, `std::exchange`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/blocking_files.hpp" // `shared_library_t`, `open_native_file`, `flush_native_file`
#include "ustore/utilities/nvme_disks.hpp" // `file_access_of`, `medium_bytes_of`, `deallocate_native_file`, `direct_alignment_of`
#include "ustore/utilities/nvme_ring.hpp" // `request_t`, `completion_t`, `queue_ref_t`, `portable_alignment_k`, `is_address_aligned`
#include "ustore/utilities/types.hpp" // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::utilities {

#pragma region Library ABI

/** The @c CUfileOpError values UStore tells apart; any other non-zero value is a failure, and every value below
 *  @c cufile_error_base_k an event reports negated is an @c errno. */
enum cufile_operation_error_t : int {
    cufile_success_k = 0,
    cufile_error_base_k = 5000,
    cufile_driver_already_open_k = 5026,
    cufile_batch_full_k = 5037,
};

/** What every cuFile call answers, @c CUfileError_t: the library's error and the CUDA driver's beneath it. */
struct cufile_error_t {
    cufile_operation_error_t error;
    int cuda_error;
};

/** The driver's properties, @c CUfileDrvProps_t, whose @c nvfs version is zero without the @c nvidia_fs module. */
struct cufile_driver_properties_t {
    struct {
        unsigned major_version;
        unsigned minor_version;
        std::size_t poll_threshold_size;
        std::size_t max_direct_io_size;
        unsigned status_flags;
        unsigned control_flags;
    } nvfs;
    unsigned feature_flags;
    unsigned max_device_cache_size;
    unsigned per_buffer_cache_size;
    unsigned max_device_pinned_memory_size;
    unsigned max_batch_io_size;
    unsigned max_batch_io_timeout_milliseconds;
};

/** The bit of the driver's control flags that allows compat mode, @c CU_FILE_ALLOW_COMPAT_MODE. */
inline constexpr unsigned cufile_allow_compat_mode_flag_k = 1u << 1;

/** The @c CUFileBoolConfigParameter_t values UStore sets before the driver opens. */
enum cufile_bool_parameter_t : int {
    cufile_parameter_allow_compat_mode_k = 1,
    cufile_parameter_force_compat_mode_k = 2,
};

/** A file as cuFile registers it, @c CUfileDescr_t, which UStore fills with a Linux descriptor. */
struct cufile_descriptor_t {
    int type;
    union {
        int file;
        void* handle;
    } handle;
    void const* file_system_operations;
};

/** @c CU_FILE_HANDLE_TYPE_OPAQUE_FD, the descriptor type of a Linux file. */
inline constexpr int cufile_handle_type_descriptor_k = 1;

/** A registered file, @c CUfileHandle_t, and a batch of requests, @c CUfileBatchHandle_t. */
using cufile_handle_t = void*;
using cufile_batch_handle_t = void*;

/** One request of a batch, @c CUfileIOParams_t, whose base may point into device or host memory. */
struct cufile_io_parameters_t {
    int mode;
    struct {
        void* base;
        std::int64_t file_offset;
        std::int64_t base_offset;
        std::size_t size;
    } batch;
    cufile_handle_t handle;
    int opcode;
    void* cookie;
};

/** One answered request of a batch, @c CUfileIOEvents_t, whose @c result is the bytes moved or a negated error. */
struct cufile_io_event_t {
    void* cookie;
    int status;
    std::size_t result;
};

/** @c CUFILE_BATCH, @c CUFILE_READ and @c CUFILE_WRITE. */
inline constexpr int cufile_batch_mode_k = 1;
inline constexpr int cufile_read_k = 0;
inline constexpr int cufile_write_k = 1;

/** The @c CUfileStatus_t values an event holds. */
enum cufile_status_t : int {
    cufile_waiting_k = 0x01,
    cufile_pending_k = 0x02,
    cufile_invalid_k = 0x04,
    cufile_canceled_k = 0x08,
    cufile_complete_k = 0x10,
    cufile_timeout_k = 0x20,
    cufile_failed_k = 0x40,
};

/** A time limit, as @c struct @c timespec spells it, which a status query passes as zero. */
struct cufile_time_limit_t {
    std::int64_t seconds;
    long nanoseconds;
};

#pragma endregion Library ABI

/**
 *  @brief The cuFile entry points UStore calls, resolved from @c libcufile by @c load.
 *
 *  Every entry is null when the library is absent or lacks any of them, which leaves the disk unmountable.
 */
struct cufile_api_t {
    cufile_error_t (*driver_open)() = nullptr;
    cufile_error_t (*driver_properties)(cufile_driver_properties_t*) = nullptr;
    cufile_error_t (*set_parameter)(cufile_bool_parameter_t, bool) = nullptr;
    cufile_error_t (*handle_register)(cufile_handle_t*, cufile_descriptor_t*) = nullptr;
    void (*handle_deregister)(cufile_handle_t) = nullptr;
    cufile_error_t (*buffer_register)(void const*, std::size_t, int) = nullptr;
    cufile_error_t (*buffer_deregister)(void const*) = nullptr;
    cufile_error_t (*batch_setup)(cufile_batch_handle_t*, unsigned) = nullptr;
    cufile_error_t (*batch_submit)(cufile_batch_handle_t, unsigned, cufile_io_parameters_t*, unsigned) = nullptr;
    cufile_error_t (*batch_status)(cufile_batch_handle_t, unsigned, unsigned*, cufile_io_event_t*,
                                   cufile_time_limit_t*) = nullptr;
    void (*batch_destroy)(cufile_batch_handle_t) = nullptr;

    /** Whether the library loaded with every entry resolved. */
    explicit operator bool() const noexcept { return batch_destroy != nullptr; }

    /** Opens the library and resolves every entry, or answers the empty table; the library stays open for the life of
     *  the process, as the table points into it. */
    static cufile_api_t load() noexcept {
        shared_library_t const library = shared_library_t::open("libcufile.so.0");
        if (!library) return {};
        cufile_api_t api;
        bool const resolved = //
            library.resolve("cuFileDriverOpen", api.driver_open) &&
            library.resolve("cuFileDriverGetProperties", api.driver_properties) &&
            library.resolve("cuFileSetParameterBool", api.set_parameter) &&
            library.resolve("cuFileHandleRegister", api.handle_register) &&
            library.resolve("cuFileHandleDeregister", api.handle_deregister) &&
            library.resolve("cuFileBufRegister", api.buffer_register) &&
            library.resolve("cuFileBufDeregister", api.buffer_deregister) &&
            library.resolve("cuFileBatchIOSetUp", api.batch_setup) &&
            library.resolve("cuFileBatchIOSubmit", api.batch_submit) &&
            library.resolve("cuFileBatchIOGetStatus", api.batch_status) &&
            library.resolve("cuFileBatchIODestroy", api.batch_destroy);
        return resolved ? api : cufile_api_t {};
    }
};

#pragma region Configuration

/** What a mount fixes about a cuFile disk before any queue holds it. */
struct linux_cufile_disk_config_t {

    /** Memory ranges the disk registers with cuFile at once. */
    std::uint16_t registered_memory_limit = 8;

    /** Targets the disk registers at once, which a directory of sorted runs raises above one. */
    std::uint16_t media_limit = 1;

    /** Whether bytes must move by DMA, may fall back to compat mode, or must take compat mode. */
    ustore_gds_t gds = ustore_gds_automatic_k;
};

/** How this machine moves bytes through cuFile, which a mount asks once before it opens a device disk. */
enum class linux_cufile_probe_t : std::uint8_t {

    /** The @c nvidia_fs module is loaded, so bytes move by DMA between the drive and device memory. */
    direct_k,

    /** No @c nvidia_fs, and the driver allows compat mode, which moves bytes through host memory. */
    compat_k,

    /** The library is present and refuses to open, or neither mode is available. */
    refused_k,

    /** No @c libcufile on this machine. */
    absent_k,
};

#pragma endregion Configuration

#if USTORE_OS_LINUX_

#pragma region Queue

template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_cufile_files_disk;

/**
 *  @brief One cuFile batch a thread owns: its request_slots, the requests staged for the library,
 *      the events one status query copies out, and the completions the disk answered itself.
 *
 *  The batch needs the library's entry points, which only a disk holds, so the first disk to hold
 *  the queue sets it up for the queue's whole depth and every later hold finds it there. A batch
 *  answers any thread of the process.
 */
class linux_cufile_queue_t {
  public:

    linux_cufile_queue_t() noexcept = default;
    linux_cufile_queue_t(linux_cufile_queue_t const&) = delete;
    linux_cufile_queue_t& operator=(linux_cufile_queue_t const&) = delete;

    linux_cufile_queue_t(linux_cufile_queue_t&& other) noexcept
        : batch_(std::exchange(other.batch_, nullptr)), batch_destroy_(std::exchange(other.batch_destroy_, nullptr)),
          submitted_requests_(std::move(other.submitted_requests_)), request_slots_(std::move(other.request_slots_)),
          staged_(std::move(other.staged_)), events_(std::move(other.events_)),
          completions_made_by_queue_(std::move(other.completions_made_by_queue_)),
          pinned_staging_(other.pinned_staging_), depth_(other.depth_), free_request_slots_(other.free_request_slots_),
          staged_count_(other.staged_count_), outstanding_(other.outstanding_),
          completions_made_by_queue_first_(other.completions_made_by_queue_first_),
          completions_made_by_queue_count_(other.completions_made_by_queue_count_) {}

    linux_cufile_queue_t& operator=(linux_cufile_queue_t&& other) noexcept {
        if (this == &other) return *this;
        teardown_();
        batch_ = std::exchange(other.batch_, nullptr);
        batch_destroy_ = std::exchange(other.batch_destroy_, nullptr);
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

    ~linux_cufile_queue_t() noexcept { teardown_(); }

    /**
     *  @brief Sizes the windows of @p depth requests, leaving the batch to the first disk that
     *      holds the queue.
     *  @param[in] depth Requests the queue keeps in flight at once, at most the
     *      driver's @c max_batch_io_size.
     *  @param[in] range The thread's blocks, which requests naming @c queue_range_k
     *      address, or empty.
     *
     *  @note The blocks are not registered with cuFile, which moves unregistered memory through
     *      its own buffers.
     */
    static expected<linux_cufile_queue_t> make(queue_depth_t depth, std::span<byte_t> range) noexcept {
        if (depth == 0) return status_t {status_code_t::invalid_argument_k, "A batch keeps a request in flight"};
        linux_cufile_queue_t queue;
        if (queue.submitted_requests_.resize(depth) != st::status_t::success_k ||
            queue.request_slots_.resize(depth) != st::status_t::success_k ||
            queue.staged_.resize(depth) != st::status_t::success_k ||
            queue.events_.resize(depth) != st::status_t::success_k ||
            queue.completions_made_by_queue_.resize(depth) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A batch could not size its windows"};
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
    friend class linux_cufile_files_disk;

    /** The request_slot that names no request, which ends a chain. */
    static constexpr std::uint32_t no_request_slot_k = ~std::uint32_t {0};

    /** One request from the moment it is taken until its completion is handed back, in a
     *  request_slot of its queue. */
    struct submitted_request_t {

        /** The request as it was taken, since an event holds only the request_slot
         *  and the result. */
        request_t request;

        /** The request_slot of the request held back until this one transfers in full,
         *  or @c no_request_slot_k. */
        std::uint32_t successor = no_request_slot_k;
    };

    /** Destroys the batch, which waits out whatever the library still holds. */
    void teardown_() noexcept {
        if (batch_) batch_destroy_(batch_);
        batch_ = nullptr;
    }

    /** The library's batch, null until a disk holds the queue. */
    cufile_batch_handle_t batch_ = nullptr;

    /** The entry destroying the batch, copied from the disk that set it up, since the library
     *  outlives every disk. */
    void (*batch_destroy_)(cufile_batch_handle_t) = nullptr;

    /** The requests in each request_slot. */
    st::vector<submitted_request_t> submitted_requests_;

    /** The free request_slots, stacked at the front. */
    st::vector<std::uint32_t> request_slots_;

    /** The requests awaiting @c cuFileBatchIOSubmit, contiguous as it reads them. */
    st::vector<cufile_io_parameters_t> staged_;

    /** Room for the events one status query copies out. */
    st::vector<cufile_io_event_t> events_;

    /** The completions the disk answered itself, read as a circle. */
    st::vector<completion_t> completions_made_by_queue_;

    /** The thread's blocks, which a request naming @c queue_range_k addresses. */
    std::span<byte_t> pinned_staging_;

    /** Requests the queue keeps in flight at once, the depth of its batch. */
    queue_depth_t depth_ = 0;

    /** Seats no request occupies. */
    std::uint32_t free_request_slots_ = 0;

    /** Requests built and not yet accepted, in order at the front of @c staged_. */
    std::uint32_t staged_count_ = 0;

    /** Requests the library accepted and has not yet answered for. */
    std::uint32_t outstanding_ = 0;

    /** Where the oldest unreaped local completion sits inside @c completions_made_by_queue_. */
    std::uint32_t completions_made_by_queue_first_ = 0;

    /** Completions the disk answered itself, which no event ever held. */
    std::uint32_t completions_made_by_queue_count_ = 0;
};

#pragma endregion Queue

#pragma region Disk

/**
 *  @brief cuFile over @c O_DIRECT files registered as media, one batch per queue set's queue,
 *      reaching device memory.
 *
 *  As with the AIO disk, the queue is the library's and only the bookkeeping is the disk's own. The
 *  batches belong to queue sets; the disk owns its media and its registered ranges, which every
 *  request names by its own cuFile handle, so a hold installs nothing. A defect the disk can see
 *  before the library does fails that one request's completion and leaves its neighbours alone; a
 *  batch the library refuses whole stays staged when it is full and fails its requests otherwise.
 *  Only a token that is not this queue's fails the whole @c submit. It serves no @c
 *  blocking_queue_t, since no database mount reads through it.
 *
 *  @tparam atomic_reference_ The reference the claim and in-submitted words are owned through.
 *  @tparam waiter_type_ What a draining core does after a poll that moved nothing.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_cufile_files_disk {
  public:

    /** The queue a thread owns for this transport. */
    using queue_t = linux_cufile_queue_t;

    linux_cufile_files_disk() noexcept = default;
    linux_cufile_files_disk(linux_cufile_files_disk const&) = delete;
    linux_cufile_files_disk& operator=(linux_cufile_files_disk const&) = delete;
    linux_cufile_files_disk(linux_cufile_files_disk&&) noexcept = default;

    linux_cufile_files_disk& operator=(linux_cufile_files_disk&& other) noexcept {
        if (this == &other) return *this;
        close_all_();
        api_ = other.api_;
        registered_memories_ = std::move(other.registered_memories_);
        media_ = std::move(other.media_);
        depth_limit_ = other.depth_limit_;
        max_transfer_bytes_ = other.max_transfer_bytes_;
        mode_ = other.mode_;
        return *this;
    }

    ~linux_cufile_files_disk() noexcept { close_all_(); }

    /**
     *  @brief Opens the driver and reads its properties, which tell DMA from compat mode.
     *  @return How bytes would move, or why they would not.
     */
    [[nodiscard]] static linux_cufile_probe_t probe(cufile_api_t const& api) noexcept {
        if (!api) return linux_cufile_probe_t::absent_k;
        cufile_driver_properties_t properties {};
        return open_driver_(api, properties) ? mode_of_(properties) : linux_cufile_probe_t::refused_k;
    }

    /**
     *  @brief Opens the driver through @p api, which outlives the disk, in the mode @p config asks
     *      for, and sizes the tables of ranges and media the queue sets' batches name.
     *  @param[in] config The ranges and media registered at once, and the GPUDirect Storage mode.
     *  @retval unsupported_k The machine has no @c libcufile.
     *  @retval device_k The driver would not open, @c ustore_gds_direct_k found no @c nvidia_fs, or compat mode is both
     *      needed and disallowed.
     */
    static expected<linux_cufile_files_disk> make(cufile_api_t const& api,
                                                  linux_cufile_disk_config_t const& config) noexcept {
        if (config.registered_memory_limit >= static_cast<std::uint16_t>(registered_memory_id_t::queue_range_k) ||
            config.media_limit == static_cast<std::uint16_t>(registered_medium_id_t::unregistered_k))
            return status_t {status_code_t::invalid_argument_k, "A disk keeps its last identifiers as special handles"};
        if (config.media_limit == 0 || config.registered_memory_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A disk needs a medium and a range to move between"};
        if (config.gds > ustore_gds_compatibility_k)
            return status_t {status_code_t::invalid_argument_k, "Unknown GPUDirect Storage mode"};

        linux_cufile_files_disk disk;
        if (!api) return status_t {status_code_t::unsupported_k, "This machine has no libcufile"};
        // The driver reads its parameters once, when it opens, so a later mount of the process inherits the first's
        if (config.gds != ustore_gds_direct_k) {
            [[maybe_unused]] cufile_error_t const allowed = api.set_parameter(cufile_parameter_allow_compat_mode_k,
                                                                              true);
        }
        if (config.gds == ustore_gds_compatibility_k) {
            [[maybe_unused]] cufile_error_t const forced = api.set_parameter(cufile_parameter_force_compat_mode_k,
                                                                             true);
        }
        cufile_driver_properties_t properties {};
        disk.api_ = &api;
        if (!open_driver_(api, properties))
            return status_t {status_code_t::device_k, "The cuFile driver would not open"};
        disk.mode_ = mode_of_(properties);
        if (config.gds == ustore_gds_direct_k && disk.mode_ != linux_cufile_probe_t::direct_k)
            return status_t {status_code_t::device_k, "GPUDirect Storage needs the nvidia_fs module, which is absent"};
        if (disk.mode_ == linux_cufile_probe_t::refused_k)
            return status_t {status_code_t::device_k, "cuFile has no nvidia_fs and compat mode is disallowed"};

        disk.depth_limit_ = properties.max_batch_io_size;
        std::uint64_t const direct_bytes = static_cast<std::uint64_t>(properties.nvfs.max_direct_io_size) * 1024u;
        disk.max_transfer_bytes_ = direct_bytes != 0 && direct_bytes < max_transfer_bytes_k ? direct_bytes
                                                                                            : max_transfer_bytes_k;
        if (status_t const sized = size_(disk.registered_memories_, config.registered_memory_limit); sized.failed())
            return sized;
        if (status_t const sized = size_(disk.media_, config.media_limit); sized.failed()) return sized;
        return disk;
    }

    /** How this disk moves bytes, @c direct_k or @c compat_k once made. */
    [[nodiscard]] linux_cufile_probe_t mode() const noexcept { return mode_; }

    /** What this disk accepts, which the tightest alignment across its registered media fixes. */
    [[nodiscard]] disk_capabilities_t capabilities() const noexcept {
        std::uint64_t memory = portable_alignment_k;
        std::uint64_t offset = 0;
        for (std::size_t index = 0; index != media_.size(); ++index) {
            registered_medium_slot_t const& slot = media_[index];
            if (slot.registered == 0) continue;
            memory = slot.memory_alignment > memory ? slot.memory_alignment : memory;
            offset = slot.offset_alignment > offset ? slot.offset_alignment : offset;
        }
        disk_capabilities_t reported;
        reported.memory_alignment = memory;
        reported.offset_alignment = offset != 0 ? offset : portable_alignment_k;
        reported.length_granularity = reported.offset_alignment;
        reported.max_transfer_bytes = max_transfer_bytes_;
        reported.depth_limit = depth_limit_;
        reported.media_limit = static_cast<std::uint16_t>(media_.size());
        reported.reaches_gpu_memory = true;
        return reported;
    }

#pragma region Queues

    /**
     *  @brief Checks that @p queue_pair names a cuFile queue, setting its batch up the first time
     *      any disk holds it.
     *  @param[in] queue_pair The queue the calling thread owns, which answers @c invalid_argument_k
     *      when it is of another transport, a blocking one included, or deeper than a batch.
     */
    status_t register_media(queue_ref_t const& queue_pair) noexcept {
        linux_cufile_queue_t* const queue = queue_of_(queue_pair);
        if (!queue) return {status_code_t::invalid_argument_k, "The queue is of another transport"};
        if (queue->batch_) return {};
        if (queue->depth_ > depth_limit_)
            return {status_code_t::invalid_argument_k, "A cuFile batch holds fewer requests than that"};
        if (api_->batch_setup(&queue->batch_, queue->depth_).error != cufile_success_k) {
            queue->batch_ = nullptr;
            return {status_code_t::device_k, "cuFile would not set a batch up"};
        }
        queue->batch_destroy_ = api_->batch_destroy;
        return {};
    }

    /** Requests @p queue_pair has submitted and not yet completed, which its credit bounds. */
    [[nodiscard]] std::size_t outstanding(queue_ref_t const& queue_pair) const noexcept {
        linux_cufile_queue_t const* const queue = queue_pair.template queue<linux_cufile_queue_t>();
        return queue ? outstanding_requests_(*queue) : 0;
    }

#pragma endregion Queues

#pragma region Registrations

    /**
     *  @brief Registers @p memory with cuFile, so requests can address it by slot.
     *  @param[in] memory Memory the caller owns and keeps alive until it deregisters the range, on the device whose
     *      context is current on this thread when it is @c gpu_k.
     *  @param[in] space Where that memory lives; this transport reaches both.
     */
    expected<registered_memory_t> register_memory(std::span<byte_t> memory, memory_space_t space) noexcept {
        if (memory.empty()) return status_t {status_code_t::invalid_argument_k, "An empty range registers nothing"};
        for (std::size_t index = 0; index != registered_memories_.size(); ++index) {
            registered_memory_slot_t& slot = registered_memories_[index];
            std::int32_t expected_free = 0;
            if (!claim_(slot.registered)
                     .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            // cuFile registers one pointer once, so a range over memory already registered shares that registration
            if (!registered_elsewhere_(memory.data(), index) &&
                api_->buffer_register(memory.data(), memory.size(), 0).error != cufile_success_k) {
                claim_(slot.registered).store(0, std::memory_order_release);
                return status_t {status_code_t::device_k, "cuFile would not register the range"};
            }
            slot.base = memory.data();
            slot.length = memory.size();
            slot.space = space;
            return registered_memory_t {disk_handle_t {static_cast<std::uint16_t>(index), slot.generation}, memory,
                                        space};
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every registered memory slot of this disk is taken"};
    }

    /**
     *  @brief Deregisters @p registered_memory, refusing while the library may still write into it.
     *  @param[in,out] registered_memory The range to give back, surrendered on success.
     */
    status_t unregister_memory(registered_memory_t& registered_memory) noexcept {
        registered_memory_slot_t* const slot = slot_of_(registered_memory);
        if (!slot) return {status_code_t::invalid_argument_k, "The registered memory is not this disk's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "The range is still a medium of requests in flight"};
        if (!registered_elsewhere_(slot->base, static_cast<std::size_t>(registered_memory.id()))) {
            [[maybe_unused]] cufile_error_t const deregistered = api_->buffer_deregister(slot->base);
        }
        ++slot->generation;
        slot->base = nullptr;
        slot->length = 0;
        claim_(slot->registered).store(0, std::memory_order_release);
        registered_memory.surrender();
        return {};
    }

    /**
     *  @brief Opens the file @p request names unbuffered and registers it with cuFile on the same descriptor.
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
            auto [file, open_status] = open_native_file(request.name, file_access_of(request.access),
                                                        file_buffering_t::unbuffered_k);
            if (open_status.failed()) {
                claim_(slot.registered).store(0, std::memory_order_release);
                return open_status;
            }
            status_t opened;
            if (request.access == medium_access_t::create_read_write_k && request.length != 0)
                opened = truncate_native_file(file, request.length);
            cufile_descriptor_t descriptor {};
            descriptor.type = cufile_handle_type_descriptor_k;
            descriptor.handle.file = file;
            if (opened.succeeded() && api_->handle_register(&slot.handle, &descriptor).error != cufile_success_k)
                opened = {status_code_t::device_k, "cuFile would not register the file"};
            if (opened.failed()) {
                [[maybe_unused]] status_t const closed = close_native_file(file);
                claim_(slot.registered).store(0, std::memory_order_release);
                return opened;
            }
            direct_alignment_t const alignment = direct_alignment_of(file);
            slot.file = file;
            slot.capacity = medium_bytes_of(file);
            slot.memory_alignment = alignment.memory > portable_alignment_k ? alignment.memory : portable_alignment_k;
            slot.offset_alignment = alignment.offset;
            return registered_medium_t {disk_handle_t {static_cast<std::uint16_t>(index), slot.generation},
                                        slot.capacity, alignment.offset};
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every medium slot of this disk is taken"};
    }

    /**
     *  @brief Deregisters and closes @p registered_medium, refusing while requests against it are still in flight.
     *  @param[in,out] registered_medium The medium to give back, surrendered on success.
     */
    status_t unregister_medium(registered_medium_t& registered_medium) noexcept {
        registered_medium_slot_t* const slot = slot_of_(registered_medium);
        if (!slot) return {status_code_t::invalid_argument_k, "The medium is not this disk's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "The medium is still named by requests in flight"};
        api_->handle_deregister(slot->handle);
        [[maybe_unused]] status_t const closed = close_native_file(slot->file);
        ++slot->generation;
        slot->handle = nullptr;
        slot->file = invalid_native_file_k;
        slot->capacity = 0;
        claim_(slot->registered).store(0, std::memory_order_release);
        registered_medium.surrender();
        return {};
    }

#pragma endregion Registrations

#pragma region Ring

    /**
     *  @brief Takes as much of @p requests as @p queue_pair still has credit for, and never waits on a transfer.
     *  @param[in] queue_pair The queue submitting, owned by the calling thread and held
     *      into this disk.
     *  @param[in] requests The batch, of which a prefix is taken.
     *  @return How many were taken, which is zero on a full queue and never a reason to block.
     *
     *  @note A request chained to its predecessor waits in its request_slot rather than in
     *      the library, and a request after a defect in the same chain is answered as
     *      cancelled right away.
     */
    expected<std::size_t> submit(queue_ref_t const& queue_pair, std::span<request_t const> requests) noexcept {
        for (request_t const& request : requests)
            if (request.token.queue_pair() != queue_pair.id())
                return status_t {status_code_t::invalid_argument_k, "A request holds another queue pair's token"};
        linux_cufile_queue_t* const queue = queue_of_(queue_pair);
        if (!queue) return status_t {status_code_t::invalid_argument_k, "The queue is of another transport"};
        assert(queue->batch_ && "Hold the disk into the queue before submitting");

        std::size_t const credit = queue->depth_ - outstanding_requests_(*queue);
        std::size_t const taking = requests.size() < credit ? requests.size() : credit;
        std::uint32_t linked = no_request_slot_k;
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
                linked = no_request_slot_k;
                chain_broken = chains_next_(request);
                continue;
            }
            std::uint32_t const request_slot = take_free_request_slot_(*queue);
            queue->submitted_requests_[request_slot].request = request;
            count_request_on_slots_(request);
            if (linked != no_request_slot_k) queue->submitted_requests_[linked].successor = request_slot;
            else stage_(*queue, request_slot);
            linked = chains_next_(request) ? request_slot : no_request_slot_k;
        }
        issue_(*queue);
        return taking;
    }

    /**
     *  @brief Copies what the library answered into @p into, and submits what that released.
     *  @param[in] queue_pair The queue polling, owned by the thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero when nothing has landed.
     */
    expected<std::size_t> poll_completions(queue_ref_t const& queue_pair, std::span<completion_t> into) noexcept {
        linux_cufile_queue_t* const queue = queue_of_(queue_pair);
        if (!queue) return status_t {status_code_t::invalid_argument_k, "The queue is of another transport"};

        std::size_t moved = 0;
        if (queue->outstanding_ != 0 && !into.empty()) {
            unsigned landed = static_cast<unsigned>(into.size() < queue->outstanding_ ? into.size()
                                                                                      : queue->outstanding_);
            cufile_time_limit_t none {};
            if (api_->batch_status(queue->batch_, 0, &landed, queue->events_.data(), &none).error != cufile_success_k)
                return status_t {status_code_t::device_k, "cuFile would not hand completions back"};
            for (unsigned index = 0; index != landed; ++index) {
                cufile_io_event_t const& event = queue->events_[index];
                std::uint32_t const request_slot = static_cast<std::uint32_t>(
                    reinterpret_cast<std::uintptr_t>(event.cookie));
                --queue->outstanding_;
                into[moved++] = retire_(*queue, request_slot,
                                        translate_(queue->submitted_requests_[request_slot].request, event));
            }
        }
        issue_(*queue);
        moved += drain_completions_made_by_queue_(*queue, into.subspan(moved));
        return moved;
    }

    /**
     *  @brief Answers everything @p queue_pair holds back from the library as cancelled.
     *  @param[in] queue_pair The queue to stop, whose accepted requests run to their end.
     *
     *  @note @c cuFileBatchIOCancel stops a whole batch for good, so what stops here are the requests still waiting on
     *      a predecessor and those a full batch left staged, as on the AIO disk.
     */
    void cancel(queue_ref_t const& queue_pair) noexcept {
        linux_cufile_queue_t* const queue = queue_of_(queue_pair);
        if (!queue) return;
        std::uint32_t const staged = queue->staged_count_;
        queue->staged_count_ = 0;
        for (std::uint32_t index = 0; index != staged; ++index) {
            std::uint32_t const request_slot = request_slot_of_(queue->staged_[index]);
            completion_t const cancelled = cancelled_completion_(
                queue->submitted_requests_[request_slot].request.token);
            push_completion_made_by_queue_(*queue, retire_(*queue, request_slot, cancelled));
        }
        for (std::uint32_t request_slot = 0; request_slot != queue->depth_; ++request_slot) {
            std::uint32_t const successor = queue->submitted_requests_[request_slot].successor;
            if (successor == no_request_slot_k) continue;
            queue->submitted_requests_[request_slot].successor = no_request_slot_k;
            cancel_linked_chain_(*queue, successor);
        }
    }

    /**
     *  @brief Finishes and discards everything @p queue_pair has outstanding, freeing its buffers.
     *  @param[in] queue_pair The queue to empty, which may be cancelled or still running.
     */
    status_t drain(queue_ref_t const& queue_pair) noexcept {
        linux_cufile_queue_t const* const queue = queue_of_(queue_pair);
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

    /** One range registered with cuFile, and the count of requests the library may still be writing into it. */
    struct registered_memory_slot_t {

        /** Zero while the slot is free, one while a range is registered in it. */
        std::int32_t registered = 0;

        /** Requests accepted against this range and not yet answered for. */
        std::int32_t in_flight = 0;

        /** Raised every time the slot is used again, so a stale place resolves to nothing. */
        queue_pair_generation_t generation = 0;

        /** Where the memory lives, device or host. */
        memory_space_t space = memory_space_t::host_k;

        /** First byte of the range, owned by the caller that registered it. */
        byte_t* base = nullptr;

        /** Bytes in the range. */
        std::uint64_t length = 0;
    };

    /** One open medium, its cuFile handle, its alignments, and the count of requests still naming it. */
    struct registered_medium_slot_t {

        /** Zero while the slot is free, one while a file is open in it. */
        std::int32_t registered = 0;

        /** Requests accepted against this medium and not yet answered for. */
        std::int32_t in_flight = 0;

        /** Raised every time the slot is used again, so a stale place resolves to nothing. */
        queue_pair_generation_t generation = 0;

        /** The open file, which a flush synchronizes. */
        native_file_t file = invalid_native_file_k;

        /** The file as cuFile registered it, which every request against this medium holds. */
        cufile_handle_t handle = nullptr;

        /** Bytes the medium held at registration. */
        std::uint64_t capacity = 0;

        /** Alignment a buffer's first byte must meet, the portable one at least, which device memory also needs. */
        std::uint64_t memory_alignment = portable_alignment_k;

        /** Alignment an extent's offset and its length must both meet, which @c O_DIRECT on the filesystem demands. */
        std::uint64_t offset_alignment = portable_alignment_k;
    };

    using submitted_request_t = linux_cufile_queue_t::submitted_request_t;

    static constexpr std::uint32_t no_request_slot_k = linux_cufile_queue_t::no_request_slot_k;

    /** The largest extent one request holds, which the driver's direct size may lower. */
    static constexpr std::uint64_t max_transfer_bytes_k = 1024ull * 1024ull * 1024ull;

    /** Completions one drain discards at a time. */
    static constexpr std::size_t drain_batch_k = 32;

    /** Fruitless polls a drain tolerates before it stops what is still held back. */
    static constexpr std::size_t drain_patience_k = 1024;

    /** Polls a drain performs before it calls the batch broken. */
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

    /** Opens the driver, which an earlier open of the process leaves open, and reads its properties. */
    [[nodiscard]] static bool open_driver_(cufile_api_t const& api, cufile_driver_properties_t& properties) noexcept {
        cufile_operation_error_t const opened = api.driver_open().error;
        if (opened != cufile_success_k && opened != cufile_driver_already_open_k) return false;
        return api.driver_properties(&properties).error == cufile_success_k;
    }

    /** How bytes move under @p properties: by DMA with @c nvidia_fs, through host memory where compat is allowed. */
    [[nodiscard]] static linux_cufile_probe_t mode_of_(cufile_driver_properties_t const& properties) noexcept {
        if (properties.nvfs.major_version != 0) return linux_cufile_probe_t::direct_k;
        return (properties.nvfs.control_flags & cufile_allow_compat_mode_flag_k) != 0 ? linux_cufile_probe_t::compat_k
                                                                                      : linux_cufile_probe_t::refused_k;
    }

    /** Gives every registration back; the queue sets' batches outlive the disk and are destroyed
     *  with their queues. */
    void close_all_() noexcept {
        for (std::size_t index = 0; index != registered_memories_.size(); ++index)
            if (registered_memories_[index].base) {
                [[maybe_unused]] cufile_error_t const deregistered = api_->buffer_deregister(
                    registered_memories_[index].base);
            }
        for (std::size_t index = 0; index != media_.size(); ++index) {
            if (media_[index].handle) api_->handle_deregister(media_[index].handle);
            if (media_[index].file != invalid_native_file_k) [[maybe_unused]]
                status_t const closed = close_native_file(media_[index].file);
        }
        registered_memories_.clear();
        media_.clear();
    }

#pragma region Bookkeeping

    /** Requests taken and not yet handed back: every occupied request_slot and every
     *  unreaped local completion. */
    [[nodiscard]] static std::size_t outstanding_requests_(linux_cufile_queue_t const& queue) noexcept {
        return (queue.depth_ - queue.free_request_slots_) + queue.completions_made_by_queue_count_;
    }

    /** The request_slot a staged request names through its cookie. */
    [[nodiscard]] static std::uint32_t request_slot_of_(cufile_io_parameters_t const& parameters) noexcept {
        return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(parameters.cookie));
    }

    [[nodiscard]] static linux_cufile_queue_t* queue_of_(queue_ref_t const& queue_pair) noexcept {
        return queue_pair.template queue<linux_cufile_queue_t>();
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

    /** The bytes @p place names: the submitting queue's own @p range for @c queue_range_k, else a
     *  registered range, or nothing. */
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

    /** Releases both once the library is done with the bytes of @p request. */
    void uncount_request_on_slots_(request_t const& request) noexcept {
        if (registered_memory_slot_t* const slot = resolve_(request.buffer); slot)
            in_flight_(*slot).store_sub(1, std::memory_order_release);
        if (registered_medium_slot_t* const slot = resolve_(request.medium); slot)
            in_flight_(*slot).store_sub(1, std::memory_order_release);
    }

    /** Whether a live slot other than @p skipped registered memory starting at @p base. */
    [[nodiscard]] bool registered_elsewhere_(byte_t const* base, std::size_t skipped) const noexcept {
        for (std::size_t index = 0; index != registered_memories_.size(); ++index)
            if (index != skipped && registered_memories_[index].base == base) return true;
        return false;
    }

    /** Pops a free request_slot of @p queue, which its credit guarantees there is. */
    static std::uint32_t take_free_request_slot_(linux_cufile_queue_t& queue) noexcept {
        return queue.request_slots_[--queue.free_request_slots_];
    }

    /** Pushes @p request_slot back onto @p queue's free stack, forgetting any request
     *  it held back. */
    static void free_request_slot_(linux_cufile_queue_t& queue, std::uint32_t request_slot) noexcept {
        queue.submitted_requests_[request_slot].successor = no_request_slot_k;
        queue.request_slots_[queue.free_request_slots_++] = request_slot;
    }

    /** Builds the request of @p request_slot into @p queue's staging window, behind everything
     *  staged before it. */
    void stage_(linux_cufile_queue_t& queue, std::uint32_t request_slot) noexcept {
        request_t const& request = queue.submitted_requests_[request_slot].request;
        cufile_io_parameters_t& parameters = queue.staged_[queue.staged_count_++];
        parameters = cufile_io_parameters_t {};
        parameters.mode = cufile_batch_mode_k;
        parameters.cookie = reinterpret_cast<void*>(static_cast<std::uintptr_t>(request_slot));
        if (request.kind == request_kind_t::flush_k || request.kind == request_kind_t::deallocate_k) return;
        // Compat mode drops a non-zero base offset, so the base points at the first byte and the offset stays zero
        parameters.batch.base = memory_of_(queue.pinned_staging_, request.buffer).data() + request.buffer.offset;
        parameters.batch.file_offset = static_cast<std::int64_t>(request.extent.offset);
        parameters.batch.size = static_cast<std::size_t>(request.extent.length);
        parameters.handle = resolve_(request.medium)->handle;
        parameters.opcode = request.kind == request_kind_t::read_k ? cufile_read_k : cufile_write_k;
    }

    /** Places @p completion at the back of @p queue's own completions, which poll drains after the
     *  library's. */
    static void push_completion_made_by_queue_(linux_cufile_queue_t& queue, completion_t const& completion) noexcept {
        queue.completions_made_by_queue_[(queue.completions_made_by_queue_first_ +
                                          queue.completions_made_by_queue_count_) %
                                         queue.depth_] = completion;
        ++queue.completions_made_by_queue_count_;
    }

    /** Moves what the disk answered itself into @p into, oldest first. */
    static std::size_t drain_completions_made_by_queue_(linux_cufile_queue_t& queue,
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
    completion_t retire_(linux_cufile_queue_t& queue, std::uint32_t request_slot,
                         completion_t const& completion) noexcept {
        submitted_request_t const& submitted = queue.submitted_requests_[request_slot];
        std::uint32_t const successor = submitted.successor;
        uncount_request_on_slots_(submitted.request);
        free_request_slot_(queue, request_slot);
        if (successor == no_request_slot_k) return completion;
        if (completion.completed_in_full()) stage_(queue, successor);
        else cancel_linked_chain_(queue, successor);
        return completion;
    }

    /** Answers the request in @p request_slot and every request held behind it as cancelled. */
    void cancel_linked_chain_(linux_cufile_queue_t& queue, std::uint32_t request_slot) noexcept {
        while (request_slot != no_request_slot_k) {
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

    /** The completion a request gets when the disk or the library refused it before it started. */
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

    /** Whatever would corrupt a batch entry rather than merely fail on the device, @p range being
     *  the submitting queue's own blocks. */
    status_t inspect_(std::span<byte_t> range, request_t const& request) noexcept {
        registered_medium_slot_t const* const medium = resolve_(request.medium);
        if (!medium) return {status_code_t::invalid_argument_k, "The request names no registered medium"};
        if (request.kind == request_kind_t::flush_k || request.kind == request_kind_t::deallocate_k) return {};
        std::span<byte_t> const memory = memory_of_(range, request.buffer);
        if (memory.empty()) return {status_code_t::invalid_argument_k, "The request names no registered range"};
        if (request.buffer.offset > memory.size() || request.extent.length > memory.size() - request.buffer.offset)
            return {status_code_t::invalid_argument_k, "The request runs past its registered range"};
        if (request.extent.length > max_transfer_bytes_)
            return {status_code_t::invalid_argument_k, "The request is larger than one transfer"};
        byte_t const* const address = memory.data() + request.buffer.offset;
        if (!is_address_aligned(address, medium->memory_alignment) ||
            !request.extent.aligned_to(medium->offset_alignment))
            return {status_code_t::invalid_argument_k, "The request does not meet the disk's alignment"};
        return {};
    }

    /**
     *  @brief Hands the staged requests to the library in one batch, answering every flush and
     *      deallocation here first.
     *
     *  A batch the library refuses as full stays staged for the next poll. Any other refusal belongs to the batch as a
     *  whole, so every staged request fails, and the chains behind them are cut.
     */
    void issue_(linux_cufile_queue_t& queue) noexcept {
        cufile_io_parameters_t* const staged = queue.staged_.data();
        std::uint32_t kept = 0;
        for (std::uint32_t index = 0; index != queue.staged_count_; ++index) {
            std::uint32_t const request_slot = request_slot_of_(staged[index]);
            request_t const& request = queue.submitted_requests_[request_slot].request;
            if (request.kind != request_kind_t::flush_k && request.kind != request_kind_t::deallocate_k) {
                staged[kept++] = staged[index];
                continue;
            }
            native_file_t const file = resolve_(request.medium)->file;
            status_t const flushed = request.kind == request_kind_t::flush_k
                                         ? flush_native_file(file)
                                         : deallocate_native_file(file, request.extent);
            completion_t const completion = flushed.failed() ? failed_completion_(request.token, flushed)
                                                             : flushed_completion_(request.token);
            push_completion_made_by_queue_(queue, retire_(queue, request_slot, completion));
        }
        queue.staged_count_ = kept;
        if (queue.staged_count_ == 0) return;
        cufile_operation_error_t const submitted =
            api_->batch_submit(queue.batch_, queue.staged_count_, staged, 0).error;
        if (submitted == cufile_batch_full_k) return;
        std::uint32_t const count = queue.staged_count_;
        queue.staged_count_ = 0;
        if (submitted == cufile_success_k) {
            queue.outstanding_ += count;
            return;
        }
        status_t const refused {status_code_t::device_k, "cuFile refused the batch"};
        for (std::uint32_t index = 0; index != count; ++index) {
            std::uint32_t const request_slot = request_slot_of_(staged[index]);
            push_completion_made_by_queue_(
                queue, retire_(queue, request_slot,
                               failed_completion_(queue.submitted_requests_[request_slot].request.token, refused)));
        }
    }

    /** The completion of a flush that synchronized its file, or a deallocation that gave
     *  its extent back. */
    [[nodiscard]] static completion_t flushed_completion_(request_token_t token) noexcept {
        completion_t completion;
        completion.token = token;
        completion.state = completion_state_t::completed_k;
        return completion;
    }

    /** Turns one event into the completion its queue reads, by the status and the result the
     *  library gave. */
    [[nodiscard]] static completion_t translate_(request_t const& request, cufile_io_event_t const& event) noexcept {
        completion_t completion;
        completion.token = request.token;
        std::int64_t const result = static_cast<std::int64_t>(event.result);
        if (event.status == cufile_complete_k && result >= 0) {
            completion.transferred_bytes = static_cast<std::uint64_t>(result);
            completion.state = completion.transferred_bytes == request.extent.length ? completion_state_t::completed_k
                                                                                     : completion_state_t::partial_k;
            return completion;
        }
        if (event.status == cufile_canceled_k) return cancelled_completion_(request.token);
        completion.state = completion_state_t::failed_k;
        completion.status = result < 0 && -result < cufile_error_base_k
                                ? native_file_failure(static_cast<int>(-result), "The device refused this request")
                                : status_t {status_code_t::device_k, "cuFile failed this request"};
        return completion;
    }

#pragma endregion Requests

    /** The library's entry points, viewed from the process's GPU drivers. */
    cufile_api_t const* api_ = nullptr;

    /** One slot per memory range the disk registers. */
    st::vector<registered_memory_slot_t> registered_memories_;

    /** One slot per medium the disk opens. */
    st::vector<registered_medium_slot_t> media_;

    /** Requests one batch holds at most, the driver's @c max_batch_io_size, which bounds a held
     *  queue's depth. */
    queue_depth_t depth_limit_ = 0;

    /** The largest extent one request holds. */
    std::uint64_t max_transfer_bytes_ = max_transfer_bytes_k;

    /** How the disk moves bytes, settled when the driver opened. */
    linux_cufile_probe_t mode_ = linux_cufile_probe_t::refused_k;
};

#pragma endregion Disk

#else

#pragma region Absent

/** The queue on a platform without cuFile, which a queue set never makes. */
class linux_cufile_queue_t {
  public:

    /** Refuses the queue, since this platform has no batch to set up. */
    static expected<linux_cufile_queue_t> make(queue_depth_t, std::span<byte_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Requests the queue keeps in flight, which is none. */
    [[nodiscard]] queue_depth_t depth() const noexcept { return 0; }
};

/**
 *  @brief The same disk on a platform without cuFile, which mounts nowhere and refuses cleanly.
 *
 *  The type exists so a caller compiles against it unconditionally and chooses at mount, where a refused @c make keeps
 *  device reads on the host transport rather than behind a build-time branch.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_cufile_files_disk {
  public:

    /** The queue a thread owns for this transport. */
    using queue_t = linux_cufile_queue_t;

    /** Answers that this platform has no cuFile to offer. */
    [[nodiscard]] static linux_cufile_probe_t probe(cufile_api_t const&) noexcept {
        return linux_cufile_probe_t::absent_k;
    }

    /** Refuses the mount, since this platform has no cuFile to mount on. */
    static expected<linux_cufile_files_disk> make(cufile_api_t const&, linux_cufile_disk_config_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Answers that nothing moves, since nothing was mounted. */
    [[nodiscard]] linux_cufile_probe_t mode() const noexcept { return linux_cufile_probe_t::absent_k; }

    /** What this disk would accept, which is nothing at all. */
    [[nodiscard]] disk_capabilities_t capabilities() const noexcept { return {}; }

    /** Refuses the registration, since there is no batch to register into. */
    status_t register_media(queue_ref_t const&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Refuses the range, since no batch would ever address it. */
    expected<registered_memory_t> register_memory(std::span<byte_t>, memory_space_t) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Refuses the range, since none was ever registered. */
    status_t unregister_memory(registered_memory_t&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Refuses the medium, since no batch would ever address it. */
    expected<registered_medium_t> register_medium(medium_options_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Refuses the medium, since none was ever opened. */
    status_t unregister_medium(registered_medium_t&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Takes nothing, since there is no batch to take it into. */
    expected<std::size_t> submit(queue_ref_t const&, std::span<request_t const>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Answers nothing, since nothing was ever submitted. */
    expected<std::size_t> poll_completions(queue_ref_t const&, std::span<completion_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Stops nothing, since nothing was ever submitted. */
    void cancel(queue_ref_t const&) noexcept {}

    /** Refuses the drain, since no queue was ever made. */
    status_t drain(queue_ref_t const&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Answers that nothing is outstanding, which is always true here. */
    [[nodiscard]] std::size_t outstanding(queue_ref_t const&) const noexcept { return 0; }
};

#pragma endregion Absent

#endif

} // namespace unum::ustore::utilities
