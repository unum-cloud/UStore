/**
 *  @file src/ram/variants_intel_haswell.cpp
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The intel_haswell unit of the RAM variants: the Haswell AVX2 StringZilla tier - sorts,
 *      byteset searches and escapes - over the serial floor's waits and atomics.
 */
#include "variants.hpp"

namespace unum::ustore::ram {
template struct ram_variant<cpu_class_t::intel_haswell_k, isolation_t::snapshot_k>;
template struct ram_variant<cpu_class_t::intel_haswell_k, isolation_t::strict_serializable_k>;
} // namespace unum::ustore::ram
