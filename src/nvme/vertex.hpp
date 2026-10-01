/**
 *  @file src/nvme/vertex.hpp
 *  @author Ash Vardanian
 *  @date December 1, 2022
 *  @brief Vertex, label and chunk identifier types of the persistent graph database.
 */

#pragma once
#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::int32_t`, `std::int64_t`

#include <compare> // `std::strong_ordering`

#include <fmt/format.h>

#include <smashtable/basic_flat_set.hpp> // `st::basic_flat_set`

#include "ustore/types.hpp" // the `st` alias

#include "nvme/stored_key.hpp" // `limit`

namespace unum::ustore::nvme {

using id_entity_t = std::int64_t;

struct label_t {
    std::int32_t raw = -1;

    friend constexpr auto operator<=>(label_t const&, label_t const&) noexcept = default;
    constexpr bool is_unknown() const noexcept { return raw < 0; }
};

struct vertex_t {
    id_entity_t id = 0;
    float weight = 1;
    label_t label;

    friend constexpr bool operator==(vertex_t const& lhs, vertex_t const& rhs) noexcept { return lhs.id == rhs.id; }
    friend constexpr auto operator<=>(vertex_t const& lhs, vertex_t const& rhs) noexcept { return lhs.id <=> rhs.id; }

    constexpr void clear() noexcept {
        id = 0;
        label.raw = 0;
        weight = 1;
    }
};

using vertices_set_t = st::basic_flat_set<vertex_t>;

/** Key of one chunk of the half-edges of an entity, ordered by entity and then by the chunk's first neighbor. */
struct id_chunk_t {
    id_entity_t entity = 0;
    id_entity_t chunk = 0;

    friend constexpr auto operator<=>(id_chunk_t const&, id_chunk_t const&) noexcept = default;

    constexpr explicit operator bool() const noexcept { return entity && chunk; }
    std::byte const* data() const noexcept { return reinterpret_cast<std::byte const*>(this); }
    constexpr std::size_t size() const noexcept { return sizeof(id_chunk_t); }

    constexpr void clear() noexcept {
        entity = 0;
        chunk = 0;
    }

    static constexpr id_chunk_t min(id_entity_t entity_id) noexcept { return id_chunk_t {entity_id, 0}; }
    static constexpr id_chunk_t max(id_entity_t entity_id) noexcept {
        return id_chunk_t {entity_id, id_entity_t {1} << 62};
    }
    static constexpr id_chunk_t min() noexcept { return id_chunk_t {0, 0}; }
    static constexpr id_chunk_t max() noexcept { return id_chunk_t {id_entity_t {1} << 62, id_entity_t {1} << 62}; }
};

static_assert(sizeof(id_chunk_t) == (sizeof(id_entity_t) * 2));

/** Bounds of a chunk identifier: the least and the greatest entity with the least and the greatest chunk. */
template <>
struct limit<id_chunk_t> {

    /** The chunk identifier of zero entity and zero chunk. */
    static constexpr id_chunk_t min() noexcept { return id_chunk_t::min(); }

    /** The greatest chunk identifier of the greatest entity. */
    static constexpr id_chunk_t max() noexcept { return id_chunk_t::max(); }
};

/** Formats a chunk identifier as `id<entity | chunk>`. */
struct id_chunk_formatter_t {

    constexpr auto parse(fmt::format_parse_context& context) { return context.begin(); }

    template <typename context_>
    auto format(id_chunk_t const& chunk_id, context_& context) const {
        return fmt::format_to(context.out(), "id<{} | {}>", chunk_id.entity, chunk_id.chunk);
    }
};

} // namespace unum::ustore::nvme

template <>
struct fmt::formatter<unum::ustore::nvme::id_chunk_t> : unum::ustore::nvme::id_chunk_formatter_t {};
