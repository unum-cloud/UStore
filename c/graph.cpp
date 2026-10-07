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

#include "ustore/ranges_args.hpp" // `find_edges_t`, `strided_range`, `collection_key_t`, `arena_t`

#include "engine.hpp"                // `engine_of`, `view_t`, `graph_find_edges_in_vertex_values`
#include "helpers/algorithm.hpp"     // `offset_in_sorted`
#include "helpers/linked_memory.hpp" // `arena_allocate`

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

/** Vertex values found by one read, indexed by task. */
struct found_values_t {

    /** Bits marking the vertices found, which tell a missing vertex apart from a bare one. */
    ustore_octet_t* presences = nullptr;

    /** One offset into @c contents per vertex, and one more closing the last. */
    ustore_length_t* offsets = nullptr;

    /** Joined values of the vertices. */
    ustore_byte_t* contents = nullptr;

    value_view_t operator[](std::size_t index) const noexcept {
        if (!bits_view_t {presences}[index]) return value_view_t {};
        return value_view_t {contents + offsets[index], offsets[index + 1] - offsets[index]};
    }
};

/** Reads the values of @p count vertices through @p view with the engine's own read, which appends to @p arena rather
 *  than rewinding it. */
expected<found_values_t> read_vertices(ustore_database_t database, std::size_t count,
                                       strided_iterator<ustore_collection_id_t const> collections,
                                       strided_iterator<ustore_key_t const> vertices, ustore_arena_t* arena,
                                       view_t const& view) noexcept {
    auto [presences, presences_status] = arena_allocate<ustore_octet_t>(arena, divide_round_up(count, bits_in_byte_k));
    if (presences_status.failed()) return presences_status;
    auto [offsets, offsets_status] = arena_allocate<ustore_length_t>(arena, count + 1);
    if (offsets_status.failed()) return offsets_status;

    found_values_t found {presences.data(), offsets.data(), nullptr};
    ustore_read_t read {};
    read.database = database;
    read.count = count;
    read.collections = collections.get();
    read.collections_stride = collections.stride();
    read.keys = vertices.get();
    read.keys_stride = vertices.stride();
    read.presences = found.presences;
    read.offsets = found.offsets;
    read.values = &found.contents;
    read.arena = arena;
    if (status_t const status = engine_of(database).read(read, view); status.failed()) return status;
    return found;
}

/** Reads the vertices and exports their degrees, and their neighbors or edges when @p export_kind_ asks for them. */
template <edge_export_t export_kind_>
status_t export_edge_tuples( //
    ustore_database_t const database, ustore_size_t const vertices_count, ustore_collection_id_t const* collections,
    ustore_size_t const collections_stride, ustore_key_t const* vertices, ustore_size_t const vertices_stride,
    ustore_vertex_role_t const* roles, ustore_size_t const roles_stride,
    strided_iterator<ustore_vertex_degree_t> const degrees, ustore_key_t** neighborships_per_vertex,
    ustore_arena_t* arena, view_t const& view) noexcept {

    find_edges_t const find_edges {
        {collections, collections_stride},
        {vertices, vertices_stride},
        {roles, roles_stride},
        vertices_count,
    };
    auto [values, values_status] = read_vertices(database, vertices_count, find_edges.collections_begin,
                                                 find_edges.vertex_id_begin, arena, view);
    if (values_status.failed()) return values_status;
    constexpr std::size_t ids_per_neighborship_k = ids_per_neighborship(export_kind_);

    std::size_t ids_count = 0;
    for (std::size_t index = 0; index != vertices_count; ++index) {
        if (!is_vertex_value(values[index]))
            return {status_code_t::malformed_k, "A vertex holds a value that is not a graph vertex"};
        ids_count += neighbors(values[index], find_edges[index].role).size();
    }
    ids_count *= ids_per_neighborship_k;

    auto [ids, ids_status] = arena_allocate<ustore_key_t>(arena, ids_count);
    if (ids_status.failed()) return ids_status;
    if (neighborships_per_vertex) *neighborships_per_vertex = ids.data();

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
    return {};
}

/** Reads the current values of @p unique_entries and points every entry at its own bytes. */
status_t pull_and_link_for_updates( //
    ustore_database_t const database, strided_range<updated_entry_t> unique_entries, ustore_arena_t* arena,
    view_t const& view) noexcept {

    std::size_t const unique_count = unique_entries.size();
    auto const collections = unique_entries.immutable().members(&updated_entry_t::collection);
    auto const keys = unique_entries.immutable().members(&updated_entry_t::key);

    // A transaction watches the vertices it rewrites, whatever the caller asked of the writes
    view_t watching = view;
    watching.watch = ustore_watch_keys_k;
    auto [found, status] = read_vertices(database, unique_count, collections.begin(), keys.begin(), arena, watching);
    if (status.failed()) return status;

    for (std::size_t index = 0; index != unique_count; ++index) {
        value_view_t const value = found[index];
        if (!is_vertex_value(value))
            return {status_code_t::malformed_k, "A vertex holds a value that is not a graph vertex"};
        unique_entries[index].content = ustore_bytes_ptr_t(value.data());
        unique_entries[index].length = value ? static_cast<ustore_length_t>(value.size()) : ustore_length_missing_k;
    }
    return {};
}

/** Writes the vertex values of @p entries back through the engine's own write, in the transaction of @p view. */
status_t write_entries(ustore_database_t const database, std::span<updated_entry_t const> entries,
                       ustore_durability_t const durability, view_t const& view) noexcept {
    if (entries.empty()) return {};
    strided_range<updated_entry_t const> const written = strided(entries);
    strided_range<ustore_collection_id_t const> const collections = written.members(&updated_entry_t::collection);
    strided_range<ustore_key_t const> const keys = written.members(&updated_entry_t::key);
    strided_range<ustore_length_t const> const lengths = written.members(&updated_entry_t::length);
    strided_range<ustore_bytes_ptr_t const> const contents = written.members(&updated_entry_t::content);

    ustore_write_t write {};
    write.database = database;
    write.count = entries.size();
    write.collections = collections.begin().get();
    write.collections_stride = collections.stride();
    write.keys = keys.begin().get();
    write.keys_stride = keys.stride();
    write.lengths = lengths.begin().get();
    write.lengths_stride = lengths.stride();
    write.values = contents.begin().get();
    write.values_stride = contents.stride();
    write.durability = durability;
    return engine_of(database).write(write, view);
}

/** Inserts or erases edges in the neighborships of both their ends, and writes every touched vertex back. */
template <neighborhood_update_t update_, typename arguments_type_>
status_t update_neighborhoods(arguments_type_ const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.sources || !arguments.targets)
        return {status_code_t::invalid_argument_k, "Need the sources and the targets of the edges"};

    // An edit carries no arena, so the vertex values it rewrites live in scratch freed once it returns
    arena_t scratch;
    std::size_t const edges_count = arguments.count;
    strided_iterator<ustore_collection_id_t const> const edge_collections =
        arguments.collections
            ? strided_iterator<ustore_collection_id_t const> {arguments.collections, arguments.collections_stride}
            : strided_iterator<ustore_collection_id_t const> {&ustore_collection_main_k, 0};
    strided_iterator<ustore_key_t const> const edges {arguments.edges, arguments.edges_stride};
    strided_iterator<ustore_key_t const> const sources {arguments.sources, arguments.sources_stride};
    strided_iterator<ustore_key_t const> const targets {arguments.targets, arguments.targets_stride};

    // Both ends of every edge, deduplicated, so each touched vertex is read and written once
    auto [unique_entries, entries_status] = arena_allocate<updated_entry_t>(scratch, edges_count * 2);
    if (entries_status.failed()) return entries_status;
    std::ranges::fill(unique_entries, updated_entry_t {});
    for (std::size_t index = 0; index != edges_count; ++index) {
        unique_entries[index].collection = edge_collections[index];
        unique_entries[index].key = sources[index];
        unique_entries[edges_count + index].collection = edge_collections[index];
        unique_entries[edges_count + index].key = targets[index];
    }

    std::size_t const unique_count = sort_and_deduplicate(unique_entries.begin(), unique_entries.end());
    unique_entries = unique_entries.first(unique_count);

    if (status_t const status = pull_and_link_for_updates(arguments.database, strided(unique_entries), scratch, view);
        status.failed())
        return status;

    auto const for_each_task = [&](auto update_entry) {
        for (std::size_t index = 0; index != edges_count; ++index) {
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
            auto [buffer, status] = arena_allocate<byte_t>(scratch,
                                                           bytes_present + bytes_for_relations + bytes_for_degrees);
            if (status.failed()) return status;
            if (bytes_present) std::memcpy(buffer.data(), entry.content, bytes_present);
            entry.content = reinterpret_cast<ustore_bytes_ptr_t>(buffer.data());
            entry.length = static_cast<ustore_length_t>(bytes_present);
        }
        for_each_task(&insert_into_entry);
    }

    // Vertices the update left unchanged move to the back and are not written
    auto const unchanged = std::ranges::partition(
        unique_entries, [](ustore_vertex_degree_t delta) { return delta != 0; }, &updated_entry_t::degree_delta);
    std::size_t const changed_count = static_cast<std::size_t>(unchanged.begin() - unique_entries.begin());
    return write_entries(arguments.database, unique_entries.first(changed_count), arguments.durability, view);
}

/** Removes the vertices of @p entries through the engine's own removal, in the transaction of @p view. */
status_t remove_entries(ustore_database_t const database, std::span<updated_entry_t const> entries,
                        ustore_durability_t const durability, view_t const& view) noexcept {
    if (entries.empty()) return {};
    strided_range<updated_entry_t const> const removed = strided(entries);
    strided_range<ustore_collection_id_t const> const collections = removed.members(&updated_entry_t::collection);
    strided_range<ustore_key_t const> const keys = removed.members(&updated_entry_t::key);

    ustore_remove_t remove {};
    remove.database = database;
    remove.count = entries.size();
    remove.collections = collections.begin().get();
    remove.collections_stride = collections.stride();
    remove.keys = keys.begin().get();
    remove.keys_stride = keys.stride();
    remove.durability = durability;
    return engine_of(database).remove(remove, view);
}

status_t graph_find_edges_in_vertex_values(ustore_graph_find_edges_t const& arguments, view_t const& view) noexcept {
    if (!arguments.count) return {};
    if (!arguments.degrees_per_vertex) return {status_code_t::invalid_argument_k, "Need an output for the degrees"};
    if (arguments.edges_per_vertex && !arguments.arena)
        return {status_code_t::invalid_argument_k, "The edges need an arena to land in"};

    // A call asking only for degrees may carry no arena, so the vertex values it reads live in scratch freed on return
    arena_t scratch;
    ustore_arena_t* const arena = arguments.arena ? arguments.arena : scratch;
    strided_iterator<ustore_vertex_degree_t> const degrees {arguments.degrees_per_vertex,
                                                            arguments.degrees_per_vertex_stride};
    if (arguments.edges_per_vertex)
        return export_edge_tuples<edge_export_t::edges_k>( //
            arguments.database, arguments.count, arguments.collections, arguments.collections_stride,
            arguments.vertices, arguments.vertices_stride, arguments.roles, arguments.roles_stride, degrees,
            arguments.edges_per_vertex, arena, view);
    return export_edge_tuples<edge_export_t::degrees_k>( //
        arguments.database, arguments.count, arguments.collections, arguments.collections_stride, arguments.vertices,
        arguments.vertices_stride, arguments.roles, arguments.roles_stride, degrees, nullptr, arena, view);
}

status_t graph_upsert_edges_in_vertex_values(ustore_graph_upsert_edges_t const& arguments,
                                             view_t const& view) noexcept {
    return update_neighborhoods<neighborhood_update_t::upsert_k>(arguments, view);
}

status_t graph_remove_edges_in_vertex_values(ustore_graph_remove_edges_t const& arguments,
                                             view_t const& view) noexcept {
    return update_neighborhoods<neighborhood_update_t::erase_k>(arguments, view);
}

status_t graph_upsert_vertices_in_vertex_values(ustore_graph_upsert_vertices_t const& arguments,
                                                view_t const& view) noexcept {
    std::size_t const count = arguments.count;
    if (!count) return {};

    // An edit carries no arena, so the presences it reads and the vertices it writes live in scratch freed on return
    arena_t scratch;
    auto [presences, presences_status] = arena_allocate<ustore_octet_t>(scratch,
                                                                        divide_round_up(count, bits_in_byte_k));
    if (presences_status.failed()) return presences_status;
    auto [missing, missing_status] = arena_allocate<updated_entry_t>(scratch, count);
    if (missing_status.failed()) return missing_status;

    ustore_read_t read {};
    read.database = arguments.database;
    read.count = count;
    read.collections = arguments.collections;
    read.collections_stride = arguments.collections_stride;
    read.keys = arguments.vertices;
    read.keys_stride = arguments.vertices_stride;
    read.presences = presences.data();
    read.arena = scratch;
    if (status_t const status = engine_of(arguments.database).read(read, view); status.failed()) return status;

    strided_iterator<ustore_collection_id_t const> const collections {arguments.collections,
                                                                      arguments.collections_stride};
    strided_iterator<ustore_key_t const> const vertices {arguments.vertices, arguments.vertices_stride};
    bits_view_t const present {presences.data()};
    std::size_t missing_count = 0;
    for (std::size_t index = 0; index != count; ++index) {
        if (present[index]) continue;
        ustore_collection_id_t const collection = collections ? collections[index] : ustore_collection_main_k;
        missing[missing_count++] = updated_entry_t {{collection, vertices[index]}, nullptr, 0, 0};
    }
    return write_entries(arguments.database, missing.first(missing_count), arguments.durability, view);
}

status_t graph_remove_vertices_in_vertex_values(ustore_graph_remove_vertices_t const& arguments,
                                                view_t const& view) noexcept {
    if (!arguments.count) return {};

    // An edit carries no arena, so the vertex values it rewrites live in scratch freed once it returns
    arena_t scratch;
    strided_iterator<ustore_collection_id_t const> const vertex_collections =
        arguments.collections
            ? strided_iterator<ustore_collection_id_t const> {arguments.collections, arguments.collections_stride}
            : strided_iterator<ustore_collection_id_t const> {&ustore_collection_main_k, 0};
    strided_iterator<ustore_key_t const> const vertices {arguments.vertices, arguments.vertices_stride};
    strided_iterator<ustore_vertex_role_t const> const vertex_roles {arguments.roles, arguments.roles_stride};

    // Only the neighbors of every removed vertex are needed to find the references to erase
    auto [degrees, degrees_status] = arena_allocate<ustore_vertex_degree_t>(scratch, arguments.count);
    if (degrees_status.failed()) return degrees_status;
    ustore_key_t* neighbors_per_vertex = nullptr;
    if (status_t const status = export_edge_tuples<edge_export_t::neighbors_k>( //
            arguments.database, arguments.count, arguments.collections, arguments.collections_stride,
            arguments.vertices, arguments.vertices_stride, arguments.roles, arguments.roles_stride,
            {degrees.data(), sizeof(ustore_vertex_degree_t)}, &neighbors_per_vertex, scratch, view);
        status.failed())
        return status;

    auto const present_degree = [&](std::size_t index) {
        ustore_vertex_degree_t const degree = degrees[index];
        return degree == ustore_vertex_degree_missing_k ? std::size_t {0} : std::size_t {degree};
    };

    std::size_t unique_count = arguments.count;
    for (std::size_t index = 0; index != arguments.count; ++index) unique_count += present_degree(index);

    auto [unique_entries, entries_status] = arena_allocate<updated_entry_t>(scratch, unique_count);
    if (entries_status.failed()) return entries_status;
    std::ranges::fill(unique_entries, updated_entry_t {});

    // Every removed vertex and every neighbor of it, sorted and deduplicated for lookups
    updated_entry_t* planned = unique_entries.data();
    ustore_key_t const* neighbor = neighbors_per_vertex;
    for (std::size_t index = 0; index != arguments.count; ++index) {
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

    if (status_t const status = pull_and_link_for_updates(arguments.database, strided(unique_entries), scratch, view);
        status.failed())
        return status;

    // Erase the references every neighbor holds, then the removed vertex itself
    for (std::size_t index = 0; index != arguments.count; ++index) {
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

    // Rewritten neighbors lead, removed vertices follow, and the untouched rest is never written
    auto const removed_and_untouched = std::ranges::partition(unique_entries, [](updated_entry_t const& entry) {
        return entry.length != ustore_length_missing_k && entry.degree_delta != 0;
    });
    auto const untouched = std::ranges::partition(
        removed_and_untouched, [](updated_entry_t const& entry) { return entry.length == ustore_length_missing_k; });
    std::size_t const rewritten_count = static_cast<std::size_t>(removed_and_untouched.begin() -
                                                                 unique_entries.begin());
    std::size_t const removed_count = static_cast<std::size_t>(untouched.begin() - removed_and_untouched.begin());

    // Neighbors are rewritten before the vertices go, so a removal failing at the head leaves them for a retry
    if (status_t const status = write_entries(arguments.database, unique_entries.first(rewritten_count),
                                              arguments.durability, view);
        status.failed())
        return status;
    return remove_entries(arguments.database, unique_entries.subspan(rewritten_count, removed_count),
                          arguments.durability, view);
}

} // namespace unum::ustore
