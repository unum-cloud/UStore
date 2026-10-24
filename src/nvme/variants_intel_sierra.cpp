/**
 *  @file src/nvme/variants_intel_sierra.cpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The intel_sierra unit of the NVMe variants: its store waits with @c TPAUSE and adds conditionally with
 *      @c CMPCCXADD.
 */
#include "variants.hpp"

namespace unum::ustore::nvme {
template struct nvme_variant<cpu_class_t::intel_sierra_k>;
} // namespace unum::ustore::nvme
