# Verification

UStore's engines publish shared state through atomics rather than locks, and its storage layer publishes bytes through syscalls whose order is the only thing standing between a crash and a corrupt database.
Both are checked here rather than argued about in review: Spin checks a Promela model of a protocol, and GenMC checks the C++ against the real header.
`check.sh` runs every model and client through ForkUnion's runner, compares each result with the expected one, and exits with the count of mismatches.

- __`durability.pml`__ — the write, flush, record and catalog order, with a crash injected at every step.
- __`nvme_ring.pml`__ — an owner's credit against a bounded completion queue and the overflow list behind it.
- __`nvme_ring_ownership.pml`__ — one issuing thread per ring, and the queue pairs that move between them.
- __`reader_gates.cpp`__ — the two-slot reader gate of `lsm_tree` and the catalog under GenMC, which finds the freed slot once its fences are dropped.
- __`transaction_commit.pml`__ — staging and committing across collections at both isolation levels.
- __`snapshot_lifetime.pml`__ — snapshots created, exported and dropped while writers publish.
- __`engine_ram.cpp`__ — the in-memory engine's commit path under GenMC, against its real header.
- __`nvme_compaction.pml`__ — LSM compaction against concurrent readers, with a crash at every step.

The first four exist; the rest name what belongs here as the engines that need them arrive.

The weak-memory models and the GenMC harness live in [`forkunion/verification`](../forkunion/verification/README.md), and a model whose correctness turns on load and store ordering includes them.
None of the three Promela models written here does, since their ordering is made by syscalls, by the kernel's own queues and by a queue pair changing hands rather than by memory accesses.

## Durability

A sorted run is written, flushed, recorded in the catalog, and only then is the catalog synced, so an extent missing from the catalog is free after a crash.
The model takes `extents` extents through those five stages while a second process may cut power between any two of them, then checks what recovery would find.

- __`catalog_entry_has_durable_bytes`__ — recovery never finds an extent in the catalog whose bytes never reached the medium, which would hand a reader a hole.
- __`acknowledged_extent_survives`__ — recovery never loses an extent the writer acknowledged, so a committed extent is in the catalog and its bytes are durable.

Two knobs break the order on purpose, and `check.sh` expects each of them to fail.
`-Dwithout_flush_before_sync` syncs the catalog before flushing the bytes, and a crash in between records an extent that is not there.
`-Dwithout_catalog_sync` leaves the catalog record in the page cache and acknowledges anyway, and a crash loses an extent the writer promised.

The model describes the design rather than a header it abstracts, since no code in the tree carries this order yet.
It assumes the catalog rename is atomic, that a write is whole blocks or nothing, and that everything not flushed is gone after the crash.
None of those three is checked here, and each is a place where an implementation can be wrong while this model still passes.

## Completion-Queue Overflow

An owner submits against bounded credit, and a completion the visible queue has no room for goes onto a kernel-side overflow list rather than being dropped.
Nothing is lost there, but an overflowed entry stays invisible until the owner enters the kernel again, which is why `poll` tests `IORING_SQ_CQ_OVERFLOW` and enters a second time.
A cancellation is what makes a full queue reachable, since the cancelling entry posts a completion nobody budgeted credit for and which `poll` discards.

- __`every_request_retires`__ — once the owner and the device have gone quiet, every submitted request has yielded exactly one terminal completion to its owner, and the owner submitted all of them.
- __`no_completion_is_lost`__ — the kernel never has to drop a completion for want of a seat, and no completion the owner is owed is left in the overflow list once everything has gone quiet.

Two knobs break the design on purpose, and `check.sh` expects each of them to fail.
`-Dwithout_overflow_check` drains the visible queue and never enters again to flush the overflow list, so the owner quiesces holding credit for a request it will never see retire.
`-Dwithout_credit_bound` submits past the owner's credit, so more completions are outstanding than the queue and the overflow list together were sized for.

The shape is one owner, two requests, one request of credit, one visible seat and one overflow seat.
One request of credit and one visible seat is the smallest shape the cancelling entry can fill the queue in, and two requests is the smallest number an owner can exceed its credit with.
The overflow list is sized at what the credit budget proves is enough, so a completion that fits nowhere means the budget was wrong rather than the kernel stingy.

## Ring Ownership

A ring belongs to exactly one thread: io_uring answers `-EEXIST` to every other, and an SPDK queue pair polled by a second thread corrupts its tracker list and reports nothing at all.
A queue pair may still move between threads, but only through an explicit handoff and only while nothing is outstanding, which is why `basic_disk::adopt` refuses a queue pair with pending or ready work.
A released queue pair is reissued with a raised generation, so a token minted under its predecessor cannot deliver into its successor's queue.

- __`one_issuer_at_a_time`__ — a thread never polls a request another thread submitted.
- __`handoff_is_quiesced`__ — a queue pair never changes hands while it still has work outstanding.
- __`no_stale_delivery`__ — a completion minted under one queue pair is never delivered to the queue pair that later occupies the same slot.

`-Dwithout_generation` reissues the slot without raising the generation, so a token minted under the previous queue pair passes `submit`'s check and delivers into its successor's queue.
`-Dwithout_quiesced_handoff` lets `adopt` take a queue pair with work still outstanding, so the thread that receives it polls a request the thread that gave it up submitted.

The shape is one queue pair slot, two threads, one request of depth, two submissions, one reissue and one handoff attempt.
One slot is enough because the hazard is two threads on the same slot, and one reissue is the smallest number that can make a token stale.
The model keeps a queue pair epoch the design does not, as the oracle the generation counter is checked against, so a knob that stops raising the generation is caught rather than hidden by its own counter.

## Running the Checks

```sh
verification/check.sh
```

It needs `spin` and a C compiler, and it exits 127 with a `skip` line when Spin is absent, so a machine without it reports the gap rather than reporting success.
Each result prints the state count and the depth Spin reached.
Every search runs under an 8 GB cgroup cap where `systemd-run` is reachable; a model that hits the cap is a model to shrink, never a cap to raise.

## When a Check Fails

A `WRONG` line names the model, the defines it was built with, the result expected and the result seen.

- __A `pass` model that failed__ means the design admits the outcome an assertion forbids.
  Read the counterexample: `spin -t -p durability.pml` replays the trail Spin left beside `pan`, one step per line, and the last steps before the assertion are the interleaving to fix.
  Fix the design or the model, and say in the commit which one was wrong.
- __A `fail` knob that passed__ means the model no longer exercises what the knob breaks, usually because a parameter shrank below the shape the bug needs.
  This is the worse failure of the two, since it silently turns the model into a check of nothing.
- __A `broken` result__ is Spin or the compiler refusing the model, and the first twenty lines of the log printed underneath say why.

Never relax an assertion to make a run green.
An assertion that is wrong is a design decision to revisit in its own commit, with the reason in the message.
