/**
 *  @file include/ustore/utilities/nvme_volume.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Volumes, the names they resolve to extents and the router placing blobs across them, and the mount of
 *      one database's storage: the transport it probes, the memory it pins, the volumes it opens.
 *
 *  A volume maps names to bytes and a transport moves them; the pair is one disk. Where there is a
 *  filesystem the filesystem is the catalog, so a run is a file, publication is a rename, and a
 *  crash leaves a partial file the mount deletes. Where there is none - a namespace, a passthrough
 *  character device, an SPDK blobstore - names live in a catalog of two slots inside the volume, and
 *  publication is the slot swap behind an image checksum. The catalog is what a directory would have been: one
 *  slot of records near the front of the medium, republished whole every time it changes. A record names a blob
 *  by bytes rather than by text, since the name of a sorted run is the sixteen bytes of its identifier and never
 *  a path.
 *
 *  Both carry the order @c verification/durability.pml checks: the bytes are written, flushed,
 *  recorded, and only then is the record synced, so an extent missing from the catalog is free after
 *  a crash. The best-fit free map never reaches the medium: a mount replays the catalog and reserves the extent
 *  of every entry it names, so an extent no entry names is free by construction. Neither is on the read path -
 *  a name is resolved once when a run is opened, and every block read afterwards is an extent plus a submit.
 *
 *  A machine has one volume per data directory rather than one per database, so the router above the
 *  volumes is a placement decision and nothing else: it holds the mounted volumes, answers which of
 *  them a blob of a given length should go to, and stamps every handle it hands back with the volume
 *  that owns the bytes. It keeps no lock and no claim, since one volume's catalog and free map are mutated by
 *  one thread at a time.
 *
 *  A database owns exactly one mount, and everything it reads or writes hangs off it. The transport is chosen
 *  once, by setting a ring and an AIO context up and taking each down again rather than by reading a version
 *  number, because a kernel that has io_uring still refuses it under Docker's seccomp profile and under
 *  @c io_uring_disabled, and keeps AIO in both; what the probes settled on is reported back in
 *  @c mount_report_t rather than inferred by a caller. The three file transports are erased at the disk, and the
 *  branch is paid once per batch on @c submit and @c poll, each of which wraps a syscall.
 *
 *  Teardown is an order, not a destructor: a queue pair may not be released with work outstanding, a
 *  pinned range may not be unpinned while a request addresses it, and a medium may not be
 *  deregistered while one names it. So the channels drain first, the arena is unpinned next, the targets
 *  go back after that, and the volumes are dropped last. The resources over a mount add the budgets, the
 *  threads and the gate background jobs pass.
 */

#pragma once

#include <cassert> // `assert`
#include <cerrno>  // `errno`, `EEXIST`
#include <cstddef> // `std::size_t`, `std::to_integer`
#include <cstdint> // `std::int32_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcmp`, `std::memcpy`, `std::memset`, `std::strlen`

#if defined(_WIN32)
#include <windows.h> // `CreateDirectoryA`, `DeleteFileA`, `RemoveDirectoryA`
#else
#include <sys/stat.h> // `mkdir`
#include <unistd.h>   // `rmdir`, `unlink`
#endif

#include <algorithm> // `std::clamp`
#include <array>     // `std::array`
#include <atomic>    // `std::memory_order`
#include <bit>       // `std::has_single_bit`
#include <concepts>  // `std::same_as`
#include <memory>    // `std::shared_ptr`, `std::make_shared`
#include <new>       // placement `new`
#include <optional>  // `std::optional`
#include <span>      // `std::span`
#include <utility>   // `std::forward`, `std::move`
#include <variant>   // `std::variant`, `std::visit`, `std::get_if`, `std::monostate`, `std::in_place_type`

#include <forkunion/atomics.hpp>       // `fu::preferred_atomic_ref`
#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <stringzilla/stringzilla.hpp> // `sz::string_t`

#include "ustore/utilities/nvme_channels.hpp"         // `io_channel_t`, `basic_io_channel_pool`, `pooled_buffer_t`
#include "ustore/utilities/nvme_disks.hpp"            // `blocking_files_disk_t`, `memory_disk_t`
#include "ustore/utilities/nvme_linux_aio_disk.hpp"   // `linux_aio_files_disk_t`, `linux_aio_probe_t`
#include "ustore/utilities/nvme_linux_uring_disk.hpp" // `linux_uring_files_disk_t`, `linux_uring_probe_t`
#include "ustore/utilities/nvme_ring.hpp"             // `extent_t`, `uuid_t`, `ring_concept`, `string_t`
#include "ustore/utilities/ram_device.hpp"            // `executor_t`, `caller_exclusivity_t`
#include "ustore/utilities/types.hpp"                 // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::utilities {

#pragma region Text

/** Text the engine owns, grown through the fallible members that never throw. */
using string_t = sz::string_t;

/** A view of text the engine does not own. */
using string_view_t = sz::string_view_t;

/** The bytes of a string: its 16-bit length, then its characters. */
inline std::size_t storage_requirements(string_t const& text) noexcept { return sizeof(std::uint16_t) + text.size(); }

/** Saves the 16-bit length of @p text, then its characters. */
inline status_t save(string_t const& text, bytes_span_t destination) noexcept {
    if (text.size() > std::numeric_limits<std::uint16_t>::max())
        return status_t {status_code_t::capacity_exhausted_k, "Data is too big"};
    if (destination.size() < storage_requirements(text))
        return status_t {status_code_t::invalid_argument_k, "Buffer is too small"};
    std::uint16_t const length = static_cast<std::uint16_t>(text.size());
    std::memcpy(destination.data(), &length, sizeof(length));
    std::memcpy(destination.data() + sizeof(length), text.data(), length);
    return {};
}

/** Loads a string saved as its 16-bit length, then its characters. */
inline status_t load(string_t& text, bytes_view_t source) noexcept {
    std::uint16_t length = 0;
    if (status_t const status = load(length, source); status.failed()) return status;
    if (source.size() - sizeof(length) < length) return status_t {status_code_t::malformed_k, "Corrupted data"};
    char const* const characters = reinterpret_cast<char const*>(source.data() + sizeof(length));
    if (sz::failed(text.assign(string_view_t {characters, length})))
        return status_t {status_code_t::out_of_memory_k, "No room for a loaded string"};
    return {};
}

#pragma endregion Text

#pragma region Extent Allocator

#pragma region Granularities

/** Bytes from which an allocation rounds its offset as well as its length. */
inline constexpr std::uint64_t large_allocation_bytes_k = std::uint64_t {8} << 20;

/** The offset alignment such an allocation takes, coarse enough for a stripe and an erase block. */
inline constexpr std::uint64_t large_allocation_alignment_k = std::uint64_t {1} << 20;

#pragma endregion Granularities

#pragma region Allocator

/**
 *  @brief The free ranges of one contiguous region, handed out by best fit and coalesced on return.
 *
 *  Two granularities, not one: every allocation rounds its length up to the disk's own granularity,
 *  and only an allocation of @c large_allocation_bytes_k or more rounds its offset up to the coarse
 *  alignment as well, since rounding a kilobyte of blob to a megabyte would waste three orders of
 *  magnitude. The map holds no allocation, only what is left, so it is rebuilt by @c reserve at
 *  mount and never written down.
 */
class extent_allocator_t {
  public:

    extent_allocator_t() noexcept = default;
    extent_allocator_t(extent_allocator_t&&) noexcept = default;
    extent_allocator_t& operator=(extent_allocator_t&&) noexcept = default;
    extent_allocator_t(extent_allocator_t const&) = delete;
    extent_allocator_t& operator=(extent_allocator_t const&) = delete;

    /**
     *  @brief Builds an empty map of @p region, refusing a shape no allocation could satisfy.
     *  @param[in] region The bytes the volume hands out, which begins and ends on @p granularity.
     *  @param[in] granularity The multiple every allocated length takes, a power of two.
     *  @param[in] large_alignment The multiple a large allocation's offset takes, at least @p granularity.
     *  @return The map, or @c status_code_t::invalid_argument_k for a shape it cannot serve.
     */
    static expected<extent_allocator_t> make(extent_t region, std::uint64_t granularity,
                                             std::uint64_t large_alignment) noexcept {
        if (!std::has_single_bit(granularity) || !std::has_single_bit(large_alignment))
            return status_t {status_code_t::invalid_argument_k, "An allocation granularity is a power of two"};
        if (large_alignment < granularity)
            return status_t {status_code_t::invalid_argument_k, "A large allocation is aligned no more finely"};
        if (!region.aligned_to(granularity) || region.offset > UINT64_MAX - region.length)
            return status_t {status_code_t::invalid_argument_k, "A region begins and ends on its granularity"};

        extent_allocator_t allocator;
        allocator.region_ = region;
        allocator.granularity_ = granularity;
        allocator.large_alignment_ = large_alignment;
        allocator.free_bytes_ = region.length;
        if (!region.empty() && allocator.free_ranges_.push_back(extent_t {region}) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "The free map would not hold the region"};
        return allocator;
    }

    /**
     *  @brief Takes @p extent out of the map verbatim, which is how a mount replays the catalog.
     *  @param[in] extent The range one catalog entry occupies, already rounded when it was allocated.
     *  @return Success, or @c status_code_t::malformed_k when the catalog claims it twice or claims
     *      bytes the volume does not have.
     */
    status_t reserve(extent_t extent) noexcept {
        if (extent.empty()) return {};
        if (!region_.contains(extent))
            return status_t {status_code_t::malformed_k, "A cataloged extent falls outside the volume"};
        for (std::size_t index = 0; index != free_ranges_.size(); ++index)
            if (free_ranges_[index].contains(extent)) return carve_(index, extent);
        return status_t {status_code_t::malformed_k, "Two cataloged extents overlap"};
    }

    /**
     *  @brief Hands out the range that leaves the least behind, rounded as the two granularities ask.
     *  @param[in] length Bytes the caller needs, rounded up to the granularity before the search.
     *  @return The range, or @c status_code_t::capacity_exhausted_k when no free range fits.
     */
    expected<extent_t> allocate(std::uint64_t length) noexcept {
        if (length == 0) return status_t {status_code_t::invalid_argument_k, "An allocation of no bytes names nothing"};
        std::uint64_t const rounded = round_up_to_multiple(length, granularity_);
        if (rounded < length)
            return status_t {status_code_t::capacity_exhausted_k, "The rounded length does not fit an address"};
        std::uint64_t const alignment = rounded >= large_allocation_bytes_k ? large_alignment_ : granularity_;

        // A linear scan, since the range count is bounded by the number of live blobs.
        std::size_t chosen = free_ranges_.size();
        std::uint64_t chosen_leftover = 0;
        std::uint64_t chosen_offset = 0;
        for (std::size_t index = 0; index != free_ranges_.size(); ++index) {
            extent_t const range = free_ranges_[index];
            std::uint64_t const start = round_up_to_multiple(range.offset, alignment);
            if (start < range.offset || start > range.end()) continue;
            std::uint64_t const reachable = range.end() - start;
            if (reachable < rounded) continue;
            std::uint64_t const leftover = reachable - rounded;
            if (chosen != free_ranges_.size() && leftover >= chosen_leftover) continue;
            chosen = index;
            chosen_leftover = leftover;
            chosen_offset = start;
        }
        if (chosen == free_ranges_.size())
            return status_t {status_code_t::capacity_exhausted_k, "No free range fits the allocation"};

        extent_t const taken {chosen_offset, rounded};
        status_t const carved = carve_(chosen, taken);
        if (carved.failed()) return carved;
        return taken;
    }

    /**
     *  @brief Gives @p extent back, merging it with the free range before it and the one after it.
     *  @param[in] extent The range one allocation held, exactly as it was handed out.
     *  @return Success, or @c status_code_t::invalid_argument_k when any of those bytes is free already.
     */
    status_t release(extent_t extent) noexcept {
        if (extent.empty()) return {};
        if (!region_.contains(extent))
            return status_t {status_code_t::invalid_argument_k, "A released extent falls outside the volume"};

        std::size_t position = 0;
        while (position != free_ranges_.size() && free_ranges_[position].offset < extent.offset) ++position;
        if (position != 0 && free_ranges_[position - 1].end() > extent.offset)
            return status_t {status_code_t::invalid_argument_k, "A released extent is free already"};
        if (position != free_ranges_.size() && free_ranges_[position].offset < extent.end())
            return status_t {status_code_t::invalid_argument_k, "A released extent is free already"};

        if (position != 0 && free_ranges_[position - 1].end() == extent.offset) {
            free_ranges_[position - 1].length += extent.length;
            if (position != free_ranges_.size() && free_ranges_[position - 1].end() == free_ranges_[position].offset) {
                free_ranges_[position - 1].length += free_ranges_[position].length;
                erase_at_(position);
            }
        }
        else if (position != free_ranges_.size() && extent.end() == free_ranges_[position].offset)
            free_ranges_[position] = extent_t {extent.offset, extent.length + free_ranges_[position].length};
        else {
            status_t const inserted = insert_at_(position, extent);
            if (inserted.failed()) return inserted;
        }
        free_bytes_ += extent.length;
        return {};
    }

    /** Bytes no allocation holds, which the alignment padding of a large allocation counts among. */
    [[nodiscard]] std::uint64_t free_bytes() const noexcept { return free_bytes_; }

    /** Bytes of the longest free range, which bounds the largest allocation that can still succeed. */
    [[nodiscard]] std::uint64_t largest_free_bytes() const noexcept {
        std::uint64_t longest = 0;
        for (std::size_t index = 0; index != free_ranges_.size(); ++index)
            if (free_ranges_[index].length > longest) longest = free_ranges_[index].length;
        return longest;
    }

    /** Bytes of the region the map hands out, free and allocated alike. */
    [[nodiscard]] std::uint64_t capacity() const noexcept { return region_.length; }

  private:

    /** Takes @p taken out of the free range at @p position, leaving its head and its tail behind. */
    status_t carve_(std::size_t position, extent_t taken) noexcept {
        extent_t const range = free_ranges_[position];
        extent_t const head {range.offset, taken.offset - range.offset};
        extent_t const tail {taken.end(), range.end() - taken.end()};
        if (!head.empty() && !tail.empty()) {
            status_t const inserted = insert_at_(position + 1, tail);
            if (inserted.failed()) return inserted;
            free_ranges_[position] = head;
        }
        else if (!head.empty()) free_ranges_[position] = head;
        else if (!tail.empty()) free_ranges_[position] = tail;
        else erase_at_(position);
        free_bytes_ -= taken.length;
        return {};
    }

    /** Puts @p range at @p position, shifting the later ranges up, since the vector has no insert. */
    status_t insert_at_(std::size_t position, extent_t range) noexcept {
        if (free_ranges_.push_back(extent_t {}) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "The free map would not grow"};
        for (std::size_t index = free_ranges_.size() - 1; index != position; --index)
            free_ranges_[index] = free_ranges_[index - 1];
        free_ranges_[position] = range;
        return {};
    }

    /** Drops the range at @p position, shifting the later ranges down, since the vector has no erase. */
    void erase_at_(std::size_t position) noexcept {
        for (std::size_t index = position + 1; index != free_ranges_.size(); ++index)
            free_ranges_[index - 1] = free_ranges_[index];
        free_ranges_.pop_back();
    }

    /** The free ranges, disjoint, non-adjacent and sorted by offset. */
    st::basic_vector<extent_t> free_ranges_;

    /** The region every range sits inside, which no allocation ever leaves. */
    extent_t region_;

    /** The multiple every allocated length takes. */
    std::uint64_t granularity_ = 1;

    /** The multiple a large allocation's offset takes. */
    std::uint64_t large_alignment_ = 1;

    /** Bytes the free ranges add up to, kept as they change rather than summed on demand. */
    std::uint64_t free_bytes_ = 0;
};

#pragma endregion Allocator

#pragma endregion Extent Allocator

#pragma region Catalog

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
    return static_cast<std::uint32_t>(round_up_to_multiple(sizeof(catalog_record_t) + name_bytes, record_alignment_k));
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
            if (record.content_length > record.extent_length)
                return status_t {status_code_t::malformed_k, "A catalog record holds more content than its extent"};

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

#pragma endregion Catalog

#pragma region Volume

#pragma region Blobs

/** One mounted volume, or @c unmounted_k for a handle that names none. */
enum class volume_id_t : std::uint16_t {
    unmounted_k = 0xFFFF,
};

/**
 *  @brief A named range of one volume, and all the read path needs after a lookup.
 *
 *  A blob hands out extents through @c slice and the caller submits them itself, so reading one
 *  block of a sorted run costs one addition and no lookup, no lock and no virtual call. That is what
 *  lets a volume be a template while the choice of transport stays a runtime one.
 */
struct volume_blob_t {

    /** The volume the bytes live on, which a router resolves to a disk exactly once. */
    volume_id_t volume = volume_id_t::unmounted_k;

    /** The medium the bytes live in, which is the whole volume or one file of a directory. */
    medium_reference_t medium;

    /** The range reserved for this blob, whose length is the rounded-up size the caller asked for. */
    extent_t extent;

    /** The bytes the caller asked for, which is at most @c extent.length and usually less. */
    std::uint64_t length = 0;

    /** The part of this blob @p offset bytes in, as the extent a request carries. */
    [[nodiscard]] constexpr extent_t slice(std::uint64_t offset, std::uint64_t bytes) const noexcept {
        extent_t const inside = extent_t {offset, bytes}.clipped_to(extent.length);
        return extent_t {extent.offset + inside.offset, inside.length};
    }
};

#pragma endregion Blobs

#pragma region Configuration

/** What a mount does when a volume holds no readable catalog. */
enum class volume_recovery_t : std::uint8_t {

    /** Reports @c malformed_k and leaves the medium untouched, which is what a database does. */
    refuse_k,

    /** Formats the volume empty, which only a test fixture and a repair tool ask for. */
    reformat_k,
};

/** What a format fixes about a volume before any blob is written into it. */
struct volume_format_t {

    /** The database this volume belongs to, which a mount refuses to open under another one. */
    uuid_t database {};

    /** Bytes the volume spans, which a format writes and a mount checks. */
    std::uint64_t capacity_bytes = 0;

    /** Bytes one catalog slot may grow to, of which the volume keeps two. */
    std::uint64_t catalog_slot_bytes = 4ull * 1024ull * 1024ull;

    /** Bytes the data region rounds every allocation up to, at least the disk's own alignment. */
    std::uint64_t allocation_granularity = portable_alignment_k;

    /** Bytes an allocation of at least eight megabytes rounds its first byte up to. */
    std::uint64_t large_allocation_alignment = 1024ull * 1024ull;

    /** What a mount does when neither catalog slot verifies. */
    volume_recovery_t recovery = volume_recovery_t::refuse_k;
};

#pragma endregion Configuration

#pragma region Extent Volume

/**
 *  @brief Names resolved to extents of one medium, with a catalog of two slots inside the volume.
 *
 *  This is the shape a medium without a filesystem takes. @c create reserves a range in memory
 *  alone, the caller writes the bytes through the ring, and @c seal performs the order the model
 *  checks, so a crash before @c seal leaves a range that is free by definition rather than a blob
 *  with a hole in it.
 *
 *  A reservation is deliberately invisible to @c find and to every publication until its @c seal,
 *  since a catalog that named a range whose bytes are still in flight would publish a hole the next
 *  unrelated @c seal made durable.
 */
template <typename disk_type_>
    requires ring_concept<disk_type_>
class basic_extent_volume {
  public:

    basic_extent_volume(basic_extent_volume const&) = delete;
    basic_extent_volume& operator=(basic_extent_volume const&) = delete;
    basic_extent_volume(basic_extent_volume&&) noexcept = default;

    /**
     *  @brief Mounts @p medium of @p disk, formatting it when it holds no volume and @p format allows.
     *  @param[inout] disk The transport, borrowed and outliving the volume.
     *  @param[inout] channel A channel to read the head and the slots through, borrowed for this call.
     *  @param[in] medium The registered medium the volume occupies.
     *  @param[in] format The identity and geometry a format writes, and what to do with a bad catalog.
     *  @return The volume, or @c status_code_t::malformed_k for a medium @p format refuses to repair.
     */
    static expected<basic_extent_volume> mount(disk_type_& disk, io_channel_t& channel,
                                               registered_medium_t const& medium,
                                               volume_format_t const& format) noexcept {
        basic_extent_volume volume {disk};
        volume.medium_ = medium.reference();
        volume.capacity_ = medium.capacity() != 0 ? medium.capacity() : format.capacity_bytes;

        auto [head, read_head_status] = volume.read_head_(channel);
        if (read_head_status.failed() && format.recovery == volume_recovery_t::refuse_k) return read_head_status;
        if (read_head_status.succeeded() && head.magic == volume_magic_k) {
            if (head.format_version != volume_format_version_k)
                return status_t {status_code_t::unsupported_k, "The volume follows another format version"};
            if (format.database.is_valid() && head.database != format.database)
                return status_t {status_code_t::invalid_argument_k, "The volume belongs to another database"};
            volume.header_ = head;
            if (status_t const loaded = volume.load_catalog_(channel, format.recovery); loaded.failed()) return loaded;
        }
        else if (status_t const written = volume.write_head_(channel, format); written.failed()) return written;

        if (status_t const rebuilt = volume.rebuild_free_map_(); rebuilt.failed()) return rebuilt;
        return volume;
    }

    /**
     *  @brief Reserves @p length bytes for @p name, without touching the medium.
     *  @param[in] name The bytes the blob is named by, at most @c blob_name_max_bytes_k of them.
     *  @param[in] kind What the blob is, which is how @c for_each enumerates it later.
     *  @param[in] length Bytes the caller will write, rounded up to the allocation granularity.
     *  @return The blob to write into, which @c find answers for only once @c seal has recorded it.
     */
    expected<volume_blob_t> create(blob_name_t name, blob_kind_t kind, std::uint64_t length) noexcept {
        if (name.empty() || name.size() > blob_name_max_bytes_k)
            return status_t {status_code_t::invalid_argument_k, "A blob name is one byte to the format's limit"};
        if (names(name)) return status_t {status_code_t::already_exists_k, "The volume names that blob"};

        auto [reserved, allocate_status] = allocator_.allocate(length);
        if (allocate_status.failed()) return allocate_status;
        catalog_entry_t entry;
        std::memcpy(entry.name_storage.data(), name.data(), name.size());
        entry.name_bytes = static_cast<std::uint16_t>(name.size());
        entry.kind = kind;
        entry.extent = reserved;
        entry.content_length = length;
        if (reserved_.push_back(std::move(entry)) != st::status_t::success_k) {
            [[maybe_unused]] status_t const returned = allocator_.release(reserved);
            return status_t {status_code_t::out_of_memory_k, "The volume could not track another reservation"};
        }
        return blob_of_(reserved, length);
    }

    /** Whether this volume already answers to @p name, by a sealed blob or by a live reservation. */
    [[nodiscard]] bool names(blob_name_t name) const noexcept {
        return catalog_.find(name) != nullptr || reservation_named_(name) != reserved_.size();
    }

    /** The blob @p name resolves to, or @c not_found_k, which costs one lookup and no I/O. */
    expected<volume_blob_t> find(blob_name_t name) const noexcept {
        catalog_entry_t const* const found = catalog_.find(name);
        if (!found) return status_t {status_code_t::not_found_k, "The volume holds no blob of that name"};
        return blob_of_(found->extent, found->content_length);
    }

    /**
     *  @brief Makes the bytes of @p blob durable and then records it, in that order and no other.
     *  @param[inout] channel The channel the caller wrote the bytes on, which is the only one that can flush them.
     *  @param[in] blob The reservation @c create handed out, whose bytes the caller has already completed.
     *  @return Success, or @c status_code_t::not_found_k when no live reservation covers that range.
     */
    status_t seal(io_channel_t& channel, volume_blob_t const& blob) noexcept {
        if (blob.medium.medium_id != medium_.medium_id || blob.medium.generation != medium_.generation)
            return {status_code_t::invalid_argument_k, "That blob was reserved on another volume"};
        std::size_t const position = reservation_at_(blob.extent);
        if (position == reserved_.size()) return {status_code_t::not_found_k, "The volume reserved no such range"};

        if (status_t const flushed = flush_bytes_(channel); flushed.failed()) return flushed;
        catalog_entry_t const& entry = reserved_[position];
        if (status_t const recorded = catalog_.insert(entry.name(), entry.kind, entry.extent, entry.content_length);
            recorded.failed())
            return recorded;
        if (status_t const published = publish_catalog_(channel); published.failed()) {
            [[maybe_unused]] expected<extent_t> const undone = catalog_.erase(entry.name());
            return published;
        }
        reserved_.erase(position);
        return {};
    }

    /** Forgets @p name durably, and only then returns its range to the free map. */
    status_t drop(io_channel_t& channel, blob_name_t name) noexcept {
        if (std::size_t const position = reservation_named_(name); position != reserved_.size()) {
            extent_t const abandoned = reserved_[position].extent;
            reserved_.erase(position);
            return allocator_.release(abandoned);
        }

        catalog_entry_t const* const found = catalog_.find(name);
        if (!found) return {status_code_t::not_found_k, "The volume holds no blob of that name"};
        catalog_entry_t const restored = *found;
        auto [forgotten, erase_status] = catalog_.erase(name);
        if (erase_status.failed()) return erase_status;
        if (status_t const published = publish_catalog_(channel); published.failed()) {
            [[maybe_unused]] status_t const undone = catalog_.insert(restored.name(), restored.kind, restored.extent,
                                                                     restored.content_length);
            return published;
        }
        return allocator_.release(forgotten);
    }

    /** Calls @p visitor with the name and the blob of every catalog entry of @p kind. */
    template <typename visitor_type_>
    void for_each(blob_kind_t kind, visitor_type_&& visitor) const noexcept {
        catalog_.for_each(kind, [&](catalog_entry_t const& entry) noexcept {
            visitor(entry.name(), blob_of_(entry.extent, entry.content_length));
        });
    }

    /** Bytes the data region hands out, which is the capacity minus the head and both slots. */
    [[nodiscard]] std::uint64_t capacity() const noexcept { return allocator_.capacity(); }

    /** Bytes no entry and no reservation covers, which a router's placement decision reads. */
    [[nodiscard]] std::uint64_t free_bytes() const noexcept { return allocator_.free_bytes(); }

    /** The largest single blob this volume could still allocate, which fragmentation lowers. */
    [[nodiscard]] std::uint64_t largest_free_bytes() const noexcept { return allocator_.largest_free_bytes(); }

    /** Blobs the catalog names right now, of which a reservation is not yet one. */
    [[nodiscard]] std::size_t size() const noexcept { return catalog_.size(); }

    /** Ranges @c create handed out that no @c seal has recorded, which a remount forgets. */
    [[nodiscard]] std::size_t reservations() const noexcept { return reserved_.size(); }

    /** The transport underneath, which every read and write of a blob's bytes goes to directly. */
    [[nodiscard]] disk_type_& disk() const noexcept { return disk_; }

    /** The medium every blob of this volume lives in. */
    [[nodiscard]] medium_reference_t medium() const noexcept { return medium_; }

    /** The head of the volume, which names the geometry a repair tool reads the slots by. */
    [[nodiscard]] volume_header_t const& header() const noexcept { return header_; }

  private:

    /** The blob a catalog entry describes, which carries the medium so a caller needs nothing else. */
    [[nodiscard]] volume_blob_t blob_of_(extent_t extent, std::uint64_t length) const noexcept {
        volume_blob_t blob;
        blob.volume = volume_id_t::unmounted_k;
        blob.medium = medium_;
        blob.extent = extent;
        blob.length = length;
        return blob;
    }

    /** Where the reservation of @p name sits, or @c reserved_.size() when there is none. */
    [[nodiscard]] std::size_t reservation_named_(blob_name_t name) const noexcept {
        for (std::size_t index = 0; index != reserved_.size(); ++index) {
            catalog_entry_t const& entry = reserved_[index];
            if (entry.name_bytes != name.size()) continue;
            if (std::memcmp(entry.name_storage.data(), name.data(), name.size()) == 0) return index;
        }
        return reserved_.size();
    }

    /** Where the reservation covering @p extent sits, or @c reserved_.size() when there is none. */
    [[nodiscard]] std::size_t reservation_at_(extent_t extent) const noexcept {
        for (std::size_t index = 0; index != reserved_.size(); ++index)
            if (reserved_[index].extent == extent) return index;
        return reserved_.size();
    }

    /** Reads the head of the volume, which a fresh medium answers with zeroes rather than a failure. */
    expected<volume_header_t> read_head_(io_channel_t& channel) noexcept {
        if (channel.requests().buffer_bytes() < portable_alignment_k)
            return status_t {status_code_t::capacity_exhausted_k, "A channel's block is shorter than a volume's head"};

        basic_read_pipeline<disk_type_> reader {disk_, channel, medium_};
        if (status_t const offered = reader.enqueue_read(extent_t {0, portable_alignment_k}, 0); offered.failed())
            return offered;
        if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;

        volume_header_t header {};
        status_t sink_status;
        auto sink = [&](completed_buffer_t const& completed) noexcept {
            if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                sink_status = whole;
                return;
            }
            std::memcpy(&header, completed.bytes.data(), sizeof(header));
        };
        if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
        if (sink_status.failed()) return sink_status;
        if (header.magic != 0 && header.magic != volume_magic_k)
            return status_t {status_code_t::malformed_k, "The volume carries another magic"};
        if (header.magic == volume_magic_k && !header_checksum_holds(header))
            return status_t {status_code_t::malformed_k, "The volume header does not match its checksum"};
        return header;
    }

    /** Writes the head and two empty catalog slots, which is what a format is. */
    status_t write_head_(io_channel_t& channel, volume_format_t const& format) noexcept {
        volume_header_t header {};
        header.magic = volume_magic_k;
        header.format_version = volume_format_version_k;
        header.header_bytes = sizeof(volume_header_t);
        header.database = format.database;
        header.capacity_bytes = capacity_;
        header.catalog_slot_offset = portable_alignment_k;
        header.catalog_slot_bytes = round_up_to_multiple(format.catalog_slot_bytes, portable_alignment_k);
        header.data_offset = round_up_to_multiple(header.catalog_slot_offset + 2 * header.catalog_slot_bytes,
                                                  format.large_allocation_alignment);
        header.allocation_granularity = format.allocation_granularity;
        header.large_allocation_alignment = format.large_allocation_alignment;
        header.catalog_generation = 0;
        set_header_checksum(header);
        if (header.data_offset >= capacity_)
            return {status_code_t::capacity_exhausted_k, "The volume is too small for its own catalog"};

        header_ = header;
        catalog_ = catalog_t {};
        basic_write_pipeline<disk_type_> writer {disk_, channel, medium_};
        auto [block, acquire_buffer_status] = writer.acquire_buffer();
        if (acquire_buffer_status.failed()) return acquire_buffer_status;
        if (block.size() < portable_alignment_k)
            return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than a volume's head"};
        std::span<byte_t> const bytes = block.bytes();
        std::memset(bytes.data(), 0, static_cast<std::size_t>(portable_alignment_k));
        std::memcpy(bytes.data(), &header, sizeof(header));
        if (status_t const queued = writer.enqueue_write(std::move(block), extent_t {0, portable_alignment_k});
            queued.failed())
            return queued;
        if (status_t const durable = writer.make_durable(); durable.failed()) return durable;
        return publish_both_slots_(channel);
    }

    /** Reads both slots and takes the newer one that verifies, which is the whole of recovery. */
    status_t load_catalog_(io_channel_t& channel, volume_recovery_t recovery) noexcept {
        catalog_t newest;
        std::uint64_t newest_generation = 0;
        bool found = false;
        for (std::uint32_t slot = 0; slot != 2; ++slot) {
            catalog_t parsed;
            expected<std::uint64_t> const generation = read_slot_(channel, slot, parsed);
            if (!generation) continue;
            if (found && *generation <= newest_generation) continue;
            newest = std::move(parsed);
            newest_generation = *generation;
            found = true;
        }
        if (found) {
            catalog_ = std::move(newest);
            header_.catalog_generation = newest_generation;
            return {};
        }
        if (recovery == volume_recovery_t::refuse_k)
            return {status_code_t::malformed_k, "Neither catalog slot of the volume verifies"};

        catalog_ = catalog_t {};
        header_.catalog_generation = 0;
        return publish_both_slots_(channel);
    }

    /**
     *  @brief Reads one slot into @p into, answering its generation when it verifies.
     *
     *  @note The image is read through a streaming block with one request of its own, since a
     *      pipeline fills the request pool whose blocks are far shorter than a catalog slot.
     */
    expected<std::uint64_t> read_slot_(io_channel_t& channel, std::uint32_t slot, catalog_t& into) noexcept {
        auto [block, acquire_status] = channel.streams().acquire();
        if (acquire_status.failed()) return acquire_status;
        std::uint64_t const wanted = slot_image_bytes_(block.size());
        if (wanted == 0)
            return status_t {status_code_t::capacity_exhausted_k, "A channel's streaming block holds no catalog page"};

        std::array<request_t, 1> const batch {
            read_request(medium_, extent_t {slot_offset_(slot), wanted}, block.reference(), channel.token(slot))};
        auto [taken, submit_status] = disk_.submit(channel.queue_pair(), std::span<request_t const> {batch});
        if (submit_status.failed()) return submit_status;
        if (taken != 1) return status_t {status_code_t::io_k, "The ring would not take a catalog read"};
        auto [completed, poll_completions_status] = poll_completions_at_least(disk_, channel.queue_pair(),
                                                                              channel.completion_buffer(), 1);
        if (poll_completions_status.failed()) return poll_completions_status;
        if (completed == 0) return status_t {status_code_t::io_k, "A catalog read never completed"};

        completion_t const& completion = channel.completion_buffer().front();
        if (!completion.completed_in_full())
            return completion.status.failed()
                       ? completion.status
                       : status_t {status_code_t::malformed_k, "The volume is shorter than its catalog slot"};

        std::span<byte_t const> const image {block.bytes().data(),
                                             static_cast<std::size_t>(completion.transferred_bytes)};
        if (status_t const parsed = into.load(image); parsed.failed()) return parsed;
        catalog_slot_header_t header {};
        std::memcpy(&header, image.data(), sizeof(header));
        return header.generation;
    }

    /** Writes the whole catalog image into the older slot and flushes it, which publishes it. */
    status_t publish_catalog_(io_channel_t& channel) noexcept {
        std::uint64_t const generation = header_.catalog_generation + 1;
        std::uint32_t const slot = static_cast<std::uint32_t>(generation & 1);

        auto [block, acquire_status] = channel.streams().acquire();
        if (acquire_status.failed()) return acquire_status;
        std::uint64_t const room = slot_image_bytes_(block.size());
        std::uint64_t const needed = catalog_.image_bytes();
        if (needed > room) return {status_code_t::capacity_exhausted_k, "The catalog outgrew the slot that holds it"};

        std::uint64_t const written = round_up_to_multiple(needed, portable_alignment_k);
        std::span<byte_t> const bytes = block.bytes();
        std::memset(bytes.data(), 0, static_cast<std::size_t>(written));
        auto [filled, save_status] = catalog_.save(bytes.first(static_cast<std::size_t>(room)), generation);
        if (save_status.failed()) return save_status;

        basic_write_pipeline<disk_type_> writer {disk_, channel, medium_};
        if (status_t const queued = writer.enqueue_write(std::move(block), extent_t {slot_offset_(slot), written});
            queued.failed())
            return queued;
        if (status_t const durable = writer.make_durable(); durable.failed()) return durable;
        header_.catalog_generation = generation;
        return {};
    }

    /** Publishes into both slots, so no image of an earlier life outlives a format or a repair. */
    status_t publish_both_slots_(io_channel_t& channel) noexcept {
        if (status_t const older = publish_catalog_(channel); older.failed()) return older;
        return publish_catalog_(channel);
    }

    /** Makes every write the channel has already completed durable, which is the step before recording. */
    status_t flush_bytes_(io_channel_t& channel) noexcept {
        basic_write_pipeline<disk_type_> writer {disk_, channel, medium_};
        return writer.make_durable();
    }

    /** Rebuilds the free map by subtracting every catalog entry from the data region. */
    status_t rebuild_free_map_() noexcept {
        if (header_.data_offset > capacity_)
            return {status_code_t::malformed_k, "The volume is shorter than its own catalog"};
        std::uint64_t const granularity = header_.allocation_granularity;
        if (!std::has_single_bit(granularity))
            return {status_code_t::malformed_k, "The volume names no power-of-two granularity"};

        extent_t const region {header_.data_offset,
                               round_down_to_multiple(capacity_ - header_.data_offset, granularity)};
        auto [allocator, make_extent_allocator_status] = extent_allocator_t::make(region, granularity,
                                                                                  header_.large_allocation_alignment);
        if (make_extent_allocator_status.failed()) return make_extent_allocator_status;
        allocator_ = std::move(allocator);

        status_t first;
        for (blob_kind_t const kind :
             {blob_kind_t::sorted_run_k, blob_kind_t::schema_k, blob_kind_t::backup_schema_k, blob_kind_t::user_data_k})
            catalog_.for_each(kind, [&](catalog_entry_t const& entry) noexcept {
                if (status_t const reserved = allocator_.reserve(entry.extent); reserved.failed() && first.succeeded())
                    first = reserved;
            });
        return first;
    }

    /** Bytes of a catalog slot a block of @p block_bytes can carry, which bounds the catalog both ways. */
    [[nodiscard]] std::uint64_t slot_image_bytes_(std::uint64_t block_bytes) const noexcept {
        std::uint64_t const bounded = header_.catalog_slot_bytes < block_bytes ? header_.catalog_slot_bytes
                                                                               : block_bytes;
        return round_down_to_multiple(bounded, portable_alignment_k);
    }

    /** Where slot @p slot begins, which alternates so a torn image never overwrites the live one. */
    [[nodiscard]] std::uint64_t slot_offset_(std::uint32_t slot) const noexcept {
        return header_.catalog_slot_offset + slot * header_.catalog_slot_bytes;
    }

    explicit basic_extent_volume(disk_type_& disk) noexcept : disk_(disk) {}

    /** The transport, which outlives the volume. */
    disk_type_& disk_;

    /** The medium the whole volume occupies. */
    medium_reference_t medium_;

    /** Bytes the volume spans, as the transport reported them. */
    std::uint64_t capacity_ = 0;

    /** The head of the volume: identity, geometry and the generation of the live slot. */
    volume_header_t header_ {};

    /** Every live entry, rebuilt at mount and rewritten whole at every publication. */
    catalog_t catalog_;

    /** Ranges @c create handed out that no @c seal has recorded, which no publication names. */
    st::basic_vector<catalog_entry_t> reserved_;

    /** Ranges no entry covers, derived from @c catalog_ and never itself stored on the medium. */
    extent_allocator_t allocator_;
};

#pragma endregion Extent Volume

#pragma endregion Volume

#pragma region Volume Router

#pragma region Contract

/** What a router asks of a volume: the names it resolves, and the free space it reports. */
template <typename volume_type_>
concept extent_volume_concept = requires(volume_type_& volume, volume_type_ const& constant, io_channel_t& channel,
                                         blob_name_t name, blob_kind_t kind, std::uint64_t length) {
    { volume.create(name, kind, length) } -> std::same_as<expected<volume_blob_t>>;
    { constant.find(name) } -> std::same_as<expected<volume_blob_t>>;
    { volume.drop(channel, name) } -> std::same_as<status_t>;
    { constant.names(name) } -> std::same_as<bool>;
    { constant.free_bytes() } -> std::same_as<std::uint64_t>;
    { constant.largest_free_bytes() } -> std::same_as<std::uint64_t>;
};

#pragma endregion Contract

#pragma region Router

/**
 *  @brief Every mounted volume of one database, and the free-space placement that spreads blobs over them.
 *
 *  The template is on the volume rather than on a variant of volumes, because a mount already knows
 *  which transport it opened and a virtual call per placement would buy nothing; a runtime-dispatched
 *  router wraps this one rather than replacing it.
 */
template <typename volume_type_>
    requires extent_volume_concept<volume_type_>
class basic_volume_router {
  public:

    basic_volume_router() noexcept = default;
    basic_volume_router(basic_volume_router const&) = delete;
    basic_volume_router& operator=(basic_volume_router const&) = delete;
    basic_volume_router(basic_volume_router&&) noexcept = default;
    basic_volume_router& operator=(basic_volume_router&&) noexcept = default;

    /**
     *  @brief An empty router with room for @p volumes_limit volumes, which a mount fills one at a time.
     *  @param[in] volumes_limit Volumes this router will ever hold, which is one per data directory.
     *
     *  @note The room is taken here and never grown, because an LSM tree holds a pointer to the volume
     *      it was mounted on; a later adoption that moved the volumes would leave every tree
     *      addressing freed memory.
     */
    static expected<basic_volume_router> make(std::size_t volumes_limit = 1) noexcept {
        if (volumes_limit == 0 || volumes_limit >= static_cast<std::size_t>(volume_id_t::unmounted_k))
            return status_t {status_code_t::invalid_argument_k, "A router names one volume to an identifier's worth"};
        basic_volume_router router;
        if (router.volumes_.reserve(volumes_limit) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "The router could not reserve its volumes"};
        router.limit_ = volumes_limit;
        return router;
    }

    /**
     *  @brief Takes @p volume in under the next identifier, which every blob placed on it carries.
     *  @param[in] volume The mounted volume, moved in and owned by the router from here on.
     */
    status_t bind_queue_pair(volume_type_&& volume) noexcept {
        // Refused rather than grown, since growing would move volumes their trees still point at.
        if (volumes_.size() >= limit_)
            return {status_code_t::capacity_exhausted_k, "The router reserved room for fewer volumes than that"};
        if (volumes_.push_back(std::move(volume)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "The router could not track another volume"};
        return {};
    }

    /**
     *  @brief The volume with the most free space that a blob of @p length still fits in.
     *  @param[in] length Bytes the blob asks for, before the volume rounds it to its granularity.
     *  @return The volume, or @c status_code_t::capacity_exhausted_k when none has a range that long.
     */
    expected<volume_id_t> optimal_volume(std::uint64_t length) const noexcept {
        std::size_t chosen = volumes_.size();
        std::uint64_t chosen_free = 0;
        for (std::size_t index = 0; index != volumes_.size(); ++index) {
            volume_type_ const& candidate = volumes_[index];
            if (candidate.largest_free_bytes() < length) continue;
            std::uint64_t const free = candidate.free_bytes();
            if (chosen != volumes_.size() && free <= chosen_free) continue;
            chosen = index;
            chosen_free = free;
        }
        if (chosen == volumes_.size())
            return status_t {status_code_t::capacity_exhausted_k, "No mounted volume fits a blob that long"};
        return static_cast<volume_id_t>(chosen);
    }

    /**
     *  @brief Places @p name by free space and reserves @p length bytes for it on the volume that won.
     *  @param[in] name The bytes the blob is named by, which no other volume of the router may hold.
     *  @param[in] kind What the blob is, which is how @c for_each enumerates it later.
     *  @param[in] length Bytes the caller will write, rounded up by the volume that takes it.
     *  @return The blob, stamped with the volume it was placed on, which the caller seals there.
     */
    expected<volume_blob_t> create(blob_name_t name, blob_kind_t kind, std::uint64_t length) noexcept {
        for (std::size_t index = 0; index != volumes_.size(); ++index)
            if (volumes_[index].names(name))
                return status_t {status_code_t::already_exists_k, "A volume of this router names that blob"};

        auto [placed, optimal_volume_status] = optimal_volume(length);
        if (optimal_volume_status.failed()) return optimal_volume_status;
        auto [created, create_status] = volumes_[static_cast<std::size_t>(placed)].create(name, kind, length);
        if (create_status.failed()) return create_status;
        created.volume = placed;
        return created;
    }

    /** The blob @p name resolves to on whichever volume holds it, stamped with that volume. */
    expected<volume_blob_t> find(blob_name_t name) const noexcept {
        for (std::size_t index = 0; index != volumes_.size(); ++index) {
            expected<volume_blob_t> found = volumes_[index].find(name);
            if (!found) continue;
            found->volume = static_cast<volume_id_t>(index);
            return found;
        }
        return status_t {status_code_t::not_found_k, "No volume of this router holds that blob"};
    }

    /** Forgets @p name on the one volume that holds it, which is the only one the drop reaches. */
    status_t drop(io_channel_t& channel, blob_name_t name) noexcept {
        for (std::size_t index = 0; index != volumes_.size(); ++index)
            if (volumes_[index].names(name)) return volumes_[index].drop(channel, name);
        return {status_code_t::not_found_k, "No volume of this router holds that blob"};
    }

    /** Calls @p visitor with the name and the stamped blob of every entry of @p kind, volume by volume. */
    template <typename visitor_type_>
    void for_each(blob_kind_t kind, visitor_type_&& visitor) const noexcept {
        for (std::size_t index = 0; index != volumes_.size(); ++index)
            volumes_[index].for_each(kind, [&](blob_name_t name, volume_blob_t blob) noexcept {
                blob.volume = static_cast<volume_id_t>(index);
                visitor(name, blob);
            });
    }

    /** The volume @p id names, which a caller reaches to seal the blob it has just written. */
    [[nodiscard]] volume_type_& volume(volume_id_t id) noexcept {
        assert(static_cast<std::size_t>(id) < volumes_.size() && "A router resolves only the volumes it adopted");
        return volumes_[static_cast<std::size_t>(id)];
    }

    /** Volumes the router has adopted, whose identifiers run from zero to one below it. */
    [[nodiscard]] std::size_t size() const noexcept { return volumes_.size(); }

  private:

    /** Volumes this router reserved room for, fixed when it was made and never grown. */
    std::size_t limit_ = 0;

    /** Every adopted volume, in the order they were adopted, which is the order of their identifiers. */
    st::basic_vector<volume_type_> volumes_;
};

#pragma endregion Router

#pragma endregion Volume Router

#pragma region Mount Configuration

/** A directory data files are placed in, with the bytes they may take there. */
struct data_directory_config_t {
    string_t path;
    std::size_t max_size = unlimited_resource_k;
};

/**
 *  @brief The transport a mount opens its data directories through, probed rather than assumed.
 *
 *  The choice is made once, when the database comes up, and the transport a mount actually got is
 *  reported back rather than inferred, since @c automatic_k resolves differently on a kernel without
 *  io_uring, inside a seccomp profile that refuses it, and on a machine that has both.
 */
enum class io_transport_t : std::uint8_t {

    /** io_uring where the kernel offers it, Linux AIO where it offers only that, and the blocking floor elsewhere. */
    automatic_k,

    /** Positioned reads and writes on the polling thread, which every platform has. */
    blocking_k,

    /** io_uring on unbuffered files, refused without a kernel ring; an arena the memlock cap refuses goes unpinned. */
    linux_uring_k,

    /** Linux AIO on unbuffered files, which Docker's seccomp profile and kernels below 6.12 keep without a ring. */
    linux_aio_k,

    /** SPDK's user-space driver, which no mount opens yet. */
    linux_spdk_k,

    /** Host memory, which a suite and a benchmark ask for and a database never does. */
    memory_k,
};

/**
 *  @brief Budgets and directories every collection of one database shares, as the NVMe init call names them.
 *
 *  Zero @c threads leaves the count to @c derived_background_threads when the database opens.
 */
struct resources_config_t {
    std::size_t memory_limit = 4 * 1024 * 1024 * 1024ull;
    std::size_t threads = 0;
    std::size_t gpu_memory_limit = 0;
    st::basic_vector<int> gpu_devices;
    string_t directory;
    io_transport_t io_transport = io_transport_t::automatic_k;
    io_channel_config_t channels;
    std::size_t shared_io_channels = 2;
    st::basic_vector<data_directory_config_t> data_directories;
};

/** Half the logical cores, between one and eight, as every thread pins a channel of 12 MB by default. */
[[nodiscard]] inline std::size_t derived_background_threads(std::size_t logical_cores_count) noexcept {
    return std::clamp<std::size_t>(logical_cores_count / 2, 1, 8);
}
#pragma endregion Mount Configuration

#pragma region Files Disk

#pragma region Disk

/**
 *  @brief The ring of a database on files: the blocking floor, AIO or io_uring, as the mount probed.
 *
 *  The io_uring and AIO alternatives exist on every platform, as stubs that refuse off Linux, so
 *  the type is portable and only the mount's probe decides which alternative is built.
 */
class any_files_disk_t {
  public:

    /** Takes the blocking transport in, which a mount builds where the probe refused a ring. */
    explicit any_files_disk_t(blocking_files_disk_t&& disk) noexcept
        : held_(std::in_place_type<blocking_files_disk_t>, std::move(disk)) {}

    /** Takes the io_uring transport in, which a mount builds where the probe found a ring. */
    explicit any_files_disk_t(linux_uring_files_disk_t&& disk) noexcept
        : held_(std::in_place_type<linux_uring_files_disk_t>, std::move(disk)) {}

    /** Takes the Linux AIO transport in, which a mount builds where the probe refused a ring and found AIO. */
    explicit any_files_disk_t(linux_aio_files_disk_t&& disk) noexcept
        : held_(std::in_place_type<linux_aio_files_disk_t>, std::move(disk)) {}

    any_files_disk_t(any_files_disk_t const&) = delete;
    any_files_disk_t& operator=(any_files_disk_t const&) = delete;
    any_files_disk_t(any_files_disk_t&&) noexcept = default;
    any_files_disk_t& operator=(any_files_disk_t&&) noexcept = default;

    /** The transport held, which is never @c automatic_k. */
    [[nodiscard]] io_transport_t transport() const noexcept {
        if (std::holds_alternative<linux_uring_files_disk_t>(held_)) return io_transport_t::linux_uring_k;
        if (std::holds_alternative<linux_aio_files_disk_t>(held_)) return io_transport_t::linux_aio_k;
        return io_transport_t::blocking_k;
    }

    /** What the held transport accepts. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept {
        return std::visit([](auto const& disk) noexcept { return disk.capabilities(); }, held_);
    }

    /** How the held transport addresses the registered arena; the blocking floor and AIO pin per request. */
    [[nodiscard]] range_pinning_t pinning() const noexcept {
        linux_uring_files_disk_t const* const linux_uring = std::get_if<linux_uring_files_disk_t>(&held_);
        return linux_uring ? linux_uring->pinning() : range_pinning_t::unpinned_k;
    }

#pragma region Queue Pairs

    /** Takes a queue pair on @p depth requests of the held ring for the calling thread. */
    expected<queue_pair_t> acquire_queue_pair(queue_depth_t depth) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.acquire_queue_pair(depth); }, held_);
    }

    /** Binds @p queue pair to the calling thread, which is how a moved queue pair changes hands. */
    status_t bind_queue_pair(queue_pair_t const& queue_pair) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.bind_queue_pair(queue_pair); }, held_);
    }

    /** Takes @p queue pair back once it has nothing outstanding, and empties the handle. */
    status_t release_queue_pair(queue_pair_t& queue_pair) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.release_queue_pair(queue_pair); }, held_);
    }

    /** Requests @p queue pair has submitted and not yet completed. */
    [[nodiscard]] std::size_t outstanding(queue_pair_t const& queue_pair) const noexcept {
        return std::visit([&](auto const& disk) noexcept { return disk.outstanding(queue_pair); }, held_);
    }

#pragma endregion Queue Pairs

#pragma region Registrations

    /** Pins @p memory so requests can address it, refusing @c gpu_k on both file transports. */
    expected<registered_memory_t> register_memory(std::span<byte_t> memory, memory_space_t space) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.register_memory(memory, space); }, held_);
    }

    /** Unpins @p registered memory, refusing while requests still address it. */
    status_t unregister_memory(registered_memory_t& registered_memory) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.unregister_memory(registered_memory); }, held_);
    }

    /** Opens the file @p request names as a medium requests address by slot. */
    expected<registered_medium_t> register_medium(medium_options_t const& request) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.register_medium(request); }, held_);
    }

    /** Closes @p registered medium's file, refusing while requests still name it. */
    status_t unregister_medium(registered_medium_t& registered_medium) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.unregister_medium(registered_medium); }, held_);
    }

#pragma endregion Registrations

#pragma region Ring

    /** Takes as much of @p requests as @p queue pair still has credit for, and never waits. */
    expected<std::size_t> submit(queue_pair_t const& queue_pair, std::span<request_t const> requests) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.submit(queue_pair, requests); }, held_);
    }

    /** Moves @p queue pair's ready completions into @p into, and never waits. */
    expected<std::size_t> poll_completions(queue_pair_t const& queue_pair, std::span<completion_t> into) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.poll_completions(queue_pair, into); }, held_);
    }

    /** Stops @p queue pair's requests that have not started. */
    void cancel(queue_pair_t const& queue_pair) noexcept {
        std::visit([&](auto& disk) noexcept { disk.cancel(queue_pair); }, held_);
    }

    /** Reaps and discards everything @p queue pair has in flight, so the queue pair can be released or handed over. */
    status_t drain(queue_pair_t const& queue_pair) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.drain(queue_pair); }, held_);
    }

#pragma endregion Ring

  private:

    /** The transport the probe chose, fixed for the life of the mount. */
    std::variant<blocking_files_disk_t, linux_uring_files_disk_t, linux_aio_files_disk_t> held_;
};

static_assert(ring_concept<any_files_disk_t>);

#pragma endregion Disk

#pragma endregion Files Disk

#pragma region Filesystem

#pragma region Paths

/** Bytes a path of this engine may span, which bounds the one buffer a mount joins paths in. */
inline constexpr std::size_t path_max_bytes_k = 4096;

/** The one file a data directory keeps its whole volume in. */
inline constexpr char volume_file_name_k[] = "volume.ustore";

#if defined(_WIN32)

/** Creates @p path alone, treating a directory that is already there as success. */
inline status_t create_directory(char const* path) noexcept {
    if (CreateDirectoryA(path, nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS) return {};
    return {status_code_t::io_k, "A directory of the database would not be created"};
}

/** Removes the file @p path, treating one that is already gone as success. */
inline status_t remove_file(char const* path) noexcept {
    if (DeleteFileA(path) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND) return {};
    return {status_code_t::io_k, "A file of the database would not be removed"};
}

/** Removes the empty directory @p path, treating one that is already gone as success. */
inline status_t remove_directory(char const* path) noexcept {
    if (RemoveDirectoryA(path) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND) return {};
    return {status_code_t::io_k, "A directory of the database would not be removed"};
}

#else

/** Creates @p path alone, treating a directory that is already there as success. */
inline status_t create_directory(char const* path) noexcept {
    if (::mkdir(path, 0755) == 0 || errno == EEXIST) return {};
    return {status_code_t::io_k, "A directory of the database would not be created"};
}

/** Removes the file @p path, treating one that is already gone as success. */
inline status_t remove_file(char const* path) noexcept {
    if (::unlink(path) == 0 || errno == ENOENT) return {};
    return {status_code_t::io_k, "A file of the database would not be removed"};
}

/** Removes the empty directory @p path, treating one that is already gone as success. */
inline status_t remove_directory(char const* path) noexcept {
    if (::rmdir(path) == 0 || errno == ENOENT) return {};
    return {status_code_t::io_k, "A directory of the database would not be removed"};
}

#endif

/**
 *  @brief Creates @p path and every parent it needs, which a mount does before it opens a volume there.
 *
 *  @note Only the last component has to be created; an intermediate that refuses, such as a Windows
 *      drive letter, is passed over rather than reported.
 */
inline status_t create_directories(char const* path) noexcept {
    char components[path_max_bytes_k];
    std::size_t const length = std::strlen(path);
    if (length == 0 || length >= sizeof(components))
        return {status_code_t::invalid_argument_k, "A directory path is one byte to the platform's limit"};
    std::memcpy(components, path, length + 1);

    for (std::size_t index = 1; index != length; ++index) {
        if (components[index] != '/' && components[index] != '\\') continue;
        components[index] = '\0';
        [[maybe_unused]] status_t const parent = create_directory(components);
        components[index] = path[index];
    }
    return create_directory(components);
}

/** Writes @c <directory>/<name> into @p into, or refuses a path the buffer will not hold. */
inline status_t join_path(char const* directory, char const* name, std::span<char> into) noexcept {
    std::size_t const directory_bytes = std::strlen(directory);
    std::size_t const name_bytes = std::strlen(name);
    if (directory_bytes + name_bytes + 2 > into.size())
        return {status_code_t::invalid_argument_k, "A path of the database outgrew the platform's limit"};

    std::memcpy(into.data(), directory, directory_bytes);
    std::size_t written = directory_bytes;
    if (written != 0 && into[written - 1] != '/' && into[written - 1] != '\\') into[written++] = '/';
    std::memcpy(into.data() + written, name, name_bytes + 1);
    return {};
}

/** Creates every data directory of @p config, which every file transport opens a volume in. */
inline status_t create_data_directories(resources_config_t const& config) noexcept {
    for (std::size_t index = 0; index != config.data_directories.size(); ++index) {
        data_directory_config_t const& directory = config.data_directories[index];
        if (directory.path.empty()) return {status_code_t::invalid_argument_k, "A data directory needs a path"};
        if (status_t const made = create_directories(directory.path.c_str()); made.failed()) return made;
    }
    return {};
}

#pragma endregion Paths

#pragma region Probe

/** What a mount opened, which is what the effective configuration of a database reports. */
struct mount_report_t {

    /** The transport the mount actually opened, which is never @c automatic_k. */
    io_transport_t transport = io_transport_t::blocking_k;

    /** What the kernel answered when the ring was set up and taken down again. */
    linux_uring_probe_t linux_uring = linux_uring_probe_t::refused_k;

    /** What the kernel answered when an AIO context was set up and taken down again. */
    linux_aio_probe_t linux_aio = linux_aio_probe_t::refused_k;

    /** Volumes the router adopted, which is one per data directory. */
    std::size_t volumes = 0;

    /** Channels the pool opened, which is the executor's threads plus the shared channels. */
    std::size_t channels = 0;

    /** Bytes the arena pinned, charged to the host budget exactly once. */
    std::uint64_t arena_bytes = 0;
};

/**
 *  @brief The transport @p wanted resolves to, given what the two probes answered.
 *
 *  The order is io_uring, then Linux AIO, then the blocking floor. A transport named outright starts
 *  at its own place in that order and only falls further down it, so asking for AIO never mounts a ring.
 */
[[nodiscard]] inline io_transport_t probed_transport(io_transport_t wanted, linux_uring_probe_t linux_uring,
                                                     linux_aio_probe_t linux_aio) noexcept {
    bool const takes_linux_uring = wanted == io_transport_t::automatic_k || wanted == io_transport_t::linux_uring_k;
    if (takes_linux_uring && linux_uring == linux_uring_probe_t::available_k) return io_transport_t::linux_uring_k;
    if (!takes_linux_uring && wanted != io_transport_t::linux_aio_k) return wanted;
    return linux_aio == linux_aio_probe_t::available_k ? io_transport_t::linux_aio_k : io_transport_t::blocking_k;
}

#pragma endregion Probe

#pragma region Mount

/**
 *  @brief One transport, the memory it pinned, the channels it queue pairs and the volumes it opened over it.
 *
 *  It is deliberately neither copyable nor movable: a channel pool and every volume hold a pointer to
 *  the disk beside them, so a mount that moved would leave both addressing a transport that is no
 *  longer there. The variant above it emplaces one rather than assigning one for that reason.
 */
template <typename disk_type_>
    requires ring_concept<disk_type_>
class basic_storage_mount {
  public:

    /** The volume every data directory of this mount holds, which is one file of the directory. */
    using volume_t = basic_extent_volume<disk_type_>;

    /** The volumes of this mount and the free-space placement that spreads blobs over them. */
    using router_t = basic_volume_router<volume_t>;

    /** Every queue pair this mount hands out, one per executor thread plus the shared channels. */
    using channel_pool_t = basic_io_channel_pool<disk_type_>;

    /** Takes @p disk in, which a mount owns until it is torn down. */
    explicit basic_storage_mount(disk_type_&& disk) noexcept : disk_(std::move(disk)) {}

    basic_storage_mount(basic_storage_mount const&) = delete;
    basic_storage_mount& operator=(basic_storage_mount const&) = delete;
    basic_storage_mount(basic_storage_mount&&) = delete;
    basic_storage_mount& operator=(basic_storage_mount&&) = delete;

    ~basic_storage_mount() noexcept { [[maybe_unused]] status_t const released = unmount(); }

    /**
     *  @brief Pins the arena, queue pairs the channels and opens one volume per data directory of @p config.
     *  @param[in] config The channel shape, the shared channels and the data directories, with their capacities.
     *  @param[inout] constraint The host budget the arena is charged to, once, for the life of the mount.
     *  @param[in] threads Executor threads, each of which gets a channel of its own.
     *  @param[in] database The database the volumes belong to, which a mount refuses to open under another.
     *  @return Success, or the first refusal, after which @c unmount gives back whatever was taken.
     */
    status_t mount(resources_config_t const& config, resource_constraint_t& constraint, std::size_t threads,
                   uuid_t const& database) noexcept {
        if (config.data_directories.empty())
            return {status_code_t::invalid_argument_k, "A mount needs a data directory to open a volume in"};
        if (config.shared_io_channels == 0)
            return {status_code_t::invalid_argument_k, "A mount keeps a channel for the threads outside its executor"};

        memory_arena_config_t arena;
        arena.bytes = config.channels.arena_bytes() * (threads + config.shared_io_channels);
        auto [pinned, make_arena_status] = registered_memory_arena_t::make(disk_, constraint, arena);
        if (make_arena_status.failed()) return make_arena_status;
        arena_ = std::move(pinned);

        auto [leased, make_channel_pool_status] = channel_pool_t::make(disk_, arena_, threads,
                                                                       config.shared_io_channels, config.channels);
        if (make_channel_pool_status.failed()) return make_channel_pool_status;
        channels_ = std::move(leased);

        if (media_.resize(config.data_directories.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A mount could not track its targets"};
        auto [built, make_router_status] = router_t::make(config.data_directories.size());
        if (make_router_status.failed()) return make_router_status;
        router_ = std::move(built);

        auto [guard, acquire_status] = channels_.acquire();
        if (acquire_status.failed()) return acquire_status;
        for (std::size_t index = 0; index != config.data_directories.size(); ++index)
            if (status_t const opened = open_volume_(config.data_directories[index], index, guard.channel(), database);
                opened.failed())
                return opened;
        return {};
    }

    /** Drains and releases the channels, unpins the arena, gives the targets back and drops the volumes. */
    status_t unmount() noexcept {
        status_t first;
        if (status_t const drained = channels_.release_all(); drained.failed() && first.succeeded()) first = drained;
        channels_ = channel_pool_t {};

        if (arena_.registered())
            if (status_t const unpinned = arena_.unregister(disk_); unpinned.failed() && first.succeeded())
                first = unpinned;
        arena_ = registered_memory_arena_t {};

        for (std::size_t index = 0; index != media_.size(); ++index) {
            if (!media_[index].held()) continue;
            if (status_t const detached = disk_.unregister_medium(media_[index]);
                detached.failed() && first.succeeded())
                first = detached;
        }
        media_.clear();

        router_ = router_t {};
        return first;
    }

    /** The transport underneath, which every read and write of a blob's bytes goes to directly. */
    [[nodiscard]] disk_type_& disk() noexcept { return disk_; }

    /** The transport underneath, for what a caller only asks of it. */
    [[nodiscard]] disk_type_ const& disk() const noexcept { return disk_; }

    /** The volumes of this database and the placement that decides which of them a blob lands on. */
    [[nodiscard]] router_t& router() noexcept { return router_; }

    /** The queue pairs every operation borrows one of, by executor thread index or from the shared channels. */
    [[nodiscard]] channel_pool_t& channels() noexcept { return channels_; }

    /** The one pinned range every I/O buffer of this database is carved out of. */
    [[nodiscard]] registered_memory_arena_t& arena() noexcept { return arena_; }

  private:

    /**
     *  @brief Opens the volume of @p directory as medium @p index, and adopts it into the router.
     *  @param[in] directory The path the volume file sits in, and the bytes the volume spans.
     *  @param[in] index The medium slot this volume takes, which @c unmount gives back.
     *  @param[inout] channel The channel the head and the catalog slots are read and written through.
     *  @param[in] database The database the volume belongs to, which a mount refuses to open under another.
     */
    status_t open_volume_(data_directory_config_t const& directory, std::size_t index, io_channel_t& channel,
                          uuid_t const& database) noexcept {
        if (directory.path.empty()) return {status_code_t::invalid_argument_k, "A data directory needs a path"};
        if (directory.max_size == 0 || directory.max_size == unlimited_resource_k)
            return {status_code_t::invalid_argument_k, "A data directory names the bytes its volume spans"};

        char path[path_max_bytes_k];
        if (status_t const joined = join_path(directory.path.c_str(), volume_file_name_k, path); joined.failed())
            return joined;

        medium_options_t request;
        request.name = path;
        request.length = directory.max_size;
        request.access = medium_access_t::create_read_write_k;
        auto [registered, register_medium_status] = disk_.register_medium(request);
        if (register_medium_status.failed()) return register_medium_status;
        media_[index] = std::move(registered);

        volume_format_t format;
        format.database = database;
        format.capacity_bytes = directory.max_size;
        expected<volume_t> opened = volume_t::mount(disk_, channel, media_[index], format);
        if (!opened) return opened.status();
        return router_.bind_queue_pair(std::move(*opened));
    }

    /** The transport, owned here and outliving every channel and volume that points at it. */
    disk_type_ disk_;

    /** The one pinned range, charged to the host budget once and carved into blocks by the channels. */
    registered_memory_arena_t arena_;

    /** Every queue pair of this mount, opened here and lent one operation at a time. */
    channel_pool_t channels_;

    /** One registered medium per data directory, given back before the volumes are dropped. */
    st::basic_vector<registered_medium_t> media_;

    /** The volumes those targets hold, and the placement that spreads blobs over them. */
    router_t router_;
};

/**
 *  @brief The storage of one database, in whichever transport the probe chose for it.
 *
 *  The probe runs once and the alternative it picked is emplaced in place, so the dispatch a caller
 *  pays is one @c visit per operation rather than one virtual call per request. A mount on files
 *  holds whichever of the three file transports the probe settled on, so a database materializes over
 *  one disk type; SPDK is named by the configuration and refused here until its disk is ported.
 */
class mounted_storage_t {
  public:

    /** The mount a suite and a benchmark ask for, whose volumes never reach a medium. */
    using memory_mount_t = basic_storage_mount<memory_disk_t>;

    /** The mount a database gets, on unbuffered files through io_uring, Linux AIO or the blocking floor. */
    using files_mount_t = basic_storage_mount<any_files_disk_t>;

    mounted_storage_t() noexcept = default;
    mounted_storage_t(mounted_storage_t const&) = delete;
    mounted_storage_t& operator=(mounted_storage_t const&) = delete;

    /**
     *  @brief Probes the transport @p config asks for and mounts the whole storage stack on it.
     *  @param[in] config The transport, the channel shape and the data directories, with their capacities.
     *  @param[inout] constraint The host budget the arena is charged to, once, for the life of the mount.
     *  @param[in] threads Executor threads, each of which gets a channel of its own.
     *  @param[in] database The database the volumes belong to, or an invalid identifier to take any.
     */
    status_t mount(resources_config_t const& config, resource_constraint_t& constraint, std::size_t threads,
                   uuid_t const& database) noexcept {
        if (mounted()) return {status_code_t::already_exists_k, "This database has already mounted its storage"};

        report_ = mount_report_t {};
        report_.linux_uring = linux_uring_files_disk_t::probe();
        report_.linux_aio = linux_aio_files_disk_t::probe();
        report_.transport = probed_transport(config.io_transport, report_.linux_uring, report_.linux_aio);

        std::uint16_t const owners = bounded_count_(threads + config.shared_io_channels,
                                                    static_cast<std::size_t>(queue_pair_id_t::unallocated_k));
        std::uint16_t const targets = bounded_count_(config.data_directories.size(),
                                                     static_cast<std::size_t>(registered_medium_id_t::unregistered_k));
        if (owners == 0 || targets == 0)
            return {status_code_t::capacity_exhausted_k, "A ring names fewer queue pairs or targets than that"};

        switch (report_.transport) {
        case io_transport_t::memory_k: {
            auto [device, make_memory_device_status] = memory_device_t::make(portable_alignment_k);
            if (make_memory_device_status.failed()) return make_memory_device_status;
            expected<memory_disk_t> disk = memory_disk_t::make(std::move(device),
                                                               ring_config_(config, owners, targets));
            if (!disk) return disk.status();
            return adopt_<memory_disk_t>(std::move(*disk), config, constraint, threads, database);
        }
        case io_transport_t::blocking_k: return mount_blocking_(config, constraint, threads, database, owners, targets);
        case io_transport_t::linux_uring_k:
            return mount_linux_uring_(config, constraint, threads, database, owners, targets);
        case io_transport_t::linux_aio_k:
            return mount_linux_aio_(config, constraint, threads, database, owners, targets);
        case io_transport_t::linux_spdk_k:
            return {status_code_t::unsupported_k, "The SPDK transport is not mounted by this build"};
        case io_transport_t::automatic_k: return {status_code_t::internal_k, "The probe left the transport unresolved"};
        }
        return {status_code_t::invalid_argument_k, "The configuration names no I/O transport"};
    }

    /** Tears the mounted transport down in order, and leaves the handle naming none. */
    status_t unmount() noexcept {
        if (!mounted()) return {};
        status_t const released = visit([](auto& mount) noexcept { return mount.unmount(); });
        held_.emplace<std::monostate>();
        report_ = mount_report_t {};
        return released;
    }

    /** Whether any transport is open, which every operation needs before it asks for a channel. */
    [[nodiscard]] bool mounted() const noexcept { return !std::holds_alternative<std::monostate>(held_); }

    /** What the mount opened, which a caller reports as the effective configuration. */
    [[nodiscard]] mount_report_t const& report() const noexcept { return report_; }

    /** How the mounted transport addresses its arena, which io_uring settles at each ring's first submission. */
    [[nodiscard]] range_pinning_t pinning() const noexcept {
        files_mount_t const* const files = std::get_if<files_mount_t>(&held_);
        return files ? files->disk().pinning() : range_pinning_t::unpinned_k;
    }

    /** Calls @p visitor with the mount that is open, or reports that none is. */
    template <typename visitor_type_>
    status_t visit(visitor_type_&& visitor) noexcept {
        if (memory_mount_t* const ram = std::get_if<memory_mount_t>(&held_)) return visitor(*ram);
        if (files_mount_t* const files = std::get_if<files_mount_t>(&held_)) return visitor(*files);
        return {status_code_t::uninitialized_k, "This database has mounted no storage"};
    }

  private:

    /** The ring shape every transport is made with, which the channels and the targets fix. */
    [[nodiscard]] static disk_config_t ring_config_(resources_config_t const& config, std::uint16_t owners,
                                                    std::uint16_t targets) noexcept {
        disk_config_t ring;
        ring.queue_pairs_limit = owners;
        ring.depth_limit = config.channels.depth;
        ring.registered_memory_limit = 1;
        ring.media_limit = targets;
        return ring;
    }

    /** Mounts the blocking floor on unbuffered files, with @p owners queue pairs over @p targets directories. */
    status_t mount_blocking_(resources_config_t const& config, resource_constraint_t& constraint, std::size_t threads,
                             uuid_t const& database, std::uint16_t owners, std::uint16_t targets) noexcept {
        if (status_t const made = create_data_directories(config); made.failed()) return made;
        auto [device, make_device_status] = blocking_files_device_t::make(file_buffering_t::unbuffered_k);
        if (make_device_status.failed()) return make_device_status;
        expected<blocking_files_disk_t> disk = blocking_files_disk_t::make(std::move(device),
                                                                           ring_config_(config, owners, targets));
        if (!disk) return disk.status();
        return adopt_<any_files_disk_t>(any_files_disk_t {std::move(*disk)}, config, constraint, threads, database);
    }

    /** Mounts io_uring on unbuffered files, one ring per queue pair, with @p owners queue pairs over @p targets
     *  directories. */
    status_t mount_linux_uring_(resources_config_t const& config, resource_constraint_t& constraint,
                                std::size_t threads, uuid_t const& database, std::uint16_t owners,
                                std::uint16_t targets) noexcept {
        if (status_t const made = create_data_directories(config); made.failed()) return made;
        linux_uring_disk_config_t ring;
        ring.queue_pairs_limit = owners;
        ring.depth_limit = config.channels.depth;
        ring.registered_memory_limit = 1;
        ring.media_limit = targets;
        ring.buffering = file_buffering_t::unbuffered_k;
        auto [disk, make_disk_status] = linux_uring_files_disk_t::make(ring);
        if (make_disk_status.failed()) return make_disk_status;
        return adopt_<any_files_disk_t>(any_files_disk_t {std::move(disk)}, config, constraint, threads, database);
    }

    /** Mounts Linux AIO on unbuffered files, one context per queue pair, with @p owners queue pairs over @p targets
     *  directories. */
    status_t mount_linux_aio_(resources_config_t const& config, resource_constraint_t& constraint, std::size_t threads,
                              uuid_t const& database, std::uint16_t owners, std::uint16_t targets) noexcept {
        if (status_t const made = create_data_directories(config); made.failed()) return made;
        linux_aio_disk_config_t contexts;
        contexts.queue_pairs_limit = owners;
        contexts.depth_limit = config.channels.depth;
        contexts.registered_memory_limit = 1;
        contexts.media_limit = targets;
        contexts.buffering = file_buffering_t::unbuffered_k;
        auto [disk, make_disk_status] = linux_aio_files_disk_t::make(contexts);
        if (make_disk_status.failed()) return make_disk_status;
        return adopt_<any_files_disk_t>(any_files_disk_t {std::move(disk)}, config, constraint, threads, database);
    }

    /** @p wanted as the 16-bit count a ring names things by, or zero when @p reserved leaves no room. */
    [[nodiscard]] static std::uint16_t bounded_count_(std::size_t wanted, std::size_t reserved) noexcept {
        return wanted != 0 && wanted < reserved ? static_cast<std::uint16_t>(wanted) : std::uint16_t {0};
    }

    /** Emplaces the mount of @p disk, opens the storage on it and records what it got. */
    template <typename disk_type_>
    status_t adopt_(disk_type_&& disk, resources_config_t const& config, resource_constraint_t& constraint,
                    std::size_t threads, uuid_t const& database) noexcept {
        basic_storage_mount<disk_type_>& opened = held_.template emplace<basic_storage_mount<disk_type_>>(
            std::move(disk));
        if (status_t const started = opened.mount(config, constraint, threads, database); started.failed()) {
            held_.emplace<std::monostate>();
            return started;
        }
        report_.volumes = opened.router().size();
        report_.channels = opened.channels().size();
        report_.arena_bytes = opened.arena().size();
        return {};
    }

    /** The one mount this database opened, or @c std::monostate while it has opened none. */
    std::variant<std::monostate, memory_mount_t, files_mount_t> held_;

    /** What the probe settled on and what the mount took, reported as the effective configuration. */
    mount_report_t report_;
};

#pragma endregion Mount

#pragma endregion Filesystem

#pragma region Resources

#pragma region Background Jobs

/**
 *  @brief The flag one owner raises to stop the background jobs it submitted.
 *
 *  An LSM tree flushing its write buffer, a level merger compacting and a backup manager copying
 *  files each hold one, so a job always has a named owner that starts it, observes it and stops
 *  it. Every part of a running job polls @c cancelled between units of work, and the gate refuses
 *  a fresh job whose owner has already raised the flag.
 */
class job_cancellation_t {
  public:

    job_cancellation_t() noexcept = default;
    job_cancellation_t(job_cancellation_t const&) = delete;
    job_cancellation_t& operator=(job_cancellation_t const&) = delete;

    /** Stops the jobs this owner submitted, and refuses the ones it has not submitted yet. */
    void cancel() noexcept {
        flag_reference_().store(static_cast<std::int32_t>(cancellation_state_t::cancelled_k),
                                std::memory_order_release);
    }

    /** Admits this owner's jobs again, for a component reopened in place. */
    void resume() noexcept {
        flag_reference_().store(static_cast<std::int32_t>(cancellation_state_t::wanted_k), std::memory_order_release);
    }

    /** Whether the owner has stopped its jobs, which every part of a running one polls. */
    bool cancelled() const noexcept {
        return flag_reference_().load(std::memory_order_acquire) !=
               static_cast<std::int32_t>(cancellation_state_t::wanted_k);
    }

  private:

    /** A @c cancellation_state_t, moved by @c cancel and @c resume. */
    std::int32_t flag_ = 0;

    fu::preferred_atomic_ref<std::int32_t> flag_reference_() noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {flag_};
    }

    fu::preferred_atomic_ref<std::int32_t const> flag_reference_() const noexcept {
        return fu::preferred_atomic_ref<std::int32_t const> {flag_};
    }
};

/** Where the pool of a @c background_executor_t stands between its making and its first job. */
enum class pool_state_t : std::int32_t {

    /** No thread exists yet, and the next job admitted spawns them. */
    dormant_k = 0,

    /** One submitter is spawning the threads, and every other one spins until it is done. */
    spawning_k = 1,

    /** The threads are up and every job fans out over them. */
    spawned_k = 2,
};

/**
 *  @brief The executor every background job of one database runs on, with the bound on jobs in flight.
 *
 *  There are no fibers and no repeating loops: an owner calls @c submit_for_n or @c submit_for_threads
 *  when a threshold it tracks is crossed, the work fans out across the pool, and the call returns once
 *  every part has finished. The gate admits a job only while fewer than @c jobs_limit are running, so
 *  thresholds crossing at once queue at their owners rather than in the pool, whose dispatch admits one
 *  caller at a time. The first job admitted spawns the pool, and every submitter only coordinates it,
 *  so a mounted database that never crosses a threshold owns no threads and pins none.
 */
class background_executor_t {
  public:

    background_executor_t(background_executor_t&&) noexcept = default;
    background_executor_t& operator=(background_executor_t&&) noexcept = default;
    background_executor_t(background_executor_t const&) = delete;
    background_executor_t& operator=(background_executor_t const&) = delete;

    /**
     *  @brief Sets the thread count, spawning nothing yet, and admits @p jobs_limit jobs at a time.
     *  @param[in] threads Thread count, at least one, which the first job admitted spawns.
     *  @param[in] jobs_limit Jobs that may be in flight at once, at least one.
     */
    static expected<background_executor_t> make(std::size_t threads, std::size_t jobs_limit) noexcept {
        if (threads == 0 || jobs_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A background executor needs a thread and a job"};
        return background_executor_t {threads, static_cast<std::int32_t>(jobs_limit)};
    }

    /** The number of threads a job fans out over, which the submitting thread is never among. */
    [[nodiscard]] std::size_t threads_count() const noexcept { return threads_; }

    /** Whether the pool is still to be spawned, being spawned by a submitter right now, or up. */
    [[nodiscard]] pool_state_t pool_state() const noexcept {
        return static_cast<pool_state_t>(pool_state_reference_().load(std::memory_order_acquire));
    }

    /** The jobs running right now, which never passes the limit the gate was made with. */
    [[nodiscard]] std::size_t jobs_in_flight() const noexcept {
        return static_cast<std::size_t>(jobs_in_flight_reference_().load(std::memory_order_acquire));
    }

    /** The jobs that may run at once. */
    [[nodiscard]] std::size_t jobs_limit() const noexcept { return static_cast<std::size_t>(jobs_limit_); }

    /**
     *  @brief Runs @p callback over @p count parts of @p owner's job, split evenly across the threads.
     *  @param[in] owner The flag the job's owner cancels it through, which the gate reads before admitting it.
     *  @param[in] count Parts of the job, which may be zero for a job with nothing to do.
     *  @param[in] callback Runs once per part, with the part index and the index of the thread running it.
     */
    template <task_callback callback_>
    status_t submit_for_n(job_cancellation_t const& owner, std::size_t count, callback_&& callback) noexcept {
        if (status_t const admitted = admit_(owner); admitted.failed()) return admitted;
        st::status_t const dispatched = executor_->for_n(count, std::forward<callback_>(callback));
        retire_();
        return to_status(dispatched);
    }

    /**
     *  @brief Runs @p callback once on every thread of @p owner's job.
     *  @param[in] owner The flag the job's owner cancels it through, which the gate reads before admitting it.
     *  @param[in] callback Runs once per thread, with the thread index and, when it takes one, the compute domain.
     */
    template <thread_callback callback_>
    status_t submit_for_threads(job_cancellation_t const& owner, callback_&& callback) noexcept {
        if (status_t const admitted = admit_(owner); admitted.failed()) return admitted;
        st::status_t const dispatched = executor_->for_threads(std::forward<callback_>(callback));
        retire_();
        return to_status(dispatched);
    }

  private:

    background_executor_t(std::size_t threads, std::int32_t jobs_limit) noexcept
        : threads_(threads), jobs_limit_(jobs_limit) {}

    /** Takes one of the job slots, spawning the pool for the first, or reports why this job does not run. */
    status_t admit_(job_cancellation_t const& owner) noexcept {
        if (owner.cancelled()) return {status_code_t::uninitialized_k, "The owner cancelled its background jobs"};
        if (status_t const spawned = spawn_(); spawned.failed()) return spawned;
        std::int32_t const observed = jobs_in_flight_reference_().fetch_add_if_at_most(1, jobs_limit_,
                                                                                       std::memory_order_acquire);
        if (observed + 1 > jobs_limit_)
            return {status_code_t::capacity_exhausted_k, "Too many background jobs are already in flight"};
        return {};
    }

    /** Hands the job slot back, so the next threshold crossing is admitted. */
    void retire_() noexcept { jobs_in_flight_reference_().sub(1, std::memory_order_release); }

    /** Spawns the pool once, on the first job, spinning out a spawn another submitter has under way. */
    status_t spawn_() noexcept {
        constexpr std::int32_t dormant_k = static_cast<std::int32_t>(pool_state_t::dormant_k);
        constexpr std::int32_t spawning_k = static_cast<std::int32_t>(pool_state_t::spawning_k);
        constexpr std::int32_t spawned_k = static_cast<std::int32_t>(pool_state_t::spawned_k);
        fu::preferred_atomic_ref<std::int32_t> const state = pool_state_reference_();
        std::int32_t observed = dormant_k;
        while (
            !state.compare_exchange_weak(observed, spawning_k, std::memory_order_acquire, std::memory_order_acquire)) {
            if (observed == spawned_k) return {};
            observed = dormant_k;
        }
        st::expected<executor_t> executor = executor_t::make(threads_, caller_exclusivity_t::caller_exclusive_k);
        if (!executor) {
            state.store(dormant_k, std::memory_order_release);
            return to_status(executor.status());
        }
        executor_.emplace(std::move(*executor));
        state.store(spawned_k, std::memory_order_release);
        return {};
    }

    fu::preferred_atomic_ref<std::int32_t> jobs_in_flight_reference_() noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {jobs_in_flight_};
    }

    fu::preferred_atomic_ref<std::int32_t const> jobs_in_flight_reference_() const noexcept {
        return fu::preferred_atomic_ref<std::int32_t const> {jobs_in_flight_};
    }

    fu::preferred_atomic_ref<std::int32_t> pool_state_reference_() noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {pool_state_};
    }

    fu::preferred_atomic_ref<std::int32_t const> pool_state_reference_() const noexcept {
        return fu::preferred_atomic_ref<std::int32_t const> {pool_state_};
    }

    /** The threads every job fans out over, never parked, since @c fu_pool_sleep races a concurrent @c admit_. */
    std::optional<executor_t> executor_;

    /** The threads the first job spawns, which @c threads_count answers before and after the spawn. */
    std::size_t threads_ = 1;

    /** A @c pool_state_t moved by @c spawn_ alone, by compare-exchange out of @c dormant_k. */
    std::int32_t pool_state_ = 0;

    /** The jobs running right now, raised by @c admit_ and lowered by @c retire_. */
    std::int32_t jobs_in_flight_ = 0;

    /** The jobs that may run at once, fixed when the gate is made. */
    std::int32_t jobs_limit_ = 1;
};

using background_executor_shared_ptr_t = std::shared_ptr<background_executor_t>;

#pragma endregion Background Jobs

#pragma region Shared Resources

/** Budgets and services every collection of one database shares. */
struct resources_t {

    /** Budget of host memory. */
    resource_constraint_t memory_constraint;

    /** Budget of GPU memory, zero when no GPU is used. */
    resource_constraint_t gpu_memory_constraint;

    /** GPU devices compaction may use. */
    st::basic_vector<int> gpu_devices;

    /** Threads and the job gate every flush, compaction and backup passes. */
    background_executor_shared_ptr_t executor;

    /** The transport, the pinned arena, the channels and the volumes mounted over the data directories. */
    mounted_storage_t storage;

    /** Sets the memory budgets, leaving the services for the caller to attach. */
    explicit resources_t(std::size_t memory_limit, std::size_t gpu_memory_limit = 0) noexcept
        : memory_constraint(memory_limit), gpu_memory_constraint(gpu_memory_limit) {}
};

using resources_shared_ptr_t = std::shared_ptr<resources_t>;

/** Constructs @p object_ in memory charged to the host budget, or returns an empty pointer once it runs out. */
template <typename object_, typename... arguments_>
std::shared_ptr<object_> make_constrained_shared(resources_shared_ptr_t const& resources,
                                                 arguments_&&... arguments) noexcept {
    constrained_allocator<byte_t> allocator {resources->memory_constraint};
    auto* const pointer = reinterpret_cast<object_*>(allocator.allocate(sizeof(object_)));
    if (!pointer) return {};

    auto deleter = [allocator](object_* constructed) mutable {
        constructed->~object_();
        allocator.deallocate(reinterpret_cast<byte_t*>(constructed), sizeof(object_));
    };
    return std::shared_ptr<object_>(new (pointer) object_(std::forward<arguments_>(arguments)...), deleter);
}

/** The jobs one database admits at once: one per data directory, one for its metadata and ten for compaction. */
inline std::size_t background_jobs_limit(std::size_t data_directories_count) noexcept {
    return data_directories_count + 1 + 10;
}

/**
 *  @brief Creates the metadata directory, opens the executor and mounts the storage of one database.
 *  @param[in] config Budgets, threads, the metadata directory and the data directories to mount.
 *  @param[in] database The database the volumes belong to, or an invalid identifier to take any.
 *  @return The resources, whose destructor tears the mount down in the order it was built in.
 */
inline expected<resources_shared_ptr_t> open_resources(resources_config_t const& config,
                                                       uuid_t const& database) noexcept {
    if (config.directory.empty()) return status_t {status_code_t::invalid_argument_k, "A database needs a directory"};
    if (status_t const made = create_directories(config.directory.c_str()); made.failed()) return made;

    resources_shared_ptr_t resources = std::make_shared<resources_t>(config.memory_limit, config.gpu_memory_limit);
    if (resources->gpu_devices.resize(config.gpu_devices.size()) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "A database could not list the GPUs it may use"};
    for (std::size_t index = 0; index != config.gpu_devices.size(); ++index)
        resources->gpu_devices[index] = config.gpu_devices[index];

    std::size_t threads = config.threads;
    if (threads == 0) {
        auto [cores, logical_cores_count_status] = executor_t::logical_cores_count();
        if (st::failed(logical_cores_count_status)) return to_status(logical_cores_count_status);
        threads = derived_background_threads(cores);
    }
    expected<background_executor_t> executor = background_executor_t::make(
        threads, background_jobs_limit(config.data_directories.size()));
    if (!executor) return executor.status();
    resources->executor = std::make_shared<background_executor_t>(std::move(*executor));

    if (status_t const mounted = resources->storage.mount(config, resources->memory_constraint,
                                                          resources->executor->threads_count(), database);
        mounted.failed())
        return mounted;
    return resources;
}

#pragma endregion Shared Resources

#pragma endregion Resources

} // namespace unum::ustore::utilities
