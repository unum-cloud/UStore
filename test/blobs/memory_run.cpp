/**
 *  @file test/blobs/memory_run.cpp
 *  @author Ash Vardanian
 *  @date October 18, 2026
 *  @brief Unit tests of `ustore/blobs/memory_run.hpp` and `ustore/blobs/collection_view.hpp`: what sealing refuses,
 *      reads at every stamp and the conflict checks over random commit streams against a reference model, a map of
 *      every version of every key, and readers walking views while one thread publishes.
 *
 *  The suite prints one line per case and a total, and exits non-zero on a failure.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcmp`, `std::memcpy`

#include <algorithm>       // `std::upper_bound`
#include <atomic>          // `std::atomic`
#include <map>             // `std::map`
#include <random>          // `std::mt19937_64`, `std::uniform_int_distribution`
#include <source_location> // `std::source_location`
#include <span>            // `std::span`
#include <thread>          // `std::thread`
#include <utility>         // `std::exchange`
#include <vector>          // `std::vector`

#include <fmt/format.h> // `fmt::println`

#include <smashtable/shared.hpp> // `st::commit_order_t`

#include "ustore/blobs/collection_view.hpp" // `collection_view`, `published_view`, `epoch_domain`
#include "ustore/blobs/memory_run.hpp"      // `keyed_delta`, `memory_run`, `sealed_writes`

namespace unum::ustore::test {

using namespace unum::ustore::blobs;

using stored_key_t = std::uint64_t;
using keyed_delta_t = keyed_delta<stored_key_t>;
using sealed_t = sealed_writes<stored_key_t>;
using run_t = memory_run<stored_key_t>;
using view_t = collection_view<run_t>;
using bytes_t = std::vector<byte_t>;

#pragma region Harness and Model

/** Prints a failed check with its location and counts it. */
static std::size_t check(bool condition, std::source_location location = std::source_location::current()) {
    if (condition) return 0;
    fmt::println("    check failed at {}:{}", location.file_name(), location.line());
    return 1;
}

template <typename body_type_>
void run(char const* name, body_type_&& body, std::size_t& failed) {
    std::size_t const failures = body();
    fmt::println("{:<48} {}", name, failures ? "FAILED" : "passed");
    failed += failures != 0;
}

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
static std::size_t agrees(bool found, delta_view_t const& delta, model_version_t const* version) {
    if (!found || !version) return check(found == (version != nullptr));
    if (check(delta.kind == version->kind && delta.bytes.size() == version->bytes.size())) return 1;
    return check(delta.bytes.empty() ||
                 std::memcmp(delta.bytes.data(), version->bytes.data(), delta.bytes.size()) == 0);
}

#pragma endregion Harness and Model

#pragma region Cases

/** Sealing refuses what a run cannot hold, and a view refuses a run that is not newer than all it names. */
static std::size_t refusals() {
    std::size_t failures = 0;
    bytes_t const payload(4, byte_t {7});
    std::vector<keyed_delta_t> none;
    failures += check(sealed_t::seal(none).code() == status_code_t::invalid_argument_k);

    std::vector<keyed_delta_t> repeated {{3, {delta_kind_t::write_k, payload}}, {3, {delta_kind_t::remove_k, {}}}};
    failures += check(sealed_t::seal(repeated).code() == status_code_t::invalid_argument_k);

    std::vector<keyed_delta_t> loaded_removal {{3, {delta_kind_t::remove_k, payload}}};
    failures += check(sealed_t::seal(loaded_removal).code() == status_code_t::invalid_argument_k);

    std::vector<keyed_delta_t> one {{3, {delta_kind_t::write_k, payload}}};
    expected<sealed_t> sealed = sealed_t::seal(one);
    if (check(sealed.has_value())) return failures + 1;
    failures += check(run_t::stamped(std::move(*sealed), unwritten_stamp_k).code() ==
                      status_code_t::invalid_argument_k);

    std::vector<keyed_delta_t> newer {{3, {delta_kind_t::write_k, payload}}};
    std::vector<keyed_delta_t> older {{4, {delta_kind_t::write_k, payload}}};
    expected<run_t> newer_run = run_t::stamped(*sealed_t::seal(newer), 9);
    expected<run_t> older_run = run_t::stamped(*sealed_t::seal(older), 9);
    if (check(newer_run.has_value() && older_run.has_value())) return failures + 1;
    view_t const empty;
    expected<view_t> holding = view_t::extended(empty, *newer_run);
    if (check(holding.has_value())) return failures + 1;
    failures += check(view_t::extended(*holding, *older_run).code() == status_code_t::invalid_argument_k);
    return failures;
}

/** A removal and a write of no bytes read back apart, which a sentinel offset or size used to decide. */
static std::size_t removal_and_empty_value_differ() {
    std::size_t failures = 0;
    std::vector<keyed_delta_t> deltas {{20, {delta_kind_t::remove_k, {}}}, {10, {delta_kind_t::write_k, {}}}};
    expected<sealed_t> sealed = sealed_t::seal(deltas);
    if (check(sealed.has_value())) return 1;
    expected<run_t> stamped = run_t::stamped(std::move(*sealed), 5);
    if (check(stamped.has_value())) return 1;
    view_t const empty;
    expected<view_t> view = view_t::extended(empty, *stamped);
    if (check(view.has_value())) return 1;

    delta_view_t found;
    failures += check(view->find(10, 5, found) && found.kind == delta_kind_t::write_k && found.bytes.empty());
    failures += check(view->find(20, 5, found) && found.kind == delta_kind_t::remove_k && found.bytes.empty());
    failures += check(!view->find(10, 4, found) && !view->find(30, 5, found));
    failures += check(view->written_since(20, 4) && !view->written_since(20, 5) && !view->written_since(30, 0));
    failures += check(view->range_written_since(11, 20, 4) && !view->range_written_since(11, 19, 4));
    return failures;
}

/** Random commits, each one run on top of the last view, read at random stamps and checked for conflicts against
 *  the model; every older view keeps answering at its own stamp. */
static std::size_t random_streams_match_model() {
    constexpr std::size_t commits_count_k = 400;
    constexpr stored_key_t keys_count_k = 64;
    constexpr std::size_t probes_per_commit_k = 24;
    std::size_t failures = 0;
    for (std::uint64_t seed = 1; seed <= 3 && failures == 0; ++seed) {
        std::mt19937_64 generator(seed);
        std::uniform_int_distribution<std::size_t> records_count(1, 8), kind_pick(0, 9), length_pick(0, 16);
        // A few keys past the written ones are probed and never found.
        std::uniform_int_distribution<stored_key_t> key_pick(0, keys_count_k - 1), probe_pick(0, keys_count_k + 7);
        std::uniform_int_distribution<int> byte_pick(0, 255);

        model_t model;
        // Reserved whole, so the runs every view names never move.
        std::vector<run_t> runs;
        std::vector<view_t> views;
        runs.reserve(commits_count_k);
        views.reserve(commits_count_k + 1);
        views.emplace_back();
        for (transaction_stamp_t stamp = 1; stamp <= commits_count_k && failures == 0; ++stamp) {
            std::vector<keyed_delta_t> deltas;
            std::vector<bytes_t> payloads;
            std::size_t const wanted = records_count(generator);
            payloads.reserve(wanted);
            for (std::size_t index = 0; index != wanted; ++index) {
                stored_key_t const stored_key = key_pick(generator);
                bool repeated = false;
                for (keyed_delta_t const& delta : deltas) repeated |= delta.stored_key == stored_key;
                if (repeated) continue;
                std::size_t const kind_roll = kind_pick(generator);
                delta_kind_t const kind = kind_roll < 6   ? delta_kind_t::write_k
                                          : kind_roll < 9 ? delta_kind_t::remove_k
                                                          : delta_kind_t::change_k;
                bytes_t& payload = payloads.emplace_back();
                if (kind != delta_kind_t::remove_k) payload.resize(length_pick(generator));
                for (byte_t& value : payload) value = static_cast<byte_t>(byte_pick(generator));
                deltas.push_back(keyed_delta_t {stored_key, {kind, payload}});
                model[stored_key].push_back(model_version_t {stamp, kind, payload});
            }

            expected<sealed_t> sealed = sealed_t::seal(deltas);
            if (check(sealed.has_value())) return failures + 1;
            expected<run_t> stamped = run_t::stamped(std::move(*sealed), stamp);
            if (check(stamped.has_value())) return failures + 1;
            runs.push_back(std::move(*stamped));
            expected<view_t> extended = view_t::extended(views.back(), runs.back());
            if (check(extended.has_value())) return failures + 1;
            views.push_back(std::move(*extended));
            view_t const& view = views.back();
            failures += check(view.newest_stamp() == stamp && view.runs().size() == stamp);

            std::uniform_int_distribution<transaction_stamp_t> stamp_pick(0, stamp);
            for (std::size_t probe = 0; probe != probes_per_commit_k; ++probe) {
                stored_key_t const stored_key = probe_pick(generator);
                transaction_stamp_t const at = stamp_pick(generator);
                delta_view_t found;
                bool const answered = view.find(stored_key, at, found);
                failures += agrees(answered, found, model_find(model, stored_key, at));

                transaction_stamp_t const from = stamp_pick(generator);
                failures += check(view.written_since(stored_key, from) == (model_newest(model, stored_key) > from));

                stored_key_t const other = probe_pick(generator);
                stored_key_t const lower = std::min(stored_key, other);
                stored_key_t const upper = std::max(stored_key, other);
                bool range_written = false;
                for (stored_key_t key = lower; key <= upper; ++key) range_written |= model_newest(model, key) > from;
                failures += check(view.range_written_since(lower, upper, from) == range_written);

                // The view published at an older stamp still answers at that stamp, whatever came after it.
                transaction_stamp_t const published_stamp = stamp_pick(generator);
                bool const answered_then = views[published_stamp].find(stored_key, head_stamp_k, found);
                failures += agrees(answered_then, found, model_find(model, stored_key, published_stamp));
            }
        }
    }
    return failures;
}

/** A view a publisher allocated and frees once replaced, counting itself out when freed. */
struct held_view_t {
    view_t view;
    std::atomic<std::size_t>* alive = nullptr;
    ~held_view_t() noexcept { alive->fetch_sub(1, std::memory_order_relaxed); }
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
static std::size_t readers_against_one_publisher() {
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
    epoch_domain epochs;
    std::size_t failures = check(epochs.reserve_threads(1).succeeded());
    std::atomic<std::size_t> alive {0};
    auto held = [&]() noexcept {
        held_view_t* const made = new held_view_t {};
        made->alive = &alive;
        alive.fetch_add(1, std::memory_order_relaxed);
        return made;
    };

    // Runs stay alive until the threads join, as only a flush frees them; views are freed as they are replaced.
    // Both are reserved whole, so the runs every view names never move.
    std::vector<run_t> first_runs, second_runs;
    first_runs.reserve(commits_count_k);
    second_runs.reserve(commits_count_k);
    held_view_t* first_current = held();
    held_view_t* second_current = held();
    published_view<view_t> first, second;
    [[maybe_unused]] view_t const* const first_empty = first.replace(first_current->view);
    [[maybe_unused]] view_t const* const second_empty = second.replace(second_current->view);

    std::size_t publisher_failures = 0;
    auto publish = [&]() noexcept {
        for (transaction_stamp_t stamp = 1; stamp <= commits_count_k && publisher_failures == 0; ++stamp) {
            bytes_t payload(sizeof(stamp));
            std::memcpy(payload.data(), &stamp, sizeof(stamp));
            delta_kind_t const kind = kind_of(stamp);
            std::span<byte_t const> const bytes = kind == delta_kind_t::remove_k ? std::span<byte_t const> {}
                                                                                 : std::span<byte_t const>(payload);
            keyed_delta_t first_delta {key_of(stamp), {kind, bytes}};
            keyed_delta_t second_delta {key_of(stamp), {kind, bytes}};
            expected<sealed_t> first_sealed = sealed_t::seal(std::span<keyed_delta_t>(&first_delta, 1));
            expected<sealed_t> second_sealed = sealed_t::seal(std::span<keyed_delta_t>(&second_delta, 1));
            publisher_failures += check(first_sealed.has_value() && second_sealed.has_value());
            if (publisher_failures) return;

            st::commit_order_t::commit_in_flight_t in_flight;
            order.begin_commit(in_flight);
            transaction_stamp_t const drawn = static_cast<transaction_stamp_t>(in_flight.stamp());
            publisher_failures += check(drawn == stamp);
            expected<run_t> first_run = run_t::stamped(std::move(*first_sealed), drawn);
            expected<run_t> second_run = run_t::stamped(std::move(*second_sealed), drawn);
            publisher_failures += check(first_run.has_value() && second_run.has_value());
            if (publisher_failures) return;
            first_runs.push_back(std::move(*first_run));
            second_runs.push_back(std::move(*second_run));
            expected<view_t> first_view = view_t::extended(first_current->view, first_runs.back());
            expected<view_t> second_view = view_t::extended(second_current->view, second_runs.back());
            publisher_failures += check(first_view.has_value() && second_view.has_value());
            if (publisher_failures) return;
            held_view_t* const first_next = held();
            held_view_t* const second_next = held();
            first_next->view = std::move(*first_view);
            second_next->view = std::move(*second_view);
            [[maybe_unused]] view_t const* const first_replaced = first.replace(first_next->view);
            [[maybe_unused]] view_t const* const second_replaced = second.replace(second_next->view);
            order.end_commit(in_flight);
            epochs.wait_for_readers();
            delete std::exchange(first_current, first_next);
            delete std::exchange(second_current, second_next);
            // Nothing replaced outlives the commit that replaced it.
            publisher_failures += check(alive.load(std::memory_order_relaxed) == 2);
        }
    };

    std::vector<std::size_t> reader_failures(readers_count_k, 0);
    std::vector<std::size_t> reader_reads(readers_count_k, 0);
    auto read = [&](std::size_t reader) noexcept {
        std::mt19937_64 generator(reader + 1);
        std::uniform_int_distribution<stored_key_t> key_pick(0, keys_count_k - 1);
        std::size_t& failures = reader_failures[reader];
        for (transaction_stamp_t at = 0; at != commits_count_k && failures == 0;) {
            epoch_domain::reader_t const inside = reader == 0 ? epochs.enter(0) : epochs.enter();
            epoch_domain::reader_t const nested = epochs.enter();
            at = static_cast<transaction_stamp_t>(order.published_stamp());
            view_t const* const first_view = first.load();
            view_t const* const second_view = second.load();
            failures += check(first_view->newest_stamp() >= at && second_view->newest_stamp() >= at);

            stored_key_t const stored_key = key_pick(generator);
            std::vector<transaction_stamp_t> const& stamps = writers[stored_key];
            std::vector<transaction_stamp_t>::const_iterator const after = std::upper_bound(stamps.begin(),
                                                                                            stamps.end(), at);
            transaction_stamp_t const newest = after == stamps.begin() ? unwritten_stamp_k : *(after - 1);
            delta_view_t first_found, second_found;
            bool const first_answered = first_view->find(stored_key, at, first_found);
            bool const second_answered = second_view->find(stored_key, at, second_found);
            failures += check(first_answered == (newest != unwritten_stamp_k) && second_answered == first_answered);
            if (first_answered && second_answered) {
                failures += check(first_found.kind == kind_of(newest) && second_found.kind == first_found.kind);
                if (first_found.kind == delta_kind_t::write_k) {
                    transaction_stamp_t written = unwritten_stamp_k;
                    failures += check(first_found.bytes.size() == sizeof(written));
                    if (failures == 0) std::memcpy(&written, first_found.bytes.data(), sizeof(written));
                    failures += check(written == newest);
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

    failures += publisher_failures;
    delete first_current;
    delete second_current;
    failures += check(alive.load() == 0);
    std::size_t reads = 0;
    for (std::size_t reader = 0; reader != readers_count_k; ++reader) {
        failures += reader_failures[reader];
        reads += reader_reads[reader];
    }
    fmt::println("    {} reads by {} readers over {} commits", reads, readers_count_k, commits_count_k);
    return failures;
}

#pragma endregion Cases

} // namespace unum::ustore::test

int main() {
    using namespace unum::ustore::test;
    std::size_t failed = 0, total = 0;
    auto counted = [&](char const* name, auto body) { run(name, body, failed), ++total; };
    counted("memory_run.refusals", refusals);
    counted("memory_run.removal_and_empty_value_differ", removal_and_empty_value_differ);
    counted("collection_view.random_streams_match_model", random_streams_match_model);
    counted("collection_view.readers_against_one_publisher", readers_against_one_publisher);
    fmt::println("total {} passed, {} failed", total - failed, failed);
    return failed ? 1 : 0;
}
