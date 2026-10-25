/**
 *  @file src/nvme/compaction.cu
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The compiled part of GPU compaction: binding a compactor to a device, and its compaction of every key slot
 *      the engine stores, with the graph's device fold for the graph's key slot, instantiated here so the host TUs of
 *      the engine call it.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int64_t`, `std::uint64_t`

#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/shared.hpp> // `st::status_t`, `st::expected`

#include "ustore/blobs/vram_nvidia_compaction.cuh"     // `compaction_kind_t`, `compaction_output`, `cuda_compactor_t`
#include "ustore/blobs/nvme_key_slot.hpp"              // `key_slot`, `value_layout_t`
#include "ustore/graph/vram_nvidia_adjacency_fold.cuh" // `adjacency_fold_t`, `vram_nvidia_adjacency_fold_t`
#include "ustore/utilities/folds.hpp"                  // `last_write_fold_t`
#include "ustore/utilities/vram_nvidia_device.hpp"     // `cuda_count_devices`, `cuda_stream_t`

namespace unum::ustore::blobs {

using utilities::last_write_fold_t;

st::expected<cuda_compactor_t> cuda_compactor_t::make(int ordinal) noexcept {
    using st::expected;
    using st::status_t;
    expected<std::size_t> const devices = utilities::cuda_count_devices();
    if (!devices) return devices.status();
    if (ordinal < 0 || static_cast<std::size_t>(ordinal) >= *devices) return status_t::invalid_argument_k;
    expected<cuda_stream_t> stream = cuda_stream_t::make(ordinal);
    if (!stream) return stream.status();
    return cuda_compactor_t(std::move(*stream));
}

#pragma region Instantiations

/** The sorted run indexes of one LSM level, as a compaction takes both of its levels. */
template <typename key_slot_type_>
using views_of = std::span<sorted_run_index_view<key_slot_type_> const>;

/** The key the engine stores: the sixteen bytes of the C ABI's key, read as two big-endian words. */
using stored_key_t = st::key128_t;

/** The key slots of the engine's collections, one per value layout a collection is created with. */
using inline_1_byte_slot_t = key_slot<stored_key_t, value_layout_t::inline_1_byte_k>;
using inline_4_bytes_slot_t = key_slot<stored_key_t, value_layout_t::inline_4_bytes_k>;
using inline_8_bytes_slot_t = key_slot<stored_key_t, value_layout_t::inline_8_bytes_k>;
using inline_16_bytes_slot_t = key_slot<stored_key_t, value_layout_t::inline_16_bytes_k>;
using inline_32_bytes_slot_t = key_slot<stored_key_t, value_layout_t::inline_32_bytes_k>;
using inline_64_bytes_slot_t = key_slot<stored_key_t, value_layout_t::inline_64_bytes_k>;
using fixed_width_slot_t = key_slot<stored_key_t, value_layout_t::fixed_width_k>;
using variable_width_slot_t = key_slot<stored_key_t, value_layout_t::variable_width_k>;

/** The key slots the storage suites model their runs with, one per value layout, since a database of them holds a
 *  collection of any layout. */
using suite_inline_1_byte_slot_t = key_slot<std::uint64_t, value_layout_t::inline_1_byte_k>;
using suite_inline_4_bytes_slot_t = key_slot<std::uint64_t, value_layout_t::inline_4_bytes_k>;
using suite_inline_8_bytes_slot_t = key_slot<std::uint64_t, value_layout_t::inline_8_bytes_k>;
using suite_inline_16_bytes_slot_t = key_slot<std::uint64_t, value_layout_t::inline_16_bytes_k>;
using suite_inline_32_bytes_slot_t = key_slot<std::uint64_t, value_layout_t::inline_32_bytes_k>;
using suite_inline_64_bytes_slot_t = key_slot<std::uint64_t, value_layout_t::inline_64_bytes_k>;
using suite_fixed_width_slot_t = key_slot<std::uint64_t, value_layout_t::fixed_width_k>;
using suite_variable_width_slot_t = key_slot<std::uint64_t, value_layout_t::variable_width_k>;

template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<inline_1_byte_slot_t>,
                                                views_of<inline_1_byte_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&, located_value_reader<inline_1_byte_slot_t>,
                                                compaction_output<inline_1_byte_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<inline_4_bytes_slot_t>,
                                                views_of<inline_4_bytes_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&, located_value_reader<inline_4_bytes_slot_t>,
                                                compaction_output<inline_4_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<inline_8_bytes_slot_t>,
                                                views_of<inline_8_bytes_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&, located_value_reader<inline_8_bytes_slot_t>,
                                                compaction_output<inline_8_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<inline_16_bytes_slot_t>,
                                                views_of<inline_16_bytes_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&, located_value_reader<inline_16_bytes_slot_t>,
                                                compaction_output<inline_16_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<inline_32_bytes_slot_t>,
                                                views_of<inline_32_bytes_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&, located_value_reader<inline_32_bytes_slot_t>,
                                                compaction_output<inline_32_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<inline_64_bytes_slot_t>,
                                                views_of<inline_64_bytes_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&, located_value_reader<inline_64_bytes_slot_t>,
                                                compaction_output<inline_64_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<fixed_width_slot_t>,
                                                views_of<fixed_width_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&, located_value_reader<fixed_width_slot_t>,
                                                compaction_output<fixed_width_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<variable_width_slot_t>,
                                                views_of<variable_width_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&, located_value_reader<variable_width_slot_t>,
                                                compaction_output<variable_width_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_inline_1_byte_slot_t>,
                                                views_of<suite_inline_1_byte_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&,
                                                located_value_reader<suite_inline_1_byte_slot_t>,
                                                compaction_output<suite_inline_1_byte_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_inline_4_bytes_slot_t>,
                                                views_of<suite_inline_4_bytes_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&,
                                                located_value_reader<suite_inline_4_bytes_slot_t>,
                                                compaction_output<suite_inline_4_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_inline_8_bytes_slot_t>,
                                                views_of<suite_inline_8_bytes_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&,
                                                located_value_reader<suite_inline_8_bytes_slot_t>,
                                                compaction_output<suite_inline_8_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_inline_16_bytes_slot_t>,
                                                views_of<suite_inline_16_bytes_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&,
                                                located_value_reader<suite_inline_16_bytes_slot_t>,
                                                compaction_output<suite_inline_16_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_inline_32_bytes_slot_t>,
                                                views_of<suite_inline_32_bytes_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&,
                                                located_value_reader<suite_inline_32_bytes_slot_t>,
                                                compaction_output<suite_inline_32_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_inline_64_bytes_slot_t>,
                                                views_of<suite_inline_64_bytes_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&,
                                                located_value_reader<suite_inline_64_bytes_slot_t>,
                                                compaction_output<suite_inline_64_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_fixed_width_slot_t>,
                                                views_of<suite_fixed_width_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&, located_value_reader<suite_fixed_width_slot_t>,
                                                compaction_output<suite_fixed_width_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_variable_width_slot_t>,
                                                views_of<suite_variable_width_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, last_write_fold_t const&,
                                                no_device_fold_t const&,
                                                located_value_reader<suite_variable_width_slot_t>,
                                                compaction_output<suite_variable_width_slot_t>&);

/** The graph store's key slot, whose changes its device fold composes. */
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<variable_width_slot_t>,
                                                views_of<variable_width_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, graph::adjacency_fold_t const&,
                                                graph::vram_nvidia_adjacency_fold_t const&,
                                                located_value_reader<variable_width_slot_t>,
                                                compaction_output<variable_width_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_variable_width_slot_t>,
                                                views_of<suite_variable_width_slot_t>, sorted_run_bounds_t const&,
                                                compaction_stamps_t const&, graph::adjacency_fold_t const&,
                                                graph::vram_nvidia_adjacency_fold_t const&,
                                                located_value_reader<suite_variable_width_slot_t>,
                                                compaction_output<suite_variable_width_slot_t>&);

#pragma endregion Instantiations

} // namespace unum::ustore::blobs
