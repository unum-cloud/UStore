/**
 *  @file src/ram_spawn.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The topology harvest and pool spawn behind every executor, on ForkUnion's compiled pool.
 */
#pragma once

#include <cstddef> // `std::size_t`

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

/** The label the threads of every pool UStore spawns hold in the OS. */
inline constexpr char const pool_name_k[] = "ustore";

/**
 *  @brief Spawns @p threads threads over @p topology, the caller among them when inclusive,
 *      using only the ForkUnion capabilities in @p allowed.
 *  @param[in] near_domain The memory domain whose local compute domains run the threads, or -1
 *      for every one.
 */
[[nodiscard]] inline st::expected<fu::pool_t> spawn_pool(fu::topology_t const& topology, std::size_t threads,
                                                         fu::caller_exclusivity_t exclusivity,
                                                         fu_capabilities_t allowed,
                                                         fu_memory_domain_id_t near_domain = -1) noexcept {
    fu::pool_t pool;
    fu::status_t const spawned = near_domain == -1 ? pool.spawn(topology, threads, exclusivity, allowed, pool_name_k)
                                                   : pool.spawn_near(topology, near_domain, threads, exclusivity,
                                                                     allowed, pool_name_k);
    if (fu::failed(spawned)) return status_from_(spawned);
    return pool;
}

} // namespace unum::ustore::utilities
