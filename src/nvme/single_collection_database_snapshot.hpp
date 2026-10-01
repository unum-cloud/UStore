/**
 *  @file src/nvme/single_collection_database_snapshot.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief One moment of a single-collection database: the level set it held, and the stamp it held it at.
 *
 *  Taking one copies no run and moves no byte. It is the collection's own hold on a level set, taken
 *  after a flush so nothing stays behind in the write buffer, carrying the commit stamp the hold was
 *  taken at. Every commit ordered before that stamp is inside the snapshot and every later one is not,
 *  which is the whole of what "its moment" means here.
 */

#pragma once

#include <cstdint> // `std::uint64_t`

#include <span>    // `std::span`
#include <utility> // `std::move`

#include "ustore/status.hpp" // `status_t`, `expected`
#include "ustore/types.hpp"  // `byte_t`

#include "nvme/collection_snapshot.hpp" // `collection_snapshot`, `select_result_t`
#include "nvme/io_channel.hpp"          // `io_channel_t`
#include "nvme/read_set.hpp"            // `transaction_stamp_t`, `unwritten_stamp_k`
#include "nvme/stored_value.hpp"        // `stored_value_size_t`

namespace unum::ustore::nvme {

#pragma region Snapshot

/**
 *  @brief The level set of one single-collection database held against publication, and its stamp.
 *
 *  Move-only, because the hold underneath belongs to exactly one owner, and it borrows the tree it
 *  counted itself into, so the database must outlive every snapshot taken of it.
 */
template <typename key_slot_type_, typename volume_type_>
class single_collection_database_snapshot {
  public:

#pragma region Types

    /** The key slot every run of the held levels holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of the held levels lives on. */
    using volume_t = volume_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The hold itself, which keeps the level set alive for as long as this snapshot lives. */
    using levels_t = collection_snapshot<key_slot_t, volume_t>;

#pragma endregion Types

#pragma region Construction

    single_collection_database_snapshot() noexcept = default;
    single_collection_database_snapshot(single_collection_database_snapshot const&) = delete;
    single_collection_database_snapshot& operator=(single_collection_database_snapshot const&) = delete;
    single_collection_database_snapshot(single_collection_database_snapshot&&) noexcept = default;
    single_collection_database_snapshot& operator=(single_collection_database_snapshot&&) noexcept = default;

    /**
     *  @brief The snapshot @p levels holds, as of @p stamp.
     *  @param[in] levels The hold the collection handed out, moved in and owned from here on.
     *  @param[in] stamp The watermark every commit inside this snapshot orders strictly before.
     */
    [[nodiscard]] static single_collection_database_snapshot of(levels_t&& levels, transaction_stamp_t stamp) noexcept {
        single_collection_database_snapshot taken;
        taken.levels_ = std::move(levels);
        taken.stamp_ = stamp;
        return taken;
    }

#pragma endregion Construction

#pragma region Shape

    /** Whether this snapshot holds a level set. */
    [[nodiscard]] bool holding() const noexcept { return levels_.holding(); }

    /** The watermark every commit inside this snapshot orders strictly before. */
    [[nodiscard]] transaction_stamp_t stamp() const noexcept { return stamp_; }

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
        return levels_.find(stored_key, size);
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[inout] channel The channel the read is submitted and completed on, held by the calling thread.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     */
    status_t select(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) const noexcept {
        return levels_.select(channel, stored_key, into, size);
    }

    /**
     *  @brief Reads the values of @p stored_keys, refilling the channel as completions retire.
     *  @param[inout] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     */
    template <typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<select_result_t> results, sink_type_&& sink) const noexcept {
        return levels_.select(channel, stored_keys, results, std::forward<sink_type_>(sink));
    }

#pragma endregion Lookups

  private:

#pragma region Private Members

    /** The hold itself, released when the snapshot dies. */
    levels_t levels_;

    /** The watermark every commit inside this snapshot orders strictly before. */
    transaction_stamp_t stamp_ = unwritten_stamp_k;

#pragma endregion Private Members
};

#pragma endregion Snapshot

} // namespace unum::ustore::nvme
