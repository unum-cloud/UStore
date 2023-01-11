/**
 * @file blobs_range.hpp
 * @author Ashot Vardanian
 * @date 26 Jun 2022
 * @addtogroup Cpp
 *
 * @brief C++ bindings for "ukv/db.h".
 */

#pragma once
#include "ukv/ukv.h"
#include "ukv/cpp/ranges.hpp" // `indexed_range_gt`

namespace unum::ukv {

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

    ukv_database_t db_ {nullptr};
    ukv_collection_t collection_ {ukv_collection_main_k};
    ukv_transaction_t txn_ {nullptr};

    arena_t arena_ {nullptr};
    ukv_length_t read_ahead_ {0};

    ukv_key_t next_min_key_ {std::numeric_limits<ukv_key_t>::min()};
    ptr_range_gt<ukv_key_t> fetched_keys_ {};
    std::size_t fetched_offset_ {0};

    status_t prefetch() noexcept {

        if (next_min_key_ == ukv_key_unknown_k)
            return {};

        ukv_length_t* found_counts = nullptr;
        ukv_key_t* found_keys = nullptr;

        status_t status;
        ukv_scan_t scan {
            .db = db_,
            .error = status.member_ptr(),
            .transaction = txn_,
            .arena = arena_.member_ptr(),
            .tasks_count = 1,
            .collections = &collection_,
            .start_keys = &next_min_key_,
            .count_limits = &read_ahead_,
            .counts = &found_counts,
            .keys = &found_keys,
        };

        ukv_scan(&scan);
        if (!status)
            return status;

        fetched_keys_ = ptr_range_gt<ukv_key_t> {found_keys, found_keys + *found_counts};
        fetched_offset_ = 0;

        auto count = static_cast<ukv_length_t>(fetched_keys_.size());
        next_min_key_ = count < read_ahead_ ? ukv_key_unknown_k : fetched_keys_[count - 1] + 1;
        return {};
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = ukv_key_t;
    using pointer = ukv_key_t*;
    using reference = ukv_key_t&;

    static constexpr std::size_t default_read_ahead_k = 256;

    keys_stream_t(ukv_database_t db,
                  ukv_collection_t collection = ukv_collection_main_k,
                  std::size_t read_ahead = keys_stream_t::default_read_ahead_k,
                  ukv_transaction_t txn = nullptr)
        : db_(db), collection_(collection), txn_(txn), arena_(db), read_ahead_(static_cast<ukv_size_t>(read_ahead)) {}

    keys_stream_t(keys_stream_t&&) = default;
    keys_stream_t& operator=(keys_stream_t&&) = default;

    keys_stream_t(keys_stream_t const&) = delete;
    keys_stream_t& operator=(keys_stream_t const&) = delete;

    status_t seek(ukv_key_t key) noexcept {
        fetched_keys_ = {};
        fetched_offset_ = 0;
        next_min_key_ = key;
        return prefetch();
    }

    status_t advance() noexcept {

        if (fetched_offset_ >= fetched_keys_.size())
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
        next_min_key_ = ukv_key_unknown_k;
        return *this;
    }

    ukv_key_t key() const noexcept { return fetched_keys_[fetched_offset_]; }
    ukv_key_t operator*() const noexcept { return key(); }
    status_t seek_to_first() noexcept { return seek(std::numeric_limits<ukv_key_t>::min()); }
    status_t seek_to_next_batch() noexcept { return seek(next_min_key_); }

    /**
     * @brief Exposes all the fetched keys at once, including the passed ones.
     * Should be used with `seek_to_next_batch`. Next `advance` will do the same.
     */
    ptr_range_gt<ukv_key_t const> keys_batch() noexcept {
        fetched_offset_ = fetched_keys_.size();
        return {fetched_keys_.begin(), fetched_keys_.end()};
    }

    bool is_end() const noexcept {
        return next_min_key_ == ukv_key_unknown_k && fetched_offset_ >= fetched_keys_.size();
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

    ukv_database_t db_ {nullptr};
    ukv_collection_t collection_ {ukv_collection_main_k};
    ukv_transaction_t txn_ {nullptr};

    arena_t arena_ {nullptr};
    ukv_length_t read_ahead_ {0};

    ukv_key_t next_min_key_ {std::numeric_limits<ukv_key_t>::min()};
    ptr_range_gt<ukv_key_t> fetched_keys_ {};
    joined_blobs_t values_view_ {};
    joined_blobs_iterator_t values_iterator_ {};
    std::size_t fetched_offset_ {0};

    status_t prefetch() noexcept {

        if (next_min_key_ == ukv_key_unknown_k)
            return {};

        ukv_length_t* found_counts = nullptr;
        ukv_key_t* found_keys = nullptr;
        status_t status;
        ukv_scan_t scan {
            .db = db_,
            .error = status.member_ptr(),
            .transaction = txn_,
            .arena = arena_.member_ptr(),
            .tasks_count = 1,
            .collections = &collection_,
            .start_keys = &next_min_key_,
            .count_limits = &read_ahead_,
            .counts = &found_counts,
            .keys = &found_keys,
        };

        ukv_scan(&scan);
        if (!status)
            return status;

        fetched_keys_ = ptr_range_gt<ukv_key_t> {found_keys, found_keys + *found_counts};
        fetched_offset_ = 0;
        auto count = static_cast<ukv_size_t>(fetched_keys_.size());

        ukv_bytes_ptr_t found_vals = nullptr;
        ukv_length_t* found_offs = nullptr;
        ukv_read_t read {
            .db = db_,
            .error = status.member_ptr(),
            .transaction = txn_,
            .arena = arena_.member_ptr(),
            .options = ukv_option_dont_discard_memory_k,
            .tasks_count = count,
            .collections = &collection_,
            .keys = found_keys,
            .keys_stride = sizeof(ukv_key_t),
            .offsets = &found_offs,
            .values = &found_vals,
        };

        ukv_read(&read);
        if (!status)
            return status;

        values_view_ = joined_blobs_t {count, found_offs, found_vals};
        values_iterator_ = values_view_.begin();
        next_min_key_ = count < read_ahead_ ? ukv_key_unknown_k : fetched_keys_[count - 1] + 1;
        return {};
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = std::pair<ukv_key_t, value_view_t>;

    static constexpr std::size_t default_read_ahead_k = 256;

    pairs_stream_t( //
        ukv_database_t db,
        ukv_collection_t collection = ukv_collection_main_k,
        std::size_t read_ahead = pairs_stream_t::default_read_ahead_k,
        ukv_transaction_t txn = nullptr)
        : db_(db), collection_(collection), txn_(txn), arena_(db_), read_ahead_(static_cast<ukv_size_t>(read_ahead)) {}

    pairs_stream_t(pairs_stream_t&&) = default;
    pairs_stream_t& operator=(pairs_stream_t&&) = default;

    pairs_stream_t(pairs_stream_t const&) = delete;
    pairs_stream_t& operator=(pairs_stream_t const&) = delete;

    status_t seek(ukv_key_t key) noexcept {
        fetched_keys_ = {};
        fetched_offset_ = 0;
        next_min_key_ = key;
        return prefetch();
    }

    status_t advance() noexcept {

        if (fetched_offset_ >= fetched_keys_.size())
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
        next_min_key_ = ukv_key_unknown_k;
        return *this;
    }

    ukv_key_t key() const noexcept { return fetched_keys_[fetched_offset_]; }
    value_view_t value() const noexcept { return *values_iterator_; }
    value_type item() const noexcept { return std::make_pair(fetched_keys_[fetched_offset_], *values_iterator_); }
    value_type operator*() const noexcept { return item(); }

    status_t seek_to_first() noexcept { return seek(std::numeric_limits<ukv_key_t>::min()); }
    status_t seek_to_next_batch() noexcept { return seek(next_min_key_); }

    /**
     * @brief Exposes all the fetched keys at once, including the passed ones.
     * Should be used with `seek_to_next_batch`. Next `advance` will do the same.
     */
    ptr_range_gt<ukv_key_t const> keys_batch() noexcept {
        fetched_offset_ = fetched_keys_.size();
        return {fetched_keys_.begin(), fetched_keys_.end()};
    }

    bool is_end() const noexcept {
        return next_min_key_ == ukv_key_unknown_k && fetched_offset_ >= fetched_keys_.size();
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

    ukv_database_t db_;
    ukv_transaction_t txn_;
    ukv_collection_t collection_;
    ukv_key_t min_key_;
    ukv_key_t max_key_;

    template <typename stream_at>
    expected_gt<stream_at> make_stream( //
        ukv_key_t target,
        std::size_t read_ahead = keys_stream_t::default_read_ahead_k) noexcept {
        stream_at stream {db_, collection_, read_ahead, txn_};
        status_t status = stream.seek(target);
        return {std::move(status), std::move(stream)};
    }

  public:
    blobs_range_t(ukv_database_t db,
                  ukv_transaction_t txn = nullptr,
                  ukv_collection_t collection = ukv_collection_main_k,
                  ukv_key_t min_key = std::numeric_limits<ukv_key_t>::min(),
                  ukv_key_t max_key = std::numeric_limits<ukv_key_t>::max()) noexcept
        : db_(db), txn_(txn), collection_(collection), min_key_(min_key), max_key_(max_key) {}

    blobs_range_t(blobs_range_t&&) = default;
    blobs_range_t& operator=(blobs_range_t&&) = default;
    blobs_range_t(blobs_range_t const&) = default;
    blobs_range_t& operator=(blobs_range_t const&) = default;

    ukv_database_t db() const noexcept { return db_; }
    ukv_transaction_t txn() const noexcept { return txn_; }
    ukv_collection_t collection() const noexcept { return collection_; }

    expected_gt<keys_stream_t> keys_begin(std::size_t read_ahead = keys_stream_t::default_read_ahead_k) noexcept {
        return make_stream<keys_stream_t>(min_key_, read_ahead);
    }

    expected_gt<keys_stream_t> keys_end() noexcept {
        return make_stream<keys_stream_t>(max_key_, max_key_ == std::numeric_limits<ukv_key_t>::max() ? 0u : 1u);
    }

    expected_gt<pairs_stream_t> pairs_begin(std::size_t read_ahead = pairs_stream_t::default_read_ahead_k) noexcept {
        return make_stream<pairs_stream_t>(min_key_, read_ahead);
    }

    expected_gt<pairs_stream_t> pairs_end() noexcept {
        return make_stream<pairs_stream_t>(max_key_, max_key_ == std::numeric_limits<ukv_key_t>::max() ? 0u : 1u);
    }

    expected_gt<size_estimates_t> size_estimates() noexcept {
        status_t status;
        arena_t arena(db_);
        auto a = arena.member_ptr();
        auto s = status.member_ptr();
        ukv_size_t* min_cardinalities = nullptr;
        ukv_size_t* max_cardinalities = nullptr;
        ukv_size_t* min_value_bytes = nullptr;
        ukv_size_t* max_value_bytes = nullptr;
        ukv_size_t* min_space_usages = nullptr;
        ukv_size_t* max_space_usages = nullptr;
        ukv_measure_t size {
            .db = db_,
            .error = s,
            .transaction = txn_,
            .arena = a,
            .collections = &collection_,
            .start_keys = &min_key_,
            .end_keys = &max_key_,
            .min_cardinalities = &min_cardinalities,
            .max_cardinalities = &max_cardinalities,
            .min_value_bytes = &min_value_bytes,
            .max_value_bytes = &max_value_bytes,
            .min_space_usages = &min_space_usages,
            .max_space_usages = &max_space_usages,
        };

        ukv_measure(&size);
        if (!status)
            return status;
        size_estimates_t result {{min_cardinalities[0], max_cardinalities[0]},
                                 {min_value_bytes[0], max_value_bytes[0]},
                                 {min_space_usages[0], max_space_usages[0]}};
        return result;
    }

    blobs_range_t& since(ukv_key_t min_key) noexcept {
        min_key_ = min_key;
        return *this;
    }
    blobs_range_t& until(ukv_key_t max_key) noexcept {
        max_key_ = max_key;
        return *this;
    }

    ukv_key_t min_key() noexcept { return min_key_; }
    ukv_key_t max_key() noexcept { return max_key_; }
};

struct keys_range_t {
    using iterator_type = keys_stream_t;
    using sample_t = ptr_range_gt<ukv_key_t>;

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

    expected_gt<sample_t> sample(std::size_t count, ukv_arena_t* arena) noexcept {
        ukv_length_t* found_counts = nullptr;
        ukv_key_t* found_keys = nullptr;
        status_t status;
        ukv_length_t c_count = static_cast<ukv_length_t>(count);
        ukv_collection_t c_collection = members.collection();
        ukv_sample_t sample {
            .db = members.db(),
            .error = status.member_ptr(),
            .transaction = members.txn(),
            .arena = arena,
            .tasks_count = 1,
            .collections = &c_collection,
            .count_limits = &c_count,
            .counts = &found_counts,
            .keys = &found_keys,
        };

        ukv_sample(&sample);

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

} // namespace unum::ukv
