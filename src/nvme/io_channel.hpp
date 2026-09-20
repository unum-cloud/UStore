/**
 *  @file src/nvme/io_channel.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief One queue pair, the blocks it fills and the completions it polls, checked out for one operation.
 *
 *  The ring's rule binds these three together: the thread that submits must be the thread that polls,
 *  and the blocks in flight must not be touched by anyone else until it does. So they live in one
 *  object, opened at mount and borrowed by callers, rather than being held by a collection or an LSM
 *  tree - all of which outlive threads and cross them, which a queue pair may not do.
 *
 *  An executor thread takes its own channel by the thread index every fan-out already passes it, so the
 *  common path contends with nobody. A thread outside the pool claims one of a few shared channels, or is
 *  refused and backs off, which is the contract the job gate already asks of its callers.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`

#include <span>    // `std::span`
#include <utility> // `std::exchange`, `std::move`

#include <forkunion/atomics.hpp>       // `fu::preferred_atomic_ref`
#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`

#include "nvme/buffer_pool.hpp" // `buffer_pool_t`, `registered_memory_arena_t`
#include "nvme/disk.hpp"        // `calling_thread_identity`
#include "nvme/ring.hpp"        // `queue_pair_t`, `completion_t`, `ring_concept`

namespace unum::ustore::nvme {

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

template <typename disk_type_>
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

    template <typename disk_type_>
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

    /** The thread that last adopted this channel, a hint the shared acquire ranks by and never trusts. */
    thread_identity_t bound_thread_ = 0;
};

/**
 *  @brief A checked-out channel, drained and returned when the guard dies.
 *
 *  Draining in the destructor is what makes the queue pair safe to hand to the next caller: the ring
 *  refuses to release a queue pair with work outstanding, and a block cannot be recycled while the device
 *  may still be writing into it.
 */
template <typename disk_type_>
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
        if (claim_) fu::preferred_atomic_ref<std::int32_t> {*claim_}.store(0, std::memory_order_release);
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
 *  @brief Which free shared channels one pass of a acquire may claim, from the cheapest to adopt to any.
 *
 *  Adopting a channel on a thread other than the one that last held it costs an io_uring transport a
 *  ring rebuild, so a acquire tries the tiers in order: a shared this thread bound stays with it, a
 *  shared nobody bound is taken before another thread's is stolen, and any free one ends the search.
 */
enum class io_channel_preference_t : std::uint8_t {

    /** A free shared whose last adopter is the calling thread, which adopts at no cost. */
    bound_to_this_thread_k,

    /** A free shared no thread has adopted yet, whose first adoption binds it here. */
    never_bound_k,

    /** Any free shared, which may cost the transport a rebind. */
    any_free_k,
};

/**
 *  @brief Every queue pair one disk hands out, opened at mount and lent one operation at a time.
 *
 *  Executor threads take their own channel by thread index, so the common path claims nothing. Threads
 *  outside the pool claim one of the shared channels and are refused with @c capacity_exhausted_k when
 *  every shared is out, which is the same back-off the job gate already asks for.
 */
template <typename disk_type_>
    requires ring_concept<disk_type_>
class basic_io_channel_pool {
  public:

    using guard_t = basic_io_channel_guard<disk_type_>;

    basic_io_channel_pool() noexcept = default;
    basic_io_channel_pool(basic_io_channel_pool const&) = delete;
    basic_io_channel_pool& operator=(basic_io_channel_pool const&) = delete;
    basic_io_channel_pool(basic_io_channel_pool&&) noexcept = default;
    basic_io_channel_pool& operator=(basic_io_channel_pool&&) noexcept = default;

    /**
     *  @brief Opens one channel per executor thread plus @p shared_io_channels more, over @p arena.
     *  @param[inout] disk The ring every channel queue pairs from, which must outlive the pool.
     *  @param[inout] arena The registered range the channels carve their blocks out of.
     *  @param[in] threads Executor threads, each of which gets a channel of its own.
     *  @param[in] shared_io_channels Channels threads outside the executor claim, at least one.
     *  @param[in] config What each channel's share of the arena is carved into.
     */
    static expected<basic_io_channel_pool> make(disk_type_& disk, registered_memory_arena_t& arena, std::size_t threads,
                                                std::size_t shared_io_channels,
                                                io_channel_config_t const& config) noexcept {
        if (shared_io_channels == 0)
            return status_t {status_code_t::invalid_argument_k, "A mount keeps a channel for its own threads"};
        std::size_t const wanted = threads + shared_io_channels;
        std::uint64_t const per_channel = config.arena_bytes();
        if (per_channel == 0 || arena.size() / per_channel < wanted)
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
            expected<queue_pair_t> leased = disk.acquire_queue_pair(config.depth);
            if (!leased) return leased.status();
            channel.queue_pair_ = std::move(*leased);
            if (channel.completions_.resize(config.depth) != st::status_t::success_k)
                return status_t {status_code_t::out_of_memory_k, "A channel could not size its polling room"};

            std::uint64_t const base = per_channel * index;
            buffer_pool_config_t requests;
            requests.buffer_bytes = config.request_buffer_bytes;
            requests.buffers_count = config.request_buffers_count;
            expected<buffer_pool_t> request_pool = buffer_pool_t::make(arena, base, requests);
            if (!request_pool) return request_pool.status();
            channel.requests_ = std::move(*request_pool);

            buffer_pool_config_t streams;
            streams.buffer_bytes = config.stream_buffer_bytes;
            streams.buffers_count = config.stream_buffers_count;
            std::uint64_t const stream_base = base + config.request_buffer_bytes * config.request_buffers_count;
            expected<buffer_pool_t> stream_pool = buffer_pool_t::make(arena, stream_base, streams);
            if (!stream_pool) return stream_pool.status();
            channel.streams_ = std::move(*stream_pool);
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

    /** The channel of executor thread @p thread_index, which nobody else takes. */
    expected<guard_t> acquire(std::size_t thread_index) noexcept {
        if (thread_index >= executor_channels_)
            return status_t {status_code_t::invalid_argument_k, "That thread has no channel of its own"};
        io_channel_t& channel = channels_[thread_index];
        if (status_t const adopted = disk_->bind_queue_pair(channel.queue_pair_); adopted.failed()) return adopted;
        fu::preferred_atomic_ref<std::uint64_t> {channel.bound_thread_}.store(calling_thread_identity(),
                                                                              std::memory_order_relaxed);
        return guard_t::held(*disk_, channel, nullptr);
    }

    /**
     *  @brief A shared channel for a thread outside the executor, or @c capacity_exhausted_k when every shared is out.
     *
     *  The shared this thread last held is preferred, so a host that calls from the same few threads
     *  never pays the transport a rebind after its first call from each; the hint is read before the
     *  claim, so a stale read costs one rebind and never a wrong queue_pair.
     */
    expected<guard_t> acquire() noexcept {
        thread_identity_t const bound_thread = calling_thread_identity();
        if (expected<guard_t> claimed = claim_shared_(io_channel_preference_t::bound_to_this_thread_k, bound_thread);
            !exhausted_(claimed))
            return claimed;
        if (expected<guard_t> claimed = claim_shared_(io_channel_preference_t::never_bound_k, bound_thread);
            !exhausted_(claimed))
            return claimed;
        return claim_shared_(io_channel_preference_t::any_free_k, bound_thread);
    }

    /** Channels the mount opened, which is the executor's threads plus the shared_channels. */
    [[nodiscard]] std::size_t size() const noexcept { return channels_.size(); }

  private:

    /** Whether @p claimed says that no shared met the pass, as opposed to a shared refusing adoption. */
    [[nodiscard]] static bool exhausted_(expected<guard_t> const& claimed) noexcept {
        return !claimed && claimed.status() == status_code_t::capacity_exhausted_k;
    }

    /** The first free shared @p preference admits for @p bound thread, or @c capacity_exhausted_k when none does. */
    expected<guard_t> claim_shared_(io_channel_preference_t preference, thread_identity_t bound_thread) noexcept {
        for (std::size_t index = 0; index != shared_claims_.size(); ++index) {
            io_channel_t& channel = channels_[executor_channels_ + index];
            fu::preferred_atomic_ref<std::uint64_t> bound {channel.bound_thread_};
            std::uint64_t const bound_to = bound.load(std::memory_order_relaxed);
            if (preference == io_channel_preference_t::bound_to_this_thread_k && bound_to != bound_thread) continue;
            if (preference == io_channel_preference_t::never_bound_k && bound_to != 0) continue;
            std::int32_t expected_free = 0;
            fu::preferred_atomic_ref<std::int32_t> claim {shared_claims_[index]};
            if (!claim.compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            if (status_t const adopted = disk_->bind_queue_pair(channel.queue_pair_); adopted.failed()) {
                claim.store(0, std::memory_order_release);
                return adopted;
            }
            bound.store(bound_thread, std::memory_order_relaxed);
            return guard_t::held(*disk_, channel, &shared_claims_[index]);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every shared channel of this mount is out"};
    }

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

} // namespace unum::ustore::nvme
