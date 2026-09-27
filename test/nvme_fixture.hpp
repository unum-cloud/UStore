/**
 *  @file test/nvme_fixture.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief One mounted ring for every engine suite: its config, the transports, the driver and the byte painters.
 *
 *  A suite names its config once, writes every case against the mount it is handed, and lets @c run_on_transport
 *  run the same cases on memory, on files through the blocking floor and through io_uring, and on a block
 *  device. A transport the machine does not offer skips with its reason; a transport the flags claim and the
 *  mount then refuses fails.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstdio>  // `std::remove`
#include <cstring> // `std::memset`

#include <sys/vfs.h> // `statfs`

#include <array>       // `std::array`
#include <memory>      // `std::unique_ptr`, `std::make_shared`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`
#include <utility>     // `std::exchange`, `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "nvme/blocking_files_disk.hpp" // `blocking_files_disk_t`, `file_buffering_t`
#include "nvme/buffer_pool.hpp"         // `registered_memory_arena_t`, `memory_arena_config_t`
#include "nvme/collection_snapshot.hpp" // `select_result_t`
#include "nvme/io_channel.hpp"          // `basic_io_channel_pool`, `io_channel_config_t`
#include "nvme/io_pipeline.hpp"         // `basic_write_pipeline`
#include "nvme/ram_disk.hpp"            // `ram_disk_t`
#include "nvme/resources.hpp"           // `resources_t`, `resources_shared_ptr_t`
#include "nvme/uring_files_disk.hpp"    // `uring_files_disk_t`, `uring_probe_t`
#include "nvme/volume.hpp"              // `basic_extent_volume`, `volume_format_t`

#include "test.hpp" // `describe`, `expect_status`, `suite_totals_t`

namespace unum::ustore::test {

using namespace unum::ustore::nvme;

#pragma region Transports

/** The rings a suite mounts, each run when the machine and the flags offer it. */
enum class transport_t : std::uint8_t {

    /** Volumes in host memory, held to the alignment a drive demands. */
    memory_k,

    /** Files under @c --directory, through positioned reads and writes on the polling thread. */
    files_k,

    /** Unbuffered files under @c --directory, through io_uring. */
    io_uring_k,

    /** The block device @c --device names, through io_uring. */
    device_k,

    /** The namespace @c --spdk-address binds, which the driver does not mount. */
    spdk_k,
};

/** Every transport, in the order a suite runs them. */
constexpr std::array<transport_t, 5> every_transport_k {
    transport_t::memory_k, transport_t::files_k, transport_t::io_uring_k, transport_t::device_k, transport_t::spdk_k};

/** The name @c --transports selects @p transport by, which is also the middle of every result line. */
[[nodiscard]] constexpr char const* name_of(transport_t transport) noexcept {
    switch (transport) {
    case transport_t::memory_k: return "ram";
    case transport_t::files_k: return "files";
    case transport_t::io_uring_k: return "uring";
    case transport_t::device_k: return "device";
    case transport_t::spdk_k: return "spdk";
    }
    return "unknown";
}

/** The result line's name: the suite, the transport and the case it ran, or the suite and the transport alone. */
struct case_label_t {

    /** The null-terminated label. */
    char text[96] {};

    case_label_t(char const* suite, char const* transport, char const* name) noexcept {
        auto const written = fmt::format_to_n(text, sizeof(text) - 1, "{}.{}.{}", suite, transport, name);
        *written.out = '\0';
    }

    case_label_t(char const* suite, char const* transport) noexcept {
        auto const written = fmt::format_to_n(text, sizeof(text) - 1, "{}.{}", suite, transport);
        *written.out = '\0';
    }

    /** The label as a C string. */
    [[nodiscard]] char const* c_str() const noexcept { return text; }
};

/** Whether a ring performs a request inside @c submit or only once the kernel hands it back. */
enum class completion_timing_t : std::uint8_t {
    synchronous_k,
    asynchronous_k,
};

/** The timing of @p transport, which is synchronous for memory and for the blocking floor. */
[[nodiscard]] constexpr completion_timing_t completion_timing_of(transport_t transport) noexcept {
    switch (transport) {
    case transport_t::memory_k:
    case transport_t::files_k: return completion_timing_t::synchronous_k;
    case transport_t::io_uring_k:
    case transport_t::device_k:
    case transport_t::spdk_k: return completion_timing_t::asynchronous_k;
    }
    return completion_timing_t::synchronous_k;
}

/** Whether a write past the end of the medium is refused or grows the medium. */
enum class capacity_behavior_t : std::uint8_t {
    fixed_k,
    growing_k,
};

/** The edge of @p transport, which a regular file grows past and every other medium refuses. */
[[nodiscard]] constexpr capacity_behavior_t capacity_behavior_of(transport_t transport) noexcept {
    switch (transport) {
    case transport_t::files_k:
    case transport_t::io_uring_k: return capacity_behavior_t::growing_k;
    case transport_t::memory_k:
    case transport_t::device_k:
    case transport_t::spdk_k: return capacity_behavior_t::fixed_k;
    }
    return capacity_behavior_t::fixed_k;
}

/** The reason the device transport reports when the suite ran without @c --device. */
constexpr char const* needs_device_k = "needs --device";

/** The reason the SPDK transport reports when the suite ran without @c --spdk-address. */
constexpr char const* needs_spdk_address_k = "needs --spdk-address";

/** The reason a case of several volumes reports on a transport that offers one medium. */
constexpr char const* one_volume_per_device_k = "a block device holds one volume";

/** What the machine and the flags offer a driver: the paths, the transports selected, and the seed. */
struct test_environment_t {

    /** The unique directory the file transports place their volumes in, or null without @c --directory. */
    char const* directory = nullptr;

    /** The block device the device transport opens, empty without @c --device. */
    std::string_view device;

    /** The PCIe address the SPDK transport binds, empty without @c --spdk-address. */
    std::string_view spdk_address;

    /** The transports @c --transports named, every one when empty. */
    texts_t transports;

    /** The seed every fixture's generator descends from. */
    std::uint64_t seed = 0;

    /** The medium @p options and the unique @p directory describe. */
    [[nodiscard]] static test_environment_t of(suite_options_t const& options,
                                               test_directory_t const& directory) noexcept {
        test_environment_t environment;
        environment.directory = directory.path();
        environment.device = options.device;
        environment.spdk_address = options.spdk_address;
        environment.transports = options.transports;
        environment.seed = options.seed.value;
        return environment;
    }

    /** Whether @c --transports named @p transport, which an absent flag does for every one. */
    [[nodiscard]] bool selects(transport_t transport) const noexcept {
        if (transports.size == 0) return true;
        for (std::string_view const name : transports.view())
            if (name == name_of(transport)) return true;
        return false;
    }
};

/** The magic @c statfs answers for a volume that lives in memory and enforces no alignment. */
constexpr std::uint64_t memory_filesystem_magic_k = 0x01021994ull;

/** Whether @p directory lives in memory, where @c O_DIRECT is accepted and alignment is ignored. */
[[nodiscard]] inline bool on_memory_filesystem(char const* directory) noexcept {
    struct statfs description {};
    if (::statfs(directory, &description) != 0) return false;
    return static_cast<std::uint64_t>(description.f_type) == memory_filesystem_magic_k;
}

/** Why this machine offers no io_uring, or null when it does. */
[[nodiscard]] inline char const* uring_refusal() noexcept {
    switch (uring_files_disk_t::probe()) {
    case uring_probe_t::too_old_k: return "needs io_uring of Linux 6.12 or newer";
    case uring_probe_t::refused_k: return "io_uring is refused on this machine";
    case uring_probe_t::available_k: return nullptr;
    }
    return nullptr;
}

/** Why @p transport cannot be mounted on @p environment, or null when it can; a path the flags claim is never a
 *  reason. */
[[nodiscard]] inline char const* transport_refusal(transport_t transport,
                                                   test_environment_t const& environment) noexcept {
    switch (transport) {
    case transport_t::memory_k: return nullptr;
    case transport_t::files_k: return environment.directory ? nullptr : needs_directory_k;
    case transport_t::io_uring_k:
        if (char const* const reason = uring_refusal()) return reason;
        if (!environment.directory) return needs_directory_k;
        if (on_memory_filesystem(environment.directory)) return "needs a filesystem that enforces O_DIRECT, not tmpfs";
        return nullptr;
    case transport_t::device_k: return environment.device.empty() ? needs_device_k : uring_refusal();
    case transport_t::spdk_k: return environment.spdk_address.empty() ? needs_spdk_address_k : nullptr;
    }
    return nullptr;
}

#pragma endregion Transports

#pragma region Shape

/** Every number a mount fixes, with the reason a suite moves it off the default beside each. */
struct fixture_config_t {

    /** Bytes of the page every header and slot begins on, which is also the alignment the RAM disk demands. */
    std::uint64_t page_bytes = portable_alignment_k;

    /** Bytes of one request block; the channels suite drops it to two pages so exhaustion is reachable. */
    std::uint64_t request_buffer_bytes = 64ull * 1024ull;

    /** Blocks one channel's request pool holds, kept above the depth so credit runs out first. */
    std::uint32_t request_buffers = 32;

    /** Bytes of one streaming block, which carries a whole index; the sorted run suite quadruples it. */
    std::uint64_t stream_buffer_bytes = 256ull * 1024ull;

    /** Streaming blocks one channel holds, which is the depth a merge writes at. */
    std::uint32_t stream_buffers = 3;

    /** Requests one queue pair keeps in flight. */
    std::uint32_t queue_depth = 16;

    /** Bytes one catalog slot spans; the volume and sorted run suites halve it so an overgrown catalog is reachable. */
    std::uint64_t catalog_slot_bytes = 256ull * 1024ull;

    /** Bytes every volume spans, which a block device ignores in favour of its own size. */
    std::uint64_t volume_bytes = 96ull * 1024ull * 1024ull;

    /** Bytes a large allocation rounds its first byte up to, which also places the data region. */
    std::uint64_t large_alignment = 1024ull * 1024ull;

    /** Spare channels the mount opens: one per channel a case holds at once, plus one per thread it spawns. */
    std::size_t shared_io_channels = 4;

    /** Executor channels the mount opens, one per thread, which only the channels suite raises above none. */
    std::size_t executor_threads = 0;

    /** Memory ranges the disk pins at once, which is the arena and one more for a case's own buffer. */
    std::uint16_t registered_memory_limit = 2;

    /** Bytes the budget every allocation of a fixture is charged to. */
    std::size_t memory_limit = 256ull * 1024ull * 1024ull;

    /** The channel config every channel of a fixture is carved to. */
    [[nodiscard]] constexpr io_channel_config_t channels() const noexcept {
        io_channel_config_t config;
        config.request_buffer_bytes = request_buffer_bytes;
        config.request_buffers_count = request_buffers;
        config.stream_buffer_bytes = stream_buffer_bytes;
        config.stream_buffers_count = stream_buffers;
        config.depth = queue_depth;
        return config;
    }

    /** The geometry a volume is formatted with, which @p recovery decides the repair policy of. */
    [[nodiscard]] volume_format_t format(volume_recovery_t recovery = volume_recovery_t::reformat_k) const noexcept {
        volume_format_t config;
        config.catalog_slot_bytes = catalog_slot_bytes;
        config.allocation_granularity = page_bytes;
        config.large_allocation_alignment = large_alignment;
        config.recovery = recovery;
        return config;
    }

    /** The ring config a disk of @p media volumes mounts with. */
    [[nodiscard]] constexpr disk_config_t disk(std::size_t media) const noexcept {
        disk_config_t config;
        config.queue_pairs_limit = static_cast<std::uint16_t>(executor_threads + shared_io_channels);
        config.depth_limit = queue_depth;
        config.registered_memory_limit = registered_memory_limit;
        config.media_limit = static_cast<std::uint16_t>(media);
        return config;
    }

    /** The same ring config for io_uring, which always runs unbuffered. */
    [[nodiscard]] constexpr uring_disk_config_t uring(std::size_t media) const noexcept {
        uring_disk_config_t config;
        config.queue_pairs_limit = static_cast<std::uint16_t>(executor_threads + shared_io_channels);
        config.depth_limit = queue_depth;
        config.registered_memory_limit = registered_memory_limit;
        config.media_limit = static_cast<std::uint16_t>(media);
        config.buffering = file_buffering_t::unbuffered_k;
        return config;
    }
};

#pragma endregion Shape

#pragma region Fixture

/** A mounted disk, its arena, its channels and the media the volumes of a case occupy, torn down in order. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
struct basic_fixture {

    using disk_t = disk_type_;
    using volume_t = basic_extent_volume<disk_type_>;
    using channel_pool_t = basic_io_channel_pool<disk_type_>;
    using guard_t = typename channel_pool_t::guard_t;

    /** The budgets every allocation is charged to, shared so a case can hold them past the fixture's death. */
    resources_shared_ptr_t resources;

    /** The ring every channel queue pairs from. */
    disk_type_ disk;

    /** The one registered range every block comes from. */
    registered_memory_arena_t arena;

    /** Every queue pair the mount opened. */
    channel_pool_t channels;

    /** One medium per volume, closed in the destructor. */
    st::basic_vector<registered_medium_t> media;

    /** The numbers this fixture was mounted with. */
    fixture_config_t config;

    /** The ring @c disk is. */
    transport_t transport;

    /** The generator every random draw of a case descends from. */
    test_random_t random;

    /** Bytes the budgets held once the mount finished, which a case hands back before its fixture dies. */
    std::size_t mounted_bytes = 0;

    /** Where a fixture dying with bytes still charged counts the leak, or null to count nowhere. */
    std::size_t* leaks = nullptr;

    basic_fixture(disk_type_&& mounted, fixture_config_t const& mounted_shape, transport_t mounted_on,
                  std::uint64_t seed) noexcept
        : resources(std::make_shared<resources_t>(mounted_shape.memory_limit)), disk(std::move(mounted)),
          config(mounted_shape), transport(mounted_on), random(seed) {}

    basic_fixture(basic_fixture const&) = delete;
    basic_fixture& operator=(basic_fixture const&) = delete;

    ~basic_fixture() noexcept {
        // Only the last owner can tell a leak from a budget a case still holds on purpose.
        if (leaks && resources.use_count() == 1)
            if (std::size_t const held = resources->memory_constraint.used(); held != mounted_bytes) {
                fmt::println(stderr, "  check failed: the budgets hold {} bytes past the mount's {}", held,
                             mounted_bytes);
                ++*leaks;
            }
        [[maybe_unused]] status_t const parked = channels.release_all();
        [[maybe_unused]] status_t const unpinned = arena.unregister(disk);
        for (std::size_t index = 0; index != media.size(); ++index)
            if (media[index].held()) [[maybe_unused]]
                status_t const closed = disk.unregister_medium(media[index]);
    }

    /** Mounts the volume of medium @p index, formatting a blank medium and treating a bad one as @p recovery says. */
    expected<volume_t> mount_volume(io_channel_t& channel, std::size_t index = 0,
                                    volume_recovery_t recovery = volume_recovery_t::reformat_k) noexcept {
        if (index >= media.size())
            return status_t {status_code_t::invalid_argument_k, "The fixture holds no medium of that index"};
        return volume_t::mount(disk, channel, media[index], config.format(recovery));
    }

    /** Whether the ring performs a request inside @c submit or only once the kernel hands it back. */
    [[nodiscard]] completion_timing_t completion_timing() const noexcept { return completion_timing_of(transport); }

    /** Whether a write past the end of a volume is refused or grows the environment. */
    [[nodiscard]] capacity_behavior_t capacity_behavior() const noexcept { return capacity_behavior_of(transport); }
};

/** Most volumes one fixture holds. */
constexpr std::size_t fixture_volumes_capacity_k = 16;

/** Opens the ring of @p disk_type_ shaped by @p config for @p media volumes. */
template <typename disk_type_>
expected<disk_type_> open_disk(fixture_config_t const& config, std::size_t media) noexcept {
    if constexpr (std::is_same_v<disk_type_, ram_disk_t>) {
        expected<ram_device_t> device = ram_device_t::make(config.page_bytes);
        if (!device) return device.status();
        return ram_disk_t::make(std::move(*device), config.disk(media));
    }
    else if constexpr (std::is_same_v<disk_type_, blocking_files_disk_t>) {
        expected<blocking_files_device_t> device = blocking_files_device_t::make(file_buffering_t::buffered_k);
        if (!device) return device.status();
        return blocking_files_disk_t::make(std::move(*device), config.disk(media));
    }
    else {
        static_assert(std::is_same_v<disk_type_, uring_files_disk_t>, "The driver mounts no other ring");
        return uring_files_disk_t::make(config.uring(media));
    }
}

/** Zeroes the head page of @p medium, so the next mount formats the medium as if it were new. */
template <typename disk_type_>
status_t blank_head(disk_type_& disk, io_channel_t& channel, medium_reference_t medium,
                    std::uint64_t page_bytes) noexcept {
    basic_write_pipeline<disk_type_> writer {disk, channel, medium};
    expected<pooled_buffer_t> block = writer.acquire_buffer();
    if (!block) return block.status();
    if (block->size() < page_bytes)
        return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than a volume's head"};
    std::memset(block->bytes().data(), 0, static_cast<std::size_t>(page_bytes));
    if (status_t const queued = writer.enqueue_write(std::move(*block), extent_t {0, page_bytes}); queued.failed())
        return queued;
    return writer.make_durable();
}

/**
 *  @brief Mounts a fresh fixture of one transport every time it is called, so every case starts on a blank environment.
 *
 *  A file medium is removed before it is created again, and a block device has its head page zeroed, so
 *  no case inherits the catalog its predecessor left; a volume in memory is blank by construction.
 */
template <typename disk_type_>
    requires ring_concept<disk_type_>
struct basic_mount {

    using fixture_t = basic_fixture<disk_type_>;
    using fixture_unique_ptr_t = std::unique_ptr<fixture_t>;

    /** The paths and the seed every fixture mounts with. */
    test_environment_t environment;

    /** The numbers every fixture mounts with unless a case hands its own. */
    fixture_config_t config;

    /** The ring every fixture mounts. */
    transport_t transport = transport_t::memory_k;

    /** The suite's name, which every file medium and every result line carries. */
    char const* suite = "";

    /** Volumes a fixture holds unless a case asks for its own capacities. */
    std::size_t volumes = 1;

    /** Where every fixture of this mount counts a leak, which the mount's @c report charges to the case. */
    std::size_t* leaks = nullptr;

    /** A fixture of @c volumes volumes of the suite's config. */
    expected<fixture_unique_ptr_t> operator()() const noexcept { return (*this)(config); }

    /** A fixture of @c volumes volumes of @p reshaped, for a case that moves a number off the suite's. */
    expected<fixture_unique_ptr_t> operator()(fixture_config_t const& reshaped) const noexcept {
        if (volumes > fixture_volumes_capacity_k)
            return status_t {status_code_t::invalid_argument_k, "A fixture holds fewer volumes than that"};
        std::array<std::uint64_t, fixture_volumes_capacity_k> capacities {};
        for (std::size_t index = 0; index != volumes; ++index) capacities[index] = reshaped.volume_bytes;
        return (*this)(reshaped, std::span<std::uint64_t const> {capacities.data(), volumes});
    }

    /** A fixture of one volume per entry of @p capacities, of the suite's config. */
    expected<fixture_unique_ptr_t> operator()(std::span<std::uint64_t const> capacities) const noexcept {
        return (*this)(config, capacities);
    }

    /** A fixture of one volume per entry of @p capacities, shaped by @p reshaped. */
    expected<fixture_unique_ptr_t> operator()(fixture_config_t const& reshaped,
                                              std::span<std::uint64_t const> capacities) const noexcept {
        if (char const* const reason = refusal(capacities.size()))
            return status_t {status_code_t::unsupported_k, reason};
        expected<disk_type_> mounted = open_disk<disk_type_>(reshaped, capacities.size());
        if (!mounted) return mounted.status();
        auto fixture = fixture_unique_ptr_t {
            new (std::nothrow) fixture_t {std::move(*mounted), reshaped, transport, environment.seed}};
        if (!fixture) return status_t {status_code_t::out_of_memory_k, "The fixture did not fit"};

        io_channel_config_t const channels = reshaped.channels();
        memory_arena_config_t arena;
        arena.bytes = channels.arena_bytes() * (reshaped.executor_threads + reshaped.shared_io_channels);
        expected<registered_memory_arena_t> pinned = registered_memory_arena_t::make(
            fixture->disk, fixture->resources->memory_constraint, arena);
        if (!pinned) return pinned.status();
        fixture->arena = std::move(*pinned);

        expected<typename fixture_t::channel_pool_t> pool = fixture_t::channel_pool_t::make(
            fixture->disk, fixture->arena, reshaped.executor_threads, reshaped.shared_io_channels, channels);
        if (!pool) return pool.status();
        fixture->channels = std::move(*pool);

        for (std::size_t index = 0; index != capacities.size(); ++index) {
            expected<registered_medium_t> registered = register_volume_(fixture->disk, index, capacities[index]);
            if (!registered) return registered.status();
            if (fixture->media.push_back(std::move(*registered)) != st::status_t::success_k)
                return status_t {status_code_t::out_of_memory_k, "The fixture could not track another medium"};
        }

        if (transport == transport_t::device_k) {
            expected<typename fixture_t::guard_t> guard = fixture->channels.acquire();
            if (!guard) return guard.status();
            if (status_t const blanked = blank_head(fixture->disk, guard->channel(), fixture->media[0].reference(),
                                                    reshaped.page_bytes);
                blanked.failed())
                return blanked;
        }
        fixture->mounted_bytes = fixture->resources->memory_constraint.used();
        fixture->leaks = leaks;
        return fixture;
    }

    /** Why this transport cannot hold @p count volumes, or null when it can. */
    [[nodiscard]] char const* refusal(std::size_t count) const noexcept {
        return transport == transport_t::device_k && count > 1 ? one_volume_per_device_k : nullptr;
    }

    /** The result line of the case @p name on this transport. */
    [[nodiscard]] case_label_t label(char const* name) const noexcept {
        return case_label_t {suite, name_of(transport), name};
    }

    /** Failures the fixtures of this mount counted as leaks since the last call, which it hands over and forgets. */
    [[nodiscard]] std::size_t take_leaks() const noexcept { return leaks ? std::exchange(*leaks, std::size_t {0}) : 0; }

  private:

    /** Registers the medium of volume @p index on @p disk: a blank buffer, a fresh file, or the device itself. */
    expected<registered_medium_t> register_volume_(disk_type_& disk, std::size_t index,
                                                   std::uint64_t capacity) const noexcept {
        medium_options_t request;
        request.length = capacity;
        request.access = medium_access_t::create_read_write_k;
        if (transport == transport_t::device_k) {
            request.name = environment.device.data();
            request.length = 0;
            request.access = medium_access_t::read_write_k;
            return disk.register_medium(request);
        }
        if (transport == transport_t::memory_k) {
            request.name = suite;
            return disk.register_medium(request);
        }
        std::array<char, 64> name {};
        auto const written = fmt::format_to_n(name.data(), name.size() - 1, "{}-{}.bin", suite, index);
        *written.out = '\0';
        test_path_t const path {environment.directory, name.data()};
        [[maybe_unused]] int const removed = std::remove(path.c_str());
        request.name = path.c_str();
        return disk.register_medium(request);
    }
};

#pragma endregion Fixture

#pragma region Driver

/** Runs @p case_function on @p mount as @c report does, failing it when a fixture it mounted died with bytes still
 *  charged to its budgets. */
template <typename disk_type_, typename result_type_>
std::size_t report_on_mount(suite_totals_t& totals, basic_mount<disk_type_> const& mount, char const* name,
                            result_type_ (*case_function)(basic_mount<disk_type_> const&)) {
    case_label_t const label = mount.label(name);
    if (!totals.selects(label.c_str())) return 0;
    case_result_t result(case_function(mount));
    result.failures += mount.take_leaks();
    return report(totals, label.c_str(), result);
}

/** A case of @p mount counting its failures, named without its template arguments. */
template <typename disk_type_>
std::size_t report(suite_totals_t& totals, basic_mount<disk_type_> const& mount, char const* name,
                   std::size_t (*case_function)(basic_mount<disk_type_> const&)) {
    return report_on_mount(totals, mount, name, case_function);
}

/** A case of @p mount that may skip, named without its template arguments. */
template <typename disk_type_>
std::size_t report(suite_totals_t& totals, basic_mount<disk_type_> const& mount, char const* name,
                   case_result_t (*case_function)(basic_mount<disk_type_> const&)) {
    return report_on_mount(totals, mount, name, case_function);
}

/** Mounts one fixture and lets it go, which is the first case of every transport. */
template <typename disk_type_>
std::size_t mounts_a_fixture(basic_mount<disk_type_> const& mount) {
    expected<typename basic_mount<disk_type_>::fixture_unique_ptr_t> const probe = mount();
    return expect_status(probe, status_code_t::success_k);
}

/** Mounts one probe fixture, reports it as the transport's @c mount case, and hands @p cases the mount when it held. */
template <typename disk_type_, typename cases_type_>
std::size_t run_on_mount(suite_totals_t& totals, basic_mount<disk_type_> const& mount, cases_type_&& cases) {
    std::size_t const failures = report(totals, mount, "mount", mounts_a_fixture);
    if (failures != 0) return failures;
    cases(mount);
    return 0;
}

/**
 *  @brief Runs @p cases on @p transport, or reports through @p totals why they did not run.
 *  @param[inout] totals Where the transport's mount and every case report.
 *  @param[in] transport The ring to mount, skipped when @c --transports names others.
 *  @param[in] medium The paths, the selection and the seed the flags gave.
 *  @param[in] config The numbers every fixture of the suite mounts with.
 *  @param[in] volumes Volumes every fixture holds, which a block device bounds at one.
 *  @param[in] suite The suite's name, which leads every result line.
 *  @param[in] cases Called with the mount, and reports each case it runs against it.
 *  @return The failures of the transport's own mount, which is zero when the cases ran or were skipped.
 */
template <typename cases_type_>
std::size_t run_on_transport(suite_totals_t& totals, transport_t transport, test_environment_t const& environment,
                             fixture_config_t const& config, std::size_t volumes, char const* suite,
                             cases_type_&& cases) {
    if (!environment.selects(transport)) return 0;
    case_label_t const label {suite, name_of(transport)};
    if (char const* const reason = transport_refusal(transport, environment))
        return report(totals, label.c_str(), case_result_t::skipped(reason));
    if (transport == transport_t::device_k && volumes > 1)
        return report(totals, label.c_str(), case_result_t::skipped(one_volume_per_device_k));

    std::size_t leaks = 0;
    switch (transport) {
    case transport_t::memory_k:
        return run_on_mount(totals, basic_mount<ram_disk_t> {environment, config, transport, suite, volumes, &leaks},
                            cases);
    case transport_t::files_k:
        return run_on_mount(
            totals, basic_mount<blocking_files_disk_t> {environment, config, transport, suite, volumes, &leaks}, cases);
    case transport_t::io_uring_k:
    case transport_t::device_k:
        return run_on_mount(
            totals, basic_mount<uring_files_disk_t> {environment, config, transport, suite, volumes, &leaks}, cases);
    case transport_t::spdk_k:
        // The driver mounts no SPDK disk: the transport skips even when `--spdk-address` names one.
        return report(totals, label.c_str(), case_result_t::skipped("the driver mounts no SPDK disk"));
    }
    return 0;
}

#pragma endregion Driver

#pragma region Byte Painters

/** The byte a value of @p seed holds at @p position, which a read checks every byte against. */
[[nodiscard]] inline byte_t painted(std::uint64_t seed, std::uint64_t position) noexcept {
    std::uint64_t mixed = position * 0x9E3779B97F4A7C15ull + seed * 0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull;
    mixed ^= mixed >> 29;
    return static_cast<byte_t>(mixed & 0xFFull);
}

/** Whether the two spans hold the same bytes. */
[[nodiscard]] inline bool same_bytes(std::span<byte_t const> first, std::span<byte_t const> second) noexcept {
    if (first.size() != second.size()) return false;
    for (std::size_t index = 0; index != first.size(); ++index)
        if (first[index] != second[index]) return false;
    return true;
}

/** One key of a model: what a lookup of it should answer, and the value it should carry. */
struct expectation_t {

    /** The key itself. */
    std::uint64_t stored_key = 0;

    /** What a lookup of that key should answer. */
    select_result_t state = select_result_t::nothing_k;

    /** Bytes the value should hold, which is zero for an erased or a present but empty value. */
    std::uint64_t bytes = 0;

    /** The seed the value's bytes are painted from. */
    std::uint64_t seed = 0;
};

/** Paints the value @p entry describes into @p into, which must already hold room for it. */
inline void paint_into(expectation_t const& entry, std::span<byte_t> into) noexcept {
    for (std::uint64_t position = 0; position != entry.bytes; ++position)
        into[static_cast<std::size_t>(position)] = painted(entry.seed, position);
}

#pragma endregion Byte Painters

} // namespace unum::ustore::test
