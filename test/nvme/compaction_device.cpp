/**
 *  @file test/nvme/compaction_device.cpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief The compaction suite's device build: device zero's table of the library's kernels, which
 *      the library this build links embeds, its device compactor instantiated there for the
 *      engine's key slots the suite merges.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include "ustore/utilities/vram_device.hpp" // `kernel_tables`, `vram_nvidia_bundle`, `vram_kernel_names_k`

using namespace unum::ustore::utilities;

kernel_table<cuda_module_t> const* suite_kernels(gpu_drivers_t const& drivers,
                                                 kernel_tables<cuda_module_t>& tables) noexcept {
    if (tables.load(drivers.cuda, drivers.cuda_contexts[0], 0, vram_nvidia_bundle(), vram_kernel_names_k).failed())
        return nullptr;
    return tables.table_of(vram_nvidia_bundle(), 0);
}
