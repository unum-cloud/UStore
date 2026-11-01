/**
 *  @file src/nvme/compaction_variants.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The variants of the device compaction the engine calls: one per key slot layout it stores, and one with the
 *      graph's device fold for each key slot a graph stores.
 *
 *  An explicit instantiation needs the definition it instantiates, so @c compaction.cpp includes
 *  this list after it. The bundles carry merge kernels for the engine's key slots alone, so the
 *  suites' key slots instantiate a compaction that refuses every merge, which the tree then runs
 *  on the host.
 */

#pragma once

#include <cstdint> // `std::uint64_t`

#include <span> // `std::span`

#include "ustore/blobs/nvme_compaction.hpp"            // `compaction_output`, `located_value_reader`
#include "ustore/blobs/vram_nvidia_compaction.hpp"     // `cuda_compactor_t`
#include "ustore/blobs/nvme_key_slot.hpp"              // `key_slot`, `value_layout_t`
#include "ustore/graph/adjacency_fold.hpp"             // `adjacency_fold_t`
#include "ustore/graph/vram_nvidia_adjacency_fold.hpp" // `vram_nvidia_adjacency_fold_t`
#include "ustore/utilities/folds.hpp"                  // `last_write_fold_t`

namespace unum::ustore::blobs {

/** The sorted run indexes of one LSM level, as a compaction takes both of its levels. */
template <typename key_slot_type_>
using views_of = std::span<sorted_run_index_view<key_slot_type_> const>;

/** Where every run of a compaction sits on its volume, beside its views. */
using compaction_extents_t = std::span<sorted_run_extents_t const>;

/** The key the engine stores: the sixteen bytes of the C ABI's key, read as two big-endian words. */
using engine_stored_key_t = st::key128_t;

/** The key slots of the engine's collections, one per value layout a collection is created with. */
using inline_12_bytes_slot_t = key_slot<engine_stored_key_t, value_layout_t::inline_12_bytes_k>;
using inline_44_bytes_slot_t = key_slot<engine_stored_key_t, value_layout_t::inline_44_bytes_k>;
using fixed_width_slot_t = key_slot<engine_stored_key_t, value_layout_t::fixed_width_k>;
using variable_width_slot_t = key_slot<engine_stored_key_t, value_layout_t::variable_width_k>;

/** The key slots the storage suites model their runs with, one per value layout, since a database of them holds a
 *  collection of any layout. */
using suite_inline_12_bytes_slot_t = key_slot<std::uint64_t, value_layout_t::inline_12_bytes_k>;
using suite_inline_44_bytes_slot_t = key_slot<std::uint64_t, value_layout_t::inline_44_bytes_k>;
using suite_fixed_width_slot_t = key_slot<std::uint64_t, value_layout_t::fixed_width_k>;
using suite_variable_width_slot_t = key_slot<std::uint64_t, value_layout_t::variable_width_k>;

template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<inline_12_bytes_slot_t>,
                                                views_of<inline_12_bytes_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                utilities::last_write_fold_t const&, no_device_fold_t const&,
                                                located_value_reader<inline_12_bytes_slot_t>,
                                                compaction_output<inline_12_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<inline_44_bytes_slot_t>,
                                                views_of<inline_44_bytes_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                utilities::last_write_fold_t const&, no_device_fold_t const&,
                                                located_value_reader<inline_44_bytes_slot_t>,
                                                compaction_output<inline_44_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<fixed_width_slot_t>,
                                                views_of<fixed_width_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                utilities::last_write_fold_t const&, no_device_fold_t const&,
                                                located_value_reader<fixed_width_slot_t>,
                                                compaction_output<fixed_width_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<variable_width_slot_t>,
                                                views_of<variable_width_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                utilities::last_write_fold_t const&, no_device_fold_t const&,
                                                located_value_reader<variable_width_slot_t>,
                                                compaction_output<variable_width_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_inline_12_bytes_slot_t>,
                                                views_of<suite_inline_12_bytes_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                utilities::last_write_fold_t const&, no_device_fold_t const&,
                                                located_value_reader<suite_inline_12_bytes_slot_t>,
                                                compaction_output<suite_inline_12_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_inline_44_bytes_slot_t>,
                                                views_of<suite_inline_44_bytes_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                utilities::last_write_fold_t const&, no_device_fold_t const&,
                                                located_value_reader<suite_inline_44_bytes_slot_t>,
                                                compaction_output<suite_inline_44_bytes_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_fixed_width_slot_t>,
                                                views_of<suite_fixed_width_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                utilities::last_write_fold_t const&, no_device_fold_t const&,
                                                located_value_reader<suite_fixed_width_slot_t>,
                                                compaction_output<suite_fixed_width_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_variable_width_slot_t>,
                                                views_of<suite_variable_width_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                utilities::last_write_fold_t const&, no_device_fold_t const&,
                                                located_value_reader<suite_variable_width_slot_t>,
                                                compaction_output<suite_variable_width_slot_t>&);

/** The graph store's key slot, whose changes its device fold composes. */
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<variable_width_slot_t>,
                                                views_of<variable_width_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                graph::adjacency_fold_t const&,
                                                graph::vram_nvidia_adjacency_fold_t const&,
                                                located_value_reader<variable_width_slot_t>,
                                                compaction_output<variable_width_slot_t>&);
template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_variable_width_slot_t>,
                                                views_of<suite_variable_width_slot_t>, compaction_extents_t,
                                                sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                graph::adjacency_fold_t const&,
                                                graph::vram_nvidia_adjacency_fold_t const&,
                                                located_value_reader<suite_variable_width_slot_t>,
                                                compaction_output<suite_variable_width_slot_t>&);

} // namespace unum::ustore::blobs
