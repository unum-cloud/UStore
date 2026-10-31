/**
 *  @file include/ustore/utilities/vram_amd_device.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief AMD devices from the host: the HIP runtime opened at first use, the device listing, modules, events and the
 *      executor calls on one device queue through.
 *
 *  The library never links @c libamdhip64: @c hip_api_t::load opens it and resolves the entry points UStore calls into
 *  one table, which stays empty where the runtime is absent, so AMD then lists no devices and the binary still loads.
 *  The few runtime types, constants and prototypes UStore calls are spelled here, copied from HIP's stable ABI, so any
 *  host compiler builds it. Kernels reach a device as code objects compiled ahead of time and embedded in the library,
 *  loaded by @c hip_module_t and launched by name.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`

#include <span>    // `std::span`
#include <utility> // `std::exchange`, `std::swap`

#include <stringzilla/capabilities.h> // `sz_capability_t`

#include "ustore/utilities/blocking_files.hpp" // `shared_library_t`
#include "ustore/utilities/ram_buffers.hpp"    // `pinned_bounce`, `managed_blocks_t`, `free_blocks`
#include "ustore/utilities/types.hpp"          // `gpu_device_t`, `gpu_class_t`, `expected`, `status_t`

namespace unum::ustore::utilities {

#pragma region Runtime ABI

/** The runtime's status word, @c hipError_t, naming the values UStore tells apart; any other is a failure. */
enum hip_result_t : int {
    hip_success_k = 0,
    hip_error_not_initialized_k = 3,
};

/** The runtime's opaque handles: @c hipStream_t, @c hipEvent_t, @c hipModule_t and @c hipFunction_t. */
using hip_stream_handle_t = struct hip_stream_opaque_t*;
using hip_event_handle_t = struct hip_event_opaque_t*;
using hip_module_handle_t = struct hip_module_opaque_t*;
using hip_function_handle_t = struct hip_function_opaque_t*;

/** The @c hipDeviceAttribute_t values UStore queries; on AMD the compute capability is the @c gfx processor's major
 *  and minor version. */
enum hip_device_attribute_t : int {
    hip_attribute_compute_capability_major_k = 23,
    hip_attribute_compute_capability_minor_k = 61,
    hip_attribute_multiprocessor_count_k = 63,
    hip_attribute_host_numa_id_k = 92,
};

/** The flags UStore passes, @c hipStreamNonBlocking, @c hipEventBlockingSync and @c hipEventDisableTiming, and the
 *  @c hipPointer_attribute values it reads. */
inline constexpr unsigned hip_stream_non_blocking_k = 1;
inline constexpr unsigned hip_event_blocking_sync_k = 1;
inline constexpr unsigned hip_event_disable_timing_k = 2;
inline constexpr int hip_pointer_memory_type_k = 2;
inline constexpr int hip_pointer_is_managed_k = 8;
inline constexpr int hip_pointer_device_ordinal_k = 9;
inline constexpr unsigned hip_memory_type_device_k = 2;

/** The @c hipMemAttachGlobal flag of managed allocations, which any stream on any device reaches. */
inline constexpr unsigned hip_attach_global_k = 1;

#pragma endregion Runtime ABI

/**
 *  @brief The HIP entry points UStore calls, resolved from @c libamdhip64 by @c load.
 *
 *  Every entry is null when the library is absent or lacks any of them, which leaves AMD with no devices; each spells
 *  the C prototype of the exported symbol it is resolved by.
 */
struct hip_api_t {
    hip_result_t (*get_device_count)(int*) = nullptr;
    hip_result_t (*get_device)(int*, int) = nullptr;
    hip_result_t (*get_device_attribute)(int*, hip_device_attribute_t, int) = nullptr;
    hip_result_t (*get_device_name)(char*, int, int) = nullptr;
    hip_result_t (*get_device_memory)(std::size_t*, int) = nullptr;
    hip_result_t (*set_device)(int) = nullptr;
    hip_result_t (*stream_create_with_flags)(hip_stream_handle_t*, unsigned) = nullptr;
    hip_result_t (*stream_destroy)(hip_stream_handle_t) = nullptr;
    hip_result_t (*stream_synchronize)(hip_stream_handle_t) = nullptr;
    hip_result_t (*event_create_with_flags)(hip_event_handle_t*, unsigned) = nullptr;
    hip_result_t (*event_record)(hip_event_handle_t, hip_stream_handle_t) = nullptr;
    hip_result_t (*event_synchronize)(hip_event_handle_t) = nullptr;
    hip_result_t (*event_destroy)(hip_event_handle_t) = nullptr;
    hip_result_t (*pointer_attribute)(void*, int, void const*) = nullptr;
    hip_result_t (*allocate_async)(void**, std::size_t, hip_stream_handle_t) = nullptr;
    hip_result_t (*free_async)(void*, hip_stream_handle_t) = nullptr;
    hip_result_t (*allocate_managed)(void**, std::size_t, unsigned) = nullptr;
    hip_result_t (*free_blocking)(void*) = nullptr;
    hip_result_t (*fill_async)(void*, unsigned char, std::size_t, hip_stream_handle_t) = nullptr;
    hip_result_t (*upload_async)(void*, void const*, std::size_t, hip_stream_handle_t) = nullptr;
    hip_result_t (*download_async)(void*, void*, std::size_t, hip_stream_handle_t) = nullptr;
    hip_result_t (*copy_async)(void*, void*, std::size_t, hip_stream_handle_t) = nullptr;
    hip_result_t (*register_host)(void*, std::size_t, unsigned) = nullptr;
    hip_result_t (*unregister_host)(void*) = nullptr;
    hip_result_t (*module_load_data)(hip_module_handle_t*, void const*) = nullptr;
    hip_result_t (*module_unload)(hip_module_handle_t) = nullptr;
    hip_result_t (*module_get_function)(hip_function_handle_t*, hip_module_handle_t, char const*) = nullptr;
    hip_result_t (*module_launch_kernel)(hip_function_handle_t, unsigned, unsigned, unsigned, unsigned, unsigned,
                                         unsigned, unsigned, hip_stream_handle_t, void**, void**) = nullptr;

    /** Whether the runtime loaded with every entry resolved. */
    explicit operator bool() const noexcept { return module_launch_kernel != nullptr; }

    /** Opens the runtime and resolves every entry, or answers the empty table. The library stays open for the life of
     *  the process, as the table points into it. */
    static hip_api_t load() noexcept {
        // Each system names the runtime its own way, and opening another's name merely fails
        shared_library_t library;
        for (char const* name :
             {"libamdhip64.so", "libamdhip64.so.7", "libamdhip64.so.6", "amdhip64_7.dll", "amdhip64_6.dll"})
            if ((library = shared_library_t::open(name))) break;
        if (!library) return {};
        hip_api_t runtime;
        bool const resolved = //
            library.resolve("hipGetDeviceCount", runtime.get_device_count) &&
            library.resolve("hipDeviceGet", runtime.get_device) &&
            library.resolve("hipDeviceGetAttribute", runtime.get_device_attribute) &&
            library.resolve("hipDeviceGetName", runtime.get_device_name) &&
            library.resolve("hipDeviceTotalMem", runtime.get_device_memory) &&
            library.resolve("hipSetDevice", runtime.set_device) &&
            library.resolve("hipStreamCreateWithFlags", runtime.stream_create_with_flags) &&
            library.resolve("hipStreamDestroy", runtime.stream_destroy) &&
            library.resolve("hipStreamSynchronize", runtime.stream_synchronize) &&
            library.resolve("hipEventCreateWithFlags", runtime.event_create_with_flags) &&
            library.resolve("hipEventRecord", runtime.event_record) &&
            library.resolve("hipEventSynchronize", runtime.event_synchronize) &&
            library.resolve("hipEventDestroy", runtime.event_destroy) &&
            library.resolve("hipPointerGetAttribute", runtime.pointer_attribute) &&
            library.resolve("hipMallocAsync", runtime.allocate_async) &&
            library.resolve("hipFreeAsync", runtime.free_async) &&
            library.resolve("hipMallocManaged", runtime.allocate_managed) &&
            library.resolve("hipFree", runtime.free_blocking) &&
            library.resolve("hipMemsetD8Async", runtime.fill_async) &&
            library.resolve("hipMemcpyHtoDAsync", runtime.upload_async) &&
            library.resolve("hipMemcpyDtoHAsync", runtime.download_async) &&
            library.resolve("hipMemcpyDtoDAsync", runtime.copy_async) &&
            library.resolve("hipHostRegister", runtime.register_host) &&
            library.resolve("hipHostUnregister", runtime.unregister_host) &&
            library.resolve("hipModuleLoadData", runtime.module_load_data) &&
            library.resolve("hipModuleUnload", runtime.module_unload) &&
            library.resolve("hipModuleGetFunction", runtime.module_get_function) &&
            library.resolve("hipModuleLaunchKernel", runtime.module_launch_kernel);
        return resolved ? runtime : hip_api_t {};
    }
};

/** The ordinal of the AMD device whose memory holds @p pointer, or -1 for host memory, pinned, managed or neither. */
inline int hip_device_of(hip_api_t const& api, void const* pointer) noexcept {
    unsigned memory_type = 0;
    int managed = 0, ordinal = -1;
    if (!api || !pointer || api.pointer_attribute(&memory_type, hip_pointer_memory_type_k, pointer) != hip_success_k ||
        memory_type != hip_memory_type_device_k ||
        api.pointer_attribute(&managed, hip_pointer_is_managed_k, pointer) != hip_success_k || managed ||
        api.pointer_attribute(&ordinal, hip_pointer_device_ordinal_k, pointer) != hip_success_k)
        return -1;
    return ordinal;
}

/** Frees every run of @p blocks on HIP ordinal @p device, waiting for the device, as an arena outliving its executor
 *  is freed. */
inline void free_hip_blocks(hip_api_t const& api, int device, device_blocks_t& blocks) noexcept {
    if (!api || api.set_device(device) != hip_success_k) return;
    free_blocks(blocks, [&](std::uintptr_t address) noexcept { api.free_blocking(reinterpret_cast<void*>(address)); });
}

/** Writes every AMD device @p api reaches into @p into, in HIP ordinal order, as the runtime reports it, and answers
 *  how many it wrote; none without @c libamdhip64. */
inline std::size_t list_amd_gpu_devices(hip_api_t const& api, std::span<gpu_device_t> into) noexcept {
    std::size_t count = 0;
    int devices_count = 0;
    if (!api || api.get_device_count(&devices_count) != hip_success_k) return count;
    for (int ordinal = 0; ordinal != devices_count && count != into.size(); ++ordinal) {
        int handle = 0, major = 0, minor = 0, compute_units = 0;
        std::size_t memory_bytes = 0;
        if (api.get_device(&handle, ordinal) != hip_success_k ||
            api.get_device_attribute(&major, hip_attribute_compute_capability_major_k, ordinal) != hip_success_k ||
            api.get_device_attribute(&minor, hip_attribute_compute_capability_minor_k, ordinal) != hip_success_k ||
            api.get_device_attribute(&compute_units, hip_attribute_multiprocessor_count_k, ordinal) != hip_success_k ||
            api.get_device_memory(&memory_bytes, handle) != hip_success_k)
            continue;
        gpu_device_t& device = into[count++];
        device = {gpu_vendor_t::amd_k,
                  static_cast<std::uint32_t>(ordinal),
                  static_cast<std::uint32_t>(major),
                  static_cast<std::uint32_t>(minor),
                  static_cast<std::uint32_t>(compute_units),
                  static_cast<std::uint64_t>(memory_bytes)};
        api.get_device_name(device.name, static_cast<int>(sizeof(device.name)), handle);
    }
    return count;
}

/** An owned event on the device current at its creation, which blocks on sync, so a wait sleeps instead of spinning a
 *  core; destruction never waits. */
class hip_event_t {
  public:
    hip_event_t() noexcept = default;
    explicit hip_event_t(hip_api_t const& api) noexcept : api_(&api) {
        api.event_create_with_flags(&handle_, hip_event_blocking_sync_k | hip_event_disable_timing_k);
    }
    hip_event_t(hip_event_t&& other) noexcept : api_(other.api_), handle_(std::exchange(other.handle_, nullptr)) {}
    hip_event_t& operator=(hip_event_t&& other) noexcept {
        std::swap(api_, other.api_), std::swap(handle_, other.handle_);
        return *this;
    }
    hip_event_t(hip_event_t const&) = delete;
    hip_event_t& operator=(hip_event_t const&) = delete;
    ~hip_event_t() noexcept {
        if (handle_) api_->event_destroy(handle_);
    }

    /** Whether the runtime made the event. */
    explicit operator bool() const noexcept { return handle_ != nullptr; }

    /** Marks everything queued on @p stream so far, and sleeps until the device ran it, answering whether it did. */
    bool record_and_wait(hip_stream_handle_t stream) noexcept {
        return handle_ && api_->event_record(handle_, stream) == hip_success_k &&
               api_->event_synchronize(handle_) == hip_success_k;
    }

  private:
    hip_api_t const* api_ = nullptr;
    hip_event_handle_t handle_ = nullptr;
};

/** One code object loaded onto one AMD device and unloaded with this owner; kernels are found by name. */
class hip_module_t {
  public:
    hip_module_t() noexcept = default;
    hip_module_t(hip_module_t&& other) noexcept : api_(other.api_), handle_(std::exchange(other.handle_, nullptr)) {}
    hip_module_t& operator=(hip_module_t&& other) noexcept {
        std::swap(api_, other.api_), std::swap(handle_, other.handle_);
        return *this;
    }
    hip_module_t(hip_module_t const&) = delete;
    hip_module_t& operator=(hip_module_t const&) = delete;
    ~hip_module_t() noexcept {
        if (handle_) api_->module_unload(handle_);
    }

    /** Loads @p code_object onto HIP ordinal @p device, whose offload bundle entry matching the device's processor the
     *  runtime picks, refusing an empty one with @c device_k. */
    static expected<hip_module_t> load(hip_api_t const& api, int device, std::span<byte_t const> code_object) noexcept {
        hip_module_t loaded_module;
        loaded_module.api_ = &api;
        if (code_object.empty() || !api || api.set_device(device) != hip_success_k ||
            api.module_load_data(&loaded_module.handle_, code_object.data()) != hip_success_k)
            return status_t {status_code_t::device_k, "The device refused the code object"};
        return loaded_module;
    }

    /** The kernel exported as @p name, or null when the code object has none. */
    hip_function_handle_t function(char const* name) const noexcept {
        hip_function_handle_t function = nullptr;
        return handle_ && api_->module_get_function(&function, handle_, name) == hip_success_k ? function : nullptr;
    }

  private:
    hip_api_t const* api_ = nullptr;
    hip_module_handle_t handle_ = nullptr;
};

/** The stream every call on one AMD device queues on, the event it waits on, the device's class, and the ceiling on
 *  the scratch those calls walk with. Work is queued, never waited on but by @c wait, so one host thread keeps the
 *  device fed. */
class hip_executor_t {
  public:
    static constexpr gpu_vendor_t vendor_k = gpu_vendor_t::amd_k;

    /** The memory of the arenas made over this executor. */
    using arena_blocks_t = hip_blocks_t;

    /** Binds HIP ordinal @p ordinal of class @p gpu_class, and queues on @p borrowed, a @c hipStream_t of the caller's
     *  on that device which outlives the executor, or on one of its own for null; its scratch never grows past
     *  @p scratch_limit bytes, zero for no limit. */
    static expected<hip_executor_t> spawn(hip_api_t const& api, int ordinal, gpu_class_t gpu_class, void* borrowed,
                                          std::size_t scratch_limit) noexcept {
        if (!api || api.set_device(ordinal) != hip_success_k)
            return status_t {status_code_t::device_k, "The HIP runtime refused the device"};
        hip_executor_t executor;
        executor.api_ = &api;
        executor.device_ = ordinal;
        executor.gpu_class_ = gpu_class;
        executor.scratch_limit_ = scratch_limit;
        executor.stream_ = static_cast<hip_stream_handle_t>(borrowed);
        if (!borrowed) {
            if (api.stream_create_with_flags(&executor.owned_stream_, hip_stream_non_blocking_k) != hip_success_k)
                return status_t {status_code_t::device_k, "Failed to create a stream on the device"};
            executor.stream_ = executor.owned_stream_;
        }
        executor.event_ = hip_event_t {api};
        if (!executor.event_) return status_t {status_code_t::device_k, "Failed to create an event on the device"};
        // The bounce stays on node zero where the runtime places the device on no NUMA node
        int numa_node = -1;
        if (api.get_device_attribute(&numa_node, hip_attribute_host_numa_id_k, ordinal) != hip_success_k ||
            numa_node < 0)
            numa_node = 0;
        executor.bounce_ = pinned_bounce<hip_api_t> {api, numa_node};
        return executor;
    }

    hip_executor_t(hip_executor_t&& other) noexcept { swap(other); }
    hip_executor_t& operator=(hip_executor_t&& other) noexcept {
        swap(other);
        return *this;
    }
    hip_executor_t(hip_executor_t const&) = delete;
    hip_executor_t& operator=(hip_executor_t const&) = delete;
    ~hip_executor_t() noexcept {
        if (!api_ || api_->set_device(device_) != hip_success_k) return;
        api_->stream_synchronize(stream_);
        free_blocks(managed_.blocks, [this](std::uintptr_t address) noexcept { free_managed(address); });
        if (owned_stream_) api_->stream_destroy(owned_stream_);
    }

    /** Waits until everything queued on the stream ran, reporting a failure among it as @c device_k. */
    status_t wait() noexcept {
        if (api_->set_device(device_) != hip_success_k || !event_.record_and_wait(stream_))
            return {status_code_t::device_k, "The device failed a call queued on the executor"};
        return {};
    }

    /** The HIP ordinal every call and arena of the executor uses. */
    int device() const noexcept { return device_; }

    /** The class of the device. */
    gpu_class_t gpu_class() const noexcept { return gpu_class_; }

    /** The StringZilla capabilities the device's class runs, the one ROCm tier for every class. */
    sz_capability_t capabilities() const noexcept {
        return gpu_class_ == gpu_class_t::unknown_k ? sz_capability_t {0} : sz_capability_t {sz_cap_rocm_k};
    }

    /** Bytes of scratch a call may take, zero for no limit. */
    std::size_t scratch_limit() const noexcept { return scratch_limit_; }

    /** The stream every call queues on, owned or borrowed. */
    hip_stream_handle_t stream() const noexcept { return stream_; }

    /** Makes the device current on the calling thread, once per call, before any other entry below. */
    status_t bind() noexcept {
        if (api_->set_device(device_) != hip_success_k)
            return {status_code_t::device_k, "The HIP runtime refused the device"};
        return {};
    }

    /** The ordinal of the AMD device holding @p pointer, or -1 for memory the host reads in place. */
    int device_of(void const* pointer) const noexcept { return hip_device_of(*api_, pointer); }

    /** Queues an allocation of @p bytes on the stream, answering its device address. */
    expected<std::uintptr_t> allocate_async(std::size_t bytes) noexcept {
        void* address = nullptr;
        if (api_->allocate_async(&address, bytes, stream_) != hip_success_k)
            return status_t {status_code_t::out_of_memory_k, "The device refused an allocation"};
        return reinterpret_cast<std::uintptr_t>(address);
    }

    /** Queues freeing @p address, which @c allocate_async answered, behind everything queued before it. */
    void free_async(std::uintptr_t address) noexcept { api_->free_async(reinterpret_cast<void*>(address), stream_); }

    /** Allocates @p bytes of managed memory, which the host and the device both reach. */
    expected<std::uintptr_t> allocate_managed(std::size_t bytes) noexcept {
        void* address = nullptr;
        if (api_->allocate_managed(&address, bytes, hip_attach_global_k) != hip_success_k)
            return status_t {status_code_t::out_of_memory_k, "The device refused a managed allocation"};
        return reinterpret_cast<std::uintptr_t>(address);
    }

    /** Frees @p address, which @c allocate_managed answered, once the device is done with it. */
    void free_managed(std::uintptr_t address) noexcept { api_->free_blocking(reinterpret_cast<void*>(address)); }

    /** Queues setting @p bytes at device @p address to @p value. */
    status_t fill_async(std::uintptr_t address, byte_t value, std::size_t bytes) noexcept {
        if (api_->fill_async(reinterpret_cast<void*>(address), static_cast<unsigned char>(value), bytes, stream_) !=
            hip_success_k)
            return {status_code_t::device_k, "The device refused a fill"};
        return {};
    }

    /** Queues copying @p source from the host to device @p address. */
    status_t upload_async(std::uintptr_t address, std::span<byte_t const> source) noexcept {
        if (api_->upload_async(reinterpret_cast<void*>(address), source.data(), source.size(), stream_) !=
            hip_success_k)
            return {status_code_t::device_k, "The device refused a copy from the host"};
        return {};
    }

    /** Queues copying device @p address into @p target on the host. */
    status_t download_async(std::span<byte_t> target, std::uintptr_t address) noexcept {
        if (api_->download_async(target.data(), reinterpret_cast<void*>(address), target.size(), stream_) !=
            hip_success_k)
            return {status_code_t::device_k, "The device refused a copy to the host"};
        return {};
    }

    /** Queues copying @p bytes from device @p source to device @p target. */
    status_t copy_async(std::uintptr_t target, std::uintptr_t source, std::size_t bytes) noexcept {
        if (api_->copy_async(reinterpret_cast<void*>(target), reinterpret_cast<void*>(source), bytes, stream_) !=
            hip_success_k)
            return {status_code_t::device_k, "The device refused a copy on the device"};
        return {};
    }

    /** The host arena on the device's NUMA node that host verbs write into and copies leave from. */
    ustore_arena_t bounce() noexcept { return bounce_.arena(); }

    /** Rewinds the bounce to its newest block, unpinning every older one before it is freed. */
    void rewind_bounce() noexcept { bounce_.rewind(); }

    /** Pins every block of the bounce grown since the last pin, so copies from and into it run asynchronously. */
    status_t pin_bounce() noexcept { return bounce_.pin(); }

    /** The managed runs StringZilla's device engines lay their state out in. */
    managed_blocks_t& managed() noexcept { return managed_; }

  private:
    hip_executor_t() noexcept = default;

    void swap(hip_executor_t& other) noexcept {
        std::swap(api_, other.api_), std::swap(owned_stream_, other.owned_stream_), std::swap(stream_, other.stream_);
        std::swap(event_, other.event_), std::swap(device_, other.device_), std::swap(gpu_class_, other.gpu_class_);
        std::swap(scratch_limit_, other.scratch_limit_), std::swap(bounce_, other.bounce_);
        std::swap(managed_, other.managed_);
    }

    hip_api_t const* api_ = nullptr;

    /** The stream made here and destroyed with the executor, null when the caller lent @c stream_. */
    hip_stream_handle_t owned_stream_ = nullptr;

    hip_stream_handle_t stream_ = nullptr;
    hip_event_t event_;
    int device_ = 0;
    gpu_class_t gpu_class_ = gpu_class_t::unknown_k;
    std::size_t scratch_limit_ = 0;
    pinned_bounce<hip_api_t> bounce_;
    managed_blocks_t managed_ {};
};

} // namespace unum::ustore::utilities
