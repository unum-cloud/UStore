/**
 *  @file include/ustore/strings_collection.hpp
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief A handle to one collection keyed by variable-length strings, with ordered prefix walks.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`, `std::ptrdiff_t`

#include <iterator>    // `std::forward_iterator_tag`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`, `std::swap`

#include "ustore/ranges.hpp" // `embedded_blobs_t`, `joined_strs_t`, `range`, `owning_view`, `read_spans_t`
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

/** Scans one page of up to @p limit strings of @p prefix from @p start, with their offsets into @p strings_offsets,
 *  which has room for @p limit plus one, and the strings themselves into @p arena. */
inline expected<joined_strs_t> scan_strings_page( //
    ustore_database_t database, ustore_view_t view, ustore_collection_id_t const& collection,
    value_view_t const& prefix, value_view_t const& start, ustore_length_t const& limit,
    ustore_length_t* strings_offsets, ustore_arena_t* arena) noexcept {

    ustore_length_t offsets[2] {};
    ustore_char_t* found_strings = nullptr;
    ustore_outcome_t outcome {};
    ustore_strings_scan_t scan {};
    scan.database = database;
    scan.outcome = &outcome;
    scan.view = view;
    scan.count = 1;
    scan.collections = &collection;
    scan.prefixes = reinterpret_cast<ustore_str_view_t const*>(prefix.member_ptr());
    scan.prefixes_lengths = prefix.member_length();
    scan.start_strings = reinterpret_cast<ustore_str_view_t const*>(start.member_ptr());
    scan.start_strings_lengths = start.member_length();
    scan.count_limits = &limit;
    scan.offsets = offsets;
    scan.strings_offsets = strings_offsets;
    scan.strings = &found_strings;
    scan.arena = arena;

    ustore_strings_scan(&scan);
    if (outcome.status != ustore_status_success_k) return status_t {outcome};
    return joined_strs_t {offsets[1] - offsets[0], strings_offsets + offsets[0], found_strings};
}

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
    ustore_view_t view_ {nullptr};
    value_view_t prefix_ {value_view_t::make_empty()};

    arena_t current_arena_;
    arena_t previous_arena_;
    ustore_length_t read_ahead_ {0};
    st::basic_vector<ustore_length_t> strings_offsets_ {};

    walk_stage_t stage_ {walk_stage_t::exhausted_k};
    value_view_t resume_ {value_view_t::make_empty()};
    joined_strs_t fetched_ {};
    std::size_t fetched_offset_ {0};

    strings_stream_t(ustore_database_t database, ustore_view_t view, ustore_collection_id_t collection,
                     value_view_t prefix, std::size_t read_ahead) noexcept
        : database_(database), collection_(collection), view_(view), prefix_(prefix),
          read_ahead_(static_cast<ustore_length_t>(read_ahead < 2 ? 2 : read_ahead)) {}

    status_t prefetch() noexcept {

        if (stage_ == walk_stage_t::exhausted_k) {
            ++fetched_offset_;
            return {};
        }

        // The page in hand holds the string the next scan resumes from, so the arenas alternate
        walk_stage_t const previous_stage = stage_;
        std::swap(current_arena_, previous_arena_);

        auto [page, scan_status] = scan_strings_page(database_, view_, collection_, prefix_, resume_, read_ahead_,
                                                     strings_offsets_.data(), current_arena_.member_ptr());
        if (scan_status.failed()) return scan_status;

        std::size_t const found = page.size();
        fetched_ = page;
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
    strings_stream_t(strings_stream_t&&) noexcept = default;
    strings_stream_t& operator=(strings_stream_t&&) noexcept = default;
    strings_stream_t(strings_stream_t const&) = delete;
    strings_stream_t& operator=(strings_stream_t const&) = delete;

    /** Opens a walk over the strings of @p prefix in @p collection, fetching @p read_ahead of them a page. */
    static expected<strings_stream_t> make(ustore_database_t database, ustore_view_t view,
                                           ustore_collection_id_t collection, value_view_t prefix,
                                           std::size_t read_ahead = default_read_ahead_k) noexcept {
        strings_stream_t stream {database, view, collection, prefix, read_ahead};
        if (st::failed(stream.strings_offsets_.resize(stream.read_ahead_ + 1)))
            return status_t {status_code_t::out_of_memory_k, "Failed to allocate a page of strings"};
        if (status_t const sought = stream.seek_to_first(); sought.failed()) return sought;
        return stream;
    }

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
        if (is_end() || other.is_end()) return is_end() == other.is_end();
        if (collection_ != other.collection_) return false;
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
    any_arena_t arena_;

    /** The view reads through this handle see, watching the buckets they read inside a transaction. */
    ustore_view_t view(ustore_watch_t watch) const noexcept { return view_through(transaction_, snapshot_, watch); }

    /** Reads the values of @p texts into @p spans, with their tape in this handle's arena. */
    expected<embedded_blobs_t> read(std::span<value_view_t const> texts, read_spans_t const& spans,
                                    ustore_watch_t watch) noexcept {
        if (texts.empty()) return embedded_blobs_t {};

        ustore_byte_t* found_values = nullptr;
        ustore_outcome_t outcome {};
        ustore_strings_read_t read {};
        read.database = database_;
        read.outcome = &outcome;
        read.view = view(watch);
        read.count = texts.size();
        read.collections = &collection_;
        read.strings = reinterpret_cast<ustore_str_view_t const*>(texts.front().member_ptr());
        read.strings_stride = sizeof(value_view_t);
        read.strings_lengths = texts.front().member_length();
        read.strings_lengths_stride = sizeof(value_view_t);
        read.presences = spans.presences;
        read.offsets = spans.offsets;
        read.values = &found_values;
        read.arena = arena_.member_ptr();

        ustore_strings_read(&read);
        if (outcome.status != ustore_status_success_k) return status_t {outcome};
        spans.derive_lengths(texts.size());
        return embedded_blobs_t {texts.size(), spans.offsets, spans.lengths, found_values};
    }

    status_t drop_with(ustore_drop_mode_t mode) noexcept {
        ustore_outcome_t outcome {};
        ustore_collection_drop_t collection_drop {};
        collection_drop.database = database_;
        collection_drop.outcome = &outcome;
        collection_drop.collection = collection_;
        collection_drop.mode = mode;
        ustore_collection_drop(&collection_drop);
        return status_t {outcome};
    }

  public:
    using prefix_range_t = range<strings_stream_t>;

    strings_collection_t() noexcept = default;
    strings_collection_t(ustore_database_t database, ustore_collection_id_t collection,
                         ustore_transaction_t transaction = nullptr, ustore_snapshot_t snapshot = {},
                         ustore_arena_t* arena = nullptr) noexcept
        : database_(database), collection_(collection), transaction_(transaction), snapshot_(snapshot), arena_(arena) {}

    operator ustore_collection_id_t() const noexcept { return collection_; }
    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }
    ustore_arena_t* member_arena() noexcept { return arena_.member_ptr(); }

    /** Upserts the value of one string. */
    status_t assign(value_view_t text, value_view_t content,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return assign({&text, 1}, {&content, 1}, durability);
    }

    /** Upserts the values of a batch of strings. */
    status_t assign(std::span<value_view_t const> texts, std::span<value_view_t const> contents,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        if (texts.size() != contents.size())
            return status_t {status_code_t::invalid_argument_k, "Every string of a batch needs its own value"};
        if (texts.empty()) return {};

        ustore_outcome_t outcome {};
        ustore_strings_write_t write {};
        write.database = database_;
        write.outcome = &outcome;
        write.transaction = transaction_;
        write.count = texts.size();
        write.collections = &collection_;
        write.strings = reinterpret_cast<ustore_str_view_t const*>(texts.front().member_ptr());
        write.strings_stride = sizeof(value_view_t);
        write.strings_lengths = texts.front().member_length();
        write.strings_lengths_stride = sizeof(value_view_t);
        write.values_lengths = contents.front().member_length();
        write.values_lengths_stride = sizeof(value_view_t);
        write.values = contents.front().member_ptr();
        write.values_stride = sizeof(value_view_t);
        write.durability = durability;

        ustore_strings_write(&write);
        return status_t {outcome};
    }

    status_t erase(value_view_t text, ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return erase({&text, 1}, durability);
    }

    status_t erase(std::span<value_view_t const> texts,
                   ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        if (texts.empty()) return {};

        ustore_outcome_t outcome {};
        ustore_strings_remove_t remove {};
        remove.database = database_;
        remove.outcome = &outcome;
        remove.transaction = transaction_;
        remove.count = texts.size();
        remove.collections = &collection_;
        remove.strings = reinterpret_cast<ustore_str_view_t const*>(texts.front().member_ptr());
        remove.strings_stride = sizeof(value_view_t);
        remove.strings_lengths = texts.front().member_length();
        remove.strings_lengths_stride = sizeof(value_view_t);
        remove.durability = durability;

        ustore_strings_remove(&remove);
        return status_t {outcome};
    }

    /** Reads the value of one string, which reads as a missing view when the collection lacks it. */
    expected<value_view_t> value(value_view_t text, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        alignas(ustore_length_t) std::byte single[read_spans_t::bytes(1)] {};
        auto [found, read_status] = read({&text, 1}, read_spans_t::carve(single, 1), watch);
        if (read_status.failed()) return read_status;
        return found[0];
    }

    /** Reads the values of a batch of strings, each addressed by its own offset and length, which the result owns. */
    expected<owning_view<embedded_blobs_t>> values(std::span<value_view_t const> texts,
                                                   ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        st::basic_vector<std::byte> buffer;
        if (st::failed(buffer.resize(read_spans_t::bytes(texts.size()))))
            return status_t {status_code_t::out_of_memory_k, "Failed to allocate the spans of a read"};
        auto [found, read_status] = read(texts, read_spans_t::carve(buffer.data(), texts.size()), watch);
        if (read_status.failed()) return read_status;
        return owning_view<embedded_blobs_t> {found, std::move(buffer)};
    }

    expected<bool> present(value_view_t text, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        auto [found, read_status] = value(text, watch);
        if (read_status.failed()) return read_status;
        return static_cast<bool>(found);
    }

    /**
     *  @brief Lists up to @p limit strings of @p prefix, in the order of their text.
     *
     *  The page starts at @p start, which it returns too when the collection holds it, so a walk that
     *  pages by hand skips the string it resumed from. The page owns its offsets, while the strings live in
     *  this handle's arena until the next call through it, and @c prefixed streams the pages instead.
     */
    expected<owning_view<joined_strs_t>> prefixed_page(value_view_t prefix, std::size_t limit,
                                                       value_view_t start = value_view_t::make_empty(),
                                                       ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        st::basic_vector<std::byte> buffer;
        if (st::failed(buffer.resize((limit + 1) * sizeof(ustore_length_t))))
            return status_t {status_code_t::out_of_memory_k, "Failed to allocate a page of strings"};
        ustore_length_t const count_limit = static_cast<ustore_length_t>(limit);
        auto [page, scan_status] = scan_strings_page(database_, view(watch), collection_, prefix, start, count_limit,
                                                     reinterpret_cast<ustore_length_t*>(buffer.data()),
                                                     arena_.member_ptr());
        if (scan_status.failed()) return scan_status;
        return owning_view<joined_strs_t> {page, std::move(buffer)};
    }

    /** Opens a stream over the strings of @p prefix, which outlives the stream. */
    expected<strings_stream_t> prefix_begin(
        value_view_t prefix = value_view_t::make_empty(),
        std::size_t read_ahead = strings_stream_t::default_read_ahead_k) const noexcept {
        return strings_stream_t::make(database_, view(ustore_watch_keys_k), collection_, prefix, read_ahead);
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

    /** Removes every string, keeping the collection. */
    status_t clear() noexcept { return drop_with(ustore_drop_keys_vals_k); }

    /** Removes the collection with all its strings. */
    status_t drop() noexcept { return drop_with(ustore_drop_keys_vals_handle_k); }
};

#pragma endregion Collections

} // namespace unum::ustore
