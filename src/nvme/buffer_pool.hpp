/**
 *  @file src/nvme/buffer_pool.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The one memory range a disk pins, and the fixed blocks channels queue pair out of it.
 *
 *  Every I/O buffer in the engine comes from here, because a request addresses memory by registered memory
 *  slot and offset rather than by pointer, and a range can only be pinned once: @c io_uring answers
 *  @c EBUSY on re-registered memory and a growing arena cannot be pinned at all. So an arena is allocated,
 *  charged and registered once at mount, and everything above it borrows a block instead of
 *  allocating. A block is the unit of one request; a transfer larger than one block becomes several
 *  requests, which is mandatory anyway, since a namespace reports a maximum transfer well under the
 *  largest value a collection may hold.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <span>    // `std::span`
#include <utility> // `std::exchange`, `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/aligned_allocator.hpp"     // `aligned_allocator`
#include "nvme/constrained_allocator.hpp" // `resource_constraint_t`
#include "nvme/extent.hpp"                // `portable_alignment_k`
#include "nvme/ring.hpp"                  // `registered_memory_t`, `ring_concept`, `memory_space_t`

namespace unum::ustore::nvme {

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
 */
class registered_memory_arena_t {
  public:

    registered_memory_arena_t() noexcept = default;
    registered_memory_arena_t(registered_memory_arena_t const&) = delete;
    registered_memory_arena_t& operator=(registered_memory_arena_t const&) = delete;

    registered_memory_arena_t(registered_memory_arena_t&& other) noexcept
        : storage_(std::move(other.storage_)), registered_memory_(std::move(other.registered_memory_)),
          constraint_(std::exchange(other.constraint_, nullptr)), charged_(std::exchange(other.charged_, 0)) {}

    registered_memory_arena_t& operator=(registered_memory_arena_t&& other) noexcept {
        if (this == &other) return *this;
        refund_();
        storage_ = std::move(other.storage_);
        registered_memory_ = std::move(other.registered_memory_);
        constraint_ = std::exchange(other.constraint_, nullptr);
        charged_ = std::exchange(other.charged_, 0);
        return *this;
    }

    ~registered_memory_arena_t() noexcept { refund_(); }

    /**
     *  @brief Allocates, charges and registers one arena against @p disk.
     *  @param[inout] disk The ring the arena is pinned in, which must outlive the arena.
     *  @param[inout] constraint The budget the whole arena is charged to, once.
     *  @param[in] config Bytes and placement, both fixed from here on.
     *  @return The arena, or @c out_of_memory_k when the budget or the allocator refuses it.
     */
    template <typename disk_type_>
        requires ring_concept<disk_type_>
    static expected<registered_memory_arena_t> make(disk_type_& disk, resource_constraint_t& constraint,
                                                    memory_arena_config_t const& config) noexcept {
        if (config.bytes == 0) return status_t {status_code_t::invalid_argument_k, "An arena holds at least one block"};
        if (config.placement == memory_space_t::gpu_k)
            return status_t {status_code_t::unsupported_k, "Device memory has no arena yet"};

        std::size_t const bytes = static_cast<std::size_t>(round_up_to_multiple(config.bytes, portable_alignment_k));
        registered_memory_arena_t arena;
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
    resource_constraint_t* constraint_ = nullptr;

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
     *  @brief Carves @p config blocks out of @p arena, starting at @p region_offset.
     *  @param[in] arena The registered range the blocks address, which must outlive the pool.
     *  @param[in] region_offset First byte of this pool's region inside the arena.
     *  @param[in] config Block size and block count, both fixed from here on.
     */
    static expected<buffer_pool_t> make(registered_memory_arena_t& arena, std::uint64_t region_offset,
                                        buffer_pool_config_t const& config) noexcept {
        if (config.buffer_bytes == 0 || config.buffers_count == 0)
            return status_t {status_code_t::invalid_argument_k, "A pool holds at least one block"};
        std::uint64_t const span = config.buffer_bytes * config.buffers_count;
        if (region_offset > arena.size() || span > arena.size() - region_offset)
            return status_t {status_code_t::invalid_argument_k, "A pool's region runs past its arena"};

        buffer_pool_t pool;
        pool.arena_ = &arena;
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
        buffer.bytes_ = arena_->bytes().subspan(static_cast<std::size_t>(region_offset_ + index * buffer_bytes_),
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
    [[nodiscard]] bool made() const noexcept { return arena_ != nullptr; }

  private:

    friend class pooled_buffer_t;

    /** Takes @p index back, which only a queue pair's own release calls. */
    void recycle_(std::uint32_t index) noexcept {
        if (free_count_ < free_indices_.size()) free_indices_[free_count_++] = index;
    }

    /** The place of the block at @p index, @p offset bytes in. */
    [[nodiscard]] memory_reference_t place_(std::uint32_t index, std::uint64_t offset) const noexcept {
        return arena_->reference(region_offset_ + index * buffer_bytes_ + offset);
    }

    /** The arena the blocks live in, null until the pool is made. */
    registered_memory_arena_t* arena_ = nullptr;

    /** Where the pool's first block sits inside the arena, so a queue pair can mint its place. */
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

} // namespace unum::ustore::nvme
