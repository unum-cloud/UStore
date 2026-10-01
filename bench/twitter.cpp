/**
 *  @file bench/twitter.cpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Benchmarks document and graph workloads over real or generated tweets parsed with yyjson.
 *
 *  Beside the flags of @c bench/harness.hpp, it takes:
 *
 *  Flag                       Default                Meaning
 *  `--directory`              required               Directory the database lives in
 *  `--dataset`                generated              Directory of `.ndjson` tweet dumps
 *  `--engine`                 `ram`                  Engine to open: `ram`, `rocksdb`, `leveldb`, `nvme`
 *  `--isolation`              `strict-serializable`  Isolation of the transactions of `ram`
 *  `--engine-transport`       `automatic`            Transport of `nvme`
 *  `--threads`                half the cores         Threads every row and the indexing run on
 *  `--max-tweets`             `1000000`              Tweets to generate without a dataset
 *  `--max-input-files-total`  `1000`                 `.ndjson` files the run maps in total
 *  `--connectivity`           `4`                    Average mentions per generated tweet
 *  `--batch-sizes`            `32,64,128`            Three ascending batch sizes; building uses the last
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int64_t`, `std::uint64_t`
#include <cstdio>  // `stderr`

#include <algorithm>    // `std::min`, `std::sort`, `std::unique`
#include <array>        // `std::array`
#include <atomic>       // `std::atomic`
#include <charconv>     // `std::to_chars`
#include <filesystem>   // `std::filesystem::directory_iterator`
#include <functional>   // `std::plus`
#include <numeric>      // `std::transform_reduce`
#include <span>         // `std::span`
#include <string>       // `std::string`, what `std::filesystem::path::string` answers
#include <string_view>  // `std::string_view`
#include <system_error> // `std::error_code`
#include <utility>      // `std::move`

#include <fmt/format.h>
#include <yyjson.h>

#include <smashtable/basic_vector.hpp>
#include <stringzilla/stringzilla.hpp>

#include "ustore/blobs.h"
#include "ustore/db.h"
#include "ustore/docs.h"
#include "ustore/graph.h"

#include "harness.hpp"
#include "helpers/blocking_files.hpp"
#include "mixed.hpp"

namespace unum::ustore::bench {

#pragma region Settings

/** Half the logical cores this process may run on, at least one. */
[[nodiscard]] std::size_t half_the_cores() noexcept {
    auto const cores = executor_t::logical_cores_count();
    return cores && *cores > 1 ? *cores / 2 : 1;
}

/** Command-line options of the benchmark, each field set by the flag of the same name. */
struct settings_t : shared_settings_t {

    /** Directory of NDJSON tweet files; tweets are generated without it. */
    std::string_view dataset;

    /** Directory the database lives in. */
    std::string_view directory;

    /** The engine to open and the options of its init call. */
    engine_options_t engine;

    /** Threads every benchmark and the indexing run on. */
    count_t threads {.value = half_the_cores(), .minimum = 1};

    /** Tweets to generate without a dataset. */
    count_t max_tweets {1'000'000};

    /** Input files mapped from the dataset. */
    count_t max_input_files_total {1000};

    /** Average mentions per generated tweet. */
    count_t connectivity {4};

    /** The three sampling batch sizes, smallest first; construction takes the last. */
    counts_t batch_sizes {.values = {32, 64, 128}, .size = 3, .minimum = 1, .minimum_size = 3, .maximum_size = 3};
};

#pragma endregion Settings

#pragma region Dataset

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
    ustore_collection_id_t docs = ustore_collection_main_k;

    /** Collection of the graph between tweets and authors. */
    ustore_collection_id_t graph = ustore_collection_main_k;

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
    if (!yyjson_is_uint(id) || !yyjson_is_uint(user_id)) return true;
    ustore_key_t const tweet_key = key_from_integer(yyjson_get_uint(id));
    ustore_key_t const user_key = key_from_integer(yyjson_get_uint(user_id));

    if (st::failed(documents.push_back(keyed_document_t {tweet_key, body})) ||
        st::failed(documents.push_back(keyed_document_t {user_key, body})) ||
        st::failed(edges.push_back(graph_edge_t {tweet_key, user_key, ustore_default_edge_id_k})))
        return false;

    yyjson_val* const retweet = yyjson_obj_get(tweet, "retweeted_status");
    yyjson_val* const retweet_id = yyjson_obj_get(retweet, "id");
    yyjson_val* const retweet_user_id = yyjson_obj_get(yyjson_obj_get(retweet, "user"), "id");
    if (yyjson_is_uint(retweet_id) && yyjson_is_uint(retweet_user_id)) {
        ustore_key_t const retweet_key = key_from_integer(yyjson_get_uint(retweet_id));
        if (st::failed(edges.push_back(graph_edge_t {tweet_key, retweet_key, ustore_default_edge_id_k})) ||
            st::failed(edges.push_back(
                graph_edge_t {user_key, key_from_integer(yyjson_get_uint(retweet_user_id)), retweet_key})))
            return false;
    }

    yyjson_val* const mentions = yyjson_obj_get(yyjson_obj_get(tweet, "entities"), "user_mentions");
    yyjson_arr_iter iterator {};
    if (!yyjson_arr_iter_init(mentions, &iterator)) return true;
    for (yyjson_val* mention = yyjson_arr_iter_next(&iterator); mention; mention = yyjson_arr_iter_next(&iterator)) {
        yyjson_val* const mentioned_id = yyjson_obj_get(mention, "id");
        if (!yyjson_is_uint(mentioned_id)) return true;
        if (st::failed(
                edges.push_back(graph_edge_t {user_key, key_from_integer(yyjson_get_uint(mentioned_id)), tweet_key})))
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
    splitmix64_t random {stream_key(settings.seed.value, "tweets", 0)};
    st::basic_vector<std::int64_t> user_ids;
    st::basic_vector<std::int64_t> mentioned_user_ids;
    if (st::failed(user_ids.reserve(tweets_count))) return false;
    for (std::size_t tweet = 0; tweet != tweets_count; ++tweet) {
        std::int64_t const tweet_id = static_cast<std::int64_t>(random.next() >> 1);
        std::int64_t const user_id = static_cast<std::int64_t>(random.next() >> 1);
        std::size_t const text_length = 1 + random.below(280);
        std::size_t const mentions_count = std::min<std::size_t>(random.below(2 * settings.connectivity.value + 1),
                                                                 user_ids.size());

        mentioned_user_ids.clear();
        for (std::size_t mention = 0; mention != mentions_count; ++mention)
            if (st::failed(mentioned_user_ids.push_back(std::int64_t {user_ids[random.below(user_ids.size())]})))
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
    std::error_code error;
    std::filesystem::directory_iterator listing {std::filesystem::path {settings.dataset}, error};
    if (error) {
        fmt::println(stderr, "Failed to open {}", settings.dataset);
        return false;
    }
    std::size_t found = 0;
    for (; !error && listing != std::filesystem::directory_iterator {}; listing.increment(error)) {
        if (listing->path().extension() != ".ndjson") continue;
        ++found;
        if (twitter.files.size() == settings.max_input_files_total.value) continue;
        std::string const path = listing->path().string();
        auto [file, map_status] = mapped_file_t::map(path.c_str());
        if (map_status.failed() || st::failed(twitter.files.push_back(std::move(file)))) {
            fmt::println(stderr, "Failed to map {}", path);
            return false;
        }
    }
    if (error) {
        fmt::println(stderr, "Failed to list {}", settings.dataset);
        return false;
    }
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

/** Upserts the documents in transactional batches of @p batch_size, each thread over its own share of batches. */
void construct_docs(executor_t& executor, settings_t const& settings, twitter_t const& twitter,
                    std::size_t batch_size) {
    std::string const name = fmt::format("construct_docs/{}", batch_size);
    if (!settings.selects(name)) return;
    st::basic_vector<batch_document_t> documents;
    if (!flatten(twitter.documents, documents)) return print(row_t {.name = name, .skipped = "no memory for batches"});
    std::span<batch_document_t const> const staged {documents.begin(), documents.end()};
    print(run_pass(executor, name, staged.size() / batch_size, [&](loop_t& loop, std::size_t) noexcept {
        upsert_scratch_t scratch;
        for (std::size_t call : loop) {
            std::span<batch_document_t const> const batch = staged.subspan(call * batch_size, batch_size);
            if (!docs_upsert(twitter.db, twitter.docs, batch, scratch)) {
                loop.fail();
                continue;
            }
            std::size_t bytes = 0;
            for (batch_document_t const& document : batch) bytes += document.length;
            loop.count(batch.size(), bytes);
        }
    }));
}

/** Upserts the edges in transactional batches of @p batch_size, each thread over its own share of batches. */
void construct_graph(executor_t& executor, settings_t const& settings, twitter_t const& twitter,
                     std::size_t batch_size) {
    std::string const name = fmt::format("construct_graph/{}", batch_size);
    if (!settings.selects(name)) return;
    st::basic_vector<graph_edge_t> edges;
    if (!flatten(twitter.edges, edges)) return print(row_t {.name = name, .skipped = "no memory for batches"});
    std::span<graph_edge_t const> const staged {edges.begin(), edges.end()};
    print(run_pass(executor, name, staged.size() / batch_size, [&](loop_t& loop, std::size_t) noexcept {
        upsert_scratch_t scratch;
        for (std::size_t call : loop) {
            std::span<graph_edge_t const> const batch = staged.subspan(call * batch_size, batch_size);
            if (edges_upsert(twitter.db, twitter.graph, batch, scratch))
                loop.count(batch.size(), batch.size() * sizeof(graph_edge_t));
            else loop.fail();
        }
    }));
}

#pragma endregion Construction

#pragma region Analytics

/** Random tweet keys one thread cycles through, drawn above its loop, and the arena and error its reads reuse. */
struct tweet_batches_t {

    /** Batches a thread cycles through, enough that the ring outgrows the caches one batch would warm. */
    static constexpr std::size_t batches_count_k = 1024;

    /** @c batches_count_k batches of @c batch_size keys, back to back. */
    st::basic_vector<ustore_key_t> keys;

    /** Keys one read carries. */
    std::size_t batch_size = 0;

    /** Arena every read of this thread reuses. */
    ustore_arena_t arena = nullptr;

    /** Error slot every read of this thread reuses. */
    ustore_error_t error {};

    tweet_batches_t() noexcept = default;
    tweet_batches_t(tweet_batches_t const&) = delete;
    tweet_batches_t& operator=(tweet_batches_t const&) = delete;
    ~tweet_batches_t() noexcept { ustore_arena_free(arena); }

    /** Draws the keys of @c batches_count_k batches of @p size tweets from the stream keyed @p stream. */
    [[nodiscard]] bool draw(twitter_t const& twitter, std::size_t size, std::uint64_t stream) noexcept {
        batch_size = size;
        if (st::failed(keys.resize(batches_count_k * size))) return false;
        splitmix64_t random {stream};
        for (ustore_key_t& key : keys) {
            std::size_t part = random.below(twitter.documents.size());
            while (twitter.documents[part].size() == 0) part = (part + 1) % twitter.documents.size();
            auto const& tweets = twitter.documents[part];
            key = tweets[random.below(tweets.size())].key;
        }
        return true;
    }

    /** The batch the thread reads on @p call. */
    [[nodiscard]] ustore_key_t const* batch(std::size_t call) const noexcept {
        return keys.data() + (call % batches_count_k) * batch_size;
    }

    /** Answers whether the last read failed, clearing its error. */
    [[nodiscard]] bool failed() noexcept {
        if (!error.status) return false;
        error = {};
        return true;
    }
};

/** One sampling read: it takes the thread's batches and the call, and reports to the loop what the call did. */
using sample_read_t = void (*)(twitter_t const&, tweet_batches_t&, loop_t&, std::size_t) noexcept;

/** Samples @p read on every thread once per batch size, each thread drawing its own keys above its loop. */
void sample(executor_t& executor, settings_t const& settings, twitter_t const& twitter, std::string_view stem,
            sample_read_t read) {
    for (std::size_t const batch_size : settings.batch_sizes.view()) {
        std::string const name = fmt::format("{}/{}", stem, batch_size);
        if (!settings.selects(name)) continue;
        print(run_threads(executor, settings, name, [&](loop_t& loop, std::size_t thread) noexcept {
            tweet_batches_t batches;
            if (!batches.draw(twitter, batch_size, stream_key(settings.seed.value, name, thread)))
                return loop.skip("no memory for keys");
            for (std::size_t call : loop) read(twitter, batches, loop, call);
        }));
    }
}

/** Samples batches of tweet keys, reading only the keys the sampler returns. */
void docs_sample_keys(twitter_t const& twitter, tweet_batches_t& batches, loop_t& loop, std::size_t) noexcept {
    ustore_length_t const limit = 1;
    ustore_length_t* found_offsets = nullptr;
    ustore_length_t* found_counts = nullptr;
    ustore_key_t* found_keys = nullptr;
    ustore_sample_t sample {};
    sample.db = twitter.db;
    sample.error = &batches.error;
    sample.arena = &batches.arena;
    sample.tasks_count = batches.batch_size;
    sample.collections = &twitter.docs;
    sample.count_limits = &limit;
    sample.count_limits_stride = 0;
    sample.offsets = &found_offsets;
    sample.counts = &found_counts;
    sample.keys = &found_keys;
    ustore_sample(&sample);
    if (batches.failed()) return loop.fail();
    loop.count(batches.batch_size, found_offsets[batches.batch_size]);
}

/** Reads whole tweets as JSON, which parses and serializes every document. */
void docs_sample_objects(twitter_t const& twitter, tweet_batches_t& batches, loop_t& loop, std::size_t call) noexcept {
    ustore_length_t* offsets = nullptr;
    ustore_bytes_ptr_t values = nullptr;
    ustore_docs_read_t read {};
    read.db = twitter.db;
    read.error = &batches.error;
    read.arena = &batches.arena;
    read.type = ustore_doc_field_json_k;
    read.tasks_count = batches.batch_size;
    read.collections = &twitter.docs;
    read.keys = batches.batch(call);
    read.keys_stride = sizeof(ustore_key_t);
    read.offsets = &offsets;
    read.values = &values;
    ustore_docs_read(&read);
    if (batches.failed()) return loop.fail();
    loop.count(batches.batch_size, offsets[batches.batch_size]);
}

/** Samples batches of tweets, reading one field out of each document. */
void docs_sample_field(twitter_t const& twitter, tweet_batches_t& batches, loop_t& loop, std::size_t call) noexcept {
    ustore_str_view_t const field = "text";
    ustore_length_t* offsets = nullptr;
    ustore_bytes_ptr_t values = nullptr;
    ustore_docs_read_t read {};
    read.db = twitter.db;
    read.error = &batches.error;
    read.arena = &batches.arena;
    read.type = ustore_doc_field_str_k;
    read.tasks_count = batches.batch_size;
    read.collections = &twitter.docs;
    read.keys = batches.batch(call);
    read.keys_stride = sizeof(ustore_key_t);
    read.fields = &field;
    read.offsets = &offsets;
    read.values = &values;
    ustore_docs_read(&read);
    if (batches.failed()) return loop.fail();
    loop.count(batches.batch_size, offsets[batches.batch_size]);
}

/** Samples batches of tweets, gathering four typed fields of each into a table. */
void docs_sample_table(twitter_t const& twitter, tweet_batches_t& batches, loop_t& loop, std::size_t call) noexcept {
    constexpr std::size_t fields_count_k = 4;
    ustore_str_view_t const names[fields_count_k] {"timestamp_ms", "reply_count", "retweet_count", "favorite_count"};
    ustore_doc_field_type_t const types[fields_count_k] {
        ustore_doc_field_str_k,
        ustore_doc_field_u32_k,
        ustore_doc_field_u32_k,
        ustore_doc_field_u32_k,
    };

    std::size_t const count = batches.batch_size;
    ustore_octet_t** validities = nullptr;
    ustore_byte_t** scalars = nullptr;
    ustore_length_t** offsets = nullptr;
    ustore_length_t** lengths = nullptr;
    ustore_byte_t* strings = nullptr;
    ustore_docs_gather_t gather {};
    gather.db = twitter.db;
    gather.error = &batches.error;
    gather.arena = &batches.arena;
    gather.docs_count = count;
    gather.fields_count = fields_count_k;
    gather.collections = &twitter.docs;
    gather.keys = batches.batch(call);
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
    if (batches.failed()) return loop.fail();

    // The first column holds strings, the other three hold 32-bit scalars.
    std::size_t const string_bytes = std::transform_reduce(
        lengths[0], lengths[0] + count, std::size_t {0}, std::plus<std::size_t> {},
        [](ustore_length_t length) noexcept { return length == ustore_length_missing_k ? 0u : length; });
    loop.count(count, string_bytes + (fields_count_k - 1) * sizeof(std::uint32_t) * count);
}

/** Sums the @p count degrees, counting a missing vertex as zero. */
[[nodiscard]] std::size_t sum_degrees(ustore_vertex_degree_t const* degrees, std::size_t count) noexcept {
    return std::transform_reduce(
        degrees, degrees + count, std::size_t {0}, std::plus<std::size_t> {},
        [](ustore_vertex_degree_t degree) noexcept { return degree != ustore_vertex_degree_missing_k ? degree : 0u; });
}

/** Gathers the edges of sampled tweets, then the edges of every vertex those edges reach. */
void graph_traverse_two_hops(twitter_t const& twitter, tweet_batches_t& batches, loop_t& loop,
                             std::size_t call) noexcept {
    ustore_vertex_role_t const role = ustore_vertex_role_any_k;
    std::size_t const count = batches.batch_size;
    ustore_vertex_degree_t* degrees = nullptr;
    ustore_key_t* ids_in_edges = nullptr;
    ustore_graph_find_edges_t first_hop {};
    first_hop.db = twitter.db;
    first_hop.error = &batches.error;
    first_hop.arena = &batches.arena;
    first_hop.tasks_count = count;
    first_hop.collections = &twitter.graph;
    first_hop.vertices = batches.batch(call);
    first_hop.vertices_stride = sizeof(ustore_key_t);
    first_hop.roles = &role;
    first_hop.degrees_per_vertex = &degrees;
    first_hop.edges_per_vertex = &ids_in_edges;
    ustore_graph_find_edges(&first_hop);
    if (batches.failed()) return loop.fail();

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
    if (batches.failed()) return loop.fail();

    total_edges += sum_degrees(degrees, unique_count);
    loop.count(total_edges, total_edges * 3 * sizeof(ustore_key_t));
}

#pragma endregion Analytics

#pragma region Database

/** Creates a collection holding @p modality into @p id, reporting a failure against @p name on the standard error. */
[[nodiscard]] bool create_collection(ustore_database_t db, char const* name, ustore_modality_t modality,
                                     ustore_collection_id_t& id) noexcept {
    ustore_error_t error {};
    ustore_collection_create_t create {};
    create.db = db;
    create.error = &error;
    create.modality = modality;
    create.collection = &id;
    ustore_collection_create(&create);
    if (!error.status) return true;
    fmt::println(stderr, "Failed to create {}: {}", name, error.message);
    return false;
}

/** Drops @p id as @p mode asks, reporting a failure on the standard error. */
void drop_collection(ustore_database_t db, ustore_collection_id_t id, ustore_drop_mode_t mode) noexcept {
    ustore_error_t error {};
    ustore_collection_drop_t drop {};
    drop.db = db;
    drop.error = &error;
    drop.collection = id;
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
    std::array<flag_t, 10> const flags {{
        {"--dataset", "path", "directory of .ndjson tweet files; tweets are generated without it", &settings.dataset},
        {"--directory", "path", "database directory, required", &settings.directory},
        {"--engine", "name", "engine to open: ram, rocksdb, leveldb or nvme", &settings.engine.name},
        {"--isolation", "name", "isolation of the transactions of ram", &settings.engine.isolation},
        {"--engine-transport", "name", "transport of nvme", &settings.engine.transport},
        {"--threads", "count", "threads every benchmark runs on", &settings.threads},
        {"--max-tweets", "count", "tweets to generate without a dataset", &settings.max_tweets},
        {"--max-input-files-total", "count", "input files mapped from the dataset", &settings.max_input_files_total},
        {"--connectivity", "count", "average mentions per generated tweet", &settings.connectivity},
        {"--batch-sizes", "counts", "sampling batch sizes, the last also for construction", &settings.batch_sizes},
    }};
    read_settings(argc, argv, settings, flags);
    if (settings.directory.empty()) {
        fmt::println(stderr, "--directory is required");
        return 1;
    }

    auto maybe_executor = executor_t::make(settings.threads.value, caller_exclusivity_t::caller_inclusive_k);
    if (!maybe_executor) {
        fmt::println(stderr, "Failed to spawn {} threads", settings.threads.value);
        return 1;
    }
    executor_t& executor = *maybe_executor;
    twitter_t twitter;
    bool const indexed = settings.dataset.empty() ? generate_dataset(settings, twitter)
                                                  : load_dataset(settings, executor, twitter);
    if (!indexed) {
        fmt::println(stderr, "Failed to prepare the dataset");
        return 1;
    }
    fmt::println("- indexed {} docs", pass_through_size(twitter.documents));
    fmt::println("- indexed {} relations", pass_through_size(twitter.edges));
    if (pass_through_size(twitter.documents) == 0) {
        fmt::println(stderr, "No tweets to sample");
        return 1;
    }

    ustore_error_t error {};
    settings.engine.directory = settings.directory.data();
    open_engine(settings.engine, &twitter.db, &error);
    if (error.status) {
        fmt::println(stderr, "Failed to open the database: {}", error.message);
        return 1;
    }

    ustore_capabilities_t capabilities {};
    ustore_database_capabilities_t request {};
    request.db = twitter.db;
    request.error = &error;
    request.capabilities = &capabilities;
    ustore_database_capabilities(&request);
    if (error.status) {
        fmt::println(stderr, "Failed to read the database capabilities: {}", error.message);
        ustore_database_free(twitter.db);
        return 1;
    }

    bool const supports_collections = (capabilities & ustore_supports_collections_k) != 0;
    bool const supports_transactions = (capabilities & ustore_supports_transactions_k) != 0;
    if (supports_collections &&
        (!create_collection(twitter.db, "twitter.docs", ustore_modality_docs_k, twitter.docs) ||
         !create_collection(twitter.db, "twitter.graph", ustore_modality_graph_k, twitter.graph))) {
        ustore_database_free(twitter.db);
        return 1;
    }

    fmt::println("Will benchmark...");
    std::size_t const construction_batch_size = settings.batch_sizes.values[2];
    if (supports_transactions) construct_docs(executor, settings, twitter, construction_batch_size);
    if (supports_collections) construct_graph(executor, settings, twitter, construction_batch_size);
    sample(executor, settings, twitter, "docs_sample_keys", docs_sample_keys);
    sample(executor, settings, twitter, "docs_sample_objects", docs_sample_objects);
    sample(executor, settings, twitter, "docs_sample_field", docs_sample_field);
    sample(executor, settings, twitter, "docs_sample_table", docs_sample_table);
    if (supports_collections) sample(executor, settings, twitter, "graph_traverse_two_hops", graph_traverse_two_hops);

    if (supports_collections) {
        drop_collection(twitter.db, twitter.docs, ustore_drop_keys_vals_handle_k);
        drop_collection(twitter.db, twitter.graph, ustore_drop_keys_vals_handle_k);
    }
    drop_collection(twitter.db, ustore_collection_main_k, ustore_drop_keys_vals_k);
    ustore_database_free(twitter.db);
    return 0;
}
