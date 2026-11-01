/**
 *  @file include/ustore/blobs/vram_nvidia_compaction.hpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief The host face of the device compactor: what a database makes once at open
 *      over a CUDA executor and hands every merge the tree sends to the device, whose
 *      kernels @c vram_nvidia_compaction.cuh launches.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <span>        // `std::span`
#include <type_traits> // `std::type_identity_t`
#include <utility>     // `std::exchange`, `std::move`, `std::swap`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <smashtable/shared.hpp>       // `st::status_t`

#include "ustore/blobs/nvme_compaction.hpp" // `compaction_kind_t`, `compaction_output`, `device_sections_reader_t`
#include "ustore/utilities/folds.hpp"       // `fold_concept`
#include "ustore/utilities/ram_buffers.hpp" // `device_blocks_t`
#include "ustore/utilities/types.hpp"       // `expected`
#include "ustore/utilities/vram_nvidia_device.hpp" // `cuda_executor_t`

namespace unum::ustore::blobs {

/**
 *  @brief The device compactor a database's merges run on, made once at open over a CUDA executor
 *      the database owns.
 *
 *  Every merge queues on the executor's stream and carves its regions off device runs the compactor
 *  owns, rewound per merge to the newest and largest, while the host buffers its copies leave from
 *  and land in are carved out of the executor's pinned bounce, rewound per merge, so a warm
 *  compactor allocates nothing. Run sections read by extent through its sections reader where one
 *  reaches the volumes, and upload from the runs' memory otherwise. Its kernels live in the
 *  library's bundles, launched through the driver from @c vram_nvidia_compaction.cuh.
 */
class cuda_compactor_t {
  public:

    /** Binds a compactor to @p executor, which outlives it, reading run sections
     *  through @p sections; refused with @c unsupported_k where the executor's device
     *  loaded no bundle of the library's kernels. */
    static expected<cuda_compactor_t> make(utilities::cuda_executor_t& executor,
                                           device_sections_reader_t sections = {}) noexcept;

    cuda_compactor_t(cuda_compactor_t&& other) noexcept
        : executor_(other.executor_), sections_(other.sections_), blocks_(std::exchange(other.blocks_, {})),
          device_allocations_(std::exchange(other.device_allocations_, 0)), reads_(std::move(other.reads_)) {}
    cuda_compactor_t& operator=(cuda_compactor_t&& other) noexcept {
        std::swap(executor_, other.executor_), std::swap(sections_, other.sections_);
        std::swap(blocks_, other.blocks_), std::swap(device_allocations_, other.device_allocations_);
        std::swap(reads_, other.reads_);
        return *this;
    }
    cuda_compactor_t(cuda_compactor_t const&) = delete;
    cuda_compactor_t& operator=(cuda_compactor_t const&) = delete;
    ~cuda_compactor_t() noexcept;

    /**
     *  @brief Does what @c host_compactor_t::compact does with identical output, composing the
     *      chains of changes alone through @p device_fold and applying the rest on the host
     *      through @p fold.
     *
     *  The host applies every chain that reaches a write, a removal or the bottom
     *  level's empty base.
     *
     *  The @p extents of every run, @p upper ones first, say where its sections sit on its volume
     *  and which stamp an ingest gave it. With @c no_device_fold_t it refuses runs holding a
     *  change with @c invalid_argument_k, and with no change to fold the folds and @p read_value
     *  go unused. A merge whose scratch would pass the executor's limit, or whose planned runs
     *  outgrow what the merge leaves free, refuses with @c out_of_memory_heap_k, so the tree
     *  merges on the host.
     */
    template <typename key_slot_type_, utilities::fold_concept fold_type_, typename device_fold_type_,
              typename allocator_type_>
        requires compactable_key_slot_concept<key_slot_type_>
    st::status_t compact(compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_type_> const> upper,
                         std::span<sorted_run_index_view<key_slot_type_> const> lower,
                         std::span<sorted_run_extents_t const> extents, sorted_run_bounds_t const& bounds,
                         compaction_stamps_t const& stamps, fold_type_ const& fold,
                         device_fold_type_ const& device_fold,
                         std::type_identity_t<located_value_reader<key_slot_type_>> read_value,
                         compaction_output<key_slot_type_, allocator_type_>& output) noexcept;

    /** Device allocations the compactor made, which stop once its scratch holds the largest
     *  merge it serves. */
    std::uint64_t device_allocations() const noexcept { return device_allocations_; }

  private:
    cuda_compactor_t(utilities::cuda_executor_t& executor, device_sections_reader_t sections) noexcept
        : executor_(&executor), sections_(sections) {}

    /** Carves @p count elements off the compactor's device blocks, counting the runs it opens, null
     *  when it cannot. */
    template <typename element_type_>
    element_type_* carve_(std::size_t count) noexcept;

    /** The executor every merge queues on and allocates through, borrowed. */
    utilities::cuda_executor_t* executor_ = nullptr;

    /** The transport run sections read through, empty where every run uploads from its memory. */
    device_sections_reader_t sections_;

    /** The device runs every merge carves its regions from, owned, rewound per merge to the
     *  newest and largest. */
    utilities::device_blocks_t blocks_ {};

    /** Device allocations the blocks made. */
    std::uint64_t device_allocations_ = 0;

    /** The sections the merge reads through @c sections_. */
    st::vector<device_section_read_t> reads_;
};

} // namespace unum::ustore::blobs
