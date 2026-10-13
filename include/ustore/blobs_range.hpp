/**
 *  @file include/ustore/blobs_range.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief A cursor over the keys and values of one binary collection, the streams built on it, and slices.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::ptrdiff_t`

#include <iterator>    // `std::forward_iterator_tag`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::pair`, `std::move`, `std::exchange`, `std::swap`

#include "ustore/ranges.hpp" // `strings_of`
#include "ustore/status.hpp" // `status_t`, `expected`
#include "ustore/types.hpp"  // `arena_t`, `receiver_t`
#include "ustore/ustore.h"

namespace unum::ustore {

/** What one pull of a cursor hands over inside the arena it was given: the keys, and the values of the keys. */
struct blobs_page_t {
    std::span<ustore_key_t const> keys;
    sz::strings_view_t values {strings_of({}, 0)};
};

/**
 *  @brief Owns a walk over the keys of one collection from its open until its destruction, which every pull resumes
 *      past the last key the pull before it handed out, at the view its receiver read at the open.
 *
 *  The explicit form of the streams below: every pull takes the caller's arena, which holds the page until its next
 *  call, so a binding hands the cursor any arena it keeps. It moves but never copies.
 */
class blobs_cursor_t {

    receiver_t receiver_ {};
    ustore_blobs_cursor_t cursor_ = nullptr;

    blobs_cursor_t(receiver_t receiver, ustore_blobs_cursor_t cursor) noexcept : receiver_(receiver), cursor_(cursor) {}

    /** Pulls up to @p wanted keys into @p arena, with their values into @p values unless it is null. */
    expected<std::span<ustore_key_t const>> pull_(std::size_t wanted, arena_t& arena,
                                                  ustore_strings_view_t* values) noexcept {
        ustore_key_t const* keys = nullptr;
        ustore_size_t written = 0;
        ustore_outcome_t outcome {};
        ustore_blobs_cursor_next_t next {};
        next.database = receiver_.database;
        next.executor = receiver_.executor;
        next.outcome = &outcome;
        next.cursor = cursor_;
        next.keys = &keys;
        next.values = values;
        next.written = &written;
        next.arena = arena;
        next.wanted = wanted;
        ustore_blobs_cursor_next(&next);
        if (status_t const status {outcome}; status.failed()) return status;
        return std::span<ustore_key_t const> {keys, static_cast<std::size_t>(written)};
    }

  public:
    blobs_cursor_t() noexcept = default;
    blobs_cursor_t(blobs_cursor_t const&) = delete;
    blobs_cursor_t& operator=(blobs_cursor_t const&) = delete;
    ~blobs_cursor_t() noexcept { ustore_blobs_cursor_free(cursor_); }

    blobs_cursor_t(blobs_cursor_t&& other) noexcept
        : receiver_(other.receiver_), cursor_(std::exchange(other.cursor_, nullptr)) {}

    blobs_cursor_t& operator=(blobs_cursor_t&& other) noexcept {
        std::swap(receiver_, other.receiver_);
        std::swap(cursor_, other.cursor_);
        return *this;
    }

    /** Opens a walk over the keys of @p collection from @p start, at the view @p receiver reads. */
    static expected<blobs_cursor_t> make(receiver_t receiver, ustore_collection_id_t collection,
                                         ustore_key_t start = smallest_key_k) noexcept {
        ustore_blobs_cursor_t cursor = nullptr;
        ustore_outcome_t outcome {};
        ustore_blobs_cursor_init_t init {.cursor = &cursor,
                                         .database = receiver.database,
                                         .outcome = &outcome,
                                         .view = receiver.view(ustore_watch_keys_k),
                                         .collection = collection,
                                         .start_key = start};
        ustore_blobs_cursor_init(&init);
        if (status_t const status {outcome}; status.failed()) return status;
        return blobs_cursor_t {receiver, cursor};
    }

    /** Pulls up to @p wanted keys into @p arena, fewer once the walk nears its end and none past it. */
    expected<std::span<ustore_key_t const>> next(std::size_t wanted, arena_t& arena) noexcept {
        return pull_(wanted, arena, nullptr);
    }

    /** Pulls up to @p wanted keys with their values into @p arena in one call. */
    expected<blobs_page_t> next_pairs(std::size_t wanted, arena_t& arena) noexcept {
        ustore_strings_view_t values {};
        auto [keys, status] = pull_(wanted, arena, &values);
        if (status.failed()) return status;
        return blobs_page_t {keys, strings_of(values, keys.size())};
    }
};

/** A stream over the keys of one collection that pulls them in pages from a cursor.
 *  It keeps the cursor and the arena holding the page in hand, so it moves but never copies, and prefers @c seek to
 *  being rebuilt. As shorthand for a range-based loop, which has no place for an arena, it owns that one arena for
 *  its lifetime, where @c blobs_cursor_t takes the caller's per pull. A page short of the read-ahead ends the stream,
 *  and a default-constructed stream is at the end. */
class keys_stream_t {

    friend class graph_stream_t;

    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {ustore_collection_main_k};

    blobs_cursor_t cursor_;
    arena_t arena_;
    std::size_t read_ahead_ {1};

    std::span<ustore_key_t const> fetched_keys_ {};
    std::size_t fetched_offset_ {0};

    keys_stream_t(receiver_t receiver, ustore_collection_id_t collection, arena_t arena,
                  std::size_t read_ahead) noexcept
        : receiver_(receiver), collection_(collection), arena_(std::move(arena)),
          read_ahead_(read_ahead ? read_ahead : 1) {}

    status_t prefetch() noexcept {
        auto [found_keys, pull_status] = cursor_.next(read_ahead_, arena_);
        if (pull_status.failed()) return pull_status;
        fetched_keys_ = found_keys;
        fetched_offset_ = 0;
        return {};
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

    /** Opens a stream over the keys of @p collection from @p start, pulling @p read_ahead of them a page into an
     *  arena of its own, made where the executor of @p receiver computes. */
    static expected<keys_stream_t> make(receiver_t receiver, ustore_collection_id_t collection, ustore_key_t start,
                                        std::size_t read_ahead = default_read_ahead_k) noexcept {
        auto [arena, arena_status] = arena_t::make(receiver.executor);
        if (arena_status.failed()) return arena_status;
        keys_stream_t stream {receiver, collection, std::move(arena), read_ahead};
        if (status_t const sought = stream.seek(start); sought.failed()) return sought;
        return stream;
    }

    /** Restarts the stream at @p key, through a cursor opened there. */
    status_t seek(ustore_key_t key) noexcept {
        auto [cursor, cursor_status] = blobs_cursor_t::make(receiver_, collection_, key);
        if (cursor_status.failed()) return cursor_status;
        cursor_ = std::move(cursor);
        return prefetch();
    }

    status_t advance() noexcept {
        if (fetched_offset_ + 1 < fetched_keys_.size()) ++fetched_offset_;
        else if (fetched_keys_.size() < read_ahead_) fetched_offset_ = fetched_keys_.size();
        else return prefetch();
        return {};
    }

    /** Advances like @c advance, but a failed pull ends the stream since a reference carries no status. */
    keys_stream_t& operator++() noexcept {
        if (advance().succeeded()) return *this;
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
        if (collection_ != other.collection_) return false;
        if (is_end() || other.is_end()) return is_end() == other.is_end();
        return key() == other.key();
    }
};

/** A stream over the keys and values of one collection that pulls both in pages from a cursor, in one call per page.
 *  As shorthand for a range-based loop, which has no place for an arena, it owns that one arena for its lifetime,
 *  where @c blobs_cursor_t takes the caller's per pull. A default-constructed stream is at the end. */
class pairs_stream_t {

    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {ustore_collection_main_k};

    blobs_cursor_t cursor_;
    arena_t arena_;
    std::size_t read_ahead_ {1};

    blobs_page_t fetched_ {};
    std::size_t fetched_offset_ {0};

    pairs_stream_t(receiver_t receiver, ustore_collection_id_t collection, arena_t arena,
                   std::size_t read_ahead) noexcept
        : receiver_(receiver), collection_(collection), arena_(std::move(arena)),
          read_ahead_(read_ahead ? read_ahead : 1) {}

    status_t prefetch() noexcept {
        auto [page, pull_status] = cursor_.next_pairs(read_ahead_, arena_);
        if (pull_status.failed()) return pull_status;
        fetched_ = page;
        fetched_offset_ = 0;
        return {};
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = std::pair<ustore_key_t, value_view_t>;

    static constexpr std::size_t default_read_ahead_k = 256;

    pairs_stream_t() noexcept = default;
    pairs_stream_t(pairs_stream_t&&) noexcept = default;
    pairs_stream_t& operator=(pairs_stream_t&&) noexcept = default;
    pairs_stream_t(pairs_stream_t const&) = delete;
    pairs_stream_t& operator=(pairs_stream_t const&) = delete;

    /** Opens a stream over the pairs of @p collection from @p start, pulling @p read_ahead of them a page into an arena
     *  of its own, made where the executor of @p receiver computes. */
    static expected<pairs_stream_t> make(receiver_t receiver, ustore_collection_id_t collection, ustore_key_t start,
                                         std::size_t read_ahead = default_read_ahead_k) noexcept {
        auto [arena, arena_status] = arena_t::make(receiver.executor);
        if (arena_status.failed()) return arena_status;
        pairs_stream_t stream {receiver, collection, std::move(arena), read_ahead};
        if (status_t const sought = stream.seek(start); sought.failed()) return sought;
        return stream;
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

    /** Advances like @c advance, but a failed pull ends the stream since a reference carries no status. */
    pairs_stream_t& operator++() noexcept {
        if (advance().succeeded()) return *this;
        fetched_ = {};
        fetched_offset_ = 0;
        return *this;
    }

    ustore_key_t key() const noexcept { return fetched_.keys[fetched_offset_]; }
    value_view_t value() const noexcept { return std::string_view {fetched_.values[fetched_offset_]}; }
    value_type pair() const noexcept { return {key(), value()}; }
    value_type operator*() const noexcept { return pair(); }

    status_t seek_to_first() noexcept { return seek(smallest_key_k); }

    /** Pulls the page after the one in hand, which @c keys_batch exposes. */
    status_t seek_to_next_batch() noexcept { return prefetch(); }

    /** Exposes the keys of the whole page in hand, passed keys included, for use with @c seek_to_next_batch. */
    std::span<ustore_key_t const> keys_batch() noexcept {
        fetched_offset_ = fetched_.keys.size();
        return fetched_.keys;
    }

    bool is_end() const noexcept {
        return fetched_.keys.size() < read_ahead_ && fetched_offset_ >= fetched_.keys.size();
    }

    bool operator==(pairs_stream_t const& other) const noexcept {
        if (collection_ != other.collection_) return false;
        if (is_end() || other.is_end()) return is_end() == other.is_end();
        return key() == other.key();
    }
};

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

/** A slice of keys in one collection, which streams its keys or pairs and estimates its size. */
class blobs_range_t {

    receiver_t receiver_;
    ustore_collection_id_t collection_;
    ustore_key_t start_key_;
    ustore_key_t end_key_;

  public:
    blobs_range_t(receiver_t receiver, ustore_collection_id_t collection = ustore_collection_main_k,
                  ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) noexcept
        : receiver_(receiver), collection_(collection), start_key_(start_key), end_key_(end_key) {}

    receiver_t receiver() const noexcept { return receiver_; }
    ustore_collection_id_t collection() const noexcept { return collection_; }

    expected<keys_stream_t> keys_begin(std::size_t read_ahead = keys_stream_t::default_read_ahead_k) noexcept {
        return keys_stream_t::make(receiver_, collection_, start_key_, read_ahead);
    }

    /** The first key at or past the upper bound, which an unbounded range never fetches. */
    expected<keys_stream_t> keys_end() noexcept { return keys_stream_t::make(receiver_, collection_, end_key_, 1); }

    expected<pairs_stream_t> pairs_begin(std::size_t read_ahead = pairs_stream_t::default_read_ahead_k) noexcept {
        return pairs_stream_t::make(receiver_, collection_, start_key_, read_ahead);
    }

    expected<pairs_stream_t> pairs_end() noexcept { return pairs_stream_t::make(receiver_, collection_, end_key_, 1); }

    /** Estimates the size of the slice through a one-off arena, since a single range answers a fixed-size report. */
    expected<size_estimates_t> size_estimates() noexcept {
        auto [arena, arena_status] = arena_t::make(receiver_.executor);
        if (arena_status.failed()) return arena_status;

        ustore_size_t const* min_cardinality = nullptr;
        ustore_size_t const* max_cardinality = nullptr;
        ustore_size_t const* min_value_bytes = nullptr;
        ustore_size_t const* max_value_bytes = nullptr;
        ustore_size_t const* min_space_usage = nullptr;
        ustore_size_t const* max_space_usage = nullptr;

        ustore_outcome_t outcome {};
        ustore_blobs_measure_t measure {};
        measure.database = receiver_.database;
        measure.executor = receiver_.executor;
        measure.outcome = &outcome;
        measure.view = receiver_.view(ustore_watch_keys_k);
        measure.count = 1;
        measure.collections = {&collection_, 0};
        measure.start_keys = {&start_key_, 0};
        measure.end_keys = {&end_key_, 0};
        measure.min_cardinalities = &min_cardinality;
        measure.max_cardinalities = &max_cardinality;
        measure.min_value_bytes = &min_value_bytes;
        measure.max_value_bytes = &max_value_bytes;
        measure.min_space_usages = &min_space_usage;
        measure.max_space_usages = &max_space_usage;
        measure.arena = arena;

        ustore_blobs_measure(&measure);
        if (status_t const status {outcome}; status.failed()) return status;
        return size_estimates_t {{*min_cardinality, *max_cardinality},
                                 {*min_value_bytes, *max_value_bytes},
                                 {*min_space_usage, *max_space_usage}};
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

/** The keys of a collection slice, counted by streaming or sampled at random. */
struct keys_range_t {
    blobs_range_t members;

    /** Counts the keys by streaming through all of them. */
    expected<std::size_t> size() noexcept {
        auto maybe_begin = members.keys_begin();
        if (!maybe_begin) return maybe_begin.status();
        auto maybe_end = members.keys_end();
        if (!maybe_end) return maybe_end.status();

        std::size_t count = 0;
        for (keys_stream_t& stream = *maybe_begin; stream != *maybe_end; ++stream) ++count;
        return count;
    }

    /** Draws up to @p count keys at random into @p arena. */
    expected<std::span<ustore_key_t const>> sample(std::size_t count, arena_t& arena) noexcept {
        ustore_collection_id_t const collection = members.collection();
        receiver_t const receiver = members.receiver();
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* keys = nullptr;

        ustore_outcome_t outcome {};
        ustore_blobs_sample_t sample {};
        sample.database = receiver.database;
        sample.executor = receiver.executor;
        sample.outcome = &outcome;
        sample.view = receiver.view(ustore_watch_keys_k);
        sample.count = 1;
        sample.collections = {&collection, 0};
        sample.offsets = &offsets;
        sample.keys = &keys;
        sample.arena = arena;
        sample.wanted = count;
        ustore_blobs_sample(&sample);

        if (status_t const status {outcome}; status.failed()) return status;
        if (!offsets) return std::span<ustore_key_t const> {};
        return std::span<ustore_key_t const> {keys + offsets[0], keys + offsets[1]};
    }
};

/** The key-value pairs of a collection slice, counted by streaming. */
struct pairs_range_t {
    blobs_range_t members;

    /** Counts the pairs by streaming through all of them. */
    expected<std::size_t> size() noexcept {
        auto maybe_begin = members.pairs_begin();
        if (!maybe_begin) return maybe_begin.status();
        auto maybe_end = members.pairs_end();
        if (!maybe_end) return maybe_end.status();

        std::size_t count = 0;
        for (pairs_stream_t& stream = *maybe_begin; stream != *maybe_end; ++stream) ++count;
        return count;
    }
};

} // namespace unum::ustore
