/**
 *  @file test/nvme/compaction_absent.cpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The device compactor of the compaction suite's host build, which makes none, so every device case skips,
 *      instantiated for the key slots @c compaction_device.cu instantiates, so the suite links alike.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <span>        // `std::span`
#include <type_traits> // `std::type_identity_t`

#include "ustore/blobs/nvme_compaction.hpp" // `cuda_compactor_t`, `compaction_output`, `sorted_run_index_view`
#include "ustore/graph/adjacency_fold.hpp"  // `adjacency_fold_t`, `vram_nvidia_adjacency_fold_t`
#include "ustore/utilities/folds.hpp"       // `last_write_fold_t`

#include "nvme/compaction_key_slots.hpp" // `inline_key_slot`, `fixed_width_key_slot`, `variable_width_key_slot`

namespace unum::ustore {

using blobs::compactable_key_slot_concept;
using blobs::compaction_kind_t;
using blobs::compaction_output;
using blobs::compaction_stamps_t;
using blobs::cuda_compactor_t;
using blobs::sorted_run_bounds_t;
using st::key128_t;
using utilities::last_write_fold_t;

expected<cuda_compactor_t> cuda_compactor_t::make(utilities::cuda_executor_t&, device_sections_reader_t) noexcept {
    return status_t {status_code_t::unsupported_k, "The suite's host build carries no CUDA compactor"};
}

cuda_compactor_t::~cuda_compactor_t() noexcept {}

template <typename key_slot_type_, utilities::fold_concept fold_type_, typename device_fold_type_,
          typename allocator_type_>
    requires compactable_key_slot_concept<key_slot_type_>
st::status_t cuda_compactor_t::compact(compaction_kind_t, std::span<sorted_run_index_view<key_slot_type_> const>,
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
