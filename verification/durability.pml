/**
 *  The order a sorted run reaches disk in, as the Storage I/O section of `v1.md` fixes it: an
 *  extent is written, flushed, recorded in the catalog, and then the catalog is synced, so an
 *  extent missing from the catalog is free after a crash.
 *
 *  Two volumes publish a catalog two ways, and this models both. A `directory_volume_t` keeps its
 *  catalog in the metadata directory and replaces it by rename, which is atomic on every
 *  filesystem. An `extent_volume_t` has no directory to rename in, so it keeps two slots in the
 *  volume itself: an image is written whole into the older slot, flushed, and from that instant it
 *  is the newer one. A torn image fails the checksum its header carries over the whole image, so a
 *  mount rejects it and reads the other slot, which is what a failed rename left behind. Both are
 *  one atomic publication here, which is what the two properties turn on.
 *
 *  `src/nvme/catalog.hpp` carries the record and slot layout, and `src/nvme/volume.hpp` the order
 *  below; `src/nvme/extent_allocator.hpp` places an extent and holds no state of its own, since the
 *  free map is rebuilt from the catalog at mount.
 *
 *  Shape: one writer takes `extents` extents through five stages each, and a power process may
 *  cut the run at any point. Two extents is enough for a crash to land between one extent's
 *  catalog sync and the next extent's write, which is the only ordering the second extent can
 *  spoil for the first.
 *
 *  Abstracted away: the bytes themselves, since an extent is durable or it is not; the free
 *  range tree, since a crash leaves every extent outside the durable catalog free by
 *  definition; partial writes, since the transport's aligned slab writes whole blocks; and the
 *  atomicity of the catalog replacement, which a rename gives outright and two slots give through
 *  a checksum over the whole image, and which the model takes as given by running the sync in one
 *  step unless a knob breaks it. Everything not durable is discarded by the crash, which the
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
 *  - `-Dwithout_catalog_checksum` publishes a slot whose records were only partly copied, which is
 *    what an `extent_volume_t` does if its header carries no checksum over the image: a crash
 *    mid-image leaves some records durable and the rest not, and recovery reads the fragment as a
 *    whole catalog. Without this knob the model would keep passing for a design that dropped the
 *    checksum, since the atomic block hides the only failure the checksum prevents.
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

// A rename, or a slot swap behind an image checksum, publishes the whole catalog at once.
#if defined(without_catalog_sync)
inline sync_catalog() {
    skip
}
#elif defined(without_catalog_checksum)
inline sync_catalog() {
    // A torn image publishes a prefix: each record lands durable or it does not.
    for (record : 0 .. extents - 1) {
        if
        :: catalog_durable[record] = catalog_cached[record]
        :: skip
        fi
    }
}
#else
inline sync_catalog() {
    atomic {
        for (record : 0 .. extents - 1) {
            catalog_durable[record] = catalog_cached[record]
        }
    }
}
#endif

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
