/**
 *  @file src/nvme/variants_intel_icelake.cpp
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The intel_icelake unit of the NVMe variants: the Ice Lake UTF-8 and byteset tiers -
 *      VBMI2 UTF-8 counting, byteset searches, escapes and hashing - over the intel_skylake sort,
 *      waits and atomics.
 */
#include "variants.hpp"

namespace unum::ustore::nvme {
template struct nvme_variant<cpu_class_t::intel_icelake_k>;
} // namespace unum::ustore::nvme
