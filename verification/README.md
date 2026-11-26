# Verification

UStore's engines publish shared state through atomics rather than locks, and its storage layer publishes bytes through syscalls whose order is the only thing standing between a crash and a corrupt database.
Both are checked here rather than argued about in review: Spin checks a Promela model of a protocol, and GenMC checks the C++ against the real header.

- `durability.pml`: the two-flush manifest commit of both volumes, rename and owner block, with a crash injected at every step.
- `nvme_ring.pml`: an owner's credit against a bounded completion queue and the overflow list behind it.
- `nvme_ring_ownership.pml`: one issuing thread per ring, and the queue pairs that move between them.
- `epoch_domain.cpp`: the reader epochs a writer waits out before it frees a replaced view of a collection, under GenMC, which finds the freed view once either fence is dropped.
- `epoch_claim/`: the parking claim a database's maintenance, catalog and blob writes run under, in Spin and GenMC: no maintenance lost, one holder at a time, and no committer starved by a holder retaking it.
- `collection_drop.pml`: the dropping mark a collection drop claims on the readers' count, against snapshot and transaction opens that back out.
- `arena_lifetime.pml`: an arena's calls, generation and exports, its free and its executor's free, the same file in every engine.
- `arena_lifetime.cpp`: the same arena's words under GenMC, with the engine's orders, where an unordered output or freed word is a data race.
- `transaction_commit.pml`: staging and committing across collections at both isolation levels.
- `snapshot_lifetime.pml`: snapshots created, exported and dropped while writers publish.
- `engine_ram.cpp`: the in-memory engine's commit path under GenMC, against its real header.
- `nvme_compaction.pml`: LSM compaction against concurrent readers, with a crash at every step.

The first eight exist; the rest name what belongs here as the engines that need them arrive.

## Running

`verification/check.sh` runs every model and client through ForkUnion's runner, from the `forkunion` submodule or from the checkout `FORKUNION_SOURCE` names.
A GenMC on the path, or named by `GENMC`, also runs the clients.
[ForkUnion's verification README](../forkunion/verification/README.md) documents the runner, the `@verify` grammar and the memory budgets.

## What the Models Found

- An arena's free waits out the calls naming it before it reads the export count, or an export opened during the wait is open when the arena is deleted, and it counts the arena off its executor last, after the memory went back through it: [`arena_lifetime.pml`](arena_lifetime.pml).
- A call advances its arena's generation once, after the export count and the executor's claim admitted it and never on its body's re-hold, and a binding copies an output only under an export, re-reading the generation inside it, or a copy reads the next call's outputs as current: [`arena_lifetime.pml`](arena_lifetime.pml).
- A call takes the arena and an export raises the count each with a sequentially consistent read-modify-write before reading the other's word, or both read the other's from before and the call rewinds outputs the export holds: [`arena_lifetime.pml`](arena_lifetime.pml), [`arena_lifetime.cpp`](arena_lifetime.cpp).
- A call the pool refuses after its claims has advanced the generation without rewinding, which reads earlier results as stale and never as current: [`arena_lifetime.pml`](arena_lifetime.pml).
