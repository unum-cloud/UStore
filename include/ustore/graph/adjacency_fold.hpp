/**
 *  @file include/ustore/graph/adjacency_fold.hpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief The graph adjacency fold: one value per vertex holding its sorted half-edges as the base, half-edge inserts
 *      and removals as change deltas, and reads that merge both in one pass.
 *
 *  A graph is one collection keyed by vertex, and the value of a vertex is its half-edges, ordered by neighbor, edge
 *  and role, which is also what names one, so an insert of a half-edge already held overwrites its weight and label.
 *  A write appends change deltas instead of rewriting the value: an insert or a removal of one half-edge, or a
 *  removal of every half-edge of the vertex, which closes every older delta.
 *
 *  A change delta and a half-edge of the base share one fixed-width layout, a base being the inserts of its
 *  half-edges back to back. Folding is then a merge of two sorted arrays of one record type, which a GPU runs as well
 *  as a CPU, and a read seeks either array by binary search without decoding it. The fields take 42 bytes, padded to
 *  48, the next multiple of 16, so both identifiers of every record in a 16-byte aligned value stay aligned, at three
 *  quarters of what a 64-byte power of two would take.
 *
 *  @code{.c}
 *  struct half_edge_delta {      // 48 bytes, native byte order
 *      uint8_t neighbor[16];     // offset 0, nil for a vertex removal
 *      uint8_t edge[16];         // offset 16, nil for a vertex removal
 *      float weight;             // offset 32
 *      int32_t label;            // offset 36
 *      uint8_t role;             // offset 40
 *      uint8_t operation;        // offset 41: 0 insert, 1 remove, 2 remove the vertex, always 0 in a base
 *      uint8_t reserved[6];      // offset 42, zero
 *  };
 *  struct vertex_value {         // 48 bytes per half-edge, empty for a vertex without any
 *      struct half_edge_delta half_edges[]; // strictly ascending by neighbor, edge and role
 *  };
 *  @endcode
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::int32_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <algorithm> // `std::sort`, `std::min`, `std::max`
#include <compare>   // `std::strong_ordering`
#include <limits>    // `std::numeric_limits`
#include <memory>    // `std::allocator_traits`
#include <new>       // placement `new`
#include <span>      // `std::span`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <smashtable/shared.hpp>       // `st::status_t`

#include "ustore/utilities/algorithm.hpp" // `compare_128`, `intersect_sorted_128`, `sorted_128_cursor_concept`
#include "ustore/utilities/folds.hpp"     // `delta_kind_t`, `delta_view_t`, `fold_concept`
#include "ustore/utilities/types.hpp"     // `byte_t`, `expected`, `round_up_to_multiple`, `smallest_key_k`

namespace unum::ustore::graph {

using utilities::byte_t;
using utilities::compare_128;
using utilities::delta_kind_t;
using utilities::delta_view_t;
using utilities::expected;
using utilities::intersect_sorted_128;
using utilities::smallest_key_k;
using utilities::status_code_t;
using utilities::status_t;

#pragma region Half Edges

/** The identifier of a vertex or an edge: the 16-byte key of the C ABI, where the nil key names nothing. */
using id_entity_t = ustore_key_t;

/** The label of a vertex or an edge, where a negative one is unknown. */
struct label_t {
    std::int32_t raw = -1;

    friend constexpr auto operator<=>(label_t const&, label_t const&) noexcept = default;
    constexpr bool is_unknown() const noexcept { return raw < 0; }
};

/** Bitmask of the parts a vertex plays in an edge. */
struct role_t {
    std::uint8_t raw = 0;

    friend constexpr auto operator<=>(role_t, role_t) noexcept = default;
    friend constexpr bool operator&(role_t first, role_t second) noexcept { return first.raw & second.raw; }
    friend constexpr role_t operator|(role_t first, role_t second) noexcept {
        return role_t {static_cast<std::uint8_t>(first.raw | second.raw)};
    }

    constexpr bool is_unknown() const noexcept { return !raw; }
    constexpr role_t allow(role_t other) const noexcept { return role_t {static_cast<std::uint8_t>(raw | other.raw)}; }
    constexpr role_t disallow(role_t other) const noexcept {
        return role_t {static_cast<std::uint8_t>(raw & ~other.raw)};
    }
};

inline constexpr role_t edge_member_k {1 << 0};
inline constexpr role_t dedge_source_k {1 << 1};
inline constexpr role_t dedge_target_k {1 << 2};
inline constexpr role_t hedge_leaf_k {1 << 3};
inline constexpr role_t hedge_parent_k {1 << 4};
inline constexpr role_t any_role_k {std::numeric_limits<std::uint8_t>::max()};

/** Whether an edge leads from its first vertex to its second one, or joins them both ways. */
enum class direction_t : bool { undirected_k, directed_k };

/** Weighted, labeled edge whose @c first and @c second endpoints follow the Boost Graph Library edge lists. */
struct labeled_edge_t {
    id_entity_t id {};
    id_entity_t first {};
    id_entity_t second {};
    float weight = 1;
    label_t label;
    direction_t direction = direction_t::directed_k;

    /** Compares everything but the direction. */
    static constexpr bool equals(labeled_edge_t const& lhs, labeled_edge_t const& rhs) noexcept {
        return lhs.id == rhs.id && lhs.first == rhs.first && lhs.second == rhs.second && lhs.label == rhs.label &&
               lhs.weight == rhs.weight;
    }

    friend constexpr bool operator==(labeled_edge_t const& lhs, labeled_edge_t const& rhs) noexcept {
        return equals(lhs, rhs);
    }
    friend constexpr auto operator<=>(labeled_edge_t const& lhs, labeled_edge_t const& rhs) noexcept {
        return lhs.id <=> rhs.id;
    }

    constexpr explicit operator bool() const noexcept {
        return id != smallest_key_k && first != smallest_key_k && second != smallest_key_k;
    }
    constexpr bool is_loop() const noexcept { return first == second; }
    constexpr bool contains(id_entity_t vertex_id) const noexcept { return first == vertex_id || second == vertex_id; }
    constexpr bool is_between(id_entity_t first_vertex_id, id_entity_t second_vertex_id) const noexcept {
        return (first == first_vertex_id && second == second_vertex_id) ||
               (second == first_vertex_id && first == second_vertex_id);
    }
    constexpr id_entity_t neighbor_of(id_entity_t known_member) const noexcept {
        return first == known_member ? second : first;
    }

    constexpr void clear() noexcept { *this = labeled_edge_t {}; }

    constexpr role_t role_of(id_entity_t known_member) const noexcept {
        constexpr role_t mapping[2][2] = {
            {edge_member_k, edge_member_k},
            {dedge_target_k, dedge_source_k},
        };
        return mapping[static_cast<std::size_t>(direction)][first == known_member];
    }
};

/** One end of a labeled edge, as the vertex at that end stores it: the edge, the other vertex and the role. */
struct labeled_half_edge_t {
    id_entity_t edge_id {};
    id_entity_t neighbor_id {};
    float edge_weight = 0;
    label_t edge_label;
    role_t role;

    constexpr labeled_half_edge_t() noexcept = default;

    constexpr explicit labeled_half_edge_t(labeled_edge_t const& edge, id_entity_t member) noexcept
        : edge_id(edge.id), edge_weight(edge.weight), edge_label(edge.label) {
        bool const is_directed = edge.direction == direction_t::directed_k;
        if (edge.first == member) {
            neighbor_id = edge.second;
            role = is_directed ? dedge_source_k : edge_member_k;
        }
        else {
            neighbor_id = edge.first;
            role = is_directed ? dedge_target_k : edge_member_k;
        }
    }

    constexpr labeled_edge_t to_edge(id_entity_t vertex) const noexcept {
        labeled_edge_t edge;
        edge.id = edge_id;
        edge.label = edge_label;
        edge.weight = edge_weight;

        switch (role.raw) {
        case dedge_source_k.raw:
            edge.direction = direction_t::directed_k;
            edge.first = vertex;
            edge.second = neighbor_id;
            break;

        case dedge_target_k.raw:
            edge.direction = direction_t::directed_k;
            edge.first = neighbor_id;
            edge.second = vertex;
            break;

        default:
            edge.direction = direction_t::undirected_k;
            edge.first = std::min(vertex, neighbor_id);
            edge.second = std::max(vertex, neighbor_id);
            break;
        }
        return edge;
    }
};

#pragma endregion Half Edges

#pragma region Delta Records

/** What one change delta does to the adjacency of its vertex. */
enum class half_edge_operation_t : std::uint8_t {

    /** Adds a half-edge, or overwrites the weight and label of the one with its neighbor, edge and role. */
    insert_k = 0,

    /** Removes the half-edge with its neighbor, edge and role, if there is one. */
    remove_k = 1,

    /** Removes every half-edge of the vertex, closing every older delta of it. */
    remove_vertex_k = 2,
};

/** One write to the adjacency of a vertex, which the collection key names: a half-edge insert or
 *  removal, or a removal of every half-edge. */
struct half_edge_delta_t {
    id_entity_t neighbor {};
    id_entity_t edge {};
    float weight = 1;
    label_t label;
    role_t role;
    half_edge_operation_t operation = half_edge_operation_t::insert_k;

    /** The record adding @p half_edge. */
    static constexpr half_edge_delta_t insert(labeled_half_edge_t const& half_edge) noexcept {
        return {half_edge.neighbor_id, half_edge.edge_id, half_edge.edge_weight,
                half_edge.edge_label,  half_edge.role,    half_edge_operation_t::insert_k};
    }

    /** The record removing the half-edge to @p neighbor through @p edge in @p role. */
    static constexpr half_edge_delta_t remove(id_entity_t neighbor, id_entity_t edge, role_t role) noexcept {
        return {neighbor, edge, 0, label_t {}, role, half_edge_operation_t::remove_k};
    }

    /** The record removing every half-edge. */
    static constexpr half_edge_delta_t remove_vertex() noexcept {
        return {{}, {}, 0, label_t {}, role_t {}, half_edge_operation_t::remove_vertex_k};
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
};

/** Where each field of a serialized record starts. */
inline constexpr std::size_t delta_neighbor_offset_k = 0, delta_edge_offset_k = 16, delta_weight_offset_k = 32,
                             delta_label_offset_k = 36, delta_role_offset_k = 40, delta_operation_offset_k = 41,
                             delta_reserved_offset_k = 42;

/** Bytes one serialized record takes: its fields padded to a multiple of an identifier's 16 bytes. */
inline constexpr std::size_t half_edge_delta_bytes_k = utilities::round_up_to_multiple(delta_reserved_offset_k,
                                                                                       sizeof(id_entity_t));

static_assert(half_edge_delta_bytes_k == 48, "the layout in the file header");

/** Writes @p delta into the record at @p destination. */
inline void save_delta(half_edge_delta_t const& delta, byte_t* destination) noexcept {
    std::memset(destination, 0, half_edge_delta_bytes_k);
    std::memcpy(destination + delta_neighbor_offset_k, &delta.neighbor, 16);
    std::memcpy(destination + delta_edge_offset_k, &delta.edge, 16);
    std::memcpy(destination + delta_weight_offset_k, &delta.weight, 4);
    std::memcpy(destination + delta_label_offset_k, &delta.label.raw, 4);
    std::memcpy(destination + delta_role_offset_k, &delta.role.raw, 1);
    std::memcpy(destination + delta_operation_offset_k, &delta.operation, 1);
}

/** Reads the record at @p source, which @ref check_delta accepted. */
inline half_edge_delta_t load_delta(byte_t const* source) noexcept {
    half_edge_delta_t delta;
    std::memcpy(&delta.neighbor, source + delta_neighbor_offset_k, 16);
    std::memcpy(&delta.edge, source + delta_edge_offset_k, 16);
    std::memcpy(&delta.weight, source + delta_weight_offset_k, 4);
    std::memcpy(&delta.label.raw, source + delta_label_offset_k, 4);
    std::memcpy(&delta.role.raw, source + delta_role_offset_k, 1);
    std::memcpy(&delta.operation, source + delta_operation_offset_k, 1);
    return delta;
}

/** The operation of the record at @p source. */
inline half_edge_operation_t operation_of(byte_t const* source) noexcept {
    return static_cast<half_edge_operation_t>(source[delta_operation_offset_k]);
}

/** Refuses a record with an unknown operation or a set reserved byte; the operation, not the
 *  neighbor, tells a removal of the vertex, so the nil key is a vertex like any other, as the C
 *  ABI's keys are. */
inline status_t check_delta(byte_t const* source) noexcept {
    auto const operation = static_cast<std::uint8_t>(source[delta_operation_offset_k]);
    if (operation > static_cast<std::uint8_t>(half_edge_operation_t::remove_vertex_k))
        return {status_code_t::malformed_k, "A half-edge delta has an unknown operation"};
    for (std::size_t offset = delta_reserved_offset_k; offset != half_edge_delta_bytes_k; ++offset)
        if (source[offset] != byte_t {0}) return {status_code_t::malformed_k, "A half-edge delta sets reserved bytes"};
    return {};
}

/** Orders the half-edges two records name by neighbor, edge and role, which is also what names one. */
inline int compare_half_edges(byte_t const* record, byte_t const* other) noexcept {
    if (int const order = compare_128(record + delta_neighbor_offset_k, other + delta_neighbor_offset_k); order)
        return order;
    if (int const order = compare_128(record + delta_edge_offset_k, other + delta_edge_offset_k); order) return order;
    return int(static_cast<std::uint8_t>(record[delta_role_offset_k])) -
           int(static_cast<std::uint8_t>(other[delta_role_offset_k]));
}

/** The records an edge writes and the vertices they go to, one per endpoint, or one for an
 *  undirected loop whose ends are one half-edge. */
inline std::size_t deltas_of_edge(labeled_edge_t const& edge, half_edge_operation_t operation,
                                  id_entity_t (&vertices)[2], half_edge_delta_t (&deltas)[2]) noexcept {
    std::size_t count = 0;
    for (id_entity_t const member : {edge.first, edge.second}) {
        labeled_half_edge_t const half_edge(edge, member);
        bool const is_second_of_loop = count == 1 && edge.is_loop();
        // A directed loop is filed once as its source and once as its target
        role_t const role = is_second_of_loop && edge.direction == direction_t::directed_k ? dedge_target_k
                                                                                           : half_edge.role;
        if (is_second_of_loop && role == deltas[0].role) break;
        half_edge_delta_t delta = half_edge_delta_t::insert(half_edge);
        delta.role = role;
        if (operation == half_edge_operation_t::remove_k)
            delta = half_edge_delta_t::remove(half_edge.neighbor_id, half_edge.edge_id, role);
        vertices[count] = member;
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

/** Records back to back, read in place: a chain of changes, or a vertex value. */
class half_edge_deltas_view_t {
  public:
    half_edge_deltas_view_t() noexcept = default;

    /** The records @p bytes holds, refusing a ragged length; @ref check_composed and @ref check_base judge them. */
    static expected<half_edge_deltas_view_t> make(std::span<byte_t const> bytes) noexcept {
        if (bytes.size() % half_edge_delta_bytes_k)
            return status_t {status_code_t::malformed_k, "A chain of half-edge deltas has a ragged length"};
        half_edge_deltas_view_t view;
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
    half_edge_operation_t operation(std::size_t index) const noexcept { return operation_of(record(index)); }

    /** The first index in <tt>[first, size)</tt> whose neighbor does not order before @p neighbor,
     *  in a sorted chain. / */
    std::size_t lower_bound(std::size_t first, id_entity_t const& neighbor) const noexcept {
        std::size_t last = size();
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

/** Refuses @p chain unless it is as @c adjacency_fold_t::compose writes one: well-formed records
 *  naming strictly ascending half-edges, after at most one removal of the vertex at its head. */
inline status_t check_composed(half_edge_deltas_view_t chain) noexcept {
    for (std::size_t index = 0; index != chain.size(); ++index) {
        if (status_t const status = check_delta(chain.record(index)); status.failed()) return status;
        if (chain.operation(index) == half_edge_operation_t::remove_vertex_k) {
            if (index) return {status_code_t::malformed_k, "A removal of the vertex follows other half-edge deltas"};
            continue;
        }
        if (index && chain.operation(index - 1) != half_edge_operation_t::remove_vertex_k &&
            compare_half_edges(chain.record(index - 1), chain.record(index)) >= 0)
            return {status_code_t::malformed_k, "Half-edge deltas are out of order"};
    }
    return {};
}

/** Refuses @p base unless it is as @c adjacency_fold_t::apply writes a vertex value: a composed chain of inserts. */
inline status_t check_base(half_edge_deltas_view_t base) noexcept {
    for (std::size_t index = 0; index != base.size(); ++index)
        if (base.operation(index) != half_edge_operation_t::insert_k)
            return {status_code_t::malformed_k, "A vertex value holds a record other than an insert"};
    return check_composed(base);
}

#pragma endregion Delta Records

#pragma region Merging Cursor

/**
 *  @brief Walks the live half-edges of a vertex value merged with a composed chain of changes, in one pass.
 *
 *  An insert in the chain adds or overwrites the half-edge it names, a removal hides the one it names, and a removal
 *  of the vertex at the head of the chain hides the whole value, so the walk yields exactly the records
 *  @c adjacency_fold_t::apply writes, without writing them. A seek leaps over both arrays by binary search.
 */
class half_edge_cursor_t {
  public:
    half_edge_cursor_t() noexcept = default;

    /** A walk over @p base, a vertex value, and @p chain, a chain @c adjacency_fold_t::compose
     *  wrote, both of which must outlive it, standing on the first live half-edge. */
    half_edge_cursor_t(half_edge_deltas_view_t base, half_edge_deltas_view_t chain) noexcept
        : base_(base), chain_(chain) {
        if (chain_.size() && chain_.operation(0) == half_edge_operation_t::remove_vertex_k)
            base_ = {}, chain_offset_ = 1;
        settle_();
    }

    /** Whether the walk stands on a half-edge. */
    bool valid() const noexcept { return record_ != nullptr; }

    /** The record of the half-edge the walk stands on, an insert in the layout of a vertex value. */
    byte_t const* record() const noexcept { return record_; }

    /** The half-edge the walk stands on. */
    labeled_half_edge_t current() const noexcept { return load_delta(record_).half_edge(); }

    /** The neighbor of the half-edge the walk stands on. */
    id_entity_t neighbor() const noexcept {
        id_entity_t key;
        std::memcpy(&key, record_ + delta_neighbor_offset_k, 16);
        return key;
    }

    /** Steps to the next live half-edge. */
    void next() noexcept {
        if (base_offset_ < base_.size() && compare_half_edges(base_.record(base_offset_), record_) == 0) ++base_offset_;
        if (chain_offset_ < chain_.size() && compare_half_edges(chain_.record(chain_offset_), record_) == 0)
            ++chain_offset_;
        settle_();
    }

    /** Leaps to the first live half-edge whose neighbor does not order before @p neighbor, never backwards. */
    void seek_neighbor(id_entity_t const& neighbor) noexcept {
        if (valid() && compare_128(record_ + delta_neighbor_offset_k, &neighbor) >= 0) return;
        base_offset_ = base_.lower_bound(base_offset_, neighbor);
        chain_offset_ = chain_.lower_bound(chain_offset_, neighbor);
        settle_();
    }

  private:

    /** Moves onto the first live half-edge at or after the current offsets. */
    void settle_() noexcept {
        for (;;) {
            bool const has_base = base_offset_ < base_.size(), has_chain = chain_offset_ < chain_.size();
            if (!has_base && !has_chain) {
                record_ = nullptr;
                return;
            }
            int order = has_base ? -1 : 1;
            if (has_base && has_chain)
                order = compare_half_edges(base_.record(base_offset_), chain_.record(chain_offset_));
            if (order < 0) {
                record_ = base_.record(base_offset_);
                return;
            }
            if (chain_.operation(chain_offset_) == half_edge_operation_t::insert_k) {
                record_ = chain_.record(chain_offset_);
                return;
            }
            ++chain_offset_;
            if (order == 0) ++base_offset_;
        }
    }

    half_edge_deltas_view_t base_, chain_;
    std::size_t base_offset_ = 0, chain_offset_ = 0;
    byte_t const* record_ = nullptr;
};

/** Walks the distinct neighbors of a half-edge walk, fitting @c sorted_128_cursor_concept for 128-bit intersections. */
class neighbor_cursor_t {
  public:
    neighbor_cursor_t() noexcept = default;

    /** The neighbors of @p half_edges. */
    explicit neighbor_cursor_t(half_edge_cursor_t half_edges) noexcept : half_edges_(half_edges) {}

    /** Whether the walk still stands on a neighbor. */
    bool valid() const noexcept { return half_edges_.valid(); }

    /** The neighbor the walk stands on, while it is valid. */
    ustore_key_t key() const noexcept { return half_edges_.neighbor(); }

    /** Steps past every half-edge to the current neighbor. */
    status_t next() noexcept {
        id_entity_t const neighbor = key();
        do half_edges_.next();
        while (half_edges_.valid() && half_edges_.neighbor() == neighbor);
        return {};
    }

    /** Leaps to the first neighbor not ordering before @p neighbor, never backwards. */
    status_t seek(ustore_key_t const& neighbor) noexcept {
        half_edges_.seek_neighbor(neighbor);
        return {};
    }

  private:
    half_edge_cursor_t half_edges_;
};

static_assert(utilities::sorted_128_cursor_concept<neighbor_cursor_t>);

#pragma endregion Merging Cursor

#pragma region Fold

/** A record of a chain being composed, with its place in the chain, which breaks ties between
 *  records of one half-edge. */
struct sequenced_record_t {
    byte_t const* record = nullptr;
    std::uint32_t sequence = 0;
};

/** Orders records with the removals of the vertex first, then by neighbor, edge, role and age. */
inline bool sequenced_less(sequenced_record_t const& first, sequenced_record_t const& second) noexcept {
    bool const first_clears = operation_of(first.record) == half_edge_operation_t::remove_vertex_k;
    bool const second_clears = operation_of(second.record) == half_edge_operation_t::remove_vertex_k;
    if (first_clears != second_clears) return first_clears;
    if (!first_clears)
        if (int const order = compare_half_edges(first.record, second.record); order) return order < 0;
    return first.sequence < second.sequence;
}

/**
 *  @brief The fold of graph adjacency, fitting the fold concept of <tt>ustore/utilities/folds.hpp</tt>.
 *
 *  A base is a vertex value, a write replaces it and a removal drops the vertex, and a change carries a chain of
 *  half-edge records. Applying yields the value with every record folded in, sorted, at whatever size it grows to.
 *  Composing keeps the newest record of every half-edge and drops everything older than a removal of the vertex,
 *  after which a removal of one half-edge has nothing left to hide and goes too. Without such a removal an insert
 *  followed by a removal of one half-edge still becomes the removal, because an older version may hold the half-edge.
 */
struct adjacency_fold_t {

    /** Bytes of every change delta, one width for all, so a device merge can compose chains in place. */
    static constexpr std::size_t change_bytes_k = half_edge_delta_bytes_k;

    /**
     *  @brief The value @p base becomes once every delta of @p deltas applies, oldest first.
     *  @return The value in a block of @p allocator, @p base itself without changes, or no bytes when no half-edge is
     *      left, which is a vertex without any.
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
        auto base_view = half_edge_deltas_view_t::make(base);
        if (!base_view) return base_view.status();
        if (status_t const status = check_base(*base_view); status.failed()) return status;
        std::span<delta_view_t const> const changes = deltas.subspan(first_change);
        if (changes.empty()) return base;

        auto chain_bytes = compose(changes, allocator);
        if (!chain_bytes) return chain_bytes.status();
        // The value keeps at most every half-edge of the base and every record of the chain
        std::size_t const capacity = base.size() + chain_bytes->size();
        if (!capacity) return std::span<byte_t const> {};
        byte_t* const block = allocator.allocate(capacity);
        if (!block) return status_t {status_code_t::out_of_memory_k, "No room for a folded vertex value"};
        std::size_t written = 0;
        for (half_edge_cursor_t cursor(*base_view, *half_edge_deltas_view_t::make(*chain_bytes)); cursor.valid();
             cursor.next(), written += half_edge_delta_bytes_k)
            std::memcpy(block + written, cursor.record(), half_edge_delta_bytes_k);
        return std::span<byte_t const>(block, written);
    }

    /**
     *  @brief One chain equivalent to @p deltas on every base: at most one removal of the vertex, first, then the
     *      newest record of every half-edge newer than it, sorted by neighbor, edge and role.
     *
     *  A write or a removal of the vertex value is a base rather than a change, so the caller applies the chain to it
     *  instead, as @ref closes tells. Composing is associative, byte for byte, as a segmented reduction needs.
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
                return status_t {status_code_t::invalid_argument_k, "A value write or removal is a base to apply"};
            auto chain = half_edge_deltas_view_t::make(delta.bytes);
            if (!chain) return chain.status();
            for (std::size_t index = 0; index != chain->size(); ++index)
                if (status_t const status = check_delta(chain->record(index)); status.failed()) return status;
            total += chain->size();
        }
        if (!total) return std::span<byte_t const> {};
        // Every record carries its place in the chain as a 32-bit sequence
        if (total > std::numeric_limits<std::uint32_t>::max())
            return status_t {status_code_t::capacity_exhausted_k, "Too many half-edge deltas to compose"};

        using entries_allocator_t =
            typename std::allocator_traits<allocator_type_>::template rebind_alloc<sequenced_record_t>;
        entries_allocator_t entries_allocator(allocator);
        sequenced_record_t* const entries = entries_allocator.allocate(total);
        if (!entries) return status_t {status_code_t::out_of_memory_k, "No room to compose half-edge deltas"};
        std::size_t count = 0;
        for (delta_view_t const& delta : deltas) {
            half_edge_deltas_view_t const chain = *half_edge_deltas_view_t::make(delta.bytes);
            for (std::size_t index = 0; index != chain.size(); ++index, ++count)
                new (entries + count) sequenced_record_t {chain.record(index), static_cast<std::uint32_t>(count)};
        }
        std::sort(entries, entries + total, sequenced_less);

        // The newest removal of the vertex sorts last among the leading ones and cuts everything older
        std::size_t index = 0;
        std::uint32_t cut = 0;
        for (; index != total && operation_of(entries[index].record) == half_edge_operation_t::remove_vertex_k; ++index)
            cut = entries[index].sequence;
        std::size_t const removals = index ? 1 : 0;
        std::size_t kept = 0;
        while (index != total) {
            std::size_t newest = index;
            while (newest + 1 != total && compare_half_edges(entries[newest + 1].record, entries[index].record) == 0)
                ++newest;
            sequenced_record_t const winner = entries[newest];
            index = newest + 1;
            if (removals && (winner.sequence < cut || operation_of(winner.record) == half_edge_operation_t::remove_k))
                continue;
            entries[kept++] = winner;
        }

        std::size_t const bytes = (removals + kept) * half_edge_delta_bytes_k;
        byte_t* const block = allocator.allocate(bytes);
        if (!block) {
            entries_allocator.deallocate(entries, total);
            return status_t {status_code_t::out_of_memory_k, "No room for a composed chain"};
        }
        if (removals) save_delta(half_edge_delta_t::remove_vertex(), block);
        for (std::size_t entry = 0; entry != kept; ++entry)
            std::memcpy(block + (removals + entry) * half_edge_delta_bytes_k, entries[entry].record,
                        half_edge_delta_bytes_k);
        entries_allocator.deallocate(entries, total);
        return std::span<byte_t const>(block, bytes);
    }

    /** Whether @p delta makes every older delta of its key irrelevant: a value write or removal, or
     *  a chain led by a removal of the vertex, as every composed chain holding one is. */
    bool closes(delta_view_t const& delta) const noexcept {
        if (delta.kind != delta_kind_t::change_k) return true;
        return delta.bytes.size() >= half_edge_delta_bytes_k &&
               operation_of(delta.bytes.data()) == half_edge_operation_t::remove_vertex_k;
    }
};

static_assert(utilities::fold_concept<adjacency_fold_t>);

/**
 *  @brief The device fold of graph adjacency, composing on one NVIDIA device what @c adjacency_fold_t composes on the
 *      host, for a compaction's chains of half-edge changes all at once.
 *
 *  Host translation units name it, as the tree and the engine pick it for a graph's merges; its @c compose runs on
 *  NVIDIA devices and is defined in @c vram_nvidia_adjacency_fold.cuh, which only nvcc compiles.
 */
struct vram_nvidia_adjacency_fold_t {

    /**
     *  @brief Composes chain @c c, the changes from @p chain_starts[c] to @p chain_starts[c + 1] oldest first, into the
     *      bytes of @p composed from @p composed_offsets[c] to @p composed_offsets[c + 1], on the device of
     *      @p executor.
     *  @tparam executor_type_ The executor whose stream the device work queues on, @c utilities::cuda_executor_t.
     *  @retval invalid_argument_k A delta is no change, or carries a ragged or malformed record.
     *  @retval out_of_memory_heap_k A buffer could not grow.
     */
    template <typename executor_type_>
    st::status_t compose(executor_type_& executor, std::span<delta_view_t const> changes,
                         std::span<std::uint64_t const> chain_starts, st::basic_vector<byte_t>& composed,
                         st::basic_vector<std::uint64_t>& composed_offsets) const noexcept;
};

#pragma endregion Fold

#pragma region Reads

/**
 *  @brief The adjacency of one vertex as a read sees it: its value merged with the composed chain of its pending
 *      changes, walked in place.
 *
 *  The value is checked for its length alone, trusted to be as @c adjacency_fold_t::apply wrote it, so a seek stays
 *  logarithmic in the half-edges, while the pending chain is checked whole. Without pending changes a degree is the
 *  length of the value.
 */
class vertex_adjacency_t {
  public:

    /** The adjacency of @p vertex, whose value is @p base, with @p pending, a chain
     *  @c adjacency_fold_t::compose wrote, folded over it, both of which must outlive every
     *  read. */
    static expected<vertex_adjacency_t> make(id_entity_t const& vertex, std::span<byte_t const> base,
                                             std::span<byte_t const> pending) noexcept {
        auto base_view = half_edge_deltas_view_t::make(base);
        if (!base_view) return base_view.status();
        auto chain = half_edge_deltas_view_t::make(pending);
        if (!chain) return chain.status();
        if (status_t const status = check_composed(*chain); status.failed()) return status;
        return vertex_adjacency_t(vertex, *base_view, *chain);
    }

    /** A walk over every live half-edge. */
    half_edge_cursor_t half_edges() const noexcept { return half_edge_cursor_t(base_, pending_); }

    /** A walk over the distinct neighbors, for @c intersect_sorted_128. */
    neighbor_cursor_t neighbors() const noexcept { return neighbor_cursor_t(half_edges()); }

    /** Calls @p callback with every live half-edge in order, stopping at the first failure it answers. */
    template <typename callback_type_>
    status_t for_each(callback_type_&& callback) const noexcept {
        for (half_edge_cursor_t cursor = half_edges(); cursor.valid(); cursor.next())
            if (status_t const status = callback(cursor.current()); status.failed()) return status;
        return {};
    }

    /** Half-edges playing any of @p roles, every one of them by default. */
    std::size_t degree(role_t roles = any_role_k) const noexcept {
        if (!pending_.size() && roles == any_role_k) return base_.size();
        std::size_t count = 0;
        for (half_edge_cursor_t cursor = half_edges(); cursor.valid(); cursor.next())
            count += (cursor.current().role & roles) ? 1 : 0;
        return count;
    }

    /** Calls @p callback with every live half-edge to @p neighbor, stopping at the first failure it answers. */
    template <typename callback_type_>
    status_t edges_to(id_entity_t const& neighbor, callback_type_&& callback) const noexcept {
        half_edge_cursor_t cursor = half_edges();
        cursor.seek_neighbor(neighbor);
        for (; cursor.valid() && cursor.neighbor() == neighbor; cursor.next())
            if (status_t const status = callback(cursor.current()); status.failed()) return status;
        return {};
    }

    /** The vertex whose adjacency this is. */
    id_entity_t vertex() const noexcept { return vertex_; }

  private:
    vertex_adjacency_t(id_entity_t const& vertex, half_edge_deltas_view_t base,
                       half_edge_deltas_view_t pending) noexcept
        : vertex_(vertex), base_(base), pending_(pending) {}

    id_entity_t vertex_ {};
    half_edge_deltas_view_t base_, pending_;
};

/** Calls @p callback with every edge between @p adjacency's vertex and @p neighbor, as @p adjacency stores them. */
template <typename callback_type_>
status_t edges_between(vertex_adjacency_t const& adjacency, id_entity_t const& neighbor,
                       callback_type_&& callback) noexcept {
    id_entity_t const vertex = adjacency.vertex();
    return adjacency.edges_to(
        neighbor, [&](labeled_half_edge_t const& half_edge) noexcept { return callback(half_edge.to_edge(vertex)); });
}

/** Calls @p callback with every distinct neighbor two vertices share, in order, through a 128-bit
 *  sorted intersection that leaps over the half-edges neither needs. */
template <typename callback_type_>
status_t common_neighbors(vertex_adjacency_t const& first, vertex_adjacency_t const& second,
                          callback_type_&& callback) noexcept {
    neighbor_cursor_t first_neighbors = first.neighbors(), second_neighbors = second.neighbors();
    return intersect_sorted_128(first_neighbors, second_neighbors, callback);
}

#pragma endregion Reads

} // namespace unum::ustore::graph
