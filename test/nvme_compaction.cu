/**
 *  @file test/nvme_compaction.cu
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief Tests host and GPU compaction against a reference merge and against each other, for 64-bit and 16-byte keys.
 *
 *  The suite compiles as CUDA and as plain C++: the CUDA build adds the device cases, which the host build reports
 *  as skipped. LSM levels are generated from a seed, with overlapping and sorted sorted runs, tombstones, and keys
 *  crowded at the edges a comparator gets wrong.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcmp`

#include <algorithm>   // `std::sort`, `std::unique`
#include <array>       // `std::array`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::type_identity`

#include <fmt/format.h>

#include <smashtable/basic_vector.hpp>
#include <smashtable/shared.hpp>

#include <ustore/types.hpp>

#include "cuda.hpp" // `host_device_m`, so CUDA kernels call these functions too
#include "test.hpp"
#include "nvme/compaction.cuh"

using namespace unum::ustore;
using namespace unum::ustore::nvme;
using st::basic_vector;
using st::expected;
using st::status_t;

#pragma region Key Slots

/** Location of an tombstoned variable-size key slot. */
inline constexpr std::uint32_t tombstone_location_k = std::numeric_limits<std::uint32_t>::max();

/** Location of an empty variable-size key slot. */
inline constexpr std::uint32_t empty_location_k = tombstone_location_k - 1;

/** Stored values of up to eight bytes inside the key slot, laid out as UStoreNVMe's inline 8-byte key slots are. */
template <typename stored_key_type_>
struct inline_key_slot {
    using stored_key_t = stored_key_type_;

    stored_key_t stored_key = limit<stored_key_t>::max();
    std::uint32_t size = 0;
    std::uint8_t data[8] = {};

    host_device_m constexpr bool is_tombstone() const noexcept { return size == 0; }
};

/** Stored values of one width in the stored values section, laid out as UStoreNVMe's fixed-size key slots are. */
template <typename stored_key_type_>
struct fixed_width_key_slot {
    using stored_key_t = stored_key_type_;

    stored_key_t stored_key = limit<stored_key_t>::max();
    std::uint32_t size = 0;
    std::uint32_t location = 0;

    host_device_m constexpr bool is_tombstone() const noexcept { return size == 0; }
    host_device_m constexpr bool is_stored_value_empty() const noexcept { return size == 0; }
};

/** Stored values of any width in the stored values section, laid out as UStoreNVMe's variable-size key slots are. */
template <typename stored_key_type_>
struct variable_width_key_slot {
    using stored_key_t = stored_key_type_;

    stored_key_t stored_key = limit<stored_key_t>::max();
    std::uint32_t size = 0;
    std::uint32_t location = 0;

    host_device_m constexpr bool is_tombstone() const noexcept { return location == tombstone_location_k; }
    host_device_m constexpr bool is_stored_value_empty() const noexcept {
        return (location == tombstone_location_k) | (location == empty_location_k);
    }
};

/** Whether a generated key slot holds a stored value or is a tombstone. */
enum class key_slot_state_t : bool {

    /** The key slot holds a stored value. */
    alive_k,

    /** The key slot is a tombstone. */
    tombstone_k,
};

/** A deterministic generator, one per case, so a seed always builds the same LSM levels. */
struct splitmix_t {

    /** The advancing state. */
    std::uint64_t state = 0;

    /** The next 64 random bits. */
    std::uint64_t next() noexcept {
        std::uint64_t mixed = (state += 0x9E3779B97F4A7C15ull);
        mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
        mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
        return mixed ^ (mixed >> 31);
    }

    /** A random value below @p bound. */
    std::uint64_t below(std::uint64_t bound) noexcept { return next() % bound; }
};

/** An inline key slot of @p key with random inline bytes. */
template <typename stored_key_type_>
inline_key_slot<stored_key_type_> make_key_slot(std::type_identity<inline_key_slot<stored_key_type_>>,
                                                stored_key_type_ const& key, key_slot_state_t state,
                                                splitmix_t& random) noexcept {
    inline_key_slot<stored_key_type_> key_slot;
    key_slot.stored_key = key;
    if (state == key_slot_state_t::tombstone_k) return key_slot;
    key_slot.size = static_cast<std::uint32_t>(1 + random.below(8));
    for (std::uint8_t& byte : key_slot.data) byte = static_cast<std::uint8_t>(random.next());
    return key_slot;
}

/** A fixed-size key slot of @p key at a random old location. */
template <typename stored_key_type_>
fixed_width_key_slot<stored_key_type_> make_key_slot(std::type_identity<fixed_width_key_slot<stored_key_type_>>,
                                                     stored_key_type_ const& key, key_slot_state_t state,
                                                     splitmix_t& random) noexcept {
    fixed_width_key_slot<stored_key_type_> key_slot;
    key_slot.stored_key = key;
    key_slot.size = state == key_slot_state_t::tombstone_k ? 0 : 24;
    key_slot.location = static_cast<std::uint32_t>(random.below(std::uint64_t {1} << 30));
    return key_slot;
}

/** A variable-size key slot of @p key, empty once in 16 when alive. */
template <typename stored_key_type_>
variable_width_key_slot<stored_key_type_> make_key_slot(std::type_identity<variable_width_key_slot<stored_key_type_>>,
                                                        stored_key_type_ const& key, key_slot_state_t state,
                                                        splitmix_t& random) noexcept {
    variable_width_key_slot<stored_key_type_> key_slot;
    key_slot.stored_key = key;
    if (state == key_slot_state_t::tombstone_k) key_slot.location = tombstone_location_k;
    else if (random.below(16) == 0) key_slot.location = empty_location_k;
    else {
        key_slot.size = static_cast<std::uint32_t>(1 + random.below(200));
        key_slot.location = static_cast<std::uint32_t>(random.below(std::uint64_t {1} << 30));
    }
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
status_t append_adversarial_keys(basic_vector<std::uint64_t>& keys) noexcept {
    constexpr std::uint64_t ones = ~std::uint64_t {0};
    constexpr std::uint64_t sign = std::uint64_t {1} << 63;
    std::array<std::uint64_t, 9> const edges {0, 1, 2, ones, ones - 1, ones - 2, sign - 1, sign, sign + 1};
    for (std::uint64_t const edge : edges) {
        status_t const status = keys.push_back(std::uint64_t {edge});
        if (status != status_t::success_k) return status;
    }
    for (std::uint64_t identity = 0; identity != 1000; ++identity) {
        status_t const status = keys.push_back(std::uint64_t {identity});
        if (status != status_t::success_k) return status;
    }
    return status_t::success_k;
}

/** Appends 16-byte keys a comparator most often gets wrong: all zeros, all ones, equal high words and integer
 * identities. */
status_t append_adversarial_keys(basic_vector<uuid_stored_key_t>& keys) noexcept {
    constexpr std::uint64_t ones = ~std::uint64_t {0};
    constexpr std::uint64_t sign = std::uint64_t {1} << 63;
    std::array<uuid_stored_key_t, 10> const edges {{{0, 0},
                                                    {0, 1},
                                                    {1, 0},
                                                    {0, ones},
                                                    {ones, 0},
                                                    {ones, ones},
                                                    {ones, ones - 1},
                                                    {sign, 0},
                                                    {sign - 1, ones},
                                                    {shared_high_word_k, ones}}};
    for (uuid_stored_key_t const& edge : edges) {
        status_t const status = keys.push_back(uuid_stored_key_t {edge});
        if (status != status_t::success_k) return status;
    }
    for (std::uint64_t low = 0; low != 500; ++low) {
        status_t status = keys.push_back(uuid_stored_key_t {shared_high_word_k, low});
        if (status != status_t::success_k) return status;
        status = keys.push_back(uuid_stored_key_t {shared_high_word_k, ones - low});
        if (status != status_t::success_k) return status;
    }
    for (std::uint64_t identity = 0; identity != 1000; ++identity) {
        status_t const status = keys.push_back(uuid_stored_key_t {0, identity});
        if (status != status_t::success_k) return status;
    }
    return status_t::success_k;
}

/** A random 64-bit key. */
std::uint64_t random_key(splitmix_t& random, std::type_identity<std::uint64_t>) noexcept { return random.next(); }

/** A random 16-byte key, a quarter of them sharing one high word. */
uuid_stored_key_t random_key(splitmix_t& random, std::type_identity<uuid_stored_key_t>) noexcept {
    std::uint64_t const high = random.below(4) == 0 ? shared_high_word_k : random.next();
    return {high, random.next()};
}

/** Fills @p universe with the adversarial keys and @p random_keys random ones, sorted and unique. */
template <typename stored_key_type_>
status_t make_universe(std::uint64_t random_keys, splitmix_t& random, basic_vector<stored_key_type_>& universe) {
    universe.clear();
    status_t status = append_adversarial_keys(universe);
    if (status != status_t::success_k) return status;
    for (std::uint64_t index = 0; index != random_keys; ++index) {
        status = universe.push_back(random_key(random, std::type_identity<stored_key_type_> {}));
        if (status != status_t::success_k) return status;
    }
    stored_key_type_* const begin = universe.data();
    std::sort(begin, begin + universe.size());
    return universe.resize(static_cast<std::size_t>(std::unique(begin, begin + universe.size()) - begin));
}

#pragma endregion Keys

#pragma region LSM Levels

/** One compaction the suite generates LSM levels for. */
struct scenario_t {

    /** Name printed with the verdict. */
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

    /** Chance per thousand that a picked key slot is tombstoned. */
    std::uint64_t tombstones_per_mille;

    /** Bounds of the new sorted runs. */
    sorted_run_bounds_t bounds;
};

/** An LSM level under test: each sorted run's sorted key slots, and its cells once laid out. */
template <typename key_slot_type_>
struct test_lsm_level {

    /** Every sorted run's key slots in ascending order, one sorted run after another. */
    basic_vector<key_slot_type_> sorted;

    /** Key slots in each sorted run. */
    basic_vector<std::uint64_t> counts;

    /** Every sorted run's cells, one sorted run after another. */
    basic_vector<key_slot_type_> cells;

    /** A view per sorted run into @c cells. */
    basic_vector<sorted_run_index_view<key_slot_type_>> views;
};

/** The views of an LSM level's sorted runs, as a compaction takes them. */
template <typename key_slot_type_>
std::span<sorted_run_index_view<key_slot_type_> const> views_of(
    test_lsm_level<key_slot_type_> const& lsm_level) noexcept {
    return std::span<sorted_run_index_view<key_slot_type_> const>(lsm_level.views.data(), lsm_level.views.size());
}

/** Assigns sorted positions to a sorted run index's cells the way the B-tree builder fills sorted runs, answering the
 * next position. */
std::uint64_t assign_sorted_positions(std::uint64_t* positions, std::uint64_t rows_count, std::uint64_t branching,
                                      std::uint64_t row, std::uint64_t next) noexcept {
    if (row >= rows_count) return next;
    for (std::uint64_t slot = 0; slot != branching; ++slot) {
        next = assign_sorted_positions(positions, rows_count, branching, row * (branching + 1) + slot + 1, next);
        positions[row * branching + slot] = next++;
    }
    return assign_sorted_positions(positions, rows_count, branching, row * (branching + 1) + branching + 1, next);
}

/** Picks each key of @p keys with the scenario's density, appending its key slot to @p key_slots. */
template <typename key_slot_type_>
status_t pick_key_slots(std::span<typename key_slot_type_::stored_key_t const> keys, scenario_t const& scenario,
                        splitmix_t& random, basic_vector<key_slot_type_>& key_slots) noexcept {
    for (std::size_t index = 0; index != keys.size(); ++index) {
        if (random.below(1000) >= scenario.density_per_mille) continue;
        key_slot_state_t const state = random.below(1000) < scenario.tombstones_per_mille
                                           ? key_slot_state_t::tombstone_k
                                           : key_slot_state_t::alive_k;
        status_t const status = key_slots.push_back(
            make_key_slot(std::type_identity<key_slot_type_> {}, keys[index], state, random));
        if (status != status_t::success_k) return status;
    }
    return status_t::success_k;
}

/** Adds @p sorted_runs overlapping sorted runs, oldest first, each picking its own keys. */
template <typename key_slot_type_>
status_t add_overlapping_sorted_runs(std::span<typename key_slot_type_::stored_key_t const> keys,
                                     scenario_t const& scenario, std::uint64_t sorted_runs, splitmix_t& random,
                                     test_lsm_level<key_slot_type_>& lsm_level) noexcept {
    for (std::uint64_t sorted_run = 0; sorted_run != sorted_runs; ++sorted_run) {
        std::size_t const before = lsm_level.sorted.size();
        status_t status = pick_key_slots<key_slot_type_>(keys, scenario, random, lsm_level.sorted);
        if (status != status_t::success_k) return status;
        std::uint64_t const count = lsm_level.sorted.size() - before;
        if (count == 0) continue;
        status = lsm_level.counts.push_back(std::uint64_t {count});
        if (status != status_t::success_k) return status;
    }
    return status_t::success_k;
}

/** Adds one sorted LSM level's keys, cut into @p sorted_runs disjoint ascending sorted runs. */
template <typename key_slot_type_>
status_t add_sorted_sorted_runs(std::span<typename key_slot_type_::stored_key_t const> keys, scenario_t const& scenario,
                                std::uint64_t sorted_runs, splitmix_t& random,
                                test_lsm_level<key_slot_type_>& lsm_level) noexcept {
    status_t status = pick_key_slots<key_slot_type_>(keys, scenario, random, lsm_level.sorted);
    if (status != status_t::success_k) return status;
    std::uint64_t const total = lsm_level.sorted.size();
    if (total == 0) return status_t::success_k;
    std::uint64_t const chunk = (total + sorted_runs - 1) / sorted_runs;
    for (std::uint64_t offset = 0; offset < total; offset += chunk) {
        status = lsm_level.counts.push_back(std::uint64_t {chunk < total - offset ? chunk : total - offset});
        if (status != status_t::success_k) return status;
    }
    return status_t::success_k;
}

/** Lays every sorted run of @p lsm_level into sorted run index cells, filling padding with poison. */
template <typename key_slot_type_>
status_t lay_out(test_lsm_level<key_slot_type_>& lsm_level, std::uint64_t branching) noexcept {
    std::uint64_t cells_count = 0;
    for (std::size_t index = 0; index != lsm_level.counts.size(); ++index)
        cells_count += round_up(lsm_level.counts[index], branching);
    status_t status = lsm_level.cells.resize(cells_count);
    if (status != status_t::success_k) return status;
    status = lsm_level.views.resize(lsm_level.counts.size());
    if (status != status_t::success_k) return status;

    basic_vector<std::uint64_t> positions;
    key_slot_type_ const poison = poison_key_slot<key_slot_type_>();
    std::uint64_t first = 0;
    std::uint64_t first_cell = 0;
    for (std::size_t sorted_run = 0; sorted_run != lsm_level.counts.size(); ++sorted_run) {
        std::uint64_t const count = lsm_level.counts[sorted_run];
        std::uint64_t const rows = (count + branching - 1) / branching;
        status = positions.resize(rows * branching);
        if (status != status_t::success_k) return status;
        [[maybe_unused]] std::uint64_t const assigned = assign_sorted_positions(positions.data(), rows, branching, 0,
                                                                                0);
        for (std::uint64_t cell = 0; cell != rows * branching; ++cell)
            lsm_level.cells[first_cell + cell] = positions[cell] < count ? lsm_level.sorted[first + positions[cell]]
                                                                         : poison;
        lsm_level.views[sorted_run] = sorted_run_index_view<key_slot_type_> {lsm_level.cells.data() + first_cell,
                                                                             count};
        first += count;
        first_cell += rows * branching;
    }
    return status_t::success_k;
}

/** The survivors a compaction must keep, found by sorting every key slot by stored key and then by survival rank. */
template <typename key_slot_type_>
status_t reference_survivors(compaction_kind_t kind, test_lsm_level<key_slot_type_> const& upper,
                             test_lsm_level<key_slot_type_> const& lower,
                             basic_vector<located_key_slot<key_slot_type_>>& survivors) noexcept {
    using record_t = located_key_slot<key_slot_type_>;
    basic_vector<record_t> all;
    status_t status = all.resize(upper.sorted.size() + lower.sorted.size());
    if (status != status_t::success_k) return status;

    std::size_t written = 0;
    std::uint32_t sorted_run = 0;
    std::array<test_lsm_level<key_slot_type_> const*, 2> const lsm_levels {&upper, &lower};
    for (test_lsm_level<key_slot_type_> const* lsm_level : lsm_levels) {
        std::uint64_t first = 0;
        for (std::size_t index = 0; index != lsm_level->counts.size(); ++index, ++sorted_run) {
            for (std::uint64_t position = 0; position != lsm_level->counts[index]; ++position)
                all[written++] = record_t {lsm_level->sorted[first + position], sorted_run};
            first += lsm_level->counts[index];
        }
    }

    std::uint64_t const upper_count = upper.counts.size();
    auto const rank = [upper_count](std::uint32_t index) noexcept {
        return index < upper_count ? upper_count - 1 - index : std::uint64_t {index};
    };
    std::sort(all.data(), all.data() + all.size(), [&rank](record_t const& first, record_t const& second) noexcept {
        if (first.key_slot.stored_key < second.key_slot.stored_key) return true;
        if (second.key_slot.stored_key < first.key_slot.stored_key) return false;
        return rank(first.sorted_run) < rank(second.sorted_run);
    });

    bool const drops_tombstones = (kind == compaction_kind_t::overlapping_into_bottom_k) |
                                  (kind == compaction_kind_t::sorted_into_bottom_k);
    survivors.clear();
    for (std::size_t index = 0; index != all.size(); ++index) {
        if (index != 0 && all[index - 1].key_slot.stored_key == all[index].key_slot.stored_key) continue;
        if (drops_tombstones && all[index].key_slot.is_tombstone()) continue;
        status = survivors.push_back(record_t {all[index]});
        if (status != status_t::success_k) return status;
    }
    return status_t::success_k;
}

#pragma endregion LSM Levels

#pragma region Checks

/** Checks a compaction's output against the reference survivors: order, bounds, padding, locations and migrations. */
template <typename key_slot_type_>
std::size_t check_compaction(compaction_kind_t kind, sorted_run_bounds_t const& bounds,
                             basic_vector<located_key_slot<key_slot_type_>> const& wanted_survivors,
                             compaction_output<key_slot_type_> const& output) {
    using stored_key_t = typename key_slot_type_::stored_key_t;
    std::size_t failures = 0;

    std::uint64_t key_slots_count = 0;
    std::uint64_t cells_count = 0;
    for (std::size_t index = 0; index != output.sorted_runs.size(); ++index) {
        key_slots_count += output.sorted_runs[index].key_slots_count;
        cells_count += round_up(output.sorted_runs[index].key_slots_count, bounds.branching);
    }
    failures += expect_eq(key_slots_count, std::uint64_t {wanted_survivors.size()});
    failures += expect_eq(output.cells.size(), std::size_t {cells_count});
    failures += expect_eq(output.migrations.size(),
                          locates_stored_values_concept<key_slot_type_> ? key_slots_count : 0u);
    if (failures != 0) return failures;

    bool const drops_tombstones = (kind == compaction_kind_t::overlapping_into_bottom_k) |
                                  (kind == compaction_kind_t::sorted_into_bottom_k);
    std::size_t empty_sorted_runs = 0;
    std::size_t oversized_sorted_runs = 0;
    std::size_t unfilled_sorted_runs = 0;
    std::size_t wrong_bytes = 0;
    std::size_t wrong_padding = 0;
    std::size_t wrong_key_slots = 0;
    std::size_t wrong_migrations = 0;
    std::size_t unordered = 0;
    std::size_t tombstones = 0;

    basic_vector<std::uint64_t> positions;
    basic_vector<std::uint64_t> cell_of;
    stored_key_t previous {};
    std::uint64_t first = 0;
    std::uint64_t first_cell = 0;
    for (std::size_t index = 0; index != output.sorted_runs.size(); ++index) {
        compacted_sorted_run_t const& sorted_run = output.sorted_runs[index];
        std::uint64_t const count = sorted_run.key_slots_count;
        std::uint64_t const rows = (count + bounds.branching - 1) / bounds.branching;
        std::uint64_t const cells = rows * bounds.branching;
        empty_sorted_runs += count == 0;
        oversized_sorted_runs += count > bounds.key_slots_capacity;
        oversized_sorted_runs += (count > 1) &&
                                 sorted_run_file_bytes<key_slot_type_>(bounds, count, sorted_run.stored_values_bytes) >
                                     bounds.file_bytes_limit;
        if (index + 1 != output.sorted_runs.size() && count != bounds.key_slots_capacity) {
            std::uint64_t const next_bytes = stored_value_bytes(wanted_survivors[first + count].key_slot);
            unfilled_sorted_runs += sorted_run_file_bytes<key_slot_type_>(
                                        bounds, count + 1, sorted_run.stored_values_bytes + next_bytes) <=
                                    bounds.file_bytes_limit;
        }
        if (count == 0) continue;

        if (positions.resize(cells) != status_t::success_k || cell_of.resize(cells) != status_t::success_k)
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
                if (!wanted_key_slot.is_stored_value_empty())
                    wanted_key_slot.location = static_cast<std::uint32_t>(section_offset + running);
                stored_value_migration_t const& migration = output.migrations[first + position];
                wrong_migrations += migration.size != stored_value_bytes(wanted.key_slot) ||
                                    migration.old_location != wanted.key_slot.location ||
                                    migration.old_sorted_run != wanted.sorted_run;
            }
            wrong_key_slots += !same_key_slot(got, wanted_key_slot);
            running += stored_value_bytes(wanted.key_slot);
            tombstones += drops_tombstones && got.is_tombstone();
            if (first + position != 0) unordered += !(previous < got.stored_key);
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
    failures += expect_eq(tombstones, std::size_t {0});
    return failures;
}

/** Expects two compactions of the same LSM levels to agree in every sorted run, cell field and migration. */
template <typename key_slot_type_>
std::size_t expect_identical(compaction_output<key_slot_type_> const& host,
                             compaction_output<key_slot_type_> const& device) {
    std::size_t failures = 0;
    failures += expect_eq(device.sorted_runs.size(), host.sorted_runs.size());
    failures += expect_eq(device.cells.size(), host.cells.size());
    failures += expect_eq(device.migrations.size(), host.migrations.size());
    if (failures != 0) return failures;

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
    failures += expect_eq(mismatches, std::size_t {0});
    return failures;
}

#pragma endregion Checks

#pragma region Cases

/** Flags of this suite, with their defaults. */
struct options_t {

    /** Seed of every generated LSM level. */
    count_t seed {42};

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

/** Every compaction the suite runs, for every key slot shape and key type. */
inline constexpr std::array<scenario_t, 9> scenarios_k {{
    {"overlapping_into_intermediate", compaction_kind_t::overlapping_into_intermediate_k, 6, 4, 20'000, 300, 100,
     roomy_bounds_k},
    {"overlapping_into_bottom", compaction_kind_t::overlapping_into_bottom_k, 6, 4, 20'000, 300, 100, tight_bounds_k},
    {"overlapping_into_empty_lsm_level", compaction_kind_t::overlapping_into_intermediate_k, 3, 0, 10'000, 400, 100,
     roomy_bounds_k},
    {"sorted_into_intermediate", compaction_kind_t::sorted_into_intermediate_k, 5, 5, 20'000, 400, 100, tight_bounds_k},
    {"sorted_into_bottom", compaction_kind_t::sorted_into_bottom_k, 5, 5, 20'000, 400, 100, roomy_bounds_k},
    {"sorted_into_new_bottom", compaction_kind_t::sorted_into_bottom_k, 4, 0, 20'000, 400, 150, roomy_bounds_k},
    {"narrow_sorted_run_indexes", compaction_kind_t::overlapping_into_bottom_k, 3, 2, 2'000, 400, 100, narrow_bounds_k},
    {"everything_tombstoned_into_bottom", compaction_kind_t::sorted_into_bottom_k, 3, 3, 5'000, 300, 1000,
     roomy_bounds_k},
    {"large_overlapping_into_intermediate", compaction_kind_t::overlapping_into_intermediate_k, 4, 3, 0, 500, 50,
     large_bounds_k},
}};

/** Checks that 16-byte keys order and round-trip exactly as their bytes compare with @c memcmp. */
std::size_t uuid_orders_like_memcmp(options_t const& options) {
    std::size_t failures = 0;
    std::size_t mismatches = 0;
    splitmix_t random {options.seed.value};
    for (std::size_t round = 0; round != 100'000; ++round) {
        std::uint8_t first_bytes[16];
        std::uint8_t second_bytes[16];
        std::uint64_t const shared = random.below(17);
        for (std::size_t index = 0; index != 16; ++index) {
            first_bytes[index] = static_cast<std::uint8_t>(random.next());
            second_bytes[index] = index < shared ? first_bytes[index] : static_cast<std::uint8_t>(random.next());
        }
        uuid_stored_key_t const first = uuid_stored_key_t::from_bytes(first_bytes);
        uuid_stored_key_t const second = uuid_stored_key_t::from_bytes(second_bytes);
        int const order = std::memcmp(first_bytes, second_bytes, sizeof(first_bytes));
        mismatches += (order < 0) != (first < second);
        mismatches += (order > 0) != (second < first);
        mismatches += (order == 0) != (first == second);
        std::uint8_t round_trip[16];
        first.to_bytes(round_trip);
        mismatches += std::memcmp(round_trip, first_bytes, sizeof(first_bytes)) != 0;
    }
    std::uint8_t ones[16];
    limit<uuid_stored_key_t>::max().to_bytes(ones);
    for (std::uint8_t const byte : ones) mismatches += byte != 0xFF;
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
        std::uint64_t const rows = (count + branching - 1) / branching;
        if (layout.cells_count() != rows * branching || positions.resize(rows * branching) != status_t::success_k) {
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

#if defined(__CUDACC__)

/** Enumerates the devices, then round-trips a pattern through pinned and device buffers on the first one, timing it. */
std::size_t report_cuda_devices_case() {
    char const* const name = "cuda.devices_round_trip";
    std::size_t failures = 0;
    expected<cuda_devices_t> const devices = cuda_devices_t::make();
    failures += expect(devices.has_value());
    if (!devices) return report(name, failures);
    for (std::size_t index = 0; index != devices->size(); ++index) {
        cuda_device_t const& device = (*devices)[index];
        fmt::println(
            "  device {}: {}, compute capability {}.{} ({}), {} multiprocessors, {} of {} MiB free, NUMA node {}",
            device.specs.ordinal, device.specs.name, device.specs.compute_major, device.specs.compute_minor,
            generation_name(static_cast<std::uint8_t>(device.specs.generation())), device.specs.multiprocessor_count,
            device.memory.free_bytes >> 20, device.memory.total_bytes >> 20, device.numa_node);
        failures += expect(device.memory.free_bytes <= device.memory.total_bytes);
        failures += expect(device.specs.total_bytes != 0);
        failures += expect(device.specs.multiprocessor_count > 0);
    }
    if (devices->size() == 0) return report(name, "no CUDA device");

    expected<cuda_stream_t> const stream = cuda_stream_t::make(0);
    failures += expect(stream.has_value());
    if (!stream) return report(name, failures);

    std::size_t const count = std::size_t {1} << 20;
    expected<cuda_pinned_buffer<std::uint64_t>> source = cuda_pinned_buffer<std::uint64_t>::make(
        count, cuda_pinned_allocator<std::uint64_t> {});
    expected<cuda_device_buffer<std::uint64_t>> on_device = cuda_device_buffer<std::uint64_t>::make(
        count, cuda_device_allocator<std::uint64_t>(0));
    expected<cuda_pinned_buffer<std::uint64_t>> target = cuda_pinned_buffer<std::uint64_t>::make(
        count, cuda_pinned_allocator<std::uint64_t> {});
    failures += expect(source.has_value() && on_device.has_value() && target.has_value());
    if (!source || !on_device || !target) return report(name, failures);

    std::uint64_t* const values = source->data();
    for (std::size_t index = 0; index != count; ++index) values[index] = index * 0x9E3779B97F4A7C15ull;

    expected<cuda_stopwatch_t> stopwatch = cuda_stopwatch_t::make(0);
    expected<cuda_phases_t> phases = cuda_phases_t::make(*stream);
    failures += expect(stopwatch.has_value() && phases.has_value());
    if (!stopwatch || !phases) return report(name, failures);

    failures += expect(stopwatch->start(*stream) == status_t::success_k);
    failures += expect(phases->begin("copy in") == status_t::success_k);
    failures += expect(on_device->copy_in(values, count, 0, stream->handle()) == status_t::success_k);
    failures += expect(phases->begin("copy out") == status_t::success_k);
    failures += expect(on_device->copy_out(target->data(), count, 0, stream->handle()) == status_t::success_k);
    failures += expect(phases->end() == status_t::success_k);
    failures += expect(stopwatch->stop(*stream) == status_t::success_k);

    std::uint64_t const* const copied = target->data();
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index != count; ++index) mismatches += copied[index] != values[index];
    failures += expect_eq(mismatches, std::size_t {0});

    expected<double> const elapsed = stopwatch->milliseconds();
    failures += expect(elapsed.has_value() && *elapsed >= 0.0);
    failures += expect_eq(phases->size(), std::size_t {2});
    for (std::size_t index = 0; index != phases->size(); ++index) {
        expected<double> const phase = phases->milliseconds(index);
        failures += expect(phase.has_value() && *phase >= 0.0);
    }

    basic_vector<std::uint64_t, cuda_pinned_allocator<std::uint64_t>> pinned_vector;
    failures += expect(pinned_vector.resize(4096) == status_t::success_k);
    return report(name, failures);
}

/** Runs a compaction on the first CUDA device and expects the host's output back in every field. */
template <typename key_slot_type_>
std::size_t report_gpu_case(char const* name, scenario_t const& scenario, test_lsm_level<key_slot_type_> const& upper,
                            test_lsm_level<key_slot_type_> const& lower,
                            compaction_output<key_slot_type_> const& host) {
    expected<std::size_t> const devices = cuda_count_devices();
    if (!devices || *devices == 0) return report(name, "no CUDA device");
    std::size_t failures = 0;
    expected<gpu_compactor_t> compactor = gpu_compactor_t::make(0);
    failures += expect(compactor.has_value());
    if (!compactor) return report(name, failures);
    compaction_output<key_slot_type_> device;
    status_t const status = compactor->compact(scenario.kind, views_of(upper), views_of(lower), scenario.bounds,
                                               device);
    failures += expect(status == status_t::success_k);
    if (status == status_t::success_k) failures += expect_identical(host, device);
    return report(name, failures);
}

#else

/** Reports the device case skipped in a build without CUDA. */
std::size_t report_cuda_devices_case() { return report("cuda.devices_round_trip", "built without CUDA"); }

/** Reports a GPU case skipped in a build without CUDA. */
template <typename key_slot_type_>
std::size_t report_gpu_case(char const* name, scenario_t const&, test_lsm_level<key_slot_type_> const&,
                            test_lsm_level<key_slot_type_> const&, compaction_output<key_slot_type_> const&) {
    return report(name, "built without CUDA");
}

#endif

/** Generates the LSM levels of @p scenario for one key slot shape, then checks the host compaction and the GPU one. */
template <typename key_slot_type_>
std::size_t run_scenario(char const* shape, std::size_t scenario_index, scenario_t const& scenario,
                         options_t const& options) {
    using stored_key_t = typename key_slot_type_::stored_key_t;
    char host_name[128] {};
    char gpu_name[128] {};
    *fmt::format_to_n(host_name, sizeof(host_name) - 1, "{}.{}.host", shape, scenario.name).out = '\0';
    *fmt::format_to_n(gpu_name, sizeof(gpu_name) - 1, "{}.{}.gpu", shape, scenario.name).out = '\0';

    std::size_t failures = 0;
    splitmix_t random {options.seed.value * 1'000'003 + scenario_index};
    basic_vector<stored_key_t> universe;
    std::uint64_t const random_keys = scenario.random_keys != 0 ? scenario.random_keys : options.large_universe.value;
    status_t status = make_universe(random_keys, random, universe);
    failures += expect(status == status_t::success_k);
    if (failures != 0) return report(host_name, failures);

    std::span<stored_key_t const> const keys(universe.data(), universe.size());
    bool const overlapping = (scenario.kind == compaction_kind_t::overlapping_into_intermediate_k) |
                             (scenario.kind == compaction_kind_t::overlapping_into_bottom_k);
    test_lsm_level<key_slot_type_> upper;
    test_lsm_level<key_slot_type_> lower;
    status = overlapping ? add_overlapping_sorted_runs(keys, scenario, scenario.upper_sorted_runs, random, upper)
                         : add_sorted_sorted_runs(keys, scenario, scenario.upper_sorted_runs, random, upper);
    if (status == status_t::success_k && scenario.lower_sorted_runs != 0)
        status = add_sorted_sorted_runs(keys, scenario, scenario.lower_sorted_runs, random, lower);
    if (status == status_t::success_k) status = lay_out(upper, scenario.bounds.branching);
    if (status == status_t::success_k) status = lay_out(lower, scenario.bounds.branching);
    basic_vector<located_key_slot<key_slot_type_>> survivors;
    if (status == status_t::success_k) status = reference_survivors(scenario.kind, upper, lower, survivors);
    failures += expect(status == status_t::success_k);
    if (failures != 0) return report(host_name, failures);

    compaction_output<key_slot_type_> host;
    status = cpu_compactor_t {}.compact(scenario.kind, views_of(upper), views_of(lower), scenario.bounds, host);
    failures += expect(status == status_t::success_k);
    if (status == status_t::success_k) failures += check_compaction(scenario.kind, scenario.bounds, survivors, host);
    std::size_t const host_failures = report(host_name, failures);
    return host_failures + report_gpu_case(gpu_name, scenario, upper, lower, host);
}

#pragma endregion Cases

int main(int argc, char** argv) {
    options_t options;
    std::array<flag_t, 2> const flags {{
        {"--seed", "count", "seed of every generated LSM level", &options.seed},
        {"--large-universe", "count", "random keys of the large cases, which cross the GPU threshold",
         &options.large_universe},
    }};
    argument_status_t const parsed = parse_test_arguments(argc, argv, flags);
    if (parsed == argument_status_t::help_k) return 0;
    if (parsed != argument_status_t::parsed_k) return 1;

    std::size_t total = 0;
    total += report("compaction.uuid_orders_like_memcmp", uuid_orders_like_memcmp(options));
    total += report("compaction.sorted_run_index_layout_matches_builder", sorted_run_index_layout_matches_builder());
    total += report_cuda_devices_case();
    for (std::size_t index = 0; index != scenarios_k.size(); ++index) {
        scenario_t const& scenario = scenarios_k[index];
        total += run_scenario<inline_key_slot<std::uint64_t>>("inline_u64", index, scenario, options);
        total += run_scenario<fixed_width_key_slot<std::uint64_t>>("fixed_u64", index, scenario, options);
        total += run_scenario<variable_width_key_slot<std::uint64_t>>("variable_width_u64", index, scenario, options);
        total += run_scenario<inline_key_slot<uuid_stored_key_t>>("inline_uuid", index, scenario, options);
        total += run_scenario<fixed_width_key_slot<uuid_stored_key_t>>("fixed_uuid", index, scenario, options);
        total += run_scenario<variable_width_key_slot<uuid_stored_key_t>>("variable_width_uuid", index, scenario,
                                                                          options);
    }
    return report("total", total) ? 1 : 0;
}
