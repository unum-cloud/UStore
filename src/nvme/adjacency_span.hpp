/**
 *  @file src/nvme/adjacency_span.hpp
 *  @author Ash Vardanian
 *  @date December 1, 2022
 *  @brief Non-owning adjacency-list view over weighted directed sparse graphs.
 */

#pragma once
#include <cstddef> // `std::size_t`

#include <span> // `std::span`

#include "nvme/vertex.hpp"

namespace unum::ustore::nvme {

/** Weighted edge between two vertices of an adjacency span. */
template <typename weight_, typename vertex_id_>
struct adjacency_edge {
    vertex_id_ first;
    vertex_id_ second;
    weight_ weight;
};

/**
 *  @brief Adjacency list of a weighted directed sparse graph, viewed as three parallel arrays.
 *  @tparam weight_ Type of the edge weights.
 *  @tparam vertex_id_ Type of the vertex identifiers.
 */
template <typename weight_, typename vertex_id_>
class adjacency_span {
  public:
    constexpr adjacency_span() noexcept = default;
    constexpr adjacency_span(std::span<vertex_id_> first_ids, std::span<vertex_id_> second_ids,
                             std::span<weight_> edge_weights) noexcept
        : firsts_(first_ids), seconds_(second_ids), weights_(edge_weights) {}

    constexpr std::size_t size() const noexcept { return firsts_.size(); }
    constexpr std::span<vertex_id_ const> firsts() const noexcept { return firsts_; }
    constexpr std::span<vertex_id_ const> seconds() const noexcept { return seconds_; }
    constexpr std::span<weight_ const> weights() const noexcept { return weights_; }

    constexpr adjacency_edge<weight_ const&, vertex_id_ const&> operator[](std::size_t index) const noexcept {
        return {firsts_[index], seconds_[index], weights_[index]};
    }
    constexpr adjacency_edge<weight_&, vertex_id_&> operator[](std::size_t index) noexcept {
        return {firsts_[index], seconds_[index], weights_[index]};
    }

  private:
    std::span<vertex_id_> firsts_;
    std::span<vertex_id_> seconds_;
    std::span<weight_> weights_;
};

using adjacency_edge_t = adjacency_edge<float, id_entity_t>;
using adjacency_span_t = adjacency_span<float const, id_entity_t const>;

} // namespace unum::ustore::nvme
