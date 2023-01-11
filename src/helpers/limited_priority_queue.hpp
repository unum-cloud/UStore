/**
 * @file limited_priority_queue.hpp
 * @author Ashot Vardanian
 *
 * @brief Building size-constrained priority-queues over managed memory.
 */
#pragma once
#include <algorithm> // `std::destroy_n`

namespace unum::ukv {

/**
 * @brief Max-heap or Priority-Queue-like structure, keeping the biggest
 *        element in front, but having a fixed-size capacity. Can't reallocate,
 *        so beyond a certain size, either the `push()`-es will be failing,
 *        or the older entries will get evicted.
 */
template <typename element_at, typename comparator_at = std::less<void>>
class limited_priority_queue_gt {
    using element_t = element_at;
    using elementc_t = element_t const;
    using ptrc_t = elementc_t*;
    using ptr_t = element_t*;
    using comparator_t = comparator_at;

  private:
    ptr_t ptr_ = nullptr;
    std::size_t length_ = 0;
    std::size_t capacity_ = 0;

    static bool higher_priority(element_t const& a, element_t const& b) noexcept { return comparator_t {}(b, a); }

  public:
    limited_priority_queue_gt(limited_priority_queue_gt const&) = delete;
    limited_priority_queue_gt& operator=(limited_priority_queue_gt const&) = delete;

    limited_priority_queue_gt(limited_priority_queue_gt&& v) noexcept
        : ptr_(std::exchange(v.ptr_, nullptr)), length_(std::exchange(v.length_, 0)),
          capacity_(std::exchange(v.capacity_, 0)) {}

    limited_priority_queue_gt& operator=(limited_priority_queue_gt&& v) noexcept {
        std::swap(v.ptr_, ptr_);
        std::swap(v.length_, length_);
        std::swap(v.capacity_, capacity_);
        return *this;
    }

    limited_priority_queue_gt(ptr_t begin, ptr_t end, std::size_t length_populated = 0) noexcept
        : ptr_(begin), length_(length_populated), capacity_(end - begin) {}

    void clear() {
        std::destroy_n(ptr_, length_);
        length_ = 0;
    }

    ptr_t data() noexcept { return ptr_; }
    ptrc_t data() const noexcept { return ptr_; }
    ptrc_t begin() const noexcept { return ptr_; }
    ptrc_t end() const noexcept { return begin() + length_; }
    element_t& operator[](std::size_t i) noexcept { return ptr_[i]; }
    std::size_t size() const noexcept { return length_; }
    std::size_t capacity() const noexcept { return capacity_; }
    bool empty() const noexcept { return !length_; }

    /**
     * @brief
     *
     * @return true If insertion succeeded.
     * @return false If the priority of the input was to low to keep it.
     */
    bool push(element_t element) {

        // Find the optimal place within the container,
        // where this entry belongs.
        auto end = ptr_ + length_;
        auto element_ptr = std::lower_bound(ptr_, end, element, &higher_priority);
        if (element_ptr == end) {
            if (length_ < capacity_) {
                new (end) element_t(std::move(element));
                ++length_;
                return true;
            }
            else
                return false;
        }
        else {
            if (length_ == capacity_)
                std::destroy_at(ptr_ + capacity_ - 1);
            auto source_ptr = element_ptr;
            for (; source_ptr + 1 != end; ++source_ptr)
                source_ptr[1] = std::move(source_ptr[0]);
            *element_ptr = std::move(element);
            length_ += length_ < capacity_;
            return true;
        }
    }

    constexpr static std::size_t implicit_memory_usage(std::size_t capacity) noexcept {
        return sizeof(std::size_t) * 3 + sizeof(element_t) * capacity;
    }

    static limited_priority_queue_gt& implicit(void* begin) noexcept {
        auto& pq = reinterpret_cast<limited_priority_queue_gt*>(begin);
        pq.ptr_ = reinterpret_cast<char*>(begin) + sizeof(std::size_t) * 3;
        return pq;
    }

    static limited_priority_queue_gt& implicit_init(void* begin, std::size_t capacity) noexcept {
        auto& pq = implicit(begin);
        pq.capacity_ = capacity;
        pq.length_ = 0;
        return pq;
    }
};

} // namespace unum::ukv
