/**
 * @file unit.cpp
 * @author Ashot Vardanian
 * @date 2022-07-06
 *
 * @brief A set of unit tests implemented using Google Test.
 */

#include <vector>
#include <unordered_set>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "ukv/ukv.hpp"

using namespace unum::ukv;
using namespace unum;

template <typename container_at>
char const* str_begin(container_at const& container) {
    if constexpr (std::is_same_v<char const*, container_at>)
        return container;
    else if constexpr (std::is_same_v<value_view_t, container_at>)
        return reinterpret_cast<char const*>(container.begin());
    else
        return reinterpret_cast<char const*>(std::data(container));
}

template <typename container_at>
char const* str_end(container_at const& container) {
    if constexpr (std::is_same_v<char const*, container_at>)
        return container + std::strlen(container);
    else if constexpr (std::is_same_v<value_view_t, container_at>)
        return reinterpret_cast<char const*>(container.end());
    else
        return str_begin(container) + std::size(container);
}

using json_t = nlohmann::json;

static json_t json_parse(char const* begin, char const* end) {
    json_t result;
    auto adapter = nlohmann::detail::input_adapter(begin, end);
    auto parser = nlohmann::detail::parser<json_t, decltype(adapter)>(std::move(adapter), nullptr, true, true);
    parser.parse(false, result);
    return result;
    // return json_t::parse(begin, end, nullptr, true, true);
}

#define M_EXPECT_EQ_JSON(str1, str2) \
    EXPECT_EQ(json_parse(str_begin(str1), str_end(str1)), json_parse(str_begin(str2), str_end(str2)));
#define M_EXPECT_EQ_MSG(str1, str2) \
    EXPECT_EQ(json_t::from_msgpack(str_begin(str1), str_end(str1)), json_parse(str_begin(str2), str_end(str2)));

static char const* path() {
    char* path = std::getenv("UKV_BACKEND_PATH");
    if (path)
        return path;

#if defined(UKV_FLIGHT_CLIENT)
    return nullptr;
#elif defined(UKV_TEST_PATH)
    return UKV_TEST_PATH;
#else
    return nullptr;
#endif
}

#pragma region Binary Collections

template <typename locations_at>
void check_length(bins_ref_gt<locations_at>& ref, ukv_length_t expected_length) {

    EXPECT_TRUE(ref.value()) << "Failed to fetch missing keys";

    auto const expects_missing = expected_length == ukv_length_missing_k;
    using extractor_t = places_arg_extractor_gt<locations_at>;
    ukv_size_t count = extractor_t {}.count(ref.locations());

    // Validate that values match
    auto maybe_retrieved = ref.value();
    auto const& retrieved = *maybe_retrieved;
    EXPECT_EQ(retrieved.size(), count);

    // Check views
    auto it = retrieved.begin();
    for (std::size_t i = 0; i != count; ++i, ++it) {
        EXPECT_EQ((*it).size(), expects_missing ? 0 : expected_length);
    }

    // Check length estimates
    auto maybe_lengths = ref.length();
    EXPECT_TRUE(maybe_lengths);
    for (std::size_t i = 0; i != count; ++i) {
        EXPECT_EQ(maybe_lengths->at(i), expected_length);
    }

    // Check boolean indicators
    auto maybe_indicators = ref.present();
    EXPECT_TRUE(maybe_indicators);
    for (std::size_t i = 0; i != count; ++i) {
        EXPECT_EQ(maybe_indicators->at(i), !expects_missing);
    }
}

template <typename locations_at>
void check_equalities(bins_ref_gt<locations_at>& ref, contents_arg_t values) {

    EXPECT_TRUE(ref.value()) << "Failed to fetch present keys";
    using extractor_t = places_arg_extractor_gt<locations_at>;

    // Validate that values match
    auto maybe_retrieved = ref.value();
    auto const& retrieved = *maybe_retrieved;
    EXPECT_EQ(retrieved.size(), extractor_t {}.count(ref.locations()));

    auto it = retrieved.begin();
    for (std::size_t i = 0; i != extractor_t {}.count(ref.locations()); ++i, ++it) {
        auto expected_len = static_cast<std::size_t>(values.lengths_begin[i]);
        auto expected_begin = reinterpret_cast<byte_t const*>(values.contents_begin[i]) + values.offsets_begin[i];

        value_view_t val_view = *it;
        value_view_t expected_view(expected_begin, expected_begin + expected_len);
        EXPECT_EQ(val_view.size(), expected_len);
        EXPECT_EQ(val_view, expected_view);
    }
}

template <typename locations_at>
void round_trip(bins_ref_gt<locations_at>& ref, contents_arg_t values) {
    EXPECT_TRUE(ref.assign(values)) << "Failed to assign";
    check_equalities(ref, values);
}

void check_binary_collection(bins_collection_t& collection) {
    std::vector<ukv_key_t> keys {34, 35, 36};
    ukv_length_t val_len = sizeof(std::uint64_t);
    std::vector<std::uint64_t> vals {34, 35, 36};
    std::vector<ukv_length_t> offs {0, val_len, val_len * 2};
    auto vals_begin = reinterpret_cast<ukv_bytes_ptr_t>(vals.data());

    auto ref = collection[keys];
    contents_arg_t values {
        .offsets_begin = {offs.data(), sizeof(ukv_length_t)},
        .lengths_begin = {&val_len, 0},
        .contents_begin = {&vals_begin, 0},
        .count = 3,
    };
    round_trip(ref, values);

    // Overwrite those values with same size integers and try again
    for (auto& val : vals)
        val += 100;
    round_trip(ref, values);

    // Overwrite with empty values, but check for existence
    EXPECT_TRUE(ref.clear());
    check_length(ref, 0);

    // Check scans
    keys_range_t present_keys = collection.keys();
    keys_stream_t present_it = present_keys.begin();
    auto expected_it = keys.begin();
    for (; expected_it != keys.end(); ++present_it, ++expected_it) {
        EXPECT_EQ(*expected_it, *present_it);
    }
    EXPECT_TRUE(present_it.is_end());

    // Remove all of the values and check that they are missing
    EXPECT_TRUE(ref.erase());
    check_length(ref, ukv_length_missing_k);
}

TEST(db, basic) {

    database_t db;
    EXPECT_TRUE(db.open(path()));

    // Try getting the main collection
    EXPECT_TRUE(db.collection());
    bins_collection_t collection = *db.collection();
    check_binary_collection(collection);
    EXPECT_TRUE(db.clear());
}

TEST(db, consistency) {

    database_t db;
    EXPECT_TRUE(db.open(path()));

    std::vector<ukv_key_t> keys {54, 55, 56};
    ukv_length_t val_len = sizeof(std::uint64_t);
    std::vector<std::uint64_t> vals {1, 2, 3};
    std::vector<ukv_length_t> offs {0, val_len, val_len * 2};
    auto vals_begin = reinterpret_cast<ukv_bytes_ptr_t>(vals.data());

    contents_arg_t values {
        .offsets_begin = {offs.data(), sizeof(ukv_length_t)},
        .lengths_begin = {&val_len, 0},
        .contents_begin = {&vals_begin, 0},
        .count = 3,
    };

    bins_collection_t collection = *db.collection();
    auto collection_ref = collection[keys];
    check_length(collection_ref, ukv_length_missing_k);
    round_trip(collection_ref, values);
    check_length(collection_ref, 8);
    db.close();

    EXPECT_TRUE(db.open(path()));
    bins_collection_t collection2 = *db.collection();
    auto collection_ref2 = collection2[keys];

    check_equalities(collection_ref2, values);
    check_length(collection_ref2, 8);

    EXPECT_TRUE(db.clear());
}

TEST(db, named) {

    if (!ukv_supports_named_collections_k)
        return;

    database_t db;
    EXPECT_TRUE(db.open(path()));

    EXPECT_TRUE(db["col1"]);
    EXPECT_TRUE(db["col2"]);

    bins_collection_t col1 = *db.add_collection("col1");
    bins_collection_t col2 = *db.add_collection("col2");

    check_binary_collection(col1);
    check_binary_collection(col2);

    EXPECT_TRUE(db.drop("col1"));
    EXPECT_TRUE(db.drop("col2"));
    EXPECT_TRUE(*db.contains(""));
    EXPECT_FALSE(*db.contains("col1"));
    EXPECT_FALSE(*db.contains("col2"));
    EXPECT_TRUE(db.clear());
    EXPECT_TRUE(*db.contains(""));
}

TEST(db, collection_list) {

    database_t db;

    EXPECT_TRUE(db.open(path()));

    if (!ukv_supports_named_collections_k) {
        EXPECT_FALSE(*db["name"]);
        EXPECT_FALSE(*db.contains("name"));
        EXPECT_FALSE(db.drop("name"));
        EXPECT_FALSE(db.drop(""));
        EXPECT_TRUE(db.collection()->clear());
        return;
    }
    else {
        bins_collection_t col1 = *db.add_collection("col1");
        bins_collection_t col2 = *db.add_collection("col2");
        bins_collection_t col3 = *db.add_collection("col3");
        bins_collection_t col4 = *db.add_collection("col4");

        EXPECT_TRUE(*db.contains("col1"));
        EXPECT_TRUE(*db.contains("col2"));
        EXPECT_FALSE(*db.contains("unknown_col"));

        auto maybe_txn = db.transact();
        EXPECT_TRUE(maybe_txn);
        auto maybe_cols = maybe_txn->collections();
        EXPECT_TRUE(maybe_cols);

        size_t count = 0;
        std::vector<std::string> collections;
        auto cols = *maybe_cols;
        while (!cols.names.is_end()) {
            collections.push_back(std::string(*cols.names));
            ++cols.names;
            ++count;
        }
        EXPECT_EQ(count, 4);
        std::sort(collections.begin(), collections.end());
        EXPECT_EQ(collections[0], "col1");
        EXPECT_EQ(collections[1], "col2");
        EXPECT_EQ(collections[2], "col3");
        EXPECT_EQ(collections[3], "col4");

        EXPECT_TRUE(db.drop("col1"));
        EXPECT_FALSE(*db.contains("col1"));
        EXPECT_FALSE(db.drop(""));
        EXPECT_TRUE(db.collection()->clear());
    }
    EXPECT_TRUE(db.clear());
}

TEST(db, paths) {

    database_t db;
    EXPECT_TRUE(db.open(path()));

    char const* keys[] {"Facebook", "Apple", "Amazon", "Netflix", "Google", "Nvidia", "Adobe"};
    char const* vals[] {"F", "A", "A", "N", "G", "N", "A"};
    std::size_t keys_count = sizeof(keys) / sizeof(keys[0]);
    ukv_char_t separator = '\0';

    arena_t arena(db);
    status_t status;
    ukv_paths_write_t paths_write {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .tasks_count = keys_count,
        .path_separator = separator,
        .paths = keys,
        .paths_stride = sizeof(char const*),
        .values_bytes = reinterpret_cast<ukv_bytes_cptr_t*>(vals),
        .values_bytes_stride = sizeof(char const*),
    };
    ukv_paths_write(&paths_write);
    char* vals_recovered = nullptr;
    ukv_paths_read_t paths_read {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .tasks_count = keys_count,
        .path_separator = separator,
        .paths = keys,
        .paths_stride = sizeof(char const*),
        .values = reinterpret_cast<ukv_bytes_ptr_t*>(&vals_recovered),
    };
    ukv_paths_read(&paths_read);
    EXPECT_TRUE(status);
    EXPECT_EQ(std::string_view(vals_recovered, keys_count * 2),
              std::string_view("F\0A\0A\0N\0G\0N\0A\0", keys_count * 2));

    // Try getting either "Netflix" or "Nvidia" as one of the keys with "N" prefix
    ukv_str_view_t prefix = "N";
    ukv_length_t max_count = 1;
    ukv_length_t* results_counts = nullptr;
    ukv_length_t* tape_offsets = nullptr;
    ukv_char_t* tape_begin = nullptr;
    ukv_paths_match_t paths_match {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .patterns = &prefix,
        .match_counts_limits = &max_count,
        .match_counts = &results_counts,
        .paths_offsets = &tape_offsets,
        .paths_strings = &tape_begin,
    };
    ukv_paths_match(&paths_match);
    auto first_match_for_a = std::string_view(tape_begin);
    EXPECT_EQ(results_counts[0], 1);
    EXPECT_TRUE(first_match_for_a == "Netflix" || first_match_for_a == "Nvidia");

    // Try getting the remaining results, which is the other one from that same pair
    max_count = 10;
    paths_match.previous = &tape_begin;
    ukv_paths_match(&paths_match);
    auto second_match_for_a = std::string_view(tape_begin);
    EXPECT_EQ(results_counts[0], 1);
    EXPECT_TRUE(second_match_for_a == "Netflix" || second_match_for_a == "Nvidia");
    EXPECT_NE(first_match_for_a, second_match_for_a);

    // Try performing parallel queries in the same collection
    ukv_str_view_t prefixes[2] = {"A", "N"};
    std::size_t prefixes_count = sizeof(prefixes) / sizeof(prefixes[0]);
    max_count = 10;
    paths_match.tasks_count = prefixes_count;
    paths_match.patterns = prefixes;
    paths_match.patterns_stride = sizeof(ukv_str_view_t);
    paths_match.previous = nullptr;
    ukv_paths_match(&paths_match);
    auto total_count = std::accumulate(results_counts, results_counts + prefixes_count, 0ul);
    strings_tape_iterator_t tape_iterator {total_count, tape_begin};
    std::set<std::string> tape_parts;
    while (!tape_iterator.is_end()) {
        tape_parts.insert(*tape_iterator);
        ++tape_iterator;
    }
    EXPECT_EQ(results_counts[0], 3);
    EXPECT_EQ(results_counts[1], 2);
    EXPECT_NE(tape_parts.find("Netflix"), tape_parts.end());
    EXPECT_NE(tape_parts.find("Adobe"), tape_parts.end());

    // Now try matching a Regular Expression
    prefix = "Netflix|Google";
    max_count = 20;
    paths_match.tasks_count = 1;
    paths_match.patterns = &prefix;
    ukv_paths_match(&paths_match);
    first_match_for_a = std::string_view(tape_begin);
    second_match_for_a = std::string_view(tape_begin + tape_offsets[1]);
    EXPECT_EQ(results_counts[0], 2);
    EXPECT_TRUE(first_match_for_a == "Netflix" || first_match_for_a == "Google");
    EXPECT_TRUE(second_match_for_a == "Netflix" || second_match_for_a == "Google");

    // Try a more complex regular expression
    prefix = "A.*e";
    max_count = 20;
    ukv_paths_match(&paths_match);
    first_match_for_a = std::string_view(tape_begin);
    second_match_for_a = std::string_view(tape_begin + tape_offsets[1]);
    EXPECT_EQ(results_counts[0], 2);
    EXPECT_TRUE(first_match_for_a == "Apple" || first_match_for_a == "Adobe");
    EXPECT_TRUE(second_match_for_a == "Apple" || second_match_for_a == "Adobe");

    EXPECT_TRUE(db.clear());
}

TEST(db, paths_linked_list) {

    constexpr std::size_t count = 500;
    database_t db;
    EXPECT_TRUE(db.open(path()));

    arena_t arena(db);
    ukv_char_t separator = '\0';
    status_t status;

    ukv_paths_write_t paths_write {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .path_separator = separator,
    };

    ukv_paths_read_t paths_read {
        .db = db,
        .error = status.member_ptr(),
        .arena = arena.member_ptr(),
        .path_separator = separator,
        // .paths = keys,
        // .paths_stride = sizeof(char const*),
        // .values = reinterpret_cast<ukv_bytes_ptr_t*>(&vals_recovered),
    };

    // Generate some random strings for our tests
    constexpr auto alphabet = "abcdefghijklmnop";
    auto make_random_str = []() {
        auto str = std::string();
        auto len = static_cast<std::size_t>(std::rand() % 100) + 8;
        for (std::size_t i = 0; i != len; ++i)
            str.push_back(alphabet[std::rand() % 16]);
        return str;
    };
    std::set<std::string> unique;
    while (unique.size() != count)
        unique.insert(make_random_str());

    // Lets form a linked list, where every key maps into the the next key.
    // Then we will traverse the linked list from start to end.
    // Then we will re-link it in reverse order and traverse again.
    std::vector<ukv_str_view_t> begins(unique.size());
    std::transform(unique.begin(), unique.end(), begins.begin(), [](std::string const& str) { return str.c_str(); });

    // Link forward
    for (std::size_t i = 0; i + 1 != begins.size(); ++i) {
        ukv_str_view_t smaller = begins[i];
        ukv_str_view_t bigger = begins[i + 1];
        paths_write.paths = &smaller;
        paths_write.values_bytes = reinterpret_cast<ukv_bytes_cptr_t*>(&bigger);
        ukv_paths_write(&paths_write);
        EXPECT_TRUE(status);

        // Check if it was successfully written:
        ukv_str_span_t bigger_received = nullptr;
        paths_read.paths = &smaller;
        paths_read.values = reinterpret_cast<ukv_bytes_ptr_t*>(&bigger_received);
        ukv_paths_read(&paths_read);
        EXPECT_TRUE(status);
        EXPECT_EQ(std::string_view(bigger), std::string_view(bigger_received));
    }

    // Traverse forward, counting the entries and checking the order
    for (std::size_t i = 0; i + 1 != begins.size(); ++i) {
        ukv_str_view_t smaller = begins[i];
        ukv_str_view_t bigger = begins[i + 1];
        ukv_str_span_t bigger_received = nullptr;
        paths_read.paths = &smaller;
        paths_read.values = reinterpret_cast<ukv_bytes_ptr_t*>(&bigger_received);
        ukv_paths_read(&paths_read);
        EXPECT_TRUE(status);
        EXPECT_EQ(std::string_view(bigger), std::string_view(bigger_received));
    }

    // Re-link in reverse order
    for (std::size_t i = 0; i + 1 != begins.size(); ++i) {
        ukv_str_view_t smaller = begins[i];
        ukv_str_view_t bigger = begins[i + 1];
        paths_write.paths = &smaller;
        paths_write.values_bytes = reinterpret_cast<ukv_bytes_cptr_t*>(&bigger);
        ukv_paths_write(&paths_write);
        EXPECT_TRUE(status);

        // Check if it was successfully over-written:
        ukv_str_span_t smaller_received = nullptr;
        paths_read.paths = &bigger;
        paths_read.values = reinterpret_cast<ukv_bytes_ptr_t*>(&smaller_received);
        ukv_paths_read(&paths_read);
        EXPECT_TRUE(status);
        EXPECT_EQ(std::string_view(smaller), std::string_view(smaller_received));
    }

    // Traverse backwards, counting the entries and checking the order
    for (std::size_t i = 0; i + 1 != begins.size(); ++i) {
        ukv_str_view_t smaller = begins[i];
        ukv_str_view_t bigger = begins[i + 1];
        ukv_str_span_t smaller_received = nullptr;
        paths_read.paths = &bigger;
        paths_read.values = reinterpret_cast<ukv_bytes_ptr_t*>(&smaller_received);
        ukv_paths_read(&paths_read);
        EXPECT_TRUE(status);
        EXPECT_EQ(std::string_view(smaller), std::string_view(smaller_received));
    }
}

TEST(db, unnamed_and_named) {

    if (!ukv_supports_named_collections_k)
        return;

    database_t db;
    EXPECT_TRUE(db.open(path()));

    std::vector<ukv_key_t> keys {54, 55, 56};
    ukv_length_t val_len = sizeof(std::uint64_t);
    std::vector<std::uint64_t> vals {1, 2, 3};
    std::vector<ukv_length_t> offs {0, val_len, val_len * 2};
    auto vals_begin = reinterpret_cast<ukv_bytes_ptr_t>(vals.data());

    contents_arg_t values {
        .offsets_begin = {offs.data(), sizeof(ukv_length_t)},
        .lengths_begin = {&val_len, 0},
        .contents_begin = {&vals_begin, 0},
        .count = 3,
    };

    for (auto&& name : {"one", "", "three"}) {
        for (auto& val : vals)
            val += 7;

        bins_collection_t collection = *db.add_collection(name);
        if (name == "") {
            EXPECT_FALSE(collection);
            continue;
        }
        EXPECT_TRUE(collection);

        auto collection_ref = collection[keys];
        check_length(collection_ref, ukv_length_missing_k);
        round_trip(collection_ref, values);
        check_length(collection_ref, 8);
    }
    EXPECT_TRUE(db.clear());
}

TEST(db, txn) {

    if (!ukv_supports_transactions_k)
        return;

    database_t db;
    EXPECT_TRUE(db.open(path()));
    EXPECT_TRUE(db.transact());
    transaction_t txn = *db.transact();

    std::vector<ukv_key_t> keys {54, 55, 56};
    ukv_length_t val_len = sizeof(std::uint64_t);
    std::vector<std::uint64_t> vals {54, 55, 56};
    std::vector<ukv_length_t> offs {0, val_len, val_len * 2};
    auto vals_begin = reinterpret_cast<ukv_bytes_ptr_t>(vals.data());

    contents_arg_t values {
        .offsets_begin = {offs.data(), sizeof(ukv_length_t)},
        .lengths_begin = {&val_len, 0},
        .contents_begin = {&vals_begin, 0},
        .count = 3,
    };

    auto txn_ref = txn[keys];
    round_trip(txn_ref, values);

    EXPECT_TRUE(db.collection());
    bins_collection_t collection = *db.collection();
    auto collection_ref = collection[keys];

    // Check for missing values before commit
    check_length(collection_ref, ukv_length_missing_k);

    auto status = txn.commit();
    status.throw_unhandled();
    status = txn.reset();
    status.throw_unhandled();

    // Validate that values match after commit
    check_equalities(collection_ref, values);
    EXPECT_TRUE(db.clear());
}

TEST(db, txn_named) {

    if (!ukv_supports_transactions_k)
        return;
    if (!ukv_supports_named_collections_k)
        return;

    database_t db;
    EXPECT_TRUE(db.open(path()));
    EXPECT_TRUE(db.transact());
    transaction_t txn = *db.transact();

    std::vector<ukv_key_t> keys {54, 55, 56};
    ukv_length_t val_len = sizeof(std::uint64_t);
    std::vector<std::uint64_t> vals {54, 55, 56};
    std::vector<ukv_length_t> offs {0, val_len, val_len * 2};
    auto vals_begin = reinterpret_cast<ukv_bytes_ptr_t>(vals.data());

    contents_arg_t values {
        .offsets_begin = {offs.data(), sizeof(ukv_length_t)},
        .lengths_begin = {&val_len, 0},
        .contents_begin = {&vals_begin, 0},
        .count = 3,
    };

    // Transaction with named collection
    EXPECT_TRUE(db.collection("named_col"));
    bins_collection_t named_collection = *db.collection("named_col");
    std::vector<collection_key_t> sub_keys {{named_collection, 54}, {named_collection, 55}, {named_collection, 56}};
    auto txn_named_collection_ref = txn[sub_keys];
    round_trip(txn_named_collection_ref, values);

    // Check for missing values before commit
    auto named_collection_ref = named_collection[keys];
    check_length(named_collection_ref, ukv_length_missing_k);

    auto status = txn.commit();
    status.throw_unhandled();
    status = txn.reset();
    status.throw_unhandled();

    // Validate that values match after commit
    check_equalities(named_collection_ref, values);
    EXPECT_TRUE(db.clear());
}

TEST(db, txn_unnamed_then_named) {

    if (!ukv_supports_transactions_k)
        return;
    if (!ukv_supports_named_collections_k)
        return;

    database_t db;
    EXPECT_TRUE(db.open(path()));

    EXPECT_TRUE(db.transact());
    transaction_t txn = *db.transact();

    std::vector<ukv_key_t> keys {54, 55, 56};
    ukv_length_t val_len = sizeof(std::uint64_t);
    std::vector<std::uint64_t> vals {54, 55, 56};
    std::vector<ukv_length_t> offs {0, val_len, val_len * 2};
    auto vals_begin = reinterpret_cast<ukv_bytes_ptr_t>(vals.data());

    contents_arg_t values {
        .offsets_begin = {offs.data(), sizeof(ukv_length_t)},
        .lengths_begin = {&val_len, 0},
        .contents_begin = {&vals_begin, 0},
        .count = 3,
    };

    auto txn_ref = txn[keys];
    round_trip(txn_ref, values);

    EXPECT_TRUE(db.collection());
    bins_collection_t collection = *db.collection();
    auto collection_ref = collection[keys];

    // Check for missing values before commit
    check_length(collection_ref, ukv_length_missing_k);

    auto status = txn.commit();
    status.throw_unhandled();
    status = txn.reset();
    status.throw_unhandled();

    // Validate that values match after commit
    check_equalities(collection_ref, values);

    // Transaction with named collection
    EXPECT_TRUE(db.add_collection("named_col"));
    bins_collection_t named_collection = *db.collection("named_col");
    std::vector<collection_key_t> sub_keys {{named_collection, 54}, {named_collection, 55}, {named_collection, 56}};
    auto txn_named_collection_ref = txn[sub_keys];
    round_trip(txn_named_collection_ref, values);

    // Check for missing values before commit
    auto named_collection_ref = named_collection[keys];
    check_length(named_collection_ref, ukv_length_missing_k);

    status = txn.commit();
    status.throw_unhandled();
    status = txn.reset();
    status.throw_unhandled();

    // Validate that values match after commit
    check_equalities(named_collection_ref, values);
    EXPECT_TRUE(db.clear());
}

#pragma region Document Collections

TEST(db, docs) {

    database_t db;
    EXPECT_TRUE(db.open(path()));

    // JSON
    docs_collection_t collection = *db.collection<docs_collection_t>();
    auto json = R"( {"person": "Carl", "age": 24} )"_json.dump();
    collection[1] = json.c_str();
    M_EXPECT_EQ_JSON(*collection[1].value(), json);
    M_EXPECT_EQ_JSON(*collection[ckf(1, "person")].value(), "\"Carl\"");
    M_EXPECT_EQ_JSON(*collection[ckf(1, "age")].value(), "24");

    // Binary
    auto maybe_person = collection[ckf(1, "person")].value(ukv_doc_field_str_k);
    EXPECT_EQ(std::string_view(maybe_person->c_str(), maybe_person->size()), std::string_view("Carl"));

#if 0
    // MsgPack
    collection.as(ukv_format_msgpack_k);
    value_view_t val = *collection[1].value();
    M_EXPECT_EQ_MSG(val, json.c_str());
    val = *collection[ckf(1, "person")].value();
    M_EXPECT_EQ_MSG(val, "\"Carl\"");
    val = *collection[ckf(1, "age")].value();
    M_EXPECT_EQ_MSG(val, "24");
#endif

    EXPECT_TRUE(db.clear());
}

TEST(db, docs_modify) {
    database_t db;
    EXPECT_TRUE(db.open(path()));
    docs_collection_t collection = *db.collection<docs_collection_t>();

    auto json = R"( { "a": {"b": "c","0":{"b":[{"1":"2"},{"3":"4"},{"5":"6"},{"7":"8"},{"9":"10"}]} } })"_json.dump();
    collection[1] = json.c_str();
    M_EXPECT_EQ_JSON(*collection[1].value(), json);

    // Merge
    auto modifier =
        R"( { "a": {"b": "c","0":{"b":[{"1":"2"},{"3":"14"},{"5":"6"},{"7":"8"},{"9":"10"},{"11":"12"}]} } })"_json
            .dump();
    EXPECT_TRUE(collection[1].merge(modifier.c_str()));
    auto result = collection[1].value();
    M_EXPECT_EQ_JSON(result->c_str(), modifier.c_str());

    // Merge by field
    modifier = R"({"9": "11"})"_json.dump();
    auto expected =
        R"( { "a": {"b": "c","0":{"b":[{"1":"2"},{"3":"14"},{"5":"6"},{"7":"8"},{"9":"11"},{"11":"12"}]} } })"_json
            .dump();
    EXPECT_TRUE(collection[ckf(1, "/a/0/b/4")].merge(modifier.c_str()));
    result = collection[1].value();
    M_EXPECT_EQ_JSON(result->c_str(), expected.c_str());

    // Patch
    modifier =
        R"([ { "op": "add", "path": "/a/key", "value": "value" },
             { "op": "replace", "path": "/a/0/b/0", "value": {"1":"3"} },
             { "op": "copy", "path": "/a/another_key", "from": "/a/key" },
             { "op": "move", "path": "/a/0/b/5", "from": "/a/0/b/1" },
             { "op": "remove", "path": "/a/b" }  ])"_json.dump();

    expected =
        R"( { "a": {"key" : "value","another_key" : "value","0":{"b":[{"1":"3"},{"5":"6"},{"7":"8"},{"9":"11"},{"11":"12"},{"3":"14"}]} } })"_json
            .dump();
    EXPECT_TRUE(collection[1].patch(modifier.c_str()));
    result = collection[1].value();
    M_EXPECT_EQ_JSON(result->c_str(), expected.c_str());

    // Patch By Field
    modifier = R"([ { "op": "add", "path": "/6", "value": {"15":"16"} } ])"_json.dump();
    expected =
        R"( { "a": {"key" : "value","another_key" :
        "value","0":{"b":[{"1":"3"},{"5":"6"},{"7":"8"},{"9":"11"},{"11":"12"},{"3":"14"},{"15":"16"}]} } })"_json
            .dump();
    EXPECT_TRUE(collection[ckf(1, "/a/0/b")].patch(modifier.c_str()));
    result = collection[1].value();
    M_EXPECT_EQ_JSON(result->c_str(), expected.c_str());

    // Update
    modifier = R"( {"person": {"name":"Carl", "age": 24}} )"_json.dump();
    EXPECT_TRUE(collection[1].update(modifier.c_str()));
    result = collection[1].value();
    M_EXPECT_EQ_JSON(result->c_str(), modifier.c_str());

    // Update By Field
    modifier = R"( {"name": "Jack", "age": 28} )"_json.dump();
    expected = R"( {"person": {"name":"Jack", "age": 28}} )"_json.dump();
    EXPECT_TRUE(collection[ckf(1, "/person")].update(modifier.c_str()));
    result = collection[1].value();
    M_EXPECT_EQ_JSON(result->c_str(), expected.c_str());

    // Insert
    modifier = R"( {"person": {"name":"Carl", "age": 24}} )"_json.dump();
    EXPECT_FALSE(collection[1].insert(modifier.c_str()));
    EXPECT_TRUE(collection[2].insert(modifier.c_str()));
    result = collection[2].value();
    M_EXPECT_EQ_JSON(result->c_str(), modifier.c_str());

    // Insert By Field
    modifier = R"("Grilish" )"_json.dump();
    expected = R"( {"person": {"name":"Carl", "age": 24, "surname" : "Grilish"}} )"_json.dump();
    EXPECT_TRUE(collection[ckf(2, "/person/surname")].insert(modifier.c_str()));
    result = collection[2].value();
    M_EXPECT_EQ_JSON(result->c_str(), expected.c_str());

    // Upsert
    modifier = R"( {"person": {"name":"Jack", "age": 28}} )"_json.dump();
    EXPECT_TRUE(collection[1].upsert(modifier.c_str()));
    result = collection[1].value();
    M_EXPECT_EQ_JSON(result->c_str(), modifier.c_str());

    // Upsert By Field
    modifier = R"("Carl")"_json.dump();
    expected = R"( {"person": {"name":"Carl", "age": 28}} )"_json.dump();
    EXPECT_TRUE(collection[ckf(1, "/person/name")].upsert(modifier.c_str()));
    result = collection[1].value();
    M_EXPECT_EQ_JSON(result->c_str(), expected.c_str());

    modifier = R"("Grilish")"_json.dump();
    expected = R"( {"person": {"name":"Carl", "age": 28, "surname" : "Grilish"}} )"_json.dump();
    EXPECT_TRUE(collection[ckf(1, "/person/surname")].upsert(modifier.c_str()));
    result = collection[1].value();
    M_EXPECT_EQ_JSON(result->c_str(), expected.c_str());

    EXPECT_TRUE(db.clear());
}

TEST(db, docs_merge_and_patch) {
    using json_t = nlohmann::json;
    database_t db;
    EXPECT_TRUE(db.open(path()));
    docs_collection_t collection = *db.collection<docs_collection_t>();

    std::ifstream f_patch("tests/patch.json");
    json_t j_object = json_t::parse(f_patch);
    for (auto it : j_object) {
        auto doc = it["doc"].dump();
        auto patch = it["patch"].dump();
        auto expected = it["expected"].dump();
        collection[1] = doc.c_str();
        EXPECT_TRUE(collection[1].patch(patch.c_str()));
        M_EXPECT_EQ_JSON(collection[1].value()->c_str(), expected.c_str());
    }

    std::ifstream f_merge("tests/merge.json");
    j_object = json_t::parse(f_merge);
    for (auto it : j_object) {
        auto doc = it["doc"].dump();
        auto merge = it["merge"].dump();
        auto expected = it["expected"].dump();
        collection[1] = doc.c_str();
        EXPECT_TRUE(collection[1].merge(merge.c_str()));
        M_EXPECT_EQ_JSON(collection[1].value()->c_str(), expected.c_str());
    }

    EXPECT_TRUE(db.clear());
}

TEST(db, docs_table) {

    using json_t = nlohmann::json;
    database_t db;
    EXPECT_TRUE(db.open(path()));

    // Inject basic data
    docs_collection_t collection = *db.collection<docs_collection_t>();
    auto json_alice = R"( { "person": "Alice", "age": 27, "height": 1 } )"_json.dump();
    auto json_bob = R"( { "person": "Bob", "age": "27", "weight": 2 } )"_json.dump();
    auto json_carl = R"( { "person": "Carl", "age": 24 } )"_json.dump();
    collection[1] = json_alice.c_str();
    collection[2] = json_bob.c_str();
    collection[3] = json_carl.c_str();
    M_EXPECT_EQ_JSON(*collection[1].value(), json_alice.c_str());
    M_EXPECT_EQ_JSON(*collection[2].value(), json_bob.c_str());

    // Just column names
    {
        auto maybe_fields = collection[1].gist();
        auto fields = *maybe_fields;

        std::vector<std::string> parsed;
        for (auto field : fields)
            parsed.emplace_back(field.data());

        EXPECT_NE(std::find(parsed.begin(), parsed.end(), "/person"), parsed.end());
        EXPECT_NE(std::find(parsed.begin(), parsed.end(), "/height"), parsed.end());
        EXPECT_NE(std::find(parsed.begin(), parsed.end(), "/age"), parsed.end());
        EXPECT_EQ(std::find(parsed.begin(), parsed.end(), "/weight"), parsed.end());
    }

    // Single cell
    {
        auto header = table_header().with<std::uint32_t>("age");
        auto maybe_table = collection[1].gather(header);
        auto table = *maybe_table;
        auto col0 = table.column<0>();

        EXPECT_EQ(col0[0].value, 27);
        EXPECT_FALSE(col0[0].converted);
    }

    // Single row
    {
        auto header = table_header() //
                          .with<std::uint32_t>("age")
                          .with<std::int32_t>("age")
                          .with<std::string_view>("age");

        auto maybe_table = collection[1].gather(header);
        auto table = *maybe_table;
        auto col0 = table.column<0>();
        auto col1 = table.column<1>();
        auto col2 = table.column<2>();

        EXPECT_EQ(col0[0].value, 27);
        EXPECT_FALSE(col0[0].converted);
        EXPECT_EQ(col1[0].value, 27);
        EXPECT_TRUE(col1[0].converted);
        EXPECT_STREQ(col2[0].value.data(), "27");
        EXPECT_TRUE(col2[0].converted);
    }

    // Single column
    {
        auto header = table_header().with<std::int32_t>("age");
        auto maybe_table = collection[{1, 2, 3, 123456}].gather(header);
        auto table = *maybe_table;
        auto col0 = table.column<0>();

        EXPECT_EQ(col0[0].value, 27);
        EXPECT_EQ(col0[1].value, 27);
        EXPECT_TRUE(col0[1].converted);
        EXPECT_EQ(col0[2].value, 24);
    }

    // Single strings column
    {
        auto header = table_header().with<std::string_view>("age");
        auto maybe_table = collection[{1, 2, 3, 123456}].gather(header);
        auto table = *maybe_table;
        auto col0 = table.column<0>();

        EXPECT_STREQ(col0[0].value.data(), "27");
        EXPECT_TRUE(col0[0].converted);
        EXPECT_STREQ(col0[1].value.data(), "27");
        EXPECT_STREQ(col0[2].value.data(), "24");
    }

    // Multi-column
    {
        auto header = table_header() //
                          .with<std::int32_t>("age")
                          .with<std::string_view>("age")
                          .with<std::string_view>("person")
                          .with<float>("person")
                          .with<std::int32_t>("height")
                          .with<std::uint64_t>("weight");

        auto maybe_table = collection[{1, 2, 3, 123456, 654321}].gather(header);
        auto table = *maybe_table;
        auto col0 = table.column<0>();
        auto col1 = table.column<1>();
        auto col2 = table.column<2>();
        auto col3 = table.column<3>();
        auto col4 = table.column<4>();
        auto col5 = table.column<5>();

        EXPECT_EQ(col0[0].value, 27);
        EXPECT_EQ(col0[1].value, 27);
        EXPECT_TRUE(col0[1].converted);
        EXPECT_EQ(col0[2].value, 24);

        EXPECT_STREQ(col1[0].value.data(), "27");
        EXPECT_TRUE(col1[0].converted);
        EXPECT_STREQ(col1[1].value.data(), "27");
        EXPECT_STREQ(col1[2].value.data(), "24");
    }

    // Multi-column Type-punned exports
    {
        table_header_t header {{
            field_type_t {"age", ukv_doc_field_i32_k},
            field_type_t {"age", ukv_doc_field_str_k},
            field_type_t {"person", ukv_doc_field_str_k},
            field_type_t {"person", ukv_doc_field_f32_k},
            field_type_t {"height", ukv_doc_field_i32_k},
            field_type_t {"weight", ukv_doc_field_u64_k},
        }};

        auto maybe_table = collection[{1, 2, 3, 123456, 654321}].gather(header);
        auto table = *maybe_table;
        auto col0 = table.column(0).as<std::int32_t>();
        auto col1 = table.column(1).as<value_view_t>();
        auto col2 = table.column(2).as<value_view_t>();
        auto col3 = table.column(3).as<float>();
        auto col4 = table.column(4).as<std::int32_t>();
        auto col5 = table.column(5).as<std::uint64_t>();

        EXPECT_EQ(col0[0].value, 27);
        EXPECT_EQ(col0[1].value, 27);
        EXPECT_TRUE(col0[1].converted);
        EXPECT_EQ(col0[2].value, 24);

        EXPECT_STREQ(col1[0].value.c_str(), "27");
        EXPECT_TRUE(col1[0].converted);
        EXPECT_STREQ(col1[1].value.c_str(), "27");
        EXPECT_STREQ(col1[2].value.c_str(), "24");
    }

    EXPECT_TRUE(db.clear());
}

#pragma region Graph Collections

TEST(db, graph_triangle) {

    database_t db;
    EXPECT_TRUE(db.open(path()));

    graph_collection_t net = *db.collection<graph_collection_t>();

    // triangle
    edge_t edge1 {1, 2, 9};
    edge_t edge2 {2, 3, 10};
    edge_t edge3 {3, 1, 11};

    EXPECT_TRUE(net.upsert(edge1));
    EXPECT_TRUE(net.upsert(edge2));
    EXPECT_TRUE(net.upsert(edge3));

    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_FALSE(*net.contains(9));
    EXPECT_FALSE(*net.contains(10));
    EXPECT_FALSE(*net.contains(1000));

    EXPECT_EQ(*net.degree(1), 2u);
    EXPECT_EQ(*net.degree(2), 2u);
    EXPECT_EQ(*net.degree(3), 2u);
    EXPECT_EQ(*net.degree(1, ukv_vertex_source_k), 1u);
    EXPECT_EQ(*net.degree(2, ukv_vertex_source_k), 1u);
    EXPECT_EQ(*net.degree(3, ukv_vertex_source_k), 1u);

    EXPECT_TRUE(net.edges(1));
    EXPECT_EQ(net.edges(1)->size(), 2ul);
    EXPECT_EQ(net.edges(1, ukv_vertex_source_k)->size(), 1ul);
    EXPECT_EQ(net.edges(1, ukv_vertex_target_k)->size(), 1ul);

    EXPECT_EQ(net.edges(3, ukv_vertex_target_k)->size(), 1ul);
    EXPECT_EQ(net.edges(2, ukv_vertex_source_k)->size(), 1ul);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].source_id, 2);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].target_id, 3);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].id, 10);
    EXPECT_EQ(net.edges(3, 1)->size(), 1ul);
    EXPECT_EQ(net.edges(1, 3)->size(), 0ul);

    // Check scans
    EXPECT_TRUE(net.edges());
    {
        std::unordered_set<edge_t, edge_hash_t> expected_edges {edge1, edge2, edge3};
        std::unordered_set<edge_t, edge_hash_t> exported_edges;

        auto present_edges = *net.edges();
        auto present_it = std::move(present_edges).begin();
        auto count_results = 0;
        while (!present_it.is_end()) {
            exported_edges.insert(*present_it);
            ++present_it;
            ++count_results;
        }
        EXPECT_EQ(count_results, 6);
        EXPECT_EQ(exported_edges, expected_edges);
    }

    // Remove a single edge, making sure that the nodes info persists
    EXPECT_TRUE(net.remove({
        {{&edge1.source_id}, 1},
        {{&edge1.target_id}, 1},
        {{&edge1.id}, 1},
    }));
    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_EQ(net.edges(1, 2)->size(), 0ul);

    // Bring that edge back
    EXPECT_TRUE(net.upsert({
        {{&edge1.source_id}, 1},
        {{&edge1.target_id}, 1},
        {{&edge1.id}, 1},
    }));
    EXPECT_EQ(net.edges(1, 2)->size(), 1ul);

    // Remove a vertex
    ukv_key_t vertex_to_remove = 2;
    EXPECT_TRUE(net.remove(vertex_to_remove));
    EXPECT_FALSE(*net.contains(vertex_to_remove));
    EXPECT_EQ(net.edges(vertex_to_remove)->size(), 0ul);
    EXPECT_EQ(net.edges(1, vertex_to_remove)->size(), 0ul);
    EXPECT_EQ(net.edges(vertex_to_remove, 1)->size(), 0ul);

    // Bring back the whole graph
    EXPECT_TRUE(net.upsert(edge1));
    EXPECT_TRUE(net.upsert(edge2));
    EXPECT_TRUE(net.upsert(edge3));
    EXPECT_TRUE(*net.contains(vertex_to_remove));
    EXPECT_EQ(net.edges(vertex_to_remove)->size(), 2ul);
    EXPECT_EQ(net.edges(1, vertex_to_remove)->size(), 1ul);
    EXPECT_EQ(net.edges(vertex_to_remove, 1)->size(), 0ul);
    EXPECT_TRUE(db.clear());
}

TEST(db, graph_triangle_batch_api) {

    database_t db;
    EXPECT_TRUE(db.open(path()));

    bins_collection_t main = *db.collection();
    graph_collection_t net = *db.collection<graph_collection_t>();

    std::vector<edge_t> triangle {
        {1, 2, 9},
        {2, 3, 10},
        {3, 1, 11},
    };

    EXPECT_TRUE(net.upsert(edges(triangle)));
    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_FALSE(*net.contains(9));
    EXPECT_FALSE(*net.contains(10));
    EXPECT_FALSE(*net.contains(1000));

    EXPECT_EQ(*net.degree(1), 2u);
    EXPECT_EQ(*net.degree(2), 2u);
    EXPECT_EQ(*net.degree(3), 2u);
    EXPECT_EQ(*net.degree(1, ukv_vertex_source_k), 1u);
    EXPECT_EQ(*net.degree(2, ukv_vertex_source_k), 1u);
    EXPECT_EQ(*net.degree(3, ukv_vertex_source_k), 1u);

    EXPECT_TRUE(net.edges(1));
    EXPECT_EQ(net.edges(1)->size(), 2ul);
    EXPECT_EQ(net.edges(1, ukv_vertex_source_k)->size(), 1ul);
    EXPECT_EQ(net.edges(1, ukv_vertex_target_k)->size(), 1ul);

    EXPECT_EQ(net.edges(3, ukv_vertex_target_k)->size(), 1ul);
    EXPECT_EQ(net.edges(2, ukv_vertex_source_k)->size(), 1ul);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].source_id, 2);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].target_id, 3);
    EXPECT_EQ((*net.edges(3, ukv_vertex_target_k))[0].id, 10);
    EXPECT_EQ(net.edges(3, 1)->size(), 1ul);
    EXPECT_EQ(net.edges(1, 3)->size(), 0ul);

    // Check scans
    EXPECT_TRUE(net.edges());
    {
        std::unordered_set<edge_t, edge_hash_t> expected_edges {triangle.begin(), triangle.end()};
        std::unordered_set<edge_t, edge_hash_t> exported_edges;

        auto present_edges = *net.edges();
        auto present_it = std::move(present_edges).begin();
        size_t count_results = 0ul;
        while (!present_it.is_end()) {
            exported_edges.insert(*present_it);
            ++present_it;
            ++count_results;
        }
        EXPECT_EQ(count_results, triangle.size() * 2);
        EXPECT_EQ(exported_edges, expected_edges);
    }

    // Remove a single edge, making sure that the nodes info persists
    EXPECT_TRUE(net.remove(edges_view_t {
        {{&triangle[0].source_id}, 1},
        {{&triangle[0].target_id}, 1},
        {{&triangle[0].id}, 1},
    }));
    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_EQ(net.edges(1, 2)->size(), 0ul);

    // Bring that edge back
    EXPECT_TRUE(net.upsert(edges_view_t {
        {{&triangle[0].source_id}, 1},
        {{&triangle[0].target_id}, 1},
        {{&triangle[0].id}, 1},
    }));
    EXPECT_EQ(net.edges(1, 2)->size(), 1ul);

    // Remove a vertex
    ukv_key_t vertex_to_remove = 2;
    EXPECT_TRUE(net.remove(vertex_to_remove));
    EXPECT_FALSE(*net.contains(vertex_to_remove));
    EXPECT_EQ(net.edges(vertex_to_remove)->size(), 0ul);
    EXPECT_EQ(net.edges(1, vertex_to_remove)->size(), 0ul);
    EXPECT_EQ(net.edges(vertex_to_remove, 1)->size(), 0ul);

    // Bring back the whole graph
    EXPECT_TRUE(net.upsert(edges(triangle)));
    EXPECT_TRUE(*net.contains(vertex_to_remove));
    EXPECT_EQ(net.edges(vertex_to_remove)->size(), 2ul);
    EXPECT_EQ(net.edges(1, vertex_to_remove)->size(), 1ul);
    EXPECT_EQ(net.edges(vertex_to_remove, 1)->size(), 0ul);
    EXPECT_TRUE(db.clear());
}

TEST(db, graph_transaction_watch) {

    if (!ukv_supports_transactions_k)
        return;

    database_t db;
    EXPECT_TRUE(db.open(path()));
    graph_collection_t net = *db.collection<graph_collection_t>();
    transaction_t txn = *db.transact();
    graph_collection_t txn_net = *txn.collection<graph_collection_t>();

    edge_t edge1 {1, 2, 1};
    edge_t edge2 {3, 1, 2};

    EXPECT_TRUE(net.upsert(edge1));
    EXPECT_TRUE(net.upsert(edge2));
    txn_net.degree(1);
    EXPECT_TRUE(txn.commit());

    EXPECT_TRUE(txn.reset());
    txn_net.degree(1);
    EXPECT_TRUE(txn.commit());

    txn_net.degree(1);
    EXPECT_TRUE(net.remove(edge1));
    EXPECT_TRUE(net.remove(edge2));
    EXPECT_FALSE(txn.commit());
    EXPECT_TRUE(txn.reset());

    txn_net.degree(1, ukv_vertex_role_any_k, false);
    EXPECT_TRUE(net.upsert(edge1));
    EXPECT_TRUE(net.upsert(edge2));
    EXPECT_TRUE(txn.commit());

    EXPECT_TRUE(db.clear());
}

edge_t make_edge(ukv_key_t edge_id, ukv_key_t v1, ukv_key_t v2) {
    return {v1, v2, edge_id};
}

std::vector<edge_t> make_edges(std::size_t vertices_count = 2, std::size_t next_connect = 1) {
    std::vector<edge_t> es;
    ukv_key_t edge_id = 0;
    for (ukv_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        ukv_key_t connect_with = vertex_id + next_connect;
        while (connect_with < vertices_count) {
            edge_id++;
            es.push_back(make_edge(edge_id, vertex_id, connect_with));
            connect_with = connect_with + next_connect;
        }
    }
    return es;
}

TEST(db, graph_random_fill) {
    database_t db;
    EXPECT_TRUE(db.open(path()));

    graph_collection_t graph = *db.collection<graph_collection_t>();

    constexpr std::size_t vertices_count = 1000;
    auto edges_vec = make_edges(vertices_count, 100);
    EXPECT_TRUE(graph.upsert(edges(edges_vec)));

    for (ukv_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_EQ(*graph.degree(vertex_id), 9u);
    }

    EXPECT_TRUE(db.clear());
}

TEST(db, graph_conflicting_transactions) {

    if (!ukv_supports_transactions_k)
        return;

    database_t db;
    EXPECT_TRUE(db.open(path()));

    graph_collection_t net = *db.collection<graph_collection_t>();

    transaction_t txn = *db.transact();
    graph_collection_t txn_net = *txn.collection<graph_collection_t>();

    // triangle
    edge_t edge1 {1, 2, 9};
    edge_t edge2 {2, 3, 10};
    edge_t edge3 {3, 1, 11};

    EXPECT_TRUE(txn_net.upsert(edge1));
    EXPECT_TRUE(txn_net.upsert(edge2));
    EXPECT_TRUE(txn_net.upsert(edge3));

    EXPECT_TRUE(*txn_net.contains(1));
    EXPECT_TRUE(*txn_net.contains(2));
    EXPECT_TRUE(*txn_net.contains(3));

    EXPECT_FALSE(*net.contains(1));
    EXPECT_FALSE(*net.contains(2));
    EXPECT_FALSE(*net.contains(3));

    auto status = txn.commit();
    status.throw_unhandled();
    EXPECT_TRUE(*net.contains(1));
    EXPECT_TRUE(*net.contains(2));
    EXPECT_TRUE(*net.contains(3));

    EXPECT_TRUE(txn.reset());
    txn_net = *txn.collection<graph_collection_t>();

    transaction_t txn2 = *db.transact();
    graph_collection_t txn_net2 = *txn2.collection<graph_collection_t>();

    edge_t edge4 {4, 5, 15};
    edge_t edge5 {5, 6, 16};

    EXPECT_TRUE(txn_net.upsert(edge4));
    EXPECT_TRUE(txn_net2.upsert(edge5));

    EXPECT_TRUE(txn.commit());
    EXPECT_FALSE(txn2.commit());

    EXPECT_TRUE(db.clear());
}

TEST(db, graph_upsert_edges) {
    database_t db;
    EXPECT_TRUE(db.open(path()));

    graph_collection_t graph = *db.collection<graph_collection_t>();

    std::vector<ukv_key_t> vertices = {1, 2, 3, 4, 5};
    auto over_the_vertices = [&](bool exist, size_t degree) {
        for (auto& vertex_id : vertices) {
            EXPECT_EQ(*graph.contains(vertex_id), exist);
            EXPECT_EQ(*graph.degree(vertex_id), degree);
        }
    };

    over_the_vertices(false, 0);

    std::vector<edge_t> star {
        {1, 3, 1},
        {1, 4, 2},
        {2, 4, 3},
        {2, 5, 4},
        {3, 5, 5},
    };
    EXPECT_TRUE(graph.upsert(edges(star)));
    over_the_vertices(true, 2u);

    std::vector<edge_t> pentagon {
        {1, 2, 6},
        {2, 3, 7},
        {3, 4, 8},
        {4, 5, 9},
        {5, 1, 10},
    };
    EXPECT_TRUE(graph.upsert(edges(pentagon)));
    over_the_vertices(true, 4u);

    EXPECT_TRUE(graph.remove(edges(star)));
    over_the_vertices(true, 2u);

    EXPECT_TRUE(graph.upsert(edges(star)));
    over_the_vertices(true, 4u);

    EXPECT_TRUE(graph.remove(edges(pentagon)));
    over_the_vertices(true, 2u);

    EXPECT_TRUE(graph.upsert(edges(pentagon)));
    over_the_vertices(true, 4u);

    std::vector<edge_t> itself {
        {1, 1, 11},
        {2, 2, 12},
        {3, 3, 13},
        {4, 4, 14},
        {5, 5, 15},
    };

    EXPECT_TRUE(graph.upsert(edges(itself)));
    over_the_vertices(true, 6u);

    EXPECT_TRUE(graph.remove(edges(star)));
    EXPECT_TRUE(graph.remove(edges(pentagon)));

    over_the_vertices(true, 2u);

    EXPECT_TRUE(graph.remove(edges(itself)));

    over_the_vertices(true, 0);

    EXPECT_TRUE(db.clear());

    over_the_vertices(false, 0);
}

TEST(db, graph_remove_vertices) {
    database_t db;
    EXPECT_TRUE(db.open(path()));

    graph_collection_t graph = *db.collection<graph_collection_t>();

    constexpr std::size_t vertices_count = 1000;
    auto edges_vec = make_edges(vertices_count, 100);
    EXPECT_TRUE(graph.upsert(edges(edges_vec)));

    for (ukv_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_TRUE(*graph.contains(vertex_id));
        EXPECT_TRUE(graph.remove(vertex_id));
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_FALSE(*graph.contains(vertex_id));
    }

    EXPECT_TRUE(db.clear());
}

TEST(db, graph_remove_edges_keep_vertices) {
    database_t db;
    EXPECT_TRUE(db.open(path()));

    graph_collection_t graph = *db.collection<graph_collection_t>();

    constexpr std::size_t vertices_count = 1000;
    auto edges_vec = make_edges(vertices_count, 100);
    EXPECT_TRUE(graph.upsert(edges(edges_vec)));
    EXPECT_TRUE(graph.remove(edges(edges_vec)));

    for (ukv_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_TRUE(*graph.contains(vertex_id));
    }

    EXPECT_TRUE(db.clear());
}

TEST(db, graph_get_edges) {
    database_t db;
    EXPECT_TRUE(db.open(path()));

    graph_collection_t graph = *db.collection<graph_collection_t>();

    constexpr std::size_t vertices_count = 1000;
    auto edges_vec = make_edges(vertices_count, 100);
    EXPECT_TRUE(graph.upsert(edges(edges_vec)));

    std::vector<edge_t> received_edges;
    for (ukv_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        auto es = *graph.edges(vertex_id);
        EXPECT_EQ(es.size(), 9u);
        for (size_t i = 0; i != es.size(); ++i)
            received_edges.push_back(es[i]);
    }

    EXPECT_TRUE(graph.remove(edges(received_edges)));
    for (ukv_key_t vertex_id = 0; vertex_id != vertices_count; ++vertex_id) {
        EXPECT_TRUE(graph.contains(vertex_id));
        EXPECT_TRUE(*graph.contains(vertex_id));
        auto es = *graph.edges(vertex_id);
        EXPECT_EQ(es.size(), 0);
    }
    EXPECT_TRUE(db.clear());
}

int main(int argc, char** argv) {
    std::filesystem::create_directory("./tmp");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}