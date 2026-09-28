/**
 *  @file bench/louvain.cpp
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief Benchmarks Louvain community detection over a UStore graph collection and over the same graph in memory.
 *
 *  One edge list, read from a SNAP file or generated with planted communities, lands both in a graph
 *  collection and in an in-memory adjacency. One level optimizer and one aggregator serve either graph,
 *  so the gap between the two timings is the cost of reading each neighborhood through UStore.
 *  The store's adjacency is built by the upsert benchmark and the in-memory one before the run, so both
 *  community-detection benchmarks time the levels alone, plus the vertex scan the store needs.
 */

#include <cstddef> // `std::size_t`
#include <cstdio>  // `stderr`

#include <algorithm>   // `std::sort`, `std::unique`, `std::remove_if`, `std::lower_bound`
#include <array>       // `std::array`
#include <charconv>    // `std::from_chars`
#include <compare>     // `std::strong_ordering`
#include <limits>      // `std::numeric_limits`
#include <random>      // `std::mt19937_64`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`, `std::swap`

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include <smashtable/basic_vector.hpp>
#include <stringzilla/stringzilla.hpp>

#include <ustore/ustore.hpp>

#include "arguments.hpp"
#include "helpers/blocking_files.hpp" // `mapped_file_t`

namespace unum::ustore::bench {

namespace bm = benchmark;

#pragma region Settings

/** Command-line options of the benchmark, each field set by the flag of the same name. */
struct settings_t {

    /** File holding one source and target pair per line; a graph is generated without it. */
    std::string_view dataset;

    /** Directory the database lives in; the engine keeps the graph in memory without it. */
    std::string_view directory;

    /** The engine to open and the options of its init call. */
    engine_options_t engine;

    /** Vertices of the generated graph, trimmed down to a whole number of communities. */
    count_t vertices {.value = 4096, .minimum = 2};

    /** Communities planted in the generated graph. */
    count_t communities {.value = 16, .minimum = 1};

    /** Edges every generated vertex draws inside its own community. */
    count_t inner_degree {.value = 8, .minimum = 1};

    /** Edges every generated vertex draws to any vertex of the graph. */
    count_t outer_degree {.value = 1, .minimum = 0};

    /** Seed of the generated graph. */
    count_t seed {42};

    /** Edges kept from the dataset, counted before repeats collapse. */
    count_t max_edges {std::numeric_limits<std::size_t>::max()};

    /** Edges every upsert batch carries into the graph collection. */
    count_t upsert_batch_size {.value = 1024, .minimum = 1};

    /** Least modularity growth, in billionths, that justifies coarsening once more. */
    count_t min_modularity_growth_per_billion {100};

    /** Levels either run coarsens through at most. */
    count_t max_levels {.value = 32, .minimum = 1};
};

#pragma endregion Settings

#pragma region Edge Lists

/** One undirected edge of the input, its lower vertex first so repeats of a pair collapse. */
struct input_edge_t {

    /** Lower key of the two vertices the edge joins. */
    ustore_key_t lower;

    /** Higher key of the two vertices the edge joins. */
    ustore_key_t upper;

    friend bool operator==(input_edge_t const&, input_edge_t const&) = default;
    friend std::strong_ordering operator<=>(input_edge_t const&, input_edge_t const&) = default;
};

/** The edge joining @p first and @p second, with the lower key first. */
[[nodiscard]] input_edge_t make_input_edge(ustore_key_t first, ustore_key_t second) noexcept {
    return first <= second ? input_edge_t {first, second} : input_edge_t {second, first};
}

/** Parses the two keys @p line begins with, answering whether both were there. */
[[nodiscard]] bool parse_input_edge(std::string_view line, input_edge_t& edge) noexcept {
    std::size_t source_end = 0;
    while (source_end != line.size() && line[source_end] != ' ' && line[source_end] != '\t') ++source_end;
    std::size_t target_begin = source_end;
    while (target_begin != line.size() && (line[target_begin] == ' ' || line[target_begin] == '\t')) ++target_begin;

    std::uint64_t source = 0;
    std::uint64_t target = 0;
    auto const parsed_source = std::from_chars(line.data(), line.data() + source_end, source);
    auto const parsed_target = std::from_chars(line.data() + target_begin, line.data() + line.size(), target);
    if (parsed_source.ec != std::errc {} || parsed_target.ec != std::errc {}) return false;
    edge = make_input_edge(key_from_integer(source), key_from_integer(target));
    return true;
}

/**
 *  @brief Reads a SNAP edge list, one source and target pair per line, into @p edges.
 *
 *  Blank lines and those opening with @c # are skipped, and the pairs stop at @p max_edges.
 *  @param[in] path The null-terminated path of the edge list.
 *  @param[in] max_edges Pairs kept at most, counted before repeats collapse.
 *  @param[out] edges Receives one entry per kept pair.
 */
status_t read_edge_list(char const* path, std::size_t max_edges, st::basic_vector<input_edge_t>& edges) noexcept {
    auto [file, map_status] = mapped_file_t::map(path);
    if (map_status.failed()) return map_status;

    for (sz::string_view_t line : sz::split(file.view(), "\n")) {
        if (edges.size() == max_edges) break;
        std::string_view trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == ' ')) trimmed.remove_suffix(1);
        if (trimmed.empty() || trimmed.front() == '#') continue;
        input_edge_t edge {};
        if (!parse_input_edge(trimmed, edge)) return {status_code_t::malformed_k, "An edge list line holds no pair"};
        if (st::failed(edges.push_back(input_edge_t {edge}))) return status_code_t::out_of_memory_k;
    }
    return {};
}

/** Draws a planted partition: @c inner_degree edges inside each community and @c outer_degree across them. */
status_t generate_edge_list(settings_t const& settings, st::basic_vector<input_edge_t>& edges) noexcept {
    std::size_t const community_size = settings.vertices.value / settings.communities.value;
    if (community_size < 2) return {status_code_t::invalid_argument_k, "--vertices below twice --communities"};
    std::size_t const planted = community_size * settings.communities.value;
    std::size_t const drawn_per_vertex = settings.inner_degree.value + settings.outer_degree.value;
    if (st::failed(edges.reserve(planted * drawn_per_vertex))) return status_code_t::out_of_memory_k;

    std::mt19937_64 generator {settings.seed.value};
    for (std::size_t vertex = 0; vertex != planted; ++vertex) {
        std::size_t const community_begin = (vertex / community_size) * community_size;
        ustore_key_t const source = key_from_integer(vertex);
        for (std::size_t drawn = 0; drawn != settings.inner_degree.value; ++drawn) {
            ustore_key_t const target = key_from_integer(community_begin + generator() % community_size);
            [[maybe_unused]] st::status_t const pushed = edges.push_back(st::assume_reserved,
                                                                         make_input_edge(source, target));
        }
        for (std::size_t drawn = 0; drawn != settings.outer_degree.value; ++drawn) {
            ustore_key_t const target = key_from_integer(generator() % planted);
            [[maybe_unused]] st::status_t const pushed = edges.push_back(st::assume_reserved,
                                                                         make_input_edge(source, target));
        }
    }
    return {};
}

/** Orders @p edges, drops self-loops and collapses repeats, leaving one entry per undirected pair. */
void canonicalize_edges(st::basic_vector<input_edge_t>& edges) noexcept {
    input_edge_t* const begin = edges.begin();
    input_edge_t* const joined_end = std::remove_if(
        begin, edges.end(), [](input_edge_t const& edge) noexcept { return edge.lower == edge.upper; });
    std::sort(begin, joined_end);
    input_edge_t* const unique_end = std::unique(begin, joined_end);
    [[maybe_unused]] st::status_t const shrunk = edges.resize(static_cast<std::size_t>(unique_end - begin));
}

/** Lays out @p edges as the C ABI's triples, all carrying the default edge identifier. */
status_t stage_edges(std::span<input_edge_t const> edges, st::basic_vector<edge_t>& staged) noexcept {
    if (st::failed(staged.reserve(edges.size()))) return status_code_t::out_of_memory_k;
    for (input_edge_t const& edge : edges) {
        [[maybe_unused]] st::status_t const pushed = staged.push_back(
            st::assume_reserved, edge_t {edge.lower, edge.upper, ustore_default_edge_id_k});
    }
    return {};
}

#pragma endregion Edge Lists

#pragma region Graphs

/** One end of a weighted edge as a compressed adjacency row holds it. */
struct weighted_neighbor_t {

    /** Position of the neighbor in the graph's vertex numbering. */
    std::size_t vertex_index;

    /** Summed weight of the edges reaching that neighbor. */
    double weight;
};

/** One directed entry of an adjacency, before the compressed rows are laid out. */
struct weighted_entry_t {

    /** Key of the vertex the entry leaves. */
    ustore_key_t source;

    /** Key of the vertex the entry enters. */
    ustore_key_t target;

    /** Weight the entry carries. */
    double weight;
};

/** Orders entries by source and then by target, so repeats of a pair become adjacent. */
[[nodiscard]] bool entry_precedes(weighted_entry_t const& first, weighted_entry_t const& second) noexcept {
    return first.source != second.source ? first.source < second.source : first.target < second.target;
}

/**
 *  @brief A weighted undirected graph in compressed sparse row form, numbering its vertices from zero.
 *
 *  Every edge appears in the rows of both its ends and a self-loop appears twice in its own row, so a
 *  vertex's summed row weight is the degree modularity expects and a community's summed inner weight
 *  counts each of its edges twice.
 */
class adjacency_graph_t {

    /** Sorted distinct vertex keys, whose positions are the numbering the rows use. */
    st::basic_vector<ustore_key_t> keys_;

    /** Where each vertex's row begins in @c neighbors_, with one trailing end offset. */
    st::basic_vector<std::size_t> row_offsets_;

    /** The rows of every vertex, one after another. */
    st::basic_vector<weighted_neighbor_t> neighbors_;

  public:
    adjacency_graph_t() noexcept = default;

    /**
     *  @brief Lays out @p entries as compressed rows, summing the weights of repeated pairs.
     *
     *  @param[inout] entries Every edge in both directions, sorted in place; a self-loop appears twice.
     */
    static expected<adjacency_graph_t> make(st::basic_vector<weighted_entry_t>& entries) noexcept {
        std::sort(entries.begin(), entries.end(), entry_precedes);

        adjacency_graph_t graph;
        if (st::failed(graph.keys_.reserve(entries.size() + 1)) ||
            st::failed(graph.row_offsets_.reserve(entries.size() + 2)) ||
            st::failed(graph.neighbors_.reserve(entries.size())))
            return status_code_t::out_of_memory_k;

        for (std::size_t index = 0; index != entries.size(); ++index) {
            weighted_entry_t const& entry = entries[index];
            bool const opens_row = index == 0 || entries[index - 1].source != entry.source;
            if (opens_row) {
                [[maybe_unused]] st::status_t const keyed = graph.keys_.push_back(st::assume_reserved,
                                                                                  ustore_key_t {entry.source});
                [[maybe_unused]] st::status_t const opened = graph.row_offsets_.push_back(
                    st::assume_reserved, std::size_t {graph.neighbors_.size()});
            }
            bool const repeats = !opens_row && entries[index - 1].target == entry.target;
            if (repeats) graph.neighbors_[graph.neighbors_.size() - 1].weight += entry.weight;
            else {
                [[maybe_unused]] st::status_t const pushed = graph.neighbors_.push_back(
                    st::assume_reserved, weighted_neighbor_t {0, entry.weight});
            }
        }
        [[maybe_unused]] st::status_t const closed = graph.row_offsets_.push_back(
            st::assume_reserved, std::size_t {graph.neighbors_.size()});

        // The rows hold keys until every key is known, so the numbering is resolved in a second pass.
        std::size_t neighbor = 0;
        for (std::size_t index = 0; index != entries.size(); ++index) {
            if (index != 0 && entries[index - 1].source == entries[index].source &&
                entries[index - 1].target == entries[index].target)
                continue;
            graph.neighbors_[neighbor].vertex_index = graph.index_of(entries[index].target);
            ++neighbor;
        }
        return graph;
    }

    /** Vertices the graph numbers. */
    [[nodiscard]] std::size_t vertices_count() const noexcept { return keys_.size(); }

    /** Position of @p key in the numbering, or the vertex count when the graph holds no such vertex. */
    [[nodiscard]] std::size_t index_of(ustore_key_t key) const noexcept {
        ustore_key_t const* const found = std::lower_bound(keys_.begin(), keys_.end(), key);
        if (found == keys_.end() || *found != key) return keys_.size();
        return static_cast<std::size_t>(found - keys_.begin());
    }

    /** Hands @p visit every neighbor of @p vertex_index with the weight reaching it. */
    template <typename visitor_type_>
    status_t for_each_neighbor(std::size_t vertex_index, visitor_type_&& visit) noexcept {
        for (std::size_t slot = row_offsets_[vertex_index]; slot != row_offsets_[vertex_index + 1]; ++slot)
            visit(neighbors_[slot].vertex_index, neighbors_[slot].weight);
        return {};
    }
};

/** A graph collection read as unit-weight edges, numbering its vertices by the order the scan returns them. */
class stored_graph_t {

    /** The collection every neighborhood read reaches. */
    graph_collection_t* collection_ = nullptr;

    /** Sorted vertex keys of the collection, whose positions are the numbering. */
    st::basic_vector<ustore_key_t> keys_;

  public:
    stored_graph_t() noexcept = default;

    /** Streams every vertex key of @p collection into the numbering the levels run over. */
    static expected<stored_graph_t> make(graph_collection_t& collection) noexcept {
        auto maybe_stream = collection.vertex_stream();
        if (!maybe_stream) return maybe_stream.status();
        keys_stream_t stream = *std::move(maybe_stream);

        stored_graph_t graph;
        graph.collection_ = &collection;
        while (!stream.is_end()) {
            for (ustore_key_t key : stream.keys_batch())
                if (st::failed(graph.keys_.push_back(ustore_key_t {key}))) return status_code_t::out_of_memory_k;
            if (status_t const sought = stream.seek_to_next_batch(); sought.failed()) return sought;
        }
        return graph;
    }

    /** Vertices the collection holds. */
    [[nodiscard]] std::size_t vertices_count() const noexcept { return keys_.size(); }

    /** Hands @p visit every distinct neighbor of @p vertex_index, each carrying unit weight. */
    template <typename visitor_type_>
    status_t for_each_neighbor(std::size_t vertex_index, visitor_type_&& visit) noexcept {
        auto [neighbors, neighbors_status] = collection_->neighbors(keys_[vertex_index]);
        if (neighbors_status.failed()) return neighbors_status;
        for (std::size_t index = 0; index != neighbors.size(); ++index) {
            ustore_key_t const* const found = std::lower_bound(keys_.begin(), keys_.end(), neighbors[index]);
            if (found == keys_.end() || *found != neighbors[index]) continue;
            visit(static_cast<std::size_t>(found - keys_.begin()), 1.0);
        }
        return {};
    }
};

#pragma endregion Graphs

#pragma region Louvain

/** Whether a level moved any vertex, which decides whether coarsening once more is worth it. */
enum class level_progress_t : bool {
    settled_k,
    moved_k,
};

/** The communities one level settled on, with the degrees that scored them. */
struct level_t {

    /** Community of every vertex, named by the position of one of its members. */
    st::basic_vector<std::size_t> communities;

    /** Summed weight of every vertex's row, which is its weighted degree. */
    st::basic_vector<double> vertex_degrees;

    /** Whether any vertex changed community. */
    level_progress_t progress = level_progress_t::settled_k;
};

/** How good a level's communities turned out. */
struct score_t {

    /** Modularity of the communities. */
    double modularity = 0.0;

    /** Distinct communities the vertices fall into. */
    std::size_t communities_count = 0;
};

/**
 *  @brief Moves every vertex of @p graph into the neighboring community that raises modularity most.
 *
 *  Sweeps until no vertex moves, scoring a move by the standard gain that only the moved vertex's
 *  neighborhood and the two communities' summed degrees enter.
 *  @param[inout] graph The graph one level runs over, whose neighborhoods it reads repeatedly.
 */
template <typename graph_type_>
expected<level_t> optimize_level(graph_type_& graph) noexcept {
    std::size_t const count = graph.vertices_count();
    level_t level;
    st::basic_vector<double> community_degrees;
    st::basic_vector<double> degrees_into_communities;
    st::basic_vector<std::size_t> touched_communities;
    if (st::failed(level.communities.resize(count)) || st::failed(level.vertex_degrees.resize(count, 0.0)) ||
        st::failed(community_degrees.resize(count, 0.0)) || st::failed(degrees_into_communities.resize(count, 0.0)) ||
        st::failed(touched_communities.reserve(count)))
        return status_code_t::out_of_memory_k;

    double degrees_sum = 0.0;
    for (std::size_t vertex = 0; vertex != count; ++vertex) {
        double degree = 0.0;
        status_t const walked = graph.for_each_neighbor(vertex,
                                                        [&](std::size_t, double weight) noexcept { degree += weight; });
        if (walked.failed()) return walked;
        level.communities[vertex] = vertex;
        level.vertex_degrees[vertex] = degree;
        community_degrees[vertex] = degree;
        degrees_sum += degree;
    }

    double const edges_weight = degrees_sum / 2.0;
    if (edges_weight == 0.0) return level;

    std::size_t moved = 1;
    while (moved != 0) {
        moved = 0;
        for (std::size_t vertex = 0; vertex != count; ++vertex) {
            status_t const walked = graph.for_each_neighbor(vertex, [&](std::size_t neighbor, double weight) noexcept {
                if (neighbor == vertex) return;
                std::size_t const community = level.communities[neighbor];
                if (degrees_into_communities[community] == 0.0) {
                    [[maybe_unused]] st::status_t const pushed = touched_communities.push_back(st::assume_reserved,
                                                                                               std::size_t {community});
                }
                degrees_into_communities[community] += weight;
            });
            if (walked.failed()) return walked;

            std::size_t const own_community = level.communities[vertex];
            double const vertex_degree = level.vertex_degrees[vertex];
            double const degree_into_own = degrees_into_communities[own_community];
            double best_gain = 0.0;
            std::size_t best_community = own_community;
            for (std::size_t const community : touched_communities) {
                if (community == own_community) continue;
                double const gain =
                    (degrees_into_communities[community] - degree_into_own) / edges_weight +
                    vertex_degree * (community_degrees[own_community] - vertex_degree - community_degrees[community]) /
                        (2.0 * edges_weight * edges_weight);
                if (gain <= best_gain) continue;
                best_gain = gain;
                best_community = community;
            }
            for (std::size_t const community : touched_communities) degrees_into_communities[community] = 0.0;
            touched_communities.clear();

            if (best_community == own_community) continue;
            community_degrees[own_community] -= vertex_degree;
            community_degrees[best_community] += vertex_degree;
            level.communities[vertex] = best_community;
            ++moved;
            level.progress = level_progress_t::moved_k;
        }
    }
    return level;
}

/** Scores @p level over @p graph in one pass, summing each community's inner weight and total degree. */
template <typename graph_type_>
expected<score_t> score_level(graph_type_& graph, level_t const& level) noexcept {
    std::size_t const count = graph.vertices_count();
    st::basic_vector<double> inner_weights;
    st::basic_vector<double> total_degrees;
    st::basic_vector<std::size_t> member_counts;
    if (st::failed(inner_weights.resize(count, 0.0)) || st::failed(total_degrees.resize(count, 0.0)) ||
        st::failed(member_counts.resize(count, 0)))
        return status_code_t::out_of_memory_k;

    double degrees_sum = 0.0;
    for (std::size_t vertex = 0; vertex != count; ++vertex) {
        std::size_t const community = level.communities[vertex];
        total_degrees[community] += level.vertex_degrees[vertex];
        member_counts[community] += 1;
        degrees_sum += level.vertex_degrees[vertex];
        status_t const walked = graph.for_each_neighbor(vertex, [&](std::size_t neighbor, double weight) noexcept {
            if (level.communities[neighbor] == community) inner_weights[community] += weight;
        });
        if (walked.failed()) return walked;
    }

    score_t score;
    double const edges_weight = degrees_sum / 2.0;
    for (std::size_t community = 0; community != count; ++community) {
        if (member_counts[community] == 0) continue;
        ++score.communities_count;
        if (edges_weight == 0.0) continue;
        double const share = total_degrees[community] / (2.0 * edges_weight);
        score.modularity += inner_weights[community] / (2.0 * edges_weight) - share * share;
    }
    return score;
}

/** The graph of @p level's communities, whose weights sum the edges between and inside their members. */
template <typename graph_type_>
expected<adjacency_graph_t> aggregate_level(graph_type_& graph, level_t const& level) noexcept {
    std::size_t const count = graph.vertices_count();
    st::basic_vector<weighted_entry_t> entries;
    for (std::size_t vertex = 0; vertex != count; ++vertex) {
        ustore_key_t const source = key_from_integer(level.communities[vertex]);
        st::status_t pushed = st::status_t::success_k;
        status_t const walked = graph.for_each_neighbor(vertex, [&](std::size_t neighbor, double weight) noexcept {
            ustore_key_t const target = key_from_integer(level.communities[neighbor]);
            if (st::succeeded(pushed)) pushed = entries.push_back(weighted_entry_t {source, target, weight});
        });
        if (walked.failed()) return walked;
        if (st::failed(pushed)) return status_code_t::out_of_memory_k;
    }
    return adjacency_graph_t::make(entries);
}

/** What one Louvain run found. */
struct run_result_t {

    /** Modularity of the coarsest partition the run kept. */
    double modularity = 0.0;

    /** Communities that partition holds. */
    std::size_t communities_count = 0;

    /** Levels the run completed. */
    std::size_t levels_count = 0;
};

/**
 *  @brief Runs Louvain over @p graph, coarsening it level by level while modularity grows enough.
 *
 *  A coarse level's modularity is the modularity of the partition it stands for on the original graph,
 *  so the run reports scores and counts rather than flattening the levels back into one partition.
 *  @param[inout] graph The finest graph, read once per sweep of the first level.
 *  @param[in] settings Supplies the growth threshold and the ceiling on levels.
 */
template <typename graph_type_>
expected<run_result_t> run_louvain(graph_type_& graph, settings_t const& settings) noexcept {
    double const min_growth = static_cast<double>(settings.min_modularity_growth_per_billion.value) / 1e9;

    auto [level, optimize_level_status] = optimize_level(graph);
    if (optimize_level_status.failed()) return optimize_level_status;
    auto [score, score_level_status] = score_level(graph, level);
    if (score_level_status.failed()) return score_level_status;

    run_result_t result;
    result.modularity = score.modularity;
    result.communities_count = score.communities_count;
    result.levels_count = 1;
    if (level.progress == level_progress_t::settled_k) return result;

    auto [coarse, aggregate_level_status] = aggregate_level(graph, level);
    if (aggregate_level_status.failed()) return aggregate_level_status;

    while (result.levels_count != settings.max_levels.value) {
        auto [coarse_level, optimize_coarse_status] = optimize_level(coarse);
        if (optimize_coarse_status.failed()) return optimize_coarse_status;
        auto [coarse_score, score_coarse_status] = score_level(coarse, coarse_level);
        if (score_coarse_status.failed()) return score_coarse_status;
        if (coarse_level.progress == level_progress_t::settled_k ||
            coarse_score.modularity - result.modularity <= min_growth)
            break;

        result.modularity = coarse_score.modularity;
        result.communities_count = coarse_score.communities_count;
        ++result.levels_count;
        auto [coarser, aggregate_coarse_status] = aggregate_level(coarse, coarse_level);
        if (aggregate_coarse_status.failed()) return aggregate_coarse_status;
        coarse = std::move(coarser);
    }
    return result;
}

#pragma endregion Louvain

#pragma region Benchmarks

/** Upserts @p staged into @p graph in batches of @p batch_size, reporting the edges and bytes it wrote. */
void bench_graph_upsert(bm::State& state, graph_collection_t& graph, std::span<edge_t const> staged,
                        std::size_t batch_size) {
    std::size_t committed = 0;
    for ([[maybe_unused]] auto iteration : state) {
        for (std::size_t offset = 0; offset < staged.size(); offset += batch_size) {
            std::size_t const count = std::min(batch_size, staged.size() - offset);
            edge_t const* const begin = staged.data() + offset;
            status_t const upserted = graph.upsert_edges(edges_view_t {begin, begin + count});
            if (upserted.failed()) {
                state.SkipWithError(upserted.message ? upserted.message : "Failed to upsert a batch of edges");
                return;
            }
            committed += count;
        }
    }
    state.counters["edges/s"] = bm::Counter(static_cast<double>(committed), bm::Counter::kIsRate);
    state.counters["bytes/s"] = bm::Counter(static_cast<double>(committed * sizeof(edge_t)), bm::Counter::kIsRate);
}

/** Runs Louvain over @p graph, reporting its modularity, communities and levels into @p result. */
template <typename graph_type_>
void bench_louvain(bm::State& state, graph_type_& graph, settings_t const& settings, run_result_t& result) {
    std::size_t vertices = 0;
    for ([[maybe_unused]] auto iteration : state) {
        auto [run, run_louvain_status] = run_louvain(graph, settings);
        if (run_louvain_status.failed()) {
            state.SkipWithError(run_louvain_status.message ? run_louvain_status.message
                                                           : "Failed to detect communities");
            return;
        }
        result = run;
        vertices += graph.vertices_count();
    }
    state.counters["vertices/s"] = bm::Counter(static_cast<double>(vertices), bm::Counter::kIsRate);
    state.counters["modularity"] = bm::Counter(result.modularity);
    state.counters["communities"] = bm::Counter(static_cast<double>(result.communities_count));
    state.counters["levels"] = bm::Counter(static_cast<double>(result.levels_count));
}

/** Runs Louvain over the graph collection, whose vertex scan and neighborhood reads it also times. */
void bench_louvain_over_ustore(bm::State& state, graph_collection_t& graph, settings_t const& settings,
                               run_result_t& result) {
    auto [stored, make_stored_graph_status] = stored_graph_t::make(graph);
    if (make_stored_graph_status.failed()) {
        state.SkipWithError(make_stored_graph_status.message ? make_stored_graph_status.message
                                                             : "Failed to scan the graph collection");
        return;
    }
    bench_louvain(state, stored, settings, result);
}

#pragma endregion Benchmarks

} // namespace unum::ustore::bench

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::bench;

    settings_t settings;
    std::array<flag_t, 14> const flags {{
        {"--dataset", "path", "SNAP edge list; a planted partition is generated without it", &settings.dataset},
        {"--engine", "name", "engine to open: ram, rocksdb, leveldb or nvme", &settings.engine.name},
        {"--isolation", "name", "isolation of the transactions of ram", &settings.engine.isolation},
        {"--engine-transport", "name", "transport of nvme", &settings.engine.transport},
        {"--directory", "path", "database directory; the graph stays in memory without it", &settings.directory},
        {"--vertices", "count", "vertices of the generated graph", &settings.vertices},
        {"--communities", "count", "communities planted in the generated graph", &settings.communities},
        {"--inner-degree", "count", "edges each generated vertex draws inside its community", &settings.inner_degree},
        {"--outer-degree", "count", "edges each generated vertex draws across communities", &settings.outer_degree},
        {"--seed", "count", "seed of the generated graph", &settings.seed},
        {"--max-edges", "count", "edges kept from the dataset", &settings.max_edges},
        {"--upsert-batch-size", "count", "edges per upsert batch", &settings.upsert_batch_size},
        {"--min-modularity-growth-per-billion", "count", "least modularity growth that justifies a level",
         &settings.min_modularity_growth_per_billion},
        {"--max-levels", "count", "levels either run coarsens through", &settings.max_levels},
    }};

    // Google Benchmark exits on `--help` inside its initialization, so these flags print first
    for (int index = 1; index < argc; ++index)
        if (std::string_view {argv[index]} == "--help") {
            print_usage(argv[0], flags);
            break;
        }
    bm::Initialize(&argc, argv);
    arguments_t const parsed = parse_arguments(argc, argv, flags, {});
    if (parsed.status == argument_status_t::help_k) return 0;
    if (parsed.status != argument_status_t::parsed_k) {
        fmt::println(stderr, "{}: {}", parsed.argument, describe(parsed.status));
        return 1;
    }

    st::basic_vector<input_edge_t> edges;
    status_t const loaded = settings.dataset.empty()
                                ? generate_edge_list(settings, edges)
                                : read_edge_list(settings.dataset.data(), settings.max_edges.value, edges);
    if (loaded.failed()) {
        fmt::println(stderr, "Failed to prepare the edge list: {}", loaded.message ? loaded.message : "unknown");
        return 1;
    }
    canonicalize_edges(edges);
    if (edges.size() == 0) {
        fmt::println(stderr, "The edge list holds no edges");
        return 1;
    }

    st::basic_vector<edge_t> staged;
    st::basic_vector<weighted_entry_t> entries;
    if (stage_edges(edges, staged).failed() || st::failed(entries.reserve(2 * edges.size()))) {
        fmt::println(stderr, "Failed to stage the edges");
        return 1;
    }
    for (input_edge_t const& edge : edges) {
        [[maybe_unused]] st::status_t const forward = entries.push_back(st::assume_reserved,
                                                                        weighted_entry_t {edge.lower, edge.upper, 1.0});
        [[maybe_unused]] st::status_t const backward = entries.push_back(
            st::assume_reserved, weighted_entry_t {edge.upper, edge.lower, 1.0});
    }

    auto maybe_memory_graph = adjacency_graph_t::make(entries);
    if (!maybe_memory_graph) {
        fmt::println(stderr, "Failed to lay out the in-memory adjacency");
        return 1;
    }
    adjacency_graph_t memory_graph = *std::move(maybe_memory_graph);
    fmt::println("Graph: {} vertices, {} edges", memory_graph.vertices_count(), edges.size());

    ustore_database_t raw = nullptr;
    ustore_error_t error {};
    settings.engine.directory = settings.directory.data();
    open_engine(settings.engine, &raw, &error);
    if (error.status) {
        fmt::println(stderr, "Failed to open the database: {}", error.message ? error.message : "unknown");
        return 1;
    }
    database_t database {raw};
    if (status_t const cleared = database.clear(); cleared.failed()) {
        fmt::println(stderr, "Failed to clear the database: {}", cleared.message ? cleared.message : "unknown");
        return 1;
    }
    auto [graph, graph_status] = database.create<graph_collection_t>("graph", ustore_modality_graph_k);
    if (graph_status.failed()) {
        fmt::println(stderr, "Failed to create the graph: {}", graph_status.message ? graph_status.message : "unknown");
        return 1;
    }

    run_result_t memory_result;
    run_result_t stored_result;
    bm::RegisterBenchmark(
        "graph_upsert",
        [&](bm::State& state) { bench_graph_upsert(state, graph, staged, settings.upsert_batch_size.value); })
        ->Iterations(1)
        ->UseRealTime();
    bm::RegisterBenchmark("louvain_in_memory",
                          [&](bm::State& state) { bench_louvain(state, memory_graph, settings, memory_result); })
        ->Iterations(1)
        ->UseRealTime();
    bm::RegisterBenchmark("louvain_over_ustore",
                          [&](bm::State& state) { bench_louvain_over_ustore(state, graph, settings, stored_result); })
        ->Iterations(1)
        ->UseRealTime();

    bm::RunSpecifiedBenchmarks();
    bm::Shutdown();

    fmt::println("In memory: modularity {:.6f}, {} communities, {} levels", memory_result.modularity,
                 memory_result.communities_count, memory_result.levels_count);
    fmt::println("Over UStore: modularity {:.6f}, {} communities, {} levels", stored_result.modularity,
                 stored_result.communities_count, stored_result.levels_count);
    return 0;
}
