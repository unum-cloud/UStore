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

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/utilities/types.hpp" // `status_t`, `expected`

#include "ustore/utilities/nvme_disks.hpp"            // `blocking_files_disk_t`
#include "ustore/utilities/nvme_linux_aio_disk.hpp"   // `linux_aio_files_disk`
#include "ustore/utilities/nvme_linux_spdk_disk.hpp"  // `linux_spdk_disk`
#include "ustore/utilities/nvme_linux_uring_disk.hpp" // `linux_uring_files_disk`
#include "ustore/utilities/nvme_volume.hpp"           // `any_files_disk`

#include "nvme/ring_cases.hpp" // `ring_cases`, `ring_fails_a_misaligned_request`

namespace unum::ustore::test {

static_assert(ring_concept<linux_spdk_disk<atomic_reference, waiter_t>>,
              "The SPDK transport is a ring like every other");
static_assert(ring_concept<linux_uring_files_disk<atomic_reference, waiter_t>>,
              "The io_uring transport is a ring like every other");
static_assert(ring_concept<linux_aio_files_disk<atomic_reference, waiter_t>>,
              "The Linux AIO transport is a ring like every other");
static_assert(ring_concept<any_files_disk<atomic_reference, waiter_t>>,
              "Any file transport is a ring like every other");

/** Mounts the RAM transport and runs the suite on it, with and without demanded alignment. */
void memory_transport(suite_totals_t& totals) {
    disk_config_t config;
    config.queue_pairs_limit = 4;
    config.depth_limit = queue_pair_depth_k;
    config.registered_memory_limit = 4;
    config.media_limit = 4;

    auto [device, make_device_status] = memory_device_t::make();
    expected<memory_disk_t> disk = make_device_status.failed() ? make_device_status
                                                               : memory_disk_t::make(std::move(device), config);
    if (report(totals, "memory.mount", expect(disk.has_value())) != 0) return;
    on_medium(totals, "memory.volume", *disk, volume_request("memory"),
              [&](medium_reference_t medium) { ring_cases(totals, "memory", *disk, medium); });
    report(totals, "memory.medium_in_flight", ring_holds_a_medium_in_flight<memory_disk_t>, *disk,
           volume_request("memory_borrowed"));

    auto [strict_device, make_strict_device_status] = memory_device_t::make(portable_alignment_k);
    expected<memory_disk_t> strict_disk = make_strict_device_status.failed()
                                              ? make_strict_device_status
                                              : memory_disk_t::make(std::move(strict_device), config);
    if (report(totals, "memory_strict.mount", expect(strict_disk.has_value())) != 0) return;
    on_medium(totals, "memory_strict.volume", *strict_disk, volume_request("memory_strict"),
              [&](medium_reference_t medium) {
                  report(totals, "memory.misaligned_request", ring_fails_a_misaligned_request<memory_disk_t>,
                         *strict_disk, medium);
              });
}

/** Mounts the blocking file transport on a preallocated volume and runs the suite on it. */
void blocking_files_transport(suite_totals_t& totals, char const* directory) {
    disk_config_t config;
    config.queue_pairs_limit = 4;
    config.depth_limit = queue_pair_depth_k;
    config.registered_memory_limit = 4;
    config.media_limit = 4;

    auto [device, make_device_status] = blocking_files_device_t::make(file_buffering_t::buffered_k);
    expected<blocking_files_disk_t> disk = make_device_status.failed()
                                               ? make_device_status
                                               : blocking_files_disk_t::make(std::move(device), config);
    if (report(totals, "blocking.mount", expect(disk.has_value())) != 0) return;
    path_t const path {directory, "volume.bin"};
    on_medium(totals, "blocking.volume", *disk, volume_request(path.c_str()),
              [&](medium_reference_t medium) { ring_cases(totals, "blocking", *disk, medium); });
    path_t const borrowed_path {directory, "volume_borrowed.bin"};
    report(totals, "blocking.medium_in_flight", ring_holds_a_medium_in_flight<blocking_files_disk_t>, *disk,
           volume_request(borrowed_path.c_str()));
    path_t const durable_path {directory, "volume_durable.bin"};
    report(totals, "blocking.keeps_what_it_flushed", ring_keeps_what_it_flushed<blocking_files_disk_t>, *disk,
           volume_request(durable_path.c_str()));

    auto [strict_device, make_strict_device_status] = blocking_files_device_t::make(file_buffering_t::unbuffered_k);
    expected<blocking_files_disk_t> strict_disk = make_strict_device_status.failed()
                                                      ? make_strict_device_status
                                                      : blocking_files_disk_t::make(std::move(strict_device), config);
    if (report(totals, "blocking_unbuffered.mount", expect(strict_disk.has_value())) != 0) return;
    path_t const strict_path {directory, "volume_unbuffered.bin"};
    on_medium(totals, "blocking_unbuffered.volume", *strict_disk, volume_request(strict_path.c_str()),
              [&](medium_reference_t medium) {
                  report(totals, "blocking.misaligned_request", ring_fails_a_misaligned_request<blocking_files_disk_t>,
                         *strict_disk, medium);
              });
}

#pragma endregion Drivers

} // namespace unum::ustore::test

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;

    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    memory_transport(totals);

    st::expected<directory_t> directory = directory_t::make(options.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    if (directory->path()) blocking_files_transport(totals, directory->path());
    else report(totals, "blocking", case_result_t::skipped(needs_directory_k));

    return suite_exit_code(totals);
}
