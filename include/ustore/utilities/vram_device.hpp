/**
 *  @file include/ustore/utilities/vram_device.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The vendor-neutral face of the GPU residency: every vendor's driver, the device listing, and one executor
 *      spawned over any vendor.
 *
 *  One binary drives NVIDIA and AMD devices at once, each vendor through a runtime its caller loads once and declared
 *  without its SDK, so every unit names both: NVIDIA devices are listed first, then AMD, and a vendor whose runtime is
 *  absent contributes no devices. Every vendor's device header provides the same surface, which
 *  @c gpu_executor_concept names.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uintptr_t`

#include <algorithm> // `std::min`
#include <concepts>  // `std::same_as`, `std::convertible_to`
#include <span>      // `std::span`
#include <utility>   // `std::move`
#include <variant>   // `std::get_if`

#include <stringzilla/capabilities.h> // `sz_capability_t`

#include "ustore/utilities/nvme_linux_cufile_disk.hpp" // `cufile_api_t`
#include "ustore/utilities/ram_buffers.hpp"            // `device_blocks_t`, `managed_blocks_t`, `bump`
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

    /** The primary context of every NVIDIA device by CUDA ordinal, null where the driver refused it, retained once here
     *  and never released, as the driver releases them at exit. */
    cuda_context_handle_t cuda_contexts[gpu_devices_capacity_k] = {};

    gpu_drivers_t() noexcept : cuda(cuda_api_t::load()), hip(hip_api_t::load()), cufile(cufile_api_t::load()) {
        int devices_count = 0;
        if (!cuda || cuda.get_device_count(&devices_count) != cuda_success_k) return;
        for (int ordinal = 0; ordinal != devices_count && ordinal != static_cast<int>(gpu_devices_capacity_k);
             ++ordinal) {
            int handle = 0;
            if (cuda.get_device(&handle, ordinal) != cuda_success_k ||
                cuda.retain_primary_context(&cuda_contexts[ordinal], handle) != cuda_success_k)
                cuda_contexts[ordinal] = nullptr;
        }
    }
    gpu_drivers_t(gpu_drivers_t const&) = delete;
    gpu_drivers_t& operator=(gpu_drivers_t const&) = delete;
};

/** Writes into @p into every device @p drivers reach, NVIDIA first, then AMD, as each driver reports it, and answers
 *  how many it wrote. */
inline std::size_t list_gpu_devices(gpu_drivers_t const& drivers, std::span<gpu_device_t> into) noexcept {
    std::size_t const count = list_nvidia_gpu_devices(drivers.cuda, into);
    return count + list_amd_gpu_devices(drivers.hip, into.subspan(count));
}

/** Spawns into @p into the executor of @p device's vendor over its driver in @p drivers, of class @p gpu_class,
 *  queueing on @p stream, the caller's stream of that vendor or null for one of its own, its scratch capped at
 *  @p scratch_limit bytes, zero for no ceiling, moving bytes between the drives and an NVIDIA device as @p gds asks. */
template <typename variant_type_>
status_t spawn_gpu_executor(gpu_drivers_t const& drivers, gpu_device_t const& device, gpu_class_t gpu_class,
                            void* stream, std::size_t scratch_limit, ustore_gds_t gds, variant_type_& into) noexcept {
    int const ordinal = static_cast<int>(device.vendor_ordinal);
    if (device.vendor == gpu_vendor_t::nvidia_k) {
        if (device.vendor_ordinal >= gpu_devices_capacity_k)
            return {status_code_t::device_k, "The CUDA ordinal passes the devices UStore lists"};
        expected<cuda_executor_t> spawned = cuda_executor_t::spawn(
            drivers.cuda, drivers.cuda_contexts[device.vendor_ordinal], ordinal, gpu_class, stream, scratch_limit, gds);
        if (!spawned) return spawned.status();
        into.template emplace<cuda_executor_t>(std::move(*spawned));
        return {};
    }
    if (device.vendor == gpu_vendor_t::amd_k) {
        expected<hip_executor_t> spawned = hip_executor_t::spawn(drivers.hip, ordinal, gpu_class, stream,
                                                                 scratch_limit);
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
