/**
 *  @file include/ustore/blobs/nvme_lsm_level.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The levels of one LSM tree: record pools naming sorted runs by handle, and the runs of
 *      one level in the order lookups walk them.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`

#include <span>    // `std::span`
#include <utility> // `std::exchange`, `std::move`

#include <smashtable/basic_growing_pool.hpp> // `st::basic_growing_pool`
#include <smashtable/basic_vector.hpp>       // `st::vector`

#include "ustore/blobs/nvme_key_slot.hpp"   // `value_layout_t`
#include "ustore/blobs/nvme_sorted_run.hpp" // `lsm_sorted_run`
#include "ustore/utilities/ram_buffers.hpp" // `constrained_allocator`, `resource_constraint`
#include "ustore/utilities/types.hpp"       // `status_t`, `expected`

namespace unum::ustore::blobs {

using utilities::constrained_allocator;
using utilities::resource_constraint;

#pragma region LSM Level

#pragma region Vocabulary

/** Whether a level's runs may overlap, which decides how a lookup and a scan order them. */
enum class lsm_level_order_t : std::uint8_t {

    /** Runs written by flushes, which overlap, so the youngest run holding a key
     *  carries its value. */
    overlapping_k,

    /** Runs written by a merge, which are disjoint and ascending, so at most one run
     *  holds a key. */
    sorted_k,
};

/** Whether anything is stored below a level, which decides whether a merge into it may
 *  drop remove deltas. */
enum class lsm_level_depth_t : std::uint8_t {

    /** Levels sit below this one, so a remove delta still shadows what they hold. */
    intermediate_k,

    /** Nothing sits below this one, so a remove delta shadows nothing and is dropped for good. */
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

#pragma region Record Pools

/**
 *  @brief Records of one kind that never move, taken and vacated by the threads of one
 *      store or database.
 *
 *  A holder names a record by a handle, its index and the generation it was taken at. A record
 *  lives from @c take until @c vacate, which steps the generation, so a handle kept past that
 *  resolves to nothing. Takes and vacates from several threads go through one claim word; resolving
 *  a handle reads only its record, which its holder keeps alive. Every byte of the records is
 *  charged to the budget the pool was made with.
 *
 *  @tparam record_type_ What a record holds, whose move assignment carries its payload and
 *      keeps its generation and any list links, so a taken record is filled and a vacated one
 *      emptied in place.
 *  @tparam index_type_ Names a record, and is the width of its generation.
 *  @tparam atomic_reference_ The reference the claim word and the budget's word are owned through.
 *  @tparam waiter_type_ What the core does while another thread holds the claim.
 */
template <typename record_type_, std::unsigned_integral index_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
class lsm_record_pool {
  public:

#pragma region Types

    /** One record of the pool. */
    using record_t = record_type_;

    /** Names a record. */
    using index_t = index_type_;

    /** The budget the records are charged to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    /** The records and the stack of the free ones, in chunks charged to the budget, which a reader
     *  resolves without the claim while a take grows them. */
    using records_t = st::basic_growing_pool<record_t, index_t, constrained_allocator<std::byte, atomic_reference_>>;

    /** A record's index and the generation it was taken at. */
    using handle_t = typename records_t::handle_t;

    /** What @c take answers when every record is taken. */
    static constexpr index_t absent_k = records_t::absent_k;

#pragma endregion Types

#pragma region Construction

    lsm_record_pool() noexcept = default;
    lsm_record_pool(lsm_record_pool const&) = delete;
    lsm_record_pool& operator=(lsm_record_pool const&) = delete;
    lsm_record_pool(lsm_record_pool&&) noexcept = default;
    lsm_record_pool& operator=(lsm_record_pool&&) noexcept = default;

    /** A pool of @p capacity free records charged to @p constraint, which must outlive it. */
    static expected<lsm_record_pool> make(std::size_t capacity, constraint_t& constraint) noexcept {
        using allocator_t = constrained_allocator<std::byte, atomic_reference_>;
        return adopt_(records_t::make(capacity, capacity, allocator_t {constraint}));
    }

    /** A pool of @p capacity free records that grows by chunks up to @p capacity_limit when a
     *  @c take finds none, charged to @p constraint, which must outlive it. */
    static expected<lsm_record_pool> make(std::size_t capacity, std::size_t capacity_limit,
                                          constraint_t& constraint) noexcept {
        using allocator_t = constrained_allocator<std::byte, atomic_reference_>;
        return adopt_(records_t::make(capacity, capacity_limit, allocator_t {constraint}));
    }

#pragma endregion Construction

#pragma region Records

    /** Moves @p record into a free record and answers its handle, leaving @p record untouched when
     *  every record is taken. */
    expected<handle_t> take(record_t&& record) noexcept {
        lock_();
        index_t const index = records_.take();
        unlock_();
        if (index == absent_k) return status_t {status_code_t::capacity_exhausted_k, "Every record of a pool is taken"};
        records_[index] = std::move(record);
        return records_.handle_of(index);
    }

    /** Records taken. */
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

    /** Records the pool holds, taken or free. */
    [[nodiscard]] std::size_t capacity() const noexcept { return records_.capacity(); }

    /** The record @p record names, or null once it was vacated. */
    [[nodiscard]] record_t* at(handle_t record) noexcept { return records_.at(record); }

    /** The record @p record names, or null once it was vacated. */
    [[nodiscard]] record_t const* at(handle_t record) const noexcept { return records_.at(record); }

    /** The record at @p index, taken or not, which list links reach by index. */
    [[nodiscard]] record_t& operator[](index_t index) noexcept { return records_[index]; }

    /** The record at @p index, taken or not. */
    [[nodiscard]] record_t const& operator[](index_t index) const noexcept { return records_[index]; }

    /** The handle naming @p record, which is a record of this pool. */
    [[nodiscard]] handle_t handle_of(record_t const& record) const noexcept {
        return records_.handle_of(records_.index_of(record));
    }

    /** Empties the record @p record names and frees it, so every copy of the handle
     *  resolves to nothing. */
    void vacate(handle_t record) noexcept {
        record_t* const held = records_.at(record);
        if (held == nullptr) return;
        *held = record_t {};
        lock_();
        records_.vacate(record.index);
        unlock_();
    }

    /** Gives back the extent of every run @p runs names to @p volume and vacates them, for runs
     *  no manifest names. */
    template <typename volume_type_>
    void discard(volume_type_& volume, std::span<handle_t const> runs) noexcept {
        for (handle_t const run : runs)
            if (record_t const* const held = records_.at(run); held != nullptr) {
                volume.deallocate(held->extent());
                vacate(run);
            }
    }

#pragma endregion Records

  private:

#pragma region Private Methods

    /** A pool over the records @p made, or the status SmashTable refused them with. */
    static expected<lsm_record_pool> adopt_(st::expected<records_t>&& made) noexcept {
        if (made.status() == st::status_t::invalid_argument_k)
            return status_t {status_code_t::invalid_argument_k, "A pool holds fewer records than its index names"};
        if (!made) return status_t {status_code_t::out_of_memory_k, "No room for the records of a pool"};
        lsm_record_pool pool;
        pool.records_ = std::move(*made);
        return pool;
    }

    /** Takes the claim word, which guards the free-index stack alone. */
    void lock_() noexcept {
        atomic_reference_<std::int32_t> const word {claim_};
        std::int32_t expected_free = 0;
        while (!word.compare_exchange_weak(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            waiter_type_ {}(&claim_, expected_free, std::size_t {0});
            expected_free = 0;
        }
    }

    /** Lets the claim word go. */
    void unlock_() noexcept { atomic_reference_<std::int32_t> {claim_}.store(0, std::memory_order_release); }

#pragma endregion Private Methods

#pragma region Private Members

    /** The records and the stack of the free ones. */
    records_t records_;

    /** Zero while no thread takes or vacates a record. */
    std::int32_t claim_ = 0;

#pragma endregion Private Members
};

/** Every run the trees of one database and one key slot hold, at most 65,534, which is what a
 *  request token packs. */
template <typename sorted_run_type_, template <typename> typename atomic_reference_, typename waiter_type_>
using lsm_run_pool = lsm_record_pool<sorted_run_type_, std::uint16_t, atomic_reference_, waiter_type_>;

#pragma endregion Record Pools

#pragma region Level

/**
 *  @brief The sorted runs of one level, answering lookups and scans over all of them at once.
 *
 *  A level names its runs by handles into the database's run pool, so a successor level set names
 *  the same records rather than copying the index behind them and a reader of either set reads the
 *  same bytes. The level owns none of them; its tree decides when nothing names a run.
 *
 *  @tparam atomic_reference_ The reference the run pool's words are owned through.
 *  @tparam waiter_type_ What the core does while a run waits for its reads.
 *  @tparam row_kit_type_ The SmashTable kit its runs' trees search with.
 */
template <typename key_slot_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, typename row_kit_type_ = st::native_row_kit_t>
class lsm_level {
  public:

#pragma region Types

    /** The key slot the runs of this level hold. */
    using key_slot_t = key_slot_type_;

    /** The volume the runs of this level live on. */
    using volume_t = volume_type_;

    /** One immutable run of this level. */
    using sorted_run_t = lsm_sorted_run<key_slot_t, volume_t, waiter_type_, row_kit_type_>;

    /** The pool the runs of this level live in. */
    using run_pool_t = lsm_run_pool<sorted_run_t, atomic_reference_, waiter_type_>;

    /** How this level names one of its runs. */
    using run_handle_t = typename run_pool_t::handle_t;

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
        : runs_(std::move(other.runs_)), run_pool_(other.run_pool_), number_(other.number_), order_(other.order_),
          size_bytes_(std::exchange(other.size_bytes_, 0)),
          population_count_(std::exchange(other.population_count_, 0)),
          lower_(std::exchange(other.lower_, limit<stored_key_t>::max())),
          upper_(std::exchange(other.upper_, limit<stored_key_t>::min())),
          range_tombstones_count_(std::exchange(other.range_tombstones_count_, 0)) {}

    lsm_level& operator=(lsm_level&& other) noexcept {
        if (this == &other) return *this;
        runs_ = std::move(other.runs_);
        run_pool_ = other.run_pool_;
        number_ = other.number_;
        order_ = other.order_;
        size_bytes_ = std::exchange(other.size_bytes_, 0);
        population_count_ = std::exchange(other.population_count_, 0);
        lower_ = std::exchange(other.lower_, limit<stored_key_t>::max());
        upper_ = std::exchange(other.upper_, limit<stored_key_t>::min());
        range_tombstones_count_ = std::exchange(other.range_tombstones_count_, 0);
        return *this;
    }

    /** An empty level numbered @p number of runs from @p runs, read in the order @p order names. */
    [[nodiscard]] static lsm_level of(run_pool_t& runs, std::uint32_t number, lsm_level_order_t order) noexcept {
        lsm_level level;
        level.run_pool_ = &runs;
        level.number_ = number;
        level.order_ = order;
        return level;
    }

    /** Names @p run, at the back for an overlapping level and in key order for a sorted one, which
     *  it must not overlap. */
    status_t insert_run(run_handle_t run) noexcept {
        if (runs_.reserve(runs_.size() + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not hold another run"};
        sorted_run_t const& inserted = *run_pool_->at(run);
        std::size_t position = runs_.size();
        if (order_ == lsm_level_order_t::sorted_k) {
            std::size_t low = 0;
            while (low != position) {
                std::size_t const middle = low + (position - low) / 2;
                if (run_at_(middle).precedes(inserted)) low = middle + 1;
                else position = middle;
            }
            if (position != runs_.size() && !inserted.precedes(run_at_(position)))
                return {status_code_t::invalid_argument_k, "A sorted level takes runs that overlap no other"};
        }
        note_(inserted);
        [[maybe_unused]] st::status_t const placed = runs_.insert(position, std::move(run));
        return {};
    }

    /** Names every run @p other names but those @p skipped lists, in the order @p other holds
     *  them, which is how a prepared level set carries a level a merge leaves alone or
     *  consumes in part. */
    status_t carry(lsm_level const& other, std::span<sorted_run_t const* const> skipped) noexcept {
        if (runs_.reserve(runs_.size() + other.runs_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not name that many runs"};
        for (std::size_t index = 0; index != other.runs_.size(); ++index) {
            sorted_run_t const* const named = &other.run_at_(index);
            bool consumed = false;
            for (sorted_run_t const* const picked : skipped) consumed |= picked == named;
            if (consumed) continue;
            note_(*named);
            [[maybe_unused]] st::status_t const placed = runs_.push_back(run_handle_t {other.runs_[index]});
        }
        return {};
    }

    /** Makes room for @p count more runs, so a publication that places them all fails before
     *  it names any. */
    status_t reserve(std::size_t count) noexcept {
        if (runs_.reserve(runs_.size() + count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not hold that many runs"};
        return {};
    }

    /** Forgets every run, which the tree does once it stops counting this level among those
     *  naming them. */
    void clear() noexcept {
        runs_.clear();
        size_bytes_ = 0;
        population_count_ = 0;
        lower_ = limit<stored_key_t>::max();
        upper_ = limit<stored_key_t>::min();
        range_tombstones_count_ = 0;
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

    /** Key slots every run of the level holds together, one key counted once per run
     *  that holds it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept { return population_count_; }

    /** Bytes every run of the level takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return size_bytes_; }

    /** Range tombstones all runs of the level carry; a read skips them when there are none. */
    [[nodiscard]] std::uint64_t range_tombstones_count() const noexcept { return range_tombstones_count_; }

    /** The least key any run of the level holds, or the greatest key of the type for
     *  an empty level. */
    [[nodiscard]] stored_key_t lower_stored_key() const noexcept { return lower_; }

    /** The greatest key any run of the level holds, or the least key of the type for
     *  an empty level. */
    [[nodiscard]] stored_key_t upper_stored_key() const noexcept { return upper_; }

    /** The run at @p index, which for an overlapping level is ordered oldest first. */
    [[nodiscard]] sorted_run_t const& run(std::size_t index) const noexcept { return run_at_(index); }

    /** The handles of the runs, in the order @c run answers them. */
    [[nodiscard]] std::span<run_handle_t const> runs() const noexcept { return std::span<run_handle_t const> {runs_}; }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether the level carries @p stored_key, and whether the key slot holding it
     *      locates a value.
     *  @param[in] stored_key The key to look for.
     *  @param[out] position Where the key sits, which is meaningless when the answer
     *      is @c missing_k.
     */
    [[nodiscard]] lookup_result_t find(stored_key_t stored_key, lsm_level_position_t& position) const noexcept {
        return find(stored_key, limit<std::uint64_t>::max(), position);
    }

    /** As @c find, answering the newest version stamped at or below @p stamp, which a reader
     *  pinned there sees. */
    [[nodiscard]] lookup_result_t find(stored_key_t stored_key, std::uint64_t stamp,
                                       lsm_level_position_t& position) const noexcept {
        position = lsm_level_position_t {};
        if (order_ == lsm_level_order_t::overlapping_k) {
            for (std::size_t index = runs_.size(); index-- != 0;) {
                sorted_run_t const& run = run_at_(index);
                if (run.population_count() == 0) continue;
                if (stored_key < run.lower_stored_key() || run.upper_stored_key() < stored_key) continue;
                std::uint64_t at = 0;
                lookup_result_t const found = run.find(stored_key, stamp, at);
                if (found == lookup_result_t::missing_k) continue;
                position = lsm_level_position_t {static_cast<std::uint32_t>(index), at};
                return found;
            }
            return lookup_result_t::missing_k;
        }

        std::size_t low = 0;
        std::size_t high = runs_.size();
        while (low != high) {
            std::size_t const middle = low + (high - low) / 2;
            sorted_run_t const& run = run_at_(middle);
            if (run.upper_stored_key() < stored_key) {
                low = middle + 1;
                continue;
            }
            if (stored_key < run.lower_stored_key()) {
                high = middle;
                continue;
            }
            std::uint64_t at = 0;
            lookup_result_t const found = run.find(stored_key, stamp, at);
            if (found != lookup_result_t::missing_k)
                position = lsm_level_position_t {static_cast<std::uint32_t>(middle), at};
            return found;
        }
        return lookup_result_t::missing_k;
    }

    /** The key slot at @p position, which must have come from a @c find that answered
     *  anything but missing. */
    [[nodiscard]] key_slot_t key_slot(lsm_level_position_t position) const noexcept {
        return run_at_(position.run_index).key_slot(position.key_slot_position);
    }

    /** Where the value of the key slot at @p position sits, empty for one that needs no read. */
    [[nodiscard]] value_window_t value_window(lsm_level_position_t position) const noexcept {
        return run_at_(position.run_index).value_window(position.key_slot_position);
    }

    /**
     *  @brief Calls @p visitor with the position and the key slot of every key, in ascending
     *      order of keys.
     *
     *  @note An overlapping level reports one position per key, the youngest run's, and skips the
     *      versions the older runs still carry; a sorted level walks its runs one after another.
     */
    template <typename visitor_type_>
    status_t for_each_key_slot(visitor_type_&& visitor) const noexcept {
        if (order_ == lsm_level_order_t::sorted_k) {
            for (std::size_t index = 0; index != runs_.size(); ++index)
                run_at_(index).for_each_key_slot([&](std::uint64_t at, key_slot_t const& slot) noexcept {
                    visitor(lsm_level_position_t {static_cast<std::uint32_t>(index), at}, slot);
                });
            return {};
        }

        st::vector<std::uint64_t> cursors;
        if (cursors.resize(runs_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not track a cursor per run"};
        for (std::size_t index = 0; index != runs_.size(); ++index) cursors[index] = 0;

        for (;;) {
            std::size_t chosen = runs_.size();
            stored_key_t smallest = limit<stored_key_t>::max();
            for (std::size_t index = 0; index != runs_.size(); ++index) {
                if (cursors[index] == run_at_(index).population_count()) continue;
                stored_key_t const key = run_at_(index).key_slot(cursors[index]).stored_key;
                if (chosen != runs_.size() && smallest < key) continue;
                chosen = index;
                smallest = key;
            }
            if (chosen == runs_.size()) return {};

            visitor(lsm_level_position_t {static_cast<std::uint32_t>(chosen), cursors[chosen]},
                    run_at_(chosen).key_slot(cursors[chosen]));
            for (std::size_t index = 0; index != runs_.size(); ++index)
                if (cursors[index] != run_at_(index).population_count() &&
                    run_at_(index).key_slot(cursors[index]).stored_key == smallest)
                    ++cursors[index];
        }
    }

#pragma endregion Lookups

  private:

#pragma region Private Methods

    /** The run at @p index, whose record the level set naming it keeps alive. */
    [[nodiscard]] sorted_run_t const& run_at_(std::size_t index) const noexcept { return *run_pool_->at(runs_[index]); }

    /** Folds @p run into the bounds, the byte count and the population the level reports. */
    void note_(sorted_run_t const& run) noexcept {
        size_bytes_ += run.size_bytes();
        population_count_ += run.population_count();
        range_tombstones_count_ += run.range_tombstones().size();
        if (run.lower_stored_key() < lower_) lower_ = run.lower_stored_key();
        if (upper_ < run.upper_stored_key()) upper_ = run.upper_stored_key();
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The runs of the level, oldest first when they overlap and ascending when they do not. */
    st::vector<run_handle_t> runs_;

    /** The pool every run of the level lives in, which outlives the level. */
    run_pool_t* run_pool_ = nullptr;

    /** Where this level sits in its tree. */
    std::uint32_t number_ = 0;

    /** Whether the runs may overlap. */
    lsm_level_order_t order_ = lsm_level_order_t::sorted_k;

    /** Bytes every run takes, kept as runs are named rather than recounted. */
    std::uint64_t size_bytes_ = 0;

    /** Key slots every run holds, kept as runs are named rather than recounted. */
    std::uint64_t population_count_ = 0;

    /** The least key any run holds. */
    stored_key_t lower_ = limit<stored_key_t>::max();

    /** The greatest key any run holds. */
    stored_key_t upper_ = limit<stored_key_t>::min();

    /** Range tombstones every run carries, kept as runs are named rather than recounted. */
    std::uint64_t range_tombstones_count_ = 0;

#pragma endregion Private Members
};

#pragma endregion Level

#pragma endregion LSM Level

} // namespace unum::ustore::blobs
