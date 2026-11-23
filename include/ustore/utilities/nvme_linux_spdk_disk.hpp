/**
 *  @file include/ustore/utilities/nvme_linux_spdk_disk.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The ring SPDK offers: a user-space NVMe driver over the namespaces of one controller, one
 *      I/O queue pair per queue set queue and controller.
 *
 *  This transport presents each namespace it registers as a target, named by its number, and keeps
 *  no name mapping of its own; the manifest above it names what the extents hold, so a torn write
 *  of a map cannot lose a drive. The driver attaches a controller once per process, so every disk
 *  opened on one address shares that attachment, and no two disks register the same namespace.
 *  Nothing here blocks, as the driver offers no interrupt: @c spdk_nvme_qpair_process_completions
 *  is the only way a command is ever seen to finish.
 *
 *  An I/O queue pair belongs to a thread's queue, since an @c spdk_nvme_qpair holds no lock and a
 *  second thread polling one corrupts its tracker list without a word. The queue allocates it on
 *  the first @c register_media of a disk on its controller and every later disk on that controller
 *  shares it; @c queue_pairs_limit is the controller's queue-pair count, which a queue set of more
 *  queues than that exhausts at @c register_media.
 *
 *  The environment is the hazard the rest of the file is arranged around. @c rte_eal_init
 *  calls @c exit on the process when it wants hugepages and finds none, when the drive is
 *  still bound to the kernel @c nvme driver, or when @c /dev/vfio cannot be opened, so a
 *  failed mount would take the database down with it. @c linux_spdk_environment::probe
 *  therefore decides by reading @c /proc/meminfo, @c /sys/kernel/mm/hugepages and the driver
 *  link under @c /sys/bus/pci/devices, and never initialises anything; the environment is
 *  acquired only once the probe says it can be. A fabric address has none of those hazards,
 *  since the driver copies every byte through a socket and never translates a page, so its
 *  environment runs on plain pages and the probe passes it at once.
 */

#pragma once

#include <cassert> // `assert`
#include <cerrno>  // `errno`, `EINTR`, `ENOMEM`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstdio>  // `std::snprintf`

#include "ustore/types.h" // `USTORE_OS_LINUX_`

#if USTORE_OS_LINUX_
#include <fcntl.h>  // `O_RDONLY`, `O_CLOEXEC`
#include <unistd.h> // `read`, `readlink`, `access`, `close`
#endif

#include <array>       // `std::array`
#include <bit>         // `std::has_single_bit`
#include <charconv>    // `std::from_chars`
#include <limits>      // `std::numeric_limits`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`

#if USTORE_WITH_LINUX_SPDK
#include <spdk/env.h> // `spdk_env_init`, `spdk_mem_register`, `spdk_vtophys`, `spdk_dma_malloc_socket`, `spdk_dma_free`
#include <spdk/env_dpdk.h> // `spdk_env_dpdk_external_init`
#include <spdk/nvme.h>     // `spdk_nvme_connect`, `spdk_nvme_ns_cmd_read`, `spdk_nvme_qpair`
#endif

#include <smashtable/basic_growing_pool.hpp> // `st::basic_growing_pool`
#include <smashtable/basic_vector.hpp>       // `st::vector`

#include "ustore/utilities/nvme_ring.hpp" // `request_t`, `completion_t`, `queue_ref_t`, `portable_alignment_k`, `is_address_aligned`
#include "ustore/utilities/types.hpp" // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::utilities {

#pragma region Probe

/** Whether this machine would let a user-space driver claim a device, or why it would not. */
enum class linux_spdk_probe_t : std::uint8_t {

    /** The device is there, hugepages are free, and the binding lets a user-space driver claim it. */
    available_k,

    /** The device is there, and the environment would abort for want of a free hugepage. */
    without_hugepages_k,

    /** The kernel still owns the device, or this process may not open @c /dev/vfio. */
    without_permission_k,

    /** No device answers at that address, which includes an address that is not a PCIe one. */
    not_found_k,
};

/** Which driver owns a PCIe device, which decides whether a user-space driver may claim it. */
enum class pci_driver_t : std::uint8_t {

    /** Nothing is bound, so neither the kernel nor a user-space driver can reach the device. */
    unbound_k,

    /** The VFIO passthrough driver, which is what a user-space NVMe driver wants. */
    vfio_pci_k,

    /** One of the UIO passthrough drivers, which serves the same purpose without an IOMMU. */
    uio_generic_k,

    /** The kernel's own NVMe driver, which must be unbound before a user-space one binds. */
    kernel_nvme_k,

    /** Some other driver holds the device, which is a refusal for the same reason. */
    foreign_k,
};

/** What a @c /proc/meminfo-shaped text says about the hugepages a DMA-capable environment needs. */
struct hugepage_inventory_t {

    /** Hugepages the kernel has reserved in the default pool. */
    std::uint64_t total_pages = 0;

    /** Hugepages of that pool nothing has taken yet. */
    std::uint64_t free_pages = 0;

    /** Bytes one hugepage of the default pool spans. */
    std::uint64_t page_bytes = 0;
};

/** A PCIe address in the @c domain:bus:device.function form both sysfs and the driver spell. */
struct pci_address_t {

    /** The address as sysfs names it, with the domain filled in when the caller left it out. */
    char text[13] = {};
};

/** Which way an address reaches its controller, which decides what the machine must offer for it. */
enum class linux_spdk_transport_t : std::uint8_t {

    /** A PCIe address, bare or behind @c trtype:PCIe, whose controller the driver claims and reaches by DMA. */
    pcie_k,

    /** A fabric transport ID, whose controller answers over a socket the driver copies through. */
    fabric_k,
};

/** Whether @p text starts with @p prefix, comparing letters without regard to case. */
[[nodiscard]] constexpr bool starts_with_uncased(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) return false;
    for (std::size_t index = 0; index != prefix.size(); ++index) {
        char first = text[index];
        char second = prefix[index];
        if (first >= 'A' && first <= 'Z') first = static_cast<char>(first - 'A' + 'a');
        if (second >= 'A' && second <= 'Z') second = static_cast<char>(second - 'A' + 'a');
        if (first != second) return false;
    }
    return true;
}

/** The value after @p key in a space-separated @c key:value transport ID, or an empty view without one. */
[[nodiscard]] constexpr std::string_view transport_field(std::string_view text, std::string_view key) noexcept {
    for (std::size_t start = 0; start < text.size();) {
        std::size_t const stop = text.find(' ', start);
        std::size_t const item_bytes = stop == std::string_view::npos ? text.size() - start : stop - start;
        std::string_view const item = text.substr(start, item_bytes);
        start += item_bytes + 1;
        if (starts_with_uncased(item, key)) return item.substr(key.size());
    }
    return {};
}

/** The transport @p address names: a fabric when its @c trtype: says one, PCIe for a bare address or @c PCIe. */
[[nodiscard]] constexpr linux_spdk_transport_t spdk_transport_named(std::string_view address) noexcept {
    std::string_view const kind = transport_field(address, "trtype:");
    if (kind.empty() || (kind.size() == 4 && starts_with_uncased(kind, "pcie"))) return linux_spdk_transport_t::pcie_k;
    return linux_spdk_transport_t::fabric_k;
}

/**
 *  @brief The number on the @p key line of a @c /proc/meminfo-shaped @p text, or zero when it has none.
 *  @param[in] text The whole file, or any text shaped like it, with lines of the form <tt>Key: value</tt>.
 *  @param[in] key The line to read, matched in full rather than as a prefix of a longer name.
 *  @return The decimal value that follows the colon, ignoring the unit that may follow it.
 */
[[nodiscard]] inline std::uint64_t meminfo_value(std::string_view text, std::string_view key) noexcept {
    for (std::size_t start = 0; start < text.size();) {
        std::size_t const line_break = text.find('\n', start);
        std::size_t const line_bytes = line_break == std::string_view::npos ? text.size() - start : line_break - start;
        std::string_view line = text.substr(start, line_bytes);
        start += line_bytes + 1;
        if (!line.starts_with(key)) continue;
        line.remove_prefix(key.size());
        if (line.empty() || line.front() != ':') continue;
        line.remove_prefix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        std::uint64_t value = 0;
        while (!line.empty() && line.front() >= '0' && line.front() <= '9') {
            value = value * 10 + static_cast<std::uint64_t>(line.front() - '0');
            line.remove_prefix(1);
        }
        return value;
    }
    return 0;
}

/**
 *  @brief Reads the default hugepage pool out of a @c /proc/meminfo-shaped @p text.
 *  @param[in] text The whole file, or a synthetic text shaped like it.
 *  @return The pool, whose @c page_bytes is zero on a kernel that reports no hugepages at all.
 */
[[nodiscard]] inline hugepage_inventory_t hugepages_in_meminfo(std::string_view text) noexcept {
    hugepage_inventory_t inventory;
    inventory.total_pages = meminfo_value(text, "HugePages_Total");
    inventory.free_pages = meminfo_value(text, "HugePages_Free");
    inventory.page_bytes = meminfo_value(text, "Hugepagesize") * 1024ull;
    return inventory;
}

/**
 *  @brief Names the driver a PCI device's @c driver link under @c /sys/bus/pci/devices resolves to.
 *  @param[in] link The link target, which is a relative path whose last component is the driver.
 *  @return Which driver holds the device, @c unbound_k for an empty link.
 */
[[nodiscard]] inline pci_driver_t pci_driver_named(std::string_view link) noexcept {
    if (link.empty()) return pci_driver_t::unbound_k;
    std::size_t const separator = link.find_last_of('/');
    std::string_view const leaf = separator == std::string_view::npos ? link : link.substr(separator + 1);
    if (leaf.empty()) return pci_driver_t::unbound_k;
    if (leaf == "vfio-pci") return pci_driver_t::vfio_pci_k;
    if (leaf == "uio_pci_generic" || leaf == "igb_uio") return pci_driver_t::uio_generic_k;
    if (leaf == "nvme") return pci_driver_t::kernel_nvme_k;
    return pci_driver_t::foreign_k;
}

/** Reads at most @p digits_limit hexadecimal digits of @p text, refusing anything else. */
inline expected<std::uint64_t> hexadecimal_field(std::string_view text, std::size_t digits_limit) noexcept {
    if (text.empty() || text.size() > digits_limit)
        return status_t {status_code_t::invalid_argument_k, "A PCIe address field is one to four hex digits"};
    std::uint64_t value = 0;
    for (char const character : text) {
        std::uint64_t digit = 0;
        if (character >= '0' && character <= '9') digit = static_cast<std::uint64_t>(character - '0');
        else if (character >= 'a' && character <= 'f') digit = static_cast<std::uint64_t>(character - 'a') + 10;
        else if (character >= 'A' && character <= 'F') digit = static_cast<std::uint64_t>(character - 'A') + 10;
        else return status_t {status_code_t::invalid_argument_k, "A PCIe address field holds hex digits alone"};
        value = value * 16 + digit;
    }
    return value;
}

/**
 *  @brief Normalises @p spelled into the @c domain:bus:device.function form sysfs names devices by.
 *  @param[in] spelled Either @c 0000:01:00.0 or the short @c 01:00.0 the driver also accepts.
 *  @return The normalised address, or why it is not a PCIe address at all.
 */
inline expected<pci_address_t> pci_address_parsed(std::string_view spelled) noexcept {
    std::size_t colons = 0;
    for (char const character : spelled) colons += character == ':' ? 1u : 0u;
    if (colons != 1 && colons != 2)
        return status_t {status_code_t::invalid_argument_k, "A PCIe address spells a bus, a device and a function"};

    std::string_view rest = spelled;
    std::uint64_t domain = 0;
    if (colons == 2) {
        std::size_t const separator = rest.find(':');
        auto [parsed, parse_domain_status] = hexadecimal_field(rest.substr(0, separator), 4);
        if (parse_domain_status.failed()) return parse_domain_status;
        domain = parsed;
        rest.remove_prefix(separator + 1);
    }

    std::size_t const separator = rest.find(':');
    auto [bus, parse_bus_status] = hexadecimal_field(rest.substr(0, separator), 2);
    if (parse_bus_status.failed()) return parse_bus_status;
    rest.remove_prefix(separator + 1);

    std::size_t const dot = rest.find('.');
    if (dot == std::string_view::npos)
        return status_t {status_code_t::invalid_argument_k, "A PCIe address separates its function with a dot"};
    auto [device, parse_device_status] = hexadecimal_field(rest.substr(0, dot), 2);
    if (parse_device_status.failed()) return parse_device_status;
    auto [function, parse_function_status] = hexadecimal_field(rest.substr(dot + 1), 1);
    if (parse_function_status.failed()) return parse_function_status;
    if (device > 0x1F || function > 7)
        return status_t {status_code_t::invalid_argument_k, "A PCIe device is below 32 and a function below 8"};

    pci_address_t address;
    [[maybe_unused]] int const written = std::snprintf(address.text, sizeof(address.text), "%04x:%02x:%02x.%01x",
                                                       static_cast<unsigned>(domain), static_cast<unsigned>(bus),
                                                       static_cast<unsigned>(device), static_cast<unsigned>(function));
    return address;
}

#pragma endregion Probe

#pragma region Files

#if USTORE_OS_LINUX_

/** Reads what fits of @p path into @p into, answering an empty text when the file will not open. */
[[nodiscard]] inline std::string_view read_system_text(char const* path, std::span<char> into) noexcept {
    int const file = ::open(path, O_RDONLY | O_CLOEXEC);
    if (file < 0) return {};
    std::size_t filled = 0;
    while (filled != into.size()) {
        long const taken = ::read(file, into.data() + filled, into.size() - filled);
        if (taken < 0 && errno == EINTR) continue;
        if (taken <= 0) break;
        filled += static_cast<std::size_t>(taken);
    }
    ::close(file);
    return std::string_view {into.data(), filled};
}

/** Reads where the symbolic link @p path points, answering an empty text when it is not one. */
[[nodiscard]] inline std::string_view read_system_link(char const* path, std::span<char> into) noexcept {
    long const taken = ::readlink(path, into.data(), into.size());
    if (taken <= 0) return {};
    return std::string_view {into.data(), static_cast<std::size_t>(taken)};
}

/** Whether @p path names something this process can see, which is how a device is found. */
[[nodiscard]] inline bool system_path_exists(char const* path) noexcept { return ::access(path, F_OK) == 0; }

/** Whether this process may both read and write @p path, which VFIO demands of its own node. */
[[nodiscard]] inline bool system_path_is_usable(char const* path) noexcept { return ::access(path, R_OK | W_OK) == 0; }

#else

/** Answers an empty text, since this platform keeps no such file. */
[[nodiscard]] inline std::string_view read_system_text(char const*, std::span<char>) noexcept { return {}; }

/** Answers an empty text, since this platform keeps no such link. */
[[nodiscard]] inline std::string_view read_system_link(char const*, std::span<char>) noexcept { return {}; }

/** Answers that nothing is there, since this platform keeps no sysfs. */
[[nodiscard]] inline bool system_path_exists(char const*) noexcept { return false; }

/** Answers that nothing is usable, since this platform keeps no sysfs. */
[[nodiscard]] inline bool system_path_is_usable(char const*) noexcept { return false; }

#endif

#pragma endregion Files

#pragma region Configuration

/** What backs the memory the driver allocates for itself, which decides what the machine must reserve. */
enum class linux_spdk_memory_t : std::uint8_t {

    /** Hugepages the kernel reserved, which a PCIe controller's DMA needs and the probe checks for. */
    hugepages_k,

    /** Plain pages of a fixed size, which suffice for a fabric the socket copies through, on any machine. */
    plain_pages_k,
};

/** What an environment is initialised with, decided by the transport the mount is for. */
struct linux_spdk_environment_config_t {

    /** Whether the driver gets hugepages and scans PCIe, or plain pages and no bus at all. */
    linux_spdk_memory_t memory = linux_spdk_memory_t::plain_pages_k;

    /** Megabytes of plain pages the driver takes at once, which it refuses to start without. */
    std::uint32_t plain_megabytes = 128;
};

/** What the controller says it is, as identify reports it, with the padding the spec allows trimmed. */
struct linux_spdk_controller_identity_t {

    /** The serial number, which a target's RPC sets and a drive's vendor burns in. */
    char serial[21] = {};

    /** The model number. */
    char model[41] = {};
};

/** What a mount fixes about an SPDK disk before any queue set's queue holds it. */
struct linux_spdk_disk_config_t {

    /** Memory ranges the disk registers at once, every one of which must be hugepage-backed. */
    std::uint16_t registered_memory_limit = 8;
};

#pragma endregion Configuration

#pragma region Environment

/**
 *  @brief The one DPDK environment a process may hold, handed to every disk that needs it.
 *
 *  The driver initialises once per process and cannot initialise again, so the environment is never
 *  finished: the first @c acquire brings it up, every later one shares it, and a handle is only the
 *  proof that it is up.
 *
 *  @note @c probe is the only member that is safe to call before anything is initialised, and it is
 *      the one a mount must call, since @c acquire reaches @c rte_eal_init and that calls @c exit
 *      on the whole process when the machine is not ready for it.
 *
 *  @tparam atomic_reference_ The reference the process's claim goes through.
 *  @tparam waiter_type_ What an @c acquire waiting on another one does between polls of the claim.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_spdk_environment {
  public:

    linux_spdk_environment() noexcept = default;

    /**
     *  @brief Decides whether the environment could be initialised, by reading files and nothing else.
     *  @param[in] address The PCIe address of the device, in either the long or the short form.
     *  @return What stands in the way, or @c available_k when nothing does.
     *
     *  @note The probe reads PCIe addresses alone. A fabric address has no sysfs device and no
     *      hugepage behind it, so it answers @c available_k at once and leaves the refusal to the
     *      connect, which is the only thing that can see the network.
     */
    [[nodiscard]] static linux_spdk_probe_t probe(char const* address) noexcept;

    /**
     *  @brief Brings the environment up, which only a probe answering @c available_k permits, or
     *      shares it once it is up.
     *  @param[in] config Hugepages for a PCIe controller, or plain pages of a size for a fabric.
     *  @return The handle, why the driver would not start, or @c unsupported_k for a config asking
     *      for other pages than the environment runs on.
     */
    static expected<linux_spdk_environment> acquire(linux_spdk_environment_config_t const& config) noexcept;

    /** Whether this handle names the process's environment. */
    [[nodiscard]] bool initialised() const noexcept { return initialised_; }

  private:

    /** Initialises the driver for the first @c acquire of the process. */
    static status_t initialise_(linux_spdk_environment_config_t const& config) noexcept;

    /** Whether the handle came from an @c acquire that found or brought the environment up. */
    bool initialised_ = false;
};

#if USTORE_OS_LINUX_

/** Bytes of @c /proc/meminfo the probe reads, which is more than the file has ever held. */
inline constexpr std::size_t meminfo_bytes_k = 8192;

/** Bytes of a sysfs driver link the probe reads, which is more than such a path has ever spanned. */
inline constexpr std::size_t link_bytes_k = 512;

template <template <typename> typename atomic_reference_, typename waiter_type_>
linux_spdk_probe_t linux_spdk_environment<atomic_reference_, waiter_type_>::probe(char const* address) noexcept {
    if (!address) return linux_spdk_probe_t::not_found_k;
    if (spdk_transport_named(address) == linux_spdk_transport_t::fabric_k) return linux_spdk_probe_t::available_k;
    std::string_view const named = transport_field(address, "traddr:");
    expected<pci_address_t> const parsed = pci_address_parsed(named.empty() ? std::string_view {address} : named);
    if (!parsed) return linux_spdk_probe_t::not_found_k;

    char device_path[64] = {};
    std::snprintf(device_path, sizeof(device_path), "/sys/bus/pci/devices/%s", parsed->text);
    if (!system_path_exists(device_path)) return linux_spdk_probe_t::not_found_k;

    std::array<char, meminfo_bytes_k> meminfo {};
    hugepage_inventory_t const hugepages = hugepages_in_meminfo(read_system_text("/proc/meminfo", meminfo));
    if (hugepages.page_bytes == 0 || hugepages.free_pages == 0) return linux_spdk_probe_t::without_hugepages_k;
    if (!system_path_exists("/sys/kernel/mm/hugepages")) return linux_spdk_probe_t::without_hugepages_k;

    char driver_path[80] = {};
    std::snprintf(driver_path, sizeof(driver_path), "/sys/bus/pci/devices/%s/driver", parsed->text);
    std::array<char, link_bytes_k> link {};
    pci_driver_t const driver = pci_driver_named(read_system_link(driver_path, link));
    if (driver != pci_driver_t::vfio_pci_k && driver != pci_driver_t::uio_generic_k)
        return linux_spdk_probe_t::without_permission_k;
    if (driver == pci_driver_t::vfio_pci_k && !system_path_is_usable("/dev/vfio/vfio"))
        return linux_spdk_probe_t::without_permission_k;
    return linux_spdk_probe_t::available_k;
}

#else

template <template <typename> typename atomic_reference_, typename waiter_type_>
linux_spdk_probe_t linux_spdk_environment<atomic_reference_, waiter_type_>::probe(char const* address) noexcept {
    if (!address) return linux_spdk_probe_t::not_found_k;
    return spdk_transport_named(address) == linux_spdk_transport_t::fabric_k ? linux_spdk_probe_t::available_k
                                                                             : linux_spdk_probe_t::not_found_k;
}

#endif

#if USTORE_WITH_LINUX_SPDK

#pragma region Process

/**
 *  @brief One connection to a controller, which every disk opened on its address shares.
 *
 *  The driver attaches a controller once per process, so a second mount on the same address takes
 *  this one rather than connecting again. Every open disk, every namespace registered and every
 *  queue holding an I/O queue pair on it holds a share, and the last share given back detaches the
 *  controller alone, so its drive may be pulled.
 */
struct linux_spdk_attachment_t {

    /** The address the controller was connected at, which a later open is matched against. */
    spdk_nvme_transport_id transport {};

    /** The controller, null until the first share connects it. */
    spdk_nvme_ctrlr* controller = nullptr;

    /** Disks open, namespaces registered and queues holding an I/O queue pair on the connection,
     *  zero while the record is free. */
    std::uint32_t holders = 0;

    /** Held while a share connects the controller or finds it failed. */
    std::int32_t claim = 0;

    /** Namespaces some disk on the controller registered, which no second disk may register. */
    st::vector<std::uint32_t> claimed;
};

/** Controllers one process may hold connected at once. */
inline constexpr std::size_t linux_spdk_attachments_limit_k = 1024;

/** What DPDK keeps once per process, shared by every instantiation of the environment and the
 *  disk: whether the environment is up and on which pages, and every connection, under claims. */
struct linux_spdk_process_t {

    /** Held while an @c acquire brings the environment up or reads that it is. */
    std::int32_t environment_claim = 0;

    /** Whether the environment is up, set by the first @c acquire and never cleared. */
    bool up = false;

    /** The pages the environment runs on, which every later @c acquire must ask for. */
    linux_spdk_memory_t memory = linux_spdk_memory_t::plain_pages_k;

    /** Held while a share is taken or given back, or a namespace claimed. */
    std::int32_t attachments_claim = 0;

    /** Every connection, in records that never move. */
    st::basic_growing_pool<linux_spdk_attachment_t> attachments;
};

[[nodiscard]] inline linux_spdk_process_t& linux_spdk_process() noexcept {
    // DPDK's state is per process, so its record is one inline object every instantiation shares.
    static linux_spdk_process_t process;
    return process;
}

#pragma endregion Process

template <template <typename> typename atomic_reference_, typename waiter_type_>
expected<linux_spdk_environment<atomic_reference_, waiter_type_>>
linux_spdk_environment<atomic_reference_, waiter_type_>::acquire(
    linux_spdk_environment_config_t const& config) noexcept {
    linux_spdk_process_t& process = linux_spdk_process();
    lock_claim<atomic_reference_, waiter_type_>(process.environment_claim);
    status_t brought = process.up && process.memory != config.memory
                           ? status_t {status_code_t::unsupported_k, "The process's environment runs on other pages"}
                           : status_t {};
    if (!process.up && brought.succeeded()) {
        brought = initialise_(config);
        process.up = brought.succeeded();
        process.memory = config.memory;
    }
    unlock_claim<atomic_reference_>(process.environment_claim);
    if (brought.failed()) return brought;
    linux_spdk_environment environment;
    environment.initialised_ = true;
    return environment;
}

template <template <typename> typename atomic_reference_, typename waiter_type_>
status_t linux_spdk_environment<atomic_reference_, waiter_type_>::initialise_(
    linux_spdk_environment_config_t const& config) noexcept {
    bool const plain = config.memory == linux_spdk_memory_t::plain_pages_k;
    if (plain && config.plain_megabytes == 0)
        return {status_code_t::invalid_argument_k, "Plain pages need a size the driver takes at once"};
    if (!spdk_env_dpdk_external_init())
        return {status_code_t::unsupported_k, "The driver initialises its environment once per process"};

    spdk_env_opts options {};
    options.opts_size = sizeof(options);
    spdk_env_opts_init(&options);
    options.name = "ustore";
    options.no_huge = plain;
    options.no_pci = plain;
    if (plain) options.mem_size = static_cast<int>(config.plain_megabytes);
    if (spdk_env_init(&options) != 0)
        return {status_code_t::device_k, "The driver would not initialise its environment"};
    spdk_unaffinitize_thread();
    return {};
}

#else

template <template <typename> typename atomic_reference_, typename waiter_type_>
expected<linux_spdk_environment<atomic_reference_, waiter_type_>>
linux_spdk_environment<atomic_reference_, waiter_type_>::acquire(linux_spdk_environment_config_t const&) noexcept {
    return status_t {status_code_t::unsupported_k, "This build holds no SPDK"};
}

#endif

#pragma endregion Environment

#pragma region DMA Memory

/** Allocates DMA-able memory from the SPDK environment on one NUMA socket, which PCIe controllers
 *  write into directly, as the arena of a disk that translates addresses must be; a build without
 *  SPDK answers @c nullptr. The bytes come unzeroed, since their first writer is the device. */
template <typename value_type_ = byte_t>
struct linux_spdk_dma_allocator {
    using value_type = value_type_;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::false_type;

    /** The NUMA socket the bytes are placed on, or -1 for whichever the environment picks. */
    int socket = -1;

    constexpr linux_spdk_dma_allocator() noexcept = default;
    constexpr explicit linux_spdk_dma_allocator(int on_socket) noexcept : socket(on_socket) {}

    /** The rebinding conversion @c std::allocator_traits performs. */
    template <typename other_type_>
    constexpr linux_spdk_dma_allocator(linux_spdk_dma_allocator<other_type_> const& other) noexcept
        : socket(other.socket) {}

#if USTORE_WITH_LINUX_SPDK
    value_type_* allocate(std::size_t count) noexcept {
        return static_cast<value_type_*>(
            spdk_dma_malloc_socket(count * sizeof(value_type_), portable_alignment_k, nullptr, socket));
    }
    void deallocate(value_type_* pointer, std::size_t) noexcept { spdk_dma_free(pointer); }
#else
    value_type_* allocate(std::size_t) noexcept { return nullptr; }
    void deallocate(value_type_*, std::size_t) noexcept {}
#endif

    bool operator==(linux_spdk_dma_allocator const& other) const noexcept { return socket == other.socket; }
};

#pragma endregion DMA Memory

#if USTORE_WITH_LINUX_SPDK

#pragma region Attachments

/** Gives one share of @p attachment back, detaching its controller after the last. */
template <template <typename> typename atomic_reference_, typename waiter_type_>
void linux_spdk_detach(linux_spdk_attachment_t& attachment) noexcept {
    linux_spdk_process_t& process = linux_spdk_process();
    lock_claim<atomic_reference_, waiter_type_>(process.attachments_claim);
    spdk_nvme_ctrlr* detached = nullptr;
    if (--attachment.holders == 0) {
        detached = std::exchange(attachment.controller, nullptr);
        attachment.claimed.clear();
        process.attachments.vacate(process.attachments.index_of(attachment));
    }
    unlock_claim<atomic_reference_>(process.attachments_claim);
    if (detached) spdk_nvme_detach(detached);
}

/** A share of the connection at @p transport, which the first share connects with @p options. */
template <template <typename> typename atomic_reference_, typename waiter_type_>
expected<linux_spdk_attachment_t*> linux_spdk_attach(spdk_nvme_transport_id const& transport,
                                                     spdk_nvme_ctrlr_opts const& options) noexcept {
    using records_t = st::basic_growing_pool<linux_spdk_attachment_t>;
    linux_spdk_process_t& process = linux_spdk_process();
    {
        lock_claim<atomic_reference_, waiter_type_>(process.attachments_claim);
        linux_spdk_attachment_t* attached = nullptr;
        status_t refused;
        for (linux_spdk_attachment_t& record : process.attachments.records())
            if (record.holders != 0 && spdk_nvme_transport_id_compare(&record.transport, &transport) == 0) {
                attached = &record;
                break;
            }
        if (!attached && process.attachments.capacity_limit() == 0) {
            auto [records, make_status] = records_t::make(1, linux_spdk_attachments_limit_k);
            if (make_status != st::status_t::success_k)
                refused = {status_code_t::out_of_memory_k, "No room to record the controllers"};
            else process.attachments = std::move(records);
        }
        if (!attached && refused.succeeded()) {
            std::uint32_t const index = process.attachments.take();
            if (index == records_t::absent_k)
                refused = {status_code_t::capacity_exhausted_k, "The process holds as many controllers as it may"};
            else {
                attached = &process.attachments[index];
                attached->transport = transport;
            }
        }
        if (attached) ++attached->holders;
        unlock_claim<atomic_reference_>(process.attachments_claim);
        if (refused.failed()) return refused;

        // `spdk_nvme_connect` hands back a cached failed controller, so a pulled or reset drive is
        // refused until its last holder detaches it, and only then can a connect reach a new one.
        lock_claim<atomic_reference_, waiter_type_>(attached->claim);
        bool const failed = attached->controller && spdk_nvme_ctrlr_is_failed(attached->controller);
        if (!failed && !attached->controller)
            attached->controller = spdk_nvme_connect(&transport, &options, sizeof(options));
        bool const connected = attached->controller != nullptr;
        unlock_claim<atomic_reference_>(attached->claim);
        if (!failed && connected) return attached;
        linux_spdk_detach<atomic_reference_, waiter_type_>(*attached);
        if (failed) return status_t {status_code_t::io_k, "The controller failed and its holders have not let go"};
        return status_t {status_code_t::not_found_k, "No controller answered at that address"};
    }
}

/** Claims namespace @p namespace_id of @p attachment for one disk while no other holds it, which
 *  holds a share of the connection until it is given back. */
template <template <typename> typename atomic_reference_, typename waiter_type_>
status_t linux_spdk_claim_namespace(linux_spdk_attachment_t& attachment, std::uint32_t namespace_id) noexcept {
    linux_spdk_process_t& process = linux_spdk_process();
    lock_claim<atomic_reference_, waiter_type_>(process.attachments_claim);
    status_t claimed;
    for (std::uint32_t const held : attachment.claimed)
        if (held == namespace_id) claimed = {status_code_t::contended_k, "Another mount registered that namespace"};
    if (claimed.succeeded() && attachment.claimed.push_back(std::uint32_t {namespace_id}) != st::status_t::success_k)
        claimed = {status_code_t::out_of_memory_k, "No room to claim another namespace"};
    if (claimed.succeeded()) ++attachment.holders;
    unlock_claim<atomic_reference_>(process.attachments_claim);
    return claimed;
}

/** Gives namespace @p namespace_id of @p attachment back, and the share it held. */
template <template <typename> typename atomic_reference_, typename waiter_type_>
void linux_spdk_release_namespace(linux_spdk_attachment_t& attachment, std::uint32_t namespace_id) noexcept {
    linux_spdk_process_t& process = linux_spdk_process();
    lock_claim<atomic_reference_, waiter_type_>(process.attachments_claim);
    for (std::size_t index = 0; index != attachment.claimed.size(); ++index)
        if (attachment.claimed[index] == namespace_id) {
            attachment.claimed.erase(index);
            break;
        }
    unlock_claim<atomic_reference_>(process.attachments_claim);
    linux_spdk_detach<atomic_reference_, waiter_type_>(attachment);
}

#pragma endregion Attachments

#pragma region Queue

template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_spdk_disk;

/**
 *  @brief The I/O queue pairs one thread owns, one per controller it held a disk of, with
 *      the credit, the commands the device holds, the chained successors held back and the
 *      completions the disk answered itself.
 *
 *  The first @c register_media of a disk on a controller allocates the pair and every later disk on
 *  that controller finds it, so a queue set takes at most one of the controller's queue pairs per
 *  queue. Each pair holds a share of its controller's attachment, so the controller stays attached
 *  while the queue lives, even past the last disk's release.
 *
 *  The thread's blocks come from @c linux_spdk_dma_allocator, so they sit in the driver's map
 *  already; a controller reached by DMA checks that they translate when the queue first registers
 *  one of its disks.
 */
class linux_spdk_queue_t {
  public:

    linux_spdk_queue_t() noexcept = default;
    linux_spdk_queue_t(linux_spdk_queue_t const&) = delete;
    linux_spdk_queue_t& operator=(linux_spdk_queue_t const&) = delete;

    linux_spdk_queue_t(linux_spdk_queue_t&& other) noexcept
        : submitted_requests_(std::move(other.submitted_requests_)), staged_(std::move(other.staged_)),
          completions_made_by_queue_(std::move(other.completions_made_by_queue_)), pairs_(std::move(other.pairs_)),
          pinned_staging_(other.pinned_staging_), media_controller_(std::exchange(other.media_controller_, nullptr)),
          media_pair_(std::exchange(other.media_pair_, nullptr)), depth_(other.depth_),
          outstanding_(other.outstanding_), first_free_submitted_request_(other.first_free_submitted_request_),
          staged_count_(other.staged_count_), staged_free_(other.staged_free_),
          completions_made_by_queue_first_(other.completions_made_by_queue_first_),
          completions_made_by_queue_count_(other.completions_made_by_queue_count_), idle_polls_(other.idle_polls_) {}

    linux_spdk_queue_t& operator=(linux_spdk_queue_t&& other) noexcept {
        if (this == &other) return *this;
        teardown_();
        submitted_requests_ = std::move(other.submitted_requests_);
        staged_ = std::move(other.staged_);
        completions_made_by_queue_ = std::move(other.completions_made_by_queue_);
        pairs_ = std::move(other.pairs_);
        pinned_staging_ = other.pinned_staging_;
        media_controller_ = std::exchange(other.media_controller_, nullptr);
        media_pair_ = std::exchange(other.media_pair_, nullptr);
        depth_ = other.depth_;
        outstanding_ = other.outstanding_;
        first_free_submitted_request_ = other.first_free_submitted_request_;
        staged_count_ = other.staged_count_;
        staged_free_ = other.staged_free_;
        completions_made_by_queue_first_ = other.completions_made_by_queue_first_;
        completions_made_by_queue_count_ = other.completions_made_by_queue_count_;
        idle_polls_ = other.idle_polls_;
        return *this;
    }

    ~linux_spdk_queue_t() noexcept { teardown_(); }

    /**
     *  @brief Sizes the windows of a queue of @p depth requests over @p range, with no pair yet.
     *  @param[in] depth Requests the queue keeps in flight at once.
     *  @param[in] range The thread's DMA blocks, which @c queue_range_k addresses, or empty.
     */
    static expected<linux_spdk_queue_t> make(queue_depth_t depth, std::span<byte_t> range) noexcept {
        if (depth == 0 || depth > depth_ceiling_k)
            return status_t {status_code_t::invalid_argument_k, "An SPDK queue holds between one and 16384 requests"};
        linux_spdk_queue_t queue;
        if (queue.submitted_requests_.resize(depth) != st::status_t::success_k ||
            queue.staged_.resize(depth) != st::status_t::success_k ||
            queue.completions_made_by_queue_.resize(depth) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "An SPDK queue could not size its windows"};
        queue.depth_ = depth;
        queue.pinned_staging_ = range;
        for (std::uint32_t index = 0; index != depth; ++index) {
            queue.submitted_requests_[index].next_free = index + 1 == depth ? no_entry_k : index + 1;
            queue.staged_[index].next_free = index + 1 == depth ? no_entry_k : index + 1;
        }
        queue.first_free_submitted_request_ = 0;
        queue.staged_free_ = 0;
        return queue;
    }

    /** Requests the queue keeps in flight at once. */
    [[nodiscard]] queue_depth_t depth() const noexcept { return depth_; }

  private:

    template <template <typename> typename atomic_reference_, typename waiter_type_>
    friend class linux_spdk_disk;

    /** Whether a submitted slot is free, holds a command the device holds, or holds its result. */
    enum class submitted_request_state_t : std::uint8_t {

        /** Nothing occupies the slot, which is on the queue's free list. */
        vacant_k,

        /** The device holds the command and has not answered for it. */
        outstanding_k,

        /** The completion callback has written the result, which the next poll copies out. */
        finished_k,
    };

    /** The index that names no slot, in a free list and in a chain alike. */
    static constexpr std::uint32_t no_entry_k = 0xFFFFFFFFu;

    /** Requests one queue keeps in flight at most, which bounds the depth a queue set
     *  may ask for. */
    static constexpr queue_depth_t depth_ceiling_k = 16384;

    /** One command the device holds, and the result its completion callback writes into it. */
    struct submitted_request_t {

        /** The token the queue minted, which the completion holds back unchanged. */
        request_token_t token;

        /** Bytes the extent asked for, which an NVMe command moves whole or not at all. */
        std::uint64_t length = 0;

        /** Bytes the device moved, which the callback fills in. */
        std::uint64_t transferred_bytes = 0;

        /** Why the command ended this way, which the callback fills in. */
        status_t status;

        /** The range the request viewed, given back when its completion is completed. */
        memory_reference_t buffer;

        /** The target the request viewed, given back when its completion is completed. */
        medium_reference_t medium;

        /** The staged request waiting on this one, or @c no_entry_k when none does. */
        std::uint32_t successor = no_entry_k;

        /** The next free slot while this one is vacant, which is how the free list is linked. */
        std::uint32_t next_free = no_entry_k;

        /** How the command ended, which the callback fills in. */
        completion_state_t outcome = completion_state_t::completed_k;

        /** Whether the slot is free, holds a command, or holds its result. */
        submitted_request_state_t state = submitted_request_state_t::vacant_k;

        /** The command's opcode, which a command-specific status is spelled by. */
        std::uint8_t opcode = 0;
    };

    /** One successor of a chained request, held back until its predecessor transfers in full. */
    struct staged_t {

        /** The request itself, issued unchanged when the chain reaches it. */
        request_t request;

        /** The staged request waiting on this one, or @c no_entry_k when none does. */
        std::uint32_t next = no_entry_k;

        /** The next free slot while this one is vacant, which is how the free list is linked. */
        std::uint32_t next_free = no_entry_k;
    };

    /** One controller's I/O queue pair, and the share of its attachment that keeps it connected. */
    struct pair_t {

        /** The controller the pair was allocated on, which a later hold looks it up by. */
        spdk_nvme_ctrlr* controller = nullptr;

        /** The pair itself, which only the thread owning the queue ever polls. */
        spdk_nvme_qpair* queue = nullptr;

        /** The connection whose share the pair holds. */
        linux_spdk_attachment_t* attachment = nullptr;

        /** Gives the share back through the claims of the disk that allocated the pair. */
        void (*detach)(linux_spdk_attachment_t&) noexcept = nullptr;
    };

    /** Frees every pair, then gives back each pair's share, which may detach its controller. */
    void teardown_() noexcept {
        // An abort the queue issued lands on the admin queue, so it is polled before the pair goes
        for (pair_t& pair : pairs_) {
            [[maybe_unused]] std::int32_t const polled = spdk_nvme_ctrlr_process_admin_completions(pair.controller);
            spdk_nvme_ctrlr_free_io_qpair(pair.queue);
        }
        for (pair_t& pair : pairs_) pair.detach(*pair.attachment);
        pairs_.clear();
        media_controller_ = nullptr;
        media_pair_ = nullptr;
    }

    /** The commands the device holds, addressed through a free list. */
    st::vector<submitted_request_t> submitted_requests_;

    /** The held successors of chained requests, addressed through a free list. */
    st::vector<staged_t> staged_;

    /** The completions the disk answered itself, read as a circle. */
    st::vector<completion_t> completions_made_by_queue_;

    /** One pair per controller the queue registered a disk of. */
    st::vector<pair_t> pairs_;

    /** The thread's DMA blocks, which a request naming @c queue_range_k addresses. */
    std::span<byte_t> pinned_staging_;

    /** The controller of the disk registered last, whose pair every submission goes to. */
    spdk_nvme_ctrlr* media_controller_ = nullptr;

    /** The pair on @c media_controller_. */
    spdk_nvme_qpair* media_pair_ = nullptr;

    /** Requests the queue keeps in flight at once. */
    queue_depth_t depth_ = 0;

    /** Commands the device holds for this queue, plus the results not yet copied out. */
    std::uint32_t outstanding_ = 0;

    /** Head of the free submitted slots, @c no_entry_k when every one is taken. */
    std::uint32_t first_free_submitted_request_ = no_entry_k;

    /** Successors of chained requests the queue is holding back. */
    std::uint32_t staged_count_ = 0;

    /** Head of the free staging slots, @c no_entry_k when every one is taken. */
    std::uint32_t staged_free_ = no_entry_k;

    /** Where the oldest unreaped local completion sits inside @c completions_made_by_queue_. */
    std::uint32_t completions_made_by_queue_first_ = 0;

    /** Completions the disk answered itself, which no command ever held. */
    std::uint32_t completions_made_by_queue_count_ = 0;

    /** Reaps since the admin queue was last polled that found nothing on the pair. */
    std::uint32_t idle_polls_ = 0;
};

#pragma endregion Queue

#pragma region Disk

/**
 *  @brief The ring SPDK offers, over the namespaces of the controller the mount connected to.
 *
 *  Every per-request defect the ring reports as a failed completion is checked here rather than by
 *  the driver, which validates almost nothing: a misalignment, an oversized transfer, a stale
 *  registered memory and an extent past the namespace all become that one request's @c failed_k and
 *  leave its neighbours alone. A short transfer never arises, since an NVMe command moves its whole
 *  extent or none of it, which is why an extent past the namespace is refused rather than clipped.
 *
 *  Chaining is paid for in software. A successor of a chained request is held in this disk's own
 *  staging and issued when its predecessor completes whole; anything short of whole cuts the rest
 *  of the chain with @c cancelled_k, which is the semantics a linked submission would have given.
 *
 *  Nothing here runs a timer, so the connect disables the keep-alive a fabric would otherwise
 *  expect, and the admin queue is polled on idle polls, which is where an abort's own completion
 *  lands. What a registered memory must prove depends on the transport: a controller reached by DMA
 *  needs every page in the driver's map, while one reached over a socket takes any host memory.
 *
 *  @tparam atomic_reference_ The reference the claim and in-submitted words are owned through.
 *  @tparam waiter_type_ What a draining core does after a poll that moved nothing.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_spdk_disk {
  public:

    /** The queue a thread owns for this transport. */
    using queue_t = linux_spdk_queue_t;

    linux_spdk_disk() noexcept = default;
    linux_spdk_disk(linux_spdk_disk const&) = delete;
    linux_spdk_disk& operator=(linux_spdk_disk const&) = delete;

    linux_spdk_disk(linux_spdk_disk&& other) noexcept
        : environment_(std::move(other.environment_)), registered_memories_(std::move(other.registered_memories_)),
          media_(std::move(other.media_)), attachment_(std::exchange(other.attachment_, nullptr)),
          controller_(std::exchange(other.controller_, nullptr)), sector_bytes_(std::exchange(other.sector_bytes_, 0)),
          max_transfer_bytes_(std::exchange(other.max_transfer_bytes_, 0)),
          queue_pairs_limit_(std::exchange(other.queue_pairs_limit_, 0)),
          registered_memories_limit_(std::exchange(other.registered_memories_limit_, 0)), reach_(other.reach_) {}

    linux_spdk_disk& operator=(linux_spdk_disk&& other) noexcept {
        if (this == &other) return *this;
        assert(!attachment_ && "release a disk before another takes its place");
        environment_ = std::move(other.environment_);
        registered_memories_ = std::move(other.registered_memories_);
        media_ = std::move(other.media_);
        attachment_ = std::exchange(other.attachment_, nullptr);
        controller_ = std::exchange(other.controller_, nullptr);
        sector_bytes_ = std::exchange(other.sector_bytes_, 0);
        max_transfer_bytes_ = std::exchange(other.max_transfer_bytes_, 0);
        queue_pairs_limit_ = std::exchange(other.queue_pairs_limit_, 0);
        registered_memories_limit_ = std::exchange(other.registered_memories_limit_, 0);
        reach_ = other.reach_;
        return *this;
    }

    ~linux_spdk_disk() noexcept { assert(!attachment_ && "release a disk before it dies"); }

    /**
     *  @brief Connects @p address, or shares the process's connection to it, and sizes the disk's
     *      bookkeeping by the controller's first active namespace.
     *  @param[in] environment A counted share of an initialised environment, kept for the disk's life.
     *  @param[in] address The PCIe address of the controller, or a fabric transport ID the driver parses.
     *  @param[in] config The ranges registered at once.
     *  @return The mounted disk, or why the controller would not answer.
     */
    static expected<linux_spdk_disk> open(linux_spdk_environment<atomic_reference_, waiter_type_> environment,
                                          char const* address, linux_spdk_disk_config_t const& config) noexcept {
        if (status_t const sane = inspect_config_(config); sane.failed()) return sane;
        if (!address) return status_t {status_code_t::invalid_argument_k, "A controller needs an address"};
        if (!environment.initialised())
            return status_t {status_code_t::invalid_argument_k, "A mount needs an initialised environment"};

        spdk_nvme_transport_id transport {};
        if (status_t const named = transport_of_(address, transport); named.failed()) return named;

        // Every disk on this controller takes queue pairs from one connection, which asks for all.
        spdk_nvme_ctrlr_opts options {};
        spdk_nvme_ctrlr_get_default_ctrlr_opts(&options, sizeof(options));
        options.keep_alive_timeout_ms = 0;
        auto [attachment, attach_status] = linux_spdk_attach<atomic_reference_, waiter_type_>(transport, options);
        if (attach_status.failed()) return attach_status;
        spdk_nvme_ctrlr* const controller = attachment->controller;

        spdk_nvme_ns* chosen = nullptr;
        for (std::uint32_t identifier = spdk_nvme_ctrlr_get_first_active_ns(controller); identifier != 0;
             identifier = spdk_nvme_ctrlr_get_next_active_ns(controller, identifier)) {
            spdk_nvme_ns* const candidate = spdk_nvme_ctrlr_get_ns(controller, identifier);
            if (!candidate || !spdk_nvme_ns_is_active(candidate)) continue;
            chosen = candidate;
            break;
        }
        if (!chosen) {
            linux_spdk_detach<atomic_reference_, waiter_type_>(*attachment);
            return status_t {status_code_t::not_found_k, "The controller holds no active namespace"};
        }

        spdk_nvme_ctrlr_opts const* const controller_options = spdk_nvme_ctrlr_get_opts(controller);
        std::uint32_t const queues = controller_options && controller_options->num_io_queues != 0
                                         ? controller_options->num_io_queues
                                         : 1u;

        linux_spdk_disk disk;
        disk.environment_ = std::move(environment);
        disk.attachment_ = attachment;
        disk.controller_ = controller;
        disk.sector_bytes_ = spdk_nvme_ns_get_sector_size(chosen);
        disk.max_transfer_bytes_ = spdk_nvme_ns_get_max_io_xfer_size(chosen);
        // NVMe numbers its queues in 16 bits, so the count always fits
        disk.queue_pairs_limit_ = static_cast<std::uint16_t>(queues);
        disk.registered_memories_limit_ = config.registered_memory_limit;
        disk.reach_ = transport.trtype == SPDK_NVME_TRANSPORT_TCP ? reach_t::copied_k : reach_t::translated_k;
        status_t sized;
        if (disk.sector_bytes_ == 0 || !std::has_single_bit(disk.sector_bytes_))
            sized = {status_code_t::unsupported_k, "The namespace reports no power-of-two sector"};
        if (sized.succeeded()) sized = size_(disk.registered_memories_, config.registered_memory_limit);
        if (sized.succeeded()) sized = size_(disk.media_, targets_limit_k);
        if (sized.failed()) {
            disk.release();
            return sized;
        }
        return disk;
    }

    /** Whether the arena this disk registers must come from @c linux_spdk_dma_allocator: a PCIe
     *  controller DMAs to translated addresses, while a socket transport copies from any memory. */
    [[nodiscard]] bool needs_dma_memory() const noexcept { return reach_ == reach_t::translated_k; }

    /** The NUMA node of the controller's PCI device, which the mount places its arena on, or -1
     *  over a fabric. */
    [[nodiscard]] int memory_domain() const noexcept {
        spdk_pci_device* const device = controller_ ? spdk_nvme_ctrlr_get_pci_device(controller_) : nullptr;
        return device ? spdk_pci_device_get_socket_id(device) : -1;
    }

    /** What this disk accepts, which the namespace's sector size and the controller's limits fix. */
    [[nodiscard]] disk_capabilities_t capabilities() const noexcept {
        disk_capabilities_t reported;
        reported.memory_alignment = portable_alignment_k;
        reported.offset_alignment = sector_bytes_;
        reported.length_granularity = sector_bytes_;
        reported.max_transfer_bytes = max_transfer_bytes_;
        reported.depth_limit = linux_spdk_queue_t::depth_ceiling_k;
        reported.queue_pairs_limit = queue_pairs_limit_;
        reported.media_limit = targets_limit_k;
        reported.reaches_gpu_memory = false;
        return reported;
    }

    /** What the controller identifies as, which a suite checks against the serial its rig set. */
    [[nodiscard]] linux_spdk_controller_identity_t identity() const noexcept {
        linux_spdk_controller_identity_t identity;
        spdk_nvme_ctrlr_data const* const data = spdk_nvme_ctrlr_get_data(controller_);
        if (!data) return identity;
        copy_trimmed_(data->sn, sizeof(data->sn), identity.serial);
        copy_trimmed_(data->mn, sizeof(data->mn), identity.model);
        return identity;
    }

#pragma region Queues

    /**
     *  @brief Points @p queue_pair's queue at its I/O queue pair on this disk's
     *      controller, allocating it on the queue's first registration of a disk there;
     *      one comparison once warm.
     *  @param[in] queue_pair The queue the calling thread owns, drained before it moves on.
     *  @return @c invalid_argument_k for another transport's queue, for one still holding
     *      requests on another controller, or for blocks a DMA controller cannot translate;
     *      @c unsupported_k for a blocking queue; @c capacity_exhausted_k once the controller
     *      granted every queue pair it has.
     *
     *  @note The first pair on a controller takes a share of its attachment, which the queue
     *      gives back when it dies, so the controller outlives the release of every disk on it.
     */
    status_t register_media(queue_ref_t const& queue_pair) noexcept {
        auto [queue, queue_status] = queue_of_(queue_pair);
        if (queue_status.failed()) return queue_status;
        if (queue->media_controller_ == controller_) return {};
        // The old pair's flights would never be polled again once submissions go to another pair
        if (outstanding_requests_(*queue) != 0)
            return {status_code_t::invalid_argument_k, "Drain the queue before it holds another controller"};
        for (linux_spdk_queue_t::pair_t& pair : queue->pairs_) {
            if (pair.controller != controller_) continue;
            queue->media_controller_ = controller_;
            queue->media_pair_ = pair.queue;
            return {};
        }

        if (reach_ == reach_t::translated_k && !queue->pinned_staging_.empty())
            if (status_t const reachable = inspect_translation_(queue->pinned_staging_); reachable.failed())
                return reachable;
        spdk_nvme_io_qpair_opts options {};
        spdk_nvme_ctrlr_get_default_io_qpair_opts(controller_, &options, sizeof(options));
        options.io_queue_size = queue->depth_ + 1;
        options.io_queue_requests = queue->depth_ * 2;
        spdk_nvme_qpair* const allocated = spdk_nvme_ctrlr_alloc_io_qpair(controller_, &options, sizeof(options));
        if (!allocated)
            return {status_code_t::capacity_exhausted_k, "The controller would not give another queue pair"};
        linux_spdk_queue_t::pair_t pair;
        pair.controller = controller_;
        pair.queue = allocated;
        pair.attachment = attachment_;
        pair.detach = &linux_spdk_detach<atomic_reference_, waiter_type_>;
        if (queue->pairs_.push_back(std::move(pair)) != st::status_t::success_k) {
            spdk_nvme_ctrlr_free_io_qpair(allocated);
            return {status_code_t::out_of_memory_k, "The queue could not record another queue pair"};
        }
        linux_spdk_process_t& process = linux_spdk_process();
        lock_claim<atomic_reference_, waiter_type_>(process.attachments_claim);
        ++attachment_->holders;
        unlock_claim<atomic_reference_>(process.attachments_claim);
        queue->media_controller_ = controller_;
        queue->media_pair_ = allocated;
        return {};
    }

    /** Requests @p queue_pair has submitted and not yet completed, staged successors included. */
    [[nodiscard]] std::size_t outstanding(queue_ref_t const& queue_pair) const noexcept {
        linux_spdk_queue_t const* const queue = queue_pair.template queue<linux_spdk_queue_t>();
        return queue ? outstanding_requests_(*queue) : 0;
    }

#pragma endregion Queues

#pragma region Registrations

    /**
     *  @brief Makes @p chunk addressable by requests, refusing what the transport cannot reach.
     *  @param[in] chunk Memory the caller owns and keeps alive until it deregisters the range.
     *  @param[in] space Where that memory lives; this transport reaches host memory alone.
     *
     *  @note A range @c spdk_vtophys cannot translate is refused here rather than at submission,
     *      where it would become a DMA to an address the device was never told about; a socket
     *      transport translates nothing and takes the range as it is.
     */
    expected<registered_memory_t> register_memory(std::span<byte_t> chunk, memory_space_t space) noexcept {
        if (chunk.empty()) return status_t {status_code_t::invalid_argument_k, "An empty range registers nothing"};
        if (space == memory_space_t::gpu_k)
            return status_t {status_code_t::unsupported_k, "This transport cannot address device memory"};
        if (!is_address_aligned(chunk.data(), portable_alignment_k))
            return status_t {status_code_t::invalid_argument_k, "A DMA range starts on a page boundary"};
        for (std::size_t index = 0; index != registered_memories_.size(); ++index) {
            registered_memory_slot_t& slot = registered_memories_[index];
            std::int32_t expected_free = 0;
            if (!claim_(slot.registered)
                     .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            if (status_t const mapped = map_(slot, chunk); mapped.failed()) {
                claim_(slot.registered).store(0, std::memory_order_release);
                return mapped;
            }
            slot.base = chunk.data();
            slot.length = chunk.size();
            slot.space = space;
            return registered_memory_t {disk_handle_t {static_cast<std::uint16_t>(index), slot.generation}, chunk,
                                        space};
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every registered_memory slot of this ring is taken"};
    }

    /**
     *  @brief Gives @p registered memory back, refusing while it is still a target of requests in flight.
     *  @param[in,out] registered_memory The range to give back, surrendered on success.
     */
    status_t unregister_memory(registered_memory_t& registered_memory) noexcept {
        registered_memory_slot_t* const slot = slot_of_(registered_memory);
        if (!slot) return {status_code_t::invalid_argument_k, "The registered_memory is not this ring's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "The range is still a target of requests in flight"};
        unmap_(*slot);
        ++slot->generation;
        slot->base = nullptr;
        slot->length = 0;
        claim_(slot->registered).store(0, std::memory_order_release);
        registered_memory.surrender();
        return {};
    }

    /**
     *  @brief Registers the namespace @p request names by its number, which no other disk of the
     *      process may hold registered meanwhile.
     *  @param[in] request The namespace's number in decimal as the name, and the access; a
     *      namespace cannot honour a length.
     *  @return The handle, @c contended_k while another disk holds the namespace, @c unsupported_k
     *      for a namespace of another sector or a shorter transfer than the first one's, or
     *      @c capacity_exhausted_k when this disk holds as many namespaces as it may.
     */
    expected<registered_medium_t> register_medium(medium_options_t const& request) noexcept {
        std::string_view const named = request.name ? std::string_view {request.name} : std::string_view {};
        std::uint32_t namespace_id = 0;
        std::from_chars_result const read = std::from_chars(named.data(), named.data() + named.size(), namespace_id);
        if (named.empty() || read.ec != std::errc {} || read.ptr != named.data() + named.size() || namespace_id == 0)
            return status_t {status_code_t::invalid_argument_k, "A namespace is named by its number"};
        spdk_nvme_ns* const chosen = spdk_nvme_ctrlr_get_ns(controller_, namespace_id);
        if (!chosen || !spdk_nvme_ns_is_active(chosen))
            return status_t {status_code_t::not_found_k, "The controller holds no such active namespace"};
        if (spdk_nvme_ns_get_sector_size(chosen) != sector_bytes_ ||
            spdk_nvme_ns_get_max_io_xfer_size(chosen) < max_transfer_bytes_)
            return status_t {status_code_t::unsupported_k, "The namespace's sector or transfer differs from the first"};
        for (std::size_t index = 0; index != media_.size(); ++index) {
            registered_medium_slot_t& slot = media_[index];
            std::int32_t expected_free = 0;
            if (!claim_(slot.registered)
                     .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            if (status_t const claimed = linux_spdk_claim_namespace<atomic_reference_, waiter_type_>(*attachment_,
                                                                                                     namespace_id);
                claimed.failed()) {
                claim_(slot.registered).store(0, std::memory_order_release);
                return claimed;
            }
            slot.access = request.access == medium_access_t::read_only_k ? medium_access_t::read_only_k
                                                                         : medium_access_t::read_write_k;
            slot.capacity = spdk_nvme_ns_get_size(chosen);
            slot.space = chosen;
            slot.namespace_id = namespace_id;
            return registered_medium_t {disk_handle_t {static_cast<std::uint16_t>(index), slot.generation},
                                        slot.capacity, sector_bytes_};
        }
        return status_t {status_code_t::capacity_exhausted_k, "This disk holds as many namespaces as it may"};
    }

    /**
     *  @brief Gives the namespace back, refusing while requests against it are still in flight.
     *  @param[in,out] descriptor The target to give back, surrendered on success.
     */
    status_t unregister_medium(registered_medium_t& descriptor) noexcept {
        registered_medium_slot_t* const slot = slot_of_(descriptor);
        if (!slot) return {status_code_t::invalid_argument_k, "The target is not this ring's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "The target is still named by requests in flight"};
        linux_spdk_release_namespace<atomic_reference_, waiter_type_>(*attachment_, slot->namespace_id);
        ++slot->generation;
        slot->capacity = 0;
        slot->space = nullptr;
        claim_(slot->registered).store(0, std::memory_order_release);
        descriptor.surrender();
        return {};
    }

#pragma endregion Registrations

#pragma region Ring

    /**
     *  @brief Takes as much of @p requests as @p queue_pair still has credit for, and never waits.
     *  @param[in] queue_pair The queue submitting, owned by the calling thread, holding this disk.
     *  @param[in] requests The batch, of which a prefix is taken.
     *  @return How many were taken, which is zero on a full queue and never a reason to block.
     */
    expected<std::size_t> submit(queue_ref_t const& queue_pair, std::span<request_t const> requests) noexcept {
        for (request_t const& request : requests)
            if (request.token.queue_pair() != queue_pair.id())
                return status_t {status_code_t::invalid_argument_k, "A request holds another queue pair's token"};
        auto [queue, queue_status] = queue_of_(queue_pair);
        if (queue_status.failed()) return queue_status;
        assert(queue->media_controller_ == controller_ && "Hold the disk into the queue before submitting");

        std::size_t const credit = queue->depth_ - outstanding_requests_(*queue);
        std::size_t const room = requests.size() < credit ? requests.size() : credit;

        std::uint32_t chained_request = no_entry_k;
        std::uint32_t chain_staged = no_entry_k;
        chaining_state_t chaining = chaining_state_t::open_k;
        std::size_t taken = 0;
        for (; taken != room; ++taken) {
            request_t const& request = requests[taken];
            if (chaining == chaining_state_t::cut_k) {
                push_completion_made_by_queue_(*queue, cancelled_completion_(request.token));
                chaining = chains_next_(request) ? chaining_state_t::cut_k : chaining_state_t::open_k;
                continue;
            }
            if (chaining == chaining_state_t::held_k) {
                auto [entry, hold_status] = hold_(*queue, request);
                if (hold_status.failed()) {
                    push_completion_made_by_queue_(*queue, failed_completion_(request.token, hold_status));
                    chaining = chains_next_(request) ? chaining_state_t::cut_k : chaining_state_t::open_k;
                    continue;
                }
                if (chain_staged == no_entry_k) queue->submitted_requests_[chained_request].successor = entry;
                else queue->staged_[chain_staged].next = entry;
                chain_staged = entry;
                chaining = chains_next_(request) ? chaining_state_t::held_k : chaining_state_t::open_k;
                continue;
            }
            status_t const defect = inspect_(queue->pinned_staging_, request);
            if (defect.failed()) {
                push_completion_made_by_queue_(*queue, failed_completion_(request.token, defect));
                chaining = chains_next_(request) ? chaining_state_t::cut_k : chaining_state_t::open_k;
                continue;
            }
            auto [started, start_status] = start_(*queue, request);
            if (start_status.failed()) {
                if (start_status == status_code_t::capacity_exhausted_k) break;
                push_completion_made_by_queue_(*queue, failed_completion_(request.token, start_status));
                chaining = chains_next_(request) ? chaining_state_t::cut_k : chaining_state_t::open_k;
                continue;
            }
            chained_request = started;
            chain_staged = no_entry_k;
            chaining = chains_next_(request) ? chaining_state_t::held_k : chaining_state_t::open_k;
        }
        return taken;
    }

    /**
     *  @brief Polls @p queue_pair's I/O queue pair and copies what finished into @p into.
     *  @param[in] queue_pair The queue polling, owned by the calling thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero when nothing has landed.
     *
     *  @note Polling is the only mechanism the driver has, which is why nothing here waits and why
     *      a bounded wait is a spin over this call rather than a blocking one.
     */
    expected<std::size_t> poll_completions(queue_ref_t const& queue_pair, std::span<completion_t> into) noexcept {
        auto [queue, queue_status] = queue_of_(queue_pair);
        if (queue_status.failed()) return queue_status;
        if (into.empty()) return std::size_t {0};
        if (queue->outstanding_ != 0) {
            std::int32_t const polled = spdk_nvme_qpair_process_completions(queue->media_pair_,
                                                                            static_cast<std::uint32_t>(into.size()));
            if (polled < 0) return status_t {status_code_t::device_k, "The queue pair failed while polling"};
            if (polled == 0) poll_admin_when_idle_(*queue);
        }
        std::size_t moved = poll_completions_(*queue, into);
        moved += drain_completions_made_by_queue_(*queue, into.subspan(moved));
        return moved;
    }

    /**
     *  @brief Asks the controller to abort what @p queue_pair has in flight, which it may refuse.
     *  @param[in] queue_pair The queue to stop, whose started commands still run to their own end.
     *
     *  @note A refused abort is not an error here: the requests finish normally and @c drain polls
     *      until they do, so a queue still sees exactly one completion per request it submitted.
     */
    void cancel(queue_ref_t const& queue_pair) noexcept {
        linux_spdk_queue_t* const queue = queue_pair.template queue<linux_spdk_queue_t>();
        if (!queue || queue->outstanding_ == 0) return;
        for (std::uint32_t index = 0; index != queue->depth_; ++index) {
            submitted_request_t& submitted = queue->submitted_requests_[index];
            if (submitted.state != submitted_request_state_t::outstanding_k) continue;
            [[maybe_unused]] int const asked = spdk_nvme_ctrlr_cmd_abort_ext(
                controller_, queue->media_pair_, &submitted, &linux_spdk_disk::aborted_, nullptr);
        }
    }

    /**
     *  @brief Finishes and discards what @p queue_pair holds, so its buffers may be freed.
     *  @param[in] queue_pair The queue to empty, which may be cancelled or still running.
     */
    status_t drain(queue_ref_t const& queue_pair) noexcept {
        auto [queue, queue_status] = queue_of_(queue_pair);
        if (queue_status.failed()) return queue_status;
        std::array<completion_t, drain_batch_k> discarded {};
        for (std::size_t spins = 0; spins != drain_spin_limit_k; ++spins) {
            if (outstanding_requests_(*queue) == 0) return {};
            auto [moved, poll_completions_status] = poll_completions(queue_pair, std::span<completion_t> {discarded});
            if (poll_completions_status.failed()) return poll_completions_status;
            if (moved != 0) continue;
            if (spins == drain_patience_k) cancel(queue_pair);
            waiter_type_ {}(&spins, spins, std::size_t {0});
        }
        return {status_code_t::io_k, "The queue would not drain"};
    }

#pragma endregion Ring

  private:

    using submitted_request_t = linux_spdk_queue_t::submitted_request_t;
    using staged_t = linux_spdk_queue_t::staged_t;
    using submitted_request_state_t = linux_spdk_queue_t::submitted_request_state_t;

    /** How the controller reaches a registered range, which decides what registered memory must prove. */
    enum class reach_t : std::uint8_t {

        /** By DMA, so every page of a range must translate in the driver's map before it is a target. */
        translated_k,

        /** Through a socket the driver copies into, so any page of host memory is a target. */
        copied_k,
    };

    /** How the driver came to know a registered range, which decides what deregistering undoes. */
    enum class mapping_t : std::uint8_t {

        /** Recorded here alone, since the transport copies through a socket and translates nothing. */
        recorded_k,

        /** Already in the driver's map when it arrived, as memory the environment allocated is. */
        mapped_k,

        /** Put into the driver's map by this disk, and taken out of it when the range is given back. */
        registered_k,
    };

    /** Where one submitted batch stands with respect to the chain it is walking. */
    enum class chaining_state_t : std::uint8_t {

        /** No chain is open, so the next request is issued on its own merits. */
        open_k,

        /** A chain is open and transferring, so the next request is staged behind it. */
        held_k,

        /** A chain was cut, so the rest of it is answered with @c cancelled_k. */
        cut_k,
    };

    /** One range in the driver's memory map, and the count of commands still writing into it. */
    struct registered_memory_slot_t {

        /** Zero while the slot is free, one while a range is recorded in it. */
        std::int32_t registered = 0;

        /** Requests accepted against this range and not yet answered for. */
        std::int32_t in_flight = 0;

        /** Raised every time the slot is used again, so a stale place resolves to nothing. */
        queue_pair_generation_t generation = 0;

        /** Where the mapped memory lives, which is host memory for every range this disk takes. */
        memory_space_t space = memory_space_t::host_k;

        /** How the driver came to know the range, which is what giving it back undoes. */
        mapping_t mapping = mapping_t::recorded_k;

        /** First byte of the range, owned by the caller that registered it. */
        byte_t* base = nullptr;

        /** Bytes in the range. */
        std::uint64_t length = 0;
    };

    /** One namespace as a registered target, and the count of commands still naming it. */
    struct registered_medium_slot_t {

        /** Zero while the namespace is unregistered, one while a handle names it. */
        std::int32_t registered = 0;

        /** Requests accepted against the namespace and not yet answered for. */
        std::int32_t in_flight = 0;

        /** Raised every time the slot is used again, so a stale place resolves to nothing. */
        queue_pair_generation_t generation = 0;

        /** What the caller may do to the namespace, which a write is checked against. */
        medium_access_t access = medium_access_t::read_write_k;

        /** Bytes the namespace spans, which an extent past it is refused for. */
        std::uint64_t capacity = 0;

        /** The namespace every request naming this target moves bytes against. */
        spdk_nvme_ns* space = nullptr;

        /** The namespace's number, which its claim on the controller is given back by. */
        std::uint32_t namespace_id = 0;
    };

    /** Namespaces one disk registers at once. */
    static constexpr std::uint16_t targets_limit_k = 16;

    /** Completions one drain discards at a time. */
    static constexpr std::size_t drain_batch_k = 32;

    /** Fruitless polls a drain tolerates before it asks the controller to abort what is left. */
    static constexpr std::size_t drain_patience_k = 1024;

    /** Reaps a drain performs before it calls the queue pair broken. */
    static constexpr std::size_t drain_spin_limit_k = std::size_t {1} << 24;

    /** The index that names no slot, in a free list and in a chain alike. */
    static constexpr std::uint32_t no_entry_k = linux_spdk_queue_t::no_entry_k;

    /** Idle polls of one queue between two polls of the admin queue. */
    static constexpr std::uint32_t admin_poll_stride_k = 64;

    /** Fills @p transport from @p address, parsing a transport ID or taking a bare address as PCIe. */
    static status_t transport_of_(char const* address, spdk_nvme_transport_id& transport) noexcept {
        if (transport_field(address, "trtype:").empty()) {
            transport.trtype = SPDK_NVME_TRANSPORT_PCIE;
            std::snprintf(transport.trstring, sizeof(transport.trstring), "PCIe");
            std::snprintf(transport.traddr, sizeof(transport.traddr), "%s", address);
            return {};
        }
        if (spdk_nvme_transport_id_parse(&transport, address) != 0)
            return {status_code_t::invalid_argument_k, "The driver could not parse that transport ID"};
        return {};
    }

    /** Copies @p count bytes of an identify field into @p into, dropping the spaces the spec pads with. */
    static void copy_trimmed_(std::int8_t const* field, std::size_t count, char* into) noexcept {
        std::size_t kept = count;
        while (kept != 0 && (field[kept - 1] == ' ' || field[kept - 1] == '\0')) --kept;
        for (std::size_t index = 0; index != kept; ++index) into[index] = static_cast<char>(field[index]);
        into[kept] = '\0';
    }

    /** Whatever a mount must refuse before it touches the controller at all. */
    static status_t inspect_config_(linux_spdk_disk_config_t const& config) noexcept {
        if (config.registered_memory_limit >= static_cast<std::uint16_t>(registered_memory_id_t::queue_range_k))
            return {status_code_t::invalid_argument_k, "A ring keeps its last identifiers for special handles"};
        if (config.registered_memory_limit == 0)
            return {status_code_t::invalid_argument_k, "A ring needs a range to move bytes through"};
        return {};
    }

    /** Grows @p elements to @p count default slots, reporting an allocation failure as a status. */
    template <typename element_type_>
    static status_t size_(st::vector<element_type_>& elements, std::size_t count) noexcept {
        if (elements.resize(count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A ring could not allocate its slots"};
        return {};
    }

    static atomic_reference_<std::int32_t> claim_(std::int32_t& word) noexcept {
        return atomic_reference_<std::int32_t> {word};
    }

    static atomic_reference_<std::int32_t> in_flight_(registered_memory_slot_t& slot) noexcept {
        return atomic_reference_<std::int32_t> {slot.in_flight};
    }

    static atomic_reference_<std::int32_t> in_flight_(registered_medium_slot_t& slot) noexcept {
        return atomic_reference_<std::int32_t> {slot.in_flight};
    }

#pragma region Callbacks

    /**
     *  @brief Writes one command's result into the submitted it was issued from.
     *
     *  The driver calls this from inside @c spdk_nvme_qpair_process_completions, on the very thread
     *  that holds the queue pair, so nothing here is shared and no atomic is needed.
     */
    static void completed_(void* context, spdk_nvme_cpl const* result) noexcept {
        submitted_request_t& submitted = *static_cast<submitted_request_t*>(context);
        if (!spdk_nvme_cpl_is_error(result)) {
            submitted.transferred_bytes = submitted.length;
            submitted.outcome = completion_state_t::completed_k;
            submitted.status = {};
            submitted.state = submitted_request_state_t::finished_k;
            return;
        }
        submitted.transferred_bytes = 0;
        submitted.state = submitted_request_state_t::finished_k;
        if (result->status.sct == SPDK_NVME_SCT_GENERIC && result->status.sc == SPDK_NVME_SC_ABORTED_BY_REQUEST) {
            submitted.outcome = completion_state_t::cancelled_k;
            submitted.status = {status_code_t::uninitialized_k, "The queue_pair cancelled this request"};
            return;
        }
        submitted.outcome = completion_state_t::failed_k;
        char const* const spelled = spdk_nvme_cpl_get_status_string_ext(&result->status, submitted.opcode);
        submitted.status = {status_code_t::device_k, spelled};
    }

    /** Swallows the completion an abort posts for itself, which no queue pair ever asked for. */
    static void aborted_(void*, spdk_nvme_cpl const*) noexcept {}

#pragma endregion Callbacks

#pragma region Bookkeeping

    /** The SPDK queue @p queue_pair names, or why this disk cannot serve it. */
    static expected<linux_spdk_queue_t*> queue_of_(queue_ref_t const& queue_pair) noexcept {
        if (linux_spdk_queue_t* const queue = queue_pair.template queue<linux_spdk_queue_t>(); queue) return queue;
        if (queue_pair.template queue<blocking_queue_t>())
            return status_t {status_code_t::unsupported_k, "An SPDK namespace has no file a blocking queue could read"};
        return status_t {status_code_t::invalid_argument_k, "The queue is of another transport"};
    }

    /** Requests @p queue has outstanding: the device's, the disk's own and the held ones. */
    [[nodiscard]] static std::size_t outstanding_requests_(linux_spdk_queue_t const& queue) noexcept {
        return queue.outstanding_ + queue.staged_count_ + queue.completions_made_by_queue_count_;
    }

    [[nodiscard]] registered_memory_slot_t* slot_of_(registered_memory_t const& registered_memory) noexcept {
        if (!registered_memory.held() ||
            static_cast<std::size_t>(registered_memory.id()) >= registered_memories_.size())
            return nullptr;
        registered_memory_slot_t& slot = registered_memories_[static_cast<std::size_t>(registered_memory.id())];
        return slot.registered != 0 && slot.generation == registered_memory.generation() ? &slot : nullptr;
    }

    [[nodiscard]] registered_medium_slot_t* slot_of_(registered_medium_t const& descriptor) noexcept {
        if (!descriptor.held() || static_cast<std::size_t>(descriptor.id()) >= media_.size()) return nullptr;
        registered_medium_slot_t& slot = media_[static_cast<std::size_t>(descriptor.id())];
        return slot.registered != 0 && slot.generation == descriptor.generation() ? &slot : nullptr;
    }

    [[nodiscard]] registered_memory_slot_t* resolve_(memory_reference_t place) noexcept {
        if (static_cast<std::size_t>(place.memory_id) >= registered_memories_.size()) return nullptr;
        registered_memory_slot_t& slot = registered_memories_[static_cast<std::size_t>(place.memory_id)];
        return slot.registered != 0 && slot.generation == place.generation ? &slot : nullptr;
    }

    [[nodiscard]] registered_medium_slot_t* resolve_(medium_reference_t place) noexcept {
        if (static_cast<std::size_t>(place.medium_id) >= media_.size()) return nullptr;
        registered_medium_slot_t& slot = media_[static_cast<std::size_t>(place.medium_id)];
        return slot.registered != 0 && slot.generation == place.generation ? &slot : nullptr;
    }

    /** The bytes @p place names: the submitting queue's own @p range for @c queue_range_k, else a
     *  registered range, or nothing. */
    [[nodiscard]] std::span<byte_t> memory_of_(std::span<byte_t> range, memory_reference_t place) noexcept {
        if (place.memory_id == registered_memory_id_t::queue_range_k) return range;
        registered_memory_slot_t const* const slot = resolve_(place);
        return slot ? std::span<byte_t> {slot->base, static_cast<std::size_t>(slot->length)} : std::span<byte_t> {};
    }

    /** Marks the range and the target of @p request as live, so neither can be taken back under it. */
    void count_request_on_slots_(request_t const& request) noexcept {
        if (registered_memory_slot_t* const slot = resolve_(request.buffer); slot)
            in_flight_(*slot).store_add(1, std::memory_order_acquire);
        if (registered_medium_slot_t* const slot = resolve_(request.medium); slot)
            in_flight_(*slot).store_add(1, std::memory_order_acquire);
    }

    /** Releases both once the device is done with the bytes the request @p submitted remembers. */
    void uncount_request_on_slots_(submitted_request_t const& submitted) noexcept {
        if (registered_memory_slot_t* const slot = resolve_(submitted.buffer); slot)
            in_flight_(*slot).store_sub(1, std::memory_order_release);
        if (registered_medium_slot_t* const slot = resolve_(submitted.medium); slot)
            in_flight_(*slot).store_sub(1, std::memory_order_release);
    }

    /** Takes one submitted slot off @p queue's free list, or @c no_entry_k when every
     *  one is taken. */
    [[nodiscard]] static std::uint32_t take_free_submitted_request_(linux_spdk_queue_t& queue) noexcept {
        std::uint32_t const index = queue.first_free_submitted_request_;
        if (index == no_entry_k) return no_entry_k;
        queue.first_free_submitted_request_ = queue.submitted_requests_[index].next_free;
        ++queue.outstanding_;
        return index;
    }

    /** Puts one submitted slot back on @p queue's free list, clearing what it held. */
    static void free_submitted_request_(linux_spdk_queue_t& queue, std::uint32_t index) noexcept {
        queue.submitted_requests_[index] = submitted_request_t {};
        queue.submitted_requests_[index].next_free = queue.first_free_submitted_request_;
        queue.first_free_submitted_request_ = index;
        --queue.outstanding_;
    }

    /** Puts one staging slot back on @p queue's free list, clearing what it held. */
    static void free_staged_(linux_spdk_queue_t& queue, std::uint32_t index) noexcept {
        queue.staged_[index] = staged_t {};
        queue.staged_[index].next_free = queue.staged_free_;
        queue.staged_free_ = index;
        --queue.staged_count_;
    }

    /** Queues @p completion among @p queue's own, which poll drains after the device's. */
    static void push_completion_made_by_queue_(linux_spdk_queue_t& queue, completion_t const& completion) noexcept {
        queue.completions_made_by_queue_[(queue.completions_made_by_queue_first_ +
                                          queue.completions_made_by_queue_count_) %
                                         queue.depth_] = completion;
        ++queue.completions_made_by_queue_count_;
    }

    /** Moves what the disk answered itself into @p into, oldest first. */
    static std::size_t drain_completions_made_by_queue_(linux_spdk_queue_t& queue,
                                                        std::span<completion_t> into) noexcept {
        std::size_t const moving = into.size() < queue.completions_made_by_queue_count_
                                       ? into.size()
                                       : queue.completions_made_by_queue_count_;
        for (std::size_t index = 0; index != moving; ++index)
            into[index] =
                queue.completions_made_by_queue_[(queue.completions_made_by_queue_first_ + index) % queue.depth_];
        queue.completions_made_by_queue_first_ = static_cast<std::uint32_t>(
            (queue.completions_made_by_queue_first_ + moving) % queue.depth_);
        queue.completions_made_by_queue_count_ -= static_cast<std::uint32_t>(moving);
        return moving;
    }

#pragma endregion Bookkeeping

#pragma region Requests

    /** The completion a request gets when a chain ahead of it was cut. */
    [[nodiscard]] static completion_t cancelled_completion_(request_token_t token) noexcept {
        completion_t completion;
        completion.token = token;
        completion.state = completion_state_t::cancelled_k;
        completion.status = {status_code_t::uninitialized_k, "The queue_pair cancelled this request"};
        return completion;
    }

    /** The completion a request gets when the disk saw a defect the driver would never report. */
    [[nodiscard]] static completion_t failed_completion_(request_token_t token, status_t reason) noexcept {
        completion_t completion;
        completion.token = token;
        completion.state = completion_state_t::failed_k;
        completion.status = reason;
        return completion;
    }

    /** Whether the request that follows @p request in its batch waits for this one to transfer whole. */
    [[nodiscard]] static bool chains_next_(request_t const& request) noexcept {
        return request.chaining == request_chaining_t::chained_k;
    }

    /** Proves the controller can reach @p chunk the way this transport moves bytes, recording how in @p slot. */
    status_t map_(registered_memory_slot_t& slot, std::span<byte_t> chunk) noexcept {
        if (reach_ == reach_t::copied_k) {
            slot.mapping = mapping_t::recorded_k;
            return {};
        }
        if (inspect_translation_(chunk).succeeded()) {
            slot.mapping = mapping_t::mapped_k;
            return {};
        }
        // The driver maps whole 2 MiB pages and refuses a range that starts or ends inside one
        constexpr std::uint64_t mapped_page_bytes = 2ull * 1024ull * 1024ull;
        if (chunk.size() % mapped_page_bytes != 0 || !is_address_aligned(chunk.data(), mapped_page_bytes))
            return {status_code_t::invalid_argument_k, "A DMA range the driver maps spans whole 2 MiB pages"};
        if (spdk_mem_register(chunk.data(), chunk.size()) != 0)
            return {status_code_t::out_of_memory_k, "The driver would not map this range"};
        if (status_t const reachable = inspect_translation_(chunk); reachable.failed()) {
            spdk_mem_unregister(chunk.data(), chunk.size());
            return reachable;
        }
        slot.mapping = mapping_t::registered_k;
        return {};
    }

    /** Takes back whatever @c map_ did for @p slot, which is nothing for a range the driver already knew. */
    static void unmap_(registered_memory_slot_t& slot) noexcept {
        if (slot.mapping == mapping_t::registered_k)
            spdk_mem_unregister(slot.base, static_cast<std::size_t>(slot.length));
        slot.mapping = mapping_t::recorded_k;
    }

    /** Polls the admin queue, where an abort's completion lands, every @c admin_poll_stride_k idle
     *  polls of @p queue. */
    void poll_admin_when_idle_(linux_spdk_queue_t& queue) noexcept {
        if (++queue.idle_polls_ != admin_poll_stride_k) return;
        queue.idle_polls_ = 0;
        [[maybe_unused]] std::int32_t const polled = spdk_nvme_ctrlr_process_admin_completions(controller_);
    }

    /** Whether the driver can translate every page of @p chunk, which a DMA target must allow. */
    static status_t inspect_translation_(std::span<byte_t> chunk) noexcept {
        std::uint64_t walked = 0;
        while (walked != chunk.size()) {
            std::uint64_t reachable = chunk.size() - walked;
            std::uint64_t const physical = spdk_vtophys(chunk.data() + walked, &reachable);
            if (physical == SPDK_VTOPHYS_ERROR || reachable == 0)
                return {status_code_t::invalid_argument_k, "A DMA range must be hugepage-backed throughout"};
            walked += reachable < chunk.size() - walked ? reachable : chunk.size() - walked;
        }
        return {};
    }

    /** Whatever the driver would accept and the device would then corrupt, checked before it is
     *  issued, @p range being the submitting queue's own blocks. */
    status_t inspect_(std::span<byte_t> range, request_t const& request) noexcept {
        registered_medium_slot_t const* const target = resolve_(request.medium);
        if (!target) return {status_code_t::invalid_argument_k, "The request names no registered target"};
        if ((request.kind == request_kind_t::write_k || request.kind == request_kind_t::deallocate_k) &&
            target->access == medium_access_t::read_only_k)
            return {status_code_t::invalid_argument_k, "The target was registered for reads alone"};
        if (request.kind == request_kind_t::flush_k) return {};
        if (request.kind == request_kind_t::deallocate_k) {
            if (!request.extent.aligned_to(sector_bytes_) ||
                request.extent.length / sector_bytes_ > (std::numeric_limits<std::uint32_t>::max)())
                return {status_code_t::invalid_argument_k, "A deallocation covers whole sectors, at most one range"};
            if (!request.extent.fits_within(target->capacity))
                return {status_code_t::invalid_argument_k, "The request runs past the namespace"};
            return {};
        }
        std::span<byte_t> const memory = memory_of_(range, request.buffer);
        if (memory.empty()) return {status_code_t::invalid_argument_k, "The request names no registered range"};
        if (request.extent.length == 0)
            return {status_code_t::invalid_argument_k, "A transfer of no bytes is not a command"};
        if (request.buffer.offset > memory.size() || request.extent.length > memory.size() - request.buffer.offset)
            return {status_code_t::invalid_argument_k, "The request runs past its registered range"};
        if (request.extent.length > max_transfer_bytes_)
            return {status_code_t::invalid_argument_k, "The request is larger than one transfer"};
        if (!request.extent.aligned_to(sector_bytes_))
            return {status_code_t::invalid_argument_k, "The request does not meet the namespace's sector"};
        if (!request.extent.fits_within(target->capacity))
            return {status_code_t::invalid_argument_k, "The request runs past the namespace"};
        if (!is_address_aligned(memory.data() + request.buffer.offset, portable_alignment_k))
            return {status_code_t::invalid_argument_k, "The payload does not sit on a page boundary"};
        return {};
    }

    /**
     *  @brief Issues @p request on @p queue's held pair, remembering what its completion needs.
     *  @return The submitted slot it occupies, or why the driver would not take it.
     */
    expected<std::uint32_t> start_(linux_spdk_queue_t& queue, request_t const& request) noexcept {
        std::uint32_t const index = take_free_submitted_request_(queue);
        if (index == no_entry_k)
            return status_t {status_code_t::capacity_exhausted_k, "This queue has no room for another command"};
        submitted_request_t& submitted = queue.submitted_requests_[index];
        submitted.token = request.token;
        submitted.length = request.kind == request_kind_t::flush_k || request.kind == request_kind_t::deallocate_k
                               ? 0
                               : request.extent.length;
        submitted.buffer = request.buffer;
        submitted.medium = request.medium;
        submitted.successor = no_entry_k;
        submitted.state = submitted_request_state_t::outstanding_k;
        count_request_on_slots_(request);

        spdk_nvme_ns* const space = resolve_(request.medium)->space;
        int issued = 0;
        submitted.opcode = request.kind == request_kind_t::flush_k        ? SPDK_NVME_OPC_FLUSH
                           : request.kind == request_kind_t::deallocate_k ? SPDK_NVME_OPC_DATASET_MANAGEMENT
                           : request.kind == request_kind_t::read_k       ? SPDK_NVME_OPC_READ
                                                                          : SPDK_NVME_OPC_WRITE;
        if (request.kind == request_kind_t::flush_k) {
            issued = spdk_nvme_ns_cmd_flush(space, queue.media_pair_, &linux_spdk_disk::completed_, &submitted);
        }
        else if (request.kind == request_kind_t::deallocate_k) {
            // The driver copies the range into the command's own payload, so it may live
            // on this stack.
            spdk_nvme_dsm_range range {};
            range.starting_lba = request.extent.offset / sector_bytes_;
            range.length = static_cast<std::uint32_t>(request.extent.length / sector_bytes_);
            issued = spdk_nvme_ns_cmd_dataset_management(space, queue.media_pair_, SPDK_NVME_DSM_ATTR_DEALLOCATE,
                                                         &range, 1, &linux_spdk_disk::completed_, &submitted);
        }
        else {
            void* const payload = memory_of_(queue.pinned_staging_, request.buffer).data() + request.buffer.offset;
            std::uint64_t const first = request.extent.offset / sector_bytes_;
            std::uint32_t const sectors = static_cast<std::uint32_t>(request.extent.length / sector_bytes_);
            issued = request.kind == request_kind_t::read_k
                         ? spdk_nvme_ns_cmd_read(space, queue.media_pair_, payload, first, sectors,
                                                 &linux_spdk_disk::completed_, &submitted, 0)
                         : spdk_nvme_ns_cmd_write(space, queue.media_pair_, payload, first, sectors,
                                                  &linux_spdk_disk::completed_, &submitted, 0);
        }
        if (issued == 0) return index;
        uncount_request_on_slots_(submitted);
        free_submitted_request_(queue, index);
        if (issued == -ENOMEM)
            return status_t {status_code_t::capacity_exhausted_k, "The queue pair has no room for another command"};
        return status_t {status_code_t::device_k, "The driver would not take this command"};
    }

    /** Holds @p request back until the chain reaches it, answering the staging slot it occupies. */
    static expected<std::uint32_t> hold_(linux_spdk_queue_t& queue, request_t const& request) noexcept {
        std::uint32_t const index = queue.staged_free_;
        if (index == no_entry_k)
            return status_t {status_code_t::capacity_exhausted_k, "This queue has no room to hold a chain"};
        queue.staged_free_ = queue.staged_[index].next_free;
        ++queue.staged_count_;
        queue.staged_[index].request = request;
        queue.staged_[index].next = no_entry_k;
        return index;
    }

    /** Answers every request of the chain from @p first with @c cancelled_k, since it was cut. */
    static void cancel_linked_chain_(linux_spdk_queue_t& queue, std::uint32_t first) noexcept {
        for (std::uint32_t entry = first; entry != no_entry_k;) {
            std::uint32_t const next = queue.staged_[entry].next;
            push_completion_made_by_queue_(queue, cancelled_completion_(queue.staged_[entry].request.token));
            free_staged_(queue, entry);
            entry = next;
        }
    }

    /** Issues the staged request @p entry now that its predecessor transferred in full. */
    void resume_(linux_spdk_queue_t& queue, std::uint32_t entry) noexcept {
        request_t const request = queue.staged_[entry].request;
        std::uint32_t const next = queue.staged_[entry].next;
        free_staged_(queue, entry);
        status_t const defect = inspect_(queue.pinned_staging_, request);
        if (defect.failed()) {
            push_completion_made_by_queue_(queue, failed_completion_(request.token, defect));
            cancel_linked_chain_(queue, next);
            return;
        }
        auto [started, start_status] = start_(queue, request);
        if (start_status.failed()) {
            push_completion_made_by_queue_(queue, failed_completion_(request.token, start_status));
            cancel_linked_chain_(queue, next);
            return;
        }
        queue.submitted_requests_[started].successor = next;
    }

    /** Copies every finished command of @p queue into @p into, resuming or cutting their chains. */
    std::size_t poll_completions_(linux_spdk_queue_t& queue, std::span<completion_t> into) noexcept {
        std::size_t moved = 0;
        for (std::uint32_t index = 0; index != queue.depth_ && moved != into.size(); ++index) {
            submitted_request_t& submitted = queue.submitted_requests_[index];
            if (submitted.state != submitted_request_state_t::finished_k) continue;
            completion_t completion;
            completion.token = submitted.token;
            completion.transferred_bytes = submitted.transferred_bytes;
            completion.status = submitted.status;
            completion.state = submitted.outcome;
            std::uint32_t const successor = submitted.successor;
            uncount_request_on_slots_(submitted);
            free_submitted_request_(queue, index);
            into[moved] = completion;
            ++moved;
            if (successor == no_entry_k) continue;
            if (completion.state == completion_state_t::completed_k) resume_(queue, successor);
            else cancel_linked_chain_(queue, successor);
        }
        return moved;
    }

#pragma endregion Requests

  public:

    /**
     *  @brief Unmaps every range, and gives back every namespace and the disk's share of the
     *      controller, detaching it after the last holder, before the disk dies.
     *  @note It waits on the process's claims, so the mount's unmount calls it, never a destructor.
     *      A queue still holding an I/O queue pair on the controller keeps it attached.
     */
    void release() noexcept {
        for (std::size_t index = 0; index != registered_memories_.size(); ++index) {
            registered_memory_slot_t& slot = registered_memories_[index];
            if (!slot.base) continue;
            unmap_(slot);
            slot.base = nullptr;
        }
        for (registered_medium_slot_t& slot : media_)
            if (slot.space)
                linux_spdk_release_namespace<atomic_reference_, waiter_type_>(*attachment_, slot.namespace_id);
        if (attachment_) linux_spdk_detach<atomic_reference_, waiter_type_>(*attachment_);
        attachment_ = nullptr;
        controller_ = nullptr;
        registered_memories_.clear();
        media_.clear();
    }

  private:

    /** The environment this disk holds a share of, declared first so it outlives everything below. */
    linux_spdk_environment<atomic_reference_, waiter_type_> environment_;

    /** One slot per memory range the driver has mapped for this disk. */
    st::vector<registered_memory_slot_t> registered_memories_;

    /** The namespaces registered as targets, at most @c targets_limit_k of them. */
    st::vector<registered_medium_slot_t> media_;

    /** The process's connection to the controller, whose share this disk gives back at close. */
    linux_spdk_attachment_t* attachment_ = nullptr;

    /** The controller of @c attachment_. */
    spdk_nvme_ctrlr* controller_ = nullptr;

    /** Bytes one sector spans, which every offset and every length must be a multiple of. */
    std::uint64_t sector_bytes_ = 0;

    /** The largest extent one command holds, as the namespace reports it. */
    std::uint64_t max_transfer_bytes_ = 0;

    /** I/O queue pairs the controller grants the process, which bounds the queues it serves. */
    std::uint16_t queue_pairs_limit_ = 0;

    /** Memory ranges the disk maps at once. */
    std::uint16_t registered_memories_limit_ = 0;

    /** How the controller reaches a registered range, fixed by the transport the mount connected over. */
    reach_t reach_ = reach_t::translated_k;
};

#pragma endregion Disk

#else

#pragma region Absent

/** The queue in a build that holds no SPDK, which a queue set never makes. */
class linux_spdk_queue_t {
  public:

    /** Refuses the queue, since this build holds no driver. */
    static expected<linux_spdk_queue_t> make(queue_depth_t, std::span<byte_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This build holds no SPDK"};
    }

    /** Requests the queue keeps in flight, which is none. */
    [[nodiscard]] queue_depth_t depth() const noexcept { return 0; }
};

/**
 *  @brief The same disk in a build that holds no SPDK, which mounts nowhere and refuses cleanly.
 *
 *  The type exists so a caller compiles against it unconditionally and chooses at mount, where a
 *  refused @c open sends it to another transport rather than to a build-time branch. The probe is
 *  not here, because it belongs to the environment and must answer honestly in every build.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_spdk_disk {
  public:

    /** The queue a thread owns for this transport. */
    using queue_t = linux_spdk_queue_t;

    /** Refuses the mount, since this build holds no driver to mount with. */
    static expected<linux_spdk_disk> open(linux_spdk_environment<atomic_reference_, waiter_type_>, char const*,
                                          linux_spdk_disk_config_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This build holds no SPDK"};
    }

    /** Whether the arena must come from DMA memory, which a disk that never
     *  connected never asks. */
    [[nodiscard]] bool needs_dma_memory() const noexcept { return false; }

    /** No memory domain, since no controller was ever connected. */
    [[nodiscard]] int memory_domain() const noexcept { return -1; }

    /** What this disk would accept, which is nothing at all. */
    [[nodiscard]] disk_capabilities_t capabilities() const noexcept { return {}; }

    /** What the controller identifies as, which is nothing, since none was ever connected. */
    [[nodiscard]] linux_spdk_controller_identity_t identity() const noexcept { return {}; }

    /** Refuses the registration, since there is no controller to register on a queue. */
    status_t register_media(queue_ref_t const&) noexcept {
        return {status_code_t::unsupported_k, "This build holds no SPDK"};
    }

    /** Refuses the range, since no driver would ever map it. */
    expected<registered_memory_t> register_memory(std::span<byte_t>, memory_space_t) noexcept {
        return status_t {status_code_t::unsupported_k, "This build holds no SPDK"};
    }

    /** Refuses the range, since none was ever mapped. */
    status_t unregister_memory(registered_memory_t&) noexcept {
        return {status_code_t::unsupported_k, "This build holds no SPDK"};
    }

    /** Refuses the target, since there is no namespace to present. */
    expected<registered_medium_t> register_medium(medium_options_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This build holds no SPDK"};
    }

    /** Refuses the target, since none was ever registered. */
    status_t unregister_medium(registered_medium_t&) noexcept {
        return {status_code_t::unsupported_k, "This build holds no SPDK"};
    }

    /** Takes nothing, since there is no queue pair to take it into. */
    expected<std::size_t> submit(queue_ref_t const&, std::span<request_t const>) noexcept {
        return status_t {status_code_t::unsupported_k, "This build holds no SPDK"};
    }

    /** Answers nothing, since nothing was ever submitted. */
    expected<std::size_t> poll_completions(queue_ref_t const&, std::span<completion_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This build holds no SPDK"};
    }

    /** Stops nothing, since nothing was ever submitted. */
    void cancel(queue_ref_t const&) noexcept {}

    /** Refuses the drain, since no queue pair was ever handed out. */
    status_t drain(queue_ref_t const&) noexcept { return {status_code_t::unsupported_k, "This build holds no SPDK"}; }

    /** Answers that nothing is outstanding, which is always true here. */
    [[nodiscard]] std::size_t outstanding(queue_ref_t const&) const noexcept { return 0; }

    /** Gives back nothing, since nothing was ever taken. */
    void release() noexcept {}
};

#pragma endregion Absent

#endif

} // namespace unum::ustore::utilities
