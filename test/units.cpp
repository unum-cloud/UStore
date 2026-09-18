/**
 *  @file test/units.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Unit tests of the blobs, documents and graph modalities through the C++ wrappers.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int32_t`, `std::uint32_t`, `std::uint64_t`
#include <cstdio>  // `stderr`
#include <cstdlib> // `std::free`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::min`, `std::sort`
#include <array>       // `std::array`
#include <atomic>      // `std::atomic`, `std::atomic_flag`
#include <numeric>     // `std::iota`
#include <string_view> // `std::string_view`

#include <fmt/format.h>
#include <yyjson.h>

#include <smashtable/basic_vector.hpp>

#include <ustore/ustore.hpp>

#include "executor.hpp"
#include "test.hpp"

using namespace unum::ustore;
using namespace unum;

#pragma region JSON

/** Views the bytes of @p value as text. */
std::string_view as_text(value_view_t value) noexcept {
    return std::string_view(reinterpret_cast<char const*>(value.begin()), value.size());
}

/** Whether two JSON texts parse to equal documents; a text that does not parse equals nothing. */
bool json_equal(std::string_view first, std::string_view second) noexcept {
    yyjson_doc* const first_document = yyjson_read(first.data(), first.size(), 0);
    yyjson_doc* const second_document = yyjson_read(second.data(), second.size(), 0);
    bool const equal = first_document && second_document &&
                       yyjson_equals(yyjson_doc_get_root(first_document), yyjson_doc_get_root(second_document));
    yyjson_doc_free(first_document);
    yyjson_doc_free(second_document);
    return equal;
}

/** A JSON text yyjson wrote for one value, freed with it. */
struct written_json_t {
    char* text = nullptr;

    explicit written_json_t(yyjson_val* value) noexcept : text(value ? yyjson_val_write(value, 0, nullptr) : nullptr) {}
    written_json_t(written_json_t const&) = delete;
    written_json_t& operator=(written_json_t const&) = delete;
    ~written_json_t() noexcept { std::free(text); }
};

#pragma endregion JSON

#pragma region Binary Modality

/** Checks that every key @p reference names holds a value of @p expected_length, or is missing. */
template <typename locations_type_>
std::size_t check_length(blobs_ref<locations_type_>& reference, ustore_length_t expected_length) {
    std::size_t failures = 0;
    bool const expects_missing = expected_length == ustore_length_missing_k;
    using extractor_t = places_arg_extractor<locations_type_>;
    ustore_size_t const count = extractor_t {}.count(reference.locations());

    auto maybe_retrieved = reference.value();
    failures += expect(maybe_retrieved);
    if (!maybe_retrieved) return failures;
    auto const& retrieved = *maybe_retrieved;
    failures += expect_eq(retrieved.size(), count);

    auto iterator = retrieved.begin();
    for (std::size_t index = 0; index != count; ++index, ++iterator)
        failures += expect_eq((*iterator).size(), expects_missing ? 0 : expected_length);

    auto maybe_lengths = reference.length();
    failures += expect(maybe_lengths);
    for (std::size_t index = 0; index != count; ++index)
        failures += expect_eq((*maybe_lengths)[index], expected_length);

    auto maybe_indicators = reference.present();
    failures += expect(maybe_indicators);
    for (std::size_t index = 0; index != count; ++index)
        failures += expect(static_cast<bool>(maybe_indicators->at(index)) == !expects_missing);
    return failures;
}

/** Checks that the keys @p reference names hold @p values, byte for byte. */
template <template <typename> typename reference_type_, typename locations_type_>
std::size_t check_equalities(reference_type_<locations_type_>& reference, contents_arg_t values) {
    std::size_t failures = 0;
    using extractor_t = places_arg_extractor<locations_type_>;
    ustore_size_t const count = extractor_t {}.count(reference.locations());

    auto maybe_retrieved = reference.value();
    failures += expect(maybe_retrieved);
    if (!maybe_retrieved) return failures;
    auto const& retrieved = *maybe_retrieved;
    failures += expect_eq(retrieved.size(), count);

    auto iterator = retrieved.begin();
    for (std::size_t index = 0; index != count; ++index, ++iterator) {
        auto const expected_begin = values[index].begin();
        value_view_t const retrieved_view = *iterator;
        value_view_t const expected_view(expected_begin, expected_begin + values[index].size());
        failures += expect_eq(retrieved_view.size(), expected_view.size());
        failures += expect(retrieved_view == expected_view);
    }
    return failures;
}

/** Assigns @p values through @p reference and checks they read back. */
template <typename locations_type_>
std::size_t round_trip(blobs_ref<locations_type_>& reference, contents_arg_t values) {
    std::size_t failures = expect(reference.assign(values).succeeded());
    failures += check_equalities(reference, values);
    return failures;
}

/** Three one-byte values under three keys, exposed in each layout the contents argument accepts. */
struct triplet_t {
    static constexpr std::size_t value_size_k = sizeof(char);

    std::array<ustore_key_t, 3> keys {'a', 'b', 'c'};
    std::array<char, 3> values {'A', 'B', 'C'};
    std::array<ustore_length_t, 3> lengths {1, 1, 1};
    std::array<ustore_length_t, 4> offsets {0, 1, 2, 3};
    ustore_octet_t presences = 1 | (1 << 1) | (1 << 2);
    std::array<ustore_bytes_ptr_t, 3> value_pointers;

    triplet_t() noexcept {
        value_pointers[0] = reinterpret_cast<ustore_bytes_ptr_t>(&values[0]);
        value_pointers[1] = reinterpret_cast<ustore_bytes_ptr_t>(&values[1]);
        value_pointers[2] = reinterpret_cast<ustore_bytes_ptr_t>(&values[2]);
    }

    contents_arg_t contents_lengths() const noexcept {
        contents_arg_t argument {};
        argument.lengths_begin = {&lengths[0], sizeof(lengths[0])};
        argument.contents_begin = {&value_pointers[0], sizeof(value_pointers[0])};
        argument.count = 3;
        return argument;
    }

    contents_arg_t contents_offsets() const noexcept {
        contents_arg_t argument {};
        argument.offsets_begin = {&offsets[0], sizeof(offsets[0])};
        argument.contents_begin = {&value_pointers[0], 0};
        argument.count = 3;
        return argument;
    }

    contents_arg_t contents_full() const noexcept {
        contents_arg_t argument {};
        argument.presences_begin = &presences;
        argument.offsets_begin = {&offsets[0], sizeof(offsets[0])};
        argument.lengths_begin = {&lengths[0], 0};
        argument.contents_begin = {&value_pointers[0], 0};
        argument.count = 3;
        return argument;
    }
};

/** Round-trips @p triplet through @p reference in each contents layout. */
template <typename locations_type_>
std::size_t round_trip(blobs_ref<locations_type_>& reference, triplet_t const& triplet) {
    std::size_t failures = 0;
    failures += round_trip(reference, triplet.contents_offsets());
    failures += round_trip(reference, triplet.contents_lengths());
    failures += round_trip(reference, triplet.contents_full());
    return failures;
}

/** Checks that @p reference holds @p triplet, read through each contents layout. */
template <template <typename> typename reference_type_, typename locations_type_>
std::size_t check_equalities(reference_type_<locations_type_>& reference, triplet_t const& triplet) {
    std::size_t failures = 0;
    failures += check_equalities(reference, triplet.contents_offsets());
    failures += check_equalities(reference, triplet.contents_lengths());
    failures += check_equalities(reference, triplet.contents_full());
    return failures;
}

/** Checks overwrites, cleared values keeping their keys, key streaming and erasure on @p collection. */
std::size_t check_binary_collection(blobs_collection_t& collection) {
    std::size_t failures = 0;
    triplet_t triplet;
    auto reference = collection[triplet.keys];
    failures += round_trip(reference, triplet);

    // Overwrite with values of the same size
    for (char& value : triplet.values) value += 7;
    failures += round_trip(reference, triplet);

    // Empty values keep their keys
    failures += expect(reference.clear().succeeded());
    failures += check_length(reference, 0);

    auto maybe_present = collection.members().keys_begin();
    failures += expect(maybe_present);
    if (!maybe_present) return failures;
    keys_stream_t& present_iterator = *maybe_present;
    for (ustore_key_t const expected_key : triplet.keys) {
        failures += expect_eq(*present_iterator, expected_key);
        ++present_iterator;
    }
    ++present_iterator;
    failures += expect(present_iterator.is_end());

    failures += expect(reference.erase().succeeded());
    failures += check_length(reference, ustore_length_missing_k);
    return failures;
}

/** Opens and clears a database, then round-trips values through the main collection. */
std::size_t database_open_clear_close(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    blobs_collection_t collection = database.main();
    failures += check_binary_collection(collection);
    return failures;
}

/** Clearing the whole database empties the main collection. */
std::size_t database_clear_collection_by_clearing_database(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    blobs_collection_t collection = database.main();
    triplet_t triplet;
    auto reference = collection[triplet.keys];
    failures += round_trip(reference, triplet.contents_offsets());

    failures += expect(collection.keys().size() == 3u);
    failures += expect(collection.items().size() == 3u);

    failures += expect(database.clear().succeeded());
    failures += check_length(reference, ustore_length_missing_k);
    return failures;
}

/** Fills keys 1000 to 1100 and 900 down to 800, then overwrites every other key across both ranges. */
std::size_t database_overwrite_with_step(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    blobs_collection_t collection = database.main();

    for (ustore_key_t key = 1000; key != 1100; ++key) failures += expect(collection[key].assign("some").succeeded());
    for (ustore_key_t key = 1000; key != 1100; ++key) failures += expect(*collection[key].value() == "some");
    failures += expect(collection.keys().size() == 100u);
    failures += expect(collection.items().size() == 100u);

    for (ustore_key_t key = 900; key != 800; --key) failures += expect(collection[key].assign("other").succeeded());
    for (ustore_key_t key = 900; key != 800; --key) failures += expect(*collection[key].value() == "other");
    failures += expect(collection.keys().size() == 200u);
    failures += expect(collection.items().size() == 200u);

    for (ustore_key_t key = 800; key != 1100; key += 2) failures += expect(collection[key].assign("third").succeeded());
    for (ustore_key_t key = 800; key != 1100; key += 2) failures += expect(*collection[key].value() == "third");
    failures += expect(collection.keys().size() == 250u);
    failures += expect(collection.items().size() == 250u);
    return failures;
}

/** Populates the database, closes and reopens it, and checks the contents survived. */
std::size_t database_persistency(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    triplet_t triplet;
    {
        blobs_collection_t main_collection = database.main();
        auto main_reference = main_collection[triplet.keys];
        failures += check_length(main_reference, ustore_length_missing_k);
        failures += round_trip(main_reference, triplet);
        failures += check_length(main_reference, triplet_t::value_size_k);

        if (database.supports_collections()) {
            blobs_collection_t named_collection = *database.create("collection");
            auto named_reference = named_collection[triplet.keys];
            failures += check_length(named_reference, ustore_length_missing_k);
            failures += round_trip(named_reference, triplet);
            failures += check_length(named_reference, triplet_t::value_size_k);
            failures += expect(named_collection.clear_values().succeeded());
            failures += check_length(named_reference, 0);
        }
    }
    database.close();
    {
        failures += expect(database.open(config.c_str()).succeeded());

        blobs_collection_t main_collection = database.main();
        auto main_reference = main_collection[triplet.keys];
        failures += check_equalities(main_reference, triplet);
        failures += check_length(main_reference, triplet_t::value_size_k);
        failures += expect(main_collection.keys().size() == 3u);
        failures += expect(main_collection.items().size() == 3u);

        if (database.supports_collections()) {
            failures += expect(database.contains("collection"));
            failures += expect(*database.contains("collection"));
            blobs_collection_t named_collection = *database["collection"];
            auto named_reference = named_collection[triplet.keys];
            failures += check_length(named_reference, 0);
            failures += expect(named_collection.keys().size() == 3u);
            failures += expect(named_collection.items().size() == 3u);
        }
    }
    return failures;
}

/** Creates, looks up, drops and clears collections under unique names. */
std::size_t database_named_collections(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    // Neither a missing collection nor the main one can be dropped
    failures += expect(!*database.contains("unknown"));
    failures += expect(database.drop("unknown").failed());
    failures += expect(database.drop("").failed());

    if (database.supports_collections()) {
        failures += expect(database["col1"]);
        failures += expect(database["col2"]);

        failures += expect(!database.create("col1"));
        blobs_collection_t first_collection = *database["col1"];
        failures += expect(!database.create("col2"));
        blobs_collection_t second_collection = *database["col2"];

        failures += check_binary_collection(first_collection);
        failures += check_binary_collection(second_collection);

        failures += expect(database.drop("col1").succeeded());
        failures += expect(database.drop("col2").succeeded());
        failures += expect(*database.contains(""));
        failures += expect(!*database.contains("col1"));
        failures += expect(!*database.contains("col2"));
    }

    failures += expect(database.clear().succeeded());
    failures += expect(*database.contains(""));
    return failures;
}

/** Lists the names of present collections. */
std::size_t database_named_collections_list(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    if (!database.supports_collections()) return failures;
    failures += expect(database.clear().succeeded());

    blobs_collection_t first_collection = *database.create("col1");
    blobs_collection_t second_collection = *database.create("col2");
    blobs_collection_t third_collection = *database.create("col3");
    blobs_collection_t fourth_collection = *database.create("col4");

    failures += expect(*database.contains("col1"));
    failures += expect(*database.contains("col2"));

    auto maybe_transaction = database.transact();
    failures += expect(maybe_transaction);
    auto maybe_collections = maybe_transaction->collections();
    failures += expect(maybe_collections);
    failures += expect_eq(maybe_collections->size(), 4u);

    // The catalog carries identifiers alone, so every name resolves through the strings modality
    std::array<std::string_view, 4> const names {"col1", "col2", "col3", "col4"};
    for (std::string_view const name : names) {
        auto maybe_found = database.find(name);
        failures += expect(maybe_found);
        if (!maybe_found) continue;
        ustore_collection_t const found = *maybe_found;
        failures += expect(std::ranges::find(*maybe_collections, found) != maybe_collections->end());
    }

    failures += expect(database.drop("col1").succeeded());
    failures += expect(!*database.contains("col1"));
    failures += expect(database.drop("").failed());
    failures += expect(database.main().clear().succeeded());
    return failures;
}

/** Clearing values keeps the keys and empties their binary strings. */
std::size_t database_clear_values(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    triplet_t triplet;
    blobs_collection_t collection = database.main();
    auto reference = collection[triplet.keys];

    failures += check_length(reference, ustore_length_missing_k);
    failures += round_trip(reference, triplet);
    failures += check_length(reference, triplet_t::value_size_k);

    failures += expect(collection.clear_values().succeeded());
    failures += check_length(reference, 0);
    failures += expect(collection.clear().succeeded());
    failures += check_length(reference, ustore_length_missing_k);
    return failures;
}

/** Reads presences through the C ABI and the C++ wrappers. */
std::size_t database_presences(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());
    blobs_collection_t main_collection = database.main();

    constexpr std::size_t keys_count = 1000;
    for (std::size_t index = 0; index != keys_count; ++index)
        if (index % 10)
            failures += expect(main_collection[static_cast<ustore_key_t>(index)].assign("value").succeeded());

    std::array<ustore_key_t, keys_count> keys;
    std::iota(keys.begin(), keys.end(), 0);
    ustore_octet_t* found_presences = nullptr;
    arena_t arena(database);
    ustore_error_t error {};
    ustore_read_t read {};
    read.db = database;
    read.error = &error;
    read.arena = arena.member_ptr();
    read.tasks_count = keys_count;
    read.keys = keys.data();
    read.keys_stride = sizeof(ustore_key_t);
    read.presences = &found_presences;

    ustore_read(&read);
    failures += expect(error.status == ustore_success_k);
    for (std::size_t index = 0; index != keys_count; ++index)
        failures += expect(static_cast<bool>(bits_view_t {found_presences}[index]) == (index % 10 != 0));

    auto maybe_presences = main_collection[keys].present();
    failures += expect(maybe_presences);
    auto const& presences = *maybe_presences;
    for (std::size_t index = 0; index != keys_count; ++index)
        failures += expect(static_cast<bool>(presences[index]) == (index % 10 != 0));
    return failures;
}

/** A key stream visits every written key in order. */
std::size_t database_scan(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());
    blobs_collection_t collection = database.main();

    constexpr std::size_t keys_count = 1000;
    std::array<ustore_key_t, keys_count> keys;
    std::iota(keys.begin(), keys.end(), 0);
    auto reference = collection[keys];
    value_view_t const value("value");
    failures += expect(reference.assign(value).succeeded());
    keys_stream_t stream(database, collection, 256);

    failures += expect(stream.seek_to_first().succeeded());
    ustore_key_t key = 0;
    while (!stream.is_end()) {
        failures += expect_eq(stream.key(), key++);
        ++stream;
    }
    failures += expect_eq(key, static_cast<ustore_key_t>(keys_count));
    return failures;
}

/** Ordered batched scan over the main collection. */
std::size_t database_batch_scan(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());
    blobs_collection_t collection = database.main();

    std::array<ustore_key_t, 512> keys;
    std::iota(keys.begin(), keys.end(), 0);
    auto reference = collection[keys];
    value_view_t const value("value");
    failures += expect(reference.assign(value).succeeded());
    keys_stream_t stream(database, collection, 256);

    failures += expect(stream.seek_to_first().succeeded());
    auto batch = stream.keys_batch();
    failures += expect_eq(batch.size(), 256u);
    failures += expect(!stream.is_end());
    for (ustore_key_t key = 0; key != 256; ++key) failures += expect_eq(batch[key], key);

    failures += expect(stream.seek_to_next_batch().succeeded());
    batch = stream.keys_batch();
    failures += expect_eq(batch.size(), 256u);
    failures += expect(!stream.is_end());
    for (ustore_key_t key = 0; key != 256; ++key) failures += expect_eq(batch[key], key + 256);

    failures += expect(stream.seek_to_next_batch().succeeded());
    batch = stream.keys_batch();
    failures += expect_eq(batch.size(), 0u);
    failures += expect(stream.is_end());
    return failures;
}

/** Readers cannot see the writes of a pending transaction, https://jepsen.io/consistency/models/read-committed */
std::size_t database_transaction_read_committed(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    if (!database.supports_transactions()) return failures;
    failures += expect(database.clear().succeeded());
    failures += expect(database.transact());
    transaction_t transaction = *database.transact();

    triplet_t triplet;
    auto transaction_reference = transaction[triplet.keys];
    failures += round_trip(transaction_reference, triplet);

    blobs_collection_t collection = database.main();
    auto collection_reference = collection[triplet.keys];

    failures += check_length(collection_reference, ustore_length_missing_k);
    failures += expect(transaction.commit().succeeded());
    failures += expect(transaction.reset().succeeded());

    failures += check_equalities(collection_reference, triplet);
    return failures;
}

/** Reads through a snapshot miss writes made after it, https://jepsen.io/consistency/models/snapshot-isolation */
std::size_t database_transaction_snapshot_isolation(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    if (!database.supports_snapshots()) return failures;
    failures += expect(database.clear().succeeded());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    blobs_collection_t collection = database.main();
    auto collection_reference = collection[triplet.keys];

    failures += check_length(collection_reference, ustore_length_missing_k);
    failures += round_trip(collection_reference, triplet);

    auto snapshot = *database.snapshot();
    auto snapshot_reference = snapshot[triplet.keys];
    failures += round_trip(snapshot_reference, triplet);
    failures += round_trip(collection_reference, same_values_triplet);

    auto maybe_retrieved = snapshot_reference.value();
    failures += expect(maybe_retrieved);
    if (!maybe_retrieved) return failures;
    auto const& retrieved = *maybe_retrieved;
    auto iterator = retrieved.begin();
    contents_arg_t const contents = same_values_triplet.contents_full();
    for (std::size_t index = 0; index != contents.size(); ++index, ++iterator) {
        auto const expected_begin = contents[index].begin();
        value_view_t const retrieved_view = *iterator;
        value_view_t const expected_view(expected_begin, expected_begin + contents[index].size());
        failures += expect_eq(retrieved_view.size(), expected_view.size());
        failures += expect(retrieved_view != expected_view);
    }

    snapshot = *database.snapshot();
    auto reference = snapshot[same_values_triplet.keys];
    failures += round_trip(reference, same_values_triplet);
    return failures;
}

/** Every new snapshot joins the listed ones, and clearing the database drops them all. */
std::size_t database_snapshots_list(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    if (!database.supports_snapshots()) return failures;
    failures += expect(database.clear().succeeded());

    auto first_snapshot = *database.snapshot();
    auto snapshots = first_snapshot.snapshots();
    failures += expect_eq((*snapshots).size(), 1u);

    auto second_snapshot = *database.snapshot();
    snapshots = second_snapshot.snapshots();
    failures += expect_eq((*snapshots).size(), 2u);

    auto third_snapshot = *database.snapshot();
    snapshots = third_snapshot.snapshots();
    failures += expect_eq((*snapshots).size(), 3u);

    first_snapshot = *database.snapshot();
    snapshots = first_snapshot.snapshots();
    failures += expect_eq((*snapshots).size(), 4u);

    failures += expect(database.clear().succeeded());
    snapshots = first_snapshot.snapshots();
    failures += expect_eq((*snapshots).size(), 0u);
    return failures;
}

/** A transaction reads the head, or the state of whichever snapshot it is set to. */
std::size_t database_transaction_with_snapshot(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    if (!database.supports_snapshots() || !database.supports_transactions()) return failures;
    failures += expect(database.clear().succeeded());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    blobs_collection_t collection = database.main();
    auto collection_reference = collection[triplet.keys];

    failures += check_length(collection_reference, ustore_length_missing_k);
    failures += round_trip(collection_reference, triplet);

    auto snapshot = *database.snapshot();
    auto snapshot_reference = snapshot[triplet.keys];
    failures += check_equalities(snapshot_reference, triplet);

    failures += round_trip(collection_reference, same_values_triplet);

    transaction_t transaction = *database.transact();
    auto head_reference = transaction[triplet.keys];
    failures += check_equalities(head_reference, same_values_triplet);

    transaction.set_snapshot(snapshot.snap());
    auto older_reference = transaction[triplet.keys];
    failures += check_equalities(older_reference, triplet);

    snapshot = *database.snapshot();
    transaction.set_snapshot(snapshot.snap());
    auto newer_reference = transaction[triplet.keys];
    failures += check_equalities(newer_reference, same_values_triplet);
    return failures;
}

/** Reads through an unknown snapshot fail, and reads through a valid one succeed again. */
std::size_t database_set_wrong_snapshot(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    if (!database.supports_snapshots()) return failures;
    failures += expect(database.clear().succeeded());

    triplet_t triplet;
    blobs_collection_t collection = database.main();
    auto collection_reference = collection[triplet.keys];

    failures += check_length(collection_reference, ustore_length_missing_k);
    failures += round_trip(collection_reference, triplet);

    auto snapshot = *database.snapshot();
    auto snapshot_reference = snapshot[triplet.keys];
    failures += check_equalities(snapshot_reference, triplet);

    auto snapshots = snapshot.snapshots();
    failures += expect_eq((*snapshots).size(), 1u);

    ustore_snapshot_t const valid_snapshot = snapshot.snap();
    snapshot.set_snapshot(1u);
    auto wrong_reference = snapshot[triplet.keys];
    failures += expect(!wrong_reference.value());

    snapshot.set_snapshot(valid_snapshot);
    auto right_reference = snapshot[triplet.keys];
    failures += expect(right_reference.value());
    return failures;
}

/** Exports snapshots as databases, then loads and modifies them independently of the original. */
std::size_t database_export_snapshot(char const* directory) {
    std::size_t failures = 0;
    test_path_t const original_directory {directory, "original/"};
    test_path_t const first_export_directory {directory, "export1/"};
    test_path_t const second_export_directory {directory, "export2/"};
    failures += expect(recreate_directory(original_directory.c_str()));
    failures += expect(recreate_directory(first_export_directory.c_str()));
    failures += expect(recreate_directory(second_export_directory.c_str()));

    test_config_t const config {original_directory.c_str()};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    if (!database.supports_snapshots()) return failures;
    failures += expect(database.clear().succeeded());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    blobs_collection_t collection = database.main();
    auto collection_reference = collection[triplet.keys];
    failures += round_trip(collection_reference, triplet);

    auto first_snapshot = *database.snapshot();
    failures += expect(first_snapshot.export_to(first_export_directory.c_str()).succeeded());
    failures += check_equalities(collection_reference, triplet);

    test_config_t const first_config {first_export_directory.c_str()};
    database_t first_export;
    failures += expect(first_export.open(first_config.c_str()).succeeded());
    auto first_collection = first_export.main();
    auto first_reference = first_collection[triplet.keys];
    failures += check_equalities(first_reference, triplet);

    failures += round_trip(collection_reference, same_values_triplet);

    auto second_snapshot = *database.snapshot();
    failures += expect(second_snapshot.export_to(second_export_directory.c_str()).succeeded());
    failures += check_equalities(collection_reference, same_values_triplet);

    test_config_t const second_config {second_export_directory.c_str()};
    database_t second_export;
    failures += expect(second_export.open(second_config.c_str()).succeeded());
    auto second_collection = second_export.main();
    auto second_reference = second_collection[same_values_triplet.keys];
    failures += check_equalities(second_reference, same_values_triplet);

    failures += check_equalities(first_reference, triplet);
    failures += check_equalities(second_reference, same_values_triplet);
    failures += round_trip(first_reference, same_values_triplet);
    failures += round_trip(second_reference, triplet);
    failures += check_equalities(first_reference, same_values_triplet);
    failures += check_equalities(second_reference, triplet);
    return failures;
}

/** One thread reads through a snapshot while another resets it to the head. */
std::size_t database_snapshot_with_threads(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    if (!database.supports_snapshots()) return failures;
    failures += expect(database.clear().succeeded());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    blobs_collection_t collection = database.main();
    auto reference = collection[triplet.keys];
    failures += round_trip(reference, triplet);

    auto snapshot = *database.snapshot();
    auto snapshot_reference = snapshot[triplet.keys];

    failures += round_trip(reference, same_values_triplet);

    std::atomic_flag snapshot_lock {};
    bool is_reset = false;
    std::atomic<std::size_t> thread_failures {0};
    auto acquire = [&] { return spin_until([&] { return !snapshot_lock.test_and_set(std::memory_order_acquire); }); };

    auto task = [&](std::size_t thread) noexcept {
        std::size_t local_failures = 0;
        if (thread == 0) {
            while (true) {
                if (!acquire()) {
                    ++local_failures;
                    break;
                }
                if (is_reset) {
                    auto head_reference = snapshot[triplet.keys];
                    local_failures += check_equalities(head_reference, same_values_triplet);
                    snapshot_lock.clear(std::memory_order_release);
                    break;
                }
                local_failures += check_equalities(snapshot_reference, triplet);
                snapshot_lock.clear(std::memory_order_release);
            }
        }
        else if (acquire()) {
            snapshot.set_snapshot(0);
            is_reset = true;
            snapshot_lock.clear(std::memory_order_release);
        }
        else { ++local_failures; }
        thread_failures.fetch_add(local_failures, std::memory_order_relaxed);
    };

    auto maybe_executor = executor_t::make(2);
    failures += expect(maybe_executor);
    if (!maybe_executor) return failures;
    failures += expect(st::succeeded(maybe_executor->for_threads(task)));
    failures += thread_failures.load();
    return failures;
}

/** Concurrent transactions erasing missing keys both commit and leave the database empty. */
std::size_t database_transaction_erase_missing(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    if (!database.supports_transactions()) return failures;
    failures += expect(database.clear().succeeded());
    failures += expect(database.transact());
    transaction_t first_transaction = *database.transact();
    transaction_t second_transaction = *database.transact();

    failures += expect(second_transaction.main().at(-7297309151944849401).erase().succeeded());
    failures += expect(first_transaction.main().at(-8640850744835793378).erase().succeeded());
    failures += expect(first_transaction.commit().succeeded());
    failures += expect(second_transaction.commit().succeeded());

    failures += expect(database.main().at(-8640850744835793378).value() == value_view_t {});
    failures += expect(database.main().at(-7297309151944849401).value() == value_view_t {});
    failures += expect(database.main().keys().size() == 0u);
    return failures;
}

/** Of two transactions writing the same key, the later commit fails. */
std::size_t database_transaction_write_conflicting(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    if (!database.supports_transactions()) return failures;
    failures += expect(database.clear().succeeded());
    failures += expect(database.transact());
    transaction_t first_transaction = *database.transact();
    transaction_t second_transaction = *database.transact();

    failures += expect(second_transaction.main().at(6).assign("a").succeeded());
    failures += expect(first_transaction.main().at(6).assign("b").succeeded());
    failures += expect(first_transaction.commit().succeeded());
    failures += expect(second_transaction.commit().failed());
    return failures;
}

/** A sequenced commit answers a positive sequence number. */
std::size_t database_transaction_sequenced_commit(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    if (!database.supports_transactions()) return failures;
    failures += expect(database.clear().succeeded());

    failures += expect(database.transact());
    transaction_t transaction = *database.transact();

    triplet_t triplet;
    auto transaction_reference = transaction[triplet.keys];

    failures += expect(transaction_reference.assign(triplet.contents_offsets()).succeeded());
    auto maybe_sequence_number = transaction.sequenced_commit();
    failures += expect(maybe_sequence_number);
    failures += expect(*maybe_sequence_number > 0);
    failures += expect(transaction.reset().succeeded());
    return failures;
}

#pragma endregion Binary Modality

#pragma region Documents Modality

/** Flat documents in the compact, key-sorted form the database writes back byte for byte. */
constexpr std::array<char const*, 3> flat_documents_k {
    R"({"age":24,"person":"Alice"})",
    R"({"age":25,"person":"Bob"})",
    R"({"age":26,"person":"Carl"})",
};

/** Nested documents in the compact, key-sorted form the database writes back byte for byte. */
constexpr std::array<std::string_view, 3> nested_documents_k {
    R"({"person":{"age":24,"name":"Alice"}})",
    R"({"person":[{"age":25,"name":"Bob"}]})",
    R"({"age":26,"person":"Carl"})",
};

/** Three JSON texts laid end to end, with the offsets the contents argument reads them through. */
struct documents_tape_t {

    /** The three JSON texts laid end to end, with no separators. */
    char bytes[256] {};

    /** Where each text starts, and where the last one ends. */
    std::array<ustore_length_t, 4> offsets {};

    /** The first byte of @c bytes, which the contents argument reads the offsets against. */
    ustore_bytes_ptr_t begin = reinterpret_cast<ustore_bytes_ptr_t>(bytes);

    documents_tape_t() noexcept = default;
    documents_tape_t(documents_tape_t const&) = delete;
    documents_tape_t& operator=(documents_tape_t const&) = delete;

    /** Lays out @p documents, which together must fit in @c bytes. */
    void assign(std::array<std::string_view, 3> const& documents) noexcept {
        ustore_length_t offset = 0;
        for (std::size_t index = 0; index != documents.size(); ++index) {
            offsets[index] = offset;
            std::memcpy(bytes + offset, documents[index].data(), documents[index].size());
            offset += static_cast<ustore_length_t>(documents[index].size());
        }
        offsets[3] = offset;
    }

    /** A contents argument that follows every later @c assign. */
    contents_arg_t contents() const noexcept {
        contents_arg_t argument {};
        argument.offsets_begin = {offsets.data(), sizeof(ustore_length_t)};
        argument.contents_begin = {&begin, 0};
        argument.count = 3;
        return argument;
    }
};

/** Maps integers to flat JSON documents and reads them whole, by field and as a binary string. */
std::size_t database_docs_flat(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    docs_collection_t collection = database.main<docs_collection_t>();
    failures += expect(collection[1].assign(flat_documents_k[0]).succeeded());
    failures += expect(collection[2].assign(flat_documents_k[1]).succeeded());
    failures += expect(collection[3].assign(flat_documents_k[2]).succeeded());
    failures += expect(json_equal(as_text(*collection[1].value()), flat_documents_k[0]));
    failures += expect(json_equal(as_text(*collection[collection_key_field_t {2, "person"}].value()), R"("Bob")"));
    failures += expect(json_equal(as_text(*collection[collection_key_field_t {3, "age"}].value()), "26"));

    auto maybe_person = collection[collection_key_field_t {1, "person"}].value(ustore_doc_field_str_k);
    failures += expect_eq(std::string_view(maybe_person->c_str(), maybe_person->size()), std::string_view("Alice"));
    return failures;
}

/** Writes nested documents in one batch and reads them back by key and by JSON pointer. */
std::size_t database_docs_nested_batch(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());
    docs_collection_t collection = database.main<docs_collection_t>();

    documents_tape_t tape;
    contents_arg_t const values = tape.contents();
    tape.assign(nested_documents_k);

    std::array<ustore_key_t, 3> keys {1, 2, 3};
    auto reference = collection[keys];
    failures += expect(reference.assign(values).succeeded());

    failures += expect(json_equal(as_text(*collection[1].value()), nested_documents_k[0]));
    failures += expect(json_equal(as_text(*collection[2].value()), nested_documents_k[1]));
    failures += expect(json_equal(as_text(*collection[3].value()), nested_documents_k[2]));

    failures += expect(
        json_equal(as_text(*collection[collection_key_field_t {1, "person"}].value()), R"({"name":"Alice","age":24})"));
    failures += expect(
        json_equal(as_text(*collection[collection_key_field_t {2, "person"}].value()), R"([{"name":"Bob","age":25}])"));
    failures += expect(
        json_equal(as_text(*collection[collection_key_field_t {2, "/person/0/name"}].value()), R"("Bob")"));

    failures += check_equalities(reference, values);

    std::array<ustore_key_t, 3> unsorted_keys {1, 3, 2};
    auto unsorted_reference = collection[unsorted_keys];
    tape.assign({nested_documents_k[0], nested_documents_k[2], nested_documents_k[1]});
    failures += check_equalities(unsorted_reference, values);

    std::array<ustore_key_t, 3> duplicate_keys {1, 2, 1};
    auto duplicate_reference = collection[duplicate_keys];
    tape.assign({nested_documents_k[0], nested_documents_k[1], nested_documents_k[0]});
    failures += check_equalities(duplicate_reference, values);

    std::array<collection_key_field_t, 3> keys_with_fields {
        collection_key_field_t {1, "person"},
        collection_key_field_t {2, "/person/0/name"},
        collection_key_field_t {3, "age"},
    };
    auto fields_reference = collection[keys_with_fields];
    tape.assign({R"({"age":24,"name":"Alice"})", R"("Bob")", "26"});
    failures += check_equalities(fields_reference, values);

    tape.assign({nested_documents_k[0], nested_documents_k[1], R"({"name":"Alice", } "age": 24})"});
    failures += expect(reference.assign(values).failed());
    return failures;
}

/** Updates, inserts and upserts whole documents and fields addressed by JSON pointers. */
std::size_t database_docs_modify(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());
    docs_collection_t collection = database.main<docs_collection_t>();
    failures += expect(collection[1].assign(nested_documents_k[0].data()).succeeded());
    failures += expect(json_equal(as_text(*collection[1].value()), nested_documents_k[0]));

    char const* modifier = R"({"person":{"name":"Charles","age":28}})";
    failures += expect(collection[1].update(modifier).succeeded());
    failures += expect(json_equal(as_text(*collection[1].value()), modifier));

    failures += expect(
        collection[collection_key_field_t {1, "/person"}].update(R"({"name":"Alice","age":24})").succeeded());
    failures += expect(json_equal(as_text(*collection[1].value()), nested_documents_k[0]));

    failures += expect(collection[1].insert(nested_documents_k[1].data()).failed());
    failures += expect(collection[2].insert(nested_documents_k[1].data()).succeeded());
    failures += expect(json_equal(as_text(*collection[2].value()), nested_documents_k[1]));

    failures += expect(collection[collection_key_field_t {2, "/person/0/surname"}].insert(R"("Doe")").succeeded());
    failures += expect(
        json_equal(as_text(*collection[2].value()), R"({"person":[{"name":"Bob","age":25,"surname":"Doe"}]})"));

    failures += expect(collection[1].upsert(nested_documents_k[2].data()).succeeded());
    failures += expect(json_equal(as_text(*collection[1].value()), nested_documents_k[2]));

    failures += expect(collection[collection_key_field_t {1, "/person"}].upsert(R"("Charles")").succeeded());
    failures += expect(json_equal(as_text(*collection[1].value()), R"({"person":"Charles","age":26})"));

    failures += expect(collection[collection_key_field_t {1, "/weight"}].upsert("70").succeeded());
    failures += expect(json_equal(as_text(*collection[1].value()), R"({"person":"Charles","age":26,"weight":70})"));
    return failures;
}

/** Whether a modification case applies a JSON Patch or a JSON Merge Patch. */
enum class document_modification_t : std::uint8_t {
    patch_k,
    merge_k,
};

/** RFC 6902 JSON Patch corner cases, each a document, a patch and the expected result. */
constexpr std::string_view patch_cases_k = R"json([
{"comment":"empty list, empty docs","doc":{},"patch":[],"expected":{}},
{"comment":"empty patch list","doc":{"foo":1},"patch":[],"expected":{"foo":1}},
{"comment":"rearrangements OK?","doc":{"foo":1,"bar":2},"patch":[],"expected":{"bar":2,"foo":1}},
{"comment":"rearrangements OK?  How about one level down ... array","doc":[{"foo":1,"bar":2}],"patch":[],"expected":[{"bar":2,"foo":1}]},
{"comment":"rearrangements OK?  How about one level down...","doc":{"foo":{"foo":1,"bar":2}},"patch":[],"expected":{"foo":{"bar":2,"foo":1}}},
{"comment":"add replaces any existing field","doc":{"foo":null},"patch":[{"op":"add","path":"/foo","value":1}],"expected":{"foo":1}},
{"comment":"toplevel array","doc":[],"patch":[{"op":"add","path":"/0","value":"foo"}],"expected":["foo"]},
{"comment":"toplevel array, no change","doc":["foo"],"patch":[],"expected":["foo"]},
{"comment":"toplevel object, numeric string","doc":{},"patch":[{"op":"add","path":"/foo","value":"1"}],"expected":{"foo":"1"}},
{"comment":"toplevel object, integer","doc":{},"patch":[{"op":"add","path":"/foo","value":1}],"expected":{"foo":1}},
{"comment":"Toplevel scalar values OK?","doc":"foo","patch":[{"op":"replace","path":"","value":"bar"}],"expected":"bar"},
{"comment":"replace object document with array document?","doc":{},"patch":[{"op":"add","path":"","value":[]}],"expected":[]},
{"comment":"replace array document with object document?","doc":[],"patch":[{"op":"add","path":"","value":{}}],"expected":{}},
{"comment":"append to root array document?","doc":[],"patch":[{"op":"add","path":"/-","value":"hello"}],"expected":["hello"]},
{"comment":"Add, / target","doc":{},"patch":[{"op":"add","path":"/","value":1}],"expected":{"":1}},
{"comment":"Add, /foo/ deep target (trailing slash)","doc":{"foo":{}},"patch":[{"op":"add","path":"/foo/","value":1}],"expected":{"foo":{"":1}}},
{"comment":"Add composite value at top level","doc":{"foo":1},"patch":[{"op":"add","path":"/bar","value":[1,2]}],"expected":{"foo":1,"bar":[1,2]}},
{"comment":"Add into composite value","doc":{"foo":1,"baz":[{"qux":"hello"}]},"patch":[{"op":"add","path":"/baz/0/foo","value":"world"}],"expected":{"foo":1,"baz":[{"qux":"hello","foo":"world"}]}},
{"doc":{"foo":1},"patch":[{"op":"add","path":"/bar","value":true}],"expected":{"foo":1,"bar":true}},
{"doc":{"foo":1},"patch":[{"op":"add","path":"/bar","value":false}],"expected":{"foo":1,"bar":false}},
{"doc":{"foo":1},"patch":[{"op":"add","path":"/bar","value":null}],"expected":{"foo":1,"bar":null}},
{"comment":"0 can be an array index or object element name","doc":{"foo":1},"patch":[{"op":"add","path":"/0","value":"bar"}],"expected":{"foo":1,"0":"bar"}},
{"doc":["foo"],"patch":[{"op":"add","path":"/1","value":"bar"}],"expected":["foo","bar"]},
{"doc":["foo","sil"],"patch":[{"op":"add","path":"/1","value":"bar"}],"expected":["foo","bar","sil"]},
{"doc":["foo","sil"],"patch":[{"op":"add","path":"/0","value":"bar"}],"expected":["bar","foo","sil"]},
{"comment":"push item to array via last index + 1","doc":["foo","sil"],"patch":[{"op":"add","path":"/2","value":"bar"}],"expected":["foo","sil","bar"]},
{"comment":"test against implementation-specific numeric parsing","doc":{"1e0":"foo"},"patch":[{"op":"test","path":"/1e0","value":"foo"}],"expected":{"1e0":"foo"}},
{"doc":["foo","sil"],"patch":[{"op":"add","path":"/1","value":["bar","baz"]}],"expected":["foo",["bar","baz"],"sil"],"comment":"value in array add not flattened"},
{"doc":{"foo":1,"bar":[1,2,3,4]},"patch":[{"op":"remove","path":"/bar"}],"expected":{"foo":1}},
{"doc":{"foo":1,"baz":[{"qux":"hello"}]},"patch":[{"op":"remove","path":"/baz/0/qux"}],"expected":{"foo":1,"baz":[{}]}},
{"doc":{"foo":1,"baz":[{"qux":"hello"}]},"patch":[{"op":"replace","path":"/foo","value":[1,2,3,4]}],"expected":{"foo":[1,2,3,4],"baz":[{"qux":"hello"}]}},
{"doc":{"foo":[1,2,3,4],"baz":[{"qux":"hello"}]},"patch":[{"op":"replace","path":"/baz/0/qux","value":"world"}],"expected":{"foo":[1,2,3,4],"baz":[{"qux":"world"}]}},
{"doc":["foo"],"patch":[{"op":"replace","path":"/0","value":"bar"}],"expected":["bar"]},
{"doc":[""],"patch":[{"op":"replace","path":"/0","value":0}],"expected":[0]},
{"doc":[""],"patch":[{"op":"replace","path":"/0","value":true}],"expected":[true]},
{"doc":[""],"patch":[{"op":"replace","path":"/0","value":false}],"expected":[false]},
{"doc":[""],"patch":[{"op":"replace","path":"/0","value":null}],"expected":[null]},
{"doc":["foo","sil"],"patch":[{"op":"replace","path":"/1","value":["bar","baz"]}],"expected":["foo",["bar","baz"]],"comment":"value in array replace not flattened"},
{"comment":"replace whole document","doc":{"foo":"bar"},"patch":[{"op":"replace","path":"","value":{"baz":"qux"}}],"expected":{"baz":"qux"}},
{"comment":"spurious patch properties","doc":{"foo":1},"patch":[{"op":"test","path":"/foo","value":1,"spurious":1}],"expected":{"foo":1}},
{"doc":{"foo":null},"patch":[{"op":"test","path":"/foo","value":null}],"expected":{"foo":null},"comment":"null value should be valid obj property"},
{"doc":{"foo":null},"patch":[{"op":"replace","path":"/foo","value":"truthy"}],"expected":{"foo":"truthy"},"comment":"null value should be valid obj property to be replaced with something truthy"},
{"doc":{"foo":null},"patch":[{"op":"move","from":"/foo","path":"/bar"}],"expected":{"bar":null},"comment":"null value should be valid obj property to be moved"},
{"doc":{"foo":null},"patch":[{"op":"copy","from":"/foo","path":"/bar"}],"expected":{"foo":null,"bar":null},"comment":"null value should be valid obj property to be copied"},
{"doc":{"foo":null},"patch":[{"op":"remove","path":"/foo"}],"expected":{},"comment":"null value should be valid obj property to be removed"},
{"doc":{"foo":"bar"},"patch":[{"op":"replace","path":"/foo","value":null}],"expected":{"foo":null},"comment":"null value should still be valid obj property replace other value"},
{"doc":{"foo":{"foo":1,"bar":2}},"patch":[{"op":"test","path":"/foo","value":{"bar":2,"foo":1}}],"expected":{"foo":{"foo":1,"bar":2}},"comment":"test should pass despite rearrangement"},
{"doc":{"foo":[{"foo":1,"bar":2}]},"patch":[{"op":"test","path":"/foo","value":[{"bar":2,"foo":1}]}],"expected":{"foo":[{"foo":1,"bar":2}]},"comment":"test should pass despite (nested) rearrangement"},
{"doc":{"foo":{"bar":[1,2,5,4]}},"patch":[{"op":"test","path":"/foo","value":{"bar":[1,2,5,4]}}],"expected":{"foo":{"bar":[1,2,5,4]}},"comment":"test should pass - no error"},
{"comment":"Empty-string element","doc":{"":1},"patch":[{"op":"test","path":"/","value":1}],"expected":{"":1}},
{"doc":{"foo":["bar","baz"],"":0,"a/b":1,"c%d":2,"e^f":3,"g|h":4,"i\\j":5,"k\"l":6," ":7,"m~n":8},"patch":[{"op":"test","path":"/foo","value":["bar","baz"]},{"op":"test","path":"/foo/0","value":"bar"},{"op":"test","path":"/","value":0},{"op":"test","path":"/a~1b","value":1},{"op":"test","path":"/c%d","value":2},{"op":"test","path":"/e^f","value":3},{"op":"test","path":"/g|h","value":4},{"op":"test","path":"/i\\j","value":5},{"op":"test","path":"/k\"l","value":6},{"op":"test","path":"/ ","value":7},{"op":"test","path":"/m~0n","value":8}],"expected":{"":0," ":7,"a/b":1,"c%d":2,"e^f":3,"foo":["bar","baz"],"g|h":4,"i\\j":5,"k\"l":6,"m~n":8}},
{"comment":"Move to same location has no effect","doc":{"foo":1},"patch":[{"op":"move","from":"/foo","path":"/foo"}],"expected":{"foo":1}},
{"doc":{"foo":1,"baz":[{"qux":"hello"}]},"patch":[{"op":"move","from":"/foo","path":"/bar"}],"expected":{"baz":[{"qux":"hello"}],"bar":1}},
{"doc":{"baz":[{"qux":"hello"}],"bar":1},"patch":[{"op":"move","from":"/baz/0/qux","path":"/baz/1"}],"expected":{"baz":[{},"hello"],"bar":1}},
{"doc":{"baz":[{"qux":"hello"}],"bar":1},"patch":[{"op":"copy","from":"/baz/0","path":"/boo"}],"expected":{"baz":[{"qux":"hello"}],"bar":1,"boo":{"qux":"hello"}}},
{"comment":"replacing the root of the document is possible with add","doc":{"foo":"bar"},"patch":[{"op":"add","path":"","value":{"baz":"qux"}}],"expected":{"baz":"qux"}},
{"comment":"Adding to \"/-\" adds to the end of the array","doc":[1,2],"patch":[{"op":"add","path":"/-","value":{"foo":["bar","baz"]}}],"expected":[1,2,{"foo":["bar","baz"]}]},
{"comment":"Adding to \"/-\" adds to the end of the array, even n levels down","doc":[1,2,[3,[4,5]]],"patch":[{"op":"add","path":"/2/1/-","value":{"foo":["bar","baz"]}}],"expected":[1,2,[3,[4,5,{"foo":["bar","baz"]}]]]},
{"comment":"test remove on array","doc":[1,2,3,4],"patch":[{"op":"remove","path":"/0"}],"expected":[2,3,4]},
{"comment":"test repeated removes","doc":[1,2,3,4],"patch":[{"op":"remove","path":"/1"},{"op":"remove","path":"/2"}],"expected":[1,3]},
{"comment":"Patch with different capitalisation than doc","doc":{"foo":"bar"},"patch":[{"op":"add","path":"/FOO","value":"BAR"}],"expected":{"foo":"bar","FOO":"BAR"}}
])json";

/** RFC 7386 JSON Merge Patch corner cases, each a document, a merge patch and the expected result. */
constexpr std::string_view merge_cases_k = R"json([
{"doc":{"a":"b"},"merge":{"a":"c"},"expected":{"a":"c"}},
{"doc":{"a":"b"},"merge":{"b":"c"},"expected":{"a":"b","b":"c"}},
{"doc":{"a":"b"},"merge":{"a":null},"expected":{}},
{"doc":{"a":"b","b":"c"},"merge":{"a":null},"expected":{"b":"c"}},
{"doc":{"a":["b"]},"merge":{"a":"c"},"expected":{"a":"c"}},
{"doc":{"a":"c"},"merge":{"a":["b"]},"expected":{"a":["b"]}},
{"doc":{"a":[{"b":"c"}]},"merge":{"a":[1]},"expected":{"a":[1]}},
{"doc":{"a":[{"b":"c"}]},"merge":{"a":{"b":"d","c":null}},"expected":{"a":{"b":"d"}}},
{"doc":["a","b"],"merge":["c","d"],"expected":["c","d"]},
{"doc":{"a":"b"},"merge":["c"],"expected":["c"]},
{"doc":{"a":"foo"},"merge":null,"expected":null},
{"doc":{"a":"foo"},"merge":"bar","expected":"bar"},
{"doc":{"e":null},"merge":{"a":1},"expected":{"e":null,"a":1}},
{"doc":[1,2],"merge":{"a":"b","c":null},"expected":{"a":"b"}},
{"doc":{},"merge":{"a":{"bb":{"ccc":null}}},"expected":{"a":{"bb":{}}}}
])json";

/** Applies every case @p modification names and compares each result with its expected document. */
std::size_t check_modification_cases(docs_collection_t& collection, document_modification_t modification) {
    std::size_t failures = 0;
    bool const patching = modification == document_modification_t::patch_k;
    std::string_view const cases_text = patching ? patch_cases_k : merge_cases_k;
    yyjson_doc* const cases = yyjson_read(cases_text.data(), cases_text.size(), 0);
    failures += expect(cases);
    if (!cases) return failures;

    char const* const modifier_field = patching ? "patch" : "merge";
    yyjson_arr_iter iterator;
    yyjson_arr_iter_init(yyjson_doc_get_root(cases), &iterator);
    while (yyjson_val* const entry = yyjson_arr_iter_next(&iterator)) {
        written_json_t const document {yyjson_obj_get(entry, "doc")};
        written_json_t const modifier {yyjson_obj_get(entry, modifier_field)};
        written_json_t const expected {yyjson_obj_get(entry, "expected")};
        if (!document.text || !modifier.text || !expected.text) continue;

        failures += expect(collection[1].assign(static_cast<char const*>(document.text)).succeeded());
        failures += patching ? expect(collection[1].patch(static_cast<char const*>(modifier.text)).succeeded())
                             : expect(collection[1].merge(static_cast<char const*>(modifier.text)).succeeded());
        auto maybe_value = collection[1].value();
        failures += expect(maybe_value);
        std::size_t const mismatches = expect(json_equal(as_text(*maybe_value), expected.text));
        if (mismatches)
            fmt::println(stderr, "    {} with {} gave {}", static_cast<char const*>(document.text),
                         static_cast<char const*>(modifier.text), as_text(*maybe_value));
        failures += mismatches;
    }
    yyjson_doc_free(cases);
    return failures;
}

/** Every RFC 6902 JSON Patch and RFC 7386 JSON Merge Patch corner case yields its expected document. */
std::size_t database_docs_merge_and_patch(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());
    docs_collection_t collection = database.main<docs_collection_t>();

    failures += check_modification_cases(collection, document_modification_t::patch_k);
    failures += check_modification_cases(collection, document_modification_t::merge_k);
    return failures;
}

/** Gathers fields of several documents into tables, through typed headers and type-punned exports. */
std::size_t database_docs_table(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    docs_collection_t collection = database.main<docs_collection_t>();
    char const* const alice = R"({"person":"Alice","age":27,"height":1})";
    char const* const bob = R"({"person":"Bob","age":"27","weight":2})";
    char const* const carl = R"({"person":"Carl","age":24})";
    failures += expect(collection[1].assign(alice).succeeded());
    failures += expect(collection[2].assign(bob).succeeded());
    failures += expect(collection[3].assign(carl).succeeded());
    failures += expect(json_equal(as_text(*collection[1].value()), alice));
    failures += expect(json_equal(as_text(*collection[2].value()), bob));

    // Column names
    {
        auto maybe_fields = collection[1].gist();
        auto fields = *maybe_fields;
        bool has_person = false;
        bool has_height = false;
        bool has_age = false;
        bool has_weight = false;
        for (auto field : fields) {
            std::string_view const name(field.data());
            has_person = has_person || name == "/person";
            has_height = has_height || name == "/height";
            has_age = has_age || name == "/age";
            has_weight = has_weight || name == "/weight";
        }
        failures += expect(has_person);
        failures += expect(has_height);
        failures += expect(has_age);
        failures += expect(!has_weight);
    }

    // Single cell
    {
        auto header = make_table_header().with<std::uint32_t>("age");
        auto maybe_table = collection[1].gather(header);
        auto table = *maybe_table;
        auto ages = table.column<0>();

        failures += expect_eq(ages[0].value, 27u);
        failures += expect(!ages[0].converted);
    }

    // Single row
    {
        auto header = make_table_header() //
                          .with<std::uint32_t>("age")
                          .with<std::int32_t>("age")
                          .with<std::string_view>("age");

        auto maybe_table = collection[1].gather(header);
        auto table = *maybe_table;
        auto unsigned_ages = table.column<0>();
        auto signed_ages = table.column<1>();
        auto age_texts = table.column<2>();

        failures += expect_eq(unsigned_ages[0].value, 27u);
        failures += expect(!unsigned_ages[0].converted);
        failures += expect_eq(signed_ages[0].value, 27);
        failures += expect(signed_ages[0].converted);
        failures += expect_eq(std::string_view(age_texts[0].value.data()), std::string_view("27"));
        failures += expect(age_texts[0].converted);
    }

    // Single column
    {
        auto header = make_table_header().with<std::int32_t>("age");
        auto maybe_table = collection[{1, 2, 3, 123456}].gather(header);
        auto table = *maybe_table;
        auto ages = table.column<0>();

        failures += expect_eq(ages[0].value, 27);
        failures += expect_eq(ages[1].value, 27);
        failures += expect(ages[1].converted);
        failures += expect_eq(ages[2].value, 24);
    }

    // Single strings column
    {
        auto header = make_table_header().with<std::string_view>("age");
        auto maybe_table = collection[{1, 2, 3, 123456}].gather(header);
        auto table = *maybe_table;
        auto age_texts = table.column<0>();

        failures += expect_eq(std::string_view(age_texts[0].value.data()), std::string_view("27"));
        failures += expect(age_texts[0].converted);
        failures += expect_eq(std::string_view(age_texts[1].value.data()), std::string_view("27"));
        failures += expect_eq(std::string_view(age_texts[2].value.data()), std::string_view("24"));
    }

    // Multiple columns
    {
        auto header = make_table_header() //
                          .with<std::int32_t>("age")
                          .with<std::string_view>("age")
                          .with<std::string_view>("person")
                          .with<float>("person")
                          .with<std::int32_t>("height")
                          .with<std::uint64_t>("weight");

        auto maybe_table = collection[{1, 2, 3, 123456, 654321}].gather(header);
        auto table = *maybe_table;
        auto ages = table.column<0>();
        auto age_texts = table.column<1>();
        [[maybe_unused]] auto persons = table.column<2>();
        [[maybe_unused]] auto person_numbers = table.column<3>();
        [[maybe_unused]] auto heights = table.column<4>();
        [[maybe_unused]] auto weights = table.column<5>();

        failures += expect_eq(ages[0].value, 27);
        failures += expect_eq(ages[1].value, 27);
        failures += expect(ages[1].converted);
        failures += expect_eq(ages[2].value, 24);

        failures += expect_eq(std::string_view(age_texts[0].value.data()), std::string_view("27"));
        failures += expect(age_texts[0].converted);
        failures += expect_eq(std::string_view(age_texts[1].value.data()), std::string_view("27"));
        failures += expect_eq(std::string_view(age_texts[2].value.data()), std::string_view("24"));
    }

    // Multiple type-punned columns
    {
        std::array<field_type_t, 6> const columns {{
            field_type_t {"age", ustore_doc_field_i32_k},
            field_type_t {"age", ustore_doc_field_str_k},
            field_type_t {"person", ustore_doc_field_str_k},
            field_type_t {"person", ustore_doc_field_f32_k},
            field_type_t {"height", ustore_doc_field_i32_k},
            field_type_t {"weight", ustore_doc_field_u64_k},
        }};
        table_header_view_t const header = make_table_header(columns);

        auto maybe_table = collection[{1, 2, 3, 123456, 654321}].gather(header);
        auto table = *maybe_table;
        auto ages = table.column(0).as<std::int32_t>();
        auto age_texts = table.column(1).as<value_view_t>();
        [[maybe_unused]] auto persons = table.column(2).as<value_view_t>();
        [[maybe_unused]] auto person_numbers = table.column(3).as<float>();
        [[maybe_unused]] auto heights = table.column(4).as<std::int32_t>();
        [[maybe_unused]] auto weights = table.column(5).as<std::uint64_t>();

        failures += expect_eq(ages[0].value, 27);
        failures += expect_eq(ages[1].value, 27);
        failures += expect(ages[1].converted);
        failures += expect_eq(ages[2].value, 24);

        failures += expect_eq(std::string_view(age_texts[0].value.c_str()), std::string_view("27"));
        failures += expect(age_texts[0].converted);
        failures += expect_eq(std::string_view(age_texts[1].value.c_str()), std::string_view("27"));
        failures += expect_eq(std::string_view(age_texts[2].value.c_str()), std::string_view("24"));
    }
    return failures;
}

#pragma endregion Documents Modality

#pragma region Graph Modality

/** Vertices every graph case builds. */
constexpr std::size_t graph_vertices_count_k = 1000;

/** Stride between the vertices each vertex connects to, which leaves every degree at nine. */
constexpr std::size_t graph_connect_stride_k = 100;

/** Connects every vertex to each @c graph_connect_stride_k-th vertex after it, and answers whether every edge fit. */
[[nodiscard]] bool fill_edges(st::basic_vector<edge_t>& edges) noexcept {
    ustore_key_t edge_id = 0;
    for (std::size_t vertex_id = 0; vertex_id != graph_vertices_count_k; ++vertex_id)
        for (std::size_t connect_with = vertex_id + graph_connect_stride_k; connect_with < graph_vertices_count_k;
             connect_with += graph_connect_stride_k) {
            ++edge_id;
            edge_t edge {static_cast<ustore_key_t>(vertex_id), static_cast<ustore_key_t>(connect_with), edge_id};
            if (st::failed(edges.push_back(std::move(edge)))) return false;
        }
    return true;
}

/** Views @p edges as the columns the graph calls take. */
edges_view_t edges_of(st::basic_vector<edge_t> const& edges) noexcept {
    return edges_view_t(edges.begin(), edges.end());
}

/** Upserts disconnected vertices. */
std::size_t database_graph_upsert_vertices(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    graph_collection_t graph = database.main<graph_collection_t>();
    std::array<ustore_key_t, 4> const vertices {1, 4, 5, 2};
    failures += expect(graph.upsert_vertices(strided(vertices)).succeeded());

    failures += expect(*graph.contains(1));
    failures += expect(*graph.contains(2));
    failures += expect(*graph.contains(4));
    failures += expect(*graph.contains(5));
    return failures;
}

/** Upserts an edge together with its vertices. */
std::size_t database_graph_upsert_edge(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    graph_collection_t graph = database.main<graph_collection_t>();
    edge_t const edge {1, 2, 9};
    failures += expect(graph.upsert_edge(edge).succeeded());
    failures += expect(*graph.contains(1));
    failures += expect(*graph.contains(2));
    failures += expect(!*graph.contains(3));

    auto maybe_neighbors = graph.neighbors(1);
    failures += expect(maybe_neighbors);
    auto const& neighbors = *maybe_neighbors;
    failures += expect_eq(neighbors.size(), 1u);
    failures += expect_eq(neighbors[0], 2);
    return failures;
}

/** Checks a triangle of three vertices and three directed edges, before and after removing its parts. */
std::size_t check_triangle(graph_collection_t& graph, std::array<edge_t, 3>& triangle) {
    std::size_t failures = 0;
    failures += expect(*graph.contains(1));
    failures += expect(*graph.contains(2));
    failures += expect(!*graph.contains(9));
    failures += expect(!*graph.contains(10));
    failures += expect(!*graph.contains(1000));

    failures += expect_eq(*graph.degree(1), 2u);
    failures += expect_eq(*graph.degree(2), 2u);
    failures += expect_eq(*graph.degree(3), 2u);
    failures += expect_eq(*graph.degree(1, ustore_vertex_source_k), 1u);
    failures += expect_eq(*graph.degree(2, ustore_vertex_source_k), 1u);
    failures += expect_eq(*graph.degree(3, ustore_vertex_source_k), 1u);

    failures += expect(graph.edges_containing(1));
    failures += expect_eq(graph.edges_containing(1)->size(), 2u);
    failures += expect_eq(graph.edges_containing(1, ustore_vertex_source_k)->size(), 1u);
    failures += expect_eq(graph.edges_containing(1, ustore_vertex_target_k)->size(), 1u);

    failures += expect_eq(graph.edges_containing(3, ustore_vertex_target_k)->size(), 1u);
    failures += expect_eq(graph.edges_containing(2, ustore_vertex_source_k)->size(), 1u);
    failures += expect_eq((*graph.edges_containing(3, ustore_vertex_target_k))[0].source_id, 2);
    failures += expect_eq((*graph.edges_containing(3, ustore_vertex_target_k))[0].target_id, 3);
    failures += expect_eq((*graph.edges_containing(3, ustore_vertex_target_k))[0].id, 10);
    failures += expect_eq(graph.edges_between(3, 1)->size(), 1u);
    failures += expect_eq(graph.edges_between(1, 3)->size(), 0u);

    failures += expect(graph.edges());
    {
        auto present_edges = *graph.edges(ustore_vertex_source_k);
        auto present_iterator = std::move(present_edges).begin();
        std::size_t count = 0;
        while (!present_iterator.is_end()) {
            if (count < triangle.size()) failures += expect(*present_iterator == triangle[count]);
            ++present_iterator;
            ++count;
        }
        failures += expect_eq(count, triangle.size());
    }

    // Removing an edge keeps its vertices
    failures += expect(graph
                           .remove_edges(edges_view_t {
                               {{&triangle[0].source_id}, 1},
                               {{&triangle[0].target_id}, 1},
                               {{&triangle[0].id}, 1},
                           })
                           .succeeded());
    failures += expect(*graph.contains(1));
    failures += expect(*graph.contains(2));
    failures += expect_eq(graph.edges_between(1, 2)->size(), 0u);

    failures += expect(graph
                           .upsert_edges(edges_view_t {
                               {{&triangle[0].source_id}, 1},
                               {{&triangle[0].target_id}, 1},
                               {{&triangle[0].id}, 1},
                           })
                           .succeeded());
    failures += expect_eq(graph.edges_between(1, 2)->size(), 1u);

    ustore_key_t const vertex_to_remove = 2;
    failures += expect(graph.remove_vertex(vertex_to_remove).succeeded());
    failures += expect(!*graph.contains(vertex_to_remove));
    failures += expect_eq(graph.edges_containing(vertex_to_remove)->size(), 0u);
    failures += expect_eq(graph.edges_between(1, vertex_to_remove)->size(), 0u);
    failures += expect_eq(graph.edges_between(vertex_to_remove, 1)->size(), 0u);

    failures += expect(graph.upsert_edges(edges(triangle)).succeeded());
    failures += expect(*graph.contains(vertex_to_remove));
    failures += expect_eq(graph.edges_containing(vertex_to_remove)->size(), 2u);
    failures += expect_eq(graph.edges_between(1, vertex_to_remove)->size(), 1u);
    failures += expect_eq(graph.edges_between(vertex_to_remove, 1)->size(), 0u);
    return failures;
}

/** Builds a triangle one edge at a time. */
std::size_t database_graph_triangle(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    graph_collection_t graph = database.main<graph_collection_t>();
    std::array<edge_t, 3> triangle {{{1, 2, 9}, {2, 3, 10}, {3, 1, 11}}};
    failures += expect(graph.upsert_edge(triangle[0]).succeeded());
    failures += expect(graph.upsert_edge(triangle[1]).succeeded());
    failures += expect(graph.upsert_edge(triangle[2]).succeeded());

    auto maybe_neighbors = graph.neighbors(1);
    failures += expect(maybe_neighbors);
    auto const& neighbors = *maybe_neighbors;
    failures += expect_eq(neighbors.size(), 2u);
    failures += expect_eq(neighbors[0], 2);
    failures += expect_eq(neighbors[1], 3);

    failures += check_triangle(graph, triangle);
    return failures;
}

/** Builds a triangle in one batch, catching inconsistent concurrent updates behind one graph call. */
std::size_t database_graph_triangle_batch(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    graph_collection_t graph = database.main<graph_collection_t>();
    std::array<edge_t, 3> triangle {{{1, 2, 9}, {2, 3, 10}, {3, 1, 11}}};
    failures += expect(graph.upsert_edges(edges(triangle)).succeeded());

    failures += check_triangle(graph, triangle);
    return failures;
}

/** A transaction reading the middle of a wedge A-B-C must fail to commit once A-B changes outside it. */
std::size_t database_graph_transaction_watch(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    if (!database.supports_transactions()) return failures;
    failures += expect(database.clear().succeeded());

    graph_collection_t graph = database.main<graph_collection_t>();
    edge_t const edge_ab {'A', 'B', 19};
    edge_t const edge_bc {'B', 'C', 31};
    failures += expect(graph.upsert_edge(edge_ab).succeeded());
    failures += expect(graph.upsert_edge(edge_bc).succeeded());

    transaction_t transaction = *database.transact();
    graph_collection_t transaction_graph = transaction.main<graph_collection_t>();
    failures += expect(transaction_graph.degree('B') == 2);
    failures += expect(transaction_graph.remove_edge(edge_bc).succeeded());
    failures += expect(graph.remove_edge(edge_ab).succeeded());

    failures += expect(transaction.commit().failed());
    return failures;
}

/** Builds a larger graph and checks every vertex degree. */
std::size_t database_graph_random_fill(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    graph_collection_t graph = database.main<graph_collection_t>();
    st::basic_vector<edge_t> generated;
    failures += expect(fill_edges(generated));
    failures += expect(graph.upsert_edges(edges_of(generated)).succeeded());

    for (ustore_key_t vertex_id = 0; vertex_id != graph_vertices_count_k; ++vertex_id) {
        failures += expect(graph.contains(vertex_id));
        failures += expect_eq(*graph.degree(vertex_id), 9u);
    }
    return failures;
}

/** Two transactions adding edges around a shared vertex: the later commit depends on stale state and fails. */
std::size_t database_graph_conflicting_transactions(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    if (!database.supports_transactions()) return failures;
    failures += expect(database.clear().succeeded());

    transaction_t first_transaction = *database.transact();
    graph_collection_t first_graph = first_transaction.main<graph_collection_t>();
    transaction_t second_transaction = *database.transact();
    graph_collection_t second_graph = second_transaction.main<graph_collection_t>();

    edge_t const first_edge {4, 5, 15};
    edge_t const second_edge {5, 6, 16};

    failures += expect(first_graph.upsert_edge(first_edge).succeeded());
    failures += expect(second_graph.upsert_edge(second_edge).succeeded());
    failures += expect(first_transaction.commit().succeeded());
    failures += expect(second_transaction.commit().failed());
    return failures;
}

/** Layers a star, a pentagon and five self-loops over the same five vertices. */
std::size_t database_graph_layering_shapes(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    graph_collection_t graph = database.main<graph_collection_t>();

    std::array<ustore_key_t, 5> const vertices {1, 2, 3, 4, 5};
    auto check_vertices = [&](ustore_vertex_degree_t degree) {
        std::size_t vertex_failures = 0;
        for (ustore_key_t const vertex_id : vertices) {
            vertex_failures += expect(*graph.contains(vertex_id) == (degree != ustore_vertex_degree_missing_k));
            vertex_failures += expect_eq(*graph.degree(vertex_id), degree);
        }
        return vertex_failures;
    };

    failures += check_vertices(ustore_vertex_degree_missing_k);

    std::array<edge_t, 5> star {{{1, 3, 1}, {1, 4, 2}, {2, 4, 3}, {2, 5, 4}, {3, 5, 5}}};
    std::array<edge_t, 5> pentagon {{{1, 2, 6}, {2, 3, 7}, {3, 4, 8}, {4, 5, 9}, {5, 1, 10}}};
    std::array<edge_t, 5> self_loops {{{1, 1, 11}, {2, 2, 12}, {3, 3, 13}, {4, 4, 14}, {5, 5, 15}}};

    failures += expect(graph.upsert_edges(edges(star)).succeeded());
    failures += check_vertices(2);
    failures += expect(graph.upsert_edges(edges(pentagon)).succeeded());
    failures += check_vertices(4);
    failures += expect(graph.remove_edges(edges(star)).succeeded());
    failures += check_vertices(2);
    failures += expect(graph.upsert_edges(edges(star)).succeeded());
    failures += check_vertices(4);
    failures += expect(graph.remove_edges(edges(pentagon)).succeeded());
    failures += check_vertices(2);
    failures += expect(graph.upsert_edges(edges(pentagon)).succeeded());
    failures += check_vertices(4);
    failures += expect(graph.upsert_edges(edges(self_loops)).succeeded());
    failures += check_vertices(6);
    failures += expect(graph.remove_edges(edges(star)).succeeded());
    failures += expect(graph.remove_edges(edges(pentagon)).succeeded());
    failures += check_vertices(2);
    failures += expect(graph.remove_edges(edges(self_loops)).succeeded());
    failures += check_vertices(0);
    failures += expect(database.clear().succeeded());
    failures += check_vertices(ustore_vertex_degree_missing_k);
    return failures;
}

/** Removes vertices one by one and in a batch, each removal updating every neighbor. */
std::size_t database_graph_remove_vertices(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    graph_collection_t graph = database.main<graph_collection_t>();
    st::basic_vector<edge_t> generated;
    failures += expect(fill_edges(generated));
    failures += expect(graph.upsert_edges(edges_of(generated)).succeeded());

    for (ustore_key_t vertex_id = 0; vertex_id != graph_vertices_count_k; ++vertex_id) {
        failures += expect(graph.contains(vertex_id));
        failures += expect(*graph.contains(vertex_id));
        failures += expect(graph.remove_vertex(vertex_id).succeeded());
        failures += expect(graph.contains(vertex_id));
        failures += expect(!*graph.contains(vertex_id));
    }

    failures += expect(graph.upsert_edges(edges_of(generated)).succeeded());
    std::array<ustore_key_t, graph_vertices_count_k> vertices;
    std::iota(vertices.begin(), vertices.end(), 0);
    failures += expect(graph.remove_vertices(strided(vertices).immutable()).succeeded());
    for (ustore_key_t vertex_id = 0; vertex_id != graph_vertices_count_k; ++vertex_id) {
        failures += expect(graph.contains(vertex_id));
        failures += expect(!*graph.contains(vertex_id));
    }
    return failures;
}

/** Removing a known list of edges leaves the vertices in the graph, disconnected. */
std::size_t database_graph_remove_edges_keep_vertices(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    graph_collection_t graph = database.main<graph_collection_t>();
    st::basic_vector<edge_t> generated;
    failures += expect(fill_edges(generated));
    failures += expect(graph.upsert_edges(edges_of(generated)).succeeded());
    failures += expect(graph.remove_edges(edges_of(generated)).succeeded());

    for (ustore_key_t vertex_id = 0; vertex_id != graph_vertices_count_k; ++vertex_id) {
        failures += expect(graph.contains(vertex_id));
        failures += expect(*graph.contains(vertex_id));
    }
    return failures;
}

/** Counts the edges of each vertex, then removes exactly those edges. */
std::size_t database_graph_get_vertex_edges(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    graph_collection_t graph = database.main<graph_collection_t>();
    st::basic_vector<edge_t> generated;
    failures += expect(fill_edges(generated));
    failures += expect(graph.upsert_edges(edges_of(generated)).succeeded());

    st::basic_vector<edge_t> received;
    for (ustore_key_t vertex_id = 0; vertex_id != graph_vertices_count_k; ++vertex_id) {
        auto vertex_edges = *graph.edges_containing(vertex_id);
        failures += expect_eq(vertex_edges.size(), 9u);
        for (std::size_t index = 0; index != vertex_edges.size(); ++index) {
            edge_t edge = vertex_edges[index];
            failures += expect(st::succeeded(received.push_back(std::move(edge))));
        }
    }
    failures += expect(graph.remove_edges(edges_of(received)).succeeded());

    for (ustore_key_t vertex_id = 0; vertex_id != graph_vertices_count_k; ++vertex_id) {
        failures += expect(graph.contains(vertex_id));
        failures += expect(*graph.contains(vertex_id));
        failures += expect_eq(graph.edges_containing(vertex_id)->size(), 0u);
    }
    return failures;
}

/** Reads the degrees of many vertices in one call. */
std::size_t database_graph_degrees(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    graph_collection_t graph = database.main<graph_collection_t>();
    std::array<ustore_key_t, graph_vertices_count_k> vertices;
    std::iota(vertices.begin(), vertices.end(), 0);

    st::basic_vector<edge_t> generated;
    failures += expect(fill_edges(generated));
    failures += expect(graph.upsert_edges(edges_of(generated)).succeeded());

    auto degrees = *graph.degrees(strided(vertices).immutable());
    failures += expect_eq(degrees.size(), graph_vertices_count_k);
    return failures;
}

/** Neighbors of a vertex include itself through a self-loop and both directions of its edges. */
std::size_t database_graph_neighbors(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());
    failures += expect(database.clear().succeeded());

    graph_collection_t graph = database.main<graph_collection_t>();
    failures += expect(graph.upsert_edge(edge_t {1, 1, 17}).succeeded());
    failures += expect(graph.upsert_edge(edge_t {1, 2, 15}).succeeded());
    failures += expect(graph.upsert_edge(edge_t {2, 3, 16}).succeeded());

    auto maybe_first_neighbors = graph.neighbors(1);
    failures += expect(maybe_first_neighbors);
    auto const& first_neighbors = *maybe_first_neighbors;
    failures += expect_eq(first_neighbors.size(), 2u);
    failures += expect_eq(first_neighbors[0], 1);
    failures += expect_eq(first_neighbors[1], 2);

    auto maybe_second_neighbors = graph.neighbors(2);
    failures += expect(maybe_second_neighbors);
    auto const& second_neighbors = *maybe_second_neighbors;
    failures += expect_eq(second_neighbors.size(), 2u);
    failures += expect_eq(second_neighbors[0], 1);
    failures += expect_eq(second_neighbors[1], 3);
    return failures;
}

#pragma endregion Graph Modality

int main(int argc, char** argv) {
    std::string_view parent;
    std::array<flag_t, 1> const flags {{directory_flag(parent)}};
    if (argument_status_t const status = parse_test_arguments(argc, argv, flags); status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;

    auto const maybe_test_directory = test_directory_t::make(parent);
    if (!maybe_test_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", parent);
        return 1;
    }
    char const* const directory = maybe_test_directory->path();
    if (directory) fmt::println("Will work in directory: {}", directory);
    else fmt::println("Will work with default configuration");

    std::size_t total = 0;
    total += report("database.open_clear_close", database_open_clear_close(directory));
    total += report("database.clear_by_clearing_db", database_clear_collection_by_clearing_database(directory));
    total += report("database.overwrite_with_step", database_overwrite_with_step(directory));
    total += directory ? report("database.persistency", database_persistency(directory))
                       : report("database.persistency", needs_directory_k);
    total += report("database.named_collections", database_named_collections(directory));
    total += report("database.named_collections_list", database_named_collections_list(directory));
    total += report("database.clear_values", database_clear_values(directory));
    total += report("database.presences", database_presences(directory));
    total += report("database.scan", database_scan(directory));
    total += report("database.batch_scan", database_batch_scan(directory));
    total += report("database.transaction_read_committed", database_transaction_read_committed(directory));
    total += report("database.snapshot_isolation", database_transaction_snapshot_isolation(directory));
    total += report("database.snapshots_list", database_snapshots_list(directory));
    total += report("database.transaction_with_snapshot", database_transaction_with_snapshot(directory));
    total += report("database.set_wrong_snapshot", database_set_wrong_snapshot(directory));
    total += directory ? report("database.export_snapshot", database_export_snapshot(directory))
                       : report("database.export_snapshot", needs_directory_k);
    total += report("database.snapshot_with_threads", database_snapshot_with_threads(directory));
    total += report("database.erase_missing", database_transaction_erase_missing(directory));
    total += report("database.write_conflicting", database_transaction_write_conflicting(directory));
    total += report("database.sequenced_commit", database_transaction_sequenced_commit(directory));

    total += report("database.docs_flat", database_docs_flat(directory));
    total += report("database.docs_nested_batch", database_docs_nested_batch(directory));
    total += report("database.docs_modify", database_docs_modify(directory));
    total += report("database.docs_merge_and_patch", database_docs_merge_and_patch(directory));
    total += report("database.docs_table", database_docs_table(directory));

    total += report("database.graph_upsert_vertices", database_graph_upsert_vertices(directory));
    total += report("database.graph_upsert_edge", database_graph_upsert_edge(directory));
    total += report("database.graph_triangle", database_graph_triangle(directory));
    total += report("database.graph_triangle_batch", database_graph_triangle_batch(directory));
    total += report("database.graph_transaction_watch", database_graph_transaction_watch(directory));
    total += report("database.graph_random_fill", database_graph_random_fill(directory));
    total += report("database.graph_conflicts", database_graph_conflicting_transactions(directory));
    total += report("database.graph_layering_shapes", database_graph_layering_shapes(directory));
    total += report("database.graph_remove_vertices", database_graph_remove_vertices(directory));
    total += report("database.graph_remove_edges", database_graph_remove_edges_keep_vertices(directory));
    total += report("database.graph_get_vertex_edges", database_graph_get_vertex_edges(directory));
    total += report("database.graph_degrees", database_graph_degrees(directory));
    total += report("database.graph_neighbors", database_graph_neighbors(directory));

    return report("total", total) ? 1 : 0;
}
