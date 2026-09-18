/**
 *  @file src/nvme/graph_store_config.hpp
 *  @author Ash Vardanian
 *  @date December 1, 2022
 *  @brief Graph database configuration and its translation into single-collection database configurations.
 */

#pragma once
#include <cstddef> // `std::size_t`

#include "nvme/config.hpp"

namespace unum::ustore::nvme {

struct graph_store_config_t {
    std::size_t transaction_max_elements = 1'000;
    std::size_t transaction_max_bytes = 100'000'000;

    std::size_t concurrency_limit = 1;
    std::size_t cache_limit = 0;
    std::size_t write_buffer_max_elements = 10'000;
    std::size_t write_buffer_max_bytes = 100'000'000;

  private:
    friend class graph_store_t;

    user_single_collection_database_config_t to_user_single_collection_database_config_() const {
        user_single_collection_database_config_t user_single_collection_database_config;
        user_single_collection_database_config.default_transaction.write_buffer_max_elements = transaction_max_elements;
        user_single_collection_database_config.default_transaction.write_buffer_max_bytes = transaction_max_bytes;

        user_single_collection_database_config.collection.concurrency_limit = concurrency_limit;
        user_single_collection_database_config.collection.cache_limit = cache_limit;
        user_single_collection_database_config.collection.write_buffer_max_elements = write_buffer_max_elements;
        user_single_collection_database_config.collection.write_buffer_max_bytes = write_buffer_max_bytes;
        user_single_collection_database_config.collection.first_level_max_bytes = 32 * write_buffer_max_bytes;
        user_single_collection_database_config.collection.level_enlarge_factor = 2;
        user_single_collection_database_config.collection.value_size = 0;
        user_single_collection_database_config.collection.value_max_size = 4'000'000;
        return user_single_collection_database_config;
    }

    single_collection_database_config_t single_collection_database_config_() const {
        return create_single_collection_database_config(to_user_single_collection_database_config_());
    }
};

} // namespace unum::ustore::nvme
