/**
 *  @file test/nvme_ring.cpp
 *  @author Ash Vardanian
 *  @date September 18, 2026
 *  @brief The ring contract, run against the RAM transport and the blocking file transport alike.
 *
 *  Every case is written once against @c ring_concept and run on both disks, since a transport that
 *  passes only its own suite proves nothing about the contract. The file cases need @c --directory
 *  and want a real filesystem: tmpfs accepts @c O_DIRECT and ignores alignment outright, so a volume
 *  in a scratch directory hides exactly the mistakes the alignment case looks for.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/blocking_files_disk.hpp" // `blocking_files_disk_t`
#include "nvme/ram_disk.hpp"            // `ram_disk_t`

#include "nvme_ring_cases.hpp" // `ring_cases`, `ring_fails_a_misaligned_request`

namespace unum::ustore {

/** Mounts the RAM transport and runs the suite on it, with and without demanded alignment. */
void ram_transport(suite_totals_t& totals) {
    disk_config_t config;
    config.queue_pairs_limit = 4;
    config.depth_limit = queue_pair_depth_k;
    config.registered_memory_limit = 4;
    config.media_limit = 4;

    expected<ram_device_t> device = ram_device_t::make();
    expected<ram_disk_t> disk = device ? ram_disk_t::make(std::move(*device), config)
                                       : expected<ram_disk_t> {device.status()};
    if (report(totals, "ram.mount", expect(disk.has_value())) != 0) return;
    on_medium(totals, "ram.volume", *disk, volume_request("ram"),
              [&](medium_reference_t medium) { ring_cases(totals, "ram", *disk, medium); });
    report(totals, "ram.medium_in_flight", ring_holds_a_medium_in_flight(*disk, volume_request("ram_borrowed")));

    expected<ram_device_t> strict_device = ram_device_t::make(portable_alignment_k);
    expected<ram_disk_t> strict_disk = strict_device ? ram_disk_t::make(std::move(*strict_device), config)
                                                     : expected<ram_disk_t> {strict_device.status()};
    if (report(totals, "ram_strict.mount", expect(strict_disk.has_value())) != 0) return;
    on_medium(totals, "ram_strict.volume", *strict_disk, volume_request("ram_strict"), [&](medium_reference_t medium) {
        report(totals, "ram.misaligned_request", ring_fails_a_misaligned_request(*strict_disk, medium));
    });
}

/** Mounts the blocking file transport on a preallocated volume and runs the suite on it. */
void blocking_files_transport(suite_totals_t& totals, char const* directory) {
    disk_config_t config;
    config.queue_pairs_limit = 4;
    config.depth_limit = queue_pair_depth_k;
    config.registered_memory_limit = 4;
    config.media_limit = 4;

    expected<blocking_files_device_t> device = blocking_files_device_t::make(file_buffering_t::buffered_k);
    expected<blocking_files_disk_t> disk = device ? blocking_files_disk_t::make(std::move(*device), config)
                                                  : expected<blocking_files_disk_t> {device.status()};
    if (report(totals, "files.mount", expect(disk.has_value())) != 0) return;
    test_path_t const path {directory, "volume.bin"};
    on_medium(totals, "files.volume", *disk, volume_request(path.c_str()),
              [&](medium_reference_t medium) { ring_cases(totals, "files", *disk, medium); });
    test_path_t const borrowed_path {directory, "volume_borrowed.bin"};
    report(totals, "files.medium_in_flight",
           ring_holds_a_medium_in_flight(*disk, volume_request(borrowed_path.c_str())));
    test_path_t const durable_path {directory, "volume_durable.bin"};
    report(totals, "files.keeps_what_it_flushed",
           ring_keeps_what_it_flushed(*disk, volume_request(durable_path.c_str())));

    expected<blocking_files_device_t> strict_device = blocking_files_device_t::make(file_buffering_t::unbuffered_k);
    expected<blocking_files_disk_t> strict_disk = strict_device
                                                      ? blocking_files_disk_t::make(std::move(*strict_device), config)
                                                      : expected<blocking_files_disk_t> {strict_device.status()};
    if (report(totals, "files_unbuffered.mount", expect(strict_disk.has_value())) != 0) return;
    test_path_t const strict_path {directory, "volume_unbuffered.bin"};
    on_medium(totals, "files_unbuffered.volume", *strict_disk, volume_request(strict_path.c_str()),
              [&](medium_reference_t medium) {
                  report(totals, "files.misaligned_request", ring_fails_a_misaligned_request(*strict_disk, medium));
              });
}

#pragma endregion Drivers

} // namespace unum::ustore

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;

    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    ram_transport(totals);

    st::expected<test_directory_t> directory = test_directory_t::make(options.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    if (directory->path()) blocking_files_transport(totals, directory->path());
    else report(totals, "files", case_result_t::skipped(needs_directory_k));

    return suite_exit_code(totals);
}
