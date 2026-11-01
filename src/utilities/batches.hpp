/**
 *  @file src/utilities/batches.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief The batched arguments of the C ABI gathered for the engines, and the checks every engine runs on them.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::ptrdiff_t`
#include <cstdint> // `std::uint64_t`, `std::uintptr_t`

#include <chrono>      // `std::chrono::steady_clock`
#include <limits>      // `std::numeric_limits`
#include <string_view> // `std::string_view`

#include "ustore/db.h"       // `ustore_ingest_append_t`, `ustore_collection_create_t`, `ustore_drop_mode_t`
#include "ustore/ranges.hpp" // `strided_iterator`, `strided`, `place_batch_t`, `edges_view_t`, `tape_string`
#include "ustore/status.hpp" // `status_t`, `expected`
#include "ustore/types.hpp"  // `smallest_key_k`, `string_of`

#include "utilities/outputs.hpp" // `collection_find_outputs_t`

namespace unum::ustore::utilities {

#pragma region Argument Packs

/** Reads string @c index of a ragged input in any layout, the accessor's views gathered into scratch beforehand. */
struct strings_arg_t {

    /** The input as the caller passed it. */
    ustore_strings_view_t view {};

    /** The view of every string an accessor wrote, or null for a tape. */
    ustore_bytes_view_t const* gathered = nullptr;

    /** Reads @p views, one per string, as the views an accessor wrote. */
    static strings_arg_t from_views(ustore_bytes_view_t const* views) noexcept {
        strings_arg_t strings;
        strings.view.layout = ustore_strings_accessor_k;
        strings.gathered = views;
        return strings;
    }

    /** Whether the input holds strings, where a zeroed view stands for the input's default. */
    explicit operator bool() const noexcept {
        return view.layout != ustore_strings_u32_tape_k || view.data.u32_tape.offsets;
    }

    ustore_bytes_view_t operator[](std::size_t index) const noexcept {
        if (view.layout == ustore_strings_accessor_k) return gathered[index];
        return ustore::tape_string(view, index);
    }
};

/** One task of a batched scan. */
struct scan_task_t {
    ustore_collection_id_t collection;
    ustore_key_t start_key;
    ustore_size_t limit;
};

/** The arguments of @c ustore_blobs_scan gathered into a structure of arrays for validation. */
struct scan_batch_t {
    strided_iterator<ustore_collection_id_t const> collections;
    strided_iterator<ustore_key_t const> start_keys;
    ustore_size_t wanted = 0;
    ustore_size_t count = 0;

    std::size_t size() const noexcept { return count; }
    scan_task_t operator[](std::size_t index) const noexcept {
        auto const signed_index = static_cast<std::ptrdiff_t>(index);
        ustore_key_t start_key = start_keys ? start_keys[signed_index] : smallest_key_k;
        return {collections[signed_index], start_key, wanted};
    }
};

/** One task of a batched sample. */
struct sample_task_t {
    ustore_collection_id_t collection;
    ustore_size_t limit;
};

/** The arguments of @c ustore_blobs_sample gathered into a structure of arrays for validation. */
struct sample_batch_t {
    strided_iterator<ustore_collection_id_t const> collections;
    ustore_size_t wanted = 0;
    ustore_size_t count = 0;

    std::size_t size() const noexcept { return count; }
    sample_task_t operator[](std::size_t index) const noexcept {
        return {collections[static_cast<std::ptrdiff_t>(index)], wanted};
    }
};

/** The tasks of a scan of any modality. */
template <typename arguments_type_>
scan_batch_t scans_of(arguments_type_ const& arguments) noexcept {
    return {strided(arguments.collections), strided(arguments.start_keys), arguments.wanted, arguments.count};
}

/** The tasks of a sample of any modality. */
template <typename arguments_type_>
sample_batch_t samples_of(arguments_type_ const& arguments) noexcept {
    return {strided(arguments.collections), arguments.wanted, arguments.count};
}

/** A splitmix64 generator, drawing the positions a reservoir keeps. */
struct sample_generator_t {
    using result_type = std::uint64_t;

    /** The state every draw advances. */
    std::uint64_t state;

    static constexpr result_type min() noexcept { return 0; }
    static constexpr result_type max() noexcept { return std::numeric_limits<result_type>::max(); }

    /** A generator seeded by the clock and the address @p salt, so two samples of one
     *  moment still differ. */
    static sample_generator_t seeded(void const* salt) noexcept {
        auto const now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        return sample_generator_t {now ^ reinterpret_cast<std::uintptr_t>(salt)};
    }

    result_type operator()() noexcept {
        std::uint64_t mixed = (state += 0x9E3779B97F4A7C15ull);
        mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
        mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
        return mixed ^ (mixed >> 31);
    }
};

/** One task of a batched edge read. */
struct edges_task_t {
    ustore_collection_id_t collection;
    ustore_key_t const& vertex_id;
    ustore_vertex_role_t role;
};

/** The vertices of a batched edge read, as the C ABI receives them. */
struct edges_batch_t {
    using value_type = edges_task_t;
    strided_iterator<ustore_collection_id_t const> collections_begin;
    strided_iterator<ustore_key_t const> vertex_id_begin;
    strided_iterator<ustore_vertex_role_t const> roles_begin;
    ustore_size_t count = 0;

    std::size_t size() const noexcept { return count; }
    edges_task_t operator[](std::size_t index) const noexcept {
        auto const signed_index = static_cast<std::ptrdiff_t>(index);
        ustore_key_t const& vertex_id = vertex_id_begin[signed_index];
        ustore_vertex_role_t role = roles_begin ? roles_begin[signed_index] : ustore_vertex_role_any_k;
        return {collections_begin[signed_index], vertex_id, role};
    }
};

/** The locations of a call addressing keys in collections, as the C ABI receives them. */
template <typename arguments_type_>
place_batch_t places_of(arguments_type_ const& arguments) noexcept {
    return {strided(arguments.collections), strided(arguments.keys), {}, arguments.count};
}

/** The edges of a call writing or removing them, whose identifiers default to @c ustore_edge_id_default_k. */
template <typename arguments_type_>
edges_view_t edges_of(arguments_type_ const& arguments) noexcept {
    strided_span<ustore_key_t const> const sources {strided(arguments.sources), arguments.count};
    strided_span<ustore_key_t const> const targets {strided(arguments.targets), arguments.count};
    if (!arguments.edges.data) return {sources, targets};
    return {sources, targets, {strided(arguments.edges), arguments.count}};
}

/** The rows one ingest append hands an engine for a collection of keys: blobs, documents, pages or strings. */
struct keyed_rows_t {

    /** Keys of blobs, documents and pages, or the keys strings map onto. */
    strided_span<ustore_key_t const> keys;

    /** Values of blobs, documents and pages, or the strings of a strings collection. */
    strings_arg_t values;

    std::size_t size() const noexcept { return keys.size(); }
};

/** The keyed rows @p arguments append to an ingest, with @p values gathered, refusing an append that left either
 *  column out. */
inline expected<keyed_rows_t> keyed_rows_of(ustore_ingest_append_t const& arguments,
                                            strings_arg_t const& values) noexcept {
    if (arguments.count && (!arguments.keys.data || !values))
        return status_t {status_code_t::invalid_argument_k, "Rows need their keys and values"};
    return keyed_rows_t {{strided(arguments.keys), arguments.count}, values};
}

/** The edges @p arguments append to an ingest, refusing an append that left their sources or targets out. */
inline expected<edges_view_t> ingest_edges_of(ustore_ingest_append_t const& arguments) noexcept {
    if (arguments.count && (!arguments.sources.data || !arguments.targets.data))
        return status_t {status_code_t::invalid_argument_k, "Edges need their sources and targets"};
    return edges_of(arguments);
}

#pragma endregion Argument Packs

#pragma region Validation

/** Checks the durability and watch of a write or a removal against its transaction,
 *  or its absence. */
inline status_t validate_choices(ustore_transaction_t const transaction, ustore_durability_t const durability,
                                 ustore_watch_t const watch) noexcept {
    if (durability != ustore_durability_buffered_k && durability != ustore_durability_flushed_k)
        return {status_code_t::invalid_argument_k, "Unknown durability"};
    if (watch != ustore_watch_keys_k && watch != ustore_watch_none_k)
        return {status_code_t::invalid_argument_k, "Unknown watch"};
    if (transaction && durability == ustore_durability_flushed_k)
        return {status_code_t::invalid_argument_k, "A transaction flushes at commit, not at a write"};
    return {};
}

/** Checks the keys and choices of a removal, which a write shares. */
inline status_t validate_remove(ustore_transaction_t const transaction, place_batch_t const& places,
                                ustore_durability_t const durability, ustore_watch_t const watch) noexcept {
    if (status_t const status = validate_choices(transaction, durability, watch); status.failed()) return status;
    if (!transaction && watch == ustore_watch_none_k)
        return {status_code_t::invalid_argument_k, "Keys stay unwatched only inside a transaction"};
    if (!places.count && places.keys_begin) return {status_code_t::invalid_argument_k, "Keys were given for no tasks"};
    if (places.count && !places.keys_begin) return {status_code_t::invalid_argument_k, "No keys were provided"};
    return {};
}

inline status_t validate_write(ustore_transaction_t const transaction, place_batch_t const& places,
                               strings_arg_t const& values, ustore_durability_t const durability,
                               ustore_watch_t const watch) noexcept {

    if (status_t const status = validate_remove(transaction, places, durability, watch); status.failed()) return status;
    if (places.count && !values) return {status_code_t::invalid_argument_k, "No values were provided"};
    return {};
}

inline status_t validate_read(place_batch_t const& places) noexcept {
    if (!places.keys_begin) return {status_code_t::invalid_argument_k, "No keys were provided"};
    return {};
}

/** Checks the mode and the outputs of a scan of any modality. */
template <typename arguments_type_>
status_t validate_scan(arguments_type_ const& arguments) noexcept {

    if (arguments.mode != ustore_scan_ordered_k && arguments.mode != ustore_scan_unordered_k)
        return {status_code_t::invalid_argument_k, "Unknown scan mode"};
    if (!arguments.offsets || !arguments.keys)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the keys"};
    return {};
}

/** Checks the output and the name of a collection create, answering the name, empty for
 *  an unnamed collection. */
inline expected<std::string_view> validate_collection_create(ustore_collection_create_t const& arguments) noexcept {
    if (!arguments.collection) return status_t {status_code_t::invalid_argument_k, "Need an output for the collection"};
    std::string_view const name = string_of(arguments.name);
    if (arguments.name.data && name.empty())
        return status_t {status_code_t::invalid_argument_k, "A name is never empty"};
    return name;
}

/** Checks the names and the output of a collection find of at least one name, and
 *  opens its columns. */
inline expected<collection_find_outputs_t> validate_collection_find(ustore_collection_find_t const& arguments,
                                                                    strings_arg_t const& names) noexcept {
    if (!names || !arguments.collections)
        return status_t {status_code_t::invalid_argument_k, "Need the names and an output for the identifiers"};
    return collection_find_outputs_t::make(arguments);
}

/** Checks that @p mode names one of the three drop modes. */
inline status_t validate_drop_mode(ustore_drop_mode_t mode) noexcept {
    if (mode != ustore_drop_collection_k && mode != ustore_drop_pairs_k && mode != ustore_drop_values_k)
        return {status_code_t::invalid_argument_k, "Unknown drop mode"};
    return {};
}

inline status_t validate_transaction_begin(ustore_transaction_t const* transaction) noexcept {
    if (!transaction) return {status_code_t::invalid_argument_k, "Need an output for the transaction"};
    return {};
}

inline status_t validate_transaction_commit(ustore_transaction_t const transaction,
                                            ustore_durability_t const durability) noexcept {

    if (!transaction) return {status_code_t::uninitialized_k, "Transaction is uninitialized"};
    if (durability != ustore_durability_buffered_k && durability != ustore_durability_flushed_k)
        return {status_code_t::invalid_argument_k, "Unknown durability"};
    return {};
}

#pragma endregion Validation

} // namespace unum::ustore::utilities
