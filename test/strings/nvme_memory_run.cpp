/**
 *  @file test/strings/nvme_memory_run.cpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief Unit tests of `ustore/strings/nvme_memory_run.hpp`: head order, what sealing refuses, reads at every stamp,
 *      conflict checks and prefix cursors over random commit streams against a reference model, a map of every
 *      version of every string, and merges by tier that keep every version a reader still sees.
 *
 *  The strings share long prefixes on purpose: ten thousand share their leading 15 bytes, so their heads tie, and the
 *  rest cover empty strings, strings of 1, 15, 16 and 17 bytes, zero and 0xFF bytes inside them, and a 64 KiB string.
 *  Every write maps its string onto one random 16-byte key. The suite prints one line per case and a total, and exits
 *  non-zero on a failure.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcmp`

#include <algorithm>       // `std::max`, `std::min`, `std::sort`
#include <map>             // `std::map`
#include <memory>          // `std::unique_ptr`
#include <random>          // `std::mt19937_64`, `std::uniform_int_distribution`
#include <set>             // `std::set`
#include <source_location> // `std::source_location`
#include <span>            // `std::span`
#include <string>          // `std::string`
#include <string_view>     // `std::string_view`
#include <vector>          // `std::vector`

#include <fmt/format.h> // `fmt::println`

#include "ustore/blobs/nvme_collection_view.hpp" // `collection_view`
#include "ustore/strings/nvme_memory_run.hpp"    // `memory_run`, `memory_run_cursor`, `kept_versions`

namespace unum::ustore::test {

using namespace unum::ustore::strings;

using blobs::head_stamp_k;

using sealed_t = strings::sealed_deltas<>;
using run_t = strings::memory_run<>;
using view_t = blobs::collection_view<run_t>;
using cursor_t = memory_run_cursor<view_t>;
using keyed_delta_t = keyed_delta<std::string_view>;
using stamped_delta_t = stamped_delta<std::string_view>;
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

/** One version of a string as the model keeps it. */
struct model_version_t {
    transaction_stamp_t stamp = unwritten_stamp_k;
    delta_kind_t kind = delta_kind_t::write_k;
    bytes_t bytes;
};

/** Every version of every string, oldest first, ordered as unsigned bytes as the runs order them. */
using model_t = std::map<std::string, std::vector<model_version_t>>;

/** The newest version of @p text at or below @p at, or null. */
static model_version_t const* model_find(model_t const& model, std::string const& text, transaction_stamp_t at) {
    model_t::const_iterator const found = model.find(text);
    if (found == model.end()) return nullptr;
    for (std::size_t index = found->second.size(); index-- != 0;)
        if (found->second[index].stamp <= at) return &found->second[index];
    return nullptr;
}

/** The stamp of the newest version of @p text, or @c unwritten_stamp_k for a string never written. */
static transaction_stamp_t model_newest(model_t const& model, std::string const& text) {
    model_t::const_iterator const found = model.find(text);
    return found == model.end() ? unwritten_stamp_k : found->second.back().stamp;
}

/** Whether a version read back agrees with the model's, or with its absence. */
static std::size_t agrees(bool found, delta_view_t const& delta, model_version_t const* version) {
    if (!found || !version) return check(found == (version != nullptr));
    if (check(delta.kind == version->kind && delta.bytes.size() == version->bytes.size())) return 1;
    return check(delta.bytes.empty() ||
                 std::memcmp(delta.bytes.data(), version->bytes.data(), delta.bytes.size()) == 0);
}

/** Whether the strings of @p view listed under @p prefix from @p start at @p at, up to @p wanted of them, match the
 *  model's, removals included. */
static std::size_t cursor_agrees(view_t const& view, model_t const& model, std::string const& prefix,
                                 std::string const& start, transaction_stamp_t at, std::size_t wanted) {
    expected<cursor_t> cursor = cursor_t::over(view, prefix, start, at);
    if (check(cursor.has_value())) return 1;
    std::size_t failures = 0;
    std::size_t listed = 0;
    for (model_t::const_iterator entry = model.lower_bound(std::max(prefix, start));
         entry != model.end() && entry->first.starts_with(prefix) && listed != wanted && failures == 0; ++entry) {
        model_version_t const* const version = model_find(model, entry->first, at);
        if (!version) continue;
        string_version_t found;
        if (check(cursor->next(found))) return failures + 1;
        failures += check(found.string == entry->first && found.stamp == version->stamp);
        failures += agrees(true, found.delta, version);
        ++listed;
    }
    string_version_t past;
    if (listed != wanted && failures == 0) failures += check(!cursor->next(past));
    return failures;
}

/** The strings every case draws from: ten thousand sharing a 15-byte prefix that holds zero and 0xFF bytes, the
 *  lengths around a head's edge, strings over a four-letter alphabet of zero, one, 'a' and 0xFF, and two 64 KiB
 *  strings differing in their last byte. */
static std::vector<std::string> string_pool(std::mt19937_64& generator) {
    std::uniform_int_distribution<int> byte_pick(0, 255), alphabet_pick(0, 3), shared_length(0, 6), mixed_length(0, 40);
    std::set<std::string> pool;
    std::string const shared {'p', '\0', 'r', '\xFF', 'e', 'f', 'i', '\0', 'x', '-', 's', 'h', 'a', 'r', 'e'};
    pool.insert(shared);
    while (pool.size() < 10'000) {
        std::string text = shared;
        for (int remaining = shared_length(generator) + 1; remaining != 0; --remaining)
            text.push_back(static_cast<char>(byte_pick(generator)));
        pool.insert(text);
    }
    std::string const letters = "abcdefghijklmnopq";
    for (std::size_t length : {0, 1, 14, 15, 16, 17}) {
        pool.insert(letters.substr(0, length));
        pool.insert(std::string(length, '\0'));
        pool.insert(std::string(length, '\xFF'));
    }
    pool.insert(letters.substr(0, 15) + std::string(1, '\0'));
    pool.insert(letters.substr(0, 15) + std::string(2, '\0'));
    pool.insert(letters.substr(0, 14) + std::string(1, '\0') + "q");
    char const alphabet[4] {'\0', '\x01', 'a', '\xFF'};
    for (std::size_t index = 0; index != 1000; ++index) {
        std::string text;
        for (int remaining = mixed_length(generator); remaining != 0; --remaining)
            text.push_back(alphabet[alphabet_pick(generator)]);
        pool.insert(text);
    }
    std::string large = shared;
    while (large.size() != 64 * 1024) large.push_back(static_cast<char>(byte_pick(generator)));
    pool.insert(large);
    large.back() = static_cast<char>(large.back() + 1);
    pool.insert(large);
    return {pool.begin(), pool.end()};
}

/** A prefix to scan under: empty, a cut of a pool string at a length around the head's edge, or a whole string. */
static std::string prefix_pick(std::vector<std::string> const& pool, std::mt19937_64& generator) {
    std::uniform_int_distribution<std::size_t> string_pick(0, pool.size() - 1), length_pick(0, 18), shape_pick(0, 5);
    std::string const& text = pool[string_pick(generator)];
    switch (shape_pick(generator)) {
    case 0: return {};
    case 1: return text;
    default: return text.substr(0, std::min(text.size(), length_pick(generator)));
    }
}

/** A start string to scan from: empty, a pool string, one cut short, or one past every string under it. */
static std::string start_pick(std::vector<std::string> const& pool, std::mt19937_64& generator) {
    std::uniform_int_distribution<std::size_t> string_pick(0, pool.size() - 1), length_pick(0, 20), shape_pick(0, 3);
    std::string const& text = pool[string_pick(generator)];
    switch (shape_pick(generator)) {
    case 0: return {};
    case 1: return text;
    case 2: return text.substr(0, std::min(text.size(), length_pick(generator)));
    default: return text + std::string(1, '\xFF');
    }
}

/** Draws one commit's deltas over @p pool into @p deltas, appending them to @p model under @p stamp; the 16-byte keys
 *  written land in @p payloads, which must outlive the sealing. */
static void draw_commit(std::vector<std::string> const& pool, std::mt19937_64& generator, transaction_stamp_t stamp,
                        model_t& model, std::vector<keyed_delta_t>& deltas, std::vector<bytes_t>& payloads) {
    std::uniform_int_distribution<std::size_t> deltas_count(1, 32), string_pick(0, pool.size() - 1),
        shared_pick(0, 9'999), kind_pick(0, 9);
    std::uniform_int_distribution<int> byte_pick(0, 255);
    std::size_t const wanted = deltas_count(generator);
    deltas.clear();
    payloads.clear();
    payloads.reserve(wanted);
    std::set<std::string_view> chosen;
    for (std::size_t index = 0; index != wanted; ++index) {
        // Half the picks land among the strings sharing 15 bytes, whose heads tie, so tails decide often.
        std::string const& text = pool[index % 2 ? string_pick(generator) : shared_pick(generator)];
        if (!chosen.insert(text).second) continue;
        delta_kind_t const kind = kind_pick(generator) < 7 ? delta_kind_t::write_k : delta_kind_t::remove_k;
        bytes_t& payload = payloads.emplace_back();
        if (kind == delta_kind_t::write_k) payload.resize(sizeof(ustore_key_t));
        for (byte_t& value : payload) value = static_cast<byte_t>(byte_pick(generator));
        deltas.push_back(keyed_delta_t {text, {kind, payload}});
        model[text].push_back(model_version_t {stamp, kind, payload});
    }
}

/** Seals and stamps @p deltas into a run that @p runs owns, answering it, or null on a failure it counts. */
static run_t const* commit(std::vector<keyed_delta_t>& deltas, transaction_stamp_t stamp,
                           std::vector<std::unique_ptr<run_t>>& runs, std::size_t& failures) {
    expected<sealed_t> sealed = sealed_t::seal(deltas, &sz_sort_order_serial);
    if ((failures += check(sealed.has_value()))) return nullptr;
    expected<run_t> stamped = run_t::stamped(std::move(*sealed), stamp);
    if ((failures += check(stamped.has_value()))) return nullptr;
    runs.push_back(std::make_unique<run_t>(std::move(*stamped)));
    return runs.back().get();
}

/** Whether the strings of @p run ascend, the versions of one string newest first, with heads and tails that agree. */
static std::size_t run_is_ordered(run_t const& run) {
    std::size_t failures = 0;
    for (std::size_t position = 0; position != run.size() && failures == 0; ++position) {
        failures += check(run.head(position) == head_of(run.stored_key(position)));
        if (position == 0) continue;
        std::string_view const previous = run.stored_key(position - 1);
        std::string_view const current = run.stored_key(position);
        failures += check(previous < current || (previous == current && run.stamp(position - 1) > run.stamp(position)));
    }
    return failures;
}

#pragma endregion Harness and Model

#pragma region Cases

/** Heads order as their strings wherever they differ, and tie only for strings sharing 15 bytes past that length. */
static std::size_t heads_order_as_strings() {
    std::mt19937_64 generator(7);
    std::vector<std::string> pool = string_pool(generator);
    std::uniform_int_distribution<std::size_t> string_pick(0, pool.size() - 1);
    std::size_t failures = 0;
    for (std::size_t pair = 0; pair != 200'000 && failures == 0; ++pair) {
        std::string const& first = pool[string_pick(generator)];
        std::string const& second = pool[string_pick(generator)];
        st::key128_t const first_head = head_of(first);
        st::key128_t const second_head = head_of(second);
        if (first_head < second_head) failures += check(first < second);
        if (first < second) failures += check(!(second_head < first_head));
        if (first_head == second_head)
            failures += check(first.size() > head_prefix_bytes_k ? second.size() > head_prefix_bytes_k
                                                                 : first == second);
        std::strong_ordering const ordered = order_strings(first_head, first, second_head, second);
        failures += check(ordered == (first <=> second));
    }
    failures += check(head_of({}) == st::key128_t {});
    failures += check(head_length_of(head_of(std::string(1, '\0'))) == 1);
    failures += check(head_length_of(head_of(std::string(17, 'a'))) == long_head_length_k);
    failures += check(tail_of(std::string(15, 'a')).empty() && tail_of(std::string(17, 'a')).size() == 2);
    return failures;
}

/** Sealing refuses what a run cannot hold, a write of anything but one 16-byte key included, and a run keeps a removal
 *  apart from a write. */
static std::size_t refusals() {
    std::size_t failures = 0;
    bytes_t const payload(sizeof(ustore_key_t), byte_t {7});
    std::vector<keyed_delta_t> none;
    failures += check(sealed_t::seal(none, &sz_sort_order_serial).code() == status_code_t::invalid_argument_k);

    std::string const repeated_text(20, 'x');
    std::vector<keyed_delta_t> repeated {{repeated_text, {delta_kind_t::write_k, payload}},
                                         {repeated_text, {delta_kind_t::remove_k, {}}}};
    failures += check(sealed_t::seal(repeated, &sz_sort_order_serial).code() == status_code_t::invalid_argument_k);

    // A batch naming a string twice keeps its last delta, in byte order, the empty string first.
    std::vector<keyed_delta_t> batch {{"b", {delta_kind_t::write_k, payload}},
                                      {"", {delta_kind_t::remove_k, {}}},
                                      {"ab", {delta_kind_t::write_k, payload}},
                                      {"b", {delta_kind_t::remove_k, {}}}};
    expected<std::size_t> const kept = strings::sort_unique_strings(std::span<keyed_delta_t> {batch},
                                                                    &sz_sort_order_serial, strings::keep_t::last_k,
                                                                    st::default_allocator<byte_t> {});
    failures += check(kept.has_value() && *kept == 3);
    failures += check(batch[0].stored_key.empty() && batch[1].stored_key == "ab" && batch[2].stored_key == "b");
    failures += check(batch[2].delta.kind == delta_kind_t::remove_k);

    std::vector<keyed_delta_t> loaded_removal {{"key", {delta_kind_t::remove_k, payload}}};
    failures += check(sealed_t::seal(loaded_removal, &sz_sort_order_serial).code() ==
                      status_code_t::invalid_argument_k);

    bytes_t const short_payload(4, byte_t {7});
    std::vector<keyed_delta_t> short_write {{"key", {delta_kind_t::write_k, short_payload}}};
    failures += check(sealed_t::seal(short_write, &sz_sort_order_serial).code() == status_code_t::invalid_argument_k);
    std::vector<keyed_delta_t> empty_write {{"key", {delta_kind_t::write_k, {}}}};
    failures += check(sealed_t::seal(empty_write, &sz_sort_order_serial).code() == status_code_t::invalid_argument_k);

    std::vector<keyed_delta_t> one {{"key", {delta_kind_t::write_k, payload}}};
    expected<sealed_t> sealed = sealed_t::seal(one, &sz_sort_order_serial);
    if (check(sealed.has_value())) return failures + 1;
    failures += check(run_t::stamped(std::move(*sealed), unwritten_stamp_k).code() ==
                      status_code_t::invalid_argument_k);

    std::string const empty_string;
    bytes_t const other_payload(sizeof(ustore_key_t), byte_t {9});
    std::vector<keyed_delta_t> deltas {{"gone", {delta_kind_t::remove_k, {}}},
                                       {"blank", {delta_kind_t::write_k, other_payload}},
                                       {empty_string, {delta_kind_t::write_k, payload}}};
    expected<run_t> stamped = run_t::stamped(*sealed_t::seal(deltas, &sz_sort_order_serial), 5);
    if (check(stamped.has_value())) return failures + 1;
    view_t const empty;
    expected<view_t> view = view_t::extended(empty, *stamped);
    if (check(view.has_value())) return failures + 1;
    delta_view_t found;
    failures += check(view->find("blank", 5, found) && found.kind == delta_kind_t::write_k &&
                      found.bytes.size() == other_payload.size() &&
                      std::memcmp(found.bytes.data(), other_payload.data(), other_payload.size()) == 0);
    failures += check(view->find("gone", 5, found) && found.kind == delta_kind_t::remove_k && found.bytes.empty());
    failures += check(view->find(empty_string, 5, found) && found.bytes.size() == payload.size());
    failures += check(stamped->values_bytes() == deltas.size() * sizeof(ustore_key_t));
    failures += check(!view->find("blank", 4, found) && !view->find(std::string_view("blank\0", 6), 5, found));
    failures += check(view->written_since("gone", 4) && !view->written_since("gone", 5));
    failures += check(view->range_written_since("a", "blank", 4) && !view->range_written_since("c", "f", 4));
    failures += check(view->range_written_since(empty_string, empty_string, 4) &&
                      !view->range_written_since("b", "a", 4));

    std::vector<keyed_delta_t> older {{"older", {delta_kind_t::write_k, payload}}};
    expected<run_t> older_run = run_t::stamped(*sealed_t::seal(older, &sz_sort_order_serial), 5);
    if (check(older_run.has_value())) return failures + 1;
    failures += check(view_t::extended(*view, *older_run).code() == status_code_t::invalid_argument_k);
    return failures;
}

/** Random commits, each one run on top of the last view, read at random stamps, checked for conflicts and listed by
 *  prefix cursors against the model; every older view keeps answering at its own stamp. */
static std::size_t random_streams_match_model() {
    constexpr transaction_stamp_t commits_count_k = 300;
    constexpr std::size_t probes_per_commit_k = 24;
    std::size_t failures = 0;
    for (std::uint64_t seed = 1; seed <= 2 && failures == 0; ++seed) {
        std::mt19937_64 generator(seed);
        std::vector<std::string> const pool = string_pool(generator);
        std::uniform_int_distribution<std::size_t> string_pick(0, pool.size() - 1);

        model_t model;
        std::vector<std::unique_ptr<run_t>> runs;
        std::vector<view_t> views;
        views.reserve(commits_count_k + 1);
        views.emplace_back();
        std::vector<keyed_delta_t> deltas;
        std::vector<bytes_t> payloads;
        for (transaction_stamp_t stamp = 1; stamp <= commits_count_k && failures == 0; ++stamp) {
            draw_commit(pool, generator, stamp, model, deltas, payloads);
            run_t const* const committed = commit(deltas, stamp, runs, failures);
            if (!committed) return failures;
            failures += run_is_ordered(*committed);
            expected<view_t> extended = view_t::extended(views.back(), *committed);
            if (check(extended.has_value())) return failures + 1;
            views.push_back(std::move(*extended));
            view_t const& view = views.back();

            std::uniform_int_distribution<transaction_stamp_t> stamp_pick(0, stamp);
            for (std::size_t probe = 0; probe != probes_per_commit_k; ++probe) {
                std::string const& text = pool[string_pick(generator)];
                transaction_stamp_t const at = stamp_pick(generator);
                delta_view_t found;
                failures += agrees(view.find(text, at, found), found, model_find(model, text, at));

                transaction_stamp_t const from = stamp_pick(generator);
                failures += check(view.written_since(text, from) == (model_newest(model, text) > from));

                std::string const& other = pool[string_pick(generator)];
                std::string const& lower = std::min(text, other);
                std::string const& upper = std::max(text, other);
                bool range_written = false;
                for (model_t::const_iterator entry = model.lower_bound(lower);
                     entry != model.end() && entry->first <= upper && !range_written; ++entry)
                    range_written = entry->second.back().stamp > from;
                failures += check(view.range_written_since(lower, upper, from) == range_written);

                transaction_stamp_t const published_stamp = stamp_pick(generator);
                bool const answered_then = views[published_stamp].find(text, head_stamp_k, found);
                failures += agrees(answered_then, found, model_find(model, text, published_stamp));
            }
            if (stamp % 10 == 0)
                for (std::size_t probe = 0; probe != 8; ++probe)
                    failures += cursor_agrees(view, model, prefix_pick(pool, generator), start_pick(pool, generator),
                                              stamp_pick(generator), 64);
        }
        failures += cursor_agrees(views.back(), model, {}, {}, head_stamp_k, pool.size());
    }
    return failures;
}

/**
 *  @brief Merges the oldest four runs of the lowest tier holding four after every commit, as the owner's pass
 *      does, keeping versions for readers pinned at every 23rd stamp and above the stamp published at the merge.
 *
 *  A reader at a pinned stamp, or at or above the last merge's horizon, must read what the model holds at its stamp,
 *  through finds and cursors alike, while the conflict checks hold at every stamp, since a key's newest version
 *  always survives.
 */
static std::size_t merges_keep_visible_versions() {
    constexpr transaction_stamp_t commits_count_k = 400;
    constexpr std::size_t fan_in_k = 4;
    constexpr transaction_stamp_t pin_stride_k = 23;
    std::size_t failures = 0;
    std::mt19937_64 generator(11);
    std::vector<std::string> const pool = string_pool(generator);
    std::uniform_int_distribution<std::size_t> string_pick(0, pool.size() - 1);

    model_t model;
    std::vector<std::unique_ptr<run_t>> runs;
    std::unique_ptr<view_t> view = std::make_unique<view_t>();
    std::vector<transaction_stamp_t> pinned;
    transaction_stamp_t horizon = unwritten_stamp_k;
    std::size_t merges = 0;
    std::vector<keyed_delta_t> deltas;
    std::vector<bytes_t> payloads;
    for (transaction_stamp_t stamp = 1; stamp <= commits_count_k && failures == 0; ++stamp) {
        draw_commit(pool, generator, stamp, model, deltas, payloads);
        run_t const* const committed = commit(deltas, stamp, runs, failures);
        if (!committed) return failures;
        expected<view_t> extended = view_t::extended(*view, *committed);
        if (check(extended.has_value())) return failures + 1;
        view = std::make_unique<view_t>(std::move(*extended));
        if (stamp % pin_stride_k == 0) pinned.push_back(stamp);

        std::span<run_t const* const> const listed = view->runs();
        std::size_t first = 0;
        std::size_t end = 0;
        for (; first != listed.size(); first = end) {
            end = first;
            while (end != listed.size() && listed[end]->tier() == listed[first]->tier()) ++end;
            if (end - first >= fan_in_k) break;
        }
        if (first != listed.size()) {
            std::span<run_t const* const> const group = listed.subspan(end - fan_in_k, fan_in_k);
            compaction_stamps_t survival;
            survival.pinned = pinned.data();
            survival.pinned_count = pinned.size();
            survival.horizon = stamp;
            st::basic_vector<stamped_delta_t> versions;
            utilities::local_arena_t folded;
            utilities::arena_allocator<byte_t> folded_bytes = folded.allocator();
            failures += check(
                kept_versions(group, survival, utilities::last_write_fold_t {}, versions, folded_bytes).succeeded());
            expected<run_t> merged = run_t::merged(std::span<stamped_delta_t const> {versions},
                                                   group.back()->oldest_stamp(), group.back()->tier());
            if (check(merged.has_value())) return failures + 1;
            std::size_t replaced = 0;
            for (run_t const* run : group) replaced += run->size();
            failures += check(merged->tier() == group.back()->tier() + 1 &&
                              merged->oldest_stamp() == group.back()->oldest_stamp() &&
                              merged->newest_stamp() == group.front()->newest_stamp() && merged->size() <= replaced);
            failures += run_is_ordered(*merged);
            runs.push_back(std::make_unique<run_t>(std::move(*merged)));

            typename view_t::runs_t laid_out;
            if (check(laid_out.reserve(listed.size() - fan_in_k + 1) == st::status_t::success_k)) return failures + 1;
            for (std::size_t index = 0; index != listed.size(); ++index) {
                if (index > end - fan_in_k && index < end) continue;
                run_t const* laid = index == end - fan_in_k ? runs.back().get() : listed[index];
                [[maybe_unused]] st::status_t const placed = laid_out.push_back(st::assume_reserved, std::move(laid));
            }
            view = std::make_unique<view_t>(view_t::of(std::move(laid_out)));
            horizon = stamp;
            ++merges;
        }

        // Readers either pinned or at or above the horizon read on, and every other stamp is free to drift.
        std::vector<transaction_stamp_t> readable = pinned;
        std::uniform_int_distribution<transaction_stamp_t> recent_pick(horizon, stamp);
        for (std::size_t recent = 0; recent != 4; ++recent) readable.push_back(recent_pick(generator));
        std::uniform_int_distribution<std::size_t> readable_pick(0, readable.size() - 1);
        std::uniform_int_distribution<transaction_stamp_t> stamp_pick(0, stamp);
        for (std::size_t probe = 0; probe != 64; ++probe) {
            std::string const& text = pool[string_pick(generator)];
            transaction_stamp_t const at = readable[readable_pick(generator)];
            delta_view_t found;
            failures += agrees(view->find(text, at, found), found, model_find(model, text, at));
            transaction_stamp_t const from = stamp_pick(generator);
            failures += check(view->written_since(text, from) == (model_newest(model, text) > from));
        }
        if (stamp % 25 == 0)
            for (std::size_t probe = 0; probe != 8; ++probe)
                failures += cursor_agrees(*view, model, prefix_pick(pool, generator), start_pick(pool, generator),
                                          readable[readable_pick(generator)], 64);
    }
    for (transaction_stamp_t const at : pinned) failures += cursor_agrees(*view, model, {}, {}, at, pool.size());
    fmt::println("    {} merges over {} commits, {} runs left", merges, commits_count_k, view->runs().size());
    return failures;
}

#pragma endregion Cases

} // namespace unum::ustore::test

int main() {
    using namespace unum::ustore::test;
    std::size_t failed = 0, total = 0;
    auto counted = [&](char const* name, auto body) { run(name, body, failed), ++total; };
    counted("strings_memory_run.heads_order_as_strings", heads_order_as_strings);
    counted("strings_memory_run.refusals", refusals);
    counted("strings_memory_run.random_streams_match_model", random_streams_match_model);
    counted("strings_memory_run.merges_keep_visible_versions", merges_keep_visible_versions);
    fmt::println("total {} passed, {} failed", total - failed, failed);
    return failed ? 1 : 0;
}
