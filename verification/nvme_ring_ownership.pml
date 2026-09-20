/**
 *  The issuing thread a ring is bound to and the lease that carries it, as `src/nvme/ring.hpp` fixes
 *  it and `src/nvme/disk.hpp` implements it. A ring belongs to exactly one thread: an io_uring ring
 *  set up with `SINGLE_ISSUER` answers `-EEXIST` to every other, and an SPDK queue pair polled by a
 *  second thread corrupts its tracker list and reports nothing at all. A lease may still move
 *  between threads, since a controller offers fewer queue pairs than the executor has threads, but
 *  only through an explicit handoff and only while nothing is outstanding, which is why
 *  `basic_disk::adopt` refuses a lease with pending or ready work. A released lease is reissued with
 *  a raised generation, so a token minted under its predecessor cannot deliver into its successor's
 *  queue.
 *
 *  Shape: one lease slot, two threads, one request of depth, `submits` submissions, `reissues`
 *  release and reissue of the slot, and `handoffs` handoff attempt. One slot is enough because the
 *  hazard is two threads on the same slot and a second slot never interacts with the first. Two
 *  threads is the smallest number a lease can change hands between. One request of depth is enough
 *  because a second outstanding request repeats the first's interleaving rather than adding one. One
 *  reissue is the smallest number that can make a token stale, since a token goes stale exactly when
 *  the slot it names moves on. Two submissions are the smallest pair the generation has to separate:
 *  one under the first lease and one under its successor.
 *
 *  The model keeps a `lease_epoch` the design does not: it is the true identity of whichever lease
 *  occupies the slot, raised on every reissue, and it is the oracle `slot_generation` is checked
 *  against. `submit` tests the generation, as the code does; `no_stale_delivery` tests the epoch, so
 *  a knob that stops raising the generation is caught rather than hidden by its own counter.
 *
 *  Abstracted away: the bytes, the buffers and the registrations, since a lease is about who may
 *  enter the ring and not about what moves through it; the handle's move and the receiver's `adopt`,
 *  which are one step here because between them the lease has no issuer and nothing may touch it;
 *  and the ring rebuild an io_uring handoff performs, because that transport reports a bad handoff
 *  as completions the owner never sees while an SPDK queue pair reports nothing, and a model belongs
 *  against the silent one. Every action is one step, since the hazard is which thread acts when and
 *  not what a half-finished action leaves behind. Two threads holding one lease at once is not
 *  representable, because the slot carries one identity and the handle is move-only; what the model
 *  checks is the consequence the transports punish, which is a thread entering the ring for work
 *  another thread issued.
 *
 *  Properties:
 *  - `one_issuer_at_a_time`: a thread never reaps a request another thread submitted, which is the
 *    `-EEXIST` on io_uring and the silently corrupted tracker list on SPDK.
 *  - `handoff_is_quiesced`: a lease never changes hands while it still has work outstanding.
 *  - `no_stale_delivery`: a completion minted under one lease is never delivered to the lease that
 *    later occupies the same slot.
 *
 *  Knobs, each of which must fail:
 *  - `-Dwithout_generation` reissues the slot without raising the generation, so a token minted
 *    under the previous lease passes `submit`'s check and its completion is delivered to the lease
 *    that took the slot over.
 *  - `-Dwithout_quiesced_handoff` lets `adopt` take a lease with work still outstanding, so the
 *    thread that receives it reaps a request the thread that gave it up submitted. It trips
 *    `handoff_is_quiesced` at the handoff itself and `one_issuer_at_a_time` at the reap behind it,
 *    which is the same defect seen once as the rule it broke and once as what that cost.
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

// The slot holds no thread's identity while no lease occupies it.
#define unleased 0

#ifdef without_quiesced_handoff
#define adopt_accepts 1
#else
#define adopt_accepts (outstanding == 0)
#endif

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
#ifndef without_generation
            slot_generation++;
#endif
            slot_issuer = unleased
       }

    :: timeout -> break
    od
}
