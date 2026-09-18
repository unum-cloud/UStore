/**
 *  @file src/nvme/delta_coding.hpp
 *  @author Ash Vardanian
 *  @date May 23, 2021
 *  @brief Delta-baseline compression of integer arrays, in blocks of three method and five length bits.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int8_t`, `std::int64_t`

#include <span> // `std::span`

#include <ustore/types.hpp> // `u8_t`

namespace unum::ustore::nvme {

/** Compresses @p values into @p output, answering the filled part of @p output. */
std::span<u8_t> delta_coding_compress(std::span<std::int64_t const> values, std::span<u8_t> output) noexcept;

/** Compresses @p values into @p output, answering the filled part of @p output. */
std::span<u8_t> delta_coding_compress(std::span<std::int32_t const> values, std::span<u8_t> output) noexcept;

/** Compresses @p values into @p output, answering the filled part of @p output. */
std::span<u8_t> delta_coding_compress(std::span<std::int16_t const> values, std::span<u8_t> output) noexcept;

/** Compresses @p values into @p output, answering the filled part of @p output. */
std::span<u8_t> delta_coding_compress(std::span<std::int8_t const> values, std::span<u8_t> output) noexcept;

/** Bytes the compressed form of @p values occupies. */
std::size_t delta_coding_compressed_size(std::span<std::int64_t const> values) noexcept;

/** Bytes the compressed form of @p values occupies. */
std::size_t delta_coding_compressed_size(std::span<std::int32_t const> values) noexcept;

/** Bytes the compressed form of @p values occupies. */
std::size_t delta_coding_compressed_size(std::span<std::int16_t const> values) noexcept;

/** Bytes the compressed form of @p values occupies. */
std::size_t delta_coding_compressed_size(std::span<std::int8_t const> values) noexcept;

/** Decompresses @p input into @p values, answering the consumed part of @p input. */
std::span<u8_t const> delta_coding_decompress(std::span<u8_t const> input, std::span<std::int64_t> values) noexcept;

/** Decompresses @p input into @p values, answering the consumed part of @p input. */
std::span<u8_t const> delta_coding_decompress(std::span<u8_t const> input, std::span<std::int32_t> values) noexcept;

/** Decompresses @p input into @p values, answering the consumed part of @p input. */
std::span<u8_t const> delta_coding_decompress(std::span<u8_t const> input, std::span<std::int16_t> values) noexcept;

/** Decompresses @p input into @p values, answering the consumed part of @p input. */
std::span<u8_t const> delta_coding_decompress(std::span<u8_t const> input, std::span<std::int8_t> values) noexcept;

/** An upper bound on the bytes the compressed form of @p count values of @p value_size bytes takes. */
constexpr std::size_t delta_coding_worst_case_size(std::size_t count, std::size_t value_size) noexcept {
    return count * (value_size + 1) + sizeof(std::uint64_t);
}

} // namespace unum::ustore::nvme
