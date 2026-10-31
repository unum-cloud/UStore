/**
 *  @file test/nvme/compaction_device.cu
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief The device compactor instantiated for the key slots of the compaction suite, and with the graph's device fold
 *      for its variable-width one, so its host build calls it.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstdint> // `std::uint64_t`

#include <span> // `std::span`

#include "ustore/blobs/vram_nvidia_compaction.cuh" // `cuda_compactor_t`, `compaction_output`, `sorted_run_index_view`
#include "ustore/graph/vram_nvidia_adjacency_fold.cuh" // `adjacency_fold_t`, `vram_nvidia_adjacency_fold_t`
#include "ustore/utilities/folds.hpp"                  // `last_write_fold_t`

#include "nvme/compaction_key_slots.hpp" // `inline_key_slot`, `fixed_width_key_slot`, `variable_width_key_slot`

namespace unum::ustore {

using blobs::compaction_kind_t;
using blobs::compaction_output;
using blobs::compaction_stamps_t;
using blobs::sorted_run_bounds_t;
using st::key128_t;
using utilities::last_write_fold_t;

/** The sorted run indexes of one LSM level, as a compaction takes both of its levels. */
template <typename key_slot_type_>
using views_of = std::span<blobs::sorted_run_index_view<key_slot_type_> const>;

template st::status_t blobs::cuda_compactor_t::compact(
    compaction_kind_t, views_of<test::inline_key_slot<std::uint64_t>>, views_of<test::inline_key_slot<std::uint64_t>>,
    std::span<blobs::sorted_run_extents_t const>, sorted_run_bounds_t const&, compaction_stamps_t const&,
    last_write_fold_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::inline_key_slot<std::uint64_t>>,
    compaction_output<test::inline_key_slot<std::uint64_t>>&);
template st::status_t blobs::cuda_compactor_t::compact(
    compaction_kind_t, views_of<test::fixed_width_key_slot<std::uint64_t>>,
    views_of<test::fixed_width_key_slot<std::uint64_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, last_write_fold_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::fixed_width_key_slot<std::uint64_t>>,
    compaction_output<test::fixed_width_key_slot<std::uint64_t>>&);
template st::status_t blobs::cuda_compactor_t::compact(
    compaction_kind_t, views_of<test::variable_width_key_slot<std::uint64_t>>,
    views_of<test::variable_width_key_slot<std::uint64_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, last_write_fold_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::variable_width_key_slot<std::uint64_t>>,
    compaction_output<test::variable_width_key_slot<std::uint64_t>>&);
template st::status_t blobs::cuda_compactor_t::compact(compaction_kind_t, views_of<test::inline_key_slot<key128_t>>,
                                                       views_of<test::inline_key_slot<key128_t>>,
                                                       std::span<blobs::sorted_run_extents_t const>,
                                                       sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                       last_write_fold_t const&, blobs::no_device_fold_t const&,
                                                       blobs::located_value_reader<test::inline_key_slot<key128_t>>,
                                                       compaction_output<test::inline_key_slot<key128_t>>&);
template st::status_t blobs::cuda_compactor_t::compact(
    compaction_kind_t, views_of<test::fixed_width_key_slot<key128_t>>, views_of<test::fixed_width_key_slot<key128_t>>,
    std::span<blobs::sorted_run_extents_t const>, sorted_run_bounds_t const&, compaction_stamps_t const&,
    last_write_fold_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::fixed_width_key_slot<key128_t>>,
    compaction_output<test::fixed_width_key_slot<key128_t>>&);
template st::status_t blobs::cuda_compactor_t::compact(
    compaction_kind_t, views_of<test::variable_width_key_slot<key128_t>>,
    views_of<test::variable_width_key_slot<key128_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, last_write_fold_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::variable_width_key_slot<key128_t>>,
    compaction_output<test::variable_width_key_slot<key128_t>>&);
template st::status_t blobs::cuda_compactor_t::compact(
    compaction_kind_t, views_of<test::variable_width_key_slot<std::uint64_t>>,
    views_of<test::variable_width_key_slot<std::uint64_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, graph::adjacency_fold_t const&,
    graph::vram_nvidia_adjacency_fold_t const&,
    blobs::located_value_reader<test::variable_width_key_slot<std::uint64_t>>,
    compaction_output<test::variable_width_key_slot<std::uint64_t>>&);

} // namespace unum::ustore
