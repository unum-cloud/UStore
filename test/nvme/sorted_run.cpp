/**
 *  @file test/nvme/sorted_run.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The sorted run over the ring: what it writes, what it refuses to open, and how deep it reads.
 *
 *  The result this file exists for is @c truncated_index: a run whose index does not read whole is
 *  a truncated run and must answer @c malformed_k rather than attaching over a half-filled index,
 *  which is the failure a mapped index could never report. The rest holds the layout to a
 *  byte-for-byte round trip at both ends of the value sizes it admits, and every section of the
 *  blob to its page boundary.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <array>   // `std::array`
#include <memory>  // `std::unique_ptr`
#include <span>    // `std::span`
#include <string>  // `std::string`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/nvme_channels.hpp" // `basic_read_pipeline`, `basic_write_pipeline`
#include "ustore/blobs/nvme_key_slot.hpp"     // `key_slot`
#include "ustore/blobs/nvme_sorted_run.hpp"   // `lsm_sorted_run`, `sorted_run_layout`
#include "ustore/strings/nvme_store.hpp"      // `head_of`, `tail_of`

#include "nvme/fixture.hpp" // `run_on_transport`, `painted`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Shape

/** The numbers every case mounts with: a streaming block of a megabyte, so one carries a whole index. */
constexpr fixture_config_t run_config_k {
    .stream_buffer_bytes = 1024ull * 1024ull,
    .stream_buffers = 2,
    .volume_bytes = 64ull * 1024ull * 1024ull,
    .shared_io_channels = 2,
    .memory_limit = std::size_t {128} * 1024 * 1024,
};

/** Bytes of the stub medium a truncated index runs off the end of. */
constexpr std::uint64_t stub_bytes_k = run_config_k.page_bytes * 3;

/** The keys and the key slots every case of this suite drives. */
using stored_key_t = std::uint64_t;
using variable_slot_t = key_slot<stored_key_t, value_layout_t::variable_width_k>;
using fixed_slot_t = key_slot<stored_key_t, value_layout_t::fixed_width_k>;
using inline_slot_t = key_slot<stored_key_t, value_layout_t::inline_12_bytes_k>;
using variable_layout_t = sorted_run_layout<variable_slot_t>;
using fixed_layout_t = sorted_run_layout<fixed_slot_t>;

/** The runs and the pipelines every case drives over the volume of @p fixture_type_. */
template <typename fixture_type_>
struct run_types {
    using fixture_t = fixture_type_;
    using guard_t = typename fixture_t::guard_t;
    using disk_t = typename fixture_t::disk_t;
    using volume_t = typename fixture_t::volume_t;
    using variable_run_t = lsm_sorted_run<variable_slot_t, volume_t, waiter_t>;
    using fixed_run_t = lsm_sorted_run<fixed_slot_t, volume_t, waiter_t>;
    using inline_run_t = lsm_sorted_run<inline_slot_t, volume_t, waiter_t>;
    using read_pipeline_t = basic_read_pipeline<disk_t>;
    using write_pipeline_t = basic_write_pipeline<disk_t, waiter_t>;
};

/** The key the key slot at @p index carries, kept odd so every even key is a miss. */
[[nodiscard]] constexpr stored_key_t key_of(std::size_t index) noexcept {
    return static_cast<stored_key_t>(index) * 2 + 1;
}

#pragma endregion Shape

#pragma region Builders

/** One stamp per key slot of a run of @p count distinct keys, the commits that wrote them in key order. */
inline st::vector<std::uint64_t> ascending_stamps(std::size_t count) {
    st::vector<std::uint64_t> stamps;
    [[maybe_unused]] st::status_t const sized = stamps.resize(count);
    for (std::size_t index = 0; index != stamps.size(); ++index) stamps[index] = index + 1;
    return stamps;
}

/** One kind per key slot of a run of @p count writes. */
inline st::vector<std::uint8_t> written_kinds(std::size_t count) {
    st::vector<std::uint8_t> kinds;
    [[maybe_unused]] st::status_t const sized = kinds.resize(count);
    for (std::uint8_t& kind : kinds) kind = static_cast<std::uint8_t>(delta_kind_t::write_k);
    return kinds;
}

/** The key slots and the bytes behind them that one variable-width case writes. */
struct variable_data_t {

    /** Key slots in ascending order of their keys. */
    st::vector<variable_slot_t> key_slots;

    /** The values back to back, in the order of their key slots. */
    st::vector<byte_t> values;

    /** Where each value starts inside @c values and one past the last, as the run's value offsets column. */
    st::vector<stored_value_offset_t> offsets;

    /** Bytes the value of every key slot holds, which a read is checked against. */
    st::vector<std::uint64_t> sizes;

    /** The @c delta_kind_t of every key slot. */
    st::vector<std::uint8_t> kinds;
};

/**
 *  @brief Lays out @p count variable-width key slots whose value sizes come from @p sized.
 *  @param[in] sized Called as @c sized(index); a negative answer is a removal and zero an empty value.
 */
template <typename sized_type_>
[[nodiscard]] std::size_t build_variable(std::size_t count, sized_type_&& sized, variable_data_t& into) {
    std::size_t failures = 0;
    failures += expect(into.sizes.resize(count) == st::status_t::success_k);
    failures += expect(into.key_slots.resize(count) == st::status_t::success_k);
    failures += expect(into.kinds.resize(count) == st::status_t::success_k);
    failures += expect(into.offsets.resize(count + 1) == st::status_t::success_k);
    if (failures != 0) return failures;

    std::uint64_t total = 0;
    for (std::size_t index = 0; index != count; ++index) {
        std::int64_t const wanted = sized(index);
        into.sizes[index] = wanted > 0 ? static_cast<std::uint64_t>(wanted) : 0;
        total += into.sizes[index];
    }
    failures += expect(into.values.resize(static_cast<std::size_t>(total)) == st::status_t::success_k);
    if (failures != 0) return failures;

    std::uint64_t written = 0;
    for (std::size_t index = 0; index != count; ++index) {
        variable_slot_t slot;
        slot.stored_key = key_of(index);
        into.offsets[index] = static_cast<stored_value_offset_t>(written);
        into.kinds[index] = static_cast<std::uint8_t>(sized(index) < 0 ? delta_kind_t::remove_k
                                                                       : delta_kind_t::write_k);
        for (std::uint64_t byte = 0; byte != into.sizes[index]; ++byte)
            into.values[static_cast<std::size_t>(written + byte)] = painted(index, byte);
        written += into.sizes[index];
        into.key_slots[index] = slot;
    }
    into.offsets[count] = static_cast<stored_value_offset_t>(written);
    return failures;
}

/** Reads every value of @p run at every position, counting the bytes that do not match what was written. */
template <typename run_type_>
[[nodiscard]] std::size_t check_variable_values(run_type_ const& run, io_channel_t& channel,
                                                variable_data_t const& data) {
    std::size_t failures = 0;
    std::size_t const count = data.sizes.size();
    st::vector<std::uint64_t> seen;
    failures += expect(seen.resize(count) == st::status_t::success_k);
    st::vector<std::uint64_t> positions;
    failures += expect(positions.resize(count) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count; ++index) {
        seen[index] = 0;
        positions[index] = index;
    }

    std::size_t mismatches = 0;
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        seen[request] += bytes.size();
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(request, offset + index)) ++mismatches;
    };
    failures += expect(run.read_values(channel, std::span<std::uint64_t const> {positions}, sink).succeeded());
    failures += expect_eq(mismatches, std::size_t {0});
    for (std::size_t index = 0; index != count; ++index) failures += expect_eq(seen[index], data.sizes[index]);
    return failures;
}

#pragma endregion Builders

#pragma region Round Trip Cases

/** A run of many values comes back byte for byte, through a volume mounted again from the environment. */
template <typename mount_type_>
case_result_t run_reads_back_every_value(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using variable_run_t = typename types_t::variable_run_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 1500;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;
    if (!(*fixture)->persists_volumes()) return case_result_t::skipped(memory_volumes_vanish_k);

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    uuid_t const content_id = uuid_generator();
    variable_data_t data;
    failures += build_variable(
        count_k, [](std::size_t index) { return 1 + static_cast<std::int64_t>(index * 37 % 4096); }, data);
    if (failures != 0) return failures;

    {
        expected<volume_t> mounted = (*fixture)->mount_volume(guard->channel());
        failures += expect(mounted.has_value());
        if (!mounted) return failures;

        expected<variable_run_t> written = variable_run_t::write(
            *mounted, guard->channel(), uuid_t {}, content_id, std::span<variable_slot_t const> {data.key_slots},
            std::span<std::uint64_t const> {ascending_stamps(data.key_slots.size())},
            std::span<std::uint8_t const> {data.kinds}, 0, std::span<byte_t const> {data.values},
            std::span<stored_value_offset_t const> {data.offsets}, 4096);
        failures += expect(written.has_value());
        if (!written) return failures;
        failures += expect_eq(written->population_count(), static_cast<std::uint64_t>(count_k));
        failures += expect_eq(written->lower_stored_key(), key_of(0));
        failures += expect_eq(written->upper_stored_key(), key_of(count_k - 1));
        failures += check_variable_values(*written, guard->channel(), data);
        manifest_file_t const files[] {file_of(written->extent(), content_id)};
        failures += expect(publish_naming(*mounted, **fixture, guard->channel(), files).succeeded());
    }

    // A run is only really written once a volume mounted again from the manifest finds it.
    expected<volume_t> remounted = (*fixture)->mount_volume(guard->channel());
    failures += expect(remounted.has_value());
    if (!remounted) return failures;
    failures += expect_eq(remounted->manifest().files.size(), std::size_t {1});
    if (remounted->manifest().files.size() != 1) return failures;
    expected<medium_extent_t> found = remounted->open(remounted->manifest().files[0]);
    failures += expect(found.has_value());
    if (!found) return failures;

    expected<variable_run_t> opened = variable_run_t::open(*remounted, guard->channel(), *found);
    failures += expect(opened.has_value());
    if (!opened) return failures;
    failures += expect_eq(opened->population_count(), static_cast<std::uint64_t>(count_k));
    failures += expect(opened->content_id() == content_id);
    failures += check_variable_values(*opened, guard->channel(), data);
    return failures;
}

/** A fixed-width run locates every value at one width, and reads them all back. */
template <typename mount_type_>
std::size_t fixed_width_run_reads_back(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using fixed_run_t = typename types_t::fixed_run_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 700;
    constexpr stored_value_size_t value_size_k = 512;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    st::vector<fixed_slot_t> key_slots;
    st::vector<byte_t> values;
    failures += expect(key_slots.resize(count_k) == st::status_t::success_k);
    failures += expect(values.resize(count_k * value_size_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    // Every seventh key is a removal, whose stride the run still spends and leaves zeroed.
    st::vector<std::uint8_t> kinds = written_kinds(count_k);
    for (std::size_t index = 0; index != count_k; ++index) {
        fixed_slot_t slot;
        slot.stored_key = key_of(index);
        key_slots[index] = slot;
        bool const removed = index % 7 == 3;
        if (removed) kinds[index] = static_cast<std::uint8_t>(delta_kind_t::remove_k);
        for (std::size_t byte = 0; byte != value_size_k; ++byte)
            values[index * value_size_k + byte] = removed ? byte_t {0} : painted(index, byte);
    }

    expected<fixed_run_t> written = fixed_run_t::write(
        *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<fixed_slot_t const> {key_slots},
        std::span<std::uint64_t const> {ascending_stamps(key_slots.size())}, std::span<std::uint8_t const> {kinds}, 0,
        std::span<byte_t const> {values}, {}, value_size_k);
    failures += expect(written.has_value());
    if (!written) return failures;

    // A fixed-width run takes exactly one stride per key slot, and finds each value by stride from its position.
    expected<fixed_run_t> short_values = fixed_run_t::write(
        *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<fixed_slot_t const> {key_slots},
        std::span<std::uint64_t const> {ascending_stamps(key_slots.size())}, std::span<std::uint8_t const> {kinds}, 0,
        std::span<byte_t const> {values}.first(values.size() - 1), {}, value_size_k);
    failures += expect_status(short_values, status_code_t::invalid_argument_k);
    std::uint64_t const values_begin = written->extent().extent.offset + fixed_layout_t::values_offset(count_k);
    for (std::size_t index = 0; index != count_k; ++index) {
        value_window_t const window = written->value_window(index);
        if (index % 7 == 3) {
            failures += expect_eq(window.extent.length, std::uint64_t {0});
            continue;
        }
        failures += expect_eq(window.extent.offset + window.skipped, values_begin + index * value_size_k);
        failures += expect_eq(window.bytes, std::uint32_t {value_size_k});
    }

    st::vector<std::uint64_t> positions;
    failures += expect(positions.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) positions[index] = index;

    std::size_t mismatches = 0;
    st::vector<std::uint64_t> seen;
    failures += expect(seen.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) seen[index] = 0;
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        seen[request] += bytes.size();
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(request, offset + index)) ++mismatches;
    };
    failures += expect(
        written->read_values(guard->channel(), std::span<std::uint64_t const> {positions}, sink).succeeded());
    failures += expect_eq(mismatches, std::size_t {0});
    for (std::size_t index = 0; index != count_k; ++index)
        failures += expect_eq(seen[index], static_cast<std::uint64_t>(index % 7 == 3 ? 0 : value_size_k));
    return failures;
}

/** An inline run carries its values in its key slots, so reading them moves no bytes at all. */
template <typename mount_type_>
std::size_t inline_run_needs_no_reads(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using inline_run_t = typename types_t::inline_run_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 300;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    st::vector<inline_slot_t> key_slots;
    failures += expect(key_slots.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) {
        inline_slot_t slot;
        slot.stored_key = key_of(index);
        slot.size = 8;
        for (std::size_t byte = 0; byte != 8; ++byte) slot.data[byte] = painted(index, byte);
        key_slots[index] = slot;
    }

    expected<inline_run_t> written = inline_run_t::write(
        *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<inline_slot_t const> {key_slots},
        std::span<std::uint64_t const> {ascending_stamps(key_slots.size())},
        std::span<std::uint8_t const> {written_kinds(key_slots.size())}, 0, std::span<byte_t const> {}, {}, 8);
    failures += expect(written.has_value());
    if (!written) return failures;

    st::vector<std::uint64_t> positions;
    failures += expect(positions.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) positions[index] = index;

    std::size_t mismatches = 0;
    std::size_t seen = 0;
    auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
        ++seen;
        if (bytes.size() != 8) ++mismatches;
        for (std::size_t index = 0; index != bytes.size(); ++index)
            if (bytes[index] != painted(request, offset + index)) ++mismatches;
    };
    std::size_t const before = (*fixture)->disk.outstanding(guard->channel().queue_pair());
    failures += expect(
        written->read_values(guard->channel(), std::span<std::uint64_t const> {positions}, sink).succeeded());
    failures += expect_eq(seen, count_k);
    failures += expect_eq(mismatches, std::size_t {0});
    failures += expect_eq((*fixture)->disk.outstanding(guard->channel().queue_pair()), before);

    // An inline run carries no values section, so its blob ends where its index does.
    failures += expect_eq(written->size_bytes(),
                          sorted_run_layout<inline_slot_t>::blob_bytes(count_k, 0, written->stamps().saved_bytes(),
                                                                       written->kinds().saved_bytes()));
    return failures;
}

#pragma endregion Round Trip Cases

#pragma region Layout Cases

/** Reads @p length bytes from @p offset of @p blob into @p into, a page-aligned range a block at a time. */
template <typename disk_type_>
status_t read_raw(disk_type_& disk, io_channel_t& channel, medium_extent_t const& blob, std::uint64_t offset,
                  std::uint64_t length, st::vector<byte_t>& into) noexcept {
    if (into.resize(static_cast<std::size_t>(length)) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "No room for the raw bytes"};
    std::uint64_t const block = round_down_to_multiple(channel.requests().buffer_bytes(), portable_alignment_k);
    basic_read_pipeline<disk_type_> reader {disk, channel, blob.medium};
    status_t moved;
    auto sink = [&](completed_buffer_t const& completed) noexcept {
        if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
            moved = whole;
            return;
        }
        std::memcpy(into.data() + std::uint64_t {completed.tag} * block, completed.bytes.data(),
                    completed.bytes.size());
    };
    request_tag_t tag = 0;
    for (std::uint64_t read = 0; read != length;) {
        while (read != length && reader.can_enqueue()) {
            std::uint64_t const chunk = block < length - read ? block : length - read;
            if (status_t const offered = reader.enqueue_read(blob.slice(offset + read, chunk), tag++); offered.failed())
                return offered;
            read += chunk;
        }
        if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
        if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
    }
    return moved;
}

/** Every section of a variable-width run starts on a page boundary and holds on disk what the run was given, and every
 *  value window and tape window lands where a prefix sum of the sizes says, at the counts around a row of cells and a
 *  page of offsets. */
template <typename mount_type_>
std::size_t sections_sit_on_page_boundaries(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using variable_run_t = typename types_t::variable_run_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;
    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    // A page of offsets holds 1024 of them, so 1023 key slots fill it with the one past the last and 1024 spill.
    std::array<std::size_t, 7> const counts {0, 1, 15, 16, 1023, 1024, 1025};
    for (std::size_t const count : counts) {
        variable_data_t data;
        failures += build_variable(
            count,
            [](std::size_t index) -> std::int64_t {
                if (index % 11 == 5) return -1;
                return static_cast<std::int64_t>(index * 37 % 300);
            },
            data);
        if (failures != 0) return failures;
        expected<variable_run_t> run = variable_run_t::write(
            *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<variable_slot_t const> {data.key_slots},
            std::span<std::uint64_t const> {ascending_stamps(count)}, std::span<std::uint8_t const> {data.kinds}, 0,
            std::span<byte_t const> {data.values}, std::span<stored_value_offset_t const> {data.offsets}, 4096);
        failures += expect(run.has_value());
        if (!run) return failures;

        std::uint64_t const keys_section = variable_layout_t::keys_offset(count);
        std::uint64_t const offsets_section = variable_layout_t::value_offsets_offset(count);
        std::uint64_t const values_section = variable_layout_t::values_offset(count);
        std::uint64_t const trailer_section = variable_layout_t::stamps_offset(count, data.values.size());
        failures += expect_eq(keys_section,
                              variable_layout_t::header_bytes() +
                                  round_up_to_multiple(count * sizeof(variable_slot_t), portable_alignment_k));
        failures += expect_eq(offsets_section - keys_section,
                              round_up_to_multiple(variable_layout_t::keys_saved_bytes(count), portable_alignment_k));
        failures += expect_eq(values_section - offsets_section,
                              round_up_to_multiple((count + 1) * 4, portable_alignment_k));
        failures += expect_eq(trailer_section - values_section,
                              round_up_to_multiple(data.values.size(), portable_alignment_k));
        failures += expect_eq(offsets_section % portable_alignment_k, std::uint64_t {0});
        failures += expect_eq(values_section % portable_alignment_k, std::uint64_t {0});
        failures += expect_eq(trailer_section % portable_alignment_k, std::uint64_t {0});

        // The header, the offsets and the values read raw off the blob as the run was handed them.
        st::vector<byte_t> stored_bytes;
        failures += expect(
            read_raw((*fixture)->disk, guard->channel(), run->extent(), 0, values_section, stored_bytes).succeeded());
        if (failures != 0) return failures;
        sorted_run_header_t header {};
        std::memcpy(&header, stored_bytes.data(), sizeof(header));
        failures += expect_eq(header.format_version, std::uint32_t {1});
        failures += expect_eq(header.key_slot_bytes, std::uint32_t {sizeof(variable_slot_t)});
        failures += expect_eq(header.stored_values_bytes, std::uint64_t {data.values.size()});
        std::size_t wrong_offsets = 0;
        for (std::size_t index = 0; index <= count; ++index) {
            stored_value_offset_t offset = 0;
            std::memcpy(&offset, stored_bytes.data() + offsets_section + index * sizeof(offset), sizeof(offset));
            wrong_offsets += offset != data.offsets[index];
        }
        failures += expect_eq(wrong_offsets, std::size_t {0});
        if (!data.values.empty()) {
            failures += expect(read_raw((*fixture)->disk, guard->channel(), run->extent(), values_section,
                                        round_up_to_multiple(data.values.size(), portable_alignment_k), stored_bytes)
                                   .succeeded());
            failures += expect(same_bytes(std::span<byte_t const> {stored_bytes}.first(data.values.size()),
                                          std::span<byte_t const> {data.values}));
        }

        // Every point window covers its value at the values section plus the prefix sum of the sizes before it.
        std::uint64_t const values_begin = run->extent().extent.offset + values_section;
        std::size_t wrong_windows = 0;
        for (std::size_t index = 0; index != count; ++index) {
            value_window_t const window = run->value_window(index);
            wrong_windows += window.bytes != data.sizes[index];
            if (window.bytes == 0) {
                wrong_windows += window.extent.length != 0;
                continue;
            }
            wrong_windows += window.extent.offset + window.skipped != values_begin + data.offsets[index];
            wrong_windows += window.extent.offset % portable_alignment_k != 0 ||
                             window.extent.length % portable_alignment_k != 0 ||
                             window.skipped + window.bytes > window.extent.length;
        }
        failures += expect_eq(wrong_windows, std::size_t {0});

        // The whole run's tape starts at offset zero, so it reads with no rebase; a slice rebases from its first.
        value_tape_window_t const whole = run->value_tape_window(0, count);
        failures += expect_eq(whole.offsets.offset, run->extent().extent.offset + offsets_section);
        failures += expect_eq(whole.offsets_skipped, std::uint32_t {0});
        failures += expect_eq(whole.first_offset, std::uint32_t {0});
        failures += expect_eq(std::uint64_t {whole.bytes}, std::uint64_t {data.values.size()});
        std::size_t const begin = count / 3;
        std::size_t const end = count - count / 3;
        value_tape_window_t const slice = run->value_tape_window(begin, end);
        failures += expect_eq(slice.offsets.offset + slice.offsets_skipped,
                              run->extent().extent.offset + offsets_section + begin * sizeof(stored_value_offset_t));
        failures += expect(slice.offsets_skipped + (end - begin + 1) * sizeof(stored_value_offset_t) <=
                           slice.offsets.length);
        failures += expect_eq(slice.first_offset, data.offsets[begin]);
        failures += expect_eq(slice.bytes, data.offsets[end] - data.offsets[begin]);
        if (slice.bytes != 0)
            failures += expect_eq(slice.values.offset + slice.values_skipped, values_begin + data.offsets[begin]);
    }
    return failures;
}

/** A run written as @c reserve, @c write_sections and @c open holds on its extent every byte
 *  @c write leaves there for the same key slots under the same name. */
template <typename mount_type_>
std::size_t split_write_matches_write(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using variable_run_t = typename types_t::variable_run_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;
    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    variable_data_t data;
    failures += build_variable(
        1025, [](std::size_t index) -> std::int64_t { return index % 11 == 5 ? -1 : std::int64_t(index * 37 % 300); },
        data);
    if (failures != 0) return failures;
    st::vector<std::uint64_t> const stamps = ascending_stamps(data.key_slots.size());
    std::span<variable_slot_t const> const key_slots {data.key_slots};
    std::span<byte_t const> const values {data.values};
    std::span<stored_value_offset_t const> const offsets {data.offsets};
    uuid_t const content_id = uuid_generator();

    st::vector<byte_t> whole_bytes;
    medium_extent_t whole_extent;
    {
        expected<variable_run_t> whole = variable_run_t::write(
            *volume, guard->channel(), uuid_t {}, content_id, key_slots, std::span<std::uint64_t const> {stamps},
            std::span<std::uint8_t const> {data.kinds}, 0, values, offsets, 4096);
        failures += expect(whole.has_value());
        if (!whole) return failures;
        failures += expect(
            read_raw((*fixture)->disk, guard->channel(), whole->extent(), 0, whole->extent().extent.length, whole_bytes)
                .succeeded());
        whole_extent = whole->extent();
    }
    volume->deallocate(whole_extent);

    expected<sorted_run_reservation_t> reservation = variable_run_t::reserve(
        *volume, guard->channel(), uuid_t {}, content_id, key_slots, std::span<std::uint64_t const> {stamps},
        std::span<std::uint8_t const> {data.kinds}, 0, values, offsets, 4096);
    failures += expect(reservation.has_value());
    if (!reservation) return failures;
    failures += expect(
        variable_run_t::write_sections(*volume, guard->channel(), *reservation, key_slots, values, offsets)
            .succeeded());
    expected<variable_run_t> split = variable_run_t::open(*volume, guard->channel(), reservation->extent);
    failures += expect(split.has_value());
    if (!split) return failures;
    st::vector<byte_t> split_bytes;
    failures += expect(
        read_raw((*fixture)->disk, guard->channel(), split->extent(), 0, split->extent().extent.length, split_bytes)
            .succeeded());
    failures += expect(same_bytes(std::span<byte_t const> {whole_bytes}, std::span<byte_t const> {split_bytes}));
    return failures;
}

#pragma endregion Layout Cases

#pragma region Truncation Cases

/** Writes the header of a run claiming @p key_slots_count key slots at the front of @p medium. */
template <typename disk_type_>
status_t paint_stub_header(disk_type_& disk, io_channel_t& channel, medium_reference_t medium,
                           std::uint64_t key_slots_count) noexcept {
    sorted_run_header_t header {};
    header.magic = sorted_run_magic_k;
    header.format_version = sorted_run_format_version_k;
    header.key_slot_bytes = static_cast<std::uint32_t>(sizeof(variable_slot_t));
    header.key_slots_count = key_slots_count;
    header.stored_values_bytes = 0;
    header.value_max_size = 4096;
    header.branching = static_cast<std::uint16_t>(variable_layout_t::branching_k);
    header.value_layout = value_layout_t::variable_width_k;
    header.content_id = uuid_generator();

    basic_write_pipeline<disk_type_, waiter_t> writer {disk, channel, medium};
    auto [block, acquire_buffer_status] = writer.acquire_buffer();
    if (acquire_buffer_status.failed()) return acquire_buffer_status;
    std::memset(block.bytes().data(), 0, static_cast<std::size_t>(run_config_k.page_bytes));
    std::memcpy(block.bytes().data(), &header, sizeof(header));
    if (status_t const queued = writer.enqueue_write(std::move(block), extent_t {0, run_config_k.page_bytes});
        queued.failed())
        return queued;
    if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;
    return writer.make_durable();
}

/** A run whose index runs off the end of its medium is malformed, and attaches nothing. */
template <typename mount_type_>
case_result_t truncated_index_is_malformed(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using variable_run_t = typename types_t::variable_run_t;
    std::size_t failures = 0;
    constexpr std::uint64_t claimed_k = 4000;
    if (char const* const reason = mount.refusal(2)) return case_result_t::skipped(reason);
    std::array<std::uint64_t, 2> const capacities {run_config_k.volume_bytes, stub_bytes_k};
    expected<std::unique_ptr<fixture_t>> fixture = mount(capacities);
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    medium_reference_t const stub = (*fixture)->media[1].reference();
    failures += expect(paint_stub_header((*fixture)->disk, guard->channel(), stub, claimed_k).succeeded());

    // The extent is as long as the run claims, so nothing but the medium can cut the index short.
    medium_extent_t handed;
    handed.medium = stub;
    handed.extent = extent_t {0, variable_layout_t::blob_bytes(claimed_k, 0, 0, 0)};
    failures += expect(handed.extent.length > stub_bytes_k);

    expected<variable_run_t> attached = variable_run_t::open(*volume, guard->channel(), handed);
    failures += expect(!attached.has_value());
    failures += expect_status(attached, status_code_t::malformed_k);

    // An extent shorter than the run it carries is refused before a byte of the index is asked for.
    medium_extent_t clipped = handed;
    clipped.extent = extent_t {0, run_config_k.page_bytes * 2};
    expected<variable_run_t> narrow = variable_run_t::open(*volume, guard->channel(), clipped);
    failures += expect(!narrow.has_value());
    failures += expect_status(narrow, status_code_t::malformed_k);

    // An extent with no header at all is refused as well, rather than read as an empty run.
    medium_extent_t blank = handed;
    blank.extent = extent_t {run_config_k.page_bytes, run_config_k.page_bytes};
    expected<variable_run_t> unwritten = variable_run_t::open(*volume, guard->channel(), blank);
    failures += expect(!unwritten.has_value());
    failures += expect_status(unwritten, status_code_t::malformed_k);
    return failures;
}

/** A run opens again from its extent until the volume deallocates it, and then opens nothing. */
template <typename mount_type_>
std::size_t deallocated_run_opens_nothing(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using variable_run_t = typename types_t::variable_run_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    variable_data_t data;
    failures += build_variable(32, [](std::size_t) { return std::int64_t {64}; }, data);
    if (failures != 0) return failures;
    medium_extent_t extent;
    {
        expected<variable_run_t> written = variable_run_t::write(
            *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<variable_slot_t const> {data.key_slots},
            std::span<std::uint64_t const> {ascending_stamps(data.key_slots.size())},
            std::span<std::uint8_t const> {data.kinds}, 0, std::span<byte_t const> {data.values},
            std::span<stored_value_offset_t const> {data.offsets}, 4096);
        failures += expect(written.has_value());
        if (!written) return failures;
        extent = written->extent();
    }
    expected<variable_run_t> reopened = variable_run_t::open(*volume, guard->channel(), extent);
    failures += expect(reopened.has_value());

    volume->deallocate(extent);
    expected<variable_run_t> dropped = variable_run_t::open(*volume, guard->channel(), extent);
    failures += expect(!dropped.has_value());
    return failures;
}

#pragma endregion Truncation Cases

#pragma region Lookup Cases

/** A present key is found at its own position, and an absent one is reported as nothing. */
template <typename mount_type_>
std::size_t point_lookup_finds_and_misses(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using variable_run_t = typename types_t::variable_run_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 811;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    variable_data_t data;
    failures += build_variable(
        count_k, [](std::size_t index) { return 1 + static_cast<std::int64_t>(index % 97); }, data);
    if (failures != 0) return failures;
    expected<variable_run_t> run = variable_run_t::write(
        *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<variable_slot_t const> {data.key_slots},
        std::span<std::uint64_t const> {ascending_stamps(data.key_slots.size())},
        std::span<std::uint8_t const> {data.kinds}, 0, std::span<byte_t const> {data.values},
        std::span<stored_value_offset_t const> {data.offsets}, 4096);
    failures += expect(run.has_value());
    if (!run) return failures;

    for (std::size_t index = 0; index != count_k; ++index) {
        std::uint64_t position = 0;
        failures += expect(run->find(key_of(index), position) == lookup_result_t::written_k);
        failures += expect_eq(position, static_cast<std::uint64_t>(index));
        failures += expect_eq(run->key_slot(position).stored_key, key_of(index));
    }
    for (std::size_t index = 0; index != count_k; ++index) {
        std::uint64_t position = 0;
        failures += expect(run->find(key_of(index) + 1, position) == lookup_result_t::missing_k);
        failures += expect_eq(position, static_cast<std::uint64_t>(count_k));
    }

    // Keys outside the run's bounds miss as cleanly as keys inside the gaps do.
    std::uint64_t position = 0;
    failures += expect(run->find(0, position) == lookup_result_t::missing_k);
    failures += expect(run->find(key_of(count_k) + 1000, position) == lookup_result_t::missing_k);

    // A walk visits every key slot exactly once, in ascending order of the keys.
    std::size_t visited = 0;
    std::size_t disordered = 0;
    stored_key_t previous = 0;
    run->for_each_key_slot([&](std::uint64_t at, variable_slot_t const& slot) noexcept {
        if (visited != 0 && !(previous < slot.stored_key)) ++disordered;
        if (at != visited) ++disordered;
        previous = slot.stored_key;
        ++visited;
    });
    failures += expect_eq(visited, count_k);
    failures += expect_eq(disordered, std::size_t {0});
    return failures;
}

/** Every lookup of a run reopened from its volume lands where the saved tree of its keys, read raw
 *  off the volume, ranks it, over every present and absent key across a tree three levels deep. */
template <typename mount_type_>
std::size_t lookups_descend_the_saved_tree(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using variable_run_t = typename types_t::variable_run_t;
    using keys_view_t = typename variable_layout_t::keys_view_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 5000;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;
    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    variable_data_t data;
    failures += build_variable(count_k, [](std::size_t index) { return static_cast<std::int64_t>(index % 5); }, data);
    if (failures != 0) return failures;
    expected<variable_run_t> written = variable_run_t::write(
        *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<variable_slot_t const> {data.key_slots},
        std::span<std::uint64_t const> {ascending_stamps(count_k)}, std::span<std::uint8_t const> {data.kinds}, 0,
        std::span<byte_t const> {data.values}, std::span<stored_value_offset_t const> {data.offsets}, 4096);
    failures += expect(written.has_value());
    if (!written) return failures;
    expected<variable_run_t> run = variable_run_t::open(*volume, guard->channel(), written->extent());
    failures += expect(run.has_value());
    if (!run) return failures;

    st::vector<byte_t> saved_bytes;
    failures += expect(read_raw((*fixture)->disk, guard->channel(), run->extent(),
                                variable_layout_t::keys_offset(count_k), variable_layout_t::keys_section_bytes(count_k),
                                saved_bytes)
                           .succeeded());
    if (failures != 0) return failures;
    st::expected<keys_view_t> const saved = keys_view_t::make(std::as_bytes(std::span<byte_t const> {saved_bytes}));
    failures += expect(saved.has_value());
    if (!saved) return failures;
    failures += expect_eq(saved->size(), count_k);
    failures += expect_eq(run->keys().size(), count_k);

    std::size_t misranked = 0;
    std::size_t misfound = 0;
    for (stored_key_t probe = 0; probe <= key_of(count_k); ++probe) {
        std::uint64_t const below = std::min<std::uint64_t>(probe / 2, count_k);
        std::uint64_t const ranked = run->lower_bound(probe);
        misranked += ranked != saved->rank(probe) || ranked != below;
        std::uint64_t position = 0;
        bool const present = probe % 2 == 1 && probe / 2 < count_k;
        lookup_result_t const result = run->find(probe, position);
        misfound += present ? result == lookup_result_t::missing_k || position != below
                            : result != lookup_result_t::missing_k;
    }
    failures += expect_eq(misranked, std::size_t {0});
    failures += expect_eq(misfound, std::size_t {0});
    return failures;
}

/** A run of one key slot and a run of none both open, and answer the only questions they can. */
template <typename mount_type_>
std::size_t runs_at_the_smallest_sizes(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using variable_run_t = typename types_t::variable_run_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    variable_data_t alone;
    failures += build_variable(1, [](std::size_t) { return std::int64_t {7}; }, alone);
    if (failures != 0) return failures;
    expected<variable_run_t> alone_run = variable_run_t::write(
        *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<variable_slot_t const> {alone.key_slots},
        std::span<std::uint64_t const> {ascending_stamps(alone.key_slots.size())},
        std::span<std::uint8_t const> {alone.kinds}, 0, std::span<byte_t const> {alone.values},
        std::span<stored_value_offset_t const> {alone.offsets}, 4096);
    failures += expect(alone_run.has_value());
    if (!alone_run) return failures;
    failures += expect_eq(alone_run->population_count(), std::uint64_t {1});
    failures += expect_eq(alone_run->lower_stored_key(), key_of(0));
    failures += expect_eq(alone_run->upper_stored_key(), key_of(0));
    std::uint64_t position = 0;
    failures += expect(alone_run->find(key_of(0), position) == lookup_result_t::written_k);
    failures += expect_eq(position, std::uint64_t {0});
    failures += expect(alone_run->find(key_of(1), position) == lookup_result_t::missing_k);
    failures += check_variable_values(*alone_run, guard->channel(), alone);

    variable_data_t none;
    failures += build_variable(0, [](std::size_t) { return std::int64_t {0}; }, none);
    expected<variable_run_t> empty = variable_run_t::write(
        *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<variable_slot_t const> {none.key_slots},
        std::span<std::uint64_t const> {ascending_stamps(none.key_slots.size())},
        std::span<std::uint8_t const> {none.kinds}, 0, std::span<byte_t const> {none.values},
        std::span<stored_value_offset_t const> {none.offsets}, 4096);
    failures += expect(empty.has_value());
    if (!empty) return failures;
    failures += expect_eq(empty->population_count(), std::uint64_t {0});
    // An empty run is its header, the page of its empty tree, the page of its one value offset, and
    // the trailer of empty stamps and kinds columns
    failures += expect_eq(empty->size_bytes(), variable_layout_t::header_bytes() + 2 * portable_alignment_k +
                                                   variable_layout_t::trailer_bytes(empty->stamps().saved_bytes(),
                                                                                    empty->kinds().saved_bytes()));
    failures += expect(empty->lower_stored_key() == limit<stored_key_t>::max());
    failures += expect(empty->upper_stored_key() == limit<stored_key_t>::min());
    failures += expect(empty->find(key_of(0), position) == lookup_result_t::missing_k);
    failures += expect_eq(position, std::uint64_t {0});
    failures += check_variable_values(*empty, guard->channel(), none);

    // Both survive a reopen, which is where a zero-length index would have been read anyway.
    expected<variable_run_t> reopened_alone = variable_run_t::open(*volume, guard->channel(), alone_run->extent());
    failures += expect(reopened_alone.has_value());
    expected<variable_run_t> reopened_empty = variable_run_t::open(*volume, guard->channel(), empty->extent());
    failures += expect(reopened_empty.has_value());
    if (reopened_empty) failures += expect_eq(reopened_empty->population_count(), std::uint64_t {0});
    return failures;
}

/** Values at both ends of what the layout admits read back whole, across as many blocks as they need. */
template <typename mount_type_>
std::size_t values_section_the_size_extremes(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using variable_run_t = typename types_t::variable_run_t;
    std::size_t failures = 0;
    constexpr std::int64_t largest_k = 192 * 1024;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    // One byte, one empty value, one that fills a block exactly, and one that spans several.
    auto sized = [](std::size_t index) -> std::int64_t {
        switch (index % 5) {
        case 0: return 1;
        case 1: return 0;
        case 2: return static_cast<std::int64_t>(run_config_k.request_buffer_bytes);
        case 3: return largest_k;
        default: return static_cast<std::int64_t>(run_config_k.page_bytes) - 1;
        }
    };
    variable_data_t data;
    failures += build_variable(40, sized, data);
    if (failures != 0) return failures;

    expected<variable_run_t> run = variable_run_t::write(
        *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<variable_slot_t const> {data.key_slots},
        std::span<std::uint64_t const> {ascending_stamps(data.key_slots.size())},
        std::span<std::uint8_t const> {data.kinds}, 0, std::span<byte_t const> {data.values},
        std::span<stored_value_offset_t const> {data.offsets}, largest_k);
    failures += expect(run.has_value());
    if (!run) return failures;
    failures += check_variable_values(*run, guard->channel(), data);

    // The widest value needs more than one block, so its window is wider than the channel's block.
    std::uint64_t position = 0;
    failures += expect(run->find(key_of(3), position) == lookup_result_t::written_k);
    value_window_t const widest = run->value_window(position);
    failures += expect(widest.extent.length > run_config_k.request_buffer_bytes);
    failures += expect_eq(static_cast<std::uint64_t>(widest.bytes), static_cast<std::uint64_t>(largest_k));

    // A present but empty value needs no read at all, and says so with an empty window.
    failures += expect(run->find(key_of(1), position) == lookup_result_t::written_k);
    failures += expect_eq(run->value_window(position).extent.length, std::uint64_t {0});
    return failures;
}

/** A remove delta is a key the run carries and a value it does not, at every layer that reports it. */
template <typename mount_type_>
std::size_t remove_deltas_read_back_as_removals(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using variable_run_t = typename types_t::variable_run_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 401;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    // Every third key is removed, every fifth holds a value that is present and empty.
    auto sized = [](std::size_t index) -> std::int64_t {
        if (index % 3 == 0) return -1;
        if (index % 5 == 0) return 0;
        return 1 + static_cast<std::int64_t>(index % 613);
    };
    variable_data_t data;
    failures += build_variable(count_k, sized, data);
    if (failures != 0) return failures;

    medium_extent_t extent;
    {
        expected<variable_run_t> written = variable_run_t::write(
            *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<variable_slot_t const> {data.key_slots},
            std::span<std::uint64_t const> {ascending_stamps(data.key_slots.size())},
            std::span<std::uint8_t const> {data.kinds}, 0, std::span<byte_t const> {data.values},
            std::span<stored_value_offset_t const> {data.offsets}, 4096);
        failures += expect(written.has_value());
        if (!written) return failures;
        extent = written->extent();
    }
    expected<variable_run_t> run = variable_run_t::open(*volume, guard->channel(), extent);
    failures += expect(run.has_value());
    if (!run) return failures;

    for (std::size_t index = 0; index != count_k; ++index) {
        std::uint64_t position = 0;
        lookup_result_t const found = run->find(key_of(index), position);
        if (sized(index) < 0) {
            failures += expect(found == lookup_result_t::removed_k);
            failures += expect(run->kind(position) == delta_kind_t::remove_k);
            failures += expect_eq(run->value_window(position).extent.length, std::uint64_t {0});
        }
        else {
            failures += expect(found == lookup_result_t::written_k);
            failures += expect(run->kind(position) == delta_kind_t::write_k);
        }
    }
    failures += check_variable_values(*run, guard->channel(), data);
    return failures;
}

#pragma endregion Lookup Cases

#pragma region Depth Cases

/**
 *  @brief A batched read of a run's values sits at the queue pair's depth through the middle of the run.
 *
 *  The extents come from the run's own windows, so what is held to the depth is the arithmetic a
 *  value read is built on rather than a batch of blocks the case made up.
 */
template <typename mount_type_>
std::size_t batched_read_holds_its_depth(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using variable_run_t = typename types_t::variable_run_t;
    using read_pipeline_t = typename types_t::read_pipeline_t;
    std::size_t failures = 0;
    constexpr std::size_t count_k = 512;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    variable_data_t data;
    failures += build_variable(
        count_k, [](std::size_t index) { return 1 + static_cast<std::int64_t>(index % 1024); }, data);
    if (failures != 0) return failures;
    expected<variable_run_t> run = variable_run_t::write(
        *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<variable_slot_t const> {data.key_slots},
        std::span<std::uint64_t const> {ascending_stamps(data.key_slots.size())},
        std::span<std::uint8_t const> {data.kinds}, 0, std::span<byte_t const> {data.values},
        std::span<stored_value_offset_t const> {data.offsets}, 4096);
    failures += expect(run.has_value());
    if (!run) return failures;

    read_pipeline_t reader {(*fixture)->disk, guard->channel(), run->extent().medium};
    std::size_t offered = 0;
    std::size_t polled_total = 0;
    std::size_t mismatches = 0;
    std::size_t deepest = 0;
    auto sink = [&](completed_buffer_t const& completed) noexcept {
        ++polled_total;
        if (require_completed_in_full(completed).failed()) {
            ++mismatches;
            return;
        }
        value_window_t const window = run->value_window(completed.tag);
        for (std::uint32_t index = 0; index != window.bytes; ++index)
            if (completed.bytes[window.skipped + index] != painted(completed.tag, index)) ++mismatches;
    };

    while (offered != count_k || reader.outstanding() != 0) {
        while (offered != count_k && reader.can_enqueue()) {
            value_window_t const window = run->value_window(offered);
            if (reader.enqueue_read(window.extent, static_cast<std::uint32_t>(offered)).failed()) break;
            ++offered;
        }
        failures += expect(reader.submit().succeeded());
        std::size_t const live = (*fixture)->disk.outstanding(guard->channel().queue_pair());
        if (live > deepest) deepest = live;
        expected<std::size_t> const seen = reader.poll_completions(sink);
        failures += expect(seen.has_value());
        if (!seen) return failures;
    }

    failures += expect_eq(polled_total, count_k);
    failures += expect_eq(mismatches, std::size_t {0});
    failures += expect_eq(deepest, static_cast<std::size_t>(run_config_k.queue_depth));

    // The run's own batched read holds as many blocks as the queue pair has credit, counted in the pool
    // because the ring performs a channel's requests inside the poll that hands them to the sink.
    st::vector<std::uint64_t> positions;
    failures += expect(positions.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) positions[index] = index;

    std::uint32_t idlest = run_config_k.request_buffers;
    auto counting = [&](std::size_t, std::uint64_t, std::span<byte_t const>) noexcept {
        std::uint32_t const free_blocks = guard->channel().requests().available_buffers();
        if (free_blocks < idlest) idlest = free_blocks;
    };
    failures += expect(
        run->read_values(guard->channel(), std::span<std::uint64_t const> {positions}, counting).succeeded());
    failures += expect_eq(run_config_k.request_buffers - idlest, run_config_k.queue_depth);
    return failures;
}

#pragma endregion Depth Cases

#pragma region Tails Cases

/** A run of strings keeps every string's head and its 16-byte value in a 32-byte cell, with no values section, and the
 *  bytes past the head in its tails, which order the strings of one head and read back whole through a volume mounted
 *  again, while a run of strings its heads hold whole carries neither tails nor their offsets. */
template <typename mount_type_>
case_result_t tails_read_back(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using tailed_slot_t = key_slot<st::key128_t, value_layout_t::uuid_k, key_tails_t::carried_k>;
    using tailed_run_t = lsm_sorted_run<tailed_slot_t, volume_t, waiter_t>;
    using tailed_layout_t = strings_run_layout<tailed_slot_t>;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;
    if (!(*fixture)->persists_volumes()) return case_result_t::skipped(memory_volumes_vanish_k);
    // A run of strings with no tie wider than a row carries the layers section's header alone.
    constexpr std::uint64_t no_layers_bytes_k = sizeof(head_layers_header_t);
    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    // Ascending as whole strings, the two versions of one string newest first, five long strings sharing one head
    std::string const long_tail(1000, 'z');
    std::array<std::string, 9> const texts {"",
                                            "a",
                                            "fifteen-bytes!!",
                                            "shared-prefix!!a",
                                            "shared-prefix!!a",
                                            "shared-prefix!!ab",
                                            "shared-prefix!!b",
                                            "shared-prefix!!" + long_tail,
                                            "zz"};
    std::array<std::uint64_t, 9> const stamps {3, 4, 5, 9, 6, 7, 8, 10, 11};
    st::vector<tailed_slot_t> key_slots;
    st::vector<std::uint8_t> kinds;
    st::vector<std::uint32_t> tail_offsets;
    std::string tails;
    failures += expect(key_slots.resize(texts.size()) == st::status_t::success_k);
    failures += expect(kinds.resize(texts.size()) == st::status_t::success_k);
    failures += expect(tail_offsets.resize(texts.size() + 1) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != texts.size(); ++index) {
        tailed_slot_t slot;
        slot.stored_key = strings::head_of(texts[index]);
        for (std::size_t position = 0; position != tailed_slot_t::size; ++position)
            slot.data[position] = painted(index, position);
        key_slots[index] = slot;
        kinds[index] = static_cast<std::uint8_t>(delta_kind_t::write_k);
        tail_offsets[index] = static_cast<std::uint32_t>(tails.size());
        tails += strings::tail_of(texts[index]);
    }
    tail_offsets[texts.size()] = static_cast<std::uint32_t>(tails.size());
    auto const bytes_of = [](std::string const& text) noexcept {
        return std::span<byte_t const> {reinterpret_cast<byte_t const*>(text.data()), text.size()};
    };
    key_tails_column_t const run_tails {std::span<std::uint32_t const> {tail_offsets}, bytes_of(tails)};

    uuid_t const content_id = uuid_generator();
    {
        expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
        failures += expect(volume.has_value());
        if (!volume) return failures;
        expected<tailed_run_t> written = tailed_run_t::write(
            *volume, guard->channel(), uuid_t {}, content_id, std::span<tailed_slot_t const> {key_slots},
            std::span<std::uint64_t const> {stamps}, std::span<std::uint8_t const> {kinds}, 0, {}, {},
            tailed_slot_t::size, run_tails);
        failures += expect(written.has_value());
        if (!written) return failures;
        std::uint64_t whole_bytes = 0;
        for (std::string const& text : texts) whole_bytes += text.size();
        failures += expect_eq(std::uint64_t {written->header().key_tape_bytes}, whole_bytes);
        failures += expect_eq(written->size_bytes(), tailed_layout_t::blob_bytes(written->header()));
        failures += expect_eq(written->size_bytes(),
                              tailed_layout_t::blob_bytes(texts.size(), whole_bytes, written->stamps().saved_bytes(),
                                                          written->kinds().saved_bytes(), no_layers_bytes_k));

        // Two strings of one head whose tails descend are out of order, which the run refuses
        std::array<tailed_slot_t, 2> const swapped_slots {key_slots[6], key_slots[5]};
        std::array<std::uint32_t, 3> const swapped_offsets {0, 1, 3};
        std::string const swapped_tails = "bab";
        std::array<std::uint64_t, 2> const swapped_stamps {1, 2};
        std::array<std::uint8_t, 2> const swapped_kinds {0, 0};
        expected<tailed_run_t> misordered = tailed_run_t::write(
            *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<tailed_slot_t const> {swapped_slots},
            std::span<std::uint64_t const> {swapped_stamps}, std::span<std::uint8_t const> {swapped_kinds}, 0, {}, {},
            tailed_slot_t::size,
            key_tails_column_t {std::span<std::uint32_t const> {swapped_offsets}, bytes_of(swapped_tails)});
        failures += expect_status(misordered, status_code_t::invalid_argument_k);

        // Strings their heads hold whole save no tails and no offsets
        std::array<tailed_slot_t, 3> const short_slots {key_slots[0], key_slots[1], key_slots[2]};
        expected<tailed_run_t> whole = tailed_run_t::write(
            *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<tailed_slot_t const> {short_slots},
            std::span<std::uint64_t const> {stamps}.first(3), std::span<std::uint8_t const> {kinds}.first(3), 0, {}, {},
            tailed_slot_t::size);
        failures += expect(whole.has_value());
        if (!whole) return failures;
        failures += expect_eq(whole->key(2), std::string_view {texts[2]});
        std::uint64_t const short_bytes = texts[0].size() + texts[1].size() + texts[2].size();
        failures += expect_eq(whole->size_bytes(),
                              tailed_layout_t::blob_bytes(3, short_bytes, whole->stamps().saved_bytes(),
                                                          whole->kinds().saved_bytes(), no_layers_bytes_k));
        manifest_file_t const files[] {file_of(written->extent(), content_id)};
        failures += expect(publish_naming(*volume, **fixture, guard->channel(), files).succeeded());
    }

    expected<volume_t> remounted = (*fixture)->mount_volume(guard->channel());
    failures += expect(remounted.has_value());
    if (!remounted) return failures;
    failures += expect_eq(remounted->manifest().files.size(), std::size_t {1});
    if (remounted->manifest().files.size() != 1) return failures;
    expected<medium_extent_t> found = remounted->open(remounted->manifest().files[0]);
    failures += expect(found.has_value());
    if (!found) return failures;
    expected<tailed_run_t> opened = tailed_run_t::open(*remounted, guard->channel(), *found);
    failures += expect(opened.has_value());
    if (!opened) return failures;
    failures += expect_eq(opened->population_count(), static_cast<std::uint64_t>(texts.size()));
    failures += expect_eq(opened->header().key_slot_bytes, std::uint32_t {32});
    failures += expect(opened->header().value_layout == value_layout_t::uuid_k);
    failures += expect_eq(opened->header().stored_values_bytes, std::uint64_t {0});
    for (std::size_t index = 0; index != texts.size(); ++index) {
        failures += expect(opened->head(index) == strings::head_of(texts[index]));
        failures += expect_eq(opened->key(index), std::string_view {texts[index]});
        failures += expect_eq(opened->stamp(index), stamps[index]);
    }
    std::array<byte_t, tailed_slot_t::size> read {};
    failures += expect(opened->read_value(guard->channel(), 7, std::span<byte_t> {read}).succeeded());
    for (std::size_t position = 0; position != read.size(); ++position)
        failures += expect(read[position] == painted(7, position));
    return failures;
}

/** URLs of 60 sections of 400 items each under one long prefix, ascending, so equal heads
 *  tie three deep. */
struct url_corpus_t {

    /** The URLs back to back. */
    st::vector<char> tape;

    /** One offset per URL into @c tape and one past the last. */
    st::vector<std::uint32_t> offsets;

    /** The URL at @p index. */
    [[nodiscard]] std::string_view at(std::size_t index) const noexcept {
        return std::string_view {tape.data() + offsets[index], offsets[index + 1] - offsets[index]};
    }

    /** URLs it holds. */
    [[nodiscard]] std::size_t size() const noexcept { return offsets.size() - 1; }

    /** The first URL not below @p text, by a plain binary search, which the test reads
     *  as the truth. */
    [[nodiscard]] std::size_t lower_bound(std::string_view text) const noexcept {
        std::size_t low = 0;
        std::size_t high = size();
        while (low != high) {
            std::size_t const middle = low + (high - low) / 2;
            if (at(middle) < text) low = middle + 1;
            else high = middle;
        }
        return low;
    }
};

/** Lays the URL corpus out, @p sections by @p items, ascending. */
[[nodiscard]] std::size_t build_url_corpus(std::size_t sections, std::size_t items, url_corpus_t& corpus) {
    std::size_t failures = 0;
    failures += expect(corpus.offsets.push_back(std::uint32_t {0}) == st::status_t::success_k);
    for (std::size_t section = 0; section != sections; ++section)
        for (std::size_t item = 0; item != items; ++item) {
            char url[96];
            auto const written = fmt::format_to_n(
                url, sizeof(url), "https://example.org/catalog/section-{:03}/item-{:05}", section, item);
            std::size_t const length = static_cast<std::size_t>(written.out - url);
            std::size_t const at = corpus.tape.size();
            failures += expect(corpus.tape.resize(at + length) == st::status_t::success_k);
            if (failures != 0) return failures;
            std::memcpy(corpus.tape.data() + at, url, length);
            failures += expect(corpus.offsets.push_back(static_cast<std::uint32_t>(at + length)) ==
                               st::status_t::success_k);
        }
    return failures;
}

/** A run of URLs sharing a 36-byte prefix gets nested layers, and every lookup the heads and the
 *  layers answer leaves at most one row of keys to compare, never a binary search over the key
 *  tape, while a key with more versions than a row keeps them in one range of its own. */
template <typename mount_type_>
std::size_t layers_narrow_every_lookup(mount_type_ const& mount) {
    using types_t = run_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using volume_t = typename types_t::volume_t;
    using tailed_slot_t = key_slot<st::key128_t, value_layout_t::uuid_k, key_tails_t::carried_k>;
    using tailed_run_t = lsm_sorted_run<tailed_slot_t, volume_t, waiter_t>;
    constexpr std::size_t row_k = strings_run_layout<tailed_slot_t>::keys_per_row_k;
    constexpr std::size_t versions_k = 40;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;
    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    expected<volume_t> volume = (*fixture)->mount_volume(guard->channel());
    failures += expect(volume.has_value());
    if (!volume) return failures;

    url_corpus_t corpus;
    failures += build_url_corpus(60, 400, corpus);
    if (failures != 0) return failures;

    // One short key in many versions, newest first, ahead of every URL.
    std::size_t const count = versions_k + corpus.size();
    st::vector<tailed_slot_t> key_slots;
    st::vector<std::uint64_t> stamps;
    st::vector<std::uint8_t> kinds;
    st::vector<std::uint32_t> tail_offsets;
    st::vector<byte_t> tails;
    failures += expect(key_slots.resize(count) == st::status_t::success_k);
    failures += expect(stamps.resize(count) == st::status_t::success_k);
    failures += expect(kinds.resize(count) == st::status_t::success_k);
    failures += expect(tail_offsets.resize(count + 1) == st::status_t::success_k);
    if (failures != 0) return failures;
    auto text_of = [&](std::size_t position) noexcept {
        return position < versions_k ? std::string_view {"a"} : corpus.at(position - versions_k);
    };
    std::uint32_t tails_bytes = 0;
    for (std::size_t position = 0; position != count; ++position) {
        std::string_view const text = text_of(position);
        tailed_slot_t slot;
        slot.stored_key = strings::head_of(text);
        for (std::size_t byte = 0; byte != tailed_slot_t::size; ++byte) slot.data[byte] = painted(position, byte);
        key_slots[position] = slot;
        stamps[position] = position < versions_k ? versions_k - position : versions_k + position;
        kinds[position] = static_cast<std::uint8_t>(delta_kind_t::write_k);
        tail_offsets[position] = tails_bytes;
        std::string_view const tail = strings::tail_of(text);
        failures += expect(tails.resize(tails_bytes + tail.size()) == st::status_t::success_k);
        if (failures != 0) return failures;
        if (!tail.empty()) std::memcpy(tails.data() + tails_bytes, tail.data(), tail.size());
        tails_bytes += static_cast<std::uint32_t>(tail.size());
    }
    tail_offsets[count] = tails_bytes;

    expected<tailed_run_t> run = tailed_run_t::write(
        *volume, guard->channel(), uuid_t {}, uuid_generator(), std::span<tailed_slot_t const> {key_slots},
        std::span<std::uint64_t const> {stamps}, std::span<std::uint8_t const> {kinds}, 0, {}, {}, tailed_slot_t::size,
        key_tails_column_t {std::span<std::uint32_t const> {tail_offsets}, std::span<byte_t const> {tails}});
    failures += expect(run.has_value());
    if (!run) return failures;

    // The URLs tie on "https://example", on ".org/catalog/se", and on each section's "ction-000/item-".
    std::uint32_t deepest = 0;
    for (head_layer_t const& layer : run->layers()) deepest = std::max(deepest, layer.depth);
    failures += expect_eq(deepest, std::uint32_t {3});
    failures += expect(run->header().layers_bytes > sizeof(head_layers_header_t));
    failures += expect_eq(run->size_bytes(), strings_run_layout<tailed_slot_t>::blob_bytes(run->header()));

    std::size_t wide = 0;
    std::size_t misplaced = 0;
    std::size_t misread = 0;
    for (std::size_t position = versions_k; position != count; ++position) {
        std::string_view const text = text_of(position);
        key_positions_t const range = run->key_range(text);
        wide += range.last - range.first > row_k;
        misplaced += position < range.first || position >= range.last;
        misread += run->lower_bound(text) != position || run->key(position) != text;
        std::span<byte_t const> const value = run->value(position);
        misread += value[0] != painted(position, 0);
    }
    failures += expect_eq(wide, std::size_t {0});
    failures += expect_eq(misplaced, std::size_t {0});
    failures += expect_eq(misread, std::size_t {0});

    // A key no URL equals lands where a plain search of the corpus puts it.
    std::size_t missed = 0;
    char probe[128];
    for (std::size_t index = 0; index < corpus.size(); index += 97) {
        std::string_view const text = corpus.at(index);
        std::memcpy(probe, text.data(), text.size());
        probe[text.size()] = '~';
        std::string_view const above {probe, text.size() + 1};
        missed += run->lower_bound(above) != versions_k + corpus.lower_bound(above);
        missed += run->lower_bound(text.substr(0, 40)) != versions_k + corpus.lower_bound(text.substr(0, 40));
    }
    failures += expect_eq(missed, std::size_t {0});

    // The versions of one key stay one range wider than a row, which one compare places whole.
    key_positions_t const versions = run->key_range("a");
    failures += expect_eq(versions.first, std::uint64_t {0});
    failures += expect_eq(versions.last, std::uint64_t {versions_k});
    failures += expect_eq(run->lower_bound("a"), std::uint64_t {0});
    failures += expect_eq(run->lower_bound("b"), std::uint64_t {versions_k});
    return failures;
}

#pragma endregion Tails Cases

} // namespace unum::ustore::test

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;

    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    st::expected<directory_t> const directory = directory_t::make(options.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    test_environment_t const environment = test_environment_t::of(options, *directory);

    auto const cases = [&](auto const& mount) {
        report(totals, mount, "reads_back_every_value", run_reads_back_every_value);
        report(totals, mount, "fixed_width_reads_back", fixed_width_run_reads_back);
        report(totals, mount, "inline_needs_no_reads", inline_run_needs_no_reads);
        report(totals, mount, "sections_on_pages", sections_sit_on_page_boundaries);
        report(totals, mount, "split_write_matches_write", split_write_matches_write);
        report(totals, mount, "truncated_index", truncated_index_is_malformed);
        report(totals, mount, "deallocated_opens_nothing", deallocated_run_opens_nothing);
        report(totals, mount, "point_lookup", point_lookup_finds_and_misses);
        report(totals, mount, "lookups_descend_the_saved_tree", lookups_descend_the_saved_tree);
        report(totals, mount, "smallest_sizes", runs_at_the_smallest_sizes);
        report(totals, mount, "value_size_extremes", values_section_the_size_extremes);
        report(totals, mount, "remove_deltas", remove_deltas_read_back_as_removals);
        report(totals, mount, "holds_its_depth", batched_read_holds_its_depth);
        report(totals, mount, "tails_read_back", tails_read_back);
        report(totals, mount, "layers_narrow_every_lookup", layers_narrow_every_lookup);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, run_config_k, 1, "run", cases);

    return suite_exit_code(totals);
}
