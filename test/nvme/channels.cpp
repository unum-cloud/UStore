/**
 *  @file test/nvme/channels.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The registered arena, the blocks channels queue pair out of it, and the pipelines that fill a ring.
 *
 *  The result this file exists for is @c holds_its_depth: a pipeline must keep the ring at
 *  the queue pair's depth through the middle of a long run, since a batch that is submitted
 *  whole and waited on reaches the depth of that batch and no further, which is the defect
 *  the pipelines replace.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <array>   // `std::array`
#include <memory>  // `std::unique_ptr`
#include <span>    // `std::span`
#include <thread>  // `std::thread`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/nvme_channels.hpp" // `buffer_pool_t`, `pooled_buffer_t`

#include "nvme/fixture.hpp" // `run_on_transport`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

#pragma region Shape

/** Bytes of one block every case reads and writes. */
constexpr std::uint64_t block_bytes_k = 4096;

/** Blocks the volume of every case holds, which is far more than one channel's pool. */
constexpr std::uint64_t volume_blocks_k = 4096;

/** The numbers every case mounts with: blocks of two pages so a read can run past the end of the volume by one. */
constexpr fixture_config_t channels_config_k {
    .request_buffer_bytes = 2 * block_bytes_k,
    .stream_buffer_bytes = 4 * block_bytes_k,
    .stream_buffers = 2,
    .volume_bytes = block_bytes_k * volume_blocks_k,
    .shared_io_channels = 1,
    .executor_threads = 1,
    .memory_limit = std::size_t {64} * 1024 * 1024,
};

/** The disk, the channels and the pipelines every case drives over @p fixture_type_. */
template <typename fixture_type_>
struct channels_types {
    using fixture_t = fixture_type_;
    using guard_t = typename fixture_t::guard_t;
    using disk_t = typename fixture_t::disk_t;
    using read_pipeline_t = basic_read_pipeline<disk_t>;
    using write_pipeline_t = basic_write_pipeline<disk_t, waiter_t>;
};

/** Writes a repeatable pattern of @p seed into @p buffer, so a read can be checked against it. */
void paint(std::span<byte_t> buffer, std::uint64_t seed) noexcept {
    for (std::size_t index = 0; index != buffer.size(); ++index)
        buffer[index] = static_cast<byte_t>((index * 131u + seed * 17u + 7u) & 0xFFu);
}

#pragma endregion Shape

#pragma region Arena and Pool Cases

/** An arena charges its budget once and returns every byte when it dies. */
template <typename mount_type_>
std::size_t arena_charges_its_budget_once(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    resources_shared_ptr_t const resources = (*fixture)->resources;
    std::size_t const charged = resources->memory_constraint.used();
    failures += expect(charged != 0);
    failures += expect_eq(charged, static_cast<std::size_t>((*fixture)->arena.size()));

    // Nothing below the arena is charged again, so its death returns every byte the mount paid for.
    (*fixture).reset();
    failures += expect_eq(resources->memory_constraint.used(), std::size_t {0});
    return failures;
}

/** Every block of a pool is handed out once, and the pool refuses once they are all out. */
template <typename mount_type_>
std::size_t pool_exhausts_and_recycles(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    buffer_pool_t& pool = guard->channel().requests();
    failures += expect_eq(pool.available_buffers(), channels_config_k.request_buffers);

    st::vector<pooled_buffer_t> held;
    for (std::uint32_t index = 0; index != channels_config_k.request_buffers; ++index) {
        expected<pooled_buffer_t> block = pool.acquire();
        failures += expect(block.has_value());
        if (!block) break;
        failures += expect(held.push_back(std::move(*block)) == st::status_t::success_k);
    }
    failures += expect_eq(pool.available_buffers(), std::uint32_t {0});
    expected<pooled_buffer_t> const beyond = pool.acquire();
    failures += expect(!beyond.has_value());
    failures += expect_status(beyond, status_code_t::capacity_exhausted_k);

    // Every block resolves to its own place, so two queue pairs never name the same bytes.
    for (std::size_t first = 0; first + 1 < held.size(); ++first)
        failures += expect(held[first].reference().offset != held[first + 1].reference().offset);

    held.clear();
    failures += expect_eq(pool.available_buffers(), channels_config_k.request_buffers);
    return failures;
}

/** A moved-from queue pair recycles nothing, and a destroyed one recycles exactly once. */
template <typename mount_type_>
std::size_t pool_recycles_exactly_once(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    buffer_pool_t& pool = guard->channel().requests();
    std::uint32_t const before = pool.available_buffers();

    {
        expected<pooled_buffer_t> first = pool.acquire();
        failures += expect(first.has_value());
        if (!first) return failures;
        failures += expect_eq(pool.available_buffers(), before - 1);

        pooled_buffer_t moved = std::move(*first);
        failures += expect(moved.held());
        failures += expect(!first->held());
        failures += expect_eq(pool.available_buffers(), before - 1);

        moved.release();
        failures += expect_eq(pool.available_buffers(), before);
        moved.release();
        failures += expect_eq(pool.available_buffers(), before);
    }
    failures += expect_eq(pool.available_buffers(), before);
    return failures;
}

#pragma endregion Arena and Pool Cases

#pragma region Channel Cases

/** Every executor thread gets a channel of its own, and no two threads get the same queue pair. */
template <typename mount_type_>
std::size_t channels_separate_executor_threads(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    std::size_t failures = 0;
    constexpr std::size_t threads_k = 4;
    fixture_config_t threaded = mount.config;
    threaded.executor_threads = threads_k;
    expected<std::unique_ptr<fixture_t>> fixture = mount(threaded);
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    std::array<std::uint16_t, threads_k> queue_pairs {};
    std::array<std::size_t, threads_k> thread_failures {};
    st::vector<std::thread> workers;
    for (std::size_t index = 0; index != threads_k; ++index)
        failures += expect(workers.push_back(std::thread {[&, index]() noexcept {
            expected<guard_t> guard = (*fixture)->channels.acquire(index);
            thread_failures[index] += expect(guard.has_value());
            if (!guard) return;
            queue_pairs[index] = static_cast<std::uint16_t>(guard->channel().queue_pair().id());
        }}) == st::status_t::success_k);
    for (std::size_t index = 0; index != workers.size(); ++index) workers[index].join();
    for (std::size_t index = 0; index != threads_k; ++index) failures += thread_failures[index];

    for (std::size_t first = 0; first != threads_k; ++first)
        for (std::size_t second = first + 1; second != threads_k; ++second)
            failures += expect(queue_pairs[first] != queue_pairs[second]);
    return failures;
}

/** Spare channels run out, and the mount says so rather than handing one queue pair to two threads. */
template <typename mount_type_>
std::size_t channels_refuse_beyond_their_spares(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    std::size_t failures = 0;
    constexpr std::size_t spares_k = 2;
    fixture_config_t spared = mount.config;
    spared.shared_io_channels = spares_k;
    expected<std::unique_ptr<fixture_t>> fixture = mount(spared);
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    st::vector<guard_t> guards;
    for (std::size_t index = 0; index != spares_k; ++index) {
        expected<guard_t> guard = (*fixture)->channels.acquire();
        failures += expect(guard.has_value());
        if (!guard) break;
        failures += expect(guards.push_back(std::move(*guard)) == st::status_t::success_k);
    }
    expected<guard_t> const beyond = (*fixture)->channels.acquire();
    failures += expect(!beyond.has_value());
    failures += expect_status(beyond, status_code_t::capacity_exhausted_k);

    guards.clear();
    expected<guard_t> reclaimed = (*fixture)->channels.acquire();
    failures += expect(reclaimed.has_value());
    return failures;
}

#pragma endregion Channel Cases

#pragma region Pipeline Cases

/** A write pipeline puts bytes on the volume and a read pipeline brings the same bytes back. */
template <typename mount_type_>
std::size_t pipeline_reads_back_what_it_wrote(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using read_pipeline_t = typename types_t::read_pipeline_t;
    using write_pipeline_t = typename types_t::write_pipeline_t;
    std::size_t failures = 0;
    constexpr std::uint64_t blocks_k = 64;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;
    medium_reference_t const medium = (*fixture)->media[0].reference();

    write_pipeline_t writer {(*fixture)->disk, guard->channel(), medium};
    for (std::uint64_t index = 0; index != blocks_k; ++index) {
        expected<pooled_buffer_t> block = writer.acquire_buffer_or_poll();
        failures += expect(block.has_value());
        if (!block) return failures;
        paint(block->bytes(), index);
        failures += expect(
            writer.enqueue_write(std::move(*block), extent_t {index * block_bytes_k, block_bytes_k}).succeeded());
        failures += expect(writer.submit().succeeded());
    }
    failures += expect(writer.make_durable().succeeded());

    read_pipeline_t reader {(*fixture)->disk, guard->channel(), medium};
    std::size_t seen = 0;
    std::size_t mismatches = 0;
    std::uint64_t offered = 0;
    auto sink = [&](completed_buffer_t const& completed) noexcept {
        ++seen;
        if (require_completed_in_full(completed).failed()) {
            ++mismatches;
            return;
        }
        for (std::size_t index = 0; index != completed.bytes.size(); ++index)
            if (completed.bytes[index] != static_cast<byte_t>((index * 131u + completed.tag * 17u + 7u) & 0xFFu)) {
                ++mismatches;
                return;
            }
    };
    while (offered != blocks_k || reader.outstanding() != 0) {
        while (offered != blocks_k && reader.can_enqueue()) {
            if (reader
                    .enqueue_read(extent_t {offered * block_bytes_k, block_bytes_k},
                                  static_cast<std::uint32_t>(offered))
                    .failed())
                break;
            ++offered;
        }
        failures += expect(reader.submit().succeeded());
        expected<std::size_t> const polled = reader.poll_completions(sink);
        failures += expect(polled.has_value());
        if (!polled) return failures;
    }
    failures += expect_eq(seen, static_cast<std::size_t>(blocks_k));
    failures += expect_eq(mismatches, std::size_t {0});
    return failures;
}

/**
 *  @brief The result this file exists for: a long run sits at the queue pair's depth, not at a batch's.
 *
 *  The old shape built every request, submitted once and waited, so the depth was the batch size. A
 *  pipeline refills as completions retire, so the observed in-flight count reaches the queue pair's
 *  depth and stays there through the middle of the run.
 */
template <typename mount_type_>
std::size_t pipeline_holds_its_depth(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using read_pipeline_t = typename types_t::read_pipeline_t;
    std::size_t failures = 0;
    constexpr std::uint64_t blocks_k = 512;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    read_pipeline_t reader {(*fixture)->disk, guard->channel(), (*fixture)->media[0].reference()};
    std::uint64_t offered = 0;
    std::size_t polled_total = 0;
    std::size_t deepest = 0;
    auto sink = [&](completed_buffer_t const&) noexcept { ++polled_total; };

    while (offered != blocks_k || reader.outstanding() != 0) {
        while (offered != blocks_k && reader.can_enqueue()) {
            if (reader
                    .enqueue_read(extent_t {(offered % volume_blocks_k) * block_bytes_k, block_bytes_k},
                                  static_cast<std::uint32_t>(offered))
                    .failed())
                break;
            ++offered;
        }
        failures += expect(reader.submit().succeeded());
        std::size_t const in_flight = (*fixture)->disk.outstanding(guard->channel().queue_pair());
        if (in_flight > deepest) deepest = in_flight;
        expected<std::size_t> const seen = reader.poll_completions(sink);
        failures += expect(seen.has_value());
        if (!seen) return failures;
    }

    failures += expect_eq(polled_total, static_cast<std::size_t>(blocks_k));
    failures += expect_eq(deepest, static_cast<std::size_t>(channels_config_k.queue_depth));
    return failures;
}

/** A write that runs past the end of a fixed medium is a refusal, never a silent short write. */
template <typename mount_type_>
case_result_t pipeline_refuses_a_short_write(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using write_pipeline_t = typename types_t::write_pipeline_t;
    if (capacity_behavior_of(mount.transport) == capacity_behavior_t::growing_k)
        return case_result_t::skipped("a regular file grows past its end");
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    write_pipeline_t writer {(*fixture)->disk, guard->channel(), (*fixture)->media[0].reference()};
    expected<pooled_buffer_t> block = writer.acquire_buffer();
    failures += expect(block.has_value());
    if (!block) return failures;
    paint(block->bytes(), 5);
    failures += expect(
        writer.enqueue_write(std::move(*block), extent_t {channels_config_k.volume_bytes, block_bytes_k}).succeeded());
    failures += expect(writer.submit().succeeded());
    failures += expect(writer.make_durable().failed());
    return failures;
}

/** A read that meets the end of the volume polls a partial, with the byte count it moved. */
template <typename mount_type_>
std::size_t pipeline_reports_a_partial_read(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using guard_t = typename types_t::guard_t;
    using read_pipeline_t = typename types_t::read_pipeline_t;
    std::size_t failures = 0;
    expected<std::unique_ptr<fixture_t>> fixture = mount();
    failures += expect_status(fixture, status_code_t::success_k);
    if (!fixture) return failures;

    expected<guard_t> guard = (*fixture)->channels.acquire();
    failures += expect(guard.has_value());
    if (!guard) return failures;

    read_pipeline_t reader {(*fixture)->disk, guard->channel(), (*fixture)->media[0].reference()};
    constexpr std::uint64_t reachable_k = block_bytes_k;
    failures += expect(
        reader.enqueue_read(extent_t {channels_config_k.volume_bytes - reachable_k, 2 * block_bytes_k}, 0).succeeded());
    failures += expect(reader.submit().succeeded());

    std::size_t seen = 0;
    std::size_t partials = 0;
    std::uint64_t moved = 0;
    auto sink = [&](completed_buffer_t const& completed) noexcept {
        ++seen;
        if (completed.state == completion_state_t::partial_k) ++partials;
        moved = completed.bytes.size();
        failures += expect_status(require_completed_in_full(completed), status_code_t::io_k);
    };
    failures += expect(reader.poll_all_completions(sink).succeeded());
    failures += expect_eq(seen, std::size_t {1});
    failures += expect_eq(partials, std::size_t {1});
    failures += expect_eq(moved, reachable_k);
    return failures;
}

#pragma endregion Pipeline Cases

} // namespace unum::ustore::test

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;

    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    st::expected<directory_t> const directory = directory_t::make(options.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    test_environment_t const environment = test_environment_t::of(options, *directory);

    auto const cases = [&](auto const& mount) {
        report(totals, mount, "arena_charges_once", arena_charges_its_budget_once);
        report(totals, mount, "pool_exhausts_and_recycles", pool_exhausts_and_recycles);
        report(totals, mount, "pool_recycles_exactly_once", pool_recycles_exactly_once);
        report(totals, mount, "separate_threads", channels_separate_executor_threads);
        report(totals, mount, "refuse_beyond_spares", channels_refuse_beyond_their_spares);
        report(totals, mount, "pipeline_reads_back", pipeline_reads_back_what_it_wrote);
        report(totals, mount, "pipeline_holds_its_depth", pipeline_holds_its_depth);
        report(totals, mount, "pipeline_refuses_short_write", pipeline_refuses_a_short_write);
        report(totals, mount, "pipeline_partial_read", pipeline_reports_a_partial_read);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(totals, transport, environment, channels_config_k, 1, "channels", cases);

    return suite_exit_code(totals);
}
