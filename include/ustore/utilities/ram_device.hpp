/**
 *  @file include/ustore/utilities/ram_device.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The host vocabulary of threads and topology, over ForkUnion's C ABI.
 *
 *  Engines, tools, tests and benchmarks dispatch through a ForkUnion pool, which runs on the
 *  calling thread while empty; besides the library's spawning only the arenas of
 *  @c utilities/ram_buffers.hpp call @c fu_* directly.
 */
#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`

#include <forkunion/forkunion.h>
#include <forkunion/types.hpp> // `fu::status_t`

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

#pragma endregion Executor

} // namespace unum::ustore::utilities
