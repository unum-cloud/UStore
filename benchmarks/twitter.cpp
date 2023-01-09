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

#include <benchmark/benchmark.h>
#include <simdjson.h>

#include <ukv/ukv.hpp>
#include <ukv/cpp/ranges.hpp> // `sort_and_deduplicate`

#include "mixed.hpp"

namespace bm = benchmark;
using namespace unum::ukv::bench;
using namespace unum::ukv;
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

static inline ukv_key_t hash(id_str_t const& id_str) {
    auto u64s = reinterpret_cast<std::uint64_t const*>(id_str);
    auto mix = hash_mix_rrxmrrxmsx_0(u64s[0]) ^ hash_mix_rrxmrrxmsx_0(u64s[1]) ^ hash_mix_rrxmrrxmsx_0(u64s[2]);
    return mix;
}

static std::string dataset_directory = "~/Datasets/Twitter/";
static std::vector<std::string> source_files;
static std::vector<std::size_t> source_sizes;
static std::vector<std::string_view> mapped_contents;
static std::vector<std::vector<doc_w_path_t>> dataset_paths;
static std::vector<std::vector<doc_w_key_t>> dataset_docs;
static std::vector<std::vector<edge_t>> dataset_graph;

static database_t db;
static ukv_collection_t collection_docs_k = ukv_collection_main_k;
static ukv_collection_t collection_graph_k = ukv_collection_main_k;
static ukv_collection_t collection_paths_k = ukv_collection_main_k;

simdjson::ondemand::document& rewound(simdjson::ondemand::document& doc) noexcept {
    doc.rewind();
    return doc;
}

simdjson::ondemand::object& rewound(simdjson::ondemand::object& doc) noexcept {
    doc.reset();
    return doc;
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
    for (auto tweet_doc : docs) {
        auto maybe_tweet = tweet_doc.get_object();
        if (maybe_tweet.error() != simdjson::SUCCESS)
            continue;
        simdjson::ondemand::object tweet = maybe_tweet.value();

        auto maybe_user = rewound(tweet).find_field("user").get_object();
        if (maybe_user.error() != simdjson::SUCCESS)
            continue;
        simdjson::ondemand::object user = maybe_user.value();
        ukv_key_t id = rewound(tweet)["id"];

        ukv_key_t user_id = rewound(user)["id"];
        std::string_view body = rewound(tweet).raw_json();
        std::string_view user_body = rewound(user).raw_json();
        std::string_view id_str = rewound(tweet)["id_str"].raw_json_token();
        std::string_view user_screen_name = rewound(user)["screen_name"].raw_json_token();

        ukv_key_t re_id;
        ukv_key_t re_user_id;
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
        edges.push_back(edge_t {.source_id = id, .target_id = user_id});
        if (maybe_retweet.error() == simdjson::SUCCESS) {
            edges.push_back(edge_t {.source_id = id, .target_id = re_id});
            edges.push_back(edge_t {.source_id = user_id, .target_id = re_user_id, .id = re_id});
        }

        auto maybe_mentions = rewound(tweet).find_field("entities").find_field("user_mentions");
        if (maybe_mentions.error() == simdjson::SUCCESS &&
            maybe_mentions.type() == simdjson::ondemand::json_type::array) {
            auto mentions = maybe_mentions.get_array().value();
            for (auto mention : mentions) {
                auto mentioned_id = mention["id"];
                if (mentioned_id.type() != simdjson::ondemand::json_type::number)
                    continue;
                edges.push_back(edge_t {.source_id = user_id, .target_id = mentioned_id, .id = id});
            }
        }
    }
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
    uniform_idx_t choose_file(0, dataset_docs.size() - 1);
    uniform_idx_t choose_hash(0, copies_per_tweet_k - 1);

    auto const batch_size = static_cast<ukv_size_t>(state.range(0));
    std::vector<ukv_key_t> batch_keys(batch_size);

    std::size_t iterations = 0;
    std::size_t successes = 0;
    for (auto _ : state) {
        for (std::size_t idx = 0; idx != batch_size; ++idx) {
            std::size_t const file_idx = choose_file(gen);
            auto const& tweets = dataset_docs[file_idx];
            uniform_idx_t choose_tweet(0, tweets.size() - 1);
            std::size_t const tweet_idx = choose_tweet(gen);
            auto const& tweet = tweets[tweet_idx];

            ukv_key_t tweet_key = tweet.first;
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
    sample_tweet_id_batches(state, [&](ukv_key_t const* ids_tweets, ukv_size_t count) {
        ukv_length_t* offsets = nullptr;
        ukv_byte_t* values = nullptr;

        status_t status;
        ukv_read_t read;
        read.db = db;
        read.error = status.member_ptr();
        read.arena = arena.member_ptr();
        read.tasks_count = count;
        read.collections = &collection_docs_k;
        read.keys = ids_tweets;
        read.keys_stride = sizeof(ukv_key_t);
        read.offsets = &offsets;
        read.values = &values;

        ukv_read(&read);
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
    sample_tweet_id_batches(state, [&](ukv_key_t const* ids_tweets, ukv_size_t count) {
        ukv_length_t* offsets = nullptr;
        ukv_byte_t* values = nullptr;

        status_t status;
        ukv_docs_read_t docs_read;
        docs_read.db = db;
        docs_read.error = status.member_ptr();
        docs_read.arena = arena.member_ptr();
        docs_read.type = ukv_doc_field_json_k;
        docs_read.tasks_count = count;
        docs_read.collections = &collection_docs_k;
        docs_read.keys = ids_tweets;
        docs_read.keys_stride = sizeof(ukv_key_t);
        docs_read.offsets = &offsets;
        docs_read.values = &values;

        ukv_docs_read(&docs_read);
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
    ukv_str_view_t field = "text";

    std::size_t received_bytes = 0;
    sample_tweet_id_batches(state, [&](ukv_key_t const* ids_tweets, ukv_size_t count) {
        ukv_length_t* offsets = nullptr;
        ukv_byte_t* values = nullptr;

        status_t status;
        ukv_docs_read_t docs_read;
        docs_read.db = db;
        docs_read.error = status.member_ptr();
        docs_read.arena = arena.member_ptr();
        docs_read.type = ukv_doc_field_str_k;
        docs_read.tasks_count = count;
        docs_read.collections = &collection_docs_k;
        docs_read.keys = ids_tweets;
        docs_read.keys_stride = sizeof(ukv_key_t);
        docs_read.fields = &field;
        docs_read.offsets = &offsets;
        docs_read.values = &values;

        ukv_docs_read(&docs_read);
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

    constexpr ukv_size_t fields_k = 4;
    ukv_str_view_t names[fields_k] {"timestamp_ms", "reply_count", "retweet_count", "favorite_count"};
    ukv_doc_field_type_t types[fields_k] {
        ukv_doc_field_str_k,
        ukv_doc_field_u32_k,
        ukv_doc_field_u32_k,
        ukv_doc_field_u32_k,
    };

    std::size_t received_bytes = 0;
    sample_tweet_id_batches(state, [&](ukv_key_t const* ids_tweets, ukv_size_t count) {
        ukv_octet_t** validities = nullptr;
        ukv_byte_t** scalars = nullptr;
        ukv_length_t** offsets = nullptr;
        ukv_length_t** lengths = nullptr;
        ukv_byte_t* strings = nullptr;

        status_t status;
        ukv_docs_gather_t docs_gather;
        docs_gather.db = db;
        docs_gather.error = status.member_ptr();
        docs_gather.arena = arena.member_ptr();
        docs_gather.docs_count = count;
        docs_gather.fields_count = fields_k;
        docs_gather.collections = &collection_docs_k;
        docs_gather.keys = ids_tweets;
        docs_gather.keys_stride = sizeof(ukv_key_t);
        docs_gather.fields = names;
        docs_gather.fields_stride = sizeof(ukv_str_view_t);
        docs_gather.types = types;
        docs_gather.types_stride = sizeof(ukv_doc_field_type_t);
        docs_gather.columns_validities = &validities;
        docs_gather.columns_scalars = &scalars;
        docs_gather.columns_offsets = &offsets;
        docs_gather.columns_lengths = &lengths;
        docs_gather.joined_strings = &strings;

        ukv_docs_gather(&docs_gather);
        if (!status)
            return false;

        // One column is just strings
        received_bytes += std::transform_reduce(&lengths[0][0],
                                                &lengths[0][count],
                                                0ul,
                                                std::plus<std::size_t> {},
                                                [](ukv_length_t length) -> std::size_t { //
                                                    return length == ukv_length_missing_k ? 0u : length;
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
    sample_tweet_id_batches(state, [&](ukv_key_t const* ids_tweets, ukv_size_t count) {
        // First hop
        ukv_vertex_role_t const role = ukv_vertex_role_any_k;
        ukv_vertex_degree_t* degrees = nullptr;
        ukv_key_t* ids_in_edges = nullptr;

        status_t status;
        ukv_graph_find_edges_t graph_find_edges_first;
        graph_find_edges_first.db = db;
        graph_find_edges_first.error = status.member_ptr();
        graph_find_edges_first.arena = arena.member_ptr();
        graph_find_edges_first.tasks_count = count;
        graph_find_edges_first.collections = &collection_graph_k;
        graph_find_edges_first.vertices = ids_tweets;
        graph_find_edges_first.vertices_stride = sizeof(ukv_key_t);
        graph_find_edges_first.roles = &role;
        graph_find_edges_first.degrees_per_vertex = &degrees;
        graph_find_edges_first.edges_per_vertex = &ids_in_edges;

        ukv_graph_find_edges(&graph_find_edges_first);
        if (!status)
            return false;

        // Now keep only the unique objects
        auto total_edges = std::transform_reduce(degrees, degrees + count, 0ul, plus, [](ukv_vertex_degree_t d) {
            return d != ukv_vertex_degree_missing_k ? d : 0;
        });
        // Compact ~ Remove edge IDs from three-tuples
        for (std::size_t i = 0; i != total_edges; ++i)
            ids_in_edges[i * 2] = ids_in_edges[i * 3], ids_in_edges[i * 2 + 1] = ids_in_edges[i * 3 + 1];
        auto unique_ids = sort_and_deduplicate(ids_in_edges, ids_in_edges + total_edges * 2);

        ukv_graph_find_edges_t graph_find_edges_second;
        graph_find_edges_second.db = db;
        graph_find_edges_second.error = status.member_ptr();
        graph_find_edges_second.arena = arena.member_ptr();
        graph_find_edges_second.options = ukv_option_dont_discard_memory_k;
        graph_find_edges_second.tasks_count = unique_ids;
        graph_find_edges_second.collections = &collection_graph_k;
        graph_find_edges_second.vertices = ids_in_edges;
        graph_find_edges_second.vertices_stride = sizeof(ukv_key_t);
        graph_find_edges_second.roles = &role;
        graph_find_edges_second.degrees_per_vertex = &degrees;
        graph_find_edges_second.edges_per_vertex = &ids_in_edges;

        // Second hop
        ukv_graph_find_edges(&graph_find_edges_second);
        if (!status)
            return false;

        total_edges += std::transform_reduce(degrees, degrees + unique_ids, 0ul, plus, [](ukv_vertex_degree_t d) {
            return d != ukv_vertex_degree_missing_k ? d : 0;
        });
        received_bytes += total_edges * 3 * sizeof(ukv_key_t);
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
    std::size_t thread_count = std::thread::hardware_concurrency() / 2;
    std::size_t max_input_files = 1000;
    std::size_t min_seconds = 10;
    std::size_t small_batch_size = 32;
    std::size_t mid_batch_size = 64;
    std::size_t big_batch_size = 128;
#if defined(UKV_DEBUG)
    max_input_files = 1;
    thread_count = 1;
#endif

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
    source_files.resize(std::min(max_input_files, source_files.size()));
    std::printf("- kept only %i files\n", static_cast<int>(source_files.size()));
    dataset_paths.resize(source_files.size());
    dataset_docs.resize(source_files.size());
    dataset_graph.resize(source_files.size());
    mapped_contents.resize(source_files.size());

    // 2. Memory-map the contents
    // As we are closing the process after the benchmarks, we can avoid unmap them.
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

    // 3. Index the dataset
    std::printf("Will index the files...\n");
    if (thread_count == 1) {
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
    std::printf("- indexed %zu docs\n", pass_through_size(dataset_docs));
    std::printf("- indexed %zu relations\n", pass_through_size(dataset_graph));
    std::printf("- indexed %zu paths\n", pass_through_size(dataset_paths));

// 4. Run the actual benchmarks
#if defined(UKV_ENGINE_IS_LEVELDB)
    db.open("/mnt/md0/Twitter/LevelDB").throw_unhandled();
#elif defined(UKV_ENGINE_IS_ROCKSDB)
    db.open("/mnt/md0/Twitter/RocksDB").throw_unhandled();
#elif defined(UKV_ENGINE_IS_UDISK)
    db.open("/mnt/md0/Twitter/UnumDB").throw_unhandled();
#else
    db.open().throw_unhandled();
#endif

    bool can_build_graph = false;
    bool can_build_paths = false;
    if (ukv_supports_named_collections_k) {
        status_t status;
        ukv_collection_create_t collection_init {
            .db = db,
            .error = status.member_ptr(),
            .name = "twitter.docs",
            .config = "",
            .id = &collection_docs_k,
        };

        ukv_collection_create(&collection_init);
        status.throw_unhandled();

        collection_init.name = "twitter.graph";
        collection_init.id = &collection_graph_k;
        ukv_collection_create(&collection_init);
        status.throw_unhandled();
        can_build_graph = true;

        collection_init.name = "twitter.nicks";
        collection_init.id = &collection_paths_k;
        ukv_collection_create(&collection_init);
        status.throw_unhandled();
        can_build_paths = true;
    }

    std::printf("Will benchmark...\n");
    bm::RegisterBenchmark("construct_docs", &construct_docs) //
        ->Iterations(pass_through_size(dataset_docs) / (thread_count * big_batch_size))
        ->UseRealTime()
        ->Threads(thread_count)
        ->Arg(big_batch_size);

    if (can_build_graph)
        bm::RegisterBenchmark("construct_graph", &construct_graph) //
            ->Iterations(pass_through_size(dataset_graph) / (thread_count * big_batch_size))
            ->UseRealTime()
            ->Threads(thread_count)
            ->Arg(big_batch_size);

    if (can_build_paths)
        bm::RegisterBenchmark("construct_paths", &construct_paths) //
            ->Iterations(pass_through_size(dataset_paths) / (thread_count * big_batch_size))
            ->UseRealTime()
            ->Threads(thread_count)
            ->Arg(big_batch_size);

    if (ukv_doc_field_default_k != ukv_doc_field_json_k)
        bm::RegisterBenchmark("docs_sample_blobs", &docs_sample_blobs) //
            ->MinTime(min_seconds)
            ->UseRealTime()
            ->Threads(thread_count)
            ->Arg(small_batch_size)
            ->Arg(mid_batch_size)
            ->Arg(big_batch_size);

    bm::RegisterBenchmark("docs_sample_objects", &docs_sample_objects) //
        ->MinTime(min_seconds)
        ->UseRealTime()
        ->Threads(thread_count)
        ->Arg(small_batch_size)
        ->Arg(mid_batch_size)
        ->Arg(big_batch_size);

    bm::RegisterBenchmark("docs_sample_field", &docs_sample_field) //
        ->MinTime(min_seconds)
        ->UseRealTime()
        ->Threads(thread_count)
        ->Arg(small_batch_size)
        ->Arg(mid_batch_size)
        ->Arg(big_batch_size);

    bm::RegisterBenchmark("docs_sample_table", &docs_sample_table) //
        ->MinTime(min_seconds)
        ->UseRealTime()
        ->Threads(thread_count)
        ->Arg(small_batch_size)
        ->Arg(mid_batch_size)
        ->Arg(big_batch_size);

    if (can_build_graph)
        bm::RegisterBenchmark("graph_traverse_two_hops", &graph_traverse_two_hops) //
            ->MinTime(min_seconds)
            ->Threads(thread_count)
            ->Arg(small_batch_size)
            ->Arg(mid_batch_size)
            ->Arg(big_batch_size);

    bm::RunSpecifiedBenchmarks();
    bm::Shutdown();

    // To avoid sanitizer complaints, we should unmap the files:
    for (auto mapped_content : mapped_contents)
        munmap((void*)mapped_content.data(), mapped_content.size());
    db.clear().throw_unhandled();

    return 0;
}
