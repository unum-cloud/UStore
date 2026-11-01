/**
 *  @file include/ustore/graph/vram_nvidia_adjacency_fold.cuh
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
 */

#pragma once

#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <limits> // `std::numeric_limits`
#include <span>   // `std::span`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <smashtable/shared.hpp>       // `st::status_t`

#include "ustore/graph/adjacency_fold.hpp"             // `check_delta`, `save_delta`
#include "ustore/graph/vram_nvidia_adjacency_fold.hpp" // `vram_nvidia_adjacency_fold_t`
#include "ustore/utilities/vram_scan.cuh"              // `chain_ticket`, `chain_offset`, `block_exclusive_scan`

#if !defined(__CUDACC__) && !defined(__HIP__)
#include "ustore/utilities/vram_device.hpp" // `launch`, `carve`
#endif

namespace unum::ustore::graph {

/** Threads of every block a fold launches. */
inline constexpr unsigned fold_block_threads_k = 256;

/** Records one chain holds at most for its block to sort it in shared memory, so a merge with a
 *  longer one folds on the host. */
inline constexpr std::uint32_t fold_chain_records_max_k = 16384;

/** One record of a chain on the device, aligned so a copy moves it in three 16-byte words. */
struct alignas(16) half_edge_record_t {
    byte_t bytes[half_edge_delta_bytes_k];
};

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
    constexpr std::uint32_t padding = std::numeric_limits<std::uint32_t>::max();
    if (first == padding || second == padding) return second == padding && first != padding;
    bool const first_clears = record_operation(records[first]) == half_edge_operation_t::remove_vertex_k;
    bool const second_clears = record_operation(records[second]) == half_edge_operation_t::remove_vertex_k;
    if (first_clears != second_clears) return first_clears;
    if (!first_clears)
        if (int const order = compare_record_half_edges(records[first], records[second]); order) return order < 0;
    return first > second;
}

/** Composes chain @c blockIdx.x of @p starts and every chain a grid's width past it: sorts the
 *  chain's record indices in @p sorted, shared memory for the next power of two of the chain's
 *  records, keeps its newest removal of the vertex and the newest record of every half-edge newer
 *  than that removal, unless it removes the half-edge behind such a removal, and writes the kept
 *  indices over the chain's span of @p kept and their count into @p kept_counts. */
template <unsigned block_threads_>
__device__ void fold_chains(half_edge_record_t const* records, std::uint32_t const* starts, std::uint32_t chains_count,
                            std::uint32_t* sorted, std::uint32_t* kept, std::uint32_t* kept_counts) noexcept {
    constexpr std::uint32_t padding = std::numeric_limits<std::uint32_t>::max();
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
        for (std::uint32_t index = thread; index < width; index += block_threads_)
            sorted[index] = index < length ? start + index : padding;
        __syncthreads();
        // A bitonic sort, whose every step compares disjoint pairs, so the block only
        // meets between steps.
        for (std::uint32_t size = 2; size <= width; size *= 2)
            for (std::uint32_t stride = size / 2; stride; stride /= 2) {
                for (std::uint32_t index = thread; index < width; index += block_threads_) {
                    std::uint32_t const partner = index ^ stride;
                    if (partner <= index) continue;
                    bool const ascending = (index & size) == 0;
                    std::uint32_t const low = sorted[index], high = sorted[partner];
                    if (ascending ? chained_before(records, high, low) : chained_before(records, low, high))
                        sorted[index] = high, sorted[partner] = low;
                }
                __syncthreads();
            }

        std::uint32_t const newest_removal = sorted[0];
        bool const removals = record_operation(records[newest_removal]) == half_edge_operation_t::remove_vertex_k;
        std::uint64_t written = 0;
        for (std::uint32_t base = 0; base < length; base += block_threads_) {
            std::uint32_t const position = base + thread;
            bool keeps = false;
            if (position < length) {
                std::uint32_t const index = sorted[position];
                half_edge_record_t const& record = records[index];
                if (record_operation(record) == half_edge_operation_t::remove_vertex_k) keeps = position == 0;
                else {
                    half_edge_record_t const& previous = records[sorted[position - (position != 0 ? 1 : 0)]];
                    bool const leads = position == 0 ||
                                       record_operation(previous) == half_edge_operation_t::remove_vertex_k ||
                                       compare_record_half_edges(previous, record) != 0;
                    bool const hidden = removals && (index < newest_removal ||
                                                     record_operation(record) == half_edge_operation_t::remove_k);
                    keeps = leads && !hidden;
                }
            }
            utilities::block_scanned_t const scan = utilities::block_exclusive_scan<block_threads_>(keeps ? 1 : 0);
            if (keeps) kept[start + written + scan.before] = sorted[position];
            written += scan.total;
        }
        if (thread == 0) kept_counts[chain] = static_cast<std::uint32_t>(written);
        __syncthreads();
    }
}

/** Writes the kept records of every chain back to back into @p composed, a removal of the vertex as
 *  @p removal spells it, and every chain's first record into @p offsets, the chains' tiles handing
 *  their totals through @p chain, which answers the records kept. */
template <unsigned block_threads_>
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
                                                   utilities::block_exclusive_scan<block_threads_>(count).total);
    for (std::uint64_t base = first; base < end; base += block_threads_) {
        std::uint64_t const index = base + threadIdx.x;
        std::uint64_t const thread_kept_count = index < end ? kept_counts[index] : 0;
        utilities::block_scanned_t const scan = utilities::block_exclusive_scan<block_threads_>(thread_kept_count);
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

template <typename executor_type_, typename blocks_type_>
st::status_t vram_nvidia_adjacency_fold_t::compose(executor_type_& executor, blocks_type_& blocks,
                                                   std::span<delta_view_t const> changes,
                                                   std::span<std::uint64_t const> chain_starts,
                                                   st::vector<byte_t>& composed,
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

    std::uint64_t total = 0;
    std::uint64_t longest = 0;
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
    }
    // A record's index is its place in its chain, so every index fits 32 bits, and a chain sorts in
    // one block's memory
    if (total > std::numeric_limits<std::uint32_t>::max()) return status_t::invalid_argument_k;
    if (longest > fold_chain_records_max_k) return status_t::out_of_memory_heap_k;
    if (total == 0) return status_t::success_k;
    if (!executor.kernels()) return status_t::invalid_argument_k;

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
    if (!gathered || !gathered_starts || !downloaded || !downloaded_offsets || executor.pin_bounce().failed())
        return status_t::out_of_memory_heap_k;
    std::uint32_t count = 0;
    for (std::size_t chain = 0; chain != chains_count; ++chain) {
        gathered_starts[chain] = count;
        for (std::uint64_t change = chain_starts[chain]; change != chain_starts[chain + 1]; ++change)
            for (std::size_t offset = 0; offset != changes[change].bytes.size(); offset += half_edge_delta_bytes_k)
                std::memcpy(gathered[count++].bytes, changes[change].bytes.data() + offset, half_edge_delta_bytes_k);
    }
    gathered_starts[chains_count] = count;

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
    // The kept total lands one past the chains' offsets, which the last chain's tile
    // overwrites with it
    if ((status = device_status(executor.fill_async(reinterpret_cast<std::uintptr_t>(offsets + chains_count),
                                                    byte_t {0}, sizeof(std::uint64_t)))) != status_t::success_k)
        return status;

    std::uint32_t width = 1;
    while (width < longest) width *= 2;
    std::uint32_t const chains = static_cast<std::uint32_t>(chains_count);
    unsigned const blocks_count = static_cast<unsigned>(chains_count < 65535 ? chains_count : 65535);
    status = device_status(utilities::launch(executor, executor.kernels()->function("ustore_fold_chains"), blocks_count,
                                             fold_block_threads_k, static_cast<unsigned>(width * sizeof(std::uint32_t)),
                                             static_cast<half_edge_record_t const*>(records),
                                             static_cast<std::uint32_t const*>(starts), chains, kept, kept_counts));
    if (status != status_t::success_k) return status;
    half_edge_record_t removal;
    save_delta(half_edge_delta_t::remove_vertex(), removal.bytes);
    std::uint64_t const tickets = std::min(
        utilities::divide_round_up(std::uint64_t {chains_count}, std::uint64_t {fold_block_threads_k}),
        utilities::chain_blocks_max_k);
    status = device_status(
        utilities::launch(executor, executor.kernels()->function("ustore_fold_write"), static_cast<unsigned>(tickets),
                          fold_block_threads_k, 0u, static_cast<half_edge_record_t const*>(records),
                          static_cast<std::uint32_t const*>(starts), static_cast<std::uint32_t const*>(kept),
                          static_cast<std::uint32_t const*>(kept_counts), chains, tickets, removal, chained, offsets,
                          offsets + chains_count));
    if (status != status_t::success_k) return status;

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
