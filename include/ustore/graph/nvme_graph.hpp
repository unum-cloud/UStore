/**
 *  @file include/ustore/graph/nvme_graph.hpp
 *  @author Ash Vardanian
 *  @date December 1, 2022
 *  @brief A persistent graph over two single-collection databases, the vertices and their half-edges, its bounds,
 *      and the optimistic transaction spanning both.
 *
 *  The vertices database, keyed by the entity, holds each vertex's identity, weight and label beside the
 *  registry of the edge blocks its adjacency is split into. The half-edges database, keyed by the entity
 *  paired with a block's first neighbor, holds those blocks. An edge is written twice, once under each of
 *  its endpoints, so a walk of one vertex reads only that vertex's blocks. Both databases hold variable-width
 *  values and take the same bounds, since a registry and an edge block are the same order of magnitude.
 *
 *  Every call reads what it needs in one batch and writes what it changed in one pass, which is the point
 *  of this layer: a thousand vertex pairs looked up together cost one depth-filling batch rather than a
 *  thousand round trips. Reads take the caller's channel and run on any number of threads; writes reach the
 *  database's own write path, which admits one writer at a time. Nothing here excludes anything: the one
 *  claim in reach is the write path of the databases underneath, which they take and give back on their own.
 *
 *  A graph spans two databases, so an indivisible change spans two transactions. Both are staged before
 *  either commits: a staged transaction holds its database's write path, so a reader that consults a
 *  vertex's registry before its edge blocks waits behind whichever half has not landed and sees both
 *  halves of a commit or neither. Nothing is read between the staging and the commit, since a staged
 *  transaction refuses every read.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <optional>    // `std::optional`
#include <span>        // `std::span`
#include <type_traits> // `std::remove_reference_t`
#include <utility>     // `std::move`

#include <smashtable/basic_flat_set.hpp> // `st::basic_flat_set`
#include <smashtable/basic_vector.hpp>   // `st::basic_vector`

#include "ustore/blobs/nvme_collection.hpp"   // `select_result_t`
#include "ustore/blobs/nvme_database.hpp"     // `basic_single_collection_database`
#include "ustore/blobs/nvme_key_slot.hpp"     // `key_slot`, `stored_value_size_t`, `transaction_id_t`
#include "ustore/blobs/nvme_tree.hpp"         // `single_collection_database_config_t`, `user_collection_config_t`
#include "ustore/graph/edge_block.hpp"        // `labeled_edge_t`, `labeled_half_edge_t`, `edge_block_t`, `id_chunk_t`
#include "ustore/graph/vertex_registry.hpp"   // `vertex_registry_t`
#include "ustore/utilities/algorithm.hpp"     // `position_t`
#include "ustore/utilities/nvme_channels.hpp" // `io_channel_t`, `basic_io_channel_pool`
#include "ustore/utilities/nvme_disks.hpp"    // `blocking_files_disk_t`
#include "ustore/utilities/nvme_ring.hpp"     // `bytes_span_t`, `bytes_view_t`, `to_status`
#include "ustore/utilities/nvme_volume.hpp"   // `basic_extent_volume`, `resources_shared_ptr_t`
#include "ustore/utilities/types.hpp"         // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::graph {

using blobs::basic_single_collection_database;
using blobs::create_single_collection_database_config;
using blobs::key_slot;
using blobs::select_result_t;
using blobs::single_collection_database_config_t;
using blobs::stored_value_size_t;
using blobs::transaction_id_t;
using blobs::transaction_stamp_t;
using blobs::user_single_collection_database_config_t;
using blobs::value_layout_t;
using utilities::basic_extent_volume;
using utilities::basic_io_channel_pool;
using utilities::blocking_files_disk_t;
using utilities::byte_t;
using utilities::io_channel_t;
using utilities::resources_shared_ptr_t;
using utilities::to_status;

#pragma region Graph Store Config

/**
 *  @brief The bounds a graph store opens its vertices and its half-edges databases with.
 *
 *  Both databases hold variable-width values and take the same bounds, since a registry and an edge
 *  block are the same order of magnitude and neither is worth a budget of its own.
 */
struct graph_store_config_t {

    /** Values one transaction of the graph stages before it refuses another. */
    std::size_t transaction_max_elements = 1'000;

    /** Bytes one transaction of the graph stages before it refuses another. */
    std::size_t transaction_max_bytes = 16ull * 1024ull * 1024ull;

    /** Readers one collection admits at once. */
    std::size_t concurrency_limit = 1;

    /** Bytes each collection's value cache holds. */
    std::size_t cache_limit = 1024ull * 1024ull;

    /** Values one write buffer stages before it flushes. */
    std::size_t write_buffer_max_elements = 8'192;

    /** Bytes one write buffer stages before it flushes. */
    std::size_t write_buffer_max_bytes = 8ull * 1024ull * 1024ull;

    /** Bytes level zero admits before a compaction is due. */
    std::size_t first_level_max_bytes = 64ull * 1024ull * 1024ull;

    /** How much deeper each level is than the one above it. */
    std::size_t level_enlarge_factor = 4;

    /** Bytes the largest registry or edge block may take. */
    stored_value_size_t value_max_size = 1024ull * 1024ull;

    /** The configuration both databases of the graph are opened with. */
    [[nodiscard]] single_collection_database_config_t databases() const noexcept {
        user_single_collection_database_config_t user;
        user.default_transaction.write_buffer_max_elements = transaction_max_elements;
        user.default_transaction.write_buffer_max_bytes = transaction_max_bytes;
        user.collection.concurrency_limit = concurrency_limit;
        user.collection.cache_limit = cache_limit;
        user.collection.write_buffer_max_elements = write_buffer_max_elements;
        user.collection.write_buffer_max_bytes = write_buffer_max_bytes;
        user.collection.first_level_max_bytes = first_level_max_bytes;
        user.collection.level_enlarge_factor = level_enlarge_factor;
        user.collection.value_size = 0;
        user.collection.value_max_size = value_max_size;
        return create_single_collection_database_config(user);
    }
};

#pragma endregion Graph Store Config

#pragma region Graph Store

#pragma region Vocabulary

/** Vertices and edges one call created, or one call removed. */
struct graph_change_t {

    /** Vertices the call brought into the graph, or took out of it. */
    std::size_t vertices = 0;

    /** Edges the call added, or removed. */
    std::size_t edges = 0;
};

/** Neighbors a set of a graph orders, which is what a walk of one vertex answers. */
using vertex_ids_set_t = st::basic_flat_set<id_entity_t>;

/** Whether one half-edge landed in an edge block that did not already carry it. */
enum class edge_update_t : std::uint8_t {

    /** The edge block already carried a half-edge to that neighbor under that label. */
    unchanged_k,

    /** The edge block gained a half-edge it did not have. */
    changed_k,
};

/** Where one record of a batch stands between the request that named it and the write that lands it. */
enum class record_state_t : std::uint8_t {

    /** Named by the batch, and not yet looked up. */
    unread_k,

    /** Looked up, and the source carries nothing under that key. */
    absent_k,

    /** Read out of the source and unchanged since. */
    present_k,

    /** Made here, so the source has never seen it and a write inserts it. */
    created_k,

    /** Read out of the source and changed since, so a write replaces it. */
    updated_k,

    /** To be taken out of the source. */
    erased_k,
};

/** Whether @p state names a record a caller may read and change. */
[[nodiscard]] constexpr bool is_live(record_state_t state) noexcept {
    return state == record_state_t::present_k || state == record_state_t::created_k ||
           state == record_state_t::updated_k;
}

/** The state a record takes once it has changed, which keeps a record the source never saw new. */
[[nodiscard]] constexpr record_state_t changed_state(record_state_t state) noexcept {
    return state == record_state_t::present_k || state == record_state_t::updated_k ? record_state_t::updated_k
                                                                                    : record_state_t::created_k;
}

/** One key of a batch, the record it parsed into, and where that record stands. */
template <typename stored_key_type_, typename record_type_>
struct record_entry {

    /** The key the source addresses this record by. */
    stored_key_type_ stored_key {};

    /** The record itself, left default-built until a read fills it. */
    record_type_ record {};

    /** Where this record stands between its request and its write-back. */
    record_state_t state = record_state_t::unread_k;
};

#pragma endregion Vocabulary

#pragma region Batches

/** Times a batched read sizes itself before it gives up on values growing under it. */
inline constexpr std::size_t batch_read_attempts_k = 4;

/**
 *  @brief The keys one call touches and the records it parsed for them, ordered by key.
 *
 *  A call names every key it could need, reads them all in one batched select, changes the records in
 *  memory and writes back only what changed. Two rounds of naming are ordinary - the registries name the
 *  edge blocks - and a second @c resolve reads only what the first one did not.
 */
template <typename stored_key_type_, typename record_type_>
class basic_record_batch {
  public:

#pragma region Types

    /** The key the source addresses every record by. */
    using stored_key_t = stored_key_type_;

    /** The record every present key parses into. */
    using record_t = record_type_;

    /** One key of this batch and the record it parsed into. */
    using entry_t = record_entry<stored_key_type_, record_type_>;

#pragma endregion Types

    basic_record_batch() noexcept = default;
    basic_record_batch(basic_record_batch const&) = delete;
    basic_record_batch& operator=(basic_record_batch const&) = delete;

    /** Keys this batch names, whether they were read, made or dropped. */
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /** The entry at @p index, which key order places. */
    [[nodiscard]] entry_t& operator[](std::size_t index) noexcept { return entries_[index]; }

    /** The entry at @p index, which key order places. */
    [[nodiscard]] entry_t const& operator[](std::size_t index) const noexcept { return entries_[index]; }

    /** The entry of @p stored_key, or null when this batch names no such key. */
    [[nodiscard]] entry_t* find(stored_key_t const& stored_key) noexcept {
        position_t const at = locate_(stored_key);
        return at.exists ? &entries_[at.offset] : nullptr;
    }

    /** The entry of @p stored_key, or null when this batch names no such key. */
    [[nodiscard]] entry_t const* find(stored_key_t const& stored_key) const noexcept {
        position_t const at = locate_(stored_key);
        return at.exists ? &entries_[at.offset] : nullptr;
    }

    /**
     *  @brief The entry of @p stored_key, made as @c unread_k when this batch names no such key.
     *  @note This may move every entry, so a caller holds what an earlier call answered only until here.
     */
    expected<entry_t*> obtain(stored_key_t const& stored_key) noexcept {
        position_t const at = locate_(stored_key);
        if (at.exists) return &entries_[at.offset];
        entry_t made;
        made.stored_key = stored_key;
        if (entries_.insert(at.offset, std::move(made)) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for another record in a batch"};
        return &entries_[at.offset];
    }

    /** Names @p stored_key so the next @c resolve reads it, unless this batch already names it. */
    status_t request(stored_key_t const& stored_key) noexcept {
        expected<entry_t*> const named = obtain(stored_key);
        return named ? status_t {} : named.status();
    }

    /**
     *  @brief Reads every key named and not yet looked up, in one batched select over @p source.
     *  @param[in] source The database or the transaction the values come out of.
     *  @param[inout] channel The channel the reads are submitted and completed on, held by the calling thread.
     */
    template <typename source_type_>
    status_t resolve(source_type_ const& source, io_channel_t& channel) noexcept {
        st::basic_vector<std::size_t> unread;
        for (std::size_t index = 0; index != entries_.size(); ++index)
            if (entries_[index].state == record_state_t::unread_k)
                if (unread.push_back(std::size_t {index}) != st::status_t::success_k)
                    return {status_code_t::out_of_memory_k, "No room to list the keys of a batch"};
        if (unread.empty()) return {};

        std::size_t const count = unread.size();
        st::basic_vector<stored_key_t> keys;
        st::basic_vector<select_result_t> answers;
        st::basic_vector<std::uint64_t> offsets;
        st::basic_vector<byte_t> values;
        if (keys.resize(count) != st::status_t::success_k || answers.resize(count) != st::status_t::success_k ||
            offsets.resize(count + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to read a batch into"};
        for (std::size_t index = 0; index != count; ++index) keys[index] = entries_[unread[index]].stored_key;

        for (std::size_t attempt = 0; attempt != batch_read_attempts_k; ++attempt) {
            // The sizes come from the index in memory, so the values land back to back in one buffer.
            std::uint64_t total = 0;
            for (std::size_t index = 0; index != count; ++index) {
                stored_value_size_t bytes = 0;
                offsets[index] = total;
                answers[index] = select_result_t::nothing_k;
                if (source.find(keys[index], bytes) == select_result_t::success_k) total += bytes;
            }
            offsets[count] = total;
            if (values.resize(static_cast<std::size_t>(total)) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "No room to read a batch into"};

            bool outgrew = false;
            auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
                std::uint64_t const room = offsets[request + 1] - offsets[request];
                if (offset + bytes.size() > room) {
                    outgrew = true;
                    return;
                }
                if (!bytes.empty()) std::memcpy(values.data() + offsets[request] + offset, bytes.data(), bytes.size());
            };
            if (status_t const read = source.select(channel, std::span<stored_key_t const> {keys},
                                                    std::span<select_result_t> {answers}, sink);
                read.failed())
                return read;
            // A commit landing between the sizing and the read leaves a value longer than the room taken for it.
            if (outgrew) continue;

            for (std::size_t index = 0; index != count; ++index) {
                entry_t& entry = entries_[unread[index]];
                std::uint64_t const bytes = offsets[index + 1] - offsets[index];
                if (answers[index] == select_result_t::incomplete_k)
                    return {status_code_t::io_k, "The medium moved fewer bytes than a record claims"};
                if (answers[index] != select_result_t::success_k || bytes == 0) {
                    entry.state = record_state_t::absent_k;
                    continue;
                }
                bytes_view_t const image {values.data() + offsets[index], static_cast<std::size_t>(bytes)};
                if (status_t const parsed = entry.record.load(image); parsed.failed()) return parsed;
                entry.state = record_state_t::present_k;
            }
            return {};
        }
        return {status_code_t::write_conflict_k, "A batched read kept finding records longer than it sized for"};
    }

    /** Inserts every record made or changed into @p destination, and takes out every one dropped. */
    template <typename destination_type_>
    status_t save(destination_type_& destination) noexcept {
        st::basic_vector<byte_t> image;
        for (std::size_t index = 0; index != entries_.size(); ++index) {
            entry_t& entry = entries_[index];
            if (entry.state == record_state_t::erased_k) {
                if (status_t const dropped = destination.erase(entry.stored_key); dropped.failed()) return dropped;
                entry.state = record_state_t::absent_k;
                continue;
            }
            if (entry.state != record_state_t::created_k && entry.state != record_state_t::updated_k) continue;

            std::size_t const bytes = entry.record.storage_requirements();
            if (image.resize(bytes) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "No room to lay a record out"};
            if (status_t const laid = entry.record.save(bytes_span_t {image}); laid.failed()) return laid;
            if (status_t const written = destination.insert(entry.stored_key, std::span<byte_t const> {image});
                written.failed())
                return written;
            entry.state = record_state_t::present_k;
        }
        return {};
    }

  private:

    /** Where @p stored_key sits among the entries, or where it would be inserted. */
    [[nodiscard]] position_t locate_(stored_key_t const& stored_key) const noexcept {
        std::size_t low = 0;
        std::size_t high = entries_.size();
        while (low != high) {
            std::size_t const middle = low + (high - low) / 2;
            if (entries_[middle].stored_key < stored_key) low = middle + 1;
            else high = middle;
        }
        return {low, low != entries_.size() && !(stored_key < entries_[low].stored_key)};
    }

    /** Every key this batch names, ordered by key so a lookup is a binary search. */
    st::basic_vector<entry_t> entries_;
};

/** The registries one call reads, keyed by the vertex they belong to. */
using vertex_batch_t = basic_record_batch<id_entity_t, vertex_registry_t>;

/** The edge blocks one call reads, keyed by the vertex and the block's first neighbor. */
using edge_block_batch_t = basic_record_batch<id_chunk_t, edge_block_t>;

/** One vertex of a batch: its registry and where that registry stands. */
using vertex_entry_t = vertex_batch_t::entry_t;

/** One edge block of a batch: the block and where it stands. */
using edge_block_entry_t = edge_block_batch_t::entry_t;

#pragma endregion Batches

#pragma region Access

/**
 *  @brief Every graph call, over whichever pair of accessors carries the vertices and the half-edges.
 *
 *  The database and its transaction answer the same reads and take the same writes, so one implementation
 *  serves both: a store hands out an access over its two databases, a transaction over its two
 *  transactions, and nothing here knows which it got.
 */
template <typename vertices_access_type_, typename half_edges_access_type_>
class basic_graph_access {
  public:

#pragma region Types

    /** What the registries are read from and written to. */
    using vertices_access_t = vertices_access_type_;

    /** What the edge blocks are read from and written to. */
    using half_edges_access_t = half_edges_access_type_;

#pragma endregion Types

    /** An access over @p vertices and @p half_edges, both of which outlive it. */
    basic_graph_access(vertices_access_type_& vertices, half_edges_access_type_& half_edges) noexcept
        : vertices_(&vertices), half_edges_(&half_edges) {}

#pragma region Reads

    /** Copies the identity, weight and label of @p vertex_id into @p into. */
    status_t find_vertex(io_channel_t& channel, id_entity_t vertex_id, vertex_t& into) const noexcept {
        vertex_batch_t vertices;
        if (status_t const named = vertices.request(vertex_id); named.failed()) return named;
        if (status_t const read = vertices.resolve(*vertices_, channel); read.failed()) return read;
        vertex_entry_t const* const entry = vertices.find(vertex_id);
        if (!entry || !is_live(entry->state)) return {status_code_t::not_found_k, "The graph holds no such vertex"};
        into = entry->record.vertex_info;
        return {};
    }

    /** Adds every vertex of @p vertex_ids the graph holds to @p into, in one batched read. */
    status_t find_vertices(io_channel_t& channel, std::span<id_entity_t const> vertex_ids,
                           vertices_set_t& into) const noexcept {
        vertex_batch_t vertices;
        for (id_entity_t const vertex_id : vertex_ids)
            if (status_t const named = vertices.request(vertex_id); named.failed()) return named;
        if (status_t const read = vertices.resolve(*vertices_, channel); read.failed()) return read;

        for (std::size_t index = 0; index != vertices.size(); ++index) {
            vertex_entry_t const& entry = vertices[index];
            if (!is_live(entry.state)) continue;
            if (st::status_t const held = into.upsert(vertex_t {entry.record.vertex_info});
                held != st::status_t::success_k)
                return to_status(held);
        }
        return {};
    }

    /** Adds every edge @p vertex_id plays @p vertex_role in to @p into, walking its blocks in neighbor order. */
    status_t edges_containing(io_channel_t& channel, id_entity_t vertex_id, role_t vertex_role,
                              edges_set_t& into) const noexcept {
        status_t held;
        if (status_t const walked = visit_edges_of_(
                channel, vertex_id, vertex_role,
                [&](labeled_edge_t const& edge) noexcept {
                    if (st::status_t const kept = into.upsert(labeled_edge_t {edge});
                        kept != st::status_t::success_k && held.succeeded())
                        held = to_status(kept);
                });
            walked.failed())
            return walked;
        return held;
    }

    /** Counts the edges @p vertex_id plays @p vertex_role in. */
    status_t degree(io_channel_t& channel, id_entity_t vertex_id, role_t vertex_role,
                    std::size_t& into) const noexcept {
        into = 0;
        return visit_edges_of_(channel, vertex_id, vertex_role, [&](labeled_edge_t const&) noexcept { ++into; });
    }

    /** Adds every edge between @p first_id and @p second_id that both play their roles in to @p into. */
    status_t edges_between(io_channel_t& channel, id_entity_t first_id, role_t first_role, id_entity_t second_id,
                           role_t second_role, edges_set_t& into) const noexcept {
        vertex_batch_t vertices;
        edge_block_batch_t blocks;
        if (status_t const named = vertices.request(first_id); named.failed()) return named;
        if (status_t const read = vertices.resolve(*vertices_, channel); read.failed()) return read;

        vertex_entry_t const* const entry = vertices.find(first_id);
        if (!entry || !is_live(entry->state)) return {};
        std::optional<id_chunk_t> const chunk = entry->record.chunk_containing(id_chunk_t {first_id, second_id});
        if (!chunk) return {};

        if (status_t const named = blocks.request(*chunk); named.failed()) return named;
        if (status_t const read = blocks.resolve(*half_edges_, channel); read.failed()) return read;
        edge_block_entry_t const* const block = blocks.find(*chunk);
        if (!block || !is_live(block->state)) return {};

        for (labeled_half_edge_t const half_edge : block->record.relations_with(second_id)) {
            labeled_edge_t const edge = half_edge.to_edge(first_id);
            if (!((edge.role_of(first_id) & first_role) && (edge.role_of(second_id) & second_role))) continue;
            if (st::status_t const kept = into.upsert(labeled_edge_t {edge}); kept != st::status_t::success_k)
                return to_status(kept);
        }
        return {};
    }

    /**
     *  @brief Adds every edge between the pairs of @p first_ids and @p second_ids to @p into, in one batch.
     *  @param[inout] channel The channel every read is submitted and completed on, held by the calling thread.
     *  @param[in] first_ids One endpoint of every pair, as many as @p second_ids names.
     *  @param[in] second_ids The other endpoint of every pair, in the order @p first_ids names them.
     *  @param[out] into The edges found, added to whatever the set already held.
     *
     *  One batch reads the registries of every endpoint and then the one edge block each pair needs, so a
     *  thousand pairs cost two depth-filling reads rather than two thousand round trips.
     */
    status_t edges_between(io_channel_t& channel, std::span<id_entity_t const> first_ids,
                           std::span<id_entity_t const> second_ids, edges_set_t& into) const noexcept {
        if (first_ids.size() != second_ids.size())
            return {status_code_t::invalid_argument_k, "A pair lookup takes as many first ends as second ones"};

        vertex_batch_t vertices;
        edge_block_batch_t blocks;
        for (std::size_t pair = 0; pair != first_ids.size(); ++pair) {
            if (status_t const named = vertices.request(first_ids[pair]); named.failed()) return named;
            if (status_t const named = vertices.request(second_ids[pair]); named.failed()) return named;
        }
        if (status_t const read = vertices.resolve(*vertices_, channel); read.failed()) return read;

        // Either endpoint's registry answers the pair, so the one that names a block wins and the other is spared.
        st::basic_vector<std::optional<id_chunk_t>> chosen;
        if (chosen.resize(first_ids.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to place the pairs of a lookup"};
        for (std::size_t pair = 0; pair != first_ids.size(); ++pair) {
            std::optional<id_chunk_t> found = chunk_over_(vertices, first_ids[pair], second_ids[pair]);
            if (!found) found = chunk_over_(vertices, second_ids[pair], first_ids[pair]);
            chosen[pair] = found;
            if (!found) continue;
            if (status_t const named = blocks.request(*found); named.failed()) return named;
        }
        if (status_t const read = blocks.resolve(*half_edges_, channel); read.failed()) return read;

        for (std::size_t pair = 0; pair != first_ids.size(); ++pair) {
            if (!chosen[pair]) continue;
            id_chunk_t const chunk = *chosen[pair];
            edge_block_entry_t const* const block = blocks.find(chunk);
            if (!block || !is_live(block->state)) continue;
            id_entity_t const neighbor = chunk.entity == first_ids[pair] ? second_ids[pair] : first_ids[pair];
            for (labeled_half_edge_t const half_edge : block->record.relations_with(neighbor))
                if (st::status_t const kept = into.upsert(half_edge.to_edge(chunk.entity));
                    kept != st::status_t::success_k)
                    return to_status(kept);
        }
        return {};
    }

    /** Adds every neighbor of every vertex of @p vertex_ids to @p into, in one batch. */
    status_t neighbors(io_channel_t& channel, std::span<id_entity_t const> vertex_ids,
                       vertex_ids_set_t& into) const noexcept {
        vertex_batch_t vertices;
        edge_block_batch_t blocks;
        if (status_t const read = read_adjacency_(channel, vertex_ids, vertices, blocks); read.failed()) return read;

        for (std::size_t index = 0; index != blocks.size(); ++index) {
            edge_block_entry_t const& entry = blocks[index];
            if (!is_live(entry.state)) continue;
            for (id_entity_t const neighbor : entry.record.neighbor_ids)
                if (st::status_t const kept = into.upsert(id_entity_t {neighbor}); kept != st::status_t::success_k)
                    return to_status(kept);
        }
        return {};
    }

#pragma endregion Reads

#pragma region Writes

    /** Writes the identity, weight and label of every vertex of @p wanted, keeping whatever edges it has. */
    status_t upsert_vertices(io_channel_t& channel, std::span<vertex_t const> wanted, std::size_t& created) noexcept {
        created = 0;
        vertex_batch_t vertices;
        for (vertex_t const& vertex : wanted)
            if (status_t const named = vertices.request(vertex.id); named.failed()) return named;
        if (status_t const read = vertices.resolve(*vertices_, channel); read.failed()) return read;

        for (vertex_t const& vertex : wanted) {
            vertex_entry_t* const entry = vertices.find(vertex.id);
            if (!entry) return {status_code_t::internal_k, "A batch lost a vertex it had named"};
            created += entry->state == record_state_t::absent_k;
            entry->record.vertex_info = vertex;
            entry->state = changed_state(entry->state);
        }
        return vertices.save(*vertices_);
    }

    /**
     *  @brief Writes every edge of @p edges under both of its endpoints, creating the vertices it names.
     *  @param[inout] channel The channel every read is submitted and completed on, held by the calling thread.
     *  @param[in] edges The edges to write, of which one per endpoint pair and label survives.
     *  @param[out] changed Vertices this call created, and edges neither endpoint already carried.
     */
    status_t upsert_edges(io_channel_t& channel, std::span<labeled_edge_t const> edges,
                          graph_change_t& changed) noexcept {
        changed = graph_change_t {};
        if (edges.empty()) return {};

        vertex_batch_t vertices;
        edge_block_batch_t blocks;
        for (labeled_edge_t const& edge : edges) {
            if (status_t const named = vertices.request(edge.first); named.failed()) return named;
            if (status_t const named = vertices.request(edge.second); named.failed()) return named;
        }
        if (status_t const read = vertices.resolve(*vertices_, channel); read.failed()) return read;

        // A vertex an edge names but the graph does not hold is created before anything routes through it.
        for (labeled_edge_t const& edge : edges)
            for (id_entity_t const member : {edge.first, edge.second}) {
                vertex_entry_t* const entry = vertices.find(member);
                if (!entry) return {status_code_t::internal_k, "A batch lost a vertex it had named"};
                if (entry->state != record_state_t::absent_k) continue;
                entry->record.clear();
                entry->record.vertex_info = vertex_t {};
                entry->record.vertex_info.id = member;
                entry->state = record_state_t::created_k;
                ++changed.vertices;
            }

        for (labeled_edge_t const& edge : edges)
            for (id_entity_t const member : {edge.first, edge.second}) {
                vertex_entry_t const* const entry = vertices.find(member);
                if (!entry) return {status_code_t::internal_k, "A batch lost a vertex it had named"};
                std::optional<id_chunk_t> const chunk = entry->record.chunk_existing_to_insert(
                    id_chunk_t {member, edge.neighbor_of(member)});
                if (chunk)
                    if (status_t const named = blocks.request(*chunk); named.failed()) return named;
            }
        if (status_t const read = blocks.resolve(*half_edges_, channel); read.failed()) return read;

        for (labeled_edge_t const& edge : edges) {
            edge_update_t outcome = edge_update_t::unchanged_k;
            for (id_entity_t const member : {edge.first, edge.second}) {
                edge_update_t one = edge_update_t::unchanged_k;
                if (status_t const added = add_half_edge_(vertices, blocks, member, labeled_half_edge_t {edge, member},
                                                          one);
                    added.failed())
                    return added;
                if (one == edge_update_t::changed_k) outcome = edge_update_t::changed_k;
            }
            changed.edges += outcome == edge_update_t::changed_k;
        }

        if (status_t const balanced = rebalance_(channel, vertices, blocks); balanced.failed()) return balanced;
        if (status_t const written = vertices.save(*vertices_); written.failed()) return written;
        return blocks.save(*half_edges_);
    }

    /** Takes every edge of @p edges out from under both of its endpoints, keeping the vertices themselves. */
    status_t remove_edges(io_channel_t& channel, std::span<labeled_edge_t const> edges,
                          graph_change_t& changed) noexcept {
        changed = graph_change_t {};
        if (edges.empty()) return {};

        vertex_batch_t vertices;
        edge_block_batch_t blocks;
        for (labeled_edge_t const& edge : edges) {
            if (status_t const named = vertices.request(edge.first); named.failed()) return named;
            if (status_t const named = vertices.request(edge.second); named.failed()) return named;
        }
        if (status_t const read = vertices.resolve(*vertices_, channel); read.failed()) return read;

        for (labeled_edge_t const& edge : edges)
            for (id_entity_t const member : {edge.first, edge.second})
                if (std::optional<id_chunk_t> const chunk = chunk_over_(vertices, member, edge.neighbor_of(member));
                    chunk)
                    if (status_t const named = blocks.request(*chunk); named.failed()) return named;
        if (status_t const read = blocks.resolve(*half_edges_, channel); read.failed()) return read;

        for (labeled_edge_t const& edge : edges) {
            edge_update_t outcome = edge_update_t::unchanged_k;
            for (id_entity_t const member : {edge.first, edge.second}) {
                edge_update_t one = edge_update_t::unchanged_k;
                if (status_t const dropped = remove_half_edge_(vertices, blocks, member,
                                                               labeled_half_edge_t {edge, member}, one);
                    dropped.failed())
                    return dropped;
                if (one == edge_update_t::changed_k) outcome = edge_update_t::changed_k;
            }
            changed.edges += outcome == edge_update_t::changed_k;
        }

        if (status_t const balanced = rebalance_(channel, vertices, blocks); balanced.failed()) return balanced;
        if (status_t const written = vertices.save(*vertices_); written.failed()) return written;
        return blocks.save(*half_edges_);
    }

    /**
     *  @brief Takes every vertex of @p vertex_ids out of the graph, and every edge that touched one.
     *  @param[inout] channel The channel every read is submitted and completed on, held by the calling thread.
     *  @param[in] vertex_ids The vertices to remove, of which the ones the graph does not hold cost nothing.
     *  @param[out] changed Vertices this call removed, and the distinct edges that went with them.
     */
    status_t remove_vertices(io_channel_t& channel, std::span<id_entity_t const> vertex_ids,
                             graph_change_t& changed) noexcept {
        changed = graph_change_t {};
        if (vertex_ids.empty()) return {};

        vertex_batch_t vertices;
        edge_block_batch_t blocks;
        if (status_t const read = read_adjacency_(channel, vertex_ids, vertices, blocks); read.failed()) return read;

        // Every neighbor of a removed vertex carries the other half of an edge, so its registry comes too.
        for (id_entity_t const vertex_id : vertex_ids) {
            vertex_entry_t const* const entry = vertices.find(vertex_id);
            if (!entry || !is_live(entry->state)) continue;
            for (id_entity_t const first_neighbor : entry->record.first_neighbors) {
                edge_block_entry_t const* const block = blocks.find(id_chunk_t {vertex_id, first_neighbor});
                if (!block || !is_live(block->state)) continue;
                for (id_entity_t const neighbor : block->record.neighbor_ids)
                    if (neighbor != vertex_id)
                        if (status_t const named = vertices.request(neighbor); named.failed()) return named;
            }
        }
        if (status_t const read = vertices.resolve(*vertices_, channel); read.failed()) return read;

        for (id_entity_t const vertex_id : vertex_ids) {
            vertex_entry_t const* const entry = vertices.find(vertex_id);
            if (!entry || !is_live(entry->state)) continue;
            for (id_entity_t const first_neighbor : entry->record.first_neighbors) {
                edge_block_entry_t const* const block = blocks.find(id_chunk_t {vertex_id, first_neighbor});
                if (!block || !is_live(block->state)) continue;
                for (id_entity_t const neighbor : block->record.neighbor_ids) {
                    if (neighbor == vertex_id) continue;
                    if (std::optional<id_chunk_t> const chunk = chunk_over_(vertices, neighbor, vertex_id); chunk)
                        if (status_t const named = blocks.request(*chunk); named.failed()) return named;
                }
            }
        }
        if (status_t const read = blocks.resolve(*half_edges_, channel); read.failed()) return read;

        st::basic_flat_set<id_entity_t> dropped_edges;
        st::basic_vector<id_entity_t> owned;
        st::basic_vector<labeled_half_edge_t> held;
        for (id_entity_t const vertex_id : vertex_ids) {
            vertex_entry_t* entry = vertices.find(vertex_id);
            if (!entry || !is_live(entry->state)) continue;

            owned.clear();
            for (id_entity_t const first_neighbor : entry->record.first_neighbors)
                if (owned.push_back(id_entity_t {first_neighbor}) != st::status_t::success_k)
                    return {status_code_t::out_of_memory_k, "No room to list the edge blocks of a vertex"};

            for (id_entity_t const first_neighbor : owned) {
                id_chunk_t const chunk {vertex_id, first_neighbor};
                edge_block_entry_t const* const block = blocks.find(chunk);
                if (!block || !is_live(block->state)) continue;

                held.clear();
                for (std::size_t offset = 0; offset != block->record.size(); ++offset)
                    if (held.push_back(block->record.at_offset(offset)) != st::status_t::success_k)
                        return {status_code_t::out_of_memory_k, "No room to list the half-edges of a block"};

                for (labeled_half_edge_t const& half_edge : held) {
                    if (st::status_t const counted = dropped_edges.upsert(id_entity_t {half_edge.edge_id});
                        counted != st::status_t::success_k)
                        return to_status(counted);
                    if (half_edge.neighbor_id == vertex_id) continue;

                    labeled_half_edge_t reverse = half_edge;
                    reverse.neighbor_id = vertex_id;
                    reverse.role = opposite(half_edge.role);
                    edge_update_t one = edge_update_t::unchanged_k;
                    if (status_t const cut = remove_half_edge_(vertices, blocks, half_edge.neighbor_id, reverse, one);
                        cut.failed())
                        return cut;
                }

                edge_block_entry_t* const emptied = blocks.find(chunk);
                if (!emptied) return {status_code_t::internal_k, "A batch lost an edge block it had read"};
                emptied->record.clear();
                if (status_t const settled = reconcile_(vertices, blocks, chunk); settled.failed()) return settled;
            }

            entry = vertices.find(vertex_id);
            if (!entry) return {status_code_t::internal_k, "A batch lost a vertex it had read"};
            entry->record.clear();
            entry->state = record_state_t::erased_k;
            ++changed.vertices;
        }
        changed.edges = dropped_edges.size();

        if (status_t const balanced = rebalance_(channel, vertices, blocks); balanced.failed()) return balanced;
        if (status_t const written = vertices.save(*vertices_); written.failed()) return written;
        return blocks.save(*half_edges_);
    }

#pragma endregion Writes

  private:

#pragma region Reading

    /** The edge block of @p vertex_id that would carry @p neighbor_id, when the registry names one. */
    [[nodiscard]] static std::optional<id_chunk_t> chunk_over_(vertex_batch_t const& vertices, id_entity_t vertex_id,
                                                               id_entity_t neighbor_id) noexcept {
        vertex_entry_t const* const entry = vertices.find(vertex_id);
        if (!entry || !is_live(entry->state)) return std::nullopt;
        return entry->record.chunk_containing(id_chunk_t {vertex_id, neighbor_id});
    }

    /** Reads the registries of @p vertex_ids and then every edge block those registries name. */
    status_t read_adjacency_(io_channel_t& channel, std::span<id_entity_t const> vertex_ids, vertex_batch_t& vertices,
                             edge_block_batch_t& blocks) const noexcept {
        for (id_entity_t const vertex_id : vertex_ids)
            if (status_t const named = vertices.request(vertex_id); named.failed()) return named;
        if (status_t const read = vertices.resolve(*vertices_, channel); read.failed()) return read;

        for (id_entity_t const vertex_id : vertex_ids) {
            vertex_entry_t const* const entry = vertices.find(vertex_id);
            if (!entry || !is_live(entry->state)) continue;
            for (id_entity_t const first_neighbor : entry->record.first_neighbors)
                if (status_t const named = blocks.request(id_chunk_t {vertex_id, first_neighbor}); named.failed())
                    return named;
        }
        return blocks.resolve(*half_edges_, channel);
    }

    /** Calls @p visitor with every edge @p vertex_id plays @p vertex_role in, in neighbor order. */
    template <typename visitor_type_>
    status_t visit_edges_of_(io_channel_t& channel, id_entity_t vertex_id, role_t vertex_role,
                             visitor_type_&& visitor) const noexcept {
        vertex_batch_t vertices;
        edge_block_batch_t blocks;
        std::span<id_entity_t const> const only {&vertex_id, 1};
        if (status_t const read = read_adjacency_(channel, only, vertices, blocks); read.failed()) return read;

        for (std::size_t index = 0; index != blocks.size(); ++index) {
            edge_block_entry_t const& entry = blocks[index];
            if (!is_live(entry.state)) continue;
            for (labeled_half_edge_t const half_edge : entry.record) {
                labeled_edge_t const edge = half_edge.to_edge(vertex_id);
                if (edge.role_of(vertex_id) & vertex_role) visitor(edge);
            }
        }
        return {};
    }

#pragma endregion Reading

#pragma region Mutation

    /** Adds @p half_edge to the blocks of @p vertex_id, registering a new block when none of its own fits. */
    status_t add_half_edge_(vertex_batch_t& vertices, edge_block_batch_t& blocks, id_entity_t vertex_id,
                            labeled_half_edge_t const& half_edge, edge_update_t& outcome) noexcept {
        outcome = edge_update_t::unchanged_k;
        vertex_entry_t const* const vertex = vertices.find(vertex_id);
        if (!vertex) return {status_code_t::internal_k, "A batch lost a vertex it had named"};
        id_chunk_t const chunk = vertex->record.chunk_to_insert(id_chunk_t {vertex_id, half_edge.neighbor_id});

        edge_block_entry_t* block = blocks.find(chunk);
        if (!block || block->state == record_state_t::unread_k) {
            // A block the registry lists is always read, so an unknown key can only be one nobody wrote yet.
            if (vertex->record.find(chunk).exists)
                return {status_code_t::internal_k, "An edge block the batch never read"};
            expected<edge_block_entry_t*> const made = blocks.obtain(chunk);
            if (!made) return made.status();
            block = *made;
            block->record.clear();
            block->state = record_state_t::created_k;
        }

        expected<position_t> const at = block->record.upsert(half_edge);
        if (!at) return at.status();
        outcome = at->exists ? edge_update_t::unchanged_k : edge_update_t::changed_k;
        return reconcile_(vertices, blocks, chunk);
    }

    /** Takes @p half_edge out of the block of @p vertex_id that carries it, if any block does. */
    status_t remove_half_edge_(vertex_batch_t& vertices, edge_block_batch_t& blocks, id_entity_t vertex_id,
                               labeled_half_edge_t const& half_edge, edge_update_t& outcome) noexcept {
        outcome = edge_update_t::unchanged_k;
        std::optional<id_chunk_t> const chunk = chunk_over_(vertices, vertex_id, half_edge.neighbor_id);
        if (!chunk) return {};
        edge_block_entry_t* const block = blocks.find(*chunk);
        if (!block || !is_live(block->state)) return {};
        if (!block->record.remove(half_edge).exists) return {};
        outcome = edge_update_t::changed_k;
        return reconcile_(vertices, blocks, *chunk);
    }

    /** Brings the registry of a block's vertex back in step with the block, re-keying one that moved. */
    status_t reconcile_(vertex_batch_t& vertices, edge_block_batch_t& blocks, id_chunk_t chunk) noexcept {
        edge_block_entry_t* block = blocks.find(chunk);
        if (!block) return {status_code_t::internal_k, "A batch lost an edge block it had read"};
        vertex_entry_t* vertex = vertices.find(chunk.entity);
        if (!vertex) return {status_code_t::internal_k, "A batch lost a vertex it had read"};

        if (block->record.empty()) {
            [[maybe_unused]] position_t const dropped = vertex->record.erase(chunk);
            vertex->state = changed_state(vertex->state);
            block->state = record_state_t::erased_k;
            return {};
        }

        id_entity_t const first_neighbor = block->record.neighbor_ids.front();
        if (first_neighbor != chunk.chunk) {
            id_chunk_t const renamed {chunk.entity, first_neighbor};
            expected<edge_block_entry_t*> const made = blocks.obtain(renamed);
            if (!made) return made.status();
            edge_block_entry_t* const medium = *made;
            block = blocks.find(chunk);
            medium->record = std::move(block->record);
            medium->state = record_state_t::created_k;
            block->record.clear();
            block->state = record_state_t::erased_k;
            [[maybe_unused]] position_t const unlisted = vertex->record.erase(chunk);
            vertex->state = changed_state(vertex->state);
            return list_block_(*vertex, *medium);
        }

        block->state = changed_state(block->state);
        return list_block_(*vertex, *block);
    }

    /** Records the bounds and the size class of @p block in @p vertex, which a lookup then routes by. */
    static status_t list_block_(vertex_entry_t& vertex, edge_block_entry_t& block) noexcept {
        if (status_t const listed = vertex.record.upsert(static_cast<edge_block_specs_t>(block.record));
            listed.failed())
            return listed;
        vertex.state = changed_state(vertex.state);
        return {};
    }

#pragma endregion Mutation

#pragma region Rebalancing

    /** Splits every block that outgrew its bound and merges every one that fell under it. */
    status_t rebalance_(io_channel_t& channel, vertex_batch_t& vertices, edge_block_batch_t& blocks) noexcept {
        for (std::size_t index = 0; index != blocks.size(); ++index) {
            if (!is_live(blocks[index].state) || blocks[index].record.size() <= edge_block_t::max_count_k) continue;
            if (status_t const split = split_block_(vertices, blocks, blocks[index].stored_key); split.failed())
                return split;
        }

        // A block that fell under its bound merges with the one after it, which the batch may not hold yet.
        std::size_t const known = blocks.size();
        for (std::size_t index = 0; index != blocks.size(); ++index) {
            std::optional<id_chunk_t> const after = successor_of_(vertices, blocks[index]);
            if (!after || blocks.find(*after) != nullptr) continue;
            if (status_t const named = blocks.request(*after); named.failed()) return named;
        }
        if (blocks.size() != known)
            if (status_t const read = blocks.resolve(*half_edges_, channel); read.failed()) return read;

        // A block under its bound keeps taking the one after it, so a thinned adjacency folds back into few.
        for (std::size_t index = 0; index != blocks.size(); ++index)
            for (std::optional<id_chunk_t> after = successor_of_(vertices, blocks[index]); after;
                 after = successor_of_(vertices, blocks[index])) {
                id_chunk_t const chunk = blocks[index].stored_key;
                edge_block_entry_t* const block = blocks.find(chunk);
                edge_block_entry_t* const next = blocks.find(*after);
                if (!next || !is_live(next->state)) break;
                if (block->record.size() + next->record.size() > edge_block_t::max_count_k) break;

                if (status_t const taken = block->record.take_from(next->record, 0, next->record.size());
                    taken.failed())
                    return taken;
                next->record.clear();
                if (status_t const settled = reconcile_(vertices, blocks, *after); settled.failed()) return settled;
                if (status_t const settled = reconcile_(vertices, blocks, chunk); settled.failed()) return settled;
            }
        return {};
    }

    /** The block after @p entry in its vertex's registry, when @p entry is live and under its bound. */
    [[nodiscard]] static std::optional<id_chunk_t> successor_of_(vertex_batch_t const& vertices,
                                                                 edge_block_entry_t const& entry) noexcept {
        if (!is_live(entry.state) || entry.record.empty()) return std::nullopt;
        if (entry.record.size() >= edge_block_t::min_count_k) return std::nullopt;
        vertex_entry_t const* const vertex = vertices.find(entry.stored_key.entity);
        if (!vertex || !is_live(vertex->state)) return std::nullopt;
        position_t const at = vertex->record.find(entry.stored_key);
        if (!at.exists || at.offset + 1 == vertex->record.size()) return std::nullopt;
        return id_chunk_t {entry.stored_key.entity, vertex->record.first_neighbors[at.offset + 1]};
    }

    /** Cuts the block under @p chunk into chunks of the optimal size, each keyed by its first neighbor. */
    status_t split_block_(vertex_batch_t& vertices, edge_block_batch_t& blocks, id_chunk_t chunk) noexcept {
        edge_block_entry_t* const source = blocks.find(chunk);
        if (!source) return {status_code_t::internal_k, "A batch lost an edge block it had read"};
        edge_block_t whole = std::move(source->record);
        source->record.clear();
        source->state = record_state_t::erased_k;

        for (std::size_t first = 0; first != whole.size();) {
            std::size_t const last = whole.split_end(first, edge_block_t::optimal_count_k);
            id_chunk_t const part {chunk.entity, whole.neighbor_ids[first]};
            expected<edge_block_entry_t*> const made = blocks.obtain(part);
            if (!made) return made.status();
            edge_block_entry_t* const medium = *made;
            medium->record.clear();
            if (status_t const taken = medium->record.take_from(whole, first, last); taken.failed()) return taken;
            medium->state = record_state_t::created_k;

            vertex_entry_t* const vertex = vertices.find(chunk.entity);
            if (!vertex) return {status_code_t::internal_k, "A batch lost a vertex it had read"};
            if (status_t const listed = list_block_(*vertex, *medium); listed.failed()) return listed;
            first = last;
        }

        // Every half-edge moved into a part, and the first part took the key the whole block had.
        edge_block_entry_t* const emptied = blocks.find(chunk);
        if (emptied->state == record_state_t::erased_k) {
            vertex_entry_t* const vertex = vertices.find(chunk.entity);
            if (!vertex) return {status_code_t::internal_k, "A batch lost a vertex it had read"};
            [[maybe_unused]] position_t const dropped = vertex->record.erase(chunk);
            vertex->state = changed_state(vertex->state);
        }
        return {};
    }

#pragma endregion Rebalancing

#pragma region Private Members

    /** What the registries are read from and written to, borrowed and outliving this access. */
    vertices_access_type_* vertices_ = nullptr;

    /** What the edge blocks are read from and written to, borrowed and outliving this access. */
    half_edges_access_type_* half_edges_ = nullptr;

#pragma endregion Private Members
};

#pragma endregion Access

#pragma region Store

/**
 *  @brief Two single-collection databases over two volumes, read and written as one graph.
 *
 *  Each database names its schema blob the same way, so each takes a volume of its own, and each keeps
 *  two channels for its own life. The store holds all four queue pairs and hands them back when it closes.
 *
 *  Pinned rather than movable: a transaction points back at the databases here, so a store is opened into
 *  a @c std::unique_ptr and stays where it was opened.
 */
template <typename volume_type_>
class basic_graph_store {
  public:

#pragma region Types

    /** The volume kind each of the two databases lives on. */
    using volume_t = volume_type_;

    /** The transport those volumes move bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_t&>().disk())>;

    /** The queue pairs the four channels of this store come from. */
    using channel_pool_t = basic_io_channel_pool<disk_t>;

    /** One borrowed queue pair, drained and given back when it dies. */
    using channel_guard_t = typename channel_pool_t::guard_t;

    /** The database holding one registry per vertex. */
    using vertices_t =
        basic_single_collection_database<key_slot<id_entity_t, value_layout_t::variable_width_k>, volume_t>;

    /** The database holding the edge blocks of every vertex. */
    using half_edges_t =
        basic_single_collection_database<key_slot<id_chunk_t, value_layout_t::variable_width_k>, volume_t>;

    /** Every graph call, over the two databases themselves. */
    using access_t = basic_graph_access<vertices_t, half_edges_t>;

    /** A store, which is pinned and therefore always owned through a pointer. */
    using graph_store_unique_ptr_t = std::unique_ptr<basic_graph_store>;

#pragma endregion Types

#pragma region Construction

    basic_graph_store(basic_graph_store const&) = delete;
    basic_graph_store& operator=(basic_graph_store const&) = delete;
    basic_graph_store(basic_graph_store&&) = delete;
    basic_graph_store& operator=(basic_graph_store&&) = delete;

    /**
     *  @brief Opens both databases of a graph, each over a volume and two channels of its own.
     *  @param[inout] vertices_volume The volume the registries land on, borrowed and outliving the store.
     *  @param[inout] half_edges_volume The volume the edge blocks land on, which is not @p vertices_volume.
     *  @param[inout] channels The pool the four channels this store keeps are taken from.
     *  @param[in] config The bounds both databases are opened with.
     *  @param[in] resources The budget every write buffer, cache and transaction is charged to.
     */
    static expected<graph_store_unique_ptr_t> open(volume_t& vertices_volume, volume_t& half_edges_volume,
                                                   channel_pool_t& channels, graph_store_config_t const& config,
                                                   resources_shared_ptr_t resources) noexcept {
        if (&vertices_volume == &half_edges_volume)
            return status_t {status_code_t::invalid_argument_k, "A graph's two databases take two volumes"};

        graph_store_unique_ptr_t store {new (std::nothrow) basic_graph_store {}};
        if (!store) return status_t {status_code_t::out_of_memory_k, "The graph store did not fit"};
        channel_guard_t* const guards[] = {&store->vertices_writing_, &store->vertices_merging_,
                                           &store->half_edges_writing_, &store->half_edges_merging_};
        for (channel_guard_t* const guard : guards) {
            expected<channel_guard_t> taken = channels.acquire();
            if (!taken) return taken.status();
            *guard = std::move(*taken);
        }

        single_collection_database_config_t const bounds = config.databases();
        expected<typename vertices_t::database_unique_ptr_t> opened_vertices = vertices_t::open(
            vertices_volume, store->vertices_writing_.channel(), store->vertices_merging_.channel(), bounds, resources);
        if (!opened_vertices) return opened_vertices.status();
        store->vertices_ = std::move(*opened_vertices);

        expected<typename half_edges_t::database_unique_ptr_t> opened_half_edges = half_edges_t::open(
            half_edges_volume, store->half_edges_writing_.channel(), store->half_edges_merging_.channel(), bounds,
            std::move(resources));
        if (!opened_half_edges) return opened_half_edges.status();
        store->half_edges_ = std::move(*opened_half_edges);
        return store;
    }

    /** Drops the data of both databases, leaving their volumes empty of it. */
    status_t destroy() noexcept {
        status_t first = half_edges_->destroy();
        if (status_t const emptied = vertices_->destroy(); emptied.failed() && first.succeeded()) first = emptied;
        return first;
    }

#pragma endregion Construction

#pragma region Shape

    /** The database holding one registry per vertex. */
    [[nodiscard]] vertices_t& vertices() noexcept { return *vertices_; }

    /** The database holding the edge blocks of every vertex. */
    [[nodiscard]] half_edges_t& half_edges() noexcept { return *half_edges_; }

    /** Every graph call, over the two databases themselves rather than over a transaction. */
    [[nodiscard]] access_t access() noexcept { return access_t {*vertices_, *half_edges_}; }

    /** Writes everything staged in both databases as one run each, so a reopen finds it. */
    status_t flush() noexcept {
        if (status_t const written = vertices_->flush(); written.failed()) return written;
        return half_edges_->flush();
    }

#pragma endregion Shape

  private:

    basic_graph_store() noexcept = default;

#pragma region Private Members

    /** The channel the vertices database writes, flushes and commits on, held for this store's life. */
    channel_guard_t vertices_writing_;

    /** The second channel a compaction of the vertices database writes its new runs on. */
    channel_guard_t vertices_merging_;

    /** The channel the half-edges database writes, flushes and commits on, held for this store's life. */
    channel_guard_t half_edges_writing_;

    /** The second channel a compaction of the half-edges database writes its new runs on. */
    channel_guard_t half_edges_merging_;

    /** The database holding one registry per vertex, which dies before its channels do. */
    typename vertices_t::database_unique_ptr_t vertices_;

    /** The database holding the edge blocks of every vertex, which dies before its channels do. */
    typename half_edges_t::database_unique_ptr_t half_edges_;

#pragma endregion Private Members
};

#pragma endregion Store

#pragma region Materialization

/**
 *  @brief The graph of a host that keeps its volumes in files, which is the shape the C ABI names.
 *
 *  The transport is fixed here rather than templated because the layer above addresses a graph by its
 *  vertices alone; a host on another transport instantiates @c basic_graph_store over that volume itself.
 */
using graph_store_t = basic_graph_store<basic_extent_volume<blocking_files_disk_t>>;

#pragma endregion Materialization

#pragma endregion Graph Store

#pragma region Graph Transaction

#pragma region Transaction

/**
 *  @brief An indivisible group of reads and writes over the vertices and the half-edges of one graph.
 *
 *  Reusable after a @c commit and after a @c rollback, both of which start it again from the moment they
 *  ran. Nothing throws: every modifying call answers a status, and a caller reading
 *  @c status_code_t::write_conflict_k rolls back and does the work again.
 */
template <typename graph_store_type_>
class basic_graph_transaction {
  public:

#pragma region Types

    /** The store both halves of this transaction commit into. */
    using graph_store_t = graph_store_type_;

    /** One optimistic transaction over the registries. */
    using vertices_transaction_t = typename graph_store_t::vertices_t::transaction_t;

    /** One optimistic transaction over the edge blocks. */
    using half_edges_transaction_t = typename graph_store_t::half_edges_t::transaction_t;

    /** Every graph call, over the two transactions rather than over the databases. */
    using access_t = basic_graph_access<vertices_transaction_t, half_edges_transaction_t>;

    /** A transaction, owned by whoever began it and retired when it dies. */
    using graph_transaction_unique_ptr_t = std::unique_ptr<basic_graph_transaction>;

#pragma endregion Types

#pragma region Construction

    basic_graph_transaction(basic_graph_transaction const&) = delete;
    basic_graph_transaction& operator=(basic_graph_transaction const&) = delete;
    basic_graph_transaction(basic_graph_transaction&&) = delete;
    basic_graph_transaction& operator=(basic_graph_transaction&&) = delete;

    /**
     *  @brief Begins one transaction over each of @p store's databases, both under @p id.
     *  @param[inout] store The graph both halves commit into, borrowed and outliving the transaction.
     *  @param[in] id The identifier both registries hold a roster slot under, unique among live transactions.
     */
    static expected<graph_transaction_unique_ptr_t> begin(graph_store_t& store, transaction_id_t id) noexcept {
        graph_transaction_unique_ptr_t transaction {new (std::nothrow) basic_graph_transaction {}};
        if (!transaction) return status_t {status_code_t::out_of_memory_k, "The graph transaction did not fit"};

        expected<typename graph_store_t::vertices_t::transaction_unique_ptr_t> vertices =
            store.vertices().create_transaction(id);
        if (!vertices) return vertices.status();
        transaction->vertices_ = std::move(*vertices);

        expected<typename graph_store_t::half_edges_t::transaction_unique_ptr_t> half_edges =
            store.half_edges().create_transaction(id);
        if (!half_edges) return half_edges.status();
        transaction->half_edges_ = std::move(*half_edges);
        return transaction;
    }

#pragma endregion Construction

#pragma region Two Phase Commit

    /** Every graph call, over the two transactions, which answer this transaction's own writes first. */
    [[nodiscard]] access_t access() noexcept { return access_t {*vertices_, *half_edges_}; }

    /**
     *  @brief Stages both halves and then lands both, starting the transaction again.
     *
     *  Both halves take their write paths before either lands, so a reader that consults a registry before
     *  the blocks it names waits behind the half that has not landed rather than reading across the two.
     *  A failure anywhere rolls back whichever half still holds a queue pair, since one left staged would hold
     *  its database's write path against every reader.
     */
    status_t commit() noexcept {
        if (status_t const staged = vertices_->stage(); staged.failed()) return staged;
        if (status_t const staged = half_edges_->stage(); staged.failed()) {
            vertices_->rollback();
            return staged;
        }
        if (status_t const landed = vertices_->commit(); landed.failed()) {
            half_edges_->rollback();
            return landed;
        }
        return half_edges_->commit();
    }

    /** Drops every change and every watch of both halves, and starts this transaction again from here. */
    void rollback() noexcept {
        vertices_->rollback();
        half_edges_->rollback();
    }

#pragma endregion Two Phase Commit

#pragma region Stamps

    /** The stamp the registries' half was created or started again at. */
    [[nodiscard]] transaction_stamp_t start_stamp() const noexcept { return vertices_->start_stamp(); }

    /** The stamp of the last commit of the registries' half that landed. */
    [[nodiscard]] transaction_stamp_t last_commit_stamp() const noexcept { return vertices_->last_commit_stamp(); }

#pragma endregion Stamps

  private:

    basic_graph_transaction() noexcept = default;

#pragma region Private Members

    /** The half committing the registries, which lands before the edge blocks do. */
    typename graph_store_t::vertices_t::transaction_unique_ptr_t vertices_;

    /** The half committing the edge blocks, which lands after the registries do. */
    typename graph_store_t::half_edges_t::transaction_unique_ptr_t half_edges_;

#pragma endregion Private Members
};

#pragma endregion Transaction

#pragma region Materialization

/** One transaction over the graph the C ABI names. */
using graph_transaction_t = basic_graph_transaction<graph_store_t>;

#pragma endregion Materialization

#pragma endregion Graph Transaction

} // namespace unum::ustore::graph
