/**
 *  @file src/helpers/linked_memory.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Growing arenas behind @c ustore_arena_t, whose blocks ForkUnion places on a memory domain.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uintptr_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::max`
#include <new>       // placement `new`
#include <span>      // `std::span`
#include <utility>   // `std::exchange`

#include <forkunion.h>

#include "ustore/status.hpp" // `expected`, `status_t`
#include "ustore/types.hpp"  // `round_up_to_multiple`, `ustore_arena_t`

/** One block of an arena, which bump-allocates past this header, and which @c ustore_arena_t names when newest. */
struct ustore_arena_handle_t {

    /** Older and smaller block of the chain, or null for the first one. */
    ustore_arena_handle_t* previous;

    /** Bytes the block spans, its header included. */
    std::size_t capacity;

    /** Bytes handed out so far, the header included. */
    std::size_t used;
};

namespace unum::ustore {

/** Bytes in the first block of an arena. */
constexpr std::size_t arena_first_block_bytes_k = 1024ul * 1024ul;

/** How many times larger each block is than the one before it. */
constexpr std::size_t arena_growth_factor_k = 2ul;

/** Memory domain of the blocks until an executor supplies the caller's own. */
constexpr fu_memory_domain_id_t arena_domain_id_k = 0;

/** Bumps @p length bytes aligned to @p alignment off @p block, or answers null when they do not fit. */
inline void* arena_bump(ustore_arena_t block, std::size_t length, std::size_t alignment) noexcept {
    auto const begin = reinterpret_cast<std::uintptr_t>(block);
    std::uintptr_t const start = round_up_to_multiple<std::uintptr_t>(begin + block->used, alignment);
    if (start + length > begin + block->capacity) return nullptr;
    block->used = start + length - begin;
    return reinterpret_cast<void*>(start);
}

/** Allocates @p count uninitialized elements in @p arena, opening its first block lazily and a larger block once the
 *  newest one is full. */
template <typename element_type_>
expected<std::span<element_type_>> arena_allocate(ustore_arena_t* arena, std::size_t count,
                                                  std::size_t alignment = alignof(element_type_)) noexcept {
    if (!count) return std::span<element_type_> {};
    if (!arena) return status_t {status_code_t::invalid_argument_k, "The call needs an arena for its outputs"};

    std::size_t const length = sizeof(element_type_) * count;
    void* bytes = *arena ? arena_bump(*arena, length, alignment) : nullptr;
    if (!bytes) {
        std::size_t const grown = *arena ? (*arena)->capacity * arena_growth_factor_k : arena_first_block_bytes_k;
        std::size_t const capacity = std::max(grown, length + alignment + sizeof(ustore_arena_handle_t));
        void* memory = nullptr;
        if (fu_allocate_on_domain_id(arena_domain_id_k, capacity, &memory) != fu_success_k || !memory)
            return status_t {status_code_t::out_of_memory_k, "Failed to allocate arena memory"};
        *arena = new (memory) ustore_arena_handle_t {*arena, capacity, sizeof(ustore_arena_handle_t)};
        bytes = arena_bump(*arena, length, alignment);
    }
    return std::span<element_type_> {static_cast<element_type_*>(bytes), count};
}

/** Grows @p elements of @p arena by @p additional elements, in place when they end its newest block and it has room,
 *  and as a copy otherwise. */
template <typename element_type_>
expected<std::span<element_type_>> arena_grow(ustore_arena_t* arena, std::span<element_type_> elements,
                                              std::size_t additional,
                                              std::size_t alignment = alignof(element_type_)) noexcept {
    ustore_arena_t const block = arena ? *arena : nullptr;
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

/** Frees every block of @p arena but the first, and empties the first one for reuse. */
inline void arena_rewind(ustore_arena_t& arena) noexcept {
    while (arena->previous) {
        ustore_arena_t const freed = std::exchange(arena, arena->previous);
        fu_free_on_domain_id(arena_domain_id_k, freed, freed->capacity);
    }
    arena->used = sizeof(ustore_arena_handle_t);
}

/** Frees every block of @p arena, where null is safe. */
inline void arena_free(ustore_arena_t arena) noexcept {
    while (arena) {
        ustore_arena_t const freed = std::exchange(arena, arena->previous);
        fu_free_on_domain_id(arena_domain_id_k, freed, freed->capacity);
    }
}

} // namespace unum::ustore
