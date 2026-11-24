/**
 *  @file test/nvme/channels.cpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The queue set's blocks, the channels holding a thread's queues, and the pipelines that
 *      fill a ring.
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
#include "ustore/utilities/nvme_queues.hpp"   // `blocking_queues_config_k`

#include "nvme/fixture.hpp" // `run_on_transport`

namespace unum::ustore::test {

using namespace unum::ustore::test::nvme;

using namespace unum::ustore::blobs;

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
    .queues_threads = 1,
    .memory_limit = std::size_t {64} * 1024 * 1024,
};

/** The disk, the channels and the pipelines every case drives over @p fixture_type_. */
template <typename fixture_type_>
struct channels_types {
    using fixture_t = fixture_type_;
    using disk_t = typename fixture_t::disk_t;
    using read_pipeline_t = util::basic_read_pipeline<disk_t>;
    using write_pipeline_t = util::basic_write_pipeline<disk_t, waiter_t>;
};

/** Writes a repeatable pattern of @p seed into @p buffer, so a read can be checked against it. */
void paint(std::span<util::byte_t> buffer, std::uint64_t seed) noexcept {
    for (std::size_t index = 0; index != buffer.size(); ++index)
        buffer[index] = static_cast<util::byte_t>((index * 131u + seed * 17u + 7u) & 0xFFu);
}

#pragma endregion Shape

#pragma region Queue Set and Pool Cases

/** A queue set's blocks are charged to no database budget, which holds only the disk's own pools,
 *  and the mount returns every byte when it dies. */
template <typename mount_type_>
void test_queues_blocks_charge_no_budget(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    resources_shared_ptr_t const resources = fixture->resources;
    std::size_t const charged = resources->memory_constraint.used();
    expect_true(charged < static_cast<std::size_t>(mount.config.channels().arena_bytes()));

    fixture.reset();
    expect_equal(resources->memory_constraint.used(), std::size_t {0});
}

/** Every block of a pool is handed out once, and the pool refuses once they are all out. */
template <typename mount_type_>
void test_pool_exhausts_and_recycles(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    util::buffer_pool_t& pool = guard->requests();
    expect_equal(pool.available_buffers(), channels_config_k.request_buffers);

    st::vector<util::pooled_buffer_t> held;
    for (std::uint32_t index = 0; index != channels_config_k.request_buffers; ++index) {
        auto [block, acquire_block_status] = pool.acquire();
        expect_true(acquire_block_status.succeeded());
        if (acquire_block_status.failed()) break;
        expect_true(held.push_back(std::move(block)) == st::status_t::success_k);
    }
    expect_equal(pool.available_buffers(), std::uint32_t {0});
    auto [beyond, beyond_status] = pool.acquire();
    expect_true(beyond_status.failed());
    expect_status(beyond_status, util::status_code_t::capacity_exhausted_k);

    // Every block resolves to its own place, so two queue pairs never name the same bytes.
    for (std::size_t first = 0; first + 1 < held.size(); ++first)
        expect_true(held[first].reference().offset != held[first + 1].reference().offset);

    held.clear();
    expect_equal(pool.available_buffers(), channels_config_k.request_buffers);
}

/** A moved-from queue pair recycles nothing, and a destroyed one recycles exactly once. */
template <typename mount_type_>
void test_pool_recycles_exactly_once(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    util::buffer_pool_t& pool = guard->requests();
    std::uint32_t const before = pool.available_buffers();

    {
        auto [first, first_status] = pool.acquire();
        expect_true(first_status.succeeded());
        if (first_status.failed()) return;
        expect_equal(pool.available_buffers(), before - 1);

        util::pooled_buffer_t moved = std::move(first);
        expect_true(moved.held());
        expect_true(!first.held());
        expect_equal(pool.available_buffers(), before - 1);

        moved.release();
        expect_equal(pool.available_buffers(), before);
        moved.release();
        expect_equal(pool.available_buffers(), before);
    }
    expect_equal(pool.available_buffers(), before);
}

#pragma endregion Queue Set and Pool Cases

#pragma region Channel Cases

/** Every thread registers the disk's media on a channel of its own, and no two threads get the same
 *  queue pair. */
template <typename mount_type_>
void test_channels_separate_queues_threads(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    constexpr std::size_t threads_k = 4;
    fixture_config_t threaded = mount.config;
    threaded.queues_threads = threads_k;
    auto [fixture, mount_status] = mount(threaded);
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    std::array<std::uint16_t, threads_k> queue_pairs {};
    st::vector<std::thread> workers;
    for (std::size_t index = 0; index != threads_k; ++index)
        expect_true(workers.push_back(std::thread {[&, index]() noexcept {
            auto [channel, channel_status] = fixture->queues.reading_channel_with_disk_media(index, fixture->disk);
            if (!expect_true(channel_status.succeeded())) return;
            queue_pairs[index] = static_cast<std::uint16_t>(channel->queue_pair().id());
        }}) == st::status_t::success_k);
    for (std::size_t index = 0; index != workers.size(); ++index) workers[index].join();

    for (std::size_t first = 0; first != threads_k; ++first)
        for (std::size_t second = first + 1; second != threads_k; ++second)
            expect_true(queue_pairs[first] != queue_pairs[second]);
}

#pragma endregion Channel Cases

#pragma region Pipeline Cases

/** A write pipeline puts bytes on the volume and a read pipeline brings the same bytes back. */
template <typename mount_type_>
void test_pipeline_reads_back_what_it_wrote(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using read_pipeline_t = typename types_t::read_pipeline_t;
    using write_pipeline_t = typename types_t::write_pipeline_t;
    constexpr std::uint64_t blocks_k = 64;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;
    util::medium_reference_t const medium = fixture->media[0].reference();

    {
        write_pipeline_t writer {fixture->disk, *guard, medium};
        for (std::uint64_t index = 0; index != blocks_k; ++index) {
            auto [block, acquire_block_status] = writer.acquire_buffer_or_poll();
            expect_true(acquire_block_status.succeeded());
            if (acquire_block_status.failed()) return;
            paint(block.bytes(), index);
            expect_true(writer.enqueue_write(std::move(block), util::extent_t {index * block_bytes_k, block_bytes_k})
                            .succeeded());
            expect_true(writer.submit().succeeded());
        }
        expect_true(writer.make_durable().succeeded());
    }

    read_pipeline_t reader {fixture->disk, *guard, medium};
    std::size_t seen = 0;
    std::size_t mismatches = 0;
    std::uint64_t offered = 0;
    auto sink = [&](util::completed_buffer_t const& completed) noexcept {
        ++seen;
        if (util::require_completed_in_full(completed).failed()) {
            ++mismatches;
            return;
        }
        for (std::size_t index = 0; index != completed.bytes.size(); ++index)
            if (completed.bytes[index] !=
                static_cast<util::byte_t>((index * 131u + completed.tag * 17u + 7u) & 0xFFu)) {
                ++mismatches;
                return;
            }
    };
    while (offered != blocks_k || reader.outstanding() != 0) {
        while (offered != blocks_k && reader.can_enqueue()) {
            if (reader
                    .enqueue_read(util::extent_t {offered * block_bytes_k, block_bytes_k},
                                  static_cast<std::uint32_t>(offered))
                    .failed())
                break;
            ++offered;
        }
        expect_true(reader.submit().succeeded());
        auto const [polled, poll_status] = reader.poll_completions(sink);
        expect_true(poll_status.succeeded());
        if (poll_status.failed()) return;
    }
    expect_equal(seen, static_cast<std::size_t>(blocks_k));
    expect_equal(mismatches, std::size_t {0});
}

/** A write pipeline staging more blocks than its channel holds, as a run write does, submits
 *  what it staged to take one back, so every write lands on the smallest queue set. */
template <typename mount_type_>
void test_pipeline_stages_past_its_blocks(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using write_pipeline_t = typename types_t::write_pipeline_t;
    fixture_config_t smallest = mount.config;
    smallest.request_buffer_bytes = util::blocking_queues_config_k.request_buffer_bytes;
    smallest.request_buffers = util::blocking_queues_config_k.request_buffers_count;
    smallest.stream_buffer_bytes = util::blocking_queues_config_k.stream_buffer_bytes;
    smallest.stream_buffers = util::blocking_queues_config_k.stream_buffers_count;
    smallest.queue_depth = static_cast<std::uint32_t>(util::blocking_queues_config_k.depth);
    auto [fixture, mount_status] = mount(smallest);
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;

    std::uint64_t const block_bytes = smallest.request_buffer_bytes;
    std::uint64_t const blocks = 2 * std::uint64_t {smallest.request_buffers} + 1;
    write_pipeline_t writer {fixture->disk, *guard, fixture->media[0].reference()};
    for (std::uint64_t index = 0; index != blocks; ++index) {
        auto [block, acquire_block_status] = writer.acquire_buffer_or_poll();
        if (!expect_status(acquire_block_status, util::status_code_t::success_k)) return;
        paint(block.bytes(), index);
        expect_true(
            writer.enqueue_write(std::move(block), util::extent_t {index * block_bytes, block_bytes}).succeeded());
    }
    expect_true(writer.make_durable().succeeded());
}

/**
 *  @brief The result this file exists for: a long run sits at the queue pair's depth, not at a batch's.
 *
 *  The old shape built every request, submitted once and waited, so the depth was the batch size. A
 *  pipeline refills as completions retire, so the observed in-flight count reaches the queue pair's
 *  depth and stays there through the middle of the run.
 */
template <typename mount_type_>
void test_pipeline_holds_its_depth(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using read_pipeline_t = typename types_t::read_pipeline_t;
    constexpr std::uint64_t blocks_k = 512;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;

    read_pipeline_t reader {fixture->disk, *guard, fixture->media[0].reference()};
    std::uint64_t offered = 0;
    std::size_t polled_total = 0;
    std::size_t deepest = 0;
    auto sink = [&](util::completed_buffer_t const&) noexcept { ++polled_total; };

    while (offered != blocks_k || reader.outstanding() != 0) {
        while (offered != blocks_k && reader.can_enqueue()) {
            if (reader
                    .enqueue_read(util::extent_t {(offered % volume_blocks_k) * block_bytes_k, block_bytes_k},
                                  static_cast<std::uint32_t>(offered))
                    .failed())
                break;
            ++offered;
        }
        expect_true(reader.submit().succeeded());
        std::size_t const in_flight = fixture->disk.outstanding(guard->queue_pair());
        if (in_flight > deepest) deepest = in_flight;
        auto const [seen, poll_status] = reader.poll_completions(sink);
        expect_true(poll_status.succeeded());
        if (poll_status.failed()) return;
    }

    expect_equal(polled_total, static_cast<std::size_t>(blocks_k));
    expect_equal(deepest, static_cast<std::size_t>(channels_config_k.queue_depth));
}

/** A write that runs past the end of a fixed medium is a refusal, never a silent short write. */
template <typename mount_type_>
case_result_t test_pipeline_refuses_a_short_write(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using write_pipeline_t = typename types_t::write_pipeline_t;
    if (capacity_behavior_of(mount.transport) == capacity_behavior_t::growing_k)
        return case_result_t::skipped("needs a medium of fixed size, not a regular file");
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return {};

    auto [guard, acquire_status] = fixture->channel(0);
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return {};

    write_pipeline_t writer {fixture->disk, *guard, fixture->media[0].reference()};
    auto [block, acquire_block_status] = writer.acquire_buffer();
    expect_true(acquire_block_status.succeeded());
    if (acquire_block_status.failed()) return {};
    paint(block.bytes(), 5);
    expect_true(writer.enqueue_write(std::move(block), util::extent_t {channels_config_k.volume_bytes, block_bytes_k})
                    .succeeded());
    expect_true(writer.submit().succeeded());
    expect_true(writer.make_durable().failed());
    return {};
}

/** A read that meets the end of the volume polls a partial, with the byte count it moved. */
template <typename mount_type_>
void test_pipeline_reports_a_partial_read(mount_type_ const& mount) {
    using types_t = channels_types<typename mount_type_::fixture_t>;
    using fixture_t = typename types_t::fixture_t;
    using read_pipeline_t = typename types_t::read_pipeline_t;
    auto [fixture, mount_status] = mount();
    expect_status(mount_status, util::status_code_t::success_k);
    if (mount_status.failed()) return;

    auto [guard, acquire_status] = fixture->channel(0);
    expect_true(acquire_status.succeeded());
    if (acquire_status.failed()) return;

    read_pipeline_t reader {fixture->disk, *guard, fixture->media[0].reference()};
    constexpr std::uint64_t reachable_k = block_bytes_k;
    expect_true(reader.enqueue_read(util::extent_t {channels_config_k.volume_bytes - reachable_k, 2 * block_bytes_k}, 0)
                    .succeeded());
    expect_true(reader.submit().succeeded());

    std::size_t seen = 0;
    std::size_t partials = 0;
    std::uint64_t moved = 0;
    auto sink = [&](util::completed_buffer_t const& completed) noexcept {
        ++seen;
        if (completed.state == util::completion_state_t::partial_k) ++partials;
        moved = completed.bytes.size();
        expect_status(util::require_completed_in_full(completed), util::status_code_t::io_k);
    };
    expect_true(reader.poll_all_completions(sink).succeeded());
    expect_equal(seen, std::size_t {1});
    expect_equal(partials, std::size_t {1});
    expect_equal(moved, reachable_k);
}

#pragma endregion Pipeline Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::test;

    start_test_run(argc, argv);

    auto [directory, directory_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(directory_status)) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }

    auto const cases = [&](auto const& mount) {
        report_on_mount(mount, "test_queues_blocks_charge_no_budget", test_queues_blocks_charge_no_budget);
        report_on_mount(mount, "test_pool_exhausts_and_recycles", test_pool_exhausts_and_recycles);
        report_on_mount(mount, "test_pool_recycles_exactly_once", test_pool_recycles_exactly_once);
        report_on_mount(mount, "test_channels_separate_queues_threads", test_channels_separate_queues_threads);
        report_on_mount(mount, "test_pipeline_reads_back_what_it_wrote", test_pipeline_reads_back_what_it_wrote);
        report_on_mount(mount, "test_pipeline_stages_past_its_blocks", test_pipeline_stages_past_its_blocks);
        report_on_mount(mount, "test_pipeline_holds_its_depth", test_pipeline_holds_its_depth);
        report_on_mount(mount, "test_pipeline_refuses_a_short_write", test_pipeline_refuses_a_short_write);
        report_on_mount(mount, "test_pipeline_reports_a_partial_read", test_pipeline_reports_a_partial_read);
    };
    for (transport_t const transport : every_transport_k)
        run_on_transport(transport, directory.path(), channels_config_k, 1, "channels", cases);

    return finish_test_run();
}
