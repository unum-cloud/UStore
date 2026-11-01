/**
 *  @file src/vram/bundles.cpp
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The bundles of kernels the library embeds, one per vendor, linked in as raw bytes beside
 *      this unit by the assembler stubs of @c ustore_gpu_bundle, or empty where the build named no
 *      codes for the vendor.
 */
#include "ustore/utilities/vram_device.hpp" // `vram_nvidia_bundle`, `vram_amd_bundle`

namespace unum::ustore {

using utilities::byte_t;

#if USTORE_BUNDLE_VRAM_NVIDIA
extern "C" byte_t const vram_nvidia_bundle_start[], vram_nvidia_bundle_end[];
std::span<byte_t const> utilities::vram_nvidia_bundle() noexcept {
    return {vram_nvidia_bundle_start, static_cast<std::size_t>(vram_nvidia_bundle_end - vram_nvidia_bundle_start)};
}
#else
std::span<byte_t const> utilities::vram_nvidia_bundle() noexcept { return {}; }
#endif

#if USTORE_BUNDLE_VRAM_AMD
extern "C" byte_t const vram_amd_bundle_start[], vram_amd_bundle_end[];
std::span<byte_t const> utilities::vram_amd_bundle() noexcept {
    return {vram_amd_bundle_start, static_cast<std::size_t>(vram_amd_bundle_end - vram_amd_bundle_start)};
}
#else
std::span<byte_t const> utilities::vram_amd_bundle() noexcept { return {}; }
#endif

} // namespace unum::ustore
