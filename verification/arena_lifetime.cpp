/**
 *  @file verification/arena_lifetime.cpp
 *  @author Ash Vardanian
 *  @date November 26, 2026
 *  @brief GenMC client for an arena's lifetime: the claim, export count and generation of
 *      @c ustore_arena_handle_t in @c ram_buffers.hpp, and the arena count of its executor, spelled
 *      step for step over @c std::atomic.
 *
 *  The header pulls in SmashTable and StringZilla, which the runner's include roots do not carry,
 *  so the client spells the words with the orders the engine writes: @c take_claims in @c lib.hpp
 *  takes the claim with a sequentially consistent exchange and re-reads @c exports, and
 *  @c ustore_arena_export_open in @c types.cpp counts, then re-reads the claim. Outputs and the
 *  memory the arena and the executor free are plain words, so an unordered access is a data race.
 *  The executor's free waits for its claim and then exchanges, where @c take_idle_claim exchanges
 *  first, since GenMC's spin-assume reads a failed exchange's loop as a wait that never ends.
 *
 *  The binding's copy under an export whose generation reads its stamp reads its own outputs. No
 *  call writes outputs an export reads, and no free deletes memory a call or a free still reads.
 *
 *  @verify pass sc,tso,rc11,imm arena_lifetime
 *  @verify pass sc,tso arena_lifetime_take_order_acq_rel: on x86 the exchange is a locked
 *      instruction, which drains the store buffer before the export count's load
 *  @verify fail rc11,imm arena_lifetime_take_order_acq_rel: the take and the export count's load
 *      reorder, so the call writes outputs the binding's export reads
 *  @verify fail sc,tso,rc11,imm arena_lifetime_copy_bracket_false: the copy checks the generation
 *      with no export open, so the deferred call rewinds the outputs under it
 *  @verify fail sc,tso,rc11,imm arena_lifetime_count_lowered_last_false: lowered first, the
 *      executor is deleted while the arena's free gives its memory back through it
 */
#include <cstdint> // `std::uint32_t`, `std::uint64_t`

#include <atomic> // `std::atomic`

#include <genmc.hpp>

/** The engine's choices; each mutant entry changes one. */
struct knobs_t {
    std::memory_order take_order = std::memory_order_seq_cst;
    bool copy_bracket = true;
    bool count_lowered_last = true;
};

constexpr knobs_t engine_k {};
constexpr knobs_t take_order_acq_rel_k {.take_order = std::memory_order_acq_rel};
constexpr knobs_t copy_bracket_false_k {.copy_bracket = false};
constexpr knobs_t count_lowered_last_false_k {.count_lowered_last = false};

/** The arena's words: @c claim, held while a call fills it, @c named_calls, @c exports and
 *  @c generation; then the executor's @c arenas count and its call claim. */
std::atomic<std::uint32_t> claim {0};
std::atomic<std::uint32_t> named_calls {0};
std::atomic<std::uint32_t> exports {0};
std::atomic<std::uint64_t> generation {0};
std::atomic<std::uint32_t> arenas {0};
std::atomic<std::uint32_t> holding {0};

/** The arena's outputs, tagged with the generation that wrote them, and a word of the executor's
 *  memory; each is deleted by overwriting it. */
std::uint64_t outputs = 0;
std::uint32_t executor_memory = 0;

/** Takes the arena as @c take_claims does: the exchange, then the export count. */
template <knobs_t const& knobs_>
bool take_arena() noexcept {
    if (claim.exchange(1, knobs_.take_order) != 0) return false;
    if (exports.load(std::memory_order_seq_cst) == 0) return true;
    claim.store(0, std::memory_order_release);
    return false;
}

/** Takes the executor's claim, letting the arena go when another call holds it. */
bool take_executor() noexcept {
    if (holding.exchange(1, std::memory_order_acquire) == 0) return true;
    claim.store(0, std::memory_order_release);
    return false;
}

/** Opens an export as @c ustore_arena_export_open does: the count raised, then the claim read. */
bool export_open() noexcept {
    exports.fetch_add(1, std::memory_order_seq_cst);
    if (claim.load(std::memory_order_seq_cst) == 0) return true;
    exports.fetch_sub(1, std::memory_order_release);
    return false;
}

/** Ends a call as @c end_call does: the arena let go, the call counted off, the executor read, and
 *  its claim given back last. */
void end_call() noexcept {
    claim.store(0, std::memory_order_release);
    named_calls.fetch_sub(1, std::memory_order_release);
    verify(executor_memory == 0);
    holding.store(0, std::memory_order_release);
}

/** The owner's deferred call, served on a pool thread once the owner dispatched it. */
void* serve(void*) noexcept {
    outputs = generation.load(std::memory_order_relaxed);
    end_call();
    return nullptr;
}

/** The binding: reads the generation, runs its call, and copies its stamped result. */
template <knobs_t const& knobs_>
void* bind(void*) noexcept {
    std::uint64_t const before = generation.load(std::memory_order_relaxed);
    if (!take_arena<knobs_>() || !take_executor()) return nullptr;
    named_calls.fetch_add(1, std::memory_order_relaxed);
    outputs = generation.fetch_add(1, std::memory_order_relaxed) + 1;
    end_call();
    std::uint64_t const stamp = before + 1;
    if constexpr (knobs_.copy_bracket) {
        if (!export_open()) return nullptr;
        if (generation.load(std::memory_order_relaxed) == stamp) verify(outputs == stamp);
        exports.fetch_sub(1, std::memory_order_release);
    }
    else if (generation.load(std::memory_order_relaxed) == stamp) verify(outputs == stamp);
    return nullptr;
}

/** Frees the executor once: refused while it counts an arena, else it waits for the claim and
 *  deletes its memory. */
void* free_executor(void*) noexcept {
    if (arenas.load(std::memory_order_acquire) != 0) return nullptr;
    for (;;) {
        while (holding.load(std::memory_order_acquire) != 0) {}
        if (holding.exchange(1, std::memory_order_seq_cst) == 0) break;
    }
    executor_memory = 1;
    return nullptr;
}

/** The owner: makes the arena, submits its deferred call, and frees the arena once the binding
 *  dropped its result, while the call may still run. */
template <knobs_t const& knobs_>
int arena_lifetime_() noexcept {
    arenas.fetch_add(1, std::memory_order_acq_rel);
    thread_t const binding = spawn(bind<knobs_>);
    thread_t const executor = spawn(free_executor);
    thread_t pool {};
    bool const served = take_arena<knobs_>() && take_executor();
    if (served) {
        named_calls.fetch_add(1, std::memory_order_relaxed);
        generation.fetch_add(1, std::memory_order_relaxed);
        pool = spawn(serve);
    }
    join(binding);
    while (named_calls.load(std::memory_order_acquire) != 0) {}
    if (exports.load(std::memory_order_acquire) == 0) {
        outputs = ~std::uint64_t {0};
        if constexpr (!knobs_.count_lowered_last) arenas.fetch_sub(1, std::memory_order_release);
        verify(executor_memory == 0);
        if constexpr (knobs_.count_lowered_last) arenas.fetch_sub(1, std::memory_order_release);
    }
    if (served) join(pool);
    join(executor);
    return 0;
}

extern "C" int arena_lifetime() { return arena_lifetime_<engine_k>(); }
extern "C" int arena_lifetime_take_order_acq_rel() { return arena_lifetime_<take_order_acq_rel_k>(); }
extern "C" int arena_lifetime_copy_bracket_false() { return arena_lifetime_<copy_bracket_false_k>(); }
extern "C" int arena_lifetime_count_lowered_last_false() { return arena_lifetime_<count_lowered_last_false_k>(); }
