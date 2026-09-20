/**
 *  @file src/nvme/catalog.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The records a volume without a filesystem under it keeps, and the index they load into.
 *
 *  A volume that owns its own bytes still needs a name to reach them by, so the catalog is what a
 *  directory would have been: one slot of records near the front of the medium, republished whole
 *  every time it changes. A record names a blob by bytes rather than by text, since the name of a
 *  sorted run is the sixteen bytes of its identifier and never a path.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::to_integer`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcmp`, `std::memcpy`, `std::memset`

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/extent.hpp" // `extent_t`, `rounded_up_to`
#include "nvme/uuid.hpp"   // `uuid_t`

namespace unum::ustore::nvme {

#pragma region Checksums

/** The reflected Castagnoli polynomial, folded once per byte. */
inline constexpr std::array<std::uint32_t, 256> crc32c_table_k = [] {
    std::array<std::uint32_t, 256> table {};
    for (std::uint32_t leading = 0; leading != 256; ++leading) {
        std::uint32_t remainder = leading;
        for (std::uint32_t step = 0; step != 8; ++step)
            remainder = (remainder & 1u) ? (remainder >> 1) ^ 0x82F63B78u : remainder >> 1;
        table[leading] = remainder;
    }
    return table;
}();

/** The CRC-32C of @p bytes, continuing @p running so two spans check as though they were one. */
[[nodiscard]] inline std::uint32_t crc32c(std::span<byte_t const> bytes, std::uint32_t running = 0) noexcept {
    std::uint32_t remainder = ~running;
    for (byte_t const observed : bytes)
        remainder = (remainder >> 8) ^ crc32c_table_k[(remainder ^ std::to_integer<std::uint8_t>(observed)) & 0xFFu];
    return ~remainder;
}

/** The object representation of @p value, which is what a checksum and a write both take. */
template <typename value_type_>
[[nodiscard]] inline std::span<byte_t const> viewed_bytes(value_type_ const& value) noexcept {
    return std::span<byte_t const> {reinterpret_cast<byte_t const*>(&value), sizeof(value_type_)};
}

#pragma endregion Checksums

#pragma region Vocabulary

/** What a blob holds, which decides who reads it back and never how it is stored. */
enum class blob_kind_t : std::uint8_t {

    /** One immutable run of a level of the tree, named by its identifier. */
    sorted_run_k,

    /** The description of the collections and their columns, of which one is live. */
    schema_k,

    /** The schema a backup froze, which a restore reads instead of the live one. */
    backup_schema_k,

    /** Bytes the engine neither writes nor interprets, kept for whoever asked for them. */
    user_data_k,
};

/** Bytes the longest blob name occupies, which a record holds inline rather than pointing at. */
inline constexpr std::size_t blob_name_max_bytes_k = 192;

/** The multiple every record begins on, so the next record's fields are reachable as they lie. */
inline constexpr std::uint64_t record_alignment_k = 8;

/** A name is bytes and not text: a sorted run is named by the sixteen bytes of its identifier. */
using blob_name_t = std::span<byte_t const>;

#pragma endregion Vocabulary

#pragma region Layouts

/** The eight bytes of @p text read as one word, in the order they sit on the medium. */
[[nodiscard]] constexpr std::uint64_t magic_of(char const (&text)[9]) noexcept {
    std::uint64_t magic = 0;
    for (std::size_t index = 0; index != 8; ++index)
        magic |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(text[index])) << (index * 8);
    return magic;
}

/** The word the first eight bytes of a volume carry. */
inline constexpr std::uint64_t volume_magic_k = magic_of("USTOREV1");

/** The word the first eight bytes of a catalog slot carry. */
inline constexpr std::uint64_t catalog_slot_magic_k = magic_of("USTORECT");

/** The layout the engine writes, which a volume of another version is not mounted by. */
inline constexpr std::uint32_t volume_format_version_k = 1;

#pragma pack(push, 1)

/** The first bytes of a volume, which say where everything else on it begins. */
struct volume_header_t {

    /** @c volume_magic_k, so a volume is told apart from an unformatted medium. */
    std::uint64_t magic = 0;

    /** The layout the rest of the volume follows, @c volume_format_version_k for this one. */
    std::uint32_t format_version = 0;

    /** Bytes this header occupies, which is also what the checksum covers. */
    std::uint32_t header_bytes = 0;

    /** The database the volume belongs to, so a stray medium is not mounted into another one. */
    uuid_t database;

    /** Bytes the volume spans, as the disk reported them when it was formatted. */
    std::uint64_t capacity_bytes = 0;

    /** First byte of the catalog slot, which sits between this header and the data. */
    std::uint64_t catalog_slot_offset = 0;

    /** Bytes the catalog slot spans, which bounds how many entries a volume can name. */
    std::uint64_t catalog_slot_bytes = 0;

    /** First byte the allocator hands out, which every extent of every entry sits above. */
    std::uint64_t data_offset = 0;

    /** The multiple every allocated length takes. */
    std::uint64_t allocation_granularity = 0;

    /** The multiple a large allocation's offset takes. */
    std::uint64_t large_allocation_alignment = 0;

    /** The generation of the catalog slot this header was written beside. */
    std::uint64_t catalog_generation = 0;

    /** CRC-32C of the first @c header_bytes bytes with this field zeroed. */
    std::uint32_t checksum = 0;

    /** Zeroed, so the checksum reads the same bytes on every compiler. */
    std::uint32_t padding = 0;
};

/** Stamps @p header with the checksum a mount will verify it by. */
inline void set_header_checksum(volume_header_t& header) noexcept {
    header.checksum = 0;
    header.checksum = crc32c(viewed_bytes(header));
}

/** Whether @p header still matches the checksum it carries, which a torn write breaks. */
[[nodiscard]] inline bool header_checksum_holds(volume_header_t const& header) noexcept {
    volume_header_t copy = header;
    copy.checksum = 0;
    return crc32c(viewed_bytes(copy)) == header.checksum;
}

/** The first bytes of a catalog slot, which describe the records that follow it. */
struct catalog_slot_header_t {

    /** @c catalog_slot_magic_k, so a slot is told apart from data that outlived a format. */
    std::uint64_t magic = 0;

    /** How many times the catalog has been republished, the higher of two slots winning. */
    std::uint64_t generation = 0;

    /** Records that follow this header. */
    std::uint32_t entries_count = 0;

    /** Bytes those records occupy, padding included. */
    std::uint32_t records_bytes = 0;

    /** The sequence_number the next entry takes, so a remount never reissues one. */
    std::uint64_t next_sequence_number = 0;

    /** CRC-32C of this header with the field zeroed, continued over @c records_bytes of records. */
    std::uint32_t checksum = 0;

    /** Zeroed, so the checksum reads the same bytes on every compiler. */
    std::uint32_t padding = 0;
};

/** One cataloged blob, its name following inline and padded to the next record. */
struct catalog_record_t {

    /** Bytes this record occupies, its name and its padding included. */
    std::uint16_t record_bytes = 0;

    /** Bytes of the name that follows this record, at most @c blob_name_max_bytes_k. */
    std::uint16_t name_bytes = 0;

    /** The @c blob_kind_t the blob holds. */
    std::uint8_t kind = 0;

    /** Zeroed, so the checksum reads the same bytes on every compiler. */
    std::uint8_t reserved[3] = {};

    /** First byte of the range the blob occupies on the volume. */
    std::uint64_t extent_offset = 0;

    /** Bytes that range spans, which is the rounded length the allocator handed out. */
    std::uint64_t extent_length = 0;

    /** Bytes of the range that are the blob, the rest being the padding to the granularity. */
    std::uint64_t content_length = 0;

    /** When the entry was made, ordering two blobs that claim the same role. */
    std::uint64_t sequence_number = 0;
};

#pragma pack(pop)

static_assert(sizeof(volume_header_t) == 96, "A volume header is what the format says it is");
static_assert(sizeof(catalog_slot_header_t) == 40, "A catalog slot header is what the format says it is");
static_assert(sizeof(catalog_record_t) == 40, "A catalog record is what the format says it is");

/** Bytes one record occupies for a name of @p name_bytes, its padding to the next record included. */
[[nodiscard]] constexpr std::uint32_t record_bytes_of(std::uint32_t name_bytes) noexcept {
    return static_cast<std::uint32_t>(rounded_up_to(sizeof(catalog_record_t) + name_bytes, record_alignment_k));
}

#pragma endregion Layouts

#pragma region Index

/** One cataloged blob as the index holds it, with its name inline rather than borrowed. */
struct catalog_entry_t {

    /** The name's bytes, of which the first @c name_bytes are the name. */
    std::array<byte_t, blob_name_max_bytes_k> name_storage {};

    /** Bytes of the name, which is never zero for an entry the catalog holds. */
    std::uint16_t name_bytes = 0;

    /** What the blob holds. */
    blob_kind_t kind = blob_kind_t::user_data_k;

    /** The range the blob occupies on the volume. */
    extent_t extent;

    /** Bytes of that range that are the blob. */
    std::uint64_t content_length = 0;

    /** When the entry was made. */
    std::uint64_t sequence_number = 0;

    /** The name, which borrows the entry and dies with it. */
    [[nodiscard]] blob_name_t name() const noexcept { return blob_name_t {name_storage.data(), name_bytes}; }
};

/**
 *  @brief Every blob one volume names, loaded from one slot and republished as one image.
 *
 *  The checksum covers the whole image rather than each record, since a per-record checksum would
 *  let a torn write publish a prefix of the new catalog as a complete one. A name is compared as
 *  bytes throughout, so nothing here ever reads past the length a record declares.
 */
class catalog_t {
  public:

    catalog_t() noexcept = default;
    catalog_t(catalog_t&&) noexcept = default;
    catalog_t& operator=(catalog_t&&) noexcept = default;
    catalog_t(catalog_t const&) = delete;
    catalog_t& operator=(catalog_t const&) = delete;

    /**
     *  @brief Reads @p image into the index, leaving the index untouched when it will not read.
     *  @param[in] image The slot as it was read off the volume, at least its header long.
     *  @return Success, or @c status_code_t::malformed_k for a checksum, a length or a kind that
     *      does not hold.
     */
    status_t load(std::span<byte_t const> image) noexcept {
        if (image.size() < sizeof(catalog_slot_header_t))
            return status_t {status_code_t::malformed_k, "The catalog slot is shorter than its header"};

        catalog_slot_header_t header {};
        std::memcpy(&header, image.data(), sizeof(header));
        if (header.magic != catalog_slot_magic_k)
            return status_t {status_code_t::malformed_k, "The catalog slot carries another magic"};
        if (header.records_bytes > image.size() - sizeof(header))
            return status_t {status_code_t::malformed_k, "The catalog records run past the image"};

        std::uint32_t const stated = header.checksum;
        header.checksum = 0;
        std::span<byte_t const> const records = image.subspan(sizeof(header), header.records_bytes);
        if (crc32c(records, crc32c(viewed_bytes(header))) != stated)
            return status_t {status_code_t::malformed_k, "The catalog slot does not match its checksum"};

        st::basic_vector<catalog_entry_t> parsed;
        std::size_t offset = 0;
        for (std::uint32_t counted = 0; counted != header.entries_count; ++counted) {
            if (records.size() - offset < sizeof(catalog_record_t))
                return status_t {status_code_t::malformed_k, "A catalog record runs past the records"};
            catalog_record_t record {};
            std::memcpy(&record, records.data() + offset, sizeof(record));
            std::size_t const record_bytes = record.record_bytes;
            std::size_t const name_bytes = record.name_bytes;
            if (name_bytes > blob_name_max_bytes_k)
                return status_t {status_code_t::malformed_k, "A blob name is longer than the format allows"};
            if (record_bytes < sizeof(record) + name_bytes || record_bytes % record_alignment_k != 0)
                return status_t {status_code_t::malformed_k, "A catalog record does not hold its own name"};
            if (record_bytes > records.size() - offset)
                return status_t {status_code_t::malformed_k, "A catalog record runs past the records"};
            if (record.kind > static_cast<std::uint8_t>(blob_kind_t::user_data_k))
                return status_t {status_code_t::malformed_k, "A catalog record names no kind"};

            catalog_entry_t entry;
            std::memcpy(entry.name_storage.data(), records.data() + offset + sizeof(record), name_bytes);
            entry.name_bytes = record.name_bytes;
            entry.kind = static_cast<blob_kind_t>(record.kind);
            entry.extent = extent_t {record.extent_offset, record.extent_length};
            entry.content_length = record.content_length;
            entry.sequence_number = record.sequence_number;
            if (parsed.push_back(std::move(entry)) != st::status_t::success_k)
                return status_t {status_code_t::out_of_memory_k, "The catalog index would not grow"};
            offset += record_bytes;
        }
        if (offset != records.size())
            return status_t {status_code_t::malformed_k, "The catalog records hold bytes no entry claims"};

        entries_ = std::move(parsed);
        next_sequence_number_ = header.next_sequence_number;
        return {};
    }

    /**
     *  @brief Writes the whole index into @p image as one slot of @p generation.
     *  @param[out] image The slot to write, which is left untouched when it is too small.
     *  @param[in] generation What to sequence_number the slot with, which the mount raises on every publish.
     *  @return Bytes written, or @c status_code_t::capacity_exhausted_k when the slot is too small.
     */
    expected<std::uint64_t> save(std::span<byte_t> image, std::uint64_t generation) noexcept {
        std::uint64_t const needed = image_bytes();
        if (needed > image.size() || needed - sizeof(catalog_slot_header_t) > UINT32_MAX ||
            entries_.size() > UINT32_MAX)
            return status_t {status_code_t::capacity_exhausted_k, "The catalog does not fit the slot"};

        std::size_t offset = sizeof(catalog_slot_header_t);
        for (std::size_t index = 0; index != entries_.size(); ++index) {
            catalog_entry_t const& entry = entries_[index];
            catalog_record_t record {};
            record.record_bytes = static_cast<std::uint16_t>(record_bytes_of(entry.name_bytes));
            record.name_bytes = entry.name_bytes;
            record.kind = static_cast<std::uint8_t>(entry.kind);
            record.extent_offset = entry.extent.offset;
            record.extent_length = entry.extent.length;
            record.content_length = entry.content_length;
            record.sequence_number = entry.sequence_number;
            std::memcpy(image.data() + offset, &record, sizeof(record));
            std::memcpy(image.data() + offset + sizeof(record), entry.name_storage.data(), entry.name_bytes);
            std::memset(image.data() + offset + sizeof(record) + entry.name_bytes, 0,
                        record.record_bytes - sizeof(record) - entry.name_bytes);
            offset += record.record_bytes;
        }

        catalog_slot_header_t header {};
        header.magic = catalog_slot_magic_k;
        header.generation = generation;
        header.entries_count = static_cast<std::uint32_t>(entries_.size());
        header.records_bytes = static_cast<std::uint32_t>(needed - sizeof(catalog_slot_header_t));
        header.next_sequence_number = next_sequence_number_;
        header.checksum = crc32c(std::span<byte_t const> {image}.subspan(sizeof(header), header.records_bytes),
                                 crc32c(viewed_bytes(header)));
        std::memcpy(image.data(), &header, sizeof(header));
        return needed;
    }

    /** The entry @p name belongs to, or @c nullptr when the catalog names no such blob. */
    [[nodiscard]] catalog_entry_t const* find(blob_name_t name) const noexcept {
        for (std::size_t index = 0; index != entries_.size(); ++index) {
            catalog_entry_t const& entry = entries_[index];
            if (entry.name_bytes != name.size()) continue;
            if (std::memcmp(entry.name_storage.data(), name.data(), name.size()) == 0) return &entry;
        }
        return nullptr;
    }

    /**
     *  @brief Names a blob that is already on the volume, stamping it as the newest of its kind.
     *  @param[in] name The bytes the blob answers to, between one and @c blob_name_max_bytes_k of them.
     *  @param[in] kind What the blob holds.
     *  @param[in] extent The range it occupies, as the allocator handed it out.
     *  @param[in] content_length Bytes of that range that are the blob.
     *  @return Success, @c status_code_t::already_exists_k for a name the catalog holds, or
     *      @c status_code_t::invalid_argument_k for a name the format cannot carry.
     */
    status_t insert(blob_name_t name, blob_kind_t kind, extent_t extent, std::uint64_t content_length) noexcept {
        if (name.empty() || name.size() > blob_name_max_bytes_k)
            return status_t {status_code_t::invalid_argument_k, "A blob name is one byte to the format's limit"};
        if (find(name)) return status_t {status_code_t::already_exists_k, "The catalog names that blob already"};

        catalog_entry_t entry;
        std::memcpy(entry.name_storage.data(), name.data(), name.size());
        entry.name_bytes = static_cast<std::uint16_t>(name.size());
        entry.kind = kind;
        entry.extent = extent;
        entry.content_length = content_length;
        entry.sequence_number = next_sequence_number_;
        if (entries_.push_back(std::move(entry)) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "The catalog index would not grow"};
        ++next_sequence_number_;
        return {};
    }

    /** Drops @p name, answering the range it held so the allocator can take it back. */
    expected<extent_t> erase(blob_name_t name) noexcept {
        catalog_entry_t const* const found = find(name);
        if (!found) return status_t {status_code_t::not_found_k, "The catalog names no such blob"};
        std::size_t const position = static_cast<std::size_t>(found - entries_.data());
        extent_t const extent = entries_[position].extent;
        for (std::size_t index = position + 1; index != entries_.size(); ++index)
            entries_[index - 1] = std::move(entries_[index]);
        entries_.pop_back();
        return extent;
    }

    /** Blobs the catalog names. */
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /** Bytes @c save would write, which a mount compares against the slot it reserved. */
    [[nodiscard]] std::uint64_t image_bytes() const noexcept {
        std::uint64_t bytes = sizeof(catalog_slot_header_t);
        for (std::size_t index = 0; index != entries_.size(); ++index)
            bytes += record_bytes_of(entries_[index].name_bytes);
        return bytes;
    }

    /** Hands every entry of @p kind to @p visitor, in the order the slot holds them. */
    template <typename visitor_type_>
    void for_each(blob_kind_t kind, visitor_type_&& visitor) const noexcept {
        for (std::size_t index = 0; index != entries_.size(); ++index)
            if (entries_[index].kind == kind) visitor(entries_[index]);
    }

  private:

    /** Every entry, in the order the slot holds them. */
    st::basic_vector<catalog_entry_t> entries_;

    /** The sequence_number the next entry takes, raised by every insert and restored by every load. */
    std::uint64_t next_sequence_number_ = 1;
};

#pragma endregion Index

} // namespace unum::ustore::nvme
