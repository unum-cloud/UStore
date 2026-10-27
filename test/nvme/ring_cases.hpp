/**
 *  @file test/nvme/ring_cases.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The transport-independent ring cases, written once and run against every disk.
 *
 *  Every case here is a template constrained on @c ring_concept, so the blocking floor, the volume in
 *  memory and the @c io_uring rings all run the same code. A case that passes on one transport and
 *  fails on another is the point: that is a conformance defect in the transport, not a case to fork.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <thread>  // `std::thread`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/utilities/nvme_ring.hpp" // `aligned_allocator`

#include "nvme/fixture.hpp" // `case_label_t`, `same_bytes`

namespace unum::ustore::test {

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Fixtures

/** Bytes of one block every case reads and writes, which is also the portable alignment. */
constexpr std::size_t block_bytes_k = 4096;

/** Blocks the volume of every case holds. */
constexpr std::size_t volume_blocks_k = 64;

/** Bytes of the volume of every case. */
constexpr std::uint64_t volume_bytes_k = block_bytes_k * volume_blocks_k;

/** Requests one queue pair of the suite keeps in flight. */
constexpr std::uint32_t queue_pair_depth_k = 8;

/** Room for the completions one poll moves. */
constexpr std::size_t polling_room_k = 64;

/** Registrations one case asks for before it expects the mount to run out of slots. */
constexpr std::size_t registered_memories_probe_k = 64;

/** What a read that straddles the end of the medium comes back as: a clipped prefix, or a refusal at submission. */
enum class ring_edge_t : std::uint8_t {
    clipped_k,
    refused_k,
};

/** A buffer unbuffered I/O accepts, which every case registers before submitting against it. */
using test_buffer_t = st::basic_vector<byte_t, aligned_allocator<byte_t, portable_alignment_k>>;

/** Allocates @p bytes of aligned memory, answering an empty buffer when it does not fit. */
[[nodiscard]] test_buffer_t make_buffer(std::size_t bytes) noexcept {
    test_buffer_t buffer;
    [[maybe_unused]] st::status_t const resized = buffer.resize(bytes);
    return buffer;
}

/** The whole buffer as the span a registered memory takes. */
[[nodiscard]] std::span<byte_t> whole(test_buffer_t& buffer) noexcept { return std::span<byte_t> {buffer}; }

/** Writes a repeatable pattern of @p seed into @p buffer, so a read can be checked against it. */
void paint(std::span<byte_t> buffer, std::uint64_t seed) noexcept {
    for (std::size_t index = 0; index != buffer.size(); ++index)
        buffer[index] = static_cast<byte_t>((index * 131u + seed * 17u + 7u) & 0xFFu);
}

#pragma endregion Fixtures

#pragma region Cases

/** A write of a few blocks comes back byte for byte through a read of the same extents. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_reads_back_what_it_wrote(disk_type_& disk, medium_reference_t medium) {
    std::size_t failures = 0;
    constexpr std::size_t blocks_k = 4;
    test_buffer_t buffer = make_buffer(2 * blocks_k * block_bytes_k);
    failures += expect_eq(buffer.size(), 2 * blocks_k * block_bytes_k);
    if (buffer.size() != 2 * blocks_k * block_bytes_k) return failures;

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

    std::span<byte_t> const source = whole(buffer).first(blocks_k * block_bytes_k);
    std::span<byte_t> const destination = whole(buffer).subspan(blocks_k * block_bytes_k);
    paint(source, 1);
    paint(destination, 0);

    std::array<request_t, blocks_k> writes {};
    for (std::size_t index = 0; index != blocks_k; ++index)
        writes[index] = write_request(medium, extent_t {index * block_bytes_k, block_bytes_k},
                                      registered_memory.reference(index * block_bytes_k),
                                      queue_pair.token(static_cast<std::uint32_t>(index)));
    expected<std::size_t> const submitted = disk.submit(queue_pair, std::span<request_t const> {writes});
    failures += expect(submitted == blocks_k);

    std::array<completion_t, polling_room_k> completions {};
    expected<std::size_t> const written = poll_completions_at_least<waiter_t>(
        disk, queue_pair, std::span<completion_t> {completions}, blocks_k);
    failures += expect(written == blocks_k);
    for (std::size_t index = 0; index != blocks_k; ++index) {
        failures += expect(completions[index].completed_in_full());
        failures += expect_eq(completions[index].transferred_bytes, std::uint64_t {block_bytes_k});
    }

    std::array<request_t, blocks_k> reads {};
    for (std::size_t index = 0; index != blocks_k; ++index)
        reads[index] = read_request(medium, extent_t {index * block_bytes_k, block_bytes_k},
                                    registered_memory.reference((blocks_k + index) * block_bytes_k),
                                    queue_pair.token(static_cast<std::uint32_t>(blocks_k + index)));
    expected<std::size_t> const asked = disk.submit(queue_pair, std::span<request_t const> {reads});
    failures += expect(asked == blocks_k);
    expected<std::size_t> const read = poll_completions_at_least<waiter_t>(
        disk, queue_pair, std::span<completion_t> {completions}, blocks_k);
    failures += expect(read == blocks_k);
    for (std::size_t index = 0; index != blocks_k; ++index) failures += expect(completions[index].completed_in_full());
    failures += expect(same_bytes(source, destination));

    failures += expect(disk.drain(queue_pair).succeeded());
    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/** One queue pair of eight carries sixty-four requests by polling what it submitted and submitting again. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_reaches_depth_beyond_its_credit(disk_type_& disk, medium_reference_t medium) {
    std::size_t failures = 0;
    constexpr std::size_t rounds_k = 8;
    test_buffer_t buffer = make_buffer(queue_pair_depth_k * block_bytes_k);
    failures += expect_eq(buffer.size(), queue_pair_depth_k * block_bytes_k);
    if (buffer.size() != queue_pair_depth_k * block_bytes_k) return failures;

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

    std::size_t retired = 0;
    std::array<completion_t, polling_room_k> completions {};
    for (std::size_t round = 0; round != rounds_k; ++round) {
        paint(whole(buffer), round);
        std::array<request_t, queue_pair_depth_k> batch {};
        for (std::size_t index = 0; index != queue_pair_depth_k; ++index) {
            std::size_t const block = (round * queue_pair_depth_k + index) % volume_blocks_k;
            batch[index] = write_request(medium, extent_t {block * block_bytes_k, block_bytes_k},
                                         registered_memory.reference(index * block_bytes_k),
                                         queue_pair.token(static_cast<std::uint32_t>(retired + index)));
        }
        expected<std::size_t> const submitted = disk.submit(queue_pair, std::span<request_t const> {batch});
        failures += expect(submitted == queue_pair_depth_k);
        expected<std::size_t> const completed = poll_completions_at_least<waiter_t>(
            disk, queue_pair, std::span<completion_t> {completions}, queue_pair_depth_k);
        failures += expect(completed == queue_pair_depth_k);
        for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
            failures += expect(completions[index].completed_in_full());
        retired += queue_pair_depth_k;
    }
    failures += expect_eq(retired, rounds_k * queue_pair_depth_k);
    failures += expect_eq(disk.outstanding(queue_pair), std::size_t {0});

    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/** A full ring takes a prefix and then nothing at all, and never waits for room. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_refuses_when_full(disk_type_& disk, medium_reference_t medium) {
    std::size_t failures = 0;
    constexpr std::size_t offered_k = queue_pair_depth_k + 3;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

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

    std::array<request_t, offered_k> batch {};
    for (std::size_t index = 0; index != offered_k; ++index)
        batch[index] = read_request(medium, extent_t {0, block_bytes_k}, registered_memory.reference(0),
                                    queue_pair.token(static_cast<std::uint32_t>(index)));
    expected<std::size_t> const first = disk.submit(queue_pair, std::span<request_t const> {batch});
    failures += expect(first == std::size_t {queue_pair_depth_k});
    expected<std::size_t> const second = disk.submit(queue_pair, std::span<request_t const> {batch}.first(1));
    failures += expect(second == std::size_t {0});
    failures += expect_eq(disk.outstanding(queue_pair), std::size_t {queue_pair_depth_k});

    std::array<completion_t, polling_room_k> completions {};
    expected<std::size_t> const completed = poll_completions_at_least<waiter_t>(
        disk, queue_pair, std::span<completion_t> {completions}, queue_pair_depth_k);
    failures += expect(completed == std::size_t {queue_pair_depth_k});
    expected<std::size_t> const third = disk.submit(queue_pair, std::span<request_t const> {batch}.first(1));
    failures += expect(third == std::size_t {1});

    failures += expect(disk.drain(queue_pair).succeeded());
    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/** A read that meets the end of the volume moves a prefix, says so, and leaves the tail alone. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_reports_a_partial_transfer(disk_type_& disk, medium_reference_t medium, ring_edge_t edge) {
    std::size_t failures = 0;
    std::uint64_t const reachable = edge == ring_edge_t::clipped_k ? 1024 : block_bytes_k;
    std::size_t const bytes = edge == ring_edge_t::clipped_k ? block_bytes_k : 2 * block_bytes_k;
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

    constexpr byte_t poison_k = static_cast<byte_t>(0xA5u);
    for (std::size_t index = 0; index != buffer.size(); ++index) buffer[index] = poison_k;

    std::array<request_t, 1> batch {read_request(medium, extent_t {volume_bytes_k - reachable, bytes},
                                                 registered_memory.reference(0), queue_pair.token(0))};
    expected<std::size_t> const submitted = disk.submit(queue_pair, std::span<request_t const> {batch});
    failures += expect(submitted == std::size_t {1});

    std::array<completion_t, polling_room_k> completions {};
    expected<std::size_t> const completed = poll_completions_at_least<waiter_t>(
        disk, queue_pair, std::span<completion_t> {completions}, 1);
    failures += expect(completed == std::size_t {1});
    failures += expect(!completions[0].completed_in_full());
    if (edge == ring_edge_t::clipped_k) {
        failures += expect(completions[0].state == completion_state_t::partial_k);
        failures += expect_eq(completions[0].transferred_bytes, reachable);
    }
    else {
        failures += expect(completions[0].state == completion_state_t::failed_k);
        failures += expect_status(completions[0].status, status_code_t::invalid_argument_k);
        failures += expect_eq(completions[0].transferred_bytes, std::uint64_t {0});
        failures += expect(buffer[0] == poison_k);
    }

    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/** A read wholly past the end of the volume refuses rather than handing back whatever the buffer held. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_refuses_a_torn_read(disk_type_& disk, medium_reference_t medium) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

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

    constexpr byte_t poison_k = static_cast<byte_t>(0x5Au);
    for (std::size_t index = 0; index != buffer.size(); ++index) buffer[index] = poison_k;

    std::array<request_t, 1> batch {read_request(medium, extent_t {volume_bytes_k + block_bytes_k, block_bytes_k},
                                                 registered_memory.reference(0), queue_pair.token(0))};
    expected<std::size_t> const submitted = disk.submit(queue_pair, std::span<request_t const> {batch});
    failures += expect(submitted == std::size_t {1});

    std::array<completion_t, polling_room_k> completions {};
    expected<std::size_t> const completed = poll_completions_at_least<waiter_t>(
        disk, queue_pair, std::span<completion_t> {completions}, 1);
    failures += expect(completed == std::size_t {1});
    failures += expect(!completions[0].completed_in_full());
    failures += expect_eq(completions[0].transferred_bytes, std::uint64_t {0});

    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/** A cancelled queue pair's outstanding requests come back cancelled, and its queue pair drains and releases. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_drains_a_cancelled_queue_pair(disk_type_& disk, medium_reference_t medium) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(queue_pair_depth_k * block_bytes_k);
    failures += expect_eq(buffer.size(), queue_pair_depth_k * block_bytes_k);
    if (buffer.size() != queue_pair_depth_k * block_bytes_k) return failures;

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

    std::array<request_t, queue_pair_depth_k> batch {};
    for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
        batch[index] = read_request(medium, extent_t {index * block_bytes_k, block_bytes_k},
                                    registered_memory.reference(index * block_bytes_k),
                                    queue_pair.token(static_cast<std::uint32_t>(index)));
    expected<std::size_t> const submitted = disk.submit(queue_pair, std::span<request_t const> {batch});
    failures += expect(submitted == std::size_t {queue_pair_depth_k});

    // A cancel stops what has not started, so a transport that runs its requests eagerly may have
    // finished them already; what every transport owes is one terminal completion per request.
    disk.cancel(queue_pair);
    std::array<completion_t, polling_room_k> completions {};
    std::array<std::uint32_t, queue_pair_depth_k> seen {};
    std::size_t retired = 0;
    for (std::size_t spins = 0; retired != queue_pair_depth_k && spins != poll_spin_limit_k; ++spins) {
        expected<std::size_t> const completed = disk.poll_completions(queue_pair,
                                                                      std::span<completion_t> {completions});
        failures += expect(completed.has_value());
        if (!completed) break;
        for (std::size_t index = 0; index != *completed; ++index) {
            completion_t const& completion = completions[index];
            if (!completion.retires()) continue;
            std::uint32_t const tag = completion.token.tag();
            failures += expect(tag < queue_pair_depth_k);
            if (tag < queue_pair_depth_k) ++seen[tag];
            if (completion.state == completion_state_t::cancelled_k)
                failures += expect_eq(completion.transferred_bytes, std::uint64_t {0});
            ++retired;
        }
    }
    failures += expect_eq(retired, std::size_t {queue_pair_depth_k});
    for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
        failures += expect_eq(seen[index], std::uint32_t {1});

    failures += expect(disk.drain(queue_pair).succeeded());
    failures += expect_eq(disk.outstanding(queue_pair), std::size_t {0});
    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/** Two queue pairs interleave their batches on one ring and each polls only the tokens it minted. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
case_result_t ring_separates_two_queue_pairs(disk_type_& disk, medium_reference_t medium) {
    std::size_t failures = 0;
    if (disk.capabilities().queue_pairs_limit < 2) return case_result_t::skipped("the ring queue pairs one queue_pair");

    test_buffer_t buffer = make_buffer(2 * queue_pair_depth_k * block_bytes_k);
    failures += expect_eq(buffer.size(), 2 * queue_pair_depth_k * block_bytes_k);
    if (buffer.size() != 2 * queue_pair_depth_k * block_bytes_k) return failures;

    expected<queue_pair_t> first_leased = disk.acquire_queue_pair(queue_pair_depth_k);
    expected<queue_pair_t> second_leased = disk.acquire_queue_pair(queue_pair_depth_k);
    failures += expect(first_leased.has_value());
    failures += expect(second_leased.has_value());
    if (!first_leased || !second_leased) {
        if (first_leased) failures += expect(disk.release_queue_pair(*first_leased).succeeded());
        if (second_leased) failures += expect(disk.release_queue_pair(*second_leased).succeeded());
        return failures;
    }
    queue_pair_t first = std::move(*first_leased);
    queue_pair_t second = std::move(*second_leased);

    expected<registered_memory_t> pinned = disk.register_memory(whole(buffer), memory_space_t::host_k);
    failures += expect(pinned.has_value());
    if (!pinned) {
        failures += expect(disk.release_queue_pair(first).succeeded());
        failures += expect(disk.release_queue_pair(second).succeeded());
        return failures;
    }
    registered_memory_t registered_memory = std::move(*pinned);

    std::array<request_t, queue_pair_depth_k> first_batch {};
    std::array<request_t, queue_pair_depth_k> second_batch {};
    for (std::size_t index = 0; index != queue_pair_depth_k; ++index) {
        first_batch[index] = read_request(medium, extent_t {index * block_bytes_k, block_bytes_k},
                                          registered_memory.reference(index * block_bytes_k),
                                          first.token(static_cast<std::uint32_t>(index)));
        second_batch[index] = read_request(medium, extent_t {index * block_bytes_k, block_bytes_k},
                                           registered_memory.reference((queue_pair_depth_k + index) * block_bytes_k),
                                           second.token(static_cast<std::uint32_t>(100 + index)));
    }
    failures += expect(disk.submit(first, std::span<request_t const> {first_batch}.first(4)) == std::size_t {4});
    failures += expect(disk.submit(second, std::span<request_t const> {second_batch}.first(4)) == std::size_t {4});
    failures += expect(disk.submit(first, std::span<request_t const> {first_batch}.subspan(4)) ==
                       std::size_t {queue_pair_depth_k - 4});

    std::array<completion_t, polling_room_k> completions {};
    expected<std::size_t> const first_reaped = poll_completions_at_least<waiter_t>(
        disk, first, std::span<completion_t> {completions}, queue_pair_depth_k);
    failures += expect(first_reaped == std::size_t {queue_pair_depth_k});
    for (std::size_t index = 0; index != queue_pair_depth_k; ++index) {
        failures += expect(completions[index].token.queue_pair() == first.id());
        failures += expect(completions[index].token.tag() < 100u);
    }

    expected<std::size_t> const second_reaped = poll_completions_at_least<waiter_t>(
        disk, second, std::span<completion_t> {completions}, 4);
    failures += expect(second_reaped == std::size_t {4});
    for (std::size_t index = 0; index != 4; ++index) {
        failures += expect(completions[index].token.queue_pair() == second.id());
        failures += expect(completions[index].token.tag() >= 100u);
    }

    failures += expect(disk.drain(first).succeeded());
    failures += expect(disk.drain(second).succeeded());
    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(first).succeeded());
    failures += expect(disk.release_queue_pair(second).succeeded());
    return failures;
}

/** A batch carrying another queue pair's token is refused whole, since a misrouted completion is unrecoverable. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_refuses_a_foreign_token(disk_type_& disk, medium_reference_t medium) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

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

    request_token_t const foreign = request_token_t::make(static_cast<queue_pair_id_t>(7), queue_pair.generation(), 0);
    std::array<request_t, 1> batch {
        read_request(medium, extent_t {0, block_bytes_k}, registered_memory.reference(0), foreign)};
    expected<std::size_t> const submitted = disk.submit(queue_pair, std::span<request_t const> {batch});
    failures += expect(submitted.failed());
    failures += expect_status(submitted, status_code_t::invalid_argument_k);
    failures += expect_eq(disk.outstanding(queue_pair), std::size_t {0});

    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/** A registered range cannot be handed back while the device may still be writing into it. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_holds_a_registered_memory_in_flight(disk_type_& disk, medium_reference_t medium) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

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

    std::array<request_t, 1> batch {
        read_request(medium, extent_t {0, block_bytes_k}, registered_memory.reference(0), queue_pair.token(0))};
    failures += expect(disk.submit(queue_pair, std::span<request_t const> {batch}) == std::size_t {1});
    failures += expect(disk.unregister_memory(registered_memory).failed());
    failures += expect(registered_memory.held());

    std::array<completion_t, polling_room_k> completions {};
    failures += expect(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {completions},
                                                           1) == std::size_t {1});
    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(!registered_memory.held());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/** A chain runs in submission order and is cut at the first result that is not whole. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_cuts_a_chain(disk_type_& disk, medium_reference_t medium) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(3 * block_bytes_k);
    failures += expect_eq(buffer.size(), 3 * block_bytes_k);
    if (buffer.size() != 3 * block_bytes_k) return failures;

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

    std::array<request_t, 3> chain {};
    chain[0] = read_request(medium, extent_t {0, block_bytes_k}, registered_memory.reference(0), queue_pair.token(0));
    chain[0].chaining = request_chaining_t::chained_k;
    chain[1] = read_request(medium, extent_t {volume_bytes_k, block_bytes_k},
                            registered_memory.reference(block_bytes_k), queue_pair.token(1));
    chain[1].chaining = request_chaining_t::chained_k;
    chain[2] = read_request(medium, extent_t {block_bytes_k, block_bytes_k},
                            registered_memory.reference(2 * block_bytes_k), queue_pair.token(2));

    failures += expect(disk.submit(queue_pair, std::span<request_t const> {chain}) == std::size_t {3});
    std::array<completion_t, polling_room_k> completions {};
    failures += expect(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {completions},
                                                           3) == std::size_t {3});
    failures += expect(completions[0].completed_in_full());
    failures += expect(!completions[1].completed_in_full());
    failures += expect(completions[2].state == completion_state_t::cancelled_k);

    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/** Durability is written as submit, poll, flush, poll, since the ring orders nothing on its own. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_flushes_what_it_reaped(disk_type_& disk, medium_reference_t medium) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

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

    paint(whole(buffer), 42);
    std::array<request_t, 1> writes {
        write_request(medium, extent_t {0, block_bytes_k}, registered_memory.reference(0), queue_pair.token(0))};
    failures += expect(disk.submit(queue_pair, std::span<request_t const> {writes}) == std::size_t {1});
    std::array<completion_t, polling_room_k> completions {};
    failures += expect(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {completions},
                                                           1) == std::size_t {1});
    failures += expect(completions[0].completed_in_full());

    std::array<request_t, 1> flushes {flush_request(medium, queue_pair.token(1))};
    failures += expect(disk.submit(queue_pair, std::span<request_t const> {flushes}) == std::size_t {1});
    failures += expect(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {completions},
                                                           1) == std::size_t {1});
    failures += expect(completions[0].completed_in_full());
    failures += expect_eq(completions[0].token.tag(), std::uint32_t {1});

    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/** A request that misses the disk's alignment fails on its own completion, and not on the submission,
 *  which is what the kernel does with a misaligned @c O_DIRECT read: it is accepted into the ring and
 *  comes back with @c -EINVAL on that one entry, leaving the rest of the batch alone. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_fails_a_misaligned_request(disk_type_& disk, medium_reference_t medium) {
    std::size_t failures = 0;
    ring_capabilities_t const limits = disk.capabilities();
    failures += expect_eq(limits.offset_alignment, std::uint64_t {portable_alignment_k});
    test_buffer_t buffer = make_buffer(2 * block_bytes_k);
    failures += expect_eq(buffer.size(), 2 * block_bytes_k);
    if (buffer.size() != 2 * block_bytes_k) return failures;

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

    std::array<request_t, 3> batch {};
    batch[0] = read_request(medium, extent_t {1, block_bytes_k}, registered_memory.reference(0), queue_pair.token(0));
    batch[1] = read_request(medium, extent_t {0, block_bytes_k}, registered_memory.reference(1), queue_pair.token(1));
    batch[2] = read_request(medium, extent_t {0, block_bytes_k}, registered_memory.reference(block_bytes_k),
                            queue_pair.token(2));
    failures += expect(disk.submit(queue_pair, std::span<request_t const> {batch}) == std::size_t {3});

    std::array<completion_t, polling_room_k> completions {};
    failures += expect(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {completions},
                                                           3) == std::size_t {3});
    failures += expect(completions[0].state == completion_state_t::failed_k);
    failures += expect_status(completions[0].status, status_code_t::invalid_argument_k);
    failures += expect(completions[1].state == completion_state_t::failed_k);
    failures += expect(completions[2].completed_in_full());

    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/** A queue pair is refused when it asks for no depth, or for more than the mount reserved per queue pair. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_refuses_an_impossible_queue_pair(disk_type_& disk, [[maybe_unused]] medium_reference_t medium) {
    std::size_t failures = 0;

    expected<queue_pair_t> const none = disk.acquire_queue_pair(0);
    failures += expect(!none.has_value());
    failures += expect_status(none, status_code_t::invalid_argument_k);

    expected<queue_pair_t> const beyond = disk.acquire_queue_pair(disk.capabilities().depth_limit + 1);
    failures += expect(!beyond.has_value());
    failures += expect_status(beyond, status_code_t::invalid_argument_k);
    return failures;
}

/** Every queue pair, registered memory and medium slot runs out, and each says so rather than overbooking. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_exhausts_its_slots(disk_type_& disk, [[maybe_unused]] medium_reference_t medium) {
    std::size_t failures = 0;
    ring_capabilities_t const limits = disk.capabilities();

    st::basic_vector<queue_pair_t> queue_pairs;
    for (std::uint16_t index = 0; index != limits.queue_pairs_limit; ++index) {
        expected<queue_pair_t> leased = disk.acquire_queue_pair(1);
        failures += expect(leased.has_value());
        if (!leased) break;
        failures += expect(queue_pairs.push_back(std::move(*leased)) == st::status_t::success_k);
    }
    expected<queue_pair_t> const beyond = disk.acquire_queue_pair(1);
    failures += expect(!beyond.has_value());
    failures += expect_status(beyond, status_code_t::capacity_exhausted_k);
    for (std::size_t index = 0; index != queue_pairs.size(); ++index)
        failures += expect(disk.release_queue_pair(queue_pairs[index]).succeeded());

    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;
    st::basic_vector<registered_memory_t> ranges;
    bool refused = false;
    for (std::size_t attempt = 0; attempt != registered_memories_probe_k && !refused; ++attempt) {
        expected<registered_memory_t> pinned = disk.register_memory(whole(buffer), memory_space_t::host_k);
        if (!pinned) {
            failures += expect_status(pinned, status_code_t::capacity_exhausted_k);
            refused = true;
            break;
        }
        failures += expect(ranges.push_back(std::move(*pinned)) == st::status_t::success_k);
    }
    failures += expect(refused);
    failures += expect(!ranges.empty());
    for (std::size_t index = 0; index != ranges.size(); ++index)
        failures += expect(disk.unregister_memory(ranges[index]).succeeded());
    return failures;
}

/** A queue pair with completions nobody completed refuses to go back, since its buffers are still borrowed. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_refuses_an_undrained_release(disk_type_& disk, medium_reference_t medium) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

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

    std::array<request_t, 1> batch {
        read_request(medium, extent_t {0, block_bytes_k}, registered_memory.reference(0), queue_pair.token(0))};
    failures += expect(disk.submit(queue_pair, std::span<request_t const> {batch}) == std::size_t {1});
    failures += expect(disk.release_queue_pair(queue_pair).failed());

    failures += expect(disk.drain(queue_pair).succeeded());
    failures += expect_eq(disk.outstanding(queue_pair), std::size_t {0});
    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/** A medium with requests against it refuses to be given back, exactly as a registered range does. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_holds_a_medium_in_flight(disk_type_& disk, medium_options_t const& request) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

    expected<registered_medium_t> registered = disk.register_medium(request);
    failures += expect(registered.has_value());
    if (!registered) return failures;
    registered_medium_t registered_medium = std::move(*registered);

    expected<queue_pair_t> leased = disk.acquire_queue_pair(queue_pair_depth_k);
    failures += expect(leased.has_value());
    if (!leased) {
        failures += expect(disk.unregister_medium(registered_medium).succeeded());
        return failures;
    }
    queue_pair_t queue_pair = std::move(*leased);

    expected<registered_memory_t> pinned = disk.register_memory(whole(buffer), memory_space_t::host_k);
    failures += expect(pinned.has_value());
    if (!pinned) {
        failures += expect(disk.release_queue_pair(queue_pair).succeeded());
        failures += expect(disk.unregister_medium(registered_medium).succeeded());
        return failures;
    }
    registered_memory_t registered_memory = std::move(*pinned);

    std::array<request_t, 1> batch {read_request(registered_medium.reference(), extent_t {0, block_bytes_k},
                                                 registered_memory.reference(0), queue_pair.token(0))};
    failures += expect(disk.submit(queue_pair, std::span<request_t const> {batch}) == std::size_t {1});
    failures += expect(disk.unregister_medium(registered_medium).failed());
    failures += expect(registered_medium.held());

    std::array<completion_t, polling_room_k> completions {};
    failures += expect(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {completions},
                                                           1) == std::size_t {1});
    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    failures += expect(disk.unregister_medium(registered_medium).succeeded());
    return failures;
}

/** A bounded wait that wants more than it has room for is refused rather than truncating silently. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_refuses_an_oversized_wait(disk_type_& disk, [[maybe_unused]] medium_reference_t medium) {
    std::size_t failures = 0;

    expected<queue_pair_t> leased = disk.acquire_queue_pair(queue_pair_depth_k);
    failures += expect(leased.has_value());
    if (!leased) return failures;
    queue_pair_t queue_pair = std::move(*leased);

    std::array<completion_t, 2> room {};
    expected<std::size_t> const wanted = poll_completions_at_least<waiter_t>(disk, queue_pair,
                                                                             std::span<completion_t> {room}, 3, 1);
    failures += expect(!wanted.has_value());
    failures += expect_status(wanted, status_code_t::invalid_argument_k);

    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/** A queue pair moves to another thread, which submits and polls on it there and hands it back. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_moves_a_queue_pair_between_threads(disk_type_& disk, medium_reference_t medium) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

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

    std::size_t moved_failures = 0;
    std::thread bound_thread {[&]() noexcept {
        moved_failures += expect(disk.bind_queue_pair(queue_pair).succeeded());
        std::array<request_t, 1> batch {
            read_request(medium, extent_t {0, block_bytes_k}, registered_memory.reference(0), queue_pair.token(0))};
        moved_failures += expect(disk.submit(queue_pair, std::span<request_t const> {batch}) == std::size_t {1});
        std::array<completion_t, polling_room_k> completions {};
        moved_failures += expect(poll_completions_at_least<waiter_t>(
                                     disk, queue_pair, std::span<completion_t> {completions}, 1) == std::size_t {1});
        moved_failures += expect(completions[0].completed_in_full());
    }};
    bound_thread.join();
    failures += moved_failures;
    failures += expect(disk.bind_queue_pair(queue_pair).succeeded());

    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

/**
 *  @brief Bytes a flush answered for are still there when the volume is registered again.
 *  @note Only a transport with a medium under it can pass this; a volume in memory is gone with its
 *      medium, which is why the RAM driver does not run it.
 */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_keeps_what_it_flushed(disk_type_& disk, medium_options_t const& request) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(2 * block_bytes_k);
    failures += expect_eq(buffer.size(), 2 * block_bytes_k);
    if (buffer.size() != 2 * block_bytes_k) return failures;

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
    std::array<completion_t, polling_room_k> completions {};

    expected<registered_medium_t> first = disk.register_medium(request);
    failures += expect(first.has_value());
    if (first) {
        registered_medium_t registered_medium = std::move(*first);
        paint(whole(buffer).first(block_bytes_k), 99);
        std::array<request_t, 1> writes {write_request(registered_medium.reference(), extent_t {0, block_bytes_k},
                                                       registered_memory.reference(0), queue_pair.token(0))};
        failures += expect(disk.submit(queue_pair, std::span<request_t const> {writes}) == std::size_t {1});
        failures += expect(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {completions},
                                                               1) == std::size_t {1});
        std::array<request_t, 1> flushes {flush_request(registered_medium.reference(), queue_pair.token(1))};
        failures += expect(disk.submit(queue_pair, std::span<request_t const> {flushes}) == std::size_t {1});
        failures += expect(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {completions},
                                                               1) == std::size_t {1});
        failures += expect(disk.unregister_medium(registered_medium).succeeded());
    }

    medium_options_t reopened = request;
    reopened.access = medium_access_t::read_write_k;
    expected<registered_medium_t> second = disk.register_medium(reopened);
    failures += expect(second.has_value());
    if (second) {
        registered_medium_t registered_medium = std::move(*second);
        std::array<request_t, 1> reads {read_request(registered_medium.reference(), extent_t {0, block_bytes_k},
                                                     registered_memory.reference(block_bytes_k), queue_pair.token(2))};
        failures += expect(disk.submit(queue_pair, std::span<request_t const> {reads}) == std::size_t {1});
        failures += expect(poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<completion_t> {completions},
                                                               1) == std::size_t {1});
        failures += expect(completions[0].completed_in_full());
        failures += expect(
            same_bytes(whole(buffer).first(block_bytes_k), whole(buffer).subspan(block_bytes_k, block_bytes_k)));
        failures += expect(disk.unregister_medium(registered_medium).succeeded());
    }

    failures += expect(disk.unregister_memory(registered_memory).succeeded());
    failures += expect(disk.release_queue_pair(queue_pair).succeeded());
    return failures;
}

#pragma endregion Cases
#pragma region Drivers

/** Runs every transport-independent case against one mounted disk, reporting each under @c ring.<transport>. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
void ring_cases(suite_totals_t& totals, char const* transport, disk_type_& disk, medium_reference_t medium,
                ring_edge_t edge = ring_edge_t::clipped_k) {
    auto const label = [&](char const* name) noexcept { return case_label_t {"ring", transport, name}; };
    report(totals, label("reads_back").c_str(), ring_reads_back_what_it_wrote<disk_type_>, disk, medium);
    report(totals, label("depth_beyond_credit").c_str(), ring_reaches_depth_beyond_its_credit<disk_type_>, disk,
           medium);
    report(totals, label("refuses_when_full").c_str(), ring_refuses_when_full<disk_type_>, disk, medium);
    report(totals, label("partial_transfer").c_str(), ring_reports_a_partial_transfer<disk_type_>, disk, medium, edge);
    report(totals, label("refuses_torn_read").c_str(), ring_refuses_a_torn_read<disk_type_>, disk, medium);
    report(totals, label("cancelled_queue_pair").c_str(), ring_drains_a_cancelled_queue_pair<disk_type_>, disk, medium);
    report(totals, label("separates_queue_pairs").c_str(), ring_separates_two_queue_pairs<disk_type_>, disk, medium);
    report(totals, label("foreign_token").c_str(), ring_refuses_a_foreign_token<disk_type_>, disk, medium);
    report(totals, label("registered_memory_in_flight").c_str(), ring_holds_a_registered_memory_in_flight<disk_type_>,
           disk, medium);
    report(totals, label("cuts_a_chain").c_str(), ring_cuts_a_chain<disk_type_>, disk, medium);
    report(totals, label("flushes_what_it_reaped").c_str(), ring_flushes_what_it_reaped<disk_type_>, disk, medium);
    report(totals, label("impossible_queue pair").c_str(), ring_refuses_an_impossible_queue_pair<disk_type_>, disk,
           medium);
    report(totals, label("exhausts_its_slots").c_str(), ring_exhausts_its_slots<disk_type_>, disk, medium);
    report(totals, label("undrained_release").c_str(), ring_refuses_an_undrained_release<disk_type_>, disk, medium);
    report(totals, label("oversized_wait").c_str(), ring_refuses_an_oversized_wait<disk_type_>, disk, medium);
    report(totals, label("moves_a_queue pair").c_str(), ring_moves_a_queue_pair_between_threads<disk_type_>, disk,
           medium);
}

/**
 *  @brief Registers one volume on @p disk, runs @p body against it, and gives the medium back.
 *  @param[in,out] totals Where the registered memory and its release report under @p name.
 *  @param[in] name The case label of the registered memory itself.
 *  @param[in,out] disk The mounted ring the volume is registered on.
 *  @param[in] request The volume to register, which a RAM device allocates and a file device opens.
 *  @param[in] body What to run against the registered medium, reporting its own cases.
 */
template <typename disk_type_, typename body_type_>
    requires ring_concept<disk_type_>
void on_medium(suite_totals_t& totals, char const* name, disk_type_& disk, medium_options_t const& request,
               body_type_&& body) {
    std::size_t failures = 0;
    expected<registered_medium_t> registered = disk.register_medium(request);
    failures += expect(registered.has_value());
    if (registered) {
        registered_medium_t registered_medium = std::move(*registered);
        body(registered_medium.reference());
        failures += expect(disk.unregister_medium(registered_medium).succeeded());
    }
    report(totals, name, failures);
}

/** The volume every case reads and writes, sized so a request can run past its end. */
[[nodiscard]] medium_options_t volume_request(char const* name) noexcept {
    medium_options_t request;
    request.name = name;
    request.length = volume_bytes_k;
    request.access = medium_access_t::create_read_write_k;
    return request;
}

} // namespace unum::ustore::test
