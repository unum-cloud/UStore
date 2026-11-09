/**
 *  @file include/ustore/utilities/ram_buffers.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Growing arenas behind @c ustore_arena_t and behind the scratch calls work in, whose host
 *      blocks ForkUnion places on a memory domain.
 *
 *  It holds the allocator SmashTable's containers take over those blocks, the aligned and budgeted
 *  allocators of unbuffered I/O with the buffers they grow, and the column of strings grown there
 *  until it is exported.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int32_t`, `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`
#include <cstdlib> // `std::aligned_alloc`, `std::free`
#include <cstring> // `std::memcpy`, `std::memset`

#include "ustore/types.h" // `USTORE_OS_WINDOWS_`

#if USTORE_OS_WINDOWS_
#include <malloc.h> // `_aligned_malloc`, `_aligned_free`
#endif

#include <algorithm>   // `std::max`
#include <array>       // `std::array`
#include <concepts>    // `std::convertible_to`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::allocator_traits`
#include <new>         // placement `new`
#include <span>        // `std::span`
#include <type_traits> // `std::true_type`, `std::is_same_v`, `std::type_identity`
#include <utility>     // `std::exchange`
#include <variant>     // `std::variant`, `std::get_if`

#include <forkunion/atomics.hpp> // `std::memory_order_acquire`, `std::memory_order_release`
#include <forkunion/forkunion.h>
#include <smashtable/basic_vector.hpp> // `st::vector`
#include <smashtable/shared.hpp>       // `st::default_allocator`
#include <stringzilla/capabilities.h>  // `sz_capability_t`, `sz_cap_serial_k`
#include <stringzilla/find.h>          // `sz_find_byteset_serial`
#include <stringzilla/hash.h>          // `sz_hash_serial`
#include <stringzilla/sort.h>          // `sz_sort_order_serial`
#include <stringzilla/strings.h>       // `sz_strings_unescape_serial`, `sz_strings_escape_serial`
#include <stringzilla/types.h>         // `sz_allocator_t`
#include <stringzilla/utf8_runes.h>    // `sz_utf8_count_strings_serial`

#include "ustore/utilities/ram_device.hpp" // `claim_t`
#include "ustore/utilities/types.hpp"      // `expected`, `status_t`, `round_up_to_multiple`, `u32_tape`, `u64_tape`
#include "ustore/utilities/tapes.hpp"      // `assemble_tape`, `tape_source_t`

/** An arena made by @c ustore_arena_init: the memory where its executor computes, and the words its calls synchronize
 *  on. */
struct ustore_arena_handle_t {

    /** One block of a host arena, which bump-allocates past this header. */
    struct block_t {

        /** Older and smaller block of the chain, or null for the first one. */
        block_t* previous;

        /** Bytes the block spans, its header included. */
        std::size_t capacity;

        /** Bytes handed out so far, the header included. */
        std::size_t used;
    };

    /** The memory of an arena on host pages: its chain of blocks, newest first, on one memory domain, zeroed until the
     *  executor making the arena names its domain. */
    struct host_blocks_t {

        /** The newest and largest block, or null until a call writes. */
        block_t* newest;

        /** The memory domain every block lives on: one its executor serves, or zero for calls on the calling
         *  thread. */
        fu_memory_domain_id_t memory_domain;
    };

    /** One run of device memory, which the host bumps without touching it. */
    struct device_block_t {
        std::uintptr_t address;
        std::size_t capacity;
    };

    /** The memory of an arena on one device: its runs listed on the host, oldest first, of which only the newest is
     *  bumped, so every older one is full. */
    struct device_blocks_t {

        /** Runs an arena lists at most, as each at least doubles the one before it from a page. */
        static constexpr std::size_t blocks_capacity_k = 32;

        /** How many of @c blocks are listed. */
        std::uint32_t count = 0;

        /** Bytes handed out of the newest run. */
        std::size_t used = 0;

        std::array<device_block_t, blocks_capacity_k> blocks {};
    };

    /** The memory of an arena on one NVIDIA device, which a VRAM executor makes and its towers carve. */
    struct cuda_blocks_t {

        /** The CUDA ordinal of the device every run lives on. */
        std::int32_t device = 0;

        device_blocks_t blocks;
    };

    /** The memory of an arena on one AMD device, which a VRAM executor makes and its towers carve. */
    struct hip_blocks_t {

        /** The HIP ordinal of the device every run lives on. */
        std::int32_t device = 0;

        device_blocks_t blocks;
    };

    /** Whether a call fills the arena, so a second call naming it finds it contended. */
    claim_t claim = claim_t::free_k;

    /** Executor calls naming the arena that have not ended, through the floor class's reference. */
    std::uint32_t named_calls = 0;

    /** Host pages for the calls of a host executor, or one device's memory for those of a VRAM executor. */
    std::variant<host_blocks_t, cuda_blocks_t, hip_blocks_t> memory;
};

namespace unum::ustore::utilities {

/** One block of a host arena. */
using arena_block_t = ustore_arena_handle_t::block_t;

/** The memory of an arena on host pages. */
using host_blocks_t = ustore_arena_handle_t::host_blocks_t;

/** One run of device memory. */
using device_block_t = ustore_arena_handle_t::device_block_t;
static_assert(sizeof(device_block_t) == 16);

/** The runs of device memory an arena or an executor bumps. */
using device_blocks_t = ustore_arena_handle_t::device_blocks_t;

/** The memory of an arena on one CUDA device. */
using cuda_blocks_t = ustore_arena_handle_t::cuda_blocks_t;

/** The memory of an arena on one AMD device. */
using hip_blocks_t = ustore_arena_handle_t::hip_blocks_t;

/** Where every allocation in an arena starts, as Apache Arrow aligns its buffers, so typed outputs read in place. */
constexpr std::size_t arena_alignment_k = 64;

/** Bytes of the smallest page a supported system maps, which every block spans a multiple of. */
constexpr std::size_t arena_page_bytes_k = 4096;

/** How many times larger each block is than the one before it, at least. */
constexpr std::size_t arena_growth_factor_k = 2ul;

/** Bumps @p length bytes aligned to @p alignment off @p block, or answers null when they do not fit. */
inline void* arena_bump(arena_block_t& block, std::size_t length, std::size_t alignment) noexcept {
    auto const begin = reinterpret_cast<std::uintptr_t>(&block);
    std::uintptr_t const start = round_up_to_multiple<std::uintptr_t>(begin + block.used, alignment);
    if (start + length > begin + block.capacity) return nullptr;
    block.used = start + length - begin;
    return reinterpret_cast<void*>(start);
}

/** Carves @p count uninitialized elements off @p blocks at a multiple of @p alignment, opening a block once the newest
 *  one is full: the first spans the allocation rounded up to a page, so a one-off arena of a small call takes one
 *  page, and every later one at least doubles the newest. */
template <typename element_type_>
expected<std::span<element_type_>> carve(host_blocks_t& blocks, std::size_t count,
                                         std::size_t alignment = arena_alignment_k) noexcept {
    if (!count) return std::span<element_type_> {};
    std::size_t const length = sizeof(element_type_) * count;
    arena_block_t* const newest = blocks.newest;
    void* bytes = newest ? arena_bump(*newest, length, alignment) : nullptr;
    if (!bytes) {
        std::size_t const grown = newest ? newest->capacity * arena_growth_factor_k : 0;
        std::size_t const capacity = round_up_to_multiple(std::max(grown, length + alignment + sizeof(arena_block_t)),
                                                          arena_page_bytes_k);
        void* memory = nullptr;
        if (fu_allocate_on_domain_id(blocks.memory_domain, capacity, &memory) != fu_success_k || !memory)
            return status_t {status_code_t::out_of_memory_k, "Failed to allocate arena memory"};
        blocks.newest = new (memory) arena_block_t {newest, capacity, sizeof(arena_block_t)};
        bytes = arena_bump(*blocks.newest, length, alignment);
    }
    return std::span<element_type_> {static_cast<element_type_*>(bytes), count};
}

/** Runs of managed memory, which the host and one device both reach, as StringZilla's device engines read what the
 *  host lays out. */
struct managed_blocks_t {
    device_blocks_t blocks;
};

/** Bumps @p padded bytes, a multiple of @c arena_alignment_k, off the newest run of @p blocks, answering zero when they
 *  do not fit; the caller's driver opens the next run. */
inline std::uintptr_t bump(device_blocks_t& blocks, std::size_t padded) noexcept {
    if (!blocks.count) return 0;
    device_block_t const& newest = blocks.blocks[blocks.count - 1];
    if (newest.capacity - blocks.used < padded) return 0;
    std::uintptr_t const address = newest.address + blocks.used;
    blocks.used += padded;
    return address;
}

/** Bytes of the run to open for @p padded bytes that do not fit the newest of @p blocks: the request rounded up to a
 *  page, and at least double the newest run. */
inline std::size_t grown_capacity(device_blocks_t const& blocks, std::size_t padded) noexcept {
    std::size_t const grown = blocks.count ? blocks.blocks[blocks.count - 1].capacity * arena_growth_factor_k : 0;
    return round_up_to_multiple(std::max(grown, padded), arena_page_bytes_k);
}

/** The host blocks of @p arena, or null for an arena on a device. */
inline host_blocks_t* host_blocks_of(ustore_arena_t arena) noexcept {
    return arena ? std::get_if<host_blocks_t>(&arena->memory) : nullptr;
}

/** Allocates @p count uninitialized elements of host memory in @p arena, as @c carve does, refusing a null arena and
 *  one on a device. */
template <typename element_type_>
expected<std::span<element_type_>> arena_allocate(ustore_arena_t arena, std::size_t count,
                                                  std::size_t alignment = arena_alignment_k) noexcept {
    if (!count) return std::span<element_type_> {};
    if (!arena) return status_t {status_code_t::invalid_argument_k, "The call needs an arena for its outputs"};
    host_blocks_t* const blocks = host_blocks_of(arena);
    if (!blocks) return status_t {status_code_t::device_k, "The arena lives in device memory"};
    return carve<element_type_>(*blocks, count, alignment);
}

/** Allocates @p count bits in @p arena, every one unset, as presence and validity bitmaps start. */
inline expected<std::span<ustore_octet_t>> arena_allocate_bits(ustore_arena_t arena, std::size_t count) noexcept {
    auto [bits, status] = arena_allocate<ustore_octet_t>(arena, divide_round_up(count, bits_in_byte_k));
    if (status.failed()) return status;
    // An empty span may carry a null pointer, which `memset` must not receive even for zero bytes
    if (!bits.empty()) std::memset(bits.data(), 0, bits.size());
    return bits;
}

/** Grows @p elements of @p arena by @p additional elements, in place when they end its newest block and it has room,
 *  and as a copy otherwise, which leaves the old elements behind: only scratch grows, and outputs are copied in once
 *  with @c arena_copy. */
template <typename element_type_>
expected<std::span<element_type_>> arena_grow(ustore_arena_t arena, std::span<element_type_> elements,
                                              std::size_t additional,
                                              std::size_t alignment = arena_alignment_k) noexcept {
    host_blocks_t* const blocks = host_blocks_of(arena);
    arena_block_t* const block = blocks ? blocks->newest : nullptr;
    std::size_t const extension = sizeof(element_type_) * additional;
    bool const ends_block = block && static_cast<void const*>(elements.data() + elements.size()) ==
                                         reinterpret_cast<char const*>(block) + block->used;
    if (ends_block && block->capacity - block->used >= extension) {
        block->used += extension;
        return std::span<element_type_> {elements.data(), elements.size() + additional};
    }

    auto [grown, status] = arena_allocate<element_type_>(arena, elements.size() + additional, alignment);
    if (status.failed()) return status;
    std::memcpy(grown.data(), elements.data(), elements.size_bytes());
    return grown;
}

/** Copies @p elements into @p arena at their final length, as every output reaches the arena of its call once. */
template <typename element_type_>
expected<std::span<element_type_>> arena_copy(ustore_arena_t arena, std::span<element_type_ const> elements) noexcept {
    auto [copied, status] = arena_allocate<element_type_>(arena, elements.size());
    if (status.failed()) return status;
    if (!copied.empty()) std::memcpy(copied.data(), elements.data(), elements.size_bytes());
    return copied;
}

/** Frees every block of @p blocks but the newest, the largest, and empties that one, so they keep their high-water
 *  mark. */
inline void rewind(host_blocks_t& blocks) noexcept {
    arena_block_t* const newest = blocks.newest;
    if (!newest) return;
    for (arena_block_t* older = std::exchange(newest->previous, nullptr); older;) {
        arena_block_t* const freed = std::exchange(older, older->previous);
        fu_free_on_domain_id(blocks.memory_domain, freed, freed->capacity);
    }
    newest->used = sizeof(arena_block_t);
}

/** Rewinds the host memory of @p arena, leaving device runs to their executor, which frees them in stream order. */
inline void arena_rewind(ustore_arena_t arena) noexcept {
    if (host_blocks_t* const blocks = host_blocks_of(arena)) rewind(*blocks);
}

/** Frees every block of @p blocks newer than @p borrowed, their first block when the caller lent it, leaving them
 *  empty, as a scratch is freed with the executor or the call keeping it. */
inline void free_blocks(host_blocks_t& blocks, arena_block_t const* borrowed = nullptr) noexcept {
    for (arena_block_t* block = std::exchange(blocks.newest, nullptr); block != borrowed;) {
        arena_block_t* const freed = std::exchange(block, block->previous);
        fu_free_on_domain_id(blocks.memory_domain, freed, freed->capacity);
    }
}

/** Frees every run of @p blocks through @p free, called with each run's address, leaving them empty. */
template <typename free_type_>
void free_blocks(device_blocks_t& blocks, free_type_&& free) noexcept {
    for (std::uint32_t block = 0; block != blocks.count; ++block) free(blocks.blocks[block].address);
    blocks.count = 0, blocks.used = 0;
}

/** Frees every host block of @p arena newer than @p borrowed, leaving it empty; the runs of an arena on a device are
 *  freed through their vendor's driver, which the host library never reaches. */
inline void arena_free_blocks(ustore_arena_handle_t& arena, arena_block_t const* borrowed = nullptr) noexcept {
    if (host_blocks_t* const blocks = std::get_if<host_blocks_t>(&arena.memory)) free_blocks(*blocks, borrowed);
}

/**
 *  @brief The host arena a device executor's copies leave from and land in, whose blocks are pinned through the
 *      driver table @p api_type_ as they grow, so those copies run asynchronously.
 *
 *  The driver must answer zero for a pin it took and keep the context or device the pins were taken under current
 *  until the bounce dies.
 */
template <typename api_type_>
class pinned_bounce {
  public:
    pinned_bounce() noexcept = default;

    /** A bounce pinning through @p api, which outlives it, its blocks on @p memory_domain. */
    pinned_bounce(api_type_ const& api, fu_memory_domain_id_t memory_domain) noexcept : api_(&api) {
        arena_.memory = host_blocks_t {.newest = nullptr, .memory_domain = memory_domain};
    }

    pinned_bounce(pinned_bounce&& other) noexcept { swap(other); }
    pinned_bounce& operator=(pinned_bounce&& other) noexcept {
        swap(other);
        return *this;
    }
    pinned_bounce(pinned_bounce const&) = delete;
    pinned_bounce& operator=(pinned_bounce const&) = delete;
    ~pinned_bounce() noexcept {
        unpin(nullptr);
        arena_free_blocks(arena_);
    }

    /** The arena host verbs write into and copies leave from. */
    ustore_arena_t arena() noexcept { return &arena_; }

    /** Rewinds the arena to its newest block, unpinning every older one before it is freed. */
    void rewind() noexcept {
        host_blocks_t& blocks = std::get<host_blocks_t>(arena_.memory);
        unpin(blocks.newest);
        utilities::rewind(blocks);
    }

    /** Pins every block grown since the last pin. */
    status_t pin() noexcept {
        host_blocks_t& blocks = std::get<host_blocks_t>(arena_.memory);
        for (arena_block_t* block = blocks.newest; block != pinned_; block = block->previous)
            if (api_->register_host(block, block->capacity, 0) != 0) {
                for (arena_block_t* pinned = blocks.newest; pinned != block; pinned = pinned->previous)
                    api_->unregister_host(pinned);
                return {status_code_t::device_k, "The driver refused to pin the bounce"};
            }
        pinned_ = blocks.newest;
        return {};
    }

  private:

    /** Unpins every pinned block but @p kept, which stays pinned if it was. */
    void unpin(arena_block_t const* kept) noexcept {
        for (arena_block_t* block = pinned_; block; block = block->previous)
            if (block != kept) api_->unregister_host(block);
        if (pinned_ != kept) pinned_ = nullptr;
    }

    void swap(pinned_bounce& other) noexcept {
        std::swap(api_, other.api_), std::swap(arena_, other.arena_), std::swap(pinned_, other.pinned_);
    }

    api_type_ const* api_ = nullptr;
    ustore_arena_handle_t arena_ {};

    /** The newest block registered with the driver, every older one registered too, or null. */
    arena_block_t* pinned_ = nullptr;
};

/** Frees every block of @p arena, which lives on host pages, and the arena itself, where null is safe. */
inline void arena_free(ustore_arena_t arena) noexcept {
    if (!arena) return;
    arena_free_blocks(*arena);
    delete arena;
}

/** Allocates from an arena, which reclaims every block when it rewinds, so deallocation does nothing. */
template <typename value_type_>
struct arena_allocator {
    using value_type = value_type_;
    using propagate_on_container_move_assignment = std::true_type;

    /** The arena every rebound copy shares; null refuses every request. */
    ustore_arena_t arena = nullptr;

    arena_allocator(ustore_arena_t arena = nullptr) noexcept : arena(arena) {}

    /** The rebinding conversion @c std::allocator_traits performs, sharing the arena. */
    template <typename other_type_>
    arena_allocator(arena_allocator<other_type_> const& other) noexcept : arena(other.arena) {}

    value_type_* allocate(std::size_t count) noexcept {
        auto [run, status] = arena_allocate<value_type_>(arena, count);
        return status.failed() ? nullptr : run.data();
    }
    void deallocate(value_type_*, std::size_t) noexcept {}

    /** Whether both allocate from one arena, so either frees what the other allocated. */
    template <typename other_type_>
    bool operator==(arena_allocator<other_type_> const& other) const noexcept {
        return arena == other.arena;
    }
};

#pragma region Aligned Allocator

/**
 *  @brief Places every allocation on an @p alignment_ boundary, and rounds its size up to one.
 *
 *  Unbuffered reads and writes accept a buffer only when its address and its length are both
 *  multiples of the device's block size, which is what this allocator guarantees for every count.
 *  Exhaustion answers @c nullptr, as every allocator here does.
 */
template <typename value_type_, std::size_t alignment_>
class aligned_allocator {
  public:
    using value_type = value_type_;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;

    /** The boundary every allocation starts on, and the multiple its size is rounded up to. */
    static constexpr std::size_t alignment_k = alignment_;

    static_assert(alignment_k != 0 && (alignment_k & (alignment_k - 1)) == 0,
                  "An alignment is a power of two, as every block size is");

    constexpr aligned_allocator() noexcept = default;

    /** The rebinding conversion @c std::allocator_traits performs; the alignment is fixed. */
    template <typename other_type_>
    constexpr aligned_allocator(aligned_allocator<other_type_, alignment_> const&) noexcept {}

    /** Allocates room for @p count values on an aligned boundary, or answers @c nullptr. */
    [[nodiscard]] value_type_* allocate(std::size_t count) noexcept {
        if (count == 0 || count > max_count_k) return nullptr;
#if USTORE_OS_WINDOWS_
        return static_cast<value_type_*>(::_aligned_malloc(aligned_bytes(count), alignment_k));
#else
        return static_cast<value_type_*>(std::aligned_alloc(alignment_k, aligned_bytes(count)));
#endif
    }

    /** Releases @p pointer, which must come from @c allocate. */
    void deallocate(value_type_* pointer, std::size_t) noexcept {
#if USTORE_OS_WINDOWS_
        ::_aligned_free(pointer);
#else
        std::free(pointer);
#endif
    }

    /** The bytes @p count values occupy once rounded up to the alignment. */
    static constexpr std::size_t aligned_bytes(std::size_t count) noexcept {
        return (count * sizeof(value_type_) + alignment_k - 1) / alignment_k * alignment_k;
    }

    template <typename other_type_>
    constexpr bool operator==(aligned_allocator<other_type_, alignment_> const&) const noexcept {
        return true;
    }

  private:

    /** The largest count whose aligned byte size still fits a @c std::size_t. */
    static constexpr std::size_t max_count_k = (std::numeric_limits<std::size_t>::max() - alignment_k) /
                                               sizeof(value_type_);
};

#pragma endregion Aligned Allocator

#pragma region Constrained Allocator

/** A budget with no limit, which every allocation fits inside. */
inline constexpr std::size_t unlimited_resource_k = static_cast<std::size_t>(-1);

/**
 *  @brief The bytes one named part of the engine may hold, and the bytes it holds now.
 *
 *  A budget is acquired and released in bytes by however many threads share it, each acquisition
 *  bounded so the budget can never be overdrawn and an exhausted one refuses instead of waiting.
 *  Whoever owns the budget outlives every allocator charging it.
 *
 *  @tparam atomic_reference_ The reference the available bytes are owned through, for one operation
 *      at a time.
 */
template <template <typename> typename atomic_reference_>
class resource_constraint {
  public:

    /** A budget of @p capacity bytes, all of them available. */
    explicit resource_constraint(std::size_t capacity = unlimited_resource_k) noexcept
        : capacity_(capacity), available_(capacity) {}

    resource_constraint(resource_constraint const&) = delete;
    resource_constraint& operator=(resource_constraint const&) = delete;

    /** The bytes the budget allows in total. */
    std::size_t capacity() const noexcept { return capacity_; }

    /** The bytes the budget still allows. */
    std::size_t available() const noexcept {
        return atomic_reference_<std::size_t const> {available_}.load(std::memory_order_acquire);
    }

    /** The bytes the budget currently holds. */
    std::size_t used() const noexcept { return capacity_ - available(); }

    /** Whether @p bytes fit now, which a later acquisition may change. */
    bool has_more(std::size_t bytes) const noexcept { return bytes <= available(); }

    /** Charges @p bytes to the budget, answering whether they fit. */
    bool acquire(std::size_t bytes) noexcept {
        atomic_reference_<std::size_t> const reference {available_};
        std::size_t const observed = reference.fetch_sub_if_at_least(bytes, 0, std::memory_order_acquire);
        return observed >= bytes;
    }

    /** Returns @p bytes to the budget, answering whether they had been charged to it. */
    bool release(std::size_t bytes) noexcept {
        atomic_reference_<std::size_t> const reference {available_};
        std::size_t const observed = reference.fetch_add_if_at_most(bytes, capacity_, std::memory_order_release);
        return observed + bytes <= capacity_;
    }

  private:

    /** The bytes the budget allows in total, fixed when it is named. */
    std::size_t capacity_;

    /** The bytes the budget still allows, which every acquisition and release moves. */
    std::size_t available_;
};

/**
 *  @brief An allocator that charges a named budget, and refuses once the budget is spent.
 *
 *  Exhaustion answers @c nullptr, which is what every container here already checks, so a budget
 *  reaches a caller as a status rather than as an exception. The budget is borrowed, never owned.
 *
 *  @tparam atomic_reference_ The reference the budget owns its word through.
 */
template <typename value_type_, template <typename> typename atomic_reference_,
          typename base_allocator_type_ = st::default_allocator<value_type_>>
class constrained_allocator {
  public:
    using value_type = value_type_;
    using base_allocator_t = base_allocator_type_;

    /** The budget every allocation is charged to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    /** A moved container keeps charging the budget its allocator named. */
    using propagate_on_container_move_assignment = std::true_type;

    /** The same budget over values of @p other_type_, drawn from the base
     *  allocator rebound alike. */
    template <typename other_type_>
    struct rebind {
        using other =
            constrained_allocator<other_type_, atomic_reference_,
                                  typename std::allocator_traits<base_allocator_t>::template rebind_alloc<other_type_>>;
    };

    /** An allocator naming no budget, which refuses every allocation until a named
     *  one replaces it. */
    constrained_allocator() noexcept : base_(), constraint_(nullptr) {}

    /** An allocator charging @p constraint, which must outlive it. */
    explicit constrained_allocator(constraint_t& constraint, base_allocator_t base = {}) noexcept
        : base_(base), constraint_(&constraint) {}

    /** The rebinding conversion @c std::allocator_traits performs; the budget travels with it. */
    template <typename other_type_, typename other_base_type_>
    constrained_allocator(constrained_allocator<other_type_, atomic_reference_, other_base_type_> const& other) noexcept
        : base_(other.base()), constraint_(other.constraint_pointer()) {}

    /** Charges room for @p count values, or answers @c nullptr when the budget refuses them. */
    [[nodiscard]] value_type_* allocate(std::size_t count) noexcept {
        // A count whose byte size wraps would charge the budget for a small block that
        // reads as valid.
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(value_type_)) return nullptr;
        std::size_t const bytes = count * sizeof(value_type_);
        if (!constraint_ || !constraint_->acquire(bytes)) return nullptr;
        value_type_* const memory = base_.allocate(count);
        if (!memory) { [[maybe_unused]] bool const returned = constraint_->release(bytes); }
        return memory;
    }

    /** Releases @p pointer and returns its bytes to the budget. */
    void deallocate(value_type_* pointer, std::size_t count) noexcept {
        base_.deallocate(pointer, count);
        if (constraint_) { [[maybe_unused]] bool const returned = constraint_->release(count * sizeof(value_type_)); }
    }

    /** The budget this allocator charges. */
    constraint_t& constraint() const noexcept { return *constraint_; }

    /** The budget this allocator charges, or null while it names none. */
    constraint_t* constraint_pointer() const noexcept { return constraint_; }

    /** The allocator the charged bytes come from, which a rebinding conversion carries over. */
    base_allocator_t const& base() const noexcept { return base_; }

    template <typename other_type_, typename other_base_type_>
    bool operator==(
        constrained_allocator<other_type_, atomic_reference_, other_base_type_> const& other) const noexcept {
        return constraint_ == other.constraint_pointer();
    }

  private:

    /** The allocator the charged bytes actually come from. */
    base_allocator_t base_;

    /** The budget every allocation is charged to, owned by whoever named it. */
    constraint_t* constraint_;
};

#pragma endregion Constrained Allocator

#pragma region Buffers

/** Alignment and size multiple of buffers used for direct I/O. */
inline constexpr std::size_t buffer_alignment_k = 4096;

/** The allocator every unbudgeted container reaches, which answers null instead of throwing. */
template <typename value_type_>
using heap_allocator = st::default_allocator<value_type_>;

/** The allocator of buffers unbuffered reads and writes accept. */
template <typename value_type_>
using direct_io_allocator = aligned_allocator<value_type_, buffer_alignment_k>;

/** Bytes charged to no budget, with no alignment beyond the heap's own. */
using buffer_t = st::vector<std::byte>;

/** Bytes of a buffer unbuffered I/O accepts, charged to no budget. */
using aligned_buffer_t = st::vector<std::byte, direct_io_allocator<std::byte>>;

/** Bytes charged to a named budget. */
template <template <typename> typename atomic_reference_>
using constrained_buffer = st::vector<std::byte, constrained_allocator<std::byte, atomic_reference_>>;

/** Bytes of a buffer unbuffered I/O accepts, charged to a named budget. */
template <template <typename> typename atomic_reference_>
using constrained_aligned_buffer =
    st::vector<std::byte, constrained_allocator<std::byte, atomic_reference_, direct_io_allocator<std::byte>>>;

#pragma endregion Buffers

/** An arena one call keeps on its own stack, on the calling thread's memory, freeing every block it grew when it
 *  dies, as a fold's scratch is. */
class local_arena_t {
  public:
    local_arena_t() noexcept = default;
    local_arena_t(local_arena_t const&) = delete;
    local_arena_t& operator=(local_arena_t const&) = delete;
    ~local_arena_t() noexcept { arena_free_blocks(handle_); }

    /** An allocator drawing from this arena, valid while it lives. */
    [[nodiscard]] arena_allocator<byte_t> allocator() noexcept { return arena_allocator<byte_t> {&handle_}; }

  private:

    /** The chain of blocks, empty until something allocates. */
    ustore_arena_handle_t handle_ {};
};

/** The status of growing a SmashTable container, which fails only when its allocator runs out of memory. */
inline status_t growth_status(st::status_t status) noexcept {
    if (st::succeeded(status)) return {};
    return {status_code_t::out_of_memory_k, "Failed to grow a container"};
}

/**
 *  @brief Copies the @p count strings @p strings answers by index, @p total bytes in all,
 *      into @p arena as the output @p tape: their offsets, then their bytes, as every ragged
 *      output is written.
 *
 *  The offsets take 32 bits while the bytes fit in 4 GiB and 64 bits beyond, so the common tape costs half the
 *  offsets. A tape of no strings stays zeroed, as an output with nothing to report does.
 */
template <typename strings_type_>
status_t export_strings(ustore_arena_t arena, std::size_t count, std::size_t total, strings_type_ const& strings,
                        ustore_strings_view_t& tape) noexcept {
    if (!count) return {};
    auto const export_as = [&]<typename offset_type_>(std::type_identity<offset_type_>) noexcept {
        auto [offsets, offsets_status] = arena_allocate<offset_type_>(arena, count + 1);
        if (offsets_status.failed()) return offsets_status;
        auto [bytes, bytes_status] = arena_allocate<byte_t>(arena, total);
        if (bytes_status.failed()) return bytes_status;
        assemble_tape(
            count,
            [&](std::size_t index) noexcept {
                ustore_bytes_view_t const string = strings(index);
                return tape_source_t {reinterpret_cast<byte_t const*>(string.data), string.length};
            },
            offsets, bytes);
        if constexpr (std::is_same_v<offset_type_, std::uint32_t>) tape = u32_tape(bytes.data(), offsets.data());
        else tape = u64_tape(bytes.data(), offsets.data());
        return status_t {};
    };
    if (total <= std::numeric_limits<std::uint32_t>::max()) return export_as(std::type_identity<std::uint32_t> {});
    return export_as(std::type_identity<std::uint64_t> {});
}

/** Whether an appended string ends where its bytes do, or after a null byte the string then spans
 *  too. */
enum class string_end_t : bool { bare_k, null_terminated_k };

/** A column of strings that grows by appending, behind 64-bit offsets, until it is exported as a narrower tape. */
template <typename allocator_type_>
class growing_strings {
    using traits_t = std::allocator_traits<allocator_type_>;
    st::vector<ustore_length_t, typename traits_t::template rebind_alloc<ustore_length_t>> offsets_;
    st::vector<byte_t, typename traits_t::template rebind_alloc<byte_t>> bytes_;

  public:
    growing_strings() noexcept = default;
    explicit growing_strings(allocator_type_ const& allocator) noexcept : offsets_(allocator), bytes_(allocator) {}

    /** Appends a copy of @p string, which moves with the bytes as the column grows. */
    status_t push_back(ustore_bytes_view_t string) noexcept {
        if (offsets_.empty())
            if (status_t const status = growth_status(offsets_.emplace_back(ustore_length_t {0})); status.failed())
                return status;
        std::size_t const offset = bytes_.size();
        if (status_t const status = growth_status(bytes_.resize(offset + string.length)); status.failed())
            return status;
        if (string.length) std::memcpy(bytes_.data() + offset, string.data, string.length);
        return growth_status(offsets_.emplace_back(static_cast<ustore_length_t>(bytes_.size())));
    }

    /** Appends a copy of @p string, followed by a null byte the string then spans when @p end
     *  asks for one. */
    status_t push_back(ustore_bytes_view_t string, string_end_t end) noexcept {
        if (status_t const status = push_back(string); status.failed() || end == string_end_t::bare_k) return status;
        if (status_t const status = growth_status(bytes_.push_back(byte_t {0})); status.failed()) return status;
        offsets_[offsets_.size() - 1] = static_cast<ustore_length_t>(bytes_.size());
        return {};
    }

    /** How many strings the column holds. */
    std::size_t size() const noexcept { return offsets_.empty() ? 0 : offsets_.size() - 1; }

    /** The string at @p index, viewing bytes the next append may move. */
    ustore_bytes_view_t operator[](std::size_t index) const noexcept {
        ustore_length_t const begin = offsets_[index];
        return {reinterpret_cast<ustore_byte_t const*>(bytes_.data()) + begin, offsets_[index + 1] - begin};
    }

    /** The column as a u64 tape over its own bytes, which the C ABI reads in place until the next append. */
    ustore_strings_view_t tape() const noexcept { return u64_tape(bytes_.data(), offsets_.data()); }

    /** Copies the column into @p arena as a u32 or u64 tape, as narrow as its bytes allow. */
    status_t export_into(ustore_arena_t arena, ustore_strings_view_t& exported) const noexcept {
        auto const strings = [this](std::size_t index) noexcept { return (*this)[index]; };
        return export_strings(arena, size(), bytes_.size(), strings, exported);
    }
};

/** Serves @p bytes from the arena behind @p handle, as a StringZilla allocator does. */
inline void* stringzilla_allocate(sz_size_t bytes, void* handle, sz_stream_t) noexcept {
    auto [block, status] = arena_allocate<byte_t>(static_cast<ustore_arena_t>(handle), bytes,
                                                  alignof(std::max_align_t));
    return status.failed() ? nullptr : block.data();
}

/** Grows the block at @p pointer in place where the arena allows, as a StringZilla allocator does. */
inline void* stringzilla_reallocate(void* pointer, sz_size_t old_bytes, sz_size_t new_bytes, void* handle,
                                    sz_stream_t) noexcept {
    if (new_bytes <= old_bytes) return pointer;
    std::span<byte_t> const old_block {static_cast<byte_t*>(pointer), old_bytes};
    auto [block, status] = arena_grow(static_cast<ustore_arena_t>(handle), old_block, new_bytes - old_bytes,
                                      alignof(std::max_align_t));
    return status.failed() ? nullptr : block.data();
}

/** Arena memory is released with the arena, never one block at a time. */
inline void stringzilla_free(void*, sz_size_t, void*, sz_stream_t) noexcept {}

/** A StringZilla allocator serving every block from @p arena, which frees them all when it rewinds. */
inline sz_allocator_t stringzilla_allocator(ustore_arena_t arena) noexcept {
    return {stringzilla_allocate, stringzilla_reallocate, stringzilla_free, arena};
}

/** The StringZilla host kernels one CPU tier calls; a tier this build left out is a stub of
 *  @c capability_k and @c compiled_k. Each slot is the widest tier at or below the kit's
 *  that StringZilla ships. */
struct serial_strings_kit_t {
    static constexpr sz_capability_t capability_k = sz_cap_serial_k;
    static constexpr bool compiled_k = true;
    static constexpr auto sort_order = sz_sort_order_serial;
    static constexpr auto utf8_count_strings = sz_utf8_count_strings_serial;
    static constexpr auto find_byteset = sz_find_byteset_serial;
    static constexpr auto strings_unescape = sz_strings_unescape_serial;
    static constexpr auto strings_escape = sz_strings_escape_serial;
    static constexpr auto hash = sz_hash_serial;
    static constexpr auto sha256_state_init = sz_sha256_state_init_serial;
    static constexpr auto sha256_state_update = sz_sha256_state_update_serial;
    static constexpr auto sha256_state_digest = sz_sha256_state_digest_serial;
};

#if STRINGZILLA_TARGET_HASWELL && STRINGZILLA_TARGET_WESTMERE
struct haswell_strings_kit_t {
    static constexpr sz_capability_t capability_k = sz_cap_haswell_k;
    static constexpr bool compiled_k = true;
    static constexpr auto sort_order = sz_sort_order_haswell;
    static constexpr auto utf8_count_strings = sz_utf8_count_strings_serial;
    static constexpr auto find_byteset = sz_find_byteset_haswell;
    static constexpr auto strings_unescape = sz_strings_unescape_haswell;
    static constexpr auto strings_escape = sz_strings_escape_haswell;
    static constexpr auto hash = sz_hash_westmere;
    static constexpr auto sha256_state_init = sz_sha256_state_init_serial;
    static constexpr auto sha256_state_update = sz_sha256_state_update_serial;
    static constexpr auto sha256_state_digest = sz_sha256_state_digest_serial;
};
#else
struct haswell_strings_kit_t {
    static constexpr sz_capability_t capability_k = sz_cap_haswell_k;
    static constexpr bool compiled_k = false;
};
#endif

#if STRINGZILLA_TARGET_SKYLAKE && STRINGZILLA_TARGET_HASWELL && STRINGZILLA_TARGET_WESTMERE
struct skylake_strings_kit_t {
    static constexpr sz_capability_t capability_k = sz_cap_skylake_k;
    static constexpr bool compiled_k = true;
    static constexpr auto sort_order = sz_sort_order_skylake;
    static constexpr auto utf8_count_strings = sz_utf8_count_strings_serial;
    static constexpr auto find_byteset = sz_find_byteset_haswell;
    static constexpr auto strings_unescape = sz_strings_unescape_haswell;
    static constexpr auto strings_escape = sz_strings_escape_haswell;
    static constexpr auto hash = sz_hash_skylake;
    static constexpr auto sha256_state_init = sz_sha256_state_init_serial;
    static constexpr auto sha256_state_update = sz_sha256_state_update_serial;
    static constexpr auto sha256_state_digest = sz_sha256_state_digest_serial;
};
#else
struct skylake_strings_kit_t {
    static constexpr sz_capability_t capability_k = sz_cap_skylake_k;
    static constexpr bool compiled_k = false;
};
#endif

#if STRINGZILLA_TARGET_ICELAKE && STRINGZILLA_TARGET_SKYLAKE && STRINGZILLA_TARGET_GOLDMONT
struct icelake_strings_kit_t {
    static constexpr sz_capability_t capability_k = sz_cap_icelake_k;
    static constexpr bool compiled_k = true;
    static constexpr auto sort_order = sz_sort_order_skylake;
    static constexpr auto utf8_count_strings = sz_utf8_count_strings_icelake;
    static constexpr auto find_byteset = sz_find_byteset_icelake;
    static constexpr auto strings_unescape = sz_strings_unescape_icelake;
    static constexpr auto strings_escape = sz_strings_escape_icelake;
    static constexpr auto hash = sz_hash_icelake;
    static constexpr auto sha256_state_init = sz_sha256_state_init_goldmont;
    static constexpr auto sha256_state_update = sz_sha256_state_update_goldmont;
    static constexpr auto sha256_state_digest = sz_sha256_state_digest_goldmont;
};
#else
struct icelake_strings_kit_t {
    static constexpr sz_capability_t capability_k = sz_cap_icelake_k;
    static constexpr bool compiled_k = false;
};
#endif

#if STRINGZILLA_TARGET_NEON && STRINGZILLA_TARGET_NEONAES
struct neon_strings_kit_t {
    static constexpr sz_capability_t capability_k = sz_cap_neon_k;
    static constexpr bool compiled_k = true;
    static constexpr auto sort_order = sz_sort_order_neon;
    static constexpr auto utf8_count_strings = sz_utf8_count_strings_neon;
    static constexpr auto find_byteset = sz_find_byteset_neon;
    static constexpr auto strings_unescape = sz_strings_unescape_neon;
    static constexpr auto strings_escape = sz_strings_escape_neon;
    static constexpr auto hash = sz_hash_neonaes;
    static constexpr auto sha256_state_init = sz_sha256_state_init_serial;
    static constexpr auto sha256_state_update = sz_sha256_state_update_serial;
    static constexpr auto sha256_state_digest = sz_sha256_state_digest_serial;
};
#else
struct neon_strings_kit_t {
    static constexpr sz_capability_t capability_k = sz_cap_neon_k;
    static constexpr bool compiled_k = false;
};
#endif

#if STRINGZILLA_TARGET_RVV
struct rvv_strings_kit_t {
    static constexpr sz_capability_t capability_k = sz_cap_rvv_k;
    static constexpr bool compiled_k = true;
    static constexpr auto sort_order = sz_sort_order_rvv;
    static constexpr auto utf8_count_strings = sz_utf8_count_strings_serial;
    static constexpr auto find_byteset = sz_find_byteset_rvv;
    static constexpr auto strings_unescape = sz_strings_unescape_rvv;
    static constexpr auto strings_escape = sz_strings_escape_rvv;
    static constexpr auto hash = sz_hash_rvv;
    static constexpr auto sha256_state_init = sz_sha256_state_init_rvv;
    static constexpr auto sha256_state_update = sz_sha256_state_update_rvv;
    static constexpr auto sha256_state_digest = sz_sha256_state_digest_rvv;
};
#else
struct rvv_strings_kit_t {
    static constexpr sz_capability_t capability_k = sz_cap_rvv_k;
    static constexpr bool compiled_k = false;
};
#endif

/** Names the six slots every strings kit carries. */
template <typename strings_kit_type_>
concept strings_kit_concept = requires {
    { strings_kit_type_::capability_k } -> std::convertible_to<sz_capability_t>;
    strings_kit_type_::sort_order;
    strings_kit_type_::utf8_count_strings;
    strings_kit_type_::find_byteset;
    strings_kit_type_::strings_unescape;
    strings_kit_type_::strings_escape;
    strings_kit_type_::hash;
};

/** Steps a SHA-256 state through the kernels of @p strings_kit_type_, as an archive hashes every
 *  run it saves. */
template <typename strings_kit_type_>
struct strings_kit_sha256 {

    void init(sz_sha256_state_t& state) const noexcept {
        [[maybe_unused]] sz_status_t const started = strings_kit_type_::sha256_state_init(&state, nullptr);
    }

    void update(sz_sha256_state_t& state, std::span<byte_t const> bytes) const noexcept {
        [[maybe_unused]] sz_status_t const stepped = strings_kit_type_::sha256_state_update(
            &state, reinterpret_cast<sz_cptr_t>(bytes.data()), bytes.size(), nullptr);
    }

    [[nodiscard]] sha256_t digest(sz_sha256_state_t const& state) const noexcept {
        sha256_t digest;
        [[maybe_unused]] sz_status_t const finished = strings_kit_type_::sha256_state_digest(&state, digest.bytes,
                                                                                             nullptr);
        return digest;
    }
};

/** The widest strings kit this build compiled in among the tiers @p capabilities_ admits, newest
 *  first. */
template <sz_capability_t capabilities_>
    requires((capabilities_ & sz_cap_cpus_k) != 0)
struct strings_ladder {
    using strings_kit_t =
        typename highest_strings_kit<capabilities_, serial_strings_kit_t, icelake_strings_kit_t, skylake_strings_kit_t,
                                     haswell_strings_kit_t, neon_strings_kit_t, rvv_strings_kit_t>::type;
    static constexpr sz_capability_t picked_k = strings_kit_t::capability_k;
};

} // namespace unum::ustore::utilities
