/**
 *  @file src/vram/gathers.hpp
 *  @author Ash Vardanian
 *  @date November 5, 2026
 *  @brief Document gathers and field reads on a VRAM executor: the host reads the documents whole,
 *      and the device walks them and builds every output in the call's device arena.
 *
 *  The documents reach the device as one tape, where the subdocuments walks of the matching kernels
 *  store the node every path selects in every document, a match slot per path, and the gather
 *  kernels convert each column or print each read's values from those nodes. A text output is
 *  measured, its lengths scanned into offsets by the tape kernels, and written once its bytes are
 *  carved, behind offsets as narrow as the host's. One wait tells the host whether a lane met a
 *  cell only the host converts, and how many bytes every text output takes; a call a lane refused
 *  is left to the host verbs, as is one on a device without the library's kernels. The engine
 *  compiles into the executor's managed runs, as a filter's does, and every page of the call lives
 *  in its device arena and the executor's bounce.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`

#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`, `std::as_bytes`
#include <string_view> // `std::string_view`

#include "ustore/docs/json_paths.hpp"         // `compile_field_paths`, `no_path_k`
#include "ustore/docs/vram_gather.cuh"        // `gather_page_t`, `gather_scalars_t`, `gather_texts_t`
#include "ustore/utilities/ram_buffers.hpp"   // `arena_allocate`, `device_blocks_t`, `divide_round_up`
#include "ustore/utilities/tapes.hpp"         // `tape_source_t`, `tape_segment_t`, `tape_sources_per_segment_k`
#include "ustore/utilities/vram_device.hpp"   // `carve`, `launch`, `rewind`, `device_blocks_of`
#include "ustore/utilities/vram_matching.cuh" // `matching_walkers_t`, `matching_found_t`

#include "docs_layer.hpp"    // `field_of`, `reads_whole_documents`
#include "engine.hpp"        // `engine_of`, `home_executor`
#include "vram/matching.hpp" // `match_memory`, `rebase_on_device`, `shape_of`, `matching_frames_k`

namespace unum::ustore {

/** Whether a device verb served its call, or left it to the host as a lane met a host-only case. */
enum class device_answer_t : bool { served_k, left_to_host_k };

/** The documents of one call laid out on a device and walked, with each field's compiled path. */
struct walked_documents_t {
    gather_page_t page;
    std::span<std::size_t const> path_of_field;
};

/** The path @p field_index compiled into, as the gather kernels take it. */
inline std::uint32_t cell_path_of(walked_documents_t const& walked, std::size_t field_index) noexcept {
    std::size_t const path = walked.path_of_field[field_index];
    return path == no_path_k ? no_cell_path_k : static_cast<std::uint32_t>(path);
}

/** Blocks of @p threads threads covering @p lanes lanes. */
inline unsigned blocks_for(std::size_t lanes, unsigned threads) noexcept {
    return static_cast<unsigned>(divide_round_up<std::size_t>(lanes, threads));
}

/**
 *  @brief Reads the documents @p whole names into the bounce through the host verbs of @p engine,
 *      uploads them into @p blocks, the call's device arena, and walks them there, storing the node
 *      every path of @p fields selects.
 *
 *  A missing or empty document is walked by no lane, so its every cell is missing, and the walks of
 *  one length class share the walkers' memory in stream order. Nothing is waited for: the walks'
 *  refusal lands in the page.
 */
template <gpu_executor_concept executor_type_>
expected<walked_documents_t> walk_documents(engine_t const& engine, ustore_docs_read_t whole,
                                            std::span<std::string_view const> fields, view_t const& view,
                                            executor_type_& executor, device_blocks_t& blocks) noexcept {
    if (!engine.host.docs || !engine.host.docs->read)
        return status_t {status_code_t::unsupported_k, "The engine keeps no collections of that modality"};
    ustore_strings_view_t tape {};
    whole.values = &tape;
    whole.arena = executor.bounce();
    whole.outputs_type = ustore_docs_field_type_json_k;
    if (status_t const status = engine.host.docs->read(whole, strings_arg_t {}, view); status.failed()) return status;

    auto [compiled, compiled_status] = compile_field_paths(fields, view.scratch);
    if (compiled_status.failed()) return compiled_status;
    std::size_t const count = static_cast<std::size_t>(whole.count), paths_count = compiled.paths.size();
    walked_documents_t walked {{}, compiled.path_of_field};
    ustore_arena_t const work = executor.bounce();

    // Every document is a row whose cells stay missing until a walk fills them, a missing one empty
    bool const wide = tape.layout == ustore_strings_u64_tape_k;
    auto [offsets, offsets_status] = arena_allocate<std::uint64_t>(work, count + 1);
    if (offsets_status.failed()) return offsets_status;
    for (std::size_t index = 0; index <= count; ++index)
        offsets[index] = wide ? tape.data.u64_tape.offsets[index] : tape.data.u32_tape.offsets[index];
    ustore_byte_t const* const data = (wide ? tape.data.u64_tape.data : tape.data.u32_tape.data) + offsets[0];
    for (std::size_t index = count + 1; index-- != 0;) offsets[index] -= offsets[0];
    std::size_t const bytes_count = static_cast<std::size_t>(offsets[count]);
    std::span<ustore_byte_t const> const bytes {data, bytes_count};

    auto [offsets_on_device, offsets_on_device_status] = carve<std::uint64_t>(blocks, executor, count + 1);
    auto [bytes_on_device, bytes_on_device_status] = carve<char>(blocks, executor, bytes_count);
    auto [matches, matches_status] = carve<sz_match_t>(blocks, executor, count * paths_count);
    auto [found, found_status] = carve<std::uint32_t>(blocks, executor, count + 1);
    for (status_t const carved : {offsets_on_device_status, bytes_on_device_status, matches_status, found_status})
        if (carved.failed()) return carved;
    std::uint32_t* const refused = found.data() + count;
    if (status_t const pinned = executor.pin_bounce(); pinned.failed()) return pinned;
    for (status_t const copied :
         {executor.upload_async(reinterpret_cast<std::uintptr_t>(offsets_on_device.data()), std::as_bytes(offsets)),
          executor.upload_async(reinterpret_cast<std::uintptr_t>(bytes_on_device.data()), std::as_bytes(bytes)),
          executor.fill_async(reinterpret_cast<std::uintptr_t>(found.data()), byte_t {0},
                              (count + 1) * sizeof(std::uint32_t))})
        if (copied.failed()) return copied;
    walked.page = {bytes_on_device.data(),
                   offsets_on_device.data(),
                   matches.data(),
                   found.data(),
                   static_cast<std::uint32_t>(paths_count),
                   static_cast<std::uint32_t>(count),
                   refused};
    if (!paths_count || !bytes_count) return walked;

    sz_strings_view_t paths_view {};
    sz_strings_from_string_views(compiled.paths.data(), compiled.paths.size(), &paths_view);
    auto [engine_slot, engine_slot_status] = arena_allocate<sz_subdocuments_engine_t>(view.scratch, 1);
    if (engine_slot_status.failed()) return engine_slot_status;
    sz_allocator_t memory = match_memory(executor);
    sz_size_t malformed_index = 0;
    if (sz_status_t const status = sz_subdocuments_engine_init(
            engine_slot.data(), &paths_view, sz_subdocuments_json_k, sz_subdocuments_duplicate_names_first_k,
            &malformed_index, whole.database->ceiling.strings, &memory, nullptr);
        status != sz_success_k)
        return stringzilla_refusal(status, status_code_t::invalid_argument_k,
                                   "The fields compile into no path the engine takes");
    compiled_match<home_of_executors<executor_type_>> rebased;
    rebased.matcher = engine_slot.data();
    if (status_t const status = rebase_on_device(rebased, executor); status.failed()) return status;
    sz_subdocuments_engine_t const* const on_device = std::get_if<sz_subdocuments_engine_t>(&rebased.on_device);
    // Field paths carry no filters, so every engine they compile rebases
    if (!on_device) return status_t {status_code_t::internal_k, "The field paths compiled into filters"};

    // Empty documents leave the short list, so no lane walks them
    auto [shape, shape_status] = shape_of(offsets.data(), count, walk_split_t::by_length_k, work);
    if (shape_status.failed()) return shape_status;
    std::size_t kept = 0;
    for (std::uint32_t const value : shape.per_thread)
        if (offsets[value + 1] != offsets[value]) shape.per_thread[kept++] = value;
    shape.per_thread = shape.per_thread.first(kept);
    // The device tables are still being uploaded, so the host only ever reads its own
    std::uint64_t const frame_words = sz_subdocuments_frame_words_simt_(engine_slot.data());
    std::uint64_t const key_bytes = round_up_to_multiple<std::uint64_t>(sz_subdocuments_key_bytes_(engine_slot.data()),
                                                                        arena_alignment_k);
    std::size_t const walkers_count = fu::max_of(fu::max_of(shape.per_thread.size(), shape.per_group.size()),
                                                 shape.per_chunk.size());
    auto [lists, lists_status] = carve<std::uint32_t>(blocks, executor, 3 * count);
    auto [stacks, stacks_status] = carve<std::uint64_t>(blocks, executor,
                                                        walkers_count * matching_frames_k * frame_words);
    auto [keys, keys_status] = carve<unsigned char>(blocks, executor, walkers_count * key_bytes);
    for (status_t const carved : {lists_status, stacks_status, keys_status})
        if (carved.failed()) return carved;
    std::span<std::uint32_t const> const listed {shape.per_thread.data(), 3 * count};
    if (status_t const pinned = executor.pin_bounce(); pinned.failed()) return pinned;
    if (status_t const copied = executor.upload_async(reinterpret_cast<std::uintptr_t>(lists.data()),
                                                      std::as_bytes(listed));
        copied.failed())
        return copied;

    matching_walkers_t const walkers {
        stacks.data(), matching_frames_k * frame_words, frame_words, keys.data(), key_bytes, refused};
    matching_found_t const stored {matches.data(), found.data(), paths_count};
    auto const page = [&](std::span<std::uint32_t> picked) noexcept {
        std::uint32_t const* const on_device_list = lists.data() + (picked.data() - listed.data());
        return page_of(bytes_on_device.data(), offsets_on_device.data(), {on_device_list, picked.size()}, nullptr);
    };
    unsigned const threads = matching_block_threads_k;
    std::size_t const group_lanes = matching_group_lanes_k;
    status_t launched;
    // JSON walks as a group however short the value, as it classifies a window across the lanes
    for (std::span<std::uint32_t> const picked : {shape.per_thread, shape.per_group})
        if (launched.succeeded() && !picked.empty())
            launched = launch(executor, executor.kernels()->function("ustore_subdocuments_find_per_group"),
                              blocks_for(picked.size() * group_lanes, threads), threads, 0u, *on_device, page(picked),
                              walkers, stored);
    if (launched.succeeded() && !shape.per_chunk.empty())
        launched = launch(executor, executor.kernels()->function("ustore_subdocuments_find_per_chunk"),
                          blocks_for(shape.per_chunk.size() * group_lanes, threads), threads, 0u, *on_device,
                          page(shape.per_chunk), walkers, stored, matching_chunk_bytes_k);
    if (launched.failed()) return launched;
    return walked;
}

/** A text output of a device call while it is measured: its rows' lengths, the offsets they scan
 *  into, and the segments the scan cuts, which no gather reads. */
struct measured_texts_t {
    gather_texts_t texts;
    std::span<std::uint64_t> offsets;
    std::span<tape_segment_t> segments;
};

/** Measures the rows of @p texts on @p executor and scans their lengths into offsets carved off
 *  @p blocks, filling in the scratch the kernels need. */
template <gpu_executor_concept executor_type_>
expected<measured_texts_t> measure_texts(walked_documents_t const& walked, gather_texts_t texts,
                                         executor_type_& executor, device_blocks_t& blocks) noexcept {
    std::size_t const rows = walked.page.rows;
    auto [measured, measured_status] = carve<tape_source_t>(blocks, executor, rows);
    auto [offsets, offsets_status] = carve<std::uint64_t>(blocks, executor, rows + 1);
    auto [segments, segments_status] = carve<tape_segment_t>(
        blocks, executor, divide_round_up<std::size_t>(rows, tape_sources_per_segment_k));
    for (status_t const carved : {measured_status, offsets_status, segments_status})
        if (carved.failed()) return carved;
    texts.measured = measured.data();
    texts.offsets = offsets.data();
    status_t launched = launch(
        executor, executor.kernels()->function("ustore_docs_measure_texts"),
        blocks_for(divide_round_up<std::size_t>(rows, gather_lane_rows_k), gather_block_threads_k),
        gather_block_threads_k, 0u, walked.page, texts);
    if (launched.succeeded())
        launched = launch(executor, executor.kernels()->function("ustore_tape_offsets_u64"), 1u, tape_block_threads_k,
                          0u, static_cast<tape_source_t const*>(measured.data()), std::uint64_t {rows},
                          tape_sources_per_segment_k, offsets.data(), segments.data());
    if (launched.failed()) return launched;
    return measured_texts_t {texts, offsets, segments};
}

/** Carves the @p total bytes of the measured @p texts off @p blocks and writes their rows there,
 *  behind offsets as narrow as their bytes allow, as the host exports a tape: 32-bit ones scanned
 *  again from the same lengths. */
template <gpu_executor_concept executor_type_>
expected<ustore_strings_view_t> write_texts(walked_documents_t const& walked, measured_texts_t measured,
                                            std::uint64_t total, executor_type_& executor,
                                            device_blocks_t& blocks) noexcept {
    auto [bytes, bytes_status] = carve<char>(blocks, executor, static_cast<std::size_t>(total));
    if (bytes_status.failed()) return bytes_status;
    measured.texts.bytes = bytes.data();
    status_t launched;
    if (total)
        launched = launch(executor, executor.kernels()->function("ustore_docs_write_texts"),
                          blocks_for(walked.page.rows, gather_block_threads_k), gather_block_threads_k, 0u, walked.page,
                          measured.texts);
    if (launched.failed()) return launched;
    if (total > (std::numeric_limits<std::uint32_t>::max)()) return u64_tape(bytes.data(), measured.offsets.data());
    std::size_t const rows = walked.page.rows;
    auto [narrow, narrow_status] = carve<std::uint32_t>(blocks, executor, rows + 1);
    if (narrow_status.failed()) return narrow_status;
    launched = launch(executor, executor.kernels()->function("ustore_tape_offsets_u32"), 1u, tape_block_threads_k, 0u,
                      static_cast<tape_source_t const*>(measured.texts.measured), std::uint64_t {rows},
                      tape_sources_per_segment_k, narrow.data(), measured.segments.data());
    if (launched.failed()) return launched;
    return u32_tape(bytes.data(), narrow.data());
}

/** Downloads the refusal of @p walked and the last offset of every one of @p measured into
 *  the bounce and waits, answering whether the call is left to the host, with the totals
 *  in @p totals. */
template <gpu_executor_concept executor_type_>
expected<device_answer_t> settle_measures(walked_documents_t const& walked, std::span<measured_texts_t const> measured,
                                          std::span<std::uint64_t> totals, executor_type_& executor) noexcept {
    auto [refused, refused_status] = arena_allocate<std::uint32_t>(executor.bounce(), 1);
    if (refused_status.failed()) return refused_status;
    status_t downloaded = executor.pin_bounce();
    if (downloaded.succeeded())
        downloaded = executor.download_async(std::as_writable_bytes(refused),
                                             reinterpret_cast<std::uintptr_t>(walked.page.refused));
    for (std::size_t index = 0; downloaded.succeeded() && index != measured.size(); ++index)
        downloaded = executor.download_async(
            std::as_writable_bytes(totals.subspan(index, 1)),
            reinterpret_cast<std::uintptr_t>(measured[index].offsets.data() + walked.page.rows));
    // Copies already queued read the bounce, so a failure still waits for them
    status_t const waited = executor.wait();
    if (downloaded.failed()) return downloaded;
    if (waited.failed()) return waited;
    return refused[0] ? device_answer_t::left_to_host_k : device_answer_t::served_k;
}

/** The whole documents @p arguments address, as the host read their walk starts from takes them. */
template <typename arguments_type_>
ustore_docs_read_t whole_documents_of(arguments_type_ const& arguments) noexcept {
    ustore_docs_read_t whole {};
    whole.database = arguments.database;
    whole.view = arguments.view;
    whole.count = arguments.count;
    whole.collections = arguments.collections;
    whole.keys = arguments.keys;
    return whole;
}

/** Whether every column type of @p arguments is one the gather kernels convert into: a scalar, a
 *  UUID, a string or binary text. */
inline bool converts_on_device(ustore_docs_gather_t const& arguments) noexcept {
    for (std::size_t field = 0; field != arguments.fields_count; ++field) {
        ustore_docs_field_type_t const type = arguments.outputs_types[field];
        if (!field_size_bytes(type) && type != ustore_docs_field_type_string_k &&
            type != ustore_docs_field_type_binary_k)
            return false;
    }
    return true;
}

/**
 *  @brief Serves @c ustore_docs_gather on @p executor's device, every column built in the call's
 *      device arena, as the host gather builds it.
 *
 *  Every column's three bitmaps share one carve, each starting aligned, its scalars another,
 *  and a text column's offsets and bytes two more, behind 64-bit offsets; the views naming them
 *  are uploaded last.
 */
template <home_t home_>
expected<device_answer_t> gather_documents_on_device(ustore_docs_gather_t const& arguments, strings_arg_t const& fields,
                                                     view_t const& view, home_executor<home_>& executor) noexcept {
    if (!arguments.count || !arguments.fields_count) return device_answer_t::served_k;
    if (!arguments.columns) return status_t {status_code_t::invalid_argument_k, "Need an output for the columns"};
    if (!fields || !arguments.outputs_types)
        return status_t {status_code_t::invalid_argument_k, "Need the fields to gather and the types of their columns"};
    if (!executor.kernels() || !converts_on_device(arguments) || arguments.count > no_cell_path_k)
        return device_answer_t::left_to_host_k;
    device_blocks_t* const blocks = device_blocks_of(arguments.arena, executor);
    if (!blocks) return status_t {status_code_t::device_k, "The arena lives where another executor computes"};

    std::size_t const fields_count = arguments.fields_count, rows = arguments.count;
    auto [names, names_status] = arena_allocate<std::string_view>(view.scratch, fields_count);
    if (names_status.failed()) return names_status;
    for (std::size_t field = 0; field != fields_count; ++field) names[field] = string_of(fields[field]);
    auto [walked, walked_status] = walk_documents(engine_of(arguments.database), whole_documents_of(arguments), names,
                                                  view, executor, *blocks);
    if (walked_status.failed()) return walked_status;

    auto [views, views_status] = arena_allocate<ustore_docs_column_view_t>(executor.bounce(), fields_count);
    if (views_status.failed()) return views_status;
    auto [measured, measured_status] = arena_allocate<measured_texts_t>(view.scratch, fields_count);
    if (measured_status.failed()) return measured_status;
    auto [texts_of_fields, texts_status] = arena_allocate<std::size_t>(view.scratch, fields_count);
    if (texts_status.failed()) return texts_status;
    std::size_t texts_count = 0;
    std::size_t const bitmap_stride = round_up_to_multiple(divide_round_up(rows, bits_in_byte_k), arena_alignment_k);
    unsigned const scalar_blocks = blocks_for(divide_round_up<std::size_t>(rows, gather_lane_rows_k),
                                              gather_block_threads_k);
    for (std::size_t field = 0; field != fields_count; ++field) {
        ustore_docs_field_type_t const type = arguments.outputs_types[field];
        auto [bitmaps, bitmaps_status] = carve<ustore_octet_t>(*blocks, executor, 3 * bitmap_stride);
        if (bitmaps_status.failed()) return bitmaps_status;
        if (status_t const filled = executor.fill_async(reinterpret_cast<std::uintptr_t>(bitmaps.data()), byte_t {0},
                                                        bitmaps.size());
            filled.failed())
            return filled;
        views[field] = {.validities = bitmaps.data(),
                        .conversions = bitmaps.data() + bitmap_stride,
                        .collisions = bitmaps.data() + 2 * bitmap_stride,
                        .scalars = nullptr,
                        .strings = {}};
        std::uint32_t const path = cell_path_of(walked, field);
        if (std::size_t const width = field_size_bytes(type); width) {
            auto [scalars, scalars_status] = carve<ustore_byte_t>(*blocks, executor, rows * width);
            if (scalars_status.failed()) return scalars_status;
            views[field].scalars = scalars.data();
            gather_scalars_t const column {bitmaps.data(), scalars.data(), bitmap_stride, type, path};
            if (status_t const launched = launch(executor, executor.kernels()->function("ustore_docs_gather_scalars"),
                                                 scalar_blocks, gather_block_threads_k, 0u, walked.page, column);
                launched.failed())
                return launched;
            continue;
        }
        gather_texts_t texts;
        texts.text = type == ustore_docs_field_type_string_k ? cell_text_t::column_text_k
                                                             : cell_text_t::column_binary_k;
        texts.path = path;
        texts.bitmaps = bitmaps.data();
        texts.bitmap_stride = bitmap_stride;
        auto [texts_measured, measure_status] = measure_texts(walked, texts, executor, *blocks);
        if (measure_status.failed()) return measure_status;
        measured[texts_count] = texts_measured;
        texts_of_fields[texts_count++] = field;
    }

    auto [totals, totals_status] = arena_allocate<std::uint64_t>(executor.bounce(), texts_count);
    if (totals_status.failed()) return totals_status;
    auto [answer, answer_status] = settle_measures(walked, measured.first(texts_count), totals, executor);
    if (answer_status.failed()) return answer_status;
    if (answer == device_answer_t::left_to_host_k) {
        rewind(*blocks, executor);
        return answer;
    }
    for (std::size_t text = 0; text != texts_count; ++text) {
        auto [tape, tape_status] = write_texts(walked, measured[text], totals[text], executor, *blocks);
        if (tape_status.failed()) return tape_status;
        views[texts_of_fields[text]].strings = tape;
    }
    auto [views_on_device, views_on_device_status] = carve<ustore_docs_column_view_t>(*blocks, executor, fields_count);
    if (views_on_device_status.failed()) return views_on_device_status;
    status_t const uploaded = executor.upload_async(reinterpret_cast<std::uintptr_t>(views_on_device.data()),
                                                    std::as_bytes(views));
    // The upload reads the bounce, which the next call rewinds, so a failure still waits for it
    status_t const waited = executor.wait();
    if (uploaded.failed()) return uploaded;
    if (waited.failed()) return waited;
    *arguments.columns = views_on_device.data();
    return device_answer_t::served_k;
}

/**
 *  @brief Serves @c ustore_docs_read of fields on @p executor's device, every output in the call's
 *      device arena, as the host read exports them, its values behind 64-bit offsets.
 *
 *  A read of whole JSON documents only is left to the host verbs, which read them as stored.
 */
template <home_t home_>
expected<device_answer_t> read_documents_on_device(ustore_docs_read_t const& arguments, strings_arg_t const& fields,
                                                   view_t const& view, home_executor<home_>& executor) noexcept {
    if (!arguments.count) return device_answer_t::served_k;
    if (reads_whole_documents(arguments, fields) || !executor.kernels() || arguments.count > no_cell_path_k)
        return device_answer_t::left_to_host_k;
    if (arguments.outputs_type != ustore_docs_field_type_json_k &&
        arguments.outputs_type != ustore_docs_field_type_string_k)
        return status_t {status_code_t::unsupported_k, "Documents read only as JSON or strings"};
    device_blocks_t* const blocks = device_blocks_of(arguments.arena, executor);
    if (!blocks) return status_t {status_code_t::device_k, "The arena lives where another executor computes"};

    std::size_t const rows = arguments.count;
    auto [names, names_status] = arena_allocate<std::string_view>(view.scratch, rows);
    if (names_status.failed()) return names_status;
    for (std::size_t task = 0; task != rows; ++task) names[task] = field_of(fields, task);
    auto [walked, walked_status] = walk_documents(engine_of(arguments.database), whole_documents_of(arguments), names,
                                                  view, executor, *blocks);
    if (walked_status.failed()) return walked_status;

    // Every task reads its own path, which the kernels look up per row
    auto [paths, paths_status] = arena_allocate<std::uint32_t>(executor.bounce(), rows);
    if (paths_status.failed()) return paths_status;
    for (std::size_t task = 0; task != rows; ++task) paths[task] = cell_path_of(walked, task);
    auto [paths_on_device, paths_on_device_status] = carve<std::uint32_t>(*blocks, executor, rows);
    auto [presences, presences_status] = carve<ustore_octet_t>(
        *blocks, executor, arguments.presences ? divide_round_up(rows, bits_in_byte_k) : 0);
    auto [lengths, lengths_status] = carve<ustore_length_t>(*blocks, executor, arguments.lengths ? rows : 0);
    for (status_t const carved : {paths_on_device_status, presences_status, lengths_status})
        if (carved.failed()) return carved;
    if (status_t const pinned = executor.pin_bounce(); pinned.failed()) return pinned;
    if (status_t const copied = executor.upload_async(reinterpret_cast<std::uintptr_t>(paths_on_device.data()),
                                                      std::as_bytes(paths));
        copied.failed())
        return copied;
    gather_texts_t texts;
    texts.text = arguments.outputs_type == ustore_docs_field_type_json_k ? cell_text_t::field_json_k
                                                                         : cell_text_t::field_text_k;
    texts.paths = paths_on_device.data();
    texts.presences = presences.data();
    texts.lengths = lengths.data();
    auto [measured, measure_status] = measure_texts(walked, texts, executor, *blocks);
    if (measure_status.failed()) return measure_status;

    auto [totals, totals_status] = arena_allocate<std::uint64_t>(executor.bounce(), 1);
    if (totals_status.failed()) return totals_status;
    auto [answer, answer_status] = settle_measures(walked, {&measured, 1}, totals, executor);
    if (answer_status.failed()) return answer_status;
    if (answer == device_answer_t::left_to_host_k) {
        rewind(*blocks, executor);
        return answer;
    }
    if (arguments.values) {
        auto [tape, tape_status] = write_texts(walked, measured, totals[0], executor, *blocks);
        if (tape_status.failed()) return tape_status;
        if (status_t const waited = executor.wait(); waited.failed()) return waited;
        *arguments.values = tape;
    }
    if (arguments.presences) *arguments.presences = presences.data();
    if (arguments.lengths) *arguments.lengths = lengths.data();
    return device_answer_t::served_k;
}

} // namespace unum::ustore
