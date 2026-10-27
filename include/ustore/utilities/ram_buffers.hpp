/**
 *  @file include/ustore/utilities/ram_buffers.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Growing arenas behind @c ustore_arena_t and behind the scratch calls work in, whose host blocks ForkUnion
 *      places on a memory domain, the allocator SmashTable's containers take over them, and the column of strings
 *      grown there until it is exported.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int32_t`, `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <algorithm>   // `std::max`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::allocator_traits`
#include <new>         // placement `new`
#include <span>        // `std::span`
#include <type_traits> // `std::true_type`, `std::is_same_v`, `std::type_identity`
#include <utility>     // `std::exchange`
#include <variant>     // `std::variant`, `std::get_if`

#include <forkunion.h>
#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <stringzilla/types.h>         // `sz_allocator_t`

#include "ustore/utilities/ram_device.hpp" // `claim_t`
#include "ustore/utilities/types.hpp"      // `expected`, `status_t`, `round_up_to_multiple`, `u32_tape`, `u64_tape`

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

    /** The memory of an arena on one CUDA device, which a VRAM executor makes and only device towers carve. */
    struct cuda_blocks_t {

        /** The CUDA ordinal every block lives on. */
        std::int32_t device;
    };

    /** Whether a call fills the arena, so a second call naming it finds it contended. */
    claim_t claim = claim_t::free_k;

    /** Deferred calls naming the arena that are not final, through `std::atomic_ref`. */
    std::uint32_t deferred_calls = 0;

    /** Host pages for the calls of a host executor, or one device's memory for those of a VRAM executor. */
    std::variant<host_blocks_t, cuda_blocks_t> memory;
};

namespace unum::ustore::utilities {

/** One block of a host arena. */
using arena_block_t = ustore_arena_handle_t::block_t;

/** The memory of an arena on host pages. */
using host_blocks_t = ustore_arena_handle_t::host_blocks_t;

/** The memory of an arena on one CUDA device. */
using cuda_blocks_t = ustore_arena_handle_t::cuda_blocks_t;

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

/** Carves @p count elements off one device's @p blocks, which only the device towers of a later wave do. */
template <typename element_type_>
expected<std::span<element_type_>> carve(cuda_blocks_t&, std::size_t count) noexcept {
    if (!count) return std::span<element_type_> {};
    return status_t {status_code_t::unsupported_k, "No engine writes outputs into device memory yet"};
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

/** Device blocks rewind once device towers carve them. */
inline void rewind(cuda_blocks_t&) noexcept {}

/** Rewinds the memory of @p arena wherever it lives. */
inline void arena_rewind(ustore_arena_t arena) noexcept {
    std::visit([](auto& memory) noexcept { rewind(memory); }, arena->memory);
}

/** Frees every block of @p blocks newer than @p borrowed, their first block when the caller lent it, leaving them
 *  empty, as a scratch is freed with the executor or the call keeping it. */
inline void free_blocks(host_blocks_t& blocks, arena_block_t const* borrowed = nullptr) noexcept {
    for (arena_block_t* block = std::exchange(blocks.newest, nullptr); block != borrowed;) {
        arena_block_t* const freed = std::exchange(block, block->previous);
        fu_free_on_domain_id(blocks.memory_domain, freed, freed->capacity);
    }
}

/** Device blocks hold nothing until device towers carve them. */
inline void free_blocks(cuda_blocks_t&) noexcept {}

/** Frees every block of @p arena newer than @p borrowed, leaving it empty, wherever it lives. */
inline void arena_free_blocks(ustore_arena_handle_t& arena, arena_block_t const* borrowed = nullptr) noexcept {
    if (host_blocks_t* const blocks = std::get_if<host_blocks_t>(&arena.memory)) free_blocks(*blocks, borrowed);
    else free_blocks(std::get<cuda_blocks_t>(arena.memory));
}

/** Frees every block of @p arena and the arena itself, where null is safe. */
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
 *  @brief Copies the @p count strings @p strings answers by index, @p total bytes in all, into @p arena as the output
 *      @p tape: their offsets, then their bytes, as every ragged output is written.
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
        offsets[0] = 0;
        for (std::size_t index = 0; index != count; ++index) {
            ustore_bytes_view_t const string = strings(index);
            if (string.length) std::memcpy(bytes.data() + offsets[index], string.data, string.length);
            offsets[index + 1] = static_cast<offset_type_>(offsets[index] + string.length);
        }
        if constexpr (std::is_same_v<offset_type_, std::uint32_t>) tape = u32_tape(bytes.data(), offsets.data());
        else tape = u64_tape(bytes.data(), offsets.data());
        return status_t {};
    };
    if (total <= std::numeric_limits<std::uint32_t>::max()) return export_as(std::type_identity<std::uint32_t> {});
    return export_as(std::type_identity<std::uint64_t> {});
}

/** A column of strings that grows by appending, behind 64-bit offsets, until it is exported as a narrower tape. */
template <typename allocator_type_>
class growing_strings {
    using traits_t = std::allocator_traits<allocator_type_>;
    st::basic_vector<ustore_length_t, typename traits_t::template rebind_alloc<ustore_length_t>> offsets_;
    st::basic_vector<byte_t, typename traits_t::template rebind_alloc<byte_t>> bytes_;

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

    /** Appends a copy of @p string followed by a null byte when @p terminated, which the string then spans. */
    status_t push_back(ustore_bytes_view_t string, bool terminated) noexcept {
        if (status_t const status = push_back(string); status.failed() || !terminated) return status;
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

} // namespace unum::ustore::utilities
