/**
 *  @file include/ustore/utilities/nvme_channels.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The blocks a channel views, the channels that hold a thread's queue with its
 *      blocks, and the pipelines that hold a channel's ring at its depth.
 *
 *  Every I/O buffer in the engine comes from a buffer pool, because a request addresses memory by a
 *  range and an offset rather than by pointer, and a range is pinned once: @c io_uring answers
 *  @c EBUSY on re-registered memory and a growing range cannot be pinned at all. So each thread of
 *  a queue set allocates its blocks once, when its queue set is made, and everything above it views
 *  a block instead of allocating. A block is the unit of one request; a transfer larger than one
 *  block becomes several requests, which is mandatory anyway, since a namespace reports a maximum
 *  transfer well under the largest value a collection may hold.
 *
 *  The ring's rule binds a queue, its blocks and its completions together: the thread that submits
 *  must be the thread that polls, and the blocks in flight must not be touched by anyone else until
 *  it does. So they live in one channel of one thread rather than in a collection or an LSM tree,
 *  all of which outlive threads and cross them. A call takes its thread's channel by the thread
 *  index every fan-out already passes it, so no channel is ever contended.
 *
 *  A batch submitted whole and then waited on reaches the depth of that batch and no further, which
 *  is the shape the engine had and the reason it never reached a drive's queue. A pipeline instead
 *  offers until the channel runs out of blocks or credit, polls what has landed, recycles those
 *  blocks and offers again, so the ring stays full from the first refill to the last offer.
 *  Durability is written as submit, poll, flush, poll, because the ring promises no ordering at all
 *  and a flush covers only what its queue pair has already completed.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`

#include <array>   // `std::array`
#include <limits>  // `std::numeric_limits`
#include <span>    // `std::span`
#include <utility> // `std::exchange`, `std::move`

#include <smashtable/basic_fixed_pool.hpp> // `st::basic_fixed_pool`
#include <smashtable/basic_vector.hpp>     // `st::vector`

#include "ustore/utilities/nvme_ring.hpp" // `a_disk`, `read_request`, `write_request`, `deallocate_request`
#include "ustore/utilities/types.hpp"     // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::utilities {

#pragma region Buffer Pool

#pragma region Pool

/** What a channel's pool is shaped by, both numbers fixed at mount. */
struct buffer_pool_config_t {

    /** Bytes of one block, a multiple of the disk's length granularity and at most its max transfer. */
    std::uint64_t buffer_bytes = 0;

    /** Blocks the pool hands out, which bounds the requests one channel keeps in flight at once. */
    std::uint32_t buffers_count = 0;
};

class buffer_pool_t;

/**
 *  @brief One block viewed from a pool, which resolves to a place and to a span of the same bytes.
 *
 *  The queue pair is move-only and never copied, since the ring's rule is that a block belongs to the
 *  device from @c submit until its completion is completed: a second handle onto it would be a second
 *  claim on memory the device is writing. Returning it is the destructor's business, so a poll loop
 *  that fails halfway cannot leak a block.
 */
class pooled_buffer_t {
  public:

    pooled_buffer_t() noexcept = default;
    pooled_buffer_t(pooled_buffer_t const&) = delete;
    pooled_buffer_t& operator=(pooled_buffer_t const&) = delete;

    pooled_buffer_t(pooled_buffer_t&& other) noexcept
        : pool_(std::exchange(other.pool_, nullptr)), index_(std::exchange(other.index_, 0)),
          bytes_(std::exchange(other.bytes_, std::span<byte_t> {})) {}

    pooled_buffer_t& operator=(pooled_buffer_t&& other) noexcept;

    ~pooled_buffer_t() noexcept { release(); }

    /** Whether this handle still holds a block. */
    [[nodiscard]] bool held() const noexcept { return pool_ != nullptr; }

    /** The bytes of the block, which the caller fills before a write and reads after a read. */
    [[nodiscard]] std::span<byte_t> bytes() const noexcept { return bytes_; }

    /** Bytes of the block, which is the pool's block size. */
    [[nodiscard]] std::uint64_t size() const noexcept { return bytes_.size(); }

    /** The place @p offset bytes into the block, which a request holds instead of a pointer. */
    [[nodiscard]] memory_reference_t reference(std::uint64_t offset = 0) const noexcept;

    /** Hands the block back early, which the destructor does anyway. */
    void release() noexcept;

  private:

    friend class buffer_pool_t;

    /** The pool the block goes back to, null once the queue pair is empty. */
    buffer_pool_t* pool_ = nullptr;

    /** The block's index in its pool, which is the free-list entry pushed back on release. */
    std::uint32_t index_ = 0;

    /** The block's bytes inside the arena. */
    std::span<byte_t> bytes_;
};

/**
 *  @brief Fixed-size blocks carved out of one region of an arena, handed out to one channel.
 *
 *  There is one size class and no atomics, because a channel owns its pool outright and a channel is
 *  single-bound thread by the ring's own rule. Exhaustion is therefore always the channel's own doing, and
 *  the channel is the thread that polls, so the answer to an exhausted pool is to poll a completion and
 *  release its block rather than to wait on anyone.
 */
class buffer_pool_t {
  public:

    buffer_pool_t() noexcept = default;
    buffer_pool_t(buffer_pool_t const&) = delete;
    buffer_pool_t& operator=(buffer_pool_t const&) = delete;
    buffer_pool_t(buffer_pool_t&&) noexcept = default;
    buffer_pool_t& operator=(buffer_pool_t&&) noexcept = default;

    /**
     *  @brief Carves @p config blocks out of @p range, starting at @p region_offset.
     *  @param[in] range The bytes the blocks live in, which must outlive the pool.
     *  @param[in] origin The place of the first byte of @p range, which every block's place
     *      is offset from.
     *  @param[in] region_offset First byte of this pool's region inside the range.
     *  @param[in] config Block size and block count, both fixed from here on.
     */
    static expected<buffer_pool_t> make(std::span<byte_t> range, memory_reference_t origin, std::uint64_t region_offset,
                                        buffer_pool_config_t const& config) noexcept {
        if (config.buffer_bytes == 0 || config.buffers_count == 0)
            return status_t {status_code_t::invalid_argument_k, "A pool holds at least one block"};
        std::uint64_t const span = config.buffer_bytes * config.buffers_count;
        if (region_offset > range.size() || span > range.size() - region_offset)
            return status_t {status_code_t::invalid_argument_k, "A pool's region runs past its range"};

        buffer_pool_t pool;
        pool.range_ = range;
        pool.origin_ = origin;
        pool.region_offset_ = region_offset;
        pool.buffer_bytes_ = config.buffer_bytes;
        auto [buffers, make_buffers_status] = buffers_t::make(config.buffers_count);
        if (make_buffers_status != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A pool could not track its blocks"};
        pool.buffers_ = std::move(buffers);
        return pool;
    }

    /** Takes one block, or answers @c capacity_exhausted_k when every block of this channel is out. */
    expected<pooled_buffer_t> acquire() noexcept {
        std::uint32_t const index = buffers_.take();
        if (index == buffers_t::absent_k)
            return status_t {status_code_t::capacity_exhausted_k, "Every block of this channel is out"};
        pooled_buffer_t buffer;
        buffer.pool_ = this;
        buffer.index_ = index;
        buffer.bytes_ = range_.subspan(static_cast<std::size_t>(region_offset_ + index * buffer_bytes_),
                                       static_cast<std::size_t>(buffer_bytes_));
        return buffer;
    }

    /** Blocks nobody holds, which a submit loop reads to decide how much more to offer. */
    [[nodiscard]] std::uint32_t available_buffers() const noexcept {
        return static_cast<std::uint32_t>(buffers_.capacity() - buffers_.size());
    }

    /** Blocks the pool was carved with. */
    [[nodiscard]] std::uint32_t total_buffers() const noexcept {
        return static_cast<std::uint32_t>(buffers_.capacity());
    }

    /** Bytes of one block, which is the unit every transfer is split into. */
    [[nodiscard]] std::uint64_t buffer_bytes() const noexcept { return buffer_bytes_; }

    /** Whether the pool was ever carved, which an unmounted device pool is not. */
    [[nodiscard]] bool made() const noexcept { return !range_.empty(); }

  private:

    friend class pooled_buffer_t;

    /** One block's record, which needs nothing past its generation since its bytes follow
     *  from its index. */
    struct buffer_record_t {

        /** Stepped every time the block comes back, zeroed by the pool's value-initialization. */
        std::uint32_t generation;
    };

    using buffers_t = st::basic_fixed_pool<buffer_record_t>;

    /** Takes @p index back, which only a queue pair's own release calls. */
    void recycle_(std::uint32_t index) noexcept { buffers_.vacate(index); }

    /** The place of the block at @p index, @p offset bytes in. */
    [[nodiscard]] memory_reference_t place_(std::uint32_t index, std::uint64_t offset) const noexcept {
        return memory_reference_t {origin_.memory_id, origin_.generation,
                                   origin_.offset + region_offset_ + index * buffer_bytes_ + offset};
    }

    /** The bytes the blocks live in, empty until the pool is made. */
    std::span<byte_t> range_;

    /** The place of the first byte of @c range_. */
    memory_reference_t origin_;

    /** Where the pool's first block sits inside the range, so a queue pair can mint its place. */
    std::uint64_t region_offset_ = 0;

    /** Bytes of one block. */
    std::uint64_t buffer_bytes_ = 0;

    /** One record per block, taken and vacated by the owning channel alone. */
    buffers_t buffers_;
};

inline pooled_buffer_t& pooled_buffer_t::operator=(pooled_buffer_t&& other) noexcept {
    if (this == &other) return *this;
    release();
    pool_ = std::exchange(other.pool_, nullptr);
    index_ = std::exchange(other.index_, 0);
    bytes_ = std::exchange(other.bytes_, std::span<byte_t> {});
    return *this;
}

inline memory_reference_t pooled_buffer_t::reference(std::uint64_t offset) const noexcept {
    return pool_ ? pool_->place_(index_, offset) : memory_reference_t {};
}

inline void pooled_buffer_t::release() noexcept {
    if (!pool_) return;
    pool_->recycle_(index_);
    pool_ = nullptr;
    bytes_ = std::span<byte_t> {};
}

#pragma endregion Pool

#pragma endregion Buffer Pool

#pragma region IO Channel

#pragma region Shape

/** What a queue set carves each thread's blocks into. */
struct io_channel_config_t {

    /** Bytes of one block in the request pool, which every ordinary transfer is split into. */
    std::uint64_t request_buffer_bytes = 64ull * 1024ull;

    /** Blocks in the request pool, which bounds what one channel keeps in flight. */
    std::uint32_t request_buffers_count = 128;

    /** Bytes of one block in the streaming pool, for an index, a merge or a backup. */
    std::uint64_t stream_buffer_bytes = 1024ull * 1024ull;

    /** Blocks in the streaming pool, which is the depth a merge writes at. */
    std::uint32_t stream_buffers_count = 4;

    /** Requests one queue pair keeps in flight, which is also the room one poll needs. */
    queue_depth_t depth = 64;

    /** Bytes one thread's blocks take, derived from the four numbers above. */
    [[nodiscard]] constexpr std::uint64_t arena_bytes() const noexcept {
        return request_buffer_bytes * request_buffers_count + stream_buffer_bytes * stream_buffers_count;
    }
};

#pragma endregion Shape

#pragma region Channel

class nvme_queues_t;

/** The queue set a call moves bytes on and the index of the thread running it, whose queues alone
 *  serve that thread. */
struct nvme_queues_ref_t {
    nvme_queues_t* queues = nullptr;
    std::size_t thread_index = 0;
};

template <typename disk_type_>
    requires a_disk<disk_type_>
class basic_read_pipeline;

/**
 *  @brief One queue of a thread and everything that thread needs to keep it full, owned
 *      by the queue set.
 *
 *  A thread holds two channels, one reading and one writing, over the same blocks, since a
 *  merge keeps two pipelines in flight at once and two pipelines cannot share one queue's
 *  completions. A call reaches a channel through
 *  @c nvme_queues_t::reading_channel_with_disk_media and never opens one.
 */
class io_channel_t {
  public:

    io_channel_t() noexcept = default;
    io_channel_t(io_channel_t const&) = delete;
    io_channel_t& operator=(io_channel_t const&) = delete;
    io_channel_t(io_channel_t&&) noexcept = default;
    io_channel_t& operator=(io_channel_t&&) noexcept = default;

    /** The queue pair this channel submits and polls on. */
    [[nodiscard]] queue_ref_t const& queue_pair() const noexcept { return queue_pair_; }

    /** The blocks a transfer of ordinary size is read into or written from. */
    [[nodiscard]] buffer_pool_t& requests() noexcept { return *requests_; }

    /** The larger blocks a streaming read or write moves, for an index, a merge or a backup. */
    [[nodiscard]] buffer_pool_t& streams() noexcept { return *streams_; }

    /** Room the channel polls into, sized to the queue pair's depth so one poll never truncates. */
    [[nodiscard]] std::span<completion_t> completion_buffer() noexcept {
        return std::span<completion_t> {completions_};
    }

    /** One block of one value's read in flight, which the caller holding the queue pair files
     *  under its tag. */
    struct read_piece_t {

        /** Which of the caller's requests the block belongs to. */
        std::uint32_t request = 0;

        /** Bytes of the block before the value's own bytes. */
        std::uint32_t skipped = 0;

        /** Bytes of the value the block holds. */
        std::uint32_t held = 0;

        /** Bytes of the value the blocks before this one held. */
        std::uint32_t offset = 0;
    };

    static_assert(sizeof(read_piece_t) == 16, "A read piece fills a quarter of a cache line");

    /** Room for the piece of every read in flight, indexed by tag modulo the queue pair's depth,
     *  which no pipeline's window reaches. */
    [[nodiscard]] std::span<read_piece_t> read_pieces() noexcept { return std::span<read_piece_t> {read_pieces_}; }

    /** The next token of this channel, whose tag is unique among its in-flight requests. */
    [[nodiscard]] request_token_t next_token() noexcept { return queue_pair_.token(next_tag_++); }

    /** A token of this channel holding the caller's own @p tag. */
    [[nodiscard]] request_token_t token(request_tag_t tag) const noexcept { return queue_pair_.token(tag); }

  private:

    friend class nvme_queues_t;

    template <typename disk_type_>
        requires a_disk<disk_type_>
    friend class basic_read_pipeline;

    /** One read a pipeline offered and has not yet retired, at the ring position of its slot. */
    struct read_entry_t {

        /** The bytes the device writes into, a block's or the caller's, null once retired. */
        byte_t* destination = nullptr;

        /** Bytes the request asked for, against which a partial transfer is measured. */
        std::uint32_t requested_bytes = 0;

        /** The caller's tag, which the sink matches its own bookkeeping on. */
        request_tag_t tag = 0;
    };

    static_assert(sizeof(read_entry_t) == 16, "A read entry fills a quarter of a cache line");

    /** The handle on the thread's queue this channel submits and polls on. */
    queue_ref_t queue_pair_;

    /** The request blocks of the thread, which its other channel shares on the same thread. */
    buffer_pool_t* requests_ = nullptr;

    /** The streaming blocks of the thread, shared the same way. */
    buffer_pool_t* streams_ = nullptr;

    /** Room for one poll, sized to the queue pair's depth. */
    st::vector<completion_t> completions_;

    /** The reads of the pipeline holding the queue pair, a ring of its depth indexed by slot. */
    st::vector<read_entry_t> read_entries_;

    /** The block each of those reads fills, none for a read into the caller's memory. */
    st::vector<pooled_buffer_t> read_buffers_;

    /** The requests of those reads, the newest of which the ring may not have taken yet. */
    st::vector<request_t> read_requests_;

    /** What every read in flight holds for its caller, a ring of the depth indexed by tag. */
    st::vector<read_piece_t> read_pieces_;

    /** The tag the next request of this channel holds, wrapping harmlessly past the depth. */
    request_tag_t next_tag_ = 0;
};

#pragma endregion Channel

#pragma endregion IO Channel

#pragma region IO Pipeline

#pragma region Polling

/** One block that came back, handed to the sink while the channel still holds its queue pair. */
struct completed_buffer_t {

    /** The tag the caller minted when it offered the transfer, naming its own bookkeeping. */
    request_tag_t tag = 0;

    /** The bytes the device touched, which is a prefix of the block on a partial transfer. */
    std::span<byte_t const> bytes;

    /** How the transfer ended, which the caller branches on before it trusts @c bytes. */
    completion_state_t state = completion_state_t::completed_k;

    /** Why it ended that way. */
    status_t status;

    /** Bytes the request asked for, against which a partial transfer is measured. */
    std::uint64_t requested_bytes = 0;

    /** Whether every byte asked for arrived, which is the only state a block read may trust. */
    [[nodiscard]] bool completed_in_full() const noexcept { return state == completion_state_t::completed_k; }
};

/** The status a caller that needs every byte reports for @p completed, which only a whole move passes. */
inline status_t require_completed_in_full(completed_buffer_t const& completed) noexcept {
    if (completed.state == completion_state_t::completed_k) return {};
    if (completed.state == completion_state_t::cancelled_k)
        return {status_code_t::uninitialized_k, "The queue_pair cancelled this transfer"};
    if (completed.status.failed()) return completed.status;
    return {status_code_t::io_k, "The device moved fewer bytes than asked"};
}

#pragma endregion Polling

#pragma region Reading

/**
 *  @brief Keeps a channel's ring as full as its blocks allow, polling to make room and sleeping
 *      only when nothing else can move.
 *
 *  A caller offers transfers, flushes them to the ring and polls or waits; the sink sees each
 *  block while the channel still owns it, copies what it needs out, and the block is recycled the
 *  moment the sink returns. A tag is the caller's own, so a sink can index whatever plan it built.
 *  The offers live in the channel's read ring at their slot's position, so nothing is allocated.
 */
template <typename disk_type_>
    requires a_disk<disk_type_>
class basic_read_pipeline {
  public:

    basic_read_pipeline(basic_read_pipeline const&) = delete;
    basic_read_pipeline& operator=(basic_read_pipeline const&) = delete;

    /** A pipeline on @p channel, which its thread owns and gives no other pipeline while this one
     *  lives, against @p disk. */
    basic_read_pipeline(disk_type_& disk, io_channel_t& channel, medium_reference_t medium) noexcept
        : disk_(disk), channel_(channel), medium_(medium) {}

    /** Waits out whatever is still in flight and hands every block back, so the next pipeline
     *  finds the read ring empty and no completion of this one arrives under its slots. */
    ~basic_read_pipeline() noexcept {
        if (outstanding_ != 0) [[maybe_unused]]
            status_t const drained = disk_.drain(channel_.queue_pair());
        for (; oldest_slot_ != next_slot_; ++oldest_slot_) {
            std::size_t const position = position_(oldest_slot_);
            channel_.read_buffers_[position].release();
            channel_.read_entries_[position].destination = nullptr;
        }
    }

    /**
     *  @brief Whether another offer would find a block, credit and room in the window, without taking any.
     *
     *  The window runs from the oldest unretired offer to the newest and never reaches the queue pair's depth,
     *  so a caller tagging offers consecutively may index a table of the depth by tag modulo depth.
     */
    [[nodiscard]] bool can_enqueue() const noexcept {
        return channel_.requests().available_buffers() != 0 && can_enqueue_into();
    }

    /** Whether another offer into registered memory the caller holds would find credit and room
     *  in the window. */
    [[nodiscard]] bool can_enqueue_into() const noexcept {
        return static_cast<std::size_t>(next_slot_ - oldest_slot_) < channel_.read_entries_.size();
    }

    /** Queues a read of @p extent of the pipeline's medium under @p tag, as the
     *  three-argument form does. */
    status_t enqueue_read(extent_t extent, request_tag_t tag) noexcept { return enqueue_read(medium_, extent, tag); }

    /** Queues a read of @p extent of @p medium under @p tag, or answers @c capacity_exhausted_k
     *  when out of room. */
    status_t enqueue_read(medium_reference_t medium, extent_t extent, request_tag_t tag) noexcept {
        if (!can_enqueue()) return {status_code_t::capacity_exhausted_k, "This channel has no block or credit left"};
        auto [block, acquire_status] = channel_.requests().acquire();
        if (acquire_status.failed()) return acquire_status;
        memory_reference_t const into = block.reference();
        std::span<byte_t> const destination = block.bytes();
        return offer_(medium, extent, into, destination, std::move(block), tag);
    }

    /**
     *  @brief Queues a read of @p extent of @p medium under @p tag straight into @p destination,
     *      registered memory whose place @p into names, which the caller holds until the read
     *      completes, so the read takes no block.
     *
     *  The sink sees the bytes where they landed, and nothing is recycled once it returns.
     */
    status_t enqueue_read(medium_reference_t medium, extent_t extent, memory_reference_t into,
                          std::span<byte_t> destination, request_tag_t tag) noexcept {
        if (!can_enqueue_into()) return {status_code_t::capacity_exhausted_k, "This channel has no credit left"};
        return offer_(medium, extent, into, destination, pooled_buffer_t {}, tag);
    }

    /** Submits what is staged, keeping whatever the ring would not take for the next call. */
    status_t submit() noexcept {
        std::size_t const positions = channel_.read_requests_.size();
        while (staged_slot_ != next_slot_) {
            std::size_t const first = position_(staged_slot_);
            std::size_t const staged = static_cast<std::size_t>(next_slot_ - staged_slot_);
            std::size_t const contiguous = staged < positions - first ? staged : positions - first;
            std::span<request_t const> const batch {channel_.read_requests_.data() + first, contiguous};
            auto [taken, submit_status] = disk_.submit(channel_.queue_pair(), batch);
            if (submit_status.failed()) return submit_status;
            if (taken == 0) return {};
            outstanding_ += taken;
            staged_slot_ += static_cast<request_tag_t>(taken);
        }
        return {};
    }

    /** Requests offered and not yet polled, staged ones included. */
    [[nodiscard]] std::size_t outstanding() const noexcept {
        return outstanding_ + static_cast<std::size_t>(next_slot_ - staged_slot_);
    }

    /**
     *  @brief Harvests every completion that has landed, calling @p sink once per block, and
     *      returns at once when none has.
     *  @param[in] sink Called with a @c completed_buffer_t while the channel still owns the block.
     *  @return How many completions arrived, which is zero when nothing has landed yet.
     */
    template <typename sink_type_>
    expected<std::size_t> poll_completions(sink_type_&& sink) noexcept {
        if (outstanding_ == 0) return std::size_t {0};
        auto [completed, poll_completions_status] = disk_.poll_completions(channel_.queue_pair(),
                                                                           channel_.completion_buffer());
        if (poll_completions_status.failed()) return poll_completions_status;
        return retire_(completed, sink);
    }

    /**
     *  @brief Harvests at least one completion, sleeping in the kernel where the disk can park the
     *      thread, and polling once where it cannot, as on SPDK and the blocking floor.
     *  @param[in] sink Called with a @c completed_buffer_t while the channel still owns the block.
     *  @return How many completions arrived, which is zero only when nothing is in flight.
     */
    template <typename sink_type_>
    expected<std::size_t> wait_completions(sink_type_&& sink) noexcept {
        if (outstanding_ == 0) return std::size_t {0};
        auto [completed, wait_completions_status] = wait_on_disk_();
        if (wait_completions_status.failed()) return wait_completions_status;
        return retire_(completed, sink);
    }

    /** Harvests until at least one block retires, which is how an exhausted pool makes room. */
    template <typename sink_type_>
    status_t poll_one_completion(sink_type_&& sink) noexcept {
        if (outstanding_ == 0) return {status_code_t::invalid_argument_k, "This channel has nothing in flight"};
        std::size_t const before = outstanding_;
        while (outstanding_ == before) {
            if (disk_.outstanding(channel_.queue_pair()) == 0)
                return {status_code_t::io_k, "The ring answered a request not retired here"};
            auto [seen, wait_completions_status] = wait_completions(sink);
            if (wait_completions_status.failed()) return wait_completions_status;
        }
        return {};
    }

    /** Harvests everything outstanding, which a caller does before it drops the pipeline. */
    template <typename sink_type_>
    status_t poll_all_completions(sink_type_&& sink) noexcept {
        // Counts what is staged as well as what is submitted, so an offer that was never flushed is
        // still driven to the device rather than leaving the caller reading nothing.
        while (outstanding() != 0) {
            if (status_t const flushed = submit(); flushed.failed()) return flushed;
            if (status_t const polled = poll_one_completion(sink); polled.failed()) return polled;
        }
        return {};
    }

  private:

    using read_entry_t = io_channel_t::read_entry_t;

    /** The read ring position of @p slot. */
    [[nodiscard]] std::size_t position_(request_tag_t slot) const noexcept {
        return static_cast<std::size_t>(slot) % channel_.read_entries_.size();
    }

    /** Stages a read of @p extent of @p medium into @p destination, whose place @p into names,
     *  keeping @p block until its completion. */
    status_t offer_(medium_reference_t medium, extent_t extent, memory_reference_t into, std::span<byte_t> destination,
                    pooled_buffer_t&& block, request_tag_t tag) noexcept {
        if (extent.length > (std::numeric_limits<std::uint32_t>::max)())
            return {status_code_t::invalid_argument_k, "A read exceeds the bytes of one request"};
        std::size_t const position = position_(next_slot_);
        channel_.read_requests_[position] = read_request(medium, extent, into, channel_.token(next_slot_));
        channel_.read_buffers_[position] = std::move(block);
        channel_.read_entries_[position] = read_entry_t {destination.data(), static_cast<std::uint32_t>(extent.length),
                                                         tag};
        ++next_slot_;
        return {};
    }

    /** Waits for at least one completion where the disk offers a sleeping wait, or polls once. */
    expected<std::size_t> wait_on_disk_() noexcept {
        if constexpr (a_sleeping_disk<disk_type_>)
            return disk_.wait_completions(channel_.queue_pair(), channel_.completion_buffer());
        else return disk_.poll_completions(channel_.queue_pair(), channel_.completion_buffer());
    }

    /** Hands the first @p completed completions of the channel's buffer to @p sink, recycling
     *  their blocks, and moves the window past the retired prefix. */
    template <typename sink_type_>
    std::size_t retire_(std::size_t completed, sink_type_& sink) noexcept {
        std::span<completion_t const> const landed = channel_.completion_buffer().first(completed);
        for (completion_t const& completion : landed) {
            if (!completion.retires()) continue;
            request_tag_t const slot = completion.token.tag();
            request_tag_t const submitted = staged_slot_ - oldest_slot_;
            if (static_cast<request_tag_t>(slot - oldest_slot_) >= submitted) continue;
            std::size_t const position = position_(slot);
            read_entry_t& entry = channel_.read_entries_[position];
            if (entry.destination == nullptr) continue;
            completed_buffer_t block;
            block.tag = entry.tag;
            block.state = completion.state;
            block.status = completion.status;
            block.requested_bytes = entry.requested_bytes;
            block.bytes = std::span<byte_t const> {entry.destination,
                                                   static_cast<std::size_t>(completion.transferred_bytes)};
            sink(block);
            channel_.read_buffers_[position].release();
            entry.destination = nullptr;
            --outstanding_;
        }
        while (oldest_slot_ != staged_slot_ && channel_.read_entries_[position_(oldest_slot_)].destination == nullptr)
            ++oldest_slot_;
        return landed.size();
    }

    /** The disk this pipeline submits against. */
    disk_type_& disk_;

    /** The channel this pipeline submits and polls on, whose read ring holds the offers. */
    io_channel_t& channel_;

    /** The medium every offer reads from. */
    medium_reference_t medium_;

    /** Requests the ring has taken and not yet handed back. */
    std::size_t outstanding_ = 0;

    /** The oldest offer not yet retired, where the window starts. */
    request_tag_t oldest_slot_ = 0;

    /** The oldest offer the ring has not yet taken, where the staged requests start. */
    request_tag_t staged_slot_ = 0;

    /** The ring tag the next offer takes, which rises for the life of the pipeline. */
    request_tag_t next_slot_ = 0;
};

#pragma endregion Reading

#pragma region Writing

/**
 *  @brief Stages bytes into blocks and writes them, with one durability barrier at the end.
 *
 *  Chaining is not that barrier and is not used for it: a flush covers only what its queue pair has
 *  already completed, so the poll before it is what makes it mean anything.
 *
 *  @tparam waiter_type_ What the core does while it waits for the barrier's completion.
 */
template <typename disk_type_, typename waiter_type_>
    requires a_disk<disk_type_>
class basic_write_pipeline {
  public:

    basic_write_pipeline(basic_write_pipeline const&) = delete;
    basic_write_pipeline& operator=(basic_write_pipeline const&) = delete;

    /** A pipeline on @p channel, which its thread owns and gives no other pipeline while this one
     *  lives, against @p disk. */
    basic_write_pipeline(disk_type_& disk, io_channel_t& channel, medium_reference_t medium) noexcept
        : disk_(disk), channel_(channel), medium_(medium) {}

    /** Takes a block to fill, or answers @c capacity_exhausted_k until a poll_completions frees one. */
    expected<pooled_buffer_t> acquire_buffer() noexcept { return channel_.requests().acquire(); }

    /** Takes a streaming block to fill, for a merge or a backup that moves whole megabytes. */
    expected<pooled_buffer_t> acquire_stream_buffer() noexcept { return channel_.streams().acquire(); }

    /** Takes a block, polling completions first when every block of the channel is out. */
    expected<pooled_buffer_t> acquire_buffer_or_poll() noexcept {
        expected<pooled_buffer_t> block = acquire_buffer();
        while (block == status_code_t::capacity_exhausted_k && outstanding() != 0) {
            if (status_t const submitted = submit(); submitted.failed()) return submitted;
            if (status_t const polled = poll_one_completion(); polled.failed()) return polled;
            block = acquire_buffer();
        }
        return block;
    }

    /**
     *  @brief Queues a write of the first @p length bytes of @p block at @p extent, taking the queue pair.
     *  @param[in] block The filled block, whose queue pair this pipeline holds until the write retires.
     *  @param[in] extent Where on the medium the bytes land, whose length is what is written.
     */
    status_t enqueue_write(pooled_buffer_t&& block, extent_t extent) noexcept {
        return stage_(write_request(medium_, extent, block.reference(), channel_.token(next_slot_)), std::move(block));
    }

    /**
     *  @brief Queues giving @p extent back to the medium, which reads as anything afterwards,
     *      holding no block.
     *  @param[in] extent The range the medium may stop keeping, retired like a write.
     */
    status_t enqueue_deallocate(extent_t extent) noexcept {
        return stage_(deallocate_request(medium_, extent, channel_.token(next_slot_)), pooled_buffer_t {});
    }

    /** Submits what is staged, keeping whatever the ring would not take for the next call. */
    status_t submit() noexcept {
        while (!staged_.empty()) {
            auto [taken, submit_status] = disk_.submit(channel_.queue_pair(), std::span<request_t const> {staged_});
            if (submit_status.failed()) return submit_status;
            if (taken == 0) return {};
            outstanding_ += taken;
            drop_front_(staged_, taken);
        }
        return {};
    }

    /** Writes offered and not yet polled, staged ones included. */
    [[nodiscard]] std::size_t outstanding() const noexcept { return outstanding_ + staged_.size(); }

    /** Harvests landed completions, recycling their blocks and failing on anything short of whole. */
    status_t poll_completions() noexcept {
        if (outstanding_ == 0) return {};
        auto [completed, poll_completions_status] = disk_.poll_completions(channel_.queue_pair(),
                                                                           channel_.completion_buffer());
        if (poll_completions_status.failed()) return poll_completions_status;
        status_t first;
        std::span<completion_t const> const landed = channel_.completion_buffer().first(completed);
        for (completion_t const& completion : landed) {
            if (!completion.retires()) continue;
            entry_t* const entry = entry_of_(completion.token.tag());
            if (!entry) continue;
            if (completion.state != completion_state_t::completed_k && first.succeeded())
                first = short_write_status_(completion);
            entry->block.release();
            entry->live = false;
            --outstanding_;
        }
        compact_();
        return first;
    }

    /** Harvests until at least one write retires, which is how an exhausted pool makes room. */
    status_t poll_one_completion() noexcept {
        if (outstanding_ == 0) return {};
        std::size_t const before = outstanding_;
        for (std::size_t spins = 0; outstanding_ == before && spins != poll_spin_limit_k; ++spins)
            if (status_t const polled = poll_completions(); polled.failed()) return polled;
        return outstanding_ == before ? status_t {status_code_t::io_k, "A channel's completions never arrived"}
                                      : status_t {};
    }

    /** Submits and reaps every write, so each has reached the device, without the flush that
     *  makes them durable. */
    status_t poll_all_completions() noexcept {
        while (outstanding() != 0) {
            if (status_t const flushed = submit(); flushed.failed()) return flushed;
            if (status_t const polled = poll_one_completion(); polled.failed()) return polled;
        }
        return {};
    }

    /**
     *  @brief Reaps every write, then flushes the device and polls that, which is the barrier.
     *
     *  @note The poll before the flush is what gives the flush meaning, since a flush covers only
     *      what its queue pair has already completed.
     */
    status_t make_durable() noexcept {
        if (status_t const polled = poll_all_completions(); polled.failed()) return polled;
        std::array<request_t, 1> barrier {flush_request(medium_, channel_.token(next_slot_++))};
        auto [taken, submit_status] = disk_.submit(channel_.queue_pair(), std::span<request_t const> {barrier});
        if (submit_status.failed()) return submit_status;
        if (taken != 1) return {status_code_t::io_k, "The ring would not take a flush"};
        auto [completed, poll_completions_status] = poll_completions_at_least<waiter_type_>(
            disk_, channel_.queue_pair(), channel_.completion_buffer(), 1);
        if (poll_completions_status.failed()) return poll_completions_status;
        if (completed == 0) return {status_code_t::io_k, "A flush never completed"};
        completion_t const& completion = channel_.completion_buffer().front();
        return completion.completed_in_full() ? status_t {} : completion.status;
    }

  private:

    /** One queued write and the block it drains, retired when its completion is polled. */
    struct entry_t {

        /** The block the device reads from, recycled once the write retires. */
        pooled_buffer_t block;

        /** Bytes the write asked for, against which a partial transfer is measured. */
        std::uint64_t requested_bytes = 0;

        /** Whether this entry still awaits its completion. */
        bool live = true;
    };

    /** Stages @p request and tracks @p block, which it holds until the request retires. */
    status_t stage_(request_t const& request, pooled_buffer_t&& block) noexcept {
        if (staged_.push_back(request_t {request}) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A pipeline could not stage another write"};
        entry_t entry;
        entry.block = std::move(block);
        entry.requested_bytes = request.extent.length;
        if (entries_.push_back(std::move(entry)) != st::status_t::success_k) {
            staged_.pop_back();
            return {status_code_t::out_of_memory_k, "A pipeline could not track another write"};
        }
        ++next_slot_;
        return {};
    }

    /** What a write that moved less than it asked for means, which is never a retry. */
    static status_t short_write_status_(completion_t const& completion) noexcept {
        if (completion.state == completion_state_t::cancelled_k)
            return {status_code_t::uninitialized_k, "The queue_pair cancelled this write"};
        if (completion.status.failed()) return completion.status;
        return {status_code_t::capacity_exhausted_k, "The volume took fewer bytes than written"};
    }

    /** The entry a completion belongs to, found by the ring tag it was minted with. */
    [[nodiscard]] entry_t* entry_of_(request_tag_t slot) noexcept {
        request_tag_t const base = next_slot_ - static_cast<request_tag_t>(entries_.size());
        std::uint32_t const index = slot - base;
        if (index >= entries_.size()) return nullptr;
        entry_t& entry = entries_[index];
        return entry.live ? &entry : nullptr;
    }

    /** Drops the retired prefix of the entry table, so a long run does not grow it without bound. */
    void compact_() noexcept {
        std::size_t retired = 0;
        while (retired != entries_.size() && !entries_[retired].live) ++retired;
        if (retired != 0) drop_front_(entries_, retired);
    }

    /** Removes the first @p count elements of @p elements, shifting the rest down. */
    template <typename element_type_>
    static void drop_front_(st::vector<element_type_>& elements, std::size_t count) noexcept {
        for (std::size_t index = count; index != elements.size(); ++index)
            elements[index - count] = std::move(elements[index]);
        for (std::size_t index = 0; index != count; ++index) elements.pop_back();
    }

    /** The disk this pipeline submits against. */
    disk_type_& disk_;

    /** The channel this pipeline submits and polls on. */
    io_channel_t& channel_;

    /** The medium every write lands on. */
    medium_reference_t medium_;

    /** One entry per queued write, in the order they were committed. */
    st::vector<entry_t> entries_;

    /** Writes built but not yet taken by the ring. */
    st::vector<request_t> staged_;

    /** Writes the ring has taken and not yet handed back. */
    std::size_t outstanding_ = 0;

    /** The ring tag the next write takes, which rises for the life of the pipeline. */
    request_tag_t next_slot_ = 0;
};

#pragma endregion Writing

#pragma endregion IO Pipeline

} // namespace unum::ustore::utilities
