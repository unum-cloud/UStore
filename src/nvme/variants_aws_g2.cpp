/**
 *  @file src/nvme/variants_aws_g2.cpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The aws_g2 unit of the NVMe variants: its store spins with @c YIELD between attempts over the
 *      standard atomics.
 */
#include "variants.hpp"

namespace unum::ustore::nvme {
template struct nvme_variant<cpu_class_t::aws_g2_k>;
} // namespace unum::ustore::nvme
