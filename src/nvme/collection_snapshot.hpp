/**
 *  @file src/nvme/collection_snapshot.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief One level set of one collection held against publication, and every read that runs against it.
 *
 *  Taking a snapshot copies no run and moves no byte: it counts a reader into the live level set, which
 *  a later publication replaces beside rather than in place, so the runs this snapshot names stay
 *  readable until it is let go. What a snapshot cannot see is the write buffer, which is why a caller
 *  taking one flushes first.
 *
 *  Every read of an LSM tree lives here and nowhere else. A collection is this plus a write buffer and
 *  a value cache in front of it, and takes a snapshot of its own tree for the keys neither of those
 *  answers.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::exchange`, `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/io_lane.hpp"      // `io_lane_t`
#include "nvme/lsm_tree.hpp"     // `lsm_tree`, `lsm_tree_position_t`
#include "nvme/stored_value.hpp" // `stored_value_size_t`
#include "nvme/types.hpp"        // `value_layout_t`, `is_inline`

namespace unum::ustore::nvme {

#pragma region Vocabulary

/** What one key of a select answered, which separates a miss from an erasure and from a truncated run. */
enum class select_result_t : std::uint8_t {

    /** Neither the write buffer nor any level carries that key. */
    nothing_k,

    /** A tombstone carries that key, so its value has been erased. */
    erased_k,

    /** Every byte the key slot claims arrived. */
    success_k,

    /** The medium moved fewer bytes than the key slot claims, so this one key failed. */
    incomplete_k,
};

#pragma endregion Vocabulary

#pragma region Snapshot

/**
 *  @brief One level set held against publication, and the point and batched reads that run against it.
 *
 *  A snapshot borrows the tree it counted itself into, so the tree must outlive it. It is move-only
 *  because the count it holds belongs to exactly one owner.
 */
template <typename key_slot_type_, typename volume_type_>
class collection_snapshot {
  public:

#pragma region Types

    /** The key slot every run of the held levels holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of the held levels lives on. */
    using volume_t = volume_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The tree this snapshot holds a level set of. */
    using tree_t = lsm_tree<key_slot_t, volume_t>;

    /** The hold itself, which keeps the level set alive for as long as this snapshot lives. */
    using levels_t = typename tree_t::snapshot_t;

    /** How the values of this collection are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so reading one needs no I/O. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

#pragma endregion Types

#pragma region Construction

    collection_snapshot() noexcept = default;
    collection_snapshot(collection_snapshot const&) = delete;
    collection_snapshot& operator=(collection_snapshot const&) = delete;

    collection_snapshot(collection_snapshot&& other) noexcept
        : tree_(std::exchange(other.tree_, nullptr)), levels_(std::move(other.levels_)) {}

    collection_snapshot& operator=(collection_snapshot&& other) noexcept {
        if (this == &other) return *this;
        tree_ = std::exchange(other.tree_, nullptr);
        levels_ = std::move(other.levels_);
        return *this;
    }

    /** Holds the live level set of @p tree, which must outlive the snapshot. */
    [[nodiscard]] static collection_snapshot of(tree_t const& tree) noexcept {
        collection_snapshot taken;
        taken.tree_ = &tree;
        taken.levels_ = tree.snapshot();
        return taken;
    }

#pragma endregion Construction

#pragma region Shape

    /** Whether this snapshot holds a level set. */
    [[nodiscard]] bool holding() const noexcept { return tree_ != nullptr && levels_.holding(); }

    /** The level set held, which a scan walks the runs of. */
    [[nodiscard]] levels_t const& levels() const noexcept { return levels_; }

    /** Key slots every level holds together, one key counted once per run that holds it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept { return levels_.population_count(); }

    /** Bytes every run of every level takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return levels_.size_bytes(); }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether the held levels carry @p stored_key, and how many bytes its value takes.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c success_k.
     */
    [[nodiscard]] select_result_t find(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        size = 0;
        lsm_tree_position_t position;
        find_result_t const found = tree_->find(levels_, stored_key, position);
        if (found == find_result_t::nothing_k) return select_result_t::nothing_k;
        if (found == find_result_t::stored_key_k) return select_result_t::erased_k;
        size = static_cast<stored_value_size_t>(
            levels_.level(position.level_number).value_window(position.level_position).bytes);
        return select_result_t::success_k;
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[inout] lane The lane the read is submitted and reaped on, held by the calling thread.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     *  @return @c not_found_k for a miss or an erasure, @c io_k for a run shorter than its index claims.
     */
    status_t select(io_lane_t& lane, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) const noexcept {
        size = 0;
        stored_value_size_t wanted = 0;
        if (find(stored_key, wanted) != select_result_t::success_k)
            return {status_code_t::not_found_k, "No level of this snapshot carries that key"};
        if (into.size() < wanted) return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};

        std::uint64_t delivered = 0;
        auto sink = [&](std::size_t, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            if (!bytes.empty()) std::memcpy(into.data() + offset, bytes.data(), bytes.size());
            delivered += bytes.size();
        };
        std::array<stored_key_t, 1> const keys {stored_key};
        std::array<find_result_t, 1> answers {find_result_t::nothing_k};
        if (status_t const read = tree_->read_values(lane, levels_, std::span<stored_key_t const> {keys},
                                                     std::span<find_result_t> {answers}, sink);
            read.failed())
            return read;

        // One key has no rest of a batch to keep, so a block short of its last one fails the whole call.
        if (delivered != wanted) return {status_code_t::io_k, "A run is shorter than its index claims"};
        size = wanted;
        return {};
    }

    /**
     *  @brief Reads the values of @p stored_keys, refilling the lane as completions retire.
     *  @param[inout] lane The lane the reads are submitted and reaped on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *  @return Success, or the failure that stopped the batch rather than one of its keys.
     *
     *  @note A value larger than one block arrives as several calls, each carrying the bytes of the
     *      value before it, and a key whose value is present and empty is reported once with no bytes.
     */
    template <typename sink_type_>
    status_t select(io_lane_t& lane, std::span<stored_key_t const> stored_keys, std::span<select_result_t> results,
                    sink_type_&& sink) const noexcept {
        if (results.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched select answers one result per key"};

        // The tree answers in its own vocabulary and the sink is told nothing about sizes, so the
        // bytes every key owed are counted here and compared once the read has run.
        st::basic_vector<find_result_t> answers;
        st::basic_vector<std::uint64_t> delivered;
        if (answers.resize(stored_keys.size()) != st::status_t::success_k ||
            delivered.resize(stored_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A snapshot could not track the keys of a batch"};
        for (std::size_t request = 0; request != stored_keys.size(); ++request) {
            answers[request] = find_result_t::nothing_k;
            delivered[request] = 0;
        }

        auto relay = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            delivered[request] += bytes.size();
            sink(request, offset, bytes);
        };
        status_t const read = tree_->read_values(lane, levels_, stored_keys, std::span<find_result_t> {answers}, relay);

        std::size_t truncated = 0;
        for (std::size_t request = 0; request != stored_keys.size(); ++request) {
            if (answers[request] == find_result_t::nothing_k) {
                results[request] = select_result_t::nothing_k;
                continue;
            }
            if (answers[request] == find_result_t::stored_key_k) {
                results[request] = select_result_t::erased_k;
                continue;
            }
            stored_value_size_t wanted = 0;
            [[maybe_unused]] select_result_t const carried = find(stored_keys[request], wanted);
            if (delivered[request] == wanted) results[request] = select_result_t::success_k;
            else {
                results[request] = select_result_t::incomplete_k;
                ++truncated;
            }
        }

        // A block short of what it asked for fails its own key; a cancelled or refused one fails the batch.
        if (read.failed() && (truncated == 0 || !(read == status_code_t::io_k))) return read;
        return {};
    }

#pragma endregion Lookups

  private:

#pragma region Private Members

    /** The tree the held level set belongs to, borrowed and outliving the snapshot. */
    tree_t const* tree_ = nullptr;

    /** The hold itself, released when the snapshot dies. */
    levels_t levels_;

#pragma endregion Private Members
};

#pragma endregion Snapshot

} // namespace unum::ustore::nvme
