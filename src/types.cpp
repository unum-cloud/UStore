/**
 *  @file src/types.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief Implements <tt>c/ustore/types.h</tt>: outcomes, the spelling of statuses and moods, the
 *      version, executors, and arenas.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstdio>  // `std::snprintf`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::find`, `std::reverse`
#include <array>       // `std::array`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`
#include <variant>     // `std::variant`, `std::get_if`, `std::visit`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/types.h" // `ustore_executor_for_ram_t`, `ustore_arena_init_t`, `ustore_version`, `USTORE_OS_LINUX_`

#if USTORE_OS_LINUX_
#include <dirent.h>        // `fdopendir`, `readdir`, `closedir`
#include <fcntl.h>         // `open`, `openat`, `O_DIRECTORY`
#include <sys/stat.h>      // `stat`, `S_ISBLK`
#include <sys/sysmacros.h> // `gnu_dev_major`, `gnu_dev_minor`
#include <unistd.h>        // `read`, `close`, `faccessat`
#endif

#include "ustore/utilities/blocking_files.hpp" // `path_max_bytes_k`
#include "ustore/utilities/nvme_volume.hpp"    // `parse_data_address`, `data_address_kind_t`

#include "lib.hpp" // `ustore_executor_handle_t`, `finish`, `wait_while_claimed`, `runs_holding_call`, `enumerator_spelling`

namespace unum::ustore {

/** The canonical spelling of every status. */
constexpr enumerator_spelling<ustore_status_t> status_names_k[] {
    {ustore_status_success_k, "success"},
    {ustore_status_deferred_k, "deferred"},
    {ustore_status_out_of_memory_k, "out-of-memory"},
    {ustore_status_read_only_k, "read-only"},
    {ustore_status_io_k, "io"},
    {ustore_status_device_k, "device"},
    {ustore_status_malformed_k, "malformed"},
    {ustore_status_internal_k, "internal"},
    {ustore_status_uninitialized_k, "uninitialized"},
    {ustore_status_contended_k, "contended"},
    {ustore_status_cancelled_k, "cancelled"},
    {ustore_status_unsupported_k, "unsupported"},
    {ustore_status_invalid_argument_k, "invalid-argument"},
    {ustore_status_not_found_k, "not-found"},
    {ustore_status_already_exists_k, "already-exists"},
    {ustore_status_write_conflict_k, "write-conflict"},
    {ustore_status_read_conflict_k, "read-conflict"},
    {ustore_status_phantom_conflict_k, "phantom-conflict"},
    {ustore_status_capacity_exhausted_k, "capacity-exhausted"},
};

/** The canonical spelling of every mood. */
constexpr enumerator_spelling<ustore_mood_t> mood_names_k[] {
    {ustore_mood_parked_k, "parked"},
    {ustore_mood_spinning_k, "spinning"},
};

/** The canonical spelling of every GPUDirect Storage mode. */
constexpr enumerator_spelling<ustore_gds_t> gds_names_k[] {
    {ustore_gds_automatic_k, "automatic"},
    {ustore_gds_direct_k, "direct"},
    {ustore_gds_compatibility_k, "compatibility"},
};

/** The canonical spelling of every GPU vendor. */
constexpr enumerator_spelling<ustore_gpu_vendor_t> gpu_vendor_names_k[] {
    {ustore_gpu_vendor_nvidia_k, "nvidia"},
    {ustore_gpu_vendor_amd_k, "amd"},
    {ustore_gpu_vendor_apple_k, "apple"},
};

/** The canonical spelling of every GPU class. */
constexpr enumerator_spelling<ustore_gpu_class_t> gpu_class_names_k[] {
    {ustore_gpu_class_nvidia_ampere_k, "nvidia-ampere"},
    {ustore_gpu_class_nvidia_ada_k, "nvidia-ada"},
    {ustore_gpu_class_nvidia_hopper_k, "nvidia-hopper"},
    {ustore_gpu_class_nvidia_blackwell_k, "nvidia-blackwell"},
    {ustore_gpu_class_nvidia_blackwellrtx_k, "nvidia-blackwellrtx"},
    {ustore_gpu_class_amd_cdna3_k, "amd-cdna3"},
    {ustore_gpu_class_amd_cdna4_k, "amd-cdna4"},
    {ustore_gpu_class_amd_cdna5_k, "amd-cdna5"},
    {ustore_gpu_class_apple_metal7_k, "apple-metal7"},
    {ustore_gpu_class_apple_metal9_k, "apple-metal9"},
    {ustore_gpu_class_apple_metal10_k, "apple-metal10"},
};

static_assert(static_cast<std::uint32_t>(gpu_class_t::amd_cdna5_k) == ustore_gpu_class_amd_cdna5_k);
static_assert(static_cast<std::uint32_t>(gpu_class_t::apple_metal10_k) == ustore_gpu_class_apple_metal10_k);
static_assert(static_cast<std::uint32_t>(gpu_vendor_t::amd_k) == ustore_gpu_vendor_amd_k);
static_assert(static_cast<std::uint32_t>(gpu_vendor_t::apple_k) == ustore_gpu_vendor_apple_k);

/** The canonical spelling of every pattern kind. */
constexpr enumerator_spelling<ustore_pattern_kind_t> pattern_kind_names_k[] {
    {ustore_pattern_regex_k, "regex"},
    {ustore_pattern_substrings_k, "substrings"},
};

/** The canonical spelling of every case sensitivity. */
constexpr enumerator_spelling<ustore_case_sensitivity_t> case_sensitivity_names_k[] {
    {ustore_cased_k, "cased"},
    {ustore_uncased_k, "uncased"},
};

/** The canonical spelling of every predicate cost. */
constexpr enumerator_spelling<ustore_predicate_cost_t> predicate_cost_names_k[] {
    {ustore_predicate_cheap_k, "cheap"},
    {ustore_predicate_expensive_k, "expensive"},
};

/** The canonical spelling of every CPU class, as UCall spells its classes. */
constexpr enumerator_spelling<ustore_cpu_class_t> cpu_class_names_k[] {
    {ustore_cpu_class_unknown_k, "unknown"},
    {ustore_cpu_class_serial_k, "serial"},
    {ustore_cpu_class_monoversioned_k, "monoversioned"},
    {ustore_cpu_class_intel_haswell_k, "intel-haswell"},
    {ustore_cpu_class_intel_sierra_k, "intel-sierra"},
    {ustore_cpu_class_intel_skylake_k, "intel-skylake"},
    {ustore_cpu_class_intel_icelake_k, "intel-icelake"},
    {ustore_cpu_class_aws_g2_k, "aws-g2"},
    {ustore_cpu_class_apple_m2_k, "apple-m2"},
    {ustore_cpu_class_aws_g5_k, "aws-g5"},
    {ustore_cpu_class_alibaba_c930_k, "alibaba-c930"},
    {ustore_cpu_class_ibm_power10_k, "ibm-power10"},
};

/** The canonical spelling of every transport. */
constexpr enumerator_spelling<ustore_transport_t> transport_names_k[] {
    {ustore_transport_automatic_k, "automatic"},     {ustore_transport_blocking_k, "blocking"},
    {ustore_transport_linux_uring_k, "linux-uring"}, {ustore_transport_linux_aio_k, "linux-aio"},
    {ustore_transport_linux_spdk_k, "linux-spdk"},
};

/** The canonical spelling of every phase a plan runs. */
constexpr enumerator_spelling<ustore_phase_t> phase_names_k[] {
    {ustore_phase_none_k, "none"},       {ustore_phase_flush_k, "flush"}, {ustore_phase_merge_k, "merge"},
    {ustore_phase_reclaim_k, "reclaim"}, {ustore_phase_done_k, "done"},
};

/** How a pool spawned here treats the thread spawning it: apart from its workers, or as its only thread in a build
 *  without OS threads. */
caller_exclusivity_t spawned_exclusivity() noexcept {
    return ram_executor_t::spawns_threads() ? caller_exclusivity_t::caller_exclusive_k
                                            : caller_exclusivity_t::caller_inclusive_k;
}

/** Explains why an executor could not have its threads. */
status_t pool_failure(st::status_t status) noexcept {
    switch (status) {
    case st::status_t::out_of_memory_heap_k: return {status_code_t::out_of_memory_k, "Out of memory for the threads"};
    case st::status_t::invalid_argument_k: return {status_code_t::invalid_argument_k, "The pool holds no threads"};
    case st::status_t::operation_not_permitted_k:
        return {status_code_t::unsupported_k, "The system refused to start the threads"};
    default: return {status_code_t::internal_k, "ForkUnion failed for an unknown reason"};
    }
}

/** Reads into @p domains the memory domains @p options name, refusing one @p topology lacks, or every domain the
 *  process may allocate on when they name none. */
status_t read_memory_domains(ustore_ram_options_t const& options, topology_t const& topology,
                             st::vector<fu_memory_domain_id_t>& domains) noexcept {
    std::size_t const available = topology.memory_domains_count();
    bool const derived = !options.memory_domains || !options.memory_domains_count;
    std::size_t const count = derived ? available : static_cast<std::size_t>(options.memory_domains_count);
    if (!count) return {status_code_t::unsupported_k, "The machine reports no memory domain"};
    if (st::failed(domains.resize(count))) return {status_code_t::out_of_memory_k, "Failed to copy the memory domains"};
    for (std::size_t index = 0; index != count; ++index) {
        if (derived) {
            domains[index] = topology.memory_domain(index);
            continue;
        }
        fu_memory_domain_id_t const named = options.memory_domains[index];
        bool known = false;
        for (std::size_t candidate = 0; candidate != available && !known; ++candidate)
            known = topology.memory_domain(candidate) == named;
        if (!known) return {status_code_t::invalid_argument_k, "No memory domain holds the identifier"};
        domains[index] = named;
    }
    return {};
}

/** Whether @p domains hold @p domain. */
bool serves(std::span<fu_memory_domain_id_t const> domains, fu_memory_domain_id_t domain) noexcept {
    return std::find(domains.begin(), domains.end(), domain) != domains.end();
}

/** Counts the cores of the compute domains of @p topology local to any of @p domains. */
std::size_t cores_near(topology_t const& topology, std::span<fu_memory_domain_id_t const> domains) noexcept {
    std::size_t cores = 0;
    for (std::size_t compute_domain = 0; compute_domain != topology.compute_domains_count(); ++compute_domain)
        if (serves(domains, topology.memory_domain_near(compute_domain))) cores += topology.cores_in(compute_domain);
    return cores;
}

/** What an NVMe executor spawns by: the fewest hardware queues of any disk under its paths, zero
 *  when none answers, and the memory domain every one of them sits nearest, or -1 when they differ
 *  or none is known. */
struct nvme_media_shape_t {
    std::size_t fewest_queues = 0;
    fu_memory_domain_id_t memory_domain = -1;
};

#if USTORE_OS_LINUX_

/** Hardware queues the disk at the sysfs directory @p disk spreads CPUs over: the entries of its
 *  @c mq/. */
std::size_t sysfs_hardware_queues(int disk) noexcept {
    int const listed = ::openat(disk, "mq", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (listed < 0) return 0;
    DIR* const entries = ::fdopendir(listed);
    if (!entries) return ::close(listed), 0u;
    std::size_t queues = 0;
    for (struct dirent const* entry = ::readdir(entries); entry; entry = ::readdir(entries))
        queues += entry->d_name[0] != '.';
    ::closedir(entries);
    return queues;
}

/** The memory domain nearest the PCI function behind the disk at the sysfs directory @p disk, as
 *  its controller's @c device/address names it; -1 off PCI or tied to no domain. */
fu_memory_domain_id_t sysfs_memory_domain(int disk) noexcept {
    int const attribute = ::openat(disk, "device/address", O_RDONLY | O_CLOEXEC);
    if (attribute < 0) return -1;
    char bus_id[32] = {};
    ssize_t const length = ::read(attribute, bus_id, sizeof(bus_id) - 1);
    ::close(attribute);
    if (length <= 0) return -1;
    if (bus_id[length - 1] == '\n') bus_id[length - 1] = 0;
    return topology_t::memory_domain_of_pci(bus_id);
}

/** The shape of the whole disk under @p location, a directory, a file or a block device: a
 *  partition's parent or a file's disk through @c st_dev; zeros where no disk backs it. */
nvme_media_shape_t disk_shape_of(std::string_view location) noexcept {
    char path[utilities::path_max_bytes_k];
    if (location.size() >= sizeof(path)) return {};
    std::memcpy(path, location.data(), location.size());
    path[location.size()] = '\0';
    struct stat description;
    if (::stat(path, &description) != 0) return {};
    dev_t const device = S_ISBLK(description.st_mode) ? description.st_rdev : description.st_dev;
    // The longest path the root spells with 32-bit majors and minors
    char sysfs[sizeof("/sys/dev/block/4294967295:4294967295")];
    std::snprintf(sysfs, sizeof(sysfs), "/sys/dev/block/%u:%u", static_cast<unsigned>(::gnu_dev_major(device)),
                  static_cast<unsigned>(::gnu_dev_minor(device)));
    int disk = ::open(sysfs, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (disk >= 0 && ::faccessat(disk, "partition", F_OK, 0) == 0) {
        int const partition = std::exchange(disk, -1);
        disk = ::openat(partition, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        ::close(partition);
    }
    if (disk < 0) return {};
    nvme_media_shape_t const shape {sysfs_hardware_queues(disk), sysfs_memory_domain(disk)};
    ::close(disk);
    return shape;
}

#else

/** Answers zeros, since only Linux reports the disk under a path. */
nvme_media_shape_t disk_shape_of(std::string_view) noexcept { return {}; }

#endif

/** The shape of the media under @p paths, data directories, raw media or SPDK namespaces as a
 *  database's open names them; an SPDK controller reports its memory domain but no queues before
 *  the driver attaches it. */
nvme_media_shape_t nvme_media_shape(std::span<ustore_bytes_view_t const> paths) noexcept {
    nvme_media_shape_t shape;
    for (std::size_t position = 0; position != paths.size(); ++position) {
        std::string_view const named {reinterpret_cast<char const*>(paths[position].data), paths[position].length};
        auto [address, parse_status] = utilities::parse_data_address(named);
        nvme_media_shape_t disk;
        if (parse_status.succeeded() && address.kind == utilities::data_address_kind_t::spdk_k) {
            auto [bus_id, bus_id_status] = utilities::pci_address_parsed(address.location);
            if (bus_id_status.succeeded()) disk.memory_domain = topology_t::memory_domain_of_pci(bus_id.text);
        }
        else if (parse_status.succeeded()) disk = disk_shape_of(address.location);
        if (disk.fewest_queues && (!shape.fewest_queues || disk.fewest_queues < shape.fewest_queues))
            shape.fewest_queues = disk.fewest_queues;
        shape.memory_domain = !position || shape.memory_domain == disk.memory_domain ? disk.memory_domain : -1;
    }
    return shape;
}

} // namespace unum::ustore

/** The queue set that @p options describe, one ring pair per entry of @p memory_domains on that
 *  domain, its transport probed when automatic and every zero size taking the engine's default. */
static expected<utilities::nvme_queues_t> make_nvme_queues(
    ustore_nvme_options_t const& options, std::span<fu_memory_domain_id_t const> memory_domains) noexcept {
    using utilities::io_transport_t;
    utilities::io_channel_config_t channels;
    bool const fits = take_unless_zero(channels.depth, options.queue_depth) &&
                      take_unless_zero(channels.request_buffer_bytes, options.request_buffer_bytes) &&
                      take_unless_zero(channels.request_buffers_count, options.request_buffers_count) &&
                      take_unless_zero(channels.stream_buffer_bytes, options.stream_buffer_bytes) &&
                      take_unless_zero(channels.stream_buffers_count, options.stream_buffers_count);
    if (!fits) return status_t {status_code_t::invalid_argument_k, "An NVMe option exceeds its range"};
    io_transport_t wanted = io_transport_t::automatic_k;
    switch (options.transport) {
    case ustore_transport_automatic_k: wanted = io_transport_t::automatic_k; break;
    case ustore_transport_blocking_k: wanted = io_transport_t::blocking_k; break;
    case ustore_transport_linux_uring_k: wanted = io_transport_t::linux_uring_k; break;
    case ustore_transport_linux_aio_k: wanted = io_transport_t::linux_aio_k; break;
    case ustore_transport_linux_spdk_k: wanted = io_transport_t::linux_spdk_k; break;
    default: return status_t {status_code_t::invalid_argument_k, "Unknown transport"};
    }
    using linux_uring_t = utilities::linux_uring_files_disk<fu::standard_atomic_ref, fu::standard_yield_t>;
    using linux_aio_t = utilities::linux_aio_files_disk<fu::standard_atomic_ref, fu::standard_yield_t>;
    auto [transport, transport_status] = utilities::probed_transport(wanted, linux_uring_t::probe(),
                                                                     linux_aio_t::probe());
    if (transport_status.failed()) return transport_status;
    return utilities::nvme_queues_t::make(transport, channels, memory_domains);
}

/** Makes in @p made the executor running calls on @p pool. Each pool thread's scratch lives on its
 *  local memory domain of @p topology when @p domains hold it, and on the first of them otherwise;
 *  the call's scratch and every arena the executor makes live where pool thread 0's does. */
static status_t make_executor(ram_executor_t pool, topology_t const& topology,
                              std::span<fu_memory_domain_id_t const> domains, std::size_t scratch_limit,
                              ustore_executor_t& made) noexcept {
    std::size_t const bytes_limit = scratch_limit ? scratch_limit : scratch_limit_default_k;
    std::size_t const threads = pool.threads_count();
    ustore_executor_handle_t* const executor = new (std::nothrow) ustore_executor_handle_t {std::move(pool)};
    if (!executor) return {status_code_t::out_of_memory_k, "Failed to allocate the executor"};
    if (st::failed(executor->scratches.resize(threads + 1))) {
        delete executor;
        return {status_code_t::out_of_memory_k, "Failed to allocate the scratch of the threads"};
    }

    executor->memory_domain = domains.front();
    auto place_scratch = [&](std::size_t thread, std::size_t compute_domain) noexcept {
        fu_memory_domain_id_t const local = topology.memory_domain_near(compute_domain);
        fu_memory_domain_id_t const placed = serves(domains, local) ? local : domains.front();
        executor->scratches[thread].memory = host_blocks_t {.memory_domain = placed, .bytes_limit = bytes_limit};
        if (thread == 0) executor->memory_domain = placed;
    };
    if (st::status_t const placed = executor->pool.for_threads(place_scratch); st::failed(placed)) {
        delete executor;
        return pool_failure(placed);
    }
    executor->scratches.back().memory = host_blocks_t {.memory_domain = executor->memory_domain,
                                                       .bytes_limit = bytes_limit};
    made = executor;
    return {};
}

ustore_status_t ustore_outcome_wait(ustore_outcome_t* outcome) { return host_verbs().wait(outcome); }

ustore_status_t ustore_outcome_status(ustore_outcome_t* outcome) { return host_verbs().load(outcome); }

char const* ustore_status_name(ustore_status_t status) { return name_in(status_names_k, status); }

ustore_status_t ustore_status_from_name(ustore_bytes_view_t name, ustore_status_t* status) {
    return value_in(status_names_k, name, status);
}

char const* ustore_mood_name(ustore_mood_t mood) { return name_in(mood_names_k, mood); }
ustore_status_t ustore_mood_from_name(ustore_bytes_view_t name, ustore_mood_t* mood) {
    return value_in(mood_names_k, name, mood);
}
char const* ustore_cpu_class_name(ustore_cpu_class_t cpu_class) { return name_in(cpu_class_names_k, cpu_class); }
ustore_status_t ustore_cpu_class_from_name(ustore_bytes_view_t name, ustore_cpu_class_t* cpu_class) {
    return value_in(cpu_class_names_k, name, cpu_class);
}
char const* ustore_pattern_kind_name(ustore_pattern_kind_t kind) { return name_in(pattern_kind_names_k, kind); }
ustore_status_t ustore_pattern_kind_from_name(ustore_bytes_view_t name, ustore_pattern_kind_t* kind) {
    return value_in(pattern_kind_names_k, name, kind);
}
char const* ustore_case_sensitivity_name(ustore_case_sensitivity_t sensitivity) {
    return name_in(case_sensitivity_names_k, sensitivity);
}
ustore_status_t ustore_case_sensitivity_from_name(ustore_bytes_view_t name, ustore_case_sensitivity_t* sensitivity) {
    return value_in(case_sensitivity_names_k, name, sensitivity);
}
char const* ustore_predicate_cost_name(ustore_predicate_cost_t cost) { return name_in(predicate_cost_names_k, cost); }
ustore_status_t ustore_predicate_cost_from_name(ustore_bytes_view_t name, ustore_predicate_cost_t* cost) {
    return value_in(predicate_cost_names_k, name, cost);
}
char const* ustore_gds_name(ustore_gds_t gds) { return name_in(gds_names_k, gds); }
ustore_status_t ustore_gds_from_name(ustore_bytes_view_t name, ustore_gds_t* gds) {
    return value_in(gds_names_k, name, gds);
}
char const* ustore_gpu_vendor_name(ustore_gpu_vendor_t vendor) { return name_in(gpu_vendor_names_k, vendor); }
ustore_status_t ustore_gpu_vendor_from_name(ustore_bytes_view_t name, ustore_gpu_vendor_t* vendor) {
    return value_in(gpu_vendor_names_k, name, vendor);
}
char const* ustore_gpu_class_name(ustore_gpu_class_t gpu_class) { return name_in(gpu_class_names_k, gpu_class); }
ustore_status_t ustore_gpu_class_from_name(ustore_bytes_view_t name, ustore_gpu_class_t* gpu_class) {
    return value_in(gpu_class_names_k, name, gpu_class);
}
char const* ustore_transport_name(ustore_transport_t transport) { return name_in(transport_names_k, transport); }
ustore_status_t ustore_transport_from_name(ustore_bytes_view_t name, ustore_transport_t* transport) {
    return value_in(transport_names_k, name, transport);
}

char const* ustore_phase_name(ustore_phase_t phase) { return name_in(phase_names_k, phase); }
ustore_status_t ustore_phase_from_name(ustore_bytes_view_t name, ustore_phase_t* phase) {
    return value_in(phase_names_k, name, phase);
}

void ustore_version(int* major, int* minor, int* patch) {
    *major = USTORE_VERSION_MAJOR;
    *minor = USTORE_VERSION_MINOR;
    *patch = USTORE_VERSION_PATCH;
}

/** The first line of @c VERSION, pre-release suffix included, which CMake hands every unit as
 *  @c USTORE_VERSION. */
char const* ustore_version_name(void) { return USTORE_VERSION; }

void ustore_executor_for_ram(ustore_executor_for_ram_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    if (!arguments->executor)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an output for the executor"});
    *arguments->executor = nullptr;
    auto const [ceiling, pick_status] = pick_cpu_ceiling(arguments->cpu_class_ceiling);
    if (pick_status.failed()) return finish(outcome, pick_status);
    fu_capabilities_t const allowed = forkunion_allowed_of(ceiling);
    st::expected<topology_t> topology = topology_t::make();
    if (!topology) return finish(outcome, pool_failure(topology.status()));
    st::vector<fu_memory_domain_id_t> domains;
    if (status_t const status = read_memory_domains(arguments->ram, *topology, domains); status.failed())
        return finish(outcome, status);
    std::size_t threads = arguments->threads;
    if (!ram_executor_t::spawns_threads()) {
        if (threads > 1) return finish(outcome, {status_code_t::unsupported_k, "This build spawns no threads"});
        threads = 1;
    }
    else if (!threads) threads = fu::max_of<std::size_t>(cores_near(*topology, domains) * 7 / 8, 1);
    // ForkUnion spawns near one memory domain or across every one, so naming several but not all takes the machine
    bool const near_one = domains.size() == 1 && topology->memory_domains_count() > 1;
    st::expected<ram_executor_t> pool = near_one
                                            ? ram_executor_t::make_near(*topology, domains[0], threads,
                                                                        spawned_exclusivity(), allowed)
                                            : ram_executor_t::make(*topology, threads, spawned_exclusivity(), allowed);
    if (!pool) return finish(outcome, pool_failure(pool.status()));
    finish(outcome,
           make_executor(std::move(*pool), *topology, domains, arguments->scratch_limit, *arguments->executor));
}

/** Makes in @p made an executor of @p threads host threads spread over the whole machine, as the
 *  VRAM executor takes, treating its caller as @p exclusivity says, using only the capabilities in
 *  @p allowed, whose scratch of @p scratch_limit bytes lives near each thread. */
static status_t spawn_across_machine(std::size_t threads, caller_exclusivity_t exclusivity, fu_capabilities_t allowed,
                                     std::size_t scratch_limit, ustore_executor_t& made) noexcept {
    st::expected<topology_t> topology = topology_t::make();
    if (!topology) return pool_failure(topology.status());
    st::vector<fu_memory_domain_id_t> domains;
    if (status_t const status = read_memory_domains({}, *topology, domains); status.failed()) return status;
    if (!ram_executor_t::spawns_threads()) {
        if (threads > 1) return {status_code_t::unsupported_k, "This build spawns no threads"};
        threads = 1;
    }
    st::expected<ram_executor_t> pool = ram_executor_t::make(*topology, threads, exclusivity, allowed);
    if (!pool) return pool_failure(pool.status());
    return make_executor(std::move(*pool), *topology, domains, scratch_limit, made);
}

void ustore_executor_for_nvme(ustore_executor_for_nvme_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    if (!arguments->executor)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an output for the executor"});
    *arguments->executor = nullptr;
    if (arguments->nvme.transport > ustore_transport_linux_spdk_k)
        return finish(outcome, {status_code_t::invalid_argument_k, "Unknown transport"});
    if (arguments->nvme.paths_count && !arguments->nvme.paths)
        return finish(outcome, {status_code_t::invalid_argument_k, "Paths with a count need their views"});
    auto const [ceiling, pick_status] = pick_cpu_ceiling(arguments->cpu_class_ceiling);
    if (pick_status.failed()) return finish(outcome, pick_status);
    st::expected<topology_t> topology = topology_t::make();
    if (!topology) return finish(outcome, pool_failure(topology.status()));
    st::vector<fu_memory_domain_id_t> domains;
    if (status_t const status = read_memory_domains(arguments->ram, *topology, domains); status.failed())
        return finish(outcome, status);

    // Named domains win; else drives on one domain of several keep their threads and staging there
    std::span<ustore_bytes_view_t const> const paths {arguments->nvme.paths,
                                                      static_cast<std::size_t>(arguments->nvme.paths_count)};
    nvme_media_shape_t const shape = nvme_media_shape(paths);
    bool const named = arguments->ram.memory_domains && arguments->ram.memory_domains_count;
    fu_memory_domain_id_t const near_domain = named                                  ? domains.front()
                                              : topology->memory_domains_count() > 1 ? shape.memory_domain
                                                                                     : fu_memory_domain_id_t {-1};
    std::size_t threads = arguments->threads;
    if (!ram_executor_t::spawns_threads()) {
        if (threads > 1) return finish(outcome, {status_code_t::unsupported_k, "This build spawns no threads"});
        threads = 1;
    }
    else if (!threads) {
        std::size_t const cores = near_domain != -1 ? cores_near(*topology, {&near_domain, 1})
                                                    : topology->logical_cores_count();
        // Without queues to count, the count stays as low as a mount keeps, as each pins 12 MB
        threads = shape.fewest_queues ? fu::min_of(shape.fewest_queues, cores)
                                      : std::clamp<std::size_t>(cores / 2, 1, 8);
        threads = fu::max_of<std::size_t>(threads, 1);
    }
    fu_capabilities_t const allowed = forkunion_allowed_of(ceiling);
    st::expected<ram_executor_t> pool = near_domain != -1
                                            ? ram_executor_t::make_near(*topology, near_domain, threads,
                                                                        spawned_exclusivity(), allowed)
                                            : ram_executor_t::make(*topology, threads, spawned_exclusivity(), allowed);
    if (!pool) return finish(outcome, pool_failure(pool.status()));
    ustore_executor_t made = nullptr;
    if (status_t const status = make_executor(std::move(*pool), *topology, domains, arguments->scratch_limit, made);
        status.failed())
        return finish(outcome, status);

    // A ring pair per pool thread and one for the holding call, each on its scratch's domain;
    // the queue set resolves an automatic transport once, as a database open does
    st::vector<fu_memory_domain_id_t> staging_domains;
    if (staging_domains.resize(made->scratches.size()) != st::status_t::success_k)
        return ustore_executor_free(made),
               finish(outcome, {status_code_t::out_of_memory_k, "Failed to copy the threads' memory domains"});
    for (std::size_t thread = 0; thread != made->scratches.size(); ++thread)
        staging_domains[thread] = host_blocks_of(&made->scratches[thread])->memory_domain;
    auto [queues, queues_status] = make_nvme_queues(arguments->nvme, staging_domains);
    if (queues_status.failed()) return ustore_executor_free(made), finish(outcome, queues_status);
    made->device.emplace<utilities::nvme_queues_t>(std::move(queues));
    *arguments->executor = made;
    finish(outcome, {});
}

void ustore_executor_for_vram(ustore_executor_for_vram_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    if (!arguments->executor)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an output for the executor"});
    *arguments->executor = nullptr;
    if (arguments->gds > ustore_gds_compatibility_k)
        return finish(outcome, {status_code_t::invalid_argument_k, "Unknown GPUDirect Storage mode"});
    std::span<gpu_device_t const> const devices = gpu_devices();
    if (arguments->device < 0 || static_cast<std::size_t>(arguments->device) >= devices.size())
        return finish(outcome, {status_code_t::device_k, "No GPU holds the ordinal"});
    gpu_device_t const& device = devices[static_cast<std::size_t>(arguments->device)];
    if (device.vendor != gpu_vendor_t::nvidia_k && arguments->gds == ustore_gds_direct_k)
        return finish(outcome, {status_code_t::device_k, "GPUDirect Storage reaches NVIDIA devices alone"});
    if (arguments->gpu_class_ceiling != ustore_gpu_class_unknown_k &&
        std::string_view {ustore_gpu_class_name(arguments->gpu_class_ceiling)} == "unrecognized")
        return finish(outcome, {status_code_t::invalid_argument_k, "Unknown GPU class ceiling"});
    gpu_runtime_t& runtime = gpu_runtime();
    gpu_class_t const gpu_class = pick_gpu_class(runtime.drivers, device,
                                                 static_cast<gpu_class_t>(arguments->gpu_class_ceiling));
    if (gpu_class == gpu_class_t::unknown_k)
        return finish(outcome,
                      {status_code_t::unsupported_k, "No compiled class runs on the device under the ceiling"});
    // The caller is the one host thread: it stages, feeds the device and waits on it
    ustore_executor_t made = nullptr;
    if (status_t const status = spawn_across_machine(1, caller_exclusivity_t::caller_inclusive_k, fu_capabilities_all_k,
                                                     0, made);
        status.failed())
        return finish(outcome, status);
    if (status_t const status = runtime.spawn(device, gpu_class, arguments->stream, arguments->scratch_limit,
                                              arguments->gds, made->device);
        status.failed()) {
        ustore_executor_free(made);
        return finish(outcome, status);
    }
    made->gpu_class = gpu_class;
    *arguments->executor = made;
    finish(outcome, {});
}

void ustore_gpu_devices(ustore_gpu_devices_t* arguments) {
    std::span<gpu_device_t const> const devices = gpu_devices();
    std::size_t const copied = arguments->devices ? fu::min_of<std::size_t>(devices.size(), arguments->devices_capacity)
                                                  : 0;
    for (std::size_t position = 0; position != copied; ++position) {
        gpu_device_t const& device = devices[position];
        ustore_gpu_device_t& listed = arguments->devices[position];
        listed = {static_cast<ustore_gpu_vendor_t>(device.vendor),
                  device.vendor_ordinal,
                  static_cast<ustore_gpu_class_t>(gpu_runtime().classes[position]),
                  device.compute_units,
                  device.memory_bytes,
                  {}};
        std::memcpy(listed.name, device.name, sizeof(listed.name));
    }
    if (arguments->devices_count) *arguments->devices_count = devices.size();
    finish(*arguments->outcome, {});
}

void ustore_executor_drain(ustore_executor_drain_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    ustore_executor_handle_t* const executor = arguments->executor;
    if (arguments->mood != ustore_mood_parked_k && arguments->mood != ustore_mood_spinning_k)
        return finish(outcome, {status_code_t::invalid_argument_k, "The mood is neither parked nor spinning"});
    if (!executor) return finish(outcome, {});
    if (runs_holding_call(*executor))
        return finish(outcome, {status_code_t::contended_k, "A callback cannot drain the executor running it"});
    wait_while_claimed(executor->holding_call.claim);
    join_delegated(*executor);
    // Every dispatch wakes a parked pool
    if (arguments->mood == ustore_mood_parked_k) executor->pool.park();
    finish(outcome, {});
}

void ustore_executor_free(ustore_executor_t executor) {
    if (!executor) return;
    wait_while_claimed(executor->holding_call.claim);
    join_delegated(*executor);
    for (ustore_arena_handle_t& scratch : executor->scratches) arena_free_blocks(scratch);
    delete executor;
}

void ustore_arena_init(ustore_arena_init_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    if (!arguments->arena)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an output for the arena"});
    *arguments->arena = nullptr;
    ustore_arena_t const arena = new (std::nothrow) ustore_arena_handle_t {};
    if (!arena) return finish(outcome, {status_code_t::out_of_memory_k, "Failed to allocate the arena"});
    // Nothing is allocated until the first call writes, on the memory domain or the device its executor names
    if (ustore_executor_t const executor = arguments->executor)
        std::visit(
            [&]<typename device_type_>(device_type_ const& device) noexcept {
                if constexpr (gpu_executor_concept<device_type_>)
                    arena->memory = typename device_type_::arena_blocks_t {.device = device.device()};
                else arena->memory = host_blocks_t {.memory_domain = executor->memory_domain};
            },
            executor->device);
    *arguments->arena = arena;
    finish(outcome, {});
}

void ustore_arena_free(ustore_arena_t arena) {
    if (!arena) return;
    wait_named_calls(arena->named_calls);
    destroy_arena(arena);
}

void ustore_arena_describe(ustore_arena_describe_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    ustore_arena_t const arena = arguments->arena;
    if (!arena || !arguments->description)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an arena and a description output"});
    ustore_arena_description_t& description = *arguments->description;
    description = {ustore_residency_ram_k, -1};
    gpu_vendor_t vendor = gpu_vendor_t::nvidia_k;
    std::int32_t vendor_ordinal = -1;
    if (cuda_blocks_t const* const cuda = std::get_if<cuda_blocks_t>(&arena->memory)) vendor_ordinal = cuda->device;
    if (hip_blocks_t const* const hip = std::get_if<hip_blocks_t>(&arena->memory)) {
        vendor = gpu_vendor_t::amd_k;
        vendor_ordinal = hip->device;
    }
    if (metal_blocks_t const* const metal = std::get_if<metal_blocks_t>(&arena->memory)) {
        vendor = gpu_vendor_t::apple_k;
        vendor_ordinal = metal->device;
    }
    if (vendor_ordinal < 0) return finish(outcome, {});
    description.residency = ustore_residency_vram_k;
    std::span<gpu_device_t const> const devices = gpu_devices();
    for (std::size_t position = 0; position != devices.size(); ++position)
        if (devices[position].vendor == vendor &&
            devices[position].vendor_ordinal == static_cast<std::uint32_t>(vendor_ordinal))
            description.device = static_cast<std::int32_t>(position);
    finish(outcome, {});
}
