# Verification

UStore's engines publish shared state through atomics rather than locks, and its storage layer publishes bytes through syscalls whose order is the only thing standing between a crash and a corrupt database.
Both are checked here rather than argued about in review: Spin checks a Promela model of a protocol, and GenMC checks the C++ against the real header.
`check.sh` runs every model, compares each verdict with the expected one, and stops at the first mismatch.

- __`durability.pml`__ — the write, flush, record and catalog order, with a crash injected at every step. Written.
- __`transaction_commit.pml`__ — staging and committing across collections at both isolation levels.
- __`snapshot_lifetime.pml`__ — snapshots created, exported and dropped while writers publish.
- __`engine_ram.cpp`__ — the in-memory engine's commit path under GenMC, against its real header.
- __`nvme_compaction.pml`__ — LSM compaction against concurrent readers, with a crash at every step.

Only the first exists; the rest name what belongs here as the engines that need them arrive.

The weak-memory models and the GenMC harness live in [`forkunion/verification`](../forkunion/verification/README.md), and a model whose correctness turns on load and store ordering includes them.
`durability.pml` does not, since its ordering is made by syscalls rather than by memory accesses.

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

## Running the Checks

```sh
verification/check.sh
```

It needs `spin` and a C compiler, and it exits 127 with a `skip` line when Spin is absent, so a machine without it reports the gap rather than reporting success.
Each verdict prints the state count and the depth Spin reached.
Every search runs under an 8 GiB cgroup cap where `systemd-run` is reachable; a model that hits the cap is a model to shrink, never a cap to raise.

## When a Check Fails

A `WRONG` line names the model, the defines it was built with, the verdict expected and the verdict seen.

- __A `pass` model that failed__ means the design admits the outcome an assertion forbids.
  Read the counterexample: `spin -t -p durability.pml` replays the trail Spin left beside `pan`, one step per line, and the last steps before the assertion are the interleaving to fix.
  Fix the design or the model, and say in the commit which one was wrong.
- __A `fail` knob that passed__ means the model no longer exercises what the knob breaks, usually because a parameter shrank below the shape the bug needs.
  This is the worse failure of the two, since it silently turns the model into a check of nothing.
- __A `broken` verdict__ is Spin or the compiler refusing the model, and the first twenty lines of the log printed underneath say why.

Never relax an assertion to make a run green.
An assertion that is wrong is a design decision to revisit in its own commit, with the reason in the message.
