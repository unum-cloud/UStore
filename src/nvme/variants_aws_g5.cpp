/**
 *  @file src/nvme/variants_aws_g5.cpp
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The aws_g5 unit of the NVMe variants: waits with @c WFE on Graviton3 and newer, over the
 *      apple_m2 atomics and NEON tier.
 */
#include "variants.hpp"

namespace unum::ustore::nvme {
template struct nvme_variant<cpu_class_t::aws_g5_k>;
} // namespace unum::ustore::nvme
