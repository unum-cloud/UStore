/**
 *  @file include/ustore/graph/adjacency_fold.hpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief The graph adjacency fold: edge blocks of sorted half-edges as the base, half-edge inserts and removals as
 *      deltas, reads that merge both in one pass, and the rebalance that folds the deltas into blocks of 16 to 512.
 *
 *  The base of a vertex is its registry, the directory of its edge blocks, and the blocks it lists, each keyed by the
 *  vertex and its first neighbor. A write appends delta records instead of rewriting a block: an insert or a removal
 *  of one half-edge, filed under the vertex and the neighbor, or a removal of the whole vertex, filed under the vertex
 *  and the nil key, which orders before every block of it. Half-edges order by neighbor, edge and role, which is also
 *  what names one, so an insert of a half-edge already held overwrites its weight and label.
 *
 *  A read composes the pending deltas of a vertex into one sorted chain, then walks the registry, the blocks it needs
 *  and that chain together, never materializing the merged adjacency. Compaction calls @ref rebalance, which merges
 *  every touched block with its deltas and splits or merges the results back into 16 to 512 half-edges, every
 *  half-edge to one neighbor staying in one block, and rewrites the registry entries to match.
 *
 *  @code{.c}
 *  struct half_edge_delta {      // 64 bytes, native byte order like the blocks
 *      uint8_t vertex[16];       // offset 0
 *      uint8_t neighbor[16];     // offset 16, nil for a vertex removal
 *      uint8_t edge[16];         // offset 32, nil for a vertex removal
 *      float weight;             // offset 48
 *      int32_t label;            // offset 52
 *      uint8_t role;             // offset 56
 *      uint8_t operation;        // offset 57: 0 insert, 1 remove, 2 remove the vertex
 *      uint8_t reserved[6];      // offset 58, zero
 *  };
 *  @endcode
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint16_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::sort`, `std::max`
#include <limits>    // `std::numeric_limits`
#include <memory>    // `std::allocator_traits`
#include <new>       // placement `new`
#include <span>      // `std::span`
#include <utility>   // `std::move`

#include "ustore/graph/edge_block.hpp"      // `labeled_half_edge_t`, `edge_block_t`, `id_chunk_t`, `role_t`
#include "ustore/graph/vertex_registry.hpp" // `vertex_registry_t`
#include "ustore/utilities/algorithm.hpp"   // `compare_128`, `intersect_sorted_128`, `sorted_128_cursor_concept`
#include "ustore/utilities/folds.hpp"       // `delta_kind_t`, `delta_view_t`, `fold_concept`
#include "ustore/utilities/types.hpp"       // `byte_t`, `expected`, `key_after`, `largest_key_k`

namespace unum::ustore::graph {

using utilities::byte_t;
using utilities::compare_128;
using utilities::delta_kind_t;
using utilities::delta_view_t;
using utilities::intersect_sorted_128;
using utilities::smallest_key_k;

#pragma region Delta Records

/** What one delta record does to the adjacency of its vertex. */
enum class half_edge_operation_t : std::uint8_t {

    /** Adds a half-edge, or overwrites the weight and label of the one with its neighbor, edge and role. */
    insert_k = 0,

    /** Removes the half-edge with its neighbor, edge and role, if there is one. */
    remove_k = 1,

    /** Removes every half-edge of the vertex, closing every older record and block of it. */
    remove_vertex_k = 2,
};

/** One write to the adjacency of a vertex: a half-edge insert or removal, or a removal of the vertex. */
struct half_edge_delta_t {
    id_entity_t vertex {};
    id_entity_t neighbor {};
    id_entity_t edge {};
    float weight = 1;
    label_t label;
    role_t role;
    half_edge_operation_t operation = half_edge_operation_t::insert_k;

    /** The record adding @p half_edge to @p vertex. */
    static constexpr half_edge_delta_t insert(id_entity_t vertex, labeled_half_edge_t const& half_edge) noexcept {
        return {vertex,
                half_edge.neighbor_id,
                half_edge.edge_id,
                half_edge.edge_weight,
                half_edge.edge_label,
                half_edge.role,
                half_edge_operation_t::insert_k};
    }

    /** The record removing the half-edge of @p vertex to @p neighbor through @p edge in @p role. */
    static constexpr half_edge_delta_t remove(id_entity_t vertex, id_entity_t neighbor, id_entity_t edge,
                                              role_t role) noexcept {
        return {vertex, neighbor, edge, 0, label_t {}, role, half_edge_operation_t::remove_k};
    }

    /** The record removing every half-edge of @p vertex. */
    static constexpr half_edge_delta_t remove_vertex(id_entity_t vertex) noexcept {
        return {vertex, {}, {}, 0, label_t {}, role_t {}, half_edge_operation_t::remove_vertex_k};
    }

    /** The half-edge an insert adds. */
    constexpr labeled_half_edge_t half_edge() const noexcept {
        labeled_half_edge_t result;
        result.neighbor_id = neighbor;
        result.edge_id = edge;
        result.edge_weight = weight;
        result.edge_label = label;
        result.role = role;
        return result;
    }

    /** The key the record is filed under, its delta flag set by the key slot: the vertex and the neighbor, or the
     *  nil key for a removal of the vertex, which orders before every block of it. */
    constexpr id_chunk_t key() const noexcept {
        return {vertex, operation == half_edge_operation_t::remove_vertex_k ? smallest_key_k : neighbor};
    }
};

/** Bytes one serialized delta record takes. */
inline constexpr std::size_t half_edge_delta_bytes_k = 64;

/** Where each field of a serialized delta record starts. */
inline constexpr std::size_t delta_vertex_offset_k = 0, delta_neighbor_offset_k = 16, delta_edge_offset_k = 32,
                             delta_weight_offset_k = 48, delta_label_offset_k = 52, delta_role_offset_k = 56,
                             delta_operation_offset_k = 57, delta_reserved_offset_k = 58;

/** Writes @p delta into the 64 bytes at @p destination. */
inline void save_delta(half_edge_delta_t const& delta, byte_t* destination) noexcept {
    std::memset(destination, 0, half_edge_delta_bytes_k);
    std::memcpy(destination + delta_vertex_offset_k, &delta.vertex, 16);
    std::memcpy(destination + delta_neighbor_offset_k, &delta.neighbor, 16);
    std::memcpy(destination + delta_edge_offset_k, &delta.edge, 16);
    std::memcpy(destination + delta_weight_offset_k, &delta.weight, 4);
    std::memcpy(destination + delta_label_offset_k, &delta.label.raw, 4);
    std::memcpy(destination + delta_role_offset_k, &delta.role.raw, 1);
    std::memcpy(destination + delta_operation_offset_k, &delta.operation, 1);
}

/** Reads the 64 bytes at @p source, which @ref check_delta accepted. */
inline half_edge_delta_t load_delta(byte_t const* source) noexcept {
    half_edge_delta_t delta;
    std::memcpy(&delta.vertex, source + delta_vertex_offset_k, 16);
    std::memcpy(&delta.neighbor, source + delta_neighbor_offset_k, 16);
    std::memcpy(&delta.edge, source + delta_edge_offset_k, 16);
    std::memcpy(&delta.weight, source + delta_weight_offset_k, 4);
    std::memcpy(&delta.label.raw, source + delta_label_offset_k, 4);
    std::memcpy(&delta.role.raw, source + delta_role_offset_k, 1);
    std::memcpy(&delta.operation, source + delta_operation_offset_k, 1);
    return delta;
}

/** Refuses a record with an unknown operation, a set reserved byte, or a half-edge to the nil key. */
inline status_t check_delta(byte_t const* source) noexcept {
    auto const operation = static_cast<std::uint8_t>(source[delta_operation_offset_k]);
    if (operation > static_cast<std::uint8_t>(half_edge_operation_t::remove_vertex_k))
        return {status_code_t::malformed_k, "A half-edge delta has an unknown operation"};
    for (std::size_t offset = delta_reserved_offset_k; offset != half_edge_delta_bytes_k; ++offset)
        if (source[offset] != byte_t {0}) return {status_code_t::malformed_k, "A half-edge delta sets reserved bytes"};
    if (operation != static_cast<std::uint8_t>(half_edge_operation_t::remove_vertex_k) &&
        compare_128(source + delta_neighbor_offset_k, &smallest_key_k) == 0)
        return {status_code_t::malformed_k, "A half-edge delta names the nil neighbor"};
    return {};
}

/** The records an edge writes, one per endpoint, or one for an undirected loop whose ends are one half-edge. */
inline std::size_t deltas_of_edge(labeled_edge_t const& edge, half_edge_operation_t operation,
                                  half_edge_delta_t (&deltas)[2]) noexcept {
    std::size_t count = 0;
    for (id_entity_t const member : {edge.first, edge.second}) {
        labeled_half_edge_t const half_edge(edge, member);
        bool const is_second_of_loop = count == 1 && edge.is_loop();
        // A directed loop is filed once as its source and once as its target
        role_t const role = is_second_of_loop && edge.direction == direction_t::directed_k ? dedge_target_k
                                                                                           : half_edge.role;
        if (is_second_of_loop && role == deltas[0].role) break;
        half_edge_delta_t delta = half_edge_delta_t::insert(member, half_edge);
        delta.role = role;
        if (operation == half_edge_operation_t::remove_k)
            delta = half_edge_delta_t::remove(member, half_edge.neighbor_id, half_edge.edge_id, role);
        deltas[count++] = delta;
    }
    return count;
}

/** Serializes @p deltas back to back into a block of @p allocator, a chain one delta view carries. */
template <typename allocator_type_>
expected<std::span<byte_t const>> encode_deltas(std::span<half_edge_delta_t const> deltas,
                                                allocator_type_& allocator) noexcept {
    if (deltas.empty()) return std::span<byte_t const> {};
    std::size_t const bytes = deltas.size() * half_edge_delta_bytes_k;
    byte_t* const block = allocator.allocate(bytes);
    if (!block) return status_t {status_code_t::out_of_memory_k, "No room for a chain of half-edge deltas"};
    for (std::size_t index = 0; index != deltas.size(); ++index)
        save_delta(deltas[index], block + index * half_edge_delta_bytes_k);
    return std::span<byte_t const>(block, bytes);
}

/** A chain of serialized delta records, read in place. */
class delta_chain_view {
  public:
    delta_chain_view() noexcept = default;

    /** The chain @p bytes holds, refusing a ragged length or a malformed record. */
    static expected<delta_chain_view> make(std::span<byte_t const> bytes) noexcept {
        if (bytes.size() % half_edge_delta_bytes_k)
            return status_t {status_code_t::malformed_k, "A chain of half-edge deltas has a ragged length"};
        for (std::size_t offset = 0; offset != bytes.size(); offset += half_edge_delta_bytes_k)
            if (status_t const status = check_delta(bytes.data() + offset); status.failed()) return status;
        delta_chain_view view;
        view.bytes_ = bytes;
        return view;
    }

    /** Records the chain holds. */
    std::size_t size() const noexcept { return bytes_.size() / half_edge_delta_bytes_k; }

    /** The record at @p index. */
    half_edge_delta_t operator[](std::size_t index) const noexcept { return load_delta(record(index)); }

    /** The bytes of the record at @p index. */
    byte_t const* record(std::size_t index) const noexcept { return bytes_.data() + index * half_edge_delta_bytes_k; }

    /** The operation of the record at @p index. */
    half_edge_operation_t operation(std::size_t index) const noexcept {
        byte_t const* const bytes = record(index);
        return static_cast<half_edge_operation_t>(bytes[delta_operation_offset_k]);
    }

    /** The first index in `[first, last)` whose neighbor does not order before @p neighbor, in a sorted chain. */
    std::size_t lower_bound(std::size_t first, std::size_t last, id_entity_t const& neighbor) const noexcept {
        while (first != last) {
            std::size_t const middle = first + (last - first) / 2;
            if (compare_128(record(middle) + delta_neighbor_offset_k, &neighbor) < 0) first = middle + 1;
            else last = middle;
        }
        return first;
    }

  private:
    std::span<byte_t const> bytes_;
};

#pragma endregion Delta Records

#pragma region Edge Block Views

/** Bytes an edge block of @p count half-edges takes as `edge_block_t::save` writes it. */
constexpr std::size_t encoded_block_bytes(std::size_t count) noexcept {
    return count ? sizeof(chunk_len_t) + 5 * sizeof(std::uint64_t) + count * (16 + 16 + 4 + 1 + 4)
                 : sizeof(chunk_len_t);
}

/** Orders two half-edges of one vertex by neighbor, edge and role, which is also what names one. */
inline int compare_half_edges(void const* neighbor, void const* edge, std::uint8_t role, void const* other_neighbor,
                              void const* other_edge, std::uint8_t other_role) noexcept {
    if (int const order = compare_128(neighbor, other_neighbor); order) return order;
    if (int const order = compare_128(edge, other_edge); order) return order;
    return int(role) - int(other_role);
}

/** A serialized edge block, read in place: the layout `edge_block_t::save` writes, without decoding it. */
class edge_block_view {
  public:
    edge_block_view() noexcept = default;

    /** The block @p bytes holds, where no bytes stand for no block, refusing any other layout. */
    static expected<edge_block_view> make(std::span<byte_t const> bytes) noexcept {
        edge_block_view view;
        if (bytes.empty()) return view;
        chunk_len_t count = 0;
        if (bytes.size() < sizeof(count)) return status_t {status_code_t::malformed_k, "An edge block is truncated"};
        std::memcpy(&count, bytes.data(), sizeof(count));
        if (bytes.size() != encoded_block_bytes(count))
            return status_t {status_code_t::malformed_k, "An edge block has the wrong length"};
        view.count_ = count;
        if (!count) return view;
        std::size_t offset = sizeof(count);
        byte_t const* columns[5] = {};
        std::size_t const widths[5] = {16, 16, 4, 1, 4};
        for (std::size_t column = 0; column != 5; ++column) {
            std::uint64_t length = 0;
            std::memcpy(&length, bytes.data() + offset, sizeof(length));
            if (length != count)
                return status_t {status_code_t::malformed_k, "An edge block column disagrees with its count"};
            columns[column] = bytes.data() + offset + sizeof(length);
            offset += sizeof(length) + count * widths[column];
        }
        view.neighbors_ = columns[0], view.edges_ = columns[1], view.labels_ = columns[2];
        view.roles_ = columns[3], view.weights_ = columns[4];
        return view;
    }

    /** Half-edges the block holds. */
    std::size_t size() const noexcept { return count_; }

    /** The bytes of the neighbor at @p index, compared in place. */
    byte_t const* neighbor_bytes(std::size_t index) const noexcept { return neighbors_ + index * 16; }

    /** The bytes of the edge at @p index, compared in place. */
    byte_t const* edge_bytes(std::size_t index) const noexcept { return edges_ + index * 16; }

    /** The role bits of the half-edge at @p index. */
    std::uint8_t role_raw(std::size_t index) const noexcept { return static_cast<std::uint8_t>(roles_[index]); }

    /** The neighbor of the half-edge at @p index. */
    id_entity_t neighbor(std::size_t index) const noexcept {
        id_entity_t key;
        std::memcpy(&key, neighbor_bytes(index), 16);
        return key;
    }

    /** The half-edge at @p index. */
    labeled_half_edge_t operator[](std::size_t index) const noexcept {
        labeled_half_edge_t half_edge;
        std::memcpy(&half_edge.neighbor_id, neighbor_bytes(index), 16);
        std::memcpy(&half_edge.edge_id, edge_bytes(index), 16);
        std::memcpy(&half_edge.edge_label.raw, labels_ + index * 4, 4);
        half_edge.role.raw = role_raw(index);
        std::memcpy(&half_edge.edge_weight, weights_ + index * 4, 4);
        return half_edge;
    }

    /** Whether the half-edges strictly ascend by neighbor, edge and role, as every fold writes them. */
    bool sorted() const noexcept {
        for (std::size_t index = 1; index < count_; ++index)
            if (compare_half_edges(neighbor_bytes(index - 1), edge_bytes(index - 1), role_raw(index - 1),
                                   neighbor_bytes(index), edge_bytes(index), role_raw(index)) >= 0)
                return false;
        return true;
    }

    /** The first index in `[first, size)` whose neighbor does not order before @p neighbor. */
    std::size_t lower_bound(std::size_t first, id_entity_t const& neighbor) const noexcept {
        std::size_t last = count_;
        while (first != last) {
            std::size_t const middle = first + (last - first) / 2;
            if (compare_128(neighbor_bytes(middle), &neighbor) < 0) first = middle + 1;
            else last = middle;
        }
        return first;
    }

  private:
    byte_t const* neighbors_ = nullptr;
    byte_t const* edges_ = nullptr;
    byte_t const* labels_ = nullptr;
    byte_t const* roles_ = nullptr;
    byte_t const* weights_ = nullptr;
    std::size_t count_ = 0;
};

/** Writes half-edges into a serialized edge block, one index at a time. */
class edge_block_writer {
  public:

    /** A writer of @p count half-edges into @p destination, which spans `encoded_block_bytes(count)`. */
    edge_block_writer(byte_t* destination, std::size_t count) noexcept {
        chunk_len_t const length = static_cast<chunk_len_t>(count);
        std::memcpy(destination, &length, sizeof(length));
        if (!count) return;
        std::size_t offset = sizeof(length);
        std::size_t const widths[5] = {16, 16, 4, 1, 4};
        for (std::size_t column = 0; column != 5; ++column) {
            std::uint64_t const wide = count;
            std::memcpy(destination + offset, &wide, sizeof(wide));
            columns_[column] = destination + offset + sizeof(wide);
            offset += sizeof(wide) + count * widths[column];
        }
    }

    /** Stores @p half_edge at @p index. */
    void set(std::size_t index, labeled_half_edge_t const& half_edge) noexcept {
        std::memcpy(columns_[0] + index * 16, &half_edge.neighbor_id, 16);
        std::memcpy(columns_[1] + index * 16, &half_edge.edge_id, 16);
        std::memcpy(columns_[2] + index * 4, &half_edge.edge_label.raw, 4);
        std::memcpy(columns_[3] + index, &half_edge.role.raw, 1);
        std::memcpy(columns_[4] + index * 4, &half_edge.edge_weight, 4);
    }

  private:
    byte_t* columns_[5] = {};
};

#pragma endregion Edge Block Views

#pragma region Merging Cursor

/**
 *  @brief The blocks of one vertex as its registry lists them, fetched by key only when a walk needs them.
 *  @tparam fetch_type_ Takes an `id_chunk_t` and answers `expected<std::span<byte_t const>>`, bytes that outlive the
 *      walk.
 */
template <typename fetch_type_>
class registry_blocks {
  public:

    /** The blocks @p registry lists, read through @p fetch. */
    registry_blocks(vertex_registry_t const& registry, fetch_type_ fetch) noexcept
        : registry_(&registry), fetch_(std::move(fetch)) {}

    /** Blocks the registry lists. */
    std::size_t size() const noexcept { return registry_->size(); }

    /** The first neighbor of the block at @p index, which keys it. */
    id_entity_t first_neighbor(std::size_t index) const noexcept { return registry_->first_neighbors[index]; }

    /** The key of the block at @p index. */
    id_chunk_t key(std::size_t index) const noexcept { return {registry_->vertex_info.id, first_neighbor(index)}; }

    /** The bytes of the block at @p index. */
    expected<std::span<byte_t const>> fetch(std::size_t index) const noexcept { return fetch_(key(index)); }

    /** The registry the blocks are listed in. */
    vertex_registry_t const& registry() const noexcept { return *registry_; }

  private:
    vertex_registry_t const* registry_ = nullptr;
    fetch_type_ fetch_;
};

/** One block given as bytes, which @c adjacency_fold::apply merges with its deltas. */
class single_block {
  public:

    /** The block @p bytes holds, which must outlive every walk over it. */
    explicit single_block(std::span<byte_t const> bytes) noexcept : bytes_(bytes) {}

    /** One block, whatever it holds. */
    std::size_t size() const noexcept { return 1; }

    /** The nil key, since a lone block bounds nothing. */
    id_entity_t first_neighbor(std::size_t) const noexcept { return smallest_key_k; }

    /** The bytes of the block. */
    expected<std::span<byte_t const>> fetch(std::size_t) const noexcept { return bytes_; }

  private:
    std::span<byte_t const> bytes_;
};

/**
 *  @brief Walks the live half-edges of a run of blocks merged with a sorted chain of deltas, in one pass.
 *
 *  An insert in the chain adds or overwrites the half-edge it names, and a removal hides the one it names, so the
 *  walk yields exactly what folding the chain into the blocks would store. Blocks are fetched only when the walk
 *  reaches them, and a seek leaps over the blocks and records that order before the neighbor it seeks.
 */
template <typename blocks_type_>
class half_edge_cursor {
  public:
    half_edge_cursor() noexcept = default;

    /** A walk over the blocks `[block_first, block_last)` of @p blocks and the records `[delta_first, delta_last)` of
     *  @p deltas, both of which must outlive it, positioned with @ref start. */
    half_edge_cursor(blocks_type_ const& blocks, std::size_t block_first, std::size_t block_last,
                     delta_chain_view deltas, std::size_t delta_first, std::size_t delta_last) noexcept
        : blocks_(&blocks), deltas_(deltas), block_index_(block_first), block_last_(block_last),
          delta_offset_(delta_first), delta_last_(delta_last) {}

    /** Moves onto the first live half-edge. */
    status_t start() noexcept { return settle_(); }

    /** Whether the walk stands on a half-edge. */
    bool valid() const noexcept { return valid_; }

    /** The half-edge the walk stands on, while it is valid. */
    labeled_half_edge_t const& current() const noexcept { return current_; }

    /** Steps to the next live half-edge. */
    status_t next() noexcept {
        if (from_block_) ++block_offset_;
        if (from_delta_) ++delta_offset_;
        return settle_();
    }

    /** Leaps to the first live half-edge whose neighbor does not order before @p neighbor, never backwards. */
    status_t seek_neighbor(id_entity_t const& neighbor) noexcept {
        if (valid_ && compare_128(current_.neighbor_id, neighbor) >= 0) return {};
        delta_offset_ = deltas_.lower_bound(delta_offset_, delta_last_, neighbor);
        if (block_index_ < block_last_) {
            // The last block starting at or before the neighbor is the only one that may hold it
            std::size_t first = block_index_ + 1, last = block_last_;
            while (first != last) {
                std::size_t const middle = first + (last - first) / 2;
                if (compare_128(blocks_->first_neighbor(middle), neighbor) <= 0) first = middle + 1;
                else last = middle;
            }
            std::size_t const target = first - 1;
            if (target != block_index_) block_index_ = target, block_offset_ = 0;
            if (status_t const status = load_(); status.failed()) return status;
            block_offset_ = block_.lower_bound(block_offset_, neighbor);
        }
        return settle_();
    }

  private:

    /** Fetches the block the walk stands in, unless it already holds it. */
    status_t load_() noexcept {
        if (loaded_ == block_index_) return {};
        auto bytes = blocks_->fetch(block_index_);
        if (!bytes) return bytes.status();
        auto view = edge_block_view::make(*bytes);
        if (!view) return view.status();
        block_ = *view;
        loaded_ = block_index_;
        return {};
    }

    /** Moves onto the first live half-edge at or after the current offsets. */
    status_t settle_() noexcept {
        for (;;) {
            bool has_block = false;
            while (block_index_ < block_last_) {
                if (status_t const status = load_(); status.failed()) return status;
                if (block_offset_ < block_.size()) {
                    has_block = true;
                    break;
                }
                ++block_index_, block_offset_ = 0;
            }
            bool const has_delta = delta_offset_ < delta_last_;
            if (!has_block && !has_delta) {
                valid_ = from_block_ = from_delta_ = false;
                return {};
            }
            int order = has_block ? -1 : 1;
            byte_t const* const record = has_delta ? deltas_.record(delta_offset_) : nullptr;
            if (has_block && has_delta)
                order = compare_half_edges(block_.neighbor_bytes(block_offset_), block_.edge_bytes(block_offset_),
                                           block_.role_raw(block_offset_), record + delta_neighbor_offset_k,
                                           record + delta_edge_offset_k,
                                           static_cast<std::uint8_t>(record[delta_role_offset_k]));
            if (order < 0) {
                current_ = block_[block_offset_];
                valid_ = from_block_ = true, from_delta_ = false;
                return {};
            }
            if (deltas_.operation(delta_offset_) != half_edge_operation_t::insert_k) {
                ++delta_offset_;
                if (order == 0) ++block_offset_;
                continue;
            }
            current_ = deltas_[delta_offset_].half_edge();
            valid_ = from_delta_ = true, from_block_ = order == 0;
            return {};
        }
    }

    blocks_type_ const* blocks_ = nullptr;
    delta_chain_view deltas_;
    edge_block_view block_;
    std::size_t loaded_ = std::numeric_limits<std::size_t>::max();
    std::size_t block_index_ = 0, block_last_ = 0, block_offset_ = 0;
    std::size_t delta_offset_ = 0, delta_last_ = 0;
    labeled_half_edge_t current_;
    bool valid_ = false, from_block_ = false, from_delta_ = false;
};

/** Walks the distinct neighbors of a half-edge walk, fitting `sorted_128_cursor_concept` for 128-bit intersections. */
template <typename blocks_type_>
class neighbor_cursor {
  public:
    neighbor_cursor() noexcept = default;

    /** The neighbors of @p half_edges, a walk already started. */
    explicit neighbor_cursor(half_edge_cursor<blocks_type_> half_edges) noexcept : half_edges_(half_edges) {}

    /** Whether the walk still stands on a neighbor. */
    bool valid() const noexcept { return half_edges_.valid(); }

    /** The neighbor the walk stands on, while it is valid. */
    ustore_key_t key() const noexcept { return half_edges_.current().neighbor_id; }

    /** Steps past every half-edge to the current neighbor. */
    status_t next() noexcept {
        id_entity_t const neighbor = half_edges_.current().neighbor_id;
        if (neighbor == utilities::largest_key_k) {
            while (half_edges_.valid())
                if (status_t const status = half_edges_.next(); status.failed()) return status;
            return {};
        }
        return half_edges_.seek_neighbor(utilities::key_after(neighbor));
    }

    /** Leaps to the first neighbor not ordering before @p neighbor, never backwards. */
    status_t seek(ustore_key_t const& neighbor) noexcept { return half_edges_.seek_neighbor(neighbor); }

  private:
    half_edge_cursor<blocks_type_> half_edges_;
};

#pragma endregion Merging Cursor

#pragma region Fold

/** A decoded record with its place in the chain, which breaks ties between records of one half-edge. */
struct sequenced_delta_t {
    half_edge_delta_t delta;
    std::uint32_t sequence = 0;
};

/** Orders records by vertex, removals of the vertex first, then by neighbor, edge, role and age. */
inline bool sequenced_less(sequenced_delta_t const& first, sequenced_delta_t const& second) noexcept {
    if (int const order = compare_128(first.delta.vertex, second.delta.vertex); order) return order < 0;
    bool const first_removes = first.delta.operation == half_edge_operation_t::remove_vertex_k;
    bool const second_removes = second.delta.operation == half_edge_operation_t::remove_vertex_k;
    if (first_removes != second_removes) return first_removes;
    if (!first_removes)
        if (int const order = compare_half_edges(&first.delta.neighbor, &first.delta.edge, first.delta.role.raw,
                                                 &second.delta.neighbor, &second.delta.edge, second.delta.role.raw);
            order)
            return order < 0;
    return first.sequence < second.sequence;
}

/** Whether two records name one half-edge of one vertex. */
inline bool same_half_edge(half_edge_delta_t const& first, half_edge_delta_t const& second) noexcept {
    return first.vertex == second.vertex && first.operation != half_edge_operation_t::remove_vertex_k &&
           second.operation != half_edge_operation_t::remove_vertex_k &&
           compare_half_edges(&first.neighbor, &first.edge, first.role.raw, &second.neighbor, &second.edge,
                              second.role.raw) == 0;
}

/** Blocks fetched at most once each, for a rebalance that counts a block before it writes it. */
template <typename blocks_type_>
class memoized_blocks {
  public:

    /** Blocks of @p blocks, remembered in @p fetched, one null span per block. */
    memoized_blocks(blocks_type_ const& blocks, std::span<byte_t const>* fetched) noexcept
        : blocks_(&blocks), fetched_(fetched) {}

    /** Blocks the underlying listing holds. */
    std::size_t size() const noexcept { return blocks_->size(); }

    /** The first neighbor of the block at @p index. */
    id_entity_t first_neighbor(std::size_t index) const noexcept { return blocks_->first_neighbor(index); }

    /** The bytes of the block at @p index, fetched on the first request only. */
    expected<std::span<byte_t const>> fetch(std::size_t index) const noexcept {
        if (fetched_[index].data()) return fetched_[index];
        auto bytes = blocks_->fetch(index);
        if (bytes) fetched_[index] = *bytes;
        return bytes;
    }

  private:
    blocks_type_ const* blocks_ = nullptr;
    std::span<byte_t const>* fetched_ = nullptr;
};

/** Counts the half-edges @p cursor yields, leaving it exhausted. */
template <typename blocks_type_>
expected<std::size_t> count_half_edges(half_edge_cursor<blocks_type_>& cursor) noexcept {
    std::size_t count = 0;
    if (status_t const status = cursor.start(); status.failed()) return status;
    while (cursor.valid()) {
        ++count;
        if (status_t const status = cursor.next(); status.failed()) return status;
    }
    return count;
}

/** Serializes @p count half-edges into a block of @p allocator. */
template <typename allocator_type_>
expected<std::span<byte_t const>> encode_edge_block(labeled_half_edge_t const* half_edges, std::size_t count,
                                                    allocator_type_& allocator) noexcept {
    if (count > std::numeric_limits<chunk_len_t>::max())
        return status_t {status_code_t::capacity_exhausted_k, "An edge block outgrows its length field"};
    std::size_t const bytes = encoded_block_bytes(count);
    byte_t* const block = allocator.allocate(bytes);
    if (!block) return status_t {status_code_t::out_of_memory_k, "No room for an edge block"};
    edge_block_writer writer(block, count);
    for (std::size_t index = 0; index != count; ++index) writer.set(index, half_edges[index]);
    return std::span<byte_t const>(block, bytes);
}

/**
 *  @brief The fold of graph adjacency, fitting the fold concept of `ustore/utilities/folds.hpp`.
 *
 *  A base is one serialized edge block, a write replaces it and a removal drops it, and a change carries a chain of
 *  half-edge delta records. Applying yields the block with every record folded in, sorted, at whatever size it grows
 *  to, since splitting a block changes its key, which @ref rebalance decides with the registry in hand. Composing
 *  keeps the newest record of every half-edge and drops everything older than a removal of the vertex, after which an
 *  insert followed by a removal of one half-edge cancels outright. Without such a removal the pair still becomes one
 *  removal, because an older level may hold the half-edge it hides.
 */
struct adjacency_fold {

    /**
     *  @brief The block @p base becomes once every delta of @p deltas applies, oldest first.
     *  @return The block in a block of @p allocator, @p base itself without changes, or @c status_code_t::not_found_k
     *      when no half-edge is left.
     */
    template <typename allocator_type_>
    expected<std::span<byte_t const>> apply(std::span<byte_t const> base, std::span<delta_view_t const> deltas,
                                            allocator_type_& allocator) const noexcept {
        std::size_t first_change = 0;
        for (std::size_t index = 0; index != deltas.size(); ++index) {
            if (deltas[index].kind == delta_kind_t::change_k) continue;
            base = deltas[index].kind == delta_kind_t::write_k ? deltas[index].bytes : std::span<byte_t const> {};
            first_change = index + 1;
        }
        auto view = edge_block_view::make(base);
        if (!view) return view.status();
        if (!view->sorted()) return status_t {status_code_t::malformed_k, "An edge block is out of order"};
        std::span<delta_view_t const> const changes = deltas.subspan(first_change);
        if (changes.empty()) {
            if (!view->size()) return status_t {status_code_t::not_found_k, "The edge block holds no half-edges"};
            return base;
        }

        auto chain_bytes = compose(changes, allocator);
        if (!chain_bytes) return chain_bytes.status();
        delta_chain_view const chain = *delta_chain_view::make(*chain_bytes);
        for (std::size_t index = 1; index < chain.size(); ++index)
            if (!(chain[index].vertex == chain[0].vertex))
                return status_t {status_code_t::invalid_argument_k, "An edge block takes deltas of one vertex"};
        bool const closed = chain.size() && chain.operation(0) == half_edge_operation_t::remove_vertex_k;

        single_block const source(base);
        std::size_t const blocks = closed || !view->size() ? 0 : 1;
        half_edge_cursor<single_block> counter(source, 0, blocks, chain, closed, chain.size());
        auto count = count_half_edges(counter);
        if (!count) return count.status();
        if (!*count) return status_t {status_code_t::not_found_k, "The edge block holds no half-edges"};
        if (*count > std::numeric_limits<chunk_len_t>::max())
            return status_t {status_code_t::capacity_exhausted_k, "An edge block outgrows its length field"};

        std::size_t const bytes = encoded_block_bytes(*count);
        byte_t* const block = allocator.allocate(bytes);
        if (!block) return status_t {status_code_t::out_of_memory_k, "No room for a folded edge block"};
        edge_block_writer writer(block, *count);
        half_edge_cursor<single_block> cursor(source, 0, blocks, chain, closed, chain.size());
        if (status_t const status = cursor.start(); status.failed()) return status;
        for (std::size_t index = 0; cursor.valid(); ++index) {
            writer.set(index, cursor.current());
            if (status_t const status = cursor.next(); status.failed()) return status;
        }
        return std::span<byte_t const>(block, bytes);
    }

    /**
     *  @brief One chain equivalent to @p deltas on every base, sorted by vertex, then neighbor, edge and role.
     *
     *  Every vertex in the chain keeps at most one removal of itself, first, then the newest record of every
     *  half-edge newer than that removal. A write or a removal of a whole block is a base rather than a change, so
     *  the caller applies the chain to it instead, as @ref closes tells.
     *
     *  @return The chain in a block of @p allocator, empty when the deltas carry no records.
     */
    template <typename allocator_type_>
    expected<std::span<byte_t const>> compose(std::span<delta_view_t const> deltas,
                                              allocator_type_& allocator) const noexcept {
        if (deltas.empty()) return status_t {status_code_t::invalid_argument_k, "No deltas to compose"};
        std::size_t total = 0;
        for (delta_view_t const& delta : deltas) {
            if (delta.kind != delta_kind_t::change_k)
                return status_t {status_code_t::invalid_argument_k, "A block write or removal is a base to apply"};
            auto chain = delta_chain_view::make(delta.bytes);
            if (!chain) return chain.status();
            total += chain->size();
        }
        if (!total) return std::span<byte_t const> {};
        if (total > std::numeric_limits<std::uint32_t>::max())
            return status_t {status_code_t::capacity_exhausted_k, "Too many half-edge deltas to compose"};

        using entries_allocator_t =
            typename std::allocator_traits<allocator_type_>::template rebind_alloc<sequenced_delta_t>;
        entries_allocator_t entries_allocator(allocator);
        sequenced_delta_t* const entries = entries_allocator.allocate(total);
        if (!entries) return status_t {status_code_t::out_of_memory_k, "No room to compose half-edge deltas"};
        std::size_t count = 0;
        for (delta_view_t const& delta : deltas) {
            delta_chain_view const chain = *delta_chain_view::make(delta.bytes);
            for (std::size_t index = 0; index != chain.size(); ++index, ++count)
                new (entries + count) sequenced_delta_t {chain[index], static_cast<std::uint32_t>(count)};
        }
        std::sort(entries, entries + total, sequenced_less);

        std::size_t kept = 0;
        for (std::size_t index = 0; index != total;) {
            id_entity_t const vertex = entries[index].delta.vertex;
            bool removed = false;
            std::uint32_t removal = 0;
            for (; index != total && entries[index].delta.vertex == vertex &&
                   entries[index].delta.operation == half_edge_operation_t::remove_vertex_k;
                 ++index)
                removed = true, removal = entries[index].sequence;
            if (removed) entries[kept++].delta = half_edge_delta_t::remove_vertex(vertex);
            while (index != total && entries[index].delta.vertex == vertex) {
                std::size_t newest = index;
                while (newest + 1 != total && same_half_edge(entries[newest + 1].delta, entries[index].delta)) ++newest;
                sequenced_delta_t const winner = entries[newest];
                index = newest + 1;
                if (removed && winner.sequence < removal) continue;
                if (removed && winner.delta.operation == half_edge_operation_t::remove_k) continue;
                entries[kept] = winner;
                if (winner.delta.operation == half_edge_operation_t::remove_k)
                    entries[kept].delta = half_edge_delta_t::remove(vertex, winner.delta.neighbor, winner.delta.edge,
                                                                    winner.delta.role);
                ++kept;
            }
        }

        std::span<byte_t const> result;
        if (kept) {
            std::size_t const bytes = kept * half_edge_delta_bytes_k;
            byte_t* const block = allocator.allocate(bytes);
            if (!block) {
                entries_allocator.deallocate(entries, total);
                return status_t {status_code_t::out_of_memory_k, "No room for a composed chain"};
            }
            for (std::size_t index = 0; index != kept; ++index)
                save_delta(entries[index].delta, block + index * half_edge_delta_bytes_k);
            result = {block, bytes};
        }
        entries_allocator.deallocate(entries, total);
        return result;
    }

    /** Whether @p delta makes every older record of its key irrelevant: a block write or removal, or a chain holding
     *  a removal of the vertex. */
    bool closes(delta_view_t const& delta) const noexcept {
        if (delta.kind != delta_kind_t::change_k) return true;
        if (delta.bytes.size() % half_edge_delta_bytes_k) return false;
        for (std::size_t offset = 0; offset != delta.bytes.size(); offset += half_edge_delta_bytes_k)
            if (static_cast<half_edge_operation_t>(delta.bytes[offset + delta_operation_offset_k]) ==
                half_edge_operation_t::remove_vertex_k)
                return true;
        return false;
    }
};

static_assert(utilities::fold_concept<adjacency_fold>);

#pragma endregion Fold

#pragma region Reads

/**
 *  @brief The adjacency of one vertex as a read sees it: the blocks its registry lists, merged with the composed
 *      chain of its pending deltas, walked in place.
 *
 *  Without pending deltas every read takes the blocks-only path: a degree sums the block lengths, and a walk reads
 *  the blocks as they are.
 */
template <typename fetch_type_>
class vertex_adjacency {
  public:
    using blocks_t = registry_blocks<fetch_type_>;
    using cursor_t = half_edge_cursor<blocks_t>;

    /**
     *  @brief The adjacency @p registry lists, with @p pending, a chain @c adjacency_fold::compose wrote for this
     *      vertex alone, folded over it.
     *  @param[in] fetch Answers the bytes of a block by key, which must outlive every read.
     */
    static expected<vertex_adjacency> make(vertex_registry_t const& registry, fetch_type_ fetch,
                                           std::span<byte_t const> pending) noexcept {
        auto chain = delta_chain_view::make(pending);
        if (!chain) return chain.status();
        bool const closed = chain->size() && chain->operation(0) == half_edge_operation_t::remove_vertex_k;
        for (std::size_t index = 0; index != chain->size(); ++index) {
            half_edge_delta_t const delta = (*chain)[index];
            if (!(delta.vertex == registry.vertex_info.id))
                return status_t {status_code_t::invalid_argument_k, "Pending deltas name another vertex"};
            if (index && delta.operation == half_edge_operation_t::remove_vertex_k)
                return status_t {status_code_t::malformed_k, "Pending deltas are not composed"};
            if (index > std::size_t(closed)) {
                half_edge_delta_t const previous = (*chain)[index - 1];
                if (compare_half_edges(&previous.neighbor, &previous.edge, previous.role.raw, &delta.neighbor,
                                       &delta.edge, delta.role.raw) >= 0)
                    return status_t {status_code_t::malformed_k, "Pending deltas are not composed"};
            }
        }
        return vertex_adjacency(registry, std::move(fetch), *chain, closed);
    }

    /** A walk over every live half-edge, positioned with `start`. */
    cursor_t half_edges() const noexcept {
        return cursor_t(blocks_, 0, closed_ ? 0 : blocks_.size(), pending_, closed_, pending_.size());
    }

    /** A walk over the distinct neighbors, for `intersect_sorted_128`. */
    expected<neighbor_cursor<blocks_t>> neighbors() const noexcept {
        cursor_t cursor = half_edges();
        if (status_t const status = cursor.start(); status.failed()) return status;
        return neighbor_cursor<blocks_t>(cursor);
    }

    /** Calls @p callback with every live half-edge in order, stopping at the first failure it answers. */
    template <typename callback_type_>
    status_t for_each(callback_type_&& callback) const noexcept {
        cursor_t cursor = half_edges();
        if (status_t const status = cursor.start(); status.failed()) return status;
        while (cursor.valid()) {
            if (status_t const status = callback(cursor.current()); status.failed()) return status;
            if (status_t const status = cursor.next(); status.failed()) return status;
        }
        return {};
    }

    /** Half-edges playing any of @p roles, every one of them by default. */
    expected<std::size_t> degree(role_t roles = any_role_k) const noexcept {
        if (!pending_.size() && roles == any_role_k) {
            std::size_t count = 0;
            for (std::size_t index = 0; index != blocks_.size(); ++index) {
                auto bytes = blocks_.fetch(index);
                if (!bytes) return bytes.status();
                auto view = edge_block_view::make(*bytes);
                if (!view) return view.status();
                count += view->size();
            }
            return count;
        }
        std::size_t count = 0;
        status_t const status = for_each([&](labeled_half_edge_t const& half_edge) noexcept {
            count += (half_edge.role & roles) ? 1 : 0;
            return status_t {};
        });
        if (status.failed()) return status;
        return count;
    }

    /** Calls @p callback with every live half-edge to @p neighbor, reading at most the one block that may hold it. */
    template <typename callback_type_>
    status_t edges_to(id_entity_t const& neighbor, callback_type_&& callback) const noexcept {
        // The last block starting at or before the neighbor is the only one that may hold it
        std::size_t block = 1, after = blocks_.size();
        while (block < after) {
            std::size_t const middle = block + (after - block) / 2;
            if (compare_128(blocks_.first_neighbor(middle), neighbor) <= 0) block = middle + 1;
            else after = middle;
        }
        block = block ? block - 1 : 0;
        std::size_t const blocks_count = closed_ || !blocks_.size() ? 0 : 1;
        std::size_t const first = pending_.lower_bound(closed_, pending_.size(), neighbor);
        std::size_t const last = pending_.lower_bound(first, pending_.size(), utilities::key_after(neighbor));
        cursor_t cursor(blocks_, block, block + blocks_count, pending_, first,
                        neighbor == utilities::largest_key_k ? pending_.size() : last);
        if (status_t const status = cursor.seek_neighbor(neighbor); status.failed()) return status;
        while (cursor.valid() && cursor.current().neighbor_id == neighbor) {
            if (status_t const status = callback(cursor.current()); status.failed()) return status;
            if (status_t const status = cursor.next(); status.failed()) return status;
        }
        return {};
    }

    /** The vertex whose adjacency this is. */
    id_entity_t vertex() const noexcept { return blocks_.registry().vertex_info.id; }

  private:
    vertex_adjacency(vertex_registry_t const& registry, fetch_type_ fetch, delta_chain_view pending,
                     bool closed) noexcept
        : blocks_(registry, std::move(fetch)), pending_(pending), closed_(closed) {}

    blocks_t blocks_;
    delta_chain_view pending_;
    bool closed_ = false;
};

/** Calls @p callback with every edge between @p adjacency's vertex and @p neighbor, as @p adjacency stores them. */
template <typename fetch_type_, typename callback_type_>
status_t edges_between(vertex_adjacency<fetch_type_> const& adjacency, id_entity_t const& neighbor,
                       callback_type_&& callback) noexcept {
    id_entity_t const vertex = adjacency.vertex();
    return adjacency.edges_to(
        neighbor, [&](labeled_half_edge_t const& half_edge) noexcept { return callback(half_edge.to_edge(vertex)); });
}

/** Calls @p callback with every distinct neighbor two vertices share, in order, through a 128-bit sorted
 *  intersection that leaps over the blocks neither needs. */
template <typename first_fetch_type_, typename second_fetch_type_, typename callback_type_>
status_t common_neighbors(vertex_adjacency<first_fetch_type_> const& first,
                          vertex_adjacency<second_fetch_type_> const& second, callback_type_&& callback) noexcept {
    auto first_neighbors = first.neighbors();
    if (!first_neighbors) return first_neighbors.status();
    auto second_neighbors = second.neighbors();
    if (!second_neighbors) return second_neighbors.status();
    return intersect_sorted_128(*first_neighbors, *second_neighbors, callback);
}

#pragma endregion Reads

#pragma region Rebalance

/** A block a rebalance wrote, which the caller stores under its key. */
struct written_block_t {
    id_chunk_t key;
    std::span<byte_t const> bytes;
};

/** What a rebalance changed: blocks to store, block keys to drop, and whether a removal of the vertex closed it. */
struct rebalanced_t {
    std::span<written_block_t const> written;
    std::span<id_chunk_t const> removed;
    bool closed = false;
};

/**
 *  @brief Folds the pending @p deltas of a vertex into its blocks, then splits or merges the touched ones back into
 *      16 to 512 half-edges and lists the results in @p registry.
 *
 *  Every block whose range of neighbors a delta touches is merged with its deltas. A result over 512 splits into
 *  blocks of about 128, one under 16 merges with the next block, or the previous one when it is the last, and an
 *  empty one disappears. Blocks nothing touched stay as they are, unread, unless a neighbor merges into them. Every
 *  half-edge to one neighbor stays in one block, and a vertex with fewer than 16 half-edges keeps one small block.
 *
 *  @param[inout] registry The registry of the vertex, rewritten to list the blocks the rebalance leaves.
 *  @param[in] fetch Answers the bytes of a block by key, which must outlive the call.
 *  @param[in] deltas Changes of this vertex alone, oldest first, possibly none for a rebalance only.
 *  @return The blocks to write and the keys to drop, in blocks of @p allocator.
 */
template <typename fetch_type_, typename allocator_type_>
expected<rebalanced_t> rebalance(vertex_registry_t& registry, fetch_type_ fetch, std::span<delta_view_t const> deltas,
                                 allocator_type_& allocator) noexcept {
    using traits_t = std::allocator_traits<allocator_type_>;
    using sizes_allocator_t = typename traits_t::template rebind_alloc<std::size_t>;
    using half_edges_allocator_t = typename traits_t::template rebind_alloc<labeled_half_edge_t>;
    using written_allocator_t = typename traits_t::template rebind_alloc<written_block_t>;
    using keys_allocator_t = typename traits_t::template rebind_alloc<id_chunk_t>;
    using spans_allocator_t = typename traits_t::template rebind_alloc<std::span<byte_t const>>;
    constexpr std::size_t unknown_k = std::numeric_limits<std::size_t>::max();
    std::size_t const min_count = edge_block_t::min_count_k, max_count = edge_block_t::max_count_k;
    id_entity_t const vertex = registry.vertex_info.id;

    std::span<byte_t const> chain_bytes;
    if (!deltas.empty()) {
        auto composed = adjacency_fold {}.compose(deltas, allocator);
        if (!composed) return composed.status();
        chain_bytes = *composed;
    }
    // The same checks a read makes refuse a chain for another vertex or one not composed
    if (auto checked = vertex_adjacency<fetch_type_>::make(registry, fetch, chain_bytes); !checked)
        return checked.status();
    delta_chain_view const chain = *delta_chain_view::make(chain_bytes);
    bool const closed = chain.size() && chain.operation(0) == half_edge_operation_t::remove_vertex_k;
    registry_blocks<fetch_type_> const listing(registry, fetch);
    std::size_t const listed = listing.size();
    std::size_t const slots = listed ? listed : 1;

    // Per slot: where its deltas start and end, its merged size, whether a delta touched it, and its bytes once read
    sizes_allocator_t sizes_allocator(allocator);
    spans_allocator_t spans_allocator(allocator);
    std::size_t const scratch_size = slots * 4;
    std::size_t* const scratch = sizes_allocator.allocate(scratch_size);
    std::span<byte_t const>* const fetched = spans_allocator.allocate(slots);
    if (!scratch || !fetched) return status_t {status_code_t::out_of_memory_k, "No room to rebalance edge blocks"};
    for (std::size_t slot = 0; slot != slots; ++slot) new (fetched + slot) std::span<byte_t const> {};
    std::size_t *const delta_first = scratch, *const delta_last = scratch + slots, *const sizes = scratch + slots * 2,
                       *const touched = scratch + slots * 3;
    auto release_scratch = [&]() noexcept {
        spans_allocator.deallocate(fetched, slots);
        sizes_allocator.deallocate(scratch, scratch_size);
    };
    using blocks_t = memoized_blocks<registry_blocks<fetch_type_>>;
    blocks_t const blocks(listing, fetched);

    for (std::size_t slot = 0; slot != slots; ++slot) {
        delta_first[slot] = slot == 0 ? std::size_t(closed)
                                      : chain.lower_bound(closed, chain.size(), blocks.first_neighbor(slot));
        delta_last[slot] = slot + 1 < listed ? chain.lower_bound(closed, chain.size(), blocks.first_neighbor(slot + 1))
                                             : chain.size();
        touched[slot] = delta_first[slot] != delta_last[slot] || (closed && slot < listed);
        sizes[slot] = unknown_k;
    }
    auto size_of = [&](std::size_t slot) noexcept -> expected<std::size_t> {
        if (sizes[slot] != unknown_k) return sizes[slot];
        std::size_t const block_count = slot < listed && !closed ? 1 : 0;
        half_edge_cursor<blocks_t> cursor(blocks, slot, slot + block_count, chain, delta_first[slot], delta_last[slot]);
        auto counted = count_half_edges(cursor);
        if (counted) sizes[slot] = *counted;
        return counted;
    };
    auto dirty = [&](std::size_t slot) noexcept {
        return touched[slot] || (slot < listed && registry.sizes[slot] != edge_block_size_t::normal_k);
    };

    // Groups of consecutive slots that become one run of blocks, reusing the delta bounds as their storage
    struct group_t {
        std::size_t first = 0, last = 0, total = 0;
        bool rewrite = false;
    };
    using groups_allocator_t = typename traits_t::template rebind_alloc<group_t>;
    groups_allocator_t groups_allocator(allocator);
    group_t* const groups = groups_allocator.allocate(slots);
    if (!groups) return release_scratch(), status_t {status_code_t::out_of_memory_k, "No room to rebalance"};
    std::size_t groups_count = 0;
    auto fail = [&](status_t status) noexcept {
        groups_allocator.deallocate(groups, slots);
        release_scratch();
        return status;
    };
    auto group_total = [&](group_t const& group) noexcept -> expected<std::size_t> {
        if (group.total != unknown_k) return group.total;
        std::size_t total = 0;
        for (std::size_t slot = group.first; slot != group.last; ++slot) {
            auto size = size_of(slot);
            if (!size) return size.status();
            total += *size;
        }
        return total;
    };

    for (std::size_t slot = 0; slot != slots;) {
        if (!dirty(slot)) {
            groups[groups_count++] = {slot, slot + 1, unknown_k, false};
            ++slot;
            continue;
        }
        group_t group {slot, slot + 1, 0, touched[slot] != 0};
        auto size = size_of(slot);
        if (!size) return fail(size.status());
        group.total = *size, ++slot;
        if (group.total) {
            for (; group.total < min_count && slot != slots; ++slot, group.last = slot) {
                auto next = size_of(slot);
                if (!next) return fail(next.status());
                group.total += *next, group.rewrite = true;
            }
            while (group.total < min_count && groups_count) {
                group_t const previous = groups[--groups_count];
                auto previous_total = group_total(previous);
                if (!previous_total) return fail(previous_total.status());
                group.first = previous.first, group.total += *previous_total, group.rewrite = true;
            }
        }
        else group.rewrite = true;
        if (group.total > max_count) group.rewrite = true;
        groups[groups_count++] = group;
    }

    // Every rewritten group splits into chunks of about the optimal size once it outgrows a block
    auto chunks_of = [&](std::size_t total) noexcept -> std::size_t {
        if (!total) return 0;
        if (total <= max_count) return 1;
        return utilities::divide_round_up(total, edge_block_t::optimal_count_k);
    };
    std::size_t written_capacity = 0, removed_capacity = 0, largest = 0;
    for (std::size_t index = 0; index != groups_count; ++index) {
        if (!groups[index].rewrite) continue;
        written_capacity += chunks_of(groups[index].total);
        removed_capacity += std::min(groups[index].last, listed) - std::min(groups[index].first, listed);
        largest = std::max(largest, groups[index].total);
    }

    written_allocator_t written_allocator(allocator);
    keys_allocator_t keys_allocator(allocator);
    half_edges_allocator_t half_edges_allocator(allocator);
    written_block_t* const written = written_capacity ? written_allocator.allocate(written_capacity) : nullptr;
    id_chunk_t* const removed = removed_capacity ? keys_allocator.allocate(removed_capacity) : nullptr;
    labeled_half_edge_t* const merged = largest ? half_edges_allocator.allocate(largest) : nullptr;
    if ((written_capacity && !written) || (removed_capacity && !removed) || (largest && !merged))
        return fail(status_t {status_code_t::out_of_memory_k, "No room for rebalanced edge blocks"});
    std::size_t written_count = 0, removed_count = 0;
    auto fail_merged = [&](status_t status) noexcept {
        if (merged) half_edges_allocator.deallocate(merged, largest);
        return fail(status);
    };

    for (std::size_t index = 0; index != groups_count; ++index) {
        group_t const& group = groups[index];
        if (!group.rewrite) continue;
        for (std::size_t slot = group.first; slot != std::min(group.last, listed); ++slot)
            removed[removed_count++] = listing.key(slot);
        if (!group.total) continue;

        std::size_t const block_first = closed ? 0 : std::min(group.first, listed);
        std::size_t const block_last = closed ? 0 : std::min(group.last, listed);
        half_edge_cursor<blocks_t> cursor(blocks, block_first, block_last, chain, delta_first[group.first],
                                          delta_last[group.last - 1]);
        std::size_t count = 0;
        if (status_t const status = cursor.start(); status.failed()) return fail_merged(status);
        for (; cursor.valid() && count != group.total; ++count) {
            merged[count] = cursor.current();
            if (status_t const status = cursor.next(); status.failed()) return fail_merged(status);
        }
        if (count != group.total || cursor.valid())
            return fail_merged(status_t {status_code_t::internal_k, "A rebalance miscounted its half-edges"});

        std::size_t const chunks = chunks_of(count);
        for (std::size_t chunk = 0, first = 0; chunk != chunks && first != count; ++chunk) {
            std::size_t last = chunk + 1 == chunks ? count : std::max(first + 1, (chunk + 1) * count / chunks);
            // Every half-edge to one neighbor belongs to one block, so a cut slides past the neighbor it lands on
            while (last != count && merged[last].neighbor_id == merged[last - 1].neighbor_id) ++last;
            auto bytes = encode_edge_block(merged + first, last - first, allocator);
            if (!bytes) return fail_merged(bytes.status());
            written[written_count++] = {id_chunk_t {vertex, merged[first].neighbor_id}, *bytes};
            first = last;
        }
    }

    // Keys rewritten in place are written, not dropped, and the registry forgets every old key before listing anew
    for (std::size_t index = 0; index != removed_count; ++index) [[maybe_unused]]
        position_t const forgotten = registry.erase(removed[index]);
    std::size_t dropped = 0;
    for (std::size_t index = 0; index != removed_count; ++index) {
        bool rewritten = false;
        for (std::size_t chunk = 0; chunk != written_count && !rewritten; ++chunk)
            rewritten = written[chunk].key == removed[index];
        if (!rewritten) removed[dropped++] = removed[index];
    }
    for (std::size_t chunk = 0; chunk != written_count; ++chunk) {
        auto view = *edge_block_view::make(written[chunk].bytes);
        edge_block_specs_t specs;
        specs.first_neighbor = view.neighbor(0);
        specs.last_neighbor = view.neighbor(view.size() - 1);
        specs.size = view.size() < min_count   ? edge_block_size_t::too_small_k
                     : view.size() > max_count ? edge_block_size_t::too_big_k
                                               : edge_block_size_t::normal_k;
        if (status_t const status = registry.upsert(specs); status.failed()) return fail_merged(status);
    }

    if (merged) half_edges_allocator.deallocate(merged, largest);
    groups_allocator.deallocate(groups, slots);
    release_scratch();
    rebalanced_t result;
    result.written = {written, written_count};
    result.removed = {removed, dropped};
    result.closed = closed;
    return result;
}

#pragma endregion Rebalance

} // namespace unum::ustore::graph
