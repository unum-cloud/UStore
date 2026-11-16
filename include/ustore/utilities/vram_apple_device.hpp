/**
 *  @file include/ustore/utilities/vram_apple_device.hpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief Apple devices from the host: Metal opened at first use, the device listing, modules of
 *      MSL source, and the executor calls on one device queue through.
 *
 *  Metal's API is Objective-C, reached here through @c objc_msgSend cast to each call's exact
 *  prototype, so a plain C++ unit drives it with no SDK header: @c metal_api_t::load opens Metal,
 *  the Objective-C runtime, I/O Kit and Core Foundation and resolves the few entry points UStore
 *  calls into one table, which stays empty where any is absent, so Apple then lists no devices and
 *  the binary still loads. Kernels travel as MSL source embedded in the library and compile on the
 *  device at the first spawn of a class, so no Metal toolchain is needed to build.
 *
 *  Memory is unified: every block a kernel reaches is a shared-storage buffer from StringZilla's
 *  unified Metal allocator, which StringZilla's own device engines read too, and the address the
 *  host hands around is its host address. A launch binds the kernel's arguments packed as one
 *  struct and the device's table of live blocks, which turns every host address the arguments
 *  carry, embedded ones included, into a device one. The layer reaching that allocator and its
 *  registry compiles where StringZilla's Metal kernels do, @c STRINGZILLA_TARGET_METAL, and every
 *  other build refuses Apple executors with @c device_k.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`
#include <cstring> // `std::memcpy`, `std::memset`, `std::memmove`, `std::strncpy`

#include <span>    // `std::span`
#include <utility> // `std::exchange`, `std::swap`

#include <stringzilla/metal.h> // `sz_allocator_init_unified_metal`, `sz_stream_synchronize_metal`

#include "ustore/utilities/blocking_files.hpp"  // `shared_library_t`
#include "ustore/utilities/ram_buffers.hpp"     // `managed_blocks_t`, `free_blocks`, `host_blocks_t`
#include "ustore/utilities/types.hpp"           // `gpu_device_t`, `expected`, `status_t`
#include "ustore/utilities/vram_apple_shared.h" // `metal_arguments_slot_k`, `metal_simdgroup_width_k`

namespace unum::ustore::utilities {

#pragma region Runtime ABI

/** The Objective-C runtime's selector, @c SEL, opaque to everyone but the runtime. */
using metal_selector_t = struct metal_selector_opaque_t*;

/** A compiled entry point, @c id<MTLComputePipelineState>, which a launch dispatches. */
using metal_function_handle_t = struct metal_function_opaque_t*;

/** A launch extent, laid out as @c MTLSize so it passes to Metal by value. */
struct metal_size_t {
    std::size_t width = 1;
    std::size_t height = 1;
    std::size_t depth = 1;
};

/** @c MTLGPUFamilyApple7 through @c MTLGPUFamilyApple10, the families a device listing names. */
enum metal_family_t : std::size_t {
    metal_family_apple7_k = 1007,
    metal_family_apple8_k = 1008,
    metal_family_apple9_k = 1009,
    metal_family_apple10_k = 1010,
};

/** @c MTLLanguageVersion3_2, the first with device-scope atomic fences and @c gpuAddress casts. */
inline constexpr std::size_t metal_language_version_k = (3u << 16) | 2u;

/** @c MTLResourceStorageModeShared, which every block takes, so host and device address
 *  the same bytes. */
inline constexpr std::size_t metal_shared_storage_k = 0;

/** @c MTLResourceUsageRead with @c MTLResourceUsageWrite, as a launch may do either
 *  to every block. */
inline constexpr std::size_t metal_usage_read_write_k = 3;

#pragma endregion Runtime ABI

/**
 *  @brief The Metal, Objective-C runtime, I/O Kit and Core Foundation entry points UStore calls,
 *      resolved on first use from the system's own frameworks and @c libobjc.
 *
 *  Every entry is null when any library is absent or lacks any of them, which leaves Apple with no
 *  devices. Foundation is opened for @c NSString alone, which no entry names, and I/O Kit for the
 *  GPU core count, which Metal does not report. Each entry spells the C prototype of its symbol,
 *  with Core Foundation's references as plain pointers and I/O Kit's objects as their 32-bit Mach
 *  ports; @c objc_msgSend takes none of its own, so every send casts it to the call's exact one.
 */
struct metal_api_t {
    void* (*copy_all_devices)() = nullptr;
    void (*send)() = nullptr;
    metal_selector_t (*register_selector)(char const*) = nullptr;
    void* (*get_class)(char const*) = nullptr;
    void* (*registry_entry_id_matching)(std::uint64_t) = nullptr;
    std::uint32_t (*get_matching_service)(std::uint32_t, void*) = nullptr;
    void* (*search_registry_property)(std::uint32_t, char const*, void const*, void const*, std::uint32_t) = nullptr;
    int (*release_registry_object)(std::uint32_t) = nullptr;
    void* (*create_string)(void const*, char const*, std::uint32_t) = nullptr;
    unsigned char (*get_number_value)(void const*, long, void*) = nullptr;
    void (*release_reference)(void const*) = nullptr;
    void* (*autorelease_pool_push)() = nullptr;
    void (*autorelease_pool_pop)(void*) = nullptr;

    /** Whether every library loaded with every entry resolved. */
    explicit operator bool() const noexcept { return autorelease_pool_pop != nullptr; }

    /** Opens the libraries and resolves every entry, or answers the empty table. The libraries stay
     *  open for the life of the process, as the table points into them. */
    static metal_api_t load() noexcept {
        shared_library_t const metal = shared_library_t::open("/System/Library/Frameworks/Metal.framework/Metal");
        shared_library_t const foundation = shared_library_t::open(
            "/System/Library/Frameworks/Foundation.framework/Foundation");
        shared_library_t const runtime = shared_library_t::open("/usr/lib/libobjc.A.dylib");
        shared_library_t const registry = shared_library_t::open("/System/Library/Frameworks/IOKit.framework/IOKit");
        shared_library_t const core = shared_library_t::open(
            "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation");
        metal_api_t api;
        bool const resolved = //
            foundation && metal.resolve("MTLCopyAllDevices", api.copy_all_devices) &&
            runtime.resolve("objc_msgSend", api.send) && runtime.resolve("sel_registerName", api.register_selector) &&
            runtime.resolve("objc_getClass", api.get_class) &&
            registry.resolve("IORegistryEntryIDMatching", api.registry_entry_id_matching) &&
            registry.resolve("IOServiceGetMatchingService", api.get_matching_service) &&
            registry.resolve("IORegistryEntrySearchCFProperty", api.search_registry_property) &&
            registry.resolve("IOObjectRelease", api.release_registry_object) &&
            core.resolve("CFStringCreateWithCString", api.create_string) &&
            core.resolve("CFNumberGetValue", api.get_number_value) &&
            core.resolve("CFRelease", api.release_reference) &&
            runtime.resolve("objc_autoreleasePoolPush", api.autorelease_pool_push) &&
            runtime.resolve("objc_autoreleasePoolPop", api.autorelease_pool_pop);
        return resolved ? api : metal_api_t {};
    }
};

/** Sends @p selector to @p target with @p arguments through @p api, cast to the exact prototype
 *  the call has. */
template <typename result_type_ = void*, typename... argument_types_>
result_type_ metal_send(metal_api_t const& api, void* target, char const* selector,
                        argument_types_... arguments) noexcept {
    using function_t = result_type_ (*)(void*, metal_selector_t, argument_types_...);
    return reinterpret_cast<function_t>(api.send)(target, api.register_selector(selector), arguments...);
}

/** An @c NSString over @p text, autoreleased. */
inline void* metal_string(metal_api_t const& api, char const* text) noexcept {
    return metal_send(api, api.get_class("NSString"), "stringWithUTF8String:", text);
}

/** The newest Apple GPU family @p device supports, as its number, such as 9 for
 *  @c MTLGPUFamilyApple9; zero for a GPU older than Apple7 or not Apple's own. */
inline std::uint32_t apple_family_of(metal_api_t const& api, void* device) noexcept {
    for (metal_family_t const family :
         {metal_family_apple10_k, metal_family_apple9_k, metal_family_apple8_k, metal_family_apple7_k})
        if (metal_send<bool>(api, device, "supportsFamily:", static_cast<std::size_t>(family)))
            return static_cast<std::uint32_t>(family - metal_family_apple7_k + 7);
    return 0;
}

/** The GPU cores of @p device, which Metal does not report: the @c gpu-core-count its accelerator,
 *  the I/O Registry entry Metal names by @c registryID, or that entry's parents and children list;
 *  zero when none does. */
inline std::uint32_t apple_core_count_of(metal_api_t const& api, void* device) noexcept {
    constexpr std::uint32_t main_port_k = 0;          // `kIOMainPortDefault`
    constexpr std::uint32_t utf8_k = 0x08000100;      // `kCFStringEncodingUTF8`
    constexpr std::uint32_t search_options_k = 1 | 2; // `kIORegistryIterateRecursively | ...Parents`
    constexpr long signed_64_bits_k = 4;              // `kCFNumberSInt64Type`
    // The lookup consumes the matching dictionary, so only the service is released here
    std::uint32_t const service = api.get_matching_service(
        main_port_k, api.registry_entry_id_matching(metal_send<std::uint64_t>(api, device, "registryID")));
    if (!service) return 0;
    void* const key = api.create_string(nullptr, "gpu-core-count", utf8_k);
    void* const listed = key ? api.search_registry_property(service, "IOService", key, nullptr, search_options_k)
                             : nullptr;
    std::int64_t cores = 0;
    if (listed && !api.get_number_value(listed, signed_64_bits_k, &cores)) cores = 0;
    if (listed) api.release_reference(listed);
    if (key) api.release_reference(key);
    api.release_registry_object(service);
    return cores > 0 ? static_cast<std::uint32_t>(cores) : 0;
}

/** Every Apple GPU Metal lists, in Metal's own order, opened once per process and kept for it, as
 *  Metal's own device objects are; a device's Apple ordinal is its position here. */
struct metal_devices_t {

    /** The retained @c id<MTLDevice> of every listed device. */
    void* handles[gpu_devices_capacity_k] = {};

    /** What the listing reports of every device: its family as the architecture's major, its cores
     *  and the working set Metal recommends. */
    gpu_device_t listed[gpu_devices_capacity_k] = {};

    std::size_t count = 0;

    metal_devices_t() noexcept = default;
    metal_devices_t(metal_devices_t const&) = delete;
    metal_devices_t& operator=(metal_devices_t const&) = delete;

    /** Opens every device @p api lists, none when the table is empty. */
    explicit metal_devices_t(metal_api_t const& api) noexcept {
        void* const devices = api ? api.copy_all_devices() : nullptr;
        if (!devices) return;
        void* const pool = api.autorelease_pool_push();
        std::size_t const devices_count = metal_send<std::size_t>(api, devices, "count");
        for (std::size_t position = 0; position != devices_count && count != gpu_devices_capacity_k; ++position) {
            void* const device = metal_send(api, devices, "objectAtIndex:", position);
            std::uint32_t const family = apple_family_of(api, device);
            if (!family) continue;
            handles[count] = metal_send(api, device, "retain");
            gpu_device_t& entry = listed[count];
            entry = {gpu_vendor_t::apple_k,
                     static_cast<std::uint32_t>(count),
                     family,
                     0,
                     apple_core_count_of(api, device),
                     metal_send<std::uint64_t>(api, device, "recommendedMaxWorkingSetSize")};
            char const* const name = metal_send<char const*>(api, metal_send(api, device, "name"), "UTF8String");
            if (name) std::strncpy(entry.name, name, sizeof(entry.name) - 1);
            ++count;
        }
        api.autorelease_pool_pop(pool);
        metal_send<void>(api, devices, "release");
    }

    /** The @c id<MTLDevice> listed at @p ordinal, or null past the listing. */
    void* device(int ordinal) const noexcept {
        return ordinal >= 0 && static_cast<std::size_t>(ordinal) < count ? handles[ordinal] : nullptr;
    }
};

/** Copies every Apple GPU @p devices opened into @p into, answering how many it wrote. */
inline std::size_t list_apple_gpu_devices(metal_devices_t const& devices, std::span<gpu_device_t> into) noexcept {
    std::size_t const count = devices.count < into.size() ? devices.count : into.size();
    for (std::size_t position = 0; position != count; ++position) into[position] = devices.listed[position];
    return count;
}

/** One library of MSL source compiled for one Apple device and released with this owner; kernels
 *  are found by name, each built into a pipeline the process keeps, as its kernel table does. */
class metal_module_t {
  public:
    using function_t = metal_function_handle_t;

    metal_module_t() noexcept = default;
    metal_module_t(metal_module_t&& other) noexcept
        : api_(other.api_), device_(other.device_), library_(std::exchange(other.library_, nullptr)) {}
    metal_module_t& operator=(metal_module_t&& other) noexcept {
        std::swap(api_, other.api_), std::swap(device_, other.device_), std::swap(library_, other.library_);
        return *this;
    }
    metal_module_t(metal_module_t const&) = delete;
    metal_module_t& operator=(metal_module_t const&) = delete;
    ~metal_module_t() noexcept {
        if (library_) metal_send<void>(*api_, library_, "release");
    }

    /** Compiles @p source, null-terminated MSL text, on @p device, an @c id<MTLDevice>, refusing an
     *  empty or unterminated one, and one the compiler rejects, with @c device_k. */
    static expected<metal_module_t> load(metal_api_t const& api, void* device,
                                         std::span<byte_t const> source) noexcept {
        if (source.empty() || source.back() != byte_t {0} || !api || !device)
            return status_t {status_code_t::device_k, "The device refused the MSL source"};
        metal_module_t loaded_module;
        loaded_module.api_ = &api;
        loaded_module.device_ = device;
        void* const pool = api.autorelease_pool_push();
        void* const options = metal_send(api, metal_send(api, api.get_class("MTLCompileOptions"), "alloc"), "init");
        metal_send<void>(api, options, "setLanguageVersion:", metal_language_version_k);
        void* error = nullptr;
        loaded_module.library_ = metal_send(api, device, "newLibraryWithSource:options:error:",
                                            metal_string(api, reinterpret_cast<char const*>(source.data())), options,
                                            &error);
        metal_send<void>(api, options, "release");
        api.autorelease_pool_pop(pool);
        if (!loaded_module.library_) return status_t {status_code_t::device_k, "The device refused the MSL source"};
        return loaded_module;
    }

    /** The pipeline of the kernel exported as @p name, or null when the library has none
     *  or its simdgroups are not the @c metal_simdgroup_width_k lanes every MSL source
     *  is written against. */
    metal_function_handle_t function(char const* name) const noexcept {
        if (!library_) return nullptr;
        metal_api_t const& api = *api_;
        void* const pool = api.autorelease_pool_push();
        void* const function = metal_send(api, library_, "newFunctionWithName:", metal_string(api, name));
        void* pipeline = nullptr;
        if (function) {
            void* error = nullptr;
            pipeline = metal_send(api, device_, "newComputePipelineStateWithFunction:error:", function, &error);
            metal_send<void>(api, function, "release");
        }
        api.autorelease_pool_pop(pool);
        if (pipeline && metal_send<std::size_t>(api, pipeline, "threadExecutionWidth") != metal_simdgroup_width_k)
            metal_send<void>(api, std::exchange(pipeline, nullptr), "release");
        return static_cast<metal_function_handle_t>(pipeline);
    }

  private:
    metal_api_t const* api_ = nullptr;
    void* device_ = nullptr;
    void* library_ = nullptr;
};

#pragma region Metal Layer

#if STRINGZILLA_TARGET_METAL

/** Shared storage of @p bytes on the device of @p queue, an @c id<MTLCommandQueue>, from
 *  StringZilla's unified allocator, answering its host address, or null on exhaustion. */
inline void* metal_allocate_shared(void* queue, std::size_t bytes) noexcept {
    sz_allocator_t allocator;
    sz_allocator_init_unified_metal(&allocator);
    return allocator.allocate(bytes, allocator.handle, queue);
}

/** Releases the shared block at host address @p address that @c metal_allocate_shared answered,
 *  which the work still queued on it keeps alive until it finishes. */
inline void metal_free_shared(void* queue, void* address) noexcept {
    sz_allocator_t allocator;
    sz_allocator_init_unified_metal(&allocator);
    allocator.free(address, 0, allocator.handle, queue);
}

/** Waits for everything committed to @p queue, answering whether the device ran it. */
inline bool metal_synchronize(void* queue) noexcept { return sz_stream_synchronize_metal(queue) == sz_success_k; }

/**
 *  @brief Commits one dispatch of @p pipeline on @p queue: @p arguments at the arguments slot, the
 *      device's table of live blocks at the blocks slot, every one of them resident, and
 *      @p shared_bytes of threadgroup memory, over @p groups threadgroups of @p threads.
 *
 *  The table is written into a buffer of its own while StringZilla's registry stays locked, as a
 *  block it lists must not be released before the command buffer retains it.
 *
 *  @par Statuses:
 *  - @c status_code_t::device_k The queue's device holds no context, or refused the encoder, the
 *    table or the commit.
 */
inline status_t metal_dispatch(void* queue, metal_function_handle_t pipeline, std::span<byte_t const> arguments,
                               unsigned groups, unsigned threads, unsigned shared_bytes) noexcept {
    using set_buffer_t = void (*)(void*, SEL, void*, sz_size_t, sz_size_t);
    using set_bytes_t = void (*)(void*, SEL, void const*, sz_size_t, sz_size_t);
    using use_resource_t = void (*)(void*, SEL, void*, sz_size_t);
    using new_buffer_t = void* (*)(void*, SEL, sz_size_t, sz_size_t);
    sz_metal_call_t call;
    if (sz_device_enter_metal_(queue, &call) != sz_success_k)
        return {status_code_t::device_k, "The device refused an encoder"};
    sz_metal_context_t* const context = call.context;
    os_unfair_lock_lock(&context->lock);
    std::size_t const blocks_count = context->blocks_count;
    void* const table = reinterpret_cast<new_buffer_t>(objc_msgSend)(
        context->device, sel_registerName("newBufferWithLength:options:"),
        (blocks_count ? blocks_count : 1) * metal_block_bytes_k, metal_shared_storage_k);
    if (table) {
        std::uint64_t* const entries = static_cast<std::uint64_t*>(sz_metal_get_(table, "contents"));
        for (std::size_t index = 0; index != blocks_count; ++index) {
            sz_metal_block_t const& block = context->blocks[index];
            std::uint64_t const device_begin = reinterpret_cast<std::uint64_t (*)(void*, SEL)>(objc_msgSend)(
                block.buffer, sel_registerName("gpuAddress"));
            entries[4 * index + 0] = reinterpret_cast<std::uintptr_t>(block.host);
            entries[4 * index + 1] = reinterpret_cast<std::uintptr_t>(block.host) + block.bytes;
            entries[4 * index + 2] = device_begin;
            entries[4 * index + 3] = 0;
            reinterpret_cast<use_resource_t>(objc_msgSend)(call.encoder, sel_registerName("useResource:usage:"),
                                                           block.buffer, metal_usage_read_write_k);
        }
    }
    os_unfair_lock_unlock(&context->lock);
    if (!table) {
        // A failed status drops the call uncommitted, so nothing is left to await
        [[maybe_unused]] sz_status_t const dropped = sz_metal_commit_(&call, sz_bad_alloc_k);
        return {status_code_t::device_k, "The device refused the block table"};
    }
    std::uint32_t const table_count = static_cast<std::uint32_t>(blocks_count);
    reinterpret_cast<set_bytes_t>(objc_msgSend)(call.encoder, sel_registerName("setBytes:length:atIndex:"),
                                                arguments.data(), arguments.size(), metal_arguments_slot_k);
    reinterpret_cast<set_buffer_t>(objc_msgSend)(call.encoder, sel_registerName("setBuffer:offset:atIndex:"), table, 0,
                                                 metal_blocks_slot_k);
    reinterpret_cast<set_bytes_t>(objc_msgSend)(call.encoder, sel_registerName("setBytes:length:atIndex:"),
                                                &table_count, sizeof(table_count), metal_blocks_count_slot_k);
    // The encoder retains the table, so this owner lets it go at once
    sz_metal_do_(table, "release");
    if (shared_bytes) sz_metal_threadgroup_memory_(call.encoder, shared_bytes, metal_shared_slot_k);
    reinterpret_cast<void (*)(void*, SEL, void*)>(objc_msgSend)(
        call.encoder, sel_registerName("setComputePipelineState:"), static_cast<void*>(pipeline));
    reinterpret_cast<void (*)(void*, SEL, sz_metal_size_t, sz_metal_size_t)>(objc_msgSend)(
        call.encoder, sel_registerName("dispatchThreadgroups:threadsPerThreadgroup:"), sz_metal_size_t {groups, 1, 1},
        sz_metal_size_t {threads, 1, 1});
    if (sz_metal_commit_(&call, sz_success_k) != sz_success_k)
        return {status_code_t::device_k, "The device refused the commit"};
    return {};
}

#else

/** Without StringZilla's Metal layer no device memory exists, so every allocation answers null. */
inline void* metal_allocate_shared(void*, std::size_t) noexcept { return nullptr; }

/** Without StringZilla's Metal layer nothing was allocated, so nothing is released. */
inline void metal_free_shared(void*, void*) noexcept {}

/** Without StringZilla's Metal layer nothing was committed, so the wait fails. */
inline bool metal_synchronize(void*) noexcept { return false; }

/** Without StringZilla's Metal layer no kernel launches. */
inline status_t metal_dispatch(void*, metal_function_handle_t, std::span<byte_t const>, unsigned, unsigned,
                               unsigned) noexcept {
    return {status_code_t::device_k, "The library compiled no Metal layer"};
}

#endif // STRINGZILLA_TARGET_METAL

/** Frees every run of @p blocks, an arena on an Apple device, as an arena outliving its executor is
 *  freed; the work still queued on a run keeps it alive until it finishes. */
inline void free_metal_blocks(device_blocks_t& blocks) noexcept {
    free_blocks(blocks,
                [](std::uintptr_t address) noexcept { metal_free_shared(nullptr, reinterpret_cast<void*>(address)); });
}

#pragma endregion Metal Layer

template <typename module_type_>
struct kernel_table;

/** The queue every call on one Apple device commits to, the device's class, and the ceiling on
 *  the scratch those calls walk with. Work is committed, never waited on but by @c wait, so one
 *  host thread keeps the device fed; memory is unified, so a copy waits for the queue and runs
 *  on the host. */
class metal_executor_t {
  public:
    static constexpr gpu_vendor_t vendor_k = gpu_vendor_t::apple_k;

    /** The memory of the arenas made over this executor. */
    using arena_blocks_t = metal_blocks_t;

    /** The kernels this executor launches. */
    using function_t = metal_function_handle_t;

    /** Binds Apple ordinal @p ordinal, @p device its @c id<MTLDevice>, and commits to @p borrowed,
     *  an @c id<MTLCommandQueue> of the caller's on that device which outlives the executor, or to
     *  one of its own for null; its scratch never grows past @p scratch_limit bytes, zero for no
     *  limit, and its launches find their kernels in @p kernels, the device's table of the
     *  library's bundle, or none for null. */
    static expected<metal_executor_t> spawn(metal_api_t const& api, void* device, int ordinal, void* borrowed,
                                            std::size_t scratch_limit,
                                            kernel_table<metal_module_t> const* kernels = nullptr) noexcept {
        if (!api || !device) return status_t {status_code_t::device_k, "Metal refused the device"};
        if constexpr (!STRINGZILLA_TARGET_METAL)
            return status_t {status_code_t::device_k, "The library compiled no Metal layer"};
        metal_executor_t executor;
        executor.api_ = &api;
        executor.device_ = ordinal;
        executor.scratch_limit_ = scratch_limit;
        executor.kernels_ = kernels;
        executor.queue_ = borrowed;
        if (!borrowed) {
            executor.owned_queue_ = metal_send(api, device, "newCommandQueue");
            if (!executor.owned_queue_) return status_t {status_code_t::device_k, "Failed to create a queue"};
            executor.queue_ = executor.owned_queue_;
        }
        executor.bounce_.memory = host_blocks_t {.newest = nullptr, .memory_domain = 0};
        return executor;
    }

    metal_executor_t(metal_executor_t&& other) noexcept { swap(other); }
    metal_executor_t& operator=(metal_executor_t&& other) noexcept {
        swap(other);
        return *this;
    }
    metal_executor_t(metal_executor_t const&) = delete;
    metal_executor_t& operator=(metal_executor_t const&) = delete;
    ~metal_executor_t() noexcept {
        if (!api_) return;
        metal_synchronize(queue_);
        free_blocks(managed_.blocks, [this](std::uintptr_t address) noexcept { free_managed(address); });
        arena_free_blocks(bounce_);
        if (owned_queue_) metal_send<void>(*api_, owned_queue_, "release");
    }

    /** Waits until everything committed to the queue ran, reporting a failure among it
     *  as @c device_k. */
    status_t wait() noexcept {
        if (!metal_synchronize(queue_))
            return {status_code_t::device_k, "The device failed a call queued on the executor"};
        return {};
    }

    /** The Apple ordinal every call and arena of the executor uses. */
    int device() const noexcept { return device_; }

    /** Bytes of scratch a call may take, zero for no limit. */
    std::size_t scratch_limit() const noexcept { return scratch_limit_; }

    /** The @c id<MTLCommandQueue> every call commits to, owned or borrowed. */
    void* stream() const noexcept { return queue_; }

    /** The runtime table every call goes through. */
    metal_api_t const& api() const noexcept { return *api_; }

    /** The kernels of the library's bundle on the device, or null when none loaded. */
    kernel_table<metal_module_t> const* kernels() const noexcept { return kernels_; }

    /** Metal keeps no current device per thread, so binding has nothing to do. */
    status_t bind() noexcept { return {}; }

    /** Every block is unified memory the host reads in place, so no pointer reads
     *  as device memory. */
    int device_of(void const*) const noexcept { return -1; }

    /** Allocates @p bytes of shared storage on the device, answering its host address. */
    expected<std::uintptr_t> allocate_async(std::size_t bytes) noexcept {
        void* const address = metal_allocate_shared(queue_, bytes);
        if (!address) return status_t {status_code_t::out_of_memory_k, "The device refused an allocation"};
        return reinterpret_cast<std::uintptr_t>(address);
    }

    /** Releases @p address, which @c allocate_async answered; the work still queued on it
     *  keeps it alive. */
    void free_async(std::uintptr_t address) noexcept { metal_free_shared(queue_, reinterpret_cast<void*>(address)); }

    /** Allocates @p bytes the host and the device both reach, which every block on Apple is. */
    expected<std::uintptr_t> allocate_managed(std::size_t bytes) noexcept { return allocate_async(bytes); }

    /** Releases @p address, which @c allocate_managed answered. */
    void free_managed(std::uintptr_t address) noexcept { free_async(address); }

    /** Sets @p bytes at @p address to @p value, once the queue ran everything committed before. */
    status_t fill_async(std::uintptr_t address, byte_t value, std::size_t bytes) noexcept {
        if (status_t const waited = wait(); waited.failed()) return waited;
        std::memset(reinterpret_cast<void*>(address), static_cast<unsigned char>(value), bytes);
        return {};
    }

    /** Copies @p source to @p address, once the queue ran everything committed before. */
    status_t upload_async(std::uintptr_t address, std::span<byte_t const> source) noexcept {
        if (status_t const waited = wait(); waited.failed()) return waited;
        if (!source.empty()) std::memcpy(reinterpret_cast<void*>(address), source.data(), source.size());
        return {};
    }

    /** Copies @p address into @p target, once the queue ran everything committed before. */
    status_t download_async(std::span<byte_t> target, std::uintptr_t address) noexcept {
        if (status_t const waited = wait(); waited.failed()) return waited;
        if (!target.empty()) std::memcpy(target.data(), reinterpret_cast<void const*>(address), target.size());
        return {};
    }

    /** Copies @p bytes from @p source to @p target, once the queue ran
     *  everything committed before. */
    status_t copy_async(std::uintptr_t target, std::uintptr_t source, std::size_t bytes) noexcept {
        if (status_t const waited = wait(); waited.failed()) return waited;
        if (bytes) std::memmove(reinterpret_cast<void*>(target), reinterpret_cast<void const*>(source), bytes);
        return {};
    }

    /** The host arena host verbs write into and copies leave from. */
    ustore_arena_t bounce() noexcept { return &bounce_; }

    /** Rewinds the bounce to its newest block. */
    void rewind_bounce() noexcept { arena_rewind(&bounce_); }

    /** Unified memory needs no pinning, as the device reaches every page the host does. */
    status_t pin_bounce() noexcept { return {}; }

    /** The managed runs StringZilla's device engines lay their state out in. */
    managed_blocks_t& managed() noexcept { return managed_; }

  private:
    metal_executor_t() noexcept = default;

    void swap(metal_executor_t& other) noexcept {
        std::swap(api_, other.api_), std::swap(owned_queue_, other.owned_queue_), std::swap(queue_, other.queue_);
        std::swap(device_, other.device_), std::swap(scratch_limit_, other.scratch_limit_);
        std::swap(kernels_, other.kernels_), std::swap(bounce_, other.bounce_), std::swap(managed_, other.managed_);
    }

    metal_api_t const* api_ = nullptr;

    /** The queue made here and released with the executor, null when the caller lent @c queue_. */
    void* owned_queue_ = nullptr;

    void* queue_ = nullptr;
    int device_ = 0;
    std::size_t scratch_limit_ = 0;
    kernel_table<metal_module_t> const* kernels_ = nullptr;
    ustore_arena_handle_t bounce_ {};
    managed_blocks_t managed_ {};
};

} // namespace unum::ustore::utilities
