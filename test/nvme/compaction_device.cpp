/**
 *  @file test/nvme/compaction_device.cpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief The compaction suite's device build: device zero's table of the library's kernels, which
 *      the library this build links embeds, its device compactor instantiated there for the
 *      engine's key slots the suite merges.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include "ustore/utilities/vram_device.hpp" // `vram_kernel_names_k`

#include "vram/classes.hpp" // `kernel_tables`, `pick_gpu_class`

using namespace unum::ustore;
using namespace unum::ustore::utilities;

kernel_table<cuda_module_t> const* suite_kernels(gpu_drivers_t& drivers,
                                                 kernel_tables<cuda_module_t>& tables) noexcept {
    gpu_device_t device;
    if (!list_nvidia_gpu_devices(drivers.cuda, {&device, 1})) return nullptr;
    gpu_class_t const gpu_class = pick_gpu_class(drivers, device, gpu_class_t::unknown_k);
    if (tables.load(drivers.cuda, drivers.cuda_contexts[0], 0, gpu_class, vram_kernel_names_k).failed()) return nullptr;
    return tables.table_of(0, gpu_class);
}
