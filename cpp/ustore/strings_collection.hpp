/**
 *  @file cpp/ustore/strings_collection.hpp
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief Handles to one collection keyed by variable-length strings, with ordered prefix walks, and their deferred
 *      twins, whose verbs submit to an executor.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::ptrdiff_t`

#include <iterator>    // `std::forward_iterator_tag`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`, `std::swap`, `std::exchange`

#include "ustore/ranges.hpp" // `strings_of`, `range`, `keys_view_t`, `bits_view_t`, `values_tape_t`
#include "ustore/status.hpp" // `status_t`, `expected`, `deferred`
#include "ustore/strings.h"
#include "ustore/types.hpp" // `arena_t`, `receiver_t`, `value_view_t`

namespace unum::ustore {

class deferred_strings_read_collection_t;
class deferred_strings_collection_t;

#pragma region Streams

/** Scans one page of up to @p limit strings of @p prefix from @p start into @p arena. */
inline expected<sz::strings_view_t> scan_strings_page( //
    receiver_t const& receiver, ustore_watch_t watch, ustore_collection_id_t const& collection,
    value_view_t const& prefix, value_view_t const& start, std::size_t limit, ustore_arena_t arena) noexcept {

    strided_strings<value_view_t> const prefixes {{&prefix, 0}};
    strided_strings<value_view_t> const starts {{&start, 0}};
    ustore_length_t const* offsets = nullptr;
    ustore_strings_view_t found_strings {};
    ustore_outcome_t outcome {};
    ustore_strings_scan_t scan {};
    scan.database = receiver.database;
    scan.executor = receiver.executor;
    scan.outcome = &outcome;
    scan.view = receiver.view(watch);
    scan.count = 1;
    scan.collections = {&collection, 0};
    scan.prefixes = prefixes.view();
    scan.start_strings = starts.view();
    scan.offsets = &offsets;
    scan.strings = &found_strings;
    scan.arena = arena;
    scan.wanted = limit;

    ustore_strings_scan(&scan);
    if (status_t const status {outcome}; status.failed()) return status;
    if (!offsets) return strings_of({}, 0);
    return strings_of(tape_slice(found_strings, offsets[0]), offsets[1] - offsets[0]);
}

/**
 *  @brief Owns a walk over the strings of one prefix from its open until its destruction, which every pull resumes
 *      past the last string the pull before it handed out, at the view its receiver read at the open.
 *
 *  The explicit form of @c strings_stream_t: every pull takes the caller's arena, which holds the page until its next
 *  call. The cursor copies its prefix and start, and moves but never copies itself.
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

    /** Opens a walk over the strings of @p prefix in @p collection from @p start, at the view @p receiver reads. */
    static expected<strings_cursor_t> make(receiver_t receiver, ustore_collection_id_t collection,
                                           value_view_t prefix = value_view_t::make_empty(),
                                           value_view_t start = value_view_t::make_empty()) noexcept {
        ustore_strings_cursor_t cursor = nullptr;
        ustore_outcome_t outcome {};
        ustore_strings_cursor_init_t init {.cursor = &cursor,
                                           .database = receiver.database,
                                           .outcome = &outcome,
                                           .view = receiver.view(ustore_watch_keys_k),
                                           .collection = collection,
                                           .prefix = prefix.bytes(),
                                           .start_string = start.bytes()};
        ustore_strings_cursor_init(&init);
        if (status_t const status {outcome}; status.failed()) return status;
        return strings_cursor_t {receiver, cursor};
    }

    /** Pulls up to @p wanted strings into @p arena in the order of their text, fewer once the walk nears its end and
     *  none past it. */
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
 *  @brief A stream over the strings of one prefix that pulls them in pages from a cursor, in the order of their text.
 *
 *  As shorthand for a range-based loop, which has no place for an arena, it owns one arena for its lifetime, where
 *  @c strings_cursor_t takes the caller's per pull. The prefix it walks is borrowed, so it outlives the stream, a page
 *  short of the read-ahead ends the walk, and a default-constructed stream is the end of every walk.
 */
class strings_stream_t {

    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {ustore_collection_main_k};
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

    /** Opens a walk over the strings of @p prefix in @p collection, pulling @p read_ahead of them a page into an arena
     *  made where the executor of @p receiver computes. */
    static expected<strings_stream_t> make(receiver_t receiver, ustore_collection_id_t collection, value_view_t prefix,
                                           std::size_t read_ahead = default_read_ahead_k) noexcept {
        auto [arena, arena_status] = arena_t::make(receiver.executor);
        if (arena_status.failed()) return arena_status;
        strings_stream_t stream {receiver, collection, prefix, std::move(arena), read_ahead};
        if (status_t const sought = stream.seek_to_first(); sought.failed()) return sought;
        return stream;
    }

    /** Restarts the walk at @p start, which is returned too when the collection holds it, through a cursor opened
     *  there. */
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

    /** Advances like @c advance, but a failed pull ends the walk since a reference carries no status. */
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

#pragma region Collections

/**
 *  @brief A handle to one collection mapping variable-length strings onto keys that only reads, cheap to construct and
 *      to copy.
 *
 *  It addresses either the latest state or the view of a transaction or snapshot, holds no state of its own, so any
 *  number of threads share it, and never outlives its database. Strings carry explicit lengths, so a zero byte inside
 *  one is stored like any other, and the empty string is an ordinary key.
 */
class strings_read_collection_t {
  protected:
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {ustore_collection_main_k};

  public:
    using prefix_range_t = range<strings_stream_t>;

    /** The handle a snapshot hands out for this kind of collection, which only reads. */
    using read_only_t = strings_read_collection_t;

    /** The twin a deferred receiver hands out, whose verbs answer deferred calls. */
    using deferred_t = deferred_strings_read_collection_t;

    /** What the collections this kind of handle addresses hold, which a creation records and a lookup checks. */
    static constexpr ustore_modality_t modality_k = ustore_modality_strings_k;

    strings_read_collection_t() noexcept = default;
    strings_read_collection_t(receiver_t receiver, ustore_collection_id_t collection) noexcept
        : receiver_(receiver), collection_(collection) {}

    operator ustore_collection_id_t() const noexcept { return collection_; }
    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }

    /** Reads the keys a batch of strings map onto into @p arena, where a missing string reads as the nil UUID, and
     *  marks the present strings in @p presences when asked. */
    expected<keys_view_t> read(std::span<value_view_t const> texts, arena_t& arena,
                               ustore_watch_t watch = ustore_watch_keys_k, bits_view_t* presences = nullptr) noexcept {
        if (texts.empty()) return keys_view_t {};

        strided_strings<value_view_t> const strings {strided(texts).begin()};
        ustore_octet_t const* found_presences = nullptr;
        ustore_key_t const* keys = nullptr;
        ustore_outcome_t outcome {};
        ustore_strings_read_t read {};
        read.database = receiver_.database;
        read.executor = receiver_.executor;
        read.outcome = &outcome;
        read.view = receiver_.view(watch);
        read.count = texts.size();
        read.collections = {&collection_, 0};
        read.strings = strings.view();
        read.presences = presences ? &found_presences : nullptr;
        read.values = &keys;
        read.arena = arena;

        ustore_strings_read(&read);
        if (status_t const status {outcome}; status.failed()) return status;
        if (presences) *presences = bits_view_t {found_presences};
        return keys_view_t {{keys, sizeof(ustore_key_t)}, texts.size()};
    }

    /** Reads the key @p text maps onto through a one-off arena, the nil UUID when the collection lacks the string. */
    expected<ustore_key_t> value(value_view_t text, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        auto [arena, arena_status] = arena_t::make(receiver_.executor);
        if (arena_status.failed()) return arena_status;
        auto [keys, read_status] = read({&text, 1}, arena, watch);
        if (read_status.failed()) return read_status;
        return keys[0];
    }

    /** Checks whether the collection holds @p text through a one-off arena. */
    expected<bool> present(value_view_t text, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        auto [arena, arena_status] = arena_t::make(receiver_.executor);
        if (arena_status.failed()) return arena_status;
        bits_view_t presences;
        expected<keys_view_t> const found = read({&text, 1}, arena, watch, &presences);
        if (found.failed()) return found.status();
        return presences && presences[0];
    }

    /**
     *  @brief Lists up to @p limit strings of @p prefix into @p arena, in the order of their text.
     *
     *  The page starts at @p start, which it returns too when the collection holds it, so a walk that pages by hand
     *  skips the string it resumed from, and @c prefixed streams the pages instead.
     */
    expected<sz::strings_view_t> prefixed_page(value_view_t prefix, std::size_t limit, arena_t& arena,
                                               value_view_t start = value_view_t::make_empty(),
                                               ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return scan_strings_page(receiver_, watch, collection_, prefix, start, limit, arena);
    }

    /** Opens a stream over the strings of @p prefix, which outlives the stream. */
    expected<strings_stream_t> prefix_begin(
        value_view_t prefix = value_view_t::make_empty(),
        std::size_t read_ahead = strings_stream_t::default_read_ahead_k) const noexcept {
        return strings_stream_t::make(receiver_, collection_, prefix, read_ahead);
    }

    /** Walks every string of @p prefix, which outlives the range, in the order of their text. */
    expected<prefix_range_t> prefixed(value_view_t prefix = value_view_t::make_empty(),
                                      std::size_t read_ahead = strings_stream_t::default_read_ahead_k) const noexcept {
        auto [begin, prefix_begin_status] = prefix_begin(prefix, read_ahead);
        if (prefix_begin_status.failed()) return prefix_begin_status;
        return prefix_range_t {std::move(begin), strings_stream_t {}};
    }

    /** Counts the strings of @p prefix by streaming through all of them. */
    expected<std::size_t> size(value_view_t prefix = value_view_t::make_empty()) const noexcept {
        auto [begin, prefix_begin_status] = prefix_begin(prefix);
        if (prefix_begin_status.failed()) return prefix_begin_status;

        std::size_t count = 0;
        for (strings_stream_t& stream = begin; !stream.is_end(); ++stream) ++count;
        return count;
    }
};

/** A handle to one collection mapping variable-length strings onto keys, cheap to construct, which reads and writes the
 *  head or the view of a transaction. */
class strings_collection_t : public strings_read_collection_t {

    status_t drop_with_(ustore_drop_mode_t mode) noexcept {
        ustore_outcome_t outcome {};
        ustore_collection_drop_t collection_drop {};
        collection_drop.database = receiver_.database;
        collection_drop.executor = receiver_.executor;
        collection_drop.outcome = &outcome;
        collection_drop.collection = collection_;
        collection_drop.mode = mode;
        ustore_collection_drop(&collection_drop);
        return status_t {outcome};
    }

  public:
    using strings_read_collection_t::strings_read_collection_t;

    /** The twin a deferred receiver hands out, whose verbs answer deferred calls. */
    using deferred_t = deferred_strings_collection_t;

    /** Upserts the key one string maps onto. */
    status_t assign(value_view_t text, ustore_key_t key,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return assign({&text, 1}, {&key, 1}, durability);
    }

    /** Upserts the keys a batch of strings map onto, one per string. */
    status_t assign(std::span<value_view_t const> texts, std::span<ustore_key_t const> keys,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        if (texts.size() != keys.size())
            return status_t {status_code_t::invalid_argument_k, "Every string of a batch needs its own key"};
        if (texts.empty()) return {};

        strided_strings<value_view_t> const strings {strided(texts).begin()};
        ustore_outcome_t outcome {};
        ustore_strings_write_t write {};
        write.database = receiver_.database;
        write.executor = receiver_.executor;
        write.outcome = &outcome;
        write.transaction = receiver_.transaction;
        write.count = texts.size();
        write.collections = {&collection_, 0};
        write.strings = strings.view();
        write.values = {keys.data(), sizeof(ustore_key_t)};
        write.durability = durability;

        ustore_strings_write(&write);
        return status_t {outcome};
    }

    status_t remove(value_view_t text, ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return remove({&text, 1}, durability);
    }

    status_t remove(std::span<value_view_t const> texts,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        if (texts.empty()) return {};

        strided_strings<value_view_t> const strings {strided(texts).begin()};
        ustore_outcome_t outcome {};
        ustore_strings_remove_t remove {};
        remove.database = receiver_.database;
        remove.executor = receiver_.executor;
        remove.outcome = &outcome;
        remove.transaction = receiver_.transaction;
        remove.count = texts.size();
        remove.collections = {&collection_, 0};
        remove.strings = strings.view();
        remove.durability = durability;

        ustore_strings_remove(&remove);
        return status_t {outcome};
    }

    /** Removes every string, keeping the collection. */
    status_t clear() noexcept { return drop_with_(ustore_drop_pairs_k); }

    /** Removes the collection with all its strings. */
    status_t drop() noexcept { return drop_with_(ustore_drop_collection_k); }
};

#pragma endregion Collections

#pragma region Deferred Verbs

/** What a deferred @c ustore_strings_read keeps beside its strings: the collection the arguments point to, and the
 *  keys the call writes into the arena it borrows. */
struct strings_read_t {
    using arguments_t = ustore_strings_read_t;
    using answer_t = expected<keys_view_t>;

    /** Collection every string lives in. */
    ustore_collection_id_t collection {};

    /** Number of strings, the length of the output. */
    ustore_size_t count = 0;

    /** Output for the key of every string inside the arena, where a missing string reads as the nil UUID. */
    ustore_key_t const* keys = nullptr;

    static void submit(arguments_t& arguments) noexcept { ustore_strings_read(&arguments); }
    answer_t finish() && noexcept { return keys_view_t {{keys, sizeof(ustore_key_t)}, count}; }
};

/** What a deferred @c ustore_strings_write keeps beside its strings and keys: the collection. */
struct strings_write_t {
    using arguments_t = ustore_strings_write_t;
    using answer_t = status_t;

    /** Collection every string lives in. */
    ustore_collection_id_t collection {};

    static void submit(arguments_t& arguments) noexcept { ustore_strings_write(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

/** What a deferred @c ustore_strings_remove keeps beside its strings: the collection. */
struct strings_remove_t {
    using arguments_t = ustore_strings_remove_t;
    using answer_t = status_t;

    /** Collection every string lives in. */
    ustore_collection_id_t collection {};

    static void submit(arguments_t& arguments) noexcept { ustore_strings_remove(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

/** What a deferred @c ustore_strings_scan keeps beside its prefixes and starts: the collection the arguments point to,
 *  and the outputs the call writes into the arena it borrows. */
struct strings_scan_t {

    /** What a finished scan hands over: the strings every prefix found, page after page, inside the arena. */
    struct finished_t {

        /** Every string found, in the order of the prefixes and then of their text. */
        sz::strings_view_t strings {strings_of({}, 0)};

        /** Where the page of every prefix starts among @c strings, with one more closing the last page. */
        std::span<ustore_length_t const> pages;
    };

    using arguments_t = ustore_strings_scan_t;
    using answer_t = expected<finished_t>;

    /** Collection every prefix is walked in. */
    ustore_collection_id_t collection {};

    /** Number of prefixes, the pages of the answer. */
    ustore_size_t count = 0;

    /** Outputs for the offsets of the pages and the tape of strings, inside the arena. */
    ustore_length_t const* pages = nullptr;
    ustore_strings_view_t strings {};

    static void submit(arguments_t& arguments) noexcept { ustore_strings_scan(&arguments); }

    answer_t finish() && noexcept {
        if (!pages) return finished_t {};
        return finished_t {strings_of(tape_slice(strings, pages[0]), pages[count] - pages[0]), {pages, count + 1}};
    }
};

#pragma endregion Deferred Verbs

#pragma region Deferred Collections

/** A collection keyed by strings that only reads, whose verbs submit to an executor and answer deferred calls, as a
 *  snapshot offers it. Every plain input a verb reads moves into the call, so nothing it borrowed can go before the
 *  call is final, while an arena is borrowed, since freeing it waits for the call. */
class deferred_strings_read_collection_t {
  protected:
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {ustore_collection_main_k};

  public:
    deferred_strings_read_collection_t() noexcept = default;
    explicit deferred_strings_read_collection_t(receiver_t receiver,
                                                ustore_collection_id_t collection = ustore_collection_main_k) noexcept
        : receiver_(receiver), collection_(collection) {}

    operator ustore_collection_id_t() const noexcept { return collection_; }

    /**
     *  @brief Reads the keys @p texts map onto into @p arena, where a missing string reads as the nil UUID.
     *  @param[in] texts Strings to read, owned by the call until it is final.
     *  @param[in] arena Memory the keys land in, borrowed until the call is final, which the answer views.
     *  @param[in] watch Whether a read inside a transaction records the strings for conflicts.
     */
    deferred<strings_read_t> read(values_tape_t texts, arena_t& arena,
                                  ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return deferred<strings_read_t>::submit(
            [&](strings_read_t& verb, ustore_strings_read_t& arguments, values_tape_t const& owned) noexcept {
                verb.collection = collection_;
                verb.count = owned.size();
                arguments.database = receiver_.database;
                arguments.executor = receiver_.executor;
                arguments.view = receiver_.view(watch);
                arguments.count = owned.size();
                arguments.collections = {&verb.collection, 0};
                arguments.strings = tape_of(owned);
                arguments.values = &verb.keys;
                arguments.arena = arena;
                return status_t {};
            },
            std::move(texts));
    }

    /**
     *  @brief Lists up to @p limit strings of every prefix of @p prefixes into @p arena, in the order of their text.
     *  @param[in] prefixes Prefixes to walk, a page each, owned by the call until it is final.
     *  @param[in] limit Most strings a page holds.
     *  @param[in] arena Memory the strings land in, borrowed until the call is final, which the answer views.
     *  @param[in] starts None to start every page at its prefix, or the smallest string of every page, which the page
     *      holds too when the collection does.
     *  @param[in] watch Whether a read inside a transaction records the strings for conflicts.
     */
    deferred<strings_scan_t> scan(values_tape_t prefixes, std::size_t limit, arena_t& arena, values_tape_t starts = {},
                                  ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return deferred<strings_scan_t>::submit(
            [&](strings_scan_t& verb, ustore_strings_scan_t& arguments, values_tape_t const& owned_prefixes,
                values_tape_t const& owned_starts) noexcept {
                std::size_t const count = owned_prefixes.size();
                if (owned_starts.size() != 0 && owned_starts.size() != count)
                    return status_t {status_code_t::invalid_argument_k, "Every prefix needs its own start, or none"};
                verb.collection = collection_;
                verb.count = count;
                arguments.database = receiver_.database;
                arguments.executor = receiver_.executor;
                arguments.view = receiver_.view(watch);
                arguments.count = count;
                arguments.collections = {&verb.collection, 0};
                arguments.prefixes = tape_of(owned_prefixes);
                arguments.start_strings = tape_of(owned_starts);
                arguments.offsets = &verb.pages;
                arguments.strings = &verb.strings;
                arguments.arena = arena;
                arguments.wanted = limit;
                return status_t {};
            },
            std::move(prefixes), std::move(starts));
    }
};

/** A collection keyed by strings whose verbs submit to an executor and answer deferred calls, reading and writing the
 *  head or a transaction. */
class deferred_strings_collection_t : public deferred_strings_read_collection_t {
  public:
    using deferred_strings_read_collection_t::deferred_strings_read_collection_t;

    /**
     *  @brief Upserts the keys of @p keys onto the strings of @p texts pairwise.
     *  @param[in] texts Strings to write, owned by the call until it is final.
     *  @param[in] keys One key per string, owned by the call until it is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    template <owned_array_concept<ustore_key_t> keys_type_>
    deferred<strings_write_t> write(values_tape_t texts, keys_type_ keys,
                                    ustore_durability_t durability = ustore_durability_buffered_k) const noexcept {
        return deferred<strings_write_t>::submit(
            [&](strings_write_t& verb, ustore_strings_write_t& arguments, values_tape_t const& owned_texts,
                keys_type_ const& owned_keys) noexcept {
                if (owned_texts.size() != owned_keys.size())
                    return status_t {status_code_t::invalid_argument_k, "Every string of a write needs its own key"};
                verb.collection = collection_;
                arguments.database = receiver_.database;
                arguments.executor = receiver_.executor;
                arguments.transaction = receiver_.transaction;
                arguments.count = owned_texts.size();
                arguments.collections = {&verb.collection, 0};
                arguments.strings = tape_of(owned_texts);
                arguments.values = {owned_keys.data(), sizeof(ustore_key_t)};
                arguments.durability = durability;
                return status_t {};
            },
            std::move(texts), std::move(keys));
    }

    /**
     *  @brief Removes the strings of @p texts together with their keys.
     *  @param[in] texts Strings to remove, owned by the call until it is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    deferred<strings_remove_t> remove(values_tape_t texts,
                                      ustore_durability_t durability = ustore_durability_buffered_k) const noexcept {
        return deferred<strings_remove_t>::submit(
            [&](strings_remove_t& verb, ustore_strings_remove_t& arguments, values_tape_t const& owned) noexcept {
                verb.collection = collection_;
                arguments.database = receiver_.database;
                arguments.executor = receiver_.executor;
                arguments.transaction = receiver_.transaction;
                arguments.count = owned.size();
                arguments.collections = {&verb.collection, 0};
                arguments.strings = tape_of(owned);
                arguments.durability = durability;
                return status_t {};
            },
            std::move(texts));
    }
};

#pragma endregion Deferred Collections

} // namespace unum::ustore
