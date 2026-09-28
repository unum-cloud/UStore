/**
 *  @file src/nvme/lsm_tree.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The levels of one LSM tree, the level set readers see, and the compaction that replaces it.
 *
 *  A flush writes one run and a compaction writes a whole level, and both publish by building the next
 *  level set beside the live one and storing which of the two is live. Readers take a snapshot by
 *  counting themselves into a set and re-reading that word, so no reader ever walks a set a publication
 *  is rewriting and no writer waits on a lock. The set a publication replaces is not freed until its
 *  last reader has left it, which is the whole of the reclamation.
 *
 *  A batched read refills the ring as completions retire rather than submitting a batch and waiting on
 *  it, so the depth a read reaches is the queue pair's and not the batch's.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`

#include <array>   // `std::array`
#include <atomic>  // `std::atomic_thread_fence`
#include <span>    // `std::span`
#include <utility> // `std::exchange`, `std::move`

#include <forkunion/atomics.hpp>       // `fu::preferred_atomic_ref`
#include <forkunion/capabilities.hpp>  // `fu::preferred_yield_t`
#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/config.hpp"           // `lsm_tree_config_t`
#include "nvme/extent.hpp"           // `extent_t`, `portable_alignment_k`
#include "nvme/io_channel.hpp"       // `io_channel_t`
#include "nvme/io_pipeline.hpp"      // `basic_read_pipeline`, `require_completed_in_full`
#include "nvme/lsm_level.hpp"        // `lsm_level`, `lsm_level_order_t`, `lsm_level_depth_t`
#include "nvme/lsm_level_merger.hpp" // `lsm_level_merger`, `compaction_cancellation_t`
#include "nvme/storage_limits.hpp"   // `storage_limits_t`
#include "nvme/uuid.hpp"             // `uuid_t`, `uuid_generator`

namespace unum::ustore::nvme {

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

/** One run of a tree as a schema names it, which is everything a restore needs to open it again. */
struct lsm_tree_run_record_t {

    /** The level the run belongs to, zero being the level flushes land on. */
    std::uint32_t level_number = 0;

    /** The run's own identifier, which is also the name its blob answers to on the volume. */
    uuid_t content_id {};
};

/**
 *  @brief What a compaction calls once its new level set is live and before it reclaims anything.
 *
 *  A durable record of the set a tree now holds must precede the drop of a run that set no longer
 *  names, or a crash between the two leaves a record naming bytes the volume has already handed out.
 */
struct lsm_tree_publication_hook_t {

    /** Called with @c context after the publication and before the first consumed run is dropped. */
    status_t (*notify)(void* context) noexcept = nullptr;

    /** What @c notify is handed, which is the owner that records the run set the tree now holds. */
    void* context = nullptr;
};

#pragma endregion Vocabulary

#pragma region Tree

/**
 *  @brief Every level of one collection's LSM tree, and the two level sets a publication alternates over.
 *
 *  A tree is moved only before anything has taken a snapshot of it, since a snapshot names the tree it
 *  counted itself into.
 */
template <typename key_slot_type_, typename volume_type_>
class lsm_tree {
  public:

#pragma region Types

    /** The key slot every run of this tree holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of this tree lives on. */
    using volume_t = volume_type_;

    /** One level of the tree. */
    using level_t = lsm_level<key_slot_t, volume_t>;

    /** One immutable run of a level. */
    using sorted_run_t = typename level_t::sorted_run_t;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The transport the volume moves bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_type_&>().disk())>;

    /** The compaction that replaces two levels with one. */
    using merger_t = lsm_level_merger<key_slot_t, volume_t>;

    /** The pipeline a batched read refills. */
    using read_pipeline_t = basic_read_pipeline<disk_t>;

    /** One level set, which is every level in order of its number. */
    using level_set_t = st::basic_vector<level_t>;

    /** How the values of this tree are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so reading one needs no I/O. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

    /**
     *  @brief One level set held against publication, which every lookup and every read runs against.
     *
     *  Holding one keeps the set alive and nothing else: a publication during a snapshot goes through,
     *  and the reader keeps reading the set it took until it lets go.
     */
    class snapshot_t {
      public:

        snapshot_t() noexcept = default;
        snapshot_t(snapshot_t const&) = delete;
        snapshot_t& operator=(snapshot_t const&) = delete;

        snapshot_t(snapshot_t&& other) noexcept : tree_(std::exchange(other.tree_, nullptr)), slot_(other.slot_) {}

        snapshot_t& operator=(snapshot_t&& other) noexcept {
            if (this == &other) return *this;
            release_();
            tree_ = std::exchange(other.tree_, nullptr);
            slot_ = other.slot_;
            return *this;
        }

        ~snapshot_t() noexcept { release_(); }

        /** Whether this snapshot holds a level set. */
        [[nodiscard]] bool holding() const noexcept { return tree_ != nullptr; }

        /** Levels the set holds, which is at least one. */
        [[nodiscard]] std::size_t size() const noexcept { return tree_->sets_[slot_].size(); }

        /** The level numbered @p number, which is also the level at that index. */
        [[nodiscard]] level_t const& level(std::size_t number) const noexcept { return tree_->sets_[slot_][number]; }

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

      private:

        friend class lsm_tree;

        /** Lets the set go, which both the destructor and a move assignment need. */
        void release_() noexcept {
            if (tree_ != nullptr) tree_->readers_reference_(slot_).fetch_sub(1, std::memory_order_release);
            tree_ = nullptr;
        }

        /** The tree the set belongs to, null once moved from. */
        lsm_tree const* tree_ = nullptr;

        /** Which of the tree's two sets this snapshot counted itself into. */
        std::size_t slot_ = 0;
    };

#pragma endregion Types

#pragma region Construction

    lsm_tree() noexcept = default;
    lsm_tree(lsm_tree const&) = delete;
    lsm_tree& operator=(lsm_tree const&) = delete;
    lsm_tree(lsm_tree&&) noexcept = default;
    lsm_tree& operator=(lsm_tree&&) noexcept = default;

    /** An empty tree of one level zero over @p volume, which must outlive it. */
    static expected<lsm_tree> mount(volume_t& volume, lsm_tree_config_t const& config) noexcept {
        lsm_tree tree;
        tree.volume_ = &volume;
        tree.config_ = config;
        level_t zero = level_t::of(0, lsm_level_order_t::overlapping_k);
        if (tree.sets_[0].push_back(std::move(zero)) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A tree could not hold its first level"};
        return tree;
    }

    /** Returns every run of every level to the volume, which destroys the tree's data, once no snapshot still holds
     *  either set. */
    status_t destroy(io_channel_t& channel) noexcept {
        for (std::size_t slot = 0; slot != readers_.size(); ++slot)
            for (readers_count_t readers = readers_reference_(slot).load(std::memory_order_acquire); readers != 0;
                 readers = readers_reference_(slot).load(std::memory_order_acquire))
                fu::preferred_yield_t {}(readers_[slot], readers, std::size_t {0});
        status_t first;
        level_set_t& live = sets_[live_slot_()];
        for (std::size_t number = 0; number != live.size(); ++number)
            if (status_t const dropped = live[number].drop(*volume_, channel); dropped.failed() && first.succeeded())
                first = dropped;
        return first;
    }

#pragma endregion Construction

#pragma region Snapshots

    /** Holds the live level set against publication until the snapshot is let go. */
    [[nodiscard]] snapshot_t snapshot() const noexcept {
        for (;;) {
            std::size_t const slot = live_slot_();
            readers_reference_(slot).fetch_add(1, std::memory_order_acquire);
            // Pairs with the publisher's fence, so the count and the live slot are never both read stale.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (live_slot_() == slot) {
                snapshot_t taken;
                taken.tree_ = this;
                taken.slot_ = slot;
                return taken;
            }
            readers_reference_(slot).fetch_sub(1, std::memory_order_release);
        }
    }

    /**
     *  @brief Whether @p taken carries @p stored_key, and whether the key slot holding it locates a value.
     *  @param[in] taken The level set the lookup runs against.
     *  @param[in] stored_key The key to look for.
     *  @param[out] position Where the key sits, which is meaningless when the answer is @c nothing_k.
     */
    [[nodiscard]] find_result_t find(snapshot_t const& taken, stored_key_t stored_key,
                                     lsm_tree_position_t& position) const noexcept {
        position = lsm_tree_position_t {};
        for (std::size_t number = 0; number != taken.size(); ++number) {
            lsm_level_position_t inside;
            find_result_t const found = taken.level(number).find(stored_key, inside);
            if (found == find_result_t::nothing_k) continue;
            position = lsm_tree_position_t {static_cast<std::uint32_t>(number), inside};
            return found;
        }
        return find_result_t::nothing_k;
    }

#pragma endregion Snapshots

#pragma region Batched Reads

    /**
     *  @brief Looks @p stored_keys up in @p taken and reads their values, refilling the channel as reads retire.
     *  @param[inout] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] taken The level set every lookup runs against.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key was, which separates a miss from an erased value.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *  @return Success, or the first failure any of the blocks reported.
     *
     *  @note A value larger than one block arrives as several calls, each carrying the bytes of the value
     *      before it, and a key whose value is present and empty is reported once with no bytes.
     */
    template <typename sink_type_>
    status_t read_values(io_channel_t& channel, snapshot_t const& taken, std::span<stored_key_t const> stored_keys,
                         std::span<find_result_t> results, sink_type_&& sink) const noexcept {
        if (results.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched read answers one result per key"};

        if constexpr (holds_values_inline_k) {
            for (std::size_t request = 0; request != stored_keys.size(); ++request) {
                lsm_tree_position_t position;
                results[request] = find(taken, stored_keys[request], position);
                if (results[request] != find_result_t::success_k) continue;
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
            st::basic_vector<pending_read_t> pending;
            if (pending.resize(depth) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A tree could not track its reads in flight"};

            read_pipeline_t reader {volume_->disk(), channel, volume_->medium()};
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
            while (request != stored_keys.size() || reader.outstanding() != 0) {
                while (request != stored_keys.size() && reader.can_enqueue()) {
                    if (chunk == 0) {
                        lsm_tree_position_t position;
                        find_result_t const found = find(taken, stored_keys[request], position);
                        results[request] = found;
                        window = found == find_result_t::success_k
                                     ? taken.level(position.level_number).value_window(position.level_position)
                                     : value_window_t {};
                        if (window.extent.length == 0) {
                            if (found == find_result_t::success_k)
                                sink(request, std::uint64_t {0}, std::span<byte_t const> {});
                            ++request;
                            continue;
                        }
                    }
                    std::uint64_t const at = chunk * block_bytes;
                    std::uint64_t const room = window.extent.length - at;
                    std::uint64_t const length = block_bytes < room ? block_bytes : room;
                    pending[tag % depth] = piece_of_(window, request, at, length);
                    if (reader.enqueue_read(extent_t {window.extent.offset + at, length}, tag).failed()) break;
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
     *  @param[inout] channel The channel the run is written on, held by the calling thread.
     *  @param[in] key_slots The key slots in ascending order of their keys, as a write buffer drained them.
     *  @param[in] stored_values The values back to back, located from the run's own values offset.
     *  @param[in] value_max_size The largest value the collection admits, kept in the run's header.
     */
    status_t flush(io_channel_t& channel, std::span<key_slot_t const> key_slots, std::span<byte_t const> stored_values,
                   stored_value_size_t value_max_size) noexcept {
        auto [written, write_sorted_run_status] = sorted_run_t::write(*volume_, channel, uuid_generator(), key_slots,
                                                                      stored_values, value_max_size);
        if (write_sorted_run_status.failed()) return write_sorted_run_status;
        uuid_t const content_id = written.content_id();

        std::size_t const live = live_slot_();
        std::size_t const staged = 1 - live;
        std::size_t const nothing_consumed = sets_[live].size();
        if (status_t const carried = carry_all_(live, staged, nothing_consumed, nothing_consumed); carried.failed()) {
            [[maybe_unused]] status_t const abandoned = sorted_run_t::drop(*volume_, channel, content_id);
            return carried;
        }
        if (status_t const adopted = sets_[staged][0].bind_queue_pair(std::move(written)); adopted.failed()) {
            retire_(staged);
            [[maybe_unused]] status_t const abandoned = sorted_run_t::drop(*volume_, channel, content_id);
            return adopted;
        }
        publish_(staged);
        retire_(live);
        return {};
    }

    /**
     *  @brief Opens the runs @p runs names and publishes them as the level set of this tree.
     *  @param[inout] channel The channel every run's header and index is read on, held by the calling thread.
     *  @param[in] runs The runs in the order their levels hold them, which is oldest first on level zero.
     *  @return Success, or the status that stopped it with the empty level set still live.
     *
     *  @note Only a tree still holding the one empty level @c mount built takes these, so a reopen
     *      adopts what the medium carries and no call ever merges two level sets into one.
     */
    status_t restore(io_channel_t& channel, std::span<lsm_tree_run_record_t const> runs) noexcept {
        std::size_t const live = live_slot_();
        if (sets_[live].size() != 1 || !sets_[live][0].empty())
            return {status_code_t::invalid_argument_k, "A tree takes its stored runs only while it holds none"};
        if (runs.empty()) return {};

        std::uint32_t deepest = 0;
        for (std::size_t index = 0; index != runs.size(); ++index)
            if (runs[index].level_number > deepest) deepest = runs[index].level_number;

        std::size_t const staged = 1 - live;
        discard_(staged);
        level_set_t& into = sets_[staged];
        if (into.reserve(std::size_t {deepest} + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A tree could not stage the levels it restores"};
        for (std::uint32_t number = 0; number <= deepest; ++number) {
            level_t level = level_t::of(number,
                                        number == 0 ? lsm_level_order_t::overlapping_k : lsm_level_order_t::sorted_k);
            [[maybe_unused]] st::status_t const placed = into.push_back(std::move(level));
        }

        for (std::size_t index = 0; index != runs.size(); ++index) {
            auto [opened, open_sorted_run_status] = sorted_run_t::open(*volume_, channel, runs[index].content_id);
            if (open_sorted_run_status.failed()) {
                discard_(staged);
                return open_sorted_run_status;
            }
            if (status_t const adopted = into[runs[index].level_number].bind_queue_pair(std::move(opened));
                adopted.failed()) {
                discard_(staged);
                return adopted;
            }
        }

        publish_(staged);
        retire_(live);
        return {};
    }

    /** Whether any level of the live set is over the bytes its number admits. */
    [[nodiscard]] bool needs_compaction() const noexcept {
        snapshot_t const taken = snapshot();
        return overfull_(sets_[taken.slot_]) != sets_[taken.slot_].size();
    }

    /**
     *  @brief Merges the shallowest level that is over its bounds into the level below it and publishes it.
     *  @param[inout] reading_channel The channel the merge gathers source values on, held by the calling thread.
     *  @param[inout] writing_channel The channel the merge writes its new runs on, which is not @p reading_channel.
     *  @param[in] placement Where the compaction runs, which a collection leaves automatic.
     *  @param[in] cancellation The word the merge reads between blocks, or null for a merge nobody stops.
     *  @param[in] published What to call once the merged set is live and before a consumed run is dropped.
     *  @return What the pass did, or the status that stopped it with the live level set untouched.
     */
    expected<compaction_outcome_t> compact(
        io_channel_t& reading_channel, io_channel_t& writing_channel, compaction_placement_t placement,
        compaction_cancellation_t const* cancellation,
        lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {}) noexcept {
        if (!claim_compaction_()) return compaction_outcome_t::nothing_to_compact_k;

        expected<compaction_outcome_t> const outcome = compact_once_(reading_channel, writing_channel, placement,
                                                                     cancellation, published);
        fu::preferred_atomic_ref<std::int32_t> {compacting_}.store(
            static_cast<std::int32_t>(compaction_state_t::idle_k), std::memory_order_release);
        return outcome;
    }

#pragma endregion Publication

  private:

#pragma region Private Types

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

    /** Which of the two sets is live right now. */
    [[nodiscard]] std::size_t live_slot_() const noexcept {
        return static_cast<std::size_t>(
            fu::preferred_atomic_ref<std::int32_t const> {live_}.load(std::memory_order_acquire));
    }

    /** The word counting the readers of set @p slot. */
    [[nodiscard]] fu::preferred_atomic_ref<readers_count_t> readers_reference_(std::size_t slot) const noexcept {
        return fu::preferred_atomic_ref<readers_count_t> {readers_[slot]};
    }

    /** Takes the right to publish, which only one thread holds at a time. */
    [[nodiscard]] bool claim_compaction_() noexcept {
        std::int32_t idle = static_cast<std::int32_t>(compaction_state_t::idle_k);
        return fu::preferred_atomic_ref<std::int32_t> {compacting_}.compare_exchange_strong(
            idle, static_cast<std::int32_t>(compaction_state_t::compacting_k), std::memory_order_acquire,
            std::memory_order_relaxed);
    }

    /** Makes set @p staged live and frees the one it replaced once its last reader has left. */
    void publish_(std::size_t staged) noexcept {
        std::size_t const retired = 1 - staged;
        fu::preferred_atomic_ref<std::int32_t> {live_}.store(static_cast<std::int32_t>(staged),
                                                             std::memory_order_release);
        // Pairs with the reader's fence, so the retired slot's count is read only after the new slot is visible.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        for (readers_count_t readers = readers_reference_(retired).load(std::memory_order_acquire); readers != 0;
             readers = readers_reference_(retired).load(std::memory_order_acquire))
            fu::preferred_yield_t {}(readers_[retired], readers, std::size_t {0});
    }

    /** Empties set @p slot without freeing a run, since every run it named is named by the live set too. */
    void retire_(std::size_t slot) noexcept {
        level_set_t& retired = sets_[slot];
        for (std::size_t number = 0; number != retired.size(); ++number) retired[number].relinquish();
        retired.clear();
    }

    /** Empties set @p slot and frees the runs it alone names, which only an abandoned restore holds. */
    void discard_(std::size_t slot) noexcept {
        level_set_t& abandoned = sets_[slot];
        for (std::size_t number = 0; number != abandoned.size(); ++number) abandoned[number].discard();
        abandoned.clear();
    }

    /**
     *  @brief Builds set @p staged out of set @p from, leaving the levels @p first through @p last empty.
     *  @param[in] from The set whose levels are named again, which is the live one.
     *  @param[in] staged The idle set, which this overwrites and leaves empty when it fails.
     *  @param[in] first The first level whose runs are left behind, which a merge consumes.
     *  @param[in] last The last such level, below @p first when the caller consumes none.
     */
    status_t carry_all_(std::size_t from, std::size_t staged, std::size_t first, std::size_t last) noexcept {
        level_set_t const& source = sets_[from];
        retire_(staged);
        level_set_t& into = sets_[staged];
        if (into.reserve(source.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A tree could not stage its next level set"};

        for (std::size_t number = 0; number != source.size(); ++number) {
            level_t level = level_t::of(source[number].number(), source[number].order());
            if (number < first || number > last)
                if (status_t const carried = level.carry(source[number]); carried.failed()) {
                    level.relinquish();
                    retire_(staged);
                    return carried;
                }
            [[maybe_unused]] st::status_t const placed = into.push_back(std::move(level));
        }
        return {};
    }

    /** The bytes level @p number admits, which grows by the tree's enlarge factor at every level. */
    [[nodiscard]] std::uint64_t level_bytes_(std::size_t number) const noexcept {
        std::uint64_t bytes = config_.lsm_level_0.max_bytes;
        std::uint64_t const factor = config_.level_enlarge_factor != 0 ? config_.level_enlarge_factor : 1;
        for (std::size_t step = 0; step != number; ++step) {
            if (bytes > limit<std::uint64_t>::max() / factor) return limit<std::uint64_t>::max();
            bytes *= factor;
        }
        return bytes;
    }

    /** The shallowest level of @p levels that is over its bounds, or the level count when none is. */
    [[nodiscard]] std::size_t overfull_(level_set_t const& levels) const noexcept {
        if (!levels.empty() && levels[0].size() >= storage_limits_t::first_level_compaction_threshold_k) return 0;
        for (std::size_t number = 0; number != levels.size(); ++number)
            if (!levels[number].empty() && levels[number].size_bytes() > level_bytes_(number)) return number;
        return levels.size();
    }

    /** Runs one merge and publishes it, which the claim around it keeps to one thread at a time. */
    expected<compaction_outcome_t> compact_once_(io_channel_t& reading_channel, io_channel_t& writing_channel,
                                                 compaction_placement_t placement,
                                                 compaction_cancellation_t const* cancellation,
                                                 lsm_tree_publication_hook_t const& published) noexcept {
        std::size_t const live = live_slot_();
        level_set_t const& source = sets_[live];
        std::size_t const upper = overfull_(source);
        if (upper == source.size()) return compaction_outcome_t::nothing_to_compact_k;
        std::size_t const lower = upper + 1;
        bool const grows = lower == source.size();
        std::size_t const last_consumed = grows ? upper : lower;

        st::basic_vector<sorted_run_t const*> consumed;
        if (status_t const recorded = record_consumed_(source, upper, last_consumed, consumed); recorded.failed())
            return recorded;

        typename merger_t::merge_request_t request;
        request.upper = &source[upper];
        request.lower = grows ? nullptr : &source[lower];
        request.number = static_cast<std::uint32_t>(lower);
        request.depth = lsm_level_depth_t::bottom_k;
        request.value_max_size = static_cast<stored_value_size_t>(config_.lsm_level_0.sorted_run.value_max_size);
        request.run_key_slots_capacity = storage_limits_t::write_buffer_elements_max_count_k;
        request.run_bytes_limit = storage_limits_t::sorted_run_file_max_size_k;
        request.placement = placement;
        request.cancellation = cancellation;
        if (!grows && lower + 1 != source.size()) request.depth = lsm_level_depth_t::intermediate_k;

        auto [merged, merge_status] = merger_t::merge(*volume_, reading_channel, writing_channel, request);
        if (merge_status.failed()) return merge_status;

        std::size_t const staged = 1 - live;
        if (status_t const carried = carry_all_(live, staged, upper, last_consumed); carried.failed()) {
            [[maybe_unused]] status_t const abandoned = merged.drop(*volume_, writing_channel);
            return carried;
        }
        if (grows) {
            if (sets_[staged].push_back(level_t::of(static_cast<std::uint32_t>(lower), lsm_level_order_t::sorted_k)) !=
                st::status_t::success_k) {
                [[maybe_unused]] status_t const abandoned = merged.drop(*volume_, writing_channel);
                retire_(staged);
                return status_t {status_code_t::out_of_memory_k, "A tree could not hold another level"};
            }
        }
        sets_[staged][lower] = std::move(merged);

        publish_(staged);
        retire_(live);
        if (published.notify != nullptr)
            if (status_t const recorded = published.notify(published.context); recorded.failed()) {
                for (std::size_t index = 0; index != consumed.size(); ++index) delete consumed[index];
                return recorded;
            }
        status_t first;
        for (std::size_t index = 0; index != consumed.size(); ++index) {
            if (status_t const dropped = sorted_run_t::drop(*volume_, writing_channel, consumed[index]->content_id());
                dropped.failed() && first.succeeded())
                first = dropped;
            delete consumed[index];
        }
        if (first.failed()) return first;
        return compaction_outcome_t::published_k;
    }

    /** Collects every run of levels @p first through @p last, which the merge has already rewritten. */
    static status_t record_consumed_(level_set_t const& source, std::size_t first, std::size_t last,
                                     st::basic_vector<sorted_run_t const*>& consumed) noexcept {
        std::size_t runs = 0;
        for (std::size_t number = first; number <= last && number != source.size(); ++number)
            runs += source[number].size();
        if (consumed.reserve(runs) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A tree could not list the runs it retires"};
        for (std::size_t number = first; number <= last && number != source.size(); ++number)
            for (std::size_t index = 0; index != source[number].size(); ++index) {
                sorted_run_t const* run = &source[number].run(index);
                [[maybe_unused]] st::status_t const placed = consumed.push_back(std::move(run));
            }
        return {};
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The volume every run of this tree lives on, null until the tree is mounted or once it is moved from. */
    volume_t* volume_ = nullptr;

    /** The bytes every level admits and how fast that grows with depth. */
    lsm_tree_config_t config_ {};

    /** The two level sets, of which one is live and the other is the next publication's room. */
    std::array<level_set_t, 2> sets_ {};

    /** Which of the two sets is live, stored with release and read with acquire. */
    std::int32_t mutable live_ = 0;

    /** Readers counted into each set, which a publication waits on before it frees the retired one; on a line of their
     *  own, so counting in never evicts the live word every reader checks. */
    alignas(fu::default_alignment_k) std::array<readers_count_t, 2> mutable readers_ {0, 0};

    /** A @c compaction_state_t, moved by @c claim_compaction_ alone, off the readers' line. */
    alignas(fu::default_alignment_k) std::int32_t compacting_ = 0;

#pragma endregion Private Members
};

#pragma endregion Tree

#pragma region Records

/** Appends a record for every run of @p tree to @p into, level by level and oldest first inside a level. */
template <typename tree_type_>
status_t record_lsm_tree_runs(tree_type_ const& tree, st::basic_vector<lsm_tree_run_record_t>& into) noexcept {
    typename tree_type_::snapshot_t const taken = tree.snapshot();
    std::size_t counted = 0;
    for (std::size_t number = 0; number != taken.size(); ++number) counted += taken.level(number).size();
    if (into.reserve(into.size() + counted) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "No room to name the runs a tree holds"};

    for (std::size_t number = 0; number != taken.size(); ++number) {
        auto const& level = taken.level(number);
        for (std::size_t index = 0; index != level.size(); ++index) {
            lsm_tree_run_record_t record;
            record.level_number = static_cast<std::uint32_t>(number);
            record.content_id = level.run(index).content_id();
            [[maybe_unused]] st::status_t const placed = into.push_back(std::move(record));
        }
    }
    return {};
}

#pragma endregion Records

} // namespace unum::ustore::nvme
