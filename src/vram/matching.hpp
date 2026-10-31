/**
 *  @file src/vram/matching.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief Filtered scans on a VRAM executor: the host lists every page, the device matches its values, and the host
 *      narrows, asks and scores what the device kept.
 *
 *  Each page lists its keys into the executor's pinned bounce, where the host read writes the values tape too. The tape
 *  reaches the device in two copies, into the call's device arena, which every page rewinds; StringZilla's engine
 *  marks one bit per value there, and the bitmap comes back in one copy and one wait. The host then narrows the
 *  candidates as a host scan does, asking the predicate and scoring with BM25 itself, and the survivors reach the
 *  device once, when the call ends, as the upload tower moves any host verb's outputs.
 *
 *  The engines keep their state in the executor's managed runs, which every call on the executor rewinds first, so a
 *  warm call allocates nothing there. The inputs staging moved into the bounce move on into the runner's
 *  scratch before the first page, which rewinds the bounce.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`, `std::uintptr_t`
#include <cstring> // `std::memcpy`

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <tuple>   // `std::tuple`, `std::get`
#include <utility> // `std::index_sequence`, `std::make_index_sequence`
#include <variant> // `std::get_if`

#include <stringzilla/capabilities.h> // `sz_capabilities_compiled_cuda`

#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_copy`, `stringzilla_allocator`
#include "ustore/utilities/vram_device.hpp" // `carve`, `rewind`, `device_blocks_of`, `gpu_executor_concept`

#include "match.hpp"        // `compiled_match`, `candidates_t`, `scan_matched`, `compile_values`
#include "vram/columns.hpp" // `upload_output_columns`

namespace unum::ustore {

/** Serves @p bytes off the managed runs of the executor @p handle points to, as a StringZilla allocator does. */
template <gpu_executor_concept executor_type_>
void* allocate_managed(sz_size_t bytes, void* handle, sz_stream_t) noexcept {
    executor_type_& executor = *static_cast<executor_type_*>(handle);
    auto [block, status] = utilities::carve<byte_t>(executor.managed(), executor, bytes);
    return status.failed() ? nullptr : block.data();
}

/** Grows the managed block at @p pointer into a new one, as a StringZilla allocator does. */
template <gpu_executor_concept executor_type_>
void* reallocate_managed(void* pointer, sz_size_t old_bytes, sz_size_t new_bytes, void* handle,
                         sz_stream_t stream) noexcept {
    if (new_bytes <= old_bytes) return pointer;
    void* const grown = allocate_managed<executor_type_>(new_bytes, handle, stream);
    // StringZilla grows its scratch only once the launches reading it were waited for, so the host may copy it
    if (grown && old_bytes) std::memcpy(grown, pointer, old_bytes);
    return grown;
}

/** The memory a device match lays its engine state out in: the managed runs of @p executor, which the host writes and
 *  the device reads, released all at once when the next call on the executor rewinds them. */
template <gpu_executor_concept executor_type_>
sz_allocator_t match_memory(executor_type_& executor) noexcept {
    return {allocate_managed<executor_type_>, reallocate_managed<executor_type_>, utilities::stringzilla_free,
            &executor};
}

/** The StringZilla capabilities a match on @p executor compiles its device engine for: those of its device that this
 *  build holds kernels for. */
inline sz_capability_t device_match_capabilities(utilities::cuda_executor_t const& executor) noexcept {
    sz_capability_t compiled = 0;
    sz_capabilities_compiled_cuda(&compiled);
    return executor.capabilities() & compiled;
}

/** The capabilities a match on an AMD executor compiles its device engine for, none, as StringZilla's ROCm tier would
 *  link the HIP runtime. */
inline sz_capability_t device_match_capabilities(utilities::hip_executor_t const&) noexcept { return 0; }

/** Compiles what @p arguments match on @p executor's device: its engine for the device's capabilities in the
 *  executor's managed runs, and the host half, the predicate and the BM25 scorer for @p host_capabilities, laid out
 *  in @p into, which must outlive the match. */
template <typename arguments_type_, gpu_executor_concept executor_type_>
expected<compiled_match<home_of_executors<executor_type_>>> compile_match(arguments_type_ const& arguments,
                                                                          ustore_arena_t into,
                                                                          sz_capability_t host_capabilities,
                                                                          executor_type_& executor) noexcept {
    sz_capability_t const device_capabilities = device_match_capabilities(executor);
    if (reads_values(arguments) && !device_capabilities)
        return status_t {status_code_t::unsupported_k, "This device matches no values yet"};
    compiled_match<home_of_executors<executor_type_>> match;
    if (status_t const status = compile_values(arguments, into, device_capabilities, match_memory(executor),
                                               match.matcher);
        status.failed())
        return status;
    if constexpr (requires { arguments.patterns; })
        if (arguments.patterns.weights) {
            value_matcher_t scorer;
            if (status_t const status = compile_patterns(arguments.patterns, into, host_capabilities,
                                                         utilities::stringzilla_allocator(into), scorer);
                status.failed())
                return status;
            match.host.scorer = std::get<sz_substrings_engine_t*>(scorer);
            match.host.weights = arguments.patterns.weights;
        }
    if (arguments.predicate.cost != ustore_predicate_cheap_k &&
        arguments.predicate.cost != ustore_predicate_expensive_k)
        return status_t {status_code_t::invalid_argument_k, "Unknown predicate cost"};
    match.host.predicate = arguments.predicate;
    return match;
}

/**
 *  @brief The face a device match shows the host listings: the predicate and the scores stay on the
 *      host, while the values of every page are matched on the executor's device.
 *
 *  Every page lays out in the executor's bounce and in @p blocks, the call's device arena, both rewound per page, as
 *  every copy queued for the page before was waited for.
 */
template <home_t home_>
class device_match {
  public:
    device_match(compiled_match<home_> const& match, home_executor<home_>& executor,
                 utilities::device_blocks_t& blocks) noexcept
        : match_(match), executor_(executor), blocks_(blocks) {}

    bool reads_values() const noexcept { return match_.reads_values(); }
    bool scores() const noexcept { return match_.host.scores(); }
    bool asks() const noexcept { return match_.host.asks(); }

    status_t ask(ustore_predicate_cost_t phase, std::span<ustore_key_t const> keys, candidates_t& candidates,
                 ustore_arena_t work) const noexcept {
        return match_.host.ask(phase, keys, candidates, work);
    }

    /** Rewinds the bounce, which is the page, and the call's device arena. */
    void rewind_page(ustore_arena_t) const noexcept {
        executor_.rewind_bounce();
        utilities::rewind(blocks_, executor_);
    }

    /** Keeps among @p candidates those whose @p values, one per candidate in their order, the device engine contains a
     *  match in, laying the host side out in @p work, the bounce. */
    status_t select(strings_arg_t const& values, candidates_t& candidates, ustore_arena_t work) const noexcept {
        if (!reads_values() || !candidates.count) return {};
        std::size_t const count = candidates.count;
        auto [views, views_status] = arena_allocate<sz_string_view_t>(work, count);
        if (views_status.failed()) return views_status;
        for (std::size_t index = 0; index != count; ++index) {
            ustore_bytes_view_t const value = values[index];
            views[index].start = reinterpret_cast<sz_cptr_t>(value.data);
            views[index].length = static_cast<sz_size_t>(value.length);
        }
        auto [contained, contained_status] = arena_allocate<sz_u1x8_t>(work, divide_round_up(count, 8));
        if (contained_status.failed()) return contained_status;
        ustore_strings_view_t const& tape = values.view;
        status_t matched;
        if (tape.layout == ustore_strings_u32_tape_k)
            matched = contains(tape.data.u32_tape.data, tape.data.u32_tape.offsets, count, contained);
        else if (tape.layout == ustore_strings_u64_tape_k)
            matched = contains(tape.data.u64_tape.data, tape.data.u64_tape.offsets, count, contained);
        else matched = contains_packed(views, work, contained);
        if (matched.failed()) return matched;
        return match_.host.narrow(views, contained.data(), candidates);
    }

  private:

    /** Packs the strings @p views name into one tape in @p work, then matches it as @c contains does. */
    status_t contains_packed(std::span<sz_string_view_t const> views, ustore_arena_t work,
                             std::span<sz_u1x8_t> contained) const noexcept {
        auto [offsets, offsets_status] = arena_allocate<std::uint64_t>(work, views.size() + 1);
        if (offsets_status.failed()) return offsets_status;
        offsets[0] = 0;
        for (std::size_t index = 0; index != views.size(); ++index)
            offsets[index + 1] = offsets[index] + views[index].length;
        auto [bytes, bytes_status] = arena_allocate<byte_t>(work, static_cast<std::size_t>(offsets[views.size()]));
        if (bytes_status.failed()) return bytes_status;
        for (std::size_t index = 0; index != views.size(); ++index)
            if (views[index].length) std::memcpy(&bytes[offsets[index]], views[index].start, views[index].length);
        return contains(bytes.data(), offsets.data(), views.size(), contained);
    }

    /**
     *  @brief Marks into @p contained, on the host, which of the @p count strings of the tape at @p data and
     *      @p offsets, both in the bounce, the device engine contains a match in.
     *
     *  One carve of the device arena holds the tape and the bitmap; the tape takes two copies up, the bitmap one copy
     *  down, and the page one wait.
     */
    template <typename data_type_, typename offset_type_>
    status_t contains(data_type_ const* data, offset_type_ const* offsets, std::size_t count,
                      std::span<sz_u1x8_t> contained) const noexcept {
        std::size_t const offsets_bytes = (count + 1) * sizeof(offset_type_);
        std::size_t const data_bytes = static_cast<std::size_t>(offsets[count]);
        std::size_t const data_start = round_up_to_multiple(offsets_bytes, arena_alignment_k);
        std::size_t const bitmap_start = data_start + round_up_to_multiple(data_bytes, arena_alignment_k);
        auto [region, region_status] = utilities::carve<byte_t>(blocks_, executor_, bitmap_start + contained.size());
        if (region_status.failed()) return region_status;
        std::uintptr_t const address = reinterpret_cast<std::uintptr_t>(region.data());
        if (status_t const status = executor_.pin_bounce(); status.failed()) return status;
        if (status_t const status = executor_.upload_async(address,
                                                           {reinterpret_cast<byte_t const*>(offsets), offsets_bytes});
            status.failed())
            return status;
        if (data_bytes)
            if (status_t const status = executor_.upload_async(address + data_start,
                                                               {reinterpret_cast<byte_t const*>(data), data_bytes});
                status.failed())
                return status;

        sz_strings_view_t strings {};
        strings.count = count;
        if constexpr (sizeof(offset_type_) == sizeof(sz_u32_t)) {
            strings.layout = sz_strings_u32_tape_k;
            strings.data.u32_tape.data = reinterpret_cast<sz_cptr_t>(address + data_start);
            strings.data.u32_tape.offsets = reinterpret_cast<sz_u32_t const*>(address);
        }
        else {
            strings.layout = sz_strings_u64_tape_k;
            strings.data.u64_tape.data = reinterpret_cast<sz_cptr_t>(address + data_start);
            strings.data.u64_tape.offsets = reinterpret_cast<sz_u64_t const*>(address);
        }
        sz_u1x8_t* const marked = reinterpret_cast<sz_u1x8_t*>(address + bitmap_start);
        sz_stream_t const stream = static_cast<sz_stream_t>(executor_.stream());
        sz_size_t malformed_index = 0;
        sz_status_t status = sz_success_k;
        if (sz_regex_engine_t* const* const regex = std::get_if<sz_regex_engine_t*>(&match_.matcher))
            status = sz_regex_contains(*regex, &strings, marked, stream);
        else if (sz_substrings_engine_t* const* const substrings = std::get_if<sz_substrings_engine_t*>(
                     &match_.matcher))
            status = sz_substrings_contains(*substrings, &strings, marked, stream);
        else if (sz_subdocuments_engine_t* const* const paths = std::get_if<sz_subdocuments_engine_t*>(&match_.matcher))
            status = sz_subdocuments_contains(*paths, &strings, marked, &malformed_index, stream);
        // Copies already queued read the bounce, which the next page rewinds, so a refusal still waits for them
        status_t const downloaded = status == sz_success_k
                                        ? executor_.download_async(std::as_writable_bytes(contained),
                                                                   reinterpret_cast<std::uintptr_t>(marked))
                                        : status_t {};
        status_t const waited = executor_.wait();
        if (status != sz_success_k) return stringzilla_refusal(status);
        return downloaded.failed() ? downloaded : waited;
    }

    compiled_match<home_> const& match_;
    home_executor<home_>& executor_;
    utilities::device_blocks_t& blocks_;
};

/** Copies the @p count elements @p data names, @p stride bytes apart or one repeated for zero, into @p scratch and
 *  repoints them there, as every page of a device listing rewinds the bounce, where staging may have put them. */
template <typename element_type_>
status_t settle(element_type_ const*& data, ustore_size_t& stride, std::size_t count, ustore_arena_t scratch) noexcept {
    if (!data || !count) return {};
    std::size_t const copied = stride ? count : 1;
    auto [copy, copy_status] = arena_allocate<element_type_>(scratch, copied);
    if (copy_status.failed()) return copy_status;
    for (std::size_t index = 0; index != copied; ++index)
        std::memcpy(&copy[index], reinterpret_cast<byte_t const*>(data) + index * stride, sizeof(element_type_));
    data = copy.data();
    if (stride) stride = sizeof(element_type_);
    return {};
}

/** Copies the tape of @p count strings at @p data and @p offsets into @p scratch and repoints it there, as @c settle
 *  copies a column. */
template <typename data_type_, typename offset_type_>
status_t settle(data_type_ const*& data, offset_type_ const*& offsets, std::size_t count,
                ustore_arena_t scratch) noexcept {
    if (!offsets || !count) return {};
    auto [copied_offsets, offsets_status] = arena_copy<offset_type_>(scratch, {offsets, count + 1});
    if (offsets_status.failed()) return offsets_status;
    auto [copied_data, data_status] = arena_copy<data_type_>(scratch, {data, static_cast<std::size_t>(offsets[count])});
    if (data_status.failed()) return data_status;
    data = copied_data.data(), offsets = copied_offsets.data();
    return {};
}

/** Copies the tape @p strings, of @p count strings, into @p scratch as @c settle does, leaving accessors, which write
 *  host memory by contract, in place. */
inline status_t settle(ustore_strings_view_t& strings, std::size_t count, ustore_arena_t scratch) noexcept {
    if (strings.layout == ustore_strings_u32_tape_k)
        return settle(strings.data.u32_tape.data, strings.data.u32_tape.offsets, count, scratch);
    if (strings.layout == ustore_strings_u64_tape_k)
        return settle(strings.data.u64_tape.data, strings.data.u64_tape.offsets, count, scratch);
    return {};
}

/**
 *  @brief Serves the filtered scan @p arguments at @p home_ through the host verbs of @p engine, as @c scan_matched
 *      serves a host scan, with the values matched on the device and the outputs moved there once.
 *
 *  The trailing @p rest are the ragged inputs the router gathered, then the view and the executor, as the upload tower
 *  takes them.
 */
template <home_t home_, typename arguments_type_, typename... rest_types_>
status_t scan_matched_on_device(home_tables<home_t::host_k> const& engine, compiled_match<home_> const& match,
                                arguments_type_ const& arguments, rest_types_&... rest) noexcept {
    constexpr std::size_t ragged_count = sizeof...(rest_types_) - 2;
    std::tuple<rest_types_&...> const tail {rest...};
    view_t const& view = std::get<ragged_count>(tail);
    home_executor<home_>& executor = std::get<ragged_count + 1>(tail);
    utilities::device_blocks_t* const blocks = utilities::device_blocks_of(arguments.arena, executor);
    if (!blocks) return {status_code_t::device_k, "The arena lives where another executor computes"};

    device_match<home_> const matching {match, executor, *blocks};
    arguments_type_ hosted = arguments;
    hosted.arena = executor.bounce();
    std::size_t const count = static_cast<std::size_t>(arguments.count);
    if (status_t const status = settle(hosted.collections.data, hosted.collections.stride, count, view.scratch);
        status.failed())
        return status;
    if constexpr (requires { hosted.start_keys.stride; })
        if (status_t const status = settle(hosted.start_keys.data, hosted.start_keys.stride, count, view.scratch);
            status.failed())
            return status;
    status_t const served = [&]<std::size_t... index_>(std::index_sequence<index_...>) noexcept {
        std::array<strings_arg_t, ragged_count> settled {std::get<index_>(tail)...};
        for (strings_arg_t& strings : settled)
            if (status_t const status = settle(strings.view, count, view.scratch); status.failed()) return status;
        return scan_matched(engine, matching, hosted, settled[index_]..., view);
    }(std::make_index_sequence<ragged_count> {});
    if (served.failed()) return served;
    return upload_output_columns(hosted, *blocks, executor);
}

} // namespace unum::ustore
