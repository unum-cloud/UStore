/**
 *  @file src/nvme/compaction.cpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The compiled part of GPU compaction: binding each vendor's compactor to its executor
 *      whose device loaded the library's kernels, freeing its device runs, and compacting each of
 *      the key slots the engine stores.
 *
 *  The NVIDIA and AMD compactors queue their merges from @c vram_nvidia_compaction.cuh and
 *  @c vram_amd_compaction.hip, and the Apple one from here. The graph's device fold for the graph's
 *  key slot is instantiated here as well, so the host TUs of the engine call it.
 */

#include "ustore/blobs/vram_amd_compaction.hip"        // `hip_compactor_t`
#include "ustore/blobs/vram_apple_compaction.hpp"      // `metal_compactor_t`
#include "ustore/blobs/vram_nvidia_compaction.cuh"     // `cuda_compactor_t`, `fold_survivors`, `launch_over`
#include "ustore/graph/vram_nvidia_adjacency_fold.cuh" // the fold's device definitions
#include "ustore/utilities/vram_scan.cuh"              // `select_flagged`

namespace unum::ustore::blobs {

#pragma region NVIDIA

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
    if (!executor_->kernels() || views.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        upper_runs > views.size())
        return status_t::invalid_argument_k;
    std::size_t const runs_count = views.size();
    std::size_t records = 0, keys_bytes = 0, saved_count = 0;
    for (strings_run_view const& view : views) {
        if (view.heads->size() > (std::numeric_limits<std::uint32_t>::max)()) return status_t::invalid_argument_k;
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
        if (view.stamps->save({reinterpret_cast<byte_t*>(host_saved + saved_at), stamps_bytes}) != status_t::success_k)
            return status_t::invalid_argument_k;
        described.saved_stamps = saved + saved_at;
        saved_at += stamps_bytes / sizeof(std::uint64_t);
        std::size_t const kinds_bytes = view.kinds->saved_bytes();
        if (view.kinds->save({reinterpret_cast<byte_t*>(host_saved + saved_at), kinds_bytes}) != status_t::success_k)
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

#pragma endregion NVIDIA

#pragma region AMD

expected<hip_compactor_t> hip_compactor_t::make(utilities::hip_executor_t& executor,
                                                device_sections_reader_t sections) noexcept {
    if (!executor.kernels())
        return status_t {status_code_t::unsupported_k, "The device loaded no bundle of the library's kernels"};
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
    std::size_t records = 0, keys_bytes = 0, saved_count = 0;
    for (strings_run_view const& view : views) {
        if (view.heads->size() > (std::numeric_limits<std::uint32_t>::max)()) return status_t::invalid_argument_k;
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
        if (view.stamps->save({reinterpret_cast<byte_t*>(host_saved + saved_at), stamps_bytes}) != status_t::success_k)
            return status_t::invalid_argument_k;
        described.saved_stamps = saved + saved_at;
        saved_at += stamps_bytes / sizeof(std::uint64_t);
        std::size_t const kinds_bytes = view.kinds->saved_bytes();
        if (view.kinds->save({reinterpret_cast<byte_t*>(host_saved + saved_at), kinds_bytes}) != status_t::success_k)
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

hip_compactor_t::~hip_compactor_t() noexcept {
    if (!blocks_.count) return;
    sections_.release();
    if (executor_->bind().failed()) return;
    utilities::rewind(blocks_, *executor_);
    executor_->free_async(blocks_.blocks[0].address);
}

#pragma endregion AMD

#pragma region Apple

expected<metal_compactor_t> metal_compactor_t::make(utilities::metal_executor_t& executor,
                                                    device_sections_reader_t sections) noexcept {
    if (!executor.kernels())
        return status_t {status_code_t::unsupported_k, "The device loaded no bundle of the library's kernels"};
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
    std::size_t records = 0, keys_bytes = 0, saved_count = 0;
    for (strings_run_view const& view : views) {
        if (view.heads->size() > (std::numeric_limits<std::uint32_t>::max)()) return status_t::invalid_argument_k;
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
        if (view.stamps->save({reinterpret_cast<byte_t*>(host_saved + saved_at), stamps_bytes}) != status_t::success_k)
            return status_t::invalid_argument_k;
        described.saved_stamps = saved + saved_at;
        saved_at += stamps_bytes / sizeof(std::uint64_t);
        std::size_t const kinds_bytes = view.kinds->saved_bytes();
        if (view.kinds->save({reinterpret_cast<byte_t*>(host_saved + saved_at), kinds_bytes}) != status_t::success_k)
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

metal_compactor_t::~metal_compactor_t() noexcept {
    if (!blocks_.count) return;
    sections_.release();
    if (executor_->bind().failed()) return;
    utilities::rewind(blocks_, *executor_);
    executor_->free_async(blocks_.blocks[0].address);
}

template <typename element_type_>
element_type_* metal_compactor_t::carve_(std::size_t count) noexcept {
    std::uint32_t const held = blocks_.count;
    expected<std::span<element_type_>> const carved = utilities::carve_runs<element_type_>(
        blocks_, *executor_, count, [&](std::size_t bytes) noexcept { return executor_->allocate_async(bytes); });
    device_allocations_ += blocks_.count - held;
    return carved ? carved->data() : nullptr;
}

template <typename record_type_>
st::status_t metal_compactor_t::pack_(char const* name, merge_pack_regions<record_type_> const& regions,
                                      std::uint64_t*& counted) noexcept {
    using st::status_t;
    // The select counts and lists in 32-bit words, so a larger merge runs on the host
    if (regions.records_count > (std::numeric_limits<std::uint32_t>::max)()) return status_t::invalid_argument_k;
    std::uint32_t const records_count = static_cast<std::uint32_t>(regions.records_count);
    // Few enough tiles that each sums the bytes of the ones before it, as many as a chain's blocks
    std::uint32_t const tile = static_cast<std::uint32_t>(round_up_to_multiple(
        divide_round_up(std::uint64_t {records_count}, utilities::chain_blocks_max_k), std::uint64_t {256}));
    std::uint32_t const tiles = static_cast<std::uint32_t>(divide_round_up(std::uint64_t {records_count}, tile));
    std::uint32_t* const counts = carve_<std::uint32_t>(std::size_t {tiles} + 1);
    std::uint32_t* const selected = carve_<std::uint32_t>(records_count);
    std::uint64_t* const tile_bytes = carve_<std::uint64_t>(tiles);
    std::uint64_t* const survivors_count = carve_<std::uint64_t>(1);
    if (!counts || !selected || !tile_bytes || !survivors_count) return status_t::out_of_memory_heap_k;
    counted = survivors_count;
    status_t status = executor_status(
        utilities::select_flagged(*executor_, regions.flags, records_count, tile, tiles, counts, selected));
    if (status != status_t::success_k) return status;
    status = executor_status(
        utilities::launch(*executor_, executor_->kernels()->function(name), tiles, merge_block_threads_k, 0u,
                          regions.merged, regions.flags, regions.replaced, regions.replacements, regions.replaced_count,
                          static_cast<std::uint32_t const*>(selected), static_cast<std::uint32_t const*>(counts), tile,
                          tiles, regions.survivors, regions.prefix, regions.shares, tile_bytes, survivors_count));
    // Value offsets scan within each tile, so every tile past the first adds the bytes before it
    if (status != status_t::success_k || regions.prefix == nullptr || tiles < 2) return status;
    return executor_status(utilities::launch(*executor_, executor_->kernels()->function("ustore_merge_offsets_apple"),
                                             tiles, merge_block_threads_k, 0u,
                                             static_cast<std::uint32_t const*>(counts), tile, tiles,
                                             static_cast<std::uint64_t const*>(tile_bytes), regions.prefix));
}

template <typename key_slot_type_, utilities::fold_concept fold_type_, typename device_fold_type_,
          typename allocator_type_>
    requires compactable_key_slot_concept<key_slot_type_>
st::status_t metal_compactor_t::compact(compaction_kind_t kind,
                                        std::span<sorted_run_index_view<key_slot_type_> const> upper,
                                        std::span<sorted_run_index_view<key_slot_type_> const> lower,
                                        std::span<sorted_run_extents_t const> extents,
                                        sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps,
                                        fold_type_ const& fold, device_fold_type_ const& device_fold,
                                        std::type_identity_t<located_value_reader<key_slot_type_>> read_value,
                                        compaction_output<key_slot_type_, allocator_type_>& output) noexcept {
    // A key slot the bundles hold no kernels for merges on the host.
    if constexpr (merge_kernel_names<key_slot_type_>::exported_k)
        return compact_on_device_(kind, upper, lower, extents, bounds, stamps, fold, device_fold, read_value, output);
    else return st::status_t::invalid_argument_k;
}

template <typename key_slot_type_, utilities::fold_concept fold_type_, typename device_fold_type_,
          typename allocator_type_>
    requires compactable_key_slot_concept<key_slot_type_>
st::status_t metal_compactor_t::compact_on_device_(
    compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_type_> const> upper,
    std::span<sorted_run_index_view<key_slot_type_> const> lower, std::span<sorted_run_extents_t const> extents,
    sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps, [[maybe_unused]] fold_type_ const& fold,
    [[maybe_unused]] device_fold_type_ const& device_fold,
    [[maybe_unused]] std::type_identity_t<located_value_reader<key_slot_type_>> read_value,
    compaction_output<key_slot_type_, allocator_type_>& output) noexcept {
    using st::status_t;
    using record_t = located_key_slot<key_slot_type_>;
    using view_t = sorted_run_index_view<key_slot_type_>;
    using names_t = merge_kernel_names<key_slot_type_>;
    constexpr bool device_folds_k = !std::is_same_v<device_fold_type_, no_device_fold_t>;
    constexpr bool locates_k = locates_stored_values_concept<key_slot_type_>;
    constexpr bool gathers_keys_k = !is_inline(key_slot_type_::value_layout_k);
    constexpr st::assume_uninitialized_t uninitialized {};

    // A device whose bundle did not load merges on the host.
    if (!executor_->kernels()) return status_t::invalid_argument_k;
    if (!admits_sorted_runs(bounds)) return status_t::invalid_argument_k;
    std::uint64_t const sorted_runs_count = upper.size() + lower.size();
    if (sorted_runs_count > (std::numeric_limits<std::uint32_t>::max)() || extents.size() != sorted_runs_count)
        return status_t::invalid_argument_k;
    if (!views_locate_values(upper, bounds) || !views_locate_values(lower, bounds)) return status_t::invalid_argument_k;
    auto const view_of = [&](std::uint64_t run) noexcept -> view_t const& {
        return run < upper.size() ? upper[run] : lower[run - upper.size()];
    };

    std::uint64_t merged_count = 0;
    std::size_t saved_count = 0;
    bool changes = false;
    sections_source_t const preferred = sections_ ? sections_source_t::volume_k : sections_source_t::memory_k;
    scratch_cursor_t sizing;
    for (std::uint64_t run = 0; run != sorted_runs_count; ++run) {
        view_t const& view = view_of(run);
        changes |= holds_changes(view.kinds);
        merged_count += view.key_slots_count;
        if (carve_sections(view, extents[run], preferred, sizing).source == sections_source_t::memory_k)
            saved_count += saved_words(view);
    }
    // Without a device fold, changes fold on the host alone, where every merge
    // holding one runs.
    if (!device_folds_k && changes) return status_t::invalid_argument_k;
    output.folded.clear();

    if (executor_->bind().failed()) return status_t::unknown_k;
    executor_->rewind_bounce();
    // The reader registered the newest run, which a rewind keeps, unless a merge opened more
    // after its sections
    if (blocks_.count > 1) sections_.release();
    utilities::rewind(blocks_, *executor_);

    // The sections land first, in one region, so the reader registers the run holding it and
    // reads into it
    std::size_t const page = static_cast<std::size_t>(utilities::portable_alignment_k);
    byte_t* const region = sizing.at != 0 ? carve_<byte_t>(static_cast<std::size_t>(sizing.at) + page) : nullptr;
    if (sizing.at != 0 && !region) return status_t::out_of_memory_heap_k;
    std::uintptr_t const sections_base = round_up_to_multiple<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(region),
                                                                              page);
    std::uintptr_t const holding_address = blocks_.count ? blocks_.blocks[blocks_.count - 1].address : 0;
    std::size_t const holding_bytes = blocks_.count ? blocks_.blocks[blocks_.count - 1].capacity : 0;

    std::size_t const records_count = static_cast<std::size_t>(merged_count);
    merge_run<key_slot_type_>* const runs = carve_<merge_run<key_slot_type_>>(sorted_runs_count);
    std::uint64_t* const saved_columns = carve_<std::uint64_t>(saved_count);
    std::uint64_t* const pinned = carve_<std::uint64_t>(static_cast<std::size_t>(stamps.pinned_count));
    record_t* const merged = carve_<record_t>(records_count);
    std::uint8_t* const flags = carve_<std::uint8_t>(records_count);
    record_t* const survivors = carve_<record_t>(records_count);
    std::uint64_t* const prefix = locates_k ? carve_<std::uint64_t>(records_count + 1) : nullptr;
    std::uint8_t* const shares = carve_<std::uint8_t>(records_count);
    key_rows_gather<key_slot_type_>* const gathers = carve_<key_rows_gather<key_slot_type_>>(sorted_runs_count);
    if ((sorted_runs_count && (!runs || !gathers)) || (saved_count && !saved_columns) ||
        (stamps.pinned_count && !pinned) ||
        (records_count && (!merged || !flags || !survivors || !shares || (locates_k && !prefix))))
        return status_t::out_of_memory_heap_k;

    merge_run<key_slot_type_>* const host_runs = carve_bounce<merge_run<key_slot_type_>>(*executor_, sorted_runs_count);
    std::uint64_t* const saved = carve_bounce<std::uint64_t>(*executor_, saved_count);
    key_rows_gather<key_slot_type_>* const host_gathers = carve_bounce<key_rows_gather<key_slot_type_>>(
        *executor_, sorted_runs_count);
    if ((sorted_runs_count && (!host_runs || !host_gathers)) || (saved_count && !saved) ||
        executor_->pin_bounce().failed())
        return status_t::out_of_memory_heap_k;
    std::uint32_t gathers_count = 0;
    std::uint64_t gathered_cells = 0;

    // Queues the reads of every non-empty section of a run on a volume, answering whether all fit.
    auto const queue_section_reads = [&](sorted_run_extents_t const& run_extents,
                                         run_sections<key_slot_type_> const& sections) noexcept {
        auto const into = [&](void const* target) noexcept {
            return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(target) - holding_address);
        };
        for (device_section_read_t const read :
             {device_section_read_t {run_extents.volume, run_extents.cells, into(sections.cells), run_extents.run},
              device_section_read_t {run_extents.volume, run_extents.value_offsets, into(sections.value_offsets),
                                     run_extents.run},
              device_section_read_t {run_extents.volume, run_extents.trailer, into(sections.trailer), run_extents.run},
              device_section_read_t {run_extents.volume, sections.keys ? run_extents.keys : utilities::extent_t {},
                                     sections.keys ? into(sections.keys) : 0, run_extents.run}})
            if (!read.extent.empty() && reads_.push_back(device_section_read_t {read}) != status_t::success_k)
                return false;
        return true;
    };
    // Every run's sections land where the sizing pass placed them, and its record reads them
    // through @c runs
    reads_.clear();
    scratch_cursor_t placing {sections_base};
    std::size_t saved_offset = 0;
    std::uint64_t first = 0;
    status_t status = status_t::success_k;
    for (std::uint64_t run = 0; run != sorted_runs_count; ++run) {
        view_t const& view = view_of(run);
        sorted_run_extents_t const& run_extents = extents[run];
        run_sections<key_slot_type_> const sections = carve_sections(view, run_extents, preferred, placing);
        merge_run<key_slot_type_>& described = host_runs[run];
        described = merge_run<key_slot_type_> {};
        described.cells = sections.cells, described.value_offsets = sections.value_offsets;
        described.assigned_stamp = run_extents.assigned_stamp, described.first = first;
        described.key_slots_count = view.key_slots_count;
        first += view.key_slots_count;
        if (sections.source == sections_source_t::volume_k) {
            if (!queue_section_reads(run_extents, sections)) return status_t::out_of_memory_heap_k;
            described.saved_stamps = sections.trailer;
            described.saved_kinds = sections.trailer + run_extents.kinds_begin / sizeof(std::uint64_t);
            if (sections.keys != nullptr) {
                host_gathers[gathers_count++] = key_rows_gather<key_slot_type_> {
                    sections.keys + run_extents.leaves_begin / sizeof(std::uint64_t), sections.cells, gathered_cells,
                    view.key_slots_count};
                gathered_cells += view.key_slots_count;
            }
            continue;
        }
        std::size_t const cells_count = static_cast<std::size_t>(view.key_slots_count);
        key_slot_type_ const* cells = view.cells;
        // A run keeping its keys in its tree alone stages its cells on the bounce to upload.
        if (cells == nullptr && cells_count != 0) {
            key_slot_type_* const staged = carve_bounce<key_slot_type_>(*executor_, cells_count);
            if (!staged) return status_t::out_of_memory_heap_k;
            for (std::size_t position = 0; position != cells_count; ++position)
                staged[position] = key_slot_of(view, position);
            cells = staged;
        }
        if ((status = upload(*executor_, sections.cells, cells, cells_count)) != status_t::success_k) return status;
        status = upload(*executor_, sections.value_offsets, view.value_offsets.data(), view.value_offsets.size());
        if (status != status_t::success_k) return status;
        if (view.key_slots_count == 0) continue;
        if (view.stamps != nullptr) {
            std::size_t const bytes = view.stamps->saved_bytes();
            if (view.stamps->save({reinterpret_cast<byte_t*>(saved + saved_offset), bytes}) != status_t::success_k)
                return status_t::invalid_argument_k;
            described.saved_stamps = saved_columns + saved_offset;
            saved_offset += bytes / sizeof(std::uint64_t);
        }
        if (view.kinds != nullptr) {
            std::size_t const bytes = view.kinds->saved_bytes();
            if (view.kinds->save({reinterpret_cast<byte_t*>(saved + saved_offset), bytes}) != status_t::success_k)
                return status_t::invalid_argument_k;
            described.saved_kinds = saved_columns + saved_offset;
            saved_offset += bytes / sizeof(std::uint64_t);
        }
    }
    if ((status = upload(*executor_, saved_columns, saved, saved_count)) != status_t::success_k) return status;
    if ((status = upload(*executor_, runs, host_runs, sorted_runs_count)) != status_t::success_k) return status;
    if (!reads_.empty()) {
        std::span<byte_t> const scratch {reinterpret_cast<byte_t*>(holding_address), holding_bytes};
        if ((status = sections_.read(reads_, scratch)) != status_t::success_k) return status;
    }
    // Runs keeping their keys in their trees alone gather their cells in one pass, then rank.
    if constexpr (gathers_keys_k)
        if (gathered_cells != 0) {
            if ((status = upload(*executor_, gathers, host_gathers, gathers_count)) != status_t::success_k)
                return status;
            status = launch_over(*executor_, names_t::gather_k, gathered_cells, std::uint64_t {1} << 20,
                                 static_cast<key_rows_gather<key_slot_type_> const*>(gathers), gathers_count,
                                 gathered_cells);
            if (status != status_t::success_k) return status;
        }

    // The pinned stamps are a few words, read by every survival check, so the device holds
    // its own copy.
    compaction_stamps_t device_stamps = stamps;
    if (stamps.pinned_count != 0) {
        status = upload(*executor_, pinned, stamps.pinned, static_cast<std::size_t>(stamps.pinned_count));
        if (status != status_t::success_k) return status;
        device_stamps.pinned = pinned;
    }

    // Every record lands at its merged position with its verdict, then the survivors pack, the
    // folded ones swapped
    std::uint64_t survivors_count = 0;
    std::uint64_t* prefixes = nullptr;
    std::uint8_t* host_shares = nullptr;
    if (merged_count != 0) {
        status = launch_over(*executor_, names_t::rank_k, merged_count, std::uint64_t {1} << 20,
                             static_cast<merge_run<key_slot_type_> const*>(runs),
                             static_cast<std::uint32_t>(sorted_runs_count), static_cast<std::uint32_t>(upper.size()),
                             bounds.value_stride, merged_count, kind, device_stamps, merged, flags);
        if (status != status_t::success_k) return status;
        std::span<std::uint64_t> indices;
        std::span<record_t> replacements;
        if constexpr (device_folds_k)
            if (changes)
                status = fold_survivors(*executor_, blocks_, kind, stamps, sorted_runs_count, bounds.value_stride,
                                        merged, flags, merged_count, fold, device_fold, read_value, output, indices,
                                        replacements);
        if (status != status_t::success_k) return status;
        std::uint64_t* const replaced = carve_<std::uint64_t>(indices.size());
        record_t* const replacing = carve_<record_t>(replacements.size());
        if (!indices.empty() && (!replaced || !replacing)) return status_t::out_of_memory_heap_k;
        if ((status = upload(*executor_, replaced, indices.data(), indices.size())) != status_t::success_k)
            return status;
        if ((status = upload(*executor_, replacing, replacements.data(), replacements.size())) != status_t::success_k)
            return status;
        std::uint64_t* counted = nullptr;
        status = pack_(names_t::pack_k,
                       merge_pack_regions<record_t> {merged, flags, merged_count, replaced, replacing,
                                                     std::uint64_t {indices.size()}, survivors, prefix, shares},
                       counted);
        if (status != status_t::success_k) return status;

        // What plans the new runs comes back at once, sized by the records, as only the
        // device counts survivors
        std::uint64_t* const selected = carve_bounce<std::uint64_t>(*executor_, 1);
        prefixes = locates_k ? carve_bounce<std::uint64_t>(*executor_, records_count + 1) : nullptr;
        host_shares = carve_bounce<std::uint8_t>(*executor_, records_count);
        if (!selected || !host_shares || (locates_k && !prefixes) || executor_->pin_bounce().failed())
            return status_t::out_of_memory_heap_k;
        if ((status = download(*executor_, selected, counted, 1)) != status_t::success_k) return status;
        if ((status = download(*executor_, prefixes, prefix, locates_k ? records_count + 1 : 0)) != status_t::success_k)
            return status;
        if ((status = download(*executor_, host_shares, shares, records_count)) != status_t::success_k) return status;
        if ((status = executor_status(executor_->wait())) != status_t::success_k) return status;
        survivors_count = *selected;
    }
    else if (locates_k) {
        prefixes = carve_bounce<std::uint64_t>(*executor_, 1);
        if (!prefixes) return status_t::out_of_memory_heap_k;
        prefixes[0] = 0;
    }

    auto const read_prefix = [&](std::uint64_t index, std::uint64_t& bytes) noexcept {
        bytes = locates_k ? prefixes[static_cast<std::size_t>(index)] : 0;
        return status_t::success_k;
    };
    auto const shares_key = [&](std::uint64_t index, bool& shared) noexcept {
        shared = host_shares[static_cast<std::size_t>(index)] != 0;
        return status_t::success_k;
    };
    status = plan_sorted_runs<key_slot_type_>(survivors_count, bounds, read_prefix, shares_key, output.sorted_runs);
    if (status != status_t::success_k) return status;

    std::uint64_t const cells_count = survivors_count;
    std::size_t const survivors_size = static_cast<std::size_t>(survivors_count);
    key_slot_type_* const cells = carve_<key_slot_type_>(survivors_size);
    std::uint64_t* const survivor_stamps = carve_<std::uint64_t>(survivors_size);
    std::uint8_t* const survivor_kinds = carve_<std::uint8_t>(survivors_size);
    if (survivors_size && (!cells || !survivor_stamps || !survivor_kinds)) return status_t::out_of_memory_heap_k;
    std::size_t const placed_count = locates_k ? survivors_size : 0;
    record_t* const placed = carve_bounce<record_t>(*executor_, placed_count);
    if ((placed_count != 0 && !placed) || executor_->pin_bounce().failed()) return status_t::out_of_memory_heap_k;
    if (cells_count != 0) {
        status = launch_over(*executor_, names_t::place_k, cells_count, std::uint64_t {1} << 20,
                             static_cast<record_t const*>(survivors), cells_count, stamps.floor, cells, survivor_stamps,
                             survivor_kinds);
        if (status != status_t::success_k) return status;
    }

    if (output.cells.resize(uninitialized, cells_count) != status_t::success_k ||
        output.stamps.resize(uninitialized, survivors_count) != status_t::success_k ||
        output.kinds.resize(uninitialized, survivors_count) != status_t::success_k)
        return status_t::out_of_memory_heap_k;
    if ((status = download(*executor_, output.cells.data(), cells, static_cast<std::size_t>(cells_count))) !=
        status_t::success_k)
        return status;
    if ((status = download(*executor_, output.stamps.data(), survivor_stamps, survivors_size)) != status_t::success_k)
        return status;
    if ((status = download(*executor_, output.kinds.data(), survivor_kinds, survivors_size)) != status_t::success_k)
        return status;
    if ((status = download(*executor_, placed, static_cast<record_t const*>(survivors), placed_count)) !=
        status_t::success_k)
        return status;
    if ((status = executor_status(executor_->wait())) != status_t::success_k) return status;

    // A merge that outgrew the newest run leaves one run as large as all it carved, so
    // the next of its size opens none; the device is idle and the reader forgets the run
    // it registered first.
    if (blocks_.count > 1) {
        std::size_t carved = blocks_.used;
        for (std::uint32_t block = 0; block + 1 != blocks_.count; ++block) carved += blocks_.blocks[block].capacity;
        sections_.release();
        utilities::rewind(blocks_, *executor_);
        if (carve_<byte_t>(carved)) utilities::rewind(blocks_, *executor_);
    }

    // The prefix is the new runs' value offsets, and the copy plan coalesces the survivors'
    // sources on the host.
    output.value_offsets.clear();
    output.value_copy_ranges.clear();
    if constexpr (locates_k) {
        if (is_variable_width(key_slot_type_::value_layout_k)) {
            if (output.value_offsets.resize(uninitialized, survivors_count + 1) != status_t::success_k)
                return status_t::out_of_memory_heap_k;
            std::memcpy(output.value_offsets.data(), prefixes, (survivors_count + 1) * sizeof(std::uint64_t));
        }
        return plan_value_copies<key_slot_type_>(placed, upper, lower, bounds.value_stride, output.sorted_runs,
                                                 output.value_copy_ranges);
    }
    return status_t::success_k;
}

#pragma endregion Apple

} // namespace unum::ustore::blobs

#include "nvme/compaction_variants.hpp" // the explicit instantiations of every vendor
