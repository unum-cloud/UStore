/**
 *  @file verification/epoch_claim/stalled_committer.pml
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief A committer stalled past twice its memory bound, against a flusher that hands the claim
 *      back and takes it again at once, round after round.
 *
 *  The committer runs @c make_room_: it reads the pressure with an acquire and leaves once it is
 *  relieved; otherwise it tries the claim, flushes under it if taken, and if not, waits until the
 *  holder lets go and reads the pressure again. The flusher starts holding the claim and runs
 *  @c rounds rounds of a flush, a release and a blocking @c take, then keeps the last claim it took
 *  for good.
 *
 *  - The committer makes progress: it never stays parked once the flusher stops.
 *
 *  @verify pass sc,rc11,far
 *  @verify stuck sc,rc11 release_step=cleared: a plain flag cleared to zero is set back to one by
 *      the flusher's retake before the woken committer reloads it, so its wait reads the very value
 *      it parked on and parks again
 *  @verify stuck sc,rc11 stalled_wait=until_taken: blocked in @c take, the committer wakes on every
 *      release yet finds the claim retaken, and the flush that already relieved it is never read
 *  @verify stuck sc stalled_wait=until_taken release_step=cleared: the design this claim replaced,
 *      a flag and a blocking take, starves the same way
 *  @verify pass sc rounds=1: under sequential memory the committer leaves within one round
 *  @verify stuck rc11 rounds=1: the wake's reload is relaxed, so the woken committer may read the
 *      pressure stale
 *  @verify pass rc11 rounds=1 wake_order=order_acquire: reloaded with an acquire, the wake
 *      synchronizes with the release it saw
 *  @verify pass sc,rc11,far waiting=handoff: @c make_room_ only tries the claim and never
 *      announces, so the committer still wakes on the epoch's step
 *  @verify stuck rc11,far waiting=handoff release_look=false: with no look after letting go, nobody
 *      hands it the claim and it parks on its announcement for good
 *  @verify pass sc,rc11,far waiting=handoff release_look=false announced_wait=until_stepped: an
 *      announced flusher parked on the epoch wakes on the committer's step and retries the
 *      @c fetch_or
 */
#define thread_count 2
#define location_count (6 + thread_count)
#ifdef waiting // set only to handoff, whose value the protocol defines after the shape
#define history_depth 20
#else
#define history_depth 10
#endif
#include <epoch_claim/protocol.pml>

bool committer_done; // the committer saw the pressure relieved

/** The collection's memory runs flushed, which relieves the pressure: a release, as the catalog
 *  naming the flushed run is published. */
inline flush(t) { store(t, pressure, order_release, 0) }

/** @c make_room_: the pressure read, and while it is stalled, the claim tried and flushed under, or
 *  the holder's release waited for; with @c stalled_wait at @c until_taken, a blocking @c take. */
proctype committer(byte t) {
    int stalled, seen, now;
    do
    :: load(t, pressure, order_acquire, stalled);
       if
       :: stalled == 0 -> break
       :: else
       fi;
       if
       :: stalled_wait == until_released ->
           take_if_free(t, seen);
           if
           :: !held(seen) -> flush(t); release(t)
           :: else -> wait_until_released(t, seen, now)
           fi
       :: else -> take(t, seen, now); flush(t); release(t)
       fi
    od;
    landed(t);
    committer_done = true
}

/** A flusher holding the claim: each round waits until the committer is parked or done, flushes,
 *  releases and takes the claim again at once, and the last claim it takes it keeps. */
proctype flusher(byte t) {
    byte round;
    int seen, now;
    for (round : 1 .. rounds) {
        (committer_done || parked(0));
        flush(t);
        release(t);
        take(t, seen, now)
    };
    landed(t)
}

init { atomic { seed(epoch, 1); seed(pressure, 1); holders = 1; run committer(0); run flusher(1) } }
