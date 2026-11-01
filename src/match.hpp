/**
 *  @file src/match.hpp
 *  @author Ash Vardanian
 *  @date October 26, 2026
 *  @brief The match a scan or a cursor compiles once, from its patterns or filter and its predicate, and the filtered
 *      listings that compose an engine's scan and read verbs with it, page by page.
 *
 *  A page is what one scan of one task lists: up to @c wanted keys or strings from where the last page ended. A cheap
 *  predicate narrows it first, so fewer values are read; patterns or a filter then read the values of the keys left;
 *  an expensive predicate is asked last, once per page, about whatever survived. The engine's scan and read write the
 *  page into the call's arena, which every page rewinds, while the survivors grow in the runner's scratch and reach
 *  the arena once, when the listing ends, so a long filtered scan holds one page at a time.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`
#include <cstring> // `std::memcpy`

#include <concepts>    // `std::convertible_to`, `std::same_as`
#include <span>        // `std::span`
#include <type_traits> // `std::is_same_v`
#include <variant>     // `std::variant`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <stringzilla/regex.h>         // `sz_regex_engine_t`, `sz_regex_engine_init`, `sz_regex_contains`
#include <stringzilla/subdocuments.h>  // `sz_subdocuments_engine_t`, `sz_subdocuments_contains`
#include <stringzilla/substrings.h>    // `sz_substrings_engine_t`, `sz_substrings_contains`

#include "ustore/types.hpp"                 // `key_after`, `largest_key_k`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_allocator`, `arena_rewind`
#include "ustore/utilities/types.hpp"       // `stringzilla_refusal`

#include "engine.hpp"             // `home_t`, `home_tables`, `view_t`
#include "utilities/batches.hpp"  // `strings_arg_t`, `validate_scan`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore {
using namespace unum::ustore::utilities;

template <typename>
struct verb_slot;

/** Where a walk or a listing stands once a page came back short of what it wanted, so nothing follows. */
struct exhausted_t {};

/** The indices of a page's entries still in the running, compacted in place by every narrowing step, with the score
 *  of each when the match scores. */
struct candidates_t {
    std::span<std::uint32_t> indices;
    std::span<float> scores;
    std::size_t count = 0;
};

/** The matcher of a match reading no values. */
struct match_everything_t {};

/** The compiled engine a match reads values with, or none, which the scans of each modality fill differently. */
using value_matcher_t =
    std::variant<match_everything_t, sz_regex_engine_t*, sz_substrings_engine_t*, sz_subdocuments_engine_t*>;

/** What one call or cursor matches at one home. */
template <home_t>
struct compiled_match;

/** A host match: the compiled patterns or filter, and the caller's predicate. */
template <>
struct compiled_match<home_t::host_k> {

    /** The compiled regex, substrings or subdocuments engine, if the match reads values. */
    value_matcher_t matcher;

    /** The caller's verdict on listed keys, whose null callback admits every key. */
    ustore_predicate_t predicate {};

    /** The substrings engine scoring what @c matcher selected, or null for a match without weights. */
    sz_substrings_engine_t* scorer = nullptr;

    /** One weight per substring pattern, borrowed from the call, read when @c scorer scores. */
    float const* weights = nullptr;

    /** Whether the match reads the values of the keys it narrows. */
    bool reads_values() const noexcept { return !std::holds_alternative<match_everything_t>(matcher); }

    /** Whether the match narrows nothing, so the plain scan serves. */
    bool matches_everything() const noexcept { return !reads_values() && !predicate.callback; }

    /** Whether the match scores the values it keeps. */
    bool scores() const noexcept { return scorer != nullptr; }

    /** Whether the match asks the predicate about the keys it lists. */
    bool asks() const noexcept { return predicate.callback != nullptr; }

    /** Rewinds @p page, which a listing lays every page out in. */
    void rewind_page(ustore_arena_t page) const noexcept { arena_rewind(page); }

    /** Keeps among @p candidates of the page listing @p keys those the predicate admits, when its cost places it at
     *  @p phase, gathering the asked keys and their admissions into @p work. */
    status_t ask(ustore_predicate_cost_t phase, std::span<ustore_key_t const> keys, candidates_t& candidates,
                 ustore_arena_t work) const noexcept {
        if (!predicate.callback || predicate.cost != phase || !candidates.count) return {};
        auto [asked, asked_status] = arena_allocate<ustore_key_t>(work, candidates.count);
        if (asked_status.failed()) return asked_status;
        auto [admissions, admissions_status] = arena_allocate<ustore_admission_t>(work, candidates.count);
        if (admissions_status.failed()) return admissions_status;
        for (std::size_t index = 0; index != candidates.count; ++index) {
            asked[index] = keys[candidates.indices[index]];
            admissions[index] = ustore_reject_k;
        }
        predicate.callback(asked.data(), admissions.data(), candidates.count, predicate.state);
        std::size_t kept = 0;
        for (std::size_t index = 0; index != candidates.count; ++index) {
            if (admissions[index] != ustore_admit_k) continue;
            if (!candidates.scores.empty()) candidates.scores[kept] = candidates.scores[index];
            candidates.indices[kept++] = candidates.indices[index];
        }
        candidates.count = kept;
        return {};
    }

    /** Keeps among @p candidates those whose @p values, one per candidate in their order, the engine contains a match
     *  in, laying out their views and the bitmap of matches in @p work. */
    status_t select(strings_arg_t const& values, candidates_t& candidates, ustore_arena_t work) const noexcept {
        if (!reads_values() || !candidates.count) return {};
        auto [views, views_status] = arena_allocate<sz_string_view_t>(work, candidates.count);
        if (views_status.failed()) return views_status;
        for (std::size_t index = 0; index != candidates.count; ++index) {
            ustore_bytes_view_t const value = values[index];
            views[index].start = reinterpret_cast<sz_cptr_t>(value.data);
            views[index].length = static_cast<sz_size_t>(value.length);
        }
        auto [contained, contained_status] = arena_allocate<sz_u1x8_t>(work, divide_round_up(candidates.count, 8));
        if (contained_status.failed()) return contained_status;
        sz_strings_view_t strings {};
        sz_strings_from_string_views(views.data(), candidates.count, &strings);
        sz_size_t malformed_index = 0;
        sz_status_t status = sz_success_k;
        if (sz_regex_engine_t* const* const regex = std::get_if<sz_regex_engine_t*>(&matcher))
            status = sz_regex_contains(*regex, &strings, contained.data(), nullptr);
        else if (sz_substrings_engine_t* const* const substrings = std::get_if<sz_substrings_engine_t*>(&matcher))
            status = sz_substrings_contains(*substrings, &strings, contained.data(), nullptr);
        else if (sz_subdocuments_engine_t* const* const paths = std::get_if<sz_subdocuments_engine_t*>(&matcher))
            status = sz_subdocuments_contains(*paths, &strings, contained.data(), &malformed_index, nullptr);
        if (status != sz_success_k)
            return stringzilla_refusal(status, status_code_t::invalid_argument_k, "Failed to match the values");
        return narrow(views, contained.data(), candidates);
    }

    /** Keeps among @p candidates those @p contained marks, compacting the @p views of their values beside them, and
     *  scores the kept when the match scores. */
    status_t narrow(std::span<sz_string_view_t> views, sz_u1x8_t const* contained,
                    candidates_t& candidates) const noexcept {
        std::size_t kept = 0;
        for (std::size_t index = 0; index != candidates.count; ++index) {
            if (!((contained[index / 8] >> (index % 8)) & 1u)) continue;
            candidates.indices[kept] = candidates.indices[index];
            views[kept] = views[index];
            ++kept;
        }
        candidates.count = kept;
        if (!scorer || !kept) return {};
        // Scores ignore lengths, so a value scores the same whatever page or call lists it
        sz_strings_view_t scored {};
        sz_strings_from_string_views(views.data(), kept, &scored);
        sz_substrings_bm25_t const parameters {.term_frequency_saturation = 1.2f,
                                               .length_normalization = 0.0f,
                                               .average_document_length = 0.0f};
        sz_status_t const scoring = sz_substrings_bm25_scores(scorer, &scored, nullptr, &parameters, weights,
                                                              candidates.scores.data(), 1, nullptr);
        if (scoring != sz_success_k)
            return stringzilla_refusal(scoring, status_code_t::invalid_argument_k, "Failed to score the values");
        return {};
    }
};

/** A device match: the engine compiled for the executor's device, and the host half asking the predicate and scoring
 *  what the device kept, as the listing runs on the host either way. */
template <home_t home_>
    requires(home_ != home_t::host_k)
struct compiled_match<home_> {

    /** The compiled engine on the executor's device, if the match reads values. */
    value_matcher_t matcher;

    /** The caller's predicate and the BM25 scorer, whose own matcher reads no values. */
    compiled_match<home_t::host_k> host;

    /** @c matcher rebased onto its tables' copy in the executor's managed runs, which the kernels
     *  take by value, or nothing for an engine no kernel walks, which the host matches. */
    std::variant<match_everything_t, sz_regex_engine_t, sz_substrings_engine_t, sz_subdocuments_engine_t> on_device;

    /** The column images a regex group or chunk primes from, in the executor's managed
     *  runs, or null. */
    std::uint32_t const* images = nullptr;

    /** Whether the match reads the values of the keys it narrows. */
    bool reads_values() const noexcept { return !std::holds_alternative<match_everything_t>(matcher); }

    /** Whether the match narrows nothing, so the plain scan serves. */
    bool matches_everything() const noexcept { return !reads_values() && host.matches_everything(); }
};

/** Whether @p arguments_type_ is a scan or cursor init carrying a match. */
template <typename arguments_type_>
concept carries_match = requires(arguments_type_ const& arguments) { arguments.predicate; };

/** Whether the patterns of a call match everything. */
inline bool matches_everything(ustore_patterns_t const& patterns) noexcept {
    return !patterns.count && !patterns.patterns.data.u32_tape.offsets && !patterns.weights;
}

/** Whether the patterns or the filter of @p arguments read the values of the keys they narrow. */
template <typename arguments_type_>
bool reads_values(arguments_type_ const& arguments) noexcept {
    if constexpr (requires { arguments.patterns; }) return !matches_everything(arguments.patterns);
    else if constexpr (requires { arguments.filter; }) return arguments.filter.length != 0;
    else return false;
}

/** The strings @p view holds, @p count of them, as StringZilla reads them, viewing @p view's own memory. */
inline sz_strings_view_t stringzilla_strings(ustore_strings_view_t const& view, std::size_t count, ustore_arena_t work,
                                             status_t& status) noexcept {
    sz_strings_view_t strings {};
    strings.count = count;
    if (view.layout == ustore_strings_u32_tape_k) {
        strings.layout = sz_strings_u32_tape_k;
        strings.data.u32_tape.data = reinterpret_cast<sz_cptr_t>(view.data.u32_tape.data);
        strings.data.u32_tape.offsets = view.data.u32_tape.offsets;
        return strings;
    }
    if (view.layout == ustore_strings_u64_tape_k) {
        strings.layout = sz_strings_u64_tape_k;
        strings.data.u64_tape.data = reinterpret_cast<sz_cptr_t>(view.data.u64_tape.data);
        strings.data.u64_tape.offsets = view.data.u64_tape.offsets;
        return strings;
    }
    auto [views, views_status] = arena_allocate<sz_string_view_t>(work, count);
    status = views_status;
    if (views_status.failed()) return strings;
    std::array<ustore_size_t, 64> indices;
    std::array<ustore_bytes_view_t, 64> gathered;
    for (std::size_t first = 0; first < count; first += indices.size()) {
        std::size_t const block = std::min(indices.size(), count - first);
        for (std::size_t index = 0; index != block; ++index) indices[index] = first + index;
        view.data.accessor.get(view.data.accessor.handle, indices.data(), block, gathered.data());
        for (std::size_t index = 0; index != block; ++index) {
            views[first + index].start = reinterpret_cast<sz_cptr_t>(gathered[index].data);
            views[first + index].length = static_cast<sz_size_t>(gathered[index].length);
        }
    }
    sz_strings_from_string_views(views.data(), count, &strings);
    return strings;
}

/** Compiles @p patterns, which the scans of blobs and strings take, into @p matcher: an engine for the kernels
 *  @p capabilities allow, whose handle is laid out in @p into and whose state @p engine_memory holds. */
inline status_t compile_patterns(ustore_patterns_t const& patterns, ustore_arena_t into, sz_capability_t capabilities,
                                 sz_allocator_t engine_memory, value_matcher_t& matcher) noexcept {
    matcher = match_everything_t {};
    if (matches_everything(patterns)) return {};
    if (patterns.kind != ustore_pattern_regex_k && patterns.kind != ustore_pattern_substrings_k)
        return {status_code_t::invalid_argument_k, "Unknown pattern kind"};
    if (patterns.sensitivity != ustore_cased_k && patterns.sensitivity != ustore_uncased_k)
        return {status_code_t::invalid_argument_k, "Unknown case sensitivity"};
    if (patterns.weights && patterns.kind == ustore_pattern_regex_k)
        return {status_code_t::invalid_argument_k, "Weights score substrings, never regexes"};
    if (!patterns.count) return {status_code_t::invalid_argument_k, "Patterns need their count"};
    status_t gathered;
    sz_strings_view_t const strings = stringzilla_strings(patterns.patterns, patterns.count, into, gathered);
    if (gathered.failed()) return gathered;
    sz_case_sensitivity_t const sensitivity = patterns.sensitivity == ustore_uncased_k ? sz_uncased_k : sz_cased_k;
    if (patterns.kind == ustore_pattern_regex_k) {
        auto [compiled, compiled_status] = arena_allocate<sz_regex_engine_t>(into, 1);
        if (compiled_status.failed()) return compiled_status;
        sz_size_t malformed_index = 0;
        sz_status_t const status = sz_regex_engine_init(compiled.data(), &strings, sensitivity, &malformed_index,
                                                        capabilities, &engine_memory, nullptr);
        if (status != sz_success_k)
            return stringzilla_refusal(status, status_code_t::invalid_argument_k,
                                       "Failed to compile the regular expression");
        matcher = compiled.data();
        return {};
    }
    auto [compiled, compiled_status] = arena_allocate<sz_substrings_engine_t>(into, 1);
    if (compiled_status.failed()) return compiled_status;
    sz_status_t const status = sz_substrings_engine_init(
        compiled.data(), &strings, sensitivity, sz_substrings_leftmost_longest_k,
        STRINGZILLA_SUBSTRINGS_HOT_STATES_AUTO, 0, 0, capabilities, &engine_memory, nullptr);
    if (status != sz_success_k)
        return stringzilla_refusal(status, status_code_t::invalid_argument_k, "Failed to compile the patterns");
    matcher = compiled.data();
    return {};
}

/** Compiles @p filter, a JSONPath or root filter of documents or an XPath of pages as @p format names, into
 *  @p matcher, as @c compile_patterns compiles patterns. */
inline status_t compile_filter(ustore_bytes_view_t filter, sz_subdocuments_format_t format, ustore_arena_t into,
                               sz_capability_t capabilities, sz_allocator_t engine_memory,
                               value_matcher_t& matcher) noexcept {
    matcher = match_everything_t {};
    if (!filter.length) return {};
    if (!filter.data) return {status_code_t::invalid_argument_k, "A filter of some length needs its bytes"};
    sz_string_view_t path;
    path.start = reinterpret_cast<sz_cptr_t>(filter.data);
    path.length = static_cast<sz_size_t>(filter.length);
    sz_strings_view_t paths {};
    sz_strings_from_string_views(&path, 1, &paths);
    auto [compiled, compiled_status] = arena_allocate<sz_subdocuments_engine_t>(into, 1);
    if (compiled_status.failed()) return compiled_status;
    sz_size_t malformed_index = 0;
    sz_status_t const status = sz_subdocuments_engine_init(compiled.data(), &paths, format,
                                                           sz_subdocuments_options_none_k, &malformed_index,
                                                           capabilities, &engine_memory, nullptr);
    if (status != sz_success_k)
        return stringzilla_refusal(status, status_code_t::invalid_argument_k, "Failed to compile the filter");
    matcher = compiled.data();
    return {};
}

/** Compiles the patterns or the filter of @p arguments into @p matcher, as @c compile_patterns does. */
template <typename arguments_type_>
status_t compile_values(arguments_type_ const& arguments, ustore_arena_t into, sz_capability_t capabilities,
                        sz_allocator_t engine_memory, value_matcher_t& matcher) noexcept {
    if constexpr (requires { arguments.patterns; })
        return compile_patterns(arguments.patterns, into, capabilities, engine_memory, matcher);
    else if constexpr (std::is_same_v<arguments_type_, ustore_docs_scan_t> ||
                       std::is_same_v<arguments_type_, ustore_docs_cursor_init_t>)
        return compile_filter(arguments.filter, sz_subdocuments_json_k, into, capabilities, engine_memory, matcher);
    else if constexpr (requires { arguments.filter; })
        return compile_filter(arguments.filter, sz_subdocuments_xml_k, into, capabilities, engine_memory, matcher);
    else return {};
}

/** Compiles what @p arguments match on the host, laying the engine handles and their state out in @p into, which must
 *  outlive the match, for the kernels @p capabilities allow. */
template <typename arguments_type_>
expected<compiled_match<home_t::host_k>> compile_match(arguments_type_ const& arguments, ustore_arena_t into,
                                                       sz_capability_t capabilities) noexcept {
    compiled_match<home_t::host_k> match;
    if (status_t const status = compile_values(arguments, into, capabilities, stringzilla_allocator(into),
                                               match.matcher);
        status.failed())
        return status;
    if constexpr (requires { arguments.patterns; })
        if (arguments.patterns.weights)
            match.scorer = std::get<sz_substrings_engine_t*>(match.matcher), match.weights = arguments.patterns.weights;
    if (arguments.predicate.cost != ustore_predicate_cheap_k &&
        arguments.predicate.cost != ustore_predicate_expensive_k)
        return status_t {status_code_t::invalid_argument_k, "Unknown predicate cost"};
    match.predicate = arguments.predicate;
    return match;
}

#pragma region Plans

/**
 *  @brief Long work split into disjoint steps per phase: @c steps counts the current phase's steps, zero once the plan
 *      is done, @c run serves one of them without allocating, and @c advance is the barrier into the next phase.
 *
 *  Progress counts steps, and cancellation is checked between them.
 */
template <typename plan_type_>
concept plan_concept = requires(plan_type_& plan, typename plan_type_::step_t step, view_t const& view) {
    { plan.steps() } -> std::convertible_to<std::size_t>;
    { plan.run(step, view) } -> std::same_as<status_t>;
    plan.advance();
};

/** Runs every phase of @p plan on the thread @p view names, step after step, reporting one unit of @p progress per
 *  step and unwinding with @c progress_cancelled_k once it answers false. */
template <plan_concept plan_type_>
status_t run_steps(plan_type_& plan, view_t const& view, progress_t& progress) noexcept {
    for (std::size_t steps = plan.steps(); steps != 0; plan.advance(), steps = plan.steps())
        for (std::size_t step = 0; step != steps; ++step) {
            if (status_t const status = plan.run(static_cast<typename plan_type_::step_t>(step), view); status.failed())
                return status;
            if (!progress.advance(1)) return progress_cancelled_k;
        }
    return {};
}

#pragma endregion Plans

#pragma region Keyed Listings

/** Reads into @p values the values of @p keys of @p collection through the host verbs of @p engine, as the scan of
 *  @p scan_type_ matches them: blobs as stored, documents whole, pages rendered as XML. */
template <typename scan_type_>
status_t read_page_values(home_tables<home_t::host_k> const& engine, ustore_database_t database,
                          ustore_collection_id_t const& collection, std::span<ustore_key_t const> keys,
                          ustore_arena_t page, view_t const& view, strings_arg_t& values) noexcept {
    values = {};
    if constexpr (std::is_same_v<scan_type_, ustore_blobs_scan_t>) {
        ustore_blobs_read_t const read {.database = database,
                                        .count = keys.size(),
                                        .collections = {&collection, 0},
                                        .keys = {keys.data(), sizeof(ustore_key_t)},
                                        .values = &values.view,
                                        .arena = page};
        return engine.blobs->read(read, view);
    }
    else if constexpr (std::is_same_v<scan_type_, ustore_docs_scan_t>) {
        if (!engine.docs->read) return {status_code_t::unsupported_k, "The engine does not read documents"};
        ustore_docs_read_t const read {.database = database,
                                       .count = keys.size(),
                                       .collections = {&collection, 0},
                                       .keys = {keys.data(), sizeof(ustore_key_t)},
                                       .values = &values.view,
                                       .arena = page};
        return engine.docs->read(read, strings_arg_t {}, view);
    }
    else if constexpr (std::is_same_v<scan_type_, ustore_pages_scan_t>) {
        if (!engine.pages->read) return {status_code_t::unsupported_k, "The engine does not render pages"};
        ustore_pages_read_t const read {.database = database,
                                        .count = keys.size(),
                                        .collections = {&collection, 0},
                                        .keys = {keys.data(), sizeof(ustore_key_t)},
                                        .values = &values.view,
                                        .arena = page,
                                        .outputs_encoding = ustore_page_encoding_xml_k};
        return engine.pages->read(read, view);
    }
    else return {status_code_t::invalid_argument_k, "The modality matches no values"};
}

/** The scores of the keys or strings a listing kept, grown in scratch beside them. */
using scores_sink_t = st::vector<float, arena_allocator<float>>;

/** The keys a keyed listing kept, grown in scratch until they reach the call's arena. */
using keyed_survivors_t = st::vector<ustore_key_t, arena_allocator<ustore_key_t>>;

/**
 *  @brief Appends to @p survivors up to @p wanted keys of @p collection from @p start on that @p match keeps, listing
 *      pages through the host scan verb of @p scan_type_ in @p page, which every page rewinds.
 *
 *  The pages are ordered, whatever the scan's mode, as each resumes past the last key of the one before. Answers the
 *  key the next listing resumes at, or nothing once the collection ends.
 */
template <typename scan_type_, typename match_type_>
status_t list_matched_keys(home_tables<home_t::host_k> const& engine, ustore_database_t database,
                           ustore_collection_id_t const& collection, ustore_key_t start, std::size_t wanted,
                           match_type_ const& match, ustore_arena_t page, view_t const& view,
                           keyed_survivors_t& survivors, std::variant<ustore_key_t, exhausted_t>& resume,
                           scores_sink_t* scores = nullptr) noexcept {
    auto const* const verbs = engine.*verb_slot<scan_type_>::template table<home_t::host_k>;
    auto const scan_verb = verbs->*verb_slot<scan_type_>::template verb<home_t::host_k>;
    view_t page_view = view;
    page_view.scratch = page;
    std::size_t const first = survivors.size();
    resume = start;
    while (survivors.size() - first < wanted) {
        match.rewind_page(page);
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* listed = nullptr;
        scan_type_ const scan {.database = database,
                               .count = 1,
                               .collections = {&collection, 0},
                               .start_keys = {&start, 0},
                               .offsets = &offsets,
                               .keys = &listed,
                               .arena = page,
                               .wanted = wanted};
        if (status_t const status = scan_verb(scan, page_view); status.failed()) return status;
        std::size_t const listed_count = offsets ? offsets[1] : 0;
        std::span<ustore_key_t const> const keys {listed, listed_count};

        auto [indices, indices_status] = arena_allocate<std::uint32_t>(page, listed_count);
        if (indices_status.failed()) return indices_status;
        candidates_t candidates {indices, {}, listed_count};
        if (scores && match.scores()) {
            auto [scored, scored_status] = arena_allocate<float>(page, listed_count);
            if (scored_status.failed()) return scored_status;
            candidates.scores = scored;
        }
        for (std::size_t index = 0; index != listed_count; ++index) indices[index] = static_cast<std::uint32_t>(index);
        if (status_t const status = match.ask(ustore_predicate_cheap_k, keys, candidates, page); status.failed())
            return status;
        if (match.reads_values() && candidates.count) {
            auto [asked, asked_status] = arena_allocate<ustore_key_t>(page, candidates.count);
            if (asked_status.failed()) return asked_status;
            for (std::size_t index = 0; index != candidates.count; ++index) asked[index] = keys[indices[index]];
            strings_arg_t values;
            if (status_t const status = read_page_values<scan_type_>(engine, database, collection, asked, page,
                                                                     page_view, values);
                status.failed())
                return status;
            if (status_t const status = match.select(values, candidates, page); status.failed()) return status;
        }
        if (status_t const status = match.ask(ustore_predicate_expensive_k, keys, candidates, page); status.failed())
            return status;

        std::size_t const room = wanted - (survivors.size() - first);
        std::size_t const taken = candidates.count < room ? candidates.count : room;
        for (std::size_t index = 0; index != taken; ++index) {
            if (status_t const status = growth_status(survivors.push_back(ustore_key_t {keys[indices[index]]}));
                status.failed())
                return status;
            if (!candidates.scores.empty())
                if (status_t const status = growth_status(scores->push_back(float {candidates.scores[index]}));
                    status.failed())
                    return status;
        }

        // A page short of what it wanted reached the end of the collection, and so did one ending at the last key
        ustore_key_t const* const last = listed_count ? listed + listed_count - 1 : nullptr;
        if (listed_count < wanted || (last && *last == largest_key_k)) {
            resume = exhausted_t {};
            break;
        }
        // A page cut short by the room left resumes after the last key handed out, never after the page
        start = taken < candidates.count ? key_after(keys[indices[taken - 1]]) : key_after(*last);
        resume = start;
    }
    return {};
}

/** A filtered scan of keys as a plan of one phase, one step per task, each listing its task's survivors after those of
 *  the tasks before it. */
template <typename scan_type_, typename match_type_>
struct keyed_scan_plan {
    using step_t = std::size_t;

    home_tables<home_t::host_k> const& engine;
    match_type_ const& match;
    scan_type_ const& arguments;
    std::span<ustore_length_t> offsets;
    keyed_survivors_t& survivors;
    scores_sink_t* scores = nullptr;
    std::size_t phases_left = 1;

    std::size_t steps() const noexcept { return phases_left ? arguments.count : 0; }
    void advance() noexcept { phases_left = 0; }

    status_t run(step_t task, view_t const& view) noexcept {
        offsets[task] = static_cast<ustore_length_t>(survivors.size());
        auto const signed_task = static_cast<std::ptrdiff_t>(task);
        strided_iterator<ustore_key_t const> const start_keys = strided(arguments.start_keys);
        strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
        ustore_key_t const start = start_keys ? start_keys[signed_task] : smallest_key_k;
        std::variant<ustore_key_t, exhausted_t> resume;
        return list_matched_keys<scan_type_>(engine, arguments.database, collections[signed_task], start,
                                             arguments.wanted, match, arguments.arena, view, survivors, resume, scores);
    }
};

/** Serves the scan @p arguments with @p match through the host verbs of @p engine as a plan of one step per task, the
 *  call's arena holding one page at a time until the survivors are copied into it. */
template <typename scan_type_, typename match_type_>
status_t scan_matched(home_tables<home_t::host_k> const& engine, match_type_ const& match, scan_type_ const& arguments,
                      view_t const& view) noexcept {
    if (status_t const status = validate_scan(arguments); status.failed() || !arguments.count) return status;
    auto [offsets, offsets_status] = arena_allocate<ustore_length_t>(view.scratch, arguments.count + 1);
    if (offsets_status.failed()) return offsets_status;
    keyed_survivors_t survivors {arena_allocator<ustore_key_t> {view.scratch}};
    scores_sink_t scores {arena_allocator<float> {view.scratch}};
    bool const scored = [&]() noexcept {
        if constexpr (requires { arguments.scores; }) return arguments.scores && match.scores();
        else return false;
    }();
    progress_t progress {arguments.progress, arguments.progress_state, arguments.count};
    scores_sink_t* const kept_scores = scored ? &scores : nullptr;
    keyed_scan_plan<scan_type_, match_type_> plan {engine, match, arguments, offsets, survivors, kept_scores};
    if (status_t const status = run_steps(plan, view, progress); status.failed()) return status;
    offsets[arguments.count] = static_cast<ustore_length_t>(survivors.size());

    match.rewind_page(arguments.arena);
    auto [copied_offsets, copied_offsets_status] = arena_copy<ustore_length_t>(arguments.arena, offsets);
    if (copied_offsets_status.failed()) return copied_offsets_status;
    auto [keys, keys_status] = arena_copy<ustore_key_t>(arguments.arena, survivors);
    if (keys_status.failed()) return keys_status;
    if constexpr (requires { arguments.scores; })
        if (scored) {
            auto [copied_scores, scores_status] = arena_copy<float>(arguments.arena, scores);
            if (scores_status.failed()) return scores_status;
            *arguments.scores = copied_scores.data();
        }
    *arguments.offsets = copied_offsets.data();
    *arguments.keys = keys.data();
    return {};
}

#pragma endregion Keyed Listings

#pragma region Strings Listings

/**
 *  @brief Appends to @p survivors up to @p wanted strings of @p collection under @p prefix
 *      from @p start on that @p match keeps, listing pages through the host strings scan
 *      of @p engine in @p page, which every page rewinds.
 *
 *  The predicate sees the keys the strings map onto, which a read of every page's strings finds. Answers the string
 *  the next listing resumes at in @p resume, kept in @p resume_bytes, or nothing once the prefix ends.
 */
template <typename match_type_, typename survivors_type_, typename resume_bytes_type_>
status_t list_matched_strings(home_tables<home_t::host_k> const& engine, ustore_database_t database,
                              ustore_collection_id_t const& collection, ustore_bytes_view_t prefix,
                              ustore_bytes_view_t start, std::size_t wanted, match_type_ const& match,
                              ustore_arena_t page, view_t const& view, survivors_type_& survivors,
                              resume_bytes_type_& resume_bytes, std::variant<ustore_bytes_view_t, exhausted_t>& resume,
                              scores_sink_t* scores = nullptr) noexcept {
    strings_verbs<home_t::host_k> const& verbs = *engine.strings;
    view_t page_view = view;
    page_view.scratch = page;
    std::size_t const first = survivors.size();
    resume = start;
    while (survivors.size() - first < wanted) {
        match.rewind_page(page);
        ustore_length_t const* offsets = nullptr;
        ustore_strings_view_t listed_view {};
        ustore_strings_scan_t const scan {.database = database,
                                          .count = 1,
                                          .collections = {&collection, 0},
                                          .offsets = &offsets,
                                          .strings = &listed_view,
                                          .arena = page,
                                          .wanted = wanted};
        if (status_t const status = verbs.scan(scan, strings_arg_t::from_views(&prefix),
                                               strings_arg_t::from_views(&start), page_view);
            status.failed())
            return status;
        std::size_t const listed_count = offsets ? offsets[1] : 0;
        strings_arg_t const listed {listed_view};

        ustore_key_t const* keys = nullptr;
        if (match.asks() && listed_count) {
            ustore_strings_read_t const read {.database = database,
                                              .count = listed_count,
                                              .collections = {&collection, 0},
                                              .values = &keys,
                                              .arena = page};
            if (status_t const status = verbs.read(read, listed, page_view); status.failed()) return status;
        }
        std::span<ustore_key_t const> const listed_keys {keys, keys ? listed_count : 0};

        auto [indices, indices_status] = arena_allocate<std::uint32_t>(page, listed_count);
        if (indices_status.failed()) return indices_status;
        candidates_t candidates {indices, {}, listed_count};
        if (scores && match.scores()) {
            auto [scored, scored_status] = arena_allocate<float>(page, listed_count);
            if (scored_status.failed()) return scored_status;
            candidates.scores = scored;
        }
        for (std::size_t index = 0; index != listed_count; ++index) indices[index] = static_cast<std::uint32_t>(index);
        if (status_t const status = match.ask(ustore_predicate_cheap_k, listed_keys, candidates, page); status.failed())
            return status;
        if (match.reads_values() && candidates.count) {
            auto [views, views_status] = arena_allocate<ustore_bytes_view_t>(page, candidates.count);
            if (views_status.failed()) return views_status;
            for (std::size_t index = 0; index != candidates.count; ++index) views[index] = listed[indices[index]];
            if (status_t const status = match.select(strings_arg_t::from_views(views.data()), candidates, page);
                status.failed())
                return status;
        }
        if (status_t const status = match.ask(ustore_predicate_expensive_k, listed_keys, candidates, page);
            status.failed())
            return status;

        std::size_t const room = wanted - (survivors.size() - first);
        std::size_t const taken = candidates.count < room ? candidates.count : room;
        for (std::size_t index = 0; index != taken; ++index) {
            if (status_t const status = survivors.push_back(listed[indices[index]]); status.failed()) return status;
            if (!candidates.scores.empty())
                if (status_t const status = growth_status(scores->push_back(float {candidates.scores[index]}));
                    status.failed())
                    return status;
        }

        if (listed_count < wanted) {
            resume = exhausted_t {};
            break;
        }
        // The smallest string after the last one consumed is that string followed by a zero byte
        ustore_bytes_view_t const last = taken < candidates.count ? listed[indices[taken - 1]]
                                                                  : listed[listed_count - 1];
        if (status_t const status = growth_status(resume_bytes.resize(last.length + 1)); status.failed()) return status;
        if (last.length) std::memcpy(resume_bytes.data(), last.data, last.length);
        resume_bytes[last.length] = byte_t {0};
        start = {reinterpret_cast<ustore_byte_t const*>(resume_bytes.data()), last.length + 1};
        resume = start;
    }
    return {};
}

/** Serves the strings scan @p arguments with @p match through the host verbs of @p engine, one filtered listing per
 *  task, as @c scan_matched serves the keyed scans. */
template <typename match_type_>
status_t scan_matched(home_tables<home_t::host_k> const& engine, match_type_ const& match,
                      ustore_strings_scan_t const& arguments, strings_arg_t const& prefixes,
                      strings_arg_t const& starts, view_t const& view) noexcept {
    if (arguments.mode != ustore_scan_ordered_k && arguments.mode != ustore_scan_unordered_k)
        return {status_code_t::invalid_argument_k, "Unknown scan mode"};
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};
    if (!arguments.count) return {};
    auto [offsets, offsets_status] = arena_allocate<ustore_length_t>(view.scratch, arguments.count + 1);
    if (offsets_status.failed()) return offsets_status;
    growing_strings<arena_allocator<byte_t>> survivors {arena_allocator<byte_t> {view.scratch}};
    st::vector<byte_t, arena_allocator<byte_t>> resume_bytes {arena_allocator<byte_t> {view.scratch}};
    scores_sink_t scores {arena_allocator<float> {view.scratch}};
    bool const scored = arguments.scores && match.scores();
    progress_t progress {arguments.progress, arguments.progress_state, arguments.count};
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    for (std::size_t task = 0; task != arguments.count; ++task) {
        offsets[task] = static_cast<ustore_length_t>(survivors.size());
        ustore_bytes_view_t const prefix = prefixes ? prefixes[task] : ustore_bytes_view_t {};
        ustore_bytes_view_t const start = starts ? starts[task] : ustore_bytes_view_t {};
        std::variant<ustore_bytes_view_t, exhausted_t> resume;
        if (status_t const status = list_matched_strings(engine, arguments.database,
                                                         collections[static_cast<std::ptrdiff_t>(task)], prefix, start,
                                                         arguments.wanted, match, arguments.arena, view, survivors,
                                                         resume_bytes, resume, scored ? &scores : nullptr);
            status.failed())
            return status;
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    offsets[arguments.count] = static_cast<ustore_length_t>(survivors.size());

    match.rewind_page(arguments.arena);
    auto [copied_offsets, copied_offsets_status] = arena_copy<ustore_length_t>(arguments.arena, offsets);
    if (copied_offsets_status.failed()) return copied_offsets_status;
    if (status_t const status = survivors.export_into(arguments.arena, *arguments.strings); status.failed())
        return status;
    if (scored) {
        auto [copied_scores, scores_status] = arena_copy<float>(arguments.arena, scores);
        if (scores_status.failed()) return scores_status;
        *arguments.scores = copied_scores.data();
    }
    *arguments.offsets = copied_offsets.data();
    return {};
}

#pragma endregion Strings Listings

} // namespace unum::ustore
