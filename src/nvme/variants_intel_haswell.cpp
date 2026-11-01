/**
 *  @file src/nvme/variants_intel_haswell.cpp
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The intel_haswell unit of the NVMe variants: the Haswell AVX2 StringZilla tier - sorts,
 *      byteset searches and escapes - over the serial floor's waits and atomics.
 */
#include "variants.hpp"

namespace unum::ustore::nvme {
template struct nvme_variant<cpu_class_t::intel_haswell_k>;
} // namespace unum::ustore::nvme
