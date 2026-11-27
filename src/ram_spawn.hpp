/**
 *  @file src/ram_spawn.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The topology harvest behind every executor, through ForkUnion's SDK.
 */
#pragma once

#include <forkunion/forkunion.hpp> // `fu::pool_t`, `fu::topology_t`

#include "ustore/utilities/ram_device.hpp" // `status_from_`
#include "ustore/utilities/types.hpp"      // `st::expected`

namespace unum::ustore::utilities {

/** Harvests the topology of the machine the process runs on. */
[[nodiscard]] inline st::expected<fu::topology_t> harvest_topology() noexcept {
    fu::topology_t topology;
    if (fu::status_t const status = topology.harvest(); fu::failed(status)) return status_from_(status);
    return topology;
}

} // namespace unum::ustore::utilities
