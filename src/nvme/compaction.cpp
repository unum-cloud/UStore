/**
 *  @file src/nvme/compaction.cpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The compiled part of GPU compaction: binding a compactor to an executor whose device
 *      loaded the library's kernels, freeing its device runs, and compacting every key slot.
 *
 *  The graph's device fold for the graph's key slot is instantiated here as well, so the host TUs
 *  of the engine call it.
 */

#include "ustore/blobs/vram_nvidia_compaction.cuh"     // `cuda_compactor_t`
#include "ustore/graph/vram_nvidia_adjacency_fold.cuh" // the fold's device definitions

namespace unum::ustore::blobs {

expected<cuda_compactor_t> cuda_compactor_t::make(utilities::cuda_executor_t& executor,
                                                  device_sections_reader_t sections) noexcept {
    if (!executor.kernels())
        return status_t {status_code_t::unsupported_k, "The device loaded no bundle of the library's kernels"};
    return cuda_compactor_t(executor, sections);
}

st::status_t cuda_compactor_t::merge_strings(compaction_kind_t kind, std::span<strings_run_view const> views,
                                             std::uint32_t upper_runs, compaction_stamps_t const& stamps,
                                             st::vector<strings_version_t>& survivors) noexcept {
    using st::status_t;
    if (!executor_->kernels() || views.size() > std::numeric_limits<std::uint32_t>::max() || upper_runs > views.size())
        return status_t::invalid_argument_k;
    std::size_t const runs_count = views.size();
    std::size_t records = 0, keys_bytes = 0, saved_count = 0;
    for (strings_run_view const& view : views) {
        if (view.heads->size() > std::numeric_limits<std::uint32_t>::max()) return status_t::invalid_argument_k;
        records += view.heads->size();
        keys_bytes += view.keys.size();
        saved_count += (view.stamps->saved_bytes() + view.kinds->saved_bytes()) / sizeof(std::uint64_t);
    }

    if (executor_->bind().failed()) return status_t::unknown_k;
    executor_->rewind_bounce();
    if (blocks_.count > 1) sections_.release();
    utilities::rewind(blocks_, *executor_);

    strings_merge_run* const runs = carve_<strings_merge_run>(runs_count);
    st::key128_t* const heads = carve_<st::key128_t>(records);
    std::uint32_t* const offsets = carve_<std::uint32_t>(records + runs_count);
    byte_t* const keys = carve_<byte_t>(keys_bytes);
    std::uint64_t* const saved = carve_<std::uint64_t>(saved_count);
    std::uint64_t* const pinned = carve_<std::uint64_t>(static_cast<std::size_t>(stamps.pinned_count));
    strings_version_t* const merged = carve_<strings_version_t>(records);
    std::uint8_t* const flags = carve_<std::uint8_t>(records);
    if ((runs_count && (!runs || !offsets)) || (records && (!heads || !merged || !flags)) || (keys_bytes && !keys) ||
        (saved_count && !saved) || (stamps.pinned_count && !pinned))
        return status_t::out_of_memory_heap_k;

    strings_merge_run* const host_runs = carve_bounce<strings_merge_run>(*executor_, runs_count);
    st::key128_t* const host_heads = carve_bounce<st::key128_t>(*executor_, records);
    std::uint64_t* const host_saved = carve_bounce<std::uint64_t>(*executor_, saved_count);
    strings_version_t* const host_merged = carve_bounce<strings_version_t>(*executor_, records);
    std::uint8_t* const host_flags = carve_bounce<std::uint8_t>(*executor_, records);
    if ((runs_count && !host_runs) || (records && (!host_heads || !host_merged || !host_flags)) ||
        (saved_count && !host_saved) || executor_->pin_bounce().failed())
        return status_t::out_of_memory_heap_k;

    // The heads leave the trees as one flat column, so adjacent threads load adjacent heads.
    status_t status = status_t::success_k;
    std::size_t first = 0, offsets_at = 0, keys_at = 0, saved_at = 0;
    for (std::size_t run = 0; run != runs_count; ++run) {
        strings_run_view const& view = views[run];
        std::size_t const count = view.heads->size();
        for (std::size_t position = 0; position != count; ++position)
            host_heads[first + position] = view.heads->select(position);
        strings_merge_run& described = host_runs[run];
        described = strings_merge_run {heads + first, offsets + offsets_at, keys + keys_at, nullptr, nullptr, first,
                                       count};
        std::size_t const stamps_bytes = view.stamps->saved_bytes();
        if (view.stamps->save({reinterpret_cast<std::byte*>(host_saved + saved_at), stamps_bytes}) !=
            status_t::success_k)
            return status_t::invalid_argument_k;
        described.saved_stamps = saved + saved_at;
        saved_at += stamps_bytes / sizeof(std::uint64_t);
        std::size_t const kinds_bytes = view.kinds->saved_bytes();
        if (view.kinds->save({reinterpret_cast<std::byte*>(host_saved + saved_at), kinds_bytes}) != status_t::success_k)
            return status_t::invalid_argument_k;
        described.saved_kinds = saved + saved_at;
        saved_at += kinds_bytes / sizeof(std::uint64_t);
        if ((status = upload(*executor_, offsets + offsets_at, view.key_offsets.data(), count + 1)) !=
                status_t::success_k ||
            (status = upload(*executor_, keys + keys_at, view.keys.data(), view.keys.size())) != status_t::success_k)
            return status;
        first += count, offsets_at += count + 1, keys_at += view.keys.size();
    }
    if ((status = upload(*executor_, heads, host_heads, records)) != status_t::success_k ||
        (status = upload(*executor_, saved, host_saved, saved_count)) != status_t::success_k ||
        (status = upload(*executor_, runs, host_runs, runs_count)) != status_t::success_k)
        return status;
    compaction_stamps_t device_stamps = stamps;
    if (stamps.pinned_count != 0) {
        status = upload(*executor_, pinned, stamps.pinned, static_cast<std::size_t>(stamps.pinned_count));
        if (status != status_t::success_k) return status;
        device_stamps.pinned = pinned;
    }

    if (records != 0) {
        status = launch_over(*executor_, merge_strings_rank_name_k, records, std::uint64_t {1} << 20,
                             static_cast<strings_merge_run const*>(runs), static_cast<std::uint32_t>(runs_count),
                             upper_runs, std::uint64_t {records}, kind, device_stamps, merged, flags);
        if (status != status_t::success_k) return status;
    }
    if ((status = download(*executor_, host_merged, merged, records)) != status_t::success_k ||
        (status = download(*executor_, host_flags, flags, records)) != status_t::success_k)
        return status;
    if ((status = executor_status(executor_->wait())) != status_t::success_k) return status;

    survivors.clear();
    if (survivors.reserve(records) != status_t::success_k) return status_t::out_of_memory_heap_k;
    for (std::size_t index = 0; index != records; ++index)
        if (host_flags[index] != 0) [[maybe_unused]]
            status_t const kept = survivors.push_back(st::assume_reserved, strings_version_t {host_merged[index]});
    return status_t::success_k;
}

cuda_compactor_t::~cuda_compactor_t() noexcept {
    if (!blocks_.count) return;
    sections_.release();
    if (executor_->bind().failed()) return;
    utilities::rewind(blocks_, *executor_);
    executor_->free_async(blocks_.blocks[0].address);
}

} // namespace unum::ustore::blobs

#include "nvme/compaction_variants.hpp" // the explicit instantiations of both
