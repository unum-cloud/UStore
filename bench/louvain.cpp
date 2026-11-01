/**
 *  @file bench/louvain.cpp
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief Benchmarks Louvain community detection over a UStore graph collection and over the same graph in memory.
 *
 *  One edge list, read from a SNAP file or generated with planted communities, lands both in a
 *  graph collection and in an in-memory adjacency. One level optimizer and one aggregator serve
 *  either graph, so the gap between the two timings is the cost of reading each neighborhood
 *  through UStore. The store's adjacency is built by the upsert benchmark and the in-memory one
 *  before the run, so both community-detection benchmarks time the levels alone, plus the vertex
 *  scan the store needs.
 *
 *  Beside the flags of @c bench/harness.hpp, whose @c --seed also seeds the generated graph, it takes:
 *
 *  Flag                                   Default                Meaning
 *  `--dataset`                            generated              SNAP edge list
 *  `--directory`                          in memory              Database directory
 *  `--engine`                             `ram`                  Engine to open
 *  `--isolation`                          `strict-serializable`  Isolation of the transactions of `ram`
 *  `--engine-transport`                   `automatic`            Transport of `nvme`
 *  `--vertices`                           `4096`                 Vertices of the generated graph
 *  `--communities`                        `16`                   Communities planted in the generated graph
 *  `--inner-degree`                       `8`                    Edges a vertex draws in its community
 *  `--outer-degree`                       `1`                    Edges a vertex draws across communities
 *  `--max-edges`                          every edge             Edges kept from the dataset
 *  `--batch-size-upsert`                  `1024`                 Edges per upsert batch
 *  `--min-modularity-growth-per-billion`  `100`                  Billionths of modularity a level must add
 *  `--max-levels`                         `32`                   Levels either run coarsens through
 */

#include <cstddef> // `std::size_t`
#include <cstdio>  // `stderr`

#include <algorithm>   // `std::sort`, `std::unique`, `std::remove_if`, `std::lower_bound`
#include <array>       // `std::array`
#include <charconv>    // `std::from_chars`
#include <compare>     // `std::strong_ordering`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <fmt/format.h>

#include <smashtable/basic_vector.hpp>
#include <stringzilla/stringzilla.hpp>

#include "ustore/ustore.hpp"

#include "ustore/utilities/blocking_files.hpp" // `mapped_file_t`

#include "harness.hpp"

namespace unum::ustore::bench {

#pragma region Settings

/** Command-line options of the benchmark, each field set by the flag of the same name. */
struct settings_t : shared_settings_t {

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

    /** Edges kept from the dataset, counted before repeats collapse. */
    count_t max_edges {std::numeric_limits<std::size_t>::max()};

    /** Edges every upsert batch carries into the graph collection. */
    count_t batch_size_upsert {.value = 1024, .minimum = 1};

    /** Least modularity growth, in billionths, that justifies coarsening once more. */
    count_t min_modularity_growth_per_billion {.value = 100, .minimum = 0};

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
status_t read_edge_list(char const* path, std::size_t max_edges, st::vector<input_edge_t>& edges) noexcept {
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
status_t generate_edge_list(settings_t const& settings, st::vector<input_edge_t>& edges) noexcept {
    std::size_t const community_size = settings.vertices.value / settings.communities.value;
    if (community_size < 2) return {status_code_t::invalid_argument_k, "--vertices below twice --communities"};
    std::size_t const planted = community_size * settings.communities.value;
    std::size_t const drawn_per_vertex = settings.inner_degree.value + settings.outer_degree.value;
    if (st::failed(edges.reserve(planted * drawn_per_vertex))) return status_code_t::out_of_memory_k;

    splitmix64_t random {stream_key(settings.seed.value, "edges", 0)};
    for (std::size_t vertex = 0; vertex != planted; ++vertex) {
        std::size_t const community_begin = (vertex / community_size) * community_size;
        ustore_key_t const source = key_from_integer(vertex);
        for (std::size_t drawn = 0; drawn != settings.inner_degree.value; ++drawn) {
            ustore_key_t const target = key_from_integer(community_begin + random.below(community_size));
            [[maybe_unused]] st::status_t const pushed = edges.push_back(st::assume_reserved,
                                                                         make_input_edge(source, target));
        }
        for (std::size_t drawn = 0; drawn != settings.outer_degree.value; ++drawn) {
            ustore_key_t const target = key_from_integer(random.below(planted));
            [[maybe_unused]] st::status_t const pushed = edges.push_back(st::assume_reserved,
                                                                         make_input_edge(source, target));
        }
    }
    return {};
}

/** Orders @p edges, drops self-loops and collapses repeats, leaving one entry per undirected pair. */
void canonicalize_edges(st::vector<input_edge_t>& edges) noexcept {
    input_edge_t* const begin = edges.begin();
    input_edge_t* const joined_end = std::remove_if(
        begin, edges.end(), [](input_edge_t const& edge) noexcept { return edge.lower == edge.upper; });
    std::sort(begin, joined_end);
    input_edge_t* const unique_end = std::unique(begin, joined_end);
    [[maybe_unused]] st::status_t const shrunk = edges.resize(static_cast<std::size_t>(unique_end - begin));
}

/** Lays out @p edges as the C ABI's triples, all carrying the default edge identifier. */
status_t stage_edges(std::span<input_edge_t const> edges, st::vector<edge_t>& staged) noexcept {
    if (st::failed(staged.reserve(edges.size()))) return status_code_t::out_of_memory_k;
    for (input_edge_t const& edge : edges) {
        [[maybe_unused]] st::status_t const pushed = staged.push_back(
            st::assume_reserved, edge_t {edge.lower, edge.upper, ustore_edge_id_default_k});
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
    st::vector<ustore_key_t> keys_;

    /** Where each vertex's row begins in @c neighbors_, with one trailing end offset. */
    st::vector<std::size_t> row_offsets_;

    /** The rows of every vertex, one after another. */
    st::vector<weighted_neighbor_t> neighbors_;

  public:
    adjacency_graph_t() noexcept = default;

    /**
     *  @brief Lays out @p entries as compressed rows, summing the weights of repeated pairs.
     *
     *  @param[in,out] entries Every edge in both directions, sorted in place; a self-loop appears twice.
     */
    static expected<adjacency_graph_t> make(st::vector<weighted_entry_t>& entries) noexcept {
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
    st::vector<ustore_key_t> keys_;

    /** Arena every neighborhood read reuses, rewound by each. */
    arena_t arena_;

  public:
    stored_graph_t() noexcept = default;

    /** Streams every vertex key of @p collection into the numbering the levels run over. */
    static expected<stored_graph_t> make(graph_collection_t& collection) noexcept {
        auto maybe_stream = collection.vertex_stream();
        if (!maybe_stream) return maybe_stream.status();
        keys_stream_t stream = *std::move(maybe_stream);
        auto [arena, arena_status] = arena_t::make();
        if (arena_status.failed()) return arena_status;

        stored_graph_t graph;
        graph.collection_ = &collection;
        graph.arena_ = std::move(arena);
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
        auto [neighbors, neighbors_status] = collection_->neighbors(keys_[vertex_index], arena_);
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
    st::vector<std::size_t> communities;

    /** Summed weight of every vertex's row, which is its weighted degree. */
    st::vector<double> vertex_degrees;

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
 *  @param[in,out] graph The graph one level runs over, whose neighborhoods it reads repeatedly.
 */
template <typename graph_type_>
expected<level_t> optimize_level(graph_type_& graph) noexcept {
    std::size_t const count = graph.vertices_count();
    level_t level;
    st::vector<double> community_degrees;
    st::vector<double> degrees_into_communities;
    st::vector<std::size_t> touched_communities;
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
    st::vector<double> inner_weights;
    st::vector<double> total_degrees;
    st::vector<std::size_t> member_counts;
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
    st::vector<weighted_entry_t> entries;
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
 *  @param[in,out] graph The finest graph, read once per sweep of the first level.
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

/** Runs Louvain over @p graph into @p result, counting its vertices, or skips @p loop on a failure. */
template <typename graph_type_>
void louvain_call(loop_t& loop, graph_type_& graph, settings_t const& settings, run_result_t& result) noexcept {
    auto [run, run_louvain_status] = run_louvain(graph, settings);
    if (run_louvain_status.failed()) return loop.skip("failed to detect communities");
    result = run;
    loop.count(graph.vertices_count());
}

#pragma endregion Benchmarks

} // namespace unum::ustore::bench

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::bench;

    settings_t settings;
    std::array<flag_t, 13> const flags {{
        {"--dataset", "path", "SNAP edge list; a planted partition is generated without it", &settings.dataset},
        {"--engine", "name", "engine to open: ram, rocksdb, leveldb or nvme", &settings.engine.name},
        {"--isolation", "name", "isolation of the transactions of ram", &settings.engine.isolation},
        {"--engine-transport", "name", "transport of nvme", &settings.engine.transport},
        {"--directory", "path", "database directory; the graph stays in memory without it", &settings.directory},
        {"--vertices", "count", "vertices of the generated graph", &settings.vertices},
        {"--communities", "count", "communities planted in the generated graph", &settings.communities},
        {"--inner-degree", "count", "edges each generated vertex draws inside its community", &settings.inner_degree},
        {"--outer-degree", "count", "edges each generated vertex draws across communities", &settings.outer_degree},
        {"--max-edges", "count", "edges kept from the dataset", &settings.max_edges},
        {"--batch-size-upsert", "count", "edges per upsert batch", &settings.batch_size_upsert},
        {"--min-modularity-growth-per-billion", "count", "least modularity growth that justifies a level",
         &settings.min_modularity_growth_per_billion},
        {"--max-levels", "count", "levels either run coarsens through", &settings.max_levels},
    }};

    read_settings(argc, argv, settings, flags);
    auto maybe_executor = executor_t::make(1, caller_exclusivity_t::caller_inclusive_k);
    if (!maybe_executor) {
        fmt::println(stderr, "Failed to pin the calling thread");
        return 1;
    }
    executor_t& executor = *maybe_executor;

    st::vector<input_edge_t> edges;
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

    st::vector<edge_t> staged;
    st::vector<weighted_entry_t> entries;
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

    ustore_database_t handle = nullptr;
    ustore_outcome_t outcome {};
    settings.engine.directory = settings.directory.data();
    open_engine(settings.engine, &handle, &outcome);
    if (outcome.status != ustore_status_success_k) {
        fmt::println(stderr, "Failed to open the database: {}", outcome.message ? outcome.message : "unknown");
        return 1;
    }
    database_t database {handle};
    if (status_t const cleared = database.clear(ustore_drop_collection_k); cleared.failed()) {
        fmt::println(stderr, "Failed to clear the database: {}", cleared.message ? cleared.message : "unknown");
        return 1;
    }
    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    if (graph_status.failed()) {
        fmt::println(stderr, "Failed to create the graph: {}", graph_status.message ? graph_status.message : "unknown");
        return 1;
    }

    run_result_t memory_result;
    run_result_t stored_result;
    if (settings.selects("graph_upsert") || settings.selects("louvain_over_ustore")) {
        std::size_t const batch_size = settings.batch_size_upsert.value;
        std::span<edge_t const> const batches {staged.begin(), staged.end()};
        row_t const upserted = run_pass(
            executor, "graph_upsert", divide_round_up(batches.size(), batch_size),
            [&](loop_t& loop, std::size_t) noexcept {
                for (std::size_t call : loop) {
                    std::span<edge_t const> const batch = batches.subspan(
                        call * batch_size, std::min(batch_size, batches.size() - call * batch_size));
                    if (graph.write_edges(edges_view_t {batch.data(), batch.data() + batch.size()}).failed())
                        return loop.skip("failed to upsert a batch of edges");
                    loop.count(batch.size(), batch.size() * sizeof(edge_t));
                }
            });
        if (settings.selects("graph_upsert")) print(upserted);
        if (upserted.skipped) return 1;
    }
    if (settings.selects("louvain_in_memory"))
        print(run_pass(executor, "louvain_in_memory", 1, [&](loop_t& loop, std::size_t) noexcept {
            for ([[maybe_unused]] std::size_t call : loop) louvain_call(loop, memory_graph, settings, memory_result);
        }));
    if (settings.selects("louvain_over_ustore"))
        print(run_pass(executor, "louvain_over_ustore", 1, [&](loop_t& loop, std::size_t) noexcept {
            for ([[maybe_unused]] std::size_t call : loop) {
                auto [stored, make_stored_graph_status] = stored_graph_t::make(graph);
                if (make_stored_graph_status.failed()) return loop.skip("failed to scan the graph collection");
                louvain_call(loop, stored, settings, stored_result);
            }
        }));

    if (memory_result.levels_count)
        fmt::println("In memory: modularity {:.6f}, {} communities, {} levels", memory_result.modularity,
                     memory_result.communities_count, memory_result.levels_count);
    if (stored_result.levels_count)
        fmt::println("Over UStore: modularity {:.6f}, {} communities, {} levels", stored_result.modularity,
                     stored_result.communities_count, stored_result.levels_count);
    return 0;
}
