/**
 *  @file include/ustore/utilities/nvme_linux_spdk_disk.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The ring SPDK offers: a user-space NVMe driver over the namespaces of one controller, one
 *      queue pair per queue pair.
 *
 *  This transport presents each namespace it registers as a target, named by its number, and keeps
 *  no name mapping of its own; the manifest above it names what the extents hold, so a torn write
 *  of a map cannot lose a drive. The driver attaches a controller once per process, so every disk
 *  opened on one address shares that attachment and takes queue pairs of its own from it, and no
 *  two disks register the same namespace. Nothing here blocks, as the driver offers no interrupt:
 *  @c spdk_nvme_qpair_process_completions is the only way a command is ever seen to finish.
 *
 *  A queue pair is a queue pair rather than a label, since an @c spdk_nvme_qpair carries no lock
 *  and a second thread polling one corrupts its tracker list without a word. @c queue_pairs_limit
 *  is therefore the controller's queue-pair count, which may be fewer than the executor has
 *  threads, and a handover is a move of the queue pair announced with @c adopt.
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

#if USTORE_WITH_SPDK
#include <spdk/env.h>      // `spdk_env_init`, `spdk_mem_register`, `spdk_vtophys`
#include <spdk/env_dpdk.h> // `spdk_env_dpdk_external_init`
#include <spdk/nvme.h>     // `spdk_nvme_connect`, `spdk_nvme_ns_cmd_read`, `spdk_nvme_qpair`
#endif

#include <smashtable/basic_growing_pool.hpp> // `st::basic_growing_pool`
#include <smashtable/basic_vector.hpp>       // `st::vector`

#include "ustore/utilities/nvme_ring.hpp" // `request_t`, `completion_t`, `queue_pair_t`, `portable_alignment_k`, `is_address_aligned`
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

/**
 *  @brief What a mount fixes about an SPDK disk before any queue pair queue pairs a share of it.
 *
 *  @c queue_pairs_limit is a request rather than a promise: the mount lowers it to the queue pairs the
 *  controller actually offers, which on a consumer drive is far fewer than a large executor has
 *  threads, so a caller reads @c capabilities to learn how many queue pairs it really got.
 */
struct linux_spdk_disk_config_t {

    /** Queue pairs the mount asks the controller for, which is the queue pairs it hands out. */
    std::uint16_t queue_pairs_limit = 1;

    /** Requests one queue pair may keep in flight, which is also the depth of its queue pair. */
    queue_depth_t depth_limit = 64;

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

#if USTORE_WITH_SPDK

#pragma region Process

/**
 *  @brief One connection to a controller, which every disk opened on its address shares.
 *
 *  The driver attaches a controller once per process, so a second mount on the same address takes
 *  this one rather than connecting again. Every open disk and every namespace registered holds a
 *  share, and the last share given back detaches the controller alone, so its drive may be pulled.
 */
struct linux_spdk_attachment_t {

    /** The address the controller was connected at, which a later open is matched against. */
    spdk_nvme_transport_id transport {};

    /** The controller, null until the first share connects it. */
    spdk_nvme_ctrlr* controller = nullptr;

    /** Disks open and namespaces registered on the connection, zero while the record is free. */
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
    return status_t {status_code_t::unsupported_k, "This build carries no SPDK"};
}

#endif

#pragma endregion Environment

#if USTORE_WITH_SPDK

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
 *  @tparam atomic_reference_ The reference the claim and in-flight words are owned through.
 *  @tparam waiter_type_ What a draining core does after a poll that moved nothing.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_spdk_disk {
  public:

    linux_spdk_disk() noexcept = default;
    linux_spdk_disk(linux_spdk_disk const&) = delete;
    linux_spdk_disk& operator=(linux_spdk_disk const&) = delete;

    linux_spdk_disk(linux_spdk_disk&& other) noexcept
        : environment_(std::move(other.environment_)), queue_pairs_(std::move(other.queue_pairs_)),
          registered_memories_(std::move(other.registered_memories_)), media_(std::move(other.media_)),
          flights_(std::move(other.flights_)), staged_(std::move(other.staged_)), locals_(std::move(other.locals_)),
          attachment_(std::exchange(other.attachment_, nullptr)),
          controller_(std::exchange(other.controller_, nullptr)), sector_bytes_(std::exchange(other.sector_bytes_, 0)),
          max_transfer_bytes_(std::exchange(other.max_transfer_bytes_, 0)),
          depth_limit_(std::exchange(other.depth_limit_, 0)),
          queue_pairs_limit_(std::exchange(other.queue_pairs_limit_, 0)),
          registered_memories_limit_(std::exchange(other.registered_memories_limit_, 0)), reach_(other.reach_) {}

    linux_spdk_disk& operator=(linux_spdk_disk&& other) noexcept {
        if (this == &other) return *this;
        assert(!attachment_ && "release a disk before another takes its place");
        environment_ = std::move(other.environment_);
        queue_pairs_ = std::move(other.queue_pairs_);
        registered_memories_ = std::move(other.registered_memories_);
        media_ = std::move(other.media_);
        flights_ = std::move(other.flights_);
        staged_ = std::move(other.staged_);
        locals_ = std::move(other.locals_);
        attachment_ = std::exchange(other.attachment_, nullptr);
        controller_ = std::exchange(other.controller_, nullptr);
        sector_bytes_ = std::exchange(other.sector_bytes_, 0);
        max_transfer_bytes_ = std::exchange(other.max_transfer_bytes_, 0);
        depth_limit_ = std::exchange(other.depth_limit_, 0);
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
     *  @param[in] config The queue pairs wanted, the per-queue pair depth and the ranges registered at once.
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
            return status_t {status_code_t::not_found_k, "The controller carries no active namespace"};
        }

        spdk_nvme_ctrlr_opts const* const controller_options = spdk_nvme_ctrlr_get_opts(controller);
        std::uint32_t const queues = controller_options && controller_options->num_io_queues != 0
                                         ? controller_options->num_io_queues
                                         : 1u;
        std::uint16_t const queue_pairs = static_cast<std::uint16_t>(
            config.queue_pairs_limit < queues ? config.queue_pairs_limit : queues);

        linux_spdk_disk disk;
        disk.environment_ = std::move(environment);
        disk.attachment_ = attachment;
        disk.controller_ = controller;
        disk.sector_bytes_ = spdk_nvme_ns_get_sector_size(chosen);
        disk.max_transfer_bytes_ = spdk_nvme_ns_get_max_io_xfer_size(chosen);
        disk.depth_limit_ = config.depth_limit;
        disk.queue_pairs_limit_ = queue_pairs;
        disk.registered_memories_limit_ = config.registered_memory_limit;
        disk.reach_ = transport.trtype == SPDK_NVME_TRANSPORT_TCP ? reach_t::copied_k : reach_t::translated_k;
        std::size_t const window = static_cast<std::size_t>(queue_pairs) * config.depth_limit;
        status_t sized;
        if (disk.sector_bytes_ == 0 || !std::has_single_bit(disk.sector_bytes_))
            sized = {status_code_t::unsupported_k, "The namespace reports no power-of-two sector"};
        if (sized.succeeded()) sized = size_(disk.queue_pairs_, queue_pairs);
        if (sized.succeeded()) sized = size_(disk.registered_memories_, config.registered_memory_limit);
        if (sized.succeeded()) sized = size_(disk.media_, targets_limit_k);
        if (sized.succeeded()) sized = size_(disk.flights_, window);
        if (sized.succeeded()) sized = size_(disk.staged_, window);
        if (sized.succeeded()) sized = size_(disk.locals_, window);
        if (sized.failed()) {
            disk.release();
            return sized;
        }
        for (std::size_t seat = 0; seat != disk.queue_pairs_.size(); ++seat) disk.link_free_lists_(seat);
        return disk;
    }

    /** What this disk accepts, which the namespace's sector size and the controller's limits fix. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept {
        ring_capabilities_t reported;
        reported.memory_alignment = portable_alignment_k;
        reported.offset_alignment = sector_bytes_;
        reported.length_granularity = sector_bytes_;
        reported.max_transfer_bytes = max_transfer_bytes_;
        reported.depth_limit = depth_limit_;
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

#pragma region Queue Pairs

    /**
     *  @brief Takes a queue pair on @p depth requests and allocates the queue pair that carries them.
     *  @param[in] depth Requests this queue pair may keep in flight, at most the mount's limit.
     *  @return The queue pair, or @c capacity_exhausted_k when every queue pair is already out.
     *
     *  @note The ceiling is the controller's queue-pair count rather than the executor's thread
     *      count, so a large executor sees fewer queue pairs than it has threads and must move them.
     */
    expected<queue_pair_t> acquire_queue_pair(queue_depth_t depth) noexcept {
        if (depth == 0 || depth > depth_limit_)
            return status_t {status_code_t::invalid_argument_k, "A queue pair asks for more depth than the ring has"};
        for (std::size_t seat = 0; seat != queue_pairs_.size(); ++seat) {
            queue_pair_slot_t& slot = queue_pairs_[seat];
            std::int32_t expected_free = 0;
            if (!claim_(slot.leased)
                     .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            spdk_nvme_io_qpair_opts options {};
            spdk_nvme_ctrlr_get_default_io_qpair_opts(controller_, &options, sizeof(options));
            options.io_queue_size = depth_limit_ + 1;
            options.io_queue_requests = depth_limit_ * 2;
            slot.queue = spdk_nvme_ctrlr_alloc_io_qpair(controller_, &options, sizeof(options));
            if (!slot.queue) {
                claim_(slot.leased).store(0, std::memory_order_release);
                return status_t {status_code_t::capacity_exhausted_k, "The controller would not give a queue pair"};
            }
            slot.depth = depth;
            slot.locals_first = 0;
            slot.locals_count = 0;
            link_free_lists_(seat);
            return queue_pair_t {disk_handle_t {static_cast<std::uint16_t>(seat), slot.generation}, depth};
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every queue pair of this ring is already held"};
    }

    /**
     *  @brief Makes the calling thread the one that polls @p queue pair.
     *  @param[in] queue_pair The queue pair this thread has just taken over, with nothing outstanding on it.
     *
     *  @note A queue pair carries no lock, so a second thread polling one corrupts its
     *      tracker list in silence; that is why a handover is announced and why the queue
     *      pair must be empty first.
     */
    status_t bind_queue_pair(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        if (outstanding_(*slot) != 0)
            return {status_code_t::invalid_argument_k, "Drain the queue pair before handing it over"};
        return {};
    }

    /**
     *  @brief Frees @p queue pair once it has nothing outstanding, and empties the handle.
     *  @param[in,out] queue_pair The queue pair to return, surrendered on success and untouched on failure.
     *
     *  @note The admin queue is polled first, so an abort issued for this queue pair finishes before the
     *      queue pair it names is gone.
     */
    status_t release_queue_pair(queue_pair_t& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        if (outstanding_(*slot) != 0)
            return {status_code_t::invalid_argument_k, "Drain the queue pair before releasing it"};
        [[maybe_unused]] std::int32_t const polled = spdk_nvme_ctrlr_process_admin_completions(controller_);
        if (slot->queue) spdk_nvme_ctrlr_free_io_qpair(slot->queue);
        slot->queue = nullptr;
        ++slot->generation;
        slot->depth = 0;
        claim_(slot->leased).store(0, std::memory_order_release);
        queue_pair.surrender();
        return {};
    }

    /** Requests @p queue pair has submitted and not yet completed, staged successors included. */
    [[nodiscard]] std::size_t outstanding(queue_pair_t const& queue_pair) const noexcept {
        queue_pair_slot_t const* const slot = slot_of_(queue_pair);
        return slot ? outstanding_(*slot) : 0;
    }

#pragma endregion Queue Pairs

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
            return status_t {status_code_t::not_found_k, "The controller carries no such active namespace"};
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
     *  @brief Takes as much of @p requests as @p queue pair still has credit for, and never waits.
     *  @param[in] queue_pair The queue pair submitting, held by the calling thread.
     *  @param[in] requests The batch, of which a prefix is taken.
     *  @return How many were taken, which is zero on a full queue and never a reason to block.
     */
    expected<std::size_t> submit(queue_pair_t const& queue_pair, std::span<request_t const> requests) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        for (request_t const& request : requests)
            if (request.token.queue_pair() != queue_pair.id() || request.token.generation() != queue_pair.generation())
                return status_t {status_code_t::invalid_argument_k, "A request carries another queue pair's token"};

        std::uint32_t const base = window_of_(queue_pair.id());
        std::size_t const credit = slot->depth - outstanding_(*slot);
        std::size_t const room = requests.size() < credit ? requests.size() : credit;

        std::uint32_t chain_flight = no_entry_k;
        std::uint32_t chain_staged = no_entry_k;
        chaining_state_t chaining = chaining_state_t::open_k;
        std::size_t taken = 0;
        for (; taken != room; ++taken) {
            request_t const& request = requests[taken];
            if (chaining == chaining_state_t::cut_k) {
                push_local_(*slot, base, cancelled_completion_(request.token));
                chaining = chains_next_(request) ? chaining_state_t::cut_k : chaining_state_t::open_k;
                continue;
            }
            if (chaining == chaining_state_t::held_k) {
                auto [entry, hold_status] = hold_(*slot, base, request);
                if (hold_status.failed()) {
                    push_local_(*slot, base, failed_completion_(request.token, hold_status));
                    chaining = chains_next_(request) ? chaining_state_t::cut_k : chaining_state_t::open_k;
                    continue;
                }
                if (chain_staged == no_entry_k) flights_[base + chain_flight].successor = entry;
                else staged_[base + chain_staged].next = entry;
                chain_staged = entry;
                chaining = chains_next_(request) ? chaining_state_t::held_k : chaining_state_t::open_k;
                continue;
            }
            status_t const defect = inspect_(request);
            if (defect.failed()) {
                push_local_(*slot, base, failed_completion_(request.token, defect));
                chaining = chains_next_(request) ? chaining_state_t::cut_k : chaining_state_t::open_k;
                continue;
            }
            auto [started, start_status] = start_(*slot, base, request);
            if (start_status.failed()) {
                if (start_status == status_code_t::capacity_exhausted_k) break;
                push_local_(*slot, base, failed_completion_(request.token, start_status));
                chaining = chains_next_(request) ? chaining_state_t::cut_k : chaining_state_t::open_k;
                continue;
            }
            chain_flight = started;
            chain_staged = no_entry_k;
            chaining = chains_next_(request) ? chaining_state_t::held_k : chaining_state_t::open_k;
        }
        return taken;
    }

    /**
     *  @brief Polls @p queue pair and copies what finished into @p into.
     *  @param[in] queue_pair The queue pair polling, held by the calling thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero when nothing has landed.
     *
     *  @note Polling is the only mechanism the driver has, which is why nothing here waits and why
     *      a bounded wait is a spin over this call rather than a blocking one.
     */
    expected<std::size_t> poll_completions(queue_pair_t const& queue_pair, std::span<completion_t> into) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        if (into.empty()) return std::size_t {0};
        std::uint32_t const base = window_of_(queue_pair.id());
        if (slot->flights_count != 0) {
            std::int32_t const polled = spdk_nvme_qpair_process_completions(slot->queue,
                                                                            static_cast<std::uint32_t>(into.size()));
            if (polled < 0) return status_t {status_code_t::device_k, "The queue pair failed while polling"};
            if (polled == 0) poll_admin_when_idle_(*slot);
        }
        std::size_t moved = poll_completions_(*slot, base, into);
        moved += drain_locals_(*slot, base, into.subspan(moved));
        return moved;
    }

    /**
     *  @brief Asks the controller to abort everything @p queue pair has in flight, which it may refuse.
     *  @param[in] queue_pair The queue pair to stop, whose started commands still run to their own end.
     *
     *  @note A refused abort is not an error here: the requests finish normally and @c drain polls
     *      until they do, so a queue pair still sees exactly one completion per request it submitted.
     */
    void cancel(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot || slot->flights_count == 0) return;
        std::uint32_t const base = window_of_(queue_pair.id());
        for (std::uint32_t index = 0; index != depth_limit_; ++index) {
            flight_t& flight = flights_[base + index];
            if (flight.state != flight_state_t::outstanding_k) continue;
            [[maybe_unused]] int const asked = spdk_nvme_ctrlr_cmd_abort_ext(controller_, slot->queue, &flight,
                                                                             &linux_spdk_disk::aborted_, nullptr);
        }
    }

    /**
     *  @brief Finishes and discards everything @p queue pair has outstanding, so its buffers may be freed.
     *  @param[in] queue_pair The queue pair to empty, which may be cancelled or still running.
     */
    status_t drain(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t const* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        std::array<completion_t, drain_batch_k> discarded {};
        for (std::size_t spins = 0; spins != drain_spin_limit_k; ++spins) {
            if (outstanding_(*slot) == 0) return {};
            auto [moved, poll_completions_status] = poll_completions(queue_pair, std::span<completion_t> {discarded});
            if (poll_completions_status.failed()) return poll_completions_status;
            if (moved != 0) continue;
            if (spins == drain_patience_k) cancel(queue_pair);
            waiter_type_ {}(&spins, spins, std::size_t {0});
        }
        return {status_code_t::io_k, "The queue pair would not drain"};
    }

#pragma endregion Ring

  private:

    /** Whether a flight slot is free, carries a command the device holds, or carries its result. */
    enum class flight_state_t : std::uint8_t {

        /** Nothing occupies the slot, which is on the queue pair's free list. */
        vacant_k,

        /** The device holds the command and has not answered for it. */
        outstanding_k,

        /** The completion callback has written the result, which the next poll copies out. */
        finished_k,
    };

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

    /** One queue pair, its credit, and the three queues it polls from. */
    struct queue_pair_slot_t {

        /** Zero while the slot is free, one while a queue pair holds it. */
        std::int32_t leased = 0;

        /** Raised every time the slot is leased again, so a stale token routes nowhere. */
        queue_pair_generation_t generation = 0;

        /** Requests this queue pair may keep in flight, fixed when the queue pair was taken. */
        queue_depth_t depth = 0;

        /** Commands the device holds for this queue pair, plus the results not yet copied out. */
        std::uint32_t flights_count = 0;

        /** Head of this queue pair's free flight slots, @c no_entry_k when every one is taken. */
        std::uint32_t flights_free = no_entry_k;

        /** Successors of chained requests this queue pair is holding back. */
        std::uint32_t staged_count = 0;

        /** Head of this queue pair's free staging slots, @c no_entry_k when every one is taken. */
        std::uint32_t staged_free = no_entry_k;

        /** Where this queue pair's oldest unreaped local completion sits inside its window. */
        std::uint32_t locals_first = 0;

        /** Completions the disk answered itself, which no command ever carried. */
        std::uint32_t locals_count = 0;

        /** Reaps since the admin queue was last polled that found nothing on the queue pair. */
        std::uint32_t idle_polls = 0;

        /** The queue pair this queue pair owns, which exactly one thread may ever poll. */
        spdk_nvme_qpair* queue = nullptr;
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

    /** One command the device holds, and the result its completion callback writes into it. */
    struct flight_t {

        /** The token the queue pair minted, which the completion carries back unchanged. */
        request_token_t token;

        /** Bytes the extent asked for, which an NVMe command moves whole or not at all. */
        std::uint64_t length = 0;

        /** Bytes the device moved, which the callback fills in. */
        std::uint64_t transferred_bytes = 0;

        /** Why the command ended this way, which the callback fills in. */
        status_t status;

        /** The range the request borrowed, given back when its completion is completed. */
        memory_reference_t buffer;

        /** The target the request borrowed, given back when its completion is completed. */
        medium_reference_t medium;

        /** The staged request waiting on this one, or @c no_entry_k when none does. */
        std::uint32_t successor = no_entry_k;

        /** The next free slot while this one is vacant, which is how the free list is linked. */
        std::uint32_t next_free = no_entry_k;

        /** How the command ended, which the callback fills in. */
        completion_state_t outcome = completion_state_t::completed_k;

        /** Whether the slot is free, holds a command, or holds its result. */
        flight_state_t state = flight_state_t::vacant_k;

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

    /** The index that names no slot, in a free list and in a chain alike. */
    static constexpr std::uint32_t no_entry_k = 0xFFFFFFFFu;

    /** Namespaces one disk registers at once. */
    static constexpr std::uint16_t targets_limit_k = 16;

    /** Requests one queue pair may keep in flight at most, which bounds what a mount may ask for. */
    static constexpr queue_depth_t depth_ceiling_k = 16384;

    /** Completions one drain discards at a time. */
    static constexpr std::size_t drain_batch_k = 32;

    /** Fruitless polls a drain tolerates before it asks the controller to abort what is left. */
    static constexpr std::size_t drain_patience_k = 1024;

    /** Reaps a drain performs before it calls the queue pair broken. */
    static constexpr std::size_t drain_spin_limit_k = std::size_t {1} << 24;

    /** Idle polls of one queue pair between two polls of the admin queue. */
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
        if (config.queue_pairs_limit == 0 || config.depth_limit == 0)
            return {status_code_t::invalid_argument_k, "A ring needs a queue pair and a request of depth"};
        if (config.queue_pairs_limit == static_cast<std::uint16_t>(queue_pair_id_t::unallocated_k) ||
            config.registered_memory_limit == static_cast<std::uint16_t>(registered_memory_id_t::unregistered_k))
            return {status_code_t::invalid_argument_k, "A ring keeps one identifier for an empty handle"};
        if (config.registered_memory_limit == 0)
            return {status_code_t::invalid_argument_k, "A ring needs a range to move bytes through"};
        if (config.depth_limit > depth_ceiling_k)
            return {status_code_t::invalid_argument_k, "A queue pair holds fewer requests than that"};
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
     *  @brief Writes one command's result into the flight it was issued from.
     *
     *  The driver calls this from inside @c spdk_nvme_qpair_process_completions, on the very thread
     *  that holds the queue pair, so nothing here is shared and no atomic is needed.
     */
    static void completed_(void* context, spdk_nvme_cpl const* result) noexcept {
        flight_t& flight = *static_cast<flight_t*>(context);
        if (!spdk_nvme_cpl_is_error(result)) {
            flight.transferred_bytes = flight.length;
            flight.outcome = completion_state_t::completed_k;
            flight.status = {};
            flight.state = flight_state_t::finished_k;
            return;
        }
        flight.transferred_bytes = 0;
        flight.state = flight_state_t::finished_k;
        if (result->status.sct == SPDK_NVME_SCT_GENERIC && result->status.sc == SPDK_NVME_SC_ABORTED_BY_REQUEST) {
            flight.outcome = completion_state_t::cancelled_k;
            flight.status = {status_code_t::uninitialized_k, "The queue_pair cancelled this request"};
            return;
        }
        flight.outcome = completion_state_t::failed_k;
        char const* const spelled = spdk_nvme_cpl_get_status_string_ext(&result->status, flight.opcode);
        flight.status = {status_code_t::device_k, spelled};
    }

    /** Swallows the completion an abort posts for itself, which no queue pair ever asked for. */
    static void aborted_(void*, spdk_nvme_cpl const*) noexcept {}

#pragma endregion Callbacks

#pragma region Bookkeeping

    /** Where one queue pair's flights, staged successors and local completions sit inside the windows. */
    [[nodiscard]] std::uint32_t window_of_(queue_pair_id_t id) const noexcept {
        return static_cast<std::uint32_t>(id) * depth_limit_;
    }

    /** Requests @p slot has outstanding, counting the device's, the disk's own and the held ones. */
    [[nodiscard]] static std::size_t outstanding_(queue_pair_slot_t const& slot) noexcept {
        return slot.flights_count + slot.staged_count + slot.locals_count;
    }

    /** Puts every flight and staging slot of @p seat back on its free lists, oldest index first. */
    void link_free_lists_(std::size_t seat) noexcept {
        std::uint32_t const base = static_cast<std::uint32_t>(seat) * depth_limit_;
        queue_pair_slot_t& slot = queue_pairs_[seat];
        for (std::uint32_t index = 0; index != depth_limit_; ++index) {
            flights_[base + index] = flight_t {};
            flights_[base + index].next_free = index + 1 == depth_limit_ ? no_entry_k : index + 1;
            staged_[base + index] = staged_t {};
            staged_[base + index].next_free = index + 1 == depth_limit_ ? no_entry_k : index + 1;
        }
        slot.flights_free = 0;
        slot.flights_count = 0;
        slot.staged_free = 0;
        slot.staged_count = 0;
    }

    [[nodiscard]] queue_pair_slot_t* slot_of_(queue_pair_t const& queue_pair) noexcept {
        if (!queue_pair.held() || static_cast<std::size_t>(queue_pair.id()) >= queue_pairs_.size()) return nullptr;
        queue_pair_slot_t& slot = queue_pairs_[static_cast<std::size_t>(queue_pair.id())];
        return slot.leased != 0 && slot.generation == queue_pair.generation() ? &slot : nullptr;
    }

    [[nodiscard]] queue_pair_slot_t const* slot_of_(queue_pair_t const& queue_pair) const noexcept {
        if (!queue_pair.held() || static_cast<std::size_t>(queue_pair.id()) >= queue_pairs_.size()) return nullptr;
        queue_pair_slot_t const& slot = queue_pairs_[static_cast<std::size_t>(queue_pair.id())];
        return slot.leased != 0 && slot.generation == queue_pair.generation() ? &slot : nullptr;
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

    /** Marks the range and the target of @p request as live, so neither can be taken back under it. */
    void borrow_(request_t const& request) noexcept {
        if (registered_memory_slot_t* const slot = resolve_(request.buffer); slot)
            in_flight_(*slot).add(1, std::memory_order_acquire);
        if (registered_medium_slot_t* const slot = resolve_(request.medium); slot)
            in_flight_(*slot).add(1, std::memory_order_acquire);
    }

    /** Releases both once the device is done with the bytes the request @p flight remembers. */
    void restore_(flight_t const& flight) noexcept {
        if (registered_memory_slot_t* const slot = resolve_(flight.buffer); slot)
            in_flight_(*slot).sub(1, std::memory_order_release);
        if (registered_medium_slot_t* const slot = resolve_(flight.medium); slot)
            in_flight_(*slot).sub(1, std::memory_order_release);
    }

    /** Takes one flight slot off @p slot's free list, or @c no_entry_k when every one is taken. */
    [[nodiscard]] std::uint32_t take_flight_(queue_pair_slot_t& slot, std::uint32_t base) noexcept {
        std::uint32_t const index = slot.flights_free;
        if (index == no_entry_k) return no_entry_k;
        slot.flights_free = flights_[base + index].next_free;
        ++slot.flights_count;
        return index;
    }

    /** Puts one flight slot back on @p slot's free list, clearing what it held. */
    void free_flight_(queue_pair_slot_t& slot, std::uint32_t base, std::uint32_t index) noexcept {
        flights_[base + index] = flight_t {};
        flights_[base + index].next_free = slot.flights_free;
        slot.flights_free = index;
        --slot.flights_count;
    }

    /** Puts one staging slot back on @p slot's free list, clearing what it held. */
    void free_staged_(queue_pair_slot_t& slot, std::uint32_t base, std::uint32_t index) noexcept {
        staged_[base + index] = staged_t {};
        staged_[base + index].next_free = slot.staged_free;
        slot.staged_free = index;
        --slot.staged_count;
    }

    /** Places @p completion at the back of @p slot's own queue, which poll drains after the device's. */
    void push_local_(queue_pair_slot_t& slot, std::uint32_t base, completion_t const& completion) noexcept {
        locals_[base + (slot.locals_first + slot.locals_count) % slot.depth] = completion;
        ++slot.locals_count;
    }

    /** Moves what the disk answered itself into @p into, oldest first. */
    std::size_t drain_locals_(queue_pair_slot_t& slot, std::uint32_t base, std::span<completion_t> into) noexcept {
        std::size_t const moving = into.size() < slot.locals_count ? into.size() : slot.locals_count;
        for (std::size_t index = 0; index != moving; ++index)
            into[index] = locals_[base + (slot.locals_first + index) % slot.depth];
        slot.locals_first = static_cast<std::uint32_t>((slot.locals_first + moving) % slot.depth);
        slot.locals_count -= static_cast<std::uint32_t>(moving);
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
        if (chunk.size() % portable_alignment_k != 0)
            return {status_code_t::invalid_argument_k, "A DMA range the driver maps spans whole pages"};
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

    /** Polls the admin queue every @c admin_poll_stride_k idle polls of @p slot, where an abort's completion lands. */
    void poll_admin_when_idle_(queue_pair_slot_t& slot) noexcept {
        if (++slot.idle_polls != admin_poll_stride_k) return;
        slot.idle_polls = 0;
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

    /** Whatever the driver would accept and the device would then corrupt, checked before it is issued. */
    status_t inspect_(request_t const& request) noexcept {
        registered_medium_slot_t const* const target = resolve_(request.medium);
        if (!target) return {status_code_t::invalid_argument_k, "The request names no registered target"};
        if ((request.kind == request_kind_t::write_k || request.kind == request_kind_t::deallocate_k) &&
            target->access == medium_access_t::read_only_k)
            return {status_code_t::invalid_argument_k, "The target was registered for reads alone"};
        if (request.kind == request_kind_t::flush_k) return {};
        if (request.kind == request_kind_t::deallocate_k) {
            if (!request.extent.aligned_to(sector_bytes_) ||
                request.extent.length / sector_bytes_ > std::numeric_limits<std::uint32_t>::max())
                return {status_code_t::invalid_argument_k, "A deallocation covers whole sectors, at most one range"};
            if (!request.extent.fits_within(target->capacity))
                return {status_code_t::invalid_argument_k, "The request runs past the namespace"};
            return {};
        }
        registered_memory_slot_t const* const chunk = resolve_(request.buffer);
        if (!chunk) return {status_code_t::invalid_argument_k, "The request names no registered range"};
        if (request.extent.length == 0)
            return {status_code_t::invalid_argument_k, "A transfer of no bytes is not a command"};
        if (request.buffer.offset > chunk->length || request.extent.length > chunk->length - request.buffer.offset)
            return {status_code_t::invalid_argument_k, "The request runs past its registered range"};
        if (request.extent.length > max_transfer_bytes_)
            return {status_code_t::invalid_argument_k, "The request is larger than one transfer"};
        if (!request.extent.aligned_to(sector_bytes_))
            return {status_code_t::invalid_argument_k, "The request does not meet the namespace's sector"};
        if (!request.extent.fits_within(target->capacity))
            return {status_code_t::invalid_argument_k, "The request runs past the namespace"};
        if (!is_address_aligned(chunk->base + request.buffer.offset, portable_alignment_k))
            return {status_code_t::invalid_argument_k, "The payload does not sit on a page boundary"};
        return {};
    }

    /**
     *  @brief Issues @p request on @p slot's queue pair and remembers what its completion will need.
     *  @return The flight slot it occupies, or why the driver would not take it.
     */
    expected<std::uint32_t> start_(queue_pair_slot_t& slot, std::uint32_t base, request_t const& request) noexcept {
        std::uint32_t const index = take_flight_(slot, base);
        if (index == no_entry_k)
            return status_t {status_code_t::capacity_exhausted_k, "This queue pair has no room for another command"};
        flight_t& flight = flights_[base + index];
        flight.token = request.token;
        flight.length = request.kind == request_kind_t::flush_k || request.kind == request_kind_t::deallocate_k
                            ? 0
                            : request.extent.length;
        flight.buffer = request.buffer;
        flight.medium = request.medium;
        flight.successor = no_entry_k;
        flight.state = flight_state_t::outstanding_k;
        borrow_(request);

        spdk_nvme_ns* const space = resolve_(request.medium)->space;
        int issued = 0;
        flight.opcode = request.kind == request_kind_t::flush_k        ? SPDK_NVME_OPC_FLUSH
                        : request.kind == request_kind_t::deallocate_k ? SPDK_NVME_OPC_DATASET_MANAGEMENT
                        : request.kind == request_kind_t::read_k       ? SPDK_NVME_OPC_READ
                                                                       : SPDK_NVME_OPC_WRITE;
        if (request.kind == request_kind_t::flush_k) {
            issued = spdk_nvme_ns_cmd_flush(space, slot.queue, &linux_spdk_disk::completed_, &flight);
        }
        else if (request.kind == request_kind_t::deallocate_k) {
            // The driver copies the range into the command's own payload, so it may live
            // on this stack.
            spdk_nvme_dsm_range range {};
            range.starting_lba = request.extent.offset / sector_bytes_;
            range.length = static_cast<std::uint32_t>(request.extent.length / sector_bytes_);
            issued = spdk_nvme_ns_cmd_dataset_management(space, slot.queue, SPDK_NVME_DSM_ATTR_DEALLOCATE, &range, 1,
                                                         &linux_spdk_disk::completed_, &flight);
        }
        else {
            registered_memory_slot_t const* const chunk = resolve_(request.buffer);
            void* const payload = chunk->base + request.buffer.offset;
            std::uint64_t const first = request.extent.offset / sector_bytes_;
            std::uint32_t const sectors = static_cast<std::uint32_t>(request.extent.length / sector_bytes_);
            issued = request.kind == request_kind_t::read_k
                         ? spdk_nvme_ns_cmd_read(space, slot.queue, payload, first, sectors,
                                                 &linux_spdk_disk::completed_, &flight, 0)
                         : spdk_nvme_ns_cmd_write(space, slot.queue, payload, first, sectors,
                                                  &linux_spdk_disk::completed_, &flight, 0);
        }
        if (issued == 0) return index;
        restore_(flight);
        free_flight_(slot, base, index);
        if (issued == -ENOMEM)
            return status_t {status_code_t::capacity_exhausted_k, "The queue pair has no room for another command"};
        return status_t {status_code_t::device_k, "The driver would not take this command"};
    }

    /** Holds @p request back until the chain reaches it, answering the staging slot it occupies. */
    expected<std::uint32_t> hold_(queue_pair_slot_t& slot, std::uint32_t base, request_t const& request) noexcept {
        std::uint32_t const index = slot.staged_free;
        if (index == no_entry_k)
            return status_t {status_code_t::capacity_exhausted_k, "This queue pair has no room to hold a chain"};
        slot.staged_free = staged_[base + index].next_free;
        ++slot.staged_count;
        staged_[base + index].request = request;
        staged_[base + index].next = no_entry_k;
        return index;
    }

    /** Answers every request of the chain from @p first with @c cancelled_k, since it was cut. */
    void cut_chain_(queue_pair_slot_t& slot, std::uint32_t base, std::uint32_t first) noexcept {
        for (std::uint32_t entry = first; entry != no_entry_k;) {
            std::uint32_t const next = staged_[base + entry].next;
            push_local_(slot, base, cancelled_completion_(staged_[base + entry].request.token));
            free_staged_(slot, base, entry);
            entry = next;
        }
    }

    /** Issues the staged request @p entry now that its predecessor transferred in full. */
    void resume_(queue_pair_slot_t& slot, std::uint32_t base, std::uint32_t entry) noexcept {
        request_t const request = staged_[base + entry].request;
        std::uint32_t const next = staged_[base + entry].next;
        free_staged_(slot, base, entry);
        status_t const defect = inspect_(request);
        if (defect.failed()) {
            push_local_(slot, base, failed_completion_(request.token, defect));
            cut_chain_(slot, base, next);
            return;
        }
        auto [started, start_status] = start_(slot, base, request);
        if (start_status.failed()) {
            push_local_(slot, base, failed_completion_(request.token, start_status));
            cut_chain_(slot, base, next);
            return;
        }
        flights_[base + started].successor = next;
    }

    /** Copies every finished command of @p slot into @p into, resuming or cutting the chains they head. */
    std::size_t poll_completions_(queue_pair_slot_t& slot, std::uint32_t base, std::span<completion_t> into) noexcept {
        std::size_t moved = 0;
        for (std::uint32_t index = 0; index != depth_limit_ && moved != into.size(); ++index) {
            flight_t& flight = flights_[base + index];
            if (flight.state != flight_state_t::finished_k) continue;
            completion_t completion;
            completion.token = flight.token;
            completion.transferred_bytes = flight.transferred_bytes;
            completion.status = flight.status;
            completion.state = flight.outcome;
            std::uint32_t const successor = flight.successor;
            restore_(flight);
            free_flight_(slot, base, index);
            into[moved] = completion;
            ++moved;
            if (successor == no_entry_k) continue;
            if (completion.state == completion_state_t::completed_k) resume_(slot, base, successor);
            else cut_chain_(slot, base, successor);
        }
        return moved;
    }

#pragma endregion Requests

  public:

    /**
     *  @brief Frees every queue pair, unmaps every range, and gives back every namespace and the
     *      disk's share of the controller, detaching it after the last, before the disk dies.
     *  @note It waits on the process's claims, so the mount's unmount calls it, never a destructor.
     */
    void release() noexcept {
        for (std::size_t seat = 0; seat != queue_pairs_.size(); ++seat) {
            if (!queue_pairs_[seat].queue) continue;
            spdk_nvme_ctrlr_free_io_qpair(queue_pairs_[seat].queue);
            queue_pairs_[seat].queue = nullptr;
        }
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
        queue_pairs_.clear();
        registered_memories_.clear();
        media_.clear();
    }

  private:

    /** The environment this disk holds a share of, declared first so it outlives everything below. */
    linux_spdk_environment<atomic_reference_, waiter_type_> environment_;

    /** One slot per queue pair the disk hands out, which is one queue pair each. */
    st::vector<queue_pair_slot_t> queue_pairs_;

    /** One slot per memory range the driver has mapped for this disk. */
    st::vector<registered_memory_slot_t> registered_memories_;

    /** The namespaces registered as targets, at most @c targets_limit_k of them. */
    st::vector<registered_medium_slot_t> media_;

    /** Every queue pair's commands, in windows of @c depth_limit_ addressed through a free list. */
    st::vector<flight_t> flights_;

    /** Every queue pair's held successors, in windows of @c depth_limit_ addressed through a free list. */
    st::vector<staged_t> staged_;

    /** Every queue pair's own completions, in windows of @c depth_limit_ read as a circle. */
    st::vector<completion_t> locals_;

    /** The process's connection to the controller, whose share this disk gives back at close. */
    linux_spdk_attachment_t* attachment_ = nullptr;

    /** The controller of @c attachment_. */
    spdk_nvme_ctrlr* controller_ = nullptr;

    /** Bytes one sector spans, which every offset and every length must be a multiple of. */
    std::uint64_t sector_bytes_ = 0;

    /** The largest extent one command carries, as the namespace reports it. */
    std::uint64_t max_transfer_bytes_ = 0;

    /** Requests one queue pair may keep in flight, and the stride of every queue pair's window. */
    queue_depth_t depth_limit_ = 0;

    /** Leases the disk hands out, lowered at mount to the queue pairs the controller offers. */
    std::uint16_t queue_pairs_limit_ = 0;

    /** Memory ranges the disk maps at once. */
    std::uint16_t registered_memories_limit_ = 0;

    /** How the controller reaches a registered range, fixed by the transport the mount connected over. */
    reach_t reach_ = reach_t::translated_k;
};

#pragma endregion Disk

#else

#pragma region Absent

/**
 *  @brief The same disk in a build that carries no SPDK, which mounts nowhere and refuses cleanly.
 *
 *  The type exists so a caller compiles against it unconditionally and chooses at mount, where a
 *  refused @c open sends it to another transport rather than to a build-time branch. The probe is
 *  not here, because it belongs to the environment and must answer honestly in every build.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class linux_spdk_disk {
  public:

    /** Refuses the mount, since this build carries no driver to mount with. */
    static expected<linux_spdk_disk> open(linux_spdk_environment<atomic_reference_, waiter_type_>, char const*,
                                          linux_spdk_disk_config_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This build carries no SPDK"};
    }

    /** What this disk would accept, which is nothing at all. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept { return {}; }

    /** What the controller identifies as, which is nothing, since none was ever connected. */
    [[nodiscard]] linux_spdk_controller_identity_t identity() const noexcept { return {}; }

    /** Refuses the queue pair, since there is no queue pair to queue pair. */
    expected<queue_pair_t> acquire_queue_pair(std::uint32_t) noexcept {
        return status_t {status_code_t::unsupported_k, "This build carries no SPDK"};
    }

    /** Refuses the handover, since no queue pair was ever handed out. */
    status_t bind_queue_pair(queue_pair_t const&) noexcept {
        return {status_code_t::unsupported_k, "This build carries no SPDK"};
    }

    /** Refuses the return, since no queue pair was ever handed out. */
    status_t release_queue_pair(queue_pair_t&) noexcept {
        return {status_code_t::unsupported_k, "This build carries no SPDK"};
    }

    /** Refuses the range, since no driver would ever map it. */
    expected<registered_memory_t> register_memory(std::span<byte_t>, memory_space_t) noexcept {
        return status_t {status_code_t::unsupported_k, "This build carries no SPDK"};
    }

    /** Refuses the range, since none was ever mapped. */
    status_t unregister_memory(registered_memory_t&) noexcept {
        return {status_code_t::unsupported_k, "This build carries no SPDK"};
    }

    /** Refuses the target, since there is no namespace to present. */
    expected<registered_medium_t> register_medium(medium_options_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This build carries no SPDK"};
    }

    /** Refuses the target, since none was ever registered. */
    status_t unregister_medium(registered_medium_t&) noexcept {
        return {status_code_t::unsupported_k, "This build carries no SPDK"};
    }

    /** Takes nothing, since there is no queue pair to take it into. */
    expected<std::size_t> submit(queue_pair_t const&, std::span<request_t const>) noexcept {
        return status_t {status_code_t::unsupported_k, "This build carries no SPDK"};
    }

    /** Answers nothing, since nothing was ever submitted. */
    expected<std::size_t> poll_completions(queue_pair_t const&, std::span<completion_t>) noexcept {
        return status_t {status_code_t::unsupported_k, "This build carries no SPDK"};
    }

    /** Stops nothing, since nothing was ever submitted. */
    void cancel(queue_pair_t const&) noexcept {}

    /** Refuses the drain, since no queue pair was ever handed out. */
    status_t drain(queue_pair_t const&) noexcept {
        return {status_code_t::unsupported_k, "This build carries no SPDK"};
    }

    /** Answers that nothing is outstanding, which is always true here. */
    [[nodiscard]] std::size_t outstanding(queue_pair_t const&) const noexcept { return 0; }

    /** Gives back nothing, since nothing was ever taken. */
    void release() noexcept {}
};

#pragma endregion Absent

#endif

} // namespace unum::ustore::utilities
