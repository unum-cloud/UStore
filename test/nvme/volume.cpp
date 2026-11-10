/**
 *  @file test/nvme/volume.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief Both volumes: run files named by a @c .ustore.json manifest, and 1 GB slots of
 *      a raw medium.
 *
 *  The results this file exists for are the two commits: a manifest copy written without its
 *  rename, or a JSON region written without its owner block, leaves the old generation in force,
 *  and every run the manifest in force does not name comes back free at the next mount. A regular
 *  file stands in for a block device, sparse, so four slots cost no disk.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstdio>  // `std::fopen`, `std::fwrite`, `std::fseek`, `std::fclose`
#include <cstring> // `std::memcpy`

#include <sys/resource.h> // `setrlimit`, `getrlimit`, `RLIMIT_NOFILE`, `RLIMIT_MEMLOCK`
#include <sys/wait.h>     // `waitpid`, `WIFEXITED`, `WEXITSTATUS`
#include <unistd.h>       // `fork`, `_exit`

#include <array>       // `std::array`
#include <filesystem>  // `std::filesystem::resize_file`, `std::filesystem::exists`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/nvme_manifest.hpp" // `manifest_file_t`
#include "ustore/utilities/nvme_mounts.hpp"   // `io_transport_t`, `probed_transport`, `addresses_of`
#include "ustore/utilities/nvme_slots.hpp"    // `slot_bytes_k`
#include "ustore/utilities/nvme_volume.hpp"   // `basic_slotted_volume`

#include "nvme/fixture.hpp" // `run_on_transport`, `painted`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Shape

/** The numbers every case mounts with: two channels, and no raw medium of the fixture's own. */
constexpr fixture_config_t volume_config_k {
    .stream_buffers = 2,
    .runs_capacity = 64,
    .shared_io_channels = 2,
    .memory_limit = std::size_t {64} * 1024 * 1024,
};

/** Slots every stand-in device holds, the manifest slot included. */
constexpr std::uint64_t stand_in_slots_k = 4;

/** Bytes a small run of a case takes, which a packed slot holds many of. */
constexpr std::uint64_t small_run_bytes_k = 3 * portable_alignment_k + 100;

#pragma endregion Shape

#pragma region Helpers

/** Paints @p bytes from @p seed, which a read back compares every byte against. */
inline void paint(std::span<byte_t> bytes, std::uint64_t seed) noexcept {
    for (std::size_t position = 0; position != bytes.size(); ++position) bytes[position] = painted(seed, position);
}

/** Whether @p bytes hold what @c paint wrote from @p seed. */
[[nodiscard]] inline bool holds_paint(std::span<byte_t const> bytes, std::uint64_t seed) noexcept {
    for (std::size_t position = 0; position != bytes.size(); ++position)
        if (bytes[position] != painted(seed, position)) return false;
    return true;
}

/** Allocates @p bytes for a fresh run on @p volume and paints them from @p seed, answering
 *  where they lie. */
template <typename fixture_type_, typename volume_type_>
expected<medium_extent_t> write_painted(fixture_type_& fixture, volume_type_& volume, io_channel_t& channel,
                                        uuid128_t const& run, std::uint64_t bytes, std::uint64_t seed) noexcept {
    auto [extent, allocate_status] = volume.allocate(run, bytes);
    if (allocate_status.failed()) return allocate_status;
    st::vector<byte_t> painted_bytes;
    if (painted_bytes.resize(static_cast<std::size_t>(bytes)) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "No room for a painted run"};
    paint(painted_bytes, seed);
    if (status_t const written = write_extent<typename fixture_type_::disk_t, waiter_t>(
            fixture.disk, channel, extent, std::span<byte_t const> {painted_bytes});
        written.failed())
        return written;
    return extent;
}

/** Reads @p bytes of @p extent back and answers whether they hold what @p seed painted. */
template <typename fixture_type_>
[[nodiscard]] bool reads_painted(fixture_type_& fixture, io_channel_t& channel, medium_extent_t const& extent,
                                 std::uint64_t bytes, std::uint64_t seed) noexcept {
    st::vector<byte_t> read;
    if (read.resize(static_cast<std::size_t>(bytes)) != st::status_t::success_k) return false;
    if (read_extent<typename fixture_type_::disk_t>(fixture.disk, channel, extent, std::span<byte_t> {read}).failed())
        return false;
    return holds_paint(read, seed);
}

/** Creates the sparse file @p name under the fixture's directory, standing in for a device of
 *  @p slots slots. */
template <typename fixture_type_>
path_t stand_in_device(fixture_type_ const& fixture, char const* name, std::uint64_t slots) noexcept {
    path_t const path {fixture.directory, name};
    std::FILE* const file = std::fopen(path.c_str(), "wb");
    if (file) std::fclose(file);
    std::error_code error;
    std::filesystem::resize_file(path.c_str(), slots * slot_bytes_k, error);
    return path;
}

/** Writes @p block over the first page at @p offset of the stand-in device @p path, as
 *  another tool would. */
inline bool overwrite_page(path_t const& path, std::uint64_t offset, std::span<byte_t const> block) noexcept {
    std::FILE* const file = std::fopen(path.c_str(), "r+b");
    if (!file) return false;
    bool const written = std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 &&
                         std::fwrite(block.data(), 1, block.size(), file) == block.size();
    return std::fclose(file) == 0 && written;
}

/** The address of @p path from @p base on, as a data directory names a raw medium. */
struct raw_address_t {
    char text[path_capacity_k + 32] {};
    raw_address_t(path_t const& path, std::uint64_t base) noexcept {
        auto const written = fmt::format_to_n(text, sizeof(text) - 1, "{}@{}", path.c_str(), base);
        *written.out = '\0';
    }
    [[nodiscard]] data_address_t address() const noexcept { return *parse_data_address(text); }
};

/** Mounts a slotted volume over @p addresses on the fixture's disk. */
template <typename fixture_type_>
auto mount_slotted(fixture_type_& fixture, io_channel_t& channel, std::span<data_address_t const> addresses) noexcept {
    using volume_t = basic_slotted_volume<typename fixture_type_::disk_t, waiter_t, volume_allocator_t>;
    volume_config_t config;
    config.database = string_view_t {"raw"};
    config.data_directories = addresses;
    return volume_t::mount(fixture.disk, channel, config, volume_allocator_t {fixture.resources->memory_constraint});
}

#pragma endregion Helpers

#pragma region Directory Cases

/** A run named by a published manifest reads back after a remount, and one never named is
 *  unlinked by it. */
template <typename fixture_type_>
case_result_t directory_keeps_named_and_reclaims_leftovers(fixture_type_& fixture) {
    if (!fixture.persists_volumes()) return case_result_t::skipped(memory_volumes_vanish_k);
    std::size_t failures = 0;
    expected<typename fixture_type_::guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    io_channel_t& channel = guard->channel();

    uuid128_t const kept = uuid_generator(), leftover = uuid_generator();
    path_t leftover_path {"", ""};
    {
        auto [volume, mount_status] = fixture.mount_volume(channel);
        failures += expect(mount_status.succeeded());
        if (mount_status.failed()) return failures;
        failures += expect_eq(volume.generation(), std::uint64_t {0});
        auto [named, named_status] = write_painted(fixture, volume, channel, kept, small_run_bytes_k, 7);
        auto [unnamed, unnamed_status] = write_painted(fixture, volume, channel, leftover, small_run_bytes_k, 8);
        failures += expect(named_status.succeeded() && unnamed_status.succeeded());
        if (named_status.failed() || unnamed_status.failed()) return failures;
        manifest_file_t const files[] {file_of(named, kept)};
        failures += expect(publish_naming(volume, fixture, channel, files).succeeded());
        failures += expect_eq(volume.generation(), std::uint64_t {1});
        char name[path_max_bytes_k];
        failures += expect(volume.manifest().file_name(leftover, name).succeeded());
        leftover_path = path_t {fixture.data_directory_path(0).c_str(), name};
        failures += expect(std::filesystem::exists(leftover_path.c_str()));
    }

    auto [volume, mount_status] = fixture.mount_volume(channel);
    failures += expect(mount_status.succeeded());
    if (mount_status.failed()) return failures;
    failures += expect_eq(volume.generation(), std::uint64_t {1});
    failures += expect_eq(volume.manifest().files.size(), std::size_t {1});
    failures += expect(!std::filesystem::exists(leftover_path.c_str()));
    if (volume.manifest().files.size() != 1) return failures;
    auto [opened, open_status] = volume.open(volume.manifest().files[0]);
    failures += expect(open_status.succeeded());
    if (open_status.succeeded()) failures += expect(reads_painted(fixture, channel, opened, small_run_bytes_k, 7));
    return failures;
}

/** A manifest copy a crash left before its rename is ignored and removed, and the generation
 *  in force stays. */
template <typename fixture_type_>
case_result_t directory_ignores_an_unrenamed_copy(fixture_type_& fixture) {
    if (!fixture.persists_volumes()) return case_result_t::skipped(memory_volumes_vanish_k);
    std::size_t failures = 0;
    expected<typename fixture_type_::guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    io_channel_t& channel = guard->channel();
    {
        auto [volume, mount_status] = fixture.mount_volume(channel);
        failures += expect(mount_status.succeeded());
        if (mount_status.failed()) return failures;
        failures += expect(publish_naming(volume, fixture, channel, {}).succeeded());
    }
    std::array<char, 64> name {};
    auto const written = fmt::format_to_n(name.data(), name.size() - 1, "{}.ustore.json.tmp", fixture.suite);
    *written.out = '\0';
    path_t const copy {fixture.data_directory_path(0).c_str(), name.data()};
    std::FILE* const file = std::fopen(copy.c_str(), "wb");
    failures += expect(file != nullptr);
    if (file) {
        std::fputs("{\"generation\": 9", file);
        std::fclose(file);
    }

    auto [volume, mount_status] = fixture.mount_volume(channel);
    failures += expect(mount_status.succeeded());
    if (mount_status.succeeded()) failures += expect_eq(volume.generation(), std::uint64_t {1});
    failures += expect(!std::filesystem::exists(copy.c_str()));
    return failures;
}

/** Runs land on the data directory with the most free bytes, and a quota that leaves no
 *  room refuses them. */
template <typename fixture_type_>
std::size_t directory_places_by_free_space(fixture_type_& fixture) {
    using volume_t = typename fixture_type_::volume_t;
    std::size_t failures = 0;
    expected<typename fixture_type_::guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    io_channel_t& channel = guard->channel();

    path_t const first = fixture.data_directory_path(0), second = fixture.data_directory_path(1);
    std::array<data_address_t, 2> addresses {};
    addresses[0].location = first.c_str();
    addresses[0].byte_limit = 4 * small_run_bytes_k;
    addresses[1].location = second.c_str();
    addresses[1].byte_limit = 8 * small_run_bytes_k;
    volume_config_t config;
    config.database = string_view_t {fixture.suite};
    config.data_directories = addresses;
    config.runs_capacity = volume_config_k.runs_capacity;
    auto [volume, mount_status] = volume_t::mount(fixture.disk, channel, config,
                                                  volume_allocator_t {fixture.resources->memory_constraint});
    failures += expect(mount_status.succeeded());
    if (mount_status.failed()) return failures;

    std::array<std::size_t, 2> placed {};
    std::uint64_t const rounded = round_up_to_multiple(small_run_bytes_k, portable_alignment_k);
    std::size_t const fits = static_cast<std::size_t>((addresses[0].byte_limit / rounded) +
                                                      (addresses[1].byte_limit / rounded));
    for (std::size_t index = 0; index != fits; ++index) {
        auto [extent, allocate_status] = volume.allocate(uuid_generator(), small_run_bytes_k);
        failures += expect(allocate_status.succeeded());
        if (allocate_status.failed()) return failures;
        ++placed[extent.data_directory];
    }
    failures += expect_eq(placed[0], static_cast<std::size_t>(addresses[0].byte_limit / rounded));
    failures += expect_eq(placed[1], static_cast<std::size_t>(addresses[1].byte_limit / rounded));
    failures += expect_status(volume.allocate(uuid_generator(), small_run_bytes_k),
                              status_code_t::capacity_exhausted_k);
    return failures;
}

/** Runs allocated back to back over uncapped directories of one filesystem spread one per
 *  directory, since a run claims its bytes before its sparse file does. */
template <typename fixture_type_>
std::size_t directory_spreads_unwritten_runs(fixture_type_& fixture) {
    constexpr std::size_t directories_k = 4;
    // Large enough that another writer on the filesystem cannot move its free bytes that far
    // between two probes.
    constexpr std::uint64_t run_bytes_k = 16ull * 1024ull * 1024ull;
    std::size_t failures = 0;
    expected<typename fixture_type_::guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    io_channel_t& channel = guard->channel();
    auto [volume, mount_status] = fixture.mount_volume(channel, directories_k);
    failures += expect(mount_status.succeeded());
    if (mount_status.failed()) return failures;

    std::array<std::size_t, directories_k> placed {};
    for (std::size_t index = 0; index != directories_k; ++index) {
        auto [extent, allocate_status] = volume.allocate(uuid_generator(), run_bytes_k);
        failures += expect(allocate_status.succeeded());
        if (allocate_status.failed()) return failures;
        ++placed[extent.data_directory];
    }
    for (std::size_t const runs : placed) failures += expect_eq(runs, std::size_t {1});
    return failures;
}

/** A second mount of the directories a live volume holds is refused, since its sweep would
 *  unlink the first mount's unpublished runs, and mounts once the first is gone. */
template <typename fixture_type_>
std::size_t directory_refuses_a_second_mount(fixture_type_& fixture) {
    std::size_t failures = 0;
    expected<typename fixture_type_::guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    io_channel_t& channel = guard->channel();
    {
        auto [volume, mount_status] = fixture.mount_volume(channel);
        failures += expect(mount_status.succeeded());
        if (mount_status.failed()) return failures;
        failures += expect(volume.allocate(uuid_generator(), small_run_bytes_k).has_value());
        failures += expect_status(fixture.mount_volume(channel), status_code_t::contended_k);
    }
    failures += expect(fixture.mount_volume(channel).has_value());
    return failures;
}

#pragma endregion Directory Cases

#pragma region Slotted Cases

/** A slot a published manifest names stays, a slot written but never named is reclaimed at the next
 *  mount, and a zero block reads as free. */
template <typename fixture_type_>
std::size_t slotted_keeps_named_and_reclaims_leftovers(fixture_type_& fixture) {
    std::size_t failures = 0;
    expected<typename fixture_type_::guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    io_channel_t& channel = guard->channel();
    path_t const device = stand_in_device(fixture, "leftovers.raw", stand_in_slots_k);
    raw_address_t const raw {device, 0};
    data_address_t const addresses[] {raw.address()};

    medium_extent_t kept {};
    {
        auto [volume, mount_status] = mount_slotted(fixture, channel, addresses);
        failures += expect(mount_status.succeeded());
        if (mount_status.failed()) return failures;
        // The manifest slot is held, and every other slot is free.
        failures += expect_eq(volume.free_bytes(), (stand_in_slots_k - 1) * sorted_run_bytes_max_k);
        auto [packed, packed_status] = write_painted(fixture, volume, channel, uuid128_t {}, small_run_bytes_k, 11);
        auto [whole, whole_status] = volume.allocate(uuid128_t {}, packed_run_bytes_max_k);
        failures += expect(packed_status.succeeded() && whole_status.succeeded());
        if (packed_status.failed() || whole_status.failed()) return failures;
        failures += expect(packed.extent.offset / slot_bytes_k != whole.extent.offset / slot_bytes_k);
        kept = packed;
        manifest_file_t const files[] {file_of(packed, uuid128_t {})};
        failures += expect(publish_naming(volume, fixture, channel, files).succeeded());
    }

    auto [volume, mount_status] = mount_slotted(fixture, channel, addresses);
    failures += expect(mount_status.succeeded());
    if (mount_status.failed()) return failures;
    failures += expect_eq(volume.generation(), std::uint64_t {1});
    // The whole-slot run was never named, so its slot came back; the packed slot stays
    // held and open.
    std::uint64_t const packed_tail = slot_payload_offset_k +
                                      round_up_to_multiple(small_run_bytes_k, portable_alignment_k);
    failures += expect_eq(volume.free_bytes(),
                          (stand_in_slots_k - 2) * sorted_run_bytes_max_k + (slot_bytes_k - packed_tail));
    if (volume.manifest().files.size() != 1) return failures + expect(false);
    auto [opened, open_status] = volume.open(volume.manifest().files[0]);
    failures += expect(open_status.succeeded());
    if (open_status.succeeded()) {
        failures += expect_eq(opened.extent.offset, kept.extent.offset);
        failures += expect(reads_painted(fixture, channel, opened, small_run_bytes_k, 11));
    }
    // A small run after the mount packs behind the kept one rather than claiming a fresh slot.
    auto [next, next_status] = volume.allocate(uuid128_t {}, small_run_bytes_k);
    failures += expect(next_status.succeeded());
    if (next_status.succeeded()) failures += expect_eq(next.extent.offset, kept.extent.offset + kept.extent.length);
    return failures;
}

/** A run given back after the manifest stopped naming it reads as zeros from its first block once
 *  the next publication lands, while the run packed beside it in the same slot stays whole. */
template <typename fixture_type_>
std::size_t slotted_zeroes_a_dropped_run(fixture_type_& fixture) {
    std::size_t failures = 0;
    expected<typename fixture_type_::guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    io_channel_t& channel = guard->channel();
    path_t const device = stand_in_device(fixture, "dropped.raw", stand_in_slots_k);
    raw_address_t const raw {device, 0};
    data_address_t const addresses[] {raw.address()};

    auto [volume, mount_status] = mount_slotted(fixture, channel, addresses);
    failures += expect(mount_status.succeeded());
    if (mount_status.failed()) return failures;
    auto [dropped, dropped_status] = write_painted(fixture, volume, channel, uuid_generator(), small_run_bytes_k, 41);
    auto [kept, kept_status] = write_painted(fixture, volume, channel, uuid_generator(), small_run_bytes_k, 42);
    failures += expect(dropped_status.succeeded() && kept_status.succeeded());
    if (dropped_status.failed() || kept_status.failed()) return failures;
    failures += expect_eq(dropped.extent.offset / slot_bytes_k, kept.extent.offset / slot_bytes_k);
    manifest_file_t const both[] {file_of(dropped, uuid128_t {}), file_of(kept, uuid128_t {})};
    failures += expect(publish_naming(volume, fixture, channel, both).succeeded());

    // The manifest stops naming the run, and only once it is durable is the run given back.
    manifest_file_t const one[] {file_of(kept, uuid128_t {})};
    failures += expect(publish_naming(volume, fixture, channel, one).succeeded());
    volume.deallocate(dropped);
    failures += expect(publish_naming(volume, fixture, channel, one).succeeded());

    std::array<byte_t, portable_alignment_k> zeros {};
    st::vector<byte_t> read;
    failures += expect(read.resize(portable_alignment_k) == st::status_t::success_k);
    failures += expect(
        read_extent<typename fixture_type_::disk_t>(fixture.disk, channel, dropped, std::span<byte_t> {read})
            .succeeded());
    failures += expect(std::memcmp(read.data(), zeros.data(), zeros.size()) == 0);
    failures += expect(reads_painted(fixture, channel, kept, small_run_bytes_k, 42));
    return failures;
}

/** A JSON copy written into the idle region without the owner block that commits it leaves the old
 *  generation in force, as a crash between the two flushes would. */
template <typename fixture_type_>
std::size_t slotted_ignores_an_uncommitted_copy(fixture_type_& fixture) {
    using volume_t = basic_slotted_volume<typename fixture_type_::disk_t, waiter_t, volume_allocator_t>;
    std::size_t failures = 0;
    expected<typename fixture_type_::guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    io_channel_t& channel = guard->channel();
    path_t const device = stand_in_device(fixture, "uncommitted.raw", stand_in_slots_k);
    raw_address_t const raw {device, 0};
    data_address_t const addresses[] {raw.address()};
    {
        auto [volume, mount_status] = mount_slotted(fixture, channel, addresses);
        failures += expect(mount_status.succeeded());
        if (mount_status.failed()) return failures;
        failures += expect(publish_naming(volume, fixture, channel, {}).succeeded());
    }
    // The first publication took region zero, so the next would write region one.
    std::array<byte_t, portable_alignment_k> copy {};
    char const text[] = "{\"generation\": 2}";
    std::memcpy(copy.data(), text, sizeof(text) - 1);
    failures += expect(overwrite_page(device, slot_payload_offset_k + volume_t::manifest_region_bytes_k, copy));

    auto [volume, mount_status] = mount_slotted(fixture, channel, addresses);
    failures += expect(mount_status.succeeded());
    if (mount_status.succeeded()) failures += expect_eq(volume.generation(), std::uint64_t {1});
    return failures;
}

/** Two databases share one medium from two bases, each skipping the other's slots as foreign; a
 *  foreign magic in a slot is skipped, and one in the base slot refuses the mount. */
template <typename fixture_type_>
std::size_t slotted_shares_a_medium(fixture_type_& fixture) {
    std::size_t failures = 0;
    expected<typename fixture_type_::guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    io_channel_t& channel = guard->channel();
    path_t const device = stand_in_device(fixture, "shared.raw", stand_in_slots_k + 1);
    raw_address_t const first_raw {device, 0}, second_raw {device, slot_bytes_k};
    data_address_t const first[] {first_raw.address()}, second[] {second_raw.address()};

    // Slot 4 of the first database holds a SafeGraphs index, which neither database may take.
    std::array<byte_t, portable_alignment_k> foreign {};
    std::uint64_t const safegraphs_magic = magic_of("SAFEGRPH");
    std::memcpy(foreign.data(), &safegraphs_magic, sizeof(safegraphs_magic));
    failures += expect(overwrite_page(device, stand_in_slots_k * slot_bytes_k, foreign));
    {
        auto [volume, mount_status] = mount_slotted(fixture, channel, first);
        failures += expect(mount_status.succeeded());
        if (mount_status.failed()) return failures;
        failures += expect_eq(volume.free_bytes(), (stand_in_slots_k - 1) * sorted_run_bytes_max_k);
        failures += expect(publish_naming(volume, fixture, channel, {}).succeeded());
        // Another mount of the same medium waits for this one, as another process claiming
        // its slots would.
        failures += expect_status(mount_slotted(fixture, channel, second), status_code_t::contended_k);
    }
    {
        // The second database's base slot is the first's slot 1, free until now.
        auto [volume, mount_status] = mount_slotted(fixture, channel, second);
        failures += expect(mount_status.succeeded());
        if (mount_status.failed()) return failures;
        auto [run, run_status] = write_painted(fixture, volume, channel, uuid128_t {}, small_run_bytes_k, 21);
        failures += expect(run_status.succeeded());
        if (run_status.failed()) return failures;
        failures += expect_eq(run.extent.offset / slot_bytes_k, std::uint64_t {2});
        manifest_file_t const files[] {file_of(run, uuid128_t {})};
        failures += expect(publish_naming(volume, fixture, channel, files).succeeded());
    }
    {
        auto [volume, mount_status] = mount_slotted(fixture, channel, first);
        failures += expect(mount_status.succeeded());
        if (mount_status.failed()) return failures;
        failures += expect_eq(volume.free_bytes(), std::uint64_t {1} * sorted_run_bytes_max_k);
        auto [run, run_status] = volume.allocate(uuid128_t {}, small_run_bytes_k);
        failures += expect(run_status.succeeded());
        if (run_status.succeeded()) failures += expect_eq(run.extent.offset / slot_bytes_k, std::uint64_t {3});
    }
    {
        auto [volume, mount_status] = mount_slotted(fixture, channel, second);
        failures += expect(mount_status.succeeded());
        if (mount_status.succeeded()) failures += expect_eq(volume.manifest().files.size(), std::size_t {1});
    }
    raw_address_t const foreign_raw {device, stand_in_slots_k * slot_bytes_k};
    data_address_t const foreign_base[] {foreign_raw.address()};
    std::size_t const refused = expect_status(mount_slotted(fixture, channel, foreign_base),
                                              status_code_t::already_exists_k);
    return failures + refused;
}

/** Data directories listed in another order than the database was created with are refused. */
template <typename fixture_type_>
std::size_t slotted_refuses_reordered_media(fixture_type_& fixture) {
    std::size_t failures = 0;
    expected<typename fixture_type_::guard_t> guard = fixture.channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    io_channel_t& channel = guard->channel();
    path_t const smaller = stand_in_device(fixture, "first.raw", 2), larger = stand_in_device(fixture, "second.raw", 3);
    raw_address_t const smaller_raw {smaller, 0}, larger_raw {larger, 0};
    data_address_t const ordered[] {smaller_raw.address(), larger_raw.address()};
    data_address_t const reordered[] {larger_raw.address(), smaller_raw.address()};
    {
        auto [volume, mount_status] = mount_slotted(fixture, channel, ordered);
        failures += expect(mount_status.succeeded());
        if (mount_status.failed()) return failures;
        auto [run, run_status] = write_painted(fixture, volume, channel, uuid128_t {}, small_run_bytes_k, 31);
        failures += expect(run_status.succeeded());
        if (run_status.failed()) return failures;
        failures += expect_eq(run.data_directory, std::uint16_t {1});
        manifest_file_t const files[] {file_of(run, uuid128_t {})};
        failures += expect(publish_naming(volume, fixture, channel, files).succeeded());
    }
    failures += expect_status(mount_slotted(fixture, channel, reordered), status_code_t::malformed_k);
    auto [volume, mount_status] = mount_slotted(fixture, channel, ordered);
    failures += expect(mount_status.succeeded());
    return failures;
}

#pragma endregion Slotted Cases

#pragma region Addresses

/** Data directory addresses read as the forms they name, and a manifest path names
 *  data directory 0. */
std::size_t addresses_read_their_forms() {
    std::size_t failures = 0;
    auto [directory, directory_status] = parse_data_address("/mnt/nvme0/molecules");
    failures += expect(directory_status.succeeded() && directory.kind == data_address_kind_t::directory_k);
    auto [device, device_status] = parse_data_address("/dev/nvme0n1@64GiB+16GiB");
    failures += expect(device_status.succeeded() && device.kind == data_address_kind_t::raw_k);
    failures += expect_eq(device.byte_base, std::uint64_t {64} << 30);
    failures += expect_eq(device.byte_limit, std::uint64_t {16} << 30);
    failures += expect(device.location == "/dev/nvme0n1");
    auto [spdk, spdk_status] = parse_data_address("spdk://0000:5e:00.0/2@0GiB");
    failures += expect(spdk_status.succeeded() && spdk.kind == data_address_kind_t::spdk_k);
    failures += expect_eq(spdk.namespace_id, std::uint32_t {2});
    failures += expect_status(parse_data_address("/dev/nvme0n1@64XiB"), status_code_t::invalid_argument_k);

    resources_config_t config;
    [[maybe_unused]] sz::status_t const assigned = config.manifest.assign(
        string_view_t {"/data/molecules.ustore.json"});
    auto [addresses, addresses_status] = addresses_of(config);
    failures += expect(addresses_status.succeeded());
    if (addresses_status.succeeded()) {
        failures += expect(addresses.database == string_view_t {"molecules"});
        failures += expect(addresses.data_directories[0].location == "/data");
    }
    return failures;
}

/** io_uring named outright never falls back, while the automatic choice does, and a kernel refusing
 *  a setup flag surfaces as unsupported with the Linux version the transport needs. */
std::size_t probed_transport_refuses_a_missing_ring() {
    std::size_t failures = 0;
    for (linux_uring_probe_t const missing : {linux_uring_probe_t::refused_k, linux_uring_probe_t::too_old_k})
        failures += expect_status(
            probed_transport(io_transport_t::linux_uring_k, missing, linux_aio_probe_t::available_k),
            status_code_t::unsupported_k);
    // A sandbox refusing every ring answers refused_k for any flag, so only a machine with rings
    // can tell a refused one
    if (linux_uring_files_disk_t::probe() == linux_uring_probe_t::available_k) {
        linux_uring_probe_t const refused_flag = linux_uring_files_disk_t::probe(std::uint32_t {1} << 31);
        failures += expect(refused_flag == linux_uring_probe_t::too_old_k);
        auto [old, old_status] = probed_transport(io_transport_t::linux_uring_k, refused_flag,
                                                  linux_aio_probe_t::available_k);
        failures += expect(std::string_view {old_status.message}.find("Linux 6.12") != std::string_view::npos);
    }
    auto [automatic, automatic_status] = probed_transport(io_transport_t::automatic_k, linux_uring_probe_t::too_old_k,
                                                          linux_aio_probe_t::available_k);
    failures += expect(automatic_status.succeeded());
    failures += expect(automatic == io_transport_t::linux_aio_k);
    return failures;
}

#pragma endregion Addresses

#pragma region Mount Limits

/** A mount sizes its file table to the descriptor limit, and refuses a limit leaving no room for a
 *  run file; each mount runs in a child, since a lowered hard limit never rises again. */
std::size_t mount_fits_the_descriptor_limit(char const* directory) {
    path_t const manifest {directory, "descriptor_limit/limits.ustore.json"};
    auto const mounts_under = [&](rlim_t descriptors, status_code_t wanted) -> std::size_t {
        pid_t const child = ::fork();
        if (child == 0) {
            struct rlimit const limit {descriptors, descriptors};
            if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) ::_exit(1);
            resources_config_t config;
            config.threads = 1;
            if (sz::failed(config.manifest.assign(string_view_t {manifest.c_str()}))) ::_exit(1);
            expected<resources_shared_ptr_t> opened = open_resources<atomic_reference, waiter_t>(config);
            if (!opened) ::_exit(opened.status().code == wanted ? 0 : 1);
            std::size_t runs = 0;
            [[maybe_unused]] status_t const visited = (*opened)->storage.visit([&](auto& mount) noexcept {
                runs = mount.volume().runs_capacity();
                return status_t {};
            });
            ::_exit(wanted == status_code_t::success_k && runs != 0 && runs < descriptors ? 0 : 1);
        }
        int status = 0;
        bool const exited = child > 0 && ::waitpid(child, &status, 0) == child && WIFEXITED(status);
        return expect(exited && WEXITSTATUS(status) == 0);
    };
    std::size_t failures = 0;
    failures += mounts_under(64, status_code_t::success_k);
    failures += mounts_under(4, status_code_t::capacity_exhausted_k);
    return failures;
}

/** A mount whose channel arena outgrows the gibibyte io_uring pins per table slot still moves its
 *  last block, pinned wherever the locked-memory cap allows. */
std::size_t mount_moves_an_arena_past_a_gibibyte(char const* directory) {
    std::size_t failures = 0;
    path_t const manifest {directory, "wide_arena/wide.ustore.json"};
    path_t const medium_path {directory, "wide_arena/medium.bin"};
    resources_config_t config;
    config.threads = 1;
    config.shared_io_channels = 7;
    config.channels.request_buffer_bytes = std::uint64_t {1} << 20;
    if (sz::failed(config.manifest.assign(string_view_t {manifest.c_str()}))) return expect(false);
    expected<resources_shared_ptr_t> opened = open_resources<atomic_reference, waiter_t>(config);
    failures += expect_status(opened, status_code_t::success_k);
    if (!opened) return failures;
    mount_report_t const report = (*opened)->storage.report();
    failures += expect(report.arena_bytes > linux_uring_registered_range_bytes_k);
    status_t const moved = (*opened)->storage.visit([&](auto& mount) noexcept -> status_t {
        auto [guard, acquire_status] = mount.channels().acquire(0);
        if (acquire_status.failed()) return acquire_status;
        medium_options_t request;
        request.name = medium_path.c_str();
        request.access = medium_access_t::create_read_write_k;
        request.length = portable_alignment_k;
        auto [medium, register_status] = mount.disk().register_medium(request);
        if (register_status.failed()) return register_status;
        queue_pair_t const& queue_pair = guard.channel().queue_pair();
        std::uint64_t const last_block = mount.arena().size() - portable_alignment_k;
        std::array<request_t, 1> const writes {write_request(medium.reference(), extent_t {0, portable_alignment_k},
                                                             mount.arena().reference(last_block), queue_pair.token(0))};
        std::array<completion_t, 1> completions {};
        expected<std::size_t> const submitted = mount.disk().submit(queue_pair, std::span<request_t const> {writes});
        expected<std::size_t> const waited = wait_completions<waiter_t>(mount.disk(), queue_pair,
                                                                        std::span<completion_t> {completions});
        status_t const released = mount.disk().unregister_medium(medium);
        if (!submitted || *submitted != 1 || !waited || *waited != 1 || !completions[0].completed_in_full())
            return {status_code_t::io_k, "The arena's last block would not move"};
        return released;
    });
    failures += expect_status(moved, status_code_t::success_k);
    struct rlimit locked {};
    if (report.transport == io_transport_t::linux_uring_k && ::getrlimit(RLIMIT_MEMLOCK, &locked) == 0 &&
        locked.rlim_cur == RLIM_INFINITY)
        failures += expect((*opened)->storage.pinning() == range_pinning_t::pinned_k);
    return failures;
}

#pragma endregion Mount Limits

#pragma region Drivers

/** What a case needs to stand in for its medium: any transport, or a file in place of a device. */
enum class needed_medium_t : bool { any_k, file_k };

/** Mounts a fixture through @p mount and runs @p body against it, skipping a case that needs a
 *  file on a transport without one. */
template <typename mount_type_, typename body_type_>
case_result_t on_fixture(mount_type_ const& mount, needed_medium_t needed, body_type_&& body) {
    if (needed == needed_medium_t::file_k &&
        (mount.transport == transport_t::memory_k || mount.transport == transport_t::device_k))
        return case_result_t::skipped("needs a file standing in for a device");
    expected<typename mount_type_::fixture_unique_ptr_t> fixture = mount();
    std::size_t const failures = expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;
    case_result_t result {body(**fixture)};
    result.failures += failures;
    return result;
}

#pragma endregion Drivers

} // namespace unum::ustore::test

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;
    using namespace unum::ustore::blobs;
    using namespace unum::ustore::utilities;

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
        auto const on = [&](char const* name, needed_medium_t needed, auto&& body) {
            case_label_t const label = mount.label(name);
            if (!totals.selects(label.c_str())) return;
            case_result_t result = on_fixture(mount, needed, body);
            result.failures += mount.take_leaks();
            report(totals, label.c_str(), result);
        };
        on("directory_reclaims_leftovers", needed_medium_t::any_k,
           [](auto& fixture) { return directory_keeps_named_and_reclaims_leftovers(fixture); });
        on("directory_unrenamed_copy", needed_medium_t::any_k,
           [](auto& fixture) { return directory_ignores_an_unrenamed_copy(fixture); });
        on("directory_free_space", needed_medium_t::any_k,
           [](auto& fixture) { return case_result_t {directory_places_by_free_space(fixture)}; });
        on("directory_spreads_unwritten_runs", needed_medium_t::any_k,
           [](auto& fixture) { return case_result_t {directory_spreads_unwritten_runs(fixture)}; });
        on("directory_refuses_a_second_mount", needed_medium_t::any_k,
           [](auto& fixture) { return case_result_t {directory_refuses_a_second_mount(fixture)}; });
        on("slotted_reclaims_leftovers", needed_medium_t::file_k,
           [](auto& fixture) { return case_result_t {slotted_keeps_named_and_reclaims_leftovers(fixture)}; });
        on("slotted_zeroes_dropped_run", needed_medium_t::file_k,
           [](auto& fixture) { return case_result_t {slotted_zeroes_a_dropped_run(fixture)}; });
        on("slotted_uncommitted_copy", needed_medium_t::file_k,
           [](auto& fixture) { return case_result_t {slotted_ignores_an_uncommitted_copy(fixture)}; });
        on("slotted_shared_medium", needed_medium_t::file_k,
           [](auto& fixture) { return case_result_t {slotted_shares_a_medium(fixture)}; });
        on("slotted_reordered_media", needed_medium_t::file_k,
           [](auto& fixture) { return case_result_t {slotted_refuses_reordered_media(fixture)}; });
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, volume_config_k, 0, "volume", cases);
    if (totals.selects("addresses")) report(totals, "addresses", addresses_read_their_forms());
    if (totals.selects("probed_transport"))
        report(totals, "probed_transport", probed_transport_refuses_a_missing_ring());
    if (environment.directory && totals.selects("descriptor_limit"))
        report(totals, "descriptor_limit", mount_fits_the_descriptor_limit(environment.directory));
    if (environment.directory && totals.selects("arena_past_a_gibibyte"))
        report(totals, "arena_past_a_gibibyte", mount_moves_an_arena_past_a_gibibyte(environment.directory));
    return suite_exit_code(totals);
}
