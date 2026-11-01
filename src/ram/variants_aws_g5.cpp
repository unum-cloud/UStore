/**
 *  @file src/ram/variants_aws_g5.cpp
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The aws_g5 unit of the RAM variants: waits with @c WFE on Graviton3 and newer, over the
 *      apple_m2 atomics and NEON tier.
 */
#include "variants.hpp"

namespace unum::ustore::ram {
template struct ram_variant<cpu_class_t::aws_g5_k, isolation_t::snapshot_k>;
template struct ram_variant<cpu_class_t::aws_g5_k, isolation_t::strict_serializable_k>;
} // namespace unum::ustore::ram
