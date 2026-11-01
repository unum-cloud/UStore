/**
 *  @file verification/epoch_claim/explicit_verbs.pml
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief Two explicit verbs taking the claim blocking, through @c claim_channel_, beside a
 *      committer trying it in rounds, each holder adding one to a tally under it.
 *
 *  The takers are @c flush, @c compact or @c create_collection: @c take, parked between attempts.
 *  The trier is @c maintain followed by @c release_claim_, @c rounds times: @c take_if_free, and
 *  only if it took the claim, the tally, the release and the retake, with the tally once more
 *  under a retaken claim. Every holder reads the tally relaxed, checks it counts every holder
 *  before it, and writes it back one higher, relaxed as well, so only the claim's acquire and
 *  release carry it between holders.
 *
 *  - One holder at a time, and every holder reads its predecessors' writes.
 *  - Nobody parks for good: every taker's park ends with a release's notify.
 *  - No retake passes a parked taker twice.
 *
 *  @budget 8192: the handoff and weaker-memory searches store about 1.5 GB of states each
 *  @verify pass sc: a parked taker is never passed twice by retakes, since a retake reads the
 *      parked count after the fence that follows its own release
 *  @verify pass rc11 rounds=1: one retake under weaker memory; two rounds need more than 16 GB
 *      under rc11 and one more than 8 GB under far, so only sequential memory checks a second pass
 *  @verify fail rc11 take_order=order_relaxed: relaxed, the next holder reads the tally from before
 *      the last holder's increment
 *  @verify fail rc11 release_order=order_relaxed: relaxed, the next holder reads the tally from
 *      before the last holder's increment
 *  @verify stuck far release_order=order_relaxed: the takers arriving in between read the epoch
 *      still held and park, and the add landing later notifies nobody
 *  @verify stuck sc recheck=false: without it, a release landing between the taker's load and its
 *      park notifies nobody, and the taker sleeps for good
 *  @verify pass sc release_step=cleared: with finite holders a flag starves nobody
 *  @verify pass rc11 rounds=1 release_step=cleared: the same under weaker memory, one round wide
 *  @verify pass sc waiting=handoff: a taker refused once announces itself and a releasing holder
 *      hands it the claim
 *  @verify stuck sc,rc11,far waiting=handoff release_look=false: with no look after letting go,
 *      nobody hands it the claim
 *  @verify pass sc waiting=handoff release_look=false announced_wait=until_stepped: parked on the
 *      epoch instead, the announced taker wakes on the holder's step and retries the @c fetch_or
 *  @verify fail sc,rc11,far waiting=handoff handoff_step=1: stepped by one it reads free, and a
 *      @c take_if_free takes the claim beside the taker it was handed to
 *  @verify fail rc11,far waiting=handoff handoff_order=order_relaxed: relaxed, the handed taker
 *      reads the tally from before its predecessor's increment
 *  @verify pass sc waiting=handoff handoff_order=order_relaxed: sequential memory carries the
 *      predecessor's increment to the handed taker without the release
 */
#define thread_count 3
#define location_count (6 + thread_count)
#ifdef waiting // set only to handoff, whose value the protocol defines after the shape
#define history_depth 30
#else
#define history_depth 18
#endif
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

/** @c maintain and @c release_claim_ in rounds: the claim tried, and only if taken, the tally, the
 *  release, the retake, and under a retaken claim the tally and the release once more. */
proctype trier(byte t) {
    byte round;
    int seen, seen_tally;
    for (round : 1 .. rounds) {
        take_if_free(t, seen);
        if
        :: !held(seen) ->
            increment(t, seen_tally);
            release(t);
            retake(t, seen);
            if
            :: !held(seen) ->
                increment(t, seen_tally);
                release(t)
            :: else
            fi
        :: else
        fi
    };
    landed(t)
}

init { atomic { run taker(0); run taker(1); run trier(2) } }
