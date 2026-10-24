/**
 *  @file src/nvme/docs_store.hpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The document writes of the C ABI over the docs stores of `include/ustore/docs/nvme_docs.hpp`, for the NVMe
 *      engine's table.
 *
 *  `src/nvme/variants.hpp` includes this file right before its table, as these calls build on its accesses, its
 *  writes and its catalog lookups. In a write addressing only docs collections, an upsert of a whole document writes
 *  it, JSON as written and any other type printed as JSON, and reads nothing. An update, an insertion, a patch, a
 *  merge, and an upsert of a field each append one @c json_fold delta to the document's change record, after
 *  replaying it over the document as the transaction sees it, read and watched. A modification that would fail
 *  answers as the read-modify-write of `src/docs.cpp` does and never lands, and a commit that changed the document
 *  since conflicts, so no record holds a delta that fails.
 *
 *  The tasks of one call on one document apply in task order, each seeing the ones before it, and a call whose tasks
 *  leave a document known whole, past a whole upsert or over a document of no bytes, writes that document, since a
 *  change record over no bytes would fold as over a missing document. A write addressing any other collection,
 *  the main one included, takes the read-modify-write of `src/docs.cpp` whole, as RAM does. Reads stay with
 *  `src/docs.cpp`, which reads through the engine's own blobs reads, folding the records.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <algorithm> // `std::sort`
#include <span>      // `std::span`

#include "ustore/docs.h"     // `ustore_docs_write_t`
#include "ustore/ranges.hpp" // `place_batch_t`, `strided`

#include "ustore/docs/json_fold.hpp"        // `json_fold`, `json_delta_t`, `encode_deltas`, `modifier_text`
#include "ustore/docs/json_values.hpp"      // `json_parse`, `extract_keys`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_allocator`

namespace unum::ustore::nvme {

using docs::delta_modification_t;
using docs::json_delta_t;
using docs::json_fold;

/** The fold every change record of a docs collection is written for. */
inline constexpr json_fold docs_fold_k {};

/** Whether every task of @p collections names a collection the catalog lists as holding @p modality, as @p view
 *  reads it, looking each run of tasks on one collection up once; a collection the catalog misses answers false, so
 *  the shared path refuses it as it would on any engine. */
template <cpu_class_t cpu_class_>
bool holds_only(database_state<cpu_class_>& state, view_t const& view,
                strided_iterator<ustore_collection_id_t const> collections, std::size_t count,
                ustore_modality_t modality) noexcept {
    auto const admits = [modality](ustore_modality_t held) noexcept -> status_t {
        if (held != modality) return {status_code_t::invalid_argument_k, "The collection holds another modality"};
        return {};
    };
    return resolve_runs(state, view, collections, count, admits).succeeded();
}

/** One task of a docs write: the document it addresses, its delta, and its place in the call, which orders the deltas
 *  of one document. */
struct document_task_t {
    ustore_collection_id_t collection;
    ustore_key_t key;
    json_delta_t delta;
    std::size_t sequence = 0;
};

/** The deltas one docs write makes to one document, in task order. */
struct document_deltas_t {
    ustore_collection_id_t collection;
    ustore_key_t key;
    std::span<json_delta_t> deltas;

    /** Whether the deltas depend on the document before them, which only a leading whole write makes irrelevant. */
    [[nodiscard]] bool reads_base() const noexcept {
        return deltas.front().modification != delta_modification_t::write_k;
    }

    /** Whether the document the deltas leave is known whole, past a whole write among them. */
    [[nodiscard]] bool writes_whole() const noexcept {
        for (json_delta_t const& delta : deltas)
            if (delta.modification == delta_modification_t::write_k) return true;
        return false;
    }
};

/**
 *  @brief Replays the deltas of @p document one by one over the document as @p access sees it, failing as the first
 *      failing one does, and stages them: the document they leave when it is known whole, or one change record.
 *
 *  Stamps the deltas with the transaction's start, which the document they were checked against was read at.
 */
template <cpu_class_t cpu_class_>
status_t stage_document(call_access<cpu_class_> const& access, document_deltas_t const& document) noexcept {
    collection_id_t const collection = document.collection;
    stored_key_t const key = stored_of(document.key.uuid);

    // A present document of no bytes still views a byte, so the replay tells it from a missing one, while a change
    // record over it would fold over no bytes, which is a missing document, so its changes write it whole
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
    for (json_delta_t& delta : document.deltas) {
        delta.stamp = access.transaction->start_stamp();
        if (status_t const applied = docs::apply_delta(replayed, delta, access.scratch); applied.failed())
            return applied;
    }
    if (whole) return write_value(access, collection, key, value_view_t {replayed.bytes});
    auto [chain, chain_status] = docs::encode_deltas(document.deltas, access.scratch);
    if (chain_status.failed()) return chain_status;
    return access.transaction->change(collection, key, chain, docs_fold_k);
}

/** Implements @c ustore_docs_write, appending the deltas of a write addressing only docs collections and taking the
 *  read-modify-write of `src/docs.cpp` for any other. */
template <cpu_class_t cpu_class_>
status_t docs_write(ustore_docs_write_t const& arguments, strings_arg_t const& fields, strings_arg_t const& values,
                    view_t const& view) noexcept {
    std::size_t const count = arguments.count;
    if (!count) return {};
    if (!values) return {status_code_t::invalid_argument_k, "No values were provided"};
    std::string_view const id_field = string_of(arguments.id_field);
    if (!arguments.keys.data && id_field.empty())
        return {status_code_t::invalid_argument_k, "Documents without keys need an id field"};
    database_state<cpu_class_>& state = state_of<cpu_class_>(arguments);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    if (!holds_only(state, view, collections, count, ustore_modality_docs_k))
        return docs_write_in_json_values(arguments, fields, values, view);
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

    auto [tasks, tasks_status] = arena_allocate<document_task_t>(scratch, count);
    if (tasks_status.failed()) return tasks_status;
    std::size_t bytes = 0;
    for (std::size_t index = 0; index != count; ++index) {
        std::string_view const field = fields ? string_of(fields[index]) : std::string_view {};
        value_view_t const value {values[index]};
        json_delta_t delta {0, static_cast<delta_modification_t>(arguments.modification), field, {}};
        // A whole upsert writes the document, which a whole JSON upsert passes through once it parses
        if (field.empty() && delta.modification == delta_modification_t::upsert_k) {
            delta.modification = delta_modification_t::write_k;
            if (arguments.type == ustore_docs_field_type_json_k)
                if (status_t const parsed = docs::json_parse(value, scratch).status(); parsed.failed()) return parsed;
        }
        auto [text, text_status] = docs::modifier_text(value, arguments.type, scratch);
        if (text_status.failed()) return text_status;
        delta.value = text;
        bytes += docs::encoded_size(delta);
        tasks[index] = document_task_t {places[index].collection, places[index].key, delta, index};
    }
    std::sort(tasks.begin(), tasks.end(), [](document_task_t const& first, document_task_t const& second) noexcept {
        if (first.collection != second.collection) return first.collection < second.collection;
        if (first.key != second.key) return first.key < second.key;
        return first.sequence < second.sequence;
    });

    // The deltas of every document sit side by side in task order, which each document's span views
    auto [deltas, deltas_status] = arena_allocate<json_delta_t>(scratch, count);
    if (deltas_status.failed()) return deltas_status;
    auto [documents, documents_status] = arena_allocate<document_deltas_t>(scratch, count);
    if (documents_status.failed()) return documents_status;
    std::size_t documents_count = 0;
    bool reads = false;
    for (std::size_t first = 0; first != count;) {
        std::size_t end = first + 1;
        while (end != count && tasks[end].collection == tasks[first].collection && tasks[end].key == tasks[first].key)
            ++end;
        for (std::size_t index = first; index != end; ++index) deltas[index] = tasks[index].delta;
        document_deltas_t const document {tasks[first].collection, tasks[first].key,
                                          deltas.subspan(first, end - first)};
        reads = reads || document.reads_base();
        documents[documents_count++] = document;
        first = end;
    }

    // A transaction watches the documents it checks the deltas against, whatever the caller asked of the writes
    return serve_write(state, view, arguments.durability, documents_count, bytes,
                       reads ? watch_schedule_t::on_commit_k : watch_schedule_t::never_k,
                       [&](call_access<cpu_class_> const& access) noexcept -> status_t {
                           for (std::size_t index = 0; index != documents_count; ++index)
                               if (status_t const staged = stage_document(access, documents[index]); staged.failed())
                                   return staged;
                           return {};
                       });
}

} // namespace unum::ustore::nvme
