/**
 *  @file include/ustore/utilities/vram_matching.hpp
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The device matching kernels of the library's bundles as one executor's device resolved
 *      them: three per family, one for each granularity a page's values walk at.
 */

#pragma once

#include "ustore/utilities/vram_device.hpp" // `kernel_table`

namespace unum::ustore::utilities {

/** The device matching kernels of one family: a lane, a group of lanes, and a lane per chunk
 *  walking each value. */
template <typename function_type_>
struct vram_matching_kit {
    function_type_ per_thread = nullptr;
    function_type_ per_group = nullptr;
    function_type_ per_chunk = nullptr;
};

/** Every family's kit, empty where the device loaded no bundle of the library's kernels. */
template <typename function_type_>
struct vram_matching_kits {
    vram_matching_kit<function_type_> substrings;
    vram_matching_kit<function_type_> regex;
    vram_matching_kit<function_type_> subdocuments;
};

/** The matching kits of the bundle @p table holds, or empty ones for a null @p table. */
template <typename module_type_>
vram_matching_kits<typename module_type_::function_t> matching_kits_of(
    kernel_table<module_type_> const* table) noexcept {
    if (!table) return {};
    return {{table->function("ustore_substrings_contains_per_thread"),
             table->function("ustore_substrings_contains_per_group"),
             table->function("ustore_substrings_contains_per_chunk")},
            {table->function("ustore_regex_contains_per_thread"), table->function("ustore_regex_contains_per_group"),
             table->function("ustore_regex_contains_per_chunk")},
            {table->function("ustore_subdocuments_contains_per_thread"),
             table->function("ustore_subdocuments_contains_per_group"),
             table->function("ustore_subdocuments_contains_per_chunk")}};
}

} // namespace unum::ustore::utilities
