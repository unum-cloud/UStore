/**
 *  @file src/nvme/variants_serial.cpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The serial unit of the NVMe variants: the floor every ladder ends on, over the standard atomics,
 *      yielding to the scheduler between attempts.
 */
#include "variants.hpp"

namespace unum::ustore::nvme {
template struct nvme_variant<cpu_class_t::serial_k>;
} // namespace unum::ustore::nvme
