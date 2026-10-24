/**
 *  @file src/nvme/variants_intel_haswell.cpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The intel_haswell unit of the NVMe variants: its store spins with @c PAUSE between attempts over the
 *      standard atomics.
 */
#include "variants.hpp"

namespace unum::ustore::nvme {
template struct nvme_variant<cpu_class_t::intel_haswell_k>;
} // namespace unum::ustore::nvme
