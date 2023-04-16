#pragma once
#include <numeric>  // `std::transform_reduce`
#include <iterator> // `std::forward_iterator_tag`

#include <benchmark/benchmark.h> // `bm::State`

#include <ukv/ukv.hpp>
#include <ukv/cpp/ranges.hpp> // `sort_and_deduplicate`

namespace unum::ukv::bench {

using doc_w_key_t = std::pair<ukv_key_t, value_view_t>;
using doc_w_path_t = std::pair<value_view_t, value_view_t>;

template <typename array_of_arrays_at>
class pass_through_iterator_gt {
    using arr2d_t = array_of_arrays_at;
    using arr1d_t = typename arr2d_t::value_type;

    arr2d_t& array_of_arrays;
    std::size_t top_idx = 0;
    std::size_t nested_idx = 0;

  public:
    using value_type = typename arr1d_t::value_type;
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::size_t;
    using reference = value_type&;
    using pointer = void;

    pass_through_iterator_gt(arr2d_t& array, std::size_t global_offset) noexcept : array_of_arrays(array) {
        nested_idx = global_offset;
        while (array_of_arrays[top_idx].size() <= nested_idx) {
            nested_idx -= array_of_arrays[top_idx].size();
            top_idx++;
        }
    }
    pass_through_iterator_gt& operator++() noexcept {
        nested_idx++;
        while (top_idx < array_of_arrays.size() && nested_idx >= array_of_arrays[top_idx].size()) {
            top_idx++;
            nested_idx = 0;
        }
        return *this;
    }
    decltype(auto) operator*() const noexcept { return array_of_arrays[top_idx][nested_idx]; }
};

template <typename array_of_arrays_at>
std::size_t pass_through_size(array_of_arrays_at& array) noexcept {
    return std::transform_reduce(array.begin(), array.end(), 0ul, std::plus<std::size_t> {}, [](auto const& array) {
        return array.size();
    });
}

template <typename array_of_arrays_at>
pass_through_iterator_gt<array_of_arrays_at> //
pass_through_iterator(array_of_arrays_at& array, std::size_t offset = 0) noexcept {
    return {array, offset};
}

template <typename underlying_at, typename transform_at>
class multyplying_iterator_gt {
    using underlying_t = underlying_at;
    using transform_t = transform_at;
    underlying_t original_;
    transform_t transform_;
    std::size_t copy_idx_ = 0;
    std::size_t multiple_ = 0;

  public:
    using value_type = typename std::iterator_traits<underlying_t>::value_type;
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::size_t;
    using reference = value_type&;
    using pointer = void;

    multyplying_iterator_gt(underlying_at&& underlying, std::size_t multiple, transform_t&& transform) noexcept
        : original_(underlying), transform_(transform), multiple_(multiple) {}

    multyplying_iterator_gt& operator++() noexcept {
        copy_idx_++;
        if (copy_idx_ == multiple_) {
            ++original_;
            copy_idx_ = 0;
        }
        return *this;
    }
    decltype(auto) operator*() const noexcept { return transform_(*original_, copy_idx_); }
};

template <typename underlying_at, typename transform_at>
multyplying_iterator_gt<underlying_at, transform_at> //
multyplying_iterator(underlying_at&& array, std::size_t multiple, transform_at&& transform) noexcept {
    return {array, multiple, transform};
}

namespace bm = benchmark;

/**
 * @brief Builds up a document collection using batch-upserts.
 * @see `ukv_docs_write()`.
 */
template <typename doc_iterator_at>
void docs_upsert( //
    bm::State& state,
    database_t& db,
    ukv_collection_t collection,
    doc_iterator_at iterator,
    std::size_t total_count) {

    status_t status;
    arena_t arena(db);

    // Locate the portion of documents prepared for this thread
    std::size_t const docs_per_thread = total_count / state.threads();
    std::size_t const first_doc_idx = state.thread_index() * docs_per_thread;
    std::advance(iterator, first_doc_idx);

    // Pre-allocate space for our document handles
    auto const batch_size = static_cast<ukv_size_t>(state.range(0));
    std::vector<ukv_key_t> batch_keys(batch_size);
    std::vector<value_view_t> batch_values(batch_size);

    // Define the shape of the tasks
    ukv_docs_write_t docs_write {};
    docs_write.db = db;
    docs_write.error = status.member_ptr();
    docs_write.modification = ukv_doc_modify_upsert_k;
    docs_write.arena = arena.member_ptr();
    docs_write.type = ukv_doc_field_json_k;
    docs_write.tasks_count = batch_size;
    docs_write.collections = &collection;
    docs_write.keys = batch_keys.data();
    docs_write.keys_stride = sizeof(ukv_key_t);
    docs_write.lengths = batch_values.front().member_length();
    docs_write.lengths_stride = sizeof(value_view_t);
    docs_write.values = batch_values.front().member_ptr();
    docs_write.values_stride = sizeof(value_view_t);

    // All the upserts must be transactional
    ukv_transaction_t transaction = nullptr;
    ukv_transaction_init_t transaction_init {};
    transaction_init.db = db;
    transaction_init.error = status.member_ptr();
    transaction_init.transaction = &transaction;
    ukv_transaction_commit_t transaction_commit {};
    transaction_commit.db = db;
    transaction_commit.error = status.member_ptr();

    // Run the benchmark
    std::size_t docs_bytes = 0;
    std::size_t docs_success = 0;
    std::size_t batches_success = 0;
    for (auto _ : state) {

        // Start a new transaction
        ukv_transaction_init(&transaction_init);
        status.throw_unhandled();

        // Generate multiple IDs for each doc, to augment the dataset.
        std::size_t docs_bytes_in_batch = 0;
        for (std::size_t idx_in_batch = 0; idx_in_batch != batch_size; ++idx_in_batch, ++iterator) {
            doc_w_key_t doc = *iterator;
            batch_keys[idx_in_batch] = doc.first;
            batch_values[idx_in_batch] = doc.second;
            docs_bytes_in_batch += doc.second.size();
        }

        // Finally, import the data.
        docs_write.transaction = transaction;
        ukv_docs_write(&docs_write);
        status.throw_unhandled();

        transaction_commit.transaction = transaction;
        ukv_transaction_commit(&transaction_commit);
        if (status) {
            docs_bytes += docs_bytes_in_batch;
            docs_success += batch_size;
            batches_success += 1;
        }
        else
            status.release_exception();
    }

    // These will be summed across threads:
    state.counters["docs/s"] = bm::Counter(docs_success, bm::Counter::kIsRate);
    state.counters["batches/s"] = bm::Counter(batches_success, bm::Counter::kIsRate);
    state.counters["bytes/s"] = bm::Counter(docs_bytes, bm::Counter::kIsRate);
}

/**
 * @brief Builds up a graph collection using batch-upserts.
 * @see `ukv_graph_upsert_edges()`.
 */
template <typename edge_iterator_at>
void edges_upsert( //
    bm::State& state,
    database_t& db,
    ukv_collection_t collection,
    edge_iterator_at iterator,
    std::size_t total_count) {

    status_t status;
    arena_t arena(db);

    // Locate the portion of edgeuments prepared for this thread
    std::size_t const edges_per_thread = total_count / state.threads();
    std::size_t const first_edge_idx = state.thread_index() * edges_per_thread;
    std::advance(iterator, first_edge_idx);

    // Pre-allocate space for our edgeument handles
    auto const batch_size = static_cast<ukv_size_t>(state.range(0));
    std::vector<edge_t> batch_edges(batch_size);

    // Define the shape of the tasks
    auto strided = edges(batch_edges);
    ukv_graph_upsert_edges_t graph_upsert_edges {};
    graph_upsert_edges.db = db;
    graph_upsert_edges.error = status.member_ptr();
    graph_upsert_edges.arena = arena.member_ptr();
    graph_upsert_edges.tasks_count = batch_size;
    graph_upsert_edges.collections = &collection;
    graph_upsert_edges.edges_ids = strided.edge_ids.begin().get();
    graph_upsert_edges.edges_stride = strided.edge_ids.stride();
    graph_upsert_edges.sources_ids = strided.source_ids.begin().get();
    graph_upsert_edges.sources_stride = strided.source_ids.stride();
    graph_upsert_edges.targets_ids = strided.target_ids.begin().get();
    graph_upsert_edges.targets_stride = strided.target_ids.stride();

    // All the upserts must be transactional
    ukv_transaction_t transaction = nullptr;
    ukv_transaction_init_t transaction_init {};
    transaction_init.db = db;
    transaction_init.error = status.member_ptr();
    transaction_init.transaction = &transaction;
    ukv_transaction_commit_t transaction_commit {};
    transaction_commit.db = db;
    transaction_commit.error = status.member_ptr();

    // Run the benchmark
    std::size_t edges_bytes = 0;
    std::size_t edges_success = 0;
    std::size_t batches_success = 0;
    for (auto _ : state) {

        // Start a new transaction
        ukv_transaction_init(&transaction_init);
        status.throw_unhandled();

        // Generate multiple IDs for each edge, to augment the dataset.
        std::size_t edges_bytes_in_batch = 0;
        for (std::size_t idx_in_batch = 0; idx_in_batch != batch_size; ++idx_in_batch, ++iterator) {
            edge_t edge = *iterator;
            batch_edges[idx_in_batch] = edge;
            edges_bytes_in_batch += sizeof(edge_t);
        }

        // Finally, import the data.
        graph_upsert_edges.transaction = transaction;
        ukv_graph_upsert_edges(&graph_upsert_edges);
        status.throw_unhandled();

        transaction_commit.transaction = transaction;
        ukv_transaction_commit(&transaction_commit);
        if (status) {
            edges_bytes += edges_bytes_in_batch;
            edges_success += batch_size;
            batches_success += 1;
        }
        else
            status.release_exception();
    }

    // These will be summed across threads:
    state.counters["edges/s"] = bm::Counter(edges_success, bm::Counter::kIsRate);
    state.counters["batches/s"] = bm::Counter(batches_success, bm::Counter::kIsRate);
    state.counters["bytes/s"] = bm::Counter(edges_bytes, bm::Counter::kIsRate);
}

template <typename paths_iterator_at>
void paths_upsert( //
    bm::State& state,
    database_t& db,
    ukv_collection_t collection,
    paths_iterator_at iterator,
    std::size_t total_count) {

    status_t status;
    arena_t arena(db);
    ukv_char_t separator = 0;

    // Locate the portion of edgeuments prepared for this thread
    std::size_t const edges_per_thread = total_count / state.threads();
    std::size_t const first_edge_idx = state.thread_index() * edges_per_thread;
    std::advance(iterator, first_edge_idx);

    // Pre-allocate space for our edgeument handles
    auto const batch_size = static_cast<ukv_size_t>(state.range(0));
    std::vector<value_view_t> batch_paths(batch_size);
    std::vector<value_view_t> batch_values(batch_size);

    // Define the shape of the tasks
    ukv_paths_write_t paths_write {};
    paths_write.db = db;
    paths_write.error = status.member_ptr();
    paths_write.arena = arena.member_ptr();
    paths_write.tasks_count = batch_size;
    paths_write.path_separator = separator;
    paths_write.paths = (ukv_str_view_t*)batch_paths.front().member_ptr();
    paths_write.paths_stride = sizeof(value_view_t);
    paths_write.paths_lengths = batch_paths.front().member_length();
    paths_write.paths_lengths_stride = sizeof(value_view_t);
    paths_write.values_bytes = (ukv_bytes_cptr_t*)batch_values.front().member_ptr();
    paths_write.values_bytes_stride = sizeof(value_view_t);
    paths_write.values_lengths = batch_values.front().member_length();
    paths_write.values_lengths_stride = sizeof(value_view_t);

    // All the upserts must be transactional
    ukv_transaction_t transaction = nullptr;
    ukv_transaction_init_t transaction_init {};
    transaction_init.db = db;
    transaction_init.error = status.member_ptr();
    transaction_init.transaction = &transaction;
    ukv_transaction_commit_t transaction_commit {};
    transaction_commit.db = db;
    transaction_commit.error = status.member_ptr();

    // Run the benchmark
    std::size_t pairs_bytes = 0;
    std::size_t pairs_success = 0;
    std::size_t batches_success = 0;
    for (auto _ : state) {

        // Start a new transaction
        ukv_transaction_init(&transaction_init);
        status.throw_unhandled();

        // Generate multiple IDs for each pair, to augment the dataset.
        std::size_t pairs_bytes_in_batch = 0;
        for (std::size_t idx_in_batch = 0; idx_in_batch != batch_size; ++idx_in_batch, ++iterator) {
            doc_w_path_t pair = *iterator;
            batch_paths[idx_in_batch] = pair.first;
            batch_values[idx_in_batch] = pair.second;
            pairs_bytes_in_batch += pair.first.size() + pair.second.size();
        }

        // Finally, import the data.
        paths_write.transaction = transaction;
        ukv_paths_write(&paths_write);
        status.throw_unhandled();

        transaction_commit.transaction = transaction;
        ukv_transaction_commit(&transaction_commit);
        if (status) {
            pairs_bytes += pairs_bytes_in_batch;
            pairs_success += batch_size;
            batches_success += 1;
        }
        else
            status.release_exception();
    }

    // These will be summed across threads:
    state.counters["pairs/s"] = bm::Counter(pairs_success, bm::Counter::kIsRate);
    state.counters["batches/s"] = bm::Counter(batches_success, bm::Counter::kIsRate);
    state.counters["bytes/s"] = bm::Counter(pairs_bytes, bm::Counter::kIsRate);
}

} // namespace unum::ukv::bench