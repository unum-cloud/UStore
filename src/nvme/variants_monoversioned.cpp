/**
 *  @file src/nvme/variants_monoversioned.cpp
 *  @author Ash Vardanian
 *  @date October 28, 2026
 *  @brief The monoversioned unit of the NVMe variants: its store waits and synchronizes with whatever the build's
 *      floor and vouched capabilities compiled.
 */
#include "variants.hpp"

namespace unum::ustore::nvme {
template struct nvme_variant<cpu_class_t::monoversioned_k>;
} // namespace unum::ustore::nvme
