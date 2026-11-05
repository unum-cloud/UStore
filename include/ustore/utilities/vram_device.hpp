/**
 *  @file include/ustore/utilities/vram_device.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The vendor-neutral face of the GPU residency: every vendor's driver, the device listing, and one executor
 *      spawned over any vendor.
 *
 *  One binary drives NVIDIA and AMD devices at once, each vendor through a runtime its caller loads
 *  once and declared without its SDK, so every unit names both: NVIDIA devices are listed first,
 *  then AMD, and a vendor whose runtime is absent contributes no devices. Every vendor's device
 *  header provides the same surface, which @c gpu_executor_concept names.
 *
 *  The library embeds one bundle of kernels per vendor, which every device loads at its first spawn
 *  into a @c kernel_table the process keeps, and every executor spawned there launches through,
 *  finding each kernel by its C name.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uintptr_t`

#include <algorithm>   // `std::min`
#include <concepts>    // `std::same_as`, `std::convertible_to`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_trivially_copyable_v`, `std::is_same_v`
#include <utility>     // `std::move`
#include <variant>     // `std::get_if`

#include <stringzilla/capabilities.h> // `sz_capability_t`

#include "ustore/utilities/nvme_linux_cufile_disk.hpp" // `cufile_api_t`
#include "ustore/utilities/ram_buffers.hpp"            // `device_blocks_t`, `managed_blocks_t`, `bump`
#include "ustore/utilities/tapes.hpp"                  // `tape_source_t`, `tape_segment_t`, `tape_block_threads_k`
#include "ustore/utilities/types.hpp"                  // `gpu_device_t`, `gpu_class_t`, `status_t`
#include "ustore/utilities/vram_amd_device.hpp"        // `hip_api_t`, `hip_executor_t`, `list_amd_gpu_devices`
#include "ustore/utilities/vram_nvidia_device.hpp"     // `cuda_api_t`, `cuda_executor_t`, `list_nvidia_gpu_devices`

namespace unum::ustore::utilities {

/** What every vendor's executor answers: its vendor and the arena memory it carves, one device, one stream it waits
 *  on, the device's class and the StringZilla capabilities it runs, the ceiling on its scratch, the memory entries
 *  arenas and towers queue on its stream, and its pinned bounce and managed runs. */
template <typename executor_type_>
concept gpu_executor_concept = requires(executor_type_& executor, executor_type_ const& spawned, std::size_t bytes,
                                        std::uintptr_t address, std::span<byte_t> target,
                                        typename executor_type_::arena_blocks_t& arena) {
    { executor_type_::vendor_k } -> std::convertible_to<gpu_vendor_t>;
    { arena.blocks } -> std::convertible_to<device_blocks_t&>;
    { executor.wait() } -> std::same_as<status_t>;
    { executor.bind() } -> std::same_as<status_t>;
    { spawned.device() } -> std::same_as<int>;
    { spawned.device_of(target.data()) } -> std::same_as<int>;
    { spawned.gpu_class() } -> std::same_as<gpu_class_t>;
    { spawned.capabilities() } -> std::same_as<sz_capability_t>;
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

static_assert(gpu_executor_concept<cuda_executor_t>);
static_assert(gpu_executor_concept<hip_executor_t>);

/** Every vendor's driver table and cuFile's, which a process builds once and hands to every GPU call; a library that
 *  is absent keeps an empty table. */
struct gpu_drivers_t {
    cuda_api_t cuda;
    hip_api_t hip;
    cufile_api_t cufile;

    /** The primary context of every NVIDIA device by CUDA ordinal, null until @c retain_context
     *  retains it, and never released, as the driver releases them at exit. */
    cuda_context_handle_t cuda_contexts[gpu_devices_capacity_k] = {};

    gpu_drivers_t() noexcept : cuda(cuda_api_t::load()), hip(hip_api_t::load()), cufile(cufile_api_t::load()) {}
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

/** Writes into @p into every device @p drivers reach, NVIDIA first, then AMD, as each driver reports it, and answers
 *  how many it wrote. */
inline std::size_t list_gpu_devices(gpu_drivers_t const& drivers, std::span<gpu_device_t> into) noexcept {
    std::size_t const count = list_nvidia_gpu_devices(drivers.cuda, into);
    return count + list_amd_gpu_devices(drivers.hip, into.subspan(count));
}

#pragma region Kernels

/** The C names every vendor's bundle exports its kernels under. */
inline constexpr char const* vram_kernel_names_k[] = {"ustore_tape_offsets_u32",
                                                      "ustore_tape_offsets_u64",
                                                      "ustore_tape_gather",
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
                                                      "ustore_subdocuments_contains_per_chunk"};

/** The bundle of NVIDIA kernels the library embeds, empty when its build named no CUDA codes. */
std::span<byte_t const> vram_nvidia_bundle() noexcept;

/** The bundle of AMD kernels the library embeds, empty when its build named no ROCm processors. */
std::span<byte_t const> vram_amd_bundle() noexcept;

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

/** The bundle every device of one vendor loaded, one table per device by its vendor's ordinal,
 *  found by the bundle's address. The first spawn on a device loads its table alone, and every
 *  later one only reads it. */
template <typename module_type_>
class kernel_tables {
  public:

    /**
     *  @brief Loads @p bundle onto @p device through @p api and resolves @p names, unless its table
     *      holds it already.
     *  @param[in] api The vendor's driver table.
     *  @param[in] place Where the module loads: an NVIDIA device's primary context, or an
     *      AMD device's ordinal.
     *  @param[in] device The vendor's ordinal of the device.
     *  @param[in] bundle The bundle, which outlives the tables.
     *  @param[in] names The C names of the kernels to resolve, which outlive the tables.
     *
     *  @par Statuses:
     *  - @c status_code_t::device_k The ordinal passes the devices UStore lists, the device refused
     *    the bundle, or the bundle exports no kernel of a name.
     */
    template <typename api_type_, typename place_type_>
    status_t load(api_type_ const& api, place_type_ place, int device, std::span<byte_t const> bundle,
                  std::span<char const* const> names) noexcept {
        if (device < 0 || device >= static_cast<int>(gpu_devices_capacity_k))
            return {status_code_t::device_k, "The ordinal passes the devices UStore lists"};
        if (table_of(bundle, device)) return {};
        expected<module_type_> loaded = module_type_::load(api, place, bundle);
        if (!loaded) return loaded.status();
        kernel_table<module_type_>& table = tables_[device];
        if (st::failed(table.functions.resize(names.size())))
            return {status_code_t::out_of_memory_k, "No room for the kernel table of a bundle"};
        for (std::size_t index = 0; index != names.size(); ++index)
            if (!(table.functions[index] = loaded->function(names[index])))
                return {status_code_t::device_k, "The bundle exports no kernel of a name"};
        table.device = device, table.module = std::move(*loaded), table.names = names;
        table.bundle = bundle.data();
        return {};
    }

    /** The table of @p bundle on @p device, or null where it never loaded. */
    kernel_table<module_type_> const* table_of(std::span<byte_t const> bundle, int device) const noexcept {
        if (device < 0 || device >= static_cast<int>(gpu_devices_capacity_k)) return nullptr;
        kernel_table<module_type_> const& table = tables_[device];
        return table.bundle && table.bundle == bundle.data() ? &table : nullptr;
    }

  private:
    kernel_table<module_type_> tables_[gpu_devices_capacity_k];
};

/**
 *  @brief Opens @p device through @p drivers: retains an NVIDIA device's primary context, then
 *      loads the library's bundle of its vendor into @p cuda_kernels or @p hip_kernels.
 *
 *  A device the bundle carries no code for keeps moving memory and refuses launches alone. The
 *  first open of a device runs alone.
 *
 *  @par Statuses:
 *  - @c status_code_t::device_k The driver refused an NVIDIA device's primary context, or serves
 *    no such vendor.
 */
inline status_t open_gpu_device(gpu_drivers_t& drivers, gpu_device_t const& device,
                                kernel_tables<cuda_module_t>& cuda_kernels,
                                kernel_tables<hip_module_t>& hip_kernels) noexcept {
    int const ordinal = static_cast<int>(device.vendor_ordinal);
    if (device.vendor == gpu_vendor_t::nvidia_k) {
        cuda_context_handle_t const context = drivers.retain_context(ordinal);
        if (!context) return {status_code_t::device_k, "The driver refused the device's primary context"};
        [[maybe_unused]] status_t const loaded = cuda_kernels.load(drivers.cuda, context, ordinal, vram_nvidia_bundle(),
                                                                   vram_kernel_names_k);
        return {};
    }
    if (device.vendor == gpu_vendor_t::amd_k) {
        [[maybe_unused]] status_t const loaded = hip_kernels.load(drivers.hip, ordinal, ordinal, vram_amd_bundle(),
                                                                  vram_kernel_names_k);
        return {};
    }
    return {status_code_t::device_k, "No driver serves the device's vendor"};
}

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
template <typename offset_type_, gpu_executor_concept executor_type_>
status_t assemble_tape(executor_type_& executor, std::span<tape_source_t const> sources,
                       std::span<offset_type_> offsets, std::span<std::byte> bytes) noexcept {
    static_assert(std::is_same_v<offset_type_, std::uint32_t> || std::is_same_v<offset_type_, std::uint64_t>,
                  "A tape's offsets take 32 or 64 bits");
    if (offsets.size() != sources.size() + 1)
        return {status_code_t::invalid_argument_k, "A tape takes one offset per string and one more"};
    if (!executor.kernels()) return {status_code_t::device_k, "The device holds no kernels of the library"};
    std::size_t const segments_count = divide_round_up(sources.size(), std::size_t {tape_sources_per_segment_k});
    tape_segment_t* segments = nullptr;
    if (segments_count) {
        expected<std::uintptr_t> allocated = executor.allocate_async(segments_count * sizeof(tape_segment_t));
        if (!allocated) return allocated.status();
        segments = reinterpret_cast<tape_segment_t*>(*allocated);
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

/** Spawns into @p into the executor of @p device, which @c open_gpu_device opened, over its
 *  vendor's driver in @p drivers, of class @p gpu_class, queueing on @p stream, the caller's
 *  stream of that vendor or null for one of its own, its scratch capped at @p scratch_limit bytes,
 *  zero for no ceiling, moving bytes between the drives and an NVIDIA device as @p gds asks, and
 *  launching the library's kernels from its device's table in @p cuda_kernels or @p hip_kernels,
 *  if one loaded. */
template <typename variant_type_>
status_t spawn_gpu_executor(gpu_drivers_t const& drivers, kernel_tables<cuda_module_t> const& cuda_kernels,
                            kernel_tables<hip_module_t> const& hip_kernels, gpu_device_t const& device,
                            gpu_class_t gpu_class, void* stream, std::size_t scratch_limit, ustore_gds_t gds,
                            variant_type_& into) noexcept {
    int const ordinal = static_cast<int>(device.vendor_ordinal);
    if (device.vendor == gpu_vendor_t::nvidia_k) {
        if (device.vendor_ordinal >= gpu_devices_capacity_k)
            return {status_code_t::device_k, "The CUDA ordinal passes the devices UStore lists"};
        cuda_context_handle_t const context = drivers.cuda_contexts[device.vendor_ordinal];
        expected<cuda_executor_t> spawned = cuda_executor_t::spawn(
            drivers.cuda, context, ordinal, gpu_class, stream, scratch_limit, gds,
            cuda_kernels.table_of(vram_nvidia_bundle(), ordinal));
        if (!spawned) return spawned.status();
        into.template emplace<cuda_executor_t>(std::move(*spawned));
        return {};
    }
    if (device.vendor == gpu_vendor_t::amd_k) {
        expected<hip_executor_t> spawned = hip_executor_t::spawn(drivers.hip, ordinal, gpu_class, stream, scratch_limit,
                                                                 hip_kernels.table_of(vram_amd_bundle(), ordinal));
        if (!spawned) return spawned.status();
        into.template emplace<hip_executor_t>(std::move(*spawned));
        return {};
    }
    return {status_code_t::device_k, "No driver serves the device's vendor"};
}

#pragma region Device Arenas

/** Bumps @p count elements off @p blocks, opening a run through @p allocate_ when the newest is full, each carve
 *  padded to @c arena_alignment_k with its tail zeroed on @p executor's stream, so a column's bitmap reads whole. A
 *  run opened under the executor's scratch limit shrinks to fit what the listed runs leave of it, or is refused. */
template <typename element_type_, gpu_executor_concept executor_type_, typename allocate_type_>
expected<std::span<element_type_>> carve_runs(device_blocks_t& blocks, executor_type_& executor, std::size_t count,
                                              allocate_type_&& allocate) noexcept {
    if (!count) return std::span<element_type_> {};
    std::size_t const length = sizeof(element_type_) * count;
    std::size_t const padded = round_up_to_multiple(length, arena_alignment_k);
    std::uintptr_t address = bump(blocks, padded);
    if (!address) {
        if (blocks.count == blocks.blocks.size())
            return status_t {status_code_t::out_of_memory_k, "The device arena lists as many runs as it holds"};
        std::size_t capacity = grown_capacity(blocks, padded);
        if (std::size_t const limit = executor.scratch_limit(); limit) {
            std::size_t held = 0;
            for (std::uint32_t block = 0; block != blocks.count; ++block) held += blocks.blocks[block].capacity;
            std::size_t const left = held < limit ? limit - held : 0;
            if (left < padded)
                return status_t {status_code_t::out_of_memory_k, "The carve passes the executor's scratch limit"};
            capacity = std::min(capacity, left);
        }
        expected<std::uintptr_t> opened = allocate(capacity);
        if (!opened) return opened.status();
        blocks.blocks[blocks.count++] = {*opened, capacity};
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
template <typename element_type_, gpu_executor_concept executor_type_>
expected<std::span<element_type_>> carve(device_blocks_t& blocks, executor_type_& executor,
                                         std::size_t count) noexcept {
    return carve_runs<element_type_>(blocks, executor, count,
                                     [&](std::size_t bytes) noexcept { return executor.allocate_async(bytes); });
}

/** Carves @p count elements of managed memory off @p blocks, which the host and @p executor's device both reach. */
template <typename element_type_, gpu_executor_concept executor_type_>
expected<std::span<element_type_>> carve(managed_blocks_t& blocks, executor_type_& executor,
                                         std::size_t count) noexcept {
    return carve_runs<element_type_>(blocks.blocks, executor, count,
                                     [&](std::size_t bytes) noexcept { return executor.allocate_managed(bytes); });
}

/** Rewinds @p blocks to their newest run, the largest, freeing every older one in stream order on @p executor. */
template <gpu_executor_concept executor_type_>
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
template <gpu_executor_concept executor_type_>
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
template <gpu_executor_concept executor_type_>
device_blocks_t* device_blocks_of(ustore_arena_t arena, executor_type_ const& executor) noexcept {
    using arena_blocks_t = typename executor_type_::arena_blocks_t;
    arena_blocks_t* const memory = arena ? std::get_if<arena_blocks_t>(&arena->memory) : nullptr;
    return memory && memory->device == executor.device() ? &memory->blocks : nullptr;
}

/** Whether @p pointer lies on a device of a vendor other than @p executor_type_'s, which @p drivers answer. */
template <gpu_executor_concept executor_type_>
bool on_another_vendor(gpu_drivers_t const& drivers, void const* pointer) noexcept {
    if constexpr (executor_type_::vendor_k == gpu_vendor_t::nvidia_k) return hip_device_of(drivers.hip, pointer) >= 0;
    else return cuda_device_of(drivers.cuda, pointer) >= 0;
}

#pragma endregion Device Arenas

} // namespace unum::ustore::utilities
