/**
 *  The completion queue an owner reaps from and the kernel-side list that catches what the queue has
 *  no room for, as the ring of `src/nvme/ring.hpp` fixes it and `src/nvme/linux_uring_files_disk.hpp`
 *  implements it: an owner submits against bounded credit, every submitted request yields one
 *  terminal completion, and a completion the visible queue cannot hold goes onto an overflow list
 *  rather than being dropped. Nothing is lost there, since `IORING_SETUP_CQ_NODROP` promises it, but
 *  an overflowed entry stays invisible until the owner enters the kernel again, which is why `reap`
 *  tests `IORING_SQ_CQ_OVERFLOW` and enters a second time when the first harvest did not fill the
 *  caller's span.
 *
 *  A cancellation is what makes the full queue reachable at all. The cancelling entry posts a
 *  completion of its own, which `reap` recognises and discards and which no lease ever budgeted
 *  credit for, so a queue sized for one owner's credit still fills with an entry that retires
 *  nothing and pushes a real completion into the overflow list behind it.
 *
 *  Shape: one owner, `requests` requests, `credit` of them outstanding at once, `queue_seats`
 *  visible seats and an overflow list of `credit + 1 - queue_seats`. One request of credit and one
 *  visible seat is the smallest shape where the cancelling entry can fill the queue, and a wider
 *  queue only postpones the same state. Two requests is the smallest number an owner can exceed its
 *  credit with, which is what the second knob needs; one request would leave that knob checking
 *  nothing. The overflow list is sized at what the credit budget proves is enough - at most `credit`
 *  request completions plus the one the cancelling entry posts, less what the visible queue already
 *  holds - so a completion that fits nowhere means the budget was wrong rather than the kernel
 *  stingy.
 *
 *  Abstracted away: the bytes, the buffers and the alignment, since a completion retires a request
 *  whatever it carried; the request's outcome, since `completed_k`, `partial_k`, `failed_k` and
 *  `cancelled_k` are terminal alike and only `continued_k` is not, which no transport here posts;
 *  the identity of one request against another, since the ring promises no ordering between two
 *  submitted requests and the model would otherwise assert one; and the submission queue, since a
 *  `submit` the kernel refuses is answered on the spot rather than through the completion queue.
 *  Every action is one step - posting one completion, harvesting one, flushing one entry, submitting
 *  one request - because the hazard is which action happens when and not what a half-finished one
 *  leaves behind. A completion is in exactly one place throughout, so the flush moves an entry
 *  rather than copying it, and `delivered` can never outrun `submitted`.
 *
 *  Properties:
 *  - `every_request_retires`: once the owner and the device have both gone quiet, every request the
 *    owner submitted has yielded exactly one terminal completion to it, and the owner submitted all
 *    of them. A stalled reaper quiesces with credit it can never earn back.
 *  - `no_completion_is_lost`: the kernel never has to drop a completion for want of a seat, and no
 *    completion the owner is owed is left sitting in the overflow list once everything has gone
 *    quiet. The cancelling entry's own completion counts towards a seat, since it occupies one, and
 *    not towards what the owner is owed, since `reap` discards it.
 *
 *  Knobs, each of which must fail:
 *  - `-Dwithout_overflow_check` drains the visible queue and never enters again to flush the
 *    overflow list, so a completion the cancelling entry pushed out of the queue stays invisible and
 *    the owner quiesces holding credit for a request it will never see retire.
 *  - `-Dwithout_credit_bound` submits past the owner's credit, so more completions are outstanding
 *    than the queue and the overflow list together were sized for and one has nowhere to go.
 */

#ifndef requests
#define requests 2
#endif

#ifndef credit
#define credit 1
#endif

#ifndef queue_seats
#define queue_seats 1
#endif

// What the credit budget proves the overflow list needs: one owner's credit plus the completion the
// cancelling entry posts, less the seats the visible queue already offers.
#define overflow_seats (credit + 1 - queue_seats)

// The cancelling entry, which travels the same queues and retires nothing.
#define cancel_unposted 0
#define cancel_in_kernel 1
#define cancel_in_queue 2
#define cancel_overflowed 3
#define cancel_consumed 4

// Requests handed to the kernel, ever, and terminal completions handed back to the owner.
byte submitted = 0;
byte delivered = 0;

// Requests the kernel holds and has not yet answered for.
byte flights = 0;

// Request completions sitting in the visible completion queue.
byte queue_requests = 0;

// Request completions the kernel holds back because the queue had no room for them.
byte overflow_requests = 0;

byte cancel_state = cancel_unposted;

bit owner_finished = 0;
bit device_finished = 0;

#define queue_used (queue_requests + (cancel_state == cancel_in_queue))
#define overflow_used (overflow_requests + (cancel_state == cancel_overflowed))

// Requests submitted and not yet reaped, which is the credit the owner has spent.
#define outstanding (submitted - delivered)

// The device answers one request at a time, and the cancelling entry answers for itself.
active proctype device() {
    bit no_completion_is_lost;
    do
    :: atomic {
        flights != 0 ->
            flights--;
            if
            :: queue_used < queue_seats -> queue_requests++
            :: else ->
                no_completion_is_lost = overflow_used < overflow_seats;
                assert(no_completion_is_lost);
                overflow_requests++
            fi
       }
    :: atomic {
        cancel_state == cancel_in_kernel ->
            if
            :: queue_used < queue_seats -> cancel_state = cancel_in_queue
            :: else ->
                no_completion_is_lost = overflow_used < overflow_seats;
                assert(no_completion_is_lost);
                cancel_state = cancel_overflowed
            fi
       }
    :: timeout -> break
    od;
    device_finished = 1
}

// The one thread that submits on this lease and reaps it, which is the only thread either is legal
// from. It breaks when nothing anywhere can move, which is the quiescence the properties read.
active proctype owner() {
    do
#ifdef without_credit_bound
    :: atomic { submitted != requests -> submitted++; flights++ }
#else
    :: atomic { submitted != requests && outstanding < credit -> submitted++; flights++ }
#endif
    :: atomic { cancel_state == cancel_unposted && flights != 0 -> cancel_state = cancel_in_kernel }
    :: atomic { queue_requests != 0 -> queue_requests--; delivered++ }
    :: atomic { cancel_state == cancel_in_queue -> cancel_state = cancel_consumed }
#ifndef without_overflow_check
    :: atomic {
        overflow_used != 0 && queue_used < queue_seats ->
            if
            :: overflow_requests != 0 -> overflow_requests--; queue_requests++
            :: cancel_state == cancel_overflowed -> cancel_state = cancel_in_queue
            fi
       }
#endif
    :: timeout -> break
    od;
    owner_finished = 1
}

init {
    bit every_request_retires;
    bit no_completion_is_lost;
    (owner_finished && device_finished);
    every_request_retires = (submitted == requests && delivered == requests);
    assert(every_request_retires);
    no_completion_is_lost = (flights == 0 && queue_requests == 0 && overflow_requests == 0);
    assert(no_completion_is_lost)
}
