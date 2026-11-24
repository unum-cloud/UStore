# Verification

UStore's engines publish shared state through atomics rather than locks, and its storage layer publishes bytes through syscalls whose order is the only thing standing between a crash and a corrupt database.
Both are checked here rather than argued about in review: Spin checks a Promela model of a protocol, and GenMC checks the C++ against the real header.

- `durability.pml`: the two-flush manifest commit of both volumes, rename and owner block, with a crash injected at every step.
- `nvme_ring.pml`: an owner's credit against a bounded completion queue and the overflow list behind it.
- `nvme_ring_ownership.pml`: one issuing thread per ring, and the queue pairs that move between them.
- `epoch_domain.cpp`: the reader epochs a writer waits out before it frees a replaced view of a collection, under GenMC, which finds the freed view once either fence is dropped.
- `epoch_claim/`: the parking claim a database's maintenance, catalog and blob writes run under, in Spin and GenMC: no maintenance lost, one holder at a time, and no committer starved by a holder retaking it.
- `collection_drop.pml`: the dropping mark a collection drop claims on the readers' count, against snapshot and transaction opens that back out.
- `transaction_commit.pml`: staging and committing across collections at both isolation levels.
- `snapshot_lifetime.pml`: snapshots created, exported and dropped while writers publish.
- `engine_ram.cpp`: the in-memory engine's commit path under GenMC, against its real header.
- `nvme_compaction.pml`: LSM compaction against concurrent readers, with a crash at every step.

The first six exist; the rest name what belongs here as the engines that need them arrive.

## Running

`verification/check.sh` runs every model and client through ForkUnion's runner, from the `forkunion` submodule or from the checkout `FORKUNION_SOURCE` names.
A GenMC on the path, or named by `GENMC`, also runs the clients.
[ForkUnion's verification README](../forkunion/verification/README.md) documents the runner, the `@verify` grammar and the memory budgets.
