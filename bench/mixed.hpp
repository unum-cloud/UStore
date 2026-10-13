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
#include "ustore/ranges.hpp"
#include "ustore/types.hpp"

namespace unum::ustore::bench {

/** A document body with the key it is stored under. */
struct keyed_document_t {

    /** Key the document is stored under. */
    ustore_key_t key;

    /** JSON text of the document. */
    std::string_view body;
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
                                      std::span<keyed_document_t const> batch, upsert_scratch_t& scratch) noexcept {
    if (!scratch.begin(database)) return false;
    strided_strings<std::string_view> const bodies {strided(batch).members(&keyed_document_t::body).begin()};
    ustore_docs_write_t write {};
    write.database = database;
    write.outcome = &scratch.outcome;
    write.transaction = scratch.transaction;
    write.type = ustore_docs_field_type_json_k;
    write.modification = ustore_docs_modify_upsert_k;
    write.count = batch.size();
    write.collections = {&collection, 0};
    write.keys = {&batch.front().key, sizeof(keyed_document_t)};
    write.values = bodies.view();
    ustore_docs_write(&write);
    return !scratch.failed() && scratch.commit(database);
}

/** Upserts @p batch into @p collection in one transaction, answering whether it committed. */
[[nodiscard]] inline bool edges_upsert(ustore_database_t database, ustore_collection_id_t collection,
                                       std::span<edge_t const> batch, upsert_scratch_t& scratch) noexcept {
    if (!scratch.begin(database)) return false;
    ustore_graph_write_edges_t write {};
    write.database = database;
    write.outcome = &scratch.outcome;
    write.transaction = scratch.transaction;
    write.count = batch.size();
    write.collections = {&collection, 0};
    write.edges = {&batch.front().id, sizeof(edge_t)};
    write.sources = {&batch.front().source, sizeof(edge_t)};
    write.targets = {&batch.front().target, sizeof(edge_t)};
    ustore_graph_write_edges(&write);
    return !scratch.failed() && scratch.commit(database);
}

} // namespace unum::ustore::bench
