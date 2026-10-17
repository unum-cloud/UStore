/**
 *  @file include/ustore/utilities/vram_nvidia_device.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief CUDA devices from the host: specifications, memory, NUMA placement, streams, events and buffers.
 *
 *  Host code over the CUDA runtime API with no kernels, so it compiles under a host compiler whenever the
 *  toolkit's headers are on the include path, and to nothing without them. Nothing here throws or keeps
 *  global state: devices enumerate into a table the caller owns.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint8_t`
#include <cstdio>  // `std::fopen`, `std::fgets`, `std::fclose`
#include <cstdlib> // `std::strtol`

#include <limits>      // `std::numeric_limits`
#include <type_traits> // `std::true_type`
#include <utility>     // `std::exchange`, `std::move`

#if __has_include(<cuda_runtime.h>)
#include <cuda_runtime.h>

#include <smashtable/basic_vector.hpp>
#include <smashtable/shared.hpp>

#include "ustore/utilities/ram_device.hpp"
#endif

#if __has_include(<cuda_runtime.h>)

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

/** GPU generations by compute capability. */
enum class cuda_generation_t : std::uint8_t {

    /** A compute capability this table does not name. */
    unknown_k,

    /** Compute capability 8.0 through 8.7. */
    ampere_k,

    /** Compute capability 8.9. */
    ada_k,

    /** Compute capability 9.0. */
    hopper_k,

    /** Compute capability 10.0 in data centers, 11.0 in Thor and 12.0 in workstations. */
    blackwell_k,
};

/** The generation of compute capability @p major and @p minor. */
constexpr cuda_generation_t cuda_generation(int major, int minor) noexcept {
    if (major == 8) return minor >= 9 ? cuda_generation_t::ada_k : cuda_generation_t::ampere_k;
    if (major == 9) return cuda_generation_t::hopper_k;
    if ((major >= 10) & (major <= 12)) return cuda_generation_t::blackwell_k;
    return cuda_generation_t::unknown_k;
}

/** The number of CUDA devices the runtime sees, which is zero on a machine without one. */
inline st::expected<std::size_t> cuda_count_devices() noexcept {
    using st::status_t;
    int count = 0;
    cudaError_t const error = cudaGetDeviceCount(&count);
    if (error == cudaErrorNoDevice) return std::size_t {0};
    status_t const status = cuda_status(error);
    if (status != status_t::success_k) return status;
    return static_cast<std::size_t>(count);
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

/** One CUDA device's hardware as the runtime reports it. */
struct cuda_device_specs_t {

    /** The ordinal every runtime call addresses the device by. */
    int ordinal = 0;

    /** The device's name, null-terminated. */
    char name[256] = {};

    /** Major compute capability. */
    int compute_major = 0;

    /** Minor compute capability. */
    int compute_minor = 0;

    /** Streaming multiprocessors, the cores kernels launch across. */
    int multiprocessor_count = 0;

    /** Memory on the device in bytes. */
    std::size_t total_bytes = 0;

    /** PCI domain of the device. */
    int pci_domain = 0;

    /** PCI bus of the device. */
    int pci_bus = 0;

    /** PCI device number of the device. */
    int pci_device = 0;

    /** The generation of the device's compute capability. */
    constexpr cuda_generation_t generation() const noexcept { return cuda_generation(compute_major, compute_minor); }

    /** Queries device @p ordinal. */
    static st::expected<cuda_device_specs_t> query(int ordinal) noexcept {
        using st::status_t;
        cudaDeviceProp properties {};
        status_t const status = cuda_status(cudaGetDeviceProperties(&properties, ordinal));
        if (status != status_t::success_k) return status;
        cuda_device_specs_t specs;
        specs.ordinal = ordinal;
        std::size_t const name_capacity = sizeof(specs.name) < sizeof(properties.name) ? sizeof(specs.name)
                                                                                       : sizeof(properties.name);
        for (std::size_t index = 0; index + 1 < name_capacity && properties.name[index] != '\0'; ++index)
            specs.name[index] = properties.name[index];
        specs.compute_major = properties.major;
        specs.compute_minor = properties.minor;
        specs.multiprocessor_count = properties.multiProcessorCount;
        specs.total_bytes = properties.totalGlobalMem;
        specs.pci_domain = properties.pciDomainID;
        specs.pci_bus = properties.pciBusID;
        specs.pci_device = properties.pciDeviceID;
        return specs;
    }
};

/** Free and total memory of one device at the moment of the query. */
struct cuda_memory_usage_t {

    /** Bytes no allocation holds. */
    std::size_t free_bytes = 0;

    /** Bytes on the device. */
    std::size_t total_bytes = 0;

    /** Queries device @p ordinal, which creates its context when none exists yet. */
    static st::expected<cuda_memory_usage_t> query(int ordinal) noexcept {
        using st::status_t;
        st::expected<cuda_device_scope_t> const scope = cuda_device_scope_t::make(ordinal);
        if (!scope) return scope.status();
        cuda_memory_usage_t usage;
        status_t const status = cuda_status(cudaMemGetInfo(&usage.free_bytes, &usage.total_bytes));
        if (status != status_t::success_k) return status;
        return usage;
    }
};

/** The host NUMA node nearest the device @p specs describe, from Linux sysfs, or @c no_memory_domain_k when unsaid. */
inline fu_memory_domain_id_t cuda_numa_node(cuda_device_specs_t const& specs) noexcept {
    char path[] = "/sys/bus/pci/devices/0000:00:00.0/numa_node";
    auto const write_hex = [&path](std::size_t end, unsigned value, std::size_t width) noexcept {
        for (std::size_t index = 0; index != width; ++index, value >>= 4) {
            unsigned const digit = value & 0xF;
            path[end - index - 1] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
        }
    };
    write_hex(25, static_cast<unsigned>(specs.pci_domain), 4);
    write_hex(28, static_cast<unsigned>(specs.pci_bus), 2);
    write_hex(31, static_cast<unsigned>(specs.pci_device), 2);

    std::FILE* const file = std::fopen(path, "r");
    if (file == nullptr) return no_memory_domain_k;
    char text[16] = {};
    char const* const read = std::fgets(text, sizeof(text), file);
    [[maybe_unused]] int const closed = std::fclose(file);
    if (read == nullptr) return no_memory_domain_k;
    long const node = std::strtol(text, nullptr, 10);
    return node < 0 ? no_memory_domain_k : static_cast<fu_memory_domain_id_t>(node);
}

/** One device with its hardware, its memory when enumerated, and its nearest NUMA node. */
struct cuda_device_t {

    /** The device's hardware. */
    cuda_device_specs_t specs;

    /** Free and total memory when the table was enumerated. */
    cuda_memory_usage_t memory;

    /** The host NUMA node nearest the device, or @c no_memory_domain_k. */
    fu_memory_domain_id_t numa_node = no_memory_domain_k;
};

/** Every CUDA device the runtime sees, enumerated once into a table its caller owns. */
class cuda_devices_t {
  public:

    /** Enumerates every device, which yields an empty table on a machine without one. */
    static st::expected<cuda_devices_t> make() noexcept {
        using st::status_t;
        auto [count, cuda_count_devices_status] = cuda_count_devices();
        if (st::failed(cuda_count_devices_status)) return cuda_count_devices_status;
        cuda_devices_t table;
        status_t const status = table.devices_.resize(count);
        if (status != status_t::success_k) return status;
        for (std::size_t index = 0; index != count; ++index) {
            int const ordinal = static_cast<int>(index);
            auto [specs, query_cuda_device_specs_status] = cuda_device_specs_t::query(ordinal);
            if (st::failed(query_cuda_device_specs_status)) return query_cuda_device_specs_status;
            auto [memory, query_cuda_memory_usage_status] = cuda_memory_usage_t::query(ordinal);
            if (st::failed(query_cuda_memory_usage_status)) return query_cuda_memory_usage_status;
            table.devices_[index] = cuda_device_t {specs, memory, cuda_numa_node(specs)};
        }
        return table;
    }

    /** Devices in the table. */
    std::size_t size() const noexcept { return devices_.size(); }

    /** The device of ordinal @p index. */
    cuda_device_t const& operator[](std::size_t index) const noexcept { return devices_[index]; }

  private:
    cuda_devices_t() noexcept = default;

    /** One entry per device, by ordinal. */
    st::basic_vector<cuda_device_t> devices_;
};

#pragma endregion Devices

#pragma region Streams

/** An owned stream on one device, drained and destroyed with its owner. */
class cuda_stream_t {
  public:

    /** Creates a stream on device @p ordinal. */
    static st::expected<cuda_stream_t> make(int ordinal) noexcept {
        using st::status_t;
        st::expected<cuda_device_scope_t> const scope = cuda_device_scope_t::make(ordinal);
        if (!scope) return scope.status();
        cudaStream_t handle = nullptr;
        status_t const status = cuda_status(cudaStreamCreate(&handle));
        if (status != status_t::success_k) return status;
        return cuda_stream_t(handle, ordinal);
    }

    cuda_stream_t(cuda_stream_t&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)), device_(other.device_) {}
    cuda_stream_t& operator=(cuda_stream_t&& other) noexcept {
        std::swap(handle_, other.handle_);
        std::swap(device_, other.device_);
        return *this;
    }
    cuda_stream_t(cuda_stream_t const&) = delete;
    cuda_stream_t& operator=(cuda_stream_t const&) = delete;

    ~cuda_stream_t() noexcept {
        if (handle_ == nullptr) return;
        st::expected<cuda_device_scope_t> const scope = cuda_device_scope_t::make(device_);
        [[maybe_unused]] cudaError_t const drained = cudaStreamSynchronize(handle_);
        [[maybe_unused]] cudaError_t const destroyed = cudaStreamDestroy(handle_);
    }

    /** Blocks until everything queued on the stream ran, reporting the first failure among it. */
    st::status_t synchronize() const noexcept { return cuda_status(cudaStreamSynchronize(handle_)); }

    /** The runtime handle kernels and transfers queue on. */
    cudaStream_t handle() const noexcept { return handle_; }

    /** The ordinal of the device the stream lives on. */
    int device() const noexcept { return device_; }

  private:
    cuda_stream_t(cudaStream_t handle, int device) noexcept : handle_(handle), device_(device) {}

    /** The owned stream, null once moved from. */
    cudaStream_t handle_ = nullptr;

    /** The ordinal of the device the stream lives on. */
    int device_ = 0;
};

/** Times the work queued on a stream between two marks, without blocking the host until the time is read. */
class cuda_stopwatch_t {
  public:

    /** A stopwatch with no events, which a @c make or a move fills. */
    cuda_stopwatch_t() noexcept = default;

    /** Creates the two events of a stopwatch on device @p ordinal. */
    static st::expected<cuda_stopwatch_t> make(int ordinal) noexcept {
        using st::status_t;
        st::expected<cuda_device_scope_t> const scope = cuda_device_scope_t::make(ordinal);
        if (!scope) return scope.status();
        cuda_stopwatch_t stopwatch;
        stopwatch.device_ = ordinal;
        status_t status = cuda_status(cudaEventCreate(&stopwatch.start_));
        if (status != status_t::success_k) return status;
        status = cuda_status(cudaEventCreate(&stopwatch.stop_));
        if (status != status_t::success_k) return status;
        return stopwatch;
    }

    cuda_stopwatch_t(cuda_stopwatch_t&& other) noexcept
        : start_(std::exchange(other.start_, nullptr)), stop_(std::exchange(other.stop_, nullptr)),
          device_(other.device_) {}
    cuda_stopwatch_t& operator=(cuda_stopwatch_t&& other) noexcept {
        std::swap(start_, other.start_);
        std::swap(stop_, other.stop_);
        std::swap(device_, other.device_);
        return *this;
    }
    cuda_stopwatch_t(cuda_stopwatch_t const&) = delete;
    cuda_stopwatch_t& operator=(cuda_stopwatch_t const&) = delete;

    ~cuda_stopwatch_t() noexcept {
        if ((start_ == nullptr) & (stop_ == nullptr)) return;
        st::expected<cuda_device_scope_t> const scope = cuda_device_scope_t::make(device_);
        if (start_ != nullptr) { [[maybe_unused]] cudaError_t const error = cudaEventDestroy(start_); }
        if (stop_ != nullptr) { [[maybe_unused]] cudaError_t const error = cudaEventDestroy(stop_); }
    }

    /** Marks the start where @p stream's queue stands now. */
    st::status_t start(cuda_stream_t const& stream) noexcept {
        return cuda_status(cudaEventRecord(start_, stream.handle()));
    }

    /** Marks the stop where @p stream's queue stands now. */
    st::status_t stop(cuda_stream_t const& stream) noexcept {
        return cuda_status(cudaEventRecord(stop_, stream.handle()));
    }

    /** Waits for the stop mark to pass, and answers the milliseconds between the marks. */
    st::expected<double> milliseconds() const noexcept {
        using st::status_t;
        status_t status = cuda_status(cudaEventSynchronize(stop_));
        if (status != status_t::success_k) return status;
        float elapsed = 0;
        status = cuda_status(cudaEventElapsedTime(&elapsed, start_, stop_));
        if (status != status_t::success_k) return status;
        return static_cast<double>(elapsed);
    }

  private:

    /** The event marking the start. */
    cudaEvent_t start_ = nullptr;

    /** The event marking the stop. */
    cudaEvent_t stop_ = nullptr;

    /** The ordinal of the device the events live on. */
    int device_ = 0;
};

/** Consecutive named phases of a GPU pipeline, timed on one borrowed stream, up to @c capacity_k of them. */
class cuda_phases_t {
  public:

    /** Most phases one tracer times. */
    static constexpr std::size_t capacity_k = 16;

    /** Creates the stopwatches of every phase on the device of @p stream, which must outlive the tracer. */
    static st::expected<cuda_phases_t> make(cuda_stream_t const& stream) noexcept {
        cuda_phases_t phases;
        phases.stream_ = &stream;
        for (std::size_t index = 0; index != capacity_k; ++index) {
            st::expected<cuda_stopwatch_t> stopwatch = cuda_stopwatch_t::make(stream.device());
            if (!stopwatch) return stopwatch.status();
            phases.stopwatches_[index] = std::move(*stopwatch);
        }
        return phases;
    }

    cuda_phases_t(cuda_phases_t&&) noexcept = default;
    cuda_phases_t& operator=(cuda_phases_t&&) noexcept = default;
    cuda_phases_t(cuda_phases_t const&) = delete;
    cuda_phases_t& operator=(cuda_phases_t const&) = delete;

    /** Ends the running phase, if any, and starts the phase @p name, a string that outlives the tracer. */
    st::status_t begin(char const* name) noexcept {
        using st::status_t;
        status_t const status = end();
        if (status != status_t::success_k) return status;
        if (size_ == capacity_k) return status_t::capacity_exhausted_k;
        names_[size_] = name;
        running_ = size_ + 1;
        return stopwatches_[size_++].start(*stream_);
    }

    /** Ends the running phase, doing nothing when none runs. */
    st::status_t end() noexcept {
        if (running_ == 0) return st::status_t::success_k;
        std::size_t const index = std::exchange(running_, 0) - 1;
        return stopwatches_[index].stop(*stream_);
    }

    /** Phases begun so far. */
    std::size_t size() const noexcept { return size_; }

    /** The name of phase @p index. */
    char const* name(std::size_t index) const noexcept { return names_[index]; }

    /** Waits for phase @p index to end on the device, and answers its milliseconds. */
    st::expected<double> milliseconds(std::size_t index) const noexcept { return stopwatches_[index].milliseconds(); }

  private:
    cuda_phases_t() noexcept = default;

    /** The stream every phase is marked on. */
    cuda_stream_t const* stream_ = nullptr;

    /** One stopwatch per phase. */
    cuda_stopwatch_t stopwatches_[capacity_k];

    /** One name per phase begun. */
    char const* names_[capacity_k] = {};

    /** Phases begun. */
    std::size_t size_ = 0;

    /** One past the index of the running phase, zero when none runs. */
    std::size_t running_ = 0;
};

#pragma endregion Streams

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

#endif
