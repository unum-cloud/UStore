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

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/types.hpp" // `expected`

#include "ustore/utilities/nvme_disks.hpp"           // `blocking_queue_t`
#include "ustore/utilities/nvme_linux_spdk_disk.hpp" // `linux_spdk_disk_t`, `linux_spdk_queue_t`, `linux_spdk_probe_t`
#include "ustore/utilities/nvme_ring.hpp"            // `a_disk`, `queue_ref_t`

#include "nvme/ring_cases.hpp" // `ring_cases`, `ring_edge_t`, every transport-independent result

/** Where the rig script lives, compiled in by CMake. */
#if !defined(USTORE_SPDK_RIG)
#define USTORE_SPDK_RIG "scripts/rigs/spdk_target.sh"
#endif

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;

using linux_spdk_disk_t = util::linux_spdk_disk<atomic_reference, waiter_t>;
using linux_spdk_environment_t = util::linux_spdk_environment<atomic_reference, waiter_t>;

#pragma region Fixtures

static_assert(util::a_disk<linux_spdk_disk_t>, "The SPDK transport is a ring in every configuration");

/** A PCIe address no machine answers at, since function seven of device 31 on bus 255 is empty. */
constexpr char const* absent_address_k = "0000:ff:1f.7";

/** A fabric transport ID of the shape the rig exports, which the probe passes without a file to read. */
constexpr char const* fabric_address_k =
    "trtype:TCP adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420 subnqn:nqn.2026-09.io.ustore:malloc";

/** The settings this suite adds to the shared ones, each a <tt>--flag</tt> or a @c
 *  USTORE_ variable. */
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
[[nodiscard]] char const* name_of(util::linux_spdk_probe_t result) noexcept {
    switch (result) {
    case util::linux_spdk_probe_t::available_k: return "available_k";
    case util::linux_spdk_probe_t::without_hugepages_k: return "without_hugepages_k";
    case util::linux_spdk_probe_t::without_permission_k: return "without_permission_k";
    case util::linux_spdk_probe_t::not_found_k: return "not_found_k";
    }
    return "unknown";
}

/** The whole of @c /proc/meminfo, or an empty text where the file will not open. */
[[nodiscard]] std::string_view read_meminfo(std::span<char> into) noexcept {
    return util::read_system_text("/proc/meminfo", into);
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
void test_spdk_refuses_an_absent_address() {
    expect_true(linux_spdk_environment_t::probe(absent_address_k) == util::linux_spdk_probe_t::not_found_k);
    expect_true(linux_spdk_environment_t::probe(nullptr) == util::linux_spdk_probe_t::not_found_k);
    expect_true(linux_spdk_environment_t::probe("") == util::linux_spdk_probe_t::not_found_k);
    expect_true(linux_spdk_environment_t::probe("not an address") == util::linux_spdk_probe_t::not_found_k);
    expect_true(linux_spdk_environment_t::probe("0000:ff:1f.7:0") == util::linux_spdk_probe_t::not_found_k);
    expect_true(linux_spdk_environment_t::probe("/dev/nvme0n1") == util::linux_spdk_probe_t::not_found_k);
}

/** The hugepage parser reads what @c /proc/meminfo says, and nothing it does not say. */
void test_spdk_parses_the_hugepage_pool() {
    util::hugepage_inventory_t const present = util::hugepages_in_meminfo(meminfo_with_hugepages_k);
    expect_equal(present.total_pages, std::uint64_t {1024});
    expect_equal(present.free_pages, std::uint64_t {768});
    expect_equal(present.page_bytes, std::uint64_t {2048} * 1024);

    util::hugepage_inventory_t const empty = util::hugepages_in_meminfo(meminfo_without_hugepages_k);
    expect_equal(empty.total_pages, std::uint64_t {0});
    expect_equal(empty.free_pages, std::uint64_t {0});
    expect_equal(empty.page_bytes, std::uint64_t {2048} * 1024);

    util::hugepage_inventory_t const silent = util::hugepages_in_meminfo(meminfo_without_lines_k);
    expect_equal(silent.free_pages, std::uint64_t {0});
    expect_equal(silent.page_bytes, std::uint64_t {0});

    util::hugepage_inventory_t const nothing = util::hugepages_in_meminfo("");
    expect_equal(nothing.free_pages, std::uint64_t {0});

    /*  A key is matched in full, so a longer name that starts the same way is not its line. */
    expect_equal(util::meminfo_value("HugePages_Freeish:   99\n", "HugePages_Free"), std::uint64_t {0});
    expect_equal(util::meminfo_value("HugePages_Free:\t512 \n", "HugePages_Free"), std::uint64_t {512});
    expect_equal(util::meminfo_value("HugePages_Free: 7", "HugePages_Free"), std::uint64_t {7});
}

/** The driver classifier reads the last component of the link sysfs keeps. */
void test_spdk_names_the_bound_driver() {
    expect_true(util::pci_driver_named("../../../../bus/pci/drivers/nvme") == util::pci_driver_t::kernel_nvme_k);
    expect_true(util::pci_driver_named("../../../../bus/pci/drivers/vfio-pci") == util::pci_driver_t::vfio_pci_k);
    expect_true(util::pci_driver_named("../../../../bus/pci/drivers/uio_pci_generic") ==
                util::pci_driver_t::uio_generic_k);
    expect_true(util::pci_driver_named("igb_uio") == util::pci_driver_t::uio_generic_k);
    expect_true(util::pci_driver_named("../../../../bus/pci/drivers/xhci_hcd") == util::pci_driver_t::foreign_k);
    expect_true(util::pci_driver_named("") == util::pci_driver_t::unbound_k);
    expect_true(util::pci_driver_named("drivers/") == util::pci_driver_t::unbound_k);
}

/** An address is normalised to the long form sysfs names devices by, or refused outright. */
void test_spdk_normalises_an_address() {
    auto const [full, full_status] = util::pci_address_parsed("0000:01:00.0");
    expect_true(full_status.succeeded());
    if (full_status.succeeded()) expect_true(std::string_view {full.text} == "0000:01:00.0");

    auto const [brief, brief_status] = util::pci_address_parsed("1:2.3");
    expect_true(brief_status.succeeded());
    if (brief_status.succeeded()) expect_true(std::string_view {brief.text} == "0000:01:02.3");

    auto const [domained, domained_status] = util::pci_address_parsed("304C:00:1F.7");
    expect_true(domained_status.succeeded());
    if (domained_status.succeeded()) expect_true(std::string_view {domained.text} == "304c:00:1f.7");

    expect_true(util::pci_address_parsed("").failed());
    expect_true(util::pci_address_parsed("0000:01:00").failed());
    expect_true(util::pci_address_parsed("0000:01:00.").failed());
    expect_true(util::pci_address_parsed("0000:zz:00.0").failed());
    expect_true(util::pci_address_parsed("00000:01:00.0").failed());
    expect_true(util::pci_address_parsed("0000:01:20.0").failed());
    expect_true(util::pci_address_parsed("0000:01:00.8").failed());
    expect_true(util::pci_address_parsed("0000:00:01:00.0").failed());
}

/** A fabric transport ID probes available at once, since nothing on the machine can stand in its way. */
void test_spdk_passes_a_fabric_address() {
    expect_true(util::spdk_transport_named(fabric_address_k) == util::linux_spdk_transport_t::fabric_k);
    expect_true(util::spdk_transport_named("TRTYPE:tcp traddr:10.0.0.1") == util::linux_spdk_transport_t::fabric_k);
    expect_true(util::spdk_transport_named("trtype:PCIe traddr:0000:01:00.0") == util::linux_spdk_transport_t::pcie_k);
    expect_true(util::spdk_transport_named("0000:01:00.0") == util::linux_spdk_transport_t::pcie_k);
    expect_true(util::spdk_transport_named("") == util::linux_spdk_transport_t::pcie_k);
    expect_true(util::transport_field("trtype:TCP traddr:10.0.0.1 trsvcid:4420", "traddr:") == "10.0.0.1");
    expect_true(util::transport_field("trtype:TCP traddr:10.0.0.1 trsvcid:4420", "trsvcid:") == "4420");
    expect_true(util::transport_field("trtype:TCP", "traddr:").empty());
    expect_true(linux_spdk_environment_t::probe(fabric_address_k) == util::linux_spdk_probe_t::available_k);
    expect_true(linux_spdk_environment_t::probe("trtype:PCIe traddr:0000:ff:1f.7") ==
                util::linux_spdk_probe_t::not_found_k);
}

/** The result this machine gets agrees with what the test reads of the hugepage pool itself. */
void test_spdk_probes_this_machine() {
    std::array<char, 8192> meminfo {};
    util::hugepage_inventory_t const hugepages = util::hugepages_in_meminfo(read_meminfo(meminfo));

    std::array<char, 64> address {};
    std::string_view const first = first_pci_address(address);
    if (first.empty()) {
        expect_true(linux_spdk_environment_t::probe(absent_address_k) == util::linux_spdk_probe_t::not_found_k,
                    "this machine lists no PCIe device, so every address probes not_found_k");
        return;
    }

    util::linux_spdk_probe_t const result = linux_spdk_environment_t::probe(address.data());
    if (hugepages.free_pages == 0)
        expect_true(result == util::linux_spdk_probe_t::without_hugepages_k,
                    "probe({}) answers {}, with {} of {} hugepages free", first, name_of(result), hugepages.free_pages,
                    hugepages.total_pages);
    else
        expect_true(result != util::linux_spdk_probe_t::not_found_k,
                    "probe({}) answers {}, with {} of {} hugepages free", first, name_of(result), hugepages.free_pages,
                    hugepages.total_pages);
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

#if USTORE_WITH_LINUX_SPDK

#pragma region Cases

/** The controller identifies as the serial the rig set; a foreign target's serial is only printed. */
void test_spdk_identifies_the_controller(linux_spdk_disk_t& disk, std::string_view wanted) {
    util::linux_spdk_controller_identity_t const identity = disk.identity();
    fmt::println("  serial {}, model {}", identity.serial, identity.model);
    expect_true(std::strlen(identity.serial) != 0);
    if (!wanted.empty()) expect_true(std::string_view {identity.serial} == wanted);
}

/** What the disk reports is the namespace's sector, the queue's depth ceiling, the controller's
 *  queue pairs, and sixteen targets. */
void test_spdk_reports_the_namespace(linux_spdk_disk_t& disk) {
    util::disk_capabilities_t const limits = disk.capabilities();
    fmt::println("  sector {} bytes, transfer up to {} bytes, {} queue pairs of {}", limits.offset_alignment,
                 limits.max_transfer_bytes, limits.queue_pairs_limit, limits.depth_limit);
    expect_true(std::has_single_bit(limits.offset_alignment) && limits.offset_alignment <= block_bytes_k);
    expect_equal(limits.length_granularity, limits.offset_alignment);
    expect_equal(limits.memory_alignment, std::uint64_t {util::portable_alignment_k});
    expect_true(limits.max_transfer_bytes >= block_bytes_k);
    expect_equal(limits.depth_limit, std::uint32_t {16384});
    expect_true(limits.queue_pairs_limit >= 1);
    expect_equal(limits.media_limit, std::uint16_t {16});
    expect_true(!limits.reaches_gpu_memory);
}

/** Shares of every connection the process holds, read while no other thread opens or closes one. */
[[nodiscard]] std::uint32_t spdk_holders() noexcept {
    std::uint32_t holders = 0;
    for (util::linux_spdk_attachment_t const& record : util::linux_spdk_process().attachments.records())
        holders += record.holders;
    return holders;
}

/** A second disk on @p address shares the process's connection and a queue's queue pair, and a
 *  namespace one disk registered is refused to the other until it is given back. */
void test_spdk_shares_the_controller(linux_spdk_disk_t& disk, linux_spdk_environment_t const& environment,
                                     char const* address, util::linux_spdk_disk_config_t const& config) {
    auto [second, open_status] = linux_spdk_disk_t::open(environment, address, config);
    expect_status(open_status, util::status_code_t::success_k);
    if (open_status.failed()) return;
    {
        auto [queue, make_queue_status] = util::linux_spdk_queue_t::make(queue_pair_depth_k, {});
        expect_status(make_queue_status, util::status_code_t::success_k);
        util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
        std::uint32_t const unpaired = spdk_holders();
        expect_status(disk.register_media(queue_pair), util::status_code_t::success_k);
        expect_equal(spdk_holders(), unpaired + 1);
        expect_status(second.register_media(queue_pair), util::status_code_t::success_k);
        expect_equal(spdk_holders(), unpaired + 1);
    }
    auto [held, hold_status] = disk.register_medium(volume_request("1"));
    expect_true(hold_status.succeeded());
    if (hold_status.succeeded()) {
        expect_status(second.register_medium(volume_request("1")), util::status_code_t::contended_k);
        expect_true(disk.unregister_medium(held).succeeded());
        auto [taken, take_status] = second.register_medium(volume_request("1"));
        expect_true(take_status.succeeded());
        if (take_status.succeeded()) expect_true(second.unregister_medium(taken).succeeded());
    }
    second.release();
}

/** Connections the process holds, read while no other thread opens or closes one. */
[[nodiscard]] std::size_t spdk_connections() noexcept {
    std::size_t live = 0;
    for (util::linux_spdk_attachment_t const& record : util::linux_spdk_process().attachments.records())
        live += record.holders != 0;
    return live;
}

/** Releasing the last disk on @p address detaches its controller, and a fresh open in the same
 *  process connects it again and serves its namespace. */
void test_spdk_reconnects_after_release(linux_spdk_disk_t& disk, linux_spdk_environment_t const& environment,
                                        char const* address, util::linux_spdk_disk_config_t const& config,
                                        std::string_view serial) {
    expect_equal(spdk_connections(), std::size_t {1});
    {
        auto [queue, make_queue_status] = util::linux_spdk_queue_t::make(queue_pair_depth_k, {});
        expect_status(make_queue_status, util::status_code_t::success_k);
        util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
        expect_status(disk.register_media(queue_pair), util::status_code_t::success_k);
        disk.release();
        expect_equal(spdk_connections(), std::size_t {1}, "a queue's pair keeps its controller attached");
    }
    expect_equal(spdk_connections(), std::size_t {0});
    auto [fresh, open_status] = linux_spdk_disk_t::open(environment, address, config);
    expect_status(open_status, util::status_code_t::success_k);
    if (open_status.failed()) return;
    expect_equal(spdk_connections(), std::size_t {1});
    if (!serial.empty()) expect_true(std::string_view {fresh.identity().serial} == serial);
    auto [registered, register_status] = fresh.register_medium(volume_request("1"));
    expect_true(register_status.succeeded());
    if (register_status.succeeded()) expect_true(fresh.unregister_medium(registered).succeeded());
    fresh.release();
    expect_equal(spdk_connections(), std::size_t {0});
}

/** A blocking queue is refused, since a namespace has no file a positioned transfer could name. */
void test_spdk_refuses_a_blocking_queue(linux_spdk_disk_t& disk) {
    auto [queue, make_queue_status] = util::blocking_queue_t::make(queue_pair_depth_k, {});
    expect_status(make_queue_status, util::status_code_t::success_k);
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    std::array<util::request_t, 1> requests {};
    expect_status(disk.register_media(queue_pair), util::status_code_t::unsupported_k);
    expect_true(disk.submit(queue_pair, std::span<util::request_t const> {requests}) ==
                util::status_code_t::unsupported_k);
}

/** A request naming @c queue_range_k moves bytes through the queue's own unregistered blocks. */
void test_spdk_moves_through_the_queue_range(linux_spdk_disk_t& disk, util::medium_options_t const& request) {
    st::vector<util::byte_t, util::linux_spdk_dma_allocator<util::byte_t>> blocks;
    expect_true(blocks.resize(2 * block_bytes_k) == st::status_t::success_k);
    if (blocks.size() != 2 * block_bytes_k) return;
    std::span<util::byte_t> const range {blocks};
    auto [queue, make_queue_status] = util::linux_spdk_queue_t::make(queue_pair_depth_k, range);
    expect_status(make_queue_status, util::status_code_t::success_k);
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_status(disk.register_media(queue_pair), util::status_code_t::success_k);
    auto [registered, register_status] = disk.register_medium(request);
    if (!expect_true(register_status.succeeded())) return;

    paint(range.first(block_bytes_k), 3);
    paint(range.subspan(block_bytes_k), 0);
    util::memory_reference_t const source {util::registered_memory_id_t::queue_range_k, 0, 0};
    util::memory_reference_t const destination {util::registered_memory_id_t::queue_range_k, 0, block_bytes_k};
    util::extent_t const extent {0, block_bytes_k};
    std::array<util::request_t, 1> write {
        util::write_request(registered.reference(), extent, source, queue_pair.token(0))};
    expect_true(disk.submit(queue_pair, std::span<util::request_t const> {write}) == std::size_t {1});
    std::array<util::completion_t, polling_room_k> completions {};
    expect_true(util::poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<util::completion_t> {completions},
                                                          1) == std::size_t {1});
    expect_true(completions[0].completed_in_full());
    std::array<util::request_t, 1> read {
        util::read_request(registered.reference(), extent, destination, queue_pair.token(1))};
    expect_true(disk.submit(queue_pair, std::span<util::request_t const> {read}) == std::size_t {1});
    expect_true(util::poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<util::completion_t> {completions},
                                                          1) == std::size_t {1});
    expect_true(completions[0].completed_in_full());
    expect_true(same_bytes(range.first(block_bytes_k), range.subspan(block_bytes_k)));
    expect_true(disk.unregister_medium(registered).succeeded());
}

/** Connects to @p address, checks what the controller says it is, and runs the ring contract over it. */
void spdk_transport(char const* address, std::string_view serial) {
    auto [environment, acquire_status] = linux_spdk_environment_t::acquire(util::linux_spdk_environment_config_t {});
    run_test_case("linux_spdk_environment_t::acquire",
                  [&] { expect_status(acquire_status, util::status_code_t::success_k); });
    if (acquire_status.failed()) return;

    util::linux_spdk_disk_config_t config;
    config.registered_memory_limit = 4;
    auto [disk, open_status] = linux_spdk_disk_t::open(environment, address, config);
    run_test_case("linux_spdk_disk_t::open", [&] { expect_status(open_status, util::status_code_t::success_k); });
    if (open_status.failed()) return;

    run_test_case("test_spdk_identifies_the_controller", test_spdk_identifies_the_controller, disk, serial);
    run_test_case("test_spdk_reports_the_namespace", test_spdk_reports_the_namespace, disk);
    run_test_case("test_spdk_refuses_a_blocking_queue", test_spdk_refuses_a_blocking_queue, disk);

    auto [registered, register_status] = disk.register_medium(volume_request("1"));
    if (register_status.succeeded() && registered.capacity() == volume_bytes_k)
        ring_cases("linux_spdk_disk_t", disk, registered.reference(), ring_edge_t::refused_k);
    run_test_case("linux_spdk_disk_t::register_medium", [&] {
        if (!expect_true(register_status.succeeded())) return;
        expect_equal(registered.capacity(), volume_bytes_k);
        expect_true(disk.unregister_medium(registered).succeeded());
    });

    run_test_case("test_ring_holds_a_medium_in_flight<linux_spdk_disk_t>",
                  test_ring_holds_a_medium_in_flight<linux_spdk_disk_t>, disk, volume_request("1"));
    run_test_case("test_ring_keeps_what_it_flushed<linux_spdk_disk_t>",
                  test_ring_keeps_what_it_flushed<linux_spdk_disk_t>, disk, volume_request("1"));
    run_test_case("test_spdk_moves_through_the_queue_range", test_spdk_moves_through_the_queue_range, disk,
                  volume_request("1"));
    run_test_case("test_spdk_shares_the_controller", test_spdk_shares_the_controller, disk, environment, address,
                  config);
    run_test_case("test_spdk_reconnects_after_release", test_spdk_reconnects_after_release, disk, environment, address,
                  config, serial);
    disk.release();
}

/** Runs the transport where the flags name a target, starting one through the rig when asked to. */
void spdk_suite(linux_spdk_suite_flags_t const& flags) {
    if (!test_run.settings.spdk_address.empty()) {
        spdk_transport(test_run.settings.spdk_address.data(), flags.serial);
        return;
    }
    if (flags.root.empty()) {
        run_test_case("linux_spdk_disk_t::open",
                      [] { return case_result_t::skipped("needs USTORE_SPDK_ADDRESS or USTORE_SPDK_ROOT"); });
        return;
    }
    linux_spdk_rig_t rig;
    bool const up = rig.up(flags.root);
    run_test_case("linux_spdk_rig_t::up", [up] { expect_true(up); });
    if (!up) return;
    fmt::println("  target at {}", rig.address());
    spdk_transport(rig.address(), flags.serial.empty() ? std::string_view {rig.serial()} : flags.serial);
    rig.down();
}

#pragma endregion Cases

#else

#pragma region Refusals

/** Every entry point answers @c unsupported_k in a build that holds no driver,
 *  and nothing crashes. */
void test_spdk_refuses_without_the_option() {
    expect_status(linux_spdk_environment_t::acquire(util::linux_spdk_environment_config_t {}),
                  util::status_code_t::unsupported_k);

    util::linux_spdk_disk_config_t config;
    config.registered_memory_limit = 4;
    auto [disk, status] = linux_spdk_disk_t::open(linux_spdk_environment_t {}, absent_address_k, config);
    expect_status(status, util::status_code_t::unsupported_k);

    linux_spdk_disk_t empty;
    util::queue_ref_t queue_pair;
    util::registered_memory_t registered_memory;
    util::registered_medium_t descriptor;
    std::array<util::byte_t, 4096> buffer {};
    std::array<util::request_t, 1> requests {};
    std::array<util::completion_t, 1> completions {};
    expect_status(util::linux_spdk_queue_t::make(8, {}), util::status_code_t::unsupported_k);
    expect_status(empty.register_media(queue_pair), util::status_code_t::unsupported_k);
    expect_true(empty.register_memory(std::span<util::byte_t> {buffer}, util::memory_space_t::host_k) ==
                util::status_code_t::unsupported_k);
    expect_status(empty.unregister_memory(registered_memory), util::status_code_t::unsupported_k);
    expect_status(empty.register_medium(util::medium_options_t {}), util::status_code_t::unsupported_k);
    expect_status(empty.unregister_medium(descriptor), util::status_code_t::unsupported_k);
    expect_true(empty.submit(queue_pair, std::span<util::request_t const> {requests}) ==
                util::status_code_t::unsupported_k);
    expect_true(empty.poll_completions(queue_pair, std::span<util::completion_t> {completions}) ==
                util::status_code_t::unsupported_k);
    expect_status(empty.drain(queue_pair), util::status_code_t::unsupported_k);
    expect_equal(empty.outstanding(queue_pair), std::size_t {0});
    empty.cancel(queue_pair);

    util::disk_capabilities_t const reported = empty.capabilities();
    expect_equal(reported.depth_limit, std::uint32_t {0});
    expect_equal(reported.queue_pairs_limit, std::uint16_t {0});
    expect_true(!reported.reaches_gpu_memory);
    expect_equal(std::strlen(empty.identity().serial), std::size_t {0});
}

/** Checks the refusals, and reports that the transport itself needs a build that
 *  holds the driver. */
void spdk_suite(linux_spdk_suite_flags_t const&) {
    run_test_case("test_spdk_refuses_without_the_option", test_spdk_refuses_without_the_option);
    run_test_case("linux_spdk_disk_t::open",
                  [] { return case_result_t::skipped("built without USTORE_WITH_LINUX_SPDK"); });
}

#pragma endregion Refusals

#endif

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;
    using namespace unum::ustore::blobs;

    start_test_run(argc, argv);
    linux_spdk_suite_flags_t flags;
    if (char const* const root = setting_text(argc, argv, "USTORE_SPDK_ROOT", "--spdk-root")) flags.root = root;
    if (char const* const serial = setting_text(argc, argv, "USTORE_SPDK_SERIAL", "--spdk-serial"))
        flags.serial = serial;

    run_test_case("test_spdk_refuses_an_absent_address", test_spdk_refuses_an_absent_address);
    run_test_case("test_spdk_parses_the_hugepage_pool", test_spdk_parses_the_hugepage_pool);
    run_test_case("test_spdk_names_the_bound_driver", test_spdk_names_the_bound_driver);
    run_test_case("test_spdk_normalises_an_address", test_spdk_normalises_an_address);
    run_test_case("test_spdk_passes_a_fabric_address", test_spdk_passes_a_fabric_address);
    run_test_case("test_spdk_probes_this_machine", test_spdk_probes_this_machine);
    spdk_suite(flags);
    return finish_test_run();
}
