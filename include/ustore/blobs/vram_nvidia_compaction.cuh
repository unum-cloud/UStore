/**
 *  @file include/ustore/blobs/vram_nvidia_compaction.cuh
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The kernels of the device compactor on CUB: extraction, unpacking, survival, folding, value offsets, padding
 *      and placement, and the one context a merge carves for them.
 *
 *  A merge sizes every region it touches up front, CUB's temporary bytes included, and carves them out of its
 *  compactor's scratch, so the scratch grows at most once per merge. Every run's sections land first, by extent where
 *  a sections reader reaches the volumes and by one upload each from memory otherwise, and two kernels unpack each
 *  run's saved stamps and kinds on the device. The host waits twice: once survival settles, for what plans the new
 *  runs, and once they are placed. A merge holding changes waits once more, as the host walks its merged records to
 *  hand the chains of changes alone to the device fold it was given and folds the rest itself. Every copy queues on the
 *  executor's stream, its host side carved off the executor's pinned bounce, which every merge rewinds.
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

#include "ustore/blobs/nvme_compaction.hpp"         // `cuda_compactor_t`, `compaction_kind_t`, `compaction_output`
#include "ustore/utilities/vram_nvidia_runtime.cuh" // `cuda_status`

#if defined(__CUDACC__)

namespace unum::ustore::blobs {

using utilities::cuda_executor_t;
using utilities::cuda_status;

static_assert(CUB_VERSION >= 300400, "GPU compaction needs CCCL 3.4 from cmake/cccl.cmake ahead of the toolkit's CUB");

/** A failure of the executor as a compaction's status. */
inline st::status_t executor_status(status_t status) noexcept {
    return status.failed() ? st::status_t::unknown_k : st::status_t::success_k;
}

/** Carves @p count elements off @p executor's bounce for one merge, which the next merge rewinds. */
template <typename element_type_>
element_type_* carve_bounce(cuda_executor_t& executor, std::size_t count) noexcept {
    auto [carved, status] = utilities::arena_allocate<element_type_>(executor.bounce(), count);
    return status.failed() ? nullptr : carved.data();
}

/** Queues uploading @p count elements from host @p source to device @p target on @p executor. */
template <typename element_type_>
st::status_t upload(cuda_executor_t& executor, element_type_* target, element_type_ const* source,
                    std::size_t count) noexcept {
    if (count == 0) return st::status_t::success_k;
    std::span<element_type_ const> const host {source, count};
    return executor_status(executor.upload_async(reinterpret_cast<std::uintptr_t>(target), std::as_bytes(host)));
}

/** Queues downloading @p count elements from device @p source to host @p target on @p executor. */
template <typename element_type_>
st::status_t download(cuda_executor_t& executor, element_type_* target, element_type_ const* source,
                      std::size_t count) noexcept {
    if (count == 0) return st::status_t::success_k;
    std::span<element_type_> const host {target, count};
    return executor_status(
        executor.download_async(std::as_writable_bytes(host), reinterpret_cast<std::uintptr_t>(source)));
}

#pragma region Kernels

/** The value at @p index of a column saved as SmashTable saves a packed array: its count, base and width words, then
 *  the packed words. */
template <typename value_type_>
constexpr value_type_ saved_field(std::uint64_t const* saved, std::uint64_t index) noexcept {
    std::uint64_t const base = saved[1];
    unsigned const bits = static_cast<unsigned>(saved[2]);
    if (bits == 0) return static_cast<value_type_>(base);
    std::uint64_t const* const words = saved + 3;
    std::uint64_t const first = index * bits;
    std::uint64_t const word = first / 64;
    unsigned const shift = static_cast<unsigned>(first % 64);
    std::uint64_t field = words[word] >> shift;
    if (shift + bits > 64) field |= words[word + 1] << (64 - shift);
    std::uint64_t const mask = bits == 64 ? ~std::uint64_t {0} : (std::uint64_t {1} << bits) - 1;
    return static_cast<value_type_>(base + (field & mask));
}

/** Copies one sorted run's cells into the sorted positions of its run, each a write stamped zero. */
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

    /** The sorted run's value offsets on the device, or null for a run finding its values by stride. */
    stored_value_offset_t const* value_offsets;

    /** Bytes every value of a fixed-width layout takes, zero for any other. */
    std::uint64_t value_stride;

    constexpr void operator()(std::int64_t cell) const noexcept {
        extract_cell(cells, layout, static_cast<std::uint64_t>(cell), sorted_run, value_offsets, value_stride, records);
    }
};

/** Unpacks one sorted run's saved stamps over its extracted records, raising a run an ingest stamped to its stamp. */
template <typename key_slot_type_>
struct unpack_stamps_kernel {

    /** The saved stamps column on the device. */
    std::uint64_t const* saved;

    /** The sorted run's run among all records. */
    located_key_slot<key_slot_type_>* records;

    /** The stamp an ingest assigned the run, which its zeros on disk read as, or zero for its own stamps. */
    std::uint64_t assigned;

    constexpr void operator()(std::int64_t index) const noexcept {
        std::uint64_t const position = static_cast<std::uint64_t>(index);
        records[position].stamp = assigned != 0 ? assigned : saved_field<std::uint64_t>(saved, position);
    }
};

/** Unpacks one sorted run's saved kinds over its extracted records. */
template <typename key_slot_type_>
struct unpack_kinds_kernel {

    /** The saved kinds column on the device. */
    std::uint64_t const* saved;

    /** The sorted run's run among all records. */
    located_key_slot<key_slot_type_>* records;

    constexpr void operator()(std::int64_t index) const noexcept {
        std::uint64_t const position = static_cast<std::uint64_t>(index);
        records[position].kind = static_cast<delta_kind_t>(saved_field<std::uint8_t>(saved, position));
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

/** Writes each survivor's stored value bytes one slot ahead, so an inclusive sum leaves the value offsets behind, and
 *  whether it carries the key of the survivor before it, both zero past the survivors, whose count only the device
 *  holds yet. */
template <typename key_slot_type_>
struct survivor_sizes_kernel {

    /** The survivors in sorted order, then what selection left behind them. */
    located_key_slot<key_slot_type_> const* survivors;

    /** How many survivors there are. */
    std::int64_t const* selected;

    /** Prefix sums, one slot longer than the records, or null for values held inline. */
    std::uint64_t* prefix;

    /** One flag per record. */
    std::uint8_t* shares;

    constexpr void operator()(std::int64_t index) const noexcept {
        bool const inside = index < *selected;
        if (prefix != nullptr) {
            if (index == 0) prefix[0] = 0;
            prefix[index + 1] = inside ? survivors[index].size : 0;
        }
        shares[index] = inside && index != 0 &&
                        survivors[index - 1].key_slot.stored_key == survivors[index].key_slot.stored_key;
    }
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

#pragma endregion Kernels

#pragma region Context

/** Bumps aligned regions off one address, which a merge runs once from zero to size its scratch, then again from the
 *  scratch itself to carve it. */
struct scratch_cursor_t {

    /** The next free address. */
    std::uintptr_t at = 0;

    /** Takes room for @p count elements at @p alignment, null for none. */
    template <typename element_type_>
    element_type_* take(std::size_t count, std::size_t alignment = 256) noexcept {
        if (count == 0) return nullptr;
        at = round_up_to_multiple<std::uintptr_t>(at, alignment);
        element_type_* const taken = reinterpret_cast<element_type_*>(at);
        at += count * sizeof(element_type_);
        return taken;
    }
};

/** Where one run's sections land in a merge's context, and whether they read from its volume. */
template <typename key_slot_type_>
struct run_sections {

    /** The cells, whole rows including padding. */
    key_slot_type_* cells = nullptr;

    /** The value offsets, null for a run finding its values by stride. */
    stored_value_offset_t* value_offsets = nullptr;

    /** The trailer of a run reading from its volume, its saved stamps first. */
    std::uint64_t* trailer = nullptr;

    /** Whether the run reads by extent rather than uploading from memory. */
    bool reads_volume = false;
};

/** Carves the sections of the run @p view describes off @p cursor: by @p extents, page aligned, when @p reads_volumes
 *  and the run lives on one, and as its memory holds them otherwise. */
template <typename key_slot_type_>
run_sections<key_slot_type_> carve_sections(sorted_run_index_view<key_slot_type_> const& view,
                                            sorted_run_extents_t const& extents, sorted_run_bounds_t const& bounds,
                                            bool reads_volumes, scratch_cursor_t& cursor) noexcept {
    run_sections<key_slot_type_> sections;
    sections.reads_volume = reads_volumes && !extents.cells.empty() && !extents.trailer.empty();
    std::uint64_t const cells_count = round_up_to_multiple(view.key_slots_count, bounds.branching);
    std::size_t const page = static_cast<std::size_t>(utilities::portable_alignment_k);
    if (sections.reads_volume) {
        sections.cells = cursor.take<key_slot_type_>(
            static_cast<std::size_t>(extents.cells.length / sizeof(key_slot_type_) + 1), page);
        if (!view.value_offsets.empty())
            sections.value_offsets = cursor.take<stored_value_offset_t>(
                static_cast<std::size_t>(extents.value_offsets.length / sizeof(stored_value_offset_t) + 1), page);
        sections.trailer = cursor.take<std::uint64_t>(
            static_cast<std::size_t>(extents.trailer.length / sizeof(std::uint64_t) + 1), page);
        return sections;
    }
    sections.cells = cursor.take<key_slot_type_>(static_cast<std::size_t>(cells_count));
    sections.value_offsets = cursor.take<stored_value_offset_t>(view.value_offsets.size());
    return sections;
}

/** Words of the saved stamps and kinds of the run @p view describes when it uploads from memory, as SmashTable saves
 *  them, a multiple of eight bytes each. */
template <typename key_slot_type_>
constexpr std::size_t saved_words(sorted_run_index_view<key_slot_type_> const& view) noexcept {
    std::size_t words = 0;
    if (view.key_slots_count == 0) return words;
    if (view.stamps != nullptr) words += view.stamps->saved_bytes() / sizeof(std::uint64_t);
    if (view.kinds != nullptr) words += view.kinds->saved_bytes() / sizeof(std::uint64_t);
    return words;
}

/**
 *  @brief The device memory one merge works in, carved once from its compactor's scratch.
 *
 *  The survivors come first and stay live to the end; everything past them is dead once survival settles and the host
 *  downloaded what plans the new runs, so the new runs' cells, stamps and kinds reuse that tail.
 */
template <typename key_slot_type_>
struct cuda_compaction_context {
    using record_t = located_key_slot<key_slot_type_>;

    /** Every record in merged order, selected in place down to the survivors. */
    record_t* kept = nullptr;

    /** Where the runs extract to before one merge path joins them into @c kept, and where folded survivors upload,
     *  or null where nothing merges or folds. */
    record_t* extracted = nullptr;

    /** Where the runs' sections start, each run's carved as @c carve_sections carves it. */
    std::uintptr_t sections = 0;

    /** The saved stamps and kinds of the runs uploading from memory. */
    std::uint64_t* saved = nullptr;

    /** One survival flag per record. */
    std::uint8_t* flags = nullptr;

    /** One flag per record telling whether it carries the key of the one before it. */
    std::uint8_t* shares = nullptr;

    /** The survivors' value offsets, where the index of every folded survivor uploads first, null for values inline
     *  that nothing folds. */
    std::uint64_t* prefix = nullptr;

    /** The pinned stamps. */
    std::uint64_t* pinned = nullptr;

    /** How many records survive. */
    std::int64_t* selected = nullptr;

    /** CUB's temporary bytes, enough for its largest call. */
    std::byte* temporary = nullptr;

    /** Bytes of @c temporary. */
    std::size_t temporary_bytes = 0;

    /** Where the dead tail starts, past the survivors. */
    std::uintptr_t tail = 0;

    /** One past the last byte carved. */
    std::uintptr_t end = 0;
};

#pragma endregion Context

#pragma region Merge

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
st::status_t fold_survivors(cuda_executor_t& executor, compaction_kind_t kind, compaction_stamps_t const& stamps,
                            std::uint64_t sorted_runs_count, std::uint64_t value_stride,
                            located_key_slot<key_slot_type_> const* merged, std::uint8_t const* flags,
                            std::uint64_t merged_count, fold_type_ const& fold, device_fold_type_ const& device_fold,
                            located_value_reader<key_slot_type_> read_value,
                            compaction_output<key_slot_type_, allocator_type_>& output,
                            std::span<std::uint64_t>& indices,
                            std::span<located_key_slot<key_slot_type_>>& replacements) noexcept {
    using st::basic_vector;
    using st::status_t;
    using record_t = located_key_slot<key_slot_type_>;

    record_t* const records = carve_bounce<record_t>(executor, static_cast<std::size_t>(merged_count));
    std::uint8_t* const kept = carve_bounce<std::uint8_t>(executor, static_cast<std::size_t>(merged_count));
    if (!records || !kept || executor.pin_bounce().failed()) return status_t::out_of_memory_heap_k;
    status_t status = download(executor, records, merged, static_cast<std::size_t>(merged_count));
    if (status != status_t::success_k) return status;
    status = download(executor, kept, flags, static_cast<std::size_t>(merged_count));
    if (status != status_t::success_k) return status;
    status = executor_status(executor.wait());
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
        folded_versions_t const versions = folded_versions(kind, stamps, records, kept, merged_count, head);
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
        status = fold_versions(fold, records, head, versions, value_of, arena_bytes, folded);
        if (status != status_t::success_k) return status;
        status = folds.push_back(folded_t {ordinal, head, folded.kind, folded.bytes});
        if (status != status_t::success_k) return status;
    }

    basic_vector<byte_t> composed;
    basic_vector<std::uint64_t> composed_offsets;
    if (chain_starts.size() > 1) {
        status = device_fold.compose(executor, changes, chain_starts, composed, composed_offsets);
        if (status != status_t::success_k) return status;
    }

    // A folded value replaces its survivor's own: inline in the key slot, or in the compaction's folded bytes
    std::uint64_t* const folded_indices = carve_bounce<std::uint64_t>(executor, folds.size());
    record_t* const folded_records = carve_bounce<record_t>(executor, folds.size());
    if (!folds.empty() && (!folded_indices || !folded_records || executor.pin_bounce().failed()))
        return status_t::out_of_memory_heap_k;
    indices = {folded_indices, folds.size()};
    replacements = {folded_records, folds.size()};
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
st::status_t cuda_compactor_t::compact(
    compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_type_> const> upper,
    std::span<sorted_run_index_view<key_slot_type_> const> lower, std::span<sorted_run_extents_t const> extents,
    sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps, [[maybe_unused]] fold_type_ const& fold,
    [[maybe_unused]] device_fold_type_ const& device_fold,
    [[maybe_unused]] std::type_identity_t<located_value_reader<key_slot_type_>> read_value,
    compaction_output<key_slot_type_, allocator_type_>& output) noexcept {
    using st::basic_vector;
    using st::status_t;
    using record_t = located_key_slot<key_slot_type_>;
    using view_t = sorted_run_index_view<key_slot_type_>;
    constexpr bool device_folds_k = !std::is_same_v<device_fold_type_, no_device_fold_t>;
    constexpr bool locates_k = locates_stored_values_concept<key_slot_type_>;
    constexpr st::assume_uninitialized_t uninitialized {};

    if (!admits_sorted_runs(bounds)) return status_t::invalid_argument_k;
    std::uint64_t const sorted_runs_count = upper.size() + lower.size();
    if (sorted_runs_count > std::numeric_limits<std::uint32_t>::max() || extents.size() != sorted_runs_count)
        return status_t::invalid_argument_k;
    if (!views_locate_values(upper, bounds) || !views_locate_values(lower, bounds)) return status_t::invalid_argument_k;
    auto const view_of = [&](std::uint64_t run) noexcept -> view_t const& {
        return run < upper.size() ? upper[run] : lower[run - upper.size()];
    };

    std::uint64_t upper_key_slots = 0;
    std::uint64_t merged_count = 0;
    std::size_t saved_count = 0;
    bool changes = false;
    for (std::uint64_t run = 0; run != sorted_runs_count; ++run) {
        view_t const& view = view_of(run);
        changes |= holds_changes(view.kinds);
        merged_count += view.key_slots_count;
        if (run < upper.size()) upper_key_slots += view.key_slots_count;
    }
    // Without a device fold, changes fold on the host alone, where every merge holding one runs.
    if (!device_folds_k && changes) return status_t::invalid_argument_k;
    output.folded.clear();

    if (executor_->bind().failed()) return status_t::unknown_k;
    executor_->rewind_bounce();
    cudaStream_t const stream = reinterpret_cast<cudaStream_t>(executor_->stream());

    // Every region and CUB's largest temporary are sized before anything queues, so the scratch grows once at most.
    survivor_first_t const order {static_cast<std::uint32_t>(upper.size())};
    bool const overlapping = (kind == compaction_kind_t::overlapping_into_intermediate_k) |
                             (kind == compaction_kind_t::overlapping_into_bottom_k);
    bool const sorts = overlapping & (upper_key_slots > 1);
    bool const merges = (upper_key_slots != 0) & (merged_count != upper_key_slots);
    bool const folds = device_folds_k && changes;
    std::int64_t const merged_items = static_cast<std::int64_t>(merged_count);
    std::size_t temporary_bytes = 0;
    if (merged_count != 0) {
        std::size_t bytes = 0;
        record_t* const none = nullptr;
        std::int64_t* const no_count = nullptr;
        std::uint64_t* const no_sums = nullptr;
        std::uint8_t* const no_flags = nullptr;
        if (sorts) {
            status_t const sized = cuda_status(cub::DeviceMergeSort::SortKeys(
                nullptr, bytes, none, static_cast<std::int64_t>(upper_key_slots), order, stream));
            if (sized != status_t::success_k) return sized;
            temporary_bytes = std::max(temporary_bytes, bytes);
        }
        if (merges) {
            status_t const sized = cuda_status(cub::DeviceMerge::MergeKeys(
                nullptr, bytes, none, static_cast<std::int64_t>(upper_key_slots), none,
                static_cast<std::int64_t>(merged_count - upper_key_slots), none, order, stream));
            if (sized != status_t::success_k) return sized;
            temporary_bytes = std::max(temporary_bytes, bytes);
        }
        status_t sized = cuda_status(
            cub::DeviceSelect::Flagged(nullptr, bytes, none, no_flags, no_count, merged_items, stream));
        if (sized != status_t::success_k) return sized;
        temporary_bytes = std::max(temporary_bytes, bytes);
        if constexpr (locates_k) {
            sized = cuda_status(cub::DeviceScan::InclusiveSum(nullptr, bytes, no_sums, merged_items, stream));
            if (sized != status_t::success_k) return sized;
            temporary_bytes = std::max(temporary_bytes, bytes);
        }
    }

    bool const reads_volumes = static_cast<bool>(sections_);
    auto const carve = [&](scratch_cursor_t& cursor, cuda_compaction_context<key_slot_type_>& context) noexcept {
        context.kept = cursor.take<record_t>(merged_count);
        context.tail = cursor.at;
        context.extracted = merges | folds ? cursor.take<record_t>(merged_count) : nullptr;
        context.sections = cursor.at;
        saved_count = 0;
        for (std::uint64_t run = 0; run != sorted_runs_count; ++run) {
            view_t const& view = view_of(run);
            if (!carve_sections(view, extents[run], bounds, reads_volumes, cursor).reads_volume)
                saved_count += saved_words(view);
        }
        context.saved = cursor.take<std::uint64_t>(saved_count);
        context.flags = cursor.take<std::uint8_t>(merged_count);
        context.shares = cursor.take<std::uint8_t>(merged_count);
        context.prefix = locates_k | folds ? cursor.take<std::uint64_t>(merged_count + 1) : nullptr;
        context.pinned = cursor.take<std::uint64_t>(static_cast<std::size_t>(stamps.pinned_count));
        context.selected = cursor.take<std::int64_t>(1);
        context.temporary = cursor.take<std::byte>(temporary_bytes);
        context.temporary_bytes = temporary_bytes;
        context.end = cursor.at;
    };
    cuda_compaction_context<key_slot_type_> context;
    scratch_cursor_t sizing;
    carve(sizing, context);
    std::size_t const page = static_cast<std::size_t>(utilities::portable_alignment_k);
    std::size_t const needed = static_cast<std::size_t>(context.end) + page;
    if (executor_->scratch_limit() != 0 && needed > executor_->scratch_limit()) return status_t::out_of_memory_heap_k;
    if (status_t const grown = reserve_(needed, 0); grown != status_t::success_k) return grown;
    std::uintptr_t base = round_up_to_multiple<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(scratch_), page);
    scratch_cursor_t carving {base};
    carve(carving, context);

    // The reads land first, so no kernel runs while a run's sections are in flight
    std::uint64_t* const saved = carve_bounce<std::uint64_t>(*executor_, saved_count);
    if (saved_count != 0 && !saved) return status_t::out_of_memory_heap_k;
    reads_.clear();
    scratch_cursor_t sections_cursor {context.sections};
    std::size_t saved_offset = 0;
    for (std::uint64_t run = 0; run != sorted_runs_count; ++run) {
        view_t const& view = view_of(run);
        run_sections<key_slot_type_> const sections = carve_sections(view, extents[run], bounds, reads_volumes,
                                                                     sections_cursor);
        sorted_run_extents_t const& run_extents = extents[run];
        auto const into = [&](void const* target) noexcept {
            return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(target) -
                                              reinterpret_cast<std::uintptr_t>(scratch_));
        };
        if (sections.reads_volume) {
            for (device_section_read_t const read :
                 {device_section_read_t {run_extents.volume, run_extents.cells, into(sections.cells)},
                  device_section_read_t {run_extents.volume, run_extents.value_offsets, into(sections.value_offsets)},
                  device_section_read_t {run_extents.volume, run_extents.trailer, into(sections.trailer)}})
                if (!read.extent.empty())
                    if (reads_.push_back(device_section_read_t {read}) != status_t::success_k)
                        return status_t::out_of_memory_heap_k;
            continue;
        }
        std::size_t const cells_count = static_cast<std::size_t>(
            round_up_to_multiple(view.key_slots_count, bounds.branching));
        status_t status = upload(*executor_, sections.cells, view.cells, cells_count);
        if (status != status_t::success_k) return status;
        status = upload(*executor_, sections.value_offsets, view.value_offsets.data(), view.value_offsets.size());
        if (status != status_t::success_k) return status;
        if (view.key_slots_count == 0) continue;
        if (view.stamps != nullptr) {
            std::size_t const bytes = view.stamps->saved_bytes();
            if (view.stamps->save({reinterpret_cast<std::byte*>(saved + saved_offset), bytes}) != status_t::success_k)
                return status_t::invalid_argument_k;
            saved_offset += bytes / sizeof(std::uint64_t);
        }
        if (view.kinds != nullptr) {
            std::size_t const bytes = view.kinds->saved_bytes();
            if (view.kinds->save({reinterpret_cast<std::byte*>(saved + saved_offset), bytes}) != status_t::success_k)
                return status_t::invalid_argument_k;
            saved_offset += bytes / sizeof(std::uint64_t);
        }
    }
    if (executor_->pin_bounce().failed()) return status_t::out_of_memory_heap_k;
    status_t status = upload(*executor_, context.saved, saved, saved_count);
    if (status != status_t::success_k) return status;
    if (!reads_.empty()) {
        status = sections_.read(reads_, std::span<byte_t> {scratch_, scratch_bytes_});
        if (status != status_t::success_k) return status;
    }

    // The pinned stamps are a few words, read by every survival check, so the device holds its own copy.
    compaction_stamps_t device_stamps = stamps;
    if (stamps.pinned_count != 0) {
        status = upload(*executor_, context.pinned, stamps.pinned, static_cast<std::size_t>(stamps.pinned_count));
        if (status != status_t::success_k) return status;
        device_stamps.pinned = context.pinned;
    }

    // Every run's cells land at their sorted positions, then its saved stamps and kinds unpack over them
    sections_cursor = scratch_cursor_t {context.sections};
    record_t* const extracted = merges ? context.extracted : context.kept;
    saved_offset = 0;
    std::uint64_t run_offset = 0;
    for (std::uint64_t run = 0; run != sorted_runs_count; ++run) {
        view_t const& view = view_of(run);
        run_sections<key_slot_type_> const sections = carve_sections(view, extents[run], bounds, reads_volumes,
                                                                     sections_cursor);
        sorted_run_index_layout_t const layout(view.key_slots_count, bounds.branching);
        if (layout.cells_count() == 0) continue;
        record_t* const records = extracted + run_offset;
        run_offset += view.key_slots_count;
        extract_kernel<key_slot_type_> const extracting {
            sections.cells,     records, layout, static_cast<std::uint32_t>(run), sections.value_offsets,
            bounds.value_stride};
        status = cuda_status(cub::DeviceFor::Bulk(static_cast<std::int64_t>(layout.cells_count()), extracting, stream));
        if (status != status_t::success_k) return status;
        std::uint64_t const* saved_stamps = nullptr;
        std::uint64_t const* saved_kinds = nullptr;
        if (sections.reads_volume) {
            saved_stamps = sections.trailer;
            saved_kinds = sections.trailer + extents[run].kinds_begin / sizeof(std::uint64_t);
        }
        else {
            if (view.stamps != nullptr) {
                saved_stamps = context.saved + saved_offset;
                saved_offset += view.stamps->saved_bytes() / sizeof(std::uint64_t);
            }
            if (view.kinds != nullptr) {
                saved_kinds = context.saved + saved_offset;
                saved_offset += view.kinds->saved_bytes() / sizeof(std::uint64_t);
            }
        }
        std::int64_t const count = static_cast<std::int64_t>(view.key_slots_count);
        if (saved_stamps != nullptr) {
            unpack_stamps_kernel<key_slot_type_> const unpacking {saved_stamps, records, extents[run].assigned_stamp};
            status = cuda_status(cub::DeviceFor::Bulk(count, unpacking, stream));
            if (status != status_t::success_k) return status;
        }
        if (saved_kinds != nullptr) {
            unpack_kinds_kernel<key_slot_type_> const unpacking {saved_kinds, records};
            status = cuda_status(cub::DeviceFor::Bulk(count, unpacking, stream));
            if (status != status_t::success_k) return status;
        }
    }

    // Overlapping upper key slots sort so the newest of every stored key leads its run, then one merge path joins
    if (sorts) {
        std::size_t bytes = context.temporary_bytes;
        status = cuda_status(cub::DeviceMergeSort::SortKeys(context.temporary, bytes, extracted,
                                                            static_cast<std::int64_t>(upper_key_slots), order, stream));
        if (status != status_t::success_k) return status;
    }
    if (merges) {
        std::size_t bytes = context.temporary_bytes;
        std::int64_t const upper_count = static_cast<std::int64_t>(upper_key_slots);
        status = cuda_status(cub::DeviceMerge::MergeKeys(context.temporary, bytes, context.extracted, upper_count,
                                                         context.extracted + upper_count, merged_items - upper_count,
                                                         context.kept, order, stream));
        if (status != status_t::success_k) return status;
    }

    // The first record of every stored key survives, unless it is a removal where removals drop
    std::int64_t selected_count = 0;
    std::int64_t* selected = nullptr;
    std::uint64_t* prefixes = nullptr;
    std::uint8_t* shares = nullptr;
    if (merged_count != 0) {
        survivor_kernel<key_slot_type_> const flagging {context.kept, context.flags, kind, device_stamps};
        status = cuda_status(cub::DeviceFor::Bulk(merged_items, flagging, stream));
        if (status != status_t::success_k) return status;
        std::span<std::uint64_t> indices;
        std::span<record_t> replacements;
        if constexpr (device_folds_k)
            if (changes) {
                status = fold_survivors(*executor_, kind, stamps, sorted_runs_count, bounds.value_stride, context.kept,
                                        context.flags, merged_count, fold, device_fold, read_value, output, indices,
                                        replacements);
                if (status != status_t::success_k) return status;
            }
        std::size_t bytes = context.temporary_bytes;
        status = cuda_status(cub::DeviceSelect::Flagged(context.temporary, bytes, context.kept, context.flags,
                                                        context.selected, merged_items, stream));
        if (status != status_t::success_k) return status;
        // The merged copy and the offsets are dead once selection ran, so the folded survivors upload into them.
        if (!indices.empty()) {
            status = upload(*executor_, context.prefix, indices.data(), indices.size());
            if (status != status_t::success_k) return status;
            status = upload(*executor_, context.extracted, replacements.data(), replacements.size());
            if (status != status_t::success_k) return status;
            folded_survivor_kernel<key_slot_type_> const replacing {context.kept, context.prefix, context.extracted};
            status = cuda_status(cub::DeviceFor::Bulk(static_cast<std::int64_t>(indices.size()), replacing, stream));
            if (status != status_t::success_k) return status;
        }

        std::uint64_t* const prefix = locates_k ? context.prefix : nullptr;
        survivor_sizes_kernel<key_slot_type_> const sizing_survivors {context.kept, context.selected, prefix,
                                                                      context.shares};
        status = cuda_status(cub::DeviceFor::Bulk(merged_items, sizing_survivors, stream));
        if (status != status_t::success_k) return status;
        if (prefix != nullptr) {
            bytes = context.temporary_bytes;
            status = cuda_status(
                cub::DeviceScan::InclusiveSum(context.temporary, bytes, prefix + 1, merged_items, stream));
            if (status != status_t::success_k) return status;
        }

        // What plans the new runs comes back at once, sized by the records, as only the device counts survivors yet
        selected = carve_bounce<std::int64_t>(*executor_, 1);
        prefixes = locates_k ? carve_bounce<std::uint64_t>(*executor_, static_cast<std::size_t>(merged_count + 1))
                             : nullptr;
        shares = carve_bounce<std::uint8_t>(*executor_, static_cast<std::size_t>(merged_count));
        if (!selected || !shares || (locates_k && !prefixes) || executor_->pin_bounce().failed())
            return status_t::out_of_memory_heap_k;
        if ((status = download(*executor_, selected, context.selected, 1)) != status_t::success_k) return status;
        if (locates_k)
            if ((status = download(*executor_, prefixes, prefix, static_cast<std::size_t>(merged_count + 1))) !=
                status_t::success_k)
                return status;
        if ((status = download(*executor_, shares, context.shares, static_cast<std::size_t>(merged_count))) !=
            status_t::success_k)
            return status;
        if ((status = executor_status(executor_->wait())) != status_t::success_k) return status;
        selected_count = *selected;
    }
    else if (locates_k) {
        prefixes = carve_bounce<std::uint64_t>(*executor_, 1);
        if (!prefixes) return status_t::out_of_memory_heap_k;
        prefixes[0] = 0;
    }
    std::uint64_t const survivors = static_cast<std::uint64_t>(selected_count);

    auto const read_prefix = [&](std::uint64_t index, std::uint64_t& bytes) noexcept {
        bytes = locates_k ? prefixes[static_cast<std::size_t>(index)] : 0;
        return status_t::success_k;
    };
    auto const shares_key = [&](std::uint64_t index, bool& shared) noexcept {
        shared = shares[static_cast<std::size_t>(index)] != 0;
        return status_t::success_k;
    };
    status = plan_sorted_runs<key_slot_type_>(survivors, bounds, read_prefix, shares_key, output.sorted_runs);
    if (status != status_t::success_k) return status;

    std::size_t const placements_count = output.sorted_runs.size();
    sorted_run_placement_t* const host_placements = carve_bounce<sorted_run_placement_t>(*executor_, placements_count);
    if (placements_count != 0 && !host_placements) return status_t::out_of_memory_heap_k;
    std::uint64_t first = 0;
    std::uint64_t cells_count = 0;
    for (std::size_t index = 0; index != placements_count; ++index) {
        std::uint64_t const count = output.sorted_runs[index].key_slots_count;
        host_placements[index] = sorted_run_placement_t {first, cells_count,
                                                         sorted_run_index_layout_t(count, bounds.branching)};
        first += count;
        cells_count += round_up_to_multiple(count, bounds.branching);
    }

    // The new runs take the tail the survivors leave dead, which only a plan of many tiny runs outgrows
    sorted_run_placement_t* placements = nullptr;
    key_slot_type_* cells = nullptr;
    std::uint64_t* survivor_stamps = nullptr;
    std::uint8_t* survivor_kinds = nullptr;
    auto const carve_output = [&](scratch_cursor_t& placing) noexcept {
        placements = placing.take<sorted_run_placement_t>(placements_count);
        cells = placing.take<key_slot_type_>(static_cast<std::size_t>(cells_count));
        survivor_stamps = placing.take<std::uint64_t>(static_cast<std::size_t>(survivors));
        survivor_kinds = placing.take<std::uint8_t>(static_cast<std::size_t>(survivors));
    };
    std::uintptr_t const tail_offset = context.tail - base;
    scratch_cursor_t placing {context.tail};
    carve_output(placing);
    if (placing.at > reinterpret_cast<std::uintptr_t>(scratch_) + scratch_bytes_) {
        std::size_t const grown = static_cast<std::size_t>(placing.at - base) + page;
        if (executor_->scratch_limit() != 0 && grown > executor_->scratch_limit())
            return status_t::out_of_memory_heap_k;
        status = reserve_(grown, static_cast<std::size_t>(survivors * sizeof(record_t)));
        if (status != status_t::success_k) return status;
        base = round_up_to_multiple<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(scratch_), page);
        context.kept = reinterpret_cast<record_t*>(base);
        placing = scratch_cursor_t {base + tail_offset};
        carve_output(placing);
    }

    std::size_t const placed_count = locates_k ? static_cast<std::size_t>(survivors) : 0;
    record_t* const placed = carve_bounce<record_t>(*executor_, placed_count);
    if ((placed_count != 0 && !placed) || executor_->pin_bounce().failed()) return status_t::out_of_memory_heap_k;
    status = upload(*executor_, placements, host_placements, placements_count);
    if (status != status_t::success_k) return status;
    if (cells_count != 0) {
        padding_kernel<key_slot_type_> const padding {cells, padding_key_slot<key_slot_type_>()};
        status = cuda_status(cub::DeviceFor::Bulk(static_cast<std::int64_t>(cells_count), padding, stream));
        if (status != status_t::success_k) return status;
    }
    if (survivors != 0) {
        place_kernel<key_slot_type_> const placing_survivors {context.kept, placements,      placements_count, cells,
                                                              stamps.floor, survivor_stamps, survivor_kinds};
        status = cuda_status(cub::DeviceFor::Bulk(static_cast<std::int64_t>(survivors), placing_survivors, stream));
        if (status != status_t::success_k) return status;
    }

    if (output.cells.resize(uninitialized, cells_count) != status_t::success_k ||
        output.stamps.resize(uninitialized, survivors) != status_t::success_k ||
        output.kinds.resize(uninitialized, survivors) != status_t::success_k)
        return status_t::out_of_memory_heap_k;
    std::size_t const survivors_count = static_cast<std::size_t>(survivors);
    if ((status = download(*executor_, output.cells.data(), cells, static_cast<std::size_t>(cells_count))) !=
        status_t::success_k)
        return status;
    if ((status = download(*executor_, output.stamps.data(), survivor_stamps, survivors_count)) != status_t::success_k)
        return status;
    if ((status = download(*executor_, output.kinds.data(), survivor_kinds, survivors_count)) != status_t::success_k)
        return status;
    if ((status = download(*executor_, placed, static_cast<record_t const*>(context.kept), placed_count)) !=
        status_t::success_k)
        return status;
    if ((status = executor_status(executor_->wait())) != status_t::success_k) return status;

    // The prefix is the new runs' value offsets, and the copy plan coalesces the survivors' sources on the host.
    output.value_offsets.clear();
    output.value_copy_ranges.clear();
    if constexpr (locates_k) {
        if (is_variable_width(key_slot_type_::value_layout_k)) {
            if (output.value_offsets.resize(uninitialized, survivors + 1) != status_t::success_k)
                return status_t::out_of_memory_heap_k;
            std::memcpy(output.value_offsets.data(), prefixes, (survivors + 1) * sizeof(std::uint64_t));
        }
        return plan_value_copies<key_slot_type_>(placed, upper, lower, bounds.value_stride, output.sorted_runs,
                                                 output.value_copy_ranges);
    }
    return status_t::success_k;
}

#pragma endregion Merge

} // namespace unum::ustore::blobs

#endif
