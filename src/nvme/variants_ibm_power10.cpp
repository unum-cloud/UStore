/**
 *  @file src/nvme/variants_ibm_power10.cpp
 *  @author Ash Vardanian
 *  @date November 14, 2026
 *  @brief The ibm_power10 unit of the NVMe variants: its store updates words with ISA 3.0 store
 *      atomics and waits with @c pause_short.
 */
#include "variants.hpp"

namespace unum::ustore::nvme {
template struct nvme_variant<cpu_class_t::ibm_power10_k>;
} // namespace unum::ustore::nvme
