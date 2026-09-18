/**
 *  @file bench/mixed.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Transactional batch upserts of documents and graph edges, shared by the benchmarks.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::ptrdiff_t`

#include <functional>  // `std::plus`
#include <iterator>    // `std::advance`, `std::forward_iterator_tag`
#include <numeric>     // `std::transform_reduce`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::remove_cvref_t`
#include <utility>     // `std::declval`

#include <benchmark/benchmark.h>

#include <smashtable/basic_vector.hpp>

#include <ustore/db.h>
#include <ustore/docs.h>
#include <ustore/graph.h>
#include <ustore/types.hpp>

namespace unum::ustore {

namespace bm = benchmark;

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

/** Walks the elements of a sequence of sequences as one flat sequence. */
template <typename nested_type_>
class pass_through_iterator {
    nested_type_ const* outer_;
    std::size_t outer_index_ = 0;
    std::size_t inner_index_ = 0;

    void skip_exhausted() noexcept {
        while (outer_index_ < outer_->size() && inner_index_ >= (*outer_)[outer_index_].size()) {
            inner_index_ -= (*outer_)[outer_index_].size();
            ++outer_index_;
        }
    }

  public:
    using value_type = typename nested_type_::value_type::value_type;
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using reference = value_type const&;
    using pointer = void;

    explicit pass_through_iterator(nested_type_ const& outer) noexcept : outer_(&outer) { skip_exhausted(); }

    pass_through_iterator& operator++() noexcept {
        ++inner_index_;
        skip_exhausted();
        return *this;
    }

    reference operator*() const noexcept { return (*outer_)[outer_index_][inner_index_]; }
};

/** Elements the flattened sequence of sequences @p outer holds. */
template <typename nested_type_>
[[nodiscard]] std::size_t pass_through_size(nested_type_ const& outer) noexcept {
    return std::transform_reduce(outer.begin(), outer.end(), std::size_t {0}, std::plus<std::size_t> {},
                                 [](auto const& inner) noexcept { return inner.size(); });
}

/** One document of an upsert batch, laid out for the strided C ABI. */
struct batch_document_t {

    /** Key the document is upserted under. */
    ustore_key_t key;

    /** First byte of the document's JSON text. */
    ustore_bytes_cptr_t value;

    /** Bytes of the document's JSON text. */
    ustore_length_t length;
};

/** Upserts documents in transactional batches of @c state.range(0), each thread from its own share of @p documents. */
template <typename nested_documents_type_>
void docs_upsert(bm::State& state, ustore_database_t db, ustore_collection_t collection,
                 nested_documents_type_ const& documents) {
    pass_through_iterator<nested_documents_type_> staged_document {documents};
    std::advance(staged_document, state.thread_index() * (pass_through_size(documents) / state.threads()));

    auto const batch_size = static_cast<std::size_t>(state.range(0));
    st::basic_vector<batch_document_t> batch;
    if (st::failed(batch.resize(batch_size))) {
        state.SkipWithError("Failed to allocate the batch");
        return;
    }

    ustore_error_t error {};
    ustore_arena_t arena = nullptr;
    ustore_transaction_t transaction = nullptr;

    ustore_docs_write_t write {};
    write.db = db;
    write.error = &error;
    write.arena = &arena;
    write.type = ustore_doc_field_json_k;
    write.modification = ustore_doc_modify_upsert_k;
    write.tasks_count = batch_size;
    write.collections = &collection;
    write.keys = &batch.data()->key;
    write.keys_stride = sizeof(batch_document_t);
    write.lengths = &batch.data()->length;
    write.lengths_stride = sizeof(batch_document_t);
    write.values = &batch.data()->value;
    write.values_stride = sizeof(batch_document_t);

    ustore_transaction_init_t transaction_init {};
    transaction_init.db = db;
    transaction_init.error = &error;
    transaction_init.transaction = &transaction;
    ustore_transaction_commit_t transaction_commit {};
    transaction_commit.db = db;
    transaction_commit.error = &error;

    std::size_t committed_bytes = 0;
    std::size_t committed_documents = 0;
    std::size_t committed_batches = 0;
    for ([[maybe_unused]] auto iteration : state) {
        ustore_transaction_init(&transaction_init);
        if (error.status) {
            state.SkipWithError(error.message);
            break;
        }

        std::size_t batch_bytes = 0;
        for (batch_document_t& staged : batch) {
            keyed_document_t const& document = *staged_document;
            ++staged_document;
            staged = {document.key, reinterpret_cast<ustore_bytes_cptr_t>(document.body.data()),
                      static_cast<ustore_length_t>(document.body.size())};
            batch_bytes += document.body.size();
        }

        write.transaction = transaction;
        ustore_docs_write(&write);
        if (error.status) {
            state.SkipWithError(error.message);
            break;
        }

        transaction_commit.transaction = transaction;
        ustore_transaction_commit(&transaction_commit);
        if (error.status) {
            error = {};
            continue;
        }
        committed_bytes += batch_bytes;
        committed_documents += batch_size;
        committed_batches += 1;
    }
    ustore_transaction_free(transaction);
    ustore_arena_free(arena);

    state.counters["docs/s"] = bm::Counter(committed_documents, bm::Counter::kIsRate);
    state.counters["batches/s"] = bm::Counter(committed_batches, bm::Counter::kIsRate);
    state.counters["bytes/s"] = bm::Counter(committed_bytes, bm::Counter::kIsRate);
}

/** Upserts edges in transactional batches of @c state.range(0), each thread from its own share of @p edges. */
template <typename nested_edges_type_>
void edges_upsert(bm::State& state, ustore_database_t db, ustore_collection_t collection,
                  nested_edges_type_ const& edges) {
    pass_through_iterator<nested_edges_type_> staged_edge {edges};
    std::advance(staged_edge, state.thread_index() * (pass_through_size(edges) / state.threads()));

    auto const batch_size = static_cast<std::size_t>(state.range(0));
    st::basic_vector<graph_edge_t> batch;
    if (st::failed(batch.resize(batch_size))) {
        state.SkipWithError("Failed to allocate the batch");
        return;
    }

    ustore_error_t error {};
    ustore_arena_t arena = nullptr;
    ustore_transaction_t transaction = nullptr;

    ustore_graph_upsert_edges_t upsert {};
    upsert.db = db;
    upsert.error = &error;
    upsert.arena = &arena;
    upsert.tasks_count = batch_size;
    upsert.collections = &collection;
    upsert.edges_ids = &batch.data()->id;
    upsert.edges_stride = sizeof(graph_edge_t);
    upsert.sources_ids = &batch.data()->source;
    upsert.sources_stride = sizeof(graph_edge_t);
    upsert.targets_ids = &batch.data()->target;
    upsert.targets_stride = sizeof(graph_edge_t);

    ustore_transaction_init_t transaction_init {};
    transaction_init.db = db;
    transaction_init.error = &error;
    transaction_init.transaction = &transaction;
    ustore_transaction_commit_t transaction_commit {};
    transaction_commit.db = db;
    transaction_commit.error = &error;

    std::size_t committed_bytes = 0;
    std::size_t committed_edges = 0;
    std::size_t committed_batches = 0;
    for ([[maybe_unused]] auto iteration : state) {
        ustore_transaction_init(&transaction_init);
        if (error.status) {
            state.SkipWithError(error.message);
            break;
        }

        for (graph_edge_t& staged : batch) {
            staged = *staged_edge;
            ++staged_edge;
        }

        upsert.transaction = transaction;
        ustore_graph_upsert_edges(&upsert);
        if (error.status) {
            state.SkipWithError(error.message);
            break;
        }

        transaction_commit.transaction = transaction;
        ustore_transaction_commit(&transaction_commit);
        if (error.status) {
            error = {};
            continue;
        }
        committed_bytes += batch_size * sizeof(graph_edge_t);
        committed_edges += batch_size;
        committed_batches += 1;
    }
    ustore_transaction_free(transaction);
    ustore_arena_free(arena);

    state.counters["edges/s"] = bm::Counter(committed_edges, bm::Counter::kIsRate);
    state.counters["batches/s"] = bm::Counter(committed_batches, bm::Counter::kIsRate);
    state.counters["bytes/s"] = bm::Counter(committed_bytes, bm::Counter::kIsRate);
}

} // namespace unum::ustore
