/**
 *  @file src/nvme/docs.hpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The document writes of the C ABI over the docs stores of
 *      <tt>include/ustore/docs/nvme_store.hpp</tt>, for the NVMe engine's table.
 *
 *  These calls build on the accesses, the writes and the collection lookups of
 *  <tt>src/nvme/database.hpp</tt>. A write addressing any collection but a docs one is refused. An
 *  upsert of a whole document writes it, JSON as written and any other type printed as JSON, and
 *  reads nothing. An update, an insertion, a patch, a merge, and an upsert of a field each append
 *  one @c json_delta_t to the document's change delta, after replaying it over the document as
 *  the transaction sees it, read and watched. A modification that would fail answers as the
 *  read-modify-write of <tt>src/docs_layer.hpp</tt> does and never lands, and a commit that changed the
 *  document since conflicts, so no change delta carries a @c json_delta_t that fails.
 *
 *  The tasks of one call on one document apply in task order, each seeing the ones before it, and a call whose tasks
 *  leave a document known whole, past a whole upsert or over a document of no bytes, writes that document, since a
 *  change delta over no bytes would fold as over a missing document. Reads take the read paths of
 *  <tt>src/docs_layer.hpp</tt> over the engine's own reads of the docs store, which fold the deltas.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <span> // `std::span`

#include "ustore/docs.h"     // `ustore_docs_write_t`
#include "ustore/ranges.hpp" // `place_batch_t`, `strided`

#include "ustore/docs/json_fold.hpp"        // `json_fold_t`, `json_delta_t`, `encode_deltas`, `encoded_size`
#include "ustore/docs/json_values.hpp"      // `extract_keys`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`

#include "docs_layer.hpp" // `document_deltas_t`, `group_document_deltas`

#include "database.hpp" // `call_access`, `resolve_runs`, `read_value`, `write_value`, `serve_write`

namespace unum::ustore::nvme {

using docs::json_delta_t;
using docs::json_fold_t;

/** The fold every change delta of a docs collection is written for. */
inline constexpr json_fold_t docs_fold_k {};

/**
 *  @brief Replays the deltas of @p document one by one over the document as @p access sees it, failing as the first
 *      failing one does, and writes them: the document they leave when it is known whole, or one change delta.
 *
 *  Stamps the deltas with the transaction's start, which the document they were checked against was read at.
 */
template <cpu_class_t cpu_class_>
status_t write_document(call_access<cpu_class_> const& access, document_deltas_t const& document) noexcept {
    collection_id_t const collection = document.collection;
    stored_key_t const key = stored_of(document.key.uuid);

    // A present document of no bytes still views a byte, so the replay tells it from a missing one, while a change
    // delta over it would fold over no bytes, which is a missing document, so its changes write it whole
    docs::document_state_t replayed;
    bool whole = document.writes_whole();
    if (document.reads_base()) {
        auto [current, read_status] = read_value(access, collection, key);
        if (read_status.failed()) return read_status;
        if (current)
            replayed.bytes = current.size() ? std::span<byte_t const> {current.begin(), current.size()}
                                            : std::span<byte_t const> {empty_value_bytes_k, 0};
        whole = whole || (current && current.empty());
    }
    for (json_delta_t& delta : document.deltas) delta.stamp = access.transaction->start_stamp();
    if (status_t const applied = replay_deltas(replayed, document.deltas, access.scratch); applied.failed())
        return applied;
    if (whole) return write_value(access, collection, key, value_view_t {replayed.bytes});
    auto [chain, chain_status] = docs::encode_deltas(document.deltas, access.scratch);
    if (chain_status.failed()) return chain_status;
    return access.transaction->change(collection, key, chain, docs_fold_k);
}

/** Implements @c ustore_docs_write, appending the deltas of a write addressing only docs collections. */
template <cpu_class_t cpu_class_>
status_t docs_write(ustore_docs_write_t const& arguments, strings_arg_t const& fields, strings_arg_t const& values,
                    view_t const& view) noexcept {
    std::size_t const count = arguments.count;
    if (!count) return {};
    if (!values) return {status_code_t::invalid_argument_k, "No values were provided"};
    std::string_view const id_field = string_of(arguments.id_field);
    if (!arguments.keys.data && id_field.empty())
        return {status_code_t::invalid_argument_k, "Documents without keys need an id field"};
    database_state<cpu_class_>& state = database_state_of<cpu_class_>(arguments);
    if (status_t const resolved = resolve_runs(state, view, strided(arguments.collections), count,
                                               ustore_modality_docs_k);
        resolved.failed())
        return resolved;
    if (arguments.modification > ustore_docs_modify_merge_k)
        return {status_code_t::invalid_argument_k, "Unknown modification"};

    ustore_arena_t const scratch = view.scratch;
    place_batch_t places = places_of(arguments);
    if (!arguments.keys.data) {
        auto [extracted, extract_status] = arena_allocate<ustore_key_t>(scratch, count);
        if (extract_status.failed()) return extract_status;
        if (status_t const extracting = docs::extract_keys(values, id_field, scratch, extracted); extracting.failed())
            return extracting;
        places.keys_begin = {extracted.data(), sizeof(ustore_key_t)};
    }
    if (status_t const valid = validate_write(view.transaction, places, values, arguments.durability, view.watch);
        valid.failed())
        return valid;

    auto [documents, grouped_status] = group_document_deltas(arguments, places, fields, values, scratch);
    if (grouped_status.failed()) return grouped_status;
    std::size_t bytes = 0;
    bool reads = false;
    for (document_deltas_t const& document : documents) {
        reads = reads || document.reads_base();
        for (json_delta_t const& delta : document.deltas) bytes += docs::encoded_size(delta);
    }

    // A transaction watches the documents it checks the deltas against, whatever the caller asked of the writes
    return serve_write(state, view, arguments.durability, documents.size(), bytes,
                       reads ? watch_schedule_t::on_commit_k : watch_schedule_t::never_k,
                       [&](call_access<cpu_class_> const& access) noexcept -> status_t {
                           for (document_deltas_t const& document : documents)
                               if (status_t const written = write_document(access, document); written.failed())
                                   return written;
                           return {};
                       });
}

} // namespace unum::ustore::nvme
