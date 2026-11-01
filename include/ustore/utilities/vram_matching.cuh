/**
 *  @file include/ustore/utilities/vram_matching.cuh
 *  @author Ash Vardanian
 *  @date October 31, 2026
 *  @brief The page of values a device match walks, as the host lays it out and the matching kernels
 *      read it, and the device steps those kernels share: staging an engine's head into shared
 *      memory and marking a value's bit.
 *
 *  A page's values sit on the device as one tape with 64-bit offsets. The host sorts them by
 *  length into three lists of indices: short values one lane walks alone, medium ones a group of
 *  32 lanes walks together, and long ones cut into chunks a lane each walks. Every kernel marks a
 *  value's bit in a bitmap of 32-bit words the host zeroed, so the walks of one value's chunks may
 *  land in any order.
 */

#pragma once

#include <cstdint> // `std::uint32_t`, `std::uint64_t`

#include <stringzilla/regex/simt.cuh>        // `sz_regex_stage_table_simt_`
#include <stringzilla/subdocuments/simt.cuh> // `sz_subdocuments_first_chunk_simt_`
#include <stringzilla/substrings/simt.cuh>   // `sz_substrings_stage_simt_`

namespace unum::ustore::utilities {

/** One page's values on the device and the bits a kernel marks: the tape, the values one launch
 *  walks, and the bitmap. */
struct matching_page_t {

    /** The bytes of every value of the page, back to back. */
    char const* bytes = nullptr;

    /** One offset per value and one past the last. */
    std::uint64_t const* offsets = nullptr;

    /** The values this launch walks, by their index in the page. */
    std::uint32_t const* picked = nullptr;

    /** How many values @c picked lists. */
    std::uint32_t picked_count = 0;

    /** One bit per value of the page, set where its value holds a match. */
    std::uint32_t* contained = nullptr;

    /** The first byte of @p value. */
    constexpr char const* start(std::uint32_t value) const noexcept { return bytes + offsets[value]; }

    /** The bytes of @p value. */
    constexpr std::uint64_t length(std::uint32_t value) const noexcept { return offsets[value + 1] - offsets[value]; }
};

/** One chunk of a long value: the value and which of its chunks, every chunk but its last as long
 *  as a launch's. */
struct matching_chunk_t {
    std::uint32_t value = 0;
    std::uint32_t index = 0;
};

static_assert(sizeof(matching_chunk_t) == 8, "A chunk is one 8-byte word");

/** The chunks of a page's long values a launch walks, a lane each. */
struct matching_chunks_t {

    /** The page the chunks cut, whose @c picked the chunks replace. */
    matching_page_t page;

    /** The chunks, of every long value in turn. */
    matching_chunk_t const* chunks = nullptr;

    /** How many chunks @c chunks lists. */
    std::uint32_t chunks_count = 0;

    /** Bytes of every chunk but a value's last. */
    std::uint64_t chunk_bytes = 0;
};

/** The memory every walker of a subdocuments launch keeps its open containers and unescaped keys
 *  in, one stack and one key buffer per walker, and the word a walk that failed raises, so the host
 *  matches the page itself. */
struct matching_walkers_t {

    /** Every walker's stack, @c stack_words words apart. */
    std::uint64_t* stacks = nullptr;

    /** Words of one walker's stack, whole frames of @c frame_words words. */
    std::uint64_t stack_words = 0;

    /** Words of one frame of the engine's walks. */
    std::uint64_t frame_words = 0;

    /** Every walker's key buffer, @c key_bytes bytes apart. */
    unsigned char* keys = nullptr;

    /** Bytes of one walker's key buffer. */
    std::uint64_t key_bytes = 0;

    /** Raised by a walk finding its value malformed or deeper than its stack. */
    std::uint32_t* refused = nullptr;
};

/** Threads of every matching kernel's block, eight groups of 32 lanes. */
inline constexpr unsigned matching_block_threads_k = 256;

/** Lanes of a group walking one medium value, on both vendors, where a 64-lane wavefront halves
 *  into two groups. */
inline constexpr unsigned matching_group_lanes_k = 32;

#if defined(__CUDACC__) || defined(__HIP__)

/** Sets the bit of @p value in @p contained where @p found, as other lanes may set bits of
 *  the same word. */
__device__ inline void mark_contained(std::uint32_t* contained, std::uint32_t value, sz_bool_t found) noexcept {
    if (found) atomicOr(contained + value / 32, 1u << (value % 32));
}

/** Stages @p staged_count rows of the hot head of a byte-exact substrings @p engine and its class
 *  map into the kernel's dynamic shared memory; every thread of the block calls it, and it ends
 *  in a barrier. */
__device__ inline sz_substrings_staged_simt_t stage_substrings(sz_substrings_engine_t const& engine,
                                                               std::uint32_t staged_count) noexcept {
    extern __shared__ sz_u8_t matching_staged[];
    sz_u32_t* const rows = reinterpret_cast<sz_u32_t*>(matching_staged + STRINGZILLA_U8_MAX + 1);
    sz_substrings_stage_simt_(&engine, matching_staged, rows, staged_count);
    sz_substrings_staged_simt_t staged;
    staged.classes = matching_staged, staged.rows = rows, staged.rows_count = staged_count;
    staged.accepts_words = engine.accepts_words;
    return staged;
}

/** Stages @p cells of the start-finding automaton of a byte-exact regex @p engine, priming from its
 *  column @p images or null, into the kernel's dynamic shared memory; every thread of the block
 *  calls it, and it ends in a barrier. */
__device__ inline sz_regex_table_simt_t stage_regex(sz_regex_engine_t const& engine, std::uint32_t const* images,
                                                    std::uint32_t cells) noexcept {
    return sz_regex_stage_table_simt_(&engine, engine.reverse_rows, images, cells);
}

/** The index of this thread's slot among a launch's threads, and of its group's slot among
 *  the launch's groups. */
__device__ inline std::uint32_t matching_thread_slot() noexcept { return blockIdx.x * blockDim.x + threadIdx.x; }
__device__ inline std::uint32_t matching_group_slot() noexcept {
    return matching_thread_slot() / matching_group_lanes_k;
}

/** Marks the values of @p page that hold a needle of the byte-exact @p engine, a lane each. */
__device__ inline void substrings_per_thread(sz_substrings_engine_t const& engine, matching_page_t const& page,
                                             std::uint32_t staged_count) noexcept {
    sz_substrings_staged_simt_t const staged = stage_substrings(engine, staged_count);
    std::uint32_t const slot = matching_thread_slot();
    if (slot >= page.picked_count) return;
    std::uint32_t const value = page.picked[slot];
    mark_contained(page.contained, value,
                   sz_substrings_first_thread_simt_(&engine, &staged, page.start(value), page.length(value)));
}

/** Marks the values of @p page that hold a needle of the byte-exact @p engine, the lanes of
 *  @p group each. */
__device__ inline void substrings_per_group(sz_substrings_engine_t const& engine, matching_page_t const& page,
                                            std::uint32_t staged_count, sz_simt_group_t const& group) noexcept {
    sz_substrings_staged_simt_t const staged = stage_substrings(engine, staged_count);
    std::uint32_t const slot = matching_group_slot();
    if (slot >= page.picked_count) return;
    std::uint32_t const value = page.picked[slot];
    sz_bool_t const found = sz_substrings_first_group_simt_(&engine, &staged, page.start(value), page.length(value),
                                                            &group);
    if (group.lane == 0) mark_contained(page.contained, value, found);
}

/** Marks the values of @p chunks that hold a needle of the byte-exact @p engine, a lane per chunk
 *  primed over the bytes before it a match ending in the chunk may start at. */
__device__ inline void substrings_per_chunk(sz_substrings_engine_t const& engine, matching_chunks_t const& chunks,
                                            std::uint32_t staged_count) noexcept {
    sz_substrings_staged_simt_t const staged = stage_substrings(engine, staged_count);
    std::uint32_t const slot = matching_thread_slot();
    if (slot >= chunks.chunks_count) return;
    matching_chunk_t const chunk = chunks.chunks[slot];
    sz_cptr_t const text = chunks.page.start(chunk.value);
    sz_size_t const length = chunks.page.length(chunk.value);
    sz_size_t const begin = chunk.index * chunks.chunk_bytes;
    sz_u32_t carried = sz_substrings_prime_simt_(&engine, &staged, text, begin);
    mark_contained(
        chunks.page.contained, chunk.value,
        sz_substrings_first_chunk_simt_(&engine, &staged, text, length, begin, begin + chunks.chunk_bytes, &carried));
}

/** Marks the values of @p page a pattern of the byte-exact @p engine matches in, a lane each. */
__device__ inline void regex_per_thread(sz_regex_engine_t const& engine, matching_page_t const& page,
                                        std::uint32_t cells) noexcept {
    sz_regex_table_simt_t const table = stage_regex(engine, nullptr, cells);
    std::uint32_t const slot = matching_thread_slot();
    if (slot >= page.picked_count) return;
    std::uint32_t const value = page.picked[slot];
    mark_contained(page.contained, value,
                   sz_regex_first_thread_simt_(&engine, &table, page.start(value), page.length(value)));
}

/** Marks the values of @p page a pattern of the byte-exact @p engine matches in, the lanes
 *  of @p group each, priming from the column @p images. */
__device__ inline void regex_per_group(sz_regex_engine_t const& engine, matching_page_t const& page,
                                       std::uint32_t const* images, std::uint32_t cells,
                                       sz_simt_group_t const& group) noexcept {
    sz_regex_table_simt_t const table = stage_regex(engine, images, cells);
    std::uint32_t const slot = matching_group_slot();
    if (slot >= page.picked_count) return;
    std::uint32_t const value = page.picked[slot];
    sz_bool_t const found = sz_regex_first_group_simt_(&engine, &table, page.start(value), page.length(value), &group);
    if (group.lane == 0) mark_contained(page.contained, value, found);
}

/** Marks the values of @p chunks a pattern of the byte-exact @p engine matches in, a lane per chunk
 *  entering it from the state the column @p images prime at its end, and raises @p refused where
 *  that state stays a frontier whose walks disagree, so the host matches the page itself. */
__device__ inline void regex_per_chunk(sz_regex_engine_t const& engine, matching_chunks_t const& chunks,
                                       std::uint32_t const* images, std::uint32_t cells,
                                       std::uint32_t* refused) noexcept {
    sz_regex_table_simt_t const table = stage_regex(engine, images, cells);
    std::uint32_t const slot = matching_thread_slot();
    if (slot >= chunks.chunks_count) return;
    matching_chunk_t const chunk = chunks.chunks[slot];
    sz_cptr_t const text = chunks.page.start(chunk.value);
    sz_size_t const length = chunks.page.length(chunk.value);
    sz_size_t const begin = chunk.index * chunks.chunk_bytes;
    sz_size_t const end = sz_min_of_two(begin + chunks.chunk_bytes, length);
    sz_regex_haystack_simt_t const haystack = {reinterpret_cast<sz_u8_t const*>(text), length, 0};
    sz_u32_t states[sz_regex_priming_states_simt_k], count = 0, founds = 0;
    sz_u32_t const primed = sz_regex_prime_simt_(&engine, &table, images, &haystack, end, sz_regex_priming_bytes_simt_k,
                                                 states, &count);
    if (primed) states[0] = primed, count = 1;
    for (sz_u32_t index = 0; index != count; ++index) {
        sz_u32_t carried = states[index];
        founds += sz_regex_first_chunk_simt_(&engine, &table, text, length, begin, end, &carried) ? 1u : 0u;
    }
    if (count == 0 || (founds != 0 && founds != count)) atomicOr(refused, 1u);
    else mark_contained(chunks.page.contained, chunk.value, founds != 0 ? sz_true_k : sz_false_k);
}

/** Marks the XML values of @p page a path of @p engine selects in, a lane each walking with
 *  its own stack. */
__device__ inline void subdocuments_per_thread(sz_subdocuments_engine_t const& engine, matching_page_t const& page,
                                               matching_walkers_t const& walkers) noexcept {
    std::uint32_t const slot = matching_thread_slot();
    if (slot >= page.picked_count) return;
    std::uint32_t const value = page.picked[slot];
    sz_size_t emitted = 0;
    sz_status_t const status = sz_subdocuments_walk_markup_simt_(
        &engine, reinterpret_cast<sz_u8_t const*>(page.start(value)), page.length(value), value,
        walkers.stacks + slot * walkers.stack_words, walkers.stack_words / walkers.frame_words, walkers.frame_words,
        walkers.key_bytes, nullptr, 0, &emitted);
    if (status != sz_success_k) atomicOr(walkers.refused, 1u);
    else mark_contained(page.contained, value, emitted != 0 ? sz_true_k : sz_false_k);
}

/** Marks the JSON values of @p page a path of @p engine selects in, a group each walking with its
 *  own stack, in chunks of @p chunk_bytes where it is not zero, so a long value's steps stay
 *  bounded per call. */
__device__ inline void subdocuments_per_group(sz_subdocuments_engine_t const& engine, matching_page_t const& page,
                                              matching_walkers_t const& walkers, std::uint64_t chunk_bytes,
                                              sz_simt_group_t const& group) noexcept {
    std::uint32_t const slot = matching_group_slot();
    if (slot >= page.picked_count) return;
    std::uint32_t const value = page.picked[slot];
    sz_u8_t const* const text = reinterpret_cast<sz_u8_t const*>(page.start(value));
    sz_size_t const length = page.length(value);
    sz_u64_t* const stack = walkers.stacks + slot * walkers.stack_words;
    sz_u8_t* const key = walkers.keys + slot * walkers.key_bytes;
    sz_size_t const frames = walkers.stack_words / walkers.frame_words;
    sz_subdocuments_progress_simt_t carried;
    sz_subdocuments_prime_simt_(&engine, text, length, stack, &carried);
    sz_size_t emitted = 0;
    sz_status_t status = sz_success_k;
    sz_size_t const step = chunk_bytes ? chunk_bytes : length;
    for (sz_size_t end = 0; status == sz_success_k && end != length;) {
        end = length - end > step ? end + step : length;
        status = sz_subdocuments_first_chunk_simt_(&engine, text, length, value, stack, frames, walkers.frame_words,
                                                   key, walkers.key_bytes, nullptr, 0, &emitted, end, &group, &carried);
    }
    if (group.lane != 0) return;
    if (status != sz_success_k) atomicOr(walkers.refused, 1u);
    else mark_contained(page.contained, value, emitted != 0 ? sz_true_k : sz_false_k);
}

#endif

} // namespace unum::ustore::utilities
