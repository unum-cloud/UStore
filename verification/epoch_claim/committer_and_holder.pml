/**
 *  @file verification/epoch_claim/committer_and_holder.pml
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief A committer publishing its view and trying the claim while the holder lets go and looks
 *      at the views once: either the committer takes the claim or the holder's look sees the view.
 *
 *  The committer is @c maintain after a commit: its view is published with a release, then
 *  @c take_if_free, and once it holds the claim it maintains, which reads its own view. The holder
 *  starts with the claim and runs @c release_claim_. The two sides are store buffering across two
 *  words, the view and the epoch, so neither the release nor the acquire closes it, and only a
 *  fence on each side does.
 *
 *  - No maintenance is lost: once both are done, someone maintained holding the claim with the view visible.
 *
 *  @verify pass sc,rc11,far
 *  @verify fail rc11,far try_fence=false: the holder's look after its own fence reads the view from
 *      before the commit, so nobody maintains it
 *  @verify fail rc11,far release_fence=false: without it, the holder's look reads the view from
 *      before the commit, so nobody maintains it
 *  @verify pass sc try_fence=false release_fence=false: sequential memory has no store buffer
 *  @verify pass rc11 release_step=cleared: a plain flag closes the same race
 *  @verify pass sc,rc11,far waiting=handoff: a release that finds nobody announced lets go and
 *      looks as before
 */
#define thread_count 2
#define location_count (3 + thread_count)
#ifdef waiting // set only to handoff, whose value the protocol defines after the shape
#define history_depth 14
#else
#define history_depth 6
#endif
#include <epoch_claim/protocol.pml>

bool maintained; // someone maintained holding the claim and reading the view
byte finished;   // the roles that are done

/** The maintenance under the claim, which reads the view into the caller's @p observed. */
inline maintain(t, observed) {
    load(t, view, order_acquire, observed);
    maintained = maintained || observed == 1
}

/** The last role out checks that the view was maintained. */
inline finish() {
    atomic {
        finished++;
        if
        :: finished == 2 -> assert(maintained)
        :: else
        fi
    }
}

/** A commit's view published, then @c maintain: the claim tried once, and the maintenance under it
 *  if taken. */
proctype committer(byte t) {
    int seen, observed;
    store(t, view, order_release, 1);
    take_if_free(t, seen);
    if
    :: !held(seen) ->
        maintain(t, observed);
        release(t)
    :: else
    fi;
    landed(t);
    finish()
}

/** @c release_claim_ from a held claim: the release, one look at the view, and only if it is due,
 *  the claim tried again and the maintenance under it. */
proctype holder(byte t) {
    int seen, observed;
    release(t);
    load(t, view, order_acquire, observed);
    if
    :: observed == 1 ->
        take_if_free(t, seen);
        if
        :: !held(seen) ->
            maintain(t, observed);
            release(t)
        :: else
        fi
    :: else
    fi;
    landed(t);
    finish()
}

init { atomic { seed(epoch, 1); holders = 1; run committer(0); run holder(1) } }
