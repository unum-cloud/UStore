/**
 *  @file src/vram/staging.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief Where the inputs of a call on a VRAM executor live, and how they reach the host verbs that serve it.
 *
 *  Every input pointer is classified once per call, through the executor's own vendor first and the other vendor
 *  next. Pinned, managed and pageable host memory is read in place, memory on the executor's device is downloaded into
 *  its pinned bounce, and memory on any other device, of either vendor, is refused with @c device_k rather than read
 *  as host memory. Strings an accessor writes are host memory by contract.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uintptr_t`

#include <span> // `std::span`

#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`
#include "ustore/utilities/vram_device.hpp" // `gpu_drivers_t`, `gpu_executor_concept`, `on_another_vendor`

namespace unum::ustore {

using utilities::gpu_drivers_t;
using utilities::gpu_executor_concept;

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
template <gpu_executor_concept executor_type_>
residence_t residence_of(gpu_drivers_t const& drivers, executor_type_ const& executor, void const* pointer) noexcept {
    if (!pointer) return residence_t::host_k;
    int const device = executor.device_of(pointer);
    if (device >= 0) return device == executor.device() ? residence_t::executor_device_k : residence_t::other_device_k;
    return utilities::on_another_vendor<executor_type_>(drivers, pointer) ? residence_t::other_device_k
                                                                          : residence_t::host_k;
}

/** Moves the inputs of one call on @p executor_type_ to the host: each is classified once, and one on the executor's
 *  device is downloaded into the bounce and repointed there, every download waited on once by @c finish. */
template <gpu_executor_concept executor_type_>
class input_stager {
  public:
    input_stager(gpu_drivers_t const& drivers, executor_type_& executor) noexcept
        : drivers_(drivers), executor_(executor) {}

    /** Repoints @p data at a host copy of its @p bytes when they live on the executor's device. */
    template <typename element_type_>
    void stage(element_type_ const*& data, std::size_t bytes) noexcept {
        if (status_.failed() || !data || !bytes) return;
        residence_t const residence = residence_of(drivers_, executor_, data);
        if (residence == residence_t::host_k) return;
        if (residence == residence_t::other_device_k) {
            status_ = {status_code_t::device_k, "An input lives on a device the executor does not compute on"};
            return;
        }
        auto [copy, copy_status] = utilities::arena_allocate<byte_t>(executor_.bounce(), bytes);
        if (copy_status.failed()) {
            status_ = copy_status;
            return;
        }
        if ((status_ = executor_.pin_bounce()).failed()) return;
        status_ = executor_.download_async(copy, reinterpret_cast<std::uintptr_t>(data));
        data = reinterpret_cast<element_type_ const*>(copy.data());
        ++pending_;
    }

    /** Repoints @p data at a host copy of its @p count elements, @p stride bytes apart or one repeated for zero. */
    template <typename element_type_>
    void stage(element_type_ const*& data, std::size_t stride, std::size_t count) noexcept {
        if (!count) return;
        stage(data, stride ? stride * (count - 1) + sizeof(element_type_) : sizeof(element_type_));
    }

    /** Repoints the tape of @p strings, @p count of them, at host copies, its offsets first, since they size its
     *  bytes. */
    void stage(ustore_strings_view_t& strings, std::size_t count) noexcept {
        if (strings.layout == ustore_strings_u32_tape_k) stage_tape(strings.data.u32_tape, count);
        else if (strings.layout == ustore_strings_u64_tape_k) stage_tape(strings.data.u64_tape, count);
    }

    /** Waits for every download still queued, answering the first failure of the staging. */
    status_t finish() noexcept {
        if (status_.failed() || !pending_) return status_;
        pending_ = 0;
        return executor_.wait();
    }

  private:
    template <typename tape_type_>
    void stage_tape(tape_type_& tape, std::size_t count) noexcept {
        if (!count) return;
        stage(tape.offsets, (count + 1) * sizeof(*tape.offsets));
        if (pending_) status_ = finish();
        if (status_.failed() || !tape.offsets) return;
        stage(tape.data, static_cast<std::size_t>(tape.offsets[count]));
    }

    gpu_drivers_t const& drivers_;
    executor_type_& executor_;
    status_t status_;

    /** Downloads queued since the last wait. */
    std::size_t pending_ = 0;
};

/** Moves every input of @p arguments, a call on @p executor, to the host as @c input_stager does, finding them by the
 *  names the argument structures give them, and waits once for what it downloaded. */
template <gpu_executor_concept executor_type_, typename arguments_type_>
status_t stage_inputs(gpu_drivers_t const& drivers, executor_type_& executor, arguments_type_& arguments) noexcept {
    input_stager<executor_type_> stager {drivers, executor};
    std::size_t const count = static_cast<std::size_t>(arguments.count);
    stager.stage(arguments.collections.data, arguments.collections.stride, count);
    if constexpr (requires { arguments.keys.stride; }) stager.stage(arguments.keys.data, arguments.keys.stride, count);
    if constexpr (requires { arguments.start_keys.stride; })
        stager.stage(arguments.start_keys.data, arguments.start_keys.stride, count);
    if constexpr (requires { arguments.vertices.stride; })
        stager.stage(arguments.vertices.data, arguments.vertices.stride, count);
    if constexpr (requires { arguments.roles.stride; })
        stager.stage(arguments.roles.data, arguments.roles.stride, count);
    if constexpr (requires { arguments.elements.stride; })
        stager.stage(arguments.elements.data, arguments.elements.stride, count);
    if constexpr (requires { arguments.strings.layout; }) stager.stage(arguments.strings, count);
    if constexpr (requires { arguments.prefixes.layout; }) stager.stage(arguments.prefixes, count);
    if constexpr (requires { arguments.start_strings.layout; }) stager.stage(arguments.start_strings, count);
    if constexpr (requires { arguments.fields_count; }) {
        std::size_t const fields_count = static_cast<std::size_t>(arguments.fields_count);
        stager.stage(arguments.fields, fields_count);
        stager.stage(arguments.outputs_types, fields_count * sizeof(*arguments.outputs_types));
    }
    else if constexpr (requires { arguments.fields.layout; }) stager.stage(arguments.fields, count);
    return stager.finish();
}

} // namespace unum::ustore
