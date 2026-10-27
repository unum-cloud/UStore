/**
 *  @file src/ram/docs.hpp
 *  @author Ash Vardanian
 *  @date October 25, 2026
 *  @brief The documents calls of the C ABI over the documents stores of `src/ram/database.hpp`, for the in-memory
 *      engine's tables.
 *
 *  Every document is its JSON text, filed under its key in the documents store of its collection. A write passes whole
 *  JSON upserts through, and any other modification reads the documents it touches, applies
 *  `ustore/docs/json_patch.hpp` and writes the new text back, inside one SmashTable transaction: the caller's, or one
 *  of the call's own, retried on conflicts. Reads, gists and gathers take the paths of `src/blobs_layers/docs.hpp`
 *  over the documents as the view sees them. A call naming a collection of another modality is refused.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include "ustore/docs.h"     // `ustore_docs_write_t`, `ustore_docs_read_t`
#include "ustore/ranges.hpp" // `place_batch_t`

#include "blobs_layers/docs.hpp" // `write_documents`, `read_document_fields`, `gist_documents`, `gather_documents`

#include "database.hpp" // `database_state`, `with_source`, `read_into_scratch`, `stage_runs_of`, `serve_read_modify_write`

namespace unum::ustore::ram {

#pragma region Document Calls

/** The documents of the collections a call reads and writes through @p source_type_, recording into @c failure the
 *  first failure of the stores, which a commit retries when it is a conflict. */
template <typename source_type_>
struct documents_in_store {
    source_type_& source;
    store_status_t& failure;

    /** Reads the documents at @p places, appending their text to the scratch of @p view. */
    expected<json_documents_t> read(place_batch_t const& places, view_t const& view) const noexcept {
        json_documents_t documents;
        documents.count = places.count;
        status_t const status = read_into_scratch<document_pair_t>(source, places, view, documents.presences,
                                                                   documents.tape, failure);
        if (status.failed()) return status;
        return documents;
    }

    /** Stages @p values as the whole text of the documents at @p places. */
    status_t write(place_batch_t const& places, strings_arg_t const& values, ustore_durability_t durability,
                   view_t const& view) const noexcept {
        if (status_t const status = validate_write(view.transaction, places, values, durability, view.watch);
            status.failed())
            return status;
        status_t outcome;
        store_status_t const staged = stage_runs_of<document_pair_t>(
            source, places.collections_begin, places.size(), outcome,
            [&](auto& part, std::size_t first, std::size_t end) noexcept {
                return stage_writes<document_pair_t>(part, places, values, first, end);
            });
        if (outcome.failed()) return outcome;
        if (st::failed(staged)) failure = staged;
        return translate(staged);
    }
};

/** Implements @c ustore_docs_write, modifying the documents in place inside one transaction. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_docs_write_t const& arguments,
               strings_arg_t const& fields, strings_arg_t const& values, view_t const& view) noexcept {
    if (!arguments.count) return {};
    return serve_read_modify_write(
        state, arguments.durability, view,
        [&](transaction_source<cpu_class_, isolation_>& source, store_status_t& failure) noexcept {
            documents_in_store<transaction_source<cpu_class_, isolation_>> const documents {source, failure};
            return write_documents(documents, arguments, fields, values, view);
        });
}

/** Implements @c ustore_docs_read, copying whole JSON documents as stored and exporting the fields of any other. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_docs_read_t const& arguments,
               strings_arg_t const& fields, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (reads_whole_documents(arguments, fields)) return read_values<document_pair_t>(state, arguments, view);
    return with_source(state, view, [&]<typename source_type_>(source_type_& source) noexcept -> status_t {
        store_status_t failure = store_status_t::success_k;
        return read_document_fields(documents_in_store<source_type_> {source, failure}, arguments, fields, view);
    });
}

/** Implements @c ustore_docs_gist. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_docs_gist_t const& arguments,
               view_t const& view) noexcept {
    return with_source(state, view, [&]<typename source_type_>(source_type_& source) noexcept -> status_t {
        store_status_t failure = store_status_t::success_k;
        return gist_documents(documents_in_store<source_type_> {source, failure}, arguments, view);
    });
}

/** Implements @c ustore_docs_gather. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_docs_gather_t const& arguments,
               strings_arg_t const& fields, view_t const& view) noexcept {
    return with_source(state, view, [&]<typename source_type_>(source_type_& source) noexcept -> status_t {
        store_status_t failure = store_status_t::success_k;
        return gather_documents(documents_in_store<source_type_> {source, failure}, arguments, fields, view);
    });
}

/** Implements @c ustore_docs_remove, committing a batch outside a transaction atomically. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_docs_remove_t const& arguments,
               view_t const& view) noexcept {
    return remove_keys<document_pair_t>(state, arguments, view);
}

/** Implements @c ustore_docs_scan as ordered walks of each collection's keys. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_docs_scan_t const& arguments,
               view_t const& view) noexcept {
    return scan_keys<document_pair_t>(state, arguments, view);
}

/** Implements @c ustore_docs_sample as a reservoir filled from a walk of each collection's keys. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t serve(database_state<cpu_class_, isolation_>& state, ustore_docs_sample_t const& arguments,
               view_t const& view) noexcept {
    return sample_keys<document_pair_t>(state, arguments, view);
}

#pragma endregion Document Calls

#pragma region Table

template <cpu_class_t cpu_class_, isolation_t isolation_>
docs_verbs<home_t::host_k> const ram_variant<cpu_class_, isolation_>::docs_verbs_k {
    .write = &serve_database<cpu_class_, isolation_, ustore_docs_write_t, strings_arg_t, strings_arg_t>,
    .read = &serve_database<cpu_class_, isolation_, ustore_docs_read_t, strings_arg_t>,
    .gist = &serve_database<cpu_class_, isolation_, ustore_docs_gist_t>,
    .gather = &serve_database<cpu_class_, isolation_, ustore_docs_gather_t, strings_arg_t>,
    .remove = &serve_database<cpu_class_, isolation_, ustore_docs_remove_t>,
    .scan = &serve_database<cpu_class_, isolation_, ustore_docs_scan_t>,
    .sample = &serve_database<cpu_class_, isolation_, ustore_docs_sample_t>,
};

#pragma endregion Table

} // namespace unum::ustore::ram
