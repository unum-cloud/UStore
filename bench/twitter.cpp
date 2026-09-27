/**
 *  @file bench/twitter.cpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Benchmarks document and graph workloads over real or generated tweets parsed with yyjson.
 */

#include <cstddef>    // `std::size_t`
#include <cstdint>    // `std::int64_t`, `std::uint64_t`
#include <cstdio>     // `stderr`
#include <dirent.h>   // `opendir`, `readdir`, `closedir`
#include <fcntl.h>    // `open`
#include <sys/mman.h> // `mmap`, `madvise`, `munmap`
#include <sys/stat.h> // `fstat`
#include <unistd.h>   // `close`

#include <algorithm>   // `std::min`, `std::sort`, `std::unique`
#include <array>       // `std::array`
#include <atomic>      // `std::atomic`
#include <charconv>    // `std::to_chars`
#include <chrono>      // `std::chrono::nanoseconds`
#include <functional>  // `std::plus`
#include <numeric>     // `std::transform_reduce`
#include <random>      // `std::mt19937_64`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::move`, `std::swap`

#include <benchmark/benchmark.h>
#include <fmt/format.h>
#include <yyjson.h>

#include <smashtable/basic_vector.hpp>
#include <stringzilla/stringzilla.hpp>

#include <ustore/blobs.h>
#include <ustore/db.h>
#include <ustore/docs.h>
#include <ustore/graph.h>

#include "arguments.hpp"
#include "executor.hpp"
#include "mixed.hpp"

namespace unum::ustore::bench {

#pragma region Settings

/** Longest database configuration: a directory path and the JSON around it. */
constexpr std::size_t config_capacity_k = 4096 + 64;

/** Command-line options of the benchmark, each field set by the flag of the same name. */
struct settings_t {

    /** Directory of NDJSON tweet files; tweets are generated without it. */
    std::string_view dataset;

    /** Directory the database lives in. */
    std::string_view directory;

    /** Threads every benchmark and the indexing run on. */
    count_t threads {.value = 1, .minimum = 1};

    /** Tweets to generate without a dataset. */
    count_t max_tweets {1'000'000};

    /** Input files mapped from the dataset. */
    count_t max_input_files_total {1000};

    /** Average mentions per generated tweet. */
    count_t connectivity {4};

    /** Minimal time each sampling benchmark runs. */
    std::chrono::nanoseconds duration = std::chrono::seconds {10};

    /** The three sampling batch sizes, smallest first; construction takes the last. */
    counts_t batch_sizes {.values = {32, 64, 128}, .size = 3, .minimum = 1, .minimum_size = 3, .maximum_size = 3};
};

#pragma endregion Settings

#pragma region Dataset

/** A read-only memory map of a whole file, unmapped on destruction. */
class mapped_file_t {
    char const* data_ = nullptr;
    std::size_t size_ = 0;

  public:
    mapped_file_t() noexcept = default;
    mapped_file_t(mapped_file_t&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}
    mapped_file_t& operator=(mapped_file_t&& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
        return *this;
    }
    ~mapped_file_t() noexcept {
        if (size_ != 0) { [[maybe_unused]] int const unmapped = munmap(const_cast<char*>(data_), size_); }
    }

    /** Maps @p path and answers whether it succeeded. */
    [[nodiscard]] bool map(char const* path) noexcept {
        int const descriptor = open(path, O_RDONLY);
        if (descriptor == -1) return false;
        bool const mapped = map_descriptor(descriptor);
        [[maybe_unused]] int const closed = close(descriptor);
        return mapped;
    }

    /** The mapped bytes, empty before a successful @c map or for an empty file. */
    [[nodiscard]] std::string_view view() const noexcept { return {data_, size_}; }

  private:

    /** Maps the whole of the already open @p descriptor and answers whether it succeeded. */
    [[nodiscard]] bool map_descriptor(int descriptor) noexcept {
        struct stat metadata {};
        if (fstat(descriptor, &metadata) != 0) return false;
        if (metadata.st_size == 0) return true;
        std::size_t const size = static_cast<std::size_t>(metadata.st_size);
        void* const begin = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, descriptor, 0);
        if (begin == MAP_FAILED) return false;
        [[maybe_unused]] int const advised = madvise(begin, size, MADV_SEQUENTIAL);
        data_ = static_cast<char const*>(begin);
        size_ = size;
        return true;
    }
};

/** Owns a parsed yyjson document. */
struct json_document_t {
    yyjson_doc* handle = nullptr;

    explicit json_document_t(yyjson_doc* document) noexcept : handle(document) {}
    json_document_t(json_document_t const&) = delete;
    json_document_t& operator=(json_document_t const&) = delete;
    ~json_document_t() noexcept { yyjson_doc_free(handle); }
};

/** Everything the benchmarks share: the open database, its collections and the indexed tweets. */
struct twitter_t {

    /** Open database. */
    ustore_database_t db = nullptr;

    /** Collection of tweet and author documents. */
    ustore_collection_t docs = ustore_collection_main_k;

    /** Collection of the graph between tweets and authors. */
    ustore_collection_t graph = ustore_collection_main_k;

    /** Memory-mapped NDJSON files the tweets come from. */
    st::basic_vector<mapped_file_t> files;

    /** Generated tweets, one JSON object per line. */
    sz::string_t generated;

    /** Documents indexed from each file, or from the generated tweets. */
    st::basic_vector<st::basic_vector<keyed_document_t>> documents;

    /** Edges indexed alongside @c documents. */
    st::basic_vector<st::basic_vector<graph_edge_t>> edges;
};

/** Indexes one tweet as documents and edges, answering false only when an allocation fails. */
[[nodiscard]] bool index_tweet(yyjson_val* tweet, std::string_view body, st::basic_vector<keyed_document_t>& documents,
                               st::basic_vector<graph_edge_t>& edges) noexcept {
    yyjson_val* const id = yyjson_obj_get(tweet, "id");
    yyjson_val* const user_id = yyjson_obj_get(yyjson_obj_get(tweet, "user"), "id");
    if (!yyjson_is_int(id) || !yyjson_is_int(user_id)) return true;
    ustore_key_t const tweet_key = yyjson_get_sint(id);
    ustore_key_t const user_key = yyjson_get_sint(user_id);

    if (st::failed(documents.push_back(keyed_document_t {tweet_key, body})) ||
        st::failed(documents.push_back(keyed_document_t {user_key, body})) ||
        st::failed(edges.push_back(graph_edge_t {tweet_key, user_key, ustore_default_edge_id_k})))
        return false;

    yyjson_val* const retweet = yyjson_obj_get(tweet, "retweeted_status");
    yyjson_val* const retweet_id = yyjson_obj_get(retweet, "id");
    yyjson_val* const retweet_user_id = yyjson_obj_get(yyjson_obj_get(retweet, "user"), "id");
    if (yyjson_is_int(retweet_id) && yyjson_is_int(retweet_user_id)) {
        ustore_key_t const retweet_key = yyjson_get_sint(retweet_id);
        if (st::failed(edges.push_back(graph_edge_t {tweet_key, retweet_key, ustore_default_edge_id_k})) ||
            st::failed(edges.push_back(graph_edge_t {user_key, yyjson_get_sint(retweet_user_id), retweet_key})))
            return false;
    }

    yyjson_val* const mentions = yyjson_obj_get(yyjson_obj_get(tweet, "entities"), "user_mentions");
    yyjson_arr_iter iterator {};
    if (!yyjson_arr_iter_init(mentions, &iterator)) return true;
    for (yyjson_val* mention = yyjson_arr_iter_next(&iterator); mention; mention = yyjson_arr_iter_next(&iterator)) {
        yyjson_val* const mentioned_id = yyjson_obj_get(mention, "id");
        if (!yyjson_is_int(mentioned_id)) return true;
        if (st::failed(edges.push_back(graph_edge_t {user_key, yyjson_get_sint(mentioned_id), tweet_key})))
            return false;
    }
    return true;
}

/** Indexes every line of @p text holding a tweet, answering false only when an allocation fails. */
[[nodiscard]] bool index_tweets(std::string_view text, st::basic_vector<keyed_document_t>& documents,
                                st::basic_vector<graph_edge_t>& edges) noexcept {
    for (sz::string_view_t line : sz::split(sz::string_view_t {text}, "\n")) {
        if (line.size() == 0) continue;
        json_document_t const document {yyjson_read(line.data(), line.size(), 0)};
        yyjson_val* const root = yyjson_doc_get_root(document.handle);
        if (!yyjson_is_obj(root)) continue;
        if (!index_tweet(root, line, documents, edges)) return false;
    }
    return true;
}

/** Appends @p value as decimal digits and answers whether the buffer grew. */
[[nodiscard]] bool append_integer(sz::string_t& text, std::int64_t value) noexcept {
    char digits[24];
    std::to_chars_result const printed = std::to_chars(digits, digits + sizeof(digits), value);
    return text.try_append(digits, static_cast<std::size_t>(printed.ptr - digits));
}

/** The tweet @c append_tweet fills with an id, a text, an author and mentions. */
constexpr std::string_view
    tweet_template_k =
        R"json({"created_at":"Sat Jan 01 00:59:05 +0000 2022","id":%1$d,"id_str":"%1$d","text":"%2$s",)json" R"json("source":"<a href=\"https://twitter.com/settings/download\" rel=\"nofollow\">Twitter for DEVICE</a>",)json" R"json("truncated":false,"in_reply_to_status_id":null,"in_reply_to_status_id_str":null,)json" R"json("in_reply_to_user_id":null,"in_reply_to_user_id_str":null,"in_reply_to_screen_name":null,)json" R"json("user":{"id":%3$d,"id_str":"%3$d","name":"USER_NAME","screen_name":"SCREEN_NAME",)json" R"json("location":"USER_LOCATION","url":null,"description":"USER_DESCRIPTION","translator_type":"none",)json" R"json("protected":false,"verified":false,"followers_count":54,"friends_count":62,"listed_count":3,)json" R"json("favourites_count":2636,"statuses_count":3297,"created_at":"Mon Jul 16 10:50:37 +0000 2018",)json" R"json("utc_offset":null,"time_zone":null,"geo_enabled":false,"lang":null,"contributors_enabled":false,)json" R"json("is_translator":false,"profile_background_color":"F5F8FA","profile_background_image_url":"",)json" R"json("profile_background_image_url_https":"","profile_background_tile":false,"profile_link_color":"1DA1F2",)json" R"json("profile_sidebar_border_color":"C0DEED","profile_sidebar_fill_color":"DDEEF6",)json" R"json("profile_text_color":"333333","profile_use_background_image":true,)json" R"json("profile_image_url":"http://pbs.twimg.com/profile_images/",)json" R"json("profile_image_url_https":"https://pbs.twimg.com/profile_images/",)json" R"json("profile_banner_url":"https://pbs.twimg.com/profile_banners/","default_profile":true,)json" R"json("default_profile_image":false,"following":null,"follow_request_sent":null,"notifications":null,)json" R"json("withheld_in_countries":[]},"geo":null,"coordinates":null,"place":null,"contributors":null,)json" R"json("is_quote_status":false,"quote_count":0,"reply_count":0,"retweet_count":0,"favorite_count":0,)json" R"json("entities":{"hashtags":[],"urls":[],"user_mentions":[%4$s],"symbols":[]},"favorited":false,)json" R"json("retweeted":false,"filter_level":"low","lang":"ja","timestamp_ms":"1640998745659"})json";

/** Appends one tweet, filling the @c %1$d id, @c %2$s text, @c %3$d author and @c %4$s mentions of the template. */
[[nodiscard]] bool append_tweet(sz::string_t& output, std::int64_t tweet_id, std::size_t text_length,
                                std::int64_t user_id,
                                st::basic_vector<std::int64_t> const& mentioned_user_ids) noexcept {
    std::string_view const tweet_template = tweet_template_k;
    std::size_t cursor = 0;
    for (std::size_t percent = tweet_template.find('%'); percent != std::string_view::npos;
         percent = tweet_template.find('%', cursor)) {
        if (!output.try_append(tweet_template.data() + cursor, percent - cursor)) return false;
        if (percent + 3 >= tweet_template.size() || tweet_template[percent + 2] != '$') return false;
        cursor = percent + 4;
        switch (tweet_template[percent + 1]) {
        case '1':
            if (!append_integer(output, tweet_id)) return false;
            break;
        case '2':
            if (!output.try_resize(output.size() + text_length, '_')) return false;
            break;
        case '3':
            if (!append_integer(output, user_id)) return false;
            break;
        case '4':
            for (std::size_t index = 0; index != mentioned_user_ids.size(); ++index) {
                std::int64_t const mentioned = mentioned_user_ids[index];
                if ((index != 0 && !output.try_push_back(',')) ||
                    !output.try_append(R"({"screen_name":"","name":"","id":)") || !append_integer(output, mentioned) ||
                    !output.try_append(R"(,"id_str":")") || !append_integer(output, mentioned) ||
                    !output.try_append(R"(","indices":[]})"))
                    return false;
            }
            break;
        default: return false;
        }
    }
    return output.try_append(tweet_template.data() + cursor, tweet_template.size() - cursor);
}

/** Generates @c max_tweets tweets from @c tweet_template_k, one per line, and indexes them. */
[[nodiscard]] bool generate_dataset(settings_t const& settings, twitter_t& twitter) noexcept {
    fmt::println("Will generate tweets...");
    std::size_t const tweets_count = settings.max_tweets.value;
    std::mt19937_64 generator {tweets_count};
    st::basic_vector<std::int64_t> user_ids;
    st::basic_vector<std::int64_t> mentioned_user_ids;
    if (st::failed(user_ids.reserve(tweets_count))) return false;
    for (std::size_t tweet = 0; tweet != tweets_count; ++tweet) {
        std::int64_t const tweet_id = static_cast<std::int64_t>(generator() >> 1);
        std::int64_t const user_id = static_cast<std::int64_t>(generator() >> 1);
        std::size_t const text_length = 1 + generator() % 280;
        std::size_t const mentions_count = std::min<std::size_t>(generator() % (2 * settings.connectivity.value + 1),
                                                                 user_ids.size());

        mentioned_user_ids.clear();
        for (std::size_t mention = 0; mention != mentions_count; ++mention)
            if (st::failed(mentioned_user_ids.push_back(std::int64_t {user_ids[generator() % user_ids.size()]})))
                return false;
        if (!append_tweet(twitter.generated, tweet_id, text_length, user_id, mentioned_user_ids) ||
            !twitter.generated.try_push_back('\n'))
            return false;
        [[maybe_unused]] st::status_t const pushed = user_ids.push_back(st::assume_reserved, std::int64_t {user_id});
    }

    if (st::failed(twitter.documents.resize(1)) || st::failed(twitter.edges.resize(1))) return false;
    fmt::println("Will index the generated tweets...");
    return index_tweets(twitter.generated, twitter.documents[0], twitter.edges[0]);
}

/** Maps up to @c max_input_files_total NDJSON files from the dataset directory and indexes them on @p executor. */
[[nodiscard]] bool load_dataset(settings_t const& settings, executor_t& executor, twitter_t& twitter) noexcept {
    fmt::println("Will search for .ndjson files...");
    sz::string_t directory;
    if (!directory.try_append(settings.dataset.data(), settings.dataset.size()) || !directory.try_push_back('/'))
        return false;

    DIR* const listing = opendir(directory.c_str());
    if (!listing) {
        fmt::println(stderr, "Failed to open {}", directory.c_str());
        return false;
    }
    std::size_t found = 0;
    sz::string_t path;
    for (dirent* entry = readdir(listing); entry; entry = readdir(listing)) {
        if (!std::string_view {entry->d_name}.ends_with(".ndjson")) continue;
        ++found;
        if (twitter.files.size() == settings.max_input_files_total.value) continue;
        path.clear();
        mapped_file_t file;
        if (!path.try_append(directory) || !path.try_append(entry->d_name) || !file.map(path.c_str()) ||
            st::failed(twitter.files.push_back(std::move(file)))) {
            [[maybe_unused]] int const closed = closedir(listing);
            fmt::println(stderr, "Failed to map {}", path.c_str());
            return false;
        }
    }
    [[maybe_unused]] int const closed = closedir(listing);
    fmt::println("- found {} files", found);
    fmt::println("- kept only {} files", twitter.files.size());

    if (st::failed(twitter.documents.resize(twitter.files.size())) ||
        st::failed(twitter.edges.resize(twitter.files.size())))
        return false;
    fmt::println("Will index the files...");
    std::atomic<std::size_t> failed_files {0};
    st::status_t const dispatched = executor.for_n(twitter.files.size(), [&](std::size_t file, std::size_t) noexcept {
        if (!index_tweets(twitter.files[file].view(), twitter.documents[file], twitter.edges[file]))
            failed_files.fetch_add(1, std::memory_order_relaxed);
    });
    return st::succeeded(dispatched) && failed_files.load() == 0;
}

#pragma endregion Dataset

#pragma region Construction

/** Builds up a collection of tweet and author documents. */
void construct_docs(bm::State& state, twitter_t const& twitter) {
    docs_upsert(state, twitter.db, twitter.docs, twitter.documents);
}

/** Builds up a graph linking tweets with their authors and retweets, and authors with the users they mention. */
void construct_graph(bm::State& state, twitter_t const& twitter) {
    edges_upsert(state, twitter.db, twitter.graph, twitter.edges);
}

#pragma endregion Construction

#pragma region Analytics

/** Hands @p callback batches of @c state.range(0) random tweet keys and counts the batches it answers false for. */
template <typename callback_type_>
void sample_tweet_batches(bm::State& state, twitter_t const& twitter, callback_type_&& callback) {
    auto const batch_size = static_cast<std::size_t>(state.range(0));
    st::basic_vector<ustore_key_t> batch;
    if (pass_through_size(twitter.documents) == 0 || st::failed(batch.resize(batch_size))) {
        state.SkipWithError("No tweets to sample");
        return;
    }

    std::mt19937_64 generator {static_cast<std::uint64_t>(state.thread_index()) + 1};
    std::size_t iterations = 0;
    std::size_t successes = 0;
    for ([[maybe_unused]] auto iteration : state) {
        for (ustore_key_t& key : batch) {
            std::size_t part = generator() % twitter.documents.size();
            while (twitter.documents[part].size() == 0) part = (part + 1) % twitter.documents.size();
            auto const& tweets = twitter.documents[part];
            key = tweets[generator() % tweets.size()].key;
        }
        successes += callback(batch.data(), batch_size);
        ++iterations;
    }

    state.counters["items/s"] = bm::Counter(iterations * batch_size, bm::Counter::kIsRate);
    state.counters["batches/s"] = bm::Counter(iterations, bm::Counter::kIsRate);
    state.counters["fails,%"] = bm::Counter((iterations - successes) * 100.0, bm::Counter::kAvgThreads);
}

/** Answers whether @p error stayed empty, freeing it otherwise. */
[[nodiscard]] bool succeeded(ustore_error_t& error) noexcept {
    if (!error.status) return true;
    error = {};
    return false;
}

/** Samples batches of tweet keys, reading only the keys the sampler returns. */
void docs_sample_keys(bm::State& state, twitter_t const& twitter) {
    auto const batch_size = static_cast<ustore_size_t>(state.range(0));
    ustore_arena_t arena = nullptr;
    ustore_error_t error {};
    ustore_length_t const limit = 1;

    std::size_t received_bytes = 0;
    std::size_t iterations = 0;
    std::size_t successes = 0;
    for ([[maybe_unused]] auto iteration : state) {
        ustore_length_t* found_offsets = nullptr;
        ustore_length_t* found_counts = nullptr;
        ustore_key_t* found_keys = nullptr;
        ustore_sample_t sample {};
        sample.db = twitter.db;
        sample.error = &error;
        sample.arena = &arena;
        sample.tasks_count = batch_size;
        sample.collections = &twitter.docs;
        sample.count_limits = &limit;
        sample.count_limits_stride = 0;
        sample.offsets = &found_offsets;
        sample.counts = &found_counts;
        sample.keys = &found_keys;
        ustore_sample(&sample);
        ++iterations;
        if (!succeeded(error)) continue;
        received_bytes += found_offsets[batch_size];
        ++successes;
    }
    ustore_arena_free(arena);

    state.counters["items/s"] = bm::Counter(iterations * batch_size, bm::Counter::kIsRate);
    state.counters["batches/s"] = bm::Counter(iterations, bm::Counter::kIsRate);
    state.counters["fails,%"] = bm::Counter((iterations - successes) * 100.0, bm::Counter::kAvgThreads);
    state.counters["bytes/s"] = bm::Counter(received_bytes, bm::Counter::kIsRate);
    state.counters["bytes/it"] = bm::Counter(received_bytes, bm::Counter::kAvgIterations);
}

/** Reads whole tweets as JSON, which parses and serializes every document. */
void docs_sample_objects(bm::State& state, twitter_t const& twitter) {
    ustore_arena_t arena = nullptr;
    ustore_error_t error {};
    std::size_t received_bytes = 0;
    sample_tweet_batches(state, twitter, [&](ustore_key_t const* keys, ustore_size_t count) noexcept {
        ustore_length_t* offsets = nullptr;
        ustore_bytes_ptr_t values = nullptr;
        ustore_docs_read_t read {};
        read.db = twitter.db;
        read.error = &error;
        read.arena = &arena;
        read.type = ustore_doc_field_json_k;
        read.tasks_count = count;
        read.collections = &twitter.docs;
        read.keys = keys;
        read.keys_stride = sizeof(ustore_key_t);
        read.offsets = &offsets;
        read.values = &values;
        ustore_docs_read(&read);
        if (!succeeded(error)) return false;
        received_bytes += offsets[count];
        return true;
    });
    ustore_arena_free(arena);

    state.counters["bytes/s"] = bm::Counter(received_bytes, bm::Counter::kIsRate);
    state.counters["bytes/it"] = bm::Counter(received_bytes, bm::Counter::kAvgIterations);
}

/** Samples batches of tweets, reading one field out of each document. */
void docs_sample_field(bm::State& state, twitter_t const& twitter) {
    ustore_arena_t arena = nullptr;
    ustore_error_t error {};
    ustore_str_view_t const field = "text";
    std::size_t received_bytes = 0;
    sample_tweet_batches(state, twitter, [&](ustore_key_t const* keys, ustore_size_t count) noexcept {
        ustore_length_t* offsets = nullptr;
        ustore_bytes_ptr_t values = nullptr;
        ustore_docs_read_t read {};
        read.db = twitter.db;
        read.error = &error;
        read.arena = &arena;
        read.type = ustore_doc_field_str_k;
        read.tasks_count = count;
        read.collections = &twitter.docs;
        read.keys = keys;
        read.keys_stride = sizeof(ustore_key_t);
        read.fields = &field;
        read.offsets = &offsets;
        read.values = &values;
        ustore_docs_read(&read);
        if (!succeeded(error)) return false;
        received_bytes += offsets[count];
        return true;
    });
    ustore_arena_free(arena);

    state.counters["bytes/s"] = bm::Counter(received_bytes, bm::Counter::kIsRate);
    state.counters["bytes/it"] = bm::Counter(received_bytes, bm::Counter::kAvgIterations);
}

/** Samples batches of tweets, gathering four typed fields of each into a table. */
void docs_sample_table(bm::State& state, twitter_t const& twitter) {
    constexpr std::size_t fields_count_k = 4;
    ustore_str_view_t const names[fields_count_k] {"timestamp_ms", "reply_count", "retweet_count", "favorite_count"};
    ustore_doc_field_type_t const types[fields_count_k] {
        ustore_doc_field_str_k,
        ustore_doc_field_u32_k,
        ustore_doc_field_u32_k,
        ustore_doc_field_u32_k,
    };

    ustore_arena_t arena = nullptr;
    ustore_error_t error {};
    std::size_t received_bytes = 0;
    sample_tweet_batches(state, twitter, [&](ustore_key_t const* keys, ustore_size_t count) noexcept {
        ustore_octet_t** validities = nullptr;
        ustore_byte_t** scalars = nullptr;
        ustore_length_t** offsets = nullptr;
        ustore_length_t** lengths = nullptr;
        ustore_byte_t* strings = nullptr;
        ustore_docs_gather_t gather {};
        gather.db = twitter.db;
        gather.error = &error;
        gather.arena = &arena;
        gather.docs_count = count;
        gather.fields_count = fields_count_k;
        gather.collections = &twitter.docs;
        gather.keys = keys;
        gather.keys_stride = sizeof(ustore_key_t);
        gather.fields = names;
        gather.fields_stride = sizeof(ustore_str_view_t);
        gather.types = types;
        gather.types_stride = sizeof(ustore_doc_field_type_t);
        gather.columns_validities = &validities;
        gather.columns_scalars = &scalars;
        gather.columns_offsets = &offsets;
        gather.columns_lengths = &lengths;
        gather.joined_strings = &strings;
        ustore_docs_gather(&gather);
        if (!succeeded(error)) return false;

        // The first column holds strings, the other three hold 32-bit scalars.
        received_bytes += std::transform_reduce(
            lengths[0], lengths[0] + count, std::size_t {0}, std::plus<std::size_t> {},
            [](ustore_length_t length) noexcept { return length == ustore_length_missing_k ? 0u : length; });
        received_bytes += (fields_count_k - 1) * sizeof(std::uint32_t) * count;
        return true;
    });
    ustore_arena_free(arena);

    state.counters["bytes/s"] = bm::Counter(received_bytes, bm::Counter::kIsRate);
    state.counters["bytes/it"] = bm::Counter(received_bytes, bm::Counter::kAvgIterations);
}

/** Sums the @p count degrees, counting a missing vertex as zero. */
[[nodiscard]] std::size_t sum_degrees(ustore_vertex_degree_t const* degrees, std::size_t count) noexcept {
    return std::transform_reduce(
        degrees, degrees + count, std::size_t {0}, std::plus<std::size_t> {},
        [](ustore_vertex_degree_t degree) noexcept { return degree != ustore_vertex_degree_missing_k ? degree : 0u; });
}

/** Gathers the edges of sampled tweets, then the edges of every vertex those edges reach. */
void graph_traverse_two_hops(bm::State& state, twitter_t const& twitter) {
    ustore_arena_t arena = nullptr;
    ustore_error_t error {};
    ustore_vertex_role_t const role = ustore_vertex_role_any_k;
    std::size_t received_bytes = 0;
    std::size_t received_edges = 0;
    sample_tweet_batches(state, twitter, [&](ustore_key_t const* keys, ustore_size_t count) noexcept {
        ustore_vertex_degree_t* degrees = nullptr;
        ustore_key_t* ids_in_edges = nullptr;
        ustore_graph_find_edges_t first_hop {};
        first_hop.db = twitter.db;
        first_hop.error = &error;
        first_hop.arena = &arena;
        first_hop.tasks_count = count;
        first_hop.collections = &twitter.graph;
        first_hop.vertices = keys;
        first_hop.vertices_stride = sizeof(ustore_key_t);
        first_hop.roles = &role;
        first_hop.degrees_per_vertex = &degrees;
        first_hop.edges_per_vertex = &ids_in_edges;
        ustore_graph_find_edges(&first_hop);
        if (!succeeded(error)) return false;

        // Keeps the two vertices of every (source, target, edge) triple, then only the unique vertices.
        std::size_t total_edges = sum_degrees(degrees, count);
        for (std::size_t index = 0; index != total_edges; ++index) {
            ids_in_edges[index * 2] = ids_in_edges[index * 3];
            ids_in_edges[index * 2 + 1] = ids_in_edges[index * 3 + 1];
        }
        std::sort(ids_in_edges, ids_in_edges + total_edges * 2);
        std::size_t const unique_count = static_cast<std::size_t>(
            std::unique(ids_in_edges, ids_in_edges + total_edges * 2) - ids_in_edges);

        ustore_graph_find_edges_t second_hop = first_hop;
        second_hop.options = ustore_option_dont_discard_memory_k;
        second_hop.tasks_count = unique_count;
        second_hop.vertices = ids_in_edges;
        ustore_graph_find_edges(&second_hop);
        if (!succeeded(error)) return false;

        total_edges += sum_degrees(degrees, unique_count);
        received_bytes += total_edges * 3 * sizeof(ustore_key_t);
        received_edges += total_edges;
        return true;
    });
    ustore_arena_free(arena);

    state.counters["bytes/s"] = bm::Counter(received_bytes, bm::Counter::kIsRate);
    state.counters["bytes/it"] = bm::Counter(received_bytes, bm::Counter::kAvgIterations);
    state.counters["edges/s"] = bm::Counter(received_edges, bm::Counter::kIsRate);
}

#pragma endregion Analytics

#pragma region Database

/** Creates a collection into @p id, reporting a failure against @p name on the standard error. */
[[nodiscard]] bool create_collection(ustore_database_t db, char const* name, ustore_collection_t& id) noexcept {
    ustore_error_t error {};
    ustore_collection_create_t create {};
    create.db = db;
    create.error = &error;
    create.config = "";
    create.id = &id;
    ustore_collection_create(&create);
    if (!error.status) return true;
    fmt::println(stderr, "Failed to create {}: {}", name, error.message);
    return false;
}

/** Drops @p id as @p mode asks, reporting a failure on the standard error. */
void drop_collection(ustore_database_t db, ustore_collection_t id, ustore_drop_mode_t mode) noexcept {
    ustore_error_t error {};
    ustore_collection_drop_t drop {};
    drop.db = db;
    drop.error = &error;
    drop.id = id;
    drop.mode = mode;
    ustore_collection_drop(&drop);
    if (error.status) fmt::println(stderr, "Failed to clear a collection: {}", error.message);
}

#pragma endregion Database

} // namespace unum::ustore::bench

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::bench;

    settings_t settings;
    if (auto const cores = executor_t::logical_cores_count(); cores && *cores > 1) settings.threads.value = *cores / 2;
    std::array<flag_t, 8> const flags {{
        {"--dataset", "path", "directory of .ndjson tweet files; tweets are generated without it", &settings.dataset},
        {"--directory", "path", "database directory, required", &settings.directory},
        {"--threads", "count", "threads every benchmark runs on", &settings.threads},
        {"--max-tweets", "count", "tweets to generate without a dataset", &settings.max_tweets},
        {"--max-input-files-total", "count", "input files mapped from the dataset", &settings.max_input_files_total},
        {"--connectivity", "count", "average mentions per generated tweet", &settings.connectivity},
        {"--duration", "duration", "minimal time of each sampling benchmark", &settings.duration},
        {"--batch-sizes", "counts", "sampling batch sizes, the last also for construction", &settings.batch_sizes},
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
    if (settings.directory.empty()) {
        fmt::println(stderr, "--directory is required");
        return 1;
    }

    char config[config_capacity_k];
    auto const written = fmt::format_to_n(config, sizeof(config) - 1, R"({{"version": "1.0", "directory": "{}"}})",
                                          settings.directory);
    if (written.size > sizeof(config) - 1) {
        fmt::println(stderr, "--directory is too long");
        return 1;
    }
    *written.out = '\0';

    twitter_t twitter;
    {
        auto maybe_executor = executor_t::make(settings.threads.value, caller_exclusivity_t::caller_inclusive_k);
        if (!maybe_executor) {
            fmt::println(stderr, "Failed to spawn {} threads", settings.threads.value);
            return 1;
        }
        bool const indexed = settings.dataset.empty() ? generate_dataset(settings, twitter)
                                                      : load_dataset(settings, *maybe_executor, twitter);
        if (!indexed) {
            fmt::println(stderr, "Failed to prepare the dataset");
            return 1;
        }
    }
    fmt::println("- indexed {} docs", pass_through_size(twitter.documents));
    fmt::println("- indexed {} relations", pass_through_size(twitter.edges));

    ustore_error_t error {};
    ustore_database_init_t init {};
    init.config = config;
    init.db = &twitter.db;
    init.error = &error;
    ustore_database_init(&init);
    if (error.status) {
        fmt::println(stderr, "Failed to open the database: {}", error.message);
        return 1;
    }

    ustore_metadata_t metadata {};
    ustore_get_metadata_t get_metadata {};
    get_metadata.db = twitter.db;
    get_metadata.error = &error;
    get_metadata.metadata = &metadata;
    ustore_get_metadata(&get_metadata);
    if (error.status) {
        fmt::println(stderr, "Failed to read the database metadata: {}", error.message);
        ustore_database_free(twitter.db);
        return 1;
    }

    bool const supports_collections = (metadata & ustore_supports_collections_k) != 0;
    bool const supports_transactions = (metadata & ustore_supports_transactions_k) != 0;
    if (supports_collections && (!create_collection(twitter.db, "twitter.docs", twitter.docs) ||
                                 !create_collection(twitter.db, "twitter.graph", twitter.graph))) {
        ustore_database_free(twitter.db);
        return 1;
    }

    fmt::println("Will benchmark...");
    std::size_t const threads = settings.threads.value;
    std::size_t const small_batch_size = settings.batch_sizes.values[0];
    std::size_t const mid_batch_size = settings.batch_sizes.values[1];
    std::size_t const big_batch_size = settings.batch_sizes.values[2];
    double const min_seconds = std::chrono::duration<double>(settings.duration).count();
    if (supports_transactions)
        bm::RegisterBenchmark("construct_docs", [&twitter](bm::State& state) { construct_docs(state, twitter); })
            ->Iterations(pass_through_size(twitter.documents) / (threads * big_batch_size))
            ->UseRealTime()
            ->Threads(threads)
            ->Arg(big_batch_size);

    if (supports_collections)
        bm::RegisterBenchmark("construct_graph", [&twitter](bm::State& state) { construct_graph(state, twitter); })
            ->Iterations(pass_through_size(twitter.edges) / (threads * big_batch_size))
            ->UseRealTime()
            ->Threads(threads)
            ->Arg(big_batch_size);

    bm::RegisterBenchmark("docs_sample_keys", [&twitter](bm::State& state) { docs_sample_keys(state, twitter); })
        ->MinTime(min_seconds)
        ->UseRealTime()
        ->Threads(threads)
        ->Arg(small_batch_size)
        ->Arg(mid_batch_size)
        ->Arg(big_batch_size);

    bm::RegisterBenchmark("docs_sample_objects", [&twitter](bm::State& state) { docs_sample_objects(state, twitter); })
        ->MinTime(min_seconds)
        ->UseRealTime()
        ->Threads(threads)
        ->Arg(small_batch_size)
        ->Arg(mid_batch_size)
        ->Arg(big_batch_size);

    bm::RegisterBenchmark("docs_sample_field", [&twitter](bm::State& state) { docs_sample_field(state, twitter); })
        ->MinTime(min_seconds)
        ->UseRealTime()
        ->Threads(threads)
        ->Arg(small_batch_size)
        ->Arg(mid_batch_size)
        ->Arg(big_batch_size);

    bm::RegisterBenchmark("docs_sample_table", [&twitter](bm::State& state) { docs_sample_table(state, twitter); })
        ->MinTime(min_seconds)
        ->UseRealTime()
        ->Threads(threads)
        ->Arg(small_batch_size)
        ->Arg(mid_batch_size)
        ->Arg(big_batch_size);

    if (supports_collections)
        bm::RegisterBenchmark("graph_traverse_two_hops",
                              [&twitter](bm::State& state) { graph_traverse_two_hops(state, twitter); })
            ->MinTime(min_seconds)
            ->Threads(threads)
            ->Arg(small_batch_size)
            ->Arg(mid_batch_size)
            ->Arg(big_batch_size);

    bm::RunSpecifiedBenchmarks();
    bm::Shutdown();

    if (supports_collections) {
        drop_collection(twitter.db, twitter.docs, ustore_drop_keys_vals_handle_k);
        drop_collection(twitter.db, twitter.graph, ustore_drop_keys_vals_handle_k);
    }
    drop_collection(twitter.db, ustore_collection_main_k, ustore_drop_keys_vals_k);
    ustore_database_free(twitter.db);
    return 0;
}
