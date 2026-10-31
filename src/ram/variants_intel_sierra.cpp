/**
 *  @file src/ram/variants_intel_sierra.cpp
 *  @author Ash Vardanian
 *  @date October 25, 2026
 *  @brief The intel_sierra unit of the RAM variants: its partition locks wait with @c TPAUSE and add
 *      conditionally with @c CMPCCXADD.
 */
#include "variants.hpp"

namespace unum::ustore::ram {
template struct ram_variant<cpu_class_t::intel_sierra_k, isolation_t::snapshot_k>;
template struct ram_variant<cpu_class_t::intel_sierra_k, isolation_t::strict_serializable_k>;
} // namespace unum::ustore::ram
