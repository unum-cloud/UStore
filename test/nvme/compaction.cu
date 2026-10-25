/**
 *  @file test/nvme/compaction.cu
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief Tests host and GPU compaction against a reference merge and against each other, for 64-bit and 16-byte keys.
 *
 *  The suite compiles as plain C++ twice: the build over the CUDA compactor adds the device cases, which the host build
 *  reports as skipped, and @c compaction_device.cu instantiates that compactor for the suite's key slots. LSM
 *  levels are generated from a seed, with overlapping and sorted runs, remove deltas, versions of one key under
 *  several stamps, and keys crowded at the edges a comparator gets wrong. Graph levels of half-edge changes check the
 *  device fold against the host fold byte for byte.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcmp`

#include <algorithm>   // `std::sort`, `std::unique`
#include <array>       // `std::array`
#include <chrono>      // `std::chrono::steady_clock`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::type_identity`

#include <fmt/format.h>

#include <smashtable/basic_vector.hpp>
#include <smashtable/shared.hpp>

#include "ustore/blobs/nvme_compaction.hpp"        // `host_compactor_t`, `compacted_sorted_run_t`, `compaction_kind_t`
#include "ustore/graph/adjacency_fold.hpp"         // `adjacency_fold_t`, `vram_nvidia_adjacency_fold_t`, `save_delta`
#include "ustore/utilities/types.hpp"              // `status_t`, `status_code_t`
#include "ustore/utilities/vram_nvidia_device.hpp" // `cuda_count_devices`, `cuda_pinned_allocator`

#include "harness.hpp"                   // `suite_exit_code`, `expect`, `expect_eq`
#include "nvme/compaction_key_slots.hpp" // `inline_key_slot`, `fixed_width_key_slot`, `variable_width_key_slot`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;
using st::basic_vector;

#pragma region Key Slots

/** Whether a generated key slot holds a stored value or a remove delta. */
enum class key_slot_state_t : bool {

    /** The key slot holds a stored value. */
    alive_k,

    /** The key slot holds a remove delta. */
    removed_k,
};

/** The kind a key slot of @p state carries in its run's kinds column. */
inline std::uint8_t kind_of(key_slot_state_t state) noexcept {
    return static_cast<std::uint8_t>(state == key_slot_state_t::removed_k ? delta_kind_t::remove_k
                                                                          : delta_kind_t::write_k);
}

/** An inline key slot of @p key with random inline bytes. */
template <typename stored_key_type_>
inline_key_slot<stored_key_type_> make_key_slot(std::type_identity<inline_key_slot<stored_key_type_>>,
                                                stored_key_type_ const& key, key_slot_state_t state,
                                                random_t& random) noexcept {
    inline_key_slot<stored_key_type_> key_slot;
    key_slot.stored_key = key;
    if (state == key_slot_state_t::removed_k) return key_slot;
    key_slot.size = static_cast<std::uint32_t>(1 + random.below(8));
    for (std::uint8_t& byte : key_slot.data) byte = static_cast<std::uint8_t>(random.next());
    return key_slot;
}

/** A fixed-size key slot of @p key at a random old location. */
template <typename stored_key_type_>
fixed_width_key_slot<stored_key_type_> make_key_slot(std::type_identity<fixed_width_key_slot<stored_key_type_>>,
                                                     stored_key_type_ const& key, key_slot_state_t state,
                                                     random_t& random) noexcept {
    fixed_width_key_slot<stored_key_type_> key_slot;
    key_slot.stored_key = key;
    key_slot.size = state == key_slot_state_t::removed_k ? 0 : 24;
    key_slot.location = static_cast<std::uint32_t>(random.below(std::uint64_t {1} << 30));
    return key_slot;
}

/** A variable-size key slot of @p key, empty once in 16 when alive. */
template <typename stored_key_type_>
variable_width_key_slot<stored_key_type_> make_key_slot(std::type_identity<variable_width_key_slot<stored_key_type_>>,
                                                        stored_key_type_ const& key, key_slot_state_t state,
                                                        random_t& random) noexcept {
    variable_width_key_slot<stored_key_type_> key_slot;
    key_slot.stored_key = key;
    if (state == key_slot_state_t::removed_k || random.below(16) == 0) return key_slot;
    key_slot.size = static_cast<std::uint32_t>(1 + random.below(200));
    key_slot.location = static_cast<std::uint32_t>(random.below(std::uint64_t {1} << 30));
    return key_slot;
}

/** The key slot input padding holds here: one that looks real, so a compaction reading padding shows in its output. */
template <typename key_slot_type_>
key_slot_type_ poison_key_slot() noexcept {
    key_slot_type_ poison {};
    poison.stored_key = limit<typename key_slot_type_::stored_key_t>::min();
    poison.size = 7;
    return poison;
}

/** Whether two inline key slots carry the same inline bytes. */
template <typename stored_key_type_>
bool same_inline_stored_value(inline_key_slot<stored_key_type_> const& first,
                              inline_key_slot<stored_key_type_> const& second) noexcept {
    return std::memcmp(first.data, second.data, sizeof(first.data)) == 0;
}

/** Key slots without inline bytes agree on them trivially. */
template <typename key_slot_type_>
bool same_inline_stored_value(key_slot_type_ const&, key_slot_type_ const&) noexcept {
    return true;
}

/** Whether two key slots agree in every field a compaction writes. */
template <typename key_slot_type_>
bool same_key_slot(key_slot_type_ const& first, key_slot_type_ const& second) noexcept {
    bool same = (first.stored_key == second.stored_key) && (first.size == second.size) &&
                same_inline_stored_value(first, second);
    if constexpr (locates_stored_values_concept<key_slot_type_>) same = same && first.location == second.location;
    return same;
}

#pragma endregion Key Slots

#pragma region Keys

/** A high word many 16-byte keys share, crowding them into one run of equal high words. */
inline constexpr std::uint64_t shared_high_word_k = 0x0123456789ABCDEFull;

/** Appends 64-bit keys a comparator most often gets wrong: both ends, the sign boundary and small integers. */
st::status_t append_adversarial_keys(basic_vector<std::uint64_t>& keys) noexcept {
    constexpr std::uint64_t ones = ~std::uint64_t {0};
    constexpr std::uint64_t sign = std::uint64_t {1} << 63;
    std::array<std::uint64_t, 9> const edges {0, 1, 2, ones, ones - 1, ones - 2, sign - 1, sign, sign + 1};
    for (std::uint64_t const edge : edges) {
        st::status_t const status = keys.push_back(std::uint64_t {edge});
        if (status != st::status_t::success_k) return status;
    }
    for (std::uint64_t identity = 0; identity != 1000; ++identity) {
        st::status_t const status = keys.push_back(std::uint64_t {identity});
        if (status != st::status_t::success_k) return status;
    }
    return st::status_t::success_k;
}

/** Appends 16-byte keys a comparator most often gets wrong: all zeros, all ones, equal high words and integer
 *  identities. */
st::status_t append_adversarial_keys(basic_vector<st::key128_t>& keys) noexcept {
    constexpr std::uint64_t ones = ~std::uint64_t {0};
    constexpr std::uint64_t sign = std::uint64_t {1} << 63;
    std::array<st::key128_t, 10> const edges {{{0, 0},
                                               {0, 1},
                                               {1, 0},
                                               {0, ones},
                                               {ones, 0},
                                               {ones, ones},
                                               {ones, ones - 1},
                                               {sign, 0},
                                               {sign - 1, ones},
                                               {shared_high_word_k, ones}}};
    for (st::key128_t const& edge : edges) {
        st::status_t const status = keys.push_back(st::key128_t {edge});
        if (status != st::status_t::success_k) return status;
    }
    for (std::uint64_t low = 0; low != 500; ++low) {
        st::status_t status = keys.push_back(st::key128_t {shared_high_word_k, low});
        if (status != st::status_t::success_k) return status;
        status = keys.push_back(st::key128_t {shared_high_word_k, ones - low});
        if (status != st::status_t::success_k) return status;
    }
    for (std::uint64_t identity = 0; identity != 1000; ++identity) {
        st::status_t const status = keys.push_back(st::key128_t {0, identity});
        if (status != st::status_t::success_k) return status;
    }
    return st::status_t::success_k;
}

/** A random 64-bit key. */
std::uint64_t random_key(random_t& random, std::type_identity<std::uint64_t>) noexcept { return random.next(); }

/** A random 16-byte key, a quarter of them sharing one high word. */
st::key128_t random_key(random_t& random, std::type_identity<st::key128_t>) noexcept {
    std::uint64_t const high = random.below(4) == 0 ? shared_high_word_k : random.next();
    return {high, random.next()};
}

/** Fills @p universe with the adversarial keys and @p random_keys random ones, sorted and unique. */
template <typename stored_key_type_>
st::status_t make_universe(std::uint64_t random_keys, random_t& random, basic_vector<stored_key_type_>& universe) {
    universe.clear();
    st::status_t status = append_adversarial_keys(universe);
    if (status != st::status_t::success_k) return status;
    for (std::uint64_t index = 0; index != random_keys; ++index) {
        status = universe.push_back(random_key(random, std::type_identity<stored_key_type_> {}));
        if (status != st::status_t::success_k) return status;
    }
    stored_key_type_* const begin = universe.data();
    std::sort(begin, begin + universe.size());
    return universe.resize(static_cast<std::size_t>(std::unique(begin, begin + universe.size()) - begin));
}

#pragma endregion Keys

#pragma region LSM Levels

/** One compaction the suite generates LSM levels for. */
struct scenario_t {

    /** Name printed with the result. */
    char const* name;

    /** Kind of compaction. */
    compaction_kind_t kind;

    /** Sorted runs on the upper LSM level. */
    std::uint64_t upper_sorted_runs;

    /** Sorted runs on the lower LSM level, none when the compaction writes a new bottom LSM level. */
    std::uint64_t lower_sorted_runs;

    /** Random keys beside the adversarial ones, or zero to take the large universe from the flags. */
    std::uint64_t random_keys;

    /** Chance per thousand that a sorted run picks a key. */
    std::uint64_t density_per_mille;

    /** Chance per thousand that a picked key slot holds a remove delta. */
    std::uint64_t removals_per_mille;

    /** Bounds of the new sorted runs. */
    sorted_run_bounds_t bounds;

    /** Chance per thousand that a picked key also carries an older version inside the same sorted run. */
    std::uint64_t versions_per_mille = 0;

    /** Stamps readers pin, drawn at random from the stamps the LSM levels carry. */
    std::uint64_t pinned = 0;

    /** Where the durable horizon sits among the stamps, per thousand of their range, a thousand keeping it past all. */
    std::uint64_t horizon_per_mille = 1000;

    /** Where the ceiling sits among the stamps, per thousand of their range, a thousand keeping it past all. */
    std::uint64_t ceiling_per_mille = 1000;

    /** Where the floor sits among the stamps, per thousand of their range. */
    std::uint64_t floor_per_mille = 0;
};

/** Stamps one sorted run's versions are drawn from: each run's own span, newer runs holding greater ones. */
inline constexpr std::uint64_t stamps_per_sorted_run_k = std::uint64_t {1} << 32;

/** An LSM level under test: each sorted run's sorted key slots, and its cells once laid out. */
template <typename key_slot_type_>
struct test_lsm_level {

    /** Every sorted run's key slots in ascending order, one sorted run after another. */
    basic_vector<key_slot_type_> sorted;

    /** The stamp of every key slot of @c sorted, newest first among the versions of one key. */
    basic_vector<std::uint64_t> stamps;

    /** The @c delta_kind_t of every key slot of @c sorted. */
    basic_vector<std::uint8_t> kinds;

    /** Key slots in each sorted run. */
    basic_vector<std::uint64_t> counts;

    /** Every sorted run's cells, one sorted run after another. */
    basic_vector<key_slot_type_> cells;

    /** Every sorted run's stamps packed, as a run keeps them, or none for an LSM level without stamps. */
    basic_vector<stamps_column_t> columns;

    /** Every sorted run's kinds packed, as a run keeps them. */
    basic_vector<kinds_column_t> kind_columns;

    /** A view per sorted run into @c cells, @c columns and @c kind_columns. */
    basic_vector<sorted_run_index_view<key_slot_type_>> views;
};

/** The views of an LSM level's sorted runs, as a compaction takes them. */
template <typename key_slot_type_>
std::span<sorted_run_index_view<key_slot_type_> const> views_of(
    test_lsm_level<key_slot_type_> const& lsm_level) noexcept {
    return lsm_level.views;
}

/** Assigns sorted positions to a sorted run index's cells the way the B-tree builder fills sorted runs, answering the
 *  next position. */
std::uint64_t assign_sorted_positions(std::uint64_t* positions, std::uint64_t rows_count, std::uint64_t branching,
                                      std::uint64_t row, std::uint64_t next) noexcept {
    if (row >= rows_count) return next;
    for (std::uint64_t slot = 0; slot != branching; ++slot) {
        next = assign_sorted_positions(positions, rows_count, branching, row * (branching + 1) + slot + 1, next);
        positions[row * branching + slot] = next++;
    }
    return assign_sorted_positions(positions, rows_count, branching, row * (branching + 1) + branching + 1, next);
}

/** A key slot state with the scenario's removal chance. */
inline key_slot_state_t random_state(scenario_t const& scenario, random_t& random) noexcept {
    return random.below(1000) < scenario.removals_per_mille ? key_slot_state_t::removed_k : key_slot_state_t::alive_k;
}

/**
 *  @brief Picks each key of @p keys with the scenario's density, appending its key slot to @p key_slots and its stamp,
 *      drawn from the span of era @p era, to @p stamps.
 *
 *  A key picked twice over carries two versions in this one sorted run, the newer from the upper half of the era.
 */
template <typename key_slot_type_>
st::status_t pick_key_slots(std::span<typename key_slot_type_::stored_key_t const> keys, scenario_t const& scenario,
                            std::uint64_t era, random_t& random, basic_vector<key_slot_type_>& key_slots,
                            basic_vector<std::uint64_t>& stamps, basic_vector<std::uint8_t>& kinds) noexcept {
    std::uint64_t const half = stamps_per_sorted_run_k / 2;
    std::uint64_t const base = era * stamps_per_sorted_run_k;
    auto const pick = [&](typename key_slot_type_::stored_key_t const& key) noexcept {
        key_slot_state_t const state = random_state(scenario, random);
        st::status_t const status = key_slots.push_back(
            make_key_slot(std::type_identity<key_slot_type_> {}, key, state, random));
        return status != st::status_t::success_k ? status : kinds.push_back(kind_of(state));
    };
    for (std::size_t index = 0; index != keys.size(); ++index) {
        if (random.below(1000) >= scenario.density_per_mille) continue;
        bool const versioned = random.below(1000) < scenario.versions_per_mille;
        st::status_t status = pick(keys[index]);
        if (status != st::status_t::success_k) return status;
        status = stamps.push_back(base +
                                  (versioned ? half + random.below(half) : random.below(stamps_per_sorted_run_k)));
        if (status != st::status_t::success_k) return status;
        if (!versioned) continue;
        status = pick(keys[index]);
        if (status != st::status_t::success_k) return status;
        status = stamps.push_back(base + random.below(half));
        if (status != st::status_t::success_k) return status;
    }
    return st::status_t::success_k;
}

/** Adds @p sorted_runs overlapping sorted runs, oldest first, each picking its own keys from an era of its own. */
template <typename key_slot_type_>
st::status_t add_overlapping_sorted_runs(std::span<typename key_slot_type_::stored_key_t const> keys,
                                         scenario_t const& scenario, std::uint64_t sorted_runs, std::uint64_t first_era,
                                         random_t& random, test_lsm_level<key_slot_type_>& lsm_level) noexcept {
    for (std::uint64_t sorted_run = 0; sorted_run != sorted_runs; ++sorted_run) {
        std::size_t const before = lsm_level.sorted.size();
        st::status_t status = pick_key_slots<key_slot_type_>(keys, scenario, first_era + sorted_run, random,
                                                             lsm_level.sorted, lsm_level.stamps, lsm_level.kinds);
        if (status != st::status_t::success_k) return status;
        std::uint64_t const count = lsm_level.sorted.size() - before;
        if (count == 0) continue;
        status = lsm_level.counts.push_back(std::uint64_t {count});
        if (status != st::status_t::success_k) return status;
    }
    return st::status_t::success_k;
}

/** Adds one sorted LSM level's keys from era @p era, cut into @p sorted_runs disjoint ascending sorted runs that never
 *  split the versions of one key. */
template <typename key_slot_type_>
st::status_t add_sorted_sorted_runs(std::span<typename key_slot_type_::stored_key_t const> keys,
                                    scenario_t const& scenario, std::uint64_t sorted_runs, std::uint64_t era,
                                    random_t& random, test_lsm_level<key_slot_type_>& lsm_level) noexcept {
    st::status_t status = pick_key_slots<key_slot_type_>(keys, scenario, era, random, lsm_level.sorted,
                                                         lsm_level.stamps, lsm_level.kinds);
    if (status != st::status_t::success_k) return status;
    std::uint64_t const total = lsm_level.sorted.size();
    if (total == 0) return st::status_t::success_k;
    std::uint64_t const chunk = divide_round_up(total, sorted_runs);
    for (std::uint64_t offset = 0; offset < total;) {
        std::uint64_t end = chunk < total - offset ? offset + chunk : total;
        while (end != total && lsm_level.sorted[end].stored_key == lsm_level.sorted[end - 1].stored_key) ++end;
        status = lsm_level.counts.push_back(std::uint64_t {end - offset});
        if (status != st::status_t::success_k) return status;
        offset = end;
    }
    return st::status_t::success_k;
}

/** Lays every sorted run of @p lsm_level into sorted run index cells, filling padding with poison. */
template <typename key_slot_type_>
st::status_t lay_out(test_lsm_level<key_slot_type_>& lsm_level, std::uint64_t branching) noexcept {
    std::uint64_t cells_count = 0;
    for (std::size_t index = 0; index != lsm_level.counts.size(); ++index)
        cells_count += round_up_to_multiple(lsm_level.counts[index], branching);
    st::status_t status = lsm_level.cells.resize(cells_count);
    if (status != st::status_t::success_k) return status;
    status = lsm_level.views.resize(lsm_level.counts.size());
    if (status != st::status_t::success_k) return status;
    bool const stamped = !lsm_level.stamps.empty();
    status = lsm_level.columns.resize(stamped ? lsm_level.counts.size() : 0);
    if (status != st::status_t::success_k) return status;
    bool const kinded = !lsm_level.kinds.empty();
    status = lsm_level.kind_columns.resize(kinded ? lsm_level.counts.size() : 0);
    if (status != st::status_t::success_k) return status;

    basic_vector<std::uint64_t> positions;
    key_slot_type_ const poison = poison_key_slot<key_slot_type_>();
    std::uint64_t first = 0;
    std::uint64_t first_cell = 0;
    for (std::size_t sorted_run = 0; sorted_run != lsm_level.counts.size(); ++sorted_run) {
        std::uint64_t const count = lsm_level.counts[sorted_run];
        std::uint64_t const rows = divide_round_up(count, branching);
        status = positions.resize(rows * branching);
        if (status != st::status_t::success_k) return status;
        [[maybe_unused]] std::uint64_t const assigned = assign_sorted_positions(positions.data(), rows, branching, 0,
                                                                                0);
        for (std::uint64_t cell = 0; cell != rows * branching; ++cell)
            lsm_level.cells[first_cell + cell] = positions[cell] < count ? lsm_level.sorted[first + positions[cell]]
                                                                         : poison;
        lsm_level.views[sorted_run] = sorted_run_index_view<key_slot_type_> {lsm_level.cells.data() + first_cell,
                                                                             count};
        if (stamped) {
            st::expected<stamps_column_t> column = stamps_column_t::make(
                std::span<std::uint64_t const> {lsm_level.stamps.data() + first, static_cast<std::size_t>(count)});
            if (!column) return column.status();
            lsm_level.columns[sorted_run] = std::move(*column);
            lsm_level.views[sorted_run].stamps = &lsm_level.columns[sorted_run];
        }
        if (kinded) {
            st::expected<kinds_column_t> column = kinds_column_t::make(
                std::span<std::uint8_t const> {lsm_level.kinds.data() + first, static_cast<std::size_t>(count)});
            if (!column) return column.status();
            lsm_level.kind_columns[sorted_run] = std::move(*column);
            lsm_level.views[sorted_run].kinds = &lsm_level.kind_columns[sorted_run];
        }
        first += count;
        first_cell += rows * branching;
    }
    return st::status_t::success_k;
}

/**
 *  @brief The survivors a compaction must keep, found by sorting every key slot by stored key, then newest first, and
 *      keeping per key what someone can still read.
 *
 *  Spelled from what is read rather than from intervals, so it checks the rule instead of restating it: the newest
 *  version at or below the ceiling, the newest at or below every pinned stamp, every version above the horizon and
 *  the newest at or below it. A remove delta kept for no reader older than every pin, at the bottom, hides nothing.
 */
template <typename key_slot_type_>
st::status_t reference_survivors(compaction_kind_t kind, test_lsm_level<key_slot_type_> const& upper,
                                 test_lsm_level<key_slot_type_> const& lower, compaction_stamps_t const& stamps,
                                 basic_vector<located_key_slot<key_slot_type_>>& survivors) noexcept {
    using record_t = located_key_slot<key_slot_type_>;
    basic_vector<record_t> all;
    st::status_t status = all.resize(upper.sorted.size() + lower.sorted.size());
    if (status != st::status_t::success_k) return status;

    std::size_t written = 0;
    std::uint32_t sorted_run = 0;
    std::array<test_lsm_level<key_slot_type_> const*, 2> const lsm_levels {&upper, &lower};
    for (test_lsm_level<key_slot_type_> const* lsm_level : lsm_levels) {
        std::uint64_t first = 0;
        for (std::size_t index = 0; index != lsm_level->counts.size(); ++index, ++sorted_run) {
            for (std::uint64_t position = 0; position != lsm_level->counts[index]; ++position) {
                std::uint64_t const stamp = lsm_level->stamps.empty() ? 0 : lsm_level->stamps[first + position];
                delta_kind_t const kind = lsm_level->kinds.empty()
                                              ? delta_kind_t::write_k
                                              : static_cast<delta_kind_t>(lsm_level->kinds[first + position]);
                all[written++] = record_t {lsm_level->sorted[first + position], sorted_run, kind, stamp};
            }
            first += lsm_level->counts[index];
        }
    }

    std::uint64_t const upper_count = upper.counts.size();
    auto const rank = [upper_count](std::uint32_t index) noexcept {
        return index < upper_count ? upper_count - 1 - index : std::uint64_t {index};
    };
    std::stable_sort(all.data(), all.data() + all.size(),
                     [&rank](record_t const& first, record_t const& second) noexcept {
                         if (first.key_slot.stored_key < second.key_slot.stored_key) return true;
                         if (second.key_slot.stored_key < first.key_slot.stored_key) return false;
                         return rank(first.sorted_run) < rank(second.sorted_run);
                     });

    bool const drops_removals = (kind == compaction_kind_t::overlapping_into_bottom_k) |
                                (kind == compaction_kind_t::sorted_into_bottom_k);
    std::uint64_t const oldest_pin = stamps.pinned_count != 0 ? stamps.pinned[0] : ~std::uint64_t {0};
    basic_vector<std::uint8_t> kept;
    status = kept.resize(all.size());
    if (status != st::status_t::success_k) return status;
    survivors.clear();
    for (std::size_t begin = 0; begin != all.size();) {
        std::size_t end = begin + 1;
        while (end != all.size() && all[end].key_slot.stored_key == all[begin].key_slot.stored_key) ++end;
        for (std::size_t index = begin; index != end; ++index) kept[index] = 0;

        // The newest version at or below each of the ceiling, the horizon and every pin is read by someone
        auto const keep_newest_at_or_below = [&](std::uint64_t bound) noexcept {
            for (std::size_t index = begin; index != end; ++index)
                if (all[index].stamp <= bound && all[index].stamp <= stamps.ceiling) {
                    kept[index] = 1;
                    return;
                }
        };
        keep_newest_at_or_below(stamps.ceiling);
        keep_newest_at_or_below(stamps.horizon);
        for (std::uint64_t pin = 0; pin != stamps.pinned_count; ++pin) keep_newest_at_or_below(stamps.pinned[pin]);
        for (std::size_t index = begin; index != end; ++index)
            if (all[index].stamp > stamps.horizon && all[index].stamp <= stamps.ceiling) kept[index] = 1;

        for (std::size_t index = begin; index != end; ++index) {
            if (!kept[index]) continue;
            bool const hides_nothing = all[index].stamp <= stamps.horizon && all[index].stamp <= oldest_pin;
            if (drops_removals && all[index].kind == delta_kind_t::remove_k && hides_nothing) continue;
            status = survivors.push_back(record_t {all[index]});
            if (status != st::status_t::success_k) return status;
        }
        begin = end;
    }
    return st::status_t::success_k;
}

#pragma endregion LSM Levels

#pragma region Checks

/** Checks a compaction's output against the reference survivors: order, bounds, padding, locations and migrations. */
template <typename key_slot_type_>
std::size_t check_compaction(compaction_kind_t kind, sorted_run_bounds_t const& bounds,
                             compaction_stamps_t const& stamps,
                             basic_vector<located_key_slot<key_slot_type_>> const& wanted_survivors,
                             compaction_output<key_slot_type_> const& output) {
    using stored_key_t = typename key_slot_type_::stored_key_t;
    std::size_t failures = 0;

    std::uint64_t key_slots_count = 0;
    std::uint64_t cells_count = 0;
    for (std::size_t index = 0; index != output.sorted_runs.size(); ++index) {
        key_slots_count += output.sorted_runs[index].key_slots_count;
        cells_count += round_up_to_multiple(output.sorted_runs[index].key_slots_count, bounds.branching);
    }
    failures += expect_eq(key_slots_count, std::uint64_t {wanted_survivors.size()});
    failures += expect_eq(output.cells.size(), std::size_t {cells_count});
    failures += expect_eq(output.migrations.size(),
                          locates_stored_values_concept<key_slot_type_> ? key_slots_count : 0u);
    failures += expect_eq(output.stamps.size(), std::size_t {key_slots_count});
    failures += expect_eq(output.kinds.size(), std::size_t {key_slots_count});
    if (failures != 0) return failures;

    // Every survivor carries its stamp raised to the floor and its kind, and the versions of one key never straddle
    // two runs
    std::size_t wrong_stamps = 0;
    std::size_t wrong_kinds = 0;
    for (std::size_t index = 0; index != wanted_survivors.size(); ++index) {
        wrong_stamps += output.stamps[index] != std::max(wanted_survivors[index].stamp, stamps.floor);
        wrong_kinds += output.kinds[index] != static_cast<std::uint8_t>(wanted_survivors[index].kind);
    }
    failures += expect_eq(wrong_stamps, std::size_t {0});
    failures += expect_eq(wrong_kinds, std::size_t {0});
    std::size_t split_keys = 0;
    bool versioned = false;
    for (std::size_t index = 1; index < wanted_survivors.size(); ++index)
        versioned |= wanted_survivors[index].key_slot.stored_key == wanted_survivors[index - 1].key_slot.stored_key;
    for (std::uint64_t index = 0, first = 0; index + 1 < output.sorted_runs.size(); ++index) {
        first += output.sorted_runs[index].key_slots_count;
        if (first == 0 || first >= wanted_survivors.size()) continue;
        split_keys += wanted_survivors[first].key_slot.stored_key == wanted_survivors[first - 1].key_slot.stored_key;
    }
    failures += expect_eq(split_keys, std::size_t {0});

    bool const drops_removals = (kind == compaction_kind_t::overlapping_into_bottom_k) |
                                (kind == compaction_kind_t::sorted_into_bottom_k);
    std::size_t empty_sorted_runs = 0;
    std::size_t oversized_sorted_runs = 0;
    std::size_t unfilled_sorted_runs = 0;
    std::size_t wrong_bytes = 0;
    std::size_t wrong_padding = 0;
    std::size_t wrong_key_slots = 0;
    std::size_t wrong_migrations = 0;
    std::size_t unordered = 0;
    std::size_t kept_removals = 0;

    basic_vector<std::uint64_t> positions;
    basic_vector<std::uint64_t> cell_of;
    stored_key_t previous {};
    std::uint64_t first = 0;
    std::uint64_t first_cell = 0;
    for (std::size_t index = 0; index != output.sorted_runs.size(); ++index) {
        compacted_sorted_run_t const& sorted_run = output.sorted_runs[index];
        std::uint64_t const count = sorted_run.key_slots_count;
        std::uint64_t const rows = divide_round_up(count, bounds.branching);
        std::uint64_t const cells = rows * bounds.branching;
        empty_sorted_runs += count == 0;
        oversized_sorted_runs += count > bounds.key_slots_capacity;
        oversized_sorted_runs += (count > 1) &&
                                 sorted_run_file_bytes<key_slot_type_>(bounds, count, sorted_run.stored_values_bytes) >
                                     bounds.file_bytes_limit;
        // A run cut short to keep a key's versions together is not unfilled, so only unversioned merges check this
        if (!versioned && index + 1 != output.sorted_runs.size() && count != bounds.key_slots_capacity) {
            std::uint64_t const next_bytes = stored_value_bytes(wanted_survivors[first + count].key_slot);
            unfilled_sorted_runs += sorted_run_file_bytes<key_slot_type_>(
                                        bounds, count + 1, sorted_run.stored_values_bytes + next_bytes) <=
                                    bounds.file_bytes_limit;
        }
        if (count == 0) continue;

        if (positions.resize(cells) != st::status_t::success_k || cell_of.resize(cells) != st::status_t::success_k)
            return failures + expect(false);
        [[maybe_unused]] std::uint64_t const assigned = assign_sorted_positions(positions.data(), rows,
                                                                                bounds.branching, 0, 0);
        for (std::uint64_t cell = 0; cell != cells; ++cell) cell_of[positions[cell]] = cell;
        for (std::uint64_t position = count; position != cells; ++position) {
            key_slot_type_ const& padding = output.cells[first_cell + cell_of[position]];
            wrong_padding += !(padding.stored_key == limit<stored_key_t>::max()) || padding.size != 0;
        }

        std::uint64_t const section_offset = stored_values_offset<key_slot_type_>(bounds, count);
        std::uint64_t running = 0;
        for (std::uint64_t position = 0; position != count; ++position) {
            located_key_slot<key_slot_type_> const& wanted = wanted_survivors[first + position];
            key_slot_type_ const& got = output.cells[first_cell + cell_of[position]];
            key_slot_type_ wanted_key_slot = wanted.key_slot;
            if constexpr (locates_stored_values_concept<key_slot_type_>) {
                wanted_key_slot.location = static_cast<std::uint32_t>(section_offset + running);
                stored_value_migration_t const& migration = output.migrations[first + position];
                wrong_migrations += migration.size != stored_value_bytes(wanted.key_slot) ||
                                    migration.old_location != wanted.key_slot.location ||
                                    migration.old_sorted_run != wanted.sorted_run;
            }
            wrong_key_slots += !same_key_slot(got, wanted_key_slot);
            running += stored_value_bytes(wanted.key_slot);
            // A remove delta survives the bottom only where a reader still needs it to hide an older version
            kept_removals += drops_removals &&
                             output.kinds[first + position] == static_cast<std::uint8_t>(delta_kind_t::remove_k) &&
                             stamps.pinned_count == 0 && stamps.horizon == ~std::uint64_t {0};
            if (first + position != 0)
                unordered += got.stored_key < previous || (!versioned && got.stored_key == previous);
            previous = got.stored_key;
        }
        wrong_bytes += running != sorted_run.stored_values_bytes;
        first += count;
        first_cell += cells;
    }

    failures += expect_eq(empty_sorted_runs, std::size_t {0});
    failures += expect_eq(oversized_sorted_runs, std::size_t {0});
    failures += expect_eq(unfilled_sorted_runs, std::size_t {0});
    failures += expect_eq(wrong_bytes, std::size_t {0});
    failures += expect_eq(wrong_padding, std::size_t {0});
    failures += expect_eq(wrong_key_slots, std::size_t {0});
    failures += expect_eq(wrong_migrations, std::size_t {0});
    failures += expect_eq(unordered, std::size_t {0});
    failures += expect_eq(kept_removals, std::size_t {0});
    return failures;
}

/** How many sizes, sorted runs, cell fields, migrations, stamps, kinds and folded bytes two compactions of the same LSM
 *  levels disagree in. */
template <typename key_slot_type_>
std::size_t differences(compaction_output<key_slot_type_> const& host,
                        compaction_output<key_slot_type_> const& device) {
    std::size_t const unequal_sizes =
        (device.sorted_runs.size() != host.sorted_runs.size()) + (device.cells.size() != host.cells.size()) +
        (device.migrations.size() != host.migrations.size()) + (device.stamps.size() != host.stamps.size()) +
        (device.kinds.size() != host.kinds.size()) + (device.folded.size() != host.folded.size());
    if (unequal_sizes != 0) return unequal_sizes;

    std::size_t mismatches = 0;
    for (std::size_t index = 0; index != host.sorted_runs.size(); ++index)
        mismatches += host.sorted_runs[index].key_slots_count != device.sorted_runs[index].key_slots_count ||
                      host.sorted_runs[index].stored_values_bytes != device.sorted_runs[index].stored_values_bytes;
    for (std::size_t index = 0; index != host.cells.size(); ++index)
        mismatches += !same_key_slot(host.cells[index], device.cells[index]);
    for (std::size_t index = 0; index != host.migrations.size(); ++index)
        mismatches += host.migrations[index].size != device.migrations[index].size ||
                      host.migrations[index].old_location != device.migrations[index].old_location ||
                      host.migrations[index].old_sorted_run != device.migrations[index].old_sorted_run;
    for (std::size_t index = 0; index != host.stamps.size(); ++index)
        mismatches += host.stamps[index] != device.stamps[index];
    for (std::size_t index = 0; index != host.kinds.size(); ++index)
        mismatches += host.kinds[index] != device.kinds[index];
    for (std::size_t index = 0; index != host.folded.size(); ++index)
        mismatches += host.folded[index] != device.folded[index];
    return mismatches;
}

/** Expects two compactions of the same LSM levels to agree in every sorted run, cell field, migration and folded
 *  byte. */
template <typename key_slot_type_>
std::size_t expect_identical(compaction_output<key_slot_type_> const& host,
                             compaction_output<key_slot_type_> const& device) {
    return expect_eq(differences(host, device), std::size_t {0});
}

#pragma endregion Checks

#pragma region Rules

/** The key slot shape the rule checks use: it locates stored values and is exactly 16 bytes. */
using checked_key_slot_t = fixed_width_key_slot<std::uint64_t>;

/** A located key slot of @p key from sorted run @p sorted_run, with no stored value to migrate. */
located_key_slot<checked_key_slot_t> checked_record(std::uint64_t key, std::uint32_t sorted_run) noexcept {
    checked_key_slot_t key_slot;
    key_slot.stored_key = key;
    key_slot.size = 24;
    key_slot.location = 0;
    return {key_slot, sorted_run};
}

/**
 *  @brief Checks that both compactors' comparator is a strict weak ordering that puts the survivor first.
 *
 *  Equal stored keys order by survival: the newest upper sorted run leads, every upper sorted run leads every lower
 *  one, and the lower sorted runs follow in ascending order.
 */
std::size_t survivor_first_orders_the_survivor_first() {
    std::size_t failures = 0;
    constexpr std::uint32_t upper_sorted_runs_count = 3;
    constexpr std::uint32_t sorted_runs_count = 5;
    survivor_first_t const order {upper_sorted_runs_count};

    failures += expect(order(checked_record(1, 4), checked_record(2, 0)));
    failures += expect(!order(checked_record(2, 0), checked_record(1, 4)));
    failures += expect(order(checked_record(7, 2), checked_record(7, 1)));
    failures += expect(order(checked_record(7, 1), checked_record(7, 0)));
    failures += expect(order(checked_record(7, 0), checked_record(7, 3)));
    failures += expect(order(checked_record(7, 3), checked_record(7, 4)));
    failures += expect(!order(checked_record(7, 4), checked_record(7, 3)));

    // Keys at both ends of the type still order by value, whichever sorted runs they came from
    constexpr std::uint64_t least = limit<std::uint64_t>::min();
    constexpr std::uint64_t greatest = limit<std::uint64_t>::max();
    failures += expect(order(checked_record(least, 4), checked_record(greatest, 0)));
    failures += expect(!order(checked_record(greatest, 0), checked_record(least, 4)));
    failures += expect(!order(checked_record(greatest, 2), checked_record(greatest, 2)));

    std::array<std::uint64_t, 4> const keys {least, 1, greatest - 1, greatest};
    std::size_t reflexive = 0;
    std::size_t asymmetric = 0;
    std::size_t transitive = 0;
    for (std::uint64_t const first_key : keys)
        for (std::uint32_t first_run = 0; first_run != sorted_runs_count; ++first_run)
            for (std::uint64_t const second_key : keys)
                for (std::uint32_t second_run = 0; second_run != sorted_runs_count; ++second_run) {
                    located_key_slot<checked_key_slot_t> const first = checked_record(first_key, first_run);
                    located_key_slot<checked_key_slot_t> const second = checked_record(second_key, second_run);
                    bool const same = (first_key == second_key) & (first_run == second_run);
                    reflexive += same && order(first, second);
                    asymmetric += order(first, second) && order(second, first);
                    if (!order(first, second)) continue;
                    for (std::uint64_t const third_key : keys)
                        for (std::uint32_t third_run = 0; third_run != sorted_runs_count; ++third_run) {
                            located_key_slot<checked_key_slot_t> const third = checked_record(third_key, third_run);
                            if (!order(second, third)) continue;
                            transitive += !order(first, third);
                        }
                }
    failures += expect_eq(reflexive, std::size_t {0});
    failures += expect_eq(asymmetric, std::size_t {0});
    failures += expect_eq(transitive, std::size_t {0});
    return failures;
}

/** Checks the survival rule over every combination of a leading key slot, a remove delta and a kind of compaction. */
std::size_t survives_follows_the_kind() {
    std::size_t failures = 0;
    std::array<compaction_kind_t, 4> const kinds {
        compaction_kind_t::overlapping_into_intermediate_k, compaction_kind_t::overlapping_into_bottom_k,
        compaction_kind_t::sorted_into_intermediate_k, compaction_kind_t::sorted_into_bottom_k};

    located_key_slot<checked_key_slot_t> const alive = checked_record(7, 0);
    located_key_slot<checked_key_slot_t> const shadowed = checked_record(7, 1);
    located_key_slot<checked_key_slot_t> const earlier = checked_record(6, 1);
    located_key_slot<checked_key_slot_t> removal = checked_record(7, 0);
    removal.key_slot.size = 0;
    removal.kind = delta_kind_t::remove_k;
    located_key_slot<checked_key_slot_t> const* const leading = nullptr;
    failures += expect(alive.kind == delta_kind_t::write_k);

    std::size_t wrong = 0;
    for (compaction_kind_t const kind : kinds) {
        bool const drops_removals = (kind == compaction_kind_t::overlapping_into_bottom_k) |
                                    (kind == compaction_kind_t::sorted_into_bottom_k);

        // The first key slot of a stored key leads, and a remove delta leads only where remove deltas stay
        wrong += !survives(leading, alive, kind, compaction_stamps_t {});
        wrong += survives(leading, removal, kind, compaction_stamps_t {}) != !drops_removals;
        wrong += !survives(&earlier, alive, kind, compaction_stamps_t {});
        wrong += survives(&earlier, removal, kind, compaction_stamps_t {}) != !drops_removals;

        // Every later key slot of the same stored key is shadowed, remove delta or not
        wrong += survives(&alive, shadowed, kind, compaction_stamps_t {});
        wrong += survives(&removal, shadowed, kind, compaction_stamps_t {});
        wrong += survives(&alive, removal, kind, compaction_stamps_t {});
    }
    failures += expect_eq(wrong, std::size_t {0});

    // Keys at both ends of the type shadow their own repeats and nothing else
    located_key_slot<checked_key_slot_t> const least = checked_record(limit<std::uint64_t>::min(), 0);
    located_key_slot<checked_key_slot_t> const greatest = checked_record(limit<std::uint64_t>::max(), 1);
    failures += expect(survives(&least, greatest, compaction_kind_t::sorted_into_bottom_k, compaction_stamps_t {}));
    failures += expect(!survives(&greatest, greatest, compaction_kind_t::sorted_into_bottom_k, compaction_stamps_t {}));
    return failures;
}

/** Checks that the split cuts sorted runs at the key slots capacity, at the file limit, and never below one. */
std::size_t plan_sorted_runs_cuts_at_the_bounds() {
    std::size_t failures = 0;
    basic_vector<compacted_sorted_run_t> sorted_runs;

    // Stored value bytes read out of a prefix sum the caller fills, as both compactors read theirs
    basic_vector<std::uint64_t> prefix;
    auto const read_prefix = [&prefix](std::uint64_t index, std::uint64_t& bytes) noexcept {
        bytes = prefix[index];
        return st::status_t::success_k;
    };
    auto const fill_prefix = [&prefix](std::uint64_t count, std::uint64_t bytes_each) noexcept {
        if (prefix.resize(count + 1) != st::status_t::success_k) return false;
        for (std::uint64_t index = 0; index != count + 1; ++index) prefix[index] = index * bytes_each;
        return true;
    };

    // Every survivor carries a key of its own, as a merge with no reader pinned leaves them
    auto const unshared = [](std::uint64_t, bool& shared) noexcept {
        shared = false;
        return st::status_t::success_k;
    };

    // A sorted run index of 16-byte cells in rows of two, with no header and no alignment to round past
    sorted_run_bounds_t bounds;
    bounds.key_slots_capacity = 4;
    bounds.file_bytes_limit = std::uint64_t {1} << 40;
    bounds.header_bytes = 0;
    bounds.alignment_bytes = 1;
    bounds.branching = 2;
    failures += expect(admits_sorted_runs(bounds));
    failures += expect_eq(sizeof(checked_key_slot_t), std::size_t {16});

    // No key slots leaves no sorted runs at all
    failures += expect(fill_prefix(0, 0));
    failures += expect(plan_sorted_runs<checked_key_slot_t>(0, bounds, read_prefix, unshared, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {0});

    // One key slot leaves one sorted run of one
    failures += expect(fill_prefix(1, 24));
    failures += expect(plan_sorted_runs<checked_key_slot_t>(1, bounds, read_prefix, unshared, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {1});
    if (sorted_runs.size() == 1) {
        failures += expect_eq(sorted_runs[0].key_slots_count, std::uint64_t {1});
        failures += expect_eq(sorted_runs[0].stored_values_bytes, std::uint64_t {24});
    }

    // The key slots capacity cuts a merge of ten into four, four and two
    failures += expect(fill_prefix(10, 24));
    failures += expect(plan_sorted_runs<checked_key_slot_t>(10, bounds, read_prefix, unshared, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {3});
    if (sorted_runs.size() == 3) {
        std::array<std::uint64_t, 3> const wanted {4, 4, 2};
        std::size_t wrong = 0;
        for (std::size_t index = 0; index != wanted.size(); ++index)
            wrong += sorted_runs[index].key_slots_count != wanted[index] ||
                     sorted_runs[index].stored_values_bytes != wanted[index] * 24;
        failures += expect_eq(wrong, std::size_t {0});
    }

    // The file limit cuts before the capacity does: three key slots of ten bytes fit 100, four do not
    bounds.key_slots_capacity = 1000;
    bounds.file_bytes_limit = 100;
    failures += expect_eq(sorted_run_file_bytes<checked_key_slot_t>(bounds, 3, 30), std::uint64_t {94});
    failures += expect_eq(sorted_run_file_bytes<checked_key_slot_t>(bounds, 4, 40), std::uint64_t {104});
    failures += expect(fill_prefix(10, 10));
    failures += expect(plan_sorted_runs<checked_key_slot_t>(10, bounds, read_prefix, unshared, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {4});
    if (sorted_runs.size() == 4) {
        std::array<std::uint64_t, 4> const wanted {3, 3, 3, 1};
        std::size_t wrong = 0;
        for (std::size_t index = 0; index != wanted.size(); ++index)
            wrong += sorted_runs[index].key_slots_count != wanted[index] ||
                     sorted_runs[index].stored_values_bytes != wanted[index] * 10;
        failures += expect_eq(wrong, std::size_t {0});
    }

    // A key slot whose stored value alone exceeds the file limit still takes a sorted run of its own
    failures += expect(fill_prefix(3, 1000));
    failures += expect(plan_sorted_runs<checked_key_slot_t>(3, bounds, read_prefix, unshared, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {3});
    if (sorted_runs.size() == 3) {
        std::size_t wrong = 0;
        for (std::size_t index = 0; index != 3; ++index)
            wrong += sorted_runs[index].key_slots_count != 1 || sorted_runs[index].stored_values_bytes != 1000;
        failures += expect_eq(wrong, std::size_t {0});
    }

    // A sorted run that exactly fills the file limit is not cut short
    bounds.file_bytes_limit = 104;
    failures += expect(fill_prefix(4, 10));
    failures += expect(plan_sorted_runs<checked_key_slot_t>(4, bounds, read_prefix, unshared, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {1});
    if (sorted_runs.size() == 1) failures += expect_eq(sorted_runs[0].key_slots_count, std::uint64_t {4});

    // A failing read of the prefix sums stops the split instead of cutting on a stale byte count
    auto const failing_prefix = [](std::uint64_t, std::uint64_t& bytes) noexcept {
        bytes = 0;
        return st::status_t::unknown_k;
    };
    failures += expect(plan_sorted_runs<checked_key_slot_t>(4, bounds, failing_prefix, unshared, sorted_runs) ==
                       st::status_t::unknown_k);

    // Versions of one key stay in one run: keys `aabbbcd` at a capacity of four cut as `aa|bbbc|d`, never inside `bbb`
    bounds.key_slots_capacity = 4;
    bounds.file_bytes_limit = std::uint64_t {1} << 40;
    std::array<int, 7> const grouped {0, 0, 1, 1, 1, 2, 3};
    auto const shares = [&grouped](std::uint64_t index, bool& shared) noexcept {
        shared = grouped[index] == grouped[index - 1];
        return st::status_t::success_k;
    };
    failures += expect(fill_prefix(7, 1));
    failures += expect(plan_sorted_runs<checked_key_slot_t>(7, bounds, read_prefix, shares, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {3});
    if (sorted_runs.size() == 3) {
        failures += expect_eq(sorted_runs[0].key_slots_count, std::uint64_t {2});
        failures += expect_eq(sorted_runs[1].key_slots_count, std::uint64_t {4});
        failures += expect_eq(sorted_runs[2].key_slots_count, std::uint64_t {1});
    }

    // A key longer than the capacity that starts the run takes the run past the capacity rather than splitting
    std::array<int, 6> const long_key {0, 0, 0, 0, 0, 1};
    auto const shares_long = [&long_key](std::uint64_t index, bool& shared) noexcept {
        shared = long_key[index] == long_key[index - 1];
        return st::status_t::success_k;
    };
    failures += expect(fill_prefix(6, 1));
    failures += expect(plan_sorted_runs<checked_key_slot_t>(6, bounds, read_prefix, shares_long, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {2});
    if (sorted_runs.size() == 2) {
        failures += expect_eq(sorted_runs[0].key_slots_count, std::uint64_t {5});
        failures += expect_eq(sorted_runs[1].key_slots_count, std::uint64_t {1});
    }
    return failures;
}

/** Checks which older versions survive: one per interval between pinned stamps, every one past the horizon, none
 *  past the ceiling, and a remove delta at the bottom only where a reader still needs it. */
std::size_t survives_keeps_what_readers_read() {
    std::size_t failures = 0;
    auto const version = [](std::uint64_t stamp, bool removal) noexcept {
        located_key_slot<checked_key_slot_t> record = checked_record(7, 0);
        record.stamp = stamp;
        if (removal) {
            record.key_slot.size = 0;
            record.kind = delta_kind_t::remove_k;
        }
        return record;
    };
    constexpr compaction_kind_t bottom = compaction_kind_t::sorted_into_bottom_k;
    constexpr compaction_kind_t intermediate = compaction_kind_t::sorted_into_intermediate_k;

    // Readers at 20 and 40: versions 50, 45, 30, 25, 10, 5 keep 50 for the head, 30 for 40 and 10 for 20
    std::array<std::uint64_t, 2> const pins {20, 40};
    compaction_stamps_t stamps;
    stamps.pinned = pins.data();
    stamps.pinned_count = pins.size();
    std::array<std::uint64_t, 6> const chain {50, 45, 30, 25, 10, 5};
    std::array<bool, 6> const wanted {true, false, true, false, true, false};
    std::size_t wrong = 0;
    for (std::size_t index = 0; index != chain.size(); ++index) {
        located_key_slot<checked_key_slot_t> const current = version(chain[index], false);
        located_key_slot<checked_key_slot_t> const previous = version(index ? chain[index - 1] : 0, false);
        wrong += survives(index ? &previous : nullptr, current, intermediate, stamps) != wanted[index];
    }
    failures += expect_eq(wrong, std::size_t {0});

    // A remove delta some reader sees stays even at the bottom; one older than every reader drops
    located_key_slot<checked_key_slot_t> const newer = version(50, false);
    failures += expect(survives(&newer, version(30, true), bottom, stamps));
    failures += expect(!survives(&newer, version(10, true), bottom, stamps));
    failures += expect(survives(&newer, version(10, true), intermediate, stamps));

    // Past the horizon every version survives, and so does the newest beneath it
    compaction_stamps_t durable;
    durable.horizon = 30;
    failures += expect(survives(&newer, version(45, false), intermediate, durable));
    located_key_slot<checked_key_slot_t> const above = version(45, false);
    failures += expect(survives(&above, version(25, false), intermediate, durable));
    located_key_slot<checked_key_slot_t> const beneath = version(25, false);
    failures += expect(!survives(&beneath, version(20, false), intermediate, durable));
    failures += expect(survives(&newer, version(45, true), bottom, durable));

    // Past the ceiling nothing survives, and the newest beneath it leads its key
    compaction_stamps_t mounted;
    mounted.horizon = 30;
    mounted.ceiling = 30;
    failures += expect(!survives(static_cast<located_key_slot<checked_key_slot_t> const*>(nullptr), version(45, false),
                                 intermediate, mounted));
    located_key_slot<checked_key_slot_t> const dropped = version(45, false);
    failures += expect(survives(&dropped, version(25, false), intermediate, mounted));
    located_key_slot<checked_key_slot_t> const leader = version(25, false);
    failures += expect(!survives(&leader, version(20, false), intermediate, mounted));
    return failures;
}

#pragma endregion Rules

#pragma region Cases

/** Flags of this suite, with their defaults. */
struct options_t {

    /** The generator every case forks, so a seed always builds the same LSM levels. */
    random_t random {0};

    /** Random keys the large cases draw from, which puts their merges past the GPU threshold. */
    count_t large_universe {600'000};
};

/** Bounds where the key slots capacity cuts sorted runs. */
inline constexpr sorted_run_bounds_t roomy_bounds_k {4096, std::uint64_t {4} << 30, 4096, 4096, 16};

/** Bounds where the file limit cuts sorted runs. */
inline constexpr sorted_run_bounds_t tight_bounds_k {std::uint64_t {1} << 20, 64 << 10, 4096, 4096, 16};

/** Bounds of tiny sorted runs with deep sorted run indexes of three cells per row. */
inline constexpr sorted_run_bounds_t narrow_bounds_k {7, std::uint64_t {4} << 30, 4096, 4096, 3};

/** Bounds of the large cases, whose sorted runs hold a quarter million key slots. */
inline constexpr sorted_run_bounds_t large_bounds_k {262'144, std::uint64_t {4} << 30, 4096, 4096, 16};

/** Every compaction the suite runs, for every key slot shape and key type: the first nine with no reader pinned and
 *  both bounds widest, which keep the newest version of every key alone, then the versioned ones. */
inline constexpr std::array<scenario_t, 15> scenarios_k {{
    {"overlapping_into_intermediate", compaction_kind_t::overlapping_into_intermediate_k, 6, 4, 20'000, 300, 100,
     roomy_bounds_k},
    {"overlapping_into_bottom", compaction_kind_t::overlapping_into_bottom_k, 6, 4, 20'000, 300, 100, tight_bounds_k},
    {"overlapping_into_empty_lsm_level", compaction_kind_t::overlapping_into_intermediate_k, 3, 0, 10'000, 400, 100,
     roomy_bounds_k},
    {"sorted_into_intermediate", compaction_kind_t::sorted_into_intermediate_k, 5, 5, 20'000, 400, 100, tight_bounds_k},
    {"sorted_into_bottom", compaction_kind_t::sorted_into_bottom_k, 5, 5, 20'000, 400, 100, roomy_bounds_k},
    {"sorted_into_new_bottom", compaction_kind_t::sorted_into_bottom_k, 4, 0, 20'000, 400, 150, roomy_bounds_k},
    {"narrow_sorted_run_indexes", compaction_kind_t::overlapping_into_bottom_k, 3, 2, 2'000, 400, 100, narrow_bounds_k},
    {"everything_removed_into_bottom", compaction_kind_t::sorted_into_bottom_k, 3, 3, 5'000, 300, 1000, roomy_bounds_k},
    {"large_overlapping_into_intermediate", compaction_kind_t::overlapping_into_intermediate_k, 4, 3, 0, 500, 50,
     large_bounds_k},
    {"pinned_overlapping_into_bottom", compaction_kind_t::overlapping_into_bottom_k, 6, 4, 20'000, 400, 200,
     roomy_bounds_k, 200, 12},
    {"pinned_sorted_into_intermediate", compaction_kind_t::sorted_into_intermediate_k, 5, 5, 20'000, 400, 200,
     narrow_bounds_k, 300, 7},
    {"horizon_overlapping_into_bottom", compaction_kind_t::overlapping_into_bottom_k, 6, 4, 20'000, 400, 200,
     tight_bounds_k, 100, 3, 600},
    {"ceiling_drops_above_mount", compaction_kind_t::overlapping_into_bottom_k, 6, 4, 20'000, 400, 200, roomy_bounds_k,
     100, 0, 550, 550},
    {"floor_raises_cold_stamps", compaction_kind_t::sorted_into_bottom_k, 5, 5, 20'000, 400, 100, roomy_bounds_k, 100,
     5, 1000, 1000, 400},
    {"large_pinned_overlapping_into_intermediate", compaction_kind_t::overlapping_into_intermediate_k, 4, 3, 0, 500, 50,
     large_bounds_k, 100, 9, 800},
}};

/** Checks that 16-byte keys order and round-trip exactly as their bytes compare with @c memcmp. */
std::size_t uuid_orders_like_memcmp(options_t const& options) {
    std::size_t failures = 0;
    std::size_t mismatches = 0;
    random_t random = options.random;
    for (std::size_t round = 0; round != 100'000; ++round) {
        std::byte first_bytes[16];
        std::byte second_bytes[16];
        std::uint64_t const shared = random.below(17);
        for (std::size_t index = 0; index != 16; ++index) {
            first_bytes[index] = static_cast<std::byte>(random.next());
            second_bytes[index] = index < shared ? first_bytes[index] : static_cast<std::byte>(random.next());
        }
        st::key128_t const first = st::key128_t::from_bytes(first_bytes);
        st::key128_t const second = st::key128_t::from_bytes(second_bytes);
        int const order = std::memcmp(first_bytes, second_bytes, sizeof(first_bytes));
        mismatches += (order < 0) != (first < second);
        mismatches += (order > 0) != (second < first);
        mismatches += (order == 0) != (first == second);
        std::byte round_trip[16];
        first.to_bytes(round_trip);
        mismatches += std::memcmp(round_trip, first_bytes, sizeof(first_bytes)) != 0;
    }
    std::byte ones[16];
    limit<st::key128_t>::max().to_bytes(ones);
    for (std::byte const byte : ones) mismatches += byte != std::byte {0xFF};
    failures += expect_eq(mismatches, std::size_t {0});
    return failures;
}

/** Checks the sorted run index layout against the recursive builder, over every small size and around level
 *  boundaries. */
std::size_t sorted_run_index_layout_matches_builder() {
    std::size_t failures = 0;
    std::size_t mismatches = 0;
    basic_vector<std::uint64_t> positions;
    auto const check = [&](std::uint64_t count, std::uint64_t branching) {
        sorted_run_index_layout_t const layout(count, branching);
        std::uint64_t const rows = divide_round_up(count, branching);
        if (layout.cells_count() != rows * branching || positions.resize(rows * branching) != st::status_t::success_k) {
            ++mismatches;
            return;
        }
        [[maybe_unused]] std::uint64_t const assigned = assign_sorted_positions(positions.data(), rows, branching, 0,
                                                                                0);
        for (std::uint64_t cell = 0; cell != layout.cells_count(); ++cell) {
            mismatches += layout.cell_to_sorted(cell) != positions[cell];
            mismatches += layout.sorted_to_cell(positions[cell]) != cell;
        }
    };
    std::array<std::uint64_t, 3> const branchings {2, 3, 16};
    for (std::uint64_t const branching : branchings)
        for (std::uint64_t count = 1; count <= 2000; ++count) check(count, branching);
    for (std::uint64_t reach = 17; reach <= 83'521; reach *= 17) {
        check(reach - 1, 16);
        check(reach, 16);
        check(reach + 1, 16);
        check(reach * 16 + 5, 16);
    }
    failures += expect_eq(mismatches, std::size_t {0});
    return failures;
}

/** A readable name of a GPU generation. */
[[maybe_unused]] std::string_view generation_name(std::uint8_t generation) noexcept {
    std::array<std::string_view, 5> const names {"unknown", "Ampere", "Ada", "Hopper", "Blackwell"};
    return generation < names.size() ? names[generation] : names[0];
}

#if USTORE_WITH_CUDA

/** Enumerates the devices, then round-trips a pattern through pinned and device buffers on the first one, timing it. */
case_result_t cuda_devices_round_trip() {
    std::size_t failures = 0;
    st::expected<cuda_devices_t> const devices = cuda_devices_t::make();
    failures += expect(devices.has_value());
    if (!devices) return failures;
    for (std::size_t index = 0; index != devices->size(); ++index) {
        cuda_device_t const& device = (*devices)[index];
        fmt::println(
            "  device {}: {}, compute capability {}.{} ({}), {} multiprocessors, {} of {} MB free, NUMA node {}",
            device.specs.ordinal, device.specs.name, device.specs.compute_major, device.specs.compute_minor,
            generation_name(static_cast<std::uint8_t>(device.specs.generation())), device.specs.multiprocessor_count,
            device.memory.free_bytes >> 20, device.memory.total_bytes >> 20, device.numa_node);
        failures += expect(device.memory.free_bytes <= device.memory.total_bytes);
        failures += expect(device.specs.total_bytes != 0);
        failures += expect(device.specs.multiprocessor_count > 0);
    }
    if (devices->size() == 0) return case_result_t::skipped("no CUDA device", failures);

    st::expected<cuda_stream_t> const stream = cuda_stream_t::make(0);
    failures += expect(stream.has_value());
    if (!stream) return failures;

    std::size_t const count = std::size_t {1} << 20;
    st::expected<cuda_pinned_buffer<std::uint64_t>> source = cuda_pinned_buffer<std::uint64_t>::make(
        count, cuda_pinned_allocator<std::uint64_t> {});
    st::expected<cuda_device_buffer<std::uint64_t>> on_device = cuda_device_buffer<std::uint64_t>::make(
        count, cuda_device_allocator<std::uint64_t>(0));
    st::expected<cuda_pinned_buffer<std::uint64_t>> target = cuda_pinned_buffer<std::uint64_t>::make(
        count, cuda_pinned_allocator<std::uint64_t> {});
    failures += expect(source.has_value() && on_device.has_value() && target.has_value());
    if (!source || !on_device || !target) return failures;

    std::uint64_t* const values = source->data();
    for (std::size_t index = 0; index != count; ++index) values[index] = index * 0x9E3779B97F4A7C15ull;

    st::expected<cuda_stopwatch_t> stopwatch = cuda_stopwatch_t::make(0);
    st::expected<cuda_phases_t> phases = cuda_phases_t::make(*stream);
    failures += expect(stopwatch.has_value() && phases.has_value());
    if (!stopwatch || !phases) return failures;

    failures += expect(stopwatch->start(*stream) == st::status_t::success_k);
    failures += expect(phases->begin("copy in") == st::status_t::success_k);
    failures += expect(on_device->copy_in(values, count, 0, stream->handle()) == st::status_t::success_k);
    failures += expect(phases->begin("copy out") == st::status_t::success_k);
    failures += expect(on_device->copy_out(target->data(), count, 0, stream->handle()) == st::status_t::success_k);
    failures += expect(phases->end() == st::status_t::success_k);
    failures += expect(stopwatch->stop(*stream) == st::status_t::success_k);

    std::uint64_t const* const copied = target->data();
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index != count; ++index) mismatches += copied[index] != values[index];
    failures += expect_eq(mismatches, std::size_t {0});

    st::expected<double> const elapsed = stopwatch->milliseconds();
    failures += expect(elapsed.has_value() && *elapsed >= 0.0);
    failures += expect_eq(phases->size(), std::size_t {2});
    for (std::size_t index = 0; index != phases->size(); ++index) {
        st::expected<double> const phase = phases->milliseconds(index);
        failures += expect(phase.has_value() && *phase >= 0.0);
    }

    basic_vector<std::uint64_t, cuda_pinned_allocator<std::uint64_t>> pinned_vector;
    failures += expect(pinned_vector.resize(4096) == st::status_t::success_k);
    return failures;
}

/** Runs a compaction on the first CUDA device and expects the host's output back in every field. */
template <typename key_slot_type_>
case_result_t gpu_case(compaction_kind_t kind, sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps,
                       test_lsm_level<key_slot_type_> const& upper, test_lsm_level<key_slot_type_> const& lower,
                       compaction_output<key_slot_type_> const& host) {
    st::expected<std::size_t> const devices = cuda_count_devices();
    if (!devices || *devices == 0) return case_result_t::skipped("no CUDA device");
    std::size_t failures = 0;
    st::expected<cuda_compactor_t> compactor = cuda_compactor_t::make(0);
    failures += expect(compactor.has_value());
    if (!compactor) return failures;
    compaction_output<key_slot_type_> device;
    st::status_t const status = compactor->compact(kind, views_of(upper), views_of(lower), bounds, stamps,
                                                   last_write_fold_t {}, no_device_fold_t {},
                                                   located_value_reader<key_slot_type_> {}, device);
    failures += expect(status == st::status_t::success_k);

    // A status alone cannot say that the build carries no kernel image for this device, so name the CUDA error
    if (status != st::status_t::success_k) {
        cudaError_t const error = cudaGetLastError();
        fmt::println(stderr, "  CUDA reported {}: {}", cudaGetErrorName(error), cudaGetErrorString(error));
    }
    if (status == st::status_t::success_k) failures += expect_identical(host, device);
    return failures;
}

#else

/** The device round trip skips in a build without CUDA. */
case_result_t cuda_devices_round_trip() { return case_result_t::skipped("built without CUDA"); }

/** A GPU case skips in a build without CUDA. */
template <typename key_slot_type_>
case_result_t gpu_case(compaction_kind_t, sorted_run_bounds_t const&, compaction_stamps_t const&,
                       test_lsm_level<key_slot_type_> const&, test_lsm_level<key_slot_type_> const&,
                       compaction_output<key_slot_type_> const&) {
    return case_result_t::skipped("built without CUDA");
}

#endif

/** Checks one compaction of @p upper into @p lower on the host against the reference, then on the GPU against the
 *  host. */
template <typename key_slot_type_>
std::size_t report_compaction(suite_totals_t& totals, char const* host_name, char const* gpu_name,
                              compaction_kind_t kind, sorted_run_bounds_t const& bounds,
                              compaction_stamps_t const& stamps, test_lsm_level<key_slot_type_> const& upper,
                              test_lsm_level<key_slot_type_> const& lower) {
    if (!totals.selects(host_name) && !totals.selects(gpu_name)) return 0;
    std::size_t failures = 0;
    basic_vector<located_key_slot<key_slot_type_>> survivors;
    st::status_t status = reference_survivors(kind, upper, lower, stamps, survivors);
    failures += expect(status == st::status_t::success_k);
    if (failures != 0) return report(totals, host_name, failures);

    // Blobs write no change, so a merge with the newest version winning reads no value.
    auto const read_nothing = [](located_key_slot<key_slot_type_> const&, arena_allocator<byte_t>&,
                                 std::span<byte_t const>&) noexcept { return st::status_t::invalid_argument_k; };
    compaction_output<key_slot_type_> host;
    status = host_compactor_t {}.compact(kind, views_of(upper), views_of(lower), bounds, stamps, last_write_fold_t {},
                                         read_nothing, host);
    failures += expect(status == st::status_t::success_k);
    if (status == st::status_t::success_k) failures += check_compaction(kind, bounds, stamps, survivors, host);
    std::size_t const host_failures = report(totals, host_name, failures);
    return host_failures + report(totals, gpu_name, gpu_case<key_slot_type_>, kind, bounds, stamps, upper, lower, host);
}

/** The stamps a scenario compacts with: pins drawn from the stamps its levels carry, and bounds at fractions of
 *  their range, all widest when the scenario names none. */
template <typename key_slot_type_>
compaction_stamps_t scenario_stamps(scenario_t const& scenario, test_lsm_level<key_slot_type_> const& upper,
                                    test_lsm_level<key_slot_type_> const& lower, random_t& random,
                                    basic_vector<std::uint64_t>& pins) noexcept {
    compaction_stamps_t stamps;
    std::uint64_t const range = (scenario.upper_sorted_runs + 2) * stamps_per_sorted_run_k;
    auto const at = [range](std::uint64_t per_mille) noexcept {
        return per_mille >= 1000 ? ~std::uint64_t {0} : range / 1000 * per_mille;
    };
    stamps.horizon = at(scenario.horizon_per_mille);
    stamps.ceiling = at(scenario.ceiling_per_mille);
    stamps.floor = scenario.floor_per_mille != 0 ? at(scenario.floor_per_mille) : 0;
    pins.clear();
    std::size_t const total = upper.stamps.size() + lower.stamps.size();
    for (std::uint64_t pin = 0; pin != scenario.pinned && total != 0; ++pin) {
        std::size_t const drawn = static_cast<std::size_t>(random.below(total));
        std::uint64_t const stamp = drawn < upper.stamps.size() ? upper.stamps[drawn]
                                                                : lower.stamps[drawn - upper.stamps.size()];
        // A reader pins a stamp at or above the floor, since the floor is the oldest a reader can still hold
        if (stamp >= stamps.floor && pins.push_back(std::uint64_t {stamp}) != st::status_t::success_k) break;
    }
    std::sort(pins.data(), pins.data() + pins.size());
    [[maybe_unused]] st::status_t const deduplicated = pins.resize(
        static_cast<std::size_t>(std::unique(pins.data(), pins.data() + pins.size()) - pins.data()));
    stamps.pinned = pins.data();
    stamps.pinned_count = pins.size();
    return stamps;
}

/** Generates the LSM levels of @p scenario for one key slot shape, then checks the host compaction and the GPU one. */
template <typename key_slot_type_>
std::size_t run_scenario(suite_totals_t& totals, char const* shape, std::size_t scenario_index,
                         scenario_t const& scenario, options_t const& options) {
    using stored_key_t = typename key_slot_type_::stored_key_t;
    char host_name[128] {};
    char gpu_name[128] {};
    *fmt::format_to_n(host_name, sizeof(host_name) - 1, "{}.{}.host", shape, scenario.name).out = '\0';
    *fmt::format_to_n(gpu_name, sizeof(gpu_name) - 1, "{}.{}.gpu", shape, scenario.name).out = '\0';

    std::size_t failures = 0;
    random_t random = options.random.fork(scenario_index);
    basic_vector<stored_key_t> universe;
    std::uint64_t const random_keys = scenario.random_keys != 0 ? scenario.random_keys : options.large_universe.value;
    st::status_t status = make_universe(random_keys, random, universe);
    failures += expect(status == st::status_t::success_k);
    if (failures != 0) return report(totals, host_name, failures);

    std::span<stored_key_t const> const keys {universe};
    bool const overlapping = (scenario.kind == compaction_kind_t::overlapping_into_intermediate_k) |
                             (scenario.kind == compaction_kind_t::overlapping_into_bottom_k);
    // The lower level is the oldest era, and every upper run is newer than all of it
    test_lsm_level<key_slot_type_> upper;
    test_lsm_level<key_slot_type_> lower;
    status = overlapping ? add_overlapping_sorted_runs(keys, scenario, scenario.upper_sorted_runs, 1, random, upper)
                         : add_sorted_sorted_runs(keys, scenario, scenario.upper_sorted_runs, 1, random, upper);
    if (status == st::status_t::success_k && scenario.lower_sorted_runs != 0)
        status = add_sorted_sorted_runs(keys, scenario, scenario.lower_sorted_runs, 0, random, lower);
    if (status == st::status_t::success_k) status = lay_out(upper, scenario.bounds.branching);
    if (status == st::status_t::success_k) status = lay_out(lower, scenario.bounds.branching);
    failures += expect(status == st::status_t::success_k);
    if (failures != 0) return report(totals, host_name, failures);
    basic_vector<std::uint64_t> pins;
    compaction_stamps_t const stamps = scenario_stamps(scenario, upper, lower, random, pins);
    return report_compaction(totals, host_name, gpu_name, scenario.kind, scenario.bounds, stamps, upper, lower);
}

#pragma endregion Cases

#pragma region Handmade Cases

/** Keys a handmade sorted run names by letter, from @c 'a' at the least to @c 'z' at the greatest. */
inline constexpr std::size_t lettered_keys_count_k = 26;

/** The 26 lettered 64-bit keys, ascending: both ends of the type, the sign boundary, and powers of two between. */
st::status_t append_lettered_keys(basic_vector<std::uint64_t>& keys) noexcept {
    constexpr std::uint64_t ones = ~std::uint64_t {0};
    constexpr std::uint64_t sign = std::uint64_t {1} << 63;
    std::array<std::uint64_t, lettered_keys_count_k> const lettered {
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        std::uint64_t {1} << 16,
        std::uint64_t {1} << 24,
        std::uint64_t {1} << 32,
        std::uint64_t {1} << 40,
        std::uint64_t {1} << 48,
        sign - 2,
        sign - 1,
        sign,
        sign + 1,
        sign + 2,
        ones - 7,
        ones - 6,
        ones - 5,
        ones - 4,
        ones - 3,
        ones - 2,
        ones - 1,
        ones,
    };
    for (std::uint64_t const key : lettered) {
        st::status_t const status = keys.push_back(std::uint64_t {key});
        if (status != st::status_t::success_k) return status;
    }
    return st::status_t::success_k;
}

/** The 26 lettered 16-byte keys, ascending: both ends of the type, one shared high word, and the sign boundary. */
st::status_t append_lettered_keys(basic_vector<st::key128_t>& keys) noexcept {
    constexpr std::uint64_t ones = ~std::uint64_t {0};
    constexpr std::uint64_t sign = std::uint64_t {1} << 63;
    std::array<st::key128_t, lettered_keys_count_k> const lettered {{
        {0, 0},
        {0, 1},
        {0, 2},
        {0, 3},
        {0, ones - 1},
        {0, ones},
        {1, 0},
        {1, 1},
        {shared_high_word_k, 0},
        {shared_high_word_k, 1},
        {shared_high_word_k, 2},
        {shared_high_word_k, ones - 1},
        {shared_high_word_k, ones},
        {sign - 1, ones},
        {sign, 0},
        {sign, 1},
        {sign, ones},
        {ones - 2, 0},
        {ones - 1, 0},
        {ones - 1, ones},
        {ones, 0},
        {ones, 1},
        {ones, 2},
        {ones, ones - 2},
        {ones, ones - 1},
        {ones, ones},
    }};
    for (st::key128_t const& key : lettered) {
        st::status_t const status = keys.push_back(st::key128_t {key});
        if (status != st::status_t::success_k) return status;
    }
    return st::status_t::success_k;
}

/**
 *  @brief One handmade compaction, its LSM levels spelled out instead of generated.
 *
 *  A sorted run is a letter per key slot, ascending, lowercase for a stored value and uppercase for a remove delta.
 *  The sorted runs of one LSM level are separated by @c '|', so an empty field is an empty sorted run and an empty
 *  spelling is an LSM level with no sorted runs at all.
 */
struct handmade_case_t {

    /** Name printed with the result. */
    char const* name;

    /** Kind of compaction. */
    compaction_kind_t kind;

    /** The upper LSM level's sorted runs, oldest first when they overlap and ascending otherwise. */
    char const* upper;

    /** The lower LSM level's sorted runs, ascending and disjoint. */
    char const* lower;

    /** Bounds of the new sorted runs. */
    sorted_run_bounds_t bounds;
};

/** Bounds where three key slots fill a sorted run, so a handmade merge is cut into several. */
inline constexpr sorted_run_bounds_t handmade_bounds_k {3, std::uint64_t {4} << 30, 4096, 4096, 3};

/** Every handmade compaction the suite runs, for every key slot shape and key type. */
inline constexpr std::array<handmade_case_t, 14> handmade_cases_k {{
    {"duplicates_across_runs", compaction_kind_t::overlapping_into_intermediate_k, "abcde|abcde|abcde", "abcde",
     roomy_bounds_k},
    {"remove_delta_newest_kept", compaction_kind_t::overlapping_into_intermediate_k, "abcde|ABCDE", "abcde",
     roomy_bounds_k},
    {"remove_delta_newest_dropped", compaction_kind_t::overlapping_into_bottom_k, "abcde|ABCDE", "abcde",
     roomy_bounds_k},
    {"remove_delta_oldest_shadowed", compaction_kind_t::overlapping_into_bottom_k, "ABCDE|abcde", "ABCDE",
     roomy_bounds_k},
    {"remove_delta_shadows_lower", compaction_kind_t::overlapping_into_intermediate_k, "ABC", "abcde", roomy_bounds_k},
    {"one_key_newest_alive", compaction_kind_t::overlapping_into_bottom_k, "a|A|a|A|a", "A", roomy_bounds_k},
    {"one_key_newest_removed", compaction_kind_t::overlapping_into_bottom_k, "a|A|a|A", "a", roomy_bounds_k},
    {"empty_and_single_sorted_runs", compaction_kind_t::overlapping_into_intermediate_k, "|a||bcd|", "|e",
     roomy_bounds_k},
    {"every_sorted_run_empty", compaction_kind_t::overlapping_into_intermediate_k, "||", "|", roomy_bounds_k},
    {"lopsided_sorted_runs", compaction_kind_t::overlapping_into_bottom_k, "a|abcdefghijklmnopqrstuvwxyz|z|ABC",
     "abcdefghijklmnopqrstuvwxyz", roomy_bounds_k},
    {"extreme_keys_only", compaction_kind_t::overlapping_into_bottom_k, "az|AZ|az", "az", roomy_bounds_k},
    {"single_key_slot_lsm_levels", compaction_kind_t::sorted_into_bottom_k, "a", "", handmade_bounds_k},
    {"splits_at_the_capacity", compaction_kind_t::sorted_into_intermediate_k, "abcd|efgh", "ijkl|mnop",
     handmade_bounds_k},
    {"everything_removed", compaction_kind_t::sorted_into_bottom_k, "ABCD|EFGH", "IJKL", handmade_bounds_k},
}};

/** Fills @p lsm_level with the sorted runs @p spelling names, one letter of @p keys per key slot. */
template <typename key_slot_type_>
st::status_t spell_lsm_level(std::span<typename key_slot_type_::stored_key_t const> keys, char const* spelling,
                             random_t& random, test_lsm_level<key_slot_type_>& lsm_level) noexcept {
    if (*spelling == '\0') return st::status_t::success_k;
    std::uint64_t key_slots = 0;
    for (char const* letter = spelling;; ++letter) {
        if (*letter == '|' || *letter == '\0') {
            st::status_t const status = lsm_level.counts.push_back(std::uint64_t {key_slots});
            if (status != st::status_t::success_k) return status;
            key_slots = 0;
            if (*letter == '\0') return st::status_t::success_k;
            continue;
        }
        bool const removed = (*letter >= 'A') & (*letter <= 'Z');
        std::size_t const index = static_cast<std::size_t>(removed ? *letter - 'A' : *letter - 'a');
        if (index >= keys.size()) return st::status_t::invalid_argument_k;
        key_slot_state_t const state = removed ? key_slot_state_t::removed_k : key_slot_state_t::alive_k;
        st::status_t status = lsm_level.sorted.push_back(
            make_key_slot(std::type_identity<key_slot_type_> {}, keys[index], state, random));
        if (status != st::status_t::success_k) return status;
        status = lsm_level.kinds.push_back(kind_of(state));
        if (status != st::status_t::success_k) return status;
        ++key_slots;
    }
}

/** Spells out the LSM levels of @p handmade for one key slot shape, then checks the host compaction and the GPU one. */
template <typename key_slot_type_>
std::size_t run_handmade_case(suite_totals_t& totals, char const* shape, std::size_t case_index,
                              handmade_case_t const& handmade, options_t const& options) {
    using stored_key_t = typename key_slot_type_::stored_key_t;
    char host_name[128] {};
    char gpu_name[128] {};
    *fmt::format_to_n(host_name, sizeof(host_name) - 1, "{}.{}.host", shape, handmade.name).out = '\0';
    *fmt::format_to_n(gpu_name, sizeof(gpu_name) - 1, "{}.{}.gpu", shape, handmade.name).out = '\0';

    std::size_t failures = 0;
    random_t random = options.random.fork(scenarios_k.size() + case_index);
    basic_vector<stored_key_t> lettered;
    st::status_t status = append_lettered_keys(lettered);
    failures += expect(status == st::status_t::success_k);
    failures += expect_eq(lettered.size(), lettered_keys_count_k);
    if (failures != 0) return report(totals, host_name, failures);

    // The lettered keys must ascend, or a handmade sorted run would not be sorted
    std::size_t unordered = 0;
    for (std::size_t index = 1; index != lettered.size(); ++index)
        unordered += !(lettered[index - 1] < lettered[index]);
    failures += expect_eq(unordered, std::size_t {0});
    if (failures != 0) return report(totals, host_name, failures);

    std::span<stored_key_t const> const keys {lettered};
    test_lsm_level<key_slot_type_> upper;
    test_lsm_level<key_slot_type_> lower;
    status = spell_lsm_level(keys, handmade.upper, random, upper);
    if (status == st::status_t::success_k) status = spell_lsm_level(keys, handmade.lower, random, lower);
    if (status == st::status_t::success_k) status = lay_out(upper, handmade.bounds.branching);
    if (status == st::status_t::success_k) status = lay_out(lower, handmade.bounds.branching);
    failures += expect(status == st::status_t::success_k);
    if (failures != 0) return report(totals, host_name, failures);
    return report_compaction(totals, host_name, gpu_name, handmade.kind, handmade.bounds, compaction_stamps_t {}, upper,
                             lower);
}

#pragma endregion Handmade Cases

#pragma region Graph Folds

/** The key slot a graph's merges move: a vertex's key, and the size and location of its value. */
using graph_key_slot_t = variable_width_key_slot<std::uint64_t>;

/** The LSM levels of a graph collection, and the values every key slot of theirs locates. */
struct graph_levels_t {

    /** The overlapping level of change deltas, each run newer than every lower one. */
    test_lsm_level<graph_key_slot_t> upper;

    /** The sorted level below it. */
    test_lsm_level<graph_key_slot_t> lower;

    /** The values of every key slot of both levels back to back, where their locations point. */
    basic_vector<byte_t> values;

    /** Records across every change delta of both levels. */
    std::uint64_t half_edges = 0;
};

/** One merge of a graph's levels, which the suite runs on the host and on the device. */
struct graph_case_t {

    /** Name printed with the result. */
    char const* name;

    /** Kind of compaction. */
    compaction_kind_t kind;

    /** Sorted runs on the upper level, which overlap. */
    std::uint64_t upper_sorted_runs;

    /** Sorted runs on the lower level. */
    std::uint64_t lower_sorted_runs;

    /** Vertices the levels key. */
    std::uint64_t vertices;

    /** Records the change deltas carry across both levels, roughly. */
    std::uint64_t half_edges;

    /** Chance per thousand that a change delta removes every half-edge of its vertex somewhere among its records. */
    std::uint64_t vertex_removals_per_mille;

    /** Chance per thousand that a lower key slot writes a whole vertex value instead of changing it. */
    std::uint64_t bases_per_mille;

    /** Chance per thousand that a lower key slot removes the vertex value instead of changing it. */
    std::uint64_t removals_per_mille;

    /** Stamps readers pin, so some keys keep older versions that changes fold into separately. */
    std::uint64_t pinned;
};

/** The graph merges: long chains of inserts and removals, chains a removal of the vertex cuts, and chains reaching a
 *  write or a removal below them, which fold on the host beside the chains the device composes. */
inline constexpr std::array<graph_case_t, 3> graph_cases_k {{
    {"graph.chains_of_half_edges", compaction_kind_t::overlapping_into_intermediate_k, 6, 3, 4096, 200'000, 0, 0, 0, 0},
    {"graph.chains_cut_by_vertex_removals", compaction_kind_t::overlapping_into_intermediate_k, 6, 3, 2048, 50'000, 200,
     0, 0, 0},
    {"graph.chains_reaching_bases", compaction_kind_t::overlapping_into_intermediate_k, 6, 3, 2048, 50'000, 100, 300,
     150, 9},
}};

/** The order the host reads the records of every change in: as written, or reversed, which a fold must not confuse. */
enum class sequence_order_t : bool { oldest_first_k, reversed_k };

/** The identifier numbered @p number. */
graph::id_entity_t numbered_entity(std::uint64_t number) noexcept { return utilities::key_from_integer(number); }

/** A random half-edge record among few neighbors, edges and roles, so later records of a chain overwrite earlier
 *  ones. */
graph::half_edge_delta_t random_half_edge(random_t& random) noexcept {
    std::array<graph::role_t, 3> const roles {graph::edge_member_k, graph::dedge_source_k, graph::dedge_target_k};
    graph::labeled_half_edge_t half_edge;
    half_edge.neighbor_id = numbered_entity(random.below(24));
    half_edge.edge_id = numbered_entity(random.below(2));
    half_edge.edge_weight = static_cast<float>(random.below(1000)) / 8;
    half_edge.edge_label = graph::label_t {static_cast<std::int32_t>(random.below(5))};
    half_edge.role = roles[random.below(roles.size())];
    if (random.below(10) < 3)
        return graph::half_edge_delta_t::remove(half_edge.neighbor_id, half_edge.edge_id, half_edge.role);
    return graph::half_edge_delta_t::insert(half_edge);
}

/** Appends one key slot of @p vertex to the last run of @p level, its value written into @p levels' values: a chain of
 *  about @p records changes, a vertex value of inserts, or a removal. */
st::status_t add_graph_key_slot(graph_case_t const& graph_case, std::uint64_t vertex, std::uint64_t stamp,
                                std::uint64_t records, random_t& random, graph_levels_t& levels,
                                test_lsm_level<graph_key_slot_t>& level) noexcept {
    bool const lower = &level == &levels.lower;
    std::uint64_t const roll = random.below(1000);
    delta_kind_t kind = delta_kind_t::change_k;
    if (lower && roll < graph_case.bases_per_mille) kind = delta_kind_t::write_k;
    else if (lower && roll < graph_case.bases_per_mille + graph_case.removals_per_mille) kind = delta_kind_t::remove_k;

    basic_vector<graph::half_edge_delta_t> deltas;
    std::uint64_t const count = 1 + random.below(2 * records);
    for (std::uint64_t index = 0; index != count && kind != delta_kind_t::remove_k; ++index)
        if (st::status_t const status = deltas.push_back(random_half_edge(random)); status != st::status_t::success_k)
            return status;
    if (kind == delta_kind_t::change_k && random.below(1000) < graph_case.vertex_removals_per_mille)
        deltas[random.below(deltas.size())] = graph::half_edge_delta_t::remove_vertex();
    // A vertex value holds the inserts of distinct half-edges, sorted
    std::size_t kept = deltas.size();
    if (kind == delta_kind_t::write_k) {
        for (graph::half_edge_delta_t& delta : deltas) delta.operation = graph::half_edge_operation_t::insert_k;
        auto const less = [](graph::half_edge_delta_t const& first, graph::half_edge_delta_t const& second) noexcept {
            byte_t first_bytes[graph::half_edge_delta_bytes_k], second_bytes[graph::half_edge_delta_bytes_k];
            graph::save_delta(first, first_bytes);
            graph::save_delta(second, second_bytes);
            return graph::compare_half_edges(first_bytes, second_bytes) < 0;
        };
        std::sort(deltas.data(), deltas.data() + deltas.size(), less);
        kept = static_cast<std::size_t>(std::unique(deltas.data(), deltas.data() + deltas.size(),
                                                    [&](auto const& first, auto const& second) noexcept {
                                                        return !less(first, second) && !less(second, first);
                                                    }) -
                                        deltas.data());
    }
    else levels.half_edges += kept;

    graph_key_slot_t key_slot;
    key_slot.stored_key = vertex;
    key_slot.location = static_cast<std::uint32_t>(levels.values.size());
    key_slot.size = static_cast<std::uint32_t>(kept * graph::half_edge_delta_bytes_k);
    st::status_t status = levels.values.resize(levels.values.size() + key_slot.size);
    if (status != st::status_t::success_k) return status;
    for (std::size_t index = 0; index != kept; ++index)
        graph::save_delta(deltas[index],
                          levels.values.data() + key_slot.location + index * graph::half_edge_delta_bytes_k);
    if ((status = level.sorted.push_back(graph_key_slot_t {key_slot})) != st::status_t::success_k) return status;
    if ((status = level.stamps.push_back(std::uint64_t {stamp})) != st::status_t::success_k) return status;
    return level.kinds.push_back(static_cast<std::uint8_t>(kind));
}

/** Generates the levels of @p graph_case: every run keys about half the vertices, the lower level one era older than
 *  every upper run, and lays them out. */
st::status_t make_graph_levels(graph_case_t const& graph_case, sorted_run_bounds_t const& bounds, random_t& random,
                               graph_levels_t& levels) noexcept {
    std::uint64_t const runs = graph_case.upper_sorted_runs + 1;
    std::uint64_t const records = graph_case.half_edges / (graph_case.vertices * runs / 2) + 1;
    st::status_t status = st::status_t::success_k;
    for (std::uint64_t run = 0; run != graph_case.upper_sorted_runs; ++run) {
        std::size_t const before = levels.upper.sorted.size();
        for (std::uint64_t vertex = 0; vertex != graph_case.vertices && status == st::status_t::success_k; ++vertex)
            if (random.below(2) == 0)
                status = add_graph_key_slot(graph_case, vertex,
                                            (run + 1) * stamps_per_sorted_run_k + random.below(stamps_per_sorted_run_k),
                                            records, random, levels, levels.upper);
        if (status != st::status_t::success_k) return status;
        if (levels.upper.sorted.size() != before)
            if ((status = levels.upper.counts.push_back(std::uint64_t {levels.upper.sorted.size() - before})) !=
                st::status_t::success_k)
                return status;
    }
    for (std::uint64_t vertex = 0; vertex != graph_case.vertices && status == st::status_t::success_k; ++vertex)
        if (random.below(2) == 0)
            status = add_graph_key_slot(graph_case, vertex, random.below(stamps_per_sorted_run_k), records, random,
                                        levels, levels.lower);
    if (status != st::status_t::success_k) return status;
    std::uint64_t const total = levels.lower.sorted.size();
    std::uint64_t const chunk = divide_round_up(total, graph_case.lower_sorted_runs);
    for (std::uint64_t offset = 0; offset < total; offset += chunk)
        if ((status = levels.lower.counts.push_back(std::uint64_t {chunk < total - offset ? chunk : total - offset})) !=
            st::status_t::success_k)
            return status;
    if ((status = lay_out(levels.upper, bounds.branching)) != st::status_t::success_k) return status;
    return lay_out(levels.lower, bounds.branching);
}

/** Pins @p count stamps drawn from the levels, as readers would. */
compaction_stamps_t graph_stamps(graph_levels_t const& levels, std::uint64_t count, random_t& random,
                                 basic_vector<std::uint64_t>& pins) noexcept {
    pins.clear();
    std::size_t const total = levels.upper.stamps.size() + levels.lower.stamps.size();
    for (std::uint64_t pin = 0; pin != count && total != 0; ++pin) {
        std::size_t const drawn = static_cast<std::size_t>(random.below(total));
        std::uint64_t const stamp = drawn < levels.upper.stamps.size()
                                        ? levels.upper.stamps[drawn]
                                        : levels.lower.stamps[drawn - levels.upper.stamps.size()];
        if (pins.push_back(std::uint64_t {stamp}) != st::status_t::success_k) break;
    }
    std::sort(pins.data(), pins.data() + pins.size());
    [[maybe_unused]] st::status_t const deduplicated = pins.resize(
        static_cast<std::size_t>(std::unique(pins.data(), pins.data() + pins.size()) - pins.data()));
    compaction_stamps_t stamps;
    stamps.pinned = pins.data();
    stamps.pinned_count = pins.size();
    return stamps;
}

#if USTORE_WITH_CUDA

/** Milliseconds since @p start. */
double milliseconds_since(std::chrono::steady_clock::time_point start) noexcept {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

/** Merges the levels of @p graph_case on the host and on the first CUDA device, composing its chains with the graph's
 *  device fold, and expects identical outputs; with @p order reversed, the host reads every change's records in
 *  reverse, which the device output must expose. */
case_result_t graph_gpu_case(graph_case_t const& graph_case, std::size_t case_index, sequence_order_t order,
                             options_t const& options) {
    st::expected<std::size_t> const devices = cuda_count_devices();
    if (!devices || *devices == 0) return case_result_t::skipped("no CUDA device");
    std::size_t failures = 0;
    random_t random = options.random.fork(case_index);
    graph_levels_t levels;
    failures += expect(make_graph_levels(graph_case, roomy_bounds_k, random, levels) == st::status_t::success_k);
    if (failures != 0) return failures;
    basic_vector<std::uint64_t> pins;
    compaction_stamps_t const stamps = graph_stamps(levels, graph_case.pinned, random, pins);

    using record_t = located_key_slot<graph_key_slot_t>;
    auto const read_value = [&levels](record_t const& record, arena_allocator<byte_t>&,
                                      std::span<byte_t const>& bytes) noexcept {
        bytes = {levels.values.data() + record.key_slot.location, record.key_slot.size};
        return st::status_t::success_k;
    };
    auto const read_reversed = [&levels](record_t const& record, arena_allocator<byte_t>& arena,
                                         std::span<byte_t const>& bytes) noexcept {
        std::size_t const size = record.key_slot.size;
        byte_t* const into = size != 0 ? arena.allocate(size) : nullptr;
        if (size != 0 && into == nullptr) return st::status_t::out_of_memory_heap_k;
        std::size_t const count = size / graph::half_edge_delta_bytes_k;
        for (std::size_t index = 0; index != count; ++index)
            std::memcpy(into + (count - 1 - index) * graph::half_edge_delta_bytes_k,
                        levels.values.data() + record.key_slot.location + index * graph::half_edge_delta_bytes_k,
                        graph::half_edge_delta_bytes_k);
        bytes = {into, size};
        return st::status_t::success_k;
    };

    compaction_output<graph_key_slot_t> host;
    std::chrono::steady_clock::time_point const host_start = std::chrono::steady_clock::now();
    st::status_t status = order == sequence_order_t::reversed_k
                              ? host_compactor_t {}.compact(graph_case.kind, views_of(levels.upper),
                                                            views_of(levels.lower), roomy_bounds_k, stamps,
                                                            graph::adjacency_fold_t {}, read_reversed, host)
                              : host_compactor_t {}.compact(graph_case.kind, views_of(levels.upper),
                                                            views_of(levels.lower), roomy_bounds_k, stamps,
                                                            graph::adjacency_fold_t {}, read_value, host);
    double const host_milliseconds = milliseconds_since(host_start);
    failures += expect(status == st::status_t::success_k);
    failures += expect(!host.folded.empty());

    st::expected<cuda_compactor_t> compactor = cuda_compactor_t::make(0);
    failures += expect(compactor.has_value());
    if (failures != 0) return failures;
    compaction_output<graph_key_slot_t> device;
    std::chrono::steady_clock::time_point const device_start = std::chrono::steady_clock::now();
    status = compactor->compact(graph_case.kind, views_of(levels.upper), views_of(levels.lower), roomy_bounds_k, stamps,
                                graph::adjacency_fold_t {}, graph::vram_nvidia_adjacency_fold_t {}, read_value, device);
    double const device_milliseconds = milliseconds_since(device_start);
    failures += expect(status == st::status_t::success_k);
    if (status != st::status_t::success_k) return failures;
    if (order == sequence_order_t::reversed_k) failures += expect(differences(host, device) != 0);
    else failures += expect_identical(host, device);
    fmt::println("  {} half-edges: host {:.1f} ms, {:.1f} M/s; device {:.1f} ms, {:.1f} M/s", levels.half_edges,
                 host_milliseconds, levels.half_edges / host_milliseconds / 1e3, device_milliseconds,
                 levels.half_edges / device_milliseconds / 1e3);
    return failures;
}

#else

/** A graph's device fold skips in a build without CUDA. */
case_result_t graph_gpu_case(graph_case_t const&, std::size_t, sequence_order_t, options_t const&) {
    return case_result_t::skipped("built without CUDA");
}

#endif

#pragma endregion Graph Folds

int main(int argc, char** argv) {
    suite_options_t suite;
    options_t options;
    std::array<flag_t, 1> const flags {{
        {"--large-universe", "count", "random keys of the large cases, which cross the GPU threshold",
         &options.large_universe},
    }};
    if (argument_status_t const status = parse_suite_options(argc, argv, suite, flags);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    options.random = suite.random();
    suite_totals_t totals = suite.totals();

    report(totals, "compaction.uuid_orders_like_memcmp", uuid_orders_like_memcmp, options);
    report(totals, "compaction.sorted_run_index_layout_matches_builder", sorted_run_index_layout_matches_builder);
    report(totals, "compaction.survivor_first_orders_the_survivor_first", survivor_first_orders_the_survivor_first);
    report(totals, "compaction.survives_follows_the_kind", survives_follows_the_kind);
    report(totals, "compaction.plan_sorted_runs_cuts_at_the_bounds", plan_sorted_runs_cuts_at_the_bounds);
    report(totals, "compaction.survives_keeps_what_readers_read", survives_keeps_what_readers_read);
    report(totals, "cuda.devices_round_trip", cuda_devices_round_trip);
    for (std::size_t index = 0; index != handmade_cases_k.size(); ++index) {
        handmade_case_t const& handmade = handmade_cases_k[index];
        run_handmade_case<inline_key_slot<std::uint64_t>>(totals, "inline_u64", index, handmade, options);
        run_handmade_case<fixed_width_key_slot<std::uint64_t>>(totals, "fixed_u64", index, handmade, options);
        run_handmade_case<variable_width_key_slot<std::uint64_t>>(totals, "variable_width_u64", index, handmade,
                                                                  options);
        run_handmade_case<inline_key_slot<st::key128_t>>(totals, "inline_uuid", index, handmade, options);
        run_handmade_case<fixed_width_key_slot<st::key128_t>>(totals, "fixed_uuid", index, handmade, options);
        run_handmade_case<variable_width_key_slot<st::key128_t>>(totals, "variable_width_uuid", index, handmade,
                                                                 options);
    }
    for (std::size_t index = 0; index != scenarios_k.size(); ++index) {
        scenario_t const& scenario = scenarios_k[index];
        run_scenario<inline_key_slot<std::uint64_t>>(totals, "inline_u64", index, scenario, options);
        run_scenario<fixed_width_key_slot<std::uint64_t>>(totals, "fixed_u64", index, scenario, options);
        run_scenario<variable_width_key_slot<std::uint64_t>>(totals, "variable_width_u64", index, scenario, options);
        run_scenario<inline_key_slot<st::key128_t>>(totals, "inline_uuid", index, scenario, options);
        run_scenario<fixed_width_key_slot<st::key128_t>>(totals, "fixed_uuid", index, scenario, options);
        run_scenario<variable_width_key_slot<st::key128_t>>(totals, "variable_width_uuid", index, scenario, options);
    }
    for (std::size_t index = 0; index != graph_cases_k.size(); ++index)
        report(totals, graph_cases_k[index].name, graph_gpu_case, graph_cases_k[index], index,
               sequence_order_t::oldest_first_k, options);
    // Reading every change's records in reverse must show in the output, or parity proves nothing about the order
    report(totals, "graph.reversed_sequences_differ", graph_gpu_case, graph_cases_k[1], std::size_t {1},
           sequence_order_t::reversed_k, options);
    return suite_exit_code(totals);
}
