/**
 *  @file test/vram/fake_amdhip64.cpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief A stand-in @c libamdhip64.so for machines without AMD hardware, which reports one gfx942 device.
 *
 *  It exports every entry @c hip_api_t::load resolves. Device and managed allocations are host memory it remembers, so
 *  pointer queries tell them apart; copies and fills run at once; streams and events are tokens, and every wait and
 *  memory entry is counted in @c fake_hip_counters, which a suite reads to count what one call costs. Module loads
 *  check the offload bundle's magic, and launches are refused, as no AMD kernel runs here.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uintptr_t`
#include <cstdlib> // `std::aligned_alloc`, `std::free`
#include <cstring> // `std::memcpy`, `std::memset`, `std::strncpy`

#include <vector> // `std::vector`

/** The @c hipError_t values the fake answers. */
enum fake_result_t : int {
    success_k = 0,
    invalid_value_k = 1,
    invalid_device_k = 101,
    invalid_image_k = 200,
    host_memory_not_registered_k = 713,
    not_supported_k = 801,
};

/** The @c hipMemoryType values pointer queries report. */
enum fake_memory_type_t : unsigned {
    host_memory_k = 1,
    device_memory_k = 2,
};

/** What one remembered range of memory is. */
enum class range_kind_t {
    device_k,
    managed_k,
    registered_k,
};

/** One remembered range of memory. */
struct range_t {
    std::uintptr_t begin;
    std::size_t length;
    range_kind_t kind;
};

/** Every range allocated or registered and not yet freed, as each call comes from one thread at a time. */
static std::vector<range_t> ranges;

static range_t const* range_of(void const* pointer) noexcept {
    std::uintptr_t const address = reinterpret_cast<std::uintptr_t>(pointer);
    for (range_t const& range : ranges)
        if (address >= range.begin && address < range.begin + range.length) return &range;
    return nullptr;
}

static int forget(void* pointer, range_kind_t kind) noexcept {
    for (std::size_t index = 0; index != ranges.size(); ++index)
        if (ranges[index].begin == reinterpret_cast<std::uintptr_t>(pointer) && ranges[index].kind == kind) {
            ranges.erase(ranges.begin() + static_cast<std::ptrdiff_t>(index));
            return success_k;
        }
    return kind == range_kind_t::registered_k ? host_memory_not_registered_k : invalid_value_k;
}

static int allocate(void** pointer, std::size_t bytes, range_kind_t kind) noexcept {
    std::size_t const rounded = (bytes + 255) / 256 * 256;
    *pointer = rounded ? std::aligned_alloc(256, rounded) : nullptr;
    if (!*pointer) return invalid_value_k;
    ranges.push_back({reinterpret_cast<std::uintptr_t>(*pointer), rounded, kind});
    return success_k;
}

static int release(void* pointer, range_kind_t kind) noexcept {
    if (!pointer) return success_k;
    int const forgotten = forget(pointer, kind);
    if (forgotten == success_k) std::free(pointer);
    return forgotten;
}

/** A token standing for a stream, an event or a module, never dereferenced. */
static int token;

extern "C" {

/** What every counted entry was called for since the library loaded. */
struct fake_hip_counters_t {
    std::size_t allocations;
    std::size_t frees;
    std::size_t managed_allocations;
    std::size_t fills;
    std::size_t uploads;
    std::size_t downloads;
    std::size_t registrations;
    std::size_t unregistrations;
    std::size_t waits;
    std::size_t pointer_queries;
};

__attribute__((visibility("default"))) fake_hip_counters_t fake_hip_counters;

/** Ranges allocated or registered and not yet freed, so a suite catches a leak. */
__attribute__((visibility("default"))) std::size_t fake_hip_live_ranges() { return ranges.size(); }

__attribute__((visibility("default"))) int hipGetDeviceCount(int* count) {
    *count = 1;
    return success_k;
}

__attribute__((visibility("default"))) int hipDeviceGet(int* device, int ordinal) {
    if (ordinal != 0) return invalid_device_k;
    *device = ordinal;
    return success_k;
}

__attribute__((visibility("default"))) int hipDeviceGetAttribute(int* value, int attribute, int device) {
    if (device != 0) return invalid_device_k;
    switch (attribute) {
    case 23: *value = 9; return success_k;   // compute capability major, gfx9
    case 61: *value = 4; return success_k;   // compute capability minor, gfx942
    case 63: *value = 304; return success_k; // compute units of an MI300X
    case 92: *value = 0; return success_k;   // host NUMA node
    default: return invalid_value_k;
    }
}

__attribute__((visibility("default"))) int hipDeviceGetName(char* name, int length, int device) {
    if (device != 0 || length <= 0) return invalid_device_k;
    std::strncpy(name, "AMD Instinct MI300X stand-in", static_cast<std::size_t>(length) - 1);
    name[length - 1] = '\0';
    return success_k;
}

__attribute__((visibility("default"))) int hipDeviceTotalMem(std::size_t* bytes, int device) {
    if (device != 0) return invalid_device_k;
    *bytes = std::size_t {192} << 30;
    return success_k;
}

__attribute__((visibility("default"))) int hipSetDevice(int device) {
    return device == 0 ? success_k : invalid_device_k;
}

__attribute__((visibility("default"))) int hipStreamCreateWithFlags(void** stream, unsigned) {
    *stream = &token;
    return success_k;
}

__attribute__((visibility("default"))) int hipStreamDestroy(void*) { return success_k; }

__attribute__((visibility("default"))) int hipStreamSynchronize(void*) {
    ++fake_hip_counters.waits;
    return success_k;
}

__attribute__((visibility("default"))) int hipEventCreateWithFlags(void** event, unsigned) {
    *event = &token;
    return success_k;
}

__attribute__((visibility("default"))) int hipEventRecord(void*, void*) { return success_k; }

__attribute__((visibility("default"))) int hipEventSynchronize(void*) {
    ++fake_hip_counters.waits;
    return success_k;
}

__attribute__((visibility("default"))) int hipEventDestroy(void*) { return success_k; }

__attribute__((visibility("default"))) int hipPointerGetAttribute(void* data, int attribute, void const* pointer) {
    ++fake_hip_counters.pointer_queries;
    range_t const* const range = range_of(pointer);
    if (!range) return invalid_value_k;
    switch (attribute) {
    case 2:
        *static_cast<unsigned*>(data) = range->kind == range_kind_t::registered_k ? host_memory_k : device_memory_k;
        return success_k;
    case 8: *static_cast<int*>(data) = range->kind == range_kind_t::managed_k; return success_k;
    case 9: *static_cast<int*>(data) = 0; return success_k;
    default: return invalid_value_k;
    }
}

__attribute__((visibility("default"))) int hipModuleLoadData(void** module, void const* image) {
    static char const magic[] = "__CLANG_OFFLOAD_BUNDLE__";
    if (!image || std::memcmp(image, magic, sizeof(magic) - 1) != 0) return invalid_image_k;
    *module = &token;
    return success_k;
}

__attribute__((visibility("default"))) int hipModuleUnload(void*) { return success_k; }

__attribute__((visibility("default"))) int hipModuleGetFunction(void** function, void*, char const* name) {
    if (!name || std::strncmp(name, "ustore_", 7) != 0) return invalid_value_k;
    *function = &token;
    return success_k;
}

__attribute__((visibility("default"))) int hipFuncGetAttribute(int* value, int, void*) {
    *value = 0;
    return success_k;
}

__attribute__((visibility("default"))) int hipFuncSetAttribute(void const*, int, int) { return not_supported_k; }

__attribute__((visibility("default"))) int hipModuleLaunchKernel(void*, unsigned, unsigned, unsigned, unsigned,
                                                                 unsigned, unsigned, unsigned, void*, void**, void**) {
    return not_supported_k;
}

__attribute__((visibility("default"))) int hipMallocAsync(void** pointer, std::size_t bytes, void*) {
    ++fake_hip_counters.allocations;
    return allocate(pointer, bytes, range_kind_t::device_k);
}

__attribute__((visibility("default"))) int hipFreeAsync(void* pointer, void*) {
    ++fake_hip_counters.frees;
    return release(pointer, range_kind_t::device_k);
}

__attribute__((visibility("default"))) int hipMallocManaged(void** pointer, std::size_t bytes, unsigned) {
    ++fake_hip_counters.managed_allocations;
    return allocate(pointer, bytes, range_kind_t::managed_k);
}

__attribute__((visibility("default"))) int hipFree(void* pointer) {
    ++fake_hip_counters.frees;
    range_t const* const range = range_of(pointer);
    return release(pointer, range ? range->kind : range_kind_t::device_k);
}

__attribute__((visibility("default"))) int hipMemsetD8Async(void* target, unsigned char value, std::size_t bytes,
                                                            void*) {
    ++fake_hip_counters.fills;
    std::memset(target, value, bytes);
    return success_k;
}

__attribute__((visibility("default"))) int hipMemcpyHtoDAsync(void* target, void const* source, std::size_t bytes,
                                                              void*) {
    ++fake_hip_counters.uploads;
    if (bytes && (!range_of(target) || range_of(target)->kind == range_kind_t::registered_k)) return invalid_value_k;
    std::memcpy(target, source, bytes);
    return success_k;
}

__attribute__((visibility("default"))) int hipMemcpyDtoHAsync(void* target, void* source, std::size_t bytes, void*) {
    ++fake_hip_counters.downloads;
    if (bytes && (!range_of(source) || range_of(source)->kind == range_kind_t::registered_k)) return invalid_value_k;
    std::memcpy(target, source, bytes);
    return success_k;
}

__attribute__((visibility("default"))) int hipMemcpyDtoDAsync(void* target, void* source, std::size_t bytes, void*) {
    range_t const* const into = range_of(target);
    range_t const* const from = range_of(source);
    if (bytes &&
        (!into || !from || into->kind == range_kind_t::registered_k || from->kind == range_kind_t::registered_k))
        return invalid_value_k;
    std::memmove(target, source, bytes);
    return success_k;
}

__attribute__((visibility("default"))) int hipHostRegister(void* pointer, std::size_t bytes, unsigned) {
    ++fake_hip_counters.registrations;
    if (range_of(pointer)) return invalid_value_k;
    ranges.push_back({reinterpret_cast<std::uintptr_t>(pointer), bytes, range_kind_t::registered_k});
    return success_k;
}

__attribute__((visibility("default"))) int hipHostUnregister(void* pointer) {
    ++fake_hip_counters.unregistrations;
    return forget(pointer, range_kind_t::registered_k);
}

} // extern "C"
