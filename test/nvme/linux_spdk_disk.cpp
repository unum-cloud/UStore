/**
 *  @file test/nvme/linux_spdk_disk.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The SPDK transport: its probe on any machine, and the ring contract over an NVMe/TCP target.
 *
 *  The probe is what keeps a failed mount from killing the process: @c rte_eal_init
 *  calls @c exit when it wants hugepages and finds none, when the drive is still the
 *  kernel's, or when @c /dev/vfio will not open, so the decision is made by reading
 *  files rather than by trying, and the parsers the probe is built from are exercised
 *  against synthetic text.
 *
 *  The ring cases run over a fabric, since a fabric needs no hugepage, no VFIO and no
 *  root. With @c --spdk-address the suite connects to a target that is already up;
 *  with @c --spdk-root it starts one itself through @c scripts/rigs/spdk_target.sh, a
 *  malloc bdev exported over NVMe/TCP, and stops it after. A namespace never answers a
 *  partial transfer, so the shared cases run with the read past the namespace refused
 *  whole rather than clipped.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstdio>  // `popen`, `pclose`, `std::fgets`
#include <cstring> // `std::strlen`, `std::memcpy`

#include <dirent.h> // `opendir`, `readdir`

#include <array>       // `std::array`
#include <bit>         // `std::has_single_bit`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include "ustore/utilities/types.hpp" // `expected`

#include "ustore/utilities/nvme_linux_spdk_disk.hpp" // `linux_spdk_disk_t`, `linux_spdk_environment_t`, `linux_spdk_probe_t`
#include "ustore/utilities/nvme_ring.hpp"            // `ring_concept`, `queue_pair_t`

#include "nvme/ring_cases.hpp" // `ring_cases`, `ring_edge_t`, every transport-independent result

/** Where the rig script lives, compiled in by CMake, since a suite reads no environment. */
#if !defined(USTORE_SPDK_RIG)
#define USTORE_SPDK_RIG "scripts/rigs/spdk_target.sh"
#endif

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

using linux_spdk_disk_t = linux_spdk_disk<atomic_reference, waiter_t>;
using linux_spdk_environment_t = linux_spdk_environment<atomic_reference>;

#pragma region Fixtures

static_assert(ring_concept<linux_spdk_disk_t>, "The SPDK transport is a ring in every configuration");

/** A PCIe address no machine answers at, since function seven of device 31 on bus 255 is empty. */
constexpr char const* absent_address_k = "0000:ff:1f.7";

/** A fabric transport ID of the shape the rig exports, which the probe passes without a file to read. */
constexpr char const* fabric_address_k =
    "trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2026-09.io.ustore:malloc";

/** Leases the mount asks for, which the target may lower to the queue pairs it grants. */
[[maybe_unused]] constexpr std::uint16_t spdk_queue_pairs_k = 4;

/** The flags this suite adds to the shared ones. */
struct linux_spdk_suite_flags_t {

    /** An SPDK tree with a finished build, whose target the suite starts and stops through the rig. */
    std::string_view root;

    /** The serial the controller must identify as, which the rig sets and a foreign target may not. */
    std::string_view serial;
};

/** A machine with a hugepage pool, as @c /proc/meminfo spells one. */
constexpr char const* meminfo_with_hugepages_k = R"(MemTotal:       201326592 kB
MemFree:         98765432 kB
AnonHugePages:      2048 kB
HugePages_Total:    1024
HugePages_Free:      768
HugePages_Rsvd:        0
Hugepagesize:       2048 kB
)";

/** A machine whose hugepage pool is declared and empty, which is what this box reports. */
constexpr char const* meminfo_without_hugepages_k = R"(MemTotal:       201326592 kB
HugePages_Total:       0
HugePages_Free:        0
Hugepagesize:       2048 kB
)";

/** A kernel that reports no hugepage lines at all, which an older or stripped one does. */
constexpr char const* meminfo_without_lines_k = R"(MemTotal:       201326592 kB
MemFree:         98765432 kB
)";

/** What a probe result is called, so a machine's own answer reads as a word rather than a number. */
[[nodiscard]] char const* describe(linux_spdk_probe_t result) noexcept {
    switch (result) {
    case linux_spdk_probe_t::available_k: return "available_k";
    case linux_spdk_probe_t::without_hugepages_k: return "without_hugepages_k";
    case linux_spdk_probe_t::without_permission_k: return "without_permission_k";
    case linux_spdk_probe_t::not_found_k: return "not_found_k";
    }
    return "unknown";
}

/** The whole of @c /proc/meminfo, or an empty text where the file will not open. */
[[nodiscard]] std::string_view read_meminfo(std::span<char> into) noexcept {
    return read_system_text("/proc/meminfo", into);
}

/** The first PCIe address sysfs lists, or an empty text on a machine that lists none. */
[[nodiscard]] std::string_view first_pci_address(std::span<char> into) noexcept {
    DIR* const directory = ::opendir("/sys/bus/pci/devices");
    if (!directory) return {};
    std::string_view found;
    while (dirent const* const entry = ::readdir(directory)) {
        std::string_view const name {entry->d_name};
        if (name == "." || name == "..") continue;
        if (name.size() >= into.size()) continue;
        for (std::size_t index = 0; index != name.size(); ++index) into[index] = name[index];
        into[name.size()] = '\0';
        found = std::string_view {into.data(), name.size()};
        break;
    }
    ::closedir(directory);
    return found;
}

#pragma endregion Fixtures

#pragma region Probe

/** A bogus address answers @c not_found_k, without a crash and without a wait. */
[[nodiscard]] std::size_t spdk_refuses_an_absent_address() {
    std::size_t failures = 0;
    failures += expect(linux_spdk_environment_t::probe(absent_address_k) == linux_spdk_probe_t::not_found_k);
    failures += expect(linux_spdk_environment_t::probe(nullptr) == linux_spdk_probe_t::not_found_k);
    failures += expect(linux_spdk_environment_t::probe("") == linux_spdk_probe_t::not_found_k);
    failures += expect(linux_spdk_environment_t::probe("not an address") == linux_spdk_probe_t::not_found_k);
    failures += expect(linux_spdk_environment_t::probe("0000:ff:1f.7:0") == linux_spdk_probe_t::not_found_k);
    failures += expect(linux_spdk_environment_t::probe("/dev/nvme0n1") == linux_spdk_probe_t::not_found_k);
    return failures;
}

/** The hugepage parser reads what @c /proc/meminfo says, and nothing it does not say. */
[[nodiscard]] std::size_t spdk_parses_the_hugepage_pool() {
    std::size_t failures = 0;
    hugepage_inventory_t const present = hugepages_in_meminfo(meminfo_with_hugepages_k);
    failures += expect_eq(present.total_pages, std::uint64_t {1024});
    failures += expect_eq(present.free_pages, std::uint64_t {768});
    failures += expect_eq(present.page_bytes, std::uint64_t {2048} * 1024);

    hugepage_inventory_t const empty = hugepages_in_meminfo(meminfo_without_hugepages_k);
    failures += expect_eq(empty.total_pages, std::uint64_t {0});
    failures += expect_eq(empty.free_pages, std::uint64_t {0});
    failures += expect_eq(empty.page_bytes, std::uint64_t {2048} * 1024);

    hugepage_inventory_t const silent = hugepages_in_meminfo(meminfo_without_lines_k);
    failures += expect_eq(silent.free_pages, std::uint64_t {0});
    failures += expect_eq(silent.page_bytes, std::uint64_t {0});

    hugepage_inventory_t const nothing = hugepages_in_meminfo("");
    failures += expect_eq(nothing.free_pages, std::uint64_t {0});

    /*  A key is matched in full, so a longer name that starts the same way is not its line. */
    failures += expect_eq(meminfo_value("HugePages_Freeish:   99\n", "HugePages_Free"), std::uint64_t {0});
    failures += expect_eq(meminfo_value("HugePages_Free:\t512 \n", "HugePages_Free"), std::uint64_t {512});
    failures += expect_eq(meminfo_value("HugePages_Free: 7", "HugePages_Free"), std::uint64_t {7});
    return failures;
}

/** The driver classifier reads the last component of the link sysfs keeps. */
[[nodiscard]] std::size_t spdk_names_the_bound_driver() {
    std::size_t failures = 0;
    failures += expect(pci_driver_named("../../../../bus/pci/drivers/nvme") == pci_driver_t::kernel_nvme_k);
    failures += expect(pci_driver_named("../../../../bus/pci/drivers/vfio-pci") == pci_driver_t::vfio_pci_k);
    failures += expect(pci_driver_named("../../../../bus/pci/drivers/uio_pci_generic") == pci_driver_t::uio_generic_k);
    failures += expect(pci_driver_named("igb_uio") == pci_driver_t::uio_generic_k);
    failures += expect(pci_driver_named("../../../../bus/pci/drivers/xhci_hcd") == pci_driver_t::foreign_k);
    failures += expect(pci_driver_named("") == pci_driver_t::unbound_k);
    failures += expect(pci_driver_named("drivers/") == pci_driver_t::unbound_k);
    return failures;
}

/** An address is normalised to the long form sysfs names devices by, or refused outright. */
[[nodiscard]] std::size_t spdk_normalises_an_address() {
    std::size_t failures = 0;
    expected<pci_address_t> const full = pci_address_parsed("0000:01:00.0");
    failures += expect(full.has_value());
    if (full) failures += expect(std::string_view {full->text} == "0000:01:00.0");

    expected<pci_address_t> const brief = pci_address_parsed("1:2.3");
    failures += expect(brief.has_value());
    if (brief) failures += expect(std::string_view {brief->text} == "0000:01:02.3");

    expected<pci_address_t> const domained = pci_address_parsed("304C:00:1F.7");
    failures += expect(domained.has_value());
    if (domained) failures += expect(std::string_view {domained->text} == "304c:00:1f.7");

    failures += expect(pci_address_parsed("").failed());
    failures += expect(pci_address_parsed("0000:01:00").failed());
    failures += expect(pci_address_parsed("0000:01:00.").failed());
    failures += expect(pci_address_parsed("0000:zz:00.0").failed());
    failures += expect(pci_address_parsed("00000:01:00.0").failed());
    failures += expect(pci_address_parsed("0000:01:20.0").failed());
    failures += expect(pci_address_parsed("0000:01:00.8").failed());
    failures += expect(pci_address_parsed("0000:00:01:00.0").failed());
    return failures;
}

/** A fabric transport ID probes available at once, since nothing on the machine can stand in its way. */
[[nodiscard]] std::size_t spdk_passes_a_fabric_address() {
    std::size_t failures = 0;
    failures += expect(spdk_transport_named(fabric_address_k) == linux_spdk_transport_t::fabric_k);
    failures += expect(spdk_transport_named("TRTYPE:tcp traddr:10.0.0.1") == linux_spdk_transport_t::fabric_k);
    failures += expect(spdk_transport_named("trtype:PCIe traddr:0000:01:00.0") == linux_spdk_transport_t::pcie_k);
    failures += expect(spdk_transport_named("0000:01:00.0") == linux_spdk_transport_t::pcie_k);
    failures += expect(spdk_transport_named("") == linux_spdk_transport_t::pcie_k);
    failures += expect(transport_field("trtype:TCP traddr:10.0.0.1 trsvcid:4420", "traddr:") == "10.0.0.1");
    failures += expect(transport_field("trtype:TCP traddr:10.0.0.1 trsvcid:4420", "trsvcid:") == "4420");
    failures += expect(transport_field("trtype:TCP", "traddr:").empty());
    failures += expect(linux_spdk_environment_t::probe(fabric_address_k) == linux_spdk_probe_t::available_k);
    failures += expect(linux_spdk_environment_t::probe("trtype:PCIe traddr:0000:ff:1f.7") ==
                       linux_spdk_probe_t::not_found_k);
    return failures;
}

/** The result this machine gets agrees with what the test reads of the hugepage pool itself. */
[[nodiscard]] std::size_t spdk_probes_this_machine() {
    std::size_t failures = 0;
    std::array<char, 8192> meminfo {};
    hugepage_inventory_t const hugepages = hugepages_in_meminfo(read_meminfo(meminfo));

    std::array<char, 64> address {};
    std::string_view const first = first_pci_address(address);
    if (first.empty()) {
        fmt::println("  this machine lists no PCIe device, so every address probes not_found_k");
        return failures + expect(linux_spdk_environment_t::probe(absent_address_k) == linux_spdk_probe_t::not_found_k);
    }

    linux_spdk_probe_t const result = linux_spdk_environment_t::probe(address.data());
    fmt::println("  probe({}) answers {}, with {} of {} hugepages free", first, describe(result), hugepages.free_pages,
                 hugepages.total_pages);
    if (hugepages.free_pages == 0) failures += expect(result == linux_spdk_probe_t::without_hugepages_k);
    else failures += expect(result != linux_spdk_probe_t::not_found_k);
    return failures;
}

#pragma endregion Probe

#pragma region Rig

/** Longest command line and longest answer the rig exchanges with the suite. */
constexpr std::size_t rig_text_capacity_k = 2048;

/**
 *  @brief Runs one verb of the rig script and keeps the last line it printed.
 *  @param[in] root The SPDK tree, handed to the script through its own environment.
 *  @param[in] verb What the script should do: @c up, @c down, @c address or @c serial.
 *  @param[out] last Room for the last line, which the address and the serial arrive on.
 *  @return Whether the script exited zero.
 */
[[nodiscard]] bool run_rig(std::string_view root, char const* verb, std::span<char> last) noexcept {
    std::array<char, rig_text_capacity_k> command {};
    auto const written = fmt::format_to_n(command.data(), command.size() - 1, "USTORE_SPDK_ROOT='{}' '{}' {}", root,
                                          USTORE_SPDK_RIG, verb);
    *written.out = '\0';
    FILE* const pipe = ::popen(command.data(), "r");
    if (!pipe) return false;
    last[0] = '\0';
    std::array<char, rig_text_capacity_k> line {};
    while (std::fgets(line.data(), static_cast<int>(line.size()), pipe)) {
        std::size_t length = std::strlen(line.data());
        while (length != 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';
        if (length == 0 || length >= last.size()) continue;
        std::memcpy(last.data(), line.data(), length + 1);
    }
    return ::pclose(pipe) == 0;
}

/** Whether a rig's target is up, which decides what the handle's death has to stop. */
enum class rig_state_t : std::uint8_t {
    down_k,
    up_k,
};

/** A target the suite started itself, stopped when the handle dies. */
class linux_spdk_rig_t {
  public:
    linux_spdk_rig_t() noexcept = default;
    linux_spdk_rig_t(linux_spdk_rig_t const&) = delete;
    linux_spdk_rig_t& operator=(linux_spdk_rig_t const&) = delete;
    ~linux_spdk_rig_t() noexcept { down(); }

    /** Starts the target of @p root, answering whether it listens; the address and the serial follow. */
    [[nodiscard]] bool up(std::string_view root) noexcept {
        root_ = root;
        if (!run_rig(root_, "up", address_)) return false;
        if (!run_rig(root_, "serial", serial_)) return false;
        state_ = rig_state_t::up_k;
        return true;
    }

    /** Stops the target, which a second call finds already gone. */
    void down() noexcept {
        if (state_ != rig_state_t::up_k) return;
        std::array<char, rig_text_capacity_k> ignored {};
        [[maybe_unused]] bool const stopped = run_rig(root_, "down", ignored);
        state_ = rig_state_t::down_k;
    }

    /** The transport ID the target listens on. */
    [[nodiscard]] char const* address() const noexcept { return address_.data(); }

    /** The serial the target's controller identifies as. */
    [[nodiscard]] char const* serial() const noexcept { return serial_.data(); }

  private:

    /** The SPDK tree the script runs the target of. */
    std::string_view root_;

    /** The transport ID @c up printed last. */
    std::array<char, rig_text_capacity_k> address_ {};

    /** The serial the script sets on its subsystem. */
    std::array<char, 64> serial_ {};

    /** Whether a target is up to stop. */
    rig_state_t state_ = rig_state_t::down_k;
};

#pragma endregion Rig

#if USTORE_WITH_SPDK

#pragma region Cases

/** The controller identifies as the serial the rig set; a foreign target's serial is only printed. */
[[nodiscard]] std::size_t spdk_identifies_the_controller(linux_spdk_disk_t& disk, std::string_view wanted) {
    std::size_t failures = 0;
    linux_spdk_controller_identity_t const identity = disk.identity();
    fmt::println("  serial {}, model {}", identity.serial, identity.model);
    failures += expect(std::strlen(identity.serial) != 0);
    if (!wanted.empty()) failures += expect(std::string_view {identity.serial} == wanted);
    return failures;
}

/** What the disk reports is the namespace's sector, the queue pair shape the mount asked for, and one target. */
[[nodiscard]] std::size_t spdk_reports_the_namespace(linux_spdk_disk_t& disk) {
    std::size_t failures = 0;
    ring_capabilities_t const limits = disk.capabilities();
    fmt::println("  sector {} bytes, transfer up to {} bytes, {} queue pairs of {}", limits.offset_alignment,
                 limits.max_transfer_bytes, limits.queue_pairs_limit, limits.depth_limit);
    failures += expect(std::has_single_bit(limits.offset_alignment) && limits.offset_alignment <= block_bytes_k);
    failures += expect_eq(limits.length_granularity, limits.offset_alignment);
    failures += expect_eq(limits.memory_alignment, std::uint64_t {portable_alignment_k});
    failures += expect(limits.max_transfer_bytes >= block_bytes_k);
    failures += expect_eq(limits.depth_limit, queue_pair_depth_k);
    failures += expect(limits.queue_pairs_limit >= 1 && limits.queue_pairs_limit <= spdk_queue_pairs_k);
    failures += expect_eq(limits.media_limit, std::uint16_t {1});
    failures += expect(!limits.reaches_gpu_memory);
    return failures;
}

/** Connects to @p address, checks what the controller says it is, and runs the ring contract over it. */
void spdk_transport(suite_totals_t& totals, char const* address, std::string_view serial) {
    expected<linux_spdk_environment_t> environment = linux_spdk_environment_t::acquire(
        linux_spdk_environment_config_t {});
    if (report(totals, "linux-spdk.environment", expect_status(environment, status_code_t::success_k)) != 0) return;

    linux_spdk_disk_config_t config;
    config.queue_pairs_limit = spdk_queue_pairs_k;
    config.depth_limit = queue_pair_depth_k;
    config.registered_memory_limit = 4;
    expected<linux_spdk_disk_t> disk = linux_spdk_disk_t::open(*environment, address, config);
    if (report(totals, "linux-spdk.mount", expect_status(disk, status_code_t::success_k)) != 0) return;

    report(totals, "linux-spdk.identify", spdk_identifies_the_controller, *disk, serial);
    report(totals, "linux-spdk.namespace", spdk_reports_the_namespace, *disk);

    std::size_t volume_failures = 0;
    expected<registered_medium_t> registered = disk->register_medium(volume_request("namespace"));
    volume_failures += expect(registered.has_value());
    if (registered) {
        registered_medium_t descriptor = std::move(*registered);
        volume_failures += expect_eq(descriptor.capacity(), volume_bytes_k);
        if (descriptor.capacity() == volume_bytes_k)
            ring_cases(totals, "linux-spdk", *disk, descriptor.reference(), ring_edge_t::refused_k);
        volume_failures += expect(disk->unregister_medium(descriptor).succeeded());
    }
    report(totals, "linux-spdk.volume", volume_failures);

    report(totals, "linux-spdk.target_in_flight", ring_holds_a_target_in_flight<linux_spdk_disk_t>, *disk,
           volume_request("namespace"));
    report(totals, "linux-spdk.keeps_what_it_flushed", ring_keeps_what_it_flushed<linux_spdk_disk_t>, *disk,
           volume_request("namespace"));
}

/** Runs the transport where the flags name a target, starting one through the rig when asked to. */
void spdk_suite(suite_totals_t& totals, suite_options_t const& options, linux_spdk_suite_flags_t const& flags) {
    if (!options.spdk_address.empty()) {
        spdk_transport(totals, options.spdk_address.data(), flags.serial);
        return;
    }
    if (flags.root.empty()) {
        report(totals, "linux-spdk", case_result_t::skipped("needs --spdk-address or --spdk-root"));
        return;
    }
    linux_spdk_rig_t rig;
    if (report(totals, "linux-spdk.rig", expect(rig.up(flags.root))) != 0) return;
    fmt::println("  target at {}", rig.address());
    spdk_transport(totals, rig.address(), flags.serial.empty() ? std::string_view {rig.serial()} : flags.serial);
    rig.down();
}

#pragma endregion Cases

#else

#pragma region Refusals

/** Every entry point answers @c unsupported_k in a build that carries no driver, and nothing crashes. */
[[nodiscard]] std::size_t spdk_refuses_without_the_option() {
    std::size_t failures = 0;
    failures += expect_status(linux_spdk_environment_t::acquire(linux_spdk_environment_config_t {}),
                              status_code_t::unsupported_k);

    linux_spdk_disk_config_t config;
    config.queue_pairs_limit = 4;
    config.depth_limit = 8;
    config.registered_memory_limit = 4;
    expected<linux_spdk_disk_t> disk = linux_spdk_disk_t::open(linux_spdk_environment_t {}, absent_address_k, config);
    failures += expect_status(disk, status_code_t::unsupported_k);

    linux_spdk_disk_t empty;
    queue_pair_t queue_pair;
    registered_memory_t registered_memory;
    registered_medium_t descriptor;
    std::array<byte_t, 4096> buffer {};
    std::array<request_t, 1> requests {};
    std::array<completion_t, 1> completions {};
    failures += expect_status(empty.acquire_queue_pair(std::uint32_t {8}), status_code_t::unsupported_k);
    failures += expect_status(empty.bind_queue_pair(queue_pair), status_code_t::unsupported_k);
    failures += expect_status(empty.release_queue_pair(queue_pair), status_code_t::unsupported_k);
    failures += expect(empty.register_memory(std::span<byte_t> {buffer}, memory_space_t::host_k) ==
                       status_code_t::unsupported_k);
    failures += expect_status(empty.unregister_memory(registered_memory), status_code_t::unsupported_k);
    failures += expect_status(empty.register_medium(medium_options_t {}), status_code_t::unsupported_k);
    failures += expect_status(empty.unregister_medium(descriptor), status_code_t::unsupported_k);
    failures += expect(empty.submit(queue_pair, std::span<request_t const> {requests}) == status_code_t::unsupported_k);
    failures += expect(empty.poll_completions(queue_pair, std::span<completion_t> {completions}) ==
                       status_code_t::unsupported_k);
    failures += expect_status(empty.drain(queue_pair), status_code_t::unsupported_k);
    failures += expect_eq(empty.outstanding(queue_pair), std::size_t {0});
    empty.cancel(queue_pair);

    ring_capabilities_t const reported = empty.capabilities();
    failures += expect_eq(reported.depth_limit, std::uint32_t {0});
    failures += expect_eq(reported.queue_pairs_limit, std::uint16_t {0});
    failures += expect(!reported.reaches_gpu_memory);
    failures += expect_eq(std::strlen(empty.identity().serial), std::size_t {0});
    return failures;
}

/** Checks the refusals, and reports that the transport itself needs a build that carries the driver. */
void spdk_suite(suite_totals_t& totals, suite_options_t const&, linux_spdk_suite_flags_t const&) {
    report(totals, "linux-spdk.refuses_without_the_option", spdk_refuses_without_the_option);
    report(totals, "linux-spdk", case_result_t::skipped("needs a build with USTORE_WITH_SPDK"));
}

#pragma endregion Refusals

#endif

} // namespace unum::ustore::test

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;
    using namespace unum::ustore::blobs;
    using namespace unum::ustore::utilities;

    suite_options_t options;
    linux_spdk_suite_flags_t flags;
    std::array<flag_t, 2> const suite_flags {{
        {"--spdk-root", "path", "SPDK tree with a finished build, whose target the suite starts and stops",
         &flags.root},
        {"--spdk-serial", "text", "serial the controller must identify as, the rig's when absent", &flags.serial},
    }};
    if (argument_status_t const status = parse_suite_options(argc, argv, options, suite_flags);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    report(totals, "linux-spdk.absent_address", spdk_refuses_an_absent_address);
    report(totals, "linux-spdk.hugepage_pool", spdk_parses_the_hugepage_pool);
    report(totals, "linux-spdk.bound_driver", spdk_names_the_bound_driver);
    report(totals, "linux-spdk.normalises_address", spdk_normalises_an_address);
    report(totals, "linux-spdk.fabric_address", spdk_passes_a_fabric_address);
    report(totals, "linux-spdk.probes_this_machine", spdk_probes_this_machine);
    spdk_suite(totals, options, flags);
    return suite_exit_code(totals);
}
