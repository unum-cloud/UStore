/**
 *  @file src/nvme/variants_apple_m2.cpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The apple_m2 unit of the NVMe variants: its store moves every word with LSE read-modify-writes and
 *      RCpc acquiring loads.
 */
#include "variants.hpp"

namespace unum::ustore::nvme {
template struct nvme_variant<cpu_class_t::apple_m2_k>;
} // namespace unum::ustore::nvme
