/**
 *  @file include/ustore/utilities/ram_device.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The one owner of threads and topology, over ForkUnion's C ABI.
 *
 *  Engines, tools, tests and benchmarks dispatch through ForkUnion's @c fu::pool_t, which
 *  @c spawn_pool fills here and which runs on the calling thread while empty; besides it only the
 *  arenas of @c utilities/ram_buffers.hpp call @c fu_* directly.
 */
#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`

#include <forkunion/forkunion.h>
#include <forkunion/forkunion.hpp> // `fu::pool_t`, `fu::topology_t`

#include <smashtable/shared.hpp>

#include "ustore/utilities/types.hpp" // `st::status_t`, `st::expected`

/** Whether one call holds a word that every other call finds contended, global like the arena
 *  handle holding it. */
enum class claim_t : std::uint32_t {
    free_k,
    held_k,
};

/** Where an executor computes, and so where the arenas it makes live, global like the arena handle
 *  holding it. */
enum class residency_t : std::uint8_t {

    /** Host threads over host pages, as every executor of the RAM, RocksDB, LevelDB and NVMe engines computes. */
    host_k,

    /** Host threads driving CUDA devices, as a VRAM executor computes, where no engine places a database yet. */
    vram_k,
};

namespace unum::ustore::utilities {

#pragma region Executor

/** Translates a ForkUnion failure into the status every UStore container reports. */
constexpr st::status_t status_from_(fu::status_t status) noexcept {
    using st::status_t;
    switch (status) {
    case fu::status_t::success_k: return status_t::success_k;
    case fu::status_t::bad_alloc_k: return status_t::out_of_memory_heap_k;
    case fu::status_t::capacity_exhausted_k: return status_t::capacity_exhausted_k;
    case fu::status_t::invalid_argument_k: return status_t::invalid_argument_k;
    case fu::status_t::thread_refused_k:
    case fu::status_t::unsupported_k:
    case fu::status_t::permission_denied_k: return status_t::operation_not_permitted_k;
    case fu::status_t::contended_k: return status_t::operation_would_block_k;
    default: return status_t::unknown_k;
    }
}

/** The memory domain naming none, where an allocation takes the plain heap. */
inline constexpr fu_memory_domain_id_t no_memory_domain_k = -1;

/** The memory domain nearest the PCI function at @p bus_id, or @c no_memory_domain_k where the
 *  topology names none. */
inline fu_memory_domain_id_t memory_domain_of_pci(char const* bus_id) noexcept {
    fu_memory_domain_id_t domain = no_memory_domain_k;
    return fu_memory_domain_id_of_pci(bus_id, &domain) == fu_success_k ? domain : no_memory_domain_k;
}

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

#pragma endregion Executor

} // namespace unum::ustore::utilities
