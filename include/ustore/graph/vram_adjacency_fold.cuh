/**
 *  @file include/ustore/graph/vram_adjacency_fold.cuh
 *  @author Ash Vardanian
 *  @date October 25, 2026
 *  @brief The device tower of @c adjacency_fold_t, which composes the chains of half-edge changes of a whole merge at
 *      once in two launches.
 *
 *  Every chain composes as @c adjacency_fold_t::compose composes one: its records sort with the
 *  removals of the vertex first, then by neighbor, edge and role, and the newest record of every
 *  half-edge survives unless the newest removal of the vertex is newer, which drops removals of one
 *  half-edge too. The first launch takes one block per chain, which sorts the chain's records in
 *  shared memory, keeps what survives and counts it; the second hands every chain its offset
 *  through one chained scan and writes the composed records. A record's place in its chain is its
 *  index among the gathered records, as the chains are gathered back to back, oldest first.
 *
 *  A chain longer than the device's shared memory sorts spills: it runs the same bitonic network
 *  across blocks in device memory before the first launch, one launch per stride at or above the
 *  block's threads and one more for the strides below, and the first launch only keeps what
 *  survives of it.
 *
 *  An Apple device has no 64-bit atomics to chain with, so there the first launch also flags the
 *  places each chain kept, the select lists them in order, and the second writes every kept record
 *  at its rank; its kernels are the MSL of @c vram_compaction_apple.metal.
 */

#pragma once

#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <array>  // `std::array`
#include <bit>    // `std::bit_ceil`, `std::countr_zero`
#include <limits> // `std::numeric_limits`
#include <span>   // `std::span`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <smashtable/shared.hpp>       // `st::status_t`

#include "ustore/graph/adjacency_fold.hpp"      // `check_delta`, `save_delta`
#include "ustore/graph/vram_adjacency_fold.hpp" // `vram_adjacency_fold_t`
#include "ustore/utilities/vram_scan.cuh"       // `chain_ticket`, `chain_offset`, `select_flagged`

#if !defined(__CUDACC__) && !defined(__HIP__)
#include "ustore/utilities/vram_device.hpp" // `launch`, `carve`
#endif

namespace unum::ustore::graph {

/** Threads of every block a fold launches. */
inline constexpr unsigned fold_block_threads_k = 256;

/** Shared memory a fold block holds beside its sort: the block scan's totals, a 64-bit word per
 *  32 lanes, the most any vendor's scan kit keeps. */
inline constexpr std::uint32_t fold_scan_bytes_k = fold_block_threads_k / 32 * sizeof(std::uint64_t);

/** One record of a chain on the device, aligned so a copy moves it in three 16-byte words. */
struct alignas(16) half_edge_record_t {
    byte_t bytes[half_edge_delta_bytes_k];
};

/** The chains a block sorts no more of in its shared memory, which sort across blocks in device
 *  memory, each in the next power of two of its records, the longest first, so every one starts
 *  at a multiple of its width. */
struct fold_spill_t { // device-boundary

    /** The record indices of every spilled chain from where its slot starts, the largest index
     *  padding each past its chain. */
    std::uint32_t* sorted = nullptr;

    /** Where every slot starts in @c sorted, and the end of the last. */
    std::uint64_t const* starts = nullptr;

    /** The chain every slot sorts. */
    std::uint32_t const* chains = nullptr;

    /** Slots, one per spilled chain. */
    std::uint32_t count = 0;

    /** The widest chain a block sorts in its shared memory, past which chains spill. */
    std::uint32_t shared_width = 0;
};

static_assert(sizeof(fold_spill_t) == 32, "A fold's spill is four words");

/** Whether one block of @p device sorts a chain of @p width records in its shared memory, beside
 *  the scan's totals. */
inline bool fold_sort_fits(utilities::gpu_device_t const& device, std::uint32_t width) noexcept {
    return std::uint64_t {width} * sizeof(std::uint32_t) + fold_scan_bytes_k <= device.shared_memory_per_block_bytes;
}

#if defined(__CUDACC__) || defined(__HIP__)

#pragma region Kernels

/** The operation of @p record, as @c operation_of reads it. */
__device__ inline half_edge_operation_t record_operation(half_edge_record_t const& record) noexcept {
    return static_cast<half_edge_operation_t>(record.bytes[delta_operation_offset_k]);
}

/** Orders the half-edges of two records as @c compare_half_edges does: the neighbor's and then the edge's bytes as
 *  @c memcmp orders them, then the role. */
__device__ inline int compare_record_half_edges(half_edge_record_t const& record,
                                                half_edge_record_t const& other) noexcept {
    for (std::size_t offset = delta_neighbor_offset_k; offset != delta_weight_offset_k; ++offset)
        if (record.bytes[offset] != other.bytes[offset])
            return int(static_cast<std::uint8_t>(record.bytes[offset])) -
                   int(static_cast<std::uint8_t>(other.bytes[offset]));
    return int(static_cast<std::uint8_t>(record.bytes[delta_role_offset_k])) -
           int(static_cast<std::uint8_t>(other.bytes[delta_role_offset_k]));
}

/** Whether gathered record @p first orders before @p second of the same chain, as @c sequenced_less
 *  orders the records of one chain, newest first where it orders oldest first, so the record of a
 *  half-edge that survives leads its run; the largest index stands for the padding past the chain,
 *  which orders last. */
__device__ inline bool chained_before(half_edge_record_t const* records, std::uint32_t first,
                                      std::uint32_t second) noexcept {
    constexpr std::uint32_t padding = (std::numeric_limits<std::uint32_t>::max)();
    if (first == padding || second == padding) return second == padding && first != padding;
    bool const first_clears = record_operation(records[first]) == half_edge_operation_t::remove_vertex_k;
    bool const second_clears = record_operation(records[second]) == half_edge_operation_t::remove_vertex_k;
    if (first_clears != second_clears) return first_clears;
    if (!first_clears)
        if (int const order = compare_record_half_edges(records[first], records[second]); order) return order < 0;
    return first > second;
}

/** Orders the indices of @p sorted at @p index and @p stride past it, as one step of a bitonic sort
 *  of runs of @p size compares them, where @p index is the lower of the pair. */
__device__ inline void bitonic_exchange(half_edge_record_t const* records, std::uint32_t* sorted, std::uint32_t index,
                                        std::uint32_t size, std::uint32_t stride) noexcept {
    std::uint32_t const partner = index ^ stride;
    if (partner <= index) return;
    bool const ascending = (index & size) == 0;
    std::uint32_t const low = sorted[index], high = sorted[partner];
    if (ascending ? chained_before(records, high, low) : chained_before(records, low, high))
        sorted[index] = high, sorted[partner] = low;
}

/** One step of a bitonic sort of the @p width indices in @p sorted, comparing every pair @p stride
 *  apart within runs of @p size, each pair by one thread of the block. */
template <unsigned block_threads_>
__device__ void bitonic_step(half_edge_record_t const* records, std::uint32_t* sorted, std::uint32_t width,
                             std::uint32_t size, std::uint32_t stride) noexcept {
    for (std::uint32_t index = threadIdx.x; index < width; index += block_threads_)
        bitonic_exchange(records, sorted, index, size, stride);
}

/** As @c bitonic_exchange, at @p index of every spilled index, in the slot of @p spill holding it,
 *  whose chain is wide enough for runs of @p size. */
__device__ inline void spilled_exchange(half_edge_record_t const* records, fold_spill_t const& spill,
                                        std::uint64_t index, std::uint32_t size, std::uint32_t stride) noexcept {
    std::uint32_t slot = 0;
    for (std::uint32_t high = spill.count; high - slot > 1;) {
        std::uint32_t const middle = slot + (high - slot) / 2;
        if (spill.starts[middle] <= index) slot = middle;
        else high = middle;
    }
    std::uint64_t const first = spill.starts[slot];
    if (spill.starts[slot + 1] - first < size) return;
    bitonic_exchange(records, spill.sorted + first, static_cast<std::uint32_t>(index - first), size, stride);
}

/** One step of the bitonic sort of every chain @p spill holds, comparing pairs @p stride apart
 *  within runs of @p size: a stride at or above the block's threads spans the grid, and a shorter
 *  one finishes every stride below it too, within tiles of two indices per thread, each one
 *  block's, as a slot starts at a multiple of its width. */
template <unsigned block_threads_>
__device__ void fold_sort_step(half_edge_record_t const* records, fold_spill_t const& spill, std::uint32_t size,
                               std::uint32_t stride) noexcept {
    std::uint64_t const total = spill.starts[spill.count];
    if (stride >= block_threads_) {
        for (std::uint64_t index = std::uint64_t {blockIdx.x} * block_threads_ + threadIdx.x; index < total;
             index += std::uint64_t {gridDim.x} * block_threads_)
            spilled_exchange(records, spill, index, size, stride);
        return;
    }
    constexpr std::uint32_t tile = 2 * block_threads_;
    for (std::uint64_t first = std::uint64_t {blockIdx.x} * tile; first < total;
         first += std::uint64_t {gridDim.x} * tile)
        for (std::uint32_t step = stride; step; step /= 2) {
            for (std::uint32_t offset = threadIdx.x; offset < tile && first + offset < total; offset += block_threads_)
                spilled_exchange(records, spill, first + offset, size, step);
            __syncthreads();
        }
}

/** Whether the record at @p position of the sorted chain @p sorted survives: the newest removal of
 *  the vertex, or the newest record of a half-edge newer than it that does not remove the half-edge
 *  behind it, where @p newest_operation is what @p newest_removal, the chain's first, does. */
__device__ inline bool keeps_sorted(half_edge_record_t const* records, std::uint32_t const* sorted,
                                    std::uint32_t position, std::uint32_t newest_removal,
                                    half_edge_operation_t newest_operation) noexcept {
    std::uint32_t const index = sorted[position];
    half_edge_record_t const& record = records[index];
    if (record_operation(record) == half_edge_operation_t::remove_vertex_k) return position == 0;
    half_edge_record_t const& previous = records[sorted[position - (position != 0 ? 1 : 0)]];
    bool const leads = position == 0 || record_operation(previous) == half_edge_operation_t::remove_vertex_k ||
                       compare_record_half_edges(previous, record) != 0;
    bool const hidden = newest_operation == half_edge_operation_t::remove_vertex_k &&
                        (index < newest_removal || record_operation(record) == half_edge_operation_t::remove_k);
    return leads && !hidden;
}

/** Keeps what survives of the chain of @p length records from @p start, whose record indices
 *  @p sorted holds in order: its newest removal of the vertex and the newest record of every
 *  half-edge newer than that removal, unless it removes the half-edge behind such a removal; writes
 *  the kept indices over the chain's span of @p kept and their count into @p kept_count. */
template <unsigned block_threads_, typename scan_type_>
__device__ void keep_sorted_chain(half_edge_record_t const* records, std::uint32_t const* sorted, std::uint32_t start,
                                  std::uint32_t length, std::uint32_t* kept, std::uint32_t& kept_count) noexcept {
    std::uint32_t const newest_removal = sorted[0];
    half_edge_operation_t const newest_operation = record_operation(records[newest_removal]);
    std::uint64_t written = 0;
    for (std::uint32_t base = 0; base < length; base += block_threads_) {
        std::uint32_t const position = base + threadIdx.x;
        bool const keeps = position < length &&
                           keeps_sorted(records, sorted, position, newest_removal, newest_operation);
        utilities::block_scanned_t const scan = scan_type_::template block_exclusive<block_threads_>(keeps ? 1 : 0);
        if (keeps) kept[start + written + scan.before] = sorted[position];
        written += scan.total;
    }
    if (threadIdx.x == 0) kept_count = static_cast<std::uint32_t>(written);
    __syncthreads();
}

/** Composes chain @c blockIdx.x of @p starts and every chain a grid's width past it: sorts the
 *  record indices of a chain up to @c fold_spill_t::shared_width wide in @p sorted, shared memory
 *  for that many, and keeps what survives as @c keep_sorted_chain does; then keeps what survives
 *  of every chain @p spill sorted before. */
template <unsigned block_threads_, typename scan_type_>
__device__ void fold_chains(half_edge_record_t const* records, std::uint32_t const* starts, std::uint32_t chains_count,
                            fold_spill_t const& spill, std::uint32_t* sorted, std::uint32_t* kept,
                            std::uint32_t* kept_counts) noexcept {
    constexpr std::uint32_t padding = (std::numeric_limits<std::uint32_t>::max)();
    unsigned const thread = threadIdx.x;
    for (std::uint32_t chain = blockIdx.x; chain < chains_count; chain += gridDim.x) {
        std::uint32_t const start = starts[chain];
        std::uint32_t const length = starts[chain + 1] - start;
        if (length == 0) {
            if (thread == 0) kept_counts[chain] = 0;
            continue;
        }
        std::uint32_t width = 1;
        while (width < length) width *= 2;
        if (width > spill.shared_width) continue;
        for (std::uint32_t index = thread; index < width; index += block_threads_)
            sorted[index] = index < length ? start + index : padding;
        __syncthreads();
        // A bitonic sort, whose every step compares disjoint pairs, so the block only
        // meets between steps.
        for (std::uint32_t size = 2; size <= width; size *= 2)
            for (std::uint32_t stride = size / 2; stride; stride /= 2) {
                bitonic_step<block_threads_>(records, sorted, width, size, stride);
                __syncthreads();
            }
        keep_sorted_chain<block_threads_, scan_type_>(records, sorted, start, length, kept, kept_counts[chain]);
    }
    for (std::uint32_t slot = blockIdx.x; slot < spill.count; slot += gridDim.x) {
        std::uint32_t const chain = spill.chains[slot];
        keep_sorted_chain<block_threads_, scan_type_>(records, spill.sorted + spill.starts[slot], starts[chain],
                                                      starts[chain + 1] - starts[chain], kept, kept_counts[chain]);
    }
}

/** Writes the kept records of every chain back to back into @p composed, a removal of the vertex as
 *  @p removal spells it, and every chain's first record into @p offsets, the chains' tiles handing
 *  their totals through @p chain, which answers the records kept. */
template <unsigned block_threads_, typename scan_type_>
__device__ void fold_write(half_edge_record_t const* records, std::uint32_t const* starts, std::uint32_t const* kept,
                           std::uint32_t const* kept_counts, std::uint32_t chains_count, std::uint64_t tickets,
                           half_edge_record_t removal, half_edge_record_t* composed, std::uint64_t* offsets,
                           std::uint64_t* chain) noexcept {
    std::uint64_t const ticket = utilities::chain_ticket(chain);
    std::uint64_t const tile = utilities::divide_round_up(std::uint64_t {chains_count}, tickets);
    std::uint64_t const first = ticket * tile < chains_count ? ticket * tile : chains_count;
    std::uint64_t const end = first + tile < chains_count ? first + tile : chains_count;
    std::uint64_t count = 0;
    for (std::uint64_t index = first + threadIdx.x; index < end; index += block_threads_) count += kept_counts[index];
    std::uint64_t before = utilities::chain_offset(chain, ticket, tickets,
                                                   scan_type_::template block_exclusive<block_threads_>(count).total);
    for (std::uint64_t base = first; base < end; base += block_threads_) {
        std::uint64_t const index = base + threadIdx.x;
        std::uint64_t const thread_kept_count = index < end ? kept_counts[index] : 0;
        utilities::block_scanned_t const scan = scan_type_::template block_exclusive<block_threads_>(thread_kept_count);
        if (index < end) {
            std::uint64_t const offset = before + scan.before;
            offsets[index] = offset;
            for (std::uint64_t record = 0; record != thread_kept_count; ++record) {
                half_edge_record_t const& source = records[kept[starts[index] + record]];
                composed[offset + record] = record_operation(source) == half_edge_operation_t::remove_vertex_k ? removal
                                                                                                               : source;
            }
        }
        before += scan.total;
    }
}

#pragma endregion Kernels

#else

/** What both fold launches of one compose read and write on the device. */
struct fold_regions_t {

    /** The gathered records, oldest first per chain, and where every chain starts among them. */
    half_edge_record_t const* records = nullptr;
    std::uint32_t const* starts = nullptr;

    /** The indices each chain keeps, over its own span, and how many. */
    std::uint32_t* kept = nullptr;
    std::uint32_t* kept_counts = nullptr;

    /** The composed records and every chain's first one among them, the records kept after the
     *  last. */
    half_edge_record_t* composed = nullptr;
    std::uint64_t* offsets = nullptr;

    /** A removal of the vertex as the composed records spell it. */
    half_edge_record_t removal {};

    /** The chains sorting across blocks, the widest of them, zero for none, and their indices. */
    fold_spill_t spill {};
    std::uint32_t spilled_width = 0;
    std::uint64_t spilled_indices = 0;

    std::uint32_t chains_count = 0;
    std::uint32_t records_count = 0;

    /** The next power of two of the longest chain one block sorts in shared memory. */
    std::uint32_t width = 0;
};

/** Queues the bitonic sort of every chain @p regions spills on @p executor, of any vendor, before
 *  the first fold launch keeps what survives of them. */
template <typename executor_type_>
st::status_t launch_sort_steps(executor_type_& executor, fold_regions_t const& regions) noexcept {
    unsigned const blocks_count = static_cast<unsigned>(
        fu::min_of(utilities::divide_round_up(regions.spilled_indices, std::uint64_t {2 * fold_block_threads_k}),
                   std::uint64_t {65535}));
    // The launch of a stride below the block's threads finishes every stride below it
    for (std::uint32_t size = 2; size <= regions.spilled_width; size *= 2)
        for (std::uint32_t stride = size / 2; stride; stride = stride >= fold_block_threads_k ? stride / 2 : 0)
            if (utilities::launch(executor, executor.kernels()->function("ustore_fold_sort_step"), blocks_count,
                                  fold_block_threads_k, 0u, regions.records, regions.spill, size, stride)
                    .failed())
                return st::status_t::unknown_k;
    return st::status_t::success_k;
}

/** Queues both fold launches over @p regions on @p executor, of an NVIDIA or an AMD device, the
 *  second handing every chain its offset through one chained scan. */
template <typename executor_type_, typename blocks_type_>
st::status_t launch_folds(executor_type_& executor, blocks_type_&, fold_regions_t const& regions) noexcept {
    using st::status_t;
    // The kept total lands one past the chains' offsets, which the last chain's tile overwrites
    if (executor
            .fill_async(reinterpret_cast<std::uintptr_t>(regions.offsets + regions.chains_count), byte_t {0},
                        sizeof(std::uint64_t))
            .failed())
        return status_t::unknown_k;
    if (status_t const sorted = launch_sort_steps(executor, regions); sorted != status_t::success_k) return sorted;
    unsigned const blocks_count = regions.chains_count < 65535 ? regions.chains_count : 65535;
    if (utilities::launch(executor, executor.kernels()->function("ustore_fold_chains"), blocks_count,
                          fold_block_threads_k, static_cast<unsigned>(regions.width * sizeof(std::uint32_t)),
                          regions.records, regions.starts, regions.chains_count, regions.spill, regions.kept,
                          regions.kept_counts)
            .failed())
        return status_t::unknown_k;
    std::uint64_t const tickets = fu::min_of(
        utilities::divide_round_up(std::uint64_t {regions.chains_count}, std::uint64_t {fold_block_threads_k}),
        utilities::chain_blocks_max_k);
    if (utilities::launch(executor, executor.kernels()->function("ustore_fold_write"), static_cast<unsigned>(tickets),
                          fold_block_threads_k, 0u, regions.records, regions.starts,
                          static_cast<std::uint32_t const*>(regions.kept),
                          static_cast<std::uint32_t const*>(regions.kept_counts), regions.chains_count, tickets,
                          regions.removal, regions.composed, regions.offsets, regions.offsets + regions.chains_count)
            .failed())
        return status_t::unknown_k;
    return status_t::success_k;
}

/** As the other @c launch_folds, on an Apple device, which has no 64-bit atomics: the first
 *  launch flags the places every chain kept, the select over @p blocks lists them, and the second
 *  writes every one. */
template <typename blocks_type_>
st::status_t launch_folds(utilities::metal_executor_t& executor, blocks_type_& blocks,
                          fold_regions_t const& regions) noexcept {
    using st::status_t;
    // Few enough tiles that the select's one scanning threadgroup walks them quickly
    std::uint32_t const tile = static_cast<std::uint32_t>(utilities::round_up_to_multiple(
        utilities::divide_round_up(std::uint64_t {regions.records_count}, utilities::chain_blocks_max_k),
        std::uint64_t {256}));
    std::uint32_t const tiles = static_cast<std::uint32_t>(
        utilities::divide_round_up(std::uint64_t {regions.records_count}, std::uint64_t {tile}));
    auto const flags = utilities::carve<std::uint8_t>(blocks, executor, regions.records_count);
    auto const counts = utilities::carve<std::uint64_t>(blocks, executor, std::size_t {tiles} + 1);
    auto const selected = utilities::carve<std::uint32_t>(blocks, executor, regions.records_count);
    if (!flags || !counts || !selected) return status_t::out_of_memory_heap_k;
    if (status_t const sorted = launch_sort_steps(executor, regions); sorted != status_t::success_k) return sorted;
    unsigned const blocks_count = regions.chains_count < 65535 ? regions.chains_count : 65535;
    // Metal binds threadgroup memory in multiples of 16 bytes
    unsigned const sorted_bytes = utilities::round_up_to_multiple(regions.width * 4u, 16u);
    if (utilities::launch(executor, executor.kernels()->function("ustore_fold_chains"), blocks_count,
                          fold_block_threads_k, sorted_bytes, regions.records, regions.starts, regions.chains_count,
                          regions.spill, regions.kept, regions.kept_counts, flags->data())
            .failed())
        return status_t::unknown_k;
    if (utilities::select_flagged(executor, static_cast<std::uint8_t const*>(flags->data()), regions.records_count,
                                  tile, tiles, counts->data(), selected->data())
            .failed())
        return status_t::unknown_k;
    std::uint64_t const groups = fu::min_of(
        utilities::divide_round_up(std::uint64_t {regions.records_count}, std::uint64_t {fold_block_threads_k}),
        std::uint64_t {65535});
    if (utilities::launch(executor, executor.kernels()->function("ustore_fold_write"), static_cast<unsigned>(groups),
                          fold_block_threads_k, 0u, regions.records, regions.starts,
                          static_cast<std::uint32_t const*>(regions.kept), regions.chains_count, tiles, regions.removal,
                          regions.composed, regions.offsets, static_cast<std::uint32_t const*>(selected->data()),
                          static_cast<std::uint64_t const*>(counts->data()))
            .failed())
        return status_t::unknown_k;
    return status_t::success_k;
}

template <typename executor_type_, typename blocks_type_>
st::status_t vram_adjacency_fold_t::compose(executor_type_& executor, blocks_type_& blocks,
                                            std::span<delta_view_t const> changes,
                                            std::span<std::uint64_t const> chain_starts, st::vector<byte_t>& composed,
                                            st::vector<std::uint64_t>& composed_offsets) const noexcept {
    using st::status_t;
    auto const device_status = [](utilities::status_t const status) noexcept {
        return status.failed() ? status_t::unknown_k : status_t::success_k;
    };

    std::size_t const chains_count = chain_starts.empty() ? 0 : chain_starts.size() - 1;
    composed.clear();
    status_t status = composed_offsets.resize(chains_count + 1);
    if (status != status_t::success_k) return status;
    for (std::uint64_t& offset : composed_offsets) offset = 0;

    // The widest chain one block sorts in its shared memory, past which chains spill
    std::uint32_t shared_width = 1;
    while (fold_sort_fits(executor.device(), shared_width * 2)) shared_width *= 2;

    std::uint64_t total = 0;
    std::uint64_t longest = 0;
    // Spilled chains per width by its bits, which then name where the width's next one goes
    std::array<std::uint32_t, 64> spilled_slots {};
    std::uint32_t spilled_chains_count = 0;
    for (std::size_t chain = 0; chain != chains_count; ++chain) {
        std::uint64_t records = 0;
        for (std::uint64_t change = chain_starts[chain]; change != chain_starts[chain + 1]; ++change) {
            delta_view_t const& delta = changes[change];
            if (delta.kind != delta_kind_t::change_k) return status_t::invalid_argument_k;
            if (delta.bytes.size() % half_edge_delta_bytes_k) return status_t::invalid_argument_k;
            for (std::size_t offset = 0; offset != delta.bytes.size(); offset += half_edge_delta_bytes_k)
                if (check_delta(delta.bytes.data() + offset).failed()) return status_t::invalid_argument_k;
            records += delta.bytes.size() / half_edge_delta_bytes_k;
        }
        total += records;
        longest = records > longest ? records : longest;
        std::uint64_t const width = std::bit_ceil(records);
        if (width > shared_width) ++spilled_slots[std::countr_zero(width)], ++spilled_chains_count;
    }
    // A record's index is its place among the gathered ones, the largest left for the padding
    if (total > (std::numeric_limits<std::uint32_t>::max)()) return status_t::capacity_exhausted_k;
    if (total == 0) return status_t::success_k;
    if (!executor.kernels()) return status_t::invalid_argument_k;

    // Every width starts where the wider ones end, so a spilled chain starts at a multiple of it
    std::array<std::uint64_t, 64> spilled_firsts {};
    std::uint64_t spilled_indices = 0;
    for (std::uint32_t bits = 64, slots = 0; bits-- != 0;) {
        std::uint32_t const chains_of_width = spilled_slots[bits];
        spilled_slots[bits] = slots, spilled_firsts[bits] = spilled_indices;
        slots += chains_of_width, spilled_indices += std::uint64_t {chains_of_width} << bits;
    }

    // The chains gather back to back into the pinned bounce, which the device reads at the
    // bus's full speed
    auto const bounce = [&executor]<typename element_type_>(std::size_t count) noexcept {
        auto [carved, carve_status] = utilities::arena_allocate<element_type_>(executor.bounce(), count);
        return carve_status.failed() ? static_cast<element_type_*>(nullptr) : carved.data();
    };
    std::size_t const records_count = static_cast<std::size_t>(total);
    half_edge_record_t* const gathered = bounce.template operator()<half_edge_record_t>(records_count);
    std::uint32_t* const gathered_starts = bounce.template operator()<std::uint32_t>(chains_count + 1);
    half_edge_record_t* const downloaded = bounce.template operator()<half_edge_record_t>(records_count);
    std::uint64_t* const downloaded_offsets = bounce.template operator()<std::uint64_t>(chains_count + 1);
    std::uint32_t* const spilled_sorted = bounce.template operator()<std::uint32_t>(spilled_indices);
    std::uint64_t* const spilled_starts = bounce.template operator()<std::uint64_t>(spilled_chains_count + 1);
    std::uint32_t* const spilled_chains = bounce.template operator()<std::uint32_t>(spilled_chains_count);
    if (!gathered || !gathered_starts || !downloaded || !downloaded_offsets || !spilled_starts ||
        (spilled_chains_count != 0 && (!spilled_sorted || !spilled_chains)) || executor.pin_bounce().failed())
        return status_t::out_of_memory_heap_k;
    constexpr std::uint32_t padding = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t count = 0;
    for (std::size_t chain = 0; chain != chains_count; ++chain) {
        gathered_starts[chain] = count;
        for (std::uint64_t change = chain_starts[chain]; change != chain_starts[chain + 1]; ++change)
            for (std::size_t offset = 0; offset != changes[change].bytes.size(); offset += half_edge_delta_bytes_k)
                std::memcpy(gathered[count++].bytes, changes[change].bytes.data() + offset, half_edge_delta_bytes_k);
        std::uint32_t const length = count - gathered_starts[chain];
        std::uint64_t const width = std::bit_ceil(std::uint64_t {length});
        if (width <= shared_width) continue;
        int const bits = std::countr_zero(width);
        std::uint32_t const slot = spilled_slots[bits]++;
        std::uint64_t const first = spilled_firsts[bits];
        spilled_firsts[bits] += width;
        spilled_chains[slot] = static_cast<std::uint32_t>(chain), spilled_starts[slot] = first;
        for (std::uint64_t index = 0; index != width; ++index)
            spilled_sorted[first + index] = index < length ? gathered_starts[chain] + static_cast<std::uint32_t>(index)
                                                           : padding;
    }
    gathered_starts[chains_count] = count;
    spilled_starts[spilled_chains_count] = spilled_indices;

    auto const device = [&]<typename element_type_>(std::size_t elements) noexcept {
        auto const carved = utilities::carve<element_type_>(blocks, executor, elements);
        return carved ? carved->data() : static_cast<element_type_*>(nullptr);
    };
    half_edge_record_t* const records = device.template operator()<half_edge_record_t>(records_count);
    std::uint32_t* const starts = device.template operator()<std::uint32_t>(chains_count + 1);
    std::uint32_t* const kept = device.template operator()<std::uint32_t>(records_count);
    std::uint32_t* const kept_counts = device.template operator()<std::uint32_t>(chains_count);
    half_edge_record_t* const chained = device.template operator()<half_edge_record_t>(records_count);
    std::uint64_t* const offsets = device.template operator()<std::uint64_t>(chains_count + 1);
    if (!records || !starts || !kept || !kept_counts || !chained || !offsets) return status_t::out_of_memory_heap_k;
    std::span<half_edge_record_t const> const gathered_records {gathered, records_count};
    std::span<std::uint32_t const> const gathered_chain_starts {gathered_starts, chains_count + 1};
    if ((status = device_status(executor.upload_async(reinterpret_cast<std::uintptr_t>(records),
                                                      std::as_bytes(gathered_records)))) != status_t::success_k)
        return status;
    if ((status = device_status(executor.upload_async(reinterpret_cast<std::uintptr_t>(starts),
                                                      std::as_bytes(gathered_chain_starts)))) != status_t::success_k)
        return status;

    fold_regions_t regions {records, starts, kept, kept_counts, chained, offsets};
    save_delta(half_edge_delta_t::remove_vertex(), regions.removal.bytes);
    regions.chains_count = static_cast<std::uint32_t>(chains_count);
    regions.records_count = static_cast<std::uint32_t>(records_count);
    regions.width = static_cast<std::uint32_t>(fu::min_of(std::bit_ceil(longest), std::uint64_t {shared_width}));
    regions.spill.count = spilled_chains_count;
    regions.spill.shared_width = shared_width;
    if (spilled_chains_count != 0) {
        regions.spill.sorted = device.template operator()<std::uint32_t>(spilled_indices);
        std::uint64_t* const spill_starts = device.template operator()<std::uint64_t>(spilled_chains_count + 1);
        std::uint32_t* const spill_chains = device.template operator()<std::uint32_t>(spilled_chains_count);
        if (!regions.spill.sorted || !spill_starts || !spill_chains) return status_t::out_of_memory_heap_k;
        regions.spill.starts = spill_starts, regions.spill.chains = spill_chains;
        regions.spilled_width = static_cast<std::uint32_t>(std::bit_ceil(longest));
        regions.spilled_indices = spilled_indices;
        std::span<std::uint32_t const> const sorted_indices {spilled_sorted, static_cast<std::size_t>(spilled_indices)};
        std::span<std::uint64_t const> const slot_starts {spilled_starts, spilled_chains_count + std::size_t {1}};
        std::span<std::uint32_t const> const slot_chains {spilled_chains, spilled_chains_count};
        if ((status = device_status(executor.upload_async(reinterpret_cast<std::uintptr_t>(regions.spill.sorted),
                                                          std::as_bytes(sorted_indices)))) != status_t::success_k ||
            (status = device_status(executor.upload_async(reinterpret_cast<std::uintptr_t>(spill_starts),
                                                          std::as_bytes(slot_starts)))) != status_t::success_k ||
            (status = device_status(executor.upload_async(reinterpret_cast<std::uintptr_t>(spill_chains),
                                                          std::as_bytes(slot_chains)))) != status_t::success_k)
            return status;
    }
    if ((status = launch_folds(executor, blocks, regions)) != status_t::success_k) return status;

    // Every chain keeps at most what it gathered, so the whole span comes back and the offsets say
    // how much is live
    std::span<half_edge_record_t> const downloaded_records {downloaded, records_count};
    std::span<std::uint64_t> const downloaded_chain_offsets {downloaded_offsets, chains_count + 1};
    if ((status = device_status(executor.download_async(std::as_writable_bytes(downloaded_records),
                                                        reinterpret_cast<std::uintptr_t>(chained)))) !=
        status_t::success_k)
        return status;
    if ((status = device_status(executor.download_async(std::as_writable_bytes(downloaded_chain_offsets),
                                                        reinterpret_cast<std::uintptr_t>(offsets)))) !=
        status_t::success_k)
        return status;
    if ((status = device_status(executor.wait())) != status_t::success_k) return status;

    std::size_t const composed_bytes = static_cast<std::size_t>(downloaded_offsets[chains_count]) *
                                       half_edge_delta_bytes_k;
    if ((status = composed.resize(composed_bytes)) != status_t::success_k) return status;
    if (composed_bytes != 0) std::memcpy(composed.data(), downloaded, composed_bytes);
    for (std::size_t chain = 0; chain <= chains_count; ++chain)
        composed_offsets[chain] = downloaded_offsets[chain] * half_edge_delta_bytes_k;
    return status_t::success_k;
}

#endif

} // namespace unum::ustore::graph
