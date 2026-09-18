/**
 *  @file src/nvme/io_types.hpp
 *  @author Ash Vardanian
 *  @date May 23, 2021
 *  @brief File identifiers and execution policies of the raw file I/O layer.
 */

#pragma once

#include <cstdint> // `std::uint16_t`

namespace unum::ustore::nvme {

/** One mounted disk of a data directory, where every stored value names the disk it sits on. */
enum class disk_id_t : std::uint16_t {
    unknown_k = 0xFFFF,
};

/** A POSIX file descriptor, where a negative value marks a failed open. */
enum class file_id_t : int {
    invalid_k = -1,
};

/** Whether a file operation blocks the caller or completes through `io_uring`. */
enum class execution_policy_t {
    sync_k,
    async_k,
};

} // namespace unum::ustore::nvme
