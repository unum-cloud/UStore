/**
 *  @file src/nvme/variants_intel_skylake.cpp
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The intel_skylake unit of the NVMe variants: the Skylake sort - AVX-512 string sorting
 *      and hashing - over the intel_haswell kernels, waits and atomics.
 */
#include "variants.hpp"

namespace unum::ustore::nvme {
template struct nvme_variant<cpu_class_t::intel_skylake_k>;
} // namespace unum::ustore::nvme
