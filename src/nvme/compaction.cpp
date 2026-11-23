/**
 *  @file src/nvme/compaction.cpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The compiled part of GPU compaction: binding each vendor's compactor to its executor
 *      whose device loaded the library's kernels, freeing its device runs, and compacting each of
 *      the key slots the engine stores.
 *
 *  The compactors queue their merges from @c vram_nvidia_compaction.cuh, @c vram_amd_compaction.hip
 *  and @c vram_apple_compaction.hpp, which this unit alone instantiates. The graph's device fold
 *  for the graph's key slot is instantiated here as well, so the host TUs of the engine call it.
 */

#include "ustore/blobs/vram_amd_compaction.hip"    // `hip_compactor_t`
#include "ustore/blobs/vram_apple_compaction.hpp"  // `metal_compactor_t`
#include "ustore/blobs/vram_nvidia_compaction.cuh" // `cuda_compactor_t`, `launch_over`
#include "ustore/graph/vram_adjacency_fold.cuh"    // the fold's device definitions
#include "ustore/utilities/vram_scan.cuh"          // `select_flagged`

#include "nvme/compaction_variants.hpp" // the key slots every vendor's compactor is instantiated over

namespace unum::ustore::blobs {

using util::bytes_of;

#pragma region NVIDIA

expected<cuda_compactor_t> cuda_compactor_t::make(util::cuda_executor_t& executor,
                                                  device_sections_reader_t sections) noexcept {
    if (!executor.kernels()) return status_t {status_code_t::unsupported_k, "The device loaded no kernel bundle"};
    return cuda_compactor_t(executor, sections);
}

st::status_t cuda_compactor_t::merge_strings(compaction_kind_t kind, std::span<strings_run_view const> views,
                                             std::uint32_t upper_runs, compaction_stamps_t const& stamps,
                                             st::vector<strings_version_t>& survivors) noexcept {
    using st::status_t;
    if (!executor_->kernels() || views.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        upper_runs > views.size())
        return status_t::invalid_argument_k;
    std::size_t const runs_count = views.size();
    std::size_t records = 0, keys_bytes = 0, saved_count = 0, ranges_count = 0, ranges_bytes = 0;
    for (strings_run_view const& view : views) {
        if (view.heads->size() > (std::numeric_limits<std::uint32_t>::max)()) return status_t::invalid_argument_k;
        records += view.heads->size();
        keys_bytes += view.keys.size();
        saved_count += (view.stamps->saved_bytes() + view.kinds->saved_bytes()) / sizeof(std::uint64_t);
        if (view.range_tombstones == nullptr) continue;
        ranges_count += view.range_tombstones->size();
        for (range_tombstone<std::string_view> const& range : view.range_tombstones->ranges())
            ranges_bytes += range.start.size() + range.end.size();
    }
    if (ranges_bytes > (std::numeric_limits<std::uint32_t>::max)()) return status_t::invalid_argument_k;

    if (executor_->bind().failed()) return status_t::unknown_k;
    executor_->rewind_bounce();
    if (blocks_.count > 1) sections_.release();
    util::rewind(blocks_, *executor_);

    strings_merge_run* const runs = carve_<strings_merge_run>(runs_count);
    st::key128_t* const heads = carve_<st::key128_t>(records);
    std::uint32_t* const offsets = carve_<std::uint32_t>(records + runs_count);
    byte_t* const keys = carve_<byte_t>(keys_bytes);
    std::uint64_t* const saved = carve_<std::uint64_t>(saved_count);
    std::uint64_t* const pinned = carve_<std::uint64_t>(static_cast<std::size_t>(stamps.pinned_count));
    strings_version_t* const merged = carve_<strings_version_t>(records);
    std::uint8_t* const flags = carve_<std::uint8_t>(records);
    strings_version_t* const packed = carve_<strings_version_t>(records);
    std::uint64_t* const chain = carve_<std::uint64_t>(1);
    std::uint64_t* const range_stamps = carve_<std::uint64_t>(ranges_count);
    std::uint32_t* const range_offsets = carve_<std::uint32_t>(ranges_count ? 2 * ranges_count + runs_count : 0);
    byte_t* const range_tape = carve_<byte_t>(ranges_bytes);
    if ((runs_count && (!runs || !offsets)) || (records && (!heads || !merged || !flags || !packed)) || !chain ||
        (keys_bytes && !keys) || (saved_count && !saved) || (stamps.pinned_count && !pinned) ||
        (ranges_count && (!range_stamps || !range_offsets)) || (ranges_bytes && !range_tape))
        return status_t::out_of_memory_heap_k;

    strings_merge_run* const host_runs = carve_bounce<strings_merge_run>(*executor_, runs_count);
    st::key128_t* const host_heads = carve_bounce<st::key128_t>(*executor_, records);
    std::uint64_t* const host_saved = carve_bounce<std::uint64_t>(*executor_, saved_count);
    std::uint64_t* const host_count = carve_bounce<std::uint64_t>(*executor_, 1);
    std::uint32_t* const host_range_offsets = carve_bounce<std::uint32_t>(
        *executor_, ranges_count ? 2 * ranges_count + runs_count : 0);
    byte_t* const host_range_tape = carve_bounce<byte_t>(*executor_, ranges_bytes);
    if ((runs_count && !host_runs) || (records && !host_heads) || !host_count || (saved_count && !host_saved) ||
        (ranges_count && !host_range_offsets) || (ranges_bytes && !host_range_tape) || executor_->pin_bounce().failed())
        return status_t::out_of_memory_heap_k;

    // The heads leave the trees as one flat column, so adjacent threads load adjacent heads.
    status_t status = status_t::success_k;
    std::size_t first = 0, offsets_at = 0, keys_at = 0, saved_at = 0;
    std::size_t ranges_before = 0, range_offsets_before = 0, range_tape_before = 0;
    for (std::size_t run = 0; run != runs_count; ++run) {
        strings_run_view const& view = views[run];
        std::size_t const count = view.heads->size();
        for (std::size_t position = 0; position != count; ++position)
            host_heads[first + position] = view.heads->select(position);
        strings_merge_run& described = host_runs[run];
        described = strings_merge_run {heads + first, offsets + offsets_at, keys + keys_at, nullptr, nullptr, first,
                                       count};
        std::size_t const stamps_bytes = view.stamps->saved_bytes();
        if (view.stamps->save(bytes_of(host_saved + saved_at, stamps_bytes / sizeof(std::uint64_t))) !=
            status_t::success_k)
            return status_t::invalid_argument_k;
        described.saved_stamps = saved + saved_at;
        saved_at += stamps_bytes / sizeof(std::uint64_t);
        std::size_t const kinds_bytes = view.kinds->saved_bytes();
        if (view.kinds->save(bytes_of(host_saved + saved_at, kinds_bytes / sizeof(std::uint64_t))) !=
            status_t::success_k)
            return status_t::invalid_argument_k;
        described.saved_kinds = saved + saved_at;
        saved_at += kinds_bytes / sizeof(std::uint64_t);
        if ((status = upload(*executor_, offsets + offsets_at, view.key_offsets.data(), count + 1)) !=
                status_t::success_k ||
            (status = upload(*executor_, keys + keys_at, view.keys.data(), view.keys.size())) != status_t::success_k)
            return status;
        first += count, offsets_at += count + 1, keys_at += view.keys.size();
        // The bounds of a run's range tombstones leave as a tape, start then end, as on disk
        if (view.range_tombstones == nullptr || view.range_tombstones->empty()) continue;
        std::size_t const ranges = view.range_tombstones->size();
        described.range_stamps = range_stamps + ranges_before;
        described.range_offsets = range_offsets + range_offsets_before;
        described.range_tape = range_tape + range_tape_before;
        described.ranges_count = ranges;
        std::uint32_t* const bounds = host_range_offsets + range_offsets_before;
        std::uint32_t written = 0;
        for (std::size_t range = 0; range != ranges; ++range) {
            range_tombstone<std::string_view> const& bounded = view.range_tombstones->range(range);
            byte_t* const tape = host_range_tape + range_tape_before + written;
            bounds[2 * range] = written;
            bounds[2 * range + 1] = written + static_cast<std::uint32_t>(bounded.start.size());
            if (!bounded.start.empty()) std::memcpy(tape, bounded.start.data(), bounded.start.size());
            if (!bounded.end.empty()) std::memcpy(tape + bounded.start.size(), bounded.end.data(), bounded.end.size());
            written += static_cast<std::uint32_t>(bounded.start.size() + bounded.end.size());
        }
        bounds[2 * ranges] = written;
        status = upload(*executor_, range_stamps + ranges_before, view.range_tombstones->stamps().data(), ranges);
        if (status != status_t::success_k) return status;
        ranges_before += ranges, range_offsets_before += 2 * ranges + 1, range_tape_before += written;
    }
    if ((status = upload(*executor_, heads, host_heads, records)) != status_t::success_k ||
        (status = upload(*executor_, saved, host_saved, saved_count)) != status_t::success_k ||
        (status = upload(*executor_, runs, host_runs, runs_count)) != status_t::success_k ||
        (status = upload(*executor_, range_offsets, host_range_offsets, range_offsets_before)) != status_t::success_k ||
        (status = upload(*executor_, range_tape, host_range_tape, range_tape_before)) != status_t::success_k)
        return status;
    compaction_stamps_t device_stamps = stamps;
    if (stamps.pinned_count != 0) {
        status = upload(*executor_, pinned, stamps.pinned, static_cast<std::size_t>(stamps.pinned_count));
        if (status != status_t::success_k) return status;
        device_stamps.pinned = pinned;
    }

    // Every version lands at its merged position with its verdict, and only survivors return
    survivors.clear();
    if (records == 0) return executor_status(executor_->wait());
    status = launch_over(*executor_, merge_strings_rank_name_k, records, std::uint64_t {1} << 20,
                         static_cast<strings_merge_run const*>(runs), static_cast<std::uint32_t>(runs_count),
                         upper_runs, std::uint64_t {records}, kind, device_stamps, merged, flags);
    if (status != status_t::success_k) return status;
    status = executor_status(
        executor_->fill_async(reinterpret_cast<std::uintptr_t>(chain), byte_t {0}, sizeof(std::uint64_t)));
    if (status != status_t::success_k) return status;
    std::uint64_t const tickets = fu::min_of(
        divide_round_up(std::uint64_t {records}, std::uint64_t {merge_block_threads_k}), util::chain_blocks_max_k);
    status = launch_over(*executor_, merge_strings_pack_name_k, tickets * merge_block_threads_k, tickets,
                         static_cast<strings_version_t const*>(merged), static_cast<std::uint8_t const*>(flags),
                         std::uint64_t {records}, tickets, packed, chain);
    if (status != status_t::success_k) return status;
    if ((status = download(*executor_, host_count, chain, 1)) != status_t::success_k) return status;
    if ((status = executor_status(executor_->wait())) != status_t::success_k) return status;

    std::size_t const survivors_count = static_cast<std::size_t>(*host_count);
    if (survivors.resize(st::assume_uninitialized_t {}, survivors_count) != status_t::success_k)
        return status_t::out_of_memory_heap_k;
    status = download(*executor_, survivors.data(), packed, survivors_count);
    if (status != status_t::success_k) return status;
    return executor_status(executor_->wait());
}

cuda_compactor_t::~cuda_compactor_t() noexcept {
    if (!blocks_.count) return;
    sections_.release();
    if (executor_->bind().failed()) return;
    util::rewind(blocks_, *executor_);
    executor_->free_async(blocks_.blocks[0].address);
}

#pragma endregion NVIDIA

#pragma region AMD

expected<hip_compactor_t> hip_compactor_t::make(util::hip_executor_t& executor,
                                                device_sections_reader_t sections) noexcept {
    if (!executor.kernels()) return status_t {status_code_t::unsupported_k, "The device loaded no kernel bundle"};
    return hip_compactor_t(executor, sections);
}

st::status_t hip_compactor_t::merge_strings(compaction_kind_t kind, std::span<strings_run_view const> views,
                                            std::uint32_t upper_runs, compaction_stamps_t const& stamps,
                                            st::vector<strings_version_t>& survivors) noexcept {
    using st::status_t;
    if (!executor_->kernels() || views.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        upper_runs > views.size())
        return status_t::invalid_argument_k;
    std::size_t const runs_count = views.size();
    std::size_t records = 0, keys_bytes = 0, saved_count = 0, ranges_count = 0, ranges_bytes = 0;
    for (strings_run_view const& view : views) {
        if (view.heads->size() > (std::numeric_limits<std::uint32_t>::max)()) return status_t::invalid_argument_k;
        records += view.heads->size();
        keys_bytes += view.keys.size();
        saved_count += (view.stamps->saved_bytes() + view.kinds->saved_bytes()) / sizeof(std::uint64_t);
        if (view.range_tombstones == nullptr) continue;
        ranges_count += view.range_tombstones->size();
        for (range_tombstone<std::string_view> const& range : view.range_tombstones->ranges())
            ranges_bytes += range.start.size() + range.end.size();
    }
    if (ranges_bytes > (std::numeric_limits<std::uint32_t>::max)()) return status_t::invalid_argument_k;

    if (executor_->bind().failed()) return status_t::unknown_k;
    executor_->rewind_bounce();
    if (blocks_.count > 1) sections_.release();
    util::rewind(blocks_, *executor_);

    strings_merge_run* const runs = carve_<strings_merge_run>(runs_count);
    st::key128_t* const heads = carve_<st::key128_t>(records);
    std::uint32_t* const offsets = carve_<std::uint32_t>(records + runs_count);
    byte_t* const keys = carve_<byte_t>(keys_bytes);
    std::uint64_t* const saved = carve_<std::uint64_t>(saved_count);
    std::uint64_t* const pinned = carve_<std::uint64_t>(static_cast<std::size_t>(stamps.pinned_count));
    strings_version_t* const merged = carve_<strings_version_t>(records);
    std::uint8_t* const flags = carve_<std::uint8_t>(records);
    strings_version_t* const packed = carve_<strings_version_t>(records);
    std::uint64_t* const chain = carve_<std::uint64_t>(1);
    std::uint64_t* const range_stamps = carve_<std::uint64_t>(ranges_count);
    std::uint32_t* const range_offsets = carve_<std::uint32_t>(ranges_count ? 2 * ranges_count + runs_count : 0);
    byte_t* const range_tape = carve_<byte_t>(ranges_bytes);
    if ((runs_count && (!runs || !offsets)) || (records && (!heads || !merged || !flags || !packed)) || !chain ||
        (keys_bytes && !keys) || (saved_count && !saved) || (stamps.pinned_count && !pinned) ||
        (ranges_count && (!range_stamps || !range_offsets)) || (ranges_bytes && !range_tape))
        return status_t::out_of_memory_heap_k;

    strings_merge_run* const host_runs = carve_bounce<strings_merge_run>(*executor_, runs_count);
    st::key128_t* const host_heads = carve_bounce<st::key128_t>(*executor_, records);
    std::uint64_t* const host_saved = carve_bounce<std::uint64_t>(*executor_, saved_count);
    std::uint64_t* const host_count = carve_bounce<std::uint64_t>(*executor_, 1);
    std::uint32_t* const host_range_offsets = carve_bounce<std::uint32_t>(
        *executor_, ranges_count ? 2 * ranges_count + runs_count : 0);
    byte_t* const host_range_tape = carve_bounce<byte_t>(*executor_, ranges_bytes);
    if ((runs_count && !host_runs) || (records && !host_heads) || !host_count || (saved_count && !host_saved) ||
        (ranges_count && !host_range_offsets) || (ranges_bytes && !host_range_tape) || executor_->pin_bounce().failed())
        return status_t::out_of_memory_heap_k;

    // The heads leave the trees as one flat column, so adjacent threads load adjacent heads.
    status_t status = status_t::success_k;
    std::size_t first = 0, offsets_at = 0, keys_at = 0, saved_at = 0;
    std::size_t ranges_before = 0, range_offsets_before = 0, range_tape_before = 0;
    for (std::size_t run = 0; run != runs_count; ++run) {
        strings_run_view const& view = views[run];
        std::size_t const count = view.heads->size();
        for (std::size_t position = 0; position != count; ++position)
            host_heads[first + position] = view.heads->select(position);
        strings_merge_run& described = host_runs[run];
        described = strings_merge_run {heads + first, offsets + offsets_at, keys + keys_at, nullptr, nullptr, first,
                                       count};
        std::size_t const stamps_bytes = view.stamps->saved_bytes();
        if (view.stamps->save(bytes_of(host_saved + saved_at, stamps_bytes / sizeof(std::uint64_t))) !=
            status_t::success_k)
            return status_t::invalid_argument_k;
        described.saved_stamps = saved + saved_at;
        saved_at += stamps_bytes / sizeof(std::uint64_t);
        std::size_t const kinds_bytes = view.kinds->saved_bytes();
        if (view.kinds->save(bytes_of(host_saved + saved_at, kinds_bytes / sizeof(std::uint64_t))) !=
            status_t::success_k)
            return status_t::invalid_argument_k;
        described.saved_kinds = saved + saved_at;
        saved_at += kinds_bytes / sizeof(std::uint64_t);
        if ((status = upload(*executor_, offsets + offsets_at, view.key_offsets.data(), count + 1)) !=
                status_t::success_k ||
            (status = upload(*executor_, keys + keys_at, view.keys.data(), view.keys.size())) != status_t::success_k)
            return status;
        first += count, offsets_at += count + 1, keys_at += view.keys.size();
        // The bounds of a run's range tombstones leave as a tape, start then end, as on disk
        if (view.range_tombstones == nullptr || view.range_tombstones->empty()) continue;
        std::size_t const ranges = view.range_tombstones->size();
        described.range_stamps = range_stamps + ranges_before;
        described.range_offsets = range_offsets + range_offsets_before;
        described.range_tape = range_tape + range_tape_before;
        described.ranges_count = ranges;
        std::uint32_t* const bounds = host_range_offsets + range_offsets_before;
        std::uint32_t written = 0;
        for (std::size_t range = 0; range != ranges; ++range) {
            range_tombstone<std::string_view> const& bounded = view.range_tombstones->range(range);
            byte_t* const tape = host_range_tape + range_tape_before + written;
            bounds[2 * range] = written;
            bounds[2 * range + 1] = written + static_cast<std::uint32_t>(bounded.start.size());
            if (!bounded.start.empty()) std::memcpy(tape, bounded.start.data(), bounded.start.size());
            if (!bounded.end.empty()) std::memcpy(tape + bounded.start.size(), bounded.end.data(), bounded.end.size());
            written += static_cast<std::uint32_t>(bounded.start.size() + bounded.end.size());
        }
        bounds[2 * ranges] = written;
        status = upload(*executor_, range_stamps + ranges_before, view.range_tombstones->stamps().data(), ranges);
        if (status != status_t::success_k) return status;
        ranges_before += ranges, range_offsets_before += 2 * ranges + 1, range_tape_before += written;
    }
    if ((status = upload(*executor_, heads, host_heads, records)) != status_t::success_k ||
        (status = upload(*executor_, saved, host_saved, saved_count)) != status_t::success_k ||
        (status = upload(*executor_, runs, host_runs, runs_count)) != status_t::success_k ||
        (status = upload(*executor_, range_offsets, host_range_offsets, range_offsets_before)) != status_t::success_k ||
        (status = upload(*executor_, range_tape, host_range_tape, range_tape_before)) != status_t::success_k)
        return status;
    compaction_stamps_t device_stamps = stamps;
    if (stamps.pinned_count != 0) {
        status = upload(*executor_, pinned, stamps.pinned, static_cast<std::size_t>(stamps.pinned_count));
        if (status != status_t::success_k) return status;
        device_stamps.pinned = pinned;
    }

    // Every version lands at its merged position with its verdict, and only survivors return
    survivors.clear();
    if (records == 0) return executor_status(executor_->wait());
    status = launch_over(*executor_, merge_strings_rank_name_k, records, std::uint64_t {1} << 20,
                         static_cast<strings_merge_run const*>(runs), static_cast<std::uint32_t>(runs_count),
                         upper_runs, std::uint64_t {records}, kind, device_stamps, merged, flags);
    if (status != status_t::success_k) return status;
    status = executor_status(
        executor_->fill_async(reinterpret_cast<std::uintptr_t>(chain), byte_t {0}, sizeof(std::uint64_t)));
    if (status != status_t::success_k) return status;
    std::uint64_t const tickets = fu::min_of(
        divide_round_up(std::uint64_t {records}, std::uint64_t {merge_block_threads_k}), util::chain_blocks_max_k);
    status = launch_over(*executor_, merge_strings_pack_name_k, tickets * merge_block_threads_k, tickets,
                         static_cast<strings_version_t const*>(merged), static_cast<std::uint8_t const*>(flags),
                         std::uint64_t {records}, tickets, packed, chain);
    if (status != status_t::success_k) return status;
    if ((status = download(*executor_, host_count, chain, 1)) != status_t::success_k) return status;
    if ((status = executor_status(executor_->wait())) != status_t::success_k) return status;

    std::size_t const survivors_count = static_cast<std::size_t>(*host_count);
    if (survivors.resize(st::assume_uninitialized_t {}, survivors_count) != status_t::success_k)
        return status_t::out_of_memory_heap_k;
    status = download(*executor_, survivors.data(), packed, survivors_count);
    if (status != status_t::success_k) return status;
    return executor_status(executor_->wait());
}

hip_compactor_t::~hip_compactor_t() noexcept {
    if (!blocks_.count) return;
    sections_.release();
    if (executor_->bind().failed()) return;
    util::rewind(blocks_, *executor_);
    executor_->free_async(blocks_.blocks[0].address);
}

#pragma endregion AMD

#pragma region Apple

expected<metal_compactor_t> metal_compactor_t::make(util::metal_executor_t& executor,
                                                    device_sections_reader_t sections) noexcept {
    if (!executor.kernels()) return status_t {status_code_t::unsupported_k, "The device loaded no kernel bundle"};
    return metal_compactor_t(executor, sections);
}

st::status_t metal_compactor_t::merge_strings(compaction_kind_t kind, std::span<strings_run_view const> views,
                                              std::uint32_t upper_runs, compaction_stamps_t const& stamps,
                                              st::vector<strings_version_t>& survivors) noexcept {
    using st::status_t;
    if (!executor_->kernels() || views.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        upper_runs > views.size())
        return status_t::invalid_argument_k;
    std::size_t const runs_count = views.size();
    std::size_t records = 0, keys_bytes = 0, saved_count = 0, ranges_count = 0, ranges_bytes = 0;
    for (strings_run_view const& view : views) {
        if (view.heads->size() > (std::numeric_limits<std::uint32_t>::max)()) return status_t::invalid_argument_k;
        records += view.heads->size();
        keys_bytes += view.keys.size();
        saved_count += (view.stamps->saved_bytes() + view.kinds->saved_bytes()) / sizeof(std::uint64_t);
        if (view.range_tombstones == nullptr) continue;
        ranges_count += view.range_tombstones->size();
        for (range_tombstone<std::string_view> const& range : view.range_tombstones->ranges())
            ranges_bytes += range.start.size() + range.end.size();
    }
    // The select lists positions in 32 bits, so a merge of more versions runs on the host
    if (ranges_bytes > (std::numeric_limits<std::uint32_t>::max)() ||
        records > (std::numeric_limits<std::uint32_t>::max)())
        return status_t::invalid_argument_k;

    if (executor_->bind().failed()) return status_t::unknown_k;
    executor_->rewind_bounce();
    if (blocks_.count > 1) sections_.release();
    util::rewind(blocks_, *executor_);

    strings_merge_run* const runs = carve_<strings_merge_run>(runs_count);
    st::key128_t* const heads = carve_<st::key128_t>(records);
    std::uint32_t* const offsets = carve_<std::uint32_t>(records + runs_count);
    byte_t* const keys = carve_<byte_t>(keys_bytes);
    std::uint64_t* const saved = carve_<std::uint64_t>(saved_count);
    std::uint64_t* const pinned = carve_<std::uint64_t>(static_cast<std::size_t>(stamps.pinned_count));
    strings_version_t* const merged = carve_<strings_version_t>(records);
    std::uint8_t* const flags = carve_<std::uint8_t>(records);
    std::uint32_t* const selected = carve_<std::uint32_t>(records);
    std::uint64_t* const range_stamps = carve_<std::uint64_t>(ranges_count);
    std::uint32_t* const range_offsets = carve_<std::uint32_t>(ranges_count ? 2 * ranges_count + runs_count : 0);
    byte_t* const range_tape = carve_<byte_t>(ranges_bytes);
    if ((runs_count && (!runs || !offsets)) || (records && (!heads || !merged || !flags || !selected)) ||
        (keys_bytes && !keys) || (saved_count && !saved) || (stamps.pinned_count && !pinned) ||
        (ranges_count && (!range_stamps || !range_offsets)) || (ranges_bytes && !range_tape))
        return status_t::out_of_memory_heap_k;

    strings_merge_run* const host_runs = carve_bounce<strings_merge_run>(*executor_, runs_count);
    st::key128_t* const host_heads = carve_bounce<st::key128_t>(*executor_, records);
    std::uint64_t* const host_saved = carve_bounce<std::uint64_t>(*executor_, saved_count);
    strings_version_t* const host_merged = carve_bounce<strings_version_t>(*executor_, records);
    std::uint32_t* const host_selected = carve_bounce<std::uint32_t>(*executor_, records);
    std::uint64_t* const host_count = carve_bounce<std::uint64_t>(*executor_, 1);
    std::uint32_t* const host_range_offsets = carve_bounce<std::uint32_t>(
        *executor_, ranges_count ? 2 * ranges_count + runs_count : 0);
    byte_t* const host_range_tape = carve_bounce<byte_t>(*executor_, ranges_bytes);
    if ((runs_count && !host_runs) || (records && (!host_heads || !host_merged || !host_selected)) || !host_count ||
        (saved_count && !host_saved) || (ranges_count && !host_range_offsets) || (ranges_bytes && !host_range_tape) ||
        executor_->pin_bounce().failed())
        return status_t::out_of_memory_heap_k;

    // The heads leave the trees as one flat column, so adjacent threads load adjacent heads.
    status_t status = status_t::success_k;
    std::size_t first = 0, offsets_at = 0, keys_at = 0, saved_at = 0;
    std::size_t ranges_before = 0, range_offsets_before = 0, range_tape_before = 0;
    for (std::size_t run = 0; run != runs_count; ++run) {
        strings_run_view const& view = views[run];
        std::size_t const count = view.heads->size();
        for (std::size_t position = 0; position != count; ++position)
            host_heads[first + position] = view.heads->select(position);
        strings_merge_run& described = host_runs[run];
        described = strings_merge_run {heads + first, offsets + offsets_at, keys + keys_at, nullptr, nullptr, first,
                                       count};
        std::size_t const stamps_bytes = view.stamps->saved_bytes();
        if (view.stamps->save(bytes_of(host_saved + saved_at, stamps_bytes / sizeof(std::uint64_t))) !=
            status_t::success_k)
            return status_t::invalid_argument_k;
        described.saved_stamps = saved + saved_at;
        saved_at += stamps_bytes / sizeof(std::uint64_t);
        std::size_t const kinds_bytes = view.kinds->saved_bytes();
        if (view.kinds->save(bytes_of(host_saved + saved_at, kinds_bytes / sizeof(std::uint64_t))) !=
            status_t::success_k)
            return status_t::invalid_argument_k;
        described.saved_kinds = saved + saved_at;
        saved_at += kinds_bytes / sizeof(std::uint64_t);
        if ((status = upload(*executor_, offsets + offsets_at, view.key_offsets.data(), count + 1)) !=
                status_t::success_k ||
            (status = upload(*executor_, keys + keys_at, view.keys.data(), view.keys.size())) != status_t::success_k)
            return status;
        first += count, offsets_at += count + 1, keys_at += view.keys.size();
        // The bounds of a run's range tombstones leave as a tape, start then end, as on disk
        if (view.range_tombstones == nullptr || view.range_tombstones->empty()) continue;
        std::size_t const ranges = view.range_tombstones->size();
        described.range_stamps = range_stamps + ranges_before;
        described.range_offsets = range_offsets + range_offsets_before;
        described.range_tape = range_tape + range_tape_before;
        described.ranges_count = ranges;
        std::uint32_t* const bounds = host_range_offsets + range_offsets_before;
        std::uint32_t written = 0;
        for (std::size_t range = 0; range != ranges; ++range) {
            range_tombstone<std::string_view> const& bounded = view.range_tombstones->range(range);
            byte_t* const tape = host_range_tape + range_tape_before + written;
            bounds[2 * range] = written;
            bounds[2 * range + 1] = written + static_cast<std::uint32_t>(bounded.start.size());
            if (!bounded.start.empty()) std::memcpy(tape, bounded.start.data(), bounded.start.size());
            if (!bounded.end.empty()) std::memcpy(tape + bounded.start.size(), bounded.end.data(), bounded.end.size());
            written += static_cast<std::uint32_t>(bounded.start.size() + bounded.end.size());
        }
        bounds[2 * ranges] = written;
        status = upload(*executor_, range_stamps + ranges_before, view.range_tombstones->stamps().data(), ranges);
        if (status != status_t::success_k) return status;
        ranges_before += ranges, range_offsets_before += 2 * ranges + 1, range_tape_before += written;
    }
    if ((status = upload(*executor_, heads, host_heads, records)) != status_t::success_k ||
        (status = upload(*executor_, saved, host_saved, saved_count)) != status_t::success_k ||
        (status = upload(*executor_, runs, host_runs, runs_count)) != status_t::success_k ||
        (status = upload(*executor_, range_offsets, host_range_offsets, range_offsets_before)) != status_t::success_k ||
        (status = upload(*executor_, range_tape, host_range_tape, range_tape_before)) != status_t::success_k)
        return status;
    compaction_stamps_t device_stamps = stamps;
    if (stamps.pinned_count != 0) {
        status = upload(*executor_, pinned, stamps.pinned, static_cast<std::size_t>(stamps.pinned_count));
        if (status != status_t::success_k) return status;
        device_stamps.pinned = pinned;
    }

    // Every version lands at its merged position with its verdict, then the select lists where the
    // survivors landed, as an Apple device has no 64-bit atomics to chain a pack with
    survivors.clear();
    if (records == 0) return executor_status(executor_->wait());
    status = launch_over(*executor_, merge_strings_rank_name_k, records, std::uint64_t {1} << 20,
                         static_cast<strings_merge_run const*>(runs), static_cast<std::uint32_t>(runs_count),
                         upper_runs, std::uint64_t {records}, kind, device_stamps, merged, flags);
    if (status != status_t::success_k) return status;
    std::uint64_t const tile = round_up_to_multiple(divide_round_up(std::uint64_t {records}, util::chain_blocks_max_k),
                                                    std::uint64_t {256});
    std::uint32_t const tiles = static_cast<std::uint32_t>(divide_round_up(std::uint64_t {records}, tile));
    std::uint64_t* const counts = carve_<std::uint64_t>(std::size_t {tiles} + 1);
    if (!counts) return status_t::out_of_memory_heap_k;
    status = executor_status(util::select_flagged(*executor_, flags, static_cast<std::uint32_t>(records),
                                                  static_cast<std::uint32_t>(tile), tiles, counts, selected));
    if (status != status_t::success_k) return status;
    if ((status = download(*executor_, host_count, counts + tiles, 1)) != status_t::success_k ||
        (status = download(*executor_, host_merged, merged, records)) != status_t::success_k)
        return status;
    if ((status = executor_status(executor_->wait())) != status_t::success_k) return status;

    std::size_t const survivors_count = static_cast<std::size_t>(*host_count);
    status = download(*executor_, host_selected, selected, survivors_count);
    if (status != status_t::success_k) return status;
    if ((status = executor_status(executor_->wait())) != status_t::success_k) return status;
    if (survivors.resize(st::assume_uninitialized_t {}, survivors_count) != status_t::success_k)
        return status_t::out_of_memory_heap_k;
    for (std::size_t index = 0; index != survivors_count; ++index) survivors[index] = host_merged[host_selected[index]];
    return status_t::success_k;
}

metal_compactor_t::~metal_compactor_t() noexcept {
    if (!blocks_.count) return;
    sections_.release();
    if (executor_->bind().failed()) return;
    util::rewind(blocks_, *executor_);
    executor_->free_async(blocks_.blocks[0].address);
}

#pragma endregion Apple

#pragma region Instantiations

/** The engine's and the suites' key slots on an NVIDIA device. */
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<inline_12_bytes_slot_t>,
                                                views_of<inline_12_bytes_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                fold_ref_t const&, no_device_fold_t const&,
                                                located_value_reader<inline_12_bytes_slot_t>,
                                                compaction_output<inline_12_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<inline_44_bytes_slot_t>,
                                                views_of<inline_44_bytes_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                fold_ref_t const&, no_device_fold_t const&,
                                                located_value_reader<inline_44_bytes_slot_t>,
                                                compaction_output<inline_44_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<fixed_width_slot_t>,
                                                views_of<fixed_width_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                fold_ref_t const&, no_device_fold_t const&,
                                                located_value_reader<fixed_width_slot_t>,
                                                compaction_output<fixed_width_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<variable_width_slot_t>,
                                                views_of<variable_width_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                fold_ref_t const&, no_device_fold_t const&,
                                                located_value_reader<variable_width_slot_t>,
                                                compaction_output<variable_width_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_inline_12_bytes_slot_t>,
                                                views_of<suite_inline_12_bytes_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                fold_ref_t const&, no_device_fold_t const&,
                                                located_value_reader<suite_inline_12_bytes_slot_t>,
                                                compaction_output<suite_inline_12_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_inline_44_bytes_slot_t>,
                                                views_of<suite_inline_44_bytes_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                fold_ref_t const&, no_device_fold_t const&,
                                                located_value_reader<suite_inline_44_bytes_slot_t>,
                                                compaction_output<suite_inline_44_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_fixed_width_slot_t>,
                                                views_of<suite_fixed_width_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                fold_ref_t const&, no_device_fold_t const&,
                                                located_value_reader<suite_fixed_width_slot_t>,
                                                compaction_output<suite_fixed_width_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_variable_width_slot_t>,
                                                views_of<suite_variable_width_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                fold_ref_t const&, no_device_fold_t const&,
                                                located_value_reader<suite_variable_width_slot_t>,
                                                compaction_output<suite_variable_width_slot_t>&);

/** The graph store's key slots on an NVIDIA device, whose changes its device fold composes. */
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<variable_width_slot_t>,
                                                views_of<variable_width_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                graph::adjacency_fold_t const&, graph::vram_adjacency_fold_t const&,
                                                located_value_reader<variable_width_slot_t>,
                                                compaction_output<variable_width_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_variable_width_slot_t>,
                                                views_of<suite_variable_width_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                graph::adjacency_fold_t const&, graph::vram_adjacency_fold_t const&,
                                                located_value_reader<suite_variable_width_slot_t>,
                                                compaction_output<suite_variable_width_slot_t>&);

/** The engine's and the suites' key slots on an AMD device. */
template st::status_t hip_compactor_t::compact(compaction_kind_t, views_of<inline_12_bytes_slot_t>,
                                               views_of<inline_12_bytes_slot_t>, compaction_extents_t,
                                               sorted_run_bounds_t const&, compaction_stamps_t const&,
                                               fold_ref_t const&, no_device_fold_t const&,
                                               located_value_reader<inline_12_bytes_slot_t>,
                                               compaction_output<inline_12_bytes_slot_t>&);
template st::status_t hip_compactor_t::compact(compaction_kind_t, views_of<inline_44_bytes_slot_t>,
                                               views_of<inline_44_bytes_slot_t>, compaction_extents_t,
                                               sorted_run_bounds_t const&, compaction_stamps_t const&,
                                               fold_ref_t const&, no_device_fold_t const&,
                                               located_value_reader<inline_44_bytes_slot_t>,
                                               compaction_output<inline_44_bytes_slot_t>&);
template st::status_t hip_compactor_t::compact(compaction_kind_t, views_of<fixed_width_slot_t>,
                                               views_of<fixed_width_slot_t>, compaction_extents_t,
                                               sorted_run_bounds_t const&, compaction_stamps_t const&,
                                               fold_ref_t const&, no_device_fold_t const&,
                                               located_value_reader<fixed_width_slot_t>,
                                               compaction_output<fixed_width_slot_t>&);
template st::status_t hip_compactor_t::compact(compaction_kind_t, views_of<variable_width_slot_t>,
                                               views_of<variable_width_slot_t>, compaction_extents_t,
                                               sorted_run_bounds_t const&, compaction_stamps_t const&,
                                               fold_ref_t const&, no_device_fold_t const&,
                                               located_value_reader<variable_width_slot_t>,
                                               compaction_output<variable_width_slot_t>&);
template st::status_t hip_compactor_t::compact(compaction_kind_t, views_of<suite_inline_12_bytes_slot_t>,
                                               views_of<suite_inline_12_bytes_slot_t>, compaction_extents_t,
                                               sorted_run_bounds_t const&, compaction_stamps_t const&,
                                               fold_ref_t const&, no_device_fold_t const&,
                                               located_value_reader<suite_inline_12_bytes_slot_t>,
                                               compaction_output<suite_inline_12_bytes_slot_t>&);
template st::status_t hip_compactor_t::compact(compaction_kind_t, views_of<suite_inline_44_bytes_slot_t>,
                                               views_of<suite_inline_44_bytes_slot_t>, compaction_extents_t,
                                               sorted_run_bounds_t const&, compaction_stamps_t const&,
                                               fold_ref_t const&, no_device_fold_t const&,
                                               located_value_reader<suite_inline_44_bytes_slot_t>,
                                               compaction_output<suite_inline_44_bytes_slot_t>&);
template st::status_t hip_compactor_t::compact(compaction_kind_t, views_of<suite_fixed_width_slot_t>,
                                               views_of<suite_fixed_width_slot_t>, compaction_extents_t,
                                               sorted_run_bounds_t const&, compaction_stamps_t const&,
                                               fold_ref_t const&, no_device_fold_t const&,
                                               located_value_reader<suite_fixed_width_slot_t>,
                                               compaction_output<suite_fixed_width_slot_t>&);
template st::status_t hip_compactor_t::compact(compaction_kind_t, views_of<suite_variable_width_slot_t>,
                                               views_of<suite_variable_width_slot_t>, compaction_extents_t,
                                               sorted_run_bounds_t const&, compaction_stamps_t const&,
                                               fold_ref_t const&, no_device_fold_t const&,
                                               located_value_reader<suite_variable_width_slot_t>,
                                               compaction_output<suite_variable_width_slot_t>&);

/** The graph store's key slots on an AMD device, whose changes its device fold composes. */
template st::status_t hip_compactor_t::compact(compaction_kind_t, views_of<variable_width_slot_t>,
                                               views_of<variable_width_slot_t>, compaction_extents_t,
                                               sorted_run_bounds_t const&, compaction_stamps_t const&,
                                               graph::adjacency_fold_t const&, graph::vram_adjacency_fold_t const&,
                                               located_value_reader<variable_width_slot_t>,
                                               compaction_output<variable_width_slot_t>&);
template st::status_t hip_compactor_t::compact(compaction_kind_t, views_of<suite_variable_width_slot_t>,
                                               views_of<suite_variable_width_slot_t>, compaction_extents_t,
                                               sorted_run_bounds_t const&, compaction_stamps_t const&,
                                               graph::adjacency_fold_t const&, graph::vram_adjacency_fold_t const&,
                                               located_value_reader<suite_variable_width_slot_t>,
                                               compaction_output<suite_variable_width_slot_t>&);

/** The engine's and the suites' key slots on an Apple device. */
template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<inline_12_bytes_slot_t>,
                                                 views_of<inline_12_bytes_slot_t>, compaction_extents_t,
                                                 sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                 fold_ref_t const&, no_device_fold_t const&,
                                                 located_value_reader<inline_12_bytes_slot_t>,
                                                 compaction_output<inline_12_bytes_slot_t>&);
template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<inline_44_bytes_slot_t>,
                                                 views_of<inline_44_bytes_slot_t>, compaction_extents_t,
                                                 sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                 fold_ref_t const&, no_device_fold_t const&,
                                                 located_value_reader<inline_44_bytes_slot_t>,
                                                 compaction_output<inline_44_bytes_slot_t>&);
template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<fixed_width_slot_t>,
                                                 views_of<fixed_width_slot_t>, compaction_extents_t,
                                                 sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                 fold_ref_t const&, no_device_fold_t const&,
                                                 located_value_reader<fixed_width_slot_t>,
                                                 compaction_output<fixed_width_slot_t>&);
template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<variable_width_slot_t>,
                                                 views_of<variable_width_slot_t>, compaction_extents_t,
                                                 sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                 fold_ref_t const&, no_device_fold_t const&,
                                                 located_value_reader<variable_width_slot_t>,
                                                 compaction_output<variable_width_slot_t>&);
template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<suite_inline_12_bytes_slot_t>,
                                                 views_of<suite_inline_12_bytes_slot_t>, compaction_extents_t,
                                                 sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                 fold_ref_t const&, no_device_fold_t const&,
                                                 located_value_reader<suite_inline_12_bytes_slot_t>,
                                                 compaction_output<suite_inline_12_bytes_slot_t>&);
template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<suite_inline_44_bytes_slot_t>,
                                                 views_of<suite_inline_44_bytes_slot_t>, compaction_extents_t,
                                                 sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                 fold_ref_t const&, no_device_fold_t const&,
                                                 located_value_reader<suite_inline_44_bytes_slot_t>,
                                                 compaction_output<suite_inline_44_bytes_slot_t>&);
template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<suite_fixed_width_slot_t>,
                                                 views_of<suite_fixed_width_slot_t>, compaction_extents_t,
                                                 sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                 fold_ref_t const&, no_device_fold_t const&,
                                                 located_value_reader<suite_fixed_width_slot_t>,
                                                 compaction_output<suite_fixed_width_slot_t>&);
template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<suite_variable_width_slot_t>,
                                                 views_of<suite_variable_width_slot_t>, compaction_extents_t,
                                                 sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                 fold_ref_t const&, no_device_fold_t const&,
                                                 located_value_reader<suite_variable_width_slot_t>,
                                                 compaction_output<suite_variable_width_slot_t>&);

/** The graph store's key slots on an Apple device, whose changes its device fold composes. */
template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<variable_width_slot_t>,
                                                 views_of<variable_width_slot_t>, compaction_extents_t,
                                                 sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                 graph::adjacency_fold_t const&, graph::vram_adjacency_fold_t const&,
                                                 located_value_reader<variable_width_slot_t>,
                                                 compaction_output<variable_width_slot_t>&);
template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<suite_variable_width_slot_t>,
                                                 views_of<suite_variable_width_slot_t>, compaction_extents_t,
                                                 sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                 graph::adjacency_fold_t const&, graph::vram_adjacency_fold_t const&,
                                                 located_value_reader<suite_variable_width_slot_t>,
                                                 compaction_output<suite_variable_width_slot_t>&);

#pragma endregion Instantiations

} // namespace unum::ustore::blobs
