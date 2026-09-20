/**
 *  @file src/nvme/filesystem.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The mount of one database's storage: the transport it probes, the memory it pins, the volumes it opens.
 *
 *  A database owns exactly one of these, and everything it reads or writes hangs off it. The
 *  transport is chosen once, here, by setting a ring up and taking it down again rather than by
 *  reading a version number, because a kernel that has io_uring still refuses it under Docker's
 *  seccomp profile and under @c io_uring_disabled; what the probe settled on is reported back in
 *  @c mount_report_t rather than inferred by a caller.
 *
 *  Teardown is an order, not a destructor: a lease may not be released with work outstanding, a
 *  pinned range may not be unpinned while a request addresses it, and a target may not be
 *  deregistered while one names it. So the lanes drain first, the slab is unpinned next, the targets
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

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`

#include "nvme/blocking_files_disk.hpp" // `blocking_files_disk_t`, `file_buffering_t`
#include "nvme/buffer_pool.hpp"         // `registered_slab_t`, `slab_config_t`
#include "nvme/config.hpp"              // `resources_config_t`, `io_discipline_t`
#include "nvme/files_disk.hpp"          // `any_files_disk_t`
#include "nvme/io_lane.hpp"             // `basic_lane_pool`, `lane_config_t`
#include "nvme/ram_disk.hpp"            // `ram_disk_t`, `ram_device_t`
#include "nvme/ring.hpp"                // `descriptor_t`, `target_request_t`, `ring_concept`
#include "nvme/uring_files_disk.hpp"    // `uring_files_disk_t`, `uring_probe_t`
#include "nvme/volume.hpp"              // `basic_extent_volume`, `volume_format_t`
#include "nvme/volume_router.hpp"       // `basic_volume_router`

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
    return {status_code_t::io_k, "A directory of the database would not be created"};
}

#else

/** Creates @p path alone, treating a directory that is already there as success. */
inline status_t create_directory(char const* path) noexcept {
    if (::mkdir(path, 0755) == 0 || errno == EEXIST) return {};
    return {status_code_t::io_k, "A directory of the database would not be created"};
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

/** Creates every data directory of @p config, which the two file transports open a volume in. */
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
    io_discipline_t discipline = io_discipline_t::blocking_k;

    /** What the kernel answered when the ring was set up and taken down again. */
    uring_probe_t uring = uring_probe_t::refused_k;

    /** Volumes the router adopted, which is one per data directory. */
    std::size_t volumes = 0;

    /** Lanes the pool opened, which is the executor's threads plus the spares. */
    std::size_t lanes = 0;

    /** Bytes the slab pinned, charged to the host budget exactly once. */
    std::uint64_t slab_bytes = 0;
};

/** The transport @p wanted resolves to given @p probe: the ring where the kernel offers it, the floor elsewhere. */
[[nodiscard]] inline io_discipline_t probed_discipline(io_discipline_t wanted, uring_probe_t probe) noexcept {
    if (wanted != io_discipline_t::automatic_k && wanted != io_discipline_t::uring_k) return wanted;
    return probe == uring_probe_t::available_k ? io_discipline_t::uring_k : io_discipline_t::blocking_k;
}

#pragma endregion Probe

#pragma region Mount

/**
 *  @brief One transport, the memory it pinned, the lanes it leases and the volumes it opened over it.
 *
 *  It is deliberately neither copyable nor movable: a lane pool and every volume hold a pointer to
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

    /** Every lease this mount hands out, one per executor thread plus the spares. */
    using lane_pool_t = basic_lane_pool<disk_type_>;

    /** Takes @p disk in, which a mount owns until it is torn down. */
    explicit basic_storage_mount(disk_type_&& disk) noexcept : disk_(std::move(disk)) {}

    basic_storage_mount(basic_storage_mount const&) = delete;
    basic_storage_mount& operator=(basic_storage_mount const&) = delete;
    basic_storage_mount(basic_storage_mount&&) = delete;
    basic_storage_mount& operator=(basic_storage_mount&&) = delete;

    ~basic_storage_mount() noexcept { [[maybe_unused]] status_t const released = unmount(); }

    /**
     *  @brief Pins the slab, leases the lanes and opens one volume per data directory of @p config.
     *  @param[in] config The lane shape, the spare lanes and the data directories, with their capacities.
     *  @param[inout] constraint The host budget the slab is charged to, once, for the life of the mount.
     *  @param[in] threads Executor threads, each of which gets a lane of its own.
     *  @param[in] database The database the volumes belong to, which a mount refuses to open under another.
     *  @return Success, or the first refusal, after which @c unmount gives back whatever was taken.
     */
    status_t mount(resources_config_t const& config, resource_constraint_t& constraint, std::size_t threads,
                   uuid_t const& database) noexcept {
        if (config.data_directories.empty())
            return {status_code_t::invalid_argument_k, "A mount needs a data directory to open a volume in"};
        if (config.spare_lanes == 0)
            return {status_code_t::invalid_argument_k, "A mount keeps a lane for the threads outside its executor"};

        slab_config_t slab;
        slab.bytes = config.lanes.slab_bytes() * (threads + config.spare_lanes);
        expected<registered_slab_t> pinned = registered_slab_t::make(disk_, constraint, slab);
        if (!pinned) return pinned.status();
        slab_ = std::move(*pinned);

        expected<lane_pool_t> leased = lane_pool_t::make(disk_, slab_, threads, config.spare_lanes, config.lanes);
        if (!leased) return leased.status();
        lanes_ = std::move(*leased);

        if (targets_.resize(config.data_directories.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A mount could not track its targets"};
        expected<router_t> built = router_t::make(config.data_directories.size());
        if (!built) return built.status();
        router_ = std::move(*built);

        expected<typename lane_pool_t::guard_t> guard = lanes_.checkout();
        if (!guard) return guard.status();
        for (std::size_t index = 0; index != config.data_directories.size(); ++index)
            if (status_t const opened = open_volume_(config.data_directories[index], index, guard->lane(), database);
                opened.failed())
                return opened;
        return {};
    }

    /** Drains and releases the lanes, unpins the slab, gives the targets back and drops the volumes. */
    status_t unmount() noexcept {
        status_t first;
        if (status_t const drained = lanes_.dismount(); drained.failed() && first.succeeded()) first = drained;
        lanes_ = lane_pool_t {};

        if (slab_.registered())
            if (status_t const unpinned = slab_.dismount(disk_); unpinned.failed() && first.succeeded())
                first = unpinned;
        slab_ = registered_slab_t {};

        for (std::size_t index = 0; index != targets_.size(); ++index) {
            if (!targets_[index].held()) continue;
            if (status_t const detached = disk_.deregister_target(targets_[index]);
                detached.failed() && first.succeeded())
                first = detached;
        }
        targets_.clear();

        router_ = router_t {};
        return first;
    }

    /** The transport underneath, which every read and write of a blob's bytes goes to directly. */
    [[nodiscard]] disk_type_& disk() noexcept { return disk_; }

    /** The transport underneath, for what a caller only asks of it. */
    [[nodiscard]] disk_type_ const& disk() const noexcept { return disk_; }

    /** The volumes of this database and the placement that decides which of them a blob lands on. */
    [[nodiscard]] router_t& router() noexcept { return router_; }

    /** The leases every operation borrows one of, by executor thread index or from the spares. */
    [[nodiscard]] lane_pool_t& lanes() noexcept { return lanes_; }

    /** The one pinned range every I/O buffer of this database is carved out of. */
    [[nodiscard]] registered_slab_t& slab() noexcept { return slab_; }

  private:

    /**
     *  @brief Opens the volume of @p directory as target @p index, and adopts it into the router.
     *  @param[in] directory The path the volume file sits in, and the bytes the volume spans.
     *  @param[in] index The target slot this volume takes, which @c unmount gives back.
     *  @param[inout] lane The lane the head and the catalog slots are read and written through.
     *  @param[in] database The database the volume belongs to, which a mount refuses to open under another.
     */
    status_t open_volume_(data_directory_config_t const& directory, std::size_t index, io_lane_t& lane,
                          uuid_t const& database) noexcept {
        if (directory.path.empty()) return {status_code_t::invalid_argument_k, "A data directory needs a path"};
        if (directory.max_size == 0 || directory.max_size == unlimited_resource_k)
            return {status_code_t::invalid_argument_k, "A data directory names the bytes its volume spans"};

        char path[path_max_bytes_k];
        if (status_t const joined = join_path(directory.path.c_str(), volume_file_name_k, path); joined.failed())
            return joined;

        target_request_t request;
        request.name = path;
        request.length = directory.max_size;
        request.access = target_access_t::create_read_write_k;
        expected<descriptor_t> registered = disk_.register_target(request);
        if (!registered) return registered.status();
        targets_[index] = std::move(*registered);

        volume_format_t format;
        format.database = database;
        format.capacity_bytes = directory.max_size;
        expected<volume_t> opened = volume_t::mount(disk_, lane, targets_[index], format);
        if (!opened) return opened.status();
        return router_.adopt(std::move(*opened));
    }

    /** The transport, owned here and outliving every lane and volume that points at it. */
    disk_type_ disk_;

    /** The one pinned range, charged to the host budget once and carved into blocks by the lanes. */
    registered_slab_t slab_;

    /** Every lease of this mount, opened here and lent one operation at a time. */
    lane_pool_t lanes_;

    /** One registered target per data directory, given back before the volumes are dropped. */
    st::basic_vector<descriptor_t> targets_;

    /** The volumes those targets hold, and the placement that spreads blobs over them. */
    router_t router_;
};

/**
 *  @brief The storage of one database, in whichever transport the probe chose for it.
 *
 *  The probe runs once and the alternative it picked is emplaced in place, so the dispatch a caller
 *  pays is one @c visit per operation rather than one virtual call per request. A mount on files
 *  holds whichever of the two file transports the probe settled on, so a database materializes over
 *  one disk type; SPDK is named by the configuration and refused here until its disk is ported.
 */
class mounted_storage_t {
  public:

    /** The mount a suite and a benchmark ask for, whose volumes never reach a medium. */
    using ram_mount_t = basic_storage_mount<ram_disk_t>;

    /** The mount a database gets, on unbuffered files through the ring or the blocking floor. */
    using files_mount_t = basic_storage_mount<any_files_disk_t>;

    mounted_storage_t() noexcept = default;
    mounted_storage_t(mounted_storage_t const&) = delete;
    mounted_storage_t& operator=(mounted_storage_t const&) = delete;

    /**
     *  @brief Probes the transport @p config asks for and mounts the whole storage stack on it.
     *  @param[in] config The discipline, the lane shape and the data directories, with their capacities.
     *  @param[inout] constraint The host budget the slab is charged to, once, for the life of the mount.
     *  @param[in] threads Executor threads, each of which gets a lane of its own.
     *  @param[in] database The database the volumes belong to, or an invalid identifier to take any.
     */
    status_t mount(resources_config_t const& config, resource_constraint_t& constraint, std::size_t threads,
                   uuid_t const& database) noexcept {
        if (mounted()) return {status_code_t::already_exists_k, "This database has already mounted its storage"};

        report_ = mount_report_t {};
        report_.uring = uring_files_disk_t::probe();
        report_.discipline = probed_discipline(config.io_discipline, report_.uring);

        std::uint16_t const owners = bounded_count_(threads + config.spare_lanes,
                                                    static_cast<std::size_t>(owner_id_t::unowned_k));
        std::uint16_t const targets = bounded_count_(config.data_directories.size(),
                                                     static_cast<std::size_t>(descriptor_id_t::unregistered_k));
        if (owners == 0 || targets == 0)
            return {status_code_t::capacity_exhausted_k, "A ring names fewer leases or targets than that"};

        switch (report_.discipline) {
        case io_discipline_t::ram_k: {
            expected<ram_device_t> device = ram_device_t::make(portable_alignment_k);
            if (!device) return device.status();
            expected<ram_disk_t> disk = ram_disk_t::make(std::move(*device), ring_config_(config, owners, targets));
            if (!disk) return disk.status();
            return adopt_<ram_disk_t>(std::move(*disk), config, constraint, threads, database);
        }
        case io_discipline_t::blocking_k:
            return mount_blocking_(config, constraint, threads, database, owners, targets);
        case io_discipline_t::uring_k: return mount_uring_(config, constraint, threads, database, owners, targets);
        case io_discipline_t::spdk_k:
            return {status_code_t::unsupported_k, "The SPDK transport is not mounted by this build"};
        case io_discipline_t::automatic_k:
            return {status_code_t::internal_k, "The probe left the discipline unresolved"};
        }
        return {status_code_t::invalid_argument_k, "The configuration names no I/O discipline"};
    }

    /** Tears the mounted transport down in order, and leaves the handle naming none. */
    status_t unmount() noexcept {
        if (!mounted()) return {};
        status_t const released = visit([](auto& mount) noexcept { return mount.unmount(); });
        held_.emplace<std::monostate>();
        report_ = mount_report_t {};
        return released;
    }

    /** Whether any transport is open, which every operation needs before it asks for a lane. */
    [[nodiscard]] bool mounted() const noexcept { return !std::holds_alternative<std::monostate>(held_); }

    /** What the mount opened, which a caller reports as the effective configuration. */
    [[nodiscard]] mount_report_t const& report() const noexcept { return report_; }

    /** How the mounted transport addresses its slab, which io_uring settles at each ring's first submission. */
    [[nodiscard]] range_pinning_t pinning() const noexcept {
        files_mount_t const* const files = std::get_if<files_mount_t>(&held_);
        return files ? files->disk().pinning() : range_pinning_t::unpinned_k;
    }

    /** Calls @p visitor with the mount that is open, or reports that none is. */
    template <typename visitor_type_>
    status_t visit(visitor_type_&& visitor) noexcept {
        if (ram_mount_t* const ram = std::get_if<ram_mount_t>(&held_)) return visitor(*ram);
        if (files_mount_t* const files = std::get_if<files_mount_t>(&held_)) return visitor(*files);
        return {status_code_t::uninitialized_k, "This database has mounted no storage"};
    }

  private:

    /** The ring shape every transport is made with, which the lanes and the targets fix. */
    [[nodiscard]] static disk_config_t ring_config_(resources_config_t const& config, std::uint16_t owners,
                                                    std::uint16_t targets) noexcept {
        disk_config_t ring;
        ring.owners_limit = owners;
        ring.depth_limit = config.lanes.depth;
        ring.registrations_limit = 1;
        ring.targets_limit = targets;
        return ring;
    }

    /** Mounts the blocking floor on unbuffered files, with @p owners leases over @p targets directories. */
    status_t mount_blocking_(resources_config_t const& config, resource_constraint_t& constraint, std::size_t threads,
                             uuid_t const& database, std::uint16_t owners, std::uint16_t targets) noexcept {
        if (status_t const made = create_data_directories(config); made.failed()) return made;
        expected<blocking_files_device_t> device = blocking_files_device_t::make(file_buffering_t::unbuffered_k);
        if (!device) return device.status();
        expected<blocking_files_disk_t> disk = blocking_files_disk_t::make(std::move(*device),
                                                                           ring_config_(config, owners, targets));
        if (!disk) return disk.status();
        return adopt_<any_files_disk_t>(any_files_disk_t {std::move(*disk)}, config, constraint, threads, database);
    }

    /** Mounts io_uring on unbuffered files, one ring per lease, with @p owners leases over @p targets directories. */
    status_t mount_uring_(resources_config_t const& config, resource_constraint_t& constraint, std::size_t threads,
                          uuid_t const& database, std::uint16_t owners, std::uint16_t targets) noexcept {
        if (status_t const made = create_data_directories(config); made.failed()) return made;
        uring_disk_config_t ring;
        ring.owners_limit = owners;
        ring.depth_limit = config.lanes.depth;
        ring.registrations_limit = 1;
        ring.targets_limit = targets;
        ring.buffering = file_buffering_t::unbuffered_k;
        expected<uring_files_disk_t> disk = uring_files_disk_t::make(ring);
        if (!disk) return disk.status();
        return adopt_<any_files_disk_t>(any_files_disk_t {std::move(*disk)}, config, constraint, threads, database);
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
        report_.lanes = opened.lanes().size();
        report_.slab_bytes = opened.slab().size();
        return {};
    }

    /** The one mount this database opened, or @c std::monostate while it has opened none. */
    std::variant<std::monostate, ram_mount_t, files_mount_t> held_;

    /** What the probe settled on and what the mount took, reported as the effective configuration. */
    mount_report_t report_;
};

#pragma endregion Mount

} // namespace unum::ustore::nvme
