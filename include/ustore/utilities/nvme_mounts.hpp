/**
 *  @file include/ustore/utilities/nvme_mounts.hpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief The mount of one database's storage: the transport it probes, the disk erased over the
 *      file transports, the memory it pins, the volume it opens over every data directory, and the
 *      shared resources every collection charges.
 *
 *  A database owns exactly one mount, and everything it reads or writes hangs off it. The
 *  transport is chosen once, by setting a ring and an AIO context up and taking each down again
 *  rather than by reading a version number, because a kernel that has io_uring still refuses it
 *  under Docker's seccomp profile and under @c io_uring_disabled, and keeps AIO in both; what
 *  the probes settled on is reported back in @c mount_report_t rather than inferred by a
 *  caller. The three file transports are erased at the disk, and the branch is paid once per
 *  batch on @c submit and @c poll, each of which wraps a syscall.
 *
 *  Teardown is an order, not a destructor: a queue pair may not be released with work
 *  outstanding, a pinned range may not be unpinned while a request addresses it, and a medium may
 *  not be deregistered while one names it. So the channels drain first, the arena is unpinned
 *  next, and the volume closes its media last. The resources over a mount add the budgets.
 *
 *  A database is opened by its manifest: @c <database>.ustore.json in a directory, whose directory
 *  is data directory 0, or the raw address of data directory 0's first slot, such as
 *  @c /dev/nvme0n1@64GiB. Further data directories follow in the order they were first listed.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint16_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::min`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::shared_ptr`, `std::make_shared`
#include <new>         // placement `new`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::forward`, `std::move`
#include <variant>     // `std::variant`, `std::visit`, `std::get_if`, `std::monostate`, `std::in_place_type`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/nvme_channels.hpp" // `basic_io_channel_pool`, `io_channel_config_t`, `registered_memory_arena`
#include "ustore/utilities/nvme_disks.hpp"    // `blocking_files_disk`, `memory_disk`
#include "ustore/utilities/nvme_linux_aio_disk.hpp"   // `linux_aio_files_disk`, `linux_aio_probe_t`
#include "ustore/utilities/nvme_linux_spdk_disk.hpp"  // `linux_spdk_disk`, `linux_spdk_environment`
#include "ustore/utilities/nvme_linux_uring_disk.hpp" // `linux_uring_files_disk`, `linux_uring_probe_t`
#include "ustore/utilities/nvme_ring.hpp"             // `queue_pair_t`, `request_t`, `completion_t`, `ring_concept`
#include "ustore/utilities/nvme_volume.hpp"           // `any_volume`, `data_address_t`, `parse_data_address`
#include "ustore/utilities/ram_buffers.hpp" // `resource_constraint`, `constrained_allocator`, `unlimited_resource_k`
#include "ustore/utilities/types.hpp"       // `status_t`, `status_code_t`, `expected`, `byte_t`, `string_t`, `uuid_t`

#if !USTORE_OS_WINDOWS_
#include <sys/resource.h> // `getrlimit`, `setrlimit`, `RLIMIT_NOFILE`
#endif

namespace unum::ustore::utilities {

#pragma region Mount Configuration

/** A data directory runs are placed in, as an address @c parse_data_address reads, with the bytes
 *  they may take there, zero meaning the whole medium. */
struct data_directory_config_t {
    string_t path;
    std::size_t max_size = 0;
};

/**
 *  @brief The transport a mount opens its data directories through, probed rather than assumed.
 *
 *  The choice is made once, when the database comes up, and the transport a mount actually got is
 *  reported back rather than inferred, since @c automatic_k resolves differently on a kernel
 *  without io_uring, inside a seccomp profile that refuses it, and on a machine that has both.
 */
enum class io_transport_t : std::uint8_t {

    /** io_uring where the kernel offers it, Linux AIO where it offers only that, and the
     *  blocking floor elsewhere. */
    automatic_k,

    /** Positioned reads and writes on the polling thread, which every platform has. */
    blocking_k,

    /** io_uring on unbuffered files, refused without a kernel ring; an arena the memlock cap
     *  refuses goes unpinned. */
    linux_uring_k,

    /** Linux AIO on unbuffered files, which Docker's seccomp profile and kernels below 6.12 keep
     *  without a ring. */
    linux_aio_k,

    /** SPDK's user-space driver over the namespaces of one controller, which @c spdk:// data
     *  directories name and a build without SPDK refuses. */
    linux_spdk_k,

    /** Host memory, which a suite and a benchmark ask for and a database never does. */
    memory_k,
};

/**
 *  @brief Budgets and directories every collection of one database shares, as the NVMe init
 *      call names them.
 *
 *  @c threads counts the executor threads that each read on a channel of their own, and zero leaves
 *  only the shared channels.
 */
struct resources_config_t {
    std::size_t memory_limit = 4 * 1024 * 1024 * 1024ull;
    std::size_t threads = 0;
    std::size_t gpu_memory_limit = 0;
    st::vector<int> gpu_devices;

    /** The manifest, @c <database>.ustore.json, or the raw address of data directory
     *  0's first slot. */
    string_t manifest;
    io_transport_t io_transport = io_transport_t::automatic_k;
    io_channel_config_t channels;
    std::size_t shared_io_channels = 2;
    st::vector<data_directory_config_t> data_directories;
};

/** The data directories and the database name @p config names, data directory 0 first. */
struct volume_addresses_t {
    string_view_t database;
    st::vector<data_address_t> data_directories;
};

/**
 *  @brief Reads the data directories of @p config: data directory 0 is the manifest's directory,
 *      or the raw address the manifest names, and every listed data directory follows unless it
 *      is that one.
 *  @return The addresses, which borrow @p config, or @c invalid_argument_k for a manifest path
 *      without the suffix or an address that does not read.
 */
inline expected<volume_addresses_t> addresses_of(resources_config_t const& config) noexcept {
    volume_addresses_t addresses;
    std::string_view const manifest {config.manifest};
    auto [first, first_status] = parse_data_address(manifest);
    if (first_status.failed()) return first_status;
    if (first.kind == data_address_kind_t::directory_k) {
        if (!manifest.ends_with(manifest_suffix_k))
            return status_t {status_code_t::invalid_argument_k, "A manifest path ends with .ustore.json"};
        std::size_t const slash = manifest.find_last_of("/\\");
        std::size_t const name_begin = slash == std::string_view::npos ? 0 : slash + 1;
        first.location = slash == std::string_view::npos ? std::string_view {"."}
                         : slash == 0                    ? manifest.substr(0, 1)
                                                         : manifest.substr(0, slash);
        std::string_view const name = manifest.substr(name_begin,
                                                      manifest.size() - name_begin - manifest_suffix_k.size());
        addresses.database = string_view_t {name};
    }
    if (addresses.data_directories.reserve(config.data_directories.size() + 1) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "No room for the data directories"};
    [[maybe_unused]] st::status_t const placed = addresses.data_directories.push_back(st::assume_reserved,
                                                                                      data_address_t {first});
    for (data_directory_config_t const& directory : config.data_directories) {
        auto [address, address_status] = parse_data_address(std::string_view {directory.path});
        if (address_status.failed()) return address_status;
        address.byte_limit = address.byte_limit != 0 ? address.byte_limit : directory.max_size;
        if (address.location == first.location && address.namespace_id == first.namespace_id &&
            address.byte_base == first.byte_base) {
            addresses.data_directories[0].byte_limit = address.byte_limit;
            continue;
        }
        [[maybe_unused]] st::status_t const listed = addresses.data_directories.push_back(st::assume_reserved,
                                                                                          data_address_t {address});
    }
    return addresses;
}

#if USTORE_OS_WINDOWS_

/** Leaves the descriptor limit alone, which Windows does not impose on handles. */
inline std::size_t raise_open_files_limit() noexcept { return std::numeric_limits<std::size_t>::max(); }

#else

/** Raises the soft limit on open descriptors to the hard one, since a directory volume keeps a file
 *  open per run, and answers the descriptors the process may now hold. */
inline std::size_t raise_open_files_limit() noexcept {
    struct rlimit limit {};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) return std::numeric_limits<std::size_t>::max();
    if (limit.rlim_cur != limit.rlim_max) {
        rlim_t const soft = limit.rlim_cur;
        limit.rlim_cur = limit.rlim_max;
        if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) limit.rlim_cur = soft;
    }
    return limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur > std::numeric_limits<std::size_t>::max()
               ? std::numeric_limits<std::size_t>::max()
               : static_cast<std::size_t>(limit.rlim_cur);
}

#endif

#pragma endregion Mount Configuration

#pragma region Files Disk

#pragma region Disk

/**
 *  @brief The ring of a database on files or drives: the blocking floor, AIO or io_uring, as the
 *      mount probed, or SPDK, as the data directories named.
 *
 *  The io_uring, AIO and SPDK alternatives exist on every platform, as stubs that refuse where
 *  they cannot run, so the type is portable and only the mount decides which alternative is built.
 *
 *  @tparam atomic_reference_ The reference every alternative owns its shared words through.
 *  @tparam waiter_type_ What a draining core does after a poll that moved nothing.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class any_files_disk {
  public:

    /** The positioned reads and writes on the polling thread. */
    using blocking_disk_t = blocking_files_disk<atomic_reference_, waiter_type_>;

    /** The io_uring transport. */
    using linux_uring_disk_t = linux_uring_files_disk<atomic_reference_, waiter_type_>;

    /** The Linux AIO transport. */
    using linux_aio_disk_t = linux_aio_files_disk<atomic_reference_, waiter_type_>;

    /** The SPDK transport, over the namespaces of one controller. */
    using linux_spdk_disk_t = linux_spdk_disk<atomic_reference_, waiter_type_>;

    /** Takes the blocking transport in, which a mount builds where the probe refused a ring. */
    explicit any_files_disk(blocking_disk_t&& disk) noexcept
        : held_(std::in_place_type<blocking_disk_t>, std::move(disk)) {}

    /** Takes the io_uring transport in, which a mount builds where the probe found a ring. */
    explicit any_files_disk(linux_uring_disk_t&& disk) noexcept
        : held_(std::in_place_type<linux_uring_disk_t>, std::move(disk)) {}

    /** Takes the Linux AIO transport in, which a mount builds where the probe refused a ring
     *  and found AIO. */
    explicit any_files_disk(linux_aio_disk_t&& disk) noexcept
        : held_(std::in_place_type<linux_aio_disk_t>, std::move(disk)) {}

    /** Takes the SPDK transport in, which a mount builds for @c spdk:// data directories. */
    explicit any_files_disk(linux_spdk_disk_t&& disk) noexcept
        : held_(std::in_place_type<linux_spdk_disk_t>, std::move(disk)) {}

    any_files_disk(any_files_disk const&) = delete;
    any_files_disk& operator=(any_files_disk const&) = delete;
    any_files_disk(any_files_disk&&) noexcept = default;
    any_files_disk& operator=(any_files_disk&&) noexcept = default;

    /** The transport held, which is never @c automatic_k. */
    [[nodiscard]] io_transport_t transport() const noexcept {
        if (std::holds_alternative<linux_uring_disk_t>(held_)) return io_transport_t::linux_uring_k;
        if (std::holds_alternative<linux_aio_disk_t>(held_)) return io_transport_t::linux_aio_k;
        if (std::holds_alternative<linux_spdk_disk_t>(held_)) return io_transport_t::linux_spdk_k;
        return io_transport_t::blocking_k;
    }

    /** What the held transport accepts. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept {
        return std::visit([](auto const& disk) noexcept { return disk.capabilities(); }, held_);
    }

    /** How the held transport addresses the registered arena; the blocking floor and AIO
     *  pin per request. */
    [[nodiscard]] range_pinning_t pinning() const noexcept {
        linux_uring_disk_t const* const linux_uring = std::get_if<linux_uring_disk_t>(&held_);
        return linux_uring ? linux_uring->pinning() : range_pinning_t::unpinned_k;
    }

    /** Whether the arena must come from DMA memory, which only SPDK over a PCIe controller asks. */
    [[nodiscard]] bool needs_dma_memory() const noexcept {
        linux_spdk_disk_t const* const linux_spdk = std::get_if<linux_spdk_disk_t>(&held_);
        return linux_spdk && linux_spdk->needs_dma_memory();
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

    /** Moves at least one of @p queue pair's completions into @p into, sleeping in the kernel where
     *  the held transport can and polling where it cannot. */
    expected<std::size_t> wait_completions(queue_pair_t const& queue_pair, std::span<completion_t> into) noexcept {
        return std::visit(
            [&](auto& disk) noexcept { return utilities::wait_completions<waiter_type_>(disk, queue_pair, into); },
            held_);
    }

    /** Stops @p queue pair's requests that have not started. */
    void cancel(queue_pair_t const& queue_pair) noexcept {
        std::visit([&](auto& disk) noexcept { disk.cancel(queue_pair); }, held_);
    }

    /** Reaps and discards everything @p queue pair has in flight, so the queue pair can be released
     *  or handed over. */
    status_t drain(queue_pair_t const& queue_pair) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.drain(queue_pair); }, held_);
    }

    /** Gives back what the held transport shares with the process, which only SPDK does. */
    void release() noexcept {
        if (linux_spdk_disk_t* const spdk = std::get_if<linux_spdk_disk_t>(&held_)) spdk->release();
    }

#pragma endregion Ring

  private:

    /** The transport the probe chose, fixed for the life of the mount. */
    std::variant<blocking_disk_t, linux_uring_disk_t, linux_aio_disk_t, linux_spdk_disk_t> held_;
};

#pragma endregion Disk

#pragma endregion Files Disk

#pragma region Probe

/** What a mount opened, which is what the effective configuration of a database reports. */
struct mount_report_t {

    /** The transport the mount actually opened, which is never @c automatic_k. */
    io_transport_t transport = io_transport_t::blocking_k;

    /** What the kernel answered when the ring was set up and taken down again. */
    linux_uring_probe_t linux_uring = linux_uring_probe_t::refused_k;

    /** What the kernel answered when an AIO context was set up and taken down again. */
    linux_aio_probe_t linux_aio = linux_aio_probe_t::refused_k;

    /** Data directories the volume spans. */
    std::size_t data_directories = 0;

    /** Channels the pool opened, which is the executor's threads plus the shared channels. */
    std::size_t channels = 0;

    /** Bytes the arena pinned, charged to the host budget exactly once. */
    std::uint64_t arena_bytes = 0;
};

/**
 *  @brief The transport @p wanted resolves to, given what the two probes answered.
 *  @param[in] wanted The transport the caller named, or @c io_transport_t::automatic_k.
 *  @param[in] linux_uring What the io_uring probe answered.
 *  @param[in] linux_aio What the Linux AIO probe answered.
 *  @return The transport, or @c status_code_t::unsupported_k for io_uring named on a kernel that
 *      creates no ring.
 *
 *  The order is io_uring, then Linux AIO, then the blocking floor. A transport named outright
 *  starts at its own place in that order and only falls further down it, so asking for AIO never
 *  mounts a ring, but io_uring named outright never falls at all.
 */
inline expected<io_transport_t> probed_transport(io_transport_t wanted, linux_uring_probe_t linux_uring,
                                                 linux_aio_probe_t linux_aio) noexcept {
    bool const takes_linux_uring = wanted == io_transport_t::automatic_k || wanted == io_transport_t::linux_uring_k;
    if (takes_linux_uring && linux_uring == linux_uring_probe_t::available_k) return io_transport_t::linux_uring_k;
    if (wanted == io_transport_t::linux_uring_k)
        return status_t {status_code_t::unsupported_k, linux_uring == linux_uring_probe_t::too_old_k
                                                           ? "The io_uring transport needs Linux 6.12 or newer"
                                                           : "The kernel refuses io_uring rings"};
    if (!takes_linux_uring && wanted != io_transport_t::linux_aio_k) return wanted;
    return linux_aio == linux_aio_probe_t::available_k ? io_transport_t::linux_aio_k : io_transport_t::blocking_k;
}

#pragma endregion Probe

#pragma region Mount

/**
 *  @brief One transport, the memory it pinned, the channels it queue pairs and the volume it opened
 *      over every data directory.
 *
 *  It is deliberately neither copyable nor movable: a channel pool and the volume hold a pointer to
 *  the disk beside them, so a mount that moved would leave both addressing a transport that is
 *  gone. The variant above it emplaces one rather than assigning one for that reason.
 *
 *  @tparam atomic_reference_ The reference the budget and the channels' claims are owned through.
 *  @tparam waiter_type_ What a core waiting on the volume's reads and barriers does.
 */
template <typename disk_type_, template <typename> typename atomic_reference_, typename waiter_type_>
    requires ring_concept<disk_type_>
class basic_storage_mount {
  public:

    /** The byte allocator the volume's pools and manifests are charged to. */
    using allocator_t = constrained_allocator<byte_t, atomic_reference_>;

    /** The volume over every data directory: run files in directories, or slots of raw media. */
    using volume_t = any_volume<disk_type_, waiter_type_, allocator_t>;

    /** Every queue pair this mount hands out, one per executor thread plus the shared channels. */
    using channel_pool_t = basic_io_channel_pool<disk_type_, atomic_reference_, waiter_type_>;

    /** The one pinned range every I/O buffer is carved out of, charged to the host budget. */
    using arena_t = registered_memory_arena<atomic_reference_>;

    /** The host budget the arena is charged to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    /** Takes @p disk in, which a mount owns until it is torn down. */
    explicit basic_storage_mount(disk_type_&& disk) noexcept : disk_(std::move(disk)) {}

    basic_storage_mount(basic_storage_mount const&) = delete;
    basic_storage_mount& operator=(basic_storage_mount const&) = delete;
    basic_storage_mount(basic_storage_mount&&) = delete;
    basic_storage_mount& operator=(basic_storage_mount&&) = delete;

    ~basic_storage_mount() noexcept { [[maybe_unused]] status_t const released = unmount(); }

    /**
     *  @brief Pins the arena, queue pairs the channels and opens the volume over the data
     *      directories of @p config.
     *  @param[in] config The channel shape, the shared channels, the manifest and
     *      the data directories.
     *  @param[in,out] constraint The host budget the arena and the volume are charged to, for the
     *      life of the mount.
     *  @param[in] threads Executor threads, each of which gets a channel of its own.
     *  @param[in] runs_capacity Run files the volume keeps registered at once on directories.
     *  @return Success, or the first refusal, after which @c unmount gives back whatever was taken.
     */
    status_t mount(resources_config_t const& config, constraint_t& constraint, std::size_t threads,
                   std::size_t runs_capacity) noexcept {
        if (config.shared_io_channels == 0)
            return {status_code_t::invalid_argument_k, "A mount keeps a channel for the threads outside its executor"};
        auto [addresses, addresses_status] = addresses_of(config);
        if (addresses_status.failed()) return addresses_status;

        // A block is one request, so none outgrows what the disk moves in one transfer, as
        // SPDK's controllers bound
        io_channel_config_t channels = config.channels;
        if (std::uint64_t const transfer = disk_.capabilities().max_transfer_bytes; transfer != 0) {
            channels.request_buffer_bytes = std::min(channels.request_buffer_bytes, transfer);
            channels.stream_buffer_bytes = std::min(channels.stream_buffer_bytes, transfer);
        }

        memory_arena_config_t arena;
        arena.bytes = channels.arena_bytes() * (threads + config.shared_io_channels);
        auto [pinned, make_arena_status] = arena_t::make(disk_, constraint, arena);
        if (make_arena_status.failed()) return make_arena_status;
        arena_ = std::move(pinned);

        auto [acquired, make_channel_pool_status] = channel_pool_t::make(disk_, arena_.registration(), threads,
                                                                         config.shared_io_channels, channels);
        if (make_channel_pool_status.failed()) return make_channel_pool_status;
        channels_ = std::move(acquired);

        volume_config_t volume;
        volume.database = addresses.database;
        volume.data_directories = std::span<data_address_t const> {addresses.data_directories};
        volume.runs_capacity = runs_capacity;
        auto [guard, acquire_status] = channels_.acquire();
        if (acquire_status.failed()) return acquire_status;
        auto [opened, mount_status] = volume_t::mount(disk_, guard.channel(), volume, allocator_t {constraint});
        if (mount_status.failed()) return mount_status;
        volume_ = std::move(opened);
        data_directories_ = addresses.data_directories.size();
        return {};
    }

    /** Drains and releases the channels, unpins the arena, closes every medium of the volume,
     *  and gives back what the transport shares with the process. */
    status_t unmount() noexcept {
        status_t first;
        if (status_t const drained = channels_.release_all(); drained.failed() && first.succeeded()) first = drained;
        channels_ = channel_pool_t {};

        if (arena_.registered())
            if (status_t const unpinned = arena_.unregister(disk_); unpinned.failed() && first.succeeded())
                first = unpinned;
        arena_ = arena_t {};

        if (status_t const closed = volume_.unmount(); closed.failed() && first.succeeded()) first = closed;
        if constexpr (requires { disk_.release(); }) disk_.release();
        return first;
    }

    /** The transport underneath, which every read and write of a run goes to directly. */
    [[nodiscard]] disk_type_& disk() noexcept { return disk_; }

    /** The transport underneath, for what a caller only asks of it. */
    [[nodiscard]] disk_type_ const& disk() const noexcept { return disk_; }

    /** The volume over every data directory, which places runs and commits the manifest. */
    [[nodiscard]] volume_t& volume() noexcept { return volume_; }

    /** Data directories the volume spans. */
    [[nodiscard]] std::size_t data_directories() const noexcept { return data_directories_; }

    /** The queue pairs every operation borrows one of, by executor thread index or from
     *  the shared channels. */
    [[nodiscard]] channel_pool_t& channels() noexcept { return channels_; }

    /** The one pinned range every I/O buffer of this database is carved out of. */
    [[nodiscard]] arena_t& arena() noexcept { return arena_; }

  private:

    /** The transport, owned here and outliving every channel and the volume that point at it. */
    disk_type_ disk_;

    /** The one pinned range, charged to the host budget once and carved into blocks
     *  by the channels. */
    arena_t arena_;

    /** Every queue pair of this mount, opened here and lent one operation at a time. */
    channel_pool_t channels_;

    /** The volume over every data directory. */
    volume_t volume_;

    /** Data directories the volume spans. */
    std::size_t data_directories_ = 0;
};

/**
 *  @brief The storage of one database, in whichever transport the probe chose for it.
 *
 *  The probe runs once and the alternative it picked is emplaced in place, so the dispatch a
 *  caller pays is one @c visit per operation rather than one virtual call per request. A mount on
 *  files holds whichever of the three file transports the probe settled on, and a mount on
 *  @c spdk:// data directories holds the SPDK disk of their one controller, so a database
 *  materializes over one disk type.
 *
 *  @tparam atomic_reference_ The reference every transport, budget and channel owns
 *      its words through.
 *  @tparam waiter_type_ What a core waiting on a transport does between polls.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class mounted_storage {
  public:

    /** The ring a suite and a benchmark ask for, whose volumes never reach a medium. */
    using memory_disk_t = memory_disk<atomic_reference_, waiter_type_>;

    /** The ring a database gets, on unbuffered files through io_uring, Linux AIO or
     *  the blocking floor. */
    using files_disk_t = any_files_disk<atomic_reference_, waiter_type_>;

    /** The mount a suite and a benchmark ask for, whose volumes never reach a medium. */
    using memory_mount_t = basic_storage_mount<memory_disk_t, atomic_reference_, waiter_type_>;

    /** The mount a database gets, on unbuffered files through io_uring, Linux AIO or
     *  the blocking floor. */
    using files_mount_t = basic_storage_mount<files_disk_t, atomic_reference_, waiter_type_>;

    /** The host budget a mount charges its arena to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    mounted_storage() noexcept = default;
    mounted_storage(mounted_storage const&) = delete;
    mounted_storage& operator=(mounted_storage const&) = delete;

    /**
     *  @brief Probes the transport @p config asks for and mounts the whole storage stack on it.
     *  @param[in] config The transport, the channel shape and the data directories,
     *      with their capacities.
     *  @param[in,out] constraint The host budget the arena is charged to, once, for the life
     *      of the mount.
     *  @param[in] threads Executor threads, each of which gets a channel of its own.
     */
    status_t mount(resources_config_t const& config, constraint_t& constraint, std::size_t threads) noexcept {
        if (mounted()) return {status_code_t::already_exists_k, "This database has already mounted its storage"};
        std::size_t const descriptors = raise_open_files_limit();

        report_ = mount_report_t {};
        auto [addresses, addresses_status] = addresses_of(config);
        if (addresses_status.failed()) return addresses_status;
        bool const on_spdk = addresses.data_directories[0].kind == data_address_kind_t::spdk_k;
        for (data_address_t const& address : addresses.data_directories)
            if ((address.kind == data_address_kind_t::spdk_k) != on_spdk ||
                (on_spdk && address.location != addresses.data_directories[0].location))
                return {status_code_t::invalid_argument_k,
                        "SPDK data directories name one controller and nothing else"};
        if (on_spdk != (config.io_transport == io_transport_t::linux_spdk_k) &&
            config.io_transport != io_transport_t::automatic_k)
            return {status_code_t::invalid_argument_k,
                    "SPDK and spdk:// data directories are taken together or not at all"};

        report_.linux_uring = files_disk_t::linux_uring_disk_t::probe();
        report_.linux_aio = files_disk_t::linux_aio_disk_t::probe();
        auto [transport, transport_status] = on_spdk ? expected<io_transport_t> {io_transport_t::linux_spdk_k}
                                                     : probed_transport(config.io_transport, report_.linux_uring,
                                                                        report_.linux_aio);
        if (transport_status.failed()) return transport_status;
        report_.transport = transport;

        std::uint16_t const owners = bounded_count_(threads + config.shared_io_channels,
                                                    static_cast<std::size_t>(queue_pair_id_t::unallocated_k));
        if (owners == 0) return {status_code_t::capacity_exhausted_k, "A ring names fewer queue pairs than that"};
        // A directory volume registers a medium per run file, and the kernel refuses a file table
        // wider than the descriptor limit, which the rings and the volume's own files share.
        std::size_t const volume_files = 2; // the lock, and one manifest file or directory at a time
        std::size_t const reserved = owners + volume_files;
        if (descriptors <= reserved)
            return {status_code_t::capacity_exhausted_k, "The descriptor limit leaves no room for a run file"};
        std::uint16_t const targets = static_cast<std::uint16_t>(
            std::min<std::size_t>(descriptors - reserved, std::numeric_limits<std::uint16_t>::max() - 1));

        switch (report_.transport) {
        case io_transport_t::memory_k: {
            auto [device, make_memory_device_status] = memory_device_t::make(portable_alignment_k);
            if (make_memory_device_status.failed()) return make_memory_device_status;
            expected<memory_disk_t> disk = memory_disk_t::make(std::move(device), ring_config_(config, owners, targets),
                                                               constraint);
            if (!disk) return disk.status();
            return adopt_<memory_disk_t>(std::move(*disk), config, constraint, threads, targets);
        }
        case io_transport_t::blocking_k: return mount_blocking_(config, constraint, threads, owners, targets);
        case io_transport_t::linux_uring_k: return mount_linux_uring_(config, constraint, threads, owners, targets);
        case io_transport_t::linux_aio_k: return mount_linux_aio_(config, constraint, threads, owners, targets);
        case io_transport_t::linux_spdk_k:
            return mount_linux_spdk_(config, constraint, threads, owners, addresses.data_directories[0].location);
        case io_transport_t::automatic_k: return {status_code_t::internal_k, "The probe left the transport unresolved"};
        }
        return {status_code_t::invalid_argument_k, "The configuration names no I/O transport"};
    }

    /** Tears the mounted transport down in order, and leaves the handle naming none. */
    status_t unmount() noexcept {
        if (!mounted()) return {};
        status_t const released = visit([](auto& mount) noexcept { return mount.unmount(); });
        held_.template emplace<std::monostate>();
        report_ = mount_report_t {};
        return released;
    }

    /** Whether any transport is open, which every operation needs before it asks for a channel. */
    [[nodiscard]] bool mounted() const noexcept { return !std::holds_alternative<std::monostate>(held_); }

    /** What the mount opened, which a caller reports as the effective configuration. */
    [[nodiscard]] mount_report_t const& report() const noexcept { return report_; }

    /** How the mounted transport addresses its arena, which io_uring settles at each
     *  ring's first submission. */
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

    /** Mounts the blocking floor on unbuffered files, with @p owners queue pairs
     *  over @p targets directories. */
    status_t mount_blocking_(resources_config_t const& config, constraint_t& constraint, std::size_t threads,
                             std::uint16_t owners, std::uint16_t targets) noexcept {
        auto [device, make_device_status] = blocking_files_device_t::make(file_buffering_t::unbuffered_k);
        if (make_device_status.failed()) return make_device_status;
        using blocking_disk_t = typename files_disk_t::blocking_disk_t;
        expected<blocking_disk_t> disk = blocking_disk_t::make(std::move(device), ring_config_(config, owners, targets),
                                                               constraint);
        if (!disk) return disk.status();
        return adopt_<files_disk_t>(files_disk_t {std::move(*disk)}, config, constraint, threads, targets);
    }

    /** Mounts io_uring on unbuffered files, one ring per queue pair, with @p owners queue pairs
     *  over @p targets directories. */
    status_t mount_linux_uring_(resources_config_t const& config, constraint_t& constraint, std::size_t threads,
                                std::uint16_t owners, std::uint16_t targets) noexcept {
        // The arena registers as one table slot per gibibyte the kernel pins at most in one
        std::uint16_t const ranges = bounded_count_(
            divide_round_up(config.channels.arena_bytes() * owners, linux_uring_registered_range_bytes_k),
            static_cast<std::size_t>(registered_memory_id_t::unregistered_k));
        if (ranges == 0)
            return {status_code_t::capacity_exhausted_k, "A ring registers fewer ranges than the arena spans"};
        linux_uring_disk_config_t ring;
        ring.queue_pairs_limit = owners;
        ring.depth_limit = config.channels.depth;
        ring.registered_memory_limit = ranges;
        ring.media_limit = targets;
        ring.buffering = file_buffering_t::unbuffered_k;
        auto [disk, make_disk_status] = files_disk_t::linux_uring_disk_t::make(ring);
        if (make_disk_status.failed()) return make_disk_status;
        return adopt_<files_disk_t>(files_disk_t {std::move(disk)}, config, constraint, threads, targets);
    }

    /** Mounts Linux AIO on unbuffered files, one context per queue pair, with @p owners queue pairs
     *  over @p targets directories. */
    status_t mount_linux_aio_(resources_config_t const& config, constraint_t& constraint, std::size_t threads,
                              std::uint16_t owners, std::uint16_t targets) noexcept {
        linux_aio_disk_config_t contexts;
        contexts.queue_pairs_limit = owners;
        contexts.depth_limit = config.channels.depth;
        contexts.registered_memory_limit = 1;
        contexts.media_limit = targets;
        contexts.buffering = file_buffering_t::unbuffered_k;
        auto [disk, make_disk_status] = files_disk_t::linux_aio_disk_t::make(contexts);
        if (make_disk_status.failed()) return make_disk_status;
        return adopt_<files_disk_t>(files_disk_t {std::move(disk)}, config, constraint, threads, targets);
    }

    /**
     *  @brief Mounts SPDK on the controller at @p controller, sharing the process's connection
     *      to it, with @p owners queue pairs of its own.
     *  @note The probe reads files alone and runs first, since the environment it guards calls
     *      @c exit on the process when the machine is not ready for it.
     */
    status_t mount_linux_spdk_(resources_config_t const& config, constraint_t& constraint, std::size_t threads,
                               std::uint16_t owners, std::string_view controller) noexcept {
        using linux_spdk_disk_t = typename files_disk_t::linux_spdk_disk_t;
        using environment_t = linux_spdk_environment<atomic_reference_, waiter_type_>;
        char address[path_max_bytes_k];
        if (controller.size() >= sizeof(address))
            return {status_code_t::invalid_argument_k, "An SPDK controller's address is too long"};
        std::memcpy(address, controller.data(), controller.size());
        address[controller.size()] = '\0';
        switch (environment_t::probe(address)) {
        case linux_spdk_probe_t::available_k: break;
        case linux_spdk_probe_t::without_hugepages_k:
            return {status_code_t::unsupported_k, "The SPDK controller needs free hugepages"};
        case linux_spdk_probe_t::without_permission_k:
            return {status_code_t::unsupported_k, "The SPDK controller is not bound to a user-space driver"};
        case linux_spdk_probe_t::not_found_k: return {status_code_t::not_found_k, "No SPDK controller at that address"};
        }
        linux_spdk_environment_config_t pages;
        pages.memory = spdk_transport_named(address) == linux_spdk_transport_t::fabric_k
                           ? linux_spdk_memory_t::plain_pages_k
                           : linux_spdk_memory_t::hugepages_k;
        auto [environment, environment_status] = environment_t::acquire(pages);
        if (environment_status.failed()) return environment_status;
        linux_spdk_disk_config_t ring;
        ring.queue_pairs_limit = owners;
        ring.depth_limit = config.channels.depth;
        ring.registered_memory_limit = 1;
        auto [disk, open_status] = linux_spdk_disk_t::open(std::move(environment), address, ring);
        if (open_status.failed()) return open_status;
        return adopt_<files_disk_t>(files_disk_t {std::move(disk)}, config, constraint, threads, 0);
    }

    /** @p wanted as the 16-bit count a ring names things by, or zero when @p reserved
     *  leaves no room. */
    [[nodiscard]] static std::uint16_t bounded_count_(std::size_t wanted, std::size_t reserved) noexcept {
        return wanted != 0 && wanted < reserved ? static_cast<std::uint16_t>(wanted) : std::uint16_t {0};
    }

    /** Emplaces the mount of @p disk, opens the storage on it and records what it got. */
    template <typename disk_type_>
    status_t adopt_(disk_type_&& disk, resources_config_t const& config, constraint_t& constraint, std::size_t threads,
                    std::size_t runs_capacity) noexcept {
        using mount_t = basic_storage_mount<disk_type_, atomic_reference_, waiter_type_>;
        mount_t& opened = held_.template emplace<mount_t>(std::move(disk));
        if (status_t const started = opened.mount(config, constraint, threads, runs_capacity); started.failed()) {
            held_.template emplace<std::monostate>();
            return started;
        }
        report_.data_directories = opened.data_directories();
        report_.channels = opened.channels().size();
        report_.arena_bytes = opened.arena().size();
        return {};
    }

    /** The one mount this database opened, or @c std::monostate while it has opened none. */
    std::variant<std::monostate, memory_mount_t, files_mount_t> held_;

    /** What the probe settled on and what the mount took, reported as the
     *  effective configuration. */
    mount_report_t report_;
};

#pragma endregion Mount

#pragma region Resources

#pragma region Shared Resources

/**
 *  @brief Budgets and services every collection of one database shares.
 *  @tparam atomic_reference_ The reference the budgets and the storage own their words through.
 *  @tparam waiter_type_ What a core waiting on the storage does between polls.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
struct shared_resources {

    /** Budget of host memory. */
    resource_constraint<atomic_reference_> memory_constraint;

    /** Budget of GPU memory, zero when no GPU is used. */
    resource_constraint<atomic_reference_> gpu_memory_constraint;

    /** GPU devices compaction may use. */
    st::vector<int> gpu_devices;

    /** The transport, the pinned arena, the channels and the volumes mounted over
     *  the data directories. */
    mounted_storage<atomic_reference_, waiter_type_> storage;

    /** Sets the memory budgets, leaving the services for the caller to attach. */
    explicit shared_resources(std::size_t memory_limit, std::size_t gpu_memory_limit = 0) noexcept
        : memory_constraint(memory_limit), gpu_memory_constraint(gpu_memory_limit) {}
};

/** The resources of one database, held by the database and every collection and buffer charged
 *  to its budgets. */
template <template <typename> typename atomic_reference_, typename waiter_type_>
using resources_shared_ptr = std::shared_ptr<shared_resources<atomic_reference_, waiter_type_>>;

/** Constructs @p object_type_ in memory charged to the host budget, or returns an empty pointer
 *  once it runs out. */
template <typename object_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          typename... argument_types_>
std::shared_ptr<object_type_> make_constrained_shared(
    resources_shared_ptr<atomic_reference_, waiter_type_> const& resources, argument_types_&&... arguments) noexcept {
    constrained_allocator<byte_t, atomic_reference_> allocator {resources->memory_constraint};
    auto* const pointer = reinterpret_cast<object_type_*>(allocator.allocate(sizeof(object_type_)));
    if (!pointer) return {};

    auto deleter = [allocator](object_type_* constructed) mutable {
        constructed->~object_type_();
        allocator.deallocate(reinterpret_cast<byte_t*>(constructed), sizeof(object_type_));
    };
    return std::shared_ptr<object_type_>(new (pointer) object_type_(std::forward<argument_types_>(arguments)...),
                                         deleter);
}

/**
 *  @brief Mounts the storage of one database over the manifest and the data directories
 *      @p config names.
 *  @param[in] config Budgets, executor threads, the manifest and the data directories to mount.
 *  @return The resources, whose destructor tears the mount down in the order it was built in.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
expected<resources_shared_ptr<atomic_reference_, waiter_type_>> open_resources(
    resources_config_t const& config) noexcept {
    if (config.manifest.empty()) return status_t {status_code_t::invalid_argument_k, "A database needs a manifest"};

    using resources_t = shared_resources<atomic_reference_, waiter_type_>;
    resources_shared_ptr<atomic_reference_, waiter_type_> resources = std::make_shared<resources_t>(
        config.memory_limit, config.gpu_memory_limit);
    if (resources->gpu_devices.resize(config.gpu_devices.size()) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "A database could not list the GPUs it may use"};
    for (std::size_t index = 0; index != config.gpu_devices.size(); ++index)
        resources->gpu_devices[index] = config.gpu_devices[index];

    if (status_t const mounted = resources->storage.mount(config, resources->memory_constraint, config.threads);
        mounted.failed())
        return mounted;
    return resources;
}

#pragma endregion Shared Resources

#pragma endregion Resources

} // namespace unum::ustore::utilities
