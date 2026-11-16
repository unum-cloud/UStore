/**
 *  @file src/vram/bundles.cpp
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The bundles of kernels the library embeds, one per kept GPU class, linked in as raw bytes
 *      beside this unit by the stubs of @c ustore_gpu_bundle, and whether a device loads one.
 */
#include "vram/classes.hpp" // `gpu_class_bundle`, `gpu_class_serves`

namespace unum::ustore {

#if USTORE_TARGET_NVIDIA_AMPERE
extern "C" byte_t const vram_nvidia_ampere_bundle_start[], vram_nvidia_ampere_bundle_end[];
#endif
#if USTORE_TARGET_NVIDIA_ADA
extern "C" byte_t const vram_nvidia_ada_bundle_start[], vram_nvidia_ada_bundle_end[];
#endif
#if USTORE_TARGET_NVIDIA_HOPPER
extern "C" byte_t const vram_nvidia_hopper_bundle_start[], vram_nvidia_hopper_bundle_end[];
#endif
#if USTORE_TARGET_NVIDIA_BLACKWELL
extern "C" byte_t const vram_nvidia_blackwell_bundle_start[], vram_nvidia_blackwell_bundle_end[];
#endif
#if USTORE_TARGET_NVIDIA_BLACKWELLRTX
extern "C" byte_t const vram_nvidia_blackwellrtx_bundle_start[], vram_nvidia_blackwellrtx_bundle_end[];
#endif
#if USTORE_TARGET_AMD_CDNA3
extern "C" byte_t const vram_amd_cdna3_bundle_start[], vram_amd_cdna3_bundle_end[];
#endif
#if USTORE_TARGET_AMD_CDNA4
extern "C" byte_t const vram_amd_cdna4_bundle_start[], vram_amd_cdna4_bundle_end[];
#endif
#if USTORE_TARGET_AMD_CDNA5
extern "C" byte_t const vram_amd_cdna5_bundle_start[], vram_amd_cdna5_bundle_end[];
#endif

[[maybe_unused]] static std::span<byte_t const> bundle_between(byte_t const* start, byte_t const* end) noexcept {
    return {start, static_cast<std::size_t>(end - start)};
}

std::span<byte_t const> gpu_class_bundle(gpu_class_t gpu_class) noexcept {
    switch (gpu_class) {
#if USTORE_TARGET_NVIDIA_AMPERE
    case gpu_class_t::nvidia_ampere_k:
        return bundle_between(vram_nvidia_ampere_bundle_start, vram_nvidia_ampere_bundle_end);
#endif
#if USTORE_TARGET_NVIDIA_ADA
    case gpu_class_t::nvidia_ada_k: return bundle_between(vram_nvidia_ada_bundle_start, vram_nvidia_ada_bundle_end);
#endif
#if USTORE_TARGET_NVIDIA_HOPPER
    case gpu_class_t::nvidia_hopper_k:
        return bundle_between(vram_nvidia_hopper_bundle_start, vram_nvidia_hopper_bundle_end);
#endif
#if USTORE_TARGET_NVIDIA_BLACKWELL
    case gpu_class_t::nvidia_blackwell_k:
        return bundle_between(vram_nvidia_blackwell_bundle_start, vram_nvidia_blackwell_bundle_end);
#endif
#if USTORE_TARGET_NVIDIA_BLACKWELLRTX
    case gpu_class_t::nvidia_blackwellrtx_k:
        return bundle_between(vram_nvidia_blackwellrtx_bundle_start, vram_nvidia_blackwellrtx_bundle_end);
#endif
#if USTORE_TARGET_AMD_CDNA3
    case gpu_class_t::amd_cdna3_k: return bundle_between(vram_amd_cdna3_bundle_start, vram_amd_cdna3_bundle_end);
#endif
#if USTORE_TARGET_AMD_CDNA4
    case gpu_class_t::amd_cdna4_k: return bundle_between(vram_amd_cdna4_bundle_start, vram_amd_cdna4_bundle_end);
#endif
#if USTORE_TARGET_AMD_CDNA5
    case gpu_class_t::amd_cdna5_k: return bundle_between(vram_amd_cdna5_bundle_start, vram_amd_cdna5_bundle_end);
#endif
    default: return {};
    }
}

bool gpu_class_serves(gpu_drivers_t& drivers, gpu_class_t gpu_class, gpu_device_t const& device) noexcept {
    std::span<byte_t const> const bundle = gpu_class_bundle(gpu_class);
    if (bundle.empty()) return false;
    int const ordinal = static_cast<int>(device.vendor_ordinal);
    switch (device.vendor) {
    case gpu_vendor_t::nvidia_k:
        return cuda_module_t::load(drivers.cuda, drivers.retain_context(ordinal), bundle).has_value();
    case gpu_vendor_t::amd_k: return hip_module_t::load(drivers.hip, ordinal, bundle).has_value();
    default: return false;
    }
}

} // namespace unum::ustore
