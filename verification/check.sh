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

section "epoch_claim/committer_and_holder.pml: a committer trying the claim while its holder lets go and looks once"
for memory in sc rc11 far; do verify epoch_claim/committer_and_holder.pml pass -Dmemory=$memory; done
for memory in rc11 far; do verify epoch_claim/committer_and_holder.pml fail -Dmemory=$memory -Dtry_fence=false; done
for memory in rc11 far; do verify epoch_claim/committer_and_holder.pml fail -Dmemory=$memory -Drelease_fence=false; done
verify epoch_claim/committer_and_holder.pml pass -Dmemory=sc -Dtry_fence=false -Drelease_fence=false
verify epoch_claim/committer_and_holder.pml pass -Dmemory=rc11 -Drelease_step=cleared

section "epoch_claim/explicit_verbs.pml: two blocking takers beside a committer trying the claim once"
for memory in sc rc11 far; do verify epoch_claim/explicit_verbs.pml pass -Dmemory=$memory; done
verify epoch_claim/explicit_verbs.pml fail -Dmemory=rc11 -Dtake_order=order_relaxed
verify epoch_claim/explicit_verbs.pml fail -Dmemory=rc11 -Drelease_order=order_relaxed
verify epoch_claim/explicit_verbs.pml stuck -Dmemory=far -Drelease_order=order_relaxed
verify epoch_claim/explicit_verbs.pml stuck -Dmemory=sc -Drecheck=false
for memory in sc rc11; do verify epoch_claim/explicit_verbs.pml pass -Dmemory=$memory -Drelease_step=cleared; done

section "epoch_claim/stalled_committer.pml: a stalled committer against a flusher retaking the claim back to back"
for memory in sc rc11 far; do verify epoch_claim/stalled_committer.pml pass -Dmemory=$memory; done
for memory in sc rc11; do verify epoch_claim/stalled_committer.pml stuck -Dmemory=$memory -Drelease_step=cleared; done
for memory in sc rc11; do verify epoch_claim/stalled_committer.pml stuck -Dmemory=$memory -Dstalled_wait=until_taken; done
verify epoch_claim/stalled_committer.pml stuck -Dmemory=sc -Dstalled_wait=until_taken -Drelease_step=cleared
verify epoch_claim/stalled_committer.pml pass -Dmemory=sc -Drounds=1
verify epoch_claim/stalled_committer.pml stuck -Dmemory=rc11 -Drounds=1
verify epoch_claim/stalled_committer.pml pass -Dmemory=rc11 -Drounds=1 -Dwake_order=order_acquire

section "reader_gates.cpp: a reader counting itself into a slot a publisher retires and frees"
if genmc_ready; then
    verify_client reader_gates.cpp pass
    verify_client reader_gates.cpp fail -Dwithout_fences

    section "epoch_claim/client.cpp: the epoch claim's three scenarios over std::atomic, with its real fences"
    for memory in sc tso rc11 imm; do verify_client epoch_claim/client.cpp pass $memory committer_and_holder; done
    for memory in rc11 imm; do verify_client epoch_claim/client.cpp fail $memory committer_and_holder_try_fence_false; done
    for memory in rc11 imm; do verify_client epoch_claim/client.cpp fail $memory committer_and_holder_release_fence_false; done
    for memory in sc tso; do verify_client epoch_claim/client.cpp pass $memory committer_and_holder_try_fence_false; done
    for memory in sc tso; do verify_client epoch_claim/client.cpp pass $memory committer_and_holder_release_fence_false; done
    for memory in sc tso rc11 imm; do verify_client epoch_claim/client.cpp pass $memory explicit_verbs; done
    for memory in rc11 imm; do verify_client epoch_claim/client.cpp fail $memory explicit_verbs_take_order_relaxed; done
    for memory in rc11 imm; do verify_client epoch_claim/client.cpp fail $memory explicit_verbs_release_order_relaxed; done
    for memory in sc tso rc11 imm; do verify_client epoch_claim/client.cpp pass $memory stalled_committer; done
    for memory in sc rc11 imm; do verify_client epoch_claim/client.cpp stuck $memory stalled_committer_release_step_cleared; done
    for memory in sc rc11 imm; do verify_client epoch_claim/client.cpp stuck $memory stalled_committer_stalled_wait_until_taken; done
    for memory in sc tso; do verify_client epoch_claim/client.cpp pass $memory stalled_committer_rounds_1; done
    for memory in rc11 imm; do verify_client epoch_claim/client.cpp stuck $memory stalled_committer_rounds_1; done
    for memory in rc11 imm; do verify_client epoch_claim/client.cpp pass $memory stalled_committer_rounds_1_wake_order_acquire; done
fi

finish
