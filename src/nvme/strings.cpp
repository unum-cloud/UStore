/**
 *  @file src/nvme/strings.cpp
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief The strings modality and the names of collections laid over NVMe's keyed calls, until NVMe keys them itself.
 *
 *  A string becomes a key of its leading bytes and its length, and the value under that key is a bucket:
 *  a sorted run of entries, each a string and its value, binary-searched by text. A string short enough to
 *  fit the key owns its bucket alone, while longer ones sharing a leading run share a bucket, so both a
 *  point lookup and a prefix walk reach the engine as an ordered walk of keys.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::strlen`

#include <algorithm>   // `std::sort`, `std::ranges::sort`, `std::ranges::lower_bound`, `std::min`
#include <compare>     // `std::strong_ordering`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <ustore/ranges_args.hpp> // `contents_arg_t`
#include <ustore/strings.h>       // `ustore_strings_write_t`, `ustore_strings_read_t`
#include <ustore/ustore.hpp>      // `ustore_read`, `strided_iterator`, `joined_blobs_t`

#include "engine.hpp"                // `name_record_t`, `read_name_record`
#include "helpers/algorithm.hpp"     // `offset_in_sorted`
#include "helpers/linked_array.hpp"  // `growing_tape_t`, `uninitialized_array`
#include "helpers/linked_memory.hpp" // `linked_memory_lock_t`

namespace unum::ustore::nvme {

/** The collection the names of collections live in as strings, which no minted UUIDv7 spells, as its byte 6 is 0x7X. */
extern ustore_collection_id_t const names_collection_k = {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE}}};

void collection_find(ustore_collection_find_t& request) noexcept;

namespace {

#pragma region Key Encoding

/** Bytes of a key that hold the leading characters of a string, leaving the last one for its length. */
constexpr std::size_t prefix_bytes_k = sizeof(ustore_key_t) - 1;

/** The length a key carries for a string too long to fit @c prefix_bytes_k, which shares a bucket. */
constexpr unsigned char bucketed_length_k = static_cast<unsigned char>(prefix_bytes_k + 1);

/** The byte at @p index of @p text, or the padding a shorter string takes. */
constexpr unsigned char padded_byte(std::string_view text, std::size_t index) noexcept {
    return index < text.size() ? static_cast<unsigned char>(text[index]) : static_cast<unsigned char>(0);
}

/** The key of a string whose leading bytes are @p text and whose length byte is @p length_byte. */
constexpr ustore_key_t key_from_bytes(std::string_view text, unsigned char length_byte) noexcept {
    ustore_key_t key {};
    for (std::size_t index = 0; index != prefix_bytes_k; ++index) key.uuid.bytes[index] = padded_byte(text, index);
    key.uuid.bytes[prefix_bytes_k] = length_byte;
    return key;
}

/** The key @p text is stored under, which orders exactly as the text of every string that fits a key. */
constexpr ustore_key_t key_from_string(std::string_view text) noexcept {
    unsigned char const length_byte = text.size() <= prefix_bytes_k ? static_cast<unsigned char>(text.size())
                                                                    : bucketed_length_k;
    return key_from_bytes(text, length_byte);
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
        ustore_key_t const bucket = key_from_string(prefix);
        return {bucket, bucket};
    }

    // Every length byte a stored string takes stays below the padding of the upper bound, so it closes the window
    ustore_key_t highest {};
    for (std::size_t index = 0; index != prefix_bytes_k; ++index)
        highest.uuid.bytes[index] = index < prefix.size() ? static_cast<unsigned char>(prefix[index])
                                                          : static_cast<unsigned char>(0xFF);
    highest.uuid.bytes[prefix_bytes_k] = 0xFF;
    return {key_from_string(prefix), highest};
}

#pragma endregion Key Encoding

#pragma region Bucket Layout

/** The word every count and offset of a bucket takes, wide enough for a bucket past 4 GiB. */
using bucket_word_t = std::uint64_t;

/** Bytes of the entry count that leads every bucket. */
constexpr std::size_t bucket_header_bytes_k = sizeof(bucket_word_t);

/** Reads the little-endian length sitting @p offset bytes into @p bytes. */
inline bucket_word_t read_length(ustore_bytes_cptr_t bytes, std::size_t offset) noexcept {
    bucket_word_t length = 0;
    std::memcpy(&length, bytes + offset, sizeof(length));
    return length;
}

/** Writes @p length as a little-endian length @p offset bytes into @p bytes. */
inline void write_length(ustore_byte_t* bytes, std::size_t offset, bucket_word_t length) noexcept {
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
    bucket_word_t count_ {0};

  public:
    bucket_view_t() = default;

    explicit bucket_view_t(value_view_t value) noexcept {
        if (value.size() < bucket_header_bytes_k) return;
        auto const bytes = reinterpret_cast<ustore_bytes_cptr_t>(value.data());
        bucket_word_t const count = read_length(bytes, 0);
        if (value.size() < bucket_header_bytes_k + 2 * sizeof(bucket_word_t) * (static_cast<std::size_t>(count) + 1))
            return;
        bytes_ = bytes;
        count_ = count;
    }

    /** Entries the bucket holds. */
    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }

    /** The string of the entry at @p index. */
    std::string_view text_of(std::size_t index) const noexcept {
        std::size_t const begin = read_length(bytes_, strings_offsets_begin_() + index * sizeof(bucket_word_t));
        std::size_t const end = read_length(bytes_, strings_offsets_begin_() + (index + 1) * sizeof(bucket_word_t));
        auto const characters = reinterpret_cast<char const*>(bytes_ + strings_begin_());
        return {characters + begin, end - begin};
    }

    /** The value of the entry at @p index. */
    value_view_t value_of(std::size_t index) const noexcept {
        std::size_t const begin = read_length(bytes_, values_offsets_begin_() + index * sizeof(bucket_word_t));
        std::size_t const end = read_length(bytes_, values_offsets_begin_() + (index + 1) * sizeof(bucket_word_t));
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
        return bucket_header_bytes_k + sizeof(bucket_word_t) * (static_cast<std::size_t>(count_) + 1);
    }

    /** Where the strings themselves start. */
    std::size_t strings_begin_() const noexcept {
        return bucket_header_bytes_k + 2 * sizeof(bucket_word_t) * (static_cast<std::size_t>(count_) + 1);
    }

    /** Where the values themselves start. */
    std::size_t values_begin_() const noexcept {
        return strings_begin_() + read_length(bytes_, strings_offsets_begin_() + count_ * sizeof(bucket_word_t));
    }
};

/** The bytes a bucket of @p entries takes once written. */
inline std::size_t bucket_bytes(std::span<bucket_entry_t const> entries) noexcept {
    std::size_t bytes = bucket_header_bytes_k + 2 * sizeof(bucket_word_t) * (entries.size() + 1);
    for (bucket_entry_t const& entry : entries) bytes += entry.text.size() + entry.value.size();
    return bytes;
}

/** Writes @p entries, already sorted by their strings, into @p bytes, which has room for them. */
inline void write_bucket(ustore_byte_t* bytes, std::span<bucket_entry_t const> entries) noexcept {
    std::size_t const count = entries.size();
    write_length(bytes, 0, static_cast<bucket_word_t>(count));

    std::size_t const strings_offsets_begin = bucket_header_bytes_k;
    std::size_t const values_offsets_begin = bucket_header_bytes_k + sizeof(bucket_word_t) * (count + 1);
    std::size_t const strings_begin = bucket_header_bytes_k + 2 * sizeof(bucket_word_t) * (count + 1);

    std::size_t strings_bytes = 0;
    std::size_t values_bytes = 0;
    for (std::size_t index = 0; index != count; ++index) {
        write_length(bytes, strings_offsets_begin + index * sizeof(bucket_word_t),
                     static_cast<bucket_word_t>(strings_bytes));
        write_length(bytes, values_offsets_begin + index * sizeof(bucket_word_t),
                     static_cast<bucket_word_t>(values_bytes));
        strings_bytes += entries[index].text.size();
        values_bytes += entries[index].value.size();
    }
    write_length(bytes, strings_offsets_begin + count * sizeof(bucket_word_t),
                 static_cast<bucket_word_t>(strings_bytes));
    write_length(bytes, values_offsets_begin + count * sizeof(bucket_word_t), static_cast<bucket_word_t>(values_bytes));

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
    ustore_collection_id_t collection {ustore_collection_main_k};

    /** Key the string is stored under. */
    ustore_key_t key {};

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

#pragma region Named Collections

/** Times a create retries recording its name, as a transaction conflicts over any other name sharing its bucket. */
constexpr std::size_t name_record_attempts_k = 16;

/** Reads the record of @p name at the head, answering whether a collection carries the name at all. */
bool look_up_name(ustore_database_t database, ustore_transaction_t transaction, std::string_view name,
                  linked_memory_lock_t& arena, ustore_error_t* error, ustore_collection_id_t& id) noexcept {
    ustore_collection_id_t const names = names_collection_k;
    char const* const text = name.data();
    ustore_length_t const length = static_cast<ustore_length_t>(name.size());
    ustore_length_t* lengths = nullptr;
    ustore_byte_t* values = nullptr;

    ustore_strings_read_t read {};
    read.db = database;
    read.error = error;
    read.transaction = transaction;
    read.arena = arena;
    read.tasks_count = 1;
    read.collections = &names;
    read.strings = &text;
    read.strings_lengths = &length;
    read.lengths = &lengths;
    read.values = &values;
    ustore_strings_read(&read);
    if (error->status || lengths[0] == ustore_length_missing_k) return false;
    ustore_modality_t modality = ustore_modality_any_k;
    if (read_name_record(std::string_view {reinterpret_cast<char const*>(values), lengths[0]}, id, modality))
        return true;
    *error = {ustore_error_malformed_k, "The name holds no collection record"};
    return false;
}

/** Writes @p record under @p name, or removes the name when @p record is null. */
void write_name_record(ustore_database_t database, ustore_transaction_t transaction, std::string_view name,
                       name_record_t const* record, ustore_options_t options, linked_memory_lock_t& arena,
                       ustore_error_t* error) noexcept {
    ustore_collection_id_t const names = names_collection_k;
    char const* const text = name.data();
    ustore_length_t const length = static_cast<ustore_length_t>(name.size());
    ustore_bytes_cptr_t const value = record ? reinterpret_cast<ustore_bytes_cptr_t>(record->bytes) : nullptr;
    ustore_length_t const value_length = sizeof(name_record_t::bytes);

    ustore_strings_write_t write {};
    write.db = database;
    write.error = error;
    write.transaction = transaction;
    write.arena = arena;
    write.options = options;
    write.tasks_count = 1;
    write.collections = &names;
    write.strings = &text;
    write.strings_lengths = &length;
    write.values = record ? &value : nullptr;
    write.values_lengths = record ? &value_length : nullptr;
    ustore_strings_write(&write);
}

/**
 *  @brief Records @p record under @p name unless another writer took the name first, answering whether it did.
 *
 *  The name is read watched and written in one transaction, so a concurrent creator of the same name makes one of
 *  the two commits conflict. A conflict over another name of the same bucket is retried.
 */
bool record_name(ustore_database_t database, std::string_view name, name_record_t const& record,
                 linked_memory_lock_t& arena, ustore_error_t* error) noexcept {
    ustore_transaction_t transaction = nullptr;
    ustore_collection_id_t taken = ustore_collection_main_k;
    bool recorded = false;
    for (std::size_t attempt = 0; !recorded && attempt != name_record_attempts_k; ++attempt) {
        *error = {};
        ustore_transaction_init_t init {};
        init.db = database;
        init.error = error;
        init.transaction = &transaction;
        ustore_transaction_init(&init);
        if (error->status) break;

        if (look_up_name(database, transaction, name, arena, error, taken)) {
            *error = {ustore_error_already_exists_k, "Another collection took the name first"};
            break;
        }
        if (error->status) break;
        write_name_record(database, transaction, name, &record, ustore_options_default_k, arena, error);
        if (error->status) break;

        ustore_transaction_commit_t commit {};
        commit.db = database;
        commit.error = error;
        commit.transaction = transaction;
        commit.options = ustore_option_write_flush_k;
        ustore_transaction_commit(&commit);
        recorded = !error->status;
        if (error->status != ustore_error_read_conflict_k && error->status != ustore_error_write_conflict_k) break;
    }
    ustore_transaction_free(transaction);
    return recorded;
}

/** Every name of a database with the collection it addresses and what that holds, exported into one arena. */
struct names_t {
    std::size_t count = 0;
    ustore_length_t* offsets = nullptr;
    ustore_char_t* tape = nullptr;
    ustore_collection_id_t* collections = nullptr;
    ustore_modality_t* modalities = nullptr;

    /** The name at @p index. */
    [[nodiscard]] std::string_view operator[](std::size_t index) const noexcept {
        return {tape + offsets[index], offsets[index + 1] - offsets[index]};
    }
};

/** Reads every name of @p database at @p snapshot, or at the head, keeping whatever @p arena held before. */
names_t scan_names(ustore_database_t database, ustore_snapshot_t snapshot, linked_memory_lock_t& arena,
                   ustore_error_t* error) noexcept {
    names_t names;
    ustore_collection_id_t const collection = names_collection_k;
    char const* const prefix = "";
    ustore_length_t const prefix_length = 0;
    ustore_length_t const limit = std::numeric_limits<ustore_length_t>::max();
    ustore_length_t* counts = nullptr;
    ustore_strings_scan_t scan {};
    scan.db = database;
    scan.error = error;
    scan.snapshot = snapshot;
    scan.arena = arena;
    scan.options = ustore_option_dont_discard_memory_k;
    scan.tasks_count = 1;
    scan.collections = &collection;
    scan.prefixes = &prefix;
    scan.prefixes_lengths = &prefix_length;
    scan.count_limits = &limit;
    scan.counts = &counts;
    scan.strings_offsets = &names.offsets;
    scan.strings = &names.tape;
    ustore_strings_scan(&scan);
    if (error->status) return names;
    names.count = counts[0];
    if (names.count == 0) return names;

    // The records sit beside the names, so one batched lookup of the names just listed answers them
    std::span<ustore_str_view_t> const listed = arena.alloc<ustore_str_view_t>(names.count, error);
    std::span<ustore_length_t> const lengths = arena.alloc<ustore_length_t>(names.count, error);
    if (error->status) return names;
    for (std::size_t index = 0; index != names.count; ++index) {
        std::string_view const name = names[index];
        listed[index] = name.data();
        lengths[index] = static_cast<ustore_length_t>(name.size());
    }
    ustore_collection_find_t find {};
    find.db = database;
    find.error = error;
    find.snapshot = snapshot;
    find.arena = arena;
    find.options = ustore_option_dont_discard_memory_k;
    find.tasks_count = names.count;
    find.names = listed.data();
    find.names_stride = sizeof(ustore_str_view_t);
    find.names_lengths = lengths.data();
    find.names_lengths_stride = sizeof(ustore_length_t);
    find.collections = &names.collections;
    find.modalities = &names.modalities;
    collection_find(find);
    return names;
}

/** A name's position in a @c names_t, beside the collection it addresses, so names sort by collection. */
struct named_position_t {
    ustore_collection_id_t id = ustore_collection_main_k;
    std::size_t position = 0;

    friend constexpr std::strong_ordering operator<=>(named_position_t const&,
                                                      named_position_t const&) noexcept = default;
};

#pragma endregion Named Collections

} // namespace

#pragma region Strings and Names

void strings_write(ustore_strings_write_t& request) noexcept {
    if (!request.tasks_count) return;
    if (!request.strings) {
        *request.error = {ustore_error_invalid_argument_k, "No strings were provided"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(request.arena, request.options, request.error);
    if (request.error->status) return;

    std::size_t const tasks_count = request.tasks_count;
    strided_iterator<ustore_collection_id_t const> const collections {request.collections, request.collections_stride};
    strings_arg_t const texts {
        {request.strings, request.strings_stride},
        {request.strings_lengths, request.strings_lengths_stride},
    };
    contents_arg_t const values {
        {request.values_presences},
        {request.values_offsets, request.values_offsets_stride},
        {request.values_lengths, request.values_lengths_stride},
        {request.values, request.values_stride},
        static_cast<ustore_size_t>(tasks_count),
    };

    // Tasks sorted by bucket and by string, so the tasks of one bucket merge in one pass
    std::span<write_task_t> const tasks = arena.alloc<write_task_t>(tasks_count, request.error);
    if (request.error->status) return;
    for (std::size_t index = 0; index != tasks_count; ++index) {
        std::string_view const text = texts[index];
        tasks[index].collection = collections ? collections[static_cast<std::ptrdiff_t>(index)]
                                              : ustore_collection_main_k;
        tasks[index].key = key_from_string(text);
        tasks[index].text = text;
        tasks[index].value = values[index];
        tasks[index].task_index = static_cast<ustore_length_t>(index);
    }
    std::sort(tasks.begin(), tasks.end());

    // Every bucket the batch touches, one per run of tasks that share a collection and a key
    std::span<touched_bucket_t> touched = arena.alloc<touched_bucket_t>(tasks_count, request.error);
    if (request.error->status) return;
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

    // A transaction watches every bucket it rewrites, whatever the caller asked of its reads, and a flush is for the
    // write that follows, which a read refuses
    ustore_options_t const read_options = ustore_options_t(
        request.options & ~ustore_option_write_flush_k &
        ~(request.transaction ? ustore_option_transaction_dont_watch_k : ustore_options_default_k));

    joined_blobs_t stored;
    pull_buckets(request.db, request.transaction, {}, touched, read_options, arena, request.error, stored);
    if (request.error->status) return;

    std::size_t task_begin = 0;
    for (std::size_t bucket_index = 0; bucket_index != touched_count; ++bucket_index) {
        touched_bucket_t& bucket = touched[bucket_index];
        std::size_t task_end = task_begin;
        while (task_end != tasks_count && tasks[task_end].collection == bucket.collection &&
               tasks[task_end].key == bucket.key)
            ++task_end;

        bucket_view_t const existing {stored[bucket_index]};
        std::size_t const room = existing.size() + (task_end - task_begin);
        std::span<bucket_entry_t> const merged = arena.alloc<bucket_entry_t>(room, request.error);
        if (request.error->status) return;

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
            std::span<byte_t> const written = arena.alloc<byte_t>(bucket_bytes(entries), request.error);
            if (request.error->status) return;
            write_bucket(reinterpret_cast<ustore_byte_t*>(written.data()), entries);
            bucket.content = reinterpret_cast<ustore_bytes_cptr_t>(written.data());
            bucket.length = static_cast<ustore_length_t>(written.size());
        }
        task_begin = task_end;
    }

    push_buckets(request.db, request.transaction, touched, request.options, arena, request.error);
}

void strings_read(ustore_strings_read_t& request) noexcept {
    if (!request.tasks_count) return;
    if (!request.strings) {
        *request.error = {ustore_error_invalid_argument_k, "No strings were provided"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(request.arena, request.options, request.error);
    if (request.error->status) return;

    std::size_t const tasks_count = request.tasks_count;
    strided_iterator<ustore_collection_id_t const> const collections {request.collections, request.collections_stride};
    strings_arg_t const texts {
        {request.strings, request.strings_stride},
        {request.strings_lengths, request.strings_lengths_stride},
    };

    std::span<touched_bucket_t> touched = arena.alloc<touched_bucket_t>(tasks_count, request.error);
    if (request.error->status) return;
    for (std::size_t index = 0; index != tasks_count; ++index) {
        touched[index] = touched_bucket_t {};
        touched[index].collection = collections ? collections[static_cast<std::ptrdiff_t>(index)]
                                                : ustore_collection_main_k;
        touched[index].key = key_from_string(texts[index]);
    }
    std::size_t const touched_count = sort_and_deduplicate(touched.begin(), touched.end());
    touched = touched.first(touched_count);

    joined_blobs_t stored;
    pull_buckets(request.db, request.transaction, request.snapshot, touched, request.options, arena, request.error,
                 stored);
    if (request.error->status) return;

    growing_tape_t exported {arena};
    exported.reserve(tasks_count, request.error);
    if (request.error->status) return;

    for (std::size_t index = 0; index != tasks_count; ++index) {
        std::string_view const text = texts[index];
        ustore_collection_id_t const collection = collections ? collections[static_cast<std::ptrdiff_t>(index)]
                                                              : ustore_collection_main_k;
        std::size_t const bucket_index = offset_in_sorted(touched,
                                                          collection_key_t {collection, key_from_string(text)});
        bucket_view_t const bucket {stored[bucket_index]};
        std::size_t const position = bucket.find(text);
        exported.push_back(position == bucket.size() ? value_view_t {} : bucket.value_of(position), request.error);
        if (request.error->status) return;
    }

    if (request.presences) *request.presences = exported.presences().get();
    if (request.offsets) *request.offsets = exported.offsets().begin().get();
    if (request.lengths) *request.lengths = exported.lengths().begin().get();
    if (request.values) *request.values = reinterpret_cast<ustore_byte_t*>(exported.contents().begin().get());
}

void strings_scan(ustore_strings_scan_t& request) noexcept {
    if (!request.tasks_count) return;
    if (!request.prefixes) {
        *request.error = {ustore_error_invalid_argument_k, "No prefixes were provided"};
        return;
    }
    if (!request.count_limits) {
        *request.error = {ustore_error_unsupported_k, "Full scans are not supported, paginate with limits"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(request.arena, request.options, request.error);
    if (request.error->status) return;

    std::size_t const tasks_count = request.tasks_count;
    strided_iterator<ustore_collection_id_t const> const collections {request.collections, request.collections_stride};
    strided_iterator<ustore_length_t const> const count_limits {request.count_limits, request.count_limits_stride};
    strings_arg_t const prefixes {
        {request.prefixes, request.prefixes_stride},
        {request.prefixes_lengths, request.prefixes_lengths_stride},
    };
    strings_arg_t const start_strings {
        {request.start_strings, request.start_strings_stride},
        {request.start_strings_lengths, request.start_strings_lengths_stride},
    };

    auto offsets = arena.alloc_or_dummy(tasks_count + 1, request.error, request.offsets);
    if (request.error->status) return;
    auto counts = arena.alloc_or_dummy(tasks_count, request.error, request.counts);
    if (request.error->status) return;

    growing_tape_t exported {arena};
    std::size_t exported_count = 0;

    for (std::size_t index = 0; index != tasks_count; ++index) {
        auto const signed_index = static_cast<std::ptrdiff_t>(index);
        ustore_collection_id_t collection = collections ? collections[signed_index] : ustore_collection_main_k;
        std::string_view const prefix = prefixes[index];
        std::string_view const start = request.start_strings ? start_strings[index] : std::string_view {};
        std::size_t const limit = count_limits[signed_index];
        key_window_t const window = window_of(prefix);

        offsets[index] = static_cast<ustore_length_t>(exported_count);
        std::size_t found = 0;
        ustore_key_t next_key = window.first;
        if (request.start_strings && !start.empty()) {
            ustore_key_t const resumed = key_from_string(start);
            if (resumed > next_key) next_key = resumed;
        }

        bool exhausted = limit == 0 || next_key > window.last;
        while (!exhausted) {
            auto const wanted_keys = static_cast<ustore_length_t>(limit - found);
            ustore_length_t* scanned_counts = nullptr;
            ustore_key_t* scanned_keys = nullptr;
            ustore_scan_t scan {};
            scan.db = request.db;
            scan.error = request.error;
            scan.transaction = request.transaction;
            scan.snapshot = request.snapshot;
            scan.arena = arena;
            scan.options = request.options;
            scan.tasks_count = 1;
            scan.collections = &collection;
            scan.start_keys = &next_key;
            scan.count_limits = &wanted_keys;
            scan.counts = &scanned_counts;
            scan.keys = &scanned_keys;

            ustore_scan(&scan);
            if (request.error->status) return;

            std::size_t const scanned = scanned_counts[0];
            if (scanned == 0) break;

            // The scan runs to the end of the collection, so the window closes the walk here
            std::size_t wanted = 0;
            while (wanted != scanned && scanned_keys[wanted] <= window.last) ++wanted;
            exhausted = wanted != scanned || scanned != wanted_keys;
            if (wanted == 0) break;

            std::span<touched_bucket_t> const touched = arena.alloc<touched_bucket_t>(wanted, request.error);
            if (request.error->status) return;
            for (std::size_t scanned_index = 0; scanned_index != wanted; ++scanned_index) {
                touched[scanned_index] = touched_bucket_t {};
                touched[scanned_index].collection = collection;
                touched[scanned_index].key = scanned_keys[scanned_index];
            }

            joined_blobs_t stored;
            pull_buckets(request.db, request.transaction, request.snapshot, touched, request.options, arena,
                         request.error, stored);
            if (request.error->status) return;

            for (std::size_t scanned_index = 0; scanned_index != wanted && found != limit; ++scanned_index) {
                bucket_view_t const bucket {stored[scanned_index]};
                for (std::size_t entry = 0; entry != bucket.size() && found != limit; ++entry) {
                    std::string_view const text = bucket.text_of(entry);
                    if (!text.starts_with(prefix)) continue;
                    if (request.start_strings && text < start) continue;
                    exported.push_back(value_view_t {text}, request.error);
                    if (request.error->status) return;
                    ++found;
                    ++exported_count;
                }
            }

            ustore_key_t const last_key = scanned_keys[wanted - 1];
            if (found == limit || last_key >= window.last) break;
            next_key = key_after(last_key);
        }

        counts[index] = static_cast<ustore_length_t>(found);
    }

    offsets[tasks_count] = static_cast<ustore_length_t>(exported_count);
    if (request.strings_offsets) *request.strings_offsets = exported.offsets().begin().get();
    if (request.strings) *request.strings = reinterpret_cast<ustore_char_t*>(exported.contents().begin().get());
}

void collection_find(ustore_collection_find_t& request) noexcept {
    if (!request.tasks_count) return;
    if (!request.names || !request.collections) {
        *request.error = {ustore_error_invalid_argument_k, "Need the names and an output for the identifiers"};
        return;
    }

    linked_memory_lock_t arena = linked_memory(request.arena, request.options, request.error);
    if (request.error->status) return;

    ustore_collection_id_t const names = names_collection_k;
    ustore_length_t* offsets = nullptr;
    ustore_length_t* lengths = nullptr;
    ustore_byte_t* values = nullptr;
    ustore_strings_read_t read {};
    read.db = request.db;
    read.error = request.error;
    read.snapshot = request.snapshot;
    read.arena = arena;
    read.tasks_count = request.tasks_count;
    read.collections = &names;
    read.strings = request.names;
    read.strings_stride = request.names_stride;
    read.strings_lengths = request.names_lengths;
    read.strings_lengths_stride = request.names_lengths_stride;
    read.offsets = &offsets;
    read.lengths = &lengths;
    read.values = &values;
    ustore_strings_read(&read);
    if (request.error->status) return;

    std::size_t const count = request.tasks_count;
    std::span<ustore_collection_id_t> const ids = arena.alloc<ustore_collection_id_t>(count, request.error);
    auto modalities = arena.alloc_or_dummy(count, request.error, request.modalities);
    auto presences = arena.alloc_or_dummy(count, request.error, request.presences);
    if (request.error->status) return;
    for (std::size_t index = 0; index != count; ++index) {
        bool const present = lengths[index] != ustore_length_missing_k;
        ustore_collection_id_t id = ustore_collection_main_k;
        ustore_modality_t modality = ustore_modality_any_k;
        std::string_view const record {reinterpret_cast<char const*>(values) + offsets[index],
                                       present ? lengths[index] : 0};
        if (present && !read_name_record(record, id, modality)) {
            *request.error = {ustore_error_malformed_k, "A name holds no collection record"};
            return;
        }
        ids[index] = id;
        modalities[index] = modality;
        presences[index] = present;
    }
    *request.collections = ids.data();
}

void with_names(ustore_collection_create_t& request, void (*create)(ustore_collection_create_t&) noexcept) noexcept {
    if (!request.name) return create(request);
    std::string_view const name = string_of(request.name, request.name_length);
    if (name.empty()) {
        *request.error = {ustore_error_invalid_argument_k, "A name is never empty, as the main collection has none"};
        return;
    }
    if (!request.collection) {
        *request.error = {ustore_error_invalid_argument_k, "Need an output for the collection"};
        return;
    }

    arena_t scratch {request.db};
    linked_memory_lock_t arena = linked_memory(scratch.member_ptr(), ustore_options_default_k, request.error);
    if (request.error->status) return;

    ustore_collection_id_t id = ustore_collection_main_k;
    if (look_up_name(request.db, nullptr, name, arena, request.error, id)) {
        *request.error = {ustore_error_already_exists_k, "A collection already carries the name"};
        return;
    }
    if (request.error->status) return;

    ustore_collection_create_t anonymous = request;
    anonymous.name = nullptr;
    anonymous.name_length = 0;
    anonymous.collection = &id;
    create(anonymous);
    if (request.error->status) return;

    ustore_capabilities_t capabilities {};
    ustore_database_capabilities_t query {};
    query.db = request.db;
    query.error = request.error;
    query.capabilities = &capabilities;
    ustore_database_capabilities(&query);

    name_record_t const record {id, request.modality};
    bool recorded = false;
    if (request.error->status == ustore_success_k && (capabilities & ustore_supports_transactions_k))
        recorded = record_name(request.db, name, record, arena, request.error);
    else if (request.error->status == ustore_success_k) {
        write_name_record(request.db, nullptr, name, &record, ustore_option_write_flush_k, arena, request.error);
        recorded = request.error->status == ustore_success_k;
    }
    if (recorded) {
        *request.collection = id;
        return;
    }

    // The collection no name addresses would be unreachable, so it goes, while the error that stopped the name stays
    ustore_error_t dropped {};
    ustore_collection_drop_t drop {};
    drop.db = request.db;
    drop.error = &dropped;
    drop.collection = id;
    drop.mode = ustore_drop_keys_vals_handle_k;
    ustore_collection_drop(&drop);
}

void with_names(ustore_collection_drop_t& request, void (*drop)(ustore_collection_drop_t&) noexcept) noexcept {
    if (request.mode != ustore_drop_keys_vals_handle_k || request.collection == ustore_collection_main_k)
        return drop(request);

    // The names go first and flushed, so a crash leaves a collection without a name, never a name without a collection
    arena_t scratch {request.db};
    linked_memory_lock_t arena = linked_memory(scratch.member_ptr(), ustore_options_default_k, request.error);
    if (request.error->status) return;
    names_t const names = scan_names(request.db, {}, arena, request.error);
    if (request.error->status) return;

    std::span<ustore_str_view_t> const unnamed = arena.alloc<ustore_str_view_t>(names.count, request.error);
    std::span<ustore_length_t> const lengths = arena.alloc<ustore_length_t>(names.count, request.error);
    if (request.error->status) return;
    std::size_t unnamed_count = 0;
    for (std::size_t index = 0; index != names.count; ++index) {
        if (names.collections[index] != request.collection) continue;
        std::string_view const name = names[index];
        unnamed[unnamed_count] = name.data();
        lengths[unnamed_count] = static_cast<ustore_length_t>(name.size());
        ++unnamed_count;
    }
    if (unnamed_count != 0) {
        ustore_collection_id_t const collection = names_collection_k;
        ustore_strings_write_t write {};
        write.db = request.db;
        write.error = request.error;
        write.arena = arena;
        write.options = ustore_option_write_flush_k;
        write.tasks_count = unnamed_count;
        write.collections = &collection;
        write.strings = unnamed.data();
        write.strings_stride = sizeof(ustore_str_view_t);
        write.strings_lengths = lengths.data();
        write.strings_lengths_stride = sizeof(ustore_length_t);
        ustore_strings_write(&write);
        if (request.error->status) return;
    }
    drop(request);
}

void with_names(ustore_collection_list_t& request, void (*list)(ustore_collection_list_t&) noexcept) noexcept {
    if (!request.count) {
        *request.error = {ustore_error_invalid_argument_k, "Need an output for the count"};
        return;
    }
    ustore_collection_id_t* ids = nullptr;
    ustore_collection_list_t listing = request;
    listing.collections = &ids;
    listing.modalities = nullptr;
    listing.names_offsets = nullptr;
    listing.names = nullptr;
    list(listing);
    if (request.error->status) return;
    if (request.collections) *request.collections = ids;
    if (!request.modalities && !request.names_offsets && !request.names) return;

    // The identifiers stay in the arena, as everything below is allocated beside them
    ustore_options_t const kept = ustore_options_t(request.options | ustore_option_dont_discard_memory_k);
    linked_memory_lock_t arena = linked_memory(request.arena, kept, request.error);
    if (request.error->status) return;
    names_t const names = scan_names(request.db, request.snapshot, arena, request.error);
    if (request.error->status) return;

    // Every name ordered by the collection it addresses, so each listed collection finds its own by bisection
    std::span<named_position_t> const named = arena.alloc<named_position_t>(names.count, request.error);
    if (request.error->status) return;
    for (std::size_t index = 0; index != names.count; ++index) named[index] = {names.collections[index], index};
    std::ranges::sort(named);

    std::size_t const count = *request.count;
    auto modalities = arena.alloc_or_dummy(count, request.error, request.modalities);
    growing_tape_t exported {arena};
    exported.reserve(count, request.error);
    if (request.error->status) return;
    for (std::size_t index = 0; index != count; ++index) {
        auto const found = std::ranges::lower_bound(named, ids[index], {}, &named_position_t::id);
        bool const present = found != named.end() && found->id == ids[index];
        modalities[index] = present ? names.modalities[found->position] : ustore_modality_any_k;
        exported.push_back(present ? value_view_t {names[found->position]} : value_view_t {}, request.error);
        if (request.error->status) return;
    }
    if (request.names_offsets) *request.names_offsets = exported.offsets().begin().get();
    if (request.names) *request.names = reinterpret_cast<ustore_char_t*>(exported.contents().begin().get());
}

#pragma endregion Strings and Names

} // namespace unum::ustore::nvme
