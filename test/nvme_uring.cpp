/**
 *  @file test/nvme_uring.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The ring contract on Linux, and the four things only a real io_uring transport can get wrong.
 *
 *  The shapes are the ones @c test/nvme_ring.cpp runs against the RAM and blocking transports, since
 *  a transport that passes only its own suite proves nothing. What is added here is what a kernel
 *  queue alone can break: a chain cut by a short result, a cancellation that must post exactly one
 *  completion per request, a completion queue drained through a window far narrower than it, and the
 *  alignments @c statx reports for the filesystem under the volume.
 *
 *  The suite needs @c --directory and refuses tmpfs, which accepts @c O_DIRECT and ignores alignment
 *  outright, hiding exactly the mistakes this transport can make.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstdio>  // `std::fopen`, `std::fscanf`, `std::fclose`
#include <cstring> // `std::strrchr`

#include <fcntl.h>       // `AT_EMPTY_PATH`
#include <linux/stat.h>  // `struct statx`, `STATX_DIOALIGN`
#include <sys/syscall.h> // `__NR_statx`
#include <unistd.h>      // `syscall`

#include <array>       // `std::array`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <thread>      // `std::thread`
#include <utility>     // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/blocking_files_disk.hpp" // `native_file_t`, `close_native_file`, `blocking_files_disk_t`
#include "nvme/uring_files_disk.hpp"    // `uring_files_disk_t`, `uring_probe_t`

#include "nvme_ring_cases.hpp" // `ring_cases`, every transport-independent verdict

namespace unum::ustore {

using namespace unum::ustore::nvme;

#pragma region Fixtures

/** The bytes the kernel exports for @p device under @c /sys/class/block, in sectors of 512. */
[[nodiscard]] std::uint64_t exported_device_bytes(char const* device) noexcept {
    char const* const slash = std::strrchr(device, '/');
    char const* const name = slash ? slash + 1 : device;
    test_path_t const sysfs {"/sys/class/block", name};
    test_path_t const sectors_path {sysfs.c_str(), "size"};
    FILE* const file = std::fopen(sectors_path.c_str(), "r");
    if (!file) return 0;
    unsigned long long sectors = 0;
    if (std::fscanf(file, "%llu", &sectors) != 1) sectors = 0;
    std::fclose(file);
    return static_cast<std::uint64_t>(sectors) * 512ull;
}

/** Registers @p request on @p disk and answers the capacity the descriptor carries, giving the target back. */
template <typename disk_type_>
expected<std::uint64_t> registered_capacity(disk_type_& disk, target_request_t const& request) {
    expected<descriptor_t> registered = disk.register_target(request);
    if (!registered) return registered.status();
    std::uint64_t const capacity = registered->capacity();
    if (status_t const released = disk.deregister_target(*registered); released.failed()) return released;
    return capacity;
}

#pragma endregion Fixtures

/**
 *  @brief Leases the ring, registers @p bytes of aligned memory, runs @p body and gives both back.
 *  @param[inout] disk The mounted ring the lease and the range come from.
 *  @param[in] bytes Bytes of buffer to register, which the body addresses through the registration.
 *  @param[in] body Called with the lease, the registration and the buffer it covers.
 */
template <typename body_type_>
std::size_t on_lease(uring_files_disk_t& disk, std::size_t bytes, body_type_&& body) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(bytes);
    failures += expect_eq(buffer.size(), bytes);
    if (buffer.size() != bytes) return failures;

    expected<ring_owner_t> leased = disk.open(lease_depth_k);
    failures += expect(leased.has_value());
    if (!leased) return failures;
    ring_owner_t owner = std::move(*leased);

    expected<registration_t> pinned = disk.register_chunk(whole(buffer), memory_space_t::host_k);
    failures += expect(pinned.has_value());
    if (!pinned) {
        failures += expect(disk.release(owner).succeeded());
        return failures;
    }
    registration_t registration = std::move(*pinned);

    failures += body(owner, registration, whole(buffer));

    failures += expect(disk.drain(owner).succeeded());
    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

#pragma region Cases

/** A request the disk can refuse before the kernel sees it fails alone, and its neighbours still run. */
std::size_t uring_fails_one_request_of_a_batch(uring_files_disk_t& disk, target_place_t target) {
    return on_lease(disk, 2 * block_bytes_k, [&](ring_owner_t& owner, registration_t& registration, std::span<byte_t>) {
        std::size_t failures = 0;
        buffer_place_t stale = registration.place(0);
        stale.generation = static_cast<std::uint16_t>(registration.generation() + 1);

        std::array<request_t, 3> batch {};
        batch[0] = read_request(target, extent_t {0, block_bytes_k}, stale, owner.token(0));
        batch[1] = read_request(target, extent_t {1, block_bytes_k}, registration.place(0), owner.token(1));
        batch[2] = read_request(target, extent_t {0, block_bytes_k}, registration.place(block_bytes_k), owner.token(2));
        failures += expect(disk.submit(owner, std::span<request_t const> {batch}) == std::size_t {3});

        std::array<completion_t, reaping_room_k> completions {};
        failures += expect(reap_at_least(disk, owner, std::span<completion_t> {completions}, 3) == std::size_t {3});
        std::size_t refused = 0;
        std::size_t whole = 0;
        for (std::size_t index = 0; index != 3; ++index) {
            if (completions[index].state == completion_state_t::failed_k) {
                refused += expect_status(completions[index].status, status_code_t::invalid_argument_k) == 0 ? 1 : 0;
                continue;
            }
            whole += completions[index].whole() ? 1 : 0;
        }
        failures += expect_eq(refused, std::size_t {2});
        failures += expect_eq(whole, std::size_t {1});
        return failures;
    });
}

/** A chain whose first read is short reports the rest cancelled, which is what cutting a link means. */
std::size_t uring_cuts_a_chain_on_a_short_read(uring_files_disk_t& disk, target_place_t target) {
    constexpr std::uint64_t reachable_k = 1024;
    return on_lease(disk, 3 * block_bytes_k, [&](ring_owner_t& owner, registration_t& registration, std::span<byte_t>) {
        std::size_t failures = 0;
        std::array<request_t, 3> chain {};
        chain[0] = read_request(target, extent_t {volume_bytes_k - reachable_k, block_bytes_k}, registration.place(0),
                                owner.token(0));
        chain[0].chaining = request_chaining_t::chained_k;
        chain[1] = read_request(target, extent_t {0, block_bytes_k}, registration.place(block_bytes_k), owner.token(1));
        chain[1].chaining = request_chaining_t::chained_k;
        chain[2] = read_request(target, extent_t {block_bytes_k, block_bytes_k}, registration.place(2 * block_bytes_k),
                                owner.token(2));

        failures += expect(disk.submit(owner, std::span<request_t const> {chain}) == std::size_t {3});
        std::array<completion_t, reaping_room_k> completions {};
        failures += expect(reap_at_least(disk, owner, std::span<completion_t> {completions}, 3) == std::size_t {3});
        failures += expect(completions[0].state == completion_state_t::partial_k);
        failures += expect_eq(completions[0].transferred, reachable_k);
        failures += expect(completions[1].state == completion_state_t::cancelled_k);
        failures += expect(completions[2].state == completion_state_t::cancelled_k);
        failures += expect_eq(completions[1].token.tag(), std::uint32_t {1});
        failures += expect_eq(completions[2].token.tag(), std::uint32_t {2});
        return failures;
    });
}

/** A cancellation mid-flight delivers one completion per submitted request, and never one extra. */
std::size_t uring_counts_completions_after_a_cancel(uring_files_disk_t& disk, target_place_t target) {
    return on_lease(
        disk, lease_depth_k * block_bytes_k, [&](ring_owner_t& owner, registration_t& registration, std::span<byte_t>) {
            std::size_t failures = 0;
            std::array<request_t, lease_depth_k> batch {};
            for (std::size_t index = 0; index != lease_depth_k; ++index)
                batch[index] = read_request(target, extent_t {index * block_bytes_k, block_bytes_k},
                                            registration.place(index * block_bytes_k),
                                            owner.token(static_cast<std::uint32_t>(index)));
            failures += expect(disk.submit(owner, std::span<request_t const> {batch}) == std::size_t {lease_depth_k});
            disk.cancel(owner);

            std::array<completion_t, reaping_room_k> completions {};
            failures += expect(reap_at_least(disk, owner, std::span<completion_t> {completions}, lease_depth_k) ==
                               std::size_t {lease_depth_k});
            std::array<std::size_t, lease_depth_k> seen {};
            for (std::size_t index = 0; index != lease_depth_k; ++index) {
                std::uint32_t const tag = completions[index].token.tag();
                failures += expect(tag < lease_depth_k);
                if (tag < lease_depth_k) ++seen[tag];
            }
            for (std::size_t index = 0; index != lease_depth_k; ++index)
                failures += expect_eq(seen[index], std::size_t {1});
            failures += expect_eq(disk.in_flight(owner), std::size_t {0});

            for (std::size_t attempt = 0; attempt != 64; ++attempt)
                failures += expect(disk.reap(owner, std::span<completion_t> {completions}) == std::size_t {0});
            return failures;
        });
}

/**
 *  @brief More completions than the queue holds still all arrive, which is what overflow costs.
 *
 *  The mount sizes the completion queue at four times the credit, so a lease alone can never fill
 *  it; what can is the completion every cancelling entry posts for itself. Asking to cancel far more
 *  often than the queue has seats puts the kernel over the edge, and from there every completion
 *  sits in the kernel's overflow list until a reap enters again to flush it.
 */
std::size_t uring_reaps_an_overflowed_queue(uring_files_disk_t& disk, target_place_t target) {
    constexpr std::size_t cancels_k = 64;
    return on_lease(
        disk, lease_depth_k * block_bytes_k, [&](ring_owner_t& owner, registration_t& registration, std::span<byte_t>) {
            std::size_t failures = 0;
            std::array<request_t, lease_depth_k> batch {};
            for (std::size_t index = 0; index != lease_depth_k; ++index)
                batch[index] = read_request(target, extent_t {index * block_bytes_k, block_bytes_k},
                                            registration.place(index * block_bytes_k),
                                            owner.token(static_cast<std::uint32_t>(index)));
            failures += expect(disk.submit(owner, std::span<request_t const> {batch}) == std::size_t {lease_depth_k});
            for (std::size_t attempt = 0; attempt != cancels_k; ++attempt) disk.cancel(owner);

            std::array<completion_t, 1> narrow {};
            std::array<std::size_t, lease_depth_k> seen {};
            for (std::size_t index = 0; index != lease_depth_k; ++index) {
                failures += expect(reap_at_least(disk, owner, std::span<completion_t> {narrow}, 1) == std::size_t {1});
                std::uint32_t const tag = narrow[0].token.tag();
                failures += expect(tag < lease_depth_k);
                if (tag < lease_depth_k) ++seen[tag];
            }
            for (std::size_t index = 0; index != lease_depth_k; ++index)
                failures += expect_eq(seen[index], std::size_t {1});
            failures += expect_eq(disk.in_flight(owner), std::size_t {0});

            std::array<completion_t, reaping_room_k> completions {};
            for (std::size_t attempt = 0; attempt != 256; ++attempt)
                failures += expect(disk.reap(owner, std::span<completion_t> {completions}) == std::size_t {0});
            return failures;
        });
}

/** A full ring drained one completion at a time still answers for every request it took. */
std::size_t uring_reaps_through_a_narrow_window(uring_files_disk_t& disk, target_place_t target) {
    return on_lease(disk, lease_depth_k * block_bytes_k,
                    [&](ring_owner_t& owner, registration_t& registration, std::span<byte_t>) {
                        std::size_t failures = 0;
                        std::size_t retired = 0;
                        for (std::size_t round = 0; round != 4; ++round) {
                            std::array<request_t, lease_depth_k> batch {};
                            for (std::size_t index = 0; index != lease_depth_k; ++index)
                                batch[index] = read_request(target, extent_t {index * block_bytes_k, block_bytes_k},
                                                            registration.place(index * block_bytes_k),
                                                            owner.token(static_cast<std::uint32_t>(retired + index)));
                            failures += expect(disk.submit(owner, std::span<request_t const> {batch}) ==
                                               std::size_t {lease_depth_k});
                            std::array<completion_t, 1> narrow {};
                            for (std::size_t index = 0; index != lease_depth_k; ++index) {
                                failures += expect(reap_at_least(disk, owner, std::span<completion_t> {narrow}, 1) ==
                                                   std::size_t {1});
                                failures += expect(narrow[0].whole());
                            }
                            failures += expect_eq(disk.in_flight(owner), std::size_t {0});
                            retired += lease_depth_k;
                        }
                        failures += expect_eq(retired, 4 * std::size_t {lease_depth_k});
                        return failures;
                    });
}

/** What the disk reports is what @c statx says the filesystem under the volume demands. */
std::size_t uring_reports_the_alignment_statx_reports(uring_files_disk_t& disk, char const* path) {
    std::size_t failures = 0;
    ring_capabilities_t const limits = disk.capabilities();

    native_file_t const file = ::open(path, O_RDONLY | O_DIRECT);
    failures += expect(file != invalid_native_file_k);
    if (file == invalid_native_file_k) return failures;
    struct statx description {};
    int const described = static_cast<int>(
        ::syscall(__NR_statx, file, "", AT_EMPTY_PATH, STATX_DIOALIGN, &description));
    close_native_file(file);
    failures += expect_eq(described, 0);
    if (described != 0) return failures;

    bool const answered = (description.stx_mask & STATX_DIOALIGN) != 0 && description.stx_dio_mem_align != 0 &&
                          description.stx_dio_offset_align != 0;
    std::uint64_t const memory = answered ? description.stx_dio_mem_align : portable_alignment_k;
    std::uint64_t const offset = answered ? description.stx_dio_offset_align : portable_alignment_k;
    failures += expect_eq(limits.memory_alignment, memory);
    failures += expect_eq(limits.offset_alignment, offset);
    failures += expect_eq(limits.length_granularity, offset);
    failures += expect_eq(limits.depth_limit, lease_depth_k);
    return failures;
}

/** Both file transports report the medium behind a block device, which @c fstat alone sizes at zero. */
std::size_t device_reports_its_capacity(std::string_view device) {
    std::size_t failures = 0;
    std::uint64_t const exported = exported_device_bytes(device.data());
    failures += expect(exported != 0);
    if (exported == 0) return failures;

    target_request_t request;
    request.name = device.data();
    request.access = target_access_t::read_only_k;

    expected<blocking_files_device_t> device_transport = blocking_files_device_t::make(file_buffering_t::unbuffered_k);
    failures += expect(device_transport.has_value());
    if (!device_transport) return failures;
    disk_config_t blocking_config;
    expected<blocking_files_disk_t> blocking = blocking_files_disk_t::make(std::move(*device_transport),
                                                                           blocking_config);
    failures += expect(blocking.has_value());
    if (!blocking) return failures;
    expected<std::uint64_t> const blocking_capacity = registered_capacity(*blocking, request);
    failures += expect(blocking_capacity.has_value());
    if (blocking_capacity) failures += expect_eq(*blocking_capacity, exported);

    uring_disk_config_t uring_config;
    expected<uring_files_disk_t> uring = uring_files_disk_t::make(uring_config);
    failures += expect(uring.has_value());
    if (!uring) return failures;
    expected<std::uint64_t> const uring_capacity = registered_capacity(*uring, request);
    failures += expect(uring_capacity.has_value());
    if (uring_capacity) failures += expect_eq(*uring_capacity, exported);

    // A device keeps the capacity of its medium, so a request to create it at a length is refused.
    target_request_t sized = request;
    sized.access = target_access_t::create_read_write_k;
    sized.length = exported;
    expected<descriptor_t> refused = uring->register_target(sized);
    failures += expect(!refused.has_value());
    if (refused) failures += expect(uring->deregister_target(*refused).succeeded());
    else failures += expect_status(refused, status_code_t::invalid_argument_k);
    return failures;
}

#pragma endregion Cases

#pragma region Driver

/** Mounts the io_uring transport on a preallocated volume and runs the suite on it. */
void uring_transport(ledger_t& ledger, char const* directory) {
    uring_disk_config_t config;
    config.owners_limit = 4;
    config.depth_limit = lease_depth_k;
    config.registrations_limit = 4;
    config.targets_limit = 4;
    config.buffering = file_buffering_t::unbuffered_k;

    expected<uring_files_disk_t> disk = uring_files_disk_t::make(config);
    if (report(ledger, "uring.mount", expect_status(disk, status_code_t::success_k)) != 0) return;

    test_path_t const path {directory, "volume.bin"};
    on_target(ledger, "uring.volume", *disk, volume_request(path.c_str()), [&](target_place_t target) {
        ring_cases(ledger, "uring", *disk, target);
        report(ledger, "uring.fails_one_request", uring_fails_one_request_of_a_batch(*disk, target));
        report(ledger, "uring.cuts_a_chain_short", uring_cuts_a_chain_on_a_short_read(*disk, target));
        report(ledger, "uring.cancel_completions", uring_counts_completions_after_a_cancel(*disk, target));
        report(ledger, "uring.narrow_reap_window", uring_reaps_through_a_narrow_window(*disk, target));
        report(ledger, "uring.overflowed_queue", uring_reaps_an_overflowed_queue(*disk, target));
        report(ledger, "uring.alignment_matches_statx", uring_reports_the_alignment_statx_reports(*disk, path.c_str()));
    });

    test_path_t const borrowed_path {directory, "volume_borrowed.bin"};
    report(ledger, "uring.target_in_flight",
           ring_holds_a_target_in_flight(*disk, volume_request(borrowed_path.c_str())));
}

#pragma endregion Driver

} // namespace unum::ustore

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::nvme;

    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    ledger_t ledger = options.ledger();

    st::expected<test_directory_t> directory = test_directory_t::make(options.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    medium_t const medium = medium_t::of(options, *directory);

    if (char const* const reason = transport_refusal(transport_t::uring_k, medium))
        report(ledger, "uring", verdict_t::skipped(reason));
    else uring_transport(ledger, medium.directory);

    if (char const* const reason = transport_refusal(transport_t::device_k, medium))
        report(ledger, "uring.device_capacity", verdict_t::skipped(reason));
    else report(ledger, "uring.device_capacity", device_reports_its_capacity(medium.device));
    return conclude(ledger);
}
