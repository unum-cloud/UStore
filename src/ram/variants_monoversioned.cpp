/**
 *  @file src/ram/variants_monoversioned.cpp
 *  @author Ash Vardanian
 *  @date October 28, 2026
 *  @brief The monoversioned unit of the RAM variants: its partition locks wait and synchronize with whatever the
 *      build's floor and vouched capabilities compiled.
 */
#include "variants.hpp"

namespace unum::ustore::ram {
template struct ram_variant<cpu_class_t::monoversioned_k, isolation_t::snapshot_k>;
template struct ram_variant<cpu_class_t::monoversioned_k, isolation_t::strict_serializable_k>;
} // namespace unum::ustore::ram
