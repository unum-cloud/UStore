/**
 *  @file src/ram/ingest.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief Implements the ingest of <tt>c/ustore/db.h</tt> for the in-memory engine: every row appended copied into
 *      the entries of its collection, then bucketed by partition, sorted, and published as one commit.
 *
 *  Every append copies its rows into the store's own entry type. Finishing buckets the entries by the partition owning
 *  their keys in one stable counting pass, sorts each bucket unless the ingest promised its order, keeps the last row
 *  of a repeated key, and stages every bucket whole into its partition of one transaction, which draws one stamp. A
 *  graph files every edge as its two half-edges and both of its vertices, and a page the tree its XML parses into.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <algorithm>   // `std::is_sorted`, `std::stable_sort`
#include <array>       // `std::array`
#include <iterator>    // `std::make_move_iterator`
#include <new>         // `std::nothrow`
#include <type_traits> // `std::is_same_v`
#include <utility>     // `std::move`
#include <variant>     // `std::variant`, `std::get_if`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/db.h" // `ustore_ingest_init_t`, `ustore_ingest_append_t`, `ustore_ingest_finish_t`

#include "ustore/docs/ram_store.hpp"        // `document_pair_t`
#include "ustore/graph/half_edges.hpp"      // `half_edge_t`, `half_edge_direction_t`
#include "ustore/pages/ram_store.hpp"       // `page_pair_t`
#include "ustore/strings/ram_store.hpp"     // `string_pair_t`, `string_key_from`
#include "ustore/utilities/ram_buffers.hpp" // `growth_status`

#include "database.hpp"          // `database_state`, `transaction_source`, `for_each_run`, `persist`
#include "ingest_layer.hpp"      // `ingest_value_parser`, `ingest_values_t`
#include "utilities/batches.hpp" // `keyed_rows_t`, `keyed_rows_of`, `ingest_edges_of`

namespace unum::ustore::ram {

#pragma region Ingest

using utilities::keyed_rows_t;

/** The entries an ingest gathered for one collection, in the order of their rows. */
template <typename entry_type_>
using ingested_entries = st::vector<entry_type_>;

/** The entries of whichever kind of store an ingest's collection lives in, in the order @c collection_store lists. */
using any_ingested_entries_t =
    std::variant<ingested_entries<value_pair_t>, ingested_entries<string_pair_t>, ingested_entries<half_edge_t>,
                 ingested_entries<docs::document_pair_t>, ingested_entries<pages::page_pair_t>>;

/** What an ingest lands as the values of entries of @p entry_type_. */
template <typename entry_type_>
inline constexpr ingest_values_t ingest_values_for = std::is_same_v<entry_type_, docs::document_pair_t>
                                                         ? ingest_values_t::documents_k
                                                     : std::is_same_v<entry_type_, pages::page_pair_t>
                                                         ? ingest_values_t::pages_k
                                                         : ingest_values_t::bytes_k;

/** Whether @p first orders strictly before @p second, as a store of @p store_type_ orders its entries. */
template <typename store_type_, typename entry_type_>
bool precedes(entry_type_ const& first, entry_type_ const& second) noexcept {
    typename store_type_::comparator_t const order;
    return order(st::mapping_key_or_itself(first), st::mapping_key_or_itself(second));
}

/** What an ingest refuses a row for when its key does not strictly ascend under @c ustore_ingest_sorted_k. */
inline constexpr status_t descending_key_k {status_code_t::invalid_argument_k,
                                            "A key descends or repeats in a stream ingested as sorted"};

/** Appends the entries of @p rows to @p entries, as a store of @p store_type_ files them, parsing every document and
 *  page through @p value_parser, and refusing a key that does not ascend when @p order promises it. */
template <typename store_type_, strings_kit_concept strings_kit_type_>
status_t gather_rows(keyed_rows_t const& rows, ustore_ingest_order_t order,
                     ingested_entries<typename store_type_::value_t>& entries,
                     ingest_value_parser<strings_kit_type_>& value_parser) noexcept {
    using entry_t = typename store_type_::value_t;
    if (status_t const reserved = growth_status(entries.reserve(entries.size() + rows.size())); reserved.failed())
        return reserved;
    for (std::size_t row = 0; row != rows.size(); ++row) {
        entry_t entry;
        if constexpr (std::is_same_v<entry_t, string_pair_t>) {
            auto [key, key_status] = string_key_from(string_of(rows.values[row]));
            if (st::failed(key_status)) return translate(key_status);
            entry = entry_t {std::move(key), rows.keys[row]};
        }
        else {
            value_parser.rewind();
            auto [value, value_status] = value_parser(rows, row);
            if (value_status.failed()) return value_status;
            auto [copied, copy_status] = copy_blob(value_view_t {value});
            if (st::failed(copy_status)) return translate(copy_status);
            entry = entry_t {rows.keys[row], typename entry_t::mapped_type {std::move(copied)}};
        }
        if (order == ustore_ingest_sorted_k && !entries.empty() && !precedes<store_type_>(entries.back(), entry))
            return descending_key_k;
        [[maybe_unused]] st::status_t const pushed = entries.push_back(st::assume_reserved, std::move(entry));
    }
    return {};
}

/** Appends four entries per edge of @p edges to @p entries, as a graph store of @p store_type_ files them: both
 *  vertices, then the half-edge under the source and the one under the target, ordered by the half-edges under their
 *  sources when @p order promises it. */
template <typename store_type_>
status_t gather_edges(edges_view_t const& edges, ustore_ingest_order_t order,
                      ingested_entries<half_edge_t>& entries) noexcept {
    constexpr std::size_t per_edge_k = 4;
    if (status_t const reserved = growth_status(entries.reserve(entries.size() + edges.size() * per_edge_k));
        reserved.failed())
        return reserved;
    for (std::size_t index = 0; index != edges.size(); ++index) {
        edge_t const edge = edges[index];
        half_edge_t const outgoing {edge.source, half_edge_direction_t::outgoing_k, edge.target, edge.id};
        if (order == ustore_ingest_sorted_k && entries.size() >= per_edge_k &&
            !precedes<store_type_>(entries[entries.size() - 2], outgoing))
            return descending_key_k;
        [[maybe_unused]] st::status_t pushed = entries.push_back(st::assume_reserved,
                                                                 half_edge_t::vertex_entry(edge.source));
        pushed = entries.push_back(st::assume_reserved, half_edge_t::vertex_entry(edge.target));
        pushed = entries.push_back(st::assume_reserved, half_edge_t {outgoing});
        pushed = entries.push_back(st::assume_reserved,
                                   half_edge_t {edge.target, half_edge_direction_t::incoming_k, edge.source, edge.id});
    }
    return {};
}

/** Where every partition's entries start in a bucketed run, and one past the last, closing the final partition. */
template <typename store_type_>
using partition_offsets = std::array<std::size_t, store_type_::partitions_k + 1>;

/** Moves @p entries into @p bucketed partition by partition, in one stable counting pass, so each partition's
 *  entries keep the order of their rows. */
template <typename store_type_>
status_t bucket_entries(store_type_ const& store, ingested_entries<typename store_type_::value_t>& entries,
                        ingested_entries<typename store_type_::value_t>& bucketed,
                        partition_offsets<store_type_>& offsets) noexcept {
    offsets.fill(0);
    for (typename store_type_::value_t const& entry : entries) ++offsets[store.partition_of(entry) + 1];
    for (std::size_t partition = 0; partition != store_type_::partitions_k; ++partition)
        offsets[partition + 1] += offsets[partition];
    if (status_t const resized = growth_status(bucketed.resize(entries.size())); resized.failed()) return resized;
    partition_offsets<store_type_> cursors = offsets;
    for (typename store_type_::value_t& entry : entries)
        bucketed[cursors[store.partition_of(entry)]++] = std::move(entry);
    entries.clear();
    return {};
}

/** Sorts the entries of one partition, unless they already ascend, and drops every entry a later one of the same
 *  key repeats, answering where the kept entries end. */
template <typename store_type_>
std::size_t settle_partition(ingested_entries<typename store_type_::value_t>& bucketed, std::size_t first,
                             std::size_t end) noexcept {
    using entry_t = typename store_type_::value_t;
    entry_t* const begin = bucketed.data();
    auto const ordered = [](entry_t const& left, entry_t const& right) noexcept {
        return precedes<store_type_>(left, right);
    };
    // A stable sort keeps a repeated key's rows in their order, so the last of each run is the last row
    if (!std::is_sorted(begin + first, begin + end, ordered)) std::stable_sort(begin + first, begin + end, ordered);
    std::size_t kept = first;
    for (std::size_t index = first; index != end; ++index) {
        if (index + 1 != end && !ordered(begin[index], begin[index + 1])) continue;
        if (kept != index) begin[kept] = std::move(begin[index]);
        ++kept;
    }
    return kept;
}

/** Lands @p entries in @p store through @p part, its partition of the ingest's transaction, sorting each partition
 *  unless @p order promised it, but a graph's, whose vertices repeat between its half-edges. */
template <typename store_type_, typename part_type_>
status_t stage_ingest(store_type_& store, part_type_& part, ustore_ingest_order_t order,
                      ingested_entries<typename store_type_::value_t>& entries) noexcept {
    using entry_t = typename store_type_::value_t;
    if (entries.empty()) return {};
    ingested_entries<entry_t> bucketed;
    partition_offsets<store_type_> offsets;
    if (status_t const bucketed_status = bucket_entries(store, entries, bucketed, offsets); bucketed_status.failed())
        return bucketed_status;
    bool const sorts = order == ustore_ingest_unsorted_k || std::is_same_v<entry_t, half_edge_t>;
    for (std::size_t partition = 0; partition != store_type_::partitions_k; ++partition) {
        std::size_t const first = offsets[partition];
        std::size_t const end = sorts ? settle_partition<store_type_>(bucketed, first, offsets[partition + 1])
                                      : offsets[partition + 1];
        if (first == end) continue;
        entry_t* const begin = bucketed.data();
        store_status_t const staged = part.upsert_partition(partition, std::make_move_iterator(begin + first),
                                                            std::make_move_iterator(begin + end), st::assume_sorted);
        if (st::failed(staged)) return translate(staged);
    }
    return {};
}

/** An ingest under way: its database, collection and order, and every entry appended so far. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
struct ingest_state {
    database_state<cpu_class_, isolation_>& database;
    ustore_collection_id_t collection;
    ustore_ingest_order_t order;
    ingest_value_parser<strings_kit_of<cpu_class_>> value_parser;
    any_ingested_entries_t entries;
};

/** The status a collection answers once its store holds another kind of entries than the ingest gathered. */
inline status_t replaced_collection() noexcept {
    return {status_code_t::not_found_k, "The collection was dropped while the ingest ran"};
}

/** Hands @p action the store @p collection of @p database lives in at the head. */
template <cpu_class_t cpu_class_, isolation_t isolation_, typename action_type_>
status_t with_ingested_store(database_state<cpu_class_, isolation_>& database, ustore_collection_id_t const& collection,
                             view_t const& view, action_type_&& action) noexcept {
    status_t outcome;
    store_status_t const visited = with_source(database, view, [&](auto& source) noexcept {
        return for_each_run(source, strided_iterator<ustore_collection_id_t const> {&collection, 0}, 1, outcome,
                            [&]<typename store_type_>(store_type_& store, std::size_t, std::size_t) noexcept {
                                outcome = action(store);
                                return outcome.failed() ? store_status_t::invalid_argument_k
                                                        : store_status_t::success_k;
                            });
    });
    return reported(visited, outcome);
}

/** Implements @c ustore_ingest_init, checking the collection exists and noting the kind of its entries. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t ingest_init(ustore_ingest_init_t const& arguments, view_t const& view) noexcept {
    database_state<cpu_class_, isolation_>& database = *state_of<database_state<cpu_class_, isolation_>>(
        arguments.database);
    any_ingested_entries_t entries;
    ingest_values_t values = ingest_values_t::bytes_k;
    status_t const opened = with_ingested_store(database, arguments.collection, view,
                                                [&]<typename store_type_>(store_type_&) noexcept -> status_t {
                                                    entries.template emplace<ingested_entries<entry_of<store_type_>>>();
                                                    values = ingest_values_for<entry_of<store_type_>>;
                                                    return {};
                                                });
    if (opened.failed()) return opened;
    ingest_state<cpu_class_, isolation_>* const ingest = new (std::nothrow) ingest_state<cpu_class_, isolation_> {
        database, arguments.collection, arguments.order, ingest_value_parser<strings_kit_of<cpu_class_>> {values},
        std::move(entries)};
    if (!ingest) return {status_code_t::out_of_memory_k, "Failed to allocate the ingest"};
    (*arguments.ingest)->state = ingest;
    return {};
}

/** Implements @c ustore_ingest_append, copying the rows into the entries, which a failure leaves as they were. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t ingest_append(ustore_ingest_append_t const& arguments, strings_arg_t const& values,
                       view_t const& view) noexcept {
    ingest_state<cpu_class_, isolation_>& ingest = *state_of<ingest_state<cpu_class_, isolation_>>(arguments.ingest);
    return with_ingested_store(
        ingest.database, ingest.collection, view, [&]<typename store_type_>(store_type_&) noexcept -> status_t {
            using entry_t = entry_of<store_type_>;
            ingested_entries<entry_t>* const entries = std::get_if<ingested_entries<entry_t>>(&ingest.entries);
            if (!entries) return replaced_collection();
            std::size_t const before = entries->size();
            status_t gathered;
            if constexpr (std::is_same_v<entry_t, half_edge_t>) {
                auto [edges, edges_status] = utilities::ingest_edges_of(arguments);
                gathered = edges_status.failed() ? edges_status
                                                 : gather_edges<store_type_>(edges, ingest.order, *entries);
            }
            else {
                auto [rows, rows_status] = utilities::keyed_rows_of(arguments, values);
                gathered = rows_status.failed()
                               ? rows_status
                               : gather_rows<store_type_>(rows, ingest.order, *entries, ingest.value_parser);
            }
            while (gathered.failed() && entries->size() != before) entries->pop_back();
            return gathered;
        });
}

/** Implements @c ustore_ingest_finish as one transaction of its own over the collection's store, published under one
 *  stamp and persisted once when flushed. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
status_t ingest_finish(ustore_ingest_finish_t const& arguments, view_t const&) noexcept {
    ingest_state<cpu_class_, isolation_>& ingest = *state_of<ingest_state<cpu_class_, isolation_>>(arguments.ingest);
    auto opened = collections_transaction_of<cpu_class_, isolation_>::make(ingest.database.order);
    if (!opened) return translate(opened.status());
    transaction_source<cpu_class_, isolation_> source {ingest.database, *opened, ustore_watch_none_k};
    status_t outcome;
    store_status_t const landed = for_each_run(
        source, strided_iterator<ustore_collection_id_t const> {&ingest.collection, 0}, 1, outcome,
        [&]<typename store_type_>(store_type_& store, std::size_t, std::size_t) noexcept -> store_status_t {
            ingested_entries<entry_of<store_type_>>* const entries =
                std::get_if<ingested_entries<entry_of<store_type_>>>(&ingest.entries);
            if (!entries) outcome = replaced_collection();
            else if (auto part = source.part(store); !part) return part.status();
            else outcome = stage_ingest(store, **part, ingest.order, *entries);
            return outcome.failed() ? store_status_t::invalid_argument_k : store_status_t::success_k;
        });
    if (status_t const status = reported(landed, outcome); status.failed()) return status;

    store_status_t committed = opened->stage();
    if (st::succeeded(committed)) committed = opened->commit();
    if (st::failed(committed)) return translate(committed);
    if (arguments.commit_stamp) *arguments.commit_stamp = opened->commit_stamp();
    if (arguments.durability == ustore_durability_flushed_k) return persist(ingest.database);
    return {};
}

/** Implements @c ustore_ingest_free, dropping every entry the ingest still holds. */
template <cpu_class_t cpu_class_, isolation_t isolation_>
void ingest_free(ustore_ingest_t handle) noexcept {
    delete state_of<ingest_state<cpu_class_, isolation_>>(handle);
}

#pragma endregion Ingest

} // namespace unum::ustore::ram
