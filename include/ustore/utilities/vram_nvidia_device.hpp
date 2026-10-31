/**
 *  @file include/ustore/utilities/vram_nvidia_device.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief NVIDIA devices from the host: the driver opened at first use, the device listing, modules, events and the
 *      executor calls on one device queue through.
 *
 *  The library never links @c libcuda or the CUDA runtime: @c cuda_api_t::load opens the driver and resolves the entry
 *  points UStore calls into one table, which stays empty where the driver is absent, so NVIDIA then lists no devices
 *  and the binary still loads. The few driver types, constants and prototypes UStore calls are spelled here, copied
 *  from the driver's stable ABI, so any host compiler builds it. Kernels reach a device as fatbins compiled ahead of
 *  time and embedded in the library, loaded by @c cuda_module_t and launched by name. The runtime helpers the graph's
 *  device fold still calls live apart, in @c vram_nvidia_runtime.cuh.
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

#pragma region Driver ABI

/** The driver's status word, @c CUresult, naming the values UStore tells apart; any other is a failure. */
enum cuda_result_t : int {
    cuda_success_k = 0,
    cuda_error_not_initialized_k = 3,
};

/** A device address, @c CUdeviceptr, which unified addressing shares with the host. */
using cuda_device_address_t = unsigned long long;

/** The driver's opaque handles: @c CUcontext, @c CUstream, @c CUevent, @c CUmodule and @c CUfunction. */
using cuda_context_handle_t = struct cuda_context_opaque_t*;
using cuda_stream_handle_t = struct cuda_stream_opaque_t*;
using cuda_event_handle_t = struct cuda_event_opaque_t*;
using cuda_module_handle_t = struct cuda_module_opaque_t*;
using cuda_function_handle_t = struct cuda_function_opaque_t*;

/** The @c CUdevice_attribute values UStore queries. */
enum cuda_device_attribute_t : int {
    cuda_attribute_multiprocessor_count_k = 16,
    cuda_attribute_compute_capability_major_k = 75,
    cuda_attribute_compute_capability_minor_k = 76,
    cuda_attribute_host_numa_id_k = 134,
};

/** The flags UStore passes, @c CU_STREAM_NON_BLOCKING, @c CU_EVENT_BLOCKING_SYNC and @c CU_EVENT_DISABLE_TIMING, and
 *  the @c CUpointer_attribute values it reads. */
inline constexpr unsigned cuda_stream_non_blocking_k = 1;
inline constexpr unsigned cuda_event_blocking_sync_k = 1;
inline constexpr unsigned cuda_event_disable_timing_k = 2;
inline constexpr int cuda_pointer_memory_type_k = 2;
inline constexpr int cuda_pointer_is_managed_k = 8;
inline constexpr int cuda_pointer_device_ordinal_k = 9;
inline constexpr unsigned cuda_memory_type_device_k = 2;

/** The @c CU_MEM_ATTACH_GLOBAL flag of managed allocations, which any stream on any device reaches. */
inline constexpr unsigned cuda_attach_global_k = 1;

#pragma endregion Driver ABI

/**
 *  @brief The driver entry points UStore calls, resolved from @c libcuda by @c load.
 *
 *  Every entry is null when the driver is absent or lacks any of them, which leaves NVIDIA with no devices; each
 *  spells the prototype of the exported symbol it is resolved by, the @c _v2 one where @c <cuda.h> maps the name onto
 *  it.
 */
struct cuda_api_t {
    cuda_result_t (*init)(unsigned) = nullptr;
    cuda_result_t (*get_device_count)(int*) = nullptr;
    cuda_result_t (*get_device)(int*, int) = nullptr;
    cuda_result_t (*get_device_attribute)(int*, cuda_device_attribute_t, int) = nullptr;
    cuda_result_t (*get_device_name)(char*, int, int) = nullptr;
    cuda_result_t (*get_device_memory)(std::size_t*, int) = nullptr;
    cuda_result_t (*retain_primary_context)(cuda_context_handle_t*, int) = nullptr;
    cuda_result_t (*set_current_context)(cuda_context_handle_t) = nullptr;
    cuda_result_t (*stream_create)(cuda_stream_handle_t*, unsigned) = nullptr;
    cuda_result_t (*stream_destroy)(cuda_stream_handle_t) = nullptr;
    cuda_result_t (*stream_synchronize)(cuda_stream_handle_t) = nullptr;
    cuda_result_t (*event_create)(cuda_event_handle_t*, unsigned) = nullptr;
    cuda_result_t (*event_record)(cuda_event_handle_t, cuda_stream_handle_t) = nullptr;
    cuda_result_t (*event_synchronize)(cuda_event_handle_t) = nullptr;
    cuda_result_t (*event_destroy)(cuda_event_handle_t) = nullptr;
    cuda_result_t (*pointer_attribute)(void*, int, cuda_device_address_t) = nullptr;
    cuda_result_t (*allocate_async)(cuda_device_address_t*, std::size_t, cuda_stream_handle_t) = nullptr;
    cuda_result_t (*free_async)(cuda_device_address_t, cuda_stream_handle_t) = nullptr;
    cuda_result_t (*allocate_managed)(cuda_device_address_t*, std::size_t, unsigned) = nullptr;
    cuda_result_t (*free_blocking)(cuda_device_address_t) = nullptr;
    cuda_result_t (*fill_async)(cuda_device_address_t, unsigned char, std::size_t, cuda_stream_handle_t) = nullptr;
    cuda_result_t (*upload_async)(cuda_device_address_t, void const*, std::size_t, cuda_stream_handle_t) = nullptr;
    cuda_result_t (*download_async)(void*, cuda_device_address_t, std::size_t, cuda_stream_handle_t) = nullptr;
    cuda_result_t (*copy_async)(cuda_device_address_t, cuda_device_address_t, std::size_t,
                                cuda_stream_handle_t) = nullptr;
    cuda_result_t (*register_host)(void*, std::size_t, unsigned) = nullptr;
    cuda_result_t (*unregister_host)(void*) = nullptr;
    cuda_result_t (*module_load_data)(cuda_module_handle_t*, void const*) = nullptr;
    cuda_result_t (*module_unload)(cuda_module_handle_t) = nullptr;
    cuda_result_t (*module_get_function)(cuda_function_handle_t*, cuda_module_handle_t, char const*) = nullptr;
    cuda_result_t (*launch_kernel)(cuda_function_handle_t, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                                   unsigned, cuda_stream_handle_t, void**, void**) = nullptr;

    /** Whether the driver loaded and initialized with every entry resolved. */
    explicit operator bool() const noexcept { return launch_kernel != nullptr; }

    /** Opens the driver, resolves every entry and initializes it, or answers the empty table. The library stays open
     *  for the life of the process, as the table points into it. */
    static cuda_api_t load() noexcept {
        // Each system names the driver its own way, and opening another's name merely fails
        shared_library_t library;
        for (char const* name : {"libcuda.so.1", "nvcuda.dll"})
            if ((library = shared_library_t::open(name))) break;
        if (!library) return {};
        cuda_api_t api;
        bool const resolved = //
            library.resolve("cuInit", api.init) && library.resolve("cuDeviceGetCount", api.get_device_count) &&
            library.resolve("cuDeviceGet", api.get_device) &&
            library.resolve("cuDeviceGetAttribute", api.get_device_attribute) &&
            library.resolve("cuDeviceGetName", api.get_device_name) &&
            library.resolve("cuDeviceTotalMem_v2", api.get_device_memory) &&
            library.resolve("cuDevicePrimaryCtxRetain", api.retain_primary_context) &&
            library.resolve("cuCtxSetCurrent", api.set_current_context) &&
            library.resolve("cuStreamCreate", api.stream_create) &&
            library.resolve("cuStreamDestroy_v2", api.stream_destroy) &&
            library.resolve("cuStreamSynchronize", api.stream_synchronize) &&
            library.resolve("cuEventCreate", api.event_create) && library.resolve("cuEventRecord", api.event_record) &&
            library.resolve("cuEventSynchronize", api.event_synchronize) &&
            library.resolve("cuEventDestroy_v2", api.event_destroy) &&
            library.resolve("cuPointerGetAttribute", api.pointer_attribute) &&
            library.resolve("cuMemAllocAsync", api.allocate_async) &&
            library.resolve("cuMemFreeAsync", api.free_async) &&
            library.resolve("cuMemAllocManaged", api.allocate_managed) &&
            library.resolve("cuMemFree_v2", api.free_blocking) && library.resolve("cuMemsetD8Async", api.fill_async) &&
            library.resolve("cuMemcpyHtoDAsync_v2", api.upload_async) &&
            library.resolve("cuMemcpyDtoHAsync_v2", api.download_async) &&
            library.resolve("cuMemcpyDtoDAsync_v2", api.copy_async) &&
            library.resolve("cuMemHostRegister_v2", api.register_host) &&
            library.resolve("cuMemHostUnregister", api.unregister_host) &&
            library.resolve("cuModuleLoadData", api.module_load_data) &&
            library.resolve("cuModuleUnload", api.module_unload) &&
            library.resolve("cuModuleGetFunction", api.module_get_function) &&
            library.resolve("cuLaunchKernel", api.launch_kernel);
        // A driver without a usable GPU fails its initialization, which lists no devices either
        if (resolved && api.init(0) == cuda_success_k) return api;
        return {};
    }
};

/** The ordinal of the NVIDIA device whose memory holds @p pointer, or -1 for host memory, pinned, managed or neither,
 *  which unified addressing tells apart through the driver. */
inline int cuda_device_of(cuda_api_t const& api, void const* pointer) noexcept {
    unsigned memory_type = 0;
    int managed = 0, ordinal = -1;
    cuda_device_address_t const address = static_cast<cuda_device_address_t>(reinterpret_cast<std::uintptr_t>(pointer));
    if (!api || !pointer ||
        api.pointer_attribute(&memory_type, cuda_pointer_memory_type_k, address) != cuda_success_k ||
        memory_type != cuda_memory_type_device_k ||
        api.pointer_attribute(&managed, cuda_pointer_is_managed_k, address) != cuda_success_k || managed ||
        api.pointer_attribute(&ordinal, cuda_pointer_device_ordinal_k, address) != cuda_success_k)
        return -1;
    return ordinal;
}

/** Frees every run of @p blocks through @p context, the retained primary context of the device they live on, waiting
 *  for the device, as an arena outliving its executor is freed. */
inline void free_cuda_blocks(cuda_api_t const& api, cuda_context_handle_t context, device_blocks_t& blocks) noexcept {
    if (!api || api.set_current_context(context) != cuda_success_k) return;
    free_blocks(blocks, [&](std::uintptr_t address) noexcept { api.free_blocking(address); });
}

/** Writes every NVIDIA device @p api reaches into @p into, in CUDA ordinal order, as the driver reports it, and
 *  answers how many it wrote; none without a driver. */
inline std::size_t list_nvidia_gpu_devices(cuda_api_t const& api, std::span<gpu_device_t> into) noexcept {
    std::size_t count = 0;
    int devices_count = 0;
    if (!api || api.get_device_count(&devices_count) != cuda_success_k) return count;
    for (int ordinal = 0; ordinal != devices_count && count != into.size(); ++ordinal) {
        int handle = 0, major = 0, minor = 0, compute_units = 0;
        std::size_t memory_bytes = 0;
        if (api.get_device(&handle, ordinal) != cuda_success_k ||
            api.get_device_attribute(&major, cuda_attribute_compute_capability_major_k, handle) != cuda_success_k ||
            api.get_device_attribute(&minor, cuda_attribute_compute_capability_minor_k, handle) != cuda_success_k ||
            api.get_device_attribute(&compute_units, cuda_attribute_multiprocessor_count_k, handle) != cuda_success_k ||
            api.get_device_memory(&memory_bytes, handle) != cuda_success_k)
            continue;
        gpu_device_t& device = into[count++];
        device = {gpu_vendor_t::nvidia_k,
                  static_cast<std::uint32_t>(ordinal),
                  static_cast<std::uint32_t>(major),
                  static_cast<std::uint32_t>(minor),
                  static_cast<std::uint32_t>(compute_units),
                  static_cast<std::uint64_t>(memory_bytes)};
        api.get_device_name(device.name, static_cast<int>(sizeof(device.name)), handle);
    }
    return count;
}

/** An owned event in the context current at its creation, which blocks on sync, so a wait sleeps instead of spinning
 *  a core; destruction never waits. */
class cuda_event_t {
  public:
    cuda_event_t() noexcept = default;
    explicit cuda_event_t(cuda_api_t const& api) noexcept : api_(&api) {
        api.event_create(&handle_, cuda_event_blocking_sync_k | cuda_event_disable_timing_k);
    }
    cuda_event_t(cuda_event_t&& other) noexcept : api_(other.api_), handle_(std::exchange(other.handle_, nullptr)) {}
    cuda_event_t& operator=(cuda_event_t&& other) noexcept {
        std::swap(api_, other.api_), std::swap(handle_, other.handle_);
        return *this;
    }
    cuda_event_t(cuda_event_t const&) = delete;
    cuda_event_t& operator=(cuda_event_t const&) = delete;
    ~cuda_event_t() noexcept {
        if (handle_) api_->event_destroy(handle_);
    }

    /** Whether the driver made the event. */
    explicit operator bool() const noexcept { return handle_ != nullptr; }

    /** Marks everything queued on @p stream so far, and sleeps until the device ran it, answering whether it did. */
    bool record_and_wait(cuda_stream_handle_t stream) noexcept {
        return handle_ && api_->event_record(handle_, stream) == cuda_success_k &&
               api_->event_synchronize(handle_) == cuda_success_k;
    }

  private:
    cuda_api_t const* api_ = nullptr;
    cuda_event_handle_t handle_ = nullptr;
};

/** One fatbin loaded into one device's primary context and unloaded with this owner; kernels are found by name. */
class cuda_module_t {
  public:
    cuda_module_t() noexcept = default;
    cuda_module_t(cuda_module_t&& other) noexcept
        : api_(other.api_), context_(other.context_), handle_(std::exchange(other.handle_, nullptr)) {}
    cuda_module_t& operator=(cuda_module_t&& other) noexcept {
        std::swap(api_, other.api_), std::swap(context_, other.context_), std::swap(handle_, other.handle_);
        return *this;
    }
    cuda_module_t(cuda_module_t const&) = delete;
    cuda_module_t& operator=(cuda_module_t const&) = delete;
    ~cuda_module_t() noexcept {
        if (handle_ && api_->set_current_context(context_) == cuda_success_k) api_->module_unload(handle_);
    }

    /** Loads @p fatbin into @p context, whose cubin matching the device the driver picks, or JIT-compiles its PTX,
     *  refusing an empty one with @c device_k. */
    static expected<cuda_module_t> load(cuda_api_t const& api, cuda_context_handle_t context,
                                        std::span<byte_t const> fatbin) noexcept {
        cuda_module_t loaded_module;
        loaded_module.api_ = &api;
        loaded_module.context_ = context;
        if (fatbin.empty() || !api || api.set_current_context(context) != cuda_success_k ||
            api.module_load_data(&loaded_module.handle_, fatbin.data()) != cuda_success_k)
            return status_t {status_code_t::device_k, "The device refused the fatbin"};
        return loaded_module;
    }

    /** The kernel exported as @p name, or null when the fatbin has none. */
    cuda_function_handle_t function(char const* name) const noexcept {
        cuda_function_handle_t function = nullptr;
        return handle_ && api_->module_get_function(&function, handle_, name) == cuda_success_k ? function : nullptr;
    }

  private:
    cuda_api_t const* api_ = nullptr;
    cuda_context_handle_t context_ = nullptr;
    cuda_module_handle_t handle_ = nullptr;
};

#pragma region Executors

/** The stream every call on one NVIDIA device queues on, the event it waits on, the device's class, and the ceiling
 *  on the scratch those calls walk with. Work is queued, never waited on but by @c wait, so one host thread keeps the
 *  device fed. */
class cuda_executor_t {
  public:
    static constexpr gpu_vendor_t vendor_k = gpu_vendor_t::nvidia_k;

    /** The memory of the arenas made over this executor. */
    using arena_blocks_t = cuda_blocks_t;

    /** Binds CUDA ordinal @p ordinal through @p context, its retained primary context, as of class @p gpu_class, and
     *  queues on @p borrowed, a @c cudaStream_t of the caller's on that device which outlives the executor, or on one
     *  of its own for null; its scratch never grows past @p scratch_limit bytes, zero for no limit, and bytes move
     *  between the drives and the device as @p gds asks. */
    static expected<cuda_executor_t> spawn(cuda_api_t const& api, cuda_context_handle_t context, int ordinal,
                                           gpu_class_t gpu_class, void* borrowed, std::size_t scratch_limit,
                                           ustore_gds_t gds) noexcept {
        if (!api || !context || api.set_current_context(context) != cuda_success_k)
            return status_t {status_code_t::device_k, "The CUDA driver refused the device"};
        cuda_executor_t executor;
        executor.api_ = &api;
        executor.context_ = context;
        executor.device_ = ordinal;
        executor.gpu_class_ = gpu_class;
        executor.scratch_limit_ = scratch_limit;
        executor.gds_ = gds;
        executor.stream_ = static_cast<cuda_stream_handle_t>(borrowed);
        if (!borrowed) {
            if (api.stream_create(&executor.owned_stream_, cuda_stream_non_blocking_k) != cuda_success_k)
                return status_t {status_code_t::device_k, "Failed to create a stream on the device"};
            executor.stream_ = executor.owned_stream_;
        }
        executor.event_ = cuda_event_t {api};
        if (!executor.event_) return status_t {status_code_t::device_k, "Failed to create an event on the device"};
        // The bounce stays on node zero where the driver places the device on no NUMA node
        int handle = 0, numa_node = -1;
        if (api.get_device(&handle, ordinal) != cuda_success_k ||
            api.get_device_attribute(&numa_node, cuda_attribute_host_numa_id_k, handle) != cuda_success_k ||
            numa_node < 0)
            numa_node = 0;
        executor.bounce_ = pinned_bounce<cuda_api_t> {api, numa_node};
        return executor;
    }

    cuda_executor_t(cuda_executor_t&& other) noexcept { swap(other); }
    cuda_executor_t& operator=(cuda_executor_t&& other) noexcept {
        swap(other);
        return *this;
    }
    cuda_executor_t(cuda_executor_t const&) = delete;
    cuda_executor_t& operator=(cuda_executor_t const&) = delete;
    ~cuda_executor_t() noexcept {
        if (!api_ || api_->set_current_context(context_) != cuda_success_k) return;
        api_->stream_synchronize(stream_);
        free_blocks(managed_.blocks, [this](std::uintptr_t address) noexcept { free_managed(address); });
        if (owned_stream_) api_->stream_destroy(owned_stream_);
    }

    /** Waits until everything queued on the stream ran, reporting a failure among it as @c device_k. */
    status_t wait() noexcept {
        if (api_->set_current_context(context_) != cuda_success_k || !event_.record_and_wait(stream_))
            return {status_code_t::device_k, "The device failed a call queued on the executor"};
        return {};
    }

    /** The CUDA ordinal every call and arena of the executor uses. */
    int device() const noexcept { return device_; }

    /** The class of the device. */
    gpu_class_t gpu_class() const noexcept { return gpu_class_; }

    /** The StringZilla capabilities the device's class runs: its tiers start at compute capability 9.0 for
     *  @c hopper and 10.0 for @c blackwell, consumer Blackwell included. */
    sz_capability_t capabilities() const noexcept {
        switch (gpu_class_) {
        case gpu_class_t::nvidia_ampere_k:
        case gpu_class_t::nvidia_ada_k: return sz_cap_cuda_k;
        case gpu_class_t::nvidia_hopper_k: return sz_cap_cuda_k | sz_cap_hopper_k;
        case gpu_class_t::nvidia_blackwell_k:
        case gpu_class_t::nvidia_blackwellrtx_k: return sz_cap_cuda_k | sz_cap_hopper_k | sz_cap_blackwell_k;
        default: return 0;
        }
    }

    /** Bytes of scratch a call may take, zero for no limit. */
    std::size_t scratch_limit() const noexcept { return scratch_limit_; }

    /** How bytes move between the drives and the device, which the compactions on the executor read sections by. */
    ustore_gds_t gds() const noexcept { return gds_; }

    /** The stream every call queues on, owned or borrowed. */
    cuda_stream_handle_t stream() const noexcept { return stream_; }

    /** Makes the device's context current on the calling thread, once per call, before any other entry below. */
    status_t bind() noexcept {
        if (api_->set_current_context(context_) != cuda_success_k)
            return {status_code_t::device_k, "The CUDA driver refused the device's context"};
        return {};
    }

    /** The ordinal of the NVIDIA device holding @p pointer, or -1 for memory the host reads in place. */
    int device_of(void const* pointer) const noexcept { return cuda_device_of(*api_, pointer); }

    /** Queues an allocation of @p bytes on the stream, answering its device address. */
    expected<std::uintptr_t> allocate_async(std::size_t bytes) noexcept {
        cuda_device_address_t address = 0;
        if (api_->allocate_async(&address, bytes, stream_) != cuda_success_k)
            return status_t {status_code_t::out_of_memory_k, "The device refused an allocation"};
        return static_cast<std::uintptr_t>(address);
    }

    /** Queues freeing @p address, which @c allocate_async answered, behind everything queued before it. */
    void free_async(std::uintptr_t address) noexcept { api_->free_async(address, stream_); }

    /** Allocates @p bytes of managed memory, which the host and the device both reach. */
    expected<std::uintptr_t> allocate_managed(std::size_t bytes) noexcept {
        cuda_device_address_t address = 0;
        if (api_->allocate_managed(&address, bytes, cuda_attach_global_k) != cuda_success_k)
            return status_t {status_code_t::out_of_memory_k, "The device refused a managed allocation"};
        return static_cast<std::uintptr_t>(address);
    }

    /** Frees @p address, which @c allocate_managed answered, once the device is done with it. */
    void free_managed(std::uintptr_t address) noexcept { api_->free_blocking(address); }

    /** Queues setting @p bytes at device @p address to @p value. */
    status_t fill_async(std::uintptr_t address, byte_t value, std::size_t bytes) noexcept {
        if (api_->fill_async(address, static_cast<unsigned char>(value), bytes, stream_) != cuda_success_k)
            return {status_code_t::device_k, "The device refused a fill"};
        return {};
    }

    /** Queues copying @p source from the host to device @p address. */
    status_t upload_async(std::uintptr_t address, std::span<byte_t const> source) noexcept {
        if (api_->upload_async(address, source.data(), source.size(), stream_) != cuda_success_k)
            return {status_code_t::device_k, "The device refused a copy from the host"};
        return {};
    }

    /** Queues copying device @p address into @p target on the host. */
    status_t download_async(std::span<byte_t> target, std::uintptr_t address) noexcept {
        if (api_->download_async(target.data(), address, target.size(), stream_) != cuda_success_k)
            return {status_code_t::device_k, "The device refused a copy to the host"};
        return {};
    }

    /** Queues copying @p bytes from device @p source to device @p target. */
    status_t copy_async(std::uintptr_t target, std::uintptr_t source, std::size_t bytes) noexcept {
        if (api_->copy_async(target, source, bytes, stream_) != cuda_success_k)
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
    cuda_executor_t() noexcept = default;

    void swap(cuda_executor_t& other) noexcept {
        std::swap(api_, other.api_), std::swap(context_, other.context_);
        std::swap(owned_stream_, other.owned_stream_), std::swap(stream_, other.stream_);
        std::swap(event_, other.event_), std::swap(device_, other.device_), std::swap(gpu_class_, other.gpu_class_);
        std::swap(scratch_limit_, other.scratch_limit_), std::swap(gds_, other.gds_);
        std::swap(bounce_, other.bounce_), std::swap(managed_, other.managed_);
    }

    cuda_api_t const* api_ = nullptr;

    /** The device's primary context, which the process's GPU runtime retained and never releases. */
    cuda_context_handle_t context_ = nullptr;

    /** The stream made here and destroyed with the executor, null when the caller lent @c stream_. */
    cuda_stream_handle_t owned_stream_ = nullptr;

    cuda_stream_handle_t stream_ = nullptr;
    cuda_event_t event_;
    int device_ = 0;
    gpu_class_t gpu_class_ = gpu_class_t::unknown_k;
    std::size_t scratch_limit_ = 0;
    ustore_gds_t gds_ = ustore_gds_automatic_k;
    pinned_bounce<cuda_api_t> bounce_;
    managed_blocks_t managed_ {};
};

#pragma endregion Executors

} // namespace unum::ustore::utilities
