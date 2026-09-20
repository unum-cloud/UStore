/**
 *  @file src/nvme/lsm_level_merger.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The compaction that merges one level into the one below it, on the host or on a CUDA device.
 *
 *  The merge itself is arithmetic over key slots and belongs to @c compaction.cuh, which decides who
 *  survives, cuts the survivors into runs that fit their bounds and plans where every value moves. What
 *  is left here is the I/O: pointing the compactor at each source run's index, then writing the planned
 *  runs out of the plan it answered.
 *
 *  Reads and writes ride separate channels because a channel is one queue pair with one completion queue, so a read
 *  pipeline polling on the channel a write pipeline submits on would swallow the writes' completions. The
 *  write side is one pipeline whose depth is the streaming blocks the channel was mounted with, and the read
 *  side refills the ring as completions retire rather than waiting on a batch.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memmove`, `std::memset`

#include <span>    // `std::span`
#include <utility> // `std::move`

#include <forkunion/atomics.hpp>       // `fu::preferred_atomic_ref`
#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/buffer_pool.hpp"    // `pooled_buffer_t`
#include "nvme/catalog.hpp"        // `blob_kind_t`
#include "nvme/compaction.cuh"     // `cpu_compactor_t`, `compaction_output`, `compaction_kind_t`
#include "nvme/extent.hpp"         // `extent_t`, `rounded_up_to`, `portable_alignment_k`
#include "nvme/io_channel.hpp"     // `io_channel_t`
#include "nvme/io_pipeline.hpp"    // `basic_read_pipeline`, `basic_write_pipeline`, `require_completed_in_full`
#include "nvme/lsm_level.hpp"      // `lsm_level`, `lsm_level_order_t`, `lsm_level_depth_t`
#include "nvme/lsm_sorted_run.hpp" // `lsm_sorted_run`, `sorted_run_header_t`, `sorted_run_layout`
#include "nvme/storage_limits.hpp" // `storage_limits_t`
#include "nvme/uuid.hpp"           // `uuid_t`, `uuid_generator`

namespace unum::ustore::nvme {

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

    /** @c cpu_compactor_t, on the calling thread. */
    host_k,

    /** @c gpu_compactor_t, on the first CUDA device. */
    device_k,
};

/**
 *  @brief The word a caller raises to stop a merge, which the merge reads between blocks.
 *
 *  A cancelled merge leaves the level set it was asked to replace untouched and returns every block and
 *  every reservation it took, so cancelling one costs the work it had done and nothing else.
 */
class compaction_cancellation_t {
  public:

    compaction_cancellation_t() noexcept = default;
    compaction_cancellation_t(compaction_cancellation_t const&) = delete;
    compaction_cancellation_t& operator=(compaction_cancellation_t const&) = delete;

    /** Raises the word, which the merge reads at its next block boundary. */
    void raise() noexcept { fu::preferred_atomic_ref<std::int32_t> {raised_}.store(1, std::memory_order_release); }

    /** Whether a caller has raised the word. */
    [[nodiscard]] bool cancelled() const noexcept {
        return fu::preferred_atomic_ref<std::int32_t const> {raised_}.load(std::memory_order_acquire) != 0;
    }

  private:

    /** Zero while the merge may go on, and one once a caller has raised it. */
    std::int32_t raised_ = 0;
};

/** Which compactor @p placement asks for, given @p devices devices and a merge of @p key_slots_count key slots. */
[[nodiscard]] constexpr compactor_choice_t choose_compactor(compaction_placement_t placement, std::size_t devices,
                                                            std::uint64_t key_slots_count) noexcept {
    if (placement == compaction_placement_t::host_k) return compactor_choice_t::host_k;
    if (placement == compaction_placement_t::device_k) return compactor_choice_t::device_k;
    return devices != 0 && key_slots_count > storage_limits_t::gpu_compaction_threshold_k ? compactor_choice_t::device_k
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
    case st::status_t::input_output_k:
        return {status_code_t::input_output_k, "A compaction could not move its key slots"};
    default: return {status_code_t::device_k, "A compaction failed on the device it ran on"};
    }
}

/** Drains a channel's queue pair before the pipelines above it hand their blocks back, which a failure needs too. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
class channel_drain_t {
  public:

    channel_drain_t(disk_type_& disk, io_channel_t& channel) noexcept : disk_(disk), channel_(channel) {}
    channel_drain_t(channel_drain_t const&) = delete;
    channel_drain_t& operator=(channel_drain_t const&) = delete;

    ~channel_drain_t() noexcept { [[maybe_unused]] status_t const drained = disk_.drain(channel_.queue_pair()); }

  private:

    /** The ring the queue pair belongs to. */
    disk_type_& disk_;

    /** The channel whose queue pair is drained. */
    io_channel_t& channel_;
};

#pragma endregion Vocabulary

#pragma region Merger

/**
 *  @brief Merges one level into the level below it, writing a whole new sorted level.
 *
 *  Both source levels are consumed whole rather than by the ranges they overlap, so the runs the new
 *  level holds are the only survivors and every source run is free the moment the merge is published.
 */
template <typename key_slot_type_, typename volume_type_>
class lsm_level_merger {
  public:

#pragma region Types

    /** The key slot the runs of both levels hold. */
    using key_slot_t = key_slot_type_;

    /** The volume the runs live on. */
    using volume_t = volume_type_;

    /** The level this merge reads and writes. */
    using level_t = lsm_level<key_slot_t, volume_t>;

    /** One immutable run of a level. */
    using sorted_run_t = typename level_t::sorted_run_t;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The transport the volume moves bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_type_&>().disk())>;

    /** Where the sections of a run sit inside its blob. */
    using sorted_run_layout_t = sorted_run_layout<key_slot_t>;

    /** The pipeline the gather reads its source values through. */
    using read_pipeline_t = basic_read_pipeline<disk_t>;

    /** The pipeline every section of every new run is written through. */
    using write_pipeline_t = basic_write_pipeline<disk_t>;

    /** What one compaction answers: the new runs, their cells and where their values come from. */
    using compaction_output_t = compaction_output<key_slot_t>;

    /** How the values of these runs are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so a merge moves no value bytes. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

    /** What one merge is asked to do, beyond the volume and the channels it is handed. */
    struct merge_request_t {

        /** The level whose runs shadow the lower level's, which is never null and never empty. */
        level_t const* upper = nullptr;

        /** The level the upper one merges into, or null when the merge writes a level that holds nothing yet. */
        level_t const* lower = nullptr;

        /** The number the new level takes. */
        std::uint32_t number = 0;

        /** Whether anything sits below the new level, which decides whether tombstones drop. */
        lsm_level_depth_t depth = lsm_level_depth_t::intermediate_k;

        /** The largest value the collection admits, which every new run's header carries. */
        stored_value_size_t value_max_size = 0;

        /** Most key slots one new run takes, which bounds how far a binary search inside it walks. */
        std::uint64_t run_key_slots_capacity = storage_limits_t::write_buffer_elements_max_count_k;

        /** Largest new run in bytes, which a run of one key slot may still exceed. */
        std::uint64_t run_bytes_limit = storage_limits_t::sorted_run_file_max_size_k;

        /** Where the compaction runs. */
        compaction_placement_t placement = compaction_placement_t::automatic_k;

        /** The word the merge reads between blocks, or null for a merge nobody stops. */
        compaction_cancellation_t const* cancellation = nullptr;
    };

#pragma endregion Types

#pragma region Merging

    /**
     *  @brief Merges @p request.upper into @p request.lower and writes the survivors as a new sorted level.
     *  @param[inout] volume The volume the new runs are reserved on, which every source run also lives on.
     *  @param[inout] reading_channel The channel the source values are gathered on, held by the calling thread.
     *  @param[inout] writing_channel The channel the new runs are written on, which must not be @p reading_channel.
     *  @param[in] request Which levels merge, what the new runs may grow to, and where the compaction runs.
     *  @return The new level, or the status that stopped the merge with every new run already dropped.
     */
    static expected<level_t> merge(volume_t& volume, io_channel_t& reading_channel, io_channel_t& writing_channel,
                                   merge_request_t const& request) noexcept {
        if (request.upper == nullptr)
            return status_t {status_code_t::invalid_argument_k, "A merge reads an upper level"};
        if (&reading_channel == &writing_channel)
            return status_t {status_code_t::invalid_argument_k, "A merge reads and writes on two channels"};

        st::basic_vector<sorted_run_t const*> sources;
        st::basic_vector<sorted_run_index_view<key_slot_t>> views;
        if (status_t const staged = stage_indexes_(*request.upper, request.lower, sources, views); staged.failed())
            return staged;

        std::size_t const upper_runs = request.upper->size();
        std::span<sorted_run_index_view<key_slot_t> const> const all {views};
        compaction_output_t output;
        if (status_t const compacted = run_compaction_(kind_of_(request.upper->order(), request.depth),
                                                       all.first(upper_runs), all.subspan(upper_runs),
                                                       bounds_of_(request), request.placement, output);
            compacted.failed())
            return compacted;

        level_t merged = level_t::of(request.number, lsm_level_order_t::sorted_k);
        if (status_t const room = merged.reserve(output.sorted_runs.size()); room.failed()) return room;

        std::uint64_t first_cell = 0;
        std::uint64_t first_key_slot = 0;
        for (std::size_t index = 0; index != output.sorted_runs.size(); ++index) {
            planned_run_t planned;
            planned.key_slots_count = output.sorted_runs[index].key_slots_count;
            planned.stored_values_bytes = output.sorted_runs[index].stored_values_bytes;
            planned.first_cell = first_cell;
            planned.first_key_slot = first_key_slot;

            expected<sorted_run_t> written = write_run_(volume, reading_channel, writing_channel,
                                                        std::span<sorted_run_t const* const> {sources}, output, planned,
                                                        request.value_max_size, request.cancellation);
            if (!written) {
                [[maybe_unused]] status_t const abandoned = merged.drop(volume, writing_channel);
                return written.status();
            }
            if (status_t const adopted = merged.bind_queue_pair(std::move(*written)); adopted.failed()) {
                [[maybe_unused]] status_t const abandoned = merged.drop(volume, writing_channel);
                return adopted;
            }
            first_cell += round_up(planned.key_slots_count, sorted_run_layout_t::branching_k);
            first_key_slot += planned.key_slots_count;
        }
        return merged;
    }

#pragma endregion Merging

  private:

#pragma region Private Types

    /** One run of the compaction's plan, with where its cells and its migrations start in the output. */
    struct planned_run_t {

        /** Key slots the run holds, padding cells excluded. */
        std::uint64_t key_slots_count = 0;

        /** Bytes of the values its key slots locate, before the section's alignment padding. */
        std::uint64_t stored_values_bytes = 0;

        /** Where this run's cells start in the compaction's cells. */
        std::uint64_t first_cell = 0;

        /** Where this run's survivors start in the compaction's migrations. */
        std::uint64_t first_key_slot = 0;
    };

    /** One block of one source value's read, kept until its completion carries the block back. */
    struct pending_piece_t {

        /** Where in the gather's scratch the bytes of this block land. */
        std::uint64_t destination = 0;

        /** Bytes of the block before the value's own bytes. */
        std::uint32_t skipped = 0;

        /** Bytes of the value this block carries. */
        std::uint32_t carried = 0;
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** Which compaction the order of the upper level and the depth of the new level together name. */
    [[nodiscard]] static constexpr compaction_kind_t kind_of_(lsm_level_order_t order,
                                                              lsm_level_depth_t depth) noexcept {
        if (order == lsm_level_order_t::overlapping_k)
            return depth == lsm_level_depth_t::bottom_k ? compaction_kind_t::overlapping_into_bottom_k
                                                        : compaction_kind_t::overlapping_into_intermediate_k;
        return depth == lsm_level_depth_t::bottom_k ? compaction_kind_t::sorted_into_bottom_k
                                                    : compaction_kind_t::sorted_into_intermediate_k;
    }

    /** The bounds every run the merge writes must fit, which the run layout and @p request together fix. */
    [[nodiscard]] static sorted_run_bounds_t bounds_of_(merge_request_t const& request) noexcept {
        sorted_run_bounds_t bounds;
        bounds.key_slots_capacity = request.run_key_slots_capacity;
        bounds.file_bytes_limit = request.run_bytes_limit;
        bounds.header_bytes = sorted_run_layout_t::header_bytes();
        bounds.alignment_bytes = portable_alignment_k;
        bounds.branching = sorted_run_layout_t::branching_k;
        return bounds;
    }

    /** Points a view of @p views at every source run's own cells, which the compactor reads in place. */
    static status_t stage_indexes_(level_t const& upper, level_t const* lower,
                                   st::basic_vector<sorted_run_t const*>& sources,
                                   st::basic_vector<sorted_run_index_view<key_slot_t>>& views) noexcept {
        std::size_t const lower_runs = lower != nullptr ? lower->size() : 0;
        std::size_t const runs = upper.size() + lower_runs;
        if (sources.resize(runs) != st::status_t::success_k || views.resize(runs) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A merge could not track its source runs"};
        for (std::size_t index = 0; index != upper.size(); ++index) sources[index] = &upper.run(index);
        for (std::size_t index = 0; index != lower_runs; ++index) sources[upper.size() + index] = &lower->run(index);

        for (std::size_t index = 0; index != runs; ++index) {
            sorted_run_t const& run = *sources[index];
            std::span<key_slot_t const> const cells = run.cells();
            if (run.layout().branching != sorted_run_layout_t::branching_k ||
                cells.size() != run.layout().cells_count())
                return {status_code_t::malformed_k, "A run's index is not laid out as a merge reads it"};
            views[index] = sorted_run_index_view<key_slot_t> {cells.data(), run.population_count()};
        }
        return {};
    }

#if USTORE_USE_CUDA

    /** Compacts on the first device when @p placement asks for one, and on the calling thread otherwise. */
    static status_t run_compaction_(compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_t> const> upper,
                                    std::span<sorted_run_index_view<key_slot_t> const> lower,
                                    sorted_run_bounds_t const& bounds, compaction_placement_t placement,
                                    compaction_output_t& output) noexcept {
        std::uint64_t merged = 0;
        for (std::size_t index = 0; index != upper.size(); ++index) merged += upper[index].key_slots_count;
        for (std::size_t index = 0; index != lower.size(); ++index) merged += lower[index].key_slots_count;

        std::size_t devices = 0;
        if (placement == compaction_placement_t::automatic_k)
            if (st::expected<std::size_t> const counted = cuda_count_devices(); counted.has_value()) devices = *counted;
        if (choose_compactor(placement, devices, merged) == compactor_choice_t::host_k)
            return compaction_status(cpu_compactor_t {}.compact(kind, upper, lower, bounds, output));

        st::expected<gpu_compactor_t> compactor = gpu_compactor_t::make(0);
        if (!compactor) return compaction_status(compactor.status());
        return compaction_status(compactor->compact(kind, upper, lower, bounds, output));
    }

#else

    /** Compacts on the calling thread, which is the only compactor a build without CUDA carries. */
    static status_t run_compaction_(compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_t> const> upper,
                                    std::span<sorted_run_index_view<key_slot_t> const> lower,
                                    sorted_run_bounds_t const& bounds, compaction_placement_t placement,
                                    compaction_output_t& output) noexcept {
        if (choose_compactor(placement, 0, 0) == compactor_choice_t::device_k)
            return {status_code_t::unsupported_k, "This build carries no device compactor"};
        return compaction_status(cpu_compactor_t {}.compact(kind, upper, lower, bounds, output));
    }

#endif

    /** Takes a streaming block from @p writer, polling its completions when every block is out. */
    static expected<pooled_buffer_t> acquire_buffer_or_poll_(write_pipeline_t& writer) noexcept {
        expected<pooled_buffer_t> block = writer.acquire_stream_buffer();
        while (!block && block.status() == status_code_t::capacity_exhausted_k && writer.outstanding() != 0) {
            if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;
            if (status_t const polled = writer.poll_one_completion(); polled.failed()) return polled;
            block = writer.acquire_stream_buffer();
        }
        return block;
    }

    /** Whether @p cancellation has been raised, which every block boundary of a merge asks. */
    [[nodiscard]] static bool stopped_(compaction_cancellation_t const* cancellation) noexcept {
        return cancellation != nullptr && cancellation->cancelled();
    }

    /** The status a merge answers once its caller has cancelled it. */
    static status_t cancelled_status_() noexcept {
        return {status_code_t::uninitialized_k, "The caller cancelled this compaction"};
    }

    /** Reserves a blob for the run @p planned describes, writes its three sections, and opens it. */
    static expected<sorted_run_t> write_run_(volume_t& volume, io_channel_t& reading_channel,
                                             io_channel_t& writing_channel,
                                             std::span<sorted_run_t const* const> sources,
                                             compaction_output_t const& output, planned_run_t const& planned,
                                             stored_value_size_t value_max_size,
                                             compaction_cancellation_t const* cancellation) noexcept {
        uuid_t const content_id = uuid_generator();
        if (!content_id.is_valid())
            return status_t {status_code_t::internal_k, "A merge could not name the run it writes"};

        sorted_run_header_t header {};
        header.magic = sorted_run_magic_k;
        header.format_version = sorted_run_format_version_k;
        header.key_slot_bytes = static_cast<std::uint32_t>(sizeof(key_slot_t));
        header.key_slots_count = planned.key_slots_count;
        header.stored_values_bytes = holds_values_inline_k ? 0 : planned.stored_values_bytes;
        header.value_max_size = value_max_size;
        header.branching = static_cast<std::uint16_t>(sorted_run_layout_t::branching_k);
        header.value_layout = value_layout_k;
        header.content_id = content_id;

        std::uint64_t const reserved_bytes = sorted_run_layout_t::blob_bytes(header.key_slots_count,
                                                                             header.stored_values_bytes);
        expected<blob_t> reserved = volume.create(sorted_run_t::name_of(content_id), blob_kind_t::sorted_run_k,
                                                  reserved_bytes);
        if (!reserved) return reserved.status();
        blob_t const blob = *reserved;

        if (status_t const written = write_sections_(volume, reading_channel, writing_channel, sources, output, planned,
                                                     header, blob, cancellation);
            written.failed()) {
            [[maybe_unused]] status_t const abandoned = volume.drop(writing_channel, sorted_run_t::name_of(content_id));
            return written;
        }
        if (status_t const sealed = volume.seal(writing_channel, blob); sealed.failed()) {
            [[maybe_unused]] status_t const abandoned = volume.drop(writing_channel, sorted_run_t::name_of(content_id));
            return sealed;
        }
        return sorted_run_t::attach(volume, writing_channel, blob);
    }

    /** Writes the header, the cells and the gathered values of one new run, and makes all three durable. */
    static status_t write_sections_(volume_t& volume, io_channel_t& reading_channel, io_channel_t& writing_channel,
                                    std::span<sorted_run_t const* const> sources, compaction_output_t const& output,
                                    planned_run_t const& planned, sorted_run_header_t const& header, blob_t const& blob,
                                    compaction_cancellation_t const* cancellation) noexcept {
        std::uint64_t const block_bytes = rounded_down_to(writing_channel.streams().buffer_bytes(),
                                                          portable_alignment_k);
        if (block_bytes < sorted_run_layout_t::header_bytes())
            return {status_code_t::capacity_exhausted_k, "A channel's streaming block is shorter than a run's header"};

        write_pipeline_t writer {volume.disk(), writing_channel, blob.medium};
        channel_drain_t<disk_t> const parked {volume.disk(), writing_channel};
        if (stopped_(cancellation)) return cancelled_status_();

        std::uint64_t const head_bytes = sorted_run_layout_t::header_bytes();
        expected<pooled_buffer_t> head = acquire_buffer_or_poll_(writer);
        if (!head) return head.status();
        std::memset(head->bytes().data(), 0, static_cast<std::size_t>(head_bytes));
        std::memcpy(head->bytes().data(), &header, sizeof(header));
        if (status_t const queued = writer.enqueue_write(std::move(*head), blob.slice(0, head_bytes)); queued.failed())
            return queued;
        if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;

        if (status_t const cells = write_cells_(writer, output, planned, header, blob, block_bytes); cells.failed())
            return cells;
        if (status_t const values = write_values_(volume, reading_channel, writer, sources, output, planned, header,
                                                  blob, block_bytes, cancellation);
            values.failed())
            return values;
        return writer.make_durable();
    }

    /** Writes the cells the compaction laid out, which are already in cell order with their padding. */
    static status_t write_cells_(write_pipeline_t& writer, compaction_output_t const& output,
                                 planned_run_t const& planned, sorted_run_header_t const& header, blob_t const& blob,
                                 std::uint64_t block_bytes) noexcept {
        std::uint64_t const section = sorted_run_layout_t::index_section_bytes(header.key_slots_count);
        std::uint64_t const filled = sorted_run_layout_t::index_bytes(header.key_slots_count);
        byte_t const* const cells = reinterpret_cast<byte_t const*>(output.cells.data() + planned.first_cell);
        for (std::uint64_t written = 0; written != section;) {
            std::uint64_t const room = section - written;
            std::uint64_t const chunk = block_bytes < room ? block_bytes : room;
            expected<pooled_buffer_t> block = acquire_buffer_or_poll_(writer);
            if (!block) return block.status();
            std::span<byte_t> const bytes = block->bytes().first(static_cast<std::size_t>(chunk));
            std::uint64_t const left = filled > written ? filled - written : 0;
            std::uint64_t const carried = left < chunk ? left : chunk;
            if (carried != 0) std::memcpy(bytes.data(), cells + written, static_cast<std::size_t>(carried));
            std::memset(bytes.data() + carried, 0, static_cast<std::size_t>(chunk - carried));
            if (status_t const queued = writer.enqueue_write(
                    std::move(*block), blob.slice(sorted_run_layout_t::index_offset() + written, chunk));
                queued.failed())
                return queued;
            if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;
            written += chunk;
        }
        return {};
    }

    /**
     *  @brief Gathers every survivor's value out of its old run and writes them back to back.
     *
     *  @note Values are read into a scratch buffer a block deep plus one read block, so a value that
     *      straddles the boundary between two written blocks is still read whole exactly once.
     */
    static status_t write_values_(volume_t& volume, io_channel_t& reading_channel, write_pipeline_t& writer,
                                  std::span<sorted_run_t const* const> sources, compaction_output_t const& output,
                                  planned_run_t const& planned, sorted_run_header_t const& header, blob_t const& blob,
                                  std::uint64_t block_bytes, compaction_cancellation_t const* cancellation) noexcept {
        if constexpr (holds_values_inline_k) return {};
        else {
            std::uint64_t const section = sorted_run_layout_t::values_section_bytes(header.stored_values_bytes);
            if (section == 0) return {};
            std::uint64_t const values_offset = sorted_run_layout_t::values_offset(header.key_slots_count);
            std::uint64_t const read_block = rounded_down_to(reading_channel.requests().buffer_bytes(),
                                                             portable_alignment_k);
            if (read_block == 0)
                return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than one page"};

            std::size_t const depth = reading_channel.queue_pair().depth();
            st::basic_vector<pending_piece_t> pending;
            if (pending.resize(depth) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A merge could not track its reads in flight"};
            st::basic_vector<byte_t> scratch;
            if (scratch.resize(static_cast<std::size_t>(block_bytes + read_block)) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A merge could not stage the values it gathers"};

            read_pipeline_t reader {volume.disk(), reading_channel, volume.medium()};
            channel_drain_t<disk_t> const parked {volume.disk(), reading_channel};

            status_t first;
            auto sink = [&](completed_buffer_t const& completed) noexcept {
                pending_piece_t const& piece = pending[completed.tag % depth];
                if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                    if (first.succeeded()) first = whole;
                    return;
                }
                if (piece.carried == 0) return;
                std::memcpy(scratch.data() + piece.destination, completed.bytes.data() + piece.skipped, piece.carried);
            };

            std::uint64_t survivor = planned.first_key_slot;
            std::uint64_t const survivors_end = planned.first_key_slot + planned.key_slots_count;
            std::uint64_t chunk_offset = 0;
            std::uint64_t staged = 0;
            std::uint64_t written = 0;
            std::uint32_t tag = 0;
            while (written != section) {
                if (stopped_(cancellation)) {
                    volume.disk().cancel(reading_channel.queue_pair());
                    return cancelled_status_();
                }

                while (staged < block_bytes && survivor != survivors_end) {
                    while (staged < block_bytes && survivor != survivors_end && reader.can_enqueue()) {
                        stored_value_migration_t const& migration = output.migrations[survivor];
                        if (migration.size == 0) {
                            ++survivor;
                            chunk_offset = 0;
                            continue;
                        }
                        std::uint64_t const aligned = rounded_down_to(migration.old_location, portable_alignment_k);
                        std::uint64_t const skipped = migration.old_location - aligned;
                        std::uint64_t const covered = rounded_up_to(skipped + migration.size, portable_alignment_k);
                        extent_t const window = sources[migration.old_sorted_run]->blob().slice(aligned, covered);
                        if (window.length != covered)
                            return {status_code_t::malformed_k, "A source run is shorter than the value it locates"};

                        std::uint64_t const length = read_block < covered - chunk_offset ? read_block
                                                                                         : covered - chunk_offset;
                        std::uint64_t const from = chunk_offset > skipped ? chunk_offset : skipped;
                        std::uint64_t const wanted_last = skipped + migration.size;
                        std::uint64_t const to = chunk_offset + length < wanted_last ? chunk_offset + length
                                                                                     : wanted_last;
                        pending_piece_t piece;
                        piece.destination = staged;
                        piece.skipped = static_cast<std::uint32_t>(from - chunk_offset);
                        piece.carried = static_cast<std::uint32_t>(to > from ? to - from : 0);
                        pending[tag % depth] = piece;
                        if (reader.enqueue_read(extent_t {window.offset + chunk_offset, length}, tag).failed()) break;
                        ++tag;
                        staged += piece.carried;
                        chunk_offset += length;
                        if (chunk_offset == covered) {
                            ++survivor;
                            chunk_offset = 0;
                        }
                    }
                    if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
                    expected<std::size_t> const polled = reader.poll_completions(sink);
                    if (!polled) return polled.status();
                    if (first.failed()) return first;
                }
                if (status_t const drained = reader.poll_all_completions(sink); drained.failed()) return drained;
                if (first.failed()) return first;

                std::uint64_t const room = section - written;
                std::uint64_t const chunk = block_bytes < room ? block_bytes : room;
                std::uint64_t const carried = staged < chunk ? staged : chunk;
                expected<pooled_buffer_t> block = acquire_buffer_or_poll_(writer);
                if (!block) return block.status();
                std::span<byte_t> const bytes = block->bytes().first(static_cast<std::size_t>(chunk));
                if (carried != 0) std::memcpy(bytes.data(), scratch.data(), static_cast<std::size_t>(carried));
                std::memset(bytes.data() + carried, 0, static_cast<std::size_t>(chunk - carried));
                if (status_t const queued = writer.enqueue_write(std::move(*block),
                                                                 blob.slice(values_offset + written, chunk));
                    queued.failed())
                    return queued;
                if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;
                written += chunk;
                if (staged > carried)
                    std::memmove(scratch.data(), scratch.data() + carried, static_cast<std::size_t>(staged - carried));
                staged -= carried;
            }
            return first;
        }
    }

#pragma endregion Private Methods
};

#pragma endregion Merger

} // namespace unum::ustore::nvme
