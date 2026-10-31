/**
 *  @file src/ram/variants_alibaba_c930.cpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The alibaba_c930 unit of the RAM variants: its partition locks swap words with Zacas and
 *      wait on reservations with @c WRS.
 */
#include "variants.hpp"

namespace unum::ustore::ram {
template struct ram_variant<cpu_class_t::alibaba_c930_k, isolation_t::snapshot_k>;
template struct ram_variant<cpu_class_t::alibaba_c930_k, isolation_t::strict_serializable_k>;
} // namespace unum::ustore::ram
