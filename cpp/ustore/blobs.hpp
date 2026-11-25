/**
 *  @file cpp/ustore/blobs.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief Handles to one binary collection, the proxies reading and writing values at a location or
 *      a batch of them, the cursors, streams and slices over keys and pairs, and the samples and
 *      measures every modality keyed by UUIDs shares.
 */
#pragma once

#include <cstddef> // `std::size_t`, `std::ptrdiff_t`

#include <algorithm>        // `std::ranges::lower_bound`
#include <concepts>         // `std::convertible_to`
#include <initializer_list> // `std::initializer_list`
#include <iterator>         // `std::forward_iterator_tag`, `std::default_sentinel_t`, `std::size`
#include <span>             // `std::span`
#include <type_traits>      // `std::conditional_t`, `std::is_rvalue_reference_v`, `std::invoke_result_t` & kin
#include <utility>          // `std::forward`, `std::move`

#include "ustore/types.hpp" // `arena_t`, `receiver_t`, `answered_`, `places_in`, `strings_view_t`, `values_view_t`

namespace unum::ustore {

#pragma region Owned Answers

/** Whether @p view_type_ compares with @p other_type_ as a value, as a value view does with a
 *  string. */
template <typename view_type_, typename other_type_>
concept a_view_comparable_with = requires(view_type_ const& view, other_type_ const& other) {
    { view == other } -> std::convertible_to<bool>;
};

/** A view of what one call wrote into an arena made for that call alone, which it frees on
 *  destruction, as shorthand with no place for an arena answers. It moves but never copies, and
 *  reads as the view it holds. */
template <typename view_type_>
class owned {
    view_type_ view_ {};
    arena_t arena_;

  public:
    owned() noexcept = default;
    owned(view_type_ const& view, arena_t arena) noexcept : view_(view), arena_(std::move(arena)) {}

    view_type_ const& view() const noexcept { return view_; }
    operator view_type_ const&() const noexcept { return view_; }
    view_type_ const* operator->() const noexcept { return &view_; }

    using value_type = typename view_type_::value_type;

    explicit operator bool() const noexcept { return static_cast<bool>(view_); }
    std::size_t size() const noexcept { return view_.size(); }
    value_type operator[](std::size_t index) const noexcept { return view_[index]; }

    /** Where a loop over the view starts, for the views that iterate, which bit sets do not. */
    template <typename iterated_type_ = view_type_>
    typename iterated_type_::iterator_t begin() const noexcept {
        return view_.begin();
    }

    /** Where a loop over the view ends, for the views that iterate. */
    template <typename iterated_type_ = view_type_>
    typename iterated_type_::iterator_t end() const noexcept {
        return view_.end();
    }

    /** Compares the view with anything it compares with, as the view itself would. */
    template <typename other_type_>
        requires a_view_comparable_with<view_type_, other_type_>
    bool operator==(other_type_ const& other) const noexcept {
        return view_ == other;
    }
};

using owned_value_t = owned<value_view_t>;

/** What shorthand answers for a call answering @p answer_type_: a scalar copied out of its one-off
 *  arena, or a view owning that arena. */
template <typename answer_type_>
using one_off_answer_of = std::conditional_t<std::is_scalar_v<answer_type_>, answer_type_, owned<answer_type_>>;

/** What @c in_one_off_arena answers for @p verb_type_, a verb given an arena. */
template <typename verb_type_>
using one_off_result_of = expected<one_off_answer_of<answer_value_t<std::invoke_result_t<verb_type_&, arena_t&>>>>;

/** Runs @p verb on an arena made for it alone where @p executor computes, as shorthand with no
 *  place for an arena does, answering a scalar copied out of that arena or a view that owns it. */
template <typename verb_type_>
one_off_result_of<verb_type_> in_one_off_arena(ustore_executor_t executor, verb_type_&& verb) noexcept {
    using answer_t = answer_value_t<std::invoke_result_t<verb_type_&, arena_t&>>;
    auto [arena, arena_status] = arena_t::make({.executor = executor});
    if (arena_status.failed()) return {{}, arena_status};
    auto [answer, answer_status] = verb(arena);
    if (answer_status.failed()) return {{}, answer_status};
    if constexpr (std::is_scalar_v<answer_t>) return {answer};
    else return {owned<answer_t> {answer, std::move(arena)}};
}

#pragma endregion Owned Answers

#pragma region Verbs

/**
 *  @brief Reads what @p answer_type_ asks of the values at @p places into @p arena: the values,
 *      their lengths, or only whether each is present.
 *  @param[in] receiver The database, executor, and transaction or snapshot the read sees.
 *  @param[in] places Collections and keys to read, owned by the caller until the call is final.
 *  @param[in] arena Memory the answer lands in, owned by the caller until the call is final, which
 *      the answer views.
 *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
 */
template <ustore_completion_t completion_, typename answer_type_>
deferrable<answer_type_, completion_> blobs_read_(receiver_t const& receiver, place_batch_t const& places,
                                                  arena_t& arena, ustore_watch_t watch) noexcept {
    return answered_<deferrable<answer_type_, completion_>>(
        [&](ustore_outcome_t* outcome, outputs<answer_type_>* written) noexcept {
            written->count = places.count;
            ustore_blobs_read_t arguments {.database = receiver.database,
                                           .executor = receiver.executor,
                                           .outcome = outcome,
                                           .completion = completion_,
                                           .view = receiver.view(watch),
                                           .count = places.count,
                                           .collections = places.collections(),
                                           .keys = places.keys(),
                                           .presences = &written->presences,
                                           .lengths = lengths_output_(*written),
                                           .values = values_output_(*written),
                                           .arena = arena.handle()};
            ustore_blobs_read(&arguments);
        });
}

/**
 *  @brief Writes @p values to @p places pairwise, or one value to every place.
 *  @param[in] values Values to write, owned by the caller until the call is final.
 *  @param[in] durability Whether the call finishes only after the data reaches the disk.
 */
template <ustore_completion_t completion_>
deferrable<void, completion_> blobs_write_(receiver_t const& receiver, place_batch_t const& places,
                                           ustore_strings_view_t values, ustore_durability_t durability) noexcept {
    return answered_<deferrable<void, completion_>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_blobs_write_t arguments {.database = receiver.database,
                                        .executor = receiver.executor,
                                        .outcome = outcome,
                                        .completion = completion_,
                                        .transaction = receiver.transaction,
                                        .count = places.count,
                                        .collections = places.collections(),
                                        .keys = places.keys(),
                                        .values = values,
                                        .durability = durability};
        ustore_blobs_write(&arguments);
    });
}

/** Removes the keys at @p places together with their values. */
template <ustore_completion_t completion_>
deferrable<void, completion_> blobs_remove_(receiver_t const& receiver, place_batch_t const& places,
                                            ustore_durability_t durability) noexcept {
    return answered_<deferrable<void, completion_>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_blobs_remove_t arguments {.database = receiver.database,
                                         .executor = receiver.executor,
                                         .outcome = outcome,
                                         .completion = completion_,
                                         .transaction = receiver.transaction,
                                         .count = places.count,
                                         .collections = places.collections(),
                                         .keys = places.keys(),
                                         .durability = durability};
        ustore_blobs_remove(&arguments);
    });
}

#pragma endregion Verbs

#pragma region Proxies

/**
 *  @brief A proxy that reads the values at one location or at a batch of locations, through a
 *      snapshot, a transaction or the head, and writes them too when @p access_ allows. One value
 *      may go to one key or to many keys, and many values to as many keys.
 *
 *  A read given an arena answers views into it, valid until the next call given that arena. The
 *  same read without one is shorthand: it makes a one-off arena and answers a result owning it, or
 *  a scalar copied out of it, which is slower than reusing an arena and keeps no state anywhere.
 *  Every call blocks.
 *
 *  @tparam locations_type_ A single location, a batch of locations, or a reference to a batch
 *      the caller owns.
 */
template <typename locations_type_, collection_access_t access_ = collection_access_t::read_write_k>
class blobs_ref {
  public:
    static_assert(!std::is_rvalue_reference_v<locations_type_>, "The locations can't be stored as an rvalue reference");

    using locations_store_t = location_store<locations_type_>;
    using locations_plain_t = typename locations_store_t::plain_t;
    static constexpr bool is_one_k = places_arg_extractor<locations_plain_t>::is_one_k;
    static constexpr bool writes_k = access_ == collection_access_t::read_write_k;

    using value_t = std::conditional_t<is_one_k, value_view_t, values_view_t>;
    using present_t = std::conditional_t<is_one_k, bool, bits_view_t>;
    using length_t = std::conditional_t<is_one_k, ustore_length_t, lengths_view_t>;

  private:
    receiver_t receiver_ {};
    locations_store_t locations_store_ {};

    place_batch_t places_() const noexcept { return locations_places_(locations_store_.ref()); }

  public:
    blobs_ref(receiver_t receiver, locations_type_&& locations) noexcept
        : receiver_(receiver), locations_store_(std::forward<locations_type_>(locations)) {}

    /** Reads the values into @p arena. */
    expected<value_t> value(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return first_or_all_<value_t>(
            blobs_read_<ustore_completion_blocking_k, values_view_t>(receiver_, places_(), arena, watch));
    }

    /** Reads the values into a one-off arena, which the answer owns. */
    expected<one_off_answer_of<value_t>> value(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept { return value(arena, watch); });
    }

    /** Reads the lengths of the values into @p arena. */
    expected<length_t> length(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return first_or_all_<length_t>(
            blobs_read_<ustore_completion_blocking_k, lengths_view_t>(receiver_, places_(), arena, watch));
    }

    /** Reads the lengths of the values into a one-off arena. */
    expected<one_off_answer_of<length_t>> length(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept { return length(arena, watch); });
    }

    /** Checks which keys are present into @p arena, where a present value may still be empty. */
    expected<present_t> present(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return first_or_all_<present_t>(
            blobs_read_<ustore_completion_blocking_k, bits_view_t>(receiver_, places_(), arena, watch));
    }

    /** Checks which keys are present into a one-off arena. */
    expected<one_off_answer_of<present_t>> present(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept { return present(arena, watch); });
    }

    /**
     *  @brief Assigns values to the keys pairwise, or one value to every key.
     *  @param[in] values Values to assign.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    template <typename values_argument_type_>
    status_t assign(values_argument_type_&& values,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        values_arg_extractor<std::decay_t<values_argument_type_>> extractor;
        return blobs_write_<ustore_completion_blocking_k>(receiver_, places_(), extractor.view(values), durability);
    }

    /**
     *  @brief Removes the keys together with their values.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    status_t remove(ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        return blobs_remove_<ustore_completion_blocking_k>(receiver_, places_(), durability);
    }

    /**
     *  @brief Keeps the keys but empties their values.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    status_t clear_values(ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        return assign(value_view_t::make_empty(), durability);
    }

    locations_plain_t& locations() noexcept { return locations_store_.ref(); }
    locations_plain_t const& locations() const noexcept { return locations_store_.ref(); }
};

/** A proxy that only reads, as snapshots hand out. */
template <typename locations_type_>
using blobs_read_ref = blobs_ref<locations_type_, collection_access_t::read_only_k>;

static_assert(blobs_read_ref<ustore_key_t>::is_one_k);
static_assert(std::is_same_v<blobs_read_ref<ustore_key_t>::value_t, value_view_t>);
static_assert(!blobs_read_ref<place_batch_t>::is_one_k);

#pragma endregion Proxies

#pragma region Cursors

/** What one pull of a cursor hands over inside the arena it was given: the keys, and the values of
 *  the keys. */
struct blobs_page_t {
    std::span<ustore_key_t const> keys;
    strings_view_t values {};
};

/** What a scan or a cursor narrows its keys by, each part zeroed to keep every key: patterns over
 *  the values of blobs and strings, a JSONPath or XPath filter over documents and pages, and a
 *  predicate over the keys. */
struct match_t {
    ustore_patterns_t patterns {};
    ustore_bytes_view_t filter {};
    ustore_predicate_t predicate {};
};

/** A per-key predicate handler: answers whether one key is kept, through a const call operator,
 *  as the executor's threads may call it at once. */
template <typename handler_type_>
concept a_key_predicate = std::is_nothrow_invocable_r_v<bool, handler_type_ const&, ustore_key_t>;

/** A batch predicate handler: given the keys of a page, it writes one admission per key, through a
 *  const call operator, as the executor's threads may call it at once. */
template <typename handler_type_>
concept a_keys_predicate = std::is_nothrow_invocable_r_v<void, handler_type_ const&, std::span<ustore_key_t const>,
                                                         std::span<ustore_admission_t>>;

/** Answers the admissions of @p count keys one at a time through the handler @p state points to. */
template <a_key_predicate handler_type_>
void admit_each_(ustore_key_t const* keys, ustore_admission_t* admissions, ustore_size_t count, void* state) noexcept {
    handler_type_ const& handler = *static_cast<handler_type_ const*>(state);
    for (ustore_size_t position = 0; position != count; ++position)
        admissions[position] = handler(keys[position]) ? ustore_admit_k : ustore_reject_k;
}

/** Answers the admissions of @p count keys in one batch through the handler @p state points to. */
template <a_keys_predicate handler_type_>
void admit_batch_(ustore_key_t const* keys, ustore_admission_t* admissions, ustore_size_t count, void* state) noexcept {
    std::size_t const size = static_cast<std::size_t>(count);
    (*static_cast<handler_type_ const*>(state))(std::span<ustore_key_t const> {keys, size},
                                                std::span<ustore_admission_t> {admissions, size});
}

/** The predicate a scan calls as <tt>handler(key)</tt>, one key at a time and cheaply, as const from
 *  the executor's threads, so @p handler must outlive every scan and cursor it is given to. */
template <a_key_predicate handler_type_>
    requires(!a_keys_predicate<handler_type_>)
ustore_predicate_t predicate_of(handler_type_& handler) noexcept {
    return {&admit_each_<handler_type_>, &handler, ustore_predicate_cost_cheap_k};
}

/** The predicate a scan calls as <tt>handler(keys, admissions)</tt> over spans, once per page at
 *  @p cost, as const from the executor's threads, so @p handler must outlive every scan and
 *  cursor it is given to. */
template <a_keys_predicate handler_type_>
ustore_predicate_t predicate_of(handler_type_& handler,
                                ustore_predicate_cost_t cost = ustore_predicate_cost_expensive_k) noexcept {
    return {&admit_batch_<handler_type_>, &handler, cost};
}

/** Whether the arguments @p call_type_ take patterns over values, as blobs and strings do. */
template <typename call_type_>
concept a_call_with_patterns = requires(call_type_& call) { call.patterns; };

/** Whether the arguments @p call_type_ take a JSONPath or XPath filter, as documents
 *  and pages do. */
template <typename call_type_>
concept a_call_with_filter = requires(call_type_& call) { call.filter; };

/** Whether the arguments @p call_type_ export values beside the keys they pull. */
template <typename call_type_>
concept a_call_with_values = requires(call_type_& call) { call.values; };

/** Whether the arguments @p call_type_ export one BM25 score per key. */
template <typename call_type_>
concept a_call_with_scores = requires(call_type_& call) { call.scores; };

/** Copies what @p match narrows by into the arguments @p call takes, as far as they
 *  take each part. */
template <typename call_type_>
void narrow(call_type_& call, match_t const& match) noexcept {
    if constexpr (a_call_with_patterns<call_type_>) call.patterns = match.patterns;
    if constexpr (a_call_with_filter<call_type_>) call.filter = match.filter;
    call.predicate = match.predicate;
}

/**
 *  @brief Owns a walk over the keys of one collection from its open until its destruction, which
 *      every pull resumes past the last key the pull before it handed out, at the view its receiver
 *      read at the open.
 *
 *  The explicit form of the streams below: every pull takes the caller's arena, which holds the
 *  page until its next call, so a binding hands the cursor any arena it keeps. It moves but never
 *  copies. One template serves the cursors of blobs, pages and graph vertices, each spelled by its
 *  handle and calls.
 */
template <typename handle_type_, typename init_type_, typename next_type_, void (*init_)(init_type_*),
          void (*next_)(next_type_*), void (*free_)(handle_type_)>
class keys_cursor {

    friend class pairs_stream_t;

    owner<handle_type_, free_> owner_;
    ustore_database_t database_ = nullptr;

    /** Pulls up to @p wanted keys into @p arena on @p executor, with their values into @p values
     *  unless it is null, the other fields of @p next taken as they are. */
    expected<std::span<ustore_key_t const>> pull_(std::size_t wanted, arena_t& arena, ustore_executor_t executor,
                                                  ustore_strings_view_t* values, next_type_ next) noexcept {
        ustore_key_t const* keys = nullptr;
        ustore_size_t written = 0;
        next.database = database_;
        next.executor = executor;
        next.cursor = owner_.handle();
        next.keys = &keys;
        if constexpr (a_call_with_values<next_type_>) next.values = values;
        next.written = &written;
        next.arena = arena.handle();
        next.wanted = wanted;
        status_t const status = answered_<status_t>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            next.outcome = outcome;
            next_(&next);
        });
        if (status.failed()) return {{}, status};
        return {std::span<ustore_key_t const> {keys, static_cast<std::size_t>(written)}};
    }

  public:
    handle_type_ handle() const noexcept { return owner_.handle(); }

    /** The output field @c make writes the opened cursor into. */
    handle_type_* handle_field() noexcept { return owner_.handle_field(); }

    /** Opens a walk over the keys of @p collection from @p start, at the view @p receiver reads,
     *  keeping the keys @p match admits, compiled once here. */
    static expected<keys_cursor> make(receiver_t receiver, ustore_collection_id_t collection,
                                      ustore_key_t start = smallest_key_k, match_t const& match = {}) noexcept {
        init_type_ arguments {.database = receiver.database,
                              .executor = receiver.executor,
                              .view = receiver.view(ustore_watch_keys_k),
                              .collection = collection,
                              .start_key = start};
        narrow(arguments, match);
        auto [opened, status] = opened_<keys_cursor>(init_, arguments, &init_type_::cursor);
        if (status.failed()) return {{}, status};
        opened.database_ = receiver.database;
        return {std::move(opened)};
    }

    /** Pulls up to @p wanted keys into @p arena on @p executor, or on the calling thread without
     *  one, fewer once the walk nears its end and none past it. */
    expected<std::span<ustore_key_t const>> next(std::size_t wanted, arena_t& arena,
                                                 executor_t const& executor = {}) noexcept {
        return pull_(wanted, arena, executor.handle(), nullptr, {});
    }

    /** Pulls up to @p wanted keys with their values into @p arena in one call on @p executor, the
     *  other fields of @p options, such as the encoding pages render in, taken as they are. */
    expected<blobs_page_t> next_pairs(std::size_t wanted, arena_t& arena, executor_t const& executor = {},
                                      next_type_ options = {}) noexcept
        requires a_call_with_values<next_type_>
    {
        ustore_strings_view_t values {};
        auto [keys, status] = pull_(wanted, arena, executor.handle(), &values, options);
        if (status.failed()) return {{}, status};
        return {blobs_page_t {keys, {values, keys.size()}}};
    }
};

/** A walk over the keys of one blobs collection. */
using blobs_cursor_t = keys_cursor<ustore_blobs_cursor_t, ustore_blobs_cursor_init_t, ustore_blobs_cursor_next_t,
                                   &ustore_blobs_cursor_init, &ustore_blobs_cursor_next, &ustore_blobs_cursor_free>;

#pragma endregion Cursors

#pragma region Streams

/** Scans up to @p wanted keys of @p collection from @p start into @p arena through @p scan_, the
 *  scan of one modality keyed by UUIDs, at the view @p receiver reads, keeping the keys @p match
 *  admits, and when @p scores is set and the patterns carry weights, fills it with one BM25 score
 *  for every key it keeps. */
template <typename scan_type_, void (*scan_)(scan_type_*)>
expected<std::span<ustore_key_t const>> scan_keys_through(receiver_t receiver, ustore_collection_id_t collection,
                                                          ustore_key_t start, std::size_t wanted, arena_t& arena,
                                                          match_t const& match = {},
                                                          float const** scores = nullptr) noexcept {
    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* keys = nullptr;
    scan_type_ arguments {.database = receiver.database,
                          .executor = receiver.executor,
                          .view = receiver.view(ustore_watch_keys_k),
                          .count = 1,
                          .collections = {&collection, 0},
                          .start_keys = {&start, 0},
                          .offsets = &offsets,
                          .keys = &keys,
                          .arena = arena.handle(),
                          .wanted = wanted};
    narrow(arguments, match);
    if constexpr (a_call_with_scores<scan_type_>) arguments.scores = scores;
    status_t const status = answered_<status_t>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        arguments.outcome = outcome;
        scan_(&arguments);
    });
    if (status.failed()) return {{}, status};
    if (!offsets) return {std::span<ustore_key_t const> {}};
    return {std::span<ustore_key_t const> {keys + offsets[0], keys + offsets[1]}};
}

/** Scans up to @p wanted keys of @p collection, which holds @p modality, from @p start into
 *  @p arena through the scan of that modality: the documents, the vertices, the pages or the
 *  keys of blobs. */
inline expected<std::span<ustore_key_t const>> scan_keys(receiver_t receiver, ustore_modality_t modality,
                                                         ustore_collection_id_t collection, ustore_key_t start,
                                                         std::size_t wanted, arena_t& arena) noexcept {
    switch (modality) {
    case ustore_modality_docs_k:
        return scan_keys_through<ustore_docs_scan_t, &ustore_docs_scan>(receiver, collection, start, wanted, arena);
    case ustore_modality_graph_k:
        return scan_keys_through<ustore_graph_scan_vertices_t, &ustore_graph_scan_vertices>(receiver, collection, start,
                                                                                            wanted, arena);
    case ustore_modality_pages_k:
        return scan_keys_through<ustore_pages_scan_t, &ustore_pages_scan>(receiver, collection, start, wanted, arena);
    default:
        return scan_keys_through<ustore_blobs_scan_t, &ustore_blobs_scan>(receiver, collection, start, wanted, arena);
    }
}

/** A stream over the keys of one collection below an end key that pulls them in pages through the
 *  scan of its modality, on the executor of its receiver, each page resuming past the last key of
 *  the one before it. It keeps the arena holding the page in hand, so it moves but never copies,
 *  and prefers @c seek to being rebuilt. As shorthand for a range-based loop, which has no place
 *  for an arena, it owns that one arena for its lifetime. A page short of the read-ahead ends the
 *  stream, and a default-constructed stream is at the end. */
class keys_stream_t {

    friend class graph_stream_t;

    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};
    ustore_modality_t modality_ {ustore_modality_blobs_k};
    ustore_key_t end_key_ {largest_key_k};

    arena_t arena_;
    std::size_t read_ahead_ {1};

    std::span<ustore_key_t const> fetched_keys_ {};
    std::size_t fetched_offset_ {0};

    keys_stream_t(receiver_t receiver, ustore_collection_id_t collection, ustore_modality_t modality,
                  ustore_key_t end_key, arena_t arena, std::size_t read_ahead) noexcept
        : receiver_(receiver), collection_(collection), modality_(modality), end_key_(end_key),
          arena_(std::move(arena)), read_ahead_(read_ahead ? read_ahead : 1) {}

    /** Pulls the page starting at @p start, rewinding the arena that held the page before it, and
     *  cuts it short at the end key, which ends the stream there. */
    status_t fetch(ustore_key_t start) noexcept {
        auto [found_keys, scan_status] = scan_keys(receiver_, modality_, collection_, start, read_ahead_, arena_);
        if (scan_status.failed()) return scan_status;
        fetched_keys_ = found_keys.first(
            static_cast<std::size_t>(std::ranges::lower_bound(found_keys, end_key_) - found_keys.begin()));
        fetched_offset_ = 0;
        return {};
    }

    /** Pulls the page past the one in hand, which is empty once that page ended at
     *  the largest key. */
    status_t prefetch() noexcept {
        if (fetched_keys_.empty() || fetched_keys_.back() == largest_key_k) {
            fetched_keys_ = {};
            fetched_offset_ = 0;
            return {};
        }
        return fetch(key_after(fetched_keys_.back()));
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = ustore_key_t;

    static constexpr std::size_t default_read_ahead_k = 256;

    keys_stream_t() noexcept = default;
    keys_stream_t(keys_stream_t&&) noexcept = default;
    keys_stream_t& operator=(keys_stream_t&&) noexcept = default;
    keys_stream_t(keys_stream_t const&) = delete;
    keys_stream_t& operator=(keys_stream_t const&) = delete;

    /** Opens a stream over the keys of @p collection, which holds @p modality, from @p start up to
     *  @p end_key, pulling @p read_ahead of them a page into an arena of its own, made where the
     *  executor of @p receiver computes. */
    static expected<keys_stream_t> make(receiver_t receiver, ustore_collection_id_t collection, ustore_key_t start,
                                        std::size_t read_ahead = default_read_ahead_k,
                                        ustore_modality_t modality = ustore_modality_blobs_k,
                                        ustore_key_t end_key = largest_key_k) noexcept {
        auto [arena, arena_status] = arena_t::make({.executor = receiver.executor});
        if (arena_status.failed()) return {{}, arena_status};
        keys_stream_t stream {receiver, collection, modality, end_key, std::move(arena), read_ahead};
        if (status_t const sought = stream.seek(start); sought.failed()) return {{}, sought};
        return {std::move(stream)};
    }

    /** Restarts the stream at @p key. */
    status_t seek(ustore_key_t key) noexcept { return fetch(key); }

    status_t advance() noexcept {
        if (fetched_offset_ + 1 < fetched_keys_.size()) ++fetched_offset_;
        else if (fetched_keys_.size() < read_ahead_) fetched_offset_ = fetched_keys_.size();
        else return prefetch();
        return {};
    }

    /** Advances like @c advance, but a failed pull ends the stream since a reference holds no
     *  status. */
    keys_stream_t& operator++() noexcept {
        if (!advance().failed()) return *this;
        fetched_keys_ = {};
        fetched_offset_ = 0;
        return *this;
    }

    ustore_key_t key() const noexcept { return fetched_keys_[fetched_offset_]; }
    ustore_key_t operator*() const noexcept { return key(); }
    status_t seek_to_first() noexcept { return seek(smallest_key_k); }

    /** Pulls the page after the one in hand, which @c keys_batch exposes. */
    status_t seek_to_next_batch() noexcept { return prefetch(); }

    /** Exposes the whole page in hand, passed keys included, for use with @c seek_to_next_batch. */
    std::span<ustore_key_t const> keys_batch() noexcept {
        fetched_offset_ = fetched_keys_.size();
        return fetched_keys_;
    }

    bool is_end() const noexcept {
        return fetched_keys_.size() < read_ahead_ && fetched_offset_ >= fetched_keys_.size();
    }

    bool operator==(keys_stream_t const& other) const noexcept {
        if (!(collection_ == other.collection_)) return false;
        if (is_end() || other.is_end()) return is_end() == other.is_end();
        return key() == other.key();
    }
};

/** One key and the value it holds, as a stream of pairs hands them out. */
struct pair_t {
    ustore_key_t key {};
    value_view_t value {};
};

/** A stream over the keys and values of one collection below an end key that pulls both in pages
 *  from a cursor, in one call per page on the executor of its receiver. As shorthand for a
 *  range-based loop, which has no place for an arena, it owns that one arena for its lifetime,
 *  where @c blobs_cursor_t takes the caller's per pull. A stream constructed by default is past
 *  its end already. */
class pairs_stream_t {

    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};
    ustore_key_t end_key_ {largest_key_k};

    blobs_cursor_t cursor_;
    arena_t arena_;
    std::size_t read_ahead_ {1};

    blobs_page_t fetched_ {};
    std::size_t fetched_offset_ {0};

    pairs_stream_t(receiver_t receiver, ustore_collection_id_t collection, ustore_key_t end_key, arena_t arena,
                   std::size_t read_ahead) noexcept
        : receiver_(receiver), collection_(collection), end_key_(end_key), arena_(std::move(arena)),
          read_ahead_(read_ahead ? read_ahead : 1) {}

    /** Pulls the next page, cut short at the end key, which ends the stream there. */
    status_t prefetch() noexcept {
        ustore_strings_view_t values {};
        auto [keys, pull_status] = cursor_.pull_(read_ahead_, arena_, receiver_.executor, &values, {});
        if (pull_status.failed()) return pull_status;
        std::size_t const kept = static_cast<std::size_t>(std::ranges::lower_bound(keys, end_key_) - keys.begin());
        fetched_ = {keys.first(kept), {values, kept}};
        fetched_offset_ = 0;
        return {};
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = pair_t;

    static constexpr std::size_t default_read_ahead_k = 256;

    pairs_stream_t() noexcept = default;
    pairs_stream_t(pairs_stream_t&&) noexcept = default;
    pairs_stream_t& operator=(pairs_stream_t&&) noexcept = default;
    pairs_stream_t(pairs_stream_t const&) = delete;
    pairs_stream_t& operator=(pairs_stream_t const&) = delete;

    /** Opens a stream over the pairs of @p collection from @p start up to @p end_key, pulling
     *  @p read_ahead of them a page into an arena of its own, made where the executor of
     *  @p receiver computes. */
    static expected<pairs_stream_t> make(receiver_t receiver, ustore_collection_id_t collection, ustore_key_t start,
                                         std::size_t read_ahead = default_read_ahead_k,
                                         ustore_key_t end_key = largest_key_k) noexcept {
        auto [arena, arena_status] = arena_t::make({.executor = receiver.executor});
        if (arena_status.failed()) return {{}, arena_status};
        pairs_stream_t stream {receiver, collection, end_key, std::move(arena), read_ahead};
        if (status_t const sought = stream.seek(start); sought.failed()) return {{}, sought};
        return {std::move(stream)};
    }

    /** Restarts the stream at @p key, through a cursor opened there. */
    status_t seek(ustore_key_t key) noexcept {
        auto [cursor, cursor_status] = blobs_cursor_t::make(receiver_, collection_, key);
        if (cursor_status.failed()) return cursor_status;
        cursor_ = std::move(cursor);
        return prefetch();
    }

    status_t advance() noexcept {
        if (fetched_offset_ + 1 < fetched_.keys.size()) ++fetched_offset_;
        else if (fetched_.keys.size() < read_ahead_) fetched_offset_ = fetched_.keys.size();
        else return prefetch();
        return {};
    }

    /** Advances like @c advance, but a failed pull ends the stream since a reference holds no
     *  status. */
    pairs_stream_t& operator++() noexcept {
        if (!advance().failed()) return *this;
        fetched_ = {};
        fetched_offset_ = 0;
        return *this;
    }

    ustore_key_t key() const noexcept { return fetched_.keys[fetched_offset_]; }
    value_view_t value() const noexcept { return fetched_.values[fetched_offset_]; }
    pair_t pair() const noexcept { return {key(), value()}; }
    pair_t operator*() const noexcept { return pair(); }

    status_t seek_to_first() noexcept { return seek(smallest_key_k); }

    /** Pulls the page after the one in hand, which @c keys_batch exposes. */
    status_t seek_to_next_batch() noexcept { return prefetch(); }

    /** Exposes the keys of the whole page in hand, passed keys included, for use with @c
     *  seek_to_next_batch. */
    std::span<ustore_key_t const> keys_batch() noexcept {
        fetched_offset_ = fetched_.keys.size();
        return fetched_.keys;
    }

    bool is_end() const noexcept {
        return fetched_.keys.size() < read_ahead_ && fetched_offset_ >= fetched_.keys.size();
    }

    bool operator==(pairs_stream_t const& other) const noexcept {
        if (!(collection_ == other.collection_)) return false;
        if (is_end() || other.is_end()) return is_end() == other.is_end();
        return key() == other.key();
    }
};

/**
 *  @brief One pass of a range-based loop over the stream it owns, which pulls its pages on the
 *      executor of its receiver into the one arena it owns.
 *
 *  The first failed open or pull ends the pass, and @c status reads it after the loop. Its
 *  iterators point back into it, so it stays where the loop found it.
 */
template <typename stream_type_>
class stream_range {
    stream_type_ stream_ {};
    status_t status_ {};

    void advance_() noexcept {
        if (status_t const advanced = stream_.advance(); advanced.failed()) {
            status_ = advanced;
            stream_ = stream_type_ {};
        }
    }

  public:
    /** Takes over @p opened, or keeps the status explaining why it failed to open. */
    explicit stream_range(expected<stream_type_> opened) noexcept
        : stream_(std::move(opened.value)), status_(opened.status) {}

    class iterator {
        stream_range* range_ = nullptr;

      public:
        using value_type = typename stream_type_::value_type;
        using difference_type = std::ptrdiff_t;

        iterator() noexcept = default;
        explicit iterator(stream_range& range) noexcept : range_(&range) {}

        value_type operator*() const noexcept { return *range_->stream_; }
        iterator& operator++() noexcept {
            range_->advance_();
            return *this;
        }
        void operator++(int) noexcept { range_->advance_(); }
        bool operator==(std::default_sentinel_t) const noexcept { return range_->stream_.is_end(); }
    };

    iterator begin() noexcept { return iterator {*this}; }
    std::default_sentinel_t end() const noexcept { return {}; }

    /** Why the pass ended early, or success once it walked everything. */
    status_t status() const noexcept { return status_; }

    /** Counts what the pass yields, refusing what an open or a pull refused. */
    expected<std::size_t> size() noexcept {
        std::size_t count = 0;
        for (iterator position = begin(); position != end(); ++position) ++count;
        if (status_.failed()) return {{}, status_};
        return {count};
    }
};

/** The keys of a collection slice, walked once by a range-based loop. */
using keys_range_t = stream_range<keys_stream_t>;

/** The key-value pairs of a blobs collection slice, walked once by a range-based loop. */
using pairs_range_t = stream_range<pairs_stream_t>;

static_assert(std::input_iterator<keys_range_t::iterator>);
static_assert(std::sentinel_for<std::default_sentinel_t, keys_range_t::iterator>);

#pragma endregion Streams

#pragma region Samples and Measures

/** Lower and upper bounds of an estimate. */
struct size_range_t {
    std::size_t min = 0;
    std::size_t max = 0;
};

/** Estimates of how many entries a key range holds and how much space they take. */
struct size_estimates_t {
    size_range_t cardinality;
    size_range_t bytes_in_values;
    size_range_t bytes_on_disk;
};

/** Where a sample writes the keys of its one task. */
template <>
struct outputs<std::span<ustore_key_t const>> {

    /** Output for two offsets into @c keys, inside the arena. */
    ustore_length_t const* offsets = nullptr;

    /** Output for the keys drawn, inside the arena. */
    ustore_key_t const* keys = nullptr;

    std::span<ustore_key_t const> answer() const noexcept {
        if (!offsets) return {};
        return {keys + offsets[0], keys + offsets[1]};
    }
};

/** Where a measure writes the bounds of its one range. */
template <>
struct outputs<size_estimates_t> {

    /** Outputs for the bounds on the entries of the range, inside the arena. */
    ustore_size_t const* min_cardinalities = nullptr;
    ustore_size_t const* max_cardinalities = nullptr;

    /** Outputs for the bounds on the bytes of the values of the range, inside the arena. */
    ustore_size_t const* min_value_bytes = nullptr;
    ustore_size_t const* max_value_bytes = nullptr;

    /** Outputs for the bounds on the disk space the range takes, inside the arena. */
    ustore_size_t const* min_space_usages = nullptr;
    ustore_size_t const* max_space_usages = nullptr;

    size_estimates_t answer() const noexcept {
        return {{*min_cardinalities, *max_cardinalities},
                {*min_value_bytes, *max_value_bytes},
                {*min_space_usages, *max_space_usages}};
    }
};

/**
 *  @brief Draws up to @p wanted keys of @p collection at random into @p arena through
 *      @p sample_, the sample of one modality keyed by UUIDs.
 *  @param[in] collection Collection to draw from, owned by the caller until the call is final.
 */
template <ustore_completion_t completion_, typename sample_type_, void (*sample_)(sample_type_*)>
deferrable<std::span<ustore_key_t const>, completion_> sample_keys_(receiver_t const& receiver,
                                                                    ustore_collection_id_t const& collection,
                                                                    std::size_t wanted, arena_t& arena,
                                                                    ustore_watch_t watch) noexcept {
    using answer_t = std::span<ustore_key_t const>;
    return answered_<deferrable<answer_t, completion_>>(
        [&](ustore_outcome_t* outcome, outputs<answer_t>* written) noexcept {
            sample_type_ arguments {.database = receiver.database,
                                    .executor = receiver.executor,
                                    .outcome = outcome,
                                    .completion = completion_,
                                    .view = receiver.view(watch),
                                    .count = 1,
                                    .collections = {&collection, 0},
                                    .offsets = &written->offsets,
                                    .keys = &written->keys,
                                    .arena = arena.handle(),
                                    .wanted = wanted};
            sample_(&arguments);
        });
}

/**
 *  @brief Estimates the entries and the space between @p start_key, inclusive, and @p end_key,
 *      exclusive, of @p collection into @p arena through @p measure_, the measure of one modality
 *      keyed by UUIDs.
 *  @param[in] collection Collection to measure, owned by the caller until the call is final.
 *  @param[in] start_key First key of the range, owned by the caller until the call is final.
 *  @param[in] end_key Key the range stops before, owned by the caller until the call is final.
 */
template <ustore_completion_t completion_, typename measure_type_, void (*measure_)(measure_type_*)>
deferrable<size_estimates_t, completion_> measure_keys_(receiver_t const& receiver,
                                                        ustore_collection_id_t const& collection,
                                                        ustore_key_t const& start_key, ustore_key_t const& end_key,
                                                        arena_t& arena, ustore_watch_t watch) noexcept {
    return answered_<deferrable<size_estimates_t, completion_>>(
        [&](ustore_outcome_t* outcome, outputs<size_estimates_t>* written) noexcept {
            measure_type_ arguments {.database = receiver.database,
                                     .executor = receiver.executor,
                                     .outcome = outcome,
                                     .completion = completion_,
                                     .view = receiver.view(watch),
                                     .count = 1,
                                     .collections = {&collection, 0},
                                     .start_keys = {&start_key, 0},
                                     .end_keys = {&end_key, 0},
                                     .min_cardinalities = &written->min_cardinalities,
                                     .max_cardinalities = &written->max_cardinalities,
                                     .min_value_bytes = &written->min_value_bytes,
                                     .max_value_bytes = &written->max_value_bytes,
                                     .min_space_usages = &written->min_space_usages,
                                     .max_space_usages = &written->max_space_usages,
                                     .arena = arena.handle()};
            measure_(&arguments);
        });
}

/** Estimates the entries of the whole key range of @p collection through @p measure_ and a one-off
 *  arena, as shorthand with no place for an arena does. */
template <typename measure_type_, void (*measure_)(measure_type_*)>
expected<size_range_t> measured_size_range_(receiver_t const& receiver,
                                            ustore_collection_id_t const& collection) noexcept {
    auto [arena, arena_status] = arena_t::make({.executor = receiver.executor});
    if (arena_status.failed()) return {{}, arena_status};
    auto [estimates, measure_status] = measure_keys_<ustore_completion_blocking_k, measure_type_, measure_>(
        receiver, collection, smallest_key_k, largest_key_k, arena, ustore_watch_keys_k);
    if (measure_status.failed()) return {{}, measure_status};
    return {estimates.cardinality};
}

#pragma endregion Samples and Measures

#pragma region Ranges

/** A slice of keys in one collection, which streams its keys through the scan of the collection's
 *  @c modality, and, in a blobs collection, streams its pairs and estimates its size. */
class blobs_range_t {

    receiver_t receiver_;
    ustore_collection_id_t collection_;
    ustore_key_t start_key_;
    ustore_key_t end_key_;
    ustore_modality_t modality_;

  public:
    blobs_range_t(receiver_t receiver, ustore_collection_id_t collection, ustore_key_t start_key = smallest_key_k,
                  ustore_key_t end_key = largest_key_k, ustore_modality_t modality = ustore_modality_blobs_k) noexcept
        : receiver_(receiver), collection_(collection), start_key_(start_key), end_key_(end_key), modality_(modality) {}

    receiver_t receiver() const noexcept { return receiver_; }
    ustore_collection_id_t collection() const noexcept { return collection_; }

    /** The keys of the slice, pulled on the receiver's executor. */
    keys_range_t keys() const noexcept {
        return keys_range_t {keys_stream_t::make(receiver_, collection_, start_key_,
                                                 keys_stream_t::default_read_ahead_k, modality_, end_key_)};
    }

    /** The pairs of the slice of a blobs collection, pulled on the receiver's executor. */
    pairs_range_t pairs() const noexcept {
        return pairs_range_t {
            pairs_stream_t::make(receiver_, collection_, start_key_, pairs_stream_t::default_read_ahead_k, end_key_)};
    }

    /** Estimates the size of the slice through a one-off arena, since a single range answers a
     *  fixed-size report. */
    expected<size_estimates_t> size_estimates() noexcept {
        auto [arena, arena_status] = arena_t::make({.executor = receiver_.executor});
        if (arena_status.failed()) return {{}, arena_status};
        return measure_keys_<ustore_completion_blocking_k, ustore_blobs_measure_t, &ustore_blobs_measure>(
            receiver_, collection_, start_key_, end_key_, arena, ustore_watch_keys_k);
    }

    blobs_range_t& start_key(ustore_key_t key) noexcept {
        start_key_ = key;
        return *this;
    }

    blobs_range_t& end_key(ustore_key_t key) noexcept {
        end_key_ = key;
        return *this;
    }

    ustore_key_t start_key() const noexcept { return start_key_; }
    ustore_key_t end_key() const noexcept { return end_key_; }
};

#pragma endregion Ranges

#pragma region Collections

/**
 *  @brief A handle to one binary collection, cheap to construct and to copy, which addresses
 *      the head or the view of a transaction or snapshot, and whose verbs answer as
 *      @p completion_ says.
 *
 *  A blocking handle answers a @c status_t or an @c expected, while a deferred one answers a
 *  @c deferred call, viewing the handle, its inputs and its arena until the call is final. A handle
 *  with @c collection_access_t::read_only_k only reads, as a snapshot hands out. It holds no state
 *  of its own, so any number of threads share it, and it never outlives its database.
 */
template <ustore_completion_t completion_, collection_access_t access_>
class blobs_collection_on {
    template <typename value_type_ = void>
    using deferrable = ustore::deferrable<value_type_, completion_>;
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};
    static constexpr bool writes_k = access_ == collection_access_t::read_write_k;

    /** A proxy over one location inside this collection, whose own collection wins over the one it
     *  names. */
    template <typename key_argument_type_>
        requires(!a_batch<key_argument_type_>)
    blobs_ref<collection_key_field_t, access_> address_(key_argument_type_ const& key) const noexcept {
        return {receiver_, location_in(collection_, key)};
    }

    /** A proxy over a batch of locations inside this collection. */
    template <a_batch keys_argument_type_>
    blobs_ref<locations_in_collection<keys_argument_type_>, access_> address_(
        keys_argument_type_&& keys) const noexcept {
        using locations_t = locations_in_collection<keys_argument_type_>;
        return {receiver_, locations_t {std::forward<keys_argument_type_>(keys), collection_}};
    }

    /** The proxy @c at answers: over a batch of locations, or over one. */
    template <typename keys_argument_type_>
    using ref_of = std::conditional_t<a_batch<keys_argument_type_>,
                                      blobs_ref<locations_in_collection<keys_argument_type_>, access_>,
                                      blobs_ref<collection_key_field_t, access_>>;

  public:
    /** The handle a snapshot hands out for this kind of collection, which only reads. */
    using read_only_t = blobs_collection_on<completion_, collection_access_t::read_only_k>;

    /** The same kind of handle, completing its verbs as @p other_completion_ says. */
    template <ustore_completion_t other_completion_>
    using completed_as = blobs_collection_on<other_completion_, access_>;

    /** What the collections this kind of handle addresses hold, which a creation records and a
     *  lookup checks. */
    static constexpr ustore_modality_t modality_k = ustore_modality_blobs_k;

    blobs_collection_on() noexcept = default;
    blobs_collection_on(receiver_t receiver, ustore_collection_id_t collection) noexcept
        : receiver_(receiver), collection_(collection) {}

    ustore_collection_id_t handle() const noexcept { return collection_; }
    receiver_t receiver() const noexcept { return receiver_; }

    /** The same collection, whose verbs block on @p executor, or on the calling
     *  thread without one. */
    completed_as<ustore_completion_blocking_k> on(executor_t const& executor) const noexcept {
        return {receiver_.on(executor.handle()), collection_};
    }

    /** The same collection, whose verbs submit to @p executor and answer deferred calls. */
    completed_as<ustore_completion_deferred_k> deferred(executor_t const& executor) const noexcept {
        return {receiver_.on(executor.handle()), collection_};
    }

#pragma region Verbs

    /**
     *  @brief Reads the values of @p keys into @p arena, the verb that reuses one arena
     *      call after call.
     *  @param[in] keys Keys to read, owned by the caller until the call is final.
     *  @param[in] arena Memory the values land in, owned by the caller until the call is final,
     *      which the answer views.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     */
    deferrable<values_view_t> read(keys_view_t keys, arena_t& arena,
                                   ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return blobs_read_<completion_, values_view_t>(receiver_, places_in(collection_, keys), arena, watch);
    }

    /**
     *  @brief Writes @p values to @p keys pairwise.
     *  @param[in] keys Keys to write, owned by the caller until the call is final.
     *  @param[in] values One value per key, owned by the caller until the call is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The @p keys and @p values differ in count.
     */
    template <a_strings_batch values_type_>
    deferrable<> write(keys_view_t keys, values_type_ const& values,
                       ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        // The call has one count, so only this side sees the keys and the values disagree
        if (keys.size() != std::size(values))
            return answered_<deferrable<>>({status_code_t::invalid_argument_k, "Every key needs its own value"});
        return blobs_write_<completion_>(receiver_, places_in(collection_, keys), strings_view_of(values), durability);
    }

    /**
     *  @brief Removes @p keys together with their values.
     *  @param[in] keys Keys to remove, owned by the caller until the call is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    deferrable<> remove(keys_view_t keys, ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        return blobs_remove_<completion_>(receiver_, places_in(collection_, keys), durability);
    }

    /** Draws up to @p wanted keys at random into @p arena, viewed until the call is final. */
    deferrable<std::span<ustore_key_t const>> sample(std::size_t wanted, arena_t& arena,
                                                     ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return sample_keys_<completion_, ustore_blobs_sample_t, &ustore_blobs_sample>(receiver_, collection_, wanted,
                                                                                      arena, watch);
    }

    /** Removes every value and keeps the keys. */
    deferrable<> clear_values() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_values_k);
    }

    /** Removes every key and value and keeps the collection. */
    deferrable<> clear() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_pairs_k);
    }

    /** Removes the collection with its name and every pair it holds. */
    deferrable<> drop() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_collection_k);
    }

#pragma endregion Verbs

#pragma region Shorthand

    blobs_range_t members(ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) const noexcept
        requires blocks<completion_>
    {
        return {receiver_, collection_, start_key, end_key};
    }

    keys_range_t keys(ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) const noexcept
        requires blocks<completion_>
    {
        return members(start_key, end_key).keys();
    }

    pairs_range_t pairs(ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) const noexcept
        requires blocks<completion_>
    {
        return members(start_key, end_key).pairs();
    }

    /** The bounds on the keys the engine estimates over the whole key range, through a one-off
     *  arena. */
    expected<size_range_t> size_range() const noexcept
        requires blocks<completion_>
    {
        return measured_size_range_<ustore_blobs_measure_t, &ustore_blobs_measure>(receiver_, collection_);
    }

    /** Counts the keys exactly by streaming through all of them. */
    expected<std::size_t> size() const noexcept
        requires blocks<completion_>
    {
        return keys().size();
    }

    blobs_ref<place_batch_t, access_> operator[](std::initializer_list<ustore_key_t> keys) const noexcept
        requires blocks<completion_>
    {
        return at(keys);
    }

    blobs_ref<place_batch_t, access_> at(std::initializer_list<ustore_key_t> keys) const noexcept
        requires blocks<completion_>
    {
        return at(keys_view_t {strided(keys)});
    }

    blobs_ref<place_batch_t, access_> operator[](keys_view_t keys) const noexcept
        requires blocks<completion_>
    {
        return at(keys);
    }

    blobs_ref<place_batch_t, access_> at(keys_view_t keys) const noexcept
        requires blocks<completion_>
    {
        return {receiver_, places_in(collection_, keys)};
    }

    template <typename keys_argument_type_>
    ref_of<keys_argument_type_> operator[](keys_argument_type_&& keys) const noexcept
        requires blocks<completion_>
    {
        return at(std::forward<keys_argument_type_>(keys));
    }

    /** Addresses one location or a batch of them inside this collection, whose
     *  own collection wins. */
    template <typename keys_argument_type_>
    ref_of<keys_argument_type_> at(keys_argument_type_&& keys) const noexcept
        requires blocks<completion_>
    {
        return address_(std::forward<keys_argument_type_>(keys));
    }

#pragma endregion Shorthand
};

/** A binary collection whose verbs block, reading and writing the head or a transaction. */
using blobs_collection_t = blobs_collection_on<ustore_completion_blocking_k, collection_access_t::read_write_k>;

/** A binary collection whose verbs block and only read, as a snapshot hands out. */
using blobs_read_collection_t = blobs_collection_on<ustore_completion_blocking_k, collection_access_t::read_only_k>;

#pragma endregion Collections

} // namespace unum::ustore
