/**
 *  @file cpp/ustore/strings_collection.hpp
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief Handles to one collection keyed by variable-length strings, with ordered prefix walks,
 *      whose verbs block or answer deferred calls as their receiver chose.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::ptrdiff_t`

#include <iterator>    // `std::forward_iterator_tag`, `std::size`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`, `std::swap`, `std::exchange`

#include "ustore/blobs_range.hpp" // `match_t`, `narrow`, `size_estimates_t`, `measure_outputs_`
#include "ustore/ranges.hpp"      // `strings_of`, `range`, `keys_view_t`, `bits_view_t`, `strings_batch_concept`
#include "ustore/status.hpp"      // `status_t`, `expected`, `answer`, `called_`, `refused_`, `defers`
#include "ustore/strings.h"
#include "ustore/types.hpp" // `arena_t`, `receiver_t`, `value_view_t`, `collection_access_t`, `collection_drop_`

namespace unum::ustore {

#pragma region Verbs

/** What a scan hands over: the strings every prefix found, page after page, inside the arena. */
struct strings_pages_t {

    /** Every string found, in the order of the prefixes and then of their text. */
    sz::strings_view_t strings {strings_of({}, 0)};

    /** Where the page of every prefix starts among @c strings, with one more closing
     *  the last page. */
    std::span<ustore_length_t const> pages;

    /** One BM25 score per string when the patterns carry weights, or @c nullptr. */
    float const* scores = nullptr;
};

/** Where a scan writes its pages, their strings and their scores. */
template <>
struct outputs<strings_pages_t> {

    /** Prefixes scanned, the pages of the answer. */
    ustore_size_t count = 0;

    /** Output for the offsets of the pages, inside the arena. */
    ustore_length_t const* pages = nullptr;

    /** Output for the tape of strings, inside the arena. */
    ustore_strings_view_t strings {};

    /** Output for one score per string, inside the arena. */
    float const* scores = nullptr;

    strings_pages_t answer() const noexcept {
        if (!pages) return {};
        return {strings_of(tape_slice(strings, pages[0]), pages[count] - pages[0]), {pages, count + 1}, scores};
    }
};

/** Where a strings read writes presence bits: nowhere, unless it answers them. */
template <typename answer_type_>
ustore_octet_t const** strings_presences_output_(outputs<answer_type_>&) noexcept {
    return nullptr;
}

inline ustore_octet_t const** strings_presences_output_(outputs<bits_view_t>& written) noexcept {
    return &written.presences;
}

/** Where a strings read writes keys: nowhere, unless it answers them. */
template <typename answer_type_>
ustore_key_t const** strings_keys_output_(outputs<answer_type_>&) noexcept {
    return nullptr;
}

inline ustore_key_t const** strings_keys_output_(outputs<keys_view_t>& written) noexcept { return &written.keys; }

/**
 *  @brief Reads what @p answer_type_ asks of the @p count strings @p texts lends: the key each maps
 *      onto, the nil UUID for a missing one, or whether each is present.
 *  @param[in] collection Collection every string lives in, borrowed until the call is final.
 *  @param[in] texts Strings to read, borrowed until the call is final.
 *  @param[in] arena Memory the answer lands in, borrowed until the call is final, which
 *      the answer views.
 */
template <ustore_completion_t completion_, typename answer_type_>
answer<completion_, answer_type_> strings_read_(receiver_t const& receiver, ustore_collection_id_t const& collection,
                                                ustore_strings_view_t texts, std::size_t count, arena_t& arena,
                                                ustore_watch_t watch) noexcept {
    return called_<completion_, answer_type_>([&](ustore_outcome_t* outcome, outputs<answer_type_>* written) noexcept {
        written->count = count;
        ustore_strings_read_t read {};
        read.database = receiver.database;
        read.executor = receiver.executor;
        read.outcome = outcome;
        read.completion = completion_;
        read.view = receiver.view(watch);
        read.count = count;
        read.collections = {&collection, 0};
        read.strings = texts;
        read.presences = strings_presences_output_(*written);
        read.values = strings_keys_output_(*written);
        read.arena = arena;
        ustore_strings_read(&read);
    });
}

/** Upserts the @p count keys @p keys lends onto the strings @p texts lends, pairwise. */
template <ustore_completion_t completion_>
answer<completion_, void> strings_write_(receiver_t const& receiver, ustore_collection_id_t const& collection,
                                         ustore_strings_view_t texts, ustore_key_t const* keys, std::size_t count,
                                         ustore_durability_t durability) noexcept {
    return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_strings_write_t write {};
        write.database = receiver.database;
        write.executor = receiver.executor;
        write.outcome = outcome;
        write.completion = completion_;
        write.transaction = receiver.transaction;
        write.count = count;
        write.collections = {&collection, 0};
        write.strings = texts;
        write.values = {keys, sizeof(ustore_key_t)};
        write.durability = durability;
        ustore_strings_write(&write);
    });
}

/** Removes the @p count strings @p texts lends together with their keys. */
template <ustore_completion_t completion_>
answer<completion_, void> strings_remove_(receiver_t const& receiver, ustore_collection_id_t const& collection,
                                          ustore_strings_view_t texts, std::size_t count,
                                          ustore_durability_t durability) noexcept {
    return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_strings_remove_t remove {};
        remove.database = receiver.database;
        remove.executor = receiver.executor;
        remove.outcome = outcome;
        remove.completion = completion_;
        remove.transaction = receiver.transaction;
        remove.count = count;
        remove.collections = {&collection, 0};
        remove.strings = texts;
        remove.durability = durability;
        ustore_strings_remove(&remove);
    });
}

/**
 *  @brief Lists up to @p limit strings of each of the @p count prefixes @p prefixes lends into @p
 *      arena, in the order of their text, each page from its start in @p starts, keeping the
 *      strings @p match admits.
 *  @param[in] starts None to start every page at its prefix, or the smallest string of every page,
 *      which the page holds too when the collection does, borrowed until the call is final.
 */
template <ustore_completion_t completion_>
answer<completion_, strings_pages_t> strings_scan_(receiver_t const& receiver, ustore_collection_id_t const& collection,
                                                   ustore_strings_view_t prefixes, ustore_strings_view_t starts,
                                                   std::size_t count, std::size_t limit, arena_t& arena,
                                                   match_t const& match, ustore_watch_t watch) noexcept {
    return called_<completion_, strings_pages_t>(
        [&](ustore_outcome_t* outcome, outputs<strings_pages_t>* written) noexcept {
            written->count = count;
            ustore_strings_scan_t scan {};
            scan.database = receiver.database;
            scan.executor = receiver.executor;
            scan.outcome = outcome;
            scan.completion = completion_;
            scan.view = receiver.view(watch);
            scan.count = count;
            scan.collections = {&collection, 0};
            scan.prefixes = prefixes;
            scan.start_strings = starts;
            scan.offsets = &written->pages;
            scan.strings = &written->strings;
            scan.scores = &written->scores;
            scan.arena = arena;
            scan.wanted = limit;
            narrow(scan, match);
            ustore_strings_scan(&scan);
        });
}

#pragma endregion Verbs

#pragma region Streams

/**
 *  @brief Owns a walk over the strings of one prefix from its open until its destruction, which
 *      every pull resumes past the last string the pull before it handed out, at the view its
 *      receiver read at the open.
 *
 *  The explicit form of @c strings_stream_t: every pull takes the caller's arena, which
 *  holds the page until its next call. The cursor copies its prefix and start, and moves but
 *  never copies itself.
 */
class strings_cursor_t {

    receiver_t receiver_ {};
    ustore_strings_cursor_t cursor_ = nullptr;

    strings_cursor_t(receiver_t receiver, ustore_strings_cursor_t cursor) noexcept
        : receiver_(receiver), cursor_(cursor) {}

  public:
    strings_cursor_t() noexcept = default;
    strings_cursor_t(strings_cursor_t const&) = delete;
    strings_cursor_t& operator=(strings_cursor_t const&) = delete;
    ~strings_cursor_t() noexcept { ustore_strings_cursor_free(cursor_); }

    strings_cursor_t(strings_cursor_t&& other) noexcept
        : receiver_(other.receiver_), cursor_(std::exchange(other.cursor_, nullptr)) {}

    strings_cursor_t& operator=(strings_cursor_t&& other) noexcept {
        std::swap(receiver_, other.receiver_);
        std::swap(cursor_, other.cursor_);
        return *this;
    }

    /** Opens a walk over the strings of @p prefix in @p collection from @p start, at the view @p
     *  receiver reads, keeping the strings @p match admits, compiled once here. */
    static expected<strings_cursor_t> make(receiver_t receiver, ustore_collection_id_t collection,
                                           value_view_t prefix = value_view_t::make_empty(),
                                           value_view_t start = value_view_t::make_empty(),
                                           match_t const& match = {}) noexcept {
        ustore_strings_cursor_t cursor = nullptr;
        ustore_outcome_t outcome {};
        ustore_strings_cursor_init_t init {.cursor = &cursor,
                                           .database = receiver.database,
                                           .executor = receiver.executor,
                                           .outcome = &outcome,
                                           .view = receiver.view(ustore_watch_keys_k),
                                           .collection = collection,
                                           .prefix = prefix.bytes(),
                                           .start_string = start.bytes(),
                                           .patterns = match.patterns,
                                           .predicate = match.predicate};
        ustore_strings_cursor_init(&init);
        if (status_t const status {outcome}; status.failed()) return status;
        return strings_cursor_t {receiver, cursor};
    }

    /** Pulls up to @p wanted strings into @p arena in the order of their text, fewer once the walk
     *  nears its end and none past it. */
    expected<sz::strings_view_t> next(std::size_t wanted, arena_t& arena) noexcept {
        ustore_strings_view_t strings {};
        ustore_size_t written = 0;
        ustore_outcome_t outcome {};
        ustore_strings_cursor_next_t next {};
        next.database = receiver_.database;
        next.executor = receiver_.executor;
        next.outcome = &outcome;
        next.cursor = cursor_;
        next.strings = &strings;
        next.written = &written;
        next.arena = arena;
        next.wanted = wanted;
        ustore_strings_cursor_next(&next);
        if (status_t const status {outcome}; status.failed()) return status;
        return strings_of(strings, written);
    }
};

/**
 *  @brief A stream over the strings of one prefix that pulls them in pages from a cursor, in the
 *      order of their text.
 *
 *  As shorthand for a range-based loop, which has no place for an arena, it owns one arena for its
 *  lifetime, where @c strings_cursor_t takes the caller's per pull. The prefix it walks is
 *  borrowed, so it outlives the stream, a page short of the read-ahead ends the walk, and a
 *  default-constructed stream is the end of every walk.
 */
class strings_stream_t {

    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};
    value_view_t prefix_ {value_view_t::make_empty()};

    strings_cursor_t cursor_;
    arena_t arena_;
    std::size_t read_ahead_ {1};

    sz::strings_view_t fetched_ {strings_of({}, 0)};
    std::size_t fetched_offset_ {0};

    strings_stream_t(receiver_t receiver, ustore_collection_id_t collection, value_view_t prefix, arena_t arena,
                     std::size_t read_ahead) noexcept
        : receiver_(receiver), collection_(collection), prefix_(prefix), arena_(std::move(arena)),
          read_ahead_(read_ahead ? read_ahead : 1) {}

    status_t prefetch() noexcept {
        expected<sz::strings_view_t> const page = cursor_.next(read_ahead_, arena_);
        if (page.failed()) return page.status();
        fetched_ = *page;
        fetched_offset_ = 0;
        return {};
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = std::string_view;

    /** Strings one page of a walk pulls. */
    static constexpr std::size_t default_read_ahead_k = 256;

    strings_stream_t() noexcept = default;
    strings_stream_t(strings_stream_t&&) noexcept = default;
    strings_stream_t& operator=(strings_stream_t&&) noexcept = default;
    strings_stream_t(strings_stream_t const&) = delete;
    strings_stream_t& operator=(strings_stream_t const&) = delete;

    /** Opens a walk over the strings of @p prefix in @p collection, pulling @p read_ahead of them a
     *  page into an arena made where the executor of @p receiver computes. */
    static expected<strings_stream_t> make(receiver_t receiver, ustore_collection_id_t collection, value_view_t prefix,
                                           std::size_t read_ahead = default_read_ahead_k) noexcept {
        auto [arena, arena_status] = arena_t::make(receiver.executor);
        if (arena_status.failed()) return arena_status;
        strings_stream_t stream {receiver, collection, prefix, std::move(arena), read_ahead};
        if (status_t const sought = stream.seek_to_first(); sought.failed()) return sought;
        return stream;
    }

    /** Restarts the walk at @p start, which is returned too when the collection holds it, through a
     *  cursor opened there. */
    status_t seek(value_view_t start) noexcept {
        auto [cursor, cursor_status] = strings_cursor_t::make(receiver_, collection_, prefix_, start);
        if (cursor_status.failed()) return cursor_status;
        cursor_ = std::move(cursor);
        return prefetch();
    }

    /** Restarts the walk at the first string of the prefix. */
    status_t seek_to_first() noexcept { return seek(prefix_); }

    status_t advance() noexcept {
        if (fetched_offset_ + 1 < fetched_.size()) ++fetched_offset_;
        else if (fetched_.size() < read_ahead_) fetched_offset_ = fetched_.size();
        else return prefetch();
        return {};
    }

    /** Advances like @c advance, but a failed pull ends the walk since a reference carries no
     *  status. */
    strings_stream_t& operator++() noexcept {
        if (advance().succeeded()) return *this;
        fetched_ = strings_of({}, 0);
        fetched_offset_ = 0;
        return *this;
    }

    std::string_view string() const noexcept { return fetched_[fetched_offset_]; }
    std::string_view operator*() const noexcept { return string(); }

    /** The whole page in hand, passed strings included, which the next @c advance leaves behind. */
    sz::strings_view_t strings_batch() noexcept {
        fetched_offset_ = fetched_.size();
        return fetched_;
    }

    bool is_end() const noexcept { return fetched_.size() < read_ahead_ && fetched_offset_ >= fetched_.size(); }

    bool operator==(strings_stream_t const& other) const noexcept {
        if (is_end() || other.is_end()) return is_end() == other.is_end();
        if (collection_ != other.collection_) return false;
        return string() == other.string();
    }
};

#pragma endregion Streams

/**
 *  @brief A handle to one collection mapping variable-length strings onto keys, cheap to construct
 *      and to copy, whose verbs answer as @p completion_ says.
 *
 *  It addresses the head or the view of a transaction or snapshot, holds no state of its own, so
 *  any number of threads share it, and never outlives its database. A blocking handle answers an
 *  @c outcome, while a deferred one answers a
 *
 *  @c deferred call, borrowing the handle, its inputs and its arena until the call is final. A
 *  handle with @c collection_access_t::read_only_k only reads, as a snapshot hands out. Strings
 *  carry explicit lengths, so a zero byte inside one is stored like any other, and the empty string
 *  is an ordinary key.
 */
template <ustore_completion_t completion_, collection_access_t access_>
class strings_collection_on {
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};

    static constexpr bool blocks_k = !defers<completion_>;
    static constexpr bool writes_k = access_ == collection_access_t::read_write_k;

  public:
    using prefix_range_t = range<strings_stream_t>;

    /** The handle a snapshot hands out for this kind of collection, which only reads. */
    using read_only_t = strings_collection_on<completion_, collection_access_t::read_only_k>;

    /** The same kind of handle, completing its verbs as @p other_completion_ says. */
    template <ustore_completion_t other_completion_>
    using completed_as = strings_collection_on<other_completion_, access_>;

    /** What the collections this kind of handle addresses hold, which a creation records and a
     *  lookup checks. */
    static constexpr ustore_modality_t modality_k = ustore_modality_strings_k;

    strings_collection_on() noexcept = default;
    strings_collection_on(receiver_t receiver, ustore_collection_id_t collection) noexcept
        : receiver_(receiver), collection_(collection) {}

    operator ustore_collection_id_t() const noexcept { return collection_; }
    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }
    receiver_t receiver() const noexcept { return receiver_; }

    /** The same collection, whose verbs block on @p executor, or on the calling
     *  thread without one. */
    completed_as<ustore_completion_blocking_k> on(ustore_executor_t executor) const noexcept {
        return {receiver_.on(executor), collection_};
    }

    /** The same collection, whose verbs submit to @p executor and answer deferred calls. */
    completed_as<ustore_completion_deferred_k> deferred(ustore_executor_t executor) const noexcept {
        return {receiver_.on(executor), collection_};
    }

#pragma region Verbs

    /**
     *  @brief Reads the keys @p texts map onto into @p arena, where a missing string reads as
     *      the nil UUID.
     *  @param[in] texts Strings to read, borrowed until the call is final.
     *  @param[in] arena Memory the keys land in, borrowed until the call is final, which
     *      the answer views.
     *  @param[in] watch Whether a read inside a transaction records the strings for conflicts.
     */
    template <strings_batch_concept texts_type_>
    answer<completion_, keys_view_t> read(texts_type_ const& texts, arena_t& arena,
                                          ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return strings_read_<completion_, keys_view_t>(receiver_, collection_, strings_view_of(texts), std::size(texts),
                                                       arena, watch);
    }

    /**
     *  @brief Upserts the keys of @p keys onto the strings of @p texts pairwise.
     *  @param[in] texts Strings to write, borrowed until the call is final.
     *  @param[in] keys One key per string, borrowed until the call is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The @p texts and @p keys differ in count.
     */
    template <strings_batch_concept texts_type_>
    answer<completion_, void> write(texts_type_ const& texts, std::span<ustore_key_t const> keys,
                                    ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        // The call has one count, so only this side sees the strings and the keys disagree
        if (std::size(texts) != keys.size())
            return refused_<completion_, void>({status_code_t::invalid_argument_k, "Every string needs its own key"});
        return strings_write_<completion_>(receiver_, collection_, strings_view_of(texts), keys.data(), keys.size(),
                                           durability);
    }

    /**
     *  @brief Removes the strings of @p texts together with their keys.
     *  @param[in] texts Strings to remove, borrowed until the call is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    template <strings_batch_concept texts_type_>
    answer<completion_, void> remove(texts_type_ const& texts,
                                     ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        return strings_remove_<completion_>(receiver_, collection_, strings_view_of(texts), std::size(texts),
                                            durability);
    }

    /**
     *  @brief Lists up to @p limit strings of every prefix of @p prefixes into @p arena, in the
     *      order of their text.
     *  @param[in] prefixes Prefixes to walk, a page each, borrowed until the call is final.
     *  @param[in] limit Most strings a page holds.
     *  @param[in] arena Memory the strings land in, borrowed until the call is final, which
     *      the answer views.
     *  @param[in] starts None to start every page at its prefix, or the smallest string of
     *      every page, which the page holds too when the collection does, borrowed until the
     *      call is final.
     *  @param[in] match What the strings are narrowed by, borrowed until the call is final.
     *  @param[in] watch Whether a read inside a transaction records the strings for conflicts.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The @p starts are neither empty nor one per prefix.
     */
    template <strings_batch_concept prefixes_type_, strings_batch_concept starts_type_ = values_tape_t>
    answer<completion_, strings_pages_t> scan(prefixes_type_ const& prefixes, std::size_t limit, arena_t& arena,
                                              starts_type_ const& starts = {}, match_t const& match = {},
                                              ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        // A zeroed view starts every page at its prefix, so only this side sees one start missing
        std::size_t const count = std::size(prefixes), started = std::size(starts);
        if (started != 0 && started != count)
            return refused_<completion_, strings_pages_t>(
                {status_code_t::invalid_argument_k, "Every prefix needs its own start, or none"});
        return strings_scan_<completion_>(receiver_, collection_, strings_view_of(prefixes), strings_view_of(starts),
                                          count, limit, arena, match, watch);
    }

    /**
     *  @brief Draws up to @p wanted strings at random into @p arena, as one page.
     *  @param[in] arena Memory the strings land in, which the answer views, borrowed until the
     *      call is final.
     *  @param[in] watch Whether a read inside a transaction records the strings for conflicts.
     */
    answer<completion_, strings_pages_t> sample(std::size_t wanted, arena_t& arena,
                                                ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return called_<completion_, strings_pages_t>(
            [&](ustore_outcome_t* outcome, outputs<strings_pages_t>* written) noexcept {
                written->count = 1;
                ustore_strings_sample_t sample {};
                sample.database = receiver_.database;
                sample.executor = receiver_.executor;
                sample.outcome = outcome;
                sample.completion = completion_;
                sample.view = receiver_.view(watch);
                sample.count = 1;
                sample.collections = {&collection_, 0};
                sample.offsets = &written->pages;
                sample.strings = &written->strings;
                sample.arena = arena;
                sample.wanted = wanted;
                ustore_strings_sample(&sample);
            });
    }

    /**
     *  @brief Estimates the strings and the space from @p start, inclusive, to @p end, exclusive,
     *      into @p arena.
     *  @param[in] start First string of the range, or an empty one to start at the first string,
     *      borrowed until the call is final.
     *  @param[in] end String the range stops before, or an empty one to bound nothing, borrowed
     *      until the call is final.
     *  @param[in] arena Memory the bounds land in, borrowed until the call is final.
     *  @param[in] watch Whether a read inside a transaction records the range for conflicts.
     */
    answer<completion_, size_estimates_t> measure(value_view_t const& start, value_view_t const& end, arena_t& arena,
                                                  ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return called_<completion_, size_estimates_t>(
            [&](ustore_outcome_t* outcome, outputs<size_estimates_t>* written) noexcept {
                // An empty bound is the zeroed view, so it never borrows the caller's text
                std::span<value_view_t const> const starts {&start, start.empty() ? 0u : 1u};
                std::span<value_view_t const> const ends {&end, end.empty() ? 0u : 1u};
                ustore_strings_measure_t measure {};
                measure.database = receiver_.database;
                measure.executor = receiver_.executor;
                measure.outcome = outcome;
                measure.completion = completion_;
                measure.view = receiver_.view(watch);
                measure.count = 1;
                measure.collections = {&collection_, 0};
                measure.start_strings = strings_view_of(starts);
                measure.end_strings = strings_view_of(ends);
                measure_outputs_(measure, *written);
                measure.arena = arena;
                ustore_strings_measure(&measure);
            });
    }

    /** Removes every string, keeping the collection. */
    answer<completion_, void> clear() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_pairs_k);
    }

    /** Removes the collection with all its strings. */
    answer<completion_, void> drop() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_collection_k);
    }

#pragma endregion Verbs

#pragma region Shorthand

    /** Reads the key @p text maps onto through a one-off arena, the nil UUID when the collection
     *  lacks the string. */
    expected<ustore_key_t> value(value_view_t text, ustore_watch_t watch = ustore_watch_keys_k) const noexcept
        requires blocks_k
    {
        auto [arena, arena_status] = arena_t::make(receiver_.executor);
        if (arena_status.failed()) return arena_status;
        auto [keys, read_status] = read(std::span<value_view_t const> {&text, 1}, arena, watch);
        if (read_status.failed()) return read_status;
        return keys[0];
    }

    /** Checks whether the collection holds @p text through a one-off arena. */
    expected<bool> present(value_view_t text, ustore_watch_t watch = ustore_watch_keys_k) const noexcept
        requires blocks_k
    {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept -> expected<bool> {
            std::span<value_view_t const> const texts {&text, 1};
            auto [presences, read_status] = strings_read_<completion_, bits_view_t>(
                receiver_, collection_, strings_view_of(texts), texts.size(), arena, watch);
            if (read_status.failed()) return read_status;
            return presences && presences[0];
        });
    }

    /** Upserts the key one string maps onto. */
    status_t assign(value_view_t text, ustore_key_t key,
                    ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires(blocks_k && writes_k)
    {
        return write(std::span<value_view_t const> {&text, 1}, {&key, 1}, durability);
    }

    /** Removes one string together with its key. */
    status_t remove(value_view_t text, ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires(blocks_k && writes_k)
    {
        return remove(std::span<value_view_t const> {&text, 1}, durability);
    }

    /**
     *  @brief Lists up to @p limit strings of @p prefix into @p arena, in the order of their text.
     *
     *  The page starts at @p start, which it returns too when the collection holds it, so a
     *  walk that pages by hand skips the string it resumed from, and @c prefixed streams
     *  the pages instead.
     */
    expected<sz::strings_view_t> prefixed_page(value_view_t prefix, std::size_t limit, arena_t& arena,
                                               value_view_t start = value_view_t::make_empty(),
                                               ustore_watch_t watch = ustore_watch_keys_k) const noexcept
        requires blocks_k
    {
        auto [pages, scan_status] = scan(std::span<value_view_t const> {&prefix, 1}, limit, arena,
                                         std::span<value_view_t const> {&start, 1}, {}, watch);
        if (scan_status.failed()) return scan_status;
        return pages.strings;
    }

    /** Opens a stream over the strings of @p prefix, which outlives the stream. */
    expected<strings_stream_t> prefix_begin(
        value_view_t prefix = value_view_t::make_empty(),
        std::size_t read_ahead = strings_stream_t::default_read_ahead_k) const noexcept
        requires blocks_k
    {
        return strings_stream_t::make(receiver_, collection_, prefix, read_ahead);
    }

    /** Walks every string of @p prefix, which outlives the range, in the order of their text. */
    expected<prefix_range_t> prefixed(value_view_t prefix = value_view_t::make_empty(),
                                      std::size_t read_ahead = strings_stream_t::default_read_ahead_k) const noexcept
        requires blocks_k
    {
        auto [begin, prefix_begin_status] = prefix_begin(prefix, read_ahead);
        if (prefix_begin_status.failed()) return prefix_begin_status;
        return prefix_range_t {std::move(begin), strings_stream_t {}};
    }

    /** Counts the strings of @p prefix by streaming through all of them. */
    expected<std::size_t> size(value_view_t prefix = value_view_t::make_empty()) const noexcept
        requires blocks_k
    {
        auto [begin, prefix_begin_status] = prefix_begin(prefix);
        if (prefix_begin_status.failed()) return prefix_begin_status;

        std::size_t count = 0;
        for (strings_stream_t& stream = begin; !stream.is_end(); ++stream) ++count;
        return count;
    }

#pragma endregion Shorthand
};

/** A collection keyed by strings whose verbs block, reading and writing the head
 *  or a transaction. */
using strings_collection_t = strings_collection_on<ustore_completion_blocking_k, collection_access_t::read_write_k>;

/** A collection keyed by strings whose verbs block and only read, as a snapshot hands out. */
using strings_read_collection_t = strings_collection_on<ustore_completion_blocking_k, collection_access_t::read_only_k>;

} // namespace unum::ustore
