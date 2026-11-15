/**
 *  @file src/ram/variants_ibm_power10.cpp
 *  @author Ash Vardanian
 *  @date November 14, 2026
 *  @brief The ibm_power10 unit of the RAM variants: its partition locks update words with ISA 3.0
 *      store atomics and wait with @c pause_short.
 */
#include "variants.hpp"

namespace unum::ustore::ram {
template struct ram_variant<cpu_class_t::ibm_power10_k, isolation_t::snapshot_k>;
template struct ram_variant<cpu_class_t::ibm_power10_k, isolation_t::strict_serializable_k>;
} // namespace unum::ustore::ram
