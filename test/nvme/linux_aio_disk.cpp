/**
 *  @file test/nvme/linux_aio_disk.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The ring contract on Linux AIO, and what only a transport without kernel links can get wrong.
 *
 *  The shapes are the ones @c test/nvme_ring.cpp runs against every other transport. What is added
 *  here is what AIO alone can break: a chain kept in user space, released link by link and cut or
 *  cancelled while its tail is still held; a batch @c io_submit takes only a prefix of; a flush the
 *  kernel refuses and the disk performs synchronously instead; and batches at the depths a drive wants.
 *
 *  The suite needs @c --directory and refuses tmpfs, which accepts @c O_DIRECT and ignores alignment
 *  outright, hiding exactly the mistakes this transport can make.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/utilities/types.hpp" // `status_t`, `expected`

#include "ustore/utilities/nvme_linux_aio_disk.hpp" // `linux_aio_files_disk_t`, `linux_aio_disk_config_t`

#include "nvme/ring_cases.hpp" // `ring_cases`, every transport-independent result

namespace unum::ustore::test {

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Fixtures

/** Requests one queue pair of the deep cases keeps in flight, where one AIO thread saturates a drive. */
constexpr queue_depth_t deep_queue_depth_k = 256;

/** Bytes of the volume the deep cases fill, which holds the largest batch either one writes. */
constexpr std::uint64_t deep_volume_bytes_k = 32ull * 1024ull * 1024ull;

/**
 *  @brief Takes a queue pair of @p depth, registers @p bytes of aligned memory, runs @p body and gives both back.
 *  @param[inout] disk The mounted disk the queue pair and the range come from.
 *  @param[in] bytes Bytes of buffer to register, which the body addresses through the registered memory.
 *  @param[in] depth Requests the queue pair keeps in flight.
 *  @param[in] body Called with the queue pair, the registered memory and the buffer it covers.
 */
template <typename body_type_>
std::size_t on_queue_pair(linux_aio_files_disk_t& disk, std::size_t bytes, queue_depth_t depth, body_type_&& body) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(bytes);
    failures += expect_eq(buffer.size(), bytes);
    if (buffer.size() != bytes) return failures;

    expected<queue_pair_t> leased = disk.acquire_queue_pair(depth);
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
    failures += expect_eq(disk.outstanding(queue_pair), std::size_t {0});
    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/** Submits all of @p requests, resubmitting what the credit left after each poll, and collects every completion. */
std::size_t submit_and_poll_all(linux_aio_files_disk_t& disk, queue_pair_t const& queue_pair,
                                std::span<request_t const> requests, std::span<completion_t> completions) {
    std::size_t failures = 0;
    std::size_t submitted = 0;
    std::size_t collected = 0;
    for (std::size_t spins = 0; collected != requests.size() && spins != poll_spin_limit_k; ++spins) {
        expected<std::size_t> const taken = disk.submit(queue_pair, requests.subspan(submitted));
        failures += expect(taken.has_value());
        if (!taken) return failures;
        submitted += *taken;
        expected<std::size_t> const moved = disk.poll_completions(queue_pair, completions.subspan(collected));
        failures += expect(moved.has_value());
        if (!moved) return failures;
        collected += *moved;
    }
    failures += expect_eq(collected, requests.size());
    return failures;
}

#pragma endregion Fixtures

#pragma region Cases

/** Writes @p count blocks of @p block bytes at @p depth, flushes, reads them back into the other half and compares. */
std::size_t aio_moves_batches_at_depth(linux_aio_files_disk_t& disk, medium_reference_t medium, std::size_t block,
                                       std::size_t count, queue_depth_t depth) {
    return on_queue_pair(
        disk, 2 * block * count, depth,
        [&](queue_pair_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t> buffer) {
            std::size_t failures = 0;
            std::span<byte_t> const source = buffer.first(block * count);
            std::span<byte_t> const destination = buffer.subspan(block * count);
            for (std::size_t index = 0; index != count; ++index) paint(source.subspan(index * block, block), index);
            paint(destination, count);

            st::basic_vector<request_t> requests;
            st::basic_vector<completion_t> completions;
            failures += expect(requests.resize(count) == st::status_t::success_k);
            failures += expect(completions.resize(count) == st::status_t::success_k);
            if (failures != 0) return failures;

            for (std::size_t index = 0; index != count; ++index)
                requests[index] = write_request(medium, extent_t {index * block, block},
                                                registered_memory.reference(index * block),
                                                queue_pair.token(static_cast<std::uint32_t>(index)));
            failures += submit_and_poll_all(disk, queue_pair, std::span<request_t const> {requests},
                                            std::span<completion_t> {completions});
            for (std::size_t index = 0; index != count; ++index)
                failures += expect_eq(completions[index].transferred_bytes, std::uint64_t {block});

            std::array<request_t, 1> const flushes {flush_request(medium, queue_pair.token(0))};
            std::array<completion_t, 1> flushed {};
            failures += submit_and_poll_all(disk, queue_pair, flushes, flushed);
            failures += expect(flushed[0].completed_in_full());

            for (std::size_t index = 0; index != count; ++index)
                requests[index] = read_request(medium, extent_t {index * block, block},
                                               registered_memory.reference((count + index) * block),
                                               queue_pair.token(static_cast<std::uint32_t>(index)));
            failures += submit_and_poll_all(disk, queue_pair, std::span<request_t const> {requests},
                                            std::span<completion_t> {completions});
            for (std::size_t index = 0; index != count; ++index)
                failures += expect(completions[index].completed_in_full());
            failures += expect(same_bytes(source, destination));
            return failures;
        });
}

/** A misaligned offset, a misaligned address and a ragged length each fail alone, and the valid neighbour runs. */
std::size_t aio_fails_misaligned_requests_alone(linux_aio_files_disk_t& disk, medium_reference_t medium) {
    return on_queue_pair(disk, 2 * block_bytes_k, queue_pair_depth_k,
                         [&](queue_pair_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
                             std::size_t failures = 0;
                             std::array<request_t, 4> batch {};
                             batch[0] = read_request(medium, extent_t {1, block_bytes_k},
                                                     registered_memory.reference(0), queue_pair.token(0));
                             batch[1] = read_request(medium, extent_t {0, block_bytes_k},
                                                     registered_memory.reference(1), queue_pair.token(1));
                             batch[2] = read_request(medium, extent_t {0, 1000}, registered_memory.reference(0),
                                                     queue_pair.token(2));
                             batch[3] = read_request(medium, extent_t {0, block_bytes_k},
                                                     registered_memory.reference(block_bytes_k), queue_pair.token(3));
                             std::array<completion_t, 4> completions {};
                             failures += submit_and_poll_all(disk, queue_pair, batch, completions);
                             for (completion_t const& completion : completions) {
                                 if (completion.token.tag() == 3) {
                                     failures += expect(completion.completed_in_full());
                                     continue;
                                 }
                                 failures += expect(completion.state == completion_state_t::failed_k);
                                 failures += expect_status(completion.status, status_code_t::invalid_argument_k);
                             }
                             return failures;
                         });
}

/**
 *  @brief Writes to a medium opened for reading fail alone, although @c io_submit stops at each of them.
 *
 *  The kernel takes the prefix before a write it refuses and answers @c EBADF for the write itself when
 *  it is first, so the disk resubmits from the write, fails it, and hands the rest over again.
 */
std::size_t aio_splits_a_partially_refused_batch(linux_aio_files_disk_t& disk, medium_reference_t medium,
                                                 char const* path) {
    medium_options_t request;
    request.name = path;
    request.access = medium_access_t::read_only_k;
    expected<registered_medium_t> reopened = disk.register_medium(request);
    std::size_t failures = expect(reopened.has_value());
    if (!reopened) return failures;
    medium_reference_t const read_only = reopened->reference();

    failures += on_queue_pair(
        disk, 4 * block_bytes_k, queue_pair_depth_k,
        [&](queue_pair_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
            std::size_t inner = 0;
            std::array<request_t, 5> batch {};
            batch[0] = read_request(medium, extent_t {0, block_bytes_k}, registered_memory.reference(0),
                                    queue_pair.token(0));
            batch[1] = write_request(read_only, extent_t {0, block_bytes_k}, registered_memory.reference(block_bytes_k),
                                     queue_pair.token(1));
            batch[2] = read_request(read_only, extent_t {block_bytes_k, block_bytes_k},
                                    registered_memory.reference(2 * block_bytes_k), queue_pair.token(2));
            batch[3] = write_request(read_only, extent_t {0, block_bytes_k}, registered_memory.reference(block_bytes_k),
                                     queue_pair.token(3));
            batch[4] = read_request(medium, extent_t {2 * block_bytes_k, block_bytes_k},
                                    registered_memory.reference(3 * block_bytes_k), queue_pair.token(4));
            std::array<completion_t, 5> completions {};
            inner += submit_and_poll_all(disk, queue_pair, batch, completions);
            std::array<std::size_t, 5> seen {};
            for (completion_t const& completion : completions) {
                std::uint32_t const tag = completion.token.tag();
                inner += expect(tag < seen.size());
                if (tag >= seen.size()) continue;
                ++seen[tag];
                if (tag != 1 && tag != 3) {
                    inner += expect(completion.completed_in_full());
                    continue;
                }
                inner += expect(completion.state == completion_state_t::failed_k);
                inner += expect_status(completion.status, status_code_t::invalid_argument_k);
            }
            for (std::size_t const count : seen) inner += expect_eq(count, std::size_t {1});
            return inner;
        });
    failures += expect(disk.unregister_medium(*reopened).succeeded());
    return failures;
}

/** A chain of whole reads completes in submission order, each link released by the poll that saw its predecessor. */
std::size_t aio_runs_a_chain_in_order(linux_aio_files_disk_t& disk, medium_reference_t medium) {
    constexpr std::size_t links_k = 5;
    return on_queue_pair(
        disk, links_k * block_bytes_k, queue_pair_depth_k,
        [&](queue_pair_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
            std::size_t failures = 0;
            std::array<request_t, links_k> chain {};
            for (std::size_t index = 0; index != links_k; ++index) {
                chain[index] = read_request(medium, extent_t {index * block_bytes_k, block_bytes_k},
                                            registered_memory.reference(index * block_bytes_k),
                                            queue_pair.token(static_cast<std::uint32_t>(index)));
                if (index + 1 != links_k) chain[index].chaining = request_chaining_t::chained_k;
            }
            failures += expect(disk.submit(queue_pair, std::span<request_t const> {chain}) == links_k);
            failures += expect_eq(disk.outstanding(queue_pair), links_k);
            std::array<completion_t, links_k> completions {};
            failures += expect(
                poll_completions_at_least(disk, queue_pair, std::span<completion_t> {completions}, links_k) == links_k);
            for (std::size_t index = 0; index != links_k; ++index) {
                failures += expect(completions[index].completed_in_full());
                failures += expect_eq(completions[index].token.tag(), static_cast<std::uint32_t>(index));
            }
            return failures;
        });
}

/** A cancel right after a chain is submitted answers every held link as cancelled, and lets the running head finish. */
std::size_t aio_cancels_a_held_chain(linux_aio_files_disk_t& disk, medium_reference_t medium) {
    constexpr std::size_t links_k = 4;
    return on_queue_pair(
        disk, (links_k + 1) * block_bytes_k, queue_pair_depth_k,
        [&](queue_pair_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
            std::size_t failures = 0;
            std::array<request_t, links_k + 1> batch {};
            for (std::size_t index = 0; index != links_k; ++index) {
                batch[index] = read_request(medium, extent_t {index * block_bytes_k, block_bytes_k},
                                            registered_memory.reference(index * block_bytes_k),
                                            queue_pair.token(static_cast<std::uint32_t>(index)));
                if (index + 1 != links_k) batch[index].chaining = request_chaining_t::chained_k;
            }
            batch[links_k] = read_request(medium, extent_t {0, block_bytes_k},
                                          registered_memory.reference(links_k * block_bytes_k),
                                          queue_pair.token(links_k));
            failures += expect(disk.submit(queue_pair, std::span<request_t const> {batch}) == links_k + 1);
            disk.cancel(queue_pair);

            std::array<completion_t, links_k + 1> completions {};
            failures += expect(poll_completions_at_least(disk, queue_pair, std::span<completion_t> {completions},
                                                         links_k + 1) == links_k + 1);
            std::array<std::size_t, links_k + 1> seen {};
            for (completion_t const& completion : completions) {
                std::uint32_t const tag = completion.token.tag();
                failures += expect(tag < seen.size());
                if (tag >= seen.size()) continue;
                ++seen[tag];
                if (tag == 0 || tag == links_k) failures += expect(completion.completed_in_full());
                else failures += expect(completion.state == completion_state_t::cancelled_k);
            }
            for (std::size_t const count : seen) failures += expect_eq(count, std::size_t {1});
            return failures;
        });
}

/** A full context drained one completion at a time still answers for every request, and takes nothing past it. */
std::size_t aio_polls_through_a_narrow_window(linux_aio_files_disk_t& disk, medium_reference_t medium) {
    return on_queue_pair(
        disk, deep_queue_depth_k * block_bytes_k, deep_queue_depth_k,
        [&](queue_pair_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
            std::size_t failures = 0;
            st::basic_vector<request_t> batch;
            failures += expect(batch.resize(deep_queue_depth_k) == st::status_t::success_k);
            if (failures != 0) return failures;
            for (std::size_t round = 0; round != 3; ++round) {
                for (std::size_t index = 0; index != deep_queue_depth_k; ++index)
                    batch[index] = read_request(
                        medium, extent_t {(index % volume_blocks_k) * block_bytes_k, block_bytes_k},
                        registered_memory.reference(index * block_bytes_k),
                        queue_pair.token(static_cast<std::uint32_t>(round * deep_queue_depth_k + index)));
                std::span<request_t const> const requests {batch};
                failures += expect(disk.submit(queue_pair, requests) == std::size_t {deep_queue_depth_k});
                failures += expect(disk.submit(queue_pair, requests.first(1)) == std::size_t {0});
                std::array<completion_t, 1> narrow {};
                for (std::size_t index = 0; index != deep_queue_depth_k; ++index) {
                    failures += expect(poll_completions_at_least(disk, queue_pair, std::span<completion_t> {narrow},
                                                                 1) == std::size_t {1});
                    failures += expect(narrow[0].completed_in_full());
                }
                failures += expect_eq(disk.outstanding(queue_pair), std::size_t {0});
            }
            return failures;
        });
}

/**
 *  @brief A flush the kernel refuses runs as @c fdatasync instead, and reports what that answered.
 *
 *  @c /dev/null takes a buffered write and has no @c fsync, so @c io_submit refuses the flush with
 *  @c EINVAL, exactly as a kernel older than 4.18 refuses every one, and the synchronous fallback then
 *  fails the same way; the flush must come back failed rather than vanish or pass.
 */
std::size_t aio_flushes_synchronously_where_the_kernel_refuses() {
    std::size_t failures = 0;
    linux_aio_disk_config_t config;
    config.depth_limit = queue_pair_depth_k;
    config.buffering = file_buffering_t::buffered_k;
    expected<linux_aio_files_disk_t> disk = linux_aio_files_disk_t::make(config);
    failures += expect(disk.has_value());
    if (!disk) return failures;

    medium_options_t request;
    request.name = "/dev/null";
    request.access = medium_access_t::read_write_k;
    expected<registered_medium_t> registered = disk->register_medium(request);
    failures += expect(registered.has_value());
    if (!registered) return failures;
    medium_reference_t const medium = registered->reference();

    failures += on_queue_pair(
        *disk, block_bytes_k, queue_pair_depth_k,
        [&](queue_pair_t& queue_pair, registered_memory_t& registered_memory, std::span<byte_t>) {
            std::size_t inner = 0;
            std::array<request_t, 2> const batch {
                write_request(medium, extent_t {0, 100}, registered_memory.reference(0), queue_pair.token(0)),
                flush_request(medium, queue_pair.token(1))};
            std::array<completion_t, 2> completions {};
            inner += submit_and_poll_all(*disk, queue_pair, batch, completions);
            for (completion_t const& completion : completions) {
                if (completion.token.tag() == 0) {
                    inner += expect(completion.completed_in_full());
                    continue;
                }
                inner += expect(completion.state == completion_state_t::failed_k);
                inner += expect_status(completion.status, status_code_t::invalid_argument_k);
            }
            return inner;
        });
    failures += expect(disk->unregister_medium(*registered).succeeded());
    return failures;
}

#pragma endregion Cases

#pragma region Driver

/** Mounts the Linux AIO transport on preallocated volumes and runs the suite on it. */
void aio_transport(suite_totals_t& totals, char const* directory) {
    linux_aio_disk_config_t config;
    config.queue_pairs_limit = 4;
    config.depth_limit = deep_queue_depth_k;
    config.registered_memory_limit = 4;
    config.media_limit = 4;
    config.buffering = file_buffering_t::unbuffered_k;

    expected<linux_aio_files_disk_t> disk = linux_aio_files_disk_t::make(config);
    if (report(totals, "linux-aio.mount", expect_status(disk, status_code_t::success_k)) != 0) return;

    path_t const path {directory, "volume.bin"};
    on_medium(totals, "linux-aio.volume", *disk, volume_request(path.c_str()), [&](medium_reference_t medium) {
        ring_cases(totals, "linux-aio", *disk, medium);
        report(totals, "linux-aio.misaligned_requests", aio_fails_misaligned_requests_alone, *disk, medium);
        report(totals, "linux-aio.partially_refused_batch", aio_splits_a_partially_refused_batch, *disk, medium,
               path.c_str());
        report(totals, "linux-aio.chain_in_order", aio_runs_a_chain_in_order, *disk, medium);
        report(totals, "linux-aio.cancel_held_chain", aio_cancels_a_held_chain, *disk, medium);
        report(totals, "linux-aio.narrow_poll_window", aio_polls_through_a_narrow_window, *disk, medium);
    });

    path_t const deep_path {directory, "volume_deep.bin"};
    medium_options_t deep = volume_request(deep_path.c_str());
    deep.length = deep_volume_bytes_k;
    on_medium(totals, "linux-aio.deep_volume", *disk, deep, [&](medium_reference_t medium) {
        report(totals, "linux-aio.small_blocks_at_depth", aio_moves_batches_at_depth, *disk, medium, std::size_t {4096},
               std::size_t {1024}, deep_queue_depth_k);
        report(totals, "linux-aio.large_blocks_at_depth", aio_moves_batches_at_depth, *disk, medium,
               std::size_t {1 << 20}, std::size_t {16}, queue_depth_t {16});
    });

    path_t const borrowed_path {directory, "volume_borrowed.bin"};
    report(totals, "linux-aio.medium_in_flight", ring_holds_a_medium_in_flight<linux_aio_files_disk_t>, *disk,
           volume_request(borrowed_path.c_str()));
    path_t const durable_path {directory, "volume_durable.bin"};
    report(totals, "linux-aio.keeps_what_it_flushed", ring_keeps_what_it_flushed<linux_aio_files_disk_t>, *disk,
           volume_request(durable_path.c_str()));
    report(totals, "linux-aio.synchronous_flush", aio_flushes_synchronously_where_the_kernel_refuses);
}

#pragma endregion Driver

} // namespace unum::ustore::test

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;

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

    if (char const* const reason = transport_refusal(transport_t::linux_aio_k, environment))
        report(totals, "linux-aio", case_result_t::skipped(reason));
    else aio_transport(totals, environment.directory);
    return suite_exit_code(totals);
}
