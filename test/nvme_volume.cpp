/**
 *  @file test/nvme_volume.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The catalog of two slots, the crashes it is built to survive, and the router above it.
 *
 *  The result this file exists for is @c crash_before_seal: a range whose bytes were written but
 *  never recorded must come back free, which is the ordering @c verification/durability.pml models
 *  and the only reason the volume publishes a whole image rather than an appended record. Every case
 *  tears the medium at an exact offset between two mounts through the same pipelines the volume
 *  writes with, so it runs on every transport the driver mounts.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <array>   // `std::array`
#include <memory>  // `std::unique_ptr`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "nvme/io_pipeline.hpp"   // `basic_read_pipeline`, `basic_write_pipeline`
#include "nvme/volume_router.hpp" // `basic_volume_router`

#include "nvme_fixture.hpp" // `basic_fixture`, `run_on_transport`, `painted`

namespace unum::ustore::test {

using namespace unum::ustore::nvme;

#pragma region Shape

/** The numbers every case mounts with: a slot longer than a request block and shorter than a stream. */
constexpr fixture_config_t volume_config_k {
    .stream_buffers = 2,
    .catalog_slot_bytes = 128ull * 1024ull,
    .volume_bytes = 32ull * 1024ull * 1024ull,
    .shared_io_channels = 2,
    .memory_limit = std::size_t {64} * 1024 * 1024,
};

/** A blob name held inline, since the catalog compares names as bytes and never as text. */
struct blob_label_t {

    /** The name's bytes, of which the first @c length are the name. */
    std::array<byte_t, blob_name_max_bytes_k> storage {};

    /** Bytes of the name, which no case leaves at zero. */
    std::uint16_t length = 0;

    /** Builds a name of @p bytes bytes that no other @p seed produces. */
    blob_label_t(std::uint64_t seed, std::uint16_t bytes = 16) noexcept : length(bytes) {
        for (std::uint16_t index = 0; index != bytes; ++index) {
            std::uint64_t const shifted = seed >> ((index % 8u) * 8u);
            storage[index] = static_cast<byte_t>((shifted ^ (index * 31u + 11u)) & 0xFFu);
        }
    }

    /** The name, which borrows this label and dies with it. */
    [[nodiscard]] blob_name_t name() const noexcept { return blob_name_t {storage.data(), length}; }
};

#pragma endregion Shape

#pragma region Byte Movers

/** Writes the pattern of @p seed over the whole extent of @p blob, zeroing the padding past its length. */
template <typename disk_type_>
status_t write_blob(disk_type_& disk, io_channel_t& channel, blob_t const& blob, std::uint64_t seed) noexcept {
    basic_write_pipeline<disk_type_> writer {disk, channel, blob.medium};
    std::uint64_t written = 0;
    while (written != blob.extent.length) {
        auto [block, acquire_buffer_or_poll_status] = writer.acquire_buffer_or_poll();
        if (acquire_buffer_or_poll_status.failed()) return acquire_buffer_or_poll_status;
        std::uint64_t const room = blob.extent.length - written;
        std::uint64_t const chunk = block.size() < room ? block.size() : room;
        std::span<byte_t> const bytes = block.bytes();
        for (std::uint64_t index = 0; index != chunk; ++index)
            bytes[static_cast<std::size_t>(index)] = written + index < blob.length ? painted(seed, written + index)
                                                                                   : byte_t {};
        if (status_t const queued = writer.enqueue_write(std::move(block), blob.slice(written, chunk)); queued.failed())
            return queued;
        if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;
        written += chunk;
    }
    return writer.make_durable();
}

/** Bytes of @p blob that do not hold the pattern of @p seed, which a round trip expects to be none. */
template <typename disk_type_>
expected<std::size_t> mismatched_bytes(disk_type_& disk, io_channel_t& channel, blob_t const& blob,
                                       std::uint64_t seed) noexcept {
    basic_read_pipeline<disk_type_> reader {disk, channel, blob.medium};
    std::uint64_t const chunk_bytes = channel.requests().buffer_bytes();
    std::uint64_t const chunks = buffers_for(blob.extent.length, chunk_bytes);
    std::uint64_t offered = 0;
    std::size_t seen = 0;
    std::size_t mismatches = 0;
    auto sink = [&](completed_buffer_t const& completed) noexcept {
        ++seen;
        if (require_completed_in_full(completed).failed()) {
            ++mismatches;
            return;
        }
        std::uint64_t const base = static_cast<std::uint64_t>(completed.tag) * chunk_bytes;
        for (std::size_t index = 0; index != completed.bytes.size(); ++index) {
            std::uint64_t const position = base + index;
            if (position >= blob.length) break;
            if (completed.bytes[index] != painted(seed, position)) ++mismatches;
        }
    };

    while (offered != chunks || reader.outstanding() != 0) {
        while (offered != chunks && reader.can_enqueue()) {
            std::uint64_t const at = offered * chunk_bytes;
            std::uint64_t const room = blob.extent.length - at;
            std::uint64_t const length = chunk_bytes < room ? chunk_bytes : room;
            if (reader.enqueue_read(blob.slice(at, length), static_cast<std::uint32_t>(offered)).failed()) break;
            ++offered;
        }
        if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
        auto [polled, poll_completions_status] = reader.poll_completions(sink);
        if (poll_completions_status.failed()) return poll_completions_status;
    }
    if (seen != chunks) return status_t {status_code_t::input_output_k, "A blob read lost a block"};
    return mismatches;
}

/** Reads one page of @p medium into @p into, which the corruption cases edit and write back. */
template <typename disk_type_>
status_t fetch_page(disk_type_& disk, io_channel_t& channel, medium_reference_t medium, std::uint64_t offset,
                    std::span<byte_t> into) noexcept {
    basic_read_pipeline<disk_type_> reader {disk, channel, medium};
    if (status_t const offered = reader.enqueue_read(extent_t {offset, into.size()}, 0); offered.failed())
        return offered;
    if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
    status_t sink_status;
    auto sink = [&](completed_buffer_t const& completed) noexcept {
        if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
            sink_status = whole;
            return;
        }
        std::memcpy(into.data(), completed.bytes.data(), into.size());
    };
    if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
    return sink_status;
}

/** Writes @p from over one page of @p medium, which is how a case tears an image on the environment. */
template <typename disk_type_>
status_t store_page(disk_type_& disk, io_channel_t& channel, medium_reference_t medium, std::uint64_t offset,
                    std::span<byte_t const> from) noexcept {
    basic_write_pipeline<disk_type_> writer {disk, channel, medium};
    auto [block, acquire_buffer_status] = writer.acquire_buffer();
    if (acquire_buffer_status.failed()) return acquire_buffer_status;
    std::memcpy(block.bytes().data(), from.data(), from.size());
    if (status_t const queued = writer.enqueue_write(std::move(block), extent_t {offset, from.size()}); queued.failed())
        return queued;
    return writer.make_durable();
}

/** The head of the volume as it sits on the environment, which the corruption cases read the geometry from. */
template <typename disk_type_>
expected<volume_header_t> fetch_head(disk_type_& disk, io_channel_t& channel, medium_reference_t medium) noexcept {
    std::array<byte_t, volume_config_k.page_bytes> page {};
    if (status_t const fetched = fetch_page(disk, channel, medium, 0, page); fetched.failed()) return fetched;
    volume_header_t header {};
    std::memcpy(&header, page.data(), sizeof(header));
    return header;
}

/** The generation slot @p slot carries on the environment, or zero when its image no longer reads. */
template <typename disk_type_>
expected<std::uint64_t> fetch_slot_generation(disk_type_& disk, io_channel_t& channel, medium_reference_t medium,
                                              volume_header_t const& header, std::uint32_t slot) noexcept {
    std::array<byte_t, volume_config_k.page_bytes> page {};
    std::uint64_t const offset = header.catalog_slot_offset + slot * header.catalog_slot_bytes;
    if (status_t const fetched = fetch_page(disk, channel, medium, offset, page); fetched.failed()) return fetched;
    catalog_slot_header_t slot_header {};
    std::memcpy(&slot_header, page.data(), sizeof(slot_header));
    return slot_header.magic == catalog_slot_magic_k ? slot_header.generation : 0;
}

/** Garbles the records of slot @p slot while keeping its header, which is what a torn image looks like. */
template <typename disk_type_>
status_t tear_slot_records(disk_type_& disk, io_channel_t& channel, medium_reference_t medium,
                           volume_header_t const& header, std::uint32_t slot) noexcept {
    std::array<byte_t, volume_config_k.page_bytes> page {};
    std::uint64_t const offset = header.catalog_slot_offset + slot * header.catalog_slot_bytes;
    if (status_t const fetched = fetch_page(disk, channel, medium, offset, page); fetched.failed()) return fetched;
    for (std::size_t index = sizeof(catalog_slot_header_t); index != page.size(); ++index)
        page[index] = static_cast<byte_t>(0x5Au);
    return store_page(disk, channel, medium, offset, page);
}

/** Overwrites the whole first page of slot @p slot, which leaves it without even a magic. */
template <typename disk_type_>
status_t erase_slot(disk_type_& disk, io_channel_t& channel, medium_reference_t medium, volume_header_t const& header,
                    std::uint32_t slot) noexcept {
    std::array<byte_t, volume_config_k.page_bytes> page {};
    for (std::size_t index = 0; index != page.size(); ++index) page[index] = static_cast<byte_t>(0xA5u);
    std::uint64_t const offset = header.catalog_slot_offset + slot * header.catalog_slot_bytes;
    return store_page(disk, channel, medium, offset, page);
}

/** Whether the first page of slot @p slot still holds only the byte @c erase_slot wrote. */
template <typename disk_type_>
expected<bool> slot_is_erased(disk_type_& disk, io_channel_t& channel, medium_reference_t medium,
                              volume_header_t const& header, std::uint32_t slot) noexcept {
    std::array<byte_t, volume_config_k.page_bytes> page {};
    std::uint64_t const offset = header.catalog_slot_offset + slot * header.catalog_slot_bytes;
    if (status_t const fetched = fetch_page(disk, channel, medium, offset, page); fetched.failed()) return fetched;
    for (std::size_t index = 0; index != page.size(); ++index)
        if (page[index] != static_cast<byte_t>(0xA5u)) return false;
    return true;
}

/** Creates, writes and seals one blob of @p seed, which most cases want as one step. */
template <typename disk_type_>
expected<blob_t> publish_blob(disk_type_& disk, io_channel_t& channel, basic_extent_volume<disk_type_>& volume,
                              blob_name_t name, blob_kind_t kind, std::uint64_t length, std::uint64_t seed) noexcept {
    auto [blob, create_status] = volume.create(name, kind, length);
    if (create_status.failed()) return create_status;
    if (status_t const written = write_blob(disk, channel, blob, seed); written.failed()) return written;
    if (status_t const sealed = volume.seal(channel, blob); sealed.failed()) return sealed;
    return blob;
}

#pragma endregion Byte Movers

#pragma region Durability Cases

/** A blob written through a pipeline and sealed comes back byte for byte under its own name. */
template <typename fixture_type_>
std::size_t volume_round_trips(fixture_type_& fixture) {
    using guard_t = typename fixture_type_::guard_t;
    using volume_t = typename fixture_type_::volume_t;
    std::size_t failures = 0;
    constexpr std::uint64_t length_k = 100'000;
    expected<guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    expected<volume_t> volume = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
    failures += expect(volume.has_value());
    if (!volume) return failures;

    blob_label_t const label {7};
    expected<blob_t> const blob = publish_blob(fixture.disk, guard->channel(), *volume, label.name(),
                                               blob_kind_t::sorted_run_k, length_k, 7);
    failures += expect(blob.has_value());
    if (!blob) return failures;
    failures += expect_eq(blob->length, length_k);
    failures += expect_eq(blob->extent.length, rounded_up_to(length_k, volume_config_k.page_bytes));

    expected<blob_t> const found = volume->find(label.name());
    failures += expect(found.has_value());
    if (!found) return failures;
    failures += expect(found->extent == blob->extent);
    failures += expect_eq(found->length, length_k);

    expected<std::size_t> const mismatches = mismatched_bytes(fixture.disk, guard->channel(), *found, 7);
    failures += expect(mismatches.has_value());
    if (mismatches) failures += expect_eq(*mismatches, std::size_t {0});
    return failures;
}

/**
 *  @brief The result this file exists for: bytes written and never sealed leave the range free.
 *
 *  This is the ordering @c verification/durability.pml checks. The blob is created, every byte of it
 *  reaches the environment, and the process dies before the catalog names it; a mount of the same medium
 *  must therefore resolve nothing and hand the range straight back to the allocator.
 */
template <typename fixture_type_>
std::size_t volume_forgets_an_unsealed_blob(fixture_type_& fixture) {
    using guard_t = typename fixture_type_::guard_t;
    using volume_t = typename fixture_type_::volume_t;
    std::size_t failures = 0;
    constexpr std::uint64_t length_k = 1024ull * 1024ull;
    constexpr std::uint64_t neighbour_k = 64ull * 1024ull;
    expected<guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    blob_label_t const lost {11};
    blob_label_t const dragged {12};
    blob_label_t const kept {13};
    std::uint64_t free_before = 0;
    {
        expected<volume_t> volume = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
        failures += expect(volume.has_value());
        if (!volume) return failures;
        free_before = volume->free_bytes();

        expected<blob_t> const blob = volume->create(lost.name(), blob_kind_t::sorted_run_k, length_k);
        failures += expect(blob.has_value());
        if (!blob) return failures;
        failures += expect(write_blob(fixture.disk, guard->channel(), *blob, 11).succeeded());
        failures += expect(volume->free_bytes() < free_before);
        failures += expect_eq(volume->reservations(), std::size_t {1});
        failures += expect_status(volume->find(lost.name()), status_code_t::not_found_k);
    }

    std::uint64_t kept_bytes = 0;
    {
        expected<volume_t> replayed = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
        failures += expect(replayed.has_value());
        if (!replayed) return failures;
        failures += expect_status(replayed->find(lost.name()), status_code_t::not_found_k);
        failures += expect_eq(replayed->free_bytes(), free_before);
        failures += expect_eq(replayed->size(), std::size_t {0});

        // The seal of one blob publishes that blob, never the reservation another caller holds.
        expected<blob_t> const unsealed = replayed->create(dragged.name(), blob_kind_t::sorted_run_k, length_k);
        failures += expect(unsealed.has_value());
        if (!unsealed) return failures;
        failures += expect(write_blob(fixture.disk, guard->channel(), *unsealed, 12).succeeded());
        expected<blob_t> const neighbour = publish_blob(fixture.disk, guard->channel(), *replayed, kept.name(),
                                                        blob_kind_t::user_data_k, neighbour_k, 13);
        failures += expect(neighbour.has_value());
        if (!neighbour) return failures;
        kept_bytes = neighbour->extent.length;
    }

    expected<volume_t> settled = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
    failures += expect(settled.has_value());
    if (!settled) return failures;
    failures += expect_status(settled->find(dragged.name()), status_code_t::not_found_k);
    failures += expect_eq(settled->size(), std::size_t {1});
    failures += expect_eq(settled->free_bytes(), free_before - kept_bytes);
    expected<blob_t> const survivor = settled->find(kept.name());
    failures += expect(survivor.has_value());
    if (!survivor) return failures;
    expected<std::size_t> const mismatches = mismatched_bytes(fixture.disk, guard->channel(), *survivor, 13);
    failures += expect(mismatches.has_value());
    if (mismatches) failures += expect_eq(*mismatches, std::size_t {0});
    return failures;
}

/** A blob that was sealed before the crash survives it, with every byte where it was left. */
template <typename fixture_type_>
std::size_t volume_keeps_a_sealed_blob(fixture_type_& fixture) {
    using guard_t = typename fixture_type_::guard_t;
    using volume_t = typename fixture_type_::volume_t;
    std::size_t failures = 0;
    constexpr std::uint64_t length_k = 300'000;
    expected<guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    blob_label_t const label {13};
    extent_t sealed_extent;
    {
        expected<volume_t> volume = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
        failures += expect(volume.has_value());
        if (!volume) return failures;
        expected<blob_t> const blob = publish_blob(fixture.disk, guard->channel(), *volume, label.name(),
                                                   blob_kind_t::user_data_k, length_k, 13);
        failures += expect(blob.has_value());
        if (!blob) return failures;
        sealed_extent = blob->extent;
    }

    expected<volume_t> replayed = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
    failures += expect(replayed.has_value());
    if (!replayed) return failures;
    expected<blob_t> const found = replayed->find(label.name());
    failures += expect(found.has_value());
    if (!found) return failures;
    failures += expect(found->extent == sealed_extent);
    failures += expect_eq(found->length, length_k);
    failures += expect_eq(replayed->size(), std::size_t {1});

    expected<std::size_t> const mismatches = mismatched_bytes(fixture.disk, guard->channel(), *found, 13);
    failures += expect(mismatches.has_value());
    if (mismatches) failures += expect_eq(*mismatches, std::size_t {0});
    return failures;
}

/** A torn newer slot costs the last seal and nothing before it, which is why there are two slots. */
template <typename fixture_type_>
std::size_t volume_survives_a_torn_slot(fixture_type_& fixture) {
    using guard_t = typename fixture_type_::guard_t;
    using volume_t = typename fixture_type_::volume_t;
    std::size_t failures = 0;
    expected<guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    blob_label_t const older {21};
    blob_label_t const newer {22};
    medium_reference_t const medium = fixture.media[0].reference();
    extent_t older_extent;
    {
        expected<volume_t> volume = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
        failures += expect(volume.has_value());
        if (!volume) return failures;
        expected<blob_t> const first = publish_blob(fixture.disk, guard->channel(), *volume, older.name(),
                                                    blob_kind_t::sorted_run_k, 40'000, 21);
        failures += expect(first.has_value());
        if (!first) return failures;
        older_extent = first->extent;
        expected<blob_t> const second = publish_blob(fixture.disk, guard->channel(), *volume, newer.name(),
                                                     blob_kind_t::sorted_run_k, 40'000, 22);
        failures += expect(second.has_value());
        if (!second) return failures;
    }

    expected<volume_header_t> const head = fetch_head(fixture.disk, guard->channel(), medium);
    failures += expect(head.has_value());
    if (!head) return failures;
    failures += expect_eq(head->magic, volume_magic_k);

    expected<std::uint64_t> const first_generation = fetch_slot_generation(fixture.disk, guard->channel(), medium,
                                                                           *head, 0);
    expected<std::uint64_t> const second_generation = fetch_slot_generation(fixture.disk, guard->channel(), medium,
                                                                            *head, 1);
    failures += expect(first_generation.has_value() && second_generation.has_value());
    if (!first_generation || !second_generation) return failures;
    failures += expect(*first_generation != *second_generation);
    std::uint32_t const newest = *first_generation > *second_generation ? 0u : 1u;

    failures += expect(tear_slot_records(fixture.disk, guard->channel(), medium, *head, newest).succeeded());

    expected<volume_t> replayed = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
    failures += expect(replayed.has_value());
    if (!replayed) return failures;
    failures += expect_eq(replayed->size(), std::size_t {1});
    expected<blob_t> const kept = replayed->find(older.name());
    failures += expect(kept.has_value());
    failures += expect_status(replayed->find(newer.name()), status_code_t::not_found_k);
    if (!kept) return failures;
    failures += expect(kept->extent == older_extent);

    expected<std::size_t> const mismatches = mismatched_bytes(fixture.disk, guard->channel(), *kept, 21);
    failures += expect(mismatches.has_value());
    if (mismatches) failures += expect_eq(*mismatches, std::size_t {0});
    return failures;
}

/** With neither slot readable a refusing mount touches nothing, and a reformatting one starts over. */
template <typename fixture_type_>
std::size_t volume_refuses_two_dead_slots(fixture_type_& fixture) {
    using guard_t = typename fixture_type_::guard_t;
    using volume_t = typename fixture_type_::volume_t;
    std::size_t failures = 0;
    expected<guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    blob_label_t const label {31};
    medium_reference_t const medium = fixture.media[0].reference();
    std::uint64_t capacity = 0;
    {
        expected<volume_t> volume = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
        failures += expect(volume.has_value());
        if (!volume) return failures;
        capacity = volume->capacity();
        expected<blob_t> const blob = publish_blob(fixture.disk, guard->channel(), *volume, label.name(),
                                                   blob_kind_t::schema_k, 8192, 31);
        failures += expect(blob.has_value());
        if (!blob) return failures;
    }

    expected<volume_header_t> const head = fetch_head(fixture.disk, guard->channel(), medium);
    failures += expect(head.has_value());
    if (!head) return failures;
    failures += expect(erase_slot(fixture.disk, guard->channel(), medium, *head, 0).succeeded());
    failures += expect(erase_slot(fixture.disk, guard->channel(), medium, *head, 1).succeeded());

    expected<volume_t> const refused = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
    failures += expect(!refused.has_value());
    failures += expect_status(refused, status_code_t::malformed_k);

    // A refusal leaves the medium exactly as it found it, so a repair tool still sees the damage.
    for (std::uint32_t slot = 0; slot != 2; ++slot) {
        expected<bool> const untouched = slot_is_erased(fixture.disk, guard->channel(), medium, *head, slot);
        failures += expect(untouched.has_value());
        if (untouched) failures += expect(*untouched);
    }

    expected<volume_t> reformatted = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::reformat_k);
    failures += expect(reformatted.has_value());
    if (!reformatted) return failures;
    failures += expect_eq(reformatted->size(), std::size_t {0});
    failures += expect_status(reformatted->find(label.name()), status_code_t::not_found_k);
    failures += expect_eq(reformatted->free_bytes(), capacity);

    // The repair is durable, so the mount after it no longer needs permission to repair anything.
    expected<volume_t> const settled = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
    failures += expect(settled.has_value());
    if (settled) failures += expect_eq(settled->size(), std::size_t {0});
    return failures;
}

/** One flipped byte of the head is caught by its checksum rather than mounted as geometry. */
template <typename fixture_type_>
std::size_t volume_refuses_a_torn_head(fixture_type_& fixture) {
    using guard_t = typename fixture_type_::guard_t;
    using volume_t = typename fixture_type_::volume_t;
    std::size_t failures = 0;
    expected<guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    medium_reference_t const medium = fixture.media[0].reference();
    blob_label_t const buried {61};
    {
        expected<volume_t> volume = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
        failures += expect(volume.has_value());
        if (!volume) return failures;

        // Both slots end up holding a readable image, so a repair has to outrank the older of them.
        expected<blob_t> const first = publish_blob(fixture.disk, guard->channel(), *volume, buried.name(),
                                                    blob_kind_t::schema_k, 4096, 61);
        failures += expect(first.has_value());
        blob_label_t const second {62};
        expected<blob_t> const later = publish_blob(fixture.disk, guard->channel(), *volume, second.name(),
                                                    blob_kind_t::schema_k, 4096, 62);
        failures += expect(later.has_value());
        if (!first || !later) return failures;
    }

    std::array<byte_t, volume_config_k.page_bytes> page {};
    failures += expect(fetch_page(fixture.disk, guard->channel(), medium, 0, page).succeeded());
    volume_header_t before {};
    std::memcpy(&before, page.data(), sizeof(before));
    failures += expect(header_checksum_holds(before));

    // The magic survives the flip, so only the checksum stands between the head and a bad geometry.
    constexpr std::size_t flipped_k = offsetof(volume_header_t, catalog_slot_offset);
    page[flipped_k] = static_cast<byte_t>(std::to_integer<std::uint8_t>(page[flipped_k]) ^ 0x40u);
    failures += expect(store_page(fixture.disk, guard->channel(), medium, 0, page).succeeded());

    volume_header_t after {};
    std::memcpy(&after, page.data(), sizeof(after));
    failures += expect_eq(after.magic, volume_magic_k);
    failures += expect(!header_checksum_holds(after));

    expected<volume_t> const refused = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
    failures += expect(!refused.has_value());
    failures += expect_status(refused, status_code_t::malformed_k);

    {
        expected<volume_t> repaired = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::reformat_k);
        failures += expect(repaired.has_value());
        if (!repaired) return failures;
        failures += expect_eq(repaired->size(), std::size_t {0});
    }

    // No image of the volume's earlier life outranks the format that replaced it.
    expected<volume_t> const settled = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
    failures += expect(settled.has_value());
    if (!settled) return failures;
    failures += expect_eq(settled->size(), std::size_t {0});
    failures += expect_status(settled->find(buried.name()), status_code_t::not_found_k);
    return failures;
}

#pragma endregion Durability Cases

#pragma region Catalog Cases

/** Every byte a mixed run of blobs took comes back, and the whole region is allocatable again. */
template <typename fixture_type_>
std::size_t volume_reuses_every_byte(fixture_type_& fixture) {
    using guard_t = typename fixture_type_::guard_t;
    using volume_t = typename fixture_type_::volume_t;
    std::size_t failures = 0;
    constexpr std::size_t blobs_k = 70;
    constexpr std::array<std::uint64_t, 7> sizes_k {4096, 12288, 65536, 262144, 1048576, 3000, 700000};
    expected<guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    expected<volume_t> volume = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
    failures += expect(volume.has_value());
    if (!volume) return failures;
    std::uint64_t const capacity = volume->capacity();
    std::uint64_t const free_before = volume->free_bytes();
    failures += expect_eq(free_before, capacity);

    for (std::size_t index = 0; index != blobs_k; ++index) {
        blob_label_t const label {index};
        expected<blob_t> const blob = volume->create(label.name(), blob_kind_t::sorted_run_k, sizes_k[index % 7]);
        failures += expect(blob.has_value());
        if (!blob) return failures;
        failures += expect(volume->seal(guard->channel(), *blob).succeeded());
    }
    failures += expect_eq(volume->size(), blobs_k);
    failures += expect(volume->free_bytes() < free_before);

    // Dropped out of order, so coalescing has to join ranges on both sides rather than only ahead.
    for (std::size_t step = 0; step != blobs_k; ++step) {
        blob_label_t const label {(step * 37) % blobs_k};
        failures += expect(volume->drop(guard->channel(), label.name()).succeeded());
    }
    failures += expect_eq(volume->size(), std::size_t {0});
    failures += expect_eq(volume->free_bytes(), free_before);
    failures += expect_eq(volume->largest_free_bytes(), capacity);

    blob_label_t const whole {999};
    expected<blob_t> const spanning = volume->create(whole.name(), blob_kind_t::user_data_k, capacity);
    failures += expect(spanning.has_value());
    if (!spanning) return failures;
    failures += expect_eq(spanning->extent.length, capacity);
    failures += expect_eq(volume->free_bytes(), std::uint64_t {0});
    return failures;
}

/** A name the volume already holds is refused, and a name it never held is not found. */
template <typename fixture_type_>
std::size_t volume_refuses_a_duplicate(fixture_type_& fixture) {
    using guard_t = typename fixture_type_::guard_t;
    using volume_t = typename fixture_type_::volume_t;
    std::size_t failures = 0;
    expected<guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    expected<volume_t> volume = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
    failures += expect(volume.has_value());
    if (!volume) return failures;

    blob_label_t const held {41};
    blob_label_t const absent {42};
    expected<blob_t> const reserved = volume->create(held.name(), blob_kind_t::sorted_run_k, 8192);
    failures += expect(reserved.has_value());
    if (!reserved) return failures;

    // A reservation claims the name before it is durable, so the second caller is refused at once.
    expected<blob_t> const twice = volume->create(held.name(), blob_kind_t::sorted_run_k, 8192);
    failures += expect(!twice.has_value());
    failures += expect_status(twice, status_code_t::already_exists_k);

    // A seal names the reservation it belongs to, so a range this volume never handed out is refused.
    blob_t stranger = *reserved;
    stranger.extent = extent_t {reserved->extent.end(), volume_config_k.page_bytes};
    failures += expect_status(volume->seal(guard->channel(), stranger), status_code_t::not_found_k);
    stranger.extent = reserved->extent;
    stranger.medium.generation = static_cast<std::uint16_t>(stranger.medium.generation + 1);
    failures += expect_status(volume->seal(guard->channel(), stranger), status_code_t::invalid_argument_k);

    failures += expect(volume->seal(guard->channel(), *reserved).succeeded());
    expected<blob_t> const after_seal = volume->create(held.name(), blob_kind_t::sorted_run_k, 8192);
    failures += expect(!after_seal.has_value());
    failures += expect_status(after_seal, status_code_t::already_exists_k);

    failures += expect_status(volume->find(absent.name()), status_code_t::not_found_k);
    failures += expect_status(volume->drop(guard->channel(), absent.name()), status_code_t::not_found_k);

    // An empty name and one past the format's limit are arguments, never names.
    failures += expect(volume->create(blob_name_t {}, blob_kind_t::sorted_run_k, 8192) ==
                       status_code_t::invalid_argument_k);
    return failures;
}

/** A walk of one kind sees that kind alone, across a catalog holding all four of them. */
template <typename fixture_type_>
std::size_t volume_walks_one_kind(fixture_type_& fixture) {
    using guard_t = typename fixture_type_::guard_t;
    using volume_t = typename fixture_type_::volume_t;
    std::size_t failures = 0;
    constexpr std::array<blob_kind_t, 4> kinds_k {blob_kind_t::sorted_run_k, blob_kind_t::schema_k,
                                                  blob_kind_t::backup_schema_k, blob_kind_t::user_data_k};
    constexpr std::array<std::size_t, 4> counts_k {3, 2, 1, 4};
    expected<guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    expected<volume_t> volume = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
    failures += expect(volume.has_value());
    if (!volume) return failures;

    std::size_t minted = 0;
    for (std::size_t kind = 0; kind != kinds_k.size(); ++kind)
        for (std::size_t index = 0; index != counts_k[kind]; ++index) {
            blob_label_t const label {50 + minted};
            expected<blob_t> const blob = volume->create(label.name(), kinds_k[kind], 4096);
            failures += expect(blob.has_value());
            if (!blob) return failures;
            failures += expect(volume->seal(guard->channel(), *blob).succeeded());
            ++minted;
        }
    failures += expect_eq(volume->size(), minted);

    for (std::size_t kind = 0; kind != kinds_k.size(); ++kind) {
        std::size_t seen = 0;
        std::size_t strays = 0;
        volume->for_each(kinds_k[kind], [&](blob_name_t name, blob_t const& blob) noexcept {
            ++seen;
            expected<blob_t> const resolved = volume->find(name);
            if (!resolved || !(resolved->extent == blob.extent)) ++strays;
        });
        failures += expect_eq(seen, counts_k[kind]);
        failures += expect_eq(strays, std::size_t {0});
    }
    return failures;
}

/** A catalog that no longer fits its slot is refused cleanly, and the slot it did not write still reads. */
template <typename fixture_type_>
std::size_t volume_refuses_an_overgrown_catalog(fixture_type_& fixture) {
    using guard_t = typename fixture_type_::guard_t;
    using volume_t = typename fixture_type_::volume_t;
    std::size_t failures = 0;
    expected<guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    std::size_t sealed = 0;
    {
        expected<volume_t> volume = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
        failures += expect(volume.has_value());
        if (!volume) return failures;

        status_t refusal;
        for (std::size_t index = 0; index != 4096 && refusal.succeeded(); ++index) {
            blob_label_t const label {index, static_cast<std::uint16_t>(blob_name_max_bytes_k)};
            expected<blob_t> const blob = volume->create(label.name(), blob_kind_t::sorted_run_k,
                                                         volume_config_k.page_bytes);
            failures += expect(blob.has_value());
            if (!blob) return failures;
            refusal = volume->seal(guard->channel(), *blob);
            if (refusal.succeeded()) ++sealed;
        }
        failures += expect(refusal.failed());
        failures += expect_status(refusal, status_code_t::capacity_exhausted_k);
        failures += expect(sealed > 64);
        failures += expect_eq(volume->size(), sealed);
    }

    expected<volume_t> replayed = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
    failures += expect(replayed.has_value());
    if (!replayed) return failures;
    failures += expect_eq(replayed->size(), sealed);
    blob_label_t const first {0, static_cast<std::uint16_t>(blob_name_max_bytes_k)};
    failures += expect(replayed->find(first.name()).has_value());

    // Neither image ever reached past its own slot, so both of them still carry a generation.
    expected<volume_header_t> const head = fetch_head(fixture.disk, guard->channel(), fixture.media[0].reference());
    failures += expect(head.has_value());
    if (!head) return failures;
    std::array<std::uint64_t, 2> generations {};
    for (std::uint32_t slot = 0; slot != 2; ++slot) {
        expected<std::uint64_t> const generation = fetch_slot_generation(fixture.disk, guard->channel(),
                                                                         fixture.media[0].reference(), *head, slot);
        failures += expect(generation.has_value());
        if (!generation) return failures;
        generations[slot] = *generation;
    }
    failures += expect(generations[0] != 0 && generations[1] != 0);
    std::uint64_t const gap = generations[0] > generations[1] ? generations[0] - generations[1]
                                                              : generations[1] - generations[0];
    failures += expect_eq(gap, std::uint64_t {1});
    return failures;
}

/**
 *  @brief A mount reads its slots through a streaming block, leaving the blocks a caller holds alone.
 *
 *  A catalog slot is far longer than one request block, which is the size a read pipeline's own pool
 *  hands out. Every request block of the channel is lent out and painted here bar one, so a mount that
 *  read a slot into that one block would run the tail of the image straight over its neighbour.
 */
template <typename fixture_type_>
std::size_t volume_mount_spares_held_blocks(fixture_type_& fixture) {
    using guard_t = typename fixture_type_::guard_t;
    using volume_t = typename fixture_type_::volume_t;
    std::size_t failures = 0;
    constexpr std::size_t blobs_k = 300;
    constexpr std::uint32_t lent_back_k = 5;
    expected<guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    {
        expected<volume_t> volume = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
        failures += expect(volume.has_value());
        if (!volume) return failures;
        for (std::size_t index = 0; index != blobs_k; ++index) {
            blob_label_t const label {index, static_cast<std::uint16_t>(blob_name_max_bytes_k)};
            expected<blob_t> const blob = volume->create(label.name(), blob_kind_t::sorted_run_k,
                                                         volume_config_k.page_bytes);
            failures += expect(blob.has_value());
            if (!blob) return failures;
            failures += expect(volume->seal(guard->channel(), *blob).succeeded());
        }
    }

    buffer_pool_t& pool = guard->channel().requests();
    std::uint32_t const blocks = pool.total_buffers();
    st::basic_vector<pooled_buffer_t> held;
    for (std::uint32_t index = 0; index != blocks; ++index) {
        expected<pooled_buffer_t> block = pool.acquire();
        failures += expect(block.has_value());
        if (!block) return failures;
        std::span<byte_t> const bytes = block->bytes();
        for (std::size_t position = 0; position != bytes.size(); ++position) bytes[position] = painted(index, position);
        failures += expect(held.push_back(std::move(*block)) == st::status_t::success_k);
    }
    failures += expect_eq(pool.available_buffers(), std::uint32_t {0});
    held[lent_back_k].release();

    expected<volume_t> replayed = fixture.mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
    failures += expect(replayed.has_value());
    if (!replayed) return failures;
    failures += expect_eq(replayed->size(), blobs_k);

    std::size_t scribbled = 0;
    for (std::uint32_t index = 0; index != blocks; ++index) {
        if (index == lent_back_k) continue;
        std::span<byte_t const> const bytes = held[index].bytes();
        for (std::size_t position = 0; position != bytes.size(); ++position)
            if (bytes[position] != painted(index, position)) ++scribbled;
    }
    failures += expect_eq(scribbled, std::size_t {0});
    return failures;
}

#pragma endregion Catalog Cases

#pragma region Router Cases

/** Placement follows free space, resolution crosses every volume, and a drop frees the right one. */
template <typename fixture_type_>
std::size_t router_places_by_free_space(fixture_type_& fixture) {
    using guard_t = typename fixture_type_::guard_t;
    using volume_t = typename fixture_type_::volume_t;
    using router_t = basic_volume_router<typename fixture_type_::volume_t>;
    std::size_t failures = 0;
    constexpr std::size_t volumes_k = 3;
    constexpr std::size_t blobs_k = 100;
    constexpr std::uint64_t spread_bytes_k = 384ull * 1024ull;
    constexpr std::uint64_t only_largest_bytes_k = 20ull * 1024ull * 1024ull;
    expected<guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    expected<router_t> router = router_t::make(volumes_k);
    failures += expect(router.has_value());
    if (!router) return failures;
    for (std::size_t index = 0; index != volumes_k; ++index) {
        expected<volume_t> volume = fixture.mount_volume(guard->channel(), index, volume_recovery_t::refuse_k);
        failures += expect(volume.has_value());
        if (!volume) return failures;
        failures += expect(router->bind_queue_pair(std::move(*volume)).succeeded());
    }
    failures += expect_eq(router->size(), volumes_k);
    failures += expect(router->volume(static_cast<volume_id_t>(0)).capacity() <
                       router->volume(static_cast<volume_id_t>(2)).capacity());

    // Only the largest volume has a range that long, so placement has no choice to make.
    expected<volume_id_t> const forced = router->optimal_volume(only_largest_bytes_k);
    failures += expect(forced.has_value());
    if (!forced) return failures;
    failures += expect_eq(static_cast<std::uint16_t>(*forced), std::uint16_t {2});

    blob_label_t const wide {1001};
    expected<blob_t> const widest = router->create(wide.name(), blob_kind_t::sorted_run_k, only_largest_bytes_k);
    failures += expect(widest.has_value());
    if (!widest) return failures;
    failures += expect_eq(static_cast<std::uint16_t>(widest->volume), std::uint16_t {2});
    failures += expect(router->volume(widest->volume).seal(guard->channel(), *widest).succeeded());
    expected<blob_t> const resolved = router->find(wide.name());
    failures += expect(resolved.has_value());
    if (resolved) failures += expect_eq(static_cast<std::uint16_t>(resolved->volume), std::uint16_t {2});
    failures += expect(router->drop(guard->channel(), wide.name()).succeeded());
    failures += expect_status(router->find(wide.name()), status_code_t::not_found_k);

    // Nothing fits when every volume is asked for more than the largest of them holds.
    failures += expect(router->optimal_volume(64ull * 1024ull * 1024ull) == status_code_t::capacity_exhausted_k);

    std::array<std::size_t, volumes_k> placed {};
    std::array<blob_label_t, volumes_k> last {blob_label_t {0}, blob_label_t {0}, blob_label_t {0}};
    for (std::size_t index = 0; index != blobs_k; ++index) {
        blob_label_t const label {2000 + index};
        expected<blob_t> const blob = router->create(label.name(), blob_kind_t::user_data_k, spread_bytes_k);
        failures += expect(blob.has_value());
        if (!blob) return failures;
        std::size_t const volume = static_cast<std::size_t>(blob->volume);
        failures += expect(volume < volumes_k);
        if (volume >= volumes_k) return failures;
        failures += expect(router->volume(blob->volume).seal(guard->channel(), *blob).succeeded());
        ++placed[volume];
        last[volume] = label;
    }

    std::size_t total = 0;
    for (std::size_t index = 0; index != volumes_k; ++index) {
        failures += expect(placed[index] != 0);
        failures += expect_eq(router->volume(static_cast<volume_id_t>(index)).size(), placed[index]);
        total += placed[index];
    }
    failures += expect_eq(total, blobs_k);

    // A blob of the third volume resolves there, and dropping it frees that volume and no other.
    blob_label_t const doomed = last[2];
    expected<blob_t> const located = router->find(doomed.name());
    failures += expect(located.has_value());
    if (!located) return failures;
    failures += expect_eq(static_cast<std::uint16_t>(located->volume), std::uint16_t {2});

    std::array<std::uint64_t, volumes_k> before {};
    for (std::size_t index = 0; index != volumes_k; ++index)
        before[index] = router->volume(static_cast<volume_id_t>(index)).free_bytes();
    failures += expect(router->drop(guard->channel(), doomed.name()).succeeded());
    for (std::size_t index = 0; index != volumes_k; ++index) {
        std::uint64_t const after = router->volume(static_cast<volume_id_t>(index)).free_bytes();
        if (index == 2) failures += expect_eq(after, before[index] + spread_bytes_k);
        else failures += expect_eq(after, before[index]);
    }
    failures += expect_status(router->find(doomed.name()), status_code_t::not_found_k);
    failures += expect_status(router->drop(guard->channel(), doomed.name()), status_code_t::not_found_k);

    // A walk of the router sees every volume's entries of that kind and stamps each with its own.
    std::size_t walked = 0;
    std::size_t strays = 0;
    router->for_each(blob_kind_t::user_data_k, [&](blob_name_t name, blob_t const& blob) noexcept {
        ++walked;
        expected<blob_t> const found = router->find(name);
        if (!found || found->volume != blob.volume) ++strays;
    });
    failures += expect_eq(walked, blobs_k - 1);
    failures += expect_eq(strays, std::size_t {0});
    return failures;
}

#pragma endregion Router Cases

#pragma region Drivers

/** Mounts one volume through @p mount and runs @p body against the fixture, so every case starts on a blank
 *  environment. */
template <typename mount_type_, typename body_type_>
std::size_t on_one_volume(mount_type_ const& mount, body_type_&& body) {
    expected<typename mount_type_::fixture_unique_ptr_t> fixture = mount();
    std::size_t failures = expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;
    return failures + body(**fixture);
}

/** Runs the router case against three volumes of deliberately different sizes. */
template <typename mount_type_>
case_result_t on_three_volumes(mount_type_ const& mount) {
    if (char const* const reason = mount.refusal(3)) return case_result_t::skipped(reason);
    std::array<std::uint64_t, 3> const capacities {8ull * 1024 * 1024, 16ull * 1024 * 1024,
                                                   volume_config_k.volume_bytes};
    expected<typename mount_type_::fixture_unique_ptr_t> fixture = mount(capacities);
    std::size_t failures = expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;
    return failures + router_places_by_free_space(**fixture);
}

#pragma endregion Drivers

} // namespace unum::ustore::test

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;
    using namespace unum::ustore::nvme;

    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();
    st::expected<test_directory_t> const directory = test_directory_t::make(options.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    test_environment_t const environment = test_environment_t::of(options, *directory);

    auto const cases = [&](auto const& mount) {
        auto const on_one = [&](char const* name, auto&& body) {
            case_label_t const label = mount.label(name);
            if (totals.selects(label.c_str()))
                report(totals, label.c_str(), on_one_volume(mount, body) + mount.take_leaks());
        };
        on_one("round_trip", [](auto& fixture) { return volume_round_trips(fixture); });
        on_one("crash_before_seal", [](auto& fixture) { return volume_forgets_an_unsealed_blob(fixture); });
        on_one("crash_after_seal", [](auto& fixture) { return volume_keeps_a_sealed_blob(fixture); });
        on_one("torn_newer_slot", [](auto& fixture) { return volume_survives_a_torn_slot(fixture); });
        on_one("two_dead_slots", [](auto& fixture) { return volume_refuses_two_dead_slots(fixture); });
        on_one("torn_head", [](auto& fixture) { return volume_refuses_a_torn_head(fixture); });
        on_one("reuses_every_byte", [](auto& fixture) { return volume_reuses_every_byte(fixture); });
        on_one("duplicate_and_absent", [](auto& fixture) { return volume_refuses_a_duplicate(fixture); });
        on_one("walks_one_kind", [](auto& fixture) { return volume_walks_one_kind(fixture); });
        on_one("overgrown_catalog", [](auto& fixture) { return volume_refuses_an_overgrown_catalog(fixture); });
        on_one("mount_spares_blocks", [](auto& fixture) { return volume_mount_spares_held_blocks(fixture); });
        report(totals, mount, "router_places_by_free_space", on_three_volumes);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, volume_config_k, 1, "volume", cases);

    return suite_exit_code(totals);
}
