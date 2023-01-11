/**
 * @file modality_vectors.cpp
 * @author Ashot Vardanian
 *
 * @brief Vectors compatibility layer.
 * Sits on top of any @see "ukv.h"-compatible system.
 *
 * Internally quantizes often f32/f16 vectors into i8 representations,
 * later constructing a Navigable Small World Graph on those vectors.
 * During search relies on an algorithm resembling A*, adding a
 * stochastic component.
 */
#include <cmath> // `std::sqrt`

#include "ukv/vectors.h"
#include "ukv/cpp/ranges_args.hpp" // `places_arg_t`

#include "helpers/linked_memory.hpp"          // `linked_memory_lock_t`
#include "helpers/algorithm.hpp"              // `transform_n`
#include "helpers/full_scan.hpp"              // `full_scan_collection`
#include "helpers/limited_priority_queue.hpp" // `limited_priority_queue_gt`

/*********************************************************/
/*****************	 C++ Implementation	  ****************/
/*********************************************************/

using namespace unum::ukv;
using namespace unum;

using real_t = float;
using quant_t = std::int8_t;
using quant_product_t = std::int16_t;

struct match_t {
    ukv_key_t key;
    ukv_float_t metric;
};

struct lower_similarity_t {
    bool operator()(match_t const& a, match_t const& b) const noexcept { return a.metric < b.metric; }
};

using pq_t = limited_priority_queue_gt<match_t, lower_similarity_t>;

static constexpr quant_t float_scaling_k = 100;
static constexpr quant_product_t product_scaling_k = float_scaling_k * float_scaling_k;

template <typename number_at>
number_at square(number_at n) noexcept {
    return n * n;
}

struct metric_dot_t {
    real_t operator()(quant_t const* a, quant_t const* b, std::size_t dims) const noexcept {
        std::int64_t sum = 0;
        for (std::size_t i = 0; i != dims; ++i) {
            quant_product_t ai = a[i];
            quant_product_t bi = b[i];
            sum += ai * bi;
        }
        return real_t(sum) / product_scaling_k;
    }
};

struct metric_cos_t {
    real_t operator()(quant_t const* a, quant_t const* b, std::size_t dims) const noexcept {
        std::int64_t sum = 0, a_norm = 0, b_norm = 0;
        for (std::size_t i = 0; i != dims; ++i) {
            quant_product_t ai = a[i];
            quant_product_t bi = b[i];
            sum += ai * bi;
            a_norm += square(ai);
            b_norm += square(bi);
        }
        auto nominator = real_t(sum) / product_scaling_k;
        auto denominator = std::sqrt(real_t(a_norm) / product_scaling_k) * //
                           std::sqrt(real_t(b_norm) / product_scaling_k);
        return nominator / denominator;
    }
};

struct metric_l2_t {

    real_t operator()(quant_t const* a, std::size_t dims) const noexcept {
        std::int64_t sum = 0;
        for (std::size_t i = 0; i != dims; ++i)
            sum += square<quant_product_t>(a[i]);
        return std::sqrt(real_t(sum) / product_scaling_k);
    }

    real_t operator()(quant_t const* a, quant_t const* b, std::size_t dims) const noexcept {
        std::int64_t sum = 0;
        for (std::size_t i = 0; i != dims; ++i)
            sum += square<quant_product_t>(a[i] - b[i]);
        return std::sqrt(real_t(sum) / product_scaling_k);
    }
};

struct entry_t {
    collection_key_t collection_key;
    value_view_t value;
};

template <typename float_at = real_t>
void quantize(float_at const* originals, std::size_t dims, quant_t* quants) noexcept {
    for (std::size_t i = 0; i != dims; ++i)
        quants[i] = static_cast<quant_t>(originals[i] * float_scaling_k);
}

void quantize(byte_t const* bytes, ukv_vector_scalar_t scalar_type, std::size_t dims, quant_t* quants) noexcept {
    switch (scalar_type) {
    case ukv_vector_scalar_f32_k: return quantize((real_t const*)bytes, dims, quants);
    case ukv_vector_scalar_f64_k: return quantize((double const*)bytes, dims, quants);
    case ukv_vector_scalar_f16_k: return quantize((std::int16_t const*)bytes, dims, quants);
    case ukv_vector_scalar_i8_k: return quantize((quant_t const*)bytes, dims, quants);
    }
}

real_t metric(quant_t const* a, quant_t const* b, std::size_t dims, ukv_vector_metric_t kind) noexcept {
    switch (kind) {
    case ukv_vector_metric_dot_k: return metric_dot_t {}(a, b, dims);
    case ukv_vector_metric_cos_k: return metric_cos_t {}(a, b, dims);
    case ukv_vector_metric_l2_k: return metric_l2_t {}(a, b, dims);
    default: return 0;
    }
}

ukv_length_t size_bytes(ukv_vector_scalar_t scalar_type) noexcept {
    switch (scalar_type) {
    case ukv_vector_scalar_f32_k: return sizeof(real_t);
    case ukv_vector_scalar_f64_k: return sizeof(double);
    case ukv_vector_scalar_f16_k: return sizeof(std::int16_t);
    case ukv_vector_scalar_i8_k: return sizeof(quant_t);
    default: return 0;
    }
}

struct vectors_arg_t {
    strided_iterator_gt<ukv_bytes_cptr_t const> contents;
    strided_iterator_gt<ukv_length_t const> offsets;
    ukv_size_t vectors_stride;
    ukv_vector_scalar_t scalar_type;
    ukv_length_t dimensions;
    ukv_size_t tasks_count = 1;

    value_view_t operator[](std::size_t i) const noexcept {
        if (!contents)
            return {};
        ukv_bytes_cptr_t begin = contents[i];
        begin += offsets ? offsets[i] : 0u;
        begin += vectors_stride * i;
        return {begin, dimensions * size_bytes(scalar_type)};
    }
};

void ukv_vectors_write(ukv_vectors_write_t* c_ptr) {

    ukv_vectors_write_t& c = *c_ptr;
    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    strided_iterator_gt<ukv_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ukv_key_t const> keys {c.keys, c.keys_stride};
    places_arg_t places_args {collections, keys, {}, c.tasks_count};

    strided_iterator_gt<ukv_bytes_cptr_t const> starts {c.vectors_starts, c.vectors_starts_stride};
    strided_iterator_gt<ukv_length_t const> offs {c.offsets, c.offsets_stride};
    vectors_arg_t vectors_args {starts, offs, c.vectors_stride, c.scalar_type, c.dimensions, c.tasks_count};

    // For each input key we must get its
    auto quantized_entries = arena.alloc<entry_t>(c.tasks_count * 2u, c.error);
    return_if_error_m(c.error);

    auto quantized_vectors = arena.alloc<quant_t>(c.tasks_count * c.dimensions, c.error);
    return_if_error_m(c.error);

    // Add the original entries
    for (std::size_t task_idx = 0; task_idx != c.tasks_count; ++task_idx) {
        entry_t& entry = quantized_entries[task_idx];
        entry.collection_key.collection = places_args[task_idx].collection;
        entry.collection_key.key = places_args[task_idx].key;
        entry.value = vectors_args[task_idx];
    }

    // Add the mirror tasks for quantized copies
    for (std::size_t task_idx = 0; task_idx != c.tasks_count; ++task_idx) {
        auto original_begin = vectors_args[task_idx].begin();
        auto quantized_begin = quantized_vectors.begin() + task_idx * c.dimensions;
        entry_t& entry = quantized_entries[c.tasks_count + task_idx];
        entry.collection_key.collection = places_args[task_idx].collection;
        entry.collection_key.key = -places_args[task_idx].key;
        entry.value = value_view_t {(ukv_bytes_cptr_t)quantized_begin, c.dimensions};
        quantize(original_begin, c.scalar_type, c.dimensions, quantized_begin);
    }

#if 0 // Future Complex Index Logic

    // Search greedily for closest entries to the provided vectors.
    ukv_length_t starting_samples_limit = 128;
    std::size_t max_search_rounds = 10;
    std::size_t max_neighbors = 4;

    // First, we need to random sample some starting points.
    ukv_length_t* starting_samples_offsets = NULL;
    ukv_length_t* starting_samples_counts = NULL;
    ukv_key_t* starting_samples_keys = NULL;
    ukv_sample_t sample {};
    sample.db = c.db;
    sample.error = c.error;
    sample.transaction = c.transaction;
    sample.arena = c.arena;
    sample.options = c.options;
    sample.collections = &first.collection_key.collection;
    sample.collections_stride = sizeof(entry_t);
    sample.count_limits = &starting_samples_limit;
    sample.count_limits_stride = 0;
    sample.offsets = &starting_samples_offsets;
    sample.counts = &starting_samples_counts;
    sample.keys = &starting_samples_keys;
    ukv_sample(&sample);
    return_if_error_m(c.error);

    // Allocate two priority queues per request.
    // One will be used for top candidates, the other one for top results.
    std::size_t bytes_per_queue = pq_t::implicit_memory_usage(max_neighbors);
    std::size_t bytes_for_queues = c.tasks_count * 2ul * bytes_per_queue;
    auto buffer_for_queues = arena.alloc<byte_t>(bytes_for_queues, c.error);
    for (std::size_t task_idx = 0; task_idx != c.tasks_count; ++task_idx) {
        implicit_init(buffer_for_queues + bytes_per_queue * (task_idx * 2ul), max_neighbors);
        implicit_init(buffer_for_queues + bytes_per_queue * (task_idx * 2ul + 1ul), max_neighbors);
    }

    // First organize connections between the vectors in
    // the current batch mapping into the same space.
    // ! This is a quadratic complexity step.
    for (std::size_t a_idx = 0; a_idx != c.tasks_count; ++a_idx) {
        auto a_place = places_args[a_idx];
        auto a_quants = quantized_vectors.begin() + a_idx * c.dimensions;
        for (std::size_t b_idx = 0; b_idx != c.tasks_count; ++b_idx) {
            auto b_place = places_args[b_idx];
            auto b_quants = quantized_vectors.begin() + b_idx * c.dimensions;
            if (a_place.collection != b_place.collection)
                continue;

            match_t match;
            match.key = key;
            match.metric = metric(a_quants, b_quants, c.dimensions, c.metric);
            pq.push(match);
        }
    }

    // Greedily search for multiple rounds.

    // Introduce bidirectional links.
#endif

    // Submit both original and quantized entries
    entry_t& first = quantized_entries[0];
    ukv_write_t write {};
    write.db = c.db;
    write.error = c.error;
    write.transaction = c.transaction;
    write.arena = c.arena;
    write.options = c.options;
    write.tasks_count = c.tasks_count * 2u;
    write.collections = &first.collection_key.collection;
    write.collections_stride = sizeof(entry_t);
    write.keys = &first.collection_key.key;
    write.keys_stride = sizeof(entry_t);
    write.lengths = first.value.member_length();
    write.lengths_stride = sizeof(entry_t);
    write.values = first.value.member_ptr();
    write.values_stride = sizeof(entry_t);
    ukv_write(&write);
}

void ukv_vectors_read(ukv_vectors_read_t* c_ptr) {

    ukv_vectors_read_t& c = *c_ptr;
    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    strided_iterator_gt<ukv_collection_t const> collections {c.collections, c.collections_stride};
    strided_iterator_gt<ukv_key_t const> keys {c.keys, c.keys_stride};
    places_arg_t places_args {collections, keys, {}, c.tasks_count};

    auto vector_size = c.dimensions * size_bytes(c.scalar_type);

    // Read from disk, but potentially re-layout the data response.
    ukv_read_t read {};
    read.db = c.db;
    read.error = c.error;
    read.transaction = c.transaction;
    read.arena = arena;
    read.options = c.options;
    read.tasks_count = c.tasks_count;
    read.collections = c.collections;
    read.collections_stride = c.collections_stride;
    read.keys = keys.get();
    read.keys_stride = keys.stride();
    read.offsets = c.offsets;
    read.presences = c.presences;
    read.values = c.vectors;
    ukv_read(&read);
    return_if_error_m(c.error);

    // From here on, if we have the offsets don't form identical-length chunks,
    // we must compact the range:
}

void ukv_vectors_search(ukv_vectors_search_t* c_ptr) {

    ukv_vectors_search_t const& c = *c_ptr;
    linked_memory_lock_t arena = linked_memory(c.arena, c.options, c.error);
    return_if_error_m(c.error);

    strided_iterator_gt<ukv_bytes_cptr_t const> starts {c.queries_starts, c.queries_starts_stride};
    strided_iterator_gt<ukv_length_t const> offs {c.queries_offsets, c.queries_offsets_stride};
    vectors_arg_t queries_args {starts, offs, c.queries_stride, c.scalar_type, c.dimensions, c.tasks_count};

    strided_iterator_gt<ukv_collection_t const> collections {c.collections, c.collections_stride};
    strided_range_gt<ukv_length_t const> count_limits {{c.match_counts_limits, c.match_counts_limits_stride},
                                                       c.tasks_count};

    auto count_limits_max = ukv_length_t {0};
    auto count_limits_sum = transform_reduce_n(count_limits.begin(), c.tasks_count, 0ul, [&](ukv_length_t l) {
        count_limits_max = std::max(count_limits_max, l);
        return l;
    });

    auto found_counts = arena.alloc_or_dummy(c.tasks_count, c.error, c.match_counts);
    return_if_error_m(c.error);
    auto found_offsets = arena.alloc_or_dummy(c.tasks_count, c.error, c.match_offsets);
    return_if_error_m(c.error);
    auto found_keys = arena.alloc_or_dummy(count_limits_sum, c.error, c.match_keys);
    return_if_error_m(c.error);
    auto found_metrics = arena.alloc_or_dummy(count_limits_sum, c.error, c.match_metrics);
    return_if_error_m(c.error);

    auto temp_matches = arena.alloc<match_t>(count_limits_max, c.error);
    return_if_error_m(c.error);
    auto quant_query = arena.alloc<quant_t>(c.dimensions, c.error);
    return_if_error_m(c.error);

    ukv_length_t total_exported_matches = 0;
    for (std::size_t i = 0; i != c.tasks_count && !*c.error; ++i) {
        auto col = collections ? collections[i] : ukv_collection_main_k;
        auto query = queries_args[i];
        auto limit = count_limits[i];
        quantize(query.begin(), c.scalar_type, c.dimensions, quant_query.begin());

        pq_t pq {temp_matches.begin(), temp_matches.begin() + limit};

        auto callback = [&](ukv_key_t key, value_view_t vector) noexcept {
            if (key >= 0)
                return false;
            match_t match;
            match.key = key;
            match.metric = metric(quant_query.begin(), (quant_t const*)vector.data(), c.dimensions, c.metric);
            if (match.metric < c.metric_threshold)
                return true;

            pq.push(match);
            return true;
        };

        auto min_key = std::numeric_limits<ukv_key_t>::min();
        full_scan_collection(c.db, c.transaction, col, c.options, min_key, limit, arena, c.error, callback);
        auto count = pq.size();

        found_counts[i] = count;
        found_offsets[i] = total_exported_matches;

        for (std::size_t j = 0; j != count; ++j)
            found_keys[total_exported_matches + j] = std::abs(temp_matches[j].key), //
                found_metrics[total_exported_matches + j] = temp_matches[j].metric;

        total_exported_matches += count;
        pq.clear();
    }
}