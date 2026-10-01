/**
 *  @file include/ustore/strings_collection.hpp
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief A handle to one collection keyed by variable-length strings, with ordered prefix walks.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::ptrdiff_t`

#include <iterator>    // `std::forward_iterator_tag`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::move`, `std::swap`

#include "ustore/ranges.hpp" // `embedded_blobs_t`, `joined_strs_t`, `range`
#include "ustore/status.hpp" // `status_t`, `expected`
#include "ustore/strings.h"
#include "ustore/types.hpp" // `any_arena_t`, `arena_t`, `value_view_t`

namespace unum::ustore {

#pragma region Streams

/** How the next page of a walk relates to the page a stream already holds. */
enum class walk_stage_t : unsigned char {

    /** Nothing was fetched yet, so the next scan starts the walk where it was seeked. */
    fresh_k,

    /** A full page was fetched, so the next scan resumes from its last string, which it repeats. */
    resumed_k,

    /** The last page came in short of the read-ahead, so no page follows it. */
    exhausted_k,
};

/**
 *  @brief A stream over the strings of one prefix that fetches them in pages, in the order of their text.
 *
 *  A scan resumes from a string rather than from an iterator, and that string lives in the page before it,
 *  so the stream alternates between two arenas and at most two pages are alive at once. The prefix it walks
 *  is borrowed, so it outlives the stream, and a default-constructed stream is the end of every walk.
 */
class strings_stream_t {

    ustore_database_t database_ {nullptr};
    ustore_collection_id_t collection_ {ustore_collection_main_k};
    ustore_transaction_t transaction_ {nullptr};
    ustore_snapshot_t snapshot_ {};
    value_view_t prefix_ {value_view_t::make_empty()};

    arena_t current_arena_ {nullptr};
    arena_t previous_arena_ {nullptr};
    ustore_length_t read_ahead_ {0};

    walk_stage_t stage_ {walk_stage_t::exhausted_k};
    value_view_t resume_ {value_view_t::make_empty()};
    joined_strs_t fetched_ {};
    std::size_t fetched_offset_ {0};

    status_t prefetch() noexcept {

        if (stage_ == walk_stage_t::exhausted_k) {
            ++fetched_offset_;
            return {};
        }

        // The page in hand holds the string the next scan resumes from, so the arenas alternate
        walk_stage_t const previous_stage = stage_;
        std::swap(current_arena_, previous_arena_);

        ustore_error_t error {};
        ustore_length_t* found_counts = nullptr;
        ustore_length_t* found_offsets = nullptr;
        ustore_char_t* found_strings = nullptr;

        ustore_strings_scan_t scan {};
        scan.db = database_;
        scan.error = &error;
        scan.transaction = transaction_;
        scan.snapshot = snapshot_;
        scan.arena = current_arena_.member_ptr();
        scan.tasks_count = 1;
        scan.collections = &collection_;
        scan.prefixes = reinterpret_cast<ustore_str_view_t const*>(prefix_.member_ptr());
        scan.prefixes_lengths = prefix_.member_length();
        scan.start_strings = reinterpret_cast<ustore_str_view_t const*>(resume_.member_ptr());
        scan.start_strings_lengths = resume_.member_length();
        scan.count_limits = &read_ahead_;
        scan.counts = &found_counts;
        scan.strings_offsets = &found_offsets;
        scan.strings = &found_strings;

        ustore_strings_scan(&scan);
        if (error.status) return status_t {error};

        ustore_length_t const found = found_counts[0];
        fetched_ = joined_strs_t {found, found_offsets, found_strings};
        fetched_offset_ = 0;
        if (previous_stage == walk_stage_t::resumed_k && found != 0 && fetched_[0] == std::string_view {resume_})
            fetched_offset_ = 1;

        stage_ = found < read_ahead_ ? walk_stage_t::exhausted_k : walk_stage_t::resumed_k;
        if (found != 0) resume_ = fetched_[found - 1];
        return {};
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = std::string_view;

    /** Strings one page of a walk fetches, which is at least two so a resumed page brings a new one. */
    static constexpr std::size_t default_read_ahead_k = 256;

    strings_stream_t() noexcept = default;
    strings_stream_t(ustore_database_t database, ustore_collection_id_t collection = ustore_collection_main_k,
                     ustore_transaction_t transaction = nullptr, ustore_snapshot_t snapshot = {},
                     value_view_t prefix = value_view_t::make_empty(),
                     std::size_t read_ahead = default_read_ahead_k) noexcept
        : database_(database), collection_(collection), transaction_(transaction), snapshot_(snapshot), prefix_(prefix),
          current_arena_(database), previous_arena_(database),
          read_ahead_(static_cast<ustore_length_t>(read_ahead < 2 ? 2 : read_ahead)) {}

    strings_stream_t(strings_stream_t&&) = default;
    strings_stream_t& operator=(strings_stream_t&&) = default;
    strings_stream_t(strings_stream_t const&) = delete;
    strings_stream_t& operator=(strings_stream_t const&) = delete;

    /** Restarts the walk at @p start, which is returned too when the collection holds it. */
    status_t seek(value_view_t start) noexcept {
        fetched_ = {};
        fetched_offset_ = 0;
        resume_ = start;
        stage_ = walk_stage_t::fresh_k;
        return prefetch();
    }

    /** Restarts the walk at the first string of the prefix. */
    status_t seek_to_first() noexcept { return seek(prefix_); }

    status_t advance() noexcept {
        if (fetched_offset_ + 1 >= fetched_.size()) return prefetch();
        ++fetched_offset_;
        return {};
    }

    /** Advances like @c advance, but a failed fetch ends the walk since a reference carries no status. */
    strings_stream_t& operator++() noexcept {
        if (advance().succeeded()) return *this;
        fetched_ = {};
        fetched_offset_ = 0;
        stage_ = walk_stage_t::exhausted_k;
        return *this;
    }

    std::string_view string() const noexcept { return fetched_[fetched_offset_]; }
    std::string_view operator*() const noexcept { return string(); }

    /** The whole page in hand, passed strings included, which the next @c advance leaves behind. */
    joined_strs_t strings_batch() noexcept {
        fetched_offset_ = fetched_.size();
        return fetched_;
    }

    bool is_end() const noexcept { return stage_ == walk_stage_t::exhausted_k && fetched_offset_ >= fetched_.size(); }

    bool operator==(strings_stream_t const& other) const noexcept {
        if (collection_ != other.collection_) return false;
        if (is_end() || other.is_end()) return is_end() == other.is_end();
        return string() == other.string();
    }
};

#pragma endregion Streams

#pragma region Collections

/**
 *  @brief A handle to one collection of arbitrary bytes keyed by variable-length strings, cheap to construct.
 *
 *  It addresses either the latest state or the view of a transaction or snapshot, reads into one arena and
 *  so serves one thread at a time, and never outlives its database. A copy stays bound to the same
 *  transaction but starts with an arena of its own. Strings carry explicit lengths, so a zero byte inside
 *  one is stored like any other, and the empty string is an ordinary key.
 */
class strings_collection_t {

    ustore_database_t database_ {nullptr};
    ustore_collection_id_t collection_ {ustore_collection_main_k};
    ustore_transaction_t transaction_ {nullptr};
    ustore_snapshot_t snapshot_ {};
    any_arena_t arena_ {nullptr};

    status_t write(std::span<value_view_t const> texts, std::span<value_view_t const> contents,
                   flush_t flush) noexcept {
        if (texts.empty()) return {};

        ustore_error_t error {};
        ustore_strings_write_t write {};
        write.db = database_;
        write.error = &error;
        write.transaction = transaction_;
        write.arena = arena_.member_ptr();
        write.options = to_options(flush);
        write.tasks_count = texts.size();
        write.collections = &collection_;
        write.strings = reinterpret_cast<ustore_str_view_t const*>(texts.front().member_ptr());
        write.strings_stride = sizeof(value_view_t);
        write.strings_lengths = texts.front().member_length();
        write.strings_lengths_stride = sizeof(value_view_t);
        if (!contents.empty()) {
            write.values = contents.front().member_ptr();
            write.values_stride = sizeof(value_view_t);
            write.values_lengths = contents.front().member_length();
            write.values_lengths_stride = sizeof(value_view_t);
        }

        ustore_strings_write(&write);
        return status_t {error};
    }

    status_t drop_with(ustore_drop_mode_t mode) noexcept {
        ustore_error_t error {};
        ustore_collection_drop_t collection_drop {};
        collection_drop.db = database_;
        collection_drop.error = &error;
        collection_drop.collection = collection_;
        collection_drop.mode = mode;
        ustore_collection_drop(&collection_drop);
        return status_t {error};
    }

  public:
    using prefix_range_t = range<strings_stream_t>;

    strings_collection_t() noexcept : arena_(nullptr) {}
    strings_collection_t(ustore_database_t database, ustore_collection_id_t collection,
                         ustore_transaction_t transaction = nullptr, ustore_snapshot_t snapshot = {},
                         ustore_arena_t* arena = nullptr) noexcept
        : database_(database), collection_(collection), transaction_(transaction), snapshot_(snapshot),
          arena_(database_, arena) {}

    strings_collection_t(strings_collection_t&& other) noexcept
        : database_(other.database_), collection_(std::exchange(other.collection_, ustore_collection_main_k)),
          transaction_(std::exchange(other.transaction_, nullptr)), snapshot_(std::exchange(other.snapshot_, {})),
          arena_(std::exchange(other.arena_, {nullptr})) {}

    strings_collection_t& operator=(strings_collection_t&& other) noexcept {
        std::swap(database_, other.database_);
        std::swap(collection_, other.collection_);
        std::swap(transaction_, other.transaction_);
        std::swap(snapshot_, other.snapshot_);
        std::swap(arena_, other.arena_);
        return *this;
    }

    strings_collection_t(strings_collection_t const& other) noexcept
        : database_(other.database_), collection_(other.collection_), transaction_(other.transaction_),
          snapshot_(other.snapshot_), arena_(other.database_) {}

    strings_collection_t& operator=(strings_collection_t const& other) noexcept {
        database_ = other.database_;
        collection_ = other.collection_;
        transaction_ = other.transaction_;
        snapshot_ = other.snapshot_;
        arena_ = any_arena_t(other.database_);
        return *this;
    }

    operator ustore_collection_id_t() const noexcept { return collection_; }
    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }
    ustore_arena_t* member_arena() noexcept { return arena_.member_ptr(); }
    ustore_database_t db() const noexcept { return database_; }
    ustore_transaction_t txn() const noexcept { return transaction_; }
    ustore_snapshot_t snap() const noexcept { return snapshot_; }

    /** Upserts the value of one string, where a @p content without bytes removes it. */
    status_t assign(value_view_t text, value_view_t content, flush_t flush = flush_t::dont_flush_k) noexcept {
        return write({&text, 1}, {&content, 1}, flush);
    }

    /** Upserts the values of a batch of strings, where a content without bytes removes its string. */
    status_t assign(std::span<value_view_t const> texts, std::span<value_view_t const> contents,
                    flush_t flush = flush_t::dont_flush_k) noexcept {
        if (texts.size() != contents.size())
            return status_t {status_code_t::invalid_argument_k, "Every string of a batch needs its own value"};
        return write(texts, contents, flush);
    }

    status_t erase(value_view_t text, flush_t flush = flush_t::dont_flush_k) noexcept {
        return write({&text, 1}, {}, flush);
    }

    status_t erase(std::span<value_view_t const> texts, flush_t flush = flush_t::dont_flush_k) noexcept {
        return write(texts, {}, flush);
    }

    /** Reads the value of one string, which reads as a missing view when the collection lacks it. */
    expected<value_view_t> value(value_view_t text, watch_t watch = watch_t::watch_k) noexcept {
        auto [read, read_status] = values({&text, 1}, watch);
        if (read_status.failed()) return read_status;
        return read[0];
    }

    /** Reads the values of a batch of strings, each addressed by its own offset and length. */
    expected<embedded_blobs_t> values(std::span<value_view_t const> texts, watch_t watch = watch_t::watch_k) noexcept {
        if (texts.empty()) return embedded_blobs_t {};

        ustore_error_t error {};
        ustore_length_t* found_offsets = nullptr;
        ustore_length_t* found_lengths = nullptr;
        ustore_byte_t* found_values = nullptr;

        ustore_strings_read_t read {};
        read.db = database_;
        read.error = &error;
        read.transaction = transaction_;
        read.snapshot = snapshot_;
        read.arena = arena_.member_ptr();
        read.options = to_options(watch);
        read.tasks_count = texts.size();
        read.collections = &collection_;
        read.strings = reinterpret_cast<ustore_str_view_t const*>(texts.front().member_ptr());
        read.strings_stride = sizeof(value_view_t);
        read.strings_lengths = texts.front().member_length();
        read.strings_lengths_stride = sizeof(value_view_t);
        read.offsets = &found_offsets;
        read.lengths = &found_lengths;
        read.values = &found_values;

        ustore_strings_read(&read);
        if (error.status) return status_t {error};
        return embedded_blobs_t {static_cast<ustore_size_t>(texts.size()), found_offsets, found_lengths, found_values};
    }

    expected<bool> present(value_view_t text, watch_t watch = watch_t::watch_k) noexcept {
        auto [found, read_status] = value(text, watch);
        if (read_status.failed()) return read_status;
        return static_cast<bool>(found);
    }

    /**
     *  @brief Lists up to @p limit strings of @p prefix, in the order of their text.
     *
     *  The page starts at @p start, which it returns too when the collection holds it, so a walk that
     *  pages by hand skips the string it resumed from. The strings live in this handle's arena until
     *  the next call through it, and @c prefixed streams the pages instead.
     */
    expected<joined_strs_t> prefixed_page(value_view_t prefix, std::size_t limit,
                                          value_view_t start = value_view_t::make_empty(),
                                          watch_t watch = watch_t::watch_k) noexcept {
        ustore_error_t error {};
        auto const count_limit = static_cast<ustore_length_t>(limit);
        ustore_length_t* found_counts = nullptr;
        ustore_length_t* found_offsets = nullptr;
        ustore_char_t* found_strings = nullptr;

        ustore_strings_scan_t scan {};
        scan.db = database_;
        scan.error = &error;
        scan.transaction = transaction_;
        scan.snapshot = snapshot_;
        scan.arena = arena_.member_ptr();
        scan.options = to_options(watch);
        scan.tasks_count = 1;
        scan.collections = &collection_;
        scan.prefixes = reinterpret_cast<ustore_str_view_t const*>(prefix.member_ptr());
        scan.prefixes_lengths = prefix.member_length();
        scan.start_strings = reinterpret_cast<ustore_str_view_t const*>(start.member_ptr());
        scan.start_strings_lengths = start.member_length();
        scan.count_limits = &count_limit;
        scan.counts = &found_counts;
        scan.strings_offsets = &found_offsets;
        scan.strings = &found_strings;

        ustore_strings_scan(&scan);
        if (error.status) return status_t {error};
        return joined_strs_t {found_counts[0], found_offsets, found_strings};
    }

    /** Opens a stream over the strings of @p prefix, which outlives the stream. */
    expected<strings_stream_t> prefix_begin(
        value_view_t prefix = value_view_t::make_empty(),
        std::size_t read_ahead = strings_stream_t::default_read_ahead_k) const noexcept {
        strings_stream_t stream {database_, collection_, transaction_, snapshot_, prefix, read_ahead};
        if (status_t const sought = stream.seek_to_first(); sought.failed()) return sought;
        return stream;
    }

    /** Walks every string of @p prefix, which outlives the range, in the order of their text. */
    expected<prefix_range_t> prefixed(value_view_t prefix = value_view_t::make_empty(),
                                      std::size_t read_ahead = strings_stream_t::default_read_ahead_k) const noexcept {
        auto [begin, prefix_begin_status] = prefix_begin(prefix, read_ahead);
        if (prefix_begin_status.failed()) return prefix_begin_status;
        strings_stream_t end {database_, collection_, transaction_, snapshot_, prefix, read_ahead};
        return prefix_range_t {std::move(begin), std::move(end)};
    }

    /** Counts the strings of @p prefix by streaming through all of them. */
    expected<std::size_t> size(value_view_t prefix = value_view_t::make_empty()) const noexcept {
        auto [begin, prefix_begin_status] = prefix_begin(prefix);
        if (prefix_begin_status.failed()) return prefix_begin_status;

        std::size_t count = 0;
        for (strings_stream_t& stream = begin; !stream.is_end(); ++stream) ++count;
        return count;
    }

    /** Removes every string, keeping the collection. */
    status_t clear() noexcept { return drop_with(ustore_drop_keys_vals_k); }

    /** Removes the collection with all its strings. */
    status_t drop() noexcept { return drop_with(ustore_drop_keys_vals_handle_k); }
};

#pragma endregion Collections

} // namespace unum::ustore
