/**
 *  @file src/vram/vram_kernels_amd.hip
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The kernels of AMD devices, built device-only into the one bundle every device loads at
 *      its first executor's spawn, from the templates the NVIDIA unit builds, as none of them
 *      depends on the lane width.
 */
#include <hip/hip_runtime.h> // `threadIdx`

#include <stringzilla/rocm.cuh> // `sz_lanes_any_rocm_`, `sz_shuffle_up_rocm_` & kin

#include "ustore/blobs/vram_nvidia_compaction.cuh"     // `merge_rank`, `merge_pack`, `merge_place`
#include "ustore/docs/vram_gather.cuh"                 // `gather_scalars`, `measure_texts`, `write_texts`
#include "ustore/graph/vram_nvidia_adjacency_fold.cuh" // `fold_chains`, `fold_write`
#include "ustore/utilities/tapes.cuh"                  // `tape_offsets_block`, `tape_gather_block`
#include "ustore/utilities/vram_matching.cuh"          // `substrings_per_thread`, `matching_page_t` & kin

using namespace unum::ustore::utilities;

/** Scans the lengths of @p count sources into 32-bit @p offsets and cuts the segments,
 *  on one block. */
extern "C" __global__ void __launch_bounds__(tape_block_threads_k)
    ustore_tape_offsets_u32(tape_source_t const* sources, std::uint64_t count, std::uint32_t sources_per_segment,
                            std::uint32_t* offsets, tape_segment_t* segments) {
    tape_offsets_block<std::uint32_t, tape_block_threads_k>(sources, count, sources_per_segment, offsets, segments);
}

/** Scans the lengths of @p count sources into 64-bit @p offsets and cuts the segments,
 *  on one block. */
extern "C" __global__ void __launch_bounds__(tape_block_threads_k)
    ustore_tape_offsets_u64(tape_source_t const* sources, std::uint64_t count, std::uint32_t sources_per_segment,
                            std::uint64_t* offsets, tape_segment_t* segments) {
    tape_offsets_block<std::uint64_t, tape_block_threads_k>(sources, count, sources_per_segment, offsets, segments);
}

/** Copies the strings of every segment into @p bytes, one block per segment. */
extern "C" __global__ void __launch_bounds__(tape_block_threads_k)
    ustore_tape_gather(tape_source_t const* sources, tape_segment_t const* segments, std::byte* bytes) {
    tape_gather_block(sources, segments, bytes);
}

/** Copies fixed-width rows a stride apart into one dense run, one row per thread. */
extern "C" __global__ void __launch_bounds__(tape_block_threads_k) ustore_tape_densify(strided_rows_t rows) {
    densify_rows(rows);
}

#pragma region Merge Kernels

using namespace unum::ustore::blobs;

/** Ranks every record of the merge of @p runs of inline 12 key slots into @p merged with
 *  its survival flag. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k) ustore_merge_rank_inline_12(
    merge_run<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_12_bytes_k>> const* runs,
    std::uint32_t runs_count, std::uint32_t upper_runs, std::uint64_t value_stride, std::uint64_t records_count,
    compaction_kind_t kind, compaction_stamps_t stamps,
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_12_bytes_k>>* merged,
    std::uint8_t* flags) {
    merge_rank(runs, runs_count, upper_runs, value_stride, records_count, kind, stamps, merged, flags);
}

/** Packs the flagged records of a merge of inline 12 key slots, one tile
 *  of @p tickets per block. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k) ustore_merge_pack_inline_12(
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_12_bytes_k>> const* merged,
    std::uint8_t const* flags, std::uint64_t records_count, std::uint64_t const* replaced,
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_12_bytes_k>> const*
        replacements,
    std::uint64_t replaced_count, std::uint64_t tickets,
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_12_bytes_k>>* survivors,
    std::uint64_t* prefix, std::uint8_t* shares, std::uint64_t* chains) {
    merge_pack<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_12_bytes_k>, merge_block_threads_k>(
        merged, flags, records_count, replaced, replacements, replaced_count, tickets, survivors, prefix, shares,
        chains);
}

/** Writes every cell of the new runs of a merge of inline 12 key slots. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k) ustore_merge_place_inline_12(
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_12_bytes_k>> const* survivors,
    std::uint64_t survivors_count, std::uint64_t floor,
    key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_12_bytes_k>* cells, std::uint64_t* stamps,
    std::uint8_t* kinds) {
    merge_place(survivors, survivors_count, floor, cells, stamps, kinds);
}

/** Ranks every record of the merge of @p runs of inline 44 key slots into @p merged with
 *  its survival flag. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k) ustore_merge_rank_inline_44(
    merge_run<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_44_bytes_k>> const* runs,
    std::uint32_t runs_count, std::uint32_t upper_runs, std::uint64_t value_stride, std::uint64_t records_count,
    compaction_kind_t kind, compaction_stamps_t stamps,
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_44_bytes_k>>* merged,
    std::uint8_t* flags) {
    merge_rank(runs, runs_count, upper_runs, value_stride, records_count, kind, stamps, merged, flags);
}

/** Packs the flagged records of a merge of inline 44 key slots, one tile
 *  of @p tickets per block. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k) ustore_merge_pack_inline_44(
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_44_bytes_k>> const* merged,
    std::uint8_t const* flags, std::uint64_t records_count, std::uint64_t const* replaced,
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_44_bytes_k>> const*
        replacements,
    std::uint64_t replaced_count, std::uint64_t tickets,
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_44_bytes_k>>* survivors,
    std::uint64_t* prefix, std::uint8_t* shares, std::uint64_t* chains) {
    merge_pack<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_44_bytes_k>, merge_block_threads_k>(
        merged, flags, records_count, replaced, replacements, replaced_count, tickets, survivors, prefix, shares,
        chains);
}

/** Writes every cell of the new runs of a merge of inline 44 key slots. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k) ustore_merge_place_inline_44(
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_44_bytes_k>> const* survivors,
    std::uint64_t survivors_count, std::uint64_t floor,
    key_slot<ashvardanian::smashtable::key128_t, value_layout_t::inline_44_bytes_k>* cells, std::uint64_t* stamps,
    std::uint8_t* kinds) {
    merge_place(survivors, survivors_count, floor, cells, stamps, kinds);
}

/** Ranks every record of the merge of @p runs of fixed width key slots into @p merged with
 *  its survival flag. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k) ustore_merge_rank_fixed_width(
    merge_run<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::fixed_width_k>> const* runs,
    std::uint32_t runs_count, std::uint32_t upper_runs, std::uint64_t value_stride, std::uint64_t records_count,
    compaction_kind_t kind, compaction_stamps_t stamps,
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::fixed_width_k>>* merged,
    std::uint8_t* flags) {
    merge_rank(runs, runs_count, upper_runs, value_stride, records_count, kind, stamps, merged, flags);
}

/** Packs the flagged records of a merge of fixed width key slots, one tile
 *  of @p tickets per block. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k) ustore_merge_pack_fixed_width(
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::fixed_width_k>> const* merged,
    std::uint8_t const* flags, std::uint64_t records_count, std::uint64_t const* replaced,
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::fixed_width_k>> const* replacements,
    std::uint64_t replaced_count, std::uint64_t tickets,
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::fixed_width_k>>* survivors,
    std::uint64_t* prefix, std::uint8_t* shares, std::uint64_t* chains) {
    merge_pack<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::fixed_width_k>, merge_block_threads_k>(
        merged, flags, records_count, replaced, replacements, replaced_count, tickets, survivors, prefix, shares,
        chains);
}

/** Writes every cell of the new runs of a merge of fixed width key slots. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k) ustore_merge_place_fixed_width(
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::fixed_width_k>> const* survivors,
    std::uint64_t survivors_count, std::uint64_t floor,
    key_slot<ashvardanian::smashtable::key128_t, value_layout_t::fixed_width_k>* cells, std::uint64_t* stamps,
    std::uint8_t* kinds) {
    merge_place(survivors, survivors_count, floor, cells, stamps, kinds);
}

/** Ranks every record of the merge of @p runs of variable width key slots into @p merged with
 *  its survival flag. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k) ustore_merge_rank_variable_width(
    merge_run<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::variable_width_k>> const* runs,
    std::uint32_t runs_count, std::uint32_t upper_runs, std::uint64_t value_stride, std::uint64_t records_count,
    compaction_kind_t kind, compaction_stamps_t stamps,
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::variable_width_k>>* merged,
    std::uint8_t* flags) {
    merge_rank(runs, runs_count, upper_runs, value_stride, records_count, kind, stamps, merged, flags);
}

/** Packs the flagged records of a merge of variable width key slots, one tile
 *  of @p tickets per block. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k) ustore_merge_pack_variable_width(
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::variable_width_k>> const* merged,
    std::uint8_t const* flags, std::uint64_t records_count, std::uint64_t const* replaced,
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::variable_width_k>> const*
        replacements,
    std::uint64_t replaced_count, std::uint64_t tickets,
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::variable_width_k>>* survivors,
    std::uint64_t* prefix, std::uint8_t* shares, std::uint64_t* chains) {
    merge_pack<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::variable_width_k>, merge_block_threads_k>(
        merged, flags, records_count, replaced, replacements, replaced_count, tickets, survivors, prefix, shares,
        chains);
}

/** Writes every cell of the new runs of a merge of variable width key slots. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k) ustore_merge_place_variable_width(
    located_key_slot<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::variable_width_k>> const* survivors,
    std::uint64_t survivors_count, std::uint64_t floor,
    key_slot<ashvardanian::smashtable::key128_t, value_layout_t::variable_width_k>* cells, std::uint64_t* stamps,
    std::uint8_t* kinds) {
    merge_place(survivors, survivors_count, floor, cells, stamps, kinds);
}

/** Gathers the cells of runs of fixed width key slots from the leaf rows of their key trees. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k) ustore_merge_gather_fixed_width(
    key_rows_gather<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::fixed_width_k>> const* gathers,
    std::uint32_t gathers_count, std::uint64_t cells_count) {
    merge_gather(gathers, gathers_count, cells_count);
}

/** Gathers the cells of runs of variable width key slots from the leaf rows of their key trees. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k) ustore_merge_gather_variable_width(
    key_rows_gather<key_slot<ashvardanian::smashtable::key128_t, value_layout_t::variable_width_k>> const* gathers,
    std::uint32_t gathers_count, std::uint64_t cells_count) {
    merge_gather(gathers, gathers_count, cells_count);
}

/** Ranks every version of a merge of runs of strings into @p merged with its survival flag. */
extern "C" __global__ void __launch_bounds__(merge_block_threads_k)
    ustore_merge_strings_rank(strings_merge_run const* runs, std::uint32_t runs_count, std::uint32_t upper_runs,
                              std::uint64_t records_count, compaction_kind_t kind, compaction_stamps_t stamps,
                              strings_version_t* merged, std::uint8_t* flags) {
    merge_strings_rank(runs, runs_count, upper_runs, records_count, kind, stamps, merged, flags);
}

#pragma endregion Merge Kernels

#pragma region Fold Kernels

using namespace unum::ustore::graph;

/** Sorts, keeps and counts the records of every chain of a graph merge, one block per chain. */
extern "C" __global__ void __launch_bounds__(fold_block_threads_k)
    ustore_fold_chains(half_edge_record_t const* records, std::uint32_t const* starts, std::uint32_t chains_count,
                       std::uint32_t* kept, std::uint32_t* kept_counts) {
    extern __shared__ std::uint32_t sorted[];
    fold_chains<fold_block_threads_k>(records, starts, chains_count, sorted, kept, kept_counts);
}

/** Writes the kept records of every chain of a graph merge back to back, one tile
 *  of @p tickets per block. */
extern "C" __global__ void __launch_bounds__(fold_block_threads_k)
    ustore_fold_write(half_edge_record_t const* records, std::uint32_t const* starts, std::uint32_t const* kept,
                      std::uint32_t const* kept_counts, std::uint32_t chains_count, std::uint64_t tickets,
                      half_edge_record_t removal, half_edge_record_t* composed, std::uint64_t* offsets,
                      std::uint64_t* chain) {
    fold_write<fold_block_threads_k>(records, starts, kept, kept_counts, chains_count, tickets, removal, composed,
                                     offsets, chain);
}

#pragma endregion Fold Kernels

#pragma region Matching Kernels

/** This vendor's 32 lanes as one group of the walks. */
__device__ inline sz_simt_group_t matching_group() noexcept {
    return {matching_group_lanes_k, threadIdx.x % matching_group_lanes_k,
            sz_lanes_any_rocm_,     sz_lanes_ballot_rocm_,
            sz_lanes_first_rocm_,   sz_lanes_below_rocm_,
            sz_shuffle_index_rocm_, sz_shuffle_up_rocm_,
            sz_group_sync_rocm_};
}

/** Marks the short values of @p page holding a needle of @p engine, a lane each. */
extern "C" __global__ void __launch_bounds__(matching_block_threads_k)
    ustore_substrings_contains_per_thread(sz_substrings_engine_t engine, matching_page_t page,
                                          std::uint32_t staged_count) {
    substrings_per_thread(engine, page, staged_count);
}

/** Marks the medium values of @p page holding a needle of @p engine, a group each. */
extern "C" __global__ void __launch_bounds__(matching_block_threads_k)
    ustore_substrings_contains_per_group(sz_substrings_engine_t engine, matching_page_t page,
                                         std::uint32_t staged_count) {
    substrings_per_group(engine, page, staged_count, matching_group());
}

/** Marks the long values of @p chunks holding a needle of @p engine, a lane per chunk. */
extern "C" __global__ void __launch_bounds__(matching_block_threads_k)
    ustore_substrings_contains_per_chunk(sz_substrings_engine_t engine, matching_chunks_t chunks,
                                         std::uint32_t staged_count) {
    substrings_per_chunk(engine, chunks, staged_count);
}

/** Marks the values of @p page a pattern of @p engine matches in, a lane each. */
extern "C" __global__ void __launch_bounds__(matching_block_threads_k)
    ustore_regex_contains_per_thread(sz_regex_engine_t engine, matching_page_t page, std::uint32_t cells) {
    regex_per_thread(engine, page, cells);
}

/** Marks the values of @p page a pattern of @p engine matches in, a group each primed from its
 *  column @p images. */
extern "C" __global__ void __launch_bounds__(matching_block_threads_k)
    ustore_regex_contains_per_group(sz_regex_engine_t engine, matching_page_t page, std::uint32_t const* images,
                                    std::uint32_t cells) {
    regex_per_group(engine, page, images, cells, matching_group());
}

/** Marks the values of @p chunks a pattern of @p engine matches in, a lane per chunk primed from
 *  its column @p images, raising @p refused where a chunk cannot answer alone. */
extern "C" __global__ void __launch_bounds__(matching_block_threads_k)
    ustore_regex_contains_per_chunk(sz_regex_engine_t engine, matching_chunks_t chunks, std::uint32_t const* images,
                                    std::uint32_t cells, std::uint32_t* refused) {
    regex_per_chunk(engine, chunks, images, cells, refused);
}

/** Marks the XML values of @p page a path of @p engine selects in, a lane each. */
extern "C" __global__ void __launch_bounds__(matching_block_threads_k)
    ustore_subdocuments_contains_per_thread(sz_subdocuments_engine_t engine, matching_page_t page,
                                            matching_walkers_t walkers) {
    subdocuments_per_thread(engine, page, walkers);
}

/** Marks the JSON values of @p page a path of @p engine selects in, a group each. */
extern "C" __global__ void __launch_bounds__(matching_block_threads_k)
    ustore_subdocuments_contains_per_group(sz_subdocuments_engine_t engine, matching_page_t page,
                                           matching_walkers_t walkers) {
    subdocuments_per_group(engine, page, walkers, 0, matching_group());
}

/** Marks the long JSON values of @p page a path of @p engine selects in, a group each walking them
 *  in chunks of @p chunk_bytes. */
extern "C" __global__ void __launch_bounds__(matching_block_threads_k)
    ustore_subdocuments_contains_per_chunk(sz_subdocuments_engine_t engine, matching_page_t page,
                                           matching_walkers_t walkers, std::uint64_t chunk_bytes) {
    subdocuments_per_group(engine, page, walkers, chunk_bytes, matching_group());
}

/** Stores the nodes @p engine's paths select in @p page's JSON values into @p found, per group. */
extern "C" __global__ void __launch_bounds__(matching_block_threads_k)
    ustore_subdocuments_find_per_group(sz_subdocuments_engine_t engine, matching_page_t page,
                                       matching_walkers_t walkers, matching_found_t found) {
    subdocuments_find_per_group(engine, page, walkers, found, 0, matching_group());
}

/** Stores the nodes the paths of @p engine select in the long JSON values of @p page into @p found,
 *  a group each walking them in chunks of @p chunk_bytes. */
extern "C" __global__ void __launch_bounds__(matching_block_threads_k)
    ustore_subdocuments_find_per_chunk(sz_subdocuments_engine_t engine, matching_page_t page,
                                       matching_walkers_t walkers, matching_found_t found, std::uint64_t chunk_bytes) {
    subdocuments_find_per_group(engine, page, walkers, found, chunk_bytes, matching_group());
}

#pragma endregion Matching Kernels

#pragma region Gather Kernels

using namespace unum::ustore::docs;

/** Converts the rows of one scalar column of a gather, eight per lane. */
extern "C" __global__ void __launch_bounds__(gather_block_threads_k)
    ustore_docs_gather_scalars(gather_page_t page, gather_scalars_t column) {
    gather_scalars(page, column);
}

/** Measures the rows of one text output of a gather or a read, eight per lane. */
extern "C" __global__ void __launch_bounds__(gather_block_threads_k)
    ustore_docs_measure_texts(gather_page_t page, gather_texts_t texts) {
    measure_texts(page, texts);
}

/** Writes the rows of one measured and scanned text output, one per lane. */
extern "C" __global__ void __launch_bounds__(gather_block_threads_k)
    ustore_docs_write_texts(gather_page_t page, gather_texts_t texts) {
    write_texts(page, texts);
}

/** Packs a boolean column into Arrow's bits, eight rows per lane. */
extern "C" __global__ void __launch_bounds__(gather_block_threads_k)
    ustore_docs_pack_bools(ustore_byte_t const* bools, std::uint64_t rows, ustore_octet_t* packed) {
    pack_bool_bytes(bools, rows, packed);
}

/** Copies a text column of 32-bit offsets without the null byte ending each row, a row per lane. */
extern "C" __global__ void __launch_bounds__(gather_block_threads_k)
    ustore_docs_drop_terminators_u32(terminated_texts<std::uint32_t> texts) {
    drop_terminators(texts);
}

/** Copies a text column of 64-bit offsets without the null byte ending each row, a row per lane. */
extern "C" __global__ void __launch_bounds__(gather_block_threads_k)
    ustore_docs_drop_terminators_u64(terminated_texts<std::uint64_t> texts) {
    drop_terminators(texts);
}

#pragma endregion Gather Kernels
