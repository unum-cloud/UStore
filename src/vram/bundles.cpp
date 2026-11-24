/**
 *  @file src/vram/bundles.cpp
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The bundles of kernels the library embeds, one per kept GPU class, linked in as raw bytes
 *      beside this unit by the stubs of @c ustore_gpu_bundle, or, for an Apple class, as the MSL
 *      source its class unit embeds, and whether a device loads one.
 */
#include <cstring> // `std::memcpy`

#include <stringzilla/regex/metal.h> // `sz_regex_arguments_metal_t`, `sz_regex_stride_metal_`

#include "vram/classes.hpp" // `gpu_class_bundle`, `gpu_class_serves`, `metal_regex_walk_t`

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
#if USTORE_TARGET_APPLE_METAL7
extern "C" unsigned char const* const vram_apple_metal7_bundle_start;
extern "C" std::size_t const vram_apple_metal7_bundle_bytes;
#endif
#if USTORE_TARGET_APPLE_METAL9
extern "C" unsigned char const* const vram_apple_metal9_bundle_start;
extern "C" std::size_t const vram_apple_metal9_bundle_bytes;
#endif
#if USTORE_TARGET_APPLE_METAL10
extern "C" unsigned char const* const vram_apple_metal10_bundle_start;
extern "C" std::size_t const vram_apple_metal10_bundle_bytes;
#endif

[[maybe_unused]] static std::span<byte_t const> bundle_between(byte_t const* start, byte_t const* end) noexcept {
    return {start, static_cast<std::size_t>(end - start)};
}

/** The MSL source an Apple class unit embeds: @p bytes from @p start, its null byte included. */
[[maybe_unused]] static std::span<byte_t const> source_bundle(unsigned char const* start, std::size_t bytes) noexcept {
    return util::bytes_of(start, bytes);
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
#if USTORE_TARGET_APPLE_METAL7
    case gpu_class_t::apple_metal7_k:
        return source_bundle(vram_apple_metal7_bundle_start, vram_apple_metal7_bundle_bytes);
#endif
#if USTORE_TARGET_APPLE_METAL9
    case gpu_class_t::apple_metal9_k:
        return source_bundle(vram_apple_metal9_bundle_start, vram_apple_metal9_bundle_bytes);
#endif
#if USTORE_TARGET_APPLE_METAL10
    case gpu_class_t::apple_metal10_k:
        return source_bundle(vram_apple_metal10_bundle_start, vram_apple_metal10_bundle_bytes);
#endif
    default: return {};
    }
}

/** The Apple GPU family @p gpu_class compiles for, zero for a class of another vendor. */
static std::uint32_t family_of_apple_class(gpu_class_t gpu_class) noexcept {
    switch (gpu_class) {
    case gpu_class_t::apple_metal7_k: return 7;
    case gpu_class_t::apple_metal9_k: return 9;
    case gpu_class_t::apple_metal10_k: return 10;
    default: return 0;
    }
}

#if USTORE_ARCH_METAL_

static_assert(sizeof(sz_regex_arguments_metal_t) == sizeof(metal_regex_walk_t) &&
                  alignof(sz_regex_arguments_metal_t) <= alignof(metal_regex_walk_t),
              "The walk record holds StringZilla's record byte for byte");

metal_regex_walk_t metal_regex_walk(sz_regex_engine_t const& engine) noexcept {
    sz_regex_arguments_metal_t record {};
    for (std::size_t kind = 0; kind != 4; ++kind)
        record.forward_starts[kind] = engine.forward_starts[kind],
        record.reverse_starts[kind] = engine.reverse_starts[kind];
    record.forward_accepts_begin = engine.forward_accepts_begin;
    record.reverse_accepts_begin = engine.reverse_accepts_begin;
    record.classes_count = engine.classes_count;
    record.case_sensitivity = static_cast<sz_u32_t>(engine.case_sensitivity);
    record.stride = static_cast<sz_u32_t>(sz_regex_stride_metal_(&engine));
    metal_regex_walk_t walk;
    std::memcpy(walk.bytes, &record, sizeof(record));
    return walk;
}

#else

metal_regex_walk_t metal_regex_walk(sz_regex_engine_t const&) noexcept { return {}; }

#endif

bool gpu_class_serves(gpu_drivers_t& drivers, gpu_class_t gpu_class, gpu_device_t const& device) noexcept {
    std::span<byte_t const> const bundle = gpu_class_bundle(gpu_class);
    if (bundle.empty()) return false;
    int const ordinal = static_cast<int>(device.vendor_ordinal);
    switch (device.vendor) {
    case gpu_vendor_t::nvidia_k:
        return cuda_module_t::load(drivers.cuda, drivers.retain_context(ordinal), bundle).has_value();
    case gpu_vendor_t::amd_k: return hip_module_t::load(drivers.hip, ordinal, bundle).has_value();
    // Metal compiles the source at the class's first spawn, so the family the device lists decides
    case gpu_vendor_t::apple_k: return device.architecture_major >= family_of_apple_class(gpu_class);
    default: return false;
    }
}

} // namespace unum::ustore
