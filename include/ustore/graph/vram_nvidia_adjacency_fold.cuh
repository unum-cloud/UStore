/**
 *  @file include/ustore/graph/vram_nvidia_adjacency_fold.cuh
 *  @author Ash Vardanian
 *  @date October 25, 2026
 *  @brief The device tower of @c adjacency_fold_t, which composes the chains of half-edge changes of a whole merge at
 *      once on CUB.
 *
 *  Every chain composes as @c adjacency_fold_t::compose composes one: its records sort with the removals of the vertex
 *  first, then by neighbor, edge and role, and the newest record of every half-edge survives unless the newest removal
 *  of the vertex is newer, which drops removals of one half-edge too. The chains are segments of one sort keyed by
 *  chain first, so a merge holding thousands of chains launches a fixed number of kernels. A record's place in its
 *  chain is its index among the gathered records, as the chains are gathered back to back, oldest first.
 *
 *  Only nvcc compiles this header: host translation units name the fold through its declaration in
 *  @c adjacency_fold.hpp, and @c compaction.cu instantiates the device compactor calling it.
 */

#pragma once

#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <initializer_list> // `std::initializer_list`
#include <limits>           // `std::numeric_limits`
#include <span>             // `std::span`

#include <cub/device/device_for.cuh>
#include <cub/device/device_merge_sort.cuh>
#include <cub/device/device_scan.cuh>
#include <cub/device/device_segmented_reduce.cuh>
#include <cub/device/device_select.cuh>

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <smashtable/shared.hpp>       // `st::status_t`

#include "ustore/graph/adjacency_fold.hpp"         // `vram_nvidia_adjacency_fold_t`, `check_delta`, `save_delta`
#include "ustore/utilities/vram_nvidia_device.hpp" // `cuda_stream_t`, `cuda_device_buffer`, `cuda_pinned_buffer`

namespace unum::ustore::graph {

#pragma region Kernels

/** One record of a chain on the device, aligned so a copy moves it in three 16-byte words. */
struct alignas(16) half_edge_record_t {
    byte_t bytes[half_edge_delta_bytes_k];
};

/** The operation of @p record, as @c operation_of reads it. */
constexpr half_edge_operation_t record_operation(half_edge_record_t const& record) noexcept {
    return static_cast<half_edge_operation_t>(record.bytes[delta_operation_offset_k]);
}

/** Orders the half-edges of two records as @c compare_half_edges does: the neighbor's and then the edge's bytes as
 *  @c memcmp orders them, then the role. */
constexpr int compare_record_half_edges(half_edge_record_t const& record, half_edge_record_t const& other) noexcept {
    for (std::size_t offset = delta_neighbor_offset_k; offset != delta_weight_offset_k; ++offset)
        if (record.bytes[offset] != other.bytes[offset])
            return int(static_cast<std::uint8_t>(record.bytes[offset])) -
                   int(static_cast<std::uint8_t>(other.bytes[offset]));
    return int(static_cast<std::uint8_t>(record.bytes[delta_role_offset_k])) -
           int(static_cast<std::uint8_t>(other.bytes[delta_role_offset_k]));
}

/** Orders records by chain, then as @c sequenced_less orders the records of one chain, newest first where it orders
 *  oldest first, so the record of a half-edge that survives leads its run. */
struct chained_records_order_t {

    /** Every gathered record. */
    half_edge_record_t const* records;

    /** The chain of every gathered record. */
    std::uint32_t const* chains;

    constexpr bool operator()(std::uint32_t first, std::uint32_t second) const noexcept {
        if (chains[first] != chains[second]) return chains[first] < chains[second];
        bool const first_clears = record_operation(records[first]) == half_edge_operation_t::remove_vertex_k;
        bool const second_clears = record_operation(records[second]) == half_edge_operation_t::remove_vertex_k;
        if (first_clears != second_clears) return first_clears;
        if (!first_clears)
            if (int const order = compare_record_half_edges(records[first], records[second]); order) return order < 0;
        return first > second;
    }
};

/** Numbers every gathered record by its index. */
struct record_numbering_kernel {

    /** One index per record. */
    std::uint32_t* order;

    constexpr void operator()(std::int64_t index) const noexcept { order[index] = static_cast<std::uint32_t>(index); }
};

/** Flags the sorted records a composed chain keeps: its newest removal of the vertex, and the newest record of every
 *  half-edge newer than that removal, unless it removes the half-edge behind such a removal. */
struct kept_records_kernel {

    /** Every gathered record. */
    half_edge_record_t const* records;

    /** The chain of every gathered record. */
    std::uint32_t const* chains;

    /** Where each chain starts, among the gathered and the sorted records alike. */
    std::uint32_t const* chain_starts;

    /** The record indices in sorted order. */
    std::uint32_t const* order;

    /** One flag per sorted record. */
    std::uint8_t* flags;

    constexpr void operator()(std::int64_t signed_position) const noexcept {
        std::uint32_t const position = static_cast<std::uint32_t>(signed_position);
        std::uint32_t const index = order[position];
        std::uint32_t const start = chain_starts[chains[index]];
        half_edge_record_t const& record = records[index];
        if (record_operation(record) == half_edge_operation_t::remove_vertex_k) {
            flags[position] = position == start ? 1 : 0;
            return;
        }
        half_edge_record_t const& previous = records[order[position - (position != start ? 1 : 0)]];
        bool const leads = position == start || record_operation(previous) == half_edge_operation_t::remove_vertex_k ||
                           compare_record_half_edges(previous, record) != 0;
        std::uint32_t const cut = order[start];
        bool const removals = record_operation(records[cut]) == half_edge_operation_t::remove_vertex_k;
        bool const hidden = removals && (index < cut || record_operation(record) == half_edge_operation_t::remove_k);
        flags[position] = leads && !hidden ? 1 : 0;
    }
};

/** Copies every kept record into its composed chain, writing a removal of the vertex as @c save_delta spells one. */
struct composed_records_kernel {

    /** Every gathered record. */
    half_edge_record_t const* records;

    /** The indices of the kept records in sorted order. */
    std::uint32_t const* kept;

    /** The removal of the vertex as @c save_delta spells it. */
    half_edge_record_t removal;

    /** The composed chains back to back. */
    half_edge_record_t* composed;

    constexpr void operator()(std::int64_t index) const noexcept {
        half_edge_record_t const& record = records[kept[index]];
        composed[index] = record_operation(record) == half_edge_operation_t::remove_vertex_k ? removal : record;
    }
};

#pragma endregion Kernels

template <typename stream_type_>
st::status_t vram_nvidia_adjacency_fold_t::compose(stream_type_ const& stream, std::span<delta_view_t const> changes,
                                                   std::span<std::uint64_t const> chain_starts,
                                                   st::basic_vector<byte_t>& composed,
                                                   st::basic_vector<std::uint64_t>& composed_offsets) const noexcept {
    using st::status_t;
    using utilities::cuda_device_allocator;
    using utilities::cuda_device_buffer;
    using utilities::cuda_pinned_allocator;
    using utilities::cuda_pinned_buffer;
    using utilities::cuda_status;

    std::size_t const chains_count = chain_starts.empty() ? 0 : chain_starts.size() - 1;
    composed.clear();
    status_t status = composed_offsets.resize(chains_count + 1);
    if (status != status_t::success_k) return status;
    for (std::uint64_t& offset : composed_offsets) offset = 0;

    std::uint64_t total = 0;
    for (delta_view_t const& change : changes) {
        if (change.kind != delta_kind_t::change_k) return status_t::invalid_argument_k;
        if (change.bytes.size() % half_edge_delta_bytes_k) return status_t::invalid_argument_k;
        for (std::size_t offset = 0; offset != change.bytes.size(); offset += half_edge_delta_bytes_k)
            if (check_delta(change.bytes.data() + offset).failed()) return status_t::invalid_argument_k;
        total += change.bytes.size() / half_edge_delta_bytes_k;
    }
    // A record's index is its place in its chain, so every index fits the 32 bits the sort moves
    if (total > std::numeric_limits<std::uint32_t>::max()) return status_t::invalid_argument_k;
    if (total == 0) return status_t::success_k;

    int const device = stream.device();
    cudaStream_t const handle = stream.handle();
    st::expected<utilities::cuda_device_scope_t> const scope = utilities::cuda_device_scope_t::make(device);
    if (!scope) return scope.status();

    // The chains gather back to back into pinned memory, which the device reads at the bus's full speed
    cuda_pinned_buffer<half_edge_record_t> gathered {cuda_pinned_allocator<half_edge_record_t> {}};
    cuda_pinned_buffer<std::uint32_t> gathered_chains {cuda_pinned_allocator<std::uint32_t> {}};
    cuda_pinned_buffer<std::uint32_t> gathered_starts {cuda_pinned_allocator<std::uint32_t> {}};
    if ((status = gathered.reserve(total)) != status_t::success_k) return status;
    if ((status = gathered_chains.reserve(total)) != status_t::success_k) return status;
    if ((status = gathered_starts.reserve(chains_count + 1)) != status_t::success_k) return status;
    std::span<half_edge_record_t> const gathered_records {gathered.data(), total};
    std::span<std::uint32_t> const record_chains {gathered_chains.data(), total};
    std::span<std::uint32_t> const gathered_chain_starts {gathered_starts.data(), chains_count + 1};
    std::uint32_t count = 0;
    for (std::size_t chain = 0; chain != chains_count; ++chain) {
        gathered_chain_starts[chain] = count;
        for (std::uint64_t change = chain_starts[chain]; change != chain_starts[chain + 1]; ++change)
            for (std::size_t offset = 0; offset != changes[change].bytes.size(); offset += half_edge_delta_bytes_k) {
                std::memcpy(gathered_records[count].bytes, changes[change].bytes.data() + offset,
                            half_edge_delta_bytes_k);
                record_chains[count++] = static_cast<std::uint32_t>(chain);
            }
    }
    gathered_chain_starts[chains_count] = count;

    cuda_device_buffer<half_edge_record_t> records {cuda_device_allocator<half_edge_record_t>(device)};
    cuda_device_buffer<std::uint32_t> chains {cuda_device_allocator<std::uint32_t>(device)};
    cuda_device_buffer<std::uint32_t> starts {cuda_device_allocator<std::uint32_t>(device)};
    cuda_device_buffer<std::uint32_t> order {cuda_device_allocator<std::uint32_t>(device)};
    cuda_device_buffer<std::uint8_t> flags {cuda_device_allocator<std::uint8_t>(device)};
    cuda_device_buffer<std::uint32_t> kept {cuda_device_allocator<std::uint32_t>(device)};
    cuda_device_buffer<std::uint32_t> kept_counts {cuda_device_allocator<std::uint32_t>(device)};
    cuda_device_buffer<std::uint32_t> kept_offsets {cuda_device_allocator<std::uint32_t>(device)};
    cuda_device_buffer<half_edge_record_t> chained {cuda_device_allocator<half_edge_record_t>(device)};
    cuda_device_buffer<std::int64_t> selected {cuda_device_allocator<std::int64_t>(device)};
    cuda_device_buffer<std::byte> temporary {cuda_device_allocator<std::byte>(device)};
    for (status_t const reserved :
         {records.reserve(count), chains.reserve(count), starts.reserve(chains_count + 1), order.reserve(count),
          flags.reserve(count), kept.reserve(count), kept_counts.reserve(chains_count),
          kept_offsets.reserve(chains_count), chained.reserve(count), selected.reserve(1)})
        if (reserved != status_t::success_k) return reserved;
    if ((status = records.copy_in(gathered.data(), count, 0, handle)) != status_t::success_k) return status;
    if ((status = chains.copy_in(gathered_chains.data(), count, 0, handle)) != status_t::success_k) return status;
    if ((status = starts.copy_in(gathered_starts.data(), chains_count + 1, 0, handle)) != status_t::success_k)
        return status;

    // Every copy above waited for the stream, so growing the scratch frees nothing a kernel still reads
    auto const grow_temporary = [&temporary, &stream](std::size_t bytes) noexcept {
        if (bytes <= temporary.size()) return status_t::success_k;
        status_t const drained = stream.synchronize();
        return drained != status_t::success_k ? drained : temporary.reserve(bytes);
    };
    std::int64_t const items = static_cast<std::int64_t>(count);
    std::int64_t const segments = static_cast<std::int64_t>(chains_count);

    // Sort the record indices by chain, then as one chain composes, newest first among the records of one half-edge
    record_numbering_kernel const numbering {order.data()};
    if ((status = cuda_status(cub::DeviceFor::Bulk(items, numbering, handle))) != status_t::success_k) return status;
    chained_records_order_t const ordering {records.data(), chains.data()};
    std::size_t bytes = 0;
    status = cuda_status(cub::DeviceMergeSort::SortKeys(nullptr, bytes, order.data(), items, ordering, handle));
    if (status != status_t::success_k || (status = grow_temporary(bytes)) != status_t::success_k) return status;
    status = cuda_status(
        cub::DeviceMergeSort::SortKeys(temporary.data(), bytes, order.data(), items, ordering, handle));
    if (status != status_t::success_k) return status;

    kept_records_kernel const keeping {records.data(), chains.data(), starts.data(), order.data(), flags.data()};
    if ((status = cuda_status(cub::DeviceFor::Bulk(items, keeping, handle))) != status_t::success_k) return status;
    bytes = 0;
    status = cuda_status(cub::DeviceSelect::Flagged(nullptr, bytes, order.data(), flags.data(), kept.data(),
                                                    selected.data(), items, handle));
    if (status != status_t::success_k || (status = grow_temporary(bytes)) != status_t::success_k) return status;
    status = cuda_status(cub::DeviceSelect::Flagged(temporary.data(), bytes, order.data(), flags.data(), kept.data(),
                                                    selected.data(), items, handle));
    if (status != status_t::success_k) return status;

    // The sort keeps every chain where it was gathered, so the gathered starts bound its kept flags too
    bytes = 0;
    status = cuda_status(cub::DeviceSegmentedReduce::Sum(nullptr, bytes, flags.data(), kept_counts.data(), segments,
                                                         starts.data(), starts.data() + 1, handle));
    if (status != status_t::success_k || (status = grow_temporary(bytes)) != status_t::success_k) return status;
    status = cuda_status(cub::DeviceSegmentedReduce::Sum(temporary.data(), bytes, flags.data(), kept_counts.data(),
                                                         segments, starts.data(), starts.data() + 1, handle));
    if (status != status_t::success_k) return status;
    bytes = 0;
    status = cuda_status(
        cub::DeviceScan::ExclusiveSum(nullptr, bytes, kept_counts.data(), kept_offsets.data(), segments, handle));
    if (status != status_t::success_k || (status = grow_temporary(bytes)) != status_t::success_k) return status;
    status = cuda_status(cub::DeviceScan::ExclusiveSum(temporary.data(), bytes, kept_counts.data(), kept_offsets.data(),
                                                       segments, handle));
    if (status != status_t::success_k) return status;

    std::int64_t kept_count = 0;
    if ((status = selected.copy_out(&kept_count, 1, 0, handle)) != status_t::success_k) return status;
    half_edge_record_t removal;
    save_delta(half_edge_delta_t::remove_vertex(), removal.bytes);
    if (kept_count != 0) {
        composed_records_kernel const composing {records.data(), kept.data(), removal, chained.data()};
        status = cuda_status(cub::DeviceFor::Bulk(kept_count, composing, handle));
        if (status != status_t::success_k) return status;
    }

    std::size_t const composed_bytes = static_cast<std::size_t>(kept_count) * half_edge_delta_bytes_k;
    if ((status = composed.resize(composed_bytes)) != status_t::success_k) return status;
    if (composed_bytes != 0) {
        status = cuda_status(
            cudaMemcpyAsync(composed.data(), chained.data(), composed_bytes, cudaMemcpyDeviceToHost, handle));
        if (status != status_t::success_k) return status;
    }
    st::basic_vector<std::uint32_t> offsets;
    if ((status = offsets.resize(chains_count)) != status_t::success_k) return status;
    if ((status = kept_offsets.copy_out(offsets.data(), chains_count, 0, handle)) != status_t::success_k) return status;
    for (std::size_t chain = 0; chain != chains_count; ++chain)
        composed_offsets[chain] = std::uint64_t {offsets[chain]} * half_edge_delta_bytes_k;
    composed_offsets[chains_count] = composed_bytes;
    return stream.synchronize();
}

} // namespace unum::ustore::graph
