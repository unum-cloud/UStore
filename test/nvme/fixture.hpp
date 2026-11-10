/**
 *  @file test/nvme/fixture.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief One mounted ring for every engine suite: its config, the transports, the driver and the byte painters.
 *
 *  A suite names its config once, writes every case against the mount it is handed, and lets @c run_on_transport
 *  run the same cases on memory, on files through the blocking floor, io_uring and Linux AIO, and on a block
 *  device. A transport the machine does not offer skips with its reason; a transport the flags claim and the
 *  mount then refuses fails.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint16_t`, `std::uint64_t`
#include <cstdio>  // `std::remove`
#include <cstring> // `std::memset`

#include "ustore/types.h" // `USTORE_OS_LINUX_`

#if USTORE_OS_LINUX_
#include <sys/vfs.h> // `statfs`
#endif

#include <array>       // `std::array`
#include <filesystem>  // `std::filesystem::directory_iterator`, `std::filesystem::copy`, `std::filesystem::rename`
#include <memory>      // `std::unique_ptr`, `std::make_shared`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`
#include <utility>     // `std::exchange`, `std::move`

#include <forkunion/atomics.hpp>       // `fu::standard_atomic_ref`
#include <forkunion/types.hpp>         // `fu::standard_yield_t`
#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/nvme_disks.hpp"            // `blocking_files_disk`
#include "ustore/utilities/nvme_linux_aio_disk.hpp"   // `linux_aio_files_disk`, `linux_aio_probe_t`
#include "ustore/utilities/nvme_linux_uring_disk.hpp" // `linux_uring_files_disk`, `linux_uring_probe_t`

#include "ustore/utilities/nvme_channels.hpp"  // `registered_memory_arena`, `memory_arena_config_t`
#include "ustore/blobs/nvme_store.hpp"         // `lookup_result_t`, `commit_ledger`, `stamp_pin`
#include "ustore/utilities/nvme_mounts.hpp"    // `shared_resources`, `resources_shared_ptr`
#include "ustore/utilities/blocking_files.hpp" // `read_file`, `file_buffering_t`
#include "ustore/utilities/nvme_manifest.hpp"  // `manifest_t`, `manifest_file_t`
#include "ustore/utilities/nvme_volume.hpp"    // `basic_directory_volume`, `volume_config_t`, `data_address_t`

#include "harness.hpp" // `expect_status`, `suite_totals_t`

namespace unum::ustore::test::nvme {

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Instantiation

/** The atomic reference every suite instantiates the engine's templates with: the portable one, as no suite picks a
 *  CPU class. */
template <typename value_type_>
using atomic_reference = fu::standard_atomic_ref<value_type_>;

/** The waiter every suite instantiates the engine's templates with, yielding between attempts. */
using waiter_t = fu::standard_yield_t;

using memory_disk_t = memory_disk<atomic_reference, waiter_t>;
using blocking_files_disk_t = blocking_files_disk<atomic_reference, waiter_t>;
using linux_uring_files_disk_t = linux_uring_files_disk<atomic_reference, waiter_t>;
using linux_aio_files_disk_t = linux_aio_files_disk<atomic_reference, waiter_t>;
using resource_constraint_t = resource_constraint<atomic_reference>;
using registered_memory_arena_t = registered_memory_arena<atomic_reference>;
using resources_t = shared_resources<atomic_reference, waiter_t>;
using resources_shared_ptr_t = resources_shared_ptr<atomic_reference, waiter_t>;
using commit_ledger_t = commit_ledger<atomic_reference, waiter_t>;
using stamp_pin_t = stamp_pin<atomic_reference, waiter_t>;
using countdown_t = countdown<atomic_reference, waiter_t>;
using volume_allocator_t = constrained_allocator<byte_t, atomic_reference>;

/** Where among @p store_types_ each tag's store sits, every store of a suite's database tagged by
 *  its value layout. */
template <typename... store_types_>
struct layout_store_tags {

    /** The index of the store whose values follow the layout @p store_tag spells, or the count of the stores. */
    static constexpr std::size_t index_of(std::uint16_t store_tag) noexcept {
        std::uint16_t const tags[] {static_cast<std::uint16_t>(store_types_::key_slot_t::value_layout_k)...};
        std::size_t index = 0;
        while (index != sizeof...(store_types_) && tags[index] != store_tag) ++index;
        return index;
    }
};

#pragma endregion Instantiation

#pragma region Transports

/** The rings a suite mounts, each run when the machine and the flags offer it. */
enum class transport_t : std::uint8_t {

    /** Volumes in host memory, held to the alignment a drive demands. */
    memory_k,

    /** Files under @c --directory, through positioned reads and writes on the polling thread. */
    blocking_k,

    /** Unbuffered files under @c --directory, through io_uring. */
    linux_uring_k,

    /** Unbuffered files under @c --directory, through Linux AIO. */
    linux_aio_k,

    /** The block device @c --device names, through io_uring. */
    device_k,

    /** The namespace @c --spdk-address binds, which the driver does not mount. */
    linux_spdk_k,
};

/** Every transport, in the order a suite runs them. */
constexpr std::array<transport_t, 6> every_transport_k {transport_t::memory_k,      transport_t::blocking_k,
                                                        transport_t::linux_uring_k, transport_t::linux_aio_k,
                                                        transport_t::device_k,      transport_t::linux_spdk_k};

/** The name @c --transports selects @p transport by, which is also the middle of every result line. */
[[nodiscard]] constexpr char const* name_of(transport_t transport) noexcept {
    switch (transport) {
    case transport_t::memory_k: return "memory";
    case transport_t::blocking_k: return "blocking";
    case transport_t::linux_uring_k: return "linux-uring";
    case transport_t::linux_aio_k: return "linux-aio";
    case transport_t::device_k: return "device";
    case transport_t::linux_spdk_k: return "linux-spdk";
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
    case transport_t::blocking_k: return completion_timing_t::synchronous_k;
    case transport_t::linux_uring_k:
    case transport_t::linux_aio_k:
    case transport_t::device_k:
    case transport_t::linux_spdk_k: return completion_timing_t::asynchronous_k;
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
    case transport_t::blocking_k:
    case transport_t::linux_uring_k:
    case transport_t::linux_aio_k: return capacity_behavior_t::growing_k;
    case transport_t::memory_k:
    case transport_t::device_k:
    case transport_t::linux_spdk_k: return capacity_behavior_t::fixed_k;
    }
    return capacity_behavior_t::fixed_k;
}

/** The reason the device transport reports when the suite ran without @c --device. */
constexpr char const* needs_device_k = "needs --device";

/** The reason the SPDK transport reports when the suite ran without @c --spdk-address. */
constexpr char const* needs_spdk_address_k = "needs --spdk-address";

/** The reason a case of several volumes reports on a transport that offers one medium. */
constexpr char const* one_volume_per_device_k = "a block device holds one volume";

/** The reason a case that remounts a volume reports on memory, whose run media die
 *  with the volume. */
constexpr char const* memory_volumes_vanish_k = "memory volumes do not outlive their mount";

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
    [[nodiscard]] static test_environment_t of(suite_options_t const& options, directory_t const& directory) noexcept {
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
[[nodiscard]] inline bool on_memory_filesystem([[maybe_unused]] char const* directory) noexcept {
#if USTORE_OS_LINUX_
    struct statfs description {};
    if (::statfs(directory, &description) != 0) return false;
    return static_cast<std::uint64_t>(description.f_type) == memory_filesystem_magic_k;
#else
    return false;
#endif
}

/** Why this machine offers no io_uring, or null when it does. */
[[nodiscard]] inline char const* uring_refusal() noexcept {
    switch (linux_uring_files_disk_t::probe()) {
    case linux_uring_probe_t::too_old_k: return "needs io_uring of Linux 6.12 or newer";
    case linux_uring_probe_t::refused_k: return "io_uring is refused on this machine";
    case linux_uring_probe_t::available_k: return nullptr;
    }
    return nullptr;
}

/** Why this machine offers no Linux AIO, or null when it does. */
[[nodiscard]] inline char const* aio_refusal() noexcept {
    return linux_aio_files_disk_t::probe() == linux_aio_probe_t::available_k ? nullptr
                                                                             : "Linux AIO is refused on this machine";
}

/** Why @p transport cannot be mounted on @p environment, or null when it can; a path the flags claim is never a
 *  reason. */
[[nodiscard]] inline char const* transport_refusal(transport_t transport,
                                                   test_environment_t const& environment) noexcept {
    switch (transport) {
    case transport_t::memory_k: return nullptr;
    case transport_t::blocking_k: return environment.directory ? nullptr : needs_directory_k;
    case transport_t::linux_uring_k:
    case transport_t::linux_aio_k:
        if (char const* const reason = transport == transport_t::linux_uring_k ? uring_refusal() : aio_refusal())
            return reason;
        if (!environment.directory) return needs_directory_k;
        if (on_memory_filesystem(environment.directory)) return "needs a filesystem that enforces O_DIRECT, not tmpfs";
        return nullptr;
    case transport_t::device_k: return environment.device.empty() ? needs_device_k : uring_refusal();
    case transport_t::linux_spdk_k: return environment.spdk_address.empty() ? needs_spdk_address_k : nullptr;
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

    /** Bytes every raw medium spans, which a block device ignores in favour of its own size. */
    std::uint64_t volume_bytes = 96ull * 1024ull * 1024ull;

    /** Run files a volume keeps registered at once, which the ring's media and the run pools
     *  are sized by. */
    std::size_t runs_capacity = 256;

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

    /** The ring config a disk of @p media volumes mounts with. */
    [[nodiscard]] constexpr disk_config_t disk(std::size_t media) const noexcept {
        disk_config_t config;
        config.queue_pairs_limit = static_cast<std::uint16_t>(executor_threads + shared_io_channels);
        config.depth_limit = queue_depth;
        config.registered_memory_limit = registered_memory_limit;
        config.media_limit = static_cast<std::uint16_t>(media + runs_capacity);
        return config;
    }

    /** The same ring config for io_uring, which always runs unbuffered. */
    [[nodiscard]] constexpr linux_uring_disk_config_t linux_uring(std::size_t media) const noexcept {
        linux_uring_disk_config_t config;
        config.queue_pairs_limit = static_cast<std::uint16_t>(executor_threads + shared_io_channels);
        config.depth_limit = queue_depth;
        config.registered_memory_limit = registered_memory_limit;
        config.media_limit = static_cast<std::uint16_t>(media + runs_capacity);
        config.buffering = file_buffering_t::unbuffered_k;
        return config;
    }

    /** The same ring config for Linux AIO, which always runs unbuffered too. */
    [[nodiscard]] constexpr linux_aio_disk_config_t linux_aio(std::size_t media) const noexcept {
        linux_aio_disk_config_t config;
        config.queue_pairs_limit = static_cast<std::uint16_t>(executor_threads + shared_io_channels);
        config.depth_limit = queue_depth;
        config.registered_memory_limit = registered_memory_limit;
        config.media_limit = static_cast<std::uint16_t>(media + runs_capacity);
        config.buffering = file_buffering_t::unbuffered_k;
        return config;
    }
};

#pragma endregion Shape

#pragma region Fixture

/** Most volumes, and most data directories of one volume, one fixture holds. */
constexpr std::size_t fixture_volumes_capacity_k = 16;

/** A mounted disk, its arena, its channels and the media the volumes of a case occupy, torn down in order. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
struct basic_fixture {

    using disk_t = disk_type_;
    using volume_t = basic_directory_volume<disk_type_, waiter_t, volume_allocator_t>;
    using channel_pool_t = basic_io_channel_pool<disk_type_, atomic_reference, waiter_t>;
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
    st::vector<registered_medium_t> media;

    /** The numbers this fixture was mounted with. */
    fixture_config_t config;

    /** The ring @c disk is. */
    transport_t transport;

    /** The generator every random draw of a case descends from. */
    random_t random;

    /** Bytes the budgets held once the mount finished, which a case hands back before its fixture dies. */
    std::size_t mounted_bytes = 0;

    /** Where a fixture dying with bytes still charged counts the leak, or null to count nowhere. */
    std::size_t* leaks = nullptr;

    /** The directory every volume of the fixture keeps its data directories in, or null
     *  without @c --directory. */
    char const* directory = nullptr;

    /** The suite's name, which names the database and the data directories of every volume. */
    char const* suite = "";

    basic_fixture(resources_shared_ptr_t&& budgets, disk_type_&& mounted, fixture_config_t const& mounted_shape,
                  transport_t mounted_on, std::uint64_t seed) noexcept
        : resources(std::move(budgets)), disk(std::move(mounted)), config(mounted_shape), transport(mounted_on),
          random(seed) {}

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

    /** Mounts the directory volume of the suite's database over @p data_directories directories,
     *  which a second call reopens from its manifest. */
    expected<volume_t> mount_volume(io_channel_t& channel, std::size_t data_directories = 1) noexcept {
        if (!directory) return status_t {status_code_t::unsupported_k, needs_directory_k};
        if (data_directories == 0 || data_directories > fixture_volumes_capacity_k)
            return status_t {status_code_t::invalid_argument_k, "A fixture's volume spans fewer directories"};
        st::vector<path_t> paths;
        std::array<data_address_t, fixture_volumes_capacity_k> addresses {};
        if (paths.reserve(data_directories) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for a volume's paths"};
        for (std::size_t index = 0; index != data_directories; ++index) {
            [[maybe_unused]] st::status_t const placed = paths.push_back(st::assume_reserved,
                                                                         data_directory_path(index));
            addresses[index].location = paths[index].c_str();
        }
        volume_config_t volume;
        volume.database = string_view_t {suite};
        volume.data_directories = std::span<data_address_t const> {addresses.data(), data_directories};
        volume.runs_capacity = config.runs_capacity;
        return volume_t::mount(disk, channel, volume, volume_allocator_t {resources->memory_constraint});
    }

    /** The path of data directory @p index of every volume of this fixture. */
    [[nodiscard]] path_t data_directory_path(std::size_t index) const noexcept {
        std::array<char, 96> name {};
        auto const written = fmt::format_to_n(name.data(), name.size() - 1, "{}.volume-{}", suite, index);
        *written.out = '\0';
        return path_t {directory ? directory : ".", name.data()};
    }

    /** Whether a volume remounted on this fixture finds the runs its predecessor wrote, which
     *  memory media do not. */
    [[nodiscard]] bool persists_volumes() const noexcept { return transport != transport_t::memory_k; }

    /** Whether the ring performs a request inside @c submit or only once the kernel hands it back. */
    [[nodiscard]] completion_timing_t completion_timing() const noexcept { return completion_timing_of(transport); }

    /** Whether a write past the end of a volume is refused or grows the environment. */
    [[nodiscard]] capacity_behavior_t capacity_behavior() const noexcept { return capacity_behavior_of(transport); }
};

/** Opens the ring of @p disk_type_ shaped by @p config for @p media volumes, its pools charged
 *  to @p constraint. */
template <typename disk_type_>
expected<disk_type_> open_disk(fixture_config_t const& config, std::size_t media,
                               resource_constraint_t& constraint) noexcept {
    if constexpr (std::is_same_v<disk_type_, memory_disk_t>) {
        auto [device, make_memory_device_status] = memory_device_t::make(config.page_bytes);
        if (make_memory_device_status.failed()) return make_memory_device_status;
        return memory_disk_t::make(std::move(device), config.disk(media), constraint);
    }
    else if constexpr (std::is_same_v<disk_type_, blocking_files_disk_t>) {
        auto [device, make_device_status] = blocking_files_device_t::make(file_buffering_t::buffered_k);
        if (make_device_status.failed()) return make_device_status;
        return blocking_files_disk_t::make(std::move(device), config.disk(media), constraint);
    }
    else if constexpr (std::is_same_v<disk_type_, linux_aio_files_disk_t>)
        return linux_aio_files_disk_t::make(config.linux_aio(media));
    else {
        static_assert(std::is_same_v<disk_type_, linux_uring_files_disk_t>, "The driver mounts no other ring");
        return linux_uring_files_disk_t::make(config.linux_uring(media));
    }
}

/** Zeroes the head page of @p medium, so the next mount formats the medium as if it were new. */
template <typename disk_type_>
status_t blank_head(disk_type_& disk, io_channel_t& channel, medium_reference_t medium,
                    std::uint64_t page_bytes) noexcept {
    auto [writer_channel, writer_channel_status] = channel.acquire();
    if (writer_channel_status.failed()) return writer_channel_status;
    basic_write_pipeline<disk_type_, waiter_t> writer {disk, std::move(writer_channel), medium};
    auto [block, acquire_buffer_status] = writer.acquire_buffer();
    if (acquire_buffer_status.failed()) return acquire_buffer_status;
    if (block.size() < page_bytes)
        return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than a volume's head"};
    std::memset(block.bytes().data(), 0, static_cast<std::size_t>(page_bytes));
    if (status_t const queued = writer.enqueue_write(std::move(block), extent_t {0, page_bytes}); queued.failed())
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
        resources_shared_ptr_t resources = std::make_shared<resources_t>(reshaped.memory_limit);
        expected<disk_type_> mounted = open_disk<disk_type_>(reshaped, capacities.size(), resources->memory_constraint);
        if (!mounted) return mounted.status();
        auto fixture = fixture_unique_ptr_t {new (std::nothrow) fixture_t {std::move(resources), std::move(*mounted),
                                                                           reshaped, transport, environment.seed}};
        if (!fixture) return status_t {status_code_t::out_of_memory_k, "The fixture did not fit"};
        fixture->directory = environment.directory;
        fixture->suite = suite;
        // Every case starts on empty data directories, so no volume inherits its
        // predecessor's manifest.
        if (environment.directory)
            for (std::size_t index = 0; index != fixture_volumes_capacity_k; ++index)
                if (!recreate_directory(fixture->data_directory_path(index).c_str()))
                    return status_t {status_code_t::io_k, "A fixture's data directory would not be emptied"};

        io_channel_config_t const channels = reshaped.channels();
        memory_arena_config_t arena;
        arena.bytes = channels.arena_bytes() * (reshaped.executor_threads + reshaped.shared_io_channels);
        auto [pinned, make_arena_status] = registered_memory_arena_t::make(
            fixture->disk, fixture->resources->memory_constraint, arena);
        if (make_arena_status.failed()) return make_arena_status;
        fixture->arena = std::move(pinned);

        auto [pool, make_status] = fixture_t::channel_pool_t::make(fixture->disk, fixture->arena.registration(),
                                                                   reshaped.executor_threads,
                                                                   reshaped.shared_io_channels, channels);
        if (make_status.failed()) return make_status;
        fixture->channels = std::move(pool);

        for (std::size_t index = 0; index != capacities.size(); ++index) {
            auto [registered, register_volume_status] = register_volume_(fixture->disk, index, capacities[index]);
            if (register_volume_status.failed()) return register_volume_status;
            if (fixture->media.push_back(std::move(registered)) != st::status_t::success_k)
                return status_t {status_code_t::out_of_memory_k, "The fixture could not track another medium"};
        }

        if (transport == transport_t::device_k) {
            auto [guard, acquire_status] = fixture->channels.acquire();
            if (acquire_status.failed()) return acquire_status;
            if (status_t const blanked = blank_head(fixture->disk, guard.channel(), fixture->media[0].reference(),
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
        path_t const path {environment.directory, name.data()};
        [[maybe_unused]] int const removed = std::remove(path.c_str());
        request.name = path.c_str();
        return disk.register_medium(request);
    }
};

#pragma endregion Fixture

#pragma region Manifests

/** Publishes a manifest naming @p files through @p volume, as the next generation of the
 *  one it read. */
template <typename volume_type_, typename fixture_type_>
status_t publish_naming(volume_type_& volume, fixture_type_& fixture, io_channel_t& channel,
                        std::span<manifest_file_t const> files) noexcept {
    manifest_t manifest;
    if (sz::failed(manifest.database.assign(volume.manifest().database.view())))
        return {status_code_t::out_of_memory_k, "No room for the database's name"};
    manifest.id = volume.manifest().id;
    manifest.generation = volume.generation() + 1;
    manifest.placement = volume.placement();
    for (manifest_file_t const& file : files)
        if (manifest.files.push_back(manifest_file_t {file}) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for the manifest's files"};
    st::vector<char> json;
    if (status_t const printed = manifest.print(volume_allocator_t {fixture.resources->memory_constraint}, json);
        printed.failed())
        return printed;
    return volume.publish_manifest(channel, std::span<char const> {json});
}

/** The manifest entry of @p extent, written for @p run, as user data of no collection. */
[[nodiscard]] inline manifest_file_t file_of(medium_extent_t const& extent, uuid128_t const& run) noexcept {
    manifest_file_t file;
    file.run = run;
    file.offset = extent.extent.offset;
    file.bytes = extent.extent.length;
    file.medium = extent.data_directory;
    return file;
}

/** The manifest the database of @p fixture last published, or an empty one before its first. */
template <typename fixture_type_>
[[nodiscard]] manifest_t manifest_on_disk(fixture_type_ const& fixture) noexcept {
    manifest_t manifest;
    std::array<char, 96> name {};
    auto const written = fmt::format_to_n(name.data(), name.size() - 1, "{}.ustore.json", fixture.suite);
    *written.out = '\0';
    path_t const path {fixture.data_directory_path(0).c_str(), name.data()};
    st::vector<char> text;
    if (read_file(path.c_str(), text).succeeded()) [[maybe_unused]]
        status_t const parsed = manifest.parse(text, volume_allocator_t {fixture.resources->memory_constraint});
    return manifest;
}

/** Runs the last published manifest of @p fixture names, which a reopen neither adds
 *  to nor loses. */
template <typename fixture_type_>
[[nodiscard]] std::size_t run_count_of(fixture_type_ const& fixture) noexcept {
    manifest_t const manifest = manifest_on_disk(fixture);
    std::size_t counted = 0;
    for (manifest_file_t const& file : manifest.files) counted += file.collection != manifest_no_collection_k;
    return counted;
}

/** Bytes every run the last published manifest of @p fixture names takes. */
template <typename fixture_type_>
[[nodiscard]] std::uint64_t run_bytes_of(fixture_type_ const& fixture) noexcept {
    manifest_t const manifest = manifest_on_disk(fixture);
    std::uint64_t counted = 0;
    for (manifest_file_t const& file : manifest.files)
        if (file.collection != manifest_no_collection_k) counted += file.bytes;
    return counted;
}

/** Run files in the first data directory of @p fixture, which a crash may leave more of than
 *  the manifest names. */
template <typename fixture_type_>
[[nodiscard]] std::size_t run_files_of(fixture_type_ const& fixture) noexcept {
    std::error_code error;
    std::size_t counted = 0;
    for (std::filesystem::directory_entry const& entry :
         std::filesystem::directory_iterator {fixture.data_directory_path(0).c_str(), error})
        counted += entry.path().extension() == ".ustore";
    return counted;
}

#pragma endregion Manifests

#pragma region Crash Copies

/** The path a crash copy of the data directory of @p fixture is kept at. */
template <typename fixture_type_>
[[nodiscard]] path_t crash_copy_path(fixture_type_ const& fixture) noexcept {
    std::array<char, 96> name {};
    auto const written = fmt::format_to_n(name.data(), name.size() - 1, "{}.crash", fixture.suite);
    *written.out = '\0';
    return path_t {fixture.directory, name.data()};
}

/** Copies the data directory of @p fixture aside, which is what a crash would leave on it
 *  at this moment. */
template <typename fixture_type_>
status_t capture_data_directory(fixture_type_ const& fixture) noexcept {
    path_t const live = fixture.data_directory_path(0), aside = crash_copy_path(fixture);
    std::error_code error;
    std::filesystem::remove_all(aside.c_str(), error);
    std::filesystem::copy(live.c_str(), aside.c_str(), std::filesystem::copy_options::recursive, error);
    return error ? status_t {status_code_t::io_k, "The data directory would not be copied"} : status_t {};
}

/** Puts the copy @c capture_data_directory took back in place of the data directory, as a restart
 *  after a crash finds it. */
template <typename fixture_type_>
status_t restore_data_directory(fixture_type_ const& fixture) noexcept {
    path_t const live = fixture.data_directory_path(0), aside = crash_copy_path(fixture);
    std::error_code error;
    std::filesystem::remove_all(live.c_str(), error);
    if (!error) std::filesystem::rename(aside.c_str(), live.c_str(), error);
    return error ? status_t {status_code_t::io_k, "The data directory would not be restored"} : status_t {};
}

#pragma endregion Crash Copies

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
 *  @param[in,out] totals Where the transport's mount and every case report.
 *  @param[in] transport The ring to mount, skipped when @c --transports names others.
 *  @param[in] environment The paths, the selection and the seed the flags gave.
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
        return run_on_mount(totals, basic_mount<memory_disk_t> {environment, config, transport, suite, volumes, &leaks},
                            cases);
    case transport_t::blocking_k:
        return run_on_mount(
            totals, basic_mount<blocking_files_disk_t> {environment, config, transport, suite, volumes, &leaks}, cases);
    case transport_t::linux_uring_k:
    case transport_t::device_k:
        return run_on_mount(
            totals, basic_mount<linux_uring_files_disk_t> {environment, config, transport, suite, volumes, &leaks},
            cases);
    case transport_t::linux_aio_k:
        return run_on_mount(
            totals, basic_mount<linux_aio_files_disk_t> {environment, config, transport, suite, volumes, &leaks},
            cases);
    case transport_t::linux_spdk_k:
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
    lookup_result_t state = lookup_result_t::missing_k;

    /** Bytes the value should hold, which is zero for a removed or a present but empty value. */
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

} // namespace unum::ustore::test::nvme
