/**
 *  @file verification/epoch_claim/explicit_handoff.pml
 *  @author Ash Vardanian
 *  @date October 27, 2026
 *  @brief One explicit verb taking the claim blocking under @c handoff, beside a committer trying
 *      it once, each holder adding one to a tally under it.
 *
 *  This is `explicit_verbs.pml` with one blocking taker fewer, so that its weaker memory models fit
 *  a search. The taker is @c take with @c waiting at @c handoff, the trier is @c maintain. A taker
 *  refused by the trier announces itself, and the trier's release hands it the claim. Every holder
 *  reads the tally relaxed, so only the handoff's orders carry the trier's increment to the taker.
 *
 *  - One holder at a time, and every holder reads its predecessors' writes.
 *  - The taker never parks for good.
 *
 *  @verify pass rc11,far: a taker announcing while the holder lets go, under weaker memory
 *  @verify pass rc11,far announced_wait=until_stepped: an announced taker parked on the epoch
 *      wakes on every step and retries the @c fetch_or
 *  @verify pass rc11,far release_look=false announced_wait=until_stepped: parked on the epoch it
 *      wakes on the trier's step and retries the @c fetch_or
 */
#define thread_count 2
#define location_count (5 + thread_count)
#define history_depth 10
#define waiting handoff
#include <epoch_claim/protocol.pml>

byte admitted; // holders so far, the tally's due

/** One increment of the tally under the claim, over the caller's @p seen_tally. */
inline increment(t, seen_tally) {
    atomic { admitted++; seen_tally = 0 };
    load(t, tally, order_relaxed, seen_tally);
    assert(seen_tally == admitted - 1);
    store(t, tally, order_relaxed, seen_tally + 1)
}

/** @c claim_channel_: the claim taken blocking, the tally, the release. */
proctype taker(byte t) {
    int seen, now, seen_tally;
    take(t, seen, now);
    increment(t, seen_tally);
    release(t);
    landed(t)
}

/** @c maintain: the claim tried once, and the tally only if taken. */
proctype trier(byte t) {
    int seen, seen_tally;
    take_if_free(t, seen);
    if
    :: !held(seen) ->
        increment(t, seen_tally);
        release(t)
    :: else
    fi;
    landed(t)
}

init { atomic { run taker(0); run trier(1) } }
