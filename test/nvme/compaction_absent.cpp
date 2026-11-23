/**
 *  @file test/nvme/compaction_absent.cpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The device compactors of the compaction suite's host build, which make none on either
 *      vendor's device, so every device case skips, instantiated for the key slots the suite
 *      merges, and no table of kernels, as this build links no bundle.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstdint> // `std::uint64_t`

#include <span>        // `std::span`
#include <type_traits> // `std::type_identity_t`

#include "ustore/blobs/nvme_compaction.hpp"        // `compaction_output`, `fold_ref_t`
#include "ustore/blobs/vram_amd_compaction.hpp"    // `hip_compactor_t`
#include "ustore/blobs/vram_nvidia_compaction.hpp" // `cuda_compactor_t`
#include "ustore/graph/adjacency_fold.hpp"         // `adjacency_fold_t`
#include "ustore/graph/vram_adjacency_fold.hpp"    // `vram_adjacency_fold_t`
#include "ustore/utilities/vram_device.hpp"        // `kernel_table`, `gpu_drivers_t`

#include "nvme/compaction_key_slots.hpp" // `inline_key_slot`, `fixed_width_key_slot`, `variable_width_key_slot`
#include "vram/classes.hpp"              // `kernel_tables`

namespace unum::ustore {

using blobs::compactable_key_slot_concept;
using blobs::compaction_kind_t;
using blobs::compaction_output;
using blobs::compaction_stamps_t;
using blobs::cuda_compactor_t;
using blobs::hip_compactor_t;
using blobs::sorted_run_bounds_t;
using st::key128_t;

util::expected<cuda_compactor_t> cuda_compactor_t::make(util::cuda_executor_t&, device_sections_reader_t) noexcept {
    return util::status_t {util::status_code_t::unsupported_k, "The suite's host build carries no device compactor"};
}

cuda_compactor_t::~cuda_compactor_t() noexcept {}

template <typename key_slot_type_, util::fold_concept fold_type_, typename device_fold_type_, typename allocator_type_>
    requires compactable_key_slot_concept<key_slot_type_>
st::status_t cuda_compactor_t::compact(compaction_kind_t, std::span<sorted_run_index_view<key_slot_type_> const>,
                                       std::span<sorted_run_index_view<key_slot_type_> const>,
                                       std::span<sorted_run_extents_t const>, sorted_run_bounds_t const&,
                                       compaction_stamps_t const&, fold_type_ const&, device_fold_type_ const&,
                                       std::type_identity_t<located_value_reader<key_slot_type_>>,
                                       compaction_output<key_slot_type_, allocator_type_>&) noexcept {
    return st::status_t::invalid_argument_k;
}

util::expected<hip_compactor_t> hip_compactor_t::make(util::hip_executor_t&, device_sections_reader_t) noexcept {
    return util::status_t {util::status_code_t::unsupported_k, "The suite's host build carries no device compactor"};
}

hip_compactor_t::~hip_compactor_t() noexcept {}

template <typename key_slot_type_, util::fold_concept fold_type_, typename device_fold_type_, typename allocator_type_>
    requires compactable_key_slot_concept<key_slot_type_>
st::status_t hip_compactor_t::compact(compaction_kind_t, std::span<sorted_run_index_view<key_slot_type_> const>,
                                      std::span<sorted_run_index_view<key_slot_type_> const>,
                                      std::span<sorted_run_extents_t const>, sorted_run_bounds_t const&,
                                      compaction_stamps_t const&, fold_type_ const&, device_fold_type_ const&,
                                      std::type_identity_t<located_value_reader<key_slot_type_>>,
                                      compaction_output<key_slot_type_, allocator_type_>&) noexcept {
    return st::status_t::invalid_argument_k;
}

/** The sorted run indexes of one LSM level, as a compaction takes both of its levels. */
template <typename key_slot_type_>
using views_of = std::span<blobs::sorted_run_index_view<key_slot_type_> const>;

/** The suite's key slots on an NVIDIA device. */
template st::status_t blobs::cuda_compactor_t::compact(
    compaction_kind_t, views_of<test::nvme::inline_key_slot<std::uint64_t>>,
    views_of<test::nvme::inline_key_slot<std::uint64_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, blobs::fold_ref_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::nvme::inline_key_slot<std::uint64_t>>,
    compaction_output<test::nvme::inline_key_slot<std::uint64_t>>&);
template st::status_t blobs::cuda_compactor_t::compact(
    compaction_kind_t, views_of<test::nvme::fixed_width_key_slot<std::uint64_t>>,
    views_of<test::nvme::fixed_width_key_slot<std::uint64_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, blobs::fold_ref_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::nvme::fixed_width_key_slot<std::uint64_t>>,
    compaction_output<test::nvme::fixed_width_key_slot<std::uint64_t>>&);
template st::status_t blobs::cuda_compactor_t::compact(
    compaction_kind_t, views_of<test::nvme::variable_width_key_slot<std::uint64_t>>,
    views_of<test::nvme::variable_width_key_slot<std::uint64_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, blobs::fold_ref_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::nvme::variable_width_key_slot<std::uint64_t>>,
    compaction_output<test::nvme::variable_width_key_slot<std::uint64_t>>&);
template st::status_t blobs::cuda_compactor_t::compact(
    compaction_kind_t, views_of<test::nvme::inline_key_slot<key128_t>>, views_of<test::nvme::inline_key_slot<key128_t>>,
    std::span<blobs::sorted_run_extents_t const>, sorted_run_bounds_t const&, compaction_stamps_t const&,
    blobs::fold_ref_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::nvme::inline_key_slot<key128_t>>,
    compaction_output<test::nvme::inline_key_slot<key128_t>>&);
template st::status_t blobs::cuda_compactor_t::compact(
    compaction_kind_t, views_of<test::nvme::fixed_width_key_slot<key128_t>>,
    views_of<test::nvme::fixed_width_key_slot<key128_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, blobs::fold_ref_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::nvme::fixed_width_key_slot<key128_t>>,
    compaction_output<test::nvme::fixed_width_key_slot<key128_t>>&);
template st::status_t blobs::cuda_compactor_t::compact(
    compaction_kind_t, views_of<test::nvme::variable_width_key_slot<key128_t>>,
    views_of<test::nvme::variable_width_key_slot<key128_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, blobs::fold_ref_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::nvme::variable_width_key_slot<key128_t>>,
    compaction_output<test::nvme::variable_width_key_slot<key128_t>>&);
template st::status_t blobs::cuda_compactor_t::compact(
    compaction_kind_t, views_of<test::nvme::variable_width_key_slot<key128_t>>,
    views_of<test::nvme::variable_width_key_slot<key128_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, graph::adjacency_fold_t const&,
    graph::vram_adjacency_fold_t const&, blobs::located_value_reader<test::nvme::variable_width_key_slot<key128_t>>,
    compaction_output<test::nvme::variable_width_key_slot<key128_t>>&);

/** The suite's key slots on an AMD device. */
template st::status_t blobs::hip_compactor_t::compact(
    compaction_kind_t, views_of<test::nvme::inline_key_slot<std::uint64_t>>,
    views_of<test::nvme::inline_key_slot<std::uint64_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, blobs::fold_ref_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::nvme::inline_key_slot<std::uint64_t>>,
    compaction_output<test::nvme::inline_key_slot<std::uint64_t>>&);
template st::status_t blobs::hip_compactor_t::compact(
    compaction_kind_t, views_of<test::nvme::fixed_width_key_slot<std::uint64_t>>,
    views_of<test::nvme::fixed_width_key_slot<std::uint64_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, blobs::fold_ref_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::nvme::fixed_width_key_slot<std::uint64_t>>,
    compaction_output<test::nvme::fixed_width_key_slot<std::uint64_t>>&);
template st::status_t blobs::hip_compactor_t::compact(
    compaction_kind_t, views_of<test::nvme::variable_width_key_slot<std::uint64_t>>,
    views_of<test::nvme::variable_width_key_slot<std::uint64_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, blobs::fold_ref_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::nvme::variable_width_key_slot<std::uint64_t>>,
    compaction_output<test::nvme::variable_width_key_slot<std::uint64_t>>&);
template st::status_t blobs::hip_compactor_t::compact(
    compaction_kind_t, views_of<test::nvme::inline_key_slot<key128_t>>, views_of<test::nvme::inline_key_slot<key128_t>>,
    std::span<blobs::sorted_run_extents_t const>, sorted_run_bounds_t const&, compaction_stamps_t const&,
    blobs::fold_ref_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::nvme::inline_key_slot<key128_t>>,
    compaction_output<test::nvme::inline_key_slot<key128_t>>&);
template st::status_t blobs::hip_compactor_t::compact(
    compaction_kind_t, views_of<test::nvme::fixed_width_key_slot<key128_t>>,
    views_of<test::nvme::fixed_width_key_slot<key128_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, blobs::fold_ref_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::nvme::fixed_width_key_slot<key128_t>>,
    compaction_output<test::nvme::fixed_width_key_slot<key128_t>>&);
template st::status_t blobs::hip_compactor_t::compact(
    compaction_kind_t, views_of<test::nvme::variable_width_key_slot<key128_t>>,
    views_of<test::nvme::variable_width_key_slot<key128_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, blobs::fold_ref_t const&, blobs::no_device_fold_t const&,
    blobs::located_value_reader<test::nvme::variable_width_key_slot<key128_t>>,
    compaction_output<test::nvme::variable_width_key_slot<key128_t>>&);
template st::status_t blobs::hip_compactor_t::compact(
    compaction_kind_t, views_of<test::nvme::variable_width_key_slot<key128_t>>,
    views_of<test::nvme::variable_width_key_slot<key128_t>>, std::span<blobs::sorted_run_extents_t const>,
    sorted_run_bounds_t const&, compaction_stamps_t const&, graph::adjacency_fold_t const&,
    graph::vram_adjacency_fold_t const&, blobs::located_value_reader<test::nvme::variable_width_key_slot<key128_t>>,
    compaction_output<test::nvme::variable_width_key_slot<key128_t>>&);

} // namespace unum::ustore

using namespace unum::ustore;

util::kernel_table<util::cuda_module_t> const* suite_kernels(util::gpu_drivers_t&,
                                                             kernel_tables<util::cuda_module_t>&) noexcept {
    return nullptr;
}

util::kernel_table<util::hip_module_t> const* suite_kernels(util::gpu_drivers_t&,
                                                            kernel_tables<util::hip_module_t>&) noexcept {
    return nullptr;
}
