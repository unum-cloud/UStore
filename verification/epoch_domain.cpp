/**
 *  @file verification/epoch_domain.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief GenMC client for the reader epochs of @c epoch_domain in @c nvme_collection_view.hpp,
 *      spelled step for step over @c std::atomic.
 *
 *  The header pulls in SmashTable and StringZilla, so the client spells one reader's slot and the
 *  writer that replaces the view it may hold, with the orders and fences the header writes:
 *  @c announce_ in @c nvme_collection_view.hpp loads the epoch, stores it relaxed and fences,
 *  @c leave_ in @c nvme_collection_view.hpp releases zero, and @c wait_for_readers in
 *  @c nvme_collection_view.hpp moves the epoch with a releasing add, fences, and waits out every
 *  slot announcing an older epoch.
 *
 *  A reader that loaded a view must never meet it freed.
 *
 *  @verify pass sc,tso,rc11,imm epoch_domain
 *  @verify pass sc epoch_domain_announce_fence_false
 *  @verify fail tso,rc11,imm epoch_domain_announce_fence_false: without it the reader's relaxed
 *      announcement waits behind its load of the view, so the writer reads the slot empty and frees
 *      the view the reader holds
 *  @verify pass sc,tso epoch_domain_wait_fence_false: on x86 the releasing add is a locked
 *      instruction, which drains the store buffer like the fence it replaces
 *  @verify fail rc11,imm epoch_domain_wait_fence_false: without it the writer's view store and its
 *      acquire load of the slot reorder, so the reader loads the old view the writer scanned past
 */
#include <cstdint> // `std::uint64_t`

#include <atomic> // `std::atomic`, `std::atomic_thread_fence`

#include <genmc.hpp>

/** The header's two fences; each mutant entry drops one. */
struct knobs_t {
    bool announce_fence = true;
    bool wait_fence = true;
};

constexpr knobs_t header_k {};
constexpr knobs_t announce_fence_false_k {.announce_fence = false};
constexpr knobs_t wait_fence_false_k {.wait_fence = false};

std::atomic<std::uint64_t> epoch {1};
std::atomic<std::uint64_t> announced {0};
std::atomic<int> view {0};
std::atomic<int> freed[2] = {0, 0};

/** Enters as @c announce_ does, loads the view as @c published_view::load does, and leaves as
 *  @c leave_ does. */
template <knobs_t const& knobs_>
void* read(void*) noexcept {
    announced.store(epoch.load(std::memory_order_acquire), std::memory_order_relaxed);
    if constexpr (knobs_.announce_fence) std::atomic_thread_fence(std::memory_order_seq_cst);
    int const loaded = view.load(std::memory_order_acquire);
    verify(freed[loaded].load(std::memory_order_relaxed) == 0);
    announced.store(0, std::memory_order_release);
    return nullptr;
}

/** Replaces view zero with view one as @c published_view::replace does, waits as
 *  @c wait_for_readers does, and frees view zero. */
template <knobs_t const& knobs_>
void* write(void*) noexcept {
    view.store(1, std::memory_order_release);
    std::uint64_t const moved = epoch.fetch_add(1, std::memory_order_release) + 1;
    if constexpr (knobs_.wait_fence) std::atomic_thread_fence(std::memory_order_seq_cst);
    for (;;) {
        std::uint64_t const seen = announced.load(std::memory_order_acquire);
        if (seen == 0 || seen >= moved) break;
    }
    freed[0].store(1, std::memory_order_relaxed);
    return nullptr;
}

template <knobs_t const& knobs_>
int epoch_domain_() noexcept {
    thread_t const reader = spawn(read<knobs_>);
    thread_t const writer = spawn(write<knobs_>);
    join(reader);
    join(writer);
    return 0;
}

extern "C" int epoch_domain() { return epoch_domain_<header_k>(); }
extern "C" int epoch_domain_announce_fence_false() { return epoch_domain_<announce_fence_false_k>(); }
extern "C" int epoch_domain_wait_fence_false() { return epoch_domain_<wait_fence_false_k>(); }
