/**
 *  @file src/nvme/ingest.hpp
 *  @author Ash Vardanian
 *  @date October 29, 2026
 *  @brief The ingest of <tt>c/ustore/db.h</tt> over the stores of <tt>src/nvme/database.hpp</tt>, for the NVMe
 *      engine's table.
 *
 *  An ingest holds the database's claim from its init until it is freed, and hands the store
 *  its rows as keyed deltas, ascending and unique, which the store writes as runs of stamp
 *  zero and publishes as one commit of its own. Under @c ustore_ingest_sorted_k every append
 *  of blobs, documents, pages or strings is laid out behind the ones before as it arrives,
 *  its keys checked against the last one before them, and a run is written once one fills.
 *  Under @c ustore_ingest_unsorted_k every append is copied and held until the finish, where
 *  StringZilla orders the rows and the last row of a repeated key wins. Pages are filed as
 *  the trees their XML parses into, and edges as two half-edge records each, grouped into one
 *  change delta per vertex at the finish, whichever order was promised, since a vertex is
 *  named by rows anywhere.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcmp`

#include <memory>      // `std::destroy_at`
#include <new>         // placement `new`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`
#include <utility>     // `std::in_place_type`
#include <variant>     // `std::variant`, `std::visit`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <stringzilla/sort.h>          // `sz_strings_view_t`

#include "ustore/db.h" // `ustore_ingest_init_t`, `ustore_ingest_append_t`, `ustore_ingest_finish_t`

#include "ustore/strings/nvme_memory_run.hpp" // `sort_unique_strings`, `keep_t`
#include "ustore/utilities/ram_buffers.hpp"   // `arena_allocate`, `arena_copy`, `arena_allocator`, `growth_status`

#include "ingest_layer.hpp"      // `parse_ingest_values`, `ingest_values_of`
#include "pages_layer.hpp"       // `clock_stamp`
#include "database.hpp"          // `database_state`, `look_up`, `stored_of`, `compact_due`
#include "graph.hpp"             // `vertex_delta_t`, `vertex_chain_t`, `chain_by_vertex`, `deltas_of_edge`
#include "utilities/batches.hpp" // `keyed_rows_t`, `keyed_rows_of`, `ingest_edges_of`

namespace unum::ustore::nvme {

using util::keyed_rows_t;

#pragma region Deltas

/** One row as the store takes it: its key and the delta it lands, viewing bytes its source keeps readable. */
using keyed_delta_t = keyed_delta<stored_key_t>;

/** One row as a strings store takes it: the string, and the 16-byte key it maps onto as the written value. */
using string_delta_t = keyed_delta<std::string_view>;

/** One row of a held stream, which an unsorted ingest orders by key, its value held beside it at the same place. */
struct held_row_t {

    /** The row's key, whose bytes order as the store orders keys. */
    ustore_key_t key;
};

/** The keys of @p rows as strings StringZilla orders, sixteen bytes each, read through an accessor. */
inline sz_strings_view_t keys_of(std::span<held_row_t const> rows) noexcept {
    sz_strings_view_t strings;
    strings.layout = sz_strings_accessor_k;
    strings.count = rows.size();
    strings.data.accessor.handle = rows.data();
    strings.data.accessor.get = [](void const* handle, sz_size_t const* indices, sz_size_t count,
                                   sz_string_view_t* views) {
        held_row_t const* const rows = static_cast<held_row_t const*>(handle);
        for (sz_size_t index = 0; index != count; ++index)
            views[index] = sz_string_view_t {reinterpret_cast<char const*>(rows[indices[index]].key.uuid.bytes),
                                             sizeof(ustore_key_t)};
    };
    return strings;
}

/** The delta of string @p row of @p rows, viewing the string and its key where @p rows keeps them. */
inline string_delta_t string_delta_of(keyed_rows_t const& rows, std::size_t row) noexcept {
    return string_delta_t {
        chars_of(rows.values[row]),
        delta_view_t {delta_kind_t::write_k, bytes_of(rows.keys[row].uuid.bytes, sizeof(ustore_key_t))}};
}

#pragma endregion Deltas

#pragma region Ingest Paths

/** A sequence an ingest keeps from one append to the next, on the arena that holds the ingest
 *  for its life. */
template <typename value_type_>
using kept_vector = st::vector<value_type_, arena_allocator<value_type_>>;

/** A sorted ingest of blobs, documents or pages, every append handed to the store, which lays it out as it arrives. */
struct sorted_keyed_ingest_t {
    explicit sorted_keyed_ingest_t(ingest_values_t values) noexcept : lands_as(values) {}

    /** What the rows land as. */
    ingest_values_t lands_as;

    /** Where the next page parsed is placed, its sequence counting the pages parsed so far. */
    pages::placement_t placement {clock_stamp(), 0};
};

/** An unsorted ingest of blobs, documents or pages, every key and value copied into the
 *  ingest's arena and held until the finish. */
struct held_keyed_ingest_t {
    held_keyed_ingest_t(ingest_values_t values, ustore_arena_t kept) noexcept
        : lands_as(values), kept(kept), keys({kept}), values({kept}) {}

    /** What the rows land as. */
    ingest_values_t lands_as;

    /** Where the next page parsed is placed, its sequence counting the pages parsed so far. */
    pages::placement_t placement {clock_stamp(), 0};

    /** The arena the ingest lives in, which the values are copied into. */
    ustore_arena_t kept;

    /** The keys of every row in the order they came, and their values at the same places. */
    kept_vector<held_row_t> keys;
    kept_vector<std::span<byte_t const>> values;
};

/** A sorted ingest of strings, every append handed to the store, which lays it out as it arrives. */
struct sorted_strings_ingest_t {};

/** An unsorted ingest of strings, every string and its key copied into the ingest's arena and
 *  held until the finish. */
struct held_strings_ingest_t {
    explicit held_strings_ingest_t(ustore_arena_t kept) noexcept : kept(kept), deltas({kept}) {}

    /** The arena the ingest lives in, which the strings and their keys are copied into. */
    ustore_arena_t kept;

    kept_vector<string_delta_t> deltas;
};

/** An ingest of edges in either order, the two half-edge records of every edge held until the finish. */
struct held_edges_ingest_t {
    explicit held_edges_ingest_t(ustore_arena_t kept) noexcept : records({kept}) {}

    kept_vector<vertex_delta_t> records;
};

/** What an ingest holds until the finish, picked at its init by the modality and the order. */
using ingest_path_t = std::variant<sorted_keyed_ingest_t, held_keyed_ingest_t, sorted_strings_ingest_t,
                                   held_strings_ingest_t, held_edges_ingest_t>;

/** The path an ingest of @p modality in @p order takes, keeping what it holds in @p kept. */
inline ingest_path_t ingest_path_of(ustore_modality_t modality, ustore_ingest_order_t order,
                                    ustore_arena_t kept) noexcept {
    bool const sorted = order == ustore_ingest_sorted_k;
    if (modality == ustore_modality_graph_k) return ingest_path_t {std::in_place_type<held_edges_ingest_t>, kept};
    if (modality == ustore_modality_strings_k && sorted)
        return ingest_path_t {std::in_place_type<sorted_strings_ingest_t>};
    if (modality == ustore_modality_strings_k) return ingest_path_t {std::in_place_type<held_strings_ingest_t>, kept};
    if (sorted) return ingest_path_t {std::in_place_type<sorted_keyed_ingest_t>, ingest_values_of(modality)};
    return ingest_path_t {std::in_place_type<held_keyed_ingest_t>, ingest_values_of(modality), kept};
}

/** An ingest under way: its database and collection, the claim and runs the store keeps for it, and its path. */
template <cpu_class_t cpu_class_>
struct ingest_state {
    database_state<cpu_class_>& database;
    ustore_collection_id_t collection;
    typename basic_database_of<cpu_class_>::ingest_t database_ingest;
    ingest_path_t path;

    ~ingest_state() noexcept { database.store->drop_ingest(database_ingest); }
};

/** Hands the rows of a sorted append of blobs, documents or pages to the store, which lays them
 *  out for its runs, their values parsed over the slices of @p view. */
template <cpu_class_t cpu_class_>
status_t write_deltas(ingest_state<cpu_class_>& ingest, sorted_keyed_ingest_t& path, keyed_rows_t const& rows,
                      view_t const& view) noexcept {
    using strings_kit_t = typename nvme_variant<cpu_class_>::strings_kit_t;
    auto [values, values_status] = parse_ingest_values<strings_kit_t>(path.lands_as, rows, path.placement, view);
    if (values_status.failed()) return values_status;
    auto [deltas, deltas_status] = arena_allocate<keyed_delta_t>(view.scratch, rows.size());
    if (deltas_status.failed()) return deltas_status;
    for (std::size_t row = 0; row != rows.size(); ++row)
        new (&deltas[row])
            keyed_delta_t {stored_of(rows.keys[row].uuid), delta_view_t {delta_kind_t::write_k, values[row]}};
    status_t const written = ingest.database.store->write_ingest(
        ingest.database_ingest, std::span<keyed_delta_t const> {deltas}, nvme_queues_of(view));
    if (written.succeeded()) path.placement.sequence += static_cast<std::uint32_t>(rows.size());
    return written;
}

/** Holds the rows of an unsorted append of blobs, documents or pages until the finish, their
 *  values parsed over the slices of @p view and copied. */
template <a_strings_kit strings_kit_type_>
status_t hold_deltas(held_keyed_ingest_t& path, keyed_rows_t const& rows, view_t const& view) noexcept {
    auto [values, values_status] = parse_ingest_values<strings_kit_type_>(path.lands_as, rows, path.placement, view);
    if (values_status.failed()) return values_status;
    std::size_t const before = path.keys.size();
    status_t status = growth_status(path.keys.reserve(before + rows.size()));
    if (status.succeeded()) status = growth_status(path.values.reserve(before + rows.size()));
    for (std::size_t row = 0; status.succeeded() && row != rows.size(); ++row) {
        auto [kept, kept_status] = util::arena_copy<byte_t>(path.kept, values[row]);
        if ((status = kept_status).failed()) break;
        [[maybe_unused]] st::status_t placed = path.keys.push_back(st::assume_reserved, held_row_t {rows.keys[row]});
        placed = path.values.push_back(st::assume_reserved, std::span<byte_t const> {kept});
    }
    while (status.failed() && path.keys.size() != before) path.keys.pop_back(), path.values.pop_back();
    if (status.succeeded()) path.placement.sequence += static_cast<std::uint32_t>(rows.size());
    return status;
}

/** Orders the held rows of @p path by key through @p strings_kit_type_, keeping the last of every
 *  key, as deltas carved in @p scratch. */
template <a_strings_kit strings_kit_type_>
expected<std::span<keyed_delta_t>> order_held_deltas(held_keyed_ingest_t& path, ustore_arena_t scratch) noexcept {
    auto [permutation, permutation_status] = arena_allocate<sz_size_t>(scratch, path.keys.size());
    if (permutation_status.failed()) return permutation_status;
    sz_strings_view_t const keys = keys_of(std::span<held_row_t const> {path.keys});
    if (sz_status_t const sorted = strings_kit_type_::sort_order(&keys, 0, sz_false_k, nullptr, permutation.data(),
                                                                 nullptr);
        sorted != sz_success_k)
        return status_t {sorted == sz_bad_alloc_k ? status_code_t::out_of_memory_k : status_code_t::internal_k,
                         sz_status_name(sorted)};
    // The later of two equal keys is the larger position, whatever the sort's stability.
    std::size_t kept = 0;
    for (std::size_t index = 0; index != permutation.size(); ++index) {
        sz_size_t const given = permutation[index];
        if (kept != 0 &&
            std::memcmp(&path.keys[permutation[kept - 1]].key, &path.keys[given].key, sizeof(ustore_key_t)) == 0) {
            if (given > permutation[kept - 1]) permutation[kept - 1] = given;
            continue;
        }
        permutation[kept++] = given;
    }
    auto [deltas, deltas_status] = arena_allocate<keyed_delta_t>(scratch, kept);
    if (deltas_status.failed()) return deltas_status;
    for (std::size_t index = 0; index != kept; ++index)
        new (&deltas[index]) keyed_delta_t {stored_of(path.keys[permutation[index]].key.uuid),
                                            delta_view_t {delta_kind_t::write_k, path.values[permutation[index]]}};
    return deltas;
}

/** Hands the strings of a sorted append to the store, which lays them out for its runs, their
 *  deltas carved in @p scratch. */
template <cpu_class_t cpu_class_>
status_t write_strings(ingest_state<cpu_class_>& ingest, keyed_rows_t const& rows, ustore_arena_t scratch,
                       nvme_queues_ref_t queues) noexcept {
    auto [deltas, deltas_status] = arena_allocate<string_delta_t>(scratch, rows.size());
    if (deltas_status.failed()) return deltas_status;
    for (std::size_t row = 0; row != rows.size(); ++row) new (&deltas[row]) string_delta_t {string_delta_of(rows, row)};
    return ingest.database.store->write_ingest(ingest.database_ingest, std::span<string_delta_t const> {deltas},
                                               queues);
}

/** Holds the strings of an unsorted append, each string and its key copied, until the finish. */
inline status_t hold_strings(held_strings_ingest_t& path, keyed_rows_t const& rows) noexcept {
    std::size_t const before = path.deltas.size();
    status_t status = growth_status(path.deltas.reserve(before + rows.size()));
    for (std::size_t row = 0; status.succeeded() && row != rows.size(); ++row) {
        string_delta_t const given = string_delta_of(rows, row);
        auto [text, text_status] = util::arena_copy<char>(path.kept, std::span<char const> {given.stored_key});
        auto [key, key_status] = util::arena_copy<byte_t>(path.kept, given.delta.bytes);
        status = text_status.failed() ? text_status : key_status;
        if (status.failed()) break;
        [[maybe_unused]] st::status_t const placed = path.deltas.push_back(
            st::assume_reserved, string_delta_t {std::string_view {text.begin(), text.end()},
                                                 delta_view_t {delta_kind_t::write_k, std::span<byte_t const> {key}}});
    }
    while (status.failed() && path.deltas.size() != before) path.deltas.pop_back();
    return status;
}

/** Holds the two half-edge records of every edge of @p edges into @p collection until the finish. */
inline status_t hold_edges(held_edges_ingest_t& path, ustore_collection_id_t const& collection,
                           util::edges_view_t const& edges) noexcept {
    if (status_t const sized = growth_status(path.records.reserve(path.records.size() + 2 * edges.size()));
        sized.failed())
        return sized;
    for (std::size_t index = 0; index != edges.size(); ++index) {
        util::edge_t const given = edges[index];
        labeled_edge_t edge;
        edge.id = given.id;
        edge.first = given.source;
        edge.second = given.target;
        ustore_key_t ends[2];
        half_edge_delta_t records[2];
        std::size_t const filed = deltas_of_edge(edge, half_edge_operation_t::insert_k, ends, records);
        for (std::size_t end = 0; end != filed; ++end) [[maybe_unused]]
            st::status_t const placed = path.records.push_back(
                st::assume_reserved, vertex_delta_t {collection, ends[end], records[end], path.records.size()});
    }
    return {};
}

/** Writes what an unsorted ingest or a graph held as runs: the rows in key order, the strings in their order, or one
 *  change per vertex holding the half-edges of every edge it ends, chained in @p scratch. */
template <cpu_class_t cpu_class_, typename path_type_>
status_t write_held(ingest_state<cpu_class_>& ingest, path_type_& path, ustore_arena_t scratch,
                    nvme_queues_ref_t queues) noexcept {
    using strings_kit_t = typename nvme_variant<cpu_class_>::strings_kit_t;
    if constexpr (std::is_same_v<path_type_, held_strings_ingest_t>) {
        auto [kept, sort_status] = strings::sort_unique_strings(std::span<string_delta_t> {path.deltas},
                                                                strings_kit_t::sort_order, strings::keep_t::last_k,
                                                                arena_allocator<string_delta_t> {scratch});
        if (sort_status.failed()) return sort_status;
        return ingest.database.store->write_ingest(ingest.database_ingest,
                                                   std::span<string_delta_t const> {path.deltas.data(), kept}, queues);
    }
    else if constexpr (std::is_same_v<path_type_, held_edges_ingest_t>) {
        st::vector<vertex_chain_t, arena_allocator<vertex_chain_t>> chains {{scratch}};
        if (status_t const chained = chain_by_vertex(std::span<vertex_delta_t> {path.records}, scratch, chains);
            chained.failed())
            return chained;
        auto [deltas, deltas_status] = arena_allocate<keyed_delta_t>(scratch, chains.size());
        if (deltas_status.failed()) return deltas_status;
        for (std::size_t index = 0; index != chains.size(); ++index)
            new (&deltas[index]) keyed_delta_t {stored_of(chains[index].vertex.uuid),
                                                delta_view_t {delta_kind_t::change_k, chains[index].records}};
        return ingest.database.store->write_ingest(ingest.database_ingest, std::span<keyed_delta_t const> {deltas},
                                                   queues);
    }
    else if constexpr (std::is_same_v<path_type_, held_keyed_ingest_t>) {
        auto [deltas, order_status] = order_held_deltas<strings_kit_t>(path, scratch);
        if (order_status.failed()) return order_status;
        return ingest.database.store->write_ingest(ingest.database_ingest, std::span<keyed_delta_t const> {deltas},
                                                   queues);
    }
    else return {};
}

/** Implements @c ustore_ingest_init, claiming the database for the ingest's whole life. */
template <cpu_class_t cpu_class_>
status_t ingest_init(ustore_ingest_init_t const& arguments, view_t const& view) noexcept {
    using any_collection_id_t = typename basic_database_of<cpu_class_>::any_collection_id_t;
    database_state<cpu_class_>& database = database_state_of<cpu_class_>(arguments);
    auto [entry, look_up_status] = look_up(database, view.scratch, nvme_queues_of(view), arguments.collection);
    if (look_up_status.failed()) return look_up_status;
    if (entry.modality < ustore_modality_blobs_k || entry.modality > ustore_modality_pages_k)
        return {status_code_t::unsupported_k, "NVMe ingests no such collection"};
    auto [room, room_status] = arena_allocate<ingest_state<cpu_class_>>(&(*arguments.ingest)->kept, 1);
    if (room_status.failed()) return room_status;
    ingest_state<cpu_class_>* const ingest = new (room.data())
        ingest_state<cpu_class_> {.database = database,
                                  .collection = arguments.collection,
                                  .path = ingest_path_of(entry.modality, arguments.order, &(*arguments.ingest)->kept)};
    status_t const begun = database.store->begin_ingest(any_collection_id_t {arguments.collection},
                                                        ingest->database_ingest);
    if (begun.failed()) {
        std::destroy_at(ingest);
        return begun;
    }
    (*arguments.ingest)->state = ingest;
    return {};
}

/** Appends the keyed or string rows of @p arguments to @p ingest along @p path, writing a sorted
 *  append as runs and holding any other until the finish. */
template <cpu_class_t cpu_class_, typename path_type_>
status_t append_rows(ingest_state<cpu_class_>& ingest, path_type_& path, ustore_ingest_append_t const& arguments,
                     strings_arg_t const& values, view_t const& view) noexcept {
    using strings_kit_t = typename nvme_variant<cpu_class_>::strings_kit_t;
    auto [rows, rows_status] = util::keyed_rows_of(arguments, values);
    if (rows_status.failed() || !rows.size()) return rows_status;
    if constexpr (std::is_same_v<path_type_, sorted_keyed_ingest_t>) return write_deltas(ingest, path, rows, view);
    else if constexpr (std::is_same_v<path_type_, held_keyed_ingest_t>)
        return hold_deltas<strings_kit_t>(path, rows, view);
    else if constexpr (std::is_same_v<path_type_, sorted_strings_ingest_t>)
        return write_strings(ingest, rows, view.scratch, nvme_queues_of(view));
    else return hold_strings(path, rows);
}

/** Implements @c ustore_ingest_append, writing a sorted append as runs and holding any other until the finish. */
template <cpu_class_t cpu_class_>
status_t ingest_append(ustore_ingest_append_t const& arguments, strings_arg_t const& values,
                       view_t const& view) noexcept {
    ingest_state<cpu_class_>& ingest = *state_of<ingest_state<cpu_class_>>(arguments.ingest);
    return std::visit(
        [&]<typename path_type_>(path_type_& path) noexcept -> status_t {
            if constexpr (std::is_same_v<path_type_, held_edges_ingest_t>) {
                auto [edges, edges_status] = util::ingest_edges_of(arguments);
                if (edges_status.failed()) return edges_status;
                return hold_edges(path, ingest.collection, edges);
            }
            else return append_rows(ingest, path, arguments, values, view);
        },
        ingest.path);
}

/** Implements @c ustore_ingest_finish, writing what was held and publishing every run as one commit. */
template <cpu_class_t cpu_class_>
status_t ingest_finish(ustore_ingest_finish_t const& arguments, view_t const& view) noexcept {
    ingest_state<cpu_class_>& ingest = *state_of<ingest_state<cpu_class_>>(arguments.ingest);
    status_t const wrote = std::visit(
        [&]<typename path_type_>(path_type_& path) noexcept {
            return write_held(ingest, path, view.scratch, nvme_queues_of(view));
        },
        ingest.path);
    if (wrote.failed()) return wrote;
    ingest_outcome_t const landed = ingest.database.store->publish_ingest(ingest.database_ingest, arguments.durability,
                                                                          nvme_queues_of(view));
    if (landed.stamp == invalid_transaction_stamp_k) return landed.status;
    if (arguments.commit_stamp) *arguments.commit_stamp = landed.stamp;
    // The ingest has landed, so a compaction it left due is met again by the next write making room
    [[maybe_unused]] status_t const compacted = compact_due(ingest.database, view);
    return landed.status;
}

/** Implements @c ustore_ingest_free, dropping every run never published and giving the claim back. */
template <cpu_class_t cpu_class_>
void ingest_free(ustore_ingest_t handle) noexcept {
    std::destroy_at(state_of<ingest_state<cpu_class_>>(handle));
}

#pragma endregion Ingest Paths

} // namespace unum::ustore::nvme
