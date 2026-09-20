/**
 *  @file test/nvme_sorted_run.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The sorted run over the ring: what it writes, what it refuses to open, and how deep it reads.
 *
 *  The result this file exists for is @c truncated_index: a run whose index does not read whole is a
 *  truncated run and must answer @c malformed_k rather than attaching over a half-filled index, which
 *  is the failure a mapped index could never report. The rest holds the layout to a byte-for-byte
 *  round trip at both ends of the value sizes it admits.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <array>   // `std::array`
#include <memory>  // `std::unique_ptr`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "nvme/io_pipeline.hpp"    // `basic_read_pipeline`, `basic_write_pipeline`
#include "nvme/key_slot.hpp"       // `key_slot`
#include "nvme/lsm_sorted_run.hpp" // `lsm_sorted_run`, `sorted_run_layout`

#include "nvme_fixture.hpp" // `basic_fixture`, `run_on_transport`, `painted`

namespace unum::ustore {

using namespace unum::ustore::nvme;

#pragma region Shape

/** The numbers every case mounts with: a streaming block of a megabyte, so one carries a whole index. */
constexpr fixture_config_t run_config_k {
    .stream_buffer_bytes = 1024ull * 1024ull,
    .stream_buffers = 2,
    .catalog_slot_bytes = 128ull * 1024ull,
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
using inline_slot_t = key_slot<stored_key_t, value_layout_t::inline_8_bytes_k>;
using variable_layout_t = sorted_run_layout<variable_slot_t>;
using fixed_layout_t = sorted_run_layout<fixed_slot_t>;

/** The runs and the pipelines every case drives over the volume of @p fixture_type_. */
template <typename fixture_type_>
struct run_types {
    using fixture_t = fixture_type_;
    using guard_t = typename fixture_t::guard_t;
    using disk_t = typename fixture_t::disk_t;
    using volume_t = typename fixture_t::volume_t;
    using variable_run_t = lsm_sorted_run<variable_slot_t, volume_t>;
    using fixed_run_t = lsm_sorted_run<fixed_slot_t, volume_t>;
    using inline_run_t = lsm_sorted_run<inline_slot_t, volume_t>;
    using read_pipeline_t = basic_read_pipeline<disk_t>;
    using write_pipeline_t = basic_write_pipeline<disk_t>;
};

/** The key the key slot at @p index carries, kept odd so every even key is a miss. */
[[nodiscard]] constexpr stored_key_t key_of(std::size_t index) noexcept {
    return static_cast<stored_key_t>(index) * 2 + 1;
}

#pragma endregion Shape

#pragma region Builders

/** The key slots and the bytes behind them that one variable-width case writes. */
struct variable_data_t {

    /** Key slots in ascending order of their keys. */
    st::basic_vector<variable_slot_t> key_slots;

    /** The values back to back, in the order their key slots locate them. */
    st::basic_vector<byte_t> values;

    /** Bytes the value of every key slot holds, which a read is checked against. */
    st::basic_vector<std::uint64_t> sizes;
};

/**
 *  @brief Lays out @p count variable-width key slots whose value sizes come from @p sized.
 *  @param[in] sized Called as @c sized(index); a negative answer is a tombstone and zero an empty value.
 */
template <typename sized_type_>
[[nodiscard]] std::size_t build_variable(std::size_t count, sized_type_&& sized, variable_data_t& into) {
    std::size_t failures = 0;
    failures += expect(into.sizes.resize(count) == st::status_t::success_k);
    failures += expect(into.key_slots.resize(count) == st::status_t::success_k);
    if (failures != 0) return failures;

    std::uint64_t total = 0;
    for (std::size_t index = 0; index != count; ++index) {
        std::int64_t const wanted = sized(index);
        into.sizes[index] = wanted > 0 ? static_cast<std::uint64_t>(wanted) : 0;
        total += into.sizes[index];
    }
    failures += expect(into.values.resize(static_cast<std::size_t>(total)) == st::status_t::success_k);
    if (failures != 0) return failures;

    std::uint64_t location = variable_layout_t::values_offset(count);
    std::uint64_t written = 0;
    for (std::size_t index = 0; index != count; ++index) {
        variable_slot_t slot;
        slot.stored_key = key_of(index);
        if (sized(index) < 0) slot.make_tombstone();
        else if (into.sizes[index] == 0) {
            slot.size = 0;
            slot.location = empty_stored_value_offset_k;
        }
        else {
            slot.size = static_cast<stored_value_size_t>(into.sizes[index]);
            slot.location = static_cast<stored_value_offset_t>(location);
            for (std::uint64_t byte = 0; byte != into.sizes[index]; ++byte)
                into.values[static_cast<std::size_t>(written + byte)] = painted(index, byte);
            location += into.sizes[index];
            written += into.sizes[index];
        }
        into.key_slots[index] = slot;
    }
    return failures;
}

/** Reads every value of @p run at every position, counting the bytes that do not match what was written. */
template <typename run_type_>
[[nodiscard]] std::size_t check_variable_values(run_type_ const& run, io_channel_t& channel,
                                                variable_data_t const& data) {
    std::size_t failures = 0;
    std::size_t const count = data.sizes.size();
    st::basic_vector<std::uint64_t> seen;
    failures += expect(seen.resize(count) == st::status_t::success_k);
    st::basic_vector<std::uint64_t> positions;
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
std::size_t run_reads_back_every_value(mount_type_ const& mount) {
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

        expected<variable_run_t> written = variable_run_t::write(*mounted, guard->channel(), content_id,
                                                                 std::span<variable_slot_t const> {data.key_slots},
                                                                 std::span<byte_t const> {data.values}, 4096);
        failures += expect(written.has_value());
        if (!written) return failures;
        failures += expect_eq(written->population_count(), static_cast<std::uint64_t>(count_k));
        failures += expect_eq(written->lower_stored_key(), key_of(0));
        failures += expect_eq(written->upper_stored_key(), key_of(count_k - 1));
        failures += check_variable_values(*written, guard->channel(), data);
    }

    // A run is only really written once a volume mounted again from the medium answers to its name.
    expected<volume_t> remounted = (*fixture)->mount_volume(guard->channel(), 0, volume_recovery_t::refuse_k);
    failures += expect(remounted.has_value());
    if (!remounted) return failures;

    expected<variable_run_t> opened = variable_run_t::open(*remounted, guard->channel(), content_id);
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

    st::basic_vector<fixed_slot_t> key_slots;
    st::basic_vector<byte_t> values;
    failures += expect(key_slots.resize(count_k) == st::status_t::success_k);
    failures += expect(values.resize(count_k * value_size_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    std::uint64_t location = fixed_layout_t::values_offset(count_k);
    for (std::size_t index = 0; index != count_k; ++index) {
        fixed_slot_t slot;
        slot.stored_key = key_of(index);
        slot.size = value_size_k;
        slot.location = static_cast<stored_value_offset_t>(location);
        location += value_size_k;
        key_slots[index] = slot;
        for (std::size_t byte = 0; byte != value_size_k; ++byte)
            values[index * value_size_k + byte] = painted(index, byte);
    }

    expected<fixed_run_t> written = fixed_run_t::write(*volume, guard->channel(), uuid_generator(),
                                                       std::span<fixed_slot_t const> {key_slots},
                                                       std::span<byte_t const> {values}, value_size_k);
    failures += expect(written.has_value());
    if (!written) return failures;

    st::basic_vector<std::uint64_t> positions;
    failures += expect(positions.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) positions[index] = index;

    std::size_t mismatches = 0;
    st::basic_vector<std::uint64_t> seen;
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
        failures += expect_eq(seen[index], static_cast<std::uint64_t>(value_size_k));
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

    st::basic_vector<inline_slot_t> key_slots;
    failures += expect(key_slots.resize(count_k) == st::status_t::success_k);
    if (failures != 0) return failures;
    for (std::size_t index = 0; index != count_k; ++index) {
        inline_slot_t slot;
        slot.stored_key = key_of(index);
        slot.size = 8;
        for (std::size_t byte = 0; byte != 8; ++byte) slot.data[byte] = painted(index, byte);
        key_slots[index] = slot;
    }

    expected<inline_run_t> written = inline_run_t::write(*volume, guard->channel(), uuid_generator(),
                                                         std::span<inline_slot_t const> {key_slots},
                                                         std::span<byte_t const> {}, 8);
    failures += expect(written.has_value());
    if (!written) return failures;

    st::basic_vector<std::uint64_t> positions;
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
    failures += expect_eq(written->size_bytes(), sorted_run_layout<inline_slot_t>::blob_bytes(count_k, 0));
    return failures;
}

#pragma endregion Round Trip Cases

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

    basic_write_pipeline<disk_type_> writer {disk, channel, medium};
    expected<pooled_buffer_t> block = writer.acquire_buffer();
    if (!block) return block.status();
    std::memset(block->bytes().data(), 0, static_cast<std::size_t>(run_config_k.page_bytes));
    std::memcpy(block->bytes().data(), &header, sizeof(header));
    if (status_t const queued = writer.enqueue_write(std::move(*block), extent_t {0, run_config_k.page_bytes});
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

    // The blob is as long as the run claims, so nothing but the medium can cut the index short.
    blob_t handed;
    handed.medium = stub;
    handed.extent = extent_t {0, variable_layout_t::blob_bytes(claimed_k, 0)};
    handed.length = handed.extent.length;
    failures += expect(handed.extent.length > stub_bytes_k);

    expected<variable_run_t> attached = variable_run_t::attach(*volume, guard->channel(), handed);
    failures += expect(!attached.has_value());
    failures += expect_status(attached, status_code_t::malformed_k);

    // A blob shorter than the run it carries is refused before a byte of the index is asked for.
    blob_t clipped = handed;
    clipped.extent = extent_t {0, run_config_k.page_bytes * 2};
    clipped.length = clipped.extent.length;
    expected<variable_run_t> narrow = variable_run_t::attach(*volume, guard->channel(), clipped);
    failures += expect(!narrow.has_value());
    failures += expect_status(narrow, status_code_t::malformed_k);

    // A blob with no header at all is refused as well, rather than read as an empty run.
    blob_t blank = handed;
    blank.extent = extent_t {run_config_k.page_bytes, run_config_k.page_bytes};
    blank.length = blank.extent.length;
    expected<variable_run_t> unwritten = variable_run_t::attach(*volume, guard->channel(), blank);
    failures += expect(!unwritten.has_value());
    failures += expect_status(unwritten, status_code_t::malformed_k);
    return failures;
}

/** A name the volume never recorded is not found, and a dropped run stops answering to its name. */
template <typename mount_type_>
std::size_t missing_run_is_not_found(mount_type_ const& mount) {
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

    uuid_t const absent = uuid_generator();
    expected<variable_run_t> missing = variable_run_t::open(*volume, guard->channel(), absent);
    failures += expect(!missing.has_value());
    failures += expect_status(missing, status_code_t::not_found_k);

    uuid_t const content_id = uuid_generator();
    variable_data_t data;
    failures += build_variable(32, [](std::size_t) { return std::int64_t {64}; }, data);
    if (failures != 0) return failures;
    std::uint64_t const free_before = volume->free_bytes();
    {
        expected<variable_run_t> written = variable_run_t::write(*volume, guard->channel(), content_id,
                                                                 std::span<variable_slot_t const> {data.key_slots},
                                                                 std::span<byte_t const> {data.values}, 4096);
        failures += expect(written.has_value());
        if (!written) return failures;
    }
    failures += expect(volume->free_bytes() < free_before);

    // A second run under the same name is refused, since a run is immutable once it is recorded.
    expected<variable_run_t> again = variable_run_t::write(*volume, guard->channel(), content_id,
                                                           std::span<variable_slot_t const> {data.key_slots},
                                                           std::span<byte_t const> {data.values}, 4096);
    failures += expect(!again.has_value());
    failures += expect_status(again, status_code_t::already_exists_k);

    failures += expect(variable_run_t::drop(*volume, guard->channel(), content_id).succeeded());
    failures += expect_eq(volume->free_bytes(), free_before);
    expected<variable_run_t> dropped = variable_run_t::open(*volume, guard->channel(), content_id);
    failures += expect(!dropped.has_value());
    failures += expect_status(dropped, status_code_t::not_found_k);
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
    expected<variable_run_t> run = variable_run_t::write(*volume, guard->channel(), uuid_generator(),
                                                         std::span<variable_slot_t const> {data.key_slots},
                                                         std::span<byte_t const> {data.values}, 4096);
    failures += expect(run.has_value());
    if (!run) return failures;

    for (std::size_t index = 0; index != count_k; ++index) {
        std::uint64_t position = 0;
        failures += expect(run->find(key_of(index), position) == find_result_t::success_k);
        failures += expect_eq(position, static_cast<std::uint64_t>(index));
        failures += expect_eq(run->key_slot(position).stored_key, key_of(index));
    }
    for (std::size_t index = 0; index != count_k; ++index) {
        std::uint64_t position = 0;
        failures += expect(run->find(key_of(index) + 1, position) == find_result_t::nothing_k);
        failures += expect_eq(position, static_cast<std::uint64_t>(count_k));
    }

    // Keys outside the run's bounds miss as cleanly as keys inside the gaps do.
    std::uint64_t position = 0;
    failures += expect(run->find(0, position) == find_result_t::nothing_k);
    failures += expect(run->find(key_of(count_k) + 1000, position) == find_result_t::nothing_k);

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
    uuid_t const one_id = uuid_generator();
    expected<variable_run_t> one = variable_run_t::write(*volume, guard->channel(), one_id,
                                                         std::span<variable_slot_t const> {alone.key_slots},
                                                         std::span<byte_t const> {alone.values}, 4096);
    failures += expect(one.has_value());
    if (!one) return failures;
    failures += expect_eq(one->population_count(), std::uint64_t {1});
    failures += expect_eq(one->lower_stored_key(), key_of(0));
    failures += expect_eq(one->upper_stored_key(), key_of(0));
    std::uint64_t position = 0;
    failures += expect(one->find(key_of(0), position) == find_result_t::success_k);
    failures += expect_eq(position, std::uint64_t {0});
    failures += expect(one->find(key_of(1), position) == find_result_t::nothing_k);
    failures += check_variable_values(*one, guard->channel(), alone);

    variable_data_t none;
    failures += build_variable(0, [](std::size_t) { return std::int64_t {0}; }, none);
    uuid_t const empty_id = uuid_generator();
    expected<variable_run_t> empty = variable_run_t::write(*volume, guard->channel(), empty_id,
                                                           std::span<variable_slot_t const> {none.key_slots},
                                                           std::span<byte_t const> {none.values}, 4096);
    failures += expect(empty.has_value());
    if (!empty) return failures;
    failures += expect_eq(empty->population_count(), std::uint64_t {0});
    failures += expect_eq(empty->size_bytes(), variable_layout_t::header_bytes());
    failures += expect(empty->lower_stored_key() == limit<stored_key_t>::max());
    failures += expect(empty->upper_stored_key() == limit<stored_key_t>::min());
    failures += expect(empty->find(key_of(0), position) == find_result_t::nothing_k);
    failures += expect_eq(position, std::uint64_t {0});
    failures += check_variable_values(*empty, guard->channel(), none);

    // Both survive a reopen, which is where a zero-length index would have been read anyway.
    expected<variable_run_t> reopened_one = variable_run_t::open(*volume, guard->channel(), one_id);
    failures += expect(reopened_one.has_value());
    expected<variable_run_t> reopened_empty = variable_run_t::open(*volume, guard->channel(), empty_id);
    failures += expect(reopened_empty.has_value());
    if (reopened_empty) failures += expect_eq(reopened_empty->population_count(), std::uint64_t {0});
    return failures;
}

/** Values at both ends of what the layout admits read back whole, across as many blocks as they need. */
template <typename mount_type_>
std::size_t values_at_the_size_extremes(mount_type_ const& mount) {
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

    expected<variable_run_t> run = variable_run_t::write(*volume, guard->channel(), uuid_generator(),
                                                         std::span<variable_slot_t const> {data.key_slots},
                                                         std::span<byte_t const> {data.values}, largest_k);
    failures += expect(run.has_value());
    if (!run) return failures;
    failures += check_variable_values(*run, guard->channel(), data);

    // The widest value needs more than one block, so its window is wider than the channel's block.
    std::uint64_t position = 0;
    failures += expect(run->find(key_of(3), position) == find_result_t::success_k);
    value_window_t const widest = run->value_window(position);
    failures += expect(widest.extent.length > run_config_k.request_buffer_bytes);
    failures += expect_eq(static_cast<std::uint64_t>(widest.bytes), static_cast<std::uint64_t>(largest_k));

    // A present but empty value needs no read at all, and says so with an empty window.
    failures += expect(run->find(key_of(1), position) == find_result_t::success_k);
    failures += expect_eq(run->value_window(position).extent.length, std::uint64_t {0});
    return failures;
}

/** A tombstone is a key the run carries and a value it does not, at every layer that reports it. */
template <typename mount_type_>
std::size_t tombstones_read_back_as_tombstones(mount_type_ const& mount) {
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

    // Every third key is erased, every fifth holds a value that is present and empty.
    auto sized = [](std::size_t index) -> std::int64_t {
        if (index % 3 == 0) return -1;
        if (index % 5 == 0) return 0;
        return 1 + static_cast<std::int64_t>(index % 613);
    };
    variable_data_t data;
    failures += build_variable(count_k, sized, data);
    if (failures != 0) return failures;

    uuid_t const content_id = uuid_generator();
    {
        expected<variable_run_t> written = variable_run_t::write(*volume, guard->channel(), content_id,
                                                                 std::span<variable_slot_t const> {data.key_slots},
                                                                 std::span<byte_t const> {data.values}, 4096);
        failures += expect(written.has_value());
        if (!written) return failures;
    }
    expected<variable_run_t> run = variable_run_t::open(*volume, guard->channel(), content_id);
    failures += expect(run.has_value());
    if (!run) return failures;

    for (std::size_t index = 0; index != count_k; ++index) {
        std::uint64_t position = 0;
        find_result_t const found = run->find(key_of(index), position);
        if (sized(index) < 0) {
            failures += expect(found == find_result_t::stored_key_k);
            failures += expect(run->key_slot(position).is_tombstone());
            failures += expect_eq(run->value_window(position).extent.length, std::uint64_t {0});
        }
        else {
            failures += expect(found == find_result_t::success_k);
            failures += expect(!run->key_slot(position).is_tombstone());
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
    expected<variable_run_t> run = variable_run_t::write(*volume, guard->channel(), uuid_generator(),
                                                         std::span<variable_slot_t const> {data.key_slots},
                                                         std::span<byte_t const> {data.values}, 4096);
    failures += expect(run.has_value());
    if (!run) return failures;

    read_pipeline_t reader {(*fixture)->disk, guard->channel(), run->blob().medium};
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
    st::basic_vector<std::uint64_t> positions;
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

} // namespace unum::ustore

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;

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
        report(totals, mount.label("reads_back_every_value").c_str(), run_reads_back_every_value(mount));
        report(totals, mount.label("fixed_width_reads_back").c_str(), fixed_width_run_reads_back(mount));
        report(totals, mount.label("inline_needs_no_reads").c_str(), inline_run_needs_no_reads(mount));
        report(totals, mount.label("truncated_index").c_str(), truncated_index_is_malformed(mount));
        report(totals, mount.label("missing_is_not_found").c_str(), missing_run_is_not_found(mount));
        report(totals, mount.label("point_lookup").c_str(), point_lookup_finds_and_misses(mount));
        report(totals, mount.label("smallest_sizes").c_str(), runs_at_the_smallest_sizes(mount));
        report(totals, mount.label("value_size_extremes").c_str(), values_at_the_size_extremes(mount));
        report(totals, mount.label("tombstones").c_str(), tombstones_read_back_as_tombstones(mount));
        report(totals, mount.label("holds_its_depth").c_str(), batched_read_holds_its_depth(mount));
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, run_config_k, 1, "run", cases);

    return suite_exit_code(totals);
}
