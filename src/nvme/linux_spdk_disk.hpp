/**
 *  @file src/nvme/linux_spdk_disk.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The ring SPDK offers: a user-space NVMe driver over one namespace, one queue pair per queue pair.
 *
 *  This transport presents exactly one target, the namespace it was mounted on, and keeps no name
 *  mapping of its own; the catalog above it names what the extents hold, so a torn write of a map
 *  cannot lose a drive. Nothing here blocks, because the driver offers no interrupt at all:
 *  @c spdk_nvme_qpair_process_completions is the only way a command is ever seen to finish.
 *
 *  A queue pair is a queue pair rather than a label, since an @c spdk_nvme_qpair carries no lock and a
 *  second thread polling one corrupts its tracker list without a word. @c queue_pairs_limit is therefore
 *  the controller's queue-pair count, which may be fewer than the executor has threads, and a
 *  handover is a move of the queue pair announced with @c adopt.
 *
 *  The environment is the hazard the rest of the file is arranged around. @c rte_eal_init calls
 *  @c exit on the process when it wants hugepages and finds none, when the drive is still bound to
 *  the kernel @c nvme driver, or when @c /dev/vfio cannot be opened, so a failed mount would take the
 *  database down with it. @c spdk_environment_t::probe therefore decides by reading @c /proc/meminfo,
 *  @c /sys/kernel/mm/hugepages and the driver link under @c /sys/bus/pci/devices, and never
 *  initialises anything; the environment is acquired only once the probe says it can be. A fabric
 *  address has none of those hazards, since the driver copies every byte through a socket and never
 *  translates a page, so its environment runs on plain pages and the probe passes it at once.
 */

#pragma once

#include <cassert> // `assert`
#include <cerrno>  // `errno`, `EINTR`, `ENOMEM`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstdio>  // `std::snprintf`

#if defined(__linux__)
#include <fcntl.h>  // `O_RDONLY`, `O_CLOEXEC`
#include <unistd.h> // `read`, `readlink`, `access`, `close`
#endif

#include <array>       // `std::array`
#include <atomic>      // `std::memory_order`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`

#if USTORE_WITH_SPDK
#include <spdk/env.h>      // `spdk_env_init`, `spdk_mem_register`, `spdk_vtophys`
#include <spdk/env_dpdk.h> // `spdk_env_dpdk_external_init`
#include <spdk/nvme.h>     // `spdk_nvme_connect`, `spdk_nvme_ns_cmd_read`, `spdk_nvme_qpair`
#endif

#include <forkunion/atomics.hpp>       // `fu::preferred_atomic_ref`
#include <forkunion/capabilities.hpp>  // `fu::preferred_yield_t`
#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/disk.hpp"   // `calling_thread_identity`
#include "nvme/extent.hpp" // `extent_t`, `portable_alignment_k`
#include "nvme/ring.hpp"   // `request_t`, `completion_t`, `queue_pair_t`

namespace unum::ustore::nvme {

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
enum class spdk_transport_t : std::uint8_t {

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
[[nodiscard]] constexpr spdk_transport_t spdk_transport_named(std::string_view address) noexcept {
    std::string_view const kind = transport_field(address, "trtype:");
    if (kind.empty() || (kind.size() == 4 && starts_with_uncased(kind, "pcie"))) return spdk_transport_t::pcie_k;
    return spdk_transport_t::fabric_k;
}

/**
 *  @brief The number on the @p key line of a @c /proc/meminfo-shaped @p text, or zero when it has none.
 *  @param[in] text The whole file, or any text shaped like it, with lines of the form `Key: value`.
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
 *  @brief Names the driver a @c /sys/bus/pci/devices/<address>/driver link resolves to.
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
        expected<std::uint64_t> const parsed = hexadecimal_field(rest.substr(0, separator), 4);
        if (!parsed) return parsed.status();
        domain = *parsed;
        rest.remove_prefix(separator + 1);
    }

    std::size_t const separator = rest.find(':');
    expected<std::uint64_t> const bus = hexadecimal_field(rest.substr(0, separator), 2);
    if (!bus) return bus.status();
    rest.remove_prefix(separator + 1);

    std::size_t const dot = rest.find('.');
    if (dot == std::string_view::npos)
        return status_t {status_code_t::invalid_argument_k, "A PCIe address separates its function with a dot"};
    expected<std::uint64_t> const device = hexadecimal_field(rest.substr(0, dot), 2);
    if (!device) return device.status();
    expected<std::uint64_t> const function = hexadecimal_field(rest.substr(dot + 1), 1);
    if (!function) return function.status();
    if (*device > 0x1F || *function > 7)
        return status_t {status_code_t::invalid_argument_k, "A PCIe device is below 32 and a function below 8"};

    pci_address_t address;
    [[maybe_unused]] int const written = std::snprintf(
        address.text, sizeof(address.text), "%04x:%02x:%02x.%01x", static_cast<unsigned>(domain),
        static_cast<unsigned>(*bus), static_cast<unsigned>(*device), static_cast<unsigned>(*function));
    return address;
}

#pragma endregion Probe

#pragma region Files

#if defined(__linux__)

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
enum class spdk_memory_t : std::uint8_t {

    /** Hugepages the kernel reserved, which a PCIe controller's DMA needs and the probe checks for. */
    hugepages_k,

    /** Plain pages of a fixed size, which suffice for a fabric the socket copies through, on any machine. */
    plain_pages_k,
};

/** What an environment is initialised with, decided by the transport the mount is for. */
struct spdk_environment_config_t {

    /** Whether the driver gets hugepages and scans PCIe, or plain pages and no bus at all. */
    spdk_memory_t memory = spdk_memory_t::plain_pages_k;

    /** Megabytes of plain pages the driver takes at once, which it refuses to start without. */
    std::uint32_t plain_megabytes = 128;
};

/** What the controller says it is, as identify reports it, with the padding the spec allows trimmed. */
struct spdk_controller_identity_t {

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
 *  The handle is counted by value: copies share one initialisation and the last one to die tears it
 *  down, so a mount takes it, hands copies to its disks and forgets about the order they close in.
 *  Nothing here is process-global; the count lives in a block the first handle allocates. The driver
 *  itself is, and initialises once per process, which is why a second, independent @c acquire is
 *  refused rather than counted.
 *
 *  @note @c probe is the only member that is safe to call before anything is initialised, and it is
 *      the one a mount must call, since @c acquire reaches @c rte_eal_init and that calls
 *      @c exit on the whole process when the machine is not ready for it.
 */
class spdk_environment_t {
  public:

    spdk_environment_t() noexcept = default;

    spdk_environment_t(spdk_environment_t const& other) noexcept : holders_(other.holders_) {
        if (holders_) count_(*holders_).add(1, std::memory_order_relaxed);
    }

    spdk_environment_t& operator=(spdk_environment_t const& other) noexcept {
        if (this == &other) return *this;
        forget_();
        holders_ = other.holders_;
        if (holders_) count_(*holders_).add(1, std::memory_order_relaxed);
        return *this;
    }

    spdk_environment_t(spdk_environment_t&& other) noexcept : holders_(std::exchange(other.holders_, nullptr)) {}

    spdk_environment_t& operator=(spdk_environment_t&& other) noexcept {
        if (this == &other) return *this;
        forget_();
        holders_ = std::exchange(other.holders_, nullptr);
        return *this;
    }

    ~spdk_environment_t() noexcept { forget_(); }

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
     *  @brief Initialises the environment, which only a probe that answered @c available_k permits.
     *  @param[in] config Hugepages for a PCIe controller, or plain pages of a given size for a fabric.
     *  @return A counted handle, or why the driver would not start.
     *
     *  @note The driver initialises once per process: a handle acquired after the last one of an
     *      earlier environment died is refused, since DPDK keeps its state until the process exits.
     */
    static expected<spdk_environment_t> acquire(spdk_environment_config_t const& config) noexcept;

    /** Whether this handle holds a share of an initialised environment. */
    [[nodiscard]] bool initialised() const noexcept { return holders_ != nullptr; }

  private:

    /** The block the counted handles share, allocated by the first of them and freed by the last. */
    struct holders_t {

        /** Handles that still hold this environment, one when the block is first allocated. */
        std::int32_t count = 1;
    };

    /** Shuts the driver down, which only the last handle of an environment reaches. */
    static void finalize_() noexcept;

    /** The counter of @p block as the atomic it is read and written through. */
    [[nodiscard]] static fu::preferred_atomic_ref<std::int32_t> count_(holders_t& block) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {block.count};
    }

    /** Drops this handle's share, tearing the environment down when it was the last one. */
    void forget_() noexcept {
        if (!holders_) return;
        if (count_(*holders_).fetch_sub(1, std::memory_order_acq_rel) == 1) {
            finalize_();
            delete holders_;
        }
        holders_ = nullptr;
    }

    /** The shared count, @c nullptr in a handle that holds no environment. */
    holders_t* holders_ = nullptr;
};

#if defined(__linux__)

/** Bytes of @c /proc/meminfo the probe reads, which is more than the file has ever held. */
inline constexpr std::size_t meminfo_bytes_k = 8192;

/** Bytes of a sysfs driver link the probe reads, which is more than such a path has ever spanned. */
inline constexpr std::size_t link_bytes_k = 512;

inline linux_spdk_probe_t spdk_environment_t::probe(char const* address) noexcept {
    if (!address) return linux_spdk_probe_t::not_found_k;
    if (spdk_transport_named(address) == spdk_transport_t::fabric_k) return linux_spdk_probe_t::available_k;
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

inline linux_spdk_probe_t spdk_environment_t::probe(char const* address) noexcept {
    if (!address) return linux_spdk_probe_t::not_found_k;
    return spdk_transport_named(address) == spdk_transport_t::fabric_k ? linux_spdk_probe_t::available_k
                                                                       : linux_spdk_probe_t::not_found_k;
}

#endif

#if USTORE_WITH_SPDK

inline expected<spdk_environment_t> spdk_environment_t::acquire(spdk_environment_config_t const& config) noexcept {
    bool const plain = config.memory == spdk_memory_t::plain_pages_k;
    if (plain && config.plain_megabytes == 0)
        return status_t {status_code_t::invalid_argument_k, "Plain pages need a size the driver takes at once"};
    if (!spdk_env_dpdk_external_init())
        return status_t {status_code_t::unsupported_k, "The driver initialises its environment once per process"};

    spdk_env_opts options {};
    options.opts_size = sizeof(options);
    spdk_env_opts_init(&options);
    options.name = "ustore";
    options.no_huge = plain;
    options.no_pci = plain;
    if (plain) options.mem_size = static_cast<int>(config.plain_megabytes);
    if (spdk_env_init(&options) != 0)
        return status_t {status_code_t::device_k, "The driver would not initialise its environment"};
    spdk_unaffinitize_thread();
    spdk_environment_t environment;
    environment.holders_ = new (std::nothrow) holders_t {};
    if (!environment.holders_) {
        spdk_env_fini();
        return status_t {status_code_t::out_of_memory_k, "The environment would not record its holders"};
    }
    return environment;
}

inline void spdk_environment_t::finalize_() noexcept { spdk_env_fini(); }

#else

inline expected<spdk_environment_t> spdk_environment_t::acquire(spdk_environment_config_t const&) noexcept {
    return status_t {status_code_t::unsupported_k, "This build carries no SPDK"};
}

inline void spdk_environment_t::finalize_() noexcept {}

#endif

#pragma endregion Environment

#if USTORE_WITH_SPDK

#pragma region Disk

/**
 *  @brief The ring SPDK offers, over the one namespace the mount connected to.
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
 */
class linux_spdk_disk_t {
  public:

    linux_spdk_disk_t() noexcept = default;
    linux_spdk_disk_t(linux_spdk_disk_t const&) = delete;
    linux_spdk_disk_t& operator=(linux_spdk_disk_t const&) = delete;

    linux_spdk_disk_t(linux_spdk_disk_t&& other) noexcept
        : environment_(std::move(other.environment_)), queue_pairs_(std::move(other.queue_pairs_)),
          registered_memories_(std::move(other.registered_memories_)), media_(std::move(other.media_)),
          flights_(std::move(other.flights_)), staged_(std::move(other.staged_)), locals_(std::move(other.locals_)),
          controller_(std::exchange(other.controller_, nullptr)), namespace_(std::exchange(other.namespace_, nullptr)),
          namespace_bytes_(std::exchange(other.namespace_bytes_, 0)),
          sector_bytes_(std::exchange(other.sector_bytes_, 0)),
          max_transfer_bytes_(std::exchange(other.max_transfer_bytes_, 0)),
          depth_limit_(std::exchange(other.depth_limit_, 0)),
          queue_pairs_limit_(std::exchange(other.queue_pairs_limit_, 0)),
          registered_memories_limit_(std::exchange(other.registered_memories_limit_, 0)), reach_(other.reach_) {}

    linux_spdk_disk_t& operator=(linux_spdk_disk_t&& other) noexcept {
        if (this == &other) return *this;
        close_all_();
        environment_ = std::move(other.environment_);
        queue_pairs_ = std::move(other.queue_pairs_);
        registered_memories_ = std::move(other.registered_memories_);
        media_ = std::move(other.media_);
        flights_ = std::move(other.flights_);
        staged_ = std::move(other.staged_);
        locals_ = std::move(other.locals_);
        controller_ = std::exchange(other.controller_, nullptr);
        namespace_ = std::exchange(other.namespace_, nullptr);
        namespace_bytes_ = std::exchange(other.namespace_bytes_, 0);
        sector_bytes_ = std::exchange(other.sector_bytes_, 0);
        max_transfer_bytes_ = std::exchange(other.max_transfer_bytes_, 0);
        depth_limit_ = std::exchange(other.depth_limit_, 0);
        queue_pairs_limit_ = std::exchange(other.queue_pairs_limit_, 0);
        registered_memories_limit_ = std::exchange(other.registered_memories_limit_, 0);
        reach_ = other.reach_;
        return *this;
    }

    ~linux_spdk_disk_t() noexcept { close_all_(); }

    /**
     *  @brief Connects @p address, takes its first active namespace, and sizes the disk's bookkeeping.
     *  @param[in] environment A counted share of an initialised environment, kept for the disk's life.
     *  @param[in] address The PCIe address of the controller, or a fabric transport ID the driver parses.
     *  @param[in] config The queue pairs wanted, the per-queue pair depth and the ranges registered at once.
     *  @return The mounted disk, or why the controller would not answer.
     */
    static expected<linux_spdk_disk_t> open(spdk_environment_t environment, char const* address,
                                            linux_spdk_disk_config_t const& config) noexcept {
        if (status_t const sane = inspect_config_(config); sane.failed()) return sane;
        if (!address) return status_t {status_code_t::invalid_argument_k, "A controller needs an address"};
        if (!environment.initialised())
            return status_t {status_code_t::invalid_argument_k, "A mount needs an initialised environment"};

        spdk_nvme_transport_id transport {};
        if (status_t const named = transport_of_(address, transport); named.failed()) return named;

        spdk_nvme_ctrlr_opts options {};
        spdk_nvme_ctrlr_get_default_ctrlr_opts(&options, sizeof(options));
        options.num_io_queues = config.queue_pairs_limit;
        options.keep_alive_timeout_ms = 0;
        spdk_nvme_ctrlr* const controller = spdk_nvme_connect(&transport, &options, sizeof(options));
        if (!controller) return status_t {status_code_t::not_found_k, "No controller answered at that address"};

        spdk_nvme_ns* chosen = nullptr;
        for (std::uint32_t identifier = spdk_nvme_ctrlr_get_first_active_ns(controller); identifier != 0;
             identifier = spdk_nvme_ctrlr_get_next_active_ns(controller, identifier)) {
            spdk_nvme_ns* const candidate = spdk_nvme_ctrlr_get_ns(controller, identifier);
            if (!candidate || !spdk_nvme_ns_is_active(candidate)) continue;
            chosen = candidate;
            break;
        }
        if (!chosen) {
            spdk_nvme_detach(controller);
            return status_t {status_code_t::not_found_k, "The controller carries no active namespace"};
        }

        spdk_nvme_ctrlr_opts const* const opts = spdk_nvme_ctrlr_get_opts(controller);
        std::uint32_t const queues = opts && opts->num_io_queues != 0 ? opts->num_io_queues : 1u;
        std::uint16_t const queue_pairs = static_cast<std::uint16_t>(
            config.queue_pairs_limit < queues ? config.queue_pairs_limit : queues);

        linux_spdk_disk_t disk;
        disk.environment_ = std::move(environment);
        disk.controller_ = controller;
        disk.namespace_ = chosen;
        disk.namespace_bytes_ = spdk_nvme_ns_get_size(chosen);
        disk.sector_bytes_ = spdk_nvme_ns_get_sector_size(chosen);
        disk.max_transfer_bytes_ = spdk_nvme_ns_get_max_io_xfer_size(chosen);
        disk.depth_limit_ = config.depth_limit;
        disk.queue_pairs_limit_ = queue_pairs;
        disk.registered_memories_limit_ = config.registered_memory_limit;
        disk.reach_ = transport.trtype == SPDK_NVME_TRANSPORT_TCP ? reach_t::copied_k : reach_t::translated_k;
        if (disk.sector_bytes_ == 0 || !is_power_of_two(disk.sector_bytes_))
            return status_t {status_code_t::unsupported_k, "The namespace reports no power-of-two sector"};

        std::size_t const window = static_cast<std::size_t>(queue_pairs) * config.depth_limit;
        if (status_t const sized = size_(disk.queue_pairs_, queue_pairs); sized.failed()) return sized;
        if (status_t const sized = size_(disk.registered_memories_, config.registered_memory_limit); sized.failed())
            return sized;
        if (status_t const sized = size_(disk.media_, targets_limit_k); sized.failed()) return sized;
        if (status_t const sized = size_(disk.flights_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.staged_, window); sized.failed()) return sized;
        if (status_t const sized = size_(disk.locals_, window); sized.failed()) return sized;
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
    [[nodiscard]] spdk_controller_identity_t identity() const noexcept {
        spdk_controller_identity_t identity;
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
            slot.bound_thread = calling_thread_identity();
            link_free_lists_(seat);
            return queue_pair_t::leased(static_cast<queue_pair_id_t>(seat), slot.generation, depth);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every queue pair of this ring is already held"};
    }

    /**
     *  @brief Makes the calling thread the one that polls @p queue pair.
     *  @param[in] queue pair The queue pair this thread has just taken over, with nothing outstanding on it.
     *
     *  @note A queue pair carries no lock, so a second thread polling one corrupts its tracker list
     *      in silence; that is why a handover is announced and why the queue pair must be empty first.
     */
    status_t bind_queue_pair(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        if (outstanding_(*slot) != 0)
            return {status_code_t::invalid_argument_k, "Drain the queue pair before handing it over"};
        slot->bound_thread = calling_thread_identity();
        return {};
    }

    /**
     *  @brief Frees @p queue pair once it has nothing outstanding, and empties the handle.
     *  @param[inout] queue pair The queue pair to return, surrendered on success and untouched on failure.
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
        slot->bound_thread = 0;
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
            return registered_memory_t::registered(static_cast<registered_memory_id_t>(index), slot.generation, chunk,
                                                   space);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every registered_memory slot of this ring is taken"};
    }

    /**
     *  @brief Gives @p registered memory back, refusing while it is still a target of requests in flight.
     *  @param[inout] registered memory The range to give back, surrendered on success.
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
     *  @brief Registers the namespace, which is the one target this transport ever presents.
     *  @param[in] request Only its access is read; the name and the length a namespace cannot honour.
     *  @return The handle, or @c capacity_exhausted_k when the namespace is already registered.
     */
    expected<registered_medium_t> register_medium(medium_options_t const& request) noexcept {
        registered_medium_slot_t& slot = media_[0];
        std::int32_t expected_free = 0;
        if (!claim_(slot.registered)
                 .compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
            return status_t {status_code_t::capacity_exhausted_k, "The namespace is already registered"};
        slot.access = request.access == medium_access_t::read_only_k ? medium_access_t::read_only_k
                                                                     : medium_access_t::read_write_k;
        slot.capacity = namespace_bytes_;
        return registered_medium_t::registered(static_cast<registered_medium_id_t>(0), slot.generation, slot.capacity);
    }

    /**
     *  @brief Gives the namespace back, refusing while requests against it are still in flight.
     *  @param[inout] descriptor The target to give back, surrendered on success.
     */
    status_t unregister_medium(registered_medium_t& descriptor) noexcept {
        registered_medium_slot_t* const slot = slot_of_(descriptor);
        if (!slot) return {status_code_t::invalid_argument_k, "The target is not this ring's"};
        if (in_flight_(*slot).load(std::memory_order_acquire) != 0)
            return {status_code_t::invalid_argument_k, "The target is still named by requests in flight"};
        ++slot->generation;
        slot->capacity = 0;
        claim_(slot->registered).store(0, std::memory_order_release);
        descriptor.surrender();
        return {};
    }

#pragma endregion Registrations

#pragma region Ring

    /**
     *  @brief Takes as much of @p requests as @p queue pair still has credit for, and never waits.
     *  @param[in] queue pair The queue pair submitting, held by the calling thread.
     *  @param[in] requests The batch, of which a prefix is taken.
     *  @return How many were taken, which is zero on a full queue and never a reason to block.
     */
    expected<std::size_t> submit(queue_pair_t const& queue_pair, std::span<request_t const> requests) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        assert(slot->bound_thread == calling_thread_identity() && "Only the thread holding a queue pair submits on it");
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
                expected<std::uint32_t> const entry = hold_(*slot, base, request);
                if (!entry) {
                    push_local_(*slot, base, failed_completion_(request.token, entry.status()));
                    chaining = chains_next_(request) ? chaining_state_t::cut_k : chaining_state_t::open_k;
                    continue;
                }
                if (chain_staged == no_entry_k) flights_[base + chain_flight].successor = *entry;
                else staged_[base + chain_staged].next = *entry;
                chain_staged = *entry;
                chaining = chains_next_(request) ? chaining_state_t::held_k : chaining_state_t::open_k;
                continue;
            }
            status_t const defect = inspect_(request);
            if (defect.failed()) {
                push_local_(*slot, base, failed_completion_(request.token, defect));
                chaining = chains_next_(request) ? chaining_state_t::cut_k : chaining_state_t::open_k;
                continue;
            }
            expected<std::uint32_t> const started = start_(*slot, base, request);
            if (!started) {
                if (started.status() == status_code_t::capacity_exhausted_k) break;
                push_local_(*slot, base, failed_completion_(request.token, started.status()));
                chaining = chains_next_(request) ? chaining_state_t::cut_k : chaining_state_t::open_k;
                continue;
            }
            chain_flight = *started;
            chain_staged = no_entry_k;
            chaining = chains_next_(request) ? chaining_state_t::held_k : chaining_state_t::open_k;
        }
        return taken;
    }

    /**
     *  @brief Polls @p queue pair and copies what finished into @p into.
     *  @param[in] queue pair The queue pair polling, held by the calling thread that submitted on it.
     *  @param[out] into Room for completions, which bounds how many one call moves.
     *  @return How many completions were moved, which is zero when nothing has landed.
     *
     *  @note Polling is the only mechanism the driver has, which is why nothing here waits and why
     *      a bounded wait is a spin over this call rather than a blocking one.
     */
    expected<std::size_t> poll_completions(queue_pair_t const& queue_pair, std::span<completion_t> into) noexcept {
        queue_pair_slot_t* const slot = slot_of_(queue_pair);
        if (!slot) return status_t {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        assert(slot->bound_thread == calling_thread_identity() && "Only the thread holding a queue pair polls on it");
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
     *  @param[in] queue pair The queue pair to stop, whose started commands still run to their own end.
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
                                                                             &linux_spdk_disk_t::aborted_, nullptr);
        }
    }

    /**
     *  @brief Finishes and discards everything @p queue pair has outstanding, so its buffers may be freed.
     *  @param[in] queue pair The queue pair to empty, which may be cancelled or still running.
     */
    status_t drain(queue_pair_t const& queue_pair) noexcept {
        queue_pair_slot_t const* const slot = slot_of_(queue_pair);
        if (!slot) return {status_code_t::invalid_argument_k, "The queue pair is not this ring's"};
        std::array<completion_t, drain_batch_k> discarded {};
        for (std::size_t spins = 0; spins != drain_spin_limit_k; ++spins) {
            if (outstanding_(*slot) == 0) return {};
            expected<std::size_t> const moved = poll_completions(queue_pair, std::span<completion_t> {discarded});
            if (!moved) return moved.status();
            if (*moved != 0) continue;
            if (spins == drain_patience_k) cancel(queue_pair);
            fu::preferred_yield_t {}(spins, spins, std::size_t {0});
        }
        return {status_code_t::input_output_k, "The queue pair would not drain"};
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

        /** The thread that took the queue pair, which every submit and poll is checked against. */
        thread_identity_t bound_thread = 0;

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

    /** The namespace as a registered target, and the count of commands still naming it. */
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
        medium_reference_t target;

        /** The staged request waiting on this one, or @c no_entry_k when none does. */
        std::uint32_t successor = no_entry_k;

        /** The next free slot while this one is vacant, which is how the free list is linked. */
        std::uint32_t next_free = no_entry_k;

        /** How the command ended, which the callback fills in. */
        completion_state_t outcome = completion_state_t::completed_k;

        /** Whether the slot is free, holds a command, or holds its result. */
        flight_state_t state = flight_state_t::vacant_k;
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

    /** Targets this transport registers, which is the one namespace it was mounted on. */
    static constexpr std::uint16_t targets_limit_k = 1;

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
    static status_t size_(st::basic_vector<element_type_>& elements, std::size_t count) noexcept {
        if (elements.resize(count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A ring could not allocate its slots"};
        return {};
    }

    static fu::preferred_atomic_ref<std::int32_t> claim_(std::int32_t& word) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {word};
    }

    static fu::preferred_atomic_ref<std::int32_t> in_flight_(registered_memory_slot_t& slot) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {slot.in_flight};
    }

    static fu::preferred_atomic_ref<std::int32_t> in_flight_(registered_medium_slot_t& slot) noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {slot.in_flight};
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
        flight.status = {status_code_t::device_k, spdk_nvme_cpl_get_status_string(&result->status)};
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
        if (request.kind == request_kind_t::write_k && target->access == medium_access_t::read_only_k)
            return {status_code_t::invalid_argument_k, "The target was registered for reads alone"};
        if (request.kind == request_kind_t::flush_k) return {};
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
        flight.length = request.kind == request_kind_t::flush_k ? 0 : request.extent.length;
        flight.buffer = request.buffer;
        flight.medium = request.medium;
        flight.successor = no_entry_k;
        flight.state = flight_state_t::outstanding_k;
        borrow_(request);

        int issued = 0;
        if (request.kind == request_kind_t::flush_k) {
            issued = spdk_nvme_ns_cmd_flush(namespace_, slot.queue, &linux_spdk_disk_t::completed_, &flight);
        }
        else {
            registered_memory_slot_t const* const chunk = resolve_(request.buffer);
            void* const payload = chunk->base + request.buffer.offset;
            std::uint64_t const first = request.extent.offset / sector_bytes_;
            std::uint32_t const sectors = static_cast<std::uint32_t>(request.extent.length / sector_bytes_);
            issued = request.kind == request_kind_t::read_k
                         ? spdk_nvme_ns_cmd_read(namespace_, slot.queue, payload, first, sectors,
                                                 &linux_spdk_disk_t::completed_, &flight, 0)
                         : spdk_nvme_ns_cmd_write(namespace_, slot.queue, payload, first, sectors,
                                                  &linux_spdk_disk_t::completed_, &flight, 0);
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
        expected<std::uint32_t> const started = start_(slot, base, request);
        if (!started) {
            push_local_(slot, base, failed_completion_(request.token, started.status()));
            cut_chain_(slot, base, next);
            return;
        }
        flights_[base + *started].successor = next;
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

    /** Frees every queue pair, unmaps every range, and detaches the controller. */
    void close_all_() noexcept {
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
        if (controller_) spdk_nvme_detach(controller_);
        controller_ = nullptr;
        namespace_ = nullptr;
        queue_pairs_.clear();
        registered_memories_.clear();
    }

    /** The environment this disk holds a share of, declared first so it outlives everything below. */
    spdk_environment_t environment_;

    /** One slot per queue pair the disk hands out, which is one queue pair each. */
    st::basic_vector<queue_pair_slot_t> queue_pairs_;

    /** One slot per memory range the driver has mapped for this disk. */
    st::basic_vector<registered_memory_slot_t> registered_memories_;

    /** The namespace as a target, which is the only one this transport ever presents. */
    st::basic_vector<registered_medium_slot_t> media_;

    /** Every queue pair's commands, in windows of @c depth_limit_ addressed through a free list. */
    st::basic_vector<flight_t> flights_;

    /** Every queue pair's held successors, in windows of @c depth_limit_ addressed through a free list. */
    st::basic_vector<staged_t> staged_;

    /** Every queue pair's own completions, in windows of @c depth_limit_ read as a circle. */
    st::basic_vector<completion_t> locals_;

    /** The controller this disk connected to, detached when the disk closes. */
    spdk_nvme_ctrlr* controller_ = nullptr;

    /** The namespace every request moves bytes against. */
    spdk_nvme_ns* namespace_ = nullptr;

    /** Bytes the namespace spans, which is the capacity its target reports. */
    std::uint64_t namespace_bytes_ = 0;

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
class linux_spdk_disk_t {
  public:

    /** Refuses the mount, since this build carries no driver to mount with. */
    static expected<linux_spdk_disk_t> open(spdk_environment_t, char const*, linux_spdk_disk_config_t const&) noexcept {
        return status_t {status_code_t::unsupported_k, "This build carries no SPDK"};
    }

    /** What this disk would accept, which is nothing at all. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept { return {}; }

    /** What the controller identifies as, which is nothing, since none was ever connected. */
    [[nodiscard]] spdk_controller_identity_t identity() const noexcept { return {}; }

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
};

#pragma endregion Absent

#endif

static_assert(ring_concept<linux_spdk_disk_t>, "The SPDK transport is a ring like every other");

} // namespace unum::ustore::nvme
