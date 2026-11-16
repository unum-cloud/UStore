/**
 *  @file include/ustore/blobs/vram_amd_compaction.hip
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The host half of the AMD device compactor, queueing the three kernels of every merge,
 *      ranking, packing and placing, through the driver.
 *
 *  The AMD bundle compiles the kernels of @c vram_nvidia_compaction.cuh with its class's block
 *  scan, so the pack compacts the survivors in one chained scan, as on an NVIDIA device. Every
 *  device region a merge needs is carved off the compactor's own device blocks, rewound per merge,
 *  and every host buffer off the executor's pinned bounce. The host half compiles with the host
 *  compiler, so nothing links a HIP runtime.
 */

#pragma once

#include "ustore/blobs/vram_amd_compaction.hpp"    // `hip_compactor_t`
#include "ustore/blobs/vram_nvidia_compaction.cuh" // `merge_run`, `carve_sections`, `fold_survivors`, `launch_over`

#include "ustore/utilities/vram_device.hpp" // `launch`, `carve_runs`, `rewind`

namespace unum::ustore::blobs {

#pragma region Merge

template <typename element_type_>
element_type_* hip_compactor_t::carve_(std::size_t count) noexcept {
    std::uint32_t const held = blocks_.count;
    expected<std::span<element_type_>> const carved = utilities::carve_runs<element_type_>(
        blocks_, *executor_, count, [&](std::size_t bytes) noexcept { return executor_->allocate_async(bytes); });
    device_allocations_ += blocks_.count - held;
    return carved ? carved->data() : nullptr;
}

template <typename record_type_>
st::status_t hip_compactor_t::pack_(char const* name, merge_pack_regions<record_type_> const& regions,
                                    std::uint64_t*& counted) noexcept {
    using st::status_t;
    std::uint64_t* const chains = carve_<std::uint64_t>(2);
    if (!chains) return status_t::out_of_memory_heap_k;
    counted = chains;
    status_t const zeroed = executor_status(
        executor_->fill_async(reinterpret_cast<std::uintptr_t>(chains), byte_t {0}, 2 * sizeof(std::uint64_t)));
    if (zeroed != status_t::success_k) return zeroed;
    std::uint64_t const tickets = fu::min_of(
        divide_round_up(regions.records_count, std::uint64_t {merge_block_threads_k}), utilities::chain_blocks_max_k);
    return launch_over(*executor_, name, tickets * merge_block_threads_k, tickets, regions.merged, regions.flags,
                       regions.records_count, regions.replaced, regions.replacements, regions.replaced_count, tickets,
                       regions.survivors, regions.prefix, regions.shares, chains);
}

template <typename key_slot_type_, utilities::fold_concept fold_type_, typename device_fold_type_,
          typename allocator_type_>
    requires compactable_key_slot_concept<key_slot_type_>
st::status_t hip_compactor_t::compact(compaction_kind_t kind,
                                      std::span<sorted_run_index_view<key_slot_type_> const> upper,
                                      std::span<sorted_run_index_view<key_slot_type_> const> lower,
                                      std::span<sorted_run_extents_t const> extents, sorted_run_bounds_t const& bounds,
                                      compaction_stamps_t const& stamps, fold_type_ const& fold,
                                      device_fold_type_ const& device_fold,
                                      std::type_identity_t<located_value_reader<key_slot_type_>> read_value,
                                      compaction_output<key_slot_type_, allocator_type_>& output) noexcept {
    // A key slot the bundles carry no kernels for merges on the host.
    if constexpr (merge_kernel_names<key_slot_type_>::exported_k)
        return compact_on_device_(kind, upper, lower, extents, bounds, stamps, fold, device_fold, read_value, output);
    else return st::status_t::invalid_argument_k;
}

template <typename key_slot_type_, utilities::fold_concept fold_type_, typename device_fold_type_,
          typename allocator_type_>
    requires compactable_key_slot_concept<key_slot_type_>
st::status_t hip_compactor_t::compact_on_device_(
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

#pragma endregion Merge

} // namespace unum::ustore::blobs
