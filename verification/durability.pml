/**
 *  The order a sorted run reaches disk in, as the Storage I/O section of `v1.md` fixes it: an
 *  extent is written, flushed, recorded in the catalog, and then the catalog is synced, so an
 *  extent missing from the catalog is free after a crash.
 *
 *  No code in the tree carries this order yet - `src/nvme/lsm_sorted_run.hpp` builds a run and
 *  `src/nvme/disk_block_layout.hpp` places it, with no catalog between them - so the model
 *  describes the design rather than a header it abstracts. The catalog is the name, first block
 *  and length record that lives in the metadata directory and is replaced by rename.
 *
 *  Shape: one writer takes `extents` extents through five stages each, and a power process may
 *  cut the run at any point. Two extents is enough for a crash to land between one extent's
 *  catalog sync and the next extent's write, which is the only ordering the second extent can
 *  spoil for the first.
 *
 *  Abstracted away: the bytes themselves, since an extent is durable or it is not; the free
 *  range tree, since a crash leaves every extent outside the durable catalog free by
 *  definition; partial writes, since the transport's aligned slab writes whole blocks; and the
 *  atomicity of the catalog replacement, which the rename gives and the model takes as given by
 *  running the sync in one step. Everything not durable is discarded by the crash, which the
 *  model expresses by never reading a cached flag once the writer has stopped.
 *
 *  Properties, checked over the state a crash leaves behind:
 *  - `catalog_entry_has_durable_bytes`: recovery never finds an extent in the catalog whose
 *    bytes never reached the medium, which would hand a reader a hole.
 *  - `acknowledged_extent_survives`: recovery never loses an extent the writer acknowledged,
 *    which the catalog claims and whose bytes are durable.
 *
 *  Knobs, each of which must fail:
 *  - `-Dwithout_flush_before_sync` syncs the catalog before flushing the bytes, and a crash in
 *    between records an extent that is not there.
 *  - `-Dwithout_catalog_sync` leaves the catalog record in the page cache and acknowledges
 *    anyway, and a crash loses an extent the writer promised.
 */

#ifndef extents
#define extents 2
#endif

#define write_stage 0
#define acknowledge_stage 4

// The bytes of an extent: cached once written, durable once flushed.
bit bytes_cached[extents];
bit bytes_durable[extents];

// The extent's catalog record: cached once written into the replacement catalog, durable once
// that catalog has been synced and renamed into place.
bit catalog_cached[extents];
bit catalog_durable[extents];

// The extent the writer has promised its caller is committed.
bit acknowledged[extents];

bit crashed = 0;
bit writer_finished = 0;
bit power_finished = 0;

// The rename publishes the whole replacement catalog at once, so no crash splits it.
inline sync_catalog() {
#ifndef without_catalog_sync
    atomic {
        for (record : 0 .. extents - 1) {
            catalog_durable[record] = catalog_cached[record]
        }
    }
#else
    skip
#endif
}

inline durable_step(stage, extent) {
    if
    :: stage == write_stage -> bytes_cached[extent] = 1
#ifdef without_flush_before_sync
    :: stage == 1 -> catalog_cached[extent] = 1
    :: stage == 2 -> sync_catalog()
    :: stage == 3 -> bytes_durable[extent] = bytes_cached[extent]
#else
    :: stage == 1 -> bytes_durable[extent] = bytes_cached[extent]
    :: stage == 2 -> catalog_cached[extent] = 1
    :: stage == 3 -> sync_catalog()
#endif
    :: stage == acknowledge_stage -> acknowledged[extent] = 1
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
    byte record;
    byte extent = 0;
    byte stage = write_stage;
    do
    :: crashed -> break
    :: else ->
        if
        :: extent == extents -> break
        :: else ->
            durable_step(stage, extent);
            if
            :: stage == acknowledge_stage -> stage = write_stage; extent++
            :: else -> stage++
            fi
        fi
    od;
    writer_finished = 1
}

init {
    byte extent;
    bit catalog_entry_has_durable_bytes;
    bit acknowledged_extent_survives;
    (writer_finished && power_finished);
    for (extent : 0 .. extents - 1) {
        catalog_entry_has_durable_bytes = (!catalog_durable[extent] || bytes_durable[extent]);
        assert(catalog_entry_has_durable_bytes);
        acknowledged_extent_survives =
            (!acknowledged[extent] || (catalog_durable[extent] && bytes_durable[extent]));
        assert(acknowledged_extent_survives)
    }
}
