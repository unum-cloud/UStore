/**
 *  @file src/vram/matching.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief Filtered scans on a VRAM executor: the host lists every page, the devices match its
 *      values, and the host narrows, asks and scores what each device kept.
 *
 *  The calling thread deals the pages across the executor's devices, page i to device i modulo
 *  their count, and no thread is pinned to a device. Each page lists its keys into its device's
 *  pinned bounce, where the host read writes the values tape too. The tape reaches the device in
 *  two copies, into the device's page runs, which every page there rewinds; the library's kernels
 *  walk StringZilla's engine there, marking one bit per value, and the bitmap comes back in one
 *  copy. While the device matches, the host lists the next page onto the next device, and waits on
 *  a device only when its turn comes round again. The host then narrows the candidates as a host
 *  scan does, asking the predicate and scoring with BM25 itself, and the survivors reach the first
 *  device once, when the call ends, as the upload tower moves any host verb's outputs.
 *
 *  The engines compile into the first device's managed runs, which every call on the executor
 *  rewinds first, so a warm call allocates nothing there, and their tables are copied once per call
 *  and device into another block of that device's runs, which the host never touches, so the device
 *  keeps them while the kernels read them through rebased copies of the engines. The host still
 *  reads the original tables during a page, which needs a device with concurrent managed access.
 *  The inputs staging moved into the bounce move on into the runner's scratch before the first
 *  page, which rewinds the bounce.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`, `std::uintptr_t`
#include <cstring> // `std::memcpy`

#include <array>   // `std::array`
#include <memory>  // `std::construct_at`
#include <span>    // `std::span`
#include <utility> // `std::index_sequence`, `std::make_index_sequence`, `std::exchange`
#include <variant> // `std::get_if`

#include "ustore/utilities/ram_buffers.hpp"   // `arena_allocate`, `arena_copy`, `stringzilla_allocator`
#include "ustore/utilities/vram_device.hpp"   // `carve`, `rewind`, `device_blocks_of`, `launch`
#include "ustore/utilities/vram_matching.cuh" // `matching_page_t`, `matching_chunks_t`, `matching_walkers_t`
#include "ustore/utilities/vram_matching.hpp" // `matching_kits_of`

#include "match.hpp"               // `compiled_match`, `candidates_t`, `scan_matched`, `compile_values`
#include "utilities/arguments.hpp" // `a_call_matching_patterns`, `a_call_taking_start_keys`
#include "vram/columns.hpp"        // `upload_output_columns`

namespace unum::ustore {

/** Serves @p bytes off the managed runs of the executor @p handle points to, as a StringZilla allocator does. */
template <util::a_gpu_stream executor_type_>
void* allocate_managed(sz_size_t bytes, void* handle, sz_stream_t) noexcept {
    executor_type_& executor = *static_cast<executor_type_*>(handle);
    auto [block, status] = util::carve<byte_t>(executor.memory.managed, executor, bytes);
    return status.failed() ? nullptr : block.data();
}

/** Grows the managed block at @p pointer into a new one, as a StringZilla allocator does. */
template <util::a_gpu_stream executor_type_>
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
template <util::a_gpu_stream executor_type_>
sz_allocator_t match_memory(executor_type_& executor) noexcept {
    return {allocate_managed<executor_type_>, reallocate_managed<executor_type_>, util::stringzilla_free, &executor};
}

/** Copies the @p bytes at @p memory into a block of @p executor's managed runs, which the host
 *  never touches again, so the device keeps it where its kernels read it until the next call
 *  rewinds the runs. */
template <util::a_gpu_stream executor_type_>
util::expected<byte_t*> copy_for_device(void const* memory, std::size_t bytes, executor_type_& executor) noexcept {
    auto [block, block_status] = util::carve<byte_t>(executor.memory.managed, executor, bytes);
    if (block_status.failed()) return block_status;
    if (util::status_t const copied = executor.upload_async(reinterpret_cast<std::uintptr_t>(block.data()),
                                                            {static_cast<byte_t const*>(memory), bytes});
        copied.failed())
        return copied;
    return block.data();
}

/** Rebases the engine @p match compiled onto a copy of its tables the device keeps, with a
 *  subdocuments engine's nested regex and a regex engine's column images, for the engines the
 *  kernels walk: byte-exact substrings, byte-exact regex with a start automaton and no lookarounds,
 *  and subdocuments paths without filters. */
template <vendored_residency_t residency_>
util::status_t rebase_on_device(compiled_match<residency_>& match, residency_stream<residency_>& executor) noexcept {
    if (sz_substrings_engine_t* const* const substrings = std::get_if<sz_substrings_engine_t*>(&match.matcher)) {
        if ((*substrings)->case_sensitivity != sz_cased_k) return {};
        auto [tables, tables_status] = copy_for_device((*substrings)->memory, (*substrings)->memory_bytes, executor);
        if (tables_status.failed()) return tables_status;
        match.on_device = sz_substrings_engine_rebase_simt_(*substrings, tables);
    }
    else if (sz_regex_engine_t* const* const regex = std::get_if<sz_regex_engine_t*>(&match.matcher)) {
        sz_regex_engine_t const& engine = **regex;
        if (engine.case_sensitivity != sz_cased_k || !engine.reverse_rows || engine.lookarounds_count) return {};
        // The host fills the images before any copy is queued, as a device may forbid touching
        // managed runs meanwhile
        auto [images, images_status] = util::carve<std::uint32_t>(
            executor.memory.managed, executor, sz_regex_images_bytes_simt_(&engine) / sizeof(std::uint32_t));
        if (images_status.failed()) return images_status;
        for (std::size_t column = 0; column != sz_regex_stride_(engine.classes_count, engine.lookarounds_count);
             ++column)
            sz_regex_column_image_simt_(&engine, engine.reverse_rows, engine.reverse_states_count, column,
                                        images.data());
        auto [tables, tables_status] = copy_for_device(engine.memory, engine.memory_bytes, executor);
        if (tables_status.failed()) return tables_status;
        match.on_device = sz_regex_engine_rebase_simt_(&engine, tables);
        match.images = images.data();
    }
    else if (sz_subdocuments_engine_t* const* const paths = std::get_if<sz_subdocuments_engine_t*>(&match.matcher)) {
        sz_subdocuments_engine_t const& engine = **paths;
        if (engine.filters_count) return {};
        auto [tables, tables_status] = copy_for_device(engine.memory, engine.memory_bytes, executor);
        if (tables_status.failed()) return tables_status;
        byte_t* nested = nullptr;
        if (engine.patterns_count) {
            auto [copied, copied_status] = copy_for_device(engine.regex.memory, engine.regex.memory_bytes, executor);
            if (copied_status.failed()) return copied_status;
            nested = copied;
        }
        match.on_device = sz_subdocuments_engine_rebase_simt_(&engine, tables, nested);
    }
    return {};
}

/** Compiles what @p arguments match on @p executor's device: its engine in the executor's
 *  managed runs, which the host walks for the shapes no kernel takes, rebased onto a copy
 *  the library's kernels walk, and the host half, the predicate and the BM25 scorer, all
 *  for @p host_capabilities, laid out in @p into, which must outlive the match. */
template <typename arguments_type_, util::a_gpu_stream executor_type_>
util::expected<compiled_match<vendored_residency_of<executor_type_>>> compile_match(arguments_type_ const& arguments,
                                                                                    ustore_arena_t into,
                                                                                    sz_capability_t host_capabilities,
                                                                                    executor_type_& executor) noexcept {
    if (reads_values(arguments) && !executor.kernels)
        return util::status_t {util::status_code_t::unsupported_k, "The device loaded no kernel bundle"};
    compiled_match<vendored_residency_of<executor_type_>> match;
    if (util::status_t const status = compile_values(arguments, into, host_capabilities, match_memory(executor),
                                                     match.matcher);
        status.failed())
        return status;
    if (util::status_t const status = rebase_on_device(match, executor); status.failed()) return status;
    if constexpr (a_call_matching_patterns<arguments_type_>)
        if (arguments.patterns.weights) {
            value_matcher_t scorer;
            if (util::status_t const status = compile_patterns(arguments.patterns, into, host_capabilities,
                                                               util::stringzilla_allocator(into), scorer);
                status.failed())
                return status;
            match.host.scorer = std::get<sz_substrings_engine_t*>(scorer);
            match.host.weights = arguments.patterns.weights;
        }
    if (arguments.predicate.cost != ustore_predicate_cost_cheap_k &&
        arguments.predicate.cost != ustore_predicate_cost_expensive_k)
        return util::status_t {util::status_code_t::invalid_argument_k, "Unknown predicate cost"};
    match.host.predicate = arguments.predicate;
    return match;
}

/** Longest value one lane walks alone, and longest a group of lanes walks whole, past which a value
 *  cuts into chunks; placeholders until the B300 measurement of each family sets them. */
inline constexpr std::uint64_t matching_thread_bytes_max_k = 256;
inline constexpr std::uint64_t matching_group_bytes_max_k = 64 * 1024;

/** Bytes of one chunk of a long value, at least four times a substrings engine's longest match. */
inline constexpr std::uint64_t matching_chunk_bytes_k = 16 * 1024;

/** Dynamic shared memory a matching kernel stages its engine's head into, which every
 *  vendor grants unasked. */
inline constexpr std::size_t matching_shared_bytes_k = 48 * 1024;

/** Containers a subdocuments walker keeps open at most, past which its page matches on the host. */
inline constexpr std::uint64_t matching_frames_k = 64;

/** The values of one page sorted by how they walk: indices of the short, the medium and
 *  the long ones. */
struct matching_shape_t {
    std::span<std::uint32_t> per_thread;
    std::span<std::uint32_t> per_group;
    std::span<std::uint32_t> per_chunk;
};

/** How a page's values are split over the lanes: by their lengths, or every value walked by one
 *  thread. */
enum class walk_split_t : bool { by_length_k, per_thread_k };

/** Sorts the @p count values at @p offsets into @p work by length, or every value to a lane when
 *  @p split says so. */
template <typename offset_type_>
util::expected<matching_shape_t> shape_of(offset_type_ const* offsets, std::size_t count, walk_split_t split,
                                          ustore_arena_t work) noexcept {
    auto [indices, indices_status] = util::arena_allocate<std::uint32_t>(work, 3 * count);
    if (indices_status.failed()) return indices_status;
    std::size_t threads = 0, groups = 0, chunks = 0;
    for (std::size_t value = 0; value != count; ++value) {
        std::uint64_t const length = offsets[value + 1] - offsets[value];
        std::uint32_t const index = static_cast<std::uint32_t>(value);
        if (split == walk_split_t::per_thread_k || length <= matching_thread_bytes_max_k) indices[threads++] = index;
        else if (length <= matching_group_bytes_max_k) indices[count + groups++] = index;
        else indices[2 * count + chunks++] = index;
    }
    return matching_shape_t {indices.subspan(0, threads), indices.subspan(count, groups),
                             indices.subspan(2 * count, chunks)};
}

/** The page of @p bytes and @p offsets on the device, walking the values @p picked lists and
 *  marking @p contained. */
inline util::matching_page_t page_of(char const* bytes, std::uint64_t const* offsets,
                                     std::span<std::uint32_t const> picked, std::uint32_t* contained) noexcept {
    return {bytes, offsets, picked.data(), static_cast<std::uint32_t>(picked.size()), contained};
}

/** Cuts the long values @p picked lists into chunks of @p chunk_bytes, written into @p work. */
template <typename offset_type_>
util::expected<std::span<util::matching_chunk_t>> chunks_of(offset_type_ const* offsets,
                                                            std::span<std::uint32_t const> picked,
                                                            std::uint64_t chunk_bytes, ustore_arena_t work) noexcept {
    std::size_t count = 0;
    for (std::uint32_t const value : picked)
        count += static_cast<std::size_t>(
            util::divide_round_up<std::uint64_t>(offsets[value + 1] - offsets[value], chunk_bytes));
    auto [chunks, chunks_status] = util::arena_allocate<util::matching_chunk_t>(work, count);
    if (chunks_status.failed()) return chunks_status;
    std::size_t written = 0;
    for (std::uint32_t const value : picked) {
        std::uint64_t const chunks_count = util::divide_round_up<std::uint64_t>(offsets[value + 1] - offsets[value],
                                                                                chunk_bytes);
        for (std::uint64_t index = 0; index != chunks_count; ++index)
            chunks[written++] = util::matching_chunk_t {value, static_cast<std::uint32_t>(index)};
    }
    return chunks;
}

/**
 *  @brief Readies @p match, already rebased onto the first device of @p executor, on every other
 *      one, answering one match per device, laid out in @p scratch.
 *
 *  Each device binds, rewinds its bounce and managed runs, as every call before was waited for, and
 *  takes its own copy of the engine's tables in its managed runs, which its kernels read; the first
 *  device is bound again last, as the rest of the call runs there.
 */
template <vendored_residency_t residency_>
util::expected<std::span<compiled_match<residency_> const>> match_on_every_device(
    compiled_match<residency_> const& match, residency_executor<residency_>& executor,
    ustore_arena_t scratch) noexcept {
    std::size_t const devices_count = executor.size();
    auto [lanes, lanes_status] = util::arena_allocate<compiled_match<residency_>>(scratch, devices_count);
    if (lanes_status.failed()) return lanes_status;
    std::construct_at(&lanes[0], match);
    for (std::size_t lane = 1; lane != devices_count; ++lane) {
        residency_stream<residency_>& device = executor.stream(lane);
        std::construct_at(&lanes[lane], match);
        if (util::status_t const status = device.bind(); status.failed()) return status;
        device.memory.bounce.rewind();
        util::rewind(device.memory.managed, device);
        if (match.reads_values() && !device.kernels)
            return util::status_t {util::status_code_t::unsupported_k, "The device loaded no kernel bundle"};
        if (util::status_t const status = rebase_on_device(lanes[lane], device); status.failed()) return status;
    }
    if (util::status_t const status = executor.stream(0).bind(); status.failed()) return status;
    return std::span<compiled_match<residency_> const> {lanes.data(), devices_count};
}

/**
 *  @brief The face a device match shows the host listings: the predicate and the scores stay on the
 *      host, while the values of every page are matched on one device of the executor.
 *
 *  Each device is a lane: a listing lays page i out in the bounce and the runs of device i modulo
 *  the devices, queues its match there and narrows it once that device comes round again, so every
 *  device matches a page while the host lists the next. A lane's pending page keeps its views and
 *  bitmap in that bounce until it is finished.
 */
template <vendored_residency_t residency_>
class device_match {
  public:
    device_match(std::span<compiled_match<residency_> const> matches, residency_executor<residency_>& executor) noexcept
        : matches_(matches), executor_(executor) {}

    bool reads_values() const noexcept { return matches_[0].reads_values(); }
    bool scores() const noexcept { return matches_[0].host.scores(); }
    bool asks() const noexcept { return matches_[0].host.asks(); }
    bool matches_everything() const noexcept { return matches_[0].matches_everything(); }

    util::status_t ask(ustore_predicate_cost_t phase, std::span<ustore_key_t const> keys, candidates_t& candidates,
                       ustore_arena_t work) const noexcept {
        return matches_[0].host.ask(phase, keys, candidates, work);
    }

    /** Rewinds the first device's bounce, where the call's outputs gather once the pages end. */
    void rewind_page(ustore_arena_t) const noexcept {
        residency_stream<residency_>& device = executor_.stream(0);
        if (device.bind().succeeded()) device.memory.bounce.rewind();
    }

    /** Devices the pages are dealt across, one page in flight on each. */
    std::size_t lanes() const noexcept { return matches_.size(); }

    /** Binds the device of @p lane and rewinds its bounce and runs for its next page, answering its
     *  bounce, where the page lays out. */
    ustore_arena_t open_page(std::size_t lane, ustore_arena_t) const noexcept {
        residency_stream<residency_>& device = executor_.stream(lane);
        [[maybe_unused]] util::status_t const bound = device.bind();
        device.memory.bounce.rewind();
        util::rewind(device.memory.blocks, device);
        return device.memory.bounce.arena();
    }

    /** Queues on the device of @p lane the match of @p values, one per candidate in their order,
     *  laying the host side out in @p work, its bounce; @c finish narrows @p candidates. */
    util::status_t select(strings_arg_t const& values, candidates_t& candidates, ustore_arena_t work,
                          std::size_t lane) const noexcept {
        if (!reads_values() || !candidates.count) return {};
        std::size_t const count = candidates.count;
        auto [views, views_status] = util::arena_allocate<sz_string_view_t>(work, count);
        if (views_status.failed()) return views_status;
        for (std::size_t index = 0; index != count; ++index) {
            ustore_bytes_view_t const value = values[index];
            views[index].start = reinterpret_cast<sz_cptr_t>(value.data);
            views[index].length = static_cast<sz_size_t>(value.length);
        }
        auto [contained, contained_status] = util::arena_allocate<sz_u1x8_t>(work, util::divide_round_up(count, 8));
        if (contained_status.failed()) return contained_status;
        pending_t& pending = pending_[lane];
        pending = {views, contained};
        ustore_strings_view_t const& tape = values.view;
        util::status_t queued;
        if (tape.layout == ustore_strings_u32_tape_k)
            queued = contains(lane, tape.data.u32_tape.data, tape.data.u32_tape.offsets, count);
        else if (tape.layout == ustore_strings_u64_tape_k)
            queued = contains(lane, tape.data.u64_tape.data, tape.data.u64_tape.offsets, count);
        else queued = contains_packed(lane, views, work);
        // A lane that queued part of its work before failing keeps its verdict, so `drain` waits
        if (queued.failed()) pending = {.refused = pending.refused};
        return queued;
    }

    /** Waits for the device of @p lane, matches on the host what it refused, and keeps among
     *  @p candidates those its bitmap marks. */
    util::status_t finish(std::size_t lane, candidates_t& candidates) const noexcept {
        // The lane stays pending until its device was waited for, leaving a failed bind to `drain`
        pending_t const pending = pending_[lane];
        if (pending.refused) {
            residency_stream<residency_>& device = executor_.stream(lane);
            if (util::status_t const status = device.bind(); status.failed()) return status;
            pending_[lane] = {};
            if (util::status_t const status = device.wait(); status.failed()) return status;
            // A document deeper than its walker's stack or malformed, or a regex chunk whose entry
            // stayed undecided, answers on the host
            if (!pending.contained.empty() && *pending.refused)
                if (util::status_t const status = contains_on_host(lane, pending.strings, pending.contained);
                    status.failed())
                    return status;
        }
        pending_[lane] = {};
        if (pending.contained.empty()) return {};
        return matches_[lane].host.narrow(pending.views, pending.contained.data(), candidates);
    }

    /** Waits for every device still matching a page, as their copies write into bounces the next
     *  call rewinds. */
    void drain() const noexcept {
        for (std::size_t lane = 0; lane != lanes(); ++lane) {
            pending_t const pending = std::exchange(pending_[lane], pending_t {});
            residency_stream<residency_>& device = executor_.stream(lane);
            if (!pending.refused || device.bind().failed()) continue;
            [[maybe_unused]] util::status_t const waited = device.wait();
        }
    }

  private:
    using kits_t = util::vram_matching_kits<typename residency_executor<residency_>::scratch_t::function_t>;

    /** The page one lane matches until @c finish: the views and bitmap of its values in the lane's
     *  bounce, the device's verdict on whether it refused any, landing there too, null for a page
     *  matched on the host at once, and the page's tape for a host match after a refusal. */
    struct pending_t {
        std::span<sz_string_view_t> views;
        std::span<sz_u1x8_t> contained;
        std::uint32_t const* refused = nullptr;
        sz_strings_view_t strings {};
    };

    /** Launches a regex kernel on @p device, which on Apple reads StringZilla's walk record after
     *  the parameters CUDA's twin reads from @p engine itself. */
    template <typename function_type_, typename... argument_types_>
    static util::status_t launch_regex_(residency_stream<residency_>& device, function_type_ function, unsigned blocks,
                                        unsigned threads, unsigned shared, sz_regex_engine_t const& engine,
                                        argument_types_... arguments) noexcept {
        if constexpr (residency_ == vendored_residency_t::vram_apple_k)
            return util::launch(device, function, blocks, threads, shared, engine, arguments...,
                                metal_regex_walk(engine));
        else return util::launch(device, function, blocks, threads, shared, engine, arguments...);
    }

    /** Packs the strings @p views name into one tape in @p work, then matches it as @c contains
     *  does. */
    util::status_t contains_packed(std::size_t lane, std::span<sz_string_view_t const> views,
                                   ustore_arena_t work) const noexcept {
        auto [offsets, offsets_status] = util::arena_allocate<std::uint64_t>(work, views.size() + 1);
        if (offsets_status.failed()) return offsets_status;
        offsets[0] = 0;
        for (std::size_t index = 0; index != views.size(); ++index)
            offsets[index + 1] = offsets[index] + views[index].length;
        auto [bytes, bytes_status] = util::arena_allocate<byte_t>(work,
                                                                  static_cast<std::size_t>(offsets[views.size()]));
        if (bytes_status.failed()) return bytes_status;
        for (std::size_t index = 0; index != views.size(); ++index)
            if (views[index].length) std::memcpy(&bytes[offsets[index]], views[index].start, views[index].length);
        return contains(lane, bytes.data(), offsets.data(), views.size());
    }

    /** The tape at @p data and @p offsets of @p count strings, as StringZilla views it. */
    template <typename data_type_, typename offset_type_>
    static sz_strings_view_t strings_of(data_type_ const* data, offset_type_ const* offsets,
                                        std::size_t count) noexcept {
        sz_strings_view_t strings {};
        strings.count = count;
        if constexpr (sizeof(offset_type_) == sizeof(sz_u32_t)) {
            strings.layout = sz_strings_u32_tape_k;
            strings.data.u32_tape.data = reinterpret_cast<sz_cptr_t>(data);
            strings.data.u32_tape.offsets = reinterpret_cast<sz_u32_t const*>(offsets);
        }
        else {
            strings.layout = sz_strings_u64_tape_k;
            strings.data.u64_tape.data = reinterpret_cast<sz_cptr_t>(data);
            strings.data.u64_tape.offsets = reinterpret_cast<sz_u64_t const*>(offsets);
        }
        return strings;
    }

    /** Marks into @p contained which of @p strings, in the bounce, the engine of @p lane contains a
     *  match in, on the host, for the shapes no kernel walks and the pages a walk refused. */
    util::status_t contains_on_host(std::size_t lane, sz_strings_view_t strings,
                                    std::span<sz_u1x8_t> contained) const noexcept {
        compiled_match<residency_> const& match = matches_[lane];
        sz_size_t malformed_index = 0;
        sz_status_t status = sz_success_k;
        if (sz_regex_engine_t* const* const regex = std::get_if<sz_regex_engine_t*>(&match.matcher))
            status = sz_regex_contains(*regex, &strings, contained.data(), nullptr);
        else if (sz_substrings_engine_t* const* const substrings = std::get_if<sz_substrings_engine_t*>(&match.matcher))
            status = sz_substrings_contains(*substrings, &strings, contained.data(), nullptr);
        else if (sz_subdocuments_engine_t* const* const paths = std::get_if<sz_subdocuments_engine_t*>(&match.matcher))
            status = sz_subdocuments_contains(*paths, &strings, contained.data(), &malformed_index, nullptr);
        if (status != sz_success_k)
            return stringzilla_refusal(status, util::status_code_t::invalid_argument_k,
                                       "The matcher rejected the values");
        return {};
    }

    /**
     *  @brief Queues on the device of @p lane the match of the @p count strings of the tape at
     *      @p data and @p offsets, both in its bounce, into the lane's pending bitmap, the device
     *      walking every value its kernels take.
     *
     *  The values sort by length into up to three launches: a lane, a group or chunks per value.
     *  The device's runs hold the tape with its offsets widened to 64 bits, the bitmap, the value
     *  lists and what the walkers keep; the inputs take one copy up each and the bitmap one copy
     *  down, which @c finish waits for. A failure first waits for what was queued, as those
     *  copies read the bounce.
     */
    template <typename data_type_, typename offset_type_>
    util::status_t contains(std::size_t lane, data_type_ const* data, offset_type_ const* offsets,
                            std::size_t count) const noexcept {
        compiled_match<residency_> const& match = matches_[lane];
        pending_t& pending = pending_[lane];
        std::span<sz_u1x8_t> const contained = pending.contained;
        pending.strings = strings_of(data, offsets, count);
        if (std::holds_alternative<match_everything_t>(match.on_device))
            return contains_on_host(lane, pending.strings, contained);
        residency_stream<residency_>& device = executor_.stream(lane);
        util::device_blocks_t& blocks = device.memory.blocks;
        ustore_arena_t const work = device.memory.bounce.arena();
        kits_t const kits = util::matching_kits_of(device.kernels);
        sz_regex_engine_t* const* const regex = std::get_if<sz_regex_engine_t*>(&match.matcher);
        sz_substrings_engine_t* const* const substrings = std::get_if<sz_substrings_engine_t*>(&match.matcher);
        sz_subdocuments_engine_t* const* const paths = std::get_if<sz_subdocuments_engine_t*>(&match.matcher);
        bool const markup = paths && (*paths)->format == sz_subdocuments_xml_k;
        auto [shape, shape_status] = shape_of(offsets, count,
                                              markup ? walk_split_t::per_thread_k : walk_split_t::by_length_k, work);
        if (shape_status.failed()) return shape_status;
        std::uint64_t const chunk_bytes = substrings
                                              ? fu::max_of<std::uint64_t>(matching_chunk_bytes_k,
                                                                          sz_substrings_chunk_bytes_simt_(*substrings))
                                              : matching_chunk_bytes_k;
        auto [chunks, chunks_status] = !paths ? chunks_of(offsets, shape.per_chunk, chunk_bytes, work)
                                              : util::expected<std::span<util::matching_chunk_t>> {
                                                    std::span<util::matching_chunk_t> {}};
        if (chunks_status.failed()) return chunks_status;
        auto [wide, wide_status] = util::arena_allocate<std::uint64_t>(work, count + 1);
        if (wide_status.failed()) return wide_status;
        for (std::size_t index = 0; index <= count; ++index) wide[index] = offsets[index];

        // One walker per value of a subdocuments launch, each with its stack and its key buffer
        std::uint64_t frame_words = 0, key_bytes = 0;
        if (paths) {
            frame_words = sz_subdocuments_frame_words_simt_(*paths);
            key_bytes = util::round_up_to_multiple<std::uint64_t>(sz_subdocuments_key_bytes_(*paths),
                                                                  util::arena_alignment_k);
        }

        std::size_t const walkers = paths ? count : 0;
        std::size_t const data_bytes = static_cast<std::size_t>(offsets[count]);
        std::size_t const words = util::divide_round_up<std::size_t>(count, 32);
        auto [wide_on_device, wide_on_device_status] = util::carve<std::uint64_t>(blocks, device, count + 1);
        auto [bytes_on_device, bytes_on_device_status] = util::carve<char>(blocks, device, data_bytes);
        auto [bits, bits_status] = util::carve<std::uint32_t>(blocks, device, words + 1);
        auto [lists, lists_status] = util::carve<std::uint32_t>(blocks, device, 3 * count);
        auto [chunk_list, chunk_list_status] = util::carve<util::matching_chunk_t>(blocks, device, chunks.size());
        auto [stacks, stacks_status] = util::carve<std::uint64_t>(blocks, device,
                                                                  walkers * matching_frames_k * frame_words);
        auto [keys, keys_status] = util::carve<unsigned char>(blocks, device, walkers * key_bytes);
        auto [refused_on_host, refused_status] = util::arena_allocate<std::uint32_t>(work, 1);
        for (util::status_t const carved : {wide_on_device_status, bytes_on_device_status, bits_status, lists_status,
                                            chunk_list_status, stacks_status, keys_status, refused_status})
            if (carved.failed()) return carved;
        if (util::status_t const status = device.memory.bounce.pin(); status.failed()) return status;
        std::span<data_type_ const> const tape {data, data_bytes};
        std::span<std::uint32_t const> const listed {shape.per_thread.data(), 3 * count};
        for (util::status_t const copied :
             {device.upload_async(reinterpret_cast<std::uintptr_t>(wide_on_device.data()), std::as_bytes(wide)),
              device.upload_async(reinterpret_cast<std::uintptr_t>(bytes_on_device.data()), std::as_bytes(tape)),
              device.upload_async(reinterpret_cast<std::uintptr_t>(lists.data()), std::as_bytes(listed)),
              device.upload_async(reinterpret_cast<std::uintptr_t>(chunk_list.data()), std::as_bytes(chunks)),
              device.fill_async(reinterpret_cast<std::uintptr_t>(bits.data()), byte_t {0},
                                (words + 1) * sizeof(std::uint32_t))})
            if (copied.failed()) return queued_failure(device, copied);

        std::uint32_t* const refused = bits.data() + words;
        auto const page = [&](std::span<std::uint32_t> picked) noexcept {
            std::uint32_t const* const on_device = lists.data() + (picked.data() - shape.per_thread.data());
            return page_of(bytes_on_device.data(), wide_on_device.data(), {on_device, picked.size()}, bits.data());
        };
        unsigned const threads = util::matching_block_threads_k;
        auto const blocks_for = [](std::size_t lanes) noexcept {
            return static_cast<unsigned>(util::divide_round_up<std::size_t>(lanes, threads));
        };
        std::size_t const group_k = util::matching_group_lanes_k;
        util::matching_chunks_t const chunking {page(shape.per_chunk), chunk_list.data(),
                                                static_cast<std::uint32_t>(chunks.size()), chunk_bytes};
        std::uint32_t const* const primed = match.images;
        util::status_t launched;
        if (substrings) {
            sz_substrings_engine_t const& engine = std::get<sz_substrings_engine_t>(match.on_device);
            std::size_t const head = sz_substrings_staged_bytes_simt_(&engine, 0);
            std::uint32_t const staged = static_cast<std::uint32_t>(fu::min_of<std::size_t>(
                engine.hot_count,
                (matching_shared_bytes_k - head) / (sz_substrings_staged_bytes_simt_(&engine, 1) - head)));
            unsigned const shared = static_cast<unsigned>(sz_substrings_staged_bytes_simt_(&engine, staged));
            if (!shape.per_thread.empty())
                launched = util::launch(device, kits.substrings.per_thread, blocks_for(shape.per_thread.size()),
                                        threads, shared, engine, page(shape.per_thread), staged);
            if (launched.succeeded() && !shape.per_group.empty())
                launched = util::launch(device, kits.substrings.per_group, blocks_for(shape.per_group.size() * group_k),
                                        threads, shared, engine, page(shape.per_group), staged);
            if (launched.succeeded() && !chunks.empty())
                launched = util::launch(device, kits.substrings.per_chunk, blocks_for(chunks.size()), threads, shared,
                                        engine, chunking, staged);
        }
        else if (regex) {
            sz_regex_engine_t const& engine = std::get<sz_regex_engine_t>(match.on_device);
            std::size_t const head = sz_regex_staged_bytes_simt_(&engine, 0);
            std::uint32_t const cells = static_cast<std::uint32_t>(fu::min_of<std::size_t>(
                engine.reverse_states_count * sz_regex_stride_(engine.classes_count, engine.lookarounds_count),
                (matching_shared_bytes_k - head) / (sz_regex_staged_bytes_simt_(&engine, 1) - head)));
            unsigned const shared = static_cast<unsigned>(sz_regex_staged_bytes_simt_(&engine, cells));
            if (!shape.per_thread.empty())
                launched = launch_regex_(device, kits.regex.per_thread, blocks_for(shape.per_thread.size()), threads,
                                         shared, engine, page(shape.per_thread), cells);
            if (launched.succeeded() && !shape.per_group.empty())
                launched = launch_regex_(device, kits.regex.per_group, blocks_for(shape.per_group.size() * group_k),
                                         threads, shared, engine, page(shape.per_group), primed, cells);
            if (launched.succeeded() && !chunks.empty())
                launched = launch_regex_(device, kits.regex.per_chunk, blocks_for(chunks.size()), threads, shared,
                                         engine, chunking, primed, cells, refused);
        }
        else {
            sz_subdocuments_engine_t const& engine = std::get<sz_subdocuments_engine_t>(match.on_device);
            // The launches run in stream order, so each reuses the walkers' memory of
            // the one before
            util::matching_walkers_t const walkers_memory {
                stacks.data(), matching_frames_k * frame_words, frame_words, keys.data(), key_bytes, refused};
            if (markup && !shape.per_thread.empty())
                launched = util::launch(device, kits.subdocuments.per_thread, blocks_for(shape.per_thread.size()),
                                        threads, 0u, engine, page(shape.per_thread), walkers_memory);
            // JSON walks as a group however short the value, as its walks classify a window
            // across the lanes
            if (!markup && !shape.per_thread.empty())
                launched = util::launch(device, kits.subdocuments.per_group,
                                        blocks_for(shape.per_thread.size() * group_k), threads, 0u, engine,
                                        page(shape.per_thread), walkers_memory);
            if (launched.succeeded() && !shape.per_group.empty())
                launched = util::launch(device, kits.subdocuments.per_group,
                                        blocks_for(shape.per_group.size() * group_k), threads, 0u, engine,
                                        page(shape.per_group), walkers_memory);
            if (launched.succeeded() && !shape.per_chunk.empty())
                launched = util::launch(device, kits.subdocuments.per_chunk,
                                        blocks_for(shape.per_chunk.size() * group_k), threads, 0u, engine,
                                        page(shape.per_chunk), walkers_memory, chunk_bytes);
        }
        if (launched.failed()) return queued_failure(device, launched);
        util::status_t downloaded = device.download_async(std::as_writable_bytes(contained),
                                                          reinterpret_cast<std::uintptr_t>(bits.data()));
        if (downloaded.succeeded())
            downloaded = device.download_async(std::as_writable_bytes(refused_on_host),
                                               reinterpret_cast<std::uintptr_t>(refused));
        if (downloaded.failed()) return queued_failure(device, downloaded);
        pending.refused = refused_on_host.data();
        return {};
    }

    /** Waits for what @p device already queued, as those copies read and write the bounce the next
     *  page rewinds, and answers @p failure. */
    static util::status_t queued_failure(residency_stream<residency_>& device, util::status_t failure) noexcept {
        [[maybe_unused]] util::status_t const waited = device.wait();
        return failure;
    }

    std::span<compiled_match<residency_> const> matches_;
    residency_executor<residency_>& executor_;
    mutable std::array<pending_t, util::gpu_devices_capacity_k> pending_ {};
};

/** Copies the @p count elements of the strided @p column, its stride apart or one repeated for
 *  zero, into @p scratch and repoints it there, as every page of a device listing rewinds the
 *  bounce, where staging may have put them. */
template <typename element_type_, typename strided_type_>
util::status_t settle(strided_type_& column, std::size_t count, ustore_arena_t scratch) noexcept {
    if (!column.data || !count) return {};
    std::size_t const copied = column.stride ? count : 1;
    auto [copy, copy_status] = util::arena_allocate<element_type_>(scratch, copied);
    if (copy_status.failed()) return copy_status;
    for (std::size_t index = 0; index != copied; ++index)
        std::memcpy(&copy[index], util::bytes_of(column.data, 1).data() + index * column.stride, sizeof(element_type_));
    column.data = copy.data();
    if (column.stride) column.stride = sizeof(element_type_);
    return {};
}

/** Copies @p tape, of @p count strings, into @p scratch and repoints it there, as @c settle
 *  copies a column. */
template <typename tape_type_>
util::status_t settle_tape(tape_type_& tape, std::size_t count, ustore_arena_t scratch) noexcept {
    if (!tape.offsets || !count) return {};
    auto [copied_offsets, offsets_status] = util::arena_copy(scratch, std::span {tape.offsets, count + 1});
    if (offsets_status.failed()) return offsets_status;
    auto [copied_data, data_status] = util::arena_copy(
        scratch, std::span {tape.data, static_cast<std::size_t>(tape.offsets[count])});
    if (data_status.failed()) return data_status;
    tape.data = copied_data.data(), tape.offsets = copied_offsets.data();
    return {};
}

/** Copies the tape @p strings, of @p count strings, into @p scratch as @c settle does, leaving accessors, which write
 *  host memory by contract, in place. */
inline util::status_t settle(ustore_strings_view_t& strings, std::size_t count, ustore_arena_t scratch) noexcept {
    if (strings.layout == ustore_strings_u32_tape_k) return settle_tape(strings.data.u32_tape, count, scratch);
    if (strings.layout == ustore_strings_u64_tape_k) return settle_tape(strings.data.u64_tape, count, scratch);
    return {};
}

/**
 *  @brief Serves the filtered scan @p arguments at @p residency_ through the host verbs of
 *      @p engine, as @c scan_matched serves a host scan, with the values matched on the device and
 *      the outputs moved there once.
 *
 *  The trailing @p rest are the ragged inputs the router gathered, then the view and the executor, as the upload tower
 *  takes them.
 */
template <vendored_residency_t residency_, typename arguments_type_, typename... rest_types_>
util::status_t scan_matched_on_device(residency_tables<vendored_residency_t::host_k> const& engine,
                                      compiled_match<residency_> const& match, arguments_type_ const& arguments,
                                      rest_types_&... rest) noexcept {
    constexpr std::size_t ragged_count = sizeof...(rest_types_) - 2;
    std::tuple<rest_types_&...> const tail {rest...};
    view_t const& view = std::get<ragged_count>(tail);
    residency_stream<residency_>& executor = std::get<ragged_count + 1>(tail);
    util::device_blocks_t* const blocks = util::device_blocks_of(arguments.arena, executor);
    if (!blocks) return {util::status_code_t::device_k, "The arena lives where another executor computes"};

    // The scan drives every stream of the executor, whose claim `visit_residency` holds
    residency_executor<residency_>& devices = std::get<residency_executor<residency_>>(view.executor->device);
    auto [matches, matches_status] = match_on_every_device(match, devices, view.scratch);
    if (matches_status.failed()) return matches_status;
    device_match<residency_> const matching {matches, devices};
    arguments_type_ hosted = arguments;
    hosted.arena = executor.memory.bounce.arena();
    std::size_t const count = static_cast<std::size_t>(arguments.count);
    if (util::status_t const status = settle<ustore_collection_id_t>(hosted.collections, count, view.scratch);
        status.failed())
        return status;
    if constexpr (a_call_taking_start_keys<arguments_type_>)
        if (util::status_t const status = settle<ustore_key_t>(hosted.start_keys, count, view.scratch); status.failed())
            return status;
    util::status_t const served = [&]<std::size_t... index_>(std::index_sequence<index_...>) noexcept {
        std::array<strings_arg_t, ragged_count> settled {std::get<index_>(tail)...};
        for (strings_arg_t& strings : settled)
            if (util::status_t const status = settle(strings.view, count, view.scratch); status.failed()) return status;
        return scan_matched(engine, matching, hosted, settled[index_]..., view);
    }(std::make_index_sequence<ragged_count> {});
    if (served.failed()) return served;
    if (util::status_t const bound = executor.bind(); bound.failed()) return bound;
    return upload_output_columns(hosted, *blocks, executor);
}

} // namespace unum::ustore
