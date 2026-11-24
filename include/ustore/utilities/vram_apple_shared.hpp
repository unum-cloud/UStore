/**
 *  @file include/ustore/utilities/vram_apple_shared.hpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The vocabulary every Apple launch and its Metal kernels share: where a launch binds its
 *      arguments and its block table, and the fixed extents every MSL source is written against.
 *
 *  Written in the C++ that MSL shares with every host compiler, as plain enums: the host includes
 *  it, and every Apple bundle embeds it first among its Metal sources, which cannot include, so
 *  the two sides read one definition of every slot they exchange. Namespaces nest one at a time,
 *  as MSL's dialect of C++ may predate nested namespace definitions.
 */
#pragma once

namespace unum {
namespace ustore {
namespace utilities {

/** The buffer and threadgroup slots every launch binds. */
enum metal_slot_t : unsigned int {

    /** The kernel's arguments, packed in order as one struct, every pointer a host address. */
    metal_arguments_slot_k = 0,

    /** The device's live blocks, sorted by host address, which turn host addresses
     *  into device ones. */
    metal_blocks_slot_k = 1,

    /** How many blocks the table holds, as one 32-bit word. */
    metal_blocks_count_slot_k = 2,

    /** The threadgroup memory of a launch, CUDA's dynamic shared memory. */
    metal_shared_slot_k = 0,
};

/** The fixed extents of every Metal launch. */
enum metal_extent_t : unsigned int {

    /** Lanes per simdgroup, which every pipeline must report as its @c threadExecutionWidth. */
    metal_simdgroup_width_k = 32,

    /** Bytes one entry of the block table takes: host begin and end, device begin,
     *  a reserved word. */
    metal_block_bytes_k = 32,

    /** Bytes the packed arguments may take, the most @c setBytes:length:atIndex: binds. */
    metal_arguments_capacity_k = 4096,
};

} // namespace utilities
} // namespace ustore
} // namespace unum
