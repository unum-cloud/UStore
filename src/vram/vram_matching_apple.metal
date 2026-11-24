/**
 *  @file src/vram/vram_matching_apple.metal
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The matching kernels of Apple devices in MSL: substrings, regex and subdocuments walks
 *      marking the values of a page that hold a match, and the subdocuments walks storing the nodes
 *      they find, which @c vram_entries_apple.cpp compiles on each device at its first launch.
 *
 *  The Metal twin of the matching kernels of @c vram_kernels_nvidia.cu , kernel for kernel and
 *  under the same names, each running the body of the @c vram_matching.cuh step of that name over
 *  the MSL walks of the engines. Every kernel reads its CUDA parameters packed in order as one
 *  struct, the engines by value laid out as their C structs with every pointer a host address,
 *  reached through the block table as @c vram_kernels_apple.metal reaches its own.
 *
 *  The host splices, ahead of this source: @c vram_apple_shared.hpp , then StringZilla's
 *  @c types.metal , @c utf8_uncased_fold/tables.h , @c utf8_uncased_fold/metal.metal ,
 *  @c substrings/metal.metal , @c regex/metal.metal and @c subdocuments/metal.metal , then
 *  @c vram_kernels_apple.metal . The regex walks read their start state, acceptance bound and row
 *  stride from a launch record in constant memory, so each regex kernel reads that record after
 *  its CUDA parameters. Only 32-bit atomic ORs mark a value's bit or raise a refusal, as on CUDA.
 */

#pragma region Mirrors

namespace unum {
namespace ustore {

/** A substrings engine passed by value, laid out as @c sz_substrings_engine_t . */
struct substrings_engine_t {
    ulong hot_rows;
    ulong byte_to_class;
    ulong base;
    ulong check;
    ulong fail;
    ulong accepts_words;
    ulong outputs;
    ulong outputs_counts;
    ulong outputs_offsets;
    ulong outputs_total;
    ulong slots_count;
    uint hot_count;
    uint classes_count;
    uint state_count;
    uint root;
    uint needles_count;
    uint max_source_match_bytes;
    uint min_source_match_bytes;
    uint max_outputs_per_state;
    uint case_sensitivity;
    ulong root_live[4];
    uint overlap_policy;
    ulong matches_budget;
    ulong chunk_budget;
    ulong haystacks_budget;
    ulong report;
    ulong capability;
    ulong allocator[4];
    ulong memory;
    ulong memory_bytes;
    ulong scratch;
    ulong scratch_bytes;
};

static_assert(sizeof(substrings_engine_t) == 272, "Laid out as `sz_substrings_engine_t`");

/** A regex engine passed by value, laid out as @c sz_regex_engine_t . */
struct regex_engine_t {
    ulong byte_to_class;
    ulong reverse_rows;
    ulong forward_rows;
    ulong forward_patterns;
    ulong forward_escapes;
    ulong prefixes;
    uint forward_starts[4];
    uint reverse_starts[4];
    uint forward_accepts_begin;
    uint reverse_accepts_begin;
    uint classes_count;
    uint forward_states_count;
    uint reverse_states_count;
    uint patterns_count;
    ulong reverse_live[4];
    uint positions_words;
    uint positions_shifts_count;
    uint positions_groups_count;
    uint positions_ranges_count;
    ulong positions_reach;
    ulong positions_shifts;
    ulong positions_groups;
    ulong positions_all;
    ulong positions_initial;
    ulong positions_ascii;
    ulong positions_ranges;
    ulong nfa_states;
    ulong nfa_links;
    uint nfa_states_count;
    uint nfa_links_count;
    uint nfa_start;
    ulong patterns_starts;
    ulong patterns_groups;
    ulong lookarounds;
    ulong bodies;
    ulong looks_bytes;
    uint lookarounds_count;
    ulong groups_names[4];
    uint case_sensitivity;
    ulong capability;
    ulong allocator[4];
    ulong memory;
    ulong memory_bytes;
    ulong scratch;
    ulong scratch_bytes;
};

static_assert(sizeof(regex_engine_t) == 400, "Laid out as `sz_regex_engine_t`");

/** A subdocuments engine passed by value, its regex engine inside it, laid out as
 *  @c sz_subdocuments_engine_t . */
struct subdocuments_engine_t {
    ulong names_data;
    ulong names_offsets;
    ulong names_slots;
    ulong names_steps;
    ulong wildcard_steps;
    ulong descendant_steps;
    ulong relative_steps;
    ulong attribute_steps;
    ulong text_steps;
    ulong filter_steps;
    ulong filters_roots;
    ulong filters;
    ulong instructions;
    ulong strings_data;
    ulong final_steps;
    ulong root_steps;
    ulong holder_steps;
    ulong slices;
    ulong slices_offsets;
    ulong steps_paths;
    uint steps_count;
    uint words_count;
    uint names_slots_count;
    uint paths_count;
    uint filters_count;
    regex_engine_t regex;
    uint patterns_count;
    uint format;
    uint options;
    ulong capability;
    ulong allocator[4];
    ulong memory;
    ulong memory_bytes;
    ulong scratch;
    ulong scratch_bytes;
};

static_assert(sizeof(subdocuments_engine_t) == 672, "Laid out as `sz_subdocuments_engine_t`");

/** One page's values and the bits a kernel marks, laid out as @c matching_page_t . */
struct matching_page_t {
    ulong bytes;
    ulong offsets;
    ulong picked;
    uint picked_count;
    ulong contained;
};

static_assert(sizeof(matching_page_t) == 40, "Laid out as `matching_page_t`");

/** One chunk of a long value, laid out as @c matching_chunk_t . */
struct matching_chunk_t {
    uint value;
    uint index;
};

static_assert(sizeof(matching_chunk_t) == 8, "Laid out as `matching_chunk_t`");

/** The chunks of a page's long values, laid out as @c matching_chunks_t . */
struct matching_chunks_t {
    matching_page_t page;
    ulong chunks;
    uint chunks_count;
    ulong chunk_bytes;
};

static_assert(sizeof(matching_chunks_t) == 64, "Laid out as `matching_chunks_t`");

/** The stacks, keys and refusal of a subdocuments launch, laid out as @c matching_walkers_t . */
struct matching_walkers_t {
    ulong stacks;
    ulong stack_words;
    ulong frame_words;
    ulong keys;
    ulong key_bytes;
    ulong refused;
};

static_assert(sizeof(matching_walkers_t) == 48, "Laid out as `matching_walkers_t`");

/** Where a subdocuments launch stores the nodes it finds, laid out as @c matching_found_t . */
struct matching_found_t {
    ulong matches;
    ulong counts;
    ulong capacity;
};

static_assert(sizeof(matching_found_t) == 24, "Laid out as `matching_found_t`");

} // namespace ustore
} // namespace unum

#pragma endregion Mirrors

#pragma region Arguments

namespace unum {
namespace ustore {

/** The arguments of @c ustore_substrings_contains_per_thread and @c _per_group . */
struct substrings_page_arguments_t {
    substrings_engine_t engine;
    matching_page_t page;
    uint staged_count;
};

static_assert(sizeof(substrings_page_arguments_t) == 320, "Packed as the CUDA parameters");

/** The arguments of @c ustore_substrings_contains_per_chunk . */
struct substrings_chunks_arguments_t {
    substrings_engine_t engine;
    matching_chunks_t chunks;
    uint staged_count;
};

static_assert(sizeof(substrings_chunks_arguments_t) == 344, "Packed as the CUDA parameters");

/** The arguments of @c ustore_regex_contains_per_thread , then the record the regex walks read. */
struct regex_thread_arguments_t {
    regex_engine_t engine;
    matching_page_t page;
    uint cells;
    sz_regex_arguments_metal_t walk;
};

static_assert(sizeof(regex_thread_arguments_t) == 448 + sizeof(sz_regex_arguments_metal_t),
              "The CUDA parameters, then the walk record");

/** The arguments of @c ustore_regex_contains_per_group , then the record the regex walks read. */
struct regex_group_arguments_t {
    regex_engine_t engine;
    matching_page_t page;
    ulong images;
    uint cells;
    sz_regex_arguments_metal_t walk;
};

static_assert(sizeof(regex_group_arguments_t) == 456 + sizeof(sz_regex_arguments_metal_t),
              "The CUDA parameters, then the walk record");

/** The arguments of @c ustore_regex_contains_per_chunk , then the record the regex walks read. */
struct regex_chunks_arguments_t {
    regex_engine_t engine;
    matching_chunks_t chunks;
    ulong images;
    uint cells;
    ulong refused;
    sz_regex_arguments_metal_t walk;
};

static_assert(sizeof(regex_chunks_arguments_t) == 488 + sizeof(sz_regex_arguments_metal_t),
              "The CUDA parameters, then the walk record");

/** The arguments of @c ustore_subdocuments_contains_per_thread and @c _per_group . */
struct subdocuments_walk_arguments_t {
    subdocuments_engine_t engine;
    matching_page_t page;
    matching_walkers_t walkers;
};

static_assert(sizeof(subdocuments_walk_arguments_t) == 760, "Packed as the CUDA parameters");

/** The arguments of @c ustore_subdocuments_contains_per_chunk . */
struct subdocuments_chunked_arguments_t {
    subdocuments_engine_t engine;
    matching_page_t page;
    matching_walkers_t walkers;
    ulong chunk_bytes;
};

static_assert(sizeof(subdocuments_chunked_arguments_t) == 768, "Packed as the CUDA parameters");

/** The arguments of @c ustore_subdocuments_find_per_group . */
struct subdocuments_find_arguments_t {
    subdocuments_engine_t engine;
    matching_page_t page;
    matching_walkers_t walkers;
    matching_found_t found;
};

static_assert(sizeof(subdocuments_find_arguments_t) == 784, "Packed as the CUDA parameters");

/** The arguments of @c ustore_subdocuments_find_per_chunk . */
struct subdocuments_find_chunked_arguments_t {
    subdocuments_engine_t engine;
    matching_page_t page;
    matching_walkers_t walkers;
    matching_found_t found;
    ulong chunk_bytes;
};

static_assert(sizeof(subdocuments_find_chunked_arguments_t) == 792, "Packed as the CUDA parameters");

} // namespace ustore
} // namespace unum

#pragma endregion Arguments

#pragma region Matching

namespace unum {
namespace ustore {

/** Lanes of a group walking one medium value, a simdgroup, as @c matching_group_lanes_k . */
constant constexpr uint matching_group_lanes_k = 32;

/** Sets the bit of @p value in @p contained , as other lanes may set bits of the same word, as
 *  @c mark_contained does where a value holds a match. */
inline void mark_contained(device atomic_uint* contained, uint value) {
    atomic_fetch_or_explicit(contained + value / 32, 1u << (value % 32), memory_order_relaxed);
}

/** This simdgroup as one group of StringZilla's walks. */
inline sz_group_metal_t matching_group(uint simd_lane, uint simd_width) {
    sz_group_metal_t const group = {simd_width, simd_lane};
    return group;
}

/** Reaches the automaton of a byte-exact substrings @p engine into @p automaton , then stages
 *  @p staged_count rows of its hot head and its class map into @p staged , as @c stage_substrings
 *  does; every thread of the threadgroup calls it, and it ends in a barrier. */
inline sz_substrings_staged_metal_t stage_substrings(constant substrings_engine_t& engine, blocks_t blocks,
                                                     uint staged_count, threadgroup uchar* staged, uint lane,
                                                     uint width, thread sz_substrings_automaton_metal_t& automaton) {
    automaton.hot_rows = reach<uint const>(blocks, engine.hot_rows);
    automaton.byte_to_class = reach<uchar const>(blocks, engine.byte_to_class);
    automaton.base = reach<uint const>(blocks, engine.base), automaton.check = reach<uint const>(blocks, engine.check);
    automaton.fail = reach<uint const>(blocks, engine.fail);
    automaton.accepts_words = reach<uint const>(blocks, engine.accepts_words);
    automaton.outputs_counts = reach<uint const>(blocks, engine.outputs_counts);
    automaton.outputs = reach<sz_substrings_output_metal_t const>(blocks, engine.outputs);
    automaton.outputs_offsets = reach<ulong const>(blocks, engine.outputs_offsets);
    automaton.hot_count = engine.hot_count, automaton.classes_count = engine.classes_count;
    automaton.root = engine.root, automaton.max_source_match_bytes = engine.max_source_match_bytes;
    automaton.overlap_policy = sz_substrings_overlap_policy_metal_t(engine.overlap_policy);
    // A C `sz_case_sensitivity_t` holds zero for a byte-exact engine
    automaton.uncased = engine.case_sensitivity != 0;
    return sz_substrings_stage_metal_(automaton, staged, staged_count, lane, width);
}

/** Stages @p cells of the start-finding automaton of a byte-exact regex @p engine , priming from
 *  its column @p images or null, into @p staged , as @c stage_regex does; every thread of the
 *  threadgroup calls it, and it ends in a barrier. */
inline sz_regex_table_metal_t stage_regex(constant regex_engine_t& engine, blocks_t blocks, device uint const* images,
                                          uint cells, threadgroup uchar* staged, uint lane, uint width) {
    return sz_regex_stage_table_metal_(reach<uchar const>(blocks, engine.byte_to_class),
                                       reach<uint const>(blocks, engine.reverse_rows), images, engine.classes_count,
                                       engine.lookarounds_count, cells, staged, lane, width);
}

/** Walks the JSON value @p value of @p page on the lanes of @p group with the stack and key buffer
 *  of walker @p walker , in chunks of @p chunk_bytes where it is not zero, storing the first
 *  @p matches_capacity of its matches at @p matches and counting every one into @p emitted , as
 *  @c subdocuments_walk_group does. */
inline int subdocuments_walk_group(constant subdocuments_engine_t& engine, constant matching_page_t& page,
                                   constant matching_walkers_t& walkers, blocks_t blocks, uint walker, uint value,
                                   ulong chunk_bytes, device sz_match_metal_t* matches, ulong matches_capacity,
                                   thread ulong& emitted, thread sz_group_metal_t const& group) {
    device ulong const* const offsets = reach<ulong const>(blocks, page.offsets);
    device uchar const* const text = reach<uchar const>(blocks, page.bytes) + offsets[value];
    ulong const length = offsets[value + 1] - offsets[value];
    device ulong* const stack = reach<ulong>(blocks, walkers.stacks) + ulong(walker) * walkers.stack_words;
    device uchar* const key = reach<uchar>(blocks, walkers.keys) + ulong(walker) * walkers.key_bytes;
    ulong const frames = walkers.stack_words / walkers.frame_words;
    sz_subdocuments_engine_metal_t reached;
    reached.names_data = reach<uchar const>(blocks, engine.names_data);
    reached.strings_data = reach<uchar const>(blocks, engine.strings_data);
    reached.names_offsets = reach<uint const>(blocks, engine.names_offsets);
    reached.names_slots = reach<uint const>(blocks, engine.names_slots);
    reached.slices_offsets = reach<uint const>(blocks, engine.slices_offsets);
    reached.steps_paths = reach<uint const>(blocks, engine.steps_paths);
    reached.names_steps = reach<ulong const>(blocks, engine.names_steps);
    reached.wildcard_steps = reach<ulong const>(blocks, engine.wildcard_steps);
    reached.descendant_steps = reach<ulong const>(blocks, engine.descendant_steps);
    reached.relative_steps = reach<ulong const>(blocks, engine.relative_steps);
    reached.attribute_steps = reach<ulong const>(blocks, engine.attribute_steps);
    reached.text_steps = reach<ulong const>(blocks, engine.text_steps);
    reached.filter_steps = reach<ulong const>(blocks, engine.filter_steps);
    reached.filters_roots = reach<ulong const>(blocks, engine.filters_roots);
    reached.final_steps = reach<ulong const>(blocks, engine.final_steps);
    reached.root_steps = reach<ulong const>(blocks, engine.root_steps);
    reached.slices = reach<sz_subdocuments_slice_metal_t const>(blocks, engine.slices);
    reached.filters = reach<sz_subdocuments_filter_metal_t const>(blocks, engine.filters);
    reached.instructions = reach<sz_subdocuments_instruction_metal_t const>(blocks, engine.instructions);
    reached.words_count = engine.words_count, reached.names_slots_count = engine.names_slots_count;
    reached.filters_count = engine.filters_count, reached.format = engine.format;
    // Paths without patterns carry no regex engine, so its fields stay empty
    bool const patterned = engine.patterns_count != 0;
    reached.byte_to_class = patterned ? reach<uchar const>(blocks, engine.regex.byte_to_class) : nullptr;
    reached.forward_rows = patterned ? reach<uint const>(blocks, engine.regex.forward_rows) : nullptr;
    reached.forward_start = patterned ? engine.regex.forward_starts[0] : 0;
    reached.forward_accepts_begin = patterned ? engine.regex.forward_accepts_begin : 0;
    reached.classes_count = patterned ? engine.regex.classes_count : 0;
    sz_subdocuments_progress_metal_t carried;
    sz_subdocuments_prime_metal_(reached, text, length, stack, carried);
    int status = sz_subdocuments_walked_metal_k;
    ulong const step = chunk_bytes ? chunk_bytes : length;
    for (ulong end = 0; status == sz_subdocuments_walked_metal_k && end != length;) {
        end = length - end > step ? end + step : length;
        status = sz_subdocuments_first_chunk_metal_(reached, text, length, value, stack, frames, walkers.frame_words,
                                                    key, walkers.key_bytes, matches, matches_capacity, emitted, end,
                                                    group, carried);
    }
    return status;
}

/** Marks the JSON values of @p page a path of @p engine selects in, a simdgroup each walking with
 *  its own stack, in chunks of @p chunk_bytes where not zero, as @c subdocuments_per_group does. */
inline void subdocuments_per_group(constant subdocuments_engine_t& engine, constant matching_page_t& page,
                                   constant matching_walkers_t& walkers, ulong chunk_bytes, blocks_t blocks,
                                   uint position, uint simd_lane, uint simd_width) {
    uint const slot = position / matching_group_lanes_k;
    if (slot >= page.picked_count) return;
    sz_group_metal_t const group = matching_group(simd_lane, simd_width);
    uint const value = reach<uint const>(blocks, page.picked)[slot];
    ulong emitted = 0;
    int const status = subdocuments_walk_group(engine, page, walkers, blocks, slot, value, chunk_bytes, nullptr, 0,
                                               emitted, group);
    if (group.lane != 0) return;
    if (status != sz_subdocuments_walked_metal_k)
        atomic_fetch_or_explicit(reach<atomic_uint>(blocks, walkers.refused), 1u, memory_order_relaxed);
    else if (emitted != 0) mark_contained(reach<atomic_uint>(blocks, page.contained), value);
}

/** Stores the nodes the paths of @p engine select in the JSON values of @p page into @p found , a
 *  simdgroup each walking with its own stack, in chunks of @p chunk_bytes where it is not zero,
 *  raising the refusal for a value with more nodes than its slots, as
 *  @c subdocuments_find_per_group does. */
inline void subdocuments_find_per_group(constant subdocuments_engine_t& engine, constant matching_page_t& page,
                                        constant matching_walkers_t& walkers, constant matching_found_t& found,
                                        ulong chunk_bytes, blocks_t blocks, uint position, uint simd_lane,
                                        uint simd_width) {
    uint const slot = position / matching_group_lanes_k;
    if (slot >= page.picked_count) return;
    sz_group_metal_t const group = matching_group(simd_lane, simd_width);
    uint const value = reach<uint const>(blocks, page.picked)[slot];
    device sz_match_metal_t* const matches = reach<sz_match_metal_t>(blocks, found.matches) +
                                             ulong(value) * found.capacity;
    ulong emitted = 0;
    int const status = subdocuments_walk_group(engine, page, walkers, blocks, slot, value, chunk_bytes, matches,
                                               found.capacity, emitted, group);
    if (group.lane != 0) return;
    if (status != sz_subdocuments_walked_metal_k || emitted > found.capacity)
        atomic_fetch_or_explicit(reach<atomic_uint>(blocks, walkers.refused), 1u, memory_order_relaxed);
    else reach<uint>(blocks, found.counts)[value] = uint(emitted);
}

} // namespace ustore
} // namespace unum

#pragma endregion Matching

#pragma region Matching Kernels

/** Marks the short values of a page holding a needle of a byte-exact substrings engine, a lane
 *  each, as CUDA's @c ustore_substrings_contains_per_thread . */
kernel void ustore_substrings_contains_per_thread(
    constant substrings_page_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
    device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
    constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
    threadgroup uchar* shared [[threadgroup(metal_shared_slot_k)]], uint slot [[thread_position_in_grid]],
    uint lane [[thread_index_in_threadgroup]], uint width [[threads_per_threadgroup]]) {
    blocks_t const blocks = {table, table_count};
    sz_substrings_automaton_metal_t automaton;
    sz_substrings_staged_metal_t const staged = stage_substrings(arguments.engine, blocks, arguments.staged_count,
                                                                 shared, lane, width, automaton);
    if (slot >= arguments.page.picked_count) return;
    device ulong const* const offsets = reach<ulong const>(blocks, arguments.page.offsets);
    uint const value = reach<uint const>(blocks, arguments.page.picked)[slot];
    device uchar const* const text = reach<uchar const>(blocks, arguments.page.bytes) + offsets[value];
    if (sz_substrings_first_thread_metal_(automaton, staged, text, offsets[value + 1] - offsets[value]))
        mark_contained(reach<atomic_uint>(blocks, arguments.page.contained), value);
}

/** Marks the medium values of a page holding a needle of a byte-exact substrings engine, a
 *  simdgroup each, as CUDA's @c ustore_substrings_contains_per_group . */
kernel void ustore_substrings_contains_per_group(
    constant substrings_page_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
    device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
    constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
    threadgroup uchar* shared [[threadgroup(metal_shared_slot_k)]], uint position [[thread_position_in_grid]],
    uint lane [[thread_index_in_threadgroup]], uint width [[threads_per_threadgroup]],
    uint simd_lane [[thread_index_in_simdgroup]], uint simd_width [[threads_per_simdgroup]]) {
    blocks_t const blocks = {table, table_count};
    sz_substrings_automaton_metal_t automaton;
    sz_substrings_staged_metal_t const staged = stage_substrings(arguments.engine, blocks, arguments.staged_count,
                                                                 shared, lane, width, automaton);
    uint const slot = position / matching_group_lanes_k;
    if (slot >= arguments.page.picked_count) return;
    sz_group_metal_t const group = matching_group(simd_lane, simd_width);
    device ulong const* const offsets = reach<ulong const>(blocks, arguments.page.offsets);
    uint const value = reach<uint const>(blocks, arguments.page.picked)[slot];
    device uchar const* const text = reach<uchar const>(blocks, arguments.page.bytes) + offsets[value];
    bool const found = sz_substrings_first_group_metal_(automaton, staged, text, offsets[value + 1] - offsets[value],
                                                        group);
    if (group.lane == 0 && found) mark_contained(reach<atomic_uint>(blocks, arguments.page.contained), value);
}

/** Marks the long values of a page holding a needle of a byte-exact substrings engine, a lane per
 *  chunk primed over the bytes before it, as CUDA's @c ustore_substrings_contains_per_chunk . */
kernel void ustore_substrings_contains_per_chunk(
    constant substrings_chunks_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
    device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
    constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
    threadgroup uchar* shared [[threadgroup(metal_shared_slot_k)]], uint slot [[thread_position_in_grid]],
    uint lane [[thread_index_in_threadgroup]], uint width [[threads_per_threadgroup]]) {
    blocks_t const blocks = {table, table_count};
    sz_substrings_automaton_metal_t automaton;
    sz_substrings_staged_metal_t const staged = stage_substrings(arguments.engine, blocks, arguments.staged_count,
                                                                 shared, lane, width, automaton);
    if (slot >= arguments.chunks.chunks_count) return;
    matching_chunk_t const chunk = reach<matching_chunk_t const>(blocks, arguments.chunks.chunks)[slot];
    device ulong const* const offsets = reach<ulong const>(blocks, arguments.chunks.page.offsets);
    device uchar const* const text = reach<uchar const>(blocks, arguments.chunks.page.bytes) + offsets[chunk.value];
    ulong const length = offsets[chunk.value + 1] - offsets[chunk.value];
    ulong const begin = ulong(chunk.index) * arguments.chunks.chunk_bytes;
    uint carried = sz_substrings_prime_metal_(automaton, staged, text, begin);
    if (sz_substrings_first_chunk_metal_(automaton, staged, text, length, begin, begin + arguments.chunks.chunk_bytes,
                                         carried))
        mark_contained(reach<atomic_uint>(blocks, arguments.chunks.page.contained), chunk.value);
}

/** Marks the values of a page a pattern of a byte-exact regex engine matches in, a lane each, as
 *  CUDA's @c ustore_regex_contains_per_thread . */
kernel void ustore_regex_contains_per_thread(constant regex_thread_arguments_t& arguments
                                             [[buffer(metal_arguments_slot_k)]],
                                             device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                             constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                             threadgroup uchar* shared [[threadgroup(metal_shared_slot_k)]],
                                             uint slot [[thread_position_in_grid]],
                                             uint lane [[thread_index_in_threadgroup]],
                                             uint width [[threads_per_threadgroup]]) {
    blocks_t const blocks = {table, table_count};
    sz_regex_table_metal_t const staged = stage_regex(arguments.engine, blocks, nullptr, arguments.cells, shared, lane,
                                                      width);
    if (slot >= arguments.page.picked_count) return;
    device ulong const* const offsets = reach<ulong const>(blocks, arguments.page.offsets);
    uint const value = reach<uint const>(blocks, arguments.page.picked)[slot];
    device uchar const* const text = reach<uchar const>(blocks, arguments.page.bytes) + offsets[value];
    if (sz_regex_first_thread_metal_(staged, arguments.walk, text, offsets[value + 1] - offsets[value]))
        mark_contained(reach<atomic_uint>(blocks, arguments.page.contained), value);
}

/** Marks the values of a page a pattern of a byte-exact regex engine matches in, a simdgroup each
 *  primed from its column images, as CUDA's @c ustore_regex_contains_per_group . */
kernel void ustore_regex_contains_per_group(
    constant regex_group_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
    device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
    constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
    threadgroup uchar* shared [[threadgroup(metal_shared_slot_k)]], uint position [[thread_position_in_grid]],
    uint lane [[thread_index_in_threadgroup]], uint width [[threads_per_threadgroup]],
    uint simd_lane [[thread_index_in_simdgroup]], uint simd_width [[threads_per_simdgroup]]) {
    blocks_t const blocks = {table, table_count};
    sz_regex_table_metal_t const staged = stage_regex(
        arguments.engine, blocks, reach<uint const>(blocks, arguments.images), arguments.cells, shared, lane, width);
    uint const slot = position / matching_group_lanes_k;
    if (slot >= arguments.page.picked_count) return;
    sz_group_metal_t const group = matching_group(simd_lane, simd_width);
    device ulong const* const offsets = reach<ulong const>(blocks, arguments.page.offsets);
    uint const value = reach<uint const>(blocks, arguments.page.picked)[slot];
    device uchar const* const text = reach<uchar const>(blocks, arguments.page.bytes) + offsets[value];
    bool const found = sz_regex_first_group_metal_(staged, arguments.walk, text, offsets[value + 1] - offsets[value],
                                                   group);
    if (group.lane == 0 && found) mark_contained(reach<atomic_uint>(blocks, arguments.page.contained), value);
}

/** Marks the values of a page a pattern of a byte-exact regex engine matches in, a lane per chunk
 *  entering it from the states its column images prime at its end, raising the refusal where those
 *  walks disagree, as CUDA's @c ustore_regex_contains_per_chunk . */
kernel void ustore_regex_contains_per_chunk(constant regex_chunks_arguments_t& arguments
                                            [[buffer(metal_arguments_slot_k)]],
                                            device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                            constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                            threadgroup uchar* shared [[threadgroup(metal_shared_slot_k)]],
                                            uint slot [[thread_position_in_grid]],
                                            uint lane [[thread_index_in_threadgroup]],
                                            uint width [[threads_per_threadgroup]]) {
    blocks_t const blocks = {table, table_count};
    device uint const* const images = reach<uint const>(blocks, arguments.images);
    sz_regex_table_metal_t const staged = stage_regex(arguments.engine, blocks, images, arguments.cells, shared, lane,
                                                      width);
    if (slot >= arguments.chunks.chunks_count) return;
    matching_chunk_t const chunk = reach<matching_chunk_t const>(blocks, arguments.chunks.chunks)[slot];
    device ulong const* const offsets = reach<ulong const>(blocks, arguments.chunks.page.offsets);
    device uchar const* const text = reach<uchar const>(blocks, arguments.chunks.page.bytes) + offsets[chunk.value];
    ulong const length = offsets[chunk.value + 1] - offsets[chunk.value];
    ulong const begin = ulong(chunk.index) * arguments.chunks.chunk_bytes;
    ulong const end = min(begin + arguments.chunks.chunk_bytes, length);
    uint states[sz_regex_priming_states_metal_k], count = 0, founds = 0;
    uint const primed = sz_regex_prime_metal_(staged, arguments.walk, images, text, 0, length, end,
                                              sz_regex_priming_bytes_metal_k, states, count);
    if (primed) states[0] = primed, count = 1;
    for (uint index = 0; index != count; ++index) {
        uint carried = states[index];
        founds += sz_regex_first_chunk_metal_(staged, arguments.walk, text, length, begin, end, carried) ? 1u : 0u;
    }
    if (count == 0 || (founds != 0 && founds != count))
        atomic_fetch_or_explicit(reach<atomic_uint>(blocks, arguments.refused), 1u, memory_order_relaxed);
    else if (founds != 0) mark_contained(reach<atomic_uint>(blocks, arguments.chunks.page.contained), chunk.value);
}

/** Marks the XML values of a page a path of a subdocuments engine selects in, a lane each walking
 *  with its own stack, as CUDA's @c ustore_subdocuments_contains_per_thread . */
kernel void ustore_subdocuments_contains_per_thread(constant subdocuments_walk_arguments_t& arguments
                                                    [[buffer(metal_arguments_slot_k)]],
                                                    device ustore_metal_block_t const* table
                                                    [[buffer(metal_blocks_slot_k)]],
                                                    constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                                    uint slot [[thread_position_in_grid]]) {
    if (slot >= arguments.page.picked_count) return;
    blocks_t const blocks = {table, table_count};
    sz_subdocuments_engine_metal_t reached;
    reached.names_data = reach<uchar const>(blocks, arguments.engine.names_data);
    reached.strings_data = reach<uchar const>(blocks, arguments.engine.strings_data);
    reached.names_offsets = reach<uint const>(blocks, arguments.engine.names_offsets);
    reached.names_slots = reach<uint const>(blocks, arguments.engine.names_slots);
    reached.slices_offsets = reach<uint const>(blocks, arguments.engine.slices_offsets);
    reached.steps_paths = reach<uint const>(blocks, arguments.engine.steps_paths);
    reached.names_steps = reach<ulong const>(blocks, arguments.engine.names_steps);
    reached.wildcard_steps = reach<ulong const>(blocks, arguments.engine.wildcard_steps);
    reached.descendant_steps = reach<ulong const>(blocks, arguments.engine.descendant_steps);
    reached.relative_steps = reach<ulong const>(blocks, arguments.engine.relative_steps);
    reached.attribute_steps = reach<ulong const>(blocks, arguments.engine.attribute_steps);
    reached.text_steps = reach<ulong const>(blocks, arguments.engine.text_steps);
    reached.filter_steps = reach<ulong const>(blocks, arguments.engine.filter_steps);
    reached.filters_roots = reach<ulong const>(blocks, arguments.engine.filters_roots);
    reached.final_steps = reach<ulong const>(blocks, arguments.engine.final_steps);
    reached.root_steps = reach<ulong const>(blocks, arguments.engine.root_steps);
    reached.slices = reach<sz_subdocuments_slice_metal_t const>(blocks, arguments.engine.slices);
    reached.filters = reach<sz_subdocuments_filter_metal_t const>(blocks, arguments.engine.filters);
    reached.instructions = reach<sz_subdocuments_instruction_metal_t const>(blocks, arguments.engine.instructions);
    reached.words_count = arguments.engine.words_count, reached.names_slots_count = arguments.engine.names_slots_count;
    reached.filters_count = arguments.engine.filters_count, reached.format = arguments.engine.format;
    // Paths without patterns carry no regex engine, so its fields stay empty
    bool const patterned = arguments.engine.patterns_count != 0;
    reached.byte_to_class = patterned ? reach<uchar const>(blocks, arguments.engine.regex.byte_to_class) : nullptr;
    reached.forward_rows = patterned ? reach<uint const>(blocks, arguments.engine.regex.forward_rows) : nullptr;
    reached.forward_start = patterned ? arguments.engine.regex.forward_starts[0] : 0;
    reached.forward_accepts_begin = patterned ? arguments.engine.regex.forward_accepts_begin : 0;
    reached.classes_count = patterned ? arguments.engine.regex.classes_count : 0;
    constant matching_walkers_t& walkers = arguments.walkers;
    device ulong const* const offsets = reach<ulong const>(blocks, arguments.page.offsets);
    uint const value = reach<uint const>(blocks, arguments.page.picked)[slot];
    device uchar const* const text = reach<uchar const>(blocks, arguments.page.bytes) + offsets[value];
    device ulong* const stack = reach<ulong>(blocks, walkers.stacks) + ulong(slot) * walkers.stack_words;
    ulong emitted = 0;
    int const status = sz_subdocuments_walk_markup_metal_(reached, text, offsets[value + 1] - offsets[value], value,
                                                          stack, walkers.stack_words / walkers.frame_words,
                                                          walkers.frame_words, walkers.key_bytes, nullptr, 0, emitted);
    if (status != sz_subdocuments_walked_metal_k)
        atomic_fetch_or_explicit(reach<atomic_uint>(blocks, walkers.refused), 1u, memory_order_relaxed);
    else if (emitted != 0) mark_contained(reach<atomic_uint>(blocks, arguments.page.contained), value);
}

/** Marks the JSON values of a page a path of a subdocuments engine selects in, a simdgroup each,
 *  as CUDA's @c ustore_subdocuments_contains_per_group . */
kernel void ustore_subdocuments_contains_per_group(
    constant subdocuments_walk_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
    device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
    constant uint& table_count [[buffer(metal_blocks_count_slot_k)]], uint position [[thread_position_in_grid]],
    uint simd_lane [[thread_index_in_simdgroup]], uint simd_width [[threads_per_simdgroup]]) {
    blocks_t const blocks = {table, table_count};
    subdocuments_per_group(arguments.engine, arguments.page, arguments.walkers, 0, blocks, position, simd_lane,
                           simd_width);
}

/** Marks the long JSON values of a page a path of a subdocuments engine selects in, a simdgroup
 *  each walking them in chunks, as CUDA's @c ustore_subdocuments_contains_per_chunk . */
kernel void ustore_subdocuments_contains_per_chunk(
    constant subdocuments_chunked_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
    device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
    constant uint& table_count [[buffer(metal_blocks_count_slot_k)]], uint position [[thread_position_in_grid]],
    uint simd_lane [[thread_index_in_simdgroup]], uint simd_width [[threads_per_simdgroup]]) {
    blocks_t const blocks = {table, table_count};
    subdocuments_per_group(arguments.engine, arguments.page, arguments.walkers, arguments.chunk_bytes, blocks, position,
                           simd_lane, simd_width);
}

/** Stores the nodes the paths of a subdocuments engine select in the JSON values of a page, a
 *  simdgroup each, as CUDA's @c ustore_subdocuments_find_per_group . */
kernel void ustore_subdocuments_find_per_group(constant subdocuments_find_arguments_t& arguments
                                               [[buffer(metal_arguments_slot_k)]],
                                               device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                               constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                               uint position [[thread_position_in_grid]],
                                               uint simd_lane [[thread_index_in_simdgroup]],
                                               uint simd_width [[threads_per_simdgroup]]) {
    blocks_t const blocks = {table, table_count};
    subdocuments_find_per_group(arguments.engine, arguments.page, arguments.walkers, arguments.found, 0, blocks,
                                position, simd_lane, simd_width);
}

/** Stores the nodes the paths of a subdocuments engine select in the long JSON values of a page, a
 *  simdgroup each walking them in chunks, as CUDA's @c ustore_subdocuments_find_per_chunk . */
kernel void ustore_subdocuments_find_per_chunk(constant subdocuments_find_chunked_arguments_t& arguments
                                               [[buffer(metal_arguments_slot_k)]],
                                               device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                               constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                               uint position [[thread_position_in_grid]],
                                               uint simd_lane [[thread_index_in_simdgroup]],
                                               uint simd_width [[threads_per_simdgroup]]) {
    blocks_t const blocks = {table, table_count};
    subdocuments_find_per_group(arguments.engine, arguments.page, arguments.walkers, arguments.found,
                                arguments.chunk_bytes, blocks, position, simd_lane, simd_width);
}

#pragma endregion Matching Kernels
