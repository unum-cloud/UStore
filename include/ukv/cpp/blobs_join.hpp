/**
 * @file blobs_join.hpp
 * @author Ashot Vardanian
 * @date 26 Jun 2022
 * @addtogroup Cpp
 *
 * @brief C++ bindings for "ukv/db.h".
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

    ukv_database_t db {nullptr};
    ukv_transaction_t txn {nullptr};
    ukv_arena_t* arena {nullptr};

    strided_range_gt<ukv_collection_t const> collections;
    ukv_key_t next_min_key_ {0};
    ukv_size_t window_size_ {0};

    strided_range_gt<ukv_key_t*> fetched_keys_;
    strided_range_gt<ukv_length_t> fetched_lengths;
};

} // namespace unum::ukv
