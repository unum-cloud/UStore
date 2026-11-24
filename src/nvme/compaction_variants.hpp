/**
 *  @file src/nvme/compaction_variants.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The variants of the device compaction the engine calls on every vendor's device: one
 *      per key slot layout it stores, and one with the graph's device fold per graph key slot.
 *
 *  The explicit instantiations live in @c compaction.cpp, beside the definitions they need; the
 *  Apple compactor's body sits in its header, beside the NVIDIA one's it includes, so every unit
 *  including this one sees their instantiations as declared elsewhere. The bundles hold merge
 *  kernels for the engine's key slots alone, so the suites' key slots instantiate a compaction that
 *  refuses every merge, which the tree then runs on the host.
 */

#pragma once

#include <cstdint> // `std::uint64_t`

#include <span> // `std::span`

#include "ustore/blobs/nvme_compaction.hpp"        // `compaction_output`, `fold_ref_t`
#include "ustore/blobs/vram_amd_compaction.hpp"    // the AMD compactor the variants instantiate
#include "ustore/blobs/vram_apple_compaction.hpp"  // `metal_compactor_t`
#include "ustore/blobs/vram_nvidia_compaction.hpp" // the NVIDIA compactor the variants instantiate
#include "ustore/blobs/nvme_key_slot.hpp"          // `key_slot`, `value_layout_t`
#include "ustore/graph/adjacency_fold.hpp"         // `adjacency_fold_t`
#include "ustore/graph/vram_adjacency_fold.hpp"    // `vram_adjacency_fold_t`

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

/** The key slots the storage suites model their runs with, one per value layout, since a database
 *  of them holds a collection of any layout. */
using suite_inline_12_bytes_slot_t = key_slot<std::uint64_t, value_layout_t::inline_12_bytes_k>;
using suite_inline_44_bytes_slot_t = key_slot<std::uint64_t, value_layout_t::inline_44_bytes_k>;
using suite_fixed_width_slot_t = key_slot<std::uint64_t, value_layout_t::fixed_width_k>;
using suite_variable_width_slot_t = key_slot<std::uint64_t, value_layout_t::variable_width_k>;

/** The engine's and the suites' key slots on an NVIDIA device, instantiated in @c compaction.cpp
 *  alone, as the Apple header includes the compactor's body. */
extern template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<inline_12_bytes_slot_t>,
                                                       views_of<inline_12_bytes_slot_t>, compaction_extents_t,
                                                       sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                       fold_ref_t const&, no_device_fold_t const&,
                                                       located_value_reader<inline_12_bytes_slot_t>,
                                                       compaction_output<inline_12_bytes_slot_t>&);
extern template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<inline_44_bytes_slot_t>,
                                                       views_of<inline_44_bytes_slot_t>, compaction_extents_t,
                                                       sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                       fold_ref_t const&, no_device_fold_t const&,
                                                       located_value_reader<inline_44_bytes_slot_t>,
                                                       compaction_output<inline_44_bytes_slot_t>&);
extern template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<fixed_width_slot_t>,
                                                       views_of<fixed_width_slot_t>, compaction_extents_t,
                                                       sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                       fold_ref_t const&, no_device_fold_t const&,
                                                       located_value_reader<fixed_width_slot_t>,
                                                       compaction_output<fixed_width_slot_t>&);
extern template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<variable_width_slot_t>,
                                                       views_of<variable_width_slot_t>, compaction_extents_t,
                                                       sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                       fold_ref_t const&, no_device_fold_t const&,
                                                       located_value_reader<variable_width_slot_t>,
                                                       compaction_output<variable_width_slot_t>&);
extern template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_inline_12_bytes_slot_t>,
                                                       views_of<suite_inline_12_bytes_slot_t>, compaction_extents_t,
                                                       sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                       fold_ref_t const&, no_device_fold_t const&,
                                                       located_value_reader<suite_inline_12_bytes_slot_t>,
                                                       compaction_output<suite_inline_12_bytes_slot_t>&);
extern template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_inline_44_bytes_slot_t>,
                                                       views_of<suite_inline_44_bytes_slot_t>, compaction_extents_t,
                                                       sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                       fold_ref_t const&, no_device_fold_t const&,
                                                       located_value_reader<suite_inline_44_bytes_slot_t>,
                                                       compaction_output<suite_inline_44_bytes_slot_t>&);
extern template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_fixed_width_slot_t>,
                                                       views_of<suite_fixed_width_slot_t>, compaction_extents_t,
                                                       sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                       fold_ref_t const&, no_device_fold_t const&,
                                                       located_value_reader<suite_fixed_width_slot_t>,
                                                       compaction_output<suite_fixed_width_slot_t>&);
extern template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_variable_width_slot_t>,
                                                       views_of<suite_variable_width_slot_t>, compaction_extents_t,
                                                       sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                       fold_ref_t const&, no_device_fold_t const&,
                                                       located_value_reader<suite_variable_width_slot_t>,
                                                       compaction_output<suite_variable_width_slot_t>&);

/** The graph store's key slots on an NVIDIA device, whose changes its device fold composes. */
extern template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<variable_width_slot_t>,
                                                       views_of<variable_width_slot_t>, compaction_extents_t,
                                                       sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                       graph::adjacency_fold_t const&,
                                                       graph::vram_adjacency_fold_t const&,
                                                       located_value_reader<variable_width_slot_t>,
                                                       compaction_output<variable_width_slot_t>&);
extern template st::status_t cuda_compactor_t::compact(compaction_kind_t, views_of<suite_variable_width_slot_t>,
                                                       views_of<suite_variable_width_slot_t>, compaction_extents_t,
                                                       sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                       graph::adjacency_fold_t const&,
                                                       graph::vram_adjacency_fold_t const&,
                                                       located_value_reader<suite_variable_width_slot_t>,
                                                       compaction_output<suite_variable_width_slot_t>&);

/** The engine's and the suites' key slots on an Apple device, instantiated in @c compaction.cpp
 *  alone, so the units of every CPU class that see the compactor's body link that copy. */
extern template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<inline_12_bytes_slot_t>,
                                                        views_of<inline_12_bytes_slot_t>, compaction_extents_t,
                                                        sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                        fold_ref_t const&, no_device_fold_t const&,
                                                        located_value_reader<inline_12_bytes_slot_t>,
                                                        compaction_output<inline_12_bytes_slot_t>&);
extern template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<inline_44_bytes_slot_t>,
                                                        views_of<inline_44_bytes_slot_t>, compaction_extents_t,
                                                        sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                        fold_ref_t const&, no_device_fold_t const&,
                                                        located_value_reader<inline_44_bytes_slot_t>,
                                                        compaction_output<inline_44_bytes_slot_t>&);
extern template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<fixed_width_slot_t>,
                                                        views_of<fixed_width_slot_t>, compaction_extents_t,
                                                        sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                        fold_ref_t const&, no_device_fold_t const&,
                                                        located_value_reader<fixed_width_slot_t>,
                                                        compaction_output<fixed_width_slot_t>&);
extern template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<variable_width_slot_t>,
                                                        views_of<variable_width_slot_t>, compaction_extents_t,
                                                        sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                        fold_ref_t const&, no_device_fold_t const&,
                                                        located_value_reader<variable_width_slot_t>,
                                                        compaction_output<variable_width_slot_t>&);
extern template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<suite_inline_12_bytes_slot_t>,
                                                        views_of<suite_inline_12_bytes_slot_t>, compaction_extents_t,
                                                        sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                        fold_ref_t const&, no_device_fold_t const&,
                                                        located_value_reader<suite_inline_12_bytes_slot_t>,
                                                        compaction_output<suite_inline_12_bytes_slot_t>&);
extern template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<suite_inline_44_bytes_slot_t>,
                                                        views_of<suite_inline_44_bytes_slot_t>, compaction_extents_t,
                                                        sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                        fold_ref_t const&, no_device_fold_t const&,
                                                        located_value_reader<suite_inline_44_bytes_slot_t>,
                                                        compaction_output<suite_inline_44_bytes_slot_t>&);
extern template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<suite_fixed_width_slot_t>,
                                                        views_of<suite_fixed_width_slot_t>, compaction_extents_t,
                                                        sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                        fold_ref_t const&, no_device_fold_t const&,
                                                        located_value_reader<suite_fixed_width_slot_t>,
                                                        compaction_output<suite_fixed_width_slot_t>&);
extern template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<suite_variable_width_slot_t>,
                                                        views_of<suite_variable_width_slot_t>, compaction_extents_t,
                                                        sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                        fold_ref_t const&, no_device_fold_t const&,
                                                        located_value_reader<suite_variable_width_slot_t>,
                                                        compaction_output<suite_variable_width_slot_t>&);

/** The graph store's key slots on an Apple device, whose changes its device fold composes. */
extern template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<variable_width_slot_t>,
                                                        views_of<variable_width_slot_t>, compaction_extents_t,
                                                        sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                        graph::adjacency_fold_t const&,
                                                        graph::vram_adjacency_fold_t const&,
                                                        located_value_reader<variable_width_slot_t>,
                                                        compaction_output<variable_width_slot_t>&);
extern template st::status_t metal_compactor_t::compact(compaction_kind_t, views_of<suite_variable_width_slot_t>,
                                                        views_of<suite_variable_width_slot_t>, compaction_extents_t,
                                                        sorted_run_bounds_t const&, compaction_stamps_t const&,
                                                        graph::adjacency_fold_t const&,
                                                        graph::vram_adjacency_fold_t const&,
                                                        located_value_reader<suite_variable_width_slot_t>,
                                                        compaction_output<suite_variable_width_slot_t>&);

} // namespace unum::ustore::blobs
