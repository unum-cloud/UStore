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

/** The transaction and outcome one upserting thread reuses across its batches. */
struct upsert_scratch_t {
    ustore_transaction_t transaction = nullptr;
    ustore_outcome_t outcome {};

    upsert_scratch_t() noexcept = default;
    upsert_scratch_t(upsert_scratch_t const&) = delete;
    upsert_scratch_t& operator=(upsert_scratch_t const&) = delete;
    ~upsert_scratch_t() noexcept { ustore_transaction_free(transaction); }

    /** Opens the transaction, or resets it after the previous batch, answering false when the engine refused. */
    [[nodiscard]] bool begin(ustore_database_t database) noexcept {
        ustore_transaction_init_t init {};
        init.database = database;
        init.outcome = &outcome;
        init.transaction = &transaction;
        ustore_transaction_init(&init);
        return !failed();
    }

    /** Commits the open transaction, answering false on a conflict or a failure. */
    [[nodiscard]] bool commit(ustore_database_t database) noexcept {
        ustore_transaction_commit_t commit {};
        commit.database = database;
        commit.outcome = &outcome;
        commit.transaction = transaction;
        ustore_transaction_commit(&commit);
        return !failed();
    }

    /** Answers whether the last call failed, clearing its outcome. */
    [[nodiscard]] bool failed() noexcept {
        if (outcome.status == ustore_status_success_k) return false;
        outcome = {};
        return true;
    }
};

/** Upserts @p batch into @p collection in one transaction, answering whether it committed. */
[[nodiscard]] inline bool docs_upsert(ustore_database_t database, ustore_collection_id_t collection,
                                      std::span<batch_document_t const> batch, upsert_scratch_t& scratch) noexcept {
    if (!scratch.begin(database)) return false;
    ustore_docs_write_t write {};
    write.database = database;
    write.outcome = &scratch.outcome;
    write.transaction = scratch.transaction;
    write.type = ustore_doc_field_json_k;
    write.modification = ustore_doc_modify_upsert_k;
    write.count = batch.size();
    write.collections = &collection;
    write.keys = &batch.front().key;
    write.keys_stride = sizeof(batch_document_t);
    write.lengths = &batch.front().length;
    write.lengths_stride = sizeof(batch_document_t);
    write.values = &batch.front().value;
    write.values_stride = sizeof(batch_document_t);
    ustore_docs_write(&write);
    return !scratch.failed() && scratch.commit(database);
}

/** Upserts @p batch into @p collection in one transaction, answering whether it committed. */
[[nodiscard]] inline bool edges_upsert(ustore_database_t database, ustore_collection_id_t collection,
                                       std::span<graph_edge_t const> batch, upsert_scratch_t& scratch) noexcept {
    if (!scratch.begin(database)) return false;
    ustore_graph_upsert_edges_t upsert {};
    upsert.database = database;
    upsert.outcome = &scratch.outcome;
    upsert.transaction = scratch.transaction;
    upsert.count = batch.size();
    upsert.collections = &collection;
    upsert.edges = &batch.front().id;
    upsert.edges_stride = sizeof(graph_edge_t);
    upsert.sources = &batch.front().source;
    upsert.sources_stride = sizeof(graph_edge_t);
    upsert.targets = &batch.front().target;
    upsert.targets_stride = sizeof(graph_edge_t);
    ustore_graph_upsert_edges(&upsert);
    return !scratch.failed() && scratch.commit(database);
}

} // namespace unum::ustore::bench
