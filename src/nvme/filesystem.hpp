/**
 *  @file src/nvme/filesystem.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The mount of one database's storage: the transport it probes, the memory it pins, the volumes it opens.
 *
 *  A database owns exactly one of these, and everything it reads or writes hangs off it. The
 *  transport is chosen once, here, by setting a ring and an AIO context up and taking each down
 *  again rather than by reading a version number, because a kernel that has io_uring still refuses
 *  it under Docker's seccomp profile and under @c io_uring_disabled, and keeps AIO in both; what the
 *  probes settled on is reported back in @c mount_report_t rather than inferred by a caller.
 *
 *  Teardown is an order, not a destructor: a queue pair may not be released with work outstanding, a
 *  pinned range may not be unpinned while a request addresses it, and a medium may not be
 *  deregistered while one names it. So the channels drain first, the arena is unpinned next, the targets
 *  go back after that, and the volumes are dropped last.
 */

#pragma once

#include <cerrno>  // `errno`, `EEXIST`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::strlen`

#if defined(_WIN32)
#include <windows.h> // `CreateDirectoryA`
#else
#include <sys/stat.h> // `mkdir`
#endif

#include <span>    // `std::span`
#include <utility> // `std::move`
#include <variant> // `std::variant`, `std::get_if`, `std::monostate`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/status.hpp" // `status_t`, `status_code_t`, `expected`

#include "nvme/blocking_files_disk.hpp"    // `blocking_files_disk_t`, `file_buffering_t`
#include "nvme/files_disk.hpp"             // `any_files_disk_t`
#include "nvme/linux_aio_files_disk.hpp"   // `linux_aio_files_disk_t`, `linux_aio_probe_t`
#include "nvme/linux_uring_files_disk.hpp" // `linux_uring_files_disk_t`, `linux_uring_probe_t`
#include "nvme/memory_disk.hpp"            // `memory_disk_t`, `memory_device_t`

#include "nvme/buffer_pool.hpp"   // `registered_memory_arena_t`, `memory_arena_config_t`
#include "nvme/config.hpp"        // `resources_config_t`, `io_transport_t`
#include "nvme/io_channel.hpp"    // `basic_io_channel_pool`, `io_channel_config_t`
#include "nvme/ring.hpp"          // `registered_medium_t`, `medium_options_t`, `ring_concept`
#include "nvme/volume.hpp"        // `basic_extent_volume`, `volume_format_t`
#include "nvme/volume_router.hpp" // `basic_volume_router`

namespace unum::ustore::nvme {

#pragma region Paths

/** Bytes a path of this engine may span, which bounds the one buffer a mount joins paths in. */
inline constexpr std::size_t path_max_bytes_k = 4096;

/** The one file a data directory keeps its whole volume in. */
inline constexpr char volume_file_name_k[] = "volume.ustore";

#if defined(_WIN32)

/** Creates @p path alone, treating a directory that is already there as success. */
inline status_t create_directory(char const* path) noexcept {
    if (CreateDirectoryA(path, nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS) return {};
    return {status_code_t::input_output_k, "A directory of the database would not be created"};
}

#else

/** Creates @p path alone, treating a directory that is already there as success. */
inline status_t create_directory(char const* path) noexcept {
    if (::mkdir(path, 0755) == 0 || errno == EEXIST) return {};
    return {status_code_t::input_output_k, "A directory of the database would not be created"};
}

#endif

/**
 *  @brief Creates @p path and every parent it needs, which a mount does before it opens a volume there.
 *
 *  @note Only the last component has to be created; an intermediate that refuses, such as a Windows
 *      drive letter, is passed over rather than reported.
 */
inline status_t create_directories(char const* path) noexcept {
    char components[path_max_bytes_k];
    std::size_t const length = std::strlen(path);
    if (length == 0 || length >= sizeof(components))
        return {status_code_t::invalid_argument_k, "A directory path is one byte to the platform's limit"};
    std::memcpy(components, path, length + 1);

    for (std::size_t index = 1; index != length; ++index) {
        if (components[index] != '/' && components[index] != '\\') continue;
        components[index] = '\0';
        [[maybe_unused]] status_t const parent = create_directory(components);
        components[index] = path[index];
    }
    return create_directory(components);
}

/** Writes @c <directory>/<name> into @p into, or refuses a path the buffer will not hold. */
inline status_t join_path(char const* directory, char const* name, std::span<char> into) noexcept {
    std::size_t const directory_bytes = std::strlen(directory);
    std::size_t const name_bytes = std::strlen(name);
    if (directory_bytes + name_bytes + 2 > into.size())
        return {status_code_t::invalid_argument_k, "A path of the database outgrew the platform's limit"};

    std::memcpy(into.data(), directory, directory_bytes);
    std::size_t written = directory_bytes;
    if (written != 0 && into[written - 1] != '/' && into[written - 1] != '\\') into[written++] = '/';
    std::memcpy(into.data() + written, name, name_bytes + 1);
    return {};
}

/** Creates every data directory of @p config, which every file transport opens a volume in. */
inline status_t create_data_directories(resources_config_t const& config) noexcept {
    for (std::size_t index = 0; index != config.data_directories.size(); ++index) {
        data_directory_config_t const& directory = config.data_directories[index];
        if (directory.path.empty()) return {status_code_t::invalid_argument_k, "A data directory needs a path"};
        if (status_t const made = create_directories(directory.path.c_str()); made.failed()) return made;
    }
    return {};
}

#pragma endregion Paths

#pragma region Probe

/** What a mount opened, which is what the effective configuration of a database reports. */
struct mount_report_t {

    /** The transport the mount actually opened, which is never @c automatic_k. */
    io_transport_t transport = io_transport_t::blocking_k;

    /** What the kernel answered when the ring was set up and taken down again. */
    linux_uring_probe_t linux_uring = linux_uring_probe_t::refused_k;

    /** What the kernel answered when an AIO context was set up and taken down again. */
    linux_aio_probe_t linux_aio = linux_aio_probe_t::refused_k;

    /** Volumes the router adopted, which is one per data directory. */
    std::size_t volumes = 0;

    /** Channels the pool opened, which is the executor's threads plus the shared channels. */
    std::size_t channels = 0;

    /** Bytes the arena pinned, charged to the host budget exactly once. */
    std::uint64_t arena_bytes = 0;
};

/**
 *  @brief The transport @p wanted resolves to, given what the two probes answered.
 *
 *  The order is io_uring, then Linux AIO, then the blocking floor. A transport named outright starts
 *  at its own place in that order and only falls further down it, so asking for AIO never mounts a ring.
 */
[[nodiscard]] inline io_transport_t probed_transport(io_transport_t wanted, linux_uring_probe_t linux_uring,
                                                     linux_aio_probe_t linux_aio) noexcept {
    bool const takes_linux_uring = wanted == io_transport_t::automatic_k || wanted == io_transport_t::linux_uring_k;
    if (takes_linux_uring && linux_uring == linux_uring_probe_t::available_k) return io_transport_t::linux_uring_k;
    if (!takes_linux_uring && wanted != io_transport_t::linux_aio_k) return wanted;
    return linux_aio == linux_aio_probe_t::available_k ? io_transport_t::linux_aio_k : io_transport_t::blocking_k;
}

#pragma endregion Probe

#pragma region Mount

/**
 *  @brief One transport, the memory it pinned, the channels it queue pairs and the volumes it opened over it.
 *
 *  It is deliberately neither copyable nor movable: a channel pool and every volume hold a pointer to
 *  the disk beside them, so a mount that moved would leave both addressing a transport that is no
 *  longer there. The variant above it emplaces one rather than assigning one for that reason.
 */
template <typename disk_type_>
    requires ring_concept<disk_type_>
class basic_storage_mount {
  public:

    /** The volume every data directory of this mount holds, which is one file of the directory. */
    using volume_t = basic_extent_volume<disk_type_>;

    /** The volumes of this mount and the free-space placement that spreads blobs over them. */
    using router_t = basic_volume_router<volume_t>;

    /** Every queue pair this mount hands out, one per executor thread plus the shared channels. */
    using channel_pool_t = basic_io_channel_pool<disk_type_>;

    /** Takes @p disk in, which a mount owns until it is torn down. */
    explicit basic_storage_mount(disk_type_&& disk) noexcept : disk_(std::move(disk)) {}

    basic_storage_mount(basic_storage_mount const&) = delete;
    basic_storage_mount& operator=(basic_storage_mount const&) = delete;
    basic_storage_mount(basic_storage_mount&&) = delete;
    basic_storage_mount& operator=(basic_storage_mount&&) = delete;

    ~basic_storage_mount() noexcept { [[maybe_unused]] status_t const released = unmount(); }

    /**
     *  @brief Pins the arena, queue pairs the channels and opens one volume per data directory of @p config.
     *  @param[in] config The channel shape, the shared channels and the data directories, with their capacities.
     *  @param[inout] constraint The host budget the arena is charged to, once, for the life of the mount.
     *  @param[in] threads Executor threads, each of which gets a channel of its own.
     *  @param[in] database The database the volumes belong to, which a mount refuses to open under another.
     *  @return Success, or the first refusal, after which @c unmount gives back whatever was taken.
     */
    status_t mount(resources_config_t const& config, resource_constraint_t& constraint, std::size_t threads,
                   uuid_t const& database) noexcept {
        if (config.data_directories.empty())
            return {status_code_t::invalid_argument_k, "A mount needs a data directory to open a volume in"};
        if (config.shared_io_channels == 0)
            return {status_code_t::invalid_argument_k, "A mount keeps a channel for the threads outside its executor"};

        memory_arena_config_t arena;
        arena.bytes = config.channels.arena_bytes() * (threads + config.shared_io_channels);
        auto [pinned, make_arena_status] = registered_memory_arena_t::make(disk_, constraint, arena);
        if (make_arena_status.failed()) return make_arena_status;
        arena_ = std::move(pinned);

        auto [leased, make_channel_pool_status] = channel_pool_t::make(disk_, arena_, threads,
                                                                       config.shared_io_channels, config.channels);
        if (make_channel_pool_status.failed()) return make_channel_pool_status;
        channels_ = std::move(leased);

        if (media_.resize(config.data_directories.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A mount could not track its targets"};
        auto [built, make_router_status] = router_t::make(config.data_directories.size());
        if (make_router_status.failed()) return make_router_status;
        router_ = std::move(built);

        auto [guard, acquire_status] = channels_.acquire();
        if (acquire_status.failed()) return acquire_status;
        for (std::size_t index = 0; index != config.data_directories.size(); ++index)
            if (status_t const opened = open_volume_(config.data_directories[index], index, guard.channel(), database);
                opened.failed())
                return opened;
        return {};
    }

    /** Drains and releases the channels, unpins the arena, gives the targets back and drops the volumes. */
    status_t unmount() noexcept {
        status_t first;
        if (status_t const drained = channels_.release_all(); drained.failed() && first.succeeded()) first = drained;
        channels_ = channel_pool_t {};

        if (arena_.registered())
            if (status_t const unpinned = arena_.unregister(disk_); unpinned.failed() && first.succeeded())
                first = unpinned;
        arena_ = registered_memory_arena_t {};

        for (std::size_t index = 0; index != media_.size(); ++index) {
            if (!media_[index].held()) continue;
            if (status_t const detached = disk_.unregister_medium(media_[index]);
                detached.failed() && first.succeeded())
                first = detached;
        }
        media_.clear();

        router_ = router_t {};
        return first;
    }

    /** The transport underneath, which every read and write of a blob's bytes goes to directly. */
    [[nodiscard]] disk_type_& disk() noexcept { return disk_; }

    /** The transport underneath, for what a caller only asks of it. */
    [[nodiscard]] disk_type_ const& disk() const noexcept { return disk_; }

    /** The volumes of this database and the placement that decides which of them a blob lands on. */
    [[nodiscard]] router_t& router() noexcept { return router_; }

    /** The queue pairs every operation borrows one of, by executor thread index or from the shared channels. */
    [[nodiscard]] channel_pool_t& channels() noexcept { return channels_; }

    /** The one pinned range every I/O buffer of this database is carved out of. */
    [[nodiscard]] registered_memory_arena_t& arena() noexcept { return arena_; }

  private:

    /**
     *  @brief Opens the volume of @p directory as medium @p index, and adopts it into the router.
     *  @param[in] directory The path the volume file sits in, and the bytes the volume spans.
     *  @param[in] index The medium slot this volume takes, which @c unmount gives back.
     *  @param[inout] channel The channel the head and the catalog slots are read and written through.
     *  @param[in] database The database the volume belongs to, which a mount refuses to open under another.
     */
    status_t open_volume_(data_directory_config_t const& directory, std::size_t index, io_channel_t& channel,
                          uuid_t const& database) noexcept {
        if (directory.path.empty()) return {status_code_t::invalid_argument_k, "A data directory needs a path"};
        if (directory.max_size == 0 || directory.max_size == unlimited_resource_k)
            return {status_code_t::invalid_argument_k, "A data directory names the bytes its volume spans"};

        char path[path_max_bytes_k];
        if (status_t const joined = join_path(directory.path.c_str(), volume_file_name_k, path); joined.failed())
            return joined;

        medium_options_t request;
        request.name = path;
        request.length = directory.max_size;
        request.access = medium_access_t::create_read_write_k;
        auto [registered, register_medium_status] = disk_.register_medium(request);
        if (register_medium_status.failed()) return register_medium_status;
        media_[index] = std::move(registered);

        volume_format_t format;
        format.database = database;
        format.capacity_bytes = directory.max_size;
        expected<volume_t> opened = volume_t::mount(disk_, channel, media_[index], format);
        if (!opened) return opened.status();
        return router_.bind_queue_pair(std::move(*opened));
    }

    /** The transport, owned here and outliving every channel and volume that points at it. */
    disk_type_ disk_;

    /** The one pinned range, charged to the host budget once and carved into blocks by the channels. */
    registered_memory_arena_t arena_;

    /** Every queue pair of this mount, opened here and lent one operation at a time. */
    channel_pool_t channels_;

    /** One registered medium per data directory, given back before the volumes are dropped. */
    st::basic_vector<registered_medium_t> media_;

    /** The volumes those targets hold, and the placement that spreads blobs over them. */
    router_t router_;
};

/**
 *  @brief The storage of one database, in whichever transport the probe chose for it.
 *
 *  The probe runs once and the alternative it picked is emplaced in place, so the dispatch a caller
 *  pays is one @c visit per operation rather than one virtual call per request. A mount on files
 *  holds whichever of the three file transports the probe settled on, so a database materializes over
 *  one disk type; SPDK is named by the configuration and refused here until its disk is ported.
 */
class mounted_storage_t {
  public:

    /** The mount a suite and a benchmark ask for, whose volumes never reach a medium. */
    using memory_mount_t = basic_storage_mount<memory_disk_t>;

    /** The mount a database gets, on unbuffered files through io_uring, Linux AIO or the blocking floor. */
    using files_mount_t = basic_storage_mount<any_files_disk_t>;

    mounted_storage_t() noexcept = default;
    mounted_storage_t(mounted_storage_t const&) = delete;
    mounted_storage_t& operator=(mounted_storage_t const&) = delete;

    /**
     *  @brief Probes the transport @p config asks for and mounts the whole storage stack on it.
     *  @param[in] config The transport, the channel shape and the data directories, with their capacities.
     *  @param[inout] constraint The host budget the arena is charged to, once, for the life of the mount.
     *  @param[in] threads Executor threads, each of which gets a channel of its own.
     *  @param[in] database The database the volumes belong to, or an invalid identifier to take any.
     */
    status_t mount(resources_config_t const& config, resource_constraint_t& constraint, std::size_t threads,
                   uuid_t const& database) noexcept {
        if (mounted()) return {status_code_t::already_exists_k, "This database has already mounted its storage"};

        report_ = mount_report_t {};
        report_.linux_uring = linux_uring_files_disk_t::probe();
        report_.linux_aio = linux_aio_files_disk_t::probe();
        report_.transport = probed_transport(config.io_transport, report_.linux_uring, report_.linux_aio);

        std::uint16_t const owners = bounded_count_(threads + config.shared_io_channels,
                                                    static_cast<std::size_t>(queue_pair_id_t::unallocated_k));
        std::uint16_t const targets = bounded_count_(config.data_directories.size(),
                                                     static_cast<std::size_t>(registered_medium_id_t::unregistered_k));
        if (owners == 0 || targets == 0)
            return {status_code_t::capacity_exhausted_k, "A ring names fewer queue pairs or targets than that"};

        switch (report_.transport) {
        case io_transport_t::memory_k: {
            auto [device, make_memory_device_status] = memory_device_t::make(portable_alignment_k);
            if (make_memory_device_status.failed()) return make_memory_device_status;
            expected<memory_disk_t> disk = memory_disk_t::make(std::move(device),
                                                               ring_config_(config, owners, targets));
            if (!disk) return disk.status();
            return adopt_<memory_disk_t>(std::move(*disk), config, constraint, threads, database);
        }
        case io_transport_t::blocking_k: return mount_blocking_(config, constraint, threads, database, owners, targets);
        case io_transport_t::linux_uring_k:
            return mount_linux_uring_(config, constraint, threads, database, owners, targets);
        case io_transport_t::linux_aio_k:
            return mount_linux_aio_(config, constraint, threads, database, owners, targets);
        case io_transport_t::linux_spdk_k:
            return {status_code_t::unsupported_k, "The SPDK transport is not mounted by this build"};
        case io_transport_t::automatic_k: return {status_code_t::internal_k, "The probe left the transport unresolved"};
        }
        return {status_code_t::invalid_argument_k, "The configuration names no I/O transport"};
    }

    /** Tears the mounted transport down in order, and leaves the handle naming none. */
    status_t unmount() noexcept {
        if (!mounted()) return {};
        status_t const released = visit([](auto& mount) noexcept { return mount.unmount(); });
        held_.emplace<std::monostate>();
        report_ = mount_report_t {};
        return released;
    }

    /** Whether any transport is open, which every operation needs before it asks for a channel. */
    [[nodiscard]] bool mounted() const noexcept { return !std::holds_alternative<std::monostate>(held_); }

    /** What the mount opened, which a caller reports as the effective configuration. */
    [[nodiscard]] mount_report_t const& report() const noexcept { return report_; }

    /** How the mounted transport addresses its arena, which io_uring settles at each ring's first submission. */
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

    /** Mounts the blocking floor on unbuffered files, with @p owners queue pairs over @p targets directories. */
    status_t mount_blocking_(resources_config_t const& config, resource_constraint_t& constraint, std::size_t threads,
                             uuid_t const& database, std::uint16_t owners, std::uint16_t targets) noexcept {
        if (status_t const made = create_data_directories(config); made.failed()) return made;
        auto [device, make_device_status] = blocking_files_device_t::make(file_buffering_t::unbuffered_k);
        if (make_device_status.failed()) return make_device_status;
        expected<blocking_files_disk_t> disk = blocking_files_disk_t::make(std::move(device),
                                                                           ring_config_(config, owners, targets));
        if (!disk) return disk.status();
        return adopt_<any_files_disk_t>(any_files_disk_t {std::move(*disk)}, config, constraint, threads, database);
    }

    /** Mounts io_uring on unbuffered files, one ring per queue pair, with @p owners queue pairs over @p targets
     *  directories. */
    status_t mount_linux_uring_(resources_config_t const& config, resource_constraint_t& constraint,
                                std::size_t threads, uuid_t const& database, std::uint16_t owners,
                                std::uint16_t targets) noexcept {
        if (status_t const made = create_data_directories(config); made.failed()) return made;
        linux_uring_disk_config_t ring;
        ring.queue_pairs_limit = owners;
        ring.depth_limit = config.channels.depth;
        ring.registered_memory_limit = 1;
        ring.media_limit = targets;
        ring.buffering = file_buffering_t::unbuffered_k;
        auto [disk, make_disk_status] = linux_uring_files_disk_t::make(ring);
        if (make_disk_status.failed()) return make_disk_status;
        return adopt_<any_files_disk_t>(any_files_disk_t {std::move(disk)}, config, constraint, threads, database);
    }

    /** Mounts Linux AIO on unbuffered files, one context per queue pair, with @p owners queue pairs over @p targets
     *  directories. */
    status_t mount_linux_aio_(resources_config_t const& config, resource_constraint_t& constraint, std::size_t threads,
                              uuid_t const& database, std::uint16_t owners, std::uint16_t targets) noexcept {
        if (status_t const made = create_data_directories(config); made.failed()) return made;
        linux_aio_disk_config_t contexts;
        contexts.queue_pairs_limit = owners;
        contexts.depth_limit = config.channels.depth;
        contexts.registered_memory_limit = 1;
        contexts.media_limit = targets;
        contexts.buffering = file_buffering_t::unbuffered_k;
        auto [disk, make_disk_status] = linux_aio_files_disk_t::make(contexts);
        if (make_disk_status.failed()) return make_disk_status;
        return adopt_<any_files_disk_t>(any_files_disk_t {std::move(disk)}, config, constraint, threads, database);
    }

    /** @p wanted as the 16-bit count a ring names things by, or zero when @p reserved leaves no room. */
    [[nodiscard]] static std::uint16_t bounded_count_(std::size_t wanted, std::size_t reserved) noexcept {
        return wanted != 0 && wanted < reserved ? static_cast<std::uint16_t>(wanted) : std::uint16_t {0};
    }

    /** Emplaces the mount of @p disk, opens the storage on it and records what it got. */
    template <typename disk_type_>
    status_t adopt_(disk_type_&& disk, resources_config_t const& config, resource_constraint_t& constraint,
                    std::size_t threads, uuid_t const& database) noexcept {
        basic_storage_mount<disk_type_>& opened = held_.template emplace<basic_storage_mount<disk_type_>>(
            std::move(disk));
        if (status_t const started = opened.mount(config, constraint, threads, database); started.failed()) {
            held_.emplace<std::monostate>();
            return started;
        }
        report_.volumes = opened.router().size();
        report_.channels = opened.channels().size();
        report_.arena_bytes = opened.arena().size();
        return {};
    }

    /** The one mount this database opened, or @c std::monostate while it has opened none. */
    std::variant<std::monostate, memory_mount_t, files_mount_t> held_;

    /** What the probe settled on and what the mount took, reported as the effective configuration. */
    mount_report_t report_;
};

#pragma endregion Mount

} // namespace unum::ustore::nvme
