/**
 *  @file include/ustore/blobs/nvme_level_merger.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The merge of one LSM level into the next: pointing the compactor at each source run's
 *      index, then writing the planned runs out on a channel apart from the reads.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::int32_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <array>       // `std::array`
#include <atomic>      // `std::memory_order_acquire`, `std::memory_order_release`
#include <concepts>    // `std::same_as`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`
#include <variant>     // `std::get_if`

#include <smashtable/basic_hash_table.hpp> // `st::assume_reserved`
#include <smashtable/basic_vector.hpp>     // `st::vector`, `st::assume_uninitialized`

#include "ustore/blobs/nvme_compaction.hpp"        // `host_compactor_t`, `compaction_output`, `compaction_kind_t`
#include "ustore/blobs/nvme_key_slot.hpp"          // `storage_limits_t`, `stored_value_size_t`, `value_layout_t`
#include "ustore/blobs/nvme_lsm_level.hpp"         // `lsm_level`
#include "ustore/blobs/nvme_schemas.hpp"           // `cancellation_state_t`
#include "ustore/blobs/nvme_sorted_run.hpp"        // `sorted_run_header_t`, `sorted_run_layout`
#include "ustore/blobs/vram_nvidia_compaction.hpp" // `cuda_compactor_t`
#include "ustore/utilities/nvme_channels.hpp"      // `io_channel_t`, `basic_read_pipeline`
#include "ustore/utilities/nvme_ring.hpp"          // `extent_t`, `ring_concept`
#include "ustore/utilities/nvme_volume.hpp"        // `medium_extent_t`
#include "ustore/utilities/types.hpp"              // `status_t`, `byte_t`, `uuid128_t`, `uuid_generator`

namespace unum::ustore::blobs {

using utilities::load;
using utilities::medium_reference_t;
using utilities::memory_reference_t;
using utilities::pooled_buffer_t;
using utilities::ring_concept;
using utilities::uuid_generator;

#pragma region LSM Level Merger

#pragma region Vocabulary

/** Where a merge runs its compaction, which a caller either fixes or leaves to the merge. */
enum class compaction_placement_t : std::uint8_t {

    /** A device when one is present and the merge is large enough, and the host otherwise. */
    automatic_k,

    /** The host, whatever devices the machine carries. */
    host_k,

    /** A device, which a build without CUDA refuses rather than falling back. */
    device_k,
};

/** Which compactor a merge takes, once the placement and the size of the merge are both known. */
enum class compactor_choice_t : std::uint8_t {

    /** @c host_compactor_t, on the calling thread. */
    host_k,

    /** @c cuda_compactor_t, on the device of the executor the database owns. */
    device_k,
};

/**
 *  @brief The word a caller raises to stop a merge, which the merge reads between blocks.
 *
 *  A cancelled merge leaves the level set it was asked to replace untouched and returns every block
 *  and every reservation it took, so cancelling one costs the work it had done and nothing else.
 *
 *  @tparam atomic_reference_ The reference the word is owned through.
 */
template <template <typename> typename atomic_reference_>
class compaction_cancellation {
  public:

    compaction_cancellation() noexcept = default;
    compaction_cancellation(compaction_cancellation const&) = delete;
    compaction_cancellation& operator=(compaction_cancellation const&) = delete;

    /** Raises the word, which the merge reads at its next block boundary. */
    void raise() noexcept {
        atomic_reference_<std::int32_t> {raised_}.store(static_cast<std::int32_t>(cancellation_state_t::cancelled_k),
                                                        std::memory_order_release);
    }

    /** Whether a caller has raised the word. */
    [[nodiscard]] bool cancelled() const noexcept {
        return atomic_reference_<std::int32_t const> {raised_}.load(std::memory_order_acquire) !=
               static_cast<std::int32_t>(cancellation_state_t::wanted_k);
    }

  private:

    /** A @c cancellation_state_t, moved by @c raise alone. */
    std::int32_t raised_ = 0;
};

/**
 *  @brief A borrowed fan-out of one merge's slices over threads, which <tt>src/</tt> builds over its
 *      executor, so the merge names no pool: @c run calls a slice once per index, each on a thread
 *      holding a reading and a writing channel of its own, and answers the first failure.
 *
 *  An empty one offers one slice, which the merge runs on its caller's channels.
 */
class compaction_fan_out_t {
  public:

    /** One slice to run, as @c run hands it to the borrowed fan-out. */
    class slice_call_t {
      public:

        /** Runs slice @p index on the calling thread over @p reading and @p writing. */
        status_t operator()(std::size_t index, io_channel_t& reading, io_channel_t& writing) const noexcept {
            return call_(slice_, index, reading, writing);
        }

      private:
        friend class compaction_fan_out_t;
        using call_t = status_t (*)(void const*, std::size_t, io_channel_t&, io_channel_t&) noexcept;
        slice_call_t(void const* slice, call_t call) noexcept : slice_(slice), call_(call) {}

        /** The merge's own slice, borrowed. */
        void const* slice_ = nullptr;

        /** Calls it as its own type. */
        call_t call_ = nullptr;
    };

    /** A fan-out of one slice, which the merge runs on its caller's channels. */
    compaction_fan_out_t() noexcept = default;

    /** Borrows @p fan_out, which answers @c slices and runs a @c slice_call_t once per slice,
     *  outliving every merge handed this. */
    template <typename fan_out_type_>
    explicit compaction_fan_out_t(fan_out_type_& fan_out) noexcept
        : context_(&fan_out), slices_(fan_out.slices()), run_(&run_through_<fan_out_type_>) {}

    /** Slices one merge is cut into, at least one. */
    [[nodiscard]] std::size_t slices() const noexcept { return slices_; }

    /** Calls @p slice as <tt>slice(index, reading, writing)</tt> once for every index below @c slices. */
    template <typename slice_type_>
    status_t run(slice_type_ const& slice) const noexcept {
        return run_(context_, slice_call_t {&slice, &call_through_<slice_type_>});
    }

  private:
    using run_t = status_t (*)(void*, slice_call_t const&) noexcept;

    template <typename slice_type_>
    static status_t call_through_(void const* slice, std::size_t index, io_channel_t& reading,
                                  io_channel_t& writing) noexcept {
        return (*static_cast<slice_type_ const*>(slice))(index, reading, writing);
    }

    template <typename fan_out_type_>
    static status_t run_through_(void* context, slice_call_t const& slice) noexcept {
        return static_cast<fan_out_type_*>(context)->run(slice);
    }

    static status_t refuse_(void*, slice_call_t const&) noexcept {
        return {status_code_t::invalid_argument_k, "No threads serve this merge's slices"};
    }

    /** The borrowed fan-out. */
    void* context_ = nullptr;

    /** Slices one merge is cut into. */
    std::size_t slices_ = 1;

    /** Calls the borrowed fan-out's @c run as its own type. */
    run_t run_ = &refuse_;
};

/** Which compactor @p placement asks for, given @p devices devices and a merge of
 *  @p key_slots_count key slots. */
[[nodiscard]] constexpr compactor_choice_t choose_compactor(compaction_placement_t placement, std::size_t devices,
                                                            std::uint64_t key_slots_count) noexcept {
    if (placement == compaction_placement_t::host_k) return compactor_choice_t::host_k;
    if (placement == compaction_placement_t::device_k) return compactor_choice_t::device_k;
    return devices != 0 && key_slots_count > storage_limits_t::cuda_compaction_threshold_k
               ? compactor_choice_t::device_k
               : compactor_choice_t::host_k;
}

/** The engine's status for the @p status a compactor reported. */
inline status_t compaction_status(st::status_t status) noexcept {
    switch (status) {
    case st::status_t::success_k: return {};
    case st::status_t::out_of_memory_heap_k: return {status_code_t::out_of_memory_k, "A compaction could not grow"};
    case st::status_t::invalid_argument_k:
        return {status_code_t::invalid_argument_k, "A compaction refused its bounds"};
    case st::status_t::capacity_exhausted_k:
        return {status_code_t::capacity_exhausted_k, "A compaction ran out of room"};
    case st::status_t::input_output_k: return {status_code_t::io_k, "A compaction could not move its key slots"};
    default: return {status_code_t::device_k, "A compaction failed on the device it ran on"};
    }
}

/** Drains a channel's queue pair before the pipelines above it hand their blocks back, which a
 *  failure needs too. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
class channel_drain {
  public:

    channel_drain(disk_type_& disk, io_channel_t& channel) noexcept : disk_(disk), channel_(channel) {}
    channel_drain(channel_drain const&) = delete;
    channel_drain& operator=(channel_drain const&) = delete;

    ~channel_drain() noexcept { [[maybe_unused]] status_t const drained = disk_.drain(channel_.queue_pair()); }

  private:

    /** The ring the queue pair belongs to. */
    disk_type_& disk_;

    /** The channel whose queue pair is drained. */
    io_channel_t& channel_;
};

#pragma endregion Vocabulary

#pragma region Merger

/**
 *  @brief Merges runs of one level with the runs of the level below that they overlap, writing
 *      new sorted runs.
 *
 *  The tree picks the runs, which are a whole level zero or one run of a deeper level, and the runs
 *  below that overlap them; the new runs replace exactly those, so every source run is free once
 *  the merge is published and no run of either level outside the picked range is read or written.
 *
 *  @tparam atomic_reference_ The reference a caller's cancellation word is read through.
 *  @tparam waiter_type_ What the core does while the merge waits for its reads and barriers.
 *  @tparam row_kit_type_ The SmashTable kit the runs it reads and writes search with.
 */
template <typename key_slot_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_ = st::native_row_kit_t>
class lsm_level_merger {
  public:

#pragma region Types

    /** The key slot the runs of both levels hold. */
    using key_slot_t = key_slot_type_;

    /** The volume the runs live on. */
    using volume_t = volume_type_;

    /** The level this merge reads and writes. */
    using level_t = lsm_level<key_slot_t, volume_t, atomic_reference_, waiter_type_, row_kit_type_>;

    /** One immutable run of a level. */
    using sorted_run_t = typename level_t::sorted_run_t;

    /** The range tombstones one run carries. */
    using range_tombstones_t = typename sorted_run_t::range_tombstones_t;

    /** The keys one range tombstone hides. */
    using range_t = typename range_tombstones_t::range_t;

    /** The pool the new runs are taken into. */
    using run_pool_t = typename level_t::run_pool_t;

    /** How a level names one run. */
    using run_handle_t = typename level_t::run_handle_t;

    /** The runs one merge wrote, ascending and disjoint. */
    using merged_runs_t = st::vector<run_handle_t>;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The transport the volume moves bytes through. */
    using disk_t = typename volume_type_::disk_t;

    /** The word a caller raises to stop a merge. */
    using cancellation_t = compaction_cancellation<atomic_reference_>;

    /** Where the sections of a run sit inside its extent. */
    using sorted_run_layout_t = sorted_run_layout<key_slot_t>;

    /** The pipeline the gather reads its source values through. */
    using read_pipeline_t = basic_read_pipeline<disk_t>;

    /** The pipeline every section of every new run is written through. */
    using write_pipeline_t = basic_write_pipeline<disk_t, waiter_type_>;

    /** What one compaction answers: the new runs, their cells and where their values come from. */
    using compaction_output_t = compaction_output<key_slot_t>;

    /** A source run's tree of keys as a compaction reads it, by position alone, which
     *  no kit searches. */
    using compaction_keys_t = st::immutable_splus_tree_view<stored_key_t>;

    /** How the values of these runs are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so a merge moves no value bytes. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

    /** One block of one source value's read, kept until its completion carries the block back. */
    struct pending_piece_t {

        /** Where the bytes of this block land: in its source's window, which half they fill
         *  telling, or in the block being gathered. */
        std::uint32_t destination = 0;

        /** Bytes of the block before the value's own bytes. */
        std::uint32_t skipped = 0;

        /** Bytes of the value this block carries. */
        std::uint32_t carried = 0;

        /** The source run whose window this block refills, or the source count for a read into the
         *  block being gathered. */
        std::uint32_t source_run = 0;
    };

    /** One half of a source run's window: the stretch of the run's extent it holds and the reads
     *  still filling it. */
    struct window_half_t {

        /** The first byte of the run's extent the half holds. */
        std::uint64_t begin = 0;

        /** One past the last byte it holds. */
        std::uint64_t end = 0;

        /** Reads of this half still in flight. */
        std::uint32_t outstanding = 0;
    };

    /** The bytes of one source run's extent its window holds, so the values a merge copies out of
     *  that run in order are read in blocks rather than one page per value, one half reading ahead
     *  while the copy drains the other. */
    struct source_window_t {

        /** The room the window holds them in: a share of a streaming block, or of the heap where
         *  no share fits. */
        std::span<byte_t> bytes;

        /** Where @c bytes sit in registered memory, which a refill reads straight into, or nowhere
         *  on the heap. */
        memory_reference_t place;

        /** Both halves, over consecutive stretches once streaming, the one beginning
         *  later being ahead. */
        std::array<window_half_t, 2> halves {};
    };

    /** One run of the compaction's plan, with where its cells, survivors and copy ranges start
     *  in the output. */
    struct planned_run_t {

        /** Key slots the run holds, padding cells excluded. */
        std::uint64_t key_slots_count = 0;

        /** Bytes of the values its key slots locate, before the section's alignment padding. */
        std::uint64_t stored_values_bytes = 0;

        /** Where this run's survivors start in the compaction's cells, stamps,
         *  kinds and offsets. */
        std::uint64_t first_key_slot = 0;

        /** Where this run's value copy ranges start in the compaction's. */
        std::uint64_t first_copy_range = 0;

        /** Value copy ranges filling this run's stored values section. */
        std::uint64_t copy_ranges_count = 0;
    };

    /** One run a slice laid out, from its header and columns to the extent it is written to, until
     *  a level takes it. */
    struct laid_out_run_t {

        /** Where its key slots and copy ranges start in its slice's output, and how many. */
        planned_run_t planned;

        /** Its header, whose name and tombstones land once the next run's first key is known. */
        sorted_run_header_t header {};

        /** Its stamps, packed for the trailer. */
        stamps_column_t stamps;

        /** Its kinds, packed for the trailer. */
        kinds_column_t kinds;

        /** The saved tree of its keys. */
        st::vector<std::uint64_t> keys;

        /** Its value offsets from its first key slot on, none but for a variable width. */
        st::vector<stored_value_offset_t> value_offsets;

        /** The pieces of the merge's range tombstones between its first key and the next run's. */
        range_tombstones_t tombstones;

        /** Those pieces as the run's last section lays them out. */
        st::vector<byte_t> laid_tombstones;

        /** Where it is written, empty until allocated and again once a level took it. */
        medium_extent_t extent;
    };

    /** What a tree keeps for one slice from one merge to the next, as its merges run one at a time,
     *  so a warm merge allocates none of them: the slice's views, the compaction's output, the runs
     *  it lays out and the gather's buffers. */
    struct merge_buffers_t {

        /** The share of every source run this slice merges, empty for a merge of one slice. */
        st::vector<sorted_run_index_view<key_slot_t>> slice_views;

        /** What the last compaction wrote, which the next one writes over. */
        compaction_output_t output;

        /** The runs the last compaction was cut into, in key order. */
        st::vector<laid_out_run_t> laid_out_runs;

        /** The block every source read in flight carries, by its tag. */
        st::vector<pending_piece_t> pending_pieces;

        /** The bytes of its extent every source run's window holds. */
        st::vector<source_window_t> source_windows;

        /** The windows' room on the heap, where the streaming blocks give no source a share. */
        st::vector<byte_t> heap_windows;
    };

    /** What one merge is asked to do, beyond the volume and the channels it is handed. */
    struct merge_request_t {

        /** The runs whose versions shadow the lower ones', oldest first when they overlap and
         *  ascending otherwise, which are never none. */
        std::span<sorted_run_t const* const> upper;

        /** Whether the upper runs may overlap, which they do on level zero alone. */
        lsm_level_order_t upper_order = lsm_level_order_t::overlapping_k;

        /** The runs of the level below that the upper ones overlap, ascending, or none. */
        std::span<sorted_run_t const* const> lower;

        /** Whether anything below the new runs overlaps their range, which decides whether
         *  remove deltas drop. */
        lsm_level_depth_t depth = lsm_level_depth_t::intermediate_k;

        /** The collection every new run's header names. */
        uuid128_t collection {};

        /** The largest value the collection admits, which every new run's header carries. */
        stored_value_size_t value_max_size = 0;

        /** Largest new run in bytes, which a run of one key slot may still exceed. */
        std::uint64_t run_bytes_limit = storage_limits_t::sorted_run_file_max_size_k;

        /** Where the compaction runs. */
        compaction_placement_t placement = compaction_placement_t::automatic_k;

        /** The compactor the database picked at open: the calling thread, or its
         *  warm device compactor. */
        compactor_ref_t compactor;

        /** The word the merge reads between blocks, or null for a merge nobody stops. */
        cancellation_t const* cancellation = nullptr;

        /** Which older versions survive for readers and for a mount, and the floor the new
         *  runs count from. */
        compaction_stamps_t stamps;

        /** The buffers a merge of fixed-width keys writes into, one per slice, which its tree
         *  keeps between merges. */
        std::span<merge_buffers_t> buffers;

        /** The threads a merge of fixed-width keys on the host fans its slices out over, or one
         *  slice on the calling thread. */
        compaction_fan_out_t fan_out;
    };

#pragma endregion Types

#pragma region Merging

    /**
     *  @brief Merges @p request.upper into @p request.lower and writes the survivors as
     *      new sorted runs.
     *  @param[in,out] volume The volume the new runs are allocated on, which every source run
     *      also lives on.
     *  @param[in,out] runs The pool the new runs are taken into, naming them in no level yet.
     *  @param[in,out] reading_channel The channel the source values are gathered on, held by
     *      the calling thread.
     *  @param[in,out] writing_channel The channel the new runs are written on, which must not
     *      be @p reading_channel.
     *  @param[in] request Which levels merge, what the new runs may grow to, and where
     *      the compaction runs.
     *  @param[in] fold How the collection's changes compose and apply, which reads their values
     *      on @p reading_channel before any is gathered.
     *  @param[in] device_fold How a device composes the collection's changes, or
     *      @c no_device_fold_t where none does.
     *  @return The new runs in ascending order, or the status that stopped the merge with every new
     *      run already given back.
     */
    template <utilities::fold_concept fold_type_, typename device_fold_type_>
    static expected<merged_runs_t> merge(volume_t& volume, run_pool_t& runs, io_channel_t& reading_channel,
                                         io_channel_t& writing_channel, merge_request_t const& request,
                                         fold_type_ const& fold, device_fold_type_ const& device_fold) noexcept {
        if (request.upper.empty())
            return status_t {status_code_t::invalid_argument_k, "A merge reads at least one upper run"};
        if (&reading_channel == &writing_channel)
            return status_t {status_code_t::invalid_argument_k, "A merge reads and writes on two channels"};
        if constexpr (sorted_run_t::tailed_k) return merge_strings_(volume, runs, writing_channel, request);
        else return merge_fixed_(volume, runs, reading_channel, writing_channel, request, fold, device_fold);
    }

#pragma endregion Merging

  private:

    /**
     *  @brief Merges runs of fixed-width keys through the compactor the request names, then gathers
     *      and writes their survivors as new runs, in slices the request's fan-out runs side by
     *      side where the host merges.
     *
     *  Every slice compacts its share of every source and lays out the runs it cuts, on its own
     *  channels; the calling thread then names and allocates every run in key order, since a
     *  volume's placement takes one writer; every slice writes its runs on its own channels again;
     *  and the calling thread hands every run to the pool in key order. A merge of one slice runs
     *  each step on the calling thread over the channels it was handed.
     */
    template <utilities::fold_concept fold_type_, typename device_fold_type_>
    static expected<merged_runs_t> merge_fixed_(volume_t& volume, run_pool_t& runs, io_channel_t& reading_channel,
                                                io_channel_t& writing_channel, merge_request_t const& request,
                                                fold_type_ const& fold, device_fold_type_ const& device_fold) noexcept {
        if (request.buffers.empty())
            return status_t {status_code_t::invalid_argument_k, "A merge writes into the buffers its tree keeps"};
        st::vector<sorted_run_t const*> sources;
        st::vector<compaction_keys_t> keys;
        st::vector<sorted_run_index_view<key_slot_t>> views;
        if (status_t const pointed = point_views_(request.upper, request.lower, sources, keys, views); pointed.failed())
            return pointed;

        std::size_t const upper_runs = request.upper.size();
        std::span<sorted_run_index_view<key_slot_t> const> const level_views {views};
        std::span<sorted_run_t const* const> const source_runs {sources};
        std::size_t const slices = slices_of_(request, level_views.first(upper_runs), level_views.subspan(upper_runs));
        std::span<merge_buffers_t> const buffers = request.buffers.first(slices);
        if (slices != 1)
            if (status_t const cut = cut_slices_(level_views, buffers); cut.failed()) return cut;

        st::vector<range_t> kept_ranges;
        st::vector<std::uint64_t> kept_stamps;
        if (status_t const kept = kept_range_tombstones_(source_runs, request, kept_ranges, kept_stamps); kept.failed())
            return kept;

        auto lay_out_slice = [&](std::size_t slice, io_channel_t& reading,
                                 [[maybe_unused]] io_channel_t& writing) noexcept -> status_t {
            merge_buffers_t& sliced = buffers[slice];
            std::span<sorted_run_index_view<key_slot_t> const> const shares =
                slices == 1 ? level_views : std::span<sorted_run_index_view<key_slot_t> const> {sliced.slice_views};
            // A fold reads the values of the changes it folds, from their runs, one at a time.
            auto read_value = [&](located_key_slot<key_slot_t> const& record, utilities::arena_allocator<byte_t>& arena,
                                  std::span<byte_t const>& bytes) noexcept {
                std::size_t const size = sources[record.sorted_run]->value_window(record.position).bytes;
                byte_t* const into = size != 0 ? arena.allocate(size) : nullptr;
                if (size != 0 && into == nullptr) return st::status_t::out_of_memory_heap_k;
                if (sources[record.sorted_run]->read_value(reading, record.position, {into, size}).failed())
                    return st::status_t::input_output_k;
                bytes = {into, size};
                return st::status_t::success_k;
            };
            if (status_t const compacted = run_compaction_(
                    kind_of_(request.upper_order, request.depth), shares.first(upper_runs), shares.subspan(upper_runs),
                    source_runs, bounds_of_(request), request.stamps, request.placement, request.compactor, fold,
                    device_fold, read_value, sliced.output);
                compacted.failed())
                return compacted;
            return lay_out_runs_(sliced, request);
        };
        if (status_t const laid = run_slices_(request.fan_out, slices, reading_channel, writing_channel, lay_out_slice);
            laid.failed())
            return laid;

        // Tombstones outliving every key of the merge still need a run, holding no key slot.
        std::size_t runs_count = 0;
        for (merge_buffers_t const& sliced : buffers) runs_count += sliced.laid_out_runs.size();
        if (runs_count == 0 && !kept_ranges.empty()) {
            if (status_t const laid = lay_out_run_(buffers[0], planned_run_t {}, request); laid.failed()) return laid;
            runs_count = 1;
        }
        if (status_t const placed = place_runs_(volume, buffers, std::span<range_t const> {kept_ranges},
                                                std::span<std::uint64_t const> {kept_stamps});
            placed.failed())
            return placed;

        auto write_slice = [&](std::size_t slice, io_channel_t& reading, io_channel_t& writing) noexcept -> status_t {
            merge_buffers_t& sliced = buffers[slice];
            for (laid_out_run_t const& laid : sliced.laid_out_runs) {
                std::span<key_slot_t const> const cells {sliced.output.cells.data() + laid.planned.first_key_slot,
                                                         static_cast<std::size_t>(laid.planned.key_slots_count)};
                if (status_t const written = write_sections_(
                        volume, reading, writing, source_runs, sliced, laid.planned, laid.header, laid.stamps,
                        laid.kinds, cells, std::span<std::uint64_t const> {laid.keys},
                        std::span<stored_value_offset_t const> {laid.value_offsets},
                        std::span<byte_t const> {laid.laid_tombstones}, laid.extent, request.cancellation);
                    written.failed())
                    return written;
            }
            return {};
        };
        if (status_t const written = run_slices_(request.fan_out, slices, reading_channel, writing_channel,
                                                 write_slice);
            written.failed()) {
            abandon_runs_(volume, buffers);
            return written;
        }

        merged_runs_t merged;
        if (merged.reserve(runs_count) != st::status_t::success_k) {
            abandon_runs_(volume, buffers);
            return status_t {status_code_t::out_of_memory_k, "A merge could not track the runs it writes"};
        }
        for (merge_buffers_t& sliced : buffers)
            for (laid_out_run_t& laid : sliced.laid_out_runs) {
                std::span<key_slot_t const> const cells {sliced.output.cells.data() + laid.planned.first_key_slot,
                                                         static_cast<std::size_t>(laid.planned.key_slots_count)};
                auto [adopted, adopt_status] = sorted_run_t::adopt(
                    volume, laid.extent, laid.header, cells, std::move(laid.keys), std::move(laid.value_offsets),
                    std::move(laid.stamps), std::move(laid.kinds), std::move(laid.tombstones));
                status_t status = adopt_status;
                if (status.succeeded()) {
                    auto [taken, take_status] = runs.take(std::move(adopted));
                    status = take_status;
                    if (take_status.succeeded()) {
                        [[maybe_unused]] st::status_t const placed = merged.push_back(std::move(taken));
                        laid.extent = medium_extent_t {};
                    }
                }
                if (status.failed()) {
                    abandon_runs_(volume, buffers);
                    runs.discard(volume, std::span<run_handle_t const> {merged});
                    return status;
                }
            }
        return merged;
    }

#pragma region Strings Merge

    /** Versions in ascending order of their keys, the versions of one key newest first, and where
     *  each key's start. */
    struct string_side_t {

        /** Every version. */
        st::vector<strings_version_t> versions;

        /** Where the versions of each key start in @c versions. */
        st::vector<std::uint32_t> keys;
    };

    /** The whole key of @p version. */
    [[nodiscard]] static std::string_view key_of_(std::span<sorted_run_t const* const> sources,
                                                  strings_version_t version) noexcept {
        return sources[version.source]->key(version.position);
    }

    /** Appends every version of the runs of @p sources from @p first to @p last, which are disjoint
     *  and ascend, to @p side, noting where each key starts. */
    static status_t side_of_(std::span<sorted_run_t const* const> sources, std::uint32_t first, std::uint32_t last,
                             string_side_t& side) noexcept {
        std::uint64_t versions = 0;
        for (std::uint32_t source = first; source != last; ++source) versions += sources[source]->population_count();
        if (versions > limit<std::uint32_t>::max_k)
            return {status_code_t::capacity_exhausted_k, "A merge of strings holds more versions than it counts"};
        if (side.versions.reserve(static_cast<std::size_t>(versions)) != st::status_t::success_k ||
            side.keys.reserve(static_cast<std::size_t>(versions)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A merge of strings could not list its versions"};
        for (std::uint32_t source = first; source != last; ++source)
            for (std::uint64_t position = 0; position != sources[source]->population_count(); ++position) {
                strings_version_t const version {source, static_cast<std::uint32_t>(position)};
                if (position == 0 || sources[source]->key(position) != sources[source]->key(position - 1))
                    [[maybe_unused]]
                    st::status_t const noted = side.keys.push_back(st::assume_reserved,
                                                                   static_cast<std::uint32_t>(side.versions.size()));
                [[maybe_unused]] st::status_t const placed = side.versions.push_back(st::assume_reserved,
                                                                                     strings_version_t {version});
            }
        return {};
    }

    /** Folds @p older into @p newer by the union of their keys, so every key's versions from
     *  @p newer come before those from @p older, which @p newer takes in place. */
    static status_t fold_sides_(std::span<sorted_run_t const* const> sources, string_side_t& newer,
                                string_side_t const& older) noexcept {
        auto const reader_of = [sources](string_side_t const& side) noexcept {
            return [sources, &side](std::size_t first, std::span<std::string_view> views) noexcept {
                for (std::size_t index = 0; index != views.size(); ++index)
                    views[index] = key_of_(sources, side.versions[side.keys[first + index]]);
            };
        };

        std::size_t const rows_max = newer.keys.size() + older.keys.size();
        st::vector<std::size_t> newer_positions;
        st::vector<std::size_t> older_positions;
        string_side_t joined;
        if (newer_positions.resize(rows_max) != st::status_t::success_k ||
            older_positions.resize(rows_max) != st::status_t::success_k ||
            joined.versions.reserve(newer.versions.size() + older.versions.size()) != st::status_t::success_k ||
            joined.keys.reserve(rows_max) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A merge of strings could not hold its union"};
        std::size_t const rows = union_sorted_keys(reader_of(newer), newer.keys.size(), reader_of(older),
                                                   older.keys.size(), std::span<std::size_t> {newer_positions},
                                                   std::span<std::size_t> {older_positions});

        auto append_key = [&](string_side_t const& side, std::size_t key) noexcept {
            std::size_t const begin = side.keys[key];
            std::size_t const end = key + 1 != side.keys.size() ? side.keys[key + 1] : side.versions.size();
            for (std::size_t version = begin; version != end; ++version) [[maybe_unused]]
                st::status_t const placed = joined.versions.push_back(st::assume_reserved,
                                                                      strings_version_t {side.versions[version]});
        };
        for (std::size_t row = 0; row != rows; ++row) {
            [[maybe_unused]] st::status_t const noted = joined.keys.push_back(
                st::assume_reserved, static_cast<std::uint32_t>(joined.versions.size()));
            if (newer_positions[row] != absent_position_k) append_key(newer, newer_positions[row]);
            if (older_positions[row] != absent_position_k) append_key(older, older_positions[row]);
        }
        newer = std::move(joined);
        return {};
    }

    /** Adds one to @p depths for every version of a tie of equal heads, @p depth heads deep, among
     *  the versions from @p first to @p last that a run would give a layer, and recurses into the
     *  ties nested in it. */
    static void count_layers_(std::span<sorted_run_t const* const> sources, std::span<strings_version_t const> versions,
                              std::size_t first, std::size_t last, std::uint32_t depth,
                              std::span<std::uint32_t> depths) noexcept {
        std::size_t const skipped = std::size_t {depth} * strings::head_prefix_bytes_k;
        auto head_of_version = [&](std::size_t index) noexcept {
            return strings::head_of(key_of_(sources, versions[index]).substr(skipped));
        };
        for (std::size_t begin = first; begin != last;) {
            st::key128_t const head = head_of_version(begin);
            std::size_t end = begin + 1;
            while (end != last && head_of_version(end) == head) ++end;
            if (end - begin > sorted_run_t::strings_layout_t::keys_per_row_k &&
                key_of_(sources, versions[begin]) != key_of_(sources, versions[end - 1])) {
                for (std::size_t index = begin; index != end; ++index) ++depths[index];
                count_layers_(sources, versions, begin, end, depth + 1, depths);
            }
            begin = end;
        }
    }

    /** Lists the versions of @p sources that survive a merge of strings, in merged order: on the
     *  device compactor where @p request places the merge, and by unions of their keys on the host
     *  otherwise or where the device refuses it. */
    static status_t strings_survivors_(std::span<sorted_run_t const* const> sources, merge_request_t const& request,
                                       st::vector<strings_version_t>& survivors) noexcept {
        std::uint32_t const upper_count = static_cast<std::uint32_t>(request.upper.size());
        compaction_kind_t const kind = kind_of_(request.upper_order, request.depth);
        std::uint64_t records = 0;
        for (sorted_run_t const* source : sources) records += source->population_count();
        cuda_compactor_t* const* const device = std::get_if<cuda_compactor_t*>(&request.compactor);
        bool tombstoned = false;
        for (sorted_run_t const* source : sources) tombstoned |= !source->range_tombstones().empty();
        // No device compaction reads range tombstones, so a merge carrying one runs here.
        if (!tombstoned &&
            choose_compactor(request.placement, device != nullptr ? 1 : 0, records) == compactor_choice_t::device_k) {
            if (device == nullptr) return {status_code_t::unsupported_k, "No device compactor serves this database"};
            // The device reads every run's heads by position, as no kit searches them.
            st::vector<st::immutable_splus_tree_view<st::key128_t>> heads;
            st::vector<strings_run_view> views;
            if (heads.resize(sources.size()) != st::status_t::success_k ||
                views.resize(sources.size()) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A merge could not track its source runs"};
            for (std::size_t index = 0; index != sources.size(); ++index) {
                st::expected<st::immutable_splus_tree_view<st::key128_t>> const viewed =
                    st::immutable_splus_tree_view<st::key128_t>::make(sources[index]->keys_bytes());
                if (!viewed) return {status_code_t::internal_k, "A source run's heads read as no tree"};
                heads[index] = *viewed;
                views[index] = strings_run_view {&heads[index], sources[index]->key_offsets(),
                                                 sources[index]->key_tape(), &sources[index]->stamps(),
                                                 &sources[index]->kinds()};
            }
            st::status_t const ranked = (*device)->merge_strings(kind, std::span<strings_run_view const> {views},
                                                                 upper_count, request.stamps, survivors);
            if (ranked != st::status_t::invalid_argument_k && ranked != st::status_t::out_of_memory_heap_k)
                return compaction_status(ranked);
        }
        return host_strings_survivors_(sources, upper_count, kind, request.stamps, request.cancellation, survivors);
    }

    /** Lists the versions of @p sources that survive, as @c strings_survivors_ does, on the calling
     *  thread: the upper runs folded newest first, then the lower runs joined, by key unions. */
    static status_t host_strings_survivors_(std::span<sorted_run_t const* const> sources, std::uint32_t upper_count,
                                            compaction_kind_t kind, compaction_stamps_t const& stamps,
                                            cancellation_t const* cancellation,
                                            st::vector<strings_version_t>& survivors) noexcept {
        // The newest upper run is the first side, and each older one folds in after it, the
        // lower level last.
        string_side_t merged_side;
        if (status_t const first = side_of_(sources, upper_count - 1, upper_count, merged_side); first.failed())
            return first;
        for (std::uint32_t source = upper_count - 1; source-- != 0;) {
            if (stopped_(cancellation)) return cancelled_status_();
            string_side_t older;
            if (status_t const listed_side = side_of_(sources, source, source + 1, older); listed_side.failed())
                return listed_side;
            if (status_t const folded = fold_sides_(sources, merged_side, older); folded.failed()) return folded;
        }
        if (upper_count != sources.size()) {
            if (stopped_(cancellation)) return cancelled_status_();
            string_side_t lower;
            if (status_t const listed_side = side_of_(sources, upper_count, static_cast<std::uint32_t>(sources.size()),
                                                      lower);
                listed_side.failed())
                return listed_side;
            if (status_t const folded = fold_sides_(sources, merged_side, lower); folded.failed()) return folded;
        }

        // The versions that survive, as the compaction of fixed-width keys decides it for each
        // key in turn.
        if (survivors.reserve(merged_side.versions.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A merge of strings could not list its survivors"};
        located_key_slot<key_slot_t> previous;
        for (std::size_t key = 0; key != merged_side.keys.size(); ++key) {
            std::size_t const begin = merged_side.keys[key];
            std::size_t const end = key + 1 != merged_side.keys.size() ? merged_side.keys[key + 1]
                                                                       : merged_side.versions.size();
            for (std::size_t index = begin; index != end; ++index) {
                strings_version_t const version = merged_side.versions[index];
                sorted_run_t const& source = *sources[version.source];
                located_key_slot<key_slot_t> current;
                current.key_slot.stored_key = source.head(version.position);
                current.sorted_run = version.source;
                current.kind = source.kind(version.position);
                current.stamp = source.stamp(version.position);
                current.position = version.position;
                bool const hidden = hidden_string_(sources, upper_count, key_of_(sources, version), version.source,
                                                   current.stamp, stamps);
                if (!hidden && survives(index != begin ? &previous : nullptr, current, kind, stamps)) [[maybe_unused]]
                    st::status_t const kept = survivors.push_back(st::assume_reserved, strings_version_t {version});
                previous = current;
            }
        }
        return {};
    }

    /** Merges runs of strings: the upper runs folded newest first and then the lower runs,
     *  concatenated, as one side, by unions of their keys, keeping the versions @p request's stamps
     *  keep, written as new runs cut to equal sizes that never split the versions of one key. */
    static expected<merged_runs_t> merge_strings_(volume_t& volume, run_pool_t& runs, io_channel_t& writing_channel,
                                                  merge_request_t const& request) noexcept {
        st::vector<sorted_run_t const*> listed;
        if (listed.resize(request.upper.size() + request.lower.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A merge could not track its source runs"};
        for (std::size_t index = 0; index != request.upper.size(); ++index) listed[index] = request.upper[index];
        for (std::size_t index = 0; index != request.lower.size(); ++index)
            listed[request.upper.size() + index] = request.lower[index];
        std::span<sorted_run_t const* const> const sources {listed};

        st::vector<strings_version_t> survivors;
        if (status_t const kept = strings_survivors_(sources, request, survivors); kept.failed()) return kept;

        // Each survivor plans its key, and a share of the layers it lands in, so the cut runs
        // fit their limit.
        st::vector<std::uint32_t> depths;
        st::vector<std::uint64_t> planned;
        if (depths.resize(survivors.size()) != st::status_t::success_k ||
            planned.resize(survivors.size() + 1) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A merge of strings could not plan its runs"};
        for (std::uint32_t& depth : depths) depth = 0;
        count_layers_(sources, std::span<strings_version_t const> {survivors}, 0, survivors.size(), 0,
                      std::span<std::uint32_t> {depths});
        planned[0] = 0;
        for (std::size_t index = 0; index != survivors.size(); ++index)
            planned[index + 1] = planned[index] + key_of_(sources, survivors[index]).size() +
                                 2 * sizeof(st::key128_t) * depths[index];
        auto const read_prefix = [&planned](std::uint64_t index, std::uint64_t& bytes) noexcept {
            bytes = planned[static_cast<std::size_t>(index)];
            return st::status_t::success_k;
        };
        // Runs split between keys alone, so a level finds every version of a key in one run.
        auto const shares_key = [&](std::uint64_t index, bool& shared) noexcept {
            shared = key_of_(sources, survivors[static_cast<std::size_t>(index)]) ==
                     key_of_(sources, survivors[static_cast<std::size_t>(index) - 1]);
            return st::status_t::success_k;
        };
        st::vector<compacted_sorted_run_t> cuts;
        if (st::status_t const cut = plan_sorted_runs<key_slot_t>(survivors.size(), bounds_of_(request), read_prefix,
                                                                  shares_key, cuts);
            cut != st::status_t::success_k)
            return compaction_status(cut);
        st::vector<range_t> kept_ranges;
        st::vector<std::uint64_t> kept_stamps;
        if (status_t const kept = kept_range_tombstones_(sources, request, kept_ranges, kept_stamps); kept.failed())
            return kept;
        // Tombstones outliving every string of the merge still need a run, holding no string.
        if (cuts.empty() && !kept_ranges.empty())
            if (cuts.push_back(compacted_sorted_run_t {}) != st::status_t::success_k)
                return status_t {status_code_t::out_of_memory_k, "A merge could not plan its runs"};

        merged_runs_t merged;
        if (merged.reserve(cuts.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A merge could not track the runs it writes"};
        std::size_t first = 0;
        for (std::size_t index = 0; index != cuts.size(); ++index) {
            compacted_sorted_run_t const& cut = cuts[index];
            if (stopped_(request.cancellation)) {
                runs.discard(volume, std::span<run_handle_t const> {merged});
                return cancelled_status_();
            }
            std::span<strings_version_t const> const run_versions =
                std::span<strings_version_t const> {survivors}.subspan(first,
                                                                       static_cast<std::size_t>(cut.key_slots_count));
            // Each run keeps the tombstone pieces between its first string and the next run's.
            std::size_t const next = first + static_cast<std::size_t>(cut.key_slots_count);
            auto [pieces, pieces_status] = clipped_string_tombstones_(
                std::span<range_t const> {kept_ranges}, std::span<std::uint64_t const> {kept_stamps},
                index == 0 ? std::string_view {} : key_of_(sources, survivors[first]),
                index + 1 == cuts.size() ? std::string_view {} : key_of_(sources, survivors[next]));
            if (pieces_status.failed()) {
                runs.discard(volume, std::span<run_handle_t const> {merged});
                return pieces_status;
            }
            auto [written, write_status] = write_strings_run_(volume, writing_channel, sources, run_versions, request,
                                                              pieces);
            if (write_status.failed()) {
                runs.discard(volume, std::span<run_handle_t const> {merged});
                return write_status;
            }
            auto [taken, take_status] = runs.take(std::move(written));
            if (take_status.failed()) {
                volume.deallocate(written.extent());
                runs.discard(volume, std::span<run_handle_t const> {merged});
                return take_status;
            }
            [[maybe_unused]] st::status_t const placed = merged.push_back(std::move(taken));
            first += static_cast<std::size_t>(cut.key_slots_count);
        }
        return merged;
    }

    /** Writes @p versions of @p sources, in order, as one new run of strings stamped from
     *  @p request's floor. */
    static expected<sorted_run_t> write_strings_run_(volume_t& volume, io_channel_t& writing_channel,
                                                     std::span<sorted_run_t const* const> sources,
                                                     std::span<strings_version_t const> versions,
                                                     merge_request_t const& request,
                                                     range_tombstones_t const& tombstones) noexcept {
        std::size_t const count = versions.size();
        std::uint64_t tails_bytes = 0;
        for (strings_version_t const version : versions)
            tails_bytes += strings::tail_of(key_of_(sources, version)).size();
        if (tails_bytes > limit<std::uint32_t>::max_k)
            return status_t {status_code_t::capacity_exhausted_k, "A merged run's keys outgrow their offsets"};
        st::vector<key_slot_t> key_slots;
        st::vector<std::uint64_t> stamps;
        st::vector<std::uint8_t> kinds;
        st::vector<std::uint32_t> tail_offsets;
        st::vector<byte_t> tails;
        if (key_slots.resize(count) != st::status_t::success_k || stamps.resize(count) != st::status_t::success_k ||
            kinds.resize(count) != st::status_t::success_k ||
            tail_offsets.resize(count + 1) != st::status_t::success_k ||
            tails.resize(static_cast<std::size_t>(tails_bytes)) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A merge of strings could not lay out a run"};
        std::uint32_t written = 0;
        for (std::size_t index = 0; index != count; ++index) {
            sorted_run_t const& source = *sources[versions[index].source];
            std::uint64_t const position = versions[index].position;
            std::string_view const tail = strings::tail_of(source.key(position));
            key_slot_t slot {};
            slot.stored_key = source.head(position);
            std::memcpy(slot.data, source.value(position).data(), key_slot_t::size);
            key_slots[index] = slot;
            stamps[index] = fu::max_of(source.stamp(position), request.stamps.floor);
            kinds[index] = static_cast<std::uint8_t>(source.kind(position));
            tail_offsets[index] = written;
            if (!tail.empty()) std::memcpy(tails.data() + written, tail.data(), tail.size());
            written += static_cast<std::uint32_t>(tail.size());
        }
        tail_offsets[count] = written;
        key_tails_column_t const column = tails_bytes != 0
                                              ? key_tails_column_t {std::span<std::uint32_t const> {tail_offsets},
                                                                    std::span<byte_t const> {tails}}
                                              : key_tails_column_t {};
        return sorted_run_t::write(volume, writing_channel, request.collection, uuid_generator(),
                                   std::span<key_slot_t const> {key_slots}, std::span<std::uint64_t const> {stamps},
                                   std::span<std::uint8_t const> {kinds}, request.stamps.floor, {}, {},
                                   request.value_max_size, column, tombstones);
    }

    /** Whether a range tombstone of @p sources hides the version of @p text stamped @p stamp that
     *  source @p source holds from every reader of @p stamps, as @c hidden_by_range decides it
     *  for fixed-width keys. */
    static bool hidden_string_(std::span<sorted_run_t const* const> sources, std::size_t upper_count,
                               std::string_view text, std::uint32_t source, std::uint64_t stamp,
                               compaction_stamps_t const& stamps) noexcept {
        for (std::size_t holder = 0; holder != sources.size(); ++holder) {
            range_tombstones_t const& tombstones = sources[holder]->range_tombstones();
            bool const own = holder == source;
            bool const newer = holder < upper_count && (source >= upper_count || holder > source);
            if (tombstones.empty() || (!own && !newer)) continue;
            for (std::size_t index = 0; index != tombstones.size(); ++index) {
                std::uint64_t const removed = tombstones.stamp(index);
                if (!tombstones.range(index).covers(text) || removed > stamps.ceiling || removed > stamps.horizon)
                    continue;
                if (own && removed <= stamp) continue;
                if (stamp >= removed || reader_interval(stamps, stamp) == reader_interval(stamps, removed)) return true;
            }
        }
        return false;
    }

    /** The pieces of @p ranges, stamped by @p stamps, that fall from @p lower up to @p upper,
     *  exclusive: what one run of strings a merge writes carries, where an empty bound leaves
     *  its side open. */
    static expected<range_tombstones_t> clipped_string_tombstones_(std::span<range_t const> ranges,
                                                                   std::span<std::uint64_t const> stamps,
                                                                   std::string_view lower,
                                                                   std::string_view upper) noexcept {
        st::vector<range_t> pieces;
        st::vector<std::uint64_t> pieces_stamps;
        if (pieces.reserve(ranges.size()) != st::status_t::success_k ||
            pieces_stamps.reserve(ranges.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A merge could not cut its range tombstones"};
        for (std::size_t index = 0; index != ranges.size(); ++index) {
            range_t piece = ranges[index];
            if (piece.start < lower) piece.start = lower;
            if (!upper.empty() && (piece.end.empty() || upper < piece.end)) piece.end = upper;
            if (!piece.end.empty() && !(piece.start < piece.end)) continue;
            [[maybe_unused]] st::status_t const cut = pieces.push_back(st::assume_reserved, range_t {piece});
            [[maybe_unused]] st::status_t const stamped = pieces_stamps.push_back(st::assume_reserved,
                                                                                  std::uint64_t {stamps[index]});
        }
        return range_tombstones_t::make(std::span<range_t const> {pieces},
                                        std::span<std::uint64_t const> {pieces_stamps});
    }

#pragma endregion Strings Merge

#pragma region Private Methods

    /** Which compaction the order of the upper level and the depth of the new
     *  level together name. */
    [[nodiscard]] static constexpr compaction_kind_t kind_of_(lsm_level_order_t order,
                                                              lsm_level_depth_t depth) noexcept {
        if (order == lsm_level_order_t::overlapping_k)
            return depth == lsm_level_depth_t::bottom_k ? compaction_kind_t::overlapping_into_bottom_k
                                                        : compaction_kind_t::overlapping_into_intermediate_k;
        return depth == lsm_level_depth_t::bottom_k ? compaction_kind_t::sorted_into_bottom_k
                                                    : compaction_kind_t::sorted_into_intermediate_k;
    }

    /** The bounds every run the merge writes must fit, which the run layout and
     *  @p request together fix. */
    [[nodiscard]] static sorted_run_bounds_t bounds_of_(merge_request_t const& request) noexcept {
        // The trailer packs at most a stamp and a kind per key slot, which its cells outweigh, and
        // one page of headers and padding, so the plan cuts the cells and values short enough to
        // leave it room.
        std::uint64_t const trailer_per_key_slot = sizeof(std::uint64_t) + sizeof(std::uint8_t);
        std::uint64_t const planned = request.run_bytes_limit > portable_alignment_k
                                          ? request.run_bytes_limit - portable_alignment_k
                                          : 1;
        sorted_run_bounds_t bounds;
        bounds.file_bytes_limit = planned / (sizeof(key_slot_t) + trailer_per_key_slot) * sizeof(key_slot_t);
        bounds.header_bytes = sorted_run_layout_t::header_bytes();
        bounds.alignment_bytes = portable_alignment_k;
        bounds.value_stride = is_fixed(value_layout_k) ? request.value_max_size : 0;
        return bounds;
    }

    /** Points a view of @p views at every source run's own cells and its tree of keys, viewed again
     *  in @p keys as a compaction reads it, both of which the compactor reads in place. */
    static status_t point_views_(std::span<sorted_run_t const* const> upper, std::span<sorted_run_t const* const> lower,
                                 st::vector<sorted_run_t const*>& sources, st::vector<compaction_keys_t>& keys,
                                 st::vector<sorted_run_index_view<key_slot_t>>& views) noexcept {
        std::size_t const runs = upper.size() + lower.size();
        if (sources.resize(runs) != st::status_t::success_k || keys.resize(runs) != st::status_t::success_k ||
            views.resize(runs) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A merge could not track its source runs"};
        for (std::size_t index = 0; index != upper.size(); ++index) sources[index] = upper[index];
        for (std::size_t index = 0; index != lower.size(); ++index) sources[upper.size() + index] = lower[index];

        for (std::size_t index = 0; index != runs; ++index) {
            sorted_run_t const& run = *sources[index];
            key_slot_t const* cells = nullptr;
            if constexpr (sorted_run_t::keeps_cells_k) cells = run.cells().data();
            st::expected<compaction_keys_t> const viewed = compaction_keys_t::make(run.keys_bytes());
            if (!viewed) return {status_code_t::internal_k, "A source run's tree of keys reads as no tree"};
            keys[index] = *viewed;
            views[index] = sorted_run_index_view<key_slot_t> {
                cells, run.population_count(), &run.stamps(), &run.kinds(),
                {},    run.value_offsets(),    &keys[index],  &run.range_tombstones()};
        }
        return {};
    }

    /** Where the sections of @p run sit on its volume, and the stamp an ingest gave it, which a
     *  device compactor reads. */
    [[nodiscard]] static sorted_run_extents_t extents_of_(sorted_run_t const& run) noexcept {
        medium_extent_t const& blob = run.extent();
        if (blob.medium.medium_id == utilities::registered_medium_id_t::unregistered_k)
            return {.assigned_stamp = run.assigned_stamp()};
        sorted_run_header_t const& header = run.header();
        std::uint64_t const count = header.key_slots_count;
        std::uint64_t const offsets_bytes = round_up_to_multiple(sorted_run_layout_t::value_offsets_bytes(count),
                                                                 portable_alignment_k);
        return sorted_run_extents_t {
            std::uint32_t {blob.data_directory},
            blob.slice(sorted_run_layout_t::index_offset(), sorted_run_layout_t::index_section_bytes(count)),
            blob.slice(sorted_run_layout_t::value_offsets_offset(count), offsets_bytes),
            blob.slice(sorted_run_layout_t::stamps_offset(count, header.stored_values_bytes),
                       sorted_run_layout_t::trailer_bytes(header.stamps_bytes, header.kinds_bytes)),
            header.stamps_bytes,
            run.assigned_stamp(),
            blob.slice(sorted_run_layout_t::keys_offset(count), sorted_run_layout_t::keys_section_bytes(count)),
            sorted_run_layout_t::keys_leaves_offset(count),
            run.content_id()};
    }

    /** Counts the key slots @p upper and @p lower merge, and tells whether any of their runs
     *  holds a change. */
    [[nodiscard]] static std::uint64_t merged_count_(std::span<sorted_run_index_view<key_slot_t> const> upper,
                                                     std::span<sorted_run_index_view<key_slot_t> const> lower,
                                                     bool& changes) noexcept {
        std::uint64_t merged = 0;
        changes = false;
        for (std::size_t index = 0; index != upper.size(); ++index) {
            merged += upper[index].key_slots_count;
            changes |= holds_changes(upper[index].kinds);
        }
        for (std::size_t index = 0; index != lower.size(); ++index) {
            merged += lower[index].key_slots_count;
            changes |= holds_changes(lower[index].kinds);
        }
        return merged;
    }

    /** Compacts on the device compactor @p compactor names when @p placement asks for a device and
     *  no run holds a change, or one does and @p device_fold composes changes, reading the sections
     *  of @p sources, and on the calling thread otherwise, where runs carrying tails and merges
     *  outgrowing the device's scratch merge. */
    template <typename fold_type_, typename device_fold_type_, typename value_reader_type_>
    static status_t run_compaction_(compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_t> const> upper,
                                    std::span<sorted_run_index_view<key_slot_t> const> lower,
                                    [[maybe_unused]] std::span<sorted_run_t const* const> sources,
                                    sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps,
                                    compaction_placement_t placement, compactor_ref_t const& compactor,
                                    fold_type_ const& fold, [[maybe_unused]] device_fold_type_ const& device_fold,
                                    value_reader_type_&& read_value, compaction_output_t& output) noexcept {
        bool changes = false;
        std::uint64_t const merged = merged_count_(upper, lower, changes);
        cuda_compactor_t* const* const device = std::get_if<cuda_compactor_t*>(&compactor);
        // No device compaction reads range tombstones, so a merge carrying one runs here.
        if (!carries_range_tombstones(upper, lower) &&
            choose_compactor(placement, device != nullptr ? 1 : 0, merged) == compactor_choice_t::device_k) {
            if (device == nullptr) return {status_code_t::unsupported_k, "No device compactor serves this database"};
            // No device compaction is compiled for key slots carrying tails.
            if constexpr (!tailed_key_slot_concept<key_slot_t>) {
                constexpr bool device_folds_k = !std::same_as<device_fold_type_, no_device_fold_t>;
                st::vector<sorted_run_extents_t> extents;
                if (extents.resize(sources.size()) != st::status_t::success_k)
                    return {status_code_t::out_of_memory_k, "A merge could not track where its runs sit"};
                for (std::size_t index = 0; index != sources.size(); ++index)
                    extents[index] = extents_of_(*sources[index]);
                st::status_t compacted = st::status_t::invalid_argument_k;
                if constexpr (device_folds_k)
                    compacted = (*device)->compact(kind, upper, lower, extents, bounds, stamps, fold, device_fold,
                                                   read_value, output);
                // No run holds a change here, so the compaction built for the last write
                // serves every fold
                else if (!changes)
                    compacted = (*device)->compact(kind, upper, lower, extents, bounds, stamps,
                                                   utilities::last_write_fold_t {}, no_device_fold_t {},
                                                   located_value_reader<key_slot_t> {}, output);
                if (compacted != st::status_t::invalid_argument_k && compacted != st::status_t::out_of_memory_heap_k)
                    return compaction_status(compacted);
            }
        }
        return compaction_status(
            host_compactor_t {}.compact(kind, upper, lower, bounds, stamps, fold, read_value, output));
    }

    /** Takes a streaming block from @p writer, polling its completions when every block is out. */
    static expected<pooled_buffer_t> acquire_buffer_or_poll_(write_pipeline_t& writer) noexcept {
        expected<pooled_buffer_t> block = writer.acquire_stream_buffer();
        while (block == status_code_t::capacity_exhausted_k && writer.outstanding() != 0) {
            if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;
            if (status_t const polled = writer.poll_one_completion(); polled.failed()) return polled;
            block = writer.acquire_stream_buffer();
        }
        return block;
    }

    /** Whether @p cancellation has been raised, which every block boundary of a merge asks. */
    [[nodiscard]] static bool stopped_(cancellation_t const* cancellation) noexcept {
        return cancellation != nullptr && cancellation->cancelled();
    }

    /** The status a merge answers once its caller has cancelled it. */
    static status_t cancelled_status_() noexcept {
        return {status_code_t::uninitialized_k, "The caller cancelled this compaction"};
    }

    /** Lists into @p ranges and @p stamps the range tombstones of @p sources @p request keeps. */
    static status_t kept_range_tombstones_(std::span<sorted_run_t const* const> sources, merge_request_t const& request,
                                           st::vector<range_t>& ranges, st::vector<std::uint64_t>& stamps) noexcept {
        compaction_kind_t const kind = kind_of_(request.upper_order, request.depth);
        for (sorted_run_t const* source : sources) {
            range_tombstones_t const& tombstones = source->range_tombstones();
            for (std::size_t index = 0; index != tombstones.size(); ++index) {
                if (drops_range_tombstone(kind, request.stamps, tombstones.stamp(index))) continue;
                if (ranges.push_back(range_t {tombstones.range(index)}) != st::status_t::success_k ||
                    stamps.push_back(std::uint64_t {tombstones.stamp(index)}) != st::status_t::success_k)
                    return {status_code_t::out_of_memory_k, "A merge could not keep its range tombstones"};
            }
        }
        return {};
    }

    /** The pieces of @p ranges, stamped by @p stamps, that fall from @p lower up to @p upper,
     *  exclusive: what one run of a merge carries, where the least and the greatest key of the type
     *  leave a side open. */
    static expected<range_tombstones_t> clipped_range_tombstones_(std::span<range_t const> ranges,
                                                                  std::span<std::uint64_t const> stamps,
                                                                  stored_key_t lower, stored_key_t upper) noexcept {
        st::vector<range_t> pieces;
        st::vector<std::uint64_t> pieces_stamps;
        if (pieces.reserve(ranges.size()) != st::status_t::success_k ||
            pieces_stamps.reserve(ranges.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A merge could not cut its range tombstones"};
        for (std::size_t index = 0; index != ranges.size(); ++index) {
            range_t piece = ranges[index];
            if (piece.start < lower) piece.start = lower;
            if (upper < piece.end) piece.end = upper;
            if (!(piece.start < piece.end)) continue;
            [[maybe_unused]] st::status_t const cut = pieces.push_back(st::assume_reserved, range_t {piece});
            [[maybe_unused]] st::status_t const stamped = pieces_stamps.push_back(st::assume_reserved,
                                                                                  std::uint64_t {stamps[index]});
        }
        return range_tombstones_t::make(std::span<range_t const> {pieces},
                                        std::span<std::uint64_t const> {pieces_stamps});
    }

    /** How many slices a merge of @p upper into @p lower runs in: as many as the request's fan-out
     *  offers, where its tree keeps buffers for each and the largest source has a key for every
     *  cut, and one where a device ranks the whole merge. */
    [[nodiscard]] static std::size_t slices_of_(merge_request_t const& request,
                                                std::span<sorted_run_index_view<key_slot_t> const> upper,
                                                std::span<sorted_run_index_view<key_slot_t> const> lower) noexcept {
        std::size_t const offered = request.fan_out.slices();
        if (offered < 2 || request.buffers.size() < offered) return 1;
        bool changes = false;
        std::uint64_t const merged = merged_count_(upper, lower, changes);
        std::size_t const devices = std::holds_alternative<cuda_compactor_t*>(request.compactor) ? 1 : 0;
        if (!carries_range_tombstones(upper, lower) &&
            choose_compactor(request.placement, devices, merged) == compactor_choice_t::device_k)
            return 1;
        std::uint64_t largest = 0;
        for (sorted_run_index_view<key_slot_t> const& view : upper) largest = fu::max_of(largest, view.key_slots_count);
        for (sorted_run_index_view<key_slot_t> const& view : lower) largest = fu::max_of(largest, view.key_slots_count);
        return largest < offered ? 1 : offered;
    }

    /** The first position of @p view whose key is not below @p stored_key. */
    [[nodiscard]] static std::uint64_t lower_bound_(sorted_run_index_view<key_slot_t> const& view,
                                                    stored_key_t const& stored_key) noexcept {
        std::uint64_t low = 0;
        std::uint64_t high = view.key_slots_count;
        while (low < high) {
            std::uint64_t const middle = low + (high - low) / 2;
            if (key_slot_of(view, view.first + middle).stored_key < stored_key) low = middle + 1;
            else high = middle;
        }
        return low;
    }

    /** Cuts the merge of @p views into one slice per buffer of @p buffers, at keys spread evenly
     *  over the largest source, each slice viewing every source from the first key at or above its
     *  cut, so every version of one key falls in one slice. */
    static status_t cut_slices_(std::span<sorted_run_index_view<key_slot_t> const> views,
                                std::span<merge_buffers_t> buffers) noexcept {
        std::size_t largest = 0;
        for (std::size_t index = 0; index != views.size(); ++index)
            if (views[index].key_slots_count > views[largest].key_slots_count) largest = index;
        for (merge_buffers_t& sliced : buffers) {
            sliced.slice_views.clear();
            if (sliced.slice_views.resize(views.size()) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A merge could not track its slices"};
        }
        std::uint64_t const largest_count = views[largest].key_slots_count;
        for (std::size_t source = 0; source != views.size(); ++source) {
            std::uint64_t begin = 0;
            for (std::size_t slice = 0; slice != buffers.size(); ++slice) {
                std::uint64_t end = views[source].key_slots_count;
                if (slice + 1 != buffers.size()) {
                    std::uint64_t const cut = (slice + 1) * largest_count / buffers.size();
                    end = fu::max_of(begin, lower_bound_(views[source], key_slot_of(views[largest], cut).stored_key));
                }
                sorted_run_index_view<key_slot_t>& share = buffers[slice].slice_views[source];
                share = views[source];
                share.first = views[source].first + begin;
                share.key_slots_count = end - begin;
                begin = end;
            }
        }
        return {};
    }

    /** Runs @p slice once per slice: on the calling thread over @p reading and @p writing for a
     *  merge of one, and through @p fan_out otherwise. */
    template <typename slice_type_>
    static status_t run_slices_(compaction_fan_out_t const& fan_out, std::size_t slices, io_channel_t& reading,
                                io_channel_t& writing, slice_type_ const& slice) noexcept {
        if (slices == 1) return slice(std::size_t {0}, reading, writing);
        return fan_out.run(slice);
    }

    /** Lays out every run the compaction in @p buffers planned, after dropping the runs it
     *  laid out before. */
    static status_t lay_out_runs_(merge_buffers_t& buffers, merge_request_t const& request) noexcept {
        buffers.laid_out_runs.clear();
        compaction_output_t const& output = buffers.output;
        if (buffers.laid_out_runs.reserve(output.sorted_runs.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A merge could not track the runs it writes"};
        planned_run_t planned;
        for (compacted_sorted_run_t const& cut : output.sorted_runs) {
            planned.key_slots_count = cut.key_slots_count;
            planned.stored_values_bytes = cut.stored_values_bytes;
            planned.copy_ranges_count = cut.value_copy_ranges_count;
            if (status_t const laid = lay_out_run_(buffers, planned, request); laid.failed()) return laid;
            planned.first_key_slot += planned.key_slots_count;
            planned.first_copy_range += planned.copy_ranges_count;
        }
        return {};
    }

    /** Packs the stamps and kinds of the run @p planned names, saves the tree of its keys, rebases
     *  its value offsets and fills its header but for its name and tombstones, appending it to the
     *  runs @p buffers lays out. */
    static status_t lay_out_run_(merge_buffers_t& buffers, planned_run_t const& planned,
                                 merge_request_t const& request) noexcept {
        compaction_output_t const& output = buffers.output;
        laid_out_run_t laid;
        laid.planned = planned;

        // The compactor already raised every stamp to the floor, so the column packs them
        // as they are.
        std::uint64_t const floor = request.stamps.floor;
        std::uint64_t newest = floor;
        auto [column, pack_status] = pack_stamps(
            std::span<std::uint64_t const> {output.stamps.data() + planned.first_key_slot,
                                            static_cast<std::size_t>(planned.key_slots_count)},
            floor, newest);
        if (pack_status.failed()) return pack_status;
        auto [kinds, pack_kinds_status] = pack_kinds(std::span<std::uint8_t const> {
            output.kinds.data() + planned.first_key_slot, static_cast<std::size_t>(planned.key_slots_count)});
        if (pack_kinds_status.failed()) return pack_kinds_status;

        sorted_run_header_t& header = laid.header;
        header.magic = sorted_run_magic_k;
        header.format_version = sorted_run_format_version_k;
        header.key_slot_bytes = static_cast<std::uint32_t>(sizeof(key_slot_t));
        header.key_slots_count = planned.key_slots_count;
        header.stored_values_bytes = holds_values_inline_k ? 0 : planned.stored_values_bytes;
        header.value_max_size = request.value_max_size;
        header.branching = static_cast<std::uint16_t>(sorted_run_layout_t::branching_k);
        header.value_layout = value_layout_k;
        header.collection = request.collection;
        header.stamps_bytes = column.saved_bytes();
        header.stamps_floor = floor;
        header.newest_stamp = newest;
        header.kinds_bytes = kinds.saved_bytes();
        laid.stamps = std::move(column);
        laid.kinds = std::move(kinds);

        // The run's own value offsets from its first key slot on, none but for a variable width.
        if (!output.value_offsets.empty()) {
            std::uint64_t const begin = output.value_offsets[planned.first_key_slot];
            if (laid.value_offsets.resize(static_cast<std::size_t>(planned.key_slots_count + 1)) !=
                st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A merge could not lay out a run's value offsets"};
            for (std::uint64_t index = 0; index != laid.value_offsets.size(); ++index)
                laid.value_offsets[index] = static_cast<stored_value_offset_t>(
                    output.value_offsets[planned.first_key_slot + index] - begin);
        }

        std::span<key_slot_t const> const cells {output.cells.data() + planned.first_key_slot,
                                                 static_cast<std::size_t>(planned.key_slots_count)};
        if (status_t const saved = sorted_run_t::save_keys(cells, laid.keys); saved.failed()) return saved;
        if (buffers.laid_out_runs.push_back(std::move(laid)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A merge could not track the runs it writes"};
        return {};
    }

    /**
     *  @brief Names every run @p buffers laid out, gives each the pieces of @p ranges
     *      between its first key and the next run's, and allocates its extent, in key order
     *      on the calling thread.
     *
     *  The runs' bounds stay disjoint, so the first run takes every piece below its first key and
     *  the last every piece above. A failure gives back every extent allocated so far.
     */
    static status_t place_runs_(volume_t& volume, std::span<merge_buffers_t> buffers, std::span<range_t const> ranges,
                                std::span<std::uint64_t const> stamps) noexcept {
        laid_out_run_t* previous = nullptr;
        stored_key_t previous_lower = limit<stored_key_t>::min_k;
        for (merge_buffers_t& sliced : buffers)
            for (laid_out_run_t& laid : sliced.laid_out_runs) {
                if (previous != nullptr) {
                    stored_key_t const first_key = sliced.output.cells[laid.planned.first_key_slot].stored_key;
                    if (status_t const placed = place_run_(volume, *previous, ranges, stamps, previous_lower,
                                                           first_key);
                        placed.failed()) {
                        abandon_runs_(volume, buffers);
                        return placed;
                    }
                    previous_lower = first_key;
                }
                previous = &laid;
            }
        if (previous == nullptr) return {};
        status_t const placed = place_run_(volume, *previous, ranges, stamps, previous_lower,
                                           limit<stored_key_t>::max_k);
        if (placed.failed()) abandon_runs_(volume, buffers);
        return placed;
    }

    /** Names @p laid, gives it the pieces of @p ranges from @p lower up to @p upper, and
     *  allocates its extent. */
    static status_t place_run_(volume_t& volume, laid_out_run_t& laid, std::span<range_t const> ranges,
                               std::span<std::uint64_t const> stamps, stored_key_t lower, stored_key_t upper) noexcept {
        uuid128_t const content_id = uuid_generator();
        if (!content_id.is_valid()) return {status_code_t::internal_k, "A merge could not name the run it writes"};
        auto [pieces, pieces_status] = clipped_range_tombstones_(ranges, stamps, lower, upper);
        if (pieces_status.failed()) return pieces_status;
        laid.tombstones = std::move(pieces);
        if (status_t const laid_out = sorted_run_t::lay_out_range_tombstones(laid.tombstones, laid.laid_tombstones);
            laid_out.failed())
            return laid_out;
        laid.header.content_id = content_id;
        laid.header.range_tombstones_count = static_cast<std::uint32_t>(laid.tombstones.size());
        laid.header.range_tombstones_bytes = static_cast<std::uint32_t>(laid.laid_tombstones.size());
        auto [extent, allocate_status] = volume.allocate(content_id, sorted_run_layout_t::blob_bytes(laid.header));
        if (allocate_status.failed()) return allocate_status;
        laid.extent = extent;
        return {};
    }

    /** Gives back the extent of every run @p buffers laid out that no level took. */
    static void abandon_runs_(volume_t& volume, std::span<merge_buffers_t> buffers) noexcept {
        for (merge_buffers_t& sliced : buffers)
            for (laid_out_run_t& laid : sliced.laid_out_runs)
                if (laid.extent.extent.length != 0) {
                    volume.deallocate(laid.extent);
                    laid.extent = medium_extent_t {};
                }
    }

    /** Writes the header, the cells and the saved tree of their keys, the value offsets, the
     *  gathered values, the stamps and the kinds of one new run, and reaps every write, leaving the
     *  flush to the manifest that names it. */
    static status_t write_sections_(volume_t& volume, io_channel_t& reading_channel, io_channel_t& writing_channel,
                                    std::span<sorted_run_t const* const> sources, merge_buffers_t& buffers,
                                    planned_run_t const& planned, sorted_run_header_t const& header,
                                    stamps_column_t const& column, kinds_column_t const& kinds,
                                    std::span<key_slot_t const> cells, std::span<std::uint64_t const> keys,
                                    std::span<stored_value_offset_t const> value_offsets,
                                    std::span<byte_t const> tombstones, medium_extent_t const& blob,
                                    cancellation_t const* cancellation) noexcept {
        std::uint64_t const block_bytes = round_down_to_multiple(writing_channel.streams().buffer_bytes(),
                                                                 portable_alignment_k);
        if (block_bytes < sorted_run_layout_t::header_bytes())
            return {status_code_t::capacity_exhausted_k, "A channel's streaming block is shorter than a run's header"};

        auto [writer_channel, writer_channel_status] = writing_channel.acquire();
        if (writer_channel_status.failed()) return writer_channel_status;
        write_pipeline_t writer {volume.disk(), std::move(writer_channel), blob.medium};
        channel_drain<disk_t> const parked {volume.disk(), writing_channel};
        if (stopped_(cancellation)) return cancelled_status_();

        std::uint64_t const head_bytes = sorted_run_layout_t::header_bytes();
        auto [head, acquire_buffer_or_poll_status] = acquire_buffer_or_poll_(writer);
        if (acquire_buffer_or_poll_status.failed()) return acquire_buffer_or_poll_status;
        std::memset(head.bytes().data(), 0, static_cast<std::size_t>(head_bytes));
        std::memcpy(head.bytes().data(), &header, sizeof(header));
        if (status_t const queued = writer.enqueue_write(std::move(head), blob.slice(0, head_bytes)); queued.failed())
            return queued;
        if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;

        if (status_t const index = sorted_run_t::write_index(writer, blob, header, cells, keys); index.failed())
            return index;
        if (status_t const offsets = sorted_run_t::write_value_offsets(writer, blob, header, value_offsets);
            offsets.failed())
            return offsets;
        if (status_t const values = write_values_(volume, reading_channel, writer, sources, buffers, planned, header,
                                                  blob, block_bytes, cancellation);
            values.failed())
            return values;
        if (status_t const trailer = sorted_run_t::write_trailer(writer, blob, header, column, kinds); trailer.failed())
            return trailer;
        if (status_t const ranges = sorted_run_t::write_range_tombstones(writer, blob, header, tombstones);
            ranges.failed())
            return ranges;
        return writer.poll_all_completions();
    }

    /**
     *  @brief Gives every source run's window its room: an equal share of the reading channel's
     *      free streaming blocks, whole shares to a block, or of the heap where no share would hold
     *      a read block.
     *  @param[in] read_block Bytes one read of the reading channel's request blocks moves.
     *  @param[out] window_blocks The streaming blocks the shares are cut from, which the caller
     *      holds until every read into them is drained.
     *  @return The bytes every window holds.
     */
    static expected<std::uint64_t> place_windows_(io_channel_t& reading_channel, std::uint64_t read_block,
                                                  merge_buffers_t& buffers,
                                                  st::vector<pooled_buffer_t>& window_blocks) noexcept {
        st::vector<source_window_t>& windows = buffers.source_windows;
        std::size_t const sources_count = windows.size();
        utilities::buffer_pool_t& streams = reading_channel.streams();
        std::size_t const free_blocks = streams.available_buffers();
        std::size_t const per_block = divide_round_up(fu::max_of<std::size_t>(sources_count, 1),
                                                      fu::max_of<std::size_t>(free_blocks, 1));
        std::uint64_t const share_bytes = round_down_to_multiple(streams.buffer_bytes() / per_block,
                                                                 portable_alignment_k);
        if (free_blocks != 0 && share_bytes >= read_block) {
            std::size_t const blocks_count = divide_round_up(sources_count, per_block);
            if (window_blocks.reserve(blocks_count) != st::status_t::success_k)
                return status_t {status_code_t::out_of_memory_k, "A merge could not track its windows"};
            for (std::size_t index = 0; index != blocks_count; ++index) {
                auto [held, acquire_status] = streams.acquire();
                if (acquire_status.failed()) return acquire_status;
                [[maybe_unused]] st::status_t const kept = window_blocks.push_back(st::assume_reserved,
                                                                                   std::move(held));
            }
            for (std::size_t run = 0; run != sources_count; ++run) {
                pooled_buffer_t const& held = window_blocks[run / per_block];
                std::uint64_t const offset = (run % per_block) * share_bytes;
                windows[run].bytes = held.bytes().subspan(static_cast<std::size_t>(offset),
                                                          static_cast<std::size_t>(share_bytes));
                windows[run].place = held.reference(offset);
            }
            return share_bytes;
        }

        // The windows split the streaming budget on the heap instead, filled through
        // the request blocks
        std::uint64_t const stream_budget = streams.buffer_bytes() * streams.total_buffers();
        std::uint64_t const window_bytes = fu::max_of(
            read_block, round_down_to_multiple(stream_budget / fu::max_of<std::size_t>(sources_count, 1), read_block));
        st::vector<byte_t>& heap = buffers.heap_windows;
        heap.clear();
        if (heap.resize(st::assume_uninitialized, static_cast<std::size_t>(window_bytes * sources_count)) !=
            st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A merge could not hold the windows it reads through"};
        for (std::size_t run = 0; run != sources_count; ++run)
            windows[run].bytes = std::span<byte_t> {heap}.subspan(static_cast<std::size_t>(run * window_bytes),
                                                                  static_cast<std::size_t>(window_bytes));
        return window_bytes;
    }

    /**
     *  @brief Gathers the run's value copy ranges out of their source runs and the folded bytes
     *      straight into the blocks written back to back, a range crossing from one block into the
     *      next split between them.
     *
     *  A merge copies each source's values in the order they sit, so every source streams through a
     *  window of its own and a range the window can't hold is read on its own into the block it
     *  lands in. A window holds two halves over consecutive stretches: once the copy crosses into
     *  the half ahead, the half behind is refilled with the stretch after it, so reads run ahead of
     *  the copy and a source waits only where it outruns them. A window shares the reading
     *  channel's streaming blocks, idle while values are gathered, and a half refills by one read
     *  straight from the drive, so a value's bytes are copied once, from window to block. Where
     *  those blocks give no source a share of a read block, the windows live on the heap and fill
     *  through the request blocks. A block waits only for the reads landing in it before it is
     *  written, and up to every streaming block of the writing channel is written while the next
     *  one is gathered.
     */
    static status_t write_values_(volume_t& volume, io_channel_t& reading_channel, write_pipeline_t& writer,
                                  std::span<sorted_run_t const* const> sources, merge_buffers_t& buffers,
                                  planned_run_t const& planned, sorted_run_header_t const& header,
                                  medium_extent_t const& blob, std::uint64_t block_bytes,
                                  cancellation_t const* cancellation) noexcept {
        if constexpr (holds_values_inline_k) return {};
        else {
            std::uint64_t const section = sorted_run_layout_t::values_section_bytes(header.stored_values_bytes);
            if (section == 0) return {};
            std::uint64_t const values_offset = sorted_run_layout_t::values_offset(header.key_slots_count);
            std::uint64_t const read_block = round_down_to_multiple(reading_channel.requests().buffer_bytes(),
                                                                    portable_alignment_k);
            if (read_block == 0)
                return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than one page"};

            compaction_output_t const& output = buffers.output;
            std::size_t const depth = reading_channel.queue_pair().depth();
            st::vector<pending_piece_t>& pending = buffers.pending_pieces;
            st::vector<source_window_t>& windows = buffers.source_windows;
            pending.clear();
            windows.clear();
            // A piece is written as its read is queued, while every window starts empty
            if (pending.resize(st::assume_uninitialized, depth) != st::status_t::success_k ||
                windows.resize(sources.size()) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A merge could not track its reads in flight"};

            // Both the windows' blocks and the block being gathered outlive the drain of every read
            // landing in them
            st::vector<pooled_buffer_t> window_blocks;
            pooled_buffer_t block;
            std::span<byte_t> room;
            auto [window_bytes, place_windows_status] = place_windows_(reading_channel, read_block, buffers,
                                                                       window_blocks);
            if (place_windows_status.failed()) return place_windows_status;

            auto [reader_channel, reader_channel_status] = reading_channel.acquire();
            if (reader_channel_status.failed()) return reader_channel_status;
            read_pipeline_t reader {volume.disk(), std::move(reader_channel), medium_reference_t {}};
            channel_drain<disk_t> const parked {volume.disk(), reading_channel};

            std::uint64_t const half_bytes = round_down_to_multiple(window_bytes / 2, portable_alignment_k);
            // Reads landing in the block being gathered, which it waits for before it is written
            std::size_t block_reads = 0;
            status_t first;
            auto sink = [&](completed_buffer_t const& completed) noexcept {
                pending_piece_t const& piece = pending[completed.tag % depth];
                if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                    if (first.succeeded()) first = whole;
                    return;
                }
                if (piece.source_run == sources.size()) --block_reads;
                else --windows[piece.source_run].halves[piece.destination < half_bytes ? 0 : 1].outstanding;
                if (piece.carried == 0) return;
                byte_t* const into = piece.source_run != sources.size() ? windows[piece.source_run].bytes.data()
                                                                        : room.data();
                std::memcpy(into + piece.destination, completed.bytes.data() + piece.skipped, piece.carried);
            };

            request_tag_t tag = 0;
            // Submits what is queued and lands whatever completed, which frees room for
            // the next read
            auto make_room = [&]() noexcept -> status_t {
                if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
                auto [polled, poll_completions_status] = reader.poll_completions(sink);
                if (poll_completions_status.failed()) return poll_completions_status;
                return first;
            };

            // Waits for every read of @p half, landing whatever else completes meanwhile
            auto settle = [&](window_half_t const& half) noexcept -> status_t {
                while (half.outstanding != 0)
                    if (status_t const made = make_room(); made.failed()) return made;
                return {};
            };

            // Points half @p index of source @p run's window at the bytes from @p begin to @p end,
            // once its earlier reads landed, and submits their reads: one straight into a streaming
            // share, or a read block at a time onto the heap
            auto refill = [&](std::uint32_t run, std::size_t index, std::uint64_t begin,
                              std::uint64_t end) noexcept -> status_t {
                source_window_t& window = windows[run];
                window_half_t& half = window.halves[index];
                if (status_t const settled = settle(half); settled.failed()) return settled;
                half.begin = begin, half.end = end;
                if (begin == end) return {};
                sorted_run_t const& source = *sources[run];
                extent_t const span = source.extent().slice(begin, end - begin);
                if (span.length != end - begin)
                    return {status_code_t::malformed_k, "A source run is shorter than the values it copies"};
                std::uint64_t const half_offset = index * half_bytes;
                bool const shared = window.place.memory_id != utilities::registered_memory_id_t::unregistered_k;
                std::uint64_t const step = shared ? span.length : read_block;
                for (std::uint64_t offset = 0; offset < span.length;) {
                    if (shared ? !reader.can_enqueue_into() : !reader.can_enqueue()) {
                        if (status_t const made = make_room(); made.failed()) return made;
                        continue;
                    }
                    std::uint64_t const length = fu::min_of(step, span.length - offset);
                    extent_t const piece {span.offset + offset, length};
                    pending[tag % depth] = pending_piece_t {static_cast<std::uint32_t>(half_offset + offset), 0,
                                                            static_cast<std::uint32_t>(shared ? 0 : length), run};
                    memory_reference_t into = window.place;
                    into.offset += half_offset;
                    status_t const queued = shared ? reader.enqueue_read(source.extent().medium, piece, into,
                                                                         window.bytes.subspan(half_offset, length), tag)
                                                   : reader.enqueue_read(source.extent().medium, piece, tag);
                    if (queued.failed()) return queued;
                    ++tag, ++half.outstanding;
                    offset += length;
                }
                return reader.submit();
            };

            // The half of source @p run's window holding byte @p at of its extent: the half ahead,
            // refilling the half behind with the stretch after it as the copy crosses in, the half
            // behind, or both moved to @p at
            auto half_holding = [&](std::uint32_t run, std::uint64_t at,
                                    std::uint64_t section_end) noexcept -> expected<std::size_t> {
                source_window_t const& window = windows[run];
                std::size_t const ahead = window.halves[1].begin > window.halves[0].begin ? 1 : 0;
                window_half_t const& leading = window.halves[ahead];
                if (leading.begin <= at && at < leading.end) {
                    if (window.halves[1 - ahead].begin < leading.begin && leading.end != section_end)
                        if (status_t const refilled = refill(run, 1 - ahead, leading.end,
                                                             fu::min_of(leading.end + half_bytes, section_end));
                            refilled.failed())
                            return refilled;
                    return ahead;
                }
                window_half_t const& trailing = window.halves[1 - ahead];
                if (trailing.begin <= at && at < trailing.end) return 1 - ahead;
                std::uint64_t const begin = round_down_to_multiple(at, portable_alignment_k);
                std::uint64_t const middle = fu::min_of(begin + half_bytes, section_end);
                if (status_t const refilled = refill(run, 0, begin, middle); refilled.failed()) return refilled;
                if (status_t const refilled = refill(run, 1, middle, fu::min_of(middle + half_bytes, section_end));
                    refilled.failed())
                    return refilled;
                return std::size_t {0};
            };

            std::uint64_t range = planned.first_copy_range;
            std::uint64_t const ranges_end = planned.first_copy_range + planned.copy_ranges_count;
            // Bytes of the current range the blocks before this one already took
            std::uint64_t range_gathered = 0;
            for (std::uint64_t written = 0; written != section;) {
                if (stopped_(cancellation)) {
                    volume.disk().cancel(reading_channel.queue_pair());
                    return cancelled_status_();
                }
                std::uint64_t const chunk = block_bytes < section - written ? block_bytes : section - written;
                auto [acquired, acquire_buffer_or_poll_status] = acquire_buffer_or_poll_(writer);
                if (acquire_buffer_or_poll_status.failed()) return acquire_buffer_or_poll_status;
                block = std::move(acquired);
                room = block.bytes().first(static_cast<std::size_t>(chunk));

                std::uint64_t gathered = 0;
                while (gathered != chunk && range != ranges_end) {
                    value_copy_range_t const& copied = output.value_copy_ranges[range];
                    std::uint64_t const taken = fu::min_of<std::uint64_t>(chunk - gathered,
                                                                          copied.bytes - range_gathered);
                    // Values a fold wrote are copied out of the compaction's own bytes.
                    if (copied.source_run == sources.size())
                        std::memcpy(room.data() + gathered, output.folded.data() + copied.source_begin + range_gathered,
                                    static_cast<std::size_t>(taken));
                    else {
                        sorted_run_t const& source = *sources[copied.source_run];
                        std::uint64_t const range_begin =
                            sorted_run_layout_t::values_offset(source.population_count()) + copied.source_begin;
                        if (copied.bytes + portable_alignment_k <= window_bytes) {
                            source_window_t const& window = windows[copied.source_run];
                            std::uint64_t const section_end = round_up_to_multiple(
                                sorted_run_layout_t::stamps_offset(source.population_count(),
                                                                   source.header().stored_values_bytes),
                                portable_alignment_k);
                            // A range crossing from one half into the other copies out of
                            // each in turn
                            for (std::uint64_t carried = 0; carried != taken;) {
                                std::uint64_t const at = range_begin + range_gathered + carried;
                                auto [index, half_status] = half_holding(copied.source_run, at, section_end);
                                if (half_status.failed()) return half_status;
                                window_half_t const& half = window.halves[index];
                                if (status_t const settled = settle(half); settled.failed()) return settled;
                                std::uint64_t const bytes = fu::min_of(taken - carried, half.end - at);
                                std::memcpy(room.data() + gathered + carried,
                                            window.bytes.data() + index * half_bytes + (at - half.begin),
                                            static_cast<std::size_t>(bytes));
                                carried += bytes;
                            }
                        }
                        else {
                            // A range no window holds is read a request block at a time into the
                            // block it lands in
                            std::uint64_t const from = range_begin + range_gathered;
                            for (std::uint64_t carried = 0; carried != taken;) {
                                if (!reader.can_enqueue()) {
                                    if (status_t const made = make_room(); made.failed()) return made;
                                    continue;
                                }
                                std::uint64_t const aligned = round_down_to_multiple(from + carried,
                                                                                     portable_alignment_k);
                                std::uint64_t const skipped = from + carried - aligned;
                                std::uint64_t const bytes = fu::min_of(taken - carried, read_block - skipped);
                                std::uint64_t const length = round_up_to_multiple(skipped + bytes,
                                                                                  portable_alignment_k);
                                extent_t const piece = source.extent().slice(aligned, length);
                                if (piece.length != length)
                                    return {status_code_t::malformed_k,
                                            "A source run is shorter than the values it copies"};
                                pending[tag % depth] = pending_piece_t {
                                    static_cast<std::uint32_t>(gathered + carried), static_cast<std::uint32_t>(skipped),
                                    static_cast<std::uint32_t>(bytes), static_cast<std::uint32_t>(sources.size())};
                                if (status_t const queued = reader.enqueue_read(source.extent().medium, piece, tag);
                                    queued.failed())
                                    return queued;
                                ++tag, ++block_reads;
                                carried += bytes;
                            }
                        }
                    }
                    gathered += taken;
                    range_gathered += taken;
                    if (range_gathered == copied.bytes) ++range, range_gathered = 0;
                }
                // The block waits for the reads landing in it alone, while every window
                // keeps reading ahead
                while (block_reads != 0)
                    if (status_t const made = make_room(); made.failed()) return made;
                if (first.failed()) return first;

                std::memset(room.data() + gathered, 0, static_cast<std::size_t>(chunk - gathered));
                if (status_t const queued = writer.enqueue_write(std::move(block),
                                                                 blob.slice(values_offset + written, chunk));
                    queued.failed())
                    return queued;
                if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;
                written += chunk;
            }
            return first;
        }
    }

#pragma endregion Private Methods
};

#pragma endregion Merger

#pragma endregion LSM Level Merger

} // namespace unum::ustore::blobs
