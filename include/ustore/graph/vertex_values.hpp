/**
 *  @file include/ustore/graph/vertex_values.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief The dense codec of a graph that keeps every vertex as one value of its degrees and sorted neighborships,
 *      over any engine's reads and writes.
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

#pragma once

#include <cstddef> // `std::ptrdiff_t`, `std::size_t`
#include <cstring> // `std::memcpy`

#include <algorithm>  // `std::ranges::binary_search`, `std::ranges::equal_range`, `std::ranges::lower_bound`
#include <compare>    // `std::strong_ordering`
#include <functional> // `std::ranges::less`
#include <iterator>   // `std::random_access_iterator_tag`
#include <optional>   // `std::optional`

#include "ustore/utilities/algorithm.hpp" // `trivial_insert`, `trivial_erase`
#include "ustore/utilities/types.hpp"     // `collection_key_t`, `neighborship_t`, `value_view_t`, `missing_length_k`

namespace unum::ustore::graph {

using utilities::collection_key_t;
using utilities::missing_length_k;
using utilities::neighborship_t;
using utilities::trivial_erase;
using utilities::trivial_insert;
using utilities::value_view_t;

/** Bytes of the header of a vertex value, its outgoing and its incoming degree. */

constexpr std::size_t bytes_in_degrees_header_k = 2 * sizeof(ustore_vertex_degree_t);

/** A vertex value being rewritten, together with how many neighborships the update changes. */
struct updated_entry_t : public collection_key_t {

    /** Bytes of the vertex value, degrees first. */
    ustore_byte_t* content = nullptr;

    /** Length of the value, or @c missing_length_k for a missing vertex. */
    ustore_length_t length = missing_length_k;

    /** Neighborships the update adds or removes. */
    ustore_vertex_degree_t degree_delta = 0;

    operator value_view_t() const noexcept { return {content, length}; }
};

/** Reads the degree at @p index of the header of @p value, which starts wherever its blob does. */
inline ustore_vertex_degree_t degree_of(ustore_byte_t const* value, std::size_t index) noexcept {
    ustore_vertex_degree_t degree;
    std::memcpy(&degree, value + index * sizeof(ustore_vertex_degree_t), sizeof(ustore_vertex_degree_t));
    return degree;
}

/** Writes @p degree at @p index of the header of @p value. */
inline void set_degree(ustore_byte_t* value, std::size_t index, ustore_vertex_degree_t degree) noexcept {
    std::memcpy(value + index * sizeof(ustore_vertex_degree_t), &degree, sizeof(ustore_vertex_degree_t));
}

/** Walks the neighborships of a value, loading each one through @c std::memcpy because the value carries no
 *  alignment of its own. */
class neighborship_iterator_t {

    ustore_byte_t const* bytes_ {nullptr};

  public:
    using value_type = neighborship_t;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::random_access_iterator_tag;
    using iterator_category = std::input_iterator_tag;

    neighborship_iterator_t() = default;
    explicit neighborship_iterator_t(ustore_byte_t const* bytes) noexcept : bytes_(bytes) {}

    /** The bytes the iterator points at. */
    ustore_byte_t const* bytes() const noexcept { return bytes_; }

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
    neighborships_view_t(ustore_byte_t const* bytes, std::size_t count) noexcept : begin_(bytes), count_(count) {}

    neighborship_iterator_t begin() const noexcept { return begin_; }
    neighborship_iterator_t end() const noexcept { return begin_ + static_cast<std::ptrdiff_t>(count_); }

    /** The first byte of the run, from which an offset inside the value is measured. */
    ustore_byte_t const* bytes() const noexcept { return begin_.bytes(); }

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }
};

/** The neighborships of a vertex value in @p role, empty for a missing vertex. */
inline neighborships_view_t neighbors(value_view_t value,
                                      ustore_vertex_role_t role = ustore_vertex_role_any_k) noexcept {
    if (value.size() < bytes_in_degrees_header_k) return {};

    auto const header = reinterpret_cast<ustore_byte_t const*>(value.begin());
    ustore_byte_t const* const first = header + bytes_in_degrees_header_k;
    std::size_t const outgoing = degree_of(header, 0);
    std::size_t const incoming = degree_of(header, 1);
    switch (role) {
    case ustore_vertex_role_source_k: return {first, outgoing};
    case ustore_vertex_role_target_k: return {first + outgoing * sizeof(neighborship_t), incoming};
    case ustore_vertex_role_any_k: return {first, outgoing + incoming};
    case ustore_vertex_role_unknown_k: return {};
    }
    return {};
}

/** Whether @p value is missing, a bare vertex, or degrees followed by exactly the neighborships they count. */
inline bool is_vertex_value(value_view_t value) noexcept {
    if (value.empty()) return true;
    if (value.size() < bytes_in_degrees_header_k) return false;

    auto const header = reinterpret_cast<ustore_byte_t const*>(value.begin());
    std::size_t const neighborships_count = std::size_t {degree_of(header, 0)} + degree_of(header, 1);
    return value.size() - bytes_in_degrees_header_k == neighborships_count * sizeof(neighborship_t);
}

/** Counts a neighborship towards the growth of @p entry unless the entry already holds it. */
inline void count_inserts_into_entry( //
    updated_entry_t& entry, ustore_vertex_role_t role, ustore_key_t neighbor_id, ustore_key_t edge_id) noexcept {

    neighborship_t const neighborship {neighbor_id, edge_id};
    if (entry.length > bytes_in_degrees_header_k && std::ranges::binary_search(neighbors(entry, role), neighborship))
        return;
    ++entry.degree_delta;
}

/** Inserts a neighborship into @p entry, whose buffer already has room for it, unless the entry holds it. */
inline void insert_into_entry( //
    updated_entry_t& entry, ustore_vertex_role_t role, ustore_key_t neighbor_id, ustore_key_t edge_id) noexcept {

    neighborship_t const neighborship {neighbor_id, edge_id};
    auto const inserted = reinterpret_cast<ustore_byte_t const*>(&neighborship);
    ustore_byte_t* const header = entry.content;
    ustore_byte_t* const neighborships = header + bytes_in_degrees_header_k;

    if (entry.length < bytes_in_degrees_header_k || entry.length == missing_length_k) {
        set_degree(header, role != ustore_vertex_role_target_k, 0);
        set_degree(header, role == ustore_vertex_role_target_k, 1);
        std::memcpy(neighborships, inserted, sizeof(neighborship_t));
        entry.length += bytes_in_degrees_header_k;
        entry.length += sizeof(neighborship_t);
        return;
    }

    neighborships_view_t const existing = neighbors(entry, role);
    auto const position = std::ranges::lower_bound(existing, neighborship);
    if (position != existing.end() && *position == neighborship) return;

    std::size_t const role_index = role == ustore_vertex_role_target_k;
    std::size_t const stored = std::size_t {degree_of(header, 0)} + degree_of(header, 1);
    auto const offset = static_cast<std::size_t>(existing.bytes() - neighborships +
                                                 (position - existing.begin()) * sizeof(neighborship_t));
    trivial_insert(neighborships, stored * sizeof(neighborship_t), offset, inserted, inserted + sizeof(neighborship_t));
    set_degree(header, role_index, degree_of(header, role_index) + 1);
    entry.length += sizeof(neighborship_t);
}

/** Erases from @p entry the neighborships with @p neighbor_id, or only the one with @p edge_id when it is given. */
inline void erase_from_entry( //
    updated_entry_t& entry, ustore_vertex_role_t role, ustore_key_t neighbor_id,
    std::optional<ustore_key_t> edge_id = {}) noexcept {

    if (entry.length < bytes_in_degrees_header_k || entry.length == missing_length_k) return;

    ustore_byte_t* const header = entry.content;
    ustore_byte_t* const neighborships = header + bytes_in_degrees_header_k;
    neighborships_view_t const existing = neighbors(entry, role);
    auto const matches = edge_id ? std::ranges::equal_range(existing, neighborship_t {neighbor_id, *edge_id})
                                 : std::ranges::equal_range(existing, neighbor_id, std::ranges::less {},
                                                            &neighborship_t::neighbor_id);
    if (matches.empty()) return;

    std::size_t const role_index = role == ustore_vertex_role_target_k;
    std::size_t const stored = std::size_t {degree_of(header, 0)} + degree_of(header, 1);
    auto const offset = static_cast<std::size_t>(existing.bytes() - neighborships +
                                                 (matches.begin() - existing.begin()) * sizeof(neighborship_t));
    std::size_t const count = matches.size();
    trivial_erase(neighborships, stored * sizeof(neighborship_t), offset, count * sizeof(neighborship_t));
    set_degree(header, role_index, degree_of(header, role_index) - static_cast<ustore_vertex_degree_t>(count));
    entry.degree_delta += static_cast<ustore_vertex_degree_t>(count);
    entry.length -= static_cast<ustore_length_t>(sizeof(neighborship_t) * count);
}

} // namespace unum::ustore::graph
