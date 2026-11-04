/**
 *  @file test/nvme/compaction.cu
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief Tests host and GPU compaction against a reference merge and against each other, for 64-bit and 16-byte keys.
 *
 *  The suite compiles as plain C++ twice: over the CUDA compactor, which @c compaction_device.cu instantiates for the
 *  suite's key slots, and over @c compaction_absent.cpp, which makes no device compactor, so its device cases report
 *  as skipped. LSM levels are generated from a seed, with overlapping and sorted runs, remove deltas, versions of one
 *  key under several stamps, and keys crowded at the edges a comparator gets wrong. Graph levels of half-edge changes
 *  check the device fold against the host fold byte for byte.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstdio>  // `std::fopen`, `std::fwrite`, `std::fclose`
#include <cstring> // `std::memcmp`, `std::memcpy`, `std::memset`

#include <algorithm>   // `std::sort`, `std::unique`
#include <array>       // `std::array`
#include <chrono>      // `std::chrono::steady_clock`
#include <span>        // `std::span`, `std::as_bytes`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::type_identity`

#include <fmt/format.h>

#include <forkunion/atomics.hpp> // `fu::standard_atomic_ref`
#include <forkunion/types.hpp>   // `fu::standard_yield_t`
#include <smashtable/basic_vector.hpp>
#include <smashtable/shared.hpp>

#include "ustore/blobs/nvme_compaction.hpp"        // `host_compactor_t`, `compacted_sorted_run_t`, `compaction_kind_t`
#include "ustore/blobs/vram_nvidia_compaction.hpp" // `cuda_compactor_t`
#include "ustore/graph/adjacency_fold.hpp"         // `adjacency_fold_t`, `save_delta`
#include "ustore/graph/vram_nvidia_adjacency_fold.hpp" // `vram_nvidia_adjacency_fold_t`
#include "ustore/utilities/nvme_linux_cufile_disk.hpp" // `linux_cufile_files_disk`, `linux_cufile_disk_config_t`
#include "ustore/utilities/types.hpp"                  // `status_t`, `status_code_t`
#include "ustore/utilities/vram_device.hpp"            // `gpu_drivers_t`, `list_gpu_devices`

#include "harness.hpp"                   // `suite_exit_code`, `expect`, `expect_eq`
#include "nvme/compaction_key_slots.hpp" // `inline_key_slot`, `fixed_width_key_slot`, `variable_width_key_slot`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::test::nvme;
using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;
using st::vector;

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

/** Bytes every value of the suite's fixed-width shapes takes, removals included. */
inline constexpr std::uint32_t fixed_value_bytes_k = 24;

/** An inline key slot of @p key with random inline bytes, whose value takes no bytes of a values section. */
template <typename stored_key_type_>
inline_key_slot<stored_key_type_> make_key_slot(std::type_identity<inline_key_slot<stored_key_type_>>,
                                                stored_key_type_ const& key, key_slot_state_t state, random_t& random,
                                                std::uint32_t& size) noexcept {
    size = 0;
    inline_key_slot<stored_key_type_> key_slot;
    key_slot.stored_key = key;
    if (state == key_slot_state_t::removed_k) return key_slot;
    key_slot.size = static_cast<std::uint32_t>(1 + random.below(sizeof(key_slot.data)));
    for (byte_t& byte : key_slot.data) byte = static_cast<byte_t>(random.next());
    return key_slot;
}

/** A fixed-width key slot of @p key, whose stride a removal spends too. */
template <typename stored_key_type_>
fixed_width_key_slot<stored_key_type_> make_key_slot(std::type_identity<fixed_width_key_slot<stored_key_type_>>,
                                                     stored_key_type_ const& key, key_slot_state_t, random_t&,
                                                     std::uint32_t& size) noexcept {
    size = fixed_value_bytes_k;
    fixed_width_key_slot<stored_key_type_> key_slot;
    key_slot.stored_key = key;
    return key_slot;
}

/** A variable-width key slot of @p key, its value empty once in 16 when alive. */
template <typename stored_key_type_>
variable_width_key_slot<stored_key_type_> make_key_slot(std::type_identity<variable_width_key_slot<stored_key_type_>>,
                                                        stored_key_type_ const& key, key_slot_state_t state,
                                                        random_t& random, std::uint32_t& size) noexcept {
    bool const empty = state == key_slot_state_t::removed_k || random.below(16) == 0;
    size = empty ? 0 : static_cast<std::uint32_t>(1 + random.below(200));
    variable_width_key_slot<stored_key_type_> key_slot;
    key_slot.stored_key = key;
    return key_slot;
}

/** The bounds @p bounds name with the value stride of @p key_slot_type_, which only a fixed width has. */
template <typename key_slot_type_>
sorted_run_bounds_t shaped_bounds(sorted_run_bounds_t bounds) noexcept {
    if constexpr (locates_stored_values_concept<key_slot_type_>)
        if (is_fixed(key_slot_type_::value_layout_k)) bounds.value_stride = fixed_value_bytes_k;
    return bounds;
}

/** The key slot input padding holds here: one that looks real, so a compaction reading padding shows in its output. */
template <typename key_slot_type_>
key_slot_type_ poison_key_slot() noexcept {
    key_slot_type_ poison {};
    poison.stored_key = limit<typename key_slot_type_::stored_key_t>::min();
    if constexpr (sizes_stored_values_concept<key_slot_type_>) poison.size = 7;
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
    bool same = (first.stored_key == second.stored_key) && same_inline_stored_value(first, second);
    if constexpr (sizes_stored_values_concept<key_slot_type_>) same = same && first.size == second.size;
    return same;
}

#pragma endregion Key Slots

#pragma region Keys

/** A high word many 16-byte keys share, crowding them into one run of equal high words. */
inline constexpr std::uint64_t shared_high_word_k = 0x0123456789ABCDEFull;

/** Appends 64-bit keys a comparator most often gets wrong: both ends, the sign boundary and small integers. */
st::status_t append_adversarial_keys(vector<std::uint64_t>& keys) noexcept {
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
st::status_t append_adversarial_keys(vector<st::key128_t>& keys) noexcept {
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
st::status_t make_universe(std::uint64_t random_keys, random_t& random, vector<stored_key_type_>& universe) {
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
    vector<key_slot_type_> sorted;

    /** The stamp of every key slot of @c sorted, newest first among the versions of one key. */
    vector<std::uint64_t> stamps;

    /** The @c delta_kind_t of every key slot of @c sorted. */
    vector<std::uint8_t> kinds;

    /** Bytes the value of every key slot of @c sorted takes in its run's values section. */
    vector<std::uint32_t> sizes;

    /** Key slots in each sorted run. */
    vector<std::uint64_t> counts;

    /** Every sorted run's cells, one sorted run after another. */
    vector<key_slot_type_> cells;

    /** Every sorted run's stamps packed, as a run keeps them, or none for an LSM level without stamps. */
    vector<stamps_column_t> columns;

    /** Every sorted run's kinds packed, as a run keeps them. */
    vector<kinds_column_t> kind_columns;

    /** Every sorted run's value offsets, one per key slot and one past the last, for a variable width alone. */
    vector<stored_value_offset_t> value_offsets;

    /** Where each sorted run's key slots start in @c sorted. */
    vector<std::uint64_t> firsts;

    /** A view per sorted run into @c cells, @c columns, @c kind_columns and @c value_offsets. */
    vector<sorted_run_index_view<key_slot_type_>> views;

    /** Where each sorted run's sections sit in the suite's sections file, empty until @c lay_sections lays them. */
    vector<sorted_run_extents_t> extents;
};

/** The views of an LSM level's sorted runs, as a compaction takes them. */
template <typename key_slot_type_>
std::span<sorted_run_index_view<key_slot_type_> const> views_of(
    test_lsm_level<key_slot_type_> const& lsm_level) noexcept {
    return lsm_level.views;
}

/** Where every sorted run of @p upper, then of @p lower, sits on its volume, as a device compaction takes them: as
 *  @c lay_sections laid it, or nowhere for a run uploading from memory. */
template <typename key_slot_type_>
vector<sorted_run_extents_t> extents_of(test_lsm_level<key_slot_type_> const& upper,
                                        test_lsm_level<key_slot_type_> const& lower) noexcept {
    vector<sorted_run_extents_t> extents;
    if (extents.resize(upper.views.size() + lower.views.size()) != st::status_t::success_k) return extents;
    for (std::size_t run = 0; run != upper.extents.size(); ++run) extents[run] = upper.extents[run];
    for (std::size_t run = 0; run != lower.extents.size(); ++run)
        extents[upper.views.size() + run] = lower.extents[run];
    return extents;
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
                            std::uint64_t era, random_t& random, test_lsm_level<key_slot_type_>& lsm_level) noexcept {
    std::uint64_t const half = stamps_per_sorted_run_k / 2;
    std::uint64_t const base = era * stamps_per_sorted_run_k;
    vector<std::uint64_t>& stamps = lsm_level.stamps;
    auto const pick = [&](typename key_slot_type_::stored_key_t const& key) noexcept {
        key_slot_state_t const state = random_state(scenario, random);
        std::uint32_t size = 0;
        st::status_t status = lsm_level.sorted.push_back(
            make_key_slot(std::type_identity<key_slot_type_> {}, key, state, random, size));
        if (status == st::status_t::success_k) status = lsm_level.sizes.push_back(std::uint32_t {size});
        return status != st::status_t::success_k ? status : lsm_level.kinds.push_back(kind_of(state));
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
        st::status_t status = pick_key_slots<key_slot_type_>(keys, scenario, first_era + sorted_run, random, lsm_level);
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
    st::status_t status = pick_key_slots<key_slot_type_>(keys, scenario, era, random, lsm_level);
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

/** Lays every sorted run of @p lsm_level into cells in sorted order, as a run keeps them, and into
 *  value offsets where its values differ in width. */
template <typename key_slot_type_>
st::status_t lay_out(test_lsm_level<key_slot_type_>& lsm_level) noexcept {
    st::status_t status = lsm_level.cells.resize(lsm_level.sorted.size());
    if (status != st::status_t::success_k) return status;
    bool offsets = false;
    if constexpr (locates_stored_values_concept<key_slot_type_>)
        offsets = is_variable_width(key_slot_type_::value_layout_k);
    status = lsm_level.value_offsets.resize(offsets ? lsm_level.sorted.size() + lsm_level.counts.size() : 0);
    if (status != st::status_t::success_k) return status;
    status = lsm_level.firsts.resize(lsm_level.counts.size());
    if (status != st::status_t::success_k) return status;
    status = lsm_level.views.resize(lsm_level.counts.size());
    if (status != st::status_t::success_k) return status;
    bool const stamped = !lsm_level.stamps.empty();
    status = lsm_level.columns.resize(stamped ? lsm_level.counts.size() : 0);
    if (status != st::status_t::success_k) return status;
    bool const kinded = !lsm_level.kinds.empty();
    status = lsm_level.kind_columns.resize(kinded ? lsm_level.counts.size() : 0);
    if (status != st::status_t::success_k) return status;

    std::uint64_t first = 0;
    for (std::size_t sorted_run = 0; sorted_run != lsm_level.counts.size(); ++sorted_run) {
        std::uint64_t const count = lsm_level.counts[sorted_run];
        for (std::uint64_t position = 0; position != count; ++position)
            lsm_level.cells[first + position] = lsm_level.sorted[first + position];
        lsm_level.views[sorted_run] = sorted_run_index_view<key_slot_type_> {lsm_level.cells.data() + first, count};
        lsm_level.firsts[sorted_run] = first;
        if (offsets) {
            stored_value_offset_t* const run_offsets = lsm_level.value_offsets.data() + first + sorted_run;
            run_offsets[0] = 0;
            for (std::uint64_t position = 0; position != count; ++position)
                run_offsets[position + 1] = run_offsets[position] + lsm_level.sizes[first + position];
            lsm_level.views[sorted_run].value_offsets = std::span<stored_value_offset_t const> {
                run_offsets, static_cast<std::size_t>(count + 1)};
        }
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
                                 vector<located_key_slot<key_slot_type_>>& survivors) noexcept {
    using record_t = located_key_slot<key_slot_type_>;
    vector<record_t> records;
    st::status_t status = records.resize(upper.sorted.size() + lower.sorted.size());
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
                records[written++] = record_t {
                    lsm_level->sorted[first + position], sorted_run, kind, stamp, static_cast<std::uint32_t>(position),
                    lsm_level->sizes[first + position]};
            }
            first += lsm_level->counts[index];
        }
    }

    std::uint64_t const upper_count = upper.counts.size();
    auto const rank = [upper_count](std::uint32_t index) noexcept {
        return index < upper_count ? upper_count - 1 - index : std::uint64_t {index};
    };
    std::stable_sort(records.data(), records.data() + records.size(),
                     [&rank](record_t const& first, record_t const& second) noexcept {
                         if (first.key_slot.stored_key < second.key_slot.stored_key) return true;
                         if (second.key_slot.stored_key < first.key_slot.stored_key) return false;
                         return rank(first.sorted_run) < rank(second.sorted_run);
                     });

    bool const drops_removals = (kind == compaction_kind_t::overlapping_into_bottom_k) |
                                (kind == compaction_kind_t::sorted_into_bottom_k);
    std::uint64_t const oldest_pin = stamps.pinned_count != 0 ? stamps.pinned[0] : ~std::uint64_t {0};
    vector<std::uint8_t> kept;
    status = kept.resize(records.size());
    if (status != st::status_t::success_k) return status;
    survivors.clear();
    for (std::size_t begin = 0; begin != records.size();) {
        std::size_t end = begin + 1;
        while (end != records.size() && records[end].key_slot.stored_key == records[begin].key_slot.stored_key) ++end;
        for (std::size_t index = begin; index != end; ++index) kept[index] = 0;

        // The newest version at or below each of the ceiling, the horizon and every pin is read by someone
        auto const keep_newest_at_or_below = [&](std::uint64_t bound) noexcept {
            for (std::size_t index = begin; index != end; ++index)
                if (records[index].stamp <= bound && records[index].stamp <= stamps.ceiling) {
                    kept[index] = 1;
                    return;
                }
        };
        keep_newest_at_or_below(stamps.ceiling);
        keep_newest_at_or_below(stamps.horizon);
        for (std::uint64_t pin = 0; pin != stamps.pinned_count; ++pin) keep_newest_at_or_below(stamps.pinned[pin]);
        for (std::size_t index = begin; index != end; ++index)
            if (records[index].stamp > stamps.horizon && records[index].stamp <= stamps.ceiling) kept[index] = 1;

        for (std::size_t index = begin; index != end; ++index) {
            if (!kept[index]) continue;
            bool const hides_nothing = records[index].stamp <= stamps.horizon && records[index].stamp <= oldest_pin;
            if (drops_removals && records[index].kind == delta_kind_t::remove_k && hides_nothing) continue;
            status = survivors.push_back(record_t {records[index]});
            if (status != st::status_t::success_k) return status;
        }
        begin = end;
    }
    return st::status_t::success_k;
}

#pragma endregion LSM Levels

#pragma region Checks

/** Where the value of @p record starts inside its source run's values section, as @p upper and @p lower lay them. */
template <typename key_slot_type_>
std::uint64_t source_begin_of(located_key_slot<key_slot_type_> const& record, sorted_run_bounds_t const& bounds,
                              test_lsm_level<key_slot_type_> const& upper,
                              test_lsm_level<key_slot_type_> const& lower) noexcept {
    std::size_t const upper_runs = upper.views.size();
    sorted_run_index_view<key_slot_type_> const& view = record.sorted_run < upper_runs
                                                            ? upper.views[record.sorted_run]
                                                            : lower.views[record.sorted_run - upper_runs];
    return !view.value_offsets.empty() ? view.value_offsets[record.position] : record.position * bounds.value_stride;
}

/** Checks a compaction's output against the reference survivors: order, bounds, value offsets, and
 *  copy ranges that cover every survivor's value in order and coalesce every neighbour they can. */
template <typename key_slot_type_>
std::size_t check_compaction(compaction_kind_t kind, sorted_run_bounds_t const& bounds,
                             compaction_stamps_t const& stamps,
                             vector<located_key_slot<key_slot_type_>> const& wanted_survivors,
                             test_lsm_level<key_slot_type_> const& upper, test_lsm_level<key_slot_type_> const& lower,
                             compaction_output<key_slot_type_> const& output) {
    using stored_key_t = typename key_slot_type_::stored_key_t;
    std::size_t failures = 0;

    std::uint64_t key_slots_count = 0;
    for (std::size_t index = 0; index != output.sorted_runs.size(); ++index)
        key_slots_count += output.sorted_runs[index].key_slots_count;
    failures += expect_eq(key_slots_count, std::uint64_t {wanted_survivors.size()});
    failures += expect_eq(output.cells.size(), std::size_t {key_slots_count});
    bool offsets = false;
    if constexpr (locates_stored_values_concept<key_slot_type_>)
        offsets = is_variable_width(key_slot_type_::value_layout_k);
    failures += expect_eq(output.value_offsets.size(), offsets ? std::size_t {key_slots_count + 1} : 0u);
    if (!locates_stored_values_concept<key_slot_type_>)
        failures += expect_eq(output.value_copy_ranges.size(), std::size_t {0});
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
    std::size_t wrong_key_slots = 0;
    std::size_t wrong_offsets = 0;
    std::size_t wrong_copies = 0;
    std::size_t uncoalesced = 0;
    std::size_t unordered = 0;
    std::size_t kept_removals = 0;

    // An equal cut lands within one key slot, one value, a row of the tree of its keys and the
    // alignment of four sections of its aim.
    std::uint64_t largest_value = 0;
    for (std::size_t index = 0; index != wanted_survivors.size(); ++index)
        largest_value = std::max<std::uint64_t>(largest_value, wanted_survivors[index].size);
    std::uint64_t const cut_slack_bytes = bounds.header_bytes + 4 * bounds.alignment_bytes + sizeof(key_slot_type_) +
                                          st::default_row_bytes_k + largest_value;

    stored_key_t previous {};
    std::uint64_t first = 0;
    std::uint64_t first_range = 0;
    for (std::size_t index = 0; index != output.sorted_runs.size(); ++index) {
        compacted_sorted_run_t const& sorted_run = output.sorted_runs[index];
        std::uint64_t const count = sorted_run.key_slots_count;
        empty_sorted_runs += count == 0;
        std::uint64_t const run_bytes = sorted_run_file_bytes<key_slot_type_>(bounds, count,
                                                                              sorted_run.stored_values_bytes);
        oversized_sorted_runs += (count > 1) && run_bytes > bounds.file_bytes_limit;
        // A run cut short to keep a key's versions together is not unfilled, so only unversioned merges check this
        if (!versioned && output.sorted_runs.size() > 1)
            unfilled_sorted_runs += 2 * (run_bytes + cut_slack_bytes) < bounds.file_bytes_limit;
        if (count == 0) continue;
        std::uint64_t const ranges_end = first_range + sorted_run.value_copy_ranges_count;
        if (ranges_end > output.value_copy_ranges.size()) return failures + expect(false);

        // Each survivor's bytes come next in the run's copy ranges, from its own source at its own offset.
        std::uint64_t running = 0;
        std::uint64_t range = first_range;
        std::uint64_t into_range = 0;
        for (std::uint64_t position = 0; position != count; ++position) {
            located_key_slot<key_slot_type_> const& wanted = wanted_survivors[first + position];
            key_slot_type_ const& actual = output.cells[first + position];
            wrong_key_slots += !same_key_slot(actual, wanted.key_slot);
            if (offsets)
                wrong_offsets += output.value_offsets[first + position + 1] - output.value_offsets[first + position] !=
                                 wanted.size;
            if (wanted.size != 0) {
                if (range == ranges_end) ++wrong_copies;
                else {
                    value_copy_range_t const& copied = output.value_copy_ranges[range];
                    wrong_copies += copied.source_run != wanted.sorted_run ||
                                    copied.source_begin + into_range != source_begin_of(wanted, bounds, upper, lower) ||
                                    copied.target_begin + into_range != running ||
                                    into_range + wanted.size > copied.bytes;
                    into_range += wanted.size;
                    if (into_range >= copied.bytes) {
                        ++range;
                        into_range = 0;
                    }
                }
            }
            running += wanted.size;
            // A remove delta survives the bottom only where a reader still needs it to hide an older version
            kept_removals += drops_removals &&
                             output.kinds[first + position] == static_cast<std::uint8_t>(delta_kind_t::remove_k) &&
                             stamps.pinned_count == 0 && stamps.horizon == ~std::uint64_t {0};
            if (first + position != 0)
                unordered += actual.stored_key < previous || (!versioned && actual.stored_key == previous);
            previous = actual.stored_key;
        }
        wrong_bytes += running != sorted_run.stored_values_bytes;
        wrong_copies += range != ranges_end;
        // Two neighbouring ranges of one source that sit back to back should have been one.
        for (std::uint64_t later = first_range + 1; later < ranges_end; ++later) {
            value_copy_range_t const& before = output.value_copy_ranges[later - 1];
            value_copy_range_t const& after = output.value_copy_ranges[later];
            uncoalesced += before.source_run == after.source_run &&
                           before.source_begin + before.bytes == after.source_begin;
        }
        first += count;
        first_range = ranges_end;
    }
    failures += expect_eq(first_range, std::uint64_t {output.value_copy_ranges.size()});

    failures += expect_eq(empty_sorted_runs, std::size_t {0});
    failures += expect_eq(oversized_sorted_runs, std::size_t {0});
    failures += expect_eq(unfilled_sorted_runs, std::size_t {0});
    failures += expect_eq(wrong_bytes, std::size_t {0});
    failures += expect_eq(wrong_key_slots, std::size_t {0});
    failures += expect_eq(wrong_offsets, std::size_t {0});
    failures += expect_eq(wrong_copies, std::size_t {0});
    failures += expect_eq(uncoalesced, std::size_t {0});
    failures += expect_eq(unordered, std::size_t {0});
    failures += expect_eq(kept_removals, std::size_t {0});
    return failures;
}

/** How many sizes, sorted runs, cell fields, value offsets, copy ranges, stamps, kinds and folded bytes two compactions
 *  of the same LSM levels disagree in. */
template <typename key_slot_type_>
std::size_t differences(compaction_output<key_slot_type_> const& host,
                        compaction_output<key_slot_type_> const& device) {
    std::size_t const unequal_sizes = (device.sorted_runs.size() != host.sorted_runs.size()) +
                                      (device.cells.size() != host.cells.size()) +
                                      (device.value_offsets.size() != host.value_offsets.size()) +
                                      (device.value_copy_ranges.size() != host.value_copy_ranges.size()) +
                                      (device.stamps.size() != host.stamps.size()) +
                                      (device.kinds.size() != host.kinds.size()) +
                                      (device.folded.size() != host.folded.size());
    if (unequal_sizes != 0) return unequal_sizes;

    std::size_t mismatches = 0;
    for (std::size_t index = 0; index != host.sorted_runs.size(); ++index)
        mismatches += host.sorted_runs[index].key_slots_count != device.sorted_runs[index].key_slots_count ||
                      host.sorted_runs[index].stored_values_bytes != device.sorted_runs[index].stored_values_bytes ||
                      host.sorted_runs[index].value_copy_ranges_count !=
                          device.sorted_runs[index].value_copy_ranges_count;
    for (std::size_t index = 0; index != host.cells.size(); ++index)
        mismatches += !same_key_slot(host.cells[index], device.cells[index]);
    for (std::size_t index = 0; index != host.value_offsets.size(); ++index)
        mismatches += host.value_offsets[index] != device.value_offsets[index];
    mismatches += std::memcmp(host.value_copy_ranges.data(), device.value_copy_ranges.data(),
                              host.value_copy_ranges.size() * sizeof(value_copy_range_t)) != 0;
    for (std::size_t index = 0; index != host.stamps.size(); ++index)
        mismatches += host.stamps[index] != device.stamps[index];
    for (std::size_t index = 0; index != host.kinds.size(); ++index)
        mismatches += host.kinds[index] != device.kinds[index];
    for (std::size_t index = 0; index != host.folded.size(); ++index)
        mismatches += host.folded[index] != device.folded[index];
    return mismatches;
}

/** Expects two compactions of the same LSM levels to agree in every sorted run, cell field, value offset, copy range
 *  and folded byte. */
template <typename key_slot_type_>
std::size_t expect_identical(compaction_output<key_slot_type_> const& host,
                             compaction_output<key_slot_type_> const& device) {
    return expect_eq(differences(host, device), std::size_t {0});
}

#pragma endregion Checks

#pragma region Rules

/** The key slot shape the rule checks use, which locates stored values. */
using checked_key_slot_t = fixed_width_key_slot<std::uint64_t>;

/** A key slot shape of exactly 16 bytes, which the run plan's byte arithmetic counts in. */
using sixteen_byte_key_slot_t = fixed_width_key_slot<st::key128_t>;

/** A located key slot of @p key from sorted run @p sorted_run, with a value of the fixed width. */
located_key_slot<checked_key_slot_t> checked_record(std::uint64_t key, std::uint32_t sorted_run) noexcept {
    checked_key_slot_t key_slot;
    key_slot.stored_key = key;
    located_key_slot<checked_key_slot_t> record {key_slot, sorted_run};
    record.size = fixed_value_bytes_k;
    return record;
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

/** Checks that the split cuts sorted runs of equal size within the file limit, and never below
 *  one key slot. */
std::size_t plan_sorted_runs_cuts_at_the_bounds() {
    std::size_t failures = 0;
    vector<compacted_sorted_run_t> sorted_runs;

    // Stored value bytes read out of a prefix sum the caller fills, as both compactors read theirs
    vector<std::uint64_t> prefix;
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

    // Runs of 16-byte cells with no header and no alignment to round past, sized with the tree
    // of their keys
    sorted_run_bounds_t bounds;
    bounds.file_bytes_limit = std::uint64_t {1} << 40;
    bounds.header_bytes = 0;
    bounds.alignment_bytes = 1;
    failures += expect(admits_sorted_runs(bounds));
    auto const run_bytes = [&bounds](std::uint64_t count, std::uint64_t values) noexcept {
        return sorted_run_file_bytes<sixteen_byte_key_slot_t>(bounds, count, values);
    };
    failures += expect_eq(sizeof(sixteen_byte_key_slot_t), std::size_t {16});

    // No key slots leaves no sorted runs at all
    failures += expect(fill_prefix(0, 0));
    failures += expect(plan_sorted_runs<sixteen_byte_key_slot_t>(0, bounds, read_prefix, unshared, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {0});

    // One key slot leaves one sorted run of one
    failures += expect(fill_prefix(1, 24));
    failures += expect(plan_sorted_runs<sixteen_byte_key_slot_t>(1, bounds, read_prefix, unshared, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {1});
    if (sorted_runs.size() == 1) {
        failures += expect_eq(sorted_runs[0].key_slots_count, std::uint64_t {1});
        failures += expect_eq(sorted_runs[0].stored_values_bytes, std::uint64_t {24});
    }

    // Twelve key slots overflow a limit of ten by two, which cut as six and six rather than
    // ten and two
    bounds.file_bytes_limit = run_bytes(10, 160);
    failures += expect(fill_prefix(12, 16));
    failures += expect(plan_sorted_runs<sixteen_byte_key_slot_t>(12, bounds, read_prefix, unshared, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {2});
    if (sorted_runs.size() == 2) {
        std::size_t wrong = 0;
        for (std::size_t index = 0; index != 2; ++index)
            wrong += sorted_runs[index].key_slots_count != 6 || sorted_runs[index].stored_values_bytes != 96;
        failures += expect_eq(wrong, std::size_t {0});
    }

    // A key slot whose stored value alone exceeds the file limit still takes a sorted run of its own
    bounds.file_bytes_limit = 100;
    failures += expect(fill_prefix(3, 1000));
    failures += expect(plan_sorted_runs<sixteen_byte_key_slot_t>(3, bounds, read_prefix, unshared, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {3});
    if (sorted_runs.size() == 3) {
        std::size_t wrong = 0;
        for (std::size_t index = 0; index != 3; ++index)
            wrong += sorted_runs[index].key_slots_count != 1 || sorted_runs[index].stored_values_bytes != 1000;
        failures += expect_eq(wrong, std::size_t {0});
    }

    // A sorted run that exactly fills the file limit is not cut short
    bounds.file_bytes_limit = run_bytes(4, 40);
    failures += expect(fill_prefix(4, 10));
    failures += expect(plan_sorted_runs<sixteen_byte_key_slot_t>(4, bounds, read_prefix, unshared, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {1});
    if (sorted_runs.size() == 1) failures += expect_eq(sorted_runs[0].key_slots_count, std::uint64_t {4});

    // A failing read of the prefix sums stops the split instead of cutting on a stale byte count
    auto const failing_prefix = [](std::uint64_t, std::uint64_t& bytes) noexcept {
        bytes = 0;
        return st::status_t::unknown_k;
    };
    failures += expect(plan_sorted_runs<sixteen_byte_key_slot_t>(4, bounds, failing_prefix, unshared, sorted_runs) ==
                       st::status_t::unknown_k);

    // Versions of one key stay in one run: keys `aabbbcd` aimed at four and three cut as
    // `aa|bbb|cd`, never inside `bbb`
    bounds.file_bytes_limit = run_bytes(4, 4);
    std::array<int, 7> const grouped {0, 0, 1, 1, 1, 2, 3};
    auto const shares = [&grouped](std::uint64_t index, bool& shared) noexcept {
        shared = grouped[index] == grouped[index - 1];
        return st::status_t::success_k;
    };
    failures += expect(fill_prefix(7, 1));
    failures += expect(plan_sorted_runs<sixteen_byte_key_slot_t>(7, bounds, read_prefix, shares, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {3});
    if (sorted_runs.size() == 3) {
        failures += expect_eq(sorted_runs[0].key_slots_count, std::uint64_t {2});
        failures += expect_eq(sorted_runs[1].key_slots_count, std::uint64_t {3});
        failures += expect_eq(sorted_runs[2].key_slots_count, std::uint64_t {2});
    }

    // A key longer than its run's aim that starts the run takes the run past the aim
    // rather than splitting
    std::array<int, 6> const long_key {0, 0, 0, 0, 0, 1};
    auto const shares_long = [&long_key](std::uint64_t index, bool& shared) noexcept {
        shared = long_key[index] == long_key[index - 1];
        return st::status_t::success_k;
    };
    failures += expect(fill_prefix(6, 1));
    failures += expect(plan_sorted_runs<sixteen_byte_key_slot_t>(6, bounds, read_prefix, shares_long, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect_eq(sorted_runs.size(), std::size_t {2});
    if (sorted_runs.size() == 2) {
        failures += expect_eq(sorted_runs[0].key_slots_count, std::uint64_t {5});
        failures += expect_eq(sorted_runs[1].key_slots_count, std::uint64_t {1});
    }

    // Strings whose tails outweigh their cells cut runs by the tails too, so no run
    // outgrows the limit
    using tailed_key_slot_t = key_slot<st::key128_t, value_layout_t::uuid_k, key_tails_t::carried_k>;
    bounds.file_bytes_limit = 4096;
    failures += expect(fill_prefix(10, 1000));
    failures += expect(plan_sorted_runs<tailed_key_slot_t>(10, bounds, read_prefix, unshared, sorted_runs) ==
                       st::status_t::success_k);
    failures += expect(sorted_runs.size() >= 3);
    std::size_t oversized = 0;
    for (compacted_sorted_run_t const& sorted_run : sorted_runs)
        oversized += sorted_run.key_slots_count > 1 &&
                     sorted_run_file_bytes<tailed_key_slot_t>(bounds, sorted_run.key_slots_count,
                                                              sorted_run.stored_values_bytes) > bounds.file_bytes_limit;
    failures += expect_eq(oversized, std::size_t {0});
    return failures;
}

/** Checks which older versions survive: one per interval between pinned stamps, every one past the horizon, none
 *  past the ceiling, and a remove delta at the bottom only where a reader still needs it. */
std::size_t survives_keeps_what_readers_read() {
    std::size_t failures = 0;
    auto const version = [](std::uint64_t stamp, delta_kind_t kind) noexcept {
        located_key_slot<checked_key_slot_t> record = checked_record(7, 0);
        record.stamp = stamp;
        record.kind = kind;
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
        located_key_slot<checked_key_slot_t> const current = version(chain[index], delta_kind_t::write_k);
        located_key_slot<checked_key_slot_t> const previous = version(index ? chain[index - 1] : 0,
                                                                      delta_kind_t::write_k);
        wrong += survives(index ? &previous : nullptr, current, intermediate, stamps) != wanted[index];
    }
    failures += expect_eq(wrong, std::size_t {0});

    // A remove delta some reader sees stays even at the bottom; one older than every reader drops
    located_key_slot<checked_key_slot_t> const newer = version(50, delta_kind_t::write_k);
    failures += expect(survives(&newer, version(30, delta_kind_t::remove_k), bottom, stamps));
    failures += expect(!survives(&newer, version(10, delta_kind_t::remove_k), bottom, stamps));
    failures += expect(survives(&newer, version(10, delta_kind_t::remove_k), intermediate, stamps));

    // Past the horizon every version survives, and so does the newest beneath it
    compaction_stamps_t durable;
    durable.horizon = 30;
    failures += expect(survives(&newer, version(45, delta_kind_t::write_k), intermediate, durable));
    located_key_slot<checked_key_slot_t> const above = version(45, delta_kind_t::write_k);
    failures += expect(survives(&above, version(25, delta_kind_t::write_k), intermediate, durable));
    located_key_slot<checked_key_slot_t> const beneath = version(25, delta_kind_t::write_k);
    failures += expect(!survives(&beneath, version(20, delta_kind_t::write_k), intermediate, durable));
    failures += expect(survives(&newer, version(45, delta_kind_t::remove_k), bottom, durable));

    // Past the ceiling nothing survives, and the newest beneath it leads its key
    compaction_stamps_t mounted;
    mounted.horizon = 30;
    mounted.ceiling = 30;
    failures += expect(!survives(static_cast<located_key_slot<checked_key_slot_t> const*>(nullptr),
                                 version(45, delta_kind_t::write_k), intermediate, mounted));
    located_key_slot<checked_key_slot_t> const dropped = version(45, delta_kind_t::write_k);
    failures += expect(survives(&dropped, version(25, delta_kind_t::write_k), intermediate, mounted));
    located_key_slot<checked_key_slot_t> const leader = version(25, delta_kind_t::write_k);
    failures += expect(!survives(&leader, version(20, delta_kind_t::write_k), intermediate, mounted));
    return failures;
}

/** A key of up to six bytes from a three-letter alphabet, so random sets share prefixes,
 *  hold prefixes of each other and order a high byte after the letters, as bytes and not
 *  as signed chars. */
struct short_key_t {
    char bytes[6] {};
    std::uint8_t length = 0;

    [[nodiscard]] std::string_view view() const noexcept { return {bytes, length}; }
};

/** Sorts @p keys and drops their repeats. */
void sort_unique_short_keys(vector<short_key_t>& keys) noexcept {
    auto const less = [](short_key_t const& first, short_key_t const& second) noexcept {
        return first.view() < second.view();
    };
    std::sort(keys.begin(), keys.end(), less);
    auto const same = [](short_key_t const& first, short_key_t const& second) noexcept {
        return first.view() == second.view();
    };
    [[maybe_unused]] st::status_t const kept = keys.resize(
        static_cast<std::size_t>(std::unique(keys.begin(), keys.end(), same) - keys.begin()));
}

/** Draws up to @p count distinct short keys into @p keys, ascending. */
st::status_t draw_short_keys(std::size_t count, random_t& random, vector<short_key_t>& keys) noexcept {
    constexpr char alphabet[] {'a', 'b', static_cast<char>(0xF0)};
    keys.clear();
    for (std::size_t index = 0; index != count; ++index) {
        short_key_t key;
        key.length = static_cast<std::uint8_t>(random.below(7));
        for (std::uint8_t byte = 0; byte != key.length; ++byte) key.bytes[byte] = alphabet[random.below(3)];
        if (keys.push_back(short_key_t {key}) != st::status_t::success_k) return st::status_t::out_of_memory_heap_k;
    }
    sort_unique_short_keys(keys);
    return st::status_t::success_k;
}

/** The union of two random sorted sets of short keys names every key once, ascending, at its
 *  position in each side or absent from it, as a brute-force walk of both sets finds it, across
 *  sides empty, small and longer than a block of views. */
std::size_t union_sorted_keys_matches_brute_force() {
    std::size_t failures = 0;
    std::size_t mismatches = 0;
    random_t random {42};
    vector<short_key_t> first, second, everything;
    vector<std::size_t> first_positions, second_positions;
    constexpr std::array<std::size_t, 6> sizes {0, 1, 5, 63, 64, 300};
    for (std::size_t round = 0; round != 200; ++round) {
        std::size_t const first_drawn = sizes[round % sizes.size()];
        std::size_t const second_drawn = sizes[round / sizes.size() % sizes.size()];
        if (draw_short_keys(first_drawn, random, first) != st::status_t::success_k ||
            draw_short_keys(second_drawn, random, second) != st::status_t::success_k ||
            first_positions.resize(first.size() + second.size()) != st::status_t::success_k ||
            second_positions.resize(first.size() + second.size()) != st::status_t::success_k)
            return failures + expect(false);
        auto const reader_of = [](vector<short_key_t> const& side) noexcept {
            return [&side](std::size_t from, std::span<std::string_view> views) noexcept {
                for (std::size_t index = 0; index != views.size(); ++index) views[index] = side[from + index].view();
            };
        };
        std::size_t const rows = union_sorted_keys(reader_of(first), first.size(), reader_of(second), second.size(),
                                                   std::span<std::size_t> {first_positions},
                                                   std::span<std::size_t> {second_positions});

        everything.clear();
        for (short_key_t const& key : first) [[maybe_unused]]
            st::status_t const placed = everything.push_back(short_key_t {key});
        for (short_key_t const& key : second) [[maybe_unused]]
            st::status_t const placed = everything.push_back(short_key_t {key});
        sort_unique_short_keys(everything);
        mismatches += rows != everything.size();
        for (std::size_t row = 0, in_first = 0, in_second = 0; row < rows && row < everything.size(); ++row) {
            std::string_view const key = everything[row].view();
            bool const first_has = in_first != first.size() && first[in_first].view() == key;
            bool const second_has = in_second != second.size() && second[in_second].view() == key;
            mismatches += first_positions[row] != (first_has ? in_first : absent_position_k);
            mismatches += second_positions[row] != (second_has ? in_second : absent_position_k);
            in_first += first_has, in_second += second_has;
        }
    }
    failures += expect_eq(mismatches, std::size_t {0});
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

    /** The process's drivers, which retained device zero's primary context, or none without a device. */
    gpu_drivers_t const* drivers = nullptr;

    /** Device zero's table of the library's kernels, or none where the build links no bundle. */
    kernel_table<cuda_module_t> const* kernels = nullptr;
};

/** Loads the library's bundle onto device zero into @p tables and answers its table, or null where
 *  this build of the suite links no bundle, as its host build does. */
kernel_table<cuda_module_t> const* suite_kernels(gpu_drivers_t const& drivers,
                                                 kernel_tables<cuda_module_t>& tables) noexcept;

/** Spawns an executor on CUDA device zero with a stream of its own, its scratch capped at @p scratch_limit bytes, or
 *  refuses with @c device_k where no driver reaches the device. */
expected<cuda_executor_t> spawn_device_zero(options_t const& options, std::size_t scratch_limit) noexcept {
    if (options.drivers == nullptr || options.drivers->cuda_contexts[0] == nullptr)
        return status_t {status_code_t::device_k, "No CUDA device zero"};
    return cuda_executor_t::spawn(options.drivers->cuda, options.drivers->cuda_contexts[0], 0, gpu_class_t::unknown_k,
                                  nullptr, scratch_limit, ustore_gds_automatic_k, options.kernels);
}

/** Bounds of one-mebibyte sorted runs. */
inline constexpr sorted_run_bounds_t roomy_bounds_k {std::uint64_t {1} << 20, 4096, 4096};

/** Bounds of 64 KiB sorted runs, which cut most merges into many. */
inline constexpr sorted_run_bounds_t tight_bounds_k {64 << 10, 4096, 4096};

/** Bounds of tiny sorted runs, a page past their five sections, which hold a handful of
 *  key slots each. */
inline constexpr sorted_run_bounds_t narrow_bounds_k {24 << 10, 4096, 4096};

/** Bounds of the large cases, whose sorted runs hold 16 MiB. */
inline constexpr sorted_run_bounds_t large_bounds_k {std::uint64_t {16} << 20, 4096, 4096};

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

/** Lists every device the drivers reach, as the library's runtime opens them at the first VRAM call. */
case_result_t gpu_devices_listed() {
    std::size_t failures = 0;
    gpu_drivers_t const drivers;
    std::array<gpu_device_t, 2 * gpu_devices_capacity_k> devices {};
    std::size_t const count = list_gpu_devices(drivers, devices);
    for (std::size_t index = 0; index != count; ++index) {
        gpu_device_t const& device = devices[index];
        fmt::println("  device {}: {}, architecture {}.{}, {} compute units, {} MB", index, device.name,
                     device.architecture_major, device.architecture_minor, device.compute_units,
                     device.memory_bytes >> 20);
        failures += expect(device.vendor != gpu_vendor_t::unknown_k);
        failures += expect(device.memory_bytes != 0);
        failures += expect(device.compute_units > 0);
    }
    if (count == 0) return case_result_t::skipped("no GPU driver or device", failures);
    return failures;
}

/** Runs a compaction on the first CUDA device and expects the host's output back in every field. */
template <typename key_slot_type_>
case_result_t gpu_case(compaction_kind_t kind, sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps,
                       test_lsm_level<key_slot_type_> const& upper, test_lsm_level<key_slot_type_> const& lower,
                       compaction_output<key_slot_type_> const& host, options_t const& options) {
    if constexpr (!std::is_same_v<typename key_slot_type_::stored_key_t, st::key128_t>)
        return case_result_t::skipped("the bundle carries merge kernels for 16-byte keys alone");
    expected<cuda_executor_t> executor = spawn_device_zero(options, 0);
    if (!executor) return case_result_t::skipped("no CUDA device");
    expected<cuda_compactor_t> compactor = cuda_compactor_t::make(*executor);
    if (!compactor) return case_result_t::skipped("no CUDA compactor");
    std::size_t failures = 0;
    compaction_output<key_slot_type_> device;
    st::status_t const status = compactor->compact(kind, views_of(upper), views_of(lower), extents_of(upper, lower),
                                                   bounds, stamps, last_write_fold_t {}, no_device_fold_t {},
                                                   located_value_reader<key_slot_type_> {}, device);
    failures += expect(status == st::status_t::success_k);
    if (status == st::status_t::success_k) failures += expect_identical(host, device);

    // A warm compactor serves the same merge again out of the scratch it already grew, allocating nothing on the
    // device.
    std::uint64_t const warmed = compactor->device_allocations();
    compaction_output<key_slot_type_> again;
    failures += expect(compactor->compact(kind, views_of(upper), views_of(lower), extents_of(upper, lower), bounds,
                                          stamps, last_write_fold_t {}, no_device_fold_t {},
                                          located_value_reader<key_slot_type_> {}, again) == st::status_t::success_k);
    failures += expect_eq(compactor->device_allocations(), warmed);
    failures += expect_identical(host, again);

    // An executor whose scratch limit no merge fits refuses before it allocates, so the tree merges on the host
    // instead.
    expected<cuda_executor_t> narrow_executor = spawn_device_zero(options, 1);
    failures += expect(narrow_executor.has_value());
    if (!narrow_executor) return failures;
    expected<cuda_compactor_t> narrow = cuda_compactor_t::make(*narrow_executor);
    failures += expect(narrow.has_value());
    if (!narrow) return failures;
    compaction_output<key_slot_type_> refused;
    st::status_t const refusal = narrow->compact(kind, views_of(upper), views_of(lower), extents_of(upper, lower),
                                                 bounds, stamps, last_write_fold_t {}, no_device_fold_t {},
                                                 located_value_reader<key_slot_type_> {}, refused);
    failures += expect(refusal == st::status_t::out_of_memory_heap_k);
    failures += expect_eq(narrow->device_allocations(), std::uint64_t {0});
    return failures;
}

/** Checks one compaction of @p upper into @p lower on the host against the reference, then on the GPU against the
 *  host. */
template <typename key_slot_type_>
std::size_t report_compaction(suite_totals_t& totals, char const* host_name, char const* gpu_name,
                              compaction_kind_t kind, sorted_run_bounds_t const& bounds,
                              compaction_stamps_t const& stamps, test_lsm_level<key_slot_type_> const& upper,
                              test_lsm_level<key_slot_type_> const& lower, options_t const& options) {
    if (!totals.selects(host_name) && !totals.selects(gpu_name)) return 0;
    std::size_t failures = 0;
    vector<located_key_slot<key_slot_type_>> survivors;
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
    if (status == st::status_t::success_k)
        failures += check_compaction(kind, bounds, stamps, survivors, upper, lower, host);
    std::size_t const host_failures = report(totals, host_name, failures);
    return host_failures +
           report(totals, gpu_name, gpu_case<key_slot_type_>, kind, bounds, stamps, upper, lower, host, options);
}

/** The stamps a scenario compacts with: pins drawn from the stamps its levels carry, and bounds at fractions of
 *  their range, all widest when the scenario names none. */
template <typename key_slot_type_>
compaction_stamps_t scenario_stamps(scenario_t const& scenario, test_lsm_level<key_slot_type_> const& upper,
                                    test_lsm_level<key_slot_type_> const& lower, random_t& random,
                                    vector<std::uint64_t>& pins) noexcept {
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
    vector<stored_key_t> universe;
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
    if (status == st::status_t::success_k) status = lay_out(upper);
    if (status == st::status_t::success_k) status = lay_out(lower);
    failures += expect(status == st::status_t::success_k);
    if (failures != 0) return report(totals, host_name, failures);
    vector<std::uint64_t> pins;
    compaction_stamps_t const stamps = scenario_stamps(scenario, upper, lower, random, pins);
    return report_compaction(totals, host_name, gpu_name, scenario.kind, shaped_bounds<key_slot_type_>(scenario.bounds),
                             stamps, upper, lower, options);
}

#pragma endregion Cases

#pragma region Handmade Cases

/** Keys a handmade sorted run names by letter, from @c 'a' at the least to @c 'z' at the greatest. */
inline constexpr std::size_t lettered_keys_count_k = 26;

/** The 26 lettered 64-bit keys, ascending: both ends of the type, the sign boundary, and powers of two between. */
st::status_t append_lettered_keys(vector<std::uint64_t>& keys) noexcept {
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
st::status_t append_lettered_keys(vector<st::key128_t>& keys) noexcept {
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

/** Bounds of 128-byte sorted runs with no header and no alignment, so a handmade merge is
 *  cut into several. */
inline constexpr sorted_run_bounds_t handmade_bounds_k {128, 0, 1};

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
        std::uint32_t size = 0;
        st::status_t status = lsm_level.sorted.push_back(
            make_key_slot(std::type_identity<key_slot_type_> {}, keys[index], state, random, size));
        if (status != st::status_t::success_k) return status;
        status = lsm_level.sizes.push_back(std::uint32_t {size});
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
    vector<stored_key_t> lettered;
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
    if (status == st::status_t::success_k) status = lay_out(upper);
    if (status == st::status_t::success_k) status = lay_out(lower);
    failures += expect(status == st::status_t::success_k);
    if (failures != 0) return report(totals, host_name, failures);
    return report_compaction(totals, host_name, gpu_name, handmade.kind, shaped_bounds<key_slot_type_>(handmade.bounds),
                             compaction_stamps_t {}, upper, lower, options);
}

#pragma endregion Handmade Cases

#pragma region Graph Folds

/** The key slot a graph's merges move: a vertex's key, its value found through its run's value offsets. */
using graph_key_slot_t = variable_width_key_slot<st::key128_t>;

/** The LSM levels of a graph collection, and the values every key slot of theirs locates. */
struct graph_levels_t {

    /** The overlapping level of change deltas, each run newer than every lower one. */
    test_lsm_level<graph_key_slot_t> upper;

    /** The sorted level below it. */
    test_lsm_level<graph_key_slot_t> lower;

    /** The values of every key slot of both levels back to back, the upper level's runs first. */
    vector<byte_t> values;

    /** Where the value of every key slot starts in @c values, in the same order. */
    vector<std::uint64_t> value_begins;

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

    vector<graph::half_edge_delta_t> deltas;
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
    key_slot.stored_key = st::key128_t {0, vertex};
    std::size_t const begin = levels.values.size();
    std::uint32_t const size = static_cast<std::uint32_t>(kept * graph::half_edge_delta_bytes_k);
    st::status_t status = levels.values.resize(begin + size);
    if (status != st::status_t::success_k) return status;
    for (std::size_t index = 0; index != kept; ++index)
        graph::save_delta(deltas[index], levels.values.data() + begin + index * graph::half_edge_delta_bytes_k);
    if ((status = levels.value_begins.push_back(std::uint64_t {begin})) != st::status_t::success_k) return status;
    if ((status = level.sizes.push_back(std::uint32_t {size})) != st::status_t::success_k) return status;
    if ((status = level.sorted.push_back(graph_key_slot_t {key_slot})) != st::status_t::success_k) return status;
    if ((status = level.stamps.push_back(std::uint64_t {stamp})) != st::status_t::success_k) return status;
    return level.kinds.push_back(static_cast<std::uint8_t>(kind));
}

/** Generates the levels of @p graph_case: every run keys about half the vertices, the lower level one era older than
 *  every upper run, and lays them out. */
st::status_t make_graph_levels(graph_case_t const& graph_case, random_t& random, graph_levels_t& levels) noexcept {
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
    if ((status = lay_out(levels.upper)) != st::status_t::success_k) return status;
    return lay_out(levels.lower);
}

/** Pins @p count stamps drawn from the levels, as readers would. */
compaction_stamps_t graph_stamps(graph_levels_t const& levels, std::uint64_t count, random_t& random,
                                 vector<std::uint64_t>& pins) noexcept {
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

/** Milliseconds since @p start. */
double milliseconds_since(std::chrono::steady_clock::time_point start) noexcept {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

/** Merges the levels of @p graph_case on the host and on the first CUDA device, composing its chains with the graph's
 *  device fold, and expects identical outputs; with @p order reversed, the host reads every change's records in
 *  reverse, which the device output must expose. */
case_result_t graph_gpu_case(graph_case_t const& graph_case, std::size_t case_index, sequence_order_t order,
                             options_t const& options) {
    expected<cuda_executor_t> executor = spawn_device_zero(options, 0);
    if (!executor) return case_result_t::skipped("no CUDA device");
    expected<cuda_compactor_t> compactor = cuda_compactor_t::make(*executor);
    if (!compactor) return case_result_t::skipped("no CUDA compactor");
    std::size_t failures = 0;
    random_t random = options.random.fork(case_index);
    graph_levels_t levels;
    failures += expect(make_graph_levels(graph_case, random, levels) == st::status_t::success_k);
    if (failures != 0) return failures;
    vector<std::uint64_t> pins;
    compaction_stamps_t const stamps = graph_stamps(levels, graph_case.pinned, random, pins);

    using record_t = located_key_slot<graph_key_slot_t>;
    // The upper level's key slots were generated first, so a record's value starts where its run's first one did.
    auto const begin_of = [&levels](record_t const& record) noexcept {
        std::size_t const upper_runs = levels.upper.firsts.size();
        std::uint64_t const ordinal = record.sorted_run < upper_runs
                                          ? levels.upper.firsts[record.sorted_run]
                                          : levels.upper.sorted.size() +
                                                levels.lower.firsts[record.sorted_run - upper_runs];
        return levels.value_begins[static_cast<std::size_t>(ordinal + record.position)];
    };
    auto const read_value = [&levels, &begin_of](record_t const& record, arena_allocator<byte_t>&,
                                                 std::span<byte_t const>& bytes) noexcept {
        bytes = {levels.values.data() + begin_of(record), record.size};
        return st::status_t::success_k;
    };
    auto const read_reversed = [&levels, &begin_of](record_t const& record, arena_allocator<byte_t>& arena,
                                                    std::span<byte_t const>& bytes) noexcept {
        std::size_t const size = record.size;
        byte_t* const into = size != 0 ? arena.allocate(size) : nullptr;
        if (size != 0 && into == nullptr) return st::status_t::out_of_memory_heap_k;
        std::size_t const count = size / graph::half_edge_delta_bytes_k;
        for (std::size_t index = 0; index != count; ++index)
            std::memcpy(into + (count - 1 - index) * graph::half_edge_delta_bytes_k,
                        levels.values.data() + begin_of(record) + index * graph::half_edge_delta_bytes_k,
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

    if (failures != 0) return failures;
    compaction_output<graph_key_slot_t> device;
    std::chrono::steady_clock::time_point const device_start = std::chrono::steady_clock::now();
    status = compactor->compact(graph_case.kind, views_of(levels.upper), views_of(levels.lower),
                                extents_of(levels.upper, levels.lower), roomy_bounds_k, stamps,
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

#pragma endregion Graph Folds

#pragma region Sections By Extent

/** The cuFile disk the extent case reads run sections through, as an engine's mount holds one. */
using linux_cufile_disk_t = linux_cufile_files_disk<fu::standard_atomic_ref, fu::standard_yield_t>;

/** Appends @p bytes to @p image, zero-padded to a page, answering the extent they take in it. */
extent_t append_section(vector<byte_t>& image, std::span<byte_t const> bytes) {
    std::uint64_t const offset = image.size();
    std::uint64_t const length = round_up_to_multiple<std::uint64_t>(bytes.size(), portable_alignment_k);
    if (image.resize(static_cast<std::size_t>(offset + length)) != st::status_t::success_k) return extent_t {};
    if (!bytes.empty()) std::memcpy(image.data() + offset, bytes.data(), bytes.size());
    std::memset(image.data() + offset + bytes.size(), 0, static_cast<std::size_t>(length - bytes.size()));
    return extent_t {offset, length};
}

/** Lays every run of @p level into @p image as a run's blob lays its index, value offsets and trailer, pointing each
 *  view's extents at them, and saving @p zeroed's stamps as zeros, as an ingest writes them. */
template <typename key_slot_type_>
std::size_t lay_sections(test_lsm_level<key_slot_type_>& level,
                         std::type_identity_t<sorted_run_index_view<key_slot_type_>> const* zeroed,
                         vector<byte_t>& image) {
    std::size_t failures = expect(level.extents.resize(level.views.size()) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t run = 0; run != level.views.size(); ++run) {
        sorted_run_index_view<key_slot_type_>& view = level.views[run];
        std::uint64_t const cells_count = view.key_slots_count;
        std::size_t const count = static_cast<std::size_t>(view.key_slots_count);
        sorted_run_extents_t& extents = level.extents[run];
        extents.cells = append_section(image, {reinterpret_cast<byte_t const*>(view.cells),
                                               static_cast<std::size_t>(cells_count * sizeof(key_slot_type_))});
        extents.value_offsets = append_section(image, std::as_bytes(view.value_offsets));
        st::expected<stamps_column_t> zeros = stamps_column_t::make(count, std::uint64_t {0});
        st::expected<kinds_column_t> writes = kinds_column_t::make(count, std::uint8_t {0});
        failures += expect(zeros.has_value() && writes.has_value());
        if (failures != 0) return failures;
        stamps_column_t const& stamps = &view == zeroed || view.stamps == nullptr ? *zeros : *view.stamps;
        kinds_column_t const& kinds = view.kinds != nullptr ? *view.kinds : *writes;
        vector<byte_t> trailer;
        failures += expect(trailer.resize(stamps.saved_bytes() + kinds.saved_bytes()) == st::status_t::success_k);
        if (failures != 0) return failures;
        std::span<std::byte> const room {reinterpret_cast<std::byte*>(trailer.data()), trailer.size()};
        failures += expect(stamps.save(room) == st::status_t::success_k);
        failures += expect(kinds.save(room.subspan(stamps.saved_bytes())) == st::status_t::success_k);
        extents.trailer = append_section(image, std::span<byte_t const> {trailer});
        extents.kinds_begin = stamps.saved_bytes();
        failures += expect(!extents.cells.empty() || count == 0);
    }
    return failures;
}

/** Merges levels whose run sections read by extent through cuFile, the newest run ingested with zero stamps on disk,
 *  and expects the host's output from them, as from the same merge uploading every run from memory, timing both. */
case_result_t gpu_reads_sections_by_extent(options_t const& options, std::string_view directory) {
    using key_slot_t = variable_width_key_slot<st::key128_t>;
    if (directory.empty()) return case_result_t::skipped(needs_directory_k);
    if (options.drivers == nullptr) return case_result_t::skipped("no CUDA device");
    linux_cufile_probe_t const probe = linux_cufile_disk_t::probe(options.drivers->cufile);
    if (probe == linux_cufile_probe_t::absent_k || probe == linux_cufile_probe_t::refused_k)
        return case_result_t::skipped("needs cuFile in direct or compat mode");
    expected<cuda_executor_t> executor = spawn_device_zero(options, 0);
    if (!executor) return case_result_t::skipped("no CUDA device");

    std::size_t failures = 0;
    scenario_t const& scenario = scenarios_k[8];
    random_t random = options.random.fork(scenarios_k.size() * 2);
    vector<st::key128_t> universe;
    failures += expect(make_universe(options.large_universe.value, random, universe) == st::status_t::success_k);
    test_lsm_level<key_slot_t> upper;
    test_lsm_level<key_slot_t> lower;
    std::span<st::key128_t const> const keys {universe};
    st::status_t status = add_overlapping_sorted_runs(keys, scenario, scenario.upper_sorted_runs, 1, random, upper);
    if (status == st::status_t::success_k)
        status = add_sorted_sorted_runs(keys, scenario, scenario.lower_sorted_runs, 0, random, lower);
    if (status == st::status_t::success_k) status = lay_out(upper);
    if (status == st::status_t::success_k) status = lay_out(lower);
    failures += expect(status == st::status_t::success_k && !upper.views.empty());
    if (failures != 0) return failures;

    // The newest upper run is ingested: one stamp above every other in memory, and zeros in its blob
    std::uint64_t newest = 0;
    for (std::uint64_t const stamp : upper.stamps) newest = stamp > newest ? stamp : newest;
    for (std::uint64_t const stamp : lower.stamps) newest = stamp > newest ? stamp : newest;
    sorted_run_index_view<key_slot_t>& ingested = upper.views[upper.views.size() - 1];
    st::expected<stamps_column_t> assigned = stamps_column_t::make(static_cast<std::size_t>(ingested.key_slots_count),
                                                                   newest + 1);
    failures += expect(assigned.has_value());
    if (failures != 0) return failures;
    stamps_column_t const& assigned_column = *assigned;
    ingested.stamps = &assigned_column;

    vector<byte_t> image;
    sorted_run_bounds_t const bounds = shaped_bounds<key_slot_t>(scenario.bounds);
    failures += lay_sections(upper, &ingested, image);
    failures += lay_sections(lower, nullptr, image);
    if (failures != 0) return failures;
    upper.extents[upper.extents.size() - 1].assigned_stamp = newest + 1;
    std::string const parent {directory};
    path_t const path {parent.c_str(), "sections.bin"};
    std::FILE* const file = std::fopen(path.c_str(), "wb");
    failures += expect(file != nullptr);
    if (failures != 0) return failures;
    failures += expect(std::fwrite(image.data(), 1, image.size(), file) == image.size());
    failures += expect(std::fclose(file) == 0);

    linux_cufile_disk_config_t config;
    config.gds = probe == linux_cufile_probe_t::direct_k ? ustore_gds_direct_k : ustore_gds_compatibility_k;
    expected<linux_cufile_disk_t> disk = linux_cufile_disk_t::make(options.drivers->cufile, config);
    failures += expect_status(disk, status_code_t::success_k);
    if (failures != 0) return failures;
    medium_options_t request;
    request.name = path.c_str();
    request.access = medium_access_t::read_only_k;

    vector<std::uint64_t> pins;
    compaction_stamps_t const stamps = scenario_stamps(scenario, upper, lower, random, pins);
    auto const read_nothing = [](located_key_slot<key_slot_t> const&, arena_allocator<byte_t>&,
                                 std::span<byte_t const>&) noexcept { return st::status_t::invalid_argument_k; };
    compaction_output<key_slot_t> host;
    failures += expect(host_compactor_t {}.compact(scenario.kind, views_of(upper), views_of(lower), bounds, stamps,
                                                   last_write_fold_t {}, read_nothing,
                                                   host) == st::status_t::success_k);
    {
        using reader_t = ring_sections_reader<linux_cufile_disk_t, fu::standard_yield_t>;
        expected<reader_t> reader = reader_t::make(std::move(*disk), std::span<medium_options_t const> {&request, 1});
        failures += expect_status(reader, status_code_t::success_k);
        if (failures != 0) return failures;
        reader_t& sections = *reader;
        expected<cuda_compactor_t> by_extent = cuda_compactor_t::make(*executor, device_sections_reader_t {&sections});
        expected<cuda_compactor_t> from_memory = cuda_compactor_t::make(*executor);
        failures += expect(by_extent.has_value() && from_memory.has_value());
        if (failures != 0) return failures;
        cuda_compactor_t& extent_compactor = *by_extent;
        cuda_compactor_t& memory_compactor = *from_memory;

        // The first merge of each grows its scratch, so the second is the one timed
        double milliseconds[2] {};
        cuda_compactor_t* const compactors[2] {&extent_compactor, &memory_compactor};
        for (std::size_t index = 0; index != 2; ++index)
            for (std::size_t round = 0; round != 2; ++round) {
                compaction_output<key_slot_t> device;
                std::chrono::steady_clock::time_point const start = std::chrono::steady_clock::now();
                status = compactors[index]->compact(scenario.kind, views_of(upper), views_of(lower),
                                                    extents_of(upper, lower), bounds, stamps, last_write_fold_t {},
                                                    no_device_fold_t {}, located_value_reader<key_slot_t> {}, device);
                milliseconds[index] = milliseconds_since(start);
                failures += expect(status == st::status_t::success_k);
                if (status == st::status_t::success_k) failures += expect_identical(host, device);
            }
        fmt::println("  {} key slots in {} MB of sections: by extent {:.1f} ms, from memory {:.1f} ms",
                     upper.sorted.size() + lower.sorted.size(), image.size() >> 20, milliseconds[0], milliseconds[1]);

        // Without its assigned stamp, the ingested run's zeros on disk would surface in the merge
        upper.extents[upper.extents.size() - 1].assigned_stamp = 0;
        compaction_output<key_slot_t> unassigned;
        failures += expect(by_extent->compact(scenario.kind, views_of(upper), views_of(lower), extents_of(upper, lower),
                                              bounds, stamps, last_write_fold_t {}, no_device_fold_t {},
                                              located_value_reader<key_slot_t> {},
                                              unassigned) == st::status_t::success_k);
        failures += expect(differences(host, unassigned) != 0);
    }
    return failures;
}

#pragma endregion Sections By Extent

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
    gpu_drivers_t drivers;
    kernel_tables<cuda_module_t> kernels;
    if (drivers.retain_context(0)) options.drivers = &drivers, options.kernels = suite_kernels(drivers, kernels);

    report(totals, "compaction.uuid_orders_like_memcmp", uuid_orders_like_memcmp, options);
    report(totals, "compaction.survivor_first_orders_the_survivor_first", survivor_first_orders_the_survivor_first);
    report(totals, "compaction.survives_follows_the_kind", survives_follows_the_kind);
    report(totals, "compaction.plan_sorted_runs_cuts_at_the_bounds", plan_sorted_runs_cuts_at_the_bounds);
    report(totals, "compaction.union_sorted_keys_matches_brute_force", union_sorted_keys_matches_brute_force);
    report(totals, "compaction.survives_keeps_what_readers_read", survives_keeps_what_readers_read);
    report(totals, "gpu.devices_listed", gpu_devices_listed);
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
    report(totals, "gpu.reads_sections_by_extent", gpu_reads_sections_by_extent, options, suite.directory);
    // Reading every change's records in reverse must show in the output, or parity proves nothing about the order
    report(totals, "graph.reversed_sequences_differ", graph_gpu_case, graph_cases_k[1], std::size_t {1},
           sequence_order_t::reversed_k, options);
    return suite_exit_code(totals);
}
