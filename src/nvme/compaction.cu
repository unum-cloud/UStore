/**
 *  @file src/nvme/compaction.cu
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The non-template part of GPU compaction: binding a compactor to a device and a stream.
 */

#include <cstddef> // `std::size_t`

#include <utility> // `std::move`

#include <smashtable/shared.hpp>

#include <ustore/types.hpp>

#include "cuda.hpp"
#include "nvme/compaction.cuh"

namespace unum::ustore::nvme {

st::expected<gpu_compactor_t> gpu_compactor_t::make(int ordinal) noexcept {
    using st::expected;
    using st::status_t;
    expected<std::size_t> const devices = cuda_count_devices();
    if (!devices) return devices.status();
    if (ordinal < 0 || static_cast<std::size_t>(ordinal) >= *devices) return status_t::invalid_argument_k;
    expected<cuda_stream_t> stream = cuda_stream_t::make(ordinal);
    if (!stream) return stream.status();
    return gpu_compactor_t(std::move(*stream));
}

} // namespace unum::ustore::nvme
