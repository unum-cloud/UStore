/**
 * @file full_scan.hpp
 * @author Ashot Vardanian
 *
 * @brief Callback-based full-scan over BLOB collection.
 */
#pragma once
#include <random>

#include "ustore/blobs.h"

namespace unum::ustore {

template <typename callback_should_continue_at>
void full_scan_collection( //
    ustore_database_t db,
    ustore_transaction_t transaction,
    ustore_collection_t collection,
    ustore_options_t options,
    ustore_key_t start_key,
    ustore_length_t read_ahead,
    linked_memory_lock_t& arena,
    ustore_error_t* error,
    callback_should_continue_at&& callback_should_continue) noexcept {

    read_ahead = std::max<ustore_length_t>(read_ahead, 2u);
    while (!*error) {
        ustore_length_t* found_blobs_count {};
        ustore_key_t* found_blobs_keys {};
        ustore_scan_t scan {};
        scan.db = db;
        scan.error = error;
        scan.transaction = transaction;
        scan.arena = arena;
        scan.options = options;
        scan.tasks_count = 1;
        scan.collections = &collection;
        scan.start_keys = &start_key;
        scan.count_limits = &read_ahead;
        scan.counts = &found_blobs_count;
        scan.keys = &found_blobs_keys;

        ustore_scan(&scan);
        if (*error)
            break;

        if (found_blobs_count[0] <= 1)
            // We have reached the end of collection
            break;

        ustore_length_t* found_blobs_offsets {};
        ustore_byte_t* found_blobs_data {};
        ustore_read_t read {};
        read.db = db;
        read.error = error;
        read.transaction = transaction;
        read.arena = arena;
        read.options = ustore_options_t(options | ustore_option_dont_discard_memory_k);
        read.tasks_count = found_blobs_count[0];
        read.collections = &collection;
        read.collections_stride = 0;
        read.keys = found_blobs_keys;
        read.keys_stride = sizeof(ustore_key_t);
        read.offsets = &found_blobs_offsets;
        read.values = &found_blobs_data;

        ustore_read(&read);
        if (*error)
            break;

        ustore_length_t const count_blobs = found_blobs_count[0];
        joined_blobs_iterator_t found_blobs {found_blobs_offsets, found_blobs_data};
        for (std::size_t i = 0; i != count_blobs; ++i, ++found_blobs) {
            value_view_t bucket = *found_blobs;
            if (!callback_should_continue(found_blobs_keys[i], bucket))
                return;
        }

        start_key = found_blobs_keys[count_blobs - 1] + 1;
    }
}

/**
 * @brief Implements reservoir sampling for RocksDB or LevelDB collections.
 * @see https://en.wikipedia.org/wiki/Reservoir_sampling
 */
template <typename level_or_rocks_iterator_at>
void reservoir_sample_iterator(level_or_rocks_iterator_at&& iterator,
                               ptr_range_gt<ustore_key_t> sampled_keys,
                               ustore_error_t* c_error) noexcept {

    std::random_device random_device;
    std::mt19937 random_generator(random_device());
    std::uniform_int_distribution<ustore_key_t> dist(std::numeric_limits<ustore_key_t>::min());

    std::size_t i = 0;
    for (iterator->SeekToFirst(); i < sampled_keys.size(); ++i, iterator->Next()) {
        return_error_if_m(iterator->Valid(), c_error, 0, "Sample Failure!");
        std::memcpy(&sampled_keys[i], iterator->key().data(), sizeof(ustore_key_t));
    }

    for (std::size_t j = 0; iterator->Valid(); ++i, iterator->Next()) {
        j = dist(random_generator) % (i + 1);
        if (j < sampled_keys.size())
            std::memcpy(&sampled_keys[j], iterator->key().data(), sizeof(ustore_key_t));
    }
}

} // namespace unum::ustore
