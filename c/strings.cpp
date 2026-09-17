/**
 *  @file c/strings.cpp
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief The strings modality over any engine, mapping variable-length strings onto arbitrary bytes.
 *
 *  A string becomes a key of its leading bytes and its length, and the value under that key is a bucket:
 *  a sorted run of entries, each a string and its value, binary-searched by text. A string short enough to
 *  fit the key owns its bucket alone, while longer ones sharing a leading run share a bucket, so both a
 *  point lookup and a prefix walk reach the engine as an ordered walk of keys.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::strlen`

#include <algorithm>   // `std::ranges::sort`, `std::min`
#include <compare>     // `std::strong_ordering`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::make_unsigned_t`

#include <ustore/ranges_args.hpp> // `contents_arg_t`
#include <ustore/strings.h>       // `ustore_strings_write_t`, `ustore_strings_read_t`
#include <ustore/ustore.hpp>      // `ustore_read`, `strided_iterator`, `joined_blobs_t`

#include "helpers/algorithm.hpp"     // `offset_in_sorted`
#include "helpers/linked_array.hpp"  // `growing_tape_t`, `uninitialized_array`
#include "helpers/linked_memory.hpp" // `linked_memory_lock_t`

namespace unum::ustore {

#pragma region Key Encoding

/** The unsigned twin of a key, in which the encoded bytes are assembled before they take their sign. */
using unsigned_key_t = std::make_unsigned_t<ustore_key_t>;

/** Bytes of a key that hold the leading characters of a string, leaving the last one for its length. */
constexpr std::size_t prefix_bytes_k = sizeof(ustore_key_t) - 1;

/** The length a key carries for a string too long to fit @c prefix_bytes_k, which shares a bucket. */
constexpr unsigned char bucketed_length_k = static_cast<unsigned char>(prefix_bytes_k + 1);

/** The bit whose flip makes an unsigned byte order match the signed order of the keys the engines compare. */
constexpr unsigned_key_t sign_bit_k = unsigned_key_t {1} << (sizeof(ustore_key_t) * 8 - 1);

/** The byte at @p index of @p text, or the padding a shorter string takes. */
constexpr unsigned char padded_byte(std::string_view text, std::size_t index) noexcept {
    return index < text.size() ? static_cast<unsigned char>(text[index]) : static_cast<unsigned char>(0);
}

/** The key of a string whose leading bytes are @p text and whose length byte is @p length_byte. */
constexpr ustore_key_t key_of_bytes(std::string_view text, unsigned char length_byte) noexcept {
    unsigned_key_t word = 0;
    for (std::size_t index = 0; index != prefix_bytes_k; ++index)
        word = static_cast<unsigned_key_t>((word << 8) | padded_byte(text, index));
    word = static_cast<unsigned_key_t>((word << 8) | length_byte);
    return static_cast<ustore_key_t>(word ^ sign_bit_k);
}

/** The key @p text is stored under, which orders exactly as the text of every string that fits a key. */
constexpr ustore_key_t key_of(std::string_view text) noexcept {
    unsigned char const length_byte = text.size() <= prefix_bytes_k ? static_cast<unsigned char>(text.size())
                                                                    : bucketed_length_k;
    return key_of_bytes(text, length_byte);
}

/** Whether a string of @p length shares its key with every other string of the same leading bytes. */
constexpr bool shares_a_bucket(std::size_t length) noexcept { return length > prefix_bytes_k; }

/** The first and the last key a walk of one prefix may touch, both included. */
struct key_window_t {

    /** The smallest key a string with the prefix can take. */
    ustore_key_t first;

    /** The largest key a string with the prefix can take. */
    ustore_key_t last;
};

/** The keys a walk of @p prefix covers, which is one bucket for a prefix longer than a key holds. */
constexpr key_window_t window_of(std::string_view prefix) noexcept {
    if (shares_a_bucket(prefix.size())) {
        ustore_key_t const bucket = key_of(prefix);
        return {bucket, bucket};
    }

    // Every length byte a stored string takes stays below the padding of the upper bound, so it closes the window
    unsigned_key_t highest = 0;
    for (std::size_t index = 0; index != prefix_bytes_k; ++index) {
        unsigned char const character = index < prefix.size() ? static_cast<unsigned char>(prefix[index])
                                                              : static_cast<unsigned char>(0xFF);
        highest = static_cast<unsigned_key_t>((highest << 8) | character);
    }
    highest = static_cast<unsigned_key_t>((highest << 8) | 0xFF);
    return {key_of(prefix), static_cast<ustore_key_t>(highest ^ sign_bit_k)};
}

#pragma endregion Key Encoding

#pragma region Bucket Layout

/** Bytes of the entry count that leads every bucket. */
constexpr std::size_t bucket_header_bytes_k = sizeof(std::uint32_t);

/** Reads the little-endian length sitting @p offset bytes into @p bytes. */
inline std::uint32_t read_length(ustore_bytes_cptr_t bytes, std::size_t offset) noexcept {
    std::uint32_t length = 0;
    std::memcpy(&length, bytes + offset, sizeof(length));
    return length;
}

/** Writes @p length as a little-endian length @p offset bytes into @p bytes. */
inline void write_length(ustore_byte_t* bytes, std::size_t offset, std::uint32_t length) noexcept {
    std::memcpy(bytes + offset, &length, sizeof(length));
}

/** One string of a bucket together with the value stored under it. */
struct bucket_entry_t {

    /** The string itself, whose bytes may hold zeros. */
    std::string_view text;

    /** The value stored under the string. */
    value_view_t value;
};

/**
 *  @brief A sorted run of entries inside one stored value, reached through @c std::memcpy over its bytes.
 *
 *  The value leads with the number of entries, then one offset per entry plus a closing one for the strings
 *  and the same for the values, then the strings and the values themselves. Nothing inside is aligned, so
 *  every length is copied out rather than read through a pointer into the tape.
 */
class bucket_view_t {

    ustore_bytes_cptr_t bytes_ {nullptr};
    std::uint32_t count_ {0};

  public:
    bucket_view_t() = default;

    explicit bucket_view_t(value_view_t value) noexcept {
        if (value.size() < bucket_header_bytes_k) return;
        auto const bytes = reinterpret_cast<ustore_bytes_cptr_t>(value.data());
        std::uint32_t const count = read_length(bytes, 0);
        if (value.size() < bucket_header_bytes_k + 2 * sizeof(std::uint32_t) * (std::size_t {count} + 1)) return;
        bytes_ = bytes;
        count_ = count;
    }

    /** Entries the bucket holds. */
    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }

    /** The string of the entry at @p index. */
    std::string_view text_of(std::size_t index) const noexcept {
        std::size_t const begin = read_length(bytes_, strings_offsets_begin_() + index * sizeof(std::uint32_t));
        std::size_t const end = read_length(bytes_, strings_offsets_begin_() + (index + 1) * sizeof(std::uint32_t));
        auto const characters = reinterpret_cast<char const*>(bytes_ + strings_begin_());
        return {characters + begin, end - begin};
    }

    /** The value of the entry at @p index. */
    value_view_t value_of(std::size_t index) const noexcept {
        std::size_t const begin = read_length(bytes_, values_offsets_begin_() + index * sizeof(std::uint32_t));
        std::size_t const end = read_length(bytes_, values_offsets_begin_() + (index + 1) * sizeof(std::uint32_t));
        return {bytes_ + values_begin_() + begin, static_cast<ustore_length_t>(end - begin)};
    }

    /** The entry at @p index. */
    bucket_entry_t operator[](std::size_t index) const noexcept { return {text_of(index), value_of(index)}; }

    /** The position of the first entry whose string is not below @p wanted. */
    std::size_t lower_bound(std::string_view wanted) const noexcept {
        std::size_t low_index = 0;
        std::size_t high_index = count_;
        while (low_index != high_index) {
            std::size_t const middle = low_index + (high_index - low_index) / 2;
            if (text_of(middle) < wanted) low_index = middle + 1;
            else high_index = middle;
        }
        return low_index;
    }

    /** The position of @p wanted, or the size of the bucket when it holds no such string. */
    std::size_t find(std::string_view wanted) const noexcept {
        std::size_t const position = lower_bound(wanted);
        if (position != count_ && text_of(position) == wanted) return position;
        return count_;
    }

  private:

    /** Where the offsets of the strings start. */
    std::size_t strings_offsets_begin_() const noexcept { return bucket_header_bytes_k; }

    /** Where the offsets of the values start. */
    std::size_t values_offsets_begin_() const noexcept {
        return bucket_header_bytes_k + sizeof(std::uint32_t) * (std::size_t {count_} + 1);
    }

    /** Where the strings themselves start. */
    std::size_t strings_begin_() const noexcept {
        return bucket_header_bytes_k + 2 * sizeof(std::uint32_t) * (std::size_t {count_} + 1);
    }

    /** Where the values themselves start. */
    std::size_t values_begin_() const noexcept {
        return strings_begin_() + read_length(bytes_, strings_offsets_begin_() + count_ * sizeof(std::uint32_t));
    }
};

/** The bytes a bucket of @p entries takes once written. */
inline std::size_t bucket_bytes(std::span<bucket_entry_t const> entries) noexcept {
    std::size_t bytes = bucket_header_bytes_k + 2 * sizeof(std::uint32_t) * (entries.size() + 1);
    for (bucket_entry_t const& entry : entries) bytes += entry.text.size() + entry.value.size();
    return bytes;
}

/** Writes @p entries, already sorted by their strings, into @p bytes, which has room for them. */
inline void write_bucket(ustore_byte_t* bytes, std::span<bucket_entry_t const> entries) noexcept {
    std::size_t const count = entries.size();
    write_length(bytes, 0, static_cast<std::uint32_t>(count));

    std::size_t const strings_offsets_begin = bucket_header_bytes_k;
    std::size_t const values_offsets_begin = bucket_header_bytes_k + sizeof(std::uint32_t) * (count + 1);
    std::size_t const strings_begin = bucket_header_bytes_k + 2 * sizeof(std::uint32_t) * (count + 1);

    std::size_t strings_bytes = 0;
    std::size_t values_bytes = 0;
    for (std::size_t index = 0; index != count; ++index) {
        write_length(bytes, strings_offsets_begin + index * sizeof(std::uint32_t),
                     static_cast<std::uint32_t>(strings_bytes));
        write_length(bytes, values_offsets_begin + index * sizeof(std::uint32_t),
                     static_cast<std::uint32_t>(values_bytes));
        strings_bytes += entries[index].text.size();
        values_bytes += entries[index].value.size();
    }
    write_length(bytes, strings_offsets_begin + count * sizeof(std::uint32_t),
                 static_cast<std::uint32_t>(strings_bytes));
    write_length(bytes, values_offsets_begin + count * sizeof(std::uint32_t), static_cast<std::uint32_t>(values_bytes));

    std::size_t const values_begin = strings_begin + strings_bytes;
    std::size_t written_strings = 0;
    std::size_t written_values = 0;
    for (bucket_entry_t const& entry : entries) {
        if (!entry.text.empty())
            std::memcpy(bytes + strings_begin + written_strings, entry.text.data(), entry.text.size());
        if (entry.value.size() != 0)
            std::memcpy(bytes + values_begin + written_values, entry.value.data(), entry.value.size());
        written_strings += entry.text.size();
        written_values += entry.value.size();
    }
}

#pragma endregion Bucket Layout

#pragma region Argument Views

/** The strings of one batch, as the C ABI receives them. */
struct strings_arg_t {

    /** Pointers to the strings. */
    strided_iterator<ustore_str_view_t const> texts;

    /** Optional lengths of the strings, where a null iterator means they are null-terminated. */
    strided_iterator<ustore_length_t const> lengths;

    /** The string of task @p index. */
    std::string_view operator[](std::size_t index) const noexcept {
        auto const signed_index = static_cast<std::ptrdiff_t>(index);
        ustore_str_view_t const text = texts[signed_index];
        if (!text) return {};
        std::size_t const length = lengths ? lengths[signed_index] : std::strlen(text);
        return {text, length};
    }
};

/** One bucket a batch reads or rewrites, addressed by its collection and key. */
struct touched_bucket_t : public collection_key_t {

    /** Bytes of the rewritten bucket, or @c nullptr when nothing was written. */
    ustore_bytes_cptr_t content {nullptr};

    /** Length of the rewritten bucket, or @c ustore_length_missing_k to remove it. */
    ustore_length_t length {ustore_length_missing_k};
};

/** One task of a batched write, ordered so the tasks of one bucket sit together. */
struct write_task_t {

    /** Collection owning the string. */
    ustore_collection_t collection {ustore_collection_main_k};

    /** Key the string is stored under. */
    ustore_key_t key {0};

    /** The string itself. */
    std::string_view text;

    /** The value written under the string, missing when the task removes it. */
    value_view_t value;

    /** Where the task sat in the batch, so the last write of one string wins. */
    ustore_length_t task_index {0};

    friend std::strong_ordering operator<=>(write_task_t const& first, write_task_t const& second) noexcept {
        if (auto const collections = first.collection <=> second.collection; collections != 0) return collections;
        if (auto const keys = first.key <=> second.key; keys != 0) return keys;
        if (auto const texts = first.text <=> second.text; texts != 0) return texts;
        return first.task_index <=> second.task_index;
    }

    friend bool operator==(write_task_t const& first, write_task_t const& second) noexcept {
        return (first <=> second) == 0;
    }
};

#pragma endregion Argument Views

#pragma region Shared Reads

/** Reads the buckets of @p touched and points every one of them at the bytes it holds today. */
void pull_buckets( //
    ustore_database_t const database, ustore_transaction_t const transaction, ustore_snapshot_t const snapshot,
    std::span<touched_bucket_t> touched, ustore_options_t const options, linked_memory_lock_t& arena,
    ustore_error_t* error, joined_blobs_t& found) noexcept {

    ustore_bytes_ptr_t found_values = nullptr;
    ustore_length_t* found_offsets = nullptr;
    strided_range<touched_bucket_t> const buckets = strided(touched);
    auto const collections = buckets.immutable().members(&touched_bucket_t::collection);
    auto const keys = buckets.immutable().members(&touched_bucket_t::key);

    ustore_read_t read {};
    read.db = database;
    read.error = error;
    read.transaction = transaction;
    read.snapshot = snapshot;
    read.arena = arena;
    read.options = options;
    read.tasks_count = static_cast<ustore_size_t>(touched.size());
    read.collections = collections.begin().get();
    read.collections_stride = collections.stride();
    read.keys = keys.begin().get();
    read.keys_stride = keys.stride();
    read.offsets = &found_offsets;
    read.values = &found_values;

    ustore_read(&read);
    if (error->status) return;
    found = joined_blobs_t {static_cast<ustore_size_t>(touched.size()), found_offsets, found_values};
}

/** Writes every bucket of @p touched back, removing the ones the batch emptied. */
void push_buckets( //
    ustore_database_t const database, ustore_transaction_t const transaction, std::span<touched_bucket_t> touched,
    ustore_options_t const options, linked_memory_lock_t& arena, ustore_error_t* error) noexcept {

    strided_range<touched_bucket_t> const buckets = strided(touched);
    auto const collections = buckets.immutable().members(&touched_bucket_t::collection);
    auto const keys = buckets.immutable().members(&touched_bucket_t::key);
    auto const contents = buckets.immutable().members(&touched_bucket_t::content);
    auto const lengths = buckets.immutable().members(&touched_bucket_t::length);

    ustore_write_t write {};
    write.db = database;
    write.error = error;
    write.transaction = transaction;
    write.arena = arena;
    write.options = options;
    write.tasks_count = static_cast<ustore_size_t>(touched.size());
    write.collections = collections.begin().get();
    write.collections_stride = collections.stride();
    write.keys = keys.begin().get();
    write.keys_stride = keys.stride();
    write.lengths = lengths.begin().get();
    write.lengths_stride = lengths.stride();
    write.values = contents.begin().get();
    write.values_stride = contents.stride();

    ustore_write(&write);
}

#pragma endregion Shared Reads

} // namespace unum::ustore

using namespace unum::ustore;

void ustore_strings_write(ustore_strings_write_t* request) {
    if (!request->tasks_count) return;
    if (!request->strings) {
        *request->error = {ustore_error_invalid_argument_k, "No strings were provided"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(request->arena, request->options, request->error);
    if (request->error->status) return;

    std::size_t const tasks_count = request->tasks_count;
    strided_iterator<ustore_collection_t const> const collections {request->collections, request->collections_stride};
    strings_arg_t const texts {
        {request->strings, request->strings_stride},
        {request->strings_lengths, request->strings_lengths_stride},
    };
    contents_arg_t const values {
        {request->values_presences},
        {request->values_offsets, request->values_offsets_stride},
        {request->values_lengths, request->values_lengths_stride},
        {request->values, request->values_stride},
        static_cast<ustore_size_t>(tasks_count),
    };

    // Tasks sorted by bucket and by string, so the tasks of one bucket merge in one pass
    std::span<write_task_t> const tasks = arena.alloc<write_task_t>(tasks_count, request->error);
    if (request->error->status) return;
    for (std::size_t index = 0; index != tasks_count; ++index) {
        std::string_view const text = texts[index];
        tasks[index].collection = collections ? collections[static_cast<std::ptrdiff_t>(index)]
                                              : ustore_collection_main_k;
        tasks[index].key = key_of(text);
        tasks[index].text = text;
        tasks[index].value = values[index];
        tasks[index].task_index = static_cast<ustore_length_t>(index);
    }
    std::ranges::sort(tasks);

    // Every bucket the batch touches, one per run of tasks that share a collection and a key
    std::span<touched_bucket_t> touched = arena.alloc<touched_bucket_t>(tasks_count, request->error);
    if (request->error->status) return;
    std::size_t touched_count = 0;
    for (std::size_t index = 0; index != tasks_count; ++index) {
        write_task_t const& task = tasks[index];
        if (touched_count != 0 && touched[touched_count - 1].collection == task.collection &&
            touched[touched_count - 1].key == task.key)
            continue;
        touched[touched_count] = touched_bucket_t {};
        touched[touched_count].collection = task.collection;
        touched[touched_count].key = task.key;
        ++touched_count;
    }
    touched = touched.first(touched_count);

    // A transaction watches every bucket it rewrites, whatever the caller asked of its reads
    ustore_options_t const read_options =
        request->transaction ? ustore_options_t(request->options & ~ustore_option_transaction_dont_watch_k)
                             : request->options;

    joined_blobs_t stored;
    pull_buckets(request->db, request->transaction, {}, touched, read_options, arena, request->error, stored);
    if (request->error->status) return;

    std::size_t task_begin = 0;
    for (std::size_t bucket_index = 0; bucket_index != touched_count; ++bucket_index) {
        touched_bucket_t& bucket = touched[bucket_index];
        std::size_t task_end = task_begin;
        while (task_end != tasks_count && tasks[task_end].collection == bucket.collection &&
               tasks[task_end].key == bucket.key)
            ++task_end;

        bucket_view_t const existing {stored[bucket_index]};
        std::size_t const room = existing.size() + (task_end - task_begin);
        std::span<bucket_entry_t> const merged = arena.alloc<bucket_entry_t>(room, request->error);
        if (request->error->status) return;

        // Two sorted runs meet, where a task replaces or removes the entry of the same string
        std::size_t merged_count = 0;
        std::size_t existing_index = 0;
        std::size_t task_index = task_begin;
        while (existing_index != existing.size() || task_index != task_end) {
            if (task_index != task_end) {
                // Only the last task naming one string takes effect, since the sort put it last
                std::size_t last_index = task_index;
                while (last_index + 1 != task_end && tasks[last_index + 1].text == tasks[task_index].text) ++last_index;

                std::string_view const text = tasks[task_index].text;
                bool const behind = existing_index == existing.size() || text <= existing.text_of(existing_index);
                if (behind) {
                    if (existing_index != existing.size() && existing.text_of(existing_index) == text) ++existing_index;
                    if (tasks[last_index].value) merged[merged_count++] = {text, tasks[last_index].value};
                    task_index = last_index + 1;
                    continue;
                }
            }
            merged[merged_count++] = existing[existing_index];
            ++existing_index;
        }

        std::span<bucket_entry_t const> const entries = merged.first(merged_count);
        if (merged_count == 0) {
            bucket.content = nullptr;
            bucket.length = ustore_length_missing_k;
        }
        else {
            std::span<byte_t> const written = arena.alloc<byte_t>(bucket_bytes(entries), request->error);
            if (request->error->status) return;
            write_bucket(reinterpret_cast<ustore_byte_t*>(written.data()), entries);
            bucket.content = reinterpret_cast<ustore_bytes_cptr_t>(written.data());
            bucket.length = static_cast<ustore_length_t>(written.size());
        }
        task_begin = task_end;
    }

    push_buckets(request->db, request->transaction, touched, request->options, arena, request->error);
}

void ustore_strings_read(ustore_strings_read_t* request) {
    if (!request->tasks_count) return;
    if (!request->strings) {
        *request->error = {ustore_error_invalid_argument_k, "No strings were provided"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(request->arena, request->options, request->error);
    if (request->error->status) return;

    std::size_t const tasks_count = request->tasks_count;
    strided_iterator<ustore_collection_t const> const collections {request->collections, request->collections_stride};
    strings_arg_t const texts {
        {request->strings, request->strings_stride},
        {request->strings_lengths, request->strings_lengths_stride},
    };

    std::span<touched_bucket_t> touched = arena.alloc<touched_bucket_t>(tasks_count, request->error);
    if (request->error->status) return;
    for (std::size_t index = 0; index != tasks_count; ++index) {
        touched[index] = touched_bucket_t {};
        touched[index].collection = collections ? collections[static_cast<std::ptrdiff_t>(index)]
                                                : ustore_collection_main_k;
        touched[index].key = key_of(texts[index]);
    }
    std::size_t const touched_count = sort_and_deduplicate(touched.begin(), touched.end());
    touched = touched.first(touched_count);

    joined_blobs_t stored;
    pull_buckets(request->db, request->transaction, request->snapshot, touched, request->options, arena, request->error,
                 stored);
    if (request->error->status) return;

    growing_tape_t exported {arena};
    exported.reserve(tasks_count, request->error);
    if (request->error->status) return;

    for (std::size_t index = 0; index != tasks_count; ++index) {
        std::string_view const text = texts[index];
        ustore_collection_t const collection = collections ? collections[static_cast<std::ptrdiff_t>(index)]
                                                           : ustore_collection_main_k;
        std::size_t const bucket_index = offset_in_sorted(touched, collection_key_t {collection, key_of(text)});
        bucket_view_t const bucket {stored[bucket_index]};
        std::size_t const position = bucket.find(text);
        exported.push_back(position == bucket.size() ? value_view_t {} : bucket.value_of(position), request->error);
        if (request->error->status) return;
    }

    if (request->presences) *request->presences = exported.presences().get();
    if (request->offsets) *request->offsets = exported.offsets().begin().get();
    if (request->lengths) *request->lengths = exported.lengths().begin().get();
    if (request->values) *request->values = reinterpret_cast<ustore_byte_t*>(exported.contents().begin().get());
}

void ustore_strings_scan(ustore_strings_scan_t* request) {
    if (!request->tasks_count) return;
    if (!request->prefixes) {
        *request->error = {ustore_error_invalid_argument_k, "No prefixes were provided"};
        return;
    }
    if (!request->count_limits) {
        *request->error = {ustore_error_unsupported_k, "Full scans are not supported, paginate with limits"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(request->arena, request->options, request->error);
    if (request->error->status) return;

    std::size_t const tasks_count = request->tasks_count;
    strided_iterator<ustore_collection_t const> const collections {request->collections, request->collections_stride};
    strided_iterator<ustore_length_t const> const count_limits {request->count_limits, request->count_limits_stride};
    strings_arg_t const prefixes {
        {request->prefixes, request->prefixes_stride},
        {request->prefixes_lengths, request->prefixes_lengths_stride},
    };
    strings_arg_t const start_strings {
        {request->start_strings, request->start_strings_stride},
        {request->start_strings_lengths, request->start_strings_lengths_stride},
    };

    auto offsets = arena.alloc_or_dummy(tasks_count + 1, request->error, request->offsets);
    if (request->error->status) return;
    auto counts = arena.alloc_or_dummy(tasks_count, request->error, request->counts);
    if (request->error->status) return;

    growing_tape_t exported {arena};
    std::size_t exported_count = 0;

    for (std::size_t index = 0; index != tasks_count; ++index) {
        auto const signed_index = static_cast<std::ptrdiff_t>(index);
        ustore_collection_t collection = collections ? collections[signed_index] : ustore_collection_main_k;
        std::string_view const prefix = prefixes[index];
        std::string_view const start = request->start_strings ? start_strings[index] : std::string_view {};
        std::size_t const limit = count_limits[signed_index];
        key_window_t const window = window_of(prefix);

        offsets[index] = static_cast<ustore_length_t>(exported_count);
        std::size_t found = 0;
        ustore_key_t next_key = window.first;
        if (request->start_strings && !start.empty()) {
            ustore_key_t const resumed = key_of(start);
            if (resumed > next_key) next_key = resumed;
        }

        bool exhausted = limit == 0 || next_key > window.last;
        while (!exhausted) {
            auto const wanted_keys = static_cast<ustore_length_t>(limit - found);
            ustore_length_t* scanned_counts = nullptr;
            ustore_key_t* scanned_keys = nullptr;
            ustore_scan_t scan {};
            scan.db = request->db;
            scan.error = request->error;
            scan.transaction = request->transaction;
            scan.snapshot = request->snapshot;
            scan.arena = arena;
            scan.options = request->options;
            scan.tasks_count = 1;
            scan.collections = &collection;
            scan.start_keys = &next_key;
            scan.count_limits = &wanted_keys;
            scan.counts = &scanned_counts;
            scan.keys = &scanned_keys;

            ustore_scan(&scan);
            if (request->error->status) return;

            std::size_t const scanned = scanned_counts[0];
            if (scanned == 0) break;

            // The scan runs to the end of the collection, so the window closes the walk here
            std::size_t wanted = 0;
            while (wanted != scanned && scanned_keys[wanted] <= window.last) ++wanted;
            exhausted = wanted != scanned || scanned != wanted_keys;
            if (wanted == 0) break;

            std::span<touched_bucket_t> const touched = arena.alloc<touched_bucket_t>(wanted, request->error);
            if (request->error->status) return;
            for (std::size_t scanned_index = 0; scanned_index != wanted; ++scanned_index) {
                touched[scanned_index] = touched_bucket_t {};
                touched[scanned_index].collection = collection;
                touched[scanned_index].key = scanned_keys[scanned_index];
            }

            joined_blobs_t stored;
            pull_buckets(request->db, request->transaction, request->snapshot, touched, request->options, arena,
                         request->error, stored);
            if (request->error->status) return;

            for (std::size_t scanned_index = 0; scanned_index != wanted && found != limit; ++scanned_index) {
                bucket_view_t const bucket {stored[scanned_index]};
                for (std::size_t entry = 0; entry != bucket.size() && found != limit; ++entry) {
                    std::string_view const text = bucket.text_of(entry);
                    if (!text.starts_with(prefix)) continue;
                    if (request->start_strings && text < start) continue;
                    exported.push_back(value_view_t {text}, request->error);
                    if (request->error->status) return;
                    ++found;
                    ++exported_count;
                }
            }

            ustore_key_t const last_key = scanned_keys[wanted - 1];
            if (found == limit || last_key == std::numeric_limits<ustore_key_t>::max()) break;
            next_key = last_key + 1;
            if (next_key > window.last) break;
        }

        counts[index] = static_cast<ustore_length_t>(found);
    }

    offsets[tasks_count] = static_cast<ustore_length_t>(exported_count);
    if (request->strings_offsets) *request->strings_offsets = exported.offsets().begin().get();
    if (request->strings) *request->strings = reinterpret_cast<ustore_char_t*>(exported.contents().begin().get());
}
