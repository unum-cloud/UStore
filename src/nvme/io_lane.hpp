/**
 *  @file src/nvme/io_lane.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief One lease, the blocks it fills and the completions it reaps, checked out for one operation.
 *
 *  The ring's rule binds these three together: the thread that submits must be the thread that reaps,
 *  and the blocks in flight must not be touched by anyone else until it does. So they live in one
 *  object, opened at mount and borrowed by callers, rather than being held by a collection or an LSM
 *  tree - all of which outlive threads and cross them, which a lease may not do.
 *
 *  An executor thread takes its own lane by the thread index every fan-out already passes it, so the
 *  common path contends with nobody. A thread outside the pool claims one of a few spare lanes, or is
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

#include "nvme/buffer_pool.hpp" // `buffer_pool_t`, `registered_slab_t`
#include "nvme/disk.hpp"        // `issuing_thread_identity`
#include "nvme/ring.hpp"        // `ring_owner_t`, `completion_t`, `ring_concept`

namespace unum::ustore::nvme {

#pragma region Shape

/** What a mount carves each lane's share of the slab into. */
struct lane_config_t {

    /** Bytes of one block in the request pool, which every ordinary transfer is split into. */
    std::uint64_t request_block_bytes = 64ull * 1024ull;

    /** Blocks in the request pool, which bounds what one lane keeps in flight. */
    std::uint32_t request_blocks_count = 128;

    /** Bytes of one block in the streaming pool, for an index, a merge or a backup. */
    std::uint64_t stream_block_bytes = 1024ull * 1024ull;

    /** Blocks in the streaming pool, which is the depth a merge writes at. */
    std::uint32_t stream_blocks_count = 4;

    /** Requests one lease keeps in flight, which is also the room one reap needs. */
    std::uint32_t depth = 64;

    /** Bytes one lane's share of the slab takes, derived from the four numbers above. */
    [[nodiscard]] constexpr std::uint64_t slab_bytes() const noexcept {
        return request_block_bytes * request_blocks_count + stream_block_bytes * stream_blocks_count;
    }
};

#pragma endregion Shape

#pragma region Lane

template <typename disk_type_>
    requires ring_concept<disk_type_>
class basic_lane_pool;

/**
 *  @brief One lease and everything the thread holding it needs, owned by the pool that opened it.
 *
 *  A lane is opened at mount and released at unmount; callers borrow one and never open one, which
 *  is what keeps the number of leases equal to the number of queue pairs a controller offers rather
 *  than to the number of objects in the engine.
 */
class io_lane_t {
  public:

    io_lane_t() noexcept = default;
    io_lane_t(io_lane_t const&) = delete;
    io_lane_t& operator=(io_lane_t const&) = delete;
    io_lane_t(io_lane_t&&) noexcept = default;
    io_lane_t& operator=(io_lane_t&&) noexcept = default;

    /** The lease this lane submits and reaps on. */
    [[nodiscard]] ring_owner_t const& owner() const noexcept { return owner_; }

    /** The blocks a transfer of ordinary size is read into or written from. */
    [[nodiscard]] buffer_pool_t& requests() noexcept { return requests_; }

    /** The larger blocks a streaming read or write moves, for an index, a merge or a backup. */
    [[nodiscard]] buffer_pool_t& streams() noexcept { return streams_; }

    /** Room the lane reaps into, sized to the lease's depth so one reap never truncates. */
    [[nodiscard]] std::span<completion_t> reaping_room() noexcept { return std::span<completion_t> {completions_}; }

    /** The next token of this lane, whose tag is unique among its in-flight requests. */
    [[nodiscard]] request_token_t next_token() noexcept { return owner_.token(next_tag_++); }

    /** A token of this lane carrying the caller's own @p tag. */
    [[nodiscard]] request_token_t token(std::uint32_t tag) const noexcept { return owner_.token(tag); }

  private:

    template <typename disk_type_>
        requires ring_concept<disk_type_>
    friend class basic_lane_pool;

    /** The lease, taken at mount and given back at unmount. */
    ring_owner_t owner_;

    /** This lane's share of the request pool, with a free list nobody else touches. */
    buffer_pool_t requests_;

    /** This lane's share of the streaming pool. */
    buffer_pool_t streams_;

    /** Room for one reap, sized to the lease's depth. */
    st::basic_vector<completion_t> completions_;

    /** The tag the next request of this lane carries, wrapping harmlessly past the depth. */
    std::uint32_t next_tag_ = 0;

    /** The thread that last adopted this lane, a hint the spare checkout ranks by and never trusts. */
    std::uint64_t issuer_ = 0;
};

/**
 *  @brief A checked-out lane, drained and returned when the guard dies.
 *
 *  Draining in the destructor is what makes the lease safe to hand to the next caller: the ring
 *  refuses to release a lease with work outstanding, and a block cannot be recycled while the device
 *  may still be writing into it.
 */
template <typename disk_type_>
    requires ring_concept<disk_type_>
class basic_lane_guard {
  public:

    basic_lane_guard() noexcept = default;
    basic_lane_guard(basic_lane_guard const&) = delete;
    basic_lane_guard& operator=(basic_lane_guard const&) = delete;

    basic_lane_guard(basic_lane_guard&& other) noexcept
        : disk_(std::exchange(other.disk_, nullptr)), lane_(std::exchange(other.lane_, nullptr)),
          claim_(std::exchange(other.claim_, nullptr)) {}

    basic_lane_guard& operator=(basic_lane_guard&& other) noexcept {
        if (this == &other) return *this;
        surrender_();
        disk_ = std::exchange(other.disk_, nullptr);
        lane_ = std::exchange(other.lane_, nullptr);
        claim_ = std::exchange(other.claim_, nullptr);
        return *this;
    }

    ~basic_lane_guard() noexcept { surrender_(); }

    /** Builds the guard a pool hands back; callers never call this. */
    [[nodiscard]] static basic_lane_guard held(disk_type_& disk, io_lane_t& lane, std::int32_t* claim) noexcept {
        basic_lane_guard guard;
        guard.disk_ = &disk;
        guard.lane_ = &lane;
        guard.claim_ = claim;
        return guard;
    }

    /** Whether this guard holds a lane. */
    [[nodiscard]] bool holding() const noexcept { return lane_ != nullptr; }

    /** The lane, which only the thread holding this guard submits and reaps on. */
    [[nodiscard]] io_lane_t& lane() const noexcept { return *lane_; }

    /** The disk the lane submits against. */
    [[nodiscard]] disk_type_& disk() const noexcept { return *disk_; }

    /** Stops the lane's unstarted requests when @p job has been cancelled, polled in a reap loop. */
    template <typename cancellation_type_>
    void cancel_if(cancellation_type_ const& job) noexcept {
        if (lane_ && job.cancelled()) disk_->cancel(lane_->owner());
    }

  private:

    /** Drains the lane and frees its claim, which both the destructor and a move assignment need. */
    void surrender_() noexcept {
        if (lane_ && disk_) [[maybe_unused]]
            status_t const drained = disk_->drain(lane_->owner());
        if (claim_) fu::preferred_atomic_ref<std::int32_t> {*claim_}.store(0, std::memory_order_release);
        disk_ = nullptr;
        lane_ = nullptr;
        claim_ = nullptr;
    }

    /** The disk the lane belongs to, null once moved from. */
    disk_type_* disk_ = nullptr;

    /** The lane this guard holds, null once moved from. */
    io_lane_t* lane_ = nullptr;

    /** The word a spare lane is claimed through, null for a lane owned by one executor thread. */
    std::int32_t* claim_ = nullptr;
};

#pragma endregion Lane

#pragma region Pool

/**
 *  @brief Which free spares one pass of a checkout may claim, from the cheapest to adopt to any.
 *
 *  Adopting a lane on a thread other than the one that last held it costs an io_uring transport a
 *  ring rebuild, so a checkout tries the tiers in order: a spare this thread bound stays with it, a
 *  spare nobody bound is taken before another thread's is stolen, and any free one ends the search.
 */
enum class lane_preference_t : std::uint8_t {

    /** A free spare whose last adopter is the calling thread, which adopts at no cost. */
    bound_here_k,

    /** A free spare no thread has adopted yet, whose first adoption binds it here. */
    never_bound_k,

    /** Any free spare, which may cost the transport a rebind. */
    any_free_k,
};

/**
 *  @brief Every lease one disk hands out, opened at mount and lent one operation at a time.
 *
 *  Executor threads take their own lane by thread index, so the common path claims nothing. Threads
 *  outside the pool claim one of the spare lanes and are refused with @c capacity_exhausted_k when
 *  every spare is out, which is the same back-off the job gate already asks for.
 */
template <typename disk_type_>
    requires ring_concept<disk_type_>
class basic_lane_pool {
  public:

    using guard_t = basic_lane_guard<disk_type_>;

    basic_lane_pool() noexcept = default;
    basic_lane_pool(basic_lane_pool const&) = delete;
    basic_lane_pool& operator=(basic_lane_pool const&) = delete;
    basic_lane_pool(basic_lane_pool&&) noexcept = default;
    basic_lane_pool& operator=(basic_lane_pool&&) noexcept = default;

    /**
     *  @brief Opens one lane per executor thread plus @p spare_lanes more, over @p slab.
     *  @param[inout] disk The ring every lane leases from, which must outlive the pool.
     *  @param[inout] slab The registered range the lanes carve their blocks out of.
     *  @param[in] threads Executor threads, each of which gets a lane of its own.
     *  @param[in] spare_lanes Lanes threads outside the executor claim, at least one.
     *  @param[in] config What each lane's share of the slab is carved into.
     */
    static expected<basic_lane_pool> make(disk_type_& disk, registered_slab_t& slab, std::size_t threads,
                                          std::size_t spare_lanes, lane_config_t const& config) noexcept {
        if (spare_lanes == 0)
            return status_t {status_code_t::invalid_argument_k, "A mount keeps a lane for its own threads"};
        std::size_t const wanted = threads + spare_lanes;
        std::uint64_t const per_lane = config.slab_bytes();
        if (per_lane == 0 || slab.size() / per_lane < wanted)
            return status_t {status_code_t::capacity_exhausted_k, "The slab does not hold a block for every lane"};

        basic_lane_pool pool;
        pool.disk_ = &disk;
        pool.executor_lanes_ = threads;
        if (pool.lanes_.resize(wanted) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A mount could not track its lanes"};
        if (pool.spare_claims_.resize(spare_lanes) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A mount could not track its spare lanes"};
        for (std::size_t index = 0; index != spare_lanes; ++index) pool.spare_claims_[index] = 0;

        for (std::size_t index = 0; index != wanted; ++index) {
            io_lane_t& lane = pool.lanes_[index];
            expected<ring_owner_t> leased = disk.open(config.depth);
            if (!leased) return leased.status();
            lane.owner_ = std::move(*leased);
            if (lane.completions_.resize(config.depth) != st::status_t::success_k)
                return status_t {status_code_t::out_of_memory_k, "A lane could not size its reaping room"};

            std::uint64_t const base = per_lane * index;
            buffer_pool_config_t requests;
            requests.block_bytes = config.request_block_bytes;
            requests.blocks_count = config.request_blocks_count;
            expected<buffer_pool_t> request_pool = buffer_pool_t::make(slab, base, requests);
            if (!request_pool) return request_pool.status();
            lane.requests_ = std::move(*request_pool);

            buffer_pool_config_t streams;
            streams.block_bytes = config.stream_block_bytes;
            streams.blocks_count = config.stream_blocks_count;
            std::uint64_t const stream_base = base + config.request_block_bytes * config.request_blocks_count;
            expected<buffer_pool_t> stream_pool = buffer_pool_t::make(slab, stream_base, streams);
            if (!stream_pool) return stream_pool.status();
            lane.streams_ = std::move(*stream_pool);
        }
        return pool;
    }

    /** Drains and releases every lease, which a mount does before it deregisters its slab. */
    status_t dismount() noexcept {
        status_t first;
        for (std::size_t index = 0; index != lanes_.size(); ++index) {
            io_lane_t& lane = lanes_[index];
            if (!lane.owner_.held()) continue;
            [[maybe_unused]] status_t const drained = disk_->drain(lane.owner_);
            if (status_t const released = disk_->release(lane.owner_); released.failed() && first.succeeded())
                first = released;
        }
        return first;
    }

    /** The lane of executor thread @p thread_index, which nobody else takes. */
    expected<guard_t> checkout(std::size_t thread_index) noexcept {
        if (thread_index >= executor_lanes_)
            return status_t {status_code_t::invalid_argument_k, "That thread has no lane of its own"};
        io_lane_t& lane = lanes_[thread_index];
        if (status_t const adopted = disk_->adopt(lane.owner_); adopted.failed()) return adopted;
        fu::preferred_atomic_ref<std::uint64_t> {lane.issuer_}.store(issuing_thread_identity(),
                                                                     std::memory_order_relaxed);
        return guard_t::held(*disk_, lane, nullptr);
    }

    /**
     *  @brief A spare lane for a thread outside the executor, or @c capacity_exhausted_k when every spare is out.
     *
     *  The spare this thread last held is preferred, so a host that calls from the same few threads
     *  never pays the transport a rebind after its first call from each; the hint is read before the
     *  claim, so a stale read costs one rebind and never a wrong owner.
     */
    expected<guard_t> checkout() noexcept {
        std::uint64_t const issuer = issuing_thread_identity();
        if (expected<guard_t> claimed = claim_spare_(lane_preference_t::bound_here_k, issuer); !exhausted_(claimed))
            return claimed;
        if (expected<guard_t> claimed = claim_spare_(lane_preference_t::never_bound_k, issuer); !exhausted_(claimed))
            return claimed;
        return claim_spare_(lane_preference_t::any_free_k, issuer);
    }

    /** Lanes the mount opened, which is the executor's threads plus the spares. */
    [[nodiscard]] std::size_t size() const noexcept { return lanes_.size(); }

  private:

    /** Whether @p claimed says that no spare met the pass, as opposed to a spare refusing adoption. */
    [[nodiscard]] static bool exhausted_(expected<guard_t> const& claimed) noexcept {
        return !claimed && claimed.status() == status_code_t::capacity_exhausted_k;
    }

    /** The first free spare @p preference admits for @p issuer, or @c capacity_exhausted_k when none does. */
    expected<guard_t> claim_spare_(lane_preference_t preference, std::uint64_t issuer) noexcept {
        for (std::size_t index = 0; index != spare_claims_.size(); ++index) {
            io_lane_t& lane = lanes_[executor_lanes_ + index];
            fu::preferred_atomic_ref<std::uint64_t> bound {lane.issuer_};
            std::uint64_t const bound_to = bound.load(std::memory_order_relaxed);
            if (preference == lane_preference_t::bound_here_k && bound_to != issuer) continue;
            if (preference == lane_preference_t::never_bound_k && bound_to != 0) continue;
            std::int32_t expected_free = 0;
            fu::preferred_atomic_ref<std::int32_t> claim {spare_claims_[index]};
            if (!claim.compare_exchange_strong(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed))
                continue;
            if (status_t const adopted = disk_->adopt(lane.owner_); adopted.failed()) {
                claim.store(0, std::memory_order_release);
                return adopted;
            }
            bound.store(issuer, std::memory_order_relaxed);
            return guard_t::held(*disk_, lane, &spare_claims_[index]);
        }
        return status_t {status_code_t::capacity_exhausted_k, "Every spare lane of this mount is out"};
    }

    /** The disk every lane leases from, null until the pool is made. */
    disk_type_* disk_ = nullptr;

    /** One lane per executor thread first, then the spares. */
    st::basic_vector<io_lane_t> lanes_;

    /** One word per spare lane, zero while free and one while claimed. */
    st::basic_vector<std::int32_t> spare_claims_;

    /** Lanes reserved for executor threads, which is where the spares start. */
    std::size_t executor_lanes_ = 0;
};

#pragma endregion Pool

} // namespace unum::ustore::nvme
