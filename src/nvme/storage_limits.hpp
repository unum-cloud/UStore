/**
 *  @file src/nvme/storage_limits.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Size and capacity limits enforced when validating configurations.
 */
#pragma once

#include <cstddef> // `std::size_t`

#include <algorithm> // `std::max`

#include "nvme/stored_value.hpp" // `empty_stored_value_offset_k`

namespace unum::ustore::nvme {

/** Size and capacity limits enforced when validating configurations. */
struct storage_limits_t {
    static constexpr std::size_t fixed_value_min_size_k = 1;
    static constexpr std::size_t fixed_value_max_size_k = 4 * 1024 * 1024;
    static constexpr std::size_t variable_value_min_size_k = 0;
    static constexpr std::size_t variable_value_max_size_k = 4 * 1024 * 1024;
    static constexpr std::size_t cache_limit_max_size_k = 100 * 1024 * 1024 * 1024ull;
    static constexpr std::size_t write_buffer_min_capacity_k = 1024;
    static constexpr std::size_t write_buffer_max_capacity_k = 1024 * 1024 * 1024ull;
    static constexpr std::size_t write_buffer_elements_min_count_k = 16;
    static constexpr std::size_t write_buffer_elements_max_count_k = 1024 * 1024;
    static constexpr std::size_t transaction_write_buffer_min_capacity_k = 1;
    static constexpr std::size_t transaction_write_buffer_max_capacity_k = write_buffer_max_capacity_k;
    static constexpr std::size_t transaction_write_buffer_elements_min_count_k = 1;
    static constexpr std::size_t transaction_write_buffer_elements_max_count_k = write_buffer_elements_max_count_k;
    static constexpr std::size_t first_level_min_capacity_bytes_k = 1024 * 1024;
    static constexpr std::size_t first_level_max_sorted_runs_count_k = 1024;
    static constexpr std::size_t first_level_compaction_threshold_k = 768;
    static constexpr std::size_t level_min_enlarge_factor_k = 2;
    static constexpr std::size_t level_max_enlarge_factor_k = 64;
    static constexpr std::size_t sorted_run_file_max_size_k = 4 * 1024 * 1024 * 1024ull - 4096;
    static constexpr std::size_t gpu_compaction_threshold_k = 900'000;
    static constexpr std::size_t value_max_size_k = std::max(fixed_value_max_size_k, variable_value_max_size_k);
    static constexpr std::size_t user_data_max_size_k = 4 * 1024 * 1024;
};

static_assert(storage_limits_t::sorted_run_file_max_size_k <= empty_stored_value_offset_k,
              "A stored value's location never reaches the offsets that mean empty and erased");

} // namespace unum::ustore::nvme
