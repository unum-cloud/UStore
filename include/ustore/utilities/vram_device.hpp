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
 *  Every vendor's device header provides the surface @c a_gpu_executor names.
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
#include <type_traits> // `std::is_trivially_copyable_v`, `std::is_same_v`
#include <utility>     // `std::move`
#include <variant>     // `std::get_if`

#include "ustore/utilities/nvme_linux_cufile_disk.hpp" // `cufile_api_t`
#include "ustore/utilities/ram_buffers.hpp"            // `device_blocks_t`, `managed_blocks_t`, `bump`
#include "ustore/utilities/tapes.hpp"                  // `tape_source_t`, `tape_segment_t`, `tape_block_threads_k`
#include "ustore/utilities/types.hpp"                  // `gpu_device_t`, `status_t`
#include "ustore/utilities/vram_amd_device.hpp"        // `hip_api_t`, `hip_executor_t`, `list_amd_gpu_devices`
#include "ustore/utilities/vram_apple_device.hpp"      // `metal_api_t`, `metal_executor_t`, `list_apple_gpu_devices`
#include "ustore/utilities/vram_nvidia_device.hpp"     // `cuda_api_t`, `cuda_executor_t`, `list_nvidia_gpu_devices`

namespace unum::ustore::utilities {

/** What every vendor's executor answers: its vendor and the arena memory it carves, one device,
 *  one stream it waits on, the ceiling on its scratch, the memory entries arenas and towers queue
 *  on its stream, and its pinned bounce and managed runs. */
template <typename executor_type_>
concept a_gpu_executor = requires(executor_type_& executor, executor_type_ const& spawned, std::size_t bytes,
                                  std::uintptr_t address, std::span<byte_t> target,
                                  typename executor_type_::arena_blocks_t& arena) {
    { executor_type_::vendor_k } -> std::convertible_to<gpu_vendor_t>;
    { arena.blocks } -> std::convertible_to<device_blocks_t&>;
    { executor.wait() } -> std::same_as<status_t>;
    { executor.bind() } -> std::same_as<status_t>;
    { spawned.device() } -> std::same_as<gpu_device_t const&>;
    { spawned.device_of(target.data()) } -> std::same_as<int>;
    { spawned.scratch_limit() } -> std::same_as<std::size_t>;
    { executor.allocate_async(bytes) } -> std::same_as<expected<std::uintptr_t>>;
    { executor.allocate_managed(bytes) } -> std::same_as<expected<std::uintptr_t>>;
    { executor.free_async(address) };
    { executor.free_managed(address) };
    { executor.fill_async(address, byte_t {}, bytes) } -> std::same_as<status_t>;
    { executor.upload_async(address, std::span<byte_t const> {target}) } -> std::same_as<status_t>;
    { executor.download_async(target, address) } -> std::same_as<status_t>;
    { executor.copy_async(address, address, bytes) } -> std::same_as<status_t>;
    { executor.bounce() } -> std::same_as<ustore_arena_t>;
    { executor.rewind_bounce() };
    { executor.pin_bounce() } -> std::same_as<status_t>;
    { executor.managed() } -> std::same_as<managed_blocks_t&>;
};

static_assert(a_gpu_executor<cuda_executor_t>);
static_assert(a_gpu_executor<hip_executor_t>);
static_assert(a_gpu_executor<metal_executor_t>);

/** Every vendor's driver table and cuFile's, which a process builds once and hands to every GPU call; a library that
 *  is absent keeps an empty table. */
struct gpu_drivers_t {
    cuda_api_t cuda;
    hip_api_t hip;
    metal_api_t metal;
    cufile_api_t cufile;

    /** The primary context of every NVIDIA device by CUDA ordinal, null until @c retain_context
     *  retains it, and never released, as the driver releases them at exit. */
    cuda_context_handle_t cuda_contexts[gpu_devices_capacity_k] = {};

    /** Every Apple device Metal lists, retained for the process. */
    metal_devices_t metal_devices;

    gpu_drivers_t() noexcept
        : cuda(cuda_api_t::load()), hip(hip_api_t::load()), metal(metal_api_t::load()), cufile(cufile_api_t::load()),
          metal_devices(metal) {}
    gpu_drivers_t(gpu_drivers_t const&) = delete;
    gpu_drivers_t& operator=(gpu_drivers_t const&) = delete;

    /** The primary context of CUDA ordinal @p ordinal, retained by the first call, which runs
     *  alone, or null where the driver refuses it. */
    cuda_context_handle_t retain_context(int ordinal) noexcept {
        if (!cuda || ordinal < 0 || ordinal >= static_cast<int>(gpu_devices_capacity_k)) return nullptr;
        cuda_context_handle_t& context = cuda_contexts[ordinal];
        int handle = 0;
        if (!context && (cuda.get_device(&handle, ordinal) != cuda_success_k ||
                         cuda.retain_primary_context(&context, handle) != cuda_success_k))
            context = nullptr;
        return context;
    }
};

/** Writes into @p into every device @p drivers reach, NVIDIA first, then AMD, then Apple, as each
 *  driver reports it, and answers how many it wrote. */
inline std::size_t list_gpu_devices(gpu_drivers_t const& drivers, std::span<gpu_device_t> into) noexcept {
    std::size_t count = list_nvidia_gpu_devices(drivers.cuda, into);
    count += list_amd_gpu_devices(drivers.hip, into.subspan(count));
    return count + list_apple_gpu_devices(drivers.metal_devices, into.subspan(count));
}

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
 *  @brief Queues @p function on @p executor's stream as @p blocks blocks of @p threads
 *      threads each, granting it @p shared_bytes of dynamic shared memory, with @p arguments
 *      copied at the launch.
 *
 *  @par Statuses:
 *  - @c status_code_t::device_k The bundle holds no such kernel, or the device refused the window
 *    or the launch.
 */
template <typename... argument_types_>
status_t launch(cuda_executor_t& executor, cuda_function_handle_t function, unsigned blocks, unsigned threads,
                unsigned shared_bytes, argument_types_... arguments) noexcept {
    static_assert((std::is_trivially_copyable_v<argument_types_> && ...), "Kernels take their arguments by bytes");
    if (!function) return {status_code_t::device_k, "The bundle holds no such kernel"};
    cuda_api_t const& api = executor.api();
    int granted = 0;
    if (shared_bytes &&
        api.function_get_attribute(&granted, cuda_function_dynamic_shared_bytes_k, function) != cuda_success_k)
        return {status_code_t::device_k, "The kernel reports no shared window"};
    if (shared_bytes > static_cast<unsigned>(granted) &&
        api.function_set_attribute(function, cuda_function_dynamic_shared_bytes_k, static_cast<int>(shared_bytes)) !=
            cuda_success_k)
        return {status_code_t::device_k, "The kernel refused the shared window"};
    void* addresses[] = {static_cast<void*>(&arguments)...};
    if (api.launch_kernel(function, blocks, 1, 1, threads, 1, 1, shared_bytes, executor.stream(), addresses, nullptr) !=
        cuda_success_k)
        return {status_code_t::device_k, "The device refused the launch"};
    return {};
}

/** @copydoc launch(cuda_executor_t&, cuda_function_handle_t, unsigned, unsigned,
 *  unsigned, argument_types_...) */
template <typename... argument_types_>
status_t launch(hip_executor_t& executor, hip_function_handle_t function, unsigned blocks, unsigned threads,
                unsigned shared_bytes, argument_types_... arguments) noexcept {
    static_assert((std::is_trivially_copyable_v<argument_types_> && ...), "Kernels take their arguments by bytes");
    if (!function) return {status_code_t::device_k, "The bundle holds no such kernel"};
    hip_api_t const& api = executor.api();
    int granted = 0;
    if (shared_bytes &&
        api.function_get_attribute(&granted, hip_function_dynamic_shared_bytes_k, function) != hip_success_k)
        return {status_code_t::device_k, "The kernel reports no shared window"};
    if (shared_bytes > static_cast<unsigned>(granted) &&
        api.function_set_attribute(static_cast<void const*>(function), hip_function_dynamic_shared_bytes_k,
                                   static_cast<int>(shared_bytes)) != hip_success_k)
        return {status_code_t::device_k, "The kernel refused the shared window"};
    void* addresses[] = {static_cast<void*>(&arguments)...};
    if (api.module_launch_kernel(function, blocks, 1, 1, threads, 1, 1, shared_bytes, executor.stream(), addresses,
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
 *  @brief Commits @p function on @p executor's queue as @p blocks threadgroups of @p threads
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
status_t launch(metal_executor_t& executor, metal_function_handle_t function, unsigned blocks, unsigned threads,
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
    return metal_dispatch(executor.stream(), function, {packed, packed_bytes_k}, blocks, threads, shared_bytes);
}

/**
 *  @brief Lays the strings @p sources name out as a tape on @p executor's device, as the
 *      host's @c assemble_tape does, in two launches on its stream: the offsets and segments
 *      on one block, then one block per segment.
 *  @param[in] executor The executor whose device holds every span.
 *  @param[in] sources The strings in tape order, in the device's memory.
 *  @param[out] offsets One offset per string and one past the last, the first zero, in
 *      the device's memory.
 *  @param[out] bytes Room for every byte of every string, which the last offset counts, in
 *      the device's memory.
 *
 *  @par Statuses:
 *  - @c status_code_t::device_k The device holds no kernels of the library, or refused a launch.
 */
template <typename offset_type_, a_gpu_executor executor_type_>
status_t assemble_tape(executor_type_& executor, std::span<tape_source_t const> sources,
                       std::span<offset_type_> offsets, std::span<byte_t> bytes) noexcept {
    static_assert(std::is_same_v<offset_type_, std::uint32_t> || std::is_same_v<offset_type_, std::uint64_t>,
                  "A tape's offsets take 32 or 64 bits");
    if (offsets.size() != sources.size() + 1)
        return {status_code_t::invalid_argument_k, "A tape needs offsets, one past strings"};
    if (!executor.kernels()) return {status_code_t::device_k, "The device holds no kernels of the library"};
    std::size_t const segments_count = divide_round_up(sources.size(), std::size_t {tape_sources_per_segment_k});
    tape_segment_t* segments = nullptr;
    if (segments_count) {
        auto const [allocated, allocate_status] = executor.allocate_async(segments_count * sizeof(tape_segment_t));
        if (allocate_status.failed()) return allocate_status;
        segments = reinterpret_cast<tape_segment_t*>(allocated);
    }
    char const* const offsets_kernel = std::is_same_v<offset_type_, std::uint32_t> ? "ustore_tape_offsets_u32"
                                                                                   : "ustore_tape_offsets_u64";
    status_t status = launch(executor, executor.kernels()->function(offsets_kernel), 1u, tape_block_threads_k, 0u,
                             sources.data(), std::uint64_t {sources.size()}, tape_sources_per_segment_k, offsets.data(),
                             segments);
    if (status.succeeded() && segments_count)
        status = launch(executor, executor.kernels()->function("ustore_tape_gather"),
                        static_cast<unsigned>(segments_count), tape_block_threads_k, 0u, sources.data(),
                        static_cast<tape_segment_t const*>(segments), bytes.data());
    if (segments) executor.free_async(reinterpret_cast<std::uintptr_t>(segments));
    return status;
}

#pragma endregion Kernels

#pragma region Device Arenas

/** Bumps @p count elements off @p blocks, opening a run through @p allocate_ when the newest is full, each carve
 *  padded to @c arena_alignment_k with its tail zeroed on @p executor's stream, so a column's bitmap reads whole. A
 *  run opened under the executor's scratch limit shrinks to fit what the listed runs leave of it, or is refused. */
template <typename element_type_, a_gpu_executor executor_type_, typename allocate_type_>
expected<std::span<element_type_>> carve_runs(device_blocks_t& blocks, executor_type_& executor, std::size_t count,
                                              allocate_type_&& allocate) noexcept {
    if (!count) return std::span<element_type_> {};
    std::size_t const length = sizeof(element_type_) * count;
    std::size_t const padded = round_up_to_multiple(length, arena_alignment_k);
    std::uintptr_t address = bump(blocks, padded);
    if (!address) {
        if (blocks.count == blocks.blocks.size())
            return status_t {status_code_t::out_of_memory_k, "The device arena lists every run it holds"};
        std::size_t capacity = grown_capacity(blocks, padded);
        if (std::size_t const limit = executor.scratch_limit(); limit) {
            std::size_t held = 0;
            for (std::uint32_t block = 0; block != blocks.count; ++block) held += blocks.blocks[block].capacity;
            std::size_t const left = held < limit ? limit - held : 0;
            if (left < padded)
                return status_t {status_code_t::out_of_memory_k, "The carve passes the executor's scratch limit"};
            capacity = fu::min_of(capacity, left);
        }
        auto const [opened, allocate_status] = allocate(capacity);
        if (allocate_status.failed()) return allocate_status;
        blocks.blocks[blocks.count++] = {opened, capacity};
        blocks.used = 0;
        address = bump(blocks, padded);
    }
    if (padded != length)
        if (status_t const status = executor.fill_async(address + length, byte_t {0}, padded - length); status.failed())
            return status;
    return std::span<element_type_> {reinterpret_cast<element_type_*>(address), count};
}

/** Carves @p count elements of device memory off @p blocks on @p executor's stream: a warm carve is a host bump and at
 *  most one fill, and a cold one adds one stream-ordered allocation at least doubling the newest run. */
template <typename element_type_, a_gpu_executor executor_type_>
expected<std::span<element_type_>> carve(device_blocks_t& blocks, executor_type_& executor,
                                         std::size_t count) noexcept {
    return carve_runs<element_type_>(blocks, executor, count,
                                     [&](std::size_t bytes) noexcept { return executor.allocate_async(bytes); });
}

/** Carves @p count elements of managed memory off @p blocks, which the host and @p executor's device both reach. */
template <typename element_type_, a_gpu_executor executor_type_>
expected<std::span<element_type_>> carve(managed_blocks_t& blocks, executor_type_& executor,
                                         std::size_t count) noexcept {
    return carve_runs<element_type_>(blocks.blocks, executor, count,
                                     [&](std::size_t bytes) noexcept { return executor.allocate_managed(bytes); });
}

/** Rewinds @p blocks to their newest run, the largest, freeing every older one in stream order on @p executor. */
template <a_gpu_executor executor_type_>
void rewind(device_blocks_t& blocks, executor_type_& executor) noexcept {
    if (blocks.count > 1) {
        for (std::uint32_t block = 0; block + 1 != blocks.count; ++block)
            executor.free_async(blocks.blocks[block].address);
        blocks.blocks[0] = blocks.blocks[blocks.count - 1];
        blocks.count = 1;
    }
    blocks.used = 0;
}

/** Rewinds managed @p blocks to their newest run, freeing every older one once @p executor's device is done. */
template <a_gpu_executor executor_type_>
void rewind(managed_blocks_t& blocks, executor_type_& executor) noexcept {
    device_blocks_t& runs = blocks.blocks;
    if (runs.count > 1) {
        for (std::uint32_t block = 0; block + 1 != runs.count; ++block)
            executor.free_managed(runs.blocks[block].address);
        runs.blocks[0] = runs.blocks[runs.count - 1];
        runs.count = 1;
    }
    runs.used = 0;
}

/** The device runs of @p arena when they live on @p executor's device, or null for any other arena. */
template <a_gpu_executor executor_type_>
device_blocks_t* device_blocks_of(ustore_arena_t arena, executor_type_ const& executor) noexcept {
    using arena_blocks_t = typename executor_type_::arena_blocks_t;
    arena_blocks_t* const memory = arena ? std::get_if<arena_blocks_t>(&arena->memory) : nullptr;
    return memory && memory->device == static_cast<int>(executor.device().vendor_ordinal) ? &memory->blocks : nullptr;
}

/** Whether @p pointer lies on a device of a vendor other than @p executor_type_'s, which
 *  @p drivers answer; Apple's unified memory reads as host memory to every vendor. */
template <a_gpu_executor executor_type_>
bool on_another_vendor(gpu_drivers_t const& drivers, void const* pointer) noexcept {
    if constexpr (executor_type_::vendor_k == gpu_vendor_t::nvidia_k) return hip_device_of(drivers.hip, pointer) >= 0;
    else if constexpr (executor_type_::vendor_k == gpu_vendor_t::amd_k)
        return cuda_device_of(drivers.cuda, pointer) >= 0;
    else return cuda_device_of(drivers.cuda, pointer) >= 0 || hip_device_of(drivers.hip, pointer) >= 0;
}

#pragma endregion Device Arenas

} // namespace unum::ustore::utilities
