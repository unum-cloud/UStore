/**
 *  @file src/ram/variants_intel_skylake.cpp
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The intel_skylake unit of the RAM variants: the Skylake sort - AVX-512 string sorting and
 *      hashing - over the intel_haswell kernels, waits and atomics.
 */
#include "variants.hpp"

namespace unum::ustore::ram {
template struct ram_variant<cpu_class_t::intel_skylake_k, isolation_t::snapshot_k>;
template struct ram_variant<cpu_class_t::intel_skylake_k, isolation_t::strict_serializable_k>;
} // namespace unum::ustore::ram
