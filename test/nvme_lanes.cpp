/**
 *  @file test/nvme_lanes.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The registered slab, the blocks lanes lease out of it, and the pipelines that fill a ring.
 *
 *  The verdict this file exists for is @c holds_its_depth: a pipeline must keep the ring at the
 *  lease's depth through the middle of a long run, since a batch that is submitted whole and waited
 *  on reaches the depth of that batch and no further, which is the defect the pipelines replace.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <array>   // `std::array`
#include <atomic>  // `std::atomic`
#include <memory>  // `std::unique_ptr`
#include <span>    // `std::span`
#include <thread>  // `std::thread`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "nvme/buffer_pool.hpp" // `buffer_pool_t`, `buffer_lease_t`
#include "nvme/io_pipeline.hpp" // `basic_read_pipeline`, `basic_write_pipeline`

#include "nvme_fixture.hpp" // `basic_fixture`, `on_transport`

namespace unum::ustore {

using namespace unum::ustore::nvme;

#pragma region Shape

/** Bytes of one block every case reads and writes. */
constexpr std::uint64_t block_bytes_k = 4096;

/** Blocks the volume of every case holds, which is far more than one lane's pool. */
constexpr std::uint64_t volume_blocks_k = 4096;

/** The numbers every case mounts with: blocks of two pages so a read can run past the end of the volume by one. */
constexpr fixture_shape_t lanes_shape_k {
    .request_block_bytes = 2 * block_bytes_k,
    .stream_block_bytes = 4 * block_bytes_k,
    .stream_blocks = 2,
    .volume_bytes = block_bytes_k * volume_blocks_k,
    .spare_lanes = 1,
    .executor_threads = 1,
    .memory_limit = std::size_t {64} * 1024 * 1024,
};

/** The disk, the lanes and the pipelines every case drives over @p fixture_type_. */
template <typename fixture_type_>
struct lanes_types {
    using fixture_t = fixture_type_;
    using guard_t = typename fixture_t::guard_t;
    using disk_t = typename fixture_t::disk_t;
    using read_pipeline_t = basic_read_pipeline<disk_t>;
    using write_pipeline_t = basic_write_pipeline<disk_t>;
};

/** Writes a repeatable pattern of @p seed into @p buffer, so a read can be checked against it. */
void paint(std::span<byte_t> buffer, std::uint64_t seed) noexcept {
    for (std::size_t index = 0; index != buffer.size(); ++index)
        buffer[index] = static_cast<byte_t>((index * 131u + seed * 17u + 7u) & 0xFFu);
}

#pragma endregion Shape

#pragma region Slab and Pool Cases

/** A slab charges its budget once and returns every byte when it dies. */
template <typename mount_type_>
std::size_t slab_charges_its_budget_once(mount_type_ const& mount) {
    using types_t = lanes_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    resources_shared_ptr_t const resources = (*fixture)->resources;
    std::size_t const charged = resources->memory_constraint.used();
    failures += expect(charged != 0);
    failures += expect_eq(charged, static_cast<std::size_t>((*fixture)->slab.size()));

    // Nothing below the slab is charged again, so its death returns every byte the mount paid for.
    (*fixture).reset();
    failures += expect_eq(resources->memory_constraint.used(), std::size_t {0});
    return failures;
}

/** Every block of a pool is handed out once, and the pool refuses once they are all out. */
template <typename mount_type_>
std::size_t pool_exhausts_and_recycles(mount_type_ const& mount) {
    using types_t = lanes_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    buffer_pool_t& pool = guard->lane().requests();
    failures += expect_eq(pool.available(), lanes_shape_k.request_blocks);

    st::basic_vector<buffer_lease_t> held;
    for (std::uint32_t index = 0; index != lanes_shape_k.request_blocks; ++index) {
        expected<buffer_lease_t> block = pool.acquire();
        failures += expect(block.has_value());
        if (!block) break;
        failures += expect(held.push_back(std::move(*block)) == st::status_t::success_k);
    }
    failures += expect_eq(pool.available(), std::uint32_t {0});
    expected<buffer_lease_t> const beyond = pool.acquire();
    failures += expect(!beyond.has_value());
    failures += expect_status(beyond, status_code_t::capacity_exhausted_k);

    // Every block resolves to its own place, so two leases never name the same bytes.
    for (std::size_t first = 0; first + 1 < held.size(); ++first)
        failures += expect(held[first].place().offset != held[first + 1].place().offset);

    held.clear();
    failures += expect_eq(pool.available(), lanes_shape_k.request_blocks);
    return failures;
}

/** A moved-from lease recycles nothing, and a destroyed one recycles exactly once. */
template <typename mount_type_>
std::size_t lease_recycles_exactly_once(mount_type_ const& mount) {
    using types_t = lanes_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    buffer_pool_t& pool = guard->lane().requests();
    std::uint32_t const before = pool.available();

    {
        expected<buffer_lease_t> first = pool.acquire();
        failures += expect(first.has_value());
        if (!first) return failures;
        failures += expect_eq(pool.available(), before - 1);

        buffer_lease_t moved = std::move(*first);
        failures += expect(moved.held());
        failures += expect(!first->held());
        failures += expect_eq(pool.available(), before - 1);

        moved.recycle();
        failures += expect_eq(pool.available(), before);
        moved.recycle();
        failures += expect_eq(pool.available(), before);
    }
    failures += expect_eq(pool.available(), before);
    return failures;
}

#pragma endregion Slab and Pool Cases

#pragma region Lane Cases

/** Every executor thread gets a lane of its own, and no two threads get the same lease. */
template <typename mount_type_>
std::size_t lanes_separate_executor_threads(mount_type_ const& mount) {
    using types_t = lanes_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    std::size_t failures = 0;
    constexpr std::size_t threads_k = 4;
    fixture_shape_t threaded = mount.shape;
    threaded.executor_threads = threads_k;
    expected<std::unique_ptr<fixture_t>> fixture = mount(threaded);
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    std::array<std::uint16_t, threads_k> owners {};
    std::array<std::size_t, threads_k> thread_failures {};
    st::basic_vector<std::thread> workers;
    for (std::size_t index = 0; index != threads_k; ++index)
        failures += expect(workers.push_back(std::thread {[&, index]() noexcept {
            expected<guard_t> guard = (*fixture)->lanes.checkout(index);
            thread_failures[index] += expect(guard.has_value());
            if (!guard) return;
            owners[index] = static_cast<std::uint16_t>(guard->lane().owner().id());
        }}) == st::status_t::success_k);
    for (std::size_t index = 0; index != workers.size(); ++index) workers[index].join();
    for (std::size_t index = 0; index != threads_k; ++index) failures += thread_failures[index];

    for (std::size_t first = 0; first != threads_k; ++first)
        for (std::size_t second = first + 1; second != threads_k; ++second)
            failures += expect(owners[first] != owners[second]);
    return failures;
}

/** Spare lanes run out, and the mount says so rather than handing one lease to two threads. */
template <typename mount_type_>
std::size_t lanes_refuse_beyond_their_spares(mount_type_ const& mount) {
    using types_t = lanes_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    std::size_t failures = 0;
    constexpr std::size_t spares_k = 2;
    fixture_shape_t spared = mount.shape;
    spared.spare_lanes = spares_k;
    expected<std::unique_ptr<fixture_t>> fixture = mount(spared);
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    st::basic_vector<guard_t> guards;
    for (std::size_t index = 0; index != spares_k; ++index) {
        expected<guard_t> guard = (*fixture)->lanes.checkout();
        failures += expect(guard.has_value());
        if (!guard) break;
        failures += expect(guards.push_back(std::move(*guard)) == st::status_t::success_k);
    }
    expected<guard_t> const beyond = (*fixture)->lanes.checkout();
    failures += expect(!beyond.has_value());
    failures += expect_status(beyond, status_code_t::capacity_exhausted_k);

    guards.clear();
    expected<guard_t> reclaimed = (*fixture)->lanes.checkout();
    failures += expect(reclaimed.has_value());
    return failures;
}

/**
 *  @brief Two strictly alternating threads each keep the spare they bound, and never share one.
 *
 *  Without affinity the second thread takes the first free spare, which the first thread just
 *  released, and on a ring that rebinds on a foreign adoption every checkout rebuilds that spare's
 *  ring; with it each thread finds its own spare free on every round after the first.
 */
template <typename mount_type_>
std::size_t spare_lane_stays_with_its_thread(mount_type_ const& mount) {
    using types_t = lanes_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    std::size_t failures = 0;
    constexpr std::size_t spares_k = 4;
    constexpr std::size_t rounds_k = 64;
    fixture_shape_t spared = mount.shape;
    spared.spare_lanes = spares_k;
    expected<std::unique_ptr<fixture_t>> fixture = mount(spared);
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    std::atomic<std::size_t> turn {0};
    std::array<std::array<owner_id_t, rounds_k>, 2> seen {};
    std::array<std::size_t, 2> thread_failures {};
    auto alternate = [&](std::size_t self) noexcept {
        for (std::size_t round = 0; round != rounds_k; ++round) {
            while (turn.load(std::memory_order_acquire) % 2 != self) std::this_thread::yield();
            {
                expected<guard_t> guard = (*fixture)->lanes.checkout();
                thread_failures[self] += expect(guard.has_value());
                if (guard) seen[self][round] = guard->lane().owner().id();
            }
            turn.fetch_add(1, std::memory_order_release);
        }
    };
    std::thread first {alternate, std::size_t {0}};
    std::thread second {alternate, std::size_t {1}};
    first.join();
    second.join();
    failures += thread_failures[0] + thread_failures[1];

    for (std::size_t round = 1; round != rounds_k; ++round) {
        failures += expect(seen[0][round] == seen[0][0]);
        failures += expect(seen[1][round] == seen[1][0]);
    }
    failures += expect(seen[0][0] != seen[1][0]);
    return failures;
}

#pragma endregion Lane Cases

#pragma region Pipeline Cases

/** A write pipeline puts bytes on the volume and a read pipeline brings the same bytes back. */
template <typename mount_type_>
std::size_t pipeline_reads_back_what_it_wrote(mount_type_ const& mount) {
    using types_t = lanes_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using read_pipeline_t = typename types_t::read_pipeline_t;
    using write_pipeline_t = typename types_t::write_pipeline_t;
    std::size_t failures = 0;
    constexpr std::uint64_t blocks_k = 64;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    target_place_t const target = (*fixture)->targets[0].place();

    write_pipeline_t writer {(*fixture)->disk, guard->lane(), target};
    for (std::uint64_t index = 0; index != blocks_k; ++index) {
        expected<buffer_lease_t> block = writer.stage_or_harvest();
        failures += expect(block.has_value());
        if (!block) return failures;
        paint(block->bytes(), index);
        failures += expect(
            writer.commit(std::move(*block), extent_t {index * block_bytes_k, block_bytes_k}).succeeded());
        failures += expect(writer.flush().succeeded());
    }
    failures += expect(writer.make_durable().succeeded());

    read_pipeline_t reader {(*fixture)->disk, guard->lane(), target};
    std::size_t seen = 0;
    std::size_t mismatches = 0;
    std::uint64_t offered = 0;
    auto sink = [&](harvested_block_t const& reaped) noexcept {
        ++seen;
        if (require_whole(reaped).failed()) {
            ++mismatches;
            return;
        }
        for (std::size_t index = 0; index != reaped.bytes.size(); ++index)
            if (reaped.bytes[index] != static_cast<byte_t>((index * 131u + reaped.tag * 17u + 7u) & 0xFFu)) {
                ++mismatches;
                return;
            }
    };
    while (offered != blocks_k || reader.outstanding() != 0) {
        while (offered != blocks_k && reader.has_room()) {
            if (reader.offer(extent_t {offered * block_bytes_k, block_bytes_k}, static_cast<std::uint32_t>(offered))
                    .failed())
                break;
            ++offered;
        }
        failures += expect(reader.flush().succeeded());
        expected<std::size_t> const harvested = reader.harvest(sink);
        failures += expect(harvested.has_value());
        if (!harvested) return failures;
    }
    failures += expect_eq(seen, static_cast<std::size_t>(blocks_k));
    failures += expect_eq(mismatches, std::size_t {0});
    return failures;
}

/**
 *  @brief The verdict this file exists for: a long run sits at the lease's depth, not at a batch's.
 *
 *  The old shape built every request, submitted once and waited, so the depth was the batch size.
 *  A pipeline refills as completions retire, so the observed in-flight count reaches the lease's
 *  depth and stays there through the middle of the run.
 */
template <typename mount_type_>
std::size_t pipeline_holds_its_depth(mount_type_ const& mount) {
    using types_t = lanes_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using read_pipeline_t = typename types_t::read_pipeline_t;
    std::size_t failures = 0;
    constexpr std::uint64_t blocks_k = 512;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    read_pipeline_t reader {(*fixture)->disk, guard->lane(), (*fixture)->targets[0].place()};
    std::uint64_t offered = 0;
    std::size_t harvested_total = 0;
    std::size_t deepest = 0;
    auto sink = [&](harvested_block_t const&) noexcept { ++harvested_total; };

    while (offered != blocks_k || reader.outstanding() != 0) {
        while (offered != blocks_k && reader.has_room()) {
            if (reader
                    .offer(extent_t {(offered % volume_blocks_k) * block_bytes_k, block_bytes_k},
                           static_cast<std::uint32_t>(offered))
                    .failed())
                break;
            ++offered;
        }
        failures += expect(reader.flush().succeeded());
        std::size_t const in_flight = (*fixture)->disk.in_flight(guard->lane().owner());
        if (in_flight > deepest) deepest = in_flight;
        expected<std::size_t> const seen = reader.harvest(sink);
        failures += expect(seen.has_value());
        if (!seen) return failures;
    }

    failures += expect_eq(harvested_total, static_cast<std::size_t>(blocks_k));
    failures += expect_eq(deepest, static_cast<std::size_t>(lanes_shape_k.lease_depth));
    return failures;
}

/** A write that runs past the end of a fixed medium is a refusal, never a silent short write. */
template <typename mount_type_>
verdict_t pipeline_refuses_a_short_write(mount_type_ const& mount) {
    using types_t = lanes_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using write_pipeline_t = typename types_t::write_pipeline_t;
    if (edge_of(mount.transport) == medium_edge_t::growing_k)
        return verdict_t::skipped("a regular file grows past its end");
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    write_pipeline_t writer {(*fixture)->disk, guard->lane(), (*fixture)->targets[0].place()};
    expected<buffer_lease_t> block = writer.stage();
    failures += expect(block.has_value());
    if (!block) return failures;
    paint(block->bytes(), 5);
    failures += expect(
        writer.commit(std::move(*block), extent_t {lanes_shape_k.volume_bytes, block_bytes_k}).succeeded());
    failures += expect(writer.flush().succeeded());
    failures += expect(writer.make_durable().failed());
    return failures;
}

/** A read that meets the end of the volume harvests a partial, with the byte count it moved. */
template <typename mount_type_>
std::size_t pipeline_reports_a_partial_read(mount_type_ const& mount) {
    using types_t = lanes_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using read_pipeline_t = typename types_t::read_pipeline_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->lanes.checkout();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    read_pipeline_t reader {(*fixture)->disk, guard->lane(), (*fixture)->targets[0].place()};
    constexpr std::uint64_t reachable_k = block_bytes_k;
    failures += expect(
        reader.offer(extent_t {lanes_shape_k.volume_bytes - reachable_k, 2 * block_bytes_k}, 0).succeeded());
    failures += expect(reader.flush().succeeded());

    std::size_t seen = 0;
    std::size_t partials = 0;
    std::uint64_t moved = 0;
    auto sink = [&](harvested_block_t const& reaped) noexcept {
        ++seen;
        if (reaped.state == completion_state_t::partial_k) ++partials;
        moved = reaped.bytes.size();
        failures += expect_status(require_whole(reaped), status_code_t::io_k);
    };
    failures += expect(reader.harvest_all(sink).succeeded());
    failures += expect_eq(seen, std::size_t {1});
    failures += expect_eq(partials, std::size_t {1});
    failures += expect_eq(moved, reachable_k);
    return failures;
}

#pragma endregion Pipeline Cases

} // namespace unum::ustore

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;

    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    ledger_t ledger = options.ledger();

    st::expected<test_directory_t> const directory = test_directory_t::make(options.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    medium_t const medium = medium_t::of(options, *directory);

    auto const cases = [&](auto const& mount) {
        report(ledger, mount.label("slab_charges_once").c_str(), slab_charges_its_budget_once(mount));
        report(ledger, mount.label("pool_exhausts_and_recycles").c_str(), pool_exhausts_and_recycles(mount));
        report(ledger, mount.label("pool_recycles_exactly_once").c_str(), lease_recycles_exactly_once(mount));
        report(ledger, mount.label("separate_threads").c_str(), lanes_separate_executor_threads(mount));
        report(ledger, mount.label("refuse_beyond_spares").c_str(), lanes_refuse_beyond_their_spares(mount));
        report(ledger, mount.label("spare_stays_with_thread").c_str(), spare_lane_stays_with_its_thread(mount));
        report(ledger, mount.label("pipeline_reads_back").c_str(), pipeline_reads_back_what_it_wrote(mount));
        report(ledger, mount.label("pipeline_holds_its_depth").c_str(), pipeline_holds_its_depth(mount));
        report(ledger, mount.label("pipeline_refuses_short_write").c_str(), pipeline_refuses_a_short_write(mount));
        report(ledger, mount.label("pipeline_partial_read").c_str(), pipeline_reports_a_partial_read(mount));
    };
    for (transport_t const transport : every_transport_k)
        on_transport(ledger, transport, medium, lanes_shape_k, 1, "lanes", cases);

    return conclude(ledger);
}
