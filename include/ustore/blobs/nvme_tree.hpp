/**
 *  @file include/ustore/blobs/nvme_tree.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The configurations and schemas of UStoreNVMe, the levels of one LSM tree, the merge of one level into
 *      the next, and the level set readers see.
 *
 *  Configurations and on-disk schemas are kept as JSON through yyjson and validated against @c storage_limits_t.
 *
 *  Level zero takes one run per flush of the memory runs, so its runs overlap and only their age
 *  tells which version of a key is current: a lookup walks them newest first and a scan merges them
 *  by smallest key. Every deeper level is written by a merge, so its runs are disjoint and
 *  ascending and a lookup is a binary search over their bounds. Both shapes carry remove deltas;
 *  only the bottom level may forget one, which is a property of the merge that writes it rather
 *  than of the level that holds it.
 *
 *  A level names its runs by handles into the database's run pool, whose records never move, so the
 *  level set a compaction publishes can name the same runs the set it replaces named. Ownership
 *  follows one rule and no other:
 *  a run counts the level sets naming it, at most the current one and the next, and is given back
 *  to its volume when the last of them lets it go.
 *
 *  The merge itself is arithmetic over key slots and belongs to @c nvme_compaction.hpp, which
 *  decides who survives, cuts the survivors into runs that fit their bounds and plans where every
 *  value moves. What is left to the merger is the I/O: pointing the compactor at each source run's
 *  index, then writing the planned runs out of the plan it answered. Reads and writes ride separate
 *  channels because a channel is one queue pair with one completion queue, so a read pipeline
 *  polling on the channel a write pipeline submits on would swallow the writes' completions.
 *
 *  A flush writes one run, and a compaction merges a whole level zero or one deeper run with the
 *  runs below that it overlaps, or moves a run that overlaps nothing down untouched. Both publish
 *  by building the next level set in the spare beside the current one, which no one changes once a
 *  reader may hold it. The tree hands the new set to the publication hook its caller names, which
 *  publishes it to readers and returns only once no reader is left in the set it replaced; that set
 *  is emptied then, which is the whole of the reclamation, and readers never wait on a writer. A
 *  batched read refills the ring as completions retire rather than submitting a batch and waiting
 *  on it, so the depth a read reaches is the queue pair's and not the batch's.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::int32_t`, `std::int64_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcmp`, `std::memcpy`, `std::memmove`, `std::memset`

#include <algorithm>   // `std::max`, `std::min`
#include <chrono>      // `std::chrono::system_clock`
#include <concepts>    // `std::invocable`, `std::same_as`, `std::unsigned_integral`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::forward`, `std::move`

#include <fmt/format.h>
#include <yyjson.h>

#include <forkunion/atomics.hpp>             // `std::memory_order_acquire`, `std::memory_order_release`
#include <smashtable/basic_growing_pool.hpp> // `st::basic_growing_pool`
#include <smashtable/basic_vector.hpp>       // `st::vector`

#include "ustore/blobs/nvme_compaction.hpp"        // `host_compactor_t`, `compaction_output`, `compaction_kind_t`
#include "ustore/blobs/nvme_key_slot.hpp"          // `storage_limits_t`, `stored_value_size_t`, `value_layout_t`
#include "ustore/blobs/nvme_level_merger.hpp"      // `lsm_level_merger`, `compaction_cancellation`
#include "ustore/blobs/nvme_lsm_level.hpp"         // `lsm_level`, `lsm_level_position_t`
#include "ustore/blobs/nvme_schemas.hpp"           // `lsm_tree_config_t`
#include "ustore/blobs/nvme_sorted_run.hpp"        // `lsm_sorted_run`, `sorted_run_header_t`, `sorted_run_layout`
#include "ustore/blobs/vram_nvidia_compaction.hpp" // `cuda_compactor_t`
#include "ustore/utilities/nvme_channels.hpp"      // `io_channel_t`, `basic_read_pipeline`
#include "ustore/utilities/nvme_mounts.hpp"        // `resources_config_t`
#include "ustore/utilities/nvme_ring.hpp"          // `extent_t`, `ring_concept`, `portable_alignment_k`
#include "ustore/utilities/nvme_slots.hpp"         // `slot_bytes_k`, `slot_payload_offset_k`
#include "ustore/utilities/nvme_volume.hpp"        // `medium_extent_t`
#include "ustore/utilities/ram_buffers.hpp"        // `constrained_allocator`, `resource_constraint`
#include "ustore/utilities/types.hpp" // `status_t`, `expected`, `byte_t`, `uuid_t`, `uuid_generator`, `save_fields`, `load_fields`

namespace unum::ustore::blobs {

using utilities::constrained_allocator;
using utilities::fields_storage_requirements;
using utilities::load;
using utilities::load_fields;
using utilities::medium_reference_t;
using utilities::pooled_buffer_t;
using utilities::resource_constraint;
using utilities::resources_config_t;
using utilities::ring_concept;
using utilities::save;
using utilities::save_fields;
using utilities::serializable_concept;
using utilities::storage_requirements;
using utilities::string_t;
using utilities::string_view_t;
using utilities::uuid_from_text;
using utilities::uuid_generator;
using utilities::uuid_text_length_k;
using utilities::uuid_to_text;

#pragma region Names

/** The name of one collection. */
using collection_alphanumeric_id_t = string_t;

/** The name and the ID of one collection. */
struct collection_ids_t {

    /** The name users address the collection by. */
    collection_alphanumeric_id_t name;

    /** The UUID its schema, every commit and the C ABI carry. */
    collection_id_t id;
};

/** The canonical text of @p identifier, as a schema document carries it. */
inline expected<string_t> uuid_to_string(uuid_t const& identifier) noexcept {
    char text[uuid_t::printed_length_k] = {};
    identifier.print(text);
    string_t printed;
    if (sz::failed(printed.assign(string_view_t {text, uuid_t::printed_length_k})))
        return status_t {status_code_t::out_of_memory_k, "No room for the text of an identifier"};
    return printed;
}

#pragma endregion Names

#pragma region LSM Tree

#pragma region Vocabulary

/** Where one key sits in a tree, which is the level that answered and where inside that level. */
struct lsm_tree_position_t {

    /** The level that answered, which is also its index in the snapshot the lookup ran against. */
    std::uint32_t level_number = 0;

    /** Where inside that level the key sits. */
    lsm_level_position_t level_position;
};

/** What one pass of compaction did. */
enum class compaction_outcome_t : std::uint8_t {

    /** Every level was inside its bounds, or another thread was already compacting. */
    nothing_to_compact_k,

    /** A merge ran and the level set it produced is live. */
    published_k,
};

/** One run of a tree as a manifest names it, which is everything a restore needs to
 *  open it again. */
struct lsm_tree_run_record_t {

    /** The level the run belongs to, zero being the level flushes land on. */
    std::uint32_t level_number = 0;

    /** The run's own identifier, which its header also carries and which a manifest
     *  may leave zero. */
    uuid_t content_id {};

    /** Where the run sits on its volume. */
    medium_extent_t extent {};

    /** The stamp an ingest assigned a run written at stamp zero, since the run keeps its
     *  zeros, or zero. */
    std::uint64_t stamp = 0;
};

/**
 *  @brief What a compaction calls once its new level set is live and before it reclaims anything.
 *
 *  A durable record of the set a tree now holds must precede the drop of a run that set no longer
 *  names, or a crash between the two leaves a record naming bytes the volume has already handed out.
 */
struct lsm_tree_publication_hook_t {

    /** Called with @c context once the new set is built and before it is made current, or null; a failure abandons
     *  the set and every run only it names, leaving the current set as it was. */
    status_t (*prepare)(void* context) noexcept = nullptr;

    /** Called with @c context after the publication and before the first consumed run is given
     *  back; a failure leaves the consumed runs' bytes to the next mount. */
    status_t (*notify)(void* context) noexcept = nullptr;

    /** What @c notify is handed, which is the owner that records the run set the tree now holds. */
    void* context = nullptr;
};

#pragma endregion Vocabulary

#pragma region Tree

/**
 *  @brief Every level of one collection's LSM tree, as the level set its writer last published.
 *
 *  The tree holds two level sets, the current one and a spare that a publication builds the next
 *  one in; both live in one allocation made at mount, so moving the tree moves no set a reader
 *  walks. A run lives in the database's run pool and counts the sets naming it, so it is given back
 *  once the last of the two lets it go.
 *
 *  @tparam atomic_reference_ The reference the compaction claim and a caller's cancellation word are owned through.
 *  @tparam waiter_type_ What the core does while the tree waits for its reads and barriers.
 *  @tparam row_kit_type_ The SmashTable kit its runs' trees search with.
 */
template <typename key_slot_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_ = st::native_row_kit_t>
class lsm_tree {
  public:

#pragma region Types

    /** The key slot every run of this tree holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of this tree lives on. */
    using volume_t = volume_type_;

    /** One level of the tree. */
    using level_t = lsm_level<key_slot_t, volume_t, atomic_reference_, waiter_type_, row_kit_type_>;

    /** One immutable run of a level. */
    using sorted_run_t = typename level_t::sorted_run_t;

    /** The range tombstones one run carries. */
    using range_tombstones_t = typename sorted_run_t::range_tombstones_t;

    /** The database's pool every run of this tree lives in. */
    using run_pool_t = typename level_t::run_pool_t;

    /** How a level names one run. */
    using run_handle_t = typename level_t::run_handle_t;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The transport the volume moves bytes through. */
    using disk_t = typename volume_type_::disk_t;

    /** The compaction that replaces picked runs of two levels with new ones. */
    using merger_t = lsm_level_merger<key_slot_t, volume_t, atomic_reference_, waiter_type_, row_kit_type_>;

    /** The word a caller raises to stop a merge. */
    using cancellation_t = compaction_cancellation<atomic_reference_>;

    /** The pipeline a batched read refills. */
    using read_pipeline_t = basic_read_pipeline<disk_t>;

    /** How the values of this tree are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so reading one needs no I/O. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

    /** Every level in order of its number, which nothing changes once a reader may hold it. */
    class level_set_t {
      public:

        level_set_t() noexcept = default;
        level_set_t(level_set_t const&) = delete;
        level_set_t& operator=(level_set_t const&) = delete;
        level_set_t(level_set_t&&) noexcept = default;
        level_set_t& operator=(level_set_t&&) noexcept = default;

        /** Levels the set holds, which is at least one once published. */
        [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }

        /** The level numbered @p number, which is also the level at that index. */
        [[nodiscard]] level_t const& level(std::size_t number) const noexcept { return levels_[number]; }

        /** Key slots every level holds together, one key counted once per run that holds it. */
        [[nodiscard]] std::uint64_t population_count() const noexcept {
            std::uint64_t counted = 0;
            for (std::size_t number = 0; number != size(); ++number) counted += level(number).population_count();
            return counted;
        }

        /** Bytes every run of every level takes on its volume. */
        [[nodiscard]] std::uint64_t size_bytes() const noexcept {
            std::uint64_t counted = 0;
            for (std::size_t number = 0; number != size(); ++number) counted += level(number).size_bytes();
            return counted;
        }

        /** The newest stamp any run carries, which a mount resumes its stamps above. */
        [[nodiscard]] std::uint64_t newest_stamp() const noexcept {
            std::uint64_t newest = 0;
            for (std::size_t number = 0; number != size(); ++number)
                for (std::size_t index = 0; index != level(number).size(); ++index)
                    if (level(number).run(index).newest_stamp() > newest)
                        newest = level(number).run(index).newest_stamp();
            return newest;
        }

      private:

        friend class lsm_tree;

        /** The levels, which name runs the tree counts this set among the namers of. */
        st::vector<level_t> levels_;
    };

#pragma endregion Types

#pragma region Construction

    lsm_tree() noexcept = default;
    lsm_tree(lsm_tree const&) = delete;
    lsm_tree& operator=(lsm_tree const&) = delete;

    lsm_tree(lsm_tree&& other) noexcept
        : volume_(std::exchange(other.volume_, nullptr)), runs_(std::exchange(other.runs_, nullptr)),
          config_(other.config_), slot_bytes_(other.slot_bytes_), level_sets_(std::move(other.level_sets_)),
          current_level_set_(other.current_level_set_), compacting_(other.compacting_),
          merge_buffers_(std::move(other.merge_buffers_)) {}

    lsm_tree& operator=(lsm_tree&& other) noexcept {
        if (this == &other) return *this;
        forget_level_sets_();
        volume_ = std::exchange(other.volume_, nullptr);
        runs_ = std::exchange(other.runs_, nullptr);
        config_ = other.config_;
        slot_bytes_ = other.slot_bytes_;
        level_sets_ = std::move(other.level_sets_);
        current_level_set_ = other.current_level_set_;
        compacting_ = other.compacting_;
        merge_buffers_ = std::move(other.merge_buffers_);
        return *this;
    }

    /** Lets go of every run without giving its bytes back, since the manifest still names them. */
    ~lsm_tree() noexcept { forget_level_sets_(); }

    /**
     *  @brief An empty tree of one level zero over @p volume, whose runs live in @p runs.
     *  @param[in,out] volume The volume the runs are allocated on, outliving the tree.
     *  @param[in] config The enlarge factor and value sizes of the tree, and the collection
     *      its runs name.
     *  @param[in,out] runs The database's pool the runs live in, outliving the tree.
     *  @param[in] slot_bytes The unit levels and runs are sized by, which only tests shrink.
     */
    static expected<lsm_tree> mount(volume_t& volume, lsm_tree_config_t const& config, run_pool_t& runs,
                                    std::uint64_t slot_bytes = utilities::slot_bytes_k) noexcept {
        // The enlarge factor sizes every level and level zero's run count alike, so a tree without one has no shape
        if (config.level_enlarge_factor < storage_limits_t::level_min_enlarge_factor_k ||
            config.level_enlarge_factor > storage_limits_t::level_max_enlarge_factor_k)
            return status_t {status_code_t::invalid_argument_k, "A tree grows its levels by a factor in its bounds"};
        if (slot_bytes <= utilities::slot_payload_offset_k)
            return status_t {status_code_t::invalid_argument_k, "A slot holds more than its owner block"};
        lsm_tree tree;
        tree.volume_ = &volume;
        tree.runs_ = &runs;
        tree.config_ = config;
        tree.slot_bytes_ = slot_bytes;
        if (tree.level_sets_.resize(2) != st::status_t::success_k ||
            tree.level_sets_[0].levels_.push_back(level_t::of(runs, 0, lsm_level_order_t::overlapping_k)) !=
                st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A tree could not hold its first level"};
        return tree;
    }

    /** Gives every run of every level back to the volume, which destroys the tree's data and which
     *  only a tree no reader reaches and no manifest names may. */
    void destroy() noexcept {
        abandon_(current_());
        abandon_(spare_());
    }

#pragma endregion Construction

#pragma region Lookups

    /** The level set the last publication made current, which its writer builds the next one from. */
    [[nodiscard]] level_set_t const& levels() const noexcept { return level_sets_[current_level_set_]; }

    /** The volume every run of this tree lives on. */
    [[nodiscard]] volume_t& volume() const noexcept { return *volume_; }

    /** The pool every run of this tree lives in. */
    [[nodiscard]] run_pool_t& runs() const noexcept { return *runs_; }

    /**
     *  @brief Whether @p taken carries @p stored_key, and whether the key slot holding it locates a value.
     *  @param[in] taken The level set the lookup runs against.
     *  @param[in] stored_key The key to look for.
     *  @param[out] position Where the key sits, which is meaningless when the answer is @c missing_k.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     */
    [[nodiscard]] lookup_result_t find(level_set_t const& taken, stored_key_t stored_key, lsm_tree_position_t& position,
                                       std::uint64_t stamp = limit<std::uint64_t>::max()) const noexcept {
        position = lsm_tree_position_t {};
        for (std::size_t number = 0; number != taken.size(); ++number) {
            lsm_level_position_t inside;
            lookup_result_t const found = taken.level(number).find(stored_key, stamp, inside);
            if (found == lookup_result_t::missing_k) continue;
            position = lsm_tree_position_t {static_cast<std::uint32_t>(number), inside};
            return found;
        }
        return lookup_result_t::missing_k;
    }

#pragma endregion Lookups

#pragma region Batched Reads

    /**
     *  @brief Looks @p stored_keys up in @p taken and reads their values, refilling the channel as reads retire.
     *  @param[in,out] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] taken The level set every lookup runs against.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key was, which separates a miss from a removed value.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     *  @return Success, or the first failure any of the blocks reported.
     *
     *  @note A value larger than one block arrives as several calls, each carrying the bytes of the value
     *      before it, and a key whose value is present and empty is reported once with no bytes.
     */
    template <typename sink_type_>
    status_t read_values(io_channel_t& channel, level_set_t const& taken, std::span<stored_key_t const> stored_keys,
                         std::span<lookup_result_t> results, sink_type_&& sink,
                         std::uint64_t stamp = limit<std::uint64_t>::max()) const noexcept {
        if (results.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched read answers one result per key"};

        if constexpr (holds_values_inline_k) {
            for (std::size_t request = 0; request != stored_keys.size(); ++request) {
                lsm_tree_position_t position;
                results[request] = find(taken, stored_keys[request], position, stamp);
                if (results[request] != lookup_result_t::written_k) continue;
                key_slot_t const& slot = taken.level(position.level_number).key_slot(position.level_position);
                sink(request, std::uint64_t {0}, std::span<byte_t const> {slot.data, slot.size});
            }
            return {};
        }
        else {
            std::uint64_t const block_bytes = round_down_to_multiple(channel.requests().buffer_bytes(),
                                                                     portable_alignment_k);
            if (block_bytes == 0)
                return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than one page"};

            std::size_t const depth = channel.queue_pair().depth();
            st::vector<pending_read_t> pending;
            if (pending.resize(depth) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A tree could not track its reads in flight"};

            auto [reader_channel, reader_channel_status] = channel.acquire();
            if (reader_channel_status.failed()) return reader_channel_status;
            read_pipeline_t reader {volume_->disk(), std::move(reader_channel), medium_reference_t {}};
            status_t first;
            auto adapter = [&](completed_buffer_t const& completed) noexcept {
                pending_read_t const& piece = pending[completed.tag % depth];
                if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                    if (first.succeeded()) first = whole;
                    return;
                }
                sink(piece.request, piece.offset, completed.bytes.subspan(piece.skipped, piece.carried));
            };

            std::size_t request = 0;
            std::uint64_t chunk = 0;
            request_tag_t tag = 0;
            value_window_t window;
            medium_reference_t medium;
            while (request != stored_keys.size() || reader.outstanding() != 0) {
                while (request != stored_keys.size() && reader.can_enqueue()) {
                    if (chunk == 0) {
                        lsm_tree_position_t position;
                        lookup_result_t const found = find(taken, stored_keys[request], position, stamp);
                        results[request] = found;
                        level_t const& level = taken.level(position.level_number);
                        window = found == lookup_result_t::written_k ? level.value_window(position.level_position)
                                                                     : value_window_t {};
                        if (window.extent.length == 0) {
                            if (found == lookup_result_t::written_k)
                                sink(request, std::uint64_t {0}, std::span<byte_t const> {});
                            ++request;
                            continue;
                        }
                        medium = level.run(position.level_position.run_index).extent().medium;
                    }
                    std::uint64_t const at = chunk * block_bytes;
                    std::uint64_t const room = window.extent.length - at;
                    std::uint64_t const length = block_bytes < room ? block_bytes : room;
                    pending[tag % depth] = piece_of_(window, request, at, length);
                    if (reader.enqueue_read(medium, extent_t {window.extent.offset + at, length}, tag).failed()) break;
                    ++tag;
                    if (at + length == window.extent.length) {
                        ++request;
                        chunk = 0;
                    }
                    else ++chunk;
                }
                if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
                auto [polled, poll_completions_status] = reader.poll_completions(adapter);
                if (poll_completions_status.failed()) return poll_completions_status;
            }
            return first;
        }
    }

#pragma endregion Batched Reads

#pragma region Publication

    /**
     *  @brief Writes @p key_slots and @p stored_values as one run and publishes it onto level zero.
     *  @param[in,out] channel The channel the run is written on, held by the calling thread.
     *  @param[in] key_slots The key slots in ascending order of their keys, as a flush drained them from memory runs.
     *  @param[in] stamps The stamp of every key slot, in the same order.
     *  @param[in] kinds The @c delta_kind_t of every key slot, in the same order.
     *  @param[in] floor The stamp every stamp is raised to at least, since no reader can tell those below it apart.
     *  @param[in] stored_values The values back to back in sorted order, one of @p value_max_size bytes per key slot
     *      for a fixed width.
     *  @param[in] value_offsets Where each value starts inside @p stored_values and one past the last, for a variable
     *      width alone.
     *  @param[in] value_max_size The largest value the collection admits, kept in the run's header.
     *  @param[in] published What to call once the new set is current, which returns once no reader holds the old one.
     *  @param[in] tails The tails of the keys the key slots' heads cut short, in the same order, none for whole keys.
     *  @param[in] tombstones The flushed commits' range tombstones, which the run keeps whole.
     */
    status_t flush(io_channel_t& channel, std::span<key_slot_t const> key_slots, std::span<std::uint64_t const> stamps,
                   std::span<std::uint8_t const> kinds, std::uint64_t floor, std::span<byte_t const> stored_values,
                   std::span<stored_value_offset_t const> value_offsets, stored_value_size_t value_max_size,
                   lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {},
                   key_tails_column_t const& tails = {},
                   range_tombstones_t const& tombstones = range_tombstones_t {}) noexcept {
        auto [written, write_sorted_run_status] = sorted_run_t::write(
            *volume_, channel, config_.collection, uuid_generator(), key_slots, stamps, kinds, floor, stored_values,
            value_offsets, value_max_size, tails, tombstones);
        if (write_sorted_run_status.failed()) return write_sorted_run_status;
        auto [flushed, take_status] = runs_->take(std::move(written));
        if (take_status.failed()) {
            volume_->deallocate(written.extent());
            return take_status;
        }
        std::span<run_handle_t const> const placed {&flushed, 1};
        if (status_t const prepared = prepare_next_({}, 0, placed); prepared.failed()) {
            runs_->discard(*volume_, placed);
            return prepared;
        }
        return publish_(published);
    }

    /**
     *  @brief Opens the runs @p runs names and publishes them as the level set of this tree.
     *  @param[in,out] channel The channel every run's header and index is read on, held by the calling thread.
     *  @param[in] runs The runs in the order their levels hold them, which is oldest first on level
     *      zero, each carrying the stamp an ingest assigned it or zero.
     *  @param[in] published What to call once the restored set is current, as a flush does.
     *  @return Success, or the status that stopped it with the empty level set still current and
     *      every run left on the volume.
     *
     *  @note Only a tree still holding the one empty level @c mount built takes these, so a reopen
     *      adopts what the medium carries and no call ever merges two level sets into one.
     */
    status_t restore(io_channel_t& channel, std::span<lsm_tree_run_record_t const> runs,
                     lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {}) noexcept {
        if (levels().size() != 1 || !levels().level(0).empty())
            return {status_code_t::invalid_argument_k, "A tree takes its stored runs only while it holds none"};
        if (runs.empty()) return {};

        std::uint32_t deepest = 0;
        for (lsm_tree_run_record_t const& record : runs)
            if (record.level_number > deepest) deepest = record.level_number;

        level_set_t& next = spare_();
        if (next.levels_.reserve(std::size_t {deepest} + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A tree could not hold the levels it restores"};
        for (std::uint32_t number = 0; number <= deepest; ++number) [[maybe_unused]]
            st::status_t const placed = next.levels_.push_back(level_t::of(
                *runs_, number, number == 0 ? lsm_level_order_t::overlapping_k : lsm_level_order_t::sorted_k));

        for (lsm_tree_run_record_t const& record : runs) {
            if (status_t const restored = restore_run_(channel, record, next); restored.failed()) {
                forget_(next);
                return restored;
            }
        }
        if (published.prepare != nullptr)
            if (status_t const prepared = published.prepare(published.context); prepared.failed()) {
                forget_(next);
                return prepared;
            }
        return replace_(published);
    }

    /**
     *  @brief Prepares the next level set naming every run of the current one, and @p runs, sorted
     *      and disjoint, on the level @c ingest_level_ picks for them, which grows empty sorted
     *      levels until it exists.
     *  @param[in,out] runs The runs an ingest wrote, in ascending order of their keys, each taken
     *      into the run pool.
     *  @return Success, or the status that stopped it, with every run it took given back and the
     *      rest left in @p runs.
     *  @note The caller holds what serializes the tree's publications, so the current set holds
     *      still, and calls @c adopt and then @c retire before it prepares again.
     */
    status_t prepare_ingest(std::span<sorted_run_t> runs) noexcept {
        if (runs.empty()) return {status_code_t::invalid_argument_k, "An ingest publishes at least one run"};
        if (!spare_().levels_.empty())
            return {status_code_t::invalid_argument_k, "A tree retires one set before it prepares another"};
        std::uint64_t bytes = 0;
        for (sorted_run_t const& run : runs) bytes += run.size_bytes();
        std::size_t const target = ingest_level_(levels(), runs.front().lower_stored_key(),
                                                 runs.back().upper_stored_key(), bytes);

        st::vector<run_handle_t> taken;
        if (taken.reserve(runs.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A tree could not track the runs an ingest wrote"};
        for (sorted_run_t& run : runs) {
            auto [handle, take_status] = runs_->take(std::move(run));
            if (take_status.failed()) {
                runs_->discard(*volume_, std::span<run_handle_t const> {taken});
                return take_status;
            }
            [[maybe_unused]] st::status_t const placed = taken.push_back(std::move(handle));
        }
        if (status_t const prepared = prepare_next_({}, target, taken); prepared.failed()) {
            runs_->discard(*volume_, std::span<run_handle_t const> {taken});
            return prepared;
        }
        return {};
    }

    /** Makes the set @c prepare_ingest prepared current; the set it replaced still names every run,
     *  so a reader holding it reads on until the caller calls @c retire. */
    void adopt() noexcept { current_level_set_ ^= 1; }

    /** Empties the set @c adopt replaced, once no reader can hold it. */
    void retire() noexcept { abandon_(spare_()); }

    /** Whether any level of @p taken is over the bytes its number admits. */
    [[nodiscard]] bool needs_compaction(level_set_t const& taken) const noexcept {
        return overfull_(taken) != taken.size();
    }

    /**
     *  @brief Merges the runs of the shallowest level over its bounds that cost least with the
     *      runs below they overlap, or moves one down untouched when it overlaps none, and
     *      publishes the result.
     *  @param[in,out] reading_channel The channel the merge gathers source values on, held by the calling thread.
     *  @param[in,out] writing_channel The channel the merge writes its new runs on, which is not @p reading_channel.
     *  @param[in] placement Where the compaction runs, which a collection leaves automatic.
     *  @param[in] compactor The compactor the database picked at open: the calling thread, or its warm device
     *      compactor.
     *  @param[in] cancellation The word the merge reads between blocks, or null for a merge nobody stops.
     *  @param[in] fold How the collection's changes compose and apply.
     *  @param[in] device_fold How a device composes the collection's changes, or @c no_device_fold_t where none does,
     *      which keeps every merge holding a change on the host.
     *  @param[in] published What to call once the merged set is current, which returns once no reader holds the set
     *      it replaced and before a consumed run is given back.
     *  @param[in] stamps Which older versions survive for readers and for a mount, and the floor the new runs count
     *      from, where the default keeps the newest version of every key alone.
     *  @param[in] fan_out The threads a merge on the host fans its slices out over, each on
     *      channels of its own, where the default merges on the calling thread over
     *      @p reading_channel and @p writing_channel.
     *  @return What the pass did, or the status that stopped it with the current level set untouched.
     */
    template <utilities::fold_concept fold_type_, typename device_fold_type_>
    expected<compaction_outcome_t> compact(
        io_channel_t& reading_channel, io_channel_t& writing_channel, compaction_placement_t placement,
        compactor_ref_t const& compactor, cancellation_t const* cancellation, fold_type_ const& fold,
        device_fold_type_ const& device_fold,
        lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {},
        compaction_stamps_t const& stamps = compaction_stamps_t {},
        compaction_fan_out_t const& fan_out = compaction_fan_out_t {}) noexcept {
        if (!claim_compaction_()) return compaction_outcome_t::nothing_to_compact_k;

        expected<compaction_outcome_t> const outcome = compact_once_(
            reading_channel, writing_channel, placement, compactor, cancellation, fold, device_fold, published, stamps,
            fan_out, merge_choice_t::overfull_k);
        atomic_reference_<std::int32_t> {compacting_}.store(static_cast<std::int32_t>(compaction_state_t::idle_k),
                                                            std::memory_order_release);
        return outcome;
    }

    /**
     *  @brief Drops every version stamped above @p ceiling, merging down each run that holds one
     *      until none does.
     *
     *  A mount runs this before anything draws a stamp: a crash may leave runs holding commits the
     *  manifest it found never promised, and those land nowhere else, so dropping them all leaves
     *  only commits that landed whole.
     */
    template <utilities::fold_concept fold_type_>
    status_t truncate_above(io_channel_t& reading_channel, io_channel_t& writing_channel, std::uint64_t ceiling,
                            fold_type_ const& fold,
                            lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {}) noexcept {
        if (!claim_compaction_()) return {status_code_t::invalid_argument_k, "A mount truncates a tree nobody merges"};
        compaction_stamps_t stamps;
        stamps.horizon = ceiling;
        stamps.ceiling = ceiling;
        stamps.floor = ceiling;
        status_t result;
        while (result.succeeded() && above_(levels(), ceiling) != levels().size()) {
            // A mount truncates on the host, so no device composes a change
            expected<compaction_outcome_t> const outcome = compact_once_(
                reading_channel, writing_channel, compaction_placement_t::host_k, {}, nullptr, fold,
                no_device_fold_t {}, published, stamps, compaction_fan_out_t {}, merge_choice_t::above_ceiling_k);
            if (!outcome) result = outcome.status();
        }
        atomic_reference_<std::int32_t> {compacting_}.store(static_cast<std::int32_t>(compaction_state_t::idle_k),
                                                            std::memory_order_release);
        return result;
    }

#pragma endregion Publication

  private:

#pragma region Private Types

    /** Which runs one merge pass takes down into the level below. */
    enum class merge_choice_t : std::uint8_t {

        /** Runs of the shallowest level over the bytes its number admits. */
        overfull_k,

        /** Runs of the shallowest level holding a version above the ceiling of
         *  the pass's stamps. */
        above_ceiling_k,
    };

    /** One block of one value's read, kept until its completion carries the block back. */
    struct pending_read_t {

        /** Which of the caller's requests this block belongs to. */
        std::size_t request = 0;

        /** Bytes of the block before the value's own bytes. */
        std::uint32_t skipped = 0;

        /** Bytes of the value this block carries. */
        std::uint32_t carried = 0;

        /** Bytes of the value that the blocks before this one carried. */
        std::uint64_t offset = 0;
    };

    /** The runs one pass consumes: those of the upper level first, then those
     *  below they overlap. */
    struct picked_runs_t {

        /** The picked runs, the upper level's oldest first or ascending, then the
         *  lower level's ascending. */
        st::vector<sorted_run_t const*> runs;

        /** How many of @c runs belong to the upper level. */
        std::size_t upper_count = 0;

        /** The least key any picked run holds. */
        stored_key_t lower_stored_key = limit<stored_key_t>::max();

        /** The greatest key any picked run holds. */
        stored_key_t upper_stored_key = limit<stored_key_t>::min();

        /** The upper level's picked runs. */
        [[nodiscard]] std::span<sorted_run_t const* const> upper() const noexcept {
            return std::span<sorted_run_t const* const> {runs}.first(upper_count);
        }

        /** The lower level's picked runs. */
        [[nodiscard]] std::span<sorted_run_t const* const> lower() const noexcept {
            return std::span<sorted_run_t const* const> {runs}.subspan(upper_count);
        }
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** The piece of @p window that the block at @p at of @p length carries, for @p request. */
    [[nodiscard]] static pending_read_t piece_of_(value_window_t const& window, std::size_t request, std::uint64_t at,
                                                  std::uint64_t length) noexcept {
        std::uint64_t const wanted_first = window.skipped;
        std::uint64_t const wanted_last = window.skipped + window.bytes;
        std::uint64_t const first = at > wanted_first ? at : wanted_first;
        std::uint64_t const last = at + length < wanted_last ? at + length : wanted_last;
        pending_read_t piece;
        piece.request = request;
        piece.skipped = static_cast<std::uint32_t>(first - at);
        piece.carried = static_cast<std::uint32_t>(last > first ? last - first : 0);
        piece.offset = first - wanted_first;
        return piece;
    }

    /** Takes the right to publish, which only one thread holds at a time. */
    [[nodiscard]] bool claim_compaction_() noexcept {
        std::int32_t idle = static_cast<std::int32_t>(compaction_state_t::idle_k);
        return atomic_reference_<std::int32_t> {compacting_}.compare_exchange_strong(
            idle, static_cast<std::int32_t>(compaction_state_t::compacting_k), std::memory_order_acquire,
            std::memory_order_relaxed);
    }

    /** The set readers are handed. */
    [[nodiscard]] level_set_t& current_() noexcept { return level_sets_[current_level_set_]; }

    /** The set the next publication is built in, which is empty between publications. */
    [[nodiscard]] level_set_t& spare_() noexcept { return level_sets_[current_level_set_ ^ 1]; }

    /** Counts @p levels among the sets naming each of its runs. */
    void name_(level_set_t& levels) noexcept {
        for (level_t const& level : levels.levels_)
            for (run_handle_t const handle : level.runs()) ++runs_->at(handle)->level_sets_naming;
    }

    /** Empties @p levels, giving back the bytes of every run no other set names and vacating it. */
    void abandon_(level_set_t& levels) noexcept {
        for (level_t const& level : levels.levels_)
            for (run_handle_t const handle : level.runs())
                if (sorted_run_t* const run = runs_->at(handle); run && --run->level_sets_naming == 0) {
                    volume_->deallocate(run->extent());
                    // Readers of its last naming set left before the retire, so this generation
                    // step races none.
                    runs_->vacate(handle);
                }
        levels.levels_.clear();
    }

    /** Empties @p levels, vacating every run no other set names and leaving its bytes to the
     *  manifest that names it. */
    void forget_(level_set_t& levels) noexcept {
        for (level_t const& level : levels.levels_)
            for (run_handle_t const handle : level.runs())
                if (sorted_run_t* const run = runs_->at(handle); run && --run->level_sets_naming == 0)
                    runs_->vacate(handle); // Its last set's readers already left, as in abandon_.
        levels.levels_.clear();
    }

    /** Forgets both sets, which a tree going away does while a manifest still names its runs. */
    void forget_level_sets_() noexcept {
        for (level_set_t& levels : level_sets_) forget_(levels);
    }

    /** Opens the run @p record names into its level of @p next, taking the stamp an ingest assigned
     *  it again, and counts @p next among its namers. */
    status_t restore_run_(io_channel_t& channel, lsm_tree_run_record_t const& record, level_set_t& next) noexcept {
        auto [opened, open_sorted_run_status] = sorted_run_t::open(*volume_, channel, record.extent);
        if (open_sorted_run_status.failed()) return open_sorted_run_status;
        if (record.stamp != 0)
            if (status_t const stamped = opened.assign_stamp(record.stamp); stamped.failed()) return stamped;
        auto [handle, take_status] = runs_->take(std::move(opened));
        if (take_status.failed()) return take_status;
        if (status_t const inserted = next.levels_[record.level_number].insert_run(handle); inserted.failed()) {
            runs_->vacate(handle);
            return inserted;
        }
        ++runs_->at(handle)->level_sets_naming;
        return {};
    }

    /**
     *  @brief Builds the spare set from the current one, leaving out @p consumed and naming
     *      @p placed on level @p number, which grows empty sorted levels until it exists, then
     *      counts the spare among their namers.
     *  @return Success, or the status that stopped it with the spare empty and @p placed for the
     *      caller to give back.
     */
    status_t prepare_next_(std::span<sorted_run_t const* const> consumed, std::size_t number,
                           std::span<run_handle_t const> placed) noexcept {
        level_set_t const& source = levels();
        level_set_t& next = spare_();
        std::size_t const deepest = !placed.empty() && number >= source.size() ? number + 1 : source.size();
        if (next.levels_.reserve(deepest) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A tree could not prepare its next level set"};
        for (std::size_t index = 0; index != deepest; ++index) {
            lsm_level_order_t const order = index == 0 ? lsm_level_order_t::overlapping_k : lsm_level_order_t::sorted_k;
            [[maybe_unused]] st::status_t const pushed = next.levels_.push_back(
                level_t::of(*runs_, static_cast<std::uint32_t>(index), order));
            if (index >= source.size()) continue;
            if (status_t const carried = next.levels_[index].carry(source.level(index), consumed); carried.failed()) {
                next.levels_.clear();
                return carried;
            }
        }
        if (!placed.empty()) {
            level_t& target = next.levels_[number];
            if (status_t const reserved = target.reserve(placed.size()); reserved.failed()) {
                next.levels_.clear();
                return reserved;
            }
            for (run_handle_t const handle : placed)
                if (status_t const inserted = target.insert_run(handle); inserted.failed()) {
                    next.levels_.clear();
                    return inserted;
                }
        }
        name_(next);
        return {};
    }

    /** Asks @p published to prepare for the spare set and publishes it, or gives back what only
     *  the spare names. */
    status_t publish_(lsm_tree_publication_hook_t const& published) noexcept {
        if (published.prepare != nullptr)
            if (status_t const prepared = published.prepare(published.context); prepared.failed()) {
                abandon_(spare_());
                return prepared;
            }
        return replace_(published);
    }

    /**
     *  @brief Makes the spare set current, calls @p published, then empties the set it replaced.
     *
     *  The hook publishes the new set to readers and returns only once none is left in the old
     *  one, so the runs only the old set named are unreachable by then; their bytes go back to
     *  the volume only once the hook recorded the new set, and a failed record leaves them to
     *  the next mount.
     */
    status_t replace_(lsm_tree_publication_hook_t const& published) noexcept {
        current_level_set_ ^= 1;
        status_t const recorded = published.notify != nullptr ? published.notify(published.context) : status_t {};
        if (recorded.failed()) forget_(spare_());
        else abandon_(spare_());
        return recorded;
    }

    /** Bytes level @p number of @p levels admits: one slot times F to the level's number at most,
     *  and above the deepest level that level's bytes divided by F per level between them, but
     *  never under one slot. */
    [[nodiscard]] std::uint64_t level_bytes_(level_set_t const& levels, std::size_t number) const noexcept {
        std::uint64_t const factor = config_.level_enlarge_factor;
        std::uint64_t ceiling = slot_bytes_;
        for (std::size_t step = 0; step != number; ++step) {
            if (ceiling > limit<std::uint64_t>::max() / factor) return limit<std::uint64_t>::max();
            ceiling *= factor;
        }
        std::size_t const deepest = levels.size() - 1;
        if (number >= deepest) return ceiling;
        std::uint64_t tracked = levels.level(deepest).size_bytes();
        for (std::size_t step = number; step != deepest; ++step) tracked /= factor;
        return tracked < slot_bytes_ ? slot_bytes_ : tracked > ceiling ? ceiling : tracked;
    }

    /** The level an ingest of @p bytes of keys from @p lower to @p upper lands on, so no shallower level holds a key
     *  of that range: level zero when one of the first two levels overlaps it, and otherwise the shallowest sorted
     *  level above the first overlapping one whose bytes fit, that one's parent when none does, or a level past the
     *  deepest when nothing overlaps. */
    [[nodiscard]] std::size_t ingest_level_(level_set_t const& levels, stored_key_t lower, stored_key_t upper,
                                            std::uint64_t bytes) const noexcept {
        std::size_t overlapping = 0;
        for (; overlapping != levels.size(); ++overlapping) {
            level_t const& level = levels.level(overlapping);
            bool overlaps = false;
            for (std::size_t index = 0; !overlaps && index != level.size(); ++index)
                overlaps = !(level.run(index).upper_stored_key() < lower ||
                             upper < level.run(index).lower_stored_key());
            if (overlaps) break;
        }
        if (overlapping != levels.size() && overlapping <= 1) return 0;
        for (std::size_t number = 1;; ++number) {
            if (overlapping != levels.size() && number + 1 == overlapping) return number;
            std::uint64_t const held = number < levels.size() ? levels.level(number).size_bytes() : 0;
            std::uint64_t const admitted = level_bytes_(levels, number);
            if (held + bytes <= admitted || admitted == limit<std::uint64_t>::max()) return number;
        }
    }

    /** The shallowest level of @p levels that is over its bounds, or the level count when none is:
     *  level zero at F runs or one slot of bytes, and every deeper one over @c level_bytes_. */
    [[nodiscard]] std::size_t overfull_(level_set_t const& levels) const noexcept {
        if (levels.size() != 0 &&
            (levels.level(0).size() >= config_.level_enlarge_factor || levels.level(0).size_bytes() >= slot_bytes_))
            return 0;
        for (std::size_t number = 1; number != levels.size(); ++number)
            if (levels.level(number).size_bytes() > level_bytes_(levels, number)) return number;
        return levels.size();
    }

    /** The shallowest level of @p levels holding a run with a version above @p ceiling, or the level count. */
    [[nodiscard]] static std::size_t above_(level_set_t const& levels, std::uint64_t ceiling) noexcept {
        for (std::size_t number = 0; number != levels.size(); ++number)
            for (std::size_t index = 0; index != levels.level(number).size(); ++index)
                if (levels.level(number).run(index).newest_stamp() > ceiling) return number;
        return levels.size();
    }

    /** Whether @p run holds or hides a key between @p lower and @p upper. */
    [[nodiscard]] static bool overlaps_(sorted_run_t const& run, stored_key_t lower, stored_key_t upper) noexcept {
        return run.holds_keys() && !(run.upper_stored_key() < lower || upper < run.lower_stored_key());
    }

    /**
     *  @brief Picks the runs one pass consumes from level @p upper of @p source and the
     *      level below it.
     *
     *  Level zero's runs overlap, so it goes whole. A deeper level gives the run holding a version
     *  above @p ceiling when @p choice asks for one, and otherwise the run whose overlap below
     *  holds the fewest bytes. The lower level then gives every run that overlaps the picked range.
     */
    status_t pick_runs_(level_set_t const& source, std::size_t upper, merge_choice_t choice, std::uint64_t ceiling,
                        picked_runs_t& picked) const noexcept {
        level_t const& from = source.level(upper);
        level_t const* const below = upper + 1 != source.size() ? &source.level(upper + 1) : nullptr;
        std::size_t const lower_runs = below != nullptr ? below->size() : 0;
        if (picked.runs.reserve(from.size() + lower_runs) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A tree could not list the runs a merge consumes"};

        if (upper == 0)
            for (std::size_t index = 0; index != from.size(); ++index) [[maybe_unused]]
                st::status_t const placed = picked.runs.push_back(&from.run(index));
        else {
            std::size_t chosen = 0;
            std::uint64_t cheapest = limit<std::uint64_t>::max();
            std::size_t first_overlapping = 0;
            for (std::size_t index = 0; index != from.size(); ++index) {
                sorted_run_t const& run = from.run(index);
                if (choice == merge_choice_t::above_ceiling_k) {
                    if (run.newest_stamp() <= ceiling) continue;
                    chosen = index;
                    break;
                }
                while (first_overlapping != lower_runs &&
                       below->run(first_overlapping).upper_stored_key() < run.lower_stored_key())
                    ++first_overlapping;
                std::uint64_t overlapped = 0;
                for (std::size_t lower = first_overlapping;
                     lower != lower_runs && !(run.upper_stored_key() < below->run(lower).lower_stored_key()); ++lower)
                    overlapped += below->run(lower).size_bytes();
                if (overlapped >= cheapest) continue;
                cheapest = overlapped;
                chosen = index;
            }
            [[maybe_unused]] st::status_t const placed = picked.runs.push_back(&from.run(chosen));
        }
        picked.upper_count = picked.runs.size();

        stored_key_t range_lower = limit<stored_key_t>::max();
        stored_key_t range_upper = limit<stored_key_t>::min();
        for (sorted_run_t const* run : picked.upper()) {
            if (!run->holds_keys()) continue;
            if (run->lower_stored_key() < range_lower) range_lower = run->lower_stored_key();
            if (range_upper < run->upper_stored_key()) range_upper = run->upper_stored_key();
        }
        picked.lower_stored_key = range_lower;
        picked.upper_stored_key = range_upper;
        for (std::size_t index = 0; index != lower_runs; ++index) {
            sorted_run_t const& run = below->run(index);
            if (!overlaps_(run, range_lower, range_upper)) continue;
            [[maybe_unused]] st::status_t const placed = picked.runs.push_back(&run);
            if (run.lower_stored_key() < picked.lower_stored_key) picked.lower_stored_key = run.lower_stored_key();
            if (picked.upper_stored_key < run.upper_stored_key()) picked.upper_stored_key = run.upper_stored_key();
        }
        return {};
    }

    /** Whether a level of @p source deeper than @p lower holds a key between @p lower_key
     *  and @p upper_key. */
    [[nodiscard]] static bool overlaps_below_(level_set_t const& source, std::size_t lower, stored_key_t lower_key,
                                              stored_key_t upper_key) noexcept {
        for (std::size_t number = lower + 1; number < source.size(); ++number) {
            level_t const& level = source.level(number);
            if (level.empty() || level.upper_stored_key() < lower_key || upper_key < level.lower_stored_key()) continue;
            for (std::size_t index = 0; index != level.size(); ++index)
                if (overlaps_(level.run(index), lower_key, upper_key)) return true;
        }
        return false;
    }

    /** Runs one pass over the runs @p choice picks and publishes it, which the claim around it
     *  keeps to one thread. */
    template <typename fold_type_, typename device_fold_type_>
    expected<compaction_outcome_t> compact_once_(io_channel_t& reading_channel, io_channel_t& writing_channel,
                                                 compaction_placement_t placement, compactor_ref_t const& compactor,
                                                 cancellation_t const* cancellation, fold_type_ const& fold,
                                                 device_fold_type_ const& device_fold,
                                                 lsm_tree_publication_hook_t const& published,
                                                 compaction_stamps_t const& stamps, compaction_fan_out_t const& fan_out,
                                                 merge_choice_t choice) noexcept {
        level_set_t const& source = levels();
        std::size_t const upper = choice == merge_choice_t::overfull_k ? overfull_(source)
                                                                       : above_(source, stamps.ceiling);
        if (upper == source.size()) return compaction_outcome_t::nothing_to_compact_k;
        std::size_t const lower = upper + 1;

        picked_runs_t picked;
        if (status_t const pick = pick_runs_(source, upper, choice, stamps.ceiling, picked); pick.failed()) return pick;

        // A run that overlaps nothing below moves down by its handle, and no byte is
        // read or written
        if (picked.lower().empty() && picked.upper_count == 1 && choice == merge_choice_t::overfull_k) {
            run_handle_t const moved = runs_->handle_of(*picked.runs[0]);
            if (status_t const prepared = prepare_next_(picked.upper(), lower,
                                                        std::span<run_handle_t const> {&moved, 1});
                prepared.failed())
                return prepared;
            if (status_t const replaced = publish_(published); replaced.failed()) return replaced;
            return compaction_outcome_t::published_k;
        }

        typename merger_t::merge_request_t request;
        request.upper = picked.upper();
        request.upper_order = source.level(upper).order();
        request.lower = picked.lower();
        request.depth = overlaps_below_(source, lower, picked.lower_stored_key, picked.upper_stored_key)
                            ? lsm_level_depth_t::intermediate_k
                            : lsm_level_depth_t::bottom_k;
        request.collection = config_.collection;
        sorted_run_config_t const& values = config_.lsm_level_0.sorted_run;
        request.value_max_size = is_fixed(value_layout_k) ? values.value_size : values.value_max_size;
        request.run_bytes_limit = slot_bytes_ - utilities::slot_payload_offset_k;
        request.placement = placement;
        request.compactor = compactor;
        request.cancellation = cancellation;
        request.stamps = stamps;
        if (merge_buffers_.size() < fan_out.slices() &&
            merge_buffers_.resize(fan_out.slices()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A tree could not keep buffers for every slice"};
        request.buffers = std::span<typename merger_t::merge_buffers_t> {merge_buffers_};
        request.fan_out = fan_out;

        auto [merged, merge_status] = merger_t::merge(*volume_, *runs_, reading_channel, writing_channel, request, fold,
                                                      device_fold);
        if (merge_status.failed()) return merge_status;
        std::span<run_handle_t const> const placed {merged};
        if (status_t const prepared = prepare_next_(std::span<sorted_run_t const* const> {picked.runs}, lower, placed);
            prepared.failed()) {
            runs_->discard(*volume_, placed);
            return prepared;
        }
        if (status_t const replaced = publish_(published); replaced.failed()) return replaced;
        return compaction_outcome_t::published_k;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The volume every run of this tree lives on, null until the tree is mounted or once it is moved from. */
    volume_t* volume_ = nullptr;

    /** The database's pool every run of this tree lives in, null until the tree is mounted or once
     *  it is moved from. */
    run_pool_t* runs_ = nullptr;

    /** How fast the bytes a level admits grow with depth, the value sizes of its runs
     *  and their collection. */
    lsm_tree_config_t config_ {};

    /** The unit levels and runs are sized by. */
    std::uint64_t slot_bytes_ = 0;

    /** The current set and the spare, allocated once at mount so a reader's set never moves
     *  with the tree. */
    st::vector<level_set_t> level_sets_;

    /** Which of @c level_sets_ readers are handed. */
    std::size_t current_level_set_ = 0;

    /** A @c compaction_state_t, moved by @c claim_compaction_ alone. */
    std::int32_t compacting_ = 0;

    /** What one merge leaves for the next, one per slice, which the compaction claim keeps to one
     *  merge at a time. */
    st::vector<typename merger_t::merge_buffers_t> merge_buffers_;

#pragma endregion Private Members
};

#pragma endregion Tree

#pragma region Records

/** Appends a record for every run of @p tree to @p into, level by level and oldest first inside a level. */
template <typename tree_type_>
status_t record_lsm_tree_runs(tree_type_ const& tree, st::vector<lsm_tree_run_record_t>& into) noexcept {
    typename tree_type_::level_set_t const& taken = tree.levels();
    std::size_t counted = 0;
    for (std::size_t number = 0; number != taken.size(); ++number) counted += taken.level(number).size();
    if (into.reserve(into.size() + counted) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "No room to name the runs a tree holds"};

    for (std::size_t number = 0; number != taken.size(); ++number) {
        typename tree_type_::level_t const& level = taken.level(number);
        for (std::size_t index = 0; index != level.size(); ++index) {
            typename tree_type_::sorted_run_t const& run = level.run(index);
            lsm_tree_run_record_t record;
            record.level_number = static_cast<std::uint32_t>(number);
            record.content_id = run.content_id();
            record.extent = run.extent();
            record.stamp = run.assigned_stamp();
            [[maybe_unused]] st::status_t const placed = into.push_back(std::move(record));
        }
    }
    return {};
}

#pragma endregion Records

#pragma endregion LSM Tree

} // namespace unum::ustore::blobs
