/**
 *  @file src/nvme/collection_scanner.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief An ordered walk over one collection of any value layout: the write buffer merged with every run.
 *
 *  A run answers a key slot by sorted position, so the walk is a merge over one cursor per run and one
 *  over the staged writes, and nothing is materialized but the staged keys, which the write buffer's
 *  own bound already caps. Where several runs carry the same key the shallowest level wins, and inside
 *  a level that may overlap the youngest run does; the staged writes outrank every run.
 *
 *  There is no prefetcher here, and adding one would be a mistake rather than an optimization. A scan
 *  that wants bytes hands the next keys to @c collection::select, which already refills the ring as
 *  completions retire; a second pipeline reading ahead on the same channel would poll the first one's
 *  completions and drop them, since a read pipeline silently discards tags it did not mint.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`

#include <span>    // `std::span`
#include <utility> // `std::exchange`, `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/status.hpp" // `status_t`, `status_code_t`, `expected`

#include "nvme/collection.hpp"          // `collection`
#include "nvme/collection_snapshot.hpp" // `collection_snapshot`, `select_result_t`
#include "nvme/stored_key.hpp"          // `limit`
#include "nvme/stored_value.hpp"        // `stored_value_size_t`

namespace unum::ustore::nvme {

#pragma region Scanner

/**
 *  @brief Every key of one collection in ascending order, each with what it is and how wide it is.
 *
 *  The scanner holds the level set it walks for its whole life, so a publication on another thread
 *  waits for it; a scan is taken, walked and dropped rather than kept beside a writer.
 */
template <typename key_slot_type_, typename volume_type_>
class collection_scanner {
  public:

#pragma region Types

    /** The key slot every run of the scanned collection holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of the scanned collection lives on. */
    using volume_t = volume_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The collection this scanner walks. */
    using collection_t = collection<key_slot_t, volume_t>;

    /** One level set of that collection's tree held against publication. */
    using snapshot_t = collection_snapshot<key_slot_t, volume_t>;

    /** One staged write as the merge sees it. */
    using staged_entry_t = typename collection_t::staged_entry_t;

#pragma endregion Types

#pragma region Construction

    collection_scanner() noexcept = default;
    collection_scanner(collection_scanner const&) = delete;
    collection_scanner& operator=(collection_scanner const&) = delete;
    collection_scanner(collection_scanner&&) noexcept = default;
    collection_scanner& operator=(collection_scanner&&) noexcept = default;

    /** A scan of @p source positioned on its smallest key, which must outlive the scanner. */
    static expected<collection_scanner> over(collection_t const& source) noexcept {
        collection_scanner scanner;
        if (status_t const listed = source.staged_in_order(scanner.staged_); listed.failed()) return listed;
        scanner.levels_ = snapshot_t::of(source.tree());

        std::size_t runs = 0;
        for (std::size_t number = 0; number != scanner.levels_.levels().size(); ++number)
            runs += scanner.levels_.levels().level(number).size();
        if (scanner.cursors_.reserve(runs) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A scan could not hold a cursor per run"};

        for (std::size_t number = 0; number != scanner.levels_.levels().size(); ++number) {
            auto const& level = scanner.levels_.levels().level(number);
            for (std::size_t index = 0; index != level.size(); ++index) {
                run_cursor_t cursor;
                cursor.level_number = static_cast<std::uint32_t>(number);
                cursor.run_index = static_cast<std::uint32_t>(index);
                cursor.position = 0;
                cursor.population = level.run(index).population_count();
                [[maybe_unused]] st::status_t const placed = scanner.cursors_.push_back(std::move(cursor));
            }
        }
        scanner.advance();
        return scanner;
    }

#pragma endregion Construction

#pragma region Walk

    /** Whether every key has been visited. */
    [[nodiscard]] bool reached_end() const noexcept { return state_ == select_result_t::nothing_k; }

    /** The key the scan sits on, meaningless once the walk has ended. */
    [[nodiscard]] stored_key_t stored_key() const noexcept { return stored_key_; }

    /** Whether the key the scan sits on carries a value or a tombstone. */
    [[nodiscard]] select_result_t state() const noexcept { return state_; }

    /** Bytes the value of that key takes, which is zero for a tombstone and for an empty value. */
    [[nodiscard]] stored_value_size_t size() const noexcept { return size_; }

    /** Moves to the next key, taking the newest version of it and skipping the versions behind it. */
    void advance() noexcept {
        stored_key_t smallest = limit<stored_key_t>::max();
        bool any = false;
        if (staged_at_ != staged_.size()) {
            smallest = staged_[staged_at_].stored_key;
            any = true;
        }
        for (std::size_t index = 0; index != cursors_.size(); ++index) {
            if (cursors_[index].position == cursors_[index].population) continue;
            stored_key_t const head = key_slot_at_(cursors_[index]).stored_key;
            if (any && !(head < smallest)) continue;
            smallest = head;
            any = true;
        }
        if (!any) {
            state_ = select_result_t::nothing_k;
            size_ = 0;
            return;
        }

        stored_key_ = smallest;
        if (staged_at_ != staged_.size() && staged_[staged_at_].stored_key == smallest) take_staged_();
        else take_youngest_run_(smallest);

        if (staged_at_ != staged_.size() && staged_[staged_at_].stored_key == smallest) ++staged_at_;
        for (std::size_t index = 0; index != cursors_.size(); ++index)
            if (cursors_[index].position != cursors_[index].population &&
                key_slot_at_(cursors_[index]).stored_key == smallest)
                ++cursors_[index].position;
    }

#pragma endregion Walk

  private:

#pragma region Private Types

    /** Where one run of one level the scan walks has got to. */
    struct run_cursor_t {

        /** The level the run belongs to, zero being the one flushes land on. */
        std::uint32_t level_number = 0;

        /** The run inside that level, older first where the runs overlap. */
        std::uint32_t run_index = 0;

        /** The next sorted position inside that run. */
        std::uint64_t position = 0;

        /** Key slots the run holds, past which the cursor is spent. */
        std::uint64_t population = 0;
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** The key slot @p cursor sits on, which must not be spent. */
    [[nodiscard]] key_slot_t const& key_slot_at_(run_cursor_t const& cursor) const noexcept {
        return levels_.levels().level(cursor.level_number).run(cursor.run_index).key_slot(cursor.position);
    }

    /** Takes the staged write as the answer, which outranks every run that carries the same key. */
    void take_staged_() noexcept {
        state_ = staged_[staged_at_].state;
        size_ = staged_[staged_at_].size;
    }

    /** Takes the shallowest and, inside a level, the youngest run carrying @p smallest as the answer. */
    void take_youngest_run_(stored_key_t smallest) noexcept {
        std::size_t chosen = cursors_.size();
        for (std::size_t index = 0; index != cursors_.size(); ++index) {
            if (cursors_[index].position == cursors_[index].population) continue;
            if (!(key_slot_at_(cursors_[index]).stored_key == smallest)) continue;
            if (chosen != cursors_.size() && !outranks_(cursors_[index], cursors_[chosen])) continue;
            chosen = index;
        }
        key_slot_t const& slot = key_slot_at_(cursors_[chosen]);
        state_ = slot.is_tombstone() ? select_result_t::erased_k : select_result_t::success_k;
        size_ = slot.is_tombstone() ? 0 : slot.size;
    }

    /** Whether @p first carries a newer version of a key than @p second does. */
    [[nodiscard]] static bool outranks_(run_cursor_t const& first, run_cursor_t const& second) noexcept {
        if (first.level_number != second.level_number) return first.level_number < second.level_number;
        return first.run_index > second.run_index;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The level set the scan walks, held for the whole life of the scanner. */
    snapshot_t levels_;

    /** Every staged write in ascending order of its key. */
    st::basic_vector<staged_entry_t> staged_;

    /** One cursor per run of every level the snapshot holds. */
    st::basic_vector<run_cursor_t> cursors_;

    /** The staged write the merge would take next. */
    std::size_t staged_at_ = 0;

    /** The key the scan sits on. */
    stored_key_t stored_key_ = stored_key_t {};

    /** What that key carries, which reads as @c nothing_k once the walk has ended. */
    select_result_t state_ = select_result_t::nothing_k;

    /** Bytes the value of that key takes. */
    stored_value_size_t size_ = 0;

#pragma endregion Private Members
};

#pragma endregion Scanner

} // namespace unum::ustore::nvme
