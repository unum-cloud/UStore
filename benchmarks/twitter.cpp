#include <fcntl.h>    // `open` files
#include <sys/stat.h> // `stat` to obtain file metadata
#include <sys/mman.h> // `mmap` to read datasets faster

#include <cstring>     // `std::memchr`
#include <algorithm>   // `std::search`
#include <filesystem>  // Listing directories is too much pain in C
#include <string_view> //
#include <vector>      //
#include <thread>      //
#include <random>      // `std::random_device` for each thread
#include <fstream>     // `std::ifstream`, `std::istreambuf_iterator`

#include <fmt/printf.h> // `fmt::sprintf`
#include <benchmark/benchmark.h>
#include <simdjson.h>

#include <argparse/argparse.hpp>

#include <ustore/ustore.hpp>
#include <ustore/cpp/ranges.hpp> // `sort_and_deduplicate`

#include "mixed.hpp"

namespace bm = benchmark;
using namespace unum::ustore::bench;
using namespace unum::ustore;
using uniform_idx_t = std::uniform_int_distribution<std::size_t>;

constexpr std::size_t id_str_max_length_k = 24;
constexpr std::size_t copies_per_tweet_k = 1;

constexpr std::size_t primes_k[10] = {
    1ul,
    10373281427301508897ull,
    10008795057561858269ull,
    7948791514834664467ull,
    3838954299457218127ull,
    3120785516547182557ull,
    4393300032555048899ull,
    7004376283452977123ull,
    9223372036854777211ull,
    14223002033854726039ull,
};
static_assert(sizeof(primes_k) >= copies_per_tweet_k, "We don't have enough primes to generate that many copies");

using id_str_t = char[id_str_max_length_k];

static inline std::uint64_t hash_mix_ror64(std::uint64_t v, int r) {
    return (v >> r) | (v << (64 - r));
}

static inline std::uint64_t hash_mix_rrxmrrxmsx_0(std::uint64_t v) {
    v ^= hash_mix_ror64(v, 25) ^ hash_mix_ror64(v, 50);
    v *= 0xA24BAED4963EE407UL;
    v ^= hash_mix_ror64(v, 24) ^ hash_mix_ror64(v, 49);
    v *= 0x9FB21C651E98DF25UL;
    return v ^ v >> 28;
}

static inline ustore_key_t hash(id_str_t const& id_str) {
    auto u64s = reinterpret_cast<std::uint64_t const*>(id_str);
    auto mix = hash_mix_rrxmrrxmsx_0(u64s[0]) ^ hash_mix_rrxmrrxmsx_0(u64s[1]) ^ hash_mix_rrxmrrxmsx_0(u64s[2]);
    return mix;
}

struct settings_t {
    bool generate_dataset;
    std::size_t threads_count;
    std::size_t max_tweets_count;
    std::size_t max_input_files;
    std::size_t connectivity_factor;
    std::size_t min_seconds;
    std::size_t small_batch_size;
    std::size_t mid_batch_size;
    std::size_t big_batch_size;
};

static std::string dataset_directory = "~/Datasets/Twitter/";
static std::vector<std::string> twitter_content;
static std::vector<std::size_t> source_sizes;
static std::vector<std::string> source_files;
static std::vector<std::string_view> mapped_contents;
static std::vector<std::vector<doc_w_path_t>> dataset_paths;
static std::vector<std::vector<doc_w_key_t>> dataset_docs;
static std::vector<std::vector<edge_t>> dataset_graph;

static database_t db;
static ustore_collection_t collection_docs_k = ustore_collection_main_k;
static ustore_collection_t collection_graph_k = ustore_collection_main_k;
static ustore_collection_t collection_paths_k = ustore_collection_main_k;

void parse_args(int argc, char* argv[], settings_t& settings) {
    argparse::ArgumentParser program(argv[0]);
    program.add_argument("-gd", "--gen_dataset").default_value(true).help("Generate dataset");
    program.add_argument("-t", "--threads")
        .default_value(std::to_string((std::thread::hardware_concurrency() / 2)))
        .help("Threads count");
    program.add_argument("-tw", "--max_tweets_count").default_value("1'000'000").help("Maximum tweets count");
    program.add_argument("-i", "--max_input_files").default_value("1000").help("Maximum input files count");
    program.add_argument("-c", "--con_factor").default_value("4").help("Connectivity factor");
    program.add_argument("-n", "--min_seconds").default_value("10").help("Minimal seconds");
    program.add_argument("-s", "--small_batch_size").default_value("32").help("Small batch size");
    program.add_argument("-m", "--mid_batch_size").default_value("64").help("Middle batch size");
    program.add_argument("-b", "--big_batch_size").default_value("128").help("Big batch size");

    program.parse_known_args(argc, argv);

    settings.generate_dataset = program.get<bool>("gen_dataset");
    settings.threads_count = std::stoi(program.get("threads"));
    settings.max_tweets_count = std::stoi(program.get("max_tweets_count"));
    settings.max_input_files = std::stoi(program.get("max_input_files"));
    settings.connectivity_factor = std::stoi(program.get("con_factor"));
    settings.min_seconds = std::stoi(program.get("min_seconds"));
    settings.small_batch_size = std::stoi(program.get("small_batch_size"));
    settings.mid_batch_size = std::stoi(program.get("mid_batch_size"));
    settings.big_batch_size = std::stoi(program.get("big_batch_size"));

    if (settings.threads_count == 0) {
        fmt::print("-threads: Zero threads count specified\n");
        exit(1);
    }
}

simdjson::ondemand::document& rewound(simdjson::ondemand::document& doc) noexcept {
    doc.rewind();
    return doc;
}

simdjson::ondemand::object& rewound(simdjson::ondemand::object& doc) noexcept {
    doc.reset();
    return doc;
}

using twitter_id_t = std::int64_t;

std::string new_tweet( //
    twitter_id_t tweet_id,
    twitter_id_t user_id,
    std::size_t tweet_length,
    std::string_view tweet_template,
    std::vector<twitter_id_t> const& mentioned_user_ids) {

    std::string mentioned_users = "";
    for (twitter_id_t mentioned_user_id : mentioned_user_ids)
        mentioned_users +=
            fmt::sprintf(R"({"screen_name":"","name":"","id":%1$d,"id_str":"%1$d","indices":[]},)", mentioned_user_id);

    // Remove the last comma if it exists.
    if (!mentioned_users.empty())
        mentioned_users.resize(mentioned_users.size() - 1);

    // tweet_template + std::string(simdjson::SIMDJSON_PADDING, ' ')
    std::string tweet_json =
        fmt::sprintf(tweet_template, tweet_id, std::string(tweet_length, '_'), user_id, mentioned_users);
    return tweet_json;
}

void generate_twitter(std::size_t connectivity_factor) {
    std::random_device random_device;
    std::mt19937 random_generator(random_device());
    std::uniform_int_distribution<twitter_id_t> positives(0);
    std::uniform_int_distribution<std::size_t> text_lengths(1, 280);

    std::vector<twitter_id_t> user_ids;
    user_ids.reserve(twitter_content.size());

    std::ifstream ifs("./assets/tweet_template.json");
    std::string tweet_template((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    for (std::size_t i = 0; i < twitter_content.size(); ++i) {
        std::size_t text_length = text_lengths(random_generator);
        twitter_id_t tweet_id = positives(random_generator);
        twitter_id_t user_id = positives(random_generator);

        std::size_t relations_count =
            std::min(positives(random_generator) % (2 * connectivity_factor + 1), user_ids.size());
        std::vector<twitter_id_t> mentioned_user_ids(relations_count);
        for (std::size_t j = 0; j != relations_count; ++j) {
            auto it = user_ids.cbegin();
            twitter_id_t user_number = positives(random_generator) % user_ids.size();
            std::advance(it, user_number);
            mentioned_user_ids[j] = *it;
        }

        twitter_content[i] = new_tweet(tweet_id, user_id, text_length, tweet_template, mentioned_user_ids);
        user_ids.push_back(user_id);
    }
}

void mmapping_ndjson() {
    std::printf("Will memory-map the files...\n");
    for (std::size_t path_idx = 0; path_idx != source_files.size(); ++path_idx) {
        auto const& path = source_files[path_idx];
        auto handle = open(path.c_str(), O_RDONLY);
        if (handle == -1)
            throw std::runtime_error("Can't open file");

        auto size = source_sizes[path_idx];
        auto begin = mmap(NULL, size, PROT_READ, MAP_PRIVATE, handle, 0);
        mapped_contents[path_idx] = std::string_view(reinterpret_cast<char const*>(begin), size);
        madvise(begin, size, MADV_SEQUENTIAL);
    }
}

static void index_tweet_doc( //
    auto tweet_doc,
    std::vector<doc_w_path_t>& docs_w_paths,
    std::vector<doc_w_key_t>& docs_w_ids,
    std::vector<edge_t>& edges) {

    auto maybe_tweet = tweet_doc.get_object();
    if (maybe_tweet.error() != simdjson::SUCCESS)
        return;
    simdjson::ondemand::object tweet = maybe_tweet.value();

    auto maybe_user = rewound(tweet).find_field("user").get_object();
    if (maybe_user.error() != simdjson::SUCCESS)
        return;
    simdjson::ondemand::object user = maybe_user.value();
    ustore_key_t id = rewound(tweet)["id"];

    ustore_key_t user_id = rewound(user)["id"];
    std::string_view body = rewound(tweet).raw_json();
    std::string_view user_body = rewound(user).raw_json();
    std::string_view id_str = rewound(tweet)["id_str"].raw_json_token();
    std::string_view user_screen_name = rewound(user)["screen_name"].raw_json_token();

    ustore_key_t re_id;
    ustore_key_t re_user_id;
    auto maybe_retweet = rewound(tweet)["retweeted_status"];
    if (maybe_retweet.error() == simdjson::SUCCESS) {
        auto retweet = maybe_retweet.get_object().value();
        re_id = rewound(retweet)["id"];
        re_user_id = rewound(retweet)["user"]["id"];
    }

    // Docs
    docs_w_ids.push_back(doc_w_key_t {id, body});
    docs_w_ids.push_back(doc_w_key_t {user_id, body});

    // Paths
    if (!id_str.empty())
        docs_w_paths.push_back(doc_w_path_t {id_str, body});
    if (!user_screen_name.empty())
        docs_w_paths.push_back(doc_w_path_t {user_screen_name, user_body});

    // Graph
    edges.push_back(edge_t {id, user_id});
    if (maybe_retweet.error() == simdjson::SUCCESS) {
        edges.push_back(edge_t {id, re_id});
        edges.push_back(edge_t {user_id, re_user_id, re_id});
    }

    auto maybe_mentions = rewound(tweet).find_field("entities").find_field("user_mentions");
    if (maybe_mentions.error() == simdjson::SUCCESS && maybe_mentions.type() == simdjson::ondemand::json_type::array) {
        auto mentions = maybe_mentions.get_array().value();
        for (auto mention : mentions) {
            auto mentioned_id = mention["id"];
            if (mentioned_id.type() != simdjson::ondemand::json_type::number)
                return;
            edges.push_back(edge_t {user_id, mentioned_id, id});
        }
    }
}

static void index_tweet( //
    std::string_view tweet,
    std::vector<doc_w_path_t>& docs_w_paths,
    std::vector<doc_w_key_t>& docs_w_ids,
    std::vector<edge_t>& edges) {

    simdjson::ondemand::parser parser;
    index_tweet_doc(parser.iterate(tweet.data(), tweet.size() - simdjson::SIMDJSON_PADDING, tweet.size()),
                    docs_w_paths,
                    docs_w_ids,
                    edges);
}

static void index_tweets( //
    std::pair<std::size_t, std::size_t> range,
    std::vector<doc_w_path_t>& docs_w_paths,
    std::vector<doc_w_key_t>& docs_w_ids,
    std::vector<edge_t>& edges) {

    // For joined jsons (ndjson) strings
    // https://github.com/simdjson/simdjson/blob/master/doc/basics.md#newline-delimited-json-ndjson-and-json-lines
    for (std::size_t idx = range.first; idx != range.second; ++idx)
        index_tweet(twitter_content[idx], docs_w_paths, docs_w_ids, edges);
}

static void index_file( //
    std::string_view mapped_contents,
    std::vector<doc_w_path_t>& docs_w_paths,
    std::vector<doc_w_key_t>& docs_w_ids,
    std::vector<edge_t>& edges) {

    // https://github.com/simdjson/simdjson/blob/master/doc/basics.md#newline-delimited-json-ndjson-and-json-lines
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document_stream docs =
        parser.iterate_many(mapped_contents.data(), mapped_contents.size(), 1000000ul);
    for (auto tweet_doc : docs)
        index_tweet_doc(tweet_doc, docs_w_paths, docs_w_ids, edges);
}

/**
 * @brief Builds up a chaotic collection of documents,
 * multiplying the number of tweets by `copies_per_tweet_k`.
 */
void construct_docs(bm::State& state) {
    return docs_upsert(state,
                       db,
                       collection_docs_k,
                       pass_through_iterator(dataset_docs),
                       pass_through_size(dataset_docs));
}

/**
 * @brief Constructs a graph between Twitter entities:
 * - Tweets and their Authors.
 * - Tweets and their Retweets.
 * - Authors and Retweeters labeled by Retweet IDs.
 */
static void construct_graph(bm::State& state) {
    return edges_upsert(state,
                        db,
                        collection_graph_k,
                        pass_through_iterator(dataset_graph),
                        pass_through_size(dataset_graph));
}

/**
 * @brief Maps string IDs to matching Twitter entities.
 */
static void construct_paths(bm::State& state) {
    return paths_upsert(state,
                        db,
                        collection_docs_k,
                        pass_through_iterator(dataset_paths),
                        pass_through_size(dataset_paths));
}

#pragma region - Analytics

template <typename callback_at>
void sample_tweet_id_batches(bm::State& state, callback_at callback) {

    std::random_device rd;
    std::mt19937 gen(rd());
    uniform_idx_t choose_part(0, dataset_docs.size() - 1);
    uniform_idx_t choose_hash(0, copies_per_tweet_k - 1);

    auto const batch_size = static_cast<ustore_size_t>(state.range(0));
    std::vector<ustore_key_t> batch_keys(batch_size);

    std::size_t iterations = 0;
    std::size_t successes = 0;
    for (auto _ : state) {
        for (std::size_t idx = 0; idx != batch_size; ++idx) {
            std::size_t const part_idx = choose_part(gen);
            auto const& tweets = dataset_docs[part_idx];
            uniform_idx_t choose_tweet(0, tweets.size() - 1);
            std::size_t const tweet_idx = choose_tweet(gen);
            auto const& tweet = tweets[tweet_idx];

            ustore_key_t tweet_key = tweet.first;
            if constexpr (copies_per_tweet_k != 1) {
                std::size_t const hash_idx = choose_hash(gen);
                tweet_key *= primes_k[hash_idx];
            }

            batch_keys[idx] = tweet_key;
        }
        successes += callback(batch_keys.data(), batch_size);
        iterations++;
    }

    state.counters["items/s"] = bm::Counter(iterations * batch_size, bm::Counter::kIsRate);
    state.counters["batches/s"] = bm::Counter(iterations, bm::Counter::kIsRate);
    state.counters["fails,%"] = bm::Counter((iterations - successes) * 100.0, bm::Counter::kAvgThreads);
}

static void docs_sample_blobs(bm::State& state) {

    arena_t arena(db);

    std::size_t received_bytes = 0;
    sample_tweet_id_batches(state, [&](ustore_key_t const* ids_tweets, ustore_size_t count) {
        ustore_length_t* offsets = nullptr;
        ustore_byte_t* values = nullptr;

        status_t status;
        ustore_read_t read {};
        read.db = db;
        read.error = status.member_ptr();
        read.arena = arena.member_ptr();
        read.tasks_count = count;
        read.collections = &collection_docs_k;
        read.keys = ids_tweets;
        read.keys_stride = sizeof(ustore_key_t);
        read.offsets = &offsets;
        read.values = &values;

        ustore_read(&read);
        if (!status)
            return false;

        received_bytes += offsets[count];
        return true;
    });

    state.counters["bytes/s"] = bm::Counter(received_bytes, bm::Counter::kIsRate);
    state.counters["bytes/it"] = bm::Counter(received_bytes, bm::Counter::kAvgIterations);
}

static void docs_sample_objects(bm::State& state) {

    // We want to trigger parsing and serialization
    arena_t arena(db);

    std::size_t received_bytes = 0;
    sample_tweet_id_batches(state, [&](ustore_key_t const* ids_tweets, ustore_size_t count) {
        ustore_length_t* offsets = nullptr;
        ustore_byte_t* values = nullptr;

        status_t status;
        ustore_docs_read_t docs_read {};
        docs_read.db = db;
        docs_read.error = status.member_ptr();
        docs_read.arena = arena.member_ptr();
        docs_read.type = ustore_doc_field_json_k;
        docs_read.tasks_count = count;
        docs_read.collections = &collection_docs_k;
        docs_read.keys = ids_tweets;
        docs_read.keys_stride = sizeof(ustore_key_t);
        docs_read.offsets = &offsets;
        docs_read.values = &values;

        ustore_docs_read(&docs_read);
        if (!status)
            return false;

        received_bytes += offsets[count];
        return true;
    });
    state.counters["bytes/s"] = bm::Counter(received_bytes, bm::Counter::kIsRate);
    state.counters["bytes/it"] = bm::Counter(received_bytes, bm::Counter::kAvgIterations);
}

static void docs_sample_field(bm::State& state) {

    arena_t arena(db);
    ustore_str_view_t field = "text";

    std::size_t received_bytes = 0;
    sample_tweet_id_batches(state, [&](ustore_key_t const* ids_tweets, ustore_size_t count) {
        ustore_length_t* offsets = nullptr;
        ustore_byte_t* values = nullptr;

        status_t status;
        ustore_docs_read_t docs_read {};
        docs_read.db = db;
        docs_read.error = status.member_ptr();
        docs_read.arena = arena.member_ptr();
        docs_read.type = ustore_doc_field_str_k;
        docs_read.tasks_count = count;
        docs_read.collections = &collection_docs_k;
        docs_read.keys = ids_tweets;
        docs_read.keys_stride = sizeof(ustore_key_t);
        docs_read.fields = &field;
        docs_read.offsets = &offsets;
        docs_read.values = &values;

        ustore_docs_read(&docs_read);
        if (!status)
            return false;

        received_bytes += offsets[count];
        return true;
    });
    state.counters["bytes/s"] = bm::Counter(received_bytes, bm::Counter::kIsRate);
    state.counters["bytes/it"] = bm::Counter(received_bytes, bm::Counter::kAvgIterations);
}

static void docs_sample_table(bm::State& state) {
    arena_t arena(db);

    constexpr ustore_size_t fields_k = 4;
    ustore_str_view_t names[fields_k] {"timestamp_ms", "reply_count", "retweet_count", "favorite_count"};
    ustore_doc_field_type_t types[fields_k] {
        ustore_doc_field_str_k,
        ustore_doc_field_u32_k,
        ustore_doc_field_u32_k,
        ustore_doc_field_u32_k,
    };

    std::size_t received_bytes = 0;
    sample_tweet_id_batches(state, [&](ustore_key_t const* ids_tweets, ustore_size_t count) {
        ustore_octet_t** validities = nullptr;
        ustore_byte_t** scalars = nullptr;
        ustore_length_t** offsets = nullptr;
        ustore_length_t** lengths = nullptr;
        ustore_byte_t* strings = nullptr;

        status_t status;
        ustore_docs_gather_t docs_gather {};
        docs_gather.db = db;
        docs_gather.error = status.member_ptr();
        docs_gather.arena = arena.member_ptr();
        docs_gather.docs_count = count;
        docs_gather.fields_count = fields_k;
        docs_gather.collections = &collection_docs_k;
        docs_gather.keys = ids_tweets;
        docs_gather.keys_stride = sizeof(ustore_key_t);
        docs_gather.fields = names;
        docs_gather.fields_stride = sizeof(ustore_str_view_t);
        docs_gather.types = types;
        docs_gather.types_stride = sizeof(ustore_doc_field_type_t);
        docs_gather.columns_validities = &validities;
        docs_gather.columns_scalars = &scalars;
        docs_gather.columns_offsets = &offsets;
        docs_gather.columns_lengths = &lengths;
        docs_gather.joined_strings = &strings;

        ustore_docs_gather(&docs_gather);
        if (!status)
            return false;

        // One column is just strings
        received_bytes += std::transform_reduce(&lengths[0][0],
                                                &lengths[0][count],
                                                0ul,
                                                std::plus<std::size_t> {},
                                                [](ustore_length_t length) -> std::size_t { //
                                                    return length == ustore_length_missing_k ? 0u : length;
                                                });
        // Others are scalars
        received_bytes += (fields_k - 1) * sizeof(std::uint32_t) * count;
        return true;
    });
    state.counters["bytes/s"] = bm::Counter(received_bytes, bm::Counter::kIsRate);
    state.counters["bytes/it"] = bm::Counter(received_bytes, bm::Counter::kAvgIterations);
}

/**
 * @brief Most Tweets in the graph have just one connection - to their Author.
 * That is why we make a two-hop benchmark. For every Tweet vertex we gather their
 * Authors and all the Retweets, as well as the connections of those Authors and
 * Retweets.
 */
static void graph_traverse_two_hops(bm::State& state) {
    arena_t arena(db);
    std::plus plus;

    std::size_t received_bytes = 0;
    std::size_t received_edges = 0;
    sample_tweet_id_batches(state, [&](ustore_key_t const* ids_tweets, ustore_size_t count) {
        // First hop
        ustore_vertex_role_t const role = ustore_vertex_role_any_k;
        ustore_vertex_degree_t* degrees = nullptr;
        ustore_key_t* ids_in_edges = nullptr;

        status_t status;
        ustore_graph_find_edges_t graph_find_edges_first {};
        graph_find_edges_first.db = db;
        graph_find_edges_first.error = status.member_ptr();
        graph_find_edges_first.arena = arena.member_ptr();
        graph_find_edges_first.tasks_count = count;
        graph_find_edges_first.collections = &collection_graph_k;
        graph_find_edges_first.vertices = ids_tweets;
        graph_find_edges_first.vertices_stride = sizeof(ustore_key_t);
        graph_find_edges_first.roles = &role;
        graph_find_edges_first.degrees_per_vertex = &degrees;
        graph_find_edges_first.edges_per_vertex = &ids_in_edges;

        ustore_graph_find_edges(&graph_find_edges_first);
        if (!status)
            return false;

        // Now keep only the unique objects
        auto total_edges = std::transform_reduce(degrees, degrees + count, 0ul, plus, [](ustore_vertex_degree_t d) {
            return d != ustore_vertex_degree_missing_k ? d : 0;
        });
        // Compact ~ Remove edge IDs from three-tuples
        for (std::size_t i = 0; i != total_edges; ++i)
            ids_in_edges[i * 2] = ids_in_edges[i * 3], ids_in_edges[i * 2 + 1] = ids_in_edges[i * 3 + 1];
        auto unique_ids = sort_and_deduplicate(ids_in_edges, ids_in_edges + total_edges * 2);

        ustore_graph_find_edges_t graph_find_edges_second {};
        graph_find_edges_second.db = db;
        graph_find_edges_second.error = status.member_ptr();
        graph_find_edges_second.arena = arena.member_ptr();
        graph_find_edges_second.options = ustore_option_dont_discard_memory_k;
        graph_find_edges_second.tasks_count = unique_ids;
        graph_find_edges_second.collections = &collection_graph_k;
        graph_find_edges_second.vertices = ids_in_edges;
        graph_find_edges_second.vertices_stride = sizeof(ustore_key_t);
        graph_find_edges_second.roles = &role;
        graph_find_edges_second.degrees_per_vertex = &degrees;
        graph_find_edges_second.edges_per_vertex = &ids_in_edges;

        // Second hop
        ustore_graph_find_edges(&graph_find_edges_second);
        if (!status)
            return false;

        total_edges += std::transform_reduce(degrees, degrees + unique_ids, 0ul, plus, [](ustore_vertex_degree_t d) {
            return d != ustore_vertex_degree_missing_k ? d : 0;
        });
        received_bytes += total_edges * 3 * sizeof(ustore_key_t);
        received_edges += total_edges;
        return true;
    });
    state.counters["bytes/s"] = bm::Counter(received_bytes, bm::Counter::kIsRate);
    state.counters["bytes/it"] = bm::Counter(received_bytes, bm::Counter::kAvgIterations);
    state.counters["edges/s"] = bm::Counter(received_edges, bm::Counter::kIsRate);
}

int main(int argc, char** argv) {
    bm::Initialize(&argc, argv);

    // We divide by two, as most modern CPUs have
    // hyper-threading with two threads per core.
    settings_t settings;
    parse_args(argc, argv, settings);

#if defined(USTORE_DEBUG)
    settings.max_input_files = 1;
    settings.max_tweets_count = 100'000;
    settings.threads_count = 1;
#endif

    if (!settings.generate_dataset) {
        // 1. Find the dataset parts
        std::printf("Will search for .ndjson files...\n");
        auto dataset_path = dataset_directory;
        auto home_path = std::getenv("HOME");
        if (dataset_path.front() == '~')
            dataset_path = std::filesystem::path(home_path) / dataset_path.substr(2);
        auto opts = std::filesystem::directory_options::follow_directory_symlink;
        for (auto const& dir_entry : std::filesystem::directory_iterator(dataset_path, opts)) {
            if (dir_entry.path().extension() != ".ndjson")
                continue;

            source_files.push_back(dir_entry.path());
            source_sizes.push_back(dir_entry.file_size());
        }
        std::printf("- found %i files\n", static_cast<int>(source_files.size()));
        source_files.resize(std::min(settings.max_input_files, source_files.size()));
        std::printf("- kept only %i files\n", static_cast<int>(source_files.size()));
        dataset_paths.resize(source_files.size());
        dataset_docs.resize(source_files.size());
        dataset_graph.resize(source_files.size());
        mapped_contents.resize(source_files.size());

        // 2. Memory-map the contents
        // As we are closing the process after the benchmarks, we can avoid unmap them.
        mmapping_ndjson();

        // 3. Index the dataset
        std::printf("Will index the files...\n");
        if (settings.threads_count == 1) {
            for (std::size_t path_idx = 0; path_idx != source_files.size(); ++path_idx)
                index_file(mapped_contents[path_idx],
                           dataset_paths[path_idx],
                           dataset_docs[path_idx],
                           dataset_graph[path_idx]);
        }
        else {
            std::vector<std::thread> parsing_threads;
            for (std::size_t path_idx = 0; path_idx != source_files.size(); ++path_idx)
                parsing_threads.push_back(std::thread( //
                    &index_file,
                    mapped_contents[path_idx],
                    std::ref(dataset_paths[path_idx]),
                    std::ref(dataset_docs[path_idx]),
                    std::ref(dataset_graph[path_idx])));
            for (auto& thread : parsing_threads)
                thread.join();
        }
    }
    else {
        // 1. Prepare the dataset parts
        std::printf("Will prepare dataset parts...\n");
        std::size_t parts_cnt = settings.threads_count;
        std::size_t part_size = settings.max_tweets_count / settings.threads_count;
        std::size_t twitters_count = parts_cnt * part_size;
        twitter_content.resize(twitters_count);

        dataset_paths.resize(parts_cnt);
        dataset_docs.resize(parts_cnt);
        dataset_graph.resize(parts_cnt);
        for (std::size_t idx = 0; idx != parts_cnt; ++idx) {
            dataset_paths[idx].reserve(part_size);
            dataset_docs[idx].reserve(part_size);
            dataset_graph[idx].reserve(part_size * settings.connectivity_factor);
        }

        // 2. Generate the contents
        std::printf("Will generate tweeter content...\n");
        generate_twitter(settings.connectivity_factor);

        // 3. Index the dataset
        std::printf("Will index the generated content...\n");
        if (settings.threads_count == 1) {
            for (std::size_t idx = 0; idx != twitters_count; ++idx)
                index_tweet(twitter_content[idx], dataset_paths[0], dataset_docs[0], dataset_graph[0]);
        }
        else {
            std::vector<std::thread> parsing_threads;
            std::size_t offset = 0;
            for (std::size_t idx = 0; idx != settings.threads_count; ++idx) {
                parsing_threads.push_back(std::thread( //
                    &index_tweets,
                    std::pair(offset, offset + part_size),
                    std::ref(dataset_paths[idx]),
                    std::ref(dataset_docs[idx]),
                    std::ref(dataset_graph[idx])));
                offset += part_size;
            }
            for (auto& thread : parsing_threads)
                thread.join();
        }
    }

    std::printf("- indexed %zu docs\n", pass_through_size(dataset_docs));
    std::printf("- indexed %zu relations\n", pass_through_size(dataset_graph));
    std::printf("- indexed %zu paths\n", pass_through_size(dataset_paths));

// 4. Run the actual benchmarks
#if defined(USTORE_ENGINE_IS_LEVELDB)
    db.open(R"({"version": "1.0", "directory": "/mnt/md0/Twitter/LevelDB"})").throw_unhandled();
#elif defined(USTORE_ENGINE_IS_ROCKSDB)
    db.open(R"({"version": "1.0", "directory": "/mnt/md0/Twitter/RocksDB"})").throw_unhandled();
#elif defined(USTORE_ENGINE_IS_UDISK)
    db.open(R"({"version": "1.0", "directory": "/mnt/md0/Twitter/UnumDB"})").throw_unhandled();
#else
    db.open().throw_unhandled();
#endif

    bool can_build_graph = false;
    bool can_build_paths = false;
    if (ustore_supports_named_collections_k) {
        status_t status;
        ustore_collection_create_t collection_init {};
        collection_init.db = db;
        collection_init.error = status.member_ptr();
        collection_init.name = "twitter.docs";
        collection_init.config = "";
        collection_init.id = &collection_docs_k;

        ustore_collection_create(&collection_init);
        status.throw_unhandled();

        collection_init.name = "twitter.graph";
        collection_init.id = &collection_graph_k;
        ustore_collection_create(&collection_init);
        status.throw_unhandled();
        can_build_graph = true;

        collection_init.name = "twitter.nicks";
        collection_init.id = &collection_paths_k;
        ustore_collection_create(&collection_init);
        status.throw_unhandled();
        can_build_paths = true;
    }

    std::printf("Will benchmark...\n");
    bm::RegisterBenchmark("construct_docs", &construct_docs) //
        ->Iterations(pass_through_size(dataset_docs) / (settings.threads_count * settings.big_batch_size))
        ->UseRealTime()
        ->Threads(settings.threads_count)
        ->Arg(settings.big_batch_size);

    if (can_build_graph)
        bm::RegisterBenchmark("construct_graph", &construct_graph) //
            ->Iterations(pass_through_size(dataset_graph) / (settings.threads_count * settings.big_batch_size))
            ->UseRealTime()
            ->Threads(settings.threads_count)
            ->Arg(settings.big_batch_size);

    if (can_build_paths)
        bm::RegisterBenchmark("construct_paths", &construct_paths) //
            ->Iterations(pass_through_size(dataset_paths) / (settings.threads_count * settings.big_batch_size))
            ->UseRealTime()
            ->Threads(settings.threads_count)
            ->Arg(settings.big_batch_size);

    if (ustore_doc_field_default_k != ustore_doc_field_json_k)
        bm::RegisterBenchmark("docs_sample_blobs", &docs_sample_blobs) //
            ->MinTime(settings.min_seconds)
            ->UseRealTime()
            ->Threads(settings.threads_count)
            ->Arg(settings.small_batch_size)
            ->Arg(settings.mid_batch_size)
            ->Arg(settings.big_batch_size);

    bm::RegisterBenchmark("docs_sample_objects", &docs_sample_objects) //
        ->MinTime(settings.min_seconds)
        ->UseRealTime()
        ->Threads(settings.threads_count)
        ->Arg(settings.small_batch_size)
        ->Arg(settings.mid_batch_size)
        ->Arg(settings.big_batch_size);

    bm::RegisterBenchmark("docs_sample_field", &docs_sample_field) //
        ->MinTime(settings.min_seconds)
        ->UseRealTime()
        ->Threads(settings.threads_count)
        ->Arg(settings.small_batch_size)
        ->Arg(settings.mid_batch_size)
        ->Arg(settings.big_batch_size);

    bm::RegisterBenchmark("docs_sample_table", &docs_sample_table) //
        ->MinTime(settings.min_seconds)
        ->UseRealTime()
        ->Threads(settings.threads_count)
        ->Arg(settings.small_batch_size)
        ->Arg(settings.mid_batch_size)
        ->Arg(settings.big_batch_size);

    if (can_build_graph)
        bm::RegisterBenchmark("graph_traverse_two_hops", &graph_traverse_two_hops) //
            ->MinTime(settings.min_seconds)
            ->Threads(settings.threads_count)
            ->Arg(settings.small_batch_size)
            ->Arg(settings.mid_batch_size)
            ->Arg(settings.big_batch_size);

    bm::RunSpecifiedBenchmarks();
    bm::Shutdown();

    // To avoid sanitizer complaints, we should unmap the files:
    for (auto mapped_content : mapped_contents)
        munmap((void*)mapped_content.data(), mapped_content.size());

    // Clear DB after benchmark
    db.clear().throw_unhandled();

    return 0;
}
