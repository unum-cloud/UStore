/**
 *  @file include/ustore/utilities/nvme_linux_cufile_disk.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The ring NVIDIA's cuFile offers: @c O_DIRECT files read into and written from device or host memory.
 *
 *  Nothing here links cuFile or includes its headers. @c cufile_api_t opens @c libcufile.so.0 once, as the process's
 *  GPU drivers load it and hand it to every disk, and the few structures it passes are declared here, so every build
 *  compiles this disk and only a machine with the library mounts it; release 1.19 and later need no @c libcuda of
 *  their own. A queue pair holds one batch of @c cuFileBatchIOSetUp sized to the mount's depth, @c submit hands staged
 *  requests to @c cuFileBatchIOSubmit, and @c poll_completions asks @c cuFileBatchIOGetStatus for whatever landed with
 *  a minimum of zero, so neither waits.
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

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int64_t`, `std::uint64_t`

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/blocking_files.hpp" // `shared_library_t`
#include "ustore/utilities/nvme_disks.hpp"     // `flush_native_file`, `deallocate_native_file`, `direct_alignment_of`
#include "ustore/utilities/nvme_ring.hpp" // `request_t`, `completion_t`, `queue_pair_t`, `portable_alignment_k`, `is_address_aligned`
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

/** The @c CUfileStatus_t values an event carries. */
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

/** What a mount fixes about a cuFile disk before any queue pair takes a share of it. */
struct linux_cufile_disk_config_t {

    /** Leases the disk hands out at once, which is also the number of batches it sets up. */
    std::uint16_t queue_pairs_limit = 1;

    /** Requests one queue pair may keep in flight, at most the driver's @c max_batch_io_size. */
    queue_depth_t depth_limit = 64;

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

#pragma region Disk

/**
 *  @brief cuFile over @c O_DIRECT files registered as media, one batch per queue pair, reaching device memory.
 *
 *  As with the AIO disk, the queue is the library's and only the bookkeeping is the disk's own. A defect the disk can
 *  see before the library does fails that one request's completion and leaves its neighbours alone; a batch the library
 *  refuses whole stays staged when it is full and fails its requests otherwise. Only a token that is not this queue
 *  pair's fails the whole @c submit.
 *
 *  @tparam atomic_reference_ The reference the claim and in-flight words are owned through.
 *  @tparam waiter_type_ What a draining core does after a poll that moved nothing.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_cufile_files_disk {
  public:

    linux_cufile_files_disk() noexcept = default;
    linux_cufile_files_disk(linux_cufile_files_disk const&) = delete;
    linux_cufile_files_disk& operator=(linux_cufile_files_disk const&) = delete;
    linux_cufile_files_disk(linux_cufile_files_disk&&) noexcept = default;

    linux_cufile_files_disk& operator=(linux_cufile_files_disk&& other) noexcept {
        if (this == &other) return *this;
        close_all_();
        api_ = other.api_;
        queue_pairs_ = std::move(other.queue_pairs_);
        registered_memories_ = std::move(other.registered_memories_);
        media_ = std::move(other.media_);
        flights_ = std::move(other.flights_);
        seats_ = std::move(other.seats_);
        staged_ = std::move(other.staged_);
        events_ = std::move(other.events_);
        locals_ = std::move(other.locals_);
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
     *  @brief Opens the driver through @p api, which outlives the disk, in the mode @p config asks for, and sets one
     *      batch up per queue pair.
     *  @param[in] config Leases, per-queue pair depth, the ranges and media, and the GPUDirect Storage mode.
     *  @retval unsupported_k The machine has no @c libcufile.
     *  @retval device_k The driver would not open, @c ustore_gds_direct_k found no @c nvidia_fs, or compat mode is both
     *      needed and disallowed.
     */
    static expected<linux_cufile_files_disk> make(cufile_api_t const& api,
                                                  linux_cufile_disk_config_t const& config) noexcept {
        if (config.queue_pairs_limit == 0 || config.depth_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A batch needs a queue pair and a request of depth"};
        if (config.queue_pairs_limit == static_cast<std::uint16_t>(queue_pair_id_t::unallocated_k) ||
            config.registered_memory_limit == static_cast<std::uint16_t>(registered_memory_id_t::unregistered_k) ||
            config.media_limit == static_cast<std::uint16_t>(registered_medium_id_t::unregistered_k))
            return status_t {status_code_t::invalid_argument_k, "A disk keeps one identifier for an empty handle"};
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
        if (config.depth_limit > properties.max_batch_io_size)
            return status_t {status_code_t::invalid_argument_k, "A cuFile batch holds fewer requests than that"};

        std::size_t const window = static_cast<std::size_t>(config.queue_pairs_limit) * config.depth_limit;
        disk.depth_limit_ = config.depth_limit;
        std::uint64_t const direct_bytes = static_cast<std::uint64_t>(properties.nvfs.max_direct_io_size) * 1024u;
        disk.max_transfer_bytes_ = direct_bytes != 0 && direct_bytes < max_transfer_bytes_k ? direct_bytes
                                                                                            : max_transfer_bytes_k;
        if (status_t const sized = size_(disk.queue_pairs_, config.queue_pairs_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.registered_memories_, config.registered_memory_limit); sized.failed())
            return sized;
        if (status_t const sized = size_(disk.media_, config.media_limit); sized.failed()) return sized;
        if (status_t const sized = size_(disk.flights_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.seats_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.staged_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.events_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.locals_, window); sized.failed()) return sized;
        for (std::size_t index = 0; index != disk.queue_pairs_.size(); ++index)
            if (api.batch_setup(&disk.queue_pairs_[index].batch, config.depth_limit).error != cufile_success_k)
                return status_t {status_code_t::device_k, "cuFile would not set a batch up"};
        return disk;
    }

    /** How this disk moves bytes, @c direct_k or @c compat_k once made. */
    [[nodiscard]] linux_cufile_probe_t mode() const noexcept { return mode_; }

    /** What this disk accepts, which the tightest alignment across its registered media fixes. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept {
        std::uint64_t memory = portable_alignment_k;
        std::uint64_t offset = 0;
        for (std::size_t index = 0; index != media_.size(); ++index) {
            registered_medium_slot_t const& slot = media_[index];
            if (slot.registered == 0) continue;
            memory = slot.memory_alignment > memory ? slot.memory_alignment : memory;
            offset = slot.offset_alignment > offset ? slot.offset_alignment : offset;
        }
        ring_capabilities_t reported;
        reported.memory_alignment = memory;
        reported.offset_alignment = offset != 0 ? offset : portable_alignment_k;
        reported.length_granularity = reported.offset_alignment;
        reported.max_transfer_bytes = max_transfer_bytes_;
        reported.depth_limit = depth_limit_;
        reported.queue_pairs_limit = static_cast<std::uint16_t>(queue_pairs_.size());
        reported.media_limit = static_cast<std::uint16_t>(media_.size());
        reported.reaches_gpu_memory = true;
        return reported;
    }

#pragma region Queue Pairs

    /**
     *  @brief Takes a queue pair on @p depth requests of one batch for the calling thread.
     *  @param[in] depth Requests this queue pair may keep in flight, at most the mount's limit.
     *  @return The queue pair, or @c capacity_exhausted_k when every batch is already out.
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
            return queue_pair_t {disk_handle_t {static_cast<std::uint16_t>(index), slot.generation}, depth};
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every queue pair of this disk is already held"};
    }

    /**
     *  @brief Binds @p queue_pair to the calling thread, which is how a moved one changes hands.
     *  @param[in] queue_pair The queue pair this thread has taken over, with nothing outstanding.
     *
     *  @note A batch answers any thread of the process, so nothing is rebuilt.
     */
    status_t bind_queue_pair(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this disk's"};
        if (outstanding_(*slot) != 0)
            return {status_code_t::invalid_argument_k, "Drain the queue pair before handing it over"};
        return {};
    }

    /**
     *  @brief Takes @p queue_pair back once it has nothing outstanding, and empties the handle.
     *  @param[in,out] queue_pair The queue pair to return, surrendered on success only.
     */
    status_t release_queue_pair(queue_pair_t& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this disk's"};
        if (outstanding_(*slot) != 0)
            return {status_code_t::invalid_argument_k, "Drain the queue pair before releasing it"};
        ++slot->generation;
        slot->depth = 0;
        slot->free_seats = 0;
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
            auto [file, open_native_file_status] = open_native_file(request.name, request.access,
                                                                    file_buffering_t::unbuffered_k);
            if (open_native_file_status.failed()) {
                claim_(slot.registered).store(0, std::memory_order_release);
                return open_native_file_status;
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
                close_native_file(file);
                claim_(slot.registered).store(0, std::memory_order_release);
                return opened;
            }
            direct_alignment_t const alignment = direct_alignment_of(file);
            slot.file = file;
            slot.capacity = native_file_capacity(file);
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
        close_native_file(slot->file);
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
     *  @param[in] queue_pair The queue pair submitting, held by the calling thread.
     *  @param[in] requests The batch, of which a prefix is taken.
     *  @return How many were taken, which is zero on a full queue pair and never a reason to block.
     *
     *  @note A request chained to its predecessor waits in its seat rather than in the library, and a request after a
     *      defect in the same chain is answered as cancelled right away.
     */
    expected<std::size_t> submit(queue_pair_t const& queue_pair, std::span<request_t const> requests) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The queue pair is not this disk's"};
        for (request_t const& request : requests)
            if (request.token.queue_pair() != queue_pair.id() || request.token.generation() != queue_pair.generation())
                return status_t {status_code_t::invalid_argument_k, "A request carries another queue pair's token"};

        std::uint32_t const base = window_of_(queue_pair.id());
        std::size_t const credit = slot->depth - outstanding_(*slot);
        std::size_t const taking = requests.size() < credit ? requests.size() : credit;
        std::uint32_t linked = no_seat_k;
        bool chain_broken = false;
        for (std::size_t index = 0; index != taking; ++index) {
            request_t const& request = requests[index];
            if (chain_broken) {
                push_local_(*slot, base, cancelled_completion_(request.token));
                chain_broken = chains_next_(request);
                continue;
            }
            status_t const defect = inspect_(request);
            if (defect.failed()) {
                push_local_(*slot, base, failed_completion_(request.token, defect));
                linked = no_seat_k;
                chain_broken = chains_next_(request);
                continue;
            }
            std::uint32_t const seat = take_seat_(*slot, base);
            flights_[base + seat].request = request;
            borrow_(request);
            if (linked != no_seat_k) flights_[base + linked].successor = seat;
            else stage_(*slot, base, seat);
            linked = chains_next_(request) ? seat : no_seat_k;
        }
        issue_(*slot, base);
        return taking;
    }

    /**
     *  @brief Copies what the library answered into @p into, and submits what that released.
     *  @param[in] queue_pair The queue pair polling, held by the thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero when nothing has landed.
     */
    expected<std::size_t> poll_completions(queue_pair_t const& queue_pair, std::span<completion_t> into) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The queue pair is not this disk's"};
        std::uint32_t const base = window_of_(queue_pair.id());

        std::size_t moved = 0;
        if (slot->flights_count != 0 && !into.empty()) {
            unsigned landed = static_cast<unsigned>(into.size() < slot->flights_count ? into.size()
                                                                                      : slot->flights_count);
            cufile_time_limit_t none {};
            if (api_->batch_status(slot->batch, 0, &landed, &events_[base], &none).error != cufile_success_k)
                return status_t {status_code_t::device_k, "cuFile would not hand completions back"};
            for (unsigned index = 0; index != landed; ++index) {
                cufile_io_event_t const& event = events_[base + index];
                std::uint32_t const seat = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(event.cookie));
                --slot->flights_count;
                into[moved++] = retire_(*slot, base, seat, translate_(flights_[base + seat].request, event));
            }
        }
        issue_(*slot, base);
        moved += drain_locals_(*slot, base, into.subspan(moved));
        return moved;
    }

    /**
     *  @brief Answers everything @p queue_pair holds back from the library as cancelled.
     *  @param[in] queue_pair The queue pair to stop, whose accepted requests run to their end.
     *
     *  @note @c cuFileBatchIOCancel stops a whole batch for good, so what stops here are the requests still waiting on
     *      a predecessor and those a full batch left staged, as on the AIO disk.
     */
    void cancel(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return;
        std::uint32_t const base = window_of_(queue_pair.id());
        std::uint32_t const staged = slot->staged_count;
        slot->staged_count = 0;
        for (std::uint32_t index = 0; index != staged; ++index) {
            std::uint32_t const seat = seat_of_(staged_[base + index]);
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
            waiter_type_ {}(&spins, spins, std::size_t {0});
        }
        return {status_code_t::io_k, "The queue pair would not drain"};
    }

#pragma endregion Ring

  private:

    /** One queue pair's share of the disk: its batch, its seats, and the queues it polls from. */
    struct queue_pair_slot_t {

        /** Zero while the slot is free, one while a queue pair holds it. */
        std::int32_t leased = 0;

        /** Raised every time the slot is leased again, so a stale token routes nowhere. */
        queue_pair_generation_t generation = 0;

        /** Requests this queue pair may keep in flight, fixed when the queue pair was taken. */
        queue_depth_t depth = 0;

        /** The library's batch, set up at mount for the mount's whole depth and kept across leases. */
        cufile_batch_handle_t batch = nullptr;

        /** Seats no request occupies, stacked at the front of this queue pair's seat window. */
        std::uint32_t free_seats = 0;

        /** Requests built and not yet accepted, in order at the front of this queue pair's staging window. */
        std::uint32_t staged_count = 0;

        /** Requests the library accepted and has not yet answered for. */
        std::uint32_t flights_count = 0;

        /** Where this queue pair's oldest unreaped local completion sits inside its window. */
        std::uint32_t locals_first = 0;

        /** Completions the disk answered itself, which no event ever carried. */
        std::uint32_t locals_count = 0;
    };

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

        /** The file as cuFile registered it, which every request against this medium carries. */
        cufile_handle_t handle = nullptr;

        /** Bytes the medium held at registration. */
        std::uint64_t capacity = 0;

        /** Alignment a buffer's first byte must meet, the portable one at least, which device memory also needs. */
        std::uint64_t memory_alignment = portable_alignment_k;

        /** Alignment an extent's offset and its length must both meet, which @c O_DIRECT on the filesystem demands. */
        std::uint64_t offset_alignment = portable_alignment_k;
    };

    /** One request from the moment it is taken until its completion is handed back, in a seat of its queue pair. */
    struct flight_t {

        /** The request as it was taken, since an event carries only the seat and the result. */
        request_t request;

        /** The seat of the request held back until this one transfers in full, or @c no_seat_k. */
        std::uint32_t successor = no_seat_k;
    };

    /** The seat that names no request, which ends a chain. */
    static constexpr std::uint32_t no_seat_k = ~std::uint32_t {0};

    /** The largest extent one request carries, which the driver's direct size may lower. */
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

    /** Destroys every batch, which waits out whatever the library still holds, then gives every registration back. */
    void close_all_() noexcept {
        for (std::size_t index = 0; index != queue_pairs_.size(); ++index)
            if (queue_pairs_[index].batch) api_->batch_destroy(queue_pairs_[index].batch);
        for (std::size_t index = 0; index != registered_memories_.size(); ++index)
            if (registered_memories_[index].base) {
                [[maybe_unused]] cufile_error_t const deregistered = api_->buffer_deregister(
                    registered_memories_[index].base);
            }
        for (std::size_t index = 0; index != media_.size(); ++index) {
            if (media_[index].handle) api_->handle_deregister(media_[index].handle);
            if (media_[index].file != invalid_native_file_k) close_native_file(media_[index].file);
        }
        queue_pairs_.clear();
        registered_memories_.clear();
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

    /** The seat a staged request names through its cookie. */
    [[nodiscard]] static std::uint32_t seat_of_(cufile_io_parameters_t const& parameters) noexcept {
        return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(parameters.cookie));
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

    /** Releases both once the library is done with the bytes of @p request. */
    void restore_(request_t const& request) noexcept {
        if (registered_memory_slot_t* const slot = resolve_(request.buffer); slot)
            in_flight_(*slot).sub(1, std::memory_order_release);
        if (registered_medium_slot_t* const slot = resolve_(request.medium); slot)
            in_flight_(*slot).sub(1, std::memory_order_release);
    }

    /** Whether a live slot other than @p skipped registered memory starting at @p base. */
    [[nodiscard]] bool registered_elsewhere_(byte_t const* base, std::size_t skipped) const noexcept {
        for (std::size_t index = 0; index != registered_memories_.size(); ++index)
            if (index != skipped && registered_memories_[index].base == base) return true;
        return false;
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

    /** Builds the request of @p seat into the staging window, behind everything staged before it. */
    void stage_(queue_pair_slot_t& slot, std::uint32_t base, std::uint32_t seat) noexcept {
        request_t const& request = flights_[base + seat].request;
        cufile_io_parameters_t& parameters = staged_[base + slot.staged_count++];
        parameters = cufile_io_parameters_t {};
        parameters.mode = cufile_batch_mode_k;
        parameters.cookie = reinterpret_cast<void*>(static_cast<std::uintptr_t>(seat));
        if (request.kind == request_kind_t::flush_k || request.kind == request_kind_t::deallocate_k) return;
        // Compat mode drops a non-zero base offset, so the base points at the first byte and the offset stays zero
        registered_memory_slot_t const* const memory = resolve_(request.buffer);
        parameters.batch.base = memory->base + request.buffer.offset;
        parameters.batch.file_offset = static_cast<std::int64_t>(request.extent.offset);
        parameters.batch.size = static_cast<std::size_t>(request.extent.length);
        parameters.handle = resolve_(request.medium)->handle;
        parameters.opcode = request.kind == request_kind_t::read_k ? cufile_read_k : cufile_write_k;
    }

    /** Places @p completion at the back of @p slot's own queue, which poll drains after the library's. */
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

    /** Whatever would corrupt a batch entry rather than merely fail on the device. */
    status_t inspect_(request_t const& request) noexcept {
        registered_medium_slot_t const* const medium = resolve_(request.medium);
        if (!medium) return {status_code_t::invalid_argument_k, "The request names no registered medium"};
        if (request.kind == request_kind_t::flush_k || request.kind == request_kind_t::deallocate_k) return {};
        registered_memory_slot_t const* const memory = resolve_(request.buffer);
        if (!memory) return {status_code_t::invalid_argument_k, "The request names no registered range"};
        if (request.buffer.offset > memory->length || request.extent.length > memory->length - request.buffer.offset)
            return {status_code_t::invalid_argument_k, "The request runs past its registered range"};
        if (request.extent.length > max_transfer_bytes_)
            return {status_code_t::invalid_argument_k, "The request is larger than one transfer"};
        byte_t const* const address = memory->base + request.buffer.offset;
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
    void issue_(queue_pair_slot_t& slot, std::uint32_t base) noexcept {
        cufile_io_parameters_t* const staged = &staged_[base];
        std::uint32_t kept = 0;
        for (std::uint32_t index = 0; index != slot.staged_count; ++index) {
            std::uint32_t const seat = seat_of_(staged[index]);
            request_t const& request = flights_[base + seat].request;
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
            push_local_(slot, base, retire_(slot, base, seat, completion));
        }
        slot.staged_count = kept;
        if (slot.staged_count == 0) return;
        cufile_operation_error_t const submitted = api_->batch_submit(slot.batch, slot.staged_count, staged, 0).error;
        if (submitted == cufile_batch_full_k) return;
        std::uint32_t const count = slot.staged_count;
        slot.staged_count = 0;
        if (submitted == cufile_success_k) {
            slot.flights_count += count;
            return;
        }
        status_t const refused {status_code_t::device_k, "cuFile refused the batch"};
        for (std::uint32_t index = 0; index != count; ++index) {
            std::uint32_t const seat = seat_of_(staged[index]);
            push_local_(slot, base,
                        retire_(slot, base, seat, failed_completion_(flights_[base + seat].request.token, refused)));
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

    /** Turns one event into the completion its queue pair reads, by the status and the result the library gave. */
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

    /** The library's entry points, borrowed from the process's GPU drivers. */
    cufile_api_t const* api_ = nullptr;

    /** One slot per queue pair the disk hands out, each with its own batch. */
    st::vector<queue_pair_slot_t> queue_pairs_;

    /** One slot per memory range the disk registers. */
    st::vector<registered_memory_slot_t> registered_memories_;

    /** One slot per medium the disk opens. */
    st::vector<registered_medium_slot_t> media_;

    /** Every queue pair's seats, in windows of @c depth_limit_. */
    st::vector<flight_t> flights_;

    /** Every queue pair's free seats, stacked in windows of @c depth_limit_. */
    st::vector<std::uint32_t> seats_;

    /** Every queue pair's requests awaiting @c cuFileBatchIOSubmit, contiguous as it reads them, in windows of
     *  @c depth_limit_. */
    st::vector<cufile_io_parameters_t> staged_;

    /** Every queue pair's room for the events one status query copies out, in windows of @c depth_limit_. */
    st::vector<cufile_io_event_t> events_;

    /** Every queue pair's own completions, in windows of @c depth_limit_ read as a circle. */
    st::vector<completion_t> locals_;

    /** Requests one queue pair may keep in flight, the depth of every batch, and the stride of every window. */
    queue_depth_t depth_limit_ = 0;

    /** The largest extent one request carries. */
    std::uint64_t max_transfer_bytes_ = max_transfer_bytes_k;

    /** How the disk moves bytes, settled when the driver opened. */
    linux_cufile_probe_t mode_ = linux_cufile_probe_t::refused_k;
};

#pragma endregion Disk

#else

#pragma region Absent

/**
 *  @brief The same disk on a platform without cuFile, which mounts nowhere and refuses cleanly.
 *
 *  The type exists so a caller compiles against it unconditionally and chooses at mount, where a refused @c make keeps
 *  device reads on the host transport rather than behind a build-time branch.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_cufile_files_disk {
  public:

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
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept { return {}; }

    /** Refuses the queue pair, since there is no batch to take a share of. */
    expected<queue_pair_t> acquire_queue_pair(std::uint32_t) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Refuses the handover, since no queue pair was ever handed out. */
    status_t bind_queue_pair(queue_pair_t const&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Refuses the return, since no queue pair was ever handed out. */
    status_t release_queue_pair(queue_pair_t&) noexcept {
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
    expected<std::size_t> submit(queue_pair_t const&, std::span<request_t const>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Answers nothing, since nothing was ever submitted. */
    expected<std::size_t> poll_completions(queue_pair_t const&, std::span<completion_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Stops nothing, since nothing was ever submitted. */
    void cancel(queue_pair_t const&) noexcept {}

    /** Refuses the drain, since no queue pair was ever handed out. */
    status_t drain(queue_pair_t const&) noexcept {
        return {status_code_t::unsupported_k, "This platform has no cuFile"};
    }

    /** Answers that nothing is outstanding, which is always true here. */
    [[nodiscard]] std::size_t outstanding(queue_pair_t const&) const noexcept { return 0; }
};

#pragma endregion Absent

#endif

} // namespace unum::ustore::utilities
