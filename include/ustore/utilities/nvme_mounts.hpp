/**
 *  @file include/ustore/utilities/nvme_mounts.hpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief The mount of one database's storage: the transport it probes, the disk erased over
 *      the file transports, the memory it pins, the volumes it opens, and the shared resources
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
 *  Teardown is an order, not a destructor: a queue pair may not be released with work
 *  outstanding, a pinned range may not be unpinned while a request addresses it, and a medium may
 *  not be deregistered while one names it. So the channels drain first, the arena is unpinned
 *  next, the targets go back after that, and the volumes are dropped last. The resources over a
 *  mount add the budgets.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint16_t`, `std::uint64_t`

#include <memory>  // `std::shared_ptr`, `std::make_shared`
#include <new>     // placement `new`
#include <span>    // `std::span`
#include <utility> // `std::forward`, `std::move`
#include <variant> // `std::variant`, `std::visit`, `std::get_if`, `std::monostate`, `std::in_place_type`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/blocking_files.hpp" // `create_directories`, `join_path`, `path_max_bytes_k`
#include "ustore/utilities/nvme_channels.hpp" // `basic_io_channel_pool`, `io_channel_config_t`, `registered_memory_arena`
#include "ustore/utilities/nvme_disks.hpp"    // `blocking_files_disk`, `memory_disk`
#include "ustore/utilities/nvme_linux_aio_disk.hpp"   // `linux_aio_files_disk`, `linux_aio_probe_t`
#include "ustore/utilities/nvme_linux_uring_disk.hpp" // `linux_uring_files_disk`, `linux_uring_probe_t`
#include "ustore/utilities/nvme_ring.hpp"             // `queue_pair_t`, `request_t`, `completion_t`, `ring_concept`
#include "ustore/utilities/nvme_volume.hpp"           // `basic_extent_volume`, `basic_volume_router`, `volume_format_t`
#include "ustore/utilities/ram_buffers.hpp" // `resource_constraint`, `constrained_allocator`, `unlimited_resource_k`
#include "ustore/utilities/types.hpp"       // `status_t`, `status_code_t`, `expected`, `byte_t`, `string_t`, `uuid_t`

namespace unum::ustore::utilities {

#pragma region Mount Configuration

/** A directory data files are placed in, with the bytes they may take there. */
struct data_directory_config_t {
    string_t path;
    std::size_t max_size = unlimited_resource_k;
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

    /** SPDK's user-space driver, which no mount opens yet. */
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
    string_t directory;
    io_transport_t io_transport = io_transport_t::automatic_k;
    io_channel_config_t channels;
    std::size_t shared_io_channels = 2;
    st::vector<data_directory_config_t> data_directories;
};

/** The one file a data directory keeps its whole volume in. */
inline constexpr char volume_file_name_k[] = "volume.ustore";

/** Creates every data directory of @p config, which every file transport opens a volume in. */
inline status_t create_data_directories(resources_config_t const& config) noexcept {
    for (std::size_t index = 0; index != config.data_directories.size(); ++index) {
        data_directory_config_t const& directory = config.data_directories[index];
        if (directory.path.empty()) return {status_code_t::invalid_argument_k, "A data directory needs a path"};
        if (status_t const made = create_directories(directory.path.c_str()); made.failed()) return made;
    }
    return {};
}

#pragma endregion Mount Configuration

#pragma region Files Disk

#pragma region Disk

/**
 *  @brief The ring of a database on files: the blocking floor, AIO or io_uring, as
 *      the mount probed.
 *
 *  The io_uring and AIO alternatives exist on every platform, as stubs that refuse off Linux, so
 *  the type is portable and only the mount's probe decides which alternative is built.
 *
 *  @tparam atomic_reference_ The reference every alternative owns its shared words through.
 *  @tparam waiter_type_ What a draining core does after a poll that moved nothing.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class any_files_disk {
  public:

    /** The positioned reads and writes on the polling thread. */
    using blocking_disk_t = blocking_files_disk<atomic_reference_>;

    /** The io_uring transport. */
    using linux_uring_disk_t = linux_uring_files_disk<atomic_reference_, waiter_type_>;

    /** The Linux AIO transport. */
    using linux_aio_disk_t = linux_aio_files_disk<atomic_reference_, waiter_type_>;

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

    any_files_disk(any_files_disk const&) = delete;
    any_files_disk& operator=(any_files_disk const&) = delete;
    any_files_disk(any_files_disk&&) noexcept = default;
    any_files_disk& operator=(any_files_disk&&) noexcept = default;

    /** The transport held, which is never @c automatic_k. */
    [[nodiscard]] io_transport_t transport() const noexcept {
        if (std::holds_alternative<linux_uring_disk_t>(held_)) return io_transport_t::linux_uring_k;
        if (std::holds_alternative<linux_aio_disk_t>(held_)) return io_transport_t::linux_aio_k;
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

    /** Stops @p queue pair's requests that have not started. */
    void cancel(queue_pair_t const& queue_pair) noexcept {
        std::visit([&](auto& disk) noexcept { disk.cancel(queue_pair); }, held_);
    }

    /** Reaps and discards everything @p queue pair has in flight, so the queue pair can be released
     *  or handed over. */
    status_t drain(queue_pair_t const& queue_pair) noexcept {
        return std::visit([&](auto& disk) noexcept { return disk.drain(queue_pair); }, held_);
    }

#pragma endregion Ring

  private:

    /** The transport the probe chose, fixed for the life of the mount. */
    std::variant<blocking_disk_t, linux_uring_disk_t, linux_aio_disk_t> held_;
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

    /** Volumes the router adopted, which is one per data directory. */
    std::size_t volumes = 0;

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
        return status_t {status_code_t::unsupported_k, "The kernel creates no io_uring ring"};
    if (!takes_linux_uring && wanted != io_transport_t::linux_aio_k) return wanted;
    return linux_aio == linux_aio_probe_t::available_k ? io_transport_t::linux_aio_k : io_transport_t::blocking_k;
}

#pragma endregion Probe

#pragma region Mount

/**
 *  @brief One transport, the memory it pinned, the channels it queue pairs and the volumes it
 *      opened over it.
 *
 *  It is deliberately neither copyable nor movable: a channel pool and every volume hold a pointer
 *  to the disk beside them, so a mount that moved would leave both addressing a transport that is
 *  gone. The variant above it emplaces one rather than assigning one for that reason.
 *
 *  @tparam atomic_reference_ The reference the budget and the channels' claims are owned through.
 *  @tparam waiter_type_ What a core waiting on the volumes' reads and barriers does.
 */
template <typename disk_type_, template <typename> typename atomic_reference_, typename waiter_type_>
    requires ring_concept<disk_type_>
class basic_storage_mount {
  public:

    /** The volume every data directory of this mount holds, which is one file of the directory. */
    using volume_t = basic_extent_volume<disk_type_, waiter_type_>;

    /** The volumes of this mount and the free-space placement that spreads blobs over them. */
    using router_t = basic_volume_router<volume_t>;

    /** Every queue pair this mount hands out, one per executor thread plus the shared channels. */
    using channel_pool_t = basic_io_channel_pool<disk_type_, atomic_reference_>;

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
     *  @brief Pins the arena, queue pairs the channels and opens one volume per data directory
     *      of @p config.
     *  @param[in] config The channel shape, the shared channels and the data directories,
     *      with their capacities.
     *  @param[in,out] constraint The host budget the arena is charged to, once, for the life
     *      of the mount.
     *  @param[in] threads Executor threads, each of which gets a channel of its own.
     *  @param[in] database The database the volumes belong to, which a mount refuses to
     *      open under another.
     *  @return Success, or the first refusal, after which @c unmount gives back whatever was taken.
     */
    status_t mount(resources_config_t const& config, constraint_t& constraint, std::size_t threads,
                   uuid_t const& database) noexcept {
        if (config.data_directories.empty())
            return {status_code_t::invalid_argument_k, "A mount needs a data directory to open a volume in"};
        if (config.shared_io_channels == 0)
            return {status_code_t::invalid_argument_k, "A mount keeps a channel for the threads outside its executor"};

        memory_arena_config_t arena;
        arena.bytes = config.channels.arena_bytes() * (threads + config.shared_io_channels);
        auto [pinned, make_arena_status] = arena_t::make(disk_, constraint, arena);
        if (make_arena_status.failed()) return make_arena_status;
        arena_ = std::move(pinned);

        auto [leased, make_channel_pool_status] = channel_pool_t::make(disk_, arena_.registration(), threads,
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

    /** Drains and releases the channels, unpins the arena, gives the targets back and
     *  drops the volumes. */
    status_t unmount() noexcept {
        status_t first;
        if (status_t const drained = channels_.release_all(); drained.failed() && first.succeeded()) first = drained;
        channels_ = channel_pool_t {};

        if (arena_.registered())
            if (status_t const unpinned = arena_.unregister(disk_); unpinned.failed() && first.succeeded())
                first = unpinned;
        arena_ = arena_t {};

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

    /** The volumes of this database and the placement that decides which of them a
     *  blob lands on. */
    [[nodiscard]] router_t& router() noexcept { return router_; }

    /** The queue pairs every operation borrows one of, by executor thread index or from
     *  the shared channels. */
    [[nodiscard]] channel_pool_t& channels() noexcept { return channels_; }

    /** The one pinned range every I/O buffer of this database is carved out of. */
    [[nodiscard]] arena_t& arena() noexcept { return arena_; }

  private:

    /**
     *  @brief Opens the volume of @p directory as medium @p index, and adopts it into the router.
     *  @param[in] directory The path the volume file sits in, and the bytes the volume spans.
     *  @param[in] index The medium slot this volume takes, which @c unmount gives back.
     *  @param[in,out] channel The channel the head and the catalog slots are read
     *      and written through.
     *  @param[in] database The database the volume belongs to, which a mount refuses to
     *      open under another.
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

    /** The one pinned range, charged to the host budget once and carved into blocks
     *  by the channels. */
    arena_t arena_;

    /** Every queue pair of this mount, opened here and lent one operation at a time. */
    channel_pool_t channels_;

    /** One registered medium per data directory, given back before the volumes are dropped. */
    st::vector<registered_medium_t> media_;

    /** The volumes those targets hold, and the placement that spreads blobs over them. */
    router_t router_;
};

/**
 *  @brief The storage of one database, in whichever transport the probe chose for it.
 *
 *  The probe runs once and the alternative it picked is emplaced in place, so the dispatch a
 *  caller pays is one @c visit per operation rather than one virtual call per request. A mount on
 *  files holds whichever of the three file transports the probe settled on, so a database
 *  materializes over one disk type; SPDK is named by the configuration and refused here until its
 *  disk is ported.
 *
 *  @tparam atomic_reference_ The reference every transport, budget and channel owns
 *      its words through.
 *  @tparam waiter_type_ What a core waiting on a transport does between polls.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class mounted_storage {
  public:

    /** The ring a suite and a benchmark ask for, whose volumes never reach a medium. */
    using memory_disk_t = memory_disk<atomic_reference_>;

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
     *  @param[in] database The database the volumes belong to, or an invalid identifier
     *      to take any.
     */
    status_t mount(resources_config_t const& config, constraint_t& constraint, std::size_t threads,
                   uuid_t const& database) noexcept {
        if (mounted()) return {status_code_t::already_exists_k, "This database has already mounted its storage"};

        report_ = mount_report_t {};
        report_.linux_uring = files_disk_t::linux_uring_disk_t::probe();
        report_.linux_aio = files_disk_t::linux_aio_disk_t::probe();
        auto [transport, transport_status] = probed_transport(config.io_transport, report_.linux_uring,
                                                              report_.linux_aio);
        if (transport_status.failed()) return transport_status;
        report_.transport = transport;

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
                             uuid_t const& database, std::uint16_t owners, std::uint16_t targets) noexcept {
        if (status_t const made = create_data_directories(config); made.failed()) return made;
        auto [device, make_device_status] = blocking_files_device_t::make(file_buffering_t::unbuffered_k);
        if (make_device_status.failed()) return make_device_status;
        using blocking_disk_t = typename files_disk_t::blocking_disk_t;
        expected<blocking_disk_t> disk = blocking_disk_t::make(std::move(device),
                                                               ring_config_(config, owners, targets));
        if (!disk) return disk.status();
        return adopt_<files_disk_t>(files_disk_t {std::move(*disk)}, config, constraint, threads, database);
    }

    /** Mounts io_uring on unbuffered files, one ring per queue pair, with @p owners queue pairs
     *  over @p targets directories. */
    status_t mount_linux_uring_(resources_config_t const& config, constraint_t& constraint, std::size_t threads,
                                uuid_t const& database, std::uint16_t owners, std::uint16_t targets) noexcept {
        if (status_t const made = create_data_directories(config); made.failed()) return made;
        linux_uring_disk_config_t ring;
        ring.queue_pairs_limit = owners;
        ring.depth_limit = config.channels.depth;
        ring.registered_memory_limit = 1;
        ring.media_limit = targets;
        ring.buffering = file_buffering_t::unbuffered_k;
        auto [disk, make_disk_status] = files_disk_t::linux_uring_disk_t::make(ring);
        if (make_disk_status.failed()) return make_disk_status;
        return adopt_<files_disk_t>(files_disk_t {std::move(disk)}, config, constraint, threads, database);
    }

    /** Mounts Linux AIO on unbuffered files, one context per queue pair, with @p owners queue pairs
     *  over @p targets directories. */
    status_t mount_linux_aio_(resources_config_t const& config, constraint_t& constraint, std::size_t threads,
                              uuid_t const& database, std::uint16_t owners, std::uint16_t targets) noexcept {
        if (status_t const made = create_data_directories(config); made.failed()) return made;
        linux_aio_disk_config_t contexts;
        contexts.queue_pairs_limit = owners;
        contexts.depth_limit = config.channels.depth;
        contexts.registered_memory_limit = 1;
        contexts.media_limit = targets;
        contexts.buffering = file_buffering_t::unbuffered_k;
        auto [disk, make_disk_status] = files_disk_t::linux_aio_disk_t::make(contexts);
        if (make_disk_status.failed()) return make_disk_status;
        return adopt_<files_disk_t>(files_disk_t {std::move(disk)}, config, constraint, threads, database);
    }

    /** @p wanted as the 16-bit count a ring names things by, or zero when @p reserved
     *  leaves no room. */
    [[nodiscard]] static std::uint16_t bounded_count_(std::size_t wanted, std::size_t reserved) noexcept {
        return wanted != 0 && wanted < reserved ? static_cast<std::uint16_t>(wanted) : std::uint16_t {0};
    }

    /** Emplaces the mount of @p disk, opens the storage on it and records what it got. */
    template <typename disk_type_>
    status_t adopt_(disk_type_&& disk, resources_config_t const& config, constraint_t& constraint, std::size_t threads,
                    uuid_t const& database) noexcept {
        using mount_t = basic_storage_mount<disk_type_, atomic_reference_, waiter_type_>;
        mount_t& opened = held_.template emplace<mount_t>(std::move(disk));
        if (status_t const started = opened.mount(config, constraint, threads, database); started.failed()) {
            held_.template emplace<std::monostate>();
            return started;
        }
        report_.volumes = opened.router().size();
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
 *  @brief Creates the metadata directory and mounts the storage of one database.
 *  @param[in] config Budgets, executor threads, the metadata directory and the data
 *      directories to mount.
 *  @param[in] database The database the volumes belong to, or an invalid identifier to take any.
 *  @return The resources, whose destructor tears the mount down in the order it was built in.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
expected<resources_shared_ptr<atomic_reference_, waiter_type_>> open_resources(resources_config_t const& config,
                                                                               uuid_t const& database) noexcept {
    if (config.directory.empty()) return status_t {status_code_t::invalid_argument_k, "A database needs a directory"};
    if (status_t const made = create_directories(config.directory.c_str()); made.failed()) return made;

    using resources_t = shared_resources<atomic_reference_, waiter_type_>;
    resources_shared_ptr<atomic_reference_, waiter_type_> resources = std::make_shared<resources_t>(
        config.memory_limit, config.gpu_memory_limit);
    if (resources->gpu_devices.resize(config.gpu_devices.size()) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "A database could not list the GPUs it may use"};
    for (std::size_t index = 0; index != config.gpu_devices.size(); ++index)
        resources->gpu_devices[index] = config.gpu_devices[index];

    if (status_t const mounted = resources->storage.mount(config, resources->memory_constraint, config.threads,
                                                          database);
        mounted.failed())
        return mounted;
    return resources;
}

#pragma endregion Shared Resources

#pragma endregion Resources

} // namespace unum::ustore::utilities
