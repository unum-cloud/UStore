/**
 *  @file test/blobs/nvme_memory_run.cpp
 *  @author Ash Vardanian
 *  @date October 18, 2026
 *  @brief Unit tests of `ustore/blobs/nvme_memory_run.hpp` and `ustore/blobs/nvme_collection_view.hpp`: what sealing
 *      refuses, reads at every stamp and the conflict checks over random commit streams against a reference model, a
 *      map of every version of every key, and readers walking views while one thread publishes.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcmp`, `std::memcpy`

#include <algorithm> // `std::sort`, `std::upper_bound`
#include <array>     // `std::array`
#include <map>       // `std::map`
#include <memory>    // `std::addressof`, `std::make_unique`, `std::unique_ptr`
#include <optional>  // `std::optional`
#include <span>      // `std::span`
#include <thread>    // `std::thread`
#include <utility>   // `std::exchange`
#include <vector>    // `std::vector`

#include <fmt/format.h> // `fmt::println`

#include <forkunion/atomics.hpp> // `fu::standard_atomic_ref`
#include <forkunion/types.hpp>   // `fu::standard_yield_t`
#include <smashtable/shared.hpp> // `st::commit_order_t`

#include "ustore/blobs/nvme_collection_view.hpp" // `collection_view`, `published_view`, `epoch_domain`
#include "ustore/blobs/nvme_memory_run.hpp"      // `keyed_delta`, `memory_run`, `sealed_deltas`
#include "ustore/utilities/algorithm.hpp"        // `sort_by_stored_key`

#include "harness.hpp" // `expect_true`, `run_test_case`, `split_mix_generator_t`

namespace unum::ustore::test {

using namespace unum::ustore::blobs;

using stored_key_t = std::uint64_t;
using keyed_delta_t = keyed_delta<stored_key_t>;
using sealed_t = sealed_deltas<stored_key_t>;
using run_t = memory_run<stored_key_t>;
using view_t = collection_view<run_t>;

/** The atomic reference every suite instantiates the engine's templates with: the portable one, as
 *  no suite picks a CPU class. */
template <typename value_type_>
using atomic_reference = fu::standard_atomic_ref<value_type_>;

/** The waiter every suite instantiates the engine's templates with, yielding between attempts. */
using waiter_t = fu::standard_yield_t;

/** The epochs every case announces its readers in. */
using epoch_domain_t = epoch_domain<atomic_reference, waiter_t>;
using bytes_t = std::vector<byte_t>;

#pragma region Model

/** One version of a key as the model keeps it. */
struct model_version_t {
    transaction_stamp_t stamp = unwritten_stamp_k;
    delta_kind_t kind = delta_kind_t::write_k;
    bytes_t bytes;
};

/** Every version of every key, oldest first, which every view must answer alike. */
using model_t = std::map<stored_key_t, std::vector<model_version_t>>;

/** The newest version of @p stored_key at or below @p at, or null. */
static model_version_t const* model_find(model_t const& model, stored_key_t stored_key, transaction_stamp_t at) {
    model_t::const_iterator const found = model.find(stored_key);
    if (found == model.end()) return nullptr;
    for (std::size_t index = found->second.size(); index-- != 0;)
        if (found->second[index].stamp <= at) return &found->second[index];
    return nullptr;
}

/** The stamp of the newest version of @p stored_key, or @c unwritten_stamp_k for a key never written. */
static transaction_stamp_t model_newest(model_t const& model, stored_key_t stored_key) {
    model_t::const_iterator const found = model.find(stored_key);
    return found == model.end() ? unwritten_stamp_k : found->second.back().stamp;
}

/** Whether a view's answer agrees with the model's version, or with its absence. */
static bool agrees(bool found, delta_view_t const& delta, model_version_t const* version) {
    if (!found || !version) return expect_true(found == (version != nullptr));
    if (!expect_true(delta.kind == version->kind && delta.bytes.size() == version->bytes.size())) return false;
    return expect_true(delta.bytes.empty() ||
                       std::memcmp(delta.bytes.data(), version->bytes.data(), delta.bytes.size()) == 0);
}

#pragma endregion Model

#pragma region Cases

/** Sealing refuses what a run cannot hold, and a view refuses a run that is not newer than all it names. */
static void test_refusals() {
    bytes_t const payload(4, byte_t {7});
    std::vector<keyed_delta_t> none;
    expect_true(sealed_t::seal(none).code() == status_code_t::invalid_argument_k);

    std::vector<keyed_delta_t> repeated {{3, {delta_kind_t::write_k, payload}}, {3, {delta_kind_t::remove_k, {}}}};
    expect_true(sealed_t::seal(repeated).code() == status_code_t::invalid_argument_k);

    std::vector<keyed_delta_t> loaded_removal {{3, {delta_kind_t::remove_k, payload}}};
    expect_true(sealed_t::seal(loaded_removal).code() == status_code_t::invalid_argument_k);

    std::vector<keyed_delta_t> deltas {{3, {delta_kind_t::write_k, payload}}};
    auto [sealed, seal_status] = sealed_t::seal(deltas);
    if (!expect_true(seal_status.succeeded())) return;
    expect_true(run_t::stamped(std::move(sealed), unwritten_stamp_k).code() == status_code_t::invalid_argument_k);

    std::vector<keyed_delta_t> newer {{3, {delta_kind_t::write_k, payload}}};
    std::vector<keyed_delta_t> older {{4, {delta_kind_t::write_k, payload}}};
    auto [newer_run, stamp_newer_status] = run_t::stamped(*sealed_t::seal(newer), 9);
    auto [older_run, stamp_older_status] = run_t::stamped(*sealed_t::seal(older), 9);
    if (!expect_true(stamp_newer_status.succeeded() && stamp_older_status.succeeded())) return;
    view_t const empty;
    auto [holding, extend_status] = view_t::extended(empty, newer_run);
    if (!expect_true(extend_status.succeeded())) return;
    expect_true(view_t::extended(holding, older_run).code() == status_code_t::invalid_argument_k);
}

/** A removal and a write of no bytes read back apart, which a sentinel offset or size used to decide. */
static void test_removal_and_empty_value_differ() {
    std::vector<keyed_delta_t> deltas {{20, {delta_kind_t::remove_k, {}}}, {10, {delta_kind_t::write_k, {}}}};
    auto [sealed, seal_status] = sealed_t::seal(deltas);
    if (!expect_true(seal_status.succeeded())) return;
    auto [stamped, stamp_status] = run_t::stamped(std::move(sealed), 5);
    if (!expect_true(stamp_status.succeeded())) return;
    view_t const empty;
    auto [view, extend_status] = view_t::extended(empty, stamped);
    if (!expect_true(extend_status.succeeded())) return;

    delta_view_t found;
    expect_true(view.find(10, 5, found) && found.kind == delta_kind_t::write_k && found.bytes.empty());
    expect_true(view.find(20, 5, found) && found.kind == delta_kind_t::remove_k && found.bytes.empty());
    expect_true(!view.find(10, 4, found) && !view.find(30, 5, found));
    expect_true(view.written_since(20, 4) && !view.written_since(20, 5) && !view.written_since(30, 0));
    expect_true(view.range_written_since(11, 20, 4) && !view.range_written_since(11, 19, 4));
}

/** Sorting orders deltas by key, by insertion below 64 entries and by radix from there, through
 *  bytes every key shares and runs of one repeated key, as a library sort over the same 16-byte
 *  keys orders them. */
static void test_sort_orders_by_key() {
    using wide_delta_t = keyed_delta<st::key128_t>;
    split_mix_generator_t generator {case_stream_key()};
    for (std::size_t const count : {std::size_t {20}, std::size_t {1000}}) {
        std::vector<wide_delta_t> deltas(count);
        for (std::size_t index = 0; index != count; ++index) {
            std::uint64_t const high = generator.below(4) << 40;
            std::uint64_t const low = generator.below(3) << 16;
            deltas[index] = wide_delta_t {{high, low}, {delta_kind_t::remove_k, {}}};
        }
        std::vector<wide_delta_t> reference = deltas;
        std::sort(reference.begin(), reference.end(), [](wide_delta_t const& first, wide_delta_t const& second) {
            return first.stored_key < second.stored_key;
        });
        util::sort_by_stored_key(std::span<wide_delta_t> {deltas});
        std::size_t mismatched = 0;
        for (std::size_t index = 0; index != count; ++index)
            mismatched += deltas[index].stored_key != reference[index].stored_key;
        expect_true(mismatched == 0);
    }
}

/** A range tombstone hides the older versions of its keys from a reader at or past its stamp,
 *  conflicts with a watch below it, and a merge of the runs drops the versions it hides from
 *  every reader while a pinned reader keeps its own; a write the same commit lands inside the
 *  range survives it. */
static void test_range_tombstones_hide_older_runs() {
    using range_t = sealed_t::range_t;
    using versions_t = st::vector<stamped_delta<stored_key_t>>;
    std::array<byte_t, 3> const payload {byte_t {5}, byte_t {5}, byte_t {5}};
    std::array<keyed_delta_t, 3> oldest {keyed_delta_t {10, {delta_kind_t::write_k, payload}},
                                         keyed_delta_t {20, {delta_kind_t::write_k, payload}},
                                         keyed_delta_t {30, {delta_kind_t::write_k, payload}}};
    std::array<keyed_delta_t, 1> ranged {keyed_delta_t {25, {delta_kind_t::write_k, payload}}};
    std::array<keyed_delta_t, 1> newest {keyed_delta_t {30, {delta_kind_t::write_k, payload}}};
    range_t const range {15, 35};
    expect_true(sealed_t::seal(std::span<keyed_delta_t> {}, std::span<range_t const> {&range, 1}).has_value());
    range_t const backwards {35, 15};
    expect_true(sealed_t::seal(ranged, std::span<range_t const> {&backwards, 1}).code() ==
                status_code_t::invalid_argument_k);
    auto [first, first_status] = run_t::stamped(*sealed_t::seal(oldest), 5);
    auto [second, second_status] = run_t::stamped(*sealed_t::seal(ranged, std::span<range_t const> {&range, 1}), 7);
    auto [third, third_status] = run_t::stamped(*sealed_t::seal(newest), 9);
    if (!expect_true(first_status.succeeded() && second_status.succeeded() && third_status.succeeded())) return;
    expect_true(second.newest_stamp() == 7 && second.range_tombstones().size() == 1);

    view_t const empty;
    auto [one, extend_one_status] = view_t::extended(empty, first);
    if (!expect_true(extend_one_status.succeeded())) return;
    auto [two, extend_two_status] = view_t::extended(one, second);
    if (!expect_true(extend_two_status.succeeded())) return;
    auto [view, extend_status] = view_t::extended(two, third);
    if (!expect_true(extend_status.succeeded())) return;
    expect_true(view.covering_stamp(20, 9) == 7 && view.covering_stamp(20, 6) == unwritten_stamp_k);
    expect_true(view.covering_stamp(10, 9) == unwritten_stamp_k && view.covering_stamp(35, 9) == 0);
    expect_true(view.written_since(20, 6) && !view.written_since(20, 7) && !view.written_since(10, 5));
    expect_true(view.range_written_since(16, 18, 6) && !view.range_written_since(16, 18, 7));

    run_t const* const runs[3] {std::addressof(third), std::addressof(second), std::addressof(first)};
    auto const kept_keys = [&](compaction_stamps_t const& survival, std::span<stored_key_t const> wanted) {
        versions_t versions;
        // The last-write fold composes nothing, so it never allocates from the arena it is given.
        util::arena_allocator<byte_t> folded_bytes;
        if (!expect_true(kept_versions(std::span<run_t const* const> {runs}, survival, util::last_write_fold_t {},
                                       versions, folded_bytes)
                             .succeeded()))
            return;
        expect_true(versions.size() == wanted.size());
        for (std::size_t index = 0; index != versions.size() && index != wanted.size(); ++index)
            expect_true(versions[index].stored_key == wanted[index]);
    };
    // With no reader, key 20 and the older 30 drop; a reader at 6 still reads both.
    std::array<stored_key_t, 3> const newest_kept {10, 25, 30};
    kept_keys(compaction_stamps_t {}, newest_kept);
    std::uint64_t const pinned[1] {6};
    compaction_stamps_t survival;
    survival.pinned = pinned;
    survival.pinned_count = 1;
    std::array<stored_key_t, 5> const pinned_kept {10, 20, 25, 30, 30};
    kept_keys(survival, pinned_kept);

    auto [tombstones, tombstones_status] = gathered_range_tombstones(std::span<run_t const* const> {runs},
                                                                     st::default_allocator<byte_t> {});
    expect_true(tombstones_status.succeeded() && tombstones.size() == 1 && tombstones.stamp(0) == 7);
}

/** Random commits, each one run on top of the last view, read at random stamps and checked for conflicts against
 *  the model; every older view keeps answering at its own stamp. */
static void test_random_streams_match_model() {
    constexpr std::size_t commits_count_k = 400;
    constexpr stored_key_t keys_count_k = 64;
    constexpr std::size_t probes_per_commit_k = 24;
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        split_mix_generator_t generator {case_stream_key(seed)};

        model_t model;
        // Reserved whole, so the runs every view names never move.
        std::vector<run_t> runs;
        std::vector<view_t> views;
        runs.reserve(commits_count_k);
        views.reserve(commits_count_k + 1);
        views.emplace_back();
        for (transaction_stamp_t stamp = 1; stamp <= commits_count_k; ++stamp) {
            std::vector<keyed_delta_t> deltas;
            std::vector<bytes_t> payloads;
            std::size_t const wanted = 1 + generator.below(8);
            payloads.reserve(wanted);
            for (std::size_t index = 0; index != wanted; ++index) {
                stored_key_t const stored_key = generator.below(keys_count_k);
                bool repeated = false;
                for (keyed_delta_t const& delta : deltas) repeated |= delta.stored_key == stored_key;
                if (repeated) continue;
                std::size_t const kind_roll = generator.below(10);
                delta_kind_t const kind = kind_roll < 6   ? delta_kind_t::write_k
                                          : kind_roll < 9 ? delta_kind_t::remove_k
                                                          : delta_kind_t::change_k;
                bytes_t& payload = payloads.emplace_back();
                if (kind != delta_kind_t::remove_k) payload.resize(generator.below(17));
                for (byte_t& value : payload) value = static_cast<byte_t>(generator.below(256));
                deltas.push_back(keyed_delta_t {stored_key, {kind, payload}});
                model[stored_key].push_back(model_version_t {stamp, kind, payload});
            }

            auto [sealed, seal_status] = sealed_t::seal(deltas);
            if (!expect_true(seal_status.succeeded())) return;
            auto [stamped, stamp_status] = run_t::stamped(std::move(sealed), stamp);
            if (!expect_true(stamp_status.succeeded())) return;
            runs.push_back(std::move(stamped));
            auto [extended, extend_status] = view_t::extended(views.back(), runs.back());
            if (!expect_true(extend_status.succeeded())) return;
            views.push_back(std::move(extended));
            view_t const& view = views.back();
            if (!expect_true(view.newest_stamp() == stamp && view.runs().size() == stamp)) return;

            for (std::size_t probe = 0; probe != probes_per_commit_k; ++probe) {
                // A few keys past the written ones are probed and never found.
                stored_key_t const stored_key = generator.below(keys_count_k + 8);
                transaction_stamp_t const at = generator.below(stamp + 1);
                delta_view_t found;
                bool const answered = view.find(stored_key, at, found);
                if (!agrees(answered, found, model_find(model, stored_key, at))) return;

                transaction_stamp_t const from = generator.below(stamp + 1);
                if (!expect_true(view.written_since(stored_key, from) == (model_newest(model, stored_key) > from)))
                    return;

                stored_key_t const other = generator.below(keys_count_k + 8);
                stored_key_t const lower = fu::min_of(stored_key, other);
                stored_key_t const upper = fu::max_of(stored_key, other);
                bool range_written = false;
                for (stored_key_t key = lower; key <= upper; ++key) range_written |= model_newest(model, key) > from;
                if (!expect_true(view.range_written_since(lower, upper, from) == range_written)) return;

                // The view published at an older stamp still answers at that stamp, whatever came after it.
                transaction_stamp_t const published_stamp = generator.below(stamp + 1);
                bool const answered_then = views[published_stamp].find(stored_key, head_stamp_k, found);
                if (!agrees(answered_then, found, model_find(model, stored_key, published_stamp))) return;
            }
        }
    }
}

/** Appends a change's bytes to the value below it, into bytes the case frees once a merge returns,
 *  so a merged run must copy every folded value it keeps. */
struct appending_fold_t {
    std::vector<bytes_t>* folded = nullptr;

    expected<std::span<byte_t const>> apply(std::span<byte_t const> base, std::span<delta_view_t const> deltas,
                                            util::arena_allocator<byte_t>&) const noexcept {
        bytes_t& joined = folded->emplace_back(base.begin(), base.end());
        for (delta_view_t const& delta : deltas) {
            if (delta.kind != delta_kind_t::change_k) joined.clear();
            joined.insert(joined.end(), delta.bytes.begin(), delta.bytes.end());
        }
        return std::span<byte_t const> {joined};
    }

    expected<std::span<byte_t const>> compose(std::span<delta_view_t const> deltas,
                                              util::arena_allocator<byte_t>& arena) const noexcept {
        return apply({}, deltas, arena);
    }

    bool closes(delta_view_t const& delta) const noexcept { return delta.kind != delta_kind_t::change_k; }
};

/** The value of @p stored_key at the head of @p runs, newest first, with its changes appended, or
 *  none. */
static std::optional<bytes_t> value_at_head(std::span<run_t const* const> runs, stored_key_t stored_key) {
    std::vector<delta_view_t> chain;
    auto gather = [&]() {
        for (run_t const* run : runs)
            for (std::size_t position = run->lower_bound(stored_key);
                 position != run->size() && run->stored_key(position) == stored_key; ++position) {
                chain.push_back(run->delta(position));
                if (chain.back().kind != delta_kind_t::change_k) return;
            }
    };
    gather();
    std::optional<bytes_t> value;
    for (std::size_t index = chain.size(); index-- != 0;) {
        if (chain[index].kind == delta_kind_t::remove_k) value.reset();
        else if (chain[index].kind == delta_kind_t::write_k || !value) value.emplace();
        if (chain[index].kind != delta_kind_t::remove_k)
            value->insert(value->end(), chain[index].bytes.begin(), chain[index].bytes.end());
    }
    return value;
}

/** Commits of writes, removals and changes, every other one lending its bytes, merge four runs of a
 *  tier at a time up to tier three, each merge copying only the values it folded and freeing the
 *  runs it took the buffers of, and every key reads back as the model. */
static void test_merges_keep_values_intact() {
    constexpr std::size_t fan_in_k = 4;
    constexpr std::size_t commits_count_k = fan_in_k * fan_in_k * fan_in_k;
    constexpr stored_key_t keys_count_k = 24;
    split_mix_generator_t generator {case_stream_key()};

    std::map<stored_key_t, std::optional<bytes_t>> model;
    // Oldest first, so the runs of one tier due for a merge sit at the back.
    std::vector<std::unique_ptr<run_t>> runs;
    auto newest_first = [&]() {
        std::vector<run_t const*> listed;
        for (std::size_t index = runs.size(); index-- != 0;) listed.push_back(runs[index].get());
        return listed;
    };
    for (transaction_stamp_t stamp = 1; stamp <= commits_count_k; ++stamp) {
        std::vector<keyed_delta_t> deltas;
        std::vector<bytes_t> payloads(1 + generator.below(6));
        for (bytes_t& payload : payloads) {
            stored_key_t const stored_key = generator.below(keys_count_k);
            bool repeated = false;
            for (keyed_delta_t const& delta : deltas) repeated |= delta.stored_key == stored_key;
            if (repeated) continue;
            std::size_t const kind_roll = generator.below(20);
            delta_kind_t const kind = kind_roll < 12   ? delta_kind_t::write_k
                                      : kind_roll < 15 ? delta_kind_t::remove_k
                                                       : delta_kind_t::change_k;
            if (kind != delta_kind_t::remove_k) payload.resize(generator.below(49));
            for (byte_t& value : payload) value = static_cast<byte_t>(generator.below(256));
            deltas.push_back(keyed_delta_t {stored_key, {kind, payload}});
            std::optional<bytes_t>& modelled = model[stored_key];
            if (kind == delta_kind_t::remove_k) modelled.reset();
            else if (kind == delta_kind_t::write_k || !modelled) modelled = payload;
            else modelled->insert(modelled->end(), payload.begin(), payload.end());
        }
        // Every other commit lends its bytes, as a transaction of one collection does, to the run.
        sealed_t::buffer_t lent;
        if (stamp % 2 == 0) {
            std::size_t filled = 0;
            for (keyed_delta_t const& delta : deltas) filled += delta.delta.bytes.size();
            if (!expect_true(lent.resize(filled) == st::status_t::success_k)) return;
            filled = 0;
            for (keyed_delta_t& delta : deltas) {
                std::size_t const size = delta.delta.bytes.size();
                if (size == 0) continue;
                std::memcpy(lent.data() + filled, delta.delta.bytes.data(), size);
                delta.delta.bytes = std::span<byte_t const> {lent.data() + filled, size};
                filled += size;
            }
        }
        byte_t const* const lent_bytes = lent.data();
        auto [sealed, seal_status] = sealed_t::seal(deltas, {}, {}, std::span<byte_t const> {lent});
        if (!expect_true(seal_status.succeeded())) return;
        sealed.adopt(lent);
        if (!expect_true(lent_bytes == nullptr || (lent.empty() && sealed.bases().front() == lent_bytes))) return;
        auto [stamped, stamp_status] = run_t::stamped(std::move(sealed), stamp);
        if (!expect_true(stamp_status.succeeded())) return;
        runs.push_back(std::make_unique<run_t>(std::move(stamped)));

        while (runs.size() >= fan_in_k && runs[runs.size() - fan_in_k]->tier() == runs.back()->tier()) {
            std::vector<run_t const*> const listed = newest_first();
            std::span<run_t const* const> const group {listed.data(), fan_in_k};
            st::vector<stamped_delta<stored_key_t>> versions;
            std::vector<bytes_t> folded;
            util::arena_allocator<byte_t> unused;
            if (!expect_true(kept_versions(group, compaction_stamps_t {}, appending_fold_t {&folded}, versions, unused)
                                 .succeeded()))
                return;
            auto [tombstones, tombstones_status] = gathered_range_tombstones(group, st::default_allocator<byte_t> {});
            if (!expect_true(tombstones_status.succeeded())) return;
            auto [merged, merge_status] = run_t::merged(group, std::span<stamped_delta<stored_key_t> const> {versions},
                                                        std::move(tombstones), group.back()->oldest_stamp(),
                                                        group.back()->tier());
            if (!expect_true(merge_status.succeeded())) return;
            for (std::size_t index = runs.size() - fan_in_k; index != runs.size(); ++index)
                merged.inherit(std::move(*runs[index]));
            runs.resize(runs.size() - fan_in_k);
            runs.push_back(std::make_unique<run_t>(std::move(merged)));
        }

        std::vector<run_t const*> const listed = newest_first();
        for (stored_key_t stored_key = 0; stored_key != keys_count_k; ++stored_key) {
            std::optional<bytes_t> const read = value_at_head(std::span<run_t const* const> {listed}, stored_key);
            std::map<stored_key_t, std::optional<bytes_t>>::const_iterator const found = model.find(stored_key);
            if (!expect_true(read == (found == model.end() ? std::nullopt : found->second))) return;
        }
    }
    expect_true(runs.size() == 1 && runs.front()->tier() == 3);
}

/** A view a publisher allocated and frees once replaced, counting itself out when freed. */
struct held_view_t {
    view_t view;
    std::size_t* alive = nullptr;
    ~held_view_t() noexcept { atomic_reference<std::size_t> {*alive}.fetch_sub(1, std::memory_order_relaxed); }
};

/**
 *  @brief Three readers walk two collections' views while one thread commits into both at once, so a reader at a
 *      published stamp must find every commit at or below it, in both collections alike.
 *
 *  Commit @c s writes key @c s*13 modulo the key count in both collections, removing it on every seventh commit and
 *  writing the eight bytes of @c s otherwise, so a reader derives the answer at any stamp without sharing state.
 *  The publisher waits out the readers after every commit and frees both replaced views before the next, while one
 *  reader announces on an executor slot, the others on shared slots, and every reader nests a second announcement.
 */
static void test_readers_against_one_publisher() {
    constexpr std::size_t commits_count_k = 2000;
    constexpr stored_key_t keys_count_k = 64;
    constexpr std::size_t readers_count_k = 3;
    auto key_of = [](transaction_stamp_t stamp) noexcept { return stored_key_t {(stamp * 13) % keys_count_k}; };
    auto kind_of = [](transaction_stamp_t stamp) noexcept {
        return stamp % 7 == 0 ? delta_kind_t::remove_k : delta_kind_t::write_k;
    };

    // Built before any thread starts and only read afterwards: the stamps writing each key, ascending.
    std::vector<std::vector<transaction_stamp_t>> writers(keys_count_k);
    for (transaction_stamp_t stamp = 1; stamp <= commits_count_k; ++stamp) writers[key_of(stamp)].push_back(stamp);

    st::commit_order_t order;
    epoch_domain_t epochs;
    expect_true(epochs.reserve_threads(1).succeeded());
    std::size_t alive {0};
    auto held = [&]() noexcept {
        held_view_t* const made = new held_view_t {};
        made->alive = &alive;
        atomic_reference<std::size_t> {alive}.fetch_add(1, std::memory_order_relaxed);
        return made;
    };

    // Runs stay alive until the threads join, as only a flush frees them; views are freed as they are replaced.
    // Both are reserved whole, so the runs every view names never move.
    std::vector<run_t> first_runs, second_runs;
    first_runs.reserve(commits_count_k);
    second_runs.reserve(commits_count_k);
    held_view_t* first_current = held();
    held_view_t* second_current = held();
    published_view<view_t, atomic_reference> first, second;
    [[maybe_unused]] view_t const* const first_empty = first.replace(first_current->view);
    [[maybe_unused]] view_t const* const second_empty = second.replace(second_current->view);

    auto publish = [&]() noexcept {
        for (transaction_stamp_t stamp = 1; stamp <= commits_count_k; ++stamp) {
            bytes_t payload(sizeof(stamp));
            std::memcpy(payload.data(), &stamp, sizeof(stamp));
            delta_kind_t const kind = kind_of(stamp);
            std::span<byte_t const> const bytes = kind == delta_kind_t::remove_k ? std::span<byte_t const> {}
                                                                                 : std::span<byte_t const>(payload);
            keyed_delta_t first_delta {key_of(stamp), {kind, bytes}};
            keyed_delta_t second_delta {key_of(stamp), {kind, bytes}};
            auto [first_sealed, seal_first_status] = sealed_t::seal(std::span<keyed_delta_t>(&first_delta, 1));
            auto [second_sealed, seal_second_status] = sealed_t::seal(std::span<keyed_delta_t>(&second_delta, 1));
            if (!expect_true(seal_first_status.succeeded() && seal_second_status.succeeded())) return;

            st::commit_order_t::commit_in_flight_t in_flight;
            order.begin_commit(in_flight);
            transaction_stamp_t const drawn = static_cast<transaction_stamp_t>(in_flight.stamp());
            if (!expect_true(drawn == stamp)) return;
            auto [first_run, stamp_first_status] = run_t::stamped(std::move(first_sealed), drawn);
            auto [second_run, stamp_second_status] = run_t::stamped(std::move(second_sealed), drawn);
            if (!expect_true(stamp_first_status.succeeded() && stamp_second_status.succeeded())) return;
            first_runs.push_back(std::move(first_run));
            second_runs.push_back(std::move(second_run));
            auto [first_view, extend_first_status] = view_t::extended(first_current->view, first_runs.back());
            auto [second_view, extend_second_status] = view_t::extended(second_current->view, second_runs.back());
            if (!expect_true(extend_first_status.succeeded() && extend_second_status.succeeded())) return;
            held_view_t* const first_next = held();
            held_view_t* const second_next = held();
            first_next->view = std::move(first_view);
            second_next->view = std::move(second_view);
            [[maybe_unused]] view_t const* const first_replaced = first.replace(first_next->view);
            [[maybe_unused]] view_t const* const second_replaced = second.replace(second_next->view);
            order.end_commit(in_flight);
            epochs.wait_for_readers();
            delete std::exchange(first_current, first_next);
            delete std::exchange(second_current, second_next);
            // Nothing replaced outlives the commit that replaced it.
            if (!expect_true(atomic_reference<std::size_t> {alive}.load(std::memory_order_relaxed) == 2)) return;
        }
    };

    std::vector<std::size_t> reader_reads(readers_count_k, 0);
    auto read = [&](std::size_t reader) noexcept {
        split_mix_generator_t generator {case_stream_key(reader + 1)};
        for (transaction_stamp_t at = 0; at != commits_count_k;) {
            epoch_domain_t::reader_t const inside = reader == 0 ? epochs.enter(0) : epochs.enter();
            epoch_domain_t::reader_t const nested = epochs.enter();
            at = static_cast<transaction_stamp_t>(order.published_stamp());
            view_t const* const first_view = first.load();
            view_t const* const second_view = second.load();
            if (!expect_true(first_view->newest_stamp() >= at && second_view->newest_stamp() >= at)) return;

            stored_key_t const stored_key = generator.below(keys_count_k);
            std::vector<transaction_stamp_t> const& stamps = writers[stored_key];
            std::vector<transaction_stamp_t>::const_iterator const after = std::upper_bound(stamps.begin(),
                                                                                            stamps.end(), at);
            transaction_stamp_t const newest = after == stamps.begin() ? unwritten_stamp_k : *(after - 1);
            delta_view_t first_found, second_found;
            bool const first_answered = first_view->find(stored_key, at, first_found);
            bool const second_answered = second_view->find(stored_key, at, second_found);
            if (!expect_true(first_answered == (newest != unwritten_stamp_k) && second_answered == first_answered))
                return;
            if (first_answered && second_answered) {
                if (!expect_true(first_found.kind == kind_of(newest) && second_found.kind == first_found.kind)) return;
                if (first_found.kind == delta_kind_t::write_k) {
                    transaction_stamp_t written = unwritten_stamp_k;
                    if (!expect_true(first_found.bytes.size() == sizeof(written))) return;
                    std::memcpy(&written, first_found.bytes.data(), sizeof(written));
                    if (!expect_true(written == newest)) return;
                }
            }
            ++reader_reads[reader];
        }
    };

    std::vector<std::thread> readers;
    for (std::size_t reader = 0; reader != readers_count_k; ++reader) readers.emplace_back(read, reader);
    std::thread publisher(publish);
    publisher.join();
    for (std::thread& reader : readers) reader.join();

    delete first_current;
    delete second_current;
    expect_true(atomic_reference<std::size_t> {alive}.load() == 0);
    std::size_t reads = 0;
    for (std::size_t reader = 0; reader != readers_count_k; ++reader) reads += reader_reads[reader];
    fmt::println("    {} reads by {} readers over {} commits", reads, readers_count_k, commits_count_k);
}

#pragma endregion Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore::test;
    start_test_run(argc, argv);
    run_test_case("test_refusals", test_refusals);
    run_test_case("test_removal_and_empty_value_differ", test_removal_and_empty_value_differ);
    run_test_case("test_range_tombstones_hide_older_runs", test_range_tombstones_hide_older_runs);
    run_test_case("test_sort_orders_by_key", test_sort_orders_by_key);
    run_test_case("test_merges_keep_values_intact", test_merges_keep_values_intact);
    run_test_case("test_random_streams_match_model", test_random_streams_match_model);
    run_test_case("test_readers_against_one_publisher", test_readers_against_one_publisher);
    return finish_test_run();
}
