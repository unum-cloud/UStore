/**
 * @file modality_paths.cpp
 * @author Ashot Vardanian
 *
 * @brief Paths (variable length keys) compatibility layer.
 * Sits on top of any @see "ukv.h"-compatible system.
 *
 * For every string key hash we store:
 * - N = number of entries (1 if no collisions appeared)
 * - N key offsets
 * - N value lengths
 * - N concatenated keys
 * - N concatenated values
 *
 * ## Mirror "Directory" Entries for Nested Paths
 *
 * Furthermore, we need to store mirror entries, that will
 * store the directory tree. In other words, for an input
 * like @b home/user/media/name we would keep:
 * - home/: @b home/user
 * - home/user/: @b home/user/media
 * - home/user/media/: @b home/user/media/name
 *
 * The mirror "directory" entries can have negative IDs.
 * Their values would be structured differently.
 */

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "ukv/paths.h"
#include "ukv/cpp/ranges_args.hpp" // `places_arg_t`

#include "helpers/linked_memory.hpp" // `linked_memory_lock_t`
#include "helpers/linked_array.hpp"  // `uninitialized_array_gt`
#include "helpers/algorithm.hpp"     // `sort_and_deduplicate`
#include "helpers/full_scan.hpp"     // `full_scan_collection`

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

using namespace unum::ukv;
using namespace unum;

struct hash_t {
    ukv_key_t operator()(std::string_view key_str) const noexcept {
        using umem_t = std::hash<std::string_view>;
        auto result = umem_t {}(key_str);
#ifdef UKV_DEBUG
        result %= 10ul;
#endif
        return static_cast<ukv_key_t>(result);
    }
};

constexpr std::size_t counter_size_k = sizeof(ukv_length_t);
constexpr std::size_t bytes_in_header_k = counter_size_k;

ukv_length_t get_bucket_size(value_view_t bucket) noexcept {
    auto lengths = reinterpret_cast<ukv_length_t const*>(bucket.data());
    return bucket.size() > bytes_in_header_k ? *lengths : 0u;
}

ptr_range_gt<ukv_length_t const> get_bucket_counters(value_view_t bucket, ukv_length_t size) noexcept {
    auto lengths = reinterpret_cast<ukv_length_t const*>(bucket.data());
    return {lengths, lengths + size * 2u + 1u};
}

consecutive_strs_iterator_t get_bucket_keys(value_view_t bucket, ukv_length_t size) noexcept {
    auto lengths = reinterpret_cast<ukv_length_t const*>(bucket.data());
    auto bytes_for_counters = size * 2u * counter_size_k;
    return {lengths + 1u, bucket.data() + bytes_in_header_k + bytes_for_counters};
}

consecutive_blobs_iterator_t get_bucket_vals(value_view_t bucket, ukv_length_t size) noexcept {
    auto lengths = reinterpret_cast<ukv_length_t const*>(bucket.data());
    auto bytes_for_counters = size * 2u * counter_size_k;
    auto bytes_for_keys = std::accumulate(lengths + 1u, lengths + 1u + size, 0ul);
    return {lengths + 1u + size, bucket.data() + bytes_in_header_k + bytes_for_counters + bytes_for_keys};
}

struct bucket_member_t {
    std::size_t idx = 0;
    std::string_view key;
    value_view_t value;

    operator bool() const noexcept { return value; }
};

template <typename bucket_member_callback_at>
void for_each_in_bucket(value_view_t bucket, bucket_member_callback_at member_callback) noexcept {
    auto bucket_size = get_bucket_size(bucket);
    if (!bucket_size)
        return;
    auto bucket_keys = get_bucket_keys(bucket, bucket_size);
    auto bucket_vals = get_bucket_vals(bucket, bucket_size);
    for (std::size_t i = 0; i != bucket_size; ++i, ++bucket_keys, ++bucket_vals)
        member_callback(bucket_member_t {i, *bucket_keys, *bucket_vals});
}

bucket_member_t find_in_bucket(value_view_t bucket, std::string_view key_str) noexcept {
    bucket_member_t result;
    for_each_in_bucket(bucket, [&](bucket_member_t const& member) {
        if (member.key == key_str)
            result = member;
    });
    return result;
}

bool starts_with(std::string_view str, std::string_view prefix) noexcept {
    return str.size() >= prefix.size() && str.substr(0, prefix.size()) == prefix;
}

std::size_t path_segments_counts(std::string_view key_str, ukv_char_t const c_separator) noexcept {
    return 0;
}

template <typename keys_callback_at>
void path_segments_enumerate(std::string_view key_str) noexcept {
}

bool is_prefix(std::string_view prefix_or_pattern) noexcept {
    return std::all_of(prefix_or_pattern.begin(), prefix_or_pattern.end(), [](char c) {
        // https://www3.ntu.edu.sg/home/ehchua/programming/howto/Regexe.html
        switch (c) {
        case '.':  // any character
        case '+':  // one or more
        case '*':  // zero or more
        case '?':  // zero or one ~ optional
        case '^':  // (not one of) | (start of line)
        case '$':  // end of line
        case '(':  // back-reference start
        case ')':  // back-reference end
        case '[':  // character class start
        case ']':  // character class end
        case '{':  // repetitions count start
        case '}':  // repetitions count end
        case '|':  // binary OR
        case '\\': // escape character
            return false;
        default: //
            return true;
        }
    });
}

/**
 * @brief Removes part of variable length string.
 * @return The shortened view of the input. Will start from same address.
 */
value_view_t remove_part(value_view_t full, value_view_t part) noexcept {
    auto removed_length = part.size();
    auto moved_length = full.size() - part.size();
    std::memmove((void*)part.begin(), (void*)part.end(), moved_length);
    return {full.begin(), full.size() - removed_length};
}

void remove_from_bucket(value_view_t& bucket, std::string_view key_str) noexcept {
    // If the entry was present, it must be clamped.
    // Matching key and length entry will be removed.
    auto [old_idx, old_key, old_val] = find_in_bucket(bucket, key_str);
    if (!old_val)
        return;

    // Most of the time slots contain just one entry
    auto old_size = get_bucket_size(bucket);
    if (old_size == 1) {
        bucket = {};
        return;
    }

    bucket = remove_part(bucket, old_val);
    bucket = remove_part(bucket, old_key);

    // Remove the value counter
    auto begin = bucket.data();
    value_view_t value_length_bytes {begin + counter_size_k * (old_size + old_idx + 1u), counter_size_k};
    bucket = remove_part(bucket, value_length_bytes);
    value_view_t key_length_bytes {begin + counter_size_k * (old_idx + 1u), counter_size_k};
    bucket = remove_part(bucket, key_length_bytes);

    // Decrement the size
    auto lengths = (ukv_length_t*)begin;
    lengths[0] -= 1u;
}

void upsert_in_bucket( //
    value_view_t& bucket,
    std::string_view key,
    value_view_t val,
    linked_memory_lock_t& arena,
    ukv_error_t* c_error) noexcept {

    auto old_size = get_bucket_size(bucket);
    auto old_bytes_for_counters = old_size * 2u * counter_size_k;
    auto old_lengths = reinterpret_cast<ukv_length_t const*>(bucket.data());
    auto old_bytes_for_keys = bucket ? std::accumulate(old_lengths + 1u, old_lengths + 1u + old_size, 0ul) : 0ul;
    auto old_bytes_for_vals =
        bucket ? std::accumulate(old_lengths + 1u + old_size, old_lengths + 1u + old_size * 2ul, 0ul) : 0ul;
    auto [old_idx, old_key, old_val] = find_in_bucket(bucket, key);
    bool is_missing = !old_val;

    auto new_size = old_size + is_missing;
    auto new_bytes_for_counters = new_size * 2u * counter_size_k;
    auto new_bytes_for_keys = old_bytes_for_keys - old_key.size() + key.size();
    auto new_bytes_for_vals = old_bytes_for_vals - old_val.size() + val.size();
    auto new_bytes = bytes_in_header_k + new_bytes_for_counters + new_bytes_for_keys + new_bytes_for_vals;

    auto new_begin = arena.alloc<byte_t>(new_bytes, c_error).begin();
    return_if_error_m(c_error);
    auto new_lengths = reinterpret_cast<ukv_length_t*>(new_begin);
    new_lengths[0] = new_size;
    auto new_keys_lengths = new_lengths + 1ul;
    auto new_vals_lengths = new_lengths + 1ul + new_size;
    auto new_keys_output = new_begin + bytes_in_header_k + new_bytes_for_counters;
    auto new_vals_output = new_begin + bytes_in_header_k + new_bytes_for_counters + new_bytes_for_keys;

    auto old_keys = get_bucket_keys(bucket, old_size);
    auto old_vals = get_bucket_vals(bucket, old_size);
    std::size_t new_idx = 0;
    for (std::size_t i = 0; i != old_size; ++i, ++old_keys, ++old_vals) {
        if (!is_missing && i == old_idx)
            continue;

        value_view_t old_key = *old_keys;
        value_view_t old_val = *old_vals;
        new_keys_lengths[new_idx] = static_cast<ukv_length_t>(old_key.size());
        new_vals_lengths[new_idx] = static_cast<ukv_length_t>(old_val.size());
        std::memcpy(new_keys_output, old_key.data(), old_key.size());
        std::memcpy(new_vals_output, old_val.data(), old_val.size());

        new_keys_output += old_key.size();
        new_vals_output += old_val.size();
        ++new_idx;
    }

    // Append the new entry at the end
    new_keys_lengths[new_idx] = static_cast<ukv_length_t>(key.size());
    new_vals_lengths[new_idx] = static_cast<ukv_length_t>(val.size());
    std::memcpy(new_keys_output, key.data(), key.size());
    std::memcpy(new_vals_output, val.data(), val.size());

    bucket = {new_begin, new_bytes};
}

void ukv_paths_write(ukv_paths_write_t* c_ptr) {

    ukv_paths_write_t& c = *c_ptr;
    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    contents_arg_t keys_str_args;
    keys_str_args.offsets_begin = {c.paths_offsets, c.paths_offsets_stride};
    keys_str_args.lengths_begin = {c.paths_lengths, c.paths_lengths_stride};
    keys_str_args.contents_begin = {(ukv_bytes_cptr_t const*)c.paths, c.paths_stride};
    keys_str_args.count = c.tasks_count;

    auto unique_col_keys = arena.alloc<collection_key_t>(c.tasks_count, c.error);
    return_if_error_m(c.error);

    // Parse and hash input string unique_col_keys
    hash_t hash;
    strided_iterator_gt<ukv_collection_t const> collections {c.collections, c.collections_stride};
    for (std::size_t i = 0; i != c.tasks_count; ++i)
        unique_col_keys[i] = {collections ? collections[i] : ukv_collection_main_k, hash(keys_str_args[i])};

    // We must sort and deduplicate this bucket IDs
    unique_col_keys = {unique_col_keys.begin(), sort_and_deduplicate(unique_col_keys.begin(), unique_col_keys.end())};

    // Read from disk
    // We don't need:
    // > presences: zero length buckets are impossible here.
    // > lengths: value lengths are always smaller than buckets.
    // We can infer those and export differently.
    ukv_length_t* buckets_offsets = nullptr;
    ukv_byte_t* buckets_values = nullptr;
    places_arg_t unique_places;
    auto unique_col_keys_strided = strided_range(unique_col_keys.begin(), unique_col_keys.end()).immutable();
    unique_places.collections_begin = unique_col_keys_strided.members(&collection_key_t::collection).begin();
    unique_places.keys_begin = unique_col_keys_strided.members(&collection_key_t::key).begin();
    unique_places.fields_begin = {};
    unique_places.count = static_cast<ukv_size_t>(unique_col_keys.size());
    auto opts = c.transaction ? ukv_options_t(c.options & ~ukv_option_transaction_dont_watch_k) : c.options;
    ukv_read_t read {
        .db = c.db,
        .error = c.error,
        .transaction = c.transaction,
        .arena = c.arena,
        .options = opts,
        .tasks_count = unique_places.count,
        .collections = unique_places.collections_begin.get(),
        .collections_stride = unique_places.collections_begin.stride(),
        .keys = unique_places.keys_begin.get(),
        .keys_stride = unique_places.keys_begin.stride(),
        .offsets = &buckets_offsets,
        .values = &buckets_values,
    };

    ukv_read(&read);
    return_if_error_m(c.error);

    joined_blobs_t joined_buckets {unique_places.count, buckets_offsets, buckets_values};
    uninitialized_array_gt<value_view_t> updated_buckets(unique_places.count, arena, c.error);
    return_if_error_m(c.error);
    transform_n(joined_buckets.begin(), unique_places.count, updated_buckets.begin());

    bits_view_t presences {c.values_presences};
    strided_iterator_gt<ukv_length_t const> offs {c.values_offsets, c.values_offsets_stride};
    strided_iterator_gt<ukv_length_t const> lens {c.values_lengths, c.values_lengths_stride};
    strided_iterator_gt<ukv_bytes_cptr_t const> vals {c.values_bytes, c.values_bytes_stride};
    contents_arg_t contents {presences, offs, lens, vals, c.tasks_count};

    // Update every unique bucket
    for (std::size_t i = 0; i != c.tasks_count; ++i) {
        std::string_view key_str = keys_str_args[i];
        ukv_key_t key = hash(key_str);
        value_view_t new_val = contents[i];
        collection_key_t collection_key {collections ? collections[i] : ukv_collection_main_k, key};
        auto bucket_idx = offset_in_sorted(unique_col_keys, collection_key);
        value_view_t& bucket = updated_buckets[bucket_idx];

        if (new_val) {
            upsert_in_bucket(bucket, key_str, new_val, arena, c.error);
            return_if_error_m(c.error);
        }
        else
            remove_from_bucket(bucket, key_str);
    }

    ukv_write_t write {
        .db = c.db,
        .error = c.error,
        .transaction = c.transaction,
        .arena = arena,
        .options = opts,
        .tasks_count = unique_places.count,
        .collections = unique_places.collections_begin.get(),
        .collections_stride = unique_places.collections_begin.stride(),
        .keys = unique_places.keys_begin.get(),
        .keys_stride = unique_places.keys_begin.stride(),
        .lengths = updated_buckets[0].member_length(),
        .lengths_stride = sizeof(value_view_t),
        .values = updated_buckets[0].member_ptr(),
        .values_stride = sizeof(value_view_t),
    };

    // Once all is updated, we can safely write back
    ukv_write(&write);
}

void ukv_paths_read(ukv_paths_read_t* c_ptr) {

    ukv_paths_read_t& c = *c_ptr;
    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    contents_arg_t keys_str_args;
    keys_str_args.offsets_begin = {c.paths_offsets, c.paths_offsets_stride};
    keys_str_args.lengths_begin = {c.paths_lengths, c.paths_lengths_stride};
    keys_str_args.contents_begin = {(ukv_bytes_cptr_t const*)c.paths, c.paths_stride};
    keys_str_args.count = c.tasks_count;

    // Getting hash-collisions is such a rare case, that we will not
    // optimize for it in the current implementation. Sorting and
    // deduplicating the IDs will cost more overall, than a repeated
    // read every once in a while.
    auto buckets_keys = arena.alloc<ukv_key_t>(c.tasks_count, c.error);
    return_if_error_m(c.error);

    // Parse and hash input string buckets_keys
    hash_t hash;
    for (std::size_t i = 0; i != c.tasks_count; ++i)
        buckets_keys[i] = hash(keys_str_args[i]);

    // Read from disk
    // We don't need:
    // > presences: zero length buckets are impossible here.
    // > lengths: value lengths are always smaller than buckets.
    // We can infer those and export differently.
    ukv_length_t* buckets_offsets = nullptr;
    ukv_byte_t* buckets_values = nullptr;
    ukv_read_t read {
        .db = c.db,
        .error = c.error,
        .transaction = c.transaction,
        .arena = arena,
        .options = c.options,
        .tasks_count = c.tasks_count,
        .collections = c.collections,
        .collections_stride = c.collections_stride,
        .keys = buckets_keys.begin(),
        .keys_stride = sizeof(ukv_key_t),
        .offsets = &buckets_offsets,
        .values = &buckets_values,
    };

    ukv_read(&read);
    return_if_error_m(c.error);

    // Some of the entries will contain more then one key-value pair in case of collisions.
    ukv_length_t exported_volume = 0;
    joined_blobs_t buckets {c.tasks_count, buckets_offsets, buckets_values};
    auto presences =
        arena.alloc_or_dummy(divide_round_up<std::size_t>(c.tasks_count, bits_in_byte_k), c.error, c.presences);
    auto lengths = arena.alloc_or_dummy(c.tasks_count, c.error, c.lengths);
    auto offsets = arena.alloc_or_dummy(c.tasks_count, c.error, c.offsets);

    for (std::size_t i = 0; i != c.tasks_count; ++i) {
        std::string_view key_str = keys_str_args[i];
        value_view_t bucket = buckets[i];

        // Now that we have found our match - clamp everything else.
        value_view_t val = find_in_bucket(bucket, key_str).value;
        if (val) {
            presences[i] = true;
            offsets[i] = exported_volume;
            lengths[i] = static_cast<ukv_length_t>(val.size());
            if (c.values)
                std::memmove(buckets_values + exported_volume, val.data(), val.size());
            buckets_values[exported_volume + val.size()] = ukv_byte_t {0};
            exported_volume += static_cast<ukv_length_t>(val.size()) + 1;
        }
        else {
            presences[i] = false;
            offsets[i] = exported_volume;
            lengths[i] = ukv_length_missing_k;
        }
    }

    offsets[c.tasks_count] = exported_volume;
    if (c.values)
        *c.values = buckets_values;
}

/**
 * - Same collection
 * - One scan request
 * - May have previous results
 */
template <typename predicate_at>
void full_scan_collection_w_predicate( //
    ukv_database_t c_db,
    ukv_transaction_t c_transaction,
    ukv_collection_t c_collection,
    std::string_view previous_path,
    ukv_length_t c_count_limit,
    ukv_options_t c_options,
    ukv_length_t& paths_count,
    growing_tape_t& paths,
    linked_memory_lock_t& arena,
    ukv_error_t* c_error,
    predicate_at predicate) {

    hash_t hash;
    bool has_reached_previous = previous_path.empty();
    ukv_key_t start_key = !previous_path.empty() ? hash(previous_path) : std::numeric_limits<ukv_key_t>::min();

    paths_count = 0;
    auto scan_in_bucket = [&](ukv_key_t, value_view_t bucket) noexcept {
        for_each_in_bucket(bucket, [&](bucket_member_t const& member) {
            if (!predicate(member.key))
                // Skip irrelevant entries
                return;
            if (member.key == previous_path) {
                // We may have reached the boundary between old results and new ones
                has_reached_previous = true;
                return;
            }
            if (!has_reached_previous)
                // Skip the results we have already seen
                return;
            if (paths_count >= c_count_limit)
                // We have more than we need
                return;

            // All the matches in this section should be exported
            paths.push_back(member.key, c_error);
            return_if_error_m(c_error);
            paths.add_terminator(byte_t {0}, c_error);
            return_if_error_m(c_error);
            ++paths_count;
        });

        return paths_count < c_count_limit;
    };

    full_scan_collection(c_db,
                         c_transaction,
                         c_collection,
                         c_options,
                         start_key,
                         c_count_limit,
                         arena,
                         c_error,
                         scan_in_bucket);
}

void full_scan_w_prefix( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_transaction,
    ukv_collection_t c_collection,
    std::string_view prefix,
    std::string_view previous_path,
    ukv_length_t c_count_limit,
    ukv_options_t const c_options,
    ukv_length_t& count,
    growing_tape_t& paths,
    linked_memory_lock_t& arena,
    ukv_error_t* c_error) {

    full_scan_collection_w_predicate( //
        c_db,
        c_transaction,
        c_collection,
        previous_path,
        c_count_limit,
        c_options,
        count,
        paths,
        arena,
        c_error,
        [=](std::string_view body) { return starts_with(body, prefix); });
}

struct pcre2_ctx_t {
    linked_memory_lock_t& arena;
    ukv_error_t* c_error;
};

static void* pcre2_malloc(PCRE2_SIZE length, void* ctx_ptr) noexcept {
    pcre2_ctx_t& ctx = *reinterpret_cast<pcre2_ctx_t*>(ctx_ptr);
    return ctx.arena.alloc<byte_t>(static_cast<std::size_t>(length), ctx.c_error).begin();
}

static void pcre2_free(void*, void*) noexcept {
    // Our arenas only grow, we don't dealloc!
}

void full_scan_w_regex( //
    ukv_database_t const c_db,
    ukv_transaction_t const c_transaction,
    ukv_collection_t c_collection,
    std::string_view pattern,
    std::string_view previous_path,
    ukv_length_t c_count_limit,
    ukv_options_t const c_options,
    ukv_length_t& count,
    growing_tape_t& paths,
    linked_memory_lock_t& arena,
    ukv_error_t* c_error) {

    pcre2_ctx_t ctx {arena, c_error};

    // https://www.pcre.org/current/doc/html/pcre2_compile.html
    pcre2_general_context* pcre2_context = pcre2_general_context_create(&pcre2_malloc, &pcre2_free, &ctx);
    pcre2_compile_context* pcre2_compile_context = pcre2_compile_context_create(pcre2_context);
    int pcre2_pattern_error_code = 0;
    PCRE2_SIZE pcre2_pattern_error_offset = 0;
    pcre2_code* pcre2_code = pcre2_compile( //
        PCRE2_SPTR8(pattern.data()),
        PCRE2_SIZE(pattern.size()),
        PCRE2_MATCH_INVALID_UTF,
        &pcre2_pattern_error_code,
        &pcre2_pattern_error_offset,
        pcre2_compile_context);

    // https://www.pcre.org/current/doc/html/pcre2_jit_compile.html
    auto jit_status = pcre2_jit_compile(pcre2_code, PCRE2_JIT_COMPLETE);
    if (jit_status != 0)
        *c_error = "Failed to JIT-compile the RegEx query";

    pcre2_match_data* match_data = pcre2_match_data_create_from_pattern(pcre2_code, pcre2_context);
    if (!match_data)
        *c_error = "Failed to allocate memory for RegEx pattern matches";

    if (!*c_error)
        full_scan_collection_w_predicate( //
            c_db,
            c_transaction,
            c_collection,
            previous_path,
            c_count_limit,
            c_options,
            count,
            paths,
            arena,
            c_error,
            [=](std::string_view body) {
                // https://www.pcre.org/current/doc/html/pcre2_jit_match.html
                // pcre2_match_data match_data;
                // pcre2_match_context match_context;
                auto found_matches = pcre2_jit_match( //
                    pcre2_code,
                    PCRE2_SPTR(body.data()),
                    PCRE2_SIZE(body.size()),
                    PCRE2_SIZE(0), // start offset
                    PCRE2_NO_UTF_CHECK,
                    match_data,
                    NULL);
                return found_matches > 0;
            });

    pcre2_match_data_free(match_data);
    pcre2_code_free(pcre2_code);
    pcre2_compile_context_free(pcre2_compile_context);
    pcre2_general_context_free(pcre2_context);
}

void ukv_paths_match(ukv_paths_match_t* c_ptr) {

    ukv_paths_match_t const& c = *c_ptr;
    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    contents_arg_t patterns_args;
    patterns_args.offsets_begin = {c.patterns_offsets, c.patterns_offsets_stride};
    patterns_args.lengths_begin = {c.patterns_lengths, c.patterns_lengths_stride};
    patterns_args.contents_begin = {(ukv_bytes_cptr_t const*)c.patterns, c.patterns_stride};
    patterns_args.count = c.tasks_count;

    contents_arg_t previous_args;
    previous_args.offsets_begin = {c.previous_offsets, c.previous_offsets_stride};
    previous_args.lengths_begin = {c.previous_lengths, c.previous_lengths_stride};
    previous_args.contents_begin = {(ukv_bytes_cptr_t const*)c.previous, c.previous_stride};
    previous_args.count = c.tasks_count;

    strided_range_gt<ukv_collection_t const> collections {{c.collections, c.collections_stride}, c.tasks_count};
    strided_range_gt<ukv_length_t const> count_limits {{c.match_counts_limits, c.match_counts_limits_stride},
                                                       c.tasks_count};

    auto count_limits_sum = transform_reduce_n(count_limits.begin(), c.tasks_count, 0ul);
    auto found_counts = arena.alloc<ukv_length_t>(c.tasks_count, c.error);
    auto found_paths = growing_tape_t(arena);
    found_paths.reserve(count_limits_sum, c.error);
    return_if_error_m(c.error);

    for (std::size_t i = 0; i != c.tasks_count && !*c.error; ++i) {
        auto col = collections ? collections[i] : ukv_collection_main_k;
        auto pattern = patterns_args[i];
        auto previous = previous_args[i];
        auto limit = count_limits[i];
        auto func = is_prefix(pattern) ? &full_scan_w_prefix : &full_scan_w_regex;
        func(c.db,
             c.transaction,
             col,
             pattern,
             previous,
             limit,
             c.options,
             found_counts[i],
             found_paths,
             arena,
             c.error);
    }

    // Export the results
    if (c.match_counts)
        *c.match_counts = found_counts.begin();
    if (c.paths_offsets)
        *c.paths_offsets = found_paths.offsets().begin().get();
    if (c.paths_strings)
        *c.paths_strings = (ukv_char_t*)found_paths.contents().begin().get();
}