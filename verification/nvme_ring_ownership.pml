/**
 *  @file verification/nvme_ring_ownership.pml
 *  @author Ash Vardanian
 *  @date September 20, 2026
 *  @brief Spin model of one issuing thread per ring, and the leases that move between them.
 *
 *  A ring belongs to exactly one thread: an io_uring ring answers `-EEXIST` to every other, and an
 *  SPDK queue pair polled by a second thread corrupts its tracker list and reports nothing at all.
 *  A lease may still move between threads, but only through an explicit handoff and only while
 *  nothing is outstanding. Two threads holding one lease at once is not representable, because the
 *  slot carries one identity and the handle is move-only.
 *
 *  - `one_issuer_at_a_time`: a thread never reaps a request another thread submitted.
 *  - `handoff_is_quiesced`: a lease never changes hands while it still has work outstanding.
 *  - `no_stale_delivery`: a completion minted under one lease is never delivered to the next one.
 *
 *  @verify pass sc
 *  @verify pass sc submits=3
 *  @verify pass sc reissues=2
 *  @verify pass sc handoffs=2
 *  @verify fail sc generation=false: reissues the slot without raising the generation, so a token
 *      minted under the previous lease passes `submit`'s check and its completion is delivered to the
 *      lease that took the slot over.
 *  @verify fail sc quiesced_handoff=false: lets `adopt` take a lease with work still outstanding, so
 *      the thread that receives it reaps a request the thread that gave it up submitted.
 */

#ifndef submits
#define submits 2
#endif

#ifndef reissues
#define reissues 1
#endif

#ifndef handoffs
#define handoffs 1
#endif

#ifndef generation
#define generation true
#endif

#ifndef quiesced_handoff
#define quiesced_handoff true
#endif

// The slot holds no thread's identity while no lease occupies it.
#define unleased 0

#define adopt_accepts (!quiesced_handoff || outstanding == 0)

// The thread the slot records as its issuer, which every submit and reap is checked against.
byte slot_issuer = unleased;

// What a lease stamps into the tokens it mints, raised every time the slot is reissued.
byte slot_generation = 0;

// The model's own oracle: which lease this really is, raised on every reissue whatever the knobs do.
byte lease_epoch = 0;

// Requests submitted on the lease and not yet reaped, which one request of depth bounds at one.
byte outstanding = 0;

// The thread that submitted the outstanding request, which is the only thread that may reap it.
byte request_issuer = unleased;

// The lease the outstanding request's token was minted under.
byte request_epoch = 0;

// A token a caller minted and kept, which its request may still carry after the slot has moved on.
bit kept_minted = 0;
byte kept_generation = 0;
byte kept_epoch = 0;

byte submits_left = submits;
byte reissues_left = reissues;
byte handoffs_left = handoffs;

// One of the two threads that contend for the slot, take it, drive it and hand it on.
active [2] proctype issuing_thread() {
    bit one_issuer_at_a_time;
    bit handoff_is_quiesced;
    bit no_stale_delivery;
    byte me;
    me = _pid + 1;
    do
    :: atomic { slot_issuer == unleased -> slot_issuer = me }

    // A caller mints a token of the lease it holds and keeps it for a request it builds later.
    :: atomic {
        slot_issuer == me && !kept_minted ->
            kept_minted = 1;
            kept_generation = slot_generation;
            kept_epoch = lease_epoch
       }

    :: atomic {
        slot_issuer == me && submits_left != 0 && outstanding == 0 ->
            submits_left--;
            outstanding = 1;
            request_issuer = me;
            request_epoch = lease_epoch
       }

    // The kept token reaches `submit`, which refuses whatever is not this lease's.
    :: atomic {
        slot_issuer == me && submits_left != 0 && outstanding == 0 && kept_minted ->
            kept_minted = 0;
            if
            :: kept_generation == slot_generation ->
                submits_left--;
                outstanding = 1;
                request_issuer = me;
                request_epoch = kept_epoch
            :: else -> skip
            fi
       }

    :: atomic {
        slot_issuer == me && outstanding != 0 ->
            one_issuer_at_a_time = (request_issuer == me);
            assert(one_issuer_at_a_time);
            no_stale_delivery = (request_epoch == lease_epoch);
            assert(no_stale_delivery);
            outstanding = 0
       }

    // The handle moves to the other thread and that thread announces itself with `adopt`.
    :: atomic {
        slot_issuer == me && handoffs_left != 0 ->
            handoffs_left--;
            if
            :: adopt_accepts ->
                handoff_is_quiesced = (outstanding == 0);
                assert(handoff_is_quiesced);
                slot_issuer = 3 - me
            :: else -> skip
            fi
       }

    :: atomic {
        slot_issuer == me && outstanding == 0 && reissues_left != 0 ->
            reissues_left--;
            lease_epoch++;
            if
            :: generation -> slot_generation++
            :: else -> skip
            fi;
            slot_issuer = unleased
       }

    :: timeout -> break
    od
}
