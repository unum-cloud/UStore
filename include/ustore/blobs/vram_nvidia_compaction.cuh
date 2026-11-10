/**
 *  @file include/ustore/blobs/vram_nvidia_compaction.cuh
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The device compactor: three kernels per merge, ranking, packing and placing, and the host
 *      half queueing them through the driver.
 *
 *  A merge of sorted runs into new ones takes three launches. The rank kernel runs one thread per
 *  input record, which reads its record out of its run's cells and saved columns, finds its merged
 *  position as its rank in its own run plus a binary search into every other run, and writes it
 *  there with its survival verdict, so nothing sorts and no two runs merge pairwise. The pack
 *  kernel compacts the survivors in one chained scan, answering their count and their value offsets
 *  on the device. The place kernel writes every cell of the new runs, padding or survivor, with its
 *  stamp and its kind. The host waits three times: once to fold what survives where changes are
 *  pending, once for what plans the new runs, and once they are placed.
 *
 *  Every device region a merge needs is carved off the compactor's own device blocks, rewound per
 *  merge, and every host buffer off the executor's pinned bounce. The kernels compile into the
 *  library's bundles and the host half with the host compiler, so nothing links a CUDA runtime.
 */

#pragma once

#include "ustore/blobs/nvme_compaction.hpp"        // `compaction_kind_t`, `compaction_output`
#include "ustore/blobs/vram_nvidia_compaction.hpp" // `cuda_compactor_t`

#include "ustore/utilities/vram_scan.cuh" // `chain_ticket`, `chain_offset`, `block_exclusive_scan`

#if !defined(__CUDACC__) && !defined(__HIP__)
#include "ustore/utilities/vram_device.hpp" // `launch`, `carve_runs`, `rewind`
#endif

namespace unum::ustore::blobs {

/** Threads of every block a merge launches. */
inline constexpr unsigned merge_block_threads_k = 256;

/** One run of a merge as its kernels read it on the device. */
template <typename key_slot_type_>
struct merge_run {

    /** The run's cells on the device, padding included. */
    key_slot_type_ const* cells = nullptr;

    /** The run's value offsets on the device, or null for a run finding its values by stride. */
    stored_value_offset_t const* value_offsets = nullptr;

    /** The run's saved stamps column on the device, or null for stamps of zero. */
    std::uint64_t const* saved_stamps = nullptr;

    /** The run's saved kinds column on the device, or null for writes alone. */
    std::uint64_t const* saved_kinds = nullptr;

    /** The stamp an ingest assigned the run, which its saved stamps read as, or zero for
     *  its own stamps. */
    std::uint64_t assigned_stamp = 0;

    /** Index of the run's first record among the records of every run. */
    std::uint64_t first = 0;

    /** Key slots the run holds, whose cells sit in sorted order. */
    std::uint64_t key_slots_count = 0;
};

/** The C names of the merge kernels the bundles export for @p key_slot_type_, none for a key slot
 *  they carry none of, whose merges run on the host. */
template <typename key_slot_type_>
struct merge_kernel_names {
    static constexpr bool exported_k = false;
};

template <value_layout_t value_layout_>
struct merge_kernel_names<key_slot<st::key128_t, value_layout_>> {
    static constexpr bool exported_k = value_layout_ == value_layout_t::inline_12_bytes_k ||
                                       value_layout_ == value_layout_t::inline_44_bytes_k ||
                                       value_layout_ == value_layout_t::fixed_width_k ||
                                       value_layout_ == value_layout_t::variable_width_k;
    static constexpr char const* rank_k = value_layout_ == value_layout_t::inline_12_bytes_k
                                              ? "ustore_merge_rank_inline_12"
                                          : value_layout_ == value_layout_t::inline_44_bytes_k
                                              ? "ustore_merge_rank_inline_44"
                                          : value_layout_ == value_layout_t::fixed_width_k
                                              ? "ustore_merge_rank_fixed_width"
                                              : "ustore_merge_rank_variable_width";
    static constexpr char const* pack_k = value_layout_ == value_layout_t::inline_12_bytes_k
                                              ? "ustore_merge_pack_inline_12"
                                          : value_layout_ == value_layout_t::inline_44_bytes_k
                                              ? "ustore_merge_pack_inline_44"
                                          : value_layout_ == value_layout_t::fixed_width_k
                                              ? "ustore_merge_pack_fixed_width"
                                              : "ustore_merge_pack_variable_width";
    static constexpr char const* place_k = value_layout_ == value_layout_t::inline_12_bytes_k
                                               ? "ustore_merge_place_inline_12"
                                           : value_layout_ == value_layout_t::inline_44_bytes_k
                                               ? "ustore_merge_place_inline_44"
                                           : value_layout_ == value_layout_t::fixed_width_k
                                               ? "ustore_merge_place_fixed_width"
                                               : "ustore_merge_place_variable_width";
    static constexpr char const* gather_k = value_layout_ == value_layout_t::fixed_width_k
                                                ? "ustore_merge_gather_fixed_width"
                                                : "ustore_merge_gather_variable_width";
};

/** One run of a merge whose cells, its keys alone, come from the leaf rows of its key tree. */
template <typename key_slot_type_>
struct key_rows_gather {

    /** The first leaf row on the device, rows of split high and low words for 16-byte keys. */
    std::uint64_t const* rows = nullptr;

    /** Where the run's cells land on the device. */
    key_slot_type_* cells = nullptr;

    /** Index of the run's first cell among the cells of every gathered run. */
    std::uint64_t first = 0;

    /** Keys the run holds. */
    std::uint64_t count = 0;
};

/** The C name of the kernel that ranks the versions of a merge of strings. */
inline constexpr char const* merge_strings_rank_name_k = "ustore_merge_strings_rank";

/** The value at @p index of a column saved as SmashTable saves a packed array: its count, base and width words, then
 *  the packed words. */
template <typename value_type_>
constexpr value_type_ saved_field(std::uint64_t const* saved, std::uint64_t index) noexcept {
    std::uint64_t const base = saved[1];
    unsigned const bits = static_cast<unsigned>(saved[2]);
    if (bits == 0) return static_cast<value_type_>(base);
    std::uint64_t const* const words = saved + 3;
    std::uint64_t const first = index * bits;
    std::uint64_t const word = first / 64;
    unsigned const shift = static_cast<unsigned>(first % 64);
    std::uint64_t field = words[word] >> shift;
    if (shift + bits > 64) field |= words[word + 1] << (64 - shift);
    std::uint64_t const mask = bits == 64 ? ~std::uint64_t {0} : (std::uint64_t {1} << bits) - 1;
    return static_cast<value_type_>(base + (field & mask));
}

#pragma region Strings Ranking

/** One run of strings as the rank kernel reads it: a head per key, whole keys, saved columns. */
struct strings_merge_run {

    /** The head of every key in sorted order, which consecutive threads load side by side. */
    st::key128_t const* heads = nullptr;

    /** One offset per key into @c keys and one past the last. */
    std::uint32_t const* key_offsets = nullptr;

    /** Every key whole, back to back in sorted order. */
    byte_t const* keys = nullptr;

    /** The run's saved stamps column, as SmashTable saves a packed array. */
    std::uint64_t const* saved_stamps = nullptr;

    /** The run's saved kinds column, as SmashTable saves a packed array. */
    std::uint64_t const* saved_kinds = nullptr;

    /** Index of the run's first record among the records of every run. */
    std::uint64_t first = 0;

    /** Keys the run holds. */
    std::uint64_t count = 0;
};

/** The key slot a merge of strings judges survival on, whose stored key is a head. */
using strings_merge_slot_t = key_slot<st::key128_t, value_layout_t::uuid_k, key_tails_t::carried_k>;

/** Where a version lands in a merge of strings, and whether it survives. */
struct strings_merge_verdict_t {

    /** Its position in merged order. */
    std::uint64_t rank = 0;

    /** Whether it survives, as @c survives decides beside the version before it. */
    bool survives = false;
};

/** Orders the key at @p first_position of @p first against the one at @p second_position of
 *  @p second: by their heads, and on a tie by the bytes past them on the tapes, as
 *  @c union_sorted_keys orders keys. */
constexpr int strings_merge_order(strings_merge_run const& first, std::uint64_t first_position,
                                  strings_merge_run const& second, std::uint64_t second_position) noexcept {
    st::key128_t const first_head = first.heads[first_position];
    st::key128_t const second_head = second.heads[second_position];
    if (first_head < second_head) return -1;
    if (second_head < first_head) return 1;
    // Equal heads share their leading bytes and a length class, so only the bytes past them differ.
    constexpr std::uint64_t head_bytes_k = sizeof(st::key128_t) - 1;
    std::uint64_t first_offset = first.key_offsets[first_position];
    std::uint64_t const first_end = first.key_offsets[first_position + 1];
    std::uint64_t second_offset = second.key_offsets[second_position];
    std::uint64_t const second_end = second.key_offsets[second_position + 1];
    first_offset += first_end - first_offset < head_bytes_k ? first_end - first_offset : head_bytes_k;
    second_offset += second_end - second_offset < head_bytes_k ? second_end - second_offset : head_bytes_k;
    for (; first_offset != first_end && second_offset != second_end; ++first_offset, ++second_offset) {
        auto const first_byte = static_cast<unsigned char>(first.keys[first_offset]);
        auto const second_byte = static_cast<unsigned char>(second.keys[second_offset]);
        if (first_byte != second_byte) return first_byte < second_byte ? -1 : 1;
    }
    return first_offset != first_end ? 1 : second_offset != second_end ? -1 : 0;
}

/** The version at @p position of @p run as @c survives reads it: its head, run, kind and stamp. */
constexpr located_key_slot<strings_merge_slot_t> strings_merge_record(strings_merge_run const& run, std::uint32_t index,
                                                                      std::uint64_t position) noexcept {
    located_key_slot<strings_merge_slot_t> record {};
    record.key_slot.stored_key = run.heads[position];
    record.sorted_run = index;
    record.position = static_cast<std::uint32_t>(position);
    if (run.saved_stamps != nullptr) record.stamp = saved_field<std::uint64_t>(run.saved_stamps, position);
    if (run.saved_kinds != nullptr)
        record.kind = static_cast<delta_kind_t>(saved_field<std::uint8_t>(run.saved_kinds, position));
    return record;
}

/**
 *  @brief Where the version at @p position of run @p run lands in the merge of @p runs, and
 *      whether it survives.
 *
 *  Its merged position is its own position plus, for every other run, how many of that run's
 *  versions order before it: the keys at or below its key for a run whose versions of a key come
 *  first, newer upper runs ahead of older ones and every upper run ahead of the lower ones, and the
 *  keys below it otherwise. The last of those of its key is the version before it, beside which it
 *  survives as @c survives decides, so the order is the host's.
 */
constexpr strings_merge_verdict_t strings_merge_verdict(strings_merge_run const* runs, std::uint32_t runs_count,
                                                        std::uint32_t upper_runs, std::uint32_t run,
                                                        std::uint64_t position, compaction_kind_t kind,
                                                        compaction_stamps_t const& stamps) noexcept {
    auto const comes_first = [upper_runs](std::uint32_t first_run, std::uint32_t second_run) noexcept {
        bool const both_upper = (first_run < upper_runs) & (second_run < upper_runs);
        return both_upper ? first_run > second_run : first_run < second_run;
    };
    strings_merge_run const& own = runs[run];
    std::uint64_t rank = position;
    std::uint32_t previous_run = run;
    std::uint64_t previous_position = position;
    bool preceded = position != 0 && strings_merge_order(own, position - 1, own, position) == 0;
    if (preceded) previous_position = position - 1;
    for (std::uint32_t other = 0; other != runs_count; ++other) {
        if (other == run) continue;
        strings_merge_run const& theirs = runs[other];
        bool const ahead = comes_first(other, run);
        std::uint64_t before = 0;
        for (std::uint64_t count = theirs.count; count;) {
            std::uint64_t const half = count / 2;
            int const order = strings_merge_order(theirs, before + half, own, position);
            if (order < 0 || (order == 0 && ahead)) before += half + 1, count -= half + 1;
            else count = half;
        }
        rank += before;
        if (before == 0 || !ahead || strings_merge_order(theirs, before - 1, own, position) != 0) continue;
        // Of the versions of this key ordered before it, the latest is in the run that comes last.
        if (!preceded || (previous_run != run && comes_first(previous_run, other)))
            previous_run = other, previous_position = before - 1, preceded = true;
    }
    located_key_slot<strings_merge_slot_t> const record = strings_merge_record(own, run, position);
    located_key_slot<strings_merge_slot_t> const previous = strings_merge_record(runs[previous_run], previous_run,
                                                                                 previous_position);
    return {rank, survives(preceded ? &previous : nullptr, record, kind, stamps)};
}

#pragma endregion Strings Ranking

#if defined(__CUDACC__) || defined(__HIP__)

#pragma region Kernels

/** The record at sorted @p position of @p run, the run's index being @p index, with its stamp
 *  and kind unpacked. */
template <typename key_slot_type_>
__device__ located_key_slot<key_slot_type_> merge_record(merge_run<key_slot_type_> const& run, std::uint32_t index,
                                                         std::uint64_t value_stride, std::uint64_t position) noexcept {
    located_key_slot<key_slot_type_> record {run.cells[position],
                                             index,
                                             delta_kind_t::write_k,
                                             0,
                                             static_cast<std::uint32_t>(position),
                                             stored_value_bytes(run.value_offsets, value_stride, position)};
    if (run.saved_stamps != nullptr)
        record.stamp = run.assigned_stamp != 0 ? run.assigned_stamp
                                               : saved_field<std::uint64_t>(run.saved_stamps, position);
    if (run.saved_kinds != nullptr)
        record.kind = static_cast<delta_kind_t>(saved_field<std::uint8_t>(run.saved_kinds, position));
    return record;
}

/**
 *  @brief Writes every record of @p runs at its merged position of @p merged, with its survival
 *      flag in @p flags.
 *
 *  A record's merged position is its sorted position in its own run plus, for every other run, how
 *  many of that run's records order before it, one binary search each; the run order of equal
 *  stored keys makes those counts exact. The record before it in merged order is the last of those,
 *  which decides its survival as @c survives does.
 */
template <typename key_slot_type_>
__device__ void merge_rank(merge_run<key_slot_type_> const* runs, std::uint32_t runs_count, std::uint32_t upper_runs,
                           std::uint64_t value_stride, std::uint64_t records_count, compaction_kind_t kind,
                           compaction_stamps_t stamps, located_key_slot<key_slot_type_>* merged,
                           std::uint8_t* flags) noexcept {
    using record_t = located_key_slot<key_slot_type_>;
    survivor_first_t const order {upper_runs};
    std::uint64_t const stride = std::uint64_t {gridDim.x} * blockDim.x;
    for (std::uint64_t index = std::uint64_t {blockIdx.x} * blockDim.x + threadIdx.x; index < records_count;
         index += stride) {
        std::uint32_t run = 0;
        for (std::uint32_t high = runs_count; high - run > 1;) {
            std::uint32_t const middle = run + (high - run) / 2;
            if (runs[middle].first <= index) run = middle;
            else high = middle;
        }
        std::uint64_t const position = index - runs[run].first;
        record_t const record = merge_record(runs[run], run, value_stride, position);
        std::uint64_t rank = position;
        record_t previous {};
        bool preceded = false;
        if (position != 0) {
            previous = merge_record(runs[run], run, value_stride, position - 1);
            preceded = same_key(previous, record);
        }
        for (std::uint32_t other = 0; other != runs_count; ++other) {
            if (other == run) continue;
            merge_run<key_slot_type_> const& theirs = runs[other];
            std::uint64_t before = 0;
            for (std::uint64_t count = theirs.key_slots_count; count;) {
                std::uint64_t const half = count / 2;
                record_t probe {};
                probe.key_slot = theirs.cells[before + half];
                probe.sorted_run = other;
                if (order(probe, record)) before += half + 1, count -= half + 1;
                else count = half;
            }
            rank += before;
            if (before == 0) continue;
            record_t const candidate = merge_record(theirs, other, value_stride, before - 1);
            if (same_key(candidate, record) && (!preceded || order(previous, candidate)))
                previous = candidate, preceded = true;
        }
        merged[rank] = record;
        flags[rank] = survives(preceded ? &previous : nullptr, record, kind, stamps) ? 1 : 0;
    }
}

/** The record at merged @p index, or the folded one replacing it, @p replaced listing
 *  their indices ascending. */
template <typename key_slot_type_>
__device__ located_key_slot<key_slot_type_> merge_packed(located_key_slot<key_slot_type_> const* merged,
                                                         std::uint64_t index, std::uint64_t const* replaced,
                                                         located_key_slot<key_slot_type_> const* replacements,
                                                         std::uint64_t replaced_count) noexcept {
    std::uint64_t low = 0;
    for (std::uint64_t count = replaced_count; count;) {
        std::uint64_t const half = count / 2;
        if (replaced[low + half] < index) low += half + 1, count -= half + 1;
        else count = half;
    }
    return low != replaced_count && replaced[low] == index ? replacements[low] : merged[index];
}

/**
 *  @brief Compacts the flagged records of @p merged into @p survivors in one chained scan, writing
 *      their value offsets into @p prefix, where it is not null, and whether each carries the key
 *      of the survivor before it into @p shares.
 *
 *  Block @c t of @p tickets packs the @c t th tile of the records: it counts its survivors and
 *  their bytes, takes the totals of the tiles before it from @p chains, the survivors' and the
 *  bytes' chain, and then writes. A survivor shares its key when a flagged record of the same key
 *  precedes it, which a walk back over that key's versions finds.
 */
template <typename key_slot_type_, unsigned block_threads_>
__device__ void merge_pack(located_key_slot<key_slot_type_> const* merged, std::uint8_t const* flags,
                           std::uint64_t records_count, std::uint64_t const* replaced,
                           located_key_slot<key_slot_type_> const* replacements, std::uint64_t replaced_count,
                           std::uint64_t tickets, located_key_slot<key_slot_type_>* survivors, std::uint64_t* prefix,
                           std::uint8_t* shares, std::uint64_t* chains) noexcept {
    using record_t = located_key_slot<key_slot_type_>;
    using utilities::block_exclusive_scan;
    using utilities::block_scanned_t;
    std::uint64_t const ticket = utilities::chain_ticket(chains);
    std::uint64_t const tile = utilities::divide_round_up(records_count, tickets);
    std::uint64_t const first = ticket * tile < records_count ? ticket * tile : records_count;
    std::uint64_t const end = first + tile < records_count ? first + tile : records_count;

    std::uint64_t kept = 0, bytes = 0;
    for (std::uint64_t index = first + threadIdx.x; index < end; index += block_threads_)
        if (flags[index]) ++kept, bytes += merge_packed(merged, index, replaced, replacements, replaced_count).size;
    std::uint64_t const kept_total = block_exclusive_scan<block_threads_>(kept).total;
    std::uint64_t const bytes_total = block_exclusive_scan<block_threads_>(bytes).total;
    std::uint64_t kept_before = utilities::chain_offset(chains, ticket, tickets, kept_total);
    std::uint64_t bytes_before = utilities::chain_offset(chains + 1, ticket, tickets, bytes_total);
    if (ticket == 0 && threadIdx.x == 0 && prefix != nullptr) prefix[0] = 0;

    for (std::uint64_t base = first; base < end; base += block_threads_) {
        std::uint64_t const index = base + threadIdx.x;
        bool const flagged = index < end && flags[index] != 0;
        record_t const record = flagged ? merge_packed(merged, index, replaced, replacements, replaced_count)
                                        : record_t {};
        std::uint64_t const size = flagged ? record.size : 0;
        block_scanned_t const kept_scan = block_exclusive_scan<block_threads_>(flagged ? 1 : 0);
        block_scanned_t const bytes_scan = block_exclusive_scan<block_threads_>(size);
        if (flagged) {
            std::uint64_t const into = kept_before + kept_scan.before;
            survivors[into] = record;
            if (prefix != nullptr) prefix[into + 1] = bytes_before + bytes_scan.before + size;
            bool shared = false;
            for (std::uint64_t older = index; older-- != 0 && same_key(merged[older], merged[index]);)
                if (flags[older]) {
                    shared = true;
                    break;
                }
            shares[into] = shared ? 1 : 0;
        }
        kept_before += kept_scan.total, bytes_before += bytes_scan.total;
    }
}

/** Writes the cell of every one of @p survivors_count survivors, which the new runs hold in order,
 *  with its stamp and its kind. */
template <typename key_slot_type_>
__device__ void merge_place(located_key_slot<key_slot_type_> const* survivors, std::uint64_t survivors_count,
                            std::uint64_t floor, key_slot_type_* cells, std::uint64_t* stamps,
                            std::uint8_t* kinds) noexcept {
    std::uint64_t const stride = std::uint64_t {gridDim.x} * blockDim.x;
    for (std::uint64_t survivor = std::uint64_t {blockIdx.x} * blockDim.x + threadIdx.x; survivor < survivors_count;
         survivor += stride)
        place_key_slot(survivors[survivor], floor, cells[survivor], stamps[survivor], kinds[survivor]);
}

/** Writes the cell of every key of every run @p gathers names, read from the leaf rows of its key
 *  tree, where consecutive threads read consecutive words of a row. */
template <typename key_slot_type_>
__device__ void merge_gather(key_rows_gather<key_slot_type_> const* gathers, std::uint32_t gathers_count,
                             std::uint64_t cells_count) noexcept {
    using format_t = typename st::immutable_splus_tree_view<typename key_slot_type_::stored_key_t>::format_t;
    std::uint64_t const stride = std::uint64_t {gridDim.x} * blockDim.x;
    for (std::uint64_t index = std::uint64_t {blockIdx.x} * blockDim.x + threadIdx.x; index < cells_count;
         index += stride) {
        std::uint32_t gather = 0;
        for (std::uint32_t high = gathers_count; high - gather > 1;) {
            std::uint32_t const middle = gather + (high - gather) / 2;
            if (gathers[middle].first <= index) gather = middle;
            else high = middle;
        }
        key_rows_gather<key_slot_type_> const& run = gathers[gather];
        std::uint64_t const position = index - run.first;
        key_slot_type_ cell {};
        cell.stored_key = format_t::key_at(reinterpret_cast<typename format_t::word_t const*>(run.rows) +
                                               position / format_t::keys_per_row_k * format_t::words_per_row_k,
                                           static_cast<std::size_t>(position % format_t::keys_per_row_k));
        run.cells[position] = cell;
    }
}

/** Writes each version of the merge of @p runs of strings at its merged position of @p merged, with
 *  its survival flag in @p flags, one thread per version, as @c strings_merge_verdict places it. */
__device__ inline void merge_strings_rank(strings_merge_run const* runs, std::uint32_t runs_count,
                                          std::uint32_t upper_runs, std::uint64_t records_count, compaction_kind_t kind,
                                          compaction_stamps_t stamps, strings_version_t* merged,
                                          std::uint8_t* flags) noexcept {
    std::uint64_t const stride = std::uint64_t {gridDim.x} * blockDim.x;
    for (std::uint64_t index = std::uint64_t {blockIdx.x} * blockDim.x + threadIdx.x; index < records_count;
         index += stride) {
        std::uint32_t run = 0;
        for (std::uint32_t high = runs_count; high - run > 1;) {
            std::uint32_t const middle = run + (high - run) / 2;
            if (runs[middle].first <= index) run = middle;
            else high = middle;
        }
        std::uint64_t const position = index - runs[run].first;
        strings_merge_verdict_t const verdict = strings_merge_verdict(runs, runs_count, upper_runs, run, position, kind,
                                                                      stamps);
        merged[verdict.rank] = strings_version_t {run, static_cast<std::uint32_t>(position)};
        flags[verdict.rank] = verdict.survives ? 1 : 0;
    }
}

#pragma endregion Kernels

#else

#pragma region Sections

using utilities::cuda_executor_t;

/** A failure of the executor as a compaction's status. */
inline st::status_t executor_status(status_t status) noexcept {
    return status.failed() ? st::status_t::unknown_k : st::status_t::success_k;
}

/** Carves @p count elements off @p executor's bounce for one merge, which the
 *  next merge rewinds. */
template <typename element_type_>
element_type_* carve_bounce(cuda_executor_t& executor, std::size_t count) noexcept {
    auto [carved, status] = utilities::arena_allocate<element_type_>(executor.bounce(), count);
    return status.failed() ? nullptr : carved.data();
}

/** Queues uploading @p count elements from host @p source to device @p target on @p executor. */
template <typename element_type_>
st::status_t upload(cuda_executor_t& executor, element_type_* target, element_type_ const* source,
                    std::size_t count) noexcept {
    if (count == 0) return st::status_t::success_k;
    std::span<element_type_ const> const host {source, count};
    return executor_status(executor.upload_async(reinterpret_cast<std::uintptr_t>(target), std::as_bytes(host)));
}

/** Queues downloading @p count elements from device @p source to host @p target on @p executor. */
template <typename element_type_>
st::status_t download(cuda_executor_t& executor, element_type_* target, element_type_ const* source,
                      std::size_t count) noexcept {
    if (count == 0) return st::status_t::success_k;
    std::span<element_type_> const host {target, count};
    return executor_status(
        executor.download_async(std::as_writable_bytes(host), reinterpret_cast<std::uintptr_t>(source)));
}

/** Queues kernel @p name of @p executor's bundle over @p items items, one thread each, as
 *  many blocks of @c merge_block_threads_k as cover them up to @p blocks_limit. */
template <typename... argument_types_>
st::status_t launch_over(cuda_executor_t& executor, char const* name, std::uint64_t items, std::uint64_t blocks_limit,
                         argument_types_... arguments) noexcept {
    std::uint64_t const blocks = fu::min_of(divide_round_up(items, std::uint64_t {merge_block_threads_k}),
                                            blocks_limit);
    return executor_status(utilities::launch(executor, executor.kernels()->function(name),
                                             static_cast<unsigned>(blocks), merge_block_threads_k, 0u, arguments...));
}

/** Bumps aligned regions off one address, which a merge runs once from zero to size its sections,
 *  then again from the region it carved to place them. */
struct scratch_cursor_t {

    /** The next free address. */
    std::uintptr_t at = 0;

    /** Takes room for @p count elements at @p alignment, null for none. */
    template <typename element_type_>
    element_type_* take(std::size_t count, std::size_t alignment = 256) noexcept {
        if (count == 0) return nullptr;
        at = round_up_to_multiple<std::uintptr_t>(at, alignment);
        element_type_* const taken = reinterpret_cast<element_type_*>(at);
        at += count * sizeof(element_type_);
        return taken;
    }
};

/** Where one run's sections come from: its volume by extent, or uploads from the memory
 *  holding its index. */
enum class sections_source_t { volume_k, memory_k };

/** Where one run's sections land in a merge's device memory, and where they come from. */
template <typename key_slot_type_>
struct run_sections {

    /** The cells, whole rows including padding. */
    key_slot_type_* cells = nullptr;

    /** The value offsets, null for a run finding its values by stride. */
    stored_value_offset_t* value_offsets = nullptr;

    /** The trailer of a run reading from its volume, its saved stamps first. */
    std::uint64_t* trailer = nullptr;

    /** The key tree of a run of keys alone read from its volume, gathered into @c cells. */
    std::uint64_t* keys = nullptr;

    sections_source_t source = sections_source_t::memory_k;
};

/** Carves the sections of the run @p view describes off @p cursor, page aligned by @p extents when
 *  @p preferred is the volume and the run lives on one, and as its memory holds them otherwise. */
template <typename key_slot_type_>
run_sections<key_slot_type_> carve_sections(sorted_run_index_view<key_slot_type_> const& view,
                                            sorted_run_extents_t const& extents, sections_source_t preferred,
                                            scratch_cursor_t& cursor) noexcept {
    run_sections<key_slot_type_> sections;
    bool const gathers = extents.cells.empty() && !extents.keys.empty();
    if (preferred == sections_source_t::volume_k && (!extents.cells.empty() || gathers) && !extents.trailer.empty())
        sections.source = sections_source_t::volume_k;
    std::uint64_t const cells_count = view.key_slots_count;
    std::size_t const page = static_cast<std::size_t>(utilities::portable_alignment_k);
    if (sections.source == sections_source_t::volume_k) {
        if (gathers) {
            sections.keys = cursor.take<std::uint64_t>(
                static_cast<std::size_t>(extents.keys.length / sizeof(std::uint64_t) + 1), page);
            sections.cells = cursor.take<key_slot_type_>(static_cast<std::size_t>(cells_count));
        }
        else
            sections.cells = cursor.take<key_slot_type_>(
                static_cast<std::size_t>(extents.cells.length / sizeof(key_slot_type_) + 1), page);
        if (!view.value_offsets.empty())
            sections.value_offsets = cursor.take<stored_value_offset_t>(
                static_cast<std::size_t>(extents.value_offsets.length / sizeof(stored_value_offset_t) + 1), page);
        sections.trailer = cursor.take<std::uint64_t>(
            static_cast<std::size_t>(extents.trailer.length / sizeof(std::uint64_t) + 1), page);
        return sections;
    }
    sections.cells = cursor.take<key_slot_type_>(static_cast<std::size_t>(cells_count));
    sections.value_offsets = cursor.take<stored_value_offset_t>(view.value_offsets.size());
    return sections;
}

/** Words of the saved stamps and kinds of the run @p view describes when it uploads from memory, as SmashTable saves
 *  them, a multiple of eight bytes each. */
template <typename key_slot_type_>
constexpr std::size_t saved_words(sorted_run_index_view<key_slot_type_> const& view) noexcept {
    std::size_t words = 0;
    if (view.key_slots_count == 0) return words;
    if (view.stamps != nullptr) words += view.stamps->saved_bytes() / sizeof(std::uint64_t);
    if (view.kinds != nullptr) words += view.kinds->saved_bytes() / sizeof(std::uint64_t);
    return words;
}

#pragma endregion Sections

#pragma region Merge

template <typename element_type_>
element_type_* cuda_compactor_t::carve_(std::size_t count) noexcept {
    std::uint32_t const held = blocks_.count;
    expected<std::span<element_type_>> const carved = utilities::carve_runs<element_type_>(
        blocks_, *executor_, count, [&](std::size_t bytes) noexcept { return executor_->allocate_async(bytes); });
    device_allocations_ += blocks_.count - held;
    return carved ? carved->data() : nullptr;
}

/**
 *  @brief Folds what the survivors of a merge fold, as @c host_compactor_t::compact does, writing
 *      their values into @p output's folded bytes and listing each folded record's merged index,
 *      ascending, and its replacement in @p indices and @p replacements.
 *
 *  The pack kernel takes those in place of the merged records.
 *
 *  The merged records and their survival flags come back to the host, which finds the versions behind every surviving
 *  change through @c folded_versions, as the host compactor does. A surviving change whose older versions are all
 *  changes, and which the bottom level does not apply onto its empty base, becomes one chain that @p device_fold
 *  composes with every other such chain at once. Every other chain folds on the host through @c fold_versions.
 */
template <typename key_slot_type_, typename fold_type_, typename device_fold_type_, typename allocator_type_>
st::status_t fold_survivors(cuda_executor_t& executor, utilities::device_blocks_t& blocks, compaction_kind_t kind,
                            compaction_stamps_t const& stamps, std::uint64_t sorted_runs_count,
                            std::uint64_t value_stride, located_key_slot<key_slot_type_> const* merged,
                            std::uint8_t const* flags, std::uint64_t merged_count, fold_type_ const& fold,
                            device_fold_type_ const& device_fold, located_value_reader<key_slot_type_> read_value,
                            compaction_output<key_slot_type_, allocator_type_>& output,
                            std::span<std::uint64_t>& indices,
                            std::span<located_key_slot<key_slot_type_>>& replacements) noexcept {
    using st::status_t;
    using st::vector;
    using record_t = located_key_slot<key_slot_type_>;

    record_t* const records = carve_bounce<record_t>(executor, static_cast<std::size_t>(merged_count));
    std::uint8_t* const kept = carve_bounce<std::uint8_t>(executor, static_cast<std::size_t>(merged_count));
    if (!records || !kept || executor.pin_bounce().failed()) return status_t::out_of_memory_heap_k;
    status_t status = download(executor, records, merged, static_cast<std::size_t>(merged_count));
    if (status != status_t::success_k) return status;
    status = download(executor, kept, flags, static_cast<std::size_t>(merged_count));
    if (status != status_t::success_k) return status;
    status = executor_status(executor.wait());
    if (status != status_t::success_k) return status;

    utilities::local_arena_t arena;
    utilities::arena_allocator<byte_t> arena_bytes = arena.allocator();
    auto const value_of = [&](record_t const& record, std::span<byte_t const>& bytes) noexcept {
        bytes = {};
        if (record.kind == delta_kind_t::remove_k) return status_t::success_k;
        return read_value(record, arena_bytes, bytes);
    };

    // A folded survivor: its value folded here, or the chain the device composes it from
    struct folded_t {
        std::uint64_t merged = 0;
        delta_kind_t kind = delta_kind_t::change_k;
        std::span<byte_t const> bytes;
        std::uint64_t chain = (std::numeric_limits<std::uint64_t>::max)();
    };
    vector<folded_t> folds;
    vector<delta_view_t> changes;
    vector<std::uint64_t> chain_starts;
    status = chain_starts.push_back(std::uint64_t {0});
    if (status != status_t::success_k) return status;

    for (std::uint64_t head = 0; head != merged_count; ++head) {
        if (!kept[head] || records[head].kind != delta_kind_t::change_k) continue;
        folded_versions_t const versions = folded_versions(kind, stamps, records, kept, merged_count, head);
        if (versions.end == head + 1 && !versions.applies) continue;
        bool only_changes = true;
        for (std::uint64_t older = head + 1; older != versions.end; ++older)
            only_changes &= records[older].kind == delta_kind_t::change_k;

        if (only_changes && !versions.applies) {
            for (std::uint64_t version = versions.end; version != head; --version) {
                std::span<byte_t const> bytes;
                if ((status = value_of(records[version - 1], bytes)) != status_t::success_k) return status;
                if ((status = changes.push_back(delta_view_t {delta_kind_t::change_k, bytes})) != status_t::success_k)
                    return status;
            }
            if ((status = chain_starts.push_back(std::uint64_t {changes.size()})) != status_t::success_k) return status;
            status = folds.push_back(folded_t {head, delta_kind_t::change_k, {}, chain_starts.size() - 2});
            if (status != status_t::success_k) return status;
            continue;
        }

        delta_view_t folded;
        status = fold_versions(fold, records, head, versions, value_of, arena_bytes, folded);
        if (status != status_t::success_k) return status;
        status = folds.push_back(folded_t {head, folded.kind, folded.bytes});
        if (status != status_t::success_k) return status;
    }

    vector<byte_t> composed;
    vector<std::uint64_t> composed_offsets;
    if (chain_starts.size() > 1) {
        status = device_fold.compose(executor, blocks, changes, chain_starts, composed, composed_offsets);
        if (status != status_t::success_k) return status;
    }

    // A folded value replaces its survivor's own: inline in the key slot, or in the compaction's folded bytes
    std::uint64_t* const folded_indices = carve_bounce<std::uint64_t>(executor, folds.size());
    record_t* const folded_records = carve_bounce<record_t>(executor, folds.size());
    if (!folds.empty() && (!folded_indices || !folded_records || executor.pin_bounce().failed()))
        return status_t::out_of_memory_heap_k;
    indices = {folded_indices, folds.size()};
    replacements = {folded_records, folds.size()};
    for (std::size_t index = 0; index != folds.size(); ++index) {
        folded_t const& folded = folds[index];
        std::span<byte_t const> const bytes =
            folded.chain == (std::numeric_limits<std::uint64_t>::max)()
                ? folded.bytes
                : std::span<byte_t const> {composed.data() + composed_offsets[folded.chain],
                                           composed_offsets[folded.chain + 1] - composed_offsets[folded.chain]};
        record_t record = records[folded.merged];
        record.kind = folded.kind;
        status = place_folded_value(bytes, value_stride, static_cast<std::uint32_t>(sorted_runs_count), record,
                                    output.folded);
        if (status != status_t::success_k) return status;
        indices[index] = folded.merged;
        replacements[index] = record;
    }
    return status_t::success_k;
}

template <typename key_slot_type_, utilities::fold_concept fold_type_, typename device_fold_type_,
          typename allocator_type_>
    requires compactable_key_slot_concept<key_slot_type_>
st::status_t cuda_compactor_t::compact(
    compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_type_> const> upper,
    std::span<sorted_run_index_view<key_slot_type_> const> lower, std::span<sorted_run_extents_t const> extents,
    sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps, [[maybe_unused]] fold_type_ const& fold,
    [[maybe_unused]] device_fold_type_ const& device_fold,
    [[maybe_unused]] std::type_identity_t<located_value_reader<key_slot_type_>> read_value,
    compaction_output<key_slot_type_, allocator_type_>& output) noexcept {
    using st::status_t;
    using record_t = located_key_slot<key_slot_type_>;
    using view_t = sorted_run_index_view<key_slot_type_>;
    using names_t = merge_kernel_names<key_slot_type_>;
    constexpr bool device_folds_k = !std::is_same_v<device_fold_type_, no_device_fold_t>;
    constexpr bool locates_k = locates_stored_values_concept<key_slot_type_>;
    constexpr st::assume_uninitialized_t uninitialized {};

    // A key slot the bundles carry no kernels for merges on the host, as does a device whose bundle
    // did not load
    if constexpr (!names_t::exported_k) return status_t::invalid_argument_k;
    else {
        if (!executor_->kernels()) return status_t::invalid_argument_k;
        if (!admits_sorted_runs(bounds)) return status_t::invalid_argument_k;
        std::uint64_t const sorted_runs_count = upper.size() + lower.size();
        if (sorted_runs_count > (std::numeric_limits<std::uint32_t>::max)() || extents.size() != sorted_runs_count)
            return status_t::invalid_argument_k;
        if (!views_locate_values(upper, bounds) || !views_locate_values(lower, bounds))
            return status_t::invalid_argument_k;
        auto const view_of = [&](std::uint64_t run) noexcept -> view_t const& {
            return run < upper.size() ? upper[run] : lower[run - upper.size()];
        };

        std::uint64_t merged_count = 0;
        std::size_t saved_count = 0;
        bool changes = false;
        sections_source_t const preferred = sections_ ? sections_source_t::volume_k : sections_source_t::memory_k;
        scratch_cursor_t sizing;
        for (std::uint64_t run = 0; run != sorted_runs_count; ++run) {
            view_t const& view = view_of(run);
            changes |= holds_changes(view.kinds);
            merged_count += view.key_slots_count;
            if (carve_sections(view, extents[run], preferred, sizing).source == sections_source_t::memory_k)
                saved_count += saved_words(view);
        }
        // Without a device fold, changes fold on the host alone, where every merge
        // holding one runs.
        if (!device_folds_k && changes) return status_t::invalid_argument_k;
        output.folded.clear();

        if (executor_->bind().failed()) return status_t::unknown_k;
        executor_->rewind_bounce();
        // The reader registered the newest run, which a rewind keeps, unless a merge opened more
        // after its sections
        if (blocks_.count > 1) sections_.release();
        utilities::rewind(blocks_, *executor_);

        // The sections land first, in one region, so the reader registers the run holding it and
        // reads into it
        std::size_t const page = static_cast<std::size_t>(utilities::portable_alignment_k);
        byte_t* const region = sizing.at != 0 ? carve_<byte_t>(static_cast<std::size_t>(sizing.at) + page) : nullptr;
        if (sizing.at != 0 && !region) return status_t::out_of_memory_heap_k;
        std::uintptr_t const sections_base = round_up_to_multiple<std::uintptr_t>(
            reinterpret_cast<std::uintptr_t>(region), page);
        std::uintptr_t const holding_address = blocks_.count ? blocks_.blocks[blocks_.count - 1].address : 0;
        std::size_t const holding_bytes = blocks_.count ? blocks_.blocks[blocks_.count - 1].capacity : 0;

        std::size_t const records_count = static_cast<std::size_t>(merged_count);
        merge_run<key_slot_type_>* const runs = carve_<merge_run<key_slot_type_>>(sorted_runs_count);
        std::uint64_t* const saved_columns = carve_<std::uint64_t>(saved_count);
        std::uint64_t* const pinned = carve_<std::uint64_t>(static_cast<std::size_t>(stamps.pinned_count));
        record_t* const merged = carve_<record_t>(records_count);
        std::uint8_t* const flags = carve_<std::uint8_t>(records_count);
        record_t* const survivors = carve_<record_t>(records_count);
        std::uint64_t* const prefix = locates_k ? carve_<std::uint64_t>(records_count + 1) : nullptr;
        std::uint8_t* const shares = carve_<std::uint8_t>(records_count);
        std::uint64_t* const chains = carve_<std::uint64_t>(2);
        key_rows_gather<key_slot_type_>* const gathers = carve_<key_rows_gather<key_slot_type_>>(sorted_runs_count);
        if ((sorted_runs_count && (!runs || !gathers)) || (saved_count && !saved_columns) ||
            (stamps.pinned_count && !pinned) ||
            (records_count && (!merged || !flags || !survivors || !shares || (locates_k && !prefix))) || !chains)
            return status_t::out_of_memory_heap_k;

        merge_run<key_slot_type_>* const host_runs = carve_bounce<merge_run<key_slot_type_>>(*executor_,
                                                                                             sorted_runs_count);
        std::uint64_t* const saved = carve_bounce<std::uint64_t>(*executor_, saved_count);
        key_rows_gather<key_slot_type_>* const host_gathers = carve_bounce<key_rows_gather<key_slot_type_>>(
            *executor_, sorted_runs_count);
        if ((sorted_runs_count && (!host_runs || !host_gathers)) || (saved_count && !saved) ||
            executor_->pin_bounce().failed())
            return status_t::out_of_memory_heap_k;
        std::uint32_t gathers_count = 0;
        std::uint64_t gathered_cells = 0;

        // Every run's sections land where the sizing pass placed them, and its record reads them
        // through @c runs
        reads_.clear();
        scratch_cursor_t placing {sections_base};
        std::size_t saved_offset = 0;
        std::uint64_t first = 0;
        status_t status = status_t::success_k;
        for (std::uint64_t run = 0; run != sorted_runs_count; ++run) {
            view_t const& view = view_of(run);
            sorted_run_extents_t const& run_extents = extents[run];
            run_sections<key_slot_type_> const sections = carve_sections(view, run_extents, preferred, placing);
            merge_run<key_slot_type_>& described = host_runs[run];
            described = merge_run<key_slot_type_> {};
            described.cells = sections.cells, described.value_offsets = sections.value_offsets;
            described.assigned_stamp = run_extents.assigned_stamp, described.first = first;
            described.key_slots_count = view.key_slots_count;
            first += view.key_slots_count;
            if (sections.source == sections_source_t::volume_k) {
                auto const into = [&](void const* target) noexcept {
                    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(target) - holding_address);
                };
                for (device_section_read_t const read :
                     {device_section_read_t {run_extents.volume, run_extents.cells, into(sections.cells),
                                             run_extents.run},
                      device_section_read_t {run_extents.volume, run_extents.value_offsets,
                                             into(sections.value_offsets), run_extents.run},
                      device_section_read_t {run_extents.volume, run_extents.trailer, into(sections.trailer),
                                             run_extents.run},
                      device_section_read_t {run_extents.volume,
                                             sections.keys ? run_extents.keys : utilities::extent_t {},
                                             sections.keys ? into(sections.keys) : 0, run_extents.run}})
                    if (!read.extent.empty())
                        if (reads_.push_back(device_section_read_t {read}) != status_t::success_k)
                            return status_t::out_of_memory_heap_k;
                described.saved_stamps = sections.trailer;
                described.saved_kinds = sections.trailer + run_extents.kinds_begin / sizeof(std::uint64_t);
                if (sections.keys != nullptr) {
                    host_gathers[gathers_count++] = key_rows_gather<key_slot_type_> {
                        sections.keys + run_extents.leaves_begin / sizeof(std::uint64_t), sections.cells,
                        gathered_cells, view.key_slots_count};
                    gathered_cells += view.key_slots_count;
                }
                continue;
            }
            std::size_t const cells_count = static_cast<std::size_t>(view.key_slots_count);
            key_slot_type_ const* cells = view.cells;
            // A run keeping its keys in its tree alone stages its cells on the bounce to upload.
            if (cells == nullptr && cells_count != 0) {
                key_slot_type_* const staged = carve_bounce<key_slot_type_>(*executor_, cells_count);
                if (!staged) return status_t::out_of_memory_heap_k;
                for (std::size_t position = 0; position != cells_count; ++position)
                    staged[position] = key_slot_of(view, position);
                cells = staged;
            }
            if ((status = upload(*executor_, sections.cells, cells, cells_count)) != status_t::success_k) return status;
            status = upload(*executor_, sections.value_offsets, view.value_offsets.data(), view.value_offsets.size());
            if (status != status_t::success_k) return status;
            if (view.key_slots_count == 0) continue;
            if (view.stamps != nullptr) {
                std::size_t const bytes = view.stamps->saved_bytes();
                if (view.stamps->save({reinterpret_cast<std::byte*>(saved + saved_offset), bytes}) !=
                    status_t::success_k)
                    return status_t::invalid_argument_k;
                described.saved_stamps = saved_columns + saved_offset;
                saved_offset += bytes / sizeof(std::uint64_t);
            }
            if (view.kinds != nullptr) {
                std::size_t const bytes = view.kinds->saved_bytes();
                if (view.kinds->save({reinterpret_cast<std::byte*>(saved + saved_offset), bytes}) !=
                    status_t::success_k)
                    return status_t::invalid_argument_k;
                described.saved_kinds = saved_columns + saved_offset;
                saved_offset += bytes / sizeof(std::uint64_t);
            }
        }
        if ((status = upload(*executor_, saved_columns, saved, saved_count)) != status_t::success_k) return status;
        if ((status = upload(*executor_, runs, host_runs, sorted_runs_count)) != status_t::success_k) return status;
        if (!reads_.empty()) {
            std::span<byte_t> const scratch {reinterpret_cast<byte_t*>(holding_address), holding_bytes};
            if ((status = sections_.read(reads_, scratch)) != status_t::success_k) return status;
        }
        // Runs keeping their keys in their trees alone gather their cells in one pass, then rank.
        if constexpr (!is_inline(key_slot_type_::value_layout_k))
            if (gathered_cells != 0) {
                if ((status = upload(*executor_, gathers, host_gathers, gathers_count)) != status_t::success_k)
                    return status;
                status = launch_over(*executor_, names_t::gather_k, gathered_cells, std::uint64_t {1} << 20,
                                     static_cast<key_rows_gather<key_slot_type_> const*>(gathers), gathers_count,
                                     gathered_cells);
                if (status != status_t::success_k) return status;
            }

        // The pinned stamps are a few words, read by every survival check, so the device holds
        // its own copy.
        compaction_stamps_t device_stamps = stamps;
        if (stamps.pinned_count != 0) {
            status = upload(*executor_, pinned, stamps.pinned, static_cast<std::size_t>(stamps.pinned_count));
            if (status != status_t::success_k) return status;
            device_stamps.pinned = pinned;
        }

        // Every record lands at its merged position with its verdict, then the survivors pack, the
        // folded ones swapped
        std::uint64_t survivors_count = 0;
        std::uint64_t* prefixes = nullptr;
        std::uint8_t* host_shares = nullptr;
        if (merged_count != 0) {
            status = launch_over(*executor_, names_t::rank_k, merged_count, std::uint64_t {1} << 20,
                                 static_cast<merge_run<key_slot_type_> const*>(runs),
                                 static_cast<std::uint32_t>(sorted_runs_count),
                                 static_cast<std::uint32_t>(upper.size()), bounds.value_stride, merged_count, kind,
                                 device_stamps, merged, flags);
            if (status != status_t::success_k) return status;
            std::span<std::uint64_t> indices;
            std::span<record_t> replacements;
            if constexpr (device_folds_k)
                if (changes) {
                    status = fold_survivors(*executor_, blocks_, kind, stamps, sorted_runs_count, bounds.value_stride,
                                            merged, flags, merged_count, fold, device_fold, read_value, output, indices,
                                            replacements);
                    if (status != status_t::success_k) return status;
                }
            std::uint64_t* const replaced = carve_<std::uint64_t>(indices.size());
            record_t* const replacing = carve_<record_t>(replacements.size());
            if (!indices.empty() && (!replaced || !replacing)) return status_t::out_of_memory_heap_k;
            if ((status = upload(*executor_, replaced, indices.data(), indices.size())) != status_t::success_k)
                return status;
            if ((status = upload(*executor_, replacing, replacements.data(), replacements.size())) !=
                status_t::success_k)
                return status;
            if ((status = executor_status(executor_->fill_async(reinterpret_cast<std::uintptr_t>(chains), byte_t {0},
                                                                2 * sizeof(std::uint64_t)))) != status_t::success_k)
                return status;
            std::uint64_t const tickets = fu::min_of(
                divide_round_up(merged_count, std::uint64_t {merge_block_threads_k}), utilities::chain_blocks_max_k);
            status = launch_over(*executor_, names_t::pack_k, tickets * merge_block_threads_k, tickets,
                                 static_cast<record_t const*>(merged), static_cast<std::uint8_t const*>(flags),
                                 merged_count, static_cast<std::uint64_t const*>(replaced),
                                 static_cast<record_t const*>(replacing), std::uint64_t {indices.size()}, tickets,
                                 survivors, prefix, shares, chains);
            if (status != status_t::success_k) return status;

            // What plans the new runs comes back at once, sized by the records, as only the
            // device counts survivors
            std::uint64_t* const selected = carve_bounce<std::uint64_t>(*executor_, 1);
            prefixes = locates_k ? carve_bounce<std::uint64_t>(*executor_, records_count + 1) : nullptr;
            host_shares = carve_bounce<std::uint8_t>(*executor_, records_count);
            if (!selected || !host_shares || (locates_k && !prefixes) || executor_->pin_bounce().failed())
                return status_t::out_of_memory_heap_k;
            if ((status = download(*executor_, selected, chains, 1)) != status_t::success_k) return status;
            if ((status = download(*executor_, prefixes, prefix, locates_k ? records_count + 1 : 0)) !=
                status_t::success_k)
                return status;
            if ((status = download(*executor_, host_shares, shares, records_count)) != status_t::success_k)
                return status;
            if ((status = executor_status(executor_->wait())) != status_t::success_k) return status;
            survivors_count = *selected;
        }
        else if (locates_k) {
            prefixes = carve_bounce<std::uint64_t>(*executor_, 1);
            if (!prefixes) return status_t::out_of_memory_heap_k;
            prefixes[0] = 0;
        }

        auto const read_prefix = [&](std::uint64_t index, std::uint64_t& bytes) noexcept {
            bytes = locates_k ? prefixes[static_cast<std::size_t>(index)] : 0;
            return status_t::success_k;
        };
        auto const shares_key = [&](std::uint64_t index, bool& shared) noexcept {
            shared = host_shares[static_cast<std::size_t>(index)] != 0;
            return status_t::success_k;
        };
        status = plan_sorted_runs<key_slot_type_>(survivors_count, bounds, read_prefix, shares_key, output.sorted_runs);
        if (status != status_t::success_k) return status;

        std::uint64_t const cells_count = survivors_count;
        std::size_t const survivors_size = static_cast<std::size_t>(survivors_count);
        key_slot_type_* const cells = carve_<key_slot_type_>(survivors_size);
        std::uint64_t* const survivor_stamps = carve_<std::uint64_t>(survivors_size);
        std::uint8_t* const survivor_kinds = carve_<std::uint8_t>(survivors_size);
        if (survivors_size && (!cells || !survivor_stamps || !survivor_kinds)) return status_t::out_of_memory_heap_k;
        std::size_t const placed_count = locates_k ? survivors_size : 0;
        record_t* const placed = carve_bounce<record_t>(*executor_, placed_count);
        if ((placed_count != 0 && !placed) || executor_->pin_bounce().failed()) return status_t::out_of_memory_heap_k;
        if (cells_count != 0) {
            status = launch_over(*executor_, names_t::place_k, cells_count, std::uint64_t {1} << 20,
                                 static_cast<record_t const*>(survivors), cells_count, stamps.floor, cells,
                                 survivor_stamps, survivor_kinds);
            if (status != status_t::success_k) return status;
        }

        if (output.cells.resize(uninitialized, cells_count) != status_t::success_k ||
            output.stamps.resize(uninitialized, survivors_count) != status_t::success_k ||
            output.kinds.resize(uninitialized, survivors_count) != status_t::success_k)
            return status_t::out_of_memory_heap_k;
        if ((status = download(*executor_, output.cells.data(), cells, static_cast<std::size_t>(cells_count))) !=
            status_t::success_k)
            return status;
        if ((status = download(*executor_, output.stamps.data(), survivor_stamps, survivors_size)) !=
            status_t::success_k)
            return status;
        if ((status = download(*executor_, output.kinds.data(), survivor_kinds, survivors_size)) != status_t::success_k)
            return status;
        if ((status = download(*executor_, placed, static_cast<record_t const*>(survivors), placed_count)) !=
            status_t::success_k)
            return status;
        if ((status = executor_status(executor_->wait())) != status_t::success_k) return status;

        // A merge that outgrew the newest run leaves one run as large as all it carved, so
        // the next of its size opens none; the device is idle and the reader forgets the run
        // it registered first.
        if (blocks_.count > 1) {
            std::size_t carved = blocks_.used;
            for (std::uint32_t block = 0; block + 1 != blocks_.count; ++block) carved += blocks_.blocks[block].capacity;
            sections_.release();
            utilities::rewind(blocks_, *executor_);
            if (carve_<byte_t>(carved)) utilities::rewind(blocks_, *executor_);
        }

        // The prefix is the new runs' value offsets, and the copy plan coalesces the survivors'
        // sources on the host.
        output.value_offsets.clear();
        output.value_copy_ranges.clear();
        if constexpr (locates_k) {
            if (is_variable_width(key_slot_type_::value_layout_k)) {
                if (output.value_offsets.resize(uninitialized, survivors_count + 1) != status_t::success_k)
                    return status_t::out_of_memory_heap_k;
                std::memcpy(output.value_offsets.data(), prefixes, (survivors_count + 1) * sizeof(std::uint64_t));
            }
            return plan_value_copies<key_slot_type_>(placed, upper, lower, bounds.value_stride, output.sorted_runs,
                                                     output.value_copy_ranges);
        }
        return status_t::success_k;
    }
}

#pragma endregion Merge

#endif

} // namespace unum::ustore::blobs
