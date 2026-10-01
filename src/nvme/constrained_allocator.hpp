/**
 *  @file src/nvme/constrained_allocator.hpp
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief A named memory budget, and the allocator that charges every allocation against one.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <atomic>      // `std::memory_order`
#include <type_traits> // `std::true_type`

#include <forkunion/atomics.hpp> // `fu::preferred_atomic_ref`
#include <smashtable/shared.hpp> // `st::default_allocator`

#include "ustore/types.hpp" // the `fu` and `st` aliases

namespace unum::ustore::nvme {

/** A budget with no limit, which every allocation fits inside. */
inline constexpr std::size_t unlimited_resource_k = static_cast<std::size_t>(-1);

/**
 *  @brief The bytes one named part of the engine may hold, and the bytes it holds now.
 *
 *  A budget is acquired and released in bytes by however many threads share it, each acquisition
 *  bounded so the budget can never be overdrawn and an exhausted one refuses instead of waiting.
 *  Whoever owns the budget outlives every allocator charging it.
 */
class resource_constraint_t {
  public:

    /** A budget of @p capacity bytes, all of them available. */
    explicit resource_constraint_t(std::size_t capacity = unlimited_resource_k) noexcept
        : capacity_(capacity), available_(capacity) {}

    resource_constraint_t(resource_constraint_t const&) = delete;
    resource_constraint_t& operator=(resource_constraint_t const&) = delete;

    /** The bytes the budget allows in total. */
    std::size_t capacity() const noexcept { return capacity_; }

    /** The bytes the budget still allows. */
    std::size_t available() const noexcept {
        return fu::preferred_atomic_ref<std::size_t const> {available_}.load(std::memory_order_acquire);
    }

    /** The bytes the budget currently holds. */
    std::size_t used() const noexcept { return capacity_ - available(); }

    /** Whether @p bytes would still fit, which a later acquisition may no longer find true. */
    bool has_more(std::size_t bytes) const noexcept { return bytes <= available(); }

    /** Charges @p bytes to the budget, answering whether they fit. */
    bool acquire(std::size_t bytes) noexcept {
        fu::preferred_atomic_ref<std::size_t> const reference {available_};
        std::size_t const observed = reference.fetch_sub_if_at_least(bytes, 0, std::memory_order_acquire);
        return observed >= bytes;
    }

    /** Returns @p bytes to the budget, answering whether they had been charged to it. */
    bool release(std::size_t bytes) noexcept {
        fu::preferred_atomic_ref<std::size_t> const reference {available_};
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
 */
template <typename value_type_, typename base_allocator_type_ = st::default_allocator<value_type_>>
class constrained_allocator {
  public:
    using value_type = value_type_;
    using base_allocator_t = base_allocator_type_;

    /** A moved container keeps charging the budget its allocator named. */
    using propagate_on_container_move_assignment = std::true_type;

    /** An allocator naming no budget, which refuses every allocation until a named one replaces it. */
    constrained_allocator() noexcept : base_(), constraint_(nullptr) {}

    /** An allocator charging @p constraint, which must outlive it. */
    explicit constrained_allocator(resource_constraint_t& constraint, base_allocator_t base = {}) noexcept
        : base_(base), constraint_(&constraint) {}

    /** The rebinding conversion @c std::allocator_traits performs; the budget travels with it. */
    template <typename other_type_, typename other_base_type_>
    constrained_allocator(constrained_allocator<other_type_, other_base_type_> const& other) noexcept
        : base_(), constraint_(other.constraint_pointer()) {}

    /** Charges room for @p count values, or answers @c nullptr when the budget refuses them. */
    [[nodiscard]] value_type_* allocate(std::size_t count) noexcept {
        if (!constraint_ || !constraint_->acquire(count * sizeof(value_type_))) return nullptr;
        value_type_* const memory = base_.allocate(count);
        if (!memory) { [[maybe_unused]] bool const returned = constraint_->release(count * sizeof(value_type_)); }
        return memory;
    }

    /** Releases @p pointer and returns its bytes to the budget. */
    void deallocate(value_type_* pointer, std::size_t count) noexcept {
        base_.deallocate(pointer, count);
        if (constraint_) { [[maybe_unused]] bool const returned = constraint_->release(count * sizeof(value_type_)); }
    }

    /** The budget this allocator charges. */
    resource_constraint_t& constraint() const noexcept { return *constraint_; }

    /** The budget this allocator charges, or null while it names none. */
    resource_constraint_t* constraint_pointer() const noexcept { return constraint_; }

    template <typename other_type_, typename other_base_type_>
    bool operator==(constrained_allocator<other_type_, other_base_type_> const& other) const noexcept {
        return constraint_ == other.constraint_pointer();
    }

  private:

    /** The allocator the charged bytes actually come from. */
    base_allocator_t base_;

    /** The budget every allocation is charged to, owned by whoever named it. */
    resource_constraint_t* constraint_;
};

} // namespace unum::ustore::nvme
