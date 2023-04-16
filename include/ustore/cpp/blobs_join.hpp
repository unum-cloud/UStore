/**
 * @file blobs_join.hpp
 * @author Ashot Vardanian
 * @date 26 Jun 2022
 * @addtogroup Cpp
 *
 * @brief C++ bindings for "ustore/db.h".
 */

#pragma once
#include "ustore/ustore.h"
#include "ustore/cpp/ranges.hpp"

namespace unum::ustore {

/**
 * @brief Implements multi-way set intersection to join entities
 * from different collections, that have matching identifiers.
 *
 * Implementation-wise, scans the smallest collection and batch-selects
 * in others.
 */
struct keys_join_stream_t {

    ustore_database_t db {nullptr};
    ustore_transaction_t txn {nullptr};
    ustore_arena_t* arena {nullptr};

    strided_range_gt<ustore_collection_t const> collections;
    ustore_key_t next_min_key_ {0};
    ustore_size_t window_size_ {0};

    strided_range_gt<ustore_key_t*> fetched_keys_;
    strided_range_gt<ustore_length_t> fetched_lengths;
};

} // namespace unum::ustore
