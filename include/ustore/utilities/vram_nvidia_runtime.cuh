/**
 *  @file include/ustore/utilities/vram_nvidia_runtime.cuh
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The CUDA runtime helpers the graph's device fold still calls: the status of a runtime result, the device
 *      scope, and the buffers it grows.
 *
 *  Host code over the CUDA runtime API, which only the compaction units see, since the library links the runtime
 *  statically into them alone and every other unit drives devices through @c vram_nvidia_device.hpp.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <limits>      // `std::numeric_limits`
#include <type_traits> // `std::true_type`
#include <utility>     // `std::exchange`, `std::move`, `std::swap`

#include <cuda_runtime.h>

#include <smashtable/shared.hpp> // `st::status_t`, `st::expected`

namespace unum::ustore::utilities {

#pragma region Devices

/** The status a CUDA runtime result maps to, keeping exhausted memory and absent devices apart from other failures. */
inline st::status_t cuda_status(cudaError_t error) noexcept {
    using st::status_t;
    switch (error) {
    case cudaSuccess: return status_t::success_k;
    case cudaErrorMemoryAllocation: return status_t::out_of_memory_heap_k;
    case cudaErrorInvalidValue:
    case cudaErrorInvalidDevice:
    case cudaErrorNoDevice: return status_t::invalid_argument_k;
    default: return status_t::unknown_k;
    }
}

/** Makes one device current for its lifetime, and the device current before it afterwards. */
class cuda_device_scope_t {
  public:

    /** Makes device @p ordinal current, remembering the device current before. */
    static st::expected<cuda_device_scope_t> make(int ordinal) noexcept {
        using st::status_t;
        int previous = 0;
        status_t status = cuda_status(cudaGetDevice(&previous));
        if (status != status_t::success_k) return status;
        if (previous != ordinal) {
            status = cuda_status(cudaSetDevice(ordinal));
            if (status != status_t::success_k) return status;
        }
        return cuda_device_scope_t(previous, ordinal);
    }

    cuda_device_scope_t(cuda_device_scope_t&& other) noexcept
        : previous_(std::exchange(other.previous_, other.current_)), current_(other.current_) {}
    cuda_device_scope_t(cuda_device_scope_t const&) = delete;
    cuda_device_scope_t& operator=(cuda_device_scope_t&&) = delete;
    cuda_device_scope_t& operator=(cuda_device_scope_t const&) = delete;

    ~cuda_device_scope_t() noexcept {
        if (previous_ != current_) { [[maybe_unused]] cudaError_t const error = cudaSetDevice(previous_); }
    }

  private:
    cuda_device_scope_t(int previous, int current) noexcept : previous_(previous), current_(current) {}

    /** The device to make current again, equal to @c current_ when there is nothing to restore. */
    int previous_ = 0;

    /** The device the scope made current. */
    int current_ = 0;
};

#pragma endregion Devices

#pragma region Memory

/** Page-locked host memory devices copy to and from directly, as a container allocator that answers null on failure. */
template <typename value_type_>
class cuda_pinned_allocator {
  public:
    using value_type = value_type_;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;

    constexpr cuda_pinned_allocator() noexcept = default;

    /** The rebinding conversion @c std::allocator_traits performs. */
    template <typename other_type_>
    constexpr cuda_pinned_allocator(cuda_pinned_allocator<other_type_> const&) noexcept {}

    /** Pins room for @p count values, or answers null. */
    value_type_* allocate(std::size_t count) noexcept {
        if ((count == 0) | (count > std::numeric_limits<std::size_t>::max() / sizeof(value_type_))) return nullptr;
        void* pointer = nullptr;
        if (cudaHostAlloc(&pointer, count * sizeof(value_type_), cudaHostAllocPortable) != cudaSuccess) return nullptr;
        return static_cast<value_type_*>(pointer);
    }

    /** Unpins and releases @p pointer, ignoring null. */
    void deallocate(value_type_* pointer, std::size_t) noexcept {
        if (pointer == nullptr) return;
        [[maybe_unused]] cudaError_t const error = cudaFreeHost(pointer);
    }

    template <typename other_type_>
    constexpr bool operator==(cuda_pinned_allocator<other_type_> const&) const noexcept {
        return true;
    }
};

/** Memory on one device for buffers kernels read, as an allocator that answers null on failure and never touches the
 *  bytes. */
template <typename value_type_>
class cuda_device_allocator {
  public:
    using value_type = value_type_;
    using propagate_on_container_move_assignment = std::true_type;

    /** Allocates on device @p ordinal. */
    explicit constexpr cuda_device_allocator(int ordinal) noexcept : device_(ordinal) {}

    /** The rebinding conversion @c std::allocator_traits performs; the device travels with it. */
    template <typename other_type_>
    constexpr cuda_device_allocator(cuda_device_allocator<other_type_> const& other) noexcept
        : device_(other.device()) {}

    /** Allocates room for @p count values on the device, or answers null. */
    value_type_* allocate(std::size_t count) noexcept {
        if ((count == 0) | (count > std::numeric_limits<std::size_t>::max() / sizeof(value_type_))) return nullptr;
        st::expected<cuda_device_scope_t> const scope = cuda_device_scope_t::make(device_);
        if (!scope) return nullptr;
        void* pointer = nullptr;
        if (cudaMalloc(&pointer, count * sizeof(value_type_)) != cudaSuccess) return nullptr;
        return static_cast<value_type_*>(pointer);
    }

    /** Releases @p pointer, ignoring null. */
    void deallocate(value_type_* pointer, std::size_t) noexcept {
        if (pointer == nullptr) return;
        st::expected<cuda_device_scope_t> const scope = cuda_device_scope_t::make(device_);
        [[maybe_unused]] cudaError_t const error = cudaFree(pointer);
    }

    /** The ordinal of the device memory lands on. */
    constexpr int device() const noexcept { return device_; }

    template <typename other_type_>
    constexpr bool operator==(cuda_device_allocator<other_type_> const& other) const noexcept {
        return device_ == other.device();
    }

  private:

    /** The ordinal of the device memory lands on. */
    int device_ = 0;
};

/** Elements in memory a CUDA allocator provides, released on destruction, whose contents a growth discards. */
template <typename element_type_, typename allocator_type_>
class cuda_buffer {
  public:

    /** An empty buffer that allocates through @p allocator. */
    explicit cuda_buffer(allocator_type_ allocator) noexcept : allocator_(std::move(allocator)) {}

    /** A buffer of @p size elements from @p allocator. */
    static st::expected<cuda_buffer> make(std::size_t size, allocator_type_ allocator) noexcept {
        using st::status_t;
        cuda_buffer buffer(std::move(allocator));
        status_t const status = buffer.reserve(size);
        if (status != status_t::success_k) return status;
        return buffer;
    }

    cuda_buffer(cuda_buffer&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
          allocator_(other.allocator_) {}
    cuda_buffer& operator=(cuda_buffer&& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
        std::swap(allocator_, other.allocator_);
        return *this;
    }
    cuda_buffer(cuda_buffer const&) = delete;
    cuda_buffer& operator=(cuda_buffer const&) = delete;

    ~cuda_buffer() noexcept { release_(); }

    /** Makes room for at least @p size elements, keeping an allocation that is large enough already. */
    st::status_t reserve(std::size_t size) noexcept {
        using st::status_t;
        if (size <= size_) return status_t::success_k;
        release_();
        data_ = allocator_.allocate(size);
        if (data_ == nullptr) return status_t::out_of_memory_heap_k;
        size_ = size;
        return status_t::success_k;
    }

    /** Copies @p count elements from @p source into the buffer at element @p offset, waiting until @p stream ran it. */
    st::status_t copy_in(element_type_ const* source, std::size_t count, std::size_t offset,
                         cudaStream_t stream) noexcept {
        using st::status_t;
        if (count == 0) return status_t::success_k;
        status_t const status = cuda_status(
            cudaMemcpyAsync(data_ + offset, source, count * sizeof(element_type_), cudaMemcpyDefault, stream));
        return status != status_t::success_k ? status : cuda_status(cudaStreamSynchronize(stream));
    }

    /** Copies @p count elements at element @p offset into @p target, waiting until @p stream ran it. */
    st::status_t copy_out(element_type_* target, std::size_t count, std::size_t offset,
                          cudaStream_t stream) const noexcept {
        using st::status_t;
        if (count == 0) return status_t::success_k;
        status_t const status = cuda_status(
            cudaMemcpyAsync(target, data_ + offset, count * sizeof(element_type_), cudaMemcpyDefault, stream));
        return status != status_t::success_k ? status : cuda_status(cudaStreamSynchronize(stream));
    }

    /** The first element, or null before the first reservation. */
    element_type_* data() const noexcept { return data_; }

    /** Elements the allocation holds. */
    std::size_t size() const noexcept { return size_; }

  private:
    void release_() noexcept {
        if (data_ != nullptr) allocator_.deallocate(data_, size_);
        data_ = nullptr;
        size_ = 0;
    }

    /** The allocation, or null before the first reservation. */
    element_type_* data_ = nullptr;

    /** Elements the allocation holds. */
    std::size_t size_ = 0;

    /** The allocator the buffer grows through. */
    allocator_type_ allocator_;
};

/** Device memory for kernels on one device. */
template <typename element_type_>
using cuda_device_buffer = cuda_buffer<element_type_, cuda_device_allocator<element_type_>>;

/** Page-locked host memory devices copy to and from directly. */
template <typename element_type_>
using cuda_pinned_buffer = cuda_buffer<element_type_, cuda_pinned_allocator<element_type_>>;

#pragma endregion Memory

} // namespace unum::ustore::utilities
