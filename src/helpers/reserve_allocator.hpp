/**
 * @file helpers/reserve_allocator.hpp
 * @author Ashot Vardanian
 *
 * @brief Replacing `std::vector` with non-throwing alternatives.
 */
#pragma once
#include <cstdint>
#include <memory>

namespace unum::ukv {

/**
 * @brief Implements a "reserving" allocator for objects bigger than pointer size.
 * The deallocated or reserved entries form a linked list, which is used for new allocations.
 */
template <typename base_allocator_at>
class reserve_allocator_gt {
  public:
    using base_t = base_allocator_at;
    using element_t = typename base_t::value_type;
    using value_type = element_t;

    template <typename other_element_at>
    struct rebind {
        using base_other_t = typename base_t::template rebind<other_element_at>::other;
        using other = reserve_allocator_gt<base_other_t>;
    };

    base_t base;
    element_t* reserve_front = nullptr;
    std::size_t reserve_length = 0;
    std::size_t max_reserve_length = 64;

    reserve_allocator_gt() noexcept {}

    template <typename other_at>
    reserve_allocator_gt(reserve_allocator_gt<other_at> const& other) noexcept
        : base(other.base), reserve_front(nullptr) {}

    element_t* allocate(std::size_t n) { return n == 1 ? allocate() : base.allocate(n); }
    void deallocate(element_t* ptr, std::size_t n) noexcept {
        return n == 1 ? deallocate(ptr) : base.deallocate(ptr, n);
    }

    element_t* allocate() {
        auto ptr = reserve_front ? pop_reserved() : base.allocate(1);
        new (ptr) element_t();
        return ptr;
    }

    void deallocate(element_t* ptr) noexcept {
        if (ptr && reserve_length < max_reserve_length) {
            ptr->~element_t();
            add_to_reserve(ptr);
        }
        else
            base.deallocate(ptr, 1);
    }

    bool reserve(std::size_t n) noexcept {
        while (reserve_length < n)
            if (!reserve_more())
                return false;
        return true;
    }

    bool reserve_more(std::size_t n) noexcept {
        while (n--)
            if (!reserve_more())
                return false;
        return true;
    }

    bool reserve_more() noexcept {
        auto ptr = base.allocate(1);
        if (!ptr)
            return false;

        add_to_reserve(ptr);
        return true;
    }

  private:
    void add_to_reserve(element_t* ptr) noexcept {
        reserve_front ? std::memcpy(ptr, &reserve_front, sizeof(std::uintptr_t))
                      : std::memset(ptr, 0, sizeof(std::uintptr_t));
        reserve_front = ptr;
        reserve_length++;
        max_reserve_length = std::max(reserve_length, max_reserve_length);
    }

    element_t* pop_reserved() noexcept {
        element_t* ptr = reserve_front;
        std::memcpy(&reserve_front, reserve_front, sizeof(std::uintptr_t));
        reserve_length--;
        return ptr;
    }
};

template <typename base_at, typename other_at>
bool operator==(reserve_allocator_gt<base_at> const& a, reserve_allocator_gt<other_at> const& b) noexcept {
    return a.base == b.base;
}

template <typename base_at, typename other_at>
bool operator!=(reserve_allocator_gt<base_at> const& a, reserve_allocator_gt<other_at> const& b) noexcept {
    return a.base != b.base;
}

} // namespace unum::ukv
