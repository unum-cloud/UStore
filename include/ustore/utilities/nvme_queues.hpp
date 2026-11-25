/**
 *  @file include/ustore/utilities/nvme_queues.hpp
 *  @author Ash Vardanian
 *  @date November 18, 2026
 *  @brief The I/O half of an NVMe executor: one reading and one writing queue per thread, over
 *      that thread's own staging, with the media of every database its calls touch registered.
 *
 *  A database owns its media, its cache and its reader epochs and never remembers a queue set, so
 *  every queue set is equal: a call names its queue set, and each thread of the call registers the
 *  database's disk media on its own queue before it submits. Registration installs the media only
 *  when the queue last registered another disk or an older medium epoch, which is no system call at
 *  all in steady state. An idle queue keeps its last disk's files referenced until its next
 *  registration, bounded by threads, two queues each and the disk's media limit.
 *
 *  The blocks are the queue set's, sized by the executor's @c ustore_nvme_options_t, placed on each
 *  thread's memory domain and charged to no database's memory limit; an SPDK queue set takes them
 *  as DMA memory. Nothing here builds a queue set for a call: @c src/ builds one set of blocking
 *  queues for a call naming no NVMe executor and frees it when the call returns.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`

#include <span>    // `std::span`
#include <utility> // `std::move`
#include <variant> // `std::variant`, `std::visit`, `std::get_if`, `std::monostate`, `std::in_place_type`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/nvme_channels.hpp"         // `io_channel_t`, `io_channel_config_t`, `buffer_pool_t`
#include "ustore/utilities/nvme_disks.hpp"            // `blocking_queue_t`
#include "ustore/utilities/nvme_linux_aio_disk.hpp"   // `linux_aio_queue_t`, `linux_aio_probe_t`
#include "ustore/utilities/nvme_linux_spdk_disk.hpp"  // `linux_spdk_queue_t`, `linux_spdk_dma_allocator`
#include "ustore/utilities/nvme_linux_uring_disk.hpp" // `linux_uring_queue_t`, `linux_uring_probe_t`
#include "ustore/utilities/nvme_ring.hpp"             // `queue_ref_t`, `queue_id_t`, `a_disk`
#include "ustore/utilities/ram_buffers.hpp"           // `allocate_domain_block`, `domain_block_t`
#include "ustore/utilities/types.hpp"                 // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::utilities {

#pragma region Transport

/**
 *  @brief The transport a queue set and a mount move bytes through, probed rather than assumed.
 *
 *  The choice is made once, when the executor or the database comes up, and the transport actually
 *  got is reported back rather than inferred, since @c automatic_k resolves differently on a kernel
 *  without io_uring, inside a seccomp profile that refuses it, and on a machine that has both.
 */
enum class io_transport_t : std::uint8_t {

    /** io_uring where the kernel offers it, Linux AIO where it offers only that, and the
     *  blocking floor elsewhere. */
    automatic_k,

    /** Positioned reads and writes on the polling thread, which every platform has. */
    blocking_k,

    /** io_uring on unbuffered files, refused without a kernel ring; blocks the memlock cap
     *  refuses go unpinned. */
    linux_uring_k,

    /** Linux AIO on unbuffered files, which Docker's seccomp profile and kernels below 6.12 keep
     *  without a ring. */
    linux_aio_k,

    /** SPDK's user-space driver over the namespaces of one controller, which @c spdk:// data
     *  directories name and a build without SPDK refuses. */
    linux_spdk_k,

    /** Host memory, which a suite and a benchmark ask for and a database never does. */
    memory_k,
};

/**
 *  @brief The transport @p wanted resolves to, given what the two probes answered.
 *  @param[in] wanted The transport the caller named, or @c io_transport_t::automatic_k.
 *  @param[in] linux_uring What the io_uring probe answered.
 *  @param[in] linux_aio What the Linux AIO probe answered.
 *  @return The transport, or @c status_code_t::unsupported_k for io_uring named on a kernel that
 *      creates no ring.
 *
 *  The order is io_uring, then Linux AIO, then the blocking floor. A transport named outright
 *  starts at its own place in that order and only falls further down it, so asking for AIO never
 *  mounts a ring, but io_uring named outright never falls at all.
 */
inline expected<io_transport_t> probed_transport(io_transport_t wanted, linux_uring_probe_t linux_uring,
                                                 linux_aio_probe_t linux_aio) noexcept {
    bool const takes_linux_uring = wanted == io_transport_t::automatic_k || wanted == io_transport_t::linux_uring_k;
    if (takes_linux_uring && linux_uring == linux_uring_probe_t::available_k) return io_transport_t::linux_uring_k;
    if (wanted == io_transport_t::linux_uring_k)
        return status_t {status_code_t::unsupported_k, linux_uring == linux_uring_probe_t::too_old_k
                                                           ? "The io_uring transport needs Linux 6.12 or newer"
                                                           : "The kernel refuses io_uring rings"};
    if (!takes_linux_uring && wanted != io_transport_t::linux_aio_k) return wanted;
    return linux_aio == linux_aio_probe_t::available_k ? io_transport_t::linux_aio_k : io_transport_t::blocking_k;
}

#pragma endregion Transport

#pragma region Queue Sets

/** The shape of the queues @c src/ builds for one call naming no NVMe executor, which runs its
 *  requests one at a time on the calling thread: a merge needs one streaming and one request block,
 *  and the other streaming blocks give its windows room before they fall back to the heap. */
inline constexpr io_channel_config_t blocking_queues_config_k {.request_buffer_bytes = 64ull * 1024ull,
                                                               .request_buffers_count = 4,
                                                               .stream_buffer_bytes = 256ull * 1024ull,
                                                               .stream_buffers_count = 3,
                                                               .depth = 4};

/** Both channels of one thread, for a merge slice that reads and writes at once. */
struct thread_channels_t {
    io_channel_t* reading = nullptr;
    io_channel_t* writing = nullptr;
};

/**
 *  @brief One reading and one writing queue per thread of an executor, each over the
 *      thread's own blocks.
 *
 *  Only thread @c t of the executor submits and polls on thread @c t 's queues, which the thread
 *  index every fan-out passes guarantees, so nothing here is claimed or contended. The queue set
 *  holds no database pointer, and a database holds no queue set, so either may be freed first.
 */
class nvme_queues_t {
  public:

    nvme_queues_t() noexcept = default;
    nvme_queues_t(nvme_queues_t const&) = delete;
    nvme_queues_t& operator=(nvme_queues_t const&) = delete;
    nvme_queues_t(nvme_queues_t&&) noexcept = default;
    nvme_queues_t& operator=(nvme_queues_t&&) noexcept = default;

    /**
     *  @brief Allocates one block range per entry of @p memory_domains, on that domain, and opens
     *      two queues of @p transport over each.
     *  @param[in] transport A resolved transport; @c memory_k and @c blocking_k both
     *      take blocking queues.
     *  @param[in] config Each thread's depth and blocks.
     *  @param[in] memory_domains Where each thread's blocks are placed, -1 for anywhere, one entry
     *      per thread of the executor, which gets two queues and one block range.
     */
    static expected<nvme_queues_t> make(io_transport_t transport, io_channel_config_t const& config,
                                        std::span<fu_memory_domain_id_t const> memory_domains) noexcept {
        std::size_t const threads = memory_domains.size();
        if (threads == 0) return status_t {status_code_t::invalid_argument_k, "A queue set needs a thread"};
        if (transport == io_transport_t::automatic_k)
            return status_t {status_code_t::invalid_argument_k, "Resolve the transport before making a queue set"};
        nvme_queues_t queues;
        if (status_t const made = queues.size_(threads, transport); made.failed()) return made;
        queues.config_ = config;
        std::uint64_t const bytes = config.arena_bytes();
        for (std::size_t index = 0; index != threads; ++index) {
            thread_queues_t& thread = queues.threads_[index];
            fu_memory_domain_id_t const memory_domain = memory_domains[index];
            if (transport == io_transport_t::linux_spdk_k) {
                dma_storage_t& dma = thread.staging.template emplace<dma_storage_t>(dma_allocator_t {memory_domain});
                if (dma.resize(st::assume_uninitialized, static_cast<std::size_t>(bytes)) != st::status_t::success_k)
                    return status_t {status_code_t::out_of_memory_k, "A thread's DMA blocks do not fit"};
            }
            else {
                auto [block, allocate_status] = allocate_domain_block(memory_domain, static_cast<std::size_t>(bytes));
                if (allocate_status.failed()) return allocate_status;
                thread.staging = std::move(block);
            }
            if (status_t const opened = queues.open_(index, config); opened.failed()) return opened;
        }
        return queues;
    }

    /**
     *  @brief Thread @p thread_index's reading channel, once @p disk's media are registered on
     *      the thread's reading queue.
     *  @param[in] thread_index The calling thread's index in the executor, which alone submits
     *      on that queue.
     *  @param[in,out] disk The disk the call reads, which installs its media only when the
     *      queue is behind.
     */
    template <typename disk_type_>
        requires a_disk<disk_type_>
    expected<io_channel_t*> reading_channel_with_disk_media(std::size_t thread_index, disk_type_& disk) noexcept {
        if (thread_index >= threads_.size())
            return status_t {status_code_t::invalid_argument_k, "The queue set lacks that thread"};
        io_channel_t& reading = threads_[thread_index].reading;
        if (status_t const registered = disk.register_media(reading.queue_pair()); registered.failed())
            return registered;
        return &reading;
    }

    /** As @c reading_channel_with_disk_media, both channels of thread @p thread_index, for a merge
     *  slice that reads and writes at once. */
    template <typename disk_type_>
        requires a_disk<disk_type_>
    expected<thread_channels_t> both_channels_with_disk_media(std::size_t thread_index, disk_type_& disk) noexcept {
        if (thread_index >= threads_.size())
            return status_t {status_code_t::invalid_argument_k, "The queue set lacks that thread"};
        thread_queues_t& thread = threads_[thread_index];
        if (status_t const registered = disk.register_media(thread.reading.queue_pair()); registered.failed())
            return registered;
        if (status_t const registered = disk.register_media(thread.writing.queue_pair()); registered.failed())
            return registered;
        return thread_channels_t {&thread.reading, &thread.writing};
    }

    /** Threads the queue set holds queues for. */
    [[nodiscard]] std::size_t threads() const noexcept { return threads_.size(); }

    /** The transport every queue of the queue set moves bytes through. */
    [[nodiscard]] io_transport_t transport() const noexcept { return transport_; }

    /** The depth and blocks every thread's channels were carved to. */
    [[nodiscard]] io_channel_config_t const& config() const noexcept { return config_; }

    /** Bytes of blocks a ring addresses unpinned, counted once per ring, as the locked-memory cap
     *  refused the pin. */
    [[nodiscard]] std::uint64_t unpinned_staging_bytes() const noexcept {
        auto unpinned = [](queue_t const& queue) noexcept -> std::uint64_t {
            linux_uring_queue_t const* ring = std::get_if<linux_uring_queue_t>(&queue);
            return ring && ring->pinning() == staging_pinning_t::unpinned_k;
        };
        std::uint64_t refused = 0;
        for (thread_queues_t const& thread : threads_)
            refused += config_.arena_bytes() * (unpinned(thread.reading_queue) + unpinned(thread.writing_queue));
        return refused;
    }

    /** Requests the queue set's rings sent unpinned while their blocks were pinned, as each crossed
     *  a gibibyte, summed over every ring; read it only while no call runs on the queue set. */
    [[nodiscard]] std::uint64_t requests_across_registered_slots() const noexcept {
        auto crossed = [](queue_t const& queue) noexcept -> std::uint64_t {
            linux_uring_queue_t const* ring = std::get_if<linux_uring_queue_t>(&queue);
            return ring ? ring->requests_across_registered_slots() : 0;
        };
        std::uint64_t requests = 0;
        for (thread_queues_t const& thread : threads_)
            requests += crossed(thread.reading_queue) + crossed(thread.writing_queue);
        return requests;
    }

  private:

    /** The allocator of DMA bytes, placed on the queue set's memory domain. */
    using dma_allocator_t = linux_spdk_dma_allocator<byte_t>;

    /** The blocks of a thread of an SPDK queue set, from the SPDK environment. */
    using dma_storage_t = st::vector<byte_t, dma_allocator_t>;

    /** A thread's queue, of the transport the queue set resolved to. */
    using queue_t = std::variant<blocking_queue_t, linux_uring_queue_t, linux_aio_queue_t, linux_spdk_queue_t>;

    /** A thread's blocks, its pools over them and its two queues with the channels holding them. */
    struct thread_queues_t {

        /** Host blocks, or DMA blocks for SPDK. */
        std::variant<std::monostate, domain_block_t, dma_storage_t> staging;

        /** The blocks an ordinary transfer is split into. */
        buffer_pool_t request_buffers;

        /** The larger blocks a merge or an index streams through. */
        buffer_pool_t stream_buffers;

        /** The queues, which the channels below name. */
        queue_t reading_queue;
        queue_t writing_queue;

        /** The channels a call views. */
        io_channel_t reading;
        io_channel_t writing;
    };

    /** Sizes the table to @p threads, which never moves once the channels point into it. */
    status_t size_(std::size_t threads, io_transport_t transport) noexcept {
        if (threads >= static_cast<std::size_t>(queue_id_t::unallocated_k) / 2)
            return {status_code_t::invalid_argument_k, "A queue set names fewer queues than that"};
        if (threads_.resize(threads) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A queue set could not track its threads"};
        transport_ = transport;
        return {};
    }

    /** The bytes thread @p thread holds, whichever storage they came from. */
    [[nodiscard]] static std::span<byte_t> bytes_of_(thread_queues_t& thread) noexcept {
        if (domain_block_t* const block = std::get_if<domain_block_t>(&thread.staging))
            return std::span<byte_t> {*block};
        if (dma_storage_t* const dma = std::get_if<dma_storage_t>(&thread.staging)) return std::span<byte_t> {*dma};
        return {};
    }

    /** A queue of the queue set's transport over @p range. */
    expected<queue_t> make_queue_(queue_depth_t depth, std::span<byte_t> range) const noexcept {
        switch (transport_) {
        case io_transport_t::memory_k:
        case io_transport_t::blocking_k: return wrapped_<blocking_queue_t>(depth, range);
        case io_transport_t::linux_uring_k: return wrapped_<linux_uring_queue_t>(depth, range);
        case io_transport_t::linux_aio_k: return wrapped_<linux_aio_queue_t>(depth, range);
        case io_transport_t::linux_spdk_k: return wrapped_<linux_spdk_queue_t>(depth, range);
        case io_transport_t::automatic_k: break;
        }
        return status_t {status_code_t::invalid_argument_k, "The queue set's transport is unresolved"};
    }

    /** A queue of @p queue_type_ over @p range, in the variant every thread holds. */
    template <typename queue_type_>
    static expected<queue_t> wrapped_(queue_depth_t depth, std::span<byte_t> range) noexcept {
        auto [queue, make_queue_status] = queue_type_::make(depth, range);
        if (make_queue_status.failed()) return make_queue_status;
        return queue_t {std::in_place_type<queue_type_>, std::move(queue)};
    }

    /** The handle on @p queue, whose tokens hold @p id. */
    [[nodiscard]] static queue_ref_t handle_of_(queue_t& queue, queue_id_t id) noexcept {
        return std::visit([id](auto& held) noexcept { return queue_ref_t {held, id}; }, queue);
    }

    /** Carves thread @p index's pools out of its blocks and opens its two queues and channels. */
    status_t open_(std::size_t index, io_channel_config_t const& config) noexcept {
        thread_queues_t& thread = threads_[index];
        std::span<byte_t> const bytes = bytes_of_(thread);
        memory_reference_t const origin {registered_memory_id_t::queue_range_k, 0, 0};

        buffer_pool_config_t requests;
        requests.buffer_bytes = config.request_buffer_bytes;
        requests.buffers_count = config.request_buffers_count;
        auto [request_pool, make_request_pool_status] = buffer_pool_t::make(bytes, origin, 0, requests);
        if (make_request_pool_status.failed()) return make_request_pool_status;
        thread.request_buffers = std::move(request_pool);

        buffer_pool_config_t streams;
        streams.buffer_bytes = config.stream_buffer_bytes;
        streams.buffers_count = config.stream_buffers_count;
        std::uint64_t const stream_base = config.request_buffer_bytes * config.request_buffers_count;
        auto [stream_pool, make_stream_pool_status] = buffer_pool_t::make(bytes, origin, stream_base, streams);
        if (make_stream_pool_status.failed()) return make_stream_pool_status;
        thread.stream_buffers = std::move(stream_pool);

        auto [reading_queue, make_reading_status] = make_queue_(config.depth, bytes);
        if (make_reading_status.failed()) return make_reading_status;
        thread.reading_queue = std::move(reading_queue);
        auto [writing_queue, make_writing_status] = make_queue_(config.depth, bytes);
        if (make_writing_status.failed()) return make_writing_status;
        thread.writing_queue = std::move(writing_queue);

        queue_id_t const reading_id = static_cast<queue_id_t>(2 * index);
        queue_id_t const writing_id = static_cast<queue_id_t>(2 * index + 1);
        if (status_t const opened = open_channel_(thread.reading, handle_of_(thread.reading_queue, reading_id), thread);
            opened.failed())
            return opened;
        return open_channel_(thread.writing, handle_of_(thread.writing_queue, writing_id), thread);
    }

    /** Points @p channel at @p queue_pair and @p thread's pools and sizes its polling room and read
     *  ring. */
    static status_t open_channel_(io_channel_t& channel, queue_ref_t queue_pair, thread_queues_t& thread) noexcept {
        std::size_t const depth = queue_pair.depth();
        channel.queue_pair_ = queue_pair;
        channel.requests_ = &thread.request_buffers;
        channel.streams_ = &thread.stream_buffers;
        if (channel.completions_.resize(depth) != st::status_t::success_k ||
            channel.read_entries_.resize(depth) != st::status_t::success_k ||
            channel.read_buffers_.resize(depth) != st::status_t::success_k ||
            channel.read_requests_.resize(depth) != st::status_t::success_k ||
            channel.read_pieces_.resize(depth) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A channel could not size its read ring"};
        return {};
    }

    /** One entry per executor thread, sized once. */
    st::vector<thread_queues_t> threads_;

    /** The transport every queue moves bytes through. */
    io_transport_t transport_ = io_transport_t::blocking_k;

    /** What every thread's channels were carved to. */
    io_channel_config_t config_;
};

#pragma endregion Queue Sets

} // namespace unum::ustore::utilities
