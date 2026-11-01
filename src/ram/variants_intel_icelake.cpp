/**
 *  @file src/ram/variants_intel_icelake.cpp
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The intel_icelake unit of the RAM variants: the Ice Lake UTF-8 and byteset tiers -
 *      VBMI2 UTF-8 counting, byteset searches, escapes and hashing - over the intel_skylake sort,
 *      waits and atomics.
 */
#include "variants.hpp"

namespace unum::ustore::ram {
template struct ram_variant<cpu_class_t::intel_icelake_k, isolation_t::snapshot_k>;
template struct ram_variant<cpu_class_t::intel_icelake_k, isolation_t::strict_serializable_k>;
} // namespace unum::ustore::ram
