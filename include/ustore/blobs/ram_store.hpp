/**
 *  @file include/ustore/blobs/ram_store.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The multi-version stores of the RAM engine on SmashTable: one per collection of the kind its modality picks,
 *      the collections and the names system collections over them, and the transaction group spanning any of them under
 *      one stamp.
 *
 *  Every store is a partitioned AVL tree of versions, each partition locked by a spin lock owning its word through the
 *  atomic reference and stalling through the waiter every store here takes. A blobs, documents or pages collection
 *  maps UUID keys onto values, a strings collection and the names map strings onto 16-byte keys held inline, and a
 *  graph collection holds half-edges with no values.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`

#include <new>         // `std::nothrow`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`
#include <variant>     // `std::in_place_type`, `std::variant`

#include <smashtable/basic_avl_tree.hpp>
#include <smashtable/basic_vector.hpp>
#include <smashtable/partitioned_store.hpp>
#include <smashtable/snapshot_store.hpp>

#include "ustore/graph/half_edges.hpp"    // `half_edge_t`, `half_edge_order_t`, `half_edge_hash_t`
#include "ustore/strings/string_keys.hpp" // `blob_t`, `string_key_t`, `string_key_order_t`, `string_key_hash_t`
#include "ustore/utilities/types.hpp"     // `status_t`, `value_view_t`, `uuid_order_t`, `uuid_hash_t`

namespace unum::ustore::blobs {

using strings::blob_t;
using strings::string_key_hash_t;
using strings::string_key_order_t;
using strings::string_key_t;
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

/** A value filed under its key. */
using value_pair_t = st::mapping<ustore_key_t, blob_t>;

/** A string and the 16-byte key it maps onto, held inline. */
using string_pair_t = st::mapping<string_key_t, ustore_key_t>;

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

/** A collection of blobs, documents or pages: every key mapped onto the bytes of its value. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_>
using values_by_key_store =
    ordered_store<value_pair_t, uuid_order_t, uuid_hash_t, isolation_, atomic_reference_, waiter_type_>;

/** A strings collection, and the names system collection, which maps names onto collection IDs. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_>
using keys_by_string_store =
    ordered_store<string_pair_t, string_key_order_t, string_key_hash_t, isolation_, atomic_reference_, waiter_type_>;

/** A graph collection: every vertex once and every edge twice, under each of its vertices, with no values. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_>
using half_edges_store = ordered_store<graph::half_edge_t, graph::half_edge_order_t, graph::half_edge_hash_t,
                                       isolation_, atomic_reference_, waiter_type_>;

/** The order every store of a database commits in, so one stamp spans a commit reaching any number of them: the
 *  shared default of SmashTable, which the sixteen partitions of every store embed whatever lock they take. */
using order_t = st::commit_order_t;

/** A claim on the stamp a call, a snapshot or a transaction reads at, which keeps every store it may reach alive. */
using claim_t = order_t::snapshot_claim_t;

template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_>
struct collection_store;

/** What the collections system collection records of a collection: what it holds, its name, and the store its
 *  entries live in. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_>
struct collection_entry {

    /** What the collection holds, which picked the kind of its store. */
    ustore_modality_t modality {};

    /** The name of the collection, empty for one created without. */
    blob_t name;

    /** The store holding the collection's entries. */
    collection_store<isolation_, atomic_reference_, waiter_type_>* store {nullptr};
};

/** A collection's UUID mapped onto what the collections system collection records of it. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_>
using collections_pair =
    st::mapping<ustore_collection_id_t, collection_entry<isolation_, atomic_reference_, waiter_type_>>;

/** The collections system collection: every collection's UUID mapped onto its entry. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_>
using collections_by_id_store = ordered_store<collections_pair<isolation_, atomic_reference_, waiter_type_>,
                                              uuid_order_t, uuid_hash_t, isolation_, atomic_reference_, waiter_type_>;

/** The store of one collection, of the kind its modality picks, which a drop retires rather than frees. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_>
struct collection_store {

    /** The collection's entries. */
    std::variant<values_by_key_store<isolation_, atomic_reference_, waiter_type_>,
                 keys_by_string_store<isolation_, atomic_reference_, waiter_type_>,
                 half_edges_store<isolation_, atomic_reference_, waiter_type_>>
        entries;

    /** The store retired before this one, while this one waits to be freed. */
    collection_store* next_retired {nullptr};

    /** The stamp the change that took this store out of the collections system collection published under. */
    generation_t retirement_stamp {0};

    template <typename kind_type_>
    collection_store(std::in_place_type_t<kind_type_> kind, order_t& order) noexcept : entries(kind, order) {}
};

/** A transaction over both system collections and every collection store it reaches, published under one stamp. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_>
using collections_transaction =
    st::dynamic_transaction_group<collections_by_id_store<isolation_, atomic_reference_, waiter_type_>,
                                  values_by_key_store<isolation_, atomic_reference_, waiter_type_>,
                                  keys_by_string_store<isolation_, atomic_reference_, waiter_type_>,
                                  half_edges_store<isolation_, atomic_reference_, waiter_type_>>;

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

/** The key @p text is stored under, owning a copy of its bytes. */
inline st::expected<string_key_t> string_key_from(std::string_view text) noexcept {
    auto [copied, copy_blob_status] = copy_blob(value_view_t {text});
    if (st::failed(copy_blob_status)) return copy_blob_status;
    return string_key_t {std::move(copied)};
}

/** Allocates the store a collection holding @p modality keeps its entries in, or answers null. */
template <isolation_t isolation_, template <typename> typename atomic_reference_, typename waiter_type_>
collection_store<isolation_, atomic_reference_, waiter_type_>* allocate_store(order_t& order,
                                                                              ustore_modality_t modality) noexcept {
    using store_t = collection_store<isolation_, atomic_reference_, waiter_type_>;
    if (modality == ustore_modality_strings_k)
        return new (std::nothrow)
            store_t(std::in_place_type<keys_by_string_store<isolation_, atomic_reference_, waiter_type_>>, order);
    if (modality == ustore_modality_graph_k)
        return new (std::nothrow)
            store_t(std::in_place_type<half_edges_store<isolation_, atomic_reference_, waiter_type_>>, order);
    return new (std::nothrow)
        store_t(std::in_place_type<values_by_key_store<isolation_, atomic_reference_, waiter_type_>>, order);
}

} // namespace unum::ustore::blobs
