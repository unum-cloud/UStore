/**
 *  @file src/nvme/stored_key.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief Keys UStoreNVMe orders key slots by: the bounds of every key type, and the 16-byte key ordered like memcmp.
 */
#pragma once

#include <cstdint> // `std::uint8_t`, `std::uint64_t`

#include <concepts>    // `std::convertible_to`, `std::same_as`
#include <limits>      // `std::numeric_limits`
#include <type_traits> // `std::is_arithmetic_v`, `std::is_trivially_copyable_v`

namespace unum::ustore::nvme {

/**
 *  @brief A 16-byte key held as two words, each read big-endian from its eight bytes.
 *
 *  Comparing @c high and then @c low as unsigned words orders keys exactly as @c memcmp orders their bytes,
 *  so an integer identity below 2^64 keeps @c high zero and orders as its value.
 */
struct uuid_stored_key_t {

    /** The first eight bytes, read big-endian. */
    std::uint64_t high = 0;

    /** The last eight bytes, read big-endian. */
    std::uint64_t low = 0;

    /** Reads a key from the 16 bytes at @p bytes. */
    static constexpr uuid_stored_key_t from_bytes(std::uint8_t const* bytes) noexcept {
        uuid_stored_key_t key;
        for (int index = 0; index != 8; ++index) {
            key.high = (key.high << 8) | bytes[index];
            key.low = (key.low << 8) | bytes[8 + index];
        }
        return key;
    }

    /** Writes the key's 16 bytes to @p bytes. */
    constexpr void to_bytes(std::uint8_t* bytes) const noexcept {
        for (int index = 0; index != 8; ++index) {
            bytes[index] = static_cast<std::uint8_t>(high >> (56 - 8 * index));
            bytes[8 + index] = static_cast<std::uint8_t>(low >> (56 - 8 * index));
        }
    }

    /** Whether both keys hold the same 16 bytes. */
    friend constexpr bool operator==(uuid_stored_key_t const& first, uuid_stored_key_t const& second) noexcept {
        return (first.high == second.high) & (first.low == second.low);
    }

    /** Whether @p first orders before @p second, as @c memcmp orders their bytes. */
    friend constexpr bool operator<(uuid_stored_key_t const& first, uuid_stored_key_t const& second) noexcept {
        return (first.high < second.high) | ((first.high == second.high) & (first.low < second.low));
    }
};

/** The least and the greatest value of a key type, one specialization per type. */
template <typename value_type_>
struct limit;

/** Bounds of an arithmetic key, as @c std::numeric_limits reports them. */
template <typename value_type_>
    requires std::is_arithmetic_v<value_type_>
struct limit<value_type_> {

    /** The least value. */
    static constexpr value_type_ min() noexcept { return min_k; }

    /** The greatest value. */
    static constexpr value_type_ max() noexcept { return max_k; }

  private:
    static constexpr value_type_ min_k = std::numeric_limits<value_type_>::lowest();
    static constexpr value_type_ max_k = std::numeric_limits<value_type_>::max();
};

/** Bounds of a 16-byte key: all bytes zero and all bytes one. */
template <>
struct limit<uuid_stored_key_t> {

    /** The key of 16 zero bytes. */
    static constexpr uuid_stored_key_t min() noexcept { return {0, 0}; }

    /** The key of 16 bytes with every bit set. */
    static constexpr uuid_stored_key_t max() noexcept { return {~std::uint64_t {0}, ~std::uint64_t {0}}; }
};

/** A key a key slot carries: trivially copyable, ordered by @c < and @c ==, with bounds from @c limit. */
template <typename stored_key_type_>
concept stored_key_concept = std::is_trivially_copyable_v<stored_key_type_> &&
                             requires(stored_key_type_ const& first, stored_key_type_ const& second) {
                                 { first < second } -> std::convertible_to<bool>;
                                 { first == second } -> std::convertible_to<bool>;
                                 { limit<stored_key_type_>::max() } -> std::same_as<stored_key_type_>;
                             };

} // namespace unum::ustore::nvme
