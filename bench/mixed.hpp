/**
 *  @file bench/mixed.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Transactional batch upserts of documents and graph edges, shared by the benchmarks.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <functional>  // `std::plus`
#include <numeric>     // `std::transform_reduce`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <smashtable/basic_vector.hpp>

#include "ustore/db.h"
#include "ustore/docs.h"
#include "ustore/graph.h"
#include "ustore/types.hpp"

namespace unum::ustore::bench {

/** A document body with the key it is stored under. */
struct keyed_document_t {

    /** Key the document is stored under. */
    ustore_key_t key;

    /** JSON text of the document. */
    std::string_view body;
};

/** A directed edge between two vertices, labeled by an edge key. */
struct graph_edge_t {

    /** Source vertex. */
    ustore_key_t source;

    /** Target vertex. */
    ustore_key_t target;

    /** Edge key, @c ustore_default_edge_id_k for an unlabeled edge. */
    ustore_key_t id;
};

/** Elements the flattened sequence of sequences @p outer holds. */
template <typename nested_type_>
[[nodiscard]] std::size_t pass_through_size(nested_type_ const& outer) noexcept {
    return std::transform_reduce(outer.begin(), outer.end(), std::size_t {0}, std::plus<std::size_t> {},
                                 [](auto const& inner) noexcept { return inner.size(); });
}

/** Copies every element of the sequence of sequences @p outer into @p flat, answering false when it cannot grow. */
template <typename nested_type_, typename element_type_>
[[nodiscard]] bool flatten(nested_type_ const& outer, st::basic_vector<element_type_>& flat) noexcept {
    if (st::failed(flat.reserve(pass_through_size(outer)))) return false;
    for (auto const& inner : outer)
        for (auto const& element : inner) {
            [[maybe_unused]] st::status_t const pushed = flat.push_back(st::assume_reserved, element_type_ {element});
        }
    return true;
}

/** One document of an upsert batch, laid out for the strided C ABI. */
struct batch_document_t {

    /** Key the document is upserted under. */
    ustore_key_t key;

    /** First byte of the document's JSON text. */
    ustore_bytes_cptr_t value;

    /** Bytes of the document's JSON text. */
    ustore_length_t length;

    batch_document_t(keyed_document_t const& document) noexcept
        : key(document.key), value(reinterpret_cast<ustore_bytes_cptr_t>(document.body.data())),
          length(static_cast<ustore_length_t>(document.body.size())) {}
};

/** The arena, transaction and error one upserting thread reuses across its batches. */
struct upsert_scratch_t {
    ustore_arena_t arena = nullptr;
    ustore_transaction_t transaction = nullptr;
    ustore_error_t error {};

    upsert_scratch_t() noexcept = default;
    upsert_scratch_t(upsert_scratch_t const&) = delete;
    upsert_scratch_t& operator=(upsert_scratch_t const&) = delete;
    ~upsert_scratch_t() noexcept {
        ustore_transaction_free(transaction);
        ustore_arena_free(arena);
    }

    /** Opens a transaction, answering false when the engine refused it. */
    [[nodiscard]] bool begin(ustore_database_t db) noexcept {
        ustore_transaction_init_t init {};
        init.db = db;
        init.error = &error;
        init.transaction = &transaction;
        ustore_transaction_init(&init);
        return !failed();
    }

    /** Commits the open transaction, answering false on a conflict or a failure. */
    [[nodiscard]] bool commit(ustore_database_t db) noexcept {
        ustore_transaction_commit_t commit {};
        commit.db = db;
        commit.error = &error;
        commit.transaction = transaction;
        ustore_transaction_commit(&commit);
        return !failed();
    }

    /** Answers whether the last call failed, clearing its error. */
    [[nodiscard]] bool failed() noexcept {
        if (!error.status) return false;
        error = {};
        return true;
    }
};

/** Upserts @p batch into @p collection in one transaction, answering whether it committed. */
[[nodiscard]] inline bool docs_upsert(ustore_database_t db, ustore_collection_id_t collection,
                                      std::span<batch_document_t const> batch, upsert_scratch_t& scratch) noexcept {
    if (!scratch.begin(db)) return false;
    ustore_docs_write_t write {};
    write.db = db;
    write.error = &scratch.error;
    write.arena = &scratch.arena;
    write.transaction = scratch.transaction;
    write.type = ustore_doc_field_json_k;
    write.modification = ustore_doc_modify_upsert_k;
    write.tasks_count = batch.size();
    write.collections = &collection;
    write.keys = &batch.front().key;
    write.keys_stride = sizeof(batch_document_t);
    write.lengths = &batch.front().length;
    write.lengths_stride = sizeof(batch_document_t);
    write.values = &batch.front().value;
    write.values_stride = sizeof(batch_document_t);
    ustore_docs_write(&write);
    return !scratch.failed() && scratch.commit(db);
}

/** Upserts @p batch into @p collection in one transaction, answering whether it committed. */
[[nodiscard]] inline bool edges_upsert(ustore_database_t db, ustore_collection_id_t collection,
                                       std::span<graph_edge_t const> batch, upsert_scratch_t& scratch) noexcept {
    if (!scratch.begin(db)) return false;
    ustore_graph_upsert_edges_t upsert {};
    upsert.db = db;
    upsert.error = &scratch.error;
    upsert.arena = &scratch.arena;
    upsert.transaction = scratch.transaction;
    upsert.tasks_count = batch.size();
    upsert.collections = &collection;
    upsert.edges_ids = &batch.front().id;
    upsert.edges_stride = sizeof(graph_edge_t);
    upsert.sources_ids = &batch.front().source;
    upsert.sources_stride = sizeof(graph_edge_t);
    upsert.targets_ids = &batch.front().target;
    upsert.targets_stride = sizeof(graph_edge_t);
    ustore_graph_upsert_edges(&upsert);
    return !scratch.failed() && scratch.commit(db);
}

} // namespace unum::ustore::bench
