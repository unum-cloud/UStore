/**
 *  @file src/ram/variants_aws_g2.cpp
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The aws_g2 unit of the RAM variants: the NEON tier - sorts, UTF-8 counting, byteset
 *      searches, escapes and AES hashing - over the serial floor's waits and atomics.
 */
#include "variants.hpp"

namespace unum::ustore::ram {
template struct ram_variant<cpu_class_t::aws_g2_k, isolation_t::snapshot_k>;
template struct ram_variant<cpu_class_t::aws_g2_k, isolation_t::strict_serializable_k>;
} // namespace unum::ustore::ram
