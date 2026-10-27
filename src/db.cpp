/**
 *  @file src/db.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief Implements `c/ustore/db.h`: databases, snapshots, transactions and collections, the views they read
 *      through, the spelling of their closed sets, and the refusing opens of the engines a build lacks.
 */

#include <cstdint> // `std::uint8_t`, `std::uint64_t`

#if defined(_WIN32)
#include <windows.h> // `NTSTATUS`, which <bcrypt.h> needs
#include <bcrypt.h>  // `BCryptGenRandom`
#else
#include <sys/random.h> // `getentropy`
#endif

#include <chrono> // `std::chrono::system_clock`

#include "ustore/db.h" // `ustore_database_describe_t`, `ustore_transaction_init_t`, `ustore_collection_create_t`

#include "lib.hpp" // `route`, `tagged_view`, `release_parent`, `enumerator_spelling`

namespace unum::ustore {

/** The canonical spelling of every durability. */
constexpr enumerator_spelling<ustore_durability_t> durability_names_k[] {
    {ustore_durability_buffered_k, "buffered"},
    {ustore_durability_flushed_k, "flushed"},
};

/** The canonical spelling of every watch. */
constexpr enumerator_spelling<ustore_watch_t> watch_names_k[] {
    {ustore_watch_keys_k, "keys"},
    {ustore_watch_none_k, "none"},
};

/** The canonical spelling of every scan mode. */
constexpr enumerator_spelling<ustore_scan_mode_t> scan_mode_names_k[] {
    {ustore_scan_ordered_k, "ordered"},
    {ustore_scan_unordered_k, "unordered"},
};

/** The canonical spelling of every drop mode. */
constexpr enumerator_spelling<ustore_drop_mode_t> drop_mode_names_k[] {
    {ustore_drop_collection_k, "collection"},
    {ustore_drop_pairs_k, "pairs"},
    {ustore_drop_values_k, "values"},
};

/** The canonical spelling of every isolation. */
constexpr enumerator_spelling<ustore_isolation_t> isolation_names_k[] {
    {ustore_isolation_strict_serializable_k, "strict-serializable"},
    {ustore_isolation_snapshot_k, "snapshot"},
};

/** The canonical spelling of every engine. */
constexpr enumerator_spelling<ustore_engine_t> engine_names_k[] {
    {ustore_engine_ram_k, "ram"},
    {ustore_engine_rocksdb_k, "rocksdb"},
    {ustore_engine_leveldb_k, "leveldb"},
    {ustore_engine_nvme_k, "nvme"},
};

/** The canonical spelling of every modality. */
constexpr enumerator_spelling<ustore_modality_t> modality_names_k[] {
    {ustore_modality_blobs_k, "blobs"},     {ustore_modality_docs_k, "docs"},   {ustore_modality_graph_k, "graph"},
    {ustore_modality_strings_k, "strings"}, {ustore_modality_pages_k, "pages"},
};

expected<ustore_collection_id_t> mint_collection_id() noexcept {
    ustore_collection_id_t id {};
    std::uint8_t* const bytes = id.uuid.bytes;
#if defined(_WIN32)
    bool const drawn = BCRYPT_SUCCESS(
        BCryptGenRandom(nullptr, bytes, sizeof(id.uuid.bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#else
    bool const drawn = getentropy(bytes, sizeof(id.uuid.bytes)) == 0;
#endif
    if (!drawn) return status_t {status_code_t::internal_k, "The system gave no entropy for a collection identifier"};
    // Version 7: Unix milliseconds big-endian in bytes [0, 6), then the version nibble and the RFC variant over the
    // random bits, so identifiers minted later order after earlier ones
    auto const now = std::chrono::system_clock::now().time_since_epoch();
    auto const milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    for (unsigned index = 0; index != 6; ++index)
        bytes[index] = static_cast<std::uint8_t>(milliseconds >> (40 - 8 * index));
    bytes[6] = static_cast<std::uint8_t>(0x70 | (bytes[6] & 0x0F));
    bytes[8] = static_cast<std::uint8_t>(0x80 | (bytes[8] & 0x3F));
    return id;
}

} // namespace unum::ustore

void ustore_database_describe(ustore_database_describe_t* arguments) { route(arguments); }
void ustore_database_clear(ustore_database_clear_t* arguments) { route(arguments); }
void ustore_database_compact(ustore_database_compact_t* arguments) { route(arguments); }
void ustore_snapshot_init(ustore_snapshot_init_t* arguments) { route(arguments); }
void ustore_snapshot_save(ustore_snapshot_save_t* arguments) { route(arguments); }
void ustore_collection_list(ustore_collection_list_t* arguments) { route(arguments); }
void ustore_collection_create(ustore_collection_create_t* arguments) { route(arguments); }
void ustore_collection_find(ustore_collection_find_t* arguments) { route(arguments); }
void ustore_collection_drop(ustore_collection_drop_t* arguments) { route(arguments); }
void ustore_transaction_init(ustore_transaction_init_t* arguments) { route(arguments); }
void ustore_transaction_stage(ustore_transaction_stage_t* arguments) { route(arguments); }
void ustore_transaction_commit(ustore_transaction_commit_t* arguments) { route(arguments); }
void ustore_transaction_rollback(ustore_transaction_rollback_t* arguments) { route(arguments); }

ustore_view_t ustore_transaction_view(ustore_transaction_t transaction, ustore_watch_t watch) {
    return tagged_view(transaction, watch == ustore_watch_none_k ? view_tag_t::blind_k : view_tag_t::watching_k);
}

ustore_view_t ustore_snapshot_view(ustore_snapshot_t snapshot) { return tagged_view(snapshot, view_tag_t::snapshot_k); }

char const* ustore_durability_name(ustore_durability_t durability) { return name_in(durability_names_k, durability); }
ustore_durability_t ustore_durability_from_name(ustore_bytes_view_t name) { return value_in(durability_names_k, name); }
char const* ustore_watch_name(ustore_watch_t watch) { return name_in(watch_names_k, watch); }
ustore_watch_t ustore_watch_from_name(ustore_bytes_view_t name) { return value_in(watch_names_k, name); }
char const* ustore_scan_mode_name(ustore_scan_mode_t scan_mode) { return name_in(scan_mode_names_k, scan_mode); }
ustore_scan_mode_t ustore_scan_mode_from_name(ustore_bytes_view_t name) { return value_in(scan_mode_names_k, name); }
char const* ustore_drop_mode_name(ustore_drop_mode_t drop_mode) { return name_in(drop_mode_names_k, drop_mode); }
ustore_drop_mode_t ustore_drop_mode_from_name(ustore_bytes_view_t name) { return value_in(drop_mode_names_k, name); }
char const* ustore_isolation_name(ustore_isolation_t isolation) { return name_in(isolation_names_k, isolation); }
ustore_isolation_t ustore_isolation_from_name(ustore_bytes_view_t name) { return value_in(isolation_names_k, name); }
char const* ustore_engine_name(ustore_engine_t engine) { return name_in(engine_names_k, engine); }
ustore_engine_t ustore_engine_from_name(ustore_bytes_view_t name) { return value_in(engine_names_k, name); }
char const* ustore_modality_name(ustore_modality_t modality) { return name_in(modality_names_k, modality); }
ustore_modality_t ustore_modality_from_name(ustore_bytes_view_t name) { return value_in(modality_names_k, name); }

void ustore_transaction_free(ustore_transaction_t transaction) {
    if (!transaction) return;
    wait_deferred_calls(transaction->deferred_calls);
    release_parent(transaction);
}

void ustore_snapshot_free(ustore_snapshot_t snapshot) {
    if (!snapshot) return;
    wait_deferred_calls(snapshot->deferred_calls);
    release_parent(snapshot);
}

void ustore_database_free(ustore_database_t database) {
    if (!database) return;
    wait_deferred_calls(database->deferred_calls);
    release_hold(database);
}

#if !USTORE_WITH_ROCKSDB
void ustore_database_open_rocksdb(ustore_database_open_rocksdb_t* arguments) {
    finish(*arguments->outcome, {status_code_t::unsupported_k, "This build carries no RocksDB engine"});
}

void ustore_database_view_rocksdb(ustore_database_view_rocksdb_t* arguments) {
    finish(*arguments->outcome, {status_code_t::unsupported_k, "This build carries no RocksDB engine"});
}
#endif

#if !USTORE_WITH_LEVELDB
void ustore_database_open_leveldb(ustore_database_open_leveldb_t* arguments) {
    finish(*arguments->outcome, {status_code_t::unsupported_k, "This build carries no LevelDB engine"});
}
#endif

#if !USTORE_WITH_NVME
void ustore_database_open_nvme(ustore_database_open_nvme_t* arguments) {
    finish(*arguments->outcome, {status_code_t::unsupported_k, "This build carries no NVMe engine"});
}
#endif
