/**
 *  @file include/ustore/blobs/vram_apple_compaction.hpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The Apple device compactor: what a database makes once at open over a Metal executor and
 *      hands every merge the tree sends to the device, and the host code queueing its kernels, the
 *      MSL of @c vram_compaction_apple.metal.
 *
 *  An Apple device has no 64-bit atomics to chain with, so the pack follows the three passes of the
 *  select and a last launch adds the bytes of the tiles before each to its value offsets. Every
 *  device region a merge needs is carved off the compactor's own device blocks, rewound per merge,
 *  and every host buffer off the executor's pinned bounce. @c src/nvme/compaction.cpp alone
 *  instantiates the bodies below, and @c src/nvme/compaction_variants.hpp declares those
 *  instantiations to every other unit.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <span>        // `std::span`
#include <type_traits> // `std::type_identity_t`
#include <utility>     // `std::exchange`, `std::move`, `std::swap`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <smashtable/shared.hpp>       // `st::status_t`

#include "ustore/blobs/nvme_compaction.hpp"        // `compaction_output`, `merge_pack_regions`
#include "ustore/blobs/vram_nvidia_compaction.cuh" // `merge_run`, `carve_sections`, `fold_survivors`, `launch_over`
#include "ustore/utilities/folds.hpp"              // `fold_concept`
#include "ustore/utilities/ram_buffers.hpp"        // `device_blocks_t`
#include "ustore/utilities/types.hpp"              // `expected`
#include "ustore/utilities/vram_apple_device.hpp"  // `metal_executor_t`
#include "ustore/utilities/vram_device.hpp"        // `launch`, `carve_runs`, `rewind`
#include "ustore/utilities/vram_scan.cuh"          // `count_flagged`

namespace unum::ustore::blobs {

using utilities::bytes_of;

/**
 *  @brief The device compactor a database's merges run on an Apple device, made once at open over
 *      a Metal executor the database owns.
 *
 *  Every merge queues on the executor's stream and carves its regions off device runs the compactor
 *  owns, rewound per merge to the newest and largest, while the host buffers its copies leave from
 *  and land in are carved out of the executor's pinned bounce, rewound per merge, so a warm
 *  compactor allocates nothing. Run sections read by extent through its sections reader where one
 *  reaches the volumes, and upload from the runs' memory otherwise. Its kernels live in the
 *  library's bundles, launched through the driver by the bodies below, and pack after the passes of
 *  a select, as the device has no 64-bit atomics to chain a scan with.
 */
class metal_compactor_t {
  public:

    /** Binds a compactor to @p executor, which outlives it, reading run sections
     *  through @p sections; refused with @c unsupported_k where the executor's device
     *  loaded no bundle of the library's kernels. */
    static expected<metal_compactor_t> make(utilities::metal_executor_t& executor,
                                            device_sections_reader_t sections = {}) noexcept;

    /** Holds nothing, as a moved-from compactor does. */
    metal_compactor_t() noexcept = default;
    metal_compactor_t(metal_compactor_t&& other) noexcept
        : executor_(other.executor_), sections_(other.sections_), blocks_(std::exchange(other.blocks_, {})),
          device_allocations_(std::exchange(other.device_allocations_, 0)), reads_(std::move(other.reads_)) {}
    metal_compactor_t& operator=(metal_compactor_t&& other) noexcept {
        std::swap(executor_, other.executor_), std::swap(sections_, other.sections_);
        std::swap(blocks_, other.blocks_), std::swap(device_allocations_, other.device_allocations_);
        std::swap(reads_, other.reads_);
        return *this;
    }
    metal_compactor_t(metal_compactor_t const&) = delete;
    metal_compactor_t& operator=(metal_compactor_t const&) = delete;
    ~metal_compactor_t() noexcept;

    /**
     *  @brief Does what @c host_compactor_t::compact does with identical output, composing the
     *      chains of changes alone through @p device_fold and applying the rest on the host
     *      through @p fold.
     *
     *  The host applies every chain that reaches a write, a removal or the bottom
     *  level's empty base.
     *
     *  The @p extents of every run, @p upper ones first, say where its sections sit on its volume
     *  and which stamp an ingest gave it. With @c no_device_fold_t the host folds every chain
     *  through @p fold, and with no change to fold the folds and @p read_value go unused. A merge
     *  whose scratch would pass the executor's limit, or whose planned runs outgrow what the
     *  merge leaves free, refuses with @c out_of_memory_heap_k, so the tree merges on the host.
     */
    template <typename key_slot_type_, utilities::fold_concept fold_type_, typename device_fold_type_,
              typename allocator_type_>
        requires compactable_key_slot_concept<key_slot_type_>
    st::status_t compact(compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_type_> const> upper,
                         std::span<sorted_run_index_view<key_slot_type_> const> lower,
                         std::span<sorted_run_extents_t const> extents, sorted_run_bounds_t const& bounds,
                         compaction_stamps_t const& stamps, fold_type_ const& fold,
                         device_fold_type_ const& device_fold,
                         std::type_identity_t<located_value_reader<key_slot_type_>> read_value,
                         compaction_output<key_slot_type_, allocator_type_>& output) noexcept;

    /**
     *  @brief Lists the versions of @p runs that survive their merge, in merged order, as the
     *      host's key unions and @c survives list them, ranked on the device by head and tape,
     *      where the survivors pack, so they alone come back.
     *  @param[in] runs The upper runs, oldest first, then the lower runs, ascending.
     *  @param[in] upper_runs How many of @p runs are upper ones.
     *  @return Success, or @c invalid_argument_k where the device loaded no kernels or a run
     *      outgrows what a version names, and @c out_of_memory_heap_k where the merge outgrows the
     *      scratch, both of which the host takes over.
     */
    st::status_t merge_strings(compaction_kind_t kind, std::span<strings_run_view const> runs, std::uint32_t upper_runs,
                               compaction_stamps_t const& stamps, st::vector<strings_version_t>& survivors) noexcept;

    /** Device allocations the compactor made, which stop once its scratch holds the largest
     *  merge it serves. */
    std::uint64_t device_allocations() const noexcept { return device_allocations_; }

  private:
    /** As @c compact, for a key slot the bundles carry merge kernels for. */
    template <typename key_slot_type_, utilities::fold_concept fold_type_, typename device_fold_type_,
              typename allocator_type_>
        requires compactable_key_slot_concept<key_slot_type_>
    st::status_t compact_on_device_(compaction_kind_t kind,
                                    std::span<sorted_run_index_view<key_slot_type_> const> upper,
                                    std::span<sorted_run_index_view<key_slot_type_> const> lower,
                                    std::span<sorted_run_extents_t const> extents, sorted_run_bounds_t const& bounds,
                                    compaction_stamps_t const& stamps, fold_type_ const& fold,
                                    device_fold_type_ const& device_fold,
                                    std::type_identity_t<located_value_reader<key_slot_type_>> read_value,
                                    compaction_output<key_slot_type_, allocator_type_>& output) noexcept;

    /** Queues the pack of kernel @p name over @p regions after the count and scan passes of a
     *  select, whose 64-bit prefixes of 32-bit tile counts hand every tile of the records the
     *  survivors before it, as NVIDIA's chain does; answers where the survivors' count lands. */
    template <typename record_type_>
    st::expected<packed_counts_t> pack_(char const* name, merge_pack_regions<record_type_> const& regions) noexcept;

    metal_compactor_t(utilities::metal_executor_t& executor, device_sections_reader_t sections) noexcept
        : executor_(&executor), sections_(sections) {}

    /** Carves @p count elements off the compactor's device blocks, counting the runs it opens, null
     *  when it cannot. */
    template <typename element_type_>
    element_type_* carve_(std::size_t count) noexcept;

    /** The executor every merge queues on and allocates through, borrowed. */
    utilities::metal_executor_t* executor_ = nullptr;

    /** The transport run sections read through, empty where every run uploads from its memory. */
    device_sections_reader_t sections_;

    /** The device runs every merge carves its regions from, owned, rewound per merge to the
     *  newest and largest. */
    utilities::device_blocks_t blocks_ {};

    /** Device allocations the blocks made. */
    std::uint64_t device_allocations_ = 0;

    /** The sections the merge reads through @c sections_. */
    st::vector<device_section_read_t> reads_;
};

#pragma region Merge

template <typename element_type_>
element_type_* metal_compactor_t::carve_(std::size_t count) noexcept {
    std::uint32_t const held = blocks_.count;
    auto const [carved, status] = utilities::carve_runs<element_type_>(
        blocks_, *executor_, count, [&](std::size_t bytes) noexcept { return executor_->allocate_async(bytes); });
    device_allocations_ += blocks_.count - held;
    return status.failed() ? nullptr : carved.data();
}

template <typename record_type_>
st::expected<packed_counts_t> metal_compactor_t::pack_(char const* name,
                                                       merge_pack_regions<record_type_> const& regions) noexcept {
    using st::status_t;
    // Few enough tiles that each sums the bytes of the ones before it, as many as a chain's blocks
    std::uint64_t const tile = round_up_to_multiple(
        divide_round_up(regions.records_count, utilities::chain_blocks_max_k), std::uint64_t {256});
    std::uint32_t const tiles = static_cast<std::uint32_t>(divide_round_up(regions.records_count, tile));
    std::uint64_t* const counts = carve_<std::uint64_t>(std::size_t {tiles} + 1);
    std::uint64_t* const tile_bytes = carve_<std::uint64_t>(tiles);
    std::uint64_t* const survivors_count = carve_<std::uint64_t>(1);
    if (!counts || !tile_bytes || !survivors_count) return status_t::out_of_memory_heap_k;
    status_t status = executor_status(utilities::count_flagged(*executor_, regions.flags, regions.records_count,
                                                               static_cast<std::uint32_t>(tile), tiles, counts));
    if (status != status_t::success_k) return status;
    status = executor_status(utilities::launch(
        *executor_, executor_->kernels()->function(name), tiles, merge_block_threads_k, 0u, regions.merged,
        regions.flags, regions.records_count, regions.replaced, regions.replacements, regions.replaced_count,
        static_cast<std::uint64_t const*>(counts), static_cast<std::uint32_t>(tile), tiles, regions.survivors,
        regions.prefix, regions.shares, tile_bytes, survivors_count));
    if (status != status_t::success_k) return status;
    // Value offsets scan within each tile, so every tile past the first adds the bytes before it
    if (regions.prefix != nullptr && tiles > 1)
        status = executor_status(
            utilities::launch(*executor_, executor_->kernels()->function("ustore_merge_offsets_apple"), tiles,
                              merge_block_threads_k, 0u, static_cast<std::uint64_t const*>(counts), tiles,
                              static_cast<std::uint64_t const*>(tile_bytes), regions.prefix));
    if (status != status_t::success_k) return status;
    return packed_counts_t {survivors_count};
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
    // A key slot the bundles carry no kernels for merges on the host.
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
    sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps, fold_type_ const& fold,
    device_fold_type_ const& device_fold, std::type_identity_t<located_value_reader<key_slot_type_>> read_value,
    compaction_output<key_slot_type_, allocator_type_>& output) noexcept {
    using st::status_t;
    using record_t = located_key_slot<key_slot_type_>;
    using view_t = sorted_run_index_view<key_slot_type_>;
    using names_t = merge_kernel_names<key_slot_type_>;
    using range_t = range_tombstone<typename key_slot_type_::stored_key_t>;
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
    std::size_t saved_count = 0, ranges_count = 0;
    bool changes = false;
    sections_source_t const preferred = sections_ ? sections_source_t::volume_k : sections_source_t::memory_k;
    scratch_cursor_t sizing;
    for (std::uint64_t run = 0; run != sorted_runs_count; ++run) {
        view_t const& view = view_of(run);
        changes |= holds_changes(view.kinds);
        merged_count += view.key_slots_count;
        if (view.range_tombstones != nullptr) ranges_count += view.range_tombstones->size();
        if (carve_sections(view, extents[run], preferred, sizing).source == sections_source_t::memory_k)
            saved_count += saved_words(view);
    }
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
    range_t* const ranges = carve_<range_t>(ranges_count);
    std::uint64_t* const range_stamps = carve_<std::uint64_t>(ranges_count);
    if ((sorted_runs_count && (!runs || !gathers)) || (saved_count && !saved_columns) ||
        (stamps.pinned_count && !pinned) || (ranges_count && (!ranges || !range_stamps)) ||
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
    std::size_t saved_offset = 0, ranges_offset = 0;
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
        // A run's range tombstones stay in memory wherever its sections sit, so they upload
        if (view.range_tombstones != nullptr && !view.range_tombstones->empty()) {
            std::size_t const count = view.range_tombstones->size();
            described.range_tombstones = {ranges + ranges_offset, range_stamps + ranges_offset, count};
            status = upload(*executor_, ranges + ranges_offset, view.range_tombstones->ranges().data(), count);
            if (status != status_t::success_k) return status;
            status = upload(*executor_, range_stamps + ranges_offset, view.range_tombstones->stamps().data(), count);
            if (status != status_t::success_k) return status;
            ranges_offset += count;
        }
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
            if (view.stamps->save(bytes_of(saved + saved_offset, bytes / sizeof(std::uint64_t))) != status_t::success_k)
                return status_t::invalid_argument_k;
            described.saved_stamps = saved_columns + saved_offset;
            saved_offset += bytes / sizeof(std::uint64_t);
        }
        if (view.kinds != nullptr) {
            std::size_t const bytes = view.kinds->saved_bytes();
            if (view.kinds->save(bytes_of(saved + saved_offset, bytes / sizeof(std::uint64_t))) != status_t::success_k)
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
        if (changes)
            status = fold_survivors(*executor_, blocks_, kind, stamps, sorted_runs_count, bounds.value_stride, merged,
                                    flags, merged_count, fold, device_fold, read_value, output, indices, replacements);
        if (status != status_t::success_k) return status;
        std::uint64_t* const replaced = carve_<std::uint64_t>(indices.size());
        record_t* const replacing = carve_<record_t>(replacements.size());
        if (!indices.empty() && (!replaced || !replacing)) return status_t::out_of_memory_heap_k;
        if ((status = upload(*executor_, replaced, indices.data(), indices.size())) != status_t::success_k)
            return status;
        if ((status = upload(*executor_, replacing, replacements.data(), replacements.size())) != status_t::success_k)
            return status;
        auto [packed, pack_status] = pack_(
            names_t::pack_k, merge_pack_regions<record_t> {merged, flags, merged_count, replaced, replacing,
                                                           std::uint64_t {indices.size()}, survivors, prefix, shares});
        if (pack_status != status_t::success_k) return pack_status;

        // What plans the new runs comes back at once, sized by the records, as only the
        // device counts survivors
        std::uint64_t* const selected = carve_bounce<std::uint64_t>(*executor_, 1);
        prefixes = locates_k ? carve_bounce<std::uint64_t>(*executor_, records_count + 1) : nullptr;
        host_shares = carve_bounce<std::uint8_t>(*executor_, records_count);
        if (!selected || !host_shares || (locates_k && !prefixes) || executor_->pin_bounce().failed())
            return status_t::out_of_memory_heap_k;
        if ((status = download(*executor_, selected, packed.counted, 1)) != status_t::success_k) return status;
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
