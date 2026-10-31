/**
 *  @file src/nvme/variants_alibaba_c930.cpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The alibaba_c930 unit of the NVMe variants: its store swaps words with Zacas and waits on reservations
 *      with @c WRS.
 */
#include "variants.hpp"

namespace unum::ustore::nvme {
template struct nvme_variant<cpu_class_t::alibaba_c930_k>;
} // namespace unum::ustore::nvme
