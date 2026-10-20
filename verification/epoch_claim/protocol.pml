/**
 *  @file verification/epoch_claim/protocol.pml
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief Spin model of @c parking_claim_t in `include/ustore/blobs/nvme_database.hpp` as an epoch:
 *      one word, odd while held and even while free, taken by a @c fetch_or and stepped by its holder.
 *
 *  The claim orders a database's maintenance, catalog and blob writes. @c take_if_free is a
 *  sequentially consistent fence, then a @c fetch_or of the held bit with an acquire: an even word
 *  became odd and the caller holds it, an odd one is written back unchanged and the caller returns
 *  at once. @c take repeats the same @c fetch_or, parked on the odd value it read between attempts.
 *  @c release adds one with a release, which steps the odd epoch to the next even one, then fences
 *  and notifies. @c wait_until_released reads the epoch with an acquire and, while it reads odd,
 *  parks on that very value, so it wakes once that holder lets go, whoever takes the claim next.
 *  No compare-exchange is spelled anywhere.
 *
 *  With @c waiting at @c handoff, a blocking taker that finds the claim held announces itself in a
 *  word of its own, indexed by its thread, and a releasing holder hands the claim to the first
 *  announced thread after its own index rather than letting it go. Only the owner writes
 *  @c announced, and writes @c idle back once it holds the claim, whether handed it or not; only a
 *  holder writes @c handed. The taker stores @c announced, fences, and tries the @c fetch_or once
 *  more, so a holder whose fenced scan missed the announcement has not let go when the taker parks.
 *  A handing holder stores @c handed with @c handoff_order, then steps the epoch by
 *  @c handoff_step with @c release_order, odd to the next odd, so the claim never reads free in
 *  between and every caller parked on the epoch still wakes, then fences and notifies. A holder
 *  that finds nobody announced lets go as before, and with @c release_look looks once more after
 *  its fence, as @c release_claim_ looks at the views: finding an announcement, it takes the claim
 *  back with @c take_if_free and hands it over. With @c release_scan false the scan before letting
 *  go is skipped and the look alone hands the claim. An announced taker parks on its announcement
 *  until it reads @c handed, or with @c announced_wait at @c until_stepped on the epoch, retrying
 *  the @c fetch_or after every step.
 *
 *  Every take that wins counts itself into @c holders and asserts it is alone, a taker reading
 *  itself handed the claim counts itself in the same way, and every release or handoff counts its
 *  holder out, so one holder at a time is checked in every scenario beside this file. A scenario
 *  that starts with the claim held seeds the epoch odd and @c holders at one. A blocking taker is
 *  @c queued from its announcement, or under @c retried from its first refused attempt, until it
 *  holds the claim, which a scenario's gate reads without ordering anything.
 *
 *  A sequentially consistent fence has no inline in `weak_memory.pml`, whose views carry only
 *  release and acquire, so @c fence_seq_cst stands in an acquire-release read-modify-write of the
 *  one word @c fences that every such fence touches. Two of them are ordered like any two writes to
 *  one word, and the later one acquires everything before the earlier one, which is the store
 *  buffering RC11's fences forbid; it is stronger than a fence only in carrying the earlier thread's
 *  whole view, and never in what a mutant drops. `client.cpp` runs the real fences under RC11 and
 *  IMM.
 *
 *  Every park is the futex-style @c wait_on of `monitor_wait.pml`, woken only by the notify that
 *  ends @c release or a handoff, and its reload is @c std::atomic_ref::wait's relaxed load.
 *
 *  Include before the scenario's roles; every scenario defines its shape first.
 */
#include "../../forkunion/verification/weak_memory.pml"
#include "../../forkunion/verification/monitor_wait.pml"

/** The words: the claim's epoch, the word standing in for every sequentially consistent fence, one
 *  announcement per thread, then the one word each scenario guards: a commit's published view, a
 *  collection's memory pressure, and the holders' tally. */
#define epoch 0
#define fences 1
#define announcement(t) (2 + (t))
#define view (2 + thread_count)
#define pressure (3 + thread_count)
#define tally (4 + thread_count)

/** The values of @c release_step: the epoch stepped to the next even value, or a plain flag cleared
 *  back to zero. */
#define stepped 0
#define cleared 1

/** The values of @c stalled_wait: a stalled committer parks until the holder lets go and reads the
 *  pressure again, or it blocks until it takes the claim itself. */
#define until_released 0
#define until_taken 1

/** The values of @c waiting: a blocking taker retries the @c fetch_or after every release, or it
 *  announces itself and a releasing holder hands it the claim. */
#define retried 0
#define handoff 1

/** The values of @c announced_wait: an announced taker parks on its announcement until a holder
 *  hands it the claim, or on the epoch until it steps. */
#define until_handed 0
#define until_stepped 1

/** The values of an announcement: its thread wants nothing, waits for the claim, or was handed it. */
#define idle 0
#define announced 1
#define handed 2

/** The knobs: the proposed header's choices, the flusher's back-to-back rounds, the two designs the
 *  epoch replaces, and the announced handoff; each is overridden by a `@verify` line. */
#ifndef try_fence
#define try_fence true
#endif
#ifndef release_fence
#define release_fence true
#endif
#ifndef take_order
#define take_order order_acquire
#endif
#ifndef release_order
#define release_order order_release
#endif
#ifndef wake_order
#define wake_order order_relaxed
#endif
#ifndef release_step
#define release_step stepped
#endif
#if release_step != stepped && release_step != cleared
#error "release_step is stepped or cleared"
#endif
#ifndef stalled_wait
#define stalled_wait until_released
#endif
#if stalled_wait != until_released && stalled_wait != until_taken
#error "stalled_wait is until_released or until_taken"
#endif
#ifndef rounds
#define rounds 2
#endif
#ifndef waiting
#define waiting retried
#endif
#if waiting != retried && waiting != handoff
#error "waiting is retried or handoff"
#endif
#ifndef announced_wait
#define announced_wait until_handed
#endif
#if announced_wait != until_handed && announced_wait != until_stepped
#error "announced_wait is until_handed or until_stepped"
#endif
#ifndef release_scan
#define release_scan true
#endif
#ifndef release_look
#define release_look true
#endif
#ifndef handoff_step
#define handoff_step 2
#endif
#ifndef handoff_order
#define handoff_order order_release
#endif

#define held(value) ((value) % 2 == 1)

/** Thread @p t is parked on a wait that no notify has ended yet. */
#define parked(t) (armed[t] != 0 && !event[t])

byte holders;                 // threads between a take and its release
bool queued[thread_count];    // a blocking take found the claim held and does not hold it yet
byte successor[thread_count]; // the announcement a holder's scan stopped at
bool found[thread_count];     // the scan stopped at an announced thread
bool retaken[thread_count];   // the look took the claim back to hand it over
hidden int fenced;            // scratch: the value a fence's stand-in read
hidden int glanced;           // scratch: what a scan step or a retake read

/** A sequentially consistent fence: an acquire-release read-modify-write of @c fences. */
inline fence_seq_cst(t) { read_modify_write(t, fences, order_acq_rel, fenced, 0) }

/** The held bit set with @c take_order, which writes an odd word back unchanged; @p seen is what it
 *  read, and an even one means the caller holds the claim. */
inline set_held(t, seen) {
    atomic {
        read_modify_write(t, epoch, take_order, seen, seen | 1);
        if
        :: !held(seen) -> holders++; assert(holders == 1); queued[t] = false
        :: else
        fi
    }
}

/** The fence, then one @c fetch_or, which holds the claim if @p seen reads even:
 *  @c parking_claim_t::take_if_free. */
inline take_if_free(t, seen) {
    if
    :: try_fence -> fence_seq_cst(t)
    :: else
    fi;
    set_held(t, seen)
}

/** Parks while the epoch reads @p seen, reloading it into @p now after every wake:
 *  @c standard_waiting_policy_t::wait_until_changed over @c std::atomic_ref::wait. */
inline wait_until_changed(t, seen, now) {
    do
    :: load(t, epoch, wake_order, now);
       if
       :: now != seen -> break
       :: else -> wait_on(t, epoch, seen, false)
       fi
    od
}

/** The epoch read with an acquire, and while it reads held, a park until it moves:
 *  @c parking_claim_t::wait_until_released. */
inline wait_until_released(t, seen, now) {
    load(t, epoch, order_acquire, seen);
    if
    :: held(seen) -> wait_until_changed(t, seen, now)
    :: else
    fi
}

/** The caller's announcement read with an acquire into @p now; reading @c handed, it holds the
 *  claim. */
inline read_handed(t, now) {
    atomic {
        load(t, announcement(t), order_acquire, now);
        if
        :: now == handed -> holders++; assert(holders == 1); queued[t] = false
        :: else
        fi
    }
}

/** Parks on the caller's announcement until a holder hands it the claim. */
inline wait_until_handed(t, now) {
    do
    :: wait_on(t, announcement(t), announced, false);
       read_handed(t, now);
       if
       :: now == handed -> break
       :: else
       fi
    od
}

/** The announcement, the fence, and the @c fetch_or retried until it holds the claim or reads
 *  itself handed it, parked between attempts as @c announced_wait says; the announcement is
 *  written back to @c idle once it holds the claim. */
inline take_announced(t, seen, now) {
    atomic { store(t, announcement(t), order_relaxed, announced); queued[t] = true };
    fence_seq_cst(t);
    do
    :: set_held(t, seen);
       if
       :: !held(seen) -> break
       :: else
       fi;
       read_handed(t, now);
       if
       :: now == handed -> break
       :: else
       fi;
       if
       :: announced_wait == until_handed -> wait_until_handed(t, now); break
       :: else -> wait_until_changed(t, seen, now)
       fi
    od;
    store(t, announcement(t), order_relaxed, idle)
}

/** The @c fetch_or, retried after a park on the odd value it read, or under @c handoff, once, and
 *  then announced: @c parking_claim_t::take. */
inline take(t, seen, now) {
    if
    :: waiting == retried ->
        do
        :: atomic { set_held(t, seen); queued[t] = held(seen) };
           if
           :: !held(seen) -> break
           :: else -> wait_until_changed(t, seen, now)
           fi
        od
    :: else ->
        set_held(t, seen);
        if
        :: held(seen) -> take_announced(t, seen, now)
        :: else
        fi
    fi
}

/** The announcements read relaxed, round robin from the thread after @p t, until one reads
 *  @c announced: @c found then holds, and @c successor names it. */
inline scan(t) {
    successor[t] = (t + 1) % thread_count;
    do
    :: found[t] || successor[t] == t -> break
    :: else ->
        atomic {
            load(t, announcement(successor[t]), order_relaxed, glanced);
            found[t] = glanced == announced;
            glanced = 0;
            if
            :: !found[t] -> successor[t] = (successor[t] + 1) % thread_count
            :: else
            fi
        }
    od
}

/** The epoch stepped to the next even value with @c release_order, or the flag cleared, then the
 *  fence and the notify. The notify shares the fence's step, since a waiter arming in between
 *  re-checks the word. */
inline let_go(t) {
    atomic {
        holders--;
        if
        :: release_step == stepped -> add_no_return(t, epoch, release_order, 1)
        :: else -> add_no_return(t, epoch, release_order, -1)
        fi
    };
    atomic {
        if
        :: release_fence -> fence_seq_cst(t)
        :: else
        fi;
        wake(epoch)
    }
}

/** The claim handed to the thread @c successor names: @c handed stored with @c handoff_order, the
 *  epoch stepped by @c handoff_step with @c release_order, then the fence and the notify on the
 *  announcement and on the epoch. */
inline hand_over(t) {
    atomic { holders--; store(t, announcement(successor[t]), handoff_order, handed) };
    add_no_return(t, epoch, release_order, handoff_step);
    atomic {
        if
        :: release_fence -> fence_seq_cst(t)
        :: else
        fi;
        wake(announcement(successor[t]));
        wake(epoch);
        found[t] = false;
        successor[t] = 0;
        retaken[t] = false
    }
}

/** @c parking_claim_t::release: under @c retried the epoch let go; under @c handoff the fenced
 *  scan and the handoff, else the epoch let go, the look, and on an announcement the claim taken
 *  back and scanned for again. */
inline release(t) {
    if
    :: waiting == retried -> let_go(t)
    :: else ->
        do
        :: if
           :: release_scan || retaken[t] ->
               if
               :: release_scan -> fence_seq_cst(t)
               :: else
               fi;
               scan(t)
           :: else
           fi;
           if
           :: found[t] -> hand_over(t); break
           :: else -> successor[t] = 0
           fi;
           let_go(t);
           if
           :: release_look -> scan(t)
           :: else
           fi;
           if
           :: found[t] -> found[t] = false; successor[t] = 0
           :: else -> successor[t] = 0; retaken[t] = false; break
           fi;
           if
           :: try_fence -> fence_seq_cst(t)
           :: else
           fi;
           atomic { set_held(t, glanced); retaken[t] = !held(glanced); glanced = 0 };
           if
           :: !retaken[t] -> break
           :: else
           fi
        od
    fi
}
