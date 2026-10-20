/**
 *  @file verification/epoch_claim/client.cpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief GenMC client for the epoch claim of @c parking_claim_t in
 *      `include/ustore/blobs/nvme_database.hpp`, spelled line for line over @c std::atomic.
 *
 *  ForkUnion's references spell the same calls as single instructions, which GenMC cannot load, so
 *  the claim's word is a @c std::atomic here: @c fetch_or for both takes, @c fetch_add for the
 *  release, and the fences as the header writes them. GenMC has no futex, so every park is the spin
 *  @c std::atomic_ref::wait performs, which GenMC reads as one blocked read, and a park that never
 *  ends is a liveness violation. The three scenarios are the Spin scenarios beside this file.
 *
 *  @c committer_and_holder publishes a commit's view, then calls @c take_if_free, against the
 *  holder's @c release_claim_, so no maintenance is lost. @c explicit_verbs runs two blocking takers
 *  and one trier, each holder adding one to a plain tally, so a broken exclusion or a stale tally is a
 *  data race GenMC reports. @c stalled_committer runs @c make_room_ against a flusher that releases
 *  and retakes the claim @c rounds times, then keeps it. The flusher flushes only once the committer
 *  has parked on the epoch the flusher holds, or is done, which a relaxed ghost word the committer
 *  writes before it parks reports without ordering anything.
 *
 *  @verify pass sc,tso,rc11,imm committer_and_holder
 *  @verify fail rc11,imm committer_and_holder_try_fence_false: @c take_if_free fences before its
 *      @c fetch_or; without it the view's store and the acquiring @c fetch_or reorder, under IMM as
 *      on Arm's @c ldseta, and the holder's look reads the view from before the commit
 *  @verify fail rc11,imm committer_and_holder_release_fence_false: @c release fences after its add;
 *      without it the releasing add and the look's acquire load reorder, under IMM as on Arm's
 *      @c staddl and @c ldar, and the committer's fenced @c fetch_or still reads the claim held
 *  @verify pass sc,tso committer_and_holder_try_fence_false: on x86 the @c fetch_or is a locked
 *      instruction, which drains the store buffer like the fence it replaces
 *  @verify pass sc,tso committer_and_holder_release_fence_false: on x86 the releasing add is a
 *      locked instruction as well
 *  @verify pass sc,tso,rc11,imm explicit_verbs
 *  @verify fail rc11,imm explicit_verbs_take_order_relaxed: the @c fetch_or that takes the claim
 *      acquires; relaxed, the next holder's tally races with the last holder's increment
 *  @verify fail rc11,imm explicit_verbs_release_order_relaxed: the add that releases the claim
 *      releases; relaxed, the next holder's tally races with the last holder's increment
 *  @verify pass sc,tso,rc11,imm stalled_committer
 *  @verify stuck sc,rc11,imm stalled_committer_release_step_cleared: a flag cleared to zero is set
 *      back to one by the flusher's retake, so the woken committer's wait reads the value it parked
 *      on and parks again, and once the flusher keeps the claim it parks for good
 *  @verify stuck sc,rc11,imm stalled_committer_stalled_wait_until_taken: blocked in @c take, the
 *      committer finds the claim retaken after every release and never reads the flush that
 *      relieved it, which is how any blocking taker fares against a flusher retaking back to back
 *  @verify pass sc,tso stalled_committer_rounds_1: the woken committer reads the flush at once,
 *      since the release it woke on follows it
 *  @verify stuck rc11,imm stalled_committer_rounds_1: the wake's reload is relaxed, so the woken
 *      committer may read the pressure stale and park on the retake, leaving only after one more
 *      release, which a single round never gives it
 *  @verify pass rc11,imm stalled_committer_rounds_1_wake_order_acquire: reloaded with an acquire,
 *      the wake synchronizes with the release it saw and the woken committer reads the flush
 */
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`

#include <atomic> // `std::atomic`, `std::atomic_thread_fence`

#include <genmc.hpp>

/** What @c release does to the odd word: steps it to the next even value, or clears a plain flag. */
enum class release_step_t { stepped, cleared };

/** How a stalled committer waits: for the holder's release then the pressure, or for the claim. */
enum class stalled_wait_t { until_released, until_taken };

/** The proposed header's choices and the flusher's rounds; each mutant entry changes one. */
struct knobs_t {
    bool try_fence = true;
    bool release_fence = true;
    std::memory_order take_order = std::memory_order_acquire;
    std::memory_order release_order = std::memory_order_release;
    std::memory_order wake_order = std::memory_order_relaxed;
    release_step_t release_step = release_step_t::stepped;
    stalled_wait_t stalled_wait = stalled_wait_t::until_released;
    std::size_t rounds = 2;
};

constexpr knobs_t header_k {};
constexpr knobs_t try_fence_false_k {.try_fence = false};
constexpr knobs_t release_fence_false_k {.release_fence = false};
constexpr knobs_t take_order_relaxed_k {.take_order = std::memory_order_relaxed};
constexpr knobs_t release_order_relaxed_k {.release_order = std::memory_order_relaxed};
constexpr knobs_t release_step_cleared_k {.release_step = release_step_t::cleared};
constexpr knobs_t stalled_wait_until_taken_k {.stalled_wait = stalled_wait_t::until_taken};
constexpr knobs_t rounds_1_k {.rounds = 1};
constexpr knobs_t rounds_1_wake_order_acquire_k {.wake_order = std::memory_order_acquire, .rounds = 1};

/** The claim, odd while held and even while free, stepped by its holder. */
template <knobs_t const& knobs_>
class parking_claim {
  public:
    explicit parking_claim(std::uint32_t epoch, std::atomic<std::uint32_t>* parked_on = nullptr) noexcept
        : epoch_(epoch), parked_on_(parked_on) {}

    [[nodiscard]] bool take_if_free() noexcept {
        if constexpr (knobs_.try_fence) std::atomic_thread_fence(std::memory_order_seq_cst);
        return epoch_.fetch_or(1, knobs_.take_order) % 2 == 0;
    }

    void take() noexcept {
        for (;;) {
            std::uint32_t const seen = epoch_.fetch_or(1, knobs_.take_order);
            if (seen % 2 == 0) return;
            wait_until_changed_(seen);
        }
    }

    void wait_until_released() noexcept {
        std::uint32_t const seen = epoch_.load(std::memory_order_acquire);
        if (seen % 2 != 0) wait_until_changed_(seen);
    }

    void release() noexcept {
        if constexpr (knobs_.release_step == release_step_t::stepped) epoch_.fetch_add(1, knobs_.release_order);
        else epoch_.fetch_sub(1, knobs_.release_order);
        if constexpr (knobs_.release_fence) std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    /** The epoch its holder took, read back for the flusher's gate. */
    [[nodiscard]] std::uint32_t held_epoch() noexcept { return epoch_.load(std::memory_order_relaxed); }

  private:

    /** @c std::atomic_ref::wait: parks while the word reads @p seen, reported to the ghost first. */
    void wait_until_changed_(std::uint32_t seen) noexcept {
        if (parked_on_) parked_on_->store(seen, std::memory_order_relaxed);
        while (epoch_.load(knobs_.wake_order) == seen) {}
        wakes_.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<std::uint32_t> epoch_;
    std::atomic<std::uint32_t>* parked_on_;
    std::atomic<std::uint32_t> wakes_ {0}; // ? Ghost: a woken retry is no zero-net-effect spin to GenMC
};

/** The words of @c committer_and_holder: the claim starts held by the holder. */
template <knobs_t const& knobs_>
struct maintenance {
    parking_claim<knobs_> claim {1};
    std::atomic<std::uint32_t> view {0};
    bool maintained = false; // ? Written only under the claim
};

template <knobs_t const& knobs_>
void maintain(maintenance<knobs_>& shared) noexcept {
    if (shared.view.load(std::memory_order_acquire) == 1) shared.maintained = true;
}

/** A commit's view published, then @c maintain: the claim tried once, maintained under if taken. */
template <knobs_t const& knobs_>
void* commit(void* argument) noexcept {
    auto& shared = *static_cast<maintenance<knobs_>*>(argument);
    shared.view.store(1, std::memory_order_release);
    if (!shared.claim.take_if_free()) return nullptr;
    maintain(shared);
    shared.claim.release();
    return nullptr;
}

/** @c release_claim_: the release, one look at the view, and the claim retaken only if it is due. */
template <knobs_t const& knobs_>
void* hold(void* argument) noexcept {
    auto& shared = *static_cast<maintenance<knobs_>*>(argument);
    shared.claim.release();
    if (shared.view.load(std::memory_order_acquire) != 1 || !shared.claim.take_if_free()) return nullptr;
    maintain(shared);
    shared.claim.release();
    return nullptr;
}

template <knobs_t const& knobs_>
int committer_and_holder_() noexcept {
    maintenance<knobs_> shared;
    thread_t const committer = spawn(commit<knobs_>, &shared);
    thread_t const holder = spawn(hold<knobs_>, &shared);
    join(committer);
    join(holder);
    verify(shared.maintained);
    return 0;
}

/** The words of @c explicit_verbs: a free claim and the tally its holders add to. */
template <knobs_t const& knobs_>
struct verbs {
    parking_claim<knobs_> claim {0};
    std::size_t tally = 0; // ? Plain, so a holder outside the claim's order is a data race
};

/** @c claim_channel_: the claim taken blocking, the tally, the release. */
template <knobs_t const& knobs_>
void* take_blocking(void* argument) noexcept {
    auto& shared = *static_cast<verbs<knobs_>*>(argument);
    shared.claim.take();
    ++shared.tally;
    shared.claim.release();
    return nullptr;
}

/** @c maintain: the claim tried once, and the tally only if taken. */
template <knobs_t const& knobs_>
void* take_once(void* argument) noexcept {
    auto& shared = *static_cast<verbs<knobs_>*>(argument);
    if (!shared.claim.take_if_free()) return nullptr;
    ++shared.tally;
    shared.claim.release();
    return nullptr;
}

template <knobs_t const& knobs_>
int explicit_verbs_() noexcept {
    verbs<knobs_> shared;
    thread_t const first = spawn(take_blocking<knobs_>, &shared);
    thread_t const second = spawn(take_blocking<knobs_>, &shared);
    thread_t const trier = spawn(take_once<knobs_>, &shared);
    join(first);
    join(second);
    join(trier);
    verify(shared.tally >= 2);
    return 0;
}

/** The words of @c stalled_committer: the claim starts held by the flusher, the pressure stalled. */
template <knobs_t const& knobs_>
struct stall {
    std::atomic<std::uint32_t> parked_on {0}; // ? Ghost: the epoch the committer last parked on
    std::atomic<std::uint32_t> done {0};      // ? Ghost: the committer saw the pressure relieved
    parking_claim<knobs_> claim {1, &parked_on};
    std::atomic<std::uint32_t> pressure {1};
};

/** @c make_room_: the pressure read, and while it is stalled, the claim tried and flushed under, or
 *  the holder's release waited for; or a blocking @c take with @c until_taken. */
template <knobs_t const& knobs_>
void* make_room(void* argument) noexcept {
    auto& shared = *static_cast<stall<knobs_>*>(argument);
    while (shared.pressure.load(std::memory_order_acquire) != 0) {
        if constexpr (knobs_.stalled_wait == stalled_wait_t::until_released) {
            if (!shared.claim.take_if_free()) {
                shared.claim.wait_until_released();
                continue;
            }
        }
        else shared.claim.take();
        shared.pressure.store(0, std::memory_order_release);
        shared.claim.release();
    }
    shared.done.store(1, std::memory_order_relaxed);
    return nullptr;
}

/** A flusher holding the claim: each round waits until the committer parked on the epoch it holds
 *  or is done, flushes, releases and retakes at once, and the last claim it takes it keeps. */
template <knobs_t const& knobs_>
void* flush_back_to_back(void* argument) noexcept {
    auto& shared = *static_cast<stall<knobs_>*>(argument);
    std::uint32_t holding = 1;
    for (std::size_t round = 0; round != knobs_.rounds; ++round) {
        while (shared.parked_on.load(std::memory_order_relaxed) != holding &&
               shared.done.load(std::memory_order_relaxed) == 0) {}
        shared.pressure.store(0, std::memory_order_release);
        shared.claim.release();
        shared.claim.take();
        holding = shared.claim.held_epoch();
    }
    return nullptr;
}

template <knobs_t const& knobs_>
int stalled_committer_() noexcept {
    stall<knobs_> shared;
    thread_t const committer = spawn(make_room<knobs_>, &shared);
    thread_t const flusher = spawn(flush_back_to_back<knobs_>, &shared);
    join(committer);
    join(flusher);
    return 0;
}

extern "C" int committer_and_holder() { return committer_and_holder_<header_k>(); }
extern "C" int committer_and_holder_try_fence_false() { return committer_and_holder_<try_fence_false_k>(); }
extern "C" int committer_and_holder_release_fence_false() { return committer_and_holder_<release_fence_false_k>(); }
extern "C" int explicit_verbs() { return explicit_verbs_<header_k>(); }
extern "C" int explicit_verbs_take_order_relaxed() { return explicit_verbs_<take_order_relaxed_k>(); }
extern "C" int explicit_verbs_release_order_relaxed() { return explicit_verbs_<release_order_relaxed_k>(); }
extern "C" int stalled_committer() { return stalled_committer_<header_k>(); }
extern "C" int stalled_committer_release_step_cleared() { return stalled_committer_<release_step_cleared_k>(); }
extern "C" int stalled_committer_stalled_wait_until_taken() { return stalled_committer_<stalled_wait_until_taken_k>(); }
extern "C" int stalled_committer_rounds_1() { return stalled_committer_<rounds_1_k>(); }
extern "C" int stalled_committer_rounds_1_wake_order_acquire() {
    return stalled_committer_<rounds_1_wake_order_acquire_k>();
}
