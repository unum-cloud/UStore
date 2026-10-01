/**
 *  @file src/helpers/linked_memory.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Growing arenas behind @c ustore_arena_t, whose private blocks ForkUnion places on a memory domain.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uintptr_t`
#include <cstring> // `std::memcpy`

#if defined(_WIN32)
#include <windows.h> // `VirtualAlloc`, `VirtualFree`
#else
#include <sys/mman.h> // `mmap`, `munmap`
#endif

#include <algorithm>   // `std::max`
#include <new>         // placement `new`
#include <span>        // `std::span`
#include <type_traits> // `std::is_same_v`
#include <utility>     // `std::exchange`

#include <forkunion.h>

#include "ustore/ranges.hpp" // `bits_span_t`
#include "ustore/types.hpp"  // `round_up_to_multiple`, `divide_round_up`

namespace unum::ustore {

/** A chain of ever larger blocks that bump-allocates and frees as a whole,
 *  one pointer wide to fit @c ustore_arena_t. */
struct linked_memory_t {

    /** Bytes in the first block. */
    static constexpr std::size_t initial_size_k = 1024ul * 1024ul;

    /** How many times larger each block is than the one before it. */
    static constexpr std::size_t growth_factor_k = 2ul;

    /** Memory domain of private blocks until an executor supplies the caller's own. */
    static constexpr fu_memory_domain_id_t domain_id_k = 0;

    /** Where the pages of a block come from. */
    enum class kind_t { private_k, shared_k };

    /** Whether a new call may drop the blocks that earlier calls filled. */
    enum class release_t : bool { forbidden_k, allowed_k };

    /** The header at the start of every block. */
    struct arena_header_t {

        /** Next, larger block of the chain. */
        arena_header_t* next = nullptr;

        /** Bytes the block spans, its header included. */
        std::size_t capacity = 0;

        /** Bytes handed out so far, the header included. */
        std::size_t used = 0;

        /** Where the pages of the block come from. */
        kind_t kind = kind_t::private_k;

        /** Whether a lock may release the chain, kept on the first block only. */
        release_t release = release_t::forbidden_k;

        void* alloc_internally(std::size_t length, std::size_t alignment) noexcept {
            auto const arena_start = reinterpret_cast<std::uintptr_t>(this);
            std::uintptr_t const arena_end = arena_start + capacity;
            std::uintptr_t const potential_start = round_up_to_multiple<std::uintptr_t>(arena_start + used, alignment);
            std::uintptr_t const potential_end = potential_start + length;
            if (potential_end > arena_end) return nullptr;
            used = potential_end - arena_start;
            return reinterpret_cast<void*>(potential_start);
        }
    };

    /** First and smallest block of the chain. */
    arena_header_t* first = nullptr;

    static arena_header_t* alloc_arena(std::size_t length, kind_t kind) noexcept {
        void* begin = nullptr;
        switch (kind) {
        case kind_t::private_k:
            if (fu_allocate_on_domain_id(domain_id_k, length, &begin) != fu_success_k) return nullptr;
            break;
        case kind_t::shared_k:
#if defined(_WIN32)
            // Windows has no `fork` for shared anonymous pages to survive, so plain committed pages serve
            begin = ::VirtualAlloc(nullptr, length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
            begin = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
            if (begin == MAP_FAILED) return nullptr;
#endif
            break;
        }
        if (!begin) return nullptr;

        arena_header_t* const header = new (begin) arena_header_t {};
        header->capacity = length;
        header->used = sizeof(arena_header_t);
        header->kind = kind;
        return header;
    }

    static void release_arena(arena_header_t* arena) noexcept {
        switch (arena->kind) {
        case kind_t::private_k: fu_free_on_domain_id(domain_id_k, arena, arena->capacity); break;
#if defined(_WIN32)
        case kind_t::shared_k: ::VirtualFree(arena, 0, MEM_RELEASE); break;
#else
        case kind_t::shared_k: ::munmap(arena, arena->capacity); break;
#endif
        }
    }

    /** Starts a chain of @p kind unless one already exists, and reports whether a chain is ready. */
    bool start_if_null(kind_t kind) noexcept {
        if (first && first->kind == kind) return true;
        // A chain of the other kind is released rather than dropped, which used to leak every block in it.
        release_all();
        first = alloc_arena(initial_size_k, kind);
        if (!first) return false;
        first->release = release_t::allowed_k;
        return true;
    }

    release_t lock_release_calls() noexcept { return std::exchange(first->release, release_t::forbidden_k); }
    void unlock_release_calls() noexcept { first->release = release_t::allowed_k; }

    /** Grows the allocation that ends at @p pointer in place, when a block still has room right after it. */
    bool cheap_extend(void* pointer, std::size_t additional_size, std::size_t alignment) const noexcept {
        for (arena_header_t* current = first; current != nullptr; current = current->next) {
            bool const is_continuation = pointer ==
                                         static_cast<void*>(reinterpret_cast<char*>(current) + current->used);
            bool const can_fit = current->capacity - current->used >= additional_size;
            if (!is_continuation || !can_fit) continue;
            return current->alloc_internally(additional_size, alignment) != nullptr;
        }
        return false;
    }

    void* alloc(std::size_t length, std::size_t alignment) noexcept {
        if (!length || !first) return nullptr;

        arena_header_t* last = nullptr;
        for (arena_header_t* current = first; current != nullptr; current = current->next) {
            if (void* const result = current->alloc_internally(length, alignment)) return result;
            last = current;
        }

        std::size_t const new_capacity = std::max(last->capacity * growth_factor_k,
                                                  length + alignment + sizeof(arena_header_t));
        arena_header_t* const new_arena = alloc_arena(new_capacity, first->kind);
        if (!new_arena) return nullptr;

        last->next = new_arena;
        return new_arena->alloc_internally(length, alignment);
    }

    void release_all() noexcept {
        arena_header_t* current = first;
        while (current != nullptr) release_arena(std::exchange(current, current->next));
        first = nullptr;
    }

    /** Frees every block but the first, and empties the first one for reuse. */
    void release_partially() noexcept {
        if (!first) return;
        arena_header_t* current = first->next;
        while (current != nullptr) release_arena(std::exchange(current, current->next));
        first->next = nullptr;
        first->used = sizeof(arena_header_t);
    }
};

/** A range that absorbs writes into a scratch element when the caller asked for no output. */
template <typename range_>
class range_or_dummy {
  public:
    using range_t = range_;
    using value_type = typename range_::value_type;
    using reference = typename range_::reference;

  private:
    range_ elements_;
    mutable value_type dummy_ {};

  public:
    explicit range_or_dummy(range_ elements) noexcept : elements_(elements) {}

    reference operator[](std::size_t index) const noexcept {
        return present() ? reference(elements_[index]) : reference(dummy_);
    }
    std::size_t size() const noexcept { return elements_.size(); }
    explicit operator bool() const noexcept { return present(); }

  private:

    /** Whether the caller asked for the output, which a null range declines. */
    bool present() const noexcept {
        if constexpr (requires { elements_.data(); }) return elements_.data() != nullptr;
        else return static_cast<bool>(elements_);
    }
};

/** Holds an arena for one C ABI call, emptying it first unless an outer call already holds it. */
class linked_memory_lock_t {

    /** Whether this lock took the right to release the arena from an outer one. */
    enum class ownership_t : bool { borrowed_k, owned_k };

    linked_memory_t& memory_;
    ownership_t ownership_ {ownership_t::borrowed_k};

  public:

    /** Whether a lock keeps what earlier calls left in the arena. */
    enum class retention_t : bool { discard_k, keep_k };

    linked_memory_lock_t(linked_memory_t& memory, linked_memory_t::kind_t kind, retention_t retention) noexcept
        : memory_(memory) {
        if (!memory_.start_if_null(kind)) return;
        if (memory_.lock_release_calls() == linked_memory_t::release_t::forbidden_k) return;
        ownership_ = ownership_t::owned_k;
        if (retention == retention_t::discard_k) memory_.release_partially();
    }

    linked_memory_lock_t(linked_memory_lock_t const&) = delete;
    linked_memory_lock_t& operator=(linked_memory_lock_t const&) = delete;

    ~linked_memory_lock_t() noexcept {
        if (ownership_ == ownership_t::owned_k) memory_.unlock_release_calls();
    }

    operator ustore_arena_t*() const noexcept { return reinterpret_cast<ustore_arena_t*>(&memory_); }

    template <typename element_>
    std::span<element_> alloc(std::size_t count, ustore_error_t* error,
                              std::size_t alignment = sizeof(element_)) noexcept {
        if (!count) return {};
        void* const result = memory_.alloc(sizeof(element_) * count, alignment);
        if (!result) {
            *error = {ustore_error_out_of_memory_k, "Failed to allocate arena memory"};
            return {};
        }
        return {static_cast<element_*>(result), count};
    }

    template <typename element_>
    std::span<element_> grow( //
        std::span<element_> elements, std::size_t additional_count, ustore_error_t* error,
        std::size_t alignment = sizeof(element_)) noexcept {

        if (!additional_count) return elements;

        std::size_t const new_count = elements.size() + additional_count;
        if (memory_.cheap_extend(elements.data() + elements.size(), sizeof(element_) * additional_count, alignment))
            return {elements.data(), new_count};

        void* const result = memory_.alloc(sizeof(element_) * new_count, alignment);
        if (!result) {
            *error = {ustore_error_out_of_memory_k, "Failed to grow arena memory"};
            return {};
        }
        std::memcpy(result, elements.data(), elements.size_bytes());
        return {static_cast<element_*>(result), new_count};
    }

    /** Allocates @p count presence bits into @p output, or a scratch bit when @p output is null. */
    range_or_dummy<bits_span_t> alloc_or_dummy( //
        std::size_t count, ustore_error_t* error, ustore_octet_t** output,
        std::size_t alignment = sizeof(ustore_octet_t)) noexcept {

        std::size_t const slots = divide_round_up(count, bits_in_byte_k);
        if (!output) return range_or_dummy<bits_span_t> {bits_span_t {nullptr}};
        *output = alloc<ustore_octet_t>(slots, error, alignment).data();
        return range_or_dummy<bits_span_t> {bits_span_t {*output}};
    }

    /** Allocates @p count elements into @p output, or a scratch element when @p output is null. */
    template <typename element_>
    range_or_dummy<std::span<element_>> alloc_or_dummy( //
        std::size_t count, ustore_error_t* error, element_** output,
        std::size_t alignment = sizeof(element_)) noexcept {

        static_assert(!std::is_same_v<element_, ustore_octet_t>);
        if (!output) return range_or_dummy<std::span<element_>> {std::span<element_> {}};
        std::span<element_> const elements = alloc<element_>(count, error, alignment);
        *output = elements.data();
        return range_or_dummy<std::span<element_>> {elements};
    }
};

/** Locks the arena of a C ABI call, choosing shared pages and retention from @p options. */
inline linked_memory_lock_t linked_memory(ustore_arena_t* arena, ustore_options_t options,
                                          [[maybe_unused]] ustore_error_t* error) noexcept {

    static_assert(sizeof(ustore_arena_t) == sizeof(linked_memory_t));
    linked_memory_t& memory = *reinterpret_cast<linked_memory_t*>(arena);
    linked_memory_t::kind_t const kind = (options & ustore_option_read_shared_memory_k)
                                             ? linked_memory_t::kind_t::shared_k
                                             : linked_memory_t::kind_t::private_k;
    linked_memory_lock_t::retention_t const retention = (options & ustore_option_dont_discard_memory_k)
                                                            ? linked_memory_lock_t::retention_t::keep_k
                                                            : linked_memory_lock_t::retention_t::discard_k;
    return linked_memory_lock_t(memory, kind, retention);
}

inline void clear_linked_memory(ustore_arena_t& arena) noexcept {
    static_assert(sizeof(ustore_arena_t) == sizeof(linked_memory_t));
    reinterpret_cast<linked_memory_t&>(arena).release_all();
}

} // namespace unum::ustore
