/**
 *  @file test/nvme/allocation_watch.cpp
 *  @author Ash Vardanian
 *  @date September 20, 2026
 *  @brief Replaces every global allocation function with one counting its request on the watch.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdlib> // `std::malloc`, `std::free`

#include <memory> // `std::align`
#include <new>    // `std::bad_alloc`, `std::align_val_t`, `std::nothrow_t`

#include "nvme/allocation_watch.hpp" // `allocation_watch_t`

namespace unum::ustore::test::nvme {

allocation_watch_t allocation_watch_g;

/**
 *  @brief Counts @p bytes against the watched sizes, as every replaced allocation function asks.
 *
 *  The block is aligned by hand inside a larger @c std::malloc one, whose address sits just before
 *  it, since not every standard library offers @c std::aligned_alloc.
 */
void* watched_allocate(std::size_t bytes, std::size_t alignment) noexcept {
    ++allocation_watch_g.calls;
    for (std::size_t index = 0; index != allocation_watch_g.count; ++index)
        if (allocation_watch_g.sizes[index] == bytes) ++allocation_watch_g.hits[index];
    void* const block = std::malloc(bytes + alignment + sizeof(void*));
    if (!block) return nullptr;
    void* aligned = static_cast<void**>(block) + 1;
    std::size_t space = bytes + alignment;
    std::align(alignment, bytes, aligned, space);
    void** const header = static_cast<void**>(aligned) - 1;
    *header = block;
    return aligned;
}

/** Releases what @c watched_allocate answered. */
void watched_free(void* pointer) noexcept {
    if (!pointer) return;
    void* const* const header = static_cast<void* const*>(pointer) - 1;
    std::free(*header);
}

} // namespace unum::ustore::test::nvme

using namespace unum::ustore::test::nvme;

void* operator new(std::size_t bytes) {
    void* const pointer = watched_allocate(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    if (pointer == nullptr) throw std::bad_alloc {};
    return pointer;
}
void* operator new[](std::size_t bytes) {
    void* const pointer = watched_allocate(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    if (pointer == nullptr) throw std::bad_alloc {};
    return pointer;
}
void* operator new(std::size_t bytes, std::nothrow_t const&) noexcept {
    return watched_allocate(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void* operator new[](std::size_t bytes, std::nothrow_t const&) noexcept {
    return watched_allocate(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    void* const pointer = watched_allocate(bytes, static_cast<std::size_t>(alignment));
    if (pointer == nullptr) throw std::bad_alloc {};
    return pointer;
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    void* const pointer = watched_allocate(bytes, static_cast<std::size_t>(alignment));
    if (pointer == nullptr) throw std::bad_alloc {};
    return pointer;
}
void* operator new(std::size_t bytes, std::align_val_t alignment, std::nothrow_t const&) noexcept {
    return watched_allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, std::nothrow_t const&) noexcept {
    return watched_allocate(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer) noexcept { watched_free(pointer); }
void operator delete[](void* pointer) noexcept { watched_free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { watched_free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { watched_free(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { watched_free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { watched_free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { watched_free(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { watched_free(pointer); }
void operator delete(void* pointer, std::nothrow_t const&) noexcept { watched_free(pointer); }
void operator delete[](void* pointer, std::nothrow_t const&) noexcept { watched_free(pointer); }
void operator delete(void* pointer, std::align_val_t, std::nothrow_t const&) noexcept { watched_free(pointer); }
void operator delete[](void* pointer, std::align_val_t, std::nothrow_t const&) noexcept { watched_free(pointer); }
