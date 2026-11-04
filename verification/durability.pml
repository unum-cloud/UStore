/**
 *  @file verification/durability.pml
 *  @author Ash Vardanian
 *  @date September 18, 2026
 *  @brief Spin model of the two-flush manifest commit of both volumes, with a crash at every step.
 *
 *  Every publication writes one new run and a manifest copy naming every run so far, flushes both,
 *  then commits the copy and flushes the commit. A directory volume writes the copy as
 *  <tt><database>.ustore.json.tmp</tt>, flushes it with the run files, renames it over the manifest and
 *  syncs the directory. A slotted volume writes the copy into the idle JSON region of the manifest
 *  slot, flushes every medium, then rewrites the slot's owner block to point at that region and
 *  flushes again. Either commit is one atomic step: a rename, or one logical block. The model
 *  assumes both and checks neither; a crash keeps exactly what was durable.
 *
 *  - @c manifest_names_durable_bytes: the manifest in force is whole, and every run it
 *    names is durable.
 *  - @c acknowledged_run_survives: every acknowledged run is named by the manifest in force.
 *
 *  @verify pass sc
 *  @verify pass sc runs=3
 *  @verify fail sc commit_before_flush=true: a crash after the commit and before the flush
 *      names missing bytes
 *  @verify fail sc multi_block_commit=true: a commit torn across two blocks points at a copy it
 *      does not measure
 */

#ifndef runs
#define runs 2
#endif

#ifndef commit_before_flush
#define commit_before_flush false
#endif

#ifndef multi_block_commit
#define multi_block_commit false
#endif

#define last_stage 5

/** The bytes of a run: cached once written, durable once flushed. */
bit bytes_cached[runs];
bit bytes_durable[runs];

/** The two manifest copies, the temporary file and the manifest or the two JSON regions, by the
 *  runs each names. */
byte copy_cached[2];
byte copy_durable[2];

/** The commit: which copy is in force and how many runs it measures, cached and durable. One
 *  logical block carries both, so they land together unless the commit spans two blocks. */
byte pointer_cached = 0;
byte pointer_durable = 0;
byte length_cached = 0;
byte length_durable = 0;

/** The runs the writer has promised its caller are published. */
bit acknowledged[runs];

bit crashed = 0;
bit writer_finished = 0;
bit power_finished = 0;

inline flush_publication(published, idle) {
    bytes_durable[published] = bytes_cached[published];
    copy_durable[idle] = copy_cached[idle]
}

inline commit(idle, published) {
    pointer_cached = idle;
    length_cached = published + 1
}

inline flush_commit() {
    if
    :: multi_block_commit -> pointer_durable = pointer_cached
    :: else ->
        atomic {
            pointer_durable = pointer_cached;
            length_durable = length_cached
        }
    fi
}

/** The second block of a torn commit lands on a step of its own, which a crash may come before. */
inline flush_second_block() {
    if
    :: multi_block_commit -> length_durable = length_cached
    :: else -> skip
    fi
}

inline publication_step(stage, published, idle) {
    if
    :: stage == 0 -> bytes_cached[published] = 1
    :: stage == 1 -> copy_cached[idle] = published + 1
    :: stage == 2 && !commit_before_flush -> flush_publication(published, idle)
    :: stage == 3 && !commit_before_flush -> commit(idle, published); flush_commit()
    :: stage == 2 && commit_before_flush -> commit(idle, published); flush_commit()
    :: stage == 3 && commit_before_flush -> flush_publication(published, idle)
    :: stage == 4 -> flush_second_block()
    :: stage == last_stage -> acknowledged[published] = 1
    fi
}

// The crash is a process of its own rather than a stage, so the scheduler lands it between
// every pair of steps the writer takes, and the run that never crashes is explored too.
active proctype power() {
    if
    :: crashed = 1
    :: skip
    fi;
    power_finished = 1
}

active proctype writer() {
    byte published = 0;
    byte stage = 0;
    byte idle = 1;
    do
    :: crashed -> break
    :: else ->
        if
        :: published == runs -> break
        :: else ->
            publication_step(stage, published, idle);
            if
            :: stage == last_stage -> stage = 0; published++; idle = 1 - idle
            :: else -> stage++
            fi
        fi
    od;
    writer_finished = 1
}

init {
    byte published;
    byte named;
    bit manifest_names_durable_bytes;
    bit acknowledged_run_survives;
    (writer_finished && power_finished);
    // A mount reads the copy the commit points at, and a copy shorter than the commit measures
    // does not parse.
    if
    :: copy_durable[pointer_durable] == length_durable -> named = length_durable
    :: else -> named = 0
    fi;
    manifest_names_durable_bytes = (copy_durable[pointer_durable] == length_durable);
    assert(manifest_names_durable_bytes);
    for (published : 0 .. runs - 1) {
        manifest_names_durable_bytes = (published >= named || bytes_durable[published]);
        assert(manifest_names_durable_bytes);
        acknowledged_run_survives = (!acknowledged[published] || published < named);
        assert(acknowledged_run_survives)
    }
}
