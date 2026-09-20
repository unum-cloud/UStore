/**
 *  @file src/nvme/lsm_level.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief One level of an LSM tree: the sorted runs it holds, and the order a lookup and a scan read them in.
 *
 *  Level zero takes one run per flushed write buffer, so its runs overlap and only their age tells which
 *  version of a key is current: a lookup walks them newest first and a scan merges them by smallest key.
 *  Every deeper level is written by a merge, so its runs are disjoint and ascending and a lookup is a
 *  binary search over their bounds. Both shapes carry tombstones; only the bottom level may forget one,
 *  which is a property of the merge that writes it rather than of the level that holds it.
 *
 *  A level holds its runs through pointers that never move, so the level set a compaction publishes can
 *  name the same runs the set it replaces named. Ownership follows one rule and no other: a level frees
 *  its runs unless it has been told to @c relinquish them, which only a level set being retired is.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`

#include <new>     // `std::nothrow`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`

#include "nvme/io_channel.hpp"     // `io_channel_t`
#include "nvme/lsm_sorted_run.hpp" // `lsm_sorted_run`, `find_result_t`, `value_window_t`
#include "nvme/types.hpp"          // `value_layout_t`, `is_inline`, `limit`

namespace unum::ustore::nvme {

#pragma region Vocabulary

/** Whether a level's runs may overlap, which decides how a lookup and a scan order them. */
enum class lsm_level_order_t : std::uint8_t {

    /** Runs written by flushes, which overlap, so the youngest run holding a key carries its value. */
    overlapping_k,

    /** Runs written by a merge, which are disjoint and ascending, so at most one run holds a key. */
    sorted_k,
};

/** Whether anything is stored below a level, which decides whether a merge into it may drop tombstones. */
enum class lsm_level_depth_t : std::uint8_t {

    /** Levels sit below this one, so a tombstone still shadows what they hold. */
    intermediate_k,

    /** Nothing sits below this one, so a tombstone shadows nothing and is dropped for good. */
    bottom_k,
};

/** Where one key sits inside a level, which is the run that holds it and its place in that run. */
struct lsm_level_position_t {

    /** The run of the level that holds the key. */
    std::uint32_t run_index = 0;

    /** The sorted position of the key inside that run. */
    std::uint64_t key_slot_position = 0;
};

#pragma endregion Vocabulary

#pragma region Level

/**
 *  @brief The sorted runs of one level, answering lookups and scans over all of them at once.
 *
 *  Every run is one heap allocation the level owns, so a successor level set copies the pointer rather
 *  than the index behind it and a reader of either set reads the same bytes.
 */
template <typename key_slot_type_, typename volume_type_>
class lsm_level {
  public:

#pragma region Types

    /** The key slot the runs of this level hold. */
    using key_slot_t = key_slot_type_;

    /** The volume the runs of this level live on. */
    using volume_t = volume_type_;

    /** One immutable run of this level. */
    using sorted_run_t = lsm_sorted_run<key_slot_t, volume_t>;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** How the values of this level are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so reading one needs no I/O. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

#pragma endregion Types

#pragma region Construction

    lsm_level() noexcept = default;
    lsm_level(lsm_level const&) = delete;
    lsm_level& operator=(lsm_level const&) = delete;

    lsm_level(lsm_level&& other) noexcept
        : runs_(std::move(other.runs_)), number_(other.number_), order_(other.order_),
          size_bytes_(std::exchange(other.size_bytes_, 0)),
          population_count_(std::exchange(other.population_count_, 0)),
          lower_(std::exchange(other.lower_, limit<stored_key_t>::max())),
          upper_(std::exchange(other.upper_, limit<stored_key_t>::min())) {}

    lsm_level& operator=(lsm_level&& other) noexcept {
        if (this == &other) return *this;
        discard();
        runs_ = std::move(other.runs_);
        number_ = other.number_;
        order_ = other.order_;
        size_bytes_ = std::exchange(other.size_bytes_, 0);
        population_count_ = std::exchange(other.population_count_, 0);
        lower_ = std::exchange(other.lower_, limit<stored_key_t>::max());
        upper_ = std::exchange(other.upper_, limit<stored_key_t>::min());
        return *this;
    }

    ~lsm_level() noexcept { discard(); }

    /** An empty level numbered @p number whose runs are read in the order @p order names. */
    [[nodiscard]] static lsm_level of(std::uint32_t number, lsm_level_order_t order) noexcept {
        lsm_level level;
        level.number_ = number;
        level.order_ = order;
        return level;
    }

    /** Takes @p run into the level, at the back for an overlapping level and in key order for a sorted one. */
    status_t bind_queue_pair(sorted_run_t&& run) noexcept {
        if (runs_.reserve(runs_.size() + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not hold another run"};
        std::size_t position = runs_.size();
        if (order_ == lsm_level_order_t::sorted_k) {
            position = 0;
            while (position != runs_.size() && runs_[position]->upper_stored_key() < run.lower_stored_key()) ++position;
            if (position != runs_.size() && !(run.upper_stored_key() < runs_[position]->lower_stored_key()))
                return {status_code_t::invalid_argument_k, "A sorted level takes runs that overlap no other"};
        }
        sorted_run_t* held = new (std::nothrow) sorted_run_t {std::move(run)};
        if (held == nullptr) return {status_code_t::out_of_memory_k, "A level could not hold another run"};
        note_(*held);
        [[maybe_unused]] st::status_t const placed = runs_.insert(position, std::move(held));
        return {};
    }

    /** Names every run @p other names, which is how a staged level set carries an untouched level. */
    status_t carry(lsm_level const& other) noexcept {
        if (runs_.reserve(runs_.size() + other.runs_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not name that many runs"};
        for (std::size_t index = 0; index != other.runs_.size(); ++index) {
            sorted_run_t* named = other.runs_[index];
            note_(*named);
            [[maybe_unused]] st::status_t const placed = runs_.push_back(std::move(named));
        }
        return {};
    }

    /** Makes room for @p count more runs, so a merge that plans them all fails before it writes any. */
    status_t reserve(std::size_t count) noexcept {
        if (runs_.reserve(runs_.size() + count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not hold that many runs"};
        return {};
    }

    /** Forgets every run without freeing it, which only a level set handing its runs on may do. */
    void relinquish() noexcept {
        runs_.clear();
        forget_();
    }

    /** Frees every run's memory without touching the medium, which the destructor does anyway. */
    void discard() noexcept {
        for (std::size_t index = 0; index != runs_.size(); ++index) delete runs_[index];
        runs_.clear();
        forget_();
    }

    /** Returns every run's bytes to @p volume and then frees them, which destroys the level's data. */
    status_t drop(volume_t& volume, io_channel_t& channel) noexcept {
        status_t first;
        for (std::size_t index = 0; index != runs_.size(); ++index)
            if (status_t const dropped = sorted_run_t::drop(volume, channel, runs_[index]->content_id());
                dropped.failed() && first.succeeded())
                first = dropped;
        discard();
        return first;
    }

#pragma endregion Construction

#pragma region Shape

    /** Where this level sits in its tree, zero being the level flushes land on. */
    [[nodiscard]] std::uint32_t number() const noexcept { return number_; }

    /** Whether this level's runs may overlap. */
    [[nodiscard]] lsm_level_order_t order() const noexcept { return order_; }

    /** Runs the level holds. */
    [[nodiscard]] std::size_t size() const noexcept { return runs_.size(); }

    /** Whether the level holds no run at all. */
    [[nodiscard]] bool empty() const noexcept { return runs_.empty(); }

    /** Key slots every run of the level holds together, one key counted once per run that holds it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept { return population_count_; }

    /** Bytes every run of the level takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return size_bytes_; }

    /** The least key any run of the level holds, or the greatest key of the type for an empty level. */
    [[nodiscard]] stored_key_t lower_stored_key() const noexcept { return lower_; }

    /** The greatest key any run of the level holds, or the least key of the type for an empty level. */
    [[nodiscard]] stored_key_t upper_stored_key() const noexcept { return upper_; }

    /** The run at @p index, which for an overlapping level is ordered oldest first. */
    [[nodiscard]] sorted_run_t const& run(std::size_t index) const noexcept { return *runs_[index]; }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether the level carries @p stored_key, and whether the key slot holding it locates a value.
     *  @param[in] stored_key The key to look for.
     *  @param[out] position Where the key sits, which is meaningless when the answer is @c nothing_k.
     */
    [[nodiscard]] find_result_t find(stored_key_t stored_key, lsm_level_position_t& position) const noexcept {
        position = lsm_level_position_t {};
        if (order_ == lsm_level_order_t::overlapping_k) {
            for (std::size_t index = runs_.size(); index-- != 0;) {
                sorted_run_t const& run = *runs_[index];
                if (run.population_count() == 0) continue;
                if (stored_key < run.lower_stored_key() || run.upper_stored_key() < stored_key) continue;
                std::uint64_t at = 0;
                find_result_t const found = run.find(stored_key, at);
                if (found == find_result_t::nothing_k) continue;
                position = lsm_level_position_t {static_cast<std::uint32_t>(index), at};
                return found;
            }
            return find_result_t::nothing_k;
        }

        std::size_t low = 0;
        std::size_t high = runs_.size();
        while (low != high) {
            std::size_t const middle = low + (high - low) / 2;
            sorted_run_t const& run = *runs_[middle];
            if (run.upper_stored_key() < stored_key) {
                low = middle + 1;
                continue;
            }
            if (stored_key < run.lower_stored_key()) {
                high = middle;
                continue;
            }
            std::uint64_t at = 0;
            find_result_t const found = run.find(stored_key, at);
            if (found != find_result_t::nothing_k)
                position = lsm_level_position_t {static_cast<std::uint32_t>(middle), at};
            return found;
        }
        return find_result_t::nothing_k;
    }

    /** The key slot at @p position, which must have come from a @c find that answered anything but nothing. */
    [[nodiscard]] key_slot_t const& key_slot(lsm_level_position_t position) const noexcept {
        return runs_[position.run_index]->key_slot(position.key_slot_position);
    }

    /** Where the value of the key slot at @p position sits, empty for one that needs no read. */
    [[nodiscard]] value_window_t value_window(lsm_level_position_t position) const noexcept {
        return runs_[position.run_index]->value_window(position.key_slot_position);
    }

    /**
     *  @brief Calls @p visitor with the position and the key slot of every key, in ascending order of keys.
     *
     *  @note An overlapping level reports one position per key, the youngest run's, and skips the versions
     *      the older runs still carry; a sorted level walks its runs one after another.
     */
    template <typename visitor_type_>
    status_t for_each_key_slot(visitor_type_&& visitor) const noexcept {
        if (order_ == lsm_level_order_t::sorted_k) {
            for (std::size_t index = 0; index != runs_.size(); ++index)
                runs_[index]->for_each_key_slot([&](std::uint64_t at, key_slot_t const& slot) noexcept {
                    visitor(lsm_level_position_t {static_cast<std::uint32_t>(index), at}, slot);
                });
            return {};
        }

        st::basic_vector<std::uint64_t> cursors;
        if (cursors.resize(runs_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not track a cursor per run"};
        for (std::size_t index = 0; index != runs_.size(); ++index) cursors[index] = 0;

        for (;;) {
            std::size_t chosen = runs_.size();
            stored_key_t smallest = limit<stored_key_t>::max();
            for (std::size_t index = 0; index != runs_.size(); ++index) {
                if (cursors[index] == runs_[index]->population_count()) continue;
                stored_key_t const key = runs_[index]->key_slot(cursors[index]).stored_key;
                if (chosen != runs_.size() && smallest < key) continue;
                chosen = index;
                smallest = key;
            }
            if (chosen == runs_.size()) return {};

            visitor(lsm_level_position_t {static_cast<std::uint32_t>(chosen), cursors[chosen]},
                    runs_[chosen]->key_slot(cursors[chosen]));
            for (std::size_t index = 0; index != runs_.size(); ++index)
                if (cursors[index] != runs_[index]->population_count() &&
                    runs_[index]->key_slot(cursors[index]).stored_key == smallest)
                    ++cursors[index];
        }
    }

#pragma endregion Lookups

  private:

#pragma region Private Methods

    /** Folds @p run into the bounds, the byte count and the population the level reports. */
    void note_(sorted_run_t const& run) noexcept {
        size_bytes_ += run.size_bytes();
        population_count_ += run.population_count();
        if (run.lower_stored_key() < lower_) lower_ = run.lower_stored_key();
        if (upper_ < run.upper_stored_key()) upper_ = run.upper_stored_key();
    }

    /** Resets everything the runs contributed, which both ways of emptying a level need. */
    void forget_() noexcept {
        size_bytes_ = 0;
        population_count_ = 0;
        lower_ = limit<stored_key_t>::max();
        upper_ = limit<stored_key_t>::min();
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The runs of the level, oldest first when they overlap and ascending when they do not. */
    st::basic_vector<sorted_run_t*> runs_;

    /** Where this level sits in its tree. */
    std::uint32_t number_ = 0;

    /** Whether the runs may overlap. */
    lsm_level_order_t order_ = lsm_level_order_t::sorted_k;

    /** Bytes every run takes, kept as runs are adopted rather than recounted. */
    std::uint64_t size_bytes_ = 0;

    /** Key slots every run holds, kept as runs are adopted rather than recounted. */
    std::uint64_t population_count_ = 0;

    /** The least key any run holds. */
    stored_key_t lower_ = limit<stored_key_t>::max();

    /** The greatest key any run holds. */
    stored_key_t upper_ = limit<stored_key_t>::min();

#pragma endregion Private Members
};

#pragma endregion Level

} // namespace unum::ustore::nvme
