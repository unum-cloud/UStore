/**
 *  @file src/vram/staging.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief Where the inputs of a call on a VRAM executor live, and how they reach the host verbs that serve it.
 *
 *  Every input pointer is classified once per call, through the executor's own vendor first and the
 *  other vendor next. Pinned, managed and pageable host memory is read in place, memory on the
 *  executor's device is downloaded into its pinned bounce, and memory on any other device, of
 *  either vendor, is refused with @c device_k rather than read as host memory. Strings an accessor
 *  writes are host memory by contract.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uintptr_t`

#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`
#include "ustore/utilities/vram_device.hpp" // `gpu_drivers_t`, `a_gpu_executor`, `on_another_vendor`

#include "utilities/arguments.hpp" // `a_call_taking_keys`, `a_call_taking_strings`, `a_call_taking_fields`

namespace unum::ustore {

using util::a_gpu_executor;
using util::gpu_drivers_t;

/** Where one input of a device call lives. */
enum class residence_t : std::uint8_t {

    /** Pinned, managed or pageable host memory, which host verbs read in place. */
    host_k,

    /** The executor's own device. */
    executor_device_k,

    /** Another device of either vendor, which no call of the executor reads. */
    other_device_k,
};

/** Where @p pointer lives as calls on @p executor see it, asking the executor's vendor first and @p drivers' other
 *  vendor next; null reads as host memory. */
template <a_gpu_executor executor_type_>
residence_t residence_of(gpu_drivers_t const& drivers, executor_type_ const& executor, void const* pointer) noexcept {
    if (!pointer) return residence_t::host_k;
    int const device = executor.device_of(pointer);
    if (device >= 0)
        return device == static_cast<int>(executor.device().vendor_ordinal) ? residence_t::executor_device_k
                                                                            : residence_t::other_device_k;
    return util::on_another_vendor<executor_type_>(drivers, pointer) ? residence_t::other_device_k
                                                                     : residence_t::host_k;
}

/** Moves the inputs of one call on @p executor_type_ to the host: each is classified once, and one on the executor's
 *  device is downloaded into the bounce and repointed there, every download waited on once by @c finish. */
template <a_gpu_executor executor_type_>
class input_stager {
  public:
    input_stager(gpu_drivers_t const& drivers, executor_type_& executor) noexcept
        : drivers_(drivers), executor_(executor) {}

    /** A host copy of the @p bytes at @p data where they live on the executor's device, else
     *  @p data as it is. */
    template <typename element_type_>
    [[nodiscard]] element_type_ const* stage(element_type_ const* data, std::size_t bytes) noexcept {
        if (status_.failed() || !data || !bytes) return data;
        residence_t const residence = residence_of(drivers_, executor_, data);
        if (residence == residence_t::host_k) return data;
        if (residence == residence_t::other_device_k) {
            status_ = {util::status_code_t::device_k, "An input lives off the executor's device"};
            return data;
        }
        auto [copy, copy_status] = util::arena_allocate<byte_t>(executor_.bounce(), bytes);
        if (copy_status.failed()) {
            status_ = copy_status;
            return data;
        }
        if ((status_ = executor_.pin_bounce()).failed()) return data;
        status_ = executor_.download_async(copy, reinterpret_cast<std::uintptr_t>(data));
        ++pending_;
        return util::elements_of<element_type_>(copy).data();
    }

    /** Repoints the strided @p column at a host copy of its @p count elements, its stride apart or
     *  one repeated for zero. */
    template <typename strided_type_>
    void stage(strided_type_& column, std::size_t count) noexcept {
        if (!count) return;
        std::size_t const element_bytes = sizeof(*column.data);
        column.data = stage(column.data, column.stride ? column.stride * (count - 1) + element_bytes : element_bytes);
    }

    /** Repoints the tape of @p strings, @p count of them, at host copies, its offsets first, since they size its
     *  bytes. */
    void stage(ustore_strings_view_t& strings, std::size_t count) noexcept {
        if (strings.layout == ustore_strings_u32_tape_k) stage_tape(strings.data.u32_tape, count);
        else if (strings.layout == ustore_strings_u64_tape_k) stage_tape(strings.data.u64_tape, count);
    }

    /** Waits for every download still queued, answering the first failure of the staging. */
    util::status_t finish() noexcept {
        if (status_.failed() || !pending_) return status_;
        pending_ = 0;
        return executor_.wait();
    }

  private:
    template <typename tape_type_>
    void stage_tape(tape_type_& tape, std::size_t count) noexcept {
        if (!count) return;
        tape.offsets = stage(tape.offsets, (count + 1) * sizeof(*tape.offsets));
        if (pending_) status_ = finish();
        if (status_.failed() || !tape.offsets) return;
        tape.data = stage(tape.data, static_cast<std::size_t>(tape.offsets[count]));
    }

    gpu_drivers_t const& drivers_;
    executor_type_& executor_;
    util::status_t status_;

    /** Downloads queued since the last wait. */
    std::size_t pending_ = 0;
};

/** Moves every input of @p arguments, a call on @p executor, to the host as @c input_stager does, finding them by the
 *  names the argument structures give them, and waits once for what it downloaded. */
template <a_gpu_executor executor_type_, typename arguments_type_>
util::status_t stage_inputs(gpu_drivers_t const& drivers, executor_type_& executor,
                            arguments_type_& arguments) noexcept {
    input_stager<executor_type_> stager {drivers, executor};
    std::size_t const count = static_cast<std::size_t>(arguments.count);
    stager.stage(arguments.collections, count);
    if constexpr (a_call_taking_keys<arguments_type_>) stager.stage(arguments.keys, count);
    if constexpr (a_call_taking_start_keys<arguments_type_>) stager.stage(arguments.start_keys, count);
    if constexpr (a_call_taking_vertices<arguments_type_>) stager.stage(arguments.vertices, count);
    if constexpr (a_call_taking_roles<arguments_type_>) stager.stage(arguments.roles, count);
    if constexpr (a_call_taking_elements<arguments_type_>) stager.stage(arguments.elements, count);
    if constexpr (a_call_taking_offsets<arguments_type_>) stager.stage(arguments.offsets, count);
    if constexpr (a_call_taking_limits<arguments_type_>) stager.stage(arguments.limits, count);
    if constexpr (a_call_taking_strings<arguments_type_>) stager.stage(arguments.strings, count);
    if constexpr (a_call_taking_prefixes<arguments_type_>) stager.stage(arguments.prefixes, count);
    if constexpr (a_call_taking_start_strings<arguments_type_>) stager.stage(arguments.start_strings, count);
    if constexpr (a_call_taking_end_strings<arguments_type_>) stager.stage(arguments.end_strings, count);
    if constexpr (a_call_taking_typed_fields<arguments_type_>) {
        std::size_t const fields_count = static_cast<std::size_t>(arguments.fields_count);
        stager.stage(arguments.fields, fields_count);
        arguments.outputs_types = stager.stage(arguments.outputs_types,
                                               fields_count * sizeof(*arguments.outputs_types));
    }
    else if constexpr (a_call_taking_fields<arguments_type_>) stager.stage(arguments.fields, count);
    return stager.finish();
}

} // namespace unum::ustore
