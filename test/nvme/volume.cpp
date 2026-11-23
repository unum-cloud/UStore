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
constexpr std::uint64_t small_run_bytes_k = 3 * util::portable_alignment_k + 100;

#pragma endregion Shape

#pragma region Helpers

/** Paints @p bytes from @p seed, which a read back compares every byte against. */
inline void paint(std::span<util::byte_t> bytes, std::uint64_t seed) noexcept {
    for (std::size_t position = 0; position != bytes.size(); ++position) bytes[position] = painted(seed, position);
}

/** Whether @p bytes hold what @c paint wrote from @p seed. */
[[nodiscard]] inline bool holds_paint(std::span<util::byte_t const> bytes, std::uint64_t seed) noexcept {
    for (std::size_t position = 0; position != bytes.size(); ++position)
        if (bytes[position] != painted(seed, position)) return false;
    return true;
}

/** Allocates @p bytes for a fresh run on @p volume and paints them from @p seed, answering
 *  where they lie. */
template <typename fixture_type_, typename volume_type_>
util::expected<util::medium_extent_t> write_painted(fixture_type_& fixture, volume_type_& volume,
                                                    util::io_channel_t& channel, util::uuid128_t const& run,
                                                    std::uint64_t bytes, std::uint64_t seed) noexcept {
    auto [extent, allocate_status] = volume.allocate(run, bytes);
    if (allocate_status.failed()) return allocate_status;
    st::vector<util::byte_t> painted_bytes;
    if (painted_bytes.resize(static_cast<std::size_t>(bytes)) != st::status_t::success_k)
        return util::status_t {util::status_code_t::out_of_memory_k, "No room for a painted run"};
    paint(painted_bytes, seed);
    if (util::status_t const written = util::write_extent<typename fixture_type_::disk_t, waiter_t>(
            fixture.disk, channel, extent, std::span<util::byte_t const> {painted_bytes});
        written.failed())
        return written;
    return extent;
}

/** Reads @p bytes of @p extent back and answers whether they hold what @p seed painted. */
template <typename fixture_type_>
[[nodiscard]] bool reads_painted(fixture_type_& fixture, util::io_channel_t& channel,
                                 util::medium_extent_t const& extent, std::uint64_t bytes,
                                 std::uint64_t seed) noexcept {
    st::vector<util::byte_t> read;
    if (read.resize(static_cast<std::size_t>(bytes)) != st::status_t::success_k) return false;
    if (util::read_extent<typename fixture_type_::disk_t>(fixture.disk, channel, extent, std::span<util::byte_t> {read})
            .failed())
        return false;
    return holds_paint(read, seed);
}

/** Creates the sparse file @p name under the fixture's directory, standing in for a device of
 *  @p slots slots. */
template <typename fixture_type_>
std::string stand_in_device(fixture_type_ const& fixture, char const* name, std::uint64_t slots) noexcept {
    std::string const path = fmt::format("{}/{}", fixture.directory, name);
    std::FILE* const file = std::fopen(path.c_str(), "wb");
    if (file) std::fclose(file);
    std::error_code error;
    std::filesystem::resize_file(path.c_str(), slots * util::slot_bytes_k, error);
    return path;
}

/** Writes @p block over the first page at @p offset of the stand-in device @p path, as
 *  another tool would. */
inline bool overwrite_page(std::string const& path, std::uint64_t offset,
                           std::span<util::byte_t const> block) noexcept {
    std::FILE* const file = std::fopen(path.c_str(), "r+b");
    if (!file) return false;
    bool const written = std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 &&
                         std::fwrite(block.data(), 1, block.size(), file) == block.size();
    return std::fclose(file) == 0 && written;
}

/** Reads the page at @p offset of the stand-in device @p path into @p block. */
inline bool read_page(std::string const& path, std::uint64_t offset, std::span<util::byte_t> block) noexcept {
    std::FILE* const file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    bool const read = std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0 &&
                      std::fread(block.data(), 1, block.size(), file) == block.size();
    return std::fclose(file) == 0 && read;
}

/** The address of @p slots slots of @p path from @p base on. */
struct raw_address_t {
    std::string text;
    raw_address_t(std::string const& path, std::uint64_t base, std::uint64_t slots)
        : text(fmt::format("{}@{}+{}", path, base, slots * util::slot_bytes_k)) {}
    [[nodiscard]] util::data_address_t address() const noexcept { return *util::parse_data_address(text); }
};

/** Mounts a slotted volume over @p addresses on the fixture's disk. */
template <typename fixture_type_>
auto mount_slotted(fixture_type_& fixture, util::io_channel_t& channel,
                   std::span<util::data_address_t const> addresses) noexcept {
    using volume_t = util::basic_slotted_volume<typename fixture_type_::disk_t, waiter_t, volume_allocator_t>;
    util::volume_config_t config;
    config.database = util::string_view_t {"raw"};
    config.data_directories = addresses;
    return volume_t::mount(fixture.disk, channel, config, volume_allocator_t {fixture.resources->memory_constraint});
}

#pragma endregion Helpers

#pragma region Directory Cases

/** A run named by a published manifest reads back after a remount, and one never named is
 *  unlinked by it. */
template <typename fixture_type_>
case_result_t test_directory_keeps_named_and_reclaims_leftovers(fixture_type_& fixture) {
    if (!fixture.persists_volumes()) return case_result_t::skipped(memory_volumes_vanish_k);
    auto [guard, acquire_status] = fixture.channels.acquire();
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return {};
    util::io_channel_t& channel = guard.channel();

    util::uuid128_t const kept = util::uuid_generator(), leftover = util::uuid_generator();
    std::string leftover_path;
    {
        auto [volume, mount_status] = fixture.mount_volume(channel);
        expect_true(mount_status.succeeded());
        if (mount_status.failed()) return {};
        expect_equal(volume.generation(), std::uint64_t {0});
        auto [named, named_status] = write_painted(fixture, volume, channel, kept, small_run_bytes_k, 7);
        auto [unnamed, unnamed_status] = write_painted(fixture, volume, channel, leftover, small_run_bytes_k, 8);
        expect_true(named_status.succeeded() && unnamed_status.succeeded());
        if (named_status.failed() || unnamed_status.failed()) return {};
        util::manifest_file_t const files[] {file_of(named, kept)};
        expect_true(publish_naming(volume, fixture, channel, files).succeeded());
        expect_equal(volume.generation(), std::uint64_t {1});
        char name[util::path_max_bytes_k];
        expect_true(volume.manifest().file_name(leftover, name).succeeded());
        leftover_path = fmt::format("{}/{}", fixture.data_directory_path(0).c_str(), name);
        expect_true(std::filesystem::exists(leftover_path.c_str()));
    }

    auto [volume, mount_status] = fixture.mount_volume(channel);
    expect_true(mount_status.succeeded());
    if (mount_status.failed()) return {};
    expect_equal(volume.generation(), std::uint64_t {1});
    expect_equal(volume.manifest().files.size(), std::size_t {1});
    expect_true(!std::filesystem::exists(leftover_path.c_str()));
    if (volume.manifest().files.size() != 1) return {};
    auto [opened, open_status] = volume.open(volume.manifest().files[0]);
    expect_true(open_status.succeeded());
    if (open_status.succeeded()) expect_true(reads_painted(fixture, channel, opened, small_run_bytes_k, 7));
    return {};
}

/** A manifest copy a crash left before its rename is ignored and removed, and the generation
 *  in force stays. */
template <typename fixture_type_>
case_result_t test_directory_ignores_an_unrenamed_copy(fixture_type_& fixture) {
    if (!fixture.persists_volumes()) return case_result_t::skipped(memory_volumes_vanish_k);
    auto [guard, acquire_status] = fixture.channels.acquire();
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return {};
    util::io_channel_t& channel = guard.channel();
    {
        auto [volume, mount_status] = fixture.mount_volume(channel);
        expect_true(mount_status.succeeded());
        if (mount_status.failed()) return {};
        expect_true(publish_naming(volume, fixture, channel, {}).succeeded());
    }
    std::array<char, 64> name {};
    auto const written = fmt::format_to_n(name.data(), name.size() - 1, "{}.ustore.json.tmp", fixture.suite);
    *written.out = '\0';
    std::string const copy = fmt::format("{}/{}", fixture.data_directory_path(0).c_str(), name.data());
    std::FILE* const file = std::fopen(copy.c_str(), "wb");
    expect_true(file != nullptr);
    if (file) {
        std::fputs("{\"generation\": 9", file);
        std::fclose(file);
    }

    auto [volume, mount_status] = fixture.mount_volume(channel);
    expect_true(mount_status.succeeded());
    if (mount_status.succeeded()) expect_equal(volume.generation(), std::uint64_t {1});
    expect_true(!std::filesystem::exists(copy.c_str()));
    return {};
}

/** Runs land on the data directory with the most free bytes, and a quota that leaves no
 *  room refuses them. */
template <typename fixture_type_>
void test_directory_places_by_free_space(fixture_type_& fixture) {
    using volume_t = typename fixture_type_::volume_t;
    auto [guard, acquire_status] = fixture.channels.acquire();
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    util::io_channel_t& channel = guard.channel();

    std::string const first = fixture.data_directory_path(0), second = fixture.data_directory_path(1);
    std::array<util::data_address_t, 2> addresses {};
    addresses[0].location = first.c_str();
    addresses[0].byte_limit = 4 * small_run_bytes_k;
    addresses[1].location = second.c_str();
    addresses[1].byte_limit = 8 * small_run_bytes_k;
    util::volume_config_t config;
    config.database = util::string_view_t {fixture.suite};
    config.data_directories = addresses;
    config.runs_capacity = volume_config_k.runs_capacity;
    auto [volume, mount_status] = volume_t::mount(fixture.disk, channel, config,
                                                  volume_allocator_t {fixture.resources->memory_constraint});
    expect_true(mount_status.succeeded());
    if (mount_status.failed()) return;

    std::array<std::size_t, 2> placed {};
    std::uint64_t const rounded = util::round_up_to_multiple(small_run_bytes_k, util::portable_alignment_k);
    std::size_t const fits = static_cast<std::size_t>((addresses[0].byte_limit / rounded) +
                                                      (addresses[1].byte_limit / rounded));
    for (std::size_t index = 0; index != fits; ++index) {
        auto [extent, allocate_status] = volume.allocate(util::uuid_generator(), small_run_bytes_k);
        expect_true(allocate_status.succeeded());
        if (allocate_status.failed()) return;
        ++placed[extent.data_directory];
    }
    expect_equal(placed[0], static_cast<std::size_t>(addresses[0].byte_limit / rounded));
    expect_equal(placed[1], static_cast<std::size_t>(addresses[1].byte_limit / rounded));
    expect_status(volume.allocate(util::uuid_generator(), small_run_bytes_k),
                  util::status_code_t::capacity_exhausted_k);
}

/** Runs allocated back to back over uncapped directories of one filesystem spread one per
 *  directory, since a run claims its bytes before its sparse file does. */
template <typename fixture_type_>
void test_directory_spreads_unwritten_runs(fixture_type_& fixture) {
    constexpr std::size_t directories_k = 4;
    // Large enough that another writer on the filesystem cannot move its free bytes that far
    // between two probes.
    constexpr std::uint64_t run_bytes_k = 16ull * 1024ull * 1024ull;
    auto [guard, acquire_status] = fixture.channels.acquire();
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    util::io_channel_t& channel = guard.channel();
    auto [volume, mount_status] = fixture.mount_volume(channel, directories_k);
    expect_true(mount_status.succeeded());
    if (mount_status.failed()) return;

    std::array<std::size_t, directories_k> placed {};
    for (std::size_t index = 0; index != directories_k; ++index) {
        auto [extent, allocate_status] = volume.allocate(util::uuid_generator(), run_bytes_k);
        expect_true(allocate_status.succeeded());
        if (allocate_status.failed()) return;
        ++placed[extent.data_directory];
    }
    for (std::size_t const runs : placed) expect_equal(runs, std::size_t {1});
}

/** A second mount of the directories a live volume holds is refused, since its sweep would
 *  unlink the first mount's unpublished runs, and mounts once the first is gone. */
template <typename fixture_type_>
void test_directory_refuses_a_second_mount(fixture_type_& fixture) {
    auto [guard, acquire_status] = fixture.channels.acquire();
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    util::io_channel_t& channel = guard.channel();
    {
        auto [volume, mount_status] = fixture.mount_volume(channel);
        expect_true(mount_status.succeeded());
        if (mount_status.failed()) return;
        expect_true(volume.allocate(util::uuid_generator(), small_run_bytes_k).has_value());
        expect_status(fixture.mount_volume(channel), util::status_code_t::contended_k);
    }
    expect_true(fixture.mount_volume(channel).has_value());
}

#pragma endregion Directory Cases

#pragma region Slotted Cases

/** A slot a published manifest names stays, a slot written but never named is reclaimed at the next
 *  mount, and a zero block reads as free. */
template <typename fixture_type_>
void test_slotted_keeps_named_and_reclaims_leftovers(fixture_type_& fixture) {
    auto [guard, acquire_status] = fixture.channels.acquire();
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    util::io_channel_t& channel = guard.channel();
    std::string const device = stand_in_device(fixture, "leftovers.raw", stand_in_slots_k);
    raw_address_t const raw {device, 0, stand_in_slots_k};
    util::data_address_t const addresses[] {raw.address()};

    util::medium_extent_t kept {};
    {
        auto [volume, mount_status] = mount_slotted(fixture, channel, addresses);
        expect_true(mount_status.succeeded());
        if (mount_status.failed()) return;
        // The manifest slot is held, and every other slot is free.
        expect_equal(volume.free_bytes(), (stand_in_slots_k - 1) * util::sorted_run_bytes_max_k);
        auto [packed, packed_status] = write_painted(fixture, volume, channel, util::uuid128_t {}, small_run_bytes_k,
                                                     11);
        auto [whole, whole_status] = volume.allocate(util::uuid128_t {}, util::packed_run_bytes_max_k);
        expect_true(packed_status.succeeded() && whole_status.succeeded());
        if (packed_status.failed() || whole_status.failed()) return;
        expect_true(packed.extent.offset / util::slot_bytes_k != whole.extent.offset / util::slot_bytes_k);
        kept = packed;
        util::manifest_file_t const files[] {file_of(packed, util::uuid128_t {})};
        expect_true(publish_naming(volume, fixture, channel, files).succeeded());
    }

    auto [volume, mount_status] = mount_slotted(fixture, channel, addresses);
    expect_true(mount_status.succeeded());
    if (mount_status.failed()) return;
    expect_equal(volume.generation(), std::uint64_t {1});
    // The whole-slot run was never named, so its slot came back; the packed slot stays
    // held and open.
    std::uint64_t const packed_tail = util::slot_payload_offset_k +
                                      util::round_up_to_multiple(small_run_bytes_k, util::portable_alignment_k);
    expect_equal(volume.free_bytes(),
                 (stand_in_slots_k - 2) * util::sorted_run_bytes_max_k + (util::slot_bytes_k - packed_tail));
    if (!expect_equal(volume.manifest().files.size(), std::size_t {1})) return;
    auto [opened, open_status] = volume.open(volume.manifest().files[0]);
    expect_true(open_status.succeeded());
    if (open_status.succeeded()) {
        expect_equal(opened.extent.offset, kept.extent.offset);
        expect_true(reads_painted(fixture, channel, opened, small_run_bytes_k, 11));
    }
    // A small run after the mount packs behind the kept one rather than claiming a fresh slot.
    auto [next, next_status] = volume.allocate(util::uuid128_t {}, small_run_bytes_k);
    expect_true(next_status.succeeded());
    if (next_status.succeeded()) expect_equal(next.extent.offset, kept.extent.offset + kept.extent.length);
}

/** A run given back after the manifest stopped naming it reads as zeros from its first block once
 *  the next publication lands, while the run packed beside it in the same slot stays whole. */
template <typename fixture_type_>
void test_slotted_zeroes_a_dropped_run(fixture_type_& fixture) {
    auto [guard, acquire_status] = fixture.channels.acquire();
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    util::io_channel_t& channel = guard.channel();
    std::string const device = stand_in_device(fixture, "dropped.raw", stand_in_slots_k);
    raw_address_t const raw {device, 0, stand_in_slots_k};
    util::data_address_t const addresses[] {raw.address()};

    auto [volume, mount_status] = mount_slotted(fixture, channel, addresses);
    expect_true(mount_status.succeeded());
    if (mount_status.failed()) return;
    auto [dropped, dropped_status] = write_painted(fixture, volume, channel, util::uuid_generator(), small_run_bytes_k,
                                                   41);
    auto [kept, kept_status] = write_painted(fixture, volume, channel, util::uuid_generator(), small_run_bytes_k, 42);
    expect_true(dropped_status.succeeded() && kept_status.succeeded());
    if (dropped_status.failed() || kept_status.failed()) return;
    expect_equal(dropped.extent.offset / util::slot_bytes_k, kept.extent.offset / util::slot_bytes_k);
    util::manifest_file_t const both[] {file_of(dropped, util::uuid128_t {}), file_of(kept, util::uuid128_t {})};
    expect_true(publish_naming(volume, fixture, channel, both).succeeded());

    // The manifest stops naming the run, and only once it is durable is the run given back.
    util::manifest_file_t const one[] {file_of(kept, util::uuid128_t {})};
    expect_true(publish_naming(volume, fixture, channel, one).succeeded());
    volume.deallocate(dropped);
    expect_true(publish_naming(volume, fixture, channel, one).succeeded());

    std::array<util::byte_t, util::portable_alignment_k> zeros {};
    st::vector<util::byte_t> read;
    expect_true(read.resize(util::portable_alignment_k) == st::status_t::success_k);
    expect_true(util::read_extent<typename fixture_type_::disk_t>(fixture.disk, channel, dropped,
                                                                  std::span<util::byte_t> {read})
                    .succeeded());
    expect_true(std::memcmp(read.data(), zeros.data(), zeros.size()) == 0);
    expect_true(reads_painted(fixture, channel, kept, small_run_bytes_k, 42));
}

/** A JSON copy written into the idle region without the owner block that commits it leaves the old
 *  generation in force, as a crash between the two flushes would. */
template <typename fixture_type_>
void test_slotted_ignores_an_uncommitted_copy(fixture_type_& fixture) {
    using volume_t = util::basic_slotted_volume<typename fixture_type_::disk_t, waiter_t, volume_allocator_t>;
    auto [guard, acquire_status] = fixture.channels.acquire();
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    util::io_channel_t& channel = guard.channel();
    std::string const device = stand_in_device(fixture, "uncommitted.raw", stand_in_slots_k);
    raw_address_t const raw {device, 0, stand_in_slots_k};
    util::data_address_t const addresses[] {raw.address()};
    {
        auto [volume, mount_status] = mount_slotted(fixture, channel, addresses);
        expect_true(mount_status.succeeded());
        if (mount_status.failed()) return;
        expect_true(publish_naming(volume, fixture, channel, {}).succeeded());
    }
    // The first publication took region zero, so the next would write region one.
    std::array<util::byte_t, util::portable_alignment_k> copy {};
    char const text[] = "{\"generation\": 2}";
    std::memcpy(copy.data(), text, sizeof(text) - 1);
    expect_true(overwrite_page(device, util::slot_payload_offset_k + volume_t::manifest_region_bytes_k, copy));

    auto [volume, mount_status] = mount_slotted(fixture, channel, addresses);
    expect_true(mount_status.succeeded());
    if (mount_status.succeeded()) expect_equal(volume.generation(), std::uint64_t {1});
}

/** Two databases share one medium through disjoint extents, neither taking a slot past its own;
 *  another tool's file keeps every slot it spans, and its magic in a base slot refuses a mount. */
template <typename fixture_type_>
void test_slotted_shares_a_medium(fixture_type_& fixture) {
    auto [guard, acquire_status] = fixture.channels.acquire();
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    util::io_channel_t& channel = guard.channel();
    std::string const device = stand_in_device(fixture, "shared.raw", stand_in_slots_k + 2);
    raw_address_t const first_raw {device, 0, 2}, second_raw {device, 2 * util::slot_bytes_k, 2};
    util::data_address_t const first[] {first_raw.address()}, second[] {second_raw.address()};

    // Slots 4 and 5 hold one SafeGraphs index, whose second slot opens with zeros like a free one.
    std::array<util::byte_t, util::portable_alignment_k> foreign {}, interior {};
    std::uint64_t const safegraphs_magic = util::magic_of("SAFEGRPH");
    std::memcpy(foreign.data(), &safegraphs_magic, sizeof(safegraphs_magic));
    paint(interior, 77);
    std::uint64_t const interior_offset = (stand_in_slots_k + 1) * util::slot_bytes_k + util::slot_payload_offset_k;
    expect_true(overwrite_page(device, stand_in_slots_k * util::slot_bytes_k, foreign));
    expect_true(overwrite_page(device, interior_offset, interior));
    {
        auto [volume, mount_status] = mount_slotted(fixture, channel, first);
        expect_true(mount_status.succeeded());
        if (mount_status.failed()) return;
        expect_equal(volume.free_bytes(), std::uint64_t {1} * util::sorted_run_bytes_max_k);
        expect_true(publish_naming(volume, fixture, channel, {}).succeeded());
        // Another mount of the same medium waits for this one, as another process claiming
        // its slots would.
        expect_status(mount_slotted(fixture, channel, second), util::status_code_t::contended_k);
    }
    {
        auto [volume, mount_status] = mount_slotted(fixture, channel, second);
        expect_true(mount_status.succeeded());
        if (mount_status.failed()) return;
        expect_equal(volume.free_bytes(), std::uint64_t {1} * util::sorted_run_bytes_max_k);
        auto [run, run_status] = write_painted(fixture, volume, channel, util::uuid128_t {}, small_run_bytes_k, 21);
        expect_true(run_status.succeeded());
        if (run_status.failed()) return;
        expect_equal(run.extent.offset / util::slot_bytes_k, std::uint64_t {3});
        util::manifest_file_t const files[] {file_of(run, util::uuid128_t {})};
        expect_true(publish_naming(volume, fixture, channel, files).succeeded());
        // Its extent is full, and the zeros opening slot 5 are not its to take.
        expect_status(volume.allocate(util::uuid128_t {}, util::sorted_run_bytes_max_k),
                      util::status_code_t::capacity_exhausted_k);
    }
    {
        auto [volume, mount_status] = mount_slotted(fixture, channel, first);
        expect_true(mount_status.succeeded());
        if (mount_status.failed()) return;
        auto [run, run_status] = volume.allocate(util::uuid128_t {}, small_run_bytes_k);
        expect_true(run_status.succeeded());
        if (run_status.succeeded()) expect_equal(run.extent.offset / util::slot_bytes_k, std::uint64_t {1});
    }
    {
        auto [volume, mount_status] = mount_slotted(fixture, channel, second);
        expect_true(mount_status.succeeded());
        if (mount_status.succeeded()) expect_equal(volume.manifest().files.size(), std::size_t {1});
    }
    std::array<util::byte_t, util::portable_alignment_k> read_back {};
    expect_true(read_page(device, interior_offset - util::slot_payload_offset_k, read_back) &&
                read_back == std::array<util::byte_t, util::portable_alignment_k> {});
    expect_true(read_page(device, interior_offset, read_back) && holds_paint(read_back, 77));

    raw_address_t const foreign_raw {device, stand_in_slots_k * util::slot_bytes_k, 2};
    util::data_address_t const foreign_base[] {foreign_raw.address()};
    expect_status(mount_slotted(fixture, channel, foreign_base), util::status_code_t::already_exists_k);
    // A raw medium is mounted by a length that fits it, never from a base to wherever it ends.
    raw_address_t const past_raw {device, 0, stand_in_slots_k + 3};
    util::data_address_t unbounded[] {first_raw.address()};
    unbounded[0].byte_limit = 0;
    util::data_address_t const past[] {past_raw.address()};
    expect_status(mount_slotted(fixture, channel, past), util::status_code_t::invalid_argument_k);
    expect_status(mount_slotted(fixture, channel, unbounded), util::status_code_t::invalid_argument_k);
}

/** Data directories listed in another order than the database was created with are refused. */
template <typename fixture_type_>
void test_slotted_refuses_reordered_media(fixture_type_& fixture) {
    auto [guard, acquire_status] = fixture.channels.acquire();
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    util::io_channel_t& channel = guard.channel();
    std::string const smaller = stand_in_device(fixture, "first.raw", 2),
                      larger = stand_in_device(fixture, "second.raw", 3);
    raw_address_t const smaller_raw {smaller, 0, 2}, larger_raw {larger, 0, 3};
    util::data_address_t const ordered[] {smaller_raw.address(), larger_raw.address()};
    util::data_address_t const reordered[] {larger_raw.address(), smaller_raw.address()};
    {
        auto [volume, mount_status] = mount_slotted(fixture, channel, ordered);
        expect_true(mount_status.succeeded());
        if (mount_status.failed()) return;
        auto [run, run_status] = write_painted(fixture, volume, channel, util::uuid128_t {}, small_run_bytes_k, 31);
        expect_true(run_status.succeeded());
        if (run_status.failed()) return;
        expect_equal(run.data_directory, std::uint16_t {1});
        util::manifest_file_t const files[] {file_of(run, util::uuid128_t {})};
        expect_true(publish_naming(volume, fixture, channel, files).succeeded());
    }
    expect_status(mount_slotted(fixture, channel, reordered), util::status_code_t::malformed_k);
    auto [volume, mount_status] = mount_slotted(fixture, channel, ordered);
    expect_true(mount_status.succeeded());
}

#pragma endregion Slotted Cases

#pragma region Addresses

/** Data directory addresses read as the forms they name, and a manifest path names
 *  data directory 0. */
void test_addresses_read_their_forms() {
    auto [directory, directory_status] = util::parse_data_address("/mnt/nvme0/molecules");
    expect_true(directory_status.succeeded() && directory.kind == util::data_address_kind_t::directory_k);
    auto [device, device_status] = util::parse_data_address("/dev/nvme0n1@64GiB+16GiB");
    expect_true(device_status.succeeded() && device.kind == util::data_address_kind_t::raw_k);
    expect_equal(device.byte_base, std::uint64_t {64} << 30);
    expect_equal(device.byte_limit, std::uint64_t {16} << 30);
    expect_true(device.location == "/dev/nvme0n1");
    auto [spdk, spdk_status] = util::parse_data_address("spdk://0000:5e:00.0/2@0GiB");
    expect_true(spdk_status.succeeded() && spdk.kind == util::data_address_kind_t::spdk_k);
    expect_equal(spdk.namespace_id, std::uint32_t {2});
    expect_status(util::parse_data_address("/dev/nvme0n1@64XiB"), util::status_code_t::invalid_argument_k);

    util::resources_config_t config;
    [[maybe_unused]] sz::status_t const assigned = config.manifest.assign(
        util::string_view_t {"/data/molecules.ustore.json"});
    auto [addresses, addresses_status] = util::addresses_of(config);
    expect_true(addresses_status.succeeded());
    if (addresses_status.succeeded()) {
        expect_true(addresses.database == util::string_view_t {"molecules"});
        expect_true(addresses.data_directories[0].location == "/data");
    }
}

/** io_uring named outright never falls back, while the automatic choice does, and a kernel refusing
 *  a setup flag surfaces as unsupported with the Linux version the transport needs. */
void test_probed_transport_refuses_a_missing_ring() {
    for (util::linux_uring_probe_t const missing :
         {util::linux_uring_probe_t::refused_k, util::linux_uring_probe_t::too_old_k})
        expect_status(
            util::probed_transport(util::io_transport_t::linux_uring_k, missing, util::linux_aio_probe_t::available_k),
            util::status_code_t::unsupported_k);
    // A sandbox refusing every ring answers refused_k for any flag, so only a machine with rings
    // can tell a refused one
    if (linux_uring_files_disk_t::probe() == util::linux_uring_probe_t::available_k) {
        util::linux_uring_probe_t const refused_flag = linux_uring_files_disk_t::probe(std::uint32_t {1} << 31);
        expect_true(refused_flag == util::linux_uring_probe_t::too_old_k);
        auto [old, old_status] = util::probed_transport(util::io_transport_t::linux_uring_k, refused_flag,
                                                        util::linux_aio_probe_t::available_k);
        expect_true(std::string_view {old_status.note}.find("Linux 6.12") != std::string_view::npos);
    }
    auto [automatic, automatic_status] = util::probed_transport(
        util::io_transport_t::automatic_k, util::linux_uring_probe_t::too_old_k, util::linux_aio_probe_t::available_k);
    expect_true(automatic_status.succeeded());
    expect_true(automatic == util::io_transport_t::linux_aio_k);
}

#pragma endregion Addresses

#pragma region Mount Limits

/** A mount sizes its file table to the descriptor limit, and refuses a limit leaving no room for a
 *  run file; each mount runs in a child, since a lowered hard limit never rises again. */
case_result_t test_mount_fits_the_descriptor_limit(char const* directory) {
    if (!directory) return case_result_t::skipped(needs_directory_k);
    std::string const manifest = fmt::format("{}/{}", directory, "descriptor_limit/limits.ustore.json");
    auto const mounts_under = [&](rlim_t descriptors, util::status_code_t wanted) {
        pid_t const child = ::fork();
        if (child == 0) {
            struct rlimit const limit {descriptors, descriptors};
            if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) ::_exit(1);
            util::resources_config_t config;
            config.threads = 1;
            if (sz::failed(config.manifest.assign(util::string_view_t {manifest.c_str()}))) ::_exit(1);
            auto const [opened, open_status] = util::open_resources<atomic_reference, waiter_t>(config);
            if (open_status.failed()) ::_exit(open_status.code == wanted ? 0 : 1);
            std::size_t runs = 0;
            [[maybe_unused]] util::status_t const visited = opened->storage.visit([&](auto& mount) noexcept {
                runs = mount.volume().runs_capacity();
                return util::status_t {};
            });
            ::_exit(wanted == util::status_code_t::success_k && runs != 0 && runs < descriptors ? 0 : 1);
        }
        int status = 0;
        bool const exited = child > 0 && ::waitpid(child, &status, 0) == child && WIFEXITED(status);
        expect_true(exited && WEXITSTATUS(status) == 0, "a mount under {} descriptors did not end as {}", descriptors,
                    status_name(wanted));
    };
    mounts_under(64, util::status_code_t::success_k);
    mounts_under(4, util::status_code_t::capacity_exhausted_k);
    return {};
}

/** A mount whose channel arena outgrows the gibibyte io_uring pins per table slot still moves its
 *  last block, pinned wherever the locked-memory cap allows. */
case_result_t test_mount_moves_an_arena_past_a_gibibyte(char const* directory) {
    if (!directory) return case_result_t::skipped(needs_directory_k);
    std::string const manifest = fmt::format("{}/{}", directory, "wide_arena/wide.ustore.json");
    std::string const medium_path = fmt::format("{}/{}", directory, "wide_arena/medium.bin");
    util::resources_config_t config;
    config.threads = 1;
    config.shared_io_channels = 7;
    config.channels.request_buffer_bytes = std::uint64_t {1} << 20;
    if (!expect_true(!sz::failed(config.manifest.assign(util::string_view_t {manifest.c_str()})))) return {};
    auto const [opened, open_status] = util::open_resources<atomic_reference, waiter_t>(config);
    if (!expect_status(open_status, util::status_code_t::success_k)) return {};
    util::mount_report_t const report = opened->storage.report();
    expect_true(report.arena_bytes > util::linux_uring_registered_range_bytes_k);
    util::status_t const moved = opened->storage.visit([&](auto& mount) noexcept -> status_t {
        auto [guard, acquire_status] = mount.channels().acquire(0);
        if (acquire_status.failed()) return acquire_status;
        util::medium_options_t request;
        request.name = medium_path.c_str();
        request.access = util::medium_access_t::create_read_write_k;
        request.length = util::portable_alignment_k;
        auto [medium, register_status] = mount.disk().register_medium(request);
        if (register_status.failed()) return register_status;
        util::queue_pair_t const& queue_pair = guard.channel().queue_pair();
        std::uint64_t const last_block = mount.arena().size() - util::portable_alignment_k;
        std::array<util::request_t, 1> const writes {
            util::write_request(medium.reference(), util::extent_t {0, util::portable_alignment_k},
                                mount.arena().reference(last_block), queue_pair.token(0))};
        std::array<util::completion_t, 1> completions {};
        auto const [submitted, submit_status] = mount.disk().submit(queue_pair,
                                                                    std::span<util::request_t const> {writes});
        auto const [waited, wait_status] = util::wait_completions<waiter_t>(
            mount.disk(), queue_pair, std::span<util::completion_t> {completions});
        util::status_t const released = mount.disk().unregister_medium(medium);
        if (submit_status.failed() || submitted != 1 || wait_status.failed() || waited != 1 ||
            !completions[0].completed_in_full())
            return {util::status_code_t::io_k, "The arena's last block would not move"};
        return released;
    });
    expect_status(moved, util::status_code_t::success_k);
    struct rlimit locked {};
    if (report.transport == util::io_transport_t::linux_uring_k && ::getrlimit(RLIMIT_MEMLOCK, &locked) == 0 &&
        locked.rlim_cur == RLIM_INFINITY)
        expect_true(opened->storage.pinning() == util::range_pinning_t::pinned_k);
    return {};
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
    auto [fixture, status] = mount();
    if (!expect_status(status, util::status_code_t::success_k)) return {};
    if constexpr (std::is_void_v<decltype(body(*fixture))>) {
        body(*fixture);
        return {};
    }
    else return body(*fixture);
}

#pragma endregion Drivers

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;
    using namespace unum::ustore::blobs;

    start_test_run(argc, argv);
    auto [directory, directory_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(directory_status)) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }

    auto const cases = [&](auto const& mount) {
        auto const on = [&](char const* name, needed_medium_t needed, auto&& body) {
            case_label_t const label = mount.label(name);
            run_test_case(label.c_str(), [&] { return on_fixture(mount, needed, body); });
        };
        on("test_directory_keeps_named_and_reclaims_leftovers", needed_medium_t::any_k,
           [](auto& fixture) { return test_directory_keeps_named_and_reclaims_leftovers(fixture); });
        on("test_directory_ignores_an_unrenamed_copy", needed_medium_t::any_k,
           [](auto& fixture) { return test_directory_ignores_an_unrenamed_copy(fixture); });
        on("test_directory_places_by_free_space", needed_medium_t::any_k,
           [](auto& fixture) { return test_directory_places_by_free_space(fixture); });
        on("test_directory_spreads_unwritten_runs", needed_medium_t::any_k,
           [](auto& fixture) { return test_directory_spreads_unwritten_runs(fixture); });
        on("test_directory_refuses_a_second_mount", needed_medium_t::any_k,
           [](auto& fixture) { return test_directory_refuses_a_second_mount(fixture); });
        on("test_slotted_keeps_named_and_reclaims_leftovers", needed_medium_t::file_k,
           [](auto& fixture) { return test_slotted_keeps_named_and_reclaims_leftovers(fixture); });
        on("test_slotted_zeroes_a_dropped_run", needed_medium_t::file_k,
           [](auto& fixture) { return test_slotted_zeroes_a_dropped_run(fixture); });
        on("test_slotted_ignores_an_uncommitted_copy", needed_medium_t::file_k,
           [](auto& fixture) { return test_slotted_ignores_an_uncommitted_copy(fixture); });
        on("test_slotted_shares_a_medium", needed_medium_t::file_k,
           [](auto& fixture) { return test_slotted_shares_a_medium(fixture); });
        on("test_slotted_refuses_reordered_media", needed_medium_t::file_k,
           [](auto& fixture) { return test_slotted_refuses_reordered_media(fixture); });
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(transport, directory.path(), volume_config_k, 0, "volume", cases);
    run_test_case("test_addresses_read_their_forms", test_addresses_read_their_forms);
    run_test_case("test_probed_transport_refuses_a_missing_ring", test_probed_transport_refuses_a_missing_ring);
    run_test_case("test_mount_fits_the_descriptor_limit", test_mount_fits_the_descriptor_limit, directory.path());
    run_test_case("test_mount_moves_an_arena_past_a_gibibyte", test_mount_moves_an_arena_past_a_gibibyte,
                  directory.path());
    return finish_test_run();
}
