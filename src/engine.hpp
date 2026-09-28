/**
 *  @file src/engine.hpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The table of calls every engine fills, which the C ABI in `c/lib.cpp` routes each handle through.
 */

#pragma once

#include <new> // `std::bad_alloc`

#include <ustore/blobs.h> // `ustore_read_t`, `ustore_write_t`, `ustore_scan_t`, `ustore_sample_t`, `ustore_measure_t`
#include <ustore/db.h>    // `ustore_database_t`, `ustore_transaction_t` and the calls on them

namespace unum::ustore {

/**
 *  @brief One engine's implementation of every call of the C ABI but its own init.
 *
 *  Every call is `noexcept`: the in-house engines never throw, and an oracle wraps each call of its library in
 *  @c guarded.
 */
struct engine_t {
    void (*get_metadata)(ustore_get_metadata_t&) noexcept;
    void (*database_control)(ustore_database_control_t&) noexcept;
    void (*snapshot_list)(ustore_snapshot_list_t&) noexcept;
    void (*snapshot_create)(ustore_snapshot_create_t&) noexcept;
    void (*snapshot_export)(ustore_snapshot_export_t&) noexcept;
    void (*snapshot_drop)(ustore_snapshot_drop_t&) noexcept;
    void (*read)(ustore_read_t&) noexcept;
    void (*write)(ustore_write_t&) noexcept;
    void (*scan)(ustore_scan_t&) noexcept;
    void (*sample)(ustore_sample_t&) noexcept;
    void (*measure)(ustore_measure_t&) noexcept;
    void (*collection_create)(ustore_collection_create_t&) noexcept;
    void (*collection_drop)(ustore_collection_drop_t&) noexcept;
    void (*collection_list)(ustore_collection_list_t&) noexcept;
    void (*transaction_init)(ustore_transaction_init_t&) noexcept;
    void (*transaction_stage)(ustore_transaction_stage_t&) noexcept;
    void (*transaction_commit)(ustore_transaction_commit_t&) noexcept;
    void (*transaction_free)(ustore_transaction_t) noexcept;
    void (*database_free)(ustore_database_t) noexcept;
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

/** Serves @p call_ of an oracle, whose library may throw, turning whatever it throws into a status. */
template <typename arguments_type_, void (*call_)(arguments_type_&)>
void guarded(arguments_type_& arguments) noexcept {
    try {
        call_(arguments);
    }
    catch (std::bad_alloc const&) {
        *arguments.error = {ustore_error_out_of_memory_k, "Out of memory"};
    }
    catch (...) {
        *arguments.error = {ustore_error_internal_k, "Unexpected engine failure"};
    }
}

} // namespace unum::ustore
