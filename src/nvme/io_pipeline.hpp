/**
 *  @file src/nvme/io_pipeline.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The submit, reap and refill loops that hold a lane's ring at its depth.
 *
 *  A batch submitted whole and then waited on reaches the depth of that batch and no further, which
 *  is the shape the engine had and the reason it never reached a drive's queue. A pipeline instead
 *  offers until the lane runs out of blocks or credit, harvests what has landed, recycles those
 *  blocks and offers again, so the ring stays full from the first refill to the last offer.
 *
 *  Durability is written as submit, reap, flush, reap, because the ring promises no ordering at all
 *  and a flush covers only what its owner has already reaped.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`

#include "nvme/buffer_pool.hpp" // `buffer_lease_t`
#include "nvme/io_lane.hpp"     // `io_lane_t`
#include "nvme/ring.hpp"        // `request_t`, `completion_t`, `ring_concept`

namespace unum::ustore::nvme {

#pragma region Harvest

/** One block that came back, handed to the sink while the lane still holds its lease. */
struct harvested_block_t {

    /** The tag the caller minted when it offered the transfer, naming its own bookkeeping. */
    std::uint32_t tag = 0;

    /** The bytes the device touched, which is a prefix of the block on a partial transfer. */
    std::span<byte_t const> bytes;

    /** How the transfer ended, which the caller branches on before it trusts @c bytes. */
    completion_state_t state = completion_state_t::completed_k;

    /** Why it ended that way. */
    status_t status;

    /** Bytes the request asked for, against which a partial transfer is measured. */
    std::uint64_t asked = 0;

    /** Whether every byte asked for arrived, which is the only state a block read may trust. */
    [[nodiscard]] bool whole() const noexcept { return state == completion_state_t::completed_k; }
};

/** The status a caller that needs every byte reports for @p reaped, which only a whole move passes. */
inline status_t require_whole(harvested_block_t const& reaped) noexcept {
    if (reaped.state == completion_state_t::completed_k) return {};
    if (reaped.state == completion_state_t::cancelled_k)
        return {status_code_t::uninitialized_k, "The owner cancelled this transfer"};
    if (reaped.status.failed()) return reaped.status;
    return {status_code_t::io_k, "The device moved fewer bytes than the request asked for"};
}

#pragma endregion Harvest

#pragma region Reading

/**
 *  @brief Keeps a lane's ring as full as its blocks allow, harvesting to make room rather than waiting.
 *
 *  A caller offers transfers, flushes them to the ring and harvests; the sink sees each block while
 *  the lane still owns it, copies what it needs out, and the block is recycled the moment the sink
 *  returns. A tag is the caller's own, so a sink can index whatever plan it built.
 */
template <typename disk_type_>
    requires ring_concept<disk_type_>
class basic_read_pipeline {
  public:

    basic_read_pipeline(basic_read_pipeline const&) = delete;
    basic_read_pipeline& operator=(basic_read_pipeline const&) = delete;

    /** A pipeline over @p lane against @p disk, both of which must outlive it. */
    basic_read_pipeline(disk_type_& disk, io_lane_t& lane, target_place_t target) noexcept
        : disk_(disk), lane_(lane), target_(target) {}

    /**
     *  @brief Whether another offer would find a block, credit and room in the window, without taking any.
     *
     *  The window runs from the oldest unretired offer to the newest and never reaches the lease's depth,
     *  so a caller tagging offers consecutively may index a table of the depth by tag modulo depth.
     */
    [[nodiscard]] bool has_room() const noexcept {
        return lane_.requests().available() != 0 && entries_.size() < lane_.owner().depth();
    }

    /** Queues a read of @p extent under @p tag, or answers @c capacity_exhausted_k when out of room. */
    status_t offer(extent_t extent, std::uint32_t tag) noexcept {
        if (!has_room()) return {status_code_t::capacity_exhausted_k, "This lane has no block or credit left"};
        expected<buffer_lease_t> block = lane_.requests().acquire();
        if (!block) return block.status();
        entry_t entry;
        entry.block = std::move(*block);
        entry.tag = tag;
        entry.asked = extent.length;
        request_t const request = read_request(target_, extent, entry.block.place(), lane_.token(next_slot_));
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
    status_t flush() noexcept {
        while (!staged_.empty()) {
            expected<std::size_t> const taken = disk_.submit(lane_.owner(), std::span<request_t const> {staged_});
            if (!taken) return taken.status();
            if (*taken == 0) return {};
            outstanding_ += *taken;
            drop_front_(staged_, *taken);
        }
        return {};
    }

    /** Requests offered and not yet harvested, staged ones included. */
    [[nodiscard]] std::size_t outstanding() const noexcept { return outstanding_ + staged_.size(); }

    /**
     *  @brief Harvests every completion that has landed, calling @p sink once per block.
     *  @param[in] sink Called with a @c harvested_block_t while the lane still owns the block.
     *  @return How many blocks the sink saw, which is zero when nothing has landed yet.
     */
    template <typename sink_type_>
    expected<std::size_t> harvest(sink_type_&& sink) noexcept {
        if (outstanding_ == 0) return std::size_t {0};
        expected<std::size_t> const reaped = disk_.reap(lane_.owner(), lane_.reaping_room());
        if (!reaped) return reaped.status();
        std::span<completion_t const> const landed = lane_.reaping_room().first(*reaped);
        for (completion_t const& completion : landed) {
            if (!completion.retires()) continue;
            entry_t* const entry = entry_of_(completion.token.tag());
            if (!entry) continue;
            harvested_block_t block;
            block.tag = entry->tag;
            block.state = completion.state;
            block.status = completion.status;
            block.asked = entry->asked;
            block.bytes = std::span<byte_t const> {entry->block.bytes().data(),
                                                   static_cast<std::size_t>(completion.transferred)};
            sink(block);
            entry->block.recycle();
            entry->live = false;
            --outstanding_;
        }
        compact_();
        return landed.size();
    }

    /** Harvests until at least one block is seen, which is how an exhausted pool makes room. */
    template <typename sink_type_>
    status_t harvest_one(sink_type_&& sink) noexcept {
        if (outstanding_ == 0) return {status_code_t::invalid_argument_k, "This lane has nothing in flight"};
        std::size_t const before = outstanding_;
        for (std::size_t spins = 0; outstanding_ == before && spins != reap_spin_limit_k; ++spins) {
            expected<std::size_t> const seen = harvest(sink);
            if (!seen) return seen.status();
        }
        return outstanding_ == before ? status_t {status_code_t::io_k, "A lane's completions never arrived"}
                                      : status_t {};
    }

    /** Harvests everything outstanding, which a caller does before it drops the pipeline. */
    template <typename sink_type_>
    status_t harvest_all(sink_type_&& sink) noexcept {
        // Counts what is staged as well as what is submitted, so an offer that was never flushed is
        // still driven to the device rather than leaving the caller reading nothing.
        while (outstanding() != 0) {
            if (status_t const flushed = flush(); flushed.failed()) return flushed;
            if (status_t const one = harvest_one(sink); one.failed()) return one;
        }
        return {};
    }

  private:

    /** One offered request and the block it fills, retired when its completion is harvested. */
    struct entry_t {

        /** The block the device writes into, recycled after the sink has seen it. */
        buffer_lease_t block;

        /** The caller's tag, which the sink matches its own bookkeeping on. */
        std::uint32_t tag = 0;

        /** Bytes the request asked for, against which a partial transfer is measured. */
        std::uint64_t asked = 0;

        /** Whether this entry still awaits its completion. */
        bool live = true;
    };

    /** The entry a completion belongs to, found by the ring tag it was minted with. */
    [[nodiscard]] entry_t* entry_of_(std::uint32_t slot) noexcept {
        std::uint32_t const base = next_slot_ - static_cast<std::uint32_t>(entries_.size());
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

    /** The lane this pipeline submits and reaps on. */
    io_lane_t& lane_;

    /** The target every offer reads from. */
    target_place_t target_;

    /** One entry per offered request, in the order they were offered. */
    st::basic_vector<entry_t> entries_;

    /** Requests built but not yet taken by the ring. */
    st::basic_vector<request_t> staged_;

    /** Requests the ring has taken and not yet handed back. */
    std::size_t outstanding_ = 0;

    /** The ring tag the next offer takes, which rises for the life of the pipeline. */
    std::uint32_t next_slot_ = 0;
};

#pragma endregion Reading

#pragma region Writing

/**
 *  @brief Stages bytes into blocks and writes them, with one durability barrier at the end.
 *
 *  Chaining is not that barrier and is not used for it: a flush covers only what its owner has
 *  already reaped, so the reap before it is what makes it mean anything.
 */
template <typename disk_type_>
    requires ring_concept<disk_type_>
class basic_write_pipeline {
  public:

    basic_write_pipeline(basic_write_pipeline const&) = delete;
    basic_write_pipeline& operator=(basic_write_pipeline const&) = delete;

    /** A pipeline over @p lane against @p disk, both of which must outlive it. */
    basic_write_pipeline(disk_type_& disk, io_lane_t& lane, target_place_t target) noexcept
        : disk_(disk), lane_(lane), target_(target) {}

    /** Takes a block to fill, or answers @c capacity_exhausted_k until a harvest frees one. */
    expected<buffer_lease_t> stage() noexcept { return lane_.requests().acquire(); }

    /** Takes a streaming block to fill, for a merge or a backup that moves whole megabytes. */
    expected<buffer_lease_t> stage_stream() noexcept { return lane_.streams().acquire(); }

    /** Takes a block, harvesting completions first when every block of the lane is out. */
    expected<buffer_lease_t> stage_or_harvest() noexcept {
        expected<buffer_lease_t> block = stage();
        while (!block && block.status() == status_code_t::capacity_exhausted_k && outstanding_ != 0) {
            if (status_t const harvested = harvest_one(); harvested.failed()) return harvested;
            block = stage();
        }
        return block;
    }

    /**
     *  @brief Queues a write of the first @p length bytes of @p block at @p extent, taking the lease.
     *  @param[in] block The filled block, whose lease this pipeline holds until the write retires.
     *  @param[in] extent Where on the target the bytes land, whose length is what is written.
     */
    status_t commit(buffer_lease_t&& block, extent_t extent) noexcept {
        request_t const request = write_request(target_, extent, block.place(), lane_.token(next_slot_));
        if (staged_.push_back(request_t {request}) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A pipeline could not stage another write"};
        entry_t entry;
        entry.block = std::move(block);
        entry.asked = extent.length;
        if (entries_.push_back(std::move(entry)) != st::status_t::success_k) {
            staged_.pop_back();
            return {status_code_t::out_of_memory_k, "A pipeline could not track another write"};
        }
        ++next_slot_;
        return {};
    }

    /** Submits what is staged, keeping whatever the ring would not take for the next call. */
    status_t flush() noexcept {
        while (!staged_.empty()) {
            expected<std::size_t> const taken = disk_.submit(lane_.owner(), std::span<request_t const> {staged_});
            if (!taken) return taken.status();
            if (*taken == 0) return {};
            outstanding_ += *taken;
            drop_front_(staged_, *taken);
        }
        return {};
    }

    /** Writes offered and not yet harvested, staged ones included. */
    [[nodiscard]] std::size_t outstanding() const noexcept { return outstanding_ + staged_.size(); }

    /** Harvests landed completions, recycling their blocks and failing on anything short of whole. */
    status_t harvest() noexcept {
        if (outstanding_ == 0) return {};
        expected<std::size_t> const reaped = disk_.reap(lane_.owner(), lane_.reaping_room());
        if (!reaped) return reaped.status();
        status_t first;
        std::span<completion_t const> const landed = lane_.reaping_room().first(*reaped);
        for (completion_t const& completion : landed) {
            if (!completion.retires()) continue;
            entry_t* const entry = entry_of_(completion.token.tag());
            if (!entry) continue;
            if (completion.state != completion_state_t::completed_k && first.succeeded())
                first = short_write_status_(completion);
            entry->block.recycle();
            entry->live = false;
            --outstanding_;
        }
        compact_();
        return first;
    }

    /** Harvests until at least one write retires, which is how an exhausted pool makes room. */
    status_t harvest_one() noexcept {
        if (outstanding_ == 0) return {};
        std::size_t const before = outstanding_;
        for (std::size_t spins = 0; outstanding_ == before && spins != reap_spin_limit_k; ++spins)
            if (status_t const harvested = harvest(); harvested.failed()) return harvested;
        return outstanding_ == before ? status_t {status_code_t::io_k, "A lane's completions never arrived"}
                                      : status_t {};
    }

    /**
     *  @brief Reaps every write, then flushes the device and reaps that, which is the barrier.
     *
     *  @note The reap before the flush is what gives the flush meaning, since a flush covers only
     *      what its owner has already reaped.
     */
    status_t make_durable() noexcept {
        while (outstanding() != 0) {
            if (status_t const flushed = flush(); flushed.failed()) return flushed;
            if (status_t const harvested = harvest_one(); harvested.failed()) return harvested;
        }

        std::array<request_t, 1> barrier {flush_request(target_, lane_.token(next_slot_++))};
        expected<std::size_t> const taken = disk_.submit(lane_.owner(), std::span<request_t const> {barrier});
        if (!taken) return taken.status();
        if (*taken != 1) return {status_code_t::io_k, "The ring would not take a flush"};
        expected<std::size_t> const reaped = reap_at_least(disk_, lane_.owner(), lane_.reaping_room(), 1);
        if (!reaped) return reaped.status();
        if (*reaped == 0) return {status_code_t::io_k, "A flush never completed"};
        completion_t const& completion = lane_.reaping_room().front();
        return completion.whole() ? status_t {} : completion.status;
    }

  private:

    /** One queued write and the block it drains, retired when its completion is harvested. */
    struct entry_t {

        /** The block the device reads from, recycled once the write retires. */
        buffer_lease_t block;

        /** Bytes the write asked for, against which a partial transfer is measured. */
        std::uint64_t asked = 0;

        /** Whether this entry still awaits its completion. */
        bool live = true;
    };

    /** What a write that moved less than it asked for means, which is never a retry. */
    static status_t short_write_status_(completion_t const& completion) noexcept {
        if (completion.state == completion_state_t::cancelled_k)
            return {status_code_t::uninitialized_k, "The owner cancelled this write"};
        if (completion.status.failed()) return completion.status;
        return {status_code_t::capacity_exhausted_k, "The volume took fewer bytes than the write asked for"};
    }

    /** The entry a completion belongs to, found by the ring tag it was minted with. */
    [[nodiscard]] entry_t* entry_of_(std::uint32_t slot) noexcept {
        std::uint32_t const base = next_slot_ - static_cast<std::uint32_t>(entries_.size());
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

    /** The lane this pipeline submits and reaps on. */
    io_lane_t& lane_;

    /** The target every write lands on. */
    target_place_t target_;

    /** One entry per queued write, in the order they were committed. */
    st::basic_vector<entry_t> entries_;

    /** Writes built but not yet taken by the ring. */
    st::basic_vector<request_t> staged_;

    /** Writes the ring has taken and not yet handed back. */
    std::size_t outstanding_ = 0;

    /** The ring tag the next write takes, which rises for the life of the pipeline. */
    std::uint32_t next_slot_ = 0;
};

#pragma endregion Writing

} // namespace unum::ustore::nvme
