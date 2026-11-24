/**
 *  @file src/vram/vram_entries_apple.cpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The bundle of every kept Apple class: the MSL source of the library's kernels, which a
 *      device compiles at the first spawn of its class, as the bundles unit reads it.
 *
 *  The Apple twin of a class's device pass over @c vram_kernels_nvidia.cu or
 *  @c vram_kernels_amd.hip, compiled once per class with that class's gate set to 1. MSL cannot
 *  include, so the source is spliced here, in order: the vocabulary the host and the kernels share,
 *  a prelude naming the class's Apple family, StringZilla's MSL engines, the kernels of tapes,
 *  gathers and selects, the compactor's kernels, then the matching kernels. Each part ends in a
 *  newline and the whole in a null byte, as Metal reads it as one C string.
 */

#include <cstddef> // `std::size_t`

#include <array> // `std::array`

namespace unum::ustore {

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif

/** The vocabulary of slots and extents the host binds by. */
constexpr char metal_shared_source_k[] = {
#embed "ustore/utilities/vram_apple_shared.hpp" suffix(, 0) if_empty(0)
};

/** StringZilla's MSL: its vocabulary, case folding tables and folding, and the substrings, regex
 *  and subdocuments engines the matching kernels walk, each part followed by a newline. */
constexpr char metal_stringzilla_source_k[] = {
#embed "stringzilla/types.metal" suffix(, '\n', )
#embed "stringzilla/utf8_uncased_fold/tables.h" suffix(, '\n', )
#embed "stringzilla/utf8_uncased_fold/metal.metal" suffix(, '\n', )
#embed "stringzilla/substrings/metal.metal" suffix(, '\n', )
#embed "stringzilla/regex/metal.metal" suffix(, '\n', )
#embed "stringzilla/subdocuments/metal.metal" suffix(, 0) if_empty(0)
};

/** The tape, gather and select kernels. */
constexpr char metal_kernels_source_k[] = {
#embed "vram/vram_kernels_apple.metal" suffix(, 0) if_empty(0)
};

/** The compactor's merge and fold kernels. */
constexpr char metal_compaction_source_k[] = {
#embed "vram/vram_compaction_apple.metal" suffix(, 0) if_empty(0)
};

/** The matching kernels, which walk the substrings, regex and subdocuments engines. */
constexpr char metal_matching_source_k[] = {
#embed "vram/vram_matching_apple.metal" suffix(, 0) if_empty(0)
};

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

/** The prelude of each class, naming the Apple family it compiles for. */
#if USTORE_TARGET_APPLE_METAL10
constexpr char metal_prelude_source_k[] =
    "namespace unum { namespace ustore { constant constexpr uint metal_family_k = 10; } }\n";
#elif USTORE_TARGET_APPLE_METAL9
constexpr char metal_prelude_source_k[] =
    "namespace unum { namespace ustore { constant constexpr uint metal_family_k = 9; } }\n";
#else
constexpr char metal_prelude_source_k[] =
    "namespace unum { namespace ustore { constant constexpr uint metal_family_k = 7; } }\n";
#endif

/** Bytes the spliced source takes: each part without its null byte, a newline after
 *  each, one null. */
template <std::size_t... sizes_>
consteval std::size_t spliced_bytes() noexcept {
    return (sizes_ + ... + 0) + 1;
}

/** Splices @p parts, null-terminated texts, in order, each followed by a newline, into one
 *  null-terminated text, as the unit loads, as the source outgrows a compiler's constant steps. */
template <std::size_t... sizes_>
std::array<unsigned char, spliced_bytes<sizes_...>()> splice(char const (&... parts)[sizes_]) noexcept {
    std::array<unsigned char, spliced_bytes<sizes_...>()> spliced {};
    std::size_t used = 0;
    auto const append = [&](char const* part, std::size_t size) {
        for (std::size_t index = 0; index + 1 < size; ++index)
            spliced[used++] = static_cast<unsigned char>(part[index]);
        spliced[used++] = '\n';
    };
    (append(parts, sizes_), ...);
    spliced[used] = 0;
    return spliced;
}

/** The source of this class's bundle, the vocabulary first, as every MSL part reads it. */
auto const metal_bundle_source_k = splice(metal_shared_source_k, metal_prelude_source_k, metal_stringzilla_source_k,
                                          metal_kernels_source_k, metal_compaction_source_k, metal_matching_source_k);

#if USTORE_TARGET_APPLE_METAL7
extern "C" unsigned char const* const vram_apple_metal7_bundle_start = metal_bundle_source_k.data();
extern "C" std::size_t const vram_apple_metal7_bundle_bytes = metal_bundle_source_k.size();
#endif
#if USTORE_TARGET_APPLE_METAL9
extern "C" unsigned char const* const vram_apple_metal9_bundle_start = metal_bundle_source_k.data();
extern "C" std::size_t const vram_apple_metal9_bundle_bytes = metal_bundle_source_k.size();
#endif
#if USTORE_TARGET_APPLE_METAL10
extern "C" unsigned char const* const vram_apple_metal10_bundle_start = metal_bundle_source_k.data();
extern "C" std::size_t const vram_apple_metal10_bundle_bytes = metal_bundle_source_k.size();
#endif

} // namespace unum::ustore
