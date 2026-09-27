#!/usr/bin/env bash
#
# Model-checks every Promela model and GenMC client here and compares each verdict with the expected
# one: `pass` means no assertion fails, `fail` means the model admits what an assertion forbids, which
# is what the `-Dwithout_*` knobs are for. ForkUnion's `check.sh` supplies the runner, as it does for
# USearch. Needs `spin` and a C compiler; a GenMC on the path, or named by `GENMC`, also runs the clients.

set -u
cd "$(dirname "$0")"
source ../forkunion/verification/check.sh

section "durability.pml: the write, flush, record and catalog order, with a crash at every step"
verify durability.pml pass
verify durability.pml pass -Dextents=3
verify durability.pml fail -Dwithout_flush_before_sync
verify durability.pml fail -Dwithout_catalog_sync
verify durability.pml fail -Dwithout_catalog_checksum

section "nvme_ring.pml: an owner's credit against a bounded completion queue and its overflow list"
verify nvme_ring.pml pass
verify nvme_ring.pml pass -Drequests=3
verify nvme_ring.pml pass -Dcredit=2 -Dqueue_seats=2
verify nvme_ring.pml fail -Dwithout_overflow_check
verify nvme_ring.pml fail -Dwithout_credit_bound

section "nvme_ring_ownership.pml: one issuing thread per ring, and the leases that move between them"
verify nvme_ring_ownership.pml pass
verify nvme_ring_ownership.pml pass -Dsubmits=3
verify nvme_ring_ownership.pml fail -Dwithout_generation
verify nvme_ring_ownership.pml fail -Dwithout_quiesced_handoff

section "reader_gates.cpp: a reader counting itself into a slot a publisher retires and frees"
if genmc_ready; then
    verify_client reader_gates.cpp pass
    verify_client reader_gates.cpp fail -Dwithout_fences
fi

finish
