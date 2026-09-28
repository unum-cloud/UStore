/**
 *  @file c/lib.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The C ABI, routing every call to the engine behind its database or transaction handle.
 *
 *  Each engine defines only its own `ustore_database_init_*`, which stamps every handle it hands out with its
 *  @c engine_t, so one library holds every engine it was built with and a program links only those it opens. The init
 *  of an engine the build lacks is defined here and refuses, so every program links against every build. Every call
 *  of the table is `noexcept`, so nothing here catches.
 */

#include <cstdint> // `std::uint8_t`, `std::uint64_t`

#if defined(_WIN32)
#include <windows.h> // `NTSTATUS`, which <bcrypt.h> needs
#include <bcrypt.h>  // `BCryptGenRandom`
#else
#include <sys/random.h> // `getentropy`
#endif

#include <chrono> // `std::chrono::system_clock`
#include <limits> // `std::numeric_limits`

#include "engine.hpp"                // `engine_t`, `handle_t`
#include "helpers/linked_memory.hpp" // `clear_linked_memory`

ustore_collection_id_t const ustore_collection_main_k = {};
ustore_length_t const ustore_length_missing_k = std::numeric_limits<ustore_length_t>::max();
ustore_key_t const ustore_key_unknown_k = {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}};

namespace unum::ustore {

/** The engine behind @p handle, as the engine stamped it. */
engine_t const& engine_of(void* handle) noexcept { return *handle_cast<handle_t>(handle)->engine; }

/** The transaction a call carries, or null. */
void* carried(ustore_transaction_t transaction) noexcept { return transaction; }

/** The transaction a call reuses, or null for one it opens afresh. */
void* carried(ustore_transaction_t const* transaction) noexcept { return transaction ? *transaction : nullptr; }

/** The snapshot a call reads, or null, which an output for a new snapshot never matches. */
void* viewed(ustore_snapshot_t snapshot) noexcept { return snapshot; }

/** Serves @p arguments on the engine of their database, refusing a transaction or a snapshot of another engine and a
 *  call naming both, as it reads exactly one view. */
template <typename arguments_type_>
void route(arguments_type_* arguments, void (*engine_t::*call)(arguments_type_&) noexcept) noexcept {
    if (!arguments->db) {
        *arguments->error = {ustore_error_uninitialized_k, "The database is uninitialized"};
        return;
    }
    engine_t const& engine = engine_of(arguments->db);
    void* transaction = nullptr;
    if constexpr (requires { carried(arguments->transaction); }) {
        transaction = carried(arguments->transaction);
        if (transaction && &engine_of(transaction) != &engine) {
            *arguments->error = {ustore_error_invalid_argument_k, "The transaction belongs to another engine"};
            return;
        }
    }
    if constexpr (requires { viewed(arguments->snapshot); }) {
        void* const snapshot = viewed(arguments->snapshot);
        if (snapshot && &engine_of(snapshot) != &engine) {
            *arguments->error = {ustore_error_invalid_argument_k, "The snapshot belongs to another engine"};
            return;
        }
        if (snapshot && transaction) {
            *arguments->error = {ustore_error_invalid_argument_k,
                                 "A call reads a snapshot or a transaction, never both"};
            return;
        }
    }
    (engine.*call)(*arguments);
}

bool mint_collection_id(ustore_collection_id_t& id) noexcept {
    std::uint8_t* const bytes = id.uuid.bytes;
#if defined(_WIN32)
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes, sizeof(id.uuid.bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return false;
#else
    if (getentropy(bytes, sizeof(id.uuid.bytes)) != 0) return false;
#endif
    // Version 7: Unix milliseconds big-endian in bytes [0, 6), then the version nibble and the RFC variant over the
    // random bits, so identifiers minted later order after earlier ones
    auto const now = std::chrono::system_clock::now().time_since_epoch();
    auto const milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    for (unsigned index = 0; index != 6; ++index)
        bytes[index] = static_cast<std::uint8_t>(milliseconds >> (40 - 8 * index));
    bytes[6] = static_cast<std::uint8_t>(0x70 | (bytes[6] & 0x0F));
    bytes[8] = static_cast<std::uint8_t>(0x80 | (bytes[8] & 0x3F));
    return true;
}

} // namespace unum::ustore

using namespace unum::ustore;

void ustore_database_capabilities(ustore_database_capabilities_t* arguments) {
    route(arguments, &engine_t::database_capabilities);
}
void ustore_database_control(ustore_database_control_t* arguments) { route(arguments, &engine_t::database_control); }
void ustore_snapshot_init(ustore_snapshot_init_t* arguments) { route(arguments, &engine_t::snapshot_init); }
void ustore_snapshot_export(ustore_snapshot_export_t* arguments) { route(arguments, &engine_t::snapshot_export); }
void ustore_read(ustore_read_t* arguments) { route(arguments, &engine_t::read); }
void ustore_write(ustore_write_t* arguments) { route(arguments, &engine_t::write); }
void ustore_scan(ustore_scan_t* arguments) { route(arguments, &engine_t::scan); }
void ustore_sample(ustore_sample_t* arguments) { route(arguments, &engine_t::sample); }
void ustore_measure(ustore_measure_t* arguments) { route(arguments, &engine_t::measure); }
void ustore_collection_create(ustore_collection_create_t* arguments) { route(arguments, &engine_t::collection_create); }
void ustore_collection_drop(ustore_collection_drop_t* arguments) { route(arguments, &engine_t::collection_drop); }
void ustore_collection_list(ustore_collection_list_t* arguments) { route(arguments, &engine_t::collection_list); }
void ustore_collection_find(ustore_collection_find_t* arguments) { route(arguments, &engine_t::collection_find); }
void ustore_strings_write(ustore_strings_write_t* arguments) { route(arguments, &engine_t::strings_write); }
void ustore_strings_read(ustore_strings_read_t* arguments) { route(arguments, &engine_t::strings_read); }
void ustore_strings_scan(ustore_strings_scan_t* arguments) { route(arguments, &engine_t::strings_scan); }
void ustore_graph_find_edges(ustore_graph_find_edges_t* arguments) { route(arguments, &engine_t::graph_find_edges); }
void ustore_graph_upsert_edges(ustore_graph_upsert_edges_t* arguments) {
    route(arguments, &engine_t::graph_upsert_edges);
}
void ustore_graph_remove_edges(ustore_graph_remove_edges_t* arguments) {
    route(arguments, &engine_t::graph_remove_edges);
}
void ustore_graph_upsert_vertices(ustore_graph_upsert_vertices_t* arguments) {
    route(arguments, &engine_t::graph_upsert_vertices);
}
void ustore_graph_remove_vertices(ustore_graph_remove_vertices_t* arguments) {
    route(arguments, &engine_t::graph_remove_vertices);
}
void ustore_transaction_init(ustore_transaction_init_t* arguments) { route(arguments, &engine_t::transaction_init); }
void ustore_transaction_stage(ustore_transaction_stage_t* arguments) { route(arguments, &engine_t::transaction_stage); }
void ustore_transaction_commit(ustore_transaction_commit_t* arguments) {
    route(arguments, &engine_t::transaction_commit);
}

void ustore_transaction_free(ustore_transaction_t transaction) {
    if (transaction) engine_of(transaction).transaction_free(transaction);
}

void ustore_snapshot_free(ustore_snapshot_t snapshot) {
    if (snapshot) engine_of(snapshot).snapshot_free(snapshot);
}

void ustore_database_free(ustore_database_t database) {
    if (database) engine_of(database).database_free(database);
}

void ustore_arena_free(ustore_arena_t arena) { clear_linked_memory(arena); }

#if !USTORE_WITH_ROCKSDB
void ustore_database_init_rocksdb(ustore_database_init_rocksdb_t* arguments) {
    *arguments->error = {ustore_error_unsupported_k, "This build carries no RocksDB engine"};
}
#endif

#if !USTORE_WITH_LEVELDB
void ustore_database_init_leveldb(ustore_database_init_leveldb_t* arguments) {
    *arguments->error = {ustore_error_unsupported_k, "This build carries no LevelDB engine"};
}
#endif

#if !USTORE_WITH_NVME
void ustore_database_init_nvme(ustore_database_init_nvme_t* arguments) {
    *arguments->error = {ustore_error_unsupported_k, "This build carries no NVMe engine"};
}
#endif
