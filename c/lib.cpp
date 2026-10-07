/**
 *  @file c/lib.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The C ABI, routing every call to the engine behind its database or transaction handle.
 *
 *  Each engine defines only its own `ustore_database_init_*`, which stamps every handle it hands out with its
 *  @c engine_t, so one library holds every engine it was built with and a program links only those it opens. The init
 *  of an engine the build lacks is defined here and refuses, so every program links against every build. Every call
 *  of the table is `noexcept`, so nothing here catches. The router decodes the view of a call once, refuses every
 *  executor, as calls run on the calling thread, rewinds the arena of the call, and publishes the status the engine
 *  answers only after it returns.
 */

#include <cstdint> // `std::uint8_t`, `std::uint64_t`, `std::uintptr_t`

#if defined(_WIN32)
#include <windows.h> // `NTSTATUS`, which <bcrypt.h> needs
#include <bcrypt.h>  // `BCryptGenRandom`
#else
#include <sys/random.h> // `getentropy`
#endif

#include <array>       // `std::array`
#include <atomic>      // `std::atomic_ref`, `std::memory_order_release`
#include <chrono>      // `std::chrono::system_clock`
#include <limits>      // `std::numeric_limits`
#include <string_view> // `std::string_view`

#include "engine.hpp"                // `engine_t`, `handle_t`, `view_t`
#include "helpers/linked_memory.hpp" // `arena_rewind`, `arena_free`

ustore_collection_id_t const ustore_collection_main_k = {};
ustore_length_t const ustore_length_missing_k = std::numeric_limits<ustore_length_t>::max();
ustore_key_t const ustore_key_unknown_k = {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}};

namespace unum::ustore {

/** The canonical spelling of every status, indexed by its value. */
constexpr std::array<std::string_view, ustore_status_cancelled_k + 1> status_names_k {
    "success",
    "out-of-memory",
    "invalid-argument",
    "uninitialized",
    "unsupported",
    "io",
    "malformed",
    "not-found",
    "already-exists",
    "write-conflict",
    "read-conflict",
    "phantom-conflict",
    "capacity-exhausted",
    "device",
    "internal",
    "pending",
    "contended",
    "cancelled",
};

/** The tag `ustore_transaction_view` and `ustore_snapshot_view` set in the low bits of a handle. */
enum class view_tag_t : std::uintptr_t {
    head_k = 0,
    watching_k = 1,
    blind_k = 2,
    snapshot_k = 3,
};

/** The low bits of a handle a view tag takes, which every handle's alignment leaves free. */
constexpr std::uintptr_t view_tag_mask_k = 3;
static_assert(alignof(handle_t) > view_tag_mask_k);

/** The view naming @p handle with @p tag, or the head for a null @p handle. */
ustore_view_t tagged_view(void* handle, view_tag_t tag) noexcept {
    if (!handle) return nullptr;
    return reinterpret_cast<ustore_view_t>(reinterpret_cast<std::uintptr_t>(handle) | static_cast<std::uintptr_t>(tag));
}

/** Decodes @p view into @p decoded, answering false for a pointer neither view constructor made. */
bool decode_view(ustore_view_t view, view_t& decoded) noexcept {
    std::uintptr_t const bits = reinterpret_cast<std::uintptr_t>(view);
    void* const handle = reinterpret_cast<void*>(bits & ~view_tag_mask_k);
    switch (static_cast<view_tag_t>(bits & view_tag_mask_k)) {
    case view_tag_t::head_k: return handle == nullptr;
    case view_tag_t::watching_k: decoded.transaction = static_cast<ustore_transaction_t>(handle); return true;
    case view_tag_t::blind_k:
        decoded.transaction = static_cast<ustore_transaction_t>(handle);
        decoded.watch = ustore_watch_none_k;
        return true;
    case view_tag_t::snapshot_k:
        decoded.snapshot = static_cast<ustore_snapshot_t>(handle);
        decoded.watch = ustore_watch_none_k;
        return true;
    }
    return false;
}

/** The transaction a call carries, or null. */
ustore_transaction_t carried(ustore_transaction_t transaction) noexcept { return transaction; }

/** The transaction a call reuses, or null for one it opens afresh. */
ustore_transaction_t carried(ustore_transaction_t const* transaction) noexcept {
    return transaction ? *transaction : nullptr;
}

/** The snapshot a call reads, or null, which an output for a new snapshot never matches. */
ustore_snapshot_t viewed(ustore_snapshot_t snapshot) noexcept { return snapshot; }

/** Serves @p arguments on the engine of their database, refusing what no engine may see. */
template <typename arguments_type_>
status_t serve(arguments_type_ const& arguments,
               status_t (*engine_t::*slot)(arguments_type_ const&, view_t const&) noexcept) noexcept {
    if (!arguments.database) return {status_code_t::uninitialized_k, "The database is uninitialized"};
    if (arguments.executor) return {status_code_t::unsupported_k, "This build runs every call on the calling thread"};
    engine_t const& engine = engine_of(arguments.database);
    if (!(engine.*slot)) return {status_code_t::unsupported_k, "The engine does not implement this call"};

    view_t view;
    if constexpr (requires { arguments.view; })
        if (!decode_view(arguments.view, view))
            return {status_code_t::invalid_argument_k, "The view was made by neither view constructor"};
    if constexpr (requires { carried(arguments.transaction); }) view.transaction = carried(arguments.transaction);
    if constexpr (requires { arguments.watch; }) view.watch = arguments.watch;
    if constexpr (requires { viewed(arguments.snapshot); }) view.snapshot = viewed(arguments.snapshot);
    void* const handle = view.transaction ? static_cast<void*>(view.transaction) : view.snapshot;
    if (handle && &engine_of(handle) != &engine)
        return {status_code_t::invalid_argument_k, "The view belongs to another engine"};

    if constexpr (requires { arguments.arena; })
        if (arguments.arena && *arguments.arena) arena_rewind(*arguments.arena);
    return (engine.*slot)(arguments, view);
}

/** Publishes @p status into @p outcome, the message first and the status last with release ordering. */
void publish(ustore_outcome_t& outcome, status_t status) noexcept {
    outcome.message = status.message;
    std::atomic_ref<ustore_status_t>(outcome.status)
        .store(static_cast<ustore_status_t>(status.code), std::memory_order_release);
}

/** Serves @p arguments and publishes the status once the engine returns. */
template <typename arguments_type_>
void route(arguments_type_* arguments,
           status_t (*engine_t::*slot)(arguments_type_ const&, view_t const&) noexcept) noexcept {
    publish(*arguments->outcome, serve(*arguments, slot));
}

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

using namespace unum::ustore;

void ustore_database_describe(ustore_database_describe_t* arguments) { route(arguments, &engine_t::database_describe); }
void ustore_database_control(ustore_database_control_t* arguments) { route(arguments, &engine_t::database_control); }
void ustore_snapshot_init(ustore_snapshot_init_t* arguments) { route(arguments, &engine_t::snapshot_init); }
void ustore_snapshot_export(ustore_snapshot_export_t* arguments) { route(arguments, &engine_t::snapshot_export); }
void ustore_collection_list(ustore_collection_list_t* arguments) { route(arguments, &engine_t::collection_list); }
void ustore_collection_create(ustore_collection_create_t* arguments) { route(arguments, &engine_t::collection_create); }
void ustore_collection_find(ustore_collection_find_t* arguments) { route(arguments, &engine_t::collection_find); }
void ustore_collection_drop(ustore_collection_drop_t* arguments) { route(arguments, &engine_t::collection_drop); }
void ustore_transaction_init(ustore_transaction_init_t* arguments) { route(arguments, &engine_t::transaction_init); }
void ustore_transaction_stage(ustore_transaction_stage_t* arguments) { route(arguments, &engine_t::transaction_stage); }
void ustore_transaction_commit(ustore_transaction_commit_t* arguments) {
    route(arguments, &engine_t::transaction_commit);
}
void ustore_transaction_rollback(ustore_transaction_rollback_t* arguments) {
    route(arguments, &engine_t::transaction_rollback);
}
void ustore_write(ustore_write_t* arguments) { route(arguments, &engine_t::write); }
void ustore_remove(ustore_remove_t* arguments) { route(arguments, &engine_t::remove); }
void ustore_read(ustore_read_t* arguments) { route(arguments, &engine_t::read); }
void ustore_scan(ustore_scan_t* arguments) { route(arguments, &engine_t::scan); }
void ustore_sample(ustore_sample_t* arguments) { route(arguments, &engine_t::sample); }
void ustore_measure(ustore_measure_t* arguments) { route(arguments, &engine_t::measure); }
void ustore_docs_write(ustore_docs_write_t* arguments) { route(arguments, &engine_t::docs_write); }
void ustore_docs_read(ustore_docs_read_t* arguments) { route(arguments, &engine_t::docs_read); }
void ustore_docs_gist(ustore_docs_gist_t* arguments) { route(arguments, &engine_t::docs_gist); }
void ustore_docs_gather(ustore_docs_gather_t* arguments) { route(arguments, &engine_t::docs_gather); }
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
void ustore_pages_import(ustore_pages_import_t* arguments) { route(arguments, &engine_t::pages_import); }
void ustore_pages_insert(ustore_pages_insert_t* arguments) { route(arguments, &engine_t::pages_insert); }
void ustore_pages_remove(ustore_pages_remove_t* arguments) { route(arguments, &engine_t::pages_remove); }
void ustore_pages_move(ustore_pages_move_t* arguments) { route(arguments, &engine_t::pages_move); }
void ustore_pages_splice(ustore_pages_splice_t* arguments) { route(arguments, &engine_t::pages_splice); }
void ustore_pages_set_attributes(ustore_pages_set_attributes_t* arguments) {
    route(arguments, &engine_t::pages_set_attributes);
}
void ustore_pages_remove_attributes(ustore_pages_remove_attributes_t* arguments) {
    route(arguments, &engine_t::pages_remove_attributes);
}
void ustore_pages_read(ustore_pages_read_t* arguments) { route(arguments, &engine_t::pages_read); }
void ustore_pages_history(ustore_pages_history_t* arguments) { route(arguments, &engine_t::pages_history); }
void ustore_strings_write(ustore_strings_write_t* arguments) { route(arguments, &engine_t::strings_write); }
void ustore_strings_remove(ustore_strings_remove_t* arguments) { route(arguments, &engine_t::strings_remove); }
void ustore_strings_read(ustore_strings_read_t* arguments) { route(arguments, &engine_t::strings_read); }
void ustore_strings_scan(ustore_strings_scan_t* arguments) { route(arguments, &engine_t::strings_scan); }

ustore_view_t ustore_transaction_view(ustore_transaction_t transaction, ustore_watch_t watch) {
    return tagged_view(transaction, watch == ustore_watch_none_k ? view_tag_t::blind_k : view_tag_t::watching_k);
}

ustore_view_t ustore_snapshot_view(ustore_snapshot_t snapshot) { return tagged_view(snapshot, view_tag_t::snapshot_k); }

ustore_status_t ustore_outcome_wait(ustore_outcome_t const* outcome) {
    // A load never writes, and `std::atomic_ref` over a constant arrives only in C++26
    ustore_status_t& status = const_cast<ustore_status_t&>(outcome->status);
    return std::atomic_ref<ustore_status_t>(status).load(std::memory_order_acquire);
}

char const* ustore_status_name(ustore_status_t status) {
    std::size_t const index = static_cast<std::size_t>(status);
    return status_names_k[index < status_names_k.size() ? index : ustore_status_internal_k].data();
}

ustore_status_t ustore_status_from_name(ustore_bytes_view_t name) {
    std::string_view const text {reinterpret_cast<char const*>(name.data), name.length};
    for (std::size_t index = 0; index != status_names_k.size(); ++index)
        if (status_names_k[index] == text) return static_cast<ustore_status_t>(index);
    return ustore_status_internal_k;
}

void ustore_version(int* major, int* minor, int* patch) {
    *major = USTORE_VERSION_MAJOR;
    *minor = USTORE_VERSION_MINOR;
    *patch = USTORE_VERSION_PATCH;
}

void ustore_executor_for_ram(ustore_executor_for_ram_t* arguments) {
    publish(*arguments->outcome, {status_code_t::unsupported_k, "This build runs every call on the calling thread"});
}

void ustore_executor_on_pool(ustore_executor_on_pool_t* arguments) {
    publish(*arguments->outcome, {status_code_t::unsupported_k, "This build runs every call on the calling thread"});
}

void ustore_executor_for_nvme(ustore_executor_for_nvme_t* arguments) {
    publish(*arguments->outcome, {status_code_t::unsupported_k, "This build runs every call on the calling thread"});
}

void ustore_executor_drain(ustore_executor_drain_t* arguments) {
    publish(*arguments->outcome, {status_code_t::unsupported_k, "This build runs every call on the calling thread"});
}

void ustore_executor_free(ustore_executor_t) {}

void ustore_transaction_free(ustore_transaction_t transaction) {
    if (transaction) engine_of(transaction).transaction_free(transaction);
}

void ustore_snapshot_free(ustore_snapshot_t snapshot) {
    if (snapshot) engine_of(snapshot).snapshot_free(snapshot);
}

void ustore_database_free(ustore_database_t database) {
    if (database) engine_of(database).database_free(database);
}

void ustore_arena_free(ustore_arena_t arena) { arena_free(arena); }

#if !USTORE_WITH_ROCKSDB
void ustore_database_init_rocksdb(ustore_database_init_rocksdb_t* arguments) {
    publish(*arguments->outcome, {status_code_t::unsupported_k, "This build carries no RocksDB engine"});
}
#endif

#if !USTORE_WITH_LEVELDB
void ustore_database_init_leveldb(ustore_database_init_leveldb_t* arguments) {
    publish(*arguments->outcome, {status_code_t::unsupported_k, "This build carries no LevelDB engine"});
}
#endif

#if !USTORE_WITH_NVME
void ustore_database_init_nvme(ustore_database_init_nvme_t* arguments) {
    publish(*arguments->outcome, {status_code_t::unsupported_k, "This build carries no NVMe engine"});
}
#endif
