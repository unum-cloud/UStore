/**
 *  @file src/nvme/ring.hpp
 *  @author Ash Vardanian
 *  @date September 18, 2026
 *  @brief The submission and completion ring every NVMe transport exposes, and the vocabulary it moves.
 *
 *  The ring is exposed rather than hidden behind a blocking read, because a blocking read reaches
 *  the depth of one thread and an NVMe namespace wants thousands of commands outstanding. An queue pair
 *  submits a batch, does other work, and polls its own completions; nothing here ever blocks, so no
 *  fiber is needed to park a caller that ran out of credit.
 *
 *  The shape is what the three real transports allow rather than what is convenient. io_uring with
 *  @c SINGLE_ISSUER and @c DEFER_TASKRUN answers @c -EEXIST when a thread other than the submitter
 *  polls, and an SPDK queue pair silently corrupts its tracker list in the same case, so a queue pair is
 *  a single-threaded queue pair rather than a label. Both address a buffer by a registered memory slot plus an
 *  offset - @c IORING_OP_READ_FIXED and @c cuFileRead alike - so a request names a registered memory and
 *  not a pointer. Neither reports a misaligned request at submission time, so every per-request
 *  defect arrives as a failed completion and @c submit refuses only what would corrupt bookkeeping.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <concepts> // `std::same_as`
#include <span>     // `std::span`
#include <utility>  // `std::exchange`

#include <forkunion/capabilities.hpp> // `fu::preferred_yield_t`

#include "ustore/status.hpp" // `status_t`, `status_code_t`, `expected`
#include "ustore/types.hpp"  // `byte_t`

#include "nvme/extent.hpp" // `extent_t`, `portable_alignment_k`

namespace unum::ustore::nvme {

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
 *  A compaction read into device memory and a compaction read into host memory are one request
 *  shape; only the registered memory call differs - @c cuFileBufRegister against
 *  @c IORING_REGISTER_BUFFERS against @c spdk_mem_register - so a disk that cannot reach device
 *  memory refuses at registered memory, where the caller can still choose another buffer.
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
 *  One shape serves all three transports because each reads only what it can: a file disk opens
 *  @c name as a path, a RAM disk keeps a buffer of @c length under that label, and a namespace
 *  ignores both, since the medium it registers is the namespace it was mounted on.
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
 *  before @c spdk_vtophys can translate it. Naming the registered memory also makes the borrow visible,
 *  so a disk can refuse to deregister memory that is still a live DMA medium.
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

#pragma endregion Requests

#pragma region Queue Pairs

/**
 *  @brief One queue pair on a share of the ring's depth, held by exactly one thread at a time.
 *
 *  The queue pair is the queue pair identity the transports demand, not a convenience: an io_uring ring set up
 *  with @c SINGLE_ISSUER refuses a foreign reaper, and an SPDK queue pair corrupts itself silently
 *  under one. Moving a queue pair hands it to another thread, which is the only legal handoff when a
 *  controller offers fewer queue pairs than the executor has threads; a queue pair is never shared. The
 *  receiving thread announces itself with the disk's @c adopt, since a ring cannot observe a move and
 *  an unannounced one is indistinguishable from two threads sharing a queue pair.
 *
 *  @note Return the queue pair with the disk's @c release before the handle dies, since the handle cannot
 *      drain the device on its own.
 */
class queue_pair_t {
  public:

    queue_pair_t() noexcept = default;
    queue_pair_t(queue_pair_t const&) = delete;
    queue_pair_t& operator=(queue_pair_t const&) = delete;

    queue_pair_t(queue_pair_t&& other) noexcept
        : id_(std::exchange(other.id_, queue_pair_id_t::unallocated_k)),
          generation_(std::exchange(other.generation_, 0)), depth_(std::exchange(other.depth_, 0)) {}

    queue_pair_t& operator=(queue_pair_t&& other) noexcept {
        assert(!held() && "Release a queue pair before overwriting it");
        id_ = std::exchange(other.id_, queue_pair_id_t::unallocated_k);
        generation_ = std::exchange(other.generation_, 0);
        depth_ = std::exchange(other.depth_, 0);
        return *this;
    }

    ~queue_pair_t() noexcept { assert(!held() && "Release a queue pair before destroying it"); }

    /** Builds the handle a disk hands back from its @c open; callers never call this. */
    [[nodiscard]] static queue_pair_t leased(queue_pair_id_t id, queue_pair_generation_t generation,
                                             queue_depth_t depth) noexcept {
        queue_pair_t queue_pair;
        queue_pair.id_ = id;
        queue_pair.generation_ = generation;
        queue_pair.depth_ = depth;
        return queue_pair;
    }

    /** Empties the handle once the disk has taken the queue pair back; callers never call this. */
    void surrender() noexcept {
        id_ = queue_pair_id_t::unallocated_k;
        generation_ = 0;
        depth_ = 0;
    }

    /** Whether this handle still holds a queue pair. */
    [[nodiscard]] bool held() const noexcept { return id_ != queue_pair_id_t::unallocated_k; }

    /** The slot this queue pair occupies. */
    [[nodiscard]] queue_pair_id_t id() const noexcept { return id_; }

    /** The generation that separates this queue pair from the slot's previous one. */
    [[nodiscard]] queue_pair_generation_t generation() const noexcept { return generation_; }

    /** Requests this queue pair may keep in flight at once, reserved when the queue pair was taken. */
    [[nodiscard]] queue_depth_t depth() const noexcept { return depth_; }

    /** Mints a token of this queue pair carrying the queue pair's own @p tag. */
    [[nodiscard]] request_token_t token(request_tag_t tag) const noexcept {
        return request_token_t::make(id_, generation_, tag);
    }

  private:

    /** The slot the queue pair occupies, @c unallocated_k once surrendered. */
    queue_pair_id_t id_ = queue_pair_id_t::unallocated_k;

    /** The generation of this queue pair, raised every time the slot is leased again. */
    queue_pair_generation_t generation_ = 0;

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
    registered_memory_t(registered_memory_t const&) = delete;
    registered_memory_t& operator=(registered_memory_t const&) = delete;

    registered_memory_t(registered_memory_t&& other) noexcept
        : id_(std::exchange(other.id_, registered_memory_id_t::unregistered_k)),
          generation_(std::exchange(other.generation_, 0)), memory_(std::exchange(other.memory_, std::span<byte_t> {})),
          space_(other.space_) {}

    registered_memory_t& operator=(registered_memory_t&& other) noexcept {
        assert(!held() && "Deregister a range before overwriting its handle");
        id_ = std::exchange(other.id_, registered_memory_id_t::unregistered_k);
        generation_ = std::exchange(other.generation_, 0);
        memory_ = std::exchange(other.memory_, std::span<byte_t> {});
        space_ = other.space_;
        return *this;
    }

    ~registered_memory_t() noexcept { assert(!held() && "Deregister a range before destroying its handle"); }

    /** Builds the handle a disk hands back from its @c register_memory; callers never call this. */
    [[nodiscard]] static registered_memory_t registered(registered_memory_id_t id, queue_pair_generation_t generation,
                                                        std::span<byte_t> memory, memory_space_t space) noexcept {
        registered_memory_t registered_memory;
        registered_memory.id_ = id;
        registered_memory.generation_ = generation;
        registered_memory.memory_ = memory;
        registered_memory.space_ = space;
        return registered_memory;
    }

    /** Empties the handle once the disk has taken the range back; callers never call this. */
    void surrender() noexcept {
        id_ = registered_memory_id_t::unregistered_k;
        generation_ = 0;
        memory_ = std::span<byte_t> {};
    }

    /** Whether this handle still names a registered range. */
    [[nodiscard]] bool held() const noexcept { return id_ != registered_memory_id_t::unregistered_k; }

    /** The slot the range occupies. */
    [[nodiscard]] registered_memory_id_t id() const noexcept { return id_; }

    /** The generation that separates this range from the slot's previous one. */
    [[nodiscard]] queue_pair_generation_t generation() const noexcept { return generation_; }

    /** The memory the range covers, which the queue pair allocated and still owns. */
    [[nodiscard]] std::span<byte_t> bytes() const noexcept { return memory_; }

    /** Where the range lives, which the disk agreed to at registered_memory. */
    [[nodiscard]] memory_space_t space() const noexcept { return space_; }

    /** The place @p offset bytes into this range, which a request carries instead of a pointer. */
    [[nodiscard]] memory_reference_t reference(std::uint64_t offset = 0) const noexcept {
        return memory_reference_t {id_, generation_, offset};
    }

  private:

    /** The slot the range occupies, @c unregistered_k once surrendered. */
    registered_memory_id_t id_ = registered_memory_id_t::unregistered_k;

    /** The generation of this registered memory, raised every time the slot is used again. */
    queue_pair_generation_t generation_ = 0;

    /** The memory the range covers, owned by the caller throughout. */
    std::span<byte_t> memory_;

    /** Where that memory lives. */
    memory_space_t space_ = memory_space_t::host_k;
};

/**
 *  @brief One registered medium, which a request addresses by slot rather than by name.
 *
 *  A medium is registered for the same reason a buffer is: @c IORING_REGISTER_FILES addresses a file
 *  by table index and skips the registered medium lookup every request would otherwise repeat, and an SPDK
 *  namespace has no name to pass at all. Registering it also makes the borrow visible, so a disk can
 *  refuse to close a medium that requests are still in flight against.
 */
class registered_medium_t {
  public:

    registered_medium_t() noexcept = default;
    registered_medium_t(registered_medium_t const&) = delete;
    registered_medium_t& operator=(registered_medium_t const&) = delete;

    registered_medium_t(registered_medium_t&& other) noexcept
        : id_(std::exchange(other.id_, registered_medium_id_t::unregistered_k)),
          generation_(std::exchange(other.generation_, 0)), capacity_(std::exchange(other.capacity_, 0)) {}

    registered_medium_t& operator=(registered_medium_t&& other) noexcept {
        assert(!held() && "Deregister a medium before overwriting its handle");
        id_ = std::exchange(other.id_, registered_medium_id_t::unregistered_k);
        generation_ = std::exchange(other.generation_, 0);
        capacity_ = std::exchange(other.capacity_, 0);
        return *this;
    }

    ~registered_medium_t() noexcept { assert(!held() && "Deregister a medium before destroying its handle"); }

    /** Builds the handle a disk hands back from its @c register_medium; callers never call this. */
    [[nodiscard]] static registered_medium_t registered(registered_medium_id_t id, queue_pair_generation_t generation,
                                                        std::uint64_t capacity) noexcept {
        registered_medium_t registered_medium;
        registered_medium.id_ = id;
        registered_medium.generation_ = generation;
        registered_medium.capacity_ = capacity;
        return registered_medium;
    }

    /** Empties the handle once the disk has taken the medium back; callers never call this. */
    void surrender() noexcept {
        id_ = registered_medium_id_t::unregistered_k;
        generation_ = 0;
        capacity_ = 0;
    }

    /** Whether this handle still names a registered medium. */
    [[nodiscard]] bool held() const noexcept { return id_ != registered_medium_id_t::unregistered_k; }

    /** The slot the medium occupies. */
    [[nodiscard]] registered_medium_id_t id() const noexcept { return id_; }

    /** The generation that separates this medium from the slot's previous one. */
    [[nodiscard]] queue_pair_generation_t generation() const noexcept { return generation_; }

    /** Bytes the medium spans, which an extent past it reads short or fails. */
    [[nodiscard]] std::uint64_t capacity() const noexcept { return capacity_; }

    /** The place a request carries instead of a name. */
    [[nodiscard]] medium_reference_t reference() const noexcept { return medium_reference_t {id_, generation_}; }

  private:

    /** The slot the medium occupies, @c unregistered_k once surrendered. */
    registered_medium_id_t id_ = registered_medium_id_t::unregistered_k;

    /** The generation of this registered memory, raised every time the slot is used again. */
    queue_pair_generation_t generation_ = 0;

    /** Bytes the medium spans, as the transport reported them at registered_memory. */
    std::uint64_t capacity_ = 0;
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
 *  @param[inout] disk The ring the queue pair polls from.
 *  @param[in] queue pair The queue pair whose completions are wanted, held by the calling thread.
 *  @param[out] into Room for the completions, which bounds how many may be wanted.
 *  @param[in] wanted Completions to wait for, at most the size of @p into.
 *  @param[in] spins_limit Spins before the wait gives up, which a healthy ring never reaches.
 *  @return The completions moved into @p into, which is @p wanted unless the budget ran out.
 */
template <typename disk_type_>
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
        if (moved == 0) fu::preferred_yield_t {}(spins, spins, std::size_t {0});
        completed += moved;
    }
    return completed;
}

#pragma endregion Contract

} // namespace unum::ustore::nvme
