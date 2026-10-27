/**
 *  @file verification/nvme_ring.pml
 *  @author Ash Vardanian
 *  @date September 20, 2026
 *  @brief Spin model of an owner's credit against a bounded completion queue and its overflow list.
 *
 *  The completion queue an owner reaps from and the kernel-side list that catches what the queue
 *  has no room for, as the ring of `src/nvme/ring.hpp` fixes it. An overflowed entry stays
 *  invisible until the owner enters the kernel again, which is why `reap` tests
 *  `IORING_SQ_CQ_OVERFLOW`. A cancellation is what makes the full queue reachable at all. Nothing
 *  is lost in the overflow list, since `IORING_SETUP_CQ_NODROP` promises it, which is not checked.
 *
 *  - `every_request_retires`: every request the owner submitted yields one terminal completion.
 *  - `no_completion_is_lost`: the kernel never has to drop a completion for want of a seat.
 *
 *  @verify pass sc
 *  @verify pass sc requests=3
 *  @verify pass sc credit=2 queue_seats=2
 *  @verify fail sc overflow_check=false: drains the visible queue and never enters again to flush
 *      the overflow list, so the owner quiesces holding credit for a request it will never see retire.
 *  @verify fail sc credit_bound=false: submits past the owner's credit, so more completions are
 *      outstanding than the queue and the overflow list together were sized for and one has nowhere
 *      to go.
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

#ifndef credit_bound
#define credit_bound true
#endif

#ifndef overflow_check
#define overflow_check true
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
    :: atomic { submitted != requests && (!credit_bound || outstanding < credit) -> submitted++; flights++ }
    :: atomic { cancel_state == cancel_unposted && flights != 0 -> cancel_state = cancel_in_kernel }
    :: atomic { queue_requests != 0 -> queue_requests--; delivered++ }
    :: atomic { cancel_state == cancel_in_queue -> cancel_state = cancel_consumed }
    :: atomic {
        overflow_check && overflow_used != 0 && queue_used < queue_seats ->
            if
            :: overflow_requests != 0 -> overflow_requests--; queue_requests++
            :: cancel_state == cancel_overflowed -> cancel_state = cancel_in_queue
            fi
       }
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
