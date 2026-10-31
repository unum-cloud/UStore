/**
 *  @file verification/arena_exports.cpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief GenMC client for the export pin of an arena in <tt>src/arrow.cpp</tt>, <tt>src/lib.hpp</tt> and
 *      <tt>src/types.cpp</tt>.
 *
 *  An Arrow export counts itself into @c exports and then reads the arena's claim, a call takes the claim and then
 *  reads @c exports, and @c ustore_arena_free and the last release share the free through the freed bit.
 *
 *  Each side writes its own word and then reads the other's, so with both sequentially consistent at least one sees
 *  the other and backs off, and a call never rewinds what a live export reads. A race may refuse both. Weaker, the
 *  two reads may each pass the other's write in a store buffer.
 *
 *  @verify pass sc,tso,rc11,imm exports_and_claim
 *  @verify fail rc11,imm exports_and_claim_order_acquire_release: store buffering lets both looks pass, and the call
 *      rewinds the outputs the export still reads
 *  @verify pass sc,tso exports_and_claim_order_acquire_release: on x86 both writes are locked instructions, which
 *      drain the store buffer
 *  @verify pass sc,tso,rc11,imm free_and_release
 */
#include <cstdint> // `std::uint32_t`

#include <atomic> // `std::atomic`

#include <genmc.hpp>

/** The pin's one choice; the mutant entry weakens it. */
struct knobs_t {
    std::memory_order order = std::memory_order_seq_cst;
};

constexpr knobs_t header_k {};
constexpr knobs_t order_acquire_release_k {.order = std::memory_order_acq_rel};

constexpr std::uint32_t freed_bit_k = std::uint32_t {1} << 31;

/** The words of an arena: its claim, its exports, and ghosts for what each side did. */
struct arena_t {
    std::atomic<std::uint32_t> claim {0};
    std::atomic<std::uint32_t> exports {0};
    std::atomic<std::uint32_t> rewound {0};   // ? Ghost: a call rewound the outputs, never while read
    std::atomic<std::uint32_t> destroyed {0}; // ? Ghost: how many times the arena was destroyed
};

/** The load half of each side, whose order follows its write's. */
constexpr std::memory_order load_order(std::memory_order order) noexcept {
    return order == std::memory_order_seq_cst ? std::memory_order_seq_cst : std::memory_order_acquire;
}

/** An exporter: pins, reads the claim, and unless a call holds the arena reads the outputs twice, unrewound between. */
template <knobs_t const& knobs_>
void* export_outputs(void* argument) noexcept {
    auto& arena = *static_cast<arena_t*>(argument);
    arena.exports.fetch_add(1, knobs_.order);
    if (arena.claim.load(load_order(knobs_.order)) == 0) {
        std::uint32_t const before = arena.rewound.load(std::memory_order_relaxed);
        verify(arena.rewound.load(std::memory_order_relaxed) == before);
    }
    arena.exports.fetch_sub(1, std::memory_order_acq_rel);
    return nullptr;
}

/** @c take_claims: takes the claim, reads the exports, and rewinds unless an export is live. */
template <knobs_t const& knobs_>
void* claim_and_rewind(void* argument) noexcept {
    auto& arena = *static_cast<arena_t*>(argument);
    if (arena.claim.exchange(1, knobs_.order) != 0) return nullptr;
    if (arena.exports.load(load_order(knobs_.order)) == 0) arena.rewound.store(1, std::memory_order_relaxed);
    arena.claim.store(0, std::memory_order_release);
    return nullptr;
}

template <knobs_t const& knobs_>
int exports_and_claim_() noexcept {
    arena_t arena;
    thread_t const exporter = spawn(export_outputs<knobs_>, &arena);
    thread_t const caller = spawn(claim_and_rewind<knobs_>, &arena);
    join(exporter);
    join(caller);
    return 0;
}

/** @c ustore_arena_free: sets the freed bit, and destroys at once when no export is live. */
void* free_arena(void* argument) noexcept {
    auto& arena = *static_cast<arena_t*>(argument);
    if (arena.exports.fetch_add(freed_bit_k, std::memory_order_acq_rel) == 0)
        arena.destroyed.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

/** The release of an exported array: unpins, and destroys when it was the last export of a freed arena. */
void* release_export(void* argument) noexcept {
    auto& arena = *static_cast<arena_t*>(argument);
    if (arena.exports.fetch_sub(1, std::memory_order_acq_rel) == (freed_bit_k | 1))
        arena.destroyed.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

int free_and_release_() noexcept {
    arena_t arena;
    arena.exports.store(2, std::memory_order_relaxed);
    thread_t const freer = spawn(free_arena, &arena);
    thread_t const first = spawn(release_export, &arena);
    thread_t const second = spawn(release_export, &arena);
    join(freer);
    join(first);
    join(second);
    verify(arena.destroyed.load(std::memory_order_relaxed) == 1);
    return 0;
}

extern "C" int exports_and_claim() { return exports_and_claim_<header_k>(); }
extern "C" int exports_and_claim_order_acquire_release() { return exports_and_claim_<order_acquire_release_k>(); }
extern "C" int free_and_release() { return free_and_release_(); }
