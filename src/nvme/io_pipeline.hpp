/**
 *  @file src/nvme/io_pipeline.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The submit, poll and refill loops that hold a channel's ring at its depth.
 *
 *  A batch submitted whole and then waited on reaches the depth of that batch and no further, which
 *  is the shape the engine had and the reason it never reached a drive's queue. A pipeline instead
 *  offers until the channel runs out of blocks or credit, polls what has landed, recycles those
 *  blocks and offers again, so the ring stays full from the first refill to the last offer.
 *
 *  Durability is written as submit, poll, flush, poll, because the ring promises no ordering at all
 *  and a flush covers only what its queue pair has already completed.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <array>   // `std::array`
#include <span>    // `std::span`
#include <utility> // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`

#include "nvme/buffer_pool.hpp" // `pooled_buffer_t`
#include "nvme/io_channel.hpp"  // `io_channel_t`
#include "nvme/ring.hpp"        // `request_t`, `completion_t`, `ring_concept`

namespace unum::ustore::nvme {

#pragma region Polling

/** One block that came back, handed to the sink while the channel still holds its queue pair. */
struct completed_buffer_t {

    /** The tag the caller minted when it offered the transfer, naming its own bookkeeping. */
    std::uint32_t tag = 0;

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
    return {status_code_t::input_output_k, "The device moved fewer bytes than the request asked for"};
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
    status_t enqueue_read(extent_t extent, std::uint32_t tag) noexcept {
        if (!can_enqueue()) return {status_code_t::capacity_exhausted_k, "This channel has no block or credit left"};
        expected<pooled_buffer_t> block = channel_.requests().acquire();
        if (!block) return block.status();
        entry_t entry;
        entry.block = std::move(*block);
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
            expected<std::size_t> const taken = disk_.submit(channel_.queue_pair(),
                                                             std::span<request_t const> {staged_});
            if (!taken) return taken.status();
            if (*taken == 0) return {};
            outstanding_ += *taken;
            drop_front_(staged_, *taken);
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
        expected<std::size_t> const completed = disk_.poll_completions(channel_.queue_pair(),
                                                                       channel_.completion_buffer());
        if (!completed) return completed.status();
        std::span<completion_t const> const landed = channel_.completion_buffer().first(*completed);
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
            expected<std::size_t> const seen = poll_completions(sink);
            if (!seen) return seen.status();
        }
        return outstanding_ == before
                   ? status_t {status_code_t::input_output_k, "A channel's completions never arrived"}
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
        std::uint32_t tag = 0;

        /** Bytes the request asked for, against which a partial transfer is measured. */
        std::uint64_t requested_bytes = 0;

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
    std::uint32_t next_slot_ = 0;
};

#pragma endregion Reading

#pragma region Writing

/**
 *  @brief Stages bytes into blocks and writes them, with one durability barrier at the end.
 *
 *  Chaining is not that barrier and is not used for it: a flush covers only what its queue pair has
 *  already completed, so the poll before it is what makes it mean anything.
 */
template <typename disk_type_>
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
        while (!block && block.status() == status_code_t::capacity_exhausted_k && outstanding_ != 0) {
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
            expected<std::size_t> const taken = disk_.submit(channel_.queue_pair(),
                                                             std::span<request_t const> {staged_});
            if (!taken) return taken.status();
            if (*taken == 0) return {};
            outstanding_ += *taken;
            drop_front_(staged_, *taken);
        }
        return {};
    }

    /** Writes offered and not yet polled, staged ones included. */
    [[nodiscard]] std::size_t outstanding() const noexcept { return outstanding_ + staged_.size(); }

    /** Harvests landed completions, recycling their blocks and failing on anything short of whole. */
    status_t poll_completions() noexcept {
        if (outstanding_ == 0) return {};
        expected<std::size_t> const completed = disk_.poll_completions(channel_.queue_pair(),
                                                                       channel_.completion_buffer());
        if (!completed) return completed.status();
        status_t first;
        std::span<completion_t const> const landed = channel_.completion_buffer().first(*completed);
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
        return outstanding_ == before
                   ? status_t {status_code_t::input_output_k, "A channel's completions never arrived"}
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
        expected<std::size_t> const taken = disk_.submit(channel_.queue_pair(), std::span<request_t const> {barrier});
        if (!taken) return taken.status();
        if (*taken != 1) return {status_code_t::input_output_k, "The ring would not take a flush"};
        expected<std::size_t> const completed = poll_completions_at_least(disk_, channel_.queue_pair(),
                                                                          channel_.completion_buffer(), 1);
        if (!completed) return completed.status();
        if (*completed == 0) return {status_code_t::input_output_k, "A flush never completed"};
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
    std::uint32_t next_slot_ = 0;
};

#pragma endregion Writing

} // namespace unum::ustore::nvme
