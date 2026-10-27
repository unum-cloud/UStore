/**
 *  @file verification/durability.pml
 *  @author Ash Vardanian
 *  @date September 18, 2026
 *  @brief Spin model of the write, flush, record and catalog order, with a crash at every step.
 *
 *  An extent is written, flushed, recorded in the catalog, and the catalog synced, so an extent
 *  missing from the catalog is free after a crash. Both volumes publish the catalog atomically:
 *  `directory_volume_t` by rename, `extent_volume_t` by a checksummed slot swap. One writer takes
 *  `extents` extents through five stages while a power process may cut the run at any step.
 *  It assumes the catalog rename is atomic and that a write is whole blocks, and checks neither.
 *
 *  - `catalog_entry_has_durable_bytes`: no catalog entry points at bytes that never landed.
 *  - `acknowledged_extent_survives`: no acknowledged extent is lost.
 *
 *  @verify pass sc
 *  @verify pass sc extents=3
 *  @verify fail sc flush_before_sync=false: a crash between sync and flush records missing bytes
 *  @verify fail sc catalog_sync=false: an acknowledged extent stays in the page cache and is lost
 *  @verify fail sc catalog_checksum=false: a torn image reads as a whole catalog
 */

#ifndef extents
#define extents 2
#endif

#ifndef flush_before_sync
#define flush_before_sync true
#endif

#ifndef catalog_sync
#define catalog_sync true
#endif

#ifndef catalog_checksum
#define catalog_checksum true
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
inline sync_catalog() {
    if
    :: !catalog_sync -> skip
    :: catalog_sync && !catalog_checksum ->
        // A torn image publishes a prefix: each record lands durable or it does not.
        for (record : 0 .. extents - 1) {
            if
            :: catalog_durable[record] = catalog_cached[record]
            :: skip
            fi
        }
    :: atomic {
        catalog_sync && catalog_checksum ->
            for (record : 0 .. extents - 1) {
                catalog_durable[record] = catalog_cached[record]
            }
       }
    fi
}

inline durable_step(stage, extent) {
    if
    :: stage == write_stage -> bytes_cached[extent] = 1
    :: stage == 1 && flush_before_sync -> bytes_durable[extent] = bytes_cached[extent]
    :: stage == 2 && flush_before_sync -> catalog_cached[extent] = 1
    :: stage == 3 && flush_before_sync -> sync_catalog()
    :: stage == 1 && !flush_before_sync -> catalog_cached[extent] = 1
    :: stage == 2 && !flush_before_sync -> sync_catalog()
    :: stage == 3 && !flush_before_sync -> bytes_durable[extent] = bytes_cached[extent]
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
