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
 *  Memory is unified: every block a kernel reaches is a shared-storage buffer UStore allocates and
 *  registers on its device's @c metal_context_t, and the address the host hands around is its host
 *  address. A launch binds the kernel's arguments packed as one struct and the device's table of
 *  live blocks, which turns every host address the arguments hold, embedded ones included, into a
 *  device one. Every call goes through @c metal_send, so the layer compiles on every host, and one
 *  without Metal lists no Apple device.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`
#include <cstring> // `std::memcpy`, `std::memset`, `std::memmove`, `std::strncpy`

#include <memory>      // `std::unique_ptr`
#include <span>        // `std::span`
#include <type_traits> // `std::type_identity`
#include <utility>     // `std::exchange`, `std::swap`

#include <forkunion/atomics.hpp> // `fu::standard_atomic_ref` - the hold
#include <forkunion/locks.hpp>   // `fu::fair_mutex_t` - a context's lock

#include "ustore/utilities/blocking_files.hpp"    // `shared_library_t`
#include "ustore/utilities/ram_buffers.hpp"       // `pinned_bounce`, `managed_blocks_t`, `free_blocks`
#include "ustore/utilities/ram_device.hpp"        // `no_memory_domain_k`
#include "ustore/utilities/types.hpp"             // `gpu_device_t`, `gpu_devices_capacity_k`, `status_t`
#include "ustore/utilities/vram_apple_shared.hpp" // `metal_arguments_slot_k`, `metal_simdgroup_width_k`

/** Whether this unit embeds the Apple kernels, which CMake sets beside StringZilla's Metal switch,
 *  since the toolchain alone cannot tell such a unit from one that only sees them. */
#if !defined(USTORE_ARCH_METAL_)
#define USTORE_ARCH_METAL_ 0
#endif

namespace unum::ustore::utilities {

#pragma region Runtime ABI

/** The Objective-C runtime's selector, @c SEL, opaque to everyone but the runtime. */
using metal_selector_t = struct metal_selector_opaque_t*;

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

/** @c MTLResourceStorageModeShared, which every block takes, so host and device address
 *  the same bytes. */
inline constexpr std::size_t metal_shared_storage_k = 0;

/** @c MTLResourceUsageRead with @c MTLResourceUsageWrite, as a launch may do either
 *  to every block. */
inline constexpr std::size_t metal_usage_read_write_k = 3;

#pragma endregion Runtime ABI

#pragma region Shared VRAM Driver

/** The Metal Shading Language versions an engine compiles its sources under, each spelled as its
 *  @c MTLLanguageVersion value. */
enum class metal_language_t : std::size_t {
    msl_3_1_k = (3u << 16) | 1u,
    msl_3_2_k = (3u << 16) | 2u,
    msl_4_0_k = (4u << 16) | 0u,
};

/**
 *  @brief The Metal, Objective-C runtime, I/O Kit and Core Foundation entry points every engine
 *      calls, resolved on first use from the system's own frameworks and @c libobjc.
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
    metal_selector_t (*register_selector)(char const*) = nullptr; // vendor-boundary
    void* (*get_class)(char const*) = nullptr;                    // vendor-boundary
    void* (*registry_entry_id_matching)(u64_t) = nullptr;
    u32_t (*get_matching_service)(u32_t, void*) = nullptr;
    void* (*search_registry_property)(u32_t, char const*, void const*, void const*, u32_t) = nullptr; // vendor-boundary
    int (*release_registry_object)(u32_t) = nullptr;
    void* (*create_string)(void const*, char const*, u32_t) = nullptr; // vendor-boundary
    unsigned char (*get_number_value)(void const*, long, void*) = nullptr;
    void (*release_reference)(void const*) = nullptr;
    void* (*autorelease_push)() = nullptr;
    void (*autorelease_pop)(void*) = nullptr;

    /** Whether every library loaded with every entry resolved. */
    explicit operator bool() const noexcept { return autorelease_pop != nullptr; }

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
            runtime.resolve("objc_autoreleasePoolPush", api.autorelease_push) &&
            runtime.resolve("objc_autoreleasePoolPop", api.autorelease_pop);
        return resolved ? api : metal_api_t {};
    }
};

/** Sends @p selector to @p target with @p arguments through @p api, cast to the exact prototype the
 *  call has. */
template <typename result_type_ = void*, typename... argument_types_>
inline result_type_ metal_send(metal_api_t const& api, void* target, char const* selector, // vendor-boundary
                               argument_types_... arguments) noexcept {
    using function_t = result_type_ (*)(void*, metal_selector_t, argument_types_...);
    return reinterpret_cast<function_t>(api.send)(target, api.register_selector(selector), arguments...);
}

/** An @c NSString over @p text, autoreleased. */
inline void* metal_string(metal_api_t const& api, char const* text) noexcept { // vendor-boundary
    return metal_send(api, reinterpret_cast<void*>(api.get_class("NSString")), "stringWithUTF8String:", text);
}

/** One library of MSL source compiled for one Apple device and released with this owner; kernels
 *  are found by name, each built into a pipeline. */
struct metal_module_t {
    using function_t = void*;

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

    /**
     *  @brief Compiles @p source, NUL-terminated MSL text, under @p language on @p device, which
     *      is the @c id<MTLDevice> the library runs on.
     *
     *  @par Statuses:
     *  - @c device_k The @p source is empty or unterminated, or the compiler rejected it.
     */
    static expected<metal_module_t> load(metal_api_t const& api, void* device, bytes_view_t source,
                                         metal_language_t language) noexcept {
        if (source.empty() || source.back() != byte_t {0} || !api || !device)
            return {{}, {status_code_t::device_k, "The device refused the MSL source"}};
        metal_module_t module;
        module.api_ = &api;
        module.device_ = device;
        void* const autoreleased = api.autorelease_push();
        void* const options = metal_send(api, metal_send(api, api.get_class("MTLCompileOptions"), "alloc"), "init");
        metal_send<void>(api, options, "setLanguageVersion:", static_cast<std::size_t>(language));
        void* error = nullptr;
        module.library_ = metal_send(api, device, "newLibraryWithSource:options:error:",
                                     metal_string(api, reinterpret_cast<char const*>(source.data())), options, &error);
        metal_send<void>(api, options, "release");
        api.autorelease_pop(autoreleased);
        if (!module.library_) return {{}, {status_code_t::device_k, "The device refused the MSL source"}};
        return {std::move(module), status_code_t::success_k};
    }

    /** The table the library was compiled through, which launches of its kernels go through. */
    metal_api_t const& api() const noexcept { return *api_; }

    /** The pipeline of the kernel exported as @p name, owned by the caller, or null when the
     *  library has none or its simdgroups are not the @c metal_simdgroup_width_k lanes every MSL
     *  source is written against. */
    function_t function(char const* name) const noexcept { // vendor-boundary
        if (!library_) return nullptr;
        metal_api_t const& api = *api_;
        void* const autoreleased = api.autorelease_push();
        void* const function = metal_send(api, library_, "newFunctionWithName:", metal_string(api, name));
        void* pipeline = nullptr;
        if (function) {
            void* error = nullptr;
            pipeline = metal_send(api, device_, "newComputePipelineStateWithFunction:error:", function, &error);
            metal_send<void>(api, function, "release");
        }
        api.autorelease_pop(autoreleased);
        if (pipeline && metal_send<std::size_t>(api, pipeline, "threadExecutionWidth") != metal_simdgroup_width_k)
            metal_send<void>(api, std::exchange(pipeline, nullptr), "release");
        return pipeline;
    }

  private:
    metal_api_t const* api_ = nullptr;
    void* device_ = nullptr;
    void* library_ = nullptr;
};

#pragma endregion Shared VRAM Driver

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

/** One shared-storage block on an Apple device: its @c id<MTLBuffer> and the host range the same
 *  bytes sit at, which a launch's block table turns into a device address. */
struct metal_block_t {
    void* buffer = nullptr;
    byte_t* host = nullptr;
    std::size_t bytes = 0;
};

/** One listed Apple GPU's process-wide state: the table its calls go through, the device, its
 *  default queue, and every shared block allocated on it, sorted by host address, which a launch's
 *  block table lists; one lock guards the blocks, which executors and arenas allocate anywhere. */
struct metal_context_t {
    metal_api_t const* api = nullptr;

    /** The retained @c id<MTLDevice>. */
    void* device = nullptr;

    /** The device's default @c id<MTLCommandQueue>, the twin of CUDA's null stream. */
    void* queue = nullptr;

    fu::fair_mutex_t lock;
    st::vector<metal_block_t> blocks;
};

/** Every Apple GPU Metal lists, in Metal's own order, each with a default queue, opened once per
 *  process and kept for it, as Metal's own device objects are; a position is an Apple ordinal. */
struct metal_contexts_t {

    /** The context of every listed device, holding its retained @c id<MTLDevice>. */
    metal_context_t contexts[gpu_devices_capacity_k];

    /** What the listing reports of every device: its family as the architecture's major, its cores,
     *  its threadgroup memory and the working set Metal recommends. */
    gpu_device_t listed[gpu_devices_capacity_k] = {};

    std::size_t count = 0;

    metal_contexts_t() noexcept = default;
    metal_contexts_t(metal_contexts_t const&) = delete;
    metal_contexts_t& operator=(metal_contexts_t const&) = delete;

    /** Opens every device @p api lists, none when the table is empty. */
    explicit metal_contexts_t(metal_api_t const& api) noexcept {
        void* const devices = api ? api.copy_all_devices() : nullptr;
        if (!devices) return;
        void* const autoreleased = api.autorelease_push();
        std::size_t const devices_count = metal_send<std::size_t>(api, devices, "count");
        for (std::size_t position = 0; position != devices_count && count != gpu_devices_capacity_k; ++position) {
            void* const device = metal_send(api, devices, "objectAtIndex:", position);
            std::uint32_t const family = apple_family_of(api, device);
            if (!family) continue;
            void* const queue = metal_send(api, device, "newCommandQueue");
            if (!queue) continue;
            contexts[count].api = &api;
            contexts[count].device = metal_send(api, device, "retain");
            contexts[count].queue = queue;
            gpu_device_t& entry = listed[count];
            entry = {gpu_vendor_t::apple_k,
                     static_cast<std::uint32_t>(count),
                     family,
                     0,
                     apple_core_count_of(api, device),
                     static_cast<std::uint32_t>(metal_send<std::size_t>(api, device, "maxThreadgroupMemoryLength")),
                     metal_send<std::uint64_t>(api, device, "recommendedMaxWorkingSetSize")};
            char const* const name = metal_send<char const*>(api, metal_send(api, device, "name"), "UTF8String");
            if (name) std::strncpy(entry.name, name, sizeof(entry.name) - 1);
            ++count;
        }
        api.autorelease_pop(autoreleased);
        metal_send<void>(api, devices, "release");
    }

    /** The context of the device listed at @p ordinal, or null past the listing. */
    metal_context_t* context(int ordinal) noexcept {
        return ordinal >= 0 && static_cast<std::size_t>(ordinal) < count ? &contexts[ordinal] : nullptr;
    }
};

/** Copies every Apple GPU @p contexts opened into @p into, answering how many it wrote. */
inline std::size_t list_apple_gpu_devices(metal_contexts_t const& contexts, std::span<gpu_device_t> into) noexcept {
    std::size_t const count = contexts.count < into.size() ? contexts.count : into.size();
    for (std::size_t position = 0; position != count; ++position) into[position] = contexts.listed[position];
    return count;
}

#pragma region Metal Layer

/** Allocates @p bytes of shared storage on @p context's device and registers it, answering its
 *  host address, or null on exhaustion. */
inline void* metal_allocate(metal_context_t& context, std::size_t bytes) noexcept {
    if (!bytes || !context.api) return nullptr;
    metal_api_t const& api = *context.api;
    st::unique_lock<fu::fair_mutex_t> guard {context.lock};
    if (st::failed(context.blocks.reserve(context.blocks.size() + 1))) return nullptr;
    void* const buffer = metal_send(api, context.device, "newBufferWithLength:options:", bytes, metal_shared_storage_k);
    if (!buffer) return nullptr;
    byte_t* const host = static_cast<byte_t*>(metal_send(api, buffer, "contents"));
    std::size_t position = 0;
    while (position != context.blocks.size() && context.blocks[position].host < host) ++position;
    [[maybe_unused]] st::status_t const inserted = context.blocks.insert(st::assume_reserved_t {}, position,
                                                                         metal_block_t {buffer, host, bytes});
    return host;
}

/** Releases the block at host address @p address that @c metal_allocate answered on @p context's
 *  device; the work still committed on it keeps it alive until it finishes. */
inline void metal_free(metal_context_t& context, void* address) noexcept {
    if (!address || !context.api) return;
    st::unique_lock<fu::fair_mutex_t> guard {context.lock};
    for (std::size_t position = 0; position != context.blocks.size(); ++position) {
        if (context.blocks[position].host != address) continue;
        metal_send<void>(*context.api, context.blocks[position].buffer, "release");
        context.blocks.erase(position);
        return;
    }
}

/** One command buffer being encoded on one queue with a compute encoder for one pipeline: binds
 *  bytes and buffers, dispatches and commits without waiting; one never committed ends its
 *  encoder and is dropped when it dies. */
struct metal_launch_t {
    metal_api_t const* api = nullptr;
    void* commands = nullptr;
    void* encoder = nullptr;

    /** Opens one command buffer on @p queue with a compute encoder for @p pipeline. */
    metal_launch_t(metal_api_t const& api, void* queue, void* pipeline) noexcept : api(&api) {
        void* const autoreleased = api.autorelease_push();
        commands = metal_send(api, queue, "commandBuffer");
        if (commands) metal_send<void>(api, commands, "retain");
        if (commands) encoder = metal_send(api, commands, "computeCommandEncoder");
        if (encoder) {
            metal_send<void>(api, encoder, "retain");
            metal_send<void>(api, encoder, "setComputePipelineState:", pipeline);
        }
        // A command buffer without its encoder is never committed, so none is kept to await
        else if (commands) metal_send<void>(api, std::exchange(commands, nullptr), "release");
        api.autorelease_pop(autoreleased);
    }
    metal_launch_t(metal_launch_t const&) = delete;
    metal_launch_t& operator=(metal_launch_t const&) = delete;
    ~metal_launch_t() noexcept {
        if (encoder) metal_send<void>(*api, encoder, "endEncoding"), metal_send<void>(*api, encoder, "release");
        if (commands) metal_send<void>(*api, commands, "release");
    }

    /** Whether the encoder is open, so the launch can still bind and dispatch. */
    explicit operator bool() const noexcept { return encoder != nullptr; }

    /** Binds @p buffer, an @c id<MTLBuffer>, at buffer slot @p index. */
    void bind(void* buffer, std::size_t index) noexcept {
        metal_send<void>(*api, encoder, "setBuffer:offset:atIndex:", buffer, std::size_t {0}, index);
    }

    /** Copies @p bytes of @p arguments into buffer slot @p index. */
    void bind_bytes(void const* arguments, std::size_t bytes, std::size_t index) noexcept {
        metal_send<void>(*api, encoder, "setBytes:length:atIndex:", arguments, bytes, index);
    }

    /** Dispatches @p groups threadgroups of @p threads each and commits, without waiting. */
    void submit(std::size_t groups, std::size_t threads) noexcept {
        metal_send<void, metal_size_t, metal_size_t>(*api, encoder, "dispatchThreadgroups:threadsPerThreadgroup:",
                                                     metal_size_t {groups}, metal_size_t {threads});
        metal_send<void>(*api, encoder, "endEncoding");
        metal_send<void>(*api, std::exchange(encoder, nullptr), "release");
        metal_send<void>(*api, commands, "commit");
    }
};

/**
 *  @brief Commits one dispatch of @p pipeline on @p queue, a queue on @p context's device:
 *      @p arguments at the arguments slot, the device's table of live blocks at the blocks slot,
 *      every one of them resident, and @p shared_bytes of threadgroup memory, over @p groups
 *      threadgroups of @p threads.
 *
 *  The table is written into a buffer of its own while the registry stays locked, as a block it
 *  lists must not be released before the command buffer retains it.
 *
 *  @par Statuses:
 *  - @c status_code_t::device_k The device refused the encoder or the table.
 */
inline status_t metal_dispatch(metal_context_t& context, void* queue, void* pipeline, std::span<byte_t const> arguments,
                               unsigned groups, unsigned threads, unsigned shared_bytes) noexcept {
    metal_api_t const& api = *context.api;
    metal_launch_t launch {api, queue, static_cast<void*>(pipeline)};
    if (!launch) return {status_code_t::device_k, "The device refused an encoder"};
    void* table = nullptr;
    std::uint32_t table_count = 0;
    {
        st::unique_lock<fu::fair_mutex_t> guard {context.lock};
        table_count = static_cast<std::uint32_t>(context.blocks.size());
        std::size_t const table_bytes = (table_count ? table_count : 1) * std::size_t {metal_block_bytes_k};
        table = metal_send(api, context.device, "newBufferWithLength:options:", table_bytes, metal_shared_storage_k);
        if (table) {
            std::uint64_t* const entries = static_cast<std::uint64_t*>(metal_send(api, table, "contents"));
            for (std::size_t index = 0; index != table_count; ++index) {
                metal_block_t const& block = context.blocks[index];
                entries[4 * index + 0] = reinterpret_cast<std::uintptr_t>(block.host);
                entries[4 * index + 1] = reinterpret_cast<std::uintptr_t>(block.host) + block.bytes;
                entries[4 * index + 2] = metal_send<std::uint64_t>(api, block.buffer, "gpuAddress");
                entries[4 * index + 3] = 0;
                metal_send<void>(api, launch.encoder, "useResource:usage:", block.buffer, metal_usage_read_write_k);
            }
        }
    }
    // The launch drops its uncommitted command buffer, so nothing is left to await
    if (!table) return {status_code_t::device_k, "The device refused the block table"};
    launch.bind_bytes(arguments.data(), arguments.size(), metal_arguments_slot_k);
    launch.bind(table, metal_blocks_slot_k);
    launch.bind_bytes(&table_count, sizeof(table_count), metal_blocks_count_slot_k);
    // The encoder retains the table, so this owner lets it go at once
    metal_send<void>(api, table, "release");
    if (shared_bytes)
        metal_send<void>(api, launch.encoder, "setThreadgroupMemoryLength:atIndex:", std::size_t {shared_bytes},
                         std::size_t {metal_shared_slot_k});
    launch.submit(groups, threads);
    return {};
}

/** Frees every run of @p blocks, an arena on @p context's device, as an arena outliving its
 *  executor is freed; the work still committed on a run keeps it alive until it finishes. */
inline void free_metal_blocks(metal_context_t& context, device_blocks_t& blocks) noexcept {
    free_blocks(blocks,
                [&](std::uintptr_t address) noexcept { metal_free(context, reinterpret_cast<void*>(address)); });
}

#pragma endregion Metal Layer

template <typename module_type_>
struct kernel_table;

/** The kernels a stream launches: the table of its device and the executor's class, which the GPU
 *  runtime keeps, or null when none loaded. */
using metal_kernels_t = kernel_table<metal_module_t> const*;

/**
 *  @brief Allocates @p bytes of shared storage on @p context's device through its registry,
 *      answering their host address, which kernels reach too.
 *
 *  @par Statuses:
 *  - @c out_of_memory_k The device refused the allocation.
 */
inline expected<std::uintptr_t> metal_allocate_block(metal_context_t& context, std::size_t bytes) noexcept {
    void* const address = metal_allocate(context, bytes);
    if (!address) return {{}, {status_code_t::out_of_memory_k, "The device refused an allocation"}};
    return {reinterpret_cast<std::uintptr_t>(address), status_code_t::success_k};
}

/** Releases @p address, which @c metal_allocate_block answered; the work still committed on it
 *  keeps it alive until it finishes. */
inline void metal_free_block(metal_context_t& context, std::uintptr_t address) noexcept {
    metal_free(context, reinterpret_cast<void*>(address));
}

/** Pins nothing, as the device reaches every page of unified memory the host does. */
inline bool pin_host(metal_api_t const&, void*, std::size_t) noexcept { return true; }

/** Unpins nothing, as @c pin_host pinned nothing. */
inline void unpin_host(metal_api_t const&, void*) noexcept {}

/** UStore's memory on one Apple stream: the bounce host verbs write into, the managed runs a
 *  device match lays its engine state out in, the runs it lays its pages out in, and the ceiling
 *  on those runs; every block is unified memory, so managed runs are plain shared storage. */
struct metal_scratch_t {

    /** The memory of the arenas made over the stream, and the kernels its device launches. */
    using arena_blocks_t = metal_blocks_t;
    using function_t = void*;

    pinned_bounce<metal_api_t> bounce;
    managed_blocks_t managed {};
    device_blocks_t blocks {};
    std::size_t limit = 0;

    /** Opens on @p stream: its bounce on the heap, its kernels from the table @p kernels_of answers
     *  for the vendor's module and the stream's ordinal, and a ceiling of @p scratch_limit bytes,
     *  zero for none. */
    template <typename stream_type_, typename kernels_of_type_>
    status_t open(stream_type_& stream, kernels_of_type_& kernels_of, std::size_t& scratch_limit) noexcept {
        bounce = pinned_bounce<metal_api_t> {*stream.handle.context()->api, no_memory_domain_k};
        stream.kernels = kernels_of(std::type_identity<metal_module_t> {}, static_cast<int>(stream.ordinal));
        limit = scratch_limit;
        return {};
    }

    /** Allocates @p bytes the host and the device both reach, which every block on Apple is. */
    template <typename stream_type_>
    expected<std::uintptr_t> allocate_managed(stream_type_ const& stream, std::size_t bytes) noexcept {
        return metal_allocate_block(*stream.handle.context(), bytes);
    }

    /** Releases @p address, which @c allocate_managed answered. */
    template <typename stream_type_>
    void free_managed(stream_type_ const& stream, std::uintptr_t address) noexcept {
        metal_free_block(*stream.handle.context(), address);
    }

    /** Releases the page runs and the managed runs once the drained @p stream ran everything that
     *  binds them; the bounce frees as it dies. */
    template <typename stream_type_>
    void release(stream_type_ const& stream) noexcept {
        metal_context_t& context = *stream.handle.context();
        free_blocks(blocks, [&](std::uintptr_t address) noexcept { metal_free_block(context, address); });
        free_blocks(managed.blocks, [&](std::uintptr_t address) noexcept { metal_free_block(context, address); });
    }
};

#pragma region Shared VRAM Executor

/** Owned @c id<MTLCommandQueue> on one Apple GPU, the twin of @c cuda_stream_t; null rides the
 *  device's default queue, which its @ref metal_context_t keeps, so the stream names its device
 *  and context. Move-only, and released when it dies. */
struct metal_stream_t {

    /** A stream on no device, which only fills the default of a record built later. */
    metal_stream_t() noexcept = default;

    /** Takes @p handle, owned and opened on the device of @p context, or rides that device's
     *  default queue when null. */
    explicit metal_stream_t(metal_context_t& context, void* handle = nullptr) noexcept
        : context_(&context), handle_(handle) {}
    metal_stream_t(metal_stream_t&& other) noexcept
        : context_(other.context_), handle_(std::exchange(other.handle_, nullptr)) {}
    metal_stream_t& operator=(metal_stream_t&& other) noexcept {
        std::swap(context_, other.context_), std::swap(handle_, other.handle_);
        return *this;
    }
    metal_stream_t(metal_stream_t const&) = delete;
    metal_stream_t& operator=(metal_stream_t const&) = delete;
    ~metal_stream_t() noexcept {
        if (handle_) metal_send<void>(*context_->api, handle_, "release");
    }

    /**
     *  @brief Opens a command queue on the device of @p context, owned from now on.
     *
     *  @par Statuses:
     *  - @c device_k The device refused the queue.
     */
    static expected<metal_stream_t> open(metal_context_t& context) noexcept {
        void* const queue = metal_send(*context.api, context.device, "newCommandQueue");
        if (!queue) return {{}, {status_code_t::device_k, "Device refused a command queue"}};
        return {metal_stream_t {context, queue}, status_code_t::success_k};
    }

    /**
     *  @brief Checks that @p handle, a caller's @c id<MTLCommandQueue>, may be adopted on the
     *      device of @p context, without retaining it.
     *
     *  @par Statuses:
     *  - @c invalid_argument_k The @p handle is null.
     *  - @c config_mismatch_k The queue sits on another device.
     */
    static status_t adoptable(metal_context_t const& context, void* handle) noexcept {
        if (!handle) return {status_code_t::invalid_argument_k, "A default stream cannot be adopted"};
        if (metal_send(*context.api, handle, "device") != context.device)
            return {status_code_t::config_mismatch_k, "A stream sits on another device"};
        return {};
    }

    /** The owned queue, or null for the device's default one. */
    void* owned() const noexcept { return handle_; }

    /** The queue work is committed to: the owned one, or the device's default one. */
    void* queue() const noexcept { return handle_ ? handle_ : context_->queue; }

    /** The context of the stream's device, or null for a stream on no device. */
    metal_context_t* context() const noexcept { return context_; }

  private:
    metal_context_t* context_ = nullptr;
    void* handle_ = nullptr;
};

/**
 *  @brief The queues a call commits to, one per entry of the devices it was spawned over, in the
 *      caller's order, each with its kernels and the engine's memory.
 *
 *  Work is committed, never waited on, so one host thread drives every device. Each stream record
 *  is built in place on the heap and never moves; it dies waiting for its queue, then releasing
 *  its memory, then releasing its queue. Apple's memory is unified, so a copy or a fill is a host
 *  one, done when it returns.
 */
struct metal_executor_t {

    /** The engine's memory on one stream. */
    using scratch_t = metal_scratch_t;

    /** One call's hold on the executor: the word it set, which it clears when it dies. Move-only,
     *  so exactly one owner releases the hold. */
    class claim_t {
      public:
        claim_t() noexcept = default;
        explicit claim_t(u32_t& held_word) noexcept : held_word_(&held_word) {}
        claim_t(claim_t&& other) noexcept : held_word_(std::exchange(other.held_word_, nullptr)) {}
        claim_t& operator=(claim_t&& other) noexcept {
            std::swap(held_word_, other.held_word_);
            return *this;
        }
        claim_t(claim_t const&) = delete;
        claim_t& operator=(claim_t const&) = delete;
        ~claim_t() noexcept {
            if (held_word_) fu::standard_atomic_ref<u32_t>(*held_word_).store(0, std::memory_order_release);
        }

      private:
        u32_t* held_word_ = nullptr;
    };

    inline static constexpr gpu_vendor_t vendor_k = gpu_vendor_t::apple_k;

    /** One device's queue, the kernels it launches, and the engine's memory, declared last so it
     *  dies first. */
    struct stream_t {

        /** The Apple ordinal of the device this stream submits to, as the GPU runtime lists it. */
        std::size_t ordinal = 0;

        /** Opened by the executor, adopted from the caller, or the device's default queue. */
        metal_stream_t handle;

        /** The kernels this stream launches, as the engine resolves them when the record opens. */
        metal_kernels_t kernels = {};

        /** The engine's memory, safe to reuse only in this stream's queue order, opened once this
         *  record sits in the executor. */
        scratch_t memory;

        /** Takes @p opened as the queue of device @p device. */
        stream_t(std::size_t device, metal_stream_t&& opened) noexcept : ordinal(device), handle(std::move(opened)) {}
        stream_t(stream_t const&) = delete;
        stream_t& operator=(stream_t const&) = delete;

        /** Waits for an owned queue, then releases the memory; the queue is released after. A
         *  record owning none, a placeholder or the device's default queue, waits for nothing. */
        ~stream_t() noexcept {
            [[maybe_unused]] status_t const ready = handle.owned() ? wait() : bind();
            memory.release(*this);
        }

        /**
         *  @brief Checks the stream names a device, which Metal needs no thread binding for.
         *
         *  @par Statuses:
         *  - @c device_k The stream names no device.
         */
        status_t bind() const noexcept {
            if (!handle.context()) return {status_code_t::device_k, "The stream names no device"};
            return {};
        }

        /**
         *  @brief Waits for every command buffer committed so far, committing an empty one, which
         *      a queue completes only after every one committed before it.
         *
         *  @par Statuses:
         *  - @c device_k The stream names no device, or the device failed the work.
         */
        status_t wait() noexcept {
            // The state of a command buffer the device ran, `MTLCommandBufferStatusCompleted`.
            constexpr std::size_t completed_k = 4;
            metal_context_t const* const context = handle.context();
            if (!context) return {status_code_t::device_k, "The stream names no device"};
            metal_api_t const& api = *context->api;
            void* const autoreleased = api.autorelease_push();
            void* const commands = metal_send(api, handle.queue(), "commandBuffer");
            std::size_t state = 0;
            if (commands) {
                metal_send<void>(api, commands, "commit");
                metal_send<void>(api, commands, "waitUntilCompleted");
                state = metal_send<std::size_t>(api, commands, "status");
            }
            api.autorelease_pop(autoreleased);
            if (state != completed_k) return {status_code_t::device_k, "The device failed the committed work"};
            return {};
        }

        /**
         *  @brief Allocates @p bytes of shared storage on this stream's device through its buffer
         *      registry, answering their host address, which kernels reach too.
         *
         *  @par Statuses:
         *  - @c out_of_memory_k The device refused the allocation.
         *  - @c unsupported_k The engine keeps no buffer registry.
         */
        expected<std::uintptr_t> allocate_async(std::size_t bytes) noexcept {
            return metal_allocate_block(*handle.context(), bytes);
        }

        /** Releases @p address, which @ref allocate_async answered; work still committed on it
         *  keeps it alive until it finishes. */
        void free_async(std::uintptr_t address) noexcept { metal_free_block(*handle.context(), address); }

        /** Copies @p bytes from @p source to @p target in unified memory; done when it returns. */
        status_t copy_async(std::uintptr_t target, std::uintptr_t source, std::size_t bytes) noexcept {
            std::memcpy(reinterpret_cast<void*>(target), reinterpret_cast<void const*>(source), bytes);
            return {};
        }

        /** Copies @p source into memory at @p target, as @ref copy_async does. */
        status_t upload_async(std::uintptr_t target, bytes_view_t source) noexcept {
            return copy_async(target, reinterpret_cast<std::uintptr_t>(source.data()), source.size());
        }

        /** Copies memory at @p source into @p target, as @ref copy_async does. */
        status_t download_async(bytes_span_t target, std::uintptr_t source) noexcept {
            return copy_async(reinterpret_cast<std::uintptr_t>(target.data()), source, target.size());
        }

        /** Sets @p bytes at @p target to @p value in unified memory; done when it returns. */
        status_t fill_async(std::uintptr_t target, byte_t value, std::size_t bytes) noexcept {
            std::memset(reinterpret_cast<void*>(target), static_cast<int>(value), bytes);
            return {};
        }
    };

    metal_executor_t() noexcept = default;
    metal_executor_t(metal_executor_t&& other) noexcept { swap(other); }
    metal_executor_t& operator=(metal_executor_t&& other) noexcept { return swap(other), *this; }
    metal_executor_t(metal_executor_t const&) = delete;
    metal_executor_t& operator=(metal_executor_t const&) = delete;

    /** Exchanges the stream records and the ceiling, none of which moves in memory; the hold
     *  stays, since a move with a call in flight is misuse. */
    void swap(metal_executor_t& other) noexcept {
        std::swap(streams_, other.streams_);
        std::swap(streams_count_, other.streams_count_);
    }

    /**
     *  @brief Claims the executor for one call, answering the hold, which releases it when it dies.
     *
     *  @par Statuses:
     *  - @c contended_k Another call holds the executor.
     */
    expected<claim_t> claim() noexcept {
        u32_t idle = 0;
        if (!fu::standard_atomic_ref<u32_t>(held_).compare_exchange_strong(idle, 1, std::memory_order_acquire,
                                                                           std::memory_order_relaxed))
            return {{}, {status_code_t::contended_k, "Another call holds the executor"}};
        return {claim_t {held_}, status_code_t::success_k};
    }

    /**
     *  @brief Spawns one command queue per entry of @p devices among @p contexts, or retains the
     *      @c id<MTLCommandQueue> of @p streams beside it.
     *
     *  Each stream's memory and kernels open with @p memory_arguments. The executor releases its
     *  own reference to a retained queue when it dies, while the caller keeps and releases theirs.
     *  Adoption follows every fallible step, so a refused spawn retains none of @p streams.
     *
     *  @par Statuses:
     *  - @c invalid_argument_k No device is named, more than a listing holds, the @p streams count
     *    differs from theirs, or a queue is null.
     *  - @c config_mismatch_k A queue sits on another device.
     *  - @c device_k A device is not listed, or refused its queue.
     *  - @c out_of_memory_k A stream record or its memory could not be allocated.
     */
    template <typename... memory_arguments_>
    static expected<metal_executor_t> spawn(metal_contexts_t& contexts, std::span<int const> devices,
                                            std::span<void* const> streams,
                                            memory_arguments_&... memory_arguments) noexcept {
        if (!streams.empty() && streams.size() != devices.size())
            return {{}, {status_code_t::invalid_argument_k, "One stream per device"}};
        for (std::size_t index = 0; index != streams.size(); ++index) {
            metal_context_t* const context = contexts.context(devices[index]);
            if (!context) return {{}, {status_code_t::device_k, "Device is not listed"}};
            if (status_t const checked = metal_stream_t::adoptable(*context, streams[index]); checked.failed())
                return {{}, checked};
        }
        // Adopted queues wait on placeholders that own nothing until every fallible step is behind.
        auto const placeholder = [](metal_context_t& context) noexcept -> expected<metal_stream_t> {
            return {metal_stream_t {context}, status_code_t::success_k};
        };
        auto [executor, build_status] = build(contexts, devices, streams.empty() ? &metal_stream_t::open : +placeholder,
                                              memory_arguments...);
        if (build_status.failed()) return {{}, build_status};
        // Every fallible step is behind, so a refusal above retained none of the caller's queues.
        for (std::size_t index = 0; index != streams.size(); ++index) {
            metal_context_t& context = *contexts.context(devices[index]);
            executor.streams_[index]->handle = metal_stream_t {context,
                                                               metal_send(*context.api, streams[index], "retain")};
        }
        return {std::move(executor), status_code_t::success_k};
    }

    /** Waits for every command buffer committed to every stream, the fleet's completion. */
    status_t wait() noexcept {
        for (std::size_t index = 0; index != streams_count_; ++index)
            if (status_t const drained = streams_[index]->wait(); drained.failed()) return drained;
        return {};
    }

    /** The stream at @p index, in the order the devices were named. */
    stream_t& stream(std::size_t index) noexcept { return *streams_[index]; }
    stream_t const& stream(std::size_t index) const noexcept { return *streams_[index]; }

    /** Streams this executor drives, one per device. */
    std::size_t size() const noexcept { return streams_count_; }

    /** Builds one stream record per entry of @p devices over the queue @p open answers there,
     *  and opens its memory and kernels in place. */
    template <typename... memory_arguments_>
    static expected<metal_executor_t> build(metal_contexts_t& contexts, std::span<int const> devices,
                                            expected<metal_stream_t> (*open)(metal_context_t&),
                                            memory_arguments_&... memory_arguments) noexcept {
        if (devices.empty() || devices.size() > gpu_devices_capacity_k)
            return {{}, {status_code_t::invalid_argument_k, "An executor names one to a listing's devices"}};
        metal_executor_t executor;
        for (int const device : devices) {
            metal_context_t* const context = contexts.context(device);
            if (!context) return {{}, {status_code_t::device_k, "Device is not listed"}};
            auto [opened, open_status] = open(*context);
            if (open_status.failed()) return {{}, open_status};
            std::unique_ptr<stream_t>& record = executor.streams_[executor.streams_count_];
            record.reset(new (std::nothrow) stream_t {static_cast<std::size_t>(device), std::move(opened)});
            if (!record) return {{}, {status_code_t::out_of_memory_k, "No room for a stream record"}};
            ++executor.streams_count_;
            if (status_t const opened_memory = record->memory.open(*record, memory_arguments...);
                opened_memory.failed())
                return {{}, opened_memory};
        }
        return {std::move(executor), status_code_t::success_k};
    }

  private:
    /** One record per named entry, in the caller's order, each on the heap so it never moves. */
    std::unique_ptr<stream_t> streams_[gpu_devices_capacity_k];
    std::size_t streams_count_ = 0;

    /** One while a caller drives the streams, set and cleared through the class's atomic
     *  reference; never swapped. */
    u32_t held_ = 0;
};

#pragma endregion Shared VRAM Executor

/** Every block is unified memory the host reads in place, so no pointer reads as device memory. */
inline int device_of(metal_executor_t::stream_t const&, void const*) noexcept { return -1; }

/** Threadgroup memory one threadgroup on @p stream's device holds, as the listing reports it. */
inline std::uint32_t shared_memory_per_block_bytes(metal_executor_t::stream_t const& stream) noexcept {
    metal_context_t const& context = *stream.handle.context();
    return static_cast<std::uint32_t>(
        metal_send<std::size_t>(*context.api, context.device, "maxThreadgroupMemoryLength"));
}

} // namespace unum::ustore::utilities
