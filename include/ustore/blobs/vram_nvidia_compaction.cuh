/**
 *  @file include/ustore/blobs/vram_nvidia_compaction.cuh
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The kernels of the device compactor on CUB: extraction, survival, folding, value offsets, padding and
 *      placement.
 *
 *  A merge holding changes copies its merged records back once survival is settled, walks them on the host as the host
 *  compactor does, hands the chains of changes alone to the device fold it was given, and folds the rest on the host.
 *
 *  Only nvcc compiles this half, so a host translation unit including it sees the host compactor of
 *  @c nvme_compaction.hpp alone, and @c compaction.cu instantiates the device compactor per key slot.
 */

#pragma once

#if defined(__CUDACC__)
#include <cub/device/device_for.cuh>
#include <cub/device/device_merge.cuh>
#include <cub/device/device_merge_sort.cuh>
#include <cub/device/device_scan.cuh>
#include <cub/device/device_select.cuh>
#include <cub/version.cuh>
#endif

#include "ustore/blobs/nvme_compaction.hpp"        // `cuda_compaction_lane_t`, `compaction_kind_t`, `compaction_output`
#include "ustore/utilities/vram_nvidia_device.hpp" // `cuda_stream_t`, `cuda_status`

#if defined(__CUDACC__)

namespace unum::ustore::blobs {

using utilities::cuda_device_scope_t;
using utilities::cuda_status;

static_assert(CUB_VERSION >= 300400, "GPU compaction needs CCCL 3.4 from cmake/cccl.cmake ahead of the toolkit's CUB");

#pragma region Kernels

/** Copies one sorted run's cells into the sorted positions of its run. */
template <typename key_slot_type_>
struct extract_kernel {

    /** The sorted run's cells on the device. */
    key_slot_type_ const* cells;

    /** The sorted run's run among all records. */
    located_key_slot<key_slot_type_>* records;

    /** Layout of the sorted run's sorted run index. */
    sorted_run_index_layout_t layout;

    /** Index of the sorted run. */
    std::uint32_t sorted_run;

    /** The sorted run's stamps in sorted order on the device, or null when it carries none. */
    std::uint64_t const* stamps;

    /** The sorted run's kinds in sorted order on the device, or null when every record is a write. */
    std::uint8_t const* kinds;

    /** The sorted run's value offsets on the device, or null for a run finding its values by stride. */
    stored_value_offset_t const* value_offsets;

    /** Bytes every value of a fixed-width layout takes, zero for any other. */
    std::uint64_t value_stride;

    constexpr void operator()(std::int64_t cell) const noexcept {
        extract_cell(cells, layout, static_cast<std::uint64_t>(cell), sorted_run, stamps, kinds, value_offsets,
                     value_stride, records);
    }
};

/** Flags the merged records that survive. */
template <typename key_slot_type_>
struct survivor_kernel {

    /** Records merged in survivor-first order. */
    located_key_slot<key_slot_type_> const* merged;

    /** One flag per merged record. */
    std::uint8_t* flags;

    /** The kind of compaction, which decides whether removals drop. */
    compaction_kind_t kind;

    /** Which older versions survive and which drop, its pinned stamps on the device. */
    compaction_stamps_t stamps;

    constexpr void operator()(std::int64_t index) const noexcept {
        located_key_slot<key_slot_type_> const* const previous = index != 0 ? merged + index - 1 : nullptr;
        flags[index] = survives(previous, merged[index], kind, stamps) ? 1 : 0;
    }
};

/** Writes each survivor's stored value bytes one slot ahead, so an inclusive sum leaves the value offsets behind. */
template <typename key_slot_type_>
struct stored_value_bytes_kernel {

    /** The survivors in sorted order. */
    located_key_slot<key_slot_type_> const* survivors;

    /** Prefix sums, one slot longer than the survivors. */
    std::uint64_t* prefix;

    constexpr void operator()(std::int64_t index) const noexcept { prefix[index + 1] = survivors[index].size; }
};

/** Fills every output cell with padding. */
template <typename key_slot_type_>
struct padding_kernel {

    /** Cells of all new sorted runs. */
    key_slot_type_* cells;

    /** The key slot padding cells hold. */
    key_slot_type_ padding;

    constexpr void operator()(std::int64_t cell) const noexcept { cells[cell] = padding; }
};

/** Where one new sorted run starts among the survivors and the cells. */
struct sorted_run_placement_t {

    /** Index of the sorted run's first survivor. */
    std::uint64_t first_key_slot = 0;

    /** Index of the sorted run's first cell. */
    std::uint64_t first_cell = 0;

    /** Layout of the sorted run's sorted run index. */
    sorted_run_index_layout_t layout;
};

/** Places each survivor into its sorted run, with its stamp and its kind. */
template <typename key_slot_type_>
struct place_kernel {

    /** The survivors in sorted order. */
    located_key_slot<key_slot_type_> const* survivors;

    /** Placements of the new sorted runs, ascending. */
    sorted_run_placement_t const* placements;

    /** Number of placements. */
    std::uint64_t placements_count;

    /** Cells of all new sorted runs. */
    key_slot_type_* cells;

    /** The stamp every survivor's stamp is raised to at least. */
    std::uint64_t floor;

    /** One stamp per survivor. */
    std::uint64_t* stamps;

    /** One kind per survivor. */
    std::uint8_t* kinds;

    constexpr void operator()(std::int64_t signed_index) const noexcept {
        std::uint64_t const index = static_cast<std::uint64_t>(signed_index);
        std::uint64_t low = 0;
        std::uint64_t high = placements_count;
        while (high - low > 1) {
            std::uint64_t const middle = low + (high - low) / 2;
            if (placements[middle].first_key_slot <= index) low = middle;
            else high = middle;
        }
        sorted_run_placement_t const& placement = placements[low];
        place_key_slot(survivors[index], placement.layout, index - placement.first_key_slot, floor,
                       cells + placement.first_cell, stamps + index, kinds + index);
    }
};

/** Writes each folded survivor over the survivor it replaces. */
template <typename key_slot_type_>
struct folded_survivor_kernel {

    /** The survivors in sorted order. */
    located_key_slot<key_slot_type_>* survivors;

    /** The index of every folded survivor among the survivors. */
    std::uint64_t const* indices;

    /** Every folded survivor. */
    located_key_slot<key_slot_type_> const* folded;

    constexpr void operator()(std::int64_t index) const noexcept { survivors[indices[index]] = folded[index]; }
};

/** Copies @p count elements from @p source to @p target on @p stream and waits until it ran, as every upload and
 *  download of a compaction does. */
template <typename element_type_>
st::status_t copy_waiting(element_type_* target, element_type_ const* source, std::size_t count,
                          cudaStream_t stream) noexcept {
    if (count == 0) return st::status_t::success_k;
    st::status_t const status = cuda_status(
        cudaMemcpyAsync(target, source, count * sizeof(element_type_), cudaMemcpyDefault, stream));
    return status != st::status_t::success_k ? status : cuda_status(cudaStreamSynchronize(stream));
}

/**
 *  @brief Folds what the survivors of a merge fold, as @c host_compactor_t::compact does, writing their values into
 *      @p output's folded bytes and listing each folded survivor's index and record in @p indices and
 *      @p replacements, which the caller writes over the survivors on the device.
 *
 *  The merged records and their survival flags come back to the host, which finds the versions behind every surviving
 *  change through @c folded_versions, as the host compactor does. A surviving change whose older versions are all
 *  changes, and which the bottom level does not apply onto its empty base, becomes one chain that @p device_fold
 *  composes with every other such chain at once. Every other chain folds on the host through @c fold_versions.
 */
template <typename key_slot_type_, typename fold_type_, typename device_fold_type_, typename allocator_type_>
st::status_t fold_survivors(cuda_stream_t const& stream, compaction_kind_t kind, compaction_stamps_t const& stamps,
                            std::uint64_t sorted_runs_count, std::uint64_t value_stride,
                            located_key_slot<key_slot_type_> const* merged, std::uint8_t const* flags,
                            std::uint64_t merged_count, fold_type_ const& fold, device_fold_type_ const& device_fold,
                            located_value_reader<key_slot_type_> read_value,
                            compaction_output<key_slot_type_, allocator_type_>& output,
                            st::basic_vector<std::uint64_t>& indices,
                            st::basic_vector<located_key_slot<key_slot_type_>>& replacements) noexcept {
    using st::basic_vector;
    using st::status_t;
    using record_t = located_key_slot<key_slot_type_>;
    cudaStream_t const handle = stream.handle();

    basic_vector<record_t> records;
    basic_vector<std::uint8_t> kept;
    status_t status = records.resize(merged_count);
    if (status != status_t::success_k) return status;
    status = kept.resize(merged_count);
    if (status != status_t::success_k) return status;
    status = cuda_status(
        cudaMemcpyAsync(records.data(), merged, merged_count * sizeof(record_t), cudaMemcpyDeviceToHost, handle));
    if (status != status_t::success_k) return status;
    status = cuda_status(cudaMemcpyAsync(kept.data(), flags, merged_count, cudaMemcpyDeviceToHost, handle));
    if (status != status_t::success_k) return status;
    status = stream.synchronize();
    if (status != status_t::success_k) return status;

    utilities::local_arena_t arena;
    utilities::arena_allocator<byte_t> arena_bytes = arena.allocator();
    auto const value_of = [&](record_t const& record, std::span<byte_t const>& bytes) noexcept {
        bytes = {};
        if (record.kind == delta_kind_t::remove_k) return status_t::success_k;
        return read_value(record, arena_bytes, bytes);
    };

    // A folded survivor: its value folded here, or the chain the device composes it from
    struct folded_t {
        std::uint64_t survivor = 0;
        std::uint64_t merged = 0;
        delta_kind_t kind = delta_kind_t::change_k;
        std::span<byte_t const> bytes;
        std::uint64_t chain = std::numeric_limits<std::uint64_t>::max();
    };
    basic_vector<folded_t> folds;
    basic_vector<delta_view_t> changes;
    basic_vector<std::uint64_t> chain_starts;
    status = chain_starts.push_back(std::uint64_t {0});
    if (status != status_t::success_k) return status;

    std::uint64_t survivor = 0;
    for (std::uint64_t head = 0; head != merged_count; ++head) {
        if (!kept[head]) continue;
        std::uint64_t const ordinal = survivor++;
        if (records[head].kind != delta_kind_t::change_k) continue;
        folded_versions_t const versions = folded_versions(kind, stamps, records.data(), kept.data(), merged_count,
                                                           head);
        if (versions.end == head + 1 && !versions.applies) continue;
        bool only_changes = true;
        for (std::uint64_t older = head + 1; older != versions.end; ++older)
            only_changes &= records[older].kind == delta_kind_t::change_k;

        if (only_changes && !versions.applies) {
            for (std::uint64_t version = versions.end; version != head; --version) {
                std::span<byte_t const> bytes;
                if ((status = value_of(records[version - 1], bytes)) != status_t::success_k) return status;
                if ((status = changes.push_back(delta_view_t {delta_kind_t::change_k, bytes})) != status_t::success_k)
                    return status;
            }
            if ((status = chain_starts.push_back(std::uint64_t {changes.size()})) != status_t::success_k) return status;
            status = folds.push_back(folded_t {ordinal, head, delta_kind_t::change_k, {}, chain_starts.size() - 2});
            if (status != status_t::success_k) return status;
            continue;
        }

        delta_view_t folded;
        status = fold_versions(fold, records.data(), head, versions, value_of, arena_bytes, folded);
        if (status != status_t::success_k) return status;
        status = folds.push_back(folded_t {ordinal, head, folded.kind, folded.bytes});
        if (status != status_t::success_k) return status;
    }

    basic_vector<byte_t> composed;
    basic_vector<std::uint64_t> composed_offsets;
    if (chain_starts.size() > 1) {
        status = device_fold.compose(stream, changes, chain_starts, composed, composed_offsets);
        if (status != status_t::success_k) return status;
    }

    // A folded value replaces its survivor's own: inline in the key slot, or in the compaction's folded bytes
    status = indices.resize(folds.size());
    if (status != status_t::success_k) return status;
    status = replacements.resize(folds.size());
    if (status != status_t::success_k) return status;
    for (std::size_t index = 0; index != folds.size(); ++index) {
        folded_t const& folded = folds[index];
        std::span<byte_t const> const bytes =
            folded.chain == std::numeric_limits<std::uint64_t>::max()
                ? folded.bytes
                : std::span<byte_t const> {composed.data() + composed_offsets[folded.chain],
                                           composed_offsets[folded.chain + 1] - composed_offsets[folded.chain]};
        record_t record = records[folded.merged];
        record.kind = folded.kind;
        status = place_folded_value(bytes, value_stride, static_cast<std::uint32_t>(sorted_runs_count), record,
                                    output.folded);
        if (status != status_t::success_k) return status;
        indices[index] = folded.survivor;
        replacements[index] = record;
    }
    return status_t::success_k;
}

template <typename key_slot_type_, utilities::fold_concept fold_type_, typename device_fold_type_,
          typename allocator_type_>
    requires compactable_key_slot_concept<key_slot_type_>
st::status_t cuda_compaction_lane_t::compact(
    compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_type_> const> upper,
    std::span<sorted_run_index_view<key_slot_type_> const> lower, sorted_run_bounds_t const& bounds,
    compaction_stamps_t const& stamps, [[maybe_unused]] fold_type_ const& fold,
    [[maybe_unused]] device_fold_type_ const& device_fold,
    [[maybe_unused]] std::type_identity_t<located_value_reader<key_slot_type_>> read_value,
    compaction_output<key_slot_type_, allocator_type_>& output) noexcept {
    using st::basic_vector;
    using st::expected;
    using st::status_t;
    using record_t = located_key_slot<key_slot_type_>;

    if (!admits_sorted_runs(bounds)) return status_t::invalid_argument_k;
    std::uint64_t const sorted_runs_count = upper.size() + lower.size();
    if (sorted_runs_count > std::numeric_limits<std::uint32_t>::max()) return status_t::invalid_argument_k;
    if (!views_locate_values(upper, bounds) || !views_locate_values(lower, bounds)) return status_t::invalid_argument_k;
    int const device = stream_.device();
    cudaStream_t const stream = stream_.handle();
    expected<cuda_device_scope_t> const scope = cuda_device_scope_t::make(device);
    if (!scope) return scope.status();
    output.folded.clear();

    constexpr bool device_folds_k = !std::is_same_v<device_fold_type_, no_device_fold_t>;
    std::uint64_t upper_key_slots = 0;
    std::uint64_t merged_count = 0;
    std::uint64_t largest_cells = 0;
    bool changes = false;
    for (std::uint64_t sorted_run = 0; sorted_run != sorted_runs_count; ++sorted_run) {
        sorted_run_index_view<key_slot_type_> const& view = sorted_run < upper.size()
                                                                ? upper[sorted_run]
                                                                : lower[sorted_run - upper.size()];
        changes |= holds_changes(view.kinds);
        // Without a device fold, changes fold on the host alone, where every merge holding one runs.
        if (!device_folds_k && changes) return status_t::invalid_argument_k;
        std::uint64_t const cells = round_up_to_multiple(view.key_slots_count, bounds.branching);
        largest_cells = cells > largest_cells ? cells : largest_cells;
        merged_count += view.key_slots_count;
        if (sorted_run < upper.size()) upper_key_slots += view.key_slots_count;
    }

    // Every buffer is the lane's own, grown to the largest merge so far, so a warm lane allocates nothing here.
    record_t* records = nullptr;
    record_t* scratch = nullptr;
    key_slot_type_* sorted_run_cells = nullptr;
    std::uint64_t* sorted_run_stamps = nullptr;
    std::uint8_t* sorted_run_kinds = nullptr;
    stored_value_offset_t* sorted_run_offsets = nullptr;
    status_t status = room_(scratch_role_t::records_k, merged_count, records);
    if (status != status_t::success_k) return status;
    status = room_(scratch_role_t::merged_k, merged_count, scratch);
    if (status != status_t::success_k) return status;
    status = room_(scratch_role_t::run_cells_k, largest_cells, sorted_run_cells);
    if (status != status_t::success_k) return status;
    status = room_(scratch_role_t::run_stamps_k, largest_cells, sorted_run_stamps);
    if (status != status_t::success_k) return status;
    status = room_(scratch_role_t::run_kinds_k, largest_cells, sorted_run_kinds);
    if (status != status_t::success_k) return status;
    status = room_(scratch_role_t::run_offsets_k, largest_cells + 1, sorted_run_offsets);
    if (status != status_t::success_k) return status;

    // A run's stamps and kinds are packed, so they unpack on the host into blocks the size of the largest run.
    basic_vector<std::uint64_t> unpacked;
    status = unpacked.resize(largest_cells);
    if (status != status_t::success_k) return status;
    basic_vector<std::uint8_t> unpacked_kinds;
    status = unpacked_kinds.resize(largest_cells);
    if (status != status_t::success_k) return status;

    // The pinned stamps are a few words, read by every survival check, so the device holds its own copy.
    compaction_stamps_t device_stamps = stamps;
    if (stamps.pinned_count != 0) {
        std::uint64_t* pinned = nullptr;
        status = room_(scratch_role_t::pinned_k, stamps.pinned_count, pinned);
        if (status != status_t::success_k) return status;
        status = copy_waiting(pinned, stamps.pinned, stamps.pinned_count, stream);
        if (status != status_t::success_k) return status;
        device_stamps.pinned = pinned;
    }

    // Growing the temporary frees what it held, which the lane does only once the kernels reading it finished.
    std::byte* temporary = nullptr;
    auto const grow_temporary = [&temporary, this](std::size_t bytes) noexcept {
        return room_(scratch_role_t::temporary_k, bytes, temporary);
    };

    // Every sorted run's cells upload and land at their sorted positions, in one run per sorted run
    std::uint64_t run_offset = 0;
    for (std::uint64_t sorted_run = 0; sorted_run != sorted_runs_count; ++sorted_run) {
        sorted_run_index_view<key_slot_type_> const& view = sorted_run < upper.size()
                                                                ? upper[sorted_run]
                                                                : lower[sorted_run - upper.size()];
        sorted_run_index_layout_t const layout(view.key_slots_count, bounds.branching);
        if (layout.cells_count() == 0) continue;
        status = copy_waiting(sorted_run_cells, view.cells, layout.cells_count(), stream);
        if (status != status_t::success_k) return status;
        std::uint64_t const* run_stamps = nullptr;
        std::uint8_t const* run_kinds = nullptr;
        bool const carries_stamps = view.stamps != nullptr && view.key_slots_count != 0;
        bool const carries_kinds = view.kinds != nullptr && view.key_slots_count != 0;
        // The uploads below read the host blocks asynchronously, so the previous run's have to finish first.
        if (carries_stamps | carries_kinds) {
            status = stream_.synchronize();
            if (status != status_t::success_k) return status;
        }
        if (carries_stamps) {
            for (std::uint64_t sorted = 0; sorted != view.key_slots_count; ++sorted)
                unpacked[sorted] = (*view.stamps)[static_cast<std::size_t>(sorted)];
            status = copy_waiting(sorted_run_stamps, unpacked.data(), view.key_slots_count, stream);
            if (status != status_t::success_k) return status;
            run_stamps = sorted_run_stamps;
        }
        if (carries_kinds) {
            for (std::uint64_t sorted = 0; sorted != view.key_slots_count; ++sorted)
                unpacked_kinds[sorted] = (*view.kinds)[static_cast<std::size_t>(sorted)];
            status = copy_waiting(sorted_run_kinds, unpacked_kinds.data(), view.key_slots_count, stream);
            if (status != status_t::success_k) return status;
            run_kinds = sorted_run_kinds;
        }
        stored_value_offset_t const* run_offsets = nullptr;
        if (!view.value_offsets.empty()) {
            status = copy_waiting(sorted_run_offsets, view.value_offsets.data(), view.value_offsets.size(), stream);
            if (status != status_t::success_k) return status;
            run_offsets = sorted_run_offsets;
        }
        extract_kernel<key_slot_type_> const kernel {sorted_run_cells, records + run_offset,
                                                     layout,           static_cast<std::uint32_t>(sorted_run),
                                                     run_stamps,       run_kinds,
                                                     run_offsets,      bounds.value_stride};
        status = cuda_status(cub::DeviceFor::Bulk(static_cast<std::int64_t>(layout.cells_count()), kernel, stream));
        if (status != status_t::success_k) return status;
        run_offset += view.key_slots_count;
    }

    // Overlapping upper key slots sort so the newest of every stored key leads its run
    survivor_first_t const order {static_cast<std::uint32_t>(upper.size())};
    bool const overlapping = (kind == compaction_kind_t::overlapping_into_intermediate_k) |
                             (kind == compaction_kind_t::overlapping_into_bottom_k);
    if (overlapping & (upper_key_slots > 1)) {
        std::size_t bytes = 0;
        std::int64_t const count = static_cast<std::int64_t>(upper_key_slots);
        status = cuda_status(cub::DeviceMergeSort::SortKeys(nullptr, bytes, records, count, order, stream));
        if (status != status_t::success_k) return status;
        status = grow_temporary(bytes);
        if (status != status_t::success_k) return status;
        status = cuda_status(cub::DeviceMergeSort::SortKeys(temporary, bytes, records, count, order, stream));
        if (status != status_t::success_k) return status;
    }

    // One merge path joins the upper and lower runs
    record_t* merged = records;
    record_t* kept = scratch;
    if ((upper_key_slots != 0) & (merged_count != upper_key_slots)) {
        std::size_t bytes = 0;
        std::int64_t const upper_count = static_cast<std::int64_t>(upper_key_slots);
        std::int64_t const lower_count = static_cast<std::int64_t>(merged_count - upper_key_slots);
        status = cuda_status(cub::DeviceMerge::MergeKeys(nullptr, bytes, records, upper_count, records + upper_count,
                                                         lower_count, scratch, order, stream));
        if (status != status_t::success_k) return status;
        status = grow_temporary(bytes);
        if (status != status_t::success_k) return status;
        status = cuda_status(cub::DeviceMerge::MergeKeys(temporary, bytes, records, upper_count, records + upper_count,
                                                         lower_count, scratch, order, stream));
        if (status != status_t::success_k) return status;
        merged = scratch;
        kept = records;
    }

    // The first record of every stored key survives, unless it is a removal where removals drop
    std::uint64_t survivors = 0;
    if (merged_count != 0) {
        std::uint8_t* flags = nullptr;
        std::int64_t* selected = nullptr;
        status = room_(scratch_role_t::flags_k, merged_count, flags);
        if (status != status_t::success_k) return status;
        status = room_(scratch_role_t::selected_k, 1, selected);
        if (status != status_t::success_k) return status;
        std::int64_t const count = static_cast<std::int64_t>(merged_count);
        survivor_kernel<key_slot_type_> const kernel {merged, flags, kind, device_stamps};
        status = cuda_status(cub::DeviceFor::Bulk(count, kernel, stream));
        if (status != status_t::success_k) return status;
        std::size_t bytes = 0;
        status = cuda_status(cub::DeviceSelect::Flagged(nullptr, bytes, merged, flags, kept, selected, count, stream));
        if (status != status_t::success_k) return status;
        status = grow_temporary(bytes);
        if (status != status_t::success_k) return status;
        status = cuda_status(
            cub::DeviceSelect::Flagged(temporary, bytes, merged, flags, kept, selected, count, stream));
        if (status != status_t::success_k) return status;
        std::int64_t selected_count = 0;
        status = copy_waiting(&selected_count, selected, 1, stream);
        if (status != status_t::success_k) return status;
        survivors = static_cast<std::uint64_t>(selected_count);
        if constexpr (device_folds_k)
            if (changes) {
                basic_vector<std::uint64_t> indices;
                basic_vector<record_t> replacements;
                status = fold_survivors(stream_, kind, stamps, sorted_runs_count, bounds.value_stride, merged, flags,
                                        merged_count, fold, device_fold, read_value, output, indices, replacements);
                if (status != status_t::success_k) return status;
                if (!indices.empty()) {
                    std::uint64_t* device_indices = nullptr;
                    record_t* device_replacements = nullptr;
                    status = room_(scratch_role_t::fold_indices_k, indices.size(), device_indices);
                    if (status != status_t::success_k) return status;
                    status = room_(scratch_role_t::fold_records_k, replacements.size(), device_replacements);
                    if (status != status_t::success_k) return status;
                    status = copy_waiting(device_indices, indices.data(), indices.size(), stream);
                    if (status != status_t::success_k) return status;
                    status = copy_waiting(device_replacements, replacements.data(), replacements.size(), stream);
                    if (status != status_t::success_k) return status;
                    folded_survivor_kernel<key_slot_type_> const kernel {kept, device_indices, device_replacements};
                    status = cuda_status(
                        cub::DeviceFor::Bulk(static_cast<std::int64_t>(indices.size()), kernel, stream));
                    if (status != status_t::success_k) return status;
                    // The host lists above free on return, which the copies already read.
                }
            }
    }

    std::uint64_t* prefix = nullptr;
    if constexpr (locates_stored_values_concept<key_slot_type_>) {
        status = room_(scratch_role_t::prefix_k, survivors + 1, prefix);
        if (status != status_t::success_k) return status;
        std::uint64_t const zero = 0;
        status = copy_waiting(prefix, &zero, 1, stream);
        if (status != status_t::success_k) return status;
        if (survivors != 0) {
            std::int64_t const count = static_cast<std::int64_t>(survivors);
            stored_value_bytes_kernel<key_slot_type_> const kernel {kept, prefix};
            status = cuda_status(cub::DeviceFor::Bulk(count, kernel, stream));
            if (status != status_t::success_k) return status;
            std::size_t bytes = 0;
            status = cuda_status(cub::DeviceScan::InclusiveSum(nullptr, bytes, prefix + 1, count, stream));
            if (status != status_t::success_k) return status;
            status = grow_temporary(bytes);
            if (status != status_t::success_k) return status;
            status = cuda_status(cub::DeviceScan::InclusiveSum(temporary, bytes, prefix + 1, count, stream));
            if (status != status_t::success_k) return status;
        }
    }
    auto const read_prefix = [prefix, stream](std::uint64_t index, std::uint64_t& bytes) noexcept {
        bytes = 0;
        return prefix != nullptr ? copy_waiting(&bytes, prefix + index, 1, stream) : status_t::success_k;
    };
    auto const shares_key = [kept, stream](std::uint64_t index, bool& shared) noexcept {
        record_t neighbours[2] {};
        status_t const copied = cuda_status(
            cudaMemcpyAsync(neighbours, kept + index - 1, sizeof(neighbours), cudaMemcpyDeviceToHost, stream));
        if (copied != status_t::success_k) return copied;
        status_t const waited = cuda_status(cudaStreamSynchronize(stream));
        shared = neighbours[0].key_slot.stored_key == neighbours[1].key_slot.stored_key;
        return waited;
    };
    status = plan_sorted_runs<key_slot_type_>(survivors, bounds, read_prefix, shares_key, output.sorted_runs);
    if (status != status_t::success_k) return status;

    basic_vector<sorted_run_placement_t> placements;
    status = placements.resize(output.sorted_runs.size());
    if (status != status_t::success_k) return status;
    std::uint64_t first = 0;
    std::uint64_t cells_count = 0;
    for (std::size_t index = 0; index != output.sorted_runs.size(); ++index) {
        std::uint64_t const count = output.sorted_runs[index].key_slots_count;
        placements[index] = sorted_run_placement_t {first, cells_count,
                                                    sorted_run_index_layout_t(count, bounds.branching)};
        first += count;
        cells_count += round_up_to_multiple(count, bounds.branching);
    }

    sorted_run_placement_t* device_placements = nullptr;
    key_slot_type_* cells = nullptr;
    std::uint64_t* survivor_stamps = nullptr;
    std::uint8_t* survivor_kinds = nullptr;
    status = room_(scratch_role_t::placements_k, placements.size(), device_placements);
    if (status != status_t::success_k) return status;
    status = copy_waiting(device_placements, placements.data(), placements.size(), stream);
    if (status != status_t::success_k) return status;
    status = room_(scratch_role_t::cells_k, cells_count, cells);
    if (status != status_t::success_k) return status;
    status = room_(scratch_role_t::stamps_k, survivors, survivor_stamps);
    if (status != status_t::success_k) return status;
    status = room_(scratch_role_t::kinds_k, survivors, survivor_kinds);
    if (status != status_t::success_k) return status;

    if (cells_count != 0) {
        padding_kernel<key_slot_type_> const kernel {cells, padding_key_slot<key_slot_type_>()};
        status = cuda_status(cub::DeviceFor::Bulk(static_cast<std::int64_t>(cells_count), kernel, stream));
        if (status != status_t::success_k) return status;
    }
    if (survivors != 0) {
        place_kernel<key_slot_type_> const kernel {kept,         device_placements, placements.size(), cells,
                                                   stamps.floor, survivor_stamps,   survivor_kinds};
        status = cuda_status(cub::DeviceFor::Bulk(static_cast<std::int64_t>(survivors), kernel, stream));
        if (status != status_t::success_k) return status;
    }

    status = output.cells.resize(cells_count);
    if (status != status_t::success_k) return status;
    status = copy_waiting(output.cells.data(), cells, cells_count, stream);
    if (status != status_t::success_k) return status;
    status = output.stamps.resize(survivors);
    if (status != status_t::success_k) return status;
    status = copy_waiting(output.stamps.data(), survivor_stamps, survivors, stream);
    if (status != status_t::success_k) return status;
    status = output.kinds.resize(survivors);
    if (status != status_t::success_k) return status;
    status = copy_waiting(output.kinds.data(), survivor_kinds, survivors, stream);
    if (status != status_t::success_k) return status;

    // The prefix is the new runs' value offsets, and the copy plan coalesces the survivors' sources on the host.
    output.value_offsets.clear();
    output.value_copy_ranges.clear();
    if constexpr (locates_stored_values_concept<key_slot_type_>) {
        if (is_variable_width(key_slot_type_::value_layout_k)) {
            status = output.value_offsets.resize(survivors + 1);
            if (status != status_t::success_k) return status;
            status = copy_waiting(output.value_offsets.data(), prefix, survivors + 1, stream);
            if (status != status_t::success_k) return status;
        }
        basic_vector<record_t> placed;
        status = placed.resize(survivors);
        if (status != status_t::success_k) return status;
        status = cuda_status(
            cudaMemcpyAsync(placed.data(), kept, survivors * sizeof(record_t), cudaMemcpyDeviceToHost, stream));
        if (status != status_t::success_k) return status;
        status = stream_.synchronize();
        if (status != status_t::success_k) return status;
        return plan_value_copies<key_slot_type_>(placed.data(), upper, lower, bounds.value_stride, output.sorted_runs,
                                                 output.value_copy_ranges);
    }
    return stream_.synchronize();
}

#pragma endregion Kernels

} // namespace unum::ustore::blobs

#endif
