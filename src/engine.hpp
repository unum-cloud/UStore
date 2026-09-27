/**
 *  @file src/engine.hpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The table of calls every engine fills, which the C ABI in `c/lib.cpp` routes each handle through.
 */

#pragma once

#include <ustore/blobs.h> // `ustore_read_t`, `ustore_write_t`, `ustore_scan_t`, `ustore_sample_t`, `ustore_measure_t`
#include <ustore/db.h>    // `ustore_database_t`, `ustore_transaction_t` and the calls on them

namespace unum::ustore {

/**
 *  @brief One engine's implementation of every call of the C ABI but its own init.
 *
 *  An engine may throw from any call but the two that free, and `c/lib.cpp` turns what it throws into a status.
 */
struct engine_t {
    void (*get_metadata)(ustore_get_metadata_t&);
    void (*database_control)(ustore_database_control_t&);
    void (*snapshot_list)(ustore_snapshot_list_t&);
    void (*snapshot_create)(ustore_snapshot_create_t&);
    void (*snapshot_export)(ustore_snapshot_export_t&);
    void (*snapshot_drop)(ustore_snapshot_drop_t&);
    void (*read)(ustore_read_t&);
    void (*write)(ustore_write_t&);
    void (*scan)(ustore_scan_t&);
    void (*sample)(ustore_sample_t&);
    void (*measure)(ustore_measure_t&);
    void (*collection_create)(ustore_collection_create_t&);
    void (*collection_drop)(ustore_collection_drop_t&);
    void (*collection_list)(ustore_collection_list_t&);
    void (*transaction_init)(ustore_transaction_init_t&);
    void (*transaction_stage)(ustore_transaction_stage_t&);
    void (*transaction_commit)(ustore_transaction_commit_t&);
    void (*transaction_free)(ustore_transaction_t);
    void (*database_free)(ustore_database_t);
};

/** Leads every database and transaction handle an engine hands out, naming the engine that serves it. */
struct handle_t {
    engine_t const* engine = nullptr;
};

/** The engine's own handle behind @p handle, which the engine handed out as the @c handle_t it derives from. */
template <typename handle_type_>
handle_type_* handle_cast(void* handle) noexcept {
    return static_cast<handle_type_*>(static_cast<handle_t*>(handle));
}

} // namespace unum::ustore
