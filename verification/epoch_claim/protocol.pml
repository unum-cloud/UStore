/**
 *  @file verification/epoch_claim/protocol.pml
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief Spin model of @c maintenance_claim in <tt>include/ustore/blobs/nvme_published_runs.hpp</tt> as
 *      an epoch: one word, odd while held and even while free, taken by a @c fetch_or and stepped
 *      by its holder.
 *
 *  The claim orders a database's maintenance, catalog and blob writes. @c take_if_free is a
 *  sequentially consistent fence, then a @c fetch_or of the held bit with an acquire. @c release
 *  adds one with a release, which steps the odd epoch to the next even one, then fences and
 *  notifies. With @c waiting at @c handoff, a releasing holder hands the claim to the first
 *  announced thread after its own index rather than letting it go. A blocking @c take counts itself
 *  in @c parked_ while it parks, and @c release_claim_ in the same file retakes the claim after its
 *  release only while that count reads zero.
 *
 *  - One holder at a time is checked in every scenario beside this file.
 *  - No compare-exchange is spelled anywhere.
 *  - @c fence_seq_cst stands in an acquire-release read-modify-write of the one word @c fences.
 *  - Every park is the futex-style @c wait_on of <tt>monitor_wait.pml</tt>.
 */
#include <weak_memory.pml>
#include <monitor_wait.pml>

/** The words: the claim's epoch, the word standing in for every sequentially consistent fence, one
 *  announcement per thread, then the one word each scenario guards: a commit's published view, a
 *  collection's memory pressure, and the holders' tally, and last the count of parked takes. */
#define epoch 0
#define fences 1
#define announcement(t) (2 + (t))
#define view (2 + thread_count)
#define pressure (3 + thread_count)
#define tally (4 + thread_count)
#define parked_takes (5 + thread_count)

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

/** The values of an announcement: its thread wants nothing, waits for the claim, or was handed it.
 *  Only the owner writes @c announced, and writes @c idle back once it holds the claim, whether
 *  handed it or not; only a holder writes @c handed. */
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
bool counted[thread_count];   // a blocking take counted itself among the parked
byte passes[thread_count];    // retakes that took the claim while that take was counted, since it last held it
byte successor[thread_count]; // the announcement a holder's scan stopped at
bool found[thread_count];     // the scan stopped at an announced thread
bool retaken[thread_count];   // the look took the claim back to hand it over
hidden int fenced;            // scratch: the value a fence's stand-in read
hidden int glanced;           // scratch: what a scan step or a retake read
hidden byte passed;           // scratch: the take a retake looks at

/** A sequentially consistent fence: an acquire-release read-modify-write of @c fences. Two of them
 *  are ordered like any two writes to one word, and the later one acquires everything before the
 *  earlier one, which is the store buffering RC11's fences forbid; it is stronger than a fence only
 *  in carrying the earlier thread's whole view, and never in what a mutant drops. */
inline fence_seq_cst(t) { read_modify_write(t, fences, order_acq_rel, fenced, 0) }

/** The held bit set with @c take_order, which writes an odd word back unchanged; @p seen is what it
 *  read, and an even one means the caller holds the claim. */
inline set_held(t, seen) {
    atomic {
        read_modify_write(t, epoch, take_order, seen, seen | 1);
        if
        :: !held(seen) -> holders++; assert(holders == 1); queued[t] = false; passes[t] = 0
        :: else
        fi
    }
}

/** The fence, then one @c fetch_or, which holds the claim if @p seen reads even:
 *  @c maintenance_claim::take_if_free. */
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
 *  @c maintenance_claim::wait_until_released. It parks on that very value, so it wakes once that
 *  holder lets go, whoever takes the claim next. */
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
 *  written back to @c idle once it holds the claim. The taker stores @c announced, fences, and
 *  tries the @c fetch_or once more, so a holder whose fenced scan missed the announcement has not
 *  let go when the taker parks. */
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

/** The @c fetch_or, retried after a park on the odd value it read, counted in @c parked_takes once
 *  from its first refusal until it holds the claim, or under @c handoff, once, and then announced:
 *  @c maintenance_claim::take. The count is a sequentially consistent add, an acquire-release add
 *  and the fence's stand-in taken in one step. */
inline take(t, seen, now) {
    if
    :: waiting == retried ->
        atomic { set_held(t, seen); queued[t] = held(seen) };
        if
        :: held(seen) ->
            // One step, as it stands for one sequentially consistent read-modify-write.
            atomic { add_no_return(t, parked_takes, order_acq_rel, 1); fence_seq_cst(t); counted[t] = true };
            do
            :: atomic { set_held(t, seen); queued[t] = held(seen) };
               if
               :: !held(seen) -> break
               :: else -> wait_until_changed(t, seen, now)
               fi
            od;
            atomic { add_no_return(t, parked_takes, order_relaxed, -1); counted[t] = false }
        :: else
        fi
    :: else ->
        set_held(t, seen);
        if
        :: held(seen) -> take_announced(t, seen, now)
        :: else
        fi
    fi
}

/** @c basic_database::release_claim_ after its release: the parked count read, which the release's
 *  fence orders, and only while it reads zero, @c take_if_free; @p seen reads odd when nothing was
 *  taken. A retake that takes the claim passes every take counted parked, and passes none twice. */
inline retake(t, seen) {
    load(t, parked_takes, order_relaxed, glanced);
    if
    :: glanced == 0 -> take_if_free(t, seen)
    :: else -> seen = 1
    fi;
    atomic {
        glanced = 0;
        if
        :: !held(seen) ->
            for (passed : 0 .. thread_count - 1) {
                if
                :: counted[passed] -> passes[passed]++; assert(passes[passed] < 2)
                :: else
                fi
            };
            passed = 0
        :: else
        fi
    }
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
 *  epoch stepped by @c handoff_step with @c release_order, odd to the next odd, so the claim never
 *  reads free in between and every caller parked on the epoch still wakes, then the fence and the
 *  notify on the announcement and on the epoch. */
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

/** @c maintenance_claim::release: under @c retried the epoch let go; under @c handoff the fenced
 *  scan and the handoff, else the epoch let go, the look, and on an announcement the claim taken
 *  back and scanned for again. With @c release_look the holder looks once more after its fence,
 *  as @c release_claim_ looks at the views: finding an announcement, it takes the claim back
 *  with @c take_if_free, fences and scans again, and hands it over. */
inline release(t) {
    if
    :: waiting == retried -> let_go(t)
    :: else ->
        do
        :: fence_seq_cst(t);
           scan(t);
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
