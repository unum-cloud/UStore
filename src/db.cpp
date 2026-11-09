/**
 *  @file src/db.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief Implements <tt>c/ustore/db.h</tt>: databases, snapshots, transactions and collections, the views they read
 *      through, the spelling of their closed sets, and the refusing opens of the engines a build lacks.
 */

#include <cstddef> // size_t, which Emscripten's <sys/random.h> needs
#include <cstdint> // `std::uint8_t`, `std::uint64_t`

#include "ustore/types.h" // `USTORE_OS_WINDOWS_`

#if USTORE_OS_WINDOWS_
#include <windows.h> // NTSTATUS, which <bcrypt.h> needs
#include <bcrypt.h>  // `BCryptGenRandom`
#else
#include <sys/random.h> // `getentropy`
#endif

#include <chrono> // `std::chrono::system_clock`
#include <new>    // `std::nothrow`

#include "ustore/db.h" // `ustore_database_describe_t`, `ustore_transaction_init_t`, `ustore_ingest_init_t`

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

/** The canonical spelling of every change kind. */
constexpr enumerator_spelling<ustore_change_kind_t> change_kind_names_k[] {
    {ustore_change_write_k, "write"},
    {ustore_change_removal_k, "removal"},
    {ustore_change_change_k, "change"},
};

/** The canonical spelling of every scan direction. */
constexpr enumerator_spelling<ustore_scan_direction_t> scan_direction_names_k[] {
    {ustore_scan_ascending_k, "ascending"},
    {ustore_scan_descending_k, "descending"},
};

/** The canonical spelling of every ingest order. */
constexpr enumerator_spelling<ustore_ingest_order_t> ingest_order_names_k[] {
    {ustore_ingest_sorted_k, "sorted"},
    {ustore_ingest_unsorted_k, "unsorted"},
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
#if USTORE_OS_WINDOWS_
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

status_t open_changes_cursor(engine_t const& engine, ustore_changes_cursor_init_t const& arguments,
                             view_t const& view) noexcept {
    if (!engine.database->changes_cursor_init)
        return {status_code_t::unsupported_k, "The engine keeps no stamps to read changes by"};
    auto* const cursor = new (std::nothrow) ustore_changes_cursor_handle_t {};
    if (!cursor) return {status_code_t::out_of_memory_k, "Failed to allocate the change feed"};
    cursor->cursor.database = arguments.database;
    cursor->cursor.collection = arguments.collection;
    if (status_t const opened = engine.database->changes_cursor_init(arguments, cursor, view); opened.failed()) {
        delete cursor;
        return opened;
    }
    take_hold(arguments.database);
    *arguments.cursor = cursor;
    return {};
}

status_t pull(engine_t const& engine, ustore_changes_cursor_next_t const& arguments, ustore_changes_cursor_handle_t&,
              view_t const& view) noexcept {
    if (!arguments.count || !arguments.keys || !arguments.stamps || !arguments.kinds)
        return {status_code_t::invalid_argument_k, "Need outputs for the count, the keys, the stamps and the kinds"};
    return engine.database->changes_cursor_next(arguments, view);
}

} // namespace unum::ustore

status_t open_ingest(ustore_ingest_init_t const& arguments) noexcept {
    if (!arguments.ingest) return {status_code_t::invalid_argument_k, "Need an output for the ingest"};
    *arguments.ingest = nullptr;
    ustore_database_t const database = arguments.database;
    if (!database) return {status_code_t::uninitialized_k, "The database is uninitialized"};
    if (arguments.order != ustore_ingest_sorted_k && arguments.order != ustore_ingest_unsorted_k)
        return {status_code_t::invalid_argument_k, "Unknown ingest order"};
    if (database->access == access_t::read_only_k)
        return {status_code_t::read_only_k, "The database was opened read-only"};
    engine_t const& engine = engine_of(database);
    if (!engine.database->ingest_init) return {status_code_t::unsupported_k, "The engine ingests nothing"};
    auto* const ingest = new (std::nothrow) ustore_ingest_handle_t {.engine = &engine, .database = database};
    if (!ingest) return {status_code_t::out_of_memory_k, "Failed to allocate the ingest"};
    *arguments.ingest = ingest;
    if (status_t const opened = engine.database->ingest_init(arguments, view_t {.scratch = &ingest->scratch});
        opened.failed()) {
        *arguments.ingest = nullptr;
        arena_free_blocks(ingest->scratch);
        delete ingest;
        return opened;
    }
    take_hold(database);
    return {};
}

status_t append_ingest(ustore_ingest_append_t const& arguments) noexcept {
    ustore_ingest_t const ingest = arguments.ingest;
    if (!ingest || !ingest->state) return {status_code_t::invalid_argument_k, "The ingest is missing or finished"};
    arena_rewind(&ingest->scratch);
    strings_arg_t values;
    if (status_t const gathered = gather_strings({arguments.values, arguments.count}, &ingest->scratch, values);
        gathered.failed())
        return gathered;
    return ingest->engine->database->ingest_append(arguments, values, view_t {.scratch = &ingest->scratch});
}

status_t finish_ingest(ustore_ingest_finish_t const& arguments) noexcept {
    if (arguments.commit_stamp) *arguments.commit_stamp = 0;
    ustore_ingest_t const ingest = arguments.ingest;
    if (!ingest || !ingest->state) return {status_code_t::invalid_argument_k, "The ingest is missing or finished"};
    status_t landed {status_code_t::invalid_argument_k, "Unknown durability"};
    arena_rewind(&ingest->scratch);
    if (arguments.durability == ustore_durability_buffered_k || arguments.durability == ustore_durability_flushed_k)
        landed = ingest->engine->database->ingest_finish(arguments, view_t {.scratch = &ingest->scratch});
    ingest->engine->database->ingest_free(ingest);
    ingest->state = nullptr;
    return landed;
}

void ustore_database_describe(ustore_database_describe_t* arguments) { route(arguments); }
void ustore_database_clear(ustore_database_clear_t* arguments) { route(arguments); }
void ustore_database_compact(ustore_database_compact_t* arguments) { route(arguments); }
void ustore_database_flush(ustore_database_flush_t* arguments) { route(arguments); }
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
void ustore_ingest_init(ustore_ingest_init_t* arguments) { finish(*arguments->outcome, open_ingest(*arguments)); }
void ustore_ingest_append(ustore_ingest_append_t* arguments) { finish(*arguments->outcome, append_ingest(*arguments)); }
void ustore_ingest_finish(ustore_ingest_finish_t* arguments) { finish(*arguments->outcome, finish_ingest(*arguments)); }

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
char const* ustore_scan_direction_name(ustore_scan_direction_t scan_direction) {
    return name_in(scan_direction_names_k, scan_direction);
}
ustore_scan_direction_t ustore_scan_direction_from_name(ustore_bytes_view_t name) {
    return value_in(scan_direction_names_k, name);
}
char const* ustore_change_kind_name(ustore_change_kind_t change_kind) {
    return name_in(change_kind_names_k, change_kind);
}
ustore_change_kind_t ustore_change_kind_from_name(ustore_bytes_view_t name) {
    return value_in(change_kind_names_k, name);
}
char const* ustore_drop_mode_name(ustore_drop_mode_t drop_mode) { return name_in(drop_mode_names_k, drop_mode); }
ustore_drop_mode_t ustore_drop_mode_from_name(ustore_bytes_view_t name) { return value_in(drop_mode_names_k, name); }
char const* ustore_isolation_name(ustore_isolation_t isolation) { return name_in(isolation_names_k, isolation); }
ustore_isolation_t ustore_isolation_from_name(ustore_bytes_view_t name) { return value_in(isolation_names_k, name); }
char const* ustore_engine_name(ustore_engine_t engine) { return name_in(engine_names_k, engine); }
ustore_engine_t ustore_engine_from_name(ustore_bytes_view_t name) { return value_in(engine_names_k, name); }
char const* ustore_modality_name(ustore_modality_t modality) { return name_in(modality_names_k, modality); }
ustore_modality_t ustore_modality_from_name(ustore_bytes_view_t name) { return value_in(modality_names_k, name); }
char const* ustore_ingest_order_name(ustore_ingest_order_t order) { return name_in(ingest_order_names_k, order); }

ustore_ingest_order_t ustore_ingest_order_from_name(ustore_bytes_view_t name) {
    return value_in(ingest_order_names_k, name);
}

void ustore_transaction_free(ustore_transaction_t transaction) {
    if (!transaction) return;
    wait_named_calls(transaction->named_calls);
    release_parent(transaction);
}

void ustore_ingest_free(ustore_ingest_t ingest) {
    if (!ingest) return;
    if (ingest->state) ingest->engine->database->ingest_free(ingest);
    ustore_database_t const database = ingest->database;
    arena_free_blocks(ingest->scratch);
    delete ingest;
    release_hold(database);
}

void ustore_changes_cursor_init(ustore_changes_cursor_init_t* arguments) { route(arguments); }
void ustore_changes_cursor_next(ustore_changes_cursor_next_t* arguments) { route(arguments); }

void ustore_changes_cursor_free(ustore_changes_cursor_t cursor) {
    if (!cursor) return;
    wait_named_calls(cursor->cursor.named_calls);
    ustore_database_t const database = cursor->cursor.database;
    if (cursor->state) engine_of(database).database->changes_cursor_free(cursor);
    delete cursor;
    release_hold(database);
}

void ustore_snapshot_free(ustore_snapshot_t snapshot) {
    if (!snapshot) return;
    wait_named_calls(snapshot->named_calls);
    release_parent(snapshot);
}

void ustore_database_free(ustore_database_t database) {
    if (!database) return;
    wait_named_calls(database->named_calls);
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
