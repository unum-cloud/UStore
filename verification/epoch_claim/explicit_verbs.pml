/**
 *  @file verification/epoch_claim/explicit_verbs.pml
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief Two explicit verbs taking the claim blocking, through @c claim_channel_, beside a
 *      committer trying it once, each holder adding one to a tally under it.
 *
 *  The takers are @c flush, @c compact or @c create_collection: @c take, parked between attempts.
 *  The trier is @c maintain: @c take_if_free once, and the tally only if it took the claim. Every
 *  holder reads the tally relaxed, checks it counts every holder before it, and writes it back one
 *  higher, relaxed as well, so only the claim's acquire and release carry it between holders.
 *
 *  One holder at a time, every holder reads its predecessors' writes, and nobody parks for good:
 *  every taker's park ends with a release's notify.
 *
 *  Fairness is not asserted here, since two takers and one trier release the claim only as often
 *  as they take it; a taker against a holder retaking back to back is `stalled_committer.pml`'s
 *  `stalled_wait=until_taken` line, which starves.
 *
 *  @verify pass sc,rc11,far
 *  @verify fail rc11 take_order=order_relaxed: the @c fetch_or that takes the claim acquires;
 *      relaxed, the next holder reads the tally from before the last holder's increment
 *  @verify fail rc11 release_order=order_relaxed: the add that releases the claim releases;
 *      relaxed, the next holder reads the tally from before the last holder's increment
 *  @verify stuck far release_order=order_relaxed: a relaxed add is posted as RAO-INT's @c aadd,
 *      so the release's notify goes out before the add lands; the takers arriving in between read
 *      the epoch still held and park, and the add landing later notifies nobody
 *  @verify stuck sc recheck=false: a parked taker re-checks the epoch after it arms, as the futex
 *      does under its lock; without it, a release landing between the taker's load and its park
 *      notifies nobody, and the taker sleeps for good
 *  @verify pass sc,rc11 release_step=cleared: with finite holders a flag starves nobody, since the
 *      last release leaves it cleared and every parked taker wakes to it
 */
#define thread_count 3
#define location_count 5
#define history_depth 10
#include "protocol.pml"

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

init { atomic { run taker(0); run taker(1); run trier(2) } }
