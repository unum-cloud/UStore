/**
 *  @file src/nvme/ring.hpp
 *  @author Ash Vardanian
 *  @date September 18, 2026
 *  @brief The submission and completion ring every NVMe transport exposes, and the vocabulary it moves.
 *
 *  The ring is exposed rather than hidden behind a blocking read, because a blocking read reaches
 *  the depth of one thread and an NVMe namespace wants thousands of commands outstanding. An owner
 *  submits a batch, does other work, and reaps its own completions; nothing here ever blocks, so no
 *  fiber is needed to park a caller that ran out of credit.
 *
 *  The shape is what the three real transports allow rather than what is convenient. io_uring with
 *  @c SINGLE_ISSUER and @c DEFER_TASKRUN answers @c -EEXIST when a thread other than the submitter
 *  reaps, and an SPDK queue pair silently corrupts its tracker list in the same case, so an owner is
 *  a single-threaded lease rather than a label. Both address a buffer by a registration slot plus an
 *  offset - @c IORING_OP_READ_FIXED and @c cuFileRead alike - so a request names a registration and
 *  not a pointer. Neither reports a misaligned request at submission time, so every per-request
 *  defect arrives as a failed completion and @c submit refuses only what would corrupt bookkeeping.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <concepts> // `std::same_as`
#include <span>     // `std::span`
#include <thread>   // `std::this_thread::yield`
#include <utility>  // `std::exchange`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/extent.hpp" // `extent_t`, `portable_alignment_k`

namespace unum::ustore::nvme {

#pragma region Vocabulary

/** What a submitted request asks the device to do. */
enum class request_kind_t : std::uint8_t {

    /** Moves @c extent.length bytes from the volume into the registered buffer. */
    read_k,

    /** Moves @c extent.length bytes from the registered buffer onto the volume. */
    write_k,

    /** Makes every write this owner has already reaped durable; its extent is empty. */
    flush_k,
};

/**
 *  @brief Whether the next request of the same batch waits for this one.
 *
 *  Chaining is the only ordering the ring offers, and it costs what @c IOSQE_IO_LINK costs: a short
 *  or failed result cuts the rest of the chain, which arrives as @c cancelled_k. A chain orders a
 *  bounded sequence inside one submitted batch and never orders across batches or across owners.
 */
enum class request_chaining_t : std::uint8_t {

    /** The request may start, execute and complete in any order against its neighbours. */
    independent_k,

    /** The following request of the same batch starts only after this one transfers in full. */
    chained_k,
};

/** How a request ended, which is the state the owner branches on rather than the status alone. */
enum class completion_state_t : std::uint8_t {

    /** Every byte of the extent moved, and the status is a success. */
    completed_k,

    /** Fewer bytes than the extent asked for moved, and only that prefix is data. */
    partial_k,

    /** The owner cancelled before the request started, or a chain ahead of it was cut. */
    cancelled_k,

    /** Nothing usable moved, and the status says why. */
    failed_k,

    /** More completions follow for this token, so the token is not retired yet. */
    continued_k,
};

/**
 *  @brief Where a registered buffer lives, which is a property of the registration and never of a request.
 *
 *  A compaction read into device memory and a compaction read into host memory are one request
 *  shape; only the registration call differs - @c cuFileBufRegister against
 *  @c IORING_REGISTER_BUFFERS against @c spdk_mem_register - so a disk that cannot reach device
 *  memory refuses at registration, where the caller can still choose another buffer.
 */
enum class memory_space_t : std::uint8_t {

    /** Ordinary host memory, which every transport reaches. */
    host_k,

    /** GPU memory, which only a GPUDirect-capable disk registers. */
    device_k,
};

/** One owner's lease on a share of the ring's depth, or @c unowned_k for a handle that holds none. */
enum class owner_id_t : std::uint16_t {
    unowned_k = 0xFFFF,
};

/** One registered memory range, or @c unregistered_k for a handle that names none. */
enum class registration_id_t : std::uint16_t {
    unregistered_k = 0xFFFF,
};

/** One registered target the bytes move to or from, or @c unregistered_k for a handle that names none. */
enum class descriptor_id_t : std::uint16_t {
    unregistered_k = 0xFFFF,
};

/** What a caller may do to a target, which the transport fixes when it registers one. */
enum class target_access_t : std::uint8_t {

    /** Reads alone, so a write against this target fails rather than growing it. */
    read_only_k,

    /** Reads and writes against a target that must already exist. */
    read_write_k,

    /** Reads and writes, creating the target at the requested length when it is absent. */
    create_read_write_k,
};

/**
 *  @brief What a caller asks a transport to register as a target.
 *
 *  One shape serves all three transports because each reads only what it can: a file disk opens
 *  @c name as a path, a RAM disk keeps a buffer of @c length under that label, and a namespace
 *  ignores both, since the target it registers is the namespace it was mounted on.
 */
struct target_request_t {

    /** A path for a file disk, a label for a RAM disk, ignored by a namespace. */
    char const* name = nullptr;

    /** Bytes to create the target with when it is absent, or zero to take whatever is there. */
    std::uint64_t length = 0;

    /** What the caller may do to it once it is registered. */
    target_access_t access = target_access_t::read_write_k;
};

#pragma endregion Vocabulary

#pragma region Tokens

/**
 *  @brief The 64 bits that travel to the device and back, naming the owner and the owner's request.
 *
 *  io_uring hands @c user_data back verbatim and inspects nothing, so the layout is ours: the owner
 *  identifier and its lease generation let the ring route a completion to exactly one owner, and the
 *  tag is the owner's own, usually an index into its request table. The generation matters because a
 *  released lease is reissued, and a stale token must not deliver into its successor's queue.
 *
 *  @note A tag is unique among one owner's in-flight requests, since cancellation on a real ring
 *      matches on these bits and would otherwise cancel the wrong request.
 */
struct request_token_t {

    /** The owner in the high 16 bits, the lease generation in the next 16, the owner's tag in the low 32. */
    std::uint64_t bits = 0;

    /** Packs an owner, its lease generation and the owner's own tag into one token. */
    [[nodiscard]] static constexpr request_token_t make(owner_id_t owner, std::uint16_t generation,
                                                        std::uint32_t tag) noexcept {
        return request_token_t {(static_cast<std::uint64_t>(owner) << 48) |
                                (static_cast<std::uint64_t>(generation) << 32) | tag};
    }

    /** The owner this completion belongs to. */
    [[nodiscard]] constexpr owner_id_t owner() const noexcept {
        return static_cast<owner_id_t>(static_cast<std::uint16_t>(bits >> 48));
    }

    /** The lease generation, which separates an owner from its predecessor in the same slot. */
    [[nodiscard]] constexpr std::uint16_t generation() const noexcept { return static_cast<std::uint16_t>(bits >> 32); }

    /** The owner's own tag, which the ring never interprets. */
    [[nodiscard]] constexpr std::uint32_t tag() const noexcept { return static_cast<std::uint32_t>(bits); }

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
 *  before @c spdk_vtophys can translate it. Naming the registration also makes the borrow visible,
 *  so a disk can refuse to deregister memory that is still a live DMA target.
 */
struct buffer_place_t {

    /** The range the bytes sit in, which the owner obtained from @c register_chunk. */
    registration_id_t registration = registration_id_t::unregistered_k;

    /** The lease generation of that range, so a stale place resolves to nothing. */
    std::uint16_t generation = 0;

    /** Bytes from the start of the range to the first byte this request touches. */
    std::uint64_t offset = 0;
};

/**
 *  @brief Which registered target a request moves bytes against, named the way its buffer is.
 *
 *  Storage is addressed exactly as memory is, by a registered slot and an offset into it, because a
 *  path cannot be submitted any more than a pointer can: @c IORING_REGISTER_FILES wants a table
 *  index, and an SPDK namespace has no path at all. The offset lives in the request's extent, so
 *  what a place carries is the slot and the generation that separates it from its predecessor.
 */
struct target_place_t {

    /** The target the bytes move against, which the owner obtained from @c register_target. */
    descriptor_id_t descriptor = descriptor_id_t::unregistered_k;

    /** The lease generation of that target, so a stale place resolves to nothing. */
    std::uint16_t generation = 0;
};

/**
 *  @brief One command the owner submits, borrowing a registered buffer until its token retires.
 *
 *  The ring never copies the bytes and never owns them. From the moment @c submit accepts a request
 *  until its completion is reaped, the registered range behind @p buffer belongs to the device: the
 *  kernel or the drive may write into it at any instant, so the owner may not free, move, resize or
 *  reuse it. That window is enforced where it can be - a range with work in flight cannot be
 *  deregistered - which is why a request names a registration rather than a pointer.
 */
struct request_t {

    /** The registered target the bytes move to or from, which one disk may hold many of. */
    target_place_t target;

    /** The range inside that target, empty for a flush. */
    extent_t extent;

    /** Where the bytes are read into, or written from; ignored by a flush. */
    buffer_place_t buffer;

    /** What the device is asked to do. */
    request_kind_t kind = request_kind_t::read_k;

    /** Whether the next request of the same batch waits for this one. */
    request_chaining_t chaining = request_chaining_t::independent_k;

    /** The bits that come back on the completion, minted by the owner's lease. */
    request_token_t token;
};

/**
 *  @brief How one request ended, delivered only to the owner named in its token.
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
    std::uint64_t transferred = 0;

    /** Why it ended this way, a success for @c completed_k and @c partial_k alike. */
    status_t status;

    /** How it ended, which the owner branches on. */
    completion_state_t state = completion_state_t::completed_k;

    /** Whether the whole extent moved, which is the only state a block read may trust. */
    [[nodiscard]] bool whole() const noexcept { return state == completion_state_t::completed_k; }

    /** Whether this completion retires the token, which a multishot transport's interim ones do not. */
    [[nodiscard]] bool retires() const noexcept { return state != completion_state_t::continued_k; }
};

/** A read of @p extent of @p target into @p buffer, tagged with @p token. */
[[nodiscard]] constexpr request_t read_request(target_place_t target, extent_t extent, buffer_place_t buffer,
                                               request_token_t token) noexcept {
    return request_t {target, extent, buffer, request_kind_t::read_k, request_chaining_t::independent_k, token};
}

/** A write of @p extent of @p target from @p buffer, tagged with @p token. */
[[nodiscard]] constexpr request_t write_request(target_place_t target, extent_t extent, buffer_place_t buffer,
                                                request_token_t token) noexcept {
    return request_t {target, extent, buffer, request_kind_t::write_k, request_chaining_t::independent_k, token};
}

/** A flush of everything this owner has already reaped of @p target, tagged with @p token. */
[[nodiscard]] constexpr request_t flush_request(target_place_t target, request_token_t token) noexcept {
    return request_t {
        target, extent_t {}, buffer_place_t {}, request_kind_t::flush_k, request_chaining_t::independent_k, token};
}

#pragma endregion Requests

#pragma region Leases

/**
 *  @brief One owner's lease on a share of the ring's depth, held by exactly one thread at a time.
 *
 *  The lease is the owner identity the transports demand, not a convenience: an io_uring ring set up
 *  with @c SINGLE_ISSUER refuses a foreign reaper, and an SPDK queue pair corrupts itself silently
 *  under one. Moving a lease hands it to another thread, which is the only legal handoff when a
 *  controller offers fewer queue pairs than the executor has threads; a lease is never shared. The
 *  receiving thread announces itself with the disk's @c adopt, since a ring cannot observe a move and
 *  an unannounced one is indistinguishable from two threads sharing a lease.
 *
 *  @note Return the lease with the disk's @c release before the handle dies, since the handle cannot
 *      drain the device on its own.
 */
class ring_owner_t {
  public:

    ring_owner_t() noexcept = default;
    ring_owner_t(ring_owner_t const&) = delete;
    ring_owner_t& operator=(ring_owner_t const&) = delete;

    ring_owner_t(ring_owner_t&& other) noexcept
        : id_(std::exchange(other.id_, owner_id_t::unowned_k)), generation_(std::exchange(other.generation_, 0)),
          depth_(std::exchange(other.depth_, 0)) {}

    ring_owner_t& operator=(ring_owner_t&& other) noexcept {
        assert(!held() && "Release a lease before overwriting it");
        id_ = std::exchange(other.id_, owner_id_t::unowned_k);
        generation_ = std::exchange(other.generation_, 0);
        depth_ = std::exchange(other.depth_, 0);
        return *this;
    }

    ~ring_owner_t() noexcept { assert(!held() && "Release a lease before destroying it"); }

    /** Builds the handle a disk hands back from its @c open; callers never call this. */
    [[nodiscard]] static ring_owner_t leased(owner_id_t id, std::uint16_t generation, std::uint32_t depth) noexcept {
        ring_owner_t owner;
        owner.id_ = id;
        owner.generation_ = generation;
        owner.depth_ = depth;
        return owner;
    }

    /** Empties the handle once the disk has taken the lease back; callers never call this. */
    void surrender() noexcept {
        id_ = owner_id_t::unowned_k;
        generation_ = 0;
        depth_ = 0;
    }

    /** Whether this handle still holds a lease. */
    [[nodiscard]] bool held() const noexcept { return id_ != owner_id_t::unowned_k; }

    /** The slot this lease occupies. */
    [[nodiscard]] owner_id_t id() const noexcept { return id_; }

    /** The generation that separates this lease from the slot's previous one. */
    [[nodiscard]] std::uint16_t generation() const noexcept { return generation_; }

    /** Requests this owner may keep in flight at once, reserved when the lease was taken. */
    [[nodiscard]] std::uint32_t depth() const noexcept { return depth_; }

    /** Mints a token of this lease carrying the owner's own @p tag. */
    [[nodiscard]] request_token_t token(std::uint32_t tag) const noexcept {
        return request_token_t::make(id_, generation_, tag);
    }

  private:

    /** The slot the lease occupies, @c unowned_k once surrendered. */
    owner_id_t id_ = owner_id_t::unowned_k;

    /** The generation of this lease, raised every time the slot is leased again. */
    std::uint16_t generation_ = 0;

    /** Requests this owner may keep in flight at once. */
    std::uint32_t depth_ = 0;
};

/**
 *  @brief One registered memory range, which a request addresses by slot and offset.
 *
 *  A registration is the borrow the ring can enforce: while requests against it are in flight the
 *  disk refuses to give it back, so a buffer cannot be freed under a live DMA target by accident.
 *  Registration is also where a transport says what it cannot reach, such as device memory on the
 *  blocking floor, and where it pins pages once instead of per request.
 */
class registration_t {
  public:

    registration_t() noexcept = default;
    registration_t(registration_t const&) = delete;
    registration_t& operator=(registration_t const&) = delete;

    registration_t(registration_t&& other) noexcept
        : id_(std::exchange(other.id_, registration_id_t::unregistered_k)),
          generation_(std::exchange(other.generation_, 0)), chunk_(std::exchange(other.chunk_, std::span<byte_t> {})),
          space_(other.space_) {}

    registration_t& operator=(registration_t&& other) noexcept {
        assert(!held() && "Deregister a range before overwriting its handle");
        id_ = std::exchange(other.id_, registration_id_t::unregistered_k);
        generation_ = std::exchange(other.generation_, 0);
        chunk_ = std::exchange(other.chunk_, std::span<byte_t> {});
        space_ = other.space_;
        return *this;
    }

    ~registration_t() noexcept { assert(!held() && "Deregister a range before destroying its handle"); }

    /** Builds the handle a disk hands back from its @c register_chunk; callers never call this. */
    [[nodiscard]] static registration_t registered(registration_id_t id, std::uint16_t generation,
                                                   std::span<byte_t> chunk, memory_space_t space) noexcept {
        registration_t registration;
        registration.id_ = id;
        registration.generation_ = generation;
        registration.chunk_ = chunk;
        registration.space_ = space;
        return registration;
    }

    /** Empties the handle once the disk has taken the range back; callers never call this. */
    void surrender() noexcept {
        id_ = registration_id_t::unregistered_k;
        generation_ = 0;
        chunk_ = std::span<byte_t> {};
    }

    /** Whether this handle still names a registered range. */
    [[nodiscard]] bool held() const noexcept { return id_ != registration_id_t::unregistered_k; }

    /** The slot the range occupies. */
    [[nodiscard]] registration_id_t id() const noexcept { return id_; }

    /** The generation that separates this range from the slot's previous one. */
    [[nodiscard]] std::uint16_t generation() const noexcept { return generation_; }

    /** The memory the range covers, which the owner allocated and still owns. */
    [[nodiscard]] std::span<byte_t> chunk() const noexcept { return chunk_; }

    /** Where the range lives, which the disk agreed to at registration. */
    [[nodiscard]] memory_space_t space() const noexcept { return space_; }

    /** The place @p offset bytes into this range, which a request carries instead of a pointer. */
    [[nodiscard]] buffer_place_t place(std::uint64_t offset = 0) const noexcept {
        return buffer_place_t {id_, generation_, offset};
    }

  private:

    /** The slot the range occupies, @c unregistered_k once surrendered. */
    registration_id_t id_ = registration_id_t::unregistered_k;

    /** The generation of this registration, raised every time the slot is used again. */
    std::uint16_t generation_ = 0;

    /** The memory the range covers, owned by the caller throughout. */
    std::span<byte_t> chunk_;

    /** Where that memory lives. */
    memory_space_t space_ = memory_space_t::host_k;
};

/**
 *  @brief One registered target, which a request addresses by slot rather than by name.
 *
 *  A target is registered for the same reason a buffer is: @c IORING_REGISTER_FILES addresses a file
 *  by table index and skips the descriptor lookup every request would otherwise repeat, and an SPDK
 *  namespace has no name to pass at all. Registering it also makes the borrow visible, so a disk can
 *  refuse to close a target that requests are still in flight against.
 */
class descriptor_t {
  public:

    descriptor_t() noexcept = default;
    descriptor_t(descriptor_t const&) = delete;
    descriptor_t& operator=(descriptor_t const&) = delete;

    descriptor_t(descriptor_t&& other) noexcept
        : id_(std::exchange(other.id_, descriptor_id_t::unregistered_k)),
          generation_(std::exchange(other.generation_, 0)), capacity_(std::exchange(other.capacity_, 0)) {}

    descriptor_t& operator=(descriptor_t&& other) noexcept {
        assert(!held() && "Deregister a target before overwriting its handle");
        id_ = std::exchange(other.id_, descriptor_id_t::unregistered_k);
        generation_ = std::exchange(other.generation_, 0);
        capacity_ = std::exchange(other.capacity_, 0);
        return *this;
    }

    ~descriptor_t() noexcept { assert(!held() && "Deregister a target before destroying its handle"); }

    /** Builds the handle a disk hands back from its @c register_target; callers never call this. */
    [[nodiscard]] static descriptor_t registered(descriptor_id_t id, std::uint16_t generation,
                                                 std::uint64_t capacity) noexcept {
        descriptor_t descriptor;
        descriptor.id_ = id;
        descriptor.generation_ = generation;
        descriptor.capacity_ = capacity;
        return descriptor;
    }

    /** Empties the handle once the disk has taken the target back; callers never call this. */
    void surrender() noexcept {
        id_ = descriptor_id_t::unregistered_k;
        generation_ = 0;
        capacity_ = 0;
    }

    /** Whether this handle still names a registered target. */
    [[nodiscard]] bool held() const noexcept { return id_ != descriptor_id_t::unregistered_k; }

    /** The slot the target occupies. */
    [[nodiscard]] descriptor_id_t id() const noexcept { return id_; }

    /** The generation that separates this target from the slot's previous one. */
    [[nodiscard]] std::uint16_t generation() const noexcept { return generation_; }

    /** Bytes the target spans, which an extent past it reads short or fails. */
    [[nodiscard]] std::uint64_t capacity() const noexcept { return capacity_; }

    /** The place a request carries instead of a name. */
    [[nodiscard]] target_place_t place() const noexcept { return target_place_t {id_, generation_}; }

  private:

    /** The slot the target occupies, @c unregistered_k once surrendered. */
    descriptor_id_t id_ = descriptor_id_t::unregistered_k;

    /** The generation of this registration, raised every time the slot is used again. */
    std::uint16_t generation_ = 0;

    /** Bytes the target spans, as the transport reported them at registration. */
    std::uint64_t capacity_ = 0;
};

#pragma endregion Leases

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

    /** Requests one owner may keep in flight at once. */
    std::uint32_t depth_limit = 0;

    /** Leases the ring can hand out at once, which a controller's queue-pair count bounds. */
    std::uint16_t owners_limit = 0;

    /** Targets the disk registers at once, which is one on a namespace and many in a directory. */
    std::uint16_t targets_limit = 0;

    /** Whether the transport can register and address GPU memory. */
    bool reaches_device_memory = false;
};

#pragma endregion Capabilities

#pragma region Contract

/**
 *  @brief What every NVMe disk offers, so a caller compiles against the transport its mount chose.
 *
 *  Nothing here blocks. @c submit takes a prefix of the batch and answers how much it took, which is
 *  zero on a full ring; @c reap answers how many completions it moved, which is zero when none are
 *  ready. A bounded wait is a spin over @c reap, because SPDK has no interrupt at all and an
 *  io_uring ring with @c DEFER_TASKRUN only processes completions when its owning thread enters.
 */
template <typename disk_type_>
concept ring_concept = requires(disk_type_& disk, disk_type_ const& constant, ring_owner_t& owner,
                                registration_t& registration, descriptor_t& descriptor, target_request_t target,
                                std::span<request_t const> requests, std::span<completion_t> completions,
                                std::span<byte_t> chunk) {
    { disk.open(std::uint32_t {}) } -> std::same_as<expected<ring_owner_t>>;
    { disk.adopt(owner) } -> std::same_as<status_t>;
    { disk.release(owner) } -> std::same_as<status_t>;
    { disk.register_chunk(chunk, memory_space_t::host_k) } -> std::same_as<expected<registration_t>>;
    { disk.deregister_chunk(registration) } -> std::same_as<status_t>;
    { disk.register_target(target) } -> std::same_as<expected<descriptor_t>>;
    { disk.deregister_target(descriptor) } -> std::same_as<status_t>;
    { disk.submit(owner, requests) } -> std::same_as<expected<std::size_t>>;
    { disk.reap(owner, completions) } -> std::same_as<expected<std::size_t>>;
    { disk.cancel(owner) } -> std::same_as<void>;
    { disk.drain(owner) } -> std::same_as<status_t>;
    { constant.in_flight(owner) } -> std::same_as<std::size_t>;
    { constant.capabilities() } -> std::same_as<ring_capabilities_t>;
};

/** Spins over a ring that never blocks, so a bounded wait needs no fiber and no interrupt. */
inline constexpr std::size_t reap_spin_limit_k = std::size_t {1} << 30;

/**
 *  @brief Reaps until @p wanted completions are in @p into, or the spin budget runs out.
 *  @param[inout] disk The ring the owner reaps from.
 *  @param[in] owner The lease whose completions are wanted, held by the calling thread.
 *  @param[out] into Room for the completions, which bounds how many may be wanted.
 *  @param[in] wanted Completions to wait for, at most the size of @p into.
 *  @param[in] spins_limit Spins before the wait gives up, which a healthy ring never reaches.
 *  @return The completions moved into @p into, which is @p wanted unless the budget ran out.
 */
template <typename disk_type_>
    requires ring_concept<disk_type_>
expected<std::size_t> reap_at_least(disk_type_& disk, ring_owner_t const& owner, std::span<completion_t> into,
                                    std::size_t wanted, std::size_t spins_limit = reap_spin_limit_k) noexcept {
    if (wanted > into.size())
        return status_t {status_code_t::invalid_argument_k, "A bounded wait needs room for what it wants"};
    std::size_t reaped = 0;
    for (std::size_t spins = 0; reaped < wanted && spins != spins_limit; ++spins) {
        expected<std::size_t> const moved = disk.reap(owner, into.subspan(reaped));
        if (!moved) return moved.status();
        if (*moved == 0) std::this_thread::yield();
        reaped += *moved;
    }
    return reaped;
}

#pragma endregion Contract

} // namespace unum::ustore::nvme
