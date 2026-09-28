/**
 *  @file src/nvme/volume.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief One disk and the names it resolves to extents, in the two shapes a medium allows.
 *
 *  A volume maps names to bytes and a transport moves them; the pair is one disk. Where there is a
 *  filesystem the filesystem is the catalog, so a run is a file, publication is a rename, and a
 *  crash leaves a partial file the mount deletes. Where there is none - a namespace, a passthrough
 *  character device, an SPDK blobstore - names live in a catalog of two slots inside the volume, and
 *  publication is the slot swap behind an image checksum.
 *
 *  Both carry the order @c verification/durability.pml checks: the bytes are written, flushed,
 *  recorded, and only then is the record synced, so an extent missing from the catalog is free after
 *  a crash. Neither is on the read path - a name is resolved once when a run is opened, and every
 *  block read afterwards is an extent plus a submit.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <array>   // `std::array`
#include <bit>     // `std::has_single_bit`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/buffer_pool.hpp"      // `registered_memory_arena_t`, `pooled_buffer_t`
#include "nvme/catalog.hpp"          // `catalog_t`, `blob_kind_t`, `volume_header_t`
#include "nvme/extent.hpp"           // `extent_t`
#include "nvme/extent_allocator.hpp" // `extent_allocator_t`
#include "nvme/io_channel.hpp"       // `io_channel_t`
#include "nvme/io_pipeline.hpp"      // `basic_read_pipeline`, `basic_write_pipeline`
#include "nvme/ring.hpp"             // `ring_concept`, `registered_medium_t`

namespace unum::ustore::nvme {

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
struct blob_t {

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
    expected<blob_t> create(blob_name_t name, blob_kind_t kind, std::uint64_t length) noexcept {
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
    expected<blob_t> find(blob_name_t name) const noexcept {
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
    status_t seal(io_channel_t& channel, blob_t const& blob) noexcept {
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
    [[nodiscard]] blob_t blob_of_(extent_t extent, std::uint64_t length) const noexcept {
        blob_t blob;
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
        if (taken != 1) return status_t {status_code_t::input_output_k, "The ring would not take a catalog read"};
        auto [completed, poll_completions_status] = poll_completions_at_least(disk_, channel.queue_pair(),
                                                                              channel.completion_buffer(), 1);
        if (poll_completions_status.failed()) return poll_completions_status;
        if (completed == 0) return status_t {status_code_t::input_output_k, "A catalog read never completed"};

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

} // namespace unum::ustore::nvme
