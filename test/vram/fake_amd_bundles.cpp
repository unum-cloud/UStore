/**
 *  @file test/vram/fake_amd_bundles.cpp
 *  @author Ash Vardanian
 *  @date November 14, 2026
 *  @brief The library's bundles unit with a CDNA3 bundle @c fake_amdhip64 loads, so the fake AMD
 *      suite picks a class in builds that kept no CDNA3 bundle; the stand-in loads any image that
 *      opens with the offload bundle's magic, and exports no kernel from it.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#if !USTORE_TARGET_AMD_CDNA3
#undef USTORE_TARGET_AMD_CDNA3
#define USTORE_TARGET_AMD_CDNA3 1
asm(".section .rodata\n"                                                 //
    ".globl vram_amd_cdna3_bundle_start\nvram_amd_cdna3_bundle_start:\n" //
    ".ascii \"__CLANG_OFFLOAD_BUNDLE__\"\n"                              //
    ".globl vram_amd_cdna3_bundle_end\nvram_amd_cdna3_bundle_end:\n.previous\n");
#endif
#include "../../src/vram/bundles.cpp"
