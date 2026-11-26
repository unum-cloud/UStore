/**
 *  @file include/ustore/utilities/nvme_mounts.hpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief The mount of one database's storage: the transport it probes, the disk erased over the
 *      file transports, the volume it opens over every data directory, and the shared resources
 *      every collection charges.
 *
 *  A database owns exactly one mount, and everything it reads or writes hangs off it. The
 *  transport is chosen once, by setting a ring and an AIO context up and taking each down again
 *  rather than by reading a version number, because a kernel that has io_uring still refuses it
 *  under Docker's seccomp profile and under @c io_uring_disabled, and keeps AIO in both; what
 *  the probes settled on is reported back in @c mount_report_t rather than inferred by a
 *  caller. The three file transports are erased at the disk, and the branch is paid once per
 *  batch on @c submit and @c poll, each of which wraps a syscall.
 *
 *  Teardown is an order, not a destructor: a medium may not be deregistered while a request names
 *  it. So every call drains its queue set's channels before it returns, and the volume closes its
 *  media last. The resources over a mount add the budgets.
 *
 *  A database is opened by its manifest: @c <database>.ustore.json in a directory, whose directory
 *  is data directory 0, or the raw address of data directory 0's first slot, such as
 *  @c /dev/nvme0n1@64GiB. Further data directories follow in the order they were first listed.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint16_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <limits>      // `std::numeric_limits`
#include <new>         // placement `new`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::forward`, `std::move`
#include <variant>     // `std::variant`, `std::visit`, `std::get_if`, `std::monostate`, `std::in_place_type`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/nvme_channels.hpp"         // `io_channel_config_t`
#include "ustore/utilities/nvme_disks.hpp"            // `blocking_files_disk`, `memory_disk`
#include "ustore/utilities/nvme_queues.hpp"           // `io_transport_t`, `probed_transport`
#include "ustore/utilities/nvme_linux_aio_disk.hpp"   // `linux_aio_files_disk`, `linux_aio_probe_t`
#include "ustore/utilities/nvme_linux_spdk_disk.hpp"  // `linux_spdk_disk`, `linux_spdk_environment`
#include "ustore/utilities/nvme_linux_uring_disk.hpp" // `linux_uring_files_disk`, `linux_uring_probe_t`
#include "ustore/utilities/nvme_ring.hpp"             // `queue_ref_t`, `request_t`, `completion_t`, `a_disk`
#include "ustore/utilities/nvme_volume.hpp"           // `any_volume`, `data_address_t`, `parse_data_address`
#include "ustore/utilities/ram_buffers.hpp" // `resource_constraint`, `constrained_allocator`, `unlimited_resource_k`
#include "ustore/utilities/types.hpp"       // `status_t`, `status_code_t`, `expected`, `byte_t`, `string_t`

#if USTORE_OS_POSIX_
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
 *  @brief Budgets and directories every collection of one database shares, as the NVMe init
 *      call names them.
 *
 *  The database holds no channel: @c channels is the shape of the queue set that opened it, checked
 *  by the validation as a queue set's own shape is.
 */
struct resources_config_t {
    std::size_t memory_limit = 4 * 1024 * 1024 * 1024ull;
    std::size_t gpu_memory_limit = 0;
    st::vector<int> gpu_devices;

    /** The manifest, @c <database>.ustore.json, or the raw address of data directory
     *  0's first slot. */
    string_t manifest;
    io_transport_t io_transport = io_transport_t::automatic_k;
    io_channel_config_t channels;
    st::vector<data_directory_config_t> data_directories;

    /** How the volume treats its media, @c read_only_k for a view. */
    medium_access_t access = medium_access_t::create_read_write_k;
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
 *  @return The addresses, which view @p config, or @c invalid_argument_k for a manifest path
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
inline std::size_t raise_open_files_limit() noexcept { return (std::numeric_limits<std::size_t>::max)(); }

#else

/** Raises the soft limit on open descriptors to the hard one, since a directory volume keeps a file
 *  open per run, and answers the descriptors the process may now hold. */
inline std::size_t raise_open_files_limit() noexcept {
    struct rlimit limit {};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) return (std::numeric_limits<std::size_t>::max)();
    if (limit.rlim_cur != limit.rlim_max) {
        rlim_t const soft = limit.rlim_cur;
        limit.rlim_cur = limit.rlim_max;
        if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) limit.rlim_cur = soft;
    }
    return limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur > (std::numeric_limits<std::size_t>::max)()
               ? (std::numeric_limits<std::size_t>::max)()
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
        : probed_disk_(std::in_place_type<blocking_disk_t>, std::move(disk)) {}

    /** Takes the io_uring transport in, which a mount builds where the probe found a ring. */
    explicit any_files_disk(linux_uring_disk_t&& disk) noexcept
        : probed_disk_(std::in_place_type<linux_uring_disk_t>, std::move(disk)) {}

    /** Takes the Linux AIO transport in, which a mount builds where the probe refused a ring
     *  and found AIO. */
    explicit any_files_disk(linux_aio_disk_t&& disk) noexcept
        : probed_disk_(std::in_place_type<linux_aio_disk_t>, std::move(disk)) {}

    /** Takes the SPDK transport in, which a mount builds for @c spdk:// data directories. */
    explicit any_files_disk(linux_spdk_disk_t&& disk) noexcept
        : probed_disk_(std::in_place_type<linux_spdk_disk_t>, std::move(disk)) {}

    any_files_disk(any_files_disk const&) = delete;
    any_files_disk& operator=(any_files_disk const&) = delete;
    any_files_disk(any_files_disk&&) noexcept = default;
    any_files_disk& operator=(any_files_disk&&) noexcept = default;

    /** The transport held, which is never @c automatic_k. */
    [[nodiscard]] io_transport_t transport() const noexcept {
        if (std::holds_alternative<linux_uring_disk_t>(probed_disk_)) return io_transport_t::linux_uring_k;
        if (std::holds_alternative<linux_aio_disk_t>(probed_disk_)) return io_transport_t::linux_aio_k;
        if (std::holds_alternative<linux_spdk_disk_t>(probed_disk_)) return io_transport_t::linux_spdk_k;
        return io_transport_t::blocking_k;
    }

    /** What the held transport accepts. */
    [[nodiscard]] disk_capabilities_t capabilities() const noexcept {
        return std::visit([](auto const& disk) noexcept { return disk.capabilities(); }, probed_disk_);
    }

    /** Whether a queue set's blocks must come from DMA memory, which only SPDK over a
     *  PCIe controller asks. */
    [[nodiscard]] bool needs_dma_memory() const noexcept {
        linux_spdk_disk_t const* const linux_spdk = std::get_if<linux_spdk_disk_t>(&probed_disk_);
        return linux_spdk && linux_spdk->needs_dma_memory();
    }

    /** The memory domain of the held controller's PCI node, or -1 for a transport without one. */
    [[nodiscard]] int memory_domain() const noexcept {
        linux_spdk_disk_t const* const linux_spdk = std::get_if<linux_spdk_disk_t>(&probed_disk_);
        return linux_spdk ? linux_spdk->memory_domain() : -1;
    }

#pragma region Queue Pairs

    /** Installs the held transport's media into @p queue_pair, which costs nothing while it already
     *  holds them. */
    status_t register_media(queue_ref_t const& queue_pair) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.register_media(queue_pair); }, probed_disk_);
    }

    /** Requests @p queue pair has submitted and not yet completed. */
    [[nodiscard]] std::size_t outstanding(queue_ref_t const& queue_pair) const noexcept {
        return std::visit([&](auto const& disk) noexcept { return disk.outstanding(queue_pair); }, probed_disk_);
    }

#pragma endregion Queue Pairs

#pragma region Registrations

    /** Pins @p memory so requests can address it, refusing @c gpu_k on both file transports. */
    expected<registered_memory_t> register_memory(std::span<byte_t> memory, memory_space_t space) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.register_memory(memory, space); }, probed_disk_);
    }

    /** Unpins @p registered memory, refusing while requests still address it. */
    status_t unregister_memory(registered_memory_t& registered_memory) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.unregister_memory(registered_memory); }, probed_disk_);
    }

    /** Opens the file @p request names as a medium requests address by slot. */
    expected<registered_medium_t> register_medium(medium_options_t const& request) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.register_medium(request); }, probed_disk_);
    }

    /** Closes @p registered medium's file, refusing while requests still name it. */
    status_t unregister_medium(registered_medium_t& registered_medium) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.unregister_medium(registered_medium); }, probed_disk_);
    }

#pragma endregion Registrations

#pragma region Ring

    /** Takes as much of @p requests as @p queue pair still has credit for, and never waits. */
    expected<std::size_t> submit(queue_ref_t const& queue_pair, std::span<request_t const> requests) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.submit(queue_pair, requests); }, probed_disk_);
    }

    /** Moves @p queue pair's ready completions into @p into, and never waits. */
    expected<std::size_t> poll_completions(queue_ref_t const& queue_pair, std::span<completion_t> into) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.poll_completions(queue_pair, into); }, probed_disk_);
    }

    /** Moves at least one of @p queue pair's completions into @p into, sleeping in the kernel where
     *  the held transport can and polling where it cannot. */
    expected<std::size_t> wait_completions(queue_ref_t const& queue_pair, std::span<completion_t> into) noexcept {
        return std::visit(
            [&](auto& disk) noexcept { return util::wait_completions<waiter_type_>(disk, queue_pair, into); },
            probed_disk_);
    }

    /** Stops @p queue pair's requests that have not started. */
    void cancel(queue_ref_t const& queue_pair) noexcept {
        std::visit([&](auto& disk) noexcept { disk.cancel(queue_pair); }, probed_disk_);
    }

    /** Reaps and discards everything @p queue pair has in flight, so the queue pair can be released
     *  or handed over. */
    status_t drain(queue_ref_t const& queue_pair) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.drain(queue_pair); }, probed_disk_);
    }

    /** Gives back what the held transport shares with the process, which only SPDK does. */
    void release() noexcept {
        if (linux_spdk_disk_t* const spdk = std::get_if<linux_spdk_disk_t>(&probed_disk_)) spdk->release();
    }

#pragma endregion Ring

  private:

    /** The transport the probe chose, fixed for the life of the mount. */
    std::variant<blocking_disk_t, linux_uring_disk_t, linux_aio_disk_t, linux_spdk_disk_t> probed_disk_;
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
};

#pragma endregion Probe

#pragma region Mount

/**
 *  @brief One transport and the volume it opened over every data directory, holding no channel,
 *      since every call registers the disk's media on its own queue set.
 *
 *  It is deliberately neither copyable nor movable: the volume holds a pointer to the disk beside
 *  it, so a mount that moved would leave it addressing a transport that is gone. The variant above
 *  it emplaces one rather than assigning one for that reason.
 *
 *  @tparam atomic_reference_ The reference the budget is owned through.
 *  @tparam waiter_type_ What a core waiting on the volume's reads and barriers does.
 */
template <typename disk_type_, template <typename> typename atomic_reference_, typename waiter_type_>
    requires a_disk<disk_type_>
class basic_storage_mount {
  public:

    /** The transport the mount owns. */
    using disk_t = disk_type_;

    /** The byte allocator the volume's pools and manifests are charged to. */
    using allocator_t = constrained_allocator<byte_t, atomic_reference_>;

    /** The volume over every data directory: run files in directories, or slots of raw media. */
    using volume_t = any_volume<disk_type_, waiter_type_, allocator_t>;

    /** The host budget the volume is charged to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    /** Takes @p disk in, which a mount owns until it is torn down. */
    explicit basic_storage_mount(disk_type_&& disk) noexcept : disk_(std::move(disk)) {}

    basic_storage_mount(basic_storage_mount const&) = delete;
    basic_storage_mount& operator=(basic_storage_mount const&) = delete;
    basic_storage_mount(basic_storage_mount&&) = delete;
    basic_storage_mount& operator=(basic_storage_mount&&) = delete;

    ~basic_storage_mount() noexcept { [[maybe_unused]] status_t const released = unmount(); }

    /**
     *  @brief Opens the volume over the data directories of @p config, on the writing channel of
     *      the thread of @p queues.
     *  @param[in] config The manifest and the data directories.
     *  @param[in,out] constraint The host budget the volume is charged to, for the life
     *      of the mount.
     *  @param[in] runs_capacity Run files the volume keeps registered at once on directories.
     *  @param[in] queues The opening call's queue set, whose queues register this disk's media.
     *  @return Success, or the first refusal, after which @c unmount gives back whatever was taken.
     */
    status_t mount(resources_config_t const& config, constraint_t& constraint, std::size_t runs_capacity,
                   nvme_queues_ref_t queues) noexcept {
        auto [addresses, addresses_status] = addresses_of(config);
        if (addresses_status.failed()) return addresses_status;

        volume_config_t volume;
        volume.database = addresses.database;
        volume.data_directories = std::span<data_address_t const> {addresses.data_directories};
        volume.runs_capacity = runs_capacity;
        volume.access = config.access;
        auto [channels, media_status] = queues.both_channels(disk_);
        if (media_status.failed()) return media_status;
        auto [opened, mount_status] = volume_t::mount(disk_, *channels.writing, volume, allocator_t {constraint});
        [[maybe_unused]] status_t const drained = disk_.drain(channels.writing->queue_pair());
        if (mount_status.failed()) return mount_status;
        volume_ = std::move(opened);
        data_directories_ = addresses.data_directories.size();
        return {};
    }

    /** Closes every medium of the volume and gives back what the transport shares with the
     *  process. */
    status_t unmount() noexcept {
        status_t const closed = volume_.unmount();
        if constexpr (a_releasing_disk<disk_type_>) disk_.release();
        return closed;
    }

    /** The transport underneath, which every read and write of a run goes to directly. */
    [[nodiscard]] disk_type_& disk() noexcept { return disk_; }

    /** The transport underneath, for what a caller only asks of it. */
    [[nodiscard]] disk_type_ const& disk() const noexcept { return disk_; }

    /** The volume over every data directory, which places runs and commits the manifest. */
    [[nodiscard]] volume_t& volume() noexcept { return volume_; }

    /** Data directories the volume spans. */
    [[nodiscard]] std::size_t data_directories() const noexcept { return data_directories_; }

  private:

    /** The transport, owned here and outliving the volume that points at it. */
    disk_type_ disk_;

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
 *  @tparam atomic_reference_ The reference every transport and budget owns its words through.
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

    /** The host budget a mount charges its volume to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    mounted_storage() noexcept = default;
    mounted_storage(mounted_storage const&) = delete;
    mounted_storage& operator=(mounted_storage const&) = delete;

    /**
     *  @brief Probes the transport @p config asks for and mounts the whole storage stack on it.
     *  @param[in] config The transport and the data directories,
     *      with their capacities.
     *  @param[in,out] constraint The host budget the volume is charged to, for the life
     *      of the mount.
     *  @param[in] queues The opening call's queue set, which reads the manifest.
     */
    status_t mount(resources_config_t const& config, constraint_t& constraint, nvme_queues_ref_t queues) noexcept {
        if (mounted()) return {status_code_t::already_exists_k, "This database has already mounted its storage"};
        std::size_t const descriptors = raise_open_files_limit();

        report_ = mount_report_t {};
        auto [addresses, addresses_status] = addresses_of(config);
        if (addresses_status.failed()) return addresses_status;
        bool const on_spdk = addresses.data_directories[0].kind == data_address_kind_t::spdk_k;
        for (data_address_t const& address : addresses.data_directories)
            if ((address.kind == data_address_kind_t::spdk_k) != on_spdk ||
                (on_spdk && address.location != addresses.data_directories[0].location))
                return {status_code_t::invalid_argument_k, "SPDK directories must share one controller"};
        if (on_spdk != (config.io_transport == io_transport_t::linux_spdk_k) &&
            config.io_transport != io_transport_t::automatic_k)
            return {status_code_t::invalid_argument_k, "The transport and SPDK directories disagree"};

        report_.linux_uring = files_disk_t::linux_uring_disk_t::probe();
        report_.linux_aio = files_disk_t::linux_aio_disk_t::probe();
        auto [transport, transport_status] = on_spdk ? expected<io_transport_t> {io_transport_t::linux_spdk_k}
                                                     : probed_transport(config.io_transport, report_.linux_uring,
                                                                        report_.linux_aio);
        if (transport_status.failed()) return transport_status;
        report_.transport = transport;

        // A directory volume registers a medium per run file, and the kernel refuses a file
        // table wider than the descriptor limit, which the volume's own files share.
        std::size_t const reserved = 2; // the lock, and one manifest file or directory at a time
        if (descriptors <= reserved)
            return {status_code_t::capacity_exhausted_k, "The descriptor limit leaves no run file"};
        std::uint16_t const targets = static_cast<std::uint16_t>(
            fu::min_of<std::size_t>(descriptors - reserved, (std::numeric_limits<std::uint16_t>::max)() - 1));

        switch (report_.transport) {
        case io_transport_t::memory_k: {
            auto [device, make_memory_device_status] = memory_device_t::make(portable_alignment_k);
            if (make_memory_device_status.failed()) return make_memory_device_status;
            auto [disk, make_disk_status] = memory_disk_t::make(std::move(device), ring_config_(targets), constraint);
            if (make_disk_status.failed()) return make_disk_status;
            return adopt_<memory_disk_t>(std::move(disk), config, constraint, queues, targets);
        }
        case io_transport_t::blocking_k: return mount_blocking_(config, constraint, queues, targets);
        case io_transport_t::linux_uring_k: return mount_linux_uring_(config, constraint, queues, targets);
        case io_transport_t::linux_aio_k: return mount_linux_aio_(config, constraint, queues, targets);
        case io_transport_t::linux_spdk_k:
            return mount_linux_spdk_(config, constraint, queues, addresses.data_directories[0].location);
        case io_transport_t::automatic_k: return {status_code_t::internal_k, "The probe left the transport unresolved"};
        }
        return {status_code_t::invalid_argument_k, "The configuration names no I/O transport"};
    }

    /** Tears the mounted transport down in order, and leaves the handle naming none. */
    status_t unmount() noexcept {
        if (!mounted()) return {};
        status_t const released = visit([](auto& mount) noexcept { return mount.unmount(); });
        probed_disk_.template emplace<std::monostate>();
        report_ = mount_report_t {};
        return released;
    }

    /** Whether any transport is open, which every operation needs before it registers the disk. */
    [[nodiscard]] bool mounted() const noexcept { return !std::holds_alternative<std::monostate>(probed_disk_); }

    /** What the mount opened, which a caller reports as the effective configuration. */
    [[nodiscard]] mount_report_t const& report() const noexcept { return report_; }

    /** Calls @p visitor with the mount that is open, or reports that none is. */
    template <typename visitor_type_>
    status_t visit(visitor_type_&& visitor) noexcept {
        if (memory_mount_t* const ram = std::get_if<memory_mount_t>(&probed_disk_)) return visitor(*ram);
        if (files_mount_t* const files = std::get_if<files_mount_t>(&probed_disk_)) return visitor(*files);
        return {status_code_t::uninitialized_k, "This database has mounted no storage"};
    }

  private:

    /** The ring shape every transport is made with, which the targets fix. */
    [[nodiscard]] static disk_config_t ring_config_(std::uint16_t targets) noexcept {
        disk_config_t ring;
        ring.registered_memory_limit = 1;
        ring.media_limit = targets;
        return ring;
    }

    /** Mounts the blocking floor on unbuffered files over @p targets directories. */
    status_t mount_blocking_(resources_config_t const& config, constraint_t& constraint, nvme_queues_ref_t queues,
                             std::uint16_t targets) noexcept {
        auto [device, make_device_status] = blocking_files_device_t::make(file_buffering_t::unbuffered_k);
        if (make_device_status.failed()) return make_device_status;
        using blocking_disk_t = typename files_disk_t::blocking_disk_t;
        auto [disk, make_disk_status] = blocking_disk_t::make(std::move(device), ring_config_(targets), constraint);
        if (make_disk_status.failed()) return make_disk_status;
        return adopt_<files_disk_t>(files_disk_t {std::move(disk)}, config, constraint, queues, targets);
    }

    /** Mounts io_uring on unbuffered files over @p targets directories. */
    status_t mount_linux_uring_(resources_config_t const& config, constraint_t& constraint, nvme_queues_ref_t queues,
                                std::uint16_t targets) noexcept {
        linux_uring_disk_config_t ring;
        ring.registered_memory_limit = 1;
        ring.media_limit = targets;
        ring.buffering = file_buffering_t::unbuffered_k;
        auto [disk, make_disk_status] = files_disk_t::linux_uring_disk_t::make(ring);
        if (make_disk_status.failed()) return make_disk_status;
        return adopt_<files_disk_t>(files_disk_t {std::move(disk)}, config, constraint, queues, targets);
    }

    /** Mounts Linux AIO on unbuffered files over @p targets directories. */
    status_t mount_linux_aio_(resources_config_t const& config, constraint_t& constraint, nvme_queues_ref_t queues,
                              std::uint16_t targets) noexcept {
        linux_aio_disk_config_t contexts;
        contexts.registered_memory_limit = 1;
        contexts.media_limit = targets;
        contexts.buffering = file_buffering_t::unbuffered_k;
        auto [disk, make_disk_status] = files_disk_t::linux_aio_disk_t::make(contexts);
        if (make_disk_status.failed()) return make_disk_status;
        return adopt_<files_disk_t>(files_disk_t {std::move(disk)}, config, constraint, queues, targets);
    }

    /**
     *  @brief Mounts SPDK on the controller at @p controller, sharing the process's
     *      connection to it.
     *  @note The probe reads files alone and runs first, since the environment it guards calls
     *      @c exit on the process when the machine is not ready for it.
     */
    status_t mount_linux_spdk_(resources_config_t const& config, constraint_t& constraint, nvme_queues_ref_t queues,
                               std::string_view controller) noexcept {
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
            return {status_code_t::unsupported_k, "The controller is not on a user-space driver"};
        case linux_spdk_probe_t::not_found_k: return {status_code_t::not_found_k, "No SPDK controller at that address"};
        }
        linux_spdk_environment_config_t pages;
        pages.memory = spdk_transport_named(address) == linux_spdk_transport_t::fabric_k
                           ? linux_spdk_memory_t::plain_pages_k
                           : linux_spdk_memory_t::hugepages_k;
        auto [environment, environment_status] = environment_t::acquire(pages);
        if (environment_status.failed()) return environment_status;
        linux_spdk_disk_config_t ring;
        ring.registered_memory_limit = 1;
        auto [disk, open_status] = linux_spdk_disk_t::open(std::move(environment), address, ring);
        if (open_status.failed()) return open_status;
        return adopt_<files_disk_t>(files_disk_t {std::move(disk)}, config, constraint, queues, 0);
    }

    /** Emplaces the mount of @p disk, opens the storage on it and records what it got. */
    template <typename disk_type_>
    status_t adopt_(disk_type_&& disk, resources_config_t const& config, constraint_t& constraint,
                    nvme_queues_ref_t queues, std::size_t runs_capacity) noexcept {
        using mount_t = basic_storage_mount<disk_type_, atomic_reference_, waiter_type_>;
        mount_t& opened = probed_disk_.template emplace<mount_t>(std::move(disk));
        if (status_t const started = opened.mount(config, constraint, runs_capacity, queues); started.failed()) {
            probed_disk_.template emplace<std::monostate>();
            return started;
        }
        report_.data_directories = opened.data_directories();
        return {};
    }

    /** The one mount this database opened, or @c std::monostate while it has opened none. */
    std::variant<std::monostate, memory_mount_t, files_mount_t> probed_disk_;

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

    /** The transport and the volumes mounted over the data directories. */
    mounted_storage<atomic_reference_, waiter_type_> storage;

    /** Sets the memory budgets, leaving the services for the caller to attach. */
    explicit shared_resources(std::size_t memory_limit, std::size_t gpu_memory_limit = 0) noexcept
        : memory_constraint(memory_limit), gpu_memory_constraint(gpu_memory_limit) {}
};

/**
 *  @brief Mounts the storage of one database over the manifest and the data directories
 *      @p config names, into @p resources, whose budgets the caller sized from @p config.
 *  @param[in,out] resources The resources the database holds, which outlive every collection
 *      and buffer charged to them and whose destructor tears the mount down in build order.
 *  @param[in] config Budgets, the manifest and the data directories to mount.
 *  @param[in] queues The opening call's queue set, which reads the manifest.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
status_t open_resources(shared_resources<atomic_reference_, waiter_type_>& resources, resources_config_t const& config,
                        nvme_queues_ref_t queues) noexcept {
    if (config.manifest.empty()) return {status_code_t::invalid_argument_k, "A database needs a manifest"};
    if (resources.gpu_devices.resize(config.gpu_devices.size()) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "No room to list the usable GPUs"};
    for (std::size_t index = 0; index != config.gpu_devices.size(); ++index)
        resources.gpu_devices[index] = config.gpu_devices[index];
    return resources.storage.mount(config, resources.memory_constraint, queues);
}

#pragma endregion Shared Resources

#pragma endregion Resources

} // namespace unum::ustore::utilities
