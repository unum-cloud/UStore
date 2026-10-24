/**
 *  @file test/nvme/linux_uring_disk.cpp
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
#undef NDEBUG // ! A test's oracle must stay live in every build

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

#include "ustore/utilities/types.hpp" // `status_t`, `expected`

#include "ustore/utilities/nvme_disks.hpp"            // `native_file_t`, `close_native_file`, `blocking_files_disk_t`
#include "ustore/utilities/nvme_linux_uring_disk.hpp" // `linux_uring_files_disk_t`, `linux_uring_probe_t`

#include "nvme/ring_cases.hpp" // `ring_cases`, every transport-independent result

namespace unum::ustore::test {

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Fixtures

/** The bytes the kernel exports for @p device under @c /sys/class/block, in sectors of 512. */
[[nodiscard]] std::uint64_t exported_device_bytes(char const* device) noexcept {
    char const* const slash = std::strrchr(device, '/');
    char const* const name = slash ? slash + 1 : device;
    path_t const sysfs {"/sys/class/block", name};
    path_t const sectors_path {sysfs.c_str(), "size"};
    FILE* const file = std::fopen(sectors_path.c_str(), "r");
    if (!file) return 0;
    unsigned long long sectors = 0;
    if (std::fscanf(file, "%llu", &sectors) != 1) sectors = 0;
    std::fclose(file);
    return static_cast<std::uint64_t>(sectors) * 512ull;
}

/** Registers @p request on @p disk and answers the capacity the registered medium carries, giving the medium back. */
template <typename disk_type_>
expected<std::uint64_t> registered_capacity(disk_type_& disk, medium_options_t const& request) {
    auto [registered, register_medium_status] = disk.register_medium(request);
    if (register_medium_status.failed()) return register_medium_status;
    std::uint64_t const capacity = registered.capacity();
    if (status_t const released = disk.unregister_medium(registered); released.failed()) return released;
    return capacity;
}

#pragma endregion Fixtures

/**
 *  @brief Leases the ring, registers @p bytes of aligned memory, runs @p body and gives both back.
 *  @param[inout] disk The mounted ring the queue pair and the range come from.
 *  @param[in] bytes Bytes of buffer to register, which the body addresses through the registered_memory.
 *  @param[in] body Called with the queue pair, the registered memory and the buffer it covers.
 */
template <typename body_type_>
std::size_t on_queue_pair(linux_uring_files_disk_t& disk, std::size_t bytes, body_type_&& body) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(bytes);
    failures += expect_eq(buffer.size(), bytes);
    if (buffer.size() != bytes) return failures;

    expected<queue_pair_t> leased = disk.acquire_queue_pair(queue_pair_depth_k);
    failures += expect(leased.has_value());
    if (!leased) return failures;
    queue_pair_t queue_pair = std::move(*leased);

    expected<registered_memory_t> pinned = disk.register_memory(whole(buffer), memory_space_t::host_k);
    failures += expect(pinned.has_value());
    if (!pinned) {
        failures += expect(disk.release_queue_pair(queue_pair).succeeded());
        return failures;
    }
    registered_memory_t registered_memory = std::move(*pinned);

    failures += body(queue_pair, registered_memory, whole(buffer));

    failures += expect(disk.drain(queue_pair).succeeded());
    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

#pragma region Cases

/** A request the disk can refuse before the kernel sees it fails alone, and its neighbours still run. */
std::size_t uring_fails_one_request_of_a_batch(linux_uring_files_disk_t& disk, medium_reference_t medium) {
    return on_queue_pair(
        disk, 2 * block_bytes_k,
        [&](queue_pair_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
            std::size_t failures = 0;
            memory_reference_t stale = registered_memory.reference(0);
            stale.generation = static_cast<std::uint16_t>(registered_memory.generation() + 1);

            std::array<request_t, 3> batch {};
            batch[0] = read_request(medium, extent_t {0, block_bytes_k}, stale, queue_pair.token(0));
            batch[1] = read_request(medium, extent_t {1, block_bytes_k}, registered_memory.reference(0),
                                    queue_pair.token(1));
            batch[2] = read_request(medium, extent_t {0, block_bytes_k}, registered_memory.reference(block_bytes_k),
                                    queue_pair.token(2));
            failures += expect(disk.submit(queue_pair, std::span<request_t const> {batch}) == std::size_t {3});

            std::array<completion_t, polling_room_k> completions {};
            failures += expect(poll_completions_at_least<waiting_policy_t>(
                                   disk, queue_pair, std::span<completion_t> {completions}, 3) == std::size_t {3});
            std::size_t refused = 0;
            std::size_t whole = 0;
            for (std::size_t index = 0; index != 3; ++index) {
                if (completions[index].state == completion_state_t::failed_k) {
                    refused += expect_status(completions[index].status, status_code_t::invalid_argument_k) == 0 ? 1 : 0;
                    continue;
                }
                whole += completions[index].completed_in_full() ? 1 : 0;
            }
            failures += expect_eq(refused, std::size_t {2});
            failures += expect_eq(whole, std::size_t {1});
            return failures;
        });
}

/** A chain whose first read is short reports the rest cancelled, which is what cutting a link means. */
std::size_t uring_cuts_a_chain_on_a_short_read(linux_uring_files_disk_t& disk, medium_reference_t medium) {
    constexpr std::uint64_t reachable_k = 1024;
    return on_queue_pair(
        disk, 3 * block_bytes_k,
        [&](queue_pair_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
            std::size_t failures = 0;
            std::array<request_t, 3> chain {};
            chain[0] = read_request(medium, extent_t {volume_bytes_k - reachable_k, block_bytes_k},
                                    registered_memory.reference(0), queue_pair.token(0));
            chain[0].chaining = request_chaining_t::chained_k;
            chain[1] = read_request(medium, extent_t {0, block_bytes_k}, registered_memory.reference(block_bytes_k),
                                    queue_pair.token(1));
            chain[1].chaining = request_chaining_t::chained_k;
            chain[2] = read_request(medium, extent_t {block_bytes_k, block_bytes_k},
                                    registered_memory.reference(2 * block_bytes_k), queue_pair.token(2));

            failures += expect(disk.submit(queue_pair, std::span<request_t const> {chain}) == std::size_t {3});
            std::array<completion_t, polling_room_k> completions {};
            failures += expect(poll_completions_at_least<waiting_policy_t>(
                                   disk, queue_pair, std::span<completion_t> {completions}, 3) == std::size_t {3});
            failures += expect(completions[0].state == completion_state_t::partial_k);
            failures += expect_eq(completions[0].transferred_bytes, reachable_k);
            failures += expect(completions[1].state == completion_state_t::cancelled_k);
            failures += expect(completions[2].state == completion_state_t::cancelled_k);
            failures += expect_eq(completions[1].token.tag(), std::uint32_t {1});
            failures += expect_eq(completions[2].token.tag(), std::uint32_t {2});
            return failures;
        });
}

/** A cancellation mid-flight delivers one completion per submitted request, and never one extra. */
std::size_t uring_counts_completions_after_a_cancel(linux_uring_files_disk_t& disk, medium_reference_t medium) {
    return on_queue_pair(
        disk, queue_pair_depth_k * block_bytes_k,
        [&](queue_pair_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
            std::size_t failures = 0;
            std::array<request_t, queue_pair_depth_k> batch {};
            for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
                batch[index] = read_request(medium, extent_t {index * block_bytes_k, block_bytes_k},
                                            registered_memory.reference(index * block_bytes_k),
                                            queue_pair.token(static_cast<std::uint32_t>(index)));
            failures += expect(disk.submit(queue_pair, std::span<request_t const> {batch}) ==
                               std::size_t {queue_pair_depth_k});
            disk.cancel(queue_pair);

            std::array<completion_t, polling_room_k> completions {};
            failures += expect(poll_completions_at_least<waiting_policy_t>(
                                   disk, queue_pair, std::span<completion_t> {completions}, queue_pair_depth_k) ==
                               std::size_t {queue_pair_depth_k});
            std::array<std::size_t, queue_pair_depth_k> seen {};
            for (std::size_t index = 0; index != queue_pair_depth_k; ++index) {
                std::uint32_t const tag = completions[index].token.tag();
                failures += expect(tag < queue_pair_depth_k);
                if (tag < queue_pair_depth_k) ++seen[tag];
            }
            for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
                failures += expect_eq(seen[index], std::size_t {1});
            failures += expect_eq(disk.outstanding(queue_pair), std::size_t {0});

            for (std::size_t attempt = 0; attempt != 64; ++attempt)
                failures += expect(disk.poll_completions(queue_pair, std::span<completion_t> {completions}) ==
                                   std::size_t {0});
            return failures;
        });
}

/**
 *  @brief More completions than the queue holds still all arrive, which is what overflow costs.
 *
 *  The mount sizes the completion queue at four times the credit, so a queue pair alone can never fill
 *  it; what can is the completion every cancelling entry posts for itself. Asking to cancel far more
 *  often than the queue has seats puts the kernel over the edge, and from there every completion
 *  sits in the kernel's overflow list until a poll enters again to flush it.
 */
std::size_t uring_polls_an_overflowed_queue(linux_uring_files_disk_t& disk, medium_reference_t medium) {
    constexpr std::size_t cancels_k = 64;
    return on_queue_pair(
        disk, queue_pair_depth_k * block_bytes_k,
        [&](queue_pair_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
            std::size_t failures = 0;
            std::array<request_t, queue_pair_depth_k> batch {};
            for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
                batch[index] = read_request(medium, extent_t {index * block_bytes_k, block_bytes_k},
                                            registered_memory.reference(index * block_bytes_k),
                                            queue_pair.token(static_cast<std::uint32_t>(index)));
            failures += expect(disk.submit(queue_pair, std::span<request_t const> {batch}) ==
                               std::size_t {queue_pair_depth_k});
            for (std::size_t attempt = 0; attempt != cancels_k; ++attempt) disk.cancel(queue_pair);

            std::array<completion_t, 1> narrow {};
            std::array<std::size_t, queue_pair_depth_k> seen {};
            for (std::size_t index = 0; index != queue_pair_depth_k; ++index) {
                failures += expect(poll_completions_at_least<waiting_policy_t>(
                                       disk, queue_pair, std::span<completion_t> {narrow}, 1) == std::size_t {1});
                std::uint32_t const tag = narrow[0].token.tag();
                failures += expect(tag < queue_pair_depth_k);
                if (tag < queue_pair_depth_k) ++seen[tag];
            }
            for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
                failures += expect_eq(seen[index], std::size_t {1});
            failures += expect_eq(disk.outstanding(queue_pair), std::size_t {0});

            std::array<completion_t, polling_room_k> completions {};
            for (std::size_t attempt = 0; attempt != 256; ++attempt)
                failures += expect(disk.poll_completions(queue_pair, std::span<completion_t> {completions}) ==
                                   std::size_t {0});
            return failures;
        });
}

/** A full ring drained one completion at a time still answers for every request it took. */
std::size_t uring_polls_through_a_narrow_window(linux_uring_files_disk_t& disk, medium_reference_t medium) {
    return on_queue_pair(
        disk, queue_pair_depth_k * block_bytes_k,
        [&](queue_pair_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
            std::size_t failures = 0;
            std::size_t retired = 0;
            for (std::size_t round = 0; round != 4; ++round) {
                std::array<request_t, queue_pair_depth_k> batch {};
                for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
                    batch[index] = read_request(medium, extent_t {index * block_bytes_k, block_bytes_k},
                                                registered_memory.reference(index * block_bytes_k),
                                                queue_pair.token(static_cast<std::uint32_t>(retired + index)));
                failures += expect(disk.submit(queue_pair, std::span<request_t const> {batch}) ==
                                   std::size_t {queue_pair_depth_k});
                std::array<completion_t, 1> narrow {};
                for (std::size_t index = 0; index != queue_pair_depth_k; ++index) {
                    failures += expect(poll_completions_at_least<waiting_policy_t>(
                                           disk, queue_pair, std::span<completion_t> {narrow}, 1) == std::size_t {1});
                    failures += expect(narrow[0].completed_in_full());
                }
                failures += expect_eq(disk.outstanding(queue_pair), std::size_t {0});
                retired += queue_pair_depth_k;
            }
            failures += expect_eq(retired, 4 * std::size_t {queue_pair_depth_k});
            return failures;
        });
}

/** What the disk reports is what @c statx says the filesystem under the volume demands. */
std::size_t uring_reports_the_alignment_statx_reports(linux_uring_files_disk_t& disk, char const* path) {
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
    failures += expect_eq(limits.depth_limit, queue_pair_depth_k);
    return failures;
}

/** Both file transports report the medium behind a block device, which @c fstat alone sizes at zero. */
std::size_t device_reports_its_capacity(std::string_view device) {
    std::size_t failures = 0;
    std::uint64_t const exported = exported_device_bytes(device.data());
    failures += expect(exported != 0);
    if (exported == 0) return failures;

    medium_options_t request;
    request.name = device.data();
    request.access = medium_access_t::read_only_k;

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

    linux_uring_disk_config_t uring_config;
    expected<linux_uring_files_disk_t> uring = linux_uring_files_disk_t::make(uring_config);
    failures += expect(uring.has_value());
    if (!uring) return failures;
    expected<std::uint64_t> const uring_capacity = registered_capacity(*uring, request);
    failures += expect(uring_capacity.has_value());
    if (uring_capacity) failures += expect_eq(*uring_capacity, exported);

    // A device keeps the capacity of its environment, so a request to create it at a length is refused.
    medium_options_t sized = request;
    sized.access = medium_access_t::create_read_write_k;
    sized.length = exported;
    expected<registered_medium_t> refused = uring->register_medium(sized);
    failures += expect(!refused.has_value());
    if (refused) failures += expect(uring->unregister_medium(*refused).succeeded());
    else failures += expect_status(refused, status_code_t::invalid_argument_k);
    return failures;
}

#pragma endregion Cases

#pragma region Driver

/** Mounts the io_uring transport on a preallocated volume and runs the suite on it. */
void uring_transport(suite_totals_t& totals, char const* directory) {
    linux_uring_disk_config_t config;
    config.queue_pairs_limit = 4;
    config.depth_limit = queue_pair_depth_k;
    config.registered_memory_limit = 4;
    config.media_limit = 4;
    config.buffering = file_buffering_t::unbuffered_k;

    expected<linux_uring_files_disk_t> disk = linux_uring_files_disk_t::make(config);
    if (report(totals, "linux-uring.mount", expect_status(disk, status_code_t::success_k)) != 0) return;

    path_t const path {directory, "volume.bin"};
    on_medium(totals, "linux-uring.volume", *disk, volume_request(path.c_str()), [&](medium_reference_t medium) {
        ring_cases(totals, "linux-uring", *disk, medium);
        report(totals, "linux-uring.fails_one_request", uring_fails_one_request_of_a_batch, *disk, medium);
        report(totals, "linux-uring.cuts_a_chain_short", uring_cuts_a_chain_on_a_short_read, *disk, medium);
        report(totals, "linux-uring.cancel_completions", uring_counts_completions_after_a_cancel, *disk, medium);
        report(totals, "linux-uring.narrow_poll_window", uring_polls_through_a_narrow_window, *disk, medium);
        report(totals, "linux-uring.overflowed_queue", uring_polls_an_overflowed_queue, *disk, medium);
        report(totals, "linux-uring.alignment_matches_statx", uring_reports_the_alignment_statx_reports, *disk,
               path.c_str());
    });

    path_t const borrowed_path {directory, "volume_borrowed.bin"};
    report(totals, "linux-uring.medium_in_flight", ring_holds_a_medium_in_flight<linux_uring_files_disk_t>, *disk,
           volume_request(borrowed_path.c_str()));
}

#pragma endregion Driver

} // namespace unum::ustore::test

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;
    using namespace unum::ustore::blobs;
    using namespace unum::ustore::utilities;

    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    st::expected<directory_t> directory = directory_t::make(options.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    test_environment_t const environment = test_environment_t::of(options, *directory);

    if (char const* const reason = transport_refusal(transport_t::linux_uring_k, environment))
        report(totals, "linux-uring", case_result_t::skipped(reason));
    else uring_transport(totals, environment.directory);

    if (char const* const reason = transport_refusal(transport_t::device_k, environment))
        report(totals, "linux-uring.device_capacity", case_result_t::skipped(reason));
    else report(totals, "linux-uring.device_capacity", device_reports_its_capacity, environment.device);
    return suite_exit_code(totals);
}
