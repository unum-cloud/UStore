/**
 *  @file src/nvme/buffer_pool.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The one memory range a disk pins, and the fixed blocks lanes lease out of it.
 *
 *  Every I/O buffer in the engine comes from here, because a request addresses memory by registration
 *  slot and offset rather than by pointer, and a range can only be pinned once: @c io_uring answers
 *  @c EBUSY on re-registration and a growing arena cannot be pinned at all. So a slab is allocated,
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
#include "nvme/extent.hpp"                // `portable_alignment_k`, `rounded_up_to`
#include "nvme/ring.hpp"                  // `registration_t`, `ring_concept`, `memory_space_t`

namespace unum::ustore::nvme {

#pragma region Slab

/** Where a slab's bytes come from, which decides the budget it is charged to and the space it registers in. */
enum class slab_placement_t : std::uint8_t {

    /** Ordinary host memory, aligned to @c portable_alignment_k and charged to the host budget. */
    host_k,

    /** GPU memory, registered as @c memory_space_t::device_k and charged to the GPU budget. */
    device_k,
};

/** What a mount fixes about a slab before any lane carves blocks out of it. */
struct slab_config_t {

    /** Bytes the slab holds, rounded up to @c portable_alignment_k when it is made. */
    std::uint64_t bytes = 0;

    /** Where the bytes live, which the disk must be able to register. */
    slab_placement_t placement = slab_placement_t::host_k;
};

/**
 *  @brief The one memory range a disk registers, owned for the life of the mount.
 *
 *  It is charged to its budget once, here, which is why no buffer below it is charged again: the
 *  bytes were paid for when the disk came up, and a mount that cannot afford its I/O memory is a
 *  configuration error rather than a failure discovered under load.
 */
class registered_slab_t {
  public:

    registered_slab_t() noexcept = default;
    registered_slab_t(registered_slab_t const&) = delete;
    registered_slab_t& operator=(registered_slab_t const&) = delete;

    registered_slab_t(registered_slab_t&& other) noexcept
        : storage_(std::move(other.storage_)), registration_(std::move(other.registration_)),
          constraint_(std::exchange(other.constraint_, nullptr)), charged_(std::exchange(other.charged_, 0)) {}

    registered_slab_t& operator=(registered_slab_t&& other) noexcept {
        if (this == &other) return *this;
        refund_();
        storage_ = std::move(other.storage_);
        registration_ = std::move(other.registration_);
        constraint_ = std::exchange(other.constraint_, nullptr);
        charged_ = std::exchange(other.charged_, 0);
        return *this;
    }

    ~registered_slab_t() noexcept { refund_(); }

    /**
     *  @brief Allocates, charges and registers one slab against @p disk.
     *  @param[inout] disk The ring the slab is pinned in, which must outlive the slab.
     *  @param[inout] constraint The budget the whole slab is charged to, once.
     *  @param[in] config Bytes and placement, both fixed from here on.
     *  @return The slab, or @c out_of_memory_k when the budget or the allocator refuses it.
     */
    template <typename disk_type_>
        requires ring_concept<disk_type_>
    static expected<registered_slab_t> make(disk_type_& disk, resource_constraint_t& constraint,
                                            slab_config_t const& config) noexcept {
        if (config.bytes == 0) return status_t {status_code_t::invalid_argument_k, "A slab holds at least one block"};
        if (config.placement == slab_placement_t::device_k)
            return status_t {status_code_t::unsupported_k, "Device memory has no slab yet"};

        std::size_t const bytes = static_cast<std::size_t>(rounded_up_to(config.bytes, portable_alignment_k));
        registered_slab_t slab;
        if (!constraint.acquire(bytes)) return status_t {status_code_t::out_of_memory_k, "The budget refused a slab"};
        slab.constraint_ = &constraint;
        slab.charged_ = bytes;
        if (slab.storage_.resize(bytes) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A slab of that size does not fit"};

        expected<registration_t> pinned = disk.register_chunk(std::span<byte_t> {slab.storage_},
                                                              memory_space_t::host_k);
        if (!pinned) return pinned.status();
        slab.registration_ = std::move(*pinned);
        return slab;
    }

    /** Unpins the slab, refusing while any request against it is still in flight. */
    template <typename disk_type_>
        requires ring_concept<disk_type_>
    status_t dismount(disk_type_& disk) noexcept {
        if (!registration_.held()) return {};
        return disk.deregister_chunk(registration_);
    }

    /** The whole range, which only the pools carved out of it address. */
    [[nodiscard]] std::span<byte_t> bytes() noexcept { return std::span<byte_t> {storage_}; }

    /** Bytes the slab holds. */
    [[nodiscard]] std::uint64_t size() const noexcept { return storage_.size(); }

    /** Whether the slab is still pinned. */
    [[nodiscard]] bool registered() const noexcept { return registration_.held(); }

    /** The place @p offset bytes into the slab, which a request carries instead of a pointer. */
    [[nodiscard]] buffer_place_t place(std::uint64_t offset) const noexcept { return registration_.place(offset); }

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

    /** The bytes of the slab, owned here and never moved once registered. */
    storage_t storage_;

    /** The one registration over @c storage_, surrendered by @c dismount. */
    registration_t registration_;

    /** The budget @c storage_ is charged to, null until the slab is mounted. */
    resource_constraint_t* constraint_ = nullptr;

    /** Bytes charged to @c constraint_, returned when the slab dies. */
    std::size_t charged_ = 0;
};

#pragma endregion Slab

#pragma region Pool

/** What a lane's pool is shaped by, both numbers fixed at mount. */
struct buffer_pool_config_t {

    /** Bytes of one block, a multiple of the disk's length granularity and at most its max transfer. */
    std::uint64_t block_bytes = 0;

    /** Blocks the pool hands out, which bounds the requests one lane keeps in flight at once. */
    std::uint32_t blocks_count = 0;
};

class buffer_pool_t;

/**
 *  @brief One block borrowed from a pool, which resolves to a place and to a span of the same bytes.
 *
 *  The lease is move-only and never copied, since the ring's rule is that a block belongs to the
 *  device from @c submit until its completion is reaped: a second handle onto it would be a second
 *  claim on memory the device is writing. Returning it is the destructor's business, so a reap loop
 *  that fails halfway cannot leak a block.
 */
class buffer_lease_t {
  public:

    buffer_lease_t() noexcept = default;
    buffer_lease_t(buffer_lease_t const&) = delete;
    buffer_lease_t& operator=(buffer_lease_t const&) = delete;

    buffer_lease_t(buffer_lease_t&& other) noexcept
        : pool_(std::exchange(other.pool_, nullptr)), index_(std::exchange(other.index_, 0)),
          bytes_(std::exchange(other.bytes_, std::span<byte_t> {})) {}

    buffer_lease_t& operator=(buffer_lease_t&& other) noexcept;

    ~buffer_lease_t() noexcept { recycle(); }

    /** Whether this handle still holds a block. */
    [[nodiscard]] bool held() const noexcept { return pool_ != nullptr; }

    /** The bytes of the block, which the caller fills before a write and reads after a read. */
    [[nodiscard]] std::span<byte_t> bytes() const noexcept { return bytes_; }

    /** Bytes of the block, which is the pool's block size. */
    [[nodiscard]] std::uint64_t size() const noexcept { return bytes_.size(); }

    /** The place @p offset bytes into the block, which a request carries instead of a pointer. */
    [[nodiscard]] buffer_place_t place(std::uint64_t offset = 0) const noexcept;

    /** Hands the block back early, which the destructor does anyway. */
    void recycle() noexcept;

  private:

    friend class buffer_pool_t;

    /** The pool the block goes back to, null once the lease is empty. */
    buffer_pool_t* pool_ = nullptr;

    /** The block's index in its pool, which is the free-list entry pushed back on recycle. */
    std::uint32_t index_ = 0;

    /** The block's bytes inside the slab. */
    std::span<byte_t> bytes_;
};

/**
 *  @brief Fixed-size blocks carved out of one region of a slab, handed out to one lane.
 *
 *  There is one size class and no atomics, because a lane owns its pool outright and a lane is
 *  single-issuer by the ring's own rule. Exhaustion is therefore always the lane's own doing, and
 *  the lane is the thread that reaps, so the answer to an exhausted pool is to reap a completion and
 *  recycle its block rather than to wait on anyone.
 */
class buffer_pool_t {
  public:

    buffer_pool_t() noexcept = default;
    buffer_pool_t(buffer_pool_t const&) = delete;
    buffer_pool_t& operator=(buffer_pool_t const&) = delete;
    buffer_pool_t(buffer_pool_t&&) noexcept = default;
    buffer_pool_t& operator=(buffer_pool_t&&) noexcept = default;

    /**
     *  @brief Carves @p config blocks out of @p slab, starting at @p region_offset.
     *  @param[in] slab The registered range the blocks address, which must outlive the pool.
     *  @param[in] region_offset First byte of this pool's region inside the slab.
     *  @param[in] config Block size and block count, both fixed from here on.
     */
    static expected<buffer_pool_t> make(registered_slab_t& slab, std::uint64_t region_offset,
                                        buffer_pool_config_t const& config) noexcept {
        if (config.block_bytes == 0 || config.blocks_count == 0)
            return status_t {status_code_t::invalid_argument_k, "A pool holds at least one block"};
        std::uint64_t const span = config.block_bytes * config.blocks_count;
        if (region_offset > slab.size() || span > slab.size() - region_offset)
            return status_t {status_code_t::invalid_argument_k, "A pool's region runs past its slab"};

        buffer_pool_t pool;
        pool.slab_ = &slab;
        pool.region_offset_ = region_offset;
        pool.block_bytes_ = config.block_bytes;
        if (pool.free_indices_.resize(config.blocks_count) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A pool could not track its blocks"};
        for (std::uint32_t index = 0; index != config.blocks_count; ++index)
            pool.free_indices_[index] = config.blocks_count - 1 - index;
        pool.free_count_ = config.blocks_count;
        return pool;
    }

    /** Takes one block, or answers @c capacity_exhausted_k when every block of this lane is out. */
    expected<buffer_lease_t> acquire() noexcept {
        if (free_count_ == 0) return status_t {status_code_t::capacity_exhausted_k, "Every block of this lane is out"};
        std::uint32_t const index = free_indices_[--free_count_];
        buffer_lease_t lease;
        lease.pool_ = this;
        lease.index_ = index;
        lease.bytes_ = slab_->bytes().subspan(static_cast<std::size_t>(region_offset_ + index * block_bytes_),
                                              static_cast<std::size_t>(block_bytes_));
        return lease;
    }

    /** Blocks nobody holds, which a submit loop reads to decide how much more to offer. */
    [[nodiscard]] std::uint32_t available() const noexcept { return free_count_; }

    /** Blocks the pool was carved with. */
    [[nodiscard]] std::uint32_t capacity() const noexcept { return static_cast<std::uint32_t>(free_indices_.size()); }

    /** Bytes of one block, which is the unit every transfer is split into. */
    [[nodiscard]] std::uint64_t block_bytes() const noexcept { return block_bytes_; }

    /** Whether the pool was ever carved, which an unmounted device pool is not. */
    [[nodiscard]] bool carved() const noexcept { return slab_ != nullptr; }

  private:

    friend class buffer_lease_t;

    /** Takes @p index back, which only a lease's own recycle calls. */
    void recycle_(std::uint32_t index) noexcept {
        if (free_count_ < free_indices_.size()) free_indices_[free_count_++] = index;
    }

    /** The place of the block at @p index, @p offset bytes in. */
    [[nodiscard]] buffer_place_t place_(std::uint32_t index, std::uint64_t offset) const noexcept {
        return slab_->place(region_offset_ + index * block_bytes_ + offset);
    }

    /** The slab the blocks live in, null until the pool is made. */
    registered_slab_t* slab_ = nullptr;

    /** Where the pool's first block sits inside the slab, so a lease can mint its place. */
    std::uint64_t region_offset_ = 0;

    /** Bytes of one block. */
    std::uint64_t block_bytes_ = 0;

    /** Indices of the blocks nobody holds, read and written as a stack by the owning lane alone. */
    st::basic_vector<std::uint32_t> free_indices_;

    /** Entries of @c free_indices_ that are live, which is the stack's height. */
    std::uint32_t free_count_ = 0;
};

inline buffer_lease_t& buffer_lease_t::operator=(buffer_lease_t&& other) noexcept {
    if (this == &other) return *this;
    recycle();
    pool_ = std::exchange(other.pool_, nullptr);
    index_ = std::exchange(other.index_, 0);
    bytes_ = std::exchange(other.bytes_, std::span<byte_t> {});
    return *this;
}

inline buffer_place_t buffer_lease_t::place(std::uint64_t offset) const noexcept {
    return pool_ ? pool_->place_(index_, offset) : buffer_place_t {};
}

inline void buffer_lease_t::recycle() noexcept {
    if (!pool_) return;
    pool_->recycle_(index_);
    pool_ = nullptr;
    bytes_ = std::span<byte_t> {};
}

/** Blocks a transfer of @p length bytes needs from a pool of @p block_bytes blocks. */
[[nodiscard]] constexpr std::uint64_t blocks_for(std::uint64_t length, std::uint64_t block_bytes) noexcept {
    return block_bytes == 0 ? 0 : (length + block_bytes - 1) / block_bytes;
}

#pragma endregion Pool

} // namespace unum::ustore::nvme
