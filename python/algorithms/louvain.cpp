/**
 * @file louvain.cpp
 * @author Davit Vardanyan
 * @version 0.1
 * @date 2023-01-26
 *
 * @brief Louvain algorithm for Community Detection.
 *
 * This Louvain algorithm implementation is split into 2 phases:
 * 1. Extract communities from a Graph database, building a super-graph of communities,
 * 2. Repeatedly extract higher-level communities from the in-memory graph of smaller communities.
 *
 * Just like in the original implementations, the goal is to maximize the modularity metric.
 * Unlike most implementations, however, during iterations we only compute the "delta",
 * which makes the implementation more efficient.
 *
 * @copyright Copyright (c) 2023
 */
#include "ukv/ukv.hpp"

using namespace unum::ukv;
using namespace unum;

struct community_degree_t {
    double in_degree {};
    double tot_degree {};
};

using partition_t = std::unordered_map<ukv_key_t, ukv_key_t>;
using vertex_degrees_t = std::unordered_map<ukv_key_t, double>;
using graph_t = std::unordered_map<ukv_key_t, std::unordered_map<ukv_key_t, double>>;
using community_degrees_t = std::unordered_map<ukv_key_t, community_degree_t>;

bool first_phase(graph_collection_t& graph,
                 partition_t& partition,
                 vertex_degrees_t& degrees,
                 community_degrees_t& community_degrees,
                 std::size_t count_edges) noexcept(false) {

    bool improvement = false;
    bool modified = true;
    auto stream = graph.vertex_stream().throw_or_release();
    vertex_degrees_t degree_in_coms;

    while (modified) {
        modified = false;
        stream.seek_to_first();
        while (!stream.is_end()) {
            auto vertex = stream.key();
            auto vertex_degree = degrees[vertex];
            auto vertex_community = partition[vertex];
            auto best_mod = 0.0;
            auto best_com = vertex_community;
            auto vertex_com_tot_degree = community_degrees[vertex_community].tot_degree;

            degree_in_coms.clear();
            auto neighbors = graph.neighbors(vertex).throw_or_release();
            for (auto neighbor : neighbors)
                degree_in_coms[partition[neighbor]] += 1;
            auto vertex_in_vertex_com_degree = degree_in_coms[vertex_community];

            for (auto neighbor : neighbors) {
                auto neighbor_community = partition[neighbor];
                if (vertex_community == neighbor_community)
                    continue;

                auto neighbor_com_tot_degree = community_degrees[neighbor_community].tot_degree;
                auto vertex_in_neighbor_com_degree = degree_in_coms[neighbor_community];
                auto delta = (1.0 / count_edges) * (vertex_in_neighbor_com_degree - vertex_in_vertex_com_degree) -
                             (vertex_degree / (2.0 * (count_edges * count_edges))) *
                                 (vertex_degree + neighbor_com_tot_degree - vertex_com_tot_degree);

                if (delta > best_mod) {
                    best_mod = delta;
                    best_com = neighbor_community;
                }
            }

            if (best_com != vertex_community) {
                community_degrees[vertex_community].tot_degree -= (vertex_degree - vertex_in_vertex_com_degree);
                auto vertex_in_best_com_degree = degree_in_coms[best_com];
                community_degrees[best_com].tot_degree += (vertex_degree - vertex_in_best_com_degree);
                community_degrees[vertex_community].in_degree -= vertex_in_vertex_com_degree;
                community_degrees[best_com].in_degree += vertex_in_best_com_degree;
                partition[vertex] = best_com;
                modified = true;
                improvement = true;
            }
            ++stream;
        }
    }

    return improvement;
}

bool second_phase(graph_t& graph,
                  partition_t& partition,
                  vertex_degrees_t& degrees,
                  community_degrees_t& community_degrees,
                  std::size_t count_edges) noexcept(false) {

    bool improvement = false;
    bool modified = true;
    vertex_degrees_t degree_in_coms;

    while (modified) {
        modified = false;
        for (auto const& graph_iter : graph) {
            auto const& vertex = graph_iter.first;
            auto const& neighbors = graph_iter.second;
            auto vertex_degree = degrees[vertex];
            auto vertex_community = partition[vertex];
            auto best_mod = 0.0;
            auto best_com = vertex_community;
            auto vertex_com_tot_degree = community_degrees[vertex_community].tot_degree;

            degree_in_coms.clear();
            for (auto const& neighbor_iter : neighbors)
                degree_in_coms[partition[neighbor_iter.first]] += neighbor_iter.second;
            auto vertex_in_vertex_com_degree = degree_in_coms[vertex_community];

            for (auto const& neighbor_iter : neighbors) {
                auto neighbor_community = partition[neighbor_iter.first];
                if (vertex_community == neighbor_community)
                    continue;

                auto neighbor_com_tot_degree = community_degrees[neighbor_community].tot_degree;
                auto vertex_in_neighbor_com_degree = degree_in_coms[neighbor_community];
                auto delta = (1.0 / count_edges) * (vertex_in_neighbor_com_degree - vertex_in_vertex_com_degree) -
                             (vertex_degree / (2.0 * (count_edges * count_edges))) *
                                 (vertex_degree + neighbor_com_tot_degree - vertex_com_tot_degree);

                if (delta > best_mod) {
                    best_mod = delta;
                    best_com = neighbor_community;
                }
            }

            if (best_com != vertex_community) {
                community_degrees[vertex_community].tot_degree -= (vertex_degree - vertex_in_vertex_com_degree);
                auto vertex_in_best_com_degree = degree_in_coms[best_com];
                community_degrees[best_com].tot_degree += (vertex_degree - vertex_in_best_com_degree);
                community_degrees[vertex_community].in_degree -= vertex_in_vertex_com_degree;
                community_degrees[best_com].in_degree += vertex_in_best_com_degree;
                partition[vertex] = best_com;
                modified = true;
                improvement = true;
            }
        }
    }

    return improvement;
}

double modularity(partition_t const& partition, community_degrees_t const& community_degrees, double deg_sum) noexcept {
    auto m = deg_sum / 2.0;
    auto norm = 1.0 / (m * m);
    auto res = 0.0;
    for (auto const& iter : partition) {
        community_degree_t const& community_degree = community_degrees.at(iter.second);
        res += ((community_degree.tot_degree - community_degree.in_degree) / m) - (deg_sum * deg_sum * norm);
    }
    return res;
}

graph_t induce_community_graph(graph_collection_t& graph, partition_t const& partition) noexcept(false) {

    graph_t induced_graph;
    auto stream = graph.vertex_stream().throw_or_release();

    while (!stream.is_end()) {
        auto vertex = stream.key();
        auto vertex_com = partition.at(vertex);
        auto neighbors = graph.neighbors(vertex).throw_or_release();
        for (auto neighbor : neighbors) {
            auto neighbor_com = partition.at(neighbor);
            if (vertex_com == neighbor_com)
                continue;
            induced_graph[vertex_com][neighbor_com] += 1;
        }
        ++stream;
    }

    return induced_graph;
}

graph_t induce_community_graph(graph_t const& graph, partition_t const& partition) noexcept(false) {

    graph_t induced_graph;
    for (auto const& graph_iter : graph) {
        auto vertex_com = partition.at(graph_iter.first);
        for (auto const& neighbor_iter : graph_iter.second) {
            auto neighbor_com = partition.at(neighbor_iter.first);
            if (vertex_com == neighbor_com)
                continue;
            induced_graph[vertex_com][neighbor_com] += neighbor_iter.second;
        }
    }

    return induced_graph;
}

partition_t best_partition(graph_collection_t& graph_collection,
                           float min_modularity_growth = 0.0000001) noexcept(false) {

    auto count_vertices = graph_collection.number_of_vertices();
    auto count_edges = graph_collection.number_of_edges();

    partition_t partition;
    std::vector<partition_t> partitions;
    vertex_degrees_t vertex_degrees;
    community_degrees_t community_degrees;

    partition.reserve(count_vertices);
    vertex_degrees.reserve(count_vertices);
    community_degrees.reserve(count_vertices);

    auto stream = graph_collection.vertex_stream().throw_or_release();
    while (!stream.is_end()) {
        auto vertices = stream.keys_batch();
        auto degrees = graph_collection.degrees(vertices).throw_or_release();
        for (std::size_t i = 0; i != vertices.size(); ++i) {
            partition[vertices[i]] = vertices[i];
            vertex_degrees[vertices[i]] = degrees[i];
            community_degrees[vertices[i]] = {0, degrees[i]};
        }
        stream.seek_to_next_batch();
    }

    bool improvement = first_phase(graph_collection, partition, vertex_degrees, community_degrees, count_edges);
    auto mod = modularity(partition, community_degrees, count_edges);
    auto graph = induce_community_graph(graph_collection, partition);
    partitions.insert(partitions.begin(), std::move(partition));

    while (improvement) {
        partition_t partition;
        partition.reserve(graph.size());
        vertex_degrees.clear();
        community_degrees.clear();

        auto degree_sum = 0;
        count_edges = 0;
        for (auto const& graph_iter : graph) {
            partition[graph_iter.first] = graph_iter.first;
            double degree = 0;
            for (auto const& neighbor_iter : graph_iter.second) {
                degree += neighbor_iter.second;
                count_edges += 1;
            }
            degree_sum += degree;
            vertex_degrees[graph_iter.first] = degree;
            community_degrees[graph_iter.first] = {0, degree};
        }

        improvement = second_phase(graph, partition, vertex_degrees, community_degrees, count_edges / 2);
        auto new_mod = modularity(partition, community_degrees, degree_sum / 2);
        if (new_mod - mod <= min_modularity_growth)
            break;

        graph = induce_community_graph(graph, partition);
        partitions.insert(partitions.begin(), std::move(partition));
        mod = new_mod;
    }

    partition = partitions[0];
    for (std::size_t index = 1; index != partitions.size(); ++index) {
        for (auto const& iter : partitions[index])
            partitions[index][iter.first] = partition[iter.second];
        partition = partitions[index];
    }

    return partition;
}