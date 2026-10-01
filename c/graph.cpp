/**
 *  @file c/graph.cpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief The graph modality over the reads and writes of any engine, storing every vertex as one value of its
 *      degrees and sorted neighborships.
 *
 *  A vertex value holds the outgoing and the incoming degree, then the outgoing neighborships sorted by
 *  target, then the incoming ones sorted by source, each ordered by the bytes of its neighbor and then of
 *  its edge identifier:
 *
 *  @code{.c}
 *  struct vertex {
 *      uint32_t outgoing_degree;         // offset 0
 *      uint32_t incoming_degree;         // offset 4
 *      struct {
 *          ustore_key_t neighbor_id;     // offset 0 of the neighborship
 *          ustore_key_t edge_id;         // offset 16 of the neighborship
 *      } neighborships[];                // offset 8, 32 bytes each, outgoing then incoming
 *  };
 *  @endcode
 */

#include <cstddef> // `std::ptrdiff_t`, `std::size_t`
#include <cstring> // `std::memcpy`

#include <algorithm>  // `std::ranges::binary_search`, `std::ranges::equal_range`, `std::ranges::partition`
#include <compare>    // `std::strong_ordering`
#include <functional> // `std::ranges::less`
#include <iterator>   // `std::random_access_iterator_tag`
#include <limits>     // `std::numeric_limits`
#include <optional>   // `std::optional`
#include <span>       // `std::span`

#include "ustore/ustore.hpp" // `ustore_read`, `strided_range`, `embedded_blobs_t`

#include "engine.hpp"                // `graph_find_edges_in_vertex_values`
#include "helpers/algorithm.hpp"     // `offset_in_sorted`
#include "helpers/linked_memory.hpp" // `linked_memory_lock_t`

ustore_key_t const ustore_default_edge_id_k = {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}};
ustore_vertex_degree_t const ustore_vertex_degree_missing_k = std::numeric_limits<ustore_vertex_degree_t>::max();

namespace unum::ustore {

constexpr std::size_t bytes_in_degrees_header_k = 2 * sizeof(ustore_vertex_degree_t);

/** The identifiers an edge export writes for every neighborship, next to the degree of every vertex. */
enum class edge_export_t { degrees_k, neighbors_k, edges_k };

/** Whether an update of neighborhoods inserts edges or erases them. */
enum class neighborhood_update_t { upsert_k, erase_k };

/** A vertex value being rewritten, together with how many neighborships the update changes. */
struct updated_entry_t : public collection_key_t {

    /** Bytes of the vertex value, degrees first. */
    ustore_bytes_ptr_t content = nullptr;

    /** Length of the value, or @c ustore_length_missing_k for a missing vertex. */
    ustore_length_t length = ustore_length_missing_k;

    /** Neighborships the update adds or removes. */
    ustore_vertex_degree_t degree_delta = 0;

    operator value_view_t() const noexcept { return {content, length}; }
};

/** How many identifiers an export writes per neighborship. */
constexpr std::size_t ids_per_neighborship(edge_export_t export_kind) noexcept {
    switch (export_kind) {
    case edge_export_t::degrees_k: return 0;
    case edge_export_t::neighbors_k: return 1;
    case edge_export_t::edges_k: return 3;
    }
    return 0;
}

/** Reads the degree at @p index of the header of @p value, which starts wherever its blob does. */
ustore_vertex_degree_t degree_of(ustore_bytes_cptr_t value, std::size_t index) noexcept {
    ustore_vertex_degree_t degree;
    std::memcpy(&degree, value + index * sizeof(ustore_vertex_degree_t), sizeof(ustore_vertex_degree_t));
    return degree;
}

/** Writes @p degree at @p index of the header of @p value. */
void set_degree(ustore_bytes_ptr_t value, std::size_t index, ustore_vertex_degree_t degree) noexcept {
    std::memcpy(value + index * sizeof(ustore_vertex_degree_t), &degree, sizeof(ustore_vertex_degree_t));
}

/** Walks the neighborships of a value, loading each one through @c std::memcpy because the value carries no
 *  alignment of its own. */
class neighborship_iterator_t {

    ustore_bytes_cptr_t bytes_ {nullptr};

  public:
    using value_type = neighborship_t;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::random_access_iterator_tag;
    using iterator_category = std::input_iterator_tag;

    neighborship_iterator_t() = default;
    explicit neighborship_iterator_t(ustore_bytes_cptr_t bytes) noexcept : bytes_(bytes) {}

    /** The bytes the iterator points at. */
    ustore_bytes_cptr_t bytes() const noexcept { return bytes_; }

    neighborship_t operator*() const noexcept {
        neighborship_t neighborship;
        std::memcpy(&neighborship, bytes_, sizeof(neighborship_t));
        return neighborship;
    }

    neighborship_t operator[](difference_type offset) const noexcept { return *(*this + offset); }

    neighborship_iterator_t& operator++() noexcept { return *this += 1; }
    neighborship_iterator_t& operator--() noexcept { return *this -= 1; }

    neighborship_iterator_t operator++(int) noexcept {
        neighborship_iterator_t const previous = *this;
        ++*this;
        return previous;
    }

    neighborship_iterator_t operator--(int) noexcept {
        neighborship_iterator_t const previous = *this;
        --*this;
        return previous;
    }

    neighborship_iterator_t& operator+=(difference_type offset) noexcept {
        bytes_ += offset * static_cast<difference_type>(sizeof(neighborship_t));
        return *this;
    }

    neighborship_iterator_t& operator-=(difference_type offset) noexcept { return *this += -offset; }

    friend neighborship_iterator_t operator+(neighborship_iterator_t iterator, difference_type offset) noexcept {
        return iterator += offset;
    }

    friend neighborship_iterator_t operator+(difference_type offset, neighborship_iterator_t iterator) noexcept {
        return iterator += offset;
    }

    friend neighborship_iterator_t operator-(neighborship_iterator_t iterator, difference_type offset) noexcept {
        return iterator -= offset;
    }

    friend difference_type operator-(neighborship_iterator_t first, neighborship_iterator_t second) noexcept {
        return (first.bytes_ - second.bytes_) / static_cast<difference_type>(sizeof(neighborship_t));
    }

    friend bool operator==(neighborship_iterator_t, neighborship_iterator_t) = default;
    friend std::strong_ordering operator<=>(neighborship_iterator_t, neighborship_iterator_t) = default;
};

/** A run of neighborships inside one vertex value, addressed by bytes rather than by typed pointers. */
class neighborships_view_t {

    neighborship_iterator_t begin_ {};
    std::size_t count_ {0};

  public:
    neighborships_view_t() = default;
    neighborships_view_t(ustore_bytes_cptr_t bytes, std::size_t count) noexcept : begin_(bytes), count_(count) {}

    neighborship_iterator_t begin() const noexcept { return begin_; }
    neighborship_iterator_t end() const noexcept { return begin_ + static_cast<std::ptrdiff_t>(count_); }

    /** The first byte of the run, from which an offset inside the value is measured. */
    ustore_bytes_cptr_t bytes() const noexcept { return begin_.bytes(); }

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }
};

/** The neighborships of a vertex value in @p role, empty for a missing vertex. */
neighborships_view_t neighbors(value_view_t value, ustore_vertex_role_t role = ustore_vertex_role_any_k) noexcept {
    if (value.size() < bytes_in_degrees_header_k) return {};

    auto const header = reinterpret_cast<ustore_bytes_cptr_t>(value.begin());
    ustore_bytes_cptr_t const first = header + bytes_in_degrees_header_k;
    std::size_t const outgoing = degree_of(header, 0);
    std::size_t const incoming = degree_of(header, 1);
    switch (role) {
    case ustore_vertex_source_k: return {first, outgoing};
    case ustore_vertex_target_k: return {first + outgoing * sizeof(neighborship_t), incoming};
    case ustore_vertex_role_any_k: return {first, outgoing + incoming};
    case ustore_vertex_role_unknown_k: return {};
    }
    return {};
}

/** Whether @p value is missing, a bare vertex, or degrees followed by exactly the neighborships they count. */
bool is_vertex_value(value_view_t value) noexcept {
    if (value.empty()) return true;
    if (value.size() < bytes_in_degrees_header_k) return false;

    auto const header = reinterpret_cast<ustore_bytes_cptr_t>(value.begin());
    std::size_t const neighborships_count = std::size_t {degree_of(header, 0)} + degree_of(header, 1);
    return value.size() - bytes_in_degrees_header_k == neighborships_count * sizeof(neighborship_t);
}

/** Counts a neighborship towards the growth of @p entry unless the entry already holds it. */
void count_inserts_into_entry( //
    updated_entry_t& entry, ustore_vertex_role_t role, ustore_key_t neighbor_id, ustore_key_t edge_id) noexcept {

    neighborship_t const neighborship {neighbor_id, edge_id};
    if (entry.length > bytes_in_degrees_header_k && std::ranges::binary_search(neighbors(entry, role), neighborship))
        return;
    ++entry.degree_delta;
}

/** Inserts a neighborship into @p entry, whose buffer already has room for it, unless the entry holds it. */
void insert_into_entry( //
    updated_entry_t& entry, ustore_vertex_role_t role, ustore_key_t neighbor_id, ustore_key_t edge_id) noexcept {

    neighborship_t const neighborship {neighbor_id, edge_id};
    auto const inserted = reinterpret_cast<ustore_bytes_cptr_t>(&neighborship);
    ustore_bytes_ptr_t const header = entry.content;
    ustore_bytes_ptr_t const neighborships = header + bytes_in_degrees_header_k;

    if (entry.length < bytes_in_degrees_header_k || entry.length == ustore_length_missing_k) {
        set_degree(header, role != ustore_vertex_target_k, 0);
        set_degree(header, role == ustore_vertex_target_k, 1);
        std::memcpy(neighborships, inserted, sizeof(neighborship_t));
        entry.length += bytes_in_degrees_header_k;
        entry.length += sizeof(neighborship_t);
        return;
    }

    neighborships_view_t const existing = neighbors(entry, role);
    auto const position = std::ranges::lower_bound(existing, neighborship);
    if (position != existing.end() && *position == neighborship) return;

    std::size_t const role_index = role == ustore_vertex_target_k;
    std::size_t const stored = std::size_t {degree_of(header, 0)} + degree_of(header, 1);
    auto const offset = static_cast<std::size_t>(existing.bytes() - neighborships +
                                                 (position - existing.begin()) * sizeof(neighborship_t));
    trivial_insert(neighborships, stored * sizeof(neighborship_t), offset, inserted, inserted + sizeof(neighborship_t));
    set_degree(header, role_index, degree_of(header, role_index) + 1);
    entry.length += sizeof(neighborship_t);
}

/** Erases from @p entry the neighborships with @p neighbor_id, or only the one with @p edge_id when it is given. */
void erase_from_entry( //
    updated_entry_t& entry, ustore_vertex_role_t role, ustore_key_t neighbor_id,
    std::optional<ustore_key_t> edge_id = {}) noexcept {

    if (entry.length < bytes_in_degrees_header_k || entry.length == ustore_length_missing_k) return;

    ustore_bytes_ptr_t const header = entry.content;
    ustore_bytes_ptr_t const neighborships = header + bytes_in_degrees_header_k;
    neighborships_view_t const existing = neighbors(entry, role);
    auto const matches = edge_id ? std::ranges::equal_range(existing, neighborship_t {neighbor_id, *edge_id})
                                 : std::ranges::equal_range(existing, neighbor_id, std::ranges::less {},
                                                            &neighborship_t::neighbor_id);
    if (matches.empty()) return;

    std::size_t const role_index = role == ustore_vertex_target_k;
    std::size_t const stored = std::size_t {degree_of(header, 0)} + degree_of(header, 1);
    auto const offset = static_cast<std::size_t>(existing.bytes() - neighborships +
                                                 (matches.begin() - existing.begin()) * sizeof(neighborship_t));
    std::size_t const count = matches.size();
    trivial_erase(neighborships, stored * sizeof(neighborship_t), offset, count * sizeof(neighborship_t));
    set_degree(header, role_index, degree_of(header, role_index) - static_cast<ustore_vertex_degree_t>(count));
    entry.degree_delta += static_cast<ustore_vertex_degree_t>(count);
    entry.length -= static_cast<ustore_length_t>(sizeof(neighborship_t) * count);
}

/** Reads the vertices and exports their degrees, and their neighbors or edges when @p export_kind_ asks for them. */
template <edge_export_t export_kind_>
void export_edge_tuples( //
    ustore_database_t const database, ustore_transaction_t const transaction, ustore_snapshot_t const snapshot,
    ustore_size_t const vertices_count, ustore_collection_id_t const* collections,
    ustore_size_t const collections_stride, ustore_key_t const* vertices, ustore_size_t const vertices_stride,
    ustore_vertex_role_t const* roles, ustore_size_t const roles_stride, ustore_options_t const options,
    ustore_vertex_degree_t** degrees_per_vertex, ustore_key_t** neighborships_per_vertex, linked_memory_lock_t& arena,
    ustore_error_t* error) noexcept {

    // Degrees come from the values, while lengths tell a missing vertex apart from a bare one
    ustore_bytes_ptr_t found_values {};
    ustore_length_t* found_offsets {};
    ustore_length_t* found_lengths {};
    ustore_read_t read {};
    read.db = database;
    read.error = error;
    read.transaction = transaction;
    read.snapshot = snapshot;
    read.arena = arena;
    read.options = options;
    read.tasks_count = vertices_count;
    read.collections = collections;
    read.collections_stride = collections_stride;
    read.keys = vertices;
    read.keys_stride = vertices_stride;
    read.offsets = &found_offsets;
    read.lengths = &found_lengths;
    read.values = &found_values;

    ustore_read(&read);
    if (error->status) return;

    embedded_blobs_t const values {vertices_count, found_offsets, found_lengths, found_values};
    find_edges_t const find_edges {
        {collections, collections_stride},
        {vertices, vertices_stride},
        {roles, roles_stride},
        vertices_count,
    };
    constexpr std::size_t ids_per_neighborship_k = ids_per_neighborship(export_kind_);

    std::size_t ids_count = 0;
    for (std::size_t index = 0; index != vertices_count; ++index) {
        if (!is_vertex_value(values[index])) {
            *error = {ustore_error_malformed_k, "A vertex holds a value that is not a graph vertex"};
            return;
        }
        ids_count += neighbors(values[index], find_edges[index].role).size();
    }
    ids_count *= ids_per_neighborship_k;

    auto ids = arena.alloc_or_dummy(ids_count, error, neighborships_per_vertex);
    if (error->status) return;
    auto degrees = arena.alloc_or_dummy(vertices_count, error, degrees_per_vertex);
    if (error->status) return;

    std::size_t exported_ids = 0;
    for (std::size_t index = 0; index != vertices_count; ++index) {
        value_view_t const value = values[index];
        find_edge_t const find_edge = find_edges[index];

        if (!value) {
            degrees[index] = ustore_vertex_degree_missing_k;
            continue;
        }

        ustore_vertex_degree_t degree = 0;
        if (find_edge.role & ustore_vertex_source_k) {
            neighborships_view_t const outgoing = neighbors(value, ustore_vertex_source_k);
            if constexpr (export_kind_ == edge_export_t::edges_k) {
                for (neighborship_t const neighborship : outgoing) {
                    ids[exported_ids] = find_edge.vertex_id;
                    ids[exported_ids + 1] = neighborship.neighbor_id;
                    ids[exported_ids + 2] = neighborship.edge_id;
                    exported_ids += ids_per_neighborship_k;
                }
            }
            else if constexpr (export_kind_ == edge_export_t::neighbors_k) {
                for (neighborship_t const neighborship : outgoing) ids[exported_ids++] = neighborship.neighbor_id;
            }
            degree += static_cast<ustore_vertex_degree_t>(outgoing.size());
        }
        if (find_edge.role & ustore_vertex_target_k) {
            neighborships_view_t const incoming = neighbors(value, ustore_vertex_target_k);
            if constexpr (export_kind_ == edge_export_t::edges_k) {
                for (neighborship_t const neighborship : incoming) {
                    ids[exported_ids] = neighborship.neighbor_id;
                    ids[exported_ids + 1] = find_edge.vertex_id;
                    ids[exported_ids + 2] = neighborship.edge_id;
                    exported_ids += ids_per_neighborship_k;
                }
            }
            else if constexpr (export_kind_ == edge_export_t::neighbors_k) {
                for (neighborship_t const neighborship : incoming) ids[exported_ids++] = neighborship.neighbor_id;
            }
            degree += static_cast<ustore_vertex_degree_t>(incoming.size());
        }
        degrees[index] = degree;
    }
}

/** Reads the current values of @p unique_entries and points every entry at its own bytes. */
void pull_and_link_for_updates( //
    ustore_database_t const database, ustore_transaction_t const transaction,
    strided_range<updated_entry_t> unique_entries, ustore_options_t const options, linked_memory_lock_t& arena,
    ustore_error_t* error) noexcept {

    ustore_bytes_ptr_t found_values = nullptr;
    ustore_length_t* found_offsets = nullptr;
    ustore_length_t* found_lengths = nullptr;
    auto const unique_count = static_cast<ustore_size_t>(unique_entries.size());
    auto const collections = unique_entries.immutable().members(&updated_entry_t::collection);
    auto const keys = unique_entries.immutable().members(&updated_entry_t::key);

    // A transaction watches the vertices it rewrites, whatever the caller asked for its reads
    ustore_options_t const read_options = transaction
                                              ? ustore_options_t(options & ~ustore_option_transaction_dont_watch_k)
                                              : options;

    ustore_read_t read {};
    read.db = database;
    read.error = error;
    read.transaction = transaction;
    read.arena = arena;
    read.options = read_options;
    read.tasks_count = unique_count;
    read.collections = collections.begin().get();
    read.collections_stride = collections.stride();
    read.keys = keys.begin().get();
    read.keys_stride = keys.stride();
    read.offsets = &found_offsets;
    read.lengths = &found_lengths;
    read.values = &found_values;

    ustore_read(&read);
    if (error->status) return;

    embedded_blobs_t const found {unique_count, found_offsets, found_lengths, found_values};
    for (std::size_t index = 0; index != unique_count; ++index) {
        value_view_t const value = found[index];
        if (!is_vertex_value(value)) {
            *error = {ustore_error_malformed_k, "A vertex holds a value that is not a graph vertex"};
            return;
        }
        unique_entries[index].content = ustore_bytes_ptr_t(value.data());
        unique_entries[index].length = value ? static_cast<ustore_length_t>(value.size()) : ustore_length_missing_k;
    }
}

/** Inserts or erases edges in the neighborships of both their ends, and writes every touched vertex back. */
template <neighborhood_update_t update_>
void update_neighborhoods( //
    ustore_database_t const database, ustore_transaction_t const transaction, ustore_size_t const tasks_count,
    ustore_collection_id_t const* collections, ustore_size_t const collections_stride, ustore_key_t const* edges_ids,
    ustore_size_t const edges_stride, ustore_key_t const* sources_ids, ustore_size_t const sources_stride,
    ustore_key_t const* targets_ids, ustore_size_t const targets_stride, ustore_options_t const options,
    linked_memory_lock_t& arena, ustore_error_t* error) noexcept {

    strided_iterator<ustore_collection_id_t const> const edge_collections {collections, collections_stride};
    strided_iterator<ustore_key_t const> const edges {edges_ids, edges_stride};
    strided_iterator<ustore_key_t const> const sources {sources_ids, sources_stride};
    strided_iterator<ustore_key_t const> const targets {targets_ids, targets_stride};

    // Both ends of every edge, deduplicated, so each touched vertex is read and written once
    std::span<updated_entry_t> unique_entries = arena.alloc<updated_entry_t>(tasks_count * 2, error);
    if (error->status) return;
    std::ranges::fill(unique_entries, updated_entry_t {});
    for (std::size_t index = 0; index != tasks_count; ++index) {
        unique_entries[index].collection = edge_collections[index];
        unique_entries[index].key = sources[index];
        unique_entries[tasks_count + index].collection = edge_collections[index];
        unique_entries[tasks_count + index].key = targets[index];
    }

    std::size_t const unique_count = sort_and_deduplicate(unique_entries.begin(), unique_entries.end());
    unique_entries = unique_entries.first(unique_count);

    strided_range<updated_entry_t> const unique_strided = strided(unique_entries);
    pull_and_link_for_updates(database, transaction, unique_strided, options, arena, error);
    if (error->status) return;

    auto const for_each_task = [&](auto update_entry) {
        for (std::size_t index = 0; index != tasks_count; ++index) {
            ustore_collection_id_t const collection = edge_collections[index];
            ustore_key_t const source_id = sources[index];
            ustore_key_t const target_id = targets[index];
            ustore_key_t const edge_id = edges ? edges[index] : ustore_key_unknown_k;
            std::size_t const source_offset = offset_in_sorted(unique_entries,
                                                               collection_key_t {collection, source_id});
            std::size_t const target_offset = offset_in_sorted(unique_entries,
                                                               collection_key_t {collection, target_id});
            update_entry(unique_entries[source_offset], ustore_vertex_source_k, target_id, edge_id);
            update_entry(unique_entries[target_offset], ustore_vertex_target_k, source_id, edge_id);
        }
    };

    if constexpr (update_ == neighborhood_update_t::erase_k) { for_each_task(&erase_from_entry); }
    else {
        // Count the growth of every vertex, reallocate each buffer once, then insert in place
        for_each_task(&count_inserts_into_entry);
        for (std::size_t index = 0; index != unique_count; ++index) {
            updated_entry_t& entry = unique_entries[index];
            std::size_t const bytes_present = entry.length != ustore_length_missing_k ? entry.length : 0;
            std::size_t const bytes_for_relations = entry.degree_delta * sizeof(neighborship_t);
            std::size_t const bytes_for_degrees = bytes_present > bytes_in_degrees_header_k ? 0
                                                                                            : bytes_in_degrees_header_k;
            std::span<byte_t> const buffer = arena.alloc<byte_t>(
                bytes_present + bytes_for_relations + bytes_for_degrees, error);
            if (error->status) return;
            if (bytes_present) std::memcpy(buffer.data(), entry.content, bytes_present);
            entry.content = reinterpret_cast<ustore_bytes_ptr_t>(buffer.data());
            entry.length = static_cast<ustore_length_t>(bytes_present);
        }
        for_each_task(&insert_into_entry);
    }

    // Vertices the update left unchanged move to the back and are not written
    auto const unchanged = std::ranges::partition(
        unique_entries, [](ustore_vertex_degree_t delta) { return delta != 0; }, &updated_entry_t::degree_delta);
    auto const changed_count = static_cast<ustore_size_t>(unchanged.begin() - unique_entries.begin());
    if (!changed_count) return;

    auto const written_collections = unique_strided.immutable().members(&updated_entry_t::collection);
    auto const written_keys = unique_strided.immutable().members(&updated_entry_t::key);
    auto const written_contents = unique_strided.immutable().members(&updated_entry_t::content);
    auto const written_lengths = unique_strided.immutable().members(&updated_entry_t::length);

    ustore_write_t write {};
    write.db = database;
    write.error = error;
    write.transaction = transaction;
    write.arena = arena;
    write.options = options;
    write.tasks_count = changed_count;
    write.collections = written_collections.begin().get();
    write.collections_stride = written_collections.stride();
    write.keys = written_keys.begin().get();
    write.keys_stride = written_keys.stride();
    write.lengths = written_lengths.begin().get();
    write.lengths_stride = written_lengths.stride();
    write.values = written_contents.begin().get();
    write.values_stride = written_contents.stride();

    ustore_write(&write);
}

void graph_find_edges_in_vertex_values(ustore_graph_find_edges_t& arguments) noexcept {
    if (!arguments.tasks_count) return;

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    if (arguments.edges_per_vertex)
        export_edge_tuples<edge_export_t::edges_k>( //
            arguments.db, arguments.transaction, arguments.snapshot, arguments.tasks_count, arguments.collections,
            arguments.collections_stride, arguments.vertices, arguments.vertices_stride, arguments.roles,
            arguments.roles_stride, arguments.options, arguments.degrees_per_vertex, arguments.edges_per_vertex, arena,
            arguments.error);
    else
        export_edge_tuples<edge_export_t::degrees_k>( //
            arguments.db, arguments.transaction, arguments.snapshot, arguments.tasks_count, arguments.collections,
            arguments.collections_stride, arguments.vertices, arguments.vertices_stride, arguments.roles,
            arguments.roles_stride, arguments.options, arguments.degrees_per_vertex, arguments.edges_per_vertex, arena,
            arguments.error);
}

void graph_upsert_edges_in_vertex_values(ustore_graph_upsert_edges_t& arguments) noexcept {
    if (!arguments.tasks_count) return;

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    update_neighborhoods<neighborhood_update_t::upsert_k>( //
        arguments.db, arguments.transaction, arguments.tasks_count, arguments.collections, arguments.collections_stride,
        arguments.edges_ids, arguments.edges_stride, arguments.sources_ids, arguments.sources_stride,
        arguments.targets_ids, arguments.targets_stride, arguments.options, arena, arguments.error);
}

void graph_remove_edges_in_vertex_values(ustore_graph_remove_edges_t& arguments) noexcept {
    if (!arguments.tasks_count) return;

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    update_neighborhoods<neighborhood_update_t::erase_k>( //
        arguments.db, arguments.transaction, arguments.tasks_count, arguments.collections, arguments.collections_stride,
        arguments.edges_ids, arguments.edges_stride, arguments.sources_ids, arguments.sources_stride,
        arguments.targets_ids, arguments.targets_stride, arguments.options, arena, arguments.error);
}

void graph_upsert_vertices_in_vertex_values(ustore_graph_upsert_vertices_t& arguments) noexcept {
    if (!arguments.tasks_count) return;

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    ustore_length_t* found_lengths {};
    ustore_read_t read {};
    read.db = arguments.db;
    read.error = arguments.error;
    read.transaction = arguments.transaction;
    read.arena = arena;
    read.options = arguments.options;
    read.tasks_count = arguments.tasks_count;
    read.collections = arguments.collections;
    read.collections_stride = arguments.collections_stride;
    read.keys = arguments.vertices;
    read.keys_stride = arguments.vertices_stride;
    read.lengths = &found_lengths;

    ustore_read(&read);
    if (arguments.error->status) return;

    std::span<ustore_key_t> const missing_vertices = arena.alloc<ustore_key_t>(arguments.tasks_count, arguments.error);
    if (arguments.error->status) return;

    strided_iterator<ustore_key_t const> const vertices {arguments.vertices, arguments.vertices_stride};
    std::size_t missing_count = 0;
    for (std::size_t index = 0; index != arguments.tasks_count; ++index)
        if (found_lengths[index] == ustore_length_missing_k) missing_vertices[missing_count++] = vertices[index];
    if (!missing_count) return;

    ustore_length_t const empty_length = 0;
    value_view_t const empty_value {""};
    ustore_write_t write {};
    write.db = arguments.db;
    write.error = arguments.error;
    write.transaction = arguments.transaction;
    write.arena = arena;
    write.tasks_count = missing_count;
    write.collections = arguments.collections;
    write.collections_stride = arguments.collections_stride;
    write.keys = missing_vertices.data();
    write.keys_stride = sizeof(ustore_key_t);
    write.lengths = &empty_length;
    write.values = empty_value.member_ptr();

    ustore_write(&write);
}

void graph_remove_vertices_in_vertex_values(ustore_graph_remove_vertices_t& arguments) noexcept {
    if (!arguments.tasks_count) return;

    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    strided_iterator<ustore_collection_id_t const> const vertex_collections {arguments.collections,
                                                                             arguments.collections_stride};
    strided_iterator<ustore_key_t const> const vertices {arguments.vertices, arguments.vertices_stride};
    strided_iterator<ustore_vertex_role_t const> const vertex_roles {arguments.roles, arguments.roles_stride};

    // Only the neighbors of every removed vertex are needed to find the references to erase
    ustore_vertex_degree_t* degrees_per_vertex = nullptr;
    ustore_key_t* neighbors_per_vertex = nullptr;
    export_edge_tuples<edge_export_t::neighbors_k>( //
        arguments.db, arguments.transaction, nullptr, arguments.tasks_count, arguments.collections,
        arguments.collections_stride, arguments.vertices, arguments.vertices_stride, arguments.roles,
        arguments.roles_stride, arguments.options, &degrees_per_vertex, &neighbors_per_vertex, arena, arguments.error);
    if (arguments.error->status) return;

    auto const present_degree = [&](std::size_t index) {
        ustore_vertex_degree_t const degree = degrees_per_vertex[index];
        return degree == ustore_vertex_degree_missing_k ? std::size_t {0} : std::size_t {degree};
    };

    std::size_t unique_count = arguments.tasks_count;
    for (std::size_t index = 0; index != arguments.tasks_count; ++index) unique_count += present_degree(index);

    std::span<updated_entry_t> unique_entries = arena.alloc<updated_entry_t>(unique_count, arguments.error);
    if (arguments.error->status) return;
    std::ranges::fill(unique_entries, updated_entry_t {});

    // Every removed vertex and every neighbor of it, sorted and deduplicated for lookups
    updated_entry_t* planned = unique_entries.data();
    ustore_key_t const* neighbor = neighbors_per_vertex;
    for (std::size_t index = 0; index != arguments.tasks_count; ++index) {
        ustore_collection_id_t const collection = vertex_collections[index];
        planned->collection = collection;
        planned->key = vertices[index];
        ++planned;
        for (std::size_t offset = 0; offset != present_degree(index); ++offset, ++neighbor, ++planned) {
            planned->collection = collection;
            planned->key = *neighbor;
        }
    }
    unique_count = sort_and_deduplicate(unique_entries.data(), planned);
    unique_entries = unique_entries.first(unique_count);

    strided_range<updated_entry_t> const unique_strided = strided(unique_entries);
    pull_and_link_for_updates(arguments.db, arguments.transaction, unique_strided, arguments.options, arena,
                              arguments.error);
    if (arguments.error->status) return;

    // Erase the references every neighbor holds, then the removed vertex itself
    for (std::size_t index = 0; index != arguments.tasks_count; ++index) {
        ustore_collection_id_t const collection = vertex_collections[index];
        ustore_key_t const vertex_id = vertices[index];
        ustore_vertex_role_t const vertex_role = vertex_roles ? vertex_roles[index] : ustore_vertex_role_any_k;
        updated_entry_t& vertex_entry =
            unique_entries[offset_in_sorted(unique_entries, collection_key_t {collection, vertex_id})];

        for (neighborship_t const neighborship : neighbors(vertex_entry, vertex_role)) {
            updated_entry_t& neighbor_entry = unique_entries[offset_in_sorted(
                unique_entries, collection_key_t {collection, neighborship.neighbor_id})];
            if (vertex_role == ustore_vertex_role_any_k) {
                erase_from_entry(neighbor_entry, ustore_vertex_source_k, vertex_id);
                erase_from_entry(neighbor_entry, ustore_vertex_target_k, vertex_id);
            }
            else erase_from_entry(neighbor_entry, invert(vertex_role), vertex_id);
        }

        vertex_entry.content = nullptr;
        vertex_entry.length = ustore_length_missing_k;
    }

    auto const written_collections = unique_strided.immutable().members(&updated_entry_t::collection);
    auto const written_keys = unique_strided.immutable().members(&updated_entry_t::key);
    auto const written_lengths = unique_strided.immutable().members(&updated_entry_t::length);
    auto const written_contents = unique_strided.immutable().members(&updated_entry_t::content);

    ustore_write_t write {};
    write.db = arguments.db;
    write.error = arguments.error;
    write.transaction = arguments.transaction;
    write.arena = arena;
    write.options = arguments.options;
    write.tasks_count = unique_count;
    write.collections = written_collections.begin().get();
    write.collections_stride = written_collections.stride();
    write.keys = written_keys.begin().get();
    write.keys_stride = written_keys.stride();
    write.lengths = written_lengths.begin().get();
    write.lengths_stride = written_lengths.stride();
    write.values = written_contents.begin().get();
    write.values_stride = written_contents.stride();

    ustore_write(&write);
}

} // namespace unum::ustore
