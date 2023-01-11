/**
 * @file pmr.hpp
 * @author Ashot Vardanian
 *
 * @brief Helper functions Polymorphic Memory Allocators.
 */
#pragma once
#include <sys/mman.h> // `mmap`
#include <limits.h>   // `CHAR_BIT`
#include <cstring>    // `std::memcpy`
#include <stdexcept>  // `std::runtime_error`
#include <memory>     // `std::allocator`
#include <vector>     // `std::vector`
#include <numeric>    // `std::accumulate`

#include "ukv/cpp/types.hpp"  // `byte_t`, `next_power_of_two`
#include "ukv/cpp/ranges.hpp" // `strided_range_gt`
#include "ukv/cpp/status.hpp" // `out_of_memory_k`

namespace unum::ukv {

struct linked_memory_t {
    static constexpr std::size_t initial_size_k = 1024ul * 1024ul;
    static constexpr std::size_t growth_factor_k = 2ul;

    struct arena_header_t;
    arena_header_t* first_ptr_ = nullptr;

    enum class kind_t { sys_k = 0, shared_k, unified_k };
    struct arena_header_t {
        arena_header_t* next = nullptr;
        std::size_t capacity = 0;
        std::size_t used = 0;
        kind_t kind = kind_t::sys_k;
        bool can_release_memory = false;

        void* alloc_internally(std::size_t length, std::size_t alignment) noexcept {
            auto arena_start = std::intptr_t(this);
            auto arena_end = arena_start + capacity;
            auto potential_start = next_multiple(arena_start + used, alignment);
            auto potential_end = potential_start + length;
            if (potential_end > arena_end)
                return nullptr;

            used = potential_end - arena_start;
            return (void*)potential_start;
        }
    };

    static arena_header_t* alloc_arena(std::size_t length, kind_t kind) noexcept {
        void* begin = nullptr;
        switch (kind) {
        case kind_t::sys_k: begin = std::malloc(length); break;
        case kind_t::shared_k:
            begin = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
            break;
        case kind_t::unified_k: break;
        }
        auto header_ptr = (arena_header_t*)begin;
        if (!header_ptr)
            return nullptr;

        std::memset(header_ptr, 0, sizeof(arena_header_t));
        header_ptr->kind = kind;
        header_ptr->capacity = length;
        header_ptr->used = sizeof(arena_header_t);
        return header_ptr;
    }

    static void release_arena(arena_header_t* arena) noexcept {
        switch (arena->kind) {
        case kind_t::sys_k: std::free(arena); break;
        case kind_t::shared_k: munmap(arena, arena->capacity); break;
        case kind_t::unified_k: break;
        }
    }

    arena_header_t& first_ref() noexcept { return *reinterpret_cast<arena_header_t*>(first_ptr_); }

    bool start_if_null(kind_t kind) noexcept {
        if (first_ptr_ && first_ptr_->kind == kind)
            return true;

        first_ptr_ = alloc_arena(initial_size_k, kind);
        first_ptr_->can_release_memory = true;
        return first_ptr_;
    }

    bool lock_release_calls() noexcept { return std::exchange(first_ref().can_release_memory, false); }
    void unlock_release_calls() noexcept { first_ref().can_release_memory = true; }

    bool cheap_extend(void* ptr, size_t additional_size, size_t alignment) const noexcept {
        arena_header_t* current = first_ptr_;
        while (current != nullptr) {
            bool is_continuation = ptr == (void*)((uint8_t*)current + current->used);
            bool can_fit = (current->capacity - current->used) >= additional_size;
            if (is_continuation && can_fit) {
                current->alloc_internally(additional_size, alignment);
                return true;
            }
            current = current->next;
        }
        return false;
    }

    void* alloc(std::size_t length, std::size_t alignment) noexcept {

        if (!length)
            return nullptr;

        arena_header_t* current = first_ptr_;
        arena_header_t* last = nullptr;
        while (current != nullptr) {
            if (auto ptr = current->alloc_internally(length, alignment); ptr)
                return ptr;
            last = current;
            current = current->next;
        }

        // We need to append a new even bigger bucket.
        auto new_capacity = std::max(last->capacity * growth_factor_k, length + alignment + sizeof(arena_header_t));
        auto new_arena = alloc_arena(new_capacity, first_ref().kind);
        if (!new_arena)
            return nullptr;

        last->next = new_arena;
        return new_arena->alloc_internally(length, alignment);
    }

    void release_all() noexcept {
        arena_header_t* current = first_ptr_;
        while (current != nullptr)
            release_arena(std::exchange(current, current->next));
        first_ptr_ = nullptr;
    }

    void release_supplementary() noexcept {
        if (!first_ptr_)
            return;
        arena_header_t* current = first_ptr_->next;
        while (current != nullptr)
            release_arena(std::exchange(current, current->next));
        first_ptr_->next = nullptr;
    }
};

template <typename range_at>
struct range_or_dummy_gt {
    using range_t = range_at;
    using value_type = typename range_t::value_type;
    using reference = typename range_t::reference;

    range_t range_;
    value_type dummy_;

    reference operator[](std::size_t i) & noexcept { return range_ ? reference(range_[i]) : reference(dummy_); }
    reference operator[](std::size_t i) const& noexcept { return range_ ? reference(range_[i]) : reference(dummy_); }
    std::size_t size() const noexcept { return range_.size(); }
    explicit operator bool() const noexcept { return range_; }
};

struct linked_memory_lock_t {
    linked_memory_t& memory;
    bool owns_the_lock = false;

    operator ukv_arena_t*() const noexcept { return (ukv_arena_t*)&memory.first_ptr_; }

    linked_memory_lock_t(linked_memory_t& memory, linked_memory_t::kind_t kind, bool keep_old_data = false) noexcept
        : memory(memory) {
        if (memory.start_if_null(kind))
            if ((owns_the_lock = memory.lock_release_calls()) && !keep_old_data)
                memory.release_supplementary();
    }

    ~linked_memory_lock_t() noexcept {
        if (owns_the_lock)
            memory.unlock_release_calls();
    }

    template <typename at>
    ptr_range_gt<at> alloc(std::size_t size, ukv_error_t* c_error, std::size_t alignment = sizeof(at)) noexcept {
        if (!size)
            return {};
        void* result = memory.alloc(sizeof(at) * size, alignment);
        log_error_if_m(result, c_error, out_of_memory_k, "Couldn't allocate new sub-arena");
        return {reinterpret_cast<at*>(result), size};
    }

    template <typename at>
    ptr_range_gt<at> grow( //
        ptr_range_gt<at> span,
        std::size_t additional_size,
        ukv_error_t* c_error,
        std::size_t alignment = sizeof(at)) noexcept {

        if (!additional_size)
            return span;

        size_t new_size = span.size() + additional_size;
        if (memory.cheap_extend(span.end(), sizeof(at) * additional_size, alignment))
            return {span.begin(), new_size};

        void* result = memory.alloc(sizeof(at) * new_size, alignment);
        if (result)
            std::memcpy(result, span.begin(), span.size_bytes());
        else
            log_error_m(c_error, out_of_memory_k, "Couldn't grow a memory buffer");
        return {reinterpret_cast<at*>(result), new_size};
    }

    range_or_dummy_gt<bits_span_t> alloc_or_dummy( //
        std::size_t size,
        ukv_error_t* c_error,
        ukv_octet_t** output,
        std::size_t alignment = sizeof(ukv_octet_t)) noexcept {

        using range_t = bits_span_t;
        auto slots = divide_round_up(size, bits_in_byte_k);
        auto range = output //
                         ? range_t {(*output = alloc<ukv_octet_t>(slots, c_error, alignment).begin())}
                         : range_t {nullptr};
        return {range, {}};
    }

    template <typename at>
    range_or_dummy_gt<ptr_range_gt<at>> alloc_or_dummy( //
        std::size_t size,
        ukv_error_t* c_error,
        at** output,
        std::size_t alignment = sizeof(at)) noexcept {

        static_assert(!std::is_same<at, ukv_octet_t>());
        using range_t = ptr_range_gt<at>;
        auto range = output //
                         ? range_t {(*output = alloc<at>(size, c_error, alignment).begin()), size}
                         : range_t {nullptr, nullptr};
        return {range, {}};
    }
};

inline linked_memory_lock_t linked_memory(ukv_arena_t* c_arena, ukv_options_t options, ukv_error_t* c_error) noexcept {

    static_assert(sizeof(ukv_arena_t) == sizeof(linked_memory_t));
    linked_memory_t& ref = *reinterpret_cast<linked_memory_t*>(c_arena);
    linked_memory_t::kind_t kind = (options & ukv_option_read_shared_memory_k) //
                                       ? linked_memory_t::kind_t::shared_k
                                       : linked_memory_t::kind_t::sys_k;
    bool keep_old_data = options & ukv_option_dont_discard_memory_k;

    return linked_memory_lock_t(ref, kind, keep_old_data);
}

inline void clear_linked_memory(ukv_arena_t& c_arena) noexcept {
    static_assert(sizeof(ukv_arena_t) == sizeof(linked_memory_t));
    linked_memory_t& ref = reinterpret_cast<linked_memory_t&>(c_arena);
    ref.release_all();
}

template <typename dangerous_at>
void safe_section(ukv_str_view_t name, ukv_error_t* c_error, dangerous_at&& dangerous) noexcept {
    try {
        dangerous();
    }
    catch (std::bad_alloc const&) {
        log_error_m(c_error, out_of_memory_k, name);
    }
    catch (...) {
        log_error_m(c_error, error_unknown_k, name);
    }
}

} // namespace unum::ukv
