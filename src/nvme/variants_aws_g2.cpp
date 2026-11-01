/**
 *  @file src/nvme/variants_aws_g2.cpp
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The aws_g2 unit of the NVMe variants: the NEON tier - sorts, UTF-8 counting, byteset
 *      searches, escapes and AES hashing - over the serial floor's waits and atomics.
 */
#include "variants.hpp"

namespace unum::ustore::nvme {
template struct nvme_variant<cpu_class_t::aws_g2_k>;
} // namespace unum::ustore::nvme
