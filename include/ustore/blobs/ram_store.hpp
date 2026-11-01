/**
 *  @file include/ustore/blobs/ram_store.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The multi-version store of the RAM engine on SmashTable, the blobs collection over it,
 *      the owned texts and trees the documents and pages collections file under their keys, and the
 *      status translation every store shares.
 *
 *  Every store is a partitioned AVL tree of versions, each partition locked by a spin lock owning its word through the
 *  atomic reference and stalling through the waiter every store here takes. A blobs collection maps UUID keys onto
 *  values; each other modality's @c ram_store.hpp instantiates the same store over its own entries.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`

#include <utility> // `std::move`

#include <smashtable/basic_avl_tree.hpp>
#include <smashtable/basic_vector.hpp>
#include <smashtable/partitioned_store.hpp>
#include <smashtable/snapshot_store.hpp>

#include "ustore/utilities/types.hpp" // `status_t`, `blob_t`, `value_view_t`, `uuid_order_t`, `uuid_hash_t`

namespace unum::ustore::blobs {

using utilities::blob_t;
using utilities::status_code_t;
using utilities::status_t;
using utilities::uuid_hash_t;
using utilities::uuid_order_t;
using utilities::value_view_t;

/** The status every SmashTable call answers with. */
using store_status_t = st::status_t;

/** The isolation level a store instantiation promises. */
using isolation_t = st::isolation_t;

/** Whether a walk carries on after an element. */
using walk_control_t = st::walk_control_t;

/** Whether a transaction's writes sit in the store, invisible. */
using staging_t = st::staging_t;

/** A stamp of the commit order: what a snapshot reads at, and what a commit publishes under. */
using generation_t = st::generation_t;

/** The JSON text of a document, owning its bytes, apart from a blob so a documents store is a type of its own. */
struct document_text_t {

    /** Bytes of the JSON text. */
    blob_t text;

    /** A copy owning bytes of its own, or why they could not be allocated. */
    st::expected<document_text_t> copy() const noexcept {
        st::expected<blob_t> copied = text.copy();
        if (!copied) return copied.status();
        return document_text_t {std::move(*copied)};
    }
};

/** The dense element tree of a page, owning its bytes, apart from a blob so a pages store is a type of its own. */
struct page_tree_bytes_t {

    /** Bytes of the encoded tree. */
    blob_t tree;

    /** A copy owning bytes of its own, or why they could not be allocated. */
    st::expected<page_tree_bytes_t> copy() const noexcept {
        st::expected<blob_t> copied = tree.copy();
        if (!copied) return copied.status();
        return page_tree_bytes_t {std::move(*copied)};
    }
};

/** The bytes a blob, a document or a page holds. */
inline blob_t const& blob_of(blob_t const& value) noexcept { return value; }
inline blob_t const& blob_of(document_text_t const& document) noexcept { return document.text; }
inline blob_t const& blob_of(page_tree_bytes_t const& page) noexcept { return page.tree; }

/** A value filed under its key. */
using value_pair_t = st::mapping<ustore_key_t, blob_t>;

/** The lock each partition takes, owning its word through @p atomic_reference_, stalling through @p waiter_type_ and
 *  then parking. */
template <template <typename> typename atomic_reference_, typename waiter_type_>
using engine_mutex = st::spin_shared_mutex<st::standard_waiting_policy<waiter_type_>, atomic_reference_>;

/** A multi-version store of @p entry_type_ ordered by @p order_type_ at @p isolation_, in partitions @p hash_type_
 *  picks, built into the order of its database. */
template <typename entry_type_, typename order_type_, typename hash_type_, isolation_t isolation_,
          template <typename> typename atomic_reference_, typename waiter_type_>
using ordered_store = st::partitioned_store<
    st::snapshot_store<st::basic_avl_tree<entry_type_, order_type_, st::default_allocator<entry_type_>>, isolation_>,
    hash_type_, engine_mutex<atomic_reference_, waiter_type_>>;

/** A blobs collection: every key mapped onto the bytes of its value. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_>
using values_by_key_store =
    ordered_store<value_pair_t, uuid_order_t, uuid_hash_t, isolation_, atomic_reference_, waiter_type_>;

/** The order every store of a database commits in, so one stamp spans a commit reaching any number of them: the
 *  shared default of SmashTable, which the sixteen partitions of every store embed whatever lock they take. */
using order_t = st::commit_order_t;

/** A claim on the stamp a call, a snapshot or a transaction reads at, which keeps every store it may reach alive. */
using claim_t = order_t::snapshot_claim_t;

/** How many times an engine-internal commit is attempted before its conflicts reach the caller. */
inline constexpr std::size_t commit_attempts_k = 16;

/** The status of the C ABI a caller reads for @p status. */
constexpr status_t translate(store_status_t status) noexcept {
    switch (status) {
    case store_status_t::success_k: return {};
    case store_status_t::unknown_k: return {status_code_t::internal_k, "SmashTable failed for an unknown reason"};
    case store_status_t::consistency_k: return {status_code_t::internal_k, "SmashTable found a staged version missing"};
    case store_status_t::write_conflict_k:
        return {status_code_t::write_conflict_k, "Write conflict: a written key was committed over"};
    case store_status_t::read_conflict_k:
        return {status_code_t::read_conflict_k, "Read conflict: a read key was committed over"};
    case store_status_t::phantom_conflict_k:
        return {status_code_t::phantom_conflict_k, "Phantom conflict: a walked range gained or lost a key"};
    case store_status_t::out_of_memory_heap_k: return {status_code_t::out_of_memory_k, "Out of memory"};
    case store_status_t::invalid_argument_k: return {status_code_t::invalid_argument_k, "Invalid argument"};
    case store_status_t::operation_not_permitted_k:
        return {status_code_t::invalid_argument_k, "Operation not permitted in the transaction's state"};
    case store_status_t::operation_would_block_k:
        return {status_code_t::write_conflict_k, "Gave up after every retry met a conflict"};
    case store_status_t::capacity_exhausted_k: return {status_code_t::capacity_exhausted_k, "Capacity exhausted"};
    case store_status_t::input_output_k: return {status_code_t::io_k, "Input or output failed"};
    case store_status_t::key_already_exists_k: return {status_code_t::already_exists_k, "Key already exists"};
    case store_status_t::key_not_found_k: return {status_code_t::not_found_k, "Key not found"};
    }
    return {status_code_t::internal_k, "SmashTable answered an unrecognized status"};
}

/** The status a call answers: the failure @p outcome recorded first, or else @p status. */
inline status_t reported(store_status_t status, status_t const& outcome) noexcept {
    return outcome.failed() ? outcome : translate(status);
}

/** Refuses a call with @p message, answering the status that stops the walk it is in. */
inline store_status_t refused(status_t& outcome, char const* message) noexcept {
    outcome = {status_code_t::invalid_argument_k, message};
    return store_status_t::invalid_argument_k;
}

/** Copies @p value into a blob the store owns. */
inline st::expected<blob_t> copy_blob(value_view_t value) noexcept {
    blob_t blob;
    if (st::failed(blob.resize(value.size()))) return store_status_t::out_of_memory_heap_k;
    if (value.size() != 0) std::memcpy(blob.data(), value.data(), value.size());
    return blob;
}

/** Views the bytes of a stored blob. */
inline value_view_t view_of(blob_t const& blob) noexcept {
    return value_view_t {blob.data(), static_cast<ustore_length_t>(blob.size())};
}

} // namespace unum::ustore::blobs
