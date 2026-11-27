/**
 *  @file test/nvme/ring_cases.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The transport-independent ring cases, written once and run against every disk.
 *
 *  Every case here is a template constrained on @c a_disk, so the blocking floor, the volume
 *  in memory and the @c io_uring rings all run the same code. A case that passes on one
 *  transport and fails on another is the point: that is a conformance defect in the transport,
 *  not a case to fork.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memset`

#include <sys/stat.h> // `stat`, `st_blocks`

#include <algorithm> // `std::find_if`
#include <array>     // `std::array`
#include <bit>       // `std::has_single_bit`
#include <span>      // `std::span`
#include <thread>    // `std::thread`
#include <utility>   // `std::move`

#include <forkunion/forkunion.hpp>     // `fu::topology_t`, `fu::pool_t`
#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/nvme_ring.hpp"   // `a_disk`, `queue_ref_t`, `countdown`, `notifier`, `completion_t`
#include "ustore/utilities/ram_buffers.hpp" // `aligned_allocator`

#include "nvme/fixture.hpp" // `case_label_t`, `same_bytes`

namespace unum::ustore::test::nvme {

using namespace unum::ustore::blobs;

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
using test_buffer_t = st::vector<util::byte_t, util::aligned_allocator<util::byte_t, util::portable_alignment_k>>;

/** Allocates @p bytes of aligned memory, answering an empty buffer when it does not fit. */
[[nodiscard]] test_buffer_t make_buffer(std::size_t bytes) noexcept {
    test_buffer_t buffer;
    [[maybe_unused]] st::status_t const resized = buffer.resize(bytes);
    return buffer;
}

/** The whole buffer as the span a registered memory takes. */
[[nodiscard]] std::span<util::byte_t> whole(test_buffer_t& buffer) noexcept { return std::span<util::byte_t> {buffer}; }

/** Writes a repeatable pattern of @p seed into @p buffer, so a read can be checked against it. */
void paint(std::span<util::byte_t> buffer, std::uint64_t seed) noexcept {
    for (std::size_t index = 0; index != buffer.size(); ++index)
        buffer[index] = static_cast<util::byte_t>((index * 131u + seed * 17u + 7u) & 0xFFu);
}

#pragma endregion Fixtures

#pragma region Cases

/** A write of a few blocks comes back byte for byte through a read of the same extents. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_reads_back_what_it_wrote(disk_type_& disk, util::medium_reference_t medium) {
    constexpr std::size_t blocks_k = 4;
    test_buffer_t buffer = make_buffer(2 * blocks_k * block_bytes_k);
    expect_equal(buffer.size(), 2 * blocks_k * block_bytes_k);
    if (buffer.size() != 2 * blocks_k * block_bytes_k) return;

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) return;

    std::span<util::byte_t> const source = whole(buffer).first(blocks_k * block_bytes_k);
    std::span<util::byte_t> const destination = whole(buffer).subspan(blocks_k * block_bytes_k);
    paint(source, 1);
    paint(destination, 0);

    std::array<util::request_t, blocks_k> writes {};
    for (std::size_t index = 0; index != blocks_k; ++index)
        writes[index] = util::write_request(medium, util::extent_t {index * block_bytes_k, block_bytes_k},
                                            registered_memory.reference(index * block_bytes_k),
                                            queue_pair.token(static_cast<std::uint32_t>(index)));
    auto const [submitted, submit_status] = disk.submit(queue_pair, std::span<util::request_t const> {writes});
    expect_true(submit_status.succeeded() && submitted == blocks_k);

    std::array<util::completion_t, polling_room_k> completions {};
    auto const [written, written_status] = util::poll_completions_at_least<waiter_t>(
        disk, queue_pair, std::span<util::completion_t> {completions}, blocks_k);
    expect_true(written_status.succeeded() && written == blocks_k);
    for (std::size_t index = 0; index != blocks_k; ++index) {
        expect_true(completions[index].completed_in_full());
        expect_equal(completions[index].transferred_bytes, std::uint64_t {block_bytes_k});
    }

    std::array<util::request_t, blocks_k> reads {};
    for (std::size_t index = 0; index != blocks_k; ++index)
        reads[index] = util::read_request(medium, util::extent_t {index * block_bytes_k, block_bytes_k},
                                          registered_memory.reference((blocks_k + index) * block_bytes_k),
                                          queue_pair.token(static_cast<std::uint32_t>(blocks_k + index)));
    auto const [asked, asked_status] = disk.submit(queue_pair, std::span<util::request_t const> {reads});
    expect_true(asked_status.succeeded() && asked == blocks_k);
    auto const [read, read_status] = util::poll_completions_at_least<waiter_t>(
        disk, queue_pair, std::span<util::completion_t> {completions}, blocks_k);
    expect_true(read_status.succeeded() && read == blocks_k);
    for (std::size_t index = 0; index != blocks_k; ++index) expect_true(completions[index].completed_in_full());
    expect_true(same_bytes(source, destination));

    expect_true(disk.drain(queue_pair).succeeded());
    expect_true(disk.unregister_memory(registered_memory).succeeded());
}

/** One queue pair of eight holds sixty-four requests by polling what it submitted and submitting
 *  again. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_reaches_depth_beyond_its_credit(disk_type_& disk, util::medium_reference_t medium) {
    constexpr std::size_t rounds_k = 8;
    test_buffer_t buffer = make_buffer(queue_pair_depth_k * block_bytes_k);
    expect_equal(buffer.size(), queue_pair_depth_k * block_bytes_k);
    if (buffer.size() != queue_pair_depth_k * block_bytes_k) return;

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) return;

    std::size_t retired = 0;
    std::array<util::completion_t, polling_room_k> completions {};
    for (std::size_t round = 0; round != rounds_k; ++round) {
        paint(whole(buffer), round);
        std::array<util::request_t, queue_pair_depth_k> batch {};
        for (std::size_t index = 0; index != queue_pair_depth_k; ++index) {
            std::size_t const block = (round * queue_pair_depth_k + index) % volume_blocks_k;
            batch[index] = util::write_request(medium, util::extent_t {block * block_bytes_k, block_bytes_k},
                                               registered_memory.reference(index * block_bytes_k),
                                               queue_pair.token(static_cast<std::uint32_t>(retired + index)));
        }
        auto const [submitted, submit_status] = disk.submit(queue_pair, std::span<util::request_t const> {batch});
        expect_true(submit_status.succeeded() && submitted == queue_pair_depth_k);
        auto const [completed, poll_status] = util::poll_completions_at_least<waiter_t>(
            disk, queue_pair, std::span<util::completion_t> {completions}, queue_pair_depth_k);
        expect_true(poll_status.succeeded() && completed == queue_pair_depth_k);
        for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
            expect_true(completions[index].completed_in_full());
        retired += queue_pair_depth_k;
    }
    expect_equal(retired, rounds_k * queue_pair_depth_k);
    expect_equal(disk.outstanding(queue_pair), std::size_t {0});

    expect_true(disk.unregister_memory(registered_memory).succeeded());
}

/** A full ring takes a prefix and then nothing at all, and never waits for room. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_refuses_when_full(disk_type_& disk, util::medium_reference_t medium) {
    constexpr std::size_t offered_k = queue_pair_depth_k + 3;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    expect_equal(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return;

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) return;

    std::array<util::request_t, offered_k> batch {};
    for (std::size_t index = 0; index != offered_k; ++index)
        batch[index] = util::read_request(medium, util::extent_t {0, block_bytes_k}, registered_memory.reference(0),
                                          queue_pair.token(static_cast<std::uint32_t>(index)));
    auto const [first, first_status] = disk.submit(queue_pair, std::span<util::request_t const> {batch});
    expect_true(first_status.succeeded() && first == std::size_t {queue_pair_depth_k});
    auto const [second, second_status] = disk.submit(queue_pair, std::span<util::request_t const> {batch}.first(1));
    expect_true(second_status.succeeded() && second == std::size_t {0});
    expect_equal(disk.outstanding(queue_pair), std::size_t {queue_pair_depth_k});

    std::array<util::completion_t, polling_room_k> completions {};
    auto const [completed, completed_status] = util::poll_completions_at_least<waiter_t>(
        disk, queue_pair, std::span<util::completion_t> {completions}, queue_pair_depth_k);
    expect_true(completed_status.succeeded() && completed == std::size_t {queue_pair_depth_k});
    auto const [third, third_status] = disk.submit(queue_pair, std::span<util::request_t const> {batch}.first(1));
    expect_true(third_status.succeeded() && third == std::size_t {1});

    expect_true(disk.drain(queue_pair).succeeded());
    expect_true(disk.unregister_memory(registered_memory).succeeded());
}

/** A read that meets the end of the volume moves a prefix, says so, and leaves the tail alone. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_reports_a_partial_transfer(disk_type_& disk, util::medium_reference_t medium, ring_edge_t edge) {
    std::uint64_t const reachable = edge == ring_edge_t::clipped_k ? 1024 : block_bytes_k;
    std::size_t const bytes = edge == ring_edge_t::clipped_k ? block_bytes_k : 2 * block_bytes_k;
    test_buffer_t buffer = make_buffer(bytes);
    expect_equal(buffer.size(), bytes);
    if (buffer.size() != bytes) return;

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) return;

    constexpr util::byte_t poison_k = static_cast<util::byte_t>(0xA5u);
    for (std::size_t index = 0; index != buffer.size(); ++index) buffer[index] = poison_k;

    std::array<util::request_t, 1> batch {util::read_request(medium, util::extent_t {volume_bytes_k - reachable, bytes},
                                                             registered_memory.reference(0), queue_pair.token(0))};
    auto const [submitted, submit_status] = disk.submit(queue_pair, std::span<util::request_t const> {batch});
    expect_true(submit_status.succeeded() && submitted == std::size_t {1});

    std::array<util::completion_t, polling_room_k> completions {};
    auto const [completed, poll_status] = util::poll_completions_at_least<waiter_t>(
        disk, queue_pair, std::span<util::completion_t> {completions}, 1);
    expect_true(poll_status.succeeded() && completed == std::size_t {1});
    expect_true(!completions[0].completed_in_full());
    if (edge == ring_edge_t::clipped_k) {
        expect_true(completions[0].state == util::completion_state_t::partial_k);
        expect_equal(completions[0].transferred_bytes, reachable);
    }
    else {
        expect_true(completions[0].state == util::completion_state_t::failed_k);
        expect_status(completions[0].status, util::status_code_t::invalid_argument_k);
        expect_equal(completions[0].transferred_bytes, std::uint64_t {0});
        expect_true(buffer[0] == poison_k);
    }

    expect_true(disk.unregister_memory(registered_memory).succeeded());
}

/** A read wholly past the end of the volume refuses rather than handing back whatever the buffer held. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_refuses_a_torn_read(disk_type_& disk, util::medium_reference_t medium) {
    test_buffer_t buffer = make_buffer(block_bytes_k);
    expect_equal(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return;

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) return;

    constexpr util::byte_t poison_k = static_cast<util::byte_t>(0x5Au);
    for (std::size_t index = 0; index != buffer.size(); ++index) buffer[index] = poison_k;

    std::array<util::request_t, 1> batch {
        util::read_request(medium, util::extent_t {volume_bytes_k + block_bytes_k, block_bytes_k},
                           registered_memory.reference(0), queue_pair.token(0))};
    auto const [submitted, submit_status] = disk.submit(queue_pair, std::span<util::request_t const> {batch});
    expect_true(submit_status.succeeded() && submitted == std::size_t {1});

    std::array<util::completion_t, polling_room_k> completions {};
    auto const [completed, poll_status] = util::poll_completions_at_least<waiter_t>(
        disk, queue_pair, std::span<util::completion_t> {completions}, 1);
    expect_true(poll_status.succeeded() && completed == std::size_t {1});
    expect_true(!completions[0].completed_in_full());
    expect_equal(completions[0].transferred_bytes, std::uint64_t {0});

    expect_true(disk.unregister_memory(registered_memory).succeeded());
}

/** A cancelled queue pair's outstanding requests come back cancelled, and its queue pair drains and releases. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_drains_a_cancelled_queue_pair(disk_type_& disk, util::medium_reference_t medium) {
    test_buffer_t buffer = make_buffer(queue_pair_depth_k * block_bytes_k);
    expect_equal(buffer.size(), queue_pair_depth_k * block_bytes_k);
    if (buffer.size() != queue_pair_depth_k * block_bytes_k) return;

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) return;

    std::array<util::request_t, queue_pair_depth_k> batch {};
    for (std::size_t index = 0; index != queue_pair_depth_k; ++index)
        batch[index] = util::read_request(medium, util::extent_t {index * block_bytes_k, block_bytes_k},
                                          registered_memory.reference(index * block_bytes_k),
                                          queue_pair.token(static_cast<std::uint32_t>(index)));
    auto const [submitted, submit_status] = disk.submit(queue_pair, std::span<util::request_t const> {batch});
    expect_true(submit_status.succeeded() && submitted == std::size_t {queue_pair_depth_k});

    // A cancel stops what has not started, so a transport that runs its requests eagerly may have
    // finished them already; what every transport owes is one terminal completion per request.
    disk.cancel(queue_pair);
    std::array<util::completion_t, polling_room_k> completions {};
    std::array<std::uint32_t, queue_pair_depth_k> seen {};
    std::size_t retired = 0;
    for (std::size_t spins = 0; retired != queue_pair_depth_k && spins != util::poll_spin_limit_k; ++spins) {
        auto const [completed, poll_status] = disk.poll_completions(queue_pair,
                                                                    std::span<util::completion_t> {completions});
        expect_true(poll_status.succeeded());
        if (poll_status.failed()) break;
        for (std::size_t index = 0; index != completed; ++index) {
            util::completion_t const& completion = completions[index];
            if (!completion.retires()) continue;
            std::uint32_t const tag = completion.token.tag();
            expect_true(tag < queue_pair_depth_k);
            if (tag < queue_pair_depth_k) ++seen[tag];
            if (completion.state == util::completion_state_t::cancelled_k)
                expect_equal(completion.transferred_bytes, std::uint64_t {0});
            ++retired;
        }
    }
    expect_equal(retired, std::size_t {queue_pair_depth_k});
    for (std::size_t index = 0; index != queue_pair_depth_k; ++index) expect_equal(seen[index], std::uint32_t {1});

    expect_true(disk.drain(queue_pair).succeeded());
    expect_equal(disk.outstanding(queue_pair), std::size_t {0});
    expect_true(disk.unregister_memory(registered_memory).succeeded());
}

/** Two queue pairs interleave their batches on one ring and each polls only the tokens it minted. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
case_result_t test_ring_separates_two_queue_pairs(disk_type_& disk, util::medium_reference_t medium) {
    test_buffer_t buffer = make_buffer(2 * queue_pair_depth_k * block_bytes_k);
    expect_equal(buffer.size(), 2 * queue_pair_depth_k * block_bytes_k);
    if (buffer.size() != 2 * queue_pair_depth_k * block_bytes_k) return {};

    auto [first_queue, make_first_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    auto [second_queue, make_second_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_first_status.succeeded());
    expect_true(make_second_status.succeeded());
    if (make_first_status.failed() || make_second_status.failed()) return {};
    util::queue_ref_t const first {first_queue, util::queue_id_t {0}};
    util::queue_ref_t const second {second_queue, util::queue_id_t {1}};
    expect_true(disk.register_media(first).succeeded());
    expect_true(disk.register_media(second).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) return {};

    std::array<util::request_t, queue_pair_depth_k> first_batch {};
    std::array<util::request_t, queue_pair_depth_k> second_batch {};
    for (std::size_t index = 0; index != queue_pair_depth_k; ++index) {
        first_batch[index] = util::read_request(medium, util::extent_t {index * block_bytes_k, block_bytes_k},
                                                registered_memory.reference(index * block_bytes_k),
                                                first.token(static_cast<std::uint32_t>(index)));
        second_batch[index] = util::read_request(
            medium, util::extent_t {index * block_bytes_k, block_bytes_k},
            registered_memory.reference((queue_pair_depth_k + index) * block_bytes_k),
            second.token(static_cast<std::uint32_t>(100 + index)));
    }
    expect_true(disk.submit(first, std::span<util::request_t const> {first_batch}.first(4)) == std::size_t {4});
    expect_true(disk.submit(second, std::span<util::request_t const> {second_batch}.first(4)) == std::size_t {4});
    expect_true(disk.submit(first, std::span<util::request_t const> {first_batch}.subspan(4)) ==
                std::size_t {queue_pair_depth_k - 4});

    std::array<util::completion_t, polling_room_k> completions {};
    auto const [first_reaped, first_reaped_status] = util::poll_completions_at_least<waiter_t>(
        disk, first, std::span<util::completion_t> {completions}, queue_pair_depth_k);
    expect_true(first_reaped_status.succeeded() && first_reaped == std::size_t {queue_pair_depth_k});
    for (std::size_t index = 0; index != queue_pair_depth_k; ++index) {
        expect_true(completions[index].token.queue_pair() == first.id());
        expect_true(completions[index].token.tag() < 100u);
    }

    auto const [second_reaped, second_reaped_status] = util::poll_completions_at_least<waiter_t>(
        disk, second, std::span<util::completion_t> {completions}, 4);
    expect_true(second_reaped_status.succeeded() && second_reaped == std::size_t {4});
    for (std::size_t index = 0; index != 4; ++index) {
        expect_true(completions[index].token.queue_pair() == second.id());
        expect_true(completions[index].token.tag() >= 100u);
    }

    expect_true(disk.drain(first).succeeded());
    expect_true(disk.drain(second).succeeded());
    expect_true(disk.unregister_memory(registered_memory).succeeded());
    return {};
}

/** A batch holding another queue pair's token is refused whole, since a misrouted completion is
 *  unrecoverable. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_refuses_a_foreign_token(disk_type_& disk, util::medium_reference_t medium) {
    test_buffer_t buffer = make_buffer(block_bytes_k);
    expect_equal(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return;

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) return;

    util::request_token_t const foreign = util::request_token_t::make(static_cast<util::queue_id_t>(7), 0, 0);
    std::array<util::request_t, 1> batch {
        util::read_request(medium, util::extent_t {0, block_bytes_k}, registered_memory.reference(0), foreign)};
    auto const [submitted, submit_status] = disk.submit(queue_pair, std::span<util::request_t const> {batch});
    expect_true(submit_status.failed());
    expect_status(submit_status, util::status_code_t::invalid_argument_k);
    expect_equal(disk.outstanding(queue_pair), std::size_t {0});

    expect_true(disk.unregister_memory(registered_memory).succeeded());
}

/** A registered range cannot be handed back while the device may still be writing into it. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_holds_a_registered_memory_in_flight(disk_type_& disk, util::medium_reference_t medium) {
    test_buffer_t buffer = make_buffer(block_bytes_k);
    expect_equal(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return;

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) return;

    std::array<util::request_t, 1> batch {util::read_request(medium, util::extent_t {0, block_bytes_k},
                                                             registered_memory.reference(0), queue_pair.token(0))};
    expect_true(disk.submit(queue_pair, std::span<util::request_t const> {batch}) == std::size_t {1});
    expect_true(disk.unregister_memory(registered_memory).failed());
    expect_true(registered_memory.held());

    std::array<util::completion_t, polling_room_k> completions {};
    expect_true(util::poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<util::completion_t> {completions},
                                                          1) == std::size_t {1});
    expect_true(disk.unregister_memory(registered_memory).succeeded());
    expect_true(!registered_memory.held());
}

/** A chain runs in submission order and is cut at the first result that is not whole. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_cuts_a_chain(disk_type_& disk, util::medium_reference_t medium) {
    test_buffer_t buffer = make_buffer(3 * block_bytes_k);
    expect_equal(buffer.size(), 3 * block_bytes_k);
    if (buffer.size() != 3 * block_bytes_k) return;

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) return;

    std::array<util::request_t, 3> chain {};
    chain[0] = util::read_request(medium, util::extent_t {0, block_bytes_k}, registered_memory.reference(0),
                                  queue_pair.token(0));
    chain[0].chaining = util::request_chaining_t::chained_k;
    chain[1] = util::read_request(medium, util::extent_t {volume_bytes_k, block_bytes_k},
                                  registered_memory.reference(block_bytes_k), queue_pair.token(1));
    chain[1].chaining = util::request_chaining_t::chained_k;
    chain[2] = util::read_request(medium, util::extent_t {block_bytes_k, block_bytes_k},
                                  registered_memory.reference(2 * block_bytes_k), queue_pair.token(2));

    expect_true(disk.submit(queue_pair, std::span<util::request_t const> {chain}) == std::size_t {3});
    std::array<util::completion_t, polling_room_k> completions {};
    expect_true(util::poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<util::completion_t> {completions},
                                                          3) == std::size_t {3});
    expect_true(completions[0].completed_in_full());
    expect_true(!completions[1].completed_in_full());
    expect_true(completions[2].state == util::completion_state_t::cancelled_k);

    expect_true(disk.unregister_memory(registered_memory).succeeded());
}

/** Durability is written as submit, poll, flush, poll, since the ring orders nothing on its own. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_flushes_what_it_reaped(disk_type_& disk, util::medium_reference_t medium) {
    test_buffer_t buffer = make_buffer(block_bytes_k);
    expect_equal(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return;

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) return;

    paint(whole(buffer), 42);
    std::array<util::request_t, 1> writes {util::write_request(medium, util::extent_t {0, block_bytes_k},
                                                               registered_memory.reference(0), queue_pair.token(0))};
    expect_true(disk.submit(queue_pair, std::span<util::request_t const> {writes}) == std::size_t {1});
    std::array<util::completion_t, polling_room_k> completions {};
    expect_true(util::poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<util::completion_t> {completions},
                                                          1) == std::size_t {1});
    expect_true(completions[0].completed_in_full());

    std::array<util::request_t, 1> flushes {util::flush_request(medium, queue_pair.token(1))};
    expect_true(disk.submit(queue_pair, std::span<util::request_t const> {flushes}) == std::size_t {1});
    expect_true(util::poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<util::completion_t> {completions},
                                                          1) == std::size_t {1});
    expect_true(completions[0].completed_in_full());
    expect_equal(completions[0].token.tag(), std::uint32_t {1});

    expect_true(disk.unregister_memory(registered_memory).succeeded());
}

/** A request that misses the disk's alignment fails on its own completion, and not on the submission,
 *  which is what the kernel does with a misaligned @c O_DIRECT read: it is accepted into the ring and
 *  comes back with @c -EINVAL on that one entry, leaving the rest of the batch alone. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_fails_a_misaligned_request(disk_type_& disk, util::medium_reference_t medium) {
    util::disk_capabilities_t const limits = disk.capabilities();
    expect_equal(limits.offset_alignment, std::uint64_t {util::portable_alignment_k});
    test_buffer_t buffer = make_buffer(2 * block_bytes_k);
    expect_equal(buffer.size(), 2 * block_bytes_k);
    if (buffer.size() != 2 * block_bytes_k) return;

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) return;

    std::array<util::request_t, 3> batch {};
    batch[0] = util::read_request(medium, util::extent_t {1, block_bytes_k}, registered_memory.reference(0),
                                  queue_pair.token(0));
    batch[1] = util::read_request(medium, util::extent_t {0, block_bytes_k}, registered_memory.reference(1),
                                  queue_pair.token(1));
    batch[2] = util::read_request(medium, util::extent_t {0, block_bytes_k}, registered_memory.reference(block_bytes_k),
                                  queue_pair.token(2));
    expect_true(disk.submit(queue_pair, std::span<util::request_t const> {batch}) == std::size_t {3});

    std::array<util::completion_t, polling_room_k> completions {};
    expect_true(util::poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<util::completion_t> {completions},
                                                          3) == std::size_t {3});
    expect_true(completions[0].state == util::completion_state_t::failed_k);
    expect_status(completions[0].status, util::status_code_t::invalid_argument_k);
    expect_true(completions[1].state == util::completion_state_t::failed_k);
    expect_true(completions[2].completed_in_full());

    expect_true(disk.unregister_memory(registered_memory).succeeded());
}

/** A queue is refused when it asks for no depth. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_refuses_an_impossible_queue_pair([[maybe_unused]] disk_type_& disk,
                                                [[maybe_unused]] util::medium_reference_t medium) {
    auto [none, none_status] = disk_type_::queue_t::make(0, {});
    expect_true(none_status.failed());
    expect_status(none_status, util::status_code_t::invalid_argument_k);
}

/** Every registered memory slot runs out, and says so rather than overbooking. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_exhausts_its_slots(disk_type_& disk, [[maybe_unused]] util::medium_reference_t medium) {
    test_buffer_t buffer = make_buffer(block_bytes_k);
    expect_equal(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return;
    st::vector<util::registered_memory_t> ranges;
    bool refused = false;
    for (std::size_t attempt = 0; attempt != registered_memories_probe_k && !refused; ++attempt) {
        auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                                util::memory_space_t::host_k);
        if (register_memory_status.failed()) {
            expect_status(register_memory_status, util::status_code_t::capacity_exhausted_k);
            refused = true;
            break;
        }
        expect_true(ranges.push_back(std::move(registered_memory)) == st::status_t::success_k);
    }
    expect_true(refused);
    expect_true(!ranges.empty());
    for (std::size_t index = 0; index != ranges.size(); ++index)
        expect_true(disk.unregister_memory(ranges[index]).succeeded());
}

/** A medium with requests against it refuses to be given back, exactly as a registered range does,
 *  and a reference kept past its release names nothing, even once another medium takes its slot. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_holds_a_medium_in_flight(disk_type_& disk, util::medium_options_t const& request) {
    test_buffer_t buffer = make_buffer(block_bytes_k);
    expect_equal(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return;

    auto [registered_medium, register_medium_status] = disk.register_medium(request);
    expect_true(register_medium_status.succeeded());
    if (register_medium_status.failed()) return;

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) {
        expect_true(disk.unregister_medium(registered_medium).succeeded());
        return;
    }
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) {
        expect_true(disk.unregister_medium(registered_medium).succeeded());
        return;
    }

    std::array<util::request_t, 1> batch {util::read_request(registered_medium.reference(),
                                                             util::extent_t {0, block_bytes_k},
                                                             registered_memory.reference(0), queue_pair.token(0))};
    expect_true(disk.submit(queue_pair, std::span<util::request_t const> {batch}) == std::size_t {1});
    expect_true(disk.unregister_medium(registered_medium).failed());
    expect_true(registered_medium.held());

    std::array<util::completion_t, polling_room_k> completions {};
    expect_true(util::poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<util::completion_t> {completions},
                                                          1) == std::size_t {1});
    util::medium_reference_t const stale = registered_medium.reference();
    expect_true(disk.unregister_medium(registered_medium).succeeded());

    util::medium_options_t reopened = request;
    reopened.access = util::medium_access_t::read_write_k;
    auto [successor_medium, register_successor_status] = disk.register_medium(reopened);
    expect_true(register_successor_status.succeeded());
    if (register_successor_status.succeeded()) {
        std::array<util::request_t, 1> stale_batch {util::read_request(
            stale, util::extent_t {0, block_bytes_k}, registered_memory.reference(0), queue_pair.token(1))};
        expect_true(disk.submit(queue_pair, std::span<util::request_t const> {stale_batch}) == std::size_t {1});
        expect_true(util::poll_completions_at_least<waiter_t>(
                        disk, queue_pair, std::span<util::completion_t> {completions}, 1) == std::size_t {1});
        expect_true(completions[0].state == util::completion_state_t::failed_k);
        expect_status(completions[0].status, util::status_code_t::invalid_argument_k);
        expect_true(disk.unregister_medium(successor_medium).succeeded());
    }
    expect_true(disk.unregister_memory(registered_memory).succeeded());
}

/** A bounded wait that wants more than it has room for is refused rather than truncating silently. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_refuses_an_oversized_wait(disk_type_& disk, [[maybe_unused]] util::medium_reference_t medium) {

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    std::array<util::completion_t, 2> room {};
    auto const [wanted, poll_status] = util::poll_completions_at_least<waiter_t>(
        disk, queue_pair, std::span<util::completion_t> {room}, 3, 1);
    expect_true(poll_status.failed());
    expect_status(poll_status, util::status_code_t::invalid_argument_k);
}

/** A queue made on one thread serves another, as a queue set made by its creator serves the pool's
 *  threads. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_serves_a_queue_on_another_thread(disk_type_& disk, util::medium_reference_t medium) {
    test_buffer_t buffer = make_buffer(block_bytes_k);
    expect_equal(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return;

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) return;

    std::thread bound_thread {[&]() noexcept {
        std::array<util::request_t, 1> batch {util::read_request(medium, util::extent_t {0, block_bytes_k},
                                                                 registered_memory.reference(0), queue_pair.token(0))};
        expect_true(disk.submit(queue_pair, std::span<util::request_t const> {batch}) == std::size_t {1});
        std::array<util::completion_t, polling_room_k> completions {};
        expect_true(util::poll_completions_at_least<waiter_t>(
                        disk, queue_pair, std::span<util::completion_t> {completions}, 1) == std::size_t {1});
        expect_true(completions[0].completed_in_full());
    }};
    bound_thread.join();

    expect_true(disk.unregister_memory(registered_memory).succeeded());
}

/**
 *  @brief Bytes a flush answered for are still there when the volume is registered again.
 *  @note Only a transport with a medium under it can pass this; a volume in memory is gone with its
 *      medium, which is why the RAM driver does not run it.
 */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_keeps_what_it_flushed(disk_type_& disk, util::medium_options_t const& request) {
    test_buffer_t buffer = make_buffer(2 * block_bytes_k);
    expect_equal(buffer.size(), 2 * block_bytes_k);
    if (buffer.size() != 2 * block_bytes_k) return;

    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) return;
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());

    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) return;
    std::array<util::completion_t, polling_room_k> completions {};

    auto [first_medium, register_first_status] = disk.register_medium(request);
    expect_true(register_first_status.succeeded());
    if (register_first_status.succeeded()) {
        paint(whole(buffer).first(block_bytes_k), 99);
        std::array<util::request_t, 1> writes {
            util::write_request(first_medium.reference(), util::extent_t {0, block_bytes_k},
                                registered_memory.reference(0), queue_pair.token(0))};
        expect_true(disk.submit(queue_pair, std::span<util::request_t const> {writes}) == std::size_t {1});
        expect_true(util::poll_completions_at_least<waiter_t>(
                        disk, queue_pair, std::span<util::completion_t> {completions}, 1) == std::size_t {1});
        std::array<util::request_t, 1> flushes {util::flush_request(first_medium.reference(), queue_pair.token(1))};
        expect_true(disk.submit(queue_pair, std::span<util::request_t const> {flushes}) == std::size_t {1});
        expect_true(util::poll_completions_at_least<waiter_t>(
                        disk, queue_pair, std::span<util::completion_t> {completions}, 1) == std::size_t {1});
        expect_true(disk.unregister_medium(first_medium).succeeded());
    }

    util::medium_options_t reopened = request;
    reopened.access = util::medium_access_t::read_write_k;
    auto [second_medium, register_second_status] = disk.register_medium(reopened);
    expect_true(register_second_status.succeeded());
    if (register_second_status.succeeded()) {
        std::array<util::request_t, 1> reads {
            util::read_request(second_medium.reference(), util::extent_t {0, block_bytes_k},
                               registered_memory.reference(block_bytes_k), queue_pair.token(2))};
        expect_true(disk.submit(queue_pair, std::span<util::request_t const> {reads}) == std::size_t {1});
        expect_true(util::poll_completions_at_least<waiter_t>(
                        disk, queue_pair, std::span<util::completion_t> {completions}, 1) == std::size_t {1});
        expect_true(completions[0].completed_in_full());
        expect_true(
            same_bytes(whole(buffer).first(block_bytes_k), whole(buffer).subspan(block_bytes_k, block_bytes_k)));
        expect_true(disk.unregister_medium(second_medium).succeeded());
    }

    expect_true(disk.unregister_memory(registered_memory).succeeded());
}

/** Deallocating a written extent of a file gives its blocks back to the filesystem, and it
 *  reads as zeros. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_deallocates_a_file_extent(disk_type_& disk, util::medium_options_t const& request) {
    constexpr std::size_t blocks_k = 16;
    test_buffer_t buffer = make_buffer(blocks_k * block_bytes_k);
    expect_equal(buffer.size(), blocks_k * block_bytes_k);
    if (buffer.size() != blocks_k * block_bytes_k) return;

    auto [registered_medium, register_medium_status] = disk.register_medium(request);
    expect_true(register_medium_status.succeeded());
    if (register_medium_status.failed()) return;
    auto [queue, make_queue_status] = disk_type_::queue_t::make(queue_pair_depth_k, {});
    expect_true(make_queue_status.succeeded());
    if (make_queue_status.failed()) {
        expect_true(disk.unregister_medium(registered_medium).succeeded());
        return;
    }
    util::queue_ref_t const queue_pair {queue, util::queue_id_t {0}};
    expect_true(disk.register_media(queue_pair).succeeded());
    auto [registered_memory, register_memory_status] = disk.register_memory(whole(buffer),
                                                                            util::memory_space_t::host_k);
    expect_true(register_memory_status.succeeded());
    if (register_memory_status.failed()) {
        expect_true(disk.unregister_medium(registered_medium).succeeded());
        return;
    }
    util::extent_t const extent {0, blocks_k * block_bytes_k};
    std::array<util::completion_t, polling_room_k> completions {};

    paint(whole(buffer), 5);
    std::array<util::request_t, 2> writes {
        util::write_request(registered_medium.reference(), extent, registered_memory.reference(0), queue_pair.token(0)),
        util::flush_request(registered_medium.reference(), queue_pair.token(1))};
    writes[0].chaining = util::request_chaining_t::chained_k;
    expect_true(disk.submit(queue_pair, std::span<util::request_t const> {writes}) == std::size_t {2});
    expect_true(util::poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<util::completion_t> {completions},
                                                          2) == std::size_t {2});
    struct stat written {};
    expect_true(::stat(request.name, &written) == 0);

    std::array<util::request_t, 1> deallocations {
        util::deallocate_request(registered_medium.reference(), extent, queue_pair.token(2))};
    expect_true(disk.submit(queue_pair, std::span<util::request_t const> {deallocations}) == std::size_t {1});
    expect_true(util::poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<util::completion_t> {completions},
                                                          1) == std::size_t {1});
    expect_true(completions[0].completed_in_full());
    struct stat punched {};
    expect_true(::stat(request.name, &punched) == 0);
    expect_true(punched.st_blocks < written.st_blocks);
    expect_equal(static_cast<std::uint64_t>(punched.st_size), static_cast<std::uint64_t>(written.st_size));

    std::array<util::request_t, 1> reads {
        util::read_request(registered_medium.reference(), extent, registered_memory.reference(0), queue_pair.token(3))};
    expect_true(disk.submit(queue_pair, std::span<util::request_t const> {reads}) == std::size_t {1});
    expect_true(util::poll_completions_at_least<waiter_t>(disk, queue_pair, std::span<util::completion_t> {completions},
                                                          1) == std::size_t {1});
    expect_true(completions[0].completed_in_full());
    test_buffer_t zeros = make_buffer(blocks_k * block_bytes_k);
    std::memset(zeros.data(), 0, zeros.size());
    expect_true(same_bytes(whole(buffer), whole(zeros)));

    expect_true(disk.unregister_memory(registered_memory).succeeded());
    expect_true(disk.unregister_medium(registered_medium).succeeded());
}

/** A file reports a power-of-two logical block, and an unbuffered disk aligns every request to
 *  whole ones of it. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void test_ring_reports_a_logical_block(disk_type_& disk, util::medium_options_t const& request) {
    auto [registered_medium, register_medium_status] = disk.register_medium(request);
    expect_true(register_medium_status.succeeded());
    if (register_medium_status.failed()) return;
    std::uint64_t const logical_block_bytes = registered_medium.logical_block_bytes();
    expect_true(std::has_single_bit(logical_block_bytes));
    if (std::has_single_bit(logical_block_bytes))
        expect_equal(disk.capabilities().offset_alignment % logical_block_bytes, std::uint64_t {0});
    expect_true(disk.unregister_medium(registered_medium).succeeded());
}

/** Racing failures on one countdown release one whole pair: the released note is the one its code's
 *  thread sent, since only the thread that wins the code writes the note. */
case_result_t test_countdown_keeps_one_failure_whole() {
    using countdown_t = util::countdown<atomic_reference, waiter_t>;
    using notifier_t = util::notifier<atomic_reference, waiter_t>;
    constexpr std::array<util::status_t, 16> failures {
        util::status_t {util::status_code_t::out_of_memory_k, "First racing part"},
        util::status_t {util::status_code_t::read_only_k, "Second racing part"},
        util::status_t {util::status_code_t::io_k, "Third racing part"},
        util::status_t {util::status_code_t::device_k, "Fourth racing part"},
        util::status_t {util::status_code_t::malformed_k, "Fifth racing part"},
        util::status_t {util::status_code_t::internal_k, "Sixth racing part"},
        util::status_t {util::status_code_t::uninitialized_k, "Seventh racing part"},
        util::status_t {util::status_code_t::contended_k, "Eighth racing part"},
        util::status_t {util::status_code_t::cancelled_k, "Ninth racing part"},
        util::status_t {util::status_code_t::unsupported_k, "Tenth racing part"},
        util::status_t {util::status_code_t::invalid_argument_k, "Eleventh racing part"},
        util::status_t {util::status_code_t::not_found_k, "Twelfth racing part"},
        util::status_t {util::status_code_t::already_exists_k, "Thirteenth racing part"},
        util::status_t {util::status_code_t::write_conflict_k, "Fourteenth racing part"},
        util::status_t {util::status_code_t::read_conflict_k, "Fifteenth racing part"},
        util::status_t {util::status_code_t::capacity_exhausted_k, "Sixteenth racing part"},
    };
    if (!(fu_comptime_capabilities() & fu_capability_os_threads_k))
        return case_result_t::skipped("needs fu_capability_os_threads_k");
    // Without memory placement ForkUnion spawns a flat pool of plain threads, which pins nothing
    fu::topology_t topology;
    if (!expect_true(fu::succeeded(topology.harvest()))) return {};
    auto [pool, pool_status] = util::spawn_pool<fu::pool_t>(
        topology, failures.size() + 1, fu::caller_inclusive_k,
        static_cast<fu_capabilities_t>(fu_capabilities_all_k & ~fu_capability_place_memory_on_domain_k));
    if (!expect_true(st::succeeded(pool_status))) return {};

    // Thread zero waits while every other thread fails with its own pair, rotated each round
    countdown_t counter;
    notifier_t const notifier {counter};
    for (std::size_t round = 0, rounds = scaled_iterations(256); round != rounds; ++round) {
        counter.reset(failures.size());
        util::status_t released;
        fu::status_t const dispatched = pool.for_threads([&](std::size_t thread) noexcept {
            if (thread != 0) notifier.fail(failures[(thread - 1 + round) % failures.size()]);
            else {
                expect_true(!counter.wait());
                released = counter.release_status();
            }
        });
        if (!expect_true(!fu::failed(dispatched))) return {};
        auto const sent = std::find_if(failures.begin(), failures.end(),
                                       [&](util::status_t const& failure) { return failure.code == released.code; });
        if (!expect_true(sent != failures.end(), "round {}", round)) return {};
        expect_true(sent->note == released.note, "round {}", round);
    }
    return {};
}

#pragma endregion Cases
#pragma region Drivers

/** Runs every transport-independent case against one mounted disk, labelling each as its function
 *  instantiated for @p disk_type , the disk's type as the caller spells it, like
 *  <tt>test_ring_cuts_a_chain<memory_disk_t></tt>. */
template <typename disk_type_>
    requires util::a_disk<disk_type_>
void ring_cases(char const* disk_type, disk_type_& disk, util::medium_reference_t medium,
                ring_edge_t edge = ring_edge_t::clipped_k) {
    auto const label = [disk_type](char const* function) { return fmt::format("{}<{}>", function, disk_type); };
    run_test_case(label("test_ring_reads_back_what_it_wrote").c_str(), test_ring_reads_back_what_it_wrote<disk_type_>,
                  disk, medium);
    run_test_case(label("test_ring_reaches_depth_beyond_its_credit").c_str(),
                  test_ring_reaches_depth_beyond_its_credit<disk_type_>, disk, medium);
    run_test_case(label("test_ring_refuses_when_full").c_str(), test_ring_refuses_when_full<disk_type_>, disk, medium);
    run_test_case(label("test_ring_reports_a_partial_transfer").c_str(),
                  test_ring_reports_a_partial_transfer<disk_type_>, disk, medium, edge);
    run_test_case(label("test_ring_refuses_a_torn_read").c_str(), test_ring_refuses_a_torn_read<disk_type_>, disk,
                  medium);
    run_test_case(label("test_ring_drains_a_cancelled_queue_pair").c_str(),
                  test_ring_drains_a_cancelled_queue_pair<disk_type_>, disk, medium);
    run_test_case(label("test_ring_separates_two_queue_pairs").c_str(), test_ring_separates_two_queue_pairs<disk_type_>,
                  disk, medium);
    run_test_case(label("test_ring_refuses_a_foreign_token").c_str(), test_ring_refuses_a_foreign_token<disk_type_>,
                  disk, medium);
    run_test_case(label("test_ring_holds_a_registered_memory_in_flight").c_str(),
                  test_ring_holds_a_registered_memory_in_flight<disk_type_>, disk, medium);
    run_test_case(label("test_ring_cuts_a_chain").c_str(), test_ring_cuts_a_chain<disk_type_>, disk, medium);
    run_test_case(label("test_ring_flushes_what_it_reaped").c_str(), test_ring_flushes_what_it_reaped<disk_type_>, disk,
                  medium);
    run_test_case(label("test_ring_refuses_an_impossible_queue_pair").c_str(),
                  test_ring_refuses_an_impossible_queue_pair<disk_type_>, disk, medium);
    run_test_case(label("test_ring_exhausts_its_slots").c_str(), test_ring_exhausts_its_slots<disk_type_>, disk,
                  medium);
    run_test_case(label("test_ring_refuses_an_oversized_wait").c_str(), test_ring_refuses_an_oversized_wait<disk_type_>,
                  disk, medium);
    run_test_case(label("test_ring_serves_a_queue_on_another_thread").c_str(),
                  test_ring_serves_a_queue_on_another_thread<disk_type_>, disk, medium);
}

/**
 *  @brief Registers one volume on @p disk as the case @p label, runs @p body against it, and gives
 *      the medium back.
 *  @param[in] label The case label of the registered medium itself.
 *  @param[in,out] disk The mounted ring the volume is registered on.
 *  @param[in] request The volume to register, which a RAM device allocates and a file device opens.
 *  @param[in] body What to run against the registered medium, running its own cases.
 */
template <typename disk_type_, typename body_type_>
    requires util::a_disk<disk_type_>
void on_medium(char const* label, disk_type_& disk, util::medium_options_t const& request, body_type_&& body) {
    auto [registered_medium, register_status] = disk.register_medium(request);
    if (register_status.succeeded()) body(registered_medium.reference());
    util::status_t const released = register_status.succeeded() ? disk.unregister_medium(registered_medium)
                                                                : register_status;
    run_test_case(label, [&] { expect_status(released, util::status_code_t::success_k); });
}

/** The volume every case reads and writes, sized so a request can run past its end. */
[[nodiscard]] util::medium_options_t volume_request(char const* name) noexcept {
    util::medium_options_t request;
    request.name = name;
    request.length = volume_bytes_k;
    request.access = util::medium_access_t::create_read_write_k;
    return request;
}

#pragma endregion Drivers

} // namespace unum::ustore::test::nvme
