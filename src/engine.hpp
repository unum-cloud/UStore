/**
 *  @file src/engine.hpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The table of calls every engine fills, which the C ABI in `c/lib.cpp` routes each handle through.
 */

#pragma once

#include <cstdint> // `std::uint8_t`
#include <cstring> // `std::memcpy`

#include <new>         // `std::bad_alloc`
#include <string_view> // `std::string_view`

#include "ustore/blobs.h"   // `ustore_read_t`, `ustore_write_t`, `ustore_remove_t` and the other binary calls
#include "ustore/db.h"      // `ustore_database_t`, `ustore_transaction_t`, `ustore_view_t` and the calls on them
#include "ustore/docs.h"    // `ustore_docs_write_t`, `ustore_docs_read_t` and the other document calls
#include "ustore/graph.h"   // `ustore_graph_find_edges_t`, `ustore_graph_upsert_edges_t` and the other graph calls
#include "ustore/pages.h"   // `ustore_pages_import_t`, `ustore_pages_read_t` and the other page calls
#include "ustore/strings.h" // `ustore_strings_write_t`, `ustore_strings_remove_t` and the other strings calls

#include "ustore/status.hpp" // `expected`, `status_t`

namespace unum::ustore {

/** What a call reads through, decoded once by the router from its view, or from the transaction a write carries. */
struct view_t {

    /** Transaction the call reads or writes through, or null. */
    ustore_transaction_t transaction = nullptr;

    /** Snapshot the call reads, or null. */
    ustore_snapshot_t snapshot = nullptr;

    /** Whether the transaction records the keys the call touches. */
    ustore_watch_t watch = ustore_watch_keys_k;
};

/**
 *  @brief One engine's implementation of every call of the C ABI but its own init.
 *
 *  Every call is `noexcept` and answers a status, which the router publishes into the caller's outcome: the in-house
 *  engines never throw, and an oracle wraps each call of its library in @c guarded. A null entry answers
 *  @c ustore_status_unsupported_k, as the pages calls do on every engine.
 */
struct engine_t {
    status_t (*database_describe)(ustore_database_describe_t const&, view_t const&) noexcept;
    status_t (*database_control)(ustore_database_control_t const&, view_t const&) noexcept;
    status_t (*snapshot_init)(ustore_snapshot_init_t const&, view_t const&) noexcept;
    status_t (*snapshot_export)(ustore_snapshot_export_t const&, view_t const&) noexcept;
    status_t (*collection_list)(ustore_collection_list_t const&, view_t const&) noexcept;
    status_t (*collection_create)(ustore_collection_create_t const&, view_t const&) noexcept;
    status_t (*collection_find)(ustore_collection_find_t const&, view_t const&) noexcept;
    status_t (*collection_drop)(ustore_collection_drop_t const&, view_t const&) noexcept;
    status_t (*transaction_init)(ustore_transaction_init_t const&, view_t const&) noexcept;
    status_t (*transaction_stage)(ustore_transaction_stage_t const&, view_t const&) noexcept;
    status_t (*transaction_commit)(ustore_transaction_commit_t const&, view_t const&) noexcept;
    status_t (*transaction_rollback)(ustore_transaction_rollback_t const&, view_t const&) noexcept;
    status_t (*write)(ustore_write_t const&, view_t const&) noexcept;
    status_t (*remove)(ustore_remove_t const&, view_t const&) noexcept;
    status_t (*read)(ustore_read_t const&, view_t const&) noexcept;
    status_t (*scan)(ustore_scan_t const&, view_t const&) noexcept;
    status_t (*sample)(ustore_sample_t const&, view_t const&) noexcept;
    status_t (*measure)(ustore_measure_t const&, view_t const&) noexcept;
    status_t (*docs_write)(ustore_docs_write_t const&, view_t const&) noexcept;
    status_t (*docs_read)(ustore_docs_read_t const&, view_t const&) noexcept;
    status_t (*docs_gist)(ustore_docs_gist_t const&, view_t const&) noexcept;
    status_t (*docs_gather)(ustore_docs_gather_t const&, view_t const&) noexcept;
    status_t (*graph_find_edges)(ustore_graph_find_edges_t const&, view_t const&) noexcept;
    status_t (*graph_upsert_edges)(ustore_graph_upsert_edges_t const&, view_t const&) noexcept;
    status_t (*graph_remove_edges)(ustore_graph_remove_edges_t const&, view_t const&) noexcept;
    status_t (*graph_upsert_vertices)(ustore_graph_upsert_vertices_t const&, view_t const&) noexcept;
    status_t (*graph_remove_vertices)(ustore_graph_remove_vertices_t const&, view_t const&) noexcept;
    status_t (*pages_import)(ustore_pages_import_t const&, view_t const&) noexcept;
    status_t (*pages_insert)(ustore_pages_insert_t const&, view_t const&) noexcept;
    status_t (*pages_remove)(ustore_pages_remove_t const&, view_t const&) noexcept;
    status_t (*pages_move)(ustore_pages_move_t const&, view_t const&) noexcept;
    status_t (*pages_splice)(ustore_pages_splice_t const&, view_t const&) noexcept;
    status_t (*pages_set_attributes)(ustore_pages_set_attributes_t const&, view_t const&) noexcept;
    status_t (*pages_remove_attributes)(ustore_pages_remove_attributes_t const&, view_t const&) noexcept;
    status_t (*pages_read)(ustore_pages_read_t const&, view_t const&) noexcept;
    status_t (*pages_history)(ustore_pages_history_t const&, view_t const&) noexcept;
    status_t (*strings_write)(ustore_strings_write_t const&, view_t const&) noexcept;
    status_t (*strings_remove)(ustore_strings_remove_t const&, view_t const&) noexcept;
    status_t (*strings_read)(ustore_strings_read_t const&, view_t const&) noexcept;
    status_t (*strings_scan)(ustore_strings_scan_t const&, view_t const&) noexcept;
    void (*transaction_free)(ustore_transaction_t) noexcept;
    void (*snapshot_free)(ustore_snapshot_t) noexcept;
    void (*database_free)(ustore_database_t) noexcept;
};

/** Leads every database, transaction and snapshot handle an engine hands out, naming the engine that serves it. */
struct handle_t {
    engine_t const* engine = nullptr;
};

/** The engine's own handle behind @p handle, which the engine handed out as the @c handle_t it derives from. */
template <typename handle_type_>
handle_type_* handle_cast(void* handle) noexcept {
    return static_cast<handle_type_*>(static_cast<handle_t*>(handle));
}

/** The engine behind @p handle, as the engine stamped it. */
inline engine_t const& engine_of(void* handle) noexcept { return *handle_cast<handle_t>(handle)->engine; }

/** The document calls of an engine keeping every document as one value of JSON text, which `c/docs.cpp` serves
 *  through the engine's own reads and writes. */
status_t docs_write_in_json_values(ustore_docs_write_t const&, view_t const&) noexcept;
status_t docs_read_in_json_values(ustore_docs_read_t const&, view_t const&) noexcept;
status_t docs_gist_in_json_values(ustore_docs_gist_t const&, view_t const&) noexcept;
status_t docs_gather_in_json_values(ustore_docs_gather_t const&, view_t const&) noexcept;

/** The graph calls of an engine keeping every vertex as one value of its degrees and sorted neighborships, which
 *  `c/graph.cpp` serves through the engine's own reads and writes. */
status_t graph_find_edges_in_vertex_values(ustore_graph_find_edges_t const&, view_t const&) noexcept;
status_t graph_upsert_edges_in_vertex_values(ustore_graph_upsert_edges_t const&, view_t const&) noexcept;
status_t graph_remove_edges_in_vertex_values(ustore_graph_remove_edges_t const&, view_t const&) noexcept;
status_t graph_upsert_vertices_in_vertex_values(ustore_graph_upsert_vertices_t const&, view_t const&) noexcept;
status_t graph_remove_vertices_in_vertex_values(ustore_graph_remove_vertices_t const&, view_t const&) noexcept;

/** Mints a UUIDv7 for a new collection, refusing when the system gives no entropy. */
expected<ustore_collection_id_t> mint_collection_id() noexcept;

/** The C ABI's opaque spelling of @p handle, which @c handle_cast turns back into the engine's own handle. */
template <typename c_handle_type_>
c_handle_type_ c_handle(handle_t* handle) noexcept {
    return reinterpret_cast<c_handle_type_>(handle);
}

/** Serves @p serve_ of an oracle, whose library may throw, turning whatever it throws into a status. */
template <typename arguments_type_, status_t (*serve_)(arguments_type_ const&, view_t const&)>
status_t guarded(arguments_type_ const& arguments, view_t const& view) noexcept {
    try {
        return serve_(arguments, view);
    }
    catch (std::bad_alloc const&) {
        return {status_code_t::out_of_memory_k, "Out of memory"};
    }
    catch (...) {
        return {status_code_t::internal_k, "Unexpected engine failure"};
    }
}

/** How a call keys the collections it addresses. */
enum class keying_t : std::uint8_t {
    uuids_k,
    strings_k,
};

/** How a collection holding @p modality is keyed. */
constexpr keying_t keying_of(ustore_modality_t modality) noexcept {
    return modality == ustore_modality_strings_k ? keying_t::strings_k : keying_t::uuids_k;
}

/** The record a collection's name maps onto: the collection ID in bytes @b [0,16), its modality in byte 16. */
struct name_record_t {
    char bytes[sizeof(ustore_collection_id_t) + 1] {};

    /** The record of the main collection, which holds anything. */
    name_record_t() noexcept = default;

    name_record_t(ustore_collection_id_t id, ustore_modality_t modality) noexcept {
        std::memcpy(bytes, &id, sizeof(id));
        bytes[sizeof(id)] = static_cast<char>(modality);
    }

    [[nodiscard]] std::string_view view() const noexcept { return {bytes, sizeof(bytes)}; }

    [[nodiscard]] ustore_collection_id_t id() const noexcept {
        ustore_collection_id_t collection;
        std::memcpy(&collection, bytes, sizeof(collection));
        return collection;
    }

    [[nodiscard]] ustore_modality_t modality() const noexcept {
        return static_cast<ustore_modality_t>(bytes[sizeof(ustore_collection_id_t)]);
    }
};

/** The record the bytes of @p record hold, refusing bytes of any other length. */
template <typename bytes_type_>
expected<name_record_t> read_name_record(bytes_type_ const& record) noexcept {
    name_record_t read;
    if (record.size() != sizeof(read.bytes))
        return status_t {status_code_t::malformed_k, "A name holds no collection record"};
    std::memcpy(read.bytes, record.data(), sizeof(read.bytes));
    return read;
}

} // namespace unum::ustore
