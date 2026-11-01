/**
 *  @file src/vram/matching.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief Filtered scans on a VRAM executor: the host lists every page, the device matches its values, and the host
 *      narrows, asks and scores what the device kept.
 *
 *  Each page lists its keys into the executor's pinned bounce, where the host read writes the
 *  values tape too. The tape reaches the device in two copies, into the call's device arena, which
 *  every page rewinds; the library's kernels walk StringZilla's engine there, marking one bit per
 *  value, and the bitmap comes back in one copy and one wait. The host then narrows the candidates
 *  as a host scan does, asking the predicate and scoring with BM25 itself, and the survivors reach
 *  the device once, when the call ends, as the upload tower moves any host verb's outputs.
 *
 *  The engines compile into the executor's managed runs, which every call on the executor
 *  rewinds first, so a warm call allocates nothing there, and their tables are copied once per
 *  call into another block of those runs, which the host never touches, so the device keeps them
 *  while the kernels read them through rebased copies of the engines. The host still reads the
 *  original tables during a page, which needs a device with concurrent managed access. The
 *  inputs staging moved into the bounce move on into the runner's scratch before the first page,
 *  which rewinds the bounce.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`, `std::uintptr_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::max`, `std::min`
#include <array>     // `std::array`
#include <span>      // `std::span`
#include <utility>   // `std::index_sequence`, `std::make_index_sequence`
#include <variant>   // `std::get_if`

#include "ustore/utilities/ram_buffers.hpp"   // `arena_allocate`, `arena_copy`, `stringzilla_allocator`
#include "ustore/utilities/vram_device.hpp"   // `carve`, `rewind`, `device_blocks_of`, `launch`
#include "ustore/utilities/vram_matching.cuh" // `matching_page_t`, `matching_chunks_t`, `matching_walkers_t`
#include "ustore/utilities/vram_matching.hpp" // `matching_kits_of`

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

/** Copies the @p bytes at @p memory into a block of @p executor's managed runs, which the host
 *  never touches again, so the device keeps it where its kernels read it until the next call
 *  rewinds the runs. */
template <gpu_executor_concept executor_type_>
expected<byte_t*> copy_for_device(void const* memory, std::size_t bytes, executor_type_& executor) noexcept {
    auto [block, block_status] = utilities::carve<byte_t>(executor.managed(), executor, bytes);
    if (block_status.failed()) return block_status;
    if (status_t const copied = executor.upload_async(reinterpret_cast<std::uintptr_t>(block.data()),
                                                      {static_cast<byte_t const*>(memory), bytes});
        copied.failed())
        return copied;
    return block.data();
}

/** Rebases the engine @p match compiled onto a copy of its tables the device keeps, with a
 *  subdocuments engine's nested regex and a regex engine's column images, for the engines the
 *  kernels walk: byte-exact substrings, byte-exact regex with a start automaton and no lookarounds,
 *  and subdocuments paths without filters. */
template <home_t home_>
status_t rebase_on_device(compiled_match<home_>& match, home_executor<home_>& executor) noexcept {
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
        auto [images, images_status] = utilities::carve<std::uint32_t>(
            executor.managed(), executor, sz_regex_images_bytes_simt_(&engine) / sizeof(std::uint32_t));
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
template <typename arguments_type_, gpu_executor_concept executor_type_>
expected<compiled_match<home_of_executors<executor_type_>>> compile_match(arguments_type_ const& arguments,
                                                                          ustore_arena_t into,
                                                                          sz_capability_t host_capabilities,
                                                                          executor_type_& executor) noexcept {
    if (reads_values(arguments) && !executor.kernels())
        return status_t {status_code_t::unsupported_k, "The device loaded no bundle of the library's kernels"};
    compiled_match<home_of_executors<executor_type_>> match;
    if (status_t const status = compile_values(arguments, into, host_capabilities, match_memory(executor),
                                               match.matcher);
        status.failed())
        return status;
    if (status_t const status = rebase_on_device(match, executor); status.failed()) return status;
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
expected<matching_shape_t> shape_of(offset_type_ const* offsets, std::size_t count, walk_split_t split,
                                    ustore_arena_t work) noexcept {
    auto [indices, indices_status] = arena_allocate<std::uint32_t>(work, 3 * count);
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
inline matching_page_t page_of(char const* bytes, std::uint64_t const* offsets, std::span<std::uint32_t const> picked,
                               std::uint32_t* contained) noexcept {
    return {bytes, offsets, picked.data(), static_cast<std::uint32_t>(picked.size()), contained};
}

/** Cuts the long values @p picked lists into chunks of @p chunk_bytes, written into @p work. */
template <typename offset_type_>
expected<std::span<matching_chunk_t>> chunks_of(offset_type_ const* offsets, std::span<std::uint32_t const> picked,
                                                std::uint64_t chunk_bytes, ustore_arena_t work) noexcept {
    std::size_t count = 0;
    for (std::uint32_t const value : picked)
        count += static_cast<std::size_t>(
            divide_round_up<std::uint64_t>(offsets[value + 1] - offsets[value], chunk_bytes));
    auto [chunks, chunks_status] = arena_allocate<matching_chunk_t>(work, count);
    if (chunks_status.failed()) return chunks_status;
    std::size_t written = 0;
    for (std::uint32_t const value : picked) {
        std::uint64_t const chunks_count = divide_round_up<std::uint64_t>(offsets[value + 1] - offsets[value],
                                                                          chunk_bytes);
        for (std::uint64_t index = 0; index != chunks_count; ++index)
            chunks[written++] = matching_chunk_t {value, static_cast<std::uint32_t>(index)};
    }
    return chunks;
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
    using kits_t = utilities::vram_matching_kits<typename home_executor<home_>::function_t>;

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

    /** Marks into @p contained which of the @p count strings of the tape at @p data and @p offsets,
     *  both in the bounce, the engine contains a match in, on the host, for the shapes no kernel
     *  walks and the pages a walk refused. */
    template <typename data_type_, typename offset_type_>
    status_t contains_on_host(data_type_ const* data, offset_type_ const* offsets, std::size_t count,
                              std::span<sz_u1x8_t> contained) const noexcept {
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
        sz_size_t malformed_index = 0;
        sz_status_t status = sz_success_k;
        if (sz_regex_engine_t* const* const regex = std::get_if<sz_regex_engine_t*>(&match_.matcher))
            status = sz_regex_contains(*regex, &strings, contained.data(), nullptr);
        else if (sz_substrings_engine_t* const* const substrings = std::get_if<sz_substrings_engine_t*>(
                     &match_.matcher))
            status = sz_substrings_contains(*substrings, &strings, contained.data(), nullptr);
        else if (sz_subdocuments_engine_t* const* const paths = std::get_if<sz_subdocuments_engine_t*>(&match_.matcher))
            status = sz_subdocuments_contains(*paths, &strings, contained.data(), &malformed_index, nullptr);
        if (status != sz_success_k)
            return stringzilla_refusal(status, status_code_t::invalid_argument_k, "Failed to match the values");
        return {};
    }

    /**
     *  @brief Marks into @p contained, on the host, which of the @p count strings of the tape
     *      at @p data and @p offsets, both in the bounce, the engine contains a match in, the
     *      device walking every value its kernels take.
     *
     *  The values sort by length into up to three launches: a lane, a group or chunks per value.
     *  The call's device arena holds the tape with its offsets widened to 64 bits, the bitmap, the
     *  value lists and what the walkers keep; the inputs take one copy up each, the bitmap one copy
     *  down, and the page one wait.
     */
    template <typename data_type_, typename offset_type_>
    status_t contains(data_type_ const* data, offset_type_ const* offsets, std::size_t count,
                      std::span<sz_u1x8_t> contained) const noexcept {
        if (std::holds_alternative<match_everything_t>(match_.on_device))
            return contains_on_host(data, offsets, count, contained);
        ustore_arena_t const work = executor_.bounce();
        kits_t const kits = utilities::matching_kits_of(executor_.kernels());
        sz_regex_engine_t* const* const regex = std::get_if<sz_regex_engine_t*>(&match_.matcher);
        sz_substrings_engine_t* const* const substrings = std::get_if<sz_substrings_engine_t*>(&match_.matcher);
        sz_subdocuments_engine_t* const* const paths = std::get_if<sz_subdocuments_engine_t*>(&match_.matcher);
        bool const markup = paths && (*paths)->format == sz_subdocuments_xml_k;
        auto [shape, shape_status] = shape_of(offsets, count,
                                              markup ? walk_split_t::per_thread_k : walk_split_t::by_length_k, work);
        if (shape_status.failed()) return shape_status;
        std::uint64_t const chunk_bytes = substrings
                                              ? std::max<std::uint64_t>(matching_chunk_bytes_k,
                                                                        sz_substrings_chunk_bytes_simt_(*substrings))
                                              : matching_chunk_bytes_k;
        auto [chunks, chunks_status] = !paths ? chunks_of(offsets, shape.per_chunk, chunk_bytes, work)
                                              : expected<std::span<matching_chunk_t>> {std::span<matching_chunk_t> {}};
        if (chunks_status.failed()) return chunks_status;
        auto [wide, wide_status] = arena_allocate<std::uint64_t>(work, count + 1);
        if (wide_status.failed()) return wide_status;
        for (std::size_t index = 0; index <= count; ++index) wide[index] = offsets[index];

        // One walker per value of a subdocuments launch, each with its stack and its key buffer
        std::uint64_t frame_words = 0, key_bytes = 0;
        if (paths) {
            frame_words = sz_subdocuments_frame_words_simt_(*paths);
            key_bytes = round_up_to_multiple<std::uint64_t>(sz_subdocuments_key_bytes_(*paths), arena_alignment_k);
        }

        std::size_t const walkers = paths ? count : 0;
        std::size_t const data_bytes = static_cast<std::size_t>(offsets[count]);
        std::size_t const words = divide_round_up<std::size_t>(count, 32);
        auto [wide_on_device, wide_on_device_status] = utilities::carve<std::uint64_t>(blocks_, executor_, count + 1);
        auto [bytes_on_device, bytes_on_device_status] = utilities::carve<char>(blocks_, executor_, data_bytes);
        auto [bits, bits_status] = utilities::carve<std::uint32_t>(blocks_, executor_, words + 1);
        auto [lists, lists_status] = utilities::carve<std::uint32_t>(blocks_, executor_, 3 * count);
        auto [chunk_list, chunk_list_status] = utilities::carve<matching_chunk_t>(blocks_, executor_, chunks.size());
        auto [stacks, stacks_status] = utilities::carve<std::uint64_t>(blocks_, executor_,
                                                                       walkers * matching_frames_k * frame_words);
        auto [keys, keys_status] = utilities::carve<unsigned char>(blocks_, executor_, walkers * key_bytes);
        auto [refused_on_host, refused_status] = arena_allocate<std::uint32_t>(work, 1);
        for (status_t const carved : {wide_on_device_status, bytes_on_device_status, bits_status, lists_status,
                                      chunk_list_status, stacks_status, keys_status, refused_status})
            if (carved.failed()) return carved;
        if (status_t const status = executor_.pin_bounce(); status.failed()) return status;
        std::span<data_type_ const> const tape {data, data_bytes};
        std::span<std::uint32_t const> const listed {shape.per_thread.data(), 3 * count};
        for (status_t const copied :
             {executor_.upload_async(reinterpret_cast<std::uintptr_t>(wide_on_device.data()), std::as_bytes(wide)),
              executor_.upload_async(reinterpret_cast<std::uintptr_t>(bytes_on_device.data()), std::as_bytes(tape)),
              executor_.upload_async(reinterpret_cast<std::uintptr_t>(lists.data()), std::as_bytes(listed)),
              executor_.upload_async(reinterpret_cast<std::uintptr_t>(chunk_list.data()), std::as_bytes(chunks)),
              executor_.fill_async(reinterpret_cast<std::uintptr_t>(bits.data()), byte_t {0},
                                   (words + 1) * sizeof(std::uint32_t))})
            if (copied.failed()) return copied;

        std::uint32_t* const refused = bits.data() + words;
        auto const page = [&](std::span<std::uint32_t> picked) noexcept {
            std::uint32_t const* const on_device = lists.data() + (picked.data() - shape.per_thread.data());
            return page_of(bytes_on_device.data(), wide_on_device.data(), {on_device, picked.size()}, bits.data());
        };
        unsigned const threads = utilities::matching_block_threads_k;
        auto const blocks_for = [](std::size_t lanes) noexcept {
            return static_cast<unsigned>(divide_round_up<std::size_t>(lanes, threads));
        };
        std::size_t const group_k = utilities::matching_group_lanes_k;
        matching_chunks_t const chunking {page(shape.per_chunk), chunk_list.data(),
                                          static_cast<std::uint32_t>(chunks.size()), chunk_bytes};
        std::uint32_t const* const primed = match_.images;
        status_t launched;
        if (substrings) {
            sz_substrings_engine_t const& engine = std::get<sz_substrings_engine_t>(match_.on_device);
            std::size_t const head = sz_substrings_staged_bytes_simt_(&engine, 0);
            std::uint32_t const staged = static_cast<std::uint32_t>(std::min<std::size_t>(
                engine.hot_count,
                (matching_shared_bytes_k - head) / (sz_substrings_staged_bytes_simt_(&engine, 1) - head)));
            unsigned const shared = static_cast<unsigned>(sz_substrings_staged_bytes_simt_(&engine, staged));
            if (!shape.per_thread.empty())
                launched = utilities::launch(executor_, kits.substrings.per_thread, blocks_for(shape.per_thread.size()),
                                             threads, shared, engine, page(shape.per_thread), staged);
            if (launched.succeeded() && !shape.per_group.empty())
                launched = utilities::launch(executor_, kits.substrings.per_group,
                                             blocks_for(shape.per_group.size() * group_k), threads, shared, engine,
                                             page(shape.per_group), staged);
            if (launched.succeeded() && !chunks.empty())
                launched = utilities::launch(executor_, kits.substrings.per_chunk, blocks_for(chunks.size()), threads,
                                             shared, engine, chunking, staged);
        }
        else if (regex) {
            sz_regex_engine_t const& engine = std::get<sz_regex_engine_t>(match_.on_device);
            std::size_t const head = sz_regex_staged_bytes_simt_(&engine, 0);
            std::uint32_t const cells = static_cast<std::uint32_t>(std::min<std::size_t>(
                engine.reverse_states_count * sz_regex_stride_(engine.classes_count, engine.lookarounds_count),
                (matching_shared_bytes_k - head) / (sz_regex_staged_bytes_simt_(&engine, 1) - head)));
            unsigned const shared = static_cast<unsigned>(sz_regex_staged_bytes_simt_(&engine, cells));
            if (!shape.per_thread.empty())
                launched = utilities::launch(executor_, kits.regex.per_thread, blocks_for(shape.per_thread.size()),
                                             threads, shared, engine, page(shape.per_thread), cells);
            if (launched.succeeded() && !shape.per_group.empty())
                launched = utilities::launch(executor_, kits.regex.per_group,
                                             blocks_for(shape.per_group.size() * group_k), threads, shared, engine,
                                             page(shape.per_group), primed, cells);
            if (launched.succeeded() && !chunks.empty())
                launched = utilities::launch(executor_, kits.regex.per_chunk, blocks_for(chunks.size()), threads,
                                             shared, engine, chunking, primed, cells, refused);
        }
        else {
            sz_subdocuments_engine_t const& engine = std::get<sz_subdocuments_engine_t>(match_.on_device);
            // The launches run in stream order, so each reuses the walkers' memory of
            // the one before
            matching_walkers_t const walkers_memory {
                stacks.data(), matching_frames_k * frame_words, frame_words, keys.data(), key_bytes, refused};
            if (markup && !shape.per_thread.empty())
                launched = utilities::launch(executor_, kits.subdocuments.per_thread,
                                             blocks_for(shape.per_thread.size()), threads, 0u, engine,
                                             page(shape.per_thread), walkers_memory);
            // JSON walks as a group however short the value, as its walks classify a window
            // across the lanes
            if (!markup && !shape.per_thread.empty())
                launched = utilities::launch(executor_, kits.subdocuments.per_group,
                                             blocks_for(shape.per_thread.size() * group_k), threads, 0u, engine,
                                             page(shape.per_thread), walkers_memory);
            if (launched.succeeded() && !shape.per_group.empty())
                launched = utilities::launch(executor_, kits.subdocuments.per_group,
                                             blocks_for(shape.per_group.size() * group_k), threads, 0u, engine,
                                             page(shape.per_group), walkers_memory);
            if (launched.succeeded() && !shape.per_chunk.empty())
                launched = utilities::launch(executor_, kits.subdocuments.per_chunk,
                                             blocks_for(shape.per_chunk.size() * group_k), threads, 0u, engine,
                                             page(shape.per_chunk), walkers_memory, chunk_bytes);
        }
        // Copies already queued read the bounce, which the next page rewinds, so a refusal still waits for them
        status_t downloaded;
        if (launched.succeeded()) {
            downloaded = executor_.download_async(std::as_writable_bytes(contained),
                                                  reinterpret_cast<std::uintptr_t>(bits.data()));
            if (downloaded.succeeded())
                downloaded = executor_.download_async(std::as_writable_bytes(refused_on_host),
                                                      reinterpret_cast<std::uintptr_t>(refused));
        }
        status_t const waited = executor_.wait();
        if (launched.failed()) return launched;
        if (downloaded.failed()) return downloaded;
        if (waited.failed()) return waited;
        // A document deeper than its walker's stack or malformed, or a regex chunk whose entry
        // stayed undecided, answers on the host
        if (refused_on_host[0]) return contains_on_host(data, offsets, count, contained);
        return {};
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
