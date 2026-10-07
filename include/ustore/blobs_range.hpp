/**
 *  @file include/ustore/blobs_range.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief Paginated streams and slices over the keys and values of one binary collection.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`, `std::ptrdiff_t`

#include <iterator> // `std::forward_iterator_tag`
#include <span>     // `std::span`
#include <utility>  // `std::pair`, `std::move`

#include "ustore/ranges.hpp" // `joined_blobs_t`, `owning_view`
#include "ustore/status.hpp" // `status_t`, `expected`
#include "ustore/types.hpp"  // `arena_t`
#include "ustore/ustore.h"

namespace unum::ustore {

/** Scans one page of up to @p limit keys of @p collection from @p start into @p keys, answering the keys found. */
inline expected<std::span<ustore_key_t>> scan_page(ustore_database_t database, ustore_view_t view,
                                                   ustore_collection_id_t const& collection, ustore_key_t const& start,
                                                   ustore_length_t const& limit, ustore_key_t* keys) noexcept {
    ustore_length_t offsets[2] {};
    ustore_outcome_t outcome {};
    ustore_scan_t scan {};
    scan.database = database;
    scan.outcome = &outcome;
    scan.view = view;
    scan.count = 1;
    scan.collections = &collection;
    scan.start_keys = &start;
    scan.count_limits = &limit;
    scan.offsets = offsets;
    scan.keys = keys;
    ustore_scan(&scan);
    if (outcome.status != ustore_status_success_k) return status_t {outcome};
    return std::span<ustore_key_t> {keys + offsets[0], keys + offsets[1]};
}

/** A stream over the keys of one collection that fetches them in pages.
 *  It keeps its paging state and the page it fetched, so it moves but never copies, and prefers @c seek to being
 *  rebuilt. A stream made at the end holds no page, and a default-constructed one is at the end. */
class keys_stream_t {

    friend class graph_stream_t;

    ustore_database_t database_ {nullptr};
    ustore_collection_id_t collection_ {ustore_collection_main_k};
    ustore_view_t view_ {nullptr};

    ustore_length_t read_ahead_ {0};
    st::basic_vector<ustore_key_t> page_ {};

    ustore_key_t next_min_key_ {ustore_key_unknown_k};
    std::span<ustore_key_t> fetched_keys_ {};
    std::size_t fetched_offset_ {0};

    keys_stream_t(ustore_database_t database, ustore_view_t view, ustore_collection_id_t collection,
                  std::size_t read_ahead) noexcept
        : database_(database), collection_(collection), view_(view),
          read_ahead_(static_cast<ustore_length_t>(read_ahead)) {}

    status_t prefetch() noexcept {

        if (next_min_key_ == ustore_key_unknown_k) {
            ++fetched_offset_;
            return {};
        }

        auto [found_keys, scan_status] = scan_page(database_, view_, collection_, next_min_key_, read_ahead_,
                                                   page_.data());
        if (scan_status.failed()) return scan_status;

        fetched_keys_ = found_keys;
        fetched_offset_ = 0;

        auto const count = static_cast<ustore_length_t>(fetched_keys_.size());
        ustore_key_t const last_key = count ? fetched_keys_[count - 1] : ustore_key_unknown_k;
        next_min_key_ = count < read_ahead_ || last_key == ustore_key_unknown_k ? ustore_key_unknown_k
                                                                                : key_after(last_key);
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

    /** Opens a stream over the keys of @p collection from @p start, fetching @p read_ahead of them a page. */
    static expected<keys_stream_t> make(ustore_database_t database, ustore_view_t view,
                                        ustore_collection_id_t collection, ustore_key_t start,
                                        std::size_t read_ahead = default_read_ahead_k) noexcept {
        keys_stream_t stream {database, view, collection, read_ahead};
        if (start != ustore_key_unknown_k && st::failed(stream.page_.resize(read_ahead)))
            return status_t {status_code_t::out_of_memory_k, "Failed to allocate a page of keys"};
        if (status_t const sought = stream.seek(start); sought.failed()) return sought;
        return stream;
    }

    status_t seek(ustore_key_t key) noexcept {
        fetched_keys_ = {};
        fetched_offset_ = 0;
        next_min_key_ = key;
        return prefetch();
    }

    status_t advance() noexcept {
        if (fetched_offset_ >= fetched_keys_.size() - 1) return prefetch();
        ++fetched_offset_;
        return {};
    }

    /** Advances like @c advance, but a failed fetch ends the stream since a reference carries no status. */
    keys_stream_t& operator++() noexcept {
        if (advance().succeeded()) return *this;
        fetched_keys_ = {};
        fetched_offset_ = 0;
        next_min_key_ = ustore_key_unknown_k;
        return *this;
    }

    ustore_key_t key() const noexcept { return fetched_keys_[fetched_offset_]; }
    ustore_key_t operator*() const noexcept { return key(); }
    status_t seek_to_first() noexcept { return seek(smallest_key_k); }
    status_t seek_to_next_batch() noexcept { return seek(next_min_key_); }

    /** Exposes the whole fetched page, passed keys included, for use with @c seek_to_next_batch. */
    std::span<ustore_key_t const> keys_batch() noexcept {
        fetched_offset_ = fetched_keys_.size();
        return fetched_keys_;
    }

    bool is_end() const noexcept {
        return next_min_key_ == ustore_key_unknown_k && fetched_offset_ >= fetched_keys_.size();
    }

    bool operator==(keys_stream_t const& other) const noexcept {
        if (collection_ != other.collection_) return false;
        if (is_end() || other.is_end()) return is_end() == other.is_end();
        return key() == other.key();
    }
};

/** A stream over the keys and values of one collection that fetches them in pages. A stream made at the end holds no
 *  page, and a default-constructed one is at the end. */
class pairs_stream_t {

    ustore_database_t database_ {nullptr};
    ustore_collection_id_t collection_ {ustore_collection_main_k};
    ustore_view_t view_ {nullptr};

    arena_t arena_;
    ustore_length_t read_ahead_ {0};
    st::basic_vector<ustore_key_t> page_ {};
    st::basic_vector<ustore_length_t> offsets_ {};

    ustore_key_t next_min_key_ {ustore_key_unknown_k};
    std::span<ustore_key_t> fetched_keys_ {};
    joined_blobs_t values_view_ {};
    joined_blobs_iterator_t values_iterator_ {};
    std::size_t fetched_offset_ {0};

    pairs_stream_t(ustore_database_t database, ustore_view_t view, ustore_collection_id_t collection,
                   std::size_t read_ahead) noexcept
        : database_(database), collection_(collection), view_(view),
          read_ahead_(static_cast<ustore_length_t>(read_ahead)) {}

    status_t prefetch() noexcept {

        if (next_min_key_ == ustore_key_unknown_k) {
            ++fetched_offset_;
            return {};
        }

        auto [found_keys, scan_status] = scan_page(database_, view_, collection_, next_min_key_, read_ahead_,
                                                   page_.data());
        if (scan_status.failed()) return scan_status;

        fetched_keys_ = found_keys;
        fetched_offset_ = 0;
        ustore_size_t const count = fetched_keys_.size();

        ustore_byte_t* found_values = nullptr;
        ustore_outcome_t outcome {};
        ustore_read_t read {};
        read.database = database_;
        read.outcome = &outcome;
        read.view = view_;
        read.count = count;
        read.collections = &collection_;
        read.keys = fetched_keys_.data();
        read.keys_stride = sizeof(ustore_key_t);
        read.offsets = offsets_.data();
        read.values = &found_values;
        read.arena = arena_.member_ptr();

        ustore_read(&read);
        if (outcome.status != ustore_status_success_k) return status_t {outcome};

        values_view_ = joined_blobs_t {count, offsets_.data(), found_values};
        values_iterator_ = values_view_.begin();
        ustore_key_t const last_key = count ? fetched_keys_[count - 1] : ustore_key_unknown_k;
        next_min_key_ = count < read_ahead_ || last_key == ustore_key_unknown_k ? ustore_key_unknown_k
                                                                                : key_after(last_key);
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

    /** Opens a stream over the pairs of @p collection from @p start, fetching @p read_ahead of them a page. */
    static expected<pairs_stream_t> make(ustore_database_t database, ustore_view_t view,
                                         ustore_collection_id_t collection, ustore_key_t start,
                                         std::size_t read_ahead = default_read_ahead_k) noexcept {
        pairs_stream_t stream {database, view, collection, read_ahead};
        if (start != ustore_key_unknown_k &&
            (st::failed(stream.page_.resize(read_ahead)) || st::failed(stream.offsets_.resize(read_ahead + 1))))
            return status_t {status_code_t::out_of_memory_k, "Failed to allocate a page of pairs"};
        if (status_t const sought = stream.seek(start); sought.failed()) return sought;
        return stream;
    }

    status_t seek(ustore_key_t key) noexcept {
        fetched_keys_ = {};
        fetched_offset_ = 0;
        next_min_key_ = key;
        return prefetch();
    }

    status_t advance() noexcept {
        if (fetched_offset_ >= fetched_keys_.size() - 1) return prefetch();
        ++fetched_offset_;
        ++values_iterator_;
        return {};
    }

    /** Advances like @c advance, but a failed fetch ends the stream since a reference carries no status. */
    pairs_stream_t& operator++() noexcept {
        if (advance().succeeded()) return *this;
        fetched_keys_ = {};
        values_iterator_ = {};
        fetched_offset_ = 0;
        next_min_key_ = ustore_key_unknown_k;
        return *this;
    }

    ustore_key_t key() const noexcept { return fetched_keys_[fetched_offset_]; }
    value_view_t value() const noexcept { return *values_iterator_; }
    value_type item() const noexcept { return {fetched_keys_[fetched_offset_], *values_iterator_}; }
    value_type operator*() const noexcept { return item(); }

    status_t seek_to_first() noexcept { return seek(smallest_key_k); }
    status_t seek_to_next_batch() noexcept { return seek(next_min_key_); }

    /** Exposes the whole fetched page, passed keys included, for use with @c seek_to_next_batch. */
    std::span<ustore_key_t const> keys_batch() noexcept {
        fetched_offset_ = fetched_keys_.size();
        return fetched_keys_;
    }

    bool is_end() const noexcept {
        return next_min_key_ == ustore_key_unknown_k && fetched_offset_ >= fetched_keys_.size();
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

    ustore_database_t database_;
    ustore_view_t view_;
    ustore_collection_id_t collection_;
    ustore_key_t min_key_;
    ustore_key_t max_key_;

  public:
    blobs_range_t(ustore_database_t database, ustore_view_t view = nullptr,
                  ustore_collection_id_t collection = ustore_collection_main_k, ustore_key_t min_key = smallest_key_k,
                  ustore_key_t max_key = largest_key_k) noexcept
        : database_(database), view_(view), collection_(collection), min_key_(min_key), max_key_(max_key) {}

    ustore_database_t database() const noexcept { return database_; }
    ustore_view_t view() const noexcept { return view_; }
    ustore_collection_id_t collection() const noexcept { return collection_; }

    expected<keys_stream_t> keys_begin(std::size_t read_ahead = keys_stream_t::default_read_ahead_k) noexcept {
        return keys_stream_t::make(database_, view_, collection_, min_key_, read_ahead);
    }

    /** The first key at or past the upper bound, which an unbounded range never fetches. */
    expected<keys_stream_t> keys_end() noexcept {
        return keys_stream_t::make(database_, view_, collection_, max_key_, 1);
    }

    expected<pairs_stream_t> pairs_begin(std::size_t read_ahead = pairs_stream_t::default_read_ahead_k) noexcept {
        return pairs_stream_t::make(database_, view_, collection_, min_key_, read_ahead);
    }

    expected<pairs_stream_t> pairs_end() noexcept {
        return pairs_stream_t::make(database_, view_, collection_, max_key_, 1);
    }

    expected<size_estimates_t> size_estimates() noexcept {
        ustore_size_t min_cardinality = 0;
        ustore_size_t max_cardinality = 0;
        ustore_size_t min_value_bytes = 0;
        ustore_size_t max_value_bytes = 0;
        ustore_size_t min_space_usage = 0;
        ustore_size_t max_space_usage = 0;

        ustore_outcome_t outcome {};
        ustore_measure_t measure {};
        measure.database = database_;
        measure.outcome = &outcome;
        measure.view = view_;
        measure.count = 1;
        measure.collections = &collection_;
        measure.start_keys = &min_key_;
        measure.end_keys = &max_key_;
        measure.min_cardinalities = &min_cardinality;
        measure.min_cardinalities_stride = sizeof(ustore_size_t);
        measure.max_cardinalities = &max_cardinality;
        measure.max_cardinalities_stride = sizeof(ustore_size_t);
        measure.min_value_bytes = &min_value_bytes;
        measure.min_value_bytes_stride = sizeof(ustore_size_t);
        measure.max_value_bytes = &max_value_bytes;
        measure.max_value_bytes_stride = sizeof(ustore_size_t);
        measure.min_space_usages = &min_space_usage;
        measure.min_space_usages_stride = sizeof(ustore_size_t);
        measure.max_space_usages = &max_space_usage;
        measure.max_space_usages_stride = sizeof(ustore_size_t);

        ustore_measure(&measure);
        if (outcome.status != ustore_status_success_k) return status_t {outcome};
        return size_estimates_t {{min_cardinality, max_cardinality},
                                 {min_value_bytes, max_value_bytes},
                                 {min_space_usage, max_space_usage}};
    }

    blobs_range_t& since(ustore_key_t min_key) noexcept {
        min_key_ = min_key;
        return *this;
    }

    blobs_range_t& until(ustore_key_t max_key) noexcept {
        max_key_ = max_key;
        return *this;
    }

    ustore_key_t min_key() const noexcept { return min_key_; }
    ustore_key_t max_key() const noexcept { return max_key_; }
};

/** The keys of a collection slice, counted by streaming or sampled at random. */
struct keys_range_t {
    using sample_t = owning_view<std::span<ustore_key_t>>;

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

    /** Draws up to @p count keys at random into a buffer the result owns. */
    expected<sample_t> sample(std::size_t count) noexcept {
        st::basic_vector<std::byte> buffer;
        if (st::failed(buffer.resize(count * sizeof(ustore_key_t))))
            return status_t {status_code_t::out_of_memory_k, "Failed to allocate a sample of keys"};
        ustore_key_t* const keys = reinterpret_cast<ustore_key_t*>(buffer.data());
        ustore_length_t const count_limit = static_cast<ustore_length_t>(count);
        ustore_collection_id_t const collection = members.collection();
        ustore_length_t offsets[2] {};

        ustore_outcome_t outcome {};
        ustore_sample_t sample {};
        sample.database = members.database();
        sample.outcome = &outcome;
        sample.view = members.view();
        sample.count = 1;
        sample.collections = &collection;
        sample.count_limits = &count_limit;
        sample.offsets = offsets;
        sample.keys = keys;
        ustore_sample(&sample);

        if (outcome.status != ustore_status_success_k) return status_t {outcome};
        return sample_t {std::span<ustore_key_t> {keys + offsets[0], keys + offsets[1]}, std::move(buffer)};
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
