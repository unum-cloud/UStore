/**
 * @file blobs_range.hpp
 * @author Ashot Vardanian
 * @date 26 Jun 2022
 * @addtogroup Cpp
 *
 * @brief C++ bindings for "ustore/db.h".
 */

#pragma once
#include "ustore/ustore.h"
#include "ustore/cpp/ranges.hpp" // `indexed_range_gt`

namespace unum::ustore {

class keys_stream_t;
class pairs_stream_t;
struct blobs_range_t;
struct pairs_range_t;
struct size_range_t;
struct size_estimates_t;

/**
 * @brief Iterator (almost) over the keys in a single collection.
 *
 * Manages it's own memory and may be expressive to construct.
 * Prefer to `seek`, instead of re-creating such a stream.
 * Unlike classical iterators, keeps an internal state,
 * which makes it @b non copy-constructible!
 *
 * ## Class Specs
 * - Concurrency: Must be used from a single thread!
 * - Lifetime: @b Must live shorter then the collection it belongs to.
 * - Copyable: No.
 * - Exceptions: Never.
 */
class keys_stream_t {

    ustore_database_t db_ {nullptr};
    ustore_collection_t collection_ {ustore_collection_main_k};
    ustore_transaction_t txn_ {nullptr};

    arena_t arena_ {nullptr};
    ustore_length_t read_ahead_ {0};

    ustore_key_t next_min_key_ {std::numeric_limits<ustore_key_t>::min()};
    ptr_range_gt<ustore_key_t> fetched_keys_ {};
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
        scan.db = db_;
        scan.error = status.member_ptr();
        scan.transaction = txn_;
        scan.arena = arena_.member_ptr();
        scan.tasks_count = 1;
        scan.collections = &collection_;
        scan.start_keys = &next_min_key_;
        scan.count_limits = &read_ahead_;
        scan.counts = &found_counts;
        scan.keys = &found_keys;

        ustore_scan(&scan);
        if (!status)
            return status;

        fetched_keys_ = ptr_range_gt<ustore_key_t> {found_keys, found_keys + *found_counts};
        fetched_offset_ = 0;

        auto count = static_cast<ustore_length_t>(fetched_keys_.size());
        next_min_key_ = count < read_ahead_ ? ustore_key_unknown_k : fetched_keys_[count - 1] + 1;
        return {};
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = ustore_key_t;
    using pointer = ustore_key_t*;
    using reference = ustore_key_t&;

    static constexpr std::size_t default_read_ahead_k = 256;

    keys_stream_t(ustore_database_t db,
                  ustore_collection_t collection = ustore_collection_main_k,
                  std::size_t read_ahead = keys_stream_t::default_read_ahead_k,
                  ustore_transaction_t txn = nullptr) noexcept
        : db_(db), collection_(collection), txn_(txn), arena_(db), read_ahead_(static_cast<ustore_size_t>(read_ahead)) {}

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

        if (fetched_offset_ >= fetched_keys_.size() - 1)
            return prefetch();

        ++fetched_offset_;
        return {};
    }

    /**
     * ! Unlike the `advance()`, canonically returns a self-reference,
     * ! meaning that the error must be propagated in a different way.
     * ! So we promote this iterator to `end()`, once an error occurs.
     */
    keys_stream_t& operator++() noexcept {
        status_t status = advance();
        if (status)
            return *this;

        fetched_keys_ = {};
        fetched_offset_ = 0;
        next_min_key_ = ustore_key_unknown_k;
        return *this;
    }

    ustore_key_t key() const noexcept { return fetched_keys_[fetched_offset_]; }
    ustore_key_t operator*() const noexcept { return key(); }
    status_t seek_to_first() noexcept { return seek(std::numeric_limits<ustore_key_t>::min()); }
    status_t seek_to_next_batch() noexcept { return seek(next_min_key_); }

    /**
     * @brief Exposes all the fetched keys at once, including the passed ones.
     * Should be used with `seek_to_next_batch`. Next `advance` will do the same.
     */
    ptr_range_gt<ustore_key_t const> keys_batch() noexcept {
        fetched_offset_ = fetched_keys_.size();
        return {fetched_keys_.begin(), fetched_keys_.end()};
    }

    bool is_end() const noexcept {
        return next_min_key_ == ustore_key_unknown_k && fetched_offset_ >= fetched_keys_.size();
    }

    bool operator==(keys_stream_t const& other) const noexcept {
        if (collection_ != other.collection_)
            return false;
        if (is_end() || other.is_end())
            return is_end() == other.is_end();
        return key() == other.key();
    }

    bool operator!=(keys_stream_t const& other) const noexcept {
        if (collection_ != other.collection_)
            return true;
        if (is_end() || other.is_end())
            return is_end() != other.is_end();
        return key() != other.key();
    }
};

class pairs_stream_t {

    ustore_database_t db_ {nullptr};
    ustore_collection_t collection_ {ustore_collection_main_k};
    ustore_transaction_t txn_ {nullptr};

    arena_t arena_ {nullptr};
    ustore_length_t read_ahead_ {0};

    ustore_key_t next_min_key_ {std::numeric_limits<ustore_key_t>::min()};
    ptr_range_gt<ustore_key_t> fetched_keys_ {};
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
        scan.db = db_;
        scan.error = status.member_ptr();
        scan.transaction = txn_;
        scan.arena = arena_.member_ptr();
        scan.tasks_count = 1;
        scan.collections = &collection_;
        scan.start_keys = &next_min_key_;
        scan.count_limits = &read_ahead_;
        scan.counts = &found_counts;
        scan.keys = &found_keys;

        ustore_scan(&scan);
        if (!status)
            return status;

        fetched_keys_ = ptr_range_gt<ustore_key_t> {found_keys, found_keys + *found_counts};
        fetched_offset_ = 0;
        auto count = static_cast<ustore_size_t>(fetched_keys_.size());

        ustore_bytes_ptr_t found_vals {};
        ustore_length_t* found_offs {};
        ustore_read_t read {};
        read.db = db_;
        read.error = status.member_ptr();
        read.transaction = txn_;
        read.arena = arena_.member_ptr();
        read.options = ustore_option_dont_discard_memory_k;
        read.tasks_count = count;
        read.collections = &collection_;
        read.keys = found_keys;
        read.keys_stride = sizeof(ustore_key_t);
        read.offsets = &found_offs;
        read.values = &found_vals;

        ustore_read(&read);
        if (!status)
            return status;

        values_view_ = joined_blobs_t {count, found_offs, found_vals};
        values_iterator_ = values_view_.begin();
        next_min_key_ = count < read_ahead_ ? ustore_key_unknown_k : fetched_keys_[count - 1] + 1;
        return {};
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = std::pair<ustore_key_t, value_view_t>;

    static constexpr std::size_t default_read_ahead_k = 256;

    pairs_stream_t( //
        ustore_database_t db,
        ustore_collection_t collection = ustore_collection_main_k,
        std::size_t read_ahead = pairs_stream_t::default_read_ahead_k,
        ustore_transaction_t txn = nullptr) noexcept
        : db_(db), collection_(collection), txn_(txn), arena_(db_), read_ahead_(static_cast<ustore_size_t>(read_ahead)) {}

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

        if (fetched_offset_ >= fetched_keys_.size() - 1)
            return prefetch();

        ++fetched_offset_;
        ++values_iterator_;
        return {};
    }

    /**
     * ! Unlike the `advance()`, canonically returns a self-reference,
     * ! meaning that the error must be propagated in a different way.
     * ! So we promote this iterator to `end()`, once an error occurs.
     */
    pairs_stream_t& operator++() noexcept {
        status_t status = advance();
        if (status)
            return *this;

        fetched_keys_ = {};
        values_iterator_ = {};
        fetched_offset_ = 0;
        next_min_key_ = ustore_key_unknown_k;
        return *this;
    }

    ustore_key_t key() const noexcept { return fetched_keys_[fetched_offset_]; }
    value_view_t value() const noexcept { return *values_iterator_; }
    value_type item() const noexcept { return std::make_pair(fetched_keys_[fetched_offset_], *values_iterator_); }
    value_type operator*() const noexcept { return item(); }

    status_t seek_to_first() noexcept { return seek(std::numeric_limits<ustore_key_t>::min()); }
    status_t seek_to_next_batch() noexcept { return seek(next_min_key_); }

    /**
     * @brief Exposes all the fetched keys at once, including the passed ones.
     * Should be used with `seek_to_next_batch`. Next `advance` will do the same.
     */
    ptr_range_gt<ustore_key_t const> keys_batch() noexcept {
        fetched_offset_ = fetched_keys_.size();
        return {fetched_keys_.begin(), fetched_keys_.end()};
    }

    bool is_end() const noexcept {
        return next_min_key_ == ustore_key_unknown_k && fetched_offset_ >= fetched_keys_.size();
    }

    bool operator==(pairs_stream_t const& other) const noexcept {
        if (collection_ != other.collection_)
            return false;
        if (is_end() || other.is_end())
            return is_end() == other.is_end();
        return key() == other.key();
    }

    bool operator!=(pairs_stream_t const& other) const noexcept {
        if (collection_ != other.collection_)
            return true;
        if (is_end() || other.is_end())
            return is_end() != other.is_end();
        return key() != other.key();
    }
};

struct size_range_t {
    std::size_t min = 0;
    std::size_t max = 0;
};

struct size_estimates_t {
    size_range_t cardinality;
    size_range_t bytes_in_values;
    size_range_t bytes_on_disk;
};

/**
 * @brief Slice of keys or key-value-pairs stored in a single collection.
 * In Python terms: @b dict().items() or @b dict().keys().
 * Supports C++ range-based loops: `for (auto key : @b collection.items())`
 * It can also be use for @b loose cardinality and disk-usage estimates.
 *
 * ## Class Specs
 * - Concurrency: Thread-safe.
 * - Lifetime: @b Must live shorter then the collection it belongs to.
 * - Copyable: Yes.
 * - Exceptions: Possible on `begin()`, `end()` calls.
 *   That interface, however, may through exceptions.
 *   For exception-less interface use `keys_begin()`, `keys_end()`.
 */
class blobs_range_t {

    ustore_database_t db_;
    ustore_transaction_t txn_;
    ustore_snapshot_t snap_;
    ustore_collection_t collection_;
    ustore_key_t min_key_;
    ustore_key_t max_key_;

    template <typename stream_at>
    expected_gt<stream_at> make_stream( //
        ustore_key_t target,
        std::size_t read_ahead = keys_stream_t::default_read_ahead_k) noexcept {
        stream_at stream {db_, collection_, read_ahead, txn_};
        status_t status = stream.seek(target);
        return {std::move(status), std::move(stream)};
    }

  public:
    blobs_range_t(ustore_database_t db,
                  ustore_transaction_t txn = nullptr,
                  ustore_snapshot_t snap = 0,
                  ustore_collection_t collection = ustore_collection_main_k,
                  ustore_key_t min_key = std::numeric_limits<ustore_key_t>::min(),
                  ustore_key_t max_key = std::numeric_limits<ustore_key_t>::max()) noexcept
        : db_(db), txn_(txn), snap_(snap), collection_(collection), min_key_(min_key), max_key_(max_key) {}

    blobs_range_t(blobs_range_t&&) = default;
    blobs_range_t& operator=(blobs_range_t&&) = default;
    blobs_range_t(blobs_range_t const&) = default;
    blobs_range_t& operator=(blobs_range_t const&) = default;

    ustore_database_t db() const noexcept { return db_; }
    ustore_transaction_t txn() const noexcept { return txn_; }
    ustore_snapshot_t snap() const noexcept { return snap_; }
    ustore_collection_t collection() const noexcept { return collection_; }

    expected_gt<keys_stream_t> keys_begin(std::size_t read_ahead = keys_stream_t::default_read_ahead_k) noexcept {
        return make_stream<keys_stream_t>(min_key_, read_ahead);
    }

    expected_gt<keys_stream_t> keys_end() noexcept {
        return make_stream<keys_stream_t>(max_key_, max_key_ == std::numeric_limits<ustore_key_t>::max() ? 0u : 1u);
    }

    expected_gt<pairs_stream_t> pairs_begin(std::size_t read_ahead = pairs_stream_t::default_read_ahead_k) noexcept {
        return make_stream<pairs_stream_t>(min_key_, read_ahead);
    }

    expected_gt<pairs_stream_t> pairs_end() noexcept {
        return make_stream<pairs_stream_t>(max_key_, max_key_ == std::numeric_limits<ustore_key_t>::max() ? 0u : 1u);
    }

    expected_gt<size_estimates_t> size_estimates() noexcept {
        status_t status;
        arena_t arena(db_);
        auto a = arena.member_ptr();
        auto s = status.member_ptr();
        ustore_size_t* min_cardinalities = nullptr;
        ustore_size_t* max_cardinalities = nullptr;
        ustore_size_t* min_value_bytes = nullptr;
        ustore_size_t* max_value_bytes = nullptr;
        ustore_size_t* min_space_usages = nullptr;
        ustore_size_t* max_space_usages = nullptr;
        ustore_measure_t size {};
        size.db = db_;
        size.error = s;
        size.transaction = txn_;
        size.snapshot = snap_;
        size.arena = a;
        size.collections = &collection_;
        size.start_keys = &min_key_;
        size.end_keys = &max_key_;
        size.min_cardinalities = &min_cardinalities;
        size.max_cardinalities = &max_cardinalities;
        size.min_value_bytes = &min_value_bytes;
        size.max_value_bytes = &max_value_bytes;
        size.min_space_usages = &min_space_usages;
        size.max_space_usages = &max_space_usages;

        ustore_measure(&size);
        if (!status)
            return status;
        size_estimates_t result {{min_cardinalities[0], max_cardinalities[0]},
                                 {min_value_bytes[0], max_value_bytes[0]},
                                 {min_space_usages[0], max_space_usages[0]}};
        return result;
    }

    blobs_range_t& since(ustore_key_t min_key) noexcept {
        min_key_ = min_key;
        return *this;
    }
    blobs_range_t& until(ustore_key_t max_key) noexcept {
        max_key_ = max_key;
        return *this;
    }

    ustore_key_t min_key() noexcept { return min_key_; }
    ustore_key_t max_key() noexcept { return max_key_; }
};

struct keys_range_t {
    using iterator_type = keys_stream_t;
    using sample_t = ptr_range_gt<ustore_key_t>;

    blobs_range_t members;

    keys_stream_t begin() noexcept(false) { return members.keys_begin().throw_or_release(); }
    keys_stream_t end() noexcept(false) { return members.keys_end().throw_or_release(); }
    std::size_t size() noexcept(false) {
        auto it = begin();
        auto e = end();
        std::size_t count = 0;
        for (; it != e; ++it)
            ++count;
        return count;
    }

    expected_gt<sample_t> sample(std::size_t count, ustore_arena_t* arena) noexcept {
        ustore_length_t* found_counts = nullptr;
        ustore_key_t* found_keys = nullptr;
        status_t status;
        ustore_length_t c_count = static_cast<ustore_length_t>(count);
        ustore_collection_t c_collection = members.collection();
        ustore_sample_t sample {};
        sample.db = members.db();
        sample.error = status.member_ptr();
        sample.transaction = members.txn();
        sample.snapshot = members.snap();
        sample.arena = arena;
        sample.tasks_count = 1;
        sample.collections = &c_collection;
        sample.count_limits = &c_count;
        sample.counts = &found_counts;
        sample.keys = &found_keys;
        ustore_sample(&sample);

        if (!status)
            return std::move(status);

        return sample_t {found_keys, found_keys + found_counts[0]};
    }
};

struct pairs_range_t {
    using iterator_type = pairs_stream_t;

    blobs_range_t members;

    pairs_stream_t begin() noexcept(false) { return members.pairs_begin().throw_or_release(); }
    pairs_stream_t end() noexcept(false) { return members.pairs_end().throw_or_release(); }
    std::size_t size() noexcept(false) {
        auto it = begin();
        auto e = end();
        std::size_t count = 0;
        for (; it != e; ++it)
            ++count;
        return count;
    }
};

} // namespace unum::ustore
