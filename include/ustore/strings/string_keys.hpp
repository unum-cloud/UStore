/**
 *  @file include/ustore/strings/string_keys.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The keys a strings collection and the names of collections file their entries under, owning their bytes,
 *      with their order and hash, and the buckets an engine with 16-byte keys alone files strings in.
 *
 *  An engine whose keys are 16 bytes, as NVMe's are, files a string under a key of its leading 15 bytes and its length
 *  byte, and the value under that key is a bucket: a sorted run of entries, each a string and its value. A string of
 *  up to 15 bytes owns its bucket alone, while longer ones sharing their leading 15 bytes share one, so a point lookup
 *  reads one bucket and a prefix walk reaches the engine as an ordered walk of keys. Keys order as their strings do,
 *  and so do the entries of a bucket, so a walk of the buckets in key order lists strings in byte order:
 *
 *  @code{.c}
 *  struct bucket {              // little-endian, unaligned
 *      uint64_t count;          // offset 0
 *      uint64_t strings[count + 1]; // offsets of every string, then the bytes they all take
 *      uint64_t values[count + 1];  // offsets of every value, then the bytes they all take
 *      char texts[];            // the strings back to back, in byte order
 *      uint8_t bytes[];         // the values back to back, in the order of their strings
 *  };
 *  @endcode
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <smashtable/shared.hpp>       // `st::expected`
#include <stringzilla/stringzilla.hpp> // `sz::string_view_t`

#include "ustore/utilities/types.hpp" // `ustore_byte_t`, `ustore_key_t`, `value_view_t`

namespace unum::ustore::strings {

using utilities::value_view_t;

/** A value, or the bytes of a string, owned so it outlives the call that wrote it. */
using blob_t = st::basic_vector<ustore_byte_t>;

/** A string of a strings collection, or the name of a collection, owning its bytes. */
struct string_key_t {

    /** Bytes of the string. */
    blob_t text;

    /** The bytes of the string. */
    std::string_view view() const noexcept { return {reinterpret_cast<char const*>(text.data()), text.size()}; }

    /** A copy owning bytes of its own, or why they could not be allocated, which the store asks of a key it
     *  records. */
    st::expected<string_key_t> copy() const noexcept {
        st::expected<blob_t> copied = text.copy();
        if (!copied) return copied.status();
        return string_key_t {std::move(*copied)};
    }
};

/** Orders strings by their bytes, unsigned. */
struct string_key_order_t {
    bool operator()(string_key_t const& first, string_key_t const& second) const noexcept {
        return sz::string_view_t {first.view()} < sz::string_view_t {second.view()};
    }
};

/** Hashes a string through StringZilla, spreading the strings of a collection over every partition. */
struct string_key_hash_t {
    std::size_t operator()(string_key_t const& stored) const noexcept {
        return sz::string_view_t {stored.view()}.hash();
    }
};

#pragma region Bucket Keys

/** Bytes of a bucket key that hold the leading bytes of a string, leaving the last one for its length. */
inline constexpr std::size_t bucket_prefix_bytes_k = sizeof(ustore_key_t) - 1;

/** The length byte of the bucket every string longer than @c bucket_prefix_bytes_k shares with its neighbors. */
inline constexpr unsigned char shared_bucket_length_k = static_cast<unsigned char>(bucket_prefix_bytes_k + 1);

/** The key the bucket of @p text sits under, ordering as the text of every string that fits a key. */
constexpr ustore_key_t bucket_key_of(std::string_view text) noexcept {
    ustore_key_t key {};
    for (std::size_t index = 0; index != bucket_prefix_bytes_k && index != text.size(); ++index)
        key.uuid.bytes[index] = static_cast<std::uint8_t>(text[index]);
    key.uuid.bytes[bucket_prefix_bytes_k] = text.size() <= bucket_prefix_bytes_k
                                                ? static_cast<std::uint8_t>(text.size())
                                                : static_cast<std::uint8_t>(shared_bucket_length_k);
    return key;
}

/** The first and the last key a walk of the strings starting with one prefix may visit, both included. */
struct bucket_window_t {
    ustore_key_t first;
    ustore_key_t last;
};

/** The keys the strings starting with @p prefix sit under, one bucket for a prefix longer than a key holds. */
constexpr bucket_window_t bucket_window_of(std::string_view prefix) noexcept {
    ustore_key_t const first = bucket_key_of(prefix);
    if (prefix.size() > bucket_prefix_bytes_k) return {first, first};

    // Every length byte a stored string takes stays below the padding of the upper bound, so it closes the window
    ustore_key_t last {};
    for (std::size_t index = 0; index != sizeof(ustore_key_t); ++index)
        last.uuid.bytes[index] = index < prefix.size() ? static_cast<std::uint8_t>(prefix[index]) : std::uint8_t {0xFF};
    return {first, last};
}

#pragma endregion Bucket Keys

#pragma region Bucket Layout

/** The word every count and offset of a bucket takes, wide enough for a bucket past 4 GB. */
using bucket_word_t = std::uint64_t;

/** One string of a bucket together with the value stored under it. */
struct bucket_entry_t {

    /** The string itself, whose bytes may hold zeros. */
    std::string_view text;

    /** The value stored under the string. */
    value_view_t value;
};

/** A sorted run of entries inside one stored value, read through @c std::memcpy, as nothing inside is aligned. */
class bucket_view_t {
    ustore_byte_t const* bytes_ = nullptr;
    std::size_t count_ = 0;

    /** The word @p offset bytes into the bucket. */
    [[nodiscard]] std::size_t word_at_(std::size_t offset) const noexcept {
        bucket_word_t word = 0;
        std::memcpy(&word, bytes_ + offset, sizeof(word));
        return static_cast<std::size_t>(word);
    }

    [[nodiscard]] std::size_t strings_offsets_begin_() const noexcept { return sizeof(bucket_word_t); }
    [[nodiscard]] std::size_t values_offsets_begin_() const noexcept { return sizeof(bucket_word_t) * (count_ + 2); }
    [[nodiscard]] std::size_t strings_begin_() const noexcept { return sizeof(bucket_word_t) * (2 * count_ + 3); }
    [[nodiscard]] std::size_t values_begin_() const noexcept {
        return strings_begin_() + word_at_(strings_offsets_begin_() + count_ * sizeof(bucket_word_t));
    }

  public:

    /** An empty bucket, which a missing value reads as. */
    bucket_view_t() noexcept = default;

    /** The bucket @p value holds, empty for a missing value and for bytes too short to be one. */
    explicit bucket_view_t(value_view_t value) noexcept {
        if (value.size() < sizeof(bucket_word_t)) return;
        bucket_word_t count = 0;
        std::memcpy(&count, value.data(), sizeof(count));
        if (count > value.size() || sizeof(bucket_word_t) * (2 * count + 3) > value.size()) return;
        bytes_ = reinterpret_cast<ustore_byte_t const*>(value.data());
        count_ = static_cast<std::size_t>(count);
    }

    /** Entries the bucket holds. */
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    /** The string of the entry at @p index. */
    [[nodiscard]] std::string_view text_of(std::size_t index) const noexcept {
        std::size_t const begin = word_at_(strings_offsets_begin_() + index * sizeof(bucket_word_t));
        std::size_t const end = word_at_(strings_offsets_begin_() + (index + 1) * sizeof(bucket_word_t));
        return {reinterpret_cast<char const*>(bytes_ + strings_begin_() + begin), end - begin};
    }

    /** The value of the entry at @p index. */
    [[nodiscard]] value_view_t value_of(std::size_t index) const noexcept {
        std::size_t const begin = word_at_(values_offsets_begin_() + index * sizeof(bucket_word_t));
        std::size_t const end = word_at_(values_offsets_begin_() + (index + 1) * sizeof(bucket_word_t));
        return value_view_t {bytes_ + values_begin_() + begin, static_cast<ustore_length_t>(end - begin)};
    }

    /** The entry at @p index. */
    [[nodiscard]] bucket_entry_t operator[](std::size_t index) const noexcept {
        return {text_of(index), value_of(index)};
    }

    /** The position of the first entry whose string is not below @p wanted. */
    [[nodiscard]] std::size_t lower_bound(std::string_view wanted) const noexcept {
        std::size_t low = 0;
        std::size_t high = count_;
        while (low != high) {
            std::size_t const middle = low + (high - low) / 2;
            if (sz::string_view_t {text_of(middle)} < sz::string_view_t {wanted}) low = middle + 1;
            else high = middle;
        }
        return low;
    }

    /** The position of @p wanted, or the size of the bucket when it holds no such string. */
    [[nodiscard]] std::size_t find(std::string_view wanted) const noexcept {
        std::size_t const position = lower_bound(wanted);
        return position != count_ && text_of(position) == wanted ? position : count_;
    }
};

/** The bytes a bucket of @p entries takes once written. */
inline std::size_t bucket_bytes(std::span<bucket_entry_t const> entries) noexcept {
    std::size_t bytes = sizeof(bucket_word_t) * (2 * entries.size() + 3);
    for (bucket_entry_t const& entry : entries) bytes += entry.text.size() + entry.value.size();
    return bytes;
}

/** Writes @p entries, already sorted by their strings, into @p bytes, which holds @c bucket_bytes of them. */
inline void write_bucket(ustore_byte_t* bytes, std::span<bucket_entry_t const> entries) noexcept {
    std::size_t const count = entries.size();
    auto const write_word = [bytes](std::size_t offset, std::size_t word) noexcept {
        bucket_word_t const written = static_cast<bucket_word_t>(word);
        std::memcpy(bytes + offset, &written, sizeof(written));
    };
    write_word(0, count);
    std::size_t const strings_offsets_begin = sizeof(bucket_word_t);
    std::size_t const values_offsets_begin = sizeof(bucket_word_t) * (count + 2);
    std::size_t const strings_begin = sizeof(bucket_word_t) * (2 * count + 3);

    std::size_t strings_bytes = 0;
    std::size_t values_bytes = 0;
    for (std::size_t index = 0; index != count; ++index) {
        write_word(strings_offsets_begin + index * sizeof(bucket_word_t), strings_bytes);
        write_word(values_offsets_begin + index * sizeof(bucket_word_t), values_bytes);
        strings_bytes += entries[index].text.size();
        values_bytes += entries[index].value.size();
    }
    write_word(strings_offsets_begin + count * sizeof(bucket_word_t), strings_bytes);
    write_word(values_offsets_begin + count * sizeof(bucket_word_t), values_bytes);

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

} // namespace unum::ustore::strings
