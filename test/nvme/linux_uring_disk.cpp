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

#include <fcntl.h>        // `AT_EMPTY_PATH`
#include <linux/stat.h>   // `struct statx`, `STATX_DIOALIGN`
#include <sys/resource.h> // `getrlimit`, `RLIMIT_MEMLOCK`
#include <sys/syscall.h>  // `__NR_statx`
#include <unistd.h>       // `syscall`

#include <array>       // `std::array`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include "ustore/utilities/types.hpp" // `status_t`, `expected`

#include "ustore/utilities/nvme_disks.hpp"            // `native_file_t`, `close_native_file`, `blocking_files_disk_t`
#include "ustore/utilities/nvme_linux_uring_disk.hpp" // `linux_uring_files_disk_t`

#include "nvme/ring_cases.hpp" // `ring_cases`, every transport-independent result

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Fixtures

/** The bytes the kernel exports for @p device under @c /sys/class/block, in sectors of 512. */
[[nodiscard]] std::uint64_t exported_device_bytes(char const* device) noexcept {
    char const* const slash = std::strrchr(device, '/');
    char const* const name = slash ? slash + 1 : device;
    std::string const sysfs = fmt::format("{}/{}", "/sys/class/block", name);
    std::string const sectors_path = fmt::format("{}/{}", sysfs.c_str(), "size");
    FILE* const file = std::fopen(sectors_path.c_str(), "r");
    if (!file) return 0;
    unsigned long long sectors = 0;
    if (std::fscanf(file, "%llu", &sectors) != 1) sectors = 0;
    std::fclose(file);
    return static_cast<std::uint64_t>(sectors) * 512ull;
}

/** Registers @p request on @p disk and answers the capacity the registered medium holds, giving
 *  the medium back. */
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
 *  @brief Makes a queue holding @p disk, registers @p bytes of aligned memory, runs @p body and
 *      gives both back.
 *  @param[in,out] disk The mounted disk the range is registered on.
 *  @param[in] bytes Bytes of buffer to register, which the body addresses through the registered_memory.
 *  @param[in] body Called with the queue pair, the registered memory and the buffer it covers.
 */
template <typename body_type_>
void on_queue_pair(linux_uring_files_disk_t& disk, std::size_t bytes, body_type_&& body) {
    test_buffer_t buffer = make_buffer(bytes);
    expect_equal(buffer.size(), bytes);
    if (buffer.size() != bytes) return;

    auto [queue, make_queue_status] = linux_uring_queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    queue_ref_t queue_pair {queue, queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    expected<registered_memory_t> pinned = disk.register_memory(whole(buffer), memory_space_t::host_k);
    expect_true(pinned.has_value());
    if (!pinned) return;
    registered_memory_t registered_memory = std::move(*pinned);

    body(queue_pair, registered_memory, whole(buffer));

    expect_true(disk.drain(queue_pair).succeeded());
    expect_true(disk.unregister_memory(registered_memory).succeeded());
}

#pragma region Cases

/** A request the disk can refuse before the kernel sees it fails alone, and its neighbours still run. */
void test_uring_fails_one_request_of_a_batch(linux_uring_files_disk_t& disk, medium_reference_t medium) {
    on_queue_pair(
        disk, 2 * block_bytes_k,
        [&](queue_ref_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
            memory_reference_t stale = registered_memory.reference(0);
            stale.generation = static_cast<std::uint16_t>(registered_memory.generation() + 1);

            std::array<request_t, 3> batch {};
            batch[0] = read_request(medium, extent_t {0, block_bytes_k}, stale, queue_pair.token(0));
            batch[1] = read_request(medium, extent_t {1, block_bytes_k}, registered_memory.reference(0),
                                    queue_pair.token(1));
            batch[2] = read_request(medium, extent_t {0, block_bytes_k}, registered_memory.reference(block_bytes_k),
                                    queue_pair.token(2));
            expect_true(disk.submit(queue_pair, std::span<request_t const> {batch}) == std::size_t {3});

            std::array<completion_t, polling_room_k> completions {};
            expect_true(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {completions},
                                                            3) == std::size_t {3});
            std::size_t refused = 0;
            std::size_t whole = 0;
            for (std::size_t index = 0; index != 3; ++index) {
                if (completions[index].state == completion_state_t::failed_k) {
                    refused += expect_status(completions[index].status, status_code_t::invalid_argument_k) ? 1 : 0;
                    continue;
                }
                whole += completions[index].completed_in_full() ? 1 : 0;
            }
            expect_equal(refused, std::size_t {2});
            expect_equal(whole, std::size_t {1});
        });
}

/** A chain whose first read is short reports the rest cancelled, which is what cutting a link means. */
void test_uring_cuts_a_chain_on_a_short_read(linux_uring_files_disk_t& disk, medium_reference_t medium) {
    constexpr std::uint64_t reachable_k = 1024;
    on_queue_pair(disk, 3 * block_bytes_k,
                  [&](queue_ref_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
                      std::array<request_t, 3> chain {};
                      chain[0] = read_request(medium, extent_t {volume_bytes_k - reachable_k, block_bytes_k},
                                              registered_memory.reference(0), queue_pair.token(0));
                      chain[0].chaining = request_chaining_t::chained_k;
                      chain[1] = read_request(medium, extent_t {0, block_bytes_k},
                                              registered_memory.reference(block_bytes_k), queue_pair.token(1));
                      chain[1].chaining = request_chaining_t::chained_k;
                      chain[2] = read_request(medium, extent_t {block_bytes_k, block_bytes_k},
                                              registered_memory.reference(2 * block_bytes_k), queue_pair.token(2));

                      expect_true(disk.submit(queue_pair, std::span<request_t const> {chain}) == std::size_t {3});
                      std::array<completion_t, polling_room_k> completions {};
                      expect_true(poll_completions_at_least<waiter_t>(
                                      disk, queue_pair, std::span<completion_t> {completions}, 3) == std::size_t {3});
                      expect_true(completions[0].state == completion_state_t::partial_k);
                      expect_equal(completions[0].transferred_bytes, reachable_k);
                      expect_true(completions[1].state == completion_state_t::cancelled_k);
                      expect_true(completions[2].state == completion_state_t::cancelled_k);
                      expect_equal(completions[1].token.tag(), std::uint32_t {1});
                      expect_equal(completions[2].token.tag(), std::uint32_t {2});
                  });
}

/** A cancellation mid-flight delivers one completion per submitted request, and never one extra. */
void test_uring_counts_completions_after_a_cancel(linux_uring_files_disk_t& disk, medium_reference_t medium) {
    on_queue_pair(
        disk, queue_pair_depth_k * block_bytes_k,
        [&](queue_ref_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
            std::array<request_t, queue_pair_depth_k> batch {};
            for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
                batch[index] = read_request(medium, extent_t {index * block_bytes_k, block_bytes_k},
                                            registered_memory.reference(index * block_bytes_k),
                                            queue_pair.token(static_cast<std::uint32_t>(index)));
            expect_true(disk.submit(queue_pair, std::span<request_t const> {batch}) ==
                        std::size_t {queue_pair_depth_k});
            disk.cancel(queue_pair);

            std::array<completion_t, polling_room_k> completions {};
            expect_true(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {completions},
                                                            queue_pair_depth_k) == std::size_t {queue_pair_depth_k});
            std::array<std::size_t, queue_pair_depth_k> seen {};
            for (std::size_t index = 0; index != queue_pair_depth_k; ++index) {
                std::uint32_t const tag = completions[index].token.tag();
                expect_true(tag < queue_pair_depth_k);
                if (tag < queue_pair_depth_k) ++seen[tag];
            }
            for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
                expect_equal(seen[index], std::size_t {1});
            expect_equal(disk.outstanding(queue_pair), std::size_t {0});

            for (std::size_t attempt = 0; attempt != 64; ++attempt)
                expect_true(disk.poll_completions(queue_pair, std::span<completion_t> {completions}) ==
                            std::size_t {0});
        });
}

/**
 *  @brief More completions than the queue holds still all arrive, which is what overflow costs.
 *
 *  The mount sizes the completion queue at four times the credit, so a queue pair alone can never
 *  fill it; what can is the completion every cancelling entry posts for itself. Asking to cancel
 *  far more often than the queue has seats puts the kernel over the edge, and from there every
 *  completion sits in the kernel's overflow list until a poll enters again to flush it.
 */
void test_uring_polls_an_overflowed_queue(linux_uring_files_disk_t& disk, medium_reference_t medium) {
    constexpr std::size_t cancels_k = 64;
    on_queue_pair(disk, queue_pair_depth_k * block_bytes_k,
                  [&](queue_ref_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
                      std::array<request_t, queue_pair_depth_k> batch {};
                      for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
                          batch[index] = read_request(medium, extent_t {index * block_bytes_k, block_bytes_k},
                                                      registered_memory.reference(index * block_bytes_k),
                                                      queue_pair.token(static_cast<std::uint32_t>(index)));
                      expect_true(disk.submit(queue_pair, std::span<request_t const> {batch}) ==
                                  std::size_t {queue_pair_depth_k});
                      for (std::size_t attempt = 0; attempt != cancels_k; ++attempt) disk.cancel(queue_pair);

                      std::array<completion_t, 1> narrow {};
                      std::array<std::size_t, queue_pair_depth_k> seen {};
                      for (std::size_t index = 0; index != queue_pair_depth_k; ++index) {
                          expect_true(poll_completions_at_least<waiter_t>(
                                          disk, queue_pair, std::span<completion_t> {narrow}, 1) == std::size_t {1});
                          std::uint32_t const tag = narrow[0].token.tag();
                          expect_true(tag < queue_pair_depth_k);
                          if (tag < queue_pair_depth_k) ++seen[tag];
                      }
                      for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
                          expect_equal(seen[index], std::size_t {1});
                      expect_equal(disk.outstanding(queue_pair), std::size_t {0});

                      std::array<completion_t, polling_room_k> completions {};
                      for (std::size_t attempt = 0; attempt != 256; ++attempt)
                          expect_true(disk.poll_completions(queue_pair, std::span<completion_t> {completions}) ==
                                      std::size_t {0});
                  });
}

/** A full ring drained one completion at a time still answers for every request it took. */
void test_uring_polls_through_a_narrow_window(linux_uring_files_disk_t& disk, medium_reference_t medium) {
    on_queue_pair(
        disk, queue_pair_depth_k * block_bytes_k,
        [&](queue_ref_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
            std::size_t retired = 0;
            for (std::size_t round = 0; round != 4; ++round) {
                std::array<request_t, queue_pair_depth_k> batch {};
                for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
                    batch[index] = read_request(medium, extent_t {index * block_bytes_k, block_bytes_k},
                                                registered_memory.reference(index * block_bytes_k),
                                                queue_pair.token(static_cast<std::uint32_t>(retired + index)));
                expect_true(disk.submit(queue_pair, std::span<request_t const> {batch}) ==
                            std::size_t {queue_pair_depth_k});
                std::array<completion_t, 1> narrow {};
                for (std::size_t index = 0; index != queue_pair_depth_k; ++index) {
                    expect_true(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {narrow},
                                                                    1) == std::size_t {1});
                    expect_true(narrow[0].completed_in_full());
                }
                expect_equal(disk.outstanding(queue_pair), std::size_t {0});
                retired += queue_pair_depth_k;
            }
            expect_equal(retired, 4 * std::size_t {queue_pair_depth_k});
        });
}

/** What the disk reports is what @c statx says the filesystem under the volume demands. */
void test_uring_reports_the_alignment_statx_reports(linux_uring_files_disk_t& disk, char const* path) {
    disk_capabilities_t const limits = disk.capabilities();

    native_file_t const file = ::open(path, O_RDONLY | O_DIRECT);
    expect_true(file != invalid_native_file_k);
    if (file == invalid_native_file_k) return;
    struct statx description {};
    int const described = static_cast<int>(
        ::syscall(__NR_statx, file, "", AT_EMPTY_PATH, STATX_DIOALIGN, &description));
    [[maybe_unused]] status_t const closed = close_native_file(file);
    expect_equal(described, 0);
    if (described != 0) return;

    bool const answered = (description.stx_mask & STATX_DIOALIGN) != 0 && description.stx_dio_mem_align != 0 &&
                          description.stx_dio_offset_align != 0;
    std::uint64_t const memory = answered ? description.stx_dio_mem_align : portable_alignment_k;
    std::uint64_t const offset = answered ? description.stx_dio_offset_align : portable_alignment_k;
    expect_equal(limits.memory_alignment, memory);
    expect_equal(limits.offset_alignment, offset);
    expect_equal(limits.length_granularity, offset);
}

/** A queue's blocks past one gibibyte pin as a run of table slots: a transfer inside the second
 *  slot moves through it, and one across the boundary moves unpinned, which the disk counts. */
void test_uring_pins_a_range_past_a_gibibyte(linux_uring_files_disk_t& disk, medium_reference_t medium) {
    std::size_t const boundary = linux_uring_registered_range_bytes_k;
    test_buffer_t blocks = make_buffer(boundary + 2 * block_bytes_k);
    expect_equal(blocks.size(), boundary + 2 * block_bytes_k);
    if (blocks.size() != boundary + 2 * block_bytes_k) return;
    std::span<byte_t> const buffer = whole(blocks);
    auto [queue, make_queue_status] = linux_uring_queue_t::make(queue_pair_depth_k, buffer);
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    queue_ref_t const queue_pair {queue, queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());
    auto const own = [](std::size_t offset) noexcept {
        return memory_reference_t {registered_memory_id_t::queue_range_k, 0, offset};
    };

    test_buffer_t expected = make_buffer(2 * block_bytes_k);
    paint(whole(expected), 3);
    std::span<byte_t> const across = buffer.subspan(boundary - block_bytes_k, 2 * block_bytes_k);
    paint(across, 3);

    std::array<completion_t, polling_room_k> completions {};
    auto const moved_in_full = [&](std::array<request_t, 2> const& batch) {
        expect_true(disk.submit(queue_pair, std::span<request_t const> {batch}) == std::size_t {2});
        expect_true(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {completions}, 2) ==
                    std::size_t {2});
        expect_true(completions[0].completed_in_full() && completions[1].completed_in_full());
    };
    moved_in_full({
        write_request(medium, extent_t {block_bytes_k, 2 * block_bytes_k}, own(boundary - block_bytes_k),
                      queue_pair.token(0)),
        write_request(medium, extent_t {0, block_bytes_k}, own(boundary), queue_pair.token(1)),
    });
    std::memset(buffer.data() + boundary - block_bytes_k, 0, 3 * block_bytes_k);
    moved_in_full({
        read_request(medium, extent_t {block_bytes_k, 2 * block_bytes_k}, own(boundary - block_bytes_k),
                     queue_pair.token(2)),
        read_request(medium, extent_t {0, block_bytes_k}, own(boundary + block_bytes_k), queue_pair.token(3)),
    });
    expect_true(std::memcmp(across.data(), expected.data(), 2 * block_bytes_k) == 0);
    expect_true(std::memcmp(buffer.data() + boundary + block_bytes_k, expected.data() + block_bytes_k, block_bytes_k) ==
                0);

    struct rlimit locked {};
    if (::getrlimit(RLIMIT_MEMLOCK, &locked) == 0 && locked.rlim_cur == RLIM_INFINITY)
        expect_true(queue.pinning() == staging_pinning_t::pinned_k);
    std::uint64_t const crossed = queue.pinning() == staging_pinning_t::pinned_k ? 2 : 0;
    expect_equal(queue.requests_across_registered_slots(), crossed);
    expect_true(disk.drain(queue_pair).succeeded());
}

/** A warm hold writes nothing: holding the same disk again leaves the queue's file table as it
 *  was, and only a medium registered in between makes the next registration rewrite it. */
void test_uring_registers_a_disk_once(linux_uring_files_disk_t& disk, char const* path) {
    auto [queue, make_queue_status] = linux_uring_queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    queue_ref_t const queue_pair {queue, queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());
    media_list_key_t const first = queue.media_list_key();
    expect_true(first.identity % 2 == 1);
    expect_true(disk.register_media(queue_pair).succeeded());
    expect_true(queue.media_list_key() == first);

    expected<registered_medium_t> registered = disk.register_medium(volume_request(path));
    expect_true(registered.has_value());
    if (!registered) return;
    expect_true(disk.register_media(queue_pair).succeeded());
    expect_true(queue.media_list_key().identity == first.identity);
    expect_true(queue.media_list_key().epoch != first.epoch);
    expect_true(disk.unregister_medium(*registered).succeeded());
}

/** A blocking queue, which a call without an executor reads through, moves bytes on an io_uring
 *  disk with positioned transfers and no ring. */
void test_uring_serves_a_blocking_queue(linux_uring_files_disk_t& disk, medium_reference_t medium) {
    test_buffer_t blocks = make_buffer(2 * block_bytes_k);
    expect_equal(blocks.size(), 2 * block_bytes_k);
    if (blocks.size() != 2 * block_bytes_k) return;
    auto [queue, make_queue_status] = blocking_queue_t::make(queue_pair_depth_k, whole(blocks));
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    queue_ref_t const queue_pair {queue, queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    paint(whole(blocks).first(block_bytes_k), 5);
    std::array<request_t, 2> batch {
        write_request(medium, extent_t {0, block_bytes_k},
                      memory_reference_t {registered_memory_id_t::queue_range_k, 0, 0}, queue_pair.token(0)),
        read_request(medium, extent_t {0, block_bytes_k},
                     memory_reference_t {registered_memory_id_t::queue_range_k, 0, block_bytes_k}, queue_pair.token(1)),
    };
    batch[0].chaining = request_chaining_t::chained_k;
    expect_true(disk.submit(queue_pair, std::span<request_t const> {batch}) == std::size_t {2});
    std::array<completion_t, polling_room_k> completions {};
    expect_true(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {completions}, 2) ==
                std::size_t {2});
    expect_true(completions[0].completed_in_full() && completions[1].completed_in_full());
    expect_true(std::memcmp(blocks.data(), blocks.data() + block_bytes_k, block_bytes_k) == 0);
}

/** Both file transports report the medium behind a block device, which @c fstat alone sizes at zero. */
void test_device_reports_its_capacity(std::string_view device) {
    std::uint64_t const exported = exported_device_bytes(device.data());
    expect_true(exported != 0);
    if (exported == 0) return;

    medium_options_t request;
    request.name = device.data();
    request.access = medium_access_t::read_only_k;

    expected<blocking_files_device_t> device_transport = blocking_files_device_t::make(file_buffering_t::unbuffered_k);
    expect_true(device_transport.has_value());
    if (!device_transport) return;
    disk_config_t blocking_config;
    resource_constraint_t budget {unlimited_resource_k};
    expected<blocking_files_disk_t> blocking = blocking_files_disk_t::make(std::move(*device_transport),
                                                                           blocking_config, budget);
    expect_true(blocking.has_value());
    if (!blocking) return;
    expected<std::uint64_t> const blocking_capacity = registered_capacity(*blocking, request);
    expect_true(blocking_capacity.has_value());
    if (blocking_capacity) expect_equal(*blocking_capacity, exported);

    linux_uring_disk_config_t uring_config;
    expected<linux_uring_files_disk_t> uring = linux_uring_files_disk_t::make(uring_config);
    expect_true(uring.has_value());
    if (!uring) return;
    expected<std::uint64_t> const uring_capacity = registered_capacity(*uring, request);
    expect_true(uring_capacity.has_value());
    if (uring_capacity) expect_equal(*uring_capacity, exported);

    // A device keeps the capacity of its environment, so a request to create it at a length is refused.
    medium_options_t sized = request;
    sized.access = medium_access_t::create_read_write_k;
    sized.length = exported;
    expected<registered_medium_t> refused = uring->register_medium(sized);
    expect_true(!refused.has_value());
    if (refused) expect_true(uring->unregister_medium(*refused).succeeded());
    else expect_status(refused, status_code_t::invalid_argument_k);
}

#pragma endregion Cases

#pragma region Driver

/** Mounts the io_uring transport on a preallocated volume and runs the suite on it. */
void uring_transport(char const* directory) {
    linux_uring_disk_config_t config;
    config.registered_memory_limit = 4;
    config.media_limit = 4;
    config.buffering = file_buffering_t::unbuffered_k;

    expected<linux_uring_files_disk_t> disk = linux_uring_files_disk_t::make(config);
    run_test_case("linux_uring_files_disk_t::make", [&] { expect_status(disk, status_code_t::success_k); });
    if (!disk) return;

    std::string const path = fmt::format("{}/{}", directory, "volume.bin");
    std::string const kept_path = fmt::format("{}/{}", directory, "volume_kept.bin");
    on_medium("linux_uring_files_disk_t::register_medium", *disk, volume_request(path.c_str()),
              [&](medium_reference_t medium) {
                  ring_cases("linux_uring_files_disk_t", *disk, medium);
                  run_test_case("test_uring_fails_one_request_of_a_batch", test_uring_fails_one_request_of_a_batch,
                                *disk, medium);
                  run_test_case("test_uring_cuts_a_chain_on_a_short_read", test_uring_cuts_a_chain_on_a_short_read,
                                *disk, medium);
                  run_test_case("test_uring_counts_completions_after_a_cancel",
                                test_uring_counts_completions_after_a_cancel, *disk, medium);
                  run_test_case("test_uring_polls_through_a_narrow_window", test_uring_polls_through_a_narrow_window,
                                *disk, medium);
                  run_test_case("test_uring_polls_an_overflowed_queue", test_uring_polls_an_overflowed_queue, *disk,
                                medium);
                  run_test_case("test_uring_pins_a_range_past_a_gibibyte", test_uring_pins_a_range_past_a_gibibyte,
                                *disk, medium);
                  run_test_case("test_uring_registers_a_disk_once", test_uring_registers_a_disk_once, *disk,
                                kept_path.c_str());
                  run_test_case("test_uring_serves_a_blocking_queue", test_uring_serves_a_blocking_queue, *disk,
                                medium);
                  run_test_case("test_uring_reports_the_alignment_statx_reports",
                                test_uring_reports_the_alignment_statx_reports, *disk, path.c_str());
              });

    std::string const viewed_path = fmt::format("{}/{}", directory, "volume_viewed.bin");
    run_test_case("test_ring_holds_a_medium_in_flight<linux_uring_files_disk_t>",
                  test_ring_holds_a_medium_in_flight<linux_uring_files_disk_t>, *disk,
                  volume_request(viewed_path.c_str()));
    std::string const deallocated_path = fmt::format("{}/{}", directory, "volume_deallocated.bin");
    run_test_case("test_ring_deallocates_a_file_extent<linux_uring_files_disk_t>",
                  test_ring_deallocates_a_file_extent<linux_uring_files_disk_t>, *disk,
                  volume_request(deallocated_path.c_str()));
    std::string const logical_block_path = fmt::format("{}/{}", directory, "volume_logical_block.bin");
    run_test_case("test_ring_reports_a_logical_block<linux_uring_files_disk_t>",
                  test_ring_reports_a_logical_block<linux_uring_files_disk_t>, *disk,
                  volume_request(logical_block_path.c_str()));
}

#pragma endregion Driver

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;
    using namespace unum::ustore::blobs;
    using namespace unum::ustore::utilities;

    start_test_run(argc, argv);

    st::expected<scratch_directory_t> directory = scratch_directory_t::make(test_run.settings.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }

    if (char const* const reason = transport_refusal(transport_t::linux_uring_k, directory->path()))
        run_test_case("linux_uring_files_disk_t::make", [reason] { return case_result_t::skipped(reason); });
    else uring_transport(directory->path());

    if (char const* const reason = transport_refusal(transport_t::device_k, directory->path()))
        run_test_case("test_device_reports_its_capacity", [reason] { return case_result_t::skipped(reason); });
    else run_test_case("test_device_reports_its_capacity", test_device_reports_its_capacity, test_run.settings.device);
    return finish_test_run();
}
