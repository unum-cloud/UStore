/**
 *  @file src/nvme/uuid.hpp
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief The 128-bit identifier of a database, a collection and a sorted run, with its text form.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#if defined(_WIN32)
#include <windows.h> // `NTSTATUS`, which <bcrypt.h> needs
#include <bcrypt.h>  // `BCryptGenRandom`
#else
#include <sys/random.h> // `getentropy`
#endif

#include <compare> // `std::strong_ordering`
#include <span>    // `std::span`

namespace unum::ustore::nvme {

/** The bit pattern of octet 8, which says whose layout the remaining bytes follow. */
enum class uuid_variant_t : std::uint8_t {

    /** The @c 0xxx pattern of the Apollo Network Computing System. */
    network_computing_system_k,

    /** The @c 10xx pattern of RFC 4122, which every identifier this engine mints carries. */
    rfc_k,

    /** The @c 110x pattern of the Component Object Model. */
    microsoft_k,

    /** The @c 111x pattern, which no specification claims. */
    reserved_k,
};

/**
 *  @brief A 128-bit identifier held as sixteen bytes, printed as the canonical 36 characters.
 *
 *  A zero identifier names nothing, which is what a default-constructed one holds and what
 *  @c is_valid reports; every minted identifier is version 4, so all but six of its bits are random.
 */
struct uuid_t {

    /** Bytes the identifier occupies in memory and on disk. */
    static constexpr std::size_t size_bytes_k = 16;

    /** Characters the canonical @c 8-4-4-4-12 text form occupies, dashes included. */
    static constexpr std::size_t printed_length_k = 36;

    /** The sixteen bytes, in the order they are printed and stored. */
    std::uint8_t bytes[size_bytes_k] = {};

    uuid_t() = default;

    /** Reads an identifier from the sixteen bytes at @p source. */
    explicit uuid_t(std::span<std::uint8_t const, size_bytes_k> source) noexcept {
        std::memcpy(bytes, source.data(), size_bytes_k);
    }

    /** Whose layout the remaining bytes follow, read off octet 8. */
    constexpr uuid_variant_t variant() const noexcept {
        if ((bytes[8] & 0x80) == 0x00) return uuid_variant_t::network_computing_system_k;
        if ((bytes[8] & 0xC0) == 0x80) return uuid_variant_t::rfc_k;
        if ((bytes[8] & 0xE0) == 0xC0) return uuid_variant_t::microsoft_k;
        return uuid_variant_t::reserved_k;
    }

    /** Whether the identifier names anything, which a zero one does not. */
    constexpr bool is_valid() const noexcept {
        std::uint8_t any = 0;
        for (std::size_t index = 0; index != size_bytes_k; ++index) any |= bytes[index];
        return any != 0;
    }

    /** The first eight bytes read as a big-endian word, which orders identifiers as @c memcmp does. */
    constexpr std::uint64_t high_word() const noexcept { return word_(0); }

    /** The last eight bytes read as a big-endian word. */
    constexpr std::uint64_t low_word() const noexcept { return word_(8); }

    friend constexpr bool operator==(uuid_t const&, uuid_t const&) = default;

    friend constexpr std::strong_ordering operator<=>(uuid_t const& first, uuid_t const& second) noexcept {
        if (std::strong_ordering const high = first.high_word() <=> second.high_word(); high != 0) return high;
        return first.low_word() <=> second.low_word();
    }

    /** Writes the canonical @c 8-4-4-4-12 text form into @p destination, without a terminator. */
    constexpr void print(std::span<char, printed_length_k> destination) const noexcept {
        std::size_t written = 0;
        for (std::size_t index = 0; index != size_bytes_k; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) destination[written++] = '-';
            destination[written++] = digits_k[bytes[index] >> 4];
            destination[written++] = digits_k[bytes[index] & 0x0F];
        }
    }

    /** Whether @p text is 36 characters of the canonical form, so @c parse would read it whole. */
    static constexpr bool parses(std::span<char const, printed_length_k> text) noexcept {
        std::size_t read = 0;
        for (std::size_t index = 0; index != size_bytes_k; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10)
                if (text[read++] != '-') return false;
            if (value_of_(text[read++]) == invalid_digit_k) return false;
            if (value_of_(text[read++]) == invalid_digit_k) return false;
        }
        return true;
    }

    /** Reads the canonical form out of @p text, which @c parses has already accepted. */
    static constexpr uuid_t parse(std::span<char const, printed_length_k> text) noexcept {
        uuid_t identifier;
        std::size_t read = 0;
        for (std::size_t index = 0; index != size_bytes_k; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) ++read;
            std::uint8_t const high = value_of_(text[read++]);
            std::uint8_t const low = value_of_(text[read++]);
            identifier.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
        }
        return identifier;
    }

  private:

    /** The lowercase hexadecimal alphabet the text form prints. */
    static constexpr char digits_k[17] = "0123456789abcdef";

    /** What @c value_of_ answers for a character that is no hexadecimal digit. */
    static constexpr std::uint8_t invalid_digit_k = 0xFF;

    /** The value of one hexadecimal digit, in either case. */
    static constexpr std::uint8_t value_of_(char digit) noexcept {
        if (digit >= '0' && digit <= '9') return static_cast<std::uint8_t>(digit - '0');
        if (digit >= 'a' && digit <= 'f') return static_cast<std::uint8_t>(digit - 'a' + 10);
        if (digit >= 'A' && digit <= 'F') return static_cast<std::uint8_t>(digit - 'A' + 10);
        return invalid_digit_k;
    }

    /** The eight bytes at @p offset read as one big-endian word. */
    constexpr std::uint64_t word_(std::size_t offset) const noexcept {
        std::uint64_t word = 0;
        for (std::size_t index = 0; index != 8; ++index) word = (word << 8) | bytes[offset + index];
        return word;
    }
};

static_assert(sizeof(uuid_t) == uuid_t::size_bytes_k, "An identifier is its sixteen bytes and nothing else");

/** Fills @p destination with bytes from the operating system, answering whether it could. */
inline bool fill_with_entropy(std::span<std::uint8_t> destination) noexcept {
#if defined(_WIN32)
    return ::BCryptGenRandom(nullptr, destination.data(), static_cast<ULONG>(destination.size()),
                             BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
    return ::getentropy(destination.data(), destination.size()) == 0;
#endif
}

/**
 *  @brief Mints a version 4 identifier from the entropy the operating system hands out.
 *
 *  Answers a zero identifier, which @c is_valid rejects, when the system refuses entropy; the
 *  caller learns of that through the identifier rather than through a status, since every source
 *  of a fresh identifier already validates it before recording it.
 */
inline uuid_t uuid_generator() noexcept {
    uuid_t identifier;
    if (!fill_with_entropy(identifier.bytes)) return {};
    identifier.bytes[6] = static_cast<std::uint8_t>((identifier.bytes[6] & 0x0F) | 0x40);
    identifier.bytes[8] = static_cast<std::uint8_t>((identifier.bytes[8] & 0x3F) | 0x80);
    return identifier;
}

/** The identifier folded onto one word, for a table keyed by a number rather than by the bytes. */
constexpr std::uint64_t uuid_to_u64(uuid_t const& identifier) noexcept {
    std::uint64_t mixed = identifier.high_word() ^ (identifier.low_word() * 0x9E3779B97F4A7C15ull);
    mixed ^= mixed >> 30;
    mixed *= 0xBF58476D1CE4E5B9ull;
    mixed ^= mixed >> 27;
    mixed *= 0x94D049BB133111EBull;
    return mixed ^ (mixed >> 31);
}

} // namespace unum::ustore::nvme
