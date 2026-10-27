/**
 *  @file verification/reader_gates.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief GenMC client for the two-slot reader gate that @c lsm_tree and the database catalog share: a
 *      reader counts itself into the live slot and re-checks it, a publisher makes the other slot live
 *      and waits for the retired slot's readers before it frees what that slot names.
 *
 *  RC11 lets a release store pass a later acquire load to another word, so without a sequentially
 *  consistent fence on both sides the publisher can read a zero count while the reader, already
 *  counted, still re-reads the old slot as live.
 *
 *  A reader counted into a slot must never meet it freed.
 *
 *  @verify pass sc,rc11 reader_gates
 *  @verify fail rc11 reader_gates_fences_false: without the fences the client is that race
 */
#include <atomic> // `std::atomic_ref`, `std::atomic_thread_fence`

#include <genmc.hpp>

/** The gate's one choice; the mutant entry drops it. */
struct knobs_t {
    bool fences = true;
};

constexpr knobs_t header_k {};
constexpr knobs_t fences_false_k {.fences = false};

int live = 0;
int readers[2] = {0, 0};
int freed[2] = {0, 0};

/** Counts itself into the live slot, re-checking it as the gate does, and checks the slot is still whole. */
template <knobs_t const& knobs_>
void* read(void*) noexcept {
    for (;;) {
        int const slot = std::atomic_ref<int>(live).load(std::memory_order_acquire);
        std::atomic_ref<int>(readers[slot]).fetch_add(1, std::memory_order_acquire);
        if constexpr (knobs_.fences) std::atomic_thread_fence(std::memory_order_seq_cst);
        if (std::atomic_ref<int>(live).load(std::memory_order_acquire) == slot) {
            verify(std::atomic_ref<int>(freed[slot]).load(std::memory_order_relaxed) == 0);
            std::atomic_ref<int>(readers[slot]).fetch_sub(1, std::memory_order_release);
            return nullptr;
        }
        std::atomic_ref<int>(readers[slot]).fetch_sub(1, std::memory_order_release);
    }
}

/** Makes slot one live, waits out slot zero's readers, then frees slot zero. */
template <knobs_t const& knobs_>
void* publish(void*) noexcept {
    std::atomic_ref<int>(live).store(1, std::memory_order_release);
    if constexpr (knobs_.fences) std::atomic_thread_fence(std::memory_order_seq_cst);
    while (std::atomic_ref<int>(readers[0]).load(std::memory_order_acquire) != 0) {}
    std::atomic_ref<int>(freed[0]).store(1, std::memory_order_relaxed);
    return nullptr;
}

template <knobs_t const& knobs_>
int reader_gates_() noexcept {
    thread_t const reader = spawn(read<knobs_>);
    thread_t const publisher = spawn(publish<knobs_>);
    join(reader);
    join(publisher);
    return 0;
}

extern "C" int reader_gates() { return reader_gates_<header_k>(); }
extern "C" int reader_gates_fences_false() { return reader_gates_<fences_false_k>(); }
