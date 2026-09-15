/**
 *  @file include/ustore/blobs_range.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief Paginated streams and slices over the keys and values of one binary collection.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::ptrdiff_t`

#include <iterator> // `std::forward_iterator_tag`
#include <limits>   // `std::numeric_limits`
#include <utility>  // `std::pair`, `std::move`

#include <ustore/ranges.hpp> // `ptr_range`, `joined_blobs_t`
#include <ustore/status.hpp> // `status_t`, `expected`
#include <ustore/types.hpp>  // `arena_t`
#include <ustore/ustore.h>

namespace unum::ustore {

/** A stream over the keys of one collection that fetches them in pages.
 *  It keeps its paging state, so it moves but never copies, and prefers @c seek to being rebuilt. */
class keys_stream_t {

    ustore_database_t database_ {nullptr};
    ustore_collection_t collection_ {ustore_collection_main_k};
    ustore_transaction_t transaction_ {nullptr};

    arena_t arena_ {nullptr};
    ustore_length_t read_ahead_ {0};

    ustore_key_t next_min_key_ {std::numeric_limits<ustore_key_t>::min()};
    ptr_range<ustore_key_t> fetched_keys_ {};
    std::size_t fetched_offset_ {0};

    status_t prefetch() noexcept {

        if (next_min_key_ == ustore_key_unknown_k) {
            ++fetched_offset_;
            return {};
        }

        ustore_length_t* found_counts = nullptr;
        ustore_key_t* found_keys = nullptr;

        status_t status;
        ustore_scan_t scan {};
        scan.db = database_;
        scan.error = status.member_ptr();
        scan.transaction = transaction_;
        scan.arena = arena_.member_ptr();
        scan.tasks_count = 1;
        scan.collections = &collection_;
        scan.start_keys = &next_min_key_;
        scan.count_limits = &read_ahead_;
        scan.counts = &found_counts;
        scan.keys = &found_keys;

        ustore_scan(&scan);
        if (!status) return status;

        fetched_keys_ = ptr_range<ustore_key_t> {found_keys, found_keys + *found_counts};
        fetched_offset_ = 0;

        auto const count = static_cast<ustore_length_t>(fetched_keys_.size());
        next_min_key_ = count < read_ahead_ ? ustore_key_unknown_k : fetched_keys_[count - 1] + 1;
        return {};
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = ustore_key_t;

    static constexpr std::size_t default_read_ahead_k = 256;

    keys_stream_t(ustore_database_t database, ustore_collection_t collection = ustore_collection_main_k,
                  std::size_t read_ahead = keys_stream_t::default_read_ahead_k,
                  ustore_transaction_t transaction = nullptr) noexcept
        : database_(database), collection_(collection), transaction_(transaction), arena_(database),
          read_ahead_(static_cast<ustore_length_t>(read_ahead)) {}

    keys_stream_t(keys_stream_t&&) = default;
    keys_stream_t& operator=(keys_stream_t&&) = default;
    keys_stream_t(keys_stream_t const&) = delete;
    keys_stream_t& operator=(keys_stream_t const&) = delete;

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
        if (status_t const status = advance(); status) return *this;
        fetched_keys_ = {};
        fetched_offset_ = 0;
        next_min_key_ = ustore_key_unknown_k;
        return *this;
    }

    ustore_key_t key() const noexcept { return fetched_keys_[fetched_offset_]; }
    ustore_key_t operator*() const noexcept { return key(); }
    status_t seek_to_first() noexcept { return seek(std::numeric_limits<ustore_key_t>::min()); }
    status_t seek_to_next_batch() noexcept { return seek(next_min_key_); }

    /** Exposes the whole fetched page, passed keys included, for use with @c seek_to_next_batch. */
    ptr_range<ustore_key_t const> keys_batch() noexcept {
        fetched_offset_ = fetched_keys_.size();
        return {fetched_keys_.begin(), fetched_keys_.end()};
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

/** A stream over the keys and values of one collection that fetches them in pages. */
class pairs_stream_t {

    ustore_database_t database_ {nullptr};
    ustore_collection_t collection_ {ustore_collection_main_k};
    ustore_transaction_t transaction_ {nullptr};

    arena_t arena_ {nullptr};
    ustore_length_t read_ahead_ {0};

    ustore_key_t next_min_key_ {std::numeric_limits<ustore_key_t>::min()};
    ptr_range<ustore_key_t> fetched_keys_ {};
    joined_blobs_t values_view_ {};
    joined_blobs_iterator_t values_iterator_ {};
    std::size_t fetched_offset_ {0};

    status_t prefetch() noexcept {

        if (next_min_key_ == ustore_key_unknown_k) {
            ++fetched_offset_;
            return {};
        }

        ustore_length_t* found_counts = nullptr;
        ustore_key_t* found_keys = nullptr;
        status_t status;
        ustore_scan_t scan {};
        scan.db = database_;
        scan.error = status.member_ptr();
        scan.transaction = transaction_;
        scan.arena = arena_.member_ptr();
        scan.tasks_count = 1;
        scan.collections = &collection_;
        scan.start_keys = &next_min_key_;
        scan.count_limits = &read_ahead_;
        scan.counts = &found_counts;
        scan.keys = &found_keys;

        ustore_scan(&scan);
        if (!status) return status;

        fetched_keys_ = ptr_range<ustore_key_t> {found_keys, found_keys + *found_counts};
        fetched_offset_ = 0;
        auto const count = static_cast<ustore_size_t>(fetched_keys_.size());

        ustore_bytes_ptr_t found_values {};
        ustore_length_t* found_offsets {};
        ustore_read_t read {};
        read.db = database_;
        read.error = status.member_ptr();
        read.transaction = transaction_;
        read.arena = arena_.member_ptr();
        read.options = ustore_option_dont_discard_memory_k;
        read.tasks_count = count;
        read.collections = &collection_;
        read.keys = found_keys;
        read.keys_stride = sizeof(ustore_key_t);
        read.offsets = &found_offsets;
        read.values = &found_values;

        ustore_read(&read);
        if (!status) return status;

        values_view_ = joined_blobs_t {count, found_offsets, found_values};
        values_iterator_ = values_view_.begin();
        next_min_key_ = count < read_ahead_ ? ustore_key_unknown_k : fetched_keys_[count - 1] + 1;
        return {};
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = std::pair<ustore_key_t, value_view_t>;

    static constexpr std::size_t default_read_ahead_k = 256;

    pairs_stream_t(ustore_database_t database, ustore_collection_t collection = ustore_collection_main_k,
                   std::size_t read_ahead = pairs_stream_t::default_read_ahead_k,
                   ustore_transaction_t transaction = nullptr) noexcept
        : database_(database), collection_(collection), transaction_(transaction), arena_(database),
          read_ahead_(static_cast<ustore_length_t>(read_ahead)) {}

    pairs_stream_t(pairs_stream_t&&) = default;
    pairs_stream_t& operator=(pairs_stream_t&&) = default;
    pairs_stream_t(pairs_stream_t const&) = delete;
    pairs_stream_t& operator=(pairs_stream_t const&) = delete;

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
        if (status_t const status = advance(); status) return *this;
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

    status_t seek_to_first() noexcept { return seek(std::numeric_limits<ustore_key_t>::min()); }
    status_t seek_to_next_batch() noexcept { return seek(next_min_key_); }

    /** Exposes the whole fetched page, passed keys included, for use with @c seek_to_next_batch. */
    ptr_range<ustore_key_t const> keys_batch() noexcept {
        fetched_offset_ = fetched_keys_.size();
        return {fetched_keys_.begin(), fetched_keys_.end()};
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
    ustore_transaction_t transaction_;
    ustore_snapshot_t snapshot_;
    ustore_collection_t collection_;
    ustore_key_t min_key_;
    ustore_key_t max_key_;

    template <typename stream_>
    expected<stream_> make_stream(ustore_key_t target, std::size_t read_ahead) noexcept {
        stream_ stream {database_, collection_, read_ahead, transaction_};
        status_t status = stream.seek(target);
        return {std::move(status), std::move(stream)};
    }

  public:
    blobs_range_t(ustore_database_t database, ustore_transaction_t transaction = nullptr,
                  ustore_snapshot_t snapshot = 0, ustore_collection_t collection = ustore_collection_main_k,
                  ustore_key_t min_key = std::numeric_limits<ustore_key_t>::min(),
                  ustore_key_t max_key = std::numeric_limits<ustore_key_t>::max()) noexcept
        : database_(database), transaction_(transaction), snapshot_(snapshot), collection_(collection),
          min_key_(min_key), max_key_(max_key) {}

    ustore_database_t db() const noexcept { return database_; }
    ustore_transaction_t txn() const noexcept { return transaction_; }
    ustore_snapshot_t snap() const noexcept { return snapshot_; }
    ustore_collection_t collection() const noexcept { return collection_; }

    expected<keys_stream_t> keys_begin(std::size_t read_ahead = keys_stream_t::default_read_ahead_k) noexcept {
        return make_stream<keys_stream_t>(min_key_, read_ahead);
    }

    expected<keys_stream_t> keys_end() noexcept {
        return make_stream<keys_stream_t>(max_key_, max_key_ == std::numeric_limits<ustore_key_t>::max() ? 0u : 1u);
    }

    expected<pairs_stream_t> pairs_begin(std::size_t read_ahead = pairs_stream_t::default_read_ahead_k) noexcept {
        return make_stream<pairs_stream_t>(min_key_, read_ahead);
    }

    expected<pairs_stream_t> pairs_end() noexcept {
        return make_stream<pairs_stream_t>(max_key_, max_key_ == std::numeric_limits<ustore_key_t>::max() ? 0u : 1u);
    }

    expected<size_estimates_t> size_estimates() noexcept {
        status_t status;
        arena_t arena(database_);
        ustore_size_t* min_cardinalities = nullptr;
        ustore_size_t* max_cardinalities = nullptr;
        ustore_size_t* min_value_bytes = nullptr;
        ustore_size_t* max_value_bytes = nullptr;
        ustore_size_t* min_space_usages = nullptr;
        ustore_size_t* max_space_usages = nullptr;

        ustore_measure_t measure {};
        measure.db = database_;
        measure.error = status.member_ptr();
        measure.transaction = transaction_;
        measure.snapshot = snapshot_;
        measure.arena = arena.member_ptr();
        measure.tasks_count = 1;
        measure.collections = &collection_;
        measure.start_keys = &min_key_;
        measure.end_keys = &max_key_;
        measure.min_cardinalities = &min_cardinalities;
        measure.max_cardinalities = &max_cardinalities;
        measure.min_value_bytes = &min_value_bytes;
        measure.max_value_bytes = &max_value_bytes;
        measure.min_space_usages = &min_space_usages;
        measure.max_space_usages = &max_space_usages;

        ustore_measure(&measure);
        if (!status) return std::move(status);
        return size_estimates_t {{min_cardinalities[0], max_cardinalities[0]},
                                 {min_value_bytes[0], max_value_bytes[0]},
                                 {min_space_usages[0], max_space_usages[0]}};
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
    using sample_t = ptr_range<ustore_key_t>;

    blobs_range_t members;

    /** Counts the keys by streaming through all of them. */
    expected<std::size_t> size() noexcept {
        auto maybe_begin = members.keys_begin();
        if (!maybe_begin) return maybe_begin.release_status();
        auto maybe_end = members.keys_end();
        if (!maybe_end) return maybe_end.release_status();

        std::size_t count = 0;
        for (keys_stream_t& stream = *maybe_begin; stream != *maybe_end; ++stream) ++count;
        return count;
    }

    expected<sample_t> sample(std::size_t count, ustore_arena_t* arena) noexcept {
        ustore_length_t* found_counts = nullptr;
        ustore_key_t* found_keys = nullptr;
        status_t status;
        auto const count_limit = static_cast<ustore_length_t>(count);
        ustore_collection_t const collection = members.collection();

        ustore_sample_t sample {};
        sample.db = members.db();
        sample.error = status.member_ptr();
        sample.transaction = members.txn();
        sample.snapshot = members.snap();
        sample.arena = arena;
        sample.tasks_count = 1;
        sample.collections = &collection;
        sample.count_limits = &count_limit;
        sample.counts = &found_counts;
        sample.keys = &found_keys;
        ustore_sample(&sample);

        if (!status) return std::move(status);
        return sample_t {found_keys, found_keys + found_counts[0]};
    }
};

/** The key-value pairs of a collection slice, counted by streaming. */
struct pairs_range_t {
    blobs_range_t members;

    /** Counts the pairs by streaming through all of them. */
    expected<std::size_t> size() noexcept {
        auto maybe_begin = members.pairs_begin();
        if (!maybe_begin) return maybe_begin.release_status();
        auto maybe_end = members.pairs_end();
        if (!maybe_end) return maybe_end.release_status();

        std::size_t count = 0;
        for (pairs_stream_t& stream = *maybe_begin; stream != *maybe_end; ++stream) ++count;
        return count;
    }
};

} // namespace unum::ustore
