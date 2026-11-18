/**
 *  @file src/vram/vram_compaction_apple.metal
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The compactor's kernels on Apple devices in MSL: ranking, packing and placing a merge,
 *      gathering cells from key trees, ranking strings, and folding graph adjacency.
 *
 *  The Metal twin of the merge and fold kernels of @c vram_kernels_nvidia.cu , under the same
 *  names, each running the body of @c vram_nvidia_compaction.cuh or @c vram_adjacency_fold.cuh
 *  restated here, as MSL includes no host header. The host splices it after
 *  @c vram_kernels_apple.metal , whose block table, block scans and select it reuses, and binds
 *  every kernel as that file's do: the CUDA parameters packed as one struct, every pointer among
 *  them, embedded ones too, a host address the block table turns into a device one.
 *
 *  Rank, place, gather and the strings rank take the CUDA parameters as they are. The pack and the
 *  fold write chain one scan across every block on CUDA with 64-bit atomics, which Metal lacks, so
 *  here they run after the select's passes instead: the pack takes every tile of the merged records
 *  from the survivors before it, which the select's count and scan passes leave as 64-bit prefixes
 *  of 32-bit tile counts, and the fold writes every kept record by its rank, which the select's
 *  write pass lists. The pack scans its value offsets per tile, which @c ustore_merge_offsets_apple
 *  raises by the bytes of the tiles before, and the fold write hands every chain the rank of its
 *  first kept record.
 */

namespace unum {
namespace ustore {

#pragma region Vocabulary

/** A 16-byte stored key as SmashTable's @c key128_t holds it, ordered by its high word first. */
struct key128_t {
    ulong high;
    ulong low;
};

inline bool operator<(key128_t first, key128_t second) {
    return first.high < second.high || (first.high == second.high && first.low < second.low);
}

inline bool operator==(key128_t first, key128_t second) { return first.high == second.high && first.low == second.low; }

/** How a key slot holds its value, as the host's @c value_layout_t numbers it. */
enum class value_layout_t : ushort {
    inline_12_bytes_k = 12,
    inline_44_bytes_k = 44,
    fixed_width_k = 65,
    variable_width_k = 66,
    uuid_k = 67,
};

/** The kinds of a delta, as the host's @c delta_kind_t numbers them. */
enum class delta_kind_t : uchar {
    write_k = 0,
    remove_k = 1,
    change_k = 2,
};

/** Which levels a compaction merges, as the host's @c compaction_kind_t numbers them. */
enum class compaction_kind_t : uchar {
    overlapping_into_intermediate_k,
    overlapping_into_bottom_k,
    sorted_into_intermediate_k,
    sorted_into_bottom_k,
};

/** A key slot holding its value inline after its key and size, as the host's @c key_slot . */
template <typename stored_key_type_, value_layout_t value_layout_>
struct key_slot {
    stored_key_type_ stored_key;
    uint size;
    uchar data[static_cast<ushort>(value_layout_)];
};

/** A key slot of a fixed-width value, which its run finds by stride: its key alone. */
template <typename stored_key_type_>
struct key_slot<stored_key_type_, value_layout_t::fixed_width_k> {
    stored_key_type_ stored_key;
};

/** A key slot of a variable-width value, which its run finds through its value offsets:
 *  its key alone. */
template <typename stored_key_type_>
struct key_slot<stored_key_type_, value_layout_t::variable_width_k> {
    stored_key_type_ stored_key;
};

/** A key slot holding its 16-byte value inline with no size, the strings modality's. */
template <typename stored_key_type_>
struct key_slot<stored_key_type_, value_layout_t::uuid_k> {
    stored_key_type_ stored_key;
    uchar data[16];
};

/** The key slot a merge of strings judges survival on, whose stored key is a head. */
using strings_merge_slot_t = key_slot<key128_t, value_layout_t::uuid_k>;

/** A key slot with its source run, kind, stamp, position and value bytes, as the host's
 *  @c located_key_slot . */
template <typename key_slot_type_>
struct located_key_slot {
    key_slot_type_ key_slot;
    uint sorted_run;
    delta_kind_t kind;
    ulong stamp;
    uint position;
    uint size;
};

/** One version of a merge of strings, its source run and its position there. */
struct strings_version_t {
    uint source;
    uint position;
};

/** The stamps a merge keeps versions for, as the host's @c compaction_stamps_t , @c pinned
 *  a host address. */
struct compaction_stamps_t {
    ulong pinned;
    ulong pinned_count;
    ulong horizon;
    ulong ceiling;
    ulong floor;
};

/** The value at @p index of a column saved as SmashTable saves a packed array: its count, base and
 *  width words, then the packed words. */
inline ulong saved_field(device ulong const* saved, ulong index) {
    ulong const base = saved[1];
    uint const bits = uint(saved[2]);
    if (bits == 0) return base;
    device ulong const* const words = saved + 3;
    ulong const first = index * bits;
    ulong const word = first / 64;
    uint const shift = uint(first % 64);
    ulong field = words[word] >> shift;
    if (shift + bits > 64) field |= words[word + 1] << (64 - shift);
    ulong const mask = bits == 64 ? ~ulong(0) : (ulong(1) << bits) - 1;
    return base + (field & mask);
}

#pragma endregion Vocabulary

#pragma region Rules

/** Orders located key slots by stored key, and equal stored keys newest first, as the host's
 *  @c survivor_first_t . */
struct survivor_first_t {
    uint upper_sorted_runs_count;

    template <typename key_slot_type_>
    bool operator()(thread located_key_slot<key_slot_type_> const& first,
                    thread located_key_slot<key_slot_type_> const& second) const {
        if (first.key_slot.stored_key < second.key_slot.stored_key) return true;
        if (second.key_slot.stored_key < first.key_slot.stored_key) return false;
        // Versions of one key inside one run keep that run's newest-first order
        if (first.sorted_run == second.sorted_run) return first.stamp > second.stamp;
        bool const both_upper = first.sorted_run < upper_sorted_runs_count &&
                                second.sorted_run < upper_sorted_runs_count;
        return both_upper ? first.sorted_run > second.sorted_run : first.sorted_run < second.sorted_run;
    }
};

/** How many of the readers @p stamps pins read at or above @p stamp . */
inline ulong reader_interval(blocks_t blocks, compaction_stamps_t stamps, ulong stamp) {
    device ulong const* const pinned = reach<ulong const>(blocks, stamps.pinned);
    ulong low = 0;
    ulong high = stamps.pinned_count;
    while (low < high) {
        ulong const middle = low + (high - low) / 2;
        if (pinned[middle] < stamp) low = middle + 1;
        else high = middle;
    }
    return stamps.pinned_count - low;
}

/** Whether a version stamped @p current , after one stamped @p previous of the same key,
 *  is still read. */
inline bool keeps_older_version(blocks_t blocks, compaction_stamps_t stamps, ulong previous, ulong current) {
    if (previous > stamps.ceiling || previous > stamps.horizon || current > stamps.horizon) return true;
    return reader_interval(blocks, stamps, current) != reader_interval(blocks, stamps, previous);
}

/** Whether a merge of @p kind writes the bottom level, where nothing older is left for a
 *  removal to hide. */
inline bool writes_bottom(compaction_kind_t kind) {
    return kind == compaction_kind_t::overlapping_into_bottom_k || kind == compaction_kind_t::sorted_into_bottom_k;
}

/** Whether @p current survives a merge: some reader still reads it, and it is no removal where
 *  nothing older survives below it to hide. */
template <typename key_slot_type_>
inline bool survives(blocks_t blocks, thread located_key_slot<key_slot_type_> const* previous,
                     thread located_key_slot<key_slot_type_> const& current, compaction_kind_t kind,
                     compaction_stamps_t stamps) {
    if (current.stamp > stamps.ceiling) return false;
    if (previous != nullptr && previous->key_slot.stored_key == current.key_slot.stored_key &&
        !keeps_older_version(blocks, stamps, previous->stamp, current.stamp))
        return false;
    // Only in the oldest interval does every older version of the key fall in the same one and
    // drop with it
    bool const oldest = current.stamp <= stamps.horizon &&
                        reader_interval(blocks, stamps, current.stamp) == stamps.pinned_count;
    return !(writes_bottom(kind) && current.kind == delta_kind_t::remove_k && oldest);
}

#pragma endregion Rules

#pragma region Merge

/** The keys from @c start up to @c end , exclusive, which one range removal hides, as the host's
 *  @c range_tombstone . */
struct range_tombstone {
    key128_t start;
    key128_t end;
};

/** One run of a merge as the host lays out @c merge_run , every pointer a host address, its
 *  last three words the host's @c range_tombstones_view . */
template <typename key_slot_type_>
struct merge_run {
    ulong cells;
    ulong value_offsets;
    ulong saved_stamps;
    ulong saved_kinds;
    ulong assigned_stamp;
    ulong first;
    ulong key_slots_count;
    ulong ranges;
    ulong range_stamps;
    ulong ranges_count;
};

/** Whether a range tombstone of @p runs hides the version of @p stored_key stamped @p stamp
 *  that run @p run holds from every reader of @p stamps , as the host's @c hidden_by_range . */
template <typename key_slot_type_>
inline bool hidden_by_range(blocks_t blocks, device merge_run<key_slot_type_> const* runs, uint runs_count,
                            uint upper_runs, key128_t stored_key, uint run, ulong stamp, compaction_stamps_t stamps) {
    for (uint source = 0; source != runs_count; ++source) {
        bool const own = source == run;
        bool const newer = source < upper_runs && (run >= upper_runs || source > run);
        if (!own && !newer) continue;
        merge_run<key_slot_type_> const theirs = runs[source];
        device range_tombstone const* const ranges = reach<range_tombstone const>(blocks, theirs.ranges);
        device ulong const* const range_stamps = reach<ulong const>(blocks, theirs.range_stamps);
        for (ulong index = 0; index != theirs.ranges_count && !(stored_key < ranges[index].start); ++index) {
            ulong const removed = range_stamps[index];
            if (!(stored_key < ranges[index].end) || removed > stamps.ceiling || removed > stamps.horizon) continue;
            if (own && removed <= stamp) continue;
            if (stamp >= removed || reader_interval(blocks, stamps, stamp) == reader_interval(blocks, stamps, removed))
                return true;
        }
    }
    return false;
}

/** The record at sorted @p position of @p run , the run's index being @p index , with its stamp
 *  and kind unpacked. */
template <typename key_slot_type_>
inline located_key_slot<key_slot_type_> merge_record(blocks_t blocks, merge_run<key_slot_type_> run, uint index,
                                                     ulong value_stride, ulong position) {
    device uint const* const value_offsets = reach<uint const>(blocks, run.value_offsets);
    located_key_slot<key_slot_type_> record;
    record.key_slot = reach<key_slot_type_ const>(blocks, run.cells)[position];
    record.sorted_run = index;
    record.kind = delta_kind_t::write_k;
    record.stamp = 0;
    record.position = uint(position);
    record.size = value_offsets != nullptr ? value_offsets[position + 1] - value_offsets[position] : uint(value_stride);
    if (run.saved_stamps != 0)
        record.stamp = run.assigned_stamp != 0 ? run.assigned_stamp
                                               : saved_field(reach<ulong const>(blocks, run.saved_stamps), position);
    if (run.saved_kinds != 0)
        record.kind = delta_kind_t(saved_field(reach<ulong const>(blocks, run.saved_kinds), position));
    return record;
}

/** The parameters of @c ustore_merge_rank_* , in CUDA order. */
struct merge_rank_arguments_t {
    ulong runs;
    uint runs_count;
    uint upper_runs;
    ulong value_stride;
    ulong records_count;
    compaction_kind_t kind;
    compaction_stamps_t stamps;
    ulong merged;
    ulong flags;
};

/** Writes every record of the runs at its merged position with its survival flag: its rank
 *  in its own run plus a binary search into every other, the last of those of its key
 *  deciding its survival. A record a range tombstone hides lands as a dropped removal. */
template <typename key_slot_type_>
inline void merge_rank(constant merge_rank_arguments_t& arguments, blocks_t blocks, uint thread_index, uint threads) {
    using record_t = located_key_slot<key_slot_type_>;
    device merge_run<key_slot_type_> const* const runs = reach<merge_run<key_slot_type_> const>(blocks, arguments.runs);
    device record_t* const merged = reach<record_t>(blocks, arguments.merged);
    device uchar* const flags = reach<uchar>(blocks, arguments.flags);
    survivor_first_t const order = {arguments.upper_runs};
    uint const runs_count = arguments.runs_count;
    for (ulong index = thread_index; index < arguments.records_count; index += threads) {
        uint run = 0;
        for (uint high = runs_count; high - run > 1;) {
            uint const middle = run + (high - run) / 2;
            if (runs[middle].first <= index) run = middle;
            else high = middle;
        }
        ulong const position = index - runs[run].first;
        record_t record = merge_record(blocks, runs[run], run, arguments.value_stride, position);
        ulong rank = position;
        record_t previous = record;
        bool preceded = false;
        if (position != 0) {
            previous = merge_record(blocks, runs[run], run, arguments.value_stride, position - 1);
            preceded = previous.key_slot.stored_key == record.key_slot.stored_key;
        }
        for (uint other = 0; other != runs_count; ++other) {
            if (other == run) continue;
            merge_run<key_slot_type_> const theirs = runs[other];
            device key_slot_type_ const* const cells = reach<key_slot_type_ const>(blocks, theirs.cells);
            record_t probe = record;
            probe.sorted_run = other;
            ulong before = 0;
            for (ulong count = theirs.key_slots_count; count;) {
                ulong const half = count / 2;
                probe.key_slot = cells[before + half];
                bool const below = order(probe, record);
                before += below ? half + 1 : 0;
                count = below ? count - half - 1 : half;
            }
            rank += before;
            if (before == 0) continue;
            record_t const candidate = merge_record(blocks, theirs, other, arguments.value_stride, before - 1);
            if (candidate.key_slot.stored_key == record.key_slot.stored_key &&
                (!preceded || order(previous, candidate)))
                previous = candidate, preceded = true;
        }
        bool const hidden = hidden_by_range(blocks, runs, runs_count, arguments.upper_runs, record.key_slot.stored_key,
                                            run, record.stamp, arguments.stamps);
        if (hidden) record.kind = delta_kind_t::remove_k;
        bool const kept = !hidden &&
                          survives(blocks, preceded ? &previous : nullptr, record, arguments.kind, arguments.stamps);
        merged[rank] = record;
        flags[rank] = kept ? 1 : 0;
    }
}

/** The record at merged @p index , or the folded one replacing it, @p replaced listing
 *  their indices ascending. */
template <typename key_slot_type_>
inline located_key_slot<key_slot_type_> merge_packed(device located_key_slot<key_slot_type_> const* merged, ulong index,
                                                     device ulong const* replaced,
                                                     device located_key_slot<key_slot_type_> const* replacements,
                                                     ulong replaced_count) {
    ulong low = 0;
    for (ulong count = replaced_count; count;) {
        ulong const half = count / 2;
        if (replaced[low + half] < index) low += half + 1, count -= half + 1;
        else count = half;
    }
    return low != replaced_count && replaced[low] == index ? replacements[low] : merged[index];
}

/** The parameters of @c ustore_merge_pack_* on Apple, which take the select's scanned counts where
 *  CUDA's take a chain. */
struct merge_pack_arguments_t {

    /** The merged records, their flags and the folded records replacing some, as CUDA's
     *  pack reads them. */
    ulong merged;
    ulong flags;
    ulong records_count;
    ulong replaced;
    ulong replacements;
    ulong replaced_count;

    /** The survivors before every tile of the merged records, and in all after the last. */
    ulong counts;

    /** Records each threadgroup packs, and the threadgroups, as the select tiles them. */
    uint tile;
    uint tiles;

    /** The survivors, their value offsets or null, and whether each shares the key before it. */
    ulong survivors;
    ulong prefix;
    ulong shares;

    /** The bytes of each tile's survivors, which @c ustore_merge_offsets_apple adds after it. */
    ulong tile_bytes;

    /** The survivors' count as one 64-bit word, which the host reads back. */
    ulong survivors_count;
};

/** Packs the survivors of tile @p group of the merged records, as CUDA's pack does after its chain:
 *  each at its rank from the survivors of the tiles before, with its value offset within the tile
 *  and whether it shares its key, then the tile's bytes. */
template <typename key_slot_type_>
inline void merge_pack(constant merge_pack_arguments_t& arguments, blocks_t blocks, uint group, uint lane,
                       threadgroup ulong* simd_totals) {
    using record_t = located_key_slot<key_slot_type_>;
    device record_t const* const merged = reach<record_t const>(blocks, arguments.merged);
    device uchar const* const flags = reach<uchar const>(blocks, arguments.flags);
    device ulong const* const replaced = reach<ulong const>(blocks, arguments.replaced);
    device record_t const* const replacements = reach<record_t const>(blocks, arguments.replacements);
    device ulong const* const counts = reach<ulong const>(blocks, arguments.counts);
    device record_t* const survivors = reach<record_t>(blocks, arguments.survivors);
    device ulong* const prefix = reach<ulong>(blocks, arguments.prefix);
    device uchar* const shares = reach<uchar>(blocks, arguments.shares);
    if (group == 0 && lane == 0) {
        reach<ulong>(blocks, arguments.survivors_count)[0] = counts[arguments.tiles];
        if (prefix != nullptr) prefix[0] = 0;
    }
    ulong const first = min(ulong(group) * arguments.tile, arguments.records_count);
    ulong const end = first + min(arguments.records_count - first, ulong(arguments.tile));
    ulong kept_before = counts[group];
    ulong carried = 0;
    for (ulong base = first; base < end; base += block_threads_k) {
        ulong const index = base + lane;
        bool const live = index < end && flags[index] != 0;
        record_t const record = merge_packed(merged, live ? index : first, replaced, replacements,
                                             arguments.replaced_count);
        ulong const size = live ? ulong(record.size) : 0;
        block_scanned<ulong> const kept_scan = block_exclusive(ulong(live ? 1 : 0), lane, simd_totals);
        block_scanned<ulong> const bytes_scan = block_exclusive(size, lane, simd_totals);
        ulong const into = kept_before + kept_scan.before;
        if (live) survivors[into] = record;
        if (live && prefix != nullptr) prefix[into + 1] = carried + bytes_scan.before + size;
        // A survivor shares its key when a flagged record of the same key precedes it
        bool shared = false;
        for (ulong older = index;
             live && !shared && older-- != 0 && merged[older].key_slot.stored_key == merged[index].key_slot.stored_key;)
            shared = flags[older] != 0;
        if (live) shares[into] = shared ? 1 : 0;
        kept_before += kept_scan.total, carried += bytes_scan.total;
    }
    if (lane == 0) reach<ulong>(blocks, arguments.tile_bytes)[group] = carried;
}

/** The parameters of @c ustore_merge_offsets_apple . */
struct merge_offsets_arguments_t {

    /** The survivors before every tile of the merged records, and in all after the last. */
    ulong counts;

    /** The threadgroups, one per tile, as the pack tiled the records. */
    uint tiles;

    /** The bytes of each tile's survivors and their value offsets within their tile, as the
     *  pack wrote them. */
    ulong tile_bytes;
    ulong prefix;
};

/** The parameters of @c ustore_merge_place_* , in CUDA order. */
struct merge_place_arguments_t {
    ulong survivors;
    ulong survivors_count;
    ulong floor;
    ulong cells;
    ulong stamps;
    ulong kinds;
};

/** Writes the cell of every survivor, which the new runs hold in order, with its stamp, raised to
 *  the floor, and its kind. */
template <typename key_slot_type_>
inline void merge_place(constant merge_place_arguments_t& arguments, blocks_t blocks, uint thread_index, uint threads) {
    device located_key_slot<key_slot_type_> const* const survivors = reach<located_key_slot<key_slot_type_> const>(
        blocks, arguments.survivors);
    device key_slot_type_* const cells = reach<key_slot_type_>(blocks, arguments.cells);
    device ulong* const stamps = reach<ulong>(blocks, arguments.stamps);
    device uchar* const kinds = reach<uchar>(blocks, arguments.kinds);
    for (ulong survivor = thread_index; survivor < arguments.survivors_count; survivor += threads) {
        located_key_slot<key_slot_type_> const source = survivors[survivor];
        stamps[survivor] = source.stamp > arguments.floor ? source.stamp : arguments.floor;
        kinds[survivor] = uchar(source.kind);
        cells[survivor] = source.key_slot;
    }
}

/** One run whose cells come from the leaf rows of its key tree, as the host lays out
 *  @c key_rows_gather . */
template <typename key_slot_type_>
struct key_rows_gather {
    ulong rows;
    ulong cells;
    ulong first;
    ulong count;
};

/** The parameters of @c ustore_merge_gather_* , in CUDA order. */
struct merge_gather_arguments_t {
    ulong gathers;
    uint gathers_count;
    ulong cells_count;
};

/** Writes the cell of every key of every gathered run, read from the leaf rows of its key tree,
 *  where consecutive threads read consecutive words of a row. */
template <typename key_slot_type_>
inline void merge_gather(constant merge_gather_arguments_t& arguments, blocks_t blocks, uint thread_index,
                         uint threads) {
    device key_rows_gather<key_slot_type_> const* const gathers = reach<key_rows_gather<key_slot_type_> const>(
        blocks, arguments.gathers);
    for (ulong index = thread_index; index < arguments.cells_count; index += threads) {
        uint gather = 0;
        for (uint high = arguments.gathers_count; high - gather > 1;) {
            uint const middle = gather + (high - gather) / 2;
            if (gathers[middle].first <= index) gather = middle;
            else high = middle;
        }
        key_rows_gather<key_slot_type_> const run = gathers[gather];
        ulong const position = index - run.first;
        // SmashTable's 512-byte rows hold 32 keys of 16 bytes: a column of high words, then one
        // of low words
        device ulong const* const row = reach<ulong const>(blocks, run.rows) + position / 32 * 64;
        key128_t const key = {row[position % 32], row[32 + position % 32]};
        key_slot_type_ cell;
        cell.stored_key = key;
        reach<key_slot_type_>(blocks, run.cells)[position] = cell;
    }
}

#pragma endregion Merge

#pragma region Strings Ranking

/** One run of strings as the host lays out @c strings_merge_run , every pointer a host address. */
struct strings_merge_run {
    ulong heads;
    ulong key_offsets;
    ulong keys;
    ulong saved_stamps;
    ulong saved_kinds;
    ulong first;
    ulong count;
    ulong range_stamps;
    ulong range_offsets;
    ulong range_tape;
    ulong ranges_count;
};

/** Where a version lands in a merge of strings, and whether it survives. */
struct strings_merge_verdict_t {
    ulong rank;
    bool survives;
};

/** Orders the key at @p first_position of @p first against the one at @p second_position of
 *  @p second : by their heads, and on a tie by the bytes past them on the tapes. */
inline int strings_merge_order(blocks_t blocks, strings_merge_run first, ulong first_position, strings_merge_run second,
                               ulong second_position) {
    key128_t const first_head = reach<key128_t const>(blocks, first.heads)[first_position];
    key128_t const second_head = reach<key128_t const>(blocks, second.heads)[second_position];
    if (first_head < second_head) return -1;
    if (second_head < first_head) return 1;
    // Equal heads share their leading bytes and a length class, so only the bytes past them differ
    ulong const head_bytes = 15;
    device uint const* const first_offsets = reach<uint const>(blocks, first.key_offsets);
    device uint const* const second_offsets = reach<uint const>(blocks, second.key_offsets);
    device uchar const* const first_keys = reach<uchar const>(blocks, first.keys);
    device uchar const* const second_keys = reach<uchar const>(blocks, second.keys);
    ulong first_offset = first_offsets[first_position];
    ulong const first_end = first_offsets[first_position + 1];
    ulong second_offset = second_offsets[second_position];
    ulong const second_end = second_offsets[second_position + 1];
    first_offset += first_end - first_offset < head_bytes ? first_end - first_offset : head_bytes;
    second_offset += second_end - second_offset < head_bytes ? second_end - second_offset : head_bytes;
    for (; first_offset != first_end && second_offset != second_end; ++first_offset, ++second_offset) {
        uchar const first_byte = first_keys[first_offset];
        uchar const second_byte = second_keys[second_offset];
        if (first_byte != second_byte) return first_byte < second_byte ? -1 : 1;
    }
    return first_offset != first_end ? 1 : second_offset != second_end ? -1 : 0;
}

/** Orders the key at @p position of @p run against the @p length bytes at @p text , as strings
 *  order: by their bytes, a prefix first. */
inline int strings_merge_order(blocks_t blocks, strings_merge_run run, ulong position, device uchar const* text,
                               uint length) {
    device uint const* const offsets = reach<uint const>(blocks, run.key_offsets);
    device uchar const* const keys = reach<uchar const>(blocks, run.keys);
    uint offset = offsets[position];
    uint const end = offsets[position + 1];
    for (uint index = 0; offset != end && index != length; ++offset, ++index) {
        uchar const key_byte = keys[offset];
        uchar const text_byte = text[index];
        if (key_byte != text_byte) return key_byte < text_byte ? -1 : 1;
    }
    uint const key_length = end - offsets[position];
    return key_length == length ? 0 : key_length < length ? -1 : 1;
}

/** Whether a range tombstone of @p runs hides the version stamped @p stamp at @p position of run
 *  @p run from every reader of @p stamps , as the host's @c strings_merge_hidden . */
inline bool strings_merge_hidden(blocks_t blocks, device strings_merge_run const* runs, uint runs_count,
                                 uint upper_runs, uint run, ulong position, ulong stamp, compaction_stamps_t stamps) {
    strings_merge_run const own_run = runs[run];
    for (uint holder = 0; holder != runs_count; ++holder) {
        strings_merge_run const theirs = runs[holder];
        bool const own = holder == run;
        bool const newer = holder < upper_runs && (run >= upper_runs || holder > run);
        if (!own && !newer) continue;
        device ulong const* const range_stamps = reach<ulong const>(blocks, theirs.range_stamps);
        device uint const* const range_offsets = reach<uint const>(blocks, theirs.range_offsets);
        device uchar const* const range_tape = reach<uchar const>(blocks, theirs.range_tape);
        for (ulong index = 0; index != theirs.ranges_count; ++index) {
            ulong const removed = range_stamps[index];
            if (removed > stamps.ceiling || removed > stamps.horizon || (own && removed <= stamp)) continue;
            if (stamp < removed && reader_interval(blocks, stamps, stamp) != reader_interval(blocks, stamps, removed))
                continue;
            device uint const* const bounds = range_offsets + 2 * index;
            if (strings_merge_order(blocks, own_run, position, range_tape + bounds[0], bounds[1] - bounds[0]) < 0)
                continue;
            if (bounds[2] == bounds[1] ||
                strings_merge_order(blocks, own_run, position, range_tape + bounds[1], bounds[2] - bounds[1]) < 0)
                return true;
        }
    }
    return false;
}

/** The version at @p position of @p run as @c survives reads it: its head, run, kind and stamp. */
inline located_key_slot<strings_merge_slot_t> strings_merge_record(blocks_t blocks, strings_merge_run run, uint index,
                                                                   ulong position) {
    located_key_slot<strings_merge_slot_t> record;
    record.key_slot.stored_key = reach<key128_t const>(blocks, run.heads)[position];
    record.sorted_run = index;
    record.kind = delta_kind_t::write_k;
    record.stamp = 0;
    record.position = uint(position);
    record.size = 0;
    if (run.saved_stamps != 0) record.stamp = saved_field(reach<ulong const>(blocks, run.saved_stamps), position);
    if (run.saved_kinds != 0)
        record.kind = delta_kind_t(saved_field(reach<ulong const>(blocks, run.saved_kinds), position));
    return record;
}

/** Where the version at @p position of run @p run lands in the merge of @p runs , and whether it
 *  survives: its own position plus, for every other run, how many of that run's versions order
 *  before it, the last of those of its key being the version before it. A version a range
 *  tombstone hides does not survive. */
inline strings_merge_verdict_t strings_merge_verdict(blocks_t blocks, device strings_merge_run const* runs,
                                                     uint runs_count, uint upper_runs, uint run, ulong position,
                                                     compaction_kind_t kind, compaction_stamps_t stamps) {
    strings_merge_run const own = runs[run];
    ulong rank = position;
    uint previous_run = run;
    ulong previous_position = position;
    bool preceded = position != 0 && strings_merge_order(blocks, own, position - 1, own, position) == 0;
    if (preceded) previous_position = position - 1;
    for (uint other = 0; other != runs_count; ++other) {
        if (other == run) continue;
        strings_merge_run const theirs = runs[other];
        // Newer upper runs come ahead of older ones, and every upper run ahead of the lower ones
        bool const both_upper = other < upper_runs && run < upper_runs;
        bool const ahead = both_upper ? other > run : other < run;
        ulong before = 0;
        for (ulong count = theirs.count; count;) {
            ulong const half = count / 2;
            int const order = strings_merge_order(blocks, theirs, before + half, own, position);
            if (order < 0 || (order == 0 && ahead)) before += half + 1, count -= half + 1;
            else count = half;
        }
        rank += before;
        if (before == 0 || !ahead || strings_merge_order(blocks, theirs, before - 1, own, position) != 0) continue;
        // Of the versions of this key ordered before it, the latest is in the run that comes last
        bool const previous_upper = previous_run < upper_runs && other < upper_runs;
        bool const previous_first = previous_upper ? previous_run > other : previous_run < other;
        if (!preceded || (previous_run != run && previous_first))
            previous_run = other, previous_position = before - 1, preceded = true;
    }
    located_key_slot<strings_merge_slot_t> const record = strings_merge_record(blocks, own, run, position);
    located_key_slot<strings_merge_slot_t> const previous = strings_merge_record(blocks, runs[previous_run],
                                                                                 previous_run, previous_position);
    bool const hidden = strings_merge_hidden(blocks, runs, runs_count, upper_runs, run, position, record.stamp, stamps);
    return {rank, !hidden && survives(blocks, preceded ? &previous : nullptr, record, kind, stamps)};
}

/** The parameters of @c ustore_merge_strings_rank , in CUDA order. */
struct merge_strings_rank_arguments_t {
    ulong runs;
    uint runs_count;
    uint upper_runs;
    ulong records_count;
    compaction_kind_t kind;
    compaction_stamps_t stamps;
    ulong merged;
    ulong flags;
};

/** Writes each version of a merge of strings at its merged position with its survival flag, one
 *  thread per version, as @c strings_merge_verdict places it. */
inline void merge_strings_rank(constant merge_strings_rank_arguments_t& arguments, blocks_t blocks, uint thread_index,
                               uint threads) {
    device strings_merge_run const* const runs = reach<strings_merge_run const>(blocks, arguments.runs);
    device strings_version_t* const merged = reach<strings_version_t>(blocks, arguments.merged);
    device uchar* const flags = reach<uchar>(blocks, arguments.flags);
    for (ulong index = thread_index; index < arguments.records_count; index += threads) {
        uint run = 0;
        for (uint high = arguments.runs_count; high - run > 1;) {
            uint const middle = run + (high - run) / 2;
            if (runs[middle].first <= index) run = middle;
            else high = middle;
        }
        ulong const position = index - runs[run].first;
        strings_merge_verdict_t const verdict = strings_merge_verdict(
            blocks, runs, arguments.runs_count, arguments.upper_runs, run, position, arguments.kind, arguments.stamps);
        strings_version_t const version = {run, uint(position)};
        merged[verdict.rank] = version;
        flags[verdict.rank] = verdict.survives ? 1 : 0;
    }
}

#pragma endregion Strings Ranking

#pragma region Adjacency Fold

/** One record of a chain on the device, aligned so a copy moves it in three 16-byte words. */
struct alignas(16) half_edge_record_t {
    uchar bytes[48];
};

/** Where a half-edge delta keeps its neighbor, the end of its edge, its role and its operation. */
constant constexpr uint delta_neighbor_offset_k = 0;
constant constexpr uint delta_weight_offset_k = 32;
constant constexpr uint delta_role_offset_k = 40;
constant constexpr uint delta_operation_offset_k = 41;

/** The operations of a half-edge delta, as the host's @c half_edge_operation_t numbers them. */
enum class half_edge_operation_t : uchar {
    insert_k = 0,
    remove_k = 1,
    remove_vertex_k = 2,
};

/** The operation of @p record . */
inline half_edge_operation_t record_operation(device half_edge_record_t const& record) {
    return half_edge_operation_t(record.bytes[delta_operation_offset_k]);
}

/** Orders the half-edges of two records as @c compare_half_edges does: the neighbor's and then the
 *  edge's bytes, then the role. */
inline int compare_record_half_edges(device half_edge_record_t const& record, device half_edge_record_t const& other) {
    for (uint offset = delta_neighbor_offset_k; offset != delta_weight_offset_k; ++offset)
        if (record.bytes[offset] != other.bytes[offset]) return int(record.bytes[offset]) - int(other.bytes[offset]);
    return int(record.bytes[delta_role_offset_k]) - int(other.bytes[delta_role_offset_k]);
}

/** Whether gathered record @p first orders before @p second of the same chain, newest first, so
 *  the record of a half-edge that survives leads its run; the largest index stands for the padding
 *  past the chain. */
inline bool chained_before(device half_edge_record_t const* records, uint first, uint second) {
    uint const padding = 0xFFFFFFFFu;
    if (first == padding || second == padding) return second == padding && first != padding;
    bool const first_clears = record_operation(records[first]) == half_edge_operation_t::remove_vertex_k;
    bool const second_clears = record_operation(records[second]) == half_edge_operation_t::remove_vertex_k;
    if (first_clears != second_clears) return first_clears;
    int const order = first_clears ? 0 : compare_record_half_edges(records[first], records[second]);
    if (order != 0) return order < 0;
    return first > second;
}

/** Orders the indices of @p sorted, in threadgroup or device memory, at @p index and @p stride past
 *  it, as one step of a bitonic sort of runs of @p size compares them, where @p index is the lower
 *  of the pair. */
template <typename sorted_type_>
inline void bitonic_exchange(device half_edge_record_t const* records, sorted_type_ sorted, uint index, uint size,
                             uint stride) {
    uint const partner = index ^ stride;
    if (partner <= index) return;
    bool const ascending = (index & size) == 0;
    uint const low = sorted[index], high = sorted[partner];
    if (ascending ? chained_before(records, high, low) : chained_before(records, low, high))
        sorted[index] = high, sorted[partner] = low;
}

/** One step of a bitonic sort of the @p width indices in @p sorted , comparing every pair @p stride
 *  apart within runs of @p size , each pair by one thread of the threadgroup. */
inline void bitonic_step(device half_edge_record_t const* records, threadgroup uint* sorted, uint width, uint size,
                         uint stride, uint lane) {
    for (uint index = lane; index < width; index += block_threads_k)
        bitonic_exchange(records, sorted, index, size, stride);
}

/** The chains a threadgroup sorts no more of in its memory, as the host's @c fold_spill_t lays
 *  them out across threadgroups in device memory. */
struct fold_spill_t {
    ulong sorted;
    ulong starts;
    ulong chains;
    uint count;
    uint shared_width;
};

/** As @c bitonic_exchange , at @p index of every spilled index in @p sorted , in the slot of the
 *  @p count slots @p starts names holding it, whose chain is wide enough for runs of @p size . */
inline void spilled_exchange(device half_edge_record_t const* records, device uint* sorted, device ulong const* starts,
                             uint count, ulong index, uint size, uint stride) {
    uint slot = 0;
    for (uint high = count; high - slot > 1;) {
        uint const middle = slot + (high - slot) / 2;
        if (starts[middle] <= index) slot = middle;
        else high = middle;
    }
    ulong const first = starts[slot];
    if (starts[slot + 1] - first < size) return;
    bitonic_exchange(records, sorted + first, uint(index - first), size, stride);
}

/** The parameters of @c ustore_fold_sort_step on Apple, in CUDA order. */
struct fold_sort_step_arguments_t {
    ulong records;
    fold_spill_t spill;
    uint size;
    uint stride;
};

/** One step of the bitonic sort of every spilled chain, as CUDA's @c fold_sort_step does: a stride
 *  at or above the threadgroup's threads spans the grid, and a shorter one finishes every stride
 *  below it within tiles of two indices per thread. */
inline void fold_sort_step(constant fold_sort_step_arguments_t& arguments, blocks_t blocks, uint group, uint groups,
                           uint lane) {
    device half_edge_record_t const* const records = reach<half_edge_record_t const>(blocks, arguments.records);
    device uint* const sorted = reach<uint>(blocks, arguments.spill.sorted);
    device ulong const* const starts = reach<ulong const>(blocks, arguments.spill.starts);
    uint const count = arguments.spill.count;
    ulong const total = starts[count];
    if (arguments.stride >= block_threads_k) {
        for (ulong index = ulong(group) * block_threads_k + lane; index < total;
             index += ulong(groups) * block_threads_k)
            spilled_exchange(records, sorted, starts, count, index, arguments.size, arguments.stride);
        return;
    }
    uint const tile = 2 * block_threads_k;
    for (ulong first = ulong(group) * tile; first < total; first += ulong(groups) * tile)
        for (uint step = arguments.stride; step; step /= 2) {
            for (uint offset = lane; offset < tile && first + offset < total; offset += block_threads_k)
                spilled_exchange(records, sorted, starts, count, first + offset, arguments.size, step);
            threadgroup_barrier(mem_flags::mem_device);
        }
}

/** Whether the record at @p position of the sorted chain survives: the newest removal of the
 *  vertex, or the newest record of a half-edge newer than it that does not remove the
 *  half-edge behind it. */
template <typename sorted_type_>
inline bool keeps_sorted(device half_edge_record_t const* records, sorted_type_ sorted, uint position,
                         uint newest_removal, half_edge_operation_t newest_operation) {
    uint const index = sorted[position];
    device half_edge_record_t const& record = records[index];
    if (record_operation(record) == half_edge_operation_t::remove_vertex_k) return position == 0;
    device half_edge_record_t const& previous = records[sorted[position - (position != 0 ? 1 : 0)]];
    bool const leads = position == 0 || record_operation(previous) == half_edge_operation_t::remove_vertex_k ||
                       compare_record_half_edges(previous, record) != 0;
    bool const hidden = newest_operation == half_edge_operation_t::remove_vertex_k &&
                        (index < newest_removal || record_operation(record) == half_edge_operation_t::remove_k);
    return leads && !hidden;
}

/** Keeps what survives of the chain of @p length records from @p start , whose record indices
 *  @p sorted holds in order, as CUDA's @c keep_sorted_chain does, then flags the places of its span
 *  it kept, so the select lists every kept record in order. */
template <typename sorted_type_>
inline void keep_sorted_chain(device half_edge_record_t const* records, sorted_type_ sorted, uint start, uint length,
                              device uint* kept, device uint& kept_count, device uchar* flags, uint lane,
                              threadgroup uint* simd_totals) {
    uint const newest_removal = sorted[0];
    half_edge_operation_t const newest_operation = record_operation(records[newest_removal]);
    uint written = 0;
    for (uint base = 0; base < length; base += block_threads_k) {
        uint const position = base + lane;
        bool const keeps = position < length &&
                           keeps_sorted(records, sorted, position, newest_removal, newest_operation);
        block_scanned<uint> const scan = block_exclusive(keeps ? 1u : 0u, lane, simd_totals);
        if (keeps) kept[start + written + scan.before] = sorted[position];
        written += scan.total;
    }
    if (lane == 0) kept_count = written;
    for (uint position = lane; position < length; position += block_threads_k)
        flags[start + position] = position < written ? 1 : 0;
    threadgroup_barrier(mem_flags::mem_threadgroup);
}

/** The parameters of @c ustore_fold_chains on Apple: CUDA's, then the flags the select reads. */
struct fold_chains_arguments_t {
    ulong records;
    ulong starts;
    uint chains_count;
    fold_spill_t spill;
    ulong kept;
    ulong kept_counts;

    /** One byte per gathered record's place, set where its chain keeps that many records. */
    ulong flags;
};

/** Composes chain @p group and every chain a grid's width past it, and keeps what survives of every
 *  spilled chain, as CUDA's @c fold_chains does, flagging the places of each span it kept. */
inline void fold_chains(constant fold_chains_arguments_t& arguments, blocks_t blocks, uint group, uint groups,
                        uint lane, threadgroup uint* sorted, threadgroup uint* simd_totals) {
    uint const padding = 0xFFFFFFFFu;
    device half_edge_record_t const* const records = reach<half_edge_record_t const>(blocks, arguments.records);
    device uint const* const starts = reach<uint const>(blocks, arguments.starts);
    device uint* const kept = reach<uint>(blocks, arguments.kept);
    device uint* const kept_counts = reach<uint>(blocks, arguments.kept_counts);
    device uchar* const flags = reach<uchar>(blocks, arguments.flags);
    for (uint chain = group; chain < arguments.chains_count; chain += groups) {
        uint const start = starts[chain];
        uint const length = starts[chain + 1] - start;
        if (length == 0) {
            if (lane == 0) kept_counts[chain] = 0;
            continue;
        }
        uint width = 1;
        while (width < length) width *= 2;
        if (width > arguments.spill.shared_width) continue;
        for (uint index = lane; index < width; index += block_threads_k)
            sorted[index] = index < length ? start + index : padding;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        // A bitonic sort, whose every step compares disjoint pairs, so the threadgroup only
        // meets between steps
        for (uint size = 2; size <= width; size *= 2)
            for (uint stride = size / 2; stride; stride /= 2) {
                bitonic_step(records, sorted, width, size, stride, lane);
                threadgroup_barrier(mem_flags::mem_threadgroup);
            }
        keep_sorted_chain(records, sorted, start, length, kept, kept_counts[chain], flags, lane, simd_totals);
    }
    device uint* const spilled = reach<uint>(blocks, arguments.spill.sorted);
    device ulong const* const slot_starts = reach<ulong const>(blocks, arguments.spill.starts);
    device uint const* const slot_chains = reach<uint const>(blocks, arguments.spill.chains);
    for (uint slot = group; slot < arguments.spill.count; slot += groups) {
        uint const chain = slot_chains[slot];
        keep_sorted_chain(records, spilled + slot_starts[slot], starts[chain], starts[chain + 1] - starts[chain], kept,
                          kept_counts[chain], flags, lane, simd_totals);
    }
}

/** The parameters of @c ustore_fold_write on Apple, which takes the select's output where CUDA's
 *  takes a chain. */
struct fold_write_arguments_t {
    ulong records;
    ulong starts;
    ulong kept;
    uint chains_count;

    /** Tiles of the select, whose scanned count after the last is the records kept. */
    uint tiles;

    /** A removal of the vertex as the composed records spell it. */
    half_edge_record_t removal;

    /** The composed records and every chain's first one, its last the records kept. */
    ulong composed;
    ulong offsets;

    /** The place of every kept record in order, the select's output, and its scanned counts. */
    ulong selected;
    ulong counts;
};

/** Writes every kept record at its rank among all kept ones, a removal of the vertex as the
 *  removal spells it, and every chain's first record into the offsets, as CUDA's @c fold_write
 *  does after its chain. */
inline void fold_write(constant fold_write_arguments_t& arguments, blocks_t blocks, uint thread_index, uint threads) {
    device half_edge_record_t const* const records = reach<half_edge_record_t const>(blocks, arguments.records);
    device uint const* const starts = reach<uint const>(blocks, arguments.starts);
    device uint const* const kept = reach<uint const>(blocks, arguments.kept);
    device half_edge_record_t* const composed = reach<half_edge_record_t>(blocks, arguments.composed);
    device ulong* const offsets = reach<ulong>(blocks, arguments.offsets);
    device uint const* const selected = reach<uint const>(blocks, arguments.selected);
    uint const total = uint(reach<ulong const>(blocks, arguments.counts)[arguments.tiles]);
    uint const chains_count = arguments.chains_count;
    half_edge_record_t const removal = arguments.removal;
    for (uint rank = thread_index; rank < total; rank += threads) {
        uint const place = selected[rank];
        // The last chain starting at or below the place holds it, past the empty chains
        // starting there too
        uint chain = 0;
        for (uint high = chains_count; high - chain > 1;) {
            uint const middle = chain + (high - chain) / 2;
            if (starts[middle] <= place) chain = middle;
            else high = middle;
        }
        device half_edge_record_t const& source = records[kept[place]];
        if (record_operation(source) == half_edge_operation_t::remove_vertex_k) composed[rank] = removal;
        else composed[rank] = source;
        // A chain's first kept record names its offset, and every empty chain just before
        // it shares that
        for (uint named = chain + 1; place == starts[chain] && named-- != 0 && starts[named] == starts[chain];)
            offsets[named] = rank;
        // Every chain keeps a record unless it is empty, so the chains past the last kept one are
        for (uint after = chain + 1; rank + 1 == total && after <= chains_count; ++after) offsets[after] = total;
    }
}

#pragma endregion Adjacency Fold

} // namespace ustore
} // namespace unum

#pragma region Merge Kernels

/** Ranks every record of a merge of inline 12 key slots, as CUDA's
 *  @c ustore_merge_rank_inline_12 . */
kernel void ustore_merge_rank_inline_12(constant merge_rank_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                        device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                        constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                        uint thread_index [[thread_position_in_grid]],
                                        uint threads [[threads_per_grid]]) {
    merge_rank<key_slot<key128_t, value_layout_t::inline_12_bytes_k>>(arguments, {table, table_count}, thread_index,
                                                                      threads);
}

/** Packs the survivors of a merge of inline 12 key slots after the select, one
 *  tile per threadgroup. */
kernel void ustore_merge_pack_inline_12(constant merge_pack_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                        device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                        constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                        uint group [[threadgroup_position_in_grid]],
                                        uint lane [[thread_index_in_threadgroup]]) {
    threadgroup ulong simd_totals[block_simds_k];
    merge_pack<key_slot<key128_t, value_layout_t::inline_12_bytes_k>>(arguments, {table, table_count}, group, lane,
                                                                      simd_totals);
}

/** Writes every cell of the new runs of a merge of inline 12 key slots, as CUDA's
 *  @c ustore_merge_place_inline_12 . */
kernel void ustore_merge_place_inline_12(constant merge_place_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                         device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                         constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                         uint thread_index [[thread_position_in_grid]],
                                         uint threads [[threads_per_grid]]) {
    merge_place<key_slot<key128_t, value_layout_t::inline_12_bytes_k>>(arguments, {table, table_count}, thread_index,
                                                                       threads);
}

/** Ranks every record of a merge of inline 44 key slots, as CUDA's
 *  @c ustore_merge_rank_inline_44 . */
kernel void ustore_merge_rank_inline_44(constant merge_rank_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                        device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                        constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                        uint thread_index [[thread_position_in_grid]],
                                        uint threads [[threads_per_grid]]) {
    merge_rank<key_slot<key128_t, value_layout_t::inline_44_bytes_k>>(arguments, {table, table_count}, thread_index,
                                                                      threads);
}

/** Packs the survivors of a merge of inline 44 key slots after the select, one
 *  tile per threadgroup. */
kernel void ustore_merge_pack_inline_44(constant merge_pack_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                        device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                        constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                        uint group [[threadgroup_position_in_grid]],
                                        uint lane [[thread_index_in_threadgroup]]) {
    threadgroup ulong simd_totals[block_simds_k];
    merge_pack<key_slot<key128_t, value_layout_t::inline_44_bytes_k>>(arguments, {table, table_count}, group, lane,
                                                                      simd_totals);
}

/** Writes every cell of the new runs of a merge of inline 44 key slots, as CUDA's
 *  @c ustore_merge_place_inline_44 . */
kernel void ustore_merge_place_inline_44(constant merge_place_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                         device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                         constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                         uint thread_index [[thread_position_in_grid]],
                                         uint threads [[threads_per_grid]]) {
    merge_place<key_slot<key128_t, value_layout_t::inline_44_bytes_k>>(arguments, {table, table_count}, thread_index,
                                                                       threads);
}

/** Ranks every record of a merge of fixed width key slots, as CUDA's
 *  @c ustore_merge_rank_fixed_width . */
kernel void ustore_merge_rank_fixed_width(constant merge_rank_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                          device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                          constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                          uint thread_index [[thread_position_in_grid]],
                                          uint threads [[threads_per_grid]]) {
    merge_rank<key_slot<key128_t, value_layout_t::fixed_width_k>>(arguments, {table, table_count}, thread_index,
                                                                  threads);
}

/** Packs the survivors of a merge of fixed width key slots after the select, one
 *  tile per threadgroup. */
kernel void ustore_merge_pack_fixed_width(constant merge_pack_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                          device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                          constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                          uint group [[threadgroup_position_in_grid]],
                                          uint lane [[thread_index_in_threadgroup]]) {
    threadgroup ulong simd_totals[block_simds_k];
    merge_pack<key_slot<key128_t, value_layout_t::fixed_width_k>>(arguments, {table, table_count}, group, lane,
                                                                  simd_totals);
}

/** Writes every cell of the new runs of a merge of fixed width key slots, as CUDA's
 *  @c ustore_merge_place_fixed_width . */
kernel void ustore_merge_place_fixed_width(constant merge_place_arguments_t& arguments
                                           [[buffer(metal_arguments_slot_k)]],
                                           device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                           constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                           uint thread_index [[thread_position_in_grid]],
                                           uint threads [[threads_per_grid]]) {
    merge_place<key_slot<key128_t, value_layout_t::fixed_width_k>>(arguments, {table, table_count}, thread_index,
                                                                   threads);
}

/** Ranks every record of a merge of variable width key slots, as CUDA's
 *  @c ustore_merge_rank_variable_width . */
kernel void ustore_merge_rank_variable_width(constant merge_rank_arguments_t& arguments
                                             [[buffer(metal_arguments_slot_k)]],
                                             device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                             constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                             uint thread_index [[thread_position_in_grid]],
                                             uint threads [[threads_per_grid]]) {
    merge_rank<key_slot<key128_t, value_layout_t::variable_width_k>>(arguments, {table, table_count}, thread_index,
                                                                     threads);
}

/** Packs the survivors of a merge of variable width key slots after the select, one
 *  tile per threadgroup. */
kernel void ustore_merge_pack_variable_width(constant merge_pack_arguments_t& arguments
                                             [[buffer(metal_arguments_slot_k)]],
                                             device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                             constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                             uint group [[threadgroup_position_in_grid]],
                                             uint lane [[thread_index_in_threadgroup]]) {
    threadgroup ulong simd_totals[block_simds_k];
    merge_pack<key_slot<key128_t, value_layout_t::variable_width_k>>(arguments, {table, table_count}, group, lane,
                                                                     simd_totals);
}

/** Writes every cell of the new runs of a merge of variable width key slots, as CUDA's
 *  @c ustore_merge_place_variable_width . */
kernel void ustore_merge_place_variable_width(constant merge_place_arguments_t& arguments
                                              [[buffer(metal_arguments_slot_k)]],
                                              device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                              constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                              uint thread_index [[thread_position_in_grid]],
                                              uint threads [[threads_per_grid]]) {
    merge_place<key_slot<key128_t, value_layout_t::variable_width_k>>(arguments, {table, table_count}, thread_index,
                                                                      threads);
}

/** Adds the bytes of the tiles before each tile of a pack to its survivors' value offsets, one tile
 *  per threadgroup, which leaves them scanned across the whole merge. */
kernel void ustore_merge_offsets_apple(constant merge_offsets_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                       device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                       constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                       uint group [[threadgroup_position_in_grid]],
                                       uint lane [[thread_index_in_threadgroup]]) {
    threadgroup ulong simd_totals[block_simds_k];
    // The first tile's offsets are scanned from zero already
    if (group == 0) return;
    blocks_t const blocks = {table, table_count};
    device ulong const* const tile_bytes = reach<ulong const>(blocks, arguments.tile_bytes);
    device ulong* const prefix = reach<ulong>(blocks, arguments.prefix);
    device ulong const* const counts = reach<ulong const>(blocks, arguments.counts);
    ulong partial = 0;
    for (uint tile = lane; tile < group; tile += block_threads_k) partial += tile_bytes[tile];
    ulong const before = block_exclusive(partial, lane, simd_totals).total;
    for (ulong into = counts[group] + lane; into < counts[group + 1]; into += block_threads_k)
        prefix[into + 1] += before;
}

/** Gathers the cells of runs of fixed width key slots from the leaf rows of their key trees. */
kernel void ustore_merge_gather_fixed_width(constant merge_gather_arguments_t& arguments
                                            [[buffer(metal_arguments_slot_k)]],
                                            device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                            constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                            uint thread_index [[thread_position_in_grid]],
                                            uint threads [[threads_per_grid]]) {
    merge_gather<key_slot<key128_t, value_layout_t::fixed_width_k>>(arguments, {table, table_count}, thread_index,
                                                                    threads);
}

/** Gathers the cells of runs of variable width key slots from the leaf rows of their key trees. */
kernel void ustore_merge_gather_variable_width(constant merge_gather_arguments_t& arguments
                                               [[buffer(metal_arguments_slot_k)]],
                                               device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                               constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                               uint thread_index [[thread_position_in_grid]],
                                               uint threads [[threads_per_grid]]) {
    merge_gather<key_slot<key128_t, value_layout_t::variable_width_k>>(arguments, {table, table_count}, thread_index,
                                                                       threads);
}

/** Ranks every version of a merge of runs of strings with its survival flag, as CUDA's
 *  @c ustore_merge_strings_rank . */
kernel void ustore_merge_strings_rank(constant merge_strings_rank_arguments_t& arguments
                                      [[buffer(metal_arguments_slot_k)]],
                                      device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                      constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                      uint thread_index [[thread_position_in_grid]],
                                      uint threads [[threads_per_grid]]) {
    merge_strings_rank(arguments, {table, table_count}, thread_index, threads);
}

#pragma endregion Merge Kernels

#pragma region Fold Kernels

/** One step of the bitonic sort of every chain of a graph merge that spills past a threadgroup's
 *  memory, across the grid, as CUDA's @c ustore_fold_sort_step . */
kernel void ustore_fold_sort_step(constant fold_sort_step_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                                  device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                                  constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                                  uint group [[threadgroup_position_in_grid]], uint groups [[threadgroups_per_grid]],
                                  uint lane [[thread_index_in_threadgroup]]) {
    fold_sort_step(arguments, {table, table_count}, group, groups, lane);
}

/** Sorts, keeps, counts and flags the records of every chain of a graph merge, one threadgroup per
 *  chain, sorting in the threadgroup memory the host sizes for the longest chain it fits, and
 *  keeps, counts and flags those of every chain the steps before sorted. */
kernel void ustore_fold_chains(constant fold_chains_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                               device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                               constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                               threadgroup uint* sorted [[threadgroup(metal_shared_slot_k)]],
                               uint group [[threadgroup_position_in_grid]], uint groups [[threadgroups_per_grid]],
                               uint lane [[thread_index_in_threadgroup]]) {
    threadgroup uint simd_totals[block_simds_k];
    fold_chains(arguments, {table, table_count}, group, groups, lane, sorted, simd_totals);
}

/** Writes the kept records of every chain of a graph merge back to back after the select,
 *  one per thread. */
kernel void ustore_fold_write(constant fold_write_arguments_t& arguments [[buffer(metal_arguments_slot_k)]],
                              device ustore_metal_block_t const* table [[buffer(metal_blocks_slot_k)]],
                              constant uint& table_count [[buffer(metal_blocks_count_slot_k)]],
                              uint thread_index [[thread_position_in_grid]], uint threads [[threads_per_grid]]) {
    fold_write(arguments, {table, table_count}, thread_index, threads);
}

#pragma endregion Fold Kernels
