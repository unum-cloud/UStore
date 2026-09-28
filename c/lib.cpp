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

#include <limits> // `std::numeric_limits`

#include "engine.hpp"                // `engine_t`, `handle_t`
#include "helpers/linked_memory.hpp" // `clear_linked_memory`

ustore_collection_t const ustore_collection_main_k = 0;
ustore_length_t const ustore_length_missing_k = std::numeric_limits<ustore_length_t>::max();
ustore_key_t const ustore_key_unknown_k = std::numeric_limits<ustore_key_t>::max();

namespace unum::ustore {

/** The engine behind @p handle, as the engine stamped it. */
engine_t const& engine_of(void* handle) noexcept { return *static_cast<handle_t*>(handle)->engine; }

/** The transaction a call carries, or null. */
void* carried(ustore_transaction_t transaction) noexcept { return transaction; }

/** The transaction a call reuses, or null for one it opens afresh. */
void* carried(ustore_transaction_t const* transaction) noexcept { return transaction ? *transaction : nullptr; }

/** Serves @p arguments on the engine of their database. */
template <typename arguments_type_>
void route(arguments_type_* arguments, void (*engine_t::*call)(arguments_type_&) noexcept) noexcept {
    if (!arguments->db) {
        *arguments->error = {ustore_error_uninitialized_k, "The database is uninitialized"};
        return;
    }
    engine_t const& engine = engine_of(arguments->db);
    if constexpr (requires { carried(arguments->transaction); }) {
        void* const transaction = carried(arguments->transaction);
        if (transaction && &engine_of(transaction) != &engine) {
            *arguments->error = {ustore_error_invalid_argument_k, "The transaction belongs to another engine"};
            return;
        }
    }
    (engine.*call)(*arguments);
}

} // namespace unum::ustore

using namespace unum::ustore;

void ustore_database_capabilities(ustore_database_capabilities_t* arguments) {
    route(arguments, &engine_t::database_capabilities);
}
void ustore_database_control(ustore_database_control_t* arguments) { route(arguments, &engine_t::database_control); }
void ustore_snapshot_list(ustore_snapshot_list_t* arguments) { route(arguments, &engine_t::snapshot_list); }
void ustore_snapshot_create(ustore_snapshot_create_t* arguments) { route(arguments, &engine_t::snapshot_create); }
void ustore_snapshot_export(ustore_snapshot_export_t* arguments) { route(arguments, &engine_t::snapshot_export); }
void ustore_snapshot_drop(ustore_snapshot_drop_t* arguments) { route(arguments, &engine_t::snapshot_drop); }
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
void ustore_transaction_init(ustore_transaction_init_t* arguments) { route(arguments, &engine_t::transaction_init); }
void ustore_transaction_stage(ustore_transaction_stage_t* arguments) { route(arguments, &engine_t::transaction_stage); }
void ustore_transaction_commit(ustore_transaction_commit_t* arguments) {
    route(arguments, &engine_t::transaction_commit);
}

void ustore_transaction_free(ustore_transaction_t transaction) {
    if (transaction) engine_of(transaction).transaction_free(transaction);
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
