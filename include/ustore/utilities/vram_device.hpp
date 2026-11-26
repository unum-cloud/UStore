/**
 *  @file include/ustore/utilities/vram_device.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The vendor-neutral face of the GPU residency: every vendor's driver, the device
 *      listing, and the kernels every vendor's executor launches.
 *
 *  One binary drives NVIDIA, AMD and Apple devices at once, each vendor through a runtime its
 *  caller loads once and declared without its SDK, so every unit names all three: NVIDIA devices
 *  are listed first, then AMD, then Apple, and a vendor whose runtime is absent lists no devices.
 *  Every vendor's device header provides the surfaces @c a_gpu_stream and @c a_gpu_executor name.
 *  Code that holds an executor and drives its streams takes its @c claim once at its top, which a
 *  second caller finds held; code handed only a stream, or called by a claimed verb, takes none.
 *
 *  The library embeds bundles of kernels, which a device loads into a @c kernel_table the process
 *  keeps, and every executor spawned over that table launches through it, finding each kernel by
 *  its C name; which bundle a device loads, its class, @c src/ picks.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uintptr_t`
#include <cstring> // `std::memcpy`

#include <concepts>    // `std::same_as`, `std::convertible_to`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_trivially_copyable_v`, `std::is_same_v`, `std::type_identity`
#include <utility>     // `std::move`
#include <variant>     // `std::get_if`, `std::variant`, `std::monostate`

#include "ustore/utilities/nvme_linux_cufile_disk.hpp" // `cufile_api_t`
#include "ustore/utilities/ram_buffers.hpp"            // `device_blocks_t`, `managed_blocks_t`, `bump`
#include "ustore/utilities/tapes.hpp"                  // `tape_source_t`, `tape_segment_t`, `tape_block_threads_k`
#include "ustore/utilities/types.hpp"                  // `gpu_device_t`, `status_t`
#include "ustore/utilities/vram_amd_device.hpp"        // `hip_api_t`, `hip_executor_t`, `list_amd_gpu_devices`
#include "ustore/utilities/vram_apple_device.hpp"      // `metal_api_t`, `metal_executor_t`, `list_apple_gpu_devices`
#include "ustore/utilities/vram_nvidia_device.hpp"     // `cuda_api_t`, `cuda_executor_t`, `list_nvidia_gpu_devices`

namespace unum::ustore::utilities {

/** UStore's optional driver entries beside the vendor tables: cuFile's, which reads drives straight
 *  into NVIDIA device memory. */
struct gpu_driver_extensions_t {
    cufile_api_t cufile;

    gpu_driver_extensions_t() noexcept : cufile(cufile_api_t::load()) {}
};

#pragma region Shared VRAM

/** Every vendor's driver table, Apple's device contexts and the engine's optional entries, which a
 *  caller builds once per process and hands to every GPU call; a vendor whose runtime is absent
 *  keeps an empty table. */
struct gpu_drivers_t {
    cuda_api_t cuda;
    hip_api_t hip;
    metal_api_t metal;
    metal_contexts_t metal_contexts;

    /** The engine's optional entries, resolved apart from the tables above. */
    gpu_driver_extensions_t extensions;

    gpu_drivers_t() noexcept
        : cuda(cuda_api_t::load()), hip(hip_api_t::load()), metal(metal_api_t::load()), metal_contexts(metal) {}
    gpu_drivers_t(gpu_drivers_t const&) = delete;
    gpu_drivers_t& operator=(gpu_drivers_t const&) = delete;
};

/** The driver each vendor's executor spawns over: its runtime's table, or Apple's contexts. */
inline cuda_api_t const& driver_of(gpu_drivers_t& drivers, std::type_identity<cuda_executor_t>) noexcept {
    return drivers.cuda;
}
inline hip_api_t const& driver_of(gpu_drivers_t& drivers, std::type_identity<hip_executor_t>) noexcept {
    return drivers.hip;
}
inline metal_contexts_t& driver_of(gpu_drivers_t& drivers, std::type_identity<metal_executor_t>) noexcept {
    return drivers.metal_contexts;
}

/** Writes into @p into every device @p drivers reach, NVIDIA first, then AMD, then Apple, as each
 *  driver reports it, and answers how many it wrote. */
inline std::size_t list_gpu_devices(gpu_drivers_t const& drivers, std::span<gpu_device_t> into) noexcept {
    std::size_t count = list_nvidia_gpu_devices(drivers.cuda, into);
    count += list_amd_gpu_devices(drivers.hip, into.subspan(count));
    count += list_apple_gpu_devices(drivers.metal_contexts, into.subspan(count));
    return count;
}

/** What one vendor's stream answers: its device, binding it, waiting for everything queued, and
 *  the memory operations queued in its order, each with that device bound. */
template <typename stream_type_>
concept a_gpu_stream = requires(stream_type_& stream, stream_type_ const& spawned, std::size_t bytes,
                                std::uintptr_t address, bytes_span_t target) {
    { spawned.ordinal } -> std::convertible_to<std::size_t>;
    { spawned.bind() } -> std::same_as<status_t>;
    { stream.wait() } -> std::same_as<status_t>;
    { stream.allocate_async(bytes) } -> std::same_as<expected<std::uintptr_t>>;
    { stream.free_async(address) };
    { stream.copy_async(address, address, bytes) } -> std::same_as<status_t>;
    { stream.upload_async(address, bytes_view_t {target}) } -> std::same_as<status_t>;
    { stream.download_async(target, address) } -> std::same_as<status_t>;
    { stream.fill_async(address, byte_t {}, bytes) } -> std::same_as<status_t>;
};

/** What every vendor's executor answers: its vendor, and its streams, one per device in the order
 *  the caller named them, and the wait for all of them. */
template <typename executor_type_>
concept a_gpu_executor = a_gpu_stream<typename executor_type_::stream_t> &&
                         requires(executor_type_& executor, executor_type_ const& spawned, std::size_t index) {
                             { executor_type_::vendor_k } -> std::convertible_to<gpu_vendor_t>;
                             { spawned.size() } -> std::same_as<std::size_t>;
                             { executor.stream(index) } -> std::same_as<typename executor_type_::stream_t&>;
                             { executor.wait() } -> std::same_as<status_t>;
                         };

static_assert(a_gpu_executor<cuda_executor_t>);
static_assert(a_gpu_executor<hip_executor_t>);
static_assert(a_gpu_executor<metal_executor_t>);

/** Whichever vendor's executor a VRAM call runs on, or none. */
using gpu_executor_t = std::variant<std::monostate, cuda_executor_t, hip_executor_t, metal_executor_t>;

/**
 *  @brief Spawns the executor of the one vendor every device in @p devices belongs to, over that
 *      vendor's driver in @p drivers, opening each stream's memory and kernels with
 *      @p memory_arguments.
 *
 *  One stream per distinct device over their vendor ordinals, which every shard there shares: the
 *  executor opens it, or adopts the entry of @p streams beside the device, a refused spawn leaving
 *  every one of them the caller's.
 *
 *  @par Statuses:
 *  - @c invalid_argument_k No device is named, one repeats beside @p streams, or a stream is null
 *    or a default one.
 *  - @c config_mismatch_k The @p devices span vendors, or a stream sits on another device than
 *    its entry names.
 *  - @c device_k The build drives no executor of their vendor, or a device refused its stream.
 */
template <typename... memory_arguments_>
expected<gpu_executor_t> spawn_gpu_executor(gpu_drivers_t& drivers, std::span<gpu_device_t const> devices,
                                            std::span<void* const> streams,
                                            memory_arguments_&... memory_arguments) noexcept {
    if (devices.empty() || devices.size() > gpu_devices_capacity_k)
        return {{}, {status_code_t::invalid_argument_k, "An executor names one to a listing's devices"}};
    int ordinals[gpu_devices_capacity_k] = {};
    std::size_t ordinals_count = 0;
    for (gpu_device_t const& device : devices) {
        if (device.vendor != devices[0].vendor)
            return {{}, {status_code_t::config_mismatch_k, "One executor drives one GPU vendor"}};
        int const ordinal = static_cast<int>(device.vendor_ordinal);
        std::size_t earlier = 0;
        while (earlier != ordinals_count && ordinals[earlier] != ordinal) ++earlier;
        // Two adopted streams on one device would leave one of them unowned.
        if (earlier != ordinals_count && !streams.empty())
            return {{}, {status_code_t::invalid_argument_k, "Each adopted stream needs its own device"}};
        if (earlier == ordinals_count) ordinals[ordinals_count++] = ordinal;
    }
    gpu_executor_t executor;
    status_t spawning {status_code_t::device_k, "The build drives no executor of the vendor"};
    auto const spawn = [&]<typename executor_type_>(std::type_identity<executor_type_> vendor) noexcept {
        if (executor_type_::vendor_k != devices[0].vendor) return false;
        auto [spawned, status] = executor_type_::spawn(
            driver_of(drivers, vendor), std::span<int const> {ordinals, ordinals_count}, streams, memory_arguments...);
        if (status.succeeded()) executor.template emplace<executor_type_>(std::move(spawned));
        spawning = status;
        return true;
    };
    spawn(std::type_identity<cuda_executor_t> {}) || spawn(std::type_identity<hip_executor_t> {}) ||
        spawn(std::type_identity<metal_executor_t> {});
    return {std::move(executor), spawning};
}

#pragma endregion Shared VRAM

#pragma region Kernels

/** The C names every vendor's bundle exports its kernels under. */
inline constexpr char const* vram_kernel_names_k[] = {"ustore_tape_offsets_u32",
                                                      "ustore_tape_offsets_u64",
                                                      "ustore_tape_gather",
                                                      "ustore_tape_densify",
                                                      "ustore_merge_rank_inline_12",
                                                      "ustore_merge_pack_inline_12",
                                                      "ustore_merge_place_inline_12",
                                                      "ustore_merge_rank_inline_44",
                                                      "ustore_merge_pack_inline_44",
                                                      "ustore_merge_place_inline_44",
                                                      "ustore_merge_rank_fixed_width",
                                                      "ustore_merge_pack_fixed_width",
                                                      "ustore_merge_place_fixed_width",
                                                      "ustore_merge_rank_variable_width",
                                                      "ustore_merge_pack_variable_width",
                                                      "ustore_merge_place_variable_width",
                                                      "ustore_merge_strings_rank",
                                                      "ustore_merge_strings_pack",
                                                      "ustore_merge_gather_fixed_width",
                                                      "ustore_merge_gather_variable_width",
                                                      "ustore_fold_sort_step",
                                                      "ustore_fold_chains",
                                                      "ustore_fold_write",
                                                      "ustore_substrings_contains_per_thread",
                                                      "ustore_substrings_contains_per_group",
                                                      "ustore_substrings_contains_per_chunk",
                                                      "ustore_regex_contains_per_thread",
                                                      "ustore_regex_contains_per_group",
                                                      "ustore_regex_contains_per_chunk",
                                                      "ustore_subdocuments_contains_per_thread",
                                                      "ustore_subdocuments_contains_per_group",
                                                      "ustore_subdocuments_contains_per_chunk",
                                                      "ustore_subdocuments_find_per_group",
                                                      "ustore_subdocuments_find_per_chunk",
                                                      "ustore_docs_gather_scalars",
                                                      "ustore_docs_measure_texts",
                                                      "ustore_docs_write_texts",
                                                      "ustore_docs_pack_bools",
                                                      "ustore_docs_drop_terminators_u32",
                                                      "ustore_docs_drop_terminators_u64"};

/** The C names every Apple bundle exports its kernels under: every other vendor's but the strings
 *  pack, and the select and the value offsets an Apple merge packs through, with no 64-bit atomics
 *  to chain with. */
inline constexpr char const* vram_apple_kernel_names_k[] = {"ustore_tape_offsets_u32",
                                                            "ustore_tape_offsets_u64",
                                                            "ustore_tape_gather",
                                                            "ustore_tape_densify",
                                                            "ustore_merge_rank_inline_12",
                                                            "ustore_merge_pack_inline_12",
                                                            "ustore_merge_place_inline_12",
                                                            "ustore_merge_rank_inline_44",
                                                            "ustore_merge_pack_inline_44",
                                                            "ustore_merge_place_inline_44",
                                                            "ustore_merge_rank_fixed_width",
                                                            "ustore_merge_pack_fixed_width",
                                                            "ustore_merge_place_fixed_width",
                                                            "ustore_merge_rank_variable_width",
                                                            "ustore_merge_pack_variable_width",
                                                            "ustore_merge_place_variable_width",
                                                            "ustore_merge_strings_rank",
                                                            "ustore_merge_gather_fixed_width",
                                                            "ustore_merge_gather_variable_width",
                                                            "ustore_fold_sort_step",
                                                            "ustore_fold_chains",
                                                            "ustore_fold_write",
                                                            "ustore_substrings_contains_per_thread",
                                                            "ustore_substrings_contains_per_group",
                                                            "ustore_substrings_contains_per_chunk",
                                                            "ustore_regex_contains_per_thread",
                                                            "ustore_regex_contains_per_group",
                                                            "ustore_regex_contains_per_chunk",
                                                            "ustore_subdocuments_contains_per_thread",
                                                            "ustore_subdocuments_contains_per_group",
                                                            "ustore_subdocuments_contains_per_chunk",
                                                            "ustore_subdocuments_find_per_group",
                                                            "ustore_subdocuments_find_per_chunk",
                                                            "ustore_docs_gather_scalars",
                                                            "ustore_docs_measure_texts",
                                                            "ustore_docs_write_texts",
                                                            "ustore_docs_pack_bools",
                                                            "ustore_docs_drop_terminators_u32",
                                                            "ustore_docs_drop_terminators_u64",
                                                            "ustore_select_count_apple",
                                                            "ustore_select_scan_apple",
                                                            "ustore_select_write_apple",
                                                            "ustore_merge_offsets_apple"};

/** One bundle loaded on one device, its kernels resolved by their C names once. */
template <typename module_type_>
struct kernel_table {
    using function_t = typename module_type_::function_t;

    /** The first byte of the bundle the module was loaded from. */
    byte_t const* bundle = nullptr;

    /** The vendor's ordinal of the device holding the module. */
    int device = 0;

    module_type_ module;

    /** The C names of the kernels, in the order of @c functions. */
    std::span<char const* const> names;

    /** The kernel of every name. */
    st::vector<function_t> functions;

    /** The kernel exported as @p name, or null when the bundle has none. */
    function_t function(std::string_view name) const noexcept {
        for (std::size_t index = 0; index != names.size(); ++index)
            if (name == names[index]) return functions[index];
        return nullptr;
    }
};

/**
 *  @brief Queues @p function on @p stream as @p blocks blocks of @p threads threads each, granting
 *      it @p shared_bytes of dynamic shared memory, with @p arguments copied when it launches.
 *
 *  @par Statuses:
 *  - @c status_code_t::device_k The bundle holds no such kernel, or the device refused the window
 *    or the launch.
 */
template <typename... argument_types_>
status_t launch(cuda_executor_t::stream_t& stream, cuda_function_handle_t function, unsigned blocks, unsigned threads,
                unsigned shared_bytes, argument_types_... arguments) noexcept {
    static_assert((std::is_trivially_copyable_v<argument_types_> && ...), "Kernels take their arguments by bytes");
    if (!function) return {status_code_t::device_k, "The bundle holds no such kernel"};
    cuda_api_t const& api = stream.handle.api();
    int granted = 0;
    if (shared_bytes &&
        api.function_get_attribute(&granted, cuda_function_dynamic_shared_bytes_k, function) != cuda_success_k)
        return {status_code_t::device_k, "The kernel reports no shared window"};
    if (shared_bytes > static_cast<unsigned>(granted) &&
        api.function_set_attribute(function, cuda_function_dynamic_shared_bytes_k, static_cast<int>(shared_bytes)) !=
            cuda_success_k)
        return {status_code_t::device_k, "The kernel refused the shared window"};
    void* addresses[] = {static_cast<void*>(&arguments)...};
    cuda_launch_config_t const config {.grid_x = blocks,
                                       .block_x = threads,
                                       .shared_bytes = shared_bytes,
                                       .stream = stream.handle};
    if (api.launch_kernel(&config, function, addresses, nullptr) != cuda_success_k)
        return {status_code_t::device_k, "The device refused the launch"};
    return {};
}

/** @copydoc launch(cuda_executor_t::stream_t&, cuda_function_handle_t, unsigned, unsigned,
 *  unsigned, argument_types_...) */
template <typename... argument_types_>
status_t launch(hip_executor_t::stream_t& stream, hip_function_handle_t function, unsigned blocks, unsigned threads,
                unsigned shared_bytes, argument_types_... arguments) noexcept {
    static_assert((std::is_trivially_copyable_v<argument_types_> && ...), "Kernels take their arguments by bytes");
    if (!function) return {status_code_t::device_k, "The bundle holds no such kernel"};
    hip_api_t const& api = stream.handle.api();
    int granted = 0;
    if (shared_bytes &&
        api.function_get_attribute(&granted, hip_function_dynamic_shared_bytes_k, function) != hip_success_k)
        return {status_code_t::device_k, "The kernel reports no shared window"};
    if (shared_bytes > static_cast<unsigned>(granted) &&
        api.function_set_attribute(static_cast<void const*>(function), hip_function_dynamic_shared_bytes_k,
                                   static_cast<int>(shared_bytes)) != hip_success_k)
        return {status_code_t::device_k, "The kernel refused the shared window"};
    void* addresses[] = {static_cast<void*>(&arguments)...};
    if (api.module_launch_kernel(function, blocks, 1, 1, threads, 1, 1, shared_bytes, stream.handle, addresses,
                                 nullptr) != hip_success_k)
        return {status_code_t::device_k, "The device refused the launch"};
    return {};
}

/** Bytes @p argument_types_ take packed in order, each at its alignment, as a struct lays them. */
template <typename... argument_types_>
consteval std::size_t packed_arguments_bytes() noexcept {
    std::size_t used = 0;
    ((used = round_up_to_multiple(used, alignof(argument_types_)) + sizeof(argument_types_)), ...);
    return used;
}

/**
 *  @brief Commits @p function to @p stream's queue as @p blocks threadgroups of @p threads
 *      threads each, with @p shared_bytes of threadgroup memory and @p arguments packed as one.
 *
 *  The arguments sit in order as one struct, each at its own alignment, and every pointer among
 *  them is a host address the kernel translates through the device's block table.
 *
 *  @par Statuses:
 *  - @c status_code_t::device_k The bundle holds no such kernel, or the device refused the
 *    encoder, the block table or the commit.
 */
template <typename... argument_types_>
status_t launch(metal_executor_t::stream_t& stream, void* function, unsigned blocks, unsigned threads,
                unsigned shared_bytes, argument_types_... arguments) noexcept {
    static_assert((std::is_trivially_copyable_v<argument_types_> && ...), "Kernels take their arguments by bytes");
    constexpr std::size_t packed_bytes_k = packed_arguments_bytes<argument_types_...>();
    static_assert(packed_bytes_k <= metal_arguments_capacity_k, "Metal binds at most 4 KB of arguments by bytes");
    if (!function) return {status_code_t::device_k, "The bundle holds no such kernel"};
    alignas(16) byte_t packed[packed_bytes_k ? packed_bytes_k : 1] = {};
    std::size_t used = 0;
    ((used = round_up_to_multiple(used, alignof(argument_types_)),
      std::memcpy(packed + used, &arguments, sizeof(argument_types_)), used += sizeof(argument_types_)),
     ...);
    return metal_dispatch(*stream.handle.context(), stream.handle.queue(), function, {packed, packed_bytes_k}, blocks,
                          threads, shared_bytes);
}

/**
 *  @brief Lays the strings @p sources name out as a tape on @p stream's device, as the host's
 *      @c assemble_tape does, in two launches on it: the offsets and segments on one block, then
 *      one block per segment.
 *  @param[in] stream The stream of the device holding every span.
 *  @param[in] sources The strings in tape order, in the device's memory.
 *  @param[out] offsets One offset per string and one past the last, the first zero, in
 *      the device's memory.
 *  @param[out] bytes Room for every byte of every string, which the last offset counts, in
 *      the device's memory.
 *
 *  @par Statuses:
 *  - @c status_code_t::device_k The device holds no kernels of the library, or refused a launch.
 */
template <typename offset_type_, a_gpu_stream stream_type_>
status_t assemble_tape(stream_type_& stream, std::span<tape_source_t const> sources, std::span<offset_type_> offsets,
                       std::span<byte_t> bytes) noexcept {
    static_assert(std::is_same_v<offset_type_, std::uint32_t> || std::is_same_v<offset_type_, std::uint64_t>,
                  "A tape's offsets take 32 or 64 bits");
    if (offsets.size() != sources.size() + 1)
        return {status_code_t::invalid_argument_k, "A tape needs offsets, one past strings"};
    auto const* const kernels = stream.kernels;
    if (!kernels) return {status_code_t::device_k, "The device holds no kernels of the library"};
    std::size_t const segments_count = divide_round_up(sources.size(), std::size_t {tape_sources_per_segment_k});
    std::uintptr_t segments = 0;
    if (segments_count) {
        auto [allocated, allocate_status] = stream.allocate_async(segments_count * sizeof(tape_segment_t));
        if (allocate_status.failed()) return allocate_status;
        segments = allocated;
    }
    char const* const offsets_kernel = std::is_same_v<offset_type_, std::uint32_t> ? "ustore_tape_offsets_u32"
                                                                                   : "ustore_tape_offsets_u64";
    status_t status = launch(stream, kernels->function(offsets_kernel), 1u, tape_block_threads_k, 0u, sources.data(),
                             std::uint64_t {sources.size()}, tape_sources_per_segment_k, offsets.data(),
                             reinterpret_cast<tape_segment_t*>(segments));
    if (status.succeeded() && segments_count)
        status = launch(stream, kernels->function("ustore_tape_gather"), static_cast<unsigned>(segments_count),
                        tape_block_threads_k, 0u, sources.data(), reinterpret_cast<tape_segment_t const*>(segments),
                        bytes.data());
    if (segments) stream.free_async(segments);
    return status;
}

#pragma endregion Kernels

#pragma region Device Arenas

/** Bumps @p count elements off @p blocks, opening a run through @p allocate when the newest is
 *  full, each carve padded to @c arena_alignment_k with its tail zeroed on @p stream, so a column's
 *  bitmap reads whole. A run opened under the stream's scratch limit shrinks to fit what the listed
 *  runs leave of it, or is refused. */
template <typename element_type_, a_gpu_stream stream_type_, typename allocate_type_>
expected<std::span<element_type_>> carve_runs(device_blocks_t& blocks, stream_type_& stream, std::size_t count,
                                              allocate_type_&& allocate) noexcept {
    if (!count) return std::span<element_type_> {};
    std::size_t const length = sizeof(element_type_) * count;
    std::size_t const padded = round_up_to_multiple(length, arena_alignment_k);
    std::uintptr_t address = bump(blocks, padded);
    if (!address) {
        if (blocks.count == blocks.blocks.size())
            return status_t {status_code_t::out_of_memory_k, "The device arena lists every run it holds"};
        std::size_t capacity = grown_capacity(blocks, padded);
        if (std::size_t const limit = stream.memory.limit; limit) {
            std::size_t held = 0;
            for (std::uint32_t block = 0; block != blocks.count; ++block) held += blocks.blocks[block].capacity;
            std::size_t const left = held < limit ? limit - held : 0;
            if (left < padded)
                return status_t {status_code_t::out_of_memory_k, "The carve passes the device's scratch limit"};
            capacity = fu::min_of(capacity, left);
        }
        auto const [opened, allocate_status] = allocate(capacity);
        if (allocate_status.failed()) return allocate_status;
        blocks.blocks[blocks.count++] = {opened, capacity};
        blocks.used = 0;
        address = bump(blocks, padded);
    }
    if (padded != length)
        if (status_t const status = stream.fill_async(address + length, byte_t {0}, padded - length); status.failed())
            return status;
    return std::span<element_type_> {reinterpret_cast<element_type_*>(address), count};
}

/** Carves @p count elements of device memory off @p blocks on @p stream: a warm carve is a host
 *  bump and at most one fill, and a cold one adds one stream-ordered allocation at least doubling
 *  the newest run. */
template <typename element_type_, a_gpu_stream stream_type_>
expected<std::span<element_type_>> carve(device_blocks_t& blocks, stream_type_& stream, std::size_t count) noexcept {
    return carve_runs<element_type_>(blocks, stream, count,
                                     [&](std::size_t bytes) noexcept { return stream.allocate_async(bytes); });
}

/** Carves @p count elements of managed memory off @p managed, which the host and @p stream's
 *  device both reach. */
template <typename element_type_, a_gpu_stream stream_type_>
expected<std::span<element_type_>> carve(managed_blocks_t& managed, stream_type_& stream, std::size_t count) noexcept {
    return carve_runs<element_type_>(managed.blocks, stream, count, [&](std::size_t bytes) noexcept {
        return stream.memory.allocate_managed(stream, bytes);
    });
}

/** Rewinds @p runs to their newest run, the largest, freeing every older one in @p stream's
 *  order. */
template <a_gpu_stream stream_type_>
void rewind(device_blocks_t& runs, stream_type_& stream) noexcept {
    if (runs.count > 1) {
        for (std::uint32_t block = 0; block + 1 != runs.count; ++block) stream.free_async(runs.blocks[block].address);
        runs.blocks[0] = runs.blocks[runs.count - 1];
        runs.count = 1;
    }
    runs.used = 0;
}

/** Rewinds @p managed to their newest run, freeing every older one once @p stream's device is
 *  done. */
template <a_gpu_stream stream_type_>
void rewind(managed_blocks_t& managed, stream_type_& stream) noexcept {
    device_blocks_t& runs = managed.blocks;
    if (runs.count > 1) {
        for (std::uint32_t block = 0; block + 1 != runs.count; ++block)
            stream.memory.free_managed(stream, runs.blocks[block].address);
        runs.blocks[0] = runs.blocks[runs.count - 1];
        runs.count = 1;
    }
    runs.used = 0;
}

/** The device runs of @p arena when they live on @p stream's device, or null for any other. */
template <a_gpu_stream stream_type_>
device_blocks_t* device_blocks_of(ustore_arena_t arena, stream_type_ const& stream) noexcept {
    using arena_blocks_t = typename decltype(stream.memory)::arena_blocks_t;
    arena_blocks_t* const memory = arena ? std::get_if<arena_blocks_t>(&arena->memory) : nullptr;
    return memory && memory->device == static_cast<int>(stream.ordinal) ? &memory->blocks : nullptr;
}

/** Whether @p pointer lies on a device of a vendor other than the stream's, which @p drivers
 *  answer; Apple's unified memory reads as host memory to every vendor. */
inline bool on_another_vendor(gpu_drivers_t const& drivers, cuda_executor_t::stream_t const&,
                              void const* pointer) noexcept {
    return hip_device_of(drivers.hip, pointer) >= 0;
}
inline bool on_another_vendor(gpu_drivers_t const& drivers, hip_executor_t::stream_t const&,
                              void const* pointer) noexcept {
    return cuda_device_of(drivers.cuda, pointer) >= 0;
}
inline bool on_another_vendor(gpu_drivers_t const& drivers, metal_executor_t::stream_t const&,
                              void const* pointer) noexcept {
    return cuda_device_of(drivers.cuda, pointer) >= 0 || hip_device_of(drivers.hip, pointer) >= 0;
}

#pragma endregion Device Arenas

} // namespace unum::ustore::utilities
