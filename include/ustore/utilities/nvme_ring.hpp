/**
 *  @file include/ustore/utilities/nvme_ring.hpp
 *  @author Ash Vardanian
 *  @date May 23, 2021
 *  @brief The submission and completion ring every NVMe transport exposes, and the vocabulary
 *      beneath it: completion counters, disk and file identifiers, alignment and extents.
 *
 *  Nothing here reaches StringZilla, so a CUDA translation unit can include the ring and the key
 *  slots above it; the allocators live in @c ram_buffers.hpp and the identifiers, byte views and
 *  serialization in @c types.hpp.
 *
 *  The ring is exposed rather than hidden behind a blocking read, because a blocking read reaches
 *  the depth of one thread and an NVMe namespace wants thousands of commands outstanding. An queue
 *  pair submits a batch, does other work, and polls its own completions; nothing here ever blocks,
 *  so no fiber is needed to park a caller that ran out of credit.
 *
 *  The shape is what the three real transports allow rather than what is convenient. io_uring
 *  with @c SINGLE_ISSUER and @c DEFER_TASKRUN answers @c -EEXIST when a thread other than the
 *  submitter polls, and an SPDK queue pair silently corrupts its tracker list in the same case,
 *  so a queue pair is a single-threaded queue pair rather than a label. Both address a buffer by
 *  a registered memory slot plus an offset - @c IORING_OP_READ_FIXED and @c cuFileRead alike - so
 *  a request names a registered memory and not a pointer. Neither reports a misaligned request at
 *  submission time, so every per-request defect arrives as a failed completion and @c submit
 *  refuses only what would corrupt bookkeeping.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int32_t`, `std::uint8_t`, `std::uint16_t`, `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`

#include <concepts> // `std::same_as`
#include <span>     // `std::span`
#include <utility>  // `std::exchange`

#include "ustore/utilities/types.hpp" // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::utilities {

#pragma region Countdown

template <template <typename> typename atomic_reference_, typename waiter_type_>
class notifier;

/**
 *  @brief The parts still owed by one fanned-out job, and the first failure any of them reported.
 *
 *  One owner holds the counter and waits on it; every part holds a @ref notifier onto it and
 *  reports through that, so the counter itself is never copied and outlives every part.
 *
 *  @tparam atomic_reference_ The reference the counter's words are owned through.
 *  @tparam waiter_type_ What the waiting owner's core does between polls.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class countdown {
  public:

    countdown() noexcept = default;

    /** A counter owed @p parts reports. */
    explicit countdown(std::size_t parts) noexcept : remaining_(static_cast<std::int32_t>(parts)) {}

    countdown(countdown&&) = default;
    countdown& operator=(countdown&&) = default;
    countdown(countdown const&) = delete;
    countdown& operator=(countdown const&) = delete;

    /** Arms the counter for @p parts reports, forgetting whatever the last round recorded. */
    void reset(std::size_t parts = 1) noexcept {
        message_ = nullptr;
        code_reference_().store(0, std::memory_order_relaxed);
        remaining_reference_().store(static_cast<std::int32_t>(parts), std::memory_order_release);
    }

    /** Adds @p parts to what the counter is owed, for a fan-out that grows while it runs. */
    void increment(std::int32_t parts = 1) noexcept { remaining_reference_().add(parts, std::memory_order_relaxed); }

    /** Whether every part has reported, or one of them has failed. */
    bool has_finished() const noexcept {
        if (code_reference_().load(std::memory_order_acquire) != 0) return true;
        return remaining_reference_().load(std::memory_order_acquire) <= 0;
    }

    /** Whether any part reported a failure. */
    bool has_failed() const noexcept { return code_reference_().load(std::memory_order_acquire) != 0; }

    /** Whether the parts still owed have fallen to @p threshold or below. */
    bool test(std::int32_t threshold) const noexcept {
        return remaining_reference_().load(std::memory_order_acquire) <= threshold;
    }

    /**
     *  @brief Waits until every part reports or one fails.
     *
     *  @return Whether every part succeeded.
     */
    bool wait() noexcept {
        for (std::int32_t remaining = remaining_reference_().load(std::memory_order_acquire); remaining > 0;
             remaining = remaining_reference_().load(std::memory_order_acquire))
            waiter_type_ {}(&remaining_, remaining, std::size_t {0});
        return !has_failed();
    }

    /** The failure a part reported, and success once the counter carries none again. */
    status_t release_status() noexcept {
        int const code = code_reference_().exchange(0, std::memory_order_acquire);
        if (code == 0) return {};
        return {static_cast<status_code_t>(code), std::exchange(message_, nullptr)};
    }

  private:

    friend class notifier<atomic_reference_, waiter_type_>;

    /** The parts still owed, which a failure drives to zero however many are outstanding. */
    std::int32_t remaining_ = 1;

    /** The @c ustore_status_t of the first failure, or zero while none has been reported. */
    int code_ = 0;

    /** The static message of the first failure, stored before the code that publishes it. */
    char const* message_ = nullptr;

    atomic_reference_<std::int32_t> remaining_reference_() noexcept {
        return atomic_reference_<std::int32_t> {remaining_};
    }

    atomic_reference_<std::int32_t const> remaining_reference_() const noexcept {
        return atomic_reference_<std::int32_t const> {remaining_};
    }

    atomic_reference_<int> code_reference_() noexcept { return atomic_reference_<int> {code_}; }

    atomic_reference_<int const> code_reference_() const noexcept { return atomic_reference_<int const> {code_}; }
};

/**
 *  @brief One part's handle onto the counter its job reports through, cheap enough to copy per part.
 *
 *  Every handle borrows a counter that outlives it, so a part reports without knowing who waits.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class notifier {
  public:

    /** The counter a handle reports through. */
    using countdown_t = countdown<atomic_reference_, waiter_type_>;

    notifier() = delete;
    notifier(notifier const&) = default;

    /** A handle onto @p counter, which must outlive every part holding one. */
    notifier(countdown_t& counter) noexcept : countdown_(counter) {}

    /** The counter this handle reports through. */
    countdown_t& counter() const noexcept { return countdown_; }

    /** Reports one part done. */
    void did_one() const noexcept { countdown_.remaining_reference_().sub(1, std::memory_order_release); }

    /** Reports every remaining part done, for a fan-out that finished early. */
    void did_all() const noexcept { countdown_.remaining_reference_().store(0, std::memory_order_release); }

    /**
     *  @brief Reports @p status, which the counter keeps if it is the first failure.
     *
     *  A failure also drives the remaining parts to zero, so a waiter stops without waiting for
     *  the parts a failed job will never run. Reporting success does nothing.
     */
    void fail(status_t status) const noexcept {
        if (status.succeeded()) return;
        countdown_.message_ = status.message;
        int expected = 0;
        if (!countdown_.code_reference_().compare_exchange_strong(expected, static_cast<int>(status.code),
                                                                  std::memory_order_release, std::memory_order_relaxed))
            return;
        countdown_.remaining_reference_().store(0, std::memory_order_release);
    }

    /** Whether any part reported a failure. */
    bool has_failed() const noexcept { return countdown_.has_failed(); }

    /** Whether every part has reported, or one of them has failed. */
    bool has_finished() const noexcept { return countdown_.has_finished(); }

    /** Whether the parts still owed have fallen to @p threshold or below. */
    bool test(std::int32_t threshold) const noexcept { return countdown_.test(threshold); }

  private:

    /** The counter this handle reports through, owned by whoever waits on it. */
    countdown_t& countdown_;
};

#pragma endregion Countdown

#pragma region IO Types

/** One mounted disk of a data directory, where every stored value names the disk it sits on. */
enum class disk_id_t : std::uint16_t {
    unknown_k = 0xFFFF,
};

/** A POSIX file descriptor, where a negative value marks a failed open. */
enum class file_id_t : int {
    invalid_k = -1,
};

#pragma endregion IO Types

#pragma region Alignment

/**
 *  @brief The alignment every transport accepts, for memory, for offsets and for lengths alike.
 *
 *  A caller that cannot ask its disk what it needs aligns to this and is accepted everywhere:
 *  io_uring on @c O_DIRECT reports @b [512,4096] through @c STATX_DIOALIGN, SPDK needs a page
 *  boundary for any transfer that crosses one, and cuFile wants 4096 on the device pointer, the
 *  file offset and the size. Nothing hardcodes 512, since a disk reports its own three numbers.
 */
inline constexpr std::uint64_t portable_alignment_k = 4096;

/** Whether @p address is a multiple of @p alignment, for the memory a registration hands the driver. */
inline bool is_address_aligned(void const* address, std::uint64_t alignment) noexcept {
    return (reinterpret_cast<std::uintptr_t>(address) & (alignment - 1)) == 0;
}

#pragma endregion Alignment

#pragma region Extent

/**
 *  @brief A contiguous byte range on one volume, counted from the volume's first byte.
 *
 *  One disk serves one volume, so an extent needs no volume identifier: a directory volume
 *  resolves a name to its disk before it builds the extent, and an extent volume addresses the
 *  namespace directly. Both fields are byte counts, never blocks, so a 512-byte and a 4096-byte
 *  format describe the same range the same way.
 */
struct extent_t {

    /** First byte of the range, which a disk requires to be a multiple of its offset alignment. */
    std::uint64_t offset = 0;

    /** Bytes in the range, which a disk requires to be a multiple of its length granularity. */
    std::uint64_t length = 0;

    /** One past the last byte, saturating rather than wrapping on a range that overflows. */
    [[nodiscard]] constexpr std::uint64_t end() const noexcept {
        return offset > UINT64_MAX - length ? UINT64_MAX : offset + length;
    }

    /** Whether the range covers no bytes, which only a flush submits. */
    [[nodiscard]] constexpr bool empty() const noexcept { return length == 0; }

    /** Whether the whole range sits below @p capacity, computed without overflowing. */
    [[nodiscard]] constexpr bool fits_within(std::uint64_t capacity) const noexcept {
        return offset <= capacity && length <= capacity - offset;
    }

    /** Whether @p other sits entirely inside this range. */
    [[nodiscard]] constexpr bool contains(extent_t const& other) const noexcept {
        return other.offset >= offset && other.end() <= end();
    }

    /** Whether the two ranges share at least one byte. */
    [[nodiscard]] constexpr bool overlaps(extent_t const& other) const noexcept {
        return offset < other.end() && other.offset < end();
    }

    /** Whether the offset and the length are both multiples of @p alignment. */
    [[nodiscard]] constexpr bool aligned_to(std::uint64_t alignment) const noexcept {
        return (offset & (alignment - 1)) == 0 && (length & (alignment - 1)) == 0;
    }

    /** The part of this range that sits below @p capacity, empty when it starts past the end. */
    [[nodiscard]] constexpr extent_t clipped_to(std::uint64_t capacity) const noexcept {
        if (offset >= capacity) return extent_t {offset, 0};
        std::uint64_t const reachable = capacity - offset;
        return extent_t {offset, length < reachable ? length : reachable};
    }

    friend constexpr bool operator==(extent_t const& first, extent_t const& second) noexcept {
        return first.offset == second.offset && first.length == second.length;
    }
};

#pragma endregion Extent

#pragma region Ring

#pragma region Vocabulary

/** A table's version, raised on every change so a ring knows its copy is behind. */
using epoch_t = std::int32_t;

/** The hash of a thread's identity, which binds a queue pair to the thread that submits on it. */
using thread_identity_t = std::uint64_t;

/** Which reissue of a queue pair slot a token belongs to, the 16 bits of the token beside the tag. */
using queue_pair_generation_t = std::uint16_t;

/** The 32 bits of a token its queue pair's holder chooses, usually a slot index. */
using request_tag_t = std::uint32_t;

/** Requests one queue pair may keep outstanding. */
using queue_depth_t = std::uint32_t;

/** What a submitted request asks the device to do. */
enum class request_kind_t : std::uint8_t {

    /** Moves @c extent.length bytes from the volume into the registered buffer. */
    read_k,

    /** Moves @c extent.length bytes from the registered buffer onto the volume. */
    write_k,

    /** Makes every write this queue pair has already completed durable; its extent is empty. */
    flush_k,

    /** Gives @c extent back to the medium, which may read it as anything afterwards; no buffer is
     *  named, and a transport that runs it at submission refuses it behind a chained request. */
    deallocate_k,
};

/**
 *  @brief Whether the next request of the same batch waits for this one.
 *
 *  Chaining is the only ordering the ring offers, and it costs what @c IOSQE_IO_LINK costs: a short
 *  or failed result cuts the rest of the chain, which arrives as @c cancelled_k. A chain orders a
 *  bounded sequence inside one submitted batch and never orders across batches or across queue_pairs.
 */
enum class request_chaining_t : std::uint8_t {

    /** The request may start, execute and complete in any order against its neighbours. */
    independent_k,

    /** The following request of the same batch starts only after this one transfers in full. */
    chained_k,
};

/** How a request ended, which is the state the queue pair branches on rather than the status alone. */
enum class completion_state_t : std::uint8_t {

    /** Every byte of the extent moved, and the status is a success. */
    completed_k,

    /** Fewer bytes than the extent asked for moved, and only that prefix is data. */
    partial_k,

    /** The queue pair cancelled before the request started, or a chain ahead of it was cut. */
    cancelled_k,

    /** Nothing usable moved, and the status says why. */
    failed_k,

    /** More completions follow for this token, so the token is not retired yet. */
    continued_k,
};

/**
 *  @brief Where a registered buffer lives, which is a property of the registered memory and never of a request.
 *
 *  A compaction read into device memory and a compaction read into host memory are one
 *  request shape; only the registered memory call differs - @c cuFileBufRegister
 *  against @c IORING_REGISTER_BUFFERS against @c spdk_mem_register - so a disk that
 *  cannot reach device memory refuses at registered memory, where the caller can still
 *  choose another buffer.
 */
enum class memory_space_t : std::uint8_t {

    /** Ordinary host memory, which every transport reaches. */
    host_k,

    /** GPU memory, which only a GPUDirect-capable disk registers. */
    gpu_k,
};

/** One queue pair on a share of the ring's depth, or @c unallocated_k for a handle that holds none. */
enum class queue_pair_id_t : std::uint16_t {
    unallocated_k = 0xFFFF,
};

/** One registered memory range, or @c unregistered_k for a handle that names none. */
enum class registered_memory_id_t : std::uint16_t {
    unregistered_k = 0xFFFF,
};

/** One registered medium the bytes move to or from, or @c unregistered_k for a handle that names none. */
enum class registered_medium_id_t : std::uint16_t {
    unregistered_k = 0xFFFF,
};

/** What a caller may do to a medium, which the transport fixes when it registers one. */
enum class medium_access_t : std::uint8_t {

    /** Reads alone, so a write against this medium fails rather than growing it. */
    read_only_k,

    /** Reads and writes against a medium that must already exist. */
    read_write_k,

    /** Reads and writes, creating the medium at the requested length when it is absent. */
    create_read_write_k,
};

/**
 *  @brief What a caller asks a transport to register as a medium.
 *
 *  One shape serves all three transports because each reads only what it can: a file disk
 *  opens @c name as a path, a RAM disk keeps a buffer of @c length under that label, and a
 *  namespace ignores both, since the medium it registers is the namespace it was mounted on.
 */
struct medium_options_t {

    /** A path for a file disk, a label for a RAM disk, ignored by a namespace. */
    char const* name = nullptr;

    /** Bytes to create the medium with when it is absent, or zero to take whatever is there. */
    std::uint64_t length = 0;

    /** What the caller may do to it once it is registered. */
    medium_access_t access = medium_access_t::read_write_k;
};

#pragma endregion Vocabulary

#pragma region Tokens

/**
 *  @brief The 64 bits that travel to the device and back, naming the queue pair and the queue pair's request.
 *
 *  io_uring hands @c user_data back verbatim and inspects nothing, so the layout is ours: the queue pair
 *  identifier and its queue pair generation let the ring route a completion to exactly one queue pair, and the
 *  tag is the queue pair's own, usually an index into its request table. The generation matters because a
 *  released queue pair is reissued, and a stale token must not deliver into its successor's queue.
 *
 *  @note A tag is unique among one queue pair's in-flight requests, since cancellation on a real ring
 *      matches on these bits and would otherwise cancel the wrong request.
 */
struct request_token_t {

    /** The queue pair in the high 16 bits, the queue pair generation in the next 16, the queue pair's tag in the
     *  low 32. */
    std::uint64_t bits = 0;

    /** Packs a queue pair, its queue pair generation and the queue pair's own tag into one token. */
    [[nodiscard]] static constexpr request_token_t make(queue_pair_id_t queue_pair, queue_pair_generation_t generation,
                                                        request_tag_t tag) noexcept {
        return request_token_t {(static_cast<std::uint64_t>(queue_pair) << 48) |
                                (static_cast<std::uint64_t>(generation) << 32) | tag};
    }

    /** The queue pair this completion belongs to. */
    [[nodiscard]] constexpr queue_pair_id_t queue_pair() const noexcept {
        return static_cast<queue_pair_id_t>(static_cast<std::uint16_t>(bits >> 48));
    }

    /** The queue pair generation, which separates a queue pair from its predecessor in the same slot. */
    [[nodiscard]] constexpr queue_pair_generation_t generation() const noexcept {
        return static_cast<queue_pair_generation_t>(bits >> 32);
    }

    /** The queue pair's own tag, which the ring never interprets. */
    [[nodiscard]] constexpr request_tag_t tag() const noexcept { return static_cast<request_tag_t>(bits); }

    friend constexpr bool operator==(request_token_t first, request_token_t second) noexcept {
        return first.bits == second.bits;
    }
};

#pragma endregion Tokens

#pragma region Requests

/**
 *  @brief A place inside one registered range, which is how a request names its buffer.
 *
 *  A bare pointer cannot be submitted: io_uring's fixed forms want a slot index plus an offset,
 *  cuFile wants the registered base plus an offset, and SPDK needs the range in its memory map
 *  before @c spdk_vtophys can translate it. Naming the registered memory also makes the borrow
 *  visible, so a disk can refuse to deregister memory that is still a live DMA medium.
 */
struct memory_reference_t {

    /** The range the bytes sit in, which the queue pair obtained from @c register_memory. */
    registered_memory_id_t memory_id = registered_memory_id_t::unregistered_k;

    /** The queue pair generation of that range, so a stale place resolves to nothing. */
    queue_pair_generation_t generation = 0;

    /** Bytes from the start of the range to the first byte this request touches. */
    std::uint64_t offset = 0;
};

/**
 *  @brief Which registered medium a request moves bytes against, named the way its buffer is.
 *
 *  Storage is addressed exactly as memory is, by a registered slot and an offset into it, because a
 *  path cannot be submitted any more than a pointer can: @c IORING_REGISTER_FILES wants a table
 *  index, and an SPDK namespace has no path at all. The offset lives in the request's extent, so
 *  what a place carries is the slot and the generation that separates it from its predecessor.
 */
struct medium_reference_t {

    /** The medium the bytes move against, which the queue pair obtained from @c register_medium. */
    registered_medium_id_t medium_id = registered_medium_id_t::unregistered_k;

    /** The queue pair generation of that medium, so a stale place resolves to nothing. */
    queue_pair_generation_t generation = 0;
};

/**
 *  @brief One command the queue pair submits, borrowing a registered buffer until its token retires.
 *
 *  The ring never copies the bytes and never owns them. From the moment @c submit accepts a request
 *  until its completion is completed, the registered range behind @p buffer belongs to the device: the
 *  kernel or the drive may write into it at any instant, so the queue pair may not free, move, resize or
 *  reuse it. That window is enforced where it can be - a range with work in flight cannot be
 *  deregistered - which is why a request names a registered memory rather than a pointer.
 */
struct request_t {

    /** The registered medium the bytes move to or from, which one disk may hold many of. */
    medium_reference_t medium;

    /** The range inside that medium, empty for a flush. */
    extent_t extent;

    /** Where the bytes are read into, or written from; ignored by a flush. */
    memory_reference_t buffer;

    /** What the device is asked to do. */
    request_kind_t kind = request_kind_t::read_k;

    /** Whether the next request of the same batch waits for this one. */
    request_chaining_t chaining = request_chaining_t::independent_k;

    /** The bits that come back on the completion, minted by the queue pair. */
    request_token_t token;
};

/**
 *  @brief How one request ended, delivered only to the queue pair named in its token.
 *
 *  A partial transfer is a state and not a failure: a positioned read that meets the end of a
 *  volume, and a write that meets a full one, both move a prefix and stop. The ring guarantees the
 *  first @p transferred bytes of the buffer and nothing else: what lies past them is unspecified,
 *  since an @c O_DIRECT read zeroes the part of its range that has no volume behind it while a
 *  buffered one leaves those bytes alone. A caller that needs the whole extent tests @c whole and
 *  reissues the remainder, and a block layer treats anything short of the whole block as a refusal.
 */
struct completion_t {

    /** The token of the request this completion belongs to. */
    request_token_t token;

    /** Bytes actually moved, which is the length of the buffer prefix the device touched. */
    std::uint64_t transferred_bytes = 0;

    /** Why it ended this way, a success for @c completed_k and @c partial_k alike. */
    status_t status;

    /** How it ended, which the queue pair branches on. */
    completion_state_t state = completion_state_t::completed_k;

    /** Whether the whole extent moved, which is the only state a block read may trust. */
    [[nodiscard]] bool completed_in_full() const noexcept { return state == completion_state_t::completed_k; }

    /** Whether this completion retires the token, which a multishot transport's interim ones do not. */
    [[nodiscard]] bool retires() const noexcept { return state != completion_state_t::continued_k; }
};

/** A read of @p extent of @p medium into @p buffer, tagged with @p token. */
[[nodiscard]] constexpr request_t read_request(medium_reference_t medium, extent_t extent, memory_reference_t buffer,
                                               request_token_t token) noexcept {
    return request_t {medium, extent, buffer, request_kind_t::read_k, request_chaining_t::independent_k, token};
}

/** A write of @p extent of @p medium from @p buffer, tagged with @p token. */
[[nodiscard]] constexpr request_t write_request(medium_reference_t medium, extent_t extent, memory_reference_t buffer,
                                                request_token_t token) noexcept {
    return request_t {medium, extent, buffer, request_kind_t::write_k, request_chaining_t::independent_k, token};
}

/** A flush of everything this queue pair has already completed of @p medium, tagged with @p token. */
[[nodiscard]] constexpr request_t flush_request(medium_reference_t medium, request_token_t token) noexcept {
    return request_t {
        medium, extent_t {}, memory_reference_t {}, request_kind_t::flush_k, request_chaining_t::independent_k, token};
}

/** A deallocation of @p extent of @p medium, which names no buffer, tagged with @p token. */
[[nodiscard]] constexpr request_t deallocate_request(medium_reference_t medium, extent_t extent,
                                                     request_token_t token) noexcept {
    return request_t {
        medium, extent, memory_reference_t {}, request_kind_t::deallocate_k, request_chaining_t::independent_k, token};
}

#pragma endregion Requests

#pragma region Queue Pairs

/**
 *  @brief The move-only claim on one record of a disk's pool: the record's index and the generation
 *      it was taken at.
 *
 *  A disk builds it from its pool's handle and empties it with @c surrender once it has vacated the
 *  record, so a handle that outlives its record never resolves to the one that took the index next.
 */
class disk_handle_t {
  public:

    /** The index an empty handle carries, which is every 16-bit pool's absent index. */
    static constexpr std::uint16_t absent_k = 0xFFFF;

    disk_handle_t() noexcept = default;
    disk_handle_t(disk_handle_t const&) = delete;
    disk_handle_t& operator=(disk_handle_t const&) = delete;

    /** The claim on record @p index of a pool, taken at @p generation, as the pool's
     *  @c handle_t names it. */
    disk_handle_t(std::uint16_t index, std::uint16_t generation) noexcept : index_(index), generation_(generation) {}

    disk_handle_t(disk_handle_t&& other) noexcept
        : index_(std::exchange(other.index_, absent_k)), generation_(std::exchange(other.generation_, 0)) {}

    disk_handle_t& operator=(disk_handle_t&& other) noexcept {
        assert(!held() && "Give a record back to its disk before overwriting its handle");
        index_ = std::exchange(other.index_, absent_k);
        generation_ = std::exchange(other.generation_, 0);
        return *this;
    }

    ~disk_handle_t() noexcept { assert(!held() && "Give a record back to its disk before destroying its handle"); }

    /** Empties the handle once the disk has vacated the record; callers never call this. */
    void surrender() noexcept {
        index_ = absent_k;
        generation_ = 0;
    }

    /** Whether this handle still names a record. */
    [[nodiscard]] bool held() const noexcept { return index_ != absent_k; }

    /** The record's index in its pool. */
    [[nodiscard]] std::uint16_t index() const noexcept { return index_; }

    /** The generation the record was taken at, which its vacate steps. */
    [[nodiscard]] std::uint16_t generation() const noexcept { return generation_; }

  private:

    /** The record's index, @c absent_k once surrendered. */
    std::uint16_t index_ = absent_k;

    /** The generation the record was taken at. */
    std::uint16_t generation_ = 0;
};

/**
 *  @brief One queue pair on a share of the ring's depth, held by exactly one thread at a time.
 *
 *  The queue pair is the queue pair identity the transports demand, not a convenience: an io_uring
 *  ring set up with @c SINGLE_ISSUER refuses a foreign reaper, and an SPDK queue pair corrupts
 *  itself silently under one. Moving a queue pair hands it to another thread, which is the only
 *  legal handoff when a controller offers fewer queue pairs than the executor has threads; a queue
 *  pair is never shared. The receiving thread announces itself with the disk's @c adopt, since a
 *  ring cannot observe a move and an unannounced one is indistinguishable from two threads sharing
 *  a queue pair.
 *
 *  @note Return the queue pair with the disk's @c release before the handle dies, since the handle cannot
 *      drain the device on its own.
 */
class queue_pair_t {
  public:

    queue_pair_t() noexcept = default;

    /** The handle a disk hands back from @c acquire_queue_pair; callers never call this. */
    queue_pair_t(disk_handle_t&& handle, queue_depth_t depth) noexcept : handle_(std::move(handle)), depth_(depth) {}

    /** Empties the handle once the disk has taken the queue pair back; callers never call this. */
    void surrender() noexcept { handle_.surrender(); }

    /** Whether this handle still holds a queue pair. */
    [[nodiscard]] bool held() const noexcept { return handle_.held(); }

    /** The slot this queue pair occupies. */
    [[nodiscard]] queue_pair_id_t id() const noexcept { return static_cast<queue_pair_id_t>(handle_.index()); }

    /** The generation that separates this queue pair from the slot's previous one. */
    [[nodiscard]] queue_pair_generation_t generation() const noexcept { return handle_.generation(); }

    /** Requests this queue pair may keep in flight at once, reserved when the queue pair was taken. */
    [[nodiscard]] queue_depth_t depth() const noexcept { return depth_; }

    /** Mints a token of this queue pair carrying the queue pair's own @p tag. */
    [[nodiscard]] request_token_t token(request_tag_t tag) const noexcept {
        return request_token_t::make(id(), generation(), tag);
    }

  private:

    /** The record the queue pair occupies in its disk's pool. */
    disk_handle_t handle_;

    /** Requests this queue pair may keep in flight at once. */
    queue_depth_t depth_ = 0;
};

/**
 *  @brief One registered memory range, which a request addresses by slot and offset.
 *
 *  A registered memory is the borrow the ring can enforce: while requests against it are in flight the
 *  disk refuses to give it back, so a buffer cannot be freed under a live DMA medium by accident.
 *  Registration is also where a transport says what it cannot reach, such as device memory on the
 *  blocking floor, and where it pins pages once instead of per request.
 */
class registered_memory_t {
  public:

    registered_memory_t() noexcept = default;

    /** The handle a disk hands back from @c register_memory; callers never call this. */
    registered_memory_t(disk_handle_t&& handle, std::span<byte_t> memory, memory_space_t space) noexcept
        : handle_(std::move(handle)), memory_(memory), space_(space) {}

    /** Empties the handle once the disk has taken the range back; callers never call this. */
    void surrender() noexcept {
        handle_.surrender();
        memory_ = std::span<byte_t> {};
    }

    /** Whether this handle still names a registered range. */
    [[nodiscard]] bool held() const noexcept { return handle_.held(); }

    /** The slot the range occupies. */
    [[nodiscard]] registered_memory_id_t id() const noexcept {
        return static_cast<registered_memory_id_t>(handle_.index());
    }

    /** The generation that separates this range from the slot's previous one. */
    [[nodiscard]] queue_pair_generation_t generation() const noexcept { return handle_.generation(); }

    /** The memory the range covers, which the queue pair allocated and still owns. */
    [[nodiscard]] std::span<byte_t> bytes() const noexcept { return memory_; }

    /** Where the range lives, which the disk agreed to at registration. */
    [[nodiscard]] memory_space_t space() const noexcept { return space_; }

    /** The place @p offset bytes into this range, which a request carries instead of a pointer. */
    [[nodiscard]] memory_reference_t reference(std::uint64_t offset = 0) const noexcept {
        return memory_reference_t {id(), generation(), offset};
    }

  private:

    /** The record the range occupies in its disk's pool. */
    disk_handle_t handle_;

    /** The memory the range covers, owned by the caller throughout. */
    std::span<byte_t> memory_;

    /** Where that memory lives. */
    memory_space_t space_ = memory_space_t::host_k;
};

/**
 *  @brief One registered medium, which a request addresses by slot rather than by name.
 *
 *  A medium is registered for the same reason a buffer is: @c IORING_REGISTER_FILES addresses a
 *  file by table index and skips the registered medium lookup every request would otherwise repeat,
 *  and an SPDK namespace has no name to pass at all. Registering it also makes the borrow visible,
 *  so a disk can refuse to close a medium that requests are still in flight against.
 */
class registered_medium_t {
  public:

    registered_medium_t() noexcept = default;

    /** The handle a disk hands back from @c register_medium; callers never call this. */
    registered_medium_t(disk_handle_t&& handle, std::uint64_t capacity, std::uint64_t logical_block_bytes) noexcept
        : handle_(std::move(handle)), capacity_(capacity), logical_block_bytes_(logical_block_bytes) {}

    /** Empties the handle once the disk has taken the medium back; callers never call this. */
    void surrender() noexcept {
        handle_.surrender();
        capacity_ = 0;
        logical_block_bytes_ = 0;
    }

    /** Whether this handle still names a registered medium. */
    [[nodiscard]] bool held() const noexcept { return handle_.held(); }

    /** The slot the medium occupies. */
    [[nodiscard]] registered_medium_id_t id() const noexcept {
        return static_cast<registered_medium_id_t>(handle_.index());
    }

    /** The generation that separates this medium from the slot's previous one. */
    [[nodiscard]] queue_pair_generation_t generation() const noexcept { return handle_.generation(); }

    /** Bytes the medium spans, which an extent past it reads short or fails. */
    [[nodiscard]] std::uint64_t capacity() const noexcept { return capacity_; }

    /** The smallest unit the medium writes atomically, which an on-disk block that must never
     *  tear fits inside. */
    [[nodiscard]] std::uint64_t logical_block_bytes() const noexcept { return logical_block_bytes_; }

    /** The place a request carries instead of a name. */
    [[nodiscard]] medium_reference_t reference() const noexcept { return medium_reference_t {id(), generation()}; }

  private:

    /** The record the medium occupies in its disk's pool. */
    disk_handle_t handle_;

    /** Bytes the medium spans, as the transport reported them at registration. */
    std::uint64_t capacity_ = 0;

    /** The medium's logical block, as the transport reported it at registration. */
    std::uint64_t logical_block_bytes_ = 0;
};

#pragma endregion Queue Pairs

#pragma region Capabilities

/**
 *  @brief What one disk accepts, probed at mount and never assumed.
 *
 *  The three alignments are genuinely different numbers: @c statx with @c STATX_DIOALIGN reports a
 *  memory alignment of 4 and an offset alignment of 512 on ext4 over a 512-byte-logical device,
 *  while a 4 Kn drive, SPDK's page-crossing rule and cuFile all want 4096. Aligning to
 *  @c portable_alignment_k satisfies every transport, so a caller that does not want to branch may
 *  use it and be accepted everywhere.
 */
struct ring_capabilities_t {

    /** Alignment the first byte of a registered buffer must meet. */
    std::uint64_t memory_alignment = 1;

    /** Alignment an extent's offset must meet. */
    std::uint64_t offset_alignment = 1;

    /** Multiple an extent's length must be. */
    std::uint64_t length_granularity = 1;

    /** Largest extent one request may carry. */
    std::uint64_t max_transfer_bytes = 0;

    /** Requests one queue pair may keep in flight at once. */
    queue_depth_t depth_limit = 0;

    /** Leases the ring can hand out at once, which a controller's queue-pair count bounds. */
    std::uint16_t queue_pairs_limit = 0;

    /** Targets the disk registers at once, which is one on a namespace and many in a directory. */
    std::uint16_t media_limit = 0;

    /** Whether the transport can register and address GPU memory. */
    bool reaches_gpu_memory = false;
};

#pragma endregion Capabilities

#pragma region Contract

/**
 *  @brief What every NVMe disk offers, so a caller compiles against the transport its mount chose.
 *
 *  Nothing here blocks. @c submit takes a prefix of the batch and answers how much it took, which is
 *  zero on a full ring; @c poll answers how many completions it moved, which is zero when none are
 *  ready. A bounded wait is a spin over @c poll, because SPDK has no interrupt at all and an
 *  io_uring ring with @c DEFER_TASKRUN only processes completions when its owning thread enters.
 */
template <typename disk_type_>
concept ring_concept = requires(disk_type_& disk, disk_type_ const& constant, queue_pair_t& queue_pair,
                                registered_memory_t& registered_memory, registered_medium_t& registered_medium,
                                medium_options_t medium, std::span<request_t const> requests,
                                std::span<completion_t> completions, std::span<byte_t> memory) {
    { disk.acquire_queue_pair(std::uint32_t {}) } -> std::same_as<expected<queue_pair_t>>;
    { disk.bind_queue_pair(queue_pair) } -> std::same_as<status_t>;
    { disk.release_queue_pair(queue_pair) } -> std::same_as<status_t>;
    { disk.register_memory(memory, memory_space_t::host_k) } -> std::same_as<expected<registered_memory_t>>;
    { disk.unregister_memory(registered_memory) } -> std::same_as<status_t>;
    { disk.register_medium(medium) } -> std::same_as<expected<registered_medium_t>>;
    { disk.unregister_medium(registered_medium) } -> std::same_as<status_t>;
    { disk.submit(queue_pair, requests) } -> std::same_as<expected<std::size_t>>;
    { disk.poll_completions(queue_pair, completions) } -> std::same_as<expected<std::size_t>>;
    { disk.cancel(queue_pair) } -> std::same_as<void>;
    { disk.drain(queue_pair) } -> std::same_as<status_t>;
    { constant.outstanding(queue_pair) } -> std::same_as<std::size_t>;
    { constant.capabilities() } -> std::same_as<ring_capabilities_t>;
};

/** Spins over a ring that never blocks, so a bounded wait needs no fiber and no interrupt. */
inline constexpr std::size_t poll_spin_limit_k = std::size_t {1} << 30;

/**
 *  @brief Reaps until @p wanted completions are in @p into, or the spin budget runs out.
 *  @param[in,out] disk The ring the queue pair polls from.
 *  @param[in] queue_pair The queue pair whose completions are wanted, held by the calling thread.
 *  @param[out] into Room for the completions, which bounds how many may be wanted.
 *  @param[in] wanted Completions to wait for, at most the size of @p into.
 *  @param[in] spins_limit Spins before the wait gives up, which a healthy ring never reaches.
 *  @return The completions moved into @p into, which is @p wanted unless the budget ran out.
 *  @tparam waiter_type_ What the polling core does after a poll that moved nothing.
 */
template <typename waiter_type_, typename disk_type_>
    requires ring_concept<disk_type_>
expected<std::size_t> poll_completions_at_least(disk_type_& disk, queue_pair_t const& queue_pair,
                                                std::span<completion_t> into, std::size_t wanted,
                                                std::size_t spins_limit = poll_spin_limit_k) noexcept {
    if (wanted > into.size())
        return status_t {status_code_t::invalid_argument_k, "A bounded wait needs room for what it wants"};
    std::size_t completed = 0;
    for (std::size_t spins = 0; completed < wanted && spins != spins_limit; ++spins) {
        auto [moved, poll_completions_status] = disk.poll_completions(queue_pair, into.subspan(completed));
        if (poll_completions_status.failed()) return poll_completions_status;
        if (moved == 0) waiter_type_ {}(&spins, spins, std::size_t {0});
        completed += moved;
    }
    return completed;
}

#pragma endregion Contract

#pragma endregion Ring

} // namespace unum::ustore::utilities
