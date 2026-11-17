/**
 *  @file test/nvme/ring.cpp
 *  @author Ash Vardanian
 *  @date September 18, 2026
 *  @brief The ring contract, run against the RAM transport and the blocking file transport alike.
 *
 *  Every case is written once against @c ring_concept and run on both disks, since a transport that
 *  passes only its own suite proves nothing about the contract. The file cases need @c --directory
 *  and want a real filesystem: tmpfs accepts @c O_DIRECT and ignores alignment outright, so a volume
 *  in a scratch directory hides exactly the mistakes the alignment case looks for.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <utility> // `std::move`

#include "ustore/utilities/types.hpp" // `expected`

#include "ustore/utilities/nvme_disks.hpp"            // `blocking_files_disk_t`
#include "ustore/utilities/nvme_linux_aio_disk.hpp"   // `linux_aio_files_disk`
#include "ustore/utilities/nvme_linux_spdk_disk.hpp"  // `linux_spdk_disk`
#include "ustore/utilities/nvme_linux_uring_disk.hpp" // `linux_uring_files_disk`
#include "ustore/utilities/nvme_mounts.hpp"           // `any_files_disk`

#include "nvme/ring_cases.hpp" // `ring_cases`, `test_ring_fails_a_misaligned_request`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

static_assert(ring_concept<linux_spdk_disk<atomic_reference, waiter_t>>,
              "The SPDK transport is a ring like every other");
static_assert(ring_concept<linux_uring_files_disk<atomic_reference, waiter_t>>,
              "The io_uring transport is a ring like every other");
static_assert(ring_concept<linux_aio_files_disk<atomic_reference, waiter_t>>,
              "The Linux AIO transport is a ring like every other");
static_assert(ring_concept<any_files_disk<atomic_reference, waiter_t>>,
              "Any file transport is a ring like every other");

#pragma region Transport Drivers

/** Mounts the RAM transport and runs the suite on it, with and without demanded alignment. */
void memory_transport() {
    resource_constraint_t budget {unlimited_resource_k};
    disk_config_t config;
    config.queue_pairs_limit = 4;
    config.depth_limit = queue_pair_depth_k;
    config.registered_memory_limit = 4;
    config.media_limit = 4;

    auto [device, make_device_status] = memory_device_t::make();
    expected<memory_disk_t> disk = make_device_status.failed() ? make_device_status
                                                               : memory_disk_t::make(std::move(device), config, budget);
    run_test_case("memory_disk_t::make", [&] { expect_true(disk.has_value()); });
    if (!disk) return;
    on_medium("memory_disk_t::register_medium", *disk, volume_request("memory"),
              [&](medium_reference_t medium) { ring_cases("memory_disk_t", *disk, medium); });
    run_test_case("test_ring_holds_a_medium_in_flight<memory_disk_t>",
                  test_ring_holds_a_medium_in_flight<memory_disk_t>, *disk, volume_request("memory_borrowed"));

    auto [strict_device, make_strict_device_status] = memory_device_t::make(portable_alignment_k);
    expected<memory_disk_t> strict_disk = make_strict_device_status.failed()
                                              ? make_strict_device_status
                                              : memory_disk_t::make(std::move(strict_device), config, budget);
    run_test_case("memory_disk_t::make(portable_alignment_k)", [&] { expect_true(strict_disk.has_value()); });
    if (!strict_disk) return;
    on_medium("memory_disk_t::register_medium(portable_alignment_k)", *strict_disk, volume_request("memory_strict"),
              [&](medium_reference_t medium) {
                  run_test_case("test_ring_fails_a_misaligned_request<memory_disk_t>",
                                test_ring_fails_a_misaligned_request<memory_disk_t>, *strict_disk, medium);
              });
}

/** Mounts the blocking file transport on a preallocated volume and runs the suite on it. */
void blocking_files_transport(char const* directory) {
    resource_constraint_t budget {unlimited_resource_k};
    disk_config_t config;
    config.queue_pairs_limit = 4;
    config.depth_limit = queue_pair_depth_k;
    config.registered_memory_limit = 4;
    config.media_limit = 4;

    auto [device, make_device_status] = blocking_files_device_t::make(file_buffering_t::buffered_k);
    expected<blocking_files_disk_t> disk = make_device_status.failed()
                                               ? make_device_status
                                               : blocking_files_disk_t::make(std::move(device), config, budget);
    run_test_case("blocking_files_disk_t::make(file_buffering_t::buffered_k)", [&] { expect_true(disk.has_value()); });
    if (!disk) return;
    std::string const path = fmt::format("{}/{}", directory, "volume.bin");
    on_medium("blocking_files_disk_t::register_medium(file_buffering_t::buffered_k)", *disk,
              volume_request(path.c_str()),
              [&](medium_reference_t medium) { ring_cases("blocking_files_disk_t", *disk, medium); });
    std::string const borrowed_path = fmt::format("{}/{}", directory, "volume_borrowed.bin");
    run_test_case("test_ring_holds_a_medium_in_flight<blocking_files_disk_t>",
                  test_ring_holds_a_medium_in_flight<blocking_files_disk_t>, *disk,
                  volume_request(borrowed_path.c_str()));
    std::string const durable_path = fmt::format("{}/{}", directory, "volume_durable.bin");
    run_test_case("test_ring_keeps_what_it_flushed<blocking_files_disk_t>",
                  test_ring_keeps_what_it_flushed<blocking_files_disk_t>, *disk, volume_request(durable_path.c_str()));
    std::string const deallocated_path = fmt::format("{}/{}", directory, "volume_deallocated.bin");
    run_test_case("test_ring_deallocates_a_file_extent<blocking_files_disk_t>",
                  test_ring_deallocates_a_file_extent<blocking_files_disk_t>, *disk,
                  volume_request(deallocated_path.c_str()));

    auto [strict_device, make_strict_device_status] = blocking_files_device_t::make(file_buffering_t::unbuffered_k);
    expected<blocking_files_disk_t> strict_disk = make_strict_device_status.failed()
                                                      ? make_strict_device_status
                                                      : blocking_files_disk_t::make(std::move(strict_device), config,
                                                                                    budget);
    run_test_case("blocking_files_disk_t::make(file_buffering_t::unbuffered_k)",
                  [&] { expect_true(strict_disk.has_value()); });
    if (!strict_disk) return;
    std::string const strict_path = fmt::format("{}/{}", directory, "volume_unbuffered.bin");
    on_medium("blocking_files_disk_t::register_medium(file_buffering_t::unbuffered_k)", *strict_disk,
              volume_request(strict_path.c_str()), [&](medium_reference_t medium) {
                  run_test_case("test_ring_fails_a_misaligned_request<blocking_files_disk_t>",
                                test_ring_fails_a_misaligned_request<blocking_files_disk_t>, *strict_disk, medium);
              });
    std::string const logical_block_path = fmt::format("{}/{}", directory, "volume_logical_block.bin");
    run_test_case("test_ring_reports_a_logical_block<blocking_files_disk_t>",
                  test_ring_reports_a_logical_block<blocking_files_disk_t>, *strict_disk,
                  volume_request(logical_block_path.c_str()));
}

#pragma endregion Transport Drivers

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;

    start_test_run(argc, argv);
    memory_transport();

    st::expected<scratch_directory_t> directory = scratch_directory_t::make(test_run.settings.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    if (directory->path()) blocking_files_transport(directory->path());
    else run_test_case("blocking_files_disk_t::make", [] { return case_result_t::skipped(needs_directory_k); });

    return finish_test_run();
}
