/**
 * @file keys_join_stream.hpp
 * @author Ashot Vardanian
 * @date 26 Jun 2022
 * @brief C++ bindings for @see "ukv/db.h".
 */

#pragma once
#include "ukv/ukv.h"
#include "ukv/cpp/ranges.hpp"

namespace unum::ukv {

/**
 * @brief Implements multi-way set intersection to join entities
 * from different collections, that have matching identifiers.
 *
 * Implementation-wise, scans the smallest collection and batch-selects
 * in others.
 */
struct keys_join_stream_t {

    ukv_t db = nullptr;
    ukv_txn_t txn = nullptr;
    ukv_arena_t* arena = nullptr;

    strided_range_gt<ukv_collection_t const> cols;
    ukv_key_t next_min_key_ = 0;
    ukv_size_t window_size = 0;

    strided_range_gt<ukv_key_t*> fetched_keys_;
    strided_range_gt<ukv_val_len_t> fetched_lengths;
};

} // namespace unum::ukv
