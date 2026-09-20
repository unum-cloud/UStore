/**
 *  @file test/nvme_ring_cases.hpp
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

#include "nvme/aligned_allocator.hpp" // `aligned_allocator`
#include "nvme/extent.hpp"            // `extent_t`, `portable_alignment_k`
#include "nvme/ring.hpp"              // `request_t`, `completion_t`, `ring_concept`

#include "nvme_fixture.hpp" // `case_label_t`, `same_bytes`

namespace unum::ustore {

using namespace unum::ustore::nvme;

#pragma region Fixtures

/** Bytes of one block every case reads and writes, which is also the portable alignment. */
constexpr std::size_t block_bytes_k = 4096;

/** Blocks the volume of every case holds. */
constexpr std::size_t volume_blocks_k = 64;

/** Bytes of the volume of every case. */
constexpr std::uint64_t volume_bytes_k = block_bytes_k * volume_blocks_k;

/** Requests one lease of the suite keeps in flight. */
constexpr std::uint32_t lease_depth_k = 8;

/** Room for the completions one reap moves. */
constexpr std::size_t reaping_room_k = 64;

/** Registrations one case asks for before it expects the mount to run out of slots. */
constexpr std::size_t registrations_probe_k = 64;

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

/** The whole buffer as the span a registration takes. */
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
std::size_t ring_reads_back_what_it_wrote(disk_type_& disk, target_place_t target) {
    std::size_t failures = 0;
    constexpr std::size_t blocks_k = 4;
    test_buffer_t buffer = make_buffer(2 * blocks_k * block_bytes_k);
    failures += expect_eq(buffer.size(), 2 * blocks_k * block_bytes_k);
    if (buffer.size() != 2 * blocks_k * block_bytes_k) return failures;

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

    std::span<byte_t> const source = whole(buffer).first(blocks_k * block_bytes_k);
    std::span<byte_t> const destination = whole(buffer).subspan(blocks_k * block_bytes_k);
    paint(source, 1);
    paint(destination, 0);

    std::array<request_t, blocks_k> writes {};
    for (std::size_t index = 0; index != blocks_k; ++index)
        writes[index] = write_request(target, extent_t {index * block_bytes_k, block_bytes_k},
                                      registration.place(index * block_bytes_k),
                                      owner.token(static_cast<std::uint32_t>(index)));
    expected<std::size_t> const submitted = disk.submit(owner, std::span<request_t const> {writes});
    failures += expect(submitted == blocks_k);

    std::array<completion_t, reaping_room_k> completions {};
    expected<std::size_t> const written = reap_at_least(disk, owner, std::span<completion_t> {completions}, blocks_k);
    failures += expect(written == blocks_k);
    for (std::size_t index = 0; index != blocks_k; ++index) {
        failures += expect(completions[index].whole());
        failures += expect_eq(completions[index].transferred, std::uint64_t {block_bytes_k});
    }

    std::array<request_t, blocks_k> reads {};
    for (std::size_t index = 0; index != blocks_k; ++index)
        reads[index] = read_request(target, extent_t {index * block_bytes_k, block_bytes_k},
                                    registration.place((blocks_k + index) * block_bytes_k),
                                    owner.token(static_cast<std::uint32_t>(blocks_k + index)));
    expected<std::size_t> const asked = disk.submit(owner, std::span<request_t const> {reads});
    failures += expect(asked == blocks_k);
    expected<std::size_t> const read = reap_at_least(disk, owner, std::span<completion_t> {completions}, blocks_k);
    failures += expect(read == blocks_k);
    for (std::size_t index = 0; index != blocks_k; ++index) failures += expect(completions[index].whole());
    failures += expect(same_bytes(source, destination));

    failures += expect(disk.drain(owner).succeeded());
    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

/** One lease of eight carries sixty-four requests by reaping what it submitted and submitting again. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_reaches_depth_beyond_its_credit(disk_type_& disk, target_place_t target) {
    std::size_t failures = 0;
    constexpr std::size_t rounds_k = 8;
    test_buffer_t buffer = make_buffer(lease_depth_k * block_bytes_k);
    failures += expect_eq(buffer.size(), lease_depth_k * block_bytes_k);
    if (buffer.size() != lease_depth_k * block_bytes_k) return failures;

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

    std::size_t retired = 0;
    std::array<completion_t, reaping_room_k> completions {};
    for (std::size_t round = 0; round != rounds_k; ++round) {
        paint(whole(buffer), round);
        std::array<request_t, lease_depth_k> batch {};
        for (std::size_t index = 0; index != lease_depth_k; ++index) {
            std::size_t const block = (round * lease_depth_k + index) % volume_blocks_k;
            batch[index] = write_request(target, extent_t {block * block_bytes_k, block_bytes_k},
                                         registration.place(index * block_bytes_k),
                                         owner.token(static_cast<std::uint32_t>(retired + index)));
        }
        expected<std::size_t> const submitted = disk.submit(owner, std::span<request_t const> {batch});
        failures += expect(submitted == lease_depth_k);
        expected<std::size_t> const reaped = reap_at_least(disk, owner, std::span<completion_t> {completions},
                                                           lease_depth_k);
        failures += expect(reaped == lease_depth_k);
        for (std::size_t index = 0; index != lease_depth_k; ++index) failures += expect(completions[index].whole());
        retired += lease_depth_k;
    }
    failures += expect_eq(retired, rounds_k * lease_depth_k);
    failures += expect_eq(disk.in_flight(owner), std::size_t {0});

    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

/** A full ring takes a prefix and then nothing at all, and never waits for room. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_refuses_when_full(disk_type_& disk, target_place_t target) {
    std::size_t failures = 0;
    constexpr std::size_t offered_k = lease_depth_k + 3;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

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

    std::array<request_t, offered_k> batch {};
    for (std::size_t index = 0; index != offered_k; ++index)
        batch[index] = read_request(target, extent_t {0, block_bytes_k}, registration.place(0),
                                    owner.token(static_cast<std::uint32_t>(index)));
    expected<std::size_t> const first = disk.submit(owner, std::span<request_t const> {batch});
    failures += expect(first == std::size_t {lease_depth_k});
    expected<std::size_t> const second = disk.submit(owner, std::span<request_t const> {batch}.first(1));
    failures += expect(second == std::size_t {0});
    failures += expect_eq(disk.in_flight(owner), std::size_t {lease_depth_k});

    std::array<completion_t, reaping_room_k> completions {};
    expected<std::size_t> const reaped = reap_at_least(disk, owner, std::span<completion_t> {completions},
                                                       lease_depth_k);
    failures += expect(reaped == std::size_t {lease_depth_k});
    expected<std::size_t> const third = disk.submit(owner, std::span<request_t const> {batch}.first(1));
    failures += expect(third == std::size_t {1});

    failures += expect(disk.drain(owner).succeeded());
    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

/** A read that meets the end of the volume moves a prefix, says so, and leaves the tail alone. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_reports_a_partial_transfer(disk_type_& disk, target_place_t target, ring_edge_t edge) {
    std::size_t failures = 0;
    std::uint64_t const reachable = edge == ring_edge_t::clipped_k ? 1024 : block_bytes_k;
    std::size_t const bytes = edge == ring_edge_t::clipped_k ? block_bytes_k : 2 * block_bytes_k;
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

    constexpr byte_t poison_k = static_cast<byte_t>(0xA5u);
    for (std::size_t index = 0; index != buffer.size(); ++index) buffer[index] = poison_k;

    std::array<request_t, 1> batch {
        read_request(target, extent_t {volume_bytes_k - reachable, bytes}, registration.place(0), owner.token(0))};
    expected<std::size_t> const submitted = disk.submit(owner, std::span<request_t const> {batch});
    failures += expect(submitted == std::size_t {1});

    std::array<completion_t, reaping_room_k> completions {};
    expected<std::size_t> const reaped = reap_at_least(disk, owner, std::span<completion_t> {completions}, 1);
    failures += expect(reaped == std::size_t {1});
    failures += expect(!completions[0].whole());
    if (edge == ring_edge_t::clipped_k) {
        failures += expect(completions[0].state == completion_state_t::partial_k);
        failures += expect_eq(completions[0].transferred, reachable);
    }
    else {
        failures += expect(completions[0].state == completion_state_t::failed_k);
        failures += expect_status(completions[0].status, status_code_t::invalid_argument_k);
        failures += expect_eq(completions[0].transferred, std::uint64_t {0});
        failures += expect(buffer[0] == poison_k);
    }

    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

/** A read wholly past the end of the volume refuses rather than handing back whatever the buffer held. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_refuses_a_torn_read(disk_type_& disk, target_place_t target) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

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

    constexpr byte_t poison_k = static_cast<byte_t>(0x5Au);
    for (std::size_t index = 0; index != buffer.size(); ++index) buffer[index] = poison_k;

    std::array<request_t, 1> batch {read_request(target, extent_t {volume_bytes_k + block_bytes_k, block_bytes_k},
                                                 registration.place(0), owner.token(0))};
    expected<std::size_t> const submitted = disk.submit(owner, std::span<request_t const> {batch});
    failures += expect(submitted == std::size_t {1});

    std::array<completion_t, reaping_room_k> completions {};
    expected<std::size_t> const reaped = reap_at_least(disk, owner, std::span<completion_t> {completions}, 1);
    failures += expect(reaped == std::size_t {1});
    failures += expect(!completions[0].whole());
    failures += expect_eq(completions[0].transferred, std::uint64_t {0});

    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

/** A cancelled owner's outstanding requests come back cancelled, and its lease drains and releases. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_drains_a_cancelled_owner(disk_type_& disk, target_place_t target) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(lease_depth_k * block_bytes_k);
    failures += expect_eq(buffer.size(), lease_depth_k * block_bytes_k);
    if (buffer.size() != lease_depth_k * block_bytes_k) return failures;

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

    std::array<request_t, lease_depth_k> batch {};
    for (std::size_t index = 0; index != lease_depth_k; ++index)
        batch[index] = read_request(target, extent_t {index * block_bytes_k, block_bytes_k},
                                    registration.place(index * block_bytes_k),
                                    owner.token(static_cast<std::uint32_t>(index)));
    expected<std::size_t> const submitted = disk.submit(owner, std::span<request_t const> {batch});
    failures += expect(submitted == std::size_t {lease_depth_k});

    // A cancel stops what has not started, so a transport that runs its requests eagerly may have
    // finished them already; what every transport owes is one terminal completion per request.
    disk.cancel(owner);
    std::array<completion_t, reaping_room_k> completions {};
    std::array<std::uint32_t, lease_depth_k> seen {};
    std::size_t retired = 0;
    for (std::size_t spins = 0; retired != lease_depth_k && spins != reap_spin_limit_k; ++spins) {
        expected<std::size_t> const reaped = disk.reap(owner, std::span<completion_t> {completions});
        failures += expect(reaped.has_value());
        if (!reaped) break;
        for (std::size_t index = 0; index != *reaped; ++index) {
            completion_t const& completion = completions[index];
            if (!completion.retires()) continue;
            std::uint32_t const tag = completion.token.tag();
            failures += expect(tag < lease_depth_k);
            if (tag < lease_depth_k) ++seen[tag];
            if (completion.state == completion_state_t::cancelled_k)
                failures += expect_eq(completion.transferred, std::uint64_t {0});
            ++retired;
        }
    }
    failures += expect_eq(retired, std::size_t {lease_depth_k});
    for (std::size_t index = 0; index != lease_depth_k; ++index) failures += expect_eq(seen[index], std::uint32_t {1});

    failures += expect(disk.drain(owner).succeeded());
    failures += expect_eq(disk.in_flight(owner), std::size_t {0});
    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

/** Two owners interleave their batches on one ring and each reaps only the tokens it minted. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
verdict_t ring_separates_two_owners(disk_type_& disk, target_place_t target) {
    std::size_t failures = 0;
    if (disk.capabilities().owners_limit < 2) return verdict_t::skipped("the ring leases one owner");

    test_buffer_t buffer = make_buffer(2 * lease_depth_k * block_bytes_k);
    failures += expect_eq(buffer.size(), 2 * lease_depth_k * block_bytes_k);
    if (buffer.size() != 2 * lease_depth_k * block_bytes_k) return failures;

    expected<ring_owner_t> first_leased = disk.open(lease_depth_k);
    expected<ring_owner_t> second_leased = disk.open(lease_depth_k);
    failures += expect(first_leased.has_value());
    failures += expect(second_leased.has_value());
    if (!first_leased || !second_leased) {
        if (first_leased) failures += expect(disk.release(*first_leased).succeeded());
        if (second_leased) failures += expect(disk.release(*second_leased).succeeded());
        return failures;
    }
    ring_owner_t first = std::move(*first_leased);
    ring_owner_t second = std::move(*second_leased);

    expected<registration_t> pinned = disk.register_chunk(whole(buffer), memory_space_t::host_k);
    failures += expect(pinned.has_value());
    if (!pinned) {
        failures += expect(disk.release(first).succeeded());
        failures += expect(disk.release(second).succeeded());
        return failures;
    }
    registration_t registration = std::move(*pinned);

    std::array<request_t, lease_depth_k> first_batch {};
    std::array<request_t, lease_depth_k> second_batch {};
    for (std::size_t index = 0; index != lease_depth_k; ++index) {
        first_batch[index] = read_request(target, extent_t {index * block_bytes_k, block_bytes_k},
                                          registration.place(index * block_bytes_k),
                                          first.token(static_cast<std::uint32_t>(index)));
        second_batch[index] = read_request(target, extent_t {index * block_bytes_k, block_bytes_k},
                                           registration.place((lease_depth_k + index) * block_bytes_k),
                                           second.token(static_cast<std::uint32_t>(100 + index)));
    }
    failures += expect(disk.submit(first, std::span<request_t const> {first_batch}.first(4)) == std::size_t {4});
    failures += expect(disk.submit(second, std::span<request_t const> {second_batch}.first(4)) == std::size_t {4});
    failures += expect(disk.submit(first, std::span<request_t const> {first_batch}.subspan(4)) ==
                       std::size_t {lease_depth_k - 4});

    std::array<completion_t, reaping_room_k> completions {};
    expected<std::size_t> const first_reaped = reap_at_least(disk, first, std::span<completion_t> {completions},
                                                             lease_depth_k);
    failures += expect(first_reaped == std::size_t {lease_depth_k});
    for (std::size_t index = 0; index != lease_depth_k; ++index) {
        failures += expect(completions[index].token.owner() == first.id());
        failures += expect(completions[index].token.tag() < 100u);
    }

    expected<std::size_t> const second_reaped = reap_at_least(disk, second, std::span<completion_t> {completions}, 4);
    failures += expect(second_reaped == std::size_t {4});
    for (std::size_t index = 0; index != 4; ++index) {
        failures += expect(completions[index].token.owner() == second.id());
        failures += expect(completions[index].token.tag() >= 100u);
    }

    failures += expect(disk.drain(first).succeeded());
    failures += expect(disk.drain(second).succeeded());
    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(first).succeeded());
    failures += expect(disk.release(second).succeeded());
    return failures;
}

/** A batch carrying another lease's token is refused whole, since a misrouted completion is unrecoverable. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_refuses_a_foreign_token(disk_type_& disk, target_place_t target) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

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

    request_token_t const foreign = request_token_t::make(static_cast<owner_id_t>(7), owner.generation(), 0);
    std::array<request_t, 1> batch {read_request(target, extent_t {0, block_bytes_k}, registration.place(0), foreign)};
    expected<std::size_t> const submitted = disk.submit(owner, std::span<request_t const> {batch});
    failures += expect(submitted.failed());
    failures += expect_status(submitted, status_code_t::invalid_argument_k);
    failures += expect_eq(disk.in_flight(owner), std::size_t {0});

    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

/** A registered range cannot be handed back while the device may still be writing into it. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_holds_a_registration_in_flight(disk_type_& disk, target_place_t target) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

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

    std::array<request_t, 1> batch {
        read_request(target, extent_t {0, block_bytes_k}, registration.place(0), owner.token(0))};
    failures += expect(disk.submit(owner, std::span<request_t const> {batch}) == std::size_t {1});
    failures += expect(disk.deregister_chunk(registration).failed());
    failures += expect(registration.held());

    std::array<completion_t, reaping_room_k> completions {};
    failures += expect(reap_at_least(disk, owner, std::span<completion_t> {completions}, 1) == std::size_t {1});
    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(!registration.held());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

/** A chain runs in submission order and is cut at the first result that is not whole. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_cuts_a_chain(disk_type_& disk, target_place_t target) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(3 * block_bytes_k);
    failures += expect_eq(buffer.size(), 3 * block_bytes_k);
    if (buffer.size() != 3 * block_bytes_k) return failures;

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

    std::array<request_t, 3> chain {};
    chain[0] = read_request(target, extent_t {0, block_bytes_k}, registration.place(0), owner.token(0));
    chain[0].chaining = request_chaining_t::chained_k;
    chain[1] = read_request(target, extent_t {volume_bytes_k, block_bytes_k}, registration.place(block_bytes_k),
                            owner.token(1));
    chain[1].chaining = request_chaining_t::chained_k;
    chain[2] = read_request(target, extent_t {block_bytes_k, block_bytes_k}, registration.place(2 * block_bytes_k),
                            owner.token(2));

    failures += expect(disk.submit(owner, std::span<request_t const> {chain}) == std::size_t {3});
    std::array<completion_t, reaping_room_k> completions {};
    failures += expect(reap_at_least(disk, owner, std::span<completion_t> {completions}, 3) == std::size_t {3});
    failures += expect(completions[0].whole());
    failures += expect(!completions[1].whole());
    failures += expect(completions[2].state == completion_state_t::cancelled_k);

    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

/** Durability is written as submit, reap, flush, reap, since the ring orders nothing on its own. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_flushes_what_it_reaped(disk_type_& disk, target_place_t target) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

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

    paint(whole(buffer), 42);
    std::array<request_t, 1> writes {
        write_request(target, extent_t {0, block_bytes_k}, registration.place(0), owner.token(0))};
    failures += expect(disk.submit(owner, std::span<request_t const> {writes}) == std::size_t {1});
    std::array<completion_t, reaping_room_k> completions {};
    failures += expect(reap_at_least(disk, owner, std::span<completion_t> {completions}, 1) == std::size_t {1});
    failures += expect(completions[0].whole());

    std::array<request_t, 1> flushes {flush_request(target, owner.token(1))};
    failures += expect(disk.submit(owner, std::span<request_t const> {flushes}) == std::size_t {1});
    failures += expect(reap_at_least(disk, owner, std::span<completion_t> {completions}, 1) == std::size_t {1});
    failures += expect(completions[0].whole());
    failures += expect_eq(completions[0].token.tag(), std::uint32_t {1});

    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

/** A request that misses the disk's alignment fails on its own completion, and not on the submission,
 *  which is what the kernel does with a misaligned @c O_DIRECT read: it is accepted into the ring and
 *  comes back with @c -EINVAL on that one entry, leaving the rest of the batch alone. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_fails_a_misaligned_request(disk_type_& disk, target_place_t target) {
    std::size_t failures = 0;
    ring_capabilities_t const limits = disk.capabilities();
    failures += expect_eq(limits.offset_alignment, std::uint64_t {portable_alignment_k});
    test_buffer_t buffer = make_buffer(2 * block_bytes_k);
    failures += expect_eq(buffer.size(), 2 * block_bytes_k);
    if (buffer.size() != 2 * block_bytes_k) return failures;

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

    std::array<request_t, 3> batch {};
    batch[0] = read_request(target, extent_t {1, block_bytes_k}, registration.place(0), owner.token(0));
    batch[1] = read_request(target, extent_t {0, block_bytes_k}, registration.place(1), owner.token(1));
    batch[2] = read_request(target, extent_t {0, block_bytes_k}, registration.place(block_bytes_k), owner.token(2));
    failures += expect(disk.submit(owner, std::span<request_t const> {batch}) == std::size_t {3});

    std::array<completion_t, reaping_room_k> completions {};
    failures += expect(reap_at_least(disk, owner, std::span<completion_t> {completions}, 3) == std::size_t {3});
    failures += expect(completions[0].state == completion_state_t::failed_k);
    failures += expect_status(completions[0].status, status_code_t::invalid_argument_k);
    failures += expect(completions[1].state == completion_state_t::failed_k);
    failures += expect(completions[2].whole());

    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

/** A lease is refused when it asks for no depth, or for more than the mount reserved per lease. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_refuses_an_impossible_lease(disk_type_& disk, [[maybe_unused]] target_place_t target) {
    std::size_t failures = 0;

    expected<ring_owner_t> const none = disk.open(0);
    failures += expect(!none.has_value());
    failures += expect_status(none, status_code_t::invalid_argument_k);

    expected<ring_owner_t> const beyond = disk.open(disk.capabilities().depth_limit + 1);
    failures += expect(!beyond.has_value());
    failures += expect_status(beyond, status_code_t::invalid_argument_k);
    return failures;
}

/** Every lease, registration and target slot runs out, and each says so rather than overbooking. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_exhausts_its_slots(disk_type_& disk, [[maybe_unused]] target_place_t target) {
    std::size_t failures = 0;
    ring_capabilities_t const limits = disk.capabilities();

    st::basic_vector<ring_owner_t> leases;
    for (std::uint16_t index = 0; index != limits.owners_limit; ++index) {
        expected<ring_owner_t> leased = disk.open(1);
        failures += expect(leased.has_value());
        if (!leased) break;
        failures += expect(leases.push_back(std::move(*leased)) == st::status_t::success_k);
    }
    expected<ring_owner_t> const beyond = disk.open(1);
    failures += expect(!beyond.has_value());
    failures += expect_status(beyond, status_code_t::capacity_exhausted_k);
    for (std::size_t index = 0; index != leases.size(); ++index)
        failures += expect(disk.release(leases[index]).succeeded());

    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;
    st::basic_vector<registration_t> ranges;
    bool refused = false;
    for (std::size_t attempt = 0; attempt != registrations_probe_k && !refused; ++attempt) {
        expected<registration_t> pinned = disk.register_chunk(whole(buffer), memory_space_t::host_k);
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
        failures += expect(disk.deregister_chunk(ranges[index]).succeeded());
    return failures;
}

/** A lease with completions nobody reaped refuses to go back, since its buffers are still borrowed. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_refuses_an_undrained_release(disk_type_& disk, target_place_t target) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

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

    std::array<request_t, 1> batch {
        read_request(target, extent_t {0, block_bytes_k}, registration.place(0), owner.token(0))};
    failures += expect(disk.submit(owner, std::span<request_t const> {batch}) == std::size_t {1});
    failures += expect(disk.release(owner).failed());

    failures += expect(disk.drain(owner).succeeded());
    failures += expect_eq(disk.in_flight(owner), std::size_t {0});
    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

/** A target with requests against it refuses to be given back, exactly as a registered range does. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_holds_a_target_in_flight(disk_type_& disk, target_request_t const& request) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

    expected<descriptor_t> registered = disk.register_target(request);
    failures += expect(registered.has_value());
    if (!registered) return failures;
    descriptor_t descriptor = std::move(*registered);

    expected<ring_owner_t> leased = disk.open(lease_depth_k);
    failures += expect(leased.has_value());
    if (!leased) {
        failures += expect(disk.deregister_target(descriptor).succeeded());
        return failures;
    }
    ring_owner_t owner = std::move(*leased);

    expected<registration_t> pinned = disk.register_chunk(whole(buffer), memory_space_t::host_k);
    failures += expect(pinned.has_value());
    if (!pinned) {
        failures += expect(disk.release(owner).succeeded());
        failures += expect(disk.deregister_target(descriptor).succeeded());
        return failures;
    }
    registration_t registration = std::move(*pinned);

    std::array<request_t, 1> batch {
        read_request(descriptor.place(), extent_t {0, block_bytes_k}, registration.place(0), owner.token(0))};
    failures += expect(disk.submit(owner, std::span<request_t const> {batch}) == std::size_t {1});
    failures += expect(disk.deregister_target(descriptor).failed());
    failures += expect(descriptor.held());

    std::array<completion_t, reaping_room_k> completions {};
    failures += expect(reap_at_least(disk, owner, std::span<completion_t> {completions}, 1) == std::size_t {1});
    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    failures += expect(disk.deregister_target(descriptor).succeeded());
    return failures;
}

/** A bounded wait that wants more than it has room for is refused rather than truncating silently. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_refuses_an_oversized_wait(disk_type_& disk, [[maybe_unused]] target_place_t target) {
    std::size_t failures = 0;

    expected<ring_owner_t> leased = disk.open(lease_depth_k);
    failures += expect(leased.has_value());
    if (!leased) return failures;
    ring_owner_t owner = std::move(*leased);

    std::array<completion_t, 2> room {};
    expected<std::size_t> const wanted = reap_at_least(disk, owner, std::span<completion_t> {room}, 3, 1);
    failures += expect(!wanted.has_value());
    failures += expect_status(wanted, status_code_t::invalid_argument_k);

    failures += expect(disk.release(owner).succeeded());
    return failures;
}

/** A lease moves to another thread, which submits and reaps on it there and hands it back. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_moves_a_lease_between_threads(disk_type_& disk, target_place_t target) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(block_bytes_k);
    failures += expect_eq(buffer.size(), std::size_t {block_bytes_k});
    if (buffer.size() != block_bytes_k) return failures;

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

    std::size_t moved_failures = 0;
    std::thread issuer {[&]() noexcept {
        moved_failures += expect(disk.adopt(owner).succeeded());
        std::array<request_t, 1> batch {
            read_request(target, extent_t {0, block_bytes_k}, registration.place(0), owner.token(0))};
        moved_failures += expect(disk.submit(owner, std::span<request_t const> {batch}) == std::size_t {1});
        std::array<completion_t, reaping_room_k> completions {};
        moved_failures += expect(reap_at_least(disk, owner, std::span<completion_t> {completions}, 1) ==
                                 std::size_t {1});
        moved_failures += expect(completions[0].whole());
    }};
    issuer.join();
    failures += moved_failures;
    failures += expect(disk.adopt(owner).succeeded());

    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

/**
 *  @brief Bytes a flush answered for are still there when the volume is registered again.
 *  @note Only a transport with a medium under it can pass this; a volume in memory is gone with its
 *      target, which is why the RAM driver does not run it.
 */
template <typename disk_type_>
    requires ring_concept<disk_type_>
std::size_t ring_keeps_what_it_flushed(disk_type_& disk, target_request_t const& request) {
    std::size_t failures = 0;
    test_buffer_t buffer = make_buffer(2 * block_bytes_k);
    failures += expect_eq(buffer.size(), 2 * block_bytes_k);
    if (buffer.size() != 2 * block_bytes_k) return failures;

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
    std::array<completion_t, reaping_room_k> completions {};

    expected<descriptor_t> first = disk.register_target(request);
    failures += expect(first.has_value());
    if (first) {
        descriptor_t descriptor = std::move(*first);
        paint(whole(buffer).first(block_bytes_k), 99);
        std::array<request_t, 1> writes {
            write_request(descriptor.place(), extent_t {0, block_bytes_k}, registration.place(0), owner.token(0))};
        failures += expect(disk.submit(owner, std::span<request_t const> {writes}) == std::size_t {1});
        failures += expect(reap_at_least(disk, owner, std::span<completion_t> {completions}, 1) == std::size_t {1});
        std::array<request_t, 1> flushes {flush_request(descriptor.place(), owner.token(1))};
        failures += expect(disk.submit(owner, std::span<request_t const> {flushes}) == std::size_t {1});
        failures += expect(reap_at_least(disk, owner, std::span<completion_t> {completions}, 1) == std::size_t {1});
        failures += expect(disk.deregister_target(descriptor).succeeded());
    }

    target_request_t reopened = request;
    reopened.access = target_access_t::read_write_k;
    expected<descriptor_t> second = disk.register_target(reopened);
    failures += expect(second.has_value());
    if (second) {
        descriptor_t descriptor = std::move(*second);
        std::array<request_t, 1> reads {read_request(descriptor.place(), extent_t {0, block_bytes_k},
                                                     registration.place(block_bytes_k), owner.token(2))};
        failures += expect(disk.submit(owner, std::span<request_t const> {reads}) == std::size_t {1});
        failures += expect(reap_at_least(disk, owner, std::span<completion_t> {completions}, 1) == std::size_t {1});
        failures += expect(completions[0].whole());
        failures += expect(
            same_bytes(whole(buffer).first(block_bytes_k), whole(buffer).subspan(block_bytes_k, block_bytes_k)));
        failures += expect(disk.deregister_target(descriptor).succeeded());
    }

    failures += expect(disk.deregister_chunk(registration).succeeded());
    failures += expect(disk.release(owner).succeeded());
    return failures;
}

#pragma endregion Cases
#pragma region Drivers

/** Runs every transport-independent case against one mounted disk, reporting each under @c ring.<transport>. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
void ring_cases(ledger_t& ledger, char const* transport, disk_type_& disk, target_place_t target,
                ring_edge_t edge = ring_edge_t::clipped_k) {
    auto const label = [&](char const* name) noexcept { return case_label_t {"ring", transport, name}; };
    report(ledger, label("reads_back").c_str(), ring_reads_back_what_it_wrote(disk, target));
    report(ledger, label("depth_beyond_credit").c_str(), ring_reaches_depth_beyond_its_credit(disk, target));
    report(ledger, label("refuses_when_full").c_str(), ring_refuses_when_full(disk, target));
    report(ledger, label("partial_transfer").c_str(), ring_reports_a_partial_transfer(disk, target, edge));
    report(ledger, label("refuses_torn_read").c_str(), ring_refuses_a_torn_read(disk, target));
    report(ledger, label("cancelled_owner").c_str(), ring_drains_a_cancelled_owner(disk, target));
    report(ledger, label("separates_owners").c_str(), ring_separates_two_owners(disk, target));
    report(ledger, label("foreign_token").c_str(), ring_refuses_a_foreign_token(disk, target));
    report(ledger, label("registration_in_flight").c_str(), ring_holds_a_registration_in_flight(disk, target));
    report(ledger, label("cuts_a_chain").c_str(), ring_cuts_a_chain(disk, target));
    report(ledger, label("flushes_what_it_reaped").c_str(), ring_flushes_what_it_reaped(disk, target));
    report(ledger, label("impossible_lease").c_str(), ring_refuses_an_impossible_lease(disk, target));
    report(ledger, label("exhausts_its_slots").c_str(), ring_exhausts_its_slots(disk, target));
    report(ledger, label("undrained_release").c_str(), ring_refuses_an_undrained_release(disk, target));
    report(ledger, label("oversized_wait").c_str(), ring_refuses_an_oversized_wait(disk, target));
    report(ledger, label("moves_a_lease").c_str(), ring_moves_a_lease_between_threads(disk, target));
}

/**
 *  @brief Registers one volume on @p disk, runs @p body against it, and gives the target back.
 *  @param[inout] ledger Where the registration and its release report under @p name.
 *  @param[in] name The case label of the registration itself.
 *  @param[inout] disk The mounted ring the volume is registered on.
 *  @param[in] request The volume to register, which a RAM device allocates and a file device opens.
 *  @param[in] body What to run against the registered target, reporting its own cases.
 */
template <typename disk_type_, typename body_type_>
    requires ring_concept<disk_type_>
void on_target(ledger_t& ledger, char const* name, disk_type_& disk, target_request_t const& request,
               body_type_&& body) {
    std::size_t failures = 0;
    expected<descriptor_t> registered = disk.register_target(request);
    failures += expect(registered.has_value());
    if (registered) {
        descriptor_t descriptor = std::move(*registered);
        body(descriptor.place());
        failures += expect(disk.deregister_target(descriptor).succeeded());
    }
    report(ledger, name, failures);
}

/** The volume every case reads and writes, sized so a request can run past its end. */
[[nodiscard]] target_request_t volume_request(char const* name) noexcept {
    target_request_t request;
    request.name = name;
    request.length = volume_bytes_k;
    request.access = target_access_t::create_read_write_k;
    return request;
}

} // namespace unum::ustore
