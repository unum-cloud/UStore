/**
 *  @file include/ustore/utilities/nvme_channels.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The one memory range a disk pins, the channels that check a queue pair out with its blocks, and the
 *      pipelines that hold a channel's ring at its depth.
 *
 *  Every I/O buffer in the engine comes from the buffer pool, because a request addresses memory by registered
 *  memory slot and offset rather than by pointer, and a range can only be pinned once: @c io_uring answers
 *  @c EBUSY on re-registered memory and a growing arena cannot be pinned at all. So an arena is allocated,
 *  charged and registered once at mount, and everything above it borrows a block instead of
 *  allocating. A block is the unit of one request; a transfer larger than one block becomes several
 *  requests, which is mandatory anyway, since a namespace reports a maximum transfer well under the
 *  largest value a collection may hold.
 *
 *  The ring's rule binds a queue pair, its blocks and its completions together: the thread that submits must be
 *  the thread that polls, and the blocks in flight must not be touched by anyone else until it does. So they live
 *  in one channel, opened at mount and borrowed by callers, rather than being held by a collection or an LSM
 *  tree - all of which outlive threads and cross them, which a queue pair may not do. An executor thread takes
 *  its own channel by the thread index every fan-out already passes it, so the common path contends with
 *  nobody. A thread outside the pool claims one of a few shared channels, or is refused and backs off, which is
 *  the contract the job gate already asks of its callers.
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
#include <span>    // `std::span`
#include <utility> // `std::exchange`, `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/utilities/nvme_ring.hpp" // `registered_memory_t`, `ring_concept`, `aligned_allocator`
#include "ustore/utilities/types.hpp"     // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::utilities {

#pragma region Buffer Pool

#pragma region Arena

/** What a mount fixes about an arena before any channel carves blocks out of it. */
struct memory_arena_config_t {

    /** Bytes the arena holds, rounded up to @c portable_alignment_k when it is made. */
    std::uint64_t bytes = 0;

    /** Where the bytes live, which the disk must be able to register. */
    memory_space_t placement = memory_space_t::host_k;
};

/**
 *  @brief The one memory range a disk registers, owned for the life of the mount.
 *
 *  It is charged to its budget once, here, which is why no buffer below it is charged again: the
 *  bytes were paid for when the disk came up, and a mount that cannot afford its I/O memory is a
 *  configuration error rather than a failure discovered under load.
 *
 *  @tparam atomic_reference_ The reference the budget owns its word through.
 */
template <template <typename> typename atomic_reference_>
class registered_memory_arena {
  public:

    /** The budget the whole arena is charged to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    registered_memory_arena() noexcept = default;
    registered_memory_arena(registered_memory_arena const&) = delete;
    registered_memory_arena& operator=(registered_memory_arena const&) = delete;

    registered_memory_arena(registered_memory_arena&& other) noexcept
        : storage_(std::move(other.storage_)), registered_memory_(std::move(other.registered_memory_)),
          constraint_(std::exchange(other.constraint_, nullptr)), charged_(std::exchange(other.charged_, 0)) {}

    registered_memory_arena& operator=(registered_memory_arena&& other) noexcept {
        if (this == &other) return *this;
        refund_();
        storage_ = std::move(other.storage_);
        registered_memory_ = std::move(other.registered_memory_);
        constraint_ = std::exchange(other.constraint_, nullptr);
        charged_ = std::exchange(other.charged_, 0);
        return *this;
    }

    ~registered_memory_arena() noexcept { refund_(); }

    /**
     *  @brief Allocates, charges and registers one arena against @p disk.
     *  @param[inout] disk The ring the arena is pinned in, which must outlive the arena.
     *  @param[inout] constraint The budget the whole arena is charged to, once.
     *  @param[in] config Bytes and placement, both fixed from here on.
     *  @return The arena, or @c out_of_memory_k when the budget or the allocator refuses it.
     */
    template <typename disk_type_>
        requires ring_concept<disk_type_>
    static expected<registered_memory_arena> make(disk_type_& disk, constraint_t& constraint,
                                                  memory_arena_config_t const& config) noexcept {
        if (config.bytes == 0) return status_t {status_code_t::invalid_argument_k, "An arena holds at least one block"};
        if (config.placement == memory_space_t::gpu_k)
            return status_t {status_code_t::unsupported_k, "Device memory has no arena yet"};

        std::size_t const bytes = static_cast<std::size_t>(round_up_to_multiple(config.bytes, portable_alignment_k));
        registered_memory_arena arena;
        if (!constraint.acquire(bytes)) return status_t {status_code_t::out_of_memory_k, "The budget refused an arena"};
        arena.constraint_ = &constraint;
        arena.charged_ = bytes;
        if (arena.storage_.resize(bytes) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "An arena of that size does not fit"};

        auto [pinned, register_memory_status] = disk.register_memory(std::span<byte_t> {arena.storage_},
                                                                     memory_space_t::host_k);
        if (register_memory_status.failed()) return register_memory_status;
        arena.registered_memory_ = std::move(pinned);
        return arena;
    }

    /** Unpins the arena, refusing while any request against it is still in flight. */
    template <typename disk_type_>
        requires ring_concept<disk_type_>
    status_t unregister(disk_type_& disk) noexcept {
        if (!registered_memory_.held()) return {};
        return disk.unregister_memory(registered_memory_);
    }

    /** The whole range, which only the pools carved out of it address. */
    [[nodiscard]] std::span<byte_t> bytes() noexcept { return std::span<byte_t> {storage_}; }

    /** Bytes the arena holds. */
    [[nodiscard]] std::uint64_t size() const noexcept { return storage_.size(); }

    /** Whether the arena is still pinned. */
    [[nodiscard]] bool registered() const noexcept { return registered_memory_.held(); }

    /** The place @p offset bytes into the arena, which a request carries instead of a pointer. */
    [[nodiscard]] memory_reference_t reference(std::uint64_t offset) const noexcept {
        return registered_memory_.reference(offset);
    }

    /** The registered range over the whole arena, which the channels carve their blocks out of. */
    [[nodiscard]] registered_memory_t const& registration() const noexcept { return registered_memory_; }

  private:

    /** The bytes, aligned so a mount demanding the portable alignment can address offset zero. */
    using storage_t = st::basic_vector<byte_t, aligned_allocator<byte_t, portable_alignment_k>>;

    /** Returns the charge to the budget, which both the destructor and a move assignment need. */
    void refund_() noexcept {
        if (constraint_ != nullptr && charged_ != 0) {
            [[maybe_unused]] bool const returned = constraint_->release(charged_);
        }
        constraint_ = nullptr;
        charged_ = 0;
    }

    /** The bytes of the arena, owned here and never moved once registered. */
    storage_t storage_;

    /** The one registered memory over @c storage_, surrendered by @c unregister. */
    registered_memory_t registered_memory_;

    /** The budget @c storage_ is charged to, null until the arena is mounted. */
    constraint_t* constraint_ = nullptr;

    /** Bytes charged to @c constraint_, returned when the arena dies. */
    std::size_t charged_ = 0;
};

#pragma endregion Arena

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
 *  @brief One block borrowed from a pool, which resolves to a place and to a span of the same bytes.
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

    /** The place @p offset bytes into the block, which a request carries instead of a pointer. */
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
     *  @param[in] range The registered range the blocks address, which must outlive the pool.
     *  @param[in] region_offset First byte of this pool's region inside the range.
     *  @param[in] config Block size and block count, both fixed from here on.
     */
    static expected<buffer_pool_t> make(registered_memory_t const& range, std::uint64_t region_offset,
                                        buffer_pool_config_t const& config) noexcept {
        if (config.buffer_bytes == 0 || config.buffers_count == 0)
            return status_t {status_code_t::invalid_argument_k, "A pool holds at least one block"};
        std::uint64_t const span = config.buffer_bytes * config.buffers_count;
        std::uint64_t const range_bytes = range.bytes().size();
        if (region_offset > range_bytes || span > range_bytes - region_offset)
            return status_t {status_code_t::invalid_argument_k, "A pool's region runs past its arena"};

        buffer_pool_t pool;
        pool.range_ = &range;
        pool.region_offset_ = region_offset;
        pool.buffer_bytes_ = config.buffer_bytes;
        if (pool.free_indices_.resize(config.buffers_count) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A pool could not track its blocks"};
        for (std::uint32_t index = 0; index != config.buffers_count; ++index)
            pool.free_indices_[index] = config.buffers_count - 1 - index;
        pool.free_count_ = config.buffers_count;
        return pool;
    }

    /** Takes one block, or answers @c capacity_exhausted_k when every block of this channel is out. */
    expected<pooled_buffer_t> acquire() noexcept {
        if (free_count_ == 0)
            return status_t {status_code_t::capacity_exhausted_k, "Every block of this channel is out"};
        std::uint32_t const index = free_indices_[--free_count_];
        pooled_buffer_t buffer;
        buffer.pool_ = this;
        buffer.index_ = index;
        buffer.bytes_ = range_->bytes().subspan(static_cast<std::size_t>(region_offset_ + index * buffer_bytes_),
                                                static_cast<std::size_t>(buffer_bytes_));
        return buffer;
    }

    /** Blocks nobody holds, which a submit loop reads to decide how much more to offer. */
    [[nodiscard]] std::uint32_t available_buffers() const noexcept { return free_count_; }

    /** Blocks the pool was carved with. */
    [[nodiscard]] std::uint32_t total_buffers() const noexcept {
        return static_cast<std::uint32_t>(free_indices_.size());
    }

    /** Bytes of one block, which is the unit every transfer is split into. */
    [[nodiscard]] std::uint64_t buffer_bytes() const noexcept { return buffer_bytes_; }

    /** Whether the pool was ever carved, which an unmounted device pool is not. */
    [[nodiscard]] bool made() const noexcept { return range_ != nullptr; }

  private:

    friend class pooled_buffer_t;

    /** Takes @p index back, which only a queue pair's own release calls. */
    void recycle_(std::uint32_t index) noexcept {
        if (free_count_ < free_indices_.size()) free_indices_[free_count_++] = index;
    }

    /** The place of the block at @p index, @p offset bytes in. */
    [[nodiscard]] memory_reference_t place_(std::uint32_t index, std::uint64_t offset) const noexcept {
        return range_->reference(region_offset_ + index * buffer_bytes_ + offset);
    }

    /** The registered range the blocks live in, null until the pool is made. */
    registered_memory_t const* range_ = nullptr;

    /** Where the pool's first block sits inside the range, so a queue pair can mint its place. */
    std::uint64_t region_offset_ = 0;

    /** Bytes of one block. */
    std::uint64_t buffer_bytes_ = 0;

    /** Indices of the blocks nobody holds, read and written as a stack by the owning channel alone. */
    st::basic_vector<std::uint32_t> free_indices_;

    /** Entries of @c free_indices_ that are live, which is the stack's height. */
    std::uint32_t free_count_ = 0;
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

/** What a mount carves each channel's share of the arena into. */
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

    /** Bytes one channel's share of the arena takes, derived from the four numbers above. */
    [[nodiscard]] constexpr std::uint64_t arena_bytes() const noexcept {
        return request_buffer_bytes * request_buffers_count + stream_buffer_bytes * stream_buffers_count;
    }
};

#pragma endregion Shape

#pragma region Channel

template <typename disk_type_, template <typename> typename atomic_reference_>
    requires ring_concept<disk_type_>
class basic_io_channel_pool;

/**
 *  @brief One queue pair and everything the thread holding it needs, owned by the pool that opened it.
 *
 *  A channel is opened at mount and released at unmount; callers borrow one and never open one, which
 *  is what keeps the number of queue pairs equal to the number of queue pairs a controller offers rather
 *  than to the number of objects in the engine.
 */
class io_channel_t {
  public:

    io_channel_t() noexcept = default;
    io_channel_t(io_channel_t const&) = delete;
    io_channel_t& operator=(io_channel_t const&) = delete;
    io_channel_t(io_channel_t&&) noexcept = default;
    io_channel_t& operator=(io_channel_t&&) noexcept = default;

    /** The queue pair this channel submits and polls on. */
    [[nodiscard]] queue_pair_t const& queue_pair() const noexcept { return queue_pair_; }

    /** The blocks a transfer of ordinary size is read into or written from. */
    [[nodiscard]] buffer_pool_t& requests() noexcept { return requests_; }

    /** The larger blocks a streaming read or write moves, for an index, a merge or a backup. */
    [[nodiscard]] buffer_pool_t& streams() noexcept { return streams_; }

    /** Room the channel polls into, sized to the queue pair's depth so one poll never truncates. */
    [[nodiscard]] std::span<completion_t> completion_buffer() noexcept {
        return std::span<completion_t> {completions_};
    }

    /** The next token of this channel, whose tag is unique among its in-flight requests. */
    [[nodiscard]] request_token_t next_token() noexcept { return queue_pair_.token(next_tag_++); }

    /** A token of this channel carrying the caller's own @p tag. */
    [[nodiscard]] request_token_t token(request_tag_t tag) const noexcept { return queue_pair_.token(tag); }

  private:

    template <typename disk_type_, template <typename> typename atomic_reference_>
        requires ring_concept<disk_type_>
    friend class basic_io_channel_pool;

    /** The queue pair, taken at mount and given back at unmount. */
    queue_pair_t queue_pair_;

    /** This channel's share of the request pool, with a free list nobody else touches. */
    buffer_pool_t requests_;

    /** This channel's share of the streaming pool. */
    buffer_pool_t streams_;

    /** Room for one poll, sized to the queue pair's depth. */
    st::basic_vector<completion_t> completions_;

    /** The tag the next request of this channel carries, wrapping harmlessly past the depth. */
    request_tag_t next_tag_ = 0;
};

/**
 *  @brief A checked-out channel, drained and returned when the guard dies.
 *
 *  Draining in the destructor is what makes the queue pair safe to hand to the next caller: the ring
 *  refuses to release a queue pair with work outstanding, and a block cannot be recycled while the device
 *  may still be writing into it.
 *
 *  @tparam atomic_reference_ The reference a shared channel's claim word is owned through.
 */
template <typename disk_type_, template <typename> typename atomic_reference_>
    requires ring_concept<disk_type_>
class basic_io_channel_guard {
  public:

    basic_io_channel_guard() noexcept = default;
    basic_io_channel_guard(basic_io_channel_guard const&) = delete;
    basic_io_channel_guard& operator=(basic_io_channel_guard const&) = delete;

    basic_io_channel_guard(basic_io_channel_guard&& other) noexcept
        : disk_(std::exchange(other.disk_, nullptr)), channel_(std::exchange(other.channel_, nullptr)),
          claim_(std::exchange(other.claim_, nullptr)) {}

    basic_io_channel_guard& operator=(basic_io_channel_guard&& other) noexcept {
        if (this == &other) return *this;
        surrender_();
        disk_ = std::exchange(other.disk_, nullptr);
        channel_ = std::exchange(other.channel_, nullptr);
        claim_ = std::exchange(other.claim_, nullptr);
        return *this;
    }

    ~basic_io_channel_guard() noexcept { surrender_(); }

    /** Builds the guard a pool hands back; callers never call this. */
    [[nodiscard]] static basic_io_channel_guard held(disk_type_& disk, io_channel_t& channel,
                                                     std::int32_t* claim) noexcept {
        basic_io_channel_guard guard;
        guard.disk_ = &disk;
        guard.channel_ = &channel;
        guard.claim_ = claim;
        return guard;
    }

    /** Whether this guard holds a channel. */
    [[nodiscard]] bool holding() const noexcept { return channel_ != nullptr; }

    /** The channel, which only the thread holding this guard submits and polls on. */
    [[nodiscard]] io_channel_t& channel() const noexcept { return *channel_; }

    /** The disk the channel submits against. */
    [[nodiscard]] disk_type_& disk() const noexcept { return *disk_; }

    /** Stops the channel's unstarted requests when @p job has been cancelled, polled in a poll loop. */
    template <typename cancellation_type_>
    void cancel_if(cancellation_type_ const& job) noexcept {
        if (channel_ && job.cancelled()) disk_->cancel(channel_->queue_pair());
    }

  private:

    /** Drains the channel and frees its claim, which both the destructor and a move assignment need. */
    void surrender_() noexcept {
        if (channel_ && disk_) [[maybe_unused]]
            status_t const drained = disk_->drain(channel_->queue_pair());
        if (claim_) atomic_reference_<std::int32_t> {*claim_}.store(0, std::memory_order_release);
        disk_ = nullptr;
        channel_ = nullptr;
        claim_ = nullptr;
    }

    /** The disk the channel belongs to, null once moved from. */
    disk_type_* disk_ = nullptr;

    /** The channel this guard holds, null once moved from. */
    io_channel_t* channel_ = nullptr;

    /** The word a shared channel is claimed through, null for a channel owned by one executor thread. */
    std::int32_t* claim_ = nullptr;
};

#pragma endregion Channel

#pragma region Pool

/**
 *  @brief Every queue pair one disk hands out, opened at mount and lent one operation at a time.
 *
 *  Executor threads take their own channel by thread index, so the common path claims nothing. Threads
 *  outside the pool claim one of the shared channels and are refused with @c capacity_exhausted_k when
 *  every shared is out, which is the same back-off the job gate already asks for.
 *
 *  @tparam atomic_reference_ The reference the shared claim words are owned through.
 */
template <typename disk_type_, template <typename> typename atomic_reference_>
    requires ring_concept<disk_type_>
class basic_io_channel_pool {
  public:

    using guard_t = basic_io_channel_guard<disk_type_, atomic_reference_>;

    basic_io_channel_pool() noexcept = default;
    basic_io_channel_pool(basic_io_channel_pool const&) = delete;
    basic_io_channel_pool& operator=(basic_io_channel_pool const&) = delete;
    basic_io_channel_pool(basic_io_channel_pool&&) noexcept = default;
    basic_io_channel_pool& operator=(basic_io_channel_pool&&) noexcept = default;

    /**
     *  @brief Opens one channel per executor thread plus @p shared_io_channels more, over @p range.
     *  @param[inout] disk The ring every channel queue pairs from, which must outlive the pool.
     *  @param[in] range The registered range the channels carve their blocks out of, outliving the pool.
     *  @param[in] threads Executor threads, each of which gets a channel of its own.
     *  @param[in] shared_io_channels Channels threads outside the executor claim, at least one.
     *  @param[in] config What each channel's share of the arena is carved into.
     */
    static expected<basic_io_channel_pool> make(disk_type_& disk, registered_memory_t const& range, std::size_t threads,
                                                std::size_t shared_io_channels,
                                                io_channel_config_t const& config) noexcept {
        if (shared_io_channels == 0)
            return status_t {status_code_t::invalid_argument_k, "A mount keeps a channel for its own threads"};
        std::size_t const wanted = threads + shared_io_channels;
        std::uint64_t const per_channel = config.arena_bytes();
        if (per_channel == 0 || range.bytes().size() / per_channel < wanted)
            return status_t {status_code_t::capacity_exhausted_k, "The arena does not hold a block for every channel"};

        basic_io_channel_pool pool;
        pool.disk_ = &disk;
        pool.executor_channels_ = threads;
        if (pool.channels_.resize(wanted) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A mount could not track its channels"};
        if (pool.shared_claims_.resize(shared_io_channels) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A mount could not track its shared channels"};
        for (std::size_t index = 0; index != shared_io_channels; ++index) pool.shared_claims_[index] = 0;

        for (std::size_t index = 0; index != wanted; ++index) {
            io_channel_t& channel = pool.channels_[index];
            auto [leased, acquire_queue_pair_status] = disk.acquire_queue_pair(config.depth);
            if (acquire_queue_pair_status.failed()) return acquire_queue_pair_status;
            channel.queue_pair_ = std::move(leased);
            if (channel.completions_.resize(config.depth) != st::status_t::success_k)
                return status_t {status_code_t::out_of_memory_k, "A channel could not size its polling room"};

            std::uint64_t const base = per_channel * index;
            buffer_pool_config_t requests;
            requests.buffer_bytes = config.request_buffer_bytes;
            requests.buffers_count = config.request_buffers_count;
            auto [request_pool, make_request_pool_status] = buffer_pool_t::make(range, base, requests);
            if (make_request_pool_status.failed()) return make_request_pool_status;
            channel.requests_ = std::move(request_pool);

            buffer_pool_config_t streams;
            streams.buffer_bytes = config.stream_buffer_bytes;
            streams.buffers_count = config.stream_buffers_count;
            std::uint64_t const stream_base = base + config.request_buffer_bytes * config.request_buffers_count;
            auto [stream_pool, make_stream_pool_status] = buffer_pool_t::make(range, stream_base, streams);
            if (make_stream_pool_status.failed()) return make_stream_pool_status;
            channel.streams_ = std::move(stream_pool);
        }
        return pool;
    }

    /** Drains and releases every queue pair, which a mount does before it deregisters its arena. */
    status_t release_all() noexcept {
        status_t first;
        for (std::size_t index = 0; index != channels_.size(); ++index) {
            io_channel_t& channel = channels_[index];
            if (!channel.queue_pair_.held()) continue;
            [[maybe_unused]] status_t const drained = disk_->drain(channel.queue_pair_);
            if (status_t const released = disk_->release_queue_pair(channel.queue_pair_);
                released.failed() && first.succeeded())
                first = released;
        }
        return first;
    }

    /** The channel of executor thread @p thread_index, which nobody else takes, or a shared one as @c acquire takes
     *  for an index past the executor's threads. */
    expected<guard_t> acquire(std::size_t thread_index) noexcept {
        if (thread_index >= executor_channels_) return acquire();
        io_channel_t& channel = channels_[thread_index];
        if (status_t const adopted = disk_->bind_queue_pair(channel.queue_pair_); adopted.failed()) return adopted;
        return guard_t::held(*disk_, channel, nullptr);
    }

    /** The first free shared channel, for a thread outside the executor, or @c capacity_exhausted_k when every
     *  shared is out. */
    expected<guard_t> acquire() noexcept {
        for (std::size_t index = 0; index != shared_claims_.size(); ++index) {
            std::int32_t expected_free = 0;
            atomic_reference_<std::int32_t> claim {shared_claims_[index]};
            if (!claim.compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            io_channel_t& channel = channels_[executor_channels_ + index];
            if (status_t const adopted = disk_->bind_queue_pair(channel.queue_pair_); adopted.failed()) {
                claim.store(0, std::memory_order_release);
                return adopted;
            }
            return guard_t::held(*disk_, channel, &shared_claims_[index]);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every shared channel of this mount is out"};
    }

    /** Channels the mount opened, which is the executor's threads plus the shared_channels. */
    [[nodiscard]] std::size_t size() const noexcept { return channels_.size(); }

    /** Executor threads with a channel of their own, indexed by the thread. */
    [[nodiscard]] std::size_t executor_channels() const noexcept { return executor_channels_; }

  private:

    /** The disk every channel queue pairs from, null until the pool is made. */
    disk_type_* disk_ = nullptr;

    /** One channel per executor thread first, then the shared_channels. */
    st::basic_vector<io_channel_t> channels_;

    /** One word per shared channel, zero while free and one while claimed. */
    st::basic_vector<std::int32_t> shared_claims_;

    /** Channels reserved for executor threads, which is where the shared channels start. */
    std::size_t executor_channels_ = 0;
};

#pragma endregion Pool

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
    return {status_code_t::io_k, "The device moved fewer bytes than the request asked for"};
}

#pragma endregion Polling

#pragma region Reading

/**
 *  @brief Keeps a channel's ring as full as its blocks allow, polling to make room rather than waiting.
 *
 *  A caller offers transfers, flushes them to the ring and polls; the sink sees each block while
 *  the channel still owns it, copies what it needs out, and the block is recycled the moment the sink
 *  returns. A tag is the caller's own, so a sink can index whatever plan it built.
 */
template <typename disk_type_>
    requires ring_concept<disk_type_>
class basic_read_pipeline {
  public:

    basic_read_pipeline(basic_read_pipeline const&) = delete;
    basic_read_pipeline& operator=(basic_read_pipeline const&) = delete;

    /** A pipeline over @p channel against @p disk, both of which must outlive it. */
    basic_read_pipeline(disk_type_& disk, io_channel_t& channel, medium_reference_t medium) noexcept
        : disk_(disk), channel_(channel), medium_(medium) {}

    /**
     *  @brief Whether another offer would find a block, credit and room in the window, without taking any.
     *
     *  The window runs from the oldest unretired offer to the newest and never reaches the queue pair's depth,
     *  so a caller tagging offers consecutively may index a table of the depth by tag modulo depth.
     */
    [[nodiscard]] bool can_enqueue() const noexcept {
        return channel_.requests().available_buffers() != 0 && entries_.size() < channel_.queue_pair().depth();
    }

    /** Queues a read of @p extent under @p tag, or answers @c capacity_exhausted_k when out of room. */
    status_t enqueue_read(extent_t extent, request_tag_t tag) noexcept {
        if (!can_enqueue()) return {status_code_t::capacity_exhausted_k, "This channel has no block or credit left"};
        auto [block, acquire_status] = channel_.requests().acquire();
        if (acquire_status.failed()) return acquire_status;
        entry_t entry;
        entry.block = std::move(block);
        entry.tag = tag;
        entry.requested_bytes = extent.length;
        request_t const request = read_request(medium_, extent, entry.block.reference(), channel_.token(next_slot_));
        if (staged_.push_back(request_t {request}) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A pipeline could not stage another request"};
        if (entries_.push_back(std::move(entry)) != st::status_t::success_k) {
            staged_.pop_back();
            return {status_code_t::out_of_memory_k, "A pipeline could not track another request"};
        }
        ++next_slot_;
        return {};
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

    /** Requests offered and not yet polled, staged ones included. */
    [[nodiscard]] std::size_t outstanding() const noexcept { return outstanding_ + staged_.size(); }

    /**
     *  @brief Harvests every completion that has landed, calling @p sink once per block.
     *  @param[in] sink Called with a @c completed_buffer_t while the channel still owns the block.
     *  @return How many blocks the sink saw, which is zero when nothing has landed yet.
     */
    template <typename sink_type_>
    expected<std::size_t> poll_completions(sink_type_&& sink) noexcept {
        if (outstanding_ == 0) return std::size_t {0};
        auto [completed, poll_completions_status] = disk_.poll_completions(channel_.queue_pair(),
                                                                           channel_.completion_buffer());
        if (poll_completions_status.failed()) return poll_completions_status;
        std::span<completion_t const> const landed = channel_.completion_buffer().first(completed);
        for (completion_t const& completion : landed) {
            if (!completion.retires()) continue;
            entry_t* const entry = entry_of_(completion.token.tag());
            if (!entry) continue;
            completed_buffer_t block;
            block.tag = entry->tag;
            block.state = completion.state;
            block.status = completion.status;
            block.requested_bytes = entry->requested_bytes;
            block.bytes = std::span<byte_t const> {entry->block.bytes().data(),
                                                   static_cast<std::size_t>(completion.transferred_bytes)};
            sink(block);
            entry->block.release();
            entry->live = false;
            --outstanding_;
        }
        compact_();
        return landed.size();
    }

    /** Harvests until at least one block is seen, which is how an exhausted pool makes room. */
    template <typename sink_type_>
    status_t poll_one_completion(sink_type_&& sink) noexcept {
        if (outstanding_ == 0) return {status_code_t::invalid_argument_k, "This channel has nothing in flight"};
        std::size_t const before = outstanding_;
        for (std::size_t spins = 0; outstanding_ == before && spins != poll_spin_limit_k; ++spins) {
            auto [seen, poll_completions_status] = poll_completions(sink);
            if (poll_completions_status.failed()) return poll_completions_status;
        }
        return outstanding_ == before ? status_t {status_code_t::io_k, "A channel's completions never arrived"}
                                      : status_t {};
    }

    /** Harvests everything outstanding, which a caller does before it drops the pipeline. */
    template <typename sink_type_>
    status_t poll_all_completions(sink_type_&& sink) noexcept {
        // Counts what is staged as well as what is submitted, so an offer that was never flushed is
        // still driven to the device rather than leaving the caller reading nothing.
        while (outstanding() != 0) {
            if (status_t const flushed = submit(); flushed.failed()) return flushed;
            if (status_t const one = poll_one_completion(sink); one.failed()) return one;
        }
        return {};
    }

  private:

    /** One offered request and the block it fills, retired when its completion is polled. */
    struct entry_t {

        /** The block the device writes into, recycled after the sink has seen it. */
        pooled_buffer_t block;

        /** The caller's tag, which the sink matches its own bookkeeping on. */
        request_tag_t tag = 0;

        /** Bytes the request asked for, against which a partial transfer is measured. */
        std::uint64_t requested_bytes = 0;

        /** Whether this entry still awaits its completion. */
        bool live = true;
    };

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
    static void drop_front_(st::basic_vector<element_type_>& elements, std::size_t count) noexcept {
        for (std::size_t index = count; index != elements.size(); ++index)
            elements[index - count] = std::move(elements[index]);
        for (std::size_t index = 0; index != count; ++index) elements.pop_back();
    }

    /** The disk this pipeline submits against. */
    disk_type_& disk_;

    /** The channel this pipeline submits and polls on. */
    io_channel_t& channel_;

    /** The medium every offer reads from. */
    medium_reference_t medium_;

    /** One entry per offered request, in the order they were offered. */
    st::basic_vector<entry_t> entries_;

    /** Requests built but not yet taken by the ring. */
    st::basic_vector<request_t> staged_;

    /** Requests the ring has taken and not yet handed back. */
    std::size_t outstanding_ = 0;

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
    requires ring_concept<disk_type_>
class basic_write_pipeline {
  public:

    basic_write_pipeline(basic_write_pipeline const&) = delete;
    basic_write_pipeline& operator=(basic_write_pipeline const&) = delete;

    /** A pipeline over @p channel against @p disk, both of which must outlive it. */
    basic_write_pipeline(disk_type_& disk, io_channel_t& channel, medium_reference_t medium) noexcept
        : disk_(disk), channel_(channel), medium_(medium) {}

    /** Takes a block to fill, or answers @c capacity_exhausted_k until a poll_completions frees one. */
    expected<pooled_buffer_t> acquire_buffer() noexcept { return channel_.requests().acquire(); }

    /** Takes a streaming block to fill, for a merge or a backup that moves whole megabytes. */
    expected<pooled_buffer_t> acquire_stream_buffer() noexcept { return channel_.streams().acquire(); }

    /** Takes a block, polling completions first when every block of the channel is out. */
    expected<pooled_buffer_t> acquire_buffer_or_poll() noexcept {
        expected<pooled_buffer_t> block = acquire_buffer();
        while (block == status_code_t::capacity_exhausted_k && outstanding_ != 0) {
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
        request_t const request = write_request(medium_, extent, block.reference(), channel_.token(next_slot_));
        if (staged_.push_back(request_t {request}) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A pipeline could not stage another write"};
        entry_t entry;
        entry.block = std::move(block);
        entry.requested_bytes = extent.length;
        if (entries_.push_back(std::move(entry)) != st::status_t::success_k) {
            staged_.pop_back();
            return {status_code_t::out_of_memory_k, "A pipeline could not track another write"};
        }
        ++next_slot_;
        return {};
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

    /**
     *  @brief Reaps every write, then flushes the device and polls that, which is the barrier.
     *
     *  @note The poll before the flush is what gives the flush meaning, since a flush covers only
     *      what its queue pair has already completed.
     */
    status_t make_durable() noexcept {
        while (outstanding() != 0) {
            if (status_t const flushed = submit(); flushed.failed()) return flushed;
            if (status_t const polled = poll_one_completion(); polled.failed()) return polled;
        }

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

    /** What a write that moved less than it asked for means, which is never a retry. */
    static status_t short_write_status_(completion_t const& completion) noexcept {
        if (completion.state == completion_state_t::cancelled_k)
            return {status_code_t::uninitialized_k, "The queue_pair cancelled this write"};
        if (completion.status.failed()) return completion.status;
        return {status_code_t::capacity_exhausted_k, "The volume took fewer bytes than the write asked for"};
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
    static void drop_front_(st::basic_vector<element_type_>& elements, std::size_t count) noexcept {
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
    st::basic_vector<entry_t> entries_;

    /** Writes built but not yet taken by the ring. */
    st::basic_vector<request_t> staged_;

    /** Writes the ring has taken and not yet handed back. */
    std::size_t outstanding_ = 0;

    /** The ring tag the next write takes, which rises for the life of the pipeline. */
    request_tag_t next_slot_ = 0;
};

#pragma endregion Writing

#pragma endregion IO Pipeline

} // namespace unum::ustore::utilities
