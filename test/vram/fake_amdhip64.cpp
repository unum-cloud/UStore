/**
 *  @file test/vram/fake_amdhip64.cpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief A stand-in @c libamdhip64.so for machines without AMD hardware, reporting two gfx942
 *      devices of its own.
 *
 *  It exports every entry @c hip_api_t::load resolves. Device and managed allocations are host
 *  memory it remembers, with the device current at the allocation, so pointer queries tell them
 *  apart; copies and fills run at once; each stream is a record of the device current at its
 *  creation, destroyed once, events are tokens, and every wait, stream and memory entry is counted
 *  in @c fake_hip_counters, which a suite reads to count what one call costs. Stream callbacks run
 *  when queued, or wait for the next synchronize, query or drain while a suite holds them through
 *  @c fake_hip_hold_callbacks. Module loads check the offload bundle's magic, and launches are
 *  refused, as no AMD kernel runs here.
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

/** One remembered range of memory, and the device current when it was allocated. */
struct range_t {
    std::uintptr_t begin;
    std::size_t length;
    range_kind_t kind;
    int device;
};

/** Devices the stand-in reports. */
static constexpr int devices_count_k = 2;

/** The device @c hipSetDevice made current, as each call comes from one thread at a time. */
static int& current_device() noexcept {
    static int device = 0;
    return device;
}

/** One stream created and not yet destroyed: its own address is its handle, and it remembers the
 *  device current at its creation. */
struct fake_stream_t {
    int device;
};

/** Every stream created and not yet destroyed, so a lookup and a destroy tell live handles
 *  apart, as each call comes from one thread at a time. */
static std::vector<fake_stream_t*>& live_streams() noexcept {
    static std::vector<fake_stream_t*> streams;
    return streams;
}

/** The live stream @p handle names, or the end for a default, destroyed or foreign one. */
static std::vector<fake_stream_t*>::iterator live_stream(void* handle) noexcept {
    std::vector<fake_stream_t*>& streams = live_streams();
    for (auto stream = streams.begin(); stream != streams.end(); ++stream)
        if (*stream == handle) return stream;
    return streams.end();
}

/** A stream callback held until its stream is next synchronized, queried or destroyed. */
struct held_callback_t {
    void (*callback)(void*, int, void*);
    void* stream;
    void* state;
};

/** Callbacks held in queue order, while a suite holds them. */
static std::vector<held_callback_t>& held_callbacks() noexcept {
    static std::vector<held_callback_t> held;
    return held;
}

/** How many suites asked to hold callbacks rather than run them when queued. */
static int& callbacks_held() noexcept {
    static int holders = 0;
    return holders;
}

/** Runs every held callback in queue order, as a drained stream would, reporting @p result. */
static void run_held_callbacks(int result = 0) noexcept {
    std::vector<held_callback_t> held;
    held.swap(held_callbacks());
    for (held_callback_t const& pending : held) pending.callback(pending.stream, result, pending.state);
}

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
    ranges.push_back({reinterpret_cast<std::uintptr_t>(*pointer), rounded, kind, current_device()});
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
    std::size_t stream_creations;
    std::size_t stream_destructions;
    std::size_t callbacks;
};

__attribute__((visibility("default"))) fake_hip_counters_t fake_hip_counters;

/** Makes queued stream callbacks wait for the next synchronize, query or drain while @p hold is
 *  positive, or run when queued again once it is zero, which runs every held one first. A waiter
 *  touching no stream meanwhile, as a blocking call sleeping on its outcome or a free of an arena
 *  or executor the held call names, hangs until the suite drains. */
__attribute__((visibility("default"))) void fake_hip_hold_callbacks(int hold) {
    callbacks_held() = hold;
    if (hold <= 0) run_held_callbacks();
}

/** Runs every held callback, as if each stream drained, each reporting @p result, which a suite
 *  sets nonzero to fail the queued work. */
__attribute__((visibility("default"))) void fake_hip_drain(int result) { run_held_callbacks(result); }

/** Ranges allocated or registered and not yet freed, so a suite catches a leak. */
__attribute__((visibility("default"))) std::size_t fake_hip_live_ranges() { return ranges.size(); }

/** Whether @p device is one the stand-in reports. */
static bool listed(int device) noexcept { return device >= 0 && device < devices_count_k; }

__attribute__((visibility("default"))) int hipGetDeviceCount(int* count) {
    *count = devices_count_k;
    return success_k;
}

__attribute__((visibility("default"))) int hipDeviceGet(int* device, int ordinal) {
    if (!listed(ordinal)) return invalid_device_k;
    *device = ordinal;
    return success_k;
}

__attribute__((visibility("default"))) int hipDeviceGetAttribute(int* value, int attribute, int device) {
    if (!listed(device)) return invalid_device_k;
    switch (attribute) {
    case 23: *value = 9; return success_k;     // compute capability major, gfx9
    case 61: *value = 4; return success_k;     // compute capability minor, gfx942
    case 63: *value = 304; return success_k;   // compute units of an MI300X
    case 75: *value = 65536; return success_k; // shared memory per block, opted in
    default: return invalid_value_k;
    }
}

__attribute__((visibility("default"))) int hipDeviceGetPCIBusId(char* bus_id, int length, int device) {
    if (!listed(device) || length <= 0) return invalid_device_k;
    // A bus no system lists, so the bounce falls back to the heap
    std::strncpy(bus_id, device ? "ffff:ff:01.0" : "ffff:ff:00.0", static_cast<std::size_t>(length) - 1);
    bus_id[length - 1] = '\0';
    return success_k;
}

__attribute__((visibility("default"))) int hipDeviceCanAccessPeer(int* can, int device, int peer) {
    if (!listed(device) || !listed(peer)) return invalid_device_k;
    *can = 0;
    return success_k;
}

__attribute__((visibility("default"))) int hipDeviceGetName(char* name, int length, int device) {
    if (!listed(device) || length <= 0) return invalid_device_k;
    std::strncpy(name, "AMD Instinct MI300X stand-in", static_cast<std::size_t>(length) - 1);
    name[length - 1] = '\0';
    return success_k;
}

__attribute__((visibility("default"))) int hipDeviceTotalMem(std::size_t* bytes, int device) {
    if (!listed(device)) return invalid_device_k;
    *bytes = std::size_t {192} << 30;
    return success_k;
}

__attribute__((visibility("default"))) int hipSetDevice(int device) {
    if (!listed(device)) return invalid_device_k;
    current_device() = device;
    return success_k;
}

__attribute__((visibility("default"))) int hipStreamCreateWithFlags(void** stream, unsigned) {
    fake_stream_t* const created = new fake_stream_t {current_device()};
    live_streams().push_back(created);
    ++fake_hip_counters.stream_creations;
    *stream = created;
    return success_k;
}

__attribute__((visibility("default"))) int hipStreamGetDevice(void* stream, int* device) {
    auto const found = live_stream(stream);
    if (found == live_streams().end()) return invalid_value_k;
    *device = (*found)->device;
    return success_k;
}

__attribute__((visibility("default"))) int hipStreamDestroy(void* stream) {
    run_held_callbacks();
    auto const found = live_stream(stream);
    if (found == live_streams().end()) return invalid_value_k;
    ++fake_hip_counters.stream_destructions;
    delete *found;
    live_streams().erase(found);
    return success_k;
}

__attribute__((visibility("default"))) int hipStreamSynchronize(void*) {
    ++fake_hip_counters.waits;
    run_held_callbacks();
    return success_k;
}

__attribute__((visibility("default"))) int hipStreamQuery(void*) {
    run_held_callbacks();
    return success_k;
}

__attribute__((visibility("default"))) int hipStreamWaitEvent(void*, void*, unsigned) { return success_k; }

/** Runs @p callback at once, which is queue order, as every queued call here ran when it was
 *  queued, or holds it while a suite asks, until a synchronize, query or drain runs it. */
__attribute__((visibility("default"))) int hipStreamAddCallback(void* stream, void (*callback)(void*, int, void*),
                                                                void* state, unsigned) {
    ++fake_hip_counters.callbacks;
    if (callbacks_held() > 0) held_callbacks().push_back({callback, stream, state});
    else callback(stream, success_k, state);
    return success_k;
}

__attribute__((visibility("default"))) int hipEventCreateWithFlags(void** event, unsigned) {
    *event = &token;
    return success_k;
}

__attribute__((visibility("default"))) int hipEventRecord(void*, void*) { return success_k; }

__attribute__((visibility("default"))) int hipEventSynchronize(void*) {
    ++fake_hip_counters.waits;
    run_held_callbacks();
    return success_k;
}

__attribute__((visibility("default"))) int hipEventQuery(void*) { return success_k; }

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
    case 9: *static_cast<int*>(data) = range->device; return success_k;
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

/** The stand-in runs no kernel, so a bundle it loads exports none and a device keeps no table. */
__attribute__((visibility("default"))) int hipModuleGetFunction(void**, void*, char const*) { return invalid_value_k; }

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

__attribute__((visibility("default"))) int hipMalloc(void** pointer, std::size_t bytes) {
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

/** Whether @p pointer is device or managed memory the stand-in remembers, which a copy reaches. */
static bool on_device(void const* pointer) noexcept {
    range_t const* const range = range_of(pointer);
    return range && range->kind != range_kind_t::registered_k;
}

__attribute__((visibility("default"))) int hipMemset(void* target, int value, std::size_t bytes) {
    ++fake_hip_counters.fills;
    std::memset(target, value, bytes);
    return success_k;
}

__attribute__((visibility("default"))) int hipMemsetAsync(void* target, int value, std::size_t bytes, void*) {
    return hipMemset(target, value, bytes);
}

__attribute__((visibility("default"))) int hipMemsetD32Async(void* target, int value, std::size_t count, void*) {
    ++fake_hip_counters.fills;
    for (std::size_t index = 0; index != count; ++index) std::memcpy(static_cast<int*>(target) + index, &value, 4);
    return success_k;
}

/** Copies @p bytes as unified addressing tells the sides apart, counting a host side as an upload
 *  or a download, and refusing a copy with neither side on a device. */
__attribute__((visibility("default"))) int hipMemcpy(void* target, void const* source, std::size_t bytes, int) {
    bool const into_device = on_device(target), from_device = on_device(source);
    if (bytes && !into_device && !from_device) return invalid_value_k;
    if (into_device && !from_device) ++fake_hip_counters.uploads;
    if (from_device && !into_device) ++fake_hip_counters.downloads;
    std::memmove(target, source, bytes);
    return success_k;
}

__attribute__((visibility("default"))) int hipMemcpyAsync(void* target, void const* source, std::size_t bytes, int kind,
                                                          void*) {
    return hipMemcpy(target, source, bytes, kind);
}

__attribute__((visibility("default"))) int hipMemcpy2DAsync(void*, std::size_t, void const*, std::size_t, std::size_t,
                                                            std::size_t, int, void*) {
    return not_supported_k;
}

__attribute__((visibility("default"))) int hipMemAdvise(void const*, std::size_t, int, int) { return success_k; }

__attribute__((visibility("default"))) int hipHostMalloc(void** pointer, std::size_t bytes, unsigned) {
    *pointer = std::aligned_alloc(256, (bytes + 255) / 256 * 256);
    return *pointer ? success_k : invalid_value_k;
}

__attribute__((visibility("default"))) int hipHostFree(void* pointer) {
    std::free(pointer);
    return success_k;
}

__attribute__((visibility("default"))) int hipHostRegister(void* pointer, std::size_t bytes, unsigned) {
    ++fake_hip_counters.registrations;
    if (range_of(pointer)) return invalid_value_k;
    ranges.push_back({reinterpret_cast<std::uintptr_t>(pointer), bytes, range_kind_t::registered_k, current_device()});
    return success_k;
}

__attribute__((visibility("default"))) int hipHostUnregister(void* pointer) {
    ++fake_hip_counters.unregistrations;
    return forget(pointer, range_kind_t::registered_k);
}

} // extern "C"
