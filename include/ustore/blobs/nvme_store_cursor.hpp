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

#include <algorithm> // `std::sort`
#include <array>     // `std::array`
#include <span>      // `std::span`
#include <utility>   // `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_key_slot.hpp"   // `limit`
#include "ustore/blobs/nvme_store.hpp"      // `nvme_store`
#include "ustore/utilities/folds.hpp"       // `fold_concept`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocator`

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
     *  reads a page, the first of which it reads before returning, its level cursors carved from
     *  @p scratch, which outlives it. */
    static expected<nvme_store_cursor> over(nvme_store_t const& source, ustore_arena_t scratch,
                                            stored_key_t from = limit<stored_key_t>::min_k,
                                            transaction_stamp_t stamp = head_stamp_k, std::size_t thread_index = 0,
                                            scan_direction_t direction = scan_direction_t::ascending_k) noexcept {
        nvme_store_cursor cursor {scratch};
        cursor.source_ = &source;
        cursor.thread_index_ = thread_index;
        cursor.direction_ = direction;
        cursor.stamp_ = stamp == head_stamp_k ? source.runs_.ledger().pin_reader(cursor.pin_) : stamp;
        if (status_t const filled = cursor.fill_(from); filled.failed()) return filled;
        return cursor;
    }

    /** A walk over @p source as @c over above, every page of which reads through @p read. A walk
     *  given the caller's reading keeps its view between pages; the snapshot behind it keeps
     *  those runs alive until it is freed, so the walk places its cursors once and keeps them
     *  from page to page. */
    static expected<nvme_store_cursor> over(nvme_store_t const& source, typename nvme_store_t::reading_t const& read,
                                            ustore_arena_t scratch, stored_key_t from = limit<stored_key_t>::min_k,
                                            scan_direction_t direction = scan_direction_t::ascending_k) noexcept {
        nvme_store_cursor cursor {scratch};
        cursor.source_ = &source;
        cursor.direction_ = direction;
        cursor.stamp_ = read.stamp;
        cursor.view_ = read.view;
        if (status_t const positioned = cursor.position_(from); positioned.failed()) return positioned;
        cursor.read_page_(0);
        return cursor;
    }

#pragma endregion Construction

#pragma region Split

    /** Whether the tasks of a split reach the bound of the walk, or stop before a key the next walk
     *  starts at. */
    enum class split_end_t : std::uint8_t {
        reaches_bound_k,
        stops_k,
    };

    /** The tasks a split cut, and where the walk after them starts. */
    struct split_t {
        std::size_t tasks = 0;
        stored_key_t stop {};
        split_end_t end = split_end_t::reaches_bound_k;
    };

    /**
     *  @brief Cuts the walk of @p read from @p from towards @p bound, inclusive, in @p direction
     *      into at most @p starts.size() tasks of about equal entries and value bytes, covering
     *      about @p wanted entries, from the key slots of its runs alone, with no read.
     *  @param[out] starts The key each task starts at, inclusive, in walk order, the first
     *      being @p from.
     *  @param[out] entries The entries every run holds inside each task, which bound the
     *      keys it finds.
     *  @param[out] bytes The value bytes those entries take, which bound the bytes it reads.
     *  @return The tasks cut, and the key the walk after them starts at, unless they
     *      reach @p bound.
     *
     *  Samples of every run's next @p wanted entries, each the key right past a stride of them,
     *  sorted by key, place the cuts and the stop, and the entries and bytes of each task are
     *  then counted exactly from the runs' positions at the cuts.
     */
    static expected<split_t> split(typename nvme_store_t::reading_t const& read, ustore_arena_t scratch,
                                   stored_key_t from, stored_key_t bound, scan_direction_t direction,
                                   std::uint64_t wanted, std::span<stored_key_t> starts,
                                   std::span<std::uint64_t> entries, std::span<std::uint64_t> bytes) noexcept {
        bool const descending = direction == scan_direction_t::descending_k;
        auto const before = [descending](stored_key_t const& first, stored_key_t const& second) noexcept {
            return descending ? second < first : first < second;
        };
        view_t const& view = *read.view;
        level_set_t const& levels = *view.levels;

        // One window per run: the positions in key order of its next entries in walk order, at
        // most @p wanted
        struct segment_t {
            std::uint32_t level_number = 0;
            std::uint32_t run_index = 0;
            std::uint64_t first = 0;
            std::uint64_t last = 0;
            std::uint64_t window = 0;
        };
        static_assert(sizeof(segment_t) == 32);
        auto const stored_key_of = [&](segment_t const& segment, std::uint64_t position) noexcept -> stored_key_t {
            if (segment.level_number == value_place_t::memory_run_k)
                return view.memory_runs.run_from_oldest(segment.run_index).stored_key(position);
            return levels.level(segment.level_number).run(segment.run_index).key_slot(position).stored_key;
        };
        auto const lower = [&](segment_t const& segment, stored_key_t key) noexcept -> std::uint64_t {
            if (segment.level_number == value_place_t::memory_run_k)
                return view.memory_runs.run_from_oldest(segment.run_index).lower_bound(key);
            return levels.level(segment.level_number).run(segment.run_index).lower_bound(key);
        };
        auto const upper = [&](segment_t const& segment, stored_key_t key) noexcept -> std::uint64_t {
            std::uint64_t position = lower(segment, key);
            std::uint64_t const count =
                segment.level_number == value_place_t::memory_run_k
                    ? view.memory_runs.run_from_oldest(segment.run_index).size()
                    : levels.level(segment.level_number).run(segment.run_index).population_count();
            while (position != count && stored_key_of(segment, position) == key) ++position;
            return position;
        };
        auto const bytes_between = [&](segment_t const& segment, std::uint64_t first,
                                       std::uint64_t last) noexcept -> std::uint64_t {
            if (segment.level_number != value_place_t::memory_run_k)
                return levels.level(segment.level_number).run(segment.run_index).value_bytes_between(first, last);
            std::uint64_t total = 0;
            for (std::uint64_t position = first; position != last; ++position)
                total += view.memory_runs.run_from_oldest(segment.run_index).delta(position).bytes.size();
            return total;
        };
        // The boundary in key order between the keys before @p key in walk order and the rest
        auto const position_of = [&](segment_t const& segment, stored_key_t key) noexcept {
            return descending ? upper(segment, key) : lower(segment, key);
        };
        auto const placed = [&](segment_t segment) noexcept {
            std::uint64_t const begin = descending ? lower(segment, bound) : lower(segment, from);
            std::uint64_t const end = descending ? upper(segment, from) : upper(segment, bound);
            std::uint64_t const window = end > begin ? fu::min_of<std::uint64_t>(end - begin, wanted) : 0;
            segment.first = descending ? end - window : begin;
            segment.last = descending ? end : begin + window;
            segment.window = window;
            return segment;
        };

        st::vector<segment_t, utilities::arena_allocator<segment_t>> segments {{scratch}};
        std::size_t runs = view.memory_runs.runs().size();
        for (std::size_t number = 0; number != levels.size(); ++number) runs += levels.level(number).size();
        if (segments.reserve(runs) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for a window per run"};
        std::uint64_t windows = 0;
        auto const keep = [&](segment_t const& segment) noexcept {
            if (!segment.window) return;
            windows += segment.window;
            [[maybe_unused]] st::status_t const kept = segments.push_back(segment_t {segment});
        };
        for (std::size_t index = 0; index != view.memory_runs.runs().size(); ++index)
            keep(placed(segment_t {value_place_t::memory_run_k, static_cast<std::uint32_t>(index)}));
        for (std::size_t number = 0; number != levels.size(); ++number) {
            level_t const& level = levels.level(number);
            if (level.empty()) continue;
            auto const level_number = static_cast<std::uint32_t>(number);
            if (level.order() != lsm_level_order_t::sorted_k) {
                for (std::size_t index = 0; index != level.size(); ++index)
                    keep(placed(segment_t {level_number, static_cast<std::uint32_t>(index)}));
                continue;
            }
            // The runs of a sorted level are disjoint and ascending, so only the next few hold
            // the walk's entries
            std::size_t index = fu::min_of(level.run_holding_key(from), level.size() - 1);
            if (descending && index != 0 && from < level.run(index).lower_stored_key()) --index;
            for (std::uint64_t covered = 0; covered < wanted;) {
                segment_t const segment = placed(segment_t {level_number, static_cast<std::uint32_t>(index)});
                if (!segment.window) break;
                keep(segment);
                covered += segment.window;
                if (descending ? index == 0 : index + 1 == level.size()) break;
                descending ? --index : ++index;
            }
        }

        // Each window gives samples in proportion to its size, each the key right past a stride
        // of entries, or @p bound where the run's range ends inside the stride
        struct sample_t {
            stored_key_t key {};
            std::uint64_t entries = 0;
            std::uint64_t bytes = 0;
        };
        std::size_t const tasks_capacity = starts.size();
        std::uint64_t const samples_wanted = 8 * tasks_capacity;
        st::vector<sample_t, utilities::arena_allocator<sample_t>> samples {{scratch}};
        if (samples.reserve(samples_wanted + segments.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A split could not hold its samples"};
        for (segment_t const& segment : segments) {
            std::uint64_t const range_end = descending ? lower(segment, bound) : upper(segment, bound);
            std::uint64_t const count = fu::min_of<std::uint64_t>(
                segment.window,
                fu::max_of<std::uint64_t>(1, divide_round_up(samples_wanted * segment.window, windows)));
            std::uint64_t const stride = segment.window / count;
            for (std::uint64_t sample = 1, previous = 0; sample <= count; ++sample) {
                std::uint64_t const covered = sample == count ? segment.window : sample * stride;
                std::uint64_t const first = descending ? segment.last - covered : segment.first + previous;
                std::uint64_t const last = descending ? segment.last - previous : segment.first + covered;
                bool const followed = descending ? first > range_end : last < range_end;
                stored_key_t const key = followed ? stored_key_of(segment, descending ? first - 1 : last) : bound;
                if (samples.push_back(sample_t {key, covered - previous, bytes_between(segment, first, last)}) !=
                    st::status_t::success_k)
                    return status_t {status_code_t::out_of_memory_k, "A split could not hold its samples"};
                previous = covered;
            }
        }
        std::sort(samples.begin(), samples.end(), [&](sample_t const& first, sample_t const& second) noexcept {
            return before(first.key, second.key);
        });

        // The walk stops at the first sample whose strides reach @p wanted entries, past them all
        split_t split;
        std::size_t covered_samples = samples.size();
        for (std::uint64_t index = 0, total = 0; index != samples.size(); ++index) {
            total += samples[index].entries;
            if (total < wanted || !before(samples[index].key, bound)) continue;
            split.stop = samples[index].key;
            split.end = split_end_t::stops_k;
            covered_samples = index + 1;
            break;
        }

        // Cuts fall at equal shares of entries and bytes, entries weighed at the mean bytes of one
        std::uint64_t total_entries = 0;
        std::uint64_t total_bytes = 0;
        for (std::size_t index = 0; index != covered_samples; ++index)
            total_entries += samples[index].entries, total_bytes += samples[index].bytes;
        std::uint64_t const entry_weight = fu::max_of<std::uint64_t>(
            1, total_bytes / fu::max_of<std::uint64_t>(1, total_entries));
        std::uint64_t const total_weight = total_entries * entry_weight + total_bytes;
        starts[0] = from;
        split.tasks = 1;
        for (std::size_t index = 0, weight = 0; index != covered_samples && split.tasks != tasks_capacity; ++index) {
            weight += samples[index].entries * entry_weight + samples[index].bytes;
            if (weight * tasks_capacity < split.tasks * total_weight) continue;
            if (!before(starts[split.tasks - 1], samples[index].key)) continue;
            if (split.end == split_end_t::stops_k && !before(samples[index].key, split.stop)) continue;
            starts[split.tasks++] = samples[index].key;
        }

        for (std::size_t task = 0; task != split.tasks; ++task) entries[task] = 0, bytes[task] = 0;
        for (segment_t const& segment : segments) {
            std::uint64_t at = position_of(segment, from);
            for (std::size_t task = 0; task != split.tasks; ++task) {
                std::uint64_t const next = task + 1 != split.tasks             ? position_of(segment, starts[task + 1])
                                           : split.end == split_end_t::stops_k ? position_of(segment, split.stop)
                                           : descending                        ? lower(segment, bound)
                                                                               : upper(segment, bound);
                std::uint64_t const first = fu::min_of(at, next);
                std::uint64_t const last = fu::max_of(at, next);
                entries[task] += last - first;
                bytes[task] += bytes_between(segment, first, last);
                at = next;
            }
        }
        return split;
    }

#pragma endregion Split

#pragma region Walk

    /** Whether every key has been visited. */
    [[nodiscard]] bool reached_end() const noexcept { return at_ == page().size() && filled_ <= page_keys_k; }

    /** The key the scan sits on, meaningless once the walk has ended. */
    [[nodiscard]] stored_key_t stored_key() const noexcept { return page_[at_].stored_key; }

    /** Whether the key the scan sits on holds a value or a removal, @c missing_k once the
     *  walk has ended. */
    [[nodiscard]] lookup_result_t state() const noexcept {
        return reached_end() ? lookup_result_t::missing_k : page_[at_].state;
    }

    /** Bytes the value of that key takes, which is zero for a removal and for an empty value. */
    [[nodiscard]] stored_value_size_t size() const noexcept { return reached_end() ? 0 : page_[at_].size; }

    /** Where the value of that key lies, meaningful while it holds one. */
    [[nodiscard]] value_place_t place() const noexcept { return places_[at_]; }

    /** The keys of the page the scan sits on, every one of which comes before the keys of
     *  the next page. */
    [[nodiscard]] std::span<entry_t const> page() const noexcept {
        return std::span<entry_t const> {page_.data(), fu::min_of(filled_, page_keys_k)};
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
        // A walk given a reading keeps its cursors, which have passed the key this page starts at.
        if (view_) {
            page_[0] = page_[page_keys_k];
            places_[0] = places_[page_keys_k];
            read_page_(1);
            return {};
        }
        return fill_(from);
    }

#pragma endregion Walk

  private:

#pragma region Private Types

    /** One reader inside the scanned collection's epochs, announced while a page is read. */
    using reader_t = typename nvme_store_t::published_runs_t::reader_t;

    /** The levels of the view the walk reads. */
    using level_set_t = typename nvme_store_t::level_set_t;

    /** One level of those. */
    using level_t = typename nvme_store_t::tree_t::level_t;

    /** One run of one level. */
    using sorted_run_t = typename nvme_store_t::sorted_run_t;

    /** Where the walk has got to in one run of level zero, or in one sorted level, whose runs it
     *  walks one after another as they are disjoint and ascending. */
    struct level_cursor_t {

        /** The level the runs belong to, zero being the one flushes land on. */
        std::uint32_t level_number = 0;

        /** The run inside that level the cursor sits in, older first where the runs overlap. */
        std::uint32_t run_index = 0;

        /** The first run the cursor may step down to: its own run on level zero, the level's first
         *  on a sorted level. */
        std::uint32_t runs_begin = 0;

        /** One past the last run the cursor may step up to: its own run plus one on level zero, the
         *  level's size on a sorted level. */
        std::uint32_t runs_end = 0;

        /** The next sorted position inside that run an ascending walk visits, or one past the last
         *  position a descending walk is yet to visit. */
        std::uint64_t position = 0;

        /** Key slots that run holds. */
        std::uint64_t population = 0;
    };

#pragma endregion Private Types

#pragma region Private Methods

    explicit nvme_store_cursor(ustore_arena_t scratch) noexcept : memory_positions_ {{scratch}}, cursors_ {{scratch}} {}

    /** Places the cursors on @p from inside the ledger's epochs, in the view published now, and
     *  reads the page from there. */
    status_t fill_(stored_key_t from) noexcept {
        reader_t const inside = source_->runs_.enter(thread_index_);
        // Every version at or below the pinned stamp stays in whichever view a
        // merge publishes later.
        view_ = source_->runs_.view();
        status_t const positioned = position_(from);
        if (positioned.succeeded()) read_page_(0);
        view_ = nullptr;
        return positioned;
    }

    /** Reads keys from where the cursors sit into the page behind the first @p kept entries, with
     *  one key past the page beside it, which the next page starts at. */
    void read_page_(std::size_t kept) noexcept {
        at_ = 0;
        filled_ = kept;
        bool const descending = direction_ == scan_direction_t::descending_k;
        while (filled_ != page_.size() && (descending ? next_down_(page_[filled_]) : next_(page_[filled_]))) ++filled_;
    }

    /** Places a cursor on the first key not below @p from, or past the last key not above it in a
     *  descending walk, in every memory run, every run of level zero and every sorted level. */
    status_t position_(stored_key_t from) noexcept {
        bool const descending = direction_ == scan_direction_t::descending_k;
        std::span<memory_run_t const* const> const memory_runs = view_->memory_runs.runs();
        if (memory_positions_.resize(memory_runs.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for a cursor per memory run"};
        for (std::size_t index = 0; index != memory_runs.size(); ++index) {
            memory_run_t const& run = *memory_runs[index];
            std::size_t position = run.lower_bound(from);
            // A descending walk starts past every version of the start key itself.
            while (descending && position != run.size() && run.stored_key(position) == from) ++position;
            memory_positions_[index] = position;
        }

        level_set_t const& levels = *view_->levels;
        std::size_t cursors = 0;
        for (std::size_t number = 0; number != levels.size(); ++number) {
            level_t const& level = levels.level(number);
            cursors += level.order() == lsm_level_order_t::sorted_k ? !level.empty() : level.size();
        }
        cursors_.clear();
        if (cursors_.reserve(cursors) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for a cursor per level"};
        for (std::size_t number = 0; number != levels.size(); ++number) {
            level_t const& level = levels.level(number);
            if (level.empty()) continue;
            std::uint32_t const level_number = static_cast<std::uint32_t>(number);
            std::uint32_t const runs = static_cast<std::uint32_t>(level.size());
            if (level.order() == lsm_level_order_t::sorted_k) {
                std::size_t const holding = fu::min_of(level.run_holding_key(from), level.size() - 1);
                place_(level_cursor_t {level_number, static_cast<std::uint32_t>(holding), 0, runs}, from);
                continue;
            }
            for (std::uint32_t index = 0; index != runs; ++index)
                place_(level_cursor_t {level_number, index, index, index + 1}, from);
        }
        return {};
    }

    /** Places @p cursor inside its run on @p from as @c position_ does, steps it past the runs of
     *  its level it has spent, and keeps it. */
    void place_(level_cursor_t cursor, stored_key_t from) noexcept {
        sorted_run_t const& run = run_of_(cursor);
        cursor.population = run.population_count();
        cursor.position = run.lower_bound(from);
        if (direction_ == scan_direction_t::descending_k) {
            while (cursor.position != cursor.population && run.key_slot(cursor.position).stored_key == from)
                ++cursor.position;
            step_down_(cursor);
        }
        else step_up_(cursor);
        [[maybe_unused]] st::status_t const placed = cursors_.push_back(std::move(cursor));
    }

    /** Moves @p cursor, once spent inside its run, onto the first key of the next run of its level
     *  holding any. */
    void step_up_(level_cursor_t& cursor) const noexcept {
        while (cursor.position == cursor.population && cursor.run_index + 1 != cursor.runs_end) {
            ++cursor.run_index;
            cursor.position = 0;
            cursor.population = run_of_(cursor).population_count();
        }
    }

    /** Moves @p cursor, once a descending walk spent it inside its run, past the last key of the
     *  run before it in its level holding any. */
    void step_down_(level_cursor_t& cursor) const noexcept {
        while (cursor.position == 0 && cursor.run_index != cursor.runs_begin) {
            --cursor.run_index;
            cursor.population = run_of_(cursor).population_count();
            cursor.position = cursor.population;
        }
    }

    /** Moves the cursors past the next key, taking its newest version at or below the scan's stamp
     *  into @p entry and skipping the versions behind it and any key whose every version is newer;
     *  answers whether a key was left. */
    bool next_(entry_t& entry) noexcept {
        std::span<memory_run_t const* const> const memory_runs = view_->memory_runs.runs();
        for (;;) {
            stored_key_t smallest = limit<stored_key_t>::max_k;
            bool any = false;
            for (std::size_t index = 0; index != memory_runs.size(); ++index) {
                if (memory_positions_[index] == memory_runs[index]->size()) continue;
                stored_key_t const head = memory_runs[index]->stored_key(memory_positions_[index]);
                if (any && !(head < smallest)) continue;
                smallest = head;
                any = true;
            }
            for (level_cursor_t const& cursor : cursors_) {
                if (cursor.position == cursor.population) continue;
                stored_key_t const head = key_slot_at_(cursor).stored_key;
                if (any && !(head < smallest)) continue;
                smallest = head;
                any = true;
            }
            if (!any) return false;

            entry.stored_key = smallest;
            bool const taken = take_newest_memory_run_(entry) || take_youngest_run_(entry);

            // Every run holds the versions of a key side by side, and one run of a sorted level
            // holds them all, so each cursor passes all of them at once.
            for (std::size_t index = 0; index != memory_runs.size(); ++index)
                while (memory_positions_[index] != memory_runs[index]->size() &&
                       memory_runs[index]->stored_key(memory_positions_[index]) == smallest)
                    ++memory_positions_[index];
            for (level_cursor_t& cursor : cursors_) {
                while (cursor.position != cursor.population && key_slot_at_(cursor).stored_key == smallest)
                    ++cursor.position;
                step_up_(cursor);
            }
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
            stored_key_t largest = limit<stored_key_t>::min_k;
            bool any = false;
            for (std::size_t index = 0; index != memory_runs.size(); ++index) {
                if (memory_positions_[index] == 0) continue;
                stored_key_t const tail = memory_runs[index]->stored_key(memory_positions_[index] - 1);
                if (any && !(largest < tail)) continue;
                largest = tail;
                any = true;
            }
            for (level_cursor_t const& cursor : cursors_) {
                if (cursor.position == 0) continue;
                stored_key_t const tail = key_slot_below_(cursor).stored_key;
                if (any && !(largest < tail)) continue;
                largest = tail;
                any = true;
            }
            if (!any) return false;

            // A cursor not on the key sits past it, where the takes find no version of it.
            for (std::size_t index = 0; index != memory_runs.size(); ++index) {
                memory_run_t const& run = *memory_runs[index];
                std::size_t& position = memory_positions_[index];
                while (position != 0 && run.stored_key(position - 1) == largest) --position;
            }
            for (level_cursor_t& cursor : cursors_)
                while (cursor.position != 0 && key_slot_below_(cursor).stored_key == largest) --cursor.position;
            entry.stored_key = largest;
            bool const taken = take_newest_memory_run_(entry) || take_youngest_run_(entry);
            for (level_cursor_t& cursor : cursors_) step_down_(cursor);
            if (taken) return true;
        }
    }

    /** The run @p cursor sits in. */
    [[nodiscard]] sorted_run_t const& run_of_(level_cursor_t const& cursor) const noexcept {
        return view_->levels->level(cursor.level_number).run(cursor.run_index);
    }

    /** The key slot right below where @p cursor sits, which must not be at its run's start. */
    [[nodiscard]] key_slot_t key_slot_below_(level_cursor_t const& cursor) const noexcept {
        return run_of_(cursor).key_slot(cursor.position - 1);
    }

    /** The key slot @p cursor sits on, which must not be spent. */
    [[nodiscard]] key_slot_t key_slot_at_(level_cursor_t const& cursor) const noexcept {
        return run_of_(cursor).key_slot(cursor.position);
    }

    /** Takes the newest memory run holding a version of @p entry's key at or below the scan's
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
            take_(hidden ? delta_kind_t::remove_range_k : delta.kind, delta.bytes.size(), entry,
                  value_place_t {value_place_t::memory_run_k,
                                 static_cast<std::uint32_t>(memory_runs.size() - 1 - index), position});
            return true;
        }
        return false;
    }

    /** Takes the newest version of @p entry's key at or below the scan's stamp, which does
     *  @p kind and holds @p size bytes at @p place, as its answer, folding the versions below a
     *  change into it. */
    void take_(delta_kind_t kind, std::size_t size, entry_t& entry, value_place_t place) noexcept {
        entry.size = 0;
        places_[filled_] = place;
        if (kind == delta_kind_t::change_k) {
            places_[filled_].level_number = value_place_t::folded_k;
            entry.state = source_->find_folded_(reading_(), entry.stored_key, entry.size);
        }
        else if (utilities::removes(kind)) entry.state = lookup_result_t::removed_k;
        else {
            entry.state = lookup_result_t::written_k;
            entry.size = static_cast<stored_value_size_t>(size);
        }
    }

    /**
     *  @brief Takes the shallowest and, inside a level, the youngest run holding a version
     *      of @p entry's key at or below the scan's stamp as its answer, the newest such
     *      version inside it.
     *  @return Whether any run holds one.
     */
    bool take_youngest_run_(entry_t& entry) noexcept {
        std::size_t chosen = cursors_.size();
        std::uint64_t chosen_position = 0;
        for (std::size_t index = 0; index != cursors_.size(); ++index) {
            level_cursor_t const& cursor = cursors_[index];
            if (chosen != cursors_.size() && !outranks_(cursor, cursors_[chosen])) continue;
            sorted_run_t const& run = run_of_(cursor);
            std::uint64_t position = cursor.position;
            while (position != cursor.population && run.key_slot(position).stored_key == entry.stored_key &&
                   run.stamp(position) > stamp_)
                ++position;
            if (position == cursor.population || !(run.key_slot(position).stored_key == entry.stored_key)) continue;
            chosen = index;
            chosen_position = position;
        }
        if (chosen == cursors_.size()) return false;
        level_cursor_t const& cursor = cursors_[chosen];
        sorted_run_t const& run = run_of_(cursor);
        lsm_tree_position_t const at {cursor.level_number, lsm_level_position_t {cursor.run_index, chosen_position}};
        bool const hidden = nvme_store_t::hidden_in_levels_(reading_(), entry.stored_key, at);
        take_(hidden ? delta_kind_t::remove_range_k : run.kind(chosen_position),
              run.value_window(chosen_position).bytes, entry,
              value_place_t {cursor.level_number, cursor.run_index, chosen_position});
        return true;
    }

    /** The view the page being read walks, at the scan's stamp. */
    [[nodiscard]] typename nvme_store_t::reading_t reading_() const noexcept {
        return typename nvme_store_t::reading_t {view_, stamp_};
    }

    /** Whether @p first holds a newer version of a key than @p second does. */
    [[nodiscard]] static bool outranks_(level_cursor_t const& first, level_cursor_t const& second) noexcept {
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
    std::size_t thread_index_ = 0;

    /** The view the page being read walks, null between pages, when no reader keeps it alive, or
     *  the view the caller's reading names for every page. */
    view_t const* view_ = nullptr;

    /** Per memory run of the view, newest first, the next sorted position an ascending walk visits,
     *  or one past the last position a descending walk is yet to visit, keeping its room from page
     *  to page, as the level cursors do. */
    st::vector<std::size_t, utilities::arena_allocator<std::size_t>> memory_positions_;

    /** The order the walk hands its keys out in. */
    scan_direction_t direction_ = scan_direction_t::ascending_k;

    /** One cursor per run of level zero and one per sorted level the view names. */
    st::vector<level_cursor_t, utilities::arena_allocator<level_cursor_t>> cursors_;

    /** The keys of the page, then the key the next page starts at. */
    std::array<entry_t, page_keys_k + 1> page_ {};

    /** Where the value of every entry of @c page_ lies. */
    std::array<value_place_t, page_keys_k + 1> places_ {};

    /** Entries the page read, a page and one more while another page follows. */
    std::size_t filled_ = 0;

    /** The entry of the page the scan sits on. */
    std::size_t at_ = 0;

#pragma endregion Private Members
};

#pragma endregion Cursor

#pragma endregion Store Cursor

} // namespace unum::ustore::blobs
