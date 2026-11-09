/**
 *  @file include/ustore/blobs/nvme_store_cursor.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The ordered walk over every key of one store, read a page at a time inside
 *      the ledger epochs.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_key_slot.hpp" // `limit`
#include "ustore/blobs/nvme_store.hpp"    // `nvme_store`
#include "ustore/utilities/folds.hpp"     // `fold_concept`

namespace unum::ustore::blobs {

#pragma region Store Cursor

#pragma region Cursor

/**
 *  @brief Every key of one store in ascending order, each with what it is and how wide it is, read
 *      a page at a time.
 *
 *  Each page is read inside the ledger's epochs from whichever view is published, and the epochs
 *  are left between pages, so a commit, merge, flush or compaction replacing a view waits for at
 *  most one page of a scan. Every page reads at the one stamp the scan is pinned at, whose versions
 *  every merge keeps, and starts at the key past the page before it, which that page found without
 *  handing it out, so no key is visited twice or skipped. A head scan pins the published stamp it
 *  starts at for as long as it lives.
 */
template <typename key_slot_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, utilities::fold_concept fold_type_, typename device_fold_type_, typename row_kit_type_>
class nvme_store_cursor {
  public:

#pragma region Types

    /** The key slot every run of the walked store holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of the walked store lives on. */
    using volume_t = volume_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The store this cursor walks. */
    using nvme_store_t =
        nvme_store<key_slot_t, volume_t, atomic_reference_, waiter_type_, fold_type_, device_fold_type_, row_kit_type_>;

    /** The view of that store the walk reads. */
    using view_t = typename nvme_store_t::view_t;

    /** One memory run of that view. */
    using memory_run_t = typename nvme_store_t::memory_run_t;

    /** One key of a page. */
    using entry_t = scan_entry<stored_key_t>;

    /** Keys one page holds. */
    static constexpr std::size_t page_keys_k = scan_page_keys<stored_key_t>();

#pragma endregion Types

#pragma region Construction

    nvme_store_cursor() noexcept = default;
    nvme_store_cursor(nvme_store_cursor const&) = delete;
    nvme_store_cursor& operator=(nvme_store_cursor const&) = delete;
    nvme_store_cursor(nvme_store_cursor&&) noexcept = default;
    nvme_store_cursor& operator=(nvme_store_cursor&&) noexcept = default;

    /** A walk over @p source positioned on its first key not below @p from, or not above it in
     *  @p direction descending, where @p source must outlive the cursor, seeing what a reader
     *  pinned at @p stamp sees, announced on executor thread @p thread_index's epoch slot while it
     *  reads a page, the first of which it reads before returning. */
    static expected<nvme_store_cursor> over(nvme_store_t const& source, stored_key_t from = limit<stored_key_t>::min(),
                                            transaction_stamp_t stamp = head_stamp_k,
                                            std::size_t thread_index = outside_executor_k,
                                            scan_direction_t direction = scan_direction_t::ascending_k) noexcept {
        nvme_store_cursor cursor;
        cursor.source_ = &source;
        cursor.thread_index_ = thread_index;
        cursor.direction_ = direction;
        cursor.stamp_ = stamp == head_stamp_k ? source.runs_.ledger().pin_reader(cursor.pin_) : stamp;
        if (status_t const filled = cursor.fill_(from); filled.failed()) return filled;
        return cursor;
    }

    /** A walk over @p source as @c over above, every page of which reads through @p read, which a
     *  caller inside the ledger's epochs took and keeps walkable while the cursor lives. */
    static expected<nvme_store_cursor> over(nvme_store_t const& source, typename nvme_store_t::reading_t const& read,
                                            stored_key_t from = limit<stored_key_t>::min(),
                                            scan_direction_t direction = scan_direction_t::ascending_k) noexcept {
        nvme_store_cursor cursor;
        cursor.source_ = &source;
        cursor.direction_ = direction;
        cursor.stamp_ = read.stamp;
        cursor.view_ = read.view;
        if (status_t const filled = cursor.fill_(from); filled.failed()) return filled;
        return cursor;
    }

#pragma endregion Construction

#pragma region Walk

    /** Whether every key has been visited. */
    [[nodiscard]] bool reached_end() const noexcept { return at_ == page().size() && filled_ <= page_keys_k; }

    /** The key the scan sits on, meaningless once the walk has ended. */
    [[nodiscard]] stored_key_t stored_key() const noexcept { return page_[at_].stored_key; }

    /** Whether the key the scan sits on carries a value or a removal, @c missing_k once the
     *  walk has ended. */
    [[nodiscard]] lookup_result_t state() const noexcept {
        return reached_end() ? lookup_result_t::missing_k : page_[at_].state;
    }

    /** Bytes the value of that key takes, which is zero for a removal and for an empty value. */
    [[nodiscard]] stored_value_size_t size() const noexcept { return reached_end() ? 0 : page_[at_].size; }

    /** The keys of the page the scan sits on, every one of which comes before the keys of
     *  the next page. */
    [[nodiscard]] std::span<entry_t const> page() const noexcept {
        return std::span<entry_t const> {page_.data(), std::min(filled_, page_keys_k)};
    }

    /** Whether a page follows the one the scan sits on, and in @p from the key it starts at. */
    [[nodiscard]] bool follows(stored_key_t& from) const noexcept {
        if (filled_ <= page_keys_k) return false;
        from = page_[page_keys_k].stored_key;
        return true;
    }

    /** Moves to the next key, reading the next page once this one is spent. */
    status_t advance() noexcept {
        if (at_ != page().size()) ++at_;
        stored_key_t from {};
        if (at_ != page().size() || !follows(from)) return {};
        return fill_(from);
    }

#pragma endregion Walk

  private:

#pragma region Private Types

    /** One reader inside the scanned collection's epochs, announced while a page is read. */
    using reader_t = typename nvme_store_t::published_runs_t::reader_t;

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

        /** One past the last position a descending walk has yet to visit, zero once spent. */
        std::uint64_t end = 0;
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** Reads the keys from @p from on into the page inside the ledger's epochs, from the view
     *  published now, with one key past the page beside it, which the next page starts at. */
    status_t fill_(stored_key_t from) noexcept {
        // A walk lent a reading keeps its view between pages, which the lender's epochs keep alive.
        if (view_) return read_page_(from);
        reader_t const inside = source_->runs_.enter(thread_index_);
        // Every version at or below the pinned stamp stays in whichever view a
        // merge publishes later.
        view_ = source_->runs_.view();
        status_t const read = read_page_(from);
        view_ = nullptr;
        return read;
    }

    /** Reads the keys from @p from on into the page from the view the walk holds now. */
    status_t read_page_(stored_key_t from) noexcept {
        at_ = 0;
        filled_ = 0;
        status_t const positioned = position_(from);
        bool const descending = direction_ == scan_direction_t::descending_k;
        while (positioned.succeeded() && filled_ != page_.size() &&
               (descending ? next_down_(page_[filled_]) : next_(page_[filled_])))
            ++filled_;
        return positioned;
    }

    /** Places a cursor on the first key not below @p from in every memory run and every run
     *  of every level. */
    status_t position_(stored_key_t from) noexcept {
        std::span<memory_run_t const* const> const memory_runs = view_->memory_runs.runs();
        if (memory_positions_.resize(memory_runs.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A scan could not hold a cursor per memory run"};
        if (memory_ends_.resize(memory_runs.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A scan could not hold a cursor per memory run"};
        for (std::size_t index = 0; index != memory_runs.size(); ++index) {
            memory_run_t const& run = *memory_runs[index];
            memory_positions_[index] = run.lower_bound(from);
            // A descending walk starts past every version of the start key itself.
            std::size_t end = memory_positions_[index];
            while (end != run.size() && run.stored_key(end) == from) ++end;
            memory_ends_[index] = end;
        }

        typename nvme_store_t::level_set_t const& levels = *view_->levels;
        std::size_t runs = 0;
        for (std::size_t number = 0; number != levels.size(); ++number) runs += levels.level(number).size();
        cursors_.clear();
        if (cursors_.reserve(runs) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A scan could not hold a cursor per run"};
        for (std::size_t number = 0; number != levels.size(); ++number) {
            auto const& level = levels.level(number);
            for (std::size_t index = 0; index != level.size(); ++index) {
                run_cursor_t cursor;
                cursor.level_number = static_cast<std::uint32_t>(number);
                cursor.run_index = static_cast<std::uint32_t>(index);
                cursor.population = level.run(index).population_count();
                cursor.position = level.run(index).lower_bound(from);
                cursor.end = cursor.position;
                while (cursor.end != cursor.population && level.run(index).key_slot(cursor.end).stored_key == from)
                    ++cursor.end;
                [[maybe_unused]] st::status_t const placed = cursors_.push_back(std::move(cursor));
            }
        }
        return {};
    }

    /** Moves the cursors past the next key, taking its newest version at or below the scan's stamp
     *  into @p entry and skipping the versions behind it and any key whose every version is newer;
     *  answers whether a key was left. */
    bool next_(entry_t& entry) noexcept {
        std::span<memory_run_t const* const> const memory_runs = view_->memory_runs.runs();
        for (;;) {
            stored_key_t smallest = limit<stored_key_t>::max();
            bool any = false;
            for (std::size_t index = 0; index != memory_runs.size(); ++index) {
                if (memory_positions_[index] == memory_runs[index]->size()) continue;
                stored_key_t const head = memory_runs[index]->stored_key(memory_positions_[index]);
                if (any && !(head < smallest)) continue;
                smallest = head;
                any = true;
            }
            for (std::size_t index = 0; index != cursors_.size(); ++index) {
                if (cursors_[index].position == cursors_[index].population) continue;
                stored_key_t const head = key_slot_at_(cursors_[index]).stored_key;
                if (any && !(head < smallest)) continue;
                smallest = head;
                any = true;
            }
            if (!any) return false;

            entry.stored_key = smallest;
            bool const taken = take_newest_memory_run_(entry) || take_youngest_run_(entry);

            // Every run holds the versions of a key side by side, so each cursor passes all of
            // them at once.
            for (std::size_t index = 0; index != memory_runs.size(); ++index)
                while (memory_positions_[index] != memory_runs[index]->size() &&
                       memory_runs[index]->stored_key(memory_positions_[index]) == smallest)
                    ++memory_positions_[index];
            for (std::size_t index = 0; index != cursors_.size(); ++index)
                while (cursors_[index].position != cursors_[index].population &&
                       key_slot_at_(cursors_[index]).stored_key == smallest)
                    ++cursors_[index].position;
            if (taken) return true;
        }
    }

    /**
     *  @brief Moves the cursors below the next key downwards, taking its newest version at or
     *      below the scan's stamp into @p entry, as @c next_ does upwards; answers whether a
     *      key was left.
     *
     *  Each run's versions of one key sit newest first, so a cursor steps down to the first version
     *  of the key it ends at, from where the takes read forward as they do for an ascending walk,
     *  and then ends there.
     */
    bool next_down_(entry_t& entry) noexcept {
        std::span<memory_run_t const* const> const memory_runs = view_->memory_runs.runs();
        for (;;) {
            stored_key_t largest = limit<stored_key_t>::min();
            bool any = false;
            for (std::size_t index = 0; index != memory_runs.size(); ++index) {
                if (memory_ends_[index] == 0) continue;
                stored_key_t const tail = memory_runs[index]->stored_key(memory_ends_[index] - 1);
                if (any && !(largest < tail)) continue;
                largest = tail;
                any = true;
            }
            for (std::size_t index = 0; index != cursors_.size(); ++index) {
                if (cursors_[index].end == 0) continue;
                stored_key_t const tail = key_slot_below_(cursors_[index]).stored_key;
                if (any && !(largest < tail)) continue;
                largest = tail;
                any = true;
            }
            if (!any) return false;

            // A cursor not on the key sits past it, where the takes find no version of it.
            for (std::size_t index = 0; index != memory_runs.size(); ++index) {
                memory_run_t const& run = *memory_runs[index];
                std::size_t start = memory_ends_[index];
                while (start != 0 && run.stored_key(start - 1) == largest) --start;
                memory_positions_[index] = start;
            }
            for (run_cursor_t& cursor : cursors_) {
                auto const& run = view_->levels->level(cursor.level_number).run(cursor.run_index);
                std::uint64_t start = cursor.end;
                while (start != 0 && run.key_slot(start - 1).stored_key == largest) --start;
                cursor.position = start;
            }
            entry.stored_key = largest;
            bool const taken = take_newest_memory_run_(entry) || take_youngest_run_(entry);
            for (std::size_t index = 0; index != memory_runs.size(); ++index)
                memory_ends_[index] = memory_positions_[index];
            for (run_cursor_t& cursor : cursors_) cursor.end = cursor.position;
            if (taken) return true;
        }
    }

    /** The key slot right below where @p cursor ends, which must not be spent. */
    [[nodiscard]] key_slot_t key_slot_below_(run_cursor_t const& cursor) const noexcept {
        return view_->levels->level(cursor.level_number).run(cursor.run_index).key_slot(cursor.end - 1);
    }

    /** The key slot @p cursor sits on, which must not be spent. */
    [[nodiscard]] key_slot_t key_slot_at_(run_cursor_t const& cursor) const noexcept {
        return view_->levels->level(cursor.level_number).run(cursor.run_index).key_slot(cursor.position);
    }

    /** Takes the newest memory run carrying a version of @p entry's key at or below the scan's
     *  stamp as its answer, which outranks every level; answers whether any does. */
    bool take_newest_memory_run_(entry_t& entry) noexcept {
        std::span<memory_run_t const* const> const memory_runs = view_->memory_runs.runs();
        for (std::size_t index = 0; index != memory_runs.size(); ++index) {
            memory_run_t const& run = *memory_runs[index];
            std::size_t position = memory_positions_[index];
            // A merged run holds a key's versions newest first, and the first at or below
            // the stamp answers.
            while (position != run.size() && run.stored_key(position) == entry.stored_key &&
                   run.stamp(position) > stamp_)
                ++position;
            if (position == run.size() || !(run.stored_key(position) == entry.stored_key)) continue;
            delta_view_t const delta = run.delta(position);
            bool const hidden = nvme_store_t::hidden_in_memory_(reading_(), entry.stored_key, run.stamp(position));
            take_(hidden ? delta_kind_t::remove_range_k : delta.kind, delta.bytes.size(), entry);
            return true;
        }
        return false;
    }

    /** Takes the newest version of @p entry's key at or below the scan's stamp, which does @p kind
     *  and carries @p size bytes, as its answer, folding the versions below a change. */
    void take_(delta_kind_t kind, std::size_t size, entry_t& entry) noexcept {
        entry.size = 0;
        if (kind == delta_kind_t::change_k)
            entry.state = source_->find_folded_(reading_(), entry.stored_key, entry.size);
        else if (utilities::removes(kind)) entry.state = lookup_result_t::removed_k;
        else {
            entry.state = lookup_result_t::written_k;
            entry.size = static_cast<stored_value_size_t>(size);
        }
    }

    /**
     *  @brief Takes the shallowest and, inside a level, the youngest run carrying a version
     *      of @p entry's key at or below the scan's stamp as its answer, the newest such
     *      version inside it.
     *  @return Whether any run carries one.
     */
    bool take_youngest_run_(entry_t& entry) noexcept {
        std::size_t chosen = cursors_.size();
        std::uint64_t chosen_position = 0;
        for (std::size_t index = 0; index != cursors_.size(); ++index) {
            run_cursor_t const& cursor = cursors_[index];
            if (chosen != cursors_.size() && !outranks_(cursor, cursors_[chosen])) continue;
            auto const& run = view_->levels->level(cursor.level_number).run(cursor.run_index);
            std::uint64_t position = cursor.position;
            while (position != cursor.population && run.key_slot(position).stored_key == entry.stored_key &&
                   run.stamp(position) > stamp_)
                ++position;
            if (position == cursor.population || !(run.key_slot(position).stored_key == entry.stored_key)) continue;
            chosen = index;
            chosen_position = position;
        }
        if (chosen == cursors_.size()) return false;
        run_cursor_t const& cursor = cursors_[chosen];
        auto const& run = view_->levels->level(cursor.level_number).run(cursor.run_index);
        lsm_tree_position_t const at {cursor.level_number, lsm_level_position_t {cursor.run_index, chosen_position}};
        bool const hidden = nvme_store_t::hidden_in_levels_(reading_(), entry.stored_key, at);
        take_(hidden ? delta_kind_t::remove_range_k : run.kind(chosen_position),
              run.value_window(chosen_position).bytes, entry);
        return true;
    }

    /** The view the page being read walks, at the scan's stamp. */
    [[nodiscard]] typename nvme_store_t::reading_t reading_() const noexcept {
        return typename nvme_store_t::reading_t {view_, stamp_};
    }

    /** Whether @p first carries a newer version of a key than @p second does. */
    [[nodiscard]] static bool outranks_(run_cursor_t const& first, run_cursor_t const& second) noexcept {
        if (first.level_number != second.level_number) return first.level_number < second.level_number;
        return first.run_index > second.run_index;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The store walked, which outlives the cursor. */
    nvme_store_t const* source_ = nullptr;

    /** The published stamp a head scan reads at, which keeps its versions in every merge while
     *  the scan lives. */
    typename nvme_store_t::pin_t pin_;

    /** The stamp the scan reads at, which sees the newest version of every key at or below it. */
    transaction_stamp_t stamp_ = head_stamp_k;

    /** The executor thread the scan announces on while it reads a page. */
    std::size_t thread_index_ = outside_executor_k;

    /** The view the page being read walks, null between pages, when no reader keeps it alive, or
     *  the view a lender's reading names for every page. */
    view_t const* view_ = nullptr;

    /** The next sorted position inside every memory run of the view, newest run first. */
    st::vector<std::size_t> memory_positions_;

    /** Per memory run, newest first, one past the last position a descending walk is yet to see. */
    st::vector<std::size_t> memory_ends_;

    /** The order the walk hands its keys out in. */
    scan_direction_t direction_ = scan_direction_t::ascending_k;

    /** One cursor per run of every level the view names. */
    st::vector<run_cursor_t> cursors_;

    /** The keys of the page, then the key the next page starts at. */
    std::array<entry_t, page_keys_k + 1> page_ {};

    /** Entries the page read, a page and one more while another page follows. */
    std::size_t filled_ = 0;

    /** The entry of the page the scan sits on. */
    std::size_t at_ = 0;

#pragma endregion Private Members
};

#pragma endregion Cursor

#pragma endregion Store Cursor

} // namespace unum::ustore::blobs
