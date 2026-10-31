/**
 *  @file src/nvme/compaction_absent.cpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The device compactor of a build without a CUDA compiler, which makes none, so every merge runs on the
 *      host, instantiated for every key slot the engine stores, so the host units of the engine link alike.
 */

#include <span>        // `std::span`
#include <type_traits> // `std::type_identity_t`

#include <smashtable/shared.hpp> // `st::status_t`

#include "ustore/blobs/nvme_compaction.hpp" // `cuda_compactor_t`, `compaction_output`, `located_value_reader`

namespace unum::ustore::blobs {

expected<cuda_compactor_t> cuda_compactor_t::make(utilities::cuda_executor_t&, device_sections_reader_t) noexcept {
    return status_t {status_code_t::unsupported_k, "This build carries no CUDA compactor"};
}

cuda_compactor_t::~cuda_compactor_t() noexcept {}

/** Refuses as a device compactor refuses a merge it cannot place, which the tree then runs on the host. */
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

} // namespace unum::ustore::blobs

#include "nvme/compaction_variants.hpp" // `views_of`, `variable_width_slot_t`
